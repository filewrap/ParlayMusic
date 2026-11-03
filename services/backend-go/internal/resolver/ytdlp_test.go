package resolver

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"
)

// requireLive skips the test unless PARLAY_LIVE_TEST=1.
func requireLive(t *testing.T) {
	t.Helper()
	if os.Getenv("PARLAY_LIVE_TEST") != "1" {
		t.Skip("live test: set PARLAY_LIVE_TEST=1 to run")
	}
}

func TestYtDlpDaemonPingVersion(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	client := NewYtDlpClient()
	defer client.Close()
	if err := client.Ping(ctx); err != nil {
		t.Fatalf("daemon ping failed: %v", err)
	}
	ver, err := client.Version(ctx)
	if err != nil {
		t.Fatalf("daemon version failed: %v", err)
	}
	if ver == "" {
		t.Errorf("daemon version is empty")
	}
	t.Logf("yt-dlp daemon version: %s", ver)
	// Second ping proves the daemon is persistent (same process).
	if err := client.Ping(ctx); err != nil {
		t.Fatalf("second daemon ping failed: %v", err)
	}
}

// TestYtDlpDaemonSurvivesCallerContextExpiry is the regression
// test for the Wave 2 verification finding: the daemon used to be
// spawned with exec.CommandContext(ctx, ...), so the boot
// warm-up's 30 s context killed the "persistent" daemon 30 s after
// startup and the next Extract failed with "write failed: broken
// pipe". The daemon must outlive the context that started it.
func TestYtDlpDaemonSurvivesCallerContextExpiry(t *testing.T) {
	client := NewYtDlpClient()
	defer client.Close()
	// Start the daemon with a context that expires in 3 s (the
	// boot warm-up shape, compressed — 3 s, not 1 s, because the
	// python + yt_dlp import alone takes ~0.7 s and a 1 s budget
	// flakes under load).
	short, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := client.Ping(short); err != nil {
		t.Fatalf("daemon ping failed: %v", err)
	}
	pid := client.cmd.Process.Pid
	time.Sleep(4 * time.Second) // past the starter context's expiry
	ctx, cancel2 := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel2()
	if err := client.Ping(ctx); err != nil {
		t.Fatalf("daemon ping after starter context expired failed: %v (daemon died with its starter context)", err)
	}
	if client.cmd == nil || client.cmd.Process == nil || client.cmd.Process.Pid != pid {
		t.Fatalf("daemon process changed across pings (was pid %d)", pid)
	}
}

func TestYtDlpDaemonBadURLFailsHonestly(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	client := NewYtDlpClient()
	defer client.Close()
	_, err := client.Extract(ctx, "not a url at all")
	if err == nil {
		t.Fatalf("expected an error extracting a garbage URL")
	}
	t.Logf("garbage URL extract error (honest): %v", err)
}

// --- live tests (PARLAY_LIVE_TEST=1) -----------------------------------

func TestLiveYouTubePlayer(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewYouTubeResolver(NewYtDlpClient())
	defer res.ytdlp.Close()
	start := time.Now()
	track, err := res.Resolve(ctx, "https://www.youtube.com/watch?v=dQw4w9WgXcQ")
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("YouTube player resolve failed: %v", err)
	}
	t.Logf("YouTube player resolve: %s in %s", track.Title, elapsed)
	if track.ID != "dQw4w9WgXcQ" {
		t.Errorf("track id = %q, want dQw4w9WgXcQ", track.ID)
	}
	if track.DurationSec != 213 {
		t.Errorf("duration = %d, want 213", track.DurationSec)
	}
	if !strings.Contains(track.Title, "Rick Astley") {
		t.Errorf("title = %q, want Rick Astley", track.Title)
	}
	if track.StreamURL == "" {
		t.Errorf("no stream URL resolved")
	} else if !strings.Contains(track.StreamURL, "googlevideo.com") {
		t.Errorf("stream URL = %q, want a googlevideo.com URL", track.StreamURL)
	}
}

func TestLiveYouTubeSearch(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewYouTubeResolver(nil)
	start := time.Now()
	tracks, err := res.Search(ctx, "never gonna give you up rick astley", 3)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("YouTube search failed: %v", err)
	}
	t.Logf("YouTube search: %d results in %s", len(tracks), elapsed)
	if len(tracks) == 0 {
		t.Fatalf("no search results")
	}
	if tracks[0].ID != "dQw4w9WgXcQ" {
		t.Errorf("top hit id = %q, want dQw4w9WgXcQ", tracks[0].ID)
	}
}

func TestLiveJioSaavnSong(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewJioSaavnResolver()
	start := time.Now()
	track, err := res.Resolve(ctx, "jsvn:rjkrTnma")
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("JioSaavn song resolve failed: %v", err)
	}
	t.Logf("JioSaavn /song resolve: %q in %s (stream %s)", track.Title, elapsed, track.StreamURL)
	if track.DurationSec != 268 {
		t.Errorf("duration = %d, want 268", track.DurationSec)
	}
	if !strings.Contains(track.StreamURL, "_320") {
		t.Errorf("stream URL = %q, want the 320_KBPS variant", track.StreamURL)
	}
}

func TestLiveJioSaavnSearch(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewJioSaavnResolver()
	start := time.Now()
	tracks, err := res.Search(ctx, "Kesariya", 3)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("JioSaavn search failed: %v", err)
	}
	t.Logf("JioSaavn search: %d results in %s", len(tracks), elapsed)
	if len(tracks) == 0 {
		t.Fatalf("no search results")
	}
	for _, tr := range tracks {
		if tr.DurationSec <= 0 {
			t.Errorf("track %q has no duration after enrichment", tr.Title)
		}
	}
}

func TestLiveDeezerPublicSearch(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewDeezerResolver()
	tracks, err := res.Search(ctx, "Daft Punk Get Lucky", 3)
	if err != nil {
		t.Fatalf("Deezer public search failed: %v", err)
	}
	if len(tracks) == 0 {
		t.Fatalf("no Deezer search results")
	}
	t.Logf("Deezer search top hit: %q by %q (preview %s)", tracks[0].Title, tracks[0].Artist, tracks[0].StreamURL)
}

func TestLiveNetEaseSearch(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	res := NewNetEaseResolver()
	tracks, err := res.Search(ctx, "Jay Chou", 3)
	if err != nil {
		t.Fatalf("NetEase search failed: %v", err)
	}
	if len(tracks) == 0 {
		t.Fatalf("no NetEase search results")
	}
	t.Logf("NetEase search top hit: %q by %q (auth %s)", tracks[0].Title, tracks[0].Artist, res.AuthMode())
}

func TestLiveLyricsLRCLib(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	lr := NewLyricsResolver(NewNetEaseResolver())
	res, err := lr.Lyrics(ctx, "Never Gonna Give You Up", "Rick Astley", "")
	if err != nil {
		t.Fatalf("LRCLib lyrics failed: %v", err)
	}
	if res.Lyrics == "" || res.Source != "lrclib" {
		t.Errorf("lyrics result = %+v", res)
	}
	t.Logf("LRCLib lyrics: %d chars, synced=%v", len(res.Lyrics), res.Synced)
}

func TestLiveRadioProbe(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	probe := ProbeStream(ctx, "https://ice1.somafm.com/groovesalad-128-mp3")
	t.Logf("radio probe groovesalad: %+v", probe)
	if !probe.Reachable {
		t.Errorf("SomaFM Groove Salad probe says unreachable: %s", probe.Probe)
	}
}

func TestLiveDaemonExtractYouTube(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	client := NewYtDlpClient()
	defer client.Close()
	start := time.Now()
	info, err := client.Extract(ctx, "https://www.youtube.com/watch?v=dQw4w9WgXcQ")
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("daemon extract failed: %v", err)
	}
	t.Logf("daemon extract: id=%v title=%v in %s", info["id"], info["title"], elapsed)
	if info["id"] != "dQw4w9WgXcQ" {
		t.Errorf("extract id = %v", info["id"])
	}
}

func TestLiveDaemonDownloadYouTube(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Second)
	defer cancel()
	dir := t.TempDir()
	client := NewYtDlpClient()
	defer client.Close()
	start := time.Now()
	path, err := client.Download(ctx, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", dir)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("daemon download failed: %v", err)
	}
	st, err := os.Stat(path)
	if err != nil {
		t.Fatalf("downloaded file not stat-able: %v", err)
	}
	t.Logf("daemon download: %s (%d bytes) in %s", path, st.Size(), elapsed)
	if st.Size() < 1024 {
		t.Errorf("downloaded file is suspiciously small: %d bytes", st.Size())
	}
}

func TestLiveHonestFailures(t *testing.T) {
	requireLive(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	// Spotify without creds.
	sp := NewSpotifyResolver(nil)
	_, err := sp.Resolve(ctx, "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC")
	if err == nil || !strings.Contains(err.Error(), "SPOTIFY_CLIENT_ID") {
		t.Errorf("Spotify without creds: err = %v", err)
	}
	// Navidrome without config.
	nv := NewNavidromeResolver()
	_, err = nv.Search(ctx, "test", 3)
	if err == nil || !IsMissingCredential(err) {
		t.Errorf("Navidrome without config: err = %v", err)
	}
	// Soulseek without slskd.
	sl := NewSoulseekResolver()
	_, err = sl.Search(ctx, "test", 3)
	if err == nil || !strings.Contains(err.Error(), "slskd unreachable") {
		t.Errorf("Soulseek without slskd: err = %v", err)
	}
	// Deezer full-track URL without ARL.
	dz := NewDeezerResolver()
	_, _, err = dz.fullTrackURL(ctx, "123")
	if err == nil || !strings.Contains(err.Error(), "DEEZER_ARL") {
		t.Errorf("Deezer full-track without ARL: err = %v", err)
	}
}
