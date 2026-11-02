// Package server is the HTTP front-end for netease-go: the IPC surface
// from contracts/ipc.json (netease.search/download/lyrics) plus /healthz.
//
// Envelope: {ok:true,data:{...}} / {ok:false,error:{code,message}}.
// Auth: X-Parlay-Token must equal $PARLAY_TOKEN when the token is set;
// /healthz is unauthenticated by design (supervisor probe).
package server

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"

	"parlay/netease-go/internal/netease"
)

const maxBody = 1 << 20 // 1 MB request bodies

// Server owns the NetEase client, the download dir and the auth token.
type Server struct {
	mux    *http.ServeMux
	client *netease.Client
	dlDir  string
	token  string
}

// New wires the routes. dlDir is created if missing.
func New(dlDir, token string) (*Server, error) {
	if err := os.MkdirAll(dlDir, 0o755); err != nil {
		return nil, fmt.Errorf("download dir: %w", err)
	}
	s := &Server{client: netease.New(), dlDir: dlDir, token: token}
	s.mux = http.NewServeMux()
	s.mux.HandleFunc("/healthz", s.healthz)
	s.mux.HandleFunc("/v1/netease/search", s.auth(s.handleSearch))
	s.mux.HandleFunc("/v1/netease/download", s.auth(s.handleDownload))
	s.mux.HandleFunc("/v1/netease/lyrics", s.auth(s.handleLyrics))
	return s, nil
}

// Handler returns the mux.
func (s *Server) Handler() http.Handler { return s.mux }

// --- envelope helpers ----------------------------------------------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeOK(w http.ResponseWriter, data any) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "data": data})
}

func writeErr(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]any{
		"ok": false,
		"error": map[string]string{
			"code":    code,
			"message": msg,
		},
	})
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "GET required")
		return
	}
	// Contract shape: top-level {ok, service}, not wrapped in data.
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "service": "netease-go"})
}

func (s *Server) auth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if s.token != "" && r.Header.Get("X-Parlay-Token") != s.token {
			writeErr(w, http.StatusUnauthorized, "unauthorized", "missing or wrong X-Parlay-Token")
			return
		}
		next(w, r)
	}
}

func readBody(r *http.Request) ([]byte, error) {
	defer r.Body.Close()
	return io.ReadAll(io.LimitReader(r.Body, maxBody))
}

// parseTrackID accepts track_id as a JSON string ("186016") or number.
func parseTrackID(raw json.RawMessage) (int64, error) {
	if len(raw) == 0 {
		return 0, fmt.Errorf("track_id is required")
	}
	var s string
	if err := json.Unmarshal(raw, &s); err == nil {
		return strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	}
	var n int64
	if err := json.Unmarshal(raw, &n); err == nil {
		return n, nil
	}
	return 0, fmt.Errorf("track_id must be a string or number")
}

// --- handlers ------------------------------------------------------------

func (s *Server) handleSearch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "POST required")
		return
	}
	body, err := readBody(r)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "unreadable body")
		return
	}
	var req struct {
		Query string `json:"query"`
		Limit int    `json:"limit"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "invalid JSON body")
		return
	}
	if strings.TrimSpace(req.Query) == "" {
		writeErr(w, http.StatusBadRequest, "bad_request", "query is required")
		return
	}
	tracks, err := s.client.SearchSong(req.Query, req.Limit)
	if err != nil {
		log.Printf("search %q: %v", req.Query, err)
		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
		return
	}
	writeOK(w, map[string]any{"results": tracks})
}

var safeName = regexp.MustCompile(`[^A-Za-z0-9._-]+`)

func (s *Server) handleDownload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "POST required")
		return
	}
	body, err := readBody(r)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "unreadable body")
		return
	}
	var req struct {
		TrackID json.RawMessage `json:"track_id"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "invalid JSON body")
		return
	}
	id, err := parseTrackID(req.TrackID)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}

	track, err := s.client.SongDetail(id)
	if err != nil {
		log.Printf("download detail %d: %v", id, err)
		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
		return
	}
	audioURL, declaredSize, level, err := s.client.SongURLWithLevel(id)
	if err != nil {
		log.Printf("download url %d: %v", id, err)
		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
		return
	}
	if declaredSize > netease.MaxBotAPIBYTES {
		// 50 MB Bot API rule: queue, don't fail — no file is written.
		writeOK(w, map[string]any{
			"file_path":  "",
			"track":      track,
			"oversize":   true,
			"size_bytes": declaredSize,
			"note":       "over the 50 MB Bot API cap; queued for a later MTProto pass, not downloaded",
		})
		return
	}

	name := safeName.ReplaceAllString(
		fmt.Sprintf("netease-%d-%s-%s", id, track.Artist, track.Title), "_")
	if len(name) > 120 {
		name = name[:120]
	}
	// The audio URL's extension is the real container: the mirror fallback
	// serves FLAC, the direct endpoints serve MP3. Default to .mp3.
	ext := ".mp3"
	if u, err := url.Parse(audioURL); err == nil {
		switch strings.ToLower(filepath.Ext(u.Path)) {
		case ".flac", ".mp3", ".m4a", ".ogg", ".wav", ".aac":
			ext = strings.ToLower(filepath.Ext(u.Path))
		}
	}
	path := filepath.Join(s.dlDir, name+ext)
	f, err := os.Create(path)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	n, oversize, err := s.client.Download(audioURL, f)
	_ = f.Close()
	if err != nil {
		_ = os.Remove(path)
		log.Printf("download %d: %v", id, err)
		writeErr(w, http.StatusBadGateway, "download_failed", err.Error())
		return
	}
	if oversize {
		// Stream exceeded the cap mid-download: drop the partial file.
		_ = os.Remove(path)
		writeOK(w, map[string]any{
			"file_path":  "",
			"track":      track,
			"oversize":   true,
			"size_bytes": n,
			"note":       "over the 50 MB Bot API cap; queued for a later MTProto pass, not kept",
		})
		return
	}
	writeOK(w, map[string]any{
		"file_path":  path,
		"track":      track,
		"oversize":   false,
		"size_bytes": n,
		"level":      level,
	})
}

func (s *Server) handleLyrics(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, http.StatusMethodNotAllowed, "bad_request", "POST required")
		return
	}
	body, err := readBody(r)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "unreadable body")
		return
	}
	var req struct {
		TrackID json.RawMessage `json:"track_id"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", "invalid JSON body")
		return
	}
	id, err := parseTrackID(req.TrackID)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_request", err.Error())
		return
	}
	lyrics, err := s.client.Lyrics(id)
	if err != nil {
		log.Printf("lyrics %d: %v", id, err)
		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
		return
	}
	writeOK(w, map[string]any{"lyrics": lyrics})
}
