// Package netease implements the netease.* UDS actions
// (phase3/actions-inventory.md §5): thin handlers over the absorbed
// NetEase client (resolver/netease + resolver.NewNetEaseResolver).
// Search, lossless FLAC download, synced lyrics.
//
// Lossless FLAC needs NETEASE_MUSIC_U: without it the resolver
// degrades through lower levels and the mirror fallback and the
// returned file may be MP3 — the response reports auth honestly
// ("music_u"|"anonymous") and never claims FLAC it did not get.
// When no playable URL resolves at any level, the download fails
// honestly (download_failed). Oversize (NetEase-specific, stricter
// than the downloaders rule): the 50 MB cap is checked before
// writing and mid-download; an oversize track returns 200 with an
// empty file_path (queued for a later MTProto pass, not downloaded).
//
// SEAM (landed, Wave 2 verification): lyrics go through the shared
// lyrics.Fetcher (internal/lyrics) — its FetchNetEase path only,
// never Fetch: Fetch's LRCLib fallback could return LRCLib lyrics
// labelled source:"netease" and break the no-lyrics → 404 rule.
// Spotify keeps its own Genius→Musixmatch→LRCLib chain and does not
// go through the Fetcher.
package netease

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/resolver/netease"
	"parlay/backend-go/internal/uds"
)

// lyricsFetcher is the synced-lyrics source for netease.lyrics: the
// shared Fetcher's NetEase-only path (FetchNetEase). A fake in tests
// implements the same one-method interface.
type lyricsFetcher interface {
	FetchNetEase(ctx context.Context, neteaseID string) (*lyrics.Result, error)
}

// Service owns the netease.* handlers.
type Service struct {
	registry *resolver.Registry
	lyrics   lyricsFetcher
	netease  *resolver.NetEaseResolver
}

// Register registers netease.search / download / lyrics on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("netease: Register called with nil deps")
	}
	svc := &Service{registry: d.Registry}
	// Guard the assignment: a nil *lyrics.Fetcher stored in the
	// interface would be a non-nil interface and defeat the
	// s.lyrics == nil check in handleLyrics.
	if d.Lyrics != nil {
		svc.lyrics = d.Lyrics
	}
	if d.Registry != nil {
		if res, err := d.Registry.ResolverFor(resolver.SourceNetEase); err == nil {
			if ne, ok := res.(*resolver.NetEaseResolver); ok {
				svc.netease = ne
			}
		}
	}
	s.Register("netease.search", svc.handleSearch)
	s.Register("netease.download", svc.handleDownload)
	s.Register("netease.lyrics", svc.handleLyrics)
}

// parseTrackID accepts a track id as a JSON string ("123",
// "netease:123", a music.163.com URL) or a JSON number. The core
// sends track_id as a string or a number — the backend accepts both
// (inventory §5).
func parseTrackID(raw json.RawMessage) (int64, error) {
	if len(raw) == 0 {
		return 0, fmt.Errorf("track_id is required")
	}
	var n int64
	if err := json.Unmarshal(raw, &n); err == nil && n != 0 {
		return n, nil
	}
	var s string
	if err := json.Unmarshal(raw, &s); err == nil {
		if id := resolver.ExtractNetEaseID(s); id != 0 {
			return id, nil
		}
	}
	return 0, fmt.Errorf("track_id %s is not a NetEase track id", strings.TrimSpace(string(raw)))
}

func (s *Service) handleSearch(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Query string `json:"query"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	if strings.TrimSpace(body.Query) == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query is required")
	}
	if s.registry == nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "NetEase resolver not configured")
	}
	tracks, err := s.registry.Search(ctx, resolver.SourceNetEase, body.Query, 10)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("NetEase search failed: %v", err))
	}
	if tracks == nil {
		tracks = []model.Track{}
	}
	return map[string]any{"results": tracks}, nil
}

func (s *Service) handleDownload(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		TrackID     json.RawMessage `json:"track_id"`
		RequestedBy *int64          `json:"requested_by"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	id, err := parseTrackID(body.TrackID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("%v", err))
	}
	if s.registry == nil || s.netease == nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "NetEase resolver not configured")
	}
	// Metadata first (title/artist for the filename and response).
	track, err := s.registry.Resolve(ctx, "netease:"+strconv.FormatInt(id, 10))
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("NetEase track %d: %v", id, err))
	}
	destDir := resolver.DefaultDownloadDir()
	result, derr := s.netease.Download(ctx, track, destDir, nil)
	auth := s.netease.AuthMode()
	if result.Oversize {
		// Oversize rule (§5.2): no oversize file is written — the
		// partial file is deleted and the response is a 200 with
		// an empty path (queued for a later MTProto pass).
		if result.FilePath != "" {
			_ = os.Remove(result.FilePath)
		}
		return map[string]any{
			"file_path": "", "track": track, "oversize": true,
			"size_bytes": result.SizeBytes, "auth": auth,
			"run_id": result.RunID,
			"note":   "file exceeds the 50 MB Bot API cap — queued for a later MTProto pass, not downloaded",
		}, nil
	}
	if derr != nil {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("NetEase download failed for track %d: %v", id, derr))
	}
	return map[string]any{
		"file_path": result.FilePath, "track": result.Track,
		"run_id": result.RunID, "size_bytes": result.SizeBytes,
		"oversize": false, "auth": auth,
	}, nil
}

func (s *Service) handleLyrics(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		TrackID json.RawMessage `json:"track_id"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	id, err := parseTrackID(body.TrackID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("%v", err))
	}
	if s.lyrics == nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "NetEase lyrics resolver not configured")
	}
	// NetEase ONLY — never the shared Fetch path: its LRCLib
	// fallback could return LRCLib lyrics labelled "netease".
	result, err := s.lyrics.FetchNetEase(ctx, strconv.FormatInt(id, 10))
	if err != nil {
		// No lyrics is a 404, not an engine error.
		if isNoLyrics(err) {
			return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no lyrics found for NetEase track %d", id))
		}
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("NetEase lyrics failed for track %d: %v", id, err))
	}
	if result == nil || strings.TrimSpace(result.Lyrics) == "" {
		return nil, uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf("no lyrics found for NetEase track %d", id))
	}
	// Title/artist come from a detail lookup; empty on failure —
	// the lyrics are still returned.
	title, artist := "", ""
	if s.registry != nil {
		if track, rerr := s.registry.Resolve(ctx, "netease:"+strconv.FormatInt(id, 10)); rerr == nil {
			title, artist = track.Title, track.Artist
		}
	}
	// Write the .lrc to the download dir (superset lrc_path; "" when
	// the write fails — the lyrics are still returned).
	lrcPath := ""
	destDir := resolver.DefaultDownloadDir()
	if err := os.MkdirAll(destDir, 0o755); err == nil {
		p := filepath.Join(destDir, fmt.Sprintf("netease-%d.lrc", id))
		if err := os.WriteFile(p, []byte(result.Lyrics), 0o644); err == nil {
			lrcPath = p
		}
	}
	return map[string]any{
		"lyrics": result.Lyrics, "source": "netease",
		"track_id": strconv.FormatInt(id, 10), "title": title, "artist": artist,
		"truncated": false, "lrc_path": lrcPath,
	}, nil
}

// isNoLyrics reports whether err is (or wraps) a "no lyrics"
// error — either the absorbed client's ErrNoLyrics or the shared
// Fetcher's ErrNoLyrics (FetchNetEase wraps both).
func isNoLyrics(err error) bool {
	return errors.Is(err, netease.ErrNoLyrics) || errors.Is(err, lyrics.ErrNoLyrics)
}
