package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// allContractActions is the full 66-action surface: the 56 IPC
// actions of FINAL-ARCHITECTURE.md §10.1 plus the 9 OD-3 actions
// of phase3/actions-inventory.md §9 (E1–E7, E9, E12) plus
// quality.get (the Opus quality surface: effective-config read
// for /quality and the settings-panel quality section). It
// mirrors contracts/uds-protocol.md §4 rows 1–66 exactly; if
// the protocol table grows, this list must grow with it.
var allContractActions = []string{
	// 56 contract actions (§10.1).
	"chat.settings",
	"download.deezer", "download.navidrome", "download.progress",
	"download.soulseek", "download.spotify", "download.spotify_playlist",
	"download.tiktok", "download.youtube",
	"fx.apply",
	"gui.now_playing", "gui.queue",
	"lemonic.download", "lemonic.search",
	"netease.download", "netease.lyrics", "netease.search",
	"queue.vote",
	"radio.list", "radio.start", "radio.status", "radio.stop",
	"search.inline", "search.query",
	"share.create", "share.resolve",
	"spotify.analyze", "spotify.details", "spotify.auth", "spotify.dislike",
	"spotify.now_playing", "spotify.resolve", "spotify.skip_check",
	"spotify.watch", "spotify.watches", "spotify.collection", "spotify.status",
	"vc.join", "vc.lyrics", "vc.mute", "vc.now_playing", "vc.progress",
	"vc.panel", "vc.pause", "vc.play", "vc.queue", "vc.radio", "vc.resume",
	"vc.seek", "vc.skip", "vc.stop", "vc.loop", "vc.shuffle", "vc.autoplay",
	"vc.autoplay_skip", "vc.volume",
	// 9 OD-3 actions (§9).
	"download.cancel", "library.delete", "mix.generate", "audio.recognize",
	"library.scan", "library.sharescan", "cache.clear", "download.prefs",
	"vc.play_file",
	// 1 quality action (Opus quality surface).
	"quality.get",
}

// TestRegisterAllExactlyOnce runs the exact production wiring
// (registerAll, the same call main() makes) against a test server
// and proves every contract action is registered exactly once.
// uds.Server.Register overwrites silently on a duplicate, so the
// handler table alone cannot show a double registration — only
// RegistrationCount can, and this test fails on any count != 1
// (including the old download/handlers vc.play_file duplicate,
// which would have shown count 2).
func TestRegisterAllExactlyOnce(t *testing.T) {
	if len(allContractActions) != 66 {
		t.Fatalf("allContractActions has %d entries, want 66 — update it with contracts/uds-protocol.md §4", len(allContractActions))
	}
	dir := t.TempDir()
	tomlPath := filepath.Join(dir, "parlay.toml")
	if err := os.WriteFile(tomlPath, []byte("[backend]\ndata_dir = "+fmt.Sprintf("%q", dir)+"\n"), 0o600); err != nil {
		t.Fatalf("write parlay.toml: %v", err)
	}
	cfg, err := config.LoadFrom(tomlPath)
	if err != nil {
		t.Fatalf("config.LoadFrom: %v", err)
	}
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()
	reg := resolver.NewRegistry()
	defer reg.Close()
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	defer client.Close()
	sock := filepath.Join(dir, "parlay-backend.sock")
	server := uds.NewServer(sock)
	voiceMgr := voice.NewManager(client, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return server.RequestJoin(ctx, chatID, localSDP)
	})
	rooms := room.NewManager(st, cfg.Limits.MaxQueueSize)
	lyr, err := lyrics.New(reg)
	if err != nil {
		t.Fatalf("lyrics.New: %v", err)
	}
	deps := app.New(st, cfg, reg, voiceMgr, server, rooms, lyr)

	registerAll(server, deps)

	// Exactly 66 contract actions + the protocol-level ping.
	actions := server.Actions()
	if len(actions) != 67 {
		t.Fatalf("registered %d actions, want 67 (66 contract + ping): %v", len(actions), actions)
	}
	for _, action := range allContractActions {
		if n := server.RegistrationCount(action); n != 1 {
			t.Errorf("action %s registered %d times, want exactly 1", action, n)
		}
	}
	if n := server.RegistrationCount("ping"); n != 1 {
		t.Errorf("ping registered %d times, want exactly 1", n)
	}
	// No action outside the contract list may be registered.
	want := map[string]bool{"ping": true}
	for _, a := range allContractActions {
		want[a] = true
	}
	for _, a := range actions {
		if !want[a] {
			t.Errorf("unexpected registered action %q (not in the 66-action contract list)", a)
		}
	}
}
