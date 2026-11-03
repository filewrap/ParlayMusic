package uds

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// testPeer is a minimal core-side peer for the tests: it dials the
// socket and speaks raw frames. The real-core interop proof (the
// C++ IpcClient) lives in tests/interop/core_peer.cpp and is run
// separately — see phase3/scaffold.md.
type testPeer struct {
	t *testing.T
	c net.Conn
}

func dialPeer(t *testing.T, sock string) *testPeer {
	t.Helper()
	c, err := net.DialTimeout("unix", sock, 5*time.Second)
	if err != nil {
		t.Fatalf("dial %s: %v", sock, err)
	}
	return &testPeer{t: t, c: c}
}

func (p *testPeer) send(payload []byte) {
	p.t.Helper()
	if err := WriteFrame(p.c, payload); err != nil {
		p.t.Fatalf("write frame: %v", err)
	}
}

func (p *testPeer) request(action string, payload map[string]any) map[string]any {
	p.t.Helper()
	raw, _ := json.Marshal(payload)
	frame, _ := json.Marshal(map[string]any{
		"type": "request", "request_id": 42, "action": action,
		"deadline_ms": 5000, "payload": json.RawMessage(raw),
	})
	p.send(frame)
	resp, err := ReadFrame(p.c)
	if err != nil {
		p.t.Fatalf("read response: %v", err)
	}
	var out map[string]any
	if err := json.Unmarshal(resp, &out); err != nil {
		p.t.Fatalf("bad response JSON: %v", err)
	}
	return out
}

func TestFrameCodecByteCompatible(t *testing.T) {
	// 4-byte little-endian length + payload, exactly as
	// parlay::uds::encodeFrame produces it.
	frame, err := EncodeFrame([]byte{1, 2, 3})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(frame, []byte{3, 0, 0, 0, 1, 2, 3}) {
		t.Fatalf("frame bytes wrong: %v", frame)
	}
	got, err := ReadFrame(bytes.NewReader(frame))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, []byte{1, 2, 3}) {
		t.Fatalf("round-trip wrong: %v", got)
	}
	// Invalid length prefixes are protocol errors.
	for _, bad := range [][]byte{
		{0, 0, 0, 0},             // zero length
		{0, 0, 0, 1},             // 16 MiB + 1 (LE 0x01000000)
		{0xFF, 0xFF, 0xFF, 0xFF}, // huge
	} {
		if _, err := ReadFrame(bytes.NewReader(bad)); err == nil {
			t.Errorf("length prefix %v must be rejected", bad)
		}
	}
	if _, err := EncodeFrame(nil); err == nil {
		t.Error("empty payload must not encode")
	}
	if _, err := EncodeFrame(make([]byte, MaxFrameBytes+1)); err == nil {
		t.Error("oversize payload must not encode")
	}
}

func startTestServer(t *testing.T) (*Server, string) {
	t.Helper()
	sock := filepath.Join(t.TempDir(), "parlay-backend.sock")
	s := NewServer(sock)
	if err := s.Listen(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s, sock
}

func TestPingAndUnknownActions(t *testing.T) {
	_, sock := startTestServer(t)
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	resp := peer.request("ping", map[string]any{})
	if resp["ok"] != true {
		t.Fatalf("ping must answer ok:true, got %v", resp)
	}
	data, _ := resp["data"].(map[string]any)
	if data["service"] != "backend-go" {
		t.Fatalf("ping data.service must be backend-go, got %v", data)
	}

	resp = peer.request("vc.bogus", map[string]any{"chat_id": -1001})
	if resp["ok"] != false {
		t.Fatalf("unknown action must answer ok:false, got %v", resp)
	}
	errBody, _ := resp["error"].(map[string]any)
	if errBody["code"] != "not_found" {
		t.Fatalf("unknown action code must be not_found, got %v", errBody)
	}

	// test.sleep_ms is stub-only: the real backend answers not_found.
	resp = peer.request("test.sleep_ms", map[string]any{"sleep_ms": 1})
	errBody, _ = resp["error"].(map[string]any)
	if resp["ok"] != false || errBody["code"] != "not_found" {
		t.Fatalf("test.sleep_ms must answer not_found, got %v", resp)
	}
}

func TestDeadlineAbandonsWork(t *testing.T) {
	s, sock := startTestServer(t)
	started := make(chan struct{})
	s.Register("test.slow", func(ctx context.Context, _ Request) (any, error) {
		close(started)
		<-ctx.Done()
		return map[string]any{"late": true}, nil
	})
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	frame, _ := json.Marshal(map[string]any{
		"type": "request", "request_id": 7, "action": "test.slow",
		"deadline_ms": 50, "payload": map[string]any{},
	})
	peer.send(frame)
	<-started
	// The handler is abandoned at the deadline and no response is
	// written: the core has already failed the call and ignores a
	// late response. The peer must see nothing within 2 s.
	peer.c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := ReadFrame(peer.c); err == nil {
		t.Fatal("expected no response after the deadline, got a frame")
	} else if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("expected a read timeout, got %v", err)
	}
}

func TestEventsReachCore(t *testing.T) {
	s, sock := startTestServer(t)
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	// Dialling is not registration: acceptLoop runs async, so wait
	// for the server to register the connection before emitting —
	// otherwise the first event is dropped and the test flakes.
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	if err := s.WaitForPeer(ctx); err != nil {
		t.Fatalf("WaitForPeer: %v", err)
	}

	s.Emit("TrackStarted", -1001, map[string]any{
		"track":   map[string]any{"id": "t1", "title": "Song", "source": "youtube"},
		"chat_id": -1001,
	})
	peer.c.SetReadDeadline(time.Now().Add(2 * time.Second))
	raw, err := ReadFrame(peer.c)
	if err != nil {
		t.Fatalf("read event: %v", err)
	}
	var env map[string]any
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatal(err)
	}
	if env["type"] != "event" || env["event"] != "TrackStarted" {
		t.Fatalf("wrong event frame: %v", env)
	}
	if env["chat_id"] != float64(-1001) {
		t.Fatalf("event chat_id wrong: %v", env["chat_id"])
	}
}

func TestWaitForPeer(t *testing.T) {
	// No peer yet: a short deadline must fail, honestly.
	s, _ := startTestServer(t)
	ctx, cancel := context.WithTimeout(t.Context(), 50*time.Millisecond)
	defer cancel()
	if err := s.WaitForPeer(ctx); err == nil {
		t.Fatal("WaitForPeer with no peer must fail")
	}
	// After a dial it succeeds.
	sock := s.SocketPath()
	peer := dialPeer(t, sock)
	defer peer.c.Close()
	ctx2, cancel2 := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel2()
	if err := s.WaitForPeer(ctx2); err != nil {
		t.Fatalf("WaitForPeer after dial: %v", err)
	}
	// A second call with a live peer returns at once.
	ctx3, cancel3 := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel3()
	if err := s.WaitForPeer(ctx3); err != nil {
		t.Fatalf("WaitForPeer with live peer: %v", err)
	}
}

func TestJoinRequestHandshake(t *testing.T) {
	s, sock := startTestServer(t)
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	// Prove the server has accepted and is serving this connection
	// before issuing the backend-initiated JoinRequest; otherwise
	// the request can race the accept loop and find no connection.
	if resp := peer.request("ping", map[string]any{}); resp["ok"] != true {
		t.Fatalf("ping before join handshake failed: %v", resp)
	}

	type result struct {
		sdp string
		gid int64
		err error
	}
	done := make(chan result, 1)
	go func() {
		sdp, gid, err := s.RequestJoin(context.Background(), -1001, "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=go\r\n")
		done <- result{sdp, gid, err}
	}()
	// The peer reads the JoinRequest and answers like the core.
	peer.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	raw, err := ReadFrame(peer.c)
	if err != nil {
		t.Fatalf("read join_request: %v", err)
	}
	var env envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatal(err)
	}
	if env.Type != "join_request" {
		t.Fatalf("expected join_request, got %q", env.Type)
	}
	if env.RequestID < JoinRequestIDBase {
		t.Fatalf("join request_id %d must be >= %d", env.RequestID, JoinRequestIDBase)
	}
	if env.LocalSDP == "" || env.ChatID == nil || *env.ChatID != -1001 {
		t.Fatalf("join_request fields wrong: %+v", env)
	}
	resp, _ := json.Marshal(map[string]any{
		"type": "response", "request_id": env.RequestID, "ok": true,
		"data": map[string]any{"remote_sdp": "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=core\r\n", "group_call_id": 777},
	})
	peer.send(resp)
	select {
	case r := <-done:
		if r.err != nil {
			t.Fatalf("RequestJoin: %v", r.err)
		}
		if r.gid != 777 || r.sdp == "" {
			t.Fatalf("join result wrong: %+v", r)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("RequestJoin never returned")
	}
}

func TestSocketPermissions(t *testing.T) {
	_, sock := startTestServer(t)
	fi, err := os.Stat(sock)
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode().Perm() != 0o600 {
		t.Errorf("socket must be 0600, got %o", fi.Mode().Perm())
	}
}

// TestJoinResponseDeliveredWhileHandlerWaits is the Phase 6
// regression test for the vc.join wedge (Phase 3/5 dispatch:
// vc.join abandoned at the full 60 s deadline): a handler that
// waits in RequestJoin must receive the core's join response
// WHILE it waits. The server's read loop must keep reading
// frames — including join responses — while a handler is running,
// or the handler and the read loop deadlock until the request
// deadline: the response sits unread in the socket while the
// handler waits for it.
func TestJoinResponseDeliveredWhileHandlerWaits(t *testing.T) {
	s, sock := startTestServer(t)
	s.Register("test.join", func(ctx context.Context, req Request) (any, error) {
		remoteSDP, gid, err := s.RequestJoin(ctx, -1001, "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=go\r\n")
		if err != nil {
			return nil, err
		}
		return map[string]any{"remote_sdp": remoteSDP, "group_call_id": gid}, nil
	})
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	// Send the request with a long deadline: if the deadlock is
	// present, the response only arrives at the deadline (30 s),
	// not after the peer answers the JoinRequest.
	frame, _ := json.Marshal(map[string]any{
		"type": "request", "request_id": 7, "action": "test.join",
		"deadline_ms": 30000, "payload": map[string]any{},
	})
	peer.send(frame)

	// The handler must reach RequestJoin and the server must emit
	// the join_request frame promptly.
	peer.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	raw, err := ReadFrame(peer.c)
	if err != nil {
		t.Fatalf("read join_request: %v", err)
	}
	var env envelope
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatal(err)
	}
	if env.Type != "join_request" {
		t.Fatalf("expected join_request, got %q", env.Type)
	}
	// Answer it immediately, like the core's join handler.
	resp, _ := json.Marshal(map[string]any{
		"type": "response", "request_id": env.RequestID, "ok": true,
		"data": map[string]any{"remote_sdp": "fake-remote", "group_call_id": 777},
	})
	start := time.Now()
	peer.send(resp)
	// The test.join response must follow within 5 s — long before
	// the 30 s request deadline. A read timeout here is the
	// deadlock: the read loop was stuck inside handleRequest
	// while the handler waited for this very frame.
	peer.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	raw, err = ReadFrame(peer.c)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("join response never arrived after answering the JoinRequest (%s): %v", elapsed, err)
	}
	t.Logf("join response arrived %s after the JoinRequest answer", elapsed)
	var out map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	if out["ok"] != true {
		t.Fatalf("join response must be ok:true, got %v", out)
	}
}

// TestSlowHandlerDoesNotStarveNextRequest is the Phase 5
// vc.lyrics flake in miniature: while one request's handler is
// wedged (waiting out its deadline), a second request on the
// same connection must still be answered within its own
// deadline. The read loop must dispatch requests concurrently;
// a slow handler must never stall the reading of the next frame.
func TestSlowHandlerDoesNotStarveNextRequest(t *testing.T) {
	s, sock := startTestServer(t)
	started := make(chan struct{})
	s.Register("test.slow", func(ctx context.Context, _ Request) (any, error) {
		close(started)
		<-ctx.Done()
		return map[string]any{"late": true}, nil
	})
	peer := dialPeer(t, sock)
	defer peer.c.Close()

	slow, _ := json.Marshal(map[string]any{
		"type": "request", "request_id": 7, "action": "test.slow",
		"deadline_ms": 30000, "payload": map[string]any{},
	})
	peer.send(slow)
	select {
	case <-started:
	case <-time.After(5 * time.Second):
		t.Fatal("slow handler never started")
	}
	// The slow handler is now wedged for 30 s. A ping sent now
	// must still be answered within its own 5 s deadline.
	start := time.Now()
	ping, _ := json.Marshal(map[string]any{
		"type": "request", "request_id": 8, "action": "ping",
		"deadline_ms": 5000, "payload": map[string]any{},
	})
	peer.send(ping)
	peer.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	raw, err := ReadFrame(peer.c)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("ping was starved behind the wedged handler (%s): %v", elapsed, err)
	}
	t.Logf("ping answered %s after the slow handler wedged", elapsed)
	var out map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	if out["ok"] != true {
		t.Fatalf("ping must answer ok:true, got %v", out)
	}
}
