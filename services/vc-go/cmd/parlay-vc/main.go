// Command parlay-vc is the Parlay Go VC engine service: the queue /
// playback-state engine behind the IPC contract in contracts/ipc.json.
//
// Environment:
//
//	PORT            HTTP listen port (default 8001)
//	PARLAY_TOKEN    X-Parlay-Token auth value; empty = allow all (warns)
//	VC_PROFILE      low-ram | high-perf (default low-ram)
//	PARLAY_DATA_DIR queue persistence directory (default ./data)
//	MONGO_URI       when set, queues persist to MongoDB (database
//	                parlay, collection state, canonical keys
//	                kv:vc:room:<chat_id> — shared with vc-ts/vc-rust,
//	                see internal/store/canonical.go); when unset but
//	                REDIS_URL is set, queues persist to Redis
//	                (parlay:kv:vc:room:<chat_id>); when neither is set,
//	                the JSON file in PARLAY_DATA_DIR is used. A
//	                set-but-unreachable MONGO_URI/REDIS_URL is fatal at
//	                startup: the service never silently falls back to
//	                JSON (that would split-brain the queue across
//	                engines).
//
// Voice joining is NOT done here: the C++ core owns the ntgcalls voice
// connection through its VoiceCall interface. This service owns queue and
// playback state only, plus resolving the next track's stream URL.
package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/resolver"
	"parlay/vc-go/internal/runner"
	"parlay/vc-go/internal/server"
	"parlay/vc-go/internal/store"
	"parlay/vc-go/internal/types"
)

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	log.SetPrefix("parlay-vc: ")
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)

	port := envOr("PORT", "8001")
	token := os.Getenv("PARLAY_TOKEN")
	profile := runner.ParseProfile(os.Getenv("VC_PROFILE"))
	cfg := runner.ConfigFor(profile)
	dataDir := envOr("PARLAY_DATA_DIR", "./data")

	if token == "" {
		log.Printf("WARNING: PARLAY_TOKEN is empty — X-Parlay-Token auth is DISABLED")
	}

	res := resolver.New()
	if res.Available() {
		log.Printf("resolver: yt-dlp found at %s", res.YtDlp)
	} else {
		log.Printf("resolver: yt-dlp NOT found — resolve returns engine_unavailable " +
			"(no yt-dlp binary; set YT_DLP or install it; autoplay picks unavailable)")
	}
	log.Printf("profile: %s (prefetchers=%d, buffer=%dKB)",
		cfg.Profile, cfg.Prefetchers, cfg.BufferKB)

	// Persistence backend (A11): MongoDB when MONGO_URI is set, Redis
	// when only REDIS_URL is set (both fatal if unreachable), JSON file
	// otherwise. Same priority and key scheme as every other stateful
	// service (see internal/store/canonical.go).
	type persistBackend interface {
		Save([]queue.PersistedRoom) error
		Load() ([]queue.PersistedRoom, error)
		Describe() string
	}
	var st persistBackend
	var closer func()
	if mongoURI := os.Getenv("MONGO_URI"); mongoURI != "" {
		ms, err := store.NewMongo(context.Background(), mongoURI, "parlay")
		if err != nil {
			log.Fatalf("store: mongo: %v", err)
		}
		st = ms
		closer = func() {
			ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel2()
			_ = ms.Close(ctx2)
		}
		log.Printf("store: MongoDB persistence at %s", ms.Describe())
	} else if redisURL := os.Getenv("REDIS_URL"); redisURL != "" {
		rs, err := store.NewRedis(context.Background(), redisURL)
		if err != nil {
			log.Fatalf("store: redis: %v", err)
		}
		st = rs
		closer = func() { _ = rs.Close() }
		log.Printf("store: Redis persistence at %s", rs.Describe())
	} else {
		js, err := store.New(dataDir)
		if err != nil {
			log.Fatalf("store: %v", err)
		}
		st = js
		log.Printf("store: JSON persistence at %s (MONGO_URI and REDIS_URL unset)", js.Describe())
	}

	var mu sync.Mutex
	var mgr *queue.Manager
	save := func() {
		mu.Lock()
		defer mu.Unlock()
		rooms := mgr.Rooms()
		persisted := make([]queue.PersistedRoom, 0, len(rooms))
		for _, r := range rooms {
			if !r.IsIdle() {
				persisted = append(persisted, r.Persist())
			}
		}
		if err := st.Save(persisted); err != nil {
			log.Printf("store: save failed: %v", err)
		}
	}
	mgr = queue.NewManager(save)

	// Autoplay: when a room drains, pick the next track from the
	// YouTube mix of the last track (resolver.Related).
	mgr.SetAutoplayPicker(func(last types.Track, exclude map[string]bool) (*types.Track, error) {
		return res.Related(context.Background(), last, exclude)
	})

	// Restore previous state (rooms come back paused — never auto-resume).
	if persisted, err := st.Load(); err != nil {
		log.Printf("store: load failed (%v); starting empty", err)
	} else if len(persisted) > 0 {
		mgr.Load(persisted)
		log.Printf("store: restored %d room(s) from %s (paused)", len(persisted), st.Describe())
	}

	// Playback runner: advances finished tracks, prefetches in high-perf.
	run := runner.New(mgr, res, cfg)
	go run.Run()

	srv := server.New(mgr, res, cfg, token)
	httpSrv := &http.Server{
		Addr:         ":" + port,
		Handler:      srv.Handler(),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 35 * time.Second, // play may shell out to yt-dlp (30s cap)
	}

	// Graceful shutdown on SIGINT/SIGTERM.
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		log.Printf("shutting down")
		run.Stop()
		save() // final persist before exit
		if closer != nil {
			closer()
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = httpSrv.Shutdown(ctx)
	}()

	log.Printf("listening on :%s", port)
	if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("listen: %v", err)
	}
	fmt.Println("parlay-vc stopped")
}
