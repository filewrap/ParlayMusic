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
	np := NowPlaying{ChatID: 42, Track: &tr, Engine: "go", Queue: []QueueItem{}}
	b, err := json.Marshal(np)
	if err != nil {
		t.Fatalf("marshal NowPlaying: %v", err)
	}
	if !strings.Contains(string(b), `"stream_url":"https://example.invalid/audio.webm"`) {
		t.Fatalf("NowPlaying must carry the track stream_url, got %s", b)
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
