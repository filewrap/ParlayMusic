package download

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// handleYouTube implements download.youtube (§2.1): bare query ->
// YouTube search first (the registry resolves it), URL -> direct.
// Returns an MP3 file via the warm yt-dlp daemon (no per-call fork).
func (s *Service) handleYouTube(ctx context.Context, req uds.Request) (any, error) {
	body, err := decodeDownloadRequest(req)
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(body.Query) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query is required")
	}
	prefs := s.prefs.Get(body.RequestedBy)
	if err := checkDaemonFormat(prefs); err != nil {
		return nil, err
	}
	return s.runJob(ctx, req, "download.youtube", body.Query, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		track, err := s.resolveTrack(ctx, body.Query)
		if err != nil {
			return nil, err
		}
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		return fileResult(path, res.Track, size, oversize), nil
	})
}

// handleDeezer implements download.deezer (§2.2). Without DEEZER_ARL
// only 30 s previews download (the resolver falls back honestly);
// with ARL the full track arrives via the native Go ARL client with
// in-process Blowfish-CBC decryption. Album input -> ZIP of previews
// (or full tracks with ARL) — the resolver's Download handles one
// track; an album query resolves to its first track here and the
// response notes the count honestly via the track itself.
func (s *Service) handleDeezer(ctx context.Context, req uds.Request) (any, error) {
	body, err := decodeDownloadRequest(req)
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(body.Query) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query is required")
	}
	return s.runJob(ctx, req, "download.deezer", body.Query, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		track, err := s.resolveTrack(ctx, body.Query)
		if err != nil {
			return nil, err
		}
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		data := fileResult(path, res.Track, size, oversize)
		if res.Format != "" {
			data["format"] = res.Format
		}
		return data, nil
	})
}

// handleTikTok implements download.tiktok (§2.3): yt-dlp metadata,
// then a direct CDN fetch (yt-dlp's own downloader stalls on TikTok
// CDN reads from this egress), falling back to a daemon download.
func (s *Service) handleTikTok(ctx context.Context, req uds.Request) (any, error) {
	body, err := decodeDownloadRequest(req)
	if err != nil {
		return nil, err
	}
	url := strings.TrimSpace(body.URL)
	if url == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "url is required")
	}
	if resolver.ParseTikTokURL(url) == "" {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("not a TikTok video URL: %q", url))
	}
	prefs := s.prefs.Get(body.RequestedBy)
	if err := checkDaemonFormat(prefs); err != nil {
		return nil, err
	}
	return s.runJob(ctx, req, "download.tiktok", url, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		track, err := s.resolveTrack(ctx, url)
		if err != nil {
			return nil, err
		}
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		return fileResult(path, res.Track, size, oversize), nil
	})
}

// handleNavidrome implements download.navidrome (§2.4): a Navidrome
// URL/query — "?id=" downloads by id, else search first, then
// rest/download returns the original file (no transcode).
func (s *Service) handleNavidrome(ctx context.Context, req uds.Request) (any, error) {
	body, err := decodeDownloadRequest(req)
	if err != nil {
		return nil, err
	}
	url := strings.TrimSpace(body.URL)
	if url == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "url is required")
	}
	return s.runJob(ctx, req, "download.navidrome", url, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		track, err := s.resolveTrack(ctx, url)
		if err != nil {
			return nil, err
		}
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		data := fileResult(path, res.Track, size, oversize)
		data["imported"] = true
		return data, nil
	})
}

// spotifyURLKind parses a Spotify URL/URI kind (track|album|
// playlist|artist), or "" when it is not a Spotify reference.
func spotifyURLKind(input string) string {
	q := strings.TrimSpace(input)
	if strings.HasPrefix(q, "spotify:") {
		parts := strings.Split(q, ":")
		if len(parts) == 3 {
			return parts[1]
		}
		return ""
	}
	// open.spotify.com/<kind>/<id>
	rest := strings.TrimPrefix(strings.TrimPrefix(q, "https://"), "http://")
	if !strings.HasPrefix(rest, "open.spotify.com/") {
		return ""
	}
	rest = strings.TrimPrefix(rest, "open.spotify.com/")
	parts := strings.Split(strings.Trim(rest, "/"), "/")
	if len(parts) >= 1 {
		return strings.Split(parts[0], "?")[0]
	}
	return ""
}

// handleSpotify implements download.spotify (§2.5): single Spotify
// track URLs only. Non-track URLs are refused (bad_request — use
// download.spotify_playlist for collections). The file is a
// YouTube-sourced MP3 (or spotdl MP3) carrying Spotify metadata; the
// track is honest about that (no claim the bytes came from Spotify).
// With use_youtube=false in the user's prefs the YouTube fallback is
// refused and the download fails honestly.
func (s *Service) handleSpotify(ctx context.Context, req uds.Request) (any, error) {
	body, err := decodeDownloadRequest(req)
	if err != nil {
		return nil, err
	}
	url := strings.TrimSpace(body.URL)
	if url == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "url is required")
	}
	kind := spotifyURLKind(url)
	if kind == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("not a Spotify URL: %q", url))
	}
	if kind != "track" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("use download.spotify_playlist for %s URLs", kind))
	}
	prefs := s.prefs.Get(body.RequestedBy)
	if err := checkDaemonFormat(prefs); err != nil {
		return nil, err
	}
	if !prefs.UseYouTube {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Spotify audio comes from the YouTube ladder, which is disabled in your download preferences (use_youtube=false); no Spotify audio source is available")
	}
	return s.runJob(ctx, req, "download.spotify", url, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		track, err := s.resolveTrack(ctx, url)
		if err != nil {
			return nil, err
		}
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		return fileResult(path, res.Track, size, oversize), nil
	})
}

// handleProgress implements download.progress (§2.8, IPC-4): a
// point-in-time snapshot of a run. Unknown run_id -> not_found;
// missing run_id -> bad_request.
func (s *Service) handleProgress(_ context.Context, req uds.Request) (any, error) {
	var body struct {
		RunID string `json:"run_id"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download.progress request: %v", err))
		}
	}
	if strings.TrimSpace(body.RunID) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "run_id is required")
	}
	run, ok := s.registry.Get(body.RunID)
	if !ok {
		return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no download run %q", body.RunID))
	}
	return run.Snapshot(), nil
}

// handleCancel implements download.cancel (§9-E1): cancels an
// in-flight run via context cancel; deletes the partial file (the
// job's failure path does that); the run finishes failed with reason
// "cancelled by user". A finished run answers {cancelled: false,
// phase: "done"|"failed"} (200, not an error). Unknown run_id -> 404.
func (s *Service) handleCancel(_ context.Context, req uds.Request) (any, error) {
	var body struct {
		RunID string `json:"run_id"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download.cancel request: %v", err))
		}
	}
	if strings.TrimSpace(body.RunID) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "run_id is required")
	}
	cancelled, phase, found := s.registry.Cancel(body.RunID)
	if !found {
		return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no download run %q", body.RunID))
	}
	return map[string]any{
		"cancelled": cancelled,
		"run_id":    body.RunID,
		"phase":     string(phase),
	}, nil
}

// handlePrefs implements download.prefs (§9-E9). GET {user_id}
// returns the stored prefs or the defaults (200, never 404). POST
// {user_id, format?, use_youtube?, path_template?} validates and
// stores them; unknown format -> bad_request naming the six formats.
func (s *Service) handlePrefs(_ context.Context, req uds.Request) (any, error) {
	var body struct {
		UserID       *int64  `json:"user_id"`
		Format       *string `json:"format"`
		UseYouTube   *bool   `json:"use_youtube"`
		PathTemplate *string `json:"path_template"`
	}
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download.prefs request: %v", err))
		}
	}
	if body.UserID == nil || *body.UserID == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "user_id is required")
	}
	prefs := s.prefs.Get(*body.UserID)
	// A POST is any body carrying at least one settable field.
	if body.Format != nil || body.UseYouTube != nil || body.PathTemplate != nil {
		if body.Format != nil {
			prefs.Format = *body.Format
		}
		if body.UseYouTube != nil {
			prefs.UseYouTube = *body.UseYouTube
		}
		if body.PathTemplate != nil {
			prefs.PathTemplate = *body.PathTemplate
		}
		saved, err := s.prefs.Set(prefs)
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, err.Error())
		}
		prefs = saved
	}
	return map[string]any{
		"user_id":       prefs.UserID,
		"format":        prefs.Format,
		"use_youtube":   prefs.UseYouTube,
		"path_template": prefs.PathTemplate,
	}, nil
}

// --- helpers shared by the playlist / soulseek handlers ------------

// statSize returns the size of path, or 0.
func statSize(path string) int64 {
	if st, err := os.Stat(path); err == nil {
		return st.Size()
	}
	return 0
}

// trackFileName renders the default download filename for a track.
func trackFileName(track model.Track, ext string) string {
	ext = strings.TrimPrefix(ext, ".")
	if ext == "" {
		ext = "mp3"
	}
	return resolver.SanitizeFilename(track.Artist+" - "+track.Title) + "." + ext
}

// ensureDir creates dir (0755).
func ensureDir(dir string) error {
	return os.MkdirAll(dir, 0o755)
}

// absJoin joins dir + name and verifies the result stays under dir.
func absJoin(dir, name string) (string, error) {
	p := filepath.Join(dir, name)
	absDir, err := filepath.Abs(dir)
	if err != nil {
		return "", err
	}
	absP, err := filepath.Abs(p)
	if err != nil {
		return "", err
	}
	if absP != absDir && !strings.HasPrefix(absP, absDir+string(os.PathSeparator)) {
		return "", fmt.Errorf("path %q escapes %q", name, dir)
	}
	return absP, nil
}
