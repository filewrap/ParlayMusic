// vc.* action handlers (phase3/actions-inventory.md §1).
//
// Shared rules (§0): validate chat_id != 0 first (bad_request);
// unknown/invalid enum -> bad_request; nothing playing ->
// not_found "nothing playing in this chat" (except vc.stop, which
// is always 200, and vc.queue list, which answers []); resolve
// failure -> engine_unavailable with the resolver's message, never
// a placeholder track; never invent durations, URLs, lyrics, or
// verdicts.
//
// Error-code map: bad_request 400, unauthorized 401, not_found
// 404, engine_unavailable 502/503, download_failed 502,
// rate_limited 429, internal 500. Unknown actions are answered
// not_found by the UDS server, not here.
package handlers

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parlay/backend-go/internal/lyrics"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/thumbs"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// --- vc.play (§1.1) --------------------------------------------------

// vcPlay resolves the query and plays now when the room is idle,
// else enqueues it (YukkiMusic rule). The voice call is ensured
// BEFORE the room is touched: a failed join fails the action with
// the room untouched. A resolved track with no playable stream URL
// (Spotify metadata-only) is an honest engine_unavailable before
// any room mutation. The feed is bound before TrackStarted fires,
// so a failed bind rolls the room back to idle with no event.
func (h *Handlers) vcPlay(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID       int64    `json:"chat_id"`
		Query        string   `json:"query"`
		RequestedBy  int64    `json:"requested_by"`
		Volume       *int     `json:"volume"`
		Speed        *float64 `json:"speed"`
		Bass         *int     `json:"bass"`
		PlayStatusID int64    `json:"play_status_id"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(p.Query) == "" {
		return nil, errBadRequest("query is required")
	}
	if p.Volume != nil && (*p.Volume < 0 || *p.Volume > 200) {
		// vc-go accepts an optional volume in the play body; the
		// room clamps it (vc-go rule, §10-D15), so out-of-range
		// is clamped, not rejected.
	}
	if p.Speed != nil && (*p.Speed < 0.5 || *p.Speed > 4.0) {
		return nil, errBadRequest("speed must be in 0.5..4.0")
	}
	if p.Bass != nil && (*p.Bass < 0 || *p.Bass > 100) {
		return nil, errBadRequest("bass must be in 0..100")
	}
	by := userIDOf(req, p.RequestedBy)

	track, err := h.deps.Registry.Resolve(ctx, p.Query)
	if err != nil {
		return nil, errEngineUnavailable("resolve failed: %v", err)
	}
	if track.StreamURL == "" && track.Source != string(resolver.SourceRadio) && track.Source != "file" {
		return nil, errEngineUnavailable("track %s resolved but has no playable stream URL", track.ID)
	}
	// The core already sent this play's status message ("Searching…"):
	// report each step as it completes so the message can be edited
	// live (resolved -> thumbnail -> joining -> the card the core
	// renders from this response). Only when the core asked for it
	// (play_status_id); silent/sync plays get no progress events.
	if p.PlayStatusID != 0 {
		h.events.PlayProgress(chatID, p.PlayStatusID, "resolved", map[string]any{
			"title": track.Title, "artist": track.Artist,
			"duration_str": track.DurationStr, "source": track.Source,
		})
	}
	// Cache the thumbnail to disk now, so the core can send it as
	// inputFileLocal: an https URL is not a valid TDLib photo. A
	// failed download is not fatal — the card falls back to text.
	h.ensureThumb(ctx, &track)
	if p.PlayStatusID != 0 {
		h.events.PlayProgress(chatID, p.PlayStatusID, "thumbnail", map[string]any{
			"has_thumbnail": track.ThumbnailPath != "",
		})
	}

	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()

	if p.PlayStatusID != 0 {
		h.events.PlayProgress(chatID, p.PlayStatusID, "joining", nil)
	}
	if err := h.ensureCall(ctx, chatID); err != nil {
		h.events.VoiceError(chatID, "join_failed", err.Error())
		return nil, errEngineUnavailable("voice join failed: %v", err)
	}

	out, err := h.rooms.Play(chatID, track, by)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	// Optional play-body room fields (§4.4: the core's speed/bass
	// chat fields ride into the room on vc.play).
	if p.Volume != nil {
		if _, verr := h.rooms.SetVolume(chatID, *p.Volume); verr != nil {
			log.Printf("handlers: vc.play apply volume chat %d: %v", chatID, verr)
		}
	}
	if p.Speed != nil {
		if _, serr := h.rooms.SetSpeed(chatID, *p.Speed); serr != nil {
			log.Printf("handlers: vc.play apply speed chat %d: %v", chatID, serr)
		}
	}
	if p.Bass != nil {
		if _, berr := h.rooms.SetBass(chatID, *p.Bass); berr != nil {
			log.Printf("handlers: vc.play apply bass chat %d: %v", chatID, berr)
		}
	}
	snap, _ := h.rooms.Snapshot(chatID)
	if !out.Started {
		return map[string]any{
			"now_playing": snap, "queued": true,
			"resolved_by": track.Source,
		}, nil
	}
	if _, berr := h.bindTrack(ctx, chatID, track); berr != nil {
		// Roll the fresh start back to idle: no TrackStarted
		// fired, the queue is intact, the error is honest.
		_, _ = h.rooms.ClearCurrent(chatID)
		h.events.VoiceError(chatID, "stream_error", berr.Error())
		return nil, errEngineUnavailable("voice bind failed: %v", berr)
	}
	h.applyMuteIfNeeded(chatID)
	h.events.TrackStarted(chatID, track)
	snap, _ = h.rooms.Snapshot(chatID)
	h.maybePrefetch(chatID, snap)
	return map[string]any{
		"now_playing": snap, "queued": false,
		"resolved_by": track.Source,
	}, nil
}

// --- vc.pause / vc.resume (§1.2, §1.3) -------------------------------

// vcPause freezes the position clock. Idempotent when already
// paused; idle is not_found. The voice pause is skipped when no
// call record exists; when a record exists and the pause fails,
// the room pause is undone and the error is reported.
func (h *Handlers) vcPause(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, err := h.rooms.Pause(chatID)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if _, ok := h.deps.Voice.Get(chatID); ok {
		if _, verr := h.deps.Voice.Pause(chatID); verr != nil {
			_, _ = h.rooms.Resume(chatID) // undo the room pause
			return nil, errEngineUnavailable("voice pause failed: %v", verr)
		}
	}
	snap, _ = h.rooms.Snapshot(chatID)
	return nowPlayingResp(snap), nil
}

// vcResume restarts the clock from the frozen offset. No-op when
// not paused; idle is not_found. When the room has a current track
// but no feed is running for it (e.g. the backend restarted and
// the room came back paused), the feed is restarted — which
// requires a live call: with no call record the resume fails
// honestly ("use vc.join first") and the room stays paused.
func (h *Handlers) vcResume(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, err := h.rooms.Resume(chatID)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if _, ok := h.rooms.Current(chatID); !ok {
		return nowPlayingResp(snap), nil
	}
	if err := h.resumeFeed(ctx, chatID); err != nil {
		return nil, err
	}
	snap, _ = h.rooms.Snapshot(chatID)
	return nowPlayingResp(snap), nil
}

// --- vc.skip / vc.stop / vc.seek (§1.4–§1.6) -------------------------

// vcSkip resets the loop counter and advances (queue pop ->
// autoplay pick -> idle). The call is ensured first, so an advance
// that starts a track always has audio or an honest error. A
// fully empty room is not_found.
func (h *Handlers) vcSkip(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok || (snap.Track == nil && len(snap.Queue) == 0) {
		return nil, errNotFound("nothing playing in this chat")
	}
	if err := h.ensureCall(ctx, chatID); err != nil {
		h.events.VoiceError(chatID, "join_failed", err.Error())
		return nil, errEngineUnavailable("voice join failed: %v", err)
	}
	_, snap, err = h.advancePlayback(ctx, chatID, "", true)
	if err != nil {
		return nil, err
	}
	return nowPlayingResp(snap), nil
}

// vcStop clears the current track and the whole queue; loop and
// autoplay reset, shuffle is kept. Always 200, even with no room
// (idempotent stop). The voice call is left on a best-effort
// basis; a teardown failure is logged, not reported — the room is
// the source of truth and the stop succeeded state-wise.
func (h *Handlers) vcStop(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	if _, err := h.rooms.Stop(chatID); err != nil {
		return nil, mapRoomErr(err)
	}
	h.clearFeeding(chatID)
	h.prefetchCancel(chatID)
	if _, ok := h.deps.Voice.Get(chatID); ok {
		if verr := h.deps.Voice.Stop(chatID); verr != nil {
			log.Printf("handlers: vc.stop leave call chat %d: %v", chatID, verr)
		}
	}
	return map[string]any{}, nil
}

// vcSeek moves the position clock (clamped to [0, duration] when
// known; negative is bad_request; idle is not_found). A seek to 0
// (replay) rebinds the feed from the start when a call exists.
// A seek to a non-zero position moves the room clock only: the DSP
// feed has no seek-to-offset, so the audio continues from its
// current decode point while the room clock — the source of truth
// for now_playing/progress — reports the requested position. True
// audio-position seek needs a voice.Manager seek (DSP stream
// re-open at an offset), which lives outside this package.
func (h *Handlers) vcSeek(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID      int64  `json:"chat_id"`
		PositionSec *int64 `json:"position_sec"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.PositionSec == nil {
		return nil, errBadRequest("position_sec is required")
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	before, hasCurrent := h.rooms.Current(chatID)
	snap, err := h.rooms.SeekTo(chatID, *p.PositionSec)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if *p.PositionSec == 0 && hasCurrent {
		// Replay: rebind the feed from the start.
		if _, ok := h.deps.Voice.Get(chatID); ok {
			if _, berr := h.bindTrack(ctx, chatID, before); berr != nil {
				_, _ = h.rooms.SeekTo(chatID, 0) // clock already 0; keep the seek
				h.events.VoiceError(chatID, "stream_error", berr.Error())
				return nil, errEngineUnavailable("voice rebind failed: %v", berr)
			}
			h.applyMuteIfNeeded(chatID)
		}
	}
	snap, _ = h.rooms.Snapshot(chatID)
	return nowPlayingResp(snap), nil
}

// --- vc.queue (§1.7) -------------------------------------------------

// vcQueue implements add/remove/clear/list with locked 0-based
// waiting-only positions. Add does NOT start playback even when
// idle; it accepts a Track or a query (resolved, vc-ts rule).
func (h *Handlers) vcQueue(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		Action      string       `json:"action"`
		ChatID      int64        `json:"chat_id"`
		Position    *int         `json:"position"`
		Track       *model.Track `json:"track"`
		Query       string       `json:"query"`
		RequestedBy int64        `json:"requested_by"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	by := userIDOf(req, p.RequestedBy)
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()

	switch p.Action {
	case "list":
		queue, _ := h.rooms.Queue(chatID)
		if queue == nil {
			queue = []model.QueueItem{}
		}
		return map[string]any{"queue": queue}, nil
	case "add":
		var track model.Track
		switch {
		case p.Track != nil:
			track = *p.Track
			if strings.TrimSpace(track.ID) == "" || strings.TrimSpace(track.Title) == "" {
				return nil, errBadRequest("track id and title are required")
			}
			if track.Source == "" {
				track.Source = "youtube" // vc-go rule
			}
		case strings.TrimSpace(p.Query) != "":
			resolved, rerr := h.deps.Registry.Resolve(ctx, p.Query)
			if rerr != nil {
				return nil, errEngineUnavailable("resolve failed: %v", rerr)
			}
			track = resolved
		default:
			return nil, errBadRequest("track or query is required")
		}
		if err := h.rooms.Enqueue(chatID, track, by); err != nil {
			return nil, mapRoomErr(err)
		}
		queue, _ := h.rooms.Queue(chatID)
		return map[string]any{"queue": queue}, nil
	case "remove":
		if p.Position == nil {
			return nil, errBadRequest("position is required")
		}
		queue, err := h.rooms.Remove(chatID, *p.Position)
		if err != nil {
			return nil, mapRoomErr(err)
		}
		return map[string]any{"queue": queue}, nil
	case "clear":
		queue, err := h.rooms.ClearQueue(chatID)
		if err != nil {
			return nil, mapRoomErr(err)
		}
		return map[string]any{"queue": queue}, nil
	default:
		return nil, errBadRequest("unknown queue action %q (want add|remove|clear|list)", p.Action)
	}
}

// --- vc.loop / vc.shuffle / vc.autoplay / vc.autoplay_skip / vc.volume ---

// vcLoop sets the remaining replays of the current track (0-10).
func (h *Handlers) vcLoop(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID int64 `json:"chat_id"`
		Count  *int  `json:"count"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Count == nil {
		return nil, errBadRequest("count is required")
	}
	if *p.Count < 0 || *p.Count > 10 {
		return nil, errBadRequest("count must be in 0..10")
	}
	snap, err := h.rooms.SetLoop(chatID, *p.Count)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	return nowPlayingResp(snap), nil
}

// vcShuffle enables/disables shuffle (enabling shuffles the waiting
// queue in place, vc-ts/vc-rust rule).
func (h *Handlers) vcShuffle(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID  int64 `json:"chat_id"`
		Enabled *bool `json:"enabled"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Enabled == nil {
		return nil, errBadRequest("enabled is required")
	}
	snap, err := h.rooms.SetShuffle(chatID, *p.Enabled)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	return nowPlayingResp(snap), nil
}

// vcAutoplay sets the autoplay flag (disabling clears the history).
func (h *Handlers) vcAutoplay(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID  int64 `json:"chat_id"`
		Enabled *bool `json:"enabled"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Enabled == nil {
		return nil, errBadRequest("enabled is required")
	}
	snap, err := h.rooms.SetAutoplay(chatID, *p.Enabled)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	return nowPlayingResp(snap), nil
}

// vcAutoplaySkip is the gated pick (§1.11): autoplay must be on
// (else bad_request), a track must be playing (else not_found),
// and the pick starts immediately with the loop reset. A failed
// or empty pick is engine_unavailable; the current track is left
// untouched in that case.
func (h *Handlers) vcAutoplaySkip(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok || snap.Track == nil {
		return nil, errNotFound("nothing playing in this chat")
	}
	if !snap.Autoplay {
		return nil, errBadRequest("autoplay is off in this chat")
	}
	if err := h.ensureCall(ctx, chatID); err != nil {
		h.events.VoiceError(chatID, "join_failed", err.Error())
		return nil, errEngineUnavailable("voice join failed: %v", err)
	}
	pick, _, err := h.rooms.AutoplaySkip(ctx, chatID)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if _, berr := h.bindTrack(ctx, chatID, pick); berr != nil {
		// The pick stands as current (the room transition is
		// irreversible — the previous track is already replaced);
		// the failure is surfaced, not hidden.
		h.events.VoiceError(chatID, "stream_error", berr.Error())
		return nil, errEngineUnavailable("voice bind failed: %v", berr)
	}
	h.applyMuteIfNeeded(chatID)
	h.events.TrackStarted(chatID, pick)
	snap, _ = h.rooms.Snapshot(chatID)
	h.maybePrefetch(chatID, snap)
	return nowPlayingResp(snap), nil
}

// vcVolume sets the room volume, clamped to 0-200 (vc-go clamp
// rule, §10-D15). Phase 3 stores it only; the Phase 5 DSP maps it
// to gain. Mute is separate: setting volume 0 does NOT mute.
func (h *Handlers) vcVolume(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID int64 `json:"chat_id"`
		Volume *int  `json:"volume"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Volume == nil {
		return nil, errBadRequest("volume is required")
	}
	snap, err := h.rooms.SetVolume(chatID, *p.Volume)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	return nowPlayingResp(snap), nil
}

// --- vc.now_playing / vc.progress (§1.13, §1.14) ---------------------

// ensureThumb downloads the track's thumbnail to disk (cached) and
// sets track.ThumbnailPath to the local path, so the core can send it
// as inputFileLocal. It never fails the request: on any error it logs
// and leaves ThumbnailPath empty, and the core falls back to a text
// card with no owner error DM.
func (h *Handlers) ensureThumb(ctx context.Context, track *model.Track) {
	if track == nil || track.ThumbnailPath != "" || track.Thumbnail == "" {
		return
	}
	p, err := thumbs.Ensure(ctx, h.deps.Config.Backend.DataDir, track.Thumbnail)
	if err != nil {
		log.Printf("handlers: thumbnail cache for %s: %v", track.ID, err)
		return
	}
	track.ThumbnailPath = p
}

// vcNowPlaying returns the room snapshot. No room, or a room with
// no track and an empty queue, is not_found (vc-go rule); a room
// with no current but a non-empty queue returns 200 with
// track null.
func (h *Handlers) vcNowPlaying(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok || (snap.Track == nil && len(snap.Queue) == 0) {
		return nil, errNotFound("nothing playing in this chat")
	}
	// Backstop for tracks resolved before the thumbnail cache
	// existed (e.g. rooms restored from bbolt after a restart):
	// ensure the current track's local thumbnail path is set.
	if snap.Track != nil {
		h.ensureThumb(ctx, snap.Track)
	}
	return nowPlayingResp(snap), nil
}

// vcProgress returns the IPC-2 progress view. The 404 rule is the
// vc-go one: a queued-only room returns 200 with now null and
// next as the first waiting track.
func (h *Handlers) vcProgress(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok || (snap.Track == nil && len(snap.Queue) == 0) {
		return nil, errNotFound("nothing playing in this chat")
	}
	var duration int64
	var now *model.Track
	if snap.Track != nil {
		duration = snap.Track.DurationSec
		now = snap.Track
	}
	var next *model.Track
	if len(snap.Queue) > 0 {
		track := snap.Queue[0].Track
		next = &track
	}
	return map[string]any{
		"position_sec": snap.PositionSec,
		"duration_sec": duration,
		"is_paused":    snap.IsPaused,
		"is_muted":     snap.IsMuted,
		"loop_enabled": snap.Loop > 0,
		"now":          now,
		"next":         next,
	}, nil
}

// --- vc.panel (§1.15) ------------------------------------------------

// panelButtons are the 10 remix panel button ids (vc-ts).
var panelButtons = []string{
	"panel_shuffle", "panel_skip", "panel_pause", "panel_resume",
	"panel_mute", "panel_unmute", "panel_voldec", "panel_volinc",
	"panel_loop", "panel_update",
}

// panelState renders the panel object the core reads (vc-ts
// panelState): the chat's status text, now/next summaries, the
// control state, and the button ids. No emojis anywhere (§7.3).
func panelState(snap model.NowPlaying) map[string]any {
	var now, next any
	if snap.Track != nil {
		now = map[string]any{
			"title": snap.Track.Title, "url": snap.Track.URL,
			"artist": snap.Track.Artist,
		}
	}
	if len(snap.Queue) > 0 {
		track := snap.Queue[0].Track
		next = map[string]any{
			"title": track.Title, "url": track.URL,
			"artist": track.Artist,
		}
	}
	var duration int64
	if snap.Track != nil {
		duration = snap.Track.DurationSec
	}
	text := "Nothing playing"
	switch {
	case snap.Track != nil && snap.IsPaused:
		text = fmt.Sprintf("Paused: %s — %s", snap.Track.Title, snap.Track.Artist)
	case snap.Track != nil:
		text = fmt.Sprintf("Now playing: %s — %s", snap.Track.Title, snap.Track.Artist)
	case len(snap.Queue) > 0:
		text = fmt.Sprintf("Queued: %d tracks, nothing playing", len(snap.Queue))
	}
	return map[string]any{
		"chat_id":      snap.ChatID,
		"text":         text,
		"now":          now,
		"next":         next,
		"paused":       snap.IsPaused,
		"is_muted":     snap.IsMuted,
		"position_sec": snap.PositionSec,
		"duration_sec": duration,
		"volume":       snap.Volume,
		"loop":         snap.Loop > 0,
		"queue_count":  len(snap.Queue),
		"buttons":      panelButtons,
	}
}

// vcPanel serves both the GET (render) and POST (act) forms: the
// presence of an `action` in the payload selects the POST form.
// The response carries BOTH keys (§10-D2): {now_playing} per the
// contract and {panel} as the core reads it; POST adds {message}.
func (h *Handlers) vcPanel(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID int64   `json:"chat_id"`
		Action *string `json:"action"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Action == nil {
		snap, ok := h.rooms.Snapshot(chatID)
		if !ok || (snap.Track == nil && len(snap.Queue) == 0) {
			return nil, errNotFound("nothing playing in this chat")
		}
		return map[string]any{
			"now_playing": snap, "panel": panelState(snap),
		}, nil
	}
	return h.panelAction(ctx, chatID, *p.Action)
}

// panelAction performs one panel button action. Control actions
// (skip/pause/resume/mute/unmute/loop) require a current track
// and answer bad_request "Not streaming." without one (vc-ts 400,
// not 404 — the panel is a control surface, its errors are action
// errors); shuffle requires at least one queued track;
// volinc/voldec work on any existing room.
func (h *Handlers) panelAction(ctx context.Context, chatID int64, action string) (any, error) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok {
		return nil, errNotFound("nothing playing in this chat")
	}
	needsCurrent := map[string]bool{
		"skip": true, "pause": true, "resume": true,
		"mute": true, "unmute": true, "loop": true,
	}
	if needsCurrent[action] && snap.Track == nil {
		return nil, errBadRequest("Not streaming.")
	}
	var message string
	switch action {
	case "update":
		message = "Panel updated."
	case "shuffle":
		if len(snap.Queue) < 1 {
			return nil, errBadRequest("Not enough items to shuffle.")
		}
		var err error
		if snap, err = h.rooms.SetShuffle(chatID, true); err != nil {
			return nil, mapRoomErr(err)
		}
		message = "Queue shuffled."
	case "skip":
		if err := h.ensureCall(ctx, chatID); err != nil {
			h.events.VoiceError(chatID, "join_failed", err.Error())
			return nil, errEngineUnavailable("voice join failed: %v", err)
		}
		var aerr error
		_, snap, aerr = h.advancePlayback(ctx, chatID, "", true)
		if aerr != nil {
			return nil, aerr
		}
		message = "Skipped."
	case "pause":
		var err error
		if snap, err = h.rooms.Pause(chatID); err != nil {
			return nil, mapRoomErr(err)
		}
		if _, ok := h.deps.Voice.Get(chatID); ok {
			if _, verr := h.deps.Voice.Pause(chatID); verr != nil {
				_, _ = h.rooms.Resume(chatID)
				return nil, errEngineUnavailable("voice pause failed: %v", verr)
			}
		}
		message = "Paused."
	case "resume":
		var err error
		if snap, err = h.rooms.Resume(chatID); err != nil {
			return nil, mapRoomErr(err)
		}
		if _, ok := h.rooms.Current(chatID); ok {
			if err := h.resumeFeed(ctx, chatID); err != nil {
				return nil, err
			}
		}
		message = "Resumed."
	case "mute", "unmute":
		muted := action == "mute"
		var err error
		if snap, err = h.rooms.SetMuted(chatID, muted); err != nil {
			return nil, mapRoomErr(err)
		}
		if _, ok := h.deps.Voice.Get(chatID); ok {
			var verr error
			if muted {
				_, verr = h.deps.Voice.Mute(chatID)
			} else {
				_, verr = h.deps.Voice.Unmute(chatID)
			}
			if verr != nil {
				_, _ = h.rooms.SetMuted(chatID, !muted)
				return nil, errEngineUnavailable("voice mute failed: %v", verr)
			}
		}
		if muted {
			message = "Muted."
		} else {
			message = "Unmuted."
		}
	case "volinc", "voldec":
		delta := 10
		if action == "voldec" {
			delta = -10
		}
		var err error
		if snap, err = h.rooms.SetVolume(chatID, snap.Volume+delta); err != nil {
			return nil, mapRoomErr(err)
		}
		message = fmt.Sprintf("Volume %d.", snap.Volume)
	case "loop":
		target := 0
		if snap.Loop == 0 {
			target = 10 // panel loop toggle: 10 or 0 (§1.15)
		}
		var err error
		if snap, err = h.rooms.SetLoop(chatID, target); err != nil {
			return nil, mapRoomErr(err)
		}
		if target == 0 {
			message = "Loop off."
		} else {
			message = "Loop on (10)."
		}
	default:
		return nil, errBadRequest("unknown panel action %q (want update|shuffle|skip|pause|resume|mute|unmute|voldec|volinc|loop)", action)
	}
	snap, _ = h.rooms.Snapshot(chatID)
	return map[string]any{
		"message": message, "panel": panelState(snap),
		"now_playing": snap,
	}, nil
}

// --- vc.lyrics (§1.16) -------------------------------------------------

// vcLyrics is POST in the new contract (FINAL §6-R5: the old GET
// is corrected, not preserved). Title/artist come from the body
// when given, else from the room's current track. No title and no
// current track is bad_request; no lyrics is not_found; an LRCLib
// outage (after its one retry) is engine_unavailable. Lyrics are
// never invented.
func (h *Handlers) vcLyrics(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID int64   `json:"chat_id"`
		Title  *string `json:"title"`
		Artist *string `json:"artist"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if h.lyrics == nil {
		return nil, errEngineUnavailable("lyrics not configured")
	}
	var track *model.Track
	if current, ok := h.rooms.Current(chatID); ok {
		track = &current
	}
	title, artist := "", ""
	if p.Title != nil {
		title = strings.TrimSpace(*p.Title)
	}
	if p.Artist != nil {
		artist = strings.TrimSpace(*p.Artist)
	}
	if title == "" && track != nil {
		title = track.Title
		artist = track.Artist
	}
	if title == "" {
		return nil, errBadRequest("no title to look lyrics up for")
	}
	var res *lyrics.Result
	if track != nil && p.Title == nil && p.Artist == nil {
		res, err = h.lyrics.FetchForTrack(ctx, *track)
	} else {
		res, err = h.lyrics.Fetch(ctx, title, artist, "")
	}
	if err != nil {
		if errors.Is(err, lyrics.ErrNoLyrics) {
			return nil, errNotFound("no lyrics found")
		}
		return nil, errEngineUnavailable("lyrics lookup failed: %v", err)
	}
	return map[string]any{
		"lyrics":    res.Lyrics,
		"source":    res.Source,
		"track_id":  res.TrackID,
		"title":     res.Title,
		"artist":    res.Artist,
		"truncated": res.Truncated,
	}, nil
}

// --- vc.join / vc.mute (§1.17, §1.18) ----------------------------------

// vcJoin owns the call: CreateCall (local params) -> JoinRequest
// over UDS (the core runs joinVideoChat on the assistant session)
// -> Connect (remote params). Idempotent: already-joined returns
// 200 {joined: true} without a second handshake. A failed
// handshake is engine_unavailable with the exact reason, never a
// faked join. now_playing is the room snapshot when a room exists,
// else an honest empty snapshot (the join succeeded; there is
// simply nothing playing yet).
func (h *Handlers) vcJoin(ctx context.Context, req uds.Request) (any, error) {
	chatID, err := h.mustChatID(req)
	if err != nil {
		return nil, err
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	if _, err := h.deps.Voice.Join(ctx, chatID); err != nil {
		h.events.VoiceError(chatID, "join_failed", err.Error())
		return nil, errEngineUnavailable("voice join failed: %v", err)
	}
	snap, ok := h.rooms.Snapshot(chatID)
	if !ok {
		snap = model.NowPlaying{
			ChatID: chatID, Engine: "backend-go",
			Queue: []model.QueueItem{}, Volume: 100, Speed: 1.0,
		}
	}
	return map[string]any{"joined": true, "now_playing": snap}, nil
}

// vcMute sets the real mute flag, orthogonal to volume. Idle is
// not_found; a repeated mute/unmute is a 200 success. The voice
// mute is skipped when no call record exists; when a record
// exists and the mute fails, the room flag is undone.
func (h *Handlers) vcMute(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID int64 `json:"chat_id"`
		Muted  *bool `json:"muted"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if p.Muted == nil {
		return nil, errBadRequest("muted is required")
	}
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	snap, err := h.rooms.SetMuted(chatID, *p.Muted)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if _, ok := h.deps.Voice.Get(chatID); ok {
		var verr error
		if *p.Muted {
			_, verr = h.deps.Voice.Mute(chatID)
		} else {
			_, verr = h.deps.Voice.Unmute(chatID)
		}
		if verr != nil {
			_, _ = h.rooms.SetMuted(chatID, !*p.Muted)
			return nil, errEngineUnavailable("voice mute failed: %v", verr)
		}
	}
	snap, _ = h.rooms.Snapshot(chatID)
	return map[string]any{"muted": snap.IsMuted, "now_playing": snap}, nil
}

// --- vc.radio (§1.19) --------------------------------------------------

// vcRadio starts/stops a radio station in the voice chat. Start
// resolves the station, probes the stream, and hands the live
// stream to the room as a radio track (duration 0, is_live). The
// room start succeeds even when the voice handoff fails — the
// failure is reported in vc_handoff and NEVER fails the start
// (§7.2) — and an unreachable stream still starts (reachable:
// false in the response, not an error); the feed is only bound
// when the probe says the stream flows. Stop clears the radio
// state and stops the room's radio track only when the room is
// still playing it; a normal music track that replaced it is NOT
// stopped. No radio is not_found.
func (h *Handlers) vcRadio(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		Action  string `json:"action"`
		ChatID  int64  `json:"chat_id"`
		Station string `json:"station"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	switch p.Action {
	case "start":
		return h.vcRadioStart(ctx, chatID, p.Station)
	case "stop":
		return h.vcRadioStop(ctx, chatID)
	default:
		return nil, errBadRequest("unknown radio action %q (want start|stop)", p.Action)
	}
}

func (h *Handlers) vcRadioStart(ctx context.Context, chatID int64, stationRef string) (any, error) {
	if strings.TrimSpace(stationRef) == "" {
		return nil, errBadRequest("station is required")
	}
	station := resolver.FindStation(stationRef)
	if station == nil {
		return nil, errBadRequest("unknown radio station %q (known: %s)", stationRef, resolver.StationIDs())
	}
	probe := resolver.ProbeStream(ctx, station.StreamURL)
	track := resolver.RadioTrackFromURL(station.StreamURL)
	track.ID = "radio:" + station.ID
	track.Title = station.Name
	track.Artist = station.Name

	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	radioState := model.RadioState{
		ChatID:    chatID,
		Station:   station.ID,
		StreamURL: station.StreamURL,
		Reachable: &probe.Reachable,
		UpdatedAt: time.Now().Unix(),
	}
	snap, err := h.rooms.StartRadio(chatID, radioState, track, 0)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	handoff := map[string]any{"ok": true, "joined": false, "reachable": probe.Reachable}
	if !probe.Reachable {
		// The radio still starts (the stream is the fallback
		// playlist until it recovers), but no feed is bound to
		// a dead stream: binding it would fail at once and the
		// feed-end advance would kill the radio. The handoff
		// reports the probe result honestly.
		handoff["ok"] = false
		handoff["error"] = fmt.Sprintf("stream unreachable: %s", probe.Probe)
		h.events.VoiceError(chatID, "radio_handoff_failed", fmt.Sprintf("radio %s: stream unreachable: %s", station.ID, probe.Probe))
	} else if err := h.ensureCall(ctx, chatID); err != nil {
		handoff["ok"] = false
		handoff["error"] = err.Error()
		h.events.VoiceError(chatID, "join_failed", err.Error())
	} else if _, berr := h.bindTrack(ctx, chatID, track); berr != nil {
		handoff["ok"] = false
		handoff["error"] = berr.Error()
		h.events.VoiceError(chatID, "radio_handoff_failed", berr.Error())
	} else {
		handoff["joined"] = true
		h.applyMuteIfNeeded(chatID)
	}
	h.events.TrackStarted(chatID, track)
	snap, _ = h.rooms.Snapshot(chatID)
	return map[string]any{"now_playing": snap, "vc_handoff": handoff}, nil
}

func (h *Handlers) vcRadioStop(ctx context.Context, chatID int64) (any, error) {
	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	wasRadio, snap, err := h.rooms.StopRadio(chatID)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	if wasRadio {
		h.clearFeeding(chatID)
		// Leave the call only when the radio was the only thing
		// playing (empty queue after the stop).
		if len(snap.Queue) == 0 {
			if _, ok := h.deps.Voice.Get(chatID); ok {
				if verr := h.deps.Voice.Stop(chatID); verr != nil {
					log.Printf("handlers: vc.radio stop leave call chat %d: %v", chatID, verr)
				}
			}
		}
	}
	return nowPlayingResp(snap), nil
}

// --- vc.play_file (§9-E12, §3.3) ---------------------------------------

// vcPlayFile plays a replied-media file the core already downloaded
// to disk. The action carries a PATH, not bytes (16 MiB frame cap).
// The path must be under the data dir (.. is rejected) and the
// file must exist (missing is not_found). The track is
// {id: "file:<file_id or basename-hash>", source: "file"} and
// plays now when idle, else enqueues, exactly like vc.play. In
// Phase 3 the room records it as current with an empty stream_url
// and NO feed is bound: the file is decoded by parlay-dsp from the
// local path in Phase 5 (inventory §3.3). The empty stream_url in
// now_playing plus the voice_state detail are the honest signals —
// never a fake join/play claim. Duration 0 (unknown) never
// auto-advances: there is no feed, so no EOF ever fires.
//
// This is the ONLY registered vc.play_file (main.go registers
// handlers after download, so this record-only version wins the
// silent-overwrite slot over download's feed-binding duplicate in
// internal/download/playfile.go; the verification worker deletes
// or neutralizes that file).
func (h *Handlers) vcPlayFile(ctx context.Context, req uds.Request) (any, error) {
	var p struct {
		ChatID      int64  `json:"chat_id"`
		FilePath    string `json:"file_path"`
		FileID      string `json:"file_id"`
		Title       string `json:"title"`
		Kind        string `json:"kind"`
		DurationSec *int64 `json:"duration_sec"`
		RequestedBy int64  `json:"requested_by"`
	}
	if err := decodePayload(req, &p); err != nil {
		return nil, err
	}
	chatID, err := chatIDOf(req, p.ChatID)
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(p.FilePath) == "" {
		return nil, errBadRequest("file_path is required")
	}
	if p.Kind != "" && !playFileKinds[p.Kind] {
		return nil, errBadRequest("unknown media kind %q (want audio|video|voice|video_note|document)", p.Kind)
	}
	if p.DurationSec != nil && *p.DurationSec < 0 {
		return nil, errBadRequest("duration_sec must not be negative")
	}
	dataDir, err := filepath.Abs(h.deps.Config.Backend.DataDir)
	if err != nil {
		return nil, errInternal("cannot resolve data dir: %v", err)
	}
	// A relative path resolves against the data dir (same rule as
	// download's validateRepliedPath); an absolute path must
	// already be under it.
	absPath := p.FilePath
	if !filepath.IsAbs(absPath) {
		absPath = filepath.Join(dataDir, absPath)
	}
	absPath, err = filepath.Abs(absPath)
	if err != nil {
		return nil, errBadRequest("bad file_path: %v", err)
	}
	if absPath != dataDir && !strings.HasPrefix(absPath, dataDir+string(os.PathSeparator)) {
		return nil, errBadRequest("file_path must be under the data dir")
	}
	info, err := os.Stat(absPath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, errNotFound("replied file not found on disk")
		}
		return nil, errInternal("cannot stat replied file: %v", err)
	}
	if info.IsDir() {
		return nil, errBadRequest("file_path is a directory, not a file")
	}
	id := p.FileID
	if id == "" {
		sum := sha1.Sum([]byte(absPath))
		id = hex.EncodeToString(sum[:])[:12]
	}
	title := strings.TrimSpace(p.Title)
	if title == "" {
		title = filepath.Base(absPath)
	}
	var duration int64
	if p.DurationSec != nil {
		duration = *p.DurationSec
	}
	track := model.Track{
		ID:          "file:" + id,
		Title:       title,
		Source:      "file",
		DurationSec: duration,
		DurationStr: resolver.FormatDurationStr(int(duration), false),
	}
	by := userIDOf(req, p.RequestedBy)

	h.chatLock(chatID).Lock()
	defer h.chatLock(chatID).Unlock()
	if err := h.ensureCall(ctx, chatID); err != nil {
		h.events.VoiceError(chatID, "join_failed", err.Error())
		return nil, errEngineUnavailable("voice join failed: %v", err)
	}
	out, err := h.rooms.Play(chatID, track, by)
	if err != nil {
		return nil, mapRoomErr(err)
	}
	snap, _ := h.rooms.Snapshot(chatID)
	if !out.Started {
		return map[string]any{
			"now_playing": snap, "queued": true,
			"resolved_by": "file",
			"voice_state": h.voiceStateForFile(chatID),
		}, nil
	}
	// No feed is bound for a local file in Phase 3 (§3.3): the
	// DSP decodes it in Phase 5. TrackStarted still fires — the
	// room started the track.
	h.events.TrackStarted(chatID, track)
	snap, _ = h.rooms.Snapshot(chatID)
	return map[string]any{
		"now_playing": snap, "queued": false,
		"resolved_by": "file",
		"voice_state": h.voiceStateForFile(chatID),
	}, nil
}

// playFileKinds is the §3.3 media-kind enum for vc.play_file.
var playFileKinds = map[string]bool{
	"audio": true, "video": true, "voice": true, "video_note": true, "document": true,
}

// voiceStateForFile reports the voice side for a replied file,
// honestly and from the real Manager state — never a claim. A
// local file has no feed in Phase 3 (streaming is always false);
// the detail is the §3.3 wording verbatim, so the core can show
// the user why no audio is flowing. joined reads the live call
// record: true only when a call is actually connected.
func (h *Handlers) voiceStateForFile(chatID int64) map[string]any {
	joined := false
	if call, ok := h.deps.Voice.Get(chatID); ok && call.State == voice.StateConnected {
		joined = true
	}
	return map[string]any{
		"joined":    joined,
		"streaming": false,
		"detail":    "local file — audio path lands with the DSP",
	}
}

// mustChatID decodes a {chat_id}-only payload (the common case for
// the simple vc.* actions).
func (h *Handlers) mustChatID(req uds.Request) (int64, error) {
	var p struct {
		ChatID int64 `json:"chat_id"`
	}
	if err := decodePayload(req, &p); err != nil {
		return 0, err
	}
	return chatIDOf(req, p.ChatID)
}
