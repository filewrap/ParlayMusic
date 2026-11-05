package download

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// mixRequest is the mix.generate body (§9-E3).
type mixRequest struct {
	ChatID      int64  `json:"chat_id"`
	Query       string `json:"query"`
	RequestedBy int64  `json:"requested_by"`
}

// handleMix implements mix.generate (§9-E3): /mix [song or URL], or
// bare /mix while a track is playing (mix based on the current
// track). Resolution: explicit query -> resolve it (same router as
// vc.play) -> that track is the seed; no query -> the room's current
// track is the seed; no query and nothing playing -> bad_request.
// Expansion: the YouTube RD-mix picker (the mix is a flat playlist,
// so one fetch usually yields the 10), run to collect up to 10
// distinct tracks (exclude = seed + room queue + already-picked).
// The response is a track LIST — the core enqueues them (vc.queue
// add per track) and reports "All mix tracks are already in the
// queue or playing." when the filtered list is empty.
//
// Exclusion gap: the inventory also excludes the autoplay history,
// but model.RoomState carries no autoplay_history field (Worker A /
// the room package owns that state), so this handler excludes the
// seed + the room queue + already-picked tracks, and notes the gap.
// Seed is not a YouTube track -> 502 (only YouTube has mixes — the
// same limit as autoplay). Mix fetch fails -> 502 "Failed to get mix:
// Please try again later."
func (s *Service) handleMix(ctx context.Context, req uds.Request) (any, error) {
	var body mixRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad mix.generate request: %v", err))
		}
	}
	if body.ChatID == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "chat_id is required")
	}
	var seed model.Track
	if strings.TrimSpace(body.Query) != "" {
		track, err := s.resolveTrack(ctx, body.Query)
		if err != nil {
			return nil, err
		}
		seed = track
	} else {
		current, err := s.roomCurrentTrack(body.ChatID)
		if err != nil {
			return nil, err
		}
		if current == nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, "Nothing is playing — give me a song to mix from.")
		}
		seed = *current
	}
	if seed.Source != string(resolver.SourceYouTube) {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("Failed to get mix: Please try again later. (only YouTube tracks have mixes; the seed is %s)", seed.Source))
	}
	seedID := strings.TrimPrefix(seed.ID, "yt:")
	if seedID == "" {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Failed to get mix: Please try again later.")
	}
	exclude := map[string]bool{seedID: true, seed.ID: true}
	if s.deps != nil && s.deps.Store != nil {
		if queue, err := s.deps.Store.GetQueue(body.ChatID); err == nil {
			for _, item := range queue {
				exclude[item.Track.ID] = true
				exclude[strings.TrimPrefix(item.Track.ID, "yt:")] = true
			}
		}
		// Autoplay history is excluded too: the room state carries
		// it (model.RoomState.AutoplayHistory, capped at 50), and
		// the room Manager persists it synchronously on every
		// transition, so the store read is current.
		if st, found, err := s.deps.Store.GetRoom(body.ChatID); err == nil && found {
			for _, id := range st.AutoplayHistory {
				exclude[id] = true
				exclude[strings.TrimPrefix(id, "yt:")] = true
			}
		}
	}
	tracks, err := s.fetchMix(ctx, seedID, exclude, 10)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Failed to get mix: Please try again later.")
	}
	if tracks == nil {
		tracks = []model.Track{}
	}
	return map[string]any{
		"tracks":       tracks,
		"source_track": seed,
	}, nil
}

// roomCurrentTrack returns the room's current track for a chat, or
// nil when nothing is playing.
func (s *Service) roomCurrentTrack(chatID int64) (*model.Track, error) {
	if s.deps == nil || s.deps.Store == nil {
		return nil, nil
	}
	st, found, err := s.deps.Store.GetRoom(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("room read failed: %v", err))
	}
	// The active track is the front of the stored list, and only
	// when the room is Playing — a queued-only room has no
	// current track to mix from.
	if !found || !st.Playing {
		return nil, nil
	}
	queue, err := s.deps.Store.GetQueue(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("queue read failed: %v", err))
	}
	if len(queue) == 0 {
		return nil, nil
	}
	track := queue[0].Track
	return &track, nil
}

// fetchMix fetches the YouTube RD-mix flat playlist for videoID via
// the warm yt-dlp daemon (the same pick the old engines used for
// autoplay: watch?v=<id>&list=RD<id>, flat playlist, first entries
// not in exclude) and returns up to limit distinct tracks.
func (s *Service) fetchMix(ctx context.Context, videoID string, exclude map[string]bool, limit int) ([]model.Track, error) {
	if s.deps == nil || s.deps.Registry == nil || s.deps.Registry.YtDlp() == nil {
		return nil, fmt.Errorf("yt-dlp daemon is not configured")
	}
	url := "https://www.youtube.com/watch?v=" + videoID + "&list=RD" + videoID
	info, err := s.deps.Registry.YtDlp().Extract(ctx, url)
	if err != nil {
		return nil, err
	}
	entries, _ := info["entries"].([]any)
	var out []model.Track
	for _, e := range entries {
		if len(out) >= limit {
			break
		}
		m, ok := e.(map[string]any)
		if !ok {
			continue
		}
		track := mixTrackFromEntry(m)
		if track.ID == "" || exclude[track.ID] {
			continue
		}
		exclude[track.ID] = true
		out = append(out, track)
	}
	return out, nil
}

// mixTrackFromEntry maps one yt-dlp flat playlist entry to a Track
// (id/title/uploader-or-channel/duration; the watch URL is the
// track URL, the stream URL resolves at play time).
func mixTrackFromEntry(m map[string]any) model.Track {
	id, _ := m["id"].(string)
	title, _ := m["title"].(string)
	if title == "" {
		title = "YouTube Track"
	}
	artist, _ := m["uploader"].(string)
	if artist == "" {
		artist, _ = m["channel"].(string)
	}
	duration := resolver.ParseDuration(m["duration"])
	return model.Track{
		ID: id, Title: title, Artist: artist, Source: string(resolver.SourceYouTube),
		URL:         "https://www.youtube.com/watch?v=" + id,
		DurationSec: int64(duration),
		DurationStr: resolver.FormatDurationStr(duration, false),
	}
}
