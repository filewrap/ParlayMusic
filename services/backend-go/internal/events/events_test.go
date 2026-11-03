package events

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// readEvent reads one length-prefixed frame from the test client
// and decodes the event envelope.
func readEvent(t *testing.T, c net.Conn) map[string]any {
	t.Helper()
	if err := c.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline: %v", err)
	}
	var header [4]byte
	if _, err := c.Read(header[:]); err != nil {
		t.Fatalf("read frame header: %v", err)
	}
	length := binary.LittleEndian.Uint32(header[:])
	payload := make([]byte, length)
	for off := 0; off < int(length); {
		n, err := c.Read(payload[off:])
		if err != nil {
			t.Fatalf("read frame payload: %v", err)
		}
		off += n
	}
	var env map[string]any
	if err := json.Unmarshal(payload, &env); err != nil {
		t.Fatalf("decode event: %v", err)
	}
	return env
}

func newTestEmitter(t *testing.T) (*Emitter, net.Conn) {
	t.Helper()
	sock := filepath.Join(t.TempDir(), "parlay-backend.sock")
	srv := uds.NewServer(sock)
	if err := srv.Listen(); err != nil {
		t.Fatalf("Listen: %v", err)
	}
	t.Cleanup(func() { _ = srv.Close() })
	c, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	t.Cleanup(func() { _ = c.Close() })
	// The server accepts asynchronously; wait for registration
	// instead of sleeping (see uds.WaitForPeer).
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	if err := srv.WaitForPeer(ctx); err != nil {
		t.Fatalf("WaitForPeer: %v", err)
	}
	return New(srv), c
}

func TestEmitterFrames(t *testing.T) {
	em, c := newTestEmitter(t)
	track := model.Track{ID: "yt:a", Title: "A", Source: "youtube", DurationSec: 100}

	em.TrackStarted(-1001, track)
	env := readEvent(t, c)
	if env["type"] != "event" || env["event"] != "TrackStarted" || env["chat_id"] != float64(-1001) {
		t.Fatalf("TrackStarted frame: %v", env)
	}
	data := env["data"].(map[string]any)
	if data["track"].(map[string]any)["id"] != "yt:a" || data["chat_id"] != float64(-1001) {
		t.Fatalf("TrackStarted data: %v", data)
	}

	em.TrackAdvanced(-1001, track, "yt:prev")
	env = readEvent(t, c)
	if env["event"] != "TrackAdvanced" {
		t.Fatalf("TrackAdvanced frame: %v", env)
	}
	data = env["data"].(map[string]any)
	if data["previous_track_id"] != "yt:prev" {
		t.Fatalf("TrackAdvanced data: %v", data)
	}

	em.TrackEnded(-1001, track, "queue_empty")
	env = readEvent(t, c)
	if env["event"] != "TrackEnded" {
		t.Fatalf("TrackEnded frame: %v", env)
	}
	if env["data"].(map[string]any)["reason"] != "queue_empty" {
		t.Fatalf("TrackEnded data: %v", env["data"])
	}

	em.QueueEmpty(-1001)
	env = readEvent(t, c)
	if env["event"] != "QueueEmpty" {
		t.Fatalf("QueueEmpty frame: %v", env)
	}
	if env["data"].(map[string]any)["chat_id"] != float64(-1001) {
		t.Fatalf("QueueEmpty data: %v", env["data"])
	}

	em.VoiceError(-1001, "join_failed", "no core connected")
	env = readEvent(t, c)
	if env["event"] != "VoiceError" {
		t.Fatalf("VoiceError frame: %v", env)
	}
	data = env["data"].(map[string]any)
	if data["code"] != "join_failed" || data["message"] != "no core connected" {
		t.Fatalf("VoiceError data: %v", data)
	}
}

func TestEmitterWithoutCoreIsSilentNoOp(t *testing.T) {
	srv := uds.NewServer(filepath.Join(t.TempDir(), "parlay-backend.sock"))
	// Never Listen, never connect: every emit must be a silent
	// no-op, never a panic or a block.
	em := New(srv)
	track := model.Track{ID: "yt:a", Title: "A", Source: "youtube"}
	em.TrackStarted(1, track)
	em.TrackAdvanced(1, track, "yt:prev")
	em.TrackEnded(1, track, "queue_empty")
	em.QueueEmpty(1)
	em.VoiceError(1, "join_failed", "boom")
}
