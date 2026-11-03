// Package server is the HTTP IPC layer for vc-go, speaking the contract in
// contracts/ipc.json: envelopes {ok:true,data} / {ok:false,error:{code,
// message}}, X-Parlay-Token auth when PARLAY_TOKEN is set, and the
// vc.play/pause/resume/skip/stop/queue/seek/loop/shuffle/autoplay/
// autoplay-skip/now-playing actions.
package server

import (
	"encoding/json"
	"net/http"
	"strconv"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/resolver"
	"parlay/vc-go/internal/runner"
	"parlay/vc-go/internal/types"
)

// Server holds the service's dependencies.
type Server struct {
	mgr      *queue.Manager
	resolver *resolver.Resolver
	cfg      runner.Config
	token    string // empty = auth disabled (warned at startup)
	degraded bool   // true when yt-dlp is unavailable
	mux      *http.ServeMux
}

// New builds the HTTP server. token "" disables auth (allowed, with a
// startup warning); degraded reports the resolver state in /healthz.
func New(mgr *queue.Manager, res *resolver.Resolver, cfg runner.Config, token string) *Server {
	s := &Server{mgr: mgr, resolver: res, cfg: cfg, token: token, degraded: !res.Available()}
	s.mux = http.NewServeMux()
	s.mux.HandleFunc("/healthz", s.healthz)
	s.mux.HandleFunc("/v1/play", s.auth(s.handlePlay))
	s.mux.HandleFunc("/v1/pause", s.auth(s.handlePause))
	s.mux.HandleFunc("/v1/resume", s.auth(s.handleResume))
	s.mux.HandleFunc("/v1/skip", s.auth(s.handleSkip))
	s.mux.HandleFunc("/v1/stop", s.auth(s.handleStop))
	s.mux.HandleFunc("/v1/queue", s.auth(s.handleQueue))
	s.mux.HandleFunc("/v1/seek", s.auth(s.handleSeek))
	s.mux.HandleFunc("/v1/loop", s.auth(s.handleLoop))
	s.mux.HandleFunc("/v1/shuffle", s.auth(s.handleShuffle))
	s.mux.HandleFunc("/v1/autoplay", s.auth(s.handleAutoplay))
	s.mux.HandleFunc("/v1/autoplay-skip", s.auth(s.handleAutoplaySkip))
	s.mux.HandleFunc("/v1/now-playing", s.auth(s.handleNowPlaying))
	s.mux.HandleFunc("/v1/progress", s.auth(s.handleProgress))
	return s
}

// Handler returns the root HTTP handler.
func (s *Server) Handler() http.Handler { return s.mux }

// --- envelope helpers ---

func writeOK(w http.ResponseWriter, data any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(types.Envelope{OK: true, Data: data})
}

func writeErr(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(types.Envelope{
		OK: false, Error: &types.ErrorBody{Code: code, Message: message},
	})
}

func badRequest(w http.ResponseWriter, msg string) {
	writeErr(w, http.StatusBadRequest, "bad_request", msg)
}

func notFound(w http.ResponseWriter, msg string) {
	writeErr(w, http.StatusNotFound, "not_found", msg)
}

// auth enforces X-Parlay-Token when a token is configured. When
// PARLAY_TOKEN is empty the service allows all requests and logs a warning
// once at startup (see main.go).
func (s *Server) auth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if s.token != "" && r.Header.Get("X-Parlay-Token") != s.token {
			writeErr(w, http.StatusUnauthorized, "unauthorized", "bad or missing X-Parlay-Token")
			return
		}
		next(w, r)
	}
}

// --- request decoding ---

func decodeJSON(w http.ResponseWriter, r *http.Request, dst any) bool {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "POST required")
		return false
	}
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		badRequest(w, "invalid JSON body: "+err.Error())
		return false
	}
	return true
}

// chatOnly is the body for pause/resume/skip/stop.
type chatOnly struct {
	ChatID int64 `json:"chat_id"`
}

func (c chatOnly) valid() bool { return c.ChatID != 0 }

// --- handlers ---

// healthz is unauthenticated by design (load-balancer / supervisor probe).
// Extra fields beyond the contract's {ok, service} describe the engine
// state: profile, yt_dlp availability, degraded mode, buffer size.
func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "GET required")
		return
	}
	writeOK(w, map[string]any{
		"ok":        true,
		"service":   "vc-go",
		"profile":   string(s.cfg.Profile),
		"engine":    "go",
		"yt_dlp":    s.resolver.Available(),
		"degraded":  s.degraded,
		"buffer_kb": s.cfg.BufferKB,
		"prefetch":  s.cfg.Prefetchers > 0,
	})
}

type playRequest struct {
	ChatID      int64  `json:"chat_id"`
	Query       string `json:"query"`
	RequestedBy int64  `json:"requested_by"`
	Volume      *int   `json:"volume,omitempty"` // optional: 0-200
}

func (s *Server) handlePlay(w http.ResponseWriter, r *http.Request) {
	var req playRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	if req.Query == "" {
		badRequest(w, "query is required")
		return
	}
	track, err := s.resolver.Resolve(r.Context(), req.Query)
	if err != nil {
		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
		return
	}
	room := s.mgr.Get(req.ChatID)
	np := room.Play(*track, req.RequestedBy)
	if req.Volume != nil {
		np = room.SetVolume(*req.Volume)
	}
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handlePause(w http.ResponseWriter, r *http.Request) {
	var req chatOnly
	if !decodeJSON(w, r, &req) {
		return
	}
	if !req.valid() {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil || room.Current() == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np := room.Pause()
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleResume(w http.ResponseWriter, r *http.Request) {
	var req chatOnly
	if !decodeJSON(w, r, &req) {
		return
	}
	if !req.valid() {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil || room.Current() == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np := room.Resume()
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleSkip(w http.ResponseWriter, r *http.Request) {
	var req chatOnly
	if !decodeJSON(w, r, &req) {
		return
	}
	if !req.valid() {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np, ok := room.Skip()
	if !ok {
		notFound(w, "nothing playing in this chat")
		return
	}
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleStop(w http.ResponseWriter, r *http.Request) {
	var req chatOnly
	if !decodeJSON(w, r, &req) {
		return
	}
	if !req.valid() {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room != nil {
		room.Stop()
		s.mgr.Changed()
	}
	writeOK(w, map[string]any{})
}

type queueRequest struct {
	Action      string       `json:"action"` // add|remove|clear|list
	ChatID      int64        `json:"chat_id"`
	Position    *int         `json:"position,omitempty"`
	Track       *types.Track `json:"track,omitempty"`
	RequestedBy int64        `json:"requested_by"`
}

func (s *Server) handleQueue(w http.ResponseWriter, r *http.Request) {
	var req queueRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	switch req.Action {
	case "list":
		room := s.mgr.Lookup(req.ChatID)
		if room == nil {
			writeOK(w, map[string]any{"queue": []types.QueueItem{}})
			return
		}
		writeOK(w, map[string]any{"queue": room.Snapshot().Queue})
	case "add":
		if req.Track == nil || req.Track.ID == "" || req.Track.Title == "" {
			badRequest(w, "track with id and title is required for add")
			return
		}
		if req.Track.Source == "" {
			req.Track.Source = "youtube"
		}
		room := s.mgr.Get(req.ChatID)
		np := room.Enqueue(*req.Track, req.RequestedBy)
		s.mgr.Changed()
		writeOK(w, map[string]any{"queue": np.Queue})
	case "remove":
		if req.Position == nil {
			badRequest(w, "position is required for remove")
			return
		}
		room := s.mgr.Lookup(req.ChatID)
		if room == nil {
			notFound(w, "no queue in this chat")
			return
		}
		np, ok := room.Remove(*req.Position)
		if !ok {
			badRequest(w, "position out of range")
			return
		}
		s.mgr.Changed()
		writeOK(w, map[string]any{"queue": np.Queue})
	case "clear":
		room := s.mgr.Lookup(req.ChatID)
		if room != nil {
			room.Clear()
			s.mgr.Changed()
		}
		writeOK(w, map[string]any{"queue": []types.QueueItem{}})
	default:
		badRequest(w, "action must be add|remove|clear|list")
	}
}

type seekRequest struct {
	ChatID      int64 `json:"chat_id"`
	PositionSec int64 `json:"position_sec"`
}

func (s *Server) handleSeek(w http.ResponseWriter, r *http.Request) {
	var req seekRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np, ok := room.SeekTo(req.PositionSec)
	if !ok {
		notFound(w, "nothing playing in this chat")
		return
	}
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

type loopRequest struct {
	ChatID int64 `json:"chat_id"`
	Count  int   `json:"count"` // 0-10, 0 = off
}

func (s *Server) handleLoop(w http.ResponseWriter, r *http.Request) {
	var req loopRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	if req.Count < 0 || req.Count > queue.MaxLoop {
		badRequest(w, "count must be 0-10")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil || room.Current() == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np := room.SetLoop(req.Count)
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

type toggleRequest struct {
	ChatID  int64 `json:"chat_id"`
	Enabled bool  `json:"enabled"`
}

func (s *Server) handleShuffle(w http.ResponseWriter, r *http.Request) {
	var req toggleRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil || room.Current() == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np := room.SetShuffle(req.Enabled)
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleAutoplay(w http.ResponseWriter, r *http.Request) {
	var req toggleRequest
	if !decodeJSON(w, r, &req) {
		return
	}
	if req.ChatID == 0 {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil || room.Current() == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np := room.SetAutoplay(req.Enabled)
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleAutoplaySkip(w http.ResponseWriter, r *http.Request) {
	var req chatOnly
	if !decodeJSON(w, r, &req) {
		return
	}
	if !req.valid() {
		badRequest(w, "chat_id is required")
		return
	}
	room := s.mgr.Lookup(req.ChatID)
	if room == nil {
		notFound(w, "nothing playing in this chat")
		return
	}
	np, ok := room.AutoplaySkip()
	if !ok {
		notFound(w, "nothing playing in this chat")
		return
	}
	s.mgr.Changed()
	writeOK(w, map[string]any{"now_playing": np})
}

func (s *Server) handleNowPlaying(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "GET required")
		return
	}
	raw := r.URL.Query().Get("chat_id")
	chatID, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || chatID == 0 {
		badRequest(w, "chat_id query parameter is required")
		return
	}
	room := s.mgr.Lookup(chatID)
	if room == nil {
		notFound(w, "no playback state for this chat")
		return
	}
	np := room.Snapshot()
	if np.Track == nil && len(np.Queue) == 0 {
		notFound(w, "no playback state for this chat")
		return
	}
	writeOK(w, map[string]any{"now_playing": np})
}

// handleProgress serves IPC-2 (UI_INVENTORY.md §29): GET
// /v1/progress?chat_id= — the snapshot the core's no-op progress-bar
// button refresh and the remix /menu panel read. It is a read-only
// view over the room snapshot: position, duration, pause/mute/loop
// state, and the current + next tracks (nil when there is none).
// Same 404 rule as /v1/now-playing: no room, or a room with nothing
// playing and an empty queue.
func (s *Server) handleProgress(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "GET required")
		return
	}
	raw := r.URL.Query().Get("chat_id")
	chatID, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || chatID == 0 {
		badRequest(w, "chat_id query parameter is required")
		return
	}
	room := s.mgr.Lookup(chatID)
	if room == nil {
		notFound(w, "no playback state for this chat")
		return
	}
	np := room.Snapshot()
	if np.Track == nil && len(np.Queue) == 0 {
		notFound(w, "no playback state for this chat")
		return
	}
	var duration int64
	if np.Track != nil {
		duration = np.Track.DurationSec
	}
	var next *types.Track
	if len(np.Queue) > 0 {
		t := np.Queue[0].Track
		next = &t
	}
	writeOK(w, map[string]any{
		"position_sec": np.PositionSec,
		"duration_sec": duration,
		"is_paused":    np.Paused,
		"is_muted":     np.IsMuted,
		"loop_enabled": np.Loop > 0,
		"now":          np.Track,
		"next":         next,
	})
}
