// YouTube RD-mix picker — the autoplay pick (inventory §3.2).
//
// The picker works a fallback tier chain (queue-port Tier 5,
// item 30): Tier 1 fetches the mix for a YouTube track —
// watch?v=<id>&list=RD<id> — Tier 2 searches "<clean title>
// similar songs". Each tier filters candidates against the played
// set, shuffles, and takes a random pick from the top 4; Tier 0
// (an iota /similar endpoint) has no Parlay equivalent and is not
// faked. A pick is resolved to a playable track through the
// single resolve path.
//
// Why this lives here and not in the resolver: no Related/mix
// method exists in internal/resolver, the yt-dlp daemon runs with
// noplaylist (so its Extract cannot return a mix), and this
// package may not edit the resolver. The call below is the same
// InnerTube `next` call resolver.fetchPlaylist makes for RD mixes
// (WEB client, playlistId RD<id>), with the same public InnerTube
// client key (refs/YukkiMusic-Go — a public client key, not a user
// credential; see internal/resolver/youtube_key.go) and the same
// headers the resolver's callInnerTube sends. It is not a second
// resolver: candidates are resolved to playable tracks through
// Registry.Resolve, the single resolve path.
//
// Outcomes: a zero Track with a nil error means "no pick" (the
// last track is not a YouTube track, the mix is empty, or every
// candidate is excluded or unresolvable). A non-nil error means
// the mix fetch itself failed.
package handlers

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math/rand/v2"
	"net/http"
	"strings"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
)

// innerTubeMix* mirrors the resolver's InnerTube constants
// (internal/resolver/youtube.go): same base, same public key, same
// WEB client, same mobile UA.
const (
	innerTubeMixBase    = "https://m.youtube.com"
	innerTubeMixKey     = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8"
	innerTubeMixClient  = "WEB"
	innerTubeMixVersion = "2.20250101.01.00"
	innerTubeMixUA      = "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36"
)

// mixPickerTimeout bounds one mix fetch (vc-go used 30 s).
const mixPickerTimeout = 30 * time.Second

// Autoplay fallback tiers (queue-port Tier 5, item 30 — Parvati
// platforms/registry.rs tiers + monitor.rs golden_zone_pick).
// Tier 0 is the iota `/similar` endpoint: it has NO Parlay
// equivalent — there is no /similar service in-tree, and the
// picker does not fake one. Tier 1 is the YouTube radio mix
// (RD<id> via InnerTube `next`, fetchMix). Tier 2 is a
// `"<clean title> similar songs"` YouTube search. Each tier
// filters candidates against the played set (exclude), shuffles,
// and takes a random pick from the top 4 (golden_zone_pick); a
// tier that yields no playable track falls through to the next,
// and a fetch error in one tier is not fatal — the next tier is
// tried. Only when every tier's fetch fails is the error
// returned (the room then goes idle on autoplay_failed).
//
// newMixPicker builds the autoplay picker around the registry.
func newMixPicker(reg *resolver.Registry) room.Picker {
	return func(ctx context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		videoID := mixVideoID(last)
		if videoID == "" {
			// Only YouTube tracks autoplay (all three engines).
			return model.Track{}, nil
		}
		ctx, cancel := context.WithTimeout(ctx, mixPickerTimeout)
		defer cancel()
		var lastErr error
		anyFetched := false
		// Tier 1: YouTube radio mix.
		if ids, err := fetchMix(ctx, videoID); err != nil {
			lastErr = err
		} else {
			anyFetched = true
			if track, ok := pickFromIDs(ctx, reg, ids, exclude); ok {
				return track, nil
			}
		}
		// Tier 2: "<clean title> similar songs" search.
		if ids, err := fetchSimilarSearch(ctx, reg, last); err != nil {
			lastErr = err
		} else {
			anyFetched = true
			if track, ok := pickFromIDs(ctx, reg, ids, exclude); ok {
				return track, nil
			}
		}
		if !anyFetched && lastErr != nil {
			return model.Track{}, lastErr
		}
		return model.Track{}, nil
	}
}

// pickFromIDs filters ids against exclude (last + queue +
// history — queue-port Tier 5, item 29: never re-suggest a played
// track), dedupes, shuffles the survivors (TgMusicBot autoplay
// shuffle), and resolves the top 4 in order until one yields a
// playable track (Parvati's golden_zone_pick: random pick from
// the top 4). An unresolvable candidate is skipped, not fatal:
// the tier has more entries. It returns false when no candidate
// resolves.
func pickFromIDs(ctx context.Context, reg *resolver.Registry, ids []string, exclude map[string]bool) (model.Track, bool) {
	seen := make(map[string]bool, len(ids))
	var cands []string
	for _, id := range ids {
		if id == "" || exclude[id] || seen[id] {
			continue
		}
		seen[id] = true
		cands = append(cands, id)
	}
	rand.Shuffle(len(cands), func(i, j int) {
		cands[i], cands[j] = cands[j], cands[i]
	})
	if len(cands) > 4 {
		cands = cands[:4]
	}
	for _, cand := range cands {
		// Resolve the candidate to a playable track through
		// the single resolve path (InnerTube player, fast).
		track, err := reg.Resolve(ctx, "yt:"+cand)
		if err != nil || track.ID == "" || track.StreamURL == "" {
			continue
		}
		return track, true
	}
	return model.Track{}, false
}

// fetchSimilarSearch is autoplay Tier 2: a YouTube search for
// "<clean title> similar songs", returning the candidate video
// ids in search order. A track with no usable title yields no
// candidates (not an error) — the tier simply does not apply.
func fetchSimilarSearch(ctx context.Context, reg *resolver.Registry, last model.Track) ([]string, error) {
	title := cleanTitleForSimilar(last.Title)
	if title == "" {
		return nil, nil
	}
	results, err := reg.Search(ctx, resolver.SourceYouTube, title+" similar songs", 8)
	if err != nil {
		return nil, fmt.Errorf("mix: similar search failed: %w", err)
	}
	var ids []string
	for _, t := range results {
		if id := mixVideoID(t); id != "" {
			ids = append(ids, id)
		}
	}
	return ids, nil
}

// similarTitleNoise are the bracketed tags stripped from a seed
// title before the Tier 2 search: they describe the video's
// packaging ("(Official Video)", "[HD]"), not the song, and they
// narrow the search to the same upload instead of similar songs.
var similarTitleNoise = []string{
	"official video", "official audio", "official lyric video",
	"official music video", "lyric video", "lyrics", "music video",
	"visualizer", "visualiser", "remaster", "remastered", "hd", "4k",
	"8d audio", "slowed", "reverb",
}

// cleanTitleForSimilar strips the packaging off a seed title for
// the Tier 2 search: bracketed segments that are pure noise go,
// a trailing " - Topic" (YouTube auto-generated artist channels)
// goes, and whitespace collapses. The core title is kept intact —
// "(feat. X)" and "(From ...)" segments that name the song stay,
// because they are part of what "similar" means. An empty result
// falls back to the original title.
func cleanTitleForSimilar(title string) string {
	t := strings.TrimSpace(title)
	// Strip bracketed noise segments, repeatedly (a title can
	// carry "(Official Video) [HD]").
	for {
		start := strings.IndexAny(t, "([")
		if start < 0 {
			break
		}
		closer := byte(')')
		if t[start] == '[' {
			closer = ']'
		}
		end := strings.IndexByte(t[start+1:], closer)
		if end < 0 {
			break
		}
		end += start + 1
		inner := strings.ToLower(strings.TrimSpace(t[start+1 : end]))
		noise := false
		for _, n := range similarTitleNoise {
			if strings.Contains(inner, n) {
				noise = true
				break
			}
		}
		if !noise {
			break
		}
		t = strings.TrimSpace(t[:start] + " " + t[end+1:])
	}
	t = strings.TrimSuffix(t, " - Topic")
	t = strings.TrimSuffix(t, "- Topic")
	t = strings.Join(strings.Fields(t), " ")
	if t == "" {
		return strings.Join(strings.Fields(title), " ")
	}
	return t
}

// mixVideoID extracts the 11-char YouTube video id from a track's
// id (bare, or with a yt:/youtube: query prefix — the core's
// literal tracks may carry the prefix even though the resolver
// itself emits bare ids) or URL, or "" when the track is not a
// YouTube video.
func mixVideoID(track model.Track) string {
	if id := extractBareYouTubeID(track.ID); id != "" {
		return id
	}
	return resolver.ExtractYouTubeID(track.URL)
}

// extractBareYouTubeID strips a yt:/youtube: prefix (if any) and
// then extracts the video id the way the resolver does.
func extractBareYouTubeID(s string) string {
	q := strings.TrimSpace(s)
	lower := strings.ToLower(q)
	for _, p := range []string{"yt:", "youtube:"} {
		if strings.HasPrefix(lower, p) {
			q = q[len(p):]
			break
		}
	}
	return resolver.ExtractYouTubeID(q)
}

// fetchMix runs one InnerTube `next` call for the RD mix of
// videoID and returns the candidate video ids in mix order.
func fetchMix(ctx context.Context, videoID string) ([]string, error) {
	body, err := json.Marshal(map[string]any{
		"context": map[string]any{"client": map[string]any{
			"clientName": innerTubeMixClient, "clientVersion": innerTubeMixVersion,
		}},
		"playlistId": "RD" + videoID,
	})
	if err != nil {
		return nil, err
	}
	url := fmt.Sprintf("%s/youtubei/v1/next?key=%s", innerTubeMixBase, innerTubeMixKey)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", innerTubeMixUA)
	client := &http.Client{Timeout: mixPickerTimeout}
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("mix: innertube next request failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("mix: innertube next error: HTTP %d", resp.StatusCode)
	}
	bodyBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("mix: innertube next: read failed: %w", err)
	}
	return parseMixJSON(bodyBytes, videoID)
}

// parseMixJSON extracts the candidate video ids, in mix order,
// from one InnerTube `next` response body.
func parseMixJSON(bodyBytes []byte, videoID string) ([]string, error) {
	var result map[string]any
	if err := json.Unmarshal(bodyBytes, &result); err != nil {
		return nil, fmt.Errorf("mix: innertube next: bad JSON: %w", err)
	}
	items, ok := digMix(result, "contents", "twoColumnWatchNextResults", "playlist", "playlist", "contents").([]any)
	if !ok {
		return nil, fmt.Errorf("mix: mix contents not found for RD%s", videoID)
	}
	var out []string
	for _, item := range items {
		vid, ok := digMix(item, "playlistPanelVideoRenderer").(map[string]any)
		if !ok {
			continue
		}
		id, _ := vid["videoId"].(string)
		if id == "" {
			continue
		}
		out = append(out, id)
	}
	return out, nil
}

// digMix walks a decoded JSON tree by keys (the resolver's dig,
// re-implemented here because it is unexported there).
func digMix(node any, keys ...string) any {
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
