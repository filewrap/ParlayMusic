// TikTok resolver — yt-dlp -J metadata via the persistent daemon,
// then a native HTTP fetch of the CDN audio (BENCHMARKS.md §2,
// locked). Ported from services/downloaders/src/tiktok.py.
//
// yt-dlp's own downloader stalls on TikTok CDN reads from this egress
// (proven 2026-11-02: "The read operation timed out", retries
// exhausted), while a plain HTTP GET of the same signed URL downloads
// the whole file in ~2 s. So the bytes come from a direct fetch; the
// native yt-dlp download is the fallback.
package resolver

import (
	"context"
	"regexp"
	"strings"
)

var tiktokURLRe = regexp.MustCompile(`(?i)(?:https?://)?(?:www\.|m\.)?(?:tiktok\.com/@[^/]+/video/|tiktok\.com/v/|vm\.tiktok\.com/|vt\.tiktok\.com/)(\w+)`)

// TikTokResolver resolves TikTok video URLs.
type TikTokResolver struct {
	ytdlp *YtDlpClient
}

// NewTikTokResolver builds a TikTok resolver. The daemon is required
// for metadata (yt-dlp -J); without it every resolve fails honestly.
func NewTikTokResolver(ytdlp *YtDlpClient) *TikTokResolver {
	return &TikTokResolver{ytdlp: ytdlp}
}

// Source implements Resolver.
func (t *TikTokResolver) Source() Source { return SourceTikTok }

// ParseTikTokURL returns the TikTok video id (or short code) in a
// URL, or "".
func ParseTikTokURL(url string) string {
	m := tiktokURLRe.FindStringSubmatch(strings.TrimSpace(url))
	if m == nil {
		return ""
	}
	return m[1]
}

// Resolve extracts metadata for a TikTok URL via the daemon and
// returns the track with the CDN audio URL as StreamURL.
func (t *TikTokResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	q = strings.TrimPrefix(strings.ToLower(q), "tiktok:")
	vid := ParseTikTokURL(q)
	if vid == "" {
		return Track{}, engineUnavailable(SourceTikTok, "not a TikTok video URL: %q", query)
	}
	info, err := t.extract(ctx, q)
	if err != nil {
		return Track{}, err
	}
	return tiktokTrackFromInfo(info, vid, q), nil
}

// Search is not supported for TikTok (no search API exists — the
// source has no public audio search). Callers resolve direct URLs.
func (t *TikTokResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	return nil, engineUnavailable(SourceTikTok, "TikTok has no search API; resolve a direct video URL instead")
}

// extract runs yt-dlp -J metadata through the persistent daemon.
func (t *TikTokResolver) extract(ctx context.Context, url string) (map[string]any, error) {
	if t.ytdlp == nil {
		return nil, engineUnavailable(SourceTikTok, "yt-dlp daemon is not configured; cannot extract TikTok metadata")
	}
	return t.ytdlp.Extract(ctx, url)
}

// Download extracts metadata via the daemon, fetches the CDN audio
// with a native HTTP GET (Referer tiktok.com), and falls back to a
// native yt-dlp download when the direct fetch fails.
func (t *TikTokResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	info, err := t.extract(ctx, track.URL)
	if err != nil {
		return DownloadResult{}, err
	}
	if track.Title == "" || track.Title == "TikTok video" {
		track = tiktokTrackFromInfo(info, track.ID, track.URL)
	}
	name := SanitizeFilename(track.Artist+" - "+track.Title) + ".mp3"
	if audioURL := bestTikTokAudioURL(info); audioURL != "" {
		if res, err := downloadHTTPWithHeaders(ctx, track, destDir, name, audioURL, runID,
			map[string]string{"Referer": "https://www.tiktok.com/"}, onProgress); err == nil {
			return res, nil
		}
		// Direct CDN fetch failed: fall through to the daemon.
	}
	if t.ytdlp == nil {
		return DownloadResult{}, engineUnavailable(SourceTikTok, "TikTok direct CDN fetch failed and the yt-dlp daemon is not configured")
	}
	return downloadViaDaemon(ctx, t.ytdlp, track, destDir, onProgress)
}

// tiktokTrackFromInfo maps a yt-dlp -J info dict to a Track.
func tiktokTrackFromInfo(info map[string]any, vid, url string) Track {
	id, _ := info["id"].(string)
	if id == "" {
		id = vid
	}
	title, _ := info["title"].(string)
	if title == "" {
		title = "TikTok video " + vid
	}
	artist, _ := info["uploader"].(string)
	if artist == "" {
		artist, _ = info["channel"].(string)
	}
	duration := ParseDuration(info["duration"])
	t := Track{
		ID: id, Title: title, Artist: artist, Source: string(SourceTikTok),
		DurationSec: int64(duration),
		DurationStr: FormatDurationStr(duration, false),
		URL:         url,
	}
	if page, _ := info["webpage_url"].(string); page != "" {
		t.URL = page
	}
	if thumb, _ := info["thumbnail"].(string); thumb != "" {
		t.Thumbnail = thumb
	}
	if u := bestTikTokAudioURL(info); u != "" {
		t.StreamURL = u
	}
	return t
}

// bestTikTokAudioURL picks the audio-only format URL: mp3 first,
// else the highest-bitrate audio-only format (downloaders tiktok.py
// _best_audio_url).
func bestTikTokAudioURL(info map[string]any) string {
	formats, ok := info["formats"].([]any)
	if !ok {
		return ""
	}
	best := ""
	bestAbr := -1.0
	for _, f := range formats {
		fm, ok := f.(map[string]any)
		if !ok {
			continue
		}
		acodec, _ := fm["acodec"].(string)
		vcodec, _ := fm["vcodec"].(string)
		u, _ := fm["url"].(string)
		if u == "" || acodec == "" || acodec == "none" || (vcodec != "" && vcodec != "none") {
			continue
		}
		ext, _ := fm["ext"].(string)
		if ext == "mp3" {
			return u
		}
		abr, _ := fm["abr"].(float64)
		if abr > bestAbr {
			bestAbr = abr
			best = u
		}
	}
	return best
}
