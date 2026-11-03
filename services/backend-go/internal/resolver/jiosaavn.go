// JioSaavn resolver — jsvn-tgvc proxy (BENCHMARKS.md §2–§3, locked).
//
// Strategy: ONE /song call (0.32 s measured) for a single id, or
// search + parallel /song enrichment (0.51 s) for free text. Clear
// URLs, NO DES-ECB — Phase 1 proved no crypto exists in this path
// (the proxy returns clear aac.saavncdn.com URLs).
//
// Quality selection (the locked correction, BENCHMARKS.md §3.6):
// select 320_KBPS, fallback 160_KBPS, then media_url. The old code
// took 96_KBPS — that must NOT be copied.
//
// Duration is a "m:ss" clock string — parsed with ParseDuration
// (the old Number("4:28") bug returned NaN -> 0; do not repeat it).
//
// Stream URLs are short-lived signed URLs (~2 min expiry, proven
// Phase 1: a URL fetched ~2 min earlier 404s). They must be used
// promptly after resolve or re-resolved; do NOT cache them long
// (Phase 4 cache must honour this).
//
// Port sources: refs/TGVCBot apps/tgvcbot/src/providers/jiosaavn.ts
// (search /song shapes), services/vc-ts/src/resolvers.js
// (searchJioSaavn / resolveJioSaavnSong / enrichJioSaavnDurations).
package resolver

import (
	"context"
	"fmt"
	"net/url"
	"strings"
	"sync"
)

// JioSaavn proxy bases, tried in order. saavn.dev is unreachable
// from the sandbox (HTTP 000) but may answer on the user's host.
var jioSaavnBases = []string{
	"https://jsvn-tgvc.vercel.app",
	"https://saavn.dev/api",
}

// JioSaavnResolver resolves JioSaavn ids (jsvn:<id>) and free text.
type JioSaavnResolver struct{}

// NewJioSaavnResolver builds a JioSaavn resolver.
func NewJioSaavnResolver() *JioSaavnResolver { return &JioSaavnResolver{} }

// Source implements Resolver.
func (j *JioSaavnResolver) Source() Source { return SourceJioSaavn }

// jioSaavnRaw is the union of the /search result and /song payload
// shapes (TGVCBot JioSaavnSongResponse / JioSaavnSearchResponse).
type jioSaavnRaw struct {
	ID             string            `json:"id"`
	Title          string            `json:"title"`
	Song           string            `json:"song"`
	Singers        string            `json:"singers"`
	PrimaryArtists string            `json:"primary_artists"`
	Duration       any               `json:"duration"`
	PermaURL       string            `json:"perma_url"`
	URL            string            `json:"url"`
	Image          string            `json:"image"`
	MediaURL       string            `json:"media_url"`
	MediaURLs      map[string]string `json:"media_urls"`
	MoreInfo       *struct {
		Singers  string `json:"singers"`
		Duration any    `json:"duration"`
	} `json:"more_info"`
}

// selectJioSaavnStreamURL picks the stream URL: 320_KBPS first,
// then 160_KBPS, then media_url. 96_KBPS is never selected while a
// higher quality exists — the old code's 96 kbps selection was the
// quality bug BENCHMARKS.md §3.6 orders fixed.
func selectJioSaavnStreamURL(r jioSaavnRaw) string {
	if r.MediaURLs != nil {
		if u := r.MediaURLs["320_KBPS"]; u != "" {
			return u
		}
		if u := r.MediaURLs["160_KBPS"]; u != "" {
			return u
		}
	}
	return r.MediaURL
}

func jioSaavnTrack(r jioSaavnRaw) Track {
	singers := r.Singers
	if singers == "" && r.MoreInfo != nil {
		singers = r.MoreInfo.Singers
	}
	if singers == "" {
		singers = r.PrimaryArtists
	}
	if singers == "" {
		singers = "Unknown"
	}
	title := r.Title
	if title == "" {
		title = r.Song
	}
	if title == "" {
		title = "Unknown"
	}
	duration := r.Duration
	if duration == nil && r.MoreInfo != nil {
		duration = r.MoreInfo.Duration
	}
	durSec := ParseDuration(duration)
	t := Track{
		ID:          "jsvn:" + r.ID,
		Title:       cleanTitle(title),
		Artist:      singers,
		Source:      string(SourceJioSaavn),
		DurationSec: int64(durSec),
		DurationStr: FormatDurationStr(durSec, false),
		URL:         r.PermaURL,
		Thumbnail:   r.Image,
	}
	if t.URL == "" {
		t.URL = r.URL
	}
	if u := selectJioSaavnStreamURL(r); u != "" {
		t.StreamURL = u
	}
	return t
}

// Resolve handles jsvn:<id>, a bare JioSaavn id, or free text
// (top search hit).
func (j *JioSaavnResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	lower := strings.ToLower(q)
	if strings.HasPrefix(lower, "jsvn:") {
		return j.resolveSong(ctx, strings.TrimSpace(q[len("jsvn:"):]))
	}
	if strings.HasPrefix(lower, "jiosaavn:") {
		q = strings.TrimSpace(q[len("jiosaavn:"):])
	} else if !strings.ContainsAny(q, " \t") {
		// Bare single-token id — one /song call.
		if t, err := j.resolveSong(ctx, q); err == nil {
			return t, nil
		}
		// Not an id after all: fall through to search.
	}
	tracks, err := j.Search(ctx, q, 1)
	if err != nil {
		return Track{}, err
	}
	if len(tracks) == 0 {
		return Track{}, engineUnavailable(SourceJioSaavn, "no JioSaavn results for %q", query)
	}
	return tracks[0], nil
}

// Search runs one /search call, then enriches each result with a
// parallel /song lookup (the search payload carries no duration and
// no media URL — vc-ts enrichJioSaavnDurations).
func (j *JioSaavnResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if limit <= 0 {
		limit = 5
	}
	q := strings.TrimSpace(strings.TrimPrefix(query, "jiosaavn:"))
	if q == "" {
		return nil, engineUnavailable(SourceJioSaavn, "empty search query")
	}
	var errs []string
	for _, base := range jioSaavnBases {
		var searchURL string
		if strings.Contains(base, "saavn.dev") {
			searchURL = base + "/search/songs?query=" + url.QueryEscape(q)
		} else {
			searchURL = base + "/search?query=" + url.QueryEscape(q)
		}
		var data struct {
			Results []jioSaavnRaw `json:"results"`
			Data    *struct {
				Results []jioSaavnRaw `json:"results"`
			} `json:"data"`
		}
		if err := getJSON(ctx, searchURL, nil, &data); err != nil {
			errs = append(errs, fmt.Sprintf("%s: %v", base, err))
			continue
		}
		raw := data.Results
		if raw == nil && data.Data != nil {
			raw = data.Data.Results
		}
		if len(raw) == 0 {
			return []Track{}, nil
		}
		if len(raw) > limit {
			raw = raw[:limit]
		}
		tracks := make([]Track, len(raw))
		for i, r := range raw {
			tracks[i] = jioSaavnTrack(r)
		}
		j.enrichDurations(ctx, tracks)
		return tracks, nil
	}
	return nil, engineUnavailable(SourceJioSaavn, "JioSaavn search unavailable (tried %s): %s",
		strings.Join(jioSaavnBases, ", "), strings.Join(errs, "; "))
}

// resolveSong runs one /song?id= call (0.32 s measured).
func (j *JioSaavnResolver) resolveSong(ctx context.Context, id string) (Track, error) {
	var errs []string
	for _, base := range jioSaavnBases {
		var songURL string
		if strings.Contains(base, "saavn.dev") {
			songURL = base + "/songs/" + url.PathEscape(id)
		} else {
			songURL = base + "/song?id=" + url.QueryEscape(id)
		}
		var data struct {
			jioSaavnRaw
			Data []jioSaavnRaw `json:"data"`
		}
		if err := getJSON(ctx, songURL, nil, &data); err != nil {
			errs = append(errs, fmt.Sprintf("%s: %v", base, err))
			continue
		}
		raw := data.jioSaavnRaw
		if len(data.Data) > 0 {
			raw = data.Data[0]
		}
		if raw.ID == "" && raw.Song == "" && raw.Title == "" {
			errs = append(errs, fmt.Sprintf("%s: empty song payload", base))
			continue
		}
		return jioSaavnTrack(raw), nil
	}
	return Track{}, engineUnavailable(SourceJioSaavn, "JioSaavn song lookup failed: %s", strings.Join(errs, "; "))
}

// enrichDurations fills duration + stream URL for results that lack
// them, with one parallel /song lookup each (vc-ts
// enrichJioSaavnDurations). A failed lookup leaves that track's
// duration at 0 rather than inventing a value.
func (j *JioSaavnResolver) enrichDurations(ctx context.Context, tracks []Track) {
	var wg sync.WaitGroup
	for i := range tracks {
		if tracks[i].DurationSec > 0 && tracks[i].StreamURL != "" {
			continue
		}
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			id := strings.TrimPrefix(tracks[i].ID, "jsvn:")
			full, err := j.resolveSong(ctx, id)
			if err != nil {
				return
			}
			if tracks[i].DurationSec <= 0 {
				tracks[i].DurationSec = full.DurationSec
				tracks[i].DurationStr = full.DurationStr
			}
			if tracks[i].StreamURL == "" {
				tracks[i].StreamURL = full.StreamURL
			}
		}(i)
	}
	wg.Wait()
}

// Download fetches the 320 kbps AAC stream URL directly (plain HTTP
// GET — the URL is a clear CDN URL, no crypto) into destDir.
func (j *JioSaavnResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	if track.StreamURL == "" {
		full, err := j.Resolve(ctx, track.ID)
		if err != nil {
			return DownloadResult{}, err
		}
		track = full
	}
	if track.StreamURL == "" {
		return DownloadResult{}, engineUnavailable(SourceJioSaavn, "JioSaavn track %s has no stream URL", track.ID)
	}
	name := SanitizeFilename(track.Artist+" - "+track.Title) + ".mp4"
	return downloadHTTP(ctx, track, destDir, name, track.StreamURL, runID, onProgress)
}
