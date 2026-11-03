package search

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parlay/backend-go/internal/download"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// LemonicClient talks to the Lemonic API (base ${LEMONIC_API},
// default https://api.lemonic.xyz). Upstream (lemonic-telegram-bot
// modules/lemonic.ts) does an UNAUTHENTICATED fetch to
// /api/raw/track/<id> — no API key exists upstream; when
// LEMONIC_API_KEY is set, requests additionally carry
// `Authorization: Bearer <key>` (keyed path). Search falls back to
// Spotify client-credentials search when the Lemonic endpoint fails
// or is unavailable (lemonic-telegram-bot's own search path).
type LemonicClient struct {
	base   string
	key    string
	http   *http.Client
	dlHTTP *http.Client
}

// NewLemonicClient builds a client from the environment
// (LEMONIC_API, LEMONIC_API_KEY). No key is required: the
// unauthenticated path is upstream behaviour.
func NewLemonicClient() *LemonicClient {
	base := strings.TrimRight(strings.TrimSpace(os.Getenv("LEMONIC_API")), "/")
	if base == "" {
		base = "https://api.lemonic.xyz"
	}
	return &LemonicClient{
		base:   base,
		key:    strings.TrimSpace(os.Getenv("LEMONIC_API_KEY")),
		http:   &http.Client{Timeout: 10 * time.Second},
		dlHTTP: &http.Client{Timeout: 60 * time.Second},
	}
}

// NewLemonicClientWithBase builds a client against an explicit base
// URL (tests use an httptest server).
func NewLemonicClientWithBase(base, key string) *LemonicClient {
	c := NewLemonicClient()
	if strings.TrimSpace(base) != "" {
		c.base = strings.TrimRight(strings.TrimSpace(base), "/")
	}
	c.key = strings.TrimSpace(key)
	return c
}

// Base returns the API base URL.
func (c *LemonicClient) Base() string { return c.base }

// Configured reports whether a Lemonic API key is set.
func (c *LemonicClient) Configured() bool { return c.key != "" }

func (c *LemonicClient) headers() map[string]string {
	if c.key != "" {
		return map[string]string{"Authorization": "Bearer " + c.key}
	}
	return nil
}

// fetch performs one Lemonic GET, returning the decoded JSON body.
func (c *LemonicClient) fetch(ctx context.Context, path string) (map[string]any, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.base+path, nil)
	if err != nil {
		return nil, err
	}
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("Lemonic API request failed (%s%s): %w", c.base, path, err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("Lemonic API read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("Lemonic API returned HTTP %d for %s", resp.StatusCode, path)
	}
	var data map[string]any
	if err := json.Unmarshal(body, &data); err != nil {
		return nil, fmt.Errorf("Lemonic API %s: bad JSON: %w", path, err)
	}
	return data, nil
}

// lemonicTrack maps one raw Lemonic record to a model Track.
// Duration may arrive as seconds, "m:ss", or milliseconds.
func lemonicTrack(r map[string]any) model.Track {
	durationSec := 0
	if ms, ok := r["duration_ms"].(float64); ok && ms > 0 {
		durationSec = int(ms / 1000)
	} else {
		durationSec = resolver.ParseDuration(firstPresent(r, "duration", "duration_sec"))
	}
	id, _ := firstString(r, "id", "track_id")
	title, _ := firstString(r, "title", "name")
	artist := ""
	if a, ok := r["artist"].(string); ok && a != "" {
		artist = a
	} else if arr, ok := r["artists"].([]any); ok && len(arr) > 0 {
		var names []string
		for _, item := range arr {
			switch v := item.(type) {
			case string:
				names = append(names, v)
			case map[string]any:
				if n, ok := v["name"].(string); ok {
					names = append(names, n)
				}
			}
		}
		artist = strings.Join(names, ", ")
	}
	if artist == "" {
		artist = "Unknown"
	}
	if title == "" {
		title = "Unknown"
	}
	trackURL, _ := firstString(r, "url", "perma_url")
	thumb, _ := firstString(r, "thumbnail", "image")
	return model.Track{
		ID: "lemonic:" + id, Title: title, Artist: artist,
		DurationSec: int64(durationSec),
		DurationStr: resolver.FormatDurationStr(durationSec, false),
		URL:         trackURL, Thumbnail: thumb, Source: "lemonic",
	}
}

func firstPresent(r map[string]any, keys ...string) any {
	for _, k := range keys {
		if v, ok := r[k]; ok && v != nil {
			return v
		}
	}
	return nil
}

func firstString(r map[string]any, keys ...string) (string, bool) {
	for _, k := range keys {
		if v, ok := r[k].(string); ok && v != "" {
			return v, true
		}
	}
	return "", false
}

// Search runs lemonic.search: the Lemonic API's own search endpoint
// first; on failure, Spotify client-credentials search (via the
// registry — the same call the Spotify resolver makes), with results
// mapped to lemonic:<spotify-id>. It returns the tracks, the path
// taken ("lemonic", "lemonic-keyed", or "spotify-search"), and an
// error only when every fallback failed. A Lemonic answer with zero
// results is an honest empty result, not a failure (no Spotify
// fallback — exactly vc-ts searchLemonic).
func (c *LemonicClient) Search(ctx context.Context, registry *resolver.Registry, query string, limit int) ([]model.Track, string, error) {
	if limit <= 0 {
		limit = 5
	}
	var tried []string
	path := "/api/search?query=" + url.QueryEscape(query) + "&limit=" + fmt.Sprint(limit)
	data, err := c.fetch(ctx, path)
	if err == nil {
		raw := rawTrackList(data)
		if raw == nil {
			return nil, "", fmt.Errorf("Lemonic search returned an unexpected response shape")
		}
		if len(raw) > 0 {
			tracks := make([]model.Track, 0, len(raw))
			for _, r := range raw {
				if len(tracks) >= limit {
					break
				}
				tracks = append(tracks, lemonicTrack(r))
			}
			via := "lemonic"
			if c.Configured() {
				via = "lemonic-keyed"
			}
			return tracks, via, nil
		}
		return []model.Track{}, "lemonic", nil
	}
	tried = append(tried, fmt.Sprintf("lemonic /api/search: %v", err))
	if registry != nil {
		sp, serr := registry.Search(ctx, resolver.SourceSpotify, query, limit)
		if serr == nil && sp != nil {
			tracks := make([]model.Track, 0, len(sp))
			for _, t := range sp {
				id := strings.TrimPrefix(t.ID, "spotify:")
				tracks = append(tracks, model.Track{
					ID: "lemonic:" + id, Title: t.Title, Artist: t.Artist,
					DurationSec: t.DurationSec, DurationStr: t.DurationStr,
					URL: t.URL, Thumbnail: t.Thumbnail, Source: "lemonic",
				})
			}
			return tracks, "spotify-search", nil
		}
		if serr != nil {
			tried = append(tried, fmt.Sprintf("spotify search: %v", serr))
		} else {
			tried = append(tried, "spotify search: SPOTIFY_CLIENT_ID/SPOTIFY_CLIENT_SECRET not set")
		}
	} else {
		tried = append(tried, "spotify search: resolver registry not configured")
	}
	return nil, "", fmt.Errorf("Lemonic search unavailable — every fallback failed: %s", strings.Join(tried, "; "))
}

// rawTrackList extracts the track array from a Lemonic search
// response (results | tracks | data).
func rawTrackList(data map[string]any) []map[string]any {
	for _, key := range []string{"results", "tracks", "data"} {
		if arr, ok := data[key].([]any); ok {
			out := make([]map[string]any, 0, len(arr))
			for _, item := range arr {
				if m, ok := item.(map[string]any); ok {
					out = append(out, m)
				}
			}
			return out
		}
	}
	return nil
}

// RawTrack fetches one raw track: GET /api/raw/track/<id> (the
// "lemonic:" prefix is stripped first — the API takes the bare id).
// It returns the mapped track and the raw record (for the
// stream/download URL).
func (c *LemonicClient) RawTrack(ctx context.Context, id string) (model.Track, map[string]any, error) {
	bare := strings.TrimPrefix(strings.TrimSpace(id), "lemonic:")
	if bare == "" {
		return model.Track{}, nil, fmt.Errorf("track id is required")
	}
	data, err := c.fetch(ctx, "/api/raw/track/"+url.PathEscape(bare))
	if err != nil {
		return model.Track{}, nil, err
	}
	raw := data
	if inner, ok := data["data"].(map[string]any); ok {
		raw = inner
	}
	if raw["id"] == nil && raw["title"] == nil {
		return model.Track{}, nil, fmt.Errorf("Lemonic has no raw track for id %s", bare)
	}
	if _, ok := raw["id"]; !ok {
		raw["id"] = bare
	}
	return lemonicTrack(raw), raw, nil
}

// stripLemonicPrefix removes the "lemonic:" prefix for raw-track
// calls and file names.
func stripLemonicPrefix(id string) string {
	return strings.TrimPrefix(strings.TrimSpace(id), "lemonic:")
}

type lemonicSearchRequest struct {
	Query string `json:"query"`
	Limit int    `json:"limit"`
}

// handleLemonicSearch implements lemonic.search (§7.11).
func (s *Service) handleLemonicSearch(ctx context.Context, req uds.Request) (any, error) {
	var body lemonicSearchRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad lemonic.search request: %v", err))
		}
	}
	if strings.TrimSpace(body.Query) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query is required")
	}
	limit := body.Limit
	if limit <= 0 {
		limit = 5
	}
	if limit > 20 {
		limit = 20
	}
	tracks, via, err := s.lemonic.Search(ctx, s.depsRegistry(), body.Query, limit)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	}
	if tracks == nil {
		tracks = []model.Track{}
	}
	return map[string]any{"results": s.enrichWithLibrary(tracks), "via": via}, nil
}

type lemonicDownloadRequest struct {
	TrackID     string `json:"track_id"`
	RequestedBy int64  `json:"requested_by"`
}

// handleLemonicDownload implements lemonic.download (§7.11): raw
// track -> stream/download URL -> fetch to
// data/lemonic/<sanitised-id>.mp3 (60s timeout) -> library index.
// No stream URL -> 502 engine_unavailable. The download is tracked
// in the shared run registry (download.progress can poll it) and the
// response carries {file_path, track, size_bytes, run_id}.
func (s *Service) handleLemonicDownload(ctx context.Context, req uds.Request) (any, error) {
	var body lemonicDownloadRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad lemonic.download request: %v", err))
		}
	}
	trackID := strings.TrimSpace(body.TrackID)
	if trackID == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "track_id is required")
	}
	runRegistry := download.SharedRegistry()
	run := runRegistry.NewRun(ctx, "lemonic.download", trackID)
	run.SetTitles([]string{trackID})
	run.Report(resolver.Progress{Phase: "downloading", Percentage: 5})
	track, raw, err := s.lemonic.RawTrack(ctx, trackID)
	if err != nil {
		run.Fail(runRegistry, err.Error())
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	}
	run.SetTrack(track.Title, "downloading", 20, nil, nil)
	streamURL, _ := firstString(raw, "stream_url", "download_url", "url")
	if streamURL == "" {
		msg := fmt.Sprintf("Lemonic returned track %s but no stream/download URL; cannot download", trackID)
		run.Fail(runRegistry, msg)
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, msg)
	}
	run.Report(resolver.Progress{Phase: "downloading", Percentage: 30})
	dataDir := resolver.DataDir()
	if s.deps != nil && s.deps.Config != nil && s.deps.Config.Backend.DataDir != "" {
		dataDir = s.deps.Config.Backend.DataDir
	}
	dir := filepath.Join(dataDir, "lemonic")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		run.Fail(runRegistry, err.Error())
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("lemonic dir failed: %v", err))
	}
	safeID := sanitizeFileName(stripLemonicPrefix(trackID))
	filePath := filepath.Join(dir, safeID+".mp3")
	if err := s.fetchToFile(ctx, streamURL, filePath); err != nil {
		run.Fail(runRegistry, err.Error())
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Lemonic stream fetch failed: %v", err))
	}
	size := fileSize(filePath)
	run.SetFilePath(filePath)
	run.SetTrack(track.Title, "done", 100, nil, nil)
	run.Finish(runRegistry)
	if s.index != nil {
		s.index.Record(track, filePath, size)
	}
	track.SizeBytes = size
	return map[string]any{"file_path": filePath, "track": track, "size_bytes": size, "run_id": run.ID}, nil
}

// fetchToFile GETs url (60s timeout) and writes it atomically
// (tmp + rename) to filePath. An empty body is a failure, not a
// zero-byte file.
func (s *Service) fetchToFile(ctx context.Context, url string, filePath string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := s.lemonic.dlHTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	tmp := filePath + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	n, err := io.Copy(f, io.LimitReader(resp.Body, 512<<20))
	closeErr := f.Close()
	if err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if closeErr != nil {
		_ = os.Remove(tmp)
		return closeErr
	}
	if n == 0 {
		_ = os.Remove(tmp)
		return fmt.Errorf("Lemonic stream was empty")
	}
	if err := os.Rename(tmp, filePath); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// sanitizeFileName replaces characters unsafe for a file name with
// "_", mirroring the vc-ts download path.
func sanitizeFileName(name string) string {
	var b strings.Builder
	for _, r := range name {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		} else {
			b.WriteRune('_')
		}
	}
	out := b.String()
	if out == "" {
		out = "track"
	}
	return out
}

func fileSize(path string) int64 {
	st, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return st.Size()
}
