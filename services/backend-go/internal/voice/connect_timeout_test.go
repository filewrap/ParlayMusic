package voice

import (
	"context"
	"errors"
	"runtime"
	"strings"
	"testing"
	"time"
)

// fakeRemoteSDP is the dispatch probe's fake remote SDP
// (tests/interop/core_peer.cpp): SDP text with no media lines
// and no real group call. The library rejects it immediately
// (ntgcalls error -102: Invalid JSON); it is the "fake SDP" the
// Phase 3/5 dispatch fed Connect.
const fakeRemoteSDP = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=core\r\n"

// TestConnectAgainstFakeSDPReturnsWithinTimeout measures the
// dispatch case for real: Connect against the fake SDP must
// return well within the connect timeout. The library rejects
// this input fast, so the measured time is ~1 ms; the timeout
// path itself (a library call that actually blocks) is proven
// by TestConnectTimeoutWithBlockingCall, because no input found
// makes ntg_connect block — it returns fast for the fake SDP
// (-102) and for a valid but unreachable transport (rc 0, the
// handshake runs asynchronously and fails via the
// connection-change callback).
func TestConnectAgainstFakeSDPReturnsWithinTimeout(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567910)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })
	start := time.Now()
	err := c.Connect(context.Background(), chatID, fakeRemoteSDP)
	elapsed := time.Since(start)
	if err == nil {
		t.Fatal("Connect accepted the fake SDP; expected an error")
	}
	t.Logf("Connect(fake SDP) returned in %s with error: %v (timeout %s)", elapsed, err, c.connectTimeout)
	if elapsed > c.connectTimeout {
		t.Errorf("Connect took %s, past the %s timeout", elapsed, c.connectTimeout)
	}
}

// TestConnectContextCancellation proves the context binds
// Connect: an already-cancelled context returns at once without
// running the call, and a context whose deadline fires while the
// call is running returns by that deadline.
func TestConnectContextCancellation(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567911)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}
	t.Cleanup(func() { _ = c.Stop(chatID) })

	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	start := time.Now()
	err := c.Connect(cancelled, chatID, fakeRemoteSDP)
	elapsed := time.Since(start)
	if !errors.Is(err, context.Canceled) {
		t.Errorf("Connect(cancelled ctx) err = %v, want context.Canceled", err)
	}
	t.Logf("Connect(cancelled ctx) returned in %s", elapsed)
	if elapsed > 5*time.Second {
		t.Errorf("Connect(cancelled ctx) took %s, want an immediate return", elapsed)
	}

	// A 1 ms deadline against a blocking stand-in: the wait is
	// the context, not the call.
	unblock := make(chan struct{})
	defer close(unblock)
	c.connectFn = func(chatID int64, remoteSDP string) error {
		<-unblock
		return nil
	}
	deadlineCtx, cancel2 := context.WithTimeout(context.Background(), time.Millisecond)
	defer cancel2()
	start = time.Now()
	err = c.Connect(deadlineCtx, chatID, fakeRemoteSDP)
	elapsed = time.Since(start)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("Connect(1ms ctx) err = %v, want context.DeadlineExceeded", err)
	}
	t.Logf("Connect(1ms ctx, blocking call) returned in %s", elapsed)
	if elapsed > 5*time.Second {
		t.Errorf("Connect(1ms ctx) took %s, want a return at the deadline", elapsed)
	}
}

// TestConnectTimeoutWithBlockingCall proves the timeout path
// with a stand-in for a library call that never returns (a
// Connect wedged inside ntg_connect): Connect returns at the
// configured timeout, the detached worker is exactly one extra
// goroutine (bounded, not growing), and it is reaped — the
// goroutine count returns to baseline — once the underlying call
// returns (here: when the test releases it, the way Stop tears
// the half-connected call down in production). A cgo call that
// never returns cannot be killed from Go; the wrapper bounds
// the wait and detaches cleanly, it does not pretend to kill
// the call.
func TestConnectTimeoutWithBlockingCall(t *testing.T) {
	const timeout = 200 * time.Millisecond
	c, err := NewWithConnectTimeout(timeout)
	if err != nil {
		t.Fatalf("NewWithConnectTimeout failed: %v", err)
	}
	chatID := int64(-1001234567912)
	if _, err := c.CreateCall(chatID); err != nil {
		t.Fatalf("CreateCall failed: %v", err)
	}

	unblock := make(chan struct{})
	c.connectFn = func(chatID int64, remoteSDP string) error {
		<-unblock
		return nil
	}
	// Let the runtime settle so the baseline count is stable.
	time.Sleep(100 * time.Millisecond)
	before := runtime.NumGoroutine()

	start := time.Now()
	err = c.Connect(context.Background(), chatID, fakeRemoteSDP)
	elapsed := time.Since(start)
	if err == nil || !strings.Contains(err.Error(), "timed out") {
		t.Fatalf("Connect(blocking call) err = %v, want a timeout error", err)
	}
	t.Logf("Connect(blocking call) returned in %s with error: %v", elapsed, err)
	if elapsed < timeout || elapsed > timeout+5*time.Second {
		t.Errorf("Connect returned in %s, want ~%s (the timeout)", elapsed, timeout)
	}

	// The worker is still inside the (stand-in) call: exactly
	// one extra goroutine, and it holds no exclusive lock.
	// Poll briefly: the worker is blocked on the channel, so the
	// count must settle at baseline+1, not drift.
	during := -1
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if runtime.NumGoroutine() == before+1 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	during = runtime.NumGoroutine()
	t.Logf("goroutines: before=%d during stuck call=%d", before, during)
	if during != before+1 {
		t.Errorf("goroutines during stuck call = %d, want %d (exactly one detached worker)", during, before+1)
	}
	if _, err := c.CreateCall(chatID + 1); err != nil {
		t.Errorf("CreateCall while a Connect is stuck failed: %v", err)
	} else {
		t.Logf("CreateCall on another chat proceeded while Connect was stuck")
		_ = c.Stop(chatID + 1)
	}

	// Release the underlying call: the worker must exit and the
	// count must return to baseline — no leak.
	close(unblock)
	deadline = time.Now().Add(5 * time.Second)
	for runtime.NumGoroutine() > before && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	after := runtime.NumGoroutine()
	t.Logf("goroutines: after release=%d (baseline %d)", after, before)
	if after != before {
		t.Errorf("goroutines after release = %d, want baseline %d (worker leaked)", after, before)
	}
	// Close waits for the worker (write lock), so it must
	// succeed now that the call has returned.
	c.Close()
}

// TestOtherOpsProceedDuringStuckConnect proves a stuck Connect
// does not queue other voice operations behind it: while a
// Connect is wedged (stand-in), CreateCall, GetState and Stop
// on other chats answer at once instead of waiting out the
// connect timeout.
func TestOtherOpsProceedDuringStuckConnect(t *testing.T) {
	c, err := NewWithConnectTimeout(30 * time.Second)
	if err != nil {
		t.Fatalf("NewWithConnectTimeout failed: %v", err)
	}
	chatStuck := int64(-1001234567913)
	chatOther := int64(-1001234567914)
	if _, err := c.CreateCall(chatStuck); err != nil {
		t.Fatalf("CreateCall(stuck) failed: %v", err)
	}
	if _, err := c.CreateCall(chatOther); err != nil {
		t.Fatalf("CreateCall(other) failed: %v", err)
	}

	unblock := make(chan struct{})
	c.connectFn = func(chatID int64, remoteSDP string) error {
		<-unblock
		return nil
	}
	done := make(chan error, 1)
	go func() {
		done <- c.Connect(context.Background(), chatStuck, fakeRemoteSDP)
	}()
	// Give the worker time to wedge inside the stand-in call.
	time.Sleep(200 * time.Millisecond)
	select {
	case err := <-done:
		t.Fatalf("Connect returned early with %v; expected it to stay stuck", err)
	default:
	}

	// Every other operation must answer at once, not after the
	// 30 s connect timeout.
	start := time.Now()
	if _, err := c.GetState(chatOther); err != nil {
		t.Errorf("GetState(other chat) while Connect stuck: %v", err)
	}
	t.Logf("GetState(other chat) while Connect stuck took %s", time.Since(start))
	if took := time.Since(start); took > 5*time.Second {
		t.Errorf("GetState took %s behind a stuck Connect", took)
	}
	start = time.Now()
	if err := c.Stop(chatOther); err != nil {
		t.Errorf("Stop(other chat) while Connect stuck: %v", err)
	}
	t.Logf("Stop(other chat) while Connect stuck took %s", time.Since(start))
	if took := time.Since(start); took > 5*time.Second {
		t.Errorf("Stop took %s behind a stuck Connect", took)
	}
	// Tear the stuck call down: the worker is released and
	// Close can destroy the instance.
	close(unblock)
	select {
	case err := <-done:
		t.Logf("stuck Connect returned after release: %v", err)
	case <-time.After(5 * time.Second):
		t.Fatal("stuck Connect did not return after release")
	}
	c.Close()
}

// TestManagerJoinTimeoutFreesNextAction proves the join handler
// itself is cancelled — not just the UDS deadline: a JoinFunc
// that never answers (a TDLib joinVideoChat that hangs) makes
// Join return at the context deadline with the join stage named,
// the call record lands in StateFailed, and the next voice-state
// action on the same chat — a retry Join, Get, Stop — answers at
// once instead of starving behind the dead join (Phase 5
// vc.lyrics flake shape at the Manager level).
func TestManagerJoinTimeoutFreesNextAction(t *testing.T) {
	c := newTestClient(t)
	chatID := int64(-1001234567915)
	m := NewManager(c, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		<-ctx.Done()
		return "", 0, ctx.Err()
	})
	t.Cleanup(m.Close)

	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	start := time.Now()
	_, err := m.Join(ctx, chatID)
	elapsed := time.Since(start)
	if err == nil {
		t.Fatal("Join with a hanging JoinFunc succeeded")
	}
	t.Logf("Join(hanging JoinFunc) returned in %s with error: %v", elapsed, err)
	if !strings.Contains(err.Error(), "join:") {
		t.Errorf("error does not name the join stage: %v", err)
	}
	if elapsed < 150*time.Millisecond || elapsed > 5*time.Second {
		t.Errorf("Join returned in %s, want ~200ms (the context deadline)", elapsed)
	}
	call, ok := m.Get(chatID)
	if !ok {
		t.Fatal("timed-out join left no call record")
	}
	if call.State != StateFailed || call.FailedStage != "join" {
		t.Errorf("timed-out join record: state=%s stage=%q (want failed/join)", call.State, call.FailedStage)
	}

	// The next voice-state action on the same chat must answer
	// at once: a retry Join (with a fast-failing JoinFunc) and
	// Stop.
	m2 := NewManager(c, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return "", 0, errors.New("joinVideoChat: GROUPCALL_FORBIDDEN (test double)")
	})
	start = time.Now()
	_, err = m2.Join(context.Background(), chatID)
	retryElapsed := time.Since(start)
	if err == nil || !strings.Contains(err.Error(), "GROUPCALL_FORBIDDEN") {
		t.Errorf("retry Join err = %v, want the fast GROUPCALL_FORBIDDEN failure", err)
	}
	t.Logf("retry Join after the timed-out join returned in %s", retryElapsed)
	if retryElapsed > 5*time.Second {
		t.Errorf("retry Join took %s behind the dead join", retryElapsed)
	}
	// Stop after the timed-out join must answer at once — a
	// fast error (the call is already torn down) is an answer,
	// a hang is not.
	start = time.Now()
	stopErr := m.Stop(chatID)
	stopElapsed := time.Since(start)
	t.Logf("Stop after the timed-out join took %s (err=%v)", stopElapsed, stopErr)
	if stopElapsed > 5*time.Second {
		t.Errorf("Stop took %s behind the dead join", stopElapsed)
	}
}
