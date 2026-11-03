// Package spotify implements the spotify.* UDS actions
// (FINAL-ARCHITECTURE.md §1.3 Part 2; phase3/actions-inventory.md §6).
// It ports services/spotify-rust: OAuth token store (AES-256-GCM,
// per-user, ciphertext only in the bbolt spotify_tokens bucket),
// client-credentials app-token cache, watch registry + poller, dislike
// store, skip-check classifier (wordlist verbatim from spotify-rust,
// Gemini when GEMINI_API_KEY is set), details/analyze.
//
// Spotify is metadata-only: audio resolution routes through the
// Tier 1→2→3 ladder via the resolver registry (never spotdl). Every
// action that needs Web API credentials fails honestly with
// engine_unavailable when SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET
// are absent — except spotify.resolve, which degrades to parse-only.
package spotify

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// AISkipConfidence is the confidence at or above which an
// ai_generated verdict skips (spotify-rust classifier::AI_SKIP_CONFIDENCE).
const AISkipConfidence = 0.7

// oauthScopes are the read-only scopes requested at login (the poller
// never skips on the user's player).
const oauthScopes = "user-read-currently-playing user-read-playback-state"

// userToken is one decrypted per-user OAuth token (memory only —
// plaintext never touches disk).
type userToken struct {
	accessToken  string
	refreshToken string
	expiresAt    int64
}

func (t *userToken) needsRefresh(now int64) bool { return t.expiresAt <= now+120 }

// polledState is the last polled currently-playing state for one
// watched user. In-memory only (spotify-rust: "playback state is
// ephemeral; the watch registry itself is persisted, this is not") —
// the poller repopulates it on its cadence after a restart.
type polledState struct {
	track      *model.Track
	playing    bool
	checkedAt  int64
	lastSkip   bool
	lastReason *string
	verdict    string
}

// Service owns the spotify.* handlers.
type Service struct {
	store    *store.Store
	cfg      *config.Config
	registry *resolver.Registry

	httpClient   *http.Client
	accountsBase string
	apiBase      string

	// App-token (client-credentials) cache: refreshed 60 s before
	// expiry (spotify-rust fetch_app_token).
	appMu      sync.Mutex
	appTok     string
	appTokenEx int64

	// Login states: single-use, 10-minute TTL (inventory §8.2).
	stateMu sync.Mutex
	states  map[string]authState

	// Poller (lazy-started on the first watch; interval from
	// SPOTIFY_POLL_INTERVAL_SECS, default 30, 0 disables).
	pollMu       sync.Mutex
	pollRunning  bool
	pollStop     chan struct{}
	pollInterval time.Duration

	npMu sync.Mutex
	np   map[int64]*polledState
}

type authState struct {
	userID    int64
	expiresAt time.Time
}

// newService builds a Service from handler deps.
func newService(d *app.Deps) *Service {
	s := &Service{
		store:    d.Store,
		cfg:      d.Config,
		registry: d.Registry,
		httpClient: &http.Client{
			Timeout: 30 * time.Second,
			Transport: &http.Transport{
				Proxy: http.ProxyFromEnvironment,
			},
		},
		accountsBase: accountsBase(),
		apiBase:      apiBase(),
		states:       map[string]authState{},
		np:           map[int64]*polledState{},
	}
	secs := 30
	if v := strings.TrimSpace(os.Getenv("SPOTIFY_POLL_INTERVAL_SECS")); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			secs = n
		}
	}
	if secs < 0 {
		secs = 0
	}
	s.pollInterval = time.Duration(secs) * time.Second
	return s
}

func accountsBase() string {
	if v := strings.TrimSpace(os.Getenv("SPOTIFY_ACCOUNTS_URL")); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "https://accounts.spotify.com"
}

func apiBase() string {
	if v := strings.TrimSpace(os.Getenv("SPOTIFY_API_BASE")); v != "" {
		return strings.TrimRight(v, "/")
	}
	return "https://api.spotify.com"
}

// Register registers all spotify.* actions on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("spotify: Register called with nil deps")
	}
	svc := newService(d)
	s.Register("spotify.resolve", svc.handleResolve)
	s.Register("spotify.collection", svc.handleCollection)
	s.Register("spotify.status", svc.handleStatus)
	s.Register("spotify.auth", svc.handleAuth)
	s.Register("spotify.watch", svc.handleWatch)
	s.Register("spotify.watches", svc.handleWatches)
	s.Register("spotify.dislike", svc.handleDislike)
	s.Register("spotify.skip_check", svc.handleSkipCheck)
	s.Register("spotify.now_playing", svc.handleNowPlaying)
	s.Register("spotify.details", svc.handleDetails)
	s.Register("spotify.analyze", svc.handleAnalyze)
}

// --- shared helpers ---------------------------------------------------

func badRequest(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf(format, args...))
}

func engineUnavailable(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf(format, args...))
}

func notFound(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf(format, args...))
}

func internalErr(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf(format, args...))
}

// decodePayload unmarshals the request payload into v.
func decodePayload(req uds.Request, v any) error {
	if len(req.Payload) == 0 {
		return badRequest("missing request payload")
	}
	if err := json.Unmarshal(req.Payload, v); err != nil {
		return badRequest("invalid request payload: %v", err)
	}
	return nil
}

// creds returns (clientID, clientSecret, redirectURI, ok). Config
// wins over env; all three must be present for OAuth, the first two
// for the app-token flow.
func (s *Service) creds() (id, secret, redirect string) {
	if s.cfg != nil {
		id, secret, redirect = s.cfg.Backend.SpotifyClientID, s.cfg.Backend.SpotifyClientSecret, s.cfg.Backend.SpotifyRedirectURI
	}
	if id == "" {
		id = os.Getenv("SPOTIFY_CLIENT_ID")
	}
	if secret == "" {
		secret = os.Getenv("SPOTIFY_CLIENT_SECRET")
	}
	if redirect == "" {
		redirect = os.Getenv("SPOTIFY_REDIRECT_URI")
	}
	return id, secret, redirect
}

func (s *Service) appCredsOK() bool {
	id, secret, _ := s.creds()
	return id != "" && secret != ""
}

func (s *Service) oauthCredsOK() bool {
	id, secret, redirect := s.creds()
	return id != "" && secret != "" && redirect != ""
}

func (s *Service) geminiKey() string {
	if s.cfg != nil && s.cfg.Backend.GeminiAPIKey != "" {
		return s.cfg.Backend.GeminiAPIKey
	}
	return os.Getenv("GEMINI_API_KEY")
}

// appToken returns a cached client-credentials token, exchanging a
// fresh one (1 POST) when expired. Cached until expiry minus a 60 s
// safety margin (spotify-rust spotify_client_token).
func (s *Service) appToken(ctx context.Context) (string, error) {
	s.appMu.Lock()
	if s.appTok != "" && time.Now().Unix() < s.appTokenEx {
		tok := s.appTok
		s.appMu.Unlock()
		return tok, nil
	}
	s.appMu.Unlock()

	id, secret, _ := s.creds()
	if id == "" || secret == "" {
		return "", engineUnavailable("Spotify credentials not configured: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)")
	}
	form := url.Values{}
	form.Set("grant_type", "client_credentials")
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.accountsBase+"/api/token", strings.NewReader(form.Encode()))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(id+":"+secret)))
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return "", engineUnavailable("Spotify token exchange failed (accounts.spotify.com/api/token unreachable or rejected): %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", engineUnavailable("Spotify token exchange failed (accounts.spotify.com/api/token returned HTTP %d)", resp.StatusCode)
	}
	var data struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int64  `json:"expires_in"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return "", internalErr("Spotify token exchange: bad JSON: %v", err)
	}
	if data.AccessToken == "" {
		return "", engineUnavailable("Spotify token exchange failed (no access_token in response)")
	}
	expiresIn := data.ExpiresIn
	if expiresIn <= 0 {
		expiresIn = 3600
	}
	s.appMu.Lock()
	s.appTok = data.AccessToken
	s.appTokenEx = time.Now().Unix() + expiresIn - 60
	s.appMu.Unlock()
	return data.AccessToken, nil
}

// apiGet performs one authenticated Web API GET with the app token,
// decoding the JSON response into v. A 401 clears the cached token
// and retries once. Non-2xx is an engine_unavailable error carrying
// the status (never a faked track).
func (s *Service) apiGet(ctx context.Context, path string, v any) error {
	for attempt := 0; attempt < 2; attempt++ {
		tok, err := s.appToken(ctx)
		if err != nil {
			return err
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.apiBase+path, nil)
		if err != nil {
			return err
		}
		req.Header.Set("Authorization", "Bearer "+tok)
		req.Header.Set("Accept", "application/json")
		resp, err := s.httpClient.Do(req)
		if err != nil {
			return engineUnavailable("Spotify API request to %s failed: %v", path, err)
		}
		if resp.StatusCode == http.StatusUnauthorized && attempt == 0 {
			resp.Body.Close()
			s.appMu.Lock()
			s.appTok = ""
			s.appTokenEx = 0
			s.appMu.Unlock()
			continue
		}
		defer resp.Body.Close()
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return engineUnavailable("Spotify API %s returned HTTP %d", path, resp.StatusCode)
		}
		if v == nil {
			return nil
		}
		if err := json.NewDecoder(resp.Body).Decode(v); err != nil {
			return internalErr("Spotify API %s: bad JSON: %v", path, err)
		}
		return nil
	}
	return engineUnavailable("Spotify API %s: unauthorized", path)
}

// parseSpotifyRef splits a Spotify URL or URI into (kind, id).
// Accepts open.spotify.com/<kind>/<id> and spotify:<kind>:<id>;
// kind is track|album|playlist|artist.
func parseSpotifyRef(input string) (kind, id string, err error) {
	q := strings.TrimSpace(input)
	if q == "" {
		return "", "", badRequest("empty Spotify reference")
	}
	if strings.HasPrefix(q, "spotify:") {
		parts := strings.Split(q, ":")
		if len(parts) == 3 && parts[1] != "" && parts[2] != "" {
			return parts[1], parts[2], nil
		}
		return "", "", badRequest("not a Spotify URL/URI: %q", input)
	}
	u, perr := url.Parse(q)
	if perr != nil || u.Host == "" {
		return "", "", badRequest("not a Spotify URL/URI: %q", input)
	}
	if !strings.HasSuffix(strings.ToLower(u.Host), "spotify.com") {
		return "", "", badRequest("not a Spotify URL/URI: %q", input)
	}
	parts := strings.Split(strings.Trim(u.Path, "/"), "/")
	if len(parts) >= 2 && parts[0] != "" && parts[1] != "" {
		return parts[0], strings.Split(parts[1], "?")[0], nil
	}
	return "", "", badRequest("not a Spotify URL/URI: %q", input)
}

// canonicalURL renders the open.spotify.com URL for a kind/id.
func canonicalURL(kind, id string) string {
	return "https://open.spotify.com/" + kind + "/" + id
}

// spotifyTrackJSON is one Spotify track object from the Web API.
type spotifyTrackJSON struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	DurationMs int64  `json:"duration_ms"`
	Artists    []struct {
		ID   string `json:"id"`
		Name string `json:"name"`
	} `json:"artists"`
	ExternalURLs struct {
		Spotify string `json:"spotify"`
	} `json:"external_urls"`
	Album struct {
		Images []struct {
			URL string `json:"url"`
		} `json:"images"`
	} `json:"album"`
}

func spotifyTrackFromJSON(data spotifyTrackJSON) model.Track {
	artists := make([]string, 0, len(data.Artists))
	for _, a := range data.Artists {
		artists = append(artists, a.Name)
	}
	duration := int(data.DurationMs / 1000)
	if duration < 0 {
		duration = 0
	}
	t := model.Track{
		ID: data.ID, Title: data.Name, Artist: strings.Join(artists, ", "),
		Source: "spotify", URL: data.ExternalURLs.Spotify,
		DurationSec: int64(duration),
		DurationStr: resolver.FormatDurationStr(duration, false),
	}
	if len(data.Album.Images) > 0 {
		t.Thumbnail = data.Album.Images[0].URL
	}
	if t.URL == "" {
		t.URL = canonicalURL("track", data.ID)
	}
	return t
}

// dislikedByUsers returns the users who dislike a track: every
// watched user plus the requesting user, checked against the store.
// The store has no dislike-scan API, so this covers exactly the
// population the watch/skip-check flow uses; a store ListDislikers
// method would make it exhaustive.
func (s *Service) dislikedByUsers(trackID string, requester int64) []int64 {
	seen := map[int64]bool{}
	var out []int64
	add := func(uid int64) {
		if uid == 0 || seen[uid] {
			return
		}
		disliked, err := s.store.IsDisliked(uid, trackID)
		if err == nil && disliked {
			seen[uid] = true
			out = append(out, uid)
		}
	}
	if ids, err := s.store.ListWatches(); err == nil {
		for _, uid := range ids {
			add(uid)
		}
	}
	add(requester)
	if out == nil {
		out = []int64{}
	}
	return out
}
