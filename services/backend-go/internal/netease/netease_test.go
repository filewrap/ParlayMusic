package netease

import (
	"context"
	"encoding/json"
	"errors"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/resolver/netease"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// fakeLyrics is a stub lyricsFetcher for tests.
type fakeLyrics struct {
	result *lyrics.Result
	err    error
}

func (f *fakeLyrics) FetchNetEase(ctx context.Context, neteaseID string) (*lyrics.Result, error) {
	return f.result, f.err
}

func testService(t *testing.T) *Service {
	t.Helper()
	t.Setenv("PARLAY_DATA_DIR", t.TempDir())
	return &Service{
		registry: resolver.NewRegistry(),
		lyrics: &fakeLyrics{
			result: &lyrics.Result{
				Lyrics: "[00:01.00]la la\n[00:05.00]la\n",
				Source: "netease", TrackID: "123",
			},
		},
	}
}

func TestParseTrackID(t *testing.T) {
	cases := []struct {
		raw  string
		want int64
	}{
		{`"123"`, 123},
		{`123`, 123},
		{`"netease:456"`, 456},
		{`"https://music.163.com/song?id=789"`, 789},
	}
	for _, c := range cases {
		got, err := parseTrackID(json.RawMessage(c.raw))
		if err != nil || got != c.want {
			t.Fatalf("parseTrackID(%s) = %d, %v, want %d", c.raw, got, err, c.want)
		}
	}
	for _, raw := range []string{`"abc"`, `0`, `""`, ``} {
		if _, err := parseTrackID(json.RawMessage(raw)); err == nil {
			t.Fatalf("parseTrackID(%s) succeeded, want error", raw)
		}
	}
}

func TestSearchValidation(t *testing.T) {
	svc := testService(t)
	_, err := svc.handleSearch(t.Context(), uds.Request{Payload: json.RawMessage(`{"query":""}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("empty query: err=%v, want bad_request", err)
	}
	_, err = svc.handleSearch(t.Context(), uds.Request{Payload: json.RawMessage(`{}`)})
	if herr, ok := err.(*uds.HandlerError); !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("missing query: err=%v, want bad_request", err)
	}
}

func TestDownloadValidation(t *testing.T) {
	svc := testService(t)
	_, err := svc.handleDownload(t.Context(), uds.Request{Payload: json.RawMessage(`{}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("missing track_id: err=%v, want bad_request", err)
	}
	_, err = svc.handleDownload(t.Context(), uds.Request{Payload: json.RawMessage(`{"track_id":"abc"}`)})
	if herr, ok := err.(*uds.HandlerError); !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("non-numeric track_id: err=%v, want bad_request", err)
	}
}

func TestLyricsHappyPath(t *testing.T) {
	svc := testService(t)
	// Point the registry at a nil resolver path is not possible; the
	// lyrics path itself does not need the registry for the happy
	// path when title/artist lookup fails — it returns empty
	// title/artist and still returns the lyrics. The registry
	// Resolve will attempt the network; guard by using a fake that
	// fails fast: instead, verify via the fake lyrics + a registry
	// whose Resolve fails quickly (no network in the sandbox egress
	// would still try). To keep this deterministic, call the handler
	// with a service whose registry is nil for metadata but whose
	// lyrics fetcher is the fake.
	svc.registry = nil
	data, err := svc.handleLyrics(t.Context(), uds.Request{Payload: json.RawMessage(`{"track_id":"123"}`)})
	if err != nil {
		t.Fatalf("netease.lyrics: %v", err)
	}
	m := data.(map[string]any)
	if m["source"] != "netease" || m["track_id"] != "123" {
		t.Fatalf("lyrics response: %v", m)
	}
	if m["lyrics"] == nil || m["lyrics"] == "" {
		t.Fatalf("lyrics empty: %v", m)
	}
	if m["truncated"] != false {
		t.Fatalf("truncated must be false: %v", m)
	}
}

func TestLyricsNoLyricsIsNotFound(t *testing.T) {
	svc := testService(t)
	svc.lyrics = &fakeLyrics{err: netease.ErrNoLyrics}
	_, err := svc.handleLyrics(t.Context(), uds.Request{Payload: json.RawMessage(`{"track_id":123}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrNotFound {
		t.Fatalf("no-lyrics: err=%v, want not_found", err)
	}
}

func TestLyricsMissingTrackID(t *testing.T) {
	svc := testService(t)
	_, err := svc.handleLyrics(t.Context(), uds.Request{Payload: json.RawMessage(`{}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("missing track_id: err=%v, want bad_request", err)
	}
}

func TestIsNoLyrics(t *testing.T) {
	if !isNoLyrics(netease.ErrNoLyrics) {
		t.Fatalf("ErrNoLyrics not recognised")
	}
	// The shared Fetcher wraps both errors; either must be recognised.
	if !isNoLyrics(lyrics.ErrNoLyrics) {
		t.Fatalf("lyrics.ErrNoLyrics not recognised")
	}
	wrapped := errors.Join(netease.ErrNoLyrics, errors.New("for track 1"))
	_ = wrapped
	if !isNoLyrics(wrapped) {
		t.Fatalf("wrapped ErrNoLyrics not recognised")
	}
	if isNoLyrics(errors.New("other")) {
		t.Fatalf("unrelated error recognised as no-lyrics")
	}
}

func TestLyricsNilFetcherIsEngineUnavailable(t *testing.T) {
	svc := testService(t)
	svc.lyrics = nil
	_, err := svc.handleLyrics(t.Context(), uds.Request{Payload: json.RawMessage(`{"track_id":"123"}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrEngineUnavailable {
		t.Fatalf("nil lyrics: err=%v, want engine_unavailable", err)
	}
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	Register(server, &app.Deps{Store: st, Registry: resolver.NewRegistry()})
}
