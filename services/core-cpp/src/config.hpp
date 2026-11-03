// config.hpp — unified Parlay configuration (FINAL-ARCHITECTURE.md
// §1.4, §8.2 "config.cpp / config.hpp REWRITE", OD-6: TOML).
//
// Sources, in precedence order (later wins):
//   1. Built-in defaults (the values below — they match today's
//      behaviour wherever a value exists today).
//   2. parlay.toml — searched as: $PARLAY_CONFIG, then ./parlay.toml,
//      <exeDir>/parlay.toml, <exeDir>/../parlay.toml. A missing file
//      is NOT an error: the core runs on defaults and records that
//      honestly in config_source / config_file_found.
//   3. Environment overrides (PARLAY_* names; the legacy
//      DURATION_LIMIT / QUEUE_LIMIT are still honoured after the
//      PARLAY_* ones, and are documented as deprecated).
//
// Secrets are NEVER in the TOML file: credentials (BOT_TOKEN,
// API_ID/API_HASH, PARLAY_TOKEN, TDLib login values) come only from
// the environment, exactly as before. No secret VALUES are stored
// here or anywhere in the repo.
//
// Errors are exact, never silent: a malformed TOML file, an unknown
// key inside a core-owned table, a wrong-typed value, an out-of-range
// value, or a malformed env override throws ConfigError naming the
// key, the offending value, and the reason. The old code silently
// fell back to defaults on a bad env int; that behaviour is gone for
// the unified keys.
//
// Protocol/physical constants stay constants (§6-R6) and are NOT in
// config: loop 0-10, volume 0-200, 960-sample frames, 48 kHz.
// Operational values live here: rate limits, per-action timeouts,
// duration/queue caps, cache TTLs, prefetch lead, Opus profile.
//
// Compatibility: Config::fromEnv() is kept (env-only, no file) so
// main.cpp and the existing tests keep working unchanged; Wave 2
// switches startup to Config::load(). duration_limit_min /
// queue_limit are kept as mirrors of limits.max_duration_minutes /
// max_queue_size so dispatcher.cpp (which reads them) needs no edit
// in this wave.
#pragma once

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace parlay {

// Thrown by Config::load/loadFromFile/loadFromText/fromEnv on any
// configuration problem. what() is "config: <key>: <reason> (got
// <value>)" or "config: <file>: <reason>" — exact enough to fix the
// file from the message alone.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message)
        : std::runtime_error(message) {}
};

// Directory holding the running executable (release layout: bin/
// next to lib/), "" when it cannot be determined. The voice backends
// resolve libtdjson.so / libntgcalls.so relative to it, so a release
// install needs no explicit lib paths.
std::string exeDir();

// --- Unified schema (§1.4) -------------------------------------------

// One token bucket: `capacity` tokens may be spent in a burst, then
// the bucket refills at `refill_rate_per_sec` tokens/second.
struct BucketConfig {
    int capacity = 5;                 // burst size (tokens), >= 1
    double refill_rate_per_sec = 1.0;  // sustained rate, > 0
};

// Sustained per-chat 1.0/s is today's 1 cmd/sec behaviour
// (dispatcher.cpp:911); capacity 5 is the new burst allowance.
// Per-user is stricter in burst (3) at the same sustained 1.0/s.
struct RateLimitConfig {
    BucketConfig chat{5, 1.0};
    BucketConfig user{3, 1.0};
};

// Per-action budgets in milliseconds. resolve_ms / join_ms /
// ipc_cmd_ms map today's IPC timeouts: 30 s (ipc post), 30 s (TDLib
// call) and 10 s (vcGet). download_ms 300 s is today's file-download
// budget. healthz_ms 5 s is today's /healthz probe budget.
struct TimeoutConfig {
    int resolve_ms = 30000;
    int join_ms = 30000;
    int ipc_cmd_ms = 10000;
    int download_ms = 300000;
    int healthz_ms = 5000;
};

struct QualityConfig {
    // ultra_320k = 320 kbps, high_256k = 256 kbps,
    // standard_192k = 192 kbps. Default standard_192k: today's
    // ceiling is 128 kbps (ffmpeg -b:a 128k), so 192k is the lowest
    // new profile; ultra_320k becomes the default only after the
    // Phase 6 live VPS bitrate test (OD-1) locks the verified ceiling.
    std::string voice_profile = "standard_192k";
    // Resolution order, Tier 1 lossless first (§1.3): Navidrome,
    // Soulseek, Deezer, NetEase, then Tier 2 JioSaavn, then Tier 3
    // YouTube, Spotify, TikTok, radio.
    std::vector<std::string> source_ladder = {
        "navidrome", "soulseek", "deezer", "netease", "jiosaavn",
        "youtube", "spotify", "tiktok", "radio"};
    // sinc_256 (mastering-grade) | sinc_128 | fast.
    std::string resampler_quality = "sinc_256";

    // Opus target bitrate implied by voice_profile (§1.4
    // opus_bitrate_kbps): 320 / 256 / 192.
    int opusBitrateKbps() const;
};

struct LimitConfig {
    int max_duration_minutes = 60;   // today's DURATION_LIMIT
    int max_queue_size = 20;         // today's QUEUE_LIMIT
    int prefetch_lead_time_sec = 30;  // resolve N+1 this far ahead
    int url_cache_ttl_sec = 120;     // CDN URLs live ~2 min (Phase 1)
};

// Known source names for quality.source_ladder validation.
inline const std::vector<std::string>& knownSources() {
    static const std::vector<std::string> kSources = {
        "navidrome", "soulseek", "deezer", "netease", "jiosaavn",
        "youtube", "spotify", "tiktok", "radio"};
    return kSources;
}

struct Config {
    // Unified schema (defaults above; file + env overrides apply).
    RateLimitConfig rate_limit;
    TimeoutConfig timeouts;
    QualityConfig quality;
    LimitConfig limits;

    // Where the effective config came from, for the startup log:
    // the parlay.toml path that was loaded, or
    // "(defaults: no parlay.toml found; see parlay.toml.example)".
    std::string config_source;
    bool config_file_found = false;

    // Telegram Bot API (required to run).
    std::string bot_token;    // BOT_TOKEN
    int64_t api_id = 0;       // API_ID (MTProto, reserved for the voice path)
    std::string api_hash;     // API_HASH (MTProto, reserved for the voice path)

    // Shared state (reserved for later Mongo/Redis wiring; the core keeps
    // its runtime state in memory today — see README.md).
    std::string mongo_uri;    // MONGO_URI
    std::string redis_url;    // REDIS_URL

    // IPC auth shared with every service (contracts/ipc.json).
    std::string parlay_token;  // PARLAY_TOKEN

    // IPC service base URLs (defaults match docker-compose.yml).
    // NOTE: the 11 HTTP services are deleted by the locked
    // architecture (Phase 3); these URLs are kept until Wave 2
    // removes the HTTP IPC client, and are NOT in parlay.toml.
    std::string vc_go_url = "http://vc-go:8001";       // VC_GO_URL
    std::string vc_ts_url = "http://vc-ts:8002";       // VC_TS_URL
    std::string vc_rust_url = "http://vc-rust:8003";   // VC_RUST_URL
    std::string fx_url = "http://audio-fx-rust:8004";  // FX_URL
    std::string share_url = "http://share-rust:8005";  // SHARE_URL
    std::string queue_url = "http://queue-rust:8006";  // QUEUE_URL
    std::string spotify_url = "http://spotify-rust:8007";  // SPOTIFY_URL
    std::string radio_url = "http://radio-js:8008";    // RADIO_URL
    std::string dl_url = "http://downloaders:8009";    // DL_URL / DOWNLOADERS_URL
    std::string netease_url = "http://netease-go:8010";  // NETEASE_URL

    int port = 8080;  // PORT — core HTTP port (GUI /api surface, Phase 6 A2)

    // Group management (NoxBeats sudo/auth model, ported to env lists).
    int64_t owner_id = 0;                    // OWNER_ID
    std::set<int64_t> sudo_ids;              // SUDO_IDS, comma-sep (seeded into the store)
    int64_t logger_id = 0;                   // LOGGER_ID (log channel)
    std::set<int64_t> allow_chats;           // PARLAY_ALLOW_CHATS, comma-sep
    std::set<int64_t> block_chats;           // PARLAY_BLOCK_CHATS, comma-sep

    // Voice (TinaMusic/NoxBeats assistant model). The assistant session
    // lives in TD_SESSION_DIR (TDLib); there is no Telethon session
    // file and no voice bridge any more (Phase 5: deleted).
    std::set<int64_t> assistant_ids;         // ASSISTANT_IDS, comma-sep
    // NTGCALLS_LIB (explicit libntgcalls.so path). Empty = the dlopen
    // search in voice_call.cpp (release lib/ next to the binary first).
    std::string ntgcalls_lib;
    std::string rtp_sink;                    // VOICE_RTP_SINK host:port for Opus pipeline proof

    // TDLib voice backend (names only — values stay in the environment).
    int64_t td_api_id = 0;          // TD_API_ID (falls back to API_ID)
    std::string td_api_hash;        // TD_API_HASH (falls back to API_HASH)
    // TD_SESSION_DIR (TDLib database dir). Defaults to
    // <PARLAY_DATA_DIR>/tdlib when unset, so a host that only sets
    // TD_API_ID/TD_API_HASH and drops a pre-authorized session dir
    // there needs nothing else for the session path.
    std::string td_session_dir;
    // TDJSON_LIB (explicit libtdjson.so path). Empty = the dlopen
    // search in tdlib_client.cpp (release lib/ next to the binary,
    // then the repo phase4 build output).
    std::string tdlib_lib;
    std::string td_phone;           // TD_PHONE (login phone, if auth needed)
    std::string td_code;            // TD_CODE (login code, if auth needed)
    std::string td_password;        // TD_PASSWORD (2FA password, if auth needed)
    std::string td_email;           // TD_EMAIL (email login, if auth needed)
    std::string td_email_code;      // TD_EMAIL_CODE (email login code)
    std::string td_first_name;      // TD_FIRST_NAME (first registration)
    std::string td_last_name;       // TD_LAST_NAME (first registration)
    bool td_qr = false;             // TD_QR=1 (QR login instead of phone)

    // Local state / logging.
    std::string data_dir = "data";           // PARLAY_DATA_DIR (file-backend state)
    std::string log_file;                    // PARLAY_LOG_FILE (optional stdout/stderr tee)
    std::string telegram_api_base = "https://api.telegram.org";  // TELEGRAM_API_BASE

    // Playback limits — mirrors of limits.max_duration_minutes /
    // limits.max_queue_size, kept so dispatcher.cpp needs no edit in
    // this wave. Always kept in sync by the load paths; do not set
    // them directly.
    int duration_limit_min = 60;
    int queue_limit = 20;
    std::string support_chat;                // SUPPORT_CHAT

    // Phase 9 UI links/images (UI_INVENTORY.md §2/§16: URLs come from
    // Parlay config only, never hardcoded from a ref bot). Empty =
    // the button/row is omitted (source) or the card is text-only
    // (START_IMG / PING_IMG).
    std::string start_img;      // START_IMG (optional /start photo)
    std::string ping_img;       // PING_IMG (optional /ping photo)
    std::string updates_url;    // PARLAY_UPDATES_URL (updates channel)
    std::string support_url;    // PARLAY_SUPPORT_URL (falls back to
                                // https://t.me/<SUPPORT_CHAT>)
    std::string source_url;     // PARLAY_SOURCE_URL (repo, e.g.
                                // https://github.com/filewrap/ParlayMusic)

    std::string long_poll_timeout_sec = "30";

    // Built-in defaults (no file, no env).
    static Config defaults();

    // Full load: defaults <- parlay.toml <- env overrides.
    // Throws ConfigError on a malformed file or override. A missing
    // file is NOT an error (defaults apply, honestly recorded).
    // An explicit $PARLAY_CONFIG that cannot be read IS an error.
    static Config load();

    // Load one file (must exist and parse). Throws ConfigError.
    static Config loadFromFile(const std::string& path);
    // Load from TOML text (tests; `source_name` labels errors).
    static Config loadFromText(const std::string& text,
                               const std::string& source_name);

    // Env-only load (no file): kept for main.cpp and existing tests.
    // Applies the same env overrides and validation as load().
    static Config fromEnv();

    // Config-file search: $PARLAY_CONFIG if set ("" when unset),
    // else the first existing candidate among ./parlay.toml,
    // <exeDir>/parlay.toml, <exeDir>/../parlay.toml; "" when none
    // exists.
    static std::string defaultConfigPath();

    // Throws ConfigError unless every unified value is in range.
    void validate() const;

    // Names of the variables that must be set for the bot to start.
    std::vector<std::string> missingRequired() const;
    // One-line, secret-free summary for the startup log.
    std::string summary() const;
};

}  // namespace parlay
