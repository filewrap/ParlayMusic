// Package config loads the unified parlay.toml (FINAL-ARCHITECTURE.md
// §1.4, OD-6: TOML) plus environment overrides. It reads the SAME
// parlay.toml the C++ core reads, with the SAME keys and the SAME
// validation ranges — the two parsers must agree, or one side silently
// runs on different limits than the other.
//
// Precedence: built-in defaults < parlay.toml < environment overrides.
// A missing file is NOT an error (honest defaults, recorded in
// Source). A malformed file fails loudly: the error names the exact
// key and reason, never a silent default. An explicit $PARLAY_CONFIG
// that cannot be read is an error.
//
// Backend keys live in the optional [backend] table and in env vars.
// That is safe for the shared file: the core's parser only validates
// keys inside the tables it owns and ignores tables it does not know
// (config.cpp: "parlay.toml is shared with the Go backend and other
// components, which own their own sections"). Secrets are NEVER in
// the file — API keys, tokens and the Spotify OAuth key come from
// the environment (or the [backend] table for non-secret endpoints
// only); nothing here hardcodes a credential.
package config

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/BurntSushi/toml"
)

// RateLimitConfig is one token bucket (capacity = burst, refill =
// sustained rate per second).
type RateLimitConfig struct {
	Capacity     int     `toml:"capacity"`
	RefillPerSec float64 `toml:"refill_rate_per_sec"`
}

// TimeoutConfig holds the per-action budgets in milliseconds.
type TimeoutConfig struct {
	ResolveMs  int `toml:"resolve_ms"`
	JoinMs     int `toml:"join_ms"`
	IPCCmdMs   int `toml:"ipc_cmd_ms"`
	DownloadMs int `toml:"download_ms"`
	HealthzMs  int `toml:"healthz_ms"`
}

// QualityConfig holds the voice profile, resolution ladder and
// resampler choice, plus the explicit Opus encoder keys. The
// explicit keys are pointers: nil means "not set in the file"
// and the value falls through to the voice_profile preset (for
// the bitrate) or the built-in default. A set key always wins
// over the preset. Env overrides exist only for voice_profile
// / resampler_quality / source_ladder, not for these keys.
type QualityConfig struct {
	VoiceProfile     string   `toml:"voice_profile"`
	SourceLadder     []string `toml:"source_ladder"`
	ResamplerQuality string   `toml:"resampler_quality"`
	// OpusBitrateBps is the explicit encoder bitrate in bits
	// per second (192000 = 192 kbps). Nil = use the
	// voice_profile preset.
	OpusBitrateBps *int `toml:"opus_bitrate_bps"`
	// OpusFrameMs is the explicit encoder frame size in ms
	// (one of 10, 20, 40, 60). Nil = the built-in default
	// (20 ms); the voice profile sets no frame size.
	OpusFrameMs *int `toml:"opus_frame_ms"`
	// OpusComplexity is the explicit Opus complexity (0-10).
	// Nil = the built-in default (10).
	OpusComplexity *int `toml:"opus_complexity"`
	// OpusVBR / OpusFEC / OpusDTX are the explicit VBR, FEC
	// and DTX switches. Nil = the built-in defaults (VBR on,
	// FEC off, DTX off).
	OpusVBR *bool `toml:"opus_vbr"`
	OpusFEC *bool `toml:"opus_fec"`
	OpusDTX *bool `toml:"opus_dtx"`
}

// Opus encoder bounds for the explicit [quality] keys. These
// MUST equal voice.OpusMinBitrateBps / OpusMaxBitrateBps /
// OpusMinComplexity / OpusMaxComplexity and the frame-size set
// voice accepts (10, 20, 40, 60): this package must stay
// cgo-free, so the bounds are duplicated here instead of
// imported, and internal/quality's TestConfigBoundsAgree
// fails the build if the two sets ever drift apart.
const (
	OpusMinBitrateBps = 6000
	OpusMaxBitrateBps = 510000
	OpusMinComplexity = 0
	OpusMaxComplexity = 10
)

// Opus field names, shared by config.OpusSettings Sources and
// the quality resolver's per-field source reporting.
const (
	OpusFieldBitrateBps = "bitrate_bps"
	OpusFieldComplexity = "complexity"
	OpusFieldVBR        = "vbr"
	OpusFieldFEC        = "fec"
	OpusFieldDTX        = "dtx"
	OpusFieldFrameMs    = "frame_ms"
)

// OpusSettings is the resolved [quality] encoder config at the
// file level: explicit keys win over the voice_profile preset,
// which wins over the built-in default (192000 bps, 20 ms,
// complexity 10, VBR on, FEC off, DTX off, 48000 Hz — the
// same default as voice.DefaultOpusEncoderConfig). Sources
// names, per field, which level set it ("config", "profile"
// or "default"); the quality resolver overlays the per-chat
// store override on top of this and reports "store" for the
// fields it sets. MaxPlaybackRateHz has no explicit key and
// is always 48000 (fullband).
type OpusSettings struct {
	BitrateBps        int
	Complexity        int
	VBR               bool
	FEC               bool
	DTX               bool
	FrameMs           int
	MaxPlaybackRateHz int
	Sources           map[string]string
}

// OpusSettings resolves the file-level Opus encoder config:
// explicit [quality] keys first, then the voice_profile
// preset (bitrate only — the profile sets no other field),
// then the built-in default.
func (q QualityConfig) OpusSettings() OpusSettings {
	s := OpusSettings{
		BitrateBps:        q.OpusBitrateKbps() * 1000,
		Complexity:        10,
		VBR:               true,
		FEC:               false,
		DTX:               false,
		FrameMs:           20,
		MaxPlaybackRateHz: 48000,
		Sources: map[string]string{
			OpusFieldBitrateBps: "profile",
			OpusFieldComplexity: "default",
			OpusFieldVBR:        "default",
			OpusFieldFEC:        "default",
			OpusFieldDTX:        "default",
			OpusFieldFrameMs:    "default",
		},
	}
	if q.OpusBitrateBps != nil {
		s.BitrateBps = *q.OpusBitrateBps
		s.Sources[OpusFieldBitrateBps] = "config"
	}
	if q.OpusComplexity != nil {
		s.Complexity = *q.OpusComplexity
		s.Sources[OpusFieldComplexity] = "config"
	}
	if q.OpusVBR != nil {
		s.VBR = *q.OpusVBR
		s.Sources[OpusFieldVBR] = "config"
	}
	if q.OpusFEC != nil {
		s.FEC = *q.OpusFEC
		s.Sources[OpusFieldFEC] = "config"
	}
	if q.OpusDTX != nil {
		s.DTX = *q.OpusDTX
		s.Sources[OpusFieldDTX] = "config"
	}
	if q.OpusFrameMs != nil {
		s.FrameMs = *q.OpusFrameMs
		s.Sources[OpusFieldFrameMs] = "config"
	}
	return s
}

// OpusBitrateKbps returns the target Opus bitrate implied by the
// voice profile (§1.4): ultra_320k 320, high_256k 256, standard_192k
// 192. It mirrors Config::opusBitrateKbps in the C++ core.
func (q QualityConfig) OpusBitrateKbps() int {
	switch q.VoiceProfile {
	case "ultra_320k":
		return 320
	case "high_256k":
		return 256
	default:
		return 192
	}
}

// LimitConfig holds the operational caps (all configurable — the
// locked §1.2 prohibition on hardcoded limits covers exactly these).
type LimitConfig struct {
	MaxDurationMinutes  int `toml:"max_duration_minutes"`
	MaxQueueSize        int `toml:"max_queue_size"`
	PrefetchLeadTimeSec int `toml:"prefetch_lead_time_sec"`
	URLCacheTTLsec      int `toml:"url_cache_ttl_sec"`
}

// BackendConfig holds backend-only settings. Endpoint values may come
// from the [backend] table; secrets (passwords, ARL cookie, music_u,
// client secret, Gemini key, Spotify OAuth key) come from the
// environment in production — the file form exists only so a local
// dev setup can spell them out without env vars, and must never be
// committed with real values.
type BackendConfig struct {
	DataDir string `toml:"data_dir"`
	UDSPath string `toml:"uds_path"`
	// UDSPathFromEnv records that UDSPath came from PARLAY_UDS_PATH
	// (not the file): the core reads the same env var and it wins
	// over [ipc] socket_path there too, so the two sides agree even
	// when the file keys differ.
	UDSPathFromEnv      bool   `toml:"-"`
	SlskdURL            string `toml:"slskd_url"`
	DeezerARL           string `toml:"deezer_arl"`
	NetEaseMusicU       string `toml:"netease_music_u"`
	SpotifyClientID     string `toml:"spotify_client_id"`
	SpotifyClientSecret string `toml:"spotify_client_secret"`
	SpotifyRedirectURI  string `toml:"spotify_redirect_uri"`
	SpotifyOAuthKey     string `toml:"spotify_oauth_key"`
	NavidromeURL        string `toml:"navidrome_url"`
	NavidromeUser       string `toml:"navidrome_user"`
	NavidromePass       string `toml:"navidrome_pass"`
	GeminiAPIKey        string `toml:"gemini_api_key"`
	// AuddAPIToken is the AudD music-recognition API token
	// (audio.recognize, Phase 3 Wave 2). Empty = recognition is not
	// configured and the action answers an honest 502; the token is
	// never logged. Env override AUDD_API_TOKEN.
	AuddAPIToken string `toml:"audd_api_token"`
	// HTTPAddr is the listen address of the backend's HTTP server
	// (Phase 6): the TypeScript Mini App static bundle, the
	// /api/v1 state + control endpoints, and the FLAC streaming
	// endpoint. Default 127.0.0.1:8081 — loopback only. Empty
	// disables the HTTP server entirely. This is the ONLY gate
	// the HTTP surface has today: there is no Telegram WebApp
	// initData validation in-tree (the bot token lives in the
	// core, not the backend), so a non-loopback address must sit
	// behind a reverse proxy the operator trusts, and the gap is
	// a live-test item (see internal/httpapi). Env override
	// PARLAY_HTTP_ADDR.
	HTTPAddr string `toml:"http_addr"`
	// MiniAppDir is the directory holding the built Mini App
	// bundle (services/miniapp/dist). Empty = auto-search
	// (services/miniapp/dist under the working directory, then
	// next to the backend binary). When no bundle is found the
	// HTTP server still runs the API + stream endpoints and
	// answers / with an honest 503 naming the missing build.
	// Env override PARLAY_MINIAPP_DIR.
	MiniAppDir string `toml:"miniapp_dir"`
}

// IPCConfig is the core-owned [ipc] table (Q6, Phase 4): the UDS
// socket the core uses to reach the backend. The backend parses it
// only so the shared parlay.toml loads cleanly under both parsers —
// the core ignores tables it does not own, and the Go parser must do
// the same for core-owned tables. The value is NOT dead: SocketPath
// falls back to it when [backend] uds_path is unset, so a socket
// path set only in [ipc] is honoured by both sides instead of
// splitting them (the core never reads [backend]).
type IPCConfig struct {
	SocketPath string `toml:"socket_path"`
}

// Config is the whole effective configuration.
type Config struct {
	RateLimit struct {
		Chat RateLimitConfig `toml:"chat"`
		User RateLimitConfig `toml:"user"`
	} `toml:"rate_limit"`
	Timeouts TimeoutConfig `toml:"timeouts"`
	Quality  QualityConfig `toml:"quality"`
	Limits   LimitConfig   `toml:"limits"`
	Backend  BackendConfig `toml:"backend"`
	IPC      IPCConfig     `toml:"ipc"`

	// Source is the parlay.toml path that was loaded, or the
	// "(defaults: ...)" note when no file was found. FileFound
	// reports whether a file was loaded at all.
	Source    string `toml:"-"`
	FileFound bool   `toml:"-"`
}

// SocketPath returns the UDS path the backend listens on
// (contracts/uds-protocol.md §1). Precedence — the two parsers must
// agree, or the core connects where the backend does not listen:
// PARLAY_UDS_PATH env (folded into Backend.UDSPath by the env pass)
// wins; then [backend] uds_path, the backend's own key; then [ipc]
// socket_path, the core's key (Q6 — the core never reads [backend],
// so a path set only in [ipc] must still be honoured here); else
// <data_dir>/parlay-backend.sock. Setting [backend] uds_path and
// [ipc] socket_path to DIFFERENT values is a misconfiguration: the
// backend listens on the former while the core connects to the
// latter. Set at most one file key (prefer [ipc]).
func (c *Config) SocketPath() string {
	if c.Backend.UDSPath != "" {
		return c.Backend.UDSPath
	}
	if c.IPC.SocketPath != "" {
		return c.IPC.SocketPath
	}
	return filepath.Join(c.Backend.DataDir, "parlay-backend.sock")
}

// SocketPathSplitWarning reports the [backend] uds_path vs [ipc]
// socket_path split hazard (Phase 4 §6 item 3, warned at startup
// since Phase 6): the core never reads [backend] uds_path, so when
// that key is set in the FILE and differs from the path the core will
// use ([ipc] socket_path, else <data_dir>/parlay-backend.sock), the
// backend listens where the core never connects and every IPC call
// fails. Empty when the setup is consistent (no [backend] key, both
// keys agree, or the path came from PARLAY_UDS_PATH — the core reads
// the same env var and it wins over [ipc] socket_path there too, so
// the two sides agree; this assumes the core runs with the same env,
// a split env is invisible to this check). It is a warning, not a
// fatal error: a split setup still starts (the operator may run the
// backend standalone), but the startup log names both paths so the
// split is visible instead of silent.
func (c *Config) SocketPathSplitWarning() string {
	if c.Backend.UDSPath == "" {
		return ""
	}
	if c.Backend.UDSPathFromEnv {
		// Env wins on both sides: no split, whatever the file says.
		return ""
	}
	corePath := c.IPC.SocketPath
	if corePath == "" {
		corePath = filepath.Join(c.Backend.DataDir, "parlay-backend.sock")
	}
	if c.Backend.UDSPath == corePath {
		return ""
	}
	return fmt.Sprintf("SOCKET PATH SPLIT: backend listens on [backend] uds_path %q but the core connects to %q ([ipc] socket_path or the default) — set at most one socket path (prefer [ipc]) or make them agree; until then the core cannot reach this backend", c.Backend.UDSPath, corePath)
}

// DBPath returns the embedded store path:
// <data_dir>/parlay.db.
func (c *Config) DBPath() string {
	return filepath.Join(c.Backend.DataDir, "parlay.db")
}

// ResolveMiniAppDir returns the directory holding the built Mini
// App bundle, or "" when none is found. An explicit [backend]
// miniapp_dir (or PARLAY_MINIAPP_DIR) wins and is returned
// verbatim — even when it does not exist yet, so a missing bundle
// is reported at request time with the exact configured path
// instead of silently falling back to a different bundle. With
// no explicit dir, the search is: services/miniapp/dist under
// the working directory, then the same path next to the backend
// binary, then <exeDir>/../../miniapp/dist (binary in
// services/backend-go/bin). The first existing directory wins.
func (c *Config) ResolveMiniAppDir() string {
	if strings.TrimSpace(c.Backend.MiniAppDir) != "" {
		return c.Backend.MiniAppDir
	}
	if _, err := os.Stat(filepath.Join("services", "miniapp", "dist")); err == nil {
		return filepath.Join("services", "miniapp", "dist")
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		for _, p := range []string{
			filepath.Join(dir, "services", "miniapp", "dist"),
			filepath.Join(dir, "..", "..", "miniapp", "dist"),
			filepath.Join(dir, "..", "miniapp", "dist"),
		} {
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return ""
}

// defaults returns the built-in configuration, matching
// parlay.toml.example and the C++ core's struct defaults exactly.
func defaults() *Config {
	c := &Config{
		Source: "(defaults: no parlay.toml found; see parlay.toml.example)",
	}
	c.RateLimit.Chat = RateLimitConfig{Capacity: 5, RefillPerSec: 1.0}
	c.RateLimit.User = RateLimitConfig{Capacity: 3, RefillPerSec: 1.0}
	c.Timeouts = TimeoutConfig{ResolveMs: 30000, JoinMs: 30000, IPCCmdMs: 10000, DownloadMs: 300000, HealthzMs: 5000}
	c.Quality = QualityConfig{
		VoiceProfile:     "standard_192k",
		SourceLadder:     []string{"navidrome", "soulseek", "deezer", "netease", "jiosaavn", "youtube", "spotify", "tiktok", "radio"},
		ResamplerQuality: "sinc_256",
	}
	c.Limits = LimitConfig{MaxDurationMinutes: 60, MaxQueueSize: 20, PrefetchLeadTimeSec: 30, URLCacheTTLsec: 120}
	c.Backend = BackendConfig{
		DataDir:  "data",
		SlskdURL: "http://127.0.0.1:5030",
		HTTPAddr: "127.0.0.1:8081",
	}
	return c
}

// knownSources is the ladder vocabulary (FINAL-ARCHITECTURE §1.4).
var knownSources = map[string]bool{
	"navidrome": true, "soulseek": true, "deezer": true,
	"netease": true, "jiosaavn": true, "youtube": true,
	"spotify": true, "tiktok": true, "radio": true,
}

// validate enforces the same ranges the C++ core enforces
// (config.cpp Config::validate): a value outside its range is a
// startup error naming the key, never a silent clamp.
func (c *Config) validate() error {
	fail := func(key, why string) error {
		return fmt.Errorf("parlay.toml: %s: %s", key, why)
	}
	if c.RateLimit.Chat.Capacity < 1 {
		return fail("rate_limit.chat.capacity", "must be an integer >= 1")
	}
	if c.RateLimit.Chat.RefillPerSec <= 0 {
		return fail("rate_limit.chat.refill_rate_per_sec", "must be a number > 0")
	}
	if c.RateLimit.User.Capacity < 1 {
		return fail("rate_limit.user.capacity", "must be an integer >= 1")
	}
	if c.RateLimit.User.RefillPerSec <= 0 {
		return fail("rate_limit.user.refill_rate_per_sec", "must be a number > 0")
	}
	checkTimeout := func(v int, name string, lo, hi int) error {
		if v < lo || v > hi {
			return fail("timeouts."+name, fmt.Sprintf("must be an integer in %d..%d ms, got %d", lo, hi, v))
		}
		return nil
	}
	if err := checkTimeout(c.Timeouts.ResolveMs, "resolve_ms", 100, 3600000); err != nil {
		return err
	}
	if err := checkTimeout(c.Timeouts.JoinMs, "join_ms", 100, 3600000); err != nil {
		return err
	}
	if err := checkTimeout(c.Timeouts.IPCCmdMs, "ipc_cmd_ms", 100, 3600000); err != nil {
		return err
	}
	if err := checkTimeout(c.Timeouts.DownloadMs, "download_ms", 1000, 3600000); err != nil {
		return err
	}
	if err := checkTimeout(c.Timeouts.HealthzMs, "healthz_ms", 100, 3600000); err != nil {
		return err
	}
	switch c.Quality.VoiceProfile {
	case "ultra_320k", "high_256k", "standard_192k":
	default:
		return fail("quality.voice_profile", "must be one of ultra_320k, high_256k, standard_192k")
	}
	switch c.Quality.ResamplerQuality {
	case "sinc_256", "sinc_128", "fast":
	default:
		return fail("quality.resampler_quality", "must be one of sinc_256, sinc_128, fast")
	}
	// Explicit Opus keys: same bounds as voice/opus.go (see
	// the OpusMinBitrateBps comment — this package stays
	// cgo-free, so the numbers are duplicated, not imported).
	if v := c.Quality.OpusBitrateBps; v != nil && (*v < OpusMinBitrateBps || *v > OpusMaxBitrateBps) {
		return fail("quality.opus_bitrate_bps", fmt.Sprintf("must be an integer in %d..%d, got %d", OpusMinBitrateBps, OpusMaxBitrateBps, *v))
	}
	if v := c.Quality.OpusComplexity; v != nil && (*v < OpusMinComplexity || *v > OpusMaxComplexity) {
		return fail("quality.opus_complexity", fmt.Sprintf("must be an integer in %d..%d, got %d", OpusMinComplexity, OpusMaxComplexity, *v))
	}
	if v := c.Quality.OpusFrameMs; v != nil {
		switch *v {
		case 10, 20, 40, 60:
		default:
			return fail("quality.opus_frame_ms", fmt.Sprintf("must be one of 10, 20, 40, 60, got %d", *v))
		}
	}
	if len(c.Quality.SourceLadder) == 0 {
		return fail("quality.source_ladder", "must list at least one source")
	}
	seen := map[string]bool{}
	for _, s := range c.Quality.SourceLadder {
		if !knownSources[s] {
			return fail("quality.source_ladder", fmt.Sprintf("unknown source %q (known: navidrome, soulseek, deezer, netease, jiosaavn, youtube, spotify, tiktok, radio)", s))
		}
		if seen[s] {
			return fail("quality.source_ladder", fmt.Sprintf("duplicate source %q", s))
		}
		seen[s] = true
	}
	if c.Limits.MaxDurationMinutes < 1 || c.Limits.MaxDurationMinutes > 1440 {
		return fail("limits.max_duration_minutes", "must be an integer in 1..1440")
	}
	if c.Limits.MaxQueueSize < 1 || c.Limits.MaxQueueSize > 10000 {
		return fail("limits.max_queue_size", "must be an integer in 1..10000")
	}
	if c.Limits.PrefetchLeadTimeSec < 0 || c.Limits.PrefetchLeadTimeSec > 3600 {
		return fail("limits.prefetch_lead_time_sec", "must be an integer in 0..3600")
	}
	if c.Limits.URLCacheTTLsec < 0 || c.Limits.URLCacheTTLsec > 86400 {
		return fail("limits.url_cache_ttl_sec", "must be an integer in 0..86400")
	}
	if c.Backend.DataDir == "" {
		return fail("backend.data_dir", "must not be empty")
	}
	if c.Backend.HTTPAddr != "" {
		host, port, err := net.SplitHostPort(c.Backend.HTTPAddr)
		if err != nil {
			return fail("backend.http_addr", fmt.Sprintf("must be host:port, got %q: %v", c.Backend.HTTPAddr, err))
		}
		if host != "" && net.ParseIP(host) == nil && host != "localhost" {
			return fail("backend.http_addr", fmt.Sprintf("host %q is not an IP address or localhost", host))
		}
		n, err := strconv.Atoi(port)
		if err != nil || n < 1 || n > 65535 {
			return fail("backend.http_addr", fmt.Sprintf("port %q must be an integer in 1..65535", port))
		}
	}
	return nil
}

// defaultConfigPath mirrors the core's search: $PARLAY_CONFIG, then
// ./parlay.toml, <exeDir>/parlay.toml, <exeDir>/../parlay.toml.
// "" means no file was found.
func defaultConfigPath() string {
	if p := os.Getenv("PARLAY_CONFIG"); p != "" {
		return p
	}
	if _, err := os.Stat("parlay.toml"); err == nil {
		return "parlay.toml"
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		for _, p := range []string{filepath.Join(dir, "parlay.toml"), filepath.Join(dir, "..", "parlay.toml")} {
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return ""
}

// Load loads the effective configuration: defaults <- parlay.toml <
// env overrides. A missing file runs on defaults; a malformed file
// or override fails with the exact key and reason.
func Load() (*Config, error) {
	return LoadFrom(defaultConfigPath())
}

// LoadFrom loads from an explicit path; "" means "search, then
// defaults". An explicit path that cannot be read is an error.
func LoadFrom(path string) (*Config, error) {
	cfg := defaults()
	if path == "" {
		path = defaultConfigPath()
		if path == "" {
			applyEnv(cfg)
			if err := cfg.validate(); err != nil {
				return nil, err
			}
			return cfg, nil
		}
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("parlay.toml: cannot read %s: %w", path, err)
	}
	md, err := toml.Decode(string(raw), cfg)
	if err != nil {
		return nil, fmt.Errorf("parlay.toml %s: %w", path, err)
	}
	if undecoded := md.Undecoded(); len(undecoded) > 0 {
		keys := make([]string, len(undecoded))
		for i, k := range undecoded {
			keys[i] = k.String()
		}
		return nil, fmt.Errorf("parlay.toml %s: unknown key(s): %s", path, strings.Join(keys, ", "))
	}
	cfg.Source = path
	cfg.FileFound = true
	applyEnv(cfg)
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	return cfg, nil
}

// applyEnv applies environment overrides; a set override always wins
// over the file. Names match parlay.toml.example and the core's
// applyEnvOverrides, plus the backend's own service variables.
func applyEnv(c *Config) {
	setInt := func(name string, dst *int) {
		if v, ok := os.LookupEnv(name); ok {
			if n, err := strconv.Atoi(strings.TrimSpace(v)); err == nil {
				*dst = n
			} else {
				*dst = -1 // fails validation with the exact key
			}
		}
	}
	setFloat := func(name string, dst *float64) {
		if v, ok := os.LookupEnv(name); ok {
			if f, err := strconv.ParseFloat(strings.TrimSpace(v), 64); err == nil {
				*dst = f
			} else {
				*dst = -1
			}
		}
	}
	setStr := func(name string, dst *string) {
		if v, ok := os.LookupEnv(name); ok {
			*dst = strings.TrimSpace(v)
		}
	}

	setInt("PARLAY_RATE_LIMIT_CHAT_CAPACITY", &c.RateLimit.Chat.Capacity)
	setFloat("PARLAY_RATE_LIMIT_CHAT_REFILL_PER_SEC", &c.RateLimit.Chat.RefillPerSec)
	setInt("PARLAY_RATE_LIMIT_USER_CAPACITY", &c.RateLimit.User.Capacity)
	setFloat("PARLAY_RATE_LIMIT_USER_REFILL_PER_SEC", &c.RateLimit.User.RefillPerSec)

	setInt("PARLAY_TIMEOUT_RESOLVE_MS", &c.Timeouts.ResolveMs)
	setInt("PARLAY_TIMEOUT_JOIN_MS", &c.Timeouts.JoinMs)
	setInt("PARLAY_TIMEOUT_IPC_CMD_MS", &c.Timeouts.IPCCmdMs)
	setInt("PARLAY_TIMEOUT_DOWNLOAD_MS", &c.Timeouts.DownloadMs)
	setInt("PARLAY_TIMEOUT_HEALTHZ_MS", &c.Timeouts.HealthzMs)

	setStr("PARLAY_VOICE_PROFILE", &c.Quality.VoiceProfile)
	setStr("PARLAY_RESAMPLER_QUALITY", &c.Quality.ResamplerQuality)
	if v, ok := os.LookupEnv("PARLAY_SOURCE_LADDER"); ok {
		var ladder []string
		for _, item := range strings.Split(v, ",") {
			if t := strings.TrimSpace(item); t != "" {
				ladder = append(ladder, t)
			}
		}
		if len(ladder) > 0 {
			c.Quality.SourceLadder = ladder
		}
	}

	setInt("PARLAY_MAX_DURATION_MINUTES", &c.Limits.MaxDurationMinutes)
	if _, ok := os.LookupEnv("PARLAY_MAX_DURATION_MINUTES"); !ok {
		setInt("DURATION_LIMIT", &c.Limits.MaxDurationMinutes)
	}
	setInt("PARLAY_MAX_QUEUE_SIZE", &c.Limits.MaxQueueSize)
	if _, ok := os.LookupEnv("PARLAY_MAX_QUEUE_SIZE"); !ok {
		setInt("QUEUE_LIMIT", &c.Limits.MaxQueueSize)
	}
	setInt("PARLAY_PREFETCH_LEAD_TIME_SEC", &c.Limits.PrefetchLeadTimeSec)
	setInt("PARLAY_URL_CACHE_TTL_SEC", &c.Limits.URLCacheTTLsec)

	// Backend keys.
	// A set-but-empty PARLAY_DATA_DIR means "unset": an empty data
	// dir would fail validation below, while the C++ core treats it
	// as unset and falls back to "data" — the two sides must agree.
	if v, ok := os.LookupEnv("PARLAY_DATA_DIR"); ok && strings.TrimSpace(v) != "" {
		c.Backend.DataDir = strings.TrimSpace(v)
	}
	setStr("PARLAY_UDS_PATH", &c.Backend.UDSPath)
	if strings.TrimSpace(os.Getenv("PARLAY_UDS_PATH")) != "" {
		c.Backend.UDSPathFromEnv = true
	}
	setStr("SLSKD_URL", &c.Backend.SlskdURL)
	setStr("DEEZER_ARL", &c.Backend.DeezerARL)
	setStr("NETEASE_MUSIC_U", &c.Backend.NetEaseMusicU)
	setStr("SPOTIFY_CLIENT_ID", &c.Backend.SpotifyClientID)
	setStr("SPOTIFY_CLIENT_SECRET", &c.Backend.SpotifyClientSecret)
	setStr("SPOTIFY_REDIRECT_URI", &c.Backend.SpotifyRedirectURI)
	setStr("PARLAY_SPOTIFY_OAUTH_KEY", &c.Backend.SpotifyOAuthKey)
	setStr("NAVIDROME_URL", &c.Backend.NavidromeURL)
	setStr("NAVIDROME_USER", &c.Backend.NavidromeUser)
	// The Navidrome password env var is NAVIDROME_PASSWORD (what
	// the resolver reads); NAVIDROME_PASS is a legacy alias from
	// the pre-Phase 4 .env.example — it still works, but the
	// canonical name wins when both are set. A set-but-empty
	// canonical name counts as unset (Q10 name trap).
	if v, ok := os.LookupEnv("NAVIDROME_PASSWORD"); ok && strings.TrimSpace(v) != "" {
		c.Backend.NavidromePass = strings.TrimSpace(v)
	} else if v, ok := os.LookupEnv("NAVIDROME_PASS"); ok {
		c.Backend.NavidromePass = strings.TrimSpace(v)
	}
	setStr("GEMINI_API_KEY", &c.Backend.GeminiAPIKey)
	setStr("AUDD_API_TOKEN", &c.Backend.AuddAPIToken)
	setStr("PARLAY_HTTP_ADDR", &c.Backend.HTTPAddr)
	setStr("PARLAY_MINIAPP_DIR", &c.Backend.MiniAppDir)
}
