// Spotify resolver — client-credentials metadata (1 POST, token
// cached to expiry) + audio resolution through the Tier 1→2→3 ladder
// (YouTube). Ported from services/vc-rust/src/main.rs
// (spotify_client_token, resolve_spotify_track,
// spotify_track_from_json) and services/vc-ts/src/resolvers.js
// (spotifyClientToken, spotifySearchTracks).
//
// No creds in the sandbox — every path fails honestly naming
// SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET. The Spotify Web API
// gives metadata only (no direct audio URL); audio always comes from
// the YouTube ladder, never from spotdl (the slow subprocess path,
// kept out of the resolve path by design).
package resolver

import (
	"context"
	"encoding/base64"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

// SpotifyResolver resolves Spotify tracks (metadata) and routes
// audio through the YouTube ladder.
type SpotifyResolver struct {
	youtube *YouTubeResolver

	mu       sync.Mutex
	token    string
	tokenExp time.Time
}

// NewSpotifyResolver builds a Spotify resolver. youtube is the
// Tier 3 ladder used for audio; it may be nil, in which case
// Resolve returns metadata only (StreamURL empty).
func NewSpotifyResolver(youtube *YouTubeResolver) *SpotifyResolver {
	return &SpotifyResolver{youtube: youtube}
}

// Source implements Resolver.
func (s *SpotifyResolver) Source() Source { return SourceSpotify }

// ExtractSpotifyID returns the id from a Spotify URL
// (open.spotify.com/<kind>/<id>) or URI (spotify:<kind>:<id>),
// or "".
func ExtractSpotifyID(input string) string {
	q := strings.TrimSpace(input)
	if strings.HasPrefix(q, "spotify:") {
		parts := strings.Split(q, ":")
		if len(parts) == 3 && parts[2] != "" {
			return parts[2]
		}
		return ""
	}
	u, err := url.Parse(q)
	if err != nil || u.Host == "" {
		return ""
	}
	if !strings.HasSuffix(strings.ToLower(u.Host), "spotify.com") {
		return ""
	}
	parts := strings.Split(strings.Trim(u.Path, "/"), "/")
	if len(parts) >= 2 && parts[1] != "" {
		id := strings.Split(parts[1], "?")[0]
		return id
	}
	return ""
}

// spotifyCreds returns (id, secret) or the honest missing-creds error.
func spotifyCreds() (string, string, error) {
	id, secret := os.Getenv("SPOTIFY_CLIENT_ID"), os.Getenv("SPOTIFY_CLIENT_SECRET")
	if id == "" || secret == "" {
		return "", "", &ErrMissingCredential{Source: SourceSpotify,
			Missing: "SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET",
			Detail:  "client-credentials flow"}
	}
	return id, secret, nil
}

// clientToken returns a cached client-credentials access token,
// exchanging a fresh one (1 POST) when expired. The token is cached
// until expiry minus a 60 s safety margin (vc-rust
// spotify_client_token).
func (s *SpotifyResolver) clientToken(ctx context.Context) (string, error) {
	s.mu.Lock()
	if s.token != "" && time.Now().Before(s.tokenExp) {
		tok := s.token
		s.mu.Unlock()
		return tok, nil
	}
	s.mu.Unlock()

	id, secret, err := spotifyCreds()
	if err != nil {
		return "", err
	}
	form := url.Values{}
	form.Set("grant_type", "client_credentials")
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "https://accounts.spotify.com/api/token", strings.NewReader(form.Encode()))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(id+":"+secret)))
	resp, err := httpClient(10 * time.Second).Do(req)
	if err != nil {
		return "", fmt.Errorf("Spotify token exchange failed (accounts.spotify.com/api/token unreachable or rejected): %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("Spotify token exchange failed (accounts.spotify.com/api/token returned HTTP %d)", resp.StatusCode)
	}
	var data struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int64  `json:"expires_in"`
	}
	if err := decodeJSON(readAllLimit(resp.Body, 1<<20), &data); err != nil {
		return "", fmt.Errorf("Spotify token exchange: bad JSON: %w", err)
	}
	if data.AccessToken == "" {
		return "", fmt.Errorf("Spotify token exchange failed (no access_token in response)")
	}
	expiresIn := data.ExpiresIn
	if expiresIn <= 0 {
		expiresIn = 3600
	}
	s.mu.Lock()
	s.token = data.AccessToken
	s.tokenExp = time.Now().Add(time.Duration(expiresIn-60) * time.Second)
	s.mu.Unlock()
	return data.AccessToken, nil
}

// Resolve handles spotify:track:<id>, a Spotify track URL, or a
// bare track id. Returns metadata + YouTube-ladder audio when the
// ladder is available; without creds it fails naming them.
func (s *SpotifyResolver) Resolve(ctx context.Context, query string) (Track, error) {
	if _, _, err := spotifyCreds(); err != nil {
		return Track{}, fmt.Errorf("Spotify track cannot be resolved: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)")
	}
	id := ExtractSpotifyID(strings.TrimSpace(query))
	if strings.HasPrefix(strings.ToLower(strings.TrimSpace(query)), "spotify:") {
		parts := strings.Split(strings.TrimSpace(query), ":")
		if len(parts) == 3 {
			id = parts[2]
		}
	}
	if id == "" {
		// Bare id?
		q := strings.TrimSpace(query)
		if q != "" && !strings.ContainsAny(q, " \t/") {
			id = q
		}
	}
	if id == "" {
		return Track{}, engineUnavailable(SourceSpotify, "not a Spotify track reference: %q", query)
	}
	token, err := s.clientToken(ctx)
	if err != nil {
		return Track{}, err
	}
	var data spotifyTrackJSON
	if err := getJSON(ctx, "https://api.spotify.com/v1/tracks/"+url.PathEscape(id),
		map[string]string{"Authorization": "Bearer " + token}, &data); err != nil {
		return Track{}, fmt.Errorf("Spotify track lookup failed for %s: %v", id, err)
	}
	if data.Name == "" {
		return Track{}, engineUnavailable(SourceSpotify, "Spotify returned no usable track for %s", id)
	}
	track := spotifyTrackFromJSON(data, id)
	if s.youtube != nil {
		// Audio via the Tier 3 ladder: YouTube search for the
		// artist + title (vc-rust resolve_spotify_track contract:
		// metadata only from Spotify, audio from YouTube).
		if yt, err := s.youtube.Search(ctx, track.Artist+" - "+track.Title, 1); err == nil && len(yt) > 0 && yt[0].StreamURL != "" {
			track.StreamURL = yt[0].StreamURL
		}
	}
	return track, nil
}

// Search runs a Spotify client-credentials track search.
func (s *SpotifyResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if _, _, err := spotifyCreds(); err != nil {
		return nil, fmt.Errorf("Spotify search cannot run: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)")
	}
	if limit <= 0 {
		limit = 5
	}
	token, err := s.clientToken(ctx)
	if err != nil {
		return nil, err
	}
	params := url.Values{}
	params.Set("q", strings.TrimSpace(query))
	params.Set("type", "track")
	params.Set("market", "IN")
	params.Set("limit", fmt.Sprint(limit))
	var data struct {
		Tracks struct {
			Items []spotifyTrackJSON `json:"items"`
		} `json:"tracks"`
	}
	if err := getJSON(ctx, "https://api.spotify.com/v1/search?"+params.Encode(),
		map[string]string{"Authorization": "Bearer " + token}, &data); err != nil {
		return nil, fmt.Errorf("Spotify search failed: %v", err)
	}
	tracks := make([]Track, 0, len(data.Tracks.Items))
	for _, item := range data.Tracks.Items {
		tracks = append(tracks, spotifyTrackFromJSON(item, item.ID))
	}
	return tracks, nil
}

// Download resolves the track (metadata + YouTube-ladder audio) and
// downloads the audio through the YouTube daemon.
func (s *SpotifyResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	if track.StreamURL == "" {
		full, err := s.Resolve(ctx, track.ID)
		if err != nil {
			return DownloadResult{}, err
		}
		track = full
	}
	if s.youtube == nil {
		return DownloadResult{}, engineUnavailable(SourceSpotify, "Spotify audio comes from the YouTube ladder, which is not configured")
	}
	yt := track
	yt.Source = string(SourceYouTube)
	res, err := s.youtube.Download(ctx, yt, destDir, onProgress)
	if err != nil {
		return DownloadResult{}, err
	}
	res.Track = track
	return res, nil
}

// spotifyTrackJSON is one Spotify track object from the Web API.
type spotifyTrackJSON struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	DurationMs int64  `json:"duration_ms"`
	Artists    []struct {
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

// spotifyTrackFromJSON maps a Spotify track object to a Track
// (vc-rust spotify_track_from_json). The Spotify API gives metadata
// only — no direct audio URL (StreamURL stays empty unless the
// caller fills it from the YouTube ladder).
func spotifyTrackFromJSON(data spotifyTrackJSON, id string) Track {
	artists := make([]string, 0, len(data.Artists))
	for _, a := range data.Artists {
		artists = append(artists, a.Name)
	}
	duration := int(data.DurationMs / 1000)
	if duration < 0 {
		duration = 0
	}
	t := Track{
		ID: id, Title: data.Name, Artist: strings.Join(artists, ", "),
		Source: string(SourceSpotify), URL: data.ExternalURLs.Spotify,
		DurationSec: int64(duration),
		DurationStr: FormatDurationStr(duration, false),
	}
	if len(data.Album.Images) > 0 {
		t.Thumbnail = data.Album.Images[0].URL
	}
	return t
}
