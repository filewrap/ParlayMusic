package download

import (
	"archive/zip"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// MaxPlaylistTracks caps a Spotify collection download at 50 tracks
// (downloaders spotify.py MAX_PLAYLIST_TRACKS); beyond that the
// response carries truncated:true.
const MaxPlaylistTracks = 50

// spotifyCollectionClient is a minimal Spotify Web API
// client-credentials client for public collections (playlist / album /
// artist top tracks). It is the download-side twin of spotify.resolve
// / spotify.collection: with creds the track list comes from the real
// Web API; without creds the yt-dlp flat extract of the public page
// is the fallback (downloaders spotify.py resolve_collection).
type spotifyCollectionClient struct {
	httpClient *http.Client
	token      string
	tokenExp   time.Time
}

func newSpotifyCollectionClient() *spotifyCollectionClient {
	return &spotifyCollectionClient{httpClient: &http.Client{Timeout: 30 * time.Second}}
}

func (c *spotifyCollectionClient) creds() (string, string, bool) {
	id := strings.TrimSpace(os.Getenv("SPOTIFY_CLIENT_ID"))
	secret := strings.TrimSpace(os.Getenv("SPOTIFY_CLIENT_SECRET"))
	return id, secret, id != "" && secret != ""
}

func (c *spotifyCollectionClient) clientToken(ctx context.Context) (string, error) {
	if c.token != "" && time.Now().Before(c.tokenExp.Add(-60*time.Second)) {
		return c.token, nil
	}
	id, secret, ok := c.creds()
	if !ok {
		return "", &resolver.ErrMissingCredential{Source: resolver.SourceSpotify,
			Missing: "SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET", Detail: "client-credentials flow"}
	}
	form := url.Values{"grant_type": {"client_credentials"}}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, "https://accounts.spotify.com/api/token", strings.NewReader(form.Encode()))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.SetBasicAuth(id, secret)
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("Spotify token endpoint failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("Spotify token endpoint returned HTTP %d", resp.StatusCode)
	}
	var data struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return "", fmt.Errorf("Spotify token response unreadable: %w", err)
	}
	if data.AccessToken == "" {
		return "", fmt.Errorf("Spotify token endpoint returned no access token")
	}
	c.token = data.AccessToken
	if data.ExpiresIn <= 0 {
		data.ExpiresIn = 3600
	}
	c.tokenExp = time.Now().Add(time.Duration(data.ExpiresIn) * time.Second)
	return c.token, nil
}

func (c *spotifyCollectionClient) getJSON(ctx context.Context, endpoint string, v any) error {
	token, err := c.clientToken(ctx)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("Spotify API request failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("Spotify API returned HTTP %d for %s", resp.StatusCode, endpoint)
	}
	return json.NewDecoder(resp.Body).Decode(v)
}

// Collection resolves a Spotify playlist/album/artist URL to its
// tracks (Web API when configured; the caller falls back to the
// yt-dlp flat extract when creds are absent). Track URLs are refused.
func (c *spotifyCollectionClient) Collection(ctx context.Context, rawURL string) (kind string, tracks []model.Track, err error) {
	kind = spotifyURLKind(rawURL)
	id := resolver.ExtractSpotifyID(rawURL)
	switch kind {
	case "playlist", "album", "artist":
	default:
		return "", nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("use download.spotify for single track URLs (got a %s URL)", kindOrUnknown(kind)))
	}
	if id == "" {
		return "", nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("not a Spotify collection URL: %q", rawURL))
	}
	if _, _, ok := c.creds(); !ok {
		return "", nil, &resolver.ErrMissingCredential{Source: resolver.SourceSpotify,
			Missing: "SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET",
			Detail:  "collection resolution needs the Web API; the caller falls back to the yt-dlp page extract"}
	}
	switch kind {
	case "playlist":
		var data struct {
			Tracks struct {
				Items []struct {
					Track spotifyAPITrack `json:"track"`
				} `json:"items"`
			} `json:"tracks"`
		}
		if err := c.getJSON(ctx, "https://api.spotify.com/v1/playlists/"+url.PathEscape(id)+"/tracks?limit=50&market=IN", &data); err != nil {
			return "", nil, err
		}
		for _, item := range data.Tracks.Items {
			if item.Track.ID != "" {
				tracks = append(tracks, spotifyAPITrackToModel(item.Track))
			}
		}
	case "album":
		var data struct {
			Tracks struct {
				Items []spotifyAPITrack `json:"items"`
			} `json:"tracks"`
		}
		if err := c.getJSON(ctx, "https://api.spotify.com/v1/albums/"+url.PathEscape(id)+"/tracks?limit=50&market=IN", &data); err != nil {
			return "", nil, err
		}
		for _, item := range data.Tracks.Items {
			tracks = append(tracks, spotifyAPITrackToModel(item))
		}
	case "artist":
		var data struct {
			Tracks []spotifyAPITrack `json:"tracks"`
		}
		if err := c.getJSON(ctx, "https://api.spotify.com/v1/artists/"+url.PathEscape(id)+"/top-tracks?market=IN", &data); err != nil {
			return "", nil, err
		}
		for _, item := range data.Tracks {
			tracks = append(tracks, spotifyAPITrackToModel(item))
		}
	}
	return kind, tracks, nil
}

// spotifyAPITrack is one Spotify track object (subset).
type spotifyAPITrack struct {
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

func spotifyAPITrackToModel(data spotifyAPITrack) model.Track {
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
		Source: string(resolver.SourceSpotify), URL: data.ExternalURLs.Spotify,
		DurationSec: int64(duration),
		DurationStr: resolver.FormatDurationStr(duration, false),
	}
	if len(data.Album.Images) > 0 {
		t.Thumbnail = data.Album.Images[0].URL
	}
	return t
}

func kindOrUnknown(kind string) string {
	if kind == "" {
		return "unknown"
	}
	return kind
}

// handleSpotifyPlaylist implements download.spotify_playlist (§2.6).
// Track URLs are refused (bad_request). Collections resolve via the
// Web API (creds) or the yt-dlp flat page extract (no creds, honest
// note on every track). Lists cap at 50 (truncated:true beyond).
// Each track downloads via download.spotify (§2.5) with per-track
// progress callbacks into the run's checklist. mode=zip: one ZIP,
// file_path set, file_paths absent. mode=files: per-file list,
// file_path null. count = files actually downloaded (not the
// playlist length).
func (s *Service) handleSpotifyPlaylist(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Mode        string `json:"mode"`
		URL         string `json:"url"`
		RequestedBy int64  `json:"requested_by"`
		Async       bool   `json:"async"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download.spotify_playlist request: %v", err))
		}
	}
	mode := strings.ToLower(strings.TrimSpace(body.Mode))
	if mode == "" {
		mode = "zip"
	}
	if mode != "zip" && mode != "files" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown mode %q (want zip or files)", body.Mode))
	}
	url := strings.TrimSpace(body.URL)
	if url == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "url is required")
	}
	kind := spotifyURLKind(url)
	if kind == "track" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "use download.spotify for single track URLs")
	}
	if kind == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("not a Spotify URL: %q", url))
	}
	prefs := s.prefs.Get(body.RequestedBy)
	if err := checkDaemonFormat(prefs); err != nil {
		return nil, err
	}
	if !prefs.UseYouTube {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Spotify audio comes from the YouTube ladder, which is disabled in your download preferences (use_youtube=false); no Spotify audio source is available")
	}
	return s.runJob(ctx, req, "download.spotify_playlist", url, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		tracks, truncated, err := s.resolveCollection(ctx, url)
		if err != nil {
			return nil, err
		}
		if len(tracks) == 0 {
			return nil, uds.NewHandlerError(uds.ErrDownloadFailed, "Spotify collection is empty")
		}
		titles := make([]string, len(tracks))
		for i, t := range tracks {
			titles[i] = t.Artist + " - " + t.Title
		}
		run.SetTitles(titles)
		dest := s.destDir()
		var downloaded []string
		var failed []any
		for _, track := range tracks {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			title := track.Artist + " - " + track.Title
			run.SetTrack(title, "downloading", 0, nil, nil)
			// Each track downloads exactly like download.spotify:
			// Spotify metadata + YouTube-ladder audio.
			res, err := s.downloadTrack(ctx, run, track, dest)
			if err != nil {
				reason := humanizeFailure(err)
				run.SetTrack(title, "failed", 0, nil, &reason)
				failed = append(failed, []any{title, reason})
				continue
			}
			path, _, _, err := s.finishFile(body.RequestedBy, res, false)
			if err != nil {
				reason := humanizeFailure(err)
				run.SetTrack(title, "failed", 0, nil, &reason)
				failed = append(failed, []any{title, reason})
				continue
			}
			run.SetTrack(title, "done", 100, nil, nil)
			downloaded = append(downloaded, path)
		}
		if len(downloaded) == 0 {
			first := ""
			if len(failed) > 0 {
				if pair, ok := failed[0].([]any); ok && len(pair) == 2 {
					first = fmt.Sprintf("%v: %v", pair[0], pair[1])
				}
			}
			return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("nothing downloadable in this collection (first failure: %s)", first))
		}
		data := map[string]any{
			"count":     len(downloaded),
			"tracks":    tracks,
			"truncated": truncated,
		}
		if mode == "zip" {
			zipPath, err := zipFiles(dest, downloaded)
			if err != nil {
				return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("zipping playlist failed: %v", err))
			}
			// The per-track files stay on disk (the core sends the
			// zip); record the zip itself in the library index under
			// the collection.
			library.SharedIndex().Record(model.Track{ID: "spotify-playlist:" + resolver.ExtractSpotifyID(url), Title: "playlist", Source: "spotify"}, zipPath, statSize(zipPath))
			data["file_path"] = zipPath
			data["file_paths"] = nil
		} else {
			data["file_path"] = nil
			data["file_paths"] = downloaded
		}
		return data, nil
	})
}

// resolveCollection resolves a Spotify collection URL to tracks:
// Web API first; without creds the yt-dlp flat extract of the public
// page (downloaders resolve_collection fallback). The 50-track cap
// applies to both paths (truncated:true beyond).
func (s *Service) resolveCollection(ctx context.Context, rawURL string) ([]model.Track, bool, error) {
	client := newSpotifyCollectionClient()
	_, tracks, err := client.Collection(ctx, rawURL)
	if err == nil {
		truncated := false
		if len(tracks) > MaxPlaylistTracks {
			tracks = tracks[:MaxPlaylistTracks]
			truncated = true
		}
		return tracks, truncated, nil
	}
	// No creds (or the API failed): fall back to the yt-dlp flat
	// page extract when the daemon is available. A missing-creds
	// error is not fatal here — the fallback is the documented
	// behaviour. Any other API error is also retried via the
	// fallback; only a daemon failure ends the call.
	if s.deps == nil || s.deps.Registry == nil || s.deps.Registry.YtDlp() == nil {
		return nil, false, mapDownloadErr(err)
	}
	info, derr := s.deps.Registry.YtDlp().Extract(ctx, rawURL)
	if derr != nil {
		return nil, false, mapDownloadErr(err)
	}
	tracks = tracksFromFlatExtract(info, rawURL)
	if len(tracks) == 0 {
		return nil, false, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Spotify collection %q produced no tracks", rawURL))
	}
	truncated := false
	if len(tracks) > MaxPlaylistTracks {
		tracks = tracks[:MaxPlaylistTracks]
		truncated = true
	}
	return tracks, truncated, nil
}

// tracksFromFlatExtract maps a yt-dlp flat playlist extract to
// Spotify tracks (downloaders _embed_track_list / extract_info_flat
// fallback: entries carry id/title/uploader/duration).
func tracksFromFlatExtract(info map[string]any, pageURL string) []model.Track {
	entries, _ := info["entries"].([]any)
	var out []model.Track
	for _, e := range entries {
		m, ok := e.(map[string]any)
		if !ok {
			continue
		}
		title, _ := m["title"].(string)
		if title == "" {
			continue
		}
		id, _ := m["id"].(string)
		if id == "" {
			id = title
		}
		artist, _ := m["uploader"].(string)
		if artist == "" {
			artist, _ = m["artist"].(string)
		}
		duration := resolver.ParseDuration(m["duration"])
		out = append(out, model.Track{
			ID: id, Title: title, Artist: artist, Source: string(resolver.SourceSpotify),
			URL: pageURL, DurationSec: int64(duration),
			DurationStr: resolver.FormatDurationStr(duration, false),
		})
	}
	return out
}

// zipFiles zips files into <dest>/<8hex>.zip and returns its path.
// The zip entries are the basenames (no directory traversal).
func zipFiles(dest string, files []string) (string, error) {
	var randBytes [4]byte
	if _, err := rand.Read(randBytes[:]); err != nil {
		return "", err
	}
	name := "playlist-" + hex.EncodeToString(randBytes[:]) + ".zip"
	zipPath := filepath.Join(dest, name)
	fh, err := os.Create(zipPath)
	if err != nil {
		return "", err
	}
	defer fh.Close()
	zw := zip.NewWriter(fh)
	for _, f := range files {
		if err := addFileToZip(zw, f); err != nil {
			zw.Close()
			_ = os.Remove(zipPath)
			return "", err
		}
	}
	if err := zw.Close(); err != nil {
		_ = os.Remove(zipPath)
		return "", err
	}
	return zipPath, nil
}

func addFileToZip(zw *zip.Writer, path string) error {
	fh, err := os.Open(path)
	if err != nil {
		return err
	}
	defer fh.Close()
	st, err := fh.Stat()
	if err != nil {
		return err
	}
	w, err := zw.CreateHeader(&zip.FileHeader{Name: filepath.Base(path), Method: zip.Deflate, Modified: st.ModTime()})
	if err != nil {
		return err
	}
	_, err = io.Copy(w, fh)
	return err
}
