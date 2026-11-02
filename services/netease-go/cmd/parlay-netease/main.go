// Command parlay-netease is the Parlay NetEase Cloud Music service:
// search, download and lyrics behind the IPC contract in
// contracts/ipc.json (netease.search/download/lyrics).
//
// Ported (re-implemented, not copied) from refs/Music163bot-Go
// (internal/bot/handlers_{search,music,lyric}.go, internal/netease).
// The upstream bot uses the encrypted Music163Api-Go library with an
// optional MUSIC_U login cookie; this service uses only the public
// anonymous endpoints, so no login or cookie is needed — and none is
// stored. Audio recognition (handlers_recognize.go) is NOT ported yet.
//
// Environment:
//
//	PORT           HTTP listen port (default 8010)
//	PARLAY_TOKEN   X-Parlay-Token auth value; empty = allow all (warns)
//	PARLAY_DL_DIR  download directory (default ./data)
package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"parlay/netease-go/internal/server"
)

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	log.SetPrefix("parlay-netease: ")
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)

	port := envOr("PORT", "8010")
	token := os.Getenv("PARLAY_TOKEN")
	dlDir := envOr("PARLAY_DL_DIR", "./data")

	if token == "" {
		log.Printf("WARNING: PARLAY_TOKEN is empty — X-Parlay-Token auth is DISABLED")
	}

	srv, err := server.New(dlDir, token)
	if err != nil {
		log.Fatalf("server: %v", err)
	}

	httpSrv := &http.Server{
		Addr:         ":" + port,
		Handler:      srv.Handler(),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 120 * time.Second, // downloads may be large
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		log.Printf("shutting down")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = httpSrv.Shutdown(ctx)
	}()

	log.Printf("listening on :%s (downloads -> %s)", port, dlDir)
	if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("listen: %v", err)
	}
	fmt.Println("parlay-netease stopped")
}
