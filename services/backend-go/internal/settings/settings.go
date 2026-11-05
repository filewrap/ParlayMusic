// Package settings implements the chat.settings UDS action
// (phase3/actions-inventory.md §8.1): per-chat settings, all 22 keys
// from contracts/ipc.json. The backend owns them in Phase 3 (no
// engine ever implemented this action; the old implementation was
// core-local). Values persist in the store settings bucket, key
// chat:<chat_id>, one JSON object with all 22 keys. Fresh start
// (OD-2): every chat starts at the defaults below.
//
// The 22 keys (ipc.json types; the inventory's §8.1 table agrees
// except where noted):
//
//	admin_mode int 0-2 (0), play_mode string everyone|admins,
//	cmd_delete bool, clean_mode bool, clean_delay_min int,
//	thumbnails bool, autoplay bool, autoplay_lang string,
//	autoplay_mood string, silent_mode bool, auto_delete bool,
//	auto_delete_min int 2-7 (5), language string "en",
//	paginate_purge_old bool, chat_music_download bool.
//
// Plus the 7 quality overrides (nil = no per-chat override;
// the effective Opus config falls through to parlay.toml
// [quality] — see internal/quality.Resolve):
//
//	quality_bitrate_bps int|null, quality_frame_ms int|null,
//	quality_complexity int|null, quality_vbr bool|null,
//	quality_fec bool|null, quality_dtx bool|null,
//	quality_dsp_enabled bool|null (false = Opus DSP off:
//	stock encoder, no custom Opus config applied).
//
// clean_mode / clean_delay_min are ONE field read through two
// names: clean_mode is whether auto-clean is on, clean_delay_min
// is the delay in minutes (0 = off); they are kept in sync.
// autoplay_lang defaults to "" (inventory §8.1 / U3 proposal —
// no value exists anywhere in-tree).
//
// GET/POST over UDS: the envelope carries no method, so a payload
// with key+value is a set, a payload with key only (or neither) is
// a get. GET with no key returns all 22; GET with a key returns
// that key only (still under "settings"). A JSON null value on a
// quality key CLEARS the per-chat override (falls back to the
// file/profile/default); null on any other key is a GET, as
// before. autoplay writes through to the room so the two views
// never disagree.
//
// A quality set only persists the override: nothing about a
// running encoder changes (the encoder is created per call and
// reads the process-global settings at creation — see
// internal/voice/opus.go), so the new values apply on the next
// join in that chat, never mid-call. The pre-join hook
// (internal/quality, installed by main.go) resolves and applies
// the effective config before every join.
package settings

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// Service owns the chat.settings handler.
type Service struct {
	store *store.Store
}

// Register registers chat.settings on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("settings: Register called with nil deps")
	}
	svc := &Service{store: d.Store}
	s.Register("chat.settings", svc.handleSettings)
}

var knownKeys = []string{
	"admin_mode", "play_mode", "cmd_delete", "clean_mode", "clean_delay_min",
	"thumbnails", "autoplay", "autoplay_lang", "autoplay_mood", "silent_mode",
	"auto_delete", "auto_delete_min", "language", "paginate_purge_old",
	"chat_music_download",
	"quality_bitrate_bps", "quality_frame_ms", "quality_complexity",
	"quality_vbr", "quality_fec", "quality_dtx",
	"quality_dsp_enabled",
}

// qualityKeys are the per-chat Opus override keys: a nil value
// means "no override". A JSON null set clears the override.
var qualityKeys = []string{
	"quality_bitrate_bps", "quality_frame_ms", "quality_complexity",
	"quality_vbr", "quality_fec", "quality_dtx",
	"quality_dsp_enabled",
}

func isQualityKey(k string) bool {
	for _, known := range qualityKeys {
		if known == k {
			return true
		}
	}
	return false
}

func isKnownKey(k string) bool {
	for _, known := range knownKeys {
		if known == k {
			return true
		}
	}
	return false
}

// ptrInt renders a nullable int setting: nil stays nil (JSON
// null = "no override"), otherwise the value.
func ptrInt(p *int) any {
	if p == nil {
		return nil
	}
	return *p
}

func ptrBool(p *bool) any {
	if p == nil {
		return nil
	}
	return *p
}

// settingsMap renders the 22-key object for the wire. clean_mode is
// a bool on the wire (ipc.json); clean_delay_min carries the minutes.
// The quality keys render null when unset (no per-chat override).
func settingsMap(st model.ChatSettings) map[string]any {
	return map[string]any{
		"admin_mode":          st.AdminMode,
		"play_mode":           st.PlayMode,
		"cmd_delete":          st.CmdDelete,
		"clean_mode":          st.CleanMode,
		"clean_delay_min":     st.CleanDelayMin,
		"thumbnails":          st.Thumbnails,
		"autoplay":            st.Autoplay,
		"autoplay_lang":       st.AutoplayLang,
		"autoplay_mood":       st.AutoplayMood,
		"silent_mode":         st.SilentMode,
		"auto_delete":         st.AutoDelete,
		"auto_delete_min":     st.AutoDeleteMin,
		"language":            st.Language,
		"paginate_purge_old":  st.PaginatePurgeOld,
		"chat_music_download": st.ChatMusicDownload,
		"quality_bitrate_bps": ptrInt(st.QualityBitrateBps),
		"quality_frame_ms":    ptrInt(st.QualityFrameMs),
		"quality_complexity":  ptrInt(st.QualityComplexity),
		"quality_vbr":         ptrBool(st.QualityVBR),
		"quality_fec":         ptrBool(st.QualityFEC),
		"quality_dtx":         ptrBool(st.QualityDTX),
		"quality_dsp_enabled": ptrBool(st.QualityDSPEnabled),
	}
}

// defaults returns the 22-key defaults for a fresh chat.
func defaults() model.ChatSettings {
	st := model.DefaultChatSettings()
	st.AutoplayLang = "" // inventory §8.1 / U3: "" (any language)
	return st
}

// parseBool accepts a JSON bool, or a "true"/"false"/"on"/"off"/
// "1"/"0" string (the core store used "on"/"off" strings).
func parseBool(raw json.RawMessage) (bool, error) {
	var b bool
	if err := json.Unmarshal(raw, &b); err == nil {
		return b, nil
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return false, fmt.Errorf("want a boolean")
	}
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "true", "on", "1", "yes":
		return true, nil
	case "false", "off", "0", "no":
		return false, nil
	}
	return false, fmt.Errorf("want a boolean, got %q", s)
}

func parseInt(raw json.RawMessage) (int, error) {
	var n int64
	if err := json.Unmarshal(raw, &n); err == nil {
		return int(n), nil
	}
	// A numeric string ("15") is accepted too.
	var str string
	if err := json.Unmarshal(raw, &str); err == nil {
		var v int
		if _, err := fmt.Sscanf(strings.TrimSpace(str), "%d", &v); err == nil {
			return v, nil
		}
	}
	var f float64
	if err := json.Unmarshal(raw, &f); err == nil {
		return int(f), nil
	}
	return 0, fmt.Errorf("want an integer")
}

func parseString(raw json.RawMessage) (string, error) {
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return "", fmt.Errorf("want a string")
	}
	return s, nil
}

// setKey validates and applies one key/value pair to st.
func setKey(st *model.ChatSettings, key string, raw json.RawMessage) error {
	switch key {
	case "admin_mode":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if n < 0 || n > 2 {
			return fmt.Errorf("admin_mode must be 0..2, got %d", n)
		}
		st.AdminMode = n
	case "play_mode":
		s, err := parseString(raw)
		if err != nil {
			return err
		}
		if s != "everyone" && s != "admins" {
			return fmt.Errorf("play_mode must be \"everyone\" or \"admins\", got %q", s)
		}
		st.PlayMode = s
	case "cmd_delete":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.CmdDelete = b
	case "clean_mode":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.CleanMode = b
		if b && st.CleanDelayMin == 0 {
			st.CleanDelayMin = 15 // panel cycle start
		}
		if !b {
			st.CleanDelayMin = 0
		}
	case "clean_delay_min":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if n < 0 || n > 1440 {
			return fmt.Errorf("clean_delay_min must be 0..1440, got %d", n)
		}
		st.CleanDelayMin = n
		st.CleanMode = n > 0
	case "thumbnails":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.Thumbnails = b
	case "autoplay":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.Autoplay = b
	case "autoplay_lang":
		s, err := parseString(raw)
		if err != nil {
			return err
		}
		st.AutoplayLang = s
	case "autoplay_mood":
		s, err := parseString(raw)
		if err != nil {
			return err
		}
		st.AutoplayMood = s
	case "silent_mode":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.SilentMode = b
	case "auto_delete":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.AutoDelete = b
	case "auto_delete_min":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if n < 2 || n > 7 {
			return fmt.Errorf("auto_delete_min must be 2..7, got %d", n)
		}
		st.AutoDeleteMin = n
	case "language":
		s, err := parseString(raw)
		if err != nil {
			return err
		}
		if s != "en" {
			return fmt.Errorf("language must be \"en\" until the core's i18n lands, got %q", s)
		}
		st.Language = s
	case "paginate_purge_old":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.PaginatePurgeOld = b
	case "chat_music_download":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.ChatMusicDownload = b
	case "quality_bitrate_bps":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if n < voice.OpusMinBitrateBps || n > voice.OpusMaxBitrateBps {
			return fmt.Errorf("quality_bitrate_bps must be %d..%d, got %d", voice.OpusMinBitrateBps, voice.OpusMaxBitrateBps, n)
		}
		st.QualityBitrateBps = &n
	case "quality_frame_ms":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if !voice.ValidOpusFrameSize(int32(n)) {
			return fmt.Errorf("quality_frame_ms must be one of 10, 20, 40, 60, got %d", n)
		}
		st.QualityFrameMs = &n
	case "quality_complexity":
		n, err := parseInt(raw)
		if err != nil {
			return err
		}
		if n < voice.OpusMinComplexity || n > voice.OpusMaxComplexity {
			return fmt.Errorf("quality_complexity must be %d..%d, got %d", voice.OpusMinComplexity, voice.OpusMaxComplexity, n)
		}
		st.QualityComplexity = &n
	case "quality_vbr":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.QualityVBR = &b
	case "quality_fec":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.QualityFEC = &b
	case "quality_dtx":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.QualityDTX = &b
	case "quality_dsp_enabled":
		b, err := parseBool(raw)
		if err != nil {
			return err
		}
		st.QualityDSPEnabled = &b
	default:
		return fmt.Errorf("unknown settings key %q (known: %s)", key, strings.Join(knownKeys, ", "))
	}
	return nil
}

// clearQualityKey removes a per-chat quality override: the
// effective config falls back to parlay.toml [quality] (or the
// profile/default). Only called for quality keys.
func clearQualityKey(st *model.ChatSettings, key string) {
	switch key {
	case "quality_bitrate_bps":
		st.QualityBitrateBps = nil
	case "quality_frame_ms":
		st.QualityFrameMs = nil
	case "quality_complexity":
		st.QualityComplexity = nil
	case "quality_vbr":
		st.QualityVBR = nil
	case "quality_fec":
		st.QualityFEC = nil
	case "quality_dtx":
		st.QualityDTX = nil
	case "quality_dsp_enabled":
		st.QualityDSPEnabled = nil
	}
}

func (s *Service) handleSettings(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		ChatID int64           `json:"chat_id"`
		Key    string          `json:"key"`
		Value  json.RawMessage `json:"value"`
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
	stored, found, err := s.store.GetSettings(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("settings lookup failed: %v", err))
	}
	st := stored
	if !found {
		st = defaults()
	}

	// An unknown key is a 400 whether the payload reads or
	// writes (same message as before, hoisted so the null
	// clear below does not treat it as a GET).
	if body.Key != "" && !isKnownKey(body.Key) {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown settings key %q (known: %s)", body.Key, strings.Join(knownKeys, ", ")))
	}
	isSet := body.Key != "" && len(body.Value) > 0 && string(body.Value) != "null"
	// A JSON null on a quality key clears the per-chat
	// override (falls back to the file/profile/default).
	isClear := body.Key != "" && isQualityKey(body.Key) && len(body.Value) > 0 && string(body.Value) == "null"
	if isSet || isClear {
		if isClear {
			clearQualityKey(&st, body.Key)
		} else if err := setKey(&st, body.Key, body.Value); err != nil {
			return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("key %s: %v", body.Key, err))
		}
		if err := s.store.PutSettings(chatID, st); err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("settings store failed: %v", err))
		}
		// autoplay writes through to the room so the two views
		// never disagree (§1.10).
		if body.Key == "autoplay" {
			room, rfound, rerr := s.store.GetRoom(chatID)
			if rerr == nil && rfound {
				room.Autoplay = st.Autoplay
				room.UpdatedAt = time.Now().Unix()
				_ = s.store.PutRoom(room)
			}
		}
	} else if body.Key != "" {
		// GET one key.
		return map[string]any{
			"chat_id":  chatID,
			"settings": map[string]any{body.Key: settingsMap(st)[body.Key]},
		}, nil
	}
	return map[string]any{"chat_id": chatID, "settings": settingsMap(st)}, nil
}
