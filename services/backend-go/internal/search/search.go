// Package search owns search.query, search.inline, lemonic.search and
// lemonic.download (actions-inventory §7.9–§7.12).
//
//   - search.query fans out across youtube, jiosaavn, lemonic,
//     spotify, deezer and netease (the §7.9 list; soulseek and
//     navidrome are not in it) via the shared resolver registry,
//     plus the Lemonic client for the lemonic lane.
//   - search.inline mirrors the old vc-ts /v1/inline modes
//     (np/s/l/lib/del/help/search/hint) exactly.
//   - lemonic.search / lemonic.download port vc-ts resolvers.js
//     (searchLemonic, lemonicRawTrack) and main.js /v1/lemonic/*:
//     the Lemonic API's own search endpoint first (unauthenticated,
//     Bearer <LEMONIC_API_KEY> when set), falling back to Spotify
//     client-credentials search (results mapped to lemonic:<spotify-id>
//     — exactly what Lemonic's /api/raw/track/<spotify-id> expects).
//
// E10 enrichment: every Track this package emits carries size_bytes
// from the library index when the track is cached on disk
// (enrichWithLibrary). cached_file_id stays empty — the backend never
// learns real Telegram file_ids (no core→backend reporting action
// exists); inventing one would break the core if passed to sendAudio.
package search

import (
	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// Service holds the search handlers' dependencies.
type Service struct {
	deps    *app.Deps
	lemonic *LemonicClient
	index   *library.Index
}

// NewService builds a Service over deps with the default Lemonic
// client (env LEMONIC_API / LEMONIC_API_KEY) and the process-wide
// library index.
func NewService(deps *app.Deps) *Service {
	return &Service{deps: deps, lemonic: NewLemonicClient(), index: library.SharedIndex()}
}

// NewServiceWithLemonic builds a Service with explicit dependencies
// (tests).
func NewServiceWithLemonic(deps *app.Deps, lemonic *LemonicClient, index *library.Index) *Service {
	if lemonic == nil {
		lemonic = NewLemonicClient()
	}
	if index == nil {
		index = library.NewIndex()
	}
	return &Service{deps: deps, lemonic: lemonic, index: index}
}

// Register registers search.query, search.inline, lemonic.search and
// lemonic.download on the UDS server.
func Register(s *uds.Server, d *app.Deps) {
	svc := NewService(d)
	s.Register("search.query", svc.handleQuery)
	s.Register("search.inline", svc.handleInline)
	s.Register("lemonic.search", svc.handleLemonicSearch)
	s.Register("lemonic.download", svc.handleLemonicDownload)
}

// enrichWithLibrary fills size_bytes on tracks that are cached on
// disk (library index), per §9-E10. It never invents a cached
// file_id.
func (s *Service) enrichWithLibrary(tracks []model.Track) []model.Track {
	if s.index == nil {
		return tracks
	}
	for i := range tracks {
		if e, ok := s.index.Get(tracks[i].ID); ok {
			tracks[i].SizeBytes = e.SizeBytes
		}
	}
	return tracks
}
