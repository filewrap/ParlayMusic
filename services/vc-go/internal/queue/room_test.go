package queue

import (
	"testing"

	"parlay/vc-go/internal/types"
)

func track(id, title string) types.Track {
	return types.Track{ID: id, Title: title, Source: "youtube"}
}

func TestPlayStartsImmediatelyThenEnqueues(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(-100123)
	np := r.Play(track("yt:a", "Song A"), 1)
	if np.Track == nil || np.Track.ID != "yt:a" {
		t.Fatalf("first play should start immediately, got %+v", np.Track)
	}
	np = r.Play(track("yt:b", "Song B"), 2)
	if np.Track.ID != "yt:a" || len(np.Queue) != 1 || np.Queue[0].Track.ID != "yt:b" {
		t.Fatalf("second play should enqueue, got %+v", np)
	}
}

func TestPauseResumeFreezesPosition(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Pause()
	p1 := r.Position()
	if p1 != 0 {
		t.Fatalf("paused position should be 0, got %d", p1)
	}
	np := r.Resume()
	if np.Paused {
		t.Fatalf("resume should clear paused")
	}
}

func TestSkipAdvancesAndDrains(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Enqueue(track("yt:b", "B"), 2)
	np, ok := r.Skip()
	if !ok || np.Track.ID != "yt:b" {
		t.Fatalf("skip should advance to B, got %+v ok=%v", np.Track, ok)
	}
	np, ok = r.Skip()
	if !ok || np.Track != nil {
		t.Fatalf("skip at end should idle the room, got %+v ok=%v", np.Track, ok)
	}
}

func TestRemoveAndClear(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Enqueue(track("yt:b", "B"), 2)
	r.Enqueue(track("yt:c", "C"), 3)
	if _, ok := r.Remove(5); ok {
		t.Fatalf("out-of-range remove should fail")
	}
	np, ok := r.Remove(0)
	if !ok || len(np.Queue) != 1 || np.Queue[0].Track.ID != "yt:c" {
		t.Fatalf("remove(0) should drop B, got %+v", np.Queue)
	}
	np = r.Clear()
	if len(np.Queue) != 0 || np.Track.ID != "yt:a" {
		t.Fatalf("clear should empty queue but keep current, got %+v", np)
	}
}

func TestSeekClamps(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	tr := track("yt:a", "A")
	tr.DurationSec = 100
	r.Play(tr, 1)
	np, ok := r.SeekTo(500)
	if !ok || np.PositionSec != 100 {
		t.Fatalf("seek past duration should clamp to 100, got %d", np.PositionSec)
	}
	np, _ = r.SeekTo(-5)
	if np.PositionSec != 0 {
		t.Fatalf("negative seek should clamp to 0, got %d", np.PositionSec)
	}
}

func TestPersistRestoreComesBackPaused(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(42)
	r.Play(track("yt:a", "A"), 7)
	r.Enqueue(track("yt:b", "B"), 8)
	p := r.Persist()
	r2 := Restore(p)
	np := r2.Snapshot()
	if !np.Paused {
		t.Fatalf("restored room must come back paused")
	}
	if np.Track == nil || np.Track.ID != "yt:a" || len(np.Queue) != 1 {
		t.Fatalf("restore lost state: %+v", np)
	}
}

func TestLoopReplaysThenAdvances(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Enqueue(track("yt:b", "B"), 2)
	r.SetLoop(2)
	np := r.Advance()
	if np.Track == nil || np.Track.ID != "yt:a" || np.Loop != 1 {
		t.Fatalf("loop 2 -> replay A with loop 1, got %+v", np)
	}
	np = r.Advance()
	if np.Track == nil || np.Track.ID != "yt:a" || np.Loop != 0 {
		t.Fatalf("loop 1 -> replay A with loop 0, got %+v", np)
	}
	np = r.Advance()
	if np.Track == nil || np.Track.ID != "yt:b" {
		t.Fatalf("loop exhausted -> advance to B, got %+v", np.Track)
	}
}

func TestLoopClampsTo0_10(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	if np := r.SetLoop(99); np.Loop != MaxLoop {
		t.Fatalf("loop 99 should clamp to %d, got %d", MaxLoop, np.Loop)
	}
	if np := r.SetLoop(-3); np.Loop != 0 {
		t.Fatalf("loop -3 should clamp to 0, got %d", np.Loop)
	}
}

func TestSkipResetsLoop(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Enqueue(track("yt:b", "B"), 2)
	r.SetLoop(3)
	np, ok := r.Skip()
	if !ok || np.Track == nil || np.Track.ID != "yt:b" || np.Loop != 0 {
		t.Fatalf("skip should reset loop and advance to B, got %+v ok=%v", np, ok)
	}
}

func TestShufflePicksEveryQueuedTrackOnce(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	for _, id := range []string{"yt:b", "yt:c", "yt:d", "yt:e", "yt:f"} {
		r.Enqueue(track(id, id), 2)
	}
	r.SetShuffle(true)
	seen := map[string]bool{}
	for i := 0; i < 5; i++ {
		np := r.Advance()
		if np.Track == nil {
			t.Fatalf("advance %d: room went idle early", i)
		}
		if seen[np.Track.ID] {
			t.Fatalf("shuffle replayed %s", np.Track.ID)
		}
		seen[np.Track.ID] = true
	}
	if len(seen) != 5 {
		t.Fatalf("shuffle should visit all 5 queued tracks, saw %v", seen)
	}
	if np := r.Advance(); np.Track != nil {
		t.Fatalf("after all 5, room should idle, got %+v", np.Track)
	}
}

func TestAutoplayPicksOnDrainAndSkipsHistory(t *testing.T) {
	m := NewManager(nil)
	picks := []types.Track{track("yt:x", "X"), track("yt:y", "Y")}
	var gotExclude []map[string]bool
	r := m.Get(1)
	r.SetAutoplayPicker(func(last types.Track, exclude map[string]bool) (*types.Track, error) {
		cp := make(map[string]bool, len(exclude))
		for k, v := range exclude {
			cp[k] = v
		}
		gotExclude = append(gotExclude, cp)
		if len(picks) == 0 {
			return nil, nil
		}
		next := picks[0]
		picks = picks[1:]
		return &next, nil
	})
	r.Play(track("yt:a", "A"), 1)
	r.SetAutoplay(true)
	np := r.Advance()
	if np.Track == nil || np.Track.ID != "yt:x" {
		t.Fatalf("drain should autoplay X, got %+v", np.Track)
	}
	np = r.Advance()
	if np.Track == nil || np.Track.ID != "yt:y" {
		t.Fatalf("drain should autoplay Y next, got %+v", np.Track)
	}
	// The second pick must have excluded the first autoplayed track
	// (it is both the last track and in history).
	if len(gotExclude) < 2 || !gotExclude[1]["yt:x"] {
		t.Fatalf("second pick exclude set wrong: %v", gotExclude)
	}
	np = r.Advance()
	if np.Track != nil {
		t.Fatalf("no more picks -> room should idle, got %+v", np.Track)
	}
}

func TestAutoplayOffIdlesOnDrain(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.SetAutoplayPicker(func(last types.Track, exclude map[string]bool) (*types.Track, error) {
		return &types.Track{ID: "yt:x", Title: "X", Source: "youtube"}, nil
	})
	r.Play(track("yt:a", "A"), 1)
	np := r.Advance()
	if np.Track != nil {
		t.Fatalf("autoplay off -> drain should idle, got %+v", np.Track)
	}
}

func TestStopResetsLoopAndAutoplayKeepsShuffle(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.SetLoop(2)
	r.SetShuffle(true)
	r.SetAutoplay(true)
	np := r.Stop()
	if np.Loop != 0 || np.Autoplay || !np.Shuffle {
		t.Fatalf("stop should reset loop+autoplay, keep shuffle: %+v", np)
	}
}

func TestPersistRestoreKeepsLoopShuffleAutoplay(t *testing.T) {
	m := NewManager(nil)
	r := m.Get(1)
	r.Play(track("yt:a", "A"), 1)
	r.Enqueue(track("yt:b", "B"), 2)
	r.SetLoop(3)
	r.SetShuffle(true)
	r.SetAutoplay(true)
	p := r.Persist()
	r2 := Restore(p)
	np := r2.Snapshot()
	if np.Loop != 3 || !np.Shuffle || !np.Autoplay {
		t.Fatalf("restore lost loop/shuffle/autoplay: %+v", np)
	}
}
