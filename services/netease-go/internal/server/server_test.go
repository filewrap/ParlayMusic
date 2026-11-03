package server

// Offline routing tests for the /v1/search contract route (B10, Phase 5
// Worker E). None of these touch the network: every assertion is on a
// validation/auth failure that happens before SearchSong is called.
import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"parlay/netease-go/internal/netease"
)

func testServer(t *testing.T, token string) *Server {
	t.Helper()
	s, err := New(t.TempDir(), token)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return s
}

func post(t *testing.T, s *Server, path, body, token string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	if token != "" {
		req.Header.Set("X-Parlay-Token", token)
	}
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	return rec
}

func TestHealthzOK(t *testing.T) {
	s := testServer(t, "")
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("healthz: got %d, want 200: %s", rec.Code, rec.Body.String())
	}
	if !strings.Contains(rec.Body.String(), "netease-go") {
		t.Fatalf("healthz body missing service name: %s", rec.Body.String())
	}
}

func TestGenericSearchRejectsMissingQuery(t *testing.T) {
	s := testServer(t, "")
	for _, body := range []string{`{}`, `{"query":""}`, `{"query":"  "}`, `not json`} {
		rec := post(t, s, "/v1/search", body, "")
		if rec.Code != http.StatusBadRequest {
			t.Fatalf("/v1/search %s: got %d, want 400: %s", body, rec.Code, rec.Body.String())
		}
	}
}

func TestGenericSearchRejectsOtherSources(t *testing.T) {
	s := testServer(t, "")
	// The gate runs before any network call, so these are offline-safe.
	for _, source := range []string{"youtube", "spotify", "deezer", "lemonic"} {
		body := `{"query":"jay chou","source":"` + source + `"}`
		rec := post(t, s, "/v1/search", body, "")
		if rec.Code != http.StatusBadRequest {
			t.Fatalf("/v1/search source=%s: got %d, want 400: %s", source, rec.Code, rec.Body.String())
		}
		if !strings.Contains(rec.Body.String(), "netease-go") {
			t.Fatalf("/v1/search source=%s: error must name the service: %s", source, rec.Body.String())
		}
	}
}

func TestGenericSearchRequiresPost(t *testing.T) {
	s := testServer(t, "")
	req := httptest.NewRequest(http.MethodGet, "/v1/search", nil)
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /v1/search: got %d, want 405", rec.Code)
	}
}

func TestNeteaseSearchRejectsMissingQuery(t *testing.T) {
	s := testServer(t, "")
	rec := post(t, s, "/v1/netease/search", `{}`, "")
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("/v1/netease/search {}: got %d, want 400", rec.Code)
	}
}

func TestAuthRequiredWhenTokenSet(t *testing.T) {
	s := testServer(t, "sekrit")
	rec := post(t, s, "/v1/search", `{"query":"x"}`, "")
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("no token: got %d, want 401", rec.Code)
	}
	// Wrong source + right token: the auth passes, the source gate fires.
	rec = post(t, s, "/v1/search", `{"query":"x","source":"youtube"}`, "sekrit")
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("bad source with token: got %d, want 400", rec.Code)
	}
}

// ------------------------------------------------ Phase 9: IPC-3/5 --

// stubNetEase answers lyric/detail/search from canned JSON so the
// handler tests never touch the network.
func stubNetEase(t *testing.T) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch {
		case strings.HasPrefix(r.URL.Path, "/api/song/lyric"):
			if strings.Contains(r.URL.RawQuery, "id=999") {
				_, _ = w.Write([]byte(`{"code":200,"lrc":{"lyric":""}}`))
				return
			}
			_, _ = w.Write([]byte(`{"code":200,"lrc":{"lyric":"[00:01.00]hello\n[00:05.00]world"}}`))
		case strings.HasPrefix(r.URL.Path, "/api/song/detail"):
			_, _ = w.Write([]byte(`{"code":200,"songs":[{"id":186016,"name":"Test Song","dt":213000,"ar":[{"name":"Test Artist"}],"al":{"name":"Test Album","picUrl":"https://p1.music.126.net/cover.jpg"}}]}`))
		case strings.HasPrefix(r.URL.Path, "/api/cloudsearch/pc"):
			_, _ = w.Write([]byte(`{"code":200,"result":{"songCount":1,"songs":[{"id":186016,"name":"Test Song","dt":213000,"ar":[{"name":"Test Artist"}],"al":{"name":"Test Album","picUrl":"https://p1.music.126.net/cover.jpg"}}]}}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
}

func testServerWithStub(t *testing.T, stub *httptest.Server) *Server {
	t.Helper()
	s := testServer(t, "")
	s.client = netease.NewWithBase(stub.URL)
	return s
}

func TestLyricsIPC5ShapeAndLRCFile(t *testing.T) {
	stub := stubNetEase(t)
	defer stub.Close()
	s := testServerWithStub(t, stub)
	rec := post(t, s, "/v1/netease/lyrics", `{"track_id":"186016"}`, "")
	if rec.Code != http.StatusOK {
		t.Fatalf("lyrics: got %d, want 200: %s", rec.Code, rec.Body.String())
	}
	var env struct {
		Data map[string]any `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("lyrics body is not JSON: %v", err)
	}
	d := env.Data
	if d["track_id"] != "186016" {
		t.Fatalf("track_id = %v, want 186016", d["track_id"])
	}
	if d["title"] != "Test Song" || d["artist"] != "Test Artist" {
		t.Fatalf("title/artist = %v/%v", d["title"], d["artist"])
	}
	if d["source"] != "netease" {
		t.Fatalf("source = %v, want netease", d["source"])
	}
	if d["truncated"] != false {
		t.Fatalf("truncated = %v, want false", d["truncated"])
	}
	if d["lyrics"] != "[00:01.00]hello\n[00:05.00]world" {
		t.Fatalf("lyrics = %v", d["lyrics"])
	}
	lrcPath, _ := d["lrc_path"].(string)
	if lrcPath == "" || !strings.HasSuffix(lrcPath, "netease-186016.lrc") {
		t.Fatalf("lrc_path = %q, want netease-186016.lrc under the dl dir", lrcPath)
	}
	raw, err := os.ReadFile(lrcPath)
	if err != nil {
		t.Fatalf("lrc file unreadable: %v", err)
	}
	if string(raw) != "[00:01.00]hello\n[00:05.00]world" {
		t.Fatalf("lrc file content = %q", raw)
	}
}

func TestLyricsNoLyricsIs404(t *testing.T) {
	stub := stubNetEase(t)
	defer stub.Close()
	s := testServerWithStub(t, stub)
	rec := post(t, s, "/v1/netease/lyrics", `{"track_id":999}`, "")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("no-lyrics: got %d, want 404: %s", rec.Code, rec.Body.String())
	}
	if !strings.Contains(rec.Body.String(), "not_found") {
		t.Fatalf("no-lyrics error must be not_found: %s", rec.Body.String())
	}
}

func TestSearchCarriesIPC3(t *testing.T) {
	stub := stubNetEase(t)
	defer stub.Close()
	s := testServerWithStub(t, stub)
	rec := post(t, s, "/v1/search", `{"query":"test song","source":"netease"}`, "")
	if rec.Code != http.StatusOK {
		t.Fatalf("search: got %d, want 200: %s", rec.Code, rec.Body.String())
	}
	var env struct {
		Data struct {
			Results []map[string]any `json:"results"`
		} `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("search body is not JSON: %v", err)
	}
	if len(env.Data.Results) != 1 {
		t.Fatalf("results = %d, want 1", len(env.Data.Results))
	}
	tr := env.Data.Results[0]
	if tr["duration_str"] != "3:33" {
		t.Fatalf("duration_str = %v, want 3:33", tr["duration_str"])
	}
	if tr["thumbnail"] != "https://p1.music.126.net/cover.jpg" {
		t.Fatalf("thumbnail = %v", tr["thumbnail"])
	}
	if _, ok := tr["is_live"]; ok {
		t.Fatalf("is_live must be absent for a NetEase song: %v", tr)
	}
}
