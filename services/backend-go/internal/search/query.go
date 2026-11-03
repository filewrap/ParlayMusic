package search

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// querySources is the search.query fan-out order (§7.9): youtube,
// jiosaavn, lemonic, spotify, deezer, netease. Soulseek and
// Navidrome are not in the list.
var querySources = []string{"youtube", "jiosaavn", "lemonic", "spotify", "deezer", "netease"}

// querySourceToResolver maps a fan-out source name to the registry
// source. Lemonic has no registry resolver — it is handled by the
// Lemonic client.
func querySourceToResolver(name string) (resolver.Source, bool) {
	switch name {
	case "youtube":
		return resolver.SourceYouTube, true
	case "jiosaavn":
		return resolver.SourceJioSaavn, true
	case "spotify":
		return resolver.SourceSpotify, true
	case "deezer":
		return resolver.SourceDeezer, true
	case "netease":
		return resolver.SourceNetEase, true
	}
	return "", false
}

type queryRequest struct {
	Query  string `json:"query"`
	Source string `json:"source"`
	Limit  int    `json:"limit"`
}

// handleQuery implements search.query (§7.9): fan-out across sources,
// per-source status, results concatenated per source and sliced to
// limit*3. Response {results, source_status} (superset per D1).
// A missing credential on one source (e.g. Spotify without
// SPOTIFY_CLIENT_ID) marks that source "failed" — it never fakes
// YouTube-attributed Spotify results and never fails the whole
// query. All sources failing -> 502 engine_unavailable naming each
// source's error. limit clamps to 1..20, default 5.
func (s *Service) handleQuery(ctx context.Context, req uds.Request) (any, error) {
	var body queryRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad search.query request: %v", err))
		}
	}
	query := body.Query
	if query == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "query is required")
	}
	limit := body.Limit
	if limit <= 0 {
		limit = 5
	}
	if limit > 20 {
		limit = 20
	}
	var sources []string
	switch body.Source {
	case "", "all":
		sources = querySources
	default:
		ok := false
		for _, src := range querySources {
			if src == body.Source {
				ok = true
				break
			}
		}
		if !ok {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown source %q (want one of youtube|jiosaavn|lemonic|spotify|deezer|netease|all)", body.Source))
		}
		sources = []string{body.Source}
	}
	if s.deps == nil || s.deps.Registry == nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "resolver registry is not configured")
	}
	// Bound the fan-out so the handler answers before the UDS frame
	// deadline.
	fanCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()

	type sourceResult struct {
		source string
		tracks []model.Track
		err    error
	}
	results := make(chan sourceResult, len(sources))
	var wg sync.WaitGroup
	for _, src := range sources {
		wg.Add(1)
		go func(src string) {
			defer wg.Done()
			if src == "lemonic" {
				tracks, _, err := s.lemonic.Search(fanCtx, s.deps.Registry, query, limit)
				results <- sourceResult{source: src, tracks: tracks, err: err}
				return
			}
			resSrc, _ := querySourceToResolver(src)
			tracks, err := s.deps.Registry.Search(fanCtx, resSrc, query, limit)
			results <- sourceResult{source: src, tracks: tracks, err: err}
		}(src)
	}
	wg.Wait()
	close(results)

	bySource := make(map[string][]model.Track, len(sources))
	status := make(map[string]string, len(sources))
	var failures []string
	for r := range results {
		switch {
		case r.err != nil:
			status[r.source] = "failed"
			failures = append(failures, fmt.Sprintf("%s: %v", r.source, r.err))
		case len(r.tracks) == 0:
			status[r.source] = "empty"
		default:
			status[r.source] = "ok"
			bySource[r.source] = r.tracks
		}
	}
	if len(failures) == len(sources) {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("search failed on all sources: %s", strings.Join(failures, "; ")))
	}
	var merged []model.Track
	for _, src := range sources {
		merged = append(merged, bySource[src]...)
	}
	if len(merged) > limit*3 {
		merged = merged[:limit*3]
	}
	if merged == nil {
		merged = []model.Track{}
	}
	merged = s.enrichWithLibrary(merged)
	return map[string]any{"results": merged, "source_status": status}, nil
}
