package fx

import (
	"encoding/json"
	"os"
	"path/filepath"
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
	return &Service{rooms: rooms, files: map[string]fileRecord{}}, st, rooms
}

func testInputFile(t *testing.T) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "song.mp3")
	if err := os.WriteFile(p, []byte("fake audio"), 0o644); err != nil {
		t.Fatalf("write input: %v", err)
	}
	return p
}

func TestApplyRecordOnly(t *testing.T) {
	svc, st, _ := testService(t)
	in := testInputFile(t)
	payload, _ := json.Marshal(map[string]any{
		"effect": "nightcore", "input_path": in, "chat_id": -100,
		"params": map[string]float64{"rate": 1.25},
	})
	data, err := svc.handleApply(t.Context(), uds.Request{ChatID: -100, Payload: payload})
	if err != nil {
		t.Fatalf("fx.apply: %v", err)
	}
	m := data.(map[string]any)
	// Record-only shape: nothing applied, DSP pending, output is
	// honestly the input path (no file was written).
	if m["applied"] != false || m["pending_dsp"] != true {
		t.Fatalf("record-only response: %v", m)
	}
	if m["output_path"] != in {
		t.Fatalf("output_path = %v, want the input path unchanged", m["output_path"])
	}
	if m["effect"] != "nightcore" {
		t.Fatalf("effect = %v", m["effect"])
	}
	// The preset is persisted on the room for the Phase 5 DSP.
	room, found, err := st.GetRoom(-100)
	if err != nil || !found {
		t.Fatalf("room: found=%v err=%v", found, err)
	}
	if room.FXPreset != "nightcore" || room.FXParams["rate"] != 1.25 {
		t.Fatalf("room fx = %q %v", room.FXPreset, room.FXParams)
	}
}

func TestApplyOriginalIsNoop(t *testing.T) {
	svc, _, _ := testService(t)
	in := testInputFile(t)
	payload, _ := json.Marshal(map[string]any{"effect": "original", "input_path": in})
	data, err := svc.handleApply(t.Context(), uds.Request{Payload: payload})
	if err != nil {
		t.Fatalf("fx.apply original: %v", err)
	}
	m := data.(map[string]any)
	if m["applied"] != true || m["pending_dsp"] != false {
		t.Fatalf("original response: %v (original is a no-op by definition)", m)
	}
}

func TestApplyValidation(t *testing.T) {
	svc, _, _ := testService(t)
	in := testInputFile(t)
	cases := []map[string]any{
		{"effect": "flanger", "input_path": in},
		{"effect": "nightcore"},
		{"effect": "nightcore", "input_path": "../escape.mp3"},
		{"effect": "nightcore", "input_path": "relative.mp3"},
		{"effect": "nightcore", "input_path": filepath.Join(t.TempDir(), "missing.mp3")},
		{"effect": "nightcore", "input_path": t.TempDir()},
	}
	for _, c := range cases {
		payload, _ := json.Marshal(c)
		_, err := svc.handleApply(t.Context(), uds.Request{Payload: payload})
		herr, ok := err.(*uds.HandlerError)
		if !ok || herr.Code != uds.ErrBadRequest {
			t.Fatalf("case %v: err=%v, want bad_request", c, err)
		}
	}
}

func TestApplyAllEffects(t *testing.T) {
	svc, _, _ := testService(t)
	in := testInputFile(t)
	for _, effect := range []string{"slow-reverb", "nightcore", "bass-boost", "8d", "original"} {
		payload, _ := json.Marshal(map[string]any{"effect": effect, "input_path": in})
		data, err := svc.handleApply(t.Context(), uds.Request{Payload: payload})
		if err != nil {
			t.Fatalf("effect %s: %v", effect, err)
		}
		m := data.(map[string]any)
		if effect == "original" {
			if m["applied"] != true {
				t.Fatalf("original must report applied=true: %v", m)
			}
		} else if m["applied"] != false || m["pending_dsp"] != true {
			t.Fatalf("effect %s must never claim applied: %v", effect, m)
		}
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
	_ = model.DefaultRoomState
}

// TestApplySurvivesRestart is the Phase 4 persistence proof: an
// fx.apply survives a store close + reopen and a fresh Manager
// Restore — the preset and params come back both from the
// Manager's read seam (Manager.FX, the seam the Phase 5 DSP will
// use) and from the raw bbolt record.
func TestApplySurvivesRestart(t *testing.T) {
	dir := t.TempDir()
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	rooms := room.NewManager(st, 0)
	svc := &Service{rooms: rooms, files: map[string]fileRecord{}}
	in := testInputFile(t)
	payload, _ := json.Marshal(map[string]any{
		"effect": "nightcore", "input_path": in, "chat_id": -100,
		"params": map[string]float64{"rate": 1.25},
	})
	if _, err := svc.handleApply(t.Context(), uds.Request{ChatID: -100, Payload: payload}); err != nil {
		t.Fatalf("fx.apply: %v", err)
	}
	if err := st.Close(); err != nil {
		t.Fatalf("store.Close: %v", err)
	}
	st2, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store reopen: %v", err)
	}
	defer st2.Close()
	// The raw bbolt record survived the restart.
	raw, found, err := st2.GetRoom(-100)
	if err != nil || !found {
		t.Fatalf("reopened store room: found=%v err=%v", found, err)
	}
	if raw.FXPreset != "nightcore" || raw.FXParams["rate"] != 1.25 {
		t.Fatalf("reopened store room fx = %q %v", raw.FXPreset, raw.FXParams)
	}
	// And a fresh Manager restores it into the live seam.
	rooms2 := room.NewManager(st2, 0)
	if err := rooms2.Restore(); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	effect, params, ok := rooms2.FX(-100)
	if !ok || effect != "nightcore" || params["rate"] != 1.25 {
		t.Fatalf("restored Manager FX = %q %v ok=%v", effect, params, ok)
	}
}

// TestApplyDoesNotClobberLiveRoom pins the exact staleness shape
// the Phase 3 audit flagged: fx writing the store directly was
// invisible to the Manager, so the next Manager mutation persisted
// the in-memory copy and clobbered the FX fields. With the fix,
// fx.apply on a playing room leaves the live track intact, the FX
// visible through the Manager, and a later Manager mutation
// (Pause → persist) still leaves the FX in the store.
func TestApplyDoesNotClobberLiveRoom(t *testing.T) {
	svc, st, rooms := testService(t)
	track := model.Track{ID: "yt:a", Title: "A", DurationSec: 300}
	if _, err := rooms.Play(-100, track, 7); err != nil {
		t.Fatalf("rooms.Play: %v", err)
	}
	in := testInputFile(t)
	payload, _ := json.Marshal(map[string]any{
		"effect": "bass-boost", "input_path": in, "chat_id": -100,
		"params": map[string]float64{"gain": 6},
	})
	if _, err := svc.handleApply(t.Context(), uds.Request{ChatID: -100, Payload: payload}); err != nil {
		t.Fatalf("fx.apply: %v", err)
	}
	// The live room is untouched by the apply.
	snap, ok := rooms.Snapshot(-100)
	if !ok || snap.Track == nil || snap.Track.ID != "yt:a" {
		t.Fatalf("snapshot after fx.apply: %+v ok=%v", snap, ok)
	}
	// The FX is visible through the Manager seam immediately.
	effect, params, ok := rooms.FX(-100)
	if !ok || effect != "bass-boost" || params["gain"] != 6 {
		t.Fatalf("Manager FX after apply = %q %v ok=%v", effect, params, ok)
	}
	// A later Manager mutation persists the room — and must NOT
	// clobber the FX (this is the assertion the old direct-write
	// code failed: Pause persisted the pre-fx in-memory copy).
	if _, err := rooms.Pause(-100); err != nil {
		t.Fatalf("rooms.Pause: %v", err)
	}
	stored, found, err := st.GetRoom(-100)
	if err != nil || !found {
		t.Fatalf("store room after Pause: found=%v err=%v", found, err)
	}
	if stored.FXPreset != "bass-boost" || stored.FXParams["gain"] != 6 {
		t.Fatalf("store room fx after Pause = %q %v (fx clobbered by Manager persist)", stored.FXPreset, stored.FXParams)
	}
	if stored.Current == nil || stored.Current.ID != "yt:a" {
		t.Fatalf("store room current after Pause = %+v (fx clobbered the live track)", stored.Current)
	}
}
