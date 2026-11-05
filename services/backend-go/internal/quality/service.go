package quality

import (
	"context"
	"encoding/json"
	"fmt"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// Service owns the quality.get UDS action.
type Service struct {
	store *store.Store
	cfg   *config.Config
	voice *voice.Manager
}

// Register registers quality.get on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil || d.Config == nil || d.Voice == nil {
		panic("quality: Register called with nil deps")
	}
	svc := &Service{store: d.Store, cfg: d.Config, voice: d.Voice}
	s.Register("quality.get", svc.handleGet)
}

// opusWire renders an OpusEncoderConfig for the wire. The
// keys match the chat.settings quality_* names minus the
// prefix (bitrate_bps, frame_ms, complexity, vbr, fec, dtx),
// plus the fixed playback rate.
func opusWire(cfg voice.OpusEncoderConfig) map[string]any {
	return map[string]any{
		"bitrate_bps":          cfg.BitrateBps,
		"frame_ms":             cfg.FrameSizeMs,
		"complexity":           cfg.Complexity,
		"vbr":                  cfg.VBREnabled,
		"fec":                  cfg.FECEnabled,
		"dtx":                  cfg.DTXEnabled,
		"max_playback_rate_hz": cfg.MaxPlaybackRateHz,
	}
}

// handleGet answers quality.get: the effective Opus config
// for chatID (store override > file > profile > default —
// or the library's stock defaults with numeric sources
// "stock" while the Opus DSP is off for this chat, see
// Effective), the winning level per field, whether the DSP
// is enabled, the library's current stored config (what the
// next encoder will actually use), and whether the chat has
// a live call. The core's /quality command and the
// settings-panel quality section render from this; neither
// recomputes the precedence itself.
//
// applies_on is always "next join": the settings are
// process-global and apply to encoders created after the
// call, so a change made while a call is live takes effect
// when the next call joins, never mid-call.
func (s *Service) handleGet(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		ChatID int64 `json:"chat_id"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	chatID := body.ChatID
	if chatID == 0 {
		chatID = req.ChatID
	}
	if chatID == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "chat_id is required")
	}
	stored, _, err := s.store.GetSettings(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("settings lookup failed: %v", err))
	}
	eff, sources, dspEnabled := Effective(s.cfg, stored)
	lib, err := voice.OpusConfig()
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("opus config readback failed: %v", err))
	}
	call, found := s.voice.Get(chatID)
	active := found && call.State == voice.StateConnected
	return map[string]any{
		"chat_id":     chatID,
		"effective":   opusWire(eff),
		"sources":     sources,
		"dsp_enabled": dspEnabled,
		"library":     opusWire(lib),
		"active_call": active,
		"applies_on":  "next join",
	}, nil
}
