package resolver

import (
	"context"
	"encoding/hex"
	"os"
	"strings"
	"testing"
	"time"
)

func TestParseDuration(t *testing.T) {
	cases := []struct {
		in   any
		want int
	}{
		{"4:28", 268}, // JioSaavn clock string — the old Number("4:28") bug gave 0
		{"1:02:03", 3723},
		{"0:00", 0},
		{"268", 268},
		{268, 268},
		{268.0, 268},
		{int64(213), 213},
		{"", 0},
		{nil, 0},
		{"abc", 0},
		{"4:28:99:11", 0},
		{"-5", 0},
		{"3:42", 222},
	}
	for _, c := range cases {
		if got := ParseDuration(c.in); got != c.want {
			t.Errorf("ParseDuration(%v) = %d, want %d", c.in, got, c.want)
		}
	}
}

func TestFormatDurationStr(t *testing.T) {
	cases := []struct {
		sec  int
		live bool
		want string
	}{
		{268, false, "4:28"},
		{0, false, "∞"},
		{0, true, "∞"},
		{61, false, "1:01"},
		{3723, false, "1h 2m 3s"},
		{3600, false, "1h"},
		{3660, false, "1h 1m"},
	}
	for _, c := range cases {
		if got := FormatDurationStr(c.sec, c.live); got != c.want {
			t.Errorf("FormatDurationStr(%d, %v) = %q, want %q", c.sec, c.live, got, c.want)
		}
	}
}

func TestExtractYouTubeID(t *testing.T) {
	cases := map[string]string{
		"https://www.youtube.com/watch?v=dQw4w9WgXcQ":              "dQw4w9WgXcQ",
		"https://youtu.be/dQw4w9WgXcQ":                             "dQw4w9WgXcQ",
		"https://www.youtube.com/shorts/dQw4w9WgXcQ":               "dQw4w9WgXcQ",
		"https://www.youtube.com/embed/dQw4w9WgXcQ":                "dQw4w9WgXcQ",
		"https://music.youtube.com/watch?v=dQw4w9WgXcQ&list=PL123": "dQw4w9WgXcQ",
		"dQw4w9WgXcQ":             "dQw4w9WgXcQ",
		"never gonna give you up": "",
		"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC": "",
	}
	for in, want := range cases {
		if got := ExtractYouTubeID(in); got != want {
			t.Errorf("ExtractYouTubeID(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestExtractYouTubePlaylistID(t *testing.T) {
	if got := ExtractYouTubePlaylistID("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PL123abc"); got != "PL123abc" {
		t.Errorf("playlist id = %q", got)
	}
	if got := ExtractYouTubePlaylistID("https://www.youtube.com/watch?v=dQw4w9WgXcQ"); got != "" {
		t.Errorf("playlist id = %q, want empty", got)
	}
}

func TestClassifyQuery(t *testing.T) {
	cases := map[string]Source{
		"https://www.youtube.com/watch?v=dQw4w9WgXcQ": SourceYouTube,
		"dQw4w9WgXcQ":             SourceYouTube,
		"never gonna give you up": SourceYouTube,
		"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC":  SourceSpotify,
		"spotify:track:4uLU6hMCjMI75M1A2tKUQC":                   SourceSpotify,
		"jsvn:rjkrTnma":                                          SourceJioSaavn,
		"jiosaavn:Kesariya":                                      SourceJioSaavn,
		"https://www.deezer.com/track/123":                       SourceDeezer,
		"https://music.163.com/song?id=123":                      SourceNetEase,
		"https://www.tiktok.com/@user/video/1234567890123456789": SourceTikTok,
		"radio:groovesalad":                                      SourceRadio,
		"soulseek:artist - title":                                SourceSoulseek,
		"navidrome:42":                                           SourceNavidrome,
	}
	for in, want := range cases {
		if got := ClassifyQuery(in); got != want {
			t.Errorf("ClassifyQuery(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestJioSaavnQualitySelection(t *testing.T) {
	// The locked correction: 320_KBPS first, never 96 while higher exists.
	full := jioSaavnRaw{MediaURLs: map[string]string{
		"96_KBPS":  "https://aac.saavncdn.com/x_96.mp4",
		"160_KBPS": "https://aac.saavncdn.com/x_160.mp4",
		"320_KBPS": "https://aac.saavncdn.com/x_320.mp4",
	}, MediaURL: "https://aac.saavncdn.com/x_160.mp4"}
	if got := selectJioSaavnStreamURL(full); !strings.Contains(got, "_320") {
		t.Errorf("full payload selected %q, want 320_KBPS", got)
	}
	no320 := jioSaavnRaw{MediaURLs: map[string]string{
		"96_KBPS":  "https://aac.saavncdn.com/x_96.mp4",
		"160_KBPS": "https://aac.saavncdn.com/x_160.mp4",
	}}
	if got := selectJioSaavnStreamURL(no320); !strings.Contains(got, "_160") {
		t.Errorf("no-320 payload selected %q, want 160_KBPS", got)
	}
	onlyMedia := jioSaavnRaw{MediaURL: "https://aac.saavncdn.com/x_160.mp4"}
	if got := selectJioSaavnStreamURL(onlyMedia); got != "https://aac.saavncdn.com/x_160.mp4" {
		t.Errorf("media_url-only payload selected %q", got)
	}
	// Duration from the "m:ss" clock string.
	tr := jioSaavnTrack(jioSaavnRaw{ID: "rjkrTnma", Song: "Kesariya", Duration: "4:28", Singers: "Arijit Singh"})
	if tr.DurationSec != 268 {
		t.Errorf("JioSaavn duration = %d, want 268", tr.DurationSec)
	}
	if tr.DurationStr != "4:28" {
		t.Errorf("JioSaavn duration_str = %q, want 4:28", tr.DurationStr)
	}
}

func TestDeezerBlowfishKeyMatchesPython(t *testing.T) {
	// Cross-check vectors computed from services/downloaders/src/deezer.py
	// _blowfish_key (deemix generateBlowfishKey) in the sandbox venv.
	cases := map[string]string{
		"123":       "6c32636d67367064637d716c3d3b6363",
		"123456789": "6d34656061377f31322a7336393f626b",
	}
	for id, wantHex := range cases {
		got := hex.EncodeToString(deezerBlowfishKey(id))
		if got != wantHex {
			t.Errorf("deezerBlowfishKey(%q) = %s, want %s", id, got, wantHex)
		}
	}
}

func TestDeezerBlowfishRoundTrip(t *testing.T) {
	key := deezerBlowfishKey("123")
	plain := []byte("0123456789abcdef0123456789abcdef")
	enc, err := deezerDecryptChunk(key, plain) // decrypt of arbitrary bytes still runs
	if err != nil {
		t.Fatalf("decrypt failed: %v", err)
	}
	// CBC decrypt is its own inverse only with the matching encrypt;
	// assert determinism + length instead of a fake round-trip.
	enc2, err := deezerDecryptChunk(key, plain)
	if err != nil {
		t.Fatalf("decrypt failed: %v", err)
	}
	if hex.EncodeToString(enc) != hex.EncodeToString(enc2) {
		t.Errorf("decrypt is not deterministic")
	}
	if len(enc) != len(plain) {
		t.Errorf("decrypt length %d, want %d", len(enc), len(plain))
	}
	if _, err := deezerDecryptChunk(key, []byte("short")); err == nil {
		t.Errorf("decrypt of a non-multiple-of-8 chunk should fail")
	}
}

func TestDeezerLegacyStreamURLShape(t *testing.T) {
	u := deezerLegacyStreamURL("123", "abcdef0123456789abcdef0123456789", "1", 9)
	if !strings.HasPrefix(u, "https://e-cdns-proxy-a.dzcdn.net/mobile/1/") {
		t.Errorf("legacy URL = %q, want e-cdns-proxy-a prefix", u)
	}
	hexPart := strings.TrimPrefix(u, "https://e-cdns-proxy-a.dzcdn.net/mobile/1/")
	if len(hexPart)%32 != 0 {
		t.Errorf("legacy URL hex part length %d is not a multiple of 32 (AES blocks)", len(hexPart))
	}
}

func TestDeezerFormatLadder(t *testing.T) {
	data := map[string]any{"FILESIZE_FLAC": float64(0), "FILESIZE_MP3_320": float64(12345), "FILESIZE_MP3_128": float64(1111)}
	format, num := pickDeezerFormat(data)
	if format != "MP3_320" || num != 3 {
		t.Errorf("pickDeezerFormat = %q/%d, want MP3_320/3", format, num)
	}
	data["FILESIZE_FLAC"] = float64(99999)
	format, num = pickDeezerFormat(data)
	if format != "FLAC" || num != 9 {
		t.Errorf("pickDeezerFormat = %q/%d, want FLAC/9", format, num)
	}
	data = map[string]any{}
	if format, _ := pickDeezerFormat(data); format != "" {
		t.Errorf("pickDeezerFormat with no sizes = %q, want empty", format)
	}
}

func TestSpotifyMissingCredsNamesVars(t *testing.T) {
	t.Setenv("SPOTIFY_CLIENT_ID", "")
	t.Setenv("SPOTIFY_CLIENT_SECRET", "")
	res := NewSpotifyResolver(nil)
	_, err := res.Resolve(context.Background(), "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC")
	if err == nil {
		t.Fatalf("expected an error without Spotify creds")
	}
	if !strings.Contains(err.Error(), "SPOTIFY_CLIENT_ID") || !strings.Contains(err.Error(), "SPOTIFY_CLIENT_SECRET") {
		t.Errorf("error %q does not name both credential vars", err)
	}
	if !IsMissingCredential(err) && !strings.Contains(err.Error(), "SPOTIFY_CLIENT_ID") {
		t.Errorf("error should be (or name) the missing-credential failure: %v", err)
	}
}

func TestNavidromeMissingEnvNamesVars(t *testing.T) {
	t.Setenv("NAVIDROME_URL", "")
	t.Setenv("NAVIDROME_USER", "")
	t.Setenv("NAVIDROME_PASSWORD", "")
	res := NewNavidromeResolver()
	_, err := res.Search(context.Background(), "test", 5)
	if err == nil {
		t.Fatalf("expected an error without Navidrome config")
	}
	if !IsMissingCredential(err) {
		t.Errorf("expected ErrMissingCredential, got %T: %v", err, err)
	}
	for _, v := range []string{"NAVIDROME_URL", "NAVIDROME_USER", "NAVIDROME_PASSWORD"} {
		if !strings.Contains(err.Error(), v) {
			t.Errorf("error %q does not name %s", err, v)
		}
	}
}

func TestSoulseekMissingDaemonHonest(t *testing.T) {
	t.Setenv("SLSKD_URL", "http://127.0.0.1:59999")
	res := NewSoulseekResolver()
	_, err := res.Search(context.Background(), "test query", 5)
	if err == nil {
		t.Fatalf("expected an error with slskd down")
	}
	if !strings.Contains(err.Error(), "slskd unreachable") {
		t.Errorf("error %q should say slskd is unreachable", err)
	}
}

func TestSoulseekScorer(t *testing.T) {
	// Perfect copy: exact duration, name in path, free slot, FLAC.
	good := slskdFile{Username: "peer1", Filename: `C:\Music\Artist - Title.flac`, Size: 1000,
		LengthSeconds: 200, HasFreeSlot: true, UploadSpeed: 5_000_000, QueueLength: 0}
	// Wrong version: same duration but a live take.
	live := slskdFile{Username: "peer2", Filename: `C:\Music\Artist - Title (live).flac`, Size: 1000,
		LengthSeconds: 200, HasFreeSlot: true, UploadSpeed: 5_000_000, QueueLength: 0}
	// Wrong duration: 60 s off.
	wrongDur := slskdFile{Username: "peer3", Filename: `C:\Music\Artist - Title.flac`, Size: 1000,
		LengthSeconds: 260, HasFreeSlot: true, UploadSpeed: 5_000_000, QueueLength: 0}
	results := scoreTrackResults([]slskdFile{live, wrongDur, good}, "Artist", "Title", 200)
	if len(results) == 0 {
		t.Fatalf("scorer dropped every candidate")
	}
	if results[0].Result.Username != "peer1" {
		t.Errorf("best candidate is %q, want peer1 (exact match)", results[0].Result.Username)
	}
	if results[0].MatchScore < 45 {
		t.Errorf("exact-match score %v, want >= 45", results[0].MatchScore)
	}
	// Duration scoring: exact 200 -> 40, 5 s off -> 30, 30 s off -> 0, 31 s off -> excluded.
	if got := durationScore(0, 5, 30); got != 40 {
		t.Errorf("durationScore(0) = %v, want 40", got)
	}
	if got := durationScore(5, 5, 30); got != 30 {
		t.Errorf("durationScore(5) = %v, want 30", got)
	}
	if got := durationScore(31, 5, 30); got != -1 {
		t.Errorf("durationScore(31) = %v, want -1 (excluded)", got)
	}
	// Version penalty: live take of a non-live title is 0.30.
	if got := versionPenalty("artist - title (live).flac", "title"); got != 0.30 {
		t.Errorf("versionPenalty(live) = %v, want 0.30", got)
	}
	if got := versionPenalty("artist - title.flac", "title"); got != 1.0 {
		t.Errorf("versionPenalty(clean) = %v, want 1.0", got)
	}
	// Reliability: free slot + fast + empty queue beats a queued peer.
	fast := reliabilityScore(true, 5_000_000, 0)
	slow := reliabilityScore(false, 100_000, 20)
	if fast <= slow {
		t.Errorf("reliability fast=%v slow=%v, fast should win", fast, slow)
	}
}

func TestRadioStations(t *testing.T) {
	if len(Stations) != 6 {
		t.Fatalf("station count = %d, want 6", len(Stations))
	}
	seen := map[string]bool{}
	for _, st := range Stations {
		if seen[st.ID] {
			t.Errorf("duplicate station id %q", st.ID)
		}
		seen[st.ID] = true
		if st.StreamURL == "" {
			t.Errorf("station %q has no stream URL", st.ID)
		}
	}
	if got := FindStation("groovesalad"); got == nil || got.Name != "SomaFM Groove Salad" {
		t.Errorf("FindStation(groovesalad) = %+v", got)
	}
	if got := FindStation("Lush"); got == nil || got.ID != "lush" {
		t.Errorf("FindStation(Lush) = %+v", got)
	}
	if got := FindStation("no-such-station"); got != nil {
		t.Errorf("FindStation(unknown) = %+v, want nil", got)
	}
	tr := RadioTrackFromURL("https://ice1.somafm.com/groovesalad-128-mp3")
	if !tr.IsLive || tr.DurationSec != 0 || tr.StreamURL == "" {
		t.Errorf("radio track = %+v, want live/0/stream", tr)
	}
	if tr.DurationStr != "∞" {
		t.Errorf("radio duration_str = %q, want ∞", tr.DurationStr)
	}
	res := NewRadioResolver()
	track, err := res.Resolve(context.Background(), "radio:groovesalad")
	if err != nil {
		t.Fatalf("radio resolve failed: %v", err)
	}
	if track.Title != "SomaFM Groove Salad" {
		t.Errorf("radio resolve title = %q", track.Title)
	}
}

func TestSanitizeFilename(t *testing.T) {
	if got := SanitizeFilename("A/B:C*D?E"); got != "A_B_C_D_E" {
		t.Errorf("sanitize = %q", got)
	}
	if got := SanitizeFilename(""); got != "track" {
		t.Errorf("sanitize empty = %q", got)
	}
}

func TestRegistryWiring(t *testing.T) {
	r := NewRegistry()
	defer r.Close()
	for _, src := range []Source{SourceYouTube, SourceJioSaavn, SourceDeezer, SourceNetEase, SourceSpotify, SourceTikTok, SourceNavidrome, SourceSoulseek, SourceRadio} {
		if _, err := r.ResolverFor(src); err != nil {
			t.Errorf("no resolver for %q: %v", src, err)
		}
	}
	if _, err := r.ResolverFor("nope"); err == nil {
		t.Errorf("expected an error for an unknown source")
	}
}

func TestInnerTubeKeyPresent(t *testing.T) {
	if len(innerTubeKey) != 39 || !strings.HasPrefix(innerTubeKey, "AIza") {
		t.Errorf("innerTubeKey has unexpected shape (len %d)", len(innerTubeKey))
	}
	if len(deezerBlowfishSecret) != 16 {
		t.Errorf("deezer blowfish secret len %d, want 16", len(deezerBlowfishSecret))
	}
}

func TestYtDlpPickAudioURL(t *testing.T) {
	formats := []any{
		map[string]any{"itag": float64(251), "url": "https://example/opus", "acodec": "opus", "vcodec": "none"},
		map[string]any{"itag": float64(140), "url": "https://example/aac", "acodec": "mp4a", "vcodec": "none"},
	}
	if got := pickYtDlpAudioURL(formats); got != "https://example/opus" {
		t.Errorf("pickYtDlpAudioURL = %q, want opus (251)", got)
	}
	formats = formats[1:]
	if got := pickYtDlpAudioURL(formats); got != "https://example/aac" {
		t.Errorf("pickYtDlpAudioURL = %q, want aac (140)", got)
	}
}

func TestYouTubePlayerFormatPick(t *testing.T) {
	result := map[string]any{"streamingData": map[string]any{"adaptiveFormats": []any{
		map[string]any{"itag": float64(140), "url": "https://example/140"},
		map[string]any{"itag": float64(251), "url": "https://example/251"},
		map[string]any{"itag": float64(249), "signatureCipher": "s=abc"},
	}}}
	if got := pickAdaptiveAudioURL(result); got != "https://example/251" {
		t.Errorf("pickAdaptiveAudioURL = %q, want itag 251", got)
	}
	// Ciphered-only formats are skipped, not returned.
	result2 := map[string]any{"streamingData": map[string]any{"adaptiveFormats": []any{
		map[string]any{"itag": float64(251), "signatureCipher": "s=abc"},
	}}}
	if got := pickAdaptiveAudioURL(result2); got != "" {
		t.Errorf("pickAdaptiveAudioURL(ciphered-only) = %q, want empty", got)
	}
}

func TestDownloadDirDefault(t *testing.T) {
	t.Setenv("PARLAY_DATA_DIR", "/tmp/parlay-test-data")
	if got := DefaultDownloadDir(); got != "/tmp/parlay-test-data/downloads" {
		t.Errorf("DefaultDownloadDir = %q", got)
	}
}

var _ = os.Getenv
var _ = time.Now
