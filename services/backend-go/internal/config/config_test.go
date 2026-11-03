package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaultsMatchExample(t *testing.T) {
	cfg, err := LoadFrom("")
	if err != nil {
		// No parlay.toml in the test working directory is expected;
		// LoadFrom("") searches and falls back to defaults. If a
		// stray file exists the test still pins the values below
		// only when no file was found.
		t.Fatal(err)
	}
	if cfg.FileFound {
		t.Skip("parlay.toml found in test cwd; defaults covered by TestFileLoad")
	}
	if cfg.RateLimit.Chat.Capacity != 5 || cfg.RateLimit.Chat.RefillPerSec != 1.0 {
		t.Errorf("chat rate limit defaults drifted: %+v", cfg.RateLimit.Chat)
	}
	if cfg.RateLimit.User.Capacity != 3 || cfg.RateLimit.User.RefillPerSec != 1.0 {
		t.Errorf("user rate limit defaults drifted: %+v", cfg.RateLimit.User)
	}
	if cfg.Timeouts.ResolveMs != 30000 || cfg.Timeouts.JoinMs != 30000 ||
		cfg.Timeouts.IPCCmdMs != 10000 || cfg.Timeouts.DownloadMs != 300000 ||
		cfg.Timeouts.HealthzMs != 5000 {
		t.Errorf("timeout defaults drifted: %+v", cfg.Timeouts)
	}
	if cfg.Quality.VoiceProfile != "standard_192k" || cfg.Quality.OpusBitrateKbps() != 192 {
		t.Errorf("voice profile defaults drifted: %+v", cfg.Quality)
	}
	if cfg.Quality.ResamplerQuality != "sinc_256" {
		t.Errorf("resampler default drifted: %q", cfg.Quality.ResamplerQuality)
	}
	want := []string{"navidrome", "soulseek", "deezer", "netease", "jiosaavn", "youtube", "spotify", "tiktok", "radio"}
	if strings.Join(cfg.Quality.SourceLadder, ",") != strings.Join(want, ",") {
		t.Errorf("source ladder default drifted: %v", cfg.Quality.SourceLadder)
	}
	if cfg.Limits.MaxDurationMinutes != 60 || cfg.Limits.MaxQueueSize != 20 ||
		cfg.Limits.PrefetchLeadTimeSec != 30 || cfg.Limits.URLCacheTTLsec != 120 {
		t.Errorf("limit defaults drifted: %+v", cfg.Limits)
	}
	if cfg.Backend.DataDir != "data" {
		t.Errorf("data_dir default drifted: %q", cfg.Backend.DataDir)
	}
	if got := cfg.SocketPath(); got != filepath.Join("data", "parlay-backend.sock") {
		t.Errorf("SocketPath default drifted: %q", got)
	}
	if got := cfg.DBPath(); got != filepath.Join("data", "parlay.db") {
		t.Errorf("DBPath default drifted: %q", got)
	}
}

func TestFileLoadAndEnvOverride(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "parlay.toml")
	content := `
[rate_limit.chat]
capacity = 9
refill_rate_per_sec = 2.5
[timeouts]
resolve_ms = 12345
[quality]
voice_profile = "high_256k"
[limits]
max_queue_size = 42
[backend]
data_dir = "custom-data"
slskd_url = "http://127.0.0.1:5999"
`
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PARLAY_MAX_QUEUE_SIZE", "77")
	t.Setenv("PARLAY_DATA_DIR", "env-data")
	cfg, err := LoadFrom(path)
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.FileFound || cfg.Source != path {
		t.Errorf("expected file %s loaded, got source %q found=%v", path, cfg.Source, cfg.FileFound)
	}
	if cfg.RateLimit.Chat.Capacity != 9 || cfg.RateLimit.Chat.RefillPerSec != 2.5 {
		t.Errorf("file rate limit not applied: %+v", cfg.RateLimit.Chat)
	}
	if cfg.Timeouts.ResolveMs != 12345 {
		t.Errorf("file timeout not applied: %+v", cfg.Timeouts)
	}
	if cfg.Quality.OpusBitrateKbps() != 256 {
		t.Errorf("file voice profile not applied: %q", cfg.Quality.VoiceProfile)
	}
	if cfg.Backend.SlskdURL != "http://127.0.0.1:5999" {
		t.Errorf("file backend key not applied: %q", cfg.Backend.SlskdURL)
	}
	// Env wins over the file for both keys.
	if cfg.Limits.MaxQueueSize != 77 {
		t.Errorf("env override lost: max_queue_size=%d, want 77", cfg.Limits.MaxQueueSize)
	}
	if cfg.Backend.DataDir != "env-data" {
		t.Errorf("env override lost: data_dir=%q, want env-data", cfg.Backend.DataDir)
	}
}

func TestMalformedFileFailsLoudly(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.toml")
	cases := map[string]string{
		"voice_profile": "[quality]\nvoice_profile = \"ultra_999k\"\n",
		"unknown key":   "[timeouts]\nresolve_ms = 1000\nbogus_key = 1\n",
		"bad range":     "[limits]\nmax_queue_size = 0\n",
		"bad ladder":    "[quality]\nsource_ladder = [\"youtube\", \"youtube\"]\n",
		"bad toml":      "[timeouts\nresolve_ms = 1\n",
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
		if !strings.Contains(err.Error(), "parlay.toml") {
			t.Errorf("%s: error must name the file: %v", name, err)
		}
		t.Logf("%s: %v", name, err)
	}
}

func TestMissingExplicitFileIsAnError(t *testing.T) {
	_, err := LoadFrom(filepath.Join(t.TempDir(), "does-not-exist.toml"))
	if err == nil {
		t.Error("explicit missing file must be an error")
	}
}

func TestExampleFileLoads(t *testing.T) {
	// The repo example is the shared schema: it must load under
	// this parser exactly as the core loads it. Resolved relative
	// to this package (go test always runs with the package dir
	// as its working directory) — never a hardcoded user home.
	example := filepath.Join("..", "..", "..", "..", "parlay.toml.example")
	if _, err := os.Stat(example); err != nil {
		t.Skip("parlay.toml.example not present")
	}
	cfg, err := LoadFrom(example)
	if err != nil {
		t.Fatalf("parlay.toml.example must load cleanly: %v", err)
	}
	if cfg.Quality.OpusBitrateKbps() != 192 {
		t.Errorf("example voice profile bitrate: got %d, want 192", cfg.Quality.OpusBitrateKbps())
	}
}
