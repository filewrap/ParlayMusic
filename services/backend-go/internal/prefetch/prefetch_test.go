package prefetch

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"parlay/backend-go/internal/model"
)

// serveBytes returns an httptest server serving payload verbatim.
func serveBytes(t *testing.T, payload []byte) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(payload)
	}))
}

func TestPrefixBytesForLeadTime(t *testing.T) {
	if got := PrefixBytesForLeadTime(30); got != 30*32_000 {
		t.Errorf("30s lead = %d bytes, want %d", got, 30*32_000)
	}
	if got := PrefixBytesForLeadTime(0); got != 64<<10 {
		t.Errorf("0s lead = %d bytes, want the 64 KiB floor", got)
	}
	if got := PrefixBytesForLeadTime(3600); got != 4<<20 {
		t.Errorf("3600s lead = %d bytes, want the 4 MiB cap", got)
	}
}

// TestPrefetchBuffersOpeningSeconds is the prefetch proof: N+1's
// handle is pre-buffered before N ends — the Ready channel (not
// wall-clock luck) signals it, and the reader yields exactly the
// served bytes, prefix first, then the live body.
func TestPrefetchBuffersOpeningSeconds(t *testing.T) {
	payload := make([]byte, 200<<10)
	for i := range payload {
		payload[i] = byte(i % 251)
	}
	srv := serveBytes(t, payload)
	defer srv.Close()

	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		track.StreamURL = srv.URL + "/audio.mp3"
		return track, nil
	}
	p := New(resolve, 4096, 5*time.Second)
	track := model.Track{ID: "dQw4w9WgXcQ", Title: "Next", Source: "youtube"}
	p.Prefetch(1, track)

	// Observe readiness on the Ready channel (not wall-clock
	// luck): the channel is the signal the opening window is
	// buffered. The 5 s bound is failure detection only.
	p.mu.Lock()
	pending, ok := p.pending[1]
	p.mu.Unlock()
	if !ok {
		t.Fatal("no pending prefetch after Prefetch")
	}
	select {
	case <-pending.Ready():
	case <-time.After(5 * time.Second):
		t.Fatal("prefetch never became ready within 5s")
	}
	if pending.BufferedBytes() != 4096 {
		t.Errorf("BufferedBytes = %d, want 4096 (the opening window)", pending.BufferedBytes())
	}
	if err := pending.Err(); err != nil {
		t.Fatalf("prefetch Err = %v", err)
	}
	// Now Take hands the ready handle over — and only once.
	stream, ok := p.Take(1, track.ID)
	if !ok {
		t.Fatal("Take did not hand over the ready prefetch")
	}
	if _, ok := p.Take(1, track.ID); ok {
		t.Fatal("Take handed the same prefetch over twice")
	}
	if n := stream.BufferedBytes(); n != 4096 {
		t.Errorf("BufferedBytes after Take = %d, want 4096", n)
	}
	// The reader yields the whole served stream, byte-exact.
	data, err := io.ReadAll(stream.Reader())
	if err != nil {
		t.Fatalf("Reader: %v", err)
	}
	if len(data) != len(payload) {
		t.Fatalf("Reader yielded %d bytes, want %d", len(data), len(payload))
	}
	for i, b := range data {
		if b != payload[i] {
			t.Fatalf("Reader byte %d = %d, want %d", i, b, payload[i])
		}
	}
	_ = stream.Close()
}

// TestPrefetchNeverBlocks proves Prefetch returns immediately
// while the resolve runs in its own goroutine: the resolve blocks
// until the test releases it, and Prefetch must already have
// returned.
func TestPrefetchNeverBlocks(t *testing.T) {
	release := make(chan struct{})
	resolve := func(ctx context.Context, track model.Track) (model.Track, error) {
		select {
		case <-release:
		case <-ctx.Done():
			return model.Track{}, ctx.Err()
		}
		track.StreamURL = "http://127.0.0.1:9/nope"
		return track, nil
	}
	p := New(resolve, 4096, 200*time.Millisecond)
	track := model.Track{ID: "slow1", Title: "Slow", Source: "youtube"}

	start := time.Now()
	p.Prefetch(1, track)
	elapsed := time.Since(start)
	// MEASURED (not asserted as a performance bar): Prefetch must
	// return while the resolve is still blocked — a 500 ms bound
	// is orders of magnitude above a goroutine spawn.
	t.Logf("MEASURED Prefetch return with blocked resolver: %v", elapsed)
	if elapsed > 500*time.Millisecond {
		t.Errorf("Prefetch blocked the caller for %v while resolving", elapsed)
	}
	close(release)
	// The goroutine exits on its own (resolve timeout / release);
	// Cancel must not hang or panic.
	p.Cancel(1)
}

// TestPrefetchTakeOnlyWhenReady proves a prefetch still in flight
// is dropped (not waited on) and the play path would fall back to
// the direct URL.
func TestPrefetchTakeOnlyWhenReady(t *testing.T) {
	started := make(chan struct{})
	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		close(started)
		time.Sleep(2 * time.Second)
		track.StreamURL = "http://127.0.0.1:9/nope"
		return track, nil
	}
	p := New(resolve, 4096, 5*time.Second)
	p.Prefetch(1, model.Track{ID: "slow2", Source: "youtube"})
	<-started // the goroutine is inside the resolve now
	if _, ok := p.Take(1, "slow2"); ok {
		t.Error("Take handed over a prefetch still in flight")
	}
}

// TestPrefetchSkipsNonStreamable proves live, radio, file and
// soulseek tracks never start a resolve: the resolver is not
// called at all.
func TestPrefetchSkipsNonStreamable(t *testing.T) {
	calls := 0
	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		calls++
		return track, nil
	}
	p := New(resolve, 4096, time.Second)
	for _, track := range []model.Track{
		{ID: "live", Source: "youtube", IsLive: true},
		{ID: "radio:lofi", Source: "radio"},
		{ID: "file:abc", Source: "file"},
		{ID: "soulseek-x", Source: "soulseek"},
	} {
		p.Prefetch(1, track)
	}
	// Give any stray goroutine a chance to run, then check.
	time.Sleep(100 * time.Millisecond)
	if calls != 0 {
		t.Errorf("prefetch resolved %d non-streamable tracks, want 0", calls)
	}
}

// TestPrefetchReplacesStale proves a newer Prefetch for another
// track drops the old one: Take for the old id fails, Take for
// the new id succeeds once ready.
func TestPrefetchReplacesStale(t *testing.T) {
	payload := []byte("audio-bytes-here")
	srv := serveBytes(t, payload)
	defer srv.Close()
	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		track.StreamURL = srv.URL + "/audio.mp3"
		return track, nil
	}
	p := New(resolve, 64, 5*time.Second)
	a := model.Track{ID: "aaa", Source: "youtube"}
	b := model.Track{ID: "bbb", Source: "youtube"}
	p.Prefetch(1, a)
	p.Prefetch(1, b)
	if _, ok := p.Take(1, "aaa"); ok {
		t.Error("Take handed over the replaced prefetch")
	}
	p.mu.Lock()
	pending, ok := p.pending[1]
	p.mu.Unlock()
	if !ok || pending.Track.ID != "bbb" {
		t.Fatalf("pending prefetch = %+v, want track bbb", pending)
	}
	select {
	case <-pending.Ready():
	case <-time.After(5 * time.Second):
		t.Fatal("replacement prefetch never became ready within 5s")
	}
	s, ok := p.Take(1, "bbb")
	if !ok {
		t.Fatal("Take did not hand over the ready replacement prefetch")
	}
	if s.BufferedBytes() == 0 {
		t.Error("handed-over prefetch buffered nothing")
	}
	_ = s.Close()
}

// TestPrefetchResolveFailure proves a failed resolve is never
// handed over: Take reports false and the error is recorded on
// the stream.
func TestPrefetchResolveFailure(t *testing.T) {
	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		return model.Track{}, context.DeadlineExceeded
	}
	p := New(resolve, 4096, 100*time.Millisecond)
	p.Prefetch(1, model.Track{ID: "bad", Source: "youtube"})
	p.mu.Lock()
	pending, ok := p.pending[1]
	p.mu.Unlock()
	if !ok {
		t.Fatal("no pending prefetch after Prefetch")
	}
	select {
	case <-pending.Ready():
	case <-time.After(2 * time.Second):
		t.Fatal("failed prefetch never signalled Ready")
	}
	if err := pending.Err(); err == nil {
		t.Error("failed prefetch reports a nil error")
	}
	// A failed prefetch is never handed over.
	if _, ok := p.Take(1, "bad"); ok {
		t.Fatal("Take handed over a failed prefetch")
	}
}

// TestPeekDoesNotConsume proves Peek returns the pending prefetch
// without removing it: the loadNextWhenReady waiter Peeks, waits
// on Ready, and then claims the handle through Take — and a Peek
// for another track, or after Take, finds nothing.
func TestPeekDoesNotConsume(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte("fake audio bytes"))
	}))
	t.Cleanup(srv.Close)
	resolve := func(_ context.Context, track model.Track) (model.Track, error) {
		track.StreamURL = srv.URL
		return track, nil
	}
	p := New(resolve, 4096, 5*time.Second)
	p.Prefetch(1, model.Track{ID: "peek1", Source: "youtube"})
	if _, ok := p.Peek(1, "other"); ok {
		t.Error("Peek returned a prefetch for another track")
	}
	s, ok := p.Peek(1, "peek1")
	if !ok || s == nil {
		t.Fatal("Peek did not return the pending prefetch")
	}
	// Peek twice: still there (no consumption).
	if _, ok := p.Peek(1, "peek1"); !ok {
		t.Error("second Peek found nothing; Peek consumed the handle")
	}
	// Take removes it; a later Peek finds nothing.
	<-s.Ready()
	if _, ok := p.Take(1, "peek1"); !ok {
		t.Fatal("Take after Peek failed")
	}
	if _, ok := p.Peek(1, "peek1"); ok {
		t.Error("Peek returned a prefetch after Take")
	}
}
