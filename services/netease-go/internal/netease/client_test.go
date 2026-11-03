package netease

// Offline tests for the NETEASE_MUSIC_U login path (B3, Phase 5 scope
// update). No real NetEase calls: clients are pointed at httptest
// servers and the captured Cookie headers are asserted. The cookie value
// used here is a dummy ("test-music-u"); no real credential exists in
// this repo.
import (
	"bytes"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

const testMusicU = "test-music-u-cookie-value"

func TestNewReadsMusicUEnv(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", "  "+testMusicU+"  ")
	c := New()
	if c.musicU != testMusicU {
		t.Fatalf("musicU = %q, want %q (trimmed)", c.musicU, testMusicU)
	}
	if !c.musicUConfigured() {
		t.Fatal("musicUConfigured must be true with the env set")
	}
}

func TestNewWithoutMusicUIsAnonymous(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", "")
	c := New()
	if c.musicU != "" || c.musicUConfigured() {
		t.Fatalf("musicU = %q, want anonymous", c.musicU)
	}
}

func TestEapiCookie(t *testing.T) {
	with := eapiCookie(testMusicU)
	if !strings.Contains(with, "MUSIC_U="+testMusicU) {
		t.Fatalf("cookie with login must carry MUSIC_U: %s", with)
	}
	if strings.Contains(with, "MUSIC_A=") {
		t.Fatalf("cookie with login must not carry anonymous MUSIC_A: %s", with)
	}
	if !strings.Contains(with, "deviceId=") || !strings.Contains(with, "appver=9.3.40") {
		t.Fatalf("cookie missing device fields: %s", with)
	}
	without := eapiCookie("")
	if !strings.Contains(without, "MUSIC_A="+defaultMusicA) {
		t.Fatalf("anonymous cookie must carry default MUSIC_A: %s", without)
	}
	if strings.Contains(without, "MUSIC_U=") {
		t.Fatalf("anonymous cookie must not carry MUSIC_U: %s", without)
	}
}

func TestIsNetEaseHost(t *testing.T) {
	for _, h := range []string{"music.163.com", "m1.music.126.net", "MUSIC.163.COM"} {
		if !isNetEaseHost(h) {
			t.Fatalf("%s must be a NetEase host", h)
		}
	}
	for _, h := range []string{"music-api.gdstudio.xyz", "example.com", "music.163.com.evil.com"} {
		if isNetEaseHost(h) {
			t.Fatalf("%s must NOT be a NetEase host", h)
		}
	}
}

// captureServer records the Cookie header of the first request.
func captureServer(t *testing.T) (*httptest.Server, *string) {
	t.Helper()
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.Header.Get("Cookie")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	return srv, &got
}

func TestSetAuthHeaders(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", testMusicU)
	c := New()
	// NetEase hosts get the cookie; UA/Referer are always set.
	for _, host := range []string{"music.163.com", "m1.music.126.net"} {
		req, err := http.NewRequest(http.MethodGet, "https://"+host+"/x", nil)
		if err != nil {
			t.Fatal(err)
		}
		c.setAuthHeaders(req)
		if got := req.Header.Get("Cookie"); got != "MUSIC_U="+testMusicU {
			t.Fatalf("%s: Cookie = %q, want MUSIC_U cookie", host, got)
		}
		if req.Header.Get("User-Agent") == "" || req.Header.Get("Referer") == "" {
			t.Fatalf("%s: UA/Referer missing", host)
		}
	}
	// Third parties never get the cookie (the mirror case).
	for _, host := range []string{"music-api.gdstudio.xyz", "example.com"} {
		req, err := http.NewRequest(http.MethodGet, "https://"+host+"/x", nil)
		if err != nil {
			t.Fatal(err)
		}
		c.setAuthHeaders(req)
		if got := req.Header.Get("Cookie"); got != "" {
			t.Fatalf("%s: Cookie leaked: %q", host, got)
		}
	}
	// Anonymous client sends no cookie anywhere.
	t.Setenv("NETEASE_MUSIC_U", "")
	c2 := New()
	req, _ := http.NewRequest(http.MethodGet, "https://music.163.com/x", nil)
	c2.setAuthHeaders(req)
	if got := req.Header.Get("Cookie"); got != "" {
		t.Fatalf("anonymous client sent Cookie: %q", got)
	}
}

func TestDoCookieScoping(t *testing.T) {
	// Direct do() test against a local server: non-NetEase host must
	// never receive the login cookie (the mirror case).
	t.Setenv("NETEASE_MUSIC_U", testMusicU)
	c := New()
	srv, got := captureServer(t)
	req, err := http.NewRequest(http.MethodGet, srv.URL+"/api.php", nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := c.do(req); err != nil {
		t.Fatalf("do() to test server: %v", err)
	}
	if *got != "" {
		t.Fatalf("cookie leaked to non-NetEase host: %q", *got)
	}
}

func TestEapiRequestSendsMusicU(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", testMusicU)
	c := New()
	srv, got := captureServer(t)
	if _, err := c.eapiRequest("/api/test", srv.URL, `{"ids":"[1]"}`); err != nil {
		t.Fatalf("eapiRequest: %v", err)
	}
	if !strings.Contains(*got, "MUSIC_U="+testMusicU) {
		t.Fatalf("eapi cookie must carry MUSIC_U: %q", *got)
	}
	if strings.Contains(*got, "MUSIC_A=") {
		t.Fatalf("eapi cookie with login must not carry MUSIC_A: %q", *got)
	}
}

func TestEapiRequestAnonymousWithoutMusicU(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", "")
	c := New()
	srv, got := captureServer(t)
	if _, err := c.eapiRequest("/api/test", srv.URL, `{"ids":"[1]"}`); err != nil {
		t.Fatalf("eapiRequest: %v", err)
	}
	if !strings.Contains(*got, "MUSIC_A="+defaultMusicA) {
		t.Fatalf("anonymous eapi cookie must carry MUSIC_A: %q", *got)
	}
}

func TestNewWithMusicU(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", "")
	c := NewWithMusicU("  " + testMusicU + "  ")
	if c.musicU != testMusicU || !c.musicUConfigured() {
		t.Fatalf("NewWithMusicU: musicU = %q, want trimmed %q", c.musicU, testMusicU)
	}
	if got := NewWithMusicU("").AuthMode(); got != "anonymous" {
		t.Fatalf("empty cookie: AuthMode = %q, want anonymous", got)
	}
	if got := NewWithMusicU(testMusicU).AuthMode(); got != "music_u" {
		t.Fatalf("with cookie: AuthMode = %q, want music_u", got)
	}
}

// eapiURLStub answers every POST with a playable URL at level standard.
func eapiURLStub(t *testing.T, cookie *string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		*cookie = r.Header.Get("Cookie")
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"code":200,"data":[{"id":1,"url":"https://m1.music.126.net/f.mp3","size":1000,"code":200,"fee":0,"md5":"","br":128000}]}`))
	}))
}

func TestSongURLWithLevelSendsMusicU(t *testing.T) {
	var gotCookie string
	srv := eapiURLStub(t, &gotCookie)
	defer srv.Close()
	c := NewWithMusicU(testMusicU)
	c.base = srv.URL
	u, size, level, err := c.SongURLWithLevel(1)
	if err != nil {
		t.Fatalf("SongURLWithLevel: %v", err)
	}
	if u != "https://m1.music.126.net/f.mp3" || size != 1000 || level != "eapi:standard" {
		t.Fatalf("got url=%q size=%d level=%q", u, size, level)
	}
	if !strings.Contains(gotCookie, "MUSIC_U="+testMusicU) {
		t.Fatalf("eapi song-URL cookie must carry MUSIC_U: %q", gotCookie)
	}
	if strings.Contains(gotCookie, "MUSIC_A=") {
		t.Fatalf("eapi song-URL cookie with login must not carry MUSIC_A: %q", gotCookie)
	}
}

func TestSongURLWithLevelAnonymous(t *testing.T) {
	var gotCookie string
	srv := eapiURLStub(t, &gotCookie)
	defer srv.Close()
	c := New()
	c.base = srv.URL
	u, _, level, err := c.SongURLWithLevel(1)
	if err != nil {
		t.Fatalf("SongURLWithLevel: %v", err)
	}
	if u == "" || level != "eapi:standard" {
		t.Fatalf("got url=%q level=%q", u, level)
	}
	if !strings.Contains(gotCookie, "MUSIC_A="+defaultMusicA) {
		t.Fatalf("anonymous eapi song-URL cookie must carry MUSIC_A: %q", gotCookie)
	}
	if strings.Contains(gotCookie, "MUSIC_U=") {
		t.Fatalf("anonymous eapi song-URL cookie must not carry MUSIC_U: %q", gotCookie)
	}
}

func TestDownloadSendsMusicUToNetEaseCDN(t *testing.T) {
	t.Setenv("NETEASE_MUSIC_U", testMusicU)
	c := New()
	var got string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got = r.Header.Get("Cookie")
		_, _ = w.Write([]byte("audio-bytes"))
	}))
	defer srv.Close()
	// Download() checks the host before sending the cookie: a
	// non-NetEase download host must never receive it. The positive
	// case (cookie attached for a NetEase CDN host) is covered by
	// TestSetAuthHeaders, which Download() calls.
	var buf bytes.Buffer
	if _, _, err := c.Download(srv.URL+"/f.mp3", &buf); err != nil {
		t.Fatalf("Download: %v", err)
	}
	if got != "" {
		t.Fatalf("cookie leaked to non-NetEase download host: %q", got)
	}
	if buf.String() != "audio-bytes" {
		t.Fatalf("download body = %q", buf.String())
	}
}

// ------------------------------------------------ Phase 9: IPC-3/5 --

func TestFormatDurationStr(t *testing.T) {
	// Same rendering as vc-go / vc-ts / vc-rust / downloaders.
	cases := []struct {
		sec  int
		live bool
		want string
	}{
		{0, false, "∞"}, {-5, false, "∞"}, {45, false, "0:45"},
		{268, false, "4:28"}, {3599, false, "59:59"}, {3600, false, "1h"},
		{3661, false, "1h 1m 1s"}, {3720, false, "1h 2m"},
		{3605, false, "1h 5s"}, {213, true, "∞"},
	}
	for _, c := range cases {
		if got := formatDurationStr(c.sec, c.live); got != c.want {
			t.Fatalf("formatDurationStr(%d, %v) = %q, want %q", c.sec, c.live, got, c.want)
		}
	}
}

func TestToTrackCarriesIPC3(t *testing.T) {
	song := apiSong{ID: 186016, Name: "Test Song", Duration: 213000,
		Artists: []struct {
			Name string `json:"name"`
		}{{Name: "Test Artist"}}}
	song.Album.PicURL = "https://p1.music.126.net/cover.jpg"
	tr := song.toTrack()
	if tr.DurationStr != "3:33" {
		t.Fatalf("DurationStr = %q, want 3:33", tr.DurationStr)
	}
	if tr.IsLive {
		t.Fatal("IsLive must be false for a NetEase song")
	}
	if tr.Thumbnail != "https://p1.music.126.net/cover.jpg" {
		t.Fatalf("Thumbnail = %q", tr.Thumbnail)
	}
	// Unknown duration renders ∞, never a fake value.
	zero := apiSong{ID: 1, Name: "No Duration", Duration: 0}
	if zero.toTrack().DurationStr != "∞" {
		t.Fatalf("zero-duration DurationStr = %q, want ∞", zero.toTrack().DurationStr)
	}
}

func TestLyricsNoLyricsIsSentinel(t *testing.T) {
	stub := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"code":200,"lrc":{"lyric":""}}`))
	}))
	defer stub.Close()
	c := NewWithBase(stub.URL)
	if _, err := c.Lyrics(186016); !errors.Is(err, ErrNoLyrics) {
		t.Fatalf("empty lyric must be ErrNoLyrics, got %v", err)
	}
}

func TestLyricsReturnsLRC(t *testing.T) {
	stub := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"code":200,"lrc":{"lyric":"[00:01.00]hello\n[00:05.00]world"}}`))
	}))
	defer stub.Close()
	c := NewWithBase(stub.URL)
	got, err := c.Lyrics(186016)
	if err != nil {
		t.Fatalf("Lyrics: %v", err)
	}
	if got != "[00:01.00]hello\n[00:05.00]world" {
		t.Fatalf("Lyrics = %q", got)
	}
}
