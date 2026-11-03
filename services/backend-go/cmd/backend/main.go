// Command backend is the unified Go backend (FINAL-ARCHITECTURE.md
// §1.3 Part 2): config load -> embedded store open -> resolver
// registry -> yt-dlp daemon warm -> voice -> UDS listen. It owns
// playback/queue/vote/share/Spotify/settings/radio state in the
// embedded bbolt store and answers the core over the persistent
// UDS socket.
//
// Wiring order matters: the room Manager and the lyrics Fetcher
// are constructed exactly once here and passed into app.Deps, so
// every feature package shares the one Manager and the one
// Fetcher (no second owner of room state, no second lyrics
// implementation). vc.play_file is registered exactly once, by
// handlers.Register (record-only per inventory §3.3); the
// download package's feed-binding duplicate was deleted in the
// Wave 2 verification pass. The autoplay picker is installed by
// handlers.Register before Listen in every order — any package
// that reads d.Room before that sees rooms with no picker
// (autoplay drains go idle, never wrong).
//
// Frame path (interim, Phase 5 replacement target): audio reaches
// ntgcalls through voice.Manager.Play's ffmpeg-decode-to-PCM path
// (internal/voice/feeder.go — one persistent ffmpeg process per
// room, 10 ms s16le frames over the external source). Phase 5
// replaces it with parlay-dsp frames (f32 -> FX chain -> one
// dither to s16le -> SendExternalFrame). No other frame path is
// invented here.
package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/download"
	"parlay/backend-go/internal/fx"
	"parlay/backend-go/internal/gui"
	"parlay/backend-go/internal/handlers"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/netease"
	"parlay/backend-go/internal/radio"
	"parlay/backend-go/internal/recognize"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/search"
	"parlay/backend-go/internal/settings"
	"parlay/backend-go/internal/share"
	"parlay/backend-go/internal/spotify"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
	"parlay/backend-go/internal/vote"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "backend-go:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	log.Printf("backend-go: config from %s", cfg.Source)
	log.Printf("backend-go: data_dir=%s opus=%dkbps ladder=%v", cfg.Backend.DataDir, cfg.Quality.OpusBitrateKbps(), cfg.Quality.SourceLadder)

	st, err := store.Open(cfg.Backend.DataDir)
	if err != nil {
		return err
	}
	defer st.Close()
	log.Printf("backend-go: store open at %s", st.Path())

	reg := resolver.NewRegistry()
	defer func() {
		if err := reg.Close(); err != nil {
			log.Printf("backend-go: registry close: %v", err)
		}
	}()
	// Warm the yt-dlp daemon at boot so the FIRST resolve never
	// forks: Ping starts the daemon (ensureStarted) and verifies
	// it answers. A failure is logged loudly and startup
	// CONTINUES — the native resolvers (InnerTube, JioSaavn,
	// NetEase, radio) still work, and yt-dlp is only the
	// fallback ladder. Note the daemon client's sticky failure:
	// once ensureStarted fails it never retries in-process, so a
	// failed warm means yt-dlp stays down until restart.
	warmCtx, warmCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer warmCancel()
	if err := reg.YtDlp().Ping(warmCtx); err != nil {
		log.Printf("backend-go: WARNING yt-dlp daemon warm failed (%v); native resolvers only until restart", err)
	} else if v, err := reg.YtDlp().Version(warmCtx); err != nil {
		log.Printf("backend-go: WARNING yt-dlp daemon pinged but version failed (%v)", err)
	} else {
		log.Printf("backend-go: yt-dlp daemon warm, version %s", v)
	}

	// Voice: the ntgcalls client, then the Manager whose join
	// handshake runs through the UDS server (the core runs
	// joinVideoChat on the assistant session and returns the
	// remote SDP; see contracts/uds-protocol.md §6).
	client, err := voice.New()
	if err != nil {
		return fmt.Errorf("voice client: %w", err)
	}
	defer client.Close()
	log.Printf("backend-go: ntgcalls %s", voice.Version())

	server := uds.NewServer(cfg.SocketPath())
	voiceMgr := voice.NewManager(client, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return server.RequestJoin(ctx, chatID, localSDP)
	})

	// The single shared room Manager and lyrics Fetcher. The
	// Fetcher is non-critical: if it cannot be built from the
	// registry, startup continues with a nil Lyrics and vc.lyrics
	// answers engine_unavailable.
	rooms := room.NewManager(st, cfg.Limits.MaxQueueSize)
	lyr, err := lyrics.New(reg)
	if err != nil {
		log.Printf("backend-go: WARNING lyrics fetcher not built (%v); vc.lyrics will answer engine_unavailable", err)
		lyr = nil
	}

	deps := app.New(st, cfg, reg, voiceMgr, server, rooms, lyr)

	registerAll(server, deps)

	if err := server.Listen(); err != nil {
		return err
	}
	defer server.Close()
	log.Printf("backend-go: listening on %s", cfg.SocketPath())

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("backend-go: shutting down")
	return nil
}

// registerAll wires every feature package onto the server — the
// exact production registration sequence, extracted so the wiring
// test (main_test.go) can run it against a test server and prove
// every contract action is registered exactly once (see
// uds.Server.RegistrationCount: Register overwrites silently, so
// only the count can catch a duplicate).
func registerAll(server *uds.Server, deps *app.Deps) {
	// Feature packages, each Register(s, d) per the Wave 2
	// contract. Registration order is otherwise irrelevant:
	// nothing reads room state at registration time, and the
	// picker is installed by handlers.Register before Listen in
	// every order. Room, events, and lyrics expose no UDS actions
	// of their own and have no Register — their surface is vc.*
	// via handlers.
	download.Register(server, deps)
	handlers.Register(server, deps)
	library.Register(server, deps)
	search.Register(server, deps)
	recognize.Register(server, deps)
	spotify.Register(server, deps)
	share.Register(server, deps)
	vote.Register(server, deps)
	fx.Register(server, deps)
	settings.Register(server, deps)
	gui.Register(server, deps)
	radio.Register(server, deps)
	netease.Register(server, deps)
}
