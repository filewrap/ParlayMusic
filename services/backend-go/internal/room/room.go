// Package room is the per-chat playback state machine of the unified
// Go backend (phase3/actions-inventory.md §4). One Room per chat,
// mutex-guarded; all mutations go through Manager methods, never
// direct field access (vc-go room.go pattern). Room state is the
// single source of truth for now_playing / queue / progress.
//
// States (inventory §4.1):
//
//	Idle ──vc.play (idle)──▶ Playing ──vc.pause──▶ Paused ──vc.resume──▶ Playing
//	  ▲                        │  │
//	  │                        │  └─vc.skip/advance
//	  └──── vc.stop / advance-to-empty / autoplay-exhausted ──┘
//
// Advance order (§4.3): loop replay → queue pop (shuffle-aware) →
// autoplay pick → idle. Position is wall-clock derived (§4.4);
// the Phase 5 DSP replaces it with a sample-accurate clock.
//
// The room is pure state: it fires NO events and touches NO voice
// calls. It returns outcomes (AdvanceOutcome, PlayOutcome) and the
// caller — internal/handlers — drives the voice Manager and fires
// the §4.5 events from those outcomes. That split is what keeps
// events firing exactly once per transition.
//
// Persistence (§4.6): every mutation writes the room snapshot
// (store.PutRoom) and the waiting queue (store.PutQueue) in bbolt.
// Restored rooms come back paused, position frozen at capture
// time. The autoplay picker is a function, not data, installed
// with SetPicker; it runs OUTSIDE the room lock (network I/O) and
// its pick is applied only if the room is still in the same drained
// state.
//
// There is NO advance ticker here. The inventory §4.3 interim
// 1-second ticker is deliberately NOT ported: the Wave 2 task
// orders event-driven advance only (voice feeder EOF via
// Manager.OnFeedEnd), no polling, no watcher. A track with no feed
// therefore never auto-advances — see the Wave 2 report.
package room

import (
	"context"
	"errors"
	"fmt"
	"math/rand/v2"
	"sync"
	"time"

	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
)

// MaxAutoplayHistory is the autoplay history cap (inventory §4.3,
// §10-D11: 50, vc-go/vc-ts rule — vc-rust's 200-with-shift is NOT
// ported). When the history reaches the cap at advance time it is
// cleared and the room goes idle.
const MaxAutoplayHistory = 50

// Errors the Manager returns. Handlers map them to contract error
// codes: ErrNothingPlaying -> not_found, ErrPositionOutOfRange /
// ErrQueueFull / ErrInvalid -> bad_request, ErrNoPick /
// ErrPickerFailed -> engine_unavailable.
var (
	// ErrNothingPlaying means the chat has no current track (and,
	// where the action needs one, no queue either): the shared
	// "nothing playing in this chat" 404 (inventory §0, §9-D5).
	ErrNothingPlaying = errors.New("nothing playing in this chat")
	// ErrPositionOutOfRange means a seek target is negative.
	ErrPositionOutOfRange = errors.New("position_sec must be >= 0")
	// ErrQueueFull means the waiting queue is at the configured
	// max_queue_size (parlay.toml [limits], not a hardcoded cap).
	ErrQueueFull = errors.New("queue is full")
	// ErrInvalid means a caller passed an invalid argument.
	ErrInvalid = errors.New("invalid argument")
	// ErrNoPick means the autoplay picker found no candidate
	// (mix empty or every candidate excluded).
	ErrNoPick = errors.New("no autoplay recommendation")
	// ErrPickerFailed means the autoplay fetch itself failed.
	ErrPickerFailed = errors.New("autoplay pick failed")
	// ErrStale means the room changed while a pick was in flight
	// (a /play or /stop won); the pick is discarded.
	ErrStale = errors.New("room changed while picking")
)

// Picker picks the next autoplay track for last, excluding every id
// in exclude (last + queue + history). It returns the zero Track
// with a nil error when the mix is empty or every candidate is
// excluded (ErrNoPick is the caller's reading of that), and a
// non-nil error when the fetch itself failed.
type Picker func(ctx context.Context, last model.Track, exclude map[string]bool) (model.Track, error)

// AdvanceKind is what Advance did.
type AdvanceKind int

const (
	// AdvanceNoOp means nothing advanced (stale end-of-track, or
	// nothing to advance to).
	AdvanceNoOp AdvanceKind = iota
	// AdvanceReplayed means the current track replayed (loop > 0
	// consumed one replay).
	AdvanceReplayed
	// AdvanceNext means the next waiting track started (queue pop).
	AdvanceNext
	// AdvanceAutoplay means an autoplay pick started.
	AdvanceAutoplay
	// AdvanceIdle means the room went idle (queue empty, autoplay
	// off/exhausted/failed).
	AdvanceIdle
)

// AdvanceOutcome describes one Advance call.
type AdvanceOutcome struct {
	Kind AdvanceKind
	// Track is the new current track, nil when the room went idle
	// or nothing advanced.
	Track *model.Track
	// PreviousTrackID is the id of the track that was current
	// before a queue pop, "" otherwise.
	PreviousTrackID string
	// EndedTrack is the track that ended (the previous current),
	// nil when Advance ran with no current track.
	EndedTrack *model.Track
	// Reason is the idle reason for AdvanceIdle: "queue_empty" |
	// "autoplay_failed" | "skipped" (inventory §4.5).
	Reason string
}

// PlayOutcome describes one Play call.
type PlayOutcome struct {
	// Started is true when the track started playing immediately
	// (the room was idle); false when it was appended to the
	// waiting queue.
	Started bool
	// Track is the track that was played or queued.
	Track model.Track
}

// roomState is one chat's room: the persisted state plus the
// runtime position-clock anchor (startedAt is zero when paused or
// idle, so it is never persisted — the stored PositionSec is the
// frozen offset at capture time).
type roomState struct {
	st        model.RoomState
	startedAt time.Time
}

// Manager owns every chat's room.
type Manager struct {
	store    *store.Store
	picker   Picker
	maxQueue int

	mu    sync.Mutex
	rooms map[int64]*roomState
}

// NewManager builds a Manager. maxQueue is the configured
// max_queue_size; <= 0 means unlimited (defensive — config
// validation normally guarantees >= 1).
func NewManager(st *store.Store, maxQueue int) *Manager {
	return &Manager{
		store:    st,
		maxQueue: maxQueue,
		rooms:    map[int64]*roomState{},
	}
}

// SetPicker installs the autoplay picker (a function, not data —
// inventory §4.6). A nil picker means autoplay picks never happen
// and a drained queue always goes idle.
func (m *Manager) SetPicker(p Picker) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.picker = p
}

// Restore loads every stored room back into memory. Restored rooms
// come back paused (all three engines agree; never auto-resume
// audio after a restart) with the position frozen at capture time.
func (m *Manager) Restore() error {
	ids, err := m.store.ListRoomIDs()
	if err != nil {
		return fmt.Errorf("room: restore: list rooms: %w", err)
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, id := range ids {
		st, found, err := m.store.GetRoom(id)
		if err != nil {
			return fmt.Errorf("room: restore chat %d: %w", id, err)
		}
		if !found {
			continue
		}
		queue, err := m.store.GetQueue(id)
		if err != nil {
			return fmt.Errorf("room: restore chat %d queue: %w", id, err)
		}
		st.Queue = queue
		st.Paused = true // never auto-resume after a restart
		m.rooms[id] = &roomState{st: st}
	}
	return nil
}

// --- position clock (§4.4) ------------------------------------------

// positionLocked is the wall-clock position in seconds: the frozen
// offset when paused/idle, else offset + elapsed since start.
func positionLocked(rs *roomState) int64 {
	if rs.st.Paused || rs.st.Current == nil || rs.startedAt.IsZero() {
		return rs.st.PositionSec
	}
	return rs.st.PositionSec + int64(time.Since(rs.startedAt)/time.Second)
}

// persistLocked writes the room snapshot and the waiting queue.
// The stored PositionSec is the position frozen at capture time;
// the in-memory offset is left untouched so a playing room's clock
// keeps running after the write.
func (m *Manager) persistLocked(rs *roomState) error {
	st := rs.st
	st.PositionSec = positionLocked(rs)
	st.UpdatedAt = time.Now().Unix()
	if err := m.store.PutRoom(st); err != nil {
		return fmt.Errorf("room: persist chat %d: %w", st.ChatID, err)
	}
	if err := m.store.PutQueue(st.ChatID, st.Queue); err != nil {
		return fmt.Errorf("room: persist chat %d queue: %w", st.ChatID, err)
	}
	return nil
}

// snapshotLocked builds the NowPlaying view over a room. Queue is
// never nil on the wire (an empty queue serialises as []).
func snapshotLocked(rs *roomState) model.NowPlaying {
	queue := make([]model.QueueItem, len(rs.st.Queue))
	for i, item := range rs.st.Queue {
		item.Position = i
		queue[i] = item
	}
	np := model.NowPlaying{
		ChatID:          rs.st.ChatID,
		Engine:          "backend-go",
		Loop:            rs.st.Loop,
		LoopCount:       rs.st.Loop,
		Paused:          rs.st.Paused,
		IsPaused:        rs.st.Paused,
		IsMuted:         rs.st.Muted,
		PositionSec:     positionLocked(rs),
		Queue:           queue,
		Shuffle:         rs.st.Shuffle,
		Autoplay:        rs.st.Autoplay,
		AutoplayEnabled: rs.st.Autoplay,
		Volume:          rs.st.Volume,
		Speed:           rs.st.Speed,
	}
	if rs.st.Current != nil {
		track := *rs.st.Current
		np.Track = &track
		np.StreamURL = track.StreamURL
	}
	return np
}

// getOrCreateLocked returns the room for a chat, creating a default
// one when the chat has none yet. Callers must hold m.mu.
func (m *Manager) getOrCreateLocked(chatID int64) *roomState {
	rs, ok := m.rooms[chatID]
	if !ok {
		rs = &roomState{st: model.DefaultRoomState(chatID)}
		m.rooms[chatID] = rs
	}
	return rs
}

// --- read-only views -------------------------------------------------

// Snapshot returns the NowPlaying view for a chat, or false when
// the chat has no room. Read-only: it never creates a room
// (inventory §1.13).
func (m *Manager) Snapshot(chatID int64) (model.NowPlaying, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return model.NowPlaying{}, false
	}
	return snapshotLocked(rs), true
}

// Queue returns the waiting queue for a chat in play order
// (0-based positions, the current track is never in the list), or
// false when the chat has no room.
func (m *Manager) Queue(chatID int64) ([]model.QueueItem, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return nil, false
	}
	out := make([]model.QueueItem, len(rs.st.Queue))
	for i, item := range rs.st.Queue {
		item.Position = i
		out[i] = item
	}
	return out, true
}

// Current returns a copy of the current track, or false when the
// room has none.
func (m *Manager) Current(chatID int64) (model.Track, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.Track{}, false
	}
	return *rs.st.Current, true
}

// FX returns the stored FX preset + parameters for a chat, or false
// when the chat has no room. The returned params are a copy: the
// caller's map never aliases the room's. This is the read side of
// the seam the Phase 5 DSP uses at stream time.
func (m *Manager) FX(chatID int64) (string, map[string]float64, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return "", nil, false
	}
	params := make(map[string]float64, len(rs.st.FXParams))
	for k, v := range rs.st.FXParams {
		params[k] = v
	}
	return rs.st.FXPreset, params, true
}

// DSPOptions returns the DSP parameters for a chat — speed, bass,
// volume, FX preset + parameters — as one dsp.Options, read from
// the room in a single lock hold so the set can never straddle a
// room mutation (a speed change landing between the FX read and
// the speed read would start the track under a torn set). A chat
// with no room gets the neutral defaults. This is the seam the
// voice path uses at bind time: handlers build the options once
// per bind and hand them to Manager.Play/PlayStream, which pass
// them to the DSP from the first frame.
func (m *Manager) DSPOptions(chatID int64) dsp.Options {
	m.mu.Lock()
	defer m.mu.Unlock()
	opts := dsp.DefaultOptions()
	rs, ok := m.rooms[chatID]
	if !ok {
		return opts
	}
	opts.Speed = rs.st.Speed
	opts.Bass = rs.st.Bass
	opts.Volume = rs.st.Volume
	opts.Effect = rs.st.FXPreset
	if len(rs.st.FXParams) > 0 {
		params := make(map[string]float64, len(rs.st.FXParams))
		for k, v := range rs.st.FXParams {
			params[k] = v
		}
		opts.EffectParams = params
	}
	return opts
}

// HasRoom reports whether the chat has a room at all.
func (m *Manager) HasRoom(chatID int64) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	_, ok := m.rooms[chatID]
	return ok
}

// --- queue ops (§4.2) ------------------------------------------------

// Play plays a track now when the room is idle, else appends it to
// the waiting queue (YukkiMusic rule, all three engines).
func (m *Manager) Play(chatID int64, track model.Track, by int64) (PlayOutcome, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	if rs.st.Current == nil {
		m.startTrackLocked(rs, track)
		if err := m.persistLocked(rs); err != nil {
			return PlayOutcome{}, err
		}
		return PlayOutcome{Started: true, Track: track}, nil
	}
	if err := m.enqueueLocked(rs, track, by); err != nil {
		return PlayOutcome{}, err
	}
	if err := m.persistLocked(rs); err != nil {
		return PlayOutcome{}, err
	}
	return PlayOutcome{Started: false, Track: track}, nil
}

// Enqueue always appends a track, never starts playback (contrast
// Play — the core's /restore and /loop copy-queueing rely on this).
func (m *Manager) Enqueue(chatID int64, track model.Track, by int64) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	if err := m.enqueueLocked(rs, track, by); err != nil {
		return err
	}
	return m.persistLocked(rs)
}

func (m *Manager) enqueueLocked(rs *roomState, track model.Track, by int64) error {
	if m.maxQueue > 0 && len(rs.st.Queue) >= m.maxQueue {
		return fmt.Errorf("%w (max %d tracks)", ErrQueueFull, m.maxQueue)
	}
	rs.st.Queue = append(rs.st.Queue, model.QueueItem{
		Position:    len(rs.st.Queue),
		RequestedBy: by,
		Track:       track,
	})
	return nil
}

// Remove deletes exactly one waiting item by 0-based index; removing
// the current track is impossible through this action. Out of range
// is an error and mutates nothing.
func (m *Manager) Remove(chatID int64, pos int) ([]model.QueueItem, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return nil, fmt.Errorf("%w: position %d out of range (empty queue)", ErrInvalid, pos)
	}
	if pos < 0 || pos >= len(rs.st.Queue) {
		return nil, fmt.Errorf("%w: position %d out of range (queue has %d items)", ErrInvalid, pos, len(rs.st.Queue))
	}
	rs.st.Queue = append(rs.st.Queue[:pos], rs.st.Queue[pos+1:]...)
	if err := m.persistLocked(rs); err != nil {
		return nil, err
	}
	return m.queueCopyLocked(rs), nil
}

// ClearQueue empties the waiting queue only; the current track keeps
// playing.
func (m *Manager) ClearQueue(chatID int64) ([]model.QueueItem, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return []model.QueueItem{}, nil
	}
	rs.st.Queue = nil
	if err := m.persistLocked(rs); err != nil {
		return nil, err
	}
	return []model.QueueItem{}, nil
}

func (m *Manager) queueCopyLocked(rs *roomState) []model.QueueItem {
	out := make([]model.QueueItem, len(rs.st.Queue))
	for i, item := range rs.st.Queue {
		item.Position = i
		out[i] = item
	}
	return out
}

// --- track lifecycle -------------------------------------------------

// startTrackLocked makes t the current track: position 0, clock
// running, not paused. It notes the track in the autoplay history
// (append if new, cap 50) and clears the radio state unless t IS
// the stored radio track.
func (m *Manager) startTrackLocked(rs *roomState, t model.Track) {
	track := t
	rs.st.Current = &track
	rs.st.PositionSec = 0
	rs.startedAt = time.Now()
	rs.st.Paused = false
	rs.st.LastTrackID = t.ID
	if rs.st.Radio == nil || rs.st.Radio.NowPlaying == nil || rs.st.Radio.NowPlaying.ID != t.ID {
		rs.st.Radio = nil
	}
	m.noteHistoryLocked(rs, t.ID)
}

// noteHistoryLocked appends id to the autoplay history when new,
// keeping at most MaxAutoplayHistory entries (oldest dropped).
func (m *Manager) noteHistoryLocked(rs *roomState, id string) {
	for _, h := range rs.st.AutoplayHistory {
		if h == id {
			return
		}
	}
	rs.st.AutoplayHistory = append(rs.st.AutoplayHistory, id)
	if len(rs.st.AutoplayHistory) > MaxAutoplayHistory {
		rs.st.AutoplayHistory = rs.st.AutoplayHistory[len(rs.st.AutoplayHistory)-MaxAutoplayHistory:]
	}
}

// popNextLocked pops the next waiting track: index 0, or a random
// index when shuffle is on (uniform over waiting; vc-go rand.Intn
// rule). The current track is never touched by shuffle.
func (m *Manager) popNextLocked(rs *roomState) (model.Track, bool) {
	if len(rs.st.Queue) == 0 {
		return model.Track{}, false
	}
	idx := 0
	if rs.st.Shuffle {
		idx = rand.IntN(len(rs.st.Queue))
	}
	item := rs.st.Queue[idx]
	rs.st.Queue = append(rs.st.Queue[:idx], rs.st.Queue[idx+1:]...)
	return item.Track, true
}

// goIdleLocked clears the current track and freezes the clock. The
// queue is empty by construction at every call site; shuffle,
// volume/mute, the autoplay flag and the history are kept.
func (m *Manager) goIdleLocked(rs *roomState) {
	rs.st.Current = nil
	rs.st.PositionSec = 0
	rs.startedAt = time.Time{}
	rs.st.Paused = false
	rs.st.Radio = nil
}

// isDefaultLocked reports whether a room carries no state worth
// keeping in the store: no current, no queue, and no non-default
// settings (inventory §1.5 / §4.6).
func isDefaultLocked(rs *roomState) bool {
	st := rs.st
	return st.Current == nil &&
		len(st.Queue) == 0 &&
		st.Loop == 0 &&
		!st.Shuffle &&
		!st.Autoplay &&
		len(st.AutoplayHistory) == 0 &&
		st.Volume == 100 &&
		st.Speed == 1.0 &&
		!st.Muted &&
		st.MutedVolume == nil &&
		st.Bass == 0 &&
		st.Radio == nil &&
		st.FXPreset == "" &&
		len(st.FXParams) == 0
}

// Advance moves the room to the next track (loop → queue → autoplay
// → idle, §4.3). endedID is the id of the track that just ended, or
// "" to advance unconditionally (vc.skip semantics). A non-empty
// endedID that does not match the current track means the end
// event is stale (the user already skipped or stopped) and Advance
// is a no-op. fromSkip selects the idle reason "skipped" instead
// of "queue_empty".
func (m *Manager) Advance(ctx context.Context, chatID int64, endedID string, fromSkip bool) (AdvanceOutcome, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}
	if endedID != "" && (rs.st.Current == nil || rs.st.Current.ID != endedID) {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}
	if rs.st.Current == nil && len(rs.st.Queue) == 0 {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}
	var ended *model.Track
	if rs.st.Current != nil {
		track := *rs.st.Current
		ended = &track
	}

	// 1. Loop: replay the current track, consuming one replay.
	if rs.st.Current != nil && rs.st.Loop > 0 {
		rs.st.Loop--
		m.startTrackLocked(rs, *rs.st.Current)
		track := *rs.st.Current
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceReplayed, Track: &track, EndedTrack: ended}, nil
	}

	// 2. Queue pop (shuffle-aware).
	if next, ok := m.popNextLocked(rs); ok {
		prevID := ""
		if ended != nil {
			prevID = ended.ID
		}
		m.startTrackLocked(rs, next)
		track := *rs.st.Current
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNext, Track: &track, PreviousTrackID: prevID, EndedTrack: ended}, nil
	}

	// 3. Autoplay pick. The picker runs OUTSIDE the lock (network
	// I/O); the pick is applied only if the room is still in the
	// same drained state.
	if rs.st.Autoplay && m.picker != nil {
		if len(rs.st.AutoplayHistory) >= MaxAutoplayHistory {
			rs.st.AutoplayHistory = nil
			m.goIdleLocked(rs)
			if err := m.persistLocked(rs); err != nil {
				m.mu.Unlock()
				return AdvanceOutcome{}, err
			}
			m.mu.Unlock()
			return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: ended, Reason: "queue_empty"}, nil
		}
		last := *rs.st.Current
		exclude := map[string]bool{last.ID: true}
		for _, id := range rs.st.AutoplayHistory {
			exclude[id] = true
		}
		for _, item := range rs.st.Queue {
			exclude[item.Track.ID] = true
		}
		picker := m.picker
		m.mu.Unlock()
		pick, pickErr := picker(ctx, last, exclude)
		m.mu.Lock()
		rs, ok = m.rooms[chatID]
		if !ok || rs.st.Current == nil || rs.st.Current.ID != last.ID || len(rs.st.Queue) != 0 {
			// A /play or /stop won while the pick was in flight.
			m.mu.Unlock()
			return AdvanceOutcome{Kind: AdvanceNoOp}, nil
		}
		if pickErr != nil || pick.ID == "" {
			m.goIdleLocked(rs)
			if err := m.persistLocked(rs); err != nil {
				m.mu.Unlock()
				return AdvanceOutcome{}, err
			}
			m.mu.Unlock()
			return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: ended, Reason: "autoplay_failed"}, nil
		}
		m.startTrackLocked(rs, pick)
		track := *rs.st.Current
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceAutoplay, Track: &track, EndedTrack: ended}, nil
	}

	// 4. Idle.
	m.goIdleLocked(rs)
	if err := m.persistLocked(rs); err != nil {
		m.mu.Unlock()
		return AdvanceOutcome{}, err
	}
	m.mu.Unlock()
	reason := "queue_empty"
	if fromSkip {
		reason = "skipped"
	}
	return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: ended, Reason: reason}, nil
}

// Skip resets the loop counter to 0 (all three engines) and then
// advances. A room with an empty current but a waiting queue starts
// the next track (vc-rust rule, §1.4); a fully empty room is
// ErrNothingPlaying.
func (m *Manager) Skip(ctx context.Context, chatID int64) (AdvanceOutcome, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok || (rs.st.Current == nil && len(rs.st.Queue) == 0) {
		m.mu.Unlock()
		return AdvanceOutcome{}, ErrNothingPlaying
	}
	rs.st.Loop = 0
	m.mu.Unlock()
	return m.Advance(ctx, chatID, "", true)
}

// AutoplaySkip is NOT a skip (§1.11): it is the gated pick. It
// requires autoplay on (else ErrInvalid) and a current track (else
// ErrNothingPlaying). The pick excludes last + queue + history, the
// loop resets to 0, and the picked track starts immediately. It
// returns the picked track and the track it replaced.
func (m *Manager) AutoplaySkip(ctx context.Context, chatID int64) (model.Track, model.Track, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		m.mu.Unlock()
		return model.Track{}, model.Track{}, ErrNothingPlaying
	}
	if !rs.st.Autoplay {
		m.mu.Unlock()
		return model.Track{}, model.Track{}, fmt.Errorf("%w: autoplay is off in this chat", ErrInvalid)
	}
	if m.picker == nil {
		m.mu.Unlock()
		return model.Track{}, model.Track{}, fmt.Errorf("%w: no autoplay picker configured", ErrPickerFailed)
	}
	last := *rs.st.Current
	exclude := map[string]bool{last.ID: true}
	for _, id := range rs.st.AutoplayHistory {
		exclude[id] = true
	}
	for _, item := range rs.st.Queue {
		exclude[item.Track.ID] = true
	}
	picker := m.picker
	m.mu.Unlock()
	pick, err := picker(ctx, last, exclude)
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok = m.rooms[chatID]
	if !ok || rs.st.Current == nil || rs.st.Current.ID != last.ID || !rs.st.Autoplay {
		return model.Track{}, model.Track{}, ErrStale
	}
	if err != nil {
		return model.Track{}, model.Track{}, fmt.Errorf("%w: %v", ErrPickerFailed, err)
	}
	if pick.ID == "" {
		return model.Track{}, model.Track{}, ErrNoPick
	}
	rs.st.Loop = 0
	m.startTrackLocked(rs, pick)
	if err := m.persistLocked(rs); err != nil {
		return model.Track{}, model.Track{}, err
	}
	return pick, last, nil
}

// Stop clears the current track and the whole queue; loop and
// autoplay reset to off; autoplay history is cleared; paused is
// cleared. Shuffle is KEPT (persistent setting, all three
// engines); volume/mute are kept. Always succeeds, even with no
// room (idempotent stop, §1.5). A room left with no state worth
// keeping is dropped from the store.
func (m *Manager) Stop(chatID int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return model.NowPlaying{
			ChatID: chatID, Engine: "backend-go",
			Queue: []model.QueueItem{}, Volume: 100, Speed: 1.0,
		}, nil
	}
	rs.st.Current = nil
	rs.st.Queue = nil
	rs.st.Loop = 0
	rs.st.Autoplay = false
	rs.st.AutoplayHistory = nil
	rs.st.Paused = false
	rs.st.PositionSec = 0
	rs.startedAt = time.Time{}
	rs.st.Radio = nil
	if isDefaultLocked(rs) {
		delete(m.rooms, chatID)
		if err := m.store.DeleteRoom(chatID); err != nil {
			return model.NowPlaying{}, err
		}
		return model.NowPlaying{
			ChatID: chatID, Engine: "backend-go",
			Queue: []model.QueueItem{}, Volume: 100, Speed: 1.0,
		}, nil
	}
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// ClearCurrent drops the current track without touching the queue
// (GoIdle semantics). It is the rollback for a vc.play fresh start
// whose voice bind failed: the room returns to idle with the queue
// intact, as if the track had never started.
func (m *Manager) ClearCurrent(chatID int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	m.goIdleLocked(rs)
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// Pause freezes the position clock (position_sec frozen, startedAt
// cleared). Idempotent when already paused; idle is
// ErrNothingPlaying.
func (m *Manager) Pause(chatID int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if !rs.st.Paused {
		rs.st.PositionSec = positionLocked(rs)
		rs.startedAt = time.Time{}
		rs.st.Paused = true
		if err := m.persistLocked(rs); err != nil {
			return model.NowPlaying{}, err
		}
	}
	return snapshotLocked(rs), nil
}

// Resume restarts the clock from the frozen offset. No-op when not
// paused; idle is ErrNothingPlaying.
func (m *Manager) Resume(chatID int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if rs.st.Paused {
		rs.startedAt = time.Now()
		rs.st.Paused = false
		if err := m.persistLocked(rs); err != nil {
			return model.NowPlaying{}, err
		}
	}
	return snapshotLocked(rs), nil
}

// SeekTo moves the position clock, clamped to [0, duration] when
// the duration is known; unknown duration (0) accepts any >= 0. A
// negative target is ErrPositionOutOfRange; idle is
// ErrNothingPlaying. When playing, startedAt resets to now at the
// new offset; when paused, only the frozen offset changes.
func (m *Manager) SeekTo(chatID int64, pos int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if pos < 0 {
		return model.NowPlaying{}, ErrPositionOutOfRange
	}
	if rs.st.Current.DurationSec > 0 && pos > rs.st.Current.DurationSec {
		pos = rs.st.Current.DurationSec
	}
	rs.st.PositionSec = pos
	if !rs.st.Paused {
		rs.startedAt = time.Now()
	}
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetLoop sets the remaining replays of the current track (0-10).
// Setting the count does not restart the track; the count is
// consumed one per replay at the natural track end. Idle is
// ErrNothingPlaying.
func (m *Manager) SetLoop(chatID int64, count int) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	rs.st.Loop = count
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetShuffle enables/disables shuffle. Enabling shuffles the
// waiting queue in place (Fisher-Yates, vc-ts/vc-rust rule) and
// sets the flag; disabling clears the flag only and never
// reorders. Enabling with < 2 waiting tracks is a no-op reorder
// that still sets the flag. A chat with no current track and an
// empty queue is ErrNothingPlaying.
func (m *Manager) SetShuffle(chatID int64, enabled bool) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || (rs.st.Current == nil && len(rs.st.Queue) == 0) {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if enabled && len(rs.st.Queue) > 1 {
		rand.Shuffle(len(rs.st.Queue), func(i, j int) {
			rs.st.Queue[i], rs.st.Queue[j] = rs.st.Queue[j], rs.st.Queue[i]
		})
	}
	rs.st.Shuffle = enabled
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetAutoplay sets the autoplay flag. Disabling clears the history
// (vc-go/vc-ts rule). A chat with no current track and an empty
// queue is ErrNothingPlaying.
func (m *Manager) SetAutoplay(chatID int64, enabled bool) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || (rs.st.Current == nil && len(rs.st.Queue) == 0) {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	rs.st.Autoplay = enabled
	if !enabled {
		rs.st.AutoplayHistory = nil
	}
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetVolume sets the room volume, clamped to 0-200 (vc-go clamp
// rule wins over vc-ts's 400, §10-D15). Mute is separate: setting
// volume 0 does NOT set is_muted. A chat with no current track and
// an empty queue is ErrNothingPlaying.
func (m *Manager) SetVolume(chatID int64, volume int) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || (rs.st.Current == nil && len(rs.st.Queue) == 0) {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if volume < 0 {
		volume = 0
	}
	if volume > 200 {
		volume = 200
	}
	rs.st.Volume = volume
	if rs.st.Muted {
		// Changing the volume while muted moves the restore
		// point: unmuting restores the latest intent, not the
		// pre-mute value.
		v := volume
		rs.st.MutedVolume = &v
	}
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetMuted sets the real mute flag, orthogonal to volume: muting
// stores the current volume in muted_volume and keeps volume
// unchanged; unmuting clears muted_volume. Idle is
// ErrNothingPlaying.
func (m *Manager) SetMuted(chatID int64, muted bool) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Current == nil {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if muted && !rs.st.Muted {
		v := rs.st.Volume
		rs.st.MutedVolume = &v
	}
	if !muted {
		rs.st.MutedVolume = nil
	}
	rs.st.Muted = muted
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetSpeed stores the playback speed (0.5-4.0, default 1.0). The
// Phase 5 DSP applies it; Phase 3 stores and reports it only
// (inventory §4.4).
func (m *Manager) SetSpeed(chatID int64, speed float64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	rs.st.Speed = speed
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetBass stores the bass level (0-100, default 0) for the Phase 5
// DSP (inventory §4.4).
func (m *Manager) SetBass(chatID int64, bass int) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	rs.st.Bass = bass
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetFX stores the FX preset + parameters for a chat (fx.apply,
// record-only in Phase 3; the Phase 5 DSP reads them at stream
// time via FX). Like SetSpeed/SetBass it creates the room when the
// chat has none — fx.apply on a chat with no room still records
// the preset. The write goes through persistLocked, so the
// in-memory room and the bbolt record can never diverge: this is
// the seam fx must use instead of writing the store directly (a
// direct store write is invisible to the Manager, and the next
// Manager mutation persists the in-memory copy and clobbers the
// FX fields — the same staleness shape the vote/gui seams had
// before Wave 2 rewired them).
func (m *Manager) SetFX(chatID int64, effect string, params map[string]float64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	rs.st.FXPreset = effect
	cp := make(map[string]float64, len(params))
	for k, v := range params {
		cp[k] = v
	}
	rs.st.FXParams = cp
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// StartRadio starts a radio track as the current track and records
// the radio state, both in the room and in the store's radio
// bucket (so radio.status reads the same state). The radio track
// has duration 0 and never auto-advances.
func (m *Manager) StartRadio(chatID int64, radio model.RadioState, track model.Track, by int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	radio.NowPlaying = &track
	m.startTrackLocked(rs, track)
	// startTrackLocked clears Radio unless the track IS the stored
	// radio track; set it after the start so the rule never has
	// to guess.
	rs.st.Radio = &radio
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	if err := m.store.PutRadio(radio); err != nil {
		return model.NowPlaying{}, fmt.Errorf("room: persist radio state chat %d: %w", chatID, err)
	}
	return snapshotLocked(rs), nil
}

// StopRadio clears the radio state. It stops the room's radio
// track only when the room is still playing that radio track — a
// normal music track that replaced it is NOT stopped. It returns
// whether the current track was the radio track (the caller leaves
// the voice call only then and only when the queue is empty), the
// snapshot, and ErrNothingPlaying when the chat has no radio.
func (m *Manager) StopRadio(chatID int64) (bool, model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || rs.st.Radio == nil {
		if err := m.store.DeleteRadio(chatID); err != nil {
			return false, model.NowPlaying{}, err
		}
		return false, model.NowPlaying{}, ErrNothingPlaying
	}
	wasRadio := rs.st.Current != nil && rs.st.Radio.NowPlaying != nil &&
		rs.st.Current.ID == rs.st.Radio.NowPlaying.ID
	rs.st.Radio = nil
	if wasRadio {
		m.goIdleLocked(rs)
	}
	if err := m.store.DeleteRadio(chatID); err != nil {
		return false, model.NowPlaying{}, err
	}
	if err := m.persistLocked(rs); err != nil {
		return false, model.NowPlaying{}, err
	}
	return wasRadio, snapshotLocked(rs), nil
}

// IsRadioCurrent reports whether the room's current track is its
// stored radio track.
func (m *Manager) IsRadioCurrent(chatID int64) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	return ok && rs.st.Radio != nil && rs.st.Current != nil &&
		rs.st.Radio.NowPlaying != nil && rs.st.Current.ID == rs.st.Radio.NowPlaying.ID
}
