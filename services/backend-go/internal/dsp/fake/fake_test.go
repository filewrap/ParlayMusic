package fake

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"parlay/backend-go/internal/dsp"
)

func makeTone(t *testing.T, seconds int) []byte {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not in PATH")
	}
	path := filepath.Join(t.TempDir(), "tone.wav")
	cmd := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=2",
		"-ar", "48000", "-ac", "2", "-c:a", "pcm_s16le", path)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg tone render failed: %v: %s", err, out)
	}
	_ = seconds
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read tone: %v", err)
	}
	return data
}

func openStream(t *testing.T, data []byte, opts dsp.Options) *Stream {
	t.Helper()
	s, err := Engine{}.OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), opts)
	if err != nil {
		t.Fatalf("OpenStream: %v", err)
	}
	return s.(*Stream)
}

// drain reads frames until EOF and returns the count.
func drain(t *testing.T, s *Stream) int {
	t.Helper()
	n := 0
	for {
		_, _, err := s.ReadFrame()
		if err == io.EOF {
			return n
		}
		if err != nil {
			t.Fatalf("ReadFrame: %v", err)
		}
		n++
		if n > 10000 {
			t.Fatal("drain ran away")
		}
	}
}

func TestDecodesToneExactFrames(t *testing.T) {
	data := makeTone(t, 2)
	s := openStream(t, data, dsp.DefaultOptions())
	if n := drain(t, s); n != 200 {
		t.Errorf("frames = %d, want 200 for a 2s tone", n)
	}
}

func TestRejectsNonWAV(t *testing.T) {
	_, err := Engine{}.OpenStream(context.Background(), io.NopCloser(bytes.NewReader([]byte("garbage"))), dsp.DefaultOptions())
	if err == nil {
		t.Error("OpenStream accepted non-WAV input")
	}
	_, err = Engine{}.OpenStream(context.Background(), nil, dsp.DefaultOptions())
	if err == nil {
		t.Error("OpenStream accepted a nil reader")
	}
}

func TestSpeedChangesFrameCount(t *testing.T) {
	data := makeTone(t, 2)
	fast := openStream(t, data, dsp.Options{Speed: 2.0, Volume: 100})
	if n := drain(t, fast); n != 100 {
		t.Errorf("frames at speed 2.0 = %d, want 100", n)
	}
	slow := openStream(t, data, dsp.Options{Speed: 0.5, Volume: 100})
	if n := drain(t, slow); n != 400 {
		t.Errorf("frames at speed 0.5 = %d, want 400", n)
	}
	// nightcore is rate 1.25 on top of speed 1.0: 200/1.25 = 160.
	nc := openStream(t, data, dsp.Options{Speed: 1.0, Volume: 100, Effect: "nightcore"})
	if n := drain(t, nc); n != 160 {
		t.Errorf("frames for nightcore = %d, want 160", n)
	}
}

func TestVolumeSilence(t *testing.T) {
	data := makeTone(t, 2)
	s := openStream(t, data, dsp.Options{Speed: 1.0, Volume: 0})
	frame, _, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	for i, b := range frame {
		if b != 0 {
			t.Fatalf("volume 0 frame has non-zero byte %d at offset %d", b, i)
		}
	}
}

func TestBassChangesOutput(t *testing.T) {
	data := makeTone(t, 2)
	plain, _, err := openStream(t, data, dsp.DefaultOptions()).ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame plain: %v", err)
	}
	bassed, _, err := openStream(t, data, dsp.Options{Speed: 1.0, Volume: 100, Bass: 100}).ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame bass: %v", err)
	}
	if bytes.Equal(plain, bassed) {
		t.Error("bass 100 produced byte-identical output to bass 0")
	}
}

func Test8DPanVaries(t *testing.T) {
	data := makeTone(t, 2)
	s := openStream(t, data, dsp.Options{Speed: 1.0, Volume: 100, Effect: "8d"})
	first, _, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	// Read ~2.5 s worth of frames is impossible in a 2 s tone;
	// instead compare the first frame with a frame ~1 s in
	// (the 0.12 Hz LFO moves audibly over a second).
	var later []byte
	for i := 0; i < 100; i++ {
		later, _, err = s.ReadFrame()
		if err != nil {
			t.Fatalf("ReadFrame %d: %v", i, err)
		}
	}
	if bytes.Equal(first, later) {
		t.Error("8d produced identical frames 1 s apart; the LFO pan is not moving")
	}
}

func TestSetParamsConfirms(t *testing.T) {
	data := makeTone(t, 2)
	s := openStream(t, data, dsp.DefaultOptions())
	if _, _, err := s.ReadFrame(); err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	opts := dsp.Options{Speed: 1.25, Bass: 60, Volume: 90, Effect: "bass-boost"}
	if err := s.SetParams(opts); err != nil {
		t.Fatalf("SetParams: %v", err)
	}
	if got := s.LastParams(); got.Effect != "bass-boost" || got.Bass != 60 || got.Speed != 1.25 {
		t.Errorf("LastParams = %+v, want the SetParams options", got)
	}
	if err := s.SetParams(dsp.Options{Speed: 99}); err == nil {
		t.Error("SetParams accepted an out-of-range speed")
	}
}

func TestLoadNextTransitionExact(t *testing.T) {
	data := makeTone(t, 2)
	s := openStream(t, data, dsp.DefaultOptions())
	if err := s.LoadNext(io.NopCloser(bytes.NewReader(data)), dsp.DefaultOptions()); err != nil {
		t.Fatalf("LoadNext: %v", err)
	}
	// A second pending LoadNext is an error.
	if err := s.LoadNext(io.NopCloser(bytes.NewReader(data)), dsp.DefaultOptions()); err == nil {
		t.Error("second LoadNext succeeded")
	}
	frames := 0
	transitions := 0
	for {
		_, transition, err := s.ReadFrame()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("ReadFrame: %v", err)
		}
		frames++
		if transition {
			transitions++
		}
		if frames > 10000 {
			t.Fatal("drain ran away")
		}
	}
	if frames != 400 {
		t.Errorf("frames = %d, want 400 for 2s + 2s gapless", frames)
	}
	if transitions != 1 {
		t.Errorf("transitions = %d, want exactly 1", transitions)
	}
}

func TestCloseUnblocksBlockedRead(t *testing.T) {
	// A reader that stalls mid-stream: ReadFrame blocks in Read,
	// and Close must wake it promptly (this is what feed.Stop
	// relies on).
	data := makeTone(t, 2)
	pr, pw := io.Pipe()
	// Feed the header + one frame, then stall the writer. The
	// writer starts BEFORE OpenStream: the header parse blocks
	// until the bytes arrive.
	go func() {
		_, _ = pw.Write(data[:78+1920])
		// Hold the pipe open without writing: the next ReadFrame
		// blocks in Read until Close closes the reader.
		time.Sleep(10 * time.Second)
		_ = pw.Close()
	}()
	s, err := Engine{}.OpenStream(context.Background(), pr, dsp.DefaultOptions())
	if err != nil {
		t.Fatalf("OpenStream: %v", err)
	}
	stream := s.(*Stream)
	first, _, err := stream.ReadFrame()
	if err != nil {
		t.Fatalf("first ReadFrame: %v", err)
	}
	if len(first) != dsp.FrameBytes {
		t.Fatalf("first frame is %d bytes, want %d", len(first), dsp.FrameBytes)
	}
	done := make(chan error, 1)
	go func() {
		_, _, err := stream.ReadFrame()
		done <- err
	}()
	// Give the second ReadFrame a moment to block in Read.
	time.Sleep(200 * time.Millisecond)
	if err := stream.Close(); err != nil {
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
	if err := stream.Close(); err != nil {
		t.Errorf("second Close: %v", err)
	}
	if _, _, err := stream.ReadFrame(); err == nil {
		t.Error("ReadFrame on a closed stream succeeded")
	}
}

func TestCombinedSpeedAboveEngineLimitRejected(t *testing.T) {
	data := makeTone(t, 2)
	// Room speed 2.0 × nightcore 1.25 = 2.5 > 2.0: the real
	// engine would clamp silently; the fake must reject like
	// the binding does.
	if _, err := (Engine{}).OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), dsp.Options{Speed: 2.0, Volume: 100, Effect: "nightcore"}); err == nil {
		t.Error("OpenStream with combined speed 2.5 succeeded, want rejection")
	}
	// Room speed 4.0 alone is already over the engine limit.
	if _, err := (Engine{}).OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), dsp.Options{Speed: 4.0, Volume: 100}); err == nil {
		t.Error("OpenStream with speed 4.0 succeeded, want rejection")
	}
	// SetParams must reject it too, and leave the old params in
	// place.
	s := openStream(t, data, dsp.Options{Speed: 1.0, Volume: 100})
	if err := s.SetParams(dsp.Options{Speed: 3.0, Volume: 100}); err == nil {
		t.Error("SetParams with speed 3.0 succeeded, want rejection")
	}
	if got := s.LastParams().Speed; got != 1.0 {
		t.Errorf("LastParams after rejected SetParams = %v, want 1.0", got)
	}
}
