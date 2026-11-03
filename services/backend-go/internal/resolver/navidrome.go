// Navidrome resolver — native Go Subsonic REST (BENCHMARKS.md §2,
// locked). Ported from services/downloaders/src/navidrome.py and
// refs/music-bot-navidrome/src/navidrome.py.
//
// Auth: token auth {u, t: md5(password+salt), s: salt, v: 1.16.1,
// c: parlay, f: json}; plain-password auth (p=<password>) is the
// fallback on 401/403 or Subsonic error 40/41/50, sticky per process
// (proven 2026-11-02 against demo.navidrome.org: token auth is
// rejected with HTTP 403 there while p= succeeds).
//
// Endpoints: rest/search3 (then search2) for lookup,
// rest/stream for a transcode stream, rest/download for the original
// file (no transcode). No server in the sandbox — every path is
// env-gated on NAVIDROME_URL / NAVIDROME_USER / NAVIDROME_PASSWORD
// and fails honestly naming the missing var.
package resolver

import (
	"context"
	"crypto/md5"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// NavidromeResolver resolves Navidrome library tracks.
type NavidromeResolver struct {
	mu       sync.Mutex
	authMode string // "token" | "password"; sticky per process
}

// NewNavidromeResolver builds a Navidrome resolver.
func NewNavidromeResolver() *NavidromeResolver {
	return &NavidromeResolver{authMode: "token"}
}

// Source implements Resolver.
func (n *NavidromeResolver) Source() Source { return SourceNavidrome }

// navidromeEnv returns (base, user, password) or the honest
// missing-config error naming every missing var.
func navidromeEnv() (string, string, string, error) {
	base := strings.TrimRight(strings.TrimSpace(os.Getenv("NAVIDROME_URL")), "/")
	user := os.Getenv("NAVIDROME_USER")
	password := os.Getenv("NAVIDROME_PASSWORD")
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
		return "", "", "", &ErrMissingCredential{Source: SourceNavidrome,
			Missing: strings.Join(missing, ", "),
			Detail:  "Navidrome is not configured; set the missing vars in the environment"}
	}
	return base, user, password, nil
}

// authParams builds the Subsonic auth params for the current mode.
func (n *NavidromeResolver) authParams(user, password string) map[string]string {
	n.mu.Lock()
	mode := n.authMode
	n.mu.Unlock()
	if mode == "password" {
		return map[string]string{"u": user, "p": password}
	}
	var saltBytes [8]byte
	_, _ = rand.Read(saltBytes[:])
	salt := hex.EncodeToString(saltBytes[:])
	sum := md5.Sum([]byte(password + salt))
	return map[string]string{"u": user, "t": hex.EncodeToString(sum[:]), "s": salt}
}

// call performs one Subsonic call, returning the raw body bytes.
// On 401/403 in token mode it flips to password mode (sticky) and
// retries once.
func (n *NavidromeResolver) call(ctx context.Context, path string, params map[string]string) ([]byte, error) {
	base, user, password, err := navidromeEnv()
	if err != nil {
		return nil, err
	}
	for attempt := 0; attempt < 2; attempt++ {
		query := url.Values{}
		query.Set("v", "1.16.1")
		query.Set("c", "parlay")
		query.Set("f", "json")
		for k, v := range n.authParams(user, password) {
			query.Set(k, v)
		}
		for k, v := range params {
			query.Set(k, v)
		}
		callURL := base + "/rest/" + path + "?" + query.Encode()
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, callURL, nil)
		if err != nil {
			return nil, err
		}
		req.Header.Set("User-Agent", "Parlay/1.0")
		resp, err := httpClient(30 * time.Second).Do(req)
		if err != nil {
			return nil, fmt.Errorf("Navidrome request failed (%s): %w", callURL, err)
		}
		body, rerr := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
		resp.Body.Close()
		if rerr != nil {
			return nil, fmt.Errorf("Navidrome read failed: %w", rerr)
		}
		if (resp.StatusCode == 401 || resp.StatusCode == 403) && n.authMode == "token" {
			n.mu.Lock()
			n.authMode = "password"
			n.mu.Unlock()
			continue
		}
		if resp.StatusCode != http.StatusOK {
			return nil, fmt.Errorf("Navidrome HTTP %d from %s", resp.StatusCode, callURL)
		}
		return body, nil
	}
	return nil, engineUnavailable(SourceNavidrome, "Navidrome auth rejected in both token and password modes")
}

// unwrap checks a subsonic-response envelope and returns its root.
func navidromeUnwrap(body []byte, path string) (map[string]any, error) {
	var parsed struct {
		Response map[string]any `json:"subsonic-response"`
	}
	if err := decodeJSON(body, &parsed); err != nil {
		return nil, fmt.Errorf("Navidrome %s: bad JSON: %w", path, err)
	}
	root := parsed.Response
	if root == nil {
		return nil, fmt.Errorf("Navidrome %s: no subsonic-response", path)
	}
	if status, _ := root["status"].(string); status == "failed" {
		errObj, _ := root["error"].(map[string]any)
		return nil, fmt.Errorf("Navidrome %s: [%v] %v", path, errObj["code"], errObj["message"])
	}
	return root, nil
}

// Ping proves auth + reachability (rest/ping -> server version).
func (n *NavidromeResolver) Ping(ctx context.Context) (string, error) {
	body, err := n.call(ctx, "ping", nil)
	if err != nil {
		return "", err
	}
	root, err := navidromeUnwrap(body, "ping")
	if err != nil {
		return "", err
	}
	if v, ok := root["serverVersion"].(string); ok && v != "" {
		return v, nil
	}
	if v, ok := root["version"].(string); ok {
		return v, nil
	}
	return "ok", nil
}

// Resolve handles navidrome:<id> or a bare numeric id.
func (n *NavidromeResolver) Resolve(ctx context.Context, query string) (Track, error) {
	q := strings.TrimSpace(query)
	q = strings.TrimPrefix(strings.ToLower(q), "navidrome:")
	if q == "" || strings.ContainsAny(q, " \t/") {
		return Track{}, engineUnavailable(SourceNavidrome, "not a Navidrome song id: %q", query)
	}
	song, err := n.songByID(ctx, q)
	if err != nil {
		return Track{}, err
	}
	return n.songToTrack(ctx, song), nil
}

// songByID fetches one song via rest/getSong.
func (n *NavidromeResolver) songByID(ctx context.Context, id string) (map[string]any, error) {
	body, err := n.call(ctx, "getSong", map[string]string{"id": id})
	if err != nil {
		return nil, err
	}
	root, err := navidromeUnwrap(body, "getSong")
	if err != nil {
		return nil, err
	}
	song, ok := root["song"].(map[string]any)
	if !ok {
		return nil, engineUnavailable(SourceNavidrome, "Navidrome song %s not found", id)
	}
	return song, nil
}

// Search runs rest/search2, falling back to rest/search3.
func (n *NavidromeResolver) Search(ctx context.Context, query string, limit int) ([]Track, error) {
	if limit <= 0 {
		limit = 5
	}
	q := strings.TrimSpace(strings.TrimPrefix(query, "navidrome:"))
	if q == "" {
		return nil, engineUnavailable(SourceNavidrome, "empty search query")
	}
	body, err := n.call(ctx, "search2", map[string]string{"query": q})
	if err != nil {
		return nil, err
	}
	root, err := navidromeUnwrap(body, "search2")
	if err != nil {
		return nil, err
	}
	songs := songsFromSearchResult(root["searchResult2"])
	if len(songs) == 0 {
		body, err = n.call(ctx, "search3", map[string]string{"query": q, "songCount": strconv.Itoa(limit)})
		if err != nil {
			return nil, err
		}
		root, err = navidromeUnwrap(body, "search3")
		if err != nil {
			return nil, err
		}
		songs = songsFromSearchResult(root["searchResult3"])
	}
	if len(songs) > limit {
		songs = songs[:limit]
	}
	tracks := make([]Track, 0, len(songs))
	for _, s := range songs {
		tracks = append(tracks, n.songToTrack(ctx, s))
	}
	return tracks, nil
}

func songsFromSearchResult(node any) []map[string]any {
	m, ok := node.(map[string]any)
	if !ok {
		return nil
	}
	raw, ok := m["song"].([]any)
	if !ok {
		return nil
	}
	out := make([]map[string]any, 0, len(raw))
	for _, item := range raw {
		if sm, ok := item.(map[string]any); ok {
			out = append(out, sm)
		}
	}
	return out
}

// songToTrack maps a Subsonic song dict to a Track. StreamURL is the
// rest/stream URL with the current auth params embedded (the backend
// hands it to parlay-dsp; it is never shown to the user as a link).
func (n *NavidromeResolver) songToTrack(ctx context.Context, song map[string]any) Track {
	id := fmt.Sprint(song["id"])
	title, _ := song["title"].(string)
	if title == "" {
		title = "Unknown"
	}
	artist, _ := song["artist"].(string)
	duration := ParseDuration(song["duration"])
	t := Track{
		ID: id, Title: title, Artist: artist, Source: string(SourceNavidrome),
		DurationSec: int64(duration),
		DurationStr: FormatDurationStr(duration, false),
	}
	base, user, password, err := navidromeEnv()
	if err == nil {
		query := url.Values{}
		query.Set("v", "1.16.1")
		query.Set("c", "parlay")
		query.Set("f", "json")
		for k, v := range n.authParams(user, password) {
			query.Set(k, v)
		}
		query.Set("id", id)
		t.StreamURL = base + "/rest/stream?" + query.Encode()
		t.URL = base + "/rest/download?id=" + url.QueryEscape(id)
	}
	return t
}

// Download fetches the original file via rest/download (no
// transcode) into destDir.
func (n *NavidromeResolver) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	runID := NewRunID()
	base, user, password, err := navidromeEnv()
	if err != nil {
		return DownloadResult{}, err
	}
	suffix := "mp3"
	if track.ID != "" {
		if song, serr := n.songByID(ctx, track.ID); serr == nil {
			if s, ok := song["suffix"].(string); ok && s != "" {
				suffix = strings.ToLower(s)
			}
			if track.Title == "" {
				track = n.songToTrack(ctx, song)
			}
		}
	}
	query := url.Values{}
	query.Set("v", "1.16.1")
	query.Set("c", "parlay")
	query.Set("f", "json")
	for k, v := range n.authParams(user, password) {
		query.Set(k, v)
	}
	query.Set("id", track.ID)
	dlURL := base + "/rest/download?" + query.Encode()
	name := SanitizeFilename(track.Artist+" - "+track.Title) + "." + suffix
	res, err := downloadHTTP(ctx, track, destDir, name, dlURL, runID, onProgress)
	if err != nil {
		return DownloadResult{}, err
	}
	// A Subsonic error is JSON, not audio — detect it by size + sniff.
	if res.SizeBytes < 4096 {
		if data, rerr := readFileHead(res.FilePath, 512); rerr == nil {
			var probe struct {
				Response map[string]any `json:"subsonic-response"`
			}
			if json.Unmarshal(data, &probe) == nil && probe.Response != nil {
				if status, _ := probe.Response["status"].(string); status == "failed" {
					errObj, _ := probe.Response["error"].(map[string]any)
					return DownloadResult{}, fmt.Errorf("Navidrome download: [%v] %v", errObj["code"], errObj["message"])
				}
			}
		}
	}
	return res, nil
}
