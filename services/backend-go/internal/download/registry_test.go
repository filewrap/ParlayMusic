package download

import (
	"context"
	"testing"
	"time"

	"parlay/backend-go/internal/resolver"
)

func TestRegistryCancelLiveRun(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.youtube", "test track")
	cancelled, phase, found := r.Cancel(run.ID)
	if !found {
		t.Fatal("Cancel: run not found")
	}
	if !cancelled {
		t.Fatal("Cancel: live run should report cancelled=true")
	}
	if phase != PhaseFailed {
		t.Fatalf("Cancel: phase = %q, want %q", phase, PhaseFailed)
	}
	// The run's context is cancelled: providers see it.
	select {
	case <-run.Context().Done():
	case <-time.After(time.Second):
		t.Fatal("Cancel: run context was not cancelled")
	}
}

func TestRegistryCancelFinishedRun(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.youtube", "done track")
	run.Finish(r)
	cancelled, phase, found := r.Cancel(run.ID)
	if !found {
		t.Fatal("Cancel: finished run not found")
	}
	if cancelled {
		t.Fatal("Cancel: finished run should report cancelled=false (200, not an error)")
	}
	if phase != PhaseDone {
		t.Fatalf("Cancel: phase = %q, want %q", phase, PhaseDone)
	}
}

func TestRegistryCancelUnknownRun(t *testing.T) {
	r := NewRegistry()
	_, _, found := r.Cancel("deadbeefdead")
	if found {
		t.Fatal("Cancel: unknown run should report found=false")
	}
	if _, ok := r.Get("deadbeefdead"); ok {
		t.Fatal("Get: unknown run should not be found")
	}
}

func TestRegistryFinishForces100(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.youtube", "x")
	run.Report(progressForTest(42))
	run.Finish(r)
	snap := run.Snapshot()
	if snap["percentage"] != 100.0 {
		t.Fatalf("Finish: percentage = %v, want 100", snap["percentage"])
	}
	if snap["phase"] != string(PhaseDone) {
		t.Fatalf("Finish: phase = %v, want done", snap["phase"])
	}
	// A finished run is still pollable (late polls).
	if _, ok := r.Get(run.ID); !ok {
		t.Fatal("Get: finished run should still be retrievable")
	}
}

func TestRegistryFailReason(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.soulseek", "x")
	run.Fail(r, "cancelled by user")
	if run.Phase() != PhaseFailed {
		t.Fatalf("Fail: phase = %q, want failed", run.Phase())
	}
	if run.ErrReason() != "cancelled by user" {
		t.Fatalf("Fail: reason = %q", run.ErrReason())
	}
}

func TestRegistryFinishedCap(t *testing.T) {
	r := NewRegistry()
	var first *Run
	for i := 0; i < MaxFinishedRuns+5; i++ {
		run := r.NewRun(context.Background(), "download.youtube", "x")
		if i == 0 {
			first = run
		}
		run.Finish(r)
	}
	if _, ok := r.Get(first.ID); ok {
		t.Fatal("registry should evict the oldest finished run past the 200 cap")
	}
	if got := len(r.finished); got != MaxFinishedRuns {
		t.Fatalf("finished runs = %d, want %d", got, MaxFinishedRuns)
	}
	// Live runs are never evicted by the cap.
	live := r.NewRun(context.Background(), "download.youtube", "live")
	for i := 0; i < MaxFinishedRuns+5; i++ {
		run := r.NewRun(context.Background(), "download.youtube", "x")
		run.Finish(r)
	}
	if _, ok := r.Get(live.ID); !ok {
		t.Fatal("live run must never be evicted")
	}
}

func TestRegistryPerTrackChecklist(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.spotify_playlist", "pl")
	run.SetTitles([]string{"Song A", "Song B"})
	run.SetTrack("Song A", "done", 100, nil, nil)
	reason := "peer rejected"
	run.SetTrack("Song B", "failed", 0, nil, &reason)
	snap := run.Snapshot()
	tracks, ok := snap["per_track"].([]TrackProgress)
	if !ok || len(tracks) != 2 {
		t.Fatalf("per_track = %#v, want 2 entries", snap["per_track"])
	}
	if tracks[0].State != "done" || tracks[0].Pct != 100 {
		t.Fatalf("Song A entry = %+v", tracks[0])
	}
	if tracks[1].State != "failed" || tracks[1].Reason == nil || *tracks[1].Reason != reason {
		t.Fatalf("Song B entry = %+v", tracks[1])
	}
}

func TestRegistryReportProgress(t *testing.T) {
	r := NewRegistry()
	run := r.NewRun(context.Background(), "download.youtube", "x")
	total := int64(1000)
	eta := int64(7)
	run.ProgressFunc()(progressForTestFull(50, 500, &total, 1024, &eta))
	snap := run.Snapshot()
	if snap["percentage"] != 50.0 {
		t.Fatalf("percentage = %v, want 50", snap["percentage"])
	}
	if snap["phase"] != string(PhaseDownloading) {
		t.Fatalf("phase = %v, want downloading", snap["phase"])
	}
	if snap["done_bytes"] != int64(500) {
		t.Fatalf("done_bytes = %v, want 500", snap["done_bytes"])
	}
}

func progressForTest(pct float64) resolver.Progress {
	return resolver.Progress{Phase: "downloading", Percentage: pct}
}

func progressForTestFull(pct float64, done int64, total *int64, speed float64, eta *int64) resolver.Progress {
	return resolver.Progress{
		Phase: "downloading", Percentage: pct, DoneBytes: done,
		TotalBytes: total, SpeedBytesPerSec: speed, ETASeconds: eta,
	}
}
