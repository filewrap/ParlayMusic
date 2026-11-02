// Package resolver turns a /v1/play query (title, YouTube URL, or track id)
// into a Track.
//
// Primary path — yt-dlp (the same tool TgMusicBot and
// telegram-music-bot-rust shell out to): `yt-dlp -j --no-playlist` returns
// one JSON object per video; we read id/title/duration/uploader/thumbnail
// and the direct audio URL. Bounded by a 30s timeout, no shell involved
// (exec.Command with an argv, never sh -c).
//
// Degraded path — when yt-dlp is not installed or the call fails: the
// query is resolved by URL pattern only. A YouTube watch/shorts/live URL
// yields a Track with the video id, source youtube and title=query; a bare
// search string yields a Track with title=query and an id derived from the
// query. The service keeps serving (healthz reports degraded=true) and the
// core can still queue the item; actual audio resolution is deferred to
// the downloaders service or a later retry.
package resolver

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os/exec"
	"regexp"
	"strings"
	"time"

	"parlay/vc-go/internal/types"
)

// Resolver resolves play queries into Tracks.
type Resolver struct {
	// YtDlp is the yt-dlp binary path, or "" when unavailable.
	YtDlp string
}

// New probes PATH for yt-dlp at startup.
func New() *Resolver {
	path, _ := exec.LookPath("yt-dlp")
	return &Resolver{YtDlp: path}
}

// Available reports whether the yt-dlp path is usable.
func (r *Resolver) Available() bool { return r.YtDlp != "" }

var (
	ytWatchRe  = regexp.MustCompile(`[?&]v=([A-Za-z0-9_-]{6,})`)
	ytShortRe  = regexp.MustCompile(`youtu\.be/([A-Za-z0-9_-]{6,})`)
	ytShortsRe = regexp.MustCompile(`/shorts/([A-Za-z0-9_-]{6,})`)
	ytLiveRe   = regexp.MustCompile(`/live/([A-Za-z0-9_-]{6,})`)
)

// youtubeVideoID extracts a video id from a YouTube URL, or "".
func youtubeVideoID(raw string) string {
	for _, re := range []*regexp.Regexp{ytWatchRe, ytShortRe, ytShortsRe, ytLiveRe} {
		if m := re.FindStringSubmatch(raw); m != nil {
			return m[1]
		}
	}
	return ""
}

// IsYouTubeURL reports whether raw looks like a YouTube link.
func IsYouTubeURL(raw string) bool {
	u, err := url.Parse(raw)
	if err != nil {
		return false
	}
	host := strings.ToLower(u.Host)
	return host == "youtube.com" || host == "www.youtube.com" ||
		host == "music.youtube.com" || host == "youtu.be"
}

// ytDlpJSON is the subset of yt-dlp -j output we use.
type ytDlpJSON struct {
	ID         string  `json:"id"`
	Title      string  `json:"title"`
	Duration   float64 `json:"duration"`
	Uploader   string  `json:"uploader"`
	Thumbnail  string  `json:"thumbnail"`
	URL        string  `json:"url"` // direct stream URL, when present
	WebpageURL string  `json:"webpage_url"`
}

// resolveViaYtDlp runs yt-dlp -j for one query/URL.
func (r *Resolver) resolveViaYtDlp(ctx context.Context, query string) (*types.Track, error) {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	// "ytsearch1:<query>" for bare searches, raw URL otherwise — the same
	// split TgMusicBot's downloader makes.
	arg := query
	if !IsYouTubeURL(query) && !strings.HasPrefix(query, "http") {
		arg = "ytsearch1:" + query
	}
	cmd := exec.CommandContext(ctx, r.YtDlp, "-j", "--no-playlist", "--no-warnings", arg)
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("yt-dlp: %w", err)
	}
	// -j prints one JSON object per line; take the first.
	line := strings.TrimSpace(strings.SplitN(string(out), "\n", 2)[0])
	var info ytDlpJSON
	if err := json.Unmarshal([]byte(line), &info); err != nil {
		return nil, fmt.Errorf("yt-dlp: bad JSON: %w", err)
	}
	if info.ID == "" {
		return nil, errors.New("yt-dlp: empty result")
	}
	t := &types.Track{
		ID:          "yt:" + info.ID,
		Title:       info.Title,
		Source:      "youtube",
		Artist:      info.Uploader,
		DurationSec: int64(info.Duration),
		Thumbnail:   info.Thumbnail,
		StreamURL:   info.URL,
	}
	if info.WebpageURL != "" {
		t.URL = info.WebpageURL
	} else {
		t.URL = "https://www.youtube.com/watch?v=" + info.ID
	}
	if t.Title == "" {
		t.Title = info.ID
	}
	return t, nil
}

// degradedTrack builds the pattern-only Track used when yt-dlp is missing
// or fails.
func degradedTrack(query string) *types.Track {
	if id := youtubeVideoID(query); id != "" {
		return &types.Track{
			ID:     "yt:" + id,
			Title:  query,
			Source: "youtube",
			URL:    "https://www.youtube.com/watch?v=" + id,
		}
	}
	sum := sha1.Sum([]byte(query))
	return &types.Track{
		ID:     "query:" + hex.EncodeToString(sum[:8]),
		Title:  query,
		Source: "youtube",
		URL:    query, // may be a raw URL the core can hand to downloaders
	}
}

// Related resolves the next autoplay candidate for last, skipping every
// id in exclude. Upstream semantics (YukkiMusic platforms.AutoplayTracks /
// TgMusicBot downloader.GetRecommendations): only YouTube tracks autoplay,
// from the YouTube mix playlist "RD"+videoID, fetched with yt-dlp as a
// flat playlist; the first entry not in exclude wins. When yt-dlp is
// unavailable or the mix is empty, (nil, nil) — the room idles.
func (r *Resolver) Related(ctx context.Context, last types.Track, exclude map[string]bool) (*types.Track, error) {
	if last.Source != "youtube" {
		return nil, nil
	}
	vid := strings.TrimPrefix(last.ID, "yt:")
	if vid == "" || !r.Available() {
		return nil, nil
	}
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	mixURL := "https://www.youtube.com/playlist?list=RD" + vid
	cmd := exec.CommandContext(ctx, r.YtDlp, "-j", "--flat-playlist",
		"--no-warnings", "--playlist-end", "10", mixURL)
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("yt-dlp mix: %w", err)
	}
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var info ytDlpJSON
		if err := json.Unmarshal([]byte(line), &info); err != nil || info.ID == "" {
			continue
		}
		id := "yt:" + info.ID
		if id == last.ID || exclude[id] {
			continue
		}
		t := &types.Track{
			ID:          id,
			Title:       info.Title,
			Source:      "youtube",
			Artist:      info.Uploader,
			DurationSec: int64(info.Duration),
			Thumbnail:   info.Thumbnail,
			URL:         "https://www.youtube.com/watch?v=" + info.ID,
		}
		if t.Title == "" {
			t.Title = info.ID
		}
		return t, nil
	}
	return nil, nil
}

// Resolve turns a play query into a Track. It tries yt-dlp first when
// available and falls back to the degraded pattern-only track on any
// failure, returning degraded=true in that case.
func (r *Resolver) Resolve(ctx context.Context, query string) (*types.Track, bool, error) {
	query = strings.TrimSpace(query)
	if query == "" {
		return nil, false, errors.New("empty query")
	}
	if r.Available() {
		if t, err := r.resolveViaYtDlp(ctx, query); err == nil {
			return t, false, nil
		} else {
			// Log-and-degrade: the queue must keep working even when
			// resolution fails (matches the contract's degraded mode).
			fmt.Printf("vc-go resolver: yt-dlp failed for %q: %v; degrading\n", query, err)
		}
	}
	return degradedTrack(query), true, nil
}
