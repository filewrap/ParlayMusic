package library

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

func testService(t *testing.T) (*Service, string) {
	t.Helper()
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		t.Fatal(err)
	}
	cfg := &config.Config{}
	cfg.Backend.DataDir = dataDir
	deps := &app.Deps{Config: cfg}
	return NewServiceWithIndex(deps, NewIndex()), dataDir
}

func mustJSON(t *testing.T, v any) json.RawMessage {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return b
}

func errCode(t *testing.T, err error) uds.ErrorCode {
	t.Helper()
	var herr *uds.HandlerError
	if !errors.As(err, &herr) {
		t.Fatalf("error is not a HandlerError: %v", err)
	}
	return herr.Code
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestIndexRecordGetStale(t *testing.T) {
	idx := NewIndex()
	track := model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube"}
	path := filepath.Join(t.TempDir(), "song.mp3")
	writeFile(t, path, "audio")
	idx.Record(track, path, 5)
	entry, ok := idx.Get("yt:abc")
	if !ok || entry.SizeBytes != 5 || entry.FilePath != path {
		t.Fatalf("Get = %+v ok=%v", entry, ok)
	}
	// The bare id resolves through the yt: variant lookup.
	if _, ok := idx.Get("abc"); !ok {
		t.Fatal("bare id should resolve via the yt: variant")
	}
	// Delete the file: the stale entry is dropped lazily on read.
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if _, ok := idx.Get("yt:abc"); ok {
		t.Fatal("stale entry (file gone) should be dropped on read")
	}
	if idx.Len() != 0 {
		t.Fatalf("index len = %d after stale drop, want 0", idx.Len())
	}
}

func TestIndexSearch(t *testing.T) {
	idx := NewIndex()
	dir := t.TempDir()
	for _, tc := range []struct{ id, title, artist string }{
		{"yt:1", "Lag Ja Gale", "Lata Mangeshkar"},
		{"yt:2", "Tere Bina Zindagi", "Lata Mangeshkar"},
		{"yt:3", "Dum Maro Dum", "Asha Bhosle"},
	} {
		p := filepath.Join(dir, tc.id+".mp3")
		writeFile(t, p, "x")
		idx.Record(model.Track{ID: tc.id, Title: tc.title, Artist: tc.artist, Source: "youtube"}, p, 1)
	}
	if got := idx.Search("lata"); len(got) != 2 {
		t.Fatalf("search 'lata' = %d entries, want 2", len(got))
	}
	if got := idx.Search("DUM MARO"); len(got) != 1 {
		t.Fatalf("search is case-insensitive substring, got %d", len(got))
	}
	if got := idx.Search(""); len(got) != 3 {
		t.Fatalf("empty search = %d, want all 3", len(got))
	}
}

func TestHandleDelete(t *testing.T) {
	svc, dataDir := testService(t)
	ctx := context.Background()
	// Unknown id -> 404.
	_, err := svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"id": "yt:nope"})})
	if errCode(t, err) != uds.ErrNotFound {
		t.Fatalf("unknown id: code = %v, want not_found", err)
	}
	// No id or path -> bad_request.
	_, err = svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("no id/path: code = %v, want bad_request", err)
	}
	// Path traversal -> bad_request.
	_, err = svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"path": "../escape.mp3"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("traversal: code = %v, want bad_request", err)
	}
	// Top-level directory -> bad_request.
	_, err = svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"path": dataDir})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("top-level dir: code = %v, want bad_request", err)
	}
	// Record + file: both deleted.
	path := filepath.Join(dataDir, "downloads", "song.mp3")
	writeFile(t, path, "audio")
	svc.index.Record(model.Track{ID: "yt:del1", Title: "Song", Source: "youtube"}, path, 5)
	got, err := svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"id": "yt:del1"})})
	if err != nil {
		t.Fatalf("delete: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["deleted"] != true {
		t.Fatalf("delete = %#v", got)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("file was not deleted with its record")
	}
	if _, ok := svc.index.Get("yt:del1"); ok {
		t.Fatal("record was not deleted")
	}
	// Record gone but file exists -> the file is deleted, deleted:true.
	path2 := filepath.Join(dataDir, "downloads", "orphan.mp3")
	writeFile(t, path2, "audio")
	got, err = svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"path": path2})})
	if err != nil {
		t.Fatalf("delete orphan file: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["deleted"] != true {
		t.Fatalf("delete orphan = %#v", got)
	}
	if _, err := os.Stat(path2); !os.IsNotExist(err) {
		t.Fatal("orphan file was not deleted")
	}
	// File already gone but record exists -> record deleted, deleted:true.
	path3 := filepath.Join(dataDir, "downloads", "gone.mp3")
	writeFile(t, path3, "audio")
	svc.index.Record(model.Track{ID: "yt:del3", Title: "Gone", Source: "youtube"}, path3, 5)
	if err := os.Remove(path3); err != nil {
		t.Fatal(err)
	}
	got, err = svc.handleDelete(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"id": "yt:del3"})})
	if err != nil {
		t.Fatalf("delete stale record: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["deleted"] != true {
		t.Fatalf("delete stale = %#v", got)
	}
}

func TestHandleCacheClear(t *testing.T) {
	svc, dataDir := testService(t)
	ctx := context.Background()
	// Unknown name -> 200 {cleared:false} ("Song not cached"), not an error.
	got, err := svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "track", "name": "No Such Song"})})
	if err != nil {
		t.Fatalf("unknown name: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["cleared"] != false {
		t.Fatalf("unknown name = %#v", got)
	}
	// Bad scope -> bad_request.
	_, err = svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "everything"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("bad scope: code = %v, want bad_request", err)
	}
	// scope track without a name -> bad_request.
	_, err = svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "track"})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("track scope without name: code = %v", err)
	}
	// Two cached entries; clear one by title, then all.
	p1 := filepath.Join(dataDir, "a.mp3")
	p2 := filepath.Join(dataDir, "b.mp3")
	writeFile(t, p1, "a")
	writeFile(t, p2, "b")
	svc.index.Record(model.Track{ID: "yt:a", Title: "Song A", Source: "youtube"}, p1, 1)
	svc.index.Record(model.Track{ID: "yt:b", Title: "Song B", Source: "youtube"}, p2, 1)
	got, err = svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "track", "name": "Song A"})})
	if err != nil {
		t.Fatalf("clear one: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["cleared"] != true {
		t.Fatalf("clear one = %#v", got)
	}
	if _, err := os.Stat(p1); !os.IsNotExist(err) {
		t.Fatal("cleared entry's file was not deleted")
	}
	if _, ok := svc.index.Get("yt:a"); ok {
		t.Fatal("cleared entry's record was not deleted")
	}
	if _, ok := svc.index.Get("yt:b"); !ok {
		t.Fatal("the other entry must survive a single-track clear")
	}
	got, err = svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "all"})})
	if err != nil {
		t.Fatalf("clear all: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["cleared"] != true {
		t.Fatalf("clear all = %#v", got)
	}
	if names, ok := got.(map[string]any)["names"].([]string); !ok || len(names) != 1 || names[0] != "Song B" {
		t.Fatalf("clear all names = %#v", got)
	}
	if svc.index.Len() != 0 {
		t.Fatalf("index len after clear all = %d", svc.index.Len())
	}
	if _, err := os.Stat(p2); !os.IsNotExist(err) {
		t.Fatal("clear all must delete the files too")
	}
	// Clearing an already-empty cache reports cleared:false (200).
	got, err = svc.handleCacheClear(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"scope": "all"})})
	if err != nil {
		t.Fatalf("clear empty: %v", err)
	}
	if m, ok := got.(map[string]any); !ok || m["cleared"] != false {
		t.Fatalf("clear empty = %#v", got)
	}
}

func TestUnderDataDir(t *testing.T) {
	svc, dataDir := testService(t)
	if !svc.underDataDir(filepath.Join(dataDir, "downloads", "x.mp3")) {
		t.Fatal("file under the data dir should be under it")
	}
	if svc.underDataDir(dataDir) {
		t.Fatal("the data dir itself is not under itself")
	}
	if svc.underDataDir(filepath.Join(dataDir, "..", "escape.mp3")) {
		t.Fatal("a path escaping the data dir must be rejected")
	}
	if svc.underDataDir("/etc/passwd") {
		t.Fatal("an absolute outside path must be rejected")
	}
}
