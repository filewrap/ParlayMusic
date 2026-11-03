package search

import (
	"context"
	"encoding/json"
	"fmt"
	"regexp"
	"strings"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// inlineMode mirrors vc-ts resolvers.js inlineMode exactly (same
// order): "np"/"s"/"l"/"help" match only as the whole query;
// "lib"/"del"/"search" take an argument after a space; a bare
// "lib"/"del"/"search" (no keyword) is that mode with no argument;
// queries shorter than 3 chars (or empty) are hint queries. The core
// renders the hint/help/no-results articles itself from the query it
// already has.
func inlineMode(query string) (mode, arg string) {
	q := strings.TrimSpace(query)
	if q == "" {
		return "hint", ""
	}
	lower := strings.ToLower(q)
	switch lower {
	case "np":
		return "np", ""
	case "s":
		return "s", ""
	case "l":
		return "l", ""
	case "help":
		return "help", ""
	}
	if m := inlineArgRe.FindStringSubmatch(q); m != nil {
		return strings.ToLower(m[1]), strings.TrimSpace(m[2])
	}
	if lower == "lib" || lower == "del" || lower == "search" {
		return lower, ""
	}
	if len([]rune(q)) < 3 {
		return "hint", q
	}
	return "search", q
}

var inlineArgRe = regexp.MustCompile(`^(?i)(lib|del|search)\s+(.+)$`)

type inlineRequest struct {
	Query  string `json:"query"`
	ChatID int64  `json:"chat_id"`
}

// libraryResult is the lib/del-mode result shape: a Track plus the
// local cache location the core needs to send (or delete) the file.
// cached_file_id is intentionally absent — the backend never learns
// real Telegram file_ids, and the core sends the local file via
// inputFileLocal. SizeBytes comes from the embedded Track.
type libraryResult struct {
	model.Track
	FilePath string `json:"file_path"`
	Cached   bool   `json:"cached"`
}

// handleInline implements search.inline (§7.10, §7.12): modes
// np/s/l/lib/del/help/search/hint. The response stays
// {results, mode} — the core knows the query it sent, so it renders
// the mode-specific result kind itself (cached audio for np, share
// message for s, lyrics article for l — lyrics text via vc.lyrics
// with the same chat_id) and the static hint/help/no-results
// articles itself.
//
//   - np/s/l return [current track] (or [] when nothing is playing
//     or no chat_id — never an error).
//   - lib/del return matching library entries (del = same as lib;
//     the core renders them with delete buttons, library.delete does
//     the deleting). This is the first mode with a real library
//     backend behind it — vc-ts returned [] here.
//   - hint/help/search-without-arg return [].
//   - search mode = YouTube first + Lemonic appended, sliced to 10
//     (JioSaavn skipped — no inline use). Lemonic is always
//     attempted and skipped only if the API itself fails. Inline
//     search never errors: a failed source is skipped, and both
//     failing returns [] (the user's typing shows nothing either
//     way).
func (s *Service) handleInline(ctx context.Context, req uds.Request) (any, error) {
	var body inlineRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad search.inline request: %v", err))
		}
	}
	mode, arg := inlineMode(body.Query)
	switch mode {
	case "np", "s", "l":
		track := s.currentTrack(body.ChatID)
		if track == nil {
			return map[string]any{"results": []model.Track{}, "mode": mode}, nil
		}
		return map[string]any{"results": s.enrichWithLibrary([]model.Track{*track}), "mode": mode}, nil
	case "lib", "del":
		if arg == "" {
			return map[string]any{"results": []libraryResult{}, "mode": mode}, nil
		}
		entries := s.index.Search(arg)
		results := make([]libraryResult, 0, len(entries))
		for _, e := range entries {
			results = append(results, libraryResult{
				Track: model.Track{
					ID: e.ID, Title: e.Title, Artist: e.Artist, Source: e.Source,
					SizeBytes: e.SizeBytes,
				},
				FilePath: e.FilePath,
				Cached:   true,
			})
		}
		return map[string]any{"results": results, "mode": mode}, nil
	case "hint", "help":
		return map[string]any{"results": []model.Track{}, "mode": mode}, nil
	case "search":
		if arg == "" {
			return map[string]any{"results": []model.Track{}, "mode": mode}, nil
		}
		var results []model.Track
		if s.deps != nil && s.deps.Registry != nil {
			if yt, err := s.deps.Registry.Search(ctx, resolver.SourceYouTube, arg, 5); err == nil {
				results = append(results, yt...)
			}
		}
		if lem, _, err := s.lemonic.Search(ctx, s.depsRegistry(), arg, 5); err == nil {
			results = append(results, lem...)
		}
		if len(results) > 10 {
			results = results[:10]
		}
		if results == nil {
			results = []model.Track{}
		}
		return map[string]any{"results": s.enrichWithLibrary(results), "mode": mode}, nil
	}
	return map[string]any{"results": []model.Track{}, "mode": mode}, nil
}

// currentTrack returns the room's current track for a chat, or nil
// when nothing is playing (no chat_id, no room, no current track).
func (s *Service) currentTrack(chatID int64) *model.Track {
	if chatID == 0 || s.deps == nil || s.deps.Store == nil {
		return nil
	}
	room, found, err := s.deps.Store.GetRoom(chatID)
	if err != nil || !found || room.Current == nil {
		return nil
	}
	track := *room.Current
	return &track
}

// depsRegistry returns the resolver registry (nil when unwired).
func (s *Service) depsRegistry() *resolver.Registry {
	if s.deps == nil {
		return nil
	}
	return s.deps.Registry
}
