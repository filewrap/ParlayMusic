// NetEase resolver — absorbs services/netease-go into this package
// (FINAL-ARCHITECTURE.md §8.1: netease-go DELETE as service, absorb
// into backend-go). The crypto is NOT rewritten: the weapi/eapi/
// linuxapi implementation lives verbatim in internal/resolver/netease
// (copied from services/netease-go/internal/netease, crypto exact —
// see the package's crypto_test.go, which runs in this module).
//
// Endpoints: search (POST /api/cloudsearch/pc), song detail, song
// URL (eapi v1 levels standard→hires, weapi v1, weapi legacy,
// eapi download br=999000, linuxapi forward, public mirror), synced
// lyrics (GET /api/song/lyric), lossless FLAC download when the
// level allows. Login is env-gated on NETEASE_MUSIC_U (a MUSIC_U
// cookie); without it the service is anonymous-only and the mirror
// fallback covers downloads. The cookie value is never logged.
package resolver

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"parlay/backend-go/internal/resolver/netease"
)

// NetEaseResolver resolves NetEase tracks.
type NetEaseResolver struct {
	client *netease.Client
}

// NewNetEaseResolver builds a NetEase resolver (reads
// $NETEASE_MUSIC_U once, via netease.New).
func NewNetEaseResolver() *NetEaseResolver {
	return &NetEaseResolver{client: netease.New()}
}

// Source implements Resolver.
func (n *NetEaseResolver) Source() Source { return SourceNetEase }

// AuthMode reports "music_u" when a login cookie is set, else
// "anonymous" (netease.Client.AuthMode).
func (n *NetEaseResolver) AuthMode() string { return n.client.AuthMode() }

// Resolve handles netease:<id>, a music.163.com song URL, a bare
// numeric id, or free text (top search hit, then SongURL for the
// stream URL).
func (n *NetEaseResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	q = strings.TrimPrefix(strings.ToLower(q), "netease:")
	if id := ExtractNetEaseID(q); id != 0 {
		track, err := n.resolveID(ctx, id)
		if err != nil {
			return Track{}, err
		}
		return track, nil
	}
	tracks, err := n.Search(ctx, q, 1)
	if err != nil {
		return Track{}, err
	}
	if len(tracks) == 0 {
		return Track{}, engineUnavailable(SourceNetEase, "no NetEase results for %q", query)
	}
	return tracks[0], nil
}

// resolveID fetches metadata + a playable URL for one id.
func (n *NetEaseResolver) resolveID(ctx context.Context, id int64) (Track, error) {
	detail, err := n.client.SongDetail(id)
	if err != nil {
		return Track{}, err
	}
	track := neteaseTrackToModel(detail)
	if u, _, err := n.client.SongURL(id); err == nil && u != "" {
		track.StreamURL = u
	} else {
		return Track{}, fmt.Errorf("netease song url: track %d has no playable URL without login (eapi/weapi/linuxapi all returned url:null; mirror fallback also failed)", id)
	}
	return track, nil
}

// Search runs a NetEase search (type=1: single tracks) and fills
// each hit's StreamURL via SongURL (one call per hit — the search
// payload carries no stream URL).
func (n *NetEaseResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	q := strings.TrimSpace(strings.TrimPrefix(query, "netease:"))
	if q == "" {
		return nil, engineUnavailable(SourceNetEase, "empty search query")
	}
	results, err := n.client.SearchSong(q, limit)
	if err != nil {
		return nil, engineUnavailable(SourceNetEase, "NetEase search failed: %v", err)
	}
	tracks := make([]Track, 0, len(results))
	for _, r := range results {
		t := neteaseTrackToModel(r)
		if id, err := strconv.ParseInt(r.ID, 10, 64); err == nil {
			if u, _, err := n.client.SongURL(id); err == nil && u != "" {
				t.StreamURL = u
			}
		}
		tracks = append(tracks, t)
	}
	return tracks, nil
}

// Lyrics fetches the synced LRC for one NetEase track id.
func (n *NetEaseResolver) Lyrics(id int64) (string, error) {
	return n.client.Lyrics(id)
}

// Download resolves the playable URL (lossless FLAC when the level
// allows) and streams it to destDir, aborting past the 50 MB Bot
// API cap (netease.MaxBotAPIBYTES) with oversize=true.
func (n *NetEaseResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	id, err := strconv.ParseInt(strings.TrimPrefix(track.ID, "netease:"), 10, 64)
	if err != nil {
		id = ExtractNetEaseID(track.URL)
	}
	if id == 0 {
		return DownloadResult{}, engineUnavailable(SourceNetEase, "NetEase download needs a numeric track id, got %q", track.ID)
	}
	url, _, err := n.client.SongURL(id)
	if err != nil {
		return DownloadResult{}, err
	}
	if url == "" {
		return DownloadResult{}, engineUnavailable(SourceNetEase, "NetEase track %d has no playable URL without login", id)
	}
	ext := ".mp3"
	if strings.Contains(strings.ToLower(url), ".flac") || strings.Contains(url, "flac") {
		ext = ".flac"
	}
	name := SanitizeFilename(track.Artist+" - "+track.Title) + ext
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return DownloadResult{}, err
	}
	destPath := filepath.Join(destDir, name)
	fh, err := os.Create(destPath)
	if err != nil {
		return DownloadResult{}, err
	}
	defer fh.Close()
	if onProgress != nil {
		onProgress(Progress{RunID: runID, Phase: "downloading"})
	}
	written, oversize, err := n.client.Download(url, fh)
	if err != nil {
		return DownloadResult{}, err
	}
	if oversize {
		return DownloadResult{FilePath: destPath, Track: track, RunID: runID, Oversize: true, SizeBytes: written},
			fmt.Errorf("netease download: file exceeds the 50 MB Bot API cap (%d bytes written, truncated)", written)
	}
	if onProgress != nil {
		size := written
		onProgress(Progress{RunID: runID, Percentage: 100, DoneBytes: size, TotalBytes: &size, Phase: "done"})
	}
	return DownloadResult{FilePath: destPath, Track: track, RunID: runID, SizeBytes: written}, nil
}

// neteaseTrackToModel maps the absorbed netease.Track to the shared
// contract Track.
func neteaseTrackToModel(t netease.Track) Track {
	return Track{
		ID: t.ID, Title: t.Title, Artist: t.Artist,
		Source:      string(SourceNetEase),
		DurationSec: int64(t.DurationSec),
		DurationStr: t.DurationStr,
		Thumbnail:   t.Thumbnail,
		URL:         t.URL,
	}
}

// ExtractNetEaseID returns the numeric song id in a NetEase URL
// (music.163.com/song?id=...), a netease: id, or a bare numeric id.
// Returns 0 when the input is not a NetEase track reference.
func ExtractNetEaseID(input string) int64 {
	q := strings.TrimSpace(input)
	q = strings.TrimPrefix(strings.ToLower(q), "netease:")
	if strings.Contains(q, "music.163.com") || strings.Contains(q, "music.126.net") {
		if i := strings.Index(q, "id="); i >= 0 {
			rest := q[i+3:]
			if j := strings.IndexAny(rest, "&#"); j >= 0 {
				rest = rest[:j]
			}
			if id, err := strconv.ParseInt(rest, 10, 64); err == nil {
				return id
			}
		}
		return 0
	}
	if id, err := strconv.ParseInt(q, 10, 64); err == nil {
		return id
	}
	return 0
}
