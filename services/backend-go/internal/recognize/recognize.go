// Package recognize owns audio.recognize (§9-E4): music recognition
// against AudD (https://api.audd.io), decided by the user 2026-11-03
// (free tier, simple API). It is env-gated on AUDD_API_TOKEN
// (config.Backend.AuddAPIToken / .env.example): until the user
// supplies a token, the action answers honestly with 502
// engine_unavailable "Music recognition is not configured", exactly
// like the other key-gated paths — never a fake result.
//
// AudD API (docs.audd.io, verified 2026-11-03): POST
// https://api.audd.io/, multipart/form-data, api_token required,
// file (binary) or url, optional return
// (apple_music,spotify,deezer,musicbrainz) and market (default us).
// Response: {status:"success"|"error", result:{artist,title,album,
// release_date,label,timecode,song_link,...}|null}. No match ->
// result null. Standard endpoint max file 10 MB; no rate limits on
// the standard endpoint. Error codes: #901 no api_token, #900 invalid
// token, #700 no file sent, #600 bad audio URL, #500 invalid audio
// file, #400 file too large, #300 fingerprinting error.
//
// The action accepts an audio file reference: a local path under the
// data dir (the core downloaded the replied voice message / audio
// file via the TDLib bot session, the same flow as vc.play_file).
// There is no second recognition provider.
package recognize

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/uds"
)

// MaxAudDFileBytes is the AudD standard endpoint's file cap
// (10 MB; AudD error #400 above it).
const MaxAudDFileBytes = 10 * 1024 * 1024

// DefaultAudDBase is the AudD API endpoint. AUDD_API_BASE overrides
// it (tests only — the production endpoint is api.audd.io).
const DefaultAudDBase = "https://api.audd.io"

// Client is a minimal AudD recognition client.
type Client struct {
	base  string
	token string
	http  *http.Client
}

// NewClient builds a client from the token and base URL. An empty
// token is allowed at construction — Recognize reports it as
// not-configured rather than failing to build.
func NewClient(token, base string) *Client {
	if strings.TrimSpace(base) == "" {
		base = DefaultAudDBase
	}
	return &Client{
		base:  strings.TrimRight(strings.TrimSpace(base), "/"),
		token: strings.TrimSpace(token),
		http:  &http.Client{Timeout: 30 * time.Second},
	}
}

// NewClientFromEnv builds a client from AUDD_API_TOKEN /
// AUDD_API_BASE.
func NewClientFromEnv() *Client {
	return NewClient(os.Getenv("AUDD_API_TOKEN"), os.Getenv("AUDD_API_BASE"))
}

// Configured reports whether an AudD token is set.
func (c *Client) Configured() bool { return c.token != "" }

// Result is one AudD recognition result.
type Result struct {
	Artist      string `json:"artist"`
	Title       string `json:"title"`
	Album       string `json:"album"`
	ReleaseDate string `json:"release_date"`
	Label       string `json:"label"`
	Timecode    string `json:"timecode"`
	SongLink    string `json:"song_link"`
	SpotifyID   string `json:"spotify_id"`
	DeezerID    string `json:"deezer_id"`
}

// auddResponse is the AudD envelope.
type auddResponse struct {
	Status string `json:"status"`
	Result *struct {
		Artist      string `json:"artist"`
		Title       string `json:"title"`
		Album       string `json:"album"`
		ReleaseDate string `json:"release_date"`
		Label       string `json:"label"`
		Timecode    string `json:"timecode"`
		SongLink    string `json:"song_link"`
		Spotify     *struct {
			ID string `json:"id"`
		} `json:"spotify"`
		Deezer *struct {
			ID json.RawMessage `json:"id"`
		} `json:"deezer"`
	} `json:"result"`
	Error *struct {
		ErrorCode    int    `json:"error_code"`
		ErrorMessage string `json:"error_message"`
	} `json:"error"`
}

// Recognize sends audio bytes to AudD and returns the match, or
// (nil, nil) when AudD answered but found no match. A missing token
// is an honest not-configured error, never a fake result.
func (c *Client) Recognize(ctx context.Context, audio []byte, fileName string) (*Result, error) {
	if !c.Configured() {
		return nil, fmt.Errorf("Music recognition is not configured: set AUDD_API_TOKEN (from dashboard.audd.io) in the environment")
	}
	if len(audio) == 0 {
		return nil, fmt.Errorf("no audio to recognize")
	}
	if len(audio) > MaxAudDFileBytes {
		return nil, fmt.Errorf("audio is too large for recognition (%d bytes; AudD limit is 10 MB)", len(audio))
	}
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	if err := w.WriteField("api_token", c.token); err != nil {
		return nil, err
	}
	// Ask for the provider ids so a match can be resolved further
	// (spotify/deezer ids feed the normal resolvers).
	if err := w.WriteField("return", "apple_music,spotify,deezer,musicbrainz"); err != nil {
		return nil, err
	}
	part, err := w.CreateFormFile("file", fileName)
	if err != nil {
		return nil, err
	}
	if _, err := part.Write(audio); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.base+"/", &body)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", w.FormDataContentType())
	req.Header.Set("User-Agent", "Parlay/1.0")
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("AudD request failed: %w", err)
	}
	defer resp.Body.Close()
	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return nil, fmt.Errorf("AudD read failed: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("AudD returned HTTP %d", resp.StatusCode)
	}
	var parsed auddResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return nil, fmt.Errorf("AudD returned bad JSON: %w", err)
	}
	if parsed.Status == "error" {
		if parsed.Error != nil {
			return nil, fmt.Errorf("AudD error #%d: %s", parsed.Error.ErrorCode, parsed.Error.ErrorMessage)
		}
		return nil, fmt.Errorf("AudD returned an error with no detail")
	}
	if parsed.Result == nil {
		return nil, nil
	}
	res := &Result{
		Artist: parsed.Result.Artist, Title: parsed.Result.Title,
		Album: parsed.Result.Album, ReleaseDate: parsed.Result.ReleaseDate,
		Label: parsed.Result.Label, Timecode: parsed.Result.Timecode,
		SongLink: parsed.Result.SongLink,
	}
	if parsed.Result.Spotify != nil {
		res.SpotifyID = parsed.Result.Spotify.ID
	}
	if parsed.Result.Deezer != nil && len(parsed.Result.Deezer.ID) > 0 {
		var idStr string
		if err := json.Unmarshal(parsed.Result.Deezer.ID, &idStr); err == nil {
			res.DeezerID = idStr
		} else {
			res.DeezerID = strings.Trim(string(parsed.Result.Deezer.ID), `"`)
		}
	}
	return res, nil
}

// Service holds the audio.recognize handler's dependencies.
type Service struct {
	deps   *app.Deps
	client *Client
}

// NewService builds a Service over deps. The AudD client reads the
// token from config (AUDD_API_TOKEN) when present, else the
// environment.
func NewService(deps *app.Deps) *Service {
	token := os.Getenv("AUDD_API_TOKEN")
	base := os.Getenv("AUDD_API_BASE")
	if deps != nil && deps.Config != nil && deps.Config.Backend.AuddAPIToken != "" {
		token = deps.Config.Backend.AuddAPIToken
	}
	return &Service{deps: deps, client: NewClient(token, base)}
}

// NewServiceWithClient builds a Service with an explicit client
// (tests).
func NewServiceWithClient(deps *app.Deps, client *Client) *Service {
	return &Service{deps: deps, client: client}
}

// Register registers audio.recognize on the UDS server.
func Register(s *uds.Server, d *app.Deps) {
	svc := NewService(d)
	s.Register("audio.recognize", svc.handleRecognize)
}

type recognizeRequest struct {
	FilePath string `json:"file_path"`
	FileID   string `json:"file_id"`
}

// handleRecognize implements audio.recognize (§9-E4): accept an audio
// file reference (local path under the data dir — the core downloaded
// the replied voice message / audio file via the TDLib bot session,
// the same flow as vc.play_file), fingerprint it against AudD, and
// return {recognized, track?, name, artists, album, source} on a
// match, {recognized: false} when AudD finds nothing.
//
// Honest failures, never fake results:
//   - no token configured -> 502 engine_unavailable "Music
//     recognition is not configured"
//   - no file reference -> bad_request
//   - file missing / outside the data dir -> not_found / bad_request
//   - file unreadable / over 10 MB / AudD provider error -> 502
//     "Can't read this voice message right now…" /
//     "Music recognition failed temporarily…"
func (s *Service) handleRecognize(ctx context.Context, req uds.Request) (any, error) {
	var body recognizeRequest
	if len(req.Payload) > 0 {
		if err := json.Unmarshal(req.Payload, &body); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad audio.recognize request: %v", err))
		}
	}
	if !s.client.Configured() {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Music recognition is not configured: set AUDD_API_TOKEN (from dashboard.audd.io) in the environment")
	}
	filePath := strings.TrimSpace(body.FilePath)
	if filePath == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "file_path is required (a local audio file under the data dir)")
	}
	absPath, err := s.validatePath(filePath)
	if err != nil {
		return nil, err
	}
	audio, err := os.ReadFile(absPath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, uds.NewHandlerError(uds.ErrNotFound, "audio file not found on disk")
		}
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "Can't read this voice message right now. Please try again later.")
	}
	if len(audio) > MaxAudDFileBytes {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, "This voice message is too long to recognize (AudD limit is 10 MB).")
	}
	result, err := s.client.Recognize(ctx, audio, filepath.Base(absPath))
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("Music recognition failed temporarily. Please try again later. (%v)", err))
	}
	if result == nil {
		return map[string]any{"recognized": false}, nil
	}
	track := model.Track{
		Title: result.Title, Artist: result.Artist, Source: "audd",
		URL: result.SongLink,
	}
	// A match is resolvable through the provider that identified it:
	// prefer the Spotify id (the normal Spotify resolver takes it),
	// else the Deezer id, else an audd:<artist> - <title> id that is
	// honestly only a label — never a fake resolvable id.
	switch {
	case result.SpotifyID != "":
		track.ID = "spotify:" + result.SpotifyID
		track.Source = "spotify"
	case result.DeezerID != "":
		track.ID = "deezer:" + result.DeezerID
		track.Source = "deezer"
	default:
		track.ID = "audd:" + strings.TrimSpace(result.Artist+" - "+result.Title)
		track.Source = "audd"
	}
	return map[string]any{
		"recognized": true,
		"track":      track,
		"name":       result.Title,
		"artists":    result.Artist,
		"album":      result.Album,
		"source":     track.Source,
	}, nil
}

// validatePath resolves file_path against the data dir and rejects
// anything outside it (the same sanitize_path rule as vc.play_file).
func (s *Service) validatePath(filePath string) (string, error) {
	dataDir := resolver.DataDir()
	if s.deps != nil && s.deps.Config != nil && s.deps.Config.Backend.DataDir != "" {
		dataDir = s.deps.Config.Backend.DataDir
	}
	absData, err := filepath.Abs(dataDir)
	if err != nil {
		return "", uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("data dir unresolvable: %v", err))
	}
	absPath := filePath
	if !filepath.IsAbs(absPath) {
		absPath = filepath.Join(absData, filePath)
	}
	absPath, err = filepath.Abs(absPath)
	if err != nil {
		return "", uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("bad file_path: %v", err))
	}
	if absPath != absData && !strings.HasPrefix(absPath, absData+string(os.PathSeparator)) {
		return "", uds.NewHandlerError(uds.ErrBadRequest, "file_path must be under the Parlay data directory")
	}
	return absPath, nil
}
