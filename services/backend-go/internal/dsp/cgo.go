//go:build parlaydsp

// The real cgo binding for the parlay-dsp Rust engine
// (services/parlay-dsp, Worker B). Built only with
// -tags parlaydsp; the default build uses unavailable.go and
// audio fails honestly with ErrUnavailable.
//
// ABI (services/parlay-dsp/parlay_dsp.h, ABI_VERSION 1; verified
// against src/lib.rs, src/engine.rs, src/params.rs, src/slot.rs):
//
//   - One engine per stream: parlay_dsp_create returns a uint64
//     handle into a process-wide registry; destroy is safe while
//     a pull is blocked (the pull wakes with EOF) and while a
//     push is in flight (the push fails ERR_HANDLE).
//   - Input is pushed per slot (0 or 1): parlay_dsp_push never
//     blocks on playback and never takes the engine mutex; the
//     feeder MUST call parlay_dsp_finish_input on reader EOF or
//     error, because EOS only arrives after finish + drain.
//   - Output is parlay_dsp_pull_s16: one 960-sample frame (20 ms
//     at 48 kHz stereo interleaved = 1920 int16 = 3840 bytes).
//     Returns 0 frame, 1 PULL_NEED_INPUT (push more, retry),
//     2 PULL_EOS (queue drained), or a negative code
//     (ERR_GENERIC -1, ERR_HANDLE -2, ERR_ARG -3, ERR_DECODE -4);
//     the transition out-param is 0 none, 1 track boundary,
//     2 loop restart. A pull may block inside the decoder until
//     more bytes are pushed or finish_input; destroy wakes it.
//     This file uses pull_s16 (TPDF-dithered), never pull_f32.
//   - FX setters are individual, not a set_params JSON call:
//     parlay_dsp_set_preset, set_speed, set_bass_db, set_volume,
//     set_loop, set_echo, set_reverb, set_8d. set_preset RESETS
//     all targets to the preset (except volume, which it
//     preserves), so the call order in applyOptions is critical:
//     preset FIRST, then the combined speed/bass/volume. The
//     engine clamps set_speed to 0.5–2.0 and set_bass_db to
//     ±24 dB SILENTLY (returns Ok), so the Go side validates the
//     combined values first and rejects a combined speed above
//     2.0 instead of playing at the wrong speed.
//   - parlay_dsp_stats fills a ParlayDspStats struct; this file
//     does not expose it (the feed already counts frames sent).
//     parlay_dsp_last_error returns the engine's last error text.
//
// Slot ownership is tracked in Go, not via parlay_dsp_free_slot:
// free_slot reports LOAD state (a slot whose bytes have not been
// pushed yet still reads "free"), so it races with the feeder's
// first push. Go claims the active slot at OpenStream and the
// other slot at LoadNext, and reclaims the old active slot when
// a transition is delivered. Pushing a new track into a slot
// the engine marked consumed on promotion works directly — the
// slot's generation bump drops the stale decoder on the next
// pull (engine.rs advance_or_loop + slot.rs sync); no
// reset_slot call is needed. Loop mode (set_loop) is never
// enabled: Go loads the same track as the next slot and the
// room owns the replay count, so a loop can never run forever.
//
// The Go-facing frame stays 480 samples (1920 bytes, 10 ms):
// each 960-sample engine frame is split into two Go frames and
// the transition flag attaches to the SECOND half — never early,
// because the track boundary sits inside the engine frame.
package dsp

/*
#cgo linux LDFLAGS: -L${SRCDIR}/../../../parlay-dsp/target/release -lparlay_dsp -lm -Wl,-rpath,${SRCDIR}/../../../parlay-dsp/target/release
#cgo CFLAGS: -I${SRCDIR}/../../../parlay-dsp
#include "parlay_dsp.h"
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"
	"unsafe"
)

// engineFrameBytes is one parlay-dsp pull_s16 frame, from the
// ABI header (PARLAY_DSP_S16_FRAME_BYTES): 960 samples/channel
// × 2 channels × 2 bytes. PARLAY_DSP_FRAME_SAMPLES_10MS (480)
// is the Go-facing half-frame; PARLAY_DSP_S16_10MS_BYTES
// (1920) is dsp.FrameBytes. The engine never emits a short
// final frame — a pull is a full 960-frame or EOS — so the Go
// side never has to pad or drop a short chunk at the sink.
const engineFrameBytes = int(C.PARLAY_DSP_S16_FRAME_BYTES)

// abiVersion is the parlay_dsp.h ABI_VERSION the binding was
// written against. The engine's parlay_dsp_version() must return
// exactly this; a mismatch is a hard error, not a warning.
const abiVersion = 1

// checkABI verifies the linked library speaks the ABI this file
// was written against. It runs once per process.
var checkABI = sync.OnceValue(func() error {
	if v := int(C.parlay_dsp_version()); v != abiVersion {
		return fmt.Errorf("dsp: parlay-dsp ABI version %d, this build expects %d", v, abiVersion)
	}
	return nil
})

// cgoEngine is the Engine implementation backed by the Rust
// library. One parlay-dsp handle is created per OpenStream.
type cgoEngine struct{}

func newEngine() Engine { return cgoEngine{} }

// Linked reports whether the real engine is linked.
func Linked() bool { return true }

func (cgoEngine) OpenStream(ctx context.Context, r io.ReadCloser, opts Options) (Stream, error) {
	if err := checkABI(); err != nil {
		return nil, err
	}
	if r == nil {
		return nil, errors.New("dsp: nil reader")
	}
	if err := opts.Validate(); err != nil {
		r.Close()
		return nil, err
	}
	var handle C.uint64_t
	if rc := C.parlay_dsp_create(&handle); rc != 0 {
		r.Close()
		return nil, fmt.Errorf("dsp: parlay_dsp_create failed (rc %d)", int(rc))
	}
	s := &cgoStream{
		handle:     handle,
		done:       make(chan struct{}),
		pushed:     make(chan struct{}, 1),
		activeSlot: 0,
		nextSlot:   -1,
		readers:    map[int]io.ReadCloser{0: r},
	}
	// Preset first (it resets speed/bass targets), then the
	// combined values. A rejected speed closes the handle and
	// the reader rather than playing at the wrong speed.
	if err := s.applyOptions(opts); err != nil {
		s.Close()
		return nil, err
	}
	s.startFeeder(ctx, 0, r)
	return s, nil
}

// cgoStream is one decoded stream: one engine handle, one feeder
// goroutine per loaded slot, and a 960→480 frame splitter.
type cgoStream struct {
	handle C.uint64_t

	mu     sync.Mutex
	closed bool
	done   chan struct{} // closed by Close; wakes a blocked ReadFrame
	pushed chan struct{} // buffered 1; feeder signals after each push

	// Slot bookkeeping (Go-owned; see the package doc). activeSlot
	// is the slot the playing track was pushed into; nextSlot is
	// the slot the next track was pushed into, or -1.
	activeSlot int
	nextSlot   int

	// readers tracks the live feeder readers by slot so Close can
	// unblock a feeder stuck in Read. Guarded by feedMu.
	feedMu  sync.Mutex
	readers map[int]io.ReadCloser
	feedErr error

	// Second half of the last pulled 960-frame, held for the next
	// ReadFrame call, with its transition flag.
	pending           []byte
	pendingTransition bool
}

// applyOptions pushes an Options set into the engine. Call order
// is the contract: set_preset FIRST (it resets the speed/bass
// targets), then the combined speed, bass, and volume. The
// combined speed is room Speed × the preset rate (nightcore 1.25,
// slow-reverb 0.8, else 1.0; params["rate"] overrides for
// nightcore/slow-reverb); the combined bass is the preset bass
// (bass-boost +10 dB, else 0) plus the room bass mapped 0–100 →
// 0–+24 dB. A combined speed above EngineMaxSpeed is rejected —
// the engine would clamp it silently and play at the wrong
// speed. A combined speed below the engine floor (only reachable
// as room 0.5 × slow-reverb 0.8 = 0.4) is raised to the floor;
// the deviation is 0.1 at an extreme setting and the alternative
// (refusing to play slow-reverb at the slowest room speed) is
// worse.
func (s *cgoStream) applyOptions(opts Options) error {
	opts = opts.Normalized()
	preset := opts.Effect
	if preset == "" {
		preset = EffectOriginal
	}
	cPreset := C.CString(preset)
	rc := C.parlay_dsp_set_preset(s.handle, cPreset)
	C.free(unsafe.Pointer(cPreset))
	if rc != 0 {
		return fmt.Errorf("dsp: set_preset %q: %w", preset, s.lastError(rc))
	}

	speed := opts.Speed * PresetRate(preset, opts.EffectParams)
	if speed > EngineMaxSpeed {
		return fmt.Errorf("dsp: combined speed %.2f exceeds the engine limit %.1f (room speed %.2f × preset rate %.2f for %q)",
			speed, EngineMaxSpeed, opts.Speed, PresetRate(preset, opts.EffectParams), preset)
	}
	if speed < EngineMinSpeed {
		speed = EngineMinSpeed
	}
	if rc := C.parlay_dsp_set_speed(s.handle, C.double(speed)); rc != 0 {
		return fmt.Errorf("dsp: set_speed %.2f: %w", speed, s.lastError(rc))
	}

	bassDB := PresetBassDB(preset) + RoomBassDB(opts.Bass)
	if bassDB > 24 {
		bassDB = 24
	}
	if bassDB < -24 {
		bassDB = -24
	}
	if rc := C.parlay_dsp_set_bass_db(s.handle, C.double(bassDB)); rc != 0 {
		return fmt.Errorf("dsp: set_bass_db %.2f: %w", bassDB, s.lastError(rc))
	}

	if rc := C.parlay_dsp_set_volume(s.handle, C.double(opts.Volume)); rc != 0 {
		return fmt.Errorf("dsp: set_volume %d: %w", opts.Volume, s.lastError(rc))
	}
	return nil
}

// lastError returns the engine's last error text for a failed
// call (rc), falling back to the numeric code when the engine
// recorded nothing.
func (s *cgoStream) lastError(rc C.int) error {
	var buf [1024]C.char
	n := C.parlay_dsp_last_error(s.handle, &buf[0], C.size_t(len(buf)))
	if n > 0 {
		return errors.New(C.GoStringN(&buf[0], n))
	}
	return fmt.Errorf("parlay-dsp error %d", int(rc))
}

// startFeeder launches the goroutine that reads r in 32 KiB
// chunks, pushes them into slot, signals the ReadFrame retry
// loop after each push, and calls finish_input on EOF or read
// error (EOS only arrives after finish + drain). A read error is
// recorded and surfaced at EOS instead of being swallowed: the
// feeder stops pushing, the engine drains what it has, and the
// next ReadFrame after the drain returns the read error.
func (s *cgoStream) startFeeder(ctx context.Context, slot int, r io.ReadCloser) {
	go func() {
		defer func() {
			s.feedMu.Lock()
			delete(s.readers, slot)
			s.feedMu.Unlock()
		}()
		buf := make([]byte, 32*1024)
		for {
			select {
			case <-s.done:
				r.Close()
				return
			default:
			}
			n, err := r.Read(buf)
			if n > 0 {
				rc := C.parlay_dsp_push(s.handle, C.int(slot),
					(*C.uchar)(unsafe.Pointer(&buf[0])), C.size_t(n))
				if rc != 0 {
					// Engine gone (Close destroyed the handle) or
					// bad slot: stop feeding; the stream is over.
					s.signalPushed()
					r.Close()
					return
				}
				s.signalPushed()
			}
			if err != nil {
				if err != io.EOF {
					s.feedMu.Lock()
					if s.feedErr == nil {
						s.feedErr = fmt.Errorf("dsp: source read: %w", err)
					}
					s.feedMu.Unlock()
				}
				// finish_input even after a read error: the
				// engine must drain what it has before EOS.
				C.parlay_dsp_finish_input(s.handle, C.int(slot))
				s.signalPushed()
				r.Close()
				return
			}
			_ = ctx // ctx is accepted for the interface; the
			// feeder stops via Close (done) or reader EOF/error.
		}
	}()
}

func (s *cgoStream) signalPushed() {
	select {
	case s.pushed <- struct{}{}:
	default:
	}
}

// ReadFrame returns one 480-sample frame (1920 bytes) plus a
// transition flag. The engine emits 960-sample frames; the
// first half is returned immediately and the second half is
// held for the next call, with the transition flag attached to
// the second half — the track boundary sits inside the engine
// frame, so attaching it to the first half could fire early.
// On NEED_INPUT it waits for a feeder push (or a short
// fallback) and retries; on EOS it returns io.EOF, or the
// feeder's read error if the source failed mid-stream.
func (s *cgoStream) ReadFrame() ([]byte, bool, error) {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil, false, io.ErrClosedPipe
	}
	if s.pending != nil {
		frame, tr := s.pending, s.pendingTransition
		s.pending, s.pendingTransition = nil, false
		if tr {
			// The boundary this half carries has now been
			// delivered: the next slot is active, the old
			// active slot is free for a later LoadNext.
			s.activeSlot = s.nextSlot
			s.nextSlot = -1
		}
		s.mu.Unlock()
		return frame, tr, nil
	}
	s.mu.Unlock()

	for {
		buf := make([]byte, engineFrameBytes)
		var transition C.int
		rc := C.parlay_dsp_pull_s16(s.handle,
			(*C.int16_t)(unsafe.Pointer(&buf[0])), &transition)
		switch {
		case rc == 0:
			// Frame: split into two Go frames. The first half
			// aliases buf (fresh per pull, never reused); the
			// second half is copied because the next pull
			// allocates a new buffer.
			second := append([]byte(nil), buf[FrameBytes:]...)
			s.mu.Lock()
			s.pending = second
			s.pendingTransition = transition != 0
			s.mu.Unlock()
			return buf[:FrameBytes], false, nil
		case rc == 1: // PULL_NEED_INPUT
			select {
			case <-s.pushed:
			case <-s.done:
				return nil, false, io.ErrClosedPipe
			case <-time.After(5 * time.Millisecond):
			}
			continue
		case rc == 2: // PULL_EOS
			s.feedMu.Lock()
			feedErr := s.feedErr
			s.feedMu.Unlock()
			if feedErr != nil {
				return nil, false, feedErr
			}
			return nil, false, io.EOF
		default: // negative: decode/handle/arg error
			return nil, false, fmt.Errorf("dsp: pull_s16: %w", s.lastError(rc))
		}
	}
}

// LoadNext loads the next track into the free slot for a gapless
// transition. The opts are validated but NOT re-applied: the
// engine's FX targets are global (one targets struct for both
// slots), so the room's current targets already apply to the
// next slot from its first frame; re-applying them here would
// also retarget the tail of the track still playing. A second
// LoadNext while one is pending is an error.
func (s *cgoStream) LoadNext(r io.ReadCloser, opts Options) error {
	if r == nil {
		return errors.New("dsp: nil next reader")
	}
	if err := opts.Validate(); err != nil {
		r.Close()
		return err
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		r.Close()
		return io.ErrClosedPipe
	}
	if s.nextSlot != -1 {
		s.mu.Unlock()
		r.Close()
		return errors.New("dsp: next slot already loaded")
	}
	slot := 1 - s.activeSlot
	s.nextSlot = slot
	s.feedMu.Lock()
	s.readers[slot] = r
	s.feedMu.Unlock()
	s.mu.Unlock()
	s.startFeeder(context.Background(), slot, r)
	return nil
}

// SetParams pushes a new Options set into the engine (preset
// first, then the combined values — see applyOptions). The
// engine smooths the change over ~5 frames (100 ms), so a
// mid-track fx.apply is click-free. A nil return is the DSP
// confirming the effect; a combined speed above the engine
// limit is an error, never a silent clamp.
func (s *cgoStream) SetParams(opts Options) error {
	if err := opts.Validate(); err != nil {
		return err
	}
	s.mu.Lock()
	closed := s.closed
	s.mu.Unlock()
	if closed {
		return io.ErrClosedPipe
	}
	return s.applyOptions(opts)
}

// Close destroys the engine handle (waking any blocked pull
// with EOF), unblocks the feeders by closing their readers,
// and unblocks a ReadFrame waiting on NEED_INPUT. It is safe
// to call concurrently with ReadFrame, LoadNext, and SetParams.
func (s *cgoStream) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	close(s.done)
	s.mu.Unlock()

	s.feedMu.Lock()
	for _, r := range s.readers {
		r.Close()
	}
	s.feedMu.Unlock()

	C.parlay_dsp_destroy(s.handle)
	return nil
}
