// Lyrics — combined LRCLib (from vc-ts) + NetEase synced LRC
// resolver.
//
// LRCLib (keyless): GET https://lrclib.net/api/search?q=<title
// <first artist>>. Returns plain lyrics, or synced (timestamped)
// lyrics when available. One retry on 503. A miss is reported as no
// lyrics, never invented. JioSaavn cannot supply lyric text (its
// /song payload has has_lyrics:true but lyrics:null).
//
// NetEase: GET /api/song/lyric via the absorbed netease client
// (synced LRC when the track has one). Preferred when the caller
// knows a NetEase track id, because NetEase lyrics are synced by
// construction.
package resolver

import (
	"context"
	"errors"
	"fmt"
	"net/url"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/resolver/netease"
)

// LyricsResult is the vc.lyrics / netease.lyrics response shape
// (contracts/ipc.json).
type LyricsResult struct {
	Lyrics       string `json:"lyrics"`
	Source       string `json:"source"` // lrclib|netease|file-tag
	TrackID      string `json:"track_id"`
	Title        string `json:"title"`
	Artist       string `json:"artist"`
	Synced       bool   `json:"synced"`
	Truncated    bool   `json:"truncated"`
	Instrumental bool   `json:"instrumental,omitempty"`
}

// LyricsResolver fetches lyrics from LRCLib, with NetEase as the
// synced source when a NetEase track id is known.
type LyricsResolver struct {
	netease *NetEaseResolver
}

// NewLyricsResolver builds a lyrics resolver.
func NewLyricsResolver(netease *NetEaseResolver) *LyricsResolver {
	return &LyricsResolver{netease: netease}
}

// Source implements Resolver (lyrics is not a play source; this is
// only used for registry bookkeeping).
func (l *LyricsResolver) Source() Source { return SourceLyrics }

// Resolve is not a play path for lyrics.
func (l *LyricsResolver) Resolve(ctx context.Context, query string) (Track, error) {
	return Track{}, engineUnavailable(SourceLyrics, "lyrics is not a playable source; use Lyrics() instead")
}

// Search is not a play path for lyrics.
func (l *LyricsResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	return nil, engineUnavailable(SourceLyrics, "lyrics is not a searchable source; use Lyrics() instead")
}

// Lyrics fetches lyrics for a track: NetEase first when a numeric
// NetEase id is given (synced LRC), else LRCLib by title + artist.
func (l *LyricsResolver) Lyrics(ctx context.Context, title, artist, neteaseID string) (*LyricsResult, error) {
	if neteaseID != "" {
		if id, err := strconv.ParseInt(strings.TrimSpace(neteaseID), 10, 64); err == nil {
			if res, err := l.neteaseLyrics(ctx, id, title, artist); err == nil {
				return res, nil
			} else if !errors.Is(err, netease.ErrNoLyrics) {
				// NetEase errored (not just "no lyrics"): fall
				// through to LRCLib rather than failing outright.
				_ = err
			} else {
				return nil, err
			}
		}
	}
	return l.lrclibLyrics(ctx, title, artist)
}

// neteaseLyrics fetches synced LRC from NetEase.
func (l *LyricsResolver) neteaseLyrics(ctx context.Context, id int64, title, artist string) (*LyricsResult, error) {
	_ = ctx
	lyrics, err := l.netease.Lyrics(id)
	if err != nil {
		return nil, err
	}
	return &LyricsResult{
		Lyrics: lyrics, Source: "netease",
		TrackID: fmt.Sprint(id), Title: title, Artist: artist,
		Synced: strings.Contains(lyrics, "["),
	}, nil
}

// lrclibLyrics fetches lyrics from LRCLib (vc-ts fetchLyrics):
// query is "<title> <first artist>" (JioSaavn artist strings are
// comma-joined lists — only the first artist is used, LRCLib
// answers far more reliably that way). One retry on transient
// failure; an empty result is a miss, not an error.
func (l *LyricsResolver) lrclibLyrics(ctx context.Context, title, artist string) (*LyricsResult, error) {
	firstArtist := strings.TrimSpace(strings.Split(artist, ",")[0])
	q := strings.TrimSpace(strings.Join([]string{title, firstArtist}, " "))
	q = strings.TrimSpace(q)
	if q == "" {
		return nil, engineUnavailable(SourceLyrics, "no title/artist to look lyrics up for")
	}
	endpoint := "https://lrclib.net/api/search?" + url.Values{"q": {q}}.Encode()
	var lastErr error
	for attempt := 0; attempt < 2; attempt++ {
		var data []struct {
			TrackName    string `json:"trackName"`
			Name         string `json:"name"`
			ArtistName   string `json:"artistName"`
			PlainLyrics  string `json:"plainLyrics"`
			SyncedLyrics string `json:"syncedLyrics"`
			Instrumental bool   `json:"instrumental"`
		}
		if err := getJSON(ctx, endpoint, nil, &data); err != nil {
			lastErr = err
			if attempt == 0 {
				select {
				case <-ctx.Done():
					return nil, ctx.Err()
				case <-time.After(500 * time.Millisecond):
				}
			}
			continue
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("lyrics: LRCLib has no lyrics for %q", q)
		}
		norm := func(s string) string {
			return strings.Join(normWords(s), " ")
		}
		hit := data[0]
		for _, d := range data {
			if norm(d.TrackName) == norm(title) || norm(d.Name) == norm(title) {
				hit = d
				break
			}
		}
		lyrics := hit.SyncedLyrics
		synced := true
		if lyrics == "" {
			lyrics = hit.PlainLyrics
			synced = false
		}
		if lyrics == "" {
			return nil, fmt.Errorf("lyrics: LRCLib hit for %q has no lyric text", q)
		}
		return &LyricsResult{
			Lyrics: lyrics, Source: "lrclib",
			Title: hit.TrackName, Artist: hit.ArtistName,
			Synced: synced, Instrumental: hit.Instrumental,
		}, nil
	}
	return nil, fmt.Errorf("lyrics: LRCLib request failed: %v", lastErr)
}
