package download

import (
	"context"
	"strings"
	"testing"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

func TestHandleMixValidation(t *testing.T) {
	_, svc, _ := testDeps(t)
	ctx := context.Background()
	// chat_id is required.
	_, err := svc.handleMix(ctx, uds.Request{Payload: mustJSON(t, map[string]any{})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("missing chat_id: code = %v", err)
	}
	// No query and nothing playing -> "Nothing is playing".
	_, err = svc.handleMix(ctx, uds.Request{Payload: mustJSON(t, map[string]any{"chat_id": 123})})
	if handlerErrCode(t, err) != uds.ErrBadRequest {
		t.Fatalf("nothing playing: code = %v", err)
	}
	if !strings.Contains(err.Error(), "Nothing is playing") {
		t.Fatalf("nothing playing: error = %v", err)
	}
}

func TestHandleMixNonYouTubeSeed(t *testing.T) {
	deps, svc, _ := testDeps(t)
	chatID := int64(123)
	// A room playing a Spotify track: the seed is not YouTube, so
	// the mix fails honestly (only YouTube tracks have mixes).
	seed := model.Track{ID: "spotify:abc", Title: "Seed", Artist: "A", Source: "spotify"}
	if err := deps.Store.PutRoom(model.RoomState{ChatID: chatID, Current: &seed}); err != nil {
		t.Fatal(err)
	}
	_, err := svc.handleMix(context.Background(), uds.Request{Payload: mustJSON(t, map[string]any{"chat_id": chatID})})
	if handlerErrCode(t, err) != uds.ErrEngineUnavailable {
		t.Fatalf("non-YouTube seed: code = %v, want engine_unavailable", err)
	}
	if !strings.Contains(err.Error(), "Failed to get mix") {
		t.Fatalf("non-YouTube seed: error = %v", err)
	}
	// The room is untouched by the failed mix.
	room, found, err := deps.Store.GetRoom(chatID)
	if err != nil || !found || room.Current == nil || room.Current.ID != "spotify:abc" {
		t.Fatalf("room after failed mix = %+v found=%v err=%v", room, found, err)
	}
}

func TestMixTrackFromEntry(t *testing.T) {
	entry := map[string]any{
		"id": "dQw4w9WgXcQ", "title": "Never Gonna Give You Up",
		"uploader": "Rick Astley", "duration": float64(213),
	}
	track := mixTrackFromEntry(entry)
	if track.ID != "dQw4w9WgXcQ" {
		t.Fatalf("id = %q", track.ID)
	}
	if track.Title != "Never Gonna Give You Up" || track.Artist != "Rick Astley" {
		t.Fatalf("track = %+v", track)
	}
	if track.DurationSec != 213 {
		t.Fatalf("duration = %d, want 213", track.DurationSec)
	}
	if track.Source != "youtube" {
		t.Fatalf("source = %q, want youtube", track.Source)
	}
	// A missing id is not a track.
	if got := mixTrackFromEntry(map[string]any{"title": "No ID"}); got.ID != "" {
		t.Fatalf("no-id entry should map to an empty track, got %+v", got)
	}
}

func TestRoomCurrentTrack(t *testing.T) {
	deps, svc, _ := testDeps(t)
	chatID := int64(77)
	// No room -> nil, no error.
	current, err := svc.roomCurrentTrack(chatID)
	if err != nil || current != nil {
		t.Fatalf("no room: current=%v err=%v", current, err)
	}
	// Room with no current track -> nil.
	if err := deps.Store.PutRoom(model.RoomState{ChatID: chatID}); err != nil {
		t.Fatal(err)
	}
	current, err = svc.roomCurrentTrack(chatID)
	if err != nil || current != nil {
		t.Fatalf("idle room: current=%v err=%v", current, err)
	}
	// Room with a current track -> that track.
	want := model.Track{ID: "yt:x", Title: "Now", Source: "youtube"}
	if err := deps.Store.PutRoom(model.RoomState{ChatID: chatID, Current: &want}); err != nil {
		t.Fatal(err)
	}
	current, err = svc.roomCurrentTrack(chatID)
	if err != nil || current == nil || current.ID != "yt:x" {
		t.Fatalf("playing room: current=%v err=%v", current, err)
	}
}
