// Package uds is the server side of the Parlay UDS protocol
// (contracts/uds-protocol.md): one persistent duplex Unix Domain
// Socket between the C++ core and this backend. It replaces the old
// HTTP transport — no per-call subprocess, no X-Parlay-Token header
// (filesystem permissions on the socket are the access control),
// no /healthz polling.
//
// Wire format (§2): 4-byte little-endian uint32 length prefix +
// UTF-8 JSON payload, 16 MiB max frame. Envelopes (§3):
//   - request  (core -> backend): {type, request_id, action,
//     chat_id?, user_id?, deadline_ms, payload}
//   - response (backend -> core): {type, request_id, ok, data}
//     or {type, request_id, ok:false, error:{code, message}}
//   - event    (backend -> core): {type, event, chat_id, data}
//   - join_request (backend -> core): {type, request_id, chat_id,
//     local_sdp}; the core answers with a response frame carrying
//     {remote_sdp, group_call_id}.
//
// Request/response multiplexing is by request_id; the backend's own
// request ids (join requests) live in a disjoint range starting at
// 1,000,000 so they can never collide with the core's.
package uds

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

// MaxFrameBytes is the largest payload in one frame (16 MiB, §2).
// A length prefix of 0 or above this is a protocol error: the frame
// is dropped and the connection is closed.
const MaxFrameBytes = 16 * 1024 * 1024

// JoinRequestIDBase is the first backend-initiated request id.
// The core's ids start at 1, so the ranges never collide (§10).
const JoinRequestIDBase = int64(1000000)

// Version is the backend version reported by the ping action.
const Version = "0.1.0"

// ErrorCode is one of the contract error codes (uds-protocol.md
// §8, contracts/ipc.json).
type ErrorCode string

const (
	ErrBadRequest        ErrorCode = "bad_request"
	ErrUnauthorized      ErrorCode = "unauthorized"
	ErrNotFound          ErrorCode = "not_found"
	ErrEngineUnavailable ErrorCode = "engine_unavailable"
	ErrDownloadFailed    ErrorCode = "download_failed"
	ErrRateLimited       ErrorCode = "rate_limited"
	ErrInternal          ErrorCode = "internal"
)

// HandlerError is a handler's honest failure: the error code the
// core maps to an HTTP status, plus the message the core surfaces.
// A plain error from a handler is reported as internal.
type HandlerError struct {
	Code    ErrorCode
	Message string
}

func (e *HandlerError) Error() string { return string(e.Code) + ": " + e.Message }

// NewHandlerError builds a HandlerError.
func NewHandlerError(code ErrorCode, message string) *HandlerError {
	return &HandlerError{Code: code, Message: message}
}

// Request is one inbound core request, decoded from the envelope.
type Request struct {
	RequestID  int64
	Action     string
	ChatID     int64
	HasChatID  bool
	UserID     int64
	HasUserID  bool
	DeadlineMs int
	Payload    json.RawMessage
}

// Handler answers one action. The returned value is serialised as
// the response's data object. Returning a *HandlerError reports its
// code; any other error reports internal. The context is cancelled
// when the request's deadline_ms elapses — handlers must abandon
// work then, not keep running past it.
type Handler func(ctx context.Context, req Request) (any, error)

// --- frame codec (§2) -----------------------------------------------

// EncodeFrame renders one frame: 4-byte LE length + payload. It
// errors on an empty or oversize payload instead of emitting a
// frame the peer must reject. This is byte-compatible with
// parlay::uds::encodeFrame in services/core-cpp/src/ipc.hpp.
func EncodeFrame(payload []byte) ([]byte, error) {
	if len(payload) == 0 {
		return nil, errors.New("uds: refusing to encode an empty frame")
	}
	if len(payload) > MaxFrameBytes {
		return nil, fmt.Errorf("uds: payload %d bytes exceeds max frame %d bytes", len(payload), MaxFrameBytes)
	}
	frame := make([]byte, 4+len(payload))
	binary.LittleEndian.PutUint32(frame[:4], uint32(len(payload)))
	copy(frame[4:], payload)
	return frame, nil
}

// WriteFrame writes one whole frame (looped write; a short write is
// retried, an error fails the call).
func WriteFrame(w io.Writer, payload []byte) error {
	frame, err := EncodeFrame(payload)
	if err != nil {
		return err
	}
	for len(frame) > 0 {
		n, err := w.Write(frame)
		if err != nil {
			return err
		}
		frame = frame[n:]
	}
	return nil
}

// ReadFrame reads one whole frame, blocking. It returns an error on
// EOF, a short read, or an invalid (0 or > MaxFrameBytes) length
// prefix — the caller drops the connection on any of these.
func ReadFrame(r io.Reader) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.LittleEndian.Uint32(header[:])
	if length == 0 || length > MaxFrameBytes {
		return nil, fmt.Errorf("uds: invalid frame length %d", length)
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

// --- envelopes (§3) --------------------------------------------------

type envelope struct {
	Type       string          `json:"type"`
	RequestID  int64           `json:"request_id,omitempty"`
	Action     string          `json:"action,omitempty"`
	ChatID     *int64          `json:"chat_id,omitempty"`
	UserID     *int64          `json:"user_id,omitempty"`
	DeadlineMs int             `json:"deadline_ms,omitempty"`
	Payload    json.RawMessage `json:"payload,omitempty"`
	Event      string          `json:"event,omitempty"`
	Data       json.RawMessage `json:"data,omitempty"`
	OK         *bool           `json:"ok,omitempty"`
	Error      *struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error,omitempty"`
	LocalSDP string `json:"local_sdp,omitempty"`
}

func responseEnvelope(requestID int64, data any) ([]byte, error) {
	raw, err := json.Marshal(data)
	if err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{
		"type": "response", "request_id": requestID, "ok": true,
		"data": json.RawMessage(raw),
	})
}

func errorEnvelope(requestID int64, code ErrorCode, message string) []byte {
	raw, _ := json.Marshal(map[string]any{
		"type": "response", "request_id": requestID, "ok": false,
		"error": map[string]string{"code": string(code), "message": message},
	})
	return raw
}

// --- server ----------------------------------------------------------

type joinResult struct {
	data json.RawMessage
	err  error
}

// Server is the UDS listener. Exactly one core connection is active
// at a time (§1): a new connection replaces the previous one, which
// is closed — the core reconnects with backoff when its socket
// drops, and never holds two live connections.
type Server struct {
	socketPath string

	mu       sync.Mutex
	handlers map[string]Handler

	// regCounts records how many times each action was registered.
	// Register overwrites silently by design (the last registration
	// wins), so a duplicate registration is invisible in the
	// handler table — the count is the only witness. The wiring
	// test asserts every contract action has count exactly 1.
	regCounts map[string]int

	listener net.Listener
	conn     *conn // the single active connection, if any

	// peerCh carries one registration signal per accepted
	// connection (buffered, non-blocking send in acceptLoop).
	// WaitForPeer consumes it; it exists because Accept runs
	// asynchronously — a client that dialled successfully has no
	// guarantee the server has registered the connection yet,
	// and an Emit in that window would be dropped.
	peerCh chan struct{}

	nextJoinID   atomic.Int64
	pendingJoins map[int64]chan joinResult
	joinsMu      sync.Mutex

	closed atomic.Bool
	done   chan struct{}
}

type conn struct {
	c       net.Conn
	writeMu sync.Mutex
}

// NewServer builds a server for the given socket path. The handler
// table starts with only the protocol-level ping action registered
// (uds-protocol.md §4: ping is not one of the 56 contract actions);
// Wave 2 registers the contract actions via Register.
func NewServer(socketPath string) *Server {
	s := &Server{
		socketPath:   socketPath,
		handlers:     map[string]Handler{},
		regCounts:    map[string]int{},
		peerCh:       make(chan struct{}, 1),
		pendingJoins: map[int64]chan joinResult{},
		done:         make(chan struct{}),
	}
	s.nextJoinID.Store(JoinRequestIDBase)
	// Registered through Register (not a direct map write) so the
	// registration count witnesses it like every other action.
	s.Register("ping", func(_ context.Context, _ Request) (any, error) {
		return map[string]string{"service": "backend-go", "version": Version}, nil
	})
	return s
}

// Register adds (or replaces) the handler for an action. Unknown
// actions answer not_found; test.sleep_ms is stub-only and always
// answers not_found (uds-protocol.md §10) — it cannot be
// registered.
func (s *Server) Register(action string, h Handler) {
	if action == "test.sleep_ms" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.handlers[action] = h
	s.regCounts[action]++
}

// Dispatch invokes the handler for req.Action in-process and
// returns its data or error, with the same lookup rules as the
// socket path (handleRequest): an unknown action — including
// test.sleep_ms — answers ErrNotFound. It exists so the HTTP
// server (internal/httpapi, Phase 6) can serve the Mini App
// through the exact vc.* / gui.* handlers the core uses over the
// socket, instead of a second implementation of the same actions.
// The caller owns timeouts: Dispatch runs the handler
// synchronously on the caller's goroutine, so pass a context
// with a deadline (the socket path applies deadline_ms the same
// way). Dispatch never touches the socket or the peer connection.
func (s *Server) Dispatch(ctx context.Context, req Request) (any, error) {
	s.mu.Lock()
	h, ok := s.handlers[req.Action]
	s.mu.Unlock()
	if req.Action == "test.sleep_ms" || !ok {
		return nil, NewHandlerError(ErrNotFound, "unknown action "+req.Action)
	}
	return h(ctx, req)
}

// RegistrationCount reports how many times action was registered
// (0 when never). The full-wiring test asserts 1 for every
// contract action: a duplicate registration silently overwrites
// the handler, so only the count can prove exactly-once wiring.
func (s *Server) RegistrationCount(action string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.regCounts[action]
}

// Actions returns the sorted list of registered action names.
func (s *Server) Actions() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]string, 0, len(s.handlers))
	for a := range s.handlers {
		out = append(out, a)
	}
	sort.Strings(out)
	return out
}

// SocketPath returns the path the server listens on.
func (s *Server) SocketPath() string { return s.socketPath }

// Listen removes any stale socket, binds the AF_UNIX listener and
// starts accepting. The socket file is created 0600 in a 0700 data
// directory (§7: filesystem permissions are the access control).
func (s *Server) Listen() error {
	if err := os.Remove(s.socketPath); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("uds: remove stale socket %s: %w", s.socketPath, err)
	}
	ln, err := net.Listen("unix", s.socketPath)
	if err != nil {
		return fmt.Errorf("uds: listen %s: %w", s.socketPath, err)
	}
	if err := os.Chmod(s.socketPath, 0o600); err != nil {
		ln.Close()
		return fmt.Errorf("uds: chmod socket %s: %w", s.socketPath, err)
	}
	s.listener = ln
	go s.acceptLoop()
	return nil
}

// Close stops accepting and drops the active connection.
func (s *Server) Close() error {
	if !s.closed.CompareAndSwap(false, true) {
		return nil
	}
	close(s.done)
	var err error
	if s.listener != nil {
		err = s.listener.Close()
	}
	s.mu.Lock()
	if s.conn != nil {
		s.conn.c.Close()
	}
	s.mu.Unlock()
	return err
}

func (s *Server) acceptLoop() {
	for {
		c, err := s.listener.Accept()
		if err != nil {
			select {
			case <-s.done:
				return
			default:
				log.Printf("uds: accept error: %v", err)
				continue
			}
		}
		nc := &conn{c: c}
		s.mu.Lock()
		if s.conn != nil {
			s.conn.c.Close()
		}
		s.conn = nc
		s.mu.Unlock()
		// Signal registration (non-blocking: a stale signal in
		// the buffer is harmless — WaitForPeer re-checks s.conn
		// after every signal).
		select {
		case s.peerCh <- struct{}{}:
		default:
		}
		go s.serveConn(nc)
	}
}

func (s *Server) serveConn(c *conn) {
	defer func() {
		c.c.Close()
		s.mu.Lock()
		if s.conn == c {
			s.conn = nil
		}
		s.mu.Unlock()
	}()
	for {
		payload, err := ReadFrame(c.c)
		if err != nil {
			return // EOF or protocol error: drop the connection
		}
		var env envelope
		if err := json.Unmarshal(payload, &env); err != nil {
			continue // corrupt frame, ignored (§2: reader drops it)
		}
		switch env.Type {
		case "request":
			// Dispatch concurrently: the read loop must keep
			// reading while a handler runs. A handler that
			// waits on a backend-initiated frame (RequestJoin
			// waits for the core's join response) deadlocks
			// against a serial read loop — the response sits
			// unread in the socket while the handler waits for
			// it, until the request deadline (Phase 3/5 vc.join
			// wedge: abandoned at the full 60 s deadline) —
			// and every later request on the connection starves
			// behind the wedged handler (Phase 5 vc.lyrics
			// flake). Responses are multiplexed by request_id
			// (§3), so order between requests is not
			// significant; concurrent writes are serialised by
			// writeMu. Each handler is still bounded by its own
			// deadline ctx in handleRequest, so a slow handler
			// cannot run past its deadline even though the
			// read loop no longer waits for it.
			go s.handleRequest(c, env)
		case "response":
			s.deliverJoinResponse(env)
		default:
			// Events and join_requests are backend-initiated; the
			// core never sends them. Unknown types are ignored.
		}
	}
}

func (s *Server) handleRequest(c *conn, env envelope) {
	req := Request{
		RequestID:  env.RequestID,
		Action:     env.Action,
		DeadlineMs: env.DeadlineMs,
		Payload:    env.Payload,
	}
	if env.ChatID != nil {
		req.ChatID, req.HasChatID = *env.ChatID, true
	}
	if env.UserID != nil {
		req.UserID, req.HasUserID = *env.UserID, true
	}
	// Promote user_id from the payload when the envelope does not
	// carry one (uds-protocol.md §3.1: requested_by, voter_id,
	// created_by fallbacks).
	if !req.HasUserID && len(env.Payload) > 0 {
		var p struct {
			UserID      *int64 `json:"user_id"`
			RequestedBy *int64 `json:"requested_by"`
			VoterID     *int64 `json:"voter_id"`
			CreatedBy   *int64 `json:"created_by"`
		}
		if json.Unmarshal(env.Payload, &p) == nil {
			switch {
			case p.UserID != nil:
				req.UserID, req.HasUserID = *p.UserID, true
			case p.RequestedBy != nil:
				req.UserID, req.HasUserID = *p.RequestedBy, true
			case p.VoterID != nil:
				req.UserID, req.HasUserID = *p.VoterID, true
			case p.CreatedBy != nil:
				req.UserID, req.HasUserID = *p.CreatedBy, true
			}
		}
	}

	s.mu.Lock()
	h, ok := s.handlers[req.Action]
	s.mu.Unlock()

	// test.sleep_ms is stub-only: the real backend answers
	// not_found (§10), as does any unknown action.
	if req.Action == "test.sleep_ms" || !ok {
		s.writeFrame(c, errorEnvelope(req.RequestID, ErrNotFound, "unknown action "+req.Action))
		return
	}

	deadlineMs := req.DeadlineMs
	if deadlineMs <= 0 {
		deadlineMs = 30000
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(deadlineMs)*time.Millisecond)
	defer cancel()

	type outcome struct {
		data any
		err  error
	}
	done := make(chan outcome, 1)
	go func() {
		data, err := h(ctx, req)
		done <- outcome{data, err}
	}()
	select {
	case <-ctx.Done():
		// Deadline elapsed: abandon the work. No response is
		// written — the core has already failed the call and
		// ignores a late response (§8).
		log.Printf("uds: action %s request %d abandoned past deadline %dms", req.Action, req.RequestID, deadlineMs)
	case out := <-done:
		if out.err != nil {
			var herr *HandlerError
			if errors.As(out.err, &herr) {
				s.writeFrame(c, errorEnvelope(req.RequestID, herr.Code, herr.Message))
			} else {
				s.writeFrame(c, errorEnvelope(req.RequestID, ErrInternal, out.err.Error()))
			}
			return
		}
		raw, err := responseEnvelope(req.RequestID, out.data)
		if err != nil {
			s.writeFrame(c, errorEnvelope(req.RequestID, ErrInternal, "failed to encode response"))
			return
		}
		s.writeFrame(c, raw)
	}
}

func (s *Server) writeFrame(c *conn, payload []byte) {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	_ = WriteFrame(c.c, payload)
}

// deliverJoinResponse routes a response frame to the pending
// JoinRequest with the same request_id. A response with no pending
// join (late, duplicate, unsolicited) is ignored (§3.2).
func (s *Server) deliverJoinResponse(env envelope) {
	if env.OK == nil {
		return
	}
	s.joinsMu.Lock()
	ch, ok := s.pendingJoins[env.RequestID]
	if ok {
		delete(s.pendingJoins, env.RequestID)
	}
	s.joinsMu.Unlock()
	if !ok {
		return
	}
	if !*env.OK {
		msg := "join failed"
		if env.Error != nil && env.Error.Message != "" {
			msg = env.Error.Message
		}
		ch <- joinResult{err: errors.New(msg)}
		return
	}
	ch <- joinResult{data: env.Data}
}

// WaitForPeer blocks until the core connection has registered
// (acceptLoop has run and s.conn is set), or ctx expires, or the
// server closes. Dialling the socket is not enough: Accept runs
// asynchronously, so a caller that must not lose the first frame
// (an event, a JoinRequest) waits here instead of sleeping or
// hoping. A returned nil means a connection is registered right
// now; it may still drop afterwards — this is a registration
// gate, not a liveness guarantee.
func (s *Server) WaitForPeer(ctx context.Context) error {
	for {
		s.mu.Lock()
		connected := s.conn != nil
		s.mu.Unlock()
		if connected {
			return nil
		}
		select {
		case <-s.peerCh:
			// Re-check: the signal may be stale (a connection
			// registered and dropped before we looked).
		case <-ctx.Done():
			return fmt.Errorf("uds: wait for core connection: %w", ctx.Err())
		case <-s.done:
			return errors.New("uds: server closed while waiting for core connection")
		}
	}
}

// Emit pushes a backend-initiated event over the active connection
// (§5). With no core connected the event is dropped — events are
// fire-and-forget, never queued — but the drop is logged loudly:
// a silent drop hid a real accept-loop race (the first event
// after a dial could vanish in the microseconds before
// acceptLoop registered the connection). Callers that must not
// lose the first frame wait on WaitForPeer first.
func (s *Server) Emit(event string, chatID int64, data any) {
	s.mu.Lock()
	c := s.conn
	s.mu.Unlock()
	if c == nil {
		log.Printf("uds: Emit %q (chat %d) dropped: no core connected", event, chatID)
		return
	}
	raw, err := json.Marshal(data)
	if err != nil {
		log.Printf("uds: Emit %q (chat %d) dropped: marshal: %v", event, chatID, err)
		return
	}
	frame, err := json.Marshal(map[string]any{
		"type": "event", "event": event, "chat_id": chatID,
		"data": json.RawMessage(raw),
	})
	if err != nil {
		log.Printf("uds: Emit %q (chat %d) dropped: frame marshal: %v", event, chatID, err)
		return
	}
	s.writeFrame(c, frame)
}

// RequestJoin sends a backend-initiated JoinRequest (§6) with the
// backend's local SDP offer and waits for the core's JoinResponse
// ({remote_sdp, group_call_id}). A missing handler or a failed join
// on the core side answers ok:false, which is an error here — never
// a silent join. The ctx deadline bounds the wait.
func (s *Server) RequestJoin(ctx context.Context, chatID int64, localSDP string) (remoteSDP string, groupCallID int64, err error) {
	s.mu.Lock()
	c := s.conn
	s.mu.Unlock()
	if c == nil {
		return "", 0, errors.New("uds: no core connected for JoinRequest")
	}
	id := s.nextJoinID.Add(1) - 1
	frame, err := json.Marshal(map[string]any{
		"type": "join_request", "request_id": id, "chat_id": chatID,
		"local_sdp": localSDP,
	})
	if err != nil {
		return "", 0, err
	}
	ch := make(chan joinResult, 1)
	s.joinsMu.Lock()
	s.pendingJoins[id] = ch
	s.joinsMu.Unlock()
	defer func() {
		s.joinsMu.Lock()
		delete(s.pendingJoins, id)
		s.joinsMu.Unlock()
	}()
	s.writeFrame(c, frame)
	select {
	case <-ctx.Done():
		return "", 0, fmt.Errorf("uds: JoinRequest %d: %w", id, ctx.Err())
	case res := <-ch:
		if res.err != nil {
			return "", 0, res.err
		}
		var data struct {
			RemoteSDP   string `json:"remote_sdp"`
			GroupCallID int64  `json:"group_call_id"`
		}
		if err := json.Unmarshal(res.data, &data); err != nil {
			return "", 0, fmt.Errorf("uds: JoinRequest %d: bad response: %w", id, err)
		}
		return data.RemoteSDP, data.GroupCallID, nil
	}
}
