package fx

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

func testService(t *testing.T) (*Service, *store.Store) {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return &Service{store: st, files: map[string]fileRecord{}}, st
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
	svc, st := testService(t)
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
	svc, _ := testService(t)
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
	svc, _ := testService(t)
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
	svc, _ := testService(t)
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
	Register(server, &app.Deps{Store: st})
	_ = model.DefaultRoomState
}
