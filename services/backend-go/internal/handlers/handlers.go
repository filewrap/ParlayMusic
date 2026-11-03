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
//     Advances fire their events right after the room transition
//     (the transition is irreversible — the queue pop is consumed)
//     and then bind; a bind failure there surfaces as VoiceError
//     plus the action error, with the room standing advanced.
//   - Track advance is event-driven only: the voice feed's EOF
//     (Manager.OnFeedEnd) or its gapless slot promotion
//     (Manager.OnTransition) advances the room. There is NO polling
//     ticker and NO watcher. OnFeedEnd/OnTransition never block the
//     pump goroutine: they capture the ended/transitioned track id
//     and dispatch the advance to a worker goroutine (blocking
//     there while a skip waits in feed.Stop() for the pump to
//     return would deadlock).
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
		deps:      d,
		rooms:     d.Room,
		events:    events.New(d.Server),
		lyrics:    d.Lyrics,
		chatLocks: map[int64]*sync.Mutex{},
		feeding:   map[int64]string{},
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
// voice callbacks (feed-end advance, stream-end logging). It is the
// contract-shaped entry point: func Register(s *uds.Server, d
// *app.Deps).
func Register(s *uds.Server, d *app.Deps) {
	h := New(d)
	// Voice callbacks: the feed-end advance and the gapless slot
	// promotion are the ONLY track advance triggers (no polling,
	// no watcher).
	d.Voice.OnFeedEnd(h.onFeedEnd)
	d.Voice.OnTransition(h.onTransition)
	d.Voice.OnStreamEnd(func(chatID int64, streamType voice.StreamType, device voice.StreamDevice) {
		// The ntgcalls stream-end is not an advance trigger (the
		// external source never ends by itself; the feeder's EOF
		// is the boundary signal). Log it; take no playback
		// action, so a stream-end can never double-advance.
		log.Printf("handlers: stream end chat %d type %d device %d", chatID, streamType, device)
	})

	for action, fn := range map[string]uds.Handler{
		"vc.play":          h.vcPlay,
		"vc.pause":         h.vcPause,
		"vc.resume":        h.vcResume,
		"vc.skip":          h.vcSkip,
		"vc.stop":          h.vcStop,
		"vc.seek":          h.vcSeek,
		"vc.queue":         h.vcQueue,
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
// When no call record exists at all the bind is skipped and (false,
// nil) is returned: the room is the source of truth and the audio
// simply is not flowing. When a call record exists, the bind is
// attempted and its error returned. The bool reports whether a
// feed is now running. In both non-running cases the feeding
// record is cleared: Manager.Play stops the old feed before
// starting the new one, so a failed bind leaves no feed running,
// and a stale feeding id would make a later feed-end look like
// the wrong track ended.
func (h *Handlers) bindTrack(ctx context.Context, chatID int64, track model.Track) (bool, error) {
	if _, ok := h.deps.Voice.Get(chatID); !ok {
		h.clearFeeding(chatID)
		return false, nil
	}
	if track.StreamURL == "" {
		h.clearFeeding(chatID)
		return false, fmt.Errorf("track %s has no playable stream URL", track.ID)
	}
	opts := h.dspOptions(chatID)
	if h.prefetch != nil {
		if stream, ok := h.prefetch.Take(chatID, track.ID); ok {
			if err := h.deps.Voice.PlayStream(ctx, chatID, stream.Reader(), opts); err != nil {
				// Prefetched handle failed: fall back to the
				// direct URL rather than failing the track.
				_ = stream.Close()
				log.Printf("handlers: prefetch play chat %d track %s failed (%v); falling back to direct URL", chatID, track.ID, err)
			} else {
				h.setFeeding(chatID, track.ID)
				return true, nil
			}
		}
	}
	if err := h.deps.Voice.Play(ctx, chatID, track.StreamURL, opts); err != nil {
		h.clearFeeding(chatID)
		return false, err
	}
	h.setFeeding(chatID, track.ID)
	return true, nil
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

// advancePlayback runs one room Advance and drives voice + events
// from the outcome. It is the single advance path for vc.skip,
// vc.autoplay_skip's aftermath, and the feed-end callback, so the
// §4.5 events fire exactly once per transition. Events fire right
// after the room transition (the transition is irreversible — the
// queue pop is consumed); the voice bind follows, and a bind
// failure surfaces as VoiceError plus the returned error, with
// the room standing advanced. The returned snapshot is the room
// after the advance.
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
	switch out.Kind {
	case room.AdvanceNoOp:
		// Nothing advanced: no events, no voice action.
	case room.AdvanceReplayed:
		// A loop replay fires TrackStarted only — never
		// TrackAdvanced (§4.5).
		h.events.TrackStarted(chatID, *out.Track)
		if rebind {
			if err := h.bindAfterAdvance(ctx, chatID, *out.Track); err != nil {
				snap, _ := h.rooms.Snapshot(chatID)
				return out, snap, err
			}
		}
	case room.AdvanceNext:
		h.events.TrackAdvanced(chatID, *out.Track, out.PreviousTrackID)
		h.events.TrackStarted(chatID, *out.Track)
		if rebind {
			if err := h.bindAfterAdvance(ctx, chatID, *out.Track); err != nil {
				snap, _ := h.rooms.Snapshot(chatID)
				return out, snap, err
			}
		}
	case room.AdvanceAutoplay:
		h.events.TrackStarted(chatID, *out.Track)
		if rebind {
			if err := h.bindAfterAdvance(ctx, chatID, *out.Track); err != nil {
				snap, _ := h.rooms.Snapshot(chatID)
				return out, snap, err
			}
		}
	case room.AdvanceIdle:
		if out.EndedTrack != nil {
			h.events.TrackEnded(chatID, *out.EndedTrack, out.Reason)
		}
		h.events.QueueEmpty(chatID)
		// The room went idle: leave the call (vc-rust rule —
		// VOICE.stop on Idle). Best effort; the room is idle
		// either way. The feeding record is cleared whether or
		// not a call record exists: an idle room has no feed.
		h.clearFeeding(chatID)
		if _, ok := h.deps.Voice.Get(chatID); ok {
			if err := h.deps.Voice.Stop(chatID); err != nil {
				log.Printf("handlers: leave idle call chat %d: %v", chatID, err)
			}
		}
	}
	snap, _ := h.rooms.Snapshot(chatID)
	// Lookahead: a track just started, so the next one is
	// prefetched in the background (the loop replay when Loop >
	// 0, else N+1 — see maybePrefetch); an idle room drops any
	// pending prefetch.
	switch out.Kind {
	case room.AdvanceNext, room.AdvanceAutoplay, room.AdvanceReplayed:
		h.maybePrefetch(chatID, snap)
	case room.AdvanceIdle:
		h.prefetchCancel(chatID)
	}
	return out, snap, nil
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
