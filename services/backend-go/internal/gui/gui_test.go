package gui

import (
	"encoding/json"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

func testService(t *testing.T) (*Service, *store.Store, *room.Manager) {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	rooms := room.NewManager(st, 0)
	return &Service{store: st, room: rooms}, st, rooms
}

// seedRoom seeds a playing room through the shared room.Manager
// (the handlers read only through it — seeding the store buckets
// directly would leave the Manager's in-memory cache empty).
func seedRoom(t *testing.T, rooms *room.Manager, chatID int64) {
	t.Helper()
	track := model.Track{ID: "yt:abc", Title: "Song", Artist: "A", Source: "youtube", DurationSec: 200, StreamURL: "https://example.invalid/stream"}
	if _, err := rooms.Play(chatID, track, 1); err != nil {
		t.Fatalf("Play: %v", err)
	}
	if err := rooms.Enqueue(chatID, model.Track{ID: "yt:def", Title: "Next", Artist: "B", Source: "youtube"}, 1); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
}

func TestNowPlayingRead(t *testing.T) {
	svc, _, rooms := testService(t)
	seedRoom(t, rooms, -100)
	data, err := svc.handleNowPlaying(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)})
	if err != nil {
		t.Fatalf("gui.now_playing: %v", err)
	}
	np := data.(map[string]any)["now_playing"].(model.NowPlaying)
	if np.Track == nil || np.Track.ID != "yt:abc" {
		t.Fatalf("now_playing track: %+v", np.Track)
	}
	if np.Engine != "backend-go" || len(np.Queue) != 1 || np.Queue[0].Track.ID != "yt:def" {
		t.Fatalf("now_playing snapshot: %+v", np)
	}
	if np.Volume != 100 || np.Speed != 1.0 {
		t.Fatalf("now_playing defaults: volume=%d speed=%v", np.Volume, np.Speed)
	}
}

func TestNowPlayingNothingPlaying(t *testing.T) {
	svc, _, _ := testService(t)
	_, err := svc.handleNowPlaying(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)})
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrNotFound {
		t.Fatalf("empty chat: err=%v, want not_found", err)
	}
}

func TestQueueRead(t *testing.T) {
	svc, _, rooms := testService(t)
	seedRoom(t, rooms, -100)
	data, err := svc.handleQueue(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)})
	if err != nil {
		t.Fatalf("gui.queue: %v", err)
	}
	queue := data.(map[string]any)["queue"].([]model.QueueItem)
	if len(queue) != 1 || queue[0].Track.ID != "yt:def" {
		t.Fatalf("queue: %+v", queue)
	}
	// Unknown chat: an empty queue, 200 — the Mini App needs an
	// empty state, not an error.
	data, err = svc.handleQueue(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-999}`)})
	if err != nil {
		t.Fatalf("gui.queue unknown chat: %v", err)
	}
	if queue := data.(map[string]any)["queue"].([]model.QueueItem); len(queue) != 0 {
		t.Fatalf("unknown chat queue: %+v", queue)
	}
}

func TestQueueIsReadOnly(t *testing.T) {
	svc, st, rooms := testService(t)
	seedRoom(t, rooms, -100)
	if _, err := svc.handleQueue(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)}); err != nil {
		t.Fatalf("gui.queue: %v", err)
	}
	if _, err := svc.handleNowPlaying(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)}); err != nil {
		t.Fatalf("gui.now_playing: %v", err)
	}
	// Reads must not mutate the room: the stored state is unchanged.
	room, _, err := st.GetRoom(-100)
	if err != nil {
		t.Fatalf("GetRoom: %v", err)
	}
	if room.Current == nil || room.Current.ID != "yt:abc" {
		t.Fatalf("room mutated by gui reads: %+v", room)
	}
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	Register(server, &app.Deps{Store: st, Room: room.NewManager(st, 0)})
}
