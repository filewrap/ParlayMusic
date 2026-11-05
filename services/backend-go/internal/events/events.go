// Package events is the ONE emitter for backend-initiated events
// (contracts/uds-protocol.md §5, phase3/actions-inventory.md §4.5).
// Every event the backend sends to the core goes through this
// package, and it is called only from the §4.5 firing points:
//
//	TrackStarted  StartTrack completes (vc.play on idle, Advance
//	              replay/next/autoplay pick, autoplay_skip pick,
//	              vc.play_file start, radio track start)
//	TrackAdvanced Advance pops a Next track (a loop replay does NOT
//	              fire this)
//	TrackEnded    A track ends with no successor (Advance to idle,
//	              vc.skip to empty, autoplay pick fails/empty)
//	QueueEmpty    The queue drains AND the room goes idle (fires
//	              alongside TrackEnded, not instead)
//	VoiceError    Voice-call failure the core should surface
//	              (join handshake failure, ntg_connect failure,
//	              stream/DSP error mid-track, radio handoff
//	              failure). NOT for resolve failures (synchronous
//	              action errors) and NOT for autoplay pick
//	              failures (those are TrackEnded/QueueEmpty).
//	PlayProgress  One vc.play's staged progress (resolved /
//	              thumbnail / joining), emitted mid-request so the
//	              core can edit that play's status message live.
//	              Only when the play body carried play_status_id;
//	              the core ignores it otherwise (unknown stages
//	              are ignored, per the protocol).
//
// DownloadReady belongs to Worker B (download package) and is
// deliberately NOT emitted here.
//
// Emission is fire-and-forget via srv.Emit: with no core connected
// the event is dropped, never queued, and never blocks playback.
package events

import (
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// Emitter sends backend-initiated events over the UDS server.
type Emitter struct {
	srv *uds.Server
}

// New builds an Emitter around srv.
func New(srv *uds.Server) *Emitter { return &Emitter{srv: srv} }

// TrackStarted fires when a track starts playing in a chat.
func (e *Emitter) TrackStarted(chatID int64, track model.Track) {
	e.srv.Emit("TrackStarted", chatID, map[string]any{
		"track": track, "chat_id": chatID,
	})
}

// TrackAdvanced fires when Advance pops a Next track. A loop replay
// does NOT fire this event.
func (e *Emitter) TrackAdvanced(chatID int64, track model.Track, previousTrackID string) {
	e.srv.Emit("TrackAdvanced", chatID, map[string]any{
		"track": track, "previous_track_id": previousTrackID,
		"chat_id": chatID,
	})
}

// TrackEnded fires when a track ends with no successor. Reason is
// one of "queue_empty" | "autoplay_failed" | "skipped".
func (e *Emitter) TrackEnded(chatID int64, track model.Track, reason string) {
	e.srv.Emit("TrackEnded", chatID, map[string]any{
		"track": track, "chat_id": chatID, "reason": reason,
	})
}

// QueueEmpty fires when the queue drains AND the room goes idle
// (the autoplay decision point for the core). It fires alongside
// TrackEnded, not instead of it.
func (e *Emitter) QueueEmpty(chatID int64) {
	e.srv.Emit("QueueEmpty", chatID, map[string]any{
		"chat_id": chatID,
	})
}

// VoiceError fires when a voice-call failure should be surfaced to
// the core: the code names the failure stage, the message carries
// the exact reason.
func (e *Emitter) VoiceError(chatID int64, code, message string) {
	e.srv.Emit("VoiceError", chatID, map[string]any{
		"chat_id": chatID, "code": code, "message": message,
	})
}

// PlayProgress fires one staged update for an in-flight vc.play
// (stage "resolved" | "thumbnail" | "joining"), so the core can
// edit that play's status message live. statusID is the
// play_status_id the core sent in the play body: the core routes
// the event to that status message and ignores events for a status
// it no longer tracks. fields carries the stage's data (resolved:
// title, artist, duration_str, source; thumbnail: has_thumbnail).
// Fire-and-forget like every other event: with no core connected
// the event is dropped, never queued, never blocking playback.
func (e *Emitter) PlayProgress(chatID, statusID int64, stage string, fields map[string]any) {
	data := map[string]any{
		"chat_id": chatID, "play_status_id": statusID, "stage": stage,
	}
	for k, v := range fields {
		data[k] = v
	}
	e.srv.Emit("PlayProgress", chatID, data)
}
