package voice

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strings"
	"sync"
	"time"
)

// Frame geometry for the external audio source.
//
// ntgcalls derives the audio frame from the sink, not from the pushed
// buffer: AudioSink::frame_size() is sample_rate * 16 / 8 / 100 *
// channel_count and frame_time() is 10 ms, so at the 48 kHz stereo
// declared by SetExternalAudioSource one frame is 480 samples,
// 1920 bytes, 10 ms. AudioStreamer::send_data() hands exactly that
// many samples to WebRTC per frame. SendExternalFrame enforces this
// size so a caller can never silently feed a mis-sized buffer.
const (
	// FrameSamples is the number of samples per channel per frame.
	FrameSamples = 480
	// FrameBytes is the byte size of one frame: 480 samples * 2
	// channels * 2 bytes (s16le).
	FrameBytes = FrameSamples * 2 * 2
	// FrameDuration is the playout duration of one frame.
	FrameDuration = 10 * time.Millisecond
	// SampleRateHz is the sample rate declared to ntgcalls.
	SampleRateHz = 48000
	// ChannelCount is the channel count declared to ntgcalls.
	ChannelCount = 2
)

// ---------------------------------------------------------------------------
// INTERIM FRAME PATH — Phase 5 replacement target.
//
// Verdict from the source inspection (full evidence in phase3/voice.md):
// ntg_set_stream_sources can NOT take a resolved stream URL or a
// compressed audio file directly.
//
//   - FILE: FileReader reads raw bytes from a local path and hands
//     them straight to the audio sink. No decoder, no HTTP. A URL
//     fails to open as a file; an MP3/FLAC/Opus file would be fed to
//     WebRTC as raw s16le garbage.
//   - FFMPEG: MediaSourceFactory::from_input throws FFmpegError
//     ("FFmpeg encoder is not yet supported") for this source.
//   - SHELL: runs a shell command and reads raw PCM from its stdout.
//     TgMusicBot and the old core voice_call.cpp both used this with
//     an ffmpeg decode-to-s16le command — one ffmpeg subprocess
//     spawned by ntgcalls per track, which the locked architecture
//     (§1.2.3, no subprocess per call) prohibits on the primary path.
//   - EXTERNAL: no reader is created; the caller pushes raw s16le
//     PCM through ntg_send_external_frame, and AudioStreamer treats
//     the bytes as 16-bit samples at the declared rate/channels.
//
// So until parlay-dsp exists (Phase 5), audio reaches ntgcalls through
// the EXTERNAL source fed by ONE persistent ffmpeg process per room,
// started when a track starts and restarted only on track change —
// never per call, never per frame. Phase 5 replaces this file whole:
// parlay-dsp decodes to f32, applies the effect chain, dithers once
// to s16le, and Go pushes the resulting 480-sample frames through
// SendExternalFrame, with no ffmpeg anywhere in the path.
// ---------------------------------------------------------------------------

// ffmpegFeed is one persistent ffmpeg decode process for one room. It
// decodes the resolved stream (URL or local file) to s16le 48 kHz
// stereo PCM on stdout and pumps it to ntgcalls in real time, one
// 10 ms frame at a time.
type ffmpegFeed struct {
	client *Client
	chatID int64
	source string

	cancel   context.CancelFunc
	cmd      *exec.Cmd
	done     chan struct{}
	doneOnce sync.Once
	ctx      context.Context

	// stderr keeps the tail of ffmpeg's diagnostics so a failed
	// decode reports its exact reason instead of a bare exit code.
	stderr *stderrCapture
	// waitOnce serialises cmd.Wait: the pump calls it when the
	// decode stream ends, Stop calls it while killing the process.
	waitOnce sync.Once
	waitErr  error

	// pauseGate blocks the pump while playback is paused. ntgcalls'
	// update_pause only touches file/shell readers, so the external
	// source is never paused by the library (see voice.go Pause);
	// the feed pauses itself instead. While paused the pump stops
	// reading ffmpeg's stdout; the pipe buffer fills and ffmpeg
	// blocks on write, so no decoded audio is lost and playback
	// resumes cleanly from the exact paused sample.
	pauseMu  sync.Mutex
	paused   bool
	resumeCh chan struct{}

	mu         sync.Mutex
	framesSent int64
	lastErr    error
}

// startFFmpegFeed starts the persistent decode process for source and
// begins pumping frames to chatID. onEOF is invoked exactly once when
// the decode stream ends: with nil when ffmpeg reached end of stream
// cleanly (track finished), or with the exact error when ffmpeg died
// or the frame pump failed. source is passed to ffmpeg as argv, never
// through a shell, so a hostile stream URL cannot inject shell syntax.
func startFFmpegFeed(ctx context.Context, client *Client, chatID int64, source string, onEOF func(error)) (*ffmpegFeed, error) {
	if strings.TrimSpace(source) == "" {
		return nil, errors.New("voice: ffmpeg feed: empty audio source")
	}
	return startFeed(ctx, client, chatID, source, nil, onEOF)
}

// startFFmpegFeedFromReader starts the decode feed with ffmpeg
// reading its input from r (stdin, "-i pipe:0") instead of a URL
// or file path: the Phase 4 prefetch path (Manager.PlayStream).
// The reader must yield the whole stream (buffered prefix first,
// then the live body); EOF on it ends the track exactly like EOF
// on a URL/file feed, and the pump's real-time pacing is
// unchanged.
func startFFmpegFeedFromReader(ctx context.Context, client *Client, chatID int64, r io.ReadCloser, onEOF func(error)) (*ffmpegFeed, error) {
	if r == nil {
		return nil, errors.New("voice: ffmpeg feed: nil stream reader")
	}
	return startFeed(ctx, client, chatID, "pipe:0", r, onEOF)
}

// startFeed is the shared feed construction: ffmpeg decodes input
// (a URL/file path, or stdin when stdin != nil) to s16le 48 kHz
// stereo PCM on stdout and the pump pushes 10 ms frames to
// ntgcalls. source is passed to ffmpeg as argv, never through a
// shell, so a hostile stream URL cannot inject shell syntax.
func startFeed(ctx context.Context, client *Client, chatID int64, input string, stdin io.Reader, onEOF func(error)) (*ffmpegFeed, error) {
	ffmpegPath, err := exec.LookPath("ffmpeg")
	if err != nil {
		return nil, fmt.Errorf("voice: ffmpeg feed: ffmpeg not found in PATH: %w", err)
	}
	feedCtx, cancel := context.WithCancel(ctx)
	args := []string{"-hide_banner", "-loglevel", "error"}
	if stdin == nil && (strings.HasPrefix(input, "http://") || strings.HasPrefix(input, "https://")) {
		args = append(args, "-reconnect", "1", "-reconnect_at_eof", "1",
			"-reconnect_streamed", "1", "-reconnect_delay_max", "2")
	}
	args = append(args, "-i", input, "-vn",
		"-f", "s16le", "-ac", "2", "-ar", "48000", "pipe:1")
	cmd := exec.CommandContext(feedCtx, ffmpegPath, args...)
	if stdin != nil {
		cmd.Stdin = stdin
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		cancel()
		return nil, fmt.Errorf("voice: ffmpeg feed: cannot attach stdout pipe: %w", err)
	}
	stderr := &stderrCapture{}
	cmd.Stderr = stderr
	if err := cmd.Start(); err != nil {
		cancel()
		return nil, fmt.Errorf("voice: ffmpeg feed: cannot start ffmpeg: %w", err)
	}
	feed := &ffmpegFeed{
		client:   client,
		chatID:   chatID,
		source:   input,
		ctx:      feedCtx,
		cancel:   cancel,
		cmd:      cmd,
		done:     make(chan struct{}),
		resumeCh: make(chan struct{}),
		stderr:   stderr,
	}
	go feed.pump(stdout, onEOF)
	return feed, nil
}

// stderrCapture keeps the last 4 KiB of ffmpeg's stderr so a failed
// decode can report the exact reason ffmpeg gave (e.g. "No such file
// or directory") instead of a bare exit status.
type stderrCapture struct {
	mu  sync.Mutex
	buf []byte
}

func (s *stderrCapture) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.buf = append(s.buf, p...)
	if len(s.buf) > 4096 {
		s.buf = s.buf[len(s.buf)-4096:]
	}
	return len(p), nil
}

func (s *stderrCapture) tail() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return strings.TrimSpace(string(s.buf))
}

// pump reads 10 ms PCM frames from ffmpeg stdout and pushes them to
// ntgcalls on a real-time schedule: frame N is sent no earlier than
// start + N*10 ms, so a fast decoder cannot flood WebRTC's audio
// source with the whole track at once. If the pump falls behind
// schedule it sends the next frame immediately instead of bursting.
func (f *ffmpegFeed) pump(stdout io.ReadCloser, onEOF func(error)) {
	defer close(f.done)
	defer stdout.Close()
	buf := make([]byte, FrameBytes)
	start := time.Now()
	var frameIndex int64
	for {
		// Pause gate: while paused, stop consuming ffmpeg output
		// (see the pauseGate field comment) instead of sending.
		for {
			f.pauseMu.Lock()
			paused := f.paused
			ch := f.resumeCh
			f.pauseMu.Unlock()
			if !paused {
				break
			}
			<-ch
		}
		_, err := io.ReadFull(stdout, buf)
		if err != nil {
			// The decode stream ended. Whether that is a clean
			// end of track or a failed decode is decided by the
			// ffmpeg exit status, not by the read error: a
			// missing input file closes stdout with zero bytes,
			// which reads as plain EOF. A trailing partial frame
			// is dropped, never padded with invented audio.
			feedErr := f.terminalErr()
			f.finish(feedErr)
			// An intentional stop (Stop called) is not a feed
			// event: the caller already knows it stopped the
			// feed, so onEOF is not invoked for it.
			if onEOF != nil && f.ctx.Err() == nil {
				onEOF(feedErr)
			}
			return
		}
		deadline := start.Add(time.Duration(frameIndex) * FrameDuration)
		if wait := time.Until(deadline); wait > 0 {
			time.Sleep(wait)
		}
		if err := f.client.SendExternalFrame(f.chatID, buf); err != nil {
			feedErr := fmt.Errorf("voice: ffmpeg feed: SendExternalFrame: %w", err)
			f.finish(feedErr)
			if onEOF != nil && f.ctx.Err() == nil {
				onEOF(feedErr)
			}
			return
		}
		f.mu.Lock()
		f.framesSent++
		f.mu.Unlock()
		frameIndex++
	}
}

func (f *ffmpegFeed) finish(err error) {
	f.doneOnce.Do(func() {
		f.mu.Lock()
		f.lastErr = err
		f.mu.Unlock()
	})
}

// wait reaps the ffmpeg process exactly once; the pump and Stop
// both need the exit status, and calling cmd.Wait twice is an error.
func (f *ffmpegFeed) wait() error {
	f.waitOnce.Do(func() {
		f.waitErr = f.cmd.Wait()
	})
	return f.waitErr
}

// terminalErr decides how the decode stream ended. An intentional
// stop reports nil (the caller knows it stopped the feed). A clean
// ffmpeg exit reports nil (end of track). Any other exit reports the
// exact ffmpeg reason, including the tail of its stderr.
func (f *ffmpegFeed) terminalErr() error {
	if f.ctx.Err() != nil {
		return nil
	}
	if err := f.wait(); err != nil {
		if tail := f.stderr.tail(); tail != "" {
			return fmt.Errorf("voice: ffmpeg feed: ffmpeg exited: %v: %s", err, tail)
		}
		return fmt.Errorf("voice: ffmpeg feed: ffmpeg exited: %w", err)
	}
	return nil
}

// FramesSent returns the number of 10 ms PCM frames pushed to ntgcalls.
func (f *ffmpegFeed) FramesSent() int64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.framesSent
}

// Err returns the terminal error of a finished feed, or nil when the
// feed ended cleanly at end of stream or is still running.
func (f *ffmpegFeed) Err() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastErr
}

// Pause stops the pump from consuming decoded audio until Resume
// is called. Safe to call when already paused.
func (f *ffmpegFeed) Pause() {
	f.pauseMu.Lock()
	defer f.pauseMu.Unlock()
	if !f.paused {
		f.paused = true
		f.resumeCh = make(chan struct{})
	}
}

// Resume unblocks a paused pump. Safe to call when not paused.
func (f *ffmpegFeed) Resume() {
	f.pauseMu.Lock()
	defer f.pauseMu.Unlock()
	if f.paused {
		f.paused = false
		close(f.resumeCh)
	}
}

// Stop kills the ffmpeg process and waits for the pump goroutine to
// exit. It is safe to call more than once and on a feed that already
// ended.
func (f *ffmpegFeed) Stop() {
	f.Resume()
	f.cancel()
	if f.cmd != nil && f.cmd.Process != nil {
		_ = f.cmd.Process.Kill()
		_ = f.wait()
	}
	<-f.done
}
