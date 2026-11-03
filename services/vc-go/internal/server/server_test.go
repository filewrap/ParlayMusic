package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/resolver"
	"parlay/vc-go/internal/runner"
)

func newTestServer(t *testing.T, res *resolver.Resolver) *Server {
	t.Helper()
	mgr := queue.NewManager(nil)
	return New(mgr, res, runner.Config{}, "")
}

func post(t *testing.T, s *Server, path, body string) (int, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	var decoded map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &decoded); err != nil {
		t.Fatalf("POST %s: response is not JSON: %v (body %q)", path, err, rec.Body.String())
	}
	return rec.Code, decoded
}

func get(t *testing.T, s *Server, path string) (int, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	var decoded map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &decoded); err != nil {
		t.Fatalf("GET %s: response is not JSON: %v (body %q)", path, err, rec.Body.String())
	}
	return rec.Code, decoded
}

// Phase 5 (A9): with no yt-dlp, /v1/play is 502 engine_unavailable
// and nothing is queued — no degraded placeholder track.
func TestPlayWithoutYtDlpIs502(t *testing.T) {
	s := newTestServer(t, &resolver.Resolver{YtDlp: ""})
	code, body := post(t, s, "/v1/play",
		`{"chat_id": 4242, "query": "https://www.youtube.com/watch?v=dQw4w9WgXcQ"}`)
	if code != http.StatusBadGateway {
		t.Fatalf("expected 502, got %d (%v)", code, body)
	}
	errBody, _ := body["error"].(map[string]any)
	if errBody["code"] != "engine_unavailable" {
		t.Fatalf("expected engine_unavailable, got %v", body)
	}
	code, body = get(t, s, "/v1/now-playing?chat_id=4242")
	if code != http.StatusNotFound {
		t.Fatalf("expected 404 (nothing queued), got %d (%v)", code, body)
	}
}

func TestHealthzReportsDegradedWithoutYtDlp(t *testing.T) {
	s := newTestServer(t, &resolver.Resolver{YtDlp: ""})
	code, body := get(t, s, "/healthz")
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d", code)
	}
	data, _ := body["data"].(map[string]any)
	if data["degraded"] != true || data["yt_dlp"] != false {
		t.Fatalf("healthz must report degraded:true, yt_dlp:false, got %v", body)
	}
}

// Offline: /v1/play with a stub yt-dlp carries the resolved
// stream_url at BOTH levels (A8 residual): nested track.stream_url
// and top-level NowPlaying.stream_url — on the play response and on
// a later GET /v1/now-playing.
func TestPlayWithStubYtDlpCarriesStreamURLBothLevels(t *testing.T) {
	script := filepath.Join(t.TempDir(), "yt-dlp")
	const info = `{"id":"dQw4w9WgXcQ","title":"Never Gonna Give You Up","duration":213,` +
		`"uploader":"Rick Astley","thumbnail":"https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg",` +
		`"url":"https://example.invalid/audio.webm",` +
		`"webpage_url":"https://www.youtube.com/watch?v=dQw4w9WgXcQ"}`
	if err := os.WriteFile(script, []byte("#!/bin/sh\nprintf '%s' '"+info+"'\n"), 0o755); err != nil {
		t.Fatalf("write stub yt-dlp: %v", err)
	}
	s := newTestServer(t, &resolver.Resolver{YtDlp: script})
	code, body := post(t, s, "/v1/play",
		`{"chat_id": 4244, "query": "https://www.youtube.com/watch?v=dQw4w9WgXcQ"}`)
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d (%v)", code, body)
	}
	data, _ := body["data"].(map[string]any)
	np, _ := data["now_playing"].(map[string]any)
	if np["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("play response must carry top-level stream_url, got %v", np)
	}
	track, _ := np["track"].(map[string]any)
	if track["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("play response must carry nested track.stream_url, got %v", track)
	}
	code, body = get(t, s, "/v1/now-playing?chat_id=4244")
	if code != http.StatusOK {
		t.Fatalf("expected 200 on now-playing, got %d (%v)", code, body)
	}
	data, _ = body["data"].(map[string]any)
	np, _ = data["now_playing"].(map[string]any)
	if np["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("GET now-playing must carry top-level stream_url, got %v", np)
	}
	track, _ = np["track"].(map[string]any)
	if track["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("GET now-playing must carry nested track.stream_url, got %v", track)
	}
}

// Live: /v1/play resolves a real YouTube track and the now-playing
// payload carries its stream_url end to end (A8). Gated on
// PARLAY_LIVE_TEST=1 (needs network + yt-dlp); tolerant of YouTube
// gating the egress IP — then the honest 502 is the assertion.
func TestPlayLiveYouTubeCarriesStreamURL(t *testing.T) {
	if os.Getenv("PARLAY_LIVE_TEST") == "" {
		t.Skip("live test: set PARLAY_LIVE_TEST=1 (needs network + yt-dlp)")
	}
	s := newTestServer(t, resolver.New())
	code, body := post(t, s, "/v1/play",
		`{"chat_id": 4243, "query": "https://www.youtube.com/watch?v=dQw4w9WgXcQ"}`)
	if code == http.StatusBadGateway {
		errBody, _ := body["error"].(map[string]any)
		if errBody["code"] != "engine_unavailable" {
			t.Fatalf("expected engine_unavailable on the honest failure, got %v", body)
		}
		t.Logf("YouTube gated right now (honest failure): %v", body)
		return
	}
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d (%v)", code, body)
	}
	data, _ := body["data"].(map[string]any)
	np, _ := data["now_playing"].(map[string]any)
	track, _ := np["track"].(map[string]any)
	streamURL, _ := track["stream_url"].(string)
	if !strings.HasPrefix(streamURL, "https://") {
		t.Fatalf("now-playing must carry the resolved stream_url, got %v", track)
	}
	if np["stream_url"] != streamURL {
		t.Fatalf("top-level stream_url must mirror track.stream_url, got %v", np)
	}
	code, body = get(t, s, "/v1/now-playing?chat_id=4243")
	if code != http.StatusOK {
		t.Fatalf("expected 200 on now-playing, got %d (%v)", code, body)
	}
	data, _ = body["data"].(map[string]any)
	np, _ = data["now_playing"].(map[string]any)
	track, _ = np["track"].(map[string]any)
	streamURL, _ = track["stream_url"].(string)
	if !strings.HasPrefix(streamURL, "https://") {
		t.Fatalf("GET now-playing must carry stream_url, got %v", track)
	}
	if np["stream_url"] != streamURL {
		t.Fatalf("GET now-playing top-level stream_url must mirror track.stream_url, got %v", np)
	}
}
