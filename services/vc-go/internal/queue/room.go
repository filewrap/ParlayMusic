// Package queue implements per-chat playback state: the current track, the
// waiting queue, pause/volume/position, loop/shuffle/autoplay — ported in
// behaviour from YukkiMusic-Go internal/core/queue.go + room.go (lock-free
// queue owned by a mutex-guarded Room) and ArcMusic
// internal/core/room_queue.go (NextTrack with loop/shuffle handling,
// RemoveFromQueue, MoveInQueue), with TgMusicBot's autoplay-history model
// (internal/cache/player_cache.go + internal/calls/autoplay.go).
//
// Differences from the upstreams, by design:
//   - No Telegram client, no ntgcalls, no MongoDB here. The room is pure
//     playback state; the C++ core owns voice joining through its
//     VoiceCall interface and calls this service over IPC.
//   - Position is wall-clock derived (startedAt + accumulated offset) the
//     way TgMusicBot derives played time from calls.Calls.PlayedTime.
//   - Queues + settings ARE persisted (MongoDB when MONGO_URI is set,
//     JSON file otherwise) — a Parlay extension; no upstream caches
//     queues in Mongo.
package queue

import (
	"math/rand"
	"sync"
	"time"

	"parlay/vc-go/internal/types"
)

// autoplayHistoryLimit caps the per-room autoplay history. When it is
// reached the history is cleared and autoplay stops for the session
// (TgMusicBot behaviour: history cleared at the limit, playback stops).
const autoplayHistoryLimit = 50

// MaxLoop is the highest loop count accepted (upstream /loop range 0-10).
const MaxLoop = 10

// AutoplayPicker resolves the next autoplay track for last, skipping any
// track id in exclude (queue + history + last itself). It returns
// (nil, nil) when no candidate exists.
type AutoplayPicker func(last types.Track, exclude map[string]bool) (*types.Track, error)

// Room is one chat's playback state. All methods are safe for concurrent
// use; callers never touch fields directly.
type Room struct {
	mu sync.Mutex

	chatID int64
	// current is the track being played, or nil when idle.
	current *types.Track
	// waiting holds tracks queued behind current, in play order.
	waiting []types.QueueItem
	// paused freezes position accumulation.
	paused bool
	// volume is 0-200, default 100.
	volume int
	// positionSec is the seek offset the current track started from.
	positionSec int64
	// startedAt is when the current track (re)started playing at
	// positionSec; zero when paused or idle.
	startedAt time.Time
	// prefetched is the next track's resolved stream URL, filled by the
	// high-perf prefetch workers (nil in low-ram mode).
	prefetched *types.Track
	// loop is the remaining replays of the current track (0 = off,
	// upstream /loop range 0-10). Decremented per replay; /skip resets it.
	loop int
	// shuffle randomises the pop from waiting instead of taking index 0.
	// The currently playing track is never touched by it.
	shuffle bool
	// autoplay picks a related track when the queue drains.
	autoplay bool
	// autoplayHistory holds ids of tracks already picked by autoplay
	// this session, so they are never suggested again.
	autoplayHistory []string
	// autoplayPick resolves the next autoplay candidate; nil disables
	// the network leg (room idles on drain instead). Set by Manager.
	autoplayPick AutoplayPicker
}

func newRoom(chatID int64) *Room {
	return &Room{chatID: chatID, volume: 100}
}

// ChatID returns the room's chat id.
func (r *Room) ChatID() int64 { return r.chatID }

// Position returns the current playback position in seconds. While playing
// it is positionSec + wall-clock elapsed since startedAt; while paused it
// is frozen at positionSec.
func (r *Room) Position() int64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.positionLocked()
}

func (r *Room) positionLocked() int64 {
	if r.current == nil {
		return 0
	}
	if r.paused || r.startedAt.IsZero() {
		return r.positionSec
	}
	return r.positionSec + int64(time.Since(r.startedAt)/time.Second)
}

// snapshotLocked builds a NowPlaying for this room. Caller holds r.mu.
func (r *Room) snapshotLocked() types.NowPlaying {
	np := types.NowPlaying{
		ChatID:      r.chatID,
		Paused:      r.paused,
		PositionSec: r.positionLocked(),
		Volume:      r.volume,
		Engine:      "go",
		Queue:       make([]types.QueueItem, 0, len(r.waiting)),
		Loop:        r.loop,
		Shuffle:     r.shuffle,
		Autoplay:    r.autoplay,
	}
	if r.current != nil {
		t := *r.current
		np.Track = &t
		// Top-level stream_url mirrors the current track's (A8
		// residual: the contract's NowPlaying.stream_url was never
		// emitted — only the nested track.stream_url was).
		np.StreamURL = t.StreamURL
	}
	for i, item := range r.waiting {
		item.Position = i
		np.Queue = append(np.Queue, item)
	}
	return np
}

// Snapshot returns the room's current NowPlaying.
func (r *Room) Snapshot() types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.snapshotLocked()
}

// Current returns a copy of the current track, or nil.
func (r *Room) Current() *types.Track {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current == nil {
		return nil
	}
	t := *r.current
	return &t
}

// IsIdle reports whether nothing is playing and the queue is empty.
func (r *Room) IsIdle() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.current == nil && len(r.waiting) == 0
}

// Play starts track immediately if idle, otherwise appends it to the
// waiting queue (YukkiMusic behaviour: /play enqueues when already
// streaming). Returns the snapshot after the change.
func (r *Room) Play(t types.Track, requestedBy int64) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current == nil {
		r.startTrackLocked(t)
	} else {
		r.waiting = append(r.waiting, types.QueueItem{
			Position:    len(r.waiting),
			RequestedBy: requestedBy,
			Track:       t,
		})
	}
	return r.snapshotLocked()
}

// Enqueue appends a track to the waiting queue.
func (r *Room) Enqueue(t types.Track, requestedBy int64) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.waiting = append(r.waiting, types.QueueItem{
		Position:    len(r.waiting),
		RequestedBy: requestedBy,
		Track:       t,
	})
	return r.snapshotLocked()
}

// Remove deletes the waiting item at 0-based position. Returns false when
// out of range.
func (r *Room) Remove(position int) (types.NowPlaying, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if position < 0 || position >= len(r.waiting) {
		return r.snapshotLocked(), false
	}
	r.waiting = append(r.waiting[:position], r.waiting[position+1:]...)
	return r.snapshotLocked(), true
}

// Clear empties the waiting queue (current track keeps playing).
func (r *Room) Clear() types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.waiting = nil
	return r.snapshotLocked()
}

// Pause freezes playback. No-op when idle or already paused.
func (r *Room) Pause() types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current != nil && !r.paused {
		r.positionSec = r.positionLocked()
		r.startedAt = time.Time{}
		r.paused = true
	}
	return r.snapshotLocked()
}

// Resume continues playback. No-op when idle or not paused.
func (r *Room) Resume() types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current != nil && r.paused {
		r.startedAt = time.Now()
		r.paused = false
	}
	return r.snapshotLocked()
}

// Seek moves the current track to positionSec (clamped to [0, duration]
// when the duration is known). No-op when idle.
func (r *Room) SeekTo(positionSec int64) (types.NowPlaying, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current == nil {
		return r.snapshotLocked(), false
	}
	if positionSec < 0 {
		positionSec = 0
	}
	if r.current.DurationSec > 0 && positionSec > r.current.DurationSec {
		positionSec = r.current.DurationSec
	}
	r.positionSec = positionSec
	if !r.paused {
		r.startedAt = time.Now()
	}
	return r.snapshotLocked(), true
}

// SetVolume clamps and stores the volume (0-200).
func (r *Room) SetVolume(v int) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	if v < 0 {
		v = 0
	}
	if v > 200 {
		v = 200
	}
	r.volume = v
	return r.snapshotLocked()
}

// SetLoop sets the remaining replays of the current track, clamped to
// [0, MaxLoop] (upstream /loop range). 0 turns looping off.
func (r *Room) SetLoop(count int) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	if count < 0 {
		count = 0
	}
	if count > MaxLoop {
		count = MaxLoop
	}
	r.loop = count
	return r.snapshotLocked()
}

// Loop returns the remaining loop count.
func (r *Room) Loop() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.loop
}

// SetShuffle enables or disables shuffle. Shuffle only randomises the
// pop from the waiting queue; the current track is untouched and the
// stored queue order is never reordered (YukkiMusic/ArcMusic semantics).
func (r *Room) SetShuffle(enabled bool) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.shuffle = enabled
	return r.snapshotLocked()
}

// Shuffle reports whether shuffle is on.
func (r *Room) Shuffle() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.shuffle
}

// SetAutoplay enables or disables autoplay for this room.
func (r *Room) SetAutoplay(enabled bool) types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.autoplay = enabled
	if !enabled {
		r.autoplayHistory = nil
	}
	return r.snapshotLocked()
}

// Autoplay reports whether autoplay is on.
func (r *Room) Autoplay() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.autoplay
}

// SetAutoplayPicker installs the function that resolves the next autoplay
// candidate. Nil disables the network leg.
func (r *Room) SetAutoplayPicker(pick AutoplayPicker) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.autoplayPick = pick
}

// Skip advances to the next queued track. The loop counter is reset
// (upstream /skip semantics). Returns false when there was nothing
// playing.
func (r *Room) Skip() (types.NowPlaying, bool) {
	r.mu.Lock()
	if r.current == nil {
		np := r.snapshotLocked()
		r.mu.Unlock()
		return np, false
	}
	r.loop = 0
	r.mu.Unlock()
	return r.Advance(), true
}

// AutoplaySkip is /v1/autoplay-skip: like Skip, but it is the explicit
// "skip this autoplay suggestion" path — same advance, loop reset.
func (r *Room) AutoplaySkip() (types.NowPlaying, bool) {
	return r.Skip()
}

// Stop clears the current track and the whole queue (TgMusicBot /stop).
// Loop and autoplay are session-scoped, so they reset too; shuffle is a
// persistent setting and is kept.
func (r *Room) Stop() types.NowPlaying {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.current = nil
	r.waiting = nil
	r.paused = false
	r.positionSec = 0
	r.startedAt = time.Time{}
	r.prefetched = nil
	r.loop = 0
	r.autoplay = false
	r.autoplayHistory = nil
	return r.snapshotLocked()
}

// startTrackLocked makes t the current track and (re)starts the clock.
func (r *Room) startTrackLocked(t types.Track) {
	r.current = &t
	r.positionSec = 0
	r.startedAt = time.Now()
	r.paused = false
	r.prefetched = nil
}

// idleLocked clears the current track. Caller holds r.mu.
func (r *Room) idleLocked() {
	r.current = nil
	r.positionSec = 0
	r.startedAt = time.Time{}
	r.paused = false
	r.prefetched = nil
}

// popWaitingLocked removes and returns the next waiting track: index 0
// when shuffle is off, a random index when it is on. Caller holds r.mu;
// ok is false when the queue is empty.
func (r *Room) popWaitingLocked() (types.QueueItem, bool) {
	if len(r.waiting) == 0 {
		return types.QueueItem{}, false
	}
	idx := 0
	if r.shuffle {
		idx = rand.Intn(len(r.waiting))
	}
	next := r.waiting[idx]
	r.waiting = append(r.waiting[:idx], r.waiting[idx+1:]...)
	return next, true
}

// autoplayExcludeLocked builds the id set the picker must avoid: the
// last track, everything already queued, and the autoplay history.
// Caller holds r.mu.
func (r *Room) autoplayExcludeLocked(last types.Track) map[string]bool {
	exclude := make(map[string]bool, len(r.waiting)+len(r.autoplayHistory)+1)
	exclude[last.ID] = true
	for _, item := range r.waiting {
		exclude[item.Track.ID] = true
	}
	for _, id := range r.autoplayHistory {
		exclude[id] = true
	}
	return exclude
}

// Advance moves to the next track, honouring loop, then the
// (shuffle-aware) queue, then autoplay. Called by the runner when
// DueForAdvance is true and by Skip/AutoplaySkip.
//
// The autoplay leg does network I/O (yt-dlp), so the room mutex is
// released while the picker runs; the room is re-checked afterwards and
// the pick is applied only if the room is still in the same drained
// state (same current track, empty queue).
func (r *Room) Advance() types.NowPlaying {
	r.mu.Lock()

	// 1. Loop: replay the current track, counter decremented.
	if r.current != nil && r.loop > 0 {
		r.loop--
		t := *r.current
		r.startTrackLocked(t)
		r.mu.Unlock()
		return r.Snapshot()
	}

	// 2. Queue pop (shuffle-aware).
	if next, ok := r.popWaitingLocked(); ok {
		// If the prefetch worker already resolved this track, use it.
		if r.prefetched != nil && r.prefetched.ID == next.Track.ID {
			next.Track = *r.prefetched
		}
		r.startTrackLocked(next.Track)
		r.mu.Unlock()
		return r.Snapshot()
	}

	// 3. Autoplay on drain.
	if r.current != nil && r.autoplay && r.autoplayPick != nil {
		if len(r.autoplayHistory) >= autoplayHistoryLimit {
			// History full: clear it and stop, upstream behaviour.
			r.autoplayHistory = nil
			r.idleLocked()
			r.mu.Unlock()
			return r.Snapshot()
		}
		last := *r.current
		exclude := r.autoplayExcludeLocked(last)
		pick := r.autoplayPick
		r.mu.Unlock()
		picked, err := pick(last, exclude)
		r.mu.Lock()
		if err == nil && picked != nil &&
			r.current != nil && r.current.ID == last.ID && len(r.waiting) == 0 {
			r.autoplayHistory = append(r.autoplayHistory, picked.ID)
			r.startTrackLocked(*picked)
		} else if r.current != nil && r.current.ID == last.ID && len(r.waiting) == 0 {
			r.idleLocked()
		}
		r.mu.Unlock()
		return r.Snapshot()
	}

	// 4. Nothing left: idle.
	r.idleLocked()
	r.mu.Unlock()
	return r.Snapshot()
}

// NextWaiting returns a copy of the first waiting track, or nil. Used by
// the prefetch workers. With shuffle on, the "first" track is only a
// hint — the pop at advance time is random.
func (r *Room) NextWaiting() *types.Track {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.waiting) == 0 {
		return nil
	}
	t := r.waiting[0].Track
	return &t
}

// SetPrefetched stores a resolved copy of the next waiting track.
func (r *Room) SetPrefetched(t types.Track) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.prefetched = &t
}

// HasPrefetch reports whether the next track is already resolved.
func (r *Room) HasPrefetch() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.prefetched != nil
}

// DueForAdvance reports whether the current track has played past its
// known duration. Unknown-duration tracks never auto-advance (the core
// must skip/stop them explicitly).
func (r *Room) DueForAdvance() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.current == nil || r.current.DurationSec <= 0 || r.paused {
		return false
	}
	return r.positionLocked() >= r.current.DurationSec
}

// --- persistence snapshot (plain data, no mutex) ---

// PersistedRoom is the JSON-serialisable form of a Room.
type PersistedRoom struct {
	ChatID          int64             `json:"chat_id"`
	Current         *types.Track      `json:"current,omitempty"`
	Waiting         []types.QueueItem `json:"waiting,omitempty"`
	Paused          bool              `json:"paused"`
	Volume          int               `json:"volume"`
	Position        int64             `json:"position_sec"`
	Loop            int               `json:"loop,omitempty"`
	Shuffle         bool              `json:"shuffle,omitempty"`
	Autoplay        bool              `json:"autoplay,omitempty"`
	AutoplayHistory []string          `json:"autoplay_history,omitempty"`
}

// Persist captures the room for the store. Position is frozen at capture
// time; on restore the track resumes from there.
func (r *Room) Persist() PersistedRoom {
	r.mu.Lock()
	defer r.mu.Unlock()
	p := PersistedRoom{
		ChatID:          r.chatID,
		Paused:          r.paused,
		Volume:          r.volume,
		Position:        r.positionLocked(),
		Loop:            r.loop,
		Shuffle:         r.shuffle,
		Autoplay:        r.autoplay,
		AutoplayHistory: append([]string(nil), r.autoplayHistory...),
	}
	if r.current != nil {
		t := *r.current
		p.Current = &t
	}
	if len(r.waiting) > 0 {
		p.Waiting = make([]types.QueueItem, len(r.waiting))
		copy(p.Waiting, r.waiting)
	}
	return p
}

// Restore rebuilds a room from persisted data. Restored rooms come back
// paused (safe default: never auto-resume audio after a restart).
// The autoplay picker is NOT restored — the Manager re-installs it.
func Restore(p PersistedRoom) *Room {
	r := newRoom(p.ChatID)
	r.current = p.Current
	r.waiting = p.Waiting
	r.volume = p.Volume
	r.positionSec = p.Position
	r.paused = true // never auto-resume after restart
	r.loop = p.Loop
	r.shuffle = p.Shuffle
	r.autoplay = p.Autoplay
	r.autoplayHistory = p.AutoplayHistory
	if r.volume == 0 {
		r.volume = 100
	}
	return r
}

// Manager owns all rooms, keyed by chat id.
type Manager struct {
	mu    sync.Mutex
	rooms map[int64]*Room
	// onChange is called after every mutation (used to trigger persistence).
	onChange func()
	// autoplayPick is installed on every room, present and future.
	autoplayPick AutoplayPicker
}

// NewManager creates an empty room manager.
func NewManager(onChange func()) *Manager {
	return &Manager{rooms: make(map[int64]*Room), onChange: onChange}
}

// SetAutoplayPicker installs the autoplay resolver on all existing rooms
// and every room created afterwards.
func (m *Manager) SetAutoplayPicker(pick AutoplayPicker) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.autoplayPick = pick
	for _, r := range m.rooms {
		r.SetAutoplayPicker(pick)
	}
}

// Get returns the room for chatID, creating it if needed.
func (m *Manager) Get(chatID int64) *Room {
	m.mu.Lock()
	defer m.mu.Unlock()
	r, ok := m.rooms[chatID]
	if !ok {
		r = newRoom(chatID)
		r.SetAutoplayPicker(m.autoplayPick)
		m.rooms[chatID] = r
	}
	return r
}

// Lookup returns the room for chatID, or nil when it does not exist.
func (m *Manager) Lookup(chatID int64) *Room {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.rooms[chatID]
}

// Rooms returns a snapshot of all rooms.
func (m *Manager) Rooms() []*Room {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]*Room, 0, len(m.rooms))
	for _, r := range m.rooms {
		out = append(out, r)
	}
	return out
}

// Changed notifies the persistence hook. Call after any mutation.
func (m *Manager) Changed() {
	if m.onChange != nil {
		m.onChange()
	}
}

// Load restores rooms from persisted data, replacing current state.
// Restored rooms get the manager's autoplay picker re-installed.
func (m *Manager) Load(persisted []PersistedRoom) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.rooms = make(map[int64]*Room, len(persisted))
	for _, p := range persisted {
		r := Restore(p)
		r.SetAutoplayPicker(m.autoplayPick)
		m.rooms[p.ChatID] = r
	}
}
