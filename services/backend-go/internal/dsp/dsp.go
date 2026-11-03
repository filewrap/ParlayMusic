// Package dsp is the Go side of the parlay-dsp Rust engine
// (FINAL-ARCHITECTURE.md Part 3, Phase 5): the DSP decodes the
// resolved stream to f32, applies the float effect chain, resamples
// to 48 kHz stereo, limits, and dithers once to s16le. Go owns the
// byte source (the PrefbufferedStream reader, an HTTP body, or a
// local file) and pushes 10 ms s16le frames to ntgcalls; Rust
// performs zero network requests or downloads.
//
// The package is split by build tag:
//
//   - dsp.go (this file, tag-free): Options, the Engine/Stream
//     interfaces, ErrUnavailable, and the preset semantics shared
//     by the real binding and the test double. Nothing here
//     touches cgo.
//   - cgo.go (//go:build parlaydsp): the real cgo binding against
//     services/parlay-dsp/parlay_dsp.h (Worker B's landed crate;
//     build with -tags parlaydsp and the library in
//     services/parlay-dsp/target/release).
//   - unavailable.go (//go:build !parlaydsp): New returns an engine
//     whose OpenStream always fails with ErrUnavailable. The
//     default build stays green and audio fails honestly at the
//     same layer the old ffmpeg path failed, never silently.
//
// The voice feed (internal/voice/feed.go) is written against
// dsp.Stream only and never knows which engine is linked. Tests
// use the test double in internal/dsp/fake (pure-Go WAV decoder);
// no test double is ever linked into production code.
package dsp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
)

// Frame geometry of one Go-facing DSP output frame: 480 samples
// × 2 channels × 2 bytes (s16le), 10 ms at 48 kHz — the size the
// ntgcalls external sink consumes. The Rust engine itself emits
// 960-sample frames (20 ms, FINAL-ARCHITECTURE.md Part 3); the
// cgo stream splits each engine frame into two Go frames and
// attaches the transition flag to the second half (never early).
const (
	// FrameSamples is the number of samples per channel per frame.
	FrameSamples = 480
	// FrameBytes is the byte size of one frame.
	FrameBytes = FrameSamples * 2 * 2
	// SampleRateHz is the DSP output sample rate.
	SampleRateHz = 48000
	// ChannelCount is the DSP output channel count.
	ChannelCount = 2
)

// EngineMaxSpeed is the parlay-dsp engine's speed ceiling
// (services/parlay-dsp/src/params.rs SPEED_MAX): the engine
// clamps set_speed to 0.5–2.0 silently, so the Go side rejects a
// combined speed above this instead of playing at the wrong speed.
// The room/contract range is wider (0.5–4.0); the difference is
// reported honestly at bind/apply time.
const EngineMaxSpeed = 2.0

// EngineMinSpeed is the engine's speed floor (params.rs SPEED_MIN).
const EngineMinSpeed = 0.5

// PresetRate is the playback-rate multiplier of an effect preset
// (services/parlay-dsp/src/params.rs preset table): nightcore
// 1.25, slow-reverb 0.8, everything else 1.0. For nightcore and
// slow-reverb the fx.apply params map may override the rate with
// a "rate" entry (a non-positive or non-finite rate falls back to
// the preset default); other presets ignore it. The effective
// speed is room Speed × PresetRate — Go never pre-multiplies the
// room speed itself.
func PresetRate(effect string, params map[string]float64) float64 {
	def := 1.0
	switch effect {
	case EffectNightcore:
		def = 1.25
	case EffectSlowReverb:
		def = 0.8
	default:
		return 1.0
	}
	if params != nil {
		if r, ok := params["rate"]; ok && r > 0 && !math.IsInf(r, 1) {
			return r
		}
	}
	return def
}

// PresetBassDB is the bass shelf gain in dB a preset contributes
// (params.rs preset table): bass-boost +10 dB @ 110 Hz, everything
// else 0. It ADDS to the room bass (see RoomBassDB).
func PresetBassDB(effect string) float64 {
	if effect == EffectBassBoost {
		return 10.0
	}
	return 0.0
}

// RoomBassDB maps the room bass level (0–100) to a shelf gain in
// dB: linear 0 → 0 dB, 100 → +24 dB (the engine's BASS_MAX_DB),
// so the full DSP range is reachable and bass 0 is flat. The
// effective bass is PresetBassDB + RoomBassDB, clamped to ±24 dB
// by the engine (and by the Go caller, so the value applied is
// the value reported).
func RoomBassDB(bass int) float64 {
	return float64(bass) * 0.24
}

// Effects are the live effect presets (contracts/ipc.json
// fx.apply, ported from services/audio-fx-rust). The empty string
// means "no preset": plain decode + resample + limiter, no effect
// chain. "original" is the no-op by definition.
const (
	EffectNone       = ""
	EffectSlowReverb = "slow-reverb"
	EffectNightcore  = "nightcore"
	EffectBassBoost  = "bass-boost"
	Effect8D         = "8d"
	EffectOriginal   = "original"
)

// ValidEffect reports whether effect is a known preset (including
// none and original).
func ValidEffect(effect string) bool {
	switch effect {
	case EffectNone, EffectSlowReverb, EffectNightcore, EffectBassBoost, Effect8D, EffectOriginal:
		return true
	default:
		return false
	}
}

// Options is everything the DSP needs for one stream. Speed and
// the preset rate combine inside the DSP (rate = speed × preset
// rate: nightcore 1.25, slow-reverb 0.8); Go never pre-multiplies,
// so the room's speed survives a preset change and vice versa.
// EffectParams are copied on the way in, never aliased.
type Options struct {
	// Speed is the playback rate 0.5–4.0 (1.0 = normal). 0 means
	// unset and normalises to 1.0.
	Speed float64 `json:"speed"`
	// Bass is the bass level 0–100 (room bass; the DSP maps it to
	// its low-shelf gain).
	Bass int `json:"bass"`
	// Volume is the output volume 0–200 (100 = unity).
	Volume int `json:"volume"`
	// Effect is the preset name, or "" / "original" for none.
	Effect string `json:"effect"`
	// EffectParams are the preset's parameters (e.g. nightcore
	// rate), passed through to the DSP untouched.
	EffectParams map[string]float64 `json:"params,omitempty"`
}

// DefaultOptions is the neutral DSP state: normal speed, no bass,
// unity volume, no preset.
func DefaultOptions() Options {
	return Options{Speed: 1.0, Volume: 100}
}

// Normalized returns o with unset fields defaulted (Speed 0 →
// 1.0) and EffectParams copied, so callers never alias the room's
// map. Volume 0 is NOT defaulted: it is a real value (silence),
// not "unset" — only Speed uses 0 as the unset sentinel, because
// 0 is outside its valid range anyway.
func (o Options) Normalized() Options {
	out := o
	if out.Speed == 0 {
		out.Speed = 1.0
	}
	if out.EffectParams != nil {
		cp := make(map[string]float64, len(out.EffectParams))
		for k, v := range out.EffectParams {
			cp[k] = v
		}
		out.EffectParams = cp
	}
	return out
}

// Validate rejects out-of-range speed/bass/volume and unknown
// effects. A zero Speed is "unset", not an error (see Normalized);
// a zero Volume is valid (silence).
func (o Options) Validate() error {
	if o.Speed != 0 && (o.Speed < 0.5 || o.Speed > 4.0) {
		return fmt.Errorf("dsp: speed %v out of range 0.5..4.0", o.Speed)
	}
	if o.Bass < 0 || o.Bass > 100 {
		return fmt.Errorf("dsp: bass %d out of range 0..100", o.Bass)
	}
	if o.Volume != 0 && (o.Volume < 0 || o.Volume > 200) {
		return fmt.Errorf("dsp: volume %d out of range 0..200", o.Volume)
	}
	if !ValidEffect(o.Effect) {
		return fmt.Errorf("dsp: unknown effect %q", o.Effect)
	}
	return nil
}

// ParamsJSON is the wire form of the options for the C-ABI
// parlay_dsp_set_params call: one JSON object, same field names
// as the struct tags.
func (o Options) ParamsJSON() ([]byte, error) {
	return json.Marshal(o.Normalized())
}

// ErrUnavailable is returned by OpenStream when no DSP library is
// linked into this build (default build without -tags parlaydsp).
// It is an honest unavailable, in the same shape as the core's
// UnavailableVoiceCall: it reports its reason and never pretends
// to play.
var ErrUnavailable = errors.New("dsp: parlay-dsp library not linked: build with -tags parlaydsp once services/parlay-dsp lands")

// Stream is one decoded track: the DSP pulls encoded bytes from
// the reader Go handed it and yields 10 ms s16le frames (1920
// bytes at 48 kHz stereo).
type Stream interface {
	// ReadFrame returns the next 10 ms frame (exactly 1920
	// bytes). transition is true on the frame that crosses the
	// active→next slot boundary: the frame is real audio from
	// the promoted slot, already gap-filled by the DSP, and the
	// caller must fire the track-advance for the next slot's
	// track without rebinding. (nil, false, io.EOF) is a clean
	// end with no next slot. A non-nil error is a decode
	// failure; the caller reports it and advances.
	ReadFrame() (frame []byte, transition bool, err error)
	// LoadNext loads the next slot while the active slot is
	// still playing (the gapless dual-slot path). opts apply to
	// the next slot from its first frame. At most one next slot
	// may be pending: a second LoadNext before the transition
	// fires is an error.
	LoadNext(r io.ReadCloser, opts Options) error
	// SetParams applies a live zero-restart parameter change.
	// The DSP smooths it across 20 ms frame boundaries, so a
	// mid-track fx.apply never clicks. A nil error is the DSP's
	// confirmation that the effect is active.
	SetParams(opts Options) error
	// Close releases the stream and its reader. It unblocks a
	// ReadFrame waiting for data.
	Close() error
}

// Engine opens decode streams. It owns no network and no files:
// it decodes from the reader Go hands it.
type Engine interface {
	// OpenStream starts decoding r with opts. The engine takes
	// ownership of r: closing the stream closes it. A decode
	// failure of the input (unsupported format, corrupt header)
	// is reported here when it is detectable up front, or on
	// the first ReadFrame otherwise.
	OpenStream(ctx context.Context, r io.ReadCloser, opts Options) (Stream, error)
}

// New returns the DSP engine for this build: the real cgo engine
// with -tags parlaydsp, the honest-unavailable engine otherwise.
// The implementation lives in cgo.go / unavailable.go.
func New() Engine {
	return newEngine()
}
