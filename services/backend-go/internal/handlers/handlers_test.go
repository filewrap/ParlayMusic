// Handler tests (vc.* happy + error paths).
//
// What runs here, and what honestly cannot: every validation,
// error-mapping, and room-only path runs offline against a real
// store, a real room Manager, and a real ntgcalls Manager — but
// no test can join a voice call (Join needs the core's
// RequestJoin over UDS and a real Telegram group call), so every
// path that ensures a call fails with the test joinFunc's canned
// error, and every path that binds audio with no call record
// skips the bind. The join-success, feed-bind, and live-advance
// paths are therefore NOT covered here; they are flagged as
// unmeasured in phase3/wave2-a-room.md. The room transitions
// underneath them are covered by internal/room tests, and the
// voice Manager's own Join/Play paths by internal/voice tests.
package handlers

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/config"
	"parlay/backend-go/internal/download"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

const testChatID = int64(-1001000000001)

var (
	trackA = model.Track{ID: "yt:aaa111aaa11", Title: "Song A", Artist: "Artist A", Source: "youtube", DurationSec: 300, StreamURL: "https://example.invalid/a.opus"}
	trackB = model.Track{ID: "yt:bbb222bbb22", Title: "Song B", Artist: "Artist B", Source: "youtube", DurationSec: 200, StreamURL: "https://example.invalid/b.opus"}
)

// testEnv is one isolated backend wiring: temp store + config +
// registry + UDS server (with a core-side client attached, so
// emitted events can be read) + a voice Manager whose join always
// fails with a canned error (no core, no Telegram).
type testEnv struct {
	t        *testing.T
	dir      string
	st       *store.Store
	cfg      *config.Config
	reg      *resolver.Registry
	srv      *uds.Server
	conn     net.Conn
	voiceMgr *voice.Manager
	deps     *app.Deps
	h        *Handlers
}

func newTestEnv(t *testing.T) *testEnv {
	t.Helper()
	dir := t.TempDir()
	tomlPath := filepath.Join(dir, "parlay.toml")
	if err := os.WriteFile(tomlPath, []byte("[backend]\ndata_dir = "+fmt.Sprintf("%q", dir)+"\n"), 0o600); err != nil {
		t.Fatalf("write parlay.toml: %v", err)
	}
	cfg, err := config.LoadFrom(tomlPath)
	if err != nil {
		t.Fatalf("config.LoadFrom: %v", err)
	}
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	reg := resolver.NewRegistry()
	sock := filepath.Join(dir, "parlay-backend.sock")
	srv := uds.NewServer(sock)
	if err := srv.Listen(); err != nil {
		t.Fatalf("server.Listen: %v", err)
	}
	conn, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatalf("dial server: %v", err)
	}
	// The server accepts asynchronously; wait for registration
	// instead of sleeping (see uds.WaitForPeer).
	wctx, wcancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer wcancel()
	if err := srv.WaitForPeer(wctx); err != nil {
		t.Fatalf("WaitForPeer: %v", err)
	}
	client, err := voice.New()
	if err != nil {
		t.Fatalf("voice.New: %v", err)
	}
	voiceMgr := voice.NewManager(client, func(ctx context.Context, chatID int64, localSDP string) (string, int64, error) {
		return "", 0, errors.New("test join refused: no core connected")
	})
	rooms := room.NewManager(st, cfg.Limits.MaxQueueSize)
	lyr, err := lyrics.New(reg)
	if err != nil {
		t.Fatalf("lyrics.New: %v", err)
	}
	deps := app.New(st, cfg, reg, voiceMgr, srv, rooms, lyr)
	h := New(deps)
	e := &testEnv{t: t, dir: dir, st: st, cfg: cfg, reg: reg, srv: srv, conn: conn, voiceMgr: voiceMgr, deps: deps, h: h}
	t.Cleanup(func() {
		_ = conn.Close()
		_ = srv.Close()
		client.Close()
		_ = st.Close()
	})
	return e
}

// call invokes a handler method with a JSON payload and the test
// chat id in the envelope.
func (e *testEnv) call(fn func(context.Context, uds.Request) (any, error), payload string) (any, error) {
	return fn(context.Background(), uds.Request{
		ChatID:    testChatID,
		HasChatID: true,
		Payload:   json.RawMessage(payload),
	})
}

// callChat invokes a handler method for an arbitrary chat id.
func (e *testEnv) callChat(fn func(context.Context, uds.Request) (any, error), chatID int64, payload string) (any, error) {
	return fn(context.Background(), uds.Request{
		ChatID:    chatID,
		HasChatID: true,
		Payload:   json.RawMessage(payload),
	})
}

// errCode extracts the HandlerError code, or "" when the error is
// not a HandlerError (a plain error surfaces as internal on the
// wire, so a test expecting a code must fail on "").
func errCode(err error) uds.ErrorCode {
	var he *uds.HandlerError
	if errors.As(err, &he) {
		return he.Code
	}
	return ""
}

// snapshot returns the room snapshot for the test chat, failing
// the test when no room exists.
func (e *testEnv) snapshot(t *testing.T) model.NowPlaying {
	t.Helper()
	snap, ok := e.h.rooms.Snapshot(testChatID)
	if !ok {
		t.Fatalf("no room for test chat")
	}
	return snap
}

// playCurrent starts trackA as the current track (room-level, no
// voice) and returns the snapshot.
func (e *testEnv) playCurrent(t *testing.T) model.NowPlaying {
	t.Helper()
	if _, err := e.h.rooms.Play(testChatID, trackA, 7); err != nil {
		t.Fatalf("rooms.Play: %v", err)
	}
	return e.snapshot(t)
}

// readEvent reads one event frame from the core-side client.
func (e *testEnv) readEvent(t *testing.T) map[string]any {
	t.Helper()
	if err := e.conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline: %v", err)
	}
	var header [4]byte
	if _, err := e.conn.Read(header[:]); err != nil {
		t.Fatalf("read frame header: %v", err)
	}
	length := binary.LittleEndian.Uint32(header[:])
	payload := make([]byte, length)
	for off := 0; off < int(length); {
		n, err := e.conn.Read(payload[off:])
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

// expectNoEvent fails when any event frame arrives within 300 ms.
func (e *testEnv) expectNoEvent(t *testing.T) {
	t.Helper()
	if err := e.conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond)); err != nil {
		t.Fatalf("SetReadDeadline: %v", err)
	}
	var header [4]byte
	if _, err := e.conn.Read(header[:]); err == nil {
		t.Fatalf("expected no event, got a frame")
	} else if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("expected a read timeout, got: %v", err)
	}
}

// waitSnapshot polls until cond holds on the test chat's snapshot
// (or the deadline passes and the test fails).
func (e *testEnv) waitSnapshot(t *testing.T, cond func(model.NowPlaying, bool) bool, what string) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		snap, ok := e.h.rooms.Snapshot(testChatID)
		if cond(snap, ok) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for snapshot: %s", what)
}

// --- vc.play -----------------------------------------------------------

func TestVCPlayValidation(t *testing.T) {
	e := newTestEnv(t)
	if _, err := e.callChat(e.h.vcPlay, 0, `{"query":"x"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play chat 0: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcPlay, `{"query":""}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play empty query: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcPlay, `{"query":"   "}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play blank query: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcPlay, `{"query":"yt:aaa","speed":9}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play speed 9: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcPlay, `{"query":"yt:aaa","bass":-1}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play bass -1: err=%v, want bad_request", err)
	}
}

// TestVCPlayResolveFailure uses the Spotify lane with no creds:
// the resolver fails honestly before any network, so vc.play
// answers engine_unavailable and the room is never created.
func TestVCPlayResolveFailure(t *testing.T) {
	e := newTestEnv(t)
	t.Setenv("SPOTIFY_CLIENT_ID", "")
	t.Setenv("SPOTIFY_CLIENT_SECRET", "")
	_, err := e.call(e.h.vcPlay, `{"query":"spotify:track:4uLU6hMCjMI75M1A2tKUQC"}`)
	if errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.play spotify no-creds: err=%v, want engine_unavailable", err)
	}
	if !strings.Contains(err.Error(), "SPOTIFY_CLIENT_ID") {
		t.Fatalf("vc.play spotify no-creds: err=%v, want the missing-credential message", err)
	}
	if e.h.rooms.HasRoom(testChatID) {
		t.Fatalf("vc.play resolve failure created a room")
	}
}

// --- vc.pause / vc.resume ----------------------------------------------

func TestVCPauseResume(t *testing.T) {
	e := newTestEnv(t)
	// Idle pause is not_found.
	if _, err := e.call(e.h.vcPause, `{}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.pause idle: err=%v, want not_found", err)
	}
	e.playCurrent(t)
	resp, err := e.call(e.h.vcPause, `{}`)
	if err != nil {
		t.Fatalf("vc.pause: %v", err)
	}
	if !resp.(map[string]any)["now_playing"].(model.NowPlaying).IsPaused {
		t.Fatalf("vc.pause: snapshot not paused: %+v", resp)
	}
	// Idempotent when already paused.
	if _, err := e.call(e.h.vcPause, `{}`); err != nil {
		t.Fatalf("vc.pause again: %v", err)
	}
	// Resume with no call record and no feed running fails
	// honestly and leaves the room paused (audio cannot resume
	// without a call; the room must not claim to be playing).
	if _, err := e.call(e.h.vcResume, `{}`); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.resume no call: err=%v, want engine_unavailable", err)
	}
	if !e.snapshot(t).IsPaused {
		t.Fatalf("vc.resume failure un-paused the room")
	}
	// Resume with a feed recorded but no call record fails the
	// same way (the paused feed cannot resume without its call).
	e.h.setFeeding(testChatID, trackA.ID)
	if _, err := e.call(e.h.vcResume, `{}`); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.resume feeding but no call: err=%v, want engine_unavailable", err)
	}
	if !e.snapshot(t).IsPaused {
		t.Fatalf("vc.resume feeding failure un-paused the room")
	}
}

// --- vc.skip / vc.stop / vc.seek ----------------------------------------

func TestVCSkipJoinFailure(t *testing.T) {
	e := newTestEnv(t)
	e.playCurrent(t)
	// Skip ensures the call first: the test join fails, so the
	// skip fails engine_unavailable, the room is untouched, and
	// exactly one VoiceError (join_failed) is emitted — no
	// TrackAdvanced, no TrackStarted.
	_, err := e.call(e.h.vcSkip, `{}`)
	if errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.skip join failure: err=%v, want engine_unavailable", err)
	}
	if got := e.snapshot(t).Track; got == nil || got.ID != trackA.ID {
		t.Fatalf("vc.skip join failure changed the room: %+v", got)
	}
	env := e.readEvent(t)
	if env["event"] != "VoiceError" {
		t.Fatalf("vc.skip join failure event: %v, want VoiceError", env)
	}
	if env["data"].(map[string]any)["code"] != "join_failed" {
		t.Fatalf("vc.skip join failure event data: %v", env["data"])
	}
	e.expectNoEvent(t)
	// Fully empty room: not_found, without attempting a join.
	if _, err := e.callChat(e.h.vcSkip, testChatID+1, `{}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.skip empty: err=%v, want not_found", err)
	}
}

func TestVCStop(t *testing.T) {
	e := newTestEnv(t)
	e.playCurrent(t)
	if err := e.h.rooms.Enqueue(testChatID, trackB, 7); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	resp, err := e.call(e.h.vcStop, `{}`)
	if err != nil {
		t.Fatalf("vc.stop: %v", err)
	}
	if len(resp.(map[string]any)) != 0 {
		t.Fatalf("vc.stop response: %v, want {}", resp)
	}
	snap, ok := e.h.rooms.Snapshot(testChatID)
	if ok && (snap.Track != nil || len(snap.Queue) != 0) {
		t.Fatalf("vc.stop left state: %+v", snap)
	}
	// Idempotent: always 200, even with no room.
	if _, err := e.callChat(e.h.vcStop, testChatID+2, `{}`); err != nil {
		t.Fatalf("vc.stop empty: %v", err)
	}
}

func TestVCSeek(t *testing.T) {
	e := newTestEnv(t)
	if _, err := e.call(e.h.vcSeek, `{"position_sec":10}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.seek idle: err=%v, want not_found", err)
	}
	e.playCurrent(t)
	if _, err := e.call(e.h.vcSeek, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.seek missing position: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcSeek, `{"position_sec":-1}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.seek negative: err=%v, want bad_request", err)
	}
	resp, err := e.call(e.h.vcSeek, `{"position_sec":60}`)
	if err != nil {
		t.Fatalf("vc.seek 60: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).PositionSec; got != 60 {
		t.Fatalf("vc.seek 60: position=%d", got)
	}
	// Clamped to the duration (300).
	resp, err = e.call(e.h.vcSeek, `{"position_sec":9999}`)
	if err != nil {
		t.Fatalf("vc.seek 9999: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).PositionSec; got != 300 {
		t.Fatalf("vc.seek 9999: position=%d, want 300", got)
	}
	// Seek to 0 replays the clock (no call record: no rebind).
	resp, err = e.call(e.h.vcSeek, `{"position_sec":0}`)
	if err != nil {
		t.Fatalf("vc.seek 0: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).PositionSec; got != 0 {
		t.Fatalf("vc.seek 0: position=%d", got)
	}
}

// --- vc.queue ------------------------------------------------------------

func TestVCQueue(t *testing.T) {
	e := newTestEnv(t)
	// List on an unknown chat is [] (200), never null.
	resp, err := e.call(e.h.vcQueue, `{"action":"list"}`)
	if err != nil {
		t.Fatalf("vc.queue list empty: %v", err)
	}
	q := resp.(map[string]any)["queue"].([]model.QueueItem)
	if q == nil || len(q) != 0 {
		t.Fatalf("vc.queue list empty: %v", resp)
	}
	// Add a literal track (source defaults to youtube); add does
	// NOT start playback even when idle.
	resp, err = e.call(e.h.vcQueue, `{"action":"add","track":{"id":"yt:ccc333ccc33","title":"Song C"}}`)
	if err != nil {
		t.Fatalf("vc.queue add: %v", err)
	}
	q = resp.(map[string]any)["queue"].([]model.QueueItem)
	if len(q) != 1 || q[0].Track.Source != "youtube" || q[0].Track.ID != "yt:ccc333ccc33" {
		t.Fatalf("vc.queue add: %v", resp)
	}
	if _, ok := e.h.rooms.Current(testChatID); ok {
		t.Fatalf("vc.queue add started playback")
	}
	// Add with a query that cannot resolve (Spotify, no creds).
	t.Setenv("SPOTIFY_CLIENT_ID", "")
	t.Setenv("SPOTIFY_CLIENT_SECRET", "")
	if _, err := e.call(e.h.vcQueue, `{"action":"add","query":"spotify:track:4uLU6hMCjMI75M1A2tKUQC"}`); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.queue add query: err=%v, want engine_unavailable", err)
	}
	// Add with neither track nor query.
	if _, err := e.call(e.h.vcQueue, `{"action":"add"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.queue add empty: err=%v, want bad_request", err)
	}
	// Remove out of range, remove, clear.
	if _, err := e.call(e.h.vcQueue, `{"action":"remove","position":5}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.queue remove oob: err=%v, want bad_request", err)
	}
	resp, err = e.call(e.h.vcQueue, `{"action":"remove","position":0}`)
	if err != nil {
		t.Fatalf("vc.queue remove: %v", err)
	}
	if len(resp.(map[string]any)["queue"].([]model.QueueItem)) != 0 {
		t.Fatalf("vc.queue remove: %v", resp)
	}
	if _, err := e.call(e.h.vcQueue, `{"action":"bogus"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.queue bogus action: err=%v, want bad_request", err)
	}
	// Clear on an empty queue is a 200 with [].
	resp, err = e.call(e.h.vcQueue, `{"action":"clear"}`)
	if err != nil {
		t.Fatalf("vc.queue clear: %v", err)
	}
	if len(resp.(map[string]any)["queue"].([]model.QueueItem)) != 0 {
		t.Fatalf("vc.queue clear: %v", resp)
	}
}

// --- vc.loop / vc.shuffle / vc.autoplay / vc.volume / vc.mute ----------

func TestVCControls(t *testing.T) {
	e := newTestEnv(t)
	// Idle controls are not_found.
	if _, err := e.call(e.h.vcLoop, `{"count":3}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.loop idle: err=%v, want not_found", err)
	}
	if _, err := e.call(e.h.vcVolume, `{"volume":50}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.volume idle: err=%v, want not_found", err)
	}
	if _, err := e.call(e.h.vcMute, `{"muted":true}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.mute idle: err=%v, want not_found", err)
	}
	e.playCurrent(t)
	// Loop: missing / out of range / set.
	if _, err := e.call(e.h.vcLoop, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.loop missing count: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcLoop, `{"count":11}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.loop 11: err=%v, want bad_request", err)
	}
	resp, err := e.call(e.h.vcLoop, `{"count":3}`)
	if err != nil {
		t.Fatalf("vc.loop 3: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).Loop; got != 3 {
		t.Fatalf("vc.loop 3: loop=%d", got)
	}
	// Shuffle + autoplay flags.
	if _, err := e.call(e.h.vcShuffle, `{"enabled":true}`); err != nil {
		t.Fatalf("vc.shuffle on: %v", err)
	}
	if !e.snapshot(t).Shuffle {
		t.Fatalf("vc.shuffle on: flag not set")
	}
	if _, err := e.call(e.h.vcAutoplay, `{"enabled":true}`); err != nil {
		t.Fatalf("vc.autoplay on: %v", err)
	}
	if !e.snapshot(t).Autoplay {
		t.Fatalf("vc.autoplay on: flag not set")
	}
	if _, err := e.call(e.h.vcShuffle, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.shuffle missing enabled: err=%v, want bad_request", err)
	}
	// Volume clamps to 200 (vc-go rule), it does not error.
	resp, err = e.call(e.h.vcVolume, `{"volume":250}`)
	if err != nil {
		t.Fatalf("vc.volume 250: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).Volume; got != 200 {
		t.Fatalf("vc.volume 250: volume=%d, want 200", got)
	}
	if _, err := e.call(e.h.vcVolume, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.volume missing: err=%v, want bad_request", err)
	}
	// Mute is a real flag, orthogonal to volume; repeated
	// mute/unmute is a 200.
	resp, err = e.call(e.h.vcMute, `{"muted":true}`)
	if err != nil {
		t.Fatalf("vc.mute on: %v", err)
	}
	if !resp.(map[string]any)["muted"].(bool) || !resp.(map[string]any)["now_playing"].(model.NowPlaying).IsMuted {
		t.Fatalf("vc.mute on: %v", resp)
	}
	if _, err := e.call(e.h.vcMute, `{"muted":true}`); err != nil {
		t.Fatalf("vc.mute again: %v", err)
	}
	resp, err = e.call(e.h.vcMute, `{"muted":false}`)
	if err != nil {
		t.Fatalf("vc.mute off: %v", err)
	}
	if resp.(map[string]any)["muted"].(bool) {
		t.Fatalf("vc.mute off: %v", resp)
	}
	if _, err := e.call(e.h.vcMute, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.mute missing: err=%v, want bad_request", err)
	}
}

// --- vc.now_playing / vc.progress ----------------------------------------

func TestVCNowPlayingProgress(t *testing.T) {
	e := newTestEnv(t)
	if _, err := e.call(e.h.vcNowPlaying, `{}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.now_playing idle: err=%v, want not_found", err)
	}
	if _, err := e.call(e.h.vcProgress, `{}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.progress idle: err=%v, want not_found", err)
	}
	// Queue-only room: 200 with track null (vc-go rule).
	if err := e.h.rooms.Enqueue(testChatID, trackA, 7); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	resp, err := e.call(e.h.vcNowPlaying, `{}`)
	if err != nil {
		t.Fatalf("vc.now_playing queue-only: %v", err)
	}
	if got := resp.(map[string]any)["now_playing"].(model.NowPlaying).Track; got != nil {
		t.Fatalf("vc.now_playing queue-only: track=%+v, want null", got)
	}
	resp, err = e.call(e.h.vcProgress, `{}`)
	if err != nil {
		t.Fatalf("vc.progress queue-only: %v", err)
	}
	m := resp.(map[string]any)
	// "now" holds a nil *Track in the map (a typed nil), so the
	// check must assert through the pointer type, not the
	// interface.
	if now, _ := m["now"].(*model.Track); now != nil {
		t.Fatalf("vc.progress queue-only: now=%v, want null", m["now"])
	}
	next, ok := m["next"].(*model.Track)
	if !ok || next == nil || next.ID != trackA.ID {
		t.Fatalf("vc.progress queue-only: next=%v", m["next"])
	}
	// With a current track: now set, duration from the track.
	e.playCurrent(t)
	resp, err = e.call(e.h.vcProgress, `{}`)
	if err != nil {
		t.Fatalf("vc.progress: %v", err)
	}
	m = resp.(map[string]any)
	if m["duration_sec"] != int64(300) || m["is_paused"] != false || m["loop_enabled"] != false {
		t.Fatalf("vc.progress: %v", m)
	}
}

// --- vc.panel ------------------------------------------------------------

func TestVCPanel(t *testing.T) {
	e := newTestEnv(t)
	if _, err := e.call(e.h.vcPanel, `{}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.panel GET idle: err=%v, want not_found", err)
	}
	e.playCurrent(t)
	resp, err := e.call(e.h.vcPanel, `{}`)
	if err != nil {
		t.Fatalf("vc.panel GET: %v", err)
	}
	m := resp.(map[string]any)
	panel, ok := m["panel"].(map[string]any)
	if !ok {
		t.Fatalf("vc.panel GET: no panel key: %v", m)
	}
	if len(panel["buttons"].([]string)) != 10 {
		t.Fatalf("vc.panel GET: buttons=%v, want 10", panel["buttons"])
	}
	if panel["loop"] != false || !strings.Contains(panel["text"].(string), "Song A") {
		t.Fatalf("vc.panel GET: panel=%v", panel)
	}
	// POST update + unknown action.
	resp, err = e.call(e.h.vcPanel, `{"action":"update"}`)
	if err != nil {
		t.Fatalf("vc.panel update: %v", err)
	}
	if resp.(map[string]any)["message"] != "Panel updated." {
		t.Fatalf("vc.panel update: %v", resp)
	}
	if _, err := e.call(e.h.vcPanel, `{"action":"explode"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.panel bogus: err=%v, want bad_request", err)
	}
	// POST loop toggle: 0 -> 10 -> 0.
	resp, err = e.call(e.h.vcPanel, `{"action":"loop"}`)
	if err != nil {
		t.Fatalf("vc.panel loop: %v", err)
	}
	if resp.(map[string]any)["message"] != "Loop on (10)." {
		t.Fatalf("vc.panel loop: %v", resp)
	}
	resp, err = e.call(e.h.vcPanel, `{"action":"loop"}`)
	if err != nil {
		t.Fatalf("vc.panel loop off: %v", err)
	}
	if resp.(map[string]any)["message"] != "Loop off." {
		t.Fatalf("vc.panel loop off: %v", resp)
	}
	// POST volinc/voldec move the volume by 10.
	resp, err = e.call(e.h.vcPanel, `{"action":"volinc"}`)
	if err != nil {
		t.Fatalf("vc.panel volinc: %v", err)
	}
	if resp.(map[string]any)["message"] != "Volume 110." {
		t.Fatalf("vc.panel volinc: %v", resp)
	}
	// POST skip on a queue-only room: "Not streaming." (400).
	if _, err := e.h.rooms.Stop(testChatID); err != nil {
		t.Fatalf("Stop: %v", err)
	}
	if err := e.h.rooms.Enqueue(testChatID, trackA, 7); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	if _, err := e.call(e.h.vcPanel, `{"action":"skip"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.panel skip queue-only: err=%v, want bad_request", err)
	}
}

// --- vc.lyrics -------------------------------------------------------------

func TestVCLyrics(t *testing.T) {
	e := newTestEnv(t)
	// No title and nothing playing.
	if _, err := e.call(e.h.vcLyrics, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.lyrics idle: err=%v, want bad_request", err)
	}
	// Current track with an empty title: the handler rejects the
	// lookup before any fetch (no title to look up is a client
	// error, not a lyrics miss).
	if _, err := e.h.rooms.Play(testChatID, model.Track{ID: "yt:x", Title: "", Source: "youtube"}, 7); err != nil {
		t.Fatalf("Play: %v", err)
	}
	if _, err := e.call(e.h.vcLyrics, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.lyrics empty title: err=%v, want bad_request", err)
	}
	// Nil fetcher: engine_unavailable "lyrics not configured".
	deps2 := *e.deps
	deps2.Lyrics = nil
	h2 := New(&deps2)
	if _, err := h2.vcLyrics(context.Background(), uds.Request{ChatID: testChatID, HasChatID: true, Payload: json.RawMessage(`{"title":"x"}`)}); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.lyrics nil fetcher: err=%v, want engine_unavailable", err)
	}
}

// --- vc.join / vc.radio ----------------------------------------------------

func TestVCJoinFailure(t *testing.T) {
	e := newTestEnv(t)
	_, err := e.call(e.h.vcJoin, `{}`)
	if errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.join failure: err=%v, want engine_unavailable", err)
	}
	if e.h.rooms.HasRoom(testChatID) {
		t.Fatalf("vc.join failure created a room")
	}
	env := e.readEvent(t)
	if env["event"] != "VoiceError" || env["data"].(map[string]any)["code"] != "join_failed" {
		t.Fatalf("vc.join failure event: %v", env)
	}
	e.expectNoEvent(t)
}

func TestVCRadio(t *testing.T) {
	e := newTestEnv(t)
	if _, err := e.call(e.h.vcRadio, `{"action":"stop"}`); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.radio stop none: err=%v, want not_found", err)
	}
	if _, err := e.call(e.h.vcRadio, `{"action":"start","station":"no-such-station"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.radio start unknown: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcRadio, `{"action":"start"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.radio start missing station: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcRadio, `{"action":"bogus"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.radio bogus: err=%v, want bad_request", err)
	}
}

// --- vc.play_file -----------------------------------------------------------

func TestVCPlayFileValidation(t *testing.T) {
	e := newTestEnv(t)
	// Missing file_path.
	if _, err := e.call(e.h.vcPlayFile, `{}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file missing path: err=%v, want bad_request", err)
	}
	// Path outside the data dir (.. escape).
	if _, err := e.call(e.h.vcPlayFile, `{"file_path":"/etc/passwd"}`); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file outside: err=%v, want bad_request", err)
	}
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q}`, filepath.Join(e.dir, "..", "escape.opus"))); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file dotdot: err=%v, want bad_request", err)
	}
	// Missing file inside the data dir.
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q}`, filepath.Join(e.dir, "gone.opus"))); errCode(err) != uds.ErrNotFound {
		t.Fatalf("vc.play_file missing file: err=%v, want not_found", err)
	}
	// A directory, not a file.
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q}`, e.dir)); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file directory: err=%v, want bad_request", err)
	}
	// Unknown media kind.
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q,"kind":"sticker"}`, filepath.Join(e.dir, "gone.opus"))); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file unknown kind: err=%v, want bad_request", err)
	}
	// Negative duration.
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q,"duration_sec":-5}`, filepath.Join(e.dir, "gone.opus"))); errCode(err) != uds.ErrBadRequest {
		t.Fatalf("vc.play_file negative duration: err=%v, want bad_request", err)
	}
	// A real file passes validation — then the join fails (no
	// core), so the action fails engine_unavailable with the room
	// untouched. (The success path needs a live call and is
	// unmeasured; see the package doc.)
	real := filepath.Join(e.dir, "reply.opus")
	if err := os.WriteFile(real, []byte("fake-opus"), 0o600); err != nil {
		t.Fatalf("write reply file: %v", err)
	}
	if _, err := e.call(e.h.vcPlayFile, fmt.Sprintf(`{"file_path":%q,"title":"Reply"}`, real)); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.play_file real file: err=%v, want engine_unavailable (join)", err)
	}
	if e.h.rooms.HasRoom(testChatID) {
		t.Fatalf("vc.play_file join failure created a room")
	}
	// A relative path resolves against the data dir (same rule as
	// download's validateRepliedPath): "reply.opus" is the file
	// just written, so it passes validation and fails at the
	// join — a bad_request here would mean it resolved against
	// the process cwd instead.
	if _, err := e.call(e.h.vcPlayFile, `{"file_path":"reply.opus","title":"Reply"}`); errCode(err) != uds.ErrEngineUnavailable {
		t.Fatalf("vc.play_file relative path: err=%v, want engine_unavailable (join)", err)
	}
}

// TestVCPlayFileVoiceState pins the voice_state shape vc.play_file
// reports (§3.3): joined reads the live call record (false with
// no call — never claimed), streaming is always false for a local
// file in Phase 3, and the detail is the §3.3 wording verbatim.
// The joined=true case needs a connected call and is unmeasured.
func TestVCPlayFileVoiceState(t *testing.T) {
	e := newTestEnv(t)
	vs := e.h.voiceStateForFile(testChatID)
	if vs["joined"] != false {
		t.Fatalf("voice_state joined: %v, want false with no call", vs)
	}
	if vs["streaming"] != false {
		t.Fatalf("voice_state streaming: %v, want false for a local file", vs)
	}
	if vs["detail"] != "local file — audio path lands with the DSP" {
		t.Fatalf("voice_state detail: %q", vs["detail"])
	}
}

// readFrame reads one whole UDS frame from the core-side client
// and decodes it as a generic envelope map.
func (e *testEnv) readFrame(t *testing.T) map[string]any {
	t.Helper()
	if err := e.conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline: %v", err)
	}
	raw, err := uds.ReadFrame(e.conn)
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	var env map[string]any
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("decode frame: %v", err)
	}
	return env
}

// TestWiringPlayFileIsHandlersVersion proves the main.go wiring
// order end to end: download.Register runs first, handlers.Register
// second, and uds.Register overwrites silently — so a vc.play_file
// dispatched over the socket reaches the handlers record-only
// version, not download's feed-binding duplicate. The two versions
// are indistinguishable by their join-failure error alone (both
// fail engine_unavailable at the join with no core), but only the
// handlers version emits VoiceError(join_failed) on that failure,
// so the event frame before the error response is the fingerprint.
func TestWiringPlayFileIsHandlersVersion(t *testing.T) {
	e := newTestEnv(t)
	download.Register(e.srv, e.deps) // main.go order: download first
	Register(e.srv, e.deps)          // handlers second — wins the slot
	real := filepath.Join(e.dir, "reply.opus")
	if err := os.WriteFile(real, []byte("fake-opus"), 0o600); err != nil {
		t.Fatalf("write reply file: %v", err)
	}
	req, err := json.Marshal(map[string]any{
		"type": "request", "request_id": 1, "action": "vc.play_file",
		"chat_id": testChatID, "deadline_ms": 10000,
		"payload": map[string]any{"chat_id": testChatID, "file_path": real, "title": "Reply"},
	})
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}
	if err := uds.WriteFrame(e.conn, req); err != nil {
		t.Fatalf("WriteFrame: %v", err)
	}
	first := e.readFrame(t)
	if first["type"] != "event" || first["event"] != "VoiceError" {
		t.Fatalf("first frame: %v, want a VoiceError event (handlers vc.play_file)", first)
	}
	if first["data"].(map[string]any)["code"] != "join_failed" {
		t.Fatalf("VoiceError data: %v, want code join_failed", first["data"])
	}
	second := e.readFrame(t)
	if second["type"] != "response" || second["request_id"] != float64(1) || second["ok"] != false {
		t.Fatalf("second frame: %v, want the error response for request 1", second)
	}
	errObj, _ := second["error"].(map[string]any)
	if errObj["code"] != "engine_unavailable" {
		t.Fatalf("response error: %v, want engine_unavailable", second["error"])
	}
	e.expectNoEvent(t)
}

// --- feed-end advance (event-driven, no polling) ----------------------------

func TestFeedEndAdvance(t *testing.T) {
	e := newTestEnv(t)
	e.playCurrent(t)
	if err := e.h.rooms.Enqueue(testChatID, trackB, 7); err != nil {
		t.Fatalf("Enqueue: %v", err)
	}
	// No autoplay pick in this test: the stub returns "no pick".
	e.h.rooms.SetPicker(func(ctx context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		return model.Track{}, nil
	})
	e.h.setFeeding(testChatID, trackA.ID)
	// The feeder's EOF: advance to B, fire TrackAdvanced +
	// TrackStarted (in that order), clear the feeding record (no
	// call record exists, so no feed is bound for B).
	e.h.onFeedEnd(testChatID, nil)
	e.waitSnapshot(t, func(snap model.NowPlaying, ok bool) bool {
		return ok && snap.Track != nil && snap.Track.ID == trackB.ID
	}, "current track B after feed end")
	if _, ok := e.h.getFeeding(testChatID); ok {
		t.Fatalf("feed-end advance left a stale feeding record")
	}
	env := e.readEvent(t)
	if env["event"] != "TrackAdvanced" {
		t.Fatalf("feed-end first event: %v, want TrackAdvanced", env)
	}
	if env["data"].(map[string]any)["previous_track_id"] != trackA.ID {
		t.Fatalf("feed-end TrackAdvanced data: %v", env["data"])
	}
	env = e.readEvent(t)
	if env["event"] != "TrackStarted" {
		t.Fatalf("feed-end second event: %v, want TrackStarted", env)
	}
	e.expectNoEvent(t)
}

func TestFeedEndErrorGoesIdle(t *testing.T) {
	e := newTestEnv(t)
	e.playCurrent(t)
	e.h.rooms.SetPicker(func(ctx context.Context, last model.Track, exclude map[string]bool) (model.Track, error) {
		return model.Track{}, nil
	})
	e.h.setFeeding(testChatID, trackA.ID)
	// A dead stream advances anyway (the queue must not wedge):
	// VoiceError first, then TrackEnded + QueueEmpty, room idle.
	e.h.onFeedEnd(testChatID, errors.New("ffmpeg: connection reset"))
	e.waitSnapshot(t, func(snap model.NowPlaying, ok bool) bool {
		return ok && snap.Track == nil
	}, "idle after feed error")
	env := e.readEvent(t)
	if env["event"] != "VoiceError" || env["data"].(map[string]any)["code"] != "stream_error" {
		t.Fatalf("feed-error first event: %v, want VoiceError stream_error", env)
	}
	env = e.readEvent(t)
	if env["event"] != "TrackEnded" {
		t.Fatalf("feed-error second event: %v, want TrackEnded", env)
	}
	env = e.readEvent(t)
	if env["event"] != "QueueEmpty" {
		t.Fatalf("feed-error third event: %v, want QueueEmpty", env)
	}
	e.expectNoEvent(t)
}

// --- mix picker parsing -------------------------------------------------------

func TestParseMixJSON(t *testing.T) {
	body := `{"contents":{"twoColumnWatchNextResults":{"playlist":{"playlist":{"contents":[
		{"playlistPanelVideoRenderer":{"videoId":"aaa111aaa11"}},
		{"playlistPanelVideoRenderer":{"videoId":"bbb222bbb22"}},
		{"otherRenderer":{"videoId":"ccc333ccc33"}},
		{"playlistPanelVideoRenderer":{}},
		{"playlistPanelVideoRenderer":{"videoId":"ddd444ddd44"}}
	]}}}}}`
	ids, err := parseMixJSON([]byte(body), "aaa111aaa11")
	if err != nil {
		t.Fatalf("parseMixJSON: %v", err)
	}
	want := []string{"aaa111aaa11", "bbb222bbb22", "ddd444ddd44"}
	if len(ids) != len(want) {
		t.Fatalf("parseMixJSON: %v, want %v", ids, want)
	}
	for i := range want {
		if ids[i] != want[i] {
			t.Fatalf("parseMixJSON: %v, want %v", ids, want)
		}
	}
	if _, err := parseMixJSON([]byte(`{"contents":{}}`), "x"); err == nil {
		t.Fatalf("parseMixJSON missing playlist: want an error")
	}
	if _, err := parseMixJSON([]byte(`not json`), "x"); err == nil {
		t.Fatalf("parseMixJSON bad JSON: want an error")
	}
}

func TestMixVideoID(t *testing.T) {
	cases := []struct{ in, want string }{
		{"yt:dQw4w9WgXcQ", "dQw4w9WgXcQ"},
		{"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"},
		{"jsvn:123", ""},
		{"spotify:track:x", ""},
		{"", ""},
	}
	for _, c := range cases {
		track := model.Track{ID: c.in, URL: c.in}
		if got := mixVideoID(track); got != c.want {
			t.Fatalf("mixVideoID(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// waitRoom polls until cond holds on the room snapshot, or fails.
func waitRoom(t *testing.T, h *Handlers, chatID int64, cond func(model.NowPlaying) bool, what string) model.NowPlaying {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if snap, ok := h.rooms.Snapshot(chatID); ok && cond(snap) {
			return snap
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("room did not become %s within 5s", what)
	return model.NowPlaying{}
}

// TestTransitionAdvancesWithoutRebind proves the gapless path: the
// DSP promotes the next slot (track B) and the room advances to B
// WITHOUT rebinding — no voice call is needed, no bind is
// attempted, the feeding record simply moves to B. The room state
// and the DSP stay in agreement with zero gap.
func TestTransitionAdvancesWithoutRebind(t *testing.T) {
	e := newTestEnv(t)
	Register(e.srv, e.deps)
	chatID := testChatID
	if _, err := e.h.rooms.Play(chatID, trackA, 7); err != nil {
		t.Fatalf("rooms.Play A: %v", err)
	}
	if _, err := e.h.rooms.Play(chatID, trackB, 7); err != nil {
		t.Fatalf("rooms.Play B (queue): %v", err)
	}
	e.h.setFeeding(chatID, trackA.ID)
	// The DSP promotes B's slot. There is no voice call in this
	// test env (join always fails), so any rebind attempt would
	// surface — the point is that none happens.
	e.h.onTransition(chatID, trackB.ID)
	snap := waitRoom(t, e.h, chatID, func(s model.NowPlaying) bool {
		return s.Track != nil && s.Track.ID == trackB.ID
	}, "track B")
	if snap.Track == nil || snap.Track.ID != trackB.ID {
		t.Fatalf("current after transition = %+v, want track B", snap.Track)
	}
	if feeding, ok := e.h.getFeeding(chatID); !ok || feeding != trackB.ID {
		t.Fatalf("feeding after transition = %q ok=%v, want track B", feeding, ok)
	}
	if _, ok := e.deps.Voice.Get(chatID); ok {
		t.Fatal("a voice call appeared during a no-rebind transition")
	}
}

// TestTransitionLoopReplay proves loop goes through the DSP's
// gapless path: the replayed track IS the transitioned track, so
// the room replays it without rebinding and the loop counter is
// consumed exactly once.
func TestTransitionLoopReplay(t *testing.T) {
	e := newTestEnv(t)
	Register(e.srv, e.deps)
	chatID := testChatID
	if _, err := e.h.rooms.Play(chatID, trackA, 7); err != nil {
		t.Fatalf("rooms.Play A: %v", err)
	}
	if _, err := e.h.rooms.SetLoop(chatID, 1); err != nil {
		t.Fatalf("rooms.SetLoop: %v", err)
	}
	e.h.setFeeding(chatID, trackA.ID)
	e.h.onTransition(chatID, trackA.ID)
	snap := waitRoom(t, e.h, chatID, func(s model.NowPlaying) bool {
		return s.Loop == 0 && s.Track != nil && s.Track.ID == trackA.ID
	}, "loop consumed, still track A")
	if snap.Loop != 0 {
		t.Fatalf("loop after transition = %d, want 0 (one replay consumed)", snap.Loop)
	}
	if feeding, ok := e.h.getFeeding(chatID); !ok || feeding != trackA.ID {
		t.Fatalf("feeding after loop transition = %q ok=%v, want track A", feeding, ok)
	}
}

// TestTransitionMismatchRebinds proves the repair path: the DSP
// plays track C, but the room's queue top is B (the queue was
// mutated between LoadNext and the promotion), so the room
// advances to B and rebinds to it instead of playing the wrong
// track under the right name. With no voice call in this env the
// rebind is best-effort (the room stands, the feeding record is
// cleared); with a live call it would Play B.
func TestTransitionMismatchRebinds(t *testing.T) {
	e := newTestEnv(t)
	Register(e.srv, e.deps)
	chatID := testChatID
	trackC := model.Track{ID: "yt:ccc333ccc33", Title: "Song C", Artist: "Artist C", Source: "youtube", DurationSec: 200, StreamURL: "https://example.invalid/c.opus"}
	if _, err := e.h.rooms.Play(chatID, trackA, 7); err != nil {
		t.Fatalf("rooms.Play A: %v", err)
	}
	if _, err := e.h.rooms.Play(chatID, trackB, 7); err != nil {
		t.Fatalf("rooms.Play B (queue): %v", err)
	}
	e.h.setFeeding(chatID, trackA.ID)
	// The DSP promoted C, but the room advances to B.
	e.h.onTransition(chatID, trackC.ID)
	snap := waitRoom(t, e.h, chatID, func(s model.NowPlaying) bool {
		return s.Track != nil && s.Track.ID == trackB.ID
	}, "track B (queue top, not the transitioned C)")
	if snap.Track == nil || snap.Track.ID != trackB.ID {
		t.Fatalf("current after mismatch transition = %+v, want track B", snap.Track)
	}
	// The rebind was attempted: with no call record the bind is
	// skipped and the feeding record cleared (best effort).
	if feeding, ok := e.h.getFeeding(chatID); ok {
		t.Fatalf("feeding after mismatch rebind = %q, want cleared (no call to bind to)", feeding)
	}
}
