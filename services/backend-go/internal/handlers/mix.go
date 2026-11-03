// YouTube RD-mix picker — the autoplay pick (inventory §3.2).
//
// The picker fetches the mix for a YouTube track — watch?v=<id>&
// list=RD<id> — and returns the first entry not in the exclude set
// (last + queue + history), resolved to a playable track.
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
		candidates, err := fetchMix(ctx, videoID)
		if err != nil {
			return model.Track{}, err
		}
		for _, cand := range candidates {
			if exclude[cand] {
				continue
			}
			// Resolve the candidate to a playable track through
			// the single resolve path (InnerTube player, fast).
			// An unresolvable candidate is skipped, not fatal:
			// the mix has more entries.
			track, err := reg.Resolve(ctx, "yt:"+cand)
			if err != nil || track.ID == "" || track.StreamURL == "" {
				continue
			}
			return track, nil
		}
		return model.Track{}, nil
	}
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
