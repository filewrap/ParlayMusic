package voice

import (
	"context"
	"errors"
	"fmt"
	"sync"
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

	feed *ffmpegFeed
}

// Manager owns one ntgcalls Client and the per-chat call records.
// It performs the join handshake through its JoinFunc and drives the
// interim ffmpeg frame feed for playback. It never calls TDLib and
// never touches the UDS socket.
type Manager struct {
	client *Client
	join   JoinFunc

	mu    sync.Mutex
	calls map[int64]*Call

	handlersMu               sync.RWMutex
	streamEndHandlers        []StreamEndFunc
	connectionChangeHandlers []ConnectionChangeFunc
	feedEndHandlers          []func(chatID int64, err error)
}

// NewManager builds a Manager around client. join may be nil only
// when Join will never be called; Join with a nil JoinFunc fails with
// an explicit error instead of pretending to join.
func NewManager(client *Client, join JoinFunc) *Manager {
	m := &Manager{
		client: client,
		join:   join,
		calls:  make(map[int64]*Call),
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

// OnFeedEnd registers a handler invoked when a chat's ffmpeg feed
// ends: err is nil on a clean end of stream (track finished), or the
// exact feed error when ffmpeg died or the frame pump failed.
func (m *Manager) OnFeedEnd(fn func(chatID int64, err error)) {
	m.handlersMu.Lock()
	defer m.handlersMu.Unlock()
	m.feedEndHandlers = append(m.feedEndHandlers, fn)
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
func (m *Manager) Join(ctx context.Context, chatID int64) (int64, error) {
	m.mu.Lock()
	if call, ok := m.calls[chatID]; ok {
		switch call.State {
		case StateConnected:
			groupCallID := call.GroupCallID
			m.mu.Unlock()
			return groupCallID, nil
		case StateJoining, StateCreated:
			m.mu.Unlock()
			return 0, fmt.Errorf("voice: join chat %d: join already in progress (state %s)", chatID, call.State)
		}
	}
	call := &Call{ChatID: chatID, State: StateCreated}
	m.calls[chatID] = call
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

	if err := ctx.Err(); err != nil {
		return fail("create_call", fmt.Errorf("context done before create_call: %w", err))
	}
	localSDP, err := m.client.CreateCall(chatID)
	if err != nil {
		return fail("create_call", err)
	}
	m.mu.Lock()
	call.LocalSDP = localSDP
	call.State = StateJoining
	m.mu.Unlock()

	if m.join == nil {
		return fail("join", errors.New("no JoinFunc configured: the voice package never calls TDLib itself; Wave 2 must wire Join to the UDS JoinRequest"))
	}
	remoteSDP, groupCallID, err := m.join(ctx, chatID, localSDP)
	if err != nil {
		return fail("join", err)
	}
	if remoteSDP == "" {
		return fail("join", errors.New("JoinFunc returned an empty remote SDP"))
	}
	if err := ctx.Err(); err != nil {
		return fail("connect", fmt.Errorf("context done before connect: %w", err))
	}
	if err := m.client.Connect(chatID, remoteSDP); err != nil {
		return fail("connect", err)
	}
	m.mu.Lock()
	call.RemoteSDP = remoteSDP
	call.GroupCallID = groupCallID
	call.State = StateConnected
	m.mu.Unlock()
	return groupCallID, nil
}

// Play starts playback of the resolved stream source (URL or local
// file path) in chatID's live call. It binds the external audio source
// and starts the room's persistent ffmpeg decode feed; on a track
// change the previous feed is stopped and a new one started — the
// feed is restarted only on track change, never per call or per
// frame. Play requires a connected call; it does not join.
func (m *Manager) Play(ctx context.Context, chatID int64, source string) error {
	m.mu.Lock()
	call, ok := m.calls[chatID]
	if !ok || call.State != StateConnected {
		state := "no call"
		if ok {
			state = call.State.String()
		}
		m.mu.Unlock()
		return fmt.Errorf("voice: play chat %d: no connected call (state %s); call Join first", chatID, state)
	}
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
	if err := m.client.SetExternalAudioSource(chatID); err != nil {
		return fmt.Errorf("voice: play chat %d: set external audio source: %w", chatID, err)
	}
	feed, err := startFFmpegFeed(ctx, m.client, chatID, source, func(feedErr error) {
		m.onFeedEnd(chatID, feedErr)
	})
	if err != nil {
		return fmt.Errorf("voice: play chat %d: %w", chatID, err)
	}
	m.mu.Lock()
	// The call may have been stopped while the feed was starting.
	if current, ok := m.calls[chatID]; ok && current == call && current.State == StateConnected {
		call.feed = feed
	} else {
		m.mu.Unlock()
		feed.Stop()
		return fmt.Errorf("voice: play chat %d: call ended while the feed was starting", chatID)
	}
	m.mu.Unlock()
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
