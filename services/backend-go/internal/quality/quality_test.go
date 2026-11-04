package quality

import (
	"context"
	"encoding/json"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

func intPtr(n int) *int    { return &n }
func boolPtr(b bool) *bool { return &b }

// saveOpusConfig snapshots the process-global encoder settings
// so a test that changes them can restore them afterwards
// (same pattern as the voice package's own helper, which is
// unexported there).
func saveOpusConfig(t *testing.T) (restore func()) {
	t.Helper()
	cur, err := voice.OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig (save): %v", err)
	}
	return func() {
		if err := voice.SetOpusConfig(cur); err != nil {
			t.Errorf("OpusConfig (restore): %v", err)
		}
	}
}

func baseConfig() *config.Config {
	return &config.Config{
		Quality: config.QualityConfig{VoiceProfile: "standard_192k"},
	}
}

func TestResolvePrecedence(t *testing.T) {
	// No overrides anywhere: profile bitrate + defaults.
	eff, src := Resolve(baseConfig(), model.ChatSettings{})
	if eff.BitrateBps != 192000 || eff.FrameSizeMs != 20 || eff.Complexity != 10 ||
		!eff.VBREnabled || eff.FECEnabled || eff.DTXEnabled || eff.MaxPlaybackRateHz != 48000 {
		t.Fatalf("defaults resolve drifted: %+v", eff)
	}
	if src[config.OpusFieldBitrateBps] != "profile" || src[config.OpusFieldFrameMs] != "default" {
		t.Fatalf("default sources drifted: %v", src)
	}

	// Profile preset: ultra_320k moves the bitrate only.
	cfg := baseConfig()
	cfg.Quality.VoiceProfile = "ultra_320k"
	eff, src = Resolve(cfg, model.ChatSettings{})
	if eff.BitrateBps != 320000 || eff.FrameSizeMs != 20 {
		t.Fatalf("ultra preset: %+v", eff)
	}
	if src[config.OpusFieldBitrateBps] != "profile" {
		t.Fatalf("ultra source = %q, want profile", src[config.OpusFieldBitrateBps])
	}

	// Explicit file keys beat the preset.
	cfg = baseConfig()
	cfg.Quality.VoiceProfile = "ultra_320k"
	cfg.Quality.OpusBitrateBps = intPtr(128000)
	cfg.Quality.OpusFrameMs = intPtr(40)
	cfg.Quality.OpusComplexity = intPtr(7)
	cfg.Quality.OpusVBR = boolPtr(false)
	eff, src = Resolve(cfg, model.ChatSettings{})
	if eff.BitrateBps != 128000 || eff.FrameSizeMs != 40 || eff.Complexity != 7 || eff.VBREnabled {
		t.Fatalf("explicit file keys lost: %+v", eff)
	}
	for _, f := range []string{config.OpusFieldBitrateBps, config.OpusFieldFrameMs, config.OpusFieldComplexity, config.OpusFieldVBR} {
		if src[f] != "config" {
			t.Fatalf("file key %s source = %q, want config", f, src[f])
		}
	}

	// Store override beats file keys, field by field: only
	// the overridden fields flip to "store".
	st := model.ChatSettings{QualityBitrateBps: intPtr(256000), QualityDTX: boolPtr(true)}
	eff, src = Resolve(cfg, st)
	if eff.BitrateBps != 256000 || !eff.DTXEnabled {
		t.Fatalf("store override lost: %+v", eff)
	}
	if eff.FrameSizeMs != 40 || eff.Complexity != 7 || eff.VBREnabled {
		t.Fatalf("store override must not touch other fields: %+v", eff)
	}
	if src[config.OpusFieldBitrateBps] != "store" || src[config.OpusFieldDTX] != "store" {
		t.Fatalf("store sources wrong: %v", src)
	}
	if src[config.OpusFieldFrameMs] != "config" {
		t.Fatalf("frame source = %q, want config", src[config.OpusFieldFrameMs])
	}

	// A nil store pointer is no override (falls through).
	st = model.ChatSettings{QualityFrameMs: intPtr(10)}
	eff, src = Resolve(baseConfig(), st)
	if eff.FrameSizeMs != 10 || eff.BitrateBps != 192000 {
		t.Fatalf("single store field: %+v", eff)
	}
	if src[config.OpusFieldFrameMs] != "store" || src[config.OpusFieldBitrateBps] != "profile" {
		t.Fatalf("single store field sources: %v", src)
	}
}

// TestConfigBoundsAgree guards the duplicated validation
// bounds: config keeps its own copies (it must stay
// cgo-free), and this test fails if they ever drift from
// voice's.
func TestConfigBoundsAgree(t *testing.T) {
	if config.OpusMinBitrateBps != voice.OpusMinBitrateBps ||
		config.OpusMaxBitrateBps != voice.OpusMaxBitrateBps ||
		config.OpusMinComplexity != voice.OpusMinComplexity ||
		config.OpusMaxComplexity != voice.OpusMaxComplexity {
		t.Fatalf("config opus bounds %+v drifted from voice %+v",
			[]int{config.OpusMinBitrateBps, config.OpusMaxBitrateBps, config.OpusMinComplexity, config.OpusMaxComplexity},
			[]int32{voice.OpusMinBitrateBps, voice.OpusMaxBitrateBps, voice.OpusMinComplexity, voice.OpusMaxComplexity})
	}
	for _, ms := range []int32{10, 20, 40, 60} {
		if !voice.ValidOpusFrameSize(ms) {
			t.Fatalf("frame %d must be valid", ms)
		}
	}
}

func TestApplyRoundTrip(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	cfg := baseConfig()
	cfg.Quality.OpusBitrateBps = intPtr(160000)
	cfg.Quality.OpusFrameMs = intPtr(20)
	eff, _, err := ApplyStartup(cfg)
	if err != nil {
		t.Fatalf("ApplyStartup: %v", err)
	}
	if eff.BitrateBps != 160000 || eff.FrameSizeMs != 20 {
		t.Fatalf("applied: %+v", eff)
	}
	got, err := voice.OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig: %v", err)
	}
	if got != eff {
		t.Fatalf("library reports %+v, want %+v (settings did not stick)", got, eff)
	}
}

func TestApplyForChatUsesStoreOverride(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	cfg := baseConfig()
	// No stored settings: the file-level config applies.
	if err := ApplyForChat(cfg, st, -100); err != nil {
		t.Fatalf("ApplyForChat (no override): %v", err)
	}
	if got, _ := voice.OpusConfig(); got.BitrateBps != 192000 || got.FrameSizeMs != 20 {
		t.Fatalf("no-override apply: %+v", got)
	}
	// A stored override wins for that chat.
	if err := st.PutSettings(-100, model.ChatSettings{QualityBitrateBps: intPtr(128000), QualityFrameMs: intPtr(40)}); err != nil {
		t.Fatalf("PutSettings: %v", err)
	}
	if err := ApplyForChat(cfg, st, -100); err != nil {
		t.Fatalf("ApplyForChat (override): %v", err)
	}
	if got, _ := voice.OpusConfig(); got.BitrateBps != 128000 || got.FrameSizeMs != 40 {
		t.Fatalf("override apply: %+v", got)
	}
}

func TestQualityGet(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	cfg := baseConfig()
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	defer client.Close()
	mgr := voice.NewManager(client, nil)
	svc := &Service{store: st, cfg: cfg, voice: mgr}

	call := func(payload string) map[string]any {
		t.Helper()
		data, err := svc.handleGet(context.Background(), uds.Request{Payload: json.RawMessage(payload)})
		if err != nil {
			t.Fatalf("quality.get %s: %v", payload, err)
		}
		return data.(map[string]any)
	}

	// Defaults: effective = profile + defaults, library =
	// whatever the process currently holds (Apply was not
	// run here, so they can differ — the test only pins the
	// effective half and the shape).
	m := call(`{"chat_id":-100}`)
	eff := m["effective"].(map[string]any)
	if eff["bitrate_bps"] != int32(192000) || eff["frame_ms"] != int32(20) {
		t.Fatalf("effective defaults: %v", eff)
	}
	if m["applies_on"] != "next join" {
		t.Fatalf("applies_on = %v, want \"next join\"", m["applies_on"])
	}
	if m["active_call"] != false {
		t.Fatalf("active_call = %v, want false (no call)", m["active_call"])
	}
	src := m["sources"].(map[string]string)
	if src[config.OpusFieldBitrateBps] != "profile" {
		t.Fatalf("sources: %v", src)
	}
	if _, ok := m["library"].(map[string]any); !ok {
		t.Fatalf("library readback missing: %v", m)
	}

	// A stored override shows up as effective with source store.
	if err := st.PutSettings(-100, model.ChatSettings{QualityBitrateBps: intPtr(256000)}); err != nil {
		t.Fatalf("PutSettings: %v", err)
	}
	m = call(`{"chat_id":-100}`)
	eff = m["effective"].(map[string]any)
	if eff["bitrate_bps"] != int32(256000) {
		t.Fatalf("effective with override: %v", eff)
	}
	src = m["sources"].(map[string]string)
	if src[config.OpusFieldBitrateBps] != "store" {
		t.Fatalf("sources with override: %v", src)
	}

	// Missing chat_id is a 400.
	if _, err := svc.handleGet(context.Background(), uds.Request{Payload: json.RawMessage(`{}`)}); err == nil {
		t.Fatal("quality.get without chat_id must fail")
	}
}

func TestRegisterPanicsOnNilDeps(t *testing.T) {
	server := uds.NewServer(t.TempDir() + "/s.sock")
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("Register with nil deps must panic")
		}
	}()
	Register(server, &app.Deps{})
}

func TestRegisterDoesNotPanic(t *testing.T) {
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	defer client.Close()
	mgr := voice.NewManager(client, nil)
	rooms := room.NewManager(st, 20)
	reg := resolver.NewRegistry()
	defer reg.Close()
	server := uds.NewServer(t.TempDir() + "/s.sock")
	deps := app.New(st, baseConfig(), reg, mgr, server, rooms, nil)
	Register(server, deps)
	if n := server.RegistrationCount("quality.get"); n != 1 {
		t.Fatalf("quality.get registered %d times, want 1", n)
	}
}
