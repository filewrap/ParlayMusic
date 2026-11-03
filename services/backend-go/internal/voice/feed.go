package voice

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"

	"parlay/backend-go/internal/dsp"
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
//
// The geometry constants live in the dsp package (dsp.FrameBytes
// etc.) — the DSP emits exactly these frames — and are re-exported
// here so existing callers keep working.
const (
	// FrameSamples is the number of samples per channel per frame.
	FrameSamples = dsp.FrameSamples
	// FrameBytes is the byte size of one frame: 480 samples * 2
	// channels * 2 bytes (s16le).
	FrameBytes = dsp.FrameBytes
	// FrameDuration is the playout duration of one frame.
	FrameDuration = 10 * time.Millisecond
	// SampleRateHz is the sample rate declared to ntgcalls.
	SampleRateHz = dsp.SampleRateHz
	// ChannelCount is the channel count declared to ntgcalls.
	ChannelCount = dsp.ChannelCount
)

// ---------------------------------------------------------------------------
// DSP FRAME PATH — Phase 5.
//
// The interim ffmpeg feed (feeder.go, deleted) is gone: no ffmpeg
// subprocess anywhere in the audio path. parlay-dsp decodes the
// stream to f32, applies the effect chain, resamples to 48 kHz
// stereo, limits, and dithers once to s16le; this feed pulls the
// resulting 10 ms frames from the DSP stream and pushes them to
// ntgcalls on a real-time schedule. The yt-dlp daemon is the
// resolver side and is untouched — it only resolves URLs, it never
// decodes audio for playback.
//
// The sink is the FrameSink interface (see below): the feed never
// calls ntgcalls directly, so the sink is one clearly-delimited
// seam. Today it is the PCM external-frame push
// (Client.SendExternalFrame); if Worker A's ntgcalls rebuild lands
// an Opus-encode path, only the sink implementation changes.
// ---------------------------------------------------------------------------

// FrameSink is the audio sink a feed pushes frames to: one method,
// one seam. *Client implements it with SendExternalFrame (the
// confirmed sink: 48 kHz stereo s16le, 1920-byte frames).
type FrameSink interface {
	SendFrame(chatID int64, data []byte) error
}

// dspFeed is one persistent DSP decode stream for one room. It
// pulls 10 ms frames from the DSP and pushes them to ntgcalls in
// real time, one frame at a time. It is started when a track
// starts and restarted only on track change — never per call,
// never per frame. A gapless transition (the DSP promoting its
// next slot) does NOT restart the feed: the pump keeps reading
// from the same stream and fires onTransition once.
type dspFeed struct {
	sink   FrameSink
	chatID int64
	stream dsp.Stream

	cancel   context.CancelFunc
	done     chan struct{}
	doneOnce sync.Once
	ctx      context.Context

	// onTransition is invoked exactly once when the DSP promotes
	// its next slot, with the track id LoadNext recorded for it.
	// The pump never blocks on it: the Manager forwards it to
	// handlers that dispatch to a worker goroutine.
	onTransition func(trackID string)

	// pauseGate blocks the pump while playback is paused. The
	// DSP keeps its decoded frames buffered (its feeder backs
	// off on the full buffer), so no decoded audio is lost and
	// playback resumes cleanly from the exact paused sample.
	pauseMu  sync.Mutex
	paused   bool
	resumeCh chan struct{}

	mu         sync.Mutex
	framesSent int64
	lastErr    error

	// nextTrackID is the track id LoadNext recorded for the
	// pending next slot, or "" when none is loaded. It is the
	// id onTransition fires with.
	nextMu      sync.Mutex
	nextTrackID string
}

// startDSPFeed starts the pump for stream and returns the feed.
// onEOF is invoked exactly once when the decode stream ends: with
// nil on a clean end of stream (track finished), or with the exact
// error when the DSP died or the frame pump failed. onTransition
// is invoked exactly once per loaded next slot, when the DSP
// promotes it. A nil onEOF/onTransition is allowed.
func startDSPFeed(ctx context.Context, sink FrameSink, chatID int64, stream dsp.Stream, onEOF func(error), onTransition func(string)) (*dspFeed, error) {
	if sink == nil {
		return nil, errors.New("voice: dsp feed: nil frame sink")
	}
	if stream == nil {
		return nil, errors.New("voice: dsp feed: nil DSP stream")
	}
	feedCtx, cancel := context.WithCancel(ctx)
	feed := &dspFeed{
		sink:         sink,
		chatID:       chatID,
		stream:       stream,
		ctx:          feedCtx,
		cancel:       cancel,
		done:         make(chan struct{}),
		resumeCh:     make(chan struct{}),
		onTransition: onTransition,
	}
	go feed.pump(onEOF)
	return feed, nil
}

// pump pulls 10 ms frames from the DSP and pushes them to ntgcalls
// on a real-time schedule: frame N is sent no earlier than start +
// N*10 ms, so a fast decoder cannot flood WebRTC's audio source
// with the whole track at once. If the pump falls behind schedule
// it sends the next frame immediately instead of bursting.
func (f *dspFeed) pump(onEOF func(error)) {
	defer close(f.done)
	start := time.Now()
	var frameIndex int64
	for {
		// Pause gate: while paused, stop pulling frames (see
		// the pauseGate field comment) instead of sending.
		for {
			f.pauseMu.Lock()
			paused := f.paused
			ch := f.resumeCh
			f.pauseMu.Unlock()
			if !paused {
				break
			}
			select {
			case <-ch:
			case <-f.ctx.Done():
				return
			}
		}
		frame, transition, err := f.stream.ReadFrame()
		if err != nil {
			// The decode stream ended. io.EOF is a clean end
			// of track; any other error is the exact DSP
			// failure. An intentional stop (Stop called) is
			// not a feed event: the caller already knows it
			// stopped the feed, so onEOF is not invoked for it.
			var feedErr error
			if err != io.EOF {
				feedErr = fmt.Errorf("voice: dsp feed: %w", err)
			}
			f.finish(feedErr)
			if onEOF != nil && f.ctx.Err() == nil {
				onEOF(feedErr)
			}
			return
		}
		deadline := start.Add(time.Duration(frameIndex) * FrameDuration)
		if wait := time.Until(deadline); wait > 0 {
			select {
			case <-time.After(wait):
			case <-f.ctx.Done():
				return
			}
		}
		if err := f.sink.SendFrame(f.chatID, frame); err != nil {
			feedErr := fmt.Errorf("voice: dsp feed: SendFrame: %w", err)
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
		if transition {
			f.fireTransition()
		}
	}
}

// fireTransition invokes onTransition exactly once with the track
// id LoadNext recorded, then clears it. It runs on the pump
// goroutine, so the callback must never block (the Manager's
// onTransition forwards to handlers, which dispatch to a worker
// goroutine).
func (f *dspFeed) fireTransition() {
	f.nextMu.Lock()
	trackID := f.nextTrackID
	f.nextTrackID = ""
	f.nextMu.Unlock()
	if trackID == "" || f.onTransition == nil {
		return
	}
	f.onTransition(trackID)
}

func (f *dspFeed) finish(err error) {
	f.doneOnce.Do(func() {
		f.mu.Lock()
		f.lastErr = err
		f.mu.Unlock()
	})
}

// LoadNext loads the next slot of the running DSP stream: the
// track that will start gaplessly when the active slot ends.
// trackID is the id onTransition fires with when the promotion
// happens. At most one next slot may be pending.
func (f *dspFeed) LoadNext(r io.ReadCloser, opts dsp.Options, trackID string) error {
	if r == nil {
		return errors.New("voice: dsp feed: LoadNext with nil reader")
	}
	if trackID == "" {
		return errors.New("voice: dsp feed: LoadNext with empty track id")
	}
	f.nextMu.Lock()
	if f.nextTrackID != "" {
		f.nextMu.Unlock()
		return errors.New("voice: dsp feed: next slot already loaded")
	}
	f.nextMu.Unlock()
	if err := f.stream.LoadNext(r, opts); err != nil {
		return fmt.Errorf("voice: dsp feed: LoadNext: %w", err)
	}
	f.nextMu.Lock()
	f.nextTrackID = trackID
	f.nextMu.Unlock()
	return nil
}

// ApplyOptions applies a live zero-restart parameter change to the
// running DSP stream. A nil error is the DSP's confirmation that
// the effect is active.
func (f *dspFeed) ApplyOptions(opts dsp.Options) error {
	if err := f.stream.SetParams(opts); err != nil {
		return fmt.Errorf("voice: dsp feed: ApplyOptions: %w", err)
	}
	return nil
}

// FramesSent returns the number of 10 ms frames pushed to ntgcalls.
func (f *dspFeed) FramesSent() int64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.framesSent
}

// Err returns the terminal error of a finished feed, or nil when the
// feed ended cleanly at end of stream or is still running.
func (f *dspFeed) Err() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastErr
}

// Pause stops the pump from pulling frames until Resume is called.
// Safe to call when already paused.
func (f *dspFeed) Pause() {
	f.pauseMu.Lock()
	defer f.pauseMu.Unlock()
	if !f.paused {
		f.paused = true
		f.resumeCh = make(chan struct{})
	}
}

// Resume unblocks a paused pump. Safe to call when not paused.
func (f *dspFeed) Resume() {
	f.pauseMu.Lock()
	defer f.pauseMu.Unlock()
	if f.paused {
		f.paused = false
		close(f.resumeCh)
	}
}

// Stop closes the DSP stream (which unblocks a ReadFrame waiting
// for data) and waits for the pump goroutine to exit. It is safe
// to call more than once and on a feed that already ended.
func (f *dspFeed) Stop() {
	f.Resume()
	f.cancel()
	_ = f.stream.Close()
	<-f.done
}
