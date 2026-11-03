// Package store — canonical VC room document (A11).
//
// The canonical room is the value stored for key `vc:room:<chat_id>`
// in the shared store (Mongo collection `state`, {_id: "kv:<key>",
// v: "<json>"}; Redis key `parlay:kv:<key>`). All three VC engines
// write and read this exact shape, so a queue written by one engine is
// servable by another after fail-over:
//
//   {
//     "version": 1, "chat_id": 123, "current": <track|null>,
//     "queue": [{"position":0,"requested_by":0,"track":{...}}],
//     "paused": true, "position_sec": 42, "volume": 100,
//     "loop": 0, "shuffle": false, "autoplay": false,
//     "history": ["<track id>", ...], "updated_at_ms": 1730000000000
//   }
//
// Track: contract fields only (id/title/source/url/artist/
// duration_sec/thumbnail/stream_url, omitempty). The only differences
// from vc-go's PersistedRoom are the field names `waiting` -> `queue`
// and `autoplay_history` -> `history`, plus the schema version and
// write timestamp. Engine-local fields (vc-rust resolved_by/file_path,
// vc-ts started_at_ms/muted_volume) never enter the document.
// Restored rooms come back paused in every engine (reader policy).
package store

import (
	"encoding/json"
	"time"

	"parlay/vc-go/internal/queue"
	"parlay/vc-go/internal/types"
)

// CanonicalRoom is the shared-store form of a room.
type CanonicalRoom struct {
	Version     int                `json:"version"`
	ChatID      int64              `json:"chat_id"`
	Current     *types.Track       `json:"current"`
	Queue       []types.QueueItem  `json:"queue,omitempty"`
	Paused      bool               `json:"paused"`
	Position    int64              `json:"position_sec"`
	Volume      int                `json:"volume"`
	Loop        int                `json:"loop,omitempty"`
	Shuffle     bool               `json:"shuffle,omitempty"`
	Autoplay    bool               `json:"autoplay,omitempty"`
	History     []string           `json:"history,omitempty"`
	UpdatedAtMs int64              `json:"updated_at_ms"`
}

// ToCanonical converts a PersistedRoom to its canonical form.
func ToCanonical(p queue.PersistedRoom) CanonicalRoom {
	return CanonicalRoom{
		Version:     1,
		ChatID:      p.ChatID,
		Current:     p.Current,
		Queue:       p.Waiting,
		Paused:      p.Paused,
		Position:    p.Position,
		Volume:      p.Volume,
		Loop:        p.Loop,
		Shuffle:     p.Shuffle,
		Autoplay:    p.Autoplay,
		History:     p.AutoplayHistory,
		UpdatedAtMs: time.Now().UnixMilli(),
	}
}

// ToPersisted converts a canonical room back to a PersistedRoom.
func (c CanonicalRoom) ToPersisted() queue.PersistedRoom {
	return queue.PersistedRoom{
		ChatID:          c.ChatID,
		Current:         c.Current,
		Waiting:         c.Queue,
		Paused:          c.Paused,
		Volume:          c.Volume,
		Position:        c.Position,
		Loop:            c.Loop,
		Shuffle:         c.Shuffle,
		Autoplay:        c.Autoplay,
		AutoplayHistory: c.History,
	}
}

// CanonicalRoomJSON renders a PersistedRoom as canonical JSON.
func CanonicalRoomJSON(p queue.PersistedRoom) string {
	b, err := json.Marshal(ToCanonical(p))
	if err != nil {
		return "{}"
	}
	return string(b)
}

// ParseCanonicalRoom parses canonical room JSON (from any engine).
// Unknown fields are ignored, so docs written by vc-ts/vc-rust load
// as-is; missing optionals default to zero values.
func ParseCanonicalRoom(text string) (queue.PersistedRoom, error) {
	var c CanonicalRoom
	if err := json.Unmarshal([]byte(text), &c); err != nil {
		return queue.PersistedRoom{}, err
	}
	return c.ToPersisted(), nil
}
