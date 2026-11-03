// Package fx implements the fx.apply UDS action
// (phase3/actions-inventory.md §7.8, ported from
// services/audio-fx-rust).
//
// Phase 5 behaviour: fx.apply is LIVE. It stores the FX preset +
// parameters on the room (through the shared room.Manager —
// Manager.SetFX — never by a direct store write) and then pushes
// them to the running DSP stream, when one exists, as a
// zero-restart parameter change. The response carries {output_path,
// applied, effect, params, live_applied, detail}:
//
//   - applied=true only when the DSP confirms the effect is
//     active on the live stream — or when the effect is
//     "original", which is a no-op by definition (the input IS the
//     output; with a live stream the clear is pushed to the DSP
//     and must also succeed).
//   - live_applied=true only for a NON-original effect now
//     sounding on the live stream (applied:true with it). For
//     "original" it stays false even when the live clear was
//     pushed: the original file remains the deliverable, and the
//     core sends it when applied:true and live_applied is false.
//   - applied=false when nothing is playing (the preset is
//     recorded on the room and applies from the first frame of the
//     next track — detail says so) or when the DSP rejects the
//     change (detail carries the exact error).
//
// There is no pending_dsp field anymore: "recorded for the next
// track" is the detail string, not a second boolean. The effect is
// NEVER claimed applied when it was only recorded. No file is
// re-encoded and no ffmpeg subprocess runs: output_path is honestly
// the input path unchanged (the processed audio is the live
// stream, not a file).
//
// Persistence (Phase 4): the room's fx field is written ONLY
// through the shared room.Manager (Manager.SetFX), never by a
// direct store write. The Manager is the single owner of room
// state: a direct store.GetRoom/PutRoom round-trip is invisible to
// it, so the next Manager mutation persists the in-memory copy and
// clobbers the FX fields (and a concurrent Manager mutation between
// the Get and the Put is lost the other way). The store has no fx
// bucket — the rooms bucket, via the Manager seam, is the fx home.
package fx

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// Effects are the ffmpeg -af chains of audio-fx-rust, kept as names
// only (the chains themselves live in parlay-dsp, Phase 5).
var effects = map[string]bool{
	"slow-reverb": true,
	"nightcore":   true,
	"bass-boost":  true,
	"8d":          true,
	"original":    true,
}

// fileRecord is the per-file FX record (in-memory only; the
// durable record is the room's fx field, written through the room
// Manager — the store has no fx bucket).
type fileRecord struct {
	effect      string
	params      map[string]float64
	outputPath  string
	applied     bool
	liveApplied bool
}

// liveFX is the voice Manager's live parameter push, as an
// interface so fx tests can substitute a fake without a voice
// call. *voice.Manager implements it (ApplyFX).
type liveFX interface {
	ApplyFX(chatID int64, opts dsp.Options) error
}

// Service owns the fx.apply handler.
type Service struct {
	rooms *room.Manager
	// voice is nil when no live apply is possible (tests that
	// construct the Service directly); fx.apply then degrades to
	// record-only and reports applied:false honestly.
	voice liveFX

	mu    sync.Mutex
	files map[string]fileRecord
}

// Register registers fx.apply on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Room == nil {
		panic("fx: Register called with nil deps")
	}
	svc := &Service{rooms: d.Room, voice: d.Voice, files: map[string]fileRecord{}}
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
	if body.OutputPath != "" {
		// Accepted and stored as the intended output; the returned
		// output_path is still the input (no file is written).
		if err := sanitizePath(body.OutputPath); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("output_path: %v", err))
		}
	}

	// Record the preset on the room first (through the shared
	// Manager only — see the package doc), then push it live.
	// The live push reads the room's full DSP options (speed,
	// bass, volume, plus the preset just stored), so a mid-track
	// fx.apply changes ONLY the preset and never resets the
	// room's speed/bass/volume.
	chatID := body.ChatID
	if chatID == 0 {
		chatID = req.ChatID
	}
	if chatID != 0 {
		if _, err := s.rooms.SetFX(chatID, body.Effect, params); err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("fx room store failed: %v", err))
		}
	}

	// Live apply: applied=true only when the DSP confirms.
	applied := false
	liveApplied := false
	detail := ""
	if body.Effect == "original" {
		// No-op by definition: the input IS the output. With a
		// live stream the clear is still pushed so the previous
		// effect stops now; without one there is nothing to
		// clear. Either way the effect is "applied" — there is
		// nothing to apply — but live_applied stays false: the
		// live clear is not an effect sounding on the stream,
		// and the original file remains the deliverable (the
		// core sends it when applied:true and live_applied is
		// false). live_applied is true only for a non-original
		// effect now sounding live, where there is no file to
		// send.
		if chatID != 0 && s.voice != nil {
			if err := s.voice.ApplyFX(chatID, s.rooms.DSPOptions(chatID)); err != nil {
				if errors.Is(err, voice.ErrNoFeed) {
					applied = true
					detail = "no live stream; nothing to clear"
				} else {
					detail = fmt.Sprintf("live clear failed: %v", err)
				}
			} else {
				applied = true
				detail = "live effect cleared"
			}
		} else {
			applied = true
			detail = "no live stream; nothing to clear"
		}
	} else if chatID == 0 || s.voice == nil {
		detail = "recorded for the next track; no live stream to apply to"
	} else if err := s.voice.ApplyFX(chatID, s.rooms.DSPOptions(chatID)); err != nil {
		if errors.Is(err, voice.ErrNoFeed) {
			detail = "recorded for the next track; no live stream to apply to"
		} else {
			detail = fmt.Sprintf("DSP rejected the effect: %v", err)
		}
	} else {
		applied = true
		liveApplied = true
	}

	s.mu.Lock()
	s.files[body.InputPath] = fileRecord{
		effect: body.Effect, params: params,
		outputPath: body.OutputPath, applied: applied, liveApplied: liveApplied,
	}
	s.mu.Unlock()

	resp := map[string]any{
		"output_path":  body.InputPath,
		"applied":      applied,
		"effect":       body.Effect,
		"live_applied": liveApplied,
		"params":       params,
		// output_path is the input unchanged: no file was written.
		// The intended output (when the caller gave one) is echoed
		// separately so the core never mistakes it for a real file.
		"intended_output_path": body.OutputPath,
	}
	if detail != "" {
		resp["detail"] = detail
	}
	return resp, nil
}
