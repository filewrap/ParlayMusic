// Package room is the per-chat playback state machine of the unified
// Go backend (phase3/actions-inventory.md §4). One Room per chat,
// mutex-guarded; all mutations go through Manager methods, never
// direct field access (vc-go room.go pattern). Room state is the
// single source of truth for now_playing / queue / progress.
//
// Queue model (queue-port Tier 1, 2026-11-05, item 10): the room
// holds ONE ordered list, Queue, whose index 0 is the playing
// track when Playing is true. Advance is a pop. There is no
// separate current pointer that can go stale — the ghost track (a
// current with no live feed, every /play queueing behind it
// forever) is unrepresentable. When Playing is false the list is
// only waiting tracks (vc.queue add on an idle room, a room
// restored with no live feed); the front track is next-up, and
// /skip activates it. The wire snapshot still reports the playing
// track as Track and the waiting tracks as Queue, so the UDS
// contract does not change shape.
//
// States (inventory §4.1):
//
//	Idle ──vc.play (idle)──▶ Playing ──vc.pause──▶ Paused ──vc.resume──▶ Playing
//	  ▲                        │  │
//	  │                        │  └─vc.skip/advance
//	  └──── vc.stop / advance-to-empty / autoplay-exhausted ──┘
//
// A room can also be queued-only: a non-empty list with Playing
// false (vc.queue add on an idle room, a restore with no live
// feed). Nothing is active there; /play jumps a track to the front
// and starts it, /skip activates the front track.
//
// Advance order (§4.3): loop replay → queue pop (shuffle-aware) →
// autoplay pick → idle. Position is wall-clock derived (§4.4),
// scaled by the room's playback speed; the Phase 5 DSP replaces
// it with a sample-accurate clock.
//
// The room is pure state: it fires NO events and touches NO voice
// calls. It returns outcomes (AdvanceOutcome, PlayOutcome) and the
// caller — internal/handlers — drives the voice Manager and fires
// the §4.5 events from those outcomes. That split is what keeps
// events firing exactly once per transition.
//
// Persistence (§4.6): every mutation writes the room snapshot
// (store.PutRoom) and the whole ordered list (store.PutQueue) in
// bbolt. Restored rooms come back paused, position frozen at
// capture time — a room that was playing comes back as a paused
// track at the front, never as a playing one. The autoplay picker
// is a function, not data, installed with SetPicker; it runs
// OUTSIDE the room lock (network I/O) and its pick is applied only
// if the room is still in the same drained state.
//
// There is NO advance ticker here. The queue watchdog (handlers,
// queue-port Tier 1 item 2) polls the room snapshots from outside
// the lock and advances a room whose wall-clock position has
// passed its duration for three consecutive ticks; the stream-end
// callback (handlers, item 1) is the other trigger. The room
// itself stays pure state, and the Wave 2 rule still holds: no
// polling inside this package.
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
	// ErrQueueFull means a waiting queue is at the configured
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
	// AdvanceReplayed means the front track replayed (loop > 0
	// consumed one replay).
	AdvanceReplayed
	// AdvanceNext means the next waiting track started (front pop,
	// or the front track of a queued-only room activated).
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
	// Track is the new front track, nil when the room went idle
	// or nothing advanced.
	Track *model.Track
	// PreviousTrackID is the id of the track that was at the front
	// before a pop, "" otherwise.
	PreviousTrackID string
	// EndedTrack is the track that ended (the previous front
	// track), nil when Advance ran with no active track.
	EndedTrack *model.Track
	// Reason is the idle reason for AdvanceIdle: "queue_empty" |
	// "autoplay_failed" | "skipped" (inventory §4.5).
	Reason string
}

// PlayOutcome describes one Play call.
type PlayOutcome struct {
	// Started is true when the track started playing immediately
	// (the room had no active track); false when it was appended
	// to the waiting list.
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
// never auto-resume audio after a restart: a room that was playing
// comes back as a paused track at the front (resumable via
// /resume, startable via /skip), and a queued-only room stays
// queued-only. A legacy pre-restructure record (playing track in
// `current`, waiting tracks in the queue bucket) is migrated:
// the current track becomes the front of the list as a paused
// track — it had no live feed, so restoring it as playing would
// be the ghost — and the store is converged to the new shape
// immediately.
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
		migrated := false
		if st.Current != nil {
			item := model.QueueItem{Position: 0, RequestedBy: 0, Track: *st.Current}
			if len(st.Queue) == 0 || st.Queue[0].Track.ID != st.Current.ID {
				st.Queue = append([]model.QueueItem{item}, st.Queue...)
			}
			st.LastTrackID = st.Current.ID
			st.Current = nil
			st.Playing = true
			st.Paused = true
			migrated = true
		}
		if st.Playing && !st.Paused {
			// Was playing at shutdown: come back paused, never
			// auto-resume with no feed behind the claim.
			st.Paused = true
		}
		rs := &roomState{st: st}
		m.rooms[id] = rs
		if migrated {
			if err := m.persistLocked(rs); err != nil {
				return fmt.Errorf("room: restore chat %d: persist migrated: %w", id, err)
			}
		}
	}
	return nil
}

// --- position clock (§4.4) ------------------------------------------

// positionLocked is the wall-clock position in seconds: the frozen
// offset when paused/idle, else offset + elapsed since start,
// scaled by the room's playback speed (the audio plays faster
// than realtime at speed > 1, so the position must too).
func positionLocked(rs *roomState) int64 {
	if !rs.st.Playing || rs.st.Paused || rs.startedAt.IsZero() {
		return rs.st.PositionSec
	}
	speed := rs.st.Speed
	if speed <= 0 {
		speed = 1.0
	}
	return rs.st.PositionSec + int64(time.Since(rs.startedAt).Seconds()*speed)
}

// persistLocked writes the room snapshot and the whole ordered
// list. The stored PositionSec is the position frozen at capture
// time; the in-memory offset is left untouched so a playing room's
// clock keeps running after the write. The legacy Current field
// is never written again: the stored shape is the one-list model.
func (m *Manager) persistLocked(rs *roomState) error {
	st := rs.st
	st.PositionSec = positionLocked(rs)
	st.UpdatedAt = time.Now().Unix()
	st.Current = nil
	if err := m.store.PutRoom(st); err != nil {
		return fmt.Errorf("room: persist chat %d: %w", st.ChatID, err)
	}
	if err := m.store.PutQueue(st.ChatID, st.Queue); err != nil {
		return fmt.Errorf("room: persist chat %d queue: %w", st.ChatID, err)
	}
	return nil
}

// waitingLocked returns the waiting queue: everything after the
// front track while one is active, else the whole list.
// Positions are renumbered 0-based; the stored positions
// (full-list indices) never reach the wire.
func waitingLocked(rs *roomState) []model.QueueItem {
	items := rs.st.Queue
	if rs.st.Playing && len(items) > 0 {
		items = items[1:]
	}
	out := make([]model.QueueItem, len(items))
	for i, item := range items {
		item.Position = i
		out[i] = item
	}
	return out
}

// snapshotLocked builds the NowPlaying view over a room. Queue is
// never nil on the wire (an empty queue serialises as []).
func snapshotLocked(rs *roomState) model.NowPlaying {
	np := model.NowPlaying{
		ChatID:          rs.st.ChatID,
		Engine:          "backend-go",
		Loop:            rs.st.Loop,
		LoopCount:       rs.st.Loop,
		Paused:          rs.st.Paused,
		IsPaused:        rs.st.Paused,
		IsMuted:         rs.st.Muted,
		PositionSec:     positionLocked(rs),
		Queue:           waitingLocked(rs),
		Shuffle:         rs.st.Shuffle,
		Autoplay:        rs.st.Autoplay,
		AutoplayEnabled: rs.st.Autoplay,
		Volume:          rs.st.Volume,
		Speed:           rs.st.Speed,
	}
	if rs.st.Playing && len(rs.st.Queue) > 0 {
		track := rs.st.Queue[0].Track
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

// PlayingSnapshots returns the NowPlaying view of every room with
// an active track, in one lock hold. The queue watchdog
// (handlers) uses it to find tracks whose wall-clock position has
// passed their duration.
func (m *Manager) PlayingSnapshots() []model.NowPlaying {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []model.NowPlaying
	for _, rs := range m.rooms {
		if rs.st.Playing {
			out = append(out, snapshotLocked(rs))
		}
	}
	return out
}

// Queue returns the waiting queue for a chat in play order
// (0-based positions, the active track is never in the list), or
// false when the chat has no room.
func (m *Manager) Queue(chatID int64) ([]model.QueueItem, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return nil, false
	}
	return waitingLocked(rs), true
}

// Current returns a copy of the active track, or false when the
// room has none.
func (m *Manager) Current(chatID int64) (model.Track, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || !rs.st.Playing || len(rs.st.Queue) == 0 {
		return model.Track{}, false
	}
	return rs.st.Queue[0].Track, true
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

// Play plays a track now when the room has no active track, else
// appends it to the waiting list (YukkiMusic rule, all three
// engines). A track started into a queued-only room jumps to the
// front; the waiting tracks stay waiting behind it.
func (m *Manager) Play(chatID int64, track model.Track, by int64) (PlayOutcome, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	if !rs.st.Playing {
		m.prependAndActivateLocked(rs, track)
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
	waiting := len(rs.st.Queue)
	if rs.st.Playing && waiting > 0 {
		waiting-- // the front track is active, not waiting
	}
	if m.maxQueue > 0 && waiting >= m.maxQueue {
		return fmt.Errorf("%w (max %d tracks)", ErrQueueFull, m.maxQueue)
	}
	rs.st.Queue = append(rs.st.Queue, model.QueueItem{
		Position:    len(rs.st.Queue),
		RequestedBy: by,
		Track:       track,
	})
	return nil
}

// Remove deletes exactly one waiting item by 0-based waiting
// position; removing the active track is impossible through this
// action. Out of range is an error and mutates nothing.
func (m *Manager) Remove(chatID int64, pos int) ([]model.QueueItem, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return nil, fmt.Errorf("%w: position %d out of range (empty queue)", ErrInvalid, pos)
	}
	waiting := waitingLocked(rs)
	if pos < 0 || pos >= len(waiting) {
		return nil, fmt.Errorf("%w: position %d out of range (queue has %d items)", ErrInvalid, pos, len(waiting))
	}
	idx := pos
	if rs.st.Playing {
		idx = pos + 1 // waiting position p is list index p+1
	}
	rs.st.Queue = append(rs.st.Queue[:idx], rs.st.Queue[idx+1:]...)
	if err := m.persistLocked(rs); err != nil {
		return nil, err
	}
	return waitingLocked(rs), nil
}

// MoveToNext moves a waiting track to the front of the waiting list
// (list index 1 while a track is playing, index 0 while idle), so
// the normal advance path picks it up next (queue-port Tier 3,
// item 23 — TgMusicBot force-play is insert-at-1 + PlayNext; the
// one-list model makes waiting position 0 exactly "play next").
// Already-next and already-playing are no-ops (moved=false). An
// unknown track id, or an empty room, is an error and mutates
// nothing.
func (m *Manager) MoveToNext(chatID int64, trackID string) (model.NowPlaying, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
		return model.NowPlaying{}, false, ErrNothingPlaying
	}
	if trackID == "" {
		return model.NowPlaying{}, false, fmt.Errorf("%w: track id is required", ErrInvalid)
	}
	// The front track while playing; the waiting list starts at 1.
	start := 0
	if rs.st.Playing {
		if rs.st.Queue[0].Track.ID == trackID {
			return snapshotLocked(rs), false, nil // already playing
		}
		start = 1
	}
	idx := -1
	for i := start; i < len(rs.st.Queue); i++ {
		if rs.st.Queue[i].Track.ID == trackID {
			idx = i
			break
		}
	}
	if idx < 0 {
		return model.NowPlaying{}, false, fmt.Errorf("%w: track %q is not in the queue", ErrInvalid, trackID)
	}
	if idx == start {
		return snapshotLocked(rs), false, nil // already next
	}
	item := rs.st.Queue[idx]
	rs.st.Queue = append(rs.st.Queue[:idx], rs.st.Queue[idx+1:]...)
	rs.st.Queue = append(rs.st.Queue[:start], append([]model.QueueItem{item}, rs.st.Queue[start:]...)...)
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, false, err
	}
	return snapshotLocked(rs), true, nil
}

// ClearQueue empties the waiting list only; the active track keeps
// playing.
func (m *Manager) ClearQueue(chatID int64) ([]model.QueueItem, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return []model.QueueItem{}, nil
	}
	if rs.st.Playing && len(rs.st.Queue) > 0 {
		rs.st.Queue = rs.st.Queue[:1]
	} else {
		rs.st.Queue = nil
	}
	if err := m.persistLocked(rs); err != nil {
		return nil, err
	}
	return []model.QueueItem{}, nil
}

// --- track lifecycle -------------------------------------------------

// activateFrontLocked makes the front track the active one:
// position 0, clock running, not paused. It notes the track in
// the autoplay history (append if new, cap 50) and clears the
// radio state unless the front track IS the stored radio track.
func (m *Manager) activateFrontLocked(rs *roomState) {
	track := rs.st.Queue[0].Track
	rs.st.Playing = true
	rs.st.Paused = false
	rs.st.PositionSec = 0
	rs.startedAt = time.Now()
	rs.st.LastTrackID = track.ID
	if rs.st.Radio == nil || rs.st.Radio.NowPlaying == nil || rs.st.Radio.NowPlaying.ID != track.ID {
		rs.st.Radio = nil
	}
	m.noteHistoryLocked(rs, track.ID)
}

// setFrontLocked replaces the front track with t (or prepends it
// when the list is empty) and activates it.
func (m *Manager) setFrontLocked(rs *roomState, t model.Track) {
	if len(rs.st.Queue) > 0 {
		rs.st.Queue[0] = model.QueueItem{Position: 0, RequestedBy: 0, Track: t}
	} else {
		rs.st.Queue = []model.QueueItem{{Position: 0, RequestedBy: 0, Track: t}}
	}
	m.activateFrontLocked(rs)
}

// prependAndActivateLocked inserts t at the front of the list and
// activates it; the previous front track (if any was waiting)
// stays waiting behind it.
func (m *Manager) prependAndActivateLocked(rs *roomState, t model.Track) {
	item := model.QueueItem{Position: 0, RequestedBy: 0, Track: t}
	rs.st.Queue = append([]model.QueueItem{item}, rs.st.Queue...)
	m.activateFrontLocked(rs)
}

// popFrontLocked removes the front track and returns it.
func (m *Manager) popFrontLocked(rs *roomState) (model.Track, bool) {
	if len(rs.st.Queue) == 0 {
		return model.Track{}, false
	}
	track := rs.st.Queue[0].Track
	rs.st.Queue = rs.st.Queue[1:]
	return track, true
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

// goIdleLocked clears the active-track flags and freezes the
// clock. The list is untouched: at every call site it is either
// empty already or the remaining tracks are meant to stay as a
// queued-only list. Shuffle, volume/mute, the autoplay flag and
// the history are kept.
func (m *Manager) goIdleLocked(rs *roomState) {
	rs.st.Playing = false
	rs.st.Paused = false
	rs.st.PositionSec = 0
	rs.startedAt = time.Time{}
	rs.st.Radio = nil
}

// isDefaultLocked reports whether a room carries no state worth
// keeping in the store: no tracks at all, and no non-default
// settings (inventory §1.5 / §4.6).
func isDefaultLocked(rs *roomState) bool {
	st := rs.st
	return len(st.Queue) == 0 &&
		!st.Playing &&
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

// Advance moves the room to the next track (loop → pop → autoplay
// → idle, §4.3). endedID is the id of the track that just ended, or
// "" to advance unconditionally (vc.skip semantics). A non-empty
// endedID that does not match the front track means the end event
// is stale (the user already skipped or stopped) and Advance is a
// no-op. fromSkip selects the idle reason "skipped" instead of
// "queue_empty". A room with no active track but a non-empty list
// activates its front track (the old "empty current but a waiting
// queue starts the next track" skip rule).
func (m *Manager) Advance(ctx context.Context, chatID int64, endedID string, fromSkip bool) (AdvanceOutcome, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}
	if len(rs.st.Queue) == 0 {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}
	if endedID != "" && (!rs.st.Playing || rs.st.Queue[0].Track.ID != endedID) {
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNoOp}, nil
	}

	// No active track: activate the front track.
	if !rs.st.Playing {
		m.activateFrontLocked(rs)
		track := rs.st.Queue[0].Track
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNext, Track: &track}, nil
	}
	ended := rs.st.Queue[0].Track

	// 1. Loop: replay the front track, consuming one replay.
	if rs.st.Loop > 0 {
		rs.st.Loop--
		m.activateFrontLocked(rs)
		track := rs.st.Queue[0].Track
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceReplayed, Track: &track, EndedTrack: &ended}, nil
	}

	// 2. Pop the ended track; the new front starts.
	m.popFrontLocked(rs)
	if len(rs.st.Queue) > 0 {
		m.activateFrontLocked(rs)
		track := rs.st.Queue[0].Track
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceNext, Track: &track, PreviousTrackID: ended.ID, EndedTrack: &ended}, nil
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
			return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: &ended, Reason: "queue_empty"}, nil
		}
		exclude := map[string]bool{ended.ID: true}
		for _, id := range rs.st.AutoplayHistory {
			exclude[id] = true
		}
		picker := m.picker
		m.mu.Unlock()
		pick, pickErr := picker(ctx, ended, exclude)
		m.mu.Lock()
		rs, ok = m.rooms[chatID]
		if !ok || len(rs.st.Queue) != 0 || rs.st.Playing {
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
			return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: &ended, Reason: "autoplay_failed"}, nil
		}
		m.prependAndActivateLocked(rs, pick)
		track := rs.st.Queue[0].Track
		if err := m.persistLocked(rs); err != nil {
			m.mu.Unlock()
			return AdvanceOutcome{}, err
		}
		m.mu.Unlock()
		return AdvanceOutcome{Kind: AdvanceAutoplay, Track: &track, EndedTrack: &ended}, nil
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
	return AdvanceOutcome{Kind: AdvanceIdle, EndedTrack: &ended, Reason: reason}, nil
}

// Skip resets the loop counter to 0 (all three engines) and then
// advances. A room with no active track but a waiting list starts
// the front track (vc-rust rule, §1.4); a fully empty room is
// ErrNothingPlaying.
func (m *Manager) Skip(ctx context.Context, chatID int64) (AdvanceOutcome, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
		m.mu.Unlock()
		return AdvanceOutcome{}, ErrNothingPlaying
	}
	rs.st.Loop = 0
	m.mu.Unlock()
	return m.Advance(ctx, chatID, "", true)
}

// AutoplaySkip is NOT a skip (§1.11): it is the gated pick. It
// requires autoplay on (else ErrInvalid) and an active track (else
// ErrNothingPlaying). The pick excludes last + queue + history, the
// loop resets to 0, and the picked track starts immediately. It
// returns the picked track and the track it replaced.
func (m *Manager) AutoplaySkip(ctx context.Context, chatID int64) (model.Track, model.Track, error) {
	m.mu.Lock()
	rs, ok := m.rooms[chatID]
	if !ok || !rs.st.Playing || len(rs.st.Queue) == 0 {
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
	last := rs.st.Queue[0].Track
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
	if !ok || !rs.st.Playing || len(rs.st.Queue) == 0 || rs.st.Queue[0].Track.ID != last.ID || !rs.st.Autoplay {
		return model.Track{}, model.Track{}, ErrStale
	}
	if err != nil {
		return model.Track{}, model.Track{}, fmt.Errorf("%w: %v", ErrPickerFailed, err)
	}
	if pick.ID == "" {
		return model.Track{}, model.Track{}, ErrNoPick
	}
	rs.st.Loop = 0
	m.setFrontLocked(rs, pick)
	if err := m.persistLocked(rs); err != nil {
		return model.Track{}, model.Track{}, err
	}
	return pick, last, nil
}

// Stop clears the active track and the whole list; loop and
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
	rs.st.Queue = nil
	rs.st.Playing = false
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

// ClearCurrent drops the front track without touching the rest of
// the list (GoIdle semantics). It is the rollback for a vc.play
// fresh start whose voice bind failed: the room returns to idle
// (or queued-only, when tracks are waiting) with the rest of the
// list intact, as if the track had never started.
func (m *Manager) ClearCurrent(chatID int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	m.popFrontLocked(rs)
	m.goIdleLocked(rs)
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// ReplaceFront drops the front track and inserts t as the new
// front, activated (clock running, not paused). It is the
// rollback for a vc.autoplay_skip whose pick failed to bind: the
// replaced track comes back as the active track, so the room
// never stands on a track with no feed behind it.
func (m *Manager) ReplaceFront(chatID int64, t model.Track) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	m.popFrontLocked(rs)
	m.setFrontLocked(rs, t)
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
	if !ok || !rs.st.Playing {
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
	if !ok || !rs.st.Playing {
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
	if !ok || !rs.st.Playing {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if pos < 0 {
		return model.NowPlaying{}, ErrPositionOutOfRange
	}
	if rs.st.Queue[0].Track.DurationSec > 0 && pos > rs.st.Queue[0].Track.DurationSec {
		pos = rs.st.Queue[0].Track.DurationSec
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

// SetLoop sets the remaining replays of the active track (0-10).
// Setting the count does not restart the track; the count is
// consumed one per replay at the natural track end. Idle is
// ErrNothingPlaying.
func (m *Manager) SetLoop(chatID int64, count int) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || !rs.st.Playing {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	rs.st.Loop = count
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetShuffle enables/disables shuffle. Enabling shuffles the
// waiting list in place (Fisher-Yates, vc-ts/vc-rust rule) and
// sets the flag; disabling clears the flag only and never
// reorders. Enabling with < 2 waiting tracks is a no-op reorder
// that still sets the flag. A chat with an empty list is
// ErrNothingPlaying.
func (m *Manager) SetShuffle(chatID int64, enabled bool) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
		return model.NowPlaying{}, ErrNothingPlaying
	}
	if enabled {
		// Shuffle the waiting part only: everything after the
		// front track while one is active, else the whole list.
		start := 0
		if rs.st.Playing {
			start = 1
		}
		if len(rs.st.Queue)-start > 1 {
			rand.Shuffle(len(rs.st.Queue)-start, func(i, j int) {
				a, b := start+i, start+j
				rs.st.Queue[a], rs.st.Queue[b] = rs.st.Queue[b], rs.st.Queue[a]
			})
		}
	}
	rs.st.Shuffle = enabled
	if err := m.persistLocked(rs); err != nil {
		return model.NowPlaying{}, err
	}
	return snapshotLocked(rs), nil
}

// SetAutoplay sets the autoplay flag. Disabling clears the history
// (vc-go/vc-ts rule). A chat with an empty list is
// ErrNothingPlaying.
func (m *Manager) SetAutoplay(chatID int64, enabled bool) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
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
// rule, §10-D15). Mute is separate: setting volume 0 does NOT set
// is_muted. A chat with an empty list is ErrNothingPlaying.
func (m *Manager) SetVolume(chatID int64, volume int) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	if !ok || len(rs.st.Queue) == 0 {
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
	if !ok || !rs.st.Playing {
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

// StartRadio starts a radio track as the front track and records
// the radio state, both in the room and in the store's radio
// bucket (so radio.status reads the same state). The radio track
// has duration 0 and never auto-advances.
func (m *Manager) StartRadio(chatID int64, radio model.RadioState, track model.Track, by int64) (model.NowPlaying, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs := m.getOrCreateLocked(chatID)
	radio.NowPlaying = &track
	m.setFrontLocked(rs, track)
	// setFrontLocked clears Radio unless the track IS the stored
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
// whether the front track was the radio track (the caller leaves
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
	wasRadio := rs.st.Playing && len(rs.st.Queue) > 0 &&
		rs.st.Radio.NowPlaying != nil && rs.st.Queue[0].Track.ID == rs.st.Radio.NowPlaying.ID
	rs.st.Radio = nil
	if wasRadio {
		m.popFrontLocked(rs)
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

// IsRadioCurrent reports whether the room's front track is its
// stored radio track.
func (m *Manager) IsRadioCurrent(chatID int64) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	rs, ok := m.rooms[chatID]
	return ok && rs.st.Playing && rs.st.Radio != nil && len(rs.st.Queue) > 0 &&
		rs.st.Radio.NowPlaying != nil && rs.st.Queue[0].Track.ID == rs.st.Radio.NowPlaying.ID
}
