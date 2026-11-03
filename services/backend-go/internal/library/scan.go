package library

import (
	"context"
	"crypto/md5"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/uds"
)

// handleScan implements library.scan (§9-E5): triggers a Navidrome
// library rescan (Subsonic startScan) and returns immediately
// (triggered:true — the scan itself is async on the Navidrome side).
// After the scan, the backend refreshes its library count from the
// Subsonic library (same client as download.navidrome). Strings
// (locked): "Library scan triggered." / "Library scan not configured
// (NAVIDROME_USER/NAVIDROME_PASS not set)." / "Library scan failed:
// wrong credentials." / "Library scan failed."
//
// Edge cases: Navidrome env unset -> 200 {triggered: false, error:
// "not configured"} (not an error — the core renders the
// not-configured string); wrong credentials -> 200 {triggered:
// false, error: "wrong credentials"}; server unreachable -> 502
// engine_unavailable "Library scan failed."
func (s *Service) handleScan(ctx context.Context, _ uds.Request) (any, error) {
	client, err := s.newSubsonicClient()
	if err != nil {
		// Env unset: the inventory's not-configured 200.
		return map[string]any{"triggered": false, "error": "not configured"}, nil
	}
	if err := client.startScan(ctx); err != nil {
		if isAuthError(err) {
			return map[string]any{"triggered": false, "error": "wrong credentials"}, nil
		}
		return nil, uds.NewHandlerError(uds.ErrEngineUnavailable, fmt.Sprintf("Library scan failed: %v", err))
	}
	data := map[string]any{"triggered": true}
	// Best-effort refresh of the library count from the server
	// (bounded: one album-list page + per-album song counts). The
	// scan itself is async server-side, so this count is the
	// library as the server reports it right now; a failed count
	// never fails the trigger.
	if n, err := client.countSongs(ctx); err == nil {
		data["tracks_found"] = n
	}
	return data, nil
}

// subsonicClient is the minimal Subsonic client library.scan needs:
// startScan + a bounded library count. Auth mirrors the resolver's
// Navidrome client (token auth md5(password+salt), password fallback
// on 401/403 or Subsonic error 40/41/50) — the same server, the same
// credentials, the same behaviour.
type subsonicClient struct {
	base     string
	user     string
	password string
	authMode string // "token" | "password"
	http     *http.Client
}

// newSubsonicClient builds the scan client from the effective
// [backend] navidrome_* values when the service has config (Q10 —
// the file keys now do what the env vars did), else the
// NAVIDROME_URL / NAVIDROME_USER / NAVIDROME_PASSWORD env vars.
func (s *Service) newSubsonicClient() (*subsonicClient, error) {
	base, user, password := "", "", ""
	if s.deps != nil && s.deps.Config != nil {
		base = strings.TrimRight(strings.TrimSpace(s.deps.Config.Backend.NavidromeURL), "/")
		user = s.deps.Config.Backend.NavidromeUser
		password = s.deps.Config.Backend.NavidromePass
	}
	if base == "" {
		base = strings.TrimRight(strings.TrimSpace(os.Getenv("NAVIDROME_URL")), "/")
	}
	if user == "" {
		user = os.Getenv("NAVIDROME_USER")
	}
	if password == "" {
		password = os.Getenv("NAVIDROME_PASSWORD")
	}
	var missing []string
	if base == "" {
		missing = append(missing, "NAVIDROME_URL")
	}
	if user == "" {
		missing = append(missing, "NAVIDROME_USER")
	}
	if password == "" {
		missing = append(missing, "NAVIDROME_PASSWORD")
	}
	if len(missing) > 0 {
		return nil, fmt.Errorf("Navidrome is not configured; set %s in the environment", strings.Join(missing, ", "))
	}
	return &subsonicClient{base: base, user: user, password: password, authMode: "token", http: &http.Client{Timeout: 30 * time.Second}}, nil
}

func (c *subsonicClient) authParams() map[string]string {
	if c.authMode == "password" {
		return map[string]string{"u": c.user, "p": c.password}
	}
	var saltBytes [8]byte
	_, _ = rand.Read(saltBytes[:])
	salt := hex.EncodeToString(saltBytes[:])
	sum := md5.Sum([]byte(c.password + salt))
	return map[string]string{"u": c.user, "t": hex.EncodeToString(sum[:]), "s": salt}
}

// call performs one Subsonic GET, returning the unwrapped
// subsonic-response root. On 401/403 in token mode it flips to
// password mode (sticky) and retries once.
func (c *subsonicClient) call(ctx context.Context, path string, params map[string]string) (map[string]any, error) {
	for attempt := 0; attempt < 2; attempt++ {
		query := url.Values{}
		query.Set("v", "1.16.1")
		query.Set("c", "parlay")
		query.Set("f", "json")
		for k, v := range c.authParams() {
			query.Set(k, v)
		}
		for k, v := range params {
			query.Set(k, v)
		}
		callURL := c.base + "/rest/" + path + "?" + query.Encode()
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, callURL, nil)
		if err != nil {
			return nil, err
		}
		req.Header.Set("User-Agent", "Parlay/1.0")
		resp, err := c.http.Do(req)
		if err != nil {
			return nil, fmt.Errorf("Navidrome request failed (%s): %w", callURL, err)
		}
		body, rerr := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
		resp.Body.Close()
		if rerr != nil {
			return nil, fmt.Errorf("Navidrome read failed: %w", rerr)
		}
		if (resp.StatusCode == 401 || resp.StatusCode == 403) && c.authMode == "token" {
			c.authMode = "password"
			continue
		}
		if resp.StatusCode != http.StatusOK {
			return nil, fmt.Errorf("Navidrome HTTP %d from %s", resp.StatusCode, callURL)
		}
		var parsed struct {
			Response map[string]any `json:"subsonic-response"`
		}
		if err := json.Unmarshal(body, &parsed); err != nil {
			return nil, fmt.Errorf("Navidrome %s: bad JSON: %w", path, err)
		}
		root := parsed.Response
		if root == nil {
			return nil, fmt.Errorf("Navidrome %s: no subsonic-response", path)
		}
		if status, _ := root["status"].(string); status == "failed" {
			errObj, _ := root["error"].(map[string]any)
			code := 0
			if f, ok := errObj["code"].(float64); ok {
				code = int(f)
			}
			if code == 40 || code == 41 || code == 50 {
				return nil, &authFailureError{fmt.Sprintf("Navidrome %s: [%d] %v", path, code, errObj["message"])}
			}
			return nil, fmt.Errorf("Navidrome %s: [%v] %v", path, errObj["code"], errObj["message"])
		}
		return root, nil
	}
	return nil, fmt.Errorf("Navidrome auth rejected in both token and password modes")
}

// authFailureError marks a wrong-credentials failure (the 200
// "wrong credentials" response, not a 502).
type authFailureError struct{ msg string }

func (e *authFailureError) Error() string { return e.msg }

func isAuthError(err error) bool {
	var authErr *authFailureError
	if errors.As(err, &authErr) {
		return true
	}
	return strings.Contains(err.Error(), "auth rejected in both token and password modes")
}

// startScan triggers the server-side library rescan.
func (c *subsonicClient) startScan(ctx context.Context) error {
	_, err := c.call(ctx, "startScan", nil)
	return err
}

// countSongs counts the server library's songs, bounded: one
// getAlbumList2 page (100 albums, alphabetical) + one getAlbum per
// album. It is a best-effort refresh count, not an exact census.
func (c *subsonicClient) countSongs(ctx context.Context) (int, error) {
	root, err := c.call(ctx, "getAlbumList2", map[string]string{"type": "alphabeticalByName", "size": strconv.Itoa(albumPageSize)})
	if err != nil {
		return 0, err
	}
	albums := albumListFromRoot(root["albumList2"])
	total := 0
	for _, album := range albums {
		if err := ctx.Err(); err != nil {
			return total, err
		}
		aroot, err := c.call(ctx, "getAlbum", map[string]string{"id": album})
		if err != nil {
			continue
		}
		songs := songsFromAlbumRoot(aroot["album"])
		total += len(songs)
	}
	return total, nil
}

func albumListFromRoot(node any) []string {
	m, ok := node.(map[string]any)
	if !ok {
		return nil
	}
	raw, ok := m["album"].([]any)
	if !ok {
		return nil
	}
	var out []string
	for _, item := range raw {
		if sm, ok := item.(map[string]any); ok {
			if id, ok := sm["id"].(string); ok && id != "" {
				out = append(out, id)
			}
		}
	}
	return out
}

func songsFromAlbumRoot(node any) []map[string]any {
	m, ok := node.(map[string]any)
	if !ok {
		return nil
	}
	raw, ok := m["song"].([]any)
	if !ok {
		return nil
	}
	var out []map[string]any
	for _, item := range raw {
		if sm, ok := item.(map[string]any); ok {
			out = append(out, sm)
		}
	}
	return out
}

// albumPageSize bounds the scan refresh enumeration (one page).
const albumPageSize = 100
