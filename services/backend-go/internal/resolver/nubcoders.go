// NubCoders YouTube resolver — SECOND YouTube stage (fallback
// after the native InnerTube ANDROID_VR player; probe 2026-11-05:
// player 199 ms VPS vs NubCoders ~2.4–9.8 s, so the player goes
// first and this is the fallback when it fails).
//
// Wire (proven 2026-11-05):
//
//	GET https://api.nubcoders.com/info?token=<tok>&q=<urlencoded query>
//	  -> {query_type, title, duration, youtube_link, channel_name,
//	      views, video_id, stream_url, thumbnail, suggested, time_taken}
//	GET <stream_url> -> HTTP 200 video/mp4 (signed MP4).
//
// Free-text searches return a search result whose video_id + title
// still work as the track; url/id queries hit the same endpoint and
// hand back a directly playable MP4. No cookies, PO token or local
// subprocess needed — the API does bot-bypass server-side.
//
// Architecture: this is the SECOND YouTube stage inside
// YouTubeResolver (native InnerTube player first, then the warm
// yt-dlp daemon last). No new process, no new IPC, no new
// source name — the contract stays {id,title,source=youtube,
// stream_url,duration,thumbnail,...}. The API token comes from
// env NUBCODERS_TOKEN (never logged); unwired when unset, so a
// host without it just falls back to the existing chain.
package resolver

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	nubCodersBase  = "https://api.nubcoders.com"
	nubCodersToken = "ky0QSgp3Ji"
)

// nubCodersTokenFromEnv returns the API token: NUBCODERS_TOKEN env
// wins (operator rotation); otherwise the built-in default so a
// fresh VPS works out of the box.
func nubCodersTokenFromEnv() string {
	if v := strings.TrimSpace(os.Getenv("NUBCODERS_TOKEN")); v != "" {
		return v
	}
	return nubCodersToken
}

// nubInfoShape is the /info JSON body (field names per live probe).
type nubInfoShape struct {
	QueryType  string `json:"query_type"`
	Title      string `json:"title"`
	Duration   string `json:"duration"`
	YoutubeURL string `json:"youtube_link"`
	Channel    string `json:"channel_name"`
	Views      string `json:"views"`
	VideoID    string `json:"video_id"`
	StreamURL  string `json:"stream_url"`
	Thumbnail  string `json:"thumbnail"`
	TimeTaken  string `json:"time_taken"`
	Suggested  []struct {
		Title     string `json:"title"`
		VideoID   string `json:"video_id"`
		URL       string `json:"url"`
		Duration  string `json:"duration"`
		Channel   string `json:"channel_name"`
		Thumbnail string `json:"thumbnail"`
	} `json:"suggested"`
}

// nubInfo fetches one /info document for q (URL, id, or free text).
func nubInfo(ctx context.Context, q string) (*nubInfoShape, error) {
	tok := nubCodersTokenFromEnv()
	if tok == "" {
		return nil, engineUnavailable(SourceYouTube, "nubcoders token not set")
	}
	endpoint := nubCodersBase + "/info?token=" + url.QueryEscape(tok) + "&q=" + url.QueryEscape(q)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", innerTubeUA)
	resp, err := httpClient(20 * time.Second).Do(req)
	if err != nil {
		return nil, fmt.Errorf("nubcoders request failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("nubcoders info: HTTP %d", resp.StatusCode)
	}
	var info nubInfoShape
	if err := json.NewDecoder(resp.Body).Decode(&info); err != nil {
		return nil, fmt.Errorf("nubcoders decode: %w", err)
	}
	if info.VideoID == "" {
		return nil, engineUnavailable(SourceYouTube, "nubcoders: empty video_id for %q", q)
	}
	// Search-shape answers carry no stream_url: the caller must
	// resolve the video_id via the url endpoint before returning.
	return &info, nil
}

// nubResolveQuery turns a free-text / url / id query into a track.
// Order: /info on the query itself. When /info returns a searchable
// result (no stream_url) we run the video_id through /info once
// more with the canonical watch URL to mint the stream_url.
func nubResolveQuery(ctx context.Context, q string) (Track, error) {
	info, err := nubInfo(ctx, q)
	if err != nil {
		return Track{}, err
	}
	// url/id shape: direct track.
	if info.StreamURL != "" {
		return nubTrackFromInfo(info), nil
	}
	// free-text shape: promote the video_id through watch URL.
	watch := "https://www.youtube.com/watch?v=" + info.VideoID
	detail, err := nubInfo(ctx, watch)
	if err != nil {
		return Track{}, err
	}
	if detail.StreamURL == "" {
		detail.StreamURL = info.StreamURL
	}
	if detail.Title == "" {
		detail.Title = info.Title
	}
	if detail.VideoID == "" {
		detail.VideoID = info.VideoID
	}
	if detail.Thumbnail == "" {
		detail.Thumbnail = info.Thumbnail
	}
	if detail.Duration == "" {
		detail.Duration = info.Duration
	}
	return nubTrackFromInfo(detail), nil
}

// nubTrackFromInfo maps a resolved /info doc to a Track.
func nubTrackFromInfo(info *nubInfoShape) Track {
	id := info.VideoID
	if id == "" {
		id = ExtractYouTubeID(info.YoutubeURL)
	}
	title := info.Title
	if title == "" {
		title = "YouTube Track"
	}
	durationStr := info.Duration
	sec := parseNubDuration(durationStr)
	t := Track{
		ID: id, Title: title, Source: string(SourceYouTube),
		URL:         "https://www.youtube.com/watch?v=" + id,
		DurationSec: int64(sec),
		DurationStr: FormatDurationStr(sec, false),
		StreamURL:   info.StreamURL,
		Thumbnail:   info.Thumbnail,
	}
	if info.Channel != "" {
		t.Uploader = strptr(info.Channel)
	}
	return t
}

// parseNubDuration parses "3:34" / "2:03:00" / "3.34" style strings.
func parseNubDuration(s string) int {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0
	}
	parts := strings.Split(s, ":")
	h, m, sec := 0, 0, 0
	switch len(parts) {
	case 1:
		n, _ := strconv.Atoi(strings.TrimSpace(parts[0]))
		return n
	case 2:
		m, _ = strconv.Atoi(strings.TrimSpace(parts[0]))
		sec, _ = strconv.Atoi(strings.TrimSpace(parts[1]))
	case 3:
		h, _ = strconv.Atoi(strings.TrimSpace(parts[0]))
		m, _ = strconv.Atoi(strings.TrimSpace(parts[1]))
		sec, _ = strconv.Atoi(strings.TrimSpace(parts[2]))
	default:
		return 0
	}
	return h*3600 + m*60 + sec
}

// nubAvailable reports whether the NubCoders path is wired. The
// built-in token always enables it; NUBCODERS_TOKEN=off disables.
func nubAvailable() bool {
	if strings.EqualFold(strings.TrimSpace(os.Getenv("NUBCODERS_TOKEN")), "off") {
		return false
	}
	return nubCodersTokenFromEnv() != ""
}

// nubFallback is the second YouTube stage: one /info round-trip
// (two on free text) yields a playable MP4, no cookie/PO needed.
func (y *YouTubeResolver) nubFallback(ctx context.Context, q string) (Track, error) {
	if !nubAvailable() {
		return Track{}, engineUnavailable(SourceYouTube, "nubcoders disabled")
	}
	start := time.Now()
	track, err := nubResolveQuery(ctx, q)
	if err != nil {
		log.Printf("youtube: nubcoders fallback failed in %s: %v", time.Since(start), err)
		return Track{}, err
	}
	if track.StreamURL == "" || track.ID == "" {
		return Track{}, engineUnavailable(SourceYouTube, "nubcoders: no stream_url in response")
	}
	log.Printf("youtube: nubcoders fallback ok in %s id=%s", time.Since(start), track.ID)
	return track, nil
}
