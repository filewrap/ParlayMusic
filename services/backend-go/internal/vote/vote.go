// Package vote implements the queue.vote UDS action
// (phase3/actions-inventory.md §7.7, ported from services/queue-rust):
// one vote per voter per track (a re-vote replaces; a decline cancels
// that voter's approval). queued = approvals >= threshold. The
// threshold is configurable: PARLAY_VOTE_THRESHOLD, default 3
// (parlay.toml has no [voting] section yet — see the report; the
// threshold is read at call time, so a config reload changes it for
// new votes and existing tallies are recomputed against it).
//
// queued is a threshold signal AND, per the Wave 2 task, the backend
// drives the room queue: when a track newly reaches threshold it is
// appended to the chat's waiting queue via the shared room.Manager
// (d.Room.Enqueue — vc.queue add semantics: it never starts playback,
// even when the room is idle). The Manager is the single owner of
// room state; this package never writes the queue bucket directly.
package vote

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// Service owns the queue.vote handler.
type Service struct {
	store *store.Store
	room  *room.Manager
}

// Register registers queue.vote on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil || d.Room == nil {
		panic("vote: Register called with nil deps")
	}
	svc := &Service{store: d.Store, room: d.Room}
	s.Register("queue.vote", svc.handleVote)
}

// threshold reads the vote threshold at call time: env
// PARLAY_VOTE_THRESHOLD, default 3, minimum 1. An unparsable value
// falls back to 3 rather than failing the vote.
func threshold() int {
	raw := strings.TrimSpace(os.Getenv("PARLAY_VOTE_THRESHOLD"))
	if raw == "" {
		return 3
	}
	n, err := strconv.Atoi(raw)
	if err != nil || n < 1 {
		return 3
	}
	return n
}

// parseUserID accepts a voter id as a JSON number or a numeric string
// (queue-rust accepted both).
func parseUserID(raw json.RawMessage) (int64, error) {
	if len(raw) == 0 {
		return 0, fmt.Errorf("missing voter_id")
	}
	var n int64
	if err := json.Unmarshal(raw, &n); err == nil && n != 0 {
		return n, nil
	}
	var s string
	if err := json.Unmarshal(raw, &s); err == nil {
		if v, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64); err == nil && v != 0 {
			return v, nil
		}
	}
	return 0, fmt.Errorf("voter_id must be a non-zero user id (number or numeric string)")
}

func (s *Service) handleVote(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Track   model.Track     `json:"track"`
		Vote    string          `json:"vote"`
		VoterID json.RawMessage `json:"voter_id"`
		ChatID  int64           `json:"chat_id"`
	}
	if len(req.Payload) == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "missing request payload")
	}
	if err := json.Unmarshal(req.Payload, &body); err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("invalid request payload: %v", err))
	}
	chatID := body.ChatID
	if chatID == 0 {
		chatID = req.ChatID
	}
	if chatID == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "chat_id is required")
	}
	if body.Track.ID == "" || body.Track.Title == "" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "track with id and title is required")
	}
	if body.Vote != "approve" && body.Vote != "decline" {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("vote must be approve or decline, got %q", body.Vote))
	}
	voterID, err := parseUserID(body.VoterID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("%v", err))
	}

	th := threshold()
	state, found, err := s.store.GetVote(chatID, body.Track.ID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("vote store lookup failed: %v", err))
	}
	if !found {
		state = model.VoteState{
			ChatID: chatID, Track: body.Track,
			Voters: map[int64]string{}, Threshold: th,
			CreatedAt: time.Now().Unix(),
		}
	}
	wasQueued := state.Queued
	state.Track = body.Track
	state.Voters[voterID] = body.Vote
	state.Threshold = th
	state.UpdatedAt = time.Now().Unix()
	// Tallies are recomputed from the voter map every time, so a
	// re-vote or a threshold change is always consistent.
	approvals, declines := 0, 0
	for _, v := range state.Voters {
		switch v {
		case "approve":
			approvals++
		case "decline":
			declines++
		}
	}
	state.Approvals, state.Declines = approvals, declines
	state.Queued = approvals >= th
	if err := s.store.PutVote(state); err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("vote store failed: %v", err))
	}
	if state.Queued && !wasQueued {
		// Newly reached threshold: drive the room queue.
		if err := s.enqueueOnThreshold(chatID, body.Track, voterID); err != nil {
			return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("vote queueing failed: %v", err))
		}
	}
	return map[string]any{
		"queued": state.Queued, "approvals": approvals,
		"declines": declines, "threshold": th,
		"track_id": body.Track.ID,
	}, nil
}

// enqueueOnThreshold appends the track to the chat's waiting queue
// via the shared room.Manager (vc.queue add semantics: never starts
// playback). A track already in the queue is not appended twice.
func (s *Service) enqueueOnThreshold(chatID int64, track model.Track, voterID int64) error {
	if queue, ok := s.room.Queue(chatID); ok {
		for _, item := range queue {
			if item.Track.ID == track.ID {
				return nil
			}
		}
	}
	return s.room.Enqueue(chatID, track, voterID)
}
