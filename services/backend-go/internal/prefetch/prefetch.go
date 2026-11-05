// Package prefetch implements the N+1 lookahead prefetch
// (FINAL-ARCHITECTURE.md §9 Phase 4): when track N starts (or N+1
// is promoted to queue top) a background goroutine re-resolves N+1
// and pre-opens its stream, buffering the opening seconds. The
// result is a PrebufferedStream — a REAL handle the voice play path
// consumes today: voice.Manager.PlayStream feeds it to the interim
// ffmpeg decode path via stdin, so N+1 starts from bytes that are
// already in memory instead of waiting on a fresh HTTP open. In
// Phase 5 parlay-dsp plugs in as the consumer of that same handle
// type (the DSP decodes from the reader instead of ffmpeg). There
// is no stub DSP consumer and none is needed: the handle is
// consumed by the existing voice path, and the seam is this type.
//
// A prefetch never blocks the playing track: Prefetch returns
// immediately and all network work happens in its own goroutine.
// Take hands over only a handle that is already ready; a prefetch
// still in flight when the advance fires is dropped and the play
// path falls back to the direct stream URL. The resolve +
// open + prefix-read are bounded by resolveTimeout; the live body
// behind a handed-over handle is NOT bounded by it — its context
// is cancelled only when the stream is closed or dropped.
package prefetch

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"parlay/backend-go/internal/model"
)

// ResolveFunc re-resolves a known track to a fresh stream URL
// (registry.ResolveTrack). It is a function, not a Registry, so
// tests can substitute a stub without a store.
type ResolveFunc func(ctx context.Context, track model.Track) (model.Track, error)

// PrebufferedStream is one pre-opened N+1 stream: the opening
// window is already buffered in memory and the live HTTP body
// continues behind it. Ready closes when the opening window is
// buffered (or the open failed); Err reports the outcome; Reader
// returns the consumable stream (buffered prefix first, then the
// live body). The DSP seam: Phase 5 consumes this same type.
type PrebufferedStream struct {
	Track model.Track

	ready  chan struct{}
	mu     sync.Mutex
	prefix []byte
	body   io.ReadCloser
	err    error
	cancel context.CancelFunc
	out    bool // Reader has transferred body ownership out
	// closed is set by Close: run aborts when it sees it, so a
	// stream dropped before its goroutine started leaks no body.
	closed bool
}

// Ready returns a channel closed when the opening window is
// buffered (or the open failed). Err is valid after it closes.
func (s *PrebufferedStream) Ready() <-chan struct{} { return s.ready }

// Err returns the open failure, or nil once Ready has closed
// successfully. Valid only after Ready closes.
func (s *PrebufferedStream) Err() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.err
}

// BufferedBytes returns the number of opening-window bytes already
// buffered in memory.
func (s *PrebufferedStream) BufferedBytes() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.prefix)
}

// Reader returns the consumable stream: the buffered prefix first,
// then the live HTTP body, ending at the true end of the stream.
// It may be called once; the caller owns the returned ReadCloser
// (closing it aborts the body). Calling it on a failed stream
// returns a reader that reports the failure on Read.
func (s *PrebufferedStream) Reader() io.ReadCloser {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.err != nil {
		return &errReader{err: s.err}
	}
	if s.body == nil {
		return io.NopCloser(bytes.NewReader(s.prefix))
	}
	s.out = true
	return &prefetchReader{prefix: bytes.NewReader(s.prefix), body: s.body, cancel: s.cancel}
}

// Close drops the stream: the in-flight open (if any) is aborted
// and the body closed. Idempotent; a no-op after Reader took the
// body out (the reader owns it then).
func (s *PrebufferedStream) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	if s.cancel != nil {
		s.cancel()
	}
	if s.body != nil && !s.out {
		err := s.body.Close()
		s.body = nil
		return err
	}
	return nil
}

// prefetchReader drains the buffered prefix, then the live body.
type prefetchReader struct {
	prefix *bytes.Reader
	body   io.ReadCloser
	cancel context.CancelFunc
}

func (r *prefetchReader) Read(p []byte) (int, error) {
	if r.prefix.Len() > 0 {
		return r.prefix.Read(p)
	}
	return r.body.Read(p)
}

func (r *prefetchReader) Close() error {
	if r.cancel != nil {
		r.cancel()
	}
	return r.body.Close()
}

// errReader reports a failed open on Read.
type errReader struct{ err error }

func (r *errReader) Read(_ []byte) (int, error) { return 0, r.err }
func (r *errReader) Close() error               { return nil }

// Prefetcher runs N+1 prefetches: one pending stream per chat at
// most (a newer Prefetch for another track replaces the old one).
type Prefetcher struct {
	resolve        ResolveFunc
	client         *http.Client
	prefixBytes    int
	resolveTimeout time.Duration

	mu      sync.Mutex
	pending map[int64]*PrebufferedStream
}

// New builds a Prefetcher. prefixBytes is the opening window to
// buffer (see PrefixBytesForLeadTime); resolveTimeout bounds the
// resolve + open + prefix-read, not the handed-over body.
func New(resolve ResolveFunc, prefixBytes int, resolveTimeout time.Duration) *Prefetcher {
	return &Prefetcher{
		resolve: resolve, client: &http.Client{Transport: http.DefaultTransport},
		prefixBytes: prefixBytes, resolveTimeout: resolveTimeout,
		pending: map[int64]*PrebufferedStream{},
	}
}

// PrefixBytesForLeadTime converts the configured prefetch lead time
// (limits.prefetch_lead_time_sec) into a buffer budget: lead
// seconds of audio at ~256 kbps (32 kB/s), clamped to [64 KiB,
// 4 MiB]. The clamp keeps a pathological lead time from
// buffering a whole album in RAM.
func PrefixBytesForLeadTime(leadSec int) int {
	n := leadSec * 32_000
	if n < 64<<10 {
		return 64 << 10
	}
	if n > 4<<20 {
		return 4 << 20
	}
	return n
}

// Prefetch starts the background prefetch of track for chatID and
// returns immediately — it never blocks the playing track. A
// pending prefetch for the same track is left alone; a pending one
// for another track is dropped. Live, radio, file and soulseek
// tracks are skipped: they have no pre-openable HTTP stream
// (live is a live stream, file is local, soulseek downloads).
func (p *Prefetcher) Prefetch(chatID int64, track model.Track) {
	switch track.Source {
	case "radio", "file", "soulseek":
		return
	}
	if track.IsLive {
		return
	}
	p.mu.Lock()
	if old, ok := p.pending[chatID]; ok && old.Track.ID == track.ID {
		p.mu.Unlock()
		return
	}
	if old, ok := p.pending[chatID]; ok {
		delete(p.pending, chatID)
		go old.Close()
	}
	s := &PrebufferedStream{Track: track, ready: make(chan struct{})}
	p.pending[chatID] = s
	p.mu.Unlock()
	go p.run(chatID, s)
}

// Take hands over the ready prefetch for chatID's track, or false
// when there is none, it is for another track, it failed, or it is
// still in flight (a still-in-flight prefetch is dropped: the play
// path falls back to the direct stream URL rather than waiting).
func (p *Prefetcher) Take(chatID int64, trackID string) (*PrebufferedStream, bool) {
	p.mu.Lock()
	s, ok := p.pending[chatID]
	if !ok || s.Track.ID != trackID {
		p.mu.Unlock()
		return nil, false
	}
	select {
	case <-s.Ready():
	default:
		delete(p.pending, chatID)
		p.mu.Unlock()
		_ = s.Close()
		return nil, false
	}
	delete(p.pending, chatID)
	p.mu.Unlock()
	if err := s.Err(); err != nil {
		_ = s.Close()
		return nil, false
	}
	return s, true
}

// Peek returns the pending prefetch for chatID's track WITHOUT
// removing it, or false when there is none or it is for another
// track. The loadNextWhenReady waiter uses it to wait on Ready
// and then claim the handle through Take (single ownership with
// the advance-time Take in bindTrack).
func (p *Prefetcher) Peek(chatID int64, trackID string) (*PrebufferedStream, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	s, ok := p.pending[chatID]
	if !ok || s.Track.ID != trackID {
		return nil, false
	}
	return s, true
}

// Cancel drops the pending prefetch for chatID, if any.
func (p *Prefetcher) Cancel(chatID int64) {
	p.mu.Lock()
	s, ok := p.pending[chatID]
	if ok {
		delete(p.pending, chatID)
	}
	p.mu.Unlock()
	if ok {
		_ = s.Close()
	}
}

// run performs one prefetch: resolve, open, buffer the opening
// window. Ready closes exactly once, when run returns.
func (p *Prefetcher) run(chatID int64, s *PrebufferedStream) {
	defer close(s.ready)
	ctx, cancel := context.WithCancel(context.Background())
	s.mu.Lock()
	s.cancel = cancel
	dropped := s.closed
	s.mu.Unlock()
	if dropped {
		// Dropped before the goroutine started: abort, leaking
		// no body and no resolve.
		return
	}

	resolveCtx, cancelResolve := context.WithTimeout(context.Background(), p.resolveTimeout)
	fresh, err := p.resolve(resolveCtx, s.Track)
	cancelResolve()
	if err != nil {
		s.setErr(fmt.Errorf("prefetch resolve: %w", err))
		return
	}
	if !strings.HasPrefix(fresh.StreamURL, "http://") && !strings.HasPrefix(fresh.StreamURL, "https://") {
		s.setErr(fmt.Errorf("prefetch: track %s has no HTTP stream URL", fresh.ID))
		return
	}
	// A YouTube signed URL 403s for a few seconds after it is
	// issued (the videoplayback edge's signature check lags the
	// player API's response), and this open runs immediately after
	// the re-resolve. Retry a 403 with a 2s backoff (up to 4
	// retries), like the voice play path does.
	var resp *http.Response
	for attempt := 0; ; attempt++ {
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, fresh.StreamURL, nil)
		if err != nil {
			s.setErr(fmt.Errorf("prefetch open: %w", err))
			return
		}
		resp, err = p.client.Do(req)
		if err != nil {
			s.setErr(fmt.Errorf("prefetch open: %w", err))
			return
		}
		if resp.StatusCode >= 200 && resp.StatusCode < 300 {
			break
		}
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden || attempt >= 4 {
			s.setErr(fmt.Errorf("prefetch open: HTTP %d for %s", resp.StatusCode, fresh.StreamURL))
			return
		}
		select {
		case <-ctx.Done():
			s.setErr(fmt.Errorf("prefetch open: %w", ctx.Err()))
			return
		case <-time.After(2 * time.Second):
		}
	}
	prefix, err := readUpTo(resp.Body, p.prefixBytes)
	if err != nil {
		_ = resp.Body.Close()
		s.setErr(fmt.Errorf("prefetch buffer: %w", err))
		return
	}
	// Download validation (queue-port Tier 2, item 15 — the
	// in-memory twin of the voice openSource check): the
	// buffered prefix must be non-empty media, not a JSON/HTML
	// error body masquerading as a stream. A failed validation
	// fails the prefetch, and the bind path falls back to the
	// direct URL — it never hands the DSP an error document.
	if err := validateMediaPrefix(prefix); err != nil {
		_ = resp.Body.Close()
		s.setErr(fmt.Errorf("prefetch buffer: %w", err))
		return
	}
	// Success: the fresh track (with its fresh StreamURL) is the
	// handle's track, and the body continues behind the prefix.
	s.mu.Lock()
	s.Track = fresh
	s.prefix = prefix
	s.body = resp.Body
	s.mu.Unlock()
}

// validateMediaPrefix rejects a buffered prefix that is not
// media: empty (0 bytes), or opening with `{`, `<`, or `[` — a
// JSON/HTML error body masquerading as a stream (NoxBeats'
// YouTube::download check, queue-port Tier 2 item 15). No real
// media format starts with those bytes (MP3 "ID3"/0xFF, FLAC
// "fLaC", OGG "OggS", WebM 0x1A, MP4 box size + "ftyp", WAV
// "RIFF"). The voice openSource path applies the same check to
// the streaming body; this one covers the prefetch path.
func validateMediaPrefix(prefix []byte) error {
	if len(prefix) == 0 {
		return fmt.Errorf("empty stream (0 bytes)")
	}
	switch prefix[0] {
	case '{', '<', '[':
		return fmt.Errorf("stream starts with %q — an error document, not media", string(prefix[0]))
	}
	return nil
}

func (s *PrebufferedStream) setErr(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.err = err
	if s.body != nil {
		_ = s.body.Close()
		s.body = nil
	}
}

// readUpTo reads up to n bytes: a short stream (EOF before n)
// returns what was read with a nil error; a real read failure is
// an error.
func readUpTo(r io.Reader, n int) ([]byte, error) {
	buf := make([]byte, 0, n)
	tmp := make([]byte, 8192)
	for len(buf) < n {
		want := n - len(buf)
		if want > len(tmp) {
			want = len(tmp)
		}
		m, err := r.Read(tmp[:want])
		buf = append(buf, tmp[:m]...)
		if err != nil {
			if err == io.EOF {
				return buf, nil
			}
			return buf, err
		}
		if m == 0 {
			return buf, nil
		}
	}
	return buf, nil
}
