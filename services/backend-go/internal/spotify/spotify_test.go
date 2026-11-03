package spotify

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

const testOAuthKey = "BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=" // 32 bytes of 0x07, base64

func testService(t *testing.T) (*Service, *store.Store, string) {
	t.Helper()
	t.Setenv("SPOTIFY_POLL_INTERVAL_SECS", "0")
	t.Setenv("GENIUS_ACCESS_TOKEN", "")
	t.Setenv("MUSIXMATCH_API_KEY", "")
	t.Setenv("GEMINI_API_KEY", "")
	t.Setenv("SPOTIFY_CLIENT_ID", "")
	t.Setenv("SPOTIFY_CLIENT_SECRET", "")
	t.Setenv("SPOTIFY_REDIRECT_URI", "")
	dir := t.TempDir()
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	cfg := &config.Config{}
	cfg.Backend.DataDir = dir
	cfg.Backend.SpotifyOAuthKey = testOAuthKey
	svc := newService(&app.Deps{Store: st, Config: cfg, Registry: resolver.NewRegistry()})
	return svc, st, dir
}

func mustSaveToken(t *testing.T, svc *Service, userID int64, access, refresh string, expiresAt int64) {
	t.Helper()
	if err := svc.saveUserToken(userID, &userToken{accessToken: access, refreshToken: refresh, expiresAt: expiresAt}); err != nil {
		t.Fatalf("saveUserToken: %v", err)
	}
}

// --- crypto -----------------------------------------------------------

func TestEncryptDecryptRoundTrip(t *testing.T) {
	svc, st, dir := testService(t)
	mustSaveToken(t, svc, 123, "access-abc", "refresh-xyz", time.Now().Unix()+3600)
	got, found, err := svc.loadUserToken(123)
	if err != nil || !found {
		t.Fatalf("loadUserToken: found=%v err=%v", found, err)
	}
	if got.accessToken != "access-abc" || got.refreshToken != "refresh-xyz" {
		t.Fatalf("round-trip mismatch: %+v", got)
	}
	// The raw token must never appear on disk (bbolt file + key file).
	raw, err := os.ReadFile(filepath.Join(dir, "parlay.db"))
	if err != nil {
		t.Fatalf("read db: %v", err)
	}
	if strings.Contains(string(raw), "access-abc") || strings.Contains(string(raw), "refresh-xyz") {
		t.Fatalf("plaintext token found in parlay.db")
	}
	keyRaw, err := os.ReadFile(filepath.Join(dir, "spotify-user-key"))
	if err != nil {
		// Config key wins over the file: no key file is expected
		// when PARLAY_SPOTIFY_OAUTH_KEY is set.
		if !os.IsNotExist(err) {
			t.Fatalf("read key file: %v", err)
		}
	} else if strings.Contains(string(keyRaw), "access-abc") {
		t.Fatalf("plaintext token found in key file")
	}
	_ = st
}

func TestEncryptDecryptWrongUserFails(t *testing.T) {
	svc, _, _ := testService(t)
	mustSaveToken(t, svc, 111, "access-abc", "refresh-xyz", time.Now().Unix()+3600)
	// Same blob, different user: the AAD binds the record to user 111.
	if _, found, _ := svc.loadUserToken(222); found {
		t.Fatalf("token for user 111 decrypted as user 222 (AAD not enforced)")
	}
}

func TestOAuthKeyFileGenerated0600(t *testing.T) {
	dir := t.TempDir()
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	cfg := &config.Config{}
	cfg.Backend.DataDir = dir
	svc := newService(&app.Deps{Store: st, Config: cfg, Registry: resolver.NewRegistry()})
	mustSaveToken(t, svc, 9, "tok", "ref", time.Now().Unix()+3600)
	info, err := os.Stat(filepath.Join(dir, "spotify-user-key"))
	if err != nil {
		t.Fatalf("key file not created: %v", err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("key file mode = %o, want 0600", info.Mode().Perm())
	}
	if _, found, err := svc.loadUserToken(9); err != nil || !found {
		t.Fatalf("token saved under a generated key did not load back: found=%v err=%v", found, err)
	}
}

// --- wordlist ---------------------------------------------------------

func TestProfanityWordlistCount(t *testing.T) {
	if len(profanityWords) != 54 {
		t.Fatalf("wordlist has %d words, want 54 (spotify-rust PROFANITY)", len(profanityWords))
	}
}

func TestProfanityWordlist(t *testing.T) {
	cases := []struct {
		text string
		want []string
	}{
		{"Highway to Hell", nil},
		{"Imagine — no hell below us", nil},
		{"class shipment", nil},
		{"shit and fuck and shit again, bitch", []string{"shit", "fuck", "bitch"}},
		{"What The FUCK Is This", []string{"fuck"}},
		{"", nil},
	}
	for _, c := range cases {
		got := profanityWordsIn(c.text)
		if len(got) != len(c.want) {
			t.Fatalf("profanityWordsIn(%q) = %v, want %v", c.text, got, c.want)
		}
		for i := range got {
			if got[i] != c.want[i] {
				t.Fatalf("profanityWordsIn(%q) = %v, want %v", c.text, got, c.want)
			}
		}
	}
}

// --- classifier --------------------------------------------------------

func TestClassifyNoKeyNoLyricsUnknown(t *testing.T) {
	svc, _, _ := testService(t)
	r := svc.classifyTrack(t.Context(), "La La Land", "Clean Artist", nil)
	if r.verdict != "unknown" || r.provider != "none" {
		t.Fatalf("classifyTrack no-key/no-lyrics = %+v, want verdict unknown provider none", r)
	}
}

func TestClassifyWordlistCleanAndProfanity(t *testing.T) {
	svc, _, _ := testService(t)
	lyrics := "la la la, clean words only"
	r := svc.classifyTrack(t.Context(), "Clean Song", "Clean Artist", &lyrics)
	if r.verdict != "clean" || r.provider != "wordlist" {
		t.Fatalf("clean lyrics classified as %+v", r)
	}
	bad := "this shit is loud"
	r = svc.classifyTrack(t.Context(), "Clean Song", "Clean Artist", &bad)
	if r.verdict != "profanity" || r.matched != "shit" {
		t.Fatalf("profane lyrics classified as %+v", r)
	}
}

// --- refs --------------------------------------------------------------

func TestParseSpotifyRef(t *testing.T) {
	kind, id, err := parseSpotifyRef("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=x")
	if err != nil || kind != "track" || id != "4uLU6hMCjMI75M1A2tKUQC" {
		t.Fatalf("parse URL: kind=%q id=%q err=%v", kind, id, err)
	}
	kind, id, err = parseSpotifyRef("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M")
	if err != nil || kind != "playlist" || id != "37i9dQZF1DXcBWIGoYBM5M" {
		t.Fatalf("parse URI: kind=%q id=%q err=%v", kind, id, err)
	}
	if _, _, err := parseSpotifyRef("https://example.com/track/123"); err == nil {
		t.Fatalf("non-Spotify URL parsed without error")
	}
	if _, _, err := parseSpotifyRef(""); err == nil {
		t.Fatalf("empty reference parsed without error")
	}
}

// --- auth ---------------------------------------------------------------

func TestAuthLoginMissingCreds(t *testing.T) {
	svc, _, _ := testService(t)
	_, err := svc.handleAuth(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"login","user_id":7}`)})
	if err == nil {
		t.Fatalf("login without creds succeeded")
	}
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrEngineUnavailable {
		t.Fatalf("login without creds: err=%v, want engine_unavailable", err)
	}
}

func TestAuthLoginURLAndStateSingleUse(t *testing.T) {
	svc, _, _ := testService(t)
	svc.cfg.Backend.SpotifyClientID = "cid"
	svc.cfg.Backend.SpotifyClientSecret = "csecret"
	svc.cfg.Backend.SpotifyRedirectURI = "http://localhost:8080/callback"
	data, err := svc.handleAuth(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"login","user_id":7}`)})
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	m := data.(map[string]any)
	u, _ := m["authorize_url"].(string)
	if !strings.Contains(u, "client_id=cid") || !strings.Contains(u, "response_type=code") || !strings.Contains(u, "state=7-") {
		t.Fatalf("authorize_url malformed: %s", u)
	}
	// Extract the state and consume it twice: second use must fail.
	parsed := strings.Split(u, "state=")
	state := strings.Split(parsed[1], "&")[0]
	if err := svc.consumeState(state, 7); err != nil {
		t.Fatalf("first consumeState failed: %v", err)
	}
	if err := svc.consumeState(state, 7); err == nil {
		t.Fatalf("state reused without error (must be single-use)")
	}
}

func TestAuthCallbackExchangeStoresEncrypted(t *testing.T) {
	svc, _, dir := testService(t)
	svc.cfg.Backend.SpotifyClientID = "cid"
	svc.cfg.Backend.SpotifyClientSecret = "csecret"
	svc.cfg.Backend.SpotifyRedirectURI = "http://localhost:8080/callback"
	svc.accountsBase = stubAccountsServer(t, `{"access_token":"tok-1","refresh_token":"ref-1","expires_in":3600}`)
	state := svc.mintState(42)
	payload, _ := json.Marshal(map[string]any{"action": "callback", "user_id": 42, "code": "authcode", "state": state})
	data, err := svc.handleAuth(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("callback: %v", err)
	}
	m := data.(map[string]any)
	if m["authorized"] != true {
		t.Fatalf("callback response: %v", m)
	}
	got, found, err := svc.loadUserToken(42)
	if err != nil || !found || got.accessToken != "tok-1" {
		t.Fatalf("stored token: found=%v err=%v tok=%+v", found, err, got)
	}
	raw, _ := os.ReadFile(filepath.Join(dir, "parlay.db"))
	if strings.Contains(string(raw), "tok-1") {
		t.Fatalf("plaintext access token in parlay.db")
	}
}

func TestAuthCallbackBadState(t *testing.T) {
	svc, _, _ := testService(t)
	svc.cfg.Backend.SpotifyClientID = "cid"
	svc.cfg.Backend.SpotifyClientSecret = "csecret"
	svc.cfg.Backend.SpotifyRedirectURI = "http://localhost:8080/callback"
	payload, _ := json.Marshal(map[string]any{"action": "callback", "user_id": 42, "code": "x", "state": "bogus"})
	_, err := svc.handleAuth(t.Context(), uds.Request{Payload: payload})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrUnauthorized {
		t.Fatalf("bad state: err=%v, want unauthorized", err)
	}
}

func TestAuthRefreshDropsTokenOnFailure(t *testing.T) {
	svc, _, _ := testService(t)
	svc.cfg.Backend.SpotifyClientID = "cid"
	svc.cfg.Backend.SpotifyClientSecret = "csecret"
	svc.cfg.Backend.SpotifyRedirectURI = "http://localhost:8080/callback"
	svc.accountsBase = stubAccountsServerFail(t)
	mustSaveToken(t, svc, 55, "old-access", "dead-refresh", time.Now().Unix()+3600)
	payload, _ := json.Marshal(map[string]any{"action": "refresh", "user_id": 55})
	_, err := svc.handleAuth(t.Context(), uds.Request{Payload: payload})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrUnauthorized {
		t.Fatalf("refresh failure: err=%v, want unauthorized", err)
	}
	if _, found, _ := svc.loadUserToken(55); found {
		t.Fatalf("dead refresh token was not dropped")
	}
}

func TestAuthRevokeIdempotent(t *testing.T) {
	svc, _, _ := testService(t)
	svc.cfg.Backend.SpotifyClientID = "cid"
	svc.cfg.Backend.SpotifyClientSecret = "csecret"
	svc.cfg.Backend.SpotifyRedirectURI = "http://localhost:8080/callback"
	mustSaveToken(t, svc, 66, "a", "r", time.Now().Unix()+3600)
	payload, _ := json.Marshal(map[string]any{"action": "revoke", "user_id": 66})
	data, err := svc.handleAuth(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("revoke: %v", err)
	}
	if data.(map[string]any)["was_authorized"] != true {
		t.Fatalf("revoke response: %v", data)
	}
	data, err = svc.handleAuth(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("second revoke: %v", err)
	}
	if data.(map[string]any)["was_authorized"] != false {
		t.Fatalf("second revoke must report was_authorized=false: %v", data)
	}
}

// stubAccountsServer serves one token response for any POST.
func stubAccountsServer(t *testing.T, body string) string {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(body))
	}))
	t.Cleanup(srv.Close)
	return srv.URL
}

func stubAccountsServerFail(t *testing.T) string {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":"invalid_grant","error_description":"Invalid refresh token"}`))
	}))
	t.Cleanup(srv.Close)
	return srv.URL
}

// --- watch / skip_check -------------------------------------------------

func TestWatchStartWithoutToken(t *testing.T) {
	svc, _, _ := testService(t)
	_, err := svc.handleWatch(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"start","user_id":77}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrNotFound {
		t.Fatalf("watch without token: err=%v, want not_found", err)
	}
}

func TestWatchStartStop(t *testing.T) {
	svc, _, _ := testService(t)
	mustSaveToken(t, svc, 77, "a", "r", time.Now().Unix()+3600)
	data, err := svc.handleWatch(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"start","user_id":77}`)})
	if err != nil {
		t.Fatalf("watch start: %v", err)
	}
	if data.(map[string]any)["watching"] != true {
		t.Fatalf("watch start response: %v", data)
	}
	data, err = svc.handleWatch(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"stop","user_id":77}`)})
	if err != nil {
		t.Fatalf("watch stop: %v", err)
	}
	if data.(map[string]any)["was_watching"] != true {
		t.Fatalf("watch stop response: %v", data)
	}
	data, err = svc.handleWatch(t.Context(), uds.Request{Payload: json.RawMessage(`{"action":"stop","user_id":77}`)})
	if err != nil {
		t.Fatalf("second watch stop: %v", err)
	}
	if data.(map[string]any)["was_watching"] != false {
		t.Fatalf("second stop must report was_watching=false: %v", data)
	}
}

func TestSkipCheckDisliked(t *testing.T) {
	svc, _, _ := testService(t)
	if err := svc.store.SetDislike(5, "track-1", true); err != nil {
		t.Fatalf("SetDislike: %v", err)
	}
	track := model.Track{ID: "track-1", Title: "Clean Song", Artist: "Clean Artist", Source: "spotify"}
	payload, _ := json.Marshal(map[string]any{"track": track, "user_id": 5})
	data, err := svc.handleSkipCheck(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("skip_check: %v", err)
	}
	m := data.(map[string]any)
	if m["skip"] != true || m["reason"] != "disliked" {
		t.Fatalf("skip_check disliked: %v", m)
	}
}

func TestSkipCheckProfanityFastPath(t *testing.T) {
	svc, _, _ := testService(t)
	track := model.Track{ID: "track-2", Title: "What the fuck", Artist: "Someone", Source: "spotify"}
	payload, _ := json.Marshal(map[string]any{"track": track})
	data, err := svc.handleSkipCheck(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("skip_check: %v", err)
	}
	m := data.(map[string]any)
	if m["skip"] != true || m["reason"] != "profanity" || m["classifier_provider"] != "wordlist" {
		t.Fatalf("skip_check profanity: %v", m)
	}
}

// --- resolve / status / details ------------------------------------------

func TestResolveParseOnlyWithoutCreds(t *testing.T) {
	svc, _, _ := testService(t)
	data, err := svc.handleResolve(t.Context(), uds.Request{Payload: json.RawMessage(`{"url":"https://open.spotify.com/track/abc123"}`)})
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	m := data.(map[string]any)
	if m["api"] != "none" || m["kind"] != "track" || m["track_id"] != "abc123" {
		t.Fatalf("parse-only resolve: %v", m)
	}
	if _, ok := m["note"]; !ok {
		t.Fatalf("parse-only resolve must carry a note: %v", m)
	}
}

func TestStatusWithoutCreds(t *testing.T) {
	svc, _, _ := testService(t)
	data, err := svc.handleStatus(t.Context(), uds.Request{Payload: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatalf("status: %v", err)
	}
	m := data.(map[string]any)
	if m["api"] != "none" || m["credentials_configured"] != false || m["token_cached"] != false {
		t.Fatalf("status without creds: %v", m)
	}
}

func TestDetailsWithoutCreds(t *testing.T) {
	svc, _, _ := testService(t)
	_, err := svc.handleDetails(t.Context(), uds.Request{Payload: json.RawMessage(`{"user_id":1,"track_id":"abc"}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrEngineUnavailable {
		t.Fatalf("details without creds: err=%v, want engine_unavailable", err)
	}
}

// --- registration over the real UDS server -------------------------------

func TestRegisterAnswersOverUDS(t *testing.T) {
	_, st, dir := testService(t)
	// Reuse the store testService opened: opening the same bbolt
	// file twice in one process blocks on the file lock.
	cfg := &config.Config{}
	cfg.Backend.DataDir = dir
	socketPath := filepath.Join(dir, "test.sock")
	server := uds.NewServer(socketPath)
	Register(server, &app.Deps{Store: st, Config: cfg, Registry: resolver.NewRegistry()})
	if err := server.Listen(); err != nil {
		t.Fatalf("Listen: %v", err)
	}
	defer server.Close()
	conn, err := net.Dial("unix", socketPath)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	frame, err := json.Marshal(map[string]any{
		"type": "request", "request_id": 1, "action": "spotify.status",
		"deadline_ms": 5000, "payload": map[string]any{},
	})
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	if err := uds.WriteFrame(conn, frame); err != nil {
		t.Fatalf("write: %v", err)
	}
	raw, err := uds.ReadFrame(conn)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	var resp struct {
		OK   bool           `json:"ok"`
		Data map[string]any `json:"data"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if !resp.OK || resp.Data["api"] != "none" {
		t.Fatalf("spotify.status over UDS: ok=%v data=%v", resp.OK, resp.Data)
	}
}
