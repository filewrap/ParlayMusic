package search

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// lemonicTestServer serves a canned Lemonic API: /api/search,
// /api/raw/track/<id>, and a stream URL host for downloads.
func lemonicTestServer(t *testing.T) (*httptest.Server, *httptest.Server) {
	t.Helper()
	stream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "fake-mp3-bytes")
	}))
	t.Cleanup(stream.Close)
	api := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/api/search":
			// Bearer key is honoured when set (keyed path).
			if r.Header.Get("Authorization") != "" && r.Header.Get("Authorization") != "Bearer test-key" {
				w.WriteHeader(http.StatusUnauthorized)
				return
			}
			fmt.Fprint(w, `{"results":[{"id":"s1","title":"Song One","artist":"Singer A","duration_sec":210,"url":"https://example.com/s1","thumbnail":"https://example.com/s1.jpg"}]}`)
		case strings.HasPrefix(r.URL.Path, "/api/raw/track/"):
			id := strings.TrimPrefix(r.URL.Path, "/api/raw/track/")
			switch id {
			case "no-url":
				fmt.Fprintf(w, `{"id":%q,"title":"No URL Track","artist":"Nobody"}`, id)
			case "missing":
				fmt.Fprint(w, `{}`)
			default:
				fmt.Fprintf(w, `{"id":%q,"title":"Raw Track","artist":"Singer A","duration_ms":200000,"stream_url":%q}`, id, stream.URL+"/audio.mp3")
			}
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(api.Close)
	return api, stream
}

func TestLemonicSearchHappy(t *testing.T) {
	api, _ := lemonicTestServer(t)
	c := NewLemonicClientWithBase(api.URL, "")
	tracks, via, err := c.Search(context.Background(), nil, "song one", 5)
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if via != "lemonic" {
		t.Fatalf("via = %q, want lemonic (no key set)", via)
	}
	if len(tracks) != 1 {
		t.Fatalf("tracks = %+v", tracks)
	}
	tr := tracks[0]
	if tr.ID != "lemonic:s1" || tr.Title != "Song One" || tr.Artist != "Singer A" || tr.DurationSec != 210 || tr.Source != "lemonic" {
		t.Fatalf("track = %+v", tr)
	}
	if tr.Thumbnail != "https://example.com/s1.jpg" {
		t.Fatalf("thumbnail = %q", tr.Thumbnail)
	}
}

func TestLemonicSearchKeyed(t *testing.T) {
	api, _ := lemonicTestServer(t)
	c := NewLemonicClientWithBase(api.URL, "test-key")
	if !c.Configured() {
		t.Fatal("key set should report Configured")
	}
	tracks, via, err := c.Search(context.Background(), nil, "song one", 5)
	if err != nil {
		t.Fatalf("keyed Search: %v", err)
	}
	if via != "lemonic-keyed" {
		t.Fatalf("via = %q, want lemonic-keyed", via)
	}
	if len(tracks) != 1 {
		t.Fatalf("tracks = %+v", tracks)
	}
}

func TestLemonicSearchFallbackError(t *testing.T) {
	// Lemonic down + no registry: the error names both attempts.
	c := NewLemonicClientWithBase("http://127.0.0.1:1", "")
	_, _, err := c.Search(context.Background(), nil, "song", 5)
	if err == nil {
		t.Fatal("Search with everything down should fail")
	}
	if !strings.Contains(err.Error(), "lemonic /api/search") || !strings.Contains(err.Error(), "spotify search") {
		t.Fatalf("fallback error should name both attempts: %v", err)
	}
}

func TestLemonicRawTrack(t *testing.T) {
	api, _ := lemonicTestServer(t)
	c := NewLemonicClientWithBase(api.URL, "")
	// The "lemonic:" prefix is stripped for the raw-track call.
	track, raw, err := c.RawTrack(context.Background(), "lemonic:abc123")
	if err != nil {
		t.Fatalf("RawTrack: %v", err)
	}
	if track.ID != "lemonic:abc123" || track.Title != "Raw Track" || track.DurationSec != 200 {
		t.Fatalf("track = %+v", track)
	}
	if raw["stream_url"] == nil {
		t.Fatalf("raw = %#v (want stream_url)", raw)
	}
	// A record with no id and no title is "no raw track".
	if _, _, err := c.RawTrack(context.Background(), "missing"); err == nil {
		t.Fatal("RawTrack for an empty record should fail")
	}
	// An empty id is a client error, not a fetch.
	if _, _, err := c.RawTrack(context.Background(), "lemonic:"); err == nil {
		t.Fatal("RawTrack with an empty id should fail")
	}
}

func TestHandleLemonicSearch(t *testing.T) {
	api, _ := lemonicTestServer(t)
	svc, _, _ := testService(t, NewLemonicClientWithBase(api.URL, ""))
	got, err := svc.handleLemonicSearch(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"query": "song one"})})
	if err != nil {
		t.Fatalf("lemonic.search: %v", err)
	}
	m := got.(map[string]any)
	if m["via"] != "lemonic" {
		t.Fatalf("via = %v", m["via"])
	}
	results := m["results"].([]model.Track)
	if len(results) != 1 || results[0].ID != "lemonic:s1" {
		t.Fatalf("results = %#v", m["results"])
	}
	_, err = svc.handleLemonicSearch(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("empty query: code = %v", err)
	}
}

func TestHandleLemonicDownload(t *testing.T) {
	api, _ := lemonicTestServer(t)
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")
	cfg := &config.Config{}
	cfg.Backend.DataDir = dataDir
	deps := &app.Deps{Config: cfg}
	index := library.NewIndex()
	svc := NewServiceWithLemonic(deps, NewLemonicClientWithBase(api.URL, ""), index)
	ctx := context.Background()
	// track_id is required.
	_, err := svc.handleLemonicDownload(ctx, uds.Request{Payload: mustJSON(t, map[string]any{})})
	if errCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("missing track_id: code = %v", err)
	}
	// No stream URL -> 502 engine_unavailable.
	_, err = svc.handleLemonicDownload(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"track_id": "lemonic:no-url"})})
	if errCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("no stream URL: code = %v, want engine_unavailable", err)
	}
	// Happy path: the file lands under data/lemonic, the library
	// index records it, the run finishes done.
	got, err := svc.handleLemonicDownload(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"track_id": "lemonic:abc123"})})
	if err != nil {
		t.Fatalf("lemonic.download: %v", err)
	}
	m := got.(map[string]any)
	filePath, _ := m["file_path"].(string)
	if filePath != filepath.Join(dataDir, "lemonic", "abc123.mp3") {
		t.Fatalf("file_path = %q", filePath)
	}
	if m["size_bytes"] != int64(len("fake-mp3-bytes")) {
		t.Fatalf("size_bytes = %v", m["size_bytes"])
	}
	content, err := os.ReadFile(filePath)
	if err != nil || string(content) != "fake-mp3-bytes" {
		t.Fatalf("file content = %q err=%v", content, err)
	}
	track, ok := m["track"].(model.Track)
	if !ok || track.ID != "lemonic:abc123" || track.SizeBytes != int64(len("fake-mp3-bytes")) {
		t.Fatalf("track = %#v", m["track"])
	}
	if _, ok := index.Get("lemonic:abc123"); !ok {
		t.Fatal("downloaded track was not recorded in the library index")
	}
	// The run is pollable via download.progress's registry.
	runID, _ := m["run_id"].(string)
	if runID == "" {
		t.Fatal("run_id missing from the response")
	}
}
