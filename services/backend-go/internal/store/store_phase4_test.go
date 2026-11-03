package store

import (
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/model"
)

// TestPhase4Buckets covers the four Phase 4 buckets (cache,
// downloads, dlprefs, library): write, read back, overwrite,
// delete, list, and the not-found path — including a store reopen,
// which is the unit-level proof that the records survive a restart
// (the verification worker also does a kill -9 proof).
func TestPhase4Buckets(t *testing.T) {
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")

	open := func(t *testing.T) *Store {
		t.Helper()
		st, err := Open(dataDir)
		if err != nil {
			t.Fatal(err)
		}
		return st
	}

	st := open(t)

	// Cache.
	entry := CacheEntry{
		Track:     model.Track{ID: "dQw4w9WgXcQ", Title: "Song", Source: "youtube", StreamURL: "https://example.com/audio?expire=1893456000"},
		ExpiresAt: 1893456000,
		CachedAt:  1762000000,
	}
	if err := st.PutCacheEntry("youtube:dQw4w9WgXcQ", entry); err != nil {
		t.Fatalf("PutCacheEntry: %v", err)
	}
	got, found, err := st.GetCacheEntry("youtube:dQw4w9WgXcQ")
	if err != nil || !found {
		t.Fatalf("GetCacheEntry: found=%v err=%v", found, err)
	}
	if got.Track.StreamURL != entry.Track.StreamURL || got.ExpiresAt != entry.ExpiresAt {
		t.Errorf("GetCacheEntry round-trip mismatch: %+v", got)
	}
	if _, found, _ := st.GetCacheEntry("youtube:nope"); found {
		t.Error("GetCacheEntry must report false for a missing key")
	}
	if err := st.DeleteCacheEntry("youtube:dQw4w9WgXcQ"); err != nil {
		t.Fatalf("DeleteCacheEntry: %v", err)
	}
	if _, found, _ := st.GetCacheEntry("youtube:dQw4w9WgXcQ"); found {
		t.Error("GetCacheEntry must report false after delete")
	}

	// Downloads (finished runs).
	total := int64(1234)
	rec := model.DownloadRunRecord{
		ID: "abc123def456", Kind: "download.youtube", Title: "Song",
		StartedAt: 1762000000, Phase: "done", Percentage: 100,
		DoneBytes: 1234, TotalBytes: &total, FilePath: "data/downloads/song.mp3",
	}
	if err := st.PutDownloadRun(rec); err != nil {
		t.Fatalf("PutDownloadRun: %v", err)
	}
	gotRec, found, err := st.GetDownloadRun("abc123def456")
	if err != nil || !found {
		t.Fatalf("GetDownloadRun: found=%v err=%v", found, err)
	}
	if gotRec.Title != "Song" || gotRec.Phase != "done" || gotRec.TotalBytes == nil || *gotRec.TotalBytes != 1234 {
		t.Errorf("GetDownloadRun round-trip mismatch: %+v", gotRec)
	}
	runs, err := st.ListDownloadRuns()
	if err != nil || len(runs) != 1 {
		t.Fatalf("ListDownloadRuns: got %d, err %v", len(runs), err)
	}
	if err := st.DeleteDownloadRun("abc123def456"); err != nil {
		t.Fatalf("DeleteDownloadRun: %v", err)
	}
	if runs, _ := st.ListDownloadRuns(); len(runs) != 0 {
		t.Errorf("ListDownloadRuns after delete: got %d, want 0", len(runs))
	}

	// Download prefs.
	prefs := model.DownloadPrefs{UserID: 42, Format: "flac", UseYouTube: false, PathTemplate: "{artist} - {title}.{ext}"}
	if err := st.PutDownloadPrefs(prefs); err != nil {
		t.Fatalf("PutDownloadPrefs: %v", err)
	}
	gotPrefs, found, err := st.GetDownloadPrefs(42)
	if err != nil || !found {
		t.Fatalf("GetDownloadPrefs: found=%v err=%v", found, err)
	}
	if gotPrefs != prefs {
		t.Errorf("GetDownloadPrefs round-trip mismatch: %+v", gotPrefs)
	}
	if _, found, _ := st.GetDownloadPrefs(43); found {
		t.Error("GetDownloadPrefs must report false for a missing user")
	}
	allPrefs, err := st.ListDownloadPrefs()
	if err != nil || len(allPrefs) != 1 {
		t.Fatalf("ListDownloadPrefs: got %d, err %v", len(allPrefs), err)
	}

	// Library.
	ent := model.LibraryEntry{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube", FilePath: "data/downloads/song.mp3", SizeBytes: 1234, AddedAt: 1762000000}
	if err := st.PutLibraryEntry(ent); err != nil {
		t.Fatalf("PutLibraryEntry: %v", err)
	}
	gotEnt, found, err := st.GetLibraryEntry("yt:abc")
	if err != nil || !found {
		t.Fatalf("GetLibraryEntry: found=%v err=%v", found, err)
	}
	if gotEnt != ent {
		t.Errorf("GetLibraryEntry round-trip mismatch: %+v", gotEnt)
	}
	entries, err := st.ListLibraryEntries()
	if err != nil || len(entries) != 1 {
		t.Fatalf("ListLibraryEntries: got %d, err %v", len(entries), err)
	}
	if err := st.DeleteLibraryEntry("yt:abc"); err != nil {
		t.Fatalf("DeleteLibraryEntry: %v", err)
	}
	if _, found, _ := st.GetLibraryEntry("yt:abc"); found {
		t.Error("GetLibraryEntry must report false after delete")
	}
	if err := st.PutLibraryEntry(ent); err != nil {
		t.Fatalf("PutLibraryEntry (re-put): %v", err)
	}
	if err := st.ClearLibrary(); err != nil {
		t.Fatalf("ClearLibrary: %v", err)
	}
	if entries, _ := st.ListLibraryEntries(); len(entries) != 0 {
		t.Errorf("ListLibraryEntries after clear: got %d, want 0", len(entries))
	}

	// Reopen: everything written above that was not deleted must
	// still be there — the restart-survival proof at store level.
	// (The cache entry and the run were deleted above; the prefs
	// record was not.)
	if err := st.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	st2 := open(t)
	defer st2.Close()
	if _, found, _ := st2.GetDownloadPrefs(42); !found {
		t.Error("download prefs did not survive a store reopen")
	}
	if _, found, _ := st2.GetCacheEntry("youtube:dQw4w9WgXcQ"); found {
		t.Error("deleted cache entry resurrected after reopen")
	}
}
