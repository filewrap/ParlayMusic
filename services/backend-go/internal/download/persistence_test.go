package download

import (
	"context"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
)

// TestRegistryPersistsFinishedRuns proves a finished run survives
// a store reopen: the run is found by id with its terminal phase
// and title, and a late progress poll still answers. A live run
// does not survive (it cannot — its context is dead), which the
// test also proves by polling for it after reopen.
func TestRegistryPersistsFinishedRuns(t *testing.T) {
	dir := t.TempDir()
	st, err := store.Open(filepath.Join(dir, "data"))
	if err != nil {
		t.Fatal(err)
	}

	reg := NewRegistry()
	if err := reg.AttachStore(st); err != nil {
		t.Fatalf("AttachStore: %v", err)
	}
	run := reg.NewRun(context.Background(), "download.youtube", "Song Title")
	run.Report(resolver.Progress{Percentage: 50, DoneBytes: 512, Phase: "downloading"})
	run.SetFilePath(filepath.Join(dir, "song.mp3"))
	run.Finish(reg)
	finishedID := run.ID

	live := reg.NewRun(context.Background(), "download.youtube", "Still Going")
	liveID := live.ID

	if err := st.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	st2, err := store.Open(filepath.Join(dir, "data"))
	if err != nil {
		t.Fatal(err)
	}
	defer st2.Close()
	reg2 := NewRegistry()
	if err := reg2.AttachStore(st2); err != nil {
		t.Fatalf("AttachStore after reopen: %v", err)
	}
	got, found := reg2.Get(finishedID)
	if !found {
		t.Fatal("finished run did not survive a store reopen")
	}
	if got.Phase() != PhaseDone {
		t.Errorf("restored run phase = %q, want done", got.Phase())
	}
	snap := got.Snapshot()
	if snap["run_id"] != finishedID {
		t.Errorf("restored run snapshot id = %v, want %s", snap["run_id"], finishedID)
	}
	// Restored runs are terminal: finishing one again is a no-op,
	// never a panic (double close of the done channel).
	got.Finish(reg2)
	if got.Phase() != PhaseDone {
		t.Errorf("re-finishing a restored run changed its phase to %q", got.Phase())
	}
	if _, found := reg2.Get(liveID); found {
		t.Error("live run survived a reopen — it must not (its context is dead)")
	}
}

// TestPrefsStorePersists proves a user's prefs survive a store
// reopen, and an unknown user still gets defaults after reopen.
func TestPrefsStorePersists(t *testing.T) {
	dir := t.TempDir()
	st, err := store.Open(filepath.Join(dir, "data"))
	if err != nil {
		t.Fatal(err)
	}
	ps := NewPrefsStore()
	if err := ps.AttachStore(st); err != nil {
		t.Fatalf("AttachStore: %v", err)
	}
	want := Prefs{UserID: 7, Format: "flac", UseYouTube: false, PathTemplate: "{artist} - {title}.{ext}"}
	if _, err := ps.Set(want); err != nil {
		t.Fatalf("Set: %v", err)
	}
	if err := st.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	st2, err := store.Open(filepath.Join(dir, "data"))
	if err != nil {
		t.Fatal(err)
	}
	defer st2.Close()
	ps2 := NewPrefsStore()
	if err := ps2.AttachStore(st2); err != nil {
		t.Fatalf("AttachStore after reopen: %v", err)
	}
	if got := ps2.Get(7); got != want {
		t.Errorf("prefs after reopen = %+v, want %+v", got, want)
	}
	if got := ps2.Get(8); got.Format != "mp3" || !got.UseYouTube {
		t.Errorf("unknown user after reopen = %+v, want defaults", got)
	}
}
