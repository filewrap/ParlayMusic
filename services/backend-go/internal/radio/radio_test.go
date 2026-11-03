package radio

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

func stubProbe(reachable bool) func(context.Context, string) resolver.ProbeResult {
	return func(ctx context.Context, url string) resolver.ProbeResult {
		return resolver.ProbeResult{Reachable: reachable, Format: "mp3", Probe: "ok (stub)"}
	}
}

func testService(t *testing.T) *Service {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return &Service{
		store: st, probe: stubProbe(true),
		handoffs: map[int64]map[string]any{},
	}
}

func TestListStations(t *testing.T) {
	svc := testService(t)
	data, err := svc.handleList(t.Context(), uds.Request{Payload: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatalf("radio.list: %v", err)
	}
	// The handler returns []stationView (unexported); re-decode via JSON.
	raw, _ := json.Marshal(data)
	var decoded struct {
		Stations []struct {
			ID        string `json:"id"`
			Name      string `json:"name"`
			Reachable bool   `json:"reachable"`
		} `json:"stations"`
	}
	if err := json.Unmarshal(raw, &decoded); err != nil {
		t.Fatalf("decode list: %v", err)
	}
	if len(decoded.Stations) != 6 {
		t.Fatalf("stations = %d, want 6 (locked list)", len(decoded.Stations))
	}
	want := map[string]bool{
		"groovesalad": true, "lush": true, "radioparadise": true,
		"fip": true, "bbcws": true, "kexp": true,
	}
	for _, st := range decoded.Stations {
		if !want[st.ID] {
			t.Fatalf("unexpected station %q", st.ID)
		}
		if !st.Reachable {
			t.Fatalf("station %s: stub probe must report reachable", st.ID)
		}
	}
}

func TestStartStopStatus(t *testing.T) {
	svc := testService(t)
	result, err := svc.Start(t.Context(), -100, "groovesalad")
	if err != nil {
		t.Fatalf("radio.start: %v", err)
	}
	if result.Station != "groovesalad" || !result.Reachable {
		t.Fatalf("start result: %+v", result)
	}
	if result.NowPlaying.Track == nil || result.NowPlaying.Track.Source != "radio" {
		t.Fatalf("radio track not on the room: %+v", result.NowPlaying.Track)
	}
	// Handoff with no voice manager configured: reported, and the
	// start still succeeds.
	if result.VCHandoff["joined"] != false {
		t.Fatalf("vc_handoff without voice: %v", result.VCHandoff)
	}
	// Status reflects the radio.
	data, err := svc.handleStatus(t.Context(), uds.Request{Payload: json.RawMessage(`{"chat_id":-100}`)})
	if err != nil {
		t.Fatalf("radio.status: %v", err)
	}
	m := data.(map[string]any)
	if m["station"] != "groovesalad" || m["listeners"] != 0 {
		t.Fatalf("status: %v", m)
	}
	// Stop clears the radio state and the room's radio track.
	vcCleared, err := svc.Stop(t.Context(), -100)
	if err != nil {
		t.Fatalf("radio.stop: %v", err)
	}
	if vcCleared {
		t.Fatalf("vc_cleared must be false with no voice manager")
	}
	if _, found, _ := svc.store.GetRadio(-100); found {
		t.Fatalf("radio state survived stop")
	}
	room, _, _ := svc.store.GetRoom(-100)
	if room.Current != nil {
		t.Fatalf("room current survived radio stop: %+v", room.Current)
	}
	// Stopping again is a 404.
	if _, err := svc.Stop(t.Context(), -100); err == nil {
		t.Fatalf("second stop succeeded, want not_found")
	} else if herr, ok := err.(*uds.HandlerError); !ok || herr.Code != uds.ErrNotFound {
		t.Fatalf("second stop: err=%v, want not_found", err)
	}
}

func TestStartUnknownStation(t *testing.T) {
	svc := testService(t)
	_, err := svc.Start(t.Context(), -100, "not-a-station")
	herr, ok := err.(*uds.HandlerError)
	if !ok || herr.Code != uds.ErrBadRequest {
		t.Fatalf("unknown station: err=%v, want bad_request", err)
	}
	if !strings.Contains(herr.Message, "groovesalad") {
		t.Fatalf("unknown-station error must name the valid stations: %s", herr.Message)
	}
}

func TestStartUnreachableStillStarts(t *testing.T) {
	svc := testService(t)
	svc.probe = stubProbe(false)
	result, err := svc.Start(t.Context(), -100, "lush")
	if err != nil {
		t.Fatalf("start with unreachable stream: %v", err)
	}
	if result.Reachable {
		t.Fatalf("unreachable probe reported reachable")
	}
	if result.NowPlaying.Track == nil {
		t.Fatalf("unreachable stream still starts with a radio track")
	}
}

func TestStopDoesNotKillMusicTrack(t *testing.T) {
	svc := testService(t)
	if _, err := svc.Start(t.Context(), -100, "kexp"); err != nil {
		t.Fatalf("start: %v", err)
	}
	// A normal music track replaces the radio track on the room.
	room, _, _ := svc.store.GetRoom(-100)
	room.Current = &model.Track{ID: "yt:abc", Title: "Song", Source: "youtube"}
	if err := svc.store.PutRoom(room); err != nil {
		t.Fatalf("PutRoom: %v", err)
	}
	if _, err := svc.Stop(t.Context(), -100); err != nil {
		t.Fatalf("stop: %v", err)
	}
	room, _, _ = svc.store.GetRoom(-100)
	if room.Current == nil || room.Current.ID != "yt:abc" {
		t.Fatalf("stop killed the music track that replaced the radio: %+v", room.Current)
	}
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	Register(server, &app.Deps{Store: st, Registry: resolver.NewRegistry()})
}
