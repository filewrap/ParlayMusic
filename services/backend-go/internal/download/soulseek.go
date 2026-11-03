package download

import (
	"context"
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// soulseekRequest is the download.soulseek body (§2.7).
type soulseekRequest struct {
	Query       string `json:"query"`
	Artist      string `json:"artist"`
	Title       string `json:"title"`
	DurationSec int    `json:"duration_sec"`
	AcceptLossy bool   `json:"accept_lossy"`
	Album       *struct {
		Artist string `json:"artist"`
		Title  string `json:"title"`
		Tracks []struct {
			Title       string `json:"title"`
			Artist      string `json:"artist"`
			DurationSec int    `json:"duration_sec"`
		} `json:"tracks"`
	} `json:"album"`
	RequestedBy int64 `json:"requested_by"`
	Async       bool  `json:"async"`
}

// handleSoulseek implements download.soulseek (§2.7): single-track
// and album downloads via slskd. slskd down / not logged in ->
// engine_unavailable naming SLSKD_URL; search ran but nothing usable
// -> download_failed. Album: per-track checklist in the run, each
// track downloaded independently; failed lists [title, reason] per
// miss; album with zero downloads -> download_failed naming the
// first failure.
//
// Lossy gap (honest): the resolver's Soulseek Download hardcodes
// lossless-only — it exposes no accept_lossy parameter and no
// lossy-only detection, so accept_lossy=true cannot change the
// outcome today: the lossless attempt runs and, when it finds nothing
// usable, the run reports lossy_available=false (unknown, not
// claimed) and the call fails download_failed. The fix is a resolver
// API addition (a DownloadWithOptions carrying AcceptLossy, or an
// exported lossy search) — flagged in phase3/wave2-b-download.md.
// The handler never claims a lossy copy exists when it cannot know.
func (s *Service) handleSoulseek(ctx context.Context, req uds.Request) (any, error) {
	var body soulseekRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad download.soulseek request: %v", err))
		}
	}
	if body.Album != nil {
		return s.handleSoulseekAlbum(ctx, req, body)
	}
	query := strings.TrimSpace(body.Query)
	artist := strings.TrimSpace(body.Artist)
	title := strings.TrimSpace(body.Title)
	if query == "" && title == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query (or artist + title) is required")
	}
	if query == "" {
		query = strings.TrimSpace(artist + " - " + title)
	}
	if artist == "" || title == "" {
		// Split "Artist - Title" the way the resolver does.
		a, t := splitArtistTitle(query)
		if artist == "" {
			artist = a
		}
		if title == "" {
			title = t
		}
	}
	track := model.Track{Artist: artist, Title: title, Source: string(resolver.SourceSoulseek), DurationSec: int64(body.DurationSec)}
	return s.runJob(ctx, req, "download.soulseek", query, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		res, err := s.downloadTrack(ctx, run, track, s.destDir())
		if err != nil {
			return nil, err
		}
		path, size, oversize, err := s.finishFile(body.RequestedBy, res, true)
		if err != nil {
			return nil, err
		}
		data := fileResult(path, res.Track, size, oversize)
		data["peer"] = res.Peer
		data["format"] = res.Format
		if data["format"] == "" {
			data["format"] = "flac"
		}
		data["source"] = "soulseek"
		return data, nil
	})
}

// handleSoulseekAlbum downloads an album track by track (§2.7).
func (s *Service) handleSoulseekAlbum(ctx context.Context, req uds.Request, body soulseekRequest) (any, error) {
	album := body.Album
	if len(album.Tracks) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "album.tracks is required")
	}
	albumTitle := strings.TrimSpace(album.Title)
	if albumTitle == "" {
		albumTitle = "album"
	}
	return s.runJob(ctx, req, "download.soulseek", albumTitle, body.Async, func(ctx context.Context, run *Run) (map[string]any, error) {
		titles := make([]string, len(album.Tracks))
		for i, t := range album.Tracks {
			artist := strings.TrimSpace(t.Artist)
			if artist == "" {
				artist = strings.TrimSpace(album.Artist)
			}
			titles[i] = strings.TrimSpace(artist + " - " + t.Title)
		}
		run.SetTitles(titles)
		albumDir := filepath.Join(s.destDir(), resolver.SanitizeFilename(strings.TrimSpace(album.Artist+" - "+albumTitle)))
		if err := ensureDir(albumDir); err != nil {
			return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("cannot create album dir: %v", err))
		}
		var count int
		var failed [][]any
		format := ""
		for i, t := range album.Tracks {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			title := titles[i]
			artist := strings.TrimSpace(t.Artist)
			if artist == "" {
				artist = strings.TrimSpace(album.Artist)
			}
			run.SetTrack(title, "downloading", 0, nil, nil)
			track := model.Track{Artist: artist, Title: strings.TrimSpace(t.Title), Source: string(resolver.SourceSoulseek), DurationSec: int64(t.DurationSec)}
			res, err := s.downloadTrack(ctx, run, track, albumDir)
			if err != nil {
				reason := humanizeFailure(err)
				run.SetTrack(title, "failed", 0, nil, &reason)
				failed = append(failed, []any{title, reason})
				continue
			}
			// Album files keep their downloaded names (the
			// path_template rename applies to single files only),
			// but the library index still records each file.
			if _, _, _, err := s.finishFile(body.RequestedBy, res, false); err != nil {
				reason := humanizeFailure(err)
				run.SetTrack(title, "failed", 0, nil, &reason)
				failed = append(failed, []any{title, reason})
				continue
			}
			if res.Format != "" {
				format = res.Format
			}
			run.SetTrack(title, "done", 100, nil, nil)
			count++
		}
		if count == 0 {
			first := ""
			if len(failed) > 0 && len(failed[0]) == 2 {
				first = fmt.Sprintf("%v: %v", failed[0][0], failed[0][1])
			}
			return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("album download failed: nothing downloaded (first failure: %s)", first))
		}
		return map[string]any{
			"count":     count,
			"total":     len(album.Tracks),
			"failed":    failed,
			"album_dir": albumDir,
			"format":    format,
			"source":    "soulseek",
		}, nil
	})
}

// splitArtistTitle splits "Artist - Title" (or "Artist: Title"); a
// query with no separator is treated as a title.
func splitArtistTitle(q string) (string, string) {
	for _, sep := range []string{" - ", " – ", ": "} {
		if i := strings.Index(q, sep); i > 0 {
			return strings.TrimSpace(q[:i]), strings.TrimSpace(q[i+len(sep):])
		}
	}
	return "", strings.TrimSpace(q)
}
