// Package fx implements the fx.apply UDS action
// (phase3/actions-inventory.md §7.8, ported from
// services/audio-fx-rust).
//
// Phase 3 behaviour (locked by FINAL-ARCHITECTURE.md §1.2
// prohibition 3 — no subprocess per call on the primary path — and
// the DSP is Phase 5): fx.apply is RECORD-ONLY. It stores the FX
// preset + parameters on the room (and a per-file record) and returns
// output_path honestly = the input path unchanged (no re-encode, no
// ffmpeg subprocess). The response carries {output_path, applied,
// effect, pending_dsp} — except "original", which is a no-op by
// definition ({applied: true, pending_dsp: false}). The effect is
// NEVER claimed applied when it was only recorded. The DSP (Phase 5)
// reads the room's fx field at stream time.
package fx

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// Effects are the ffmpeg -af chains of audio-fx-rust, kept as names
// only in Phase 3 (the chains themselves live in parlay-dsp, Phase 5).
var effects = map[string]bool{
	"slow-reverb": true,
	"nightcore":   true,
	"bass-boost":  true,
	"8d":          true,
	"original":    true,
}

// fileRecord is the per-file FX record (in-memory; the durable
// record is the room's fx field — the store has no fx bucket, see
// the report).
type fileRecord struct {
	effect     string
	params     map[string]float64
	outputPath string
	applied    bool
	pendingDSP bool
}

// Service owns the fx.apply handler.
type Service struct {
	store *store.Store

	mu    sync.Mutex
	files map[string]fileRecord
}

// Register registers fx.apply on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("fx: Register called with nil deps")
	}
	svc := &Service{store: d.Store, files: map[string]fileRecord{}}
	s.Register("fx.apply", svc.handleApply)
}

// sanitizePath rejects ".." traversal (audio-fx-rust rule, same as
// the vc.play_file path rule) and requires an absolute path.
func sanitizePath(p string) error {
	if p == "" {
		return fmt.Errorf("input_path is required")
	}
	clean := filepath.Clean(p)
	if !filepath.IsAbs(clean) {
		return fmt.Errorf("input_path must be an absolute path")
	}
	for _, part := range strings.Split(clean, string(filepath.Separator)) {
		if part == ".." {
			return fmt.Errorf("input_path must not contain ..")
		}
	}
	return nil
}

func (s *Service) handleApply(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Effect      string             `json:"effect"`
		InputPath   string             `json:"input_path"`
		OutputPath  string             `json:"output_path"`
		RequestedBy *int64             `json:"requested_by"`
		ChatID      int64              `json:"chat_id"`
		Params      map[string]float64 `json:"params"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	if body.Effect == "" || !effects[body.Effect] {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown effect %q (want slow-reverb|nightcore|bass-boost|8d|original)", body.Effect))
	}
	if err := sanitizePath(body.InputPath); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("%v", err))
	}
	info, err := os.Stat(body.InputPath)
	if err != nil || info.IsDir() {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("input_path is not a file: %s", body.InputPath))
	}
	// Validate params: finite numbers only.
	for k, v := range body.Params {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("param %q is not a finite number", k))
		}
	}
	params := body.Params
	if params == nil {
		params = map[string]float64{}
	}

	// Record-only: nothing is processed. "original" is a no-op by
	// definition (stream copy in the old code — the input IS the
	// output); every other effect waits for the Phase 5 DSP.
	applied := body.Effect == "original"
	pendingDSP := !applied
	if body.OutputPath != "" {
		// Accepted and stored as the intended output; the returned
		// output_path is still the input until the DSP writes it.
		if err := sanitizePath(body.OutputPath); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("output_path: %v", err))
		}
	}

	chatID := body.ChatID
	if chatID == 0 {
		chatID = req.ChatID
	}
	if chatID != 0 {
		room, found, err := s.store.GetRoom(chatID)
		if err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("fx room lookup failed: %v", err))
		}
		if !found {
			room = model.DefaultRoomState(chatID)
		}
		room.FXPreset = body.Effect
		room.FXParams = params
		room.UpdatedAt = time.Now().Unix()
		if err := s.store.PutRoom(room); err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("fx room store failed: %v", err))
		}
	}
	s.mu.Lock()
	s.files[body.InputPath] = fileRecord{
		effect: body.Effect, params: params,
		outputPath: body.OutputPath, applied: applied, pendingDSP: pendingDSP,
	}
	s.mu.Unlock()

	return map[string]any{
		"output_path": body.InputPath,
		"applied":     applied,
		"effect":      body.Effect,
		"pending_dsp": pendingDSP,
		"params":      params,
		// output_path is the input unchanged: no file was written.
		// The intended output (when the caller gave one) is echoed
		// separately so the core never mistakes it for a real file.
		"intended_output_path": body.OutputPath,
	}, nil
}
