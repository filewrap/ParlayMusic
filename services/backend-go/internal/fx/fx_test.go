package fx

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
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
	// Record-only shape (no voice configured): nothing applied,
	// no pending_dsp field anymore, the detail says the preset
	// is recorded for the next track — and output is honestly
	// the input path (no file was written).
	if m["applied"] != false || m["live_applied"] != false {
		t.Fatalf("record-only response: %v", m)
	}
	if _, ok := m["pending_dsp"]; ok {
		t.Fatalf("record-only response still carries pending_dsp: %v", m)
	}
	if m["detail"] != "recorded for the next track; no live stream to apply to" {
		t.Fatalf("record-only detail = %v", m["detail"])
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
	if m["applied"] != true {
		t.Fatalf("original response: %v (original is a no-op by definition)", m)
	}
	if _, ok := m["pending_dsp"]; ok {
		t.Fatalf("original response still carries pending_dsp: %v", m)
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
		} else if m["applied"] != false {
			t.Fatalf("effect %s must never claim applied with no live stream: %v", effect, m)
		}
		if _, ok := m["pending_dsp"]; ok {
			t.Fatalf("effect %s response still carries pending_dsp: %v", effect, m)
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
// Manager's read seam (Manager.FX, the seam the Phase 5 DSP uses)
// and from the raw bbolt record.
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

// fakeVoice is the test double for the voice Manager's live
// parameter push (the liveFX interface). err is what ApplyFX
// returns: nil confirms the effect, voice.ErrNoFeed means no live
// feed, anything else is a DSP rejection. lastOpts records the
// options the DSP was given, so tests can prove the room's
// speed/bass/volume rode along with the preset.
type fakeVoice struct {
	err      error
	lastOpts dsp.Options
	calls    int
}

func (f *fakeVoice) ApplyFX(chatID int64, opts dsp.Options) error {
	f.calls++
	f.lastOpts = opts
	return f.err
}

func applyPayload(t *testing.T, svc *Service, effect, in string, chatID int64) map[string]any {
	t.Helper()
	payload, _ := json.Marshal(map[string]any{
		"effect": effect, "input_path": in, "chat_id": chatID,
		"params": map[string]float64{"rate": 1.25},
	})
	data, err := svc.handleApply(t.Context(), uds.Request{ChatID: chatID, Payload: payload})
	if err != nil {
		t.Fatalf("fx.apply: %v", err)
	}
	return data.(map[string]any)
}

// TestApplyLiveConfirmed proves the honest-applied path: the DSP
// confirms the effect (ApplyFX returns nil) and fx.apply reports
// applied:true — with the room's speed/bass/volume carried in the
// same options, so a vc.play speed/bass survives an fx.apply.
func TestApplyLiveConfirmed(t *testing.T) {
	_, st, rooms := testService(t)
	_ = st
	// The room's vc.play speed/bass, set before the apply.
	if _, err := rooms.SetSpeed(-100, 1.5); err != nil {
		t.Fatalf("SetSpeed: %v", err)
	}
	if _, err := rooms.SetBass(-100, 40); err != nil {
		t.Fatalf("SetBass: %v", err)
	}
	fv := &fakeVoice{}
	svc := &Service{rooms: rooms, voice: fv, files: map[string]fileRecord{}}
	m := applyPayload(t, svc, "nightcore", testInputFile(t), -100)
	if m["applied"] != true || m["live_applied"] != true {
		t.Fatalf("live-confirmed response: %v", m)
	}
	if fv.calls != 1 {
		t.Fatalf("ApplyFX called %d times, want 1", fv.calls)
	}
	got := fv.lastOpts
	if got.Effect != "nightcore" || got.Speed != 1.5 || got.Bass != 40 || got.Volume != 100 || got.EffectParams["rate"] != 1.25 {
		t.Errorf("DSP got options %+v, want effect nightcore speed 1.5 bass 40 volume 100 rate 1.25", got)
	}
}

// TestApplyNoLiveFeed proves the record-only honesty: the preset is
// stored on the room, but applied stays false when no stream is
// playing — the effect is never claimed applied when only recorded.
func TestApplyNoLiveFeed(t *testing.T) {
	_, _, rooms := testService(t)
	fv := &fakeVoice{err: voice.ErrNoFeed}
	svc := &Service{rooms: rooms, voice: fv, files: map[string]fileRecord{}}
	m := applyPayload(t, svc, "bass-boost", testInputFile(t), -100)
	if m["applied"] != false || m["live_applied"] != false {
		t.Fatalf("no-live-feed response: %v", m)
	}
	if detail, _ := m["detail"].(string); !strings.Contains(detail, "no live stream") {
		t.Fatalf("no-live-feed detail = %q", detail)
	}
	// The preset is still recorded for the next track.
	if effect, _, ok := rooms.FX(-100); !ok || effect != "bass-boost" {
		t.Fatalf("room FX after no-live-feed apply = %q ok=%v", effect, ok)
	}
}

// TestApplyDSPRejects proves a DSP rejection is reported honestly:
// applied:false and the detail carries the exact error.
func TestApplyDSPRejects(t *testing.T) {
	_, _, rooms := testService(t)
	fv := &fakeVoice{err: errors.New("dsp: effect params out of range")}
	svc := &Service{rooms: rooms, voice: fv, files: map[string]fileRecord{}}
	m := applyPayload(t, svc, "8d", testInputFile(t), -100)
	if m["applied"] != false || m["live_applied"] != false {
		t.Fatalf("rejected response: %v", m)
	}
	if detail, _ := m["detail"].(string); !strings.Contains(detail, "dsp: effect params out of range") {
		t.Fatalf("rejected detail = %q", detail)
	}
}

// TestApplyOriginalLiveClear proves "original" clears a live effect:
// the clear is pushed to the DSP and applied:true only when the
// push succeeds — but live_applied stays false: the clear is not
// an effect sounding on the stream, and the original file remains
// the deliverable (the core sends it when applied:true and
// live_applied is false).
func TestApplyOriginalLiveClear(t *testing.T) {
	_, _, rooms := testService(t)
	fv := &fakeVoice{}
	svc := &Service{rooms: rooms, voice: fv, files: map[string]fileRecord{}}
	m := applyPayload(t, svc, "original", testInputFile(t), -100)
	if m["applied"] != true || m["live_applied"] != false {
		t.Fatalf("original live-clear response: %v", m)
	}
	if fv.lastOpts.Effect != "original" {
		t.Errorf("DSP got effect %q for the original clear", fv.lastOpts.Effect)
	}
}

// TestApplyOriginalNoLiveFeed proves "original" with nothing
// playing is applied:true (a no-op by definition — there is
// nothing to clear), while live_applied stays false.
func TestApplyOriginalNoLiveFeed(t *testing.T) {
	_, _, rooms := testService(t)
	fv := &fakeVoice{err: voice.ErrNoFeed}
	svc := &Service{rooms: rooms, voice: fv, files: map[string]fileRecord{}}
	m := applyPayload(t, svc, "original", testInputFile(t), -100)
	if m["applied"] != true || m["live_applied"] != false {
		t.Fatalf("original no-live-feed response: %v", m)
	}
}
