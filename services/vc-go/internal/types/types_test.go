package types

import (
	"encoding/json"
	"strings"
	"testing"
)

// StreamURL must be serialized as `stream_url` in IPC payloads
// (Track / NowPlaying), so the core can fetch vc-go's resolved direct
// audio URL from /v1/now-playing and queue responses (A8). Empty stays
// omitted; Prefetched stays internal.
func TestTrackStreamURLSerialized(t *testing.T) {
	tr := Track{
		ID: "yt:dQw4w9WgXcQ", Title: "Never Gonna Give You Up",
		Source: "youtube", URL: "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
		StreamURL: "https://example.invalid/audio.webm",
	}
	b, err := json.Marshal(tr)
	if err != nil {
		t.Fatalf("marshal Track: %v", err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("unmarshal Track JSON: %v", err)
	}
	if m["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("expected stream_url in %s", b)
	}
}

func TestTrackStreamURLOmittedWhenEmpty(t *testing.T) {
	tr := Track{ID: "yt:abc", Title: "A", Source: "youtube"}
	b, err := json.Marshal(tr)
	if err != nil {
		t.Fatalf("marshal Track: %v", err)
	}
	if strings.Contains(string(b), "stream_url") {
		t.Fatalf("empty StreamURL must be omitted, got %s", b)
	}
}

func TestNowPlayingCarriesStreamURL(t *testing.T) {
	tr := Track{
		ID: "yt:abc", Title: "A", Source: "youtube",
		StreamURL: "https://example.invalid/audio.webm",
	}
	np := NowPlaying{ChatID: 42, Track: &tr, Engine: "go", Queue: []QueueItem{},
		StreamURL: tr.StreamURL}
	b, err := json.Marshal(np)
	if err != nil {
		t.Fatalf("marshal NowPlaying: %v", err)
	}
	if !strings.Contains(string(b), `"stream_url":"https://example.invalid/audio.webm"`) {
		t.Fatalf("NowPlaying must carry the track stream_url, got %s", b)
	}
	// The URL is emitted at BOTH levels: nested under track AND
	// top-level (contract NowPlaying.stream_url).
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("unmarshal NowPlaying JSON: %v", err)
	}
	if m["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("top-level stream_url missing in %s", b)
	}
	track, _ := m["track"].(map[string]any)
	if track["stream_url"] != "https://example.invalid/audio.webm" {
		t.Fatalf("nested track.stream_url missing in %s", b)
	}
}

func TestNowPlayingStreamURLOmittedWhenEmpty(t *testing.T) {
	tr := Track{ID: "yt:abc", Title: "A", Source: "youtube"}
	np := NowPlaying{ChatID: 42, Track: &tr, Engine: "go", Queue: []QueueItem{}}
	b, err := json.Marshal(np)
	if err != nil {
		t.Fatalf("marshal NowPlaying: %v", err)
	}
	if strings.Contains(string(b), "stream_url") {
		t.Fatalf("empty StreamURL must be omitted at both levels, got %s", b)
	}
	npIdle := NowPlaying{ChatID: 42, Engine: "go", Queue: []QueueItem{}}
	b, err = json.Marshal(npIdle)
	if err != nil {
		t.Fatalf("marshal idle NowPlaying: %v", err)
	}
	if strings.Contains(string(b), "stream_url") {
		t.Fatalf("idle NowPlaying must not carry stream_url, got %s", b)
	}
}

func TestTrackStreamURLRoundTrip(t *testing.T) {
	raw := `{"id":"yt:abc","title":"A","source":"youtube","stream_url":"https://example.invalid/audio.webm"}`
	var tr Track
	if err := json.Unmarshal([]byte(raw), &tr); err != nil {
		t.Fatalf("unmarshal Track: %v", err)
	}
	if tr.StreamURL != "https://example.invalid/audio.webm" {
		t.Fatalf("StreamURL not decoded, got %q", tr.StreamURL)
	}
}

func TestTrackPrefetchedStaysInternal(t *testing.T) {
	tr := Track{ID: "yt:abc", Title: "A", Source: "youtube", Prefetched: true}
	b, err := json.Marshal(tr)
	if err != nil {
		t.Fatalf("marshal Track: %v", err)
	}
	if strings.Contains(string(b), "refetch") {
		t.Fatalf("Prefetched must stay internal to vc-go, got %s", b)
	}
}

// IPC-3 (§29): duration_str is M:SS under an hour, Xh Ym Zs at/over an
// hour, and ∞ when unknown or live.
func TestFormatDurationStr(t *testing.T) {
	cases := []struct {
		sec    int64
		live   bool
		expect string
	}{
		{0, false, "∞"},
		{-5, false, "∞"},
		{45, false, "0:45"},
		{268, false, "4:28"},
		{3599, false, "59:59"},
		{3600, false, "1h"},
		{3661, false, "1h 1m 1s"},
		{3720, false, "1h 2m"},
		{3605, false, "1h 5s"},
		{7325, false, "2h 2m 5s"},
		{213, true, "∞"},
		{0, true, "∞"},
	}
	for _, c := range cases {
		if got := FormatDurationStr(c.sec, c.live); got != c.expect {
			t.Fatalf("FormatDurationStr(%d, live=%v) = %q, want %q", c.sec, c.live, got, c.expect)
		}
	}
}

func TestFormatUploadDate(t *testing.T) {
	if got := FormatUploadDate("20200115"); got != "2020-01-15" {
		t.Fatalf("FormatUploadDate(20200115) = %q, want 2020-01-15", got)
	}
	for _, raw := range []string{"", "2020", "2020011", "2020-01-15", "abcdefgh", "2020AB15"} {
		if got := FormatUploadDate(raw); got != raw {
			t.Fatalf("FormatUploadDate(%q) must pass through unchanged, got %q", raw, got)
		}
	}
}

// IPC-3 (§29): the enriched search fields serialize with the exact
// contract names; views stays absent (null) when the source has none.
func TestTrackIPC3FieldsSerialized(t *testing.T) {
	views := int64(1234567)
	tr := Track{
		ID: "yt:abc", Title: "A", Source: "youtube",
		DurationSec: 268, DurationStr: "4:28", Views: &views,
		Uploader: "Rick Astley", UploadTime: "2009-10-25", IsLive: false,
	}
	b, err := json.Marshal(tr)
	if err != nil {
		t.Fatalf("marshal Track: %v", err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("unmarshal Track JSON: %v", err)
	}
	if m["duration_str"] != "4:28" {
		t.Fatalf("duration_str missing/wrong in %s", b)
	}
	if m["views"] != float64(1234567) {
		t.Fatalf("views missing/wrong in %s", b)
	}
	if m["uploader"] != "Rick Astley" || m["upload_time"] != "2009-10-25" {
		t.Fatalf("uploader/upload_time missing/wrong in %s", b)
	}
	if _, ok := m["is_live"]; ok {
		t.Fatalf("is_live:false must be omitted, got %s", b)
	}
	live := Track{ID: "yt:live", Title: "L", Source: "youtube", IsLive: true, DurationStr: "∞"}
	b, _ = json.Marshal(live)
	var lm map[string]any
	_ = json.Unmarshal(b, &lm)
	if lm["is_live"] != true || lm["duration_str"] != "∞" {
		t.Fatalf("live track must carry is_live:true + duration_str ∞, got %s", b)
	}
	plain := Track{ID: "yt:abc", Title: "A", Source: "youtube"}
	b, _ = json.Marshal(plain)
	if strings.Contains(string(b), "duration_str") || strings.Contains(string(b), "views") ||
		strings.Contains(string(b), "uploader") || strings.Contains(string(b), "upload_time") {
		t.Fatalf("IPC-3 fields must be omitted when unknown, got %s", b)
	}
}

// IPC-1 (§29): the now-playing card fields serialize with the exact
// contract names and mirror the legacy fields.
func TestNowPlayingIPC1Fields(t *testing.T) {
	tr := Track{ID: "yt:abc", Title: "A", Source: "youtube"}
	np := NowPlaying{
		ChatID: 42, Track: &tr, Engine: "go", Queue: []QueueItem{},
		Paused: true, Loop: 3, Autoplay: true,
		IsPaused: true, IsMuted: true, LoopCount: 3, Speed: 1.0, AutoplayEnabled: true,
	}
	b, err := json.Marshal(np)
	if err != nil {
		t.Fatalf("marshal NowPlaying: %v", err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("unmarshal NowPlaying JSON: %v", err)
	}
	if m["is_paused"] != true || m["is_muted"] != true {
		t.Fatalf("is_paused/is_muted missing/wrong in %s", b)
	}
	if m["loop_count"] != float64(3) {
		t.Fatalf("loop_count missing/wrong in %s", b)
	}
	if m["speed"] != float64(1) {
		t.Fatalf("speed missing/wrong in %s", b)
	}
	if m["autoplay_enabled"] != true {
		t.Fatalf("autoplay_enabled missing/wrong in %s", b)
	}
}
