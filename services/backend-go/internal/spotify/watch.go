// Watch registry + currently-playing poller (inventory §6.5, §6.9,
// ported from services/spotify-rust/src/main.rs poller_loop +
// handle_spotify_watch / handle_spotify_now_playing).
//
// The poller runs only when SPOTIFY_POLL_INTERVAL_SECS > 0
// (default 30 s). Per watched user with a stored token: refresh when
// the token expires within 120 s, fetch
// /v1/me/player/currently-playing, on track change run the skip-check
// decision and record the verdict. A 401 drops the token (the user
// must re-login); other failures keep the previous state. The poller
// never skips on the user's player (scopes are read-only).
package spotify

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"parlay/backend-go/internal/model"
	"parlay/backend-go/internal/uds"
)

// Stop halts the poller (used by tests; the backend process exit
// stops it in production).
func (s *Service) Stop() {
	s.pollMu.Lock()
	defer s.pollMu.Unlock()
	if s.pollRunning {
		close(s.pollStop)
		s.pollRunning = false
	}
}

// ensurePoller starts the poller goroutine once, when polling is
// enabled. It is lazy (first watch start) so a backend that never
// watches anyone spawns no goroutine.
func (s *Service) ensurePoller() {
	if s.pollInterval <= 0 {
		return
	}
	s.pollMu.Lock()
	defer s.pollMu.Unlock()
	if s.pollRunning {
		return
	}
	s.pollRunning = true
	s.pollStop = make(chan struct{})
	go s.pollerLoop(s.pollStop)
}

func (s *Service) pollerLoop(stop <-chan struct{}) {
	ticker := time.NewTicker(s.pollInterval)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			s.pollOnce(context.Background())
		}
	}
}

// pollOnce polls every watched user once.
func (s *Service) pollOnce(ctx context.Context) {
	ids, err := s.store.ListWatches()
	if err != nil {
		return
	}
	for _, userID := range ids {
		s.pollUser(ctx, userID)
	}
}

// pollUser refreshes the token if needed and fetches the user's
// currently-playing track.
func (s *Service) pollUser(ctx context.Context, userID int64) {
	tok, err := s.refreshIfNeeded(ctx, userID)
	if err != nil {
		// No token (404) or a dead refresh token (401, dropped):
		// the poller skips this user and keeps the previous state.
		return
	}
	now, code, err := s.fetchCurrentlyPlaying(ctx, tok.accessToken)
	if err != nil {
		if code == http.StatusUnauthorized {
			// Dead token: drop it; the user must log in again.
			_, _ = s.deleteUserToken(userID)
		}
		return
	}
	s.npMu.Lock()
	prev, ok := s.np[userID]
	s.npMu.Unlock()
	checkedAt := time.Now().Unix()
	if now == nil || !now.isPlaying {
		s.npMu.Lock()
		s.np[userID] = &polledState{playing: false, checkedAt: checkedAt, verdict: "unknown"}
		s.npMu.Unlock()
		return
	}
	track := now.track
	changed := !ok || prev == nil || prev.track == nil || prev.track.ID != track.ID
	state := &polledState{track: &track, playing: true, checkedAt: checkedAt, verdict: "unknown"}
	if ok && prev != nil {
		state.lastSkip = prev.lastSkip
		state.lastReason = prev.lastReason
		state.verdict = prev.verdict
	}
	if changed {
		// Track change: run the skip-check decision and record it.
		decision, derr := s.decideSkip(ctx, track, &userID)
		if derr == nil {
			state.lastSkip = decision.skip
			if decision.reason != nil {
				state.lastReason = decision.reason
			}
			state.verdict = decision.classifierVerdict
		}
	}
	s.npMu.Lock()
	s.np[userID] = state
	s.npMu.Unlock()
}

// currentlyPlaying is one /v1/me/player/currently-playing result.
type currentlyPlaying struct {
	track     model.Track
	isPlaying bool
}

// fetchCurrentlyPlaying fetches the user's currently-playing track.
// (nil, 0, nil) means nothing is playing (HTTP 204 or no item).
// A 401 is returned with its code so the caller can drop the token.
func (s *Service) fetchCurrentlyPlaying(ctx context.Context, accessToken string) (*currentlyPlaying, int, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.apiBase+"/v1/me/player/currently-playing", nil)
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Accept", "application/json")
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNoContent {
		return nil, 0, nil
	}
	if resp.StatusCode == http.StatusUnauthorized {
		return nil, http.StatusUnauthorized, fmt.Errorf("spotify: currently-playing returned 401")
	}
	if resp.StatusCode != http.StatusOK {
		return nil, resp.StatusCode, fmt.Errorf("spotify: currently-playing returned HTTP %d", resp.StatusCode)
	}
	var data struct {
		IsPlaying bool `json:"is_playing"`
		Item      *struct {
			spotifyTrackJSON
		} `json:"item"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return nil, 0, err
	}
	if data.Item == nil || data.Item.Name == "" {
		return nil, 0, nil
	}
	track := spotifyTrackFromJSON(data.Item.spotifyTrackJSON)
	return &currentlyPlaying{track: track, isPlaying: data.IsPlaying}, 0, nil
}

func (s *Service) handleWatch(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Action string `json:"action"`
		UserID *int64 `json:"user_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.UserID == nil || *body.UserID == 0 {
		return nil, badRequest("user_id is required")
	}
	userID := *body.UserID
	switch body.Action {
	case "start":
		// A watch without a token would poll nothing; answer
		// honestly instead of storing a dead watch.
		if _, found, err := s.loadUserToken(userID); err != nil {
			return nil, internalErr("Spotify token load failed: %v", err)
		} else if !found {
			return nil, notFound("no Spotify token — log in first")
		}
		if err := s.store.SetWatch(userID, true); err != nil {
			return nil, internalErr("Spotify watch store failed: %v", err)
		}
		s.ensurePoller()
		return map[string]any{"watching": true, "user_id": userID}, nil
	case "stop":
		was, err := s.store.IsWatching(userID)
		if err != nil {
			return nil, internalErr("Spotify watch lookup failed: %v", err)
		}
		if err := s.store.SetWatch(userID, false); err != nil {
			return nil, internalErr("Spotify watch store failed: %v", err)
		}
		return map[string]any{"watching": false, "was_watching": was, "user_id": userID}, nil
	default:
		return nil, badRequest("unknown watch action %q (want start|stop)", body.Action)
	}
}

func (s *Service) handleWatches(ctx context.Context, req uds.Request) (any, error) {
	ids, err := s.store.ListWatches()
	if err != nil {
		return nil, internalErr("Spotify watches lookup failed: %v", err)
	}
	if ids == nil {
		ids = []int64{}
	}
	// ipc.json says watches is [UserId]; the inventory's
	// [{user_id, started_at}] shape cannot be served because the
	// store records no started_at for a watch.
	return map[string]any{"watches": ids}, nil
}

func (s *Service) handleNowPlaying(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		UserID *int64 `json:"user_id"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.UserID == nil || *body.UserID == 0 {
		if req.HasUserID && req.UserID != 0 {
			body.UserID = &req.UserID
		} else {
			return nil, badRequest("user_id is required")
		}
	}
	userID := *body.UserID
	s.npMu.Lock()
	state, ok := s.np[userID]
	s.npMu.Unlock()
	// Last polled state only — a live fetch is NOT done here (the
	// poller cadence is the freshness contract).
	if !ok || state == nil {
		return map[string]any{
			"playing": false, "user_id": userID, "track": nil,
			"checked_at": 0, "last_skip": false, "last_reason": nil,
			"classifier_verdict": "unknown",
			"note":               "user is not watched or has never been polled",
		}, nil
	}
	var track any
	if state.track != nil {
		track = *state.track
	}
	return map[string]any{
		"playing": state.playing, "user_id": userID, "track": track,
		"checked_at": state.checkedAt, "last_skip": state.lastSkip,
		"last_reason":        state.lastReason,
		"classifier_verdict": state.verdict,
	}, nil
}
