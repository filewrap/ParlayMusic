package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// unsetEnv unsets an env var for the test, restoring it after.
func unsetEnv(t *testing.T, key string) {
	t.Helper()
	old, had := os.LookupEnv(key)
	if err := os.Unsetenv(key); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if had {
			_ = os.Setenv(key, old)
		}
	})
}

// TestBackendKeysQ10 proves the nine parsed-but-unused [backend]
// keys are resolved (wiring-audit Q10): the seven wireable keys
// load from the file and reach the resolvers via BackendConfig; the
// two keys with no consumer (slskd_user, slskd_pass) and the dead
// ytdlp_path are gone from the schema — a file that still sets
// them fails loudly instead of silently ignoring them.
func TestBackendKeysQ10(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "parlay.toml")
	content := `
[backend]
slskd_url = "http://127.0.0.1:5999"
deezer_arl = "file-arl"
netease_music_u = "file-music-u"
navidrome_url = "http://navi.test:4533"
navidrome_user = "file-user"
navidrome_pass = "file-pass"
`
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	// Clear every env override so the file values are what load.
	// (Unset, not set-empty: setStr overwrites the file value
	// with a set-but-empty env var.)
	for _, k := range []string{"SLSKD_URL", "DEEZER_ARL", "NETEASE_MUSIC_U",
		"NAVIDROME_URL", "NAVIDROME_USER", "NAVIDROME_PASSWORD", "NAVIDROME_PASS"} {
		unsetEnv(t, k)
	}
	cfg, err := LoadFrom(path)
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if cfg.Backend.SlskdURL != "http://127.0.0.1:5999" {
		t.Errorf("slskd_url = %q", cfg.Backend.SlskdURL)
	}
	if cfg.Backend.DeezerARL != "file-arl" {
		t.Errorf("deezer_arl = %q", cfg.Backend.DeezerARL)
	}
	if cfg.Backend.NetEaseMusicU != "file-music-u" {
		t.Errorf("netease_music_u = %q", cfg.Backend.NetEaseMusicU)
	}
	if cfg.Backend.NavidromeURL != "http://navi.test:4533" {
		t.Errorf("navidrome_url = %q", cfg.Backend.NavidromeURL)
	}
	if cfg.Backend.NavidromeUser != "file-user" {
		t.Errorf("navidrome_user = %q", cfg.Backend.NavidromeUser)
	}
	if cfg.Backend.NavidromePass != "file-pass" {
		t.Errorf("navidrome_pass = %q", cfg.Backend.NavidromePass)
	}

	// Deleted keys fail loudly (unknown-key error), never silently.
	for _, key := range []string{"slskd_user", "slskd_pass", "ytdlp_path"} {
		bad := filepath.Join(dir, "bad-"+key+".toml")
		if err := os.WriteFile(bad, []byte("[backend]\n"+key+" = \"x\"\n"), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := LoadFrom(bad); err == nil || !strings.Contains(err.Error(), key) {
			t.Errorf("file with %s must fail naming the key, got err=%v", key, err)
		}
	}
}

// TestNavidromePasswordPrecedence proves the Q10 name trap is
// resolved: NAVIDROME_PASSWORD is canonical and wins over the
// legacy NAVIDROME_PASS alias; the alias still works alone.
func TestNavidromePasswordPrecedence(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "parlay.toml")
	if err := os.WriteFile(path, []byte("[backend]\nnavidrome_pass = \"file-pass\"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("NAVIDROME_PASSWORD", "canonical-pass")
	t.Setenv("NAVIDROME_PASS", "alias-pass")
	cfg, err := LoadFrom(path)
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if cfg.Backend.NavidromePass != "canonical-pass" {
		t.Errorf("both set: NavidromePass = %q, want the canonical NAVIDROME_PASSWORD", cfg.Backend.NavidromePass)
	}

	t.Setenv("NAVIDROME_PASSWORD", "")
	cfg, err = LoadFrom(path)
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if cfg.Backend.NavidromePass != "alias-pass" {
		t.Errorf("alias only: NavidromePass = %q, want the legacy NAVIDROME_PASS", cfg.Backend.NavidromePass)
	}
}

// TestIPCSectionQ6 proves the core-owned [ipc] table (Q6, Phase 4)
// loads under the Go parser and is honoured by SocketPath: the
// backend must listen where the core connects. Precedence:
// PARLAY_UDS_PATH env > [backend] uds_path > [ipc] socket_path >
// <data_dir>/parlay-backend.sock. The core never reads [backend],
// so a path set only in [ipc] must still reach SocketPath —
// otherwise the two sides split.
func TestIPCSectionQ6(t *testing.T) {
	dir := t.TempDir()
	unsetEnv(t, "PARLAY_UDS_PATH")
	write := func(name, content string) string {
		t.Helper()
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		return p
	}

	// [ipc] alone sets the socket path.
	cfg, err := LoadFrom(write("ipc-only.toml", "[ipc]\nsocket_path = \"data/custom.sock\"\n"))
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if cfg.IPC.SocketPath != "data/custom.sock" {
		t.Errorf("[ipc] socket_path = %q", cfg.IPC.SocketPath)
	}
	if got := cfg.SocketPath(); got != "data/custom.sock" {
		t.Errorf("SocketPath with [ipc] only = %q, want data/custom.sock", got)
	}

	// [backend] uds_path wins over [ipc] socket_path.
	cfg, err = LoadFrom(write("both.toml", "[backend]\nuds_path = \"data/backend.sock\"\n[ipc]\nsocket_path = \"data/ipc.sock\"\n"))
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if got := cfg.SocketPath(); got != "data/backend.sock" {
		t.Errorf("SocketPath with both keys = %q, want data/backend.sock", got)
	}

	// Neither key: the default next to the state file.
	cfg, err = LoadFrom(write("neither.toml", "[backend]\ndata_dir = \"data\"\n"))
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if got := cfg.SocketPath(); got != filepath.Join("data", "parlay-backend.sock") {
		t.Errorf("SocketPath with neither key = %q, want data/parlay-backend.sock", got)
	}

	// Env wins over both file keys.
	t.Setenv("PARLAY_UDS_PATH", "data/env.sock")
	cfg, err = LoadFrom(write("env.toml", "[backend]\nuds_path = \"data/backend.sock\"\n[ipc]\nsocket_path = \"data/ipc.sock\"\n"))
	if err != nil {
		t.Fatalf("LoadFrom: %v", err)
	}
	if got := cfg.SocketPath(); got != "data/env.sock" {
		t.Errorf("SocketPath with env set = %q, want data/env.sock", got)
	}
}
