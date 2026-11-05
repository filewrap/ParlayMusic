// Package quality resolves and applies the effective Opus
// encoder config, and serves the quality.get UDS action the
// core's /quality command and settings-panel quality section
// render from (contracts/ipc.json).
//
// Resolution precedence (highest first):
//
//  1. store override — the joining chat's stored quality_*
//     settings (chat.settings, per-chat, set from Telegram).
//  2. parlay.toml [quality] explicit keys (opus_bitrate_bps,
//     opus_frame_ms, opus_complexity, opus_vbr, opus_fec,
//     opus_dtx).
//  3. voice_profile preset (bitrate only: ultra_320k 320,
//     high_256k 256, standard_192k 192).
//  4. built-in default (192000 bps, 20 ms, complexity 10, VBR
//     on, FEC off, DTX off, 48000 Hz —
//     voice.DefaultOpusEncoderConfig).
//
// The config package resolves levels 2–4
// (QualityConfig.OpusSettings, with per-field sources); this
// package overlays level 1 and converts to the voice type.
//
// Application: the settings are process-global in
// libntgcalls and apply to Opus encoders created AFTER the
// call — a running encoder is never reconfigured, so every
// field (bitrate included) takes effect on the NEXT join,
// never mid-call (see internal/voice/opus.go). The backend
// applies the full effective config at startup (no chat, so
// no store override) and again before EVERY join via the
// voice Manager's pre-join hook, so a per-chat override lands
// on that chat's next call without a restart.
//
// The Opus DSP switch (quality_dsp_enabled per chat,
// [quality] dsp_enabled in the file, default on) sits above
// the precedence: when it is off, Effective ignores every
// quality setting and returns the patched library's stock
// defaults (voice.StockOpusEncoderConfig) with the numeric
// sources reported as "stock" — the encoder then runs exactly
// as a fresh patched library would, with no Parlay-applied
// Opus config in force. Apply restores those stock defaults
// (rather than skipping the set) so a previous chat's applied
// config cannot leak into a DSP-off chat's call through the
// process-global stored config.
package quality

import (
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/voice"
)

// Resolve returns the effective Opus encoder config for one
// chat's settings plus the winning level per field
// ("store", "config", "profile" or "default", keyed by the
// config.OpusField* names). It performs no I/O and never
// calls the library: the caller applies the result with
// Apply, or renders it with quality.get.
func Resolve(cfg *config.Config, st model.ChatSettings) (voice.OpusEncoderConfig, map[string]string) {
	file := cfg.Quality.OpusSettings()
	eff := voice.OpusEncoderConfig{
		BitrateBps:        int32(file.BitrateBps),
		Complexity:        int32(file.Complexity),
		VBREnabled:        file.VBR,
		FECEnabled:        file.FEC,
		DTXEnabled:        file.DTX,
		FrameSizeMs:       int32(file.FrameMs),
		MaxPlaybackRateHz: int32(file.MaxPlaybackRateHz),
	}
	sources := file.Sources
	if st.QualityBitrateBps != nil {
		eff.BitrateBps = int32(*st.QualityBitrateBps)
		sources[config.OpusFieldBitrateBps] = "store"
	}
	if st.QualityComplexity != nil {
		eff.Complexity = int32(*st.QualityComplexity)
		sources[config.OpusFieldComplexity] = "store"
	}
	if st.QualityVBR != nil {
		eff.VBREnabled = *st.QualityVBR
		sources[config.OpusFieldVBR] = "store"
	}
	if st.QualityFEC != nil {
		eff.FECEnabled = *st.QualityFEC
		sources[config.OpusFieldFEC] = "store"
	}
	if st.QualityDTX != nil {
		eff.DTXEnabled = *st.QualityDTX
		sources[config.OpusFieldDTX] = "store"
	}
	if st.QualityFrameMs != nil {
		eff.FrameSizeMs = int32(*st.QualityFrameMs)
		sources[config.OpusFieldFrameMs] = "store"
	}
	return eff, sources
}

// DSPEnabled resolves the Opus DSP switch for one chat's
// settings: the per-chat quality_dsp_enabled override wins
// over parlay.toml [quality] dsp_enabled, which wins over
// the default (on). It also returns the winning level
// ("store", "config" or "default") for the sources object.
func DSPEnabled(cfg *config.Config, st model.ChatSettings) (bool, string) {
	file := cfg.Quality.OpusSettings()
	if st.QualityDSPEnabled != nil {
		return *st.QualityDSPEnabled, "store"
	}
	return file.DSPEnabled, file.Sources[config.OpusFieldDSPEnabled]
}

// Effective returns the Opus config that will actually be in
// force for one chat: Resolve's config while the DSP is on;
// the patched library's stock defaults while it is off, with
// every numeric source reported as "stock" (no quality
// setting is in force then, so claiming a store/config/
// profile source would lie). The third return is the DSP
// state itself; sources always carries the dsp_enabled level
// (store | config | default). It performs no I/O and never
// calls the library: the caller applies the result with
// Apply, or renders it with quality.get.
func Effective(cfg *config.Config, st model.ChatSettings) (voice.OpusEncoderConfig, map[string]string, bool) {
	eff, sources := Resolve(cfg, st)
	enabled, dspSource := DSPEnabled(cfg, st)
	sources[config.OpusFieldDSPEnabled] = dspSource
	if !enabled {
		eff = voice.StockOpusEncoderConfig()
		for _, f := range []string{
			config.OpusFieldBitrateBps, config.OpusFieldFrameMs,
			config.OpusFieldComplexity, config.OpusFieldVBR,
			config.OpusFieldFEC, config.OpusFieldDTX,
		} {
			sources[f] = "stock"
		}
	}
	return eff, sources, enabled
}
