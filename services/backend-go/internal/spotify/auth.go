// Spotify OAuth (inventory §8.2, ported from spotify-rust auth.rs):
// login / callback / refresh / revoke. Missing SPOTIFY_CLIENT_ID /
// SPOTIFY_CLIENT_SECRET / SPOTIFY_REDIRECT_URI fails ALL four
// sub-actions with engine_unavailable. Scopes are read-only by
// design. The state is "<user_id>-<random16>", stored server-side,
// single-use, 10-minute TTL.
package spotify

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"parlay/backend-go/internal/uds"
)

func newAuthState(userID int64) string {
	raw := make([]byte, 8)
	if _, err := rand.Read(raw); err != nil {
		// crypto/rand failure is fatal to unguessability; fall back
		// to a time-mixed value rather than a predictable state.
		return fmt.Sprintf("%d-%d", userID, time.Now().UnixNano())
	}
	return fmt.Sprintf("%d-%s", userID, hex.EncodeToString(raw))
}

func (s *Service) mintState(userID int64) string {
	state := newAuthState(userID)
	s.stateMu.Lock()
	s.states[state] = authState{userID: userID, expiresAt: time.Now().Add(10 * time.Minute)}
	s.stateMu.Unlock()
	return state
}

// consumeState validates a callback state: unknown, mismatched-user,
// or expired states fail; a state is deleted on first use whether
// the callback succeeds or not.
func (s *Service) consumeState(state string, userID int64) error {
	s.stateMu.Lock()
	entry, ok := s.states[state]
	if ok {
		delete(s.states, state)
	}
	s.stateMu.Unlock()
	if !ok {
		return uds.NewHandlerError(uds.ErrUnauthorized, "invalid state — start login again")
	}
	if entry.userID != userID {
		return uds.NewHandlerError(uds.ErrUnauthorized, "invalid state — start login again")
	}
	if time.Now().After(entry.expiresAt) {
		return uds.NewHandlerError(uds.ErrUnauthorized, "invalid state — start login again")
	}
	return nil
}

func (s *Service) authorizeURL(clientID, redirectURI, state string) string {
	q := url.Values{}
	q.Set("client_id", clientID)
	q.Set("response_type", "code")
	q.Set("redirect_uri", redirectURI)
	q.Set("scope", oauthScopes)
	q.Set("state", state)
	return s.accountsBase + "/authorize?" + q.Encode()
}

// postTokenForm POSTs a token-exchange/refresh form to the accounts
// endpoint with Basic client auth. Non-200 returns a download_failed
// error carrying Spotify's error text (never the secret).
func (s *Service) postTokenForm(ctx context.Context, form url.Values) (map[string]any, error) {
	id, secret, _ := s.creds()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.accountsBase+"/api/token", strings.NewReader(form.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(id+":"+secret)))
	resp, err := s.httpClient.Do(req)
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Spotify token endpoint unreachable: %v", err))
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Spotify token endpoint read failed: %v", err))
	}
	if resp.StatusCode != http.StatusOK {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Spotify token endpoint returned HTTP %d: %s", resp.StatusCode, spotifyErrorText(body)))
	}
	var data map[string]any
	if err := json.Unmarshal(body, &data); err != nil {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, fmt.Sprintf("Spotify token endpoint returned bad JSON: %v", err))
	}
	return data, nil
}

func spotifyErrorText(body []byte) string {
	var data struct {
		Error            string `json:"error"`
		ErrorDescription string `json:"error_description"`
	}
	if err := json.Unmarshal(body, &data); err == nil && data.Error != "" {
		if data.ErrorDescription != "" {
			return data.Error + ": " + data.ErrorDescription
		}
		return data.Error
	}
	text := strings.TrimSpace(string(body))
	if len(text) > 200 {
		text = text[:200]
	}
	if text == "" {
		return "no error detail"
	}
	return text
}

// exchangeCode trades an authorization code for tokens. Both an
// access token AND a refresh token are required (spotify-rust
// token_from_json).
func (s *Service) exchangeCode(ctx context.Context, code, redirectURI string) (*userToken, error) {
	form := url.Values{}
	form.Set("grant_type", "authorization_code")
	form.Set("code", code)
	form.Set("redirect_uri", redirectURI)
	data, err := s.postTokenForm(ctx, form)
	if err != nil {
		return nil, err
	}
	access, _ := data["access_token"].(string)
	refresh, _ := data["refresh_token"].(string)
	if access == "" || refresh == "" {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, "Spotify code exchange returned no access/refresh token")
	}
	expiresIn := float64(3600)
	if v, ok := data["expires_in"].(float64); ok && v > 0 {
		expiresIn = v
	}
	return &userToken{accessToken: access, refreshToken: refresh, expiresAt: time.Now().Unix() + int64(expiresIn)}, nil
}

// refreshUserToken trades a refresh token for a new access token.
// Spotify may or may not return a new refresh token; when it does
// not, the old one is kept.
func (s *Service) refreshUserToken(ctx context.Context, old *userToken) (*userToken, error) {
	form := url.Values{}
	form.Set("grant_type", "refresh_token")
	form.Set("refresh_token", old.refreshToken)
	data, err := s.postTokenForm(ctx, form)
	if err != nil {
		return nil, err
	}
	access, _ := data["access_token"].(string)
	if access == "" {
		return nil, uds.NewHandlerError(uds.ErrDownloadFailed, "Spotify refresh returned no access token")
	}
	refresh := old.refreshToken
	if v, _ := data["refresh_token"].(string); v != "" {
		refresh = v
	}
	expiresIn := float64(3600)
	if v, ok := data["expires_in"].(float64); ok && v > 0 {
		expiresIn = v
	}
	return &userToken{accessToken: access, refreshToken: refresh, expiresAt: time.Now().Unix() + int64(expiresIn)}, nil
}

// refreshIfNeeded loads a user's token and refreshes it when it
// expires within 120 s. On refresh failure the token is DROPPED and
// an unauthorized error is returned (spotify-rust: a dead refresh
// token can never recover; the user must log in again).
func (s *Service) refreshIfNeeded(ctx context.Context, userID int64) (*userToken, error) {
	tok, found, err := s.loadUserToken(userID)
	if err != nil {
		return nil, internalErr("Spotify token load failed: %v", err)
	}
	if !found {
		return nil, notFound("no Spotify token — log in first")
	}
	if !tok.needsRefresh(time.Now().Unix()) {
		return tok, nil
	}
	fresh, err := s.refreshUserToken(ctx, tok)
	if err != nil {
		_, _ = s.deleteUserToken(userID)
		return nil, uds.NewHandlerError(uds.ErrUnauthorized, "Spotify refresh token rejected (invalid_grant); token dropped, log in again")
	}
	if err := s.saveUserToken(userID, fresh); err != nil {
		return nil, internalErr("Spotify token save failed: %v", err)
	}
	return fresh, nil
}

func (s *Service) handleAuth(ctx context.Context, req uds.Request) (any, error) {
	var body struct {
		Action      string `json:"action"`
		UserID      *int64 `json:"user_id"`
		Code        string `json:"code"`
		State       string `json:"state"`
		RedirectURI string `json:"redirect_uri"`
	}
	if err := decodePayload(req, &body); err != nil {
		return nil, err
	}
	if body.UserID == nil || *body.UserID == 0 {
		return nil, badRequest("user_id is required")
	}
	userID := *body.UserID
	if !s.oauthCredsOK() {
		return nil, engineUnavailable("Spotify credentials not configured: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET / SPOTIFY_REDIRECT_URI are not set")
	}
	id, _, redirect := s.creds()
	if body.RedirectURI != "" {
		redirect = body.RedirectURI
	}

	switch body.Action {
	case "login":
		state := s.mintState(userID)
		return map[string]any{
			"authorized":    false,
			"user_id":       userID,
			"authorize_url": s.authorizeURL(id, redirect, state),
			"note":          "complete the URL in a browser, then POST action:callback with the returned code",
		}, nil
	case "callback":
		if body.Code == "" {
			return nil, badRequest("code is required for callback")
		}
		if err := s.consumeState(body.State, userID); err != nil {
			return nil, err
		}
		tok, err := s.exchangeCode(ctx, body.Code, redirect)
		if err != nil {
			return nil, err
		}
		if err := s.saveUserToken(userID, tok); err != nil {
			return nil, internalErr("Spotify token save failed: %v", err)
		}
		return map[string]any{"authorized": true, "user_id": userID, "expires_at": tok.expiresAt}, nil
	case "refresh":
		tok, found, err := s.loadUserToken(userID)
		if err != nil {
			return nil, internalErr("Spotify token load failed: %v", err)
		}
		if !found {
			return nil, notFound("no Spotify token — log in first")
		}
		fresh, err := s.refreshUserToken(ctx, tok)
		if err != nil {
			_, _ = s.deleteUserToken(userID)
			return nil, uds.NewHandlerError(uds.ErrUnauthorized, "Spotify refresh token rejected (invalid_grant); token dropped, log in again")
		}
		if err := s.saveUserToken(userID, fresh); err != nil {
			return nil, internalErr("Spotify token save failed: %v", err)
		}
		return map[string]any{"authorized": true, "user_id": userID, "expires_at": fresh.expiresAt}, nil
	case "revoke":
		was, err := s.deleteUserToken(userID)
		if err != nil {
			return nil, internalErr("Spotify token delete failed: %v", err)
		}
		return map[string]any{"authorized": false, "user_id": userID, "was_authorized": was}, nil
	default:
		return nil, badRequest("unknown auth action %q (want login|callback|refresh|revoke)", body.Action)
	}
}
