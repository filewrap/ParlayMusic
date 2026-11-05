// Package handlers owns the vc.* UDS actions (phase3/
// actions-inventory.md §1) plus vc.play_file (§9-E12). It is the
// only package that drives both the room state machine and the
// voice Manager, and the only caller of the events emitter — so
// every §4.5 firing point is reached exactly once per transition.
//
// Ordering rules (the ones that keep state, audio, and events
// consistent):
//
//   - The room is the single source of truth. Voice operations are
//     best-effort when NO call record exists at all (the room
//     transition stands); when a call record exists, a failed
//     voice operation is an honest engine_unavailable.
//   - Every user-initiated track start (vc.play, vc.skip,
//     vc.autoplay_skip) ensures the voice call FIRST (join or
//     no-op). A failed join fails the action with the room
//     untouched. Background advances (feed-end) never join.
//   - vc.play binds the feed BEFORE firing TrackStarted, so a
//     failed bind rolls the room back to idle with no event fired.
//     Advances bind BEFORE firing their events too (the
//     transition is irreversible — the queue pop is consumed) and
//     a bind failure advances past the broken track, bounded to
//     three binds per call; when the voice call itself is gone
//     the session ends instead. A track is never reported as
//     playing with no feed behind it.
//   - Track advance has three triggers, all funnelling through
//     the one advancePlayback path with the ended-track id as
//     the dedup key, so a double fire is a no-op (queue-port
//     Tier 1): the voice feed's EOF (Manager.OnFeedEnd), the
//     ntgcalls stream-end for audio streams
//     (Manager.OnStreamEnd), and the 4-second wall-clock
//     watchdog (a track past its duration for three consecutive
//     ticks advances — the rescue when both event sources fail).
//     The DSP gapless slot promotion (Manager.OnTransition)
//     advances without a rebind, because the audio is already
//     flowing. OnFeedEnd/OnTransition/OnStreamEnd/watchdog never
//     block their caller: they capture the ended track id and
//     dispatch the advance to a worker goroutine (blocking
//     there while a skip waits in feed.Stop() for the pump to
//     return would deadlock). The video-chat-ended watcher
//     (Manager.OnConnectionChange, queue-port Tier 2 item 18)
//     clears the whole chat state when a live call ends — the
//     queue does not survive a call ending — but only for a
//     terminal transition of a call that was connected, so it
//     never fights the join logic's own recovery.
//   - One per-chat lock serialises every operation that changes
//     the current track and binds voice for a chat, so a feed-end
//     advance and a user skip can never interleave into a
//     double-advance or a bind of the wrong track.
//
// Frame path (Phase 5): audio reaches ntgcalls through the
// parlay-dsp engine (internal/dsp) — the DSP decodes the stream,
// applies the room's FX/speed/bass/volume, and this package's
// bindTrack hands it the PrefbufferedStream reader or the direct
// stream URL. No ffmpeg subprocess exists anywhere in the path.
package handlers

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"sync"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/events"
	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/prefetch"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// Handlers owns the vc.* actions and the playback orchestration.
type Handlers struct {
	deps     *app.Deps
	rooms    *room.Manager
	events   *events.Emitter
	lyrics   *lyrics.Fetcher
	picker   room.Picker
	prefetch *prefetch.Prefetcher

	mu        sync.Mutex
	chatLocks map[int64]*sync.Mutex

	feedMu  sync.Mutex
	feeding map[int64]string // chatID -> track id currently being fed to voice

	// watchdogMu guards watchdogStreak: chatID -> the last track
	// id the watchdog saw past its duration, and how many
	// consecutive 4-second ticks it has been past it. A track
	// change resets the streak, so a stale entry can never
	// advance a fresh track early.
	watchdogMu     sync.Mutex
	watchdogStreak map[int64]watchdogState

	// stuckMu guards stuck: chatID -> the track id that just
	// failed its bind budget in advancePlaybackMode (queue-port
	// Tier 2, item 14). The room stands on that track with no
	// feed behind it, and the watchdog's feeding check would
	// otherwise never rescue it — the mark tells the watchdog
	// the track is proven broken, so it advances on the next
	// tick without waiting out the track's duration. The mark
	// is cleared when the track advances, when the front track
	// changes, or when the session ends.
	stuckMu sync.Mutex
	stuck   map[int64]string
}

// watchdogState is one chat's watchdog streak.
type watchdogState struct {
	trackID string
	streak  int
}

// New builds the Handlers for deps: it takes the room manager and
// the lyrics fetcher from deps — the single shared instances
// main.go constructed — and installs the autoplay picker on the
// shared manager. It never constructs a second Manager or Fetcher:
// a divergent second owner of room state is exactly what
// app.Deps exists to prevent. Stored rooms are restored (paused).
// A restore failure is logged loudly and the backend starts with
// empty rooms — the store is bbolt, and a corrupt room record must
// not brick startup. A nil d.Lyrics is tolerated (lyrics not
// configured); vc.lyrics answers engine_unavailable in that case.
func New(d *app.Deps) *Handlers {
	h := &Handlers{
		deps:           d,
		rooms:          d.Room,
		events:         events.New(d.Server),
		lyrics:         d.Lyrics,
		chatLocks:      map[int64]*sync.Mutex{},
		feeding:        map[int64]string{},
		watchdogStreak: map[int64]watchdogState{},
		stuck:          map[int64]string{},
	}
	h.picker = newMixPicker(d.Registry)
	d.Room.SetPicker(h.picker)
	// Phase 4 lookahead prefetch: N+1 is pre-resolved and
	// pre-buffered when a track starts. A zero lead time disables
	// it (limits.prefetch_lead_time_sec, 0..3600); the resolve
	// timeout is the configured resolve budget.
	if d.Config != nil && d.Config.Limits.PrefetchLeadTimeSec > 0 {
		h.prefetch = prefetch.New(
			d.Registry.ResolveTrack,
			prefetch.PrefixBytesForLeadTime(d.Config.Limits.PrefetchLeadTimeSec),
			time.Duration(d.Config.Timeouts.ResolveMs)*time.Millisecond,
		)
	}
	if err := d.Room.Restore(); err != nil {
		log.Printf("handlers: room restore failed (%v); starting with empty rooms", err)
	}
	return h
}

// Register wires the vc.* actions onto the server and installs the
// voice callbacks (feed-end advance, stream-end advance, gapless
// promotion) and the queue watchdog. It is the contract-shaped
// entry point: func Register(s *uds.Server, d *app.Deps).
func Register(s *uds.Server, d *app.Deps) {
	h := New(d)
	// Voice callbacks: the feed-end advance, the ntgcalls
	// stream-end advance (audio streams only), and the gapless
	// slot promotion are the event-driven advance triggers, and
	// the watchdog loop is the rescue when both event sources
	// fail (queue-port Tier 1, items 1-2).
	d.Voice.OnFeedEnd(h.onFeedEnd)
	d.Voice.OnTransition(h.onTransition)
	d.Voice.OnStreamEnd(h.onStreamEnd)
	// The video-chat-ended watcher (queue-port Tier 2, item 18):
	// when a live call ends, the whole chat state is cleared.
	d.Voice.OnConnectionChange(h.onConnectionChange)
	go h.watchdogLoop()

	for action, fn := range map[string]uds.Handler{
		"vc.play":          h.vcPlay,
		"vc.pause":         h.vcPause,
		"vc.resume":        h.vcResume,
		"vc.skip":          h.vcSkip,
		"vc.stop":          h.vcStop,
		"vc.seek":          h.vcSeek,
		"vc.queue":         h.vcQueue,
		"vc.play_next":     h.vcPlayNext,
		"vc.loop":          h.vcLoop,
		"vc.shuffle":       h.vcShuffle,
		"vc.autoplay":      h.vcAutoplay,
		"vc.autoplay_skip": h.vcAutoplaySkip,
		"vc.volume":        h.vcVolume,
		"vc.now_playing":   h.vcNowPlaying,
		"vc.progress":      h.vcProgress,
		"vc.panel":         h.vcPanel,
		"vc.lyrics":        h.vcLyrics,
		"vc.join":          h.vcJoin,
		"vc.mute":          h.vcMute,
		"vc.radio":         h.vcRadio,
		"vc.play_file":     h.vcPlayFile,
		"vc.card":          h.vcCard,
	} {
		s.Register(action, fn)
	}
}

// --- helpers ---------------------------------------------------------

// errBadRequest builds a bad_request HandlerError.
func errBadRequest(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf(format, args...))
}

// errNotFound builds a not_found HandlerError.
func errNotFound(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrNotFound, fmt.Sprintf(format, args...))
}

// errEngineUnavailable builds an engine_unavailable HandlerError.
func errEngineUnavailable(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf(format, args...))
}

// errInternal builds an internal HandlerError.
func errInternal(format string, args ...any) error {
	return uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf(format, args...))
}

// mapRoomErr maps a room error to its contract code. Room errors
// already carry the honest message.
func mapRoomErr(err error) error {
	switch {
	case errors.Is(err, room.ErrNothingPlaying):
		return uds.NewHandlerError(uds.ErrNotFound, err.Error())
	case errors.Is(err, room.ErrPositionOutOfRange),
		errors.Is(err, room.ErrQueueFull),
		errors.Is(err, room.ErrInvalid):
		return uds.NewHandlerError(uds.ErrBadRequest, err.Error())
	case errors.Is(err, room.ErrNoPick), errors.Is(err, room.ErrPickerFailed):
		return uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	case errors.Is(err, room.ErrStale):
		return uds.NewHandlerError(uds.ErrEngineUnavailable, err.Error())
	default:
		return err
	}
}

// decodePayload unmarshals the request payload into dst. An empty
// payload decodes as the zero value (actions whose only input is
// the envelope chat_id send no body).
func decodePayload(req uds.Request, dst any) error {
	if len(req.Payload) == 0 {
		return nil
	}
	if err := json.Unmarshal(req.Payload, dst); err != nil {
		return errBadRequest("unparsable payload: %v", err)
	}
	return nil
}

// chatIDOf resolves the chat id: the envelope's promoted chat_id
// wins, else the payload's chat_id. Zero either way is a
// bad_request.
func chatIDOf(req uds.Request, payloadChatID int64) (int64, error) {
	if req.HasChatID && req.ChatID != 0 {
		return req.ChatID, nil
	}
	if payloadChatID != 0 {
		return payloadChatID, nil
	}
	return 0, errBadRequest("chat_id is required")
}

// userIDOf resolves the acting user: the envelope's promoted
// user_id wins, else the payload's requested_by.
func userIDOf(req uds.Request, requestedBy int64) int64 {
	if req.HasUserID {
		return req.UserID
	}
	return requestedBy
}

// nowPlayingResp is the {now_playing} response shape.
func nowPlayingResp(np model.NowPlaying) map[string]any {
	return map[string]any{"now_playing": np}
}

// chatLock returns the per-chat lock for chatID, creating it on
// first use.
func (h *Handlers) chatLock(chatID int64) *sync.Mutex {
	h.mu.Lock()
	defer h.mu.Unlock()
	l, ok := h.chatLocks[chatID]
	if !ok {
		l = &sync.Mutex{}
		h.chatLocks[chatID] = l
	}
	return l
}

// setFeeding records that chatID's feed is playing trackID.
func (h *Handlers) setFeeding(chatID int64, trackID string) {
	h.feedMu.Lock()
	defer h.feedMu.Unlock()
	h.feeding[chatID] = trackID
}

// getFeeding returns the track id being fed in chatID, if any.
func (h *Handlers) getFeeding(chatID int64) (string, bool) {
	h.feedMu.Lock()
	defer h.feedMu.Unlock()
	id, ok := h.feeding[chatID]
	return id, ok
}

// clearFeeding forgets chatID's feed record.
func (h *Handlers) clearFeeding(chatID int64) {
	h.feedMu.Lock()
	defer h.feedMu.Unlock()
	delete(h.feeding, chatID)
}

// --- voice orchestration ---------------------------------------------

// hasLiveCall reports whether chatID has a connected voice call.
// A call record in any other state (joining, failed, left) is not
// a live call: binding a feed to it would fail in startStream,
// so callers treat it as "no call" and end the session instead of
// draining the queue into a dead call.
func (h *Handlers) hasLiveCall(chatID int64) bool {
	call, ok := h.deps.Voice.Get(chatID)
	return ok && call.State == voice.StateConnected
}

// ensureCall joins the voice call for chatID when no connected call
// exists, and is a no-op when one does. A failure returns the exact
// stage error and never reports success.
func (h *Handlers) ensureCall(ctx context.Context, chatID int64) error {
	if call, ok := h.deps.Voice.Get(chatID); ok && call.State == voice.StateConnected {
		return nil
	}
	_, err := h.deps.Voice.Join(ctx, chatID)
	return err
}

// maybePrefetch starts the background prefetch of the next track
// for chatID, from a snapshot taken right after a track started.
// It returns immediately — the prefetch never blocks the playing
// track. What "next" means depends on the loop state:
//
//   - Loop > 0: the next track IS the current track (a loop replay).
//     The current track is prefetched so the replay goes through
//     the DSP's gapless dual-slot path (loadNextWhenReady loads it
//     as the next slot) instead of a rebind gap.
//   - Otherwise: the queue top (N+1), as before.
//
// It is a no-op when prefetch is disabled, there is no next track,
// or the next track is not prefetchable (live, radio, file,
// soulseek — the prefetcher itself skips those; a file/loop replay
// degrades to the normal rebind path). Callers must hold no locks
// the prefetcher needs (it takes none).
func (h *Handlers) maybePrefetch(chatID int64, snap model.NowPlaying) {
	if h.prefetch == nil {
		return
	}
	var next model.Track
	if snap.Loop > 0 && snap.Track != nil {
		// Loop replay: the current track is next.
		next = *snap.Track
	} else {
		if len(snap.Queue) == 0 {
			return
		}
		next = snap.Queue[0].Track
	}
	if next.IsLive {
		return
	}
	switch next.Source {
	case "radio", "file", "soulseek":
		return
	}
	h.prefetch.Prefetch(chatID, next)
	// When the prefetch becomes ready, load it into the running
	// DSP feed's next slot so the transition is gapless. The
	// waiter claims the handle through Take (single ownership
	// with the advance-time Take in bindTrack — exactly one of
	// them wins), so a track that advances before its prefetch
	// is ready still binds normally.
	go h.loadNextWhenReady(chatID, next)
}

// loadNextWhenReady waits for the prefetch of track to become
// ready, then loads it into the running DSP feed's next slot. It
// is best-effort throughout: any failure (prefetch replaced or
// failed, no live feed, next slot already loaded) just leaves the
// advance-time bindTrack path to do its normal rebind.
func (h *Handlers) loadNextWhenReady(chatID int64, track model.Track) {
	stream, ok := h.prefetch.Peek(chatID, track.ID)
	if !ok {
		return
	}
	<-stream.Ready()
	taken, ok := h.prefetch.Take(chatID, track.ID)
	if !ok {
		return
	}
	r := taken.Reader()
	if err := h.deps.Voice.LoadNext(chatID, r, h.rooms.DSPOptions(chatID), track.ID); err != nil {
		// The handle is consumed; close the reader so the body
		// is not leaked. The advance-time Take finds nothing
		// and falls back to the direct URL.
		_ = r.Close()
		log.Printf("handlers: load next slot chat %d track %s failed (%v); gapless transition unavailable, rebind on advance", chatID, track.ID, err)
	}
}

// dspOptions returns the DSP parameters for chatID from the room
// (speed, bass, volume, FX preset + parameters) in a single read.
func (h *Handlers) dspOptions(chatID int64) dsp.Options {
	return h.rooms.DSPOptions(chatID)
}

// prefetchCancel drops the pending prefetch for chatID (stop/idle).
func (h *Handlers) prefetchCancel(chatID int64) {
	if h.prefetch != nil {
		h.prefetch.Cancel(chatID)
	}
}

// bindTrack binds the feed for track in chatID (voice.Manager.Play
// — the DSP path, decoding the direct stream URL — or
// voice.Manager.PlayStream for a ready prefetch handle, decoding
// the PrefbufferedStream reader). When a ready prefetch handle
// exists for the track it is consumed first (the opening seconds
// are already buffered); a missing, failed, or still-in-flight
// prefetch falls back to the direct stream URL, never delaying the
// bind. The room's DSP parameters (speed, bass, volume, FX
// preset) are read once per bind and applied from the first frame.
// When no call record exists at all the bind FAILS LOUDLY: a
// track must never be reported as playing with no feed behind it
// (the ghost — queue-port Tier 1, item 6). Callers advance past
// the track or end the session on this error, never claim
// success. The bool reports whether a feed is now running. In
// both non-running cases the feeding record is cleared:
// Manager.Play stops the old feed before starting the new one, so
// a failed bind leaves no feed running, and a stale feeding id
// would make a later feed-end look like the wrong track ended.
//
// Play-attempt retry (queue-port Tier 2, item 16): one bind
// attempt is retried, in place, for Retryable-class errors only
// (INTERDC_* → 2 s, short FLOOD_WAIT → the parsed wait, other
// transient failures → 2 s), up to voice.MaxAttempts attempts
// total, sleeping ctx-aware — a cancelled context ends the bind
// with the last stage error. A Fatal or Unknown error returns
// at once with no retry, and a call that dies mid-retry ends the
// bind immediately (no sleep into a dead call). The retry is
// inside the bind primitive, so every caller (vc.play's fresh
// start, advances, resume, radio, autoplay-skip) gets it; the
// advance path still classifies the final error (Fatal/Rotate
// end the session, anything else advances past the track).
func (h *Handlers) bindTrack(ctx context.Context, chatID int64, track model.Track) (bool, error) {
	if !h.hasLiveCall(chatID) {
		h.clearFeeding(chatID)
		return false, fmt.Errorf("no active voice call in this chat")
	}
	if track.StreamURL == "" {
		h.clearFeeding(chatID)
		return false, fmt.Errorf("track %s has no playable stream URL", track.ID)
	}
	opts := h.dspOptions(chatID)
	var lastErr error
	for attempt := 1; attempt <= voice.MaxAttempts; attempt++ {
		if attempt > 1 {
			// The call may have died while the last attempt
			// failed (a failed source swap marks it failed):
			// do not sleep into a dead call.
			if !h.hasLiveCall(chatID) {
				h.clearFeeding(chatID)
				return false, lastErr
			}
			if !sleepCtx(ctx, voice.RetryDelay(lastErr)) {
				h.clearFeeding(chatID)
				return false, lastErr
			}
		}
		if err := h.tryBindOnce(ctx, chatID, track, opts); err == nil {
			h.setFeeding(chatID, track.ID)
			return true, nil
		} else {
			lastErr = err
		}
		if voice.Classify(lastErr) != voice.ClassRetryable {
			break
		}
		log.Printf("handlers: bind chat %d track %s attempt %d failed (%v); retrying", chatID, track.ID, attempt, lastErr)
	}
	h.clearFeeding(chatID)
	return false, lastErr
}

// tryBindOnce is one bind attempt: a ready prefetch handle first
// (PlayStream), else the direct stream URL (Play). A failed
// prefetch play falls back to the direct URL inside the same
// attempt rather than failing the track — the prefetch is an
// optimisation, never a gate. It returns nil when a feed is
// running; the feeding record is the caller's job.
func (h *Handlers) tryBindOnce(ctx context.Context, chatID int64, track model.Track, opts dsp.Options) error {
	if h.prefetch != nil {
		if stream, ok := h.prefetch.Take(chatID, track.ID); ok {
			if err := h.deps.Voice.PlayStream(ctx, chatID, stream.Reader(), opts); err != nil {
				// Prefetched handle failed: fall back to the
				// direct URL rather than failing the track.
				_ = stream.Close()
				log.Printf("handlers: prefetch play chat %d track %s failed (%v); falling back to direct URL", chatID, track.ID, err)
			} else {
				return nil
			}
		}
	}
	return h.deps.Voice.Play(ctx, chatID, track.StreamURL, opts)
}

// sleepCtx sleeps d, or returns false early when ctx is done. A
// zero or negative d returns true at once.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		return true
	}
	select {
	case <-ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}

// applyMuteIfNeeded re-applies the room's mute flag to a freshly
// bound feed (a room that was muted before its call dropped must
// stay muted when audio restarts). Best effort: a failure is
// logged, the room flag stands, and the user can toggle mute again.
func (h *Handlers) applyMuteIfNeeded(chatID int64) {
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok || !snap.IsMuted {
		return
	}
	if _, err := h.deps.Voice.Mute(chatID); err != nil {
		log.Printf("handlers: re-apply mute chat %d: %v", chatID, err)
	}
}

// resumeFeed (re)starts audio for the room's current track after
// the room clock was resumed. Callers must hold the per-chat lock
// and have already called rooms.Resume. On any failure the room
// is rolled back to paused and the error is returned, so the room
// never claims to be playing with no audio behind it.
func (h *Handlers) resumeFeed(ctx context.Context, chatID int64) error {
	current, ok := h.rooms.Current(chatID)
	if !ok {
		return nil // nothing to feed; the room is idle
	}
	if feeding, ok := h.getFeeding(chatID); ok && feeding == current.ID {
		// The feed is paused, not stopped: resume it in place.
		// Without a call record the feed cannot resume — fail
		// honestly instead of leaving the room "playing" mute.
		if _, ok := h.deps.Voice.Get(chatID); !ok {
			_, _ = h.rooms.Pause(chatID)
			return errEngineUnavailable("no active voice call in this chat — use vc.join first")
		}
		if _, verr := h.deps.Voice.Resume(chatID); verr != nil {
			_, _ = h.rooms.Pause(chatID) // undo the resume
			return errEngineUnavailable("voice resume failed: %v", verr)
		}
		return nil
	}
	// No feed is running for this track: (re)start it. This needs
	// a live call — without one the resume cannot produce audio.
	if _, ok := h.deps.Voice.Get(chatID); !ok {
		_, _ = h.rooms.Pause(chatID) // undo the resume
		return errEngineUnavailable("no active voice call in this chat — use vc.join first")
	}
	if _, berr := h.bindTrack(ctx, chatID, current); berr != nil {
		_, _ = h.rooms.Pause(chatID) // undo the resume
		h.events.VoiceError(chatID, "stream_error", berr.Error())
		return errEngineUnavailable("voice bind failed: %v", berr)
	}
	h.applyMuteIfNeeded(chatID)
	return nil
}

// maxBindAttempts is the bind budget per advance call: a track
// that fails to bind is popped and the next one tried, at most
// this many binds (Parvati's 3-attempt rule, queue-port Tier 1
// item 16). The budget is checked when a bind FAILS, not at the
// top of the loop, so the room never parks on a track that was
// never tried: when the budget is spent the room stands on the
// track whose bind just failed, marked stuck for the watchdog
// (item 14), which advances past it on its next tick — the queue
// drains one broken track per watchdog tick instead of wedging.
const maxBindAttempts = 3

// advancePlayback runs one room Advance and drives voice + events
// from the outcome. It is the single advance path for vc.skip,
// vc.autoplay_skip's aftermath, the feed-end callback, the
// stream-end callback, and the watchdog, so the §4.5 events fire
// exactly once per transition. Events fire only after the voice
// bind succeeds — a track that never got a feed is never
// announced as playing. A bind failure advances past the broken
// track (bounded by maxBindAttempts); when the voice call itself
// is gone the session ends instead (queue-port Tier 1, item 5:
// a failure always moves the queue or ends the session). A
// Fatal-class bind error (GROUPCALL_INVALID, RTMP, call gone)
// ends the session at once, and a Rotate-class one evicts the
// assistant first (queue-port Tier 2, item 16) — neither drains
// the queue through a call that cannot play. The returned
// snapshot is the room after the advance.
func (h *Handlers) advancePlayback(ctx context.Context, chatID int64, endedID string, fromSkip bool) (room.AdvanceOutcome, model.NowPlaying, error) {
	return h.advancePlaybackMode(ctx, chatID, endedID, fromSkip, true)
}

// advancePlaybackMode is advancePlayback with the rebind switch.
// rebind=true is the normal path: the advanced-to track is bound
// with a fresh feed. rebind=false is the gapless-transition path
// (advanceFromTransition): the DSP already promoted the next slot
// and the audio is flowing, so no bind happens — the caller
// verifies the room advanced to the track the DSP is playing and
// rebinds only on a mismatch. The Idle branch is identical in
// both modes: the room went idle and the call is left.
func (h *Handlers) advancePlaybackMode(ctx context.Context, chatID int64, endedID string, fromSkip bool, rebind bool) (room.AdvanceOutcome, model.NowPlaying, error) {
	out, err := h.rooms.Advance(ctx, chatID, endedID, fromSkip)
	if err != nil {
		return out, model.NowPlaying{}, mapRoomErr(err)
	}
	binds := 0
	for {
		switch out.Kind {
		case room.AdvanceNoOp:
			// Nothing advanced: no events, no voice action.
			snap, _ := h.rooms.Snapshot(chatID)
			return out, snap, nil
		case room.AdvanceReplayed, room.AdvanceNext, room.AdvanceAutoplay:
			if !rebind {
				// Gapless path: the DSP is already playing
				// the promoted track; events only.
				h.fireAdvanceEvents(chatID, out)
				snap, _ := h.rooms.Snapshot(chatID)
				h.maybePrefetch(chatID, snap)
				return out, snap, nil
			}
			binds++
			if berr := h.bindAfterAdvance(ctx, chatID, *out.Track); berr != nil {
				// Classified end-of-the-line first: a
				// Fatal bind error (GROUPCALL_INVALID,
				// RTMP, the call itself gone) and a
				// Rotate one (the assistant is dead or
				// flooded) end the session instead of
				// draining the queue through a call that
				// cannot play (item 16). bindTrack
				// already retried the Retryable classes
				// in place, so a Retryable error here
				// means its retries are spent — it
				// advances past the track like Unknown.
				switch voice.Classify(berr) {
				case voice.ClassFatal:
					h.endSession(chatID, out)
					snap, _ := h.rooms.Snapshot(chatID)
					return out, snap, berr
				case voice.ClassRotate:
					evicted := h.deps.Voice.EvictCurrentAssistant(berr.Error())
					log.Printf("handlers: advance chat %d bind failed with rotate-class error (%v); evicted assistant %q, ending session", chatID, berr, evicted)
					h.endSession(chatID, out)
					snap, _ := h.rooms.Snapshot(chatID)
					return out, snap, berr
				}
				if binds >= maxBindAttempts {
					// Budget spent: park on this track
					// — it WAS tried (the budget is
					// checked on failure, never before
					// a first attempt) — and mark it
					// stuck so the watchdog rescues it
					// on its next tick (item 14).
					h.markStuck(chatID, out.Track.ID)
					h.events.VoiceError(chatID, "stream_error", "voice bind failed repeatedly; track left standing for the watchdog")
					snap, _ := h.rooms.Snapshot(chatID)
					return out, snap, errEngineUnavailable("voice bind failed")
				}
				if !h.hasLiveCall(chatID) {
					// The call is gone: end the session
					// (item 5) instead of draining the
					// queue into a dead call.
					h.endSession(chatID, out)
					snap, _ := h.rooms.Snapshot(chatID)
					return out, snap, berr
				}
				// Advance past the broken track and try the
				// next one. The pop is consumed; the broken
				// track never fired TrackStarted.
				log.Printf("handlers: advance chat %d track %s failed to bind (%v); advancing past it", chatID, out.Track.ID, berr)
				out, err = h.rooms.Advance(ctx, chatID, out.Track.ID, false)
				if err != nil {
					return out, model.NowPlaying{}, mapRoomErr(err)
				}
				continue
			}
			h.fireAdvanceEvents(chatID, out)
			snap, _ := h.rooms.Snapshot(chatID)
			h.maybePrefetch(chatID, snap)
			return out, snap, nil
		case room.AdvanceIdle:
			h.fireIdleEvents(chatID, out)
			snap, _ := h.rooms.Snapshot(chatID)
			h.prefetchCancel(chatID)
			return out, snap, nil
		}
	}
}

// fireAdvanceEvents fires the §4.5 events for a track that just
// started playing. A loop replay fires TrackStarted only — never
// TrackAdvanced.
func (h *Handlers) fireAdvanceEvents(chatID int64, out room.AdvanceOutcome) {
	switch out.Kind {
	case room.AdvanceReplayed:
		h.events.TrackStarted(chatID, *out.Track)
	case room.AdvanceNext:
		h.events.TrackAdvanced(chatID, *out.Track, out.PreviousTrackID)
		h.events.TrackStarted(chatID, *out.Track)
	case room.AdvanceAutoplay:
		h.events.TrackStarted(chatID, *out.Track)
	}
}

// fireIdleEvents fires the §4.5 events for a room that went idle
// and leaves the voice call (vc-rust rule — VOICE.stop on Idle).
// Best effort; the room is idle either way. The feeding record is
// cleared whether or not a call record exists: an idle room has
// no feed.
func (h *Handlers) fireIdleEvents(chatID int64, out room.AdvanceOutcome) {
	if out.EndedTrack != nil {
		h.events.TrackEnded(chatID, *out.EndedTrack, out.Reason)
	}
	h.events.QueueEmpty(chatID)
	h.clearFeeding(chatID)
	if _, ok := h.deps.Voice.Get(chatID); ok {
		if err := h.deps.Voice.Stop(chatID); err != nil {
			log.Printf("handlers: leave idle call chat %d: %v", chatID, err)
		}
	}
}

// endSession ends the playback session for a chat whose voice call
// is gone: the room is stopped outright (queue included), the
// feeding record is cleared, any pending prefetch is dropped, and
// the call is left best-effort. A failure always moves the queue
// or ends the session — it never leaves a track standing with no
// call behind it (queue-port Tier 1, item 5).
func (h *Handlers) endSession(chatID int64, out room.AdvanceOutcome) {
	if out.EndedTrack != nil {
		h.events.TrackEnded(chatID, *out.EndedTrack, "call_ended")
	}
	h.events.QueueEmpty(chatID)
	h.clearFeeding(chatID)
	h.clearStuck(chatID)
	h.prefetchCancel(chatID)
	if _, err := h.rooms.Stop(chatID); err != nil {
		log.Printf("handlers: end session chat %d room stop: %v", chatID, err)
	}
	if _, ok := h.deps.Voice.Get(chatID); ok {
		if err := h.deps.Voice.Stop(chatID); err != nil {
			log.Printf("handlers: end session chat %d leave call: %v", chatID, err)
		}
	}
}

// bindAfterAdvance binds the feed for a track an advance just
// started, then re-applies the mute flag. A bind failure emits
// VoiceError and returns engine_unavailable.
func (h *Handlers) bindAfterAdvance(ctx context.Context, chatID int64, track model.Track) error {
	_, err := h.bindTrack(ctx, chatID, track)
	if err != nil {
		h.events.VoiceError(chatID, "stream_error", err.Error())
		return errEngineUnavailable("%v", err)
	}
	h.applyMuteIfNeeded(chatID)
	return nil
}

// onFeedEnd is the voice feeder's end-of-track callback
// (Manager.OnFeedEnd). It NEVER blocks the pump goroutine: it
// captures the ended track id synchronously and dispatches the
// advance to a worker goroutine. A feed error emits VoiceError and
// still advances (a dead stream must not wedge the queue); a
// clean EOF advances silently.
func (h *Handlers) onFeedEnd(chatID int64, feedErr error) {
	endedID, ok := h.getFeeding(chatID)
	if !ok {
		return // no feed was playing; spurious end
	}
	go h.advanceFromFeedEnd(chatID, endedID, feedErr)
}

func (h *Handlers) advanceFromFeedEnd(chatID int64, endedID string, feedErr error) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	if feedErr != nil {
		h.events.VoiceError(chatID, "stream_error", feedErr.Error())
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	// Errors are already surfaced via VoiceError; the room
	// transition (or NoOp, when the user already skipped or
	// stopped) is the outcome.
	_, _, _ = h.advancePlayback(ctx, chatID, endedID, false)
}

// advanceFromTrigger runs one background advance for a trigger
// that already captured its ended track id (stream-end,
// watchdog). The per-chat lock serialises it against skips and
// other triggers; the ended-id dedup in room.Advance makes a
// stale or duplicate fire a no-op.
func (h *Handlers) advanceFromTrigger(chatID int64, endedID string) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	_, _, _ = h.advancePlayback(ctx, chatID, endedID, false)
}

// onStreamEnd is the ntgcalls stream-end callback
// (Manager.OnStreamEnd) — a real advance trigger (queue-port
// Tier 1, item 1). Video-stream ends never advance the audio
// queue (item 9); an audio-stream end advances the room exactly
// like a feed EOF, with the ended-track id as the dedup key, so a
// stream-end that races a feed-end (or a skip) is a no-op. It
// NEVER blocks the ntgcalls callback thread.
func (h *Handlers) onStreamEnd(chatID int64, streamType voice.StreamType, device voice.StreamDevice) {
	if streamType != voice.StreamTypeAudio {
		return
	}
	endedID, ok := h.getFeeding(chatID)
	if !ok {
		return // no feed was playing; spurious end
	}
	go h.advanceFromTrigger(chatID, endedID)
}

// onConnectionChange is the video-chat-ended watcher
// (Manager.OnConnectionChange, queue-port Tier 2 item 18): when
// the voice chat ends, the whole chat state is cleared — the
// queue does not survive a call ending (TgMusicBot's watcher;
// NoxBeats' on_chat_closed → stop). It NEVER blocks the ntgcalls
// callback thread: it checks the call record synchronously and
// dispatches the teardown to a worker goroutine.
//
// It acts ONLY on a terminal transition of a call that WAS
// connected. The Manager marks exactly those
// FailedStage "connection" (dispatchConnectionChange only marks
// a record that was StateConnected); a terminal event from a
// join attempt lands as stage "connect"/"join"/"create_call",
// and a stale event for a record a fresh join already replaced
// shows a live state — both are ignored, so the watcher never
// fights the join logic's own recovery (a failed join that
// Manager.Join is about to retry must not wipe the room).
// Transient/connecting states and presentation-kind changes are
// ignored: a presentation ending is not the call ending.
func (h *Handlers) onConnectionChange(chatID int64, state voice.ConnectionState, kind voice.ConnectionKind) {
	if kind != voice.ConnectionKindNormal {
		return
	}
	switch state {
	case voice.ConnectionFailed, voice.ConnectionTimeout, voice.ConnectionClosed:
	default:
		return
	}
	call, ok := h.deps.Voice.Get(chatID)
	if !ok || call.State != voice.StateFailed || call.FailedStage != "connection" {
		return // join noise, a stale event, or already rejoined
	}
	go h.endSessionFromCallEnd(chatID)
}

// endSessionFromCallEnd clears the whole chat state after the
// voice call ended: room stopped (queue included), feeding
// record cleared, pending prefetch dropped, call left
// best-effort — the endSession path, with the current track as
// the ended track. The call record is re-checked under the
// per-chat lock: a fresh join may have landed between the event
// and this worker, and then there is nothing to clear.
func (h *Handlers) endSessionFromCallEnd(chatID int64) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	call, ok := h.deps.Voice.Get(chatID)
	if !ok || call.State != voice.StateFailed || call.FailedStage != "connection" {
		return
	}
	var ended *model.Track
	if current, ok := h.rooms.Current(chatID); ok {
		t := current
		ended = &t
	}
	log.Printf("handlers: voice call ended chat %d; clearing room state", chatID)
	h.endSession(chatID, room.AdvanceOutcome{EndedTrack: ended})
}

// watchdogInterval is the queue watchdog tick (Parvati's 4 s
// monitor, queue-port Tier 1 item 2). watchdogEndStreak is how
// many consecutive ticks a track must be past its duration
// before the watchdog advances (12 s of overshoot).
const (
	watchdogInterval  = 4 * time.Second
	watchdogEndStreak = 3
)

// watchdogLoop runs the queue watchdog for the backend's
// lifetime: every tick it checks every room with an active track
// whose wall-clock position has passed the track's duration.
func (h *Handlers) watchdogLoop() {
	t := time.NewTicker(watchdogInterval)
	defer t.Stop()
	for range t.C {
		h.watchdogTick()
	}
}

// watchdogTick advances rooms whose active track is past its
// duration for watchdogEndStreak consecutive ticks. The position
// is the room's wall-clock clock (never polled from the voice
// engine); the feed record must exist for the track (a track with
// no feed — a replied file, an unreachable radio — is not the
// watchdog's job). Paused rooms and duration-less tracks never
// advance. EXCEPTION (queue-port Tier 2, item 14): a track
// marked stuck — it already failed its bind budget, so there is
// no feed by definition and no point waiting out its duration —
// advances on the next tick, with no streak and no duration
// check. The binds already proved it broken.
func (h *Handlers) watchdogTick() {
	for _, snap := range h.rooms.PlayingSnapshots() {
		trackID := ""
		if snap.Track != nil {
			trackID = snap.Track.ID
		}
		// A stale stuck mark (the front track changed since
		// the mark was set) is dropped before anything
		// consults it, so it can never advance a fresh track.
		h.clearStuckIfNot(snap.ChatID, trackID)
		if snap.Track == nil || snap.IsPaused {
			h.watchdogReset(snap.ChatID)
			continue
		}
		if h.isStuck(snap.ChatID, snap.Track.ID) {
			h.clearStuck(snap.ChatID)
			log.Printf("handlers: watchdog chat %d track %s stuck after failed binds; advancing", snap.ChatID, snap.Track.ID)
			go h.advanceFromTrigger(snap.ChatID, snap.Track.ID)
			continue
		}
		if snap.Track.DurationSec <= 0 {
			h.watchdogReset(snap.ChatID)
			continue
		}
		feeding, ok := h.getFeeding(snap.ChatID)
		if !ok || feeding != snap.Track.ID {
			h.watchdogReset(snap.ChatID)
			continue
		}
		if snap.PositionSec < snap.Track.DurationSec {
			h.watchdogReset(snap.ChatID)
			continue
		}
		h.watchdogMu.Lock()
		st := h.watchdogStreak[snap.ChatID]
		if st.trackID != snap.Track.ID {
			st = watchdogState{trackID: snap.Track.ID}
		}
		st.streak++
		h.watchdogStreak[snap.ChatID] = st
		streak := st.streak
		h.watchdogMu.Unlock()
		if streak >= watchdogEndStreak {
			h.watchdogReset(snap.ChatID)
			log.Printf("handlers: watchdog chat %d track %s past duration for %d ticks; advancing", snap.ChatID, snap.Track.ID, streak)
			go h.advanceFromTrigger(snap.ChatID, snap.Track.ID)
		}
	}
}

// watchdogReset clears a chat's watchdog streak.
func (h *Handlers) watchdogReset(chatID int64) {
	h.watchdogMu.Lock()
	defer h.watchdogMu.Unlock()
	delete(h.watchdogStreak, chatID)
}

// markStuck records that chatID's front track (trackID) failed its
// bind budget in advancePlaybackMode and the room stands on it
// with no feed. The watchdog rescues it on its next tick
// (item 14). The mark is deliberately NOT cleared by
// watchdogReset: the streak resets whenever the track is not yet
// past its duration, but the stuck mark must survive until the
// track advances or changes.
func (h *Handlers) markStuck(chatID int64, trackID string) {
	h.stuckMu.Lock()
	defer h.stuckMu.Unlock()
	h.stuck[chatID] = trackID
}

// isStuck reports whether chatID is marked stuck on trackID.
func (h *Handlers) isStuck(chatID int64, trackID string) bool {
	h.stuckMu.Lock()
	defer h.stuckMu.Unlock()
	return h.stuck[chatID] == trackID
}

// clearStuck drops chatID's stuck mark, if any.
func (h *Handlers) clearStuck(chatID int64) {
	h.stuckMu.Lock()
	defer h.stuckMu.Unlock()
	delete(h.stuck, chatID)
}

// clearStuckIfNot drops chatID's stuck mark when it names a
// different track than trackID — the front track changed, so the
// mark is stale and must never advance the fresh track.
func (h *Handlers) clearStuckIfNot(chatID int64, trackID string) {
	h.stuckMu.Lock()
	defer h.stuckMu.Unlock()
	if id, ok := h.stuck[chatID]; ok && id != trackID {
		delete(h.stuck, chatID)
	}
}

// onTransition is the DSP gapless-promotion callback
// (Manager.OnTransition). It NEVER blocks the pump goroutine: it
// captures the transitioned track id synchronously and dispatches
// the advance to a worker goroutine, exactly like onFeedEnd.
func (h *Handlers) onTransition(chatID int64, trackID string) {
	go h.advanceFromTransition(chatID, trackID)
}

// advanceFromTransition advances the room after the DSP promoted
// its next slot — WITHOUT rebinding, because the audio is already
// flowing. The room must advance to the track the DSP is playing;
// when it does not (the queue was mutated between the LoadNext and
// the promotion, so Advance popped a different track — or the
// room went idle), the transition is repaired with a normal
// rebind (or the idle path, which leaves the call). The feeding
// record is updated to the track now playing so the next feed-end
// matches it.
func (h *Handlers) advanceFromTransition(chatID int64, trackID string) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	endedID, ok := h.getFeeding(chatID)
	if !ok {
		return // no feed was playing; spurious transition
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	out, _, _ := h.advancePlaybackMode(ctx, chatID, endedID, false, false)
	switch out.Kind {
	case room.AdvanceNoOp:
		// The user already skipped or stopped; the skip's bind
		// already replaced the feed. Nothing to do.
		return
	case room.AdvanceIdle:
		// advancePlaybackMode's Idle branch already left the
		// call (the promoted audio is stopped with it).
		return
	}
	if out.Track == nil {
		return
	}
	if out.Track.ID != trackID {
		// The room advanced to a different track than the DSP
		// is playing: rebind to the room's track (a gap, but
		// correct — the alternative is playing the wrong track
		// under the right name).
		log.Printf("handlers: transition chat %d advanced to %s but DSP plays %s; rebinding", chatID, out.Track.ID, trackID)
		if err := h.bindAfterAdvance(ctx, chatID, *out.Track); err != nil {
			log.Printf("handlers: transition rebind chat %d failed: %v", chatID, err)
		}
		return
	}
	h.setFeeding(chatID, out.Track.ID)
}
