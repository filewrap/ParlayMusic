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
// Frame path (Phase 5): audio reaches ntgcalls through the
// parlay-dsp engine (internal/dsp) — the DSP decodes the stream,
// applies the room's FX/speed/bass/volume, and the voice feed
// pushes 10 ms s16le frames over the external source. No ffmpeg
// subprocess exists anywhere in the path. The engine is linked
// only with -tags parlaydsp (Worker B's library); without the tag
// the build logs the honest-unavailable state and Play fails with
// dsp.ErrUnavailable instead of playing.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/download"
	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/fx"
	"parlay/backend-go/internal/gui"
	"parlay/backend-go/internal/handlers"
	"parlay/backend-go/internal/httpapi"
	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/netease"
	"parlay/backend-go/internal/quality"
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
	// The [backend] uds_path vs [ipc] socket_path split hazard
	// (Phase 4 §6 item 3): warn loudly at startup when the two
	// keys disagree — the core never reads [backend] uds_path, so
	// a split setup starts fine and then fails every IPC call.
	if w := cfg.SocketPathSplitWarning(); w != "" {
		log.Printf("backend-go: WARNING: %s", w)
	}

	st, err := store.Open(cfg.Backend.DataDir)
	if err != nil {
		return err
	}
	defer st.Close()
	log.Printf("backend-go: store open at %s", st.Path())

	reg := resolver.NewRegistry()
	// Phase 4: the [backend] file keys reach the resolvers (Q10),
	// and the TTL-aware resolved-track cache sits on the store
	// (canonical "<source>:<track_id>", expiry from the CDN
	// signature or limits.url_cache_ttl_sec).
	reg.Configure(cfg.Backend)
	reg.SetCache(resolver.NewCache(st, cfg.Limits.URLCacheTTLsec))
	// Phase 4: finished download runs, download prefs and the
	// library index persist in the store (new buckets), so a
	// restart keeps them. Each AttachStore is best-effort: a
	// failure is logged loudly and the backend keeps running
	// memory-only for that piece rather than refusing to start.
	if err := download.SharedRegistry().AttachStore(st); err != nil {
		log.Printf("backend-go: WARNING download runs not persisted (%v); finished runs are memory-only until restart", err)
	}
	if err := download.SharedPrefs().AttachStore(st); err != nil {
		log.Printf("backend-go: WARNING download prefs not persisted (%v); prefs are memory-only until restart", err)
	}
	if err := library.SharedIndex().AttachStore(st); err != nil {
		log.Printf("backend-go: WARNING library index not persisted (%v); the index is memory-only until restart", err)
	}
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
	// remote SDP; see contracts/uds-protocol.md §6). The connect
	// timeout is timeouts.join_ms: Connect is a stage of the
	// join, so the join budget is its cap (the join's context
	// bounds it earlier whenever the core set a shorter
	// deadline), and a Connect wedged inside the library returns
	// by the deadline instead of holding the join handler past
	// it (see Client.Connect).
	client, err := voice.NewWithConnectTimeout(time.Duration(cfg.Timeouts.JoinMs) * time.Millisecond)
	if err != nil {
		return fmt.Errorf("voice client: %w", err)
	}
	defer client.Close()
	log.Printf("backend-go: ntgcalls %s", voice.Version())

	// Opus encoder: apply the full effective config from
	// parlay.toml [quality] (explicit keys > voice_profile
	// preset > built-in default 192k/20ms) BEFORE the first
	// join. The settings are process-global and only affect
	// Opus encoders created after the call (see
	// internal/voice/opus.go), so this must run before the
	// UDS server listens and any join can happen. A failure
	// here is fatal: the encoder would otherwise run at the
	// library default 320k/60ms instead of the configured
	// profile, silently. The readback inside Apply proves
	// the setting stuck (it fails loudly on a mismatch).
	// Per-chat store overrides are applied by the pre-join
	// hook below, not here — there is no chat at startup.
	if _, _, err := quality.ApplyStartup(cfg); err != nil {
		return fmt.Errorf("opus config: %w", err)
	}

	server := uds.NewServer(cfg.SocketPath())
	voiceMgr := voice.NewManager(client, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return server.RequestJoin(ctx, chatID, localSDP)
	})
	// Pre-join Opus apply: before every join (before
	// CreateCall creates the encoder), resolve the effective
	// config for the joining chat — its stored quality_*
	// overrides over the file-level config — and apply it.
	// A hook error fails that join at stage "opus_config".
	voiceMgr.SetPreJoinHook(func(ctx context.Context, chatID int64) error {
		return quality.ApplyForChat(cfg, st, chatID)
	})
	// DSP engine: the real parlay-dsp library with -tags
	// parlaydsp, the honest-unavailable engine otherwise (see
	// internal/dsp). Startup continues either way — the voice
	// path fails honestly at Play time when unlinked — but the
	// state is logged loudly so an unlinked production build is
	// never mistaken for a working one.
	voiceMgr.SetDSP(dsp.New())
	if dsp.Linked() {
		log.Printf("backend-go: parlay-dsp linked")
	} else {
		log.Printf("backend-go: WARNING parlay-dsp NOT linked (build with -tags parlaydsp once services/parlay-dsp lands); audio will fail with dsp.ErrUnavailable")
	}

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

	// Phase 6: the HTTP server — Mini App bundle, /api/v1 state
	// + control (a transport into the same gui.* / vc.* actions
	// above), and the FLAC stream endpoint. It runs beside the
	// UDS server, never instead of it. An empty http_addr
	// disables it; a bind failure is logged loudly but does not
	// take the voice path down with it.
	httpSrv := httpapi.New(server, rooms, library.SharedIndex(), cfg.ResolveMiniAppDir())
	if cfg.Backend.HTTPAddr != "" {
		go func() {
			if err := httpSrv.Start(cfg.Backend.HTTPAddr); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Printf("backend-go: WARNING http server failed (%v); Mini App + /api/v1 unavailable", err)
			}
		}()
		log.Printf("backend-go: http api on %s (miniapp dir %q)", cfg.Backend.HTTPAddr, cfg.ResolveMiniAppDir())
	} else {
		log.Printf("backend-go: http api disabled (empty backend.http_addr)")
	}
	defer func() { _ = httpSrv.Close() }()

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
	quality.Register(server, deps)
}
