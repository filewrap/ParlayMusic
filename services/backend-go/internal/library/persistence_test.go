package library

import (
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
)

// TestIndexPersists proves a library entry survives a store reopen
// (the file still on disk), and that an entry whose file vanished
// while the backend was down is dropped on load, not resurrected.
func TestIndexPersists(t *testing.T) {
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")
	file := filepath.Join(dir, "song.mp3")
	if err := os.WriteFile(file, []byte("fake audio"), 0o644); err != nil {
		t.Fatal(err)
	}
	gone := filepath.Join(dir, "gone.mp3")
	if err := os.WriteFile(gone, []byte("fake audio"), 0o644); err != nil {
		t.Fatal(err)
	}

	st, err := store.Open(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	idx := NewIndex()
	if err := idx.AttachStore(st); err != nil {
		t.Fatalf("AttachStore: %v", err)
	}
	idx.Record(model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube"}, file, 1234)
	idx.Record(model.Track{ID: "yt:gone", Title: "Gone", Artist: "A", Source: "youtube"}, gone, 10)
	if err := os.Remove(gone); err != nil {
		t.Fatal(err)
	}
	if err := st.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	st2, err := store.Open(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	defer st2.Close()
	idx2 := NewIndex()
	if err := idx2.AttachStore(st2); err != nil {
		t.Fatalf("AttachStore after reopen: %v", err)
	}
	if e, ok := idx2.Get("yt:abc"); !ok || e.SizeBytes != 1234 {
		t.Errorf("entry did not survive a store reopen: ok=%v entry=%+v", ok, e)
	}
	if _, ok := idx2.Get("yt:gone"); ok {
		t.Error("entry whose file vanished was resurrected after reopen")
	}
	if _, found, _ := st2.GetLibraryEntry("yt:gone"); found {
		t.Error("stale entry was not deleted from the store on load")
	}
	// Delete persists too.
	idx2.Delete("yt:abc")
	if _, found, _ := st2.GetLibraryEntry("yt:abc"); found {
		t.Error("deleted entry still in the store")
	}
}
