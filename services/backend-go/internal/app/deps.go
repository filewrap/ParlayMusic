// Package app is the handler dependency bundle for the unified Go
// backend (Phase 3, Wave 2 contract). Every feature package exposes
// func Register(s *uds.Server, d *app.Deps); Deps is the single
// bundle they all receive, so no handler reaches for globals and no
// second store, resolver, voice manager, room manager, or lyrics
// fetcher can be constructed by accident.
//
// This replaces the temporary contract-shaped stub Worker C left in
// app.go (deleted with this file's arrival, per that stub's own
// header): same Deps shape, plus the New constructor.
package app

import (
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// Deps is the handler dependency bundle (Wave 2 contract, exact
// shape). Store is the single embedded bbolt store; Config is the
// loaded parlay.toml; Registry is the single resolver set; Voice is
// the single ntgcalls manager; Server is the UDS server handlers
// register on and emit events through; Room is the single room
// state machine (the one owner of now_playing/queue/progress —
// feature packages read room state through d.Room, never a second
// Manager); Lyrics is the single shared lyrics fetcher (vc.lyrics
// and netease.lyrics both use it; spotify keeps its own
// Genius→Musixmatch→LRCLib chain and does NOT go through it).
//
// Lyrics may be nil: it means lyrics are not configured (the
// fetcher could not be built from the registry), and vc.lyrics
// answers engine_unavailable in that case. Every other field must
// be non-nil; New panics otherwise.
type Deps struct {
	Store    *store.Store
	Config   *config.Config
	Registry *resolver.Registry
	Voice    *voice.Manager
	Server   *uds.Server
	Room     *room.Manager
	Lyrics   *lyrics.Fetcher
}

// New builds the dependency bundle. No argument may be nil except
// Lyrics (see Deps): a nil dependency is a wiring bug, and failing
// at construction beats a nil-pointer panic on the first request.
// main.go constructs each dependency exactly once and passes it in
// — New only assembles, it never constructs a second Manager,
// Fetcher, store, or client.
func New(st *store.Store, cfg *config.Config, reg *resolver.Registry, voiceMgr *voice.Manager, srv *uds.Server, rooms *room.Manager, lyr *lyrics.Fetcher) *Deps {
	if st == nil {
		panic("app: New called with nil Store")
	}
	if cfg == nil {
		panic("app: New called with nil Config")
	}
	if reg == nil {
		panic("app: New called with nil Registry")
	}
	if voiceMgr == nil {
		panic("app: New called with nil Voice manager")
	}
	if srv == nil {
		panic("app: New called with nil Server")
	}
	if rooms == nil {
		panic("app: New called with nil Room manager")
	}
	return &Deps{
		Store:    st,
		Config:   cfg,
		Registry: reg,
		Voice:    voiceMgr,
		Server:   srv,
		Room:     rooms,
		Lyrics:   lyr,
	}
}
