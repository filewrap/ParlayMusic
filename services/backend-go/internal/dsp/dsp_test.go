package dsp

import (
	"encoding/json"
	"testing"
)

func TestOptionsValidate(t *testing.T) {
	valid := []Options{
		{},
		{Speed: 1.0, Volume: 100},
		{Speed: 0.5, Bass: 0, Volume: 0, Effect: "original"},
		{Speed: 4.0, Bass: 100, Volume: 200, Effect: "nightcore", EffectParams: map[string]float64{"rate": 1.25}},
		{Speed: 0, Bass: 50, Volume: 0, Effect: "8d"},
	}
	for i, o := range valid {
		if err := o.Validate(); err != nil {
			t.Errorf("valid options %d (%+v): %v", i, o, err)
		}
	}
	invalid := []Options{
		{Speed: 0.4},
		{Speed: 4.1},
		{Speed: 99},
		{Bass: -1},
		{Bass: 101},
		{Volume: -1},
		{Volume: 201},
		{Effect: "reverb"},
		{Effect: "Nightcore"},
	}
	for i, o := range invalid {
		if err := o.Validate(); err == nil {
			t.Errorf("invalid options %d (%+v) passed validation", i, o)
		}
	}
}

func TestOptionsNormalized(t *testing.T) {
	o := Options{Speed: 0, Volume: 90, EffectParams: map[string]float64{"rate": 1.25}}.Normalized()
	if o.Speed != 1.0 || o.Volume != 90 {
		t.Errorf("normalized = %+v, want speed 1.0 volume 90", o)
	}
	// Volume 0 is silence, not "unset": it survives normalising.
	if got := (Options{Speed: 1.0, Volume: 0}.Normalized()).Volume; got != 0 {
		t.Errorf("normalized volume 0 = %d, want 0 (silence)", got)
	}
	// Normalised params are a copy: mutating the result must not
	// touch the source map.
	o.EffectParams["rate"] = 9
	src := Options{EffectParams: map[string]float64{"rate": 1.25}}.Normalized()
	if src.EffectParams["rate"] != 1.25 {
		t.Errorf("Normalized aliased the source params map")
	}
	if got := DefaultOptions(); got.Speed != 1.0 || got.Volume != 100 || got.Effect != "" {
		t.Errorf("DefaultOptions = %+v", got)
	}
}

func TestOptionsParamsJSON(t *testing.T) {
	o := Options{Speed: 1.5, Bass: 40, Volume: 90, Effect: "nightcore", EffectParams: map[string]float64{"rate": 1.25}}
	js, err := o.ParamsJSON()
	if err != nil {
		t.Fatalf("ParamsJSON: %v", err)
	}
	var back Options
	if err := json.Unmarshal(js, &back); err != nil {
		t.Fatalf("ParamsJSON is not valid JSON: %v", err)
	}
	if back.Speed != 1.5 || back.Bass != 40 || back.Volume != 90 || back.Effect != "nightcore" || back.EffectParams["rate"] != 1.25 {
		t.Errorf("ParamsJSON round-trip = %+v", back)
	}
}

func TestValidEffect(t *testing.T) {
	for _, e := range []string{"", "slow-reverb", "nightcore", "bass-boost", "8d", "original"} {
		if !ValidEffect(e) {
			t.Errorf("ValidEffect(%q) = false", e)
		}
	}
	if ValidEffect("echo") || ValidEffect("ORIGINAL") {
		t.Error("ValidEffect accepted an unknown effect")
	}
}

func TestPresetRate(t *testing.T) {
	if got := PresetRate("nightcore", nil); got != 1.25 {
		t.Errorf("nightcore rate = %v, want 1.25", got)
	}
	if got := PresetRate("slow-reverb", nil); got != 0.8 {
		t.Errorf("slow-reverb rate = %v, want 0.8", got)
	}
	if got := PresetRate("bass-boost", nil); got != 1.0 {
		t.Errorf("bass-boost rate = %v, want 1.0", got)
	}
	if got := PresetRate("original", nil); got != 1.0 {
		t.Errorf("original rate = %v, want 1.0", got)
	}
	// params["rate"] overrides the preset rate for nightcore and
	// slow-reverb only; a non-positive rate falls back.
	if got := PresetRate("nightcore", map[string]float64{"rate": 1.5}); got != 1.5 {
		t.Errorf("nightcore rate override = %v, want 1.5", got)
	}
	if got := PresetRate("nightcore", map[string]float64{"rate": 0}); got != 1.25 {
		t.Errorf("nightcore rate 0 = %v, want preset 1.25", got)
	}
	if got := PresetRate("bass-boost", map[string]float64{"rate": 1.5}); got != 1.0 {
		t.Errorf("bass-boost rate override = %v, want 1.0", got)
	}
}

func TestPresetBassDB(t *testing.T) {
	if got := PresetBassDB("bass-boost"); got != 10.0 {
		t.Errorf("bass-boost bass = %v dB, want 10", got)
	}
	if got := PresetBassDB("nightcore"); got != 0.0 {
		t.Errorf("nightcore bass = %v dB, want 0", got)
	}
}

func TestRoomBassDB(t *testing.T) {
	if got := RoomBassDB(0); got != 0.0 {
		t.Errorf("bass 0 = %v dB, want 0", got)
	}
	if got := RoomBassDB(100); got != 24.0 {
		t.Errorf("bass 100 = %v dB, want 24", got)
	}
	if got := RoomBassDB(50); got != 12.0 {
		t.Errorf("bass 50 = %v dB, want 12", got)
	}
}
