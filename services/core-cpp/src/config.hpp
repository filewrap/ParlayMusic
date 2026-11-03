// config.hpp — configuration from environment names only.
// No secret VALUES are ever stored here or anywhere in the repo; every
// credential is read at process start from the environment (see .env.example
// at the Parlay root / docker-compose.yml for the full list).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace parlay {

// Directory holding the running executable (release layout: bin/
// next to lib/), "" when it cannot be determined. The voice backends
// resolve libtdjson.so / libntgcalls.so relative to it, so a release
// install needs no explicit lib paths.
std::string exeDir();

struct Config {
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

    // Playback limits (NoxBeats config.cpp defaults).
    int duration_limit_min = 60;             // DURATION_LIMIT (minutes)
    int queue_limit = 20;                    // QUEUE_LIMIT
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

    static Config fromEnv();
    // Names of the variables that must be set for the bot to start.
    std::vector<std::string> missingRequired() const;
    // One-line, secret-free summary for the startup log.
    std::string summary() const;
};

}  // namespace parlay
