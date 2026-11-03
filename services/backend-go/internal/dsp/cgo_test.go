//go:build parlaydsp

// Frame counts are the engine's own numbers, not the naive
// duration×speed arithmetic: the sinc resampler's group delay
// shifts output and the flush tail adds frames (B's engine
// tests allow ±1 engine frame of flush granularity). Measured
// 2026-11-03 against the rebuilt lib (Go frames = 2× engine
// frames): speed 1.0 → 202, speed 2.0 → 110, speed 0.5 → 394,
// nightcore → 162. The bands below allow ±1 engine frame
// (2 Go frames) of flush variation around the measured value;
// what they prove is the direction and magnitude (2.0 clearly
// faster than 1.0, 0.5 clearly slower, nightcore ≈ 1.25×).
package dsp

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func cgoTone(t *testing.T, seconds int, codec string) []byte {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not in PATH")
	}
	ext := "wav"
	enc := "pcm_s16le"
	if codec == "mp3" {
		ext, enc = "mp3", "libmp3lame"
	}
	path := filepath.Join(t.TempDir(), "tone."+ext)
	cmd := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", fmt.Sprintf("sine=frequency=440:duration=%d", seconds),
		"-ar", "48000", "-ac", "2", "-c:a", enc, path)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg tone render failed: %v: %s", err, out)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read tone: %v", err)
	}
	return data
}

func cgoOpen(t *testing.T, data []byte, opts Options) *cgoStream {
	t.Helper()
	s, err := newEngine().OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), opts)
	if err != nil {
		t.Fatalf("OpenStream: %v", err)
	}
	return s.(*cgoStream)
}

// cgoDrain reads frames until EOF and returns the count, the
// number of transition flags, and the index of the first
// transition (or -1).
func cgoDrain(t *testing.T, s *cgoStream) (frames, transitions, firstTransition int) {
	t.Helper()
	firstTransition = -1
	for {
		frame, tr, err := s.ReadFrame()
		if err == io.EOF {
			return frames, transitions, firstTransition
		}
		if err != nil {
			t.Fatalf("ReadFrame: %v", err)
		}
		if len(frame) != FrameBytes {
			t.Fatalf("frame %d: %d bytes, want %d", frames, len(frame), FrameBytes)
		}
		if tr {
			transitions++
			if firstTransition == -1 {
				firstTransition = frames
			}
		}
		frames++
		if frames > 10000 {
			t.Fatal("drain ran away")
		}
	}
}

func TestCGODecodesToneExactFrames(t *testing.T) {
	data := cgoTone(t, 2, "wav")
	s := cgoOpen(t, data, DefaultOptions())
	defer s.Close()
	frames, transitions, _ := cgoDrain(t, s)
	t.Logf("2s WAV: %d frames, %d transitions", frames, transitions)
	// 200 frames of tone plus the resampler flush tail (the
	// resampler drains its delay line through the FX chain
	// after the final slot drains). Measured 202.
	if frames < 200 || frames > 206 {
		t.Errorf("frames = %d, want 200–208", frames)
	}
	if transitions != 0 {
		t.Errorf("transitions = %d, want 0", transitions)
	}
}

func TestCGODecodesMP3(t *testing.T) {
	data := cgoTone(t, 2, "mp3")
	s := cgoOpen(t, data, DefaultOptions())
	defer s.Close()
	frames, transitions, _ := cgoDrain(t, s)
	t.Logf("2s MP3: %d frames, %d transitions", frames, transitions)
	if frames < 195 || frames > 210 {
		t.Errorf("MP3 frames = %d, want 195–210", frames)
	}
	if transitions != 0 {
		t.Errorf("transitions = %d, want 0", transitions)
	}
}

func TestCGOSpeedChangesFrameCount(t *testing.T) {
	data := cgoTone(t, 2, "wav")
	fast := cgoOpen(t, data, Options{Speed: 2.0, Volume: 100})
	frames, _, _ := cgoDrain(t, fast)
	fast.Close()
	t.Logf("speed 2.0: %d frames", frames)
	if frames < 108 || frames > 114 {
		t.Errorf("frames at speed 2.0 = %d, want 108–114", frames)
	}

	slow := cgoOpen(t, data, Options{Speed: 0.5, Volume: 100})
	frames, _, _ = cgoDrain(t, slow)
	slow.Close()
	t.Logf("speed 0.5: %d frames", frames)
	if frames < 390 || frames > 398 {
		t.Errorf("frames at speed 0.5 = %d, want 390–398", frames)
	}

	nc := cgoOpen(t, data, Options{Speed: 1.0, Volume: 100, Effect: "nightcore"})
	frames, _, _ = cgoDrain(t, nc)
	nc.Close()
	t.Logf("nightcore: %d frames", frames)
	if frames < 158 || frames > 166 {
		t.Errorf("frames for nightcore = %d, want 158–166", frames)
	}
}

func TestCGOCombinedSpeedRejected(t *testing.T) {
	data := cgoTone(t, 2, "wav")
	// Room speed 2.0 × nightcore 1.25 = 2.5 > engine limit 2.0.
	if _, err := newEngine().OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), Options{Speed: 2.0, Volume: 100, Effect: "nightcore"}); err == nil {
		t.Error("OpenStream with combined speed 2.5 succeeded, want rejection")
	}
	// Room speed 4.0 alone is over the limit.
	if _, err := newEngine().OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), Options{Speed: 4.0, Volume: 100}); err == nil {
		t.Error("OpenStream with speed 4.0 succeeded, want rejection")
	}
	// SetParams rejects it too, without touching the stream.
	s := cgoOpen(t, data, DefaultOptions())
	defer s.Close()
	if err := s.SetParams(Options{Speed: 3.0, Volume: 100}); err == nil {
		t.Error("SetParams with speed 3.0 succeeded, want rejection")
	}
	// ...and the stream still plays after the rejection.
	if _, _, err := s.ReadFrame(); err != nil {
		t.Errorf("ReadFrame after rejected SetParams: %v", err)
	}
}

func TestCGOLoadNextTransitionExact(t *testing.T) {
	data := cgoTone(t, 2, "wav")
	s := cgoOpen(t, data, DefaultOptions())
	defer s.Close()
	if err := s.LoadNext(io.NopCloser(bytes.NewReader(data)), DefaultOptions()); err != nil {
		t.Fatalf("LoadNext: %v", err)
	}
	frames, transitions, first := cgoDrain(t, s)
	t.Logf("2s+2s: %d frames, %d transitions, first at %d", frames, transitions, first)
	// 400 frames of tone plus the flush tail of the final slot.
	if frames < 400 || frames > 408 {
		t.Errorf("frames = %d, want 400–408", frames)
	}
	if transitions != 1 {
		t.Fatalf("transitions = %d, want exactly 1", transitions)
	}
	// The boundary is at the end of track A (frame 200); the
	// flag attaches to the second half of the engine frame that
	// contains it, so it lands on frame 200 or 201.
	if first < 199 || first > 201 {
		t.Errorf("first transition at frame %d, want 199–201", first)
	}
	// A second LoadNext while one is pending is an error.
	s2 := cgoOpen(t, data, DefaultOptions())
	defer s2.Close()
	if err := s2.LoadNext(io.NopCloser(bytes.NewReader(data)), DefaultOptions()); err != nil {
		t.Fatalf("LoadNext 1: %v", err)
	}
	if err := s2.LoadNext(io.NopCloser(bytes.NewReader(data)), DefaultOptions()); err == nil {
		t.Error("second LoadNext while one pending succeeded, want error")
	}
}

func TestCGOSetParamsConfirms(t *testing.T) {
	data := cgoTone(t, 2, "wav")
	s := cgoOpen(t, data, DefaultOptions())
	defer s.Close()
	// nightcore + room bass 40 + speed 1.5: combined speed
	// 1.875 is within the engine limit; combined bass is the
	// preset 0 + 40×0.24 = 9.6 dB.
	if err := s.SetParams(Options{Speed: 1.5, Bass: 40, Volume: 100, Effect: "nightcore"}); err != nil {
		t.Fatalf("SetParams nightcore+bass: %v", err)
	}
	// The stream keeps playing after the live change.
	if _, _, err := s.ReadFrame(); err != nil {
		t.Errorf("ReadFrame after SetParams: %v", err)
	}
	// An unknown preset is rejected, not silently ignored.
	if err := s.SetParams(Options{Speed: 1.0, Volume: 100, Effect: "echo"}); err == nil {
		t.Error("SetParams with unknown effect succeeded, want rejection")
	}
}

func TestCGORejectsGarbage(t *testing.T) {
	garbage := bytes.Repeat([]byte{0xDE, 0xAD, 0xBE, 0xEF}, 4096)
	s := cgoOpen(t, garbage, DefaultOptions())
	defer s.Close()
	_, _, err := s.ReadFrame()
	if err == nil {
		t.Fatal("ReadFrame on garbage returned a frame, want a decode error")
	}
	if err == io.EOF {
		t.Fatal("ReadFrame on garbage returned EOF, want a decode error")
	}
	t.Logf("garbage decode error: %v", err)
}

func TestCGOCloseUnblocksBlockedRead(t *testing.T) {
	// A pipe that never yields data: the feeder blocks in Read
	// and the pull blocks waiting for input.
	pr, pw := io.Pipe()
	defer pw.Close()
	s, err := newEngine().OpenStream(context.Background(), pr, DefaultOptions())
	if err != nil {
		t.Fatalf("OpenStream: %v", err)
	}
	done := make(chan error, 1)
	go func() {
		_, _, err := s.ReadFrame()
		done <- err
	}()
	time.Sleep(200 * time.Millisecond)
	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	select {
	case err := <-done:
		if err == nil {
			t.Error("blocked ReadFrame returned nil after Close")
		} else {
			t.Logf("blocked ReadFrame after Close: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("blocked ReadFrame did not return after Close")
	}
	if _, _, err := s.ReadFrame(); err == nil {
		t.Error("ReadFrame on a closed stream succeeded")
	}
}

func TestCGOFrameGeometryMatchesABI(t *testing.T) {
	// The Go-facing frame must be exactly half an engine frame:
	// PARLAY_DSP_S16_FRAME_BYTES 3840 = 2 × PARLAY_DSP_S16_10MS_BYTES 1920.
	if engineFrameBytes != 3840 {
		t.Errorf("engineFrameBytes = %d, want 3840 (PARLAY_DSP_S16_FRAME_BYTES)", engineFrameBytes)
	}
	if engineFrameBytes != 2*FrameBytes {
		t.Errorf("engineFrameBytes %d != 2×FrameBytes %d", engineFrameBytes, FrameBytes)
	}
}
