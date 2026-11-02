// Package resolver turns a /v1/play query (title, YouTube URL, or track id)
// into a Track.
//
// Primary path — yt-dlp (the same tool TgMusicBot and
// telegram-music-bot-rust shell out to): `yt-dlp -j --no-playlist` returns
// one JSON object per video; we read id/title/duration/uploader/thumbnail
// and the direct audio URL. Bounded by a 30s timeout, no shell involved
// (exec.Command with an argv, never sh -c). When YouTube answers its bot
// check, the call is retried once with the android player client (same
// retry as the downloaders service, vc-ts and vc-rust).
//
// There is NO degraded/pattern fallback any more (Phase 5, A9): a query
// yt-dlp cannot resolve is an error, surfaced by the server as
// `engine_unavailable` (HTTP 502) so the core can fail over to another
// engine — a fake track (title = the query, invented id, no duration)
// would sit in the queue forever and hide the failure. `healthz`
// still reports `degraded:true` when no yt-dlp binary is found, as an
// honest availability signal.
//
// yt-dlp discovery (B12): PATH first, then $YT_DLP, then the
// downloaders service venv (`services/downloaders/.venv/bin/yt-dlp`,
// probed exe-relative and cwd-relative), so the resolver works
// wherever the download path already works.
package resolver

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"parlay/vc-go/internal/types"
)

// Resolver resolves play queries into Tracks.
type Resolver struct {
	// YtDlp is the yt-dlp binary path, or "" when unavailable.
	YtDlp string
}

// probeYtDlp reports whether p runs and answers --version.
func probeYtDlp(p string) bool {
	if p == "" {
		return false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return exec.CommandContext(ctx, p, "--version").Run() == nil
}

// findVenvYtDlp walks up from the working directory looking for the
// downloaders service venv (downloaders/.venv/bin/yt-dlp, or
// services/downloaders/.venv/bin/yt-dlp at the repo root). Covers
// every caller depth: the deployed binary (cwd = repo root or
// services/vc-go), `go test` (cwd = an internal/* package dir), and
// a developer shell anywhere under the repo.
func findVenvYtDlp() string {
	dir, err := os.Getwd()
	if err != nil {
		return ""
	}
	for i := 0; i < 8; i++ {
		for _, rel := range []string{
			filepath.Join(dir, "downloaders/.venv/bin/yt-dlp"),
			filepath.Join(dir, "services/downloaders/.venv/bin/yt-dlp"),
		} {
			if probeYtDlp(rel) {
				return rel
			}
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return ""
}

// findYtDlp locates the yt-dlp binary: $YT_DLP, then PATH, then the
// downloaders service venv (exe-relative, then cwd-walk).
func findYtDlp() string {
	if v := os.Getenv("YT_DLP"); v != "" && probeYtDlp(v) {
		return v
	}
	if p, err := exec.LookPath("yt-dlp"); err == nil {
		return p
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		for _, rel := range []string{
			"../../downloaders/.venv/bin/yt-dlp",
			"../../../downloaders/.venv/bin/yt-dlp",
		} {
			if abs, err := filepath.Abs(filepath.Join(dir, rel)); err == nil && probeYtDlp(abs) {
				return abs
			}
		}
	}
	return findVenvYtDlp()
}

// New probes for a yt-dlp binary (see findYtDlp).
func New() *Resolver {
	return &Resolver{YtDlp: findYtDlp()}
}

// Available reports whether the yt-dlp path is usable.
func (r *Resolver) Available() bool { return r.YtDlp != "" }

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

// isBotCheck spots YouTube's bot-check gate in yt-dlp stderr — the
// same test as the downloaders service, vc-ts and vc-rust.
func isBotCheck(stderr string) bool {
	return strings.Contains(stderr, "not a bot") ||
		strings.Contains(stderr, "Sign in to confirm")
}

func trimErr(s string) string {
	s = strings.TrimSpace(strings.ReplaceAll(s, "\n", " "))
	if len(s) > 300 {
		s = s[:300]
	}
	return s
}

// runYtDlp runs yt-dlp with argv (no shell), capturing stdout and
// stderr separately. When YouTube answers its bot check, it retries
// once with the android player client (same retry as the downloaders
// service, vc-ts and vc-rust; no cookies or credentials).
func (r *Resolver) runYtDlp(ctx context.Context, args []string, timeout time.Duration) ([]byte, error) {
	run := func(extra []string) ([]byte, string, error) {
		ctx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		full := append(append([]string{}, extra...), args...)
		cmd := exec.CommandContext(ctx, r.YtDlp, full...)
		var stdout, stderr strings.Builder
		cmd.Stdout = &stdout
		cmd.Stderr = &stderr
		err := cmd.Run()
		return []byte(stdout.String()), stderr.String(), err
	}
	out, stderr, err := run(nil)
	if err == nil {
		return out, nil
	}
	if !isBotCheck(stderr) {
		return nil, fmt.Errorf("yt-dlp: %w (stderr: %s)", err, trimErr(stderr))
	}
	out, stderr, err = run([]string{"--extractor-args", "youtube:player_client=android"})
	if err != nil {
		return nil, fmt.Errorf("yt-dlp: %w (stderr: %s)", err, trimErr(stderr))
	}
	return out, nil
}

// resolveViaYtDlp runs yt-dlp -j for one query/URL.
func (r *Resolver) resolveViaYtDlp(ctx context.Context, query string) (*types.Track, error) {
	// "ytsearch1:<query>" for bare searches, raw URL otherwise — the same
	// split TgMusicBot's downloader makes.
	arg := query
	if !IsYouTubeURL(query) && !strings.HasPrefix(query, "http") {
		arg = "ytsearch1:" + query
	}
	out, err := r.runYtDlp(ctx, []string{"-j", "-f", "bestaudio/best", "--no-playlist", "--no-warnings", arg}, 30*time.Second)
	if err != nil {
		return nil, err
	}
	// -j prints one JSON object per line; take the first.
	// -f bestaudio/best matters: without a format selection the
	// top-level "url" is absent for multi-format videos, so StreamURL
	// would stay empty (A8 — the resolver never populated it).
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
	// Watch URL + list=RD<id> (same form vc-rust / vc-ts use, proven
	// live): the mix comes back flat, first unplayed entry wins.
	mixURL := "https://www.youtube.com/watch?v=" + vid + "&list=RD" + vid
	out, err := r.runYtDlp(ctx, []string{"-j", "--flat-playlist",
		"--no-warnings", "--playlist-end", "10", mixURL}, 30*time.Second)
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

// Resolve turns a play query into a Track. It uses yt-dlp when
// available; when yt-dlp is missing or the call fails it returns an
// error (no degraded/pattern fallback any more, Phase 5 A9) — the
// server maps it to 502 engine_unavailable so the core can fail over.
func (r *Resolver) Resolve(ctx context.Context, query string) (*types.Track, error) {
	query = strings.TrimSpace(query)
	if query == "" {
		return nil, errors.New("empty query")
	}
	if !r.Available() {
		return nil, fmt.Errorf("yt-dlp unavailable (no binary found; set YT_DLP or install yt-dlp): cannot resolve %q", query)
	}
	return r.resolveViaYtDlp(ctx, query)
}
