// Opus encoder configuration (Phase 5, OD-1): the Go binding
// for the patched libntgcalls.so's C API in
// refs/ntgcalls/shared-output/include/ntgcalls_opus.h
// (targets/c/ntgcalls_opus.h in the ntgcalls tree; see
// phase5/worker-a-ntgcalls.md).
//
// The three functions are PROCESS-GLOBAL, not per-client: the
// C API takes no ntgcalls handle, and the settings apply to
// Opus encoders created AFTER the call. A running encoder is
// not reconfigured. The backend therefore applies the locked
// bitrate once at startup (cmd/backend/main.go, right after
// voice.New, before the UDS server listens and any join can
// happen) and never touches it again at runtime; the bitrate
// ladder (320→256→192k) is a startup/profile choice, not a
// live control.
//
// The struct deliberately has NO signal field: WebRTC M152's
// AudioEncoderOpusConfig has no signal field and
// WebRtcOpus_EncoderCreate never calls OPUS_SET_SIGNAL, so the
// encoder runs OPUS_AUTO with OPUS_APPLICATION_AUDIO. At the
// locked 192–320 kbps stereo rates libopus selects
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

// DefaultOpusEncoderConfig is the locked encoder profile:
// 320 kbps, complexity 10, VBR on, FEC off, DTX off, 60 ms
// frames, 48 kHz fullband. The library ships these defaults,
// so applying this config is a no-op readback check, not a
// change — but the backend applies the profile bitrate from
// parlay.toml [quality] instead (see SetOpusBitrate).
func DefaultOpusEncoderConfig() OpusEncoderConfig {
	return OpusEncoderConfig{
		BitrateBps:        320000,
		Complexity:        10,
		VBREnabled:        true,
		FECEnabled:        false,
		DTXEnabled:        false,
		FrameSizeMs:       60,
		MaxPlaybackRateHz: 48000,
	}
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

// validOpusFrameSize reports whether ms is a frame size the
// encoder accepts (10, 20, 40, or 60 ms).
func validOpusFrameSize(ms int32) bool {
	switch ms {
	case 10, 20, 40, 60:
		return true
	default:
		return false
	}
}

// validateOpusConfig checks cfg against the library's accepted
// ranges and names the offending field, so a bad profile fails
// at startup with a readable error instead of a bare -206.
func validateOpusConfig(cfg OpusEncoderConfig) error {
	switch {
	case cfg.BitrateBps < OpusMinBitrateBps || cfg.BitrateBps > OpusMaxBitrateBps:
		return fmt.Errorf("voice: opus bitrate_bps %d outside %d–%d", cfg.BitrateBps, OpusMinBitrateBps, OpusMaxBitrateBps)
	case cfg.Complexity < OpusMinComplexity || cfg.Complexity > OpusMaxComplexity:
		return fmt.Errorf("voice: opus complexity %d outside %d–%d", cfg.Complexity, OpusMinComplexity, OpusMaxComplexity)
	case !validOpusFrameSize(cfg.FrameSizeMs):
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
// settings (the locked defaults until SetOpusConfig or
// SetOpusBitrate is called).
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
	if err := validateOpusConfig(cfg); err != nil {
		return err
	}
	x := cfg.toC()
	if err := checkResult(C.ntg_opus_set_config(&x)); err != nil {
		return fmt.Errorf("voice: opus set_config: %w", err)
	}
	return nil
}

// SetOpusBitrate sets only the global Opus bitrate (bits per
// second), keeping every other setting. This is the bitrate
// ladder mechanism (320000 → 256000 → 192000) and the call the
// backend makes at startup from parlay.toml [quality]
// (voice_profile → kbps). Like SetOpusConfig, it only affects
// encoders created after the call.
func SetOpusBitrate(bps int32) error {
	if bps < OpusMinBitrateBps || bps > OpusMaxBitrateBps {
		return fmt.Errorf("voice: opus bitrate_bps %d outside %d–%d", bps, OpusMinBitrateBps, OpusMaxBitrateBps)
	}
	if err := checkResult(C.ntg_opus_set_bitrate(C.int32_t(bps))); err != nil {
		return fmt.Errorf("voice: opus set_bitrate: %w", err)
	}
	return nil
}
