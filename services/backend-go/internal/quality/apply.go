package quality

import (
	"fmt"
	"log"

	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/voice"
)

// Apply resolves the effective config for st and applies it
// to the library via voice.SetOpusConfig, then reads it back.
// The readback must equal the applied config: the patched
// library round-trips exactly, so a mismatch means the
// settings did not stick (for example a no-op fallback
// linked by mistake — see internal/voice/opus.go on
// opus_stub.c) and Apply fails loudly instead of letting the
// backend run on unknown encoder settings. The returned
// config and sources are the applied values, for logging and
// for quality.get. When the Opus DSP is off for this chat,
// the "effective" config is the library's stock defaults
// (see Effective): Apply restores them so no earlier chat's
// applied config leaks in through the process-global stored
// config, and logs that the DSP is off instead of the
// applied-values line.
func Apply(cfg *config.Config, st model.ChatSettings) (voice.OpusEncoderConfig, map[string]string, error) {
	eff, sources, enabled := Effective(cfg, st)
	if err := voice.ValidateOpusConfig(eff); err != nil {
		return eff, sources, fmt.Errorf("quality: effective opus config invalid: %w", err)
	}
	if err := voice.SetOpusConfig(eff); err != nil {
		return eff, sources, fmt.Errorf("quality: opus set_config: %w", err)
	}
	got, err := voice.OpusConfig()
	if err != nil {
		return eff, sources, fmt.Errorf("quality: opus config readback: %w", err)
	}
	if got != eff {
		return eff, sources, fmt.Errorf("quality: opus config did not stick: applied %+v, library reports %+v", eff, got)
	}
	if !enabled {
		log.Printf("quality: opus dsp off — stock encoder defaults restored (bitrate=%d frame=%dms, no custom opus config applied; dsp_enabled source=%s)",
			eff.BitrateBps, eff.FrameSizeMs,
			sources[config.OpusFieldDSPEnabled])
		return eff, sources, nil
	}
	log.Printf("quality: opus applied bitrate=%d complexity=%d vbr=%v fec=%v dtx=%v frame=%dms maxplay=%d (sources: bitrate=%s frame=%s complexity=%s vbr=%s fec=%s dtx=%s)",
		eff.BitrateBps, eff.Complexity, eff.VBREnabled, eff.FECEnabled, eff.DTXEnabled, eff.FrameSizeMs, eff.MaxPlaybackRateHz,
		sources[config.OpusFieldBitrateBps], sources[config.OpusFieldFrameMs], sources[config.OpusFieldComplexity],
		sources[config.OpusFieldVBR], sources[config.OpusFieldFEC], sources[config.OpusFieldDTX])
	return eff, sources, nil
}

// ApplyStartup applies the file-level effective config (no
// chat, so no store override) at backend startup, before the
// UDS server listens and any join can happen.
func ApplyStartup(cfg *config.Config) (voice.OpusEncoderConfig, map[string]string, error) {
	return Apply(cfg, model.ChatSettings{})
}

// ApplyForChat applies the effective config for chatID: the
// chat's stored quality overrides over the file-level config.
// A chat with no stored settings applies the file-level
// config. This is the voice Manager's pre-join hook
// (installed by main.go): it runs before CreateCall on every
// join, so the joining chat's overrides land on that call.
func ApplyForChat(cfg *config.Config, st *store.Store, chatID int64) error {
	settings, _, err := st.GetSettings(chatID)
	if err != nil {
		return fmt.Errorf("quality: settings lookup chat %d: %w", chatID, err)
	}
	_, _, err = Apply(cfg, settings)
	return err
}
