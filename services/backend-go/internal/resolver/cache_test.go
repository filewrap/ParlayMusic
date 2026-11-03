package resolver

import (
	"context"
	"strconv"
	"strings"
	"testing"
	"time"

	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
)

// fakeClock is an injected clock for expiry tests: time moves only
// when the test advances it, so expiry is proven without sleeping
// on real CDN values.
type fakeClock struct{ now time.Time }

func (f *fakeClock) Now() time.Time { return f.now }
func (f *fakeClock) Advance(d time.Duration) {
	f.now = f.now.Add(d)
}

// stubResolver counts Resolve calls and returns a fixed track.
type stubResolver struct {
	src   Source
	calls int
	track Track
	err   error
}

func (s *stubResolver) Source() Source { return s.src }
func (s *stubResolver) Resolve(_ context.Context, _ string) (Track, error) {
	s.calls++
	return s.track, s.err
}
func (s *stubResolver) Search(_ context.Context, _ string, _ int) ([]Track, error) {
	return []Track{s.track}, nil
}

func TestCacheKeyFormat(t *testing.T) {
	cases := []struct {
		track model.Track
		want  string
	}{
		{model.Track{ID: "dQw4w9WgXcQ", Source: "youtube"}, "youtube:dQw4w9WgXcQ"},
		{model.Track{ID: "jsvn:abc123", Source: "jiosaavn"}, "jiosaavn:abc123"},
		{model.Track{ID: "12345", Source: "deezer"}, "deezer:12345"},
		{model.Track{ID: "67890", Source: "netease"}, "netease:67890"},
	}
	for _, c := range cases {
		if got := CacheKeyForTrack(c.track); got != c.want {
			t.Errorf("CacheKeyForTrack(%+v) = %q, want %q", c.track, got, c.want)
		}
	}
	qcases := []struct {
		query string
		want  string
		ok    bool
	}{
		{"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "youtube:dQw4w9WgXcQ", true},
		{"dQw4w9WgXcQ", "youtube:dQw4w9WgXcQ", true},
		{"jsvn:abc123", "jiosaavn:abc123", true},
		{"https://www.deezer.com/track/12345", "deezer:12345", true},
		{"netease:67890", "netease:67890", true},
		{"https://open.spotify.com/track/abc", "spotify:abc", true},
		{"some free text query", "", false},
		{"radio:lofi", "", false},
		{"soulseek:artist - title", "", false},
	}
	for _, c := range qcases {
		got, ok := CacheKeyForQuery(c.query)
		if ok != c.ok || got != c.want {
			t.Errorf("CacheKeyForQuery(%q) = %q,%v want %q,%v", c.query, got, ok, c.want, c.ok)
		}
	}
}

func TestParseStreamExpiry(t *testing.T) {
	now := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	fallback := 120 * time.Second

	// CDN signature wins over the fallback.
	oneHour := now.Add(time.Hour)
	expStr := strconv.FormatInt(oneHour.Unix(), 10)
	got := ParseStreamExpiry("https://cdn.example.com/a.mp3?expire="+expStr, fallback, now)
	if !got.Equal(oneHour) {
		t.Errorf("expire= not parsed: got %v", got)
	}
	// expires= and exp= aliases.
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3?expires="+expStr, fallback, now); !got.Equal(oneHour) {
		t.Errorf("expires= not parsed: got %v", got)
	}
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3?exp="+expStr, fallback, now); !got.Equal(oneHour) {
		t.Errorf("exp= not parsed: got %v", got)
	}
	// No signature: the fallback TTL applies.
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3", fallback, now); !got.Equal(now.Add(fallback)) {
		t.Errorf("no signature: got %v, want %v", got, now.Add(fallback))
	}
	// Already-expired signature: stale on arrival.
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3?expire="+strconv.FormatInt(now.Add(-time.Hour).Unix(), 10), fallback, now); !got.Equal(now) {
		t.Errorf("expired signature: got %v, want now", got)
	}
	// Absurd value (>24h out): ignored in favour of the fallback.
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3?expire="+strconv.FormatInt(now.Add(48*time.Hour).Unix(), 10), fallback, now); !got.Equal(now.Add(fallback)) {
		t.Errorf("absurd signature: got %v, want %v", got, now.Add(fallback))
	}
	// Zero fallback with no signature: do not cache.
	if got := ParseStreamExpiry("https://cdn.example.com/a.mp3", 0, now); !got.Equal(now) {
		t.Errorf("zero fallback: got %v, want now", got)
	}
}

func TestCachePutGetExpiry(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	clock := &fakeClock{now: time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)}
	c := NewCacheWithClock(st, 120*time.Second, clock.Now)

	track := model.Track{
		ID: "dQw4w9WgXcQ", Title: "Song", Source: "youtube",
		StreamURL: "https://cdn.example.com/a.mp3?expire=" + strconv.FormatInt(clock.now.Add(time.Hour).Unix(), 10), // now+1h
	}
	if !c.Put(track) {
		t.Fatal("Put of a cacheable track returned false")
	}
	if got, hit := c.Get("youtube:dQw4w9WgXcQ"); !hit || got.StreamURL != track.StreamURL {
		t.Fatalf("Get after Put: hit=%v track=%+v", hit, got)
	}
	// Still valid 59 minutes later.
	clock.Advance(59 * time.Minute)
	if _, hit := c.Get("youtube:dQw4w9WgXcQ"); !hit {
		t.Error("entry expired before its CDN expiry")
	}
	// Past the CDN expiry: never served, and dropped lazily.
	clock.Advance(2 * time.Minute)
	if got, hit := c.Get("youtube:dQw4w9WgXcQ"); hit {
		t.Errorf("expired entry served: %+v", got)
	}
	if _, found, _ := st.GetCacheEntry("youtube:dQw4w9WgXcQ"); found {
		t.Error("expired entry was not dropped on read")
	}

	// Fallback TTL when the URL carries no signature.
	plain := model.Track{ID: "plain1", Title: "Plain", Source: "youtube",
		StreamURL: "https://cdn.example.com/b.mp3"}
	if !c.Put(plain) {
		t.Fatal("Put without signature but with TTL returned false")
	}
	clock.Advance(119 * time.Second)
	if _, hit := c.Get("youtube:plain1"); !hit {
		t.Error("entry expired before its fallback TTL")
	}
	clock.Advance(2 * time.Second)
	if _, hit := c.Get("youtube:plain1"); hit {
		t.Error("entry served past its fallback TTL")
	}

	// Not cacheable: no stream URL, live, radio, already-expired.
	for _, bad := range []model.Track{
		{ID: "x", Title: "No URL", Source: "youtube"},
		{ID: "x", Title: "Live", Source: "youtube", StreamURL: "https://cdn.example.com/l.mp3", IsLive: true},
		{ID: "radio:lofi", Title: "Radio", Source: "radio", StreamURL: "https://cdn.example.com/r.mp3"},
		{ID: "x", Title: "Stale", Source: "youtube", StreamURL: "https://cdn.example.com/s.mp3?expire=1000000000"},
	} {
		if c.Put(bad) {
			t.Errorf("Put(%+v) stored a non-cacheable track", bad)
		}
	}
	// Zero TTL and no signature: not stored.
	zero := NewCacheWithClock(st, 0, clock.Now)
	if zero.Put(plain) {
		t.Error("Put with zero TTL and no signature stored the track")
	}
}

func TestRegistryResolveUsesCache(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	clock := &fakeClock{now: time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)}
	r := NewRegistry()
	r.SetCache(NewCacheWithClock(st, 120*time.Second, clock.Now))
	stub := &stubResolver{src: SourceYouTube, track: model.Track{
		ID: "dQw4w9WgXcQ", Title: "Song", Source: "youtube",
		StreamURL: "https://cdn.example.com/a.mp3?expire=" + strconv.FormatInt(clock.now.Add(time.Hour).Unix(), 10),
	}}
	r.resolvers[SourceYouTube] = stub

	ctx := context.Background()
	firstStart := time.Now()
	if _, err := r.Resolve(ctx, "dQw4w9WgXcQ"); err != nil {
		t.Fatalf("first Resolve: %v", err)
	}
	firstDur := time.Since(firstStart)
	if stub.calls != 1 {
		t.Fatalf("first Resolve: stub calls = %d, want 1", stub.calls)
	}

	// MEASURED (not asserted): the cached resolve. 100 iterations
	// so the mean is stable; the number is logged for the report.
	const n = 100
	cachedStart := time.Now()
	for i := 0; i < n; i++ {
		track, err := r.Resolve(ctx, "dQw4w9WgXcQ")
		if err != nil {
			t.Fatalf("cached Resolve %d: %v", i, err)
		}
		if track.StreamURL != stub.track.StreamURL {
			t.Fatalf("cached Resolve %d: wrong URL %q", i, track.StreamURL)
		}
	}
	cachedTotal := time.Since(cachedStart)
	t.Logf("MEASURED resolve: uncached=%v cached_mean=%v cached_total_100x=%v (stub calls=%d)",
		firstDur, cachedTotal/time.Duration(n), cachedTotal, stub.calls)
	if stub.calls != 1 {
		t.Errorf("cached resolves hit the resolver: stub calls = %d, want 1", stub.calls)
	}
}

func TestRegistryResolveReResolvesAfterExpiry(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	clock := &fakeClock{now: time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)}
	r := NewRegistry()
	r.SetCache(NewCacheWithClock(st, 120*time.Second, clock.Now))
	v1 := model.Track{ID: "dQw4w9WgXcQ", Title: "Song", Source: "youtube",
		StreamURL: "https://cdn.example.com/a.mp3?expire=" + strconv.FormatInt(clock.now.Add(60*time.Second).Unix(), 10)} // now+60s
	v2 := model.Track{ID: "dQw4w9WgXcQ", Title: "Song", Source: "youtube",
		StreamURL: "https://cdn.example.com/b.mp3?expire=" + strconv.FormatInt(clock.now.Add(2*time.Hour).Unix(), 10)}
	stub := &stubResolver{src: SourceYouTube, track: v1}
	r.resolvers[SourceYouTube] = stub

	ctx := context.Background()
	got, err := r.Resolve(ctx, "dQw4w9WgXcQ")
	if err != nil || got.StreamURL != v1.StreamURL {
		t.Fatalf("first Resolve: got %+v err %v", got, err)
	}
	// Inside the TTL the cached v1 is served, never re-resolved.
	if got, err := r.Resolve(ctx, "dQw4w9WgXcQ"); err != nil || got.StreamURL != v1.StreamURL || stub.calls != 1 {
		t.Fatalf("cached Resolve: got %+v calls=%d err %v", got, stub.calls, err)
	}
	// Past the CDN expiry the entry is re-resolved: the stale v1
	// URL is never served again.
	clock.Advance(61 * time.Second)
	stub.track = v2
	got, err = r.Resolve(ctx, "dQw4w9WgXcQ")
	if err != nil {
		t.Fatalf("post-expiry Resolve: %v", err)
	}
	if got.StreamURL != v2.StreamURL {
		t.Errorf("post-expiry Resolve served stale URL %q, want %q", got.StreamURL, v2.StreamURL)
	}
	if stub.calls != 2 {
		t.Errorf("post-expiry Resolve: stub calls = %d, want 2", stub.calls)
	}
}

func TestRegistryResolveTrackUsesCache(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	clock := &fakeClock{now: time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)}
	r := NewRegistry()
	r.SetCache(NewCacheWithClock(st, 120*time.Second, clock.Now))
	queued := model.Track{ID: "jsvn:abc123", Title: "Song", Source: "jiosaavn",
		URL: "https://www.jiosaavn.com/song/slug/abc123"}
	fresh := model.Track{ID: "jsvn:abc123", Title: "Song", Source: "jiosaavn",
		StreamURL: "https://cdn.example.com/j.mp3?expire=" + strconv.FormatInt(clock.now.Add(time.Hour).Unix(), 10)}
	stub := &stubResolver{src: SourceJioSaavn, track: fresh}
	r.resolvers[SourceJioSaavn] = stub

	ctx := context.Background()
	got, err := r.ResolveTrack(ctx, queued)
	if err != nil || got.StreamURL != fresh.StreamURL {
		t.Fatalf("ResolveTrack: got %+v err %v", got, err)
	}
	// Second call is a cache hit: the resolver is not consulted.
	if _, err := r.ResolveTrack(ctx, queued); err != nil {
		t.Fatalf("ResolveTrack (cached): %v", err)
	}
	if stub.calls != 1 {
		t.Errorf("ResolveTrack cache hit called the resolver: calls=%d", stub.calls)
	}
	// A track with no re-resolvable form is an honest error.
	if _, err := r.ResolveTrack(ctx, model.Track{ID: "x", Source: "soulseek"}); err == nil {
		t.Error("ResolveTrack(soulseek) must fail honestly")
	}
}

// TestRegistryConfigureQ10 proves the nine [backend] keys are
// wired: file values reach the resolvers, and a zero config leaves
// the env fallback in place.
func TestRegistryConfigureQ10(t *testing.T) {
	t.Setenv("DEEZER_ARL", "")
	t.Setenv("NAVIDROME_URL", "")
	t.Setenv("NAVIDROME_USER", "")
	t.Setenv("NAVIDROME_PASSWORD", "")
	t.Setenv("SLSKD_URL", "")

	r := NewRegistry()
	r.Configure(config.BackendConfig{
		SlskdURL:      "http://127.0.0.1:5999",
		DeezerARL:     "test-arl-value",
		NetEaseMusicU: "test-music-u",
		NavidromeURL:  "http://navidrome.test:4533",
		NavidromeUser: "navi-user",
		NavidromePass: "navi-pass",
	})
	if got := r.soulseek.slskdBaseURL(); got != "http://127.0.0.1:5999" {
		t.Errorf("slskd base URL = %q, want the configured value", got)
	}
	if got := r.deezer.arlValue(); got != "test-arl-value" {
		t.Errorf("deezer ARL = %q, want the configured value", got)
	}
	if !r.deezer.arlConfigured() {
		t.Error("deezer must report configured after Configure")
	}
	if mode := r.netease.AuthMode(); mode != "music_u" {
		t.Errorf("netease auth mode = %q, want music_u", mode)
	}
	base, user, pass, err := r.navidrome.endpoints()
	if err != nil {
		t.Fatalf("navidrome endpoints: %v", err)
	}
	if base != "http://navidrome.test:4533" || user != "navi-user" || pass != "navi-pass" {
		t.Errorf("navidrome endpoints = %q %q %q, want the configured values", base, user, pass)
	}

	// A zero config changes nothing: env fallback still applies.
	r2 := NewRegistry()
	r2.Configure(config.BackendConfig{})
	t.Setenv("DEEZER_ARL", "env-arl")
	if got := r2.deezer.arlValue(); got != "env-arl" {
		t.Errorf("zero config: deezer ARL = %q, want the env fallback", got)
	}
	if got := r2.soulseek.slskdBaseURL(); got != "http://127.0.0.1:5030" {
		t.Errorf("zero config: slskd base URL = %q, want the default", got)
	}
}

// TestFindYtDlpPythonQ11 proves the yt-dlp locator never returns a
// hardcoded user-home path: with no env override and no repo in
// sight it degrades to system python3, and the env override wins.
func TestFindYtDlpPythonQ11(t *testing.T) {
	t.Setenv("PARLAY_YTDLP_PYTHON", "")
	t.Setenv("PARLAY_YTDLP_WORKER", "")
	t.Chdir(t.TempDir())
	got, err := findYtDlpPython()
	if err != nil {
		t.Fatalf("findYtDlpPython: %v", err)
	}
	if strings.Contains(got, "/home/hatch") {
		t.Errorf("findYtDlpPython returned a hardcoded home path: %q", got)
	}
	if got != "python3" {
		t.Errorf("findYtDlpPython outside the repo = %q, want python3", got)
	}
	if _, err := findWorkerScript(); err == nil {
		t.Error("findWorkerScript outside the repo must fail honestly")
	}
	t.Setenv("PARLAY_YTDLP_PYTHON", "/opt/venv/bin/python")
	if got, _ := findYtDlpPython(); got != "/opt/venv/bin/python" {
		t.Errorf("PARLAY_YTDLP_PYTHON override = %q", got)
	}
}

// TestDataDirQ12 proves the resolver data dir is one root with the
// core's data dir: "data" by default, $PARLAY_DATA_DIR when set —
// never ~/.local/share/parlay.
func TestDataDirQ12(t *testing.T) {
	t.Setenv("PARLAY_DATA_DIR", "")
	if got := DataDir(); got != "data" {
		t.Errorf("DataDir() unset = %q, want \"data\"", got)
	}
	if got := DefaultDownloadDir(); got != "data/downloads" {
		t.Errorf("DefaultDownloadDir() unset = %q, want \"data/downloads\"", got)
	}
	t.Setenv("PARLAY_DATA_DIR", "/tmp/parlay-data")
	if got := DataDir(); got != "/tmp/parlay-data" {
		t.Errorf("DataDir() set = %q", got)
	}
}
