// Package resolver implements the native in-process track resolvers of
// the unified Go backend (FINAL-ARCHITECTURE.md §1.3 Part 2, §9 Phase 3).
//
// One Resolver per source, ported from the in-tree services and the refs
// named in BENCHMARKS.md §2 (the LOCKED fastest-resolution strategy per
// source). No subprocess is spawned on any primary path: the only
// subprocess in the whole package is the single persistent yt-dlp daemon
// (scripts/yt_dlp_worker.py), started once at backend boot and spoken to
// over stdin/stdout JSON-RPC lines; it is the fallback when native
// InnerTube hits an age-gated or signature-ciphered edge case.
//
// Every key-gated path fails honestly: when a credential, cookie or
// server is missing the error names exactly what is missing
// (ErrMissingCredential, code "engine_unavailable") and no fake track
// is ever returned. Live tests are gated on PARLAY_LIVE_TEST=1; without
// it only pure unit tests run.
package resolver

import (
	"bytes"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"parlay/backend-go/internal/model"
)

// Track is the shared contract Track (contracts/ipc.json v0.3.1).
type Track = model.Track

// Source identifies a resolver. Values match contracts/ipc.json
// Track.source plus the internal "soulseek"/"tiktok"/"lemonic" lanes.
type Source string

const (
	SourceYouTube   Source = "youtube"
	SourceSpotify   Source = "spotify"
	SourceNetEase   Source = "netease"
	SourceDeezer    Source = "deezer"
	SourceJioSaavn  Source = "jiosaavn"
	SourceTikTok    Source = "tiktok"
	SourceNavidrome Source = "navidrome"
	SourceSoulseek  Source = "soulseek"
	SourceRadio     Source = "radio"
	SourceLyrics    Source = "lyrics"
)

// Resolver resolves one query (URL, id or free text) to a playable
// track and searches a source for metadata lists.
type Resolver interface {
	// Source is the contracts/ipc.json source name this resolver serves.
	Source() Source
	// Resolve returns the single best track for query, with StreamURL
	// set whenever the source can supply a playable URL directly.
	// A Spotify resolve returns metadata only (StreamURL empty) unless
	// the implementation routes audio through the YouTube ladder.
	Resolve(ctx context.Context, query string) (Track, error)
	// Search returns up to limit metadata matches for a free-text query.
	Search(ctx context.Context, query string, limit int) ([]Track, error)
}

// Downloader downloads a resolved track to a file under destDir.
type Downloader interface {
	// Download fetches track to destDir and reports progress through
	// onProgress (nil allowed). It returns the file path, the track
	// (with any fields learned during download filled in) and a run id
	// for download.progress lookups.
	Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error)
}

// DownloadResult is the download.* response shape (contracts/ipc.json).
type DownloadResult struct {
	FilePath  string `json:"file_path"`
	Track     Track  `json:"track"`
	RunID     string `json:"run_id"`
	Oversize  bool   `json:"oversize,omitempty"`
	SizeBytes int64  `json:"size_bytes,omitempty"`
	Peer      string `json:"peer,omitempty"`
	Format    string `json:"format,omitempty"`
}

// Progress is one download.progress snapshot (contracts/ipc.json
// download.progress). Phase mirrors the contract enum.
type Progress struct {
	RunID            string  `json:"run_id"`
	Percentage       float64 `json:"percentage"`
	DoneBytes        int64   `json:"done_bytes"`
	TotalBytes       *int64  `json:"total_bytes"`
	SpeedBytesPerSec float64 `json:"speed_bytes_per_sec"`
	ETASeconds       *int64  `json:"eta_sec"`
	ElapsedSeconds   float64 `json:"elapsed_sec"`
	Phase            string  `json:"phase"` // searching|downloading|uploading|done|failed
}

// ProgressFunc receives progress snapshots during a download.
type ProgressFunc func(Progress)

// ErrMissingCredential marks a key-gated path whose credential,
// cookie or server is absent. It maps to the contract's
// "engine_unavailable" code; Error() names the missing thing.
type ErrMissingCredential struct {
	Source  Source
	Missing string // env var / cookie / server that is absent
	Detail  string
}

func (e *ErrMissingCredential) Error() string {
	if e.Detail != "" {
		return fmt.Sprintf("%s: %s not set: %s", e.Source, e.Missing, e.Detail)
	}
	return fmt.Sprintf("%s: %s not set", e.Source, e.Missing)
}

// IsMissingCredential reports whether err is (or wraps) an
// ErrMissingCredential.
func IsMissingCredential(err error) bool {
	var m *ErrMissingCredential
	return errors.As(err, &m)
}

// EngineUnavailable is a non-credential availability failure
// (upstream down, unparseable response). It also maps to
// "engine_unavailable".
type EngineUnavailable struct {
	Source Source
	Why    string
}

func (e *EngineUnavailable) Error() string {
	return fmt.Sprintf("%s unavailable: %s", e.Source, e.Why)
}

func engineUnavailable(source Source, format string, args ...any) error {
	return &EngineUnavailable{Source: source, Why: fmt.Sprintf(format, args...)}
}

// Registry holds one Resolver per source plus the downloaders.
type Registry struct {
	resolvers   map[Source]Resolver
	downloaders map[Source]Downloader
	ytdlp       *YtDlpClient
}

// NewRegistry builds the full resolver set. The yt-dlp daemon client
// is created lazily: the daemon process itself only starts on first
// use (YouTube fallback or TikTok metadata), never at construction.
func NewRegistry() *Registry {
	r := &Registry{
		resolvers:   map[Source]Resolver{},
		downloaders: map[Source]Downloader{},
	}
	r.ytdlp = NewYtDlpClient()
	yt := NewYouTubeResolver(r.ytdlp)
	js := NewJioSaavnResolver()
	dz := NewDeezerResolver()
	ne := NewNetEaseResolver()
	sp := NewSpotifyResolver(yt)
	tt := NewTikTokResolver(r.ytdlp)
	nv := NewNavidromeResolver()
	sl := NewSoulseekResolver()
	rd := NewRadioResolver()
	for _, res := range []Resolver{yt, js, dz, ne, sp, tt, nv, sl, rd} {
		r.resolvers[res.Source()] = res
	}
	for src, dl := range map[Source]Downloader{
		SourceYouTube:   yt,
		SourceJioSaavn:  js,
		SourceDeezer:    dz,
		SourceNetEase:   ne,
		SourceSpotify:   sp,
		SourceTikTok:    tt,
		SourceNavidrome: nv,
		SourceSoulseek:  sl,
	} {
		r.downloaders[src] = dl
	}
	return r
}

// YtDlp returns the shared persistent yt-dlp daemon client.
func (r *Registry) YtDlp() *YtDlpClient { return r.ytdlp }

// Close stops the persistent yt-dlp daemon if it was started.
func (r *Registry) Close() error {
	if r.ytdlp != nil {
		return r.ytdlp.Close()
	}
	return nil
}

// ResolverFor returns the resolver for a source, or an honest error.
func (r *Registry) ResolverFor(src Source) (Resolver, error) {
	res, ok := r.resolvers[src]
	if !ok {
		return nil, engineUnavailable(src, "no resolver registered for source %q", src)
	}
	return res, nil
}

// Resolve dispatches a play query to the right resolver by shape,
// mirroring vc-rust classify_query_source plus the source-prefixed
// lanes (jsvn:, netease:, deezer:, tiktok:, radio:, soulseek:):
//
//	YouTube URL/id  -> youtube
//	Spotify URL/id  -> spotify (metadata + YouTube ladder audio)
//	JioSaavn id/url -> jiosaavn
//	Deezer URL      -> deezer
//	NetEase URL/id  -> netease
//	TikTok URL      -> tiktok
//	Navidrome id    -> navidrome (navidrome:<id>)
//	radio:<ref>     -> radio (station id/name or raw stream URL)
//	soulseek:<query>-> soulseek
//	anything else   -> youtube search
func (r *Registry) Resolve(ctx context.Context, query string) (Track, error) {
	start := time.Now()
	src := ClassifyQuery(query)
	res, err := r.ResolverFor(src)
	if err != nil {
		return Track{}, err
	}
	track, err := res.Resolve(ctx, query)
	LogResolve(src, start, err)
	return track, err
}

// Search dispatches a search to one source (or all resolvers that
// support free-text search when source is empty).
func (r *Registry) Search(ctx context.Context, source Source, query string, limit int) ([]Track, error) {
	if source != "" {
		res, err := r.ResolverFor(source)
		if err != nil {
			return nil, err
		}
		return res.Search(ctx, query, limit)
	}
	var out []Track
	var errs []string
	for _, src := range []Source{SourceYouTube, SourceJioSaavn, SourceDeezer, SourceNetEase, SourceSpotify, SourceNavidrome, SourceSoulseek} {
		res := r.resolvers[src]
		if res == nil {
			continue
		}
		tracks, err := res.Search(ctx, query, limit)
		if err != nil {
			errs = append(errs, fmt.Sprintf("%s: %v", src, err))
			continue
		}
		out = append(out, tracks...)
	}
	if len(out) == 0 && len(errs) > 0 {
		return nil, engineUnavailable("", "search failed on all sources: %s", strings.Join(errs, "; "))
	}
	return out, nil
}

// Download downloads a resolved track with the downloader for its
// source. Radio tracks have no file to download (the URL is the
// live stream) and return an honest error.
func (r *Registry) Download(ctx context.Context, track Track, destDir string, onProgress ProgressFunc) (DownloadResult, error) {
	if track.Source == string(SourceRadio) {
		return DownloadResult{}, engineUnavailable(SourceRadio, "radio is a live stream, there is no file to download")
	}
	dl, ok := r.downloaders[Source(track.Source)]
	if !ok {
		return DownloadResult{}, engineUnavailable(Source(track.Source), "no downloader for source %q", track.Source)
	}
	if destDir == "" {
		destDir = DefaultDownloadDir()
	}
	return dl.Download(ctx, track, destDir, onProgress)
}

// ClassifyQuery returns the source a play query belongs to, by shape
// only (no network). Free text classifies as YouTube search.
func ClassifyQuery(query string) Source {
	q := strings.TrimSpace(query)
	lower := strings.ToLower(q)
	for _, p := range []struct {
		prefix string
		src    Source
	}{
		{"jsvn:", SourceJioSaavn},
		{"jiosaavn:", SourceJioSaavn},
		{"netease:", SourceNetEase},
		{"deezer:", SourceDeezer},
		{"tiktok:", SourceTikTok},
		{"navidrome:", SourceNavidrome},
		{"radio:", SourceRadio},
		{"soulseek:", SourceSoulseek},
		{"spotify:", SourceSpotify},
		{"youtube:", SourceYouTube},
	} {
		if strings.HasPrefix(lower, p.prefix) {
			return p.src
		}
	}
	if ExtractYouTubeID(q) != "" {
		return SourceYouTube
	}
	if ExtractSpotifyID(q) != "" {
		return SourceSpotify
	}
	if ExtractDeezerURL(q) != "" {
		return SourceDeezer
	}
	if ExtractNetEaseID(q) != 0 {
		return SourceNetEase
	}
	if ParseTikTokURL(q) != "" {
		return SourceTikTok
	}
	return SourceYouTube
}

// LogResolve emits one Phase 1 telemetry line per resolve attempt
// (success or failure), same shape as the engines:
// [telemetry] t_resolve_us=<int> source=<source> engine=go
func LogResolve(source Source, start time.Time, err error) {
	us := time.Since(start).Microseconds()
	if err != nil {
		fmt.Printf("[telemetry] t_resolve_us=%d source=%s engine=go ok=0 error=%s\n", us, source, err)
		return
	}
	fmt.Printf("[telemetry] t_resolve_us=%d source=%s engine=go ok=1\n", us, source)
}

// ParseDuration parses a duration in any shape the upstreams use:
// seconds as a number, seconds as a numeric string, or a "m:ss" /
// "h:mm:ss" clock string (jsvn-tgvc /song returns "4:28"). Returns
// integer seconds, or 0 when unparseable. Ported from vc-ts
// resolvers.js parseDuration — the old Number("4:28") bug returned
// NaN -> 0, so every JioSaavn track lost its duration; this must
// never regress to that.
func ParseDuration(value any) int {
	switch v := value.(type) {
	case nil:
		return 0
	case int:
		if v < 0 {
			return 0
		}
		return v
	case int64:
		if v < 0 {
			return 0
		}
		return int(v)
	case float64:
		if v != v || v < 0 {
			return 0
		}
		return int(v + 0.5)
	case string:
		s := strings.TrimSpace(v)
		if s == "" {
			return 0
		}
		if n, err := strconv.ParseFloat(s, 64); err == nil {
			if n < 0 {
				return 0
			}
			return int(n + 0.5)
		}
		parts := strings.Split(s, ":")
		if len(parts) < 2 || len(parts) > 3 {
			return 0
		}
		nums := make([]float64, len(parts))
		for i, p := range parts {
			n, err := strconv.ParseFloat(strings.TrimSpace(p), 64)
			if err != nil || n < 0 {
				return 0
			}
			nums[i] = n
		}
		if len(nums) == 2 {
			return int(nums[0]*60 + nums[1] + 0.5)
		}
		return int(nums[0]*3600 + nums[1]*60 + nums[2] + 0.5)
	default:
		return ParseDuration(fmt.Sprint(v))
	}
}

// FormatDurationStr renders the IPC-3 human duration — same rendering
// as vc-go / vc-ts / vc-rust / downloaders / netease-go: "M:SS" under
// an hour, "Xh Ym Zs" at/over an hour (zero parts dropped), "∞" when
// the duration is unknown or the track is live.
func FormatDurationStr(sec int, isLive bool) string {
	if isLive || sec <= 0 {
		return "∞"
	}
	if sec < 3600 {
		return fmt.Sprintf("%d:%02d", sec/60, sec%60)
	}
	out := fmt.Sprintf("%dh", sec/3600)
	if m := (sec % 3600) / 60; m > 0 {
		out += fmt.Sprintf(" %dm", m)
	}
	if s := sec % 60; s > 0 {
		out += fmt.Sprintf(" %ds", s)
	}
	return out
}

// SanitizeFilename makes a string safe as a file name: path
// separators and control characters become "_", leading/trailing
// dots and spaces are trimmed, length is capped at 150 runes.
// Ported from downloaders src/util.py sanitize_filename.
func SanitizeFilename(name string) string {
	var b strings.Builder
	for _, r := range name {
		if r == '/' || r == '\\' || r == ':' || r == '*' || r == '?' || r == '"' || r == '<' || r == '>' || r == '|' || r < 0x20 {
			b.WriteRune('_')
			continue
		}
		b.WriteRune(r)
	}
	s := strings.Trim(strings.TrimSpace(b.String()), ". ")
	if s == "" {
		s = "track"
	}
	if len([]rune(s)) > 150 {
		s = string([]rune(s)[:150])
	}
	return s
}

// DataDir returns the Parlay data directory (PARLAY_DATA_DIR, or
// ~/.local/share/parlay). Download targets live under
// <PARLAY_DATA_DIR>/downloads/.
func DataDir() string {
	if d := strings.TrimSpace(os.Getenv("PARLAY_DATA_DIR")); d != "" {
		return d
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return filepath.Join(".", "data")
	}
	return filepath.Join(home, ".local", "share", "parlay")
}

// DefaultDownloadDir is <PARLAY_DATA_DIR>/downloads/.
func DefaultDownloadDir() string {
	return filepath.Join(DataDir(), "downloads")
}

// NewRunID returns a 12-hex progress-run id (contracts/ipc.json
// v0.3.1: every download.* POST response carries run_id).
func NewRunID() string {
	sum := sha1.Sum([]byte(fmt.Sprintf("%d", time.Now().UnixNano())))
	return hex.EncodeToString(sum[:])[:12]
}

// httpClient is the shared proxy-aware HTTP client for resolvers
// (http.DefaultTransport reads HTTPS_PROXY/HTTP_PROXY from the
// environment, which is how the sandbox egress proxy is honoured).
func httpClient(timeout time.Duration) *http.Client {
	return &http.Client{Timeout: timeout}
}

// getJSON performs one GET with a JSON response, decoding into v.
// Non-200 is an error carrying the status and a short body excerpt.
func getJSON(ctx context.Context, url string, headers map[string]string, v any) error {
	return doJSON(ctx, http.MethodGet, url, nil, headers, v)
}

// doJSON performs one HTTP request with an optional JSON body and
// decodes a JSON response into v (v may be nil to skip decoding).
func doJSON(ctx context.Context, method, url string, body io.Reader, headers map[string]string, v any) error {
	req, err := http.NewRequestWithContext(ctx, method, url, body)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "Parlay/1.0")
	req.Header.Set("Accept", "application/json")
	for k, val := range headers {
		req.Header.Set(k, val)
	}
	resp, err := httpClient(30 * time.Second).Do(req)
	if err != nil {
		return fmt.Errorf("HTTP request to %s failed: %w", url, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
	if err != nil {
		return fmt.Errorf("HTTP read from %s failed: %w", url, err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		excerpt := strings.TrimSpace(string(data))
		if len(excerpt) > 200 {
			excerpt = excerpt[:200]
		}
		return fmt.Errorf("HTTP %d from %s: %s", resp.StatusCode, url, excerpt)
	}
	if v == nil {
		return nil
	}
	return decodeJSON(data, v)
}

var errJSONEmpty = errors.New("empty JSON response")

// decodeJSON decodes the first JSON value in data (some upstreams
// intermittently return the same object twice concatenated; a
// streaming decoder tolerates that where Unmarshal does not).
func decodeJSON(data []byte, v any) error {
	if len(strings.TrimSpace(string(data))) == 0 {
		return errJSONEmpty
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	return dec.Decode(v)
}

// strptr returns a pointer to s (for the nullable IPC-3 fields).
func strptr(s string) *string { return &s }

// intptr returns a pointer to n.
func intptr(n int64) *int64 { return &n }

// cleanTitle unescapes the HTML entities JioSaavn/TGVCBot put in
// titles (&quot; -> ").
func cleanTitle(s string) string {
	return strings.ReplaceAll(s, "&quot;", `"`)
}

var nonWordRe = regexp.MustCompile(`[^a-z0-9]+`)

// normWords lowercases and splits text into words (LRCLib match).
func normWords(s string) []string {
	s = strings.ToLower(s)
	s = nonWordRe.ReplaceAllString(s, " ")
	return strings.Fields(s)
}
