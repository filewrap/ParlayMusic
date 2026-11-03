package voice

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
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

// requiredSymbols is every ntg_ symbol this package binds. The cgo
// link already fails if a referenced symbol is absent; this test
// re-verifies the whole set explicitly so a missing symbol is
// reported by name instead of as a linker error.
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
	err := c.Connect(chatID, "this is not an SDP offer")
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
	ended := make(chan error, 1)
	feed, err := startFFmpegFeed(context.Background(), c, chatID, tone, func(feedErr error) {
		ended <- feedErr
	})
	if err != nil {
		t.Fatalf("startFFmpegFeed failed: %v", err)
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

// TestFeedFromReaderEndToEnd proves the Phase 4 prefetch feed
// path: ffmpeg decodes from a stdin reader (the PrebufferedStream
// reader) instead of a URL/file, pushes the same 10 ms frames to
// ntgcalls, and ends cleanly at EOF. The reader here is a tone
// file's bytes — the same bytes the prefetch reader yields (buffered
// prefix first, then the live body).
func TestFeedFromReaderEndToEnd(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567897)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	tone := makeTone(t, 2)
	data, err := os.ReadFile(tone)
	if err != nil {
		t.Fatalf("read tone: %v", err)
	}
	ended := make(chan error, 1)
	feed, err := startFFmpegFeedFromReader(context.Background(), c, chatID, io.NopCloser(bytes.NewReader(data)), func(feedErr error) {
		ended <- feedErr
	})
	if err != nil {
		t.Fatalf("startFFmpegFeedFromReader failed: %v", err)
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
	t.Logf("stdin feed sent %d frames for a 2s tone (expect ~200)", sent)
	if sent < 180 || sent > 220 {
		t.Errorf("frames sent = %d, want ~200 for 2s of audio", sent)
	}
}

func TestFeedRejectsBadSource(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567899)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	if err := c.SetExternalAudioSource(chatID); err != nil {
		t.Fatalf("SetExternalAudioSource failed: %v", err)
	}
	ended := make(chan error, 1)
	feed, err := startFFmpegFeed(context.Background(), c, chatID, "/nonexistent/no-such-file.flac", func(feedErr error) {
		ended <- feedErr
	})
	if err != nil {
		// A synchronous start failure is also an honest failure.
		t.Logf("startFFmpegFeed on a missing file failed synchronously: %v", err)
		return
	}
	t.Cleanup(feed.Stop)
	select {
	case feedErr := <-ended:
		if feedErr == nil {
			t.Fatal("feed on a missing file ended cleanly; expected an error")
		}
		t.Logf("feed on a missing file exact error: %v", feedErr)
		if feed.FramesSent() != 0 {
			t.Errorf("feed sent %d frames from a missing file", feed.FramesSent())
		}
	case <-time.After(15 * time.Second):
		t.Fatal("feed on a missing file never terminated")
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
	feed, err := startFFmpegFeed(context.Background(), c, chatID, tone, nil)
	if err != nil {
		t.Fatalf("startFFmpegFeed failed: %v", err)
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

func TestManagerPlayRequiresConnectedCall(t *testing.T) {
	c := newTestClient(t)
	m := NewManager(c, nil)
	t.Cleanup(m.Close)
	err := m.Play(context.Background(), -1001234567901, "/tmp/does-not-matter.wav")
	if err == nil {
		t.Fatal("Play with no connected call succeeded")
	}
	t.Logf("Play without a call exact error: %v", err)
}
