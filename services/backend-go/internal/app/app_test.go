// Deps construction tests: New assembles exactly the instances
// it is given (no second Manager/Fetcher is constructed anywhere
// inside it), panics on any nil load-bearing dependency, and
// tolerates a nil Lyrics (lyrics not configured — vc.lyrics
// answers engine_unavailable in that case).
package app

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

func newTestParts(t *testing.T) (*store.Store, *config.Config, *resolver.Registry, *voice.Manager, *uds.Server, *room.Manager, *lyrics.Fetcher) {
	t.Helper()
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
	reg := resolver.NewRegistry()
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	t.Cleanup(func() {
		client.Close()
		_ = st.Close()
	})
	vm := voice.NewManager(client, nil)
	srv := uds.NewServer(filepath.Join(dir, "parlay-backend.sock"))
	rooms := room.NewManager(st, cfg.Limits.MaxQueueSize)
	lyr, err := lyrics.New(reg)
	if err != nil {
		t.Fatalf("lyrics.New: %v", err)
	}
	return st, cfg, reg, vm, srv, rooms, lyr
}

func TestNewAssemblesGivenInstances(t *testing.T) {
	st, cfg, reg, vm, srv, rooms, lyr := newTestParts(t)
	d := New(st, cfg, reg, vm, srv, rooms, lyr)
	if d.Store != st || d.Config != cfg || d.Registry != reg || d.Voice != vm || d.Server != srv || d.Room != rooms || d.Lyrics != lyr {
		t.Fatalf("New did not keep the given instances: %+v", d)
	}
}

func TestNewNilLyricsAllowed(t *testing.T) {
	st, cfg, reg, vm, srv, rooms, _ := newTestParts(t)
	d := New(st, cfg, reg, vm, srv, rooms, nil)
	if d.Lyrics != nil {
		t.Fatalf("New with nil Lyrics: Lyrics=%v, want nil", d.Lyrics)
	}
	if d.Room != rooms {
		t.Fatalf("New with nil Lyrics: Room not kept")
	}
}

func TestNewPanicsOnNil(t *testing.T) {
	st, cfg, reg, vm, srv, rooms, lyr := newTestParts(t)
	cases := []struct {
		name string
		call func()
	}{
		{"nil Store", func() { New(nil, cfg, reg, vm, srv, rooms, lyr) }},
		{"nil Config", func() { New(st, nil, reg, vm, srv, rooms, lyr) }},
		{"nil Registry", func() { New(st, cfg, nil, vm, srv, rooms, lyr) }},
		{"nil Voice", func() { New(st, cfg, reg, nil, srv, rooms, lyr) }},
		{"nil Server", func() { New(st, cfg, reg, vm, nil, rooms, lyr) }},
		{"nil Room", func() { New(st, cfg, reg, vm, srv, nil, lyr) }},
	}
	for _, c := range cases {
		func() {
			defer func() {
				if r := recover(); r == nil {
					t.Errorf("New(%s): want panic, got none", c.name)
				}
			}()
			c.call()
		}()
	}
}
