package voice

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/dsp/fake"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/prefetch"
)

// libPath resolves the real libntgcalls.so under test (the same
// library the cgo link uses), relative to this test file.
func libPath(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	root := filepath.Dir(file)
	for i := 0; i < 4; i++ {
		root = filepath.Dir(root)
	}
	return filepath.Join(root, "refs", "ntgcalls", "shared-output", "lib", "libntgcalls.so")
}

// requiredSymbols is every ntg_ symbol this package binds, plus the
// OD-1 Opus encoder config API (ntgcalls_opus.h) whose Go binding lands
// with the Phase 5 DSP/voice work -- the symbols must already be exported
// by the patched libntgcalls.so. The cgo link already fails if a
// referenced symbol is absent; this test re-verifies the whole set
// explicitly so a missing symbol is reported by name instead of as a
// linker error.
var requiredSymbols = []string{
	"ntg_instance_create",
	"ntg_instance_destroy",
	"ntg_create_call",
	"ntg_connect",
	"ntg_set_stream_sources",
	"ntg_pause",
	"ntg_resume",
	"ntg_mute",
	"ntg_unmute",
	"ntg_stop",
	"ntg_send_external_frame",
	"ntg_on_stream_end_callback",
	"ntg_on_connection_change_callback",
	"ntg_get_version",
	"ntg_last_error",
	"ntg_opus_set_config",
	"ntg_opus_get_config",
	"ntg_opus_set_bitrate",
	"ntg_string_free",
	"ntg_get_state",
	"ntg_time",
}

func TestRequiredSymbols(t *testing.T) {
	nm, err := exec.LookPath("nm")
	if err != nil {
		t.Skip("nm not in PATH, cannot verify symbols")
	}
	out, err := exec.Command(nm, "-D", libPath(t)).Output()
	if err != nil {
		t.Fatalf("nm -D on libntgcalls.so failed: %v", err)
	}
	for _, sym := range requiredSymbols {
		if !strings.Contains(string(out), " T "+sym+"\n") && !strings.Contains(string(out), " T "+sym+" ") {
			t.Errorf("required symbol %s is absent from libntgcalls.so", sym)
		}
	}
}

func TestVersion(t *testing.T) {
	version := Version()
	if version == "" {
		t.Fatal("ntg_get_version returned an empty string")
	}
	t.Logf("libntgcalls version: %s", version)
	if !strings.Contains(version, "3.0.5") {
		t.Errorf("expected libntgcalls 3.0.5, got %q", version)
	}
}

func newTestClient(t *testing.T) *Client {
	t.Helper()
	c, err := New()
	if err != nil {
		t.Fatalf("New() failed: %v", err)
	}
	t.Cleanup(c.Close)
	return c
}

func TestCreateCallProducesParams(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567890)
	start := time.Now()
	offer, err := c.CreateCall(chatID)
	if err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	elapsed := time.Since(start)
	t.Logf("CreateCall took %s, params JSON is %d bytes", elapsed, len(offer))
	// The "offer" is ntgcalls' JSON params object, not SDP text:
	// {"ufrag", "pwd", "fingerprints": [...], "ssrc", ...}. The
	// core passes it verbatim to joinVideoChat as params.
	var params struct {
		Ufrag        string `json:"ufrag"`
		Pwd          string `json:"pwd"`
		Fingerprints []struct {
			Hash        string `json:"hash"`
			Setup       string `json:"setup"`
			Fingerprint string `json:"fingerprint"`
		} `json:"fingerprints"`
		SSRC int64 `json:"ssrc"`
	}
	if err := json.Unmarshal([]byte(offer), &params); err != nil {
		t.Fatalf("CreateCall did not return JSON params: %v (%.200q)", err, offer)
	}
	if params.Ufrag == "" || params.Pwd == "" {
		t.Errorf("params JSON has empty ufrag/pwd: %.200q", offer)
	}
	if len(params.Fingerprints) == 0 || params.Fingerprints[0].Hash != "sha-256" {
		t.Errorf("params JSON has no sha-256 fingerprint: %.300q", offer)
	}
	if params.SSRC == 0 {
		t.Errorf("params JSON has zero ssrc: %.200q", offer)
	}
	if err := c.Stop(chatID); err != nil {
		t.Fatalf("Stop after CreateCall failed: %v", err)
	}
}

func TestConnectRejectsBadPayload(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567891)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	err := c.Connect(context.Background(), chatID, "this is not an SDP offer")
	if err == nil {
		t.Fatal("Connect accepted a garbage payload; expected an error")
	}
	// The exact error is the contract: a failed join must surface
	// the stage and the library's reason, never a fake success.
	t.Logf("Connect(garbage) exact error: %v", err)
}

func TestSendExternalFrameRequiresSource(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567892)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	frame := make([]byte, FrameBytes)
	err := c.SendExternalFrame(chatID, frame)
	if err == nil {
		t.Fatal("SendExternalFrame succeeded with no external source bound; expected \"External source not initialized\"")
	}
	t.Logf("SendExternalFrame without source exact error: %v", err)
	if !strings.Contains(err.Error(), "External source not initialized") {
		t.Errorf("unexpected error: %v", err)
	}
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	if err := c.SendExternalFrame(chatID, frame); err != nil {
		t.Fatalf("SendExternalFrame after binding the external source failed: %v", err)
	}
}

func TestPauseMuteUnmuteStopOnCreatedCall(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567893)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	// ntgcalls returns the "changed" flag, not the resulting state,
	// so these tests assert no error and log the flag; the Manager
	// owns the Paused/Muted record.
	changed, err := c.Pause(chatID)
	if err != nil {
		t.Fatalf("Pause on a created call failed: %v", err)
	}
	t.Logf("Pause changed=%v", changed)
	changed, err = c.Resume(chatID)
	if err != nil {
		t.Fatalf("Resume on a created call failed: %v", err)
	}
	t.Logf("Resume changed=%v", changed)
	changed, err = c.Mute(chatID)
	if err != nil {
		t.Fatalf("Mute on a created call failed: %v", err)
	}
	t.Logf("Mute changed=%v", changed)
	changed, err = c.Unmute(chatID)
	if err != nil {
		t.Fatalf("Unmute on a created call failed: %v", err)
	}
	t.Logf("Unmute changed=%v", changed)
	state, err := c.GetState(chatID)
	if err != nil {
		t.Fatalf("GetState failed: %v", err)
	}
	t.Logf("GetState after unmute: %+v", state)
	if state.Muted {
		t.Error("call is still muted after Unmute")
	}
	if err := c.Stop(chatID); err != nil {
		t.Fatalf("Stop on a created call failed: %v", err)
	}
}

func TestManagerJoinWithoutJoinFunc(t *testing.T) {
	c := newTestClient(t)
	m := NewManager(c, nil)
	t.Cleanup(m.Close)
	_, err := m.Join(context.Background(), -1001234567894)
	if err == nil {
		t.Fatal("Join with no JoinFunc succeeded; expected a failure naming the join stage")
	}
	t.Logf("Join without JoinFunc exact error: %v", err)
	if !strings.Contains(err.Error(), "join:") {
		t.Errorf("error does not name the join stage: %v", err)
	}
	call, ok := m.Get(-1001234567894)
	if !ok {
		t.Fatal("failed join left no call record")
	}
	if call.State != StateFailed || call.FailedStage != "join" {
		t.Errorf("failed join record: state=%s stage=%q (want failed/join)", call.State, call.FailedStage)
	}
}

func TestManagerJoinStageErrors(t *testing.T) {
	c := newTestClient(t)

	// Stage "join": the JoinFunc itself fails.
	joinErr := errors.New("joinVideoChat: GROUPCALL_FORBIDDEN (test double)")
	m := NewManager(c, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		var params struct {
			Ufrag string `json:"ufrag"`
			Pwd   string `json:"pwd"`
		}
		if err := json.Unmarshal([]byte(localSDP), &params); err != nil {
			t.Errorf("JoinFunc received local params that are not JSON: %v (%.80q)", err, localSDP)
		}
		if params.Ufrag == "" || params.Pwd == "" {
			t.Errorf("JoinFunc received local params with empty ufrag/pwd: %.120q", localSDP)
		}
		return "", 0, joinErr
	})
	t.Cleanup(m.Close)
	_, err := m.Join(context.Background(), -1001234567895)
	if err == nil {
		t.Fatal("Join succeeded although the JoinFunc failed")
	}
	t.Logf("Join with failing JoinFunc exact error: %v", err)
	if !strings.Contains(err.Error(), "join:") || !strings.Contains(err.Error(), "GROUPCALL_FORBIDDEN") {
		t.Errorf("error does not carry the join stage and the exact reason: %v", err)
	}
	call, _ := m.Get(-1001234567895)
	if call.FailedStage != "join" {
		t.Errorf("FailedStage=%q, want %q", call.FailedStage, "join")
	}

	// Stage "connect": the JoinFunc succeeds but hands back a remote
	// SDP ntgcalls cannot parse.
	m2 := NewManager(c, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return "not a remote SDP", 12345, nil
	})
	t.Cleanup(m2.Close)
	_, err = m2.Join(context.Background(), -1001234567896)
	if err == nil {
		t.Fatal("Join succeeded with a garbage remote SDP")
	}
	t.Logf("Join with garbage remote SDP exact error: %v", err)
	if !strings.Contains(err.Error(), "connect:") {
		t.Errorf("error does not name the connect stage: %v", err)
	}
	call, _ = m2.Get(-1001234567896)
	if call.FailedStage != "connect" {
		t.Errorf("FailedStage=%q, want %q", call.FailedStage, "connect")
	}
}

func TestManagerPreJoinHook(t *testing.T) {
	c := newTestClient(t)
	restore := saveOpusConfig(t)
	defer restore()

	// A hook error fails the join at stage "opus_config"
	// before CreateCall runs: the JoinFunc is never invoked.
	joinCalled := false
	m := NewManager(c, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		joinCalled = true
		return "{}", 12345, nil
	})
	t.Cleanup(m.Close)
	m.SetPreJoinHook(func(ctx context.Context, chatID int64) error {
		return errors.New("opus config refused (test double)")
	})
	_, err := m.Join(context.Background(), -1001234567897)
	if err == nil {
		t.Fatal("Join succeeded although the pre-join hook failed")
	}
	if !strings.Contains(err.Error(), "opus_config:") {
		t.Errorf("error does not name the opus_config stage: %v", err)
	}
	if joinCalled {
		t.Error("JoinFunc ran although the pre-join hook failed first")
	}
	call, _ := m.Get(-1001234567897)
	if call.FailedStage != "opus_config" {
		t.Errorf("FailedStage=%q, want %q", call.FailedStage, "opus_config")
	}

	// A hook that applies a config runs before CreateCall:
	// the library holds the hook's config even though the
	// join then fails at the join stage (no JoinFunc here,
	// so CreateCall succeeds and the join stage fails).
	m2 := NewManager(c, nil)
	t.Cleanup(m2.Close)
	m2.SetPreJoinHook(func(ctx context.Context, chatID int64) error {
		return SetOpusConfig(OpusEncoderConfig{BitrateBps: 160000, Complexity: 10, VBREnabled: true, FECEnabled: false, DTXEnabled: false, FrameSizeMs: 20, MaxPlaybackRateHz: 48000})
	})
	_, err = m2.Join(context.Background(), -1001234567898)
	if err == nil {
		t.Fatal("Join without JoinFunc succeeded; expected a failure naming the join stage")
	}
	if !strings.Contains(err.Error(), "join:") {
		t.Errorf("error does not name the join stage: %v", err)
	}
	got, err := OpusConfig()
	if err != nil {
		t.Fatalf("OpusConfig: %v", err)
	}
	if got.BitrateBps != 160000 || got.FrameSizeMs != 20 {
		t.Errorf("pre-join hook config not applied: %+v", got)
	}

	// Clearing the hook restores the old behaviour (no hook
	// runs, join fails at the join stage as before).
	m2.SetPreJoinHook(nil)
	_, err = m2.Join(context.Background(), -1001234567899)
	if err == nil || !strings.Contains(err.Error(), "join:") {
		t.Errorf("join after clearing the hook: err=%v, want a join-stage failure", err)
	}
}

func TestManagerPauseWithoutCall(t *testing.T) {
	c := newTestClient(t)
	m := NewManager(c, nil)
	t.Cleanup(m.Close)
	if _, err := m.Pause(-1001234567897); err == nil {
		t.Error("Pause with no call succeeded; expected \"no active call\"")
	} else {
		t.Logf("Pause without call exact error: %v", err)
	}
	if err := m.Stop(-1001234567897); err == nil {
		t.Error("Stop with no call succeeded; expected \"no active call\"")
	}
}

// makeTone renders a short local PCM WAV with ffmpeg so the feed test
// exercises a real decode without any network or credentials.
func makeTone(t *testing.T, seconds int) string {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not in PATH")
	}
	path := filepath.Join(t.TempDir(), fmt.Sprintf("tone-%ds.wav", seconds))
	cmd := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", fmt.Sprintf("sine=frequency=440:duration=%d", seconds),
		"-ar", "48000", "-ac", "2", "-c:a", "pcm_s16le", path)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ffmpeg tone render failed: %v: %s", err, out)
	}
	return path
}

// openFakeStream opens a fake DSP stream for the WAV file at
// path with opts. The fake is the test double for parlay-dsp
// (internal/dsp/fake): a pure-Go WAV decoder with a real (crude)
// effect chain. It returns the concrete *fake.Stream so tests can
// assert the parameters the DSP confirmed (LastParams) as well as
// the dsp.Stream the feed consumes.
func openFakeStream(t *testing.T, path string, opts dsp.Options) (*fake.Stream, dsp.Stream) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read tone: %v", err)
	}
	stream, err := fake.Engine{}.OpenStream(context.Background(), io.NopCloser(bytes.NewReader(data)), opts)
	if err != nil {
		t.Fatalf("fake OpenStream: %v", err)
	}
	fs, ok := stream.(*fake.Stream)
	if !ok {
		t.Fatalf("fake OpenStream returned %T, want *fake.Stream", stream)
	}
	return fs, stream
}

func TestFeedEndToEnd(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567898)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	tone := makeTone(t, 2)
	_, stream := openFakeStream(t, tone, dsp.DefaultOptions())
	ended := make(chan error, 1)
	feed, err := startDSPFeed(context.Background(), c, chatID, stream, func(feedErr error) {
		ended <- feedErr
	}, nil)
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	select {
	case feedErr := <-ended:
		if feedErr != nil {
			t.Fatalf("feed ended with an error: %v", feedErr)
		}
	case <-time.After(15 * time.Second):
		t.Fatalf("feed did not reach end of stream within 15s (frames sent: %d)", feed.FramesSent())
	}
	sent := feed.FramesSent()
	// 2 s of audio is 200 ten-millisecond frames; allow a small
	// shortfall for the trailing partial frame and scheduling.
	t.Logf("feed sent %d frames for a 2s tone (expect ~200)", sent)
	if sent < 180 || sent > 220 {
		t.Errorf("frames sent = %d, want ~200 for 2s of audio", sent)
	}
}

// TestFeedFromPrefetchReaderEndToEnd proves the Phase 4 → Phase 5
// seam: a real PrefbufferedStream (Prefetcher against a local HTTP
// server serving the tone) hands its Reader to the DSP feed, which
// decodes it and pushes the same 10 ms frames to ntgcalls, ending
// cleanly at EOF. The reader yields the buffered prefix first,
// then the live body — exactly what the prefetch hands over.
func TestFeedFromPrefetchReaderEndToEnd(t *testing.T) {
	tone := makeTone(t, 2)
	data, err := os.ReadFile(tone)
	if err != nil {
		t.Fatalf("read tone: %v", err)
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "audio/wav")
		_, _ = w.Write(data)
	}))
	t.Cleanup(srv.Close)

	track := model.Track{ID: "yt:prefetch-tone", Title: "Tone", Source: "youtube", StreamURL: srv.URL}
	p := prefetch.New(func(ctx context.Context, tr model.Track) (model.Track, error) {
		return track, nil
	}, prefetch.PrefixBytesForLeadTime(2), 10*time.Second)
	p.Prefetch(-1001234567897, track)
	pending, ok := p.Peek(-1001234567897, track.ID)
	if !ok {
		t.Fatal("prefetch not pending")
	}
	select {
	case <-pending.Ready():
	case <-time.After(10 * time.Second):
		t.Fatal("prefetch never became ready")
	}
	stream, ok := p.Take(-1001234567897, track.ID)
	if !ok {
		t.Fatalf("prefetch Take failed: %v", pending.Err())
	}

	c := newTestClient(t)
	chatID := int64(-1001234567897)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	dspStream2, err := fake.Engine{}.OpenStream(context.Background(), stream.Reader(), dsp.DefaultOptions())
	if err != nil {
		t.Fatalf("fake OpenStream over prefetch reader: %v", err)
	}
	ended := make(chan error, 1)
	feed, err := startDSPFeed(context.Background(), c, chatID, dspStream2, func(feedErr error) {
		ended <- feedErr
	}, nil)
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	select {
	case feedErr := <-ended:
		if feedErr != nil {
			t.Fatalf("feed ended with an error: %v", feedErr)
		}
	case <-time.After(15 * time.Second):
		t.Fatalf("feed did not reach end of stream within 15s (frames sent: %d)", feed.FramesSent())
	}
	sent := feed.FramesSent()
	t.Logf("prefetch-reader feed sent %d frames for a 2s tone (expect ~200)", sent)
	if sent < 180 || sent > 220 {
		t.Errorf("frames sent = %d, want ~200 for 2s of audio", sent)
	}
}

func TestFeedRejectsBadSource(t *testing.T) {
	// A non-WAV input fails at OpenStream: the DSP reports the
	// exact decode failure instead of feeding garbage to WebRTC.
	_, err := fake.Engine{}.OpenStream(context.Background(), io.NopCloser(strings.NewReader("this is not audio")), dsp.DefaultOptions())
	if err == nil {
		t.Fatal("fake OpenStream accepted garbage input")
	} else {
		t.Logf("fake OpenStream exact error: %v", err)
	}

	// A missing local file fails synchronously at open time.
	c := newTestClient(t)
	m := NewManager(c, nil)
	t.Cleanup(m.Close)
	if err := m.Play(context.Background(), -1001234567899, "/nonexistent/no-such-file.flac", dsp.DefaultOptions()); err == nil {
		t.Fatal("Play on a missing file succeeded")
	} else {
		t.Logf("Play on a missing file exact error: %v", err)
	}

	// A nil reader is rejected before any DSP work.
	if err := m.PlayStream(context.Background(), -1001234567899, nil, dsp.DefaultOptions()); err == nil {
		t.Fatal("PlayStream with nil reader succeeded")
	}
}

func TestFeedPauseResume(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567900)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	tone := makeTone(t, 10)
	_, stream := openFakeStream(t, tone, dsp.DefaultOptions())
	feed, err := startDSPFeed(context.Background(), c, chatID, stream, nil, nil)
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	// Let a few frames flow, then pause: the frame count must stop
	// advancing while paused and resume afterwards.
	deadline := time.Now().Add(10 * time.Second)
	for feed.FramesSent() < 20 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if feed.FramesSent() < 20 {
		t.Fatalf("feed sent only %d frames before pause", feed.FramesSent())
	}
	feed.Pause()
	pausedAt := feed.FramesSent()
	time.Sleep(600 * time.Millisecond)
	pausedLater := feed.FramesSent()
	t.Logf("frames while paused: %d -> %d (600 ms)", pausedAt, pausedLater)
	if pausedLater-pausedAt > 1 {
		t.Errorf("feed kept sending while paused: %d frames in 600 ms", pausedLater-pausedAt)
	}
	feed.Resume()
	deadline = time.Now().Add(10 * time.Second)
	for feed.FramesSent() < pausedLater+20 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if feed.FramesSent() < pausedLater+20 {
		t.Errorf("feed did not resume sending after Resume (sent %d, want >= %d)", feed.FramesSent(), pausedLater+20)
	}
}

// TestFeedSpeedReachesDSP proves vc.play's speed reaches the DSP:
// a 2 s tone at speed 2.0 yields ~100 frames (not ~200), and the
// stream confirms the speed it was given.
func TestFeedSpeedReachesDSP(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567901)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	tone := makeTone(t, 2)
	opts := dsp.Options{Speed: 2.0, Volume: 100}
	fakeStream, stream := openFakeStream(t, tone, opts)
	ended := make(chan error, 1)
	feed, err := startDSPFeed(context.Background(), c, chatID, stream, func(feedErr error) {
		ended <- feedErr
	}, nil)
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	select {
	case feedErr := <-ended:
		if feedErr != nil {
			t.Fatalf("feed ended with an error: %v", feedErr)
		}
	case <-time.After(15 * time.Second):
		t.Fatalf("feed did not reach end of stream within 15s (frames sent: %d)", feed.FramesSent())
	}
	sent := feed.FramesSent()
	t.Logf("feed sent %d frames for a 2s tone at speed 2.0 (expect ~100)", sent)
	if sent < 90 || sent > 110 {
		t.Errorf("frames sent = %d, want ~100 for 2s of audio at speed 2.0", sent)
	}
	if got := fakeStream.LastParams().Speed; got != 2.0 {
		t.Errorf("DSP confirmed speed = %v, want 2.0", got)
	}
}

// TestFeedTransitionGapless proves the DSP dual-slot path: track B
// is loaded as the next slot while track A plays; the feed
// promotes it without restarting, fires onTransition exactly once
// with B's id, and ends cleanly only after B finishes. Total
// frames are ~400 (2 s + 2 s) with no gap frame dropped at the
// boundary.
func TestFeedTransitionGapless(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567902)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	toneA := makeTone(t, 2)
	toneB := makeTone(t, 2)
	_, streamA := openFakeStream(t, toneA, dsp.DefaultOptions())
	dataB, err := os.ReadFile(toneB)
	if err != nil {
		t.Fatalf("read tone B: %v", err)
	}
	ended := make(chan error, 1)
	transitions := make(chan string, 2)
	feed, err := startDSPFeed(context.Background(), c, chatID, streamA, func(feedErr error) {
		ended <- feedErr
	}, func(trackID string) {
		transitions <- trackID
	})
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	// Load the next slot while A is still playing.
	if err := feed.LoadNext(io.NopCloser(bytes.NewReader(dataB)), dsp.DefaultOptions(), "track-b"); err != nil {
		t.Fatalf("LoadNext failed: %v", err)
	}
	// A second LoadNext before the transition must fail: at most
	// one next slot may be pending.
	if err := feed.LoadNext(io.NopCloser(bytes.NewReader(dataB)), dsp.DefaultOptions(), "track-b-again"); err == nil {
		t.Fatal("second LoadNext before the transition succeeded")
	}
	select {
	case got := <-transitions:
		if got != "track-b" {
			t.Fatalf("transition fired with track %q, want track-b", got)
		}
	case <-time.After(15 * time.Second):
		t.Fatalf("transition never fired (frames sent: %d)", feed.FramesSent())
	}
	select {
	case <-transitions:
		t.Fatal("transition fired more than once")
	case feedErr := <-ended:
		if feedErr != nil {
			t.Fatalf("feed ended with an error: %v", feedErr)
		}
	case <-time.After(15 * time.Second):
		t.Fatalf("feed did not end after the transition (frames sent: %d)", feed.FramesSent())
	}
	sent := feed.FramesSent()
	t.Logf("gapless feed sent %d frames for 2s + 2s (expect ~400)", sent)
	if sent < 380 || sent > 420 {
		t.Errorf("frames sent = %d, want ~400 for 2s + 2s gapless", sent)
	}
}

// TestFeedApplyOptions proves fx.apply's live push: ApplyOptions
// on a running feed returns nil (the DSP's confirmation) and the
// stream records the new parameters from the next frame on.
func TestFeedApplyOptions(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567903)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	tone := makeTone(t, 10)
	fakeStream, stream := openFakeStream(t, tone, dsp.DefaultOptions())
	feed, err := startDSPFeed(context.Background(), c, chatID, stream, nil, nil)
	if err != nil {
		t.Fatalf("startDSPFeed failed: %v", err)
	}
	t.Cleanup(feed.Stop)
	deadline := time.Now().Add(10 * time.Second)
	for feed.FramesSent() < 20 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if feed.FramesSent() < 20 {
		t.Fatalf("feed sent only %d frames before ApplyOptions", feed.FramesSent())
	}
	opts := dsp.Options{Speed: 1.0, Bass: 80, Volume: 100, Effect: "bass-boost"}
	if err := feed.ApplyOptions(opts); err != nil {
		t.Fatalf("ApplyOptions failed: %v", err)
	}
	got := fakeStream.LastParams()
	if got.Effect != "bass-boost" || got.Bass != 80 {
		t.Errorf("DSP confirmed params = %+v, want effect bass-boost bass 80", got)
	}
	if err := feed.ApplyOptions(dsp.Options{Speed: 99}); err == nil {
		t.Error("ApplyOptions accepted an out-of-range speed")
	}
}

func TestManagerPlayRequiresConnectedCall(t *testing.T) {
	c := newTestClient(t)
	m := NewManager(c, nil)
	t.Cleanup(m.Close)
	// An existing file reaches the call-state gate (a missing
	// file fails earlier, at open time — see
	// TestFeedRejectsBadSource).
	tone := makeTone(t, 1)
	if err := m.Play(context.Background(), -1001234567901, tone, dsp.DefaultOptions()); err == nil {
		t.Fatal("Play with no connected call succeeded")
	} else {
		t.Logf("Play without a call exact error: %v", err)
	}
	if err := m.PlayStream(context.Background(), -1001234567901, io.NopCloser(strings.NewReader("x")), dsp.DefaultOptions()); err == nil {
		t.Fatal("PlayStream with no connected call succeeded")
	}
	if err := m.ApplyFX(-1001234567901, dsp.DefaultOptions()); !errors.Is(err, ErrNoFeed) {
		t.Errorf("ApplyFX with no feed: err = %v, want ErrNoFeed", err)
	}
	if err := m.LoadNext(-1001234567901, io.NopCloser(strings.NewReader("x")), dsp.DefaultOptions(), "track-x"); !errors.Is(err, ErrNoFeed) {
		t.Errorf("LoadNext with no feed: err = %v, want ErrNoFeed", err)
	}
}
