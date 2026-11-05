// Package radio implements the radio.* UDS actions
// (phase3/actions-inventory.md §7.1–§7.4, ported from
// services/radio-js): the curated station list, native stream
// verification (resolver.ProbeStream — no ffprobe subprocess),
// per-chat radio state, and the VC handoff.
//
// A stream that fails its probe still starts: the radio is the
// fallback playlist until the stream recovers (reachable:false in
// the response, not an error). A handoff failure never fails the
// start either — it is reported in vc_handoff.
//
// SEAM for vc.radio (Worker A): vc.radio hands off to Start/Stop
// below — Start returns the full StartResult (station, stream URL,
// probe, vc_handoff, now_playing) and Stop returns whether the VC
// was cleared. vc.radio wraps those into {now_playing}.
package radio

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"time"

	"parlay/backend-go/internal/app"
	"parlay/backend-go/internal/dsp"
	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/resolver"
	"parlay/backend-go/internal/room"
	"parlay/backend-go/internal/store"
	"parlay/backend-go/internal/uds"
	"parlay/backend-go/internal/voice"
)

// fallbackRotationSecs is the fallback-playlist rotation interval
// (radio-js: the playlist rotates every 180 s).
const fallbackRotationSecs = 180

// Service owns the radio.* handlers.
type Service struct {
	store    *store.Store
	rooms    *room.Manager
	voice    *voice.Manager
	registry *resolver.Registry

	// probe verifies a stream URL. Overridable for tests (default:
	// resolver.ProbeStream — native HTTP/ICY, no ffprobe).
	probe func(ctx context.Context, streamURL string) resolver.ProbeResult

	mu       sync.Mutex
	handoffs map[int64]map[string]any // chatID -> last vc_handoff
}

// StartResult is the outcome of Start (also the radio.start
// response shape, plus now_playing).
type StartResult struct {
	Station          string           `json:"station"`
	StationName      string           `json:"station_name"`
	StreamURL        string           `json:"stream_url"`
	Reachable        bool             `json:"reachable"`
	Probe            string           `json:"probe"`
	Format           string           `json:"format"`
	FallbackPlaylist []string         `json:"fallback_playlist"`
	VCHandoff        map[string]any   `json:"vc_handoff"`
	NowPlaying       model.NowPlaying `json:"now_playing"`
}

// Register registers radio.list / start / stop / status on s.
func Register(s *uds.Server, d *app.Deps) {
	if d == nil || d.Store == nil {
		panic("radio: Register called with nil deps")
	}
	svc := &Service{
		store: d.Store, rooms: d.Room, voice: d.Voice, registry: d.Registry,
		probe: resolver.ProbeStream, handoffs: map[int64]map[string]any{},
	}
	s.Register("radio.list", svc.handleList)
	s.Register("radio.start", svc.handleStart)
	s.Register("radio.stop", svc.handleStop)
	s.Register("radio.status", svc.handleStatus)
}

// radioTrack builds the radio track for a station at a playlist
// index: id radio:<station_id>:<playlist_index>, title
// "<station> — <playlist entry>", duration 0, is_live.
func radioTrack(st *resolver.Station, index int) model.Track {
	entry := ""
	if len(st.Playlist) > 0 {
		entry = st.Playlist[index%len(st.Playlist)]
	}
	title := st.Name
	if entry != "" {
		title = st.Name + " — " + entry
	}
	return model.Track{
		ID:    "radio:" + st.ID + ":" + fmt.Sprint(index),
		Title: title, Artist: st.Name, Source: "radio",
		URL: st.StreamURL, StreamURL: st.StreamURL,
		DurationSec: 0, DurationStr: "∞", IsLive: true,
	}
}

// playlistIndex computes the current fallback-playlist index from
// the elapsed time since the radio started.
func playlistIndex(st *resolver.Station, startedAt time.Time) int {
	if len(st.Playlist) == 0 {
		return 0
	}
	elapsed := int(time.Since(startedAt).Seconds())
	if elapsed < 0 {
		elapsed = 0
	}
	return (elapsed / fallbackRotationSecs) % len(st.Playlist)
}

// Start resolves a station, probes its stream (best effort),
// persists the radio state, puts the radio track on the room, and
// attempts the VC handoff. A failed probe or handoff never fails
// the start — both are reported honestly in the result.
func (s *Service) Start(ctx context.Context, chatID int64, stationRef string) (*StartResult, error) {
	if chatID == 0 {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, "chat_id is required")
	}
	station := resolver.FindStation(stationRef)
	if station == nil {
		return nil, uds.NewHandlerError(uds.ErrBadRequest, fmt.Sprintf("unknown radio station %q (known: %s)", stationRef, resolver.StationIDs()))
	}
	probe := s.probe(ctx, station.StreamURL)
	track := radioTrack(station, 0)
	if s.rooms == nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, "room manager not configured")
	}
	now := time.Now()
	reachable := probe.Reachable
	radioState := model.RadioState{
		ChatID: chatID, Station: station.ID, StreamURL: station.StreamURL,
		Reachable: &reachable, NowPlaying: &track, UpdatedAt: now.Unix(),
	}
	// The room mutation goes through the Manager (never a direct
	// store write, which would leave the Manager's in-memory room
	// stale): the radio track becomes the front track and the
	// radio state is recorded, both in the room and in the
	// store's radio bucket.
	snap, err := s.rooms.StartRadio(chatID, radioState, track, 0)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("room start failed: %v", err))
	}
	handoff := s.voiceHandoff(ctx, chatID, station.StreamURL)
	s.mu.Lock()
	s.handoffs[chatID] = handoff
	s.mu.Unlock()
	return &StartResult{
		Station: station.ID, StationName: station.Name,
		StreamURL: station.StreamURL, Reachable: probe.Reachable,
		Probe: probe.Probe, Format: probe.Format,
		FallbackPlaylist: station.Playlist,
		VCHandoff:        handoff,
		NowPlaying:       snap,
	}, nil
}

// voiceHandoff joins the voice chat and binds the radio stream.
// Any failure is reported in the handoff map, never raised: the
// start succeeds regardless (inventory §7.2).
func (s *Service) voiceHandoff(ctx context.Context, chatID int64, streamURL string) map[string]any {
	if s.voice == nil {
		return map[string]any{"joined": false, "error": "voice manager not configured"}
	}
	opts := dsp.DefaultOptions()
	if s.rooms != nil {
		opts = s.rooms.DSPOptions(chatID)
	}
	groupCallID, err := s.voice.Join(ctx, chatID)
	if err != nil {
		return map[string]any{"joined": false, "error": err.Error()}
	}
	if err := s.voice.Play(ctx, chatID, streamURL, opts); err != nil {
		return map[string]any{"joined": true, "group_call_id": groupCallID, "error": err.Error()}
	}
	return map[string]any{"joined": true, "group_call_id": groupCallID}
}

// Stop clears the radio state for a chat. The room's radio track is
// stopped only if it is still the current track — a normal music
// track that replaced the radio is NOT stopped. Returns whether the
// voice call was cleared.
func (s *Service) Stop(ctx context.Context, chatID int64) (bool, error) {
	if chatID == 0 {
		return false, uds.NewHandlerError(uds.ErrBadRequest, "chat_id is required")
	}
	if _, found, err := s.store.GetRadio(chatID); err != nil {
		return false, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("radio lookup failed: %v", err))
	} else if !found {
		return false, uds.NewHandlerError(uds.ErrNotFound, "no radio is playing in this chat")
	}
	vcCleared := false
	if s.rooms == nil {
		return false, uds.NewHandlerError(uds.ErrInternal, "room manager not configured")
	}
	// The room mutation goes through the Manager: the radio track
	// is dropped from the front only when it is still the active
	// track — a normal music track that replaced it is NOT
	// stopped. StopRadio deletes the radio bucket itself; a room
	// with no radio state is "not the active track", not an error.
	wasRadio, _, err := s.rooms.StopRadio(chatID)
	if err != nil {
		if errors.Is(err, room.ErrNothingPlaying) {
			wasRadio = false
		} else {
			return false, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("room stop failed: %v", err))
		}
	}
	if wasRadio {
		if s.voice != nil {
			// Leave the call only if the call was radio-only:
			// the room had no other track playing. Stop reports
			// an error when there is no call; that means there
			// was nothing to clear, not a failure.
			if err := s.voice.Stop(chatID); err == nil {
				vcCleared = true
			}
		}
	}
	s.mu.Lock()
	delete(s.handoffs, chatID)
	s.mu.Unlock()
	return vcCleared, nil
}

// --- handlers -----------------------------------------------------------

func (s *Service) handleList(ctx context.Context, req uds.Request) (any, error) {
	type stationView struct {
		ID        string `json:"id"`
		Name      string `json:"name"`
		Genre     string `json:"genre"`
		StreamURL string `json:"stream_url"`
		Reachable bool   `json:"reachable"`
		Format    string `json:"format"`
	}
	views := make([]stationView, len(resolver.Stations))
	var wg sync.WaitGroup
	for i, st := range resolver.Stations {
		wg.Add(1)
		go func(i int, st resolver.Station) {
			defer wg.Done()
			probe := s.probe(ctx, st.StreamURL)
			views[i] = stationView{
				ID: st.ID, Name: st.Name, Genre: st.Genre,
				StreamURL: st.StreamURL, Reachable: probe.Reachable,
				Format: probe.Format,
			}
		}(i, st)
	}
	wg.Wait()
	return map[string]any{"stations": views}, nil
}

func (s *Service) handleStart(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		ChatID  int64  `json:"chat_id"`
		Station string `json:"station"`
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
	result, err := s.Start(ctx, chatID, body.Station)
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"station": result.Station, "station_name": result.StationName,
		"stream_url": result.StreamURL, "reachable": result.Reachable,
		"probe": result.Probe, "format": result.Format,
		"fallback_playlist": result.FallbackPlaylist,
		"vc_handoff":        result.VCHandoff,
		"now_playing":       result.NowPlaying,
	}, nil
}

func (s *Service) handleStop(ctx context.Context, req uds.Request) (any, error) {
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
	vcCleared, err := s.Stop(ctx, chatID)
	if err != nil {
		return nil, err
	}
	// Both keys: the contract's {stopped} and radio-js's
	// {vc_cleared} (inventory §9-D8).
	return map[string]any{"stopped": true, "vc_cleared": vcCleared}, nil
}

func (s *Service) handleStatus(ctx context.Context, req uds.Request) (any, error) {
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
	state, found, err := s.store.GetRadio(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("radio lookup failed: %v", err))
	}
	if !found {
		return nil, uds.NewHandlerError(uds.ErrNotFound, "no radio is playing in this chat")
	}
	station := resolver.FindStation(state.Station)
	nowPlaying := state.NowPlaying
	startedAt := time.Unix(state.UpdatedAt, 0)
	if station != nil {
		// The fallback playlist rotates every 180 s; the
		// now-playing title follows the current entry.
		track := radioTrack(station, playlistIndex(station, startedAt))
		nowPlaying = &track
	}
	queue, err := s.store.GetQueue(chatID)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrInternal, fmt.Sprintf("queue lookup failed: %v", err))
	}
	autoplay := false
	queueLength := len(queue)
	if room, found, err := s.store.GetRoom(chatID); err == nil && found {
		autoplay = room.Autoplay
		// The stored list includes the playing track at index 0;
		// the reported queue length counts waiting tracks only.
		if room.Playing && queueLength > 0 {
			queueLength--
		}
	}
	s.mu.Lock()
	handoff := s.handoffs[chatID]
	s.mu.Unlock()
	var vcHandoff any
	if handoff != nil {
		vcHandoff = handoff
	}
	stationName := state.Station
	if station != nil {
		stationName = station.Name
	}
	return map[string]any{
		"station":       state.Station,
		"station_name":  stationName,
		"stream_url":    state.StreamURL,
		"now_playing":   nowPlaying,
		"started_at_ms": state.UpdatedAt * 1000,
		"reachable":     state.Reachable,
		"vc_handoff":    vcHandoff,
		// Superset fields (inventory §7.4): listeners are always
		// 0 — no listener accounting exists.
		"listeners": 0, "total_listeners": 0, "loop_type": "none",
		"autoplay_enabled": autoplay, "queue_length": queueLength,
	}, nil
}
