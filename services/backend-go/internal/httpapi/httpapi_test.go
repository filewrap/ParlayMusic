package httpapi

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/gui"
	"parlay/backend-go/internal/handlers"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// testEnv is one wired httpapi server over a real store, room
// Manager, UDS server (gui.* + vc.* registered) and library
// index — the same wiring main() builds, minus the socket listen.
type testEnv struct {
	srv    *Server
	rooms  *room.Manager
	index  *library.Index
	voiceM *voice.Manager
}

func newTestEnv(t *testing.T, staticDir string) *testEnv {
	return newTestEnvWithJoin(t, staticDir, nil)
}

func newTestEnvWithJoin(t *testing.T, staticDir string, join voice.JoinFunc) *testEnv {
	t.Helper()
	dir := t.TempDir()
	tomlPath := filepath.Join(dir, "parlay.toml")
	if err := os.WriteFile(tomlPath, []byte("[backend]\ndata_dir = "+fmt.Sprintf("%q", dir)+"\n"), 0o600); err != nil {
		t.Fatalf("write parlay.toml: %v", err)
	}
	cfg, err := config.LoadFrom(tomlPath)
	if err != nil {
		t.Fatalf("config.LoadFrom: %v", err)
	}
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	reg := resolver.NewRegistry()
	t.Cleanup(func() { _ = reg.Close() })
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	t.Cleanup(client.Close)
	server := uds.NewServer(filepath.Join(dir, "parlay-backend.sock"))
	voiceMgr := voice.NewManager(client, join)
	t.Cleanup(voiceMgr.Close)
	rooms := room.NewManager(st, cfg.Limits.MaxQueueSize)
	lyr, err := lyrics.New(reg)
	if err != nil {
		t.Fatalf("lyrics.New: %v", err)
	}
	deps := app.New(st, cfg, reg, voiceMgr, server, rooms, lyr)
	gui.Register(server, deps)
	handlers.Register(server, deps)
	index := library.NewIndex()
	srv := New(server, rooms, index, staticDir)
	return &testEnv{srv: srv, rooms: rooms, index: index, voiceM: voiceMgr}
}

// renderAudio renders a real audio file with ffmpeg (sine tone)
// so the stream tests exercise genuine codec bytes, never
// hand-made fixtures. ext is "flac" or "wav".
func renderAudio(t *testing.T, ext string, seconds int) (path string, data []byte) {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not in PATH")
	}
	path = filepath.Join(t.TempDir(), fmt.Sprintf("tone-%ds.%s", seconds, ext))
	cmd := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", fmt.Sprintf("sine=frequency=440:duration=%d", seconds),
		"-ar", "48000", "-ac", "2", path)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg render %s failed: %v: %s", ext, err, out)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read rendered %s: %v", ext, err)
	}
	if len(data) < 100 {
		t.Fatalf("rendered %s suspiciously small: %d bytes", ext, len(data))
	}
	return path, data
}

func hashBytes(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

func (e *testEnv) get(t *testing.T, path string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	rec := httptest.NewRecorder()
	e.srv.Handler().ServeHTTP(rec, req)
	return rec
}

func (e *testEnv) post(t *testing.T, path, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	e.srv.Handler().ServeHTTP(rec, req)
	return rec
}

func decodeBody(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var v map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &v); err != nil {
		t.Fatalf("response is not JSON (%d): %v: %.200q", rec.Code, err, rec.Body.String())
	}
	return v
}

func playTrack(t *testing.T, e *testEnv, chatID int64, track model.Track) {
	t.Helper()
	if _, err := e.rooms.Play(chatID, track, 7); err != nil {
		t.Fatalf("rooms.Play: %v", err)
	}
}

var flacTrack = model.Track{ID: "test:flac-tone", Title: "FLAC Tone", Artist: "Test", Source: "test", DurationSec: 5, StreamURL: "https://example.invalid/flac-tone"}

func TestHealthz(t *testing.T) {
	e := newTestEnv(t, "")
	rec := e.get(t, "/api/v1/healthz")
	if rec.Code != http.StatusOK {
		t.Fatalf("healthz status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body := decodeBody(t, rec)
	if body["ok"] != true || body["service"] != "backend-go" {
		t.Fatalf("healthz body: %v", body)
	}
}

func TestStreamFLACFullBodyByteIdentical(t *testing.T) {
	e := newTestEnv(t, "")
	flacPath, flacData := renderAudio(t, "flac", 5)
	if !bytes.HasPrefix(flacData, []byte{'f', 'L', 'a', 'C'}) {
		t.Fatalf("rendered file does not start with the FLAC magic")
	}
	playTrack(t, e, -1001, flacTrack)
	e.index.Record(flacTrack, flacPath, int64(len(flacData)))

	rec := e.get(t, "/api/v1/stream/-1001/flac")
	if rec.Code != http.StatusOK {
		t.Fatalf("full stream status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	if ct := rec.Header().Get("Content-Type"); ct != "audio/flac" {
		t.Errorf("Content-Type=%q, want audio/flac", ct)
	}
	if ar := rec.Header().Get("Accept-Ranges"); ar != "bytes" {
		t.Errorf("Accept-Ranges=%q, want bytes", ar)
	}
	if rec.Header().Get("Content-Length") != fmt.Sprintf("%d", len(flacData)) {
		t.Errorf("Content-Length=%q, want %d", rec.Header().Get("Content-Length"), len(flacData))
	}
	got := rec.Body.Bytes()
	if hashBytes(got) != hashBytes(flacData) {
		t.Fatalf("full stream body hash differs from the cached FLAC file (got %d bytes, want %d)", len(got), len(flacData))
	}
	t.Logf("full FLAC stream: %d bytes, sha256 %s, byte-identical to the cached file", len(got), hashBytes(got))
}

func TestStreamFLACRanges(t *testing.T) {
	e := newTestEnv(t, "")
	flacPath, flacData := renderAudio(t, "flac", 5)
	playTrack(t, e, -1001, flacTrack)
	e.index.Record(flacTrack, flacPath, int64(len(flacData)))
	size := len(flacData)

	cases := []struct {
		name     string
		rangeHdr string
		wantFrom int
		wantTo   int // inclusive
	}{
		{"first bytes", "bytes=0-99", 0, 99},
		{"middle range", "bytes=1000-1999", 1000, 1999},
		{"open-ended range", fmt.Sprintf("bytes=%d-", size-1000), size - 1000, size - 1},
		{"last byte", fmt.Sprintf("bytes=%d-%d", size-1, size-1), size - 1, size - 1},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/api/v1/stream/-1001/flac", nil)
			req.Header.Set("Range", tc.rangeHdr)
			rec := httptest.NewRecorder()
			e.srv.Handler().ServeHTTP(rec, req)
			if rec.Code != http.StatusPartialContent {
				t.Fatalf("range %q status=%d, want 206: %s", tc.rangeHdr, rec.Code, rec.Body.String())
			}
			wantRange := fmt.Sprintf("bytes %d-%d/%d", tc.wantFrom, tc.wantTo, size)
			if got := rec.Header().Get("Content-Range"); got != wantRange {
				t.Errorf("Content-Range=%q, want %q", got, wantRange)
			}
			want := flacData[tc.wantFrom : tc.wantTo+1]
			if !bytes.Equal(rec.Body.Bytes(), want) {
				t.Fatalf("range %q body differs from source bytes [%d:%d]", tc.rangeHdr, tc.wantFrom, tc.wantTo+1)
			}
			t.Logf("range %q: 206, %d bytes, identical to source", tc.rangeHdr, len(want))
		})
	}

	// Out-of-range: 416 with the size in Content-Range.
	req := httptest.NewRequest(http.MethodGet, "/api/v1/stream/-1001/flac", nil)
	req.Header.Set("Range", fmt.Sprintf("bytes=%d-", size+100))
	rec := httptest.NewRecorder()
	e.srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusRequestedRangeNotSatisfiable {
		t.Fatalf("out-of-range status=%d, want 416: %s", rec.Code, rec.Body.String())
	}
	if got := rec.Header().Get("Content-Range"); got != fmt.Sprintf("bytes */%d", size) {
		t.Errorf("416 Content-Range=%q, want bytes */%d", got, size)
	}
	t.Logf("out-of-range: 416, Content-Range bytes */%d", size)
}

func TestStreamFLACErrorsAreHonest(t *testing.T) {
	e := newTestEnv(t, "")

	// Nothing playing in the chat.
	rec := e.get(t, "/api/v1/stream/-1001/flac")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("no room: status=%d, want 404", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "nothing playing in this chat" {
		t.Fatalf("no room: error=%v, want 'nothing playing in this chat'", body["error"])
	}

	// Track playing, but never downloaded: no library entry.
	playTrack(t, e, -1002, flacTrack)
	rec = e.get(t, "/api/v1/stream/-1002/flac")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("no cached file: status=%d, want 404", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "no cached file for the current track" {
		t.Fatalf("no cached file: error=%v", body["error"])
	}

	// Track cached, but the cached file is not FLAC (WAV).
	wavPath, _ := renderAudio(t, "wav", 2)
	e.index.Record(flacTrack, wavPath, 0)
	rec = e.get(t, "/api/v1/stream/-1002/flac")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("non-FLAC cache: status=%d, want 404", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "current track has no cached FLAC file" {
		t.Fatalf("non-FLAC cache: error=%v", body["error"])
	}

	// A .flac name whose bytes are not FLAC at all.
	fakePath := filepath.Join(t.TempDir(), "fake.flac")
	if err := os.WriteFile(fakePath, []byte("this is not a flac file, just text"), 0o600); err != nil {
		t.Fatal(err)
	}
	e.index.Record(flacTrack, fakePath, 0)
	rec = e.get(t, "/api/v1/stream/-1002/flac")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("fake FLAC bytes: status=%d, want 404", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "cached file for the current track is not FLAC" {
		t.Fatalf("fake FLAC bytes: error=%v", body["error"])
	}

	// A malformed chat id in the path is a 400, not a 404.
	rec = e.get(t, "/api/v1/stream/not-a-chat/flac")
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("bad chat id: status=%d, want 400", rec.Code)
	}
}

func TestStateEndpointsReuseGuiActions(t *testing.T) {
	e := newTestEnv(t, "")
	playTrack(t, e, -1001, model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube", DurationSec: 200, StreamURL: "https://example.invalid/stream"})
	if err := e.rooms.Enqueue(-1001, model.Track{ID: "yt:def", Title: "Next", Artist: "B", Source: "youtube"}, 1); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}

	rec := e.get(t, "/api/v1/now-playing?chat_id=-1001")
	if rec.Code != http.StatusOK {
		t.Fatalf("now-playing status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body := decodeBody(t, rec)
	np, _ := body["now_playing"].(map[string]any)
	if np == nil {
		t.Fatalf("now-playing body has no now_playing object: %v", body)
	}
	track, _ := np["track"].(map[string]any)
	if track["id"] != "yt:abc" {
		t.Fatalf("now-playing track id=%v, want yt:abc", track["id"])
	}
	if np["volume"] != float64(100) {
		t.Fatalf("now-playing volume=%v, want 100", np["volume"])
	}
	queue, _ := np["queue"].([]any)
	if len(queue) != 1 {
		t.Fatalf("now-playing queue len=%d, want 1", len(queue))
	}

	rec = e.get(t, "/api/v1/queue?chat_id=-1001")
	if rec.Code != http.StatusOK {
		t.Fatalf("queue status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body = decodeBody(t, rec)
	items, _ := body["queue"].([]any)
	if len(items) != 1 {
		t.Fatalf("queue len=%d, want 1", len(items))
	}

	// Unknown chat: the gui.* 404 rule, verbatim.
	rec = e.get(t, "/api/v1/now-playing?chat_id=-9999")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("unknown chat now-playing status=%d, want 404", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "nothing playing in this chat" {
		t.Fatalf("unknown chat error=%v", body["error"])
	}

	// Missing chat_id: 400.
	rec = e.get(t, "/api/v1/now-playing")
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("missing chat_id status=%d, want 400", rec.Code)
	}
}

func TestControlEndpointsReuseVcActions(t *testing.T) {
	e := newTestEnv(t, "")
	playTrack(t, e, -1001, model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube", DurationSec: 200, StreamURL: "https://example.invalid/stream"})

	// Pause: room pause, no voice call involved (same as vc.pause
	// with no call record).
	rec := e.post(t, "/api/v1/pause", `{"chat_id":-1001}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("pause status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body := decodeBody(t, rec)
	np, _ := body["now_playing"].(map[string]any)
	if np["is_paused"] != true {
		t.Fatalf("pause did not pause: %v", np)
	}

	// Resume with a current track but no voice call: vc.resume's
	// honest 502, naming the join that is missing.
	rec = e.post(t, "/api/v1/resume", `{"chat_id":-1001}`)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("resume without a call status=%d, want 502: %s", rec.Code, rec.Body.String())
	}
	body = decodeBody(t, rec)
	if !strings.Contains(fmt.Sprint(body["error"]), "use vc.join first") {
		t.Fatalf("resume without a call error=%v, want it to name vc.join", body["error"])
	}

	// Volume: the vc.volume range rule applies unchanged.
	rec = e.post(t, "/api/v1/volume", `{"chat_id":-1001,"volume":150}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("volume status=%d, want 200: %s", rec.Code, rec.Body.String())
	}
	body = decodeBody(t, rec)
	np, _ = body["now_playing"].(map[string]any)
	if np["volume"] != float64(150) {
		t.Fatalf("volume=%v, want 150", np["volume"])
	}
	rec = e.post(t, "/api/v1/volume", `{"chat_id":-1001,"volume":201}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("volume 201 status=%d, want 200 (clamped): %s", rec.Code, rec.Body.String())
	}
	body = decodeBody(t, rec)
	np, _ = body["now_playing"].(map[string]any)
	if np["volume"] != float64(200) {
		t.Fatalf("volume 201 clamped to %v, want 200", np["volume"])
	}
	rec = e.post(t, "/api/v1/volume", `{"chat_id":-1001}`)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("volume missing status=%d, want 400", rec.Code)
	}
	if body := decodeBody(t, rec); body["error"] != "volume is required" {
		t.Fatalf("volume missing error=%v", body["error"])
	}

	// Skip with a current track but no voice call: vc.skip's
	// ensureCall fails honestly (502), it never pretends to skip.
	rec = e.post(t, "/api/v1/skip", `{"chat_id":-1001}`)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("skip without a call status=%d, want 502: %s", rec.Code, rec.Body.String())
	}
}

func TestStaticServing(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "index.html"), []byte("<!doctype html><title>Parlay</title>"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "app.js"), []byte("console.log('parlay');"), 0o600); err != nil {
		t.Fatal(err)
	}
	e := newTestEnv(t, dir)
	rec := e.get(t, "/")
	if rec.Code != http.StatusOK {
		t.Fatalf("GET / status=%d, want 200", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "Parlay") {
		t.Fatalf("GET / body does not contain the bundle index: %.100q", rec.Body.String())
	}
	rec = e.get(t, "/app.js")
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "parlay") {
		t.Fatalf("GET /app.js status=%d body=%.100q", rec.Code, rec.Body.String())
	}
	if cc := rec.Header().Get("Cache-Control"); cc != "no-cache" {
		t.Errorf("static Cache-Control=%q, want no-cache", cc)
	}

	// No bundle built: an honest 503 naming the build, never
	// demo data.
	e2 := newTestEnv(t, "")
	rec = e2.get(t, "/")
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("no bundle GET / status=%d, want 503", rec.Code)
	}
	if body := decodeBody(t, rec); body["code"] != "miniapp_not_built" {
		t.Fatalf("no bundle error code=%v, want miniapp_not_built", body["code"])
	}
}

func TestStreamDoesNotDependOnStatic(t *testing.T) {
	// The stream endpoint works with no Mini App bundle at all.
	e := newTestEnv(t, "")
	flacPath, flacData := renderAudio(t, "flac", 5)
	playTrack(t, e, -1001, flacTrack)
	e.index.Record(flacTrack, flacPath, int64(len(flacData)))
	rec := e.get(t, "/api/v1/stream/-1001/flac")
	if rec.Code != http.StatusOK {
		t.Fatalf("stream without bundle status=%d, want 200", rec.Code)
	}
	if hashBytes(rec.Body.Bytes()) != hashBytes(flacData) {
		t.Fatal("stream without bundle is not byte-identical to the cached FLAC")
	}
}

// TestBodyTooLarge is a guard on parseChatID's 1 MiB body cap:
// a POST body over the cap is a 400, not a hang or a crash.
func TestOversizePostBodyRejected(t *testing.T) {
	e := newTestEnv(t, "")
	huge := `{"chat_id":-1001,"pad":"` + strings.Repeat("x", 1<<20) + `"}`
	rec := e.post(t, "/api/v1/pause", huge)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("oversize body status=%d, want 400", rec.Code)
	}
}
