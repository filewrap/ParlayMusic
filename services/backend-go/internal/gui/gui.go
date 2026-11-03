// Package gui implements the gui.* UDS actions
// (phase3/actions-inventory.md §0): read-only state views for the
// Mini App serving in Phase 6. Pure reads from the shared room
// Manager — the room owns the live position clock, so these handlers
// report the live snapshot, never a persisted frozen position.
// No mutations. (Mini App serving itself is Phase 6.)
package gui

import (
	"context"
	"encoding/json"
	"fmt"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
)

// Service owns the gui.* handlers.
type Service struct {
	store *store.Store
	room  *room.Manager
}

// Register registers gui.now_playing and gui.queue on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil || d.Room == nil {
		panic("gui: Register called with nil deps")
	}
	svc := &Service{store: d.Store, room: d.Room}
	s.Register("gui.now_playing", svc.handleNowPlaying)
	s.Register("gui.queue", svc.handleQueue)
}

// Snapshot builds a NowPlaying view from a persisted room state and
// its waiting queue. Exported so the radio package can render the
// same shape for its now_playing superset field.
func Snapshot(room model.RoomState, queue []model.QueueItem) model.NowPlaying {
	if queue == nil {
		queue = []model.QueueItem{}
	}
	np := model.NowPlaying{
		ChatID:          room.ChatID,
		Track:           room.Current,
		Engine:          "backend-go",
		Loop:            room.Loop,
		LoopCount:       room.Loop,
		Paused:          room.Paused,
		IsPaused:        room.Paused,
		IsMuted:         room.Muted,
		PositionSec:     room.PositionSec,
		Queue:           queue,
		Shuffle:         room.Shuffle,
		Autoplay:        room.Autoplay,
		AutoplayEnabled: room.Autoplay,
		Volume:          room.Volume,
		Speed:           room.Speed,
		StreamURL:       room.StreamURL,
	}
	return np
}

func (s *Service) handleNowPlaying(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		ChatID int64 `json:"chat_id"`
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
	np, found := s.room.Snapshot(chatID)
	// Same 404 rule as vc.now_playing: no room, or a room with no
	// track and an empty queue, is "nothing playing in this chat".
	// A room with no current track but a non-empty queue returns
	// 200 with track null.
	if !found || (np.Track == nil && len(np.Queue) == 0) {
		return nil, uds.NewHandlerError(uds.ErrNotFound, "nothing playing in this chat")
	}
	return map[string]any{"now_playing": np}, nil
}

func (s *Service) handleQueue(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		ChatID int64 `json:"chat_id"`
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
	queue, found := s.room.Queue(chatID)
	if !found {
		queue = []model.QueueItem{}
	}
	return map[string]any{"queue": queue}, nil
}
