package lyrics

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/resolver/netease"
)

func TestNewRequiresNetEaseResolver(t *testing.T) {
	reg := resolver.NewRegistry()
	f, err := New(reg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if f == nil || f.res == nil {
		t.Fatalf("New returned a nil fetcher/resolver")
	}
}

// TestFetchEmptyQueryFailsHonestly pins the mapping boundary: an
// empty title/artist never reaches the network — the resolver
// fails naming the missing title, and the Fetcher passes that
// failure through unchanged (it is a caller error, not a miss and
// not a source outage).
func TestFetchEmptyQueryFailsHonestly(t *testing.T) {
	reg := resolver.NewRegistry()
	f, err := New(reg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	_, err = f.Fetch(context.Background(), "", "", "")
	if err == nil || !strings.Contains(err.Error(), "no title/artist") {
		t.Fatalf("Fetch empty title/artist: err=%v, want an error naming the missing title", err)
	}
}

func TestFetchForTrackNonNumericNetEaseID(t *testing.T) {
	reg := resolver.NewRegistry()
	f, err := New(reg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// A NetEase track whose id is not numeric has no NetEase leg
	// to run (ExtractNetEaseID returns 0), so the LRCLib leg runs
	// with the empty title and fails naming it — without touching
	// the network. (A numeric id would run the NetEase leg live;
	// that routing is the resolver's tested behaviour.)
	track := model.Track{ID: "not-a-number", Title: "", Artist: "", Source: "netease"}
	_, err = f.FetchForTrack(context.Background(), track)
	if err == nil || !strings.Contains(err.Error(), "no title/artist") {
		t.Fatalf("FetchForTrack non-numeric id: err=%v, want an error naming the missing title", err)
	}
}

// TestFetchNetEaseInvalidIDs pins the validation boundary: a
// non-numeric, zero, or negative id never reaches the network —
// FetchNetEase fails naming the bad id. Callers (netease.lyrics)
// validate the id shape before calling, so this is a second line
// of defence, not the first.
func TestFetchNetEaseInvalidIDs(t *testing.T) {
	reg := resolver.NewRegistry()
	f, err := New(reg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	for _, id := range []string{"", "  ", "abc", "netease:123", "12.5", "0", "-5", "123abc"} {
		_, err := f.FetchNetEase(context.Background(), id)
		if err == nil || !strings.Contains(err.Error(), "invalid netease id") {
			t.Fatalf("FetchNetEase(%q): err=%v, want an error naming the invalid id", id, err)
		}
	}
}

// TestFetchNetEaseLive verifies the NetEase-only path against the
// real endpoint (anonymous — no MUSIC_U; the lyric endpoint does
// not need login). Gated on PARLAY_LIVE_TEST=1 like the other live
// tests: it asserts the success envelope only. The miss mapping
// (ErrNoLyrics wrapping both sentinels) cannot be produced live
// by id guessing — the endpoint returns a 22-byte timestamp line
// even for nonexistent ids (probed 2026-11-03: ids 1, 2, 3,
// 99999999999 all returned lyrics) — so that branch is verified
// by code inspection, not by a test, and is flagged as such in
// the Wave 2 report.
func TestFetchNetEaseLive(t *testing.T) {
	if os.Getenv("PARLAY_LIVE_TEST") != "1" {
		t.Skip("live test: set PARLAY_LIVE_TEST=1 to run")
	}
	reg := resolver.NewRegistry()
	f, err := New(reg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	res, err := f.FetchNetEase(context.Background(), "123456")
	if err != nil {
		t.Fatalf("FetchNetEase live: %v", err)
	}
	if res.Source != "netease" || res.TrackID != "123456" || !res.Synced || strings.TrimSpace(res.Lyrics) == "" {
		t.Fatalf("FetchNetEase live: unexpected result %+v", res)
	}
}

// TestMapNetEaseLyricsErr pins the contract the netease.lyrics
// rewire depends on: a NetEase "no lyrics" error (built exactly
// the way the client builds it) must satisfy errors.Is against
// BOTH sentinels, so the handler's existing isNoLyrics check
// (netease.ErrNoLyrics) keeps working and the shared check
// (lyrics.ErrNoLyrics) works too. A non-miss error passes through
// unchanged.
func TestMapNetEaseLyricsErr(t *testing.T) {
	miss := fmt.Errorf("%w for track %d", netease.ErrNoLyrics, 123456)
	mapped := mapNetEaseLyricsErr(miss)
	if !errors.Is(mapped, ErrNoLyrics) {
		t.Fatalf("mapped miss must match lyrics.ErrNoLyrics: %v", mapped)
	}
	if !errors.Is(mapped, netease.ErrNoLyrics) {
		t.Fatalf("mapped miss must match netease.ErrNoLyrics: %v", mapped)
	}
	boom := errors.New("boom")
	if mapNetEaseLyricsErr(boom) != boom {
		t.Fatalf("non-miss error must pass through unchanged")
	}
}
