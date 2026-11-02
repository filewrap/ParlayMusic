package server

// Offline routing tests for the /v1/search contract route (B10, Phase 5
// Worker E). None of these touch the network: every assertion is on a
// validation/auth failure that happens before SearchSong is called.
import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
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
