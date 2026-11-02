// config.cpp
#include "config.hpp"

#include <cstdlib>
#include <sstream>

namespace parlay {
namespace {

std::string envStr(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

int64_t envInt(const char* name, int64_t fallback = 0) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stoll(v);
    } catch (...) {
        return fallback;
    }
}

std::set<int64_t> envIdSet(const char* name) {
    std::set<int64_t> out;
    std::string raw = envStr(name);
    std::stringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim whitespace.
        size_t b = item.find_first_not_of(" \t\r\n");
        size_t e = item.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        try {
            out.insert(std::stoll(item.substr(b, e - b + 1)));
        } catch (...) {
            // Ignore a malformed entry; the rest of the list still applies.
        }
    }
    return out;
}

}  // namespace

Config Config::fromEnv() {
    Config c;
    c.bot_token = envStr("BOT_TOKEN");
    c.api_id = envInt("API_ID");
    c.api_hash = envStr("API_HASH");
    c.mongo_uri = envStr("MONGO_URI");
    c.redis_url = envStr("REDIS_URL");
    c.parlay_token = envStr("PARLAY_TOKEN");
    c.vc_go_url = envStr("VC_GO_URL", c.vc_go_url);
    c.vc_ts_url = envStr("VC_TS_URL", c.vc_ts_url);
    c.vc_rust_url = envStr("VC_RUST_URL", c.vc_rust_url);
    c.fx_url = envStr("FX_URL", c.fx_url);
    c.share_url = envStr("SHARE_URL", c.share_url);
    c.queue_url = envStr("QUEUE_URL", c.queue_url);
    c.spotify_url = envStr("SPOTIFY_URL", c.spotify_url);
    c.dl_url = envStr("DL_URL", envStr("DOWNLOADERS_URL", c.dl_url));
    c.radio_url = envStr("RADIO_URL", c.radio_url);
    c.netease_url = envStr("NETEASE_URL", c.netease_url);
    c.port = static_cast<int>(envInt("PORT", c.port));
    c.owner_id = envInt("OWNER_ID");
    c.sudo_ids = envIdSet("SUDO_IDS");
    c.logger_id = envInt("LOGGER_ID");
    c.allow_chats = envIdSet("PARLAY_ALLOW_CHATS");
    c.block_chats = envIdSet("PARLAY_BLOCK_CHATS");
    c.assistant_ids = envIdSet("ASSISTANT_IDS");
    c.assistant_session_file = envStr("ASSISTANT_SESSION_FILE");
    c.ntgcalls_lib = envStr("NTGCALLS_LIB");
    c.voice_bridge = envStr("VOICE_BRIDGE");
    c.rtp_sink = envStr("VOICE_RTP_SINK");
    c.td_api_id = envInt("TD_API_ID", c.api_id);
    c.td_api_hash = envStr("TD_API_HASH", c.api_hash);
    c.td_session_dir = envStr("TD_SESSION_DIR");
    c.tdlib_lib = envStr("TDJSON_LIB");
    c.td_phone = envStr("TD_PHONE");
    c.td_code = envStr("TD_CODE");
    c.td_password = envStr("TD_PASSWORD");
    c.data_dir = envStr("PARLAY_DATA_DIR", c.data_dir);
    c.log_file = envStr("PARLAY_LOG_FILE");
    c.telegram_api_base = envStr("TELEGRAM_API_BASE", c.telegram_api_base);
    c.duration_limit_min = static_cast<int>(envInt("DURATION_LIMIT", c.duration_limit_min));
    c.queue_limit = static_cast<int>(envInt("QUEUE_LIMIT", c.queue_limit));
    c.support_chat = envStr("SUPPORT_CHAT");
    return c;
}

std::vector<std::string> Config::missingRequired() const {
    std::vector<std::string> missing;
    if (bot_token.empty()) missing.push_back("BOT_TOKEN");
    if (parlay_token.empty()) missing.push_back("PARLAY_TOKEN");
    return missing;
}

std::string Config::summary() const {
    std::string s = "parlay-core port=" + std::to_string(port);
    s += " vc_go=" + vc_go_url + " vc_ts=" + vc_ts_url + " vc_rust=" + vc_rust_url;
    s += " owner_id=" + (owner_id ? std::to_string(owner_id) : std::string("(unset)"));
    s += " sudo_ids=" + std::to_string(sudo_ids.size());
    s += " logger_id=" + (logger_id ? std::to_string(logger_id) : std::string("(unset)"));
    s += " assistant_ids=" + std::to_string(assistant_ids.size());
    s += " session_file=" + std::string(assistant_session_file.empty() ? "unset" : "set");
    s += " allow_chats=" + std::to_string(allow_chats.size());
    s += " block_chats=" + std::to_string(block_chats.size());
    s += " api_id=" + std::string(api_id ? "set" : "unset");
    s += " tdlib=" + std::string(td_api_id && !td_api_hash.empty() &&
                                         !td_session_dir.empty()
                                     ? "configured"
                                     : "unset");
    s += " mongo=" + std::string(mongo_uri.empty() ? "unset" : "set");
    s += " redis=" + std::string(redis_url.empty() ? "unset" : "set");
    return s;
}

}  // namespace parlay
