package room

import (
	"context"
	"errors"
	"testing"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
)

func newTestManager(t *testing.T) *Manager {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })
	return NewManager(st, 20)
}

func mkTrack(id string, dur int64) model.Track {
	return model.Track{
		ID: id, Title: "Track " + id, Source: "youtube",
		DurationSec: dur, StreamURL: "https://example.invalid/" + id,
	}
}

func mustPlay(t *testing.T, m *Manager, chatID int64, track model.Track) {
	t.Helper()
	out, err := m.Play(chatID, track, 7)
	if err != nil {
		t.Fatalf("Play %s: %v", track.ID, err)
	}
	if !out.Started {
		t.Fatalf("Play %s: expected start, got queue", track.ID)
	}
}

func TestPlayIdleStartsAndBusyEnqueues(t *testing.T) {
	m := newTestManager(t)
	a, b := mkTrack("yt:a", 100), mkTrack("yt:b", 100)

	out, err := m.Play(1, a, 7)
	if err != nil || !out.Started {
		t.Fatalf("Play idle: started=%v err=%v", out.Started, err)
	}
	snap, ok := m.Snapshot(1)
	if !ok || snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("Snapshot after play: %+v ok=%v", snap, ok)
	}
	if snap.Queue == nil {
		t.Fatalf("Snapshot queue must be [] not null")
	}

	out, err = m.Play(1, b, 8)
	if err != nil || out.Started {
		t.Fatalf("Play busy: started=%v err=%v", out.Started, err)
	}
	snap, _ = m.Snapshot(1)
	if len(snap.Queue) != 1 || snap.Queue[0].Track.ID != "yt:b" || snap.Queue[0].Position != 0 || snap.Queue[0].RequestedBy != 8 {
		t.Fatalf("queue after busy play: %+v", snap.Queue)
	}
}

func TestEnqueueNeverStarts(t *testing.T) {
	m := newTestManager(t)
	if err := m.Enqueue(1, mkTrack("yt:a", 100), 7); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	snap, ok := m.Snapshot(1)
	if !ok {
		t.Fatalf("Enqueue must create the room")
	}
	if snap.Track != nil {
		t.Fatalf("Enqueue started playback: %+v", snap.Track)
	}
	if len(snap.Queue) != 1 {
		t.Fatalf("queue: %+v", snap.Queue)
	}
	// A queue-only room is observable with track null (§1.13).
	if snap.PositionSec != 0 {
		t.Fatalf("position: %d", snap.PositionSec)
	}
}

func TestQueueRemoveIsZeroBased(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	for _, id := range []string{"yt:b", "yt:c", "yt:d"} {
		if err := m.Enqueue(1, mkTrack(id, 100), 7); err != nil {
			t.Fatalf("Enqueue %s: %v", id, err)
		}
	}
	q, err := m.Remove(1, 1) // removes yt:c, the SECOND waiting track
	if err != nil {
		t.Fatalf("Remove: %v", err)
	}
	if len(q) != 2 || q[0].Track.ID != "yt:b" || q[1].Track.ID != "yt:d" || q[1].Position != 1 {
		t.Fatalf("queue after remove: %+v", q)
	}
	if _, err := m.Remove(1, 2); !errors.Is(err, ErrInvalid) {
		t.Fatalf("Remove out of range: err=%v", err)
	}
	if _, err := m.Remove(1, -1); !errors.Is(err, ErrInvalid) {
		t.Fatalf("Remove negative: err=%v", err)
	}
	// Removing cannot touch the current track: the queue is intact.
	snap, _ := m.Snapshot(1)
	if snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("current after removes: %+v", snap.Track)
	}
}

func TestClearQueueKeepsCurrent(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	q, err := m.ClearQueue(1)
	if err != nil || len(q) != 0 {
		t.Fatalf("ClearQueue: %v %+v", err, q)
	}
	snap, _ := m.Snapshot(1)
	if snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("current after clear: %+v", snap.Track)
	}
}

func TestQueueFull(t *testing.T) {
	m := newTestManager(t)
	m.maxQueue = 2
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	_ = m.Enqueue(1, mkTrack("yt:c", 100), 7)
	if _, err := m.Play(1, mkTrack("yt:d", 100), 7); !errors.Is(err, ErrQueueFull) {
		t.Fatalf("Play past cap: err=%v", err)
	}
	if err := m.Enqueue(1, mkTrack("yt:d", 100), 7); !errors.Is(err, ErrQueueFull) {
		t.Fatalf("Enqueue past cap: err=%v", err)
	}
}

func TestPauseResumeClock(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 300))
	time.Sleep(1100 * time.Millisecond)
	snap, _ := m.Snapshot(1)
	if snap.PositionSec < 1 {
		t.Fatalf("clock did not run: %d", snap.PositionSec)
	}
	paused, err := m.Pause(1)
	if err != nil || !paused.IsPaused {
		t.Fatalf("Pause: %+v %v", paused, err)
	}
	frozen := paused.PositionSec
	time.Sleep(400 * time.Millisecond)
	snap, _ = m.Snapshot(1)
	if snap.PositionSec != frozen {
		t.Fatalf("paused clock moved: %d -> %d", frozen, snap.PositionSec)
	}
	// Idempotent pause: no error, same frozen offset.
	again, err := m.Pause(1)
	if err != nil || again.PositionSec != frozen {
		t.Fatalf("second Pause: %+v %v", again, err)
	}
	resumed, err := m.Resume(1)
	if err != nil || resumed.IsPaused {
		t.Fatalf("Resume: %+v %v", resumed, err)
	}
	if resumed.PositionSec != frozen {
		t.Fatalf("Resume moved the offset: %d -> %d", frozen, resumed.PositionSec)
	}
	time.Sleep(1100 * time.Millisecond)
	snap, _ = m.Snapshot(1)
	if snap.PositionSec < frozen+1 {
		t.Fatalf("resumed clock did not run: %d", snap.PositionSec)
	}
}

func TestPauseResumeIdle(t *testing.T) {
	m := newTestManager(t)
	if _, err := m.Pause(1); !errors.Is(err, ErrNothingPlaying) {
		t.Fatalf("Pause idle: %v", err)
	}
	if _, err := m.Resume(1); !errors.Is(err, ErrNothingPlaying) {
		t.Fatalf("Resume idle: %v", err)
	}
	if _, err := m.SeekTo(1, 5); !errors.Is(err, ErrNothingPlaying) {
		t.Fatalf("Seek idle: %v", err)
	}
}

func TestSeekClamps(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if _, err := m.SeekTo(1, -1); !errors.Is(err, ErrPositionOutOfRange) {
		t.Fatalf("Seek negative: %v", err)
	}
	snap, err := m.SeekTo(1, 500) // beyond duration: clamp to 100
	if err != nil || snap.PositionSec != 100 {
		t.Fatalf("Seek beyond duration: %+v %v", snap, err)
	}
	snap, err = m.SeekTo(1, 42)
	if err != nil || snap.PositionSec != 42 {
		t.Fatalf("Seek 42: %+v %v", snap, err)
	}
	// Unknown duration accepts any >= 0.
	mustPlay(t, m, 2, model.Track{ID: "radio:x", Title: "radio", Source: "radio", DurationSec: 0})
	snap, err = m.SeekTo(2, 9999)
	if err != nil || snap.PositionSec != 9999 {
		t.Fatalf("Seek unknown duration: %+v %v", snap, err)
	}
}

func TestLoopConsumedAtAdvance(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	if _, err := m.SetLoop(1, 2); err != nil {
		t.Fatalf("SetLoop: %v", err)
	}
	out, err := m.Advance(ctx, 1, "yt:a", false)
	if err != nil || out.Kind != AdvanceReplayed || out.Track == nil || out.Track.ID != "yt:a" {
		t.Fatalf("Advance 1 (loop): %+v %v", out, err)
	}
	snap, _ := m.Snapshot(1)
	if snap.Loop != 1 {
		t.Fatalf("loop after replay 1: %d", snap.Loop)
	}
	out, err = m.Advance(ctx, 1, "yt:a", false)
	if err != nil || out.Kind != AdvanceReplayed {
		t.Fatalf("Advance 2 (loop): %+v %v", out, err)
	}
	snap, _ = m.Snapshot(1)
	if snap.Loop != 0 {
		t.Fatalf("loop after replay 2: %d", snap.Loop)
	}
	// Loop exhausted: the queue pops.
	out, err = m.Advance(ctx, 1, "yt:a", false)
	if err != nil || out.Kind != AdvanceNext || out.Track == nil || out.Track.ID != "yt:b" || out.PreviousTrackID != "yt:a" {
		t.Fatalf("Advance 3 (queue): %+v %v", out, err)
	}
	// Queue empty, autoplay off: idle, reason queue_empty.
	out, err = m.Advance(ctx, 1, "yt:b", false)
	if err != nil || out.Kind != AdvanceIdle || out.Reason != "queue_empty" {
		t.Fatalf("Advance 4 (idle): %+v %v", out, err)
	}
	if out.EndedTrack == nil || out.EndedTrack.ID != "yt:b" {
		t.Fatalf("idle ended track: %+v", out.EndedTrack)
	}
	snap, _ = m.Snapshot(1)
	if snap.Track != nil || snap.PositionSec != 0 {
		t.Fatalf("room after idle: %+v", snap)
	}
}

func TestAdvanceStaleEndIsNoOp(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	out, err := m.Advance(ctx, 1, "yt:someone-else", false)
	if err != nil || out.Kind != AdvanceNoOp {
		t.Fatalf("stale advance: %+v %v", out, err)
	}
	snap, _ := m.Snapshot(1)
	if snap.Track == nil || snap.Track.ID != "yt:a" || len(snap.Queue) != 1 {
		t.Fatalf("room changed on stale advance: %+v", snap)
	}
}

func TestSkipResetsLoopAndStartsQueuedWhenCurrentEmpty(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	if _, err := m.SetLoop(1, 5); err != nil {
		t.Fatalf("SetLoop: %v", err)
	}
	out, err := m.Skip(ctx, 1)
	if err != nil || out.Kind != AdvanceNext || out.Track == nil || out.Track.ID != "yt:b" {
		t.Fatalf("Skip: %+v %v", out, err)
	}
	snap, _ := m.Snapshot(1)
	if snap.Loop != 0 {
		t.Fatalf("skip did not reset loop: %d", snap.Loop)
	}
	// Skip to empty: idle, reason skipped.
	out, err = m.Skip(ctx, 1)
	if err != nil || out.Kind != AdvanceIdle || out.Reason != "skipped" {
		t.Fatalf("Skip to empty: %+v %v", out, err)
	}
	// Fully empty skip is an error.
	if _, err := m.Skip(ctx, 1); !errors.Is(err, ErrNothingPlaying) {
		t.Fatalf("Skip empty: %v", err)
	}
	// Skip with an empty current but a waiting queue starts the
	// next track (vc-rust rule, §1.4).
	_ = m.Enqueue(1, mkTrack("yt:c", 100), 7)
	out, err = m.Skip(ctx, 1)
	if err != nil || out.Kind != AdvanceNext || out.Track == nil || out.Track.ID != "yt:c" {
		t.Fatalf("Skip queue-only: %+v %v", out, err)
	}
}

func TestShuffleInPlace(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	ids := []string{"yt:b", "yt:c", "yt:d", "yt:e", "yt:f"}
	for _, id := range ids {
		_ = m.Enqueue(1, mkTrack(id, 100), 7)
	}
	snap, err := m.SetShuffle(1, true)
	if err != nil || !snap.Shuffle {
		t.Fatalf("SetShuffle on: %+v %v", snap, err)
	}
	seen := map[string]bool{}
	for _, item := range snap.Queue {
		seen[item.Track.ID] = true
		if item.Position < 0 || item.Position >= len(ids) {
			t.Fatalf("bad position: %+v", item)
		}
	}
	if len(seen) != len(ids) {
		t.Fatalf("shuffle lost tracks: %+v", snap.Queue)
	}
	if snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("shuffle touched current: %+v", snap.Track)
	}
	before := make([]string, len(snap.Queue))
	for i, item := range snap.Queue {
		before[i] = item.Track.ID
	}
	snap, err = m.SetShuffle(1, false)
	if err != nil || snap.Shuffle {
		t.Fatalf("SetShuffle off: %+v %v", snap, err)
	}
	after := make([]string, len(snap.Queue))
	for i, item := range snap.Queue {
		after[i] = item.Track.ID
	}
	for i := range before {
		if before[i] != after[i] {
			t.Fatalf("disabling shuffle reordered: %v -> %v", before, after)
		}
	}
}

func TestAutoplayDrainAndHistoryCap(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if _, err := m.SetAutoplay(1, true); err != nil {
		t.Fatalf("SetAutoplay: %v", err)
	}
	calls := 0
	m.SetPicker(func(_ context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		calls++
		if last.ID != "yt:a" {
			t.Errorf("picker last: %s", last.ID)
		}
		if !exclude["yt:a"] {
			t.Errorf("picker exclude missing last")
		}
		return mkTrack("yt:pick1", 200), nil
	})
	out, err := m.Advance(ctx, 1, "yt:a", false)
	if err != nil || out.Kind != AdvanceAutoplay || out.Track == nil || out.Track.ID != "yt:pick1" {
		t.Fatalf("autoplay advance: %+v %v", out, err)
	}
	if calls != 1 {
		t.Fatalf("picker calls: %d", calls)
	}
	snap, _ := m.Snapshot(1)
	if snap.Autoplay != true {
		t.Fatalf("autoplay flag cleared by advance")
	}
	// History at the cap: the next advance clears it and idles
	// instead of picking (§4.3 step 3).
	rs := m.rooms[1]
	rs.st.AutoplayHistory = make([]string, MaxAutoplayHistory)
	for i := range rs.st.AutoplayHistory {
		rs.st.AutoplayHistory[i] = "yt:old"
	}
	out, err = m.Advance(ctx, 1, "yt:pick1", false)
	if err != nil || out.Kind != AdvanceIdle || out.Reason != "queue_empty" {
		t.Fatalf("cap advance: %+v %v", out, err)
	}
	if len(m.rooms[1].st.AutoplayHistory) != 0 {
		t.Fatalf("history not cleared at cap")
	}
}

func TestAutoplayPickStaleWhenRoomChanges(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if _, err := m.SetAutoplay(1, true); err != nil {
		t.Fatalf("SetAutoplay: %v", err)
	}
	m.SetPicker(func(_ context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		// A /play wins while the pick is in flight: the queue
		// is no longer empty, so the pick must be discarded.
		_ = m.Enqueue(1, mkTrack("yt:intruder", 100), 9)
		return mkTrack("yt:pick1", 200), nil
	})
	out, err := m.Advance(ctx, 1, "yt:a", false)
	if err != nil || out.Kind != AdvanceNoOp {
		t.Fatalf("stale pick advance: %+v %v", out, err)
	}
	snap, _ := m.Snapshot(1)
	if snap.Track == nil || snap.Track.ID != "yt:a" || len(snap.Queue) != 1 {
		t.Fatalf("room after stale pick: %+v", snap)
	}
}

func TestAutoplaySkipGate(t *testing.T) {
	m := newTestManager(t)
	ctx := context.Background()
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if _, _, err := m.AutoplaySkip(ctx, 1); !errors.Is(err, ErrInvalid) {
		t.Fatalf("autoplay_skip with autoplay off: %v", err)
	}
	if _, err := m.SetAutoplay(1, true); err != nil {
		t.Fatalf("SetAutoplay: %v", err)
	}
	if _, err := m.SetLoop(1, 3); err != nil {
		t.Fatalf("SetLoop: %v", err)
	}
	m.SetPicker(func(_ context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		if !exclude["yt:a"] {
			t.Errorf("skip picker exclude missing last")
		}
		return mkTrack("yt:skip-pick", 180), nil
	})
	pick, prev, err := m.AutoplaySkip(ctx, 1)
	if err != nil || pick.ID != "yt:skip-pick" || prev.ID != "yt:a" {
		t.Fatalf("AutoplaySkip: %v %v %v", pick, prev, err)
	}
	snap, _ := m.Snapshot(1)
	if snap.Track == nil || snap.Track.ID != "yt:skip-pick" || snap.Loop != 0 {
		t.Fatalf("room after autoplay_skip: %+v", snap)
	}
	// No pick available: the mix is empty.
	m.SetPicker(func(_ context.Context, _ model.Track, _ map[string]bool) (model.Track, error) {
		return model.Track{}, nil
	})
	if _, _, err := m.AutoplaySkip(ctx, 1); !errors.Is(err, ErrNoPick) {
		t.Fatalf("autoplay_skip empty mix: %v", err)
	}
}

func TestStopKeepsShuffleAndVolumePrunesDefaultRooms(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	_ = m.Enqueue(1, mkTrack("yt:b", 100), 7)
	if _, err := m.SetShuffle(1, true); err != nil {
		t.Fatalf("SetShuffle: %v", err)
	}
	if _, err := m.SetVolume(1, 150); err != nil {
		t.Fatalf("SetVolume: %v", err)
	}
	if _, err := m.SetAutoplay(1, true); err != nil {
		t.Fatalf("SetAutoplay: %v", err)
	}
	snap, err := m.Stop(1)
	if err != nil || snap.Track != nil || len(snap.Queue) != 0 {
		t.Fatalf("Stop: %+v %v", snap, err)
	}
	// Shuffle and volume survive the stop; loop/autoplay reset.
	rs := m.rooms[1]
	if rs == nil || !rs.st.Shuffle || rs.st.Volume != 150 || rs.st.Autoplay || rs.st.Loop != 0 {
		t.Fatalf("room after stop: %+v", rs.st)
	}
	// Idempotent stop on an unknown chat: 200, empty snapshot.
	snap, err = m.Stop(99)
	if err != nil || snap.ChatID != 99 {
		t.Fatalf("Stop unknown: %+v %v", snap, err)
	}
	// A default room is dropped from the store on stop.
	m2 := newTestManager(t)
	mustPlay(t, m2, 5, mkTrack("yt:a", 100))
	if _, err := m2.Stop(5); err != nil {
		t.Fatalf("Stop: %v", err)
	}
	if _, found, _ := m2.store.GetRoom(5); found {
		t.Fatalf("default room was not pruned from the store")
	}
	if m2.HasRoom(5) {
		t.Fatalf("default room was not pruned from memory")
	}
}

func TestVolumeClampAndMute(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	snap, err := m.SetVolume(1, 500)
	if err != nil || snap.Volume != 200 {
		t.Fatalf("SetVolume 500: %+v %v", snap, err)
	}
	snap, err = m.SetVolume(1, -5)
	if err != nil || snap.Volume != 0 {
		t.Fatalf("SetVolume -5: %+v %v", snap, err)
	}
	snap, err = m.SetVolume(1, 120)
	if err != nil {
		t.Fatalf("SetVolume 120: %v", err)
	}
	snap, err = m.SetMuted(1, true)
	if err != nil || !snap.IsMuted || snap.Volume != 120 {
		t.Fatalf("SetMuted on: %+v %v", snap, err)
	}
	if m.rooms[1].st.MutedVolume == nil || *m.rooms[1].st.MutedVolume != 120 {
		t.Fatalf("muted_volume not stored: %+v", m.rooms[1].st.MutedVolume)
	}
	snap, err = m.SetMuted(1, false)
	if err != nil || snap.IsMuted {
		t.Fatalf("SetMuted off: %+v %v", snap, err)
	}
	if m.rooms[1].st.MutedVolume != nil {
		t.Fatalf("muted_volume not cleared")
	}
}

func TestSetAutoplayDisableClearsHistory(t *testing.T) {
	m := newTestManager(t)
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if _, err := m.SetAutoplay(1, true); err != nil {
		t.Fatalf("SetAutoplay: %v", err)
	}
	m.rooms[1].st.AutoplayHistory = []string{"yt:a", "yt:b"}
	snap, err := m.SetAutoplay(1, false)
	if err != nil || snap.Autoplay {
		t.Fatalf("SetAutoplay off: %+v %v", snap, err)
	}
	if len(m.rooms[1].st.AutoplayHistory) != 0 {
		t.Fatalf("history not cleared on disable")
	}
}

func TestRestoreComesBackPaused(t *testing.T) {
	dir := t.TempDir()
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	m1 := NewManager(st, 20)
	mustPlay(t, m1, 1, mkTrack("yt:a", 300))
	_ = m1.Enqueue(1, mkTrack("yt:b", 100), 7)
	time.Sleep(1100 * time.Millisecond)
	// Freeze the position with a mutation (Pause persists it):
	// position is only written to the store on mutation — there
	// is no ticker — so a kill mid-track restores the last
	// persisted position, not the kill-time position.
	paused, err := m1.Pause(1)
	if err != nil || paused.PositionSec < 1 {
		t.Fatalf("Pause before restart: %+v %v", paused, err)
	}
	if err := st.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	st2, err := store.Open(dir)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer st2.Close()
	m2 := NewManager(st2, 20)
	if err := m2.Restore(); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	snap, ok := m2.Snapshot(1)
	if !ok || !snap.IsPaused || snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("restored snapshot: %+v ok=%v", snap, ok)
	}
	if snap.PositionSec < 1 {
		t.Fatalf("restored position not frozen at capture: %d", snap.PositionSec)
	}
	if len(snap.Queue) != 1 || snap.Queue[0].Track.ID != "yt:b" {
		t.Fatalf("restored queue: %+v", snap.Queue)
	}
}

func TestRadioStartStop(t *testing.T) {
	m := newTestManager(t)
	radio := model.RadioState{ChatID: 1, Station: "groovesalad", StreamURL: "https://example.invalid/groove"}
	track := model.Track{ID: "radio:groovesalad", Title: "SomaFM Groove Salad", Source: "radio", StreamURL: radio.StreamURL, IsLive: true}
	snap, err := m.StartRadio(1, radio, track, 0)
	if err != nil || snap.Track == nil || snap.Track.ID != "radio:groovesalad" {
		t.Fatalf("StartRadio: %+v %v", snap, err)
	}
	if !m.IsRadioCurrent(1) {
		t.Fatalf("IsRadioCurrent false after start")
	}
	stored, found, err := m.store.GetRadio(1)
	if err != nil || !found || stored.Station != "groovesalad" {
		t.Fatalf("radio bucket: %+v %v %v", stored, found, err)
	}
	// A music play while the radio is current enqueues behind it
	// (Play never pre-empts); after a stop the music starts and
	// the radio state is gone.
	out, err := m.Play(1, mkTrack("yt:a", 100), 7)
	if err != nil || out.Started {
		t.Fatalf("Play during radio: started=%v err=%v", out.Started, err)
	}
	if _, err := m.Stop(1); err != nil {
		t.Fatalf("Stop: %v", err)
	}
	mustPlay(t, m, 1, mkTrack("yt:a", 100))
	if m.IsRadioCurrent(1) {
		t.Fatalf("radio still current after music track")
	}
	// Stop with no radio is an error.
	if _, _, err := m.StopRadio(1); !errors.Is(err, ErrNothingPlaying) {
		t.Fatalf("StopRadio without radio: %v", err)
	}
	// Stop while the radio track is current clears it.
	snap, err = m.StartRadio(1, radio, track, 0)
	if err != nil {
		t.Fatalf("StartRadio 2: %v", err)
	}
	_ = snap
	wasRadio, snap, err := m.StopRadio(1)
	if err != nil || !wasRadio || snap.Track != nil {
		t.Fatalf("StopRadio: %v %+v %v", wasRadio, snap, err)
	}
	if _, found, _ := m.store.GetRadio(1); found {
		t.Fatalf("radio bucket not cleared")
	}
}

func TestSnapshotQueueOnlyRoom(t *testing.T) {
	m := newTestManager(t)
	_ = m.Enqueue(1, mkTrack("yt:a", 100), 7)
	snap, ok := m.Snapshot(1)
	if !ok || snap.Track != nil || len(snap.Queue) != 1 {
		t.Fatalf("queue-only snapshot: %+v ok=%v", snap, ok)
	}
	if _, ok := m.Snapshot(99); ok {
		t.Fatalf("Snapshot unknown chat returned a room")
	}
}

// TestSetFX pins the fx seam: SetFX stores the preset + params
// through persistLocked (bbolt rooms bucket) and FX reads them
// back; the params map is copied on both sides, so later caller
// mutation cannot corrupt the room.
func TestSetFX(t *testing.T) {
	m := newTestManager(t)
	if _, _, ok := m.FX(1); ok {
		t.Fatalf("FX on unknown chat returned a room")
	}
	params := map[string]float64{"rate": 1.25}
	if _, err := m.SetFX(1, "nightcore", params); err != nil {
		t.Fatalf("SetFX: %v", err)
	}
	params["rate"] = 9 // mutating the caller's map must not leak in
	effect, got, ok := m.FX(1)
	if !ok || effect != "nightcore" || got["rate"] != 1.25 {
		t.Fatalf("FX = %q %v ok=%v", effect, got, ok)
	}
	got["rate"] = 9 // mutating the returned map must not leak in either
	if _, got2, _ := m.FX(1); got2["rate"] != 1.25 {
		t.Fatalf("FX params aliased the room: %v", got2)
	}
	// "original" overwrites like any other preset.
	if _, err := m.SetFX(1, "original", nil); err != nil {
		t.Fatalf("SetFX original: %v", err)
	}
	if effect, _, _ := m.FX(1); effect != "original" {
		t.Fatalf("FX after original = %q", effect)
	}
}

// TestDSPOptions proves the DSP seam reads the room's playback
// parameters as one set: speed, bass, volume, FX preset +
// parameters. A chat with no room gets the neutral defaults.
// The params map is a copy: mutating it must not touch the room.
func TestDSPOptions(t *testing.T) {
	m := newTestManager(t)
	got := m.DSPOptions(-100)
	if got.Speed != 1.0 || got.Bass != 0 || got.Volume != 100 || got.Effect != "" {
		t.Fatalf("DSPOptions with no room = %+v, want neutral defaults", got)
	}
	mustPlay(t, m, -100, mkTrack("yt:a", 300))
	if _, err := m.SetSpeed(-100, 1.5); err != nil {
		t.Fatalf("SetSpeed: %v", err)
	}
	if _, err := m.SetBass(-100, 40); err != nil {
		t.Fatalf("SetBass: %v", err)
	}
	if _, err := m.SetVolume(-100, 80); err != nil {
		t.Fatalf("SetVolume: %v", err)
	}
	if _, err := m.SetFX(-100, "nightcore", map[string]float64{"rate": 1.25}); err != nil {
		t.Fatalf("SetFX: %v", err)
	}
	got = m.DSPOptions(-100)
	if got.Speed != 1.5 || got.Bass != 40 || got.Volume != 80 || got.Effect != "nightcore" || got.EffectParams["rate"] != 1.25 {
		t.Fatalf("DSPOptions = %+v, want speed 1.5 bass 40 volume 80 nightcore rate 1.25", got)
	}
	got.EffectParams["rate"] = 9
	if _, params, _ := m.FX(-100); params["rate"] != 1.25 {
		t.Fatal("DSPOptions aliased the room's FX params map")
	}
}
