package store

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/types"
)

func TestJSONStoreRoundTrip(t *testing.T) {
	dir := t.TempDir()
	st, err := New(dir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	rooms := []queue.PersistedRoom{{
		ChatID:  7,
		Current: &types.Track{ID: "yt:a", Title: "A", Source: "youtube"},
		Waiting: []types.QueueItem{{Position: 0, RequestedBy: 1, Track: types.Track{ID: "yt:b", Title: "B", Source: "youtube"}}},
		Volume:  100,
		Loop:    2,
		Shuffle: true,
	}}
	if err := st.Save(rooms); err != nil {
		t.Fatalf("Save: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "queues.json")); err != nil {
		t.Fatalf("queues.json missing: %v", err)
	}
	got, err := st.Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(got) != 1 || got[0].ChatID != 7 || got[0].Loop != 2 || !got[0].Shuffle ||
		got[0].Current == nil || got[0].Current.ID != "yt:a" || len(got[0].Waiting) != 1 {
		t.Fatalf("round-trip lost state: %+v", got)
	}
}

// TestMongoStoreRoundTrip exercises the real MongoDB backend against the
// local mongod. It skips when no mongod answers (unit runs without one).
// Layout is canonical (A11): collection `state`, _id "kv:vc:room:<id>",
// v = canonical room JSON.
func TestMongoStoreRoundTrip(t *testing.T) {
	uri := os.Getenv("MONGO_URI")
	if uri == "" {
		uri = "mongodb://127.0.0.1:27017"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	ms, err := NewMongo(ctx, uri, "parlay_test")
	if err != nil {
		t.Skipf("no local mongod at %s: %v", uri, err)
	}
	defer func() { _ = ms.Close(context.Background()) }()
	// Start clean.
	if err := ms.Save(nil); err != nil {
		t.Fatalf("prune: %v", err)
	}
	rooms := []queue.PersistedRoom{
		{
			ChatID:  11,
			Current: &types.Track{ID: "yt:a", Title: "A", Source: "youtube", StreamURL: "https://rr1---sn.googlevideo.com/x"},
			Waiting: []types.QueueItem{{Position: 0, RequestedBy: 1, Track: types.Track{ID: "yt:b", Title: "B", Source: "youtube"}}},
			Volume:  100, Loop: 2, Shuffle: true, Autoplay: true,
			AutoplayHistory: []string{"yt:z"},
		},
		{ChatID: 22, Volume: 80},
	}
	if err := ms.Save(rooms); err != nil {
		t.Fatalf("Save: %v", err)
	}
	// The raw document is the canonical shape: _id kv:vc:room:<id>,
	// v a JSON string with "queue" (not "waiting") and version 1.
	var raw struct {
		ID    string `bson:"_id"`
		Value string `bson:"v"`
	}
	if err := ms.coll.FindOne(ctx, bson.M{"_id": "kv:vc:room:11"}).Decode(&raw); err != nil {
		t.Fatalf("raw doc for chat 11 missing: %v", err)
	}
	if !strings.Contains(raw.Value, `"queue"`) || strings.Contains(raw.Value, `"waiting"`) {
		t.Fatalf("canonical value must use queue, not waiting: %s", raw.Value)
	}
	if !strings.Contains(raw.Value, `"version":1`) || !strings.Contains(raw.Value, `"history":["yt:z"]`) {
		t.Fatalf("canonical value missing version/history: %s", raw.Value)
	}
	got, err := ms.Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("expected 2 rooms, got %d", len(got))
	}
	byID := map[int64]queue.PersistedRoom{}
	for _, p := range got {
		byID[p.ChatID] = p
	}
	r11 := byID[11]
	if r11.Current == nil || r11.Current.ID != "yt:a" || len(r11.Waiting) != 1 ||
		r11.Loop != 2 || !r11.Shuffle || !r11.Autoplay ||
		len(r11.AutoplayHistory) != 1 || r11.AutoplayHistory[0] != "yt:z" {
		t.Fatalf("room 11 lost state: %+v", r11)
	}
	if r11.Current.StreamURL != "https://rr1---sn.googlevideo.com/x" {
		t.Fatalf("room 11 lost stream_url: %+v", r11.Current)
	}
	if byID[22].Volume != 80 {
		t.Fatalf("room 22 volume wrong: %+v", byID[22])
	}
	// Saving a subset prunes the stale chat.
	if err := ms.Save(rooms[:1]); err != nil {
		t.Fatalf("Save subset: %v", err)
	}
	got, err = ms.Load()
	if err != nil {
		t.Fatalf("Load after prune: %v", err)
	}
	if len(got) != 1 || got[0].ChatID != 11 {
		t.Fatalf("prune should leave only chat 11, got %+v", got)
	}
	// Clean up after ourselves.
	if err := ms.Save(nil); err != nil {
		t.Fatalf("final prune: %v", err)
	}
}

// TestRedisStoreRoundTrip exercises the real Redis backend. It skips
// when no redis answers (unit runs without one).
func TestRedisStoreRoundTrip(t *testing.T) {
	uri := os.Getenv("REDIS_URL")
	if uri == "" {
		uri = "redis://127.0.0.1:6379/0"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	rs, err := NewRedis(ctx, uri)
	if err != nil {
		t.Skipf("no local redis at %s: %v", uri, err)
	}
	defer func() { _ = rs.Close() }()
	if err := rs.Save(nil); err != nil {
		t.Fatalf("prune: %v", err)
	}
	rooms := []queue.PersistedRoom{
		{
			ChatID:  33,
			Current: &types.Track{ID: "yt:c", Title: "C", Source: "youtube"},
			Waiting: []types.QueueItem{{Position: 0, RequestedBy: 2, Track: types.Track{ID: "yt:d", Title: "D", Source: "youtube"}}},
			Volume:  90, Loop: 1,
		},
	}
	if err := rs.Save(rooms); err != nil {
		t.Fatalf("Save: %v", err)
	}
	got, err := rs.Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(got) != 1 || got[0].ChatID != 33 || got[0].Current == nil ||
		got[0].Current.ID != "yt:c" || len(got[0].Waiting) != 1 || got[0].Loop != 1 {
		t.Fatalf("round-trip lost state: %+v", got)
	}
	if err := rs.Save(nil); err != nil {
		t.Fatalf("final prune: %v", err)
	}
	got, err = rs.Load()
	if err != nil {
		t.Fatalf("Load after prune: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("prune should leave no rooms, got %+v", got)
	}
}

// TestCanonicalRoomRoundTrip checks the canonical document (A11):
// PersistedRoom -> canonical JSON -> PersistedRoom preserves tracks,
// order and position, and uses the canonical field names.
func TestCanonicalRoomRoundTrip(t *testing.T) {
	p := queue.PersistedRoom{
		ChatID:   55,
		Current:  &types.Track{ID: "yt:a", Title: "A", Source: "youtube", StreamURL: "https://example.invalid/stream", DurationSec: 213},
		Waiting:  []types.QueueItem{{Position: 0, RequestedBy: 7, Track: types.Track{ID: "yt:b", Title: "B", Source: "youtube", DurationSec: 172}}},
		Paused:   true,
		Volume:   80,
		Position: 42,
		Loop:     2,
		Shuffle:  true,
		Autoplay: true,
		AutoplayHistory: []string{"yt:z"},
	}
	text := CanonicalRoomJSON(p)
	if !strings.Contains(text, `"version":1`) || !strings.Contains(text, `"queue"`) ||
		strings.Contains(text, `"waiting"`) || !strings.Contains(text, `"history":["yt:z"]`) ||
		!strings.Contains(text, `"updated_at_ms"`) {
		t.Fatalf("canonical JSON has wrong shape: %s", text)
	}
	back, err := ParseCanonicalRoom(text)
	if err != nil {
		t.Fatalf("ParseCanonicalRoom: %v", err)
	}
	if back.ChatID != 55 || back.Current == nil || back.Current.ID != "yt:a" ||
		back.Current.StreamURL != "https://example.invalid/stream" ||
		len(back.Waiting) != 1 || back.Waiting[0].Track.ID != "yt:b" ||
		back.Waiting[0].RequestedBy != 7 || back.Position != 42 || back.Loop != 2 ||
		!back.Shuffle || !back.Autoplay || len(back.AutoplayHistory) != 1 {
		t.Fatalf("canonical round-trip lost state: %+v", back)
	}
}

// TestCanonicalRoomFromForeignDoc checks that a canonical document
// written by another engine (vc-ts/vc-rust shape: no stream_url, no
// waiting/history renames needed, extra fields ignored) loads with
// the same tracks, order and position.
func TestCanonicalRoomFromForeignDoc(t *testing.T) {
	doc := `{"version":1,"chat_id":777,
		"current":{"id":"yt:ts-a","title":"TS A","artist":"A","source":"youtube",
			"url":"https://www.youtube.com/watch?v=ts-a","duration_sec":213},
		"queue":[{"position":0,"requested_by":9,"track":{"id":"yt:ts-b","title":"TS B","source":"youtube","duration_sec":172}},
			{"position":1,"requested_by":9,"track":{"id":"yt:ts-c","title":"TS C","source":"youtube","duration_sec":240}}],
		"paused":false,"position_sec":57,"volume":100,"loop":1,"shuffle":false,
		"autoplay":true,"history":["yt:ts-z"],"updated_at_ms":1793000000000}`
	p, err := ParseCanonicalRoom(doc)
	if err != nil {
		t.Fatalf("ParseCanonicalRoom: %v", err)
	}
	if p.ChatID != 777 || p.Current == nil || p.Current.ID != "yt:ts-a" ||
		p.Position != 57 || p.Loop != 1 || !p.Autoplay ||
		len(p.Waiting) != 2 || p.Waiting[0].Track.ID != "yt:ts-b" ||
		p.Waiting[1].Track.ID != "yt:ts-c" || len(p.AutoplayHistory) != 1 {
		t.Fatalf("foreign doc lost state: %+v", p)
	}
}
