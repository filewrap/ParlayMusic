// YouTube resolver — native Go InnerTube (BENCHMARKS.md §2, locked).
//
// Strategy (measured Phase 1, same video dQw4w9WgXcQ): one in-process
// HTTPS POST, no subprocess — ANDROID_VR player 0.61 s / ANDROID player
// 0.81 s / WEB search 1.56 s, vs yt-dlp 5.7–6.7 s (7–10x faster).
// WEB player is UNPLAYABLE (metadata only), so the player call always
// uses ANDROID_VR (fallback ANDROID); search uses ANDROID first — WEB
// gets WAF 403 on datacenter IPs (VPS 2026-11-04: WEB search 403,
// resolves 6–13 s through the fallback chain) — with WEB second
// (params=CAASAhAB, video-only results) and the daemon last.
//
// Stream URL = streamingData.adaptiveFormats itag 251 (Opus) first,
// then 140 (AAC). Formats that carry only a signatureCipher (no direct
// URL) are skipped — those go to the warm yt-dlp daemon fallback,
// which is the only yt-dlp path in the backend.
//
// Port sources: refs/YukkiMusic-Go internal/platforms/youtube.go
// (callInnerTube, fetchVideo, parseNodes, extractVideoID),
// refs/ParvatiMusic bot/src/platforms/youtube.rs (call_innertube,
// search/player contexts), refs/ArcMusic internal/platforms/youtube.go.
package resolver

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// InnerTube client identities. WEB is for search (its client version
// comes from the refs); ANDROID_VR / ANDROID are for player (measured
// playability OK in Phase 1; IOS 19.29.1 answered HTTP 400 — stale
// client version, not retried).
const (
	innerTubeBase         = "https://m.youtube.com"
	innerTubeWeb          = "WEB"
	innerTubeWebVer       = "2.20250101.01.00"
	innerTubeAndroidVR    = "ANDROID_VR"
	innerTubeAndroidVRVer = "1.62.27"
	innerTubeAndroid      = "ANDROID"
	innerTubeAndroidVer   = "20.10.38"
)

// innerTubeUA is the mobile UA both refs send with InnerTube calls.
const innerTubeUA = "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36"

var (
	youtubeLinkRe = regexp.MustCompile(`(?i)^(?:https?://)?(?:www\.|m\.|music\.)?(?:youtube\.com|youtu\.be)/\S+`)
	videoIDRe1    = regexp.MustCompile(`(?i)(?:youtube\.com/(?:watch\?v=|embed/|shorts/|live/)|youtu\.be/)([A-Za-z0-9_-]{11})`)
	videoIDRe2    = regexp.MustCompile(`(?:v=|/)([0-9A-Za-z_-]{11})`)
	playlistIDRe1 = regexp.MustCompile(`(?i)(?:youtube\.com|music\.youtube\.com).*(?:\?|&)list=([A-Za-z0-9_-]+)`)
	playlistIDRe2 = regexp.MustCompile(`list=([0-9A-Za-z_-]+)`)
	bareVideoIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{11}$`)
)

// YouTubeResolver resolves YouTube URLs, bare 11-char ids and
// free-text search via native InnerTube.
type YouTubeResolver struct {
	ytdlp *YtDlpClient
}

// NewYouTubeResolver builds a YouTube resolver. ytdlp may be nil —
// the daemon fallback is then unavailable and ciphered formats fail
// honestly instead of silently.
func NewYouTubeResolver(ytdlp *YtDlpClient) *YouTubeResolver {
	return &YouTubeResolver{ytdlp: ytdlp}
}

// Source implements Resolver.
func (y *YouTubeResolver) Source() Source { return SourceYouTube }

// ExtractYouTubeID returns the 11-char video id in a YouTube URL or
// bare id, or "" when the input is not a YouTube video reference.
// Ported from YukkiMusic-Go extractVideoID (videoIDRe1 then Re2),
// with CanGet's host gate applied first: the loose Re2 pattern
// would otherwise match any 11-char run in a Spotify/TikTok URL.
func ExtractYouTubeID(input string) string {
	q := strings.TrimSpace(input)
	if bareVideoIDRe.MatchString(q) {
		return q
	}
	if !youtubeLinkRe.MatchString(q) {
		return ""
	}
	if m := videoIDRe1.FindStringSubmatch(q); m != nil {
		return m[1]
	}
	if m := videoIDRe2.FindStringSubmatch(q); m != nil {
		return m[1]
	}
	return ""
}

// ExtractYouTubePlaylistID returns the playlist id in a YouTube URL,
// or "".
func ExtractYouTubePlaylistID(input string) string {
	if m := playlistIDRe1.FindStringSubmatch(input); m != nil {
		return m[1]
	}
	if m := playlistIDRe2.FindStringSubmatch(input); m != nil {
		return m[1]
	}
	return ""
}

// IsYouTubeURL reports whether input looks like a YouTube link.
func IsYouTubeURL(input string) bool { return youtubeLinkRe.MatchString(strings.TrimSpace(input)) }

// Resolve handles a YouTube URL, a bare video id, a playlist URL
// (first track) or free text (top search hit).
func (y *YouTubeResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	if q == "" {
		return Track{}, engineUnavailable(SourceYouTube, "empty query")
	}
	if pid := ExtractYouTubePlaylistID(q); pid != "" && ExtractYouTubeID(q) == "" {
		tracks, err := y.fetchPlaylist(ctx, pid, 1)
		if err != nil {
			return Track{}, err
		}
		if len(tracks) == 0 {
			return Track{}, engineUnavailable(SourceYouTube, "playlist %s has no playable tracks", pid)
		}
		return tracks[0], nil
	}
	if vid := ExtractYouTubeID(q); vid != "" {
		track, err := y.fetchVideo(ctx, vid)
		if err == nil {
			return track, nil
		}
		// Player failed (age-gated / ciphered / unplayable): warm
		// yt-dlp daemon fallback, exactly one path.
		if y.ytdlp != nil {
			if dt, derr := y.resolveViaDaemon(ctx, vid); derr == nil {
				return dt, nil
			} else {
				return Track{}, fmt.Errorf("youtube player failed for %s: %v; yt-dlp daemon fallback also failed: %v", vid, err, derr)
			}
		}
		return Track{}, err
	}
	tracks, err := y.Search(ctx, q, 1)
	if err != nil {
		return Track{}, err
	}
	if len(tracks) == 0 {
		return Track{}, engineUnavailable(SourceYouTube, "no YouTube results for %q", q)
	}
	// A search result only provides metadata and video ID; fetchVideo
	// resolves the playable audio StreamURL (Opus 251 / AAC 140).
	resolved, err := y.fetchVideo(ctx, tracks[0].ID)
	if err == nil && resolved.StreamURL != "" {
		if resolved.Title == "" {
			resolved.Title = tracks[0].Title
		}
		return resolved, nil
	}
	if y.ytdlp != nil {
		dstart := time.Now()
		if dt, derr := y.resolveViaDaemon(ctx, tracks[0].ID); derr == nil && dt.StreamURL != "" {
			log.Printf("youtube: resolve daemon ok in %s", time.Since(dstart))
			return dt, nil
		} else {
			log.Printf("youtube: resolve daemon failed in %s: %v", time.Since(dstart), derr)
		}
	}
	return tracks[0], nil
}

// Search runs an InnerTube search (ANDROID first — WEB gets WAF 403 on
// datacenter IPs — falling back to WEB, then daemon). Each stage logs
// its duration so a slow resolve names the slow stage.
func (y *YouTubeResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if limit <= 0 {
		limit = 5
	}
	if strings.TrimSpace(query) == "" {
		return nil, engineUnavailable(SourceYouTube, "empty search query")
	}

	// 1. Try ANDROID search (survives the WAF 403 that kills WEB on datacenter IPs)
	androidPayload := map[string]any{
		"context": map[string]any{"client": map[string]any{
			"clientName": innerTubeAndroid, "clientVersion": innerTubeAndroidVer,
		}},
		"query": query,
	}
	var result map[string]any
	start := time.Now()
	if err := y.callInnerTube(ctx, "search", innerTubeAndroid, innerTubeAndroidVer, androidPayload, &result); err == nil {
		var tracks []Track
		parseVideoNodes(result, &tracks, limit)
		if len(tracks) > 0 {
			log.Printf("youtube: search ANDROID ok in %s (%d tracks)", time.Since(start), len(tracks))
			return tracks, nil
		}
		log.Printf("youtube: search ANDROID ok in %s but 0 tracks", time.Since(start))
	} else {
		log.Printf("youtube: search ANDROID failed in %s: %v", time.Since(start), err)
	}

	// 2. Fallback to WEB search
	webPayload := map[string]any{
		"context": map[string]any{"client": map[string]any{
			"clientName": innerTubeWeb, "clientVersion": innerTubeWebVer,
		}},
		"query":  query,
		"params": "CAASAhAB",
	}
	result = nil
	start = time.Now()
	if err := y.callInnerTube(ctx, "search", innerTubeWeb, innerTubeWebVer, webPayload, &result); err == nil {
		var tracks []Track
		parseVideoNodes(result, &tracks, limit)
		if len(tracks) > 0 {
			log.Printf("youtube: search WEB ok in %s (%d tracks)", time.Since(start), len(tracks))
			return tracks, nil
		}
		log.Printf("youtube: search WEB ok in %s but 0 tracks", time.Since(start))
	} else {
		log.Printf("youtube: search WEB failed in %s: %v", time.Since(start), err)
	}

	// 3. Fallback to yt-dlp daemon search
	if y.ytdlp != nil {
		start = time.Now()
		if dt, derr := y.searchViaDaemon(ctx, query); derr == nil && dt.ID != "" {
			log.Printf("youtube: search daemon ok in %s", time.Since(start))
			return []Track{dt}, nil
		} else {
			log.Printf("youtube: search daemon failed in %s: %v", time.Since(start), derr)
		}
	}

	return nil, engineUnavailable(SourceYouTube, "no YouTube results for %q", query)
}

// fetchVideo resolves one video id via the ANDROID_VR player call
// (ANDROID fallback), returning title, duration and the best
// adaptive audio stream URL (itag 251 Opus, else 140 AAC).
func (y *YouTubeResolver) fetchVideo(ctx context.Context, videoID string) (Track, error) {
	var result map[string]any
	var lastErr error
	for _, client := range []struct{ name, ver string }{
		{innerTubeAndroidVR, innerTubeAndroidVRVer},
		{innerTubeAndroid, innerTubeAndroidVer},
	} {
		payload := map[string]any{
			"context": map[string]any{"client": map[string]any{
				"clientName": client.name, "clientVersion": client.ver,
			}},
			"videoId": videoID,
		}
		result = nil
		start := time.Now()
		if err := y.callInnerTube(ctx, "player", client.name, client.ver, payload, &result); err != nil {
			log.Printf("youtube: player %s failed in %s: %v", client.name, time.Since(start), err)
			lastErr = err
			continue
		}
		if status, _ := dig(result, "playabilityStatus", "status").(string); status != "" && status != "OK" {
			reason, _ := dig(result, "playabilityStatus", "reason").(string)
			lastErr = engineUnavailable(SourceYouTube, "video %s not playable (status %s: %s)", videoID, status, reason)
			log.Printf("youtube: player %s not playable in %s: %v", client.name, time.Since(start), lastErr)
			continue
		}
		track, err := trackFromPlayer(result, videoID)
		if err != nil {
			log.Printf("youtube: player %s parse failed in %s: %v", client.name, time.Since(start), err)
			lastErr = err
			continue
		}
		log.Printf("youtube: player %s ok in %s", client.name, time.Since(start))
		return track, nil
	}
	if lastErr == nil {
		lastErr = engineUnavailable(SourceYouTube, "video %s: no playable response", videoID)
	}
	return Track{}, lastErr
}

// fetchPlaylist resolves a playlist (browse VL+id) or a mix
// (next RD+id) to tracks, up to limit.
func (y *YouTubeResolver) fetchPlaylist(ctx context.Context, playlistID string, limit int) ([]Track, error) {
	if strings.HasPrefix(playlistID, "RD") {
		payload := map[string]any{
			"context": map[string]any{"client": map[string]any{
				"clientName": innerTubeWeb, "clientVersion": innerTubeWebVer,
			}},
			"playlistId": playlistID,
		}
		var result map[string]any
		if err := y.callInnerTube(ctx, "next", innerTubeWeb, innerTubeWebVer, payload, &result); err != nil {
			return nil, err
		}
		items, ok := dig(result, "contents", "twoColumnWatchNextResults", "playlist", "playlist", "contents").([]any)
		if !ok {
			return nil, engineUnavailable(SourceYouTube, "mix contents not found for %s", playlistID)
		}
		var tracks []Track
		for _, item := range items {
			if limit > 0 && len(tracks) >= limit {
				break
			}
			vid, ok := dig(item, "playlistPanelVideoRenderer").(map[string]any)
			if !ok {
				continue
			}
			id, _ := vid["videoId"].(string)
			if id == "" {
				continue
			}
			tracks = append(tracks, Track{
				ID: id, Title: extractText(dig(vid, "title")),
				URL:         "https://www.youtube.com/watch?v=" + id,
				Thumbnail:   thumbnailURL(vid),
				DurationSec: int64(ParseDuration(extractText(dig(vid, "lengthText")))),
				Source:      string(SourceYouTube),
				DurationStr: FormatDurationStr(ParseDuration(extractText(dig(vid, "lengthText"))), false),
			})
		}
		return tracks, nil
	}
	browseID := playlistID
	if !strings.HasPrefix(browseID, "VL") {
		browseID = "VL" + playlistID
	}
	payload := map[string]any{
		"context": map[string]any{"client": map[string]any{
			"clientName": innerTubeWeb, "clientVersion": innerTubeWebVer,
		}},
		"browseId": browseID,
	}
	var result map[string]any
	if err := y.callInnerTube(ctx, "browse", innerTubeWeb, innerTubeWebVer, payload, &result); err != nil {
		return nil, err
	}
	var tracks []Track
	parseVideoNodes(result, &tracks, limit)
	return tracks, nil
}

// resolveViaDaemon is the warm yt-dlp fallback: one JSON-RPC extract
// over the persistent daemon (no fork). Used only when the native
// player call cannot produce a playable track.
func (y *YouTubeResolver) resolveViaDaemon(ctx context.Context, videoID string) (Track, error) {
	info, err := y.ytdlp.Extract(ctx, "https://www.youtube.com/watch?v="+videoID)
	if err != nil {
		return Track{}, err
	}
	return trackFromYtDlpInfo(info, videoID), nil
}

// searchViaDaemon is the warm yt-dlp fallback for search: runs
// ytsearch1:<query> over the persistent daemon. Used when InnerTube
// search fails or is throttled.
func (y *YouTubeResolver) searchViaDaemon(ctx context.Context, query string) (Track, error) {
	if y.ytdlp == nil {
		return Track{}, engineUnavailable(SourceYouTube, "yt-dlp daemon is not configured")
	}
	info, err := y.ytdlp.Extract(ctx, "ytsearch1:"+query)
	if err != nil {
		return Track{}, err
	}
	if entries, ok := info["entries"].([]any); ok && len(entries) > 0 {
		if first, ok := entries[0].(map[string]any); ok {
			vid, _ := first["id"].(string)
			return trackFromYtDlpInfo(first, vid), nil
		}
	}
	return trackFromYtDlpInfo(info, ""), nil
}

// Download downloads a YouTube track via the warm daemon (bestaudio,
// extracted to mp3) into destDir.
func (y *YouTubeResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	if y.ytdlp == nil {
		return DownloadResult{}, engineUnavailable(SourceYouTube, "yt-dlp daemon is not configured; cannot download YouTube track %s", track.ID)
	}
	return downloadViaDaemon(ctx, y.ytdlp, track, destDir, onProgress)
}

// callInnerTube POSTs one InnerTube call (YukkiMusic-Go callInnerTube,
// ParvatiMusic call_innertube): POST
// https://m.youtube.com/youtubei/v1/<endpoint>?key=<key> with the
// client context in the body.
func (y *YouTubeResolver) callInnerTube(ctx context.Context, endpoint, clientName, clientVersion string, payload map[string]any, result any) error {
	body, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	url := fmt.Sprintf("%s/youtubei/v1/%s?key=%s", innerTubeBase, endpoint, innerTubeKey)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if clientName == innerTubeWeb {
		req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36")
		req.Header.Set("Origin", "https://www.youtube.com")
		req.Header.Set("Referer", "https://www.youtube.com/")
	} else {
		req.Header.Set("User-Agent", innerTubeUA)
	}
	resp, err := httpClient(30 * time.Second).Do(req)
	if err != nil {
		return fmt.Errorf("innertube %s request failed: %w", endpoint, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("innertube %s error: HTTP %d", endpoint, resp.StatusCode)
	}
	return json.NewDecoder(resp.Body).Decode(result)
}

// trackFromPlayer builds a Track from an ANDROID_VR/ANDROID player
// response: videoDetails for metadata, streamingData.adaptiveFormats
// for the audio URL (itag 251 Opus first, then 140 AAC).
func trackFromPlayer(result map[string]any, videoID string) (Track, error) {
	details, ok := dig(result, "videoDetails").(map[string]any)
	if !ok {
		return Track{}, engineUnavailable(SourceYouTube, "videoDetails not found for %s", videoID)
	}
	id, _ := details["videoId"].(string)
	if id == "" {
		id = videoID
	}
	title, _ := details["title"].(string)
	duration := ParseDuration(details["lengthSeconds"])
	streamURL := pickAdaptiveAudioURL(result)
	t := Track{
		ID: id, Title: title, Source: string(SourceYouTube),
		URL:         "https://www.youtube.com/watch?v=" + id,
		DurationSec: int64(duration),
		Thumbnail:   thumbnailURL(result),
		StreamURL:   streamURL,
		DurationStr: FormatDurationStr(duration, false),
	}
	if uploader, _ := details["author"].(string); uploader != "" {
		t.Uploader = strptr(uploader)
	}
	if views, ok := details["viewCount"].(string); ok {
		if n, err := strconv.ParseInt(views, 10, 64); err == nil {
			t.Views = intptr(n)
		}
	}
	if title == "" {
		return Track{}, engineUnavailable(SourceYouTube, "video %s: empty title in player response", videoID)
	}
	if streamURL == "" {
		return Track{}, engineUnavailable(SourceYouTube, "video %s: no direct adaptive audio URL (itag 251/140 absent or signature-ciphered)", videoID)
	}
	return t, nil
}

// pickAdaptiveAudioURL selects the audio stream URL from
// streamingData.adaptiveFormats: itag 251 (Opus) first, then 140
// (AAC). Formats with only a signatureCipher and no direct URL are
// skipped — they need the yt-dlp daemon.
func pickAdaptiveAudioURL(result map[string]any) string {
	formats, ok := dig(result, "streamingData", "adaptiveFormats").([]any)
	if !ok {
		return ""
	}
	byItag := map[int64]string{}
	for _, f := range formats {
		fm, ok := f.(map[string]any)
		if !ok {
			continue
		}
		itag := int64(0)
		switch v := fm["itag"].(type) {
		case float64:
			itag = int64(v)
		case json.Number:
			itag, _ = v.Int64()
		}
		url, _ := fm["url"].(string)
		if url == "" {
			continue
		}
		byItag[itag] = url
	}
	if u := byItag[251]; u != "" {
		return u
	}
	return byItag[140]
}

// trackFromYtDlpInfo maps a yt-dlp -J info dict to a Track (daemon
// fallback path). Duration/views/uploader follow the vc-ts
// enrichYtIPC3 mapping.
func trackFromYtDlpInfo(info map[string]any, videoID string) Track {
	id, _ := info["id"].(string)
	if id == "" {
		id = videoID
	}
	title, _ := info["title"].(string)
	if title == "" {
		title = "YouTube Track"
	}
	duration := ParseDuration(info["duration"])
	t := Track{
		ID: id, Title: title, Source: string(SourceYouTube),
		URL:         "https://www.youtube.com/watch?v=" + id,
		DurationSec: int64(duration),
		DurationStr: FormatDurationStr(duration, false),
	}
	if u, _ := info["url"].(string); u != "" {
		t.StreamURL = u
	} else if formats, ok := info["formats"].([]any); ok {
		t.StreamURL = pickYtDlpAudioURL(formats)
	}
	if thumb, _ := info["thumbnail"].(string); thumb != "" {
		t.Thumbnail = thumb
	}
	if up, _ := info["uploader"].(string); up != "" {
		t.Uploader = strptr(up)
	} else if ch, _ := info["channel"].(string); ch != "" {
		t.Uploader = strptr(ch)
	}
	if vc, ok := info["view_count"].(float64); ok && vc > 0 {
		t.Views = intptr(int64(vc))
	}
	if ud, _ := info["upload_date"].(string); ud != "" && len(ud) == 8 {
		t.UploadTime = strptr(ud[:4] + "-" + ud[4:6] + "-" + ud[6:8])
	}
	if live, _ := info["is_live"].(bool); live {
		t.IsLive = true
		t.DurationStr = FormatDurationStr(duration, true)
	}
	return t
}

// pickYtDlpAudioURL picks the best audio-only format URL from a
// yt-dlp formats list: Opus itag 251 first, then AAC 140, then the
// highest-bitrate audio-only format.
func pickYtDlpAudioURL(formats []any) string {
	best := ""
	bestAbr := -1.0
	for _, f := range formats {
		fm, ok := f.(map[string]any)
		if !ok {
			continue
		}
		acodec, _ := fm["acodec"].(string)
		vcodec, _ := fm["vcodec"].(string)
		url, _ := fm["url"].(string)
		if url == "" || acodec == "none" || acodec == "" || (vcodec != "" && vcodec != "none") {
			continue
		}
		fid, _ := fm["format_id"].(string)
		itag := int64(0)
		if v, ok := fm["itag"].(float64); ok {
			itag = int64(v)
		}
		if fid == "251" || itag == 251 {
			return url
		}
		if fid == "140" || itag == 140 {
			best = url
			bestAbr = 1 << 30
			continue
		}
		abr, _ := fm["abr"].(float64)
		if abr > bestAbr {
			bestAbr = abr
			best = url
		}
	}
	return best
}

// parseVideoNodes walks an InnerTube response tree collecting
// videoRenderer (or playlistVideoRenderer) nodes into tracks,
// skipping items without a duration (live/upcoming videos have no
// lengthText — YukkiMusic-Go parseNodes does the same).
func parseVideoNodes(node any, tracks *[]Track, limit int) {
	if limit > 0 && len(*tracks) >= limit {
		return
	}
	switch v := node.(type) {
	case []any:
		for _, item := range v {
			parseVideoNodes(item, tracks, limit)
			if limit > 0 && len(*tracks) >= limit {
				return
			}
		}
	case map[string]any:
		for _, key := range []string{"videoRenderer", "playlistVideoRenderer", "compactVideoRenderer"} {
			if vid, ok := v[key].(map[string]any); ok {
				id, _ := vid["videoId"].(string)
				if id == "" {
					return
				}
				durationText := extractText(dig(vid, "lengthText"))
				if durationText == "" {
					return // live / upcoming: no duration
				}
				title := extractText(dig(vid, "title"))
				if title == "" {
					title = "YouTube Track"
				}
				duration := ParseDuration(durationText)
				t := Track{
					ID: id, Title: title, Source: string(SourceYouTube),
					URL:         "https://www.youtube.com/watch?v=" + id,
					Thumbnail:   thumbnailURL(vid),
					DurationSec: int64(duration),
					DurationStr: FormatDurationStr(duration, false),
				}
				*tracks = append(*tracks, t)
				return
			}
		}
		for _, val := range v {
			parseVideoNodes(val, tracks, limit)
			if limit > 0 && len(*tracks) >= limit {
				return
			}
		}
	}
}

// dig walks a decoded-JSON tree by keys, returning nil when any step
// is missing or of the wrong shape.
func dig(node any, keys ...string) any {
	cur := node
	for _, k := range keys {
		m, ok := cur.(map[string]any)
		if !ok {
			return nil
		}
		cur = m[k]
	}
	return cur
}

// extractText joins the runs[].text of an InnerTube text object.
func extractText(node any) string {
	m, ok := node.(map[string]any)
	if !ok {
		if s, ok := node.(string); ok {
			return s
		}
		return ""
	}
	if s, ok := m["simpleText"].(string); ok {
		return s
	}
	runs, ok := m["runs"].([]any)
	if !ok {
		return ""
	}
	var b strings.Builder
	for _, r := range runs {
		if rm, ok := r.(map[string]any); ok {
			if t, ok := rm["text"].(string); ok {
				b.WriteString(t)
			}
		}
	}
	return b.String()
}

// thumbnailURL returns the last (largest) thumbnail URL in a node.
func thumbnailURL(node map[string]any) string {
	thumbs, ok := dig(node, "thumbnail", "thumbnails").([]any)
	if !ok {
		thumbs, ok = dig(node, "videoDetails", "thumbnail", "thumbnails").([]any)
	}
	if !ok || len(thumbs) == 0 {
		return ""
	}
	last, ok := thumbs[len(thumbs)-1].(map[string]any)
	if !ok {
		return ""
	}
	u, _ := last["url"].(string)
	return u
}
