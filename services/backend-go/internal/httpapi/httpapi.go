// Package httpapi is the backend's HTTP server (Phase 6,
// FINAL-ARCHITECTURE.md §1.3 Part 2): it serves the TypeScript
// Mini App static bundle, the /api/v1 state + control endpoints,
// and the FLAC streaming endpoint. It invents no second API: the
// state and control endpoints are a transport into the exact
// gui.* / vc.* UDS actions the core uses (uds.Server.Dispatch),
// so a tap in the Mini App and the same command on Telegram run
// the same handler, the same per-chat lock, and the same voice
// coupling, and answer the same honest errors.
//
// AUTH GAP (documented, not hidden): there is no Telegram WebApp
// initData validation anywhere in-tree — the bot token lives in
// the core, not the backend, and no helper for it exists — so
// this server must NOT be exposed directly to the internet. The
// default listen address is loopback-only (127.0.0.1:8081); a
// non-loopback address is accepted only behind a reverse proxy
// the operator trusts, and Start logs a loud warning in that
// case. Real initData validation (HMAC over the bot token) is a
// live-test item for the VPS deployment, not something this
// package fakes with a home-grown scheme.
package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/library"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/uds"
)

// Server is the backend HTTP server. The zero value is useless;
// build one with New.
type Server struct {
	dispatch *uds.Server
	rooms    *room.Manager
	index    *library.Index
	miniApp  string // built Mini App bundle dir; "" = none found

	mux  *http.ServeMux
	http *http.Server
}

// New builds the server. dispatch is the UDS server the gui.* /
// vc.* handlers are registered on; rooms and index are the same
// singletons the handlers read (the room Manager owns live
// playback state, the library index owns the on-disk FLAC cache).
// miniApp is the built bundle directory (config.ResolveMiniAppDir);
// "" means no bundle was found and / answers an honest 503 while
// the API and stream endpoints keep working.
func New(dispatch *uds.Server, rooms *room.Manager, index *library.Index, miniApp string) *Server {
	if dispatch == nil || rooms == nil || index == nil {
		panic("httpapi: New called with nil deps")
	}
	s := &Server{dispatch: dispatch, rooms: rooms, index: index, miniApp: miniApp}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/healthz", s.handleHealthz)
	mux.HandleFunc("GET /api/v1/now-playing", s.handleAction("gui.now_playing"))
	mux.HandleFunc("GET /api/v1/queue", s.handleAction("gui.queue"))
	mux.HandleFunc("POST /api/v1/pause", s.handleAction("vc.pause"))
	mux.HandleFunc("POST /api/v1/resume", s.handleAction("vc.resume"))
	mux.HandleFunc("POST /api/v1/skip", s.handleAction("vc.skip"))
	mux.HandleFunc("POST /api/v1/volume", s.handleVolume)
	mux.HandleFunc("GET /api/v1/stream/{chat_id}/flac", s.handleStreamFLAC)
	mux.HandleFunc("GET /", s.handleStatic)
	s.mux = mux
	return s
}

// Handler returns the server's handler (tests serve it with
// httptest; Start wraps it in an http.Server).
func (s *Server) Handler() http.Handler { return s.mux }

// Start listens on addr and serves until Close or a fatal listen
// error. A non-loopback address logs a loud warning: the HTTP
// surface has no initData validation (see the package doc) and
// must sit behind a trusted reverse proxy in that case.
func (s *Server) Start(addr string) error {
	if host, _, err := net.SplitHostPort(addr); err == nil && host != "" {
		if ip := net.ParseIP(host); ip == nil || !ip.IsLoopback() {
			if host != "localhost" {
				log.Printf("httpapi: WARNING listening on non-loopback %s with NO Telegram WebApp initData validation in-tree; put this behind a trusted reverse proxy", addr)
			}
		}
	}
	s.http = &http.Server{
		Addr:              addr,
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
		// No WriteTimeout: a FLAC stream of a long track
		// legitimately holds the connection open for minutes.
	}
	log.Printf("httpapi: listening on %s (miniapp dir %q)", addr, s.miniApp)
	return s.http.ListenAndServe()
}

// Close shuts the server down gracefully.
func (s *Server) Close() error {
	if s.http == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.http.Shutdown(ctx)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]any{"ok": false, "error": msg, "code": code})
}

func writeHandlerError(w http.ResponseWriter, err error) {
	var herr *uds.HandlerError
	if errors.As(err, &herr) {
		writeError(w, httpStatus(herr.Code), string(herr.Code), herr.Message)
		return
	}
	writeError(w, http.StatusInternalServerError, string(uds.ErrInternal), err.Error())
}

// httpStatus maps a UDS error code to the HTTP status the core
// maps it to (contracts/uds-protocol.md §8).
func httpStatus(code uds.ErrorCode) int {
	switch code {
	case uds.ErrBadRequest:
		return http.StatusBadRequest
	case uds.ErrUnauthorized:
		return http.StatusUnauthorized
	case uds.ErrNotFound:
		return http.StatusNotFound
	case uds.ErrEngineUnavailable, uds.ErrDownloadFailed:
		return http.StatusBadGateway
	case uds.ErrRateLimited:
		return http.StatusTooManyRequests
	default:
		return http.StatusInternalServerError
	}
}

// parseChatID reads chat_id from the query string (GET) or the
// JSON body (POST). It reports the same "chat_id is required"
// failure the gui.* handlers report for a missing id.
func parseChatID(w http.ResponseWriter, r *http.Request) (int64, error) {
	if r.Method == http.MethodGet {
		raw := strings.TrimSpace(r.URL.Query().Get("chat_id"))
		if raw == "" {
			return 0, fmt.Errorf("chat_id is required")
		}
		id, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || id == 0 {
			return 0, fmt.Errorf("chat_id is required")
		}
		return id, nil
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
	if err != nil {
		return 0, fmt.Errorf("invalid request body: %v", err)
	}
	var p struct {
		ChatID int64 `json:"chat_id"`
	}
	if err := json.Unmarshal(body, &p); err != nil {
		return 0, fmt.Errorf("invalid request body: %v", err)
	}
	if p.ChatID == 0 {
		return 0, fmt.Errorf("chat_id is required")
	}
	return p.ChatID, nil
}

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "backend-go", "version": uds.Version})
}

// handleAction serves a read or control endpoint by dispatching
// the named UDS action with a {"chat_id": ...} payload — the
// exact action the core invokes over the socket. The response
// data is written verbatim: the Mini App sees the same shape
// Telegram commands see ({"now_playing": ...}, {"queue": ...}).
func (s *Server) handleAction(action string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		chatID, err := parseChatID(w, r)
		if err != nil {
			writeError(w, http.StatusBadRequest, string(uds.ErrBadRequest), err.Error())
			return
		}
		payload, _ := json.Marshal(map[string]any{"chat_id": chatID})
		data, err := s.dispatch.Dispatch(r.Context(), uds.Request{
			Action: action, ChatID: chatID, HasChatID: true, Payload: payload,
		})
		if err != nil {
			writeHandlerError(w, err)
			return
		}
		writeJSON(w, http.StatusOK, data)
	}
}

// handleVolume dispatches vc.volume, which needs the volume in
// the payload alongside chat_id (the action's own validation —
// range 0-200, required — applies unchanged).
func (s *Server) handleVolume(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
	if err != nil {
		writeError(w, http.StatusBadRequest, string(uds.ErrBadRequest), fmt.Sprintf("invalid request body: %v", err))
		return
	}
	var p struct {
		ChatID int64 `json:"chat_id"`
		Volume *int  `json:"volume"`
	}
	if err := json.Unmarshal(body, &p); err != nil {
		writeError(w, http.StatusBadRequest, string(uds.ErrBadRequest), fmt.Sprintf("invalid request body: %v", err))
		return
	}
	if p.ChatID == 0 {
		writeError(w, http.StatusBadRequest, string(uds.ErrBadRequest), "chat_id is required")
		return
	}
	payload, _ := json.Marshal(map[string]any{"chat_id": p.ChatID, "volume": p.Volume})
	data, err := s.dispatch.Dispatch(r.Context(), uds.Request{
		Action: "vc.volume", ChatID: p.ChatID, HasChatID: true, Payload: payload,
	})
	if err != nil {
		writeHandlerError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, data)
}

// handleStatic serves the built Mini App bundle. The bundle is
// static files only (index.html + app.js + styles.css); a missing
// bundle is an honest 503 naming the build step, never demo data.
func (s *Server) handleStatic(w http.ResponseWriter, r *http.Request) {
	if s.miniApp == "" {
		writeError(w, http.StatusServiceUnavailable, "miniapp_not_built", "mini app bundle not built: run services/miniapp/build.sh, then restart the backend")
		return
	}
	// Never serve a stale player: the bundle is tiny and the
	// Mini App re-fetches on every open.
	w.Header().Set("Cache-Control", "no-cache")
	http.FileServer(http.Dir(s.miniApp)).ServeHTTP(w, r)
}
