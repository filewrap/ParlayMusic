package voice

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/dsp"
)

// JoinFunc completes the voice join handshake for one chat. It
// receives the local connection parameters produced by CreateCall —
// ntgcalls' JSON params object (ufrag, pwd, fingerprints, ssrc),
// called local_sdp in contracts/uds-protocol.md §6 — and must return
// the remote parameters (the transport JSON from the TDLib
// joinVideoChat) and the Telegram group call id. The voice package
// never calls TDLib itself and never touches the UDS socket: Wave 2
// wires this function to the UDS server's JoinRequest
// (contracts/uds-protocol.md §6), which asks the core to run
// joinVideoChat(local_sdp) on the assistant session and returns
// {remote_sdp, group_call_id}.
type JoinFunc func(ctx context.Context, chatID int64, localSDP string) (remoteSDP string, groupCallID int64, err error)

// CallState is the lifecycle state of one chat's call.
type CallState int

const (
	// StateCreated means CreateCall succeeded and the handshake has
	// not finished yet.
	StateCreated CallState = iota
	// StateJoining means the JoinFunc is running.
	StateJoining
	// StateConnected means Connect succeeded; the call is live.
	StateConnected
	// StateFailed means a stage failed; FailedStage and FailedReason
	// name it exactly.
	StateFailed
)

// String renders the state for logs and diagnostics.
func (s CallState) String() string {
	switch s {
	case StateCreated:
		return "created"
	case StateJoining:
		return "joining"
	case StateConnected:
		return "connected"
	case StateFailed:
		return "failed"
	default:
		return "unknown"
	}
}

// Call is the per-chat call record owned by the Manager.
type Call struct {
	ChatID      int64
	GroupCallID int64
	State       CallState
	Paused      bool
	Muted       bool
	LocalSDP    string
	RemoteSDP   string
	// FailedStage is the join stage that failed ("create_call",
	// "join", or "connect"); empty unless State is StateFailed.
	FailedStage string
	// FailedReason is the exact error from the failed stage.
	FailedReason string
	// FeedEnded is set when the current track's decode stream ends
	// (clean EOF) or the feed dies; FeedErr holds the feed error, or
	// nil on a clean end of stream.
	FeedEnded bool
	FeedErr   error

	feed *dspFeed
}

// ErrNoFeed is returned by ApplyFX and LoadNext when the chat has
// no live feed: there is no DSP stream to push parameters or a
// next slot into. Callers that record FX for the next track
// (fx.apply with nothing playing) treat this as "recorded, not
// live" rather than as a failure.
var ErrNoFeed = errors.New("voice: no live feed in this chat")

// Manager owns one ntgcalls Client and the per-chat call records.
// It performs the join handshake through its JoinFunc and drives
// the DSP frame feed for playback. It never calls TDLib and
// never touches the UDS socket.
//
// Assistant rotation (queue-port Tier 2, item 13) — the seam, not
// the full plumbing: the Manager holds a registry of assistant
// accounts and Join rotates through the healthy ones, evicting
// the one a Rotate-class error names. Today the registry holds
// exactly ONE assistant ("assistant") — the C++ core runs
// joinVideoChat on its single shared assistant TDLib session
// (dispatcher.cpp: "the shared assistant session"), and the
// JoinFunc carries no assistant id, so every attempt reaches
// that same session. A second assistant becomes real only with
// core-side work: one TDLib client (session dir) per assistant
// plus an assistant id on the UDS JoinRequest so the core can
// route the join to the right session. Until that lands,
// registering a second assistant here changes nothing at the
// core, and a Rotate-class error with one assistant ends the
// join with "no healthy assistant available" — honest, and the
// eviction lasts for the process lifetime (a restart gives a
// clean slate). Do not present the registry as multi-assistant
// support until the core routing exists.
type Manager struct {
	client *Client
	join   JoinFunc

	// dspEngine is the parlay-dsp engine the feeds decode with,
	// installed by SetDSP (main.go, from dsp.New()). A nil engine
	// means Play/PlayStream fail honestly with dsp.ErrUnavailable
	// instead of playing.
	dspEngine dsp.Engine

	mu    sync.Mutex
	calls map[int64]*Call

	// assistants is the rotation registry (item 13). assistantMu
	// guards it separately from mu so an eviction never blocks
	// on a join in progress.
	assistantMu sync.Mutex
	assistants  []Assistant

	// joining tracks chats with a Join call in flight, guarded
	// by joiningMu. Join is retried in a loop now (up to
	// MaxAttempts with sleeps between attempts), so the
	// prologue check and the first record creation are no
	// longer one lock hold — without this mark, two concurrent
	// Joins (vc.play under the chat lock, radio's voiceHandoff
	// outside it) could both pass the prologue and run two
	// handshakes for one chat. The mark is the "already in
	// progress" answer; the Connected check still runs first,
	// so a join that just succeeded answers with its group
	// call id instead of "in progress".
	joiningMu sync.Mutex
	joining   map[int64]bool

	// preJoinHook, when set, runs before CreateCall on every
	// Join (after the already-connected / already-in-progress
	// checks, before any encoder is created). The backend
	// installs the effective-Opus-config applier here
	// (internal/quality, wired by main.go): store override for
	// the joining chat > parlay.toml [quality] explicit keys >
	// voice_profile preset > built-in default. The hook must
	// run before CreateCall because the encoder reads the
	// process-global settings at creation — a hook that ran
	// after Connect would apply to the NEXT call, not this
	// one. A hook error fails the join at stage "opus_config"
	// (the call record lands in StateFailed like any other
	// stage failure, so a retry re-runs the hook).
	preJoinHook func(ctx context.Context, chatID int64) error

	handlersMu               sync.RWMutex
	streamEndHandlers        []StreamEndFunc
	connectionChangeHandlers []ConnectionChangeFunc
	feedEndHandlers          []func(chatID int64, err error)
	transitionHandlers       []func(chatID int64, trackID string)
}

// Assistant is one assistant account in the rotation registry
// (queue-port Tier 2, item 13). ID is the stable name the core
// will one day route a join to; Evicted means a Rotate-class
// error named this account and it is out of rotation until the
// backend restarts. EvictReason is the error that evicted it.
type Assistant struct {
	ID          string
	Evicted     bool
	EvictReason string
}

// singleAssistantID is the id of the one assistant Parlay has
// today: the core's single shared assistant session. It is the
// registry's only entry until the core-side multi-assistant
// plumbing lands (see the Manager doc).
const singleAssistantID = "assistant"

// NewManager builds a Manager around client. join may be nil only
// when Join will never be called; Join with a nil JoinFunc fails with
// an explicit error instead of pretending to join.
func NewManager(client *Client, join JoinFunc) *Manager {
	m := &Manager{
		client:     client,
		join:       join,
		calls:      make(map[int64]*Call),
		assistants: []Assistant{{ID: singleAssistantID}},
		joining:    map[int64]bool{},
	}
	// ntgcalls callbacks arrive on library threads and must not
	// block, so the Manager's own dispatch runs in a goroutine.
	client.OnStreamEnd(func(chatID int64, streamType StreamType, device StreamDevice) {
		go m.dispatchStreamEnd(chatID, streamType, device)
	})
	client.OnConnectionChange(func(chatID int64, state ConnectionState, kind ConnectionKind) {
		go m.dispatchConnectionChange(chatID, state, kind)
	})
	return m
}

// RegisterAssistant adds id to the rotation registry, if it is
// not already there. It is the seam for N assistants — but
// registering a second id changes nothing until the core routes
// joins per assistant (see the Manager doc): the JoinFunc
// carries no assistant id today, so every attempt still reaches
// the single shared session.
func (m *Manager) RegisterAssistant(id string) {
	if strings.TrimSpace(id) == "" {
		return
	}
	m.assistantMu.Lock()
	defer m.assistantMu.Unlock()
	for _, a := range m.assistants {
		if a.ID == id {
			return
		}
	}
	m.assistants = append(m.assistants, Assistant{ID: id})
}

// CurrentAssistantID returns the first non-evicted assistant's
// id, or "" when every assistant is evicted.
func (m *Manager) CurrentAssistantID() string {
	m.assistantMu.Lock()
	defer m.assistantMu.Unlock()
	for _, a := range m.assistants {
		if !a.Evicted {
			return a.ID
		}
	}
	return ""
}

// HealthyAssistants returns the ids of the non-evicted
// assistants, in registry order.
func (m *Manager) HealthyAssistants() []string {
	m.assistantMu.Lock()
	defer m.assistantMu.Unlock()
	var out []string
	for _, a := range m.assistants {
		if !a.Evicted {
			out = append(out, a.ID)
		}
	}
	return out
}

// EvictAssistant marks id evicted with reason, out of rotation
// until the backend restarts. It is a no-op for an unknown id.
// It returns whether an entry was actually evicted.
func (m *Manager) EvictAssistant(id, reason string) bool {
	m.assistantMu.Lock()
	defer m.assistantMu.Unlock()
	for i := range m.assistants {
		if m.assistants[i].ID == id && !m.assistants[i].Evicted {
			m.assistants[i].Evicted = true
			m.assistants[i].EvictReason = reason
			log.Printf("voice: assistant %q evicted: %s", id, reason)
			return true
		}
	}
	return false
}

// EvictCurrentAssistant evicts the first healthy assistant and
// returns its id, or "" when none is healthy. The join path
// calls this on a Rotate-class error (item 13).
func (m *Manager) EvictCurrentAssistant(reason string) string {
	id := m.CurrentAssistantID()
	if id == "" {
		return ""
	}
	m.EvictAssistant(id, reason)
	return id
}

// SetPreJoinHook installs fn, run before CreateCall on every
// Join (see the preJoinHook field). A nil fn clears the hook.
// It is called once at startup (main.go); calling it while
// joins are running races the next Join, like any other
// one-time wiring call.
func (m *Manager) SetPreJoinHook(fn func(ctx context.Context, chatID int64) error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.preJoinHook = fn
}

// OnStreamEnd registers a handler for ntgcalls stream-end events.
func (m *Manager) OnStreamEnd(fn StreamEndFunc) {
	m.handlersMu.Lock()
	defer m.handlersMu.Unlock()
	m.streamEndHandlers = append(m.streamEndHandlers, fn)
}

// OnConnectionChange registers a handler for connection changes.
func (m *Manager) OnConnectionChange(fn ConnectionChangeFunc) {
	m.handlersMu.Lock()
	defer m.handlersMu.Unlock()
	m.connectionChangeHandlers = append(m.connectionChangeHandlers, fn)
}

// SetDSP installs the parlay-dsp engine the Manager's feeds
// decode with. It is called once at startup (main.go, from
// dsp.New()); calling it while feeds are running is a wiring bug
// and the new engine applies only to feeds started after the call.
func (m *Manager) SetDSP(e dsp.Engine) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.dspEngine = e
}

// OnFeedEnd registers a handler invoked when a chat's DSP feed
// ends: err is nil on a clean end of stream (track finished), or the
// exact feed error when the DSP died or the frame pump failed.
func (m *Manager) OnFeedEnd(fn func(chatID int64, err error)) {
	m.handlersMu.Lock()
	defer m.handlersMu.Unlock()
	m.feedEndHandlers = append(m.feedEndHandlers, fn)
}

// OnTransition registers a handler invoked when a chat's DSP feed
// promotes its next slot: the track with id trackID is now
// playing, gaplessly, with no rebind. The handler must never
// block the feed pump: handlers dispatch to a worker goroutine
// (see handlers.onTransition). The feed keeps playing the promoted
// slot while the handler runs.
func (m *Manager) OnTransition(fn func(chatID int64, trackID string)) {
	m.handlersMu.Lock()
	defer m.handlersMu.Unlock()
	m.transitionHandlers = append(m.transitionHandlers, fn)
}

// onTransition forwards a DSP slot promotion to the registered
// handlers. It never blocks: it only copies the handler list and
// calls each one, and every handler dispatches asynchronously.
func (m *Manager) onTransition(chatID int64, trackID string) {
	m.handlersMu.RLock()
	handlers := append([]func(chatID int64, trackID string){}, m.transitionHandlers...)
	m.handlersMu.RUnlock()
	for _, fn := range handlers {
		fn(chatID, trackID)
	}
}

func (m *Manager) dispatchStreamEnd(chatID int64, streamType StreamType, device StreamDevice) {
	m.handlersMu.RLock()
	handlers := append([]StreamEndFunc(nil), m.streamEndHandlers...)
	m.handlersMu.RUnlock()
	for _, fn := range handlers {
		fn(chatID, streamType, device)
	}
}

func (m *Manager) dispatchConnectionChange(chatID int64, state ConnectionState, kind ConnectionKind) {
	if state == ConnectionFailed || state == ConnectionTimeout || state == ConnectionClosed {
		m.mu.Lock()
		if call, ok := m.calls[chatID]; ok && call.State == StateConnected {
			call.State = StateFailed
			call.FailedStage = "connection"
			call.FailedReason = fmt.Sprintf("ntgcalls connection %s", stateName(state))
		}
		m.mu.Unlock()
	}
	m.handlersMu.RLock()
	handlers := append([]ConnectionChangeFunc(nil), m.connectionChangeHandlers...)
	m.handlersMu.RUnlock()
	for _, fn := range handlers {
		fn(chatID, state, kind)
	}
}

func stateName(s ConnectionState) string {
	switch s {
	case ConnectionConnected:
		return "connected"
	case ConnectionFailed:
		return "failed"
	case ConnectionTimeout:
		return "timeout"
	case ConnectionClosed:
		return "closed"
	default:
		return "connecting"
	}
}

// Join performs the voice join handshake for chatID and returns the
// group call id:
//
//  1. create_call — the local connection parameters are generated
//     in-process (ntgcalls' JSON params object, not SDP text).
//  2. join — the JoinFunc runs joinVideoChat(local_sdp) via the core
//     and returns the remote parameters plus the group call id.
//  3. connect — the remote parameters complete the handshake in
//     ntgcalls.
//
// A failure at any stage returns an error naming the exact stage and
// reason, leaves the call record in StateFailed with FailedStage /
// FailedReason set, and never reports success. Joining a chat that is
// already connected returns the existing group call id without
// re-running the handshake.
//
// Retry/rotate policy (queue-port Tier 2, items 13 + 16): one
// handshake is attempted at most MaxAttempts times, classified by
// the shared classifier (errors.go). A Fatal or Unknown error
// returns immediately; a Retryable error sleeps RetryDelay
// (ctx-aware — a cancelled context ends the join with the last
// stage error, not a context error) and retries the same
// assistant; a Rotate error evicts the current assistant and
// tries the next healthy one, and when none is healthy the join
// fails with "no healthy assistant available". The per-attempt
// work lives in joinOnce; the policy lives here, so the stages
// never learn about rotation.
func (m *Manager) Join(ctx context.Context, chatID int64) (int64, error) {
	m.mu.Lock()
	if call, ok := m.calls[chatID]; ok && call.State == StateConnected {
		groupCallID := call.GroupCallID
		m.mu.Unlock()
		return groupCallID, nil
	}
	m.mu.Unlock()

	// One Join per chat at a time (see the joining field): a
	// second caller gets "already in progress" instead of
	// starting a parallel handshake.
	m.joiningMu.Lock()
	if m.joining[chatID] {
		m.joiningMu.Unlock()
		return 0, fmt.Errorf("voice: join chat %d: join already in progress", chatID)
	}
	m.joining[chatID] = true
	m.joiningMu.Unlock()
	defer func() {
		m.joiningMu.Lock()
		delete(m.joining, chatID)
		m.joiningMu.Unlock()
	}()

	var lastErr error
	for attempt := 1; attempt <= MaxAttempts; attempt++ {
		groupCallID, err := m.joinOnce(ctx, chatID)
		if err == nil {
			return groupCallID, nil
		}
		lastErr = err
		if attempt == MaxAttempts {
			break
		}
		switch Classify(err) {
		case ClassFatal, ClassUnknown:
			return 0, err
		case ClassRotate:
			evicted := m.EvictCurrentAssistant(err.Error())
			if m.CurrentAssistantID() == "" {
				return 0, fmt.Errorf("voice: join chat %d: assistant %q evicted (%s); no healthy assistant available: %w", chatID, evicted, err.Error(), err)
			}
			log.Printf("voice: join chat %d: attempt %d failed (%v); rotating to assistant %q", chatID, attempt, err, m.CurrentAssistantID())
		case ClassRetryable:
			delay := RetryDelay(err)
			log.Printf("voice: join chat %d: attempt %d failed (%v); retrying in %s", chatID, attempt, err, delay)
			if !sleepCtx(ctx, delay) {
				return 0, lastErr
			}
		}
	}
	return 0, fmt.Errorf("voice: join chat %d: failed after %d attempts: %w", chatID, MaxAttempts, lastErr)
}

// sleepCtx sleeps d, or returns false early when ctx is done. A
// zero or negative d returns true at once.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		return true
	}
	select {
	case <-ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}

// joinOnce runs one join handshake for chatID: the pre-join hook,
// CreateCall, the JoinFunc, and Connect. It creates a fresh call
// record (a previous attempt left StateFailed, which Join's
// prologue lets through) and leaves it StateFailed with
// FailedStage / FailedReason on any stage failure. Callers must
// have run Join's prologue (already-connected / already-in-progress
// checks) — joinOnce does not repeat them.
func (m *Manager) joinOnce(ctx context.Context, chatID int64) (int64, error) {
	m.mu.Lock()
	call := &Call{ChatID: chatID, State: StateCreated}
	m.calls[chatID] = call
	hook := m.preJoinHook
	m.mu.Unlock()

	fail := func(stage string, err error) (int64, error) {
		// Best-effort teardown of the half-created call; the
		// teardown error never masks the stage error.
		_ = m.client.Stop(chatID)
		m.mu.Lock()
		call.State = StateFailed
		call.FailedStage = stage
		call.FailedReason = err.Error()
		m.mu.Unlock()
		return 0, fmt.Errorf("voice: join chat %d: %s: %w", chatID, stage, err)
	}

	log.Printf("voice: initiating join for chat %d", chatID)
	// The pre-join hook applies the effective Opus config for
	// this chat before any encoder is created (see the
	// preJoinHook field): a hook error fails the join at stage
	// "opus_config" instead of joining with unknown encoder
	// settings.
	if hook != nil {
		if err := hook(ctx, chatID); err != nil {
			return fail("opus_config", err)
		}
	}
	if err := ctx.Err(); err != nil {
		return fail("create_call", fmt.Errorf("context done before create_call: %w", err))
	}
	localSDP, err := m.client.CreateCall(chatID)
	if err != nil {
		return fail("create_call", err)
	}
	log.Printf("voice: CreateCall succeeded for chat %d (local_sdp len: %d)", chatID, len(localSDP))
	m.mu.Lock()
	call.LocalSDP = localSDP
	call.State = StateJoining
	m.mu.Unlock()

	if m.join == nil {
		return fail("join", errors.New("no JoinFunc configured: the voice package never calls TDLib itself; Wave 2 must wire Join to the UDS JoinRequest"))
	}
	log.Printf("voice: invoking JoinFunc for chat %d", chatID)
	remoteSDP, groupCallID, err := m.join(ctx, chatID, localSDP)
	if err != nil {
		log.Printf("voice: JoinFunc returned error for chat %d: %v", chatID, err)
		return fail("join", err)
	}
	if remoteSDP == "" {
		log.Printf("voice: JoinFunc returned empty remote SDP for chat %d", chatID)
		return fail("join", errors.New("JoinFunc returned an empty remote SDP"))
	}
	log.Printf("voice: JoinFunc succeeded for chat %d (remote_sdp len: %d, group_call_id: %d), connecting ntgcalls...", chatID, len(remoteSDP), groupCallID)
	if err := ctx.Err(); err != nil {
		return fail("connect", fmt.Errorf("context done before connect: %w", err))
	}
	// Connect is bounded by ctx and by the client's connect
	// timeout (timeouts.join_ms): a Connect wedged inside the
	// library returns by the deadline instead of holding the
	// join handler — and the per-chat lock — past it, and the
	// call record lands in StateFailed so the next voice-state
	// action (and a retry) is not starved behind the dead join.
	if err := m.client.Connect(ctx, chatID, remoteSDP); err != nil {
		log.Printf("voice: ntgcalls Connect failed for chat %d: %v", chatID, err)
		return fail("connect", err)
	}
	log.Printf("voice: ntgcalls Connect succeeded for chat %d (group_call_id %d)", chatID, groupCallID)
	m.mu.Lock()
	call.RemoteSDP = remoteSDP
	call.GroupCallID = groupCallID
	call.State = StateConnected
	m.mu.Unlock()
	return groupCallID, nil
}

// Play starts playback of the resolved stream source (URL or local
// file path) in chatID's live call. It binds the external audio source
// and starts the room's persistent DSP feed; on a track change the
// previous feed is stopped and a new one started — the feed is
// restarted only on track change, never per call or per frame. Play
// requires a connected call; it does not join. opts carries the
// room's DSP parameters (speed, bass, volume, FX preset) and is
// applied to the stream from its first frame.
//
// The source is opened in Go — http(s) URLs with an HTTP GET, local
// paths with os.Open — and the body is handed to the DSP as its
// byte source. There is no ffmpeg anywhere in this path. Note the
// one behaviour the old ffmpeg feed had that Go does not: ffmpeg
// reconnected a dropped HTTP stream; a mid-stream read failure
// now ends the track (the feed error advances the room), it does
// not silently reconnect.
func (m *Manager) Play(ctx context.Context, chatID int64, source string, opts dsp.Options) error {
	if strings.TrimSpace(source) == "" {
		return fmt.Errorf("voice: play chat %d: empty audio source", chatID)
	}
	r, err := openSource(ctx, source)
	if err != nil {
		return fmt.Errorf("voice: play chat %d: %w", chatID, err)
	}
	if err := m.startStream(ctx, chatID, r, opts); err != nil {
		_ = r.Close()
		return err
	}
	return nil
}

// openSource opens source for DSP decoding: an http(s) URL is
// fetched (the body streams; a non-2xx status is an error), any
// other path is opened as a local file.
//
// A YouTube signed URL 403s for a few seconds after it is issued —
// the videoplayback edge's signature check lags the player API's
// response. The play path opens the URL 2-4s after resolve (the join
// handshake runs first), so the first attempt can still land inside
// the window. A 403 is therefore retried with a 2s backoff (up to
// 4 retries, ~8s worst case; the URL is valid for hours) before
// surfacing as an error.
//
// The returned body is validated before the DSP ever sees it
// (queue-port Tier 2, item 15 — the streaming twin of the
// prefetch prefix check): an empty body, or a body whose first
// byte is `{`, `<`, or `[`, is a JSON/HTML error document
// masquerading as media, not audio, and fails here instead of
// feeding the decoder garbage that ends the track silently.
// No real media format starts with those bytes (MP3 "ID3"/0xFF,
// FLAC "fLaC", OGG "OggS", WebM 0x1A, MP4 box size + "ftyp",
// WAV "RIFF").
func openSource(ctx context.Context, source string) (io.ReadCloser, error) {
	if strings.HasPrefix(source, "http://") || strings.HasPrefix(source, "https://") {
		var resp *http.Response
		for attempt := 0; ; attempt++ {
			req, err := http.NewRequestWithContext(ctx, http.MethodGet, source, nil)
			if err != nil {
				return nil, fmt.Errorf("open stream URL: %w", err)
			}
			resp, err = http.DefaultClient.Do(req)
			if err != nil {
				return nil, fmt.Errorf("open stream URL: %w", err)
			}
			if resp.StatusCode >= 200 && resp.StatusCode < 300 {
				return validateMediaStream(resp.Body, source)
			}
			_ = resp.Body.Close()
			if resp.StatusCode != http.StatusForbidden || attempt >= 4 {
				log.Printf("voice: open stream URL: HTTP %d (%s)", resp.StatusCode, streamURLShort(source))
				return nil, fmt.Errorf("open stream URL: HTTP %d", resp.StatusCode)
			}
			log.Printf("voice: open stream URL: HTTP 403 on fresh signed URL, retry %d/4 in 2s", attempt+1)
			select {
			case <-ctx.Done():
				return nil, fmt.Errorf("open stream URL: %w", ctx.Err())
			case <-time.After(2 * time.Second):
			}
		}
	}
	f, err := os.Open(source)
	if err != nil {
		return nil, fmt.Errorf("open audio file: %w", err)
	}
	return validateMediaStream(f, source)
}

// validateMediaStream peeks the first byte of r and rejects an
// empty body or a body that opens with `{`, `<`, or `[` — a
// JSON/HTML error document, not media (item 15). The peeked byte
// is replayed: the returned reader yields the whole stream from
// the start, so the DSP reads exactly what it would have read.
// The caller owns the returned ReadCloser; on error r is closed.
func validateMediaStream(r io.ReadCloser, source string) (io.ReadCloser, error) {
	display := source
	if strings.HasPrefix(source, "http://") || strings.HasPrefix(source, "https://") {
		display = streamURLShort(source)
	}
	br := bufio.NewReader(r)
	b, err := br.Peek(1)
	if err != nil {
		_ = r.Close()
		if err == io.EOF {
			return nil, fmt.Errorf("open stream: empty response (0 bytes) for %s", display)
		}
		return nil, fmt.Errorf("open stream: peek first byte: %w", err)
	}
	switch b[0] {
	case '{', '<', '[':
		_ = r.Close()
		return nil, fmt.Errorf("open stream: response for %s starts with %q — an error document, not media", display, string(b[0]))
	}
	return struct {
		io.Reader
		io.Closer
	}{br, r}, nil
}

// streamURLShort renders a stream URL for logs: host plus the
// itag/ip query params that identify the signed format.
func streamURLShort(source string) string {
	u, err := url.Parse(source)
	if err != nil {
		return source[:min(len(source), 80)]
	}
	return fmt.Sprintf("%s itag=%s ip=%s", u.Host, u.Query().Get("itag"), u.Query().Get("ip"))
}

// PlayStream starts playback of a pre-buffered stream (the
// Phase 4 prefetch handle) in chatID's live call. It is the same
// DSP path as Play, but the decode input is the prefetch reader —
// the opening seconds are already in memory, so the first frames
// need no fresh HTTP open — instead of a URL or file path. Like
// Play it requires a connected call, binds the external audio
// source, and (re)starts the room's persistent feed on track
// change. opts carries the room's DSP parameters and is applied
// from the first frame.
func (m *Manager) PlayStream(ctx context.Context, chatID int64, r io.ReadCloser, opts dsp.Options) error {
	if r == nil {
		return fmt.Errorf("voice: play stream chat %d: nil stream reader", chatID)
	}
	return m.startStream(ctx, chatID, r, opts)
}

// startStream is the shared Play/PlayStream construction: bind the
// external audio source, open the DSP stream for r with opts, and
// start the feed pump. The caller owns r on error (startStream
// closes it when the DSP stream fails to open); on success the
// DSP stream owns it.
//
// Hot-swap (queue-port Tier 2, item 12): this already IS the
// hot-swap — the old feed is stopped and SetExternalAudioSource
// re-binds the source inside the EXISTING live call; the call is
// never left and re-joined for a track change. The only rejoin
// leg is the set_source failure below, which marks the call
// record failed so the session ends and a fresh /play rejoins.
func (m *Manager) startStream(ctx context.Context, chatID int64, r io.ReadCloser, opts dsp.Options) error {
	if err := opts.Validate(); err != nil {
		return fmt.Errorf("voice: play stream chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	call, ok := m.calls[chatID]
	if !ok || call.State != StateConnected {
		state := "no call"
		if ok {
			state = call.State.String()
		}
		m.mu.Unlock()
		return fmt.Errorf("voice: play stream chat %d: no connected call (state %s); call Join first", chatID, state)
	}
	engine := m.dspEngine
	oldFeed := call.feed
	call.feed = nil
	call.FeedEnded = false
	call.FeedErr = nil
	m.mu.Unlock()

	// Stop the previous track's feed outside the manager lock: the
	// feed's pump goroutine must never block on this lock.
	if oldFeed != nil {
		oldFeed.Stop()
	}
	if engine == nil {
		return fmt.Errorf("voice: play stream chat %d: %w", chatID, dsp.ErrUnavailable)
	}
	if err := m.client.SetExternalAudioSource(chatID); err != nil {
		// Hot-swap failure (queue-port Tier 2, item 12): this
		// IS the hot-swap — startStream already swaps the
		// source inside the live call instead of leaving and
		// rejoining, and the old feed was stopped above. When
		// the swap itself fails the call record is dead (the
		// ntgcalls side has no source to feed), so the record
		// is marked failed here, stage "set_source" — the
		// NoxBeats rule (erase the call record on set_source
		// failure and fall through to a full rejoin). Leaving
		// it StateConnected would make every later bind retry
		// the same dead swap and drain the queue track by
		// track; marked failed, hasLiveCall goes false and
		// the advance path ends the session, and a fresh
		// /play rejoins — the rejoin leg of the hot-swap
		// rule. There is no video flag in Parlay (audio-only
		// external source), so no video-flag rejoin case
		// exists.
		m.mu.Lock()
		if current, ok := m.calls[chatID]; ok && current == call {
			current.State = StateFailed
			current.FailedStage = "set_source"
			current.FailedReason = err.Error()
		}
		m.mu.Unlock()
		log.Printf("voice: play stream chat %d: set external audio source failed (%v); call marked failed", chatID, err)
		return fmt.Errorf("voice: play stream chat %d: set external audio source: %w", chatID, err)
	}
	stream, err := engine.OpenStream(ctx, r, opts)
	if err != nil {
		return fmt.Errorf("voice: play stream chat %d: open DSP stream: %w", chatID, err)
	}
	feed, err := startDSPFeed(ctx, m.client, chatID, stream, func(feedErr error) {
		m.onFeedEnd(chatID, feedErr)
	}, func(trackID string) {
		m.onTransition(chatID, trackID)
	})
	if err != nil {
		_ = stream.Close()
		return fmt.Errorf("voice: play stream chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	// The call may have been stopped while the feed was starting.
	if current, ok := m.calls[chatID]; ok && current == call && current.State == StateConnected {
		call.feed = feed
	} else {
		m.mu.Unlock()
		feed.Stop()
		return fmt.Errorf("voice: play stream chat %d: call ended while the feed was starting", chatID)
	}
	m.mu.Unlock()
	return nil
}

// ApplyFX applies a live zero-restart parameter change to the
// running DSP stream in chatID. A nil error is the DSP's
// confirmation that the effect is active. ErrNoFeed when the chat
// has no live feed (fx.apply records the preset for the next
// track instead).
func (m *Manager) ApplyFX(chatID int64, opts dsp.Options) error {
	m.mu.Lock()
	call, ok := m.calls[chatID]
	if !ok {
		m.mu.Unlock()
		return fmt.Errorf("voice: apply fx chat %d: %w", chatID, ErrNoFeed)
	}
	feed := call.feed
	m.mu.Unlock()
	if feed == nil {
		return fmt.Errorf("voice: apply fx chat %d: %w", chatID, ErrNoFeed)
	}
	if err := feed.ApplyOptions(opts); err != nil {
		return fmt.Errorf("voice: apply fx chat %d: %w", chatID, err)
	}
	return nil
}

// LoadNext loads the next slot of the running DSP stream in
// chatID: the track that will start gaplessly when the active slot
// ends. r is the next track's byte source (a PrefbufferedStream
// reader), opts its DSP parameters, trackID the id the transition
// fires with. ErrNoFeed when the chat has no live feed.
func (m *Manager) LoadNext(chatID int64, r io.ReadCloser, opts dsp.Options, trackID string) error {
	m.mu.Lock()
	call, ok := m.calls[chatID]
	if !ok {
		m.mu.Unlock()
		return fmt.Errorf("voice: load next chat %d: %w", chatID, ErrNoFeed)
	}
	feed := call.feed
	m.mu.Unlock()
	if feed == nil {
		return fmt.Errorf("voice: load next chat %d: %w", chatID, ErrNoFeed)
	}
	if err := feed.LoadNext(r, opts, trackID); err != nil {
		return fmt.Errorf("voice: load next chat %d: %w", chatID, err)
	}
	return nil
}

// onFeedEnd records the feed outcome and notifies handlers. It never
// stops the feed itself: it runs on the feed pump goroutine, and
// stopping there would deadlock on the pump's exit.
func (m *Manager) onFeedEnd(chatID int64, feedErr error) {
	m.mu.Lock()
	if call, ok := m.calls[chatID]; ok {
		call.FeedEnded = true
		call.FeedErr = feedErr
	}
	m.mu.Unlock()
	m.handlersMu.RLock()
	handlers := append([]func(chatID int64, err error){}, m.feedEndHandlers...)
	m.handlersMu.RUnlock()
	for _, fn := range handlers {
		fn(chatID, feedErr)
	}
}

// Pause pauses playback in chatID. The library-level pause is a
// no-op for the external source (ntgcalls only pauses file/shell
// readers), so the decode feed is gated as well; the returned bool is
// the library's "changed" flag, not the paused state.
func (m *Manager) Pause(chatID int64) (bool, error) {
	call, err := m.requireCall(chatID, "pause")
	if err != nil {
		return false, err
	}
	m.mu.Lock()
	feed := call.feed
	m.mu.Unlock()
	if feed != nil {
		feed.Pause()
	}
	changed, err := m.client.Pause(chatID)
	if err != nil {
		if feed != nil {
			feed.Resume()
		}
		return false, fmt.Errorf("voice: pause chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	call.Paused = true
	m.mu.Unlock()
	return changed, nil
}

// Resume resumes playback in chatID. The returned bool is the
// library's "changed" flag (see Pause).
func (m *Manager) Resume(chatID int64) (bool, error) {
	call, err := m.requireCall(chatID, "resume")
	if err != nil {
		return false, err
	}
	changed, err := m.client.Resume(chatID)
	if err != nil {
		return false, fmt.Errorf("voice: resume chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	feed := call.feed
	call.Paused = false
	m.mu.Unlock()
	if feed != nil {
		feed.Resume()
	}
	return changed, nil
}

// Mute mutes playback in chatID. Unlike pause, the library-level mute
// works for the external source (it toggles the outgoing track), so
// no feed gating is needed. The returned bool is the library's
// "changed" flag.
func (m *Manager) Mute(chatID int64) (bool, error) {
	call, err := m.requireCall(chatID, "mute")
	if err != nil {
		return false, err
	}
	changed, err := m.client.Mute(chatID)
	if err != nil {
		return false, fmt.Errorf("voice: mute chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	call.Muted = true
	m.mu.Unlock()
	return changed, nil
}

// Unmute unmutes playback in chatID. The returned bool is the
// library's "changed" flag (see Mute).
func (m *Manager) Unmute(chatID int64) (bool, error) {
	call, err := m.requireCall(chatID, "unmute")
	if err != nil {
		return false, err
	}
	changed, err := m.client.Unmute(chatID)
	if err != nil {
		return false, fmt.Errorf("voice: unmute chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	call.Muted = false
	m.mu.Unlock()
	return changed, nil
}

// Stop ends the call in chatID: the decode feed is stopped first,
// then the ntgcalls call, and the record is removed. The record is
// removed even when the ntgcalls stop fails, because a failed stop
// means the library side is already gone.
func (m *Manager) Stop(chatID int64) error {
	m.mu.Lock()
	call, ok := m.calls[chatID]
	if !ok {
		m.mu.Unlock()
		return fmt.Errorf("voice: stop chat %d: no active call", chatID)
	}
	feed := call.feed
	call.feed = nil
	delete(m.calls, chatID)
	m.mu.Unlock()
	if feed != nil {
		feed.Stop()
	}
	if err := m.client.Stop(chatID); err != nil {
		return fmt.Errorf("voice: stop chat %d: %w", chatID, err)
	}
	return nil
}

// Get returns a copy of the call record for chatID, if one exists.
func (m *Manager) Get(chatID int64) (Call, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	call, ok := m.calls[chatID]
	if !ok {
		return Call{}, false
	}
	return *call, true
}

// ChatIDs returns the chat ids with a call record.
func (m *Manager) ChatIDs() []int64 {
	m.mu.Lock()
	defer m.mu.Unlock()
	ids := make([]int64, 0, len(m.calls))
	for id := range m.calls {
		ids = append(ids, id)
	}
	return ids
}

// Time returns the playback clock of chatID in milliseconds.
func (m *Manager) Time(chatID int64) (uint64, error) {
	if _, err := m.requireCall(chatID, "time"); err != nil {
		return 0, err
	}
	return m.client.Time(chatID)
}

// Close stops every call and destroys the ntgcalls instance.
func (m *Manager) Close() {
	ids := m.ChatIDs()
	for _, id := range ids {
		_ = m.Stop(id)
	}
	m.client.Close()
}

func (m *Manager) requireCall(chatID int64, verb string) (*Call, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	call, ok := m.calls[chatID]
	if !ok {
		return nil, fmt.Errorf("voice: %s chat %d: no active call", verb, chatID)
	}
	return call, nil
}
