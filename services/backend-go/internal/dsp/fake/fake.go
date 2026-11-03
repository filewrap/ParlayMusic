// Package fake is the TEST DOUBLE for the parlay-dsp engine
// (internal/dsp). It implements dsp.Engine/dsp.Stream with a
// pure-Go WAV (PCM s16le, 48 kHz stereo) decoder and a crude but
// REAL effect chain: rate (speed × preset rate), volume gain, a
// one-pole bass shelf, and 8D LFO panning. It is not a stub: it
// decodes real bytes, applies real transforms, honours the
// dual-slot transition contract (LoadNext + transition flag with
// exact sample fill across the boundary), and confirms live
// parameter changes via SetParams.
//
// What it deliberately does NOT model (documented, not hidden):
// non-WAV formats (symphonia decodes those in the real DSP — the
// fake rejects them with an error), resampling of non-48 kHz
// sources, reverb tails (slow-reverb's rate is real; its echo is
// not), the limiter, and TPDF dither. Tests that need those wait
// for the real library.
//
// This package is imported by tests ONLY (voice, fx, handlers).
// No production file imports it.
package fake

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"sync"

	"parlay/backend-go/internal/dsp"
)

// Engine is the test-double DSP engine.
type Engine struct{}

// checkSpeed mirrors the real engine's speed ceiling
// (dsp.EngineMaxSpeed): the combined speed (room Speed ×
// preset rate) above the ceiling is rejected, never silently
// clamped — the fake must be as honest as the binding it
// stands in for.
func checkSpeed(opts dsp.Options) error {
	speed := opts.Normalized().Speed * dsp.PresetRate(opts.Effect, opts.EffectParams)
	if speed > dsp.EngineMaxSpeed {
		return fmt.Errorf("fake dsp: combined speed %.2f exceeds the engine limit %.1f (room speed %.2f × preset rate %.2f for %q)",
			speed, dsp.EngineMaxSpeed, opts.Normalized().Speed, dsp.PresetRate(opts.Effect, opts.EffectParams), opts.Effect)
	}
	return nil
}

// OpenStream starts decoding r (WAV only) with opts. The header
// is parsed eagerly so an unsupported input fails here, not on
// the first frame.
func (Engine) OpenStream(_ context.Context, r io.ReadCloser, opts dsp.Options) (dsp.Stream, error) {
	if r == nil {
		return nil, errors.New("fake dsp: OpenStream with nil reader")
	}
	if err := opts.Validate(); err != nil {
		return nil, err
	}
	if err := checkSpeed(opts); err != nil {
		return nil, err
	}
	s := &Stream{opts: opts.Normalized(), lastParams: opts.Normalized()}
	if err := s.openReader(r); err != nil {
		return nil, err
	}
	return s, nil
}

// Stream is one decoded track in the fake engine.
type Stream struct {
	mu   sync.Mutex // guards opts, closed, next slot, counters
	opts dsp.Options
	// lastParams is the last options the stream confirmed (via
	// OpenStream or SetParams). Tests read it to prove parameters
	// reached the DSP.
	lastParams dsp.Options
	closed     bool
	frames     int

	// next slot (LoadNext): installed under mu, consumed by the
	// decode loop. The next slot's opts are validated at LoadNext
	// but NOT applied at the switch: the real engine's FX
	// targets are global (one targets struct for both slots), so
	// the current targets already apply to the next slot from
	// its first sample.
	nextR   io.ReadCloser
	hasNext bool

	decMu sync.Mutex // held for a whole ReadFrame (incl. reads)
	r     io.ReadCloser
	// buf is decoded source frames (float pairs) not yet
	// consumed; pos is the fractional read position in buf.
	buf      [][2]float64
	pos      float64
	srcEOF   bool
	leftover []byte // raw bytes (< 1 frame) carried between reads
	// lp is the one-pole lowpass state per channel (bass shelf);
	// phase is the 8D LFO phase; clock counts output samples.
	lp    [2]float64
	phase float64
	clock int64
}

// presetRate is the playback-rate multiplier of an effect preset,
// shared with the real binding (dsp.PresetRate, which mirrors
// the parlay-dsp preset table). Kept as a local alias so the
// decode loop reads plainly.
func presetRate(effect string, params map[string]float64) float64 {
	return dsp.PresetRate(effect, params)
}

// openReader parses the WAV header of r and makes it the active
// source. It walks the RIFF subchunks (fmt, LIST, ... ) until the
// data subchunk: ffmpeg-rendered tones carry a LIST-INFO chunk
// between fmt and data, so the data offset is not fixed. The
// caller must hold decMu (or be the constructor, before any other
// goroutine can reach the stream).
func (s *Stream) openReader(r io.ReadCloser) error {
	hdr := make([]byte, 12)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return fmt.Errorf("fake dsp: WAV header: %w", err)
	}
	if string(hdr[0:4]) != "RIFF" || string(hdr[8:12]) != "WAVE" {
		return errors.New("fake dsp: not a WAV file (only WAV is decoded by the test double)")
	}
	fmtFound := false
	for {
		sub := make([]byte, 8)
		if _, err := io.ReadFull(r, sub); err != nil {
			return fmt.Errorf("fake dsp: WAV subchunk header: %w", err)
		}
		id := string(sub[0:4])
		size := binary.LittleEndian.Uint32(sub[4:8])
		switch id {
		case "fmt ":
			body := make([]byte, size)
			if _, err := io.ReadFull(r, body); err != nil {
				return fmt.Errorf("fake dsp: WAV fmt chunk: %w", err)
			}
			if size < 16 {
				return errors.New("fake dsp: truncated WAV fmt chunk")
			}
			format := binary.LittleEndian.Uint16(body[0:2])
			channels := binary.LittleEndian.Uint16(body[2:4])
			rate := binary.LittleEndian.Uint32(body[4:8])
			bits := binary.LittleEndian.Uint16(body[14:16])
			if format != 1 || channels != 2 || rate != 48000 || bits != 16 {
				return fmt.Errorf("fake dsp: unsupported WAV format=%d channels=%d rate=%d bits=%d (want PCM s16le 48 kHz stereo)", format, channels, rate, bits)
			}
			fmtFound = true
		case "data":
			if !fmtFound {
				return errors.New("fake dsp: WAV data subchunk before fmt")
			}
			s.r = r
			s.buf = nil
			s.pos = 0
			s.srcEOF = false
			s.leftover = nil
			return nil
		default:
			// Unknown chunk (e.g. LIST-INFO): skip it,
			// word-aligned.
			if _, err := io.CopyN(io.Discard, r, int64(size+size%2)); err != nil {
				return fmt.Errorf("fake dsp: WAV skip chunk %q: %w", id, err)
			}
		}
	}
}

// currentOpts snapshots the active options under mu.
func (s *Stream) currentOpts() dsp.Options {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.opts
}

// ensure buffers at least n more decoded source frames past the
// read position (or exhausts the source). It switches to the next
// slot ONLY when the active buffer is fully exhausted and the
// source hit EOF — switching earlier would discard buffered
// active samples — and reports the switch. The caller holds decMu.
func (s *Stream) ensure(n int) (switched bool, err error) {
	for len(s.buf)-int(s.pos) < n+1 && !s.srcEOF {
		raw := make([]byte, 8192)
		m, rerr := s.r.Read(raw)
		data := append(s.leftover, raw[:m]...)
		frames := len(data) / 4
		for i := 0; i < frames; i++ {
			l := int16(binary.LittleEndian.Uint16(data[i*4:]))
			r := int16(binary.LittleEndian.Uint16(data[i*4+2:]))
			s.buf = append(s.buf, [2]float64{float64(l) / 32768, float64(r) / 32768})
		}
		s.leftover = append([]byte(nil), data[frames*4:]...)
		if rerr != nil {
			if rerr == io.EOF {
				s.srcEOF = true
			} else {
				return false, rerr
			}
		}
		if m == 0 && rerr == nil {
			return false, errors.New("fake dsp: zero-byte read with no error")
		}
	}
	// Switch only on full exhaustion: any buffered active frame
	// is still owed to the frame being assembled.
	if s.srcEOF && len(s.buf)-int(s.pos) == 0 {
		s.mu.Lock()
		hasNext := s.hasNext
		nextR := s.nextR
		s.mu.Unlock()
		if hasNext {
			// Promote the next slot mid-frame: the frame being
			// assembled is filled from the next track, exactly
			// like the real DSP's gapless boundary. The
			// current FX targets carry over unchanged (the
			// real engine's targets are global); the next
			// slot's opts were validated at LoadNext only.
			old := s.r
			_ = old.Close()
			s.mu.Lock()
			s.hasNext = false
			s.nextR = nil
			s.mu.Unlock()
			if err := s.openReader(nextR); err != nil {
				return false, fmt.Errorf("fake dsp: next slot: %w", err)
			}
			return true, nil
		}
	}
	return false, nil
}

// ReadFrame returns one 1920-byte frame, the transition flag, or
// io.EOF. A trailing partial frame is dropped, never padded —
// the same rule the old ffmpeg feed had.
func (s *Stream) ReadFrame() ([]byte, bool, error) {
	s.decMu.Lock()
	defer s.decMu.Unlock()
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil, false, errors.New("fake dsp: ReadFrame on closed stream")
	}
	s.mu.Unlock()

	opts := s.currentOpts()
	rate := opts.Speed * presetRate(opts.Effect, opts.EffectParams)
	out := make([]byte, dsp.FrameBytes)
	transition := false
	for i := 0; i < dsp.FrameSamples; i++ {
		for {
			if len(s.buf)-int(s.pos) >= 1 {
				break
			}
			switched, err := s.ensure(1)
			if err != nil {
				return nil, false, err
			}
			if switched {
				// The next slot plays under the current
				// (global) targets; nothing to recompute.
				transition = true
				continue
			}
			if s.srcEOF {
				// No more source and no next slot: the track
				// ends here; a partial frame is dropped.
				return nil, false, io.EOF
			}
		}
		idx := int(s.pos)
		frac := s.pos - float64(idx)
		var sample [2]float64
		for ch := 0; ch < 2; ch++ {
			a := s.buf[idx][ch]
			// The interpolation neighbour: held at the last
			// sample when the source ends exactly on this
			// frame, so a track whose length is an exact
			// number of frames still yields its final frame.
			b := a
			if idx+1 < len(s.buf) {
				b = s.buf[idx+1][ch]
			}
			v := a + (b-a)*frac
			// Bass: add the one-pole lowpassed signal.
			s.lp[ch] += 0.08 * (v - s.lp[ch])
			v += s.lp[ch] * (float64(opts.Bass) / 100) * 1.2
			// 8D: per-sample LFO pan.
			if opts.Effect == dsp.Effect8D {
				ph := s.phase + float64(s.clock)*2*math.Pi*0.12/48000
				gain := 0.5 + 0.5*math.Sin(ph)
				if ch == 0 {
					v *= gain
				} else {
					v *= 1 - gain
				}
			}
			// Volume: linear gain.
			v *= float64(opts.Volume) / 100
			if v > 1 {
				v = 1
			} else if v < -1 {
				v = -1
			}
			sample[ch] = v
		}
		binary.LittleEndian.PutUint16(out[i*4:], uint16(int16(sample[0]*32767)))
		binary.LittleEndian.PutUint16(out[i*4+2:], uint16(int16(sample[1]*32767)))
		s.clock++
		s.pos += rate
	}
	// Drop consumed source frames, keeping one for interpolation.
	drop := int(s.pos) - 1
	if drop < 0 {
		drop = 0
	}
	if drop > len(s.buf)-1 {
		drop = len(s.buf) - 1
	}
	if drop > 0 {
		s.buf = append([][2]float64(nil), s.buf[drop:]...)
		s.pos -= float64(drop)
	}
	s.mu.Lock()
	s.frames++
	s.mu.Unlock()
	return out, transition, nil
}

// LoadNext loads the next slot. Only one may be pending. The
// opts are validated (a bad speed is rejected here, as in the
// real binding) but the engine's FX targets are global, so the
// current targets apply to the next slot from its first sample.
func (s *Stream) LoadNext(r io.ReadCloser, opts dsp.Options) error {
	if r == nil {
		return errors.New("fake dsp: LoadNext with nil reader")
	}
	if err := opts.Validate(); err != nil {
		return err
	}
	if err := checkSpeed(opts); err != nil {
		return err
	}
	s.decMu.Lock()
	defer s.decMu.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("fake dsp: LoadNext on closed stream")
	}
	if s.hasNext {
		return errors.New("fake dsp: next slot already loaded")
	}
	s.nextR = r
	s.hasNext = true
	return nil
}

// SetParams applies a live parameter change and confirms it: a
// nil error IS the DSP's confirmation (see dsp.Stream).
func (s *Stream) SetParams(opts dsp.Options) error {
	if err := opts.Validate(); err != nil {
		return err
	}
	if err := checkSpeed(opts); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("fake dsp: SetParams on closed stream")
	}
	s.opts = opts.Normalized()
	s.lastParams = opts.Normalized()
	return nil
}

// Close releases the stream and its readers. It unblocks a
// ReadFrame waiting on the reader.
func (s *Stream) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	nextR := s.nextR
	s.nextR = nil
	s.hasNext = false
	s.mu.Unlock()
	if s.r != nil {
		_ = s.r.Close()
	}
	if nextR != nil {
		_ = nextR.Close()
	}
	return nil
}

// LastParams returns the options the stream last confirmed (via
// OpenStream or SetParams).
func (s *Stream) LastParams() dsp.Options {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lastParams
}

// FramesRead returns the number of frames ReadFrame has produced.
func (s *Stream) FramesRead() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.frames
}
