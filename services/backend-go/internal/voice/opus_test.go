package voice

import (
	"testing"
)

// saveOpusConfig snapshots the process-global encoder settings
// so a test that changes them can restore them afterwards.
// The settings are process-global (see opus.go); tests must not
// leak a changed bitrate into other tests.
func saveOpusConfig(t *testing.T) (restore func()) {
	t.Helper()
	cur, err := OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig (save): %v", err)
	}
	return func() {
		if err := SetOpusConfig(cur); err != nil {
			t.Errorf("OpusConfig (restore): %v", err)
		}
	}
}

func TestOpusDefaults(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	// Apply the locked defaults, then read them back: the
	// round-trip proves set_config/get_config agree, and the
	// values are the locked profile (320k, complexity 10, VBR
	// on, FEC/DTX off, 60 ms, 48 kHz fullband).
	if err := SetOpusConfig(DefaultOpusEncoderConfig()); err != nil {
		t.Fatalf("SetOpusConfig(defaults): %v", err)
	}
	got, err := OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig: %v", err)
	}
	want := DefaultOpusEncoderConfig()
	if got != want {
		t.Errorf("OpusConfig = %+v, want %+v", got, want)
	}
}

func TestOpusSetBitrate(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	if err := SetOpusConfig(DefaultOpusEncoderConfig()); err != nil {
		t.Fatalf("SetOpusConfig(defaults): %v", err)
	}
	// The ladder step: only the bitrate changes, everything
	// else stays at the locked defaults.
	if err := SetOpusBitrate(256000); err != nil {
		t.Fatalf("SetOpusBitrate(256000): %v", err)
	}
	got, err := OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig: %v", err)
	}
	if got.BitrateBps != 256000 {
		t.Errorf("bitrate = %d, want 256000", got.BitrateBps)
	}
	if got.Complexity != 10 || !got.VBREnabled || got.FECEnabled || got.DTXEnabled || got.FrameSizeMs != 60 || got.MaxPlaybackRateHz != 48000 {
		t.Errorf("other settings changed by set_bitrate: %+v", got)
	}
	// An out-of-range bitrate is rejected and leaves the stored
	// value untouched.
	if err := SetOpusBitrate(100); err == nil {
		t.Error("SetOpusBitrate(100) succeeded, want rejection")
	}
	if got, _ := OpusConfig(); got.BitrateBps != 256000 {
		t.Errorf("bitrate after rejected set = %d, want 256000", got.BitrateBps)
	}
}

func TestOpusSetConfig(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	// Full-config set: the standard_192k profile values.
	cfg := OpusEncoderConfig{BitrateBps: 192000, Complexity: 10, VBREnabled: true, FECEnabled: false, DTXEnabled: false, FrameSizeMs: 60, MaxPlaybackRateHz: 48000}
	if err := SetOpusConfig(cfg); err != nil {
		t.Fatalf("SetOpusConfig(192k): %v", err)
	}
	got, err := OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig: %v", err)
	}
	if got != cfg {
		t.Errorf("OpusConfig = %+v, want %+v", got, cfg)
	}
	// Rejected values: the stored config must not change.
	for _, bad := range []OpusEncoderConfig{
		{BitrateBps: 100, Complexity: 10, VBREnabled: true, FrameSizeMs: 60, MaxPlaybackRateHz: 48000},
		{BitrateBps: 192000, Complexity: 99, VBREnabled: true, FrameSizeMs: 60, MaxPlaybackRateHz: 48000},
		{BitrateBps: 192000, Complexity: 10, VBREnabled: true, FrameSizeMs: 30, MaxPlaybackRateHz: 48000},
		{BitrateBps: 192000, Complexity: 10, VBREnabled: true, FrameSizeMs: 60, MaxPlaybackRateHz: 1000},
	} {
		if err := SetOpusConfig(bad); err == nil {
			t.Errorf("SetOpusConfig(%+v) succeeded, want rejection", bad)
		}
	}
	if got, _ := OpusConfig(); got != cfg {
		t.Errorf("config after rejected sets = %+v, want %+v", got, cfg)
	}
}

func TestOpusValidationEdges(t *testing.T) {
	restore := saveOpusConfig(t)
	defer restore()
	// Valid edges are accepted by the library (restored after).
	for _, bps := range []int32{6000, 510000} {
		if err := SetOpusBitrate(bps); err != nil {
			t.Errorf("SetOpusBitrate(%d): %v", bps, err)
		}
	}
	// Invalid edges are rejected Go-side, before any C call.
	for _, bps := range []int32{5999, 510001, 0, -1} {
		if err := SetOpusBitrate(bps); err == nil {
			t.Errorf("SetOpusBitrate(%d) succeeded, want rejection", bps)
		}
	}
	for _, ms := range []int32{0, 10, 20, 40, 60} {
		cfg := DefaultOpusEncoderConfig()
		cfg.FrameSizeMs = ms
		err := SetOpusConfig(cfg)
		if ms == 0 {
			if err == nil {
				t.Error("SetOpusConfig(frame 0) succeeded, want rejection")
			}
		} else if err != nil {
			t.Errorf("SetOpusConfig(frame %d): %v", ms, err)
		}
	}
}
