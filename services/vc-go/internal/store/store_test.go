package store

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

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
func TestMongoStoreRoundTrip(t *testing.T) {
	uri := os.Getenv("MONGO_URI")
	if uri == "" {
		uri = "mongodb://127.0.0.1:27017"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	ms, err := NewMongo(ctx, uri, "parlay_test", "vc_rooms_test")
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
			Current: &types.Track{ID: "yt:a", Title: "A", Source: "youtube"},
			Waiting: []types.QueueItem{{Position: 0, RequestedBy: 1, Track: types.Track{ID: "yt:b", Title: "B", Source: "youtube"}}},
			Volume:  100, Loop: 2, Shuffle: true, Autoplay: true,
			AutoplayHistory: []string{"yt:z"},
		},
		{ChatID: 22, Volume: 80},
	}
	if err := ms.Save(rooms); err != nil {
		t.Fatalf("Save: %v", err)
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
