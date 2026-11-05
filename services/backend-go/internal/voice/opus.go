// Opus encoder configuration (Phase 5, OD-1): the Go binding
// for the patched libntgcalls.so's C API in
// refs/ntgcalls/shared-output/include/ntgcalls_opus.h
// (targets/c/ntgcalls_opus.h in the ntgcalls tree; see
// phase5/worker-a-ntgcalls.md).
//
// The three functions are PROCESS-GLOBAL, not per-client: the
// C API takes no ntgcalls handle, and the settings apply to
// Opus encoders created AFTER the call. A running encoder is
// NOT reconfigured: there is no live bitrate path in the C API
// (the patch notes RtpParameters max_bitrate_bps would be the
// live mechanism, and no such call is exposed to Go), so every
// field — bitrate included — takes effect on the NEXT encoder
// creation, i.e. the next join. The backend therefore applies
// the full effective config once at startup (cmd/backend/main.go,
// right after voice.New, before the UDS server listens and any
// join can happen) and again before EVERY join (the voice
// Manager's pre-join hook, installed by main.go), so a
// per-chat store override or a config change lands on the next
// call instead of waiting for a restart. A settings change
// never claims to be live: the UI text says "applies on next
// join" for every field.
//
// opus_stub.c holds weak no-op fallbacks for the three C
// functions, built ONLY with -tags ntgopus_stub (a stock
// libntgcalls.so without the OD-1 C API). The default build
// must NOT compile it: a weak definition in the executable
// interposes the shared library's strong symbol, so with the
// stub linked every Set call silently succeeds while changing
// nothing and every readback returns the stub's hardcoded
// 192000 bps / 60 ms — the startup readback then "proves" a
// setting that was never applied. The Opus round-trip tests
// (TestOpusDefaults, TestOpusSetBitrate) fail while the stub
// is linked, which is how the regression shows itself.
//
// The struct deliberately has NO signal field: WebRTC M152's
// AudioEncoderOpusConfig has no signal field and
// WebRtcOpus_EncoderCreate never calls OPUS_SET_SIGNAL, so the
// encoder runs OPUS_AUTO with OPUS_APPLICATION_AUDIO. At the
// locked 128–192 kbps stereo rates libopus selects
// CELT_ONLY/fullband identically to OPUS_SIGNAL_MUSIC, but the
// literal call does not exist — nothing here pretends to set
// it (coordinator decision, see worker-a-ntgcalls.md §6).
package voice

/*
#include "ntgcalls.h"
#include "ntgcalls_opus.h"
#include <stdlib.h>
*/
import "C"

import (
	"fmt"
)

// OpusEncoderConfig mirrors ntg_opus_encoder_config. BitrateBps
// is bits per second (320000 = 320 kbps). There is no Signal
// field (see the package doc): OPUS_AUTO + APPLICATION_AUDIO
// is what the encoder runs.
type OpusEncoderConfig struct {
	BitrateBps        int32
	Complexity        int32
	VBREnabled        bool
	FECEnabled        bool
	DTXEnabled        bool
	FrameSizeMs       int32
	MaxPlaybackRateHz int32
}

// DefaultOpusEncoderConfig is the default encoder profile:
// 192 kbps, complexity 10, VBR on, FEC off, DTX off, 20 ms
// frames, 48 kHz fullband. 20 ms aligns 1:1 with the DSP's
// 960-sample (20 ms) frames at 48 kHz, and 60 ms packets at
// 256/320 kbps (1920/2400 bytes) exceed a typical MTU, so the
// frame default moved from 60 ms to 20 ms with the bitrate
// default from 320k to 192k (Opus quality saturates at
// 128–192 kbps stereo; 256/320 kbps remain selectable, not
// the default). This is the backend's default AND the patched
// library's own fresh default (verified by a fresh-process
// OpusConfig() readback, 2026-11-05: a fresh patched
// libntgcalls.so reports exactly this config) — the old
// "320000 bps / 60 ms" claim described a stock UNPATCHED
// libntgcalls, not this patched one. The effective config is
// resolved by internal/quality (store override > parlay.toml
// [quality] explicit keys > voice_profile preset > this
// default); see config.QualityConfig.OpusSettings for the
// file-level half.
func DefaultOpusEncoderConfig() OpusEncoderConfig {
	return OpusEncoderConfig{
		BitrateBps:        192000,
		Complexity:        10,
		VBREnabled:        true,
		FECEnabled:        false,
		DTXEnabled:        false,
		FrameSizeMs:       20,
		MaxPlaybackRateHz: 48000,
	}
}

// StockOpusEncoderConfig is the patched library's own stock
// encoder config: exactly what a fresh libntgcalls.so holds
// before any SetOpusConfig call (verified by a fresh-process
// OpusConfig() readback, 2026-11-05), which is numerically
// identical to DefaultOpusEncoderConfig. DSP-off restores
// this: the backend applies it (rather than skipping the set
// entirely) so a previous chat's applied config cannot leak
// into a DSP-off chat's call through the process-global
// stored config — after the restore, the encoder runs
// exactly as a fresh patched library would, with no
// Parlay-applied Opus config in force.
func StockOpusEncoderConfig() OpusEncoderConfig {
	return DefaultOpusEncoderConfig()
}

// Opus bitrate/parameter limits, mirroring the C validation in
// ntgcalls_opus.h (out-of-range values return
// NTG_ERR_INVALID_PARAMS, -206, from the library).
const (
	// OpusMinBitrateBps / OpusMaxBitrateBps bound bitrate_bps.
	OpusMinBitrateBps = 6000
	OpusMaxBitrateBps = 510000
	// OpusMinComplexity / OpusMaxComplexity bound complexity.
	OpusMinComplexity = 0
	OpusMaxComplexity = 10
	// OpusMinPlaybackRateHz / OpusMaxPlaybackRateHz bound
	// max_playback_rate_hz.
	OpusMinPlaybackRateHz = 8000
	OpusMaxPlaybackRateHz = 48000
)

// ValidOpusFrameSize reports whether ms is a frame size the
// encoder accepts (10, 20, 40, or 60 ms). Exported: the
// settings package validates the per-chat frame override with
// the same set instead of duplicating it.
func ValidOpusFrameSize(ms int32) bool {
	switch ms {
	case 10, 20, 40, 60:
		return true
	default:
		return false
	}
}

// ValidateOpusConfig checks cfg against the library's accepted
// ranges and names the offending field, so a bad profile fails
// at startup with a readable error instead of a bare -206.
// Exported: the quality resolver (internal/quality) validates
// the merged effective config with the same bounds instead of
// duplicating them; the config package keeps its own copies
// (it must stay cgo-free) and the voice tests assert the two
// sets agree.
func ValidateOpusConfig(cfg OpusEncoderConfig) error {
	switch {
	case cfg.BitrateBps < OpusMinBitrateBps || cfg.BitrateBps > OpusMaxBitrateBps:
		return fmt.Errorf("voice: opus bitrate_bps %d outside %d–%d", cfg.BitrateBps, OpusMinBitrateBps, OpusMaxBitrateBps)
	case cfg.Complexity < OpusMinComplexity || cfg.Complexity > OpusMaxComplexity:
		return fmt.Errorf("voice: opus complexity %d outside %d–%d", cfg.Complexity, OpusMinComplexity, OpusMaxComplexity)
	case !ValidOpusFrameSize(cfg.FrameSizeMs):
		return fmt.Errorf("voice: opus frame_size_ms %d must be one of 10, 20, 40, 60", cfg.FrameSizeMs)
	case cfg.MaxPlaybackRateHz < OpusMinPlaybackRateHz || cfg.MaxPlaybackRateHz > OpusMaxPlaybackRateHz:
		return fmt.Errorf("voice: opus max_playback_rate_hz %d outside %d–%d", cfg.MaxPlaybackRateHz, OpusMinPlaybackRateHz, OpusMaxPlaybackRateHz)
	default:
		return nil
	}
}

func (c OpusEncoderConfig) toC() C.ntg_opus_encoder_config {
	var x C.ntg_opus_encoder_config
	x.bitrate_bps = C.int32_t(c.BitrateBps)
	x.complexity = C.int32_t(c.Complexity)
	x.vbr_enabled = C.bool(c.VBREnabled)
	x.fec_enabled = C.bool(c.FECEnabled)
	x.dtx_enabled = C.bool(c.DTXEnabled)
	x.frame_size_ms = C.int32_t(c.FrameSizeMs)
	x.max_playback_rate_hz = C.int32_t(c.MaxPlaybackRateHz)
	return x
}

func opusConfigFromC(x C.ntg_opus_encoder_config) OpusEncoderConfig {
	return OpusEncoderConfig{
		BitrateBps:        int32(x.bitrate_bps),
		Complexity:        int32(x.complexity),
		VBREnabled:        bool(x.vbr_enabled),
		FECEnabled:        bool(x.fec_enabled),
		DTXEnabled:        bool(x.dtx_enabled),
		FrameSizeMs:       int32(x.frame_size_ms),
		MaxPlaybackRateHz: int32(x.max_playback_rate_hz),
	}
}

// OpusConfig reads back the current global Opus encoder
// settings (the patched library's fresh defaults — 192000 bps,
// 20 ms, complexity 10, VBR on, FEC/DTX off, 48 kHz — until
// SetOpusConfig or SetOpusBitrate is called; verified by a
// fresh-process readback, 2026-11-05).
func OpusConfig() (OpusEncoderConfig, error) {
	var out C.ntg_opus_encoder_config
	if err := checkResult(C.ntg_opus_get_config(&out)); err != nil {
		return OpusEncoderConfig{}, fmt.Errorf("voice: opus get_config: %w", err)
	}
	return opusConfigFromC(out), nil
}

// SetOpusConfig replaces the global Opus encoder settings. It
// must be called before the first join: encoders created after
// the call use the new settings, a running encoder is not
// reconfigured. A nil error is the library confirming the
// settings; an out-of-range value is rejected before the call
// with the field named.
func SetOpusConfig(cfg OpusEncoderConfig) error {
	if err := ValidateOpusConfig(cfg); err != nil {
		return err
	}
	x := cfg.toC()
	if err := checkResult(C.ntg_opus_set_config(&x)); err != nil {
		return fmt.Errorf("voice: opus set_config: %w", err)
	}
	return nil
}

// SetOpusBitrate sets only the global Opus bitrate (bits per
// second), keeping every other setting. Like SetOpusConfig, it
// only affects encoders created after the call: it does NOT
// retune a running encoder, so a bitrate change made while a
// call is live takes effect on the next join, not mid-call
// (the patch documents RtpParameters max_bitrate_bps as the
// live mechanism, and no such call is exposed to Go — see the
// package doc). The backend applies bitrate changes through
// SetOpusConfig on the full effective config instead; this
// stays as the narrow ladder helper for tests and callers
// that only move the bitrate.
func SetOpusBitrate(bps int32) error {
	if bps < OpusMinBitrateBps || bps > OpusMaxBitrateBps {
		return fmt.Errorf("voice: opus bitrate_bps %d outside %d–%d", bps, OpusMinBitrateBps, OpusMaxBitrateBps)
	}
	if err := checkResult(C.ntg_opus_set_bitrate(C.int32_t(bps))); err != nil {
		return fmt.Errorf("voice: opus set_bitrate: %w", err)
	}
	return nil
}
