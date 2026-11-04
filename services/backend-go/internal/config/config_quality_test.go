package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// OpusSettings resolution: explicit [quality] keys win over the
// voice_profile preset, which wins over the built-in default
// (192000 bps, 20 ms, complexity 10, VBR on, FEC/DTX off,
// 48000 Hz). Sources name the winning level per field.

func TestOpusSettingsDefaults(t *testing.T) {
	cfg := defaults()
	s := cfg.Quality.OpusSettings()
	if s.BitrateBps != 192000 || s.FrameMs != 20 || s.Complexity != 10 ||
		!s.VBR || s.FEC || s.DTX || s.MaxPlaybackRateHz != 48000 {
		t.Fatalf("default OpusSettings drifted: %+v", s)
	}
	if s.Sources[OpusFieldBitrateBps] != "profile" {
		t.Errorf("default bitrate source = %q, want \"profile\" (standard_192k)", s.Sources[OpusFieldBitrateBps])
	}
	for _, f := range []string{OpusFieldComplexity, OpusFieldVBR, OpusFieldFEC, OpusFieldDTX, OpusFieldFrameMs} {
		if s.Sources[f] != "default" {
			t.Errorf("default %s source = %q, want \"default\"", f, s.Sources[f])
		}
	}
}

func TestOpusSettingsProfilePresets(t *testing.T) {
	for profile, want := range map[string]int{
		"ultra_320k":    320000,
		"high_256k":     256000,
		"standard_192k": 192000,
	} {
		cfg := defaults()
		cfg.Quality.VoiceProfile = profile
		s := cfg.Quality.OpusSettings()
		if s.BitrateBps != want {
			t.Errorf("profile %s: bitrate = %d, want %d", profile, s.BitrateBps, want)
		}
		if s.Sources[OpusFieldBitrateBps] != "profile" {
			t.Errorf("profile %s: bitrate source = %q, want \"profile\"", profile, s.Sources[OpusFieldBitrateBps])
		}
	}
}

func TestOpusSettingsExplicitKeysWin(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "parlay.toml")
	content := `[quality]
voice_profile = "ultra_320k"
opus_bitrate_bps = 128000
opus_frame_ms = 40
opus_complexity = 7
opus_vbr = false
opus_fec = true
opus_dtx = true
`
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadFrom(path)
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	// The preset is still ultra_320k, but every explicit key wins.
	if cfg.Quality.OpusBitrateKbps() != 320 {
		t.Errorf("profile bitrate = %d, want 320 (explicit keys do not change the preset)", cfg.Quality.OpusBitrateKbps())
	}
	s := cfg.Quality.OpusSettings()
	if s.BitrateBps != 128000 || s.FrameMs != 40 || s.Complexity != 7 ||
		s.VBR || !s.FEC || !s.DTX || s.MaxPlaybackRateHz != 48000 {
		t.Fatalf("explicit OpusSettings not applied: %+v", s)
	}
	for _, f := range []string{OpusFieldBitrateBps, OpusFieldComplexity, OpusFieldVBR, OpusFieldFEC, OpusFieldDTX, OpusFieldFrameMs} {
		if s.Sources[f] != "config" {
			t.Errorf("explicit %s source = %q, want \"config\"", f, s.Sources[f])
		}
	}
}

func TestOpusExplicitKeysValidation(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.toml")
	cases := map[string]string{
		"bitrate low":  "[quality]\nopus_bitrate_bps = 100\n",
		"bitrate high": "[quality]\nopus_bitrate_bps = 600000\n",
		"complexity":   "[quality]\nopus_complexity = 11\n",
		"frame 30":     "[quality]\nopus_frame_ms = 30\n",
		"frame 0":      "[quality]\nopus_frame_ms = 0\n",
	}
	for name, content := range cases {
		if err := os.WriteFile(bad, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		_, err := LoadFrom(bad)
		if err == nil {
			t.Errorf("%s: expected a loud failure, got none", name)
			continue
		}
		if !strings.Contains(err.Error(), "quality.opus_") {
			t.Errorf("%s: error must name the exact key: %v", name, err)
		}
		t.Logf("%s: %v", name, err)
	}
	// Edges are accepted.
	for _, content := range []string{
		"[quality]\nopus_bitrate_bps = 6000\n",
		"[quality]\nopus_bitrate_bps = 510000\n",
		"[quality]\nopus_complexity = 0\n",
		"[quality]\nopus_frame_ms = 10\n",
		"[quality]\nopus_frame_ms = 60\n",
	} {
		if err := os.WriteFile(bad, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := LoadFrom(bad); err != nil {
			t.Errorf("edge %q must load: %v", content, err)
		}
	}
}

func TestOpusSettingsExampleKeysLoad(t *testing.T) {
	// parlay.toml.example spells the explicit keys out (set to
	// the defaults); they must load and resolve with source
	// "config".
	example := filepath.Join("..", "..", "..", "..", "parlay.toml.example")
	if _, err := os.Stat(example); err != nil {
		t.Skip("parlay.toml.example not present")
	}
	cfg, err := LoadFrom(example)
	if err != nil {
		t.Fatalf("parlay.toml.example must load cleanly: %v", err)
	}
	s := cfg.Quality.OpusSettings()
	if s.BitrateBps != 192000 || s.FrameMs != 20 || s.Complexity != 10 ||
		!s.VBR || s.FEC || s.DTX {
		t.Fatalf("example OpusSettings drifted: %+v", s)
	}
	for _, f := range []string{OpusFieldBitrateBps, OpusFieldComplexity, OpusFieldVBR, OpusFieldFEC, OpusFieldDTX, OpusFieldFrameMs} {
		if s.Sources[f] != "config" {
			t.Errorf("example %s source = %q, want \"config\"", f, s.Sources[f])
		}
	}
}
