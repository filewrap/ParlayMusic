// Package lyrics is the shared lyrics resolver used by vc.lyrics
// and by netease.lyrics (Worker C): LRCLib (keyless) + NetEase
// synced LRC.
//
// There is exactly ONE implementation of the fetch logic —
// resolver.LyricsResolver (Wave 1, internal/resolver/lyrics.go)
// plus the NetEase client's Lyrics call. This package does NOT
// re-implement either: it is the shared entry point that wraps
// those resolvers and normalises their outcomes into the two cases
// the handlers need:
//
//   - ErrNoLyrics: the track simply has no lyrics (LRCLib miss, or
//     NetEase ErrNoLyrics) — the handler answers not_found.
//   - any other error: the lyrics source failed (LRCLib 503 after
//     its one retry, transport failure) — the handler answers
//     engine_unavailable.
//
// Lyrics are never invented: a miss is a miss, not an empty string.
//
// Two fetch paths exist, and they must not be confused:
//
//   - Fetch / FetchForTrack: NetEase first when a NetEase id is
//     known, else LRCLib by title + artist (and LRCLib as the
//     fallback when NetEase errors without saying "no lyrics").
//     This is the vc.lyrics path.
//   - FetchNetEase: NetEase ONLY, never LRCLib. This is the
//     netease.lyrics path: that action must answer 404 when NetEase
//     has no lyrics, and its response hardcodes source:"netease" —
//     routing it through Fetch could return LRCLib lyrics labelled
//     netease whenever NetEase errored and a title was known.
package lyrics

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/resolver/netease"
)

// ErrNoLyrics marks a lookup that completed but found no lyrics.
var ErrNoLyrics = errors.New("lyrics: no lyrics found")

// Result is the vc.lyrics / netease.lyrics response shape
// (contracts/ipc.json): the resolver's LyricsResult, shared.
type Result = resolver.LyricsResult

// Fetcher fetches lyrics from LRCLib, with NetEase as the synced
// source when a NetEase track id is known. It keeps both the
// combined resolver (Fetch) and the raw NetEase resolver
// (FetchNetEase); the fetch itself always happens in the resolver
// / netease client — this package only wraps and normalises.
type Fetcher struct {
	res *resolver.LyricsResolver
	ne  *resolver.NetEaseResolver
}

// New builds a Fetcher from the registry's NetEase resolver. It
// fails honestly when the registry has no NetEase resolver to wrap
// — a lyrics fetcher without its NetEase leg is a wiring bug, not a
// degraded mode.
func New(reg *resolver.Registry) (*Fetcher, error) {
	res, err := reg.ResolverFor(resolver.SourceNetEase)
	if err != nil {
		return nil, fmt.Errorf("lyrics: %w", err)
	}
	ne, ok := res.(*resolver.NetEaseResolver)
	if !ok {
		return nil, fmt.Errorf("lyrics: registry resolver for netease is %T, not *resolver.NetEaseResolver", res)
	}
	return &Fetcher{res: resolver.NewLyricsResolver(ne), ne: ne}, nil
}

// Fetch fetches lyrics for a track: NetEase first when a numeric
// NetEase id is given (synced LRC), else LRCLib by title + artist.
// A lookup that finds nothing returns ErrNoLyrics; a source
// failure returns the underlying error.
func (f *Fetcher) Fetch(ctx context.Context, title, artist, neteaseID string) (*Result, error) {
	res, err := f.res.Lyrics(ctx, title, artist, neteaseID)
	if err != nil {
		if errors.Is(err, netease.ErrNoLyrics) {
			return nil, ErrNoLyrics
		}
		msg := err.Error()
		if strings.Contains(msg, "has no lyrics for") || strings.Contains(msg, "has no lyric text") {
			return nil, ErrNoLyrics
		}
		return nil, err
	}
	return res, nil
}

// FetchNetEase fetches lyrics for one NetEase track id from
// NetEase ONLY — no LRCLib fallback, ever. This is the
// netease.lyrics path: when NetEase has no lyrics it returns
// ErrNoLyrics (the caller answers not_found); a NetEase outage
// returns the underlying error (the caller answers
// engine_unavailable); LRCLib text can never come back labelled
// netease.
//
// The returned error wraps BOTH lyrics.ErrNoLyrics and
// netease.ErrNoLyrics, so the netease handler's existing
// errors.Is(err, netease.ErrNoLyrics) check keeps working after
// the rewire. An invalid (non-numeric, zero, or negative) id is a
// plain error — callers validate the id shape before calling.
//
// Title/artist are left empty: the NetEase lyric endpoint returns
// lyric text only, and the caller fills them from its own track
// lookup (as netease.lyrics already does via Registry.Resolve).
// The ctx is accepted for signature symmetry with Fetch; the
// NetEase client carries its own 10s timeout and does not take a
// context yet.
func (f *Fetcher) FetchNetEase(ctx context.Context, neteaseID string) (*Result, error) {
	_ = ctx
	id, err := strconv.ParseInt(strings.TrimSpace(neteaseID), 10, 64)
	if err != nil || id <= 0 {
		return nil, fmt.Errorf("lyrics: invalid netease id %q", neteaseID)
	}
	lyric, err := f.ne.Lyrics(id)
	if err != nil {
		return nil, mapNetEaseLyricsErr(err)
	}
	return &Result{
		Lyrics:  lyric,
		Source:  "netease",
		TrackID: strconv.FormatInt(id, 10),
		Synced:  strings.Contains(lyric, "["),
	}, nil
}

// mapNetEaseLyricsErr converts a NetEase lyrics error into the
// Fetcher's error contract: a "no lyrics" miss wraps BOTH
// lyrics.ErrNoLyrics and netease.ErrNoLyrics (so the netease
// handler's existing errors.Is check keeps working after the
// rewire); any other error passes through unchanged.
func mapNetEaseLyricsErr(err error) error {
	if errors.Is(err, netease.ErrNoLyrics) {
		return fmt.Errorf("%w: %w", ErrNoLyrics, err)
	}
	return err
}

// FetchForTrack resolves title/artist (and the NetEase id, when the
// track is a NetEase track) from a contract Track and fetches its
// lyrics.
func (f *Fetcher) FetchForTrack(ctx context.Context, track model.Track) (*Result, error) {
	neteaseID := ""
	if track.Source == string(resolver.SourceNetEase) {
		if id := resolver.ExtractNetEaseID(track.ID); id != 0 {
			neteaseID = fmt.Sprint(id)
		}
	}
	res, err := f.Fetch(ctx, track.Title, track.Artist, neteaseID)
	if err != nil {
		return nil, err
	}
	if res.TrackID == "" {
		res.TrackID = track.ID
	}
	return res, nil
}
