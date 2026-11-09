// Package pot fetches YouTube proof-of-origin (PO) tokens from the
// bgutil POT sidecar (services: build/bgutil-ytdlp-pot-provider,
// POST /get_pot on 127.0.0.1:4416) and caches one token per video ID.
//
// Minting mechanism (bgutil-ytdlp-pot-provider 2.0.2, server/src/
// session_manager.ts generatePoToken): the sidecar runs YouTube's
// BotGuard JS in Node, calls Google's GenerateIT endpoint, then
// WebPoMinter.mintAsWebsafeString(contentBinding). The token it
// mints is a WEB token — the BotGuard challenge is a web-client
// challenge — so the innertube_context sent with every mint request
// is the WEB client (name/version duplicated here, not imported
// from resolver, to avoid an import cycle). The content binding is
// the VIDEO ID: yt-dlp's pot/utils.py get_webpo_content_binding
// binds PLAYER/SUBS-context tokens to the video id.
//
// Where the token rides: the Go InnerTube player payload
// (resolver.fetchVideo, serviceIntegrityDimensions.poToken — the
// same field yt-dlp sets in _video.py _extract_player_response)
// and the yt-dlp daemon RPC (params.po_token, "web.player+<token>",
// the --po-token CLI format).
//
// HONEST CAVEATS (2026-11-05): bgutil mints WEB tokens and
// yt-dlp's WEBPO_CLIENTS list does NOT include ANDROID_VR/ANDROID,
// so whether YouTube accepts a web-minted token on an
// ANDROID_VR/ANDROID player request is UNVERIFIED — the sandbox
// check proves attach + still-succeeds, the VPS test is the real
// proof. The bgutil README states a PO token "does not guarantee
// bypassing 403 errors or bot checks" and "does not bypass
// IP-based login restrictions". A missing sidecar never blocks a
// resolve: Token returns "" + error and callers proceed exactly as
// before (one warning line, then silence until the sidecar recovers).
//
// The token VALUE is never logged anywhere in this package.
package pot

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// Sidecar defaults. PARLAY_POT_URL overrides the base URL (no
// trailing slash); empty disables nothing — a missing sidecar just
// means Token fails and callers resolve without a token.
const (
	defaultBaseURL = "http://127.0.0.1:4416"
	// tokenTTL matches bgutil's TOKEN_TTL default (6h). Tokens
	// minted by the sidecar are also cached server-side per
	// content binding, so a cache hit here usually means a cache
	// hit there too.
	tokenTTL = 6 * time.Hour
	// fetchTimeout bounds one mint attempt. A fresh mint runs
	// BotGuard + GenerateIT (seconds, slower the first time);
	// a refused connection fails immediately.
	fetchTimeout = 60 * time.Second
)

// The WEB client identity the token is minted for (bgutil mints
// web tokens; keep in sync with resolver.innerTubeWeb/Ver).
const (
	mintClientName    = "WEB"
	mintClientVersion = "2.20250101.01.00"
)

type cachedToken struct {
	token     string
	expiresAt time.Time
}

// Client is a bgutil /get_pot client with a per-video-ID cache.
type Client struct {
	baseURL string
	http    *http.Client

	mu         sync.Mutex
	cache      map[string]cachedToken
	downLogged bool
}

// NewClient builds a client for the sidecar at PARLAY_POT_URL (or
// the 127.0.0.1:4416 default). It dials nothing until Token is
// called.
func NewClient() *Client {
	base := strings.TrimSpace(os.Getenv("PARLAY_POT_URL"))
	if base == "" {
		base = defaultBaseURL
	}
	return &Client{
		baseURL: strings.TrimRight(base, "/"),
		http:    &http.Client{Timeout: fetchTimeout},
		cache:   map[string]cachedToken{},
	}
}

// Token returns a cached PO token for videoID, minting one through
// the sidecar on a miss. On any failure it returns "" + error; the
// caller resolves without a token.
func (c *Client) Token(ctx context.Context, videoID string) (string, error) {
	return c.token(ctx, videoID, false)
}

// Refresh mints a FRESH token for videoID (bypass_cache: true) and
// replaces the cached one. Callers use it once after a 403 /
// LOGIN_REQUIRED, which is the signature of a stale or rejected
// token.
func (c *Client) Refresh(ctx context.Context, videoID string) (string, error) {
	return c.token(ctx, videoID, true)
}

// FormatPlayer renders a minted token the way yt-dlp expects an
// mweb player-context token: "mweb.player+<token>" (the --po-token CLI
// format; see yt_dlp_worker.py).
func FormatPlayer(token string) string {
	return "mweb.player+" + token
}

func (c *Client) token(ctx context.Context, videoID string, bypassCache bool) (string, error) {
	if videoID == "" {
		return "", fmt.Errorf("pot: empty video id")
	}
	if !bypassCache {
		c.mu.Lock()
		if ent, ok := c.cache[videoID]; ok && time.Now().Before(ent.expiresAt) {
			c.mu.Unlock()
			return ent.token, nil
		}
		c.mu.Unlock()
	}
	tok, err := c.mint(ctx, videoID, bypassCache)
	if err != nil {
		c.logDownOnce(err)
		return "", err
	}
	c.mu.Lock()
	c.cache[videoID] = cachedToken{token: tok, expiresAt: time.Now().Add(tokenTTL)}
	c.downLogged = false
	c.mu.Unlock()
	if bypassCache {
		log.Printf("pot: refreshed token for video %s (bypass_cache)", videoID)
	} else {
		log.Printf("pot: minted token for video %s (cached 6h)", videoID)
	}
	return tok, nil
}

// mint POSTs one /get_pot request. The request body follows
// bgutil server/src/main.ts:114 — content_binding is the video id
// (PLAYER context), innertube_context is the WEB client the
// BotGuard challenge is minted for.
func (c *Client) mint(ctx context.Context, videoID string, bypassCache bool) (string, error) {
	body, err := json.Marshal(map[string]any{
		"content_binding": videoID,
		"bypass_cache":    bypassCache,
		"innertube_context": map[string]any{"client": map[string]any{
			"clientName":    mintClientName,
			"clientVersion": mintClientVersion,
		}},
	})
	if err != nil {
		return "", err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/get_pot", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return "", fmt.Errorf("pot: sidecar %s unreachable: %w", c.baseURL, err)
	}
	defer resp.Body.Close()
	var out struct {
		PoToken string `json:"poToken"`
		Error   string `json:"error"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", fmt.Errorf("pot: sidecar %s bad /get_pot response: %w", c.baseURL, err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("pot: sidecar /get_pot HTTP %d: %s", resp.StatusCode, out.Error)
	}
	if out.Error != "" {
		return "", fmt.Errorf("pot: sidecar /get_pot error: %s", out.Error)
	}
	if out.PoToken == "" {
		return "", fmt.Errorf("pot: sidecar returned an empty token")
	}
	return out.PoToken, nil
}

// logDownOnce warns once while the sidecar is unreachable, then
// stays silent until a mint succeeds again — a dead sidecar must
// not spam one line per resolve.
func (c *Client) logDownOnce(err error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.downLogged {
		return
	}
	c.downLogged = true
	log.Printf("pot: sidecar unreachable, resolving without PO tokens (degraded): %v", err)
}
