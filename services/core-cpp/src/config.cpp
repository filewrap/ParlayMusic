// config.cpp — unified config: parlay.toml (OD-6) + env overrides.
// See config.hpp for the precedence and error contract.
#include "config.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "toml.hpp"

namespace parlay {

std::string exeDir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string path(buf);
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash);
}

namespace {

[[noreturn]] void cfgFail(const std::string& key, const std::string& reason,
                          const std::string& got = "") {
    std::string msg = "config: " + key + ": " + reason;
    if (!got.empty()) msg += " (got '" + got + "')";
    throw ConfigError(msg);
}

// --- legacy lenient env helpers (credentials / service URLs) --------
// These keep the old behaviour (a malformed legacy int silently falls
// back) so existing callers and tests are unaffected. Only the NEW
// unified PARLAY_* keys are strict (below).
std::string envStr(const char* name, const std::string& fallback = "") {
    // A set-but-empty var means "unset" (same rule as envInt below
    // and the PARLAY_DATA_DIR handling in applyLegacyEnv): .env
    // ships TD_API_HASH= empty, and an empty value must fall back
    // to API_HASH instead of shadowing it with "".
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
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

// --- strict env parsing for the unified keys ------------------------
// A set-but-malformed PARLAY_* override is an exact error, never a
// silent fallback to the file/default value. Unset or empty = absent.
bool envPresent(const char* name, std::string& out) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    out = v;
    return true;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int64_t envStrictInt(const char* name) {
    std::string raw;
    if (!envPresent(name, raw)) return 0;  // caller checks presence first
    try {
        size_t used = 0;
        int64_t v = std::stoll(raw, &used, 10);
        if (used != raw.size())
            cfgFail(name, "must be an integer", raw);
        return v;
    } catch (const ConfigError&) {
        throw;
    } catch (...) {
        cfgFail(name, "must be an integer", raw);
    }
}

double envStrictDouble(const char* name) {
    std::string raw;
    if (!envPresent(name, raw)) return 0.0;
    try {
        size_t used = 0;
        double v = std::stod(raw, &used);
        if (used != raw.size())
            cfgFail(name, "must be a number", raw);
        return v;
    } catch (const ConfigError&) {
        throw;
    } catch (...) {
        cfgFail(name, "must be a number", raw);
    }
}

// --- TOML -> Config ---------------------------------------------------
// Typed reads that re-wrap toml errors as ConfigError naming the key.
int64_t tomlInt(const toml::Document& doc, const std::string& path) {
    try {
        return doc.getInteger(path);
    } catch (const toml::ParseError& e) {
        cfgFail(path, std::string("must be an integer: ") + e.what());
    }
}

double tomlNumber(const toml::Document& doc, const std::string& path) {
    try {
        return doc.getNumber(path);
    } catch (const toml::ParseError& e) {
        cfgFail(path, std::string("must be a number: ") + e.what());
    }
}

std::string tomlString(const toml::Document& doc, const std::string& path) {
    try {
        return doc.getString(path);
    } catch (const toml::ParseError& e) {
        cfgFail(path, std::string("must be a string: ") + e.what());
    }
}

std::vector<std::string> tomlStringArray(const toml::Document& doc,
                                         const std::string& path) {
    try {
        return doc.getStringArray(path);
    } catch (const toml::ParseError& e) {
        cfgFail(path, std::string("must be an array of strings: ") +
                         e.what());
    }
}

// Unknown keys inside a core-owned table are typos until proven
// otherwise: reject them. Unknown top-level tables are NOT rejected —
// parlay.toml is shared with the Go backend and other components,
// which own their own sections.
void checkKnownKeys(const toml::Document& doc, const std::string& table,
                    const std::vector<std::string>& allowed) {
    const toml::Value* t = doc.getTable(table);
    if (t == nullptr) return;
    for (const auto& kv : t->table) {
        if (std::find(allowed.begin(), allowed.end(), kv.first) ==
            allowed.end())
            cfgFail(table + "." + kv.first,
                    "unknown key (allowed here: " +
                        [&] {
                            std::string s;
                            for (size_t i = 0; i < allowed.size(); ++i) {
                                if (i) s += ", ";
                                s += allowed[i];
                            }
                            return s;
                        }() +
                        ")");
    }
}

void applyToml(const toml::Document& doc, Config& c) {
    // Root-level bare keys: the schema has none.
    for (const auto& kv : doc.root().table) {
        if (!kv.second.isTable())
            cfgFail(kv.first, "unknown key: bare keys are not allowed "
                              "at the top level; put it in a [table]");
    }
    // [rate_limit] holds only the chat/user subtables.
    checkKnownKeys(doc, "rate_limit", {"chat", "user"});
    checkKnownKeys(doc, "rate_limit.chat", {"capacity", "refill_rate_per_sec"});
    checkKnownKeys(doc, "rate_limit.user", {"capacity", "refill_rate_per_sec"});
    checkKnownKeys(doc, "timeouts",
                   {"resolve_ms", "join_ms", "ipc_cmd_ms", "download_ms",
                    "healthz_ms"});
    checkKnownKeys(doc, "quality",
                   {"voice_profile", "source_ladder", "resampler_quality",
                    "opus_bitrate_bps", "opus_frame_ms", "opus_complexity",
                    "opus_vbr", "opus_fec", "opus_dtx"});
    checkKnownKeys(doc, "limits",
                   {"max_duration_minutes", "max_queue_size",
                    "prefetch_lead_time_sec", "url_cache_ttl_sec"});
    // [ipc] holds the UDS socket path (Q6, Phase 4): the only key.
    checkKnownKeys(doc, "ipc", {"socket_path"});

    if (doc.has("rate_limit.chat.capacity"))
        c.rate_limit.chat.capacity =
            static_cast<int>(tomlInt(doc, "rate_limit.chat.capacity"));
    if (doc.has("rate_limit.chat.refill_rate_per_sec"))
        c.rate_limit.chat.refill_rate_per_sec =
            tomlNumber(doc, "rate_limit.chat.refill_rate_per_sec");
    if (doc.has("rate_limit.user.capacity"))
        c.rate_limit.user.capacity =
            static_cast<int>(tomlInt(doc, "rate_limit.user.capacity"));
    if (doc.has("rate_limit.user.refill_rate_per_sec"))
        c.rate_limit.user.refill_rate_per_sec =
            tomlNumber(doc, "rate_limit.user.refill_rate_per_sec");

    if (doc.has("timeouts.resolve_ms"))
        c.timeouts.resolve_ms = static_cast<int>(tomlInt(doc, "timeouts.resolve_ms"));
    if (doc.has("timeouts.join_ms"))
        c.timeouts.join_ms = static_cast<int>(tomlInt(doc, "timeouts.join_ms"));
    if (doc.has("timeouts.ipc_cmd_ms"))
        c.timeouts.ipc_cmd_ms = static_cast<int>(tomlInt(doc, "timeouts.ipc_cmd_ms"));
    if (doc.has("timeouts.download_ms"))
        c.timeouts.download_ms =
            static_cast<int>(tomlInt(doc, "timeouts.download_ms"));
    if (doc.has("timeouts.healthz_ms"))
        c.timeouts.healthz_ms = static_cast<int>(tomlInt(doc, "timeouts.healthz_ms"));

    if (doc.has("quality.voice_profile"))
        c.quality.voice_profile = tomlString(doc, "quality.voice_profile");
    if (doc.has("quality.source_ladder"))
        c.quality.source_ladder = tomlStringArray(doc, "quality.source_ladder");
    if (doc.has("quality.resampler_quality"))
        c.quality.resampler_quality =
            tomlString(doc, "quality.resampler_quality");

    if (doc.has("limits.max_duration_minutes"))
        c.limits.max_duration_minutes =
            static_cast<int>(tomlInt(doc, "limits.max_duration_minutes"));
    if (doc.has("limits.max_queue_size"))
        c.limits.max_queue_size =
            static_cast<int>(tomlInt(doc, "limits.max_queue_size"));
    if (doc.has("limits.prefetch_lead_time_sec"))
        c.limits.prefetch_lead_time_sec =
            static_cast<int>(tomlInt(doc, "limits.prefetch_lead_time_sec"));
    if (doc.has("limits.url_cache_ttl_sec"))
        c.limits.url_cache_ttl_sec =
            static_cast<int>(tomlInt(doc, "limits.url_cache_ttl_sec"));

    if (doc.has("ipc.socket_path"))
        c.ipc_socket_path = tomlString(doc, "ipc.socket_path");
}

// --- env overrides (win over the file) -------------------------------
void applyEnvOverrides(Config& c) {
    std::string raw;
    // Rate limits.
    if (envPresent("PARLAY_RATE_LIMIT_CHAT_CAPACITY", raw))
        c.rate_limit.chat.capacity =
            static_cast<int>(envStrictInt("PARLAY_RATE_LIMIT_CHAT_CAPACITY"));
    if (envPresent("PARLAY_RATE_LIMIT_CHAT_REFILL_PER_SEC", raw))
        c.rate_limit.chat.refill_rate_per_sec =
            envStrictDouble("PARLAY_RATE_LIMIT_CHAT_REFILL_PER_SEC");
    if (envPresent("PARLAY_RATE_LIMIT_USER_CAPACITY", raw))
        c.rate_limit.user.capacity =
            static_cast<int>(envStrictInt("PARLAY_RATE_LIMIT_USER_CAPACITY"));
    if (envPresent("PARLAY_RATE_LIMIT_USER_REFILL_PER_SEC", raw))
        c.rate_limit.user.refill_rate_per_sec =
            envStrictDouble("PARLAY_RATE_LIMIT_USER_REFILL_PER_SEC");
    // Timeouts.
    if (envPresent("PARLAY_TIMEOUT_RESOLVE_MS", raw))
        c.timeouts.resolve_ms =
            static_cast<int>(envStrictInt("PARLAY_TIMEOUT_RESOLVE_MS"));
    if (envPresent("PARLAY_TIMEOUT_JOIN_MS", raw))
        c.timeouts.join_ms =
            static_cast<int>(envStrictInt("PARLAY_TIMEOUT_JOIN_MS"));
    if (envPresent("PARLAY_TIMEOUT_IPC_CMD_MS", raw))
        c.timeouts.ipc_cmd_ms =
            static_cast<int>(envStrictInt("PARLAY_TIMEOUT_IPC_CMD_MS"));
    if (envPresent("PARLAY_TIMEOUT_DOWNLOAD_MS", raw))
        c.timeouts.download_ms =
            static_cast<int>(envStrictInt("PARLAY_TIMEOUT_DOWNLOAD_MS"));
    if (envPresent("PARLAY_TIMEOUT_HEALTHZ_MS", raw))
        c.timeouts.healthz_ms =
            static_cast<int>(envStrictInt("PARLAY_TIMEOUT_HEALTHZ_MS"));
    // Quality.
    if (envPresent("PARLAY_VOICE_PROFILE", raw))
        c.quality.voice_profile = trim(raw);
    if (envPresent("PARLAY_SOURCE_LADDER", raw)) {
        std::vector<std::string> ladder;
        std::stringstream ss(raw);
        std::string item;
        while (std::getline(ss, item, ',')) {
            std::string t = trim(item);
            if (!t.empty()) ladder.push_back(t);
        }
        if (!ladder.empty()) c.quality.source_ladder = ladder;
    }
    if (envPresent("PARLAY_RESAMPLER_QUALITY", raw))
        c.quality.resampler_quality = trim(raw);
    // Limits (PARLAY_* wins; legacy DURATION_LIMIT / QUEUE_LIMIT are
    // honoured after them, and are deprecated).
    if (envPresent("PARLAY_MAX_DURATION_MINUTES", raw))
        c.limits.max_duration_minutes =
            static_cast<int>(envStrictInt("PARLAY_MAX_DURATION_MINUTES"));
    else if (envPresent("DURATION_LIMIT", raw))
        c.limits.max_duration_minutes =
            static_cast<int>(envStrictInt("DURATION_LIMIT"));
    if (envPresent("PARLAY_MAX_QUEUE_SIZE", raw))
        c.limits.max_queue_size =
            static_cast<int>(envStrictInt("PARLAY_MAX_QUEUE_SIZE"));
    else if (envPresent("QUEUE_LIMIT", raw))
        c.limits.max_queue_size =
            static_cast<int>(envStrictInt("QUEUE_LIMIT"));
    if (envPresent("PARLAY_PREFETCH_LEAD_TIME_SEC", raw))
        c.limits.prefetch_lead_time_sec =
            static_cast<int>(envStrictInt("PARLAY_PREFETCH_LEAD_TIME_SEC"));
    if (envPresent("PARLAY_URL_CACHE_TTL_SEC", raw))
        c.limits.url_cache_ttl_sec =
            static_cast<int>(envStrictInt("PARLAY_URL_CACHE_TTL_SEC"));
    // UDS socket path: env wins over [ipc] socket_path in the file.
    // (envPresent treats set-but-empty as absent, like PARLAY_DATA_DIR.)
    if (envPresent("PARLAY_UDS_PATH", raw)) c.ipc_socket_path = trim(raw);
}

void syncMirrors(Config& c) {
    c.duration_limit_min = c.limits.max_duration_minutes;
    c.queue_limit = c.limits.max_queue_size;
}

// Legacy env fields (credentials, auth lists, TDLib).
// Unchanged from the pre-unification fromEnv(), except that the
// duration/queue limits now flow through limits (above).
void applyLegacyEnv(Config& c) {
    c.bot_token = envStr("BOT_TOKEN");
    c.api_id = envInt("API_ID");
    c.api_hash = envStr("API_HASH");
    c.parlay_token = envStr("PARLAY_TOKEN");
    c.port = static_cast<int>(envInt("PORT", c.port));
    c.owner_id = envInt("OWNER_ID");
    c.sudo_ids = envIdSet("SUDO_IDS");
    c.logger_id = envInt("LOGGER_ID");
    c.allow_chats = envIdSet("PARLAY_ALLOW_CHATS");
    c.block_chats = envIdSet("PARLAY_BLOCK_CHATS");
    c.assistant_ids = envIdSet("ASSISTANT_IDS");
    c.rtp_sink = envStr("VOICE_RTP_SINK");
    c.td_api_id = envInt("TD_API_ID", c.api_id);
    c.td_api_hash = envStr("TD_API_HASH", c.api_hash);
    // A set-but-empty PARLAY_DATA_DIR means "unset": an empty
    // data_dir would otherwise resolve the TDLib session dirs to
    // absolute "/tdlib" / "/tdlib-bot" (TdLibClient::Params::
    // fromEnvFor) while the socket path silently falls back to
    // "data" — the two sides must agree on "data".
    if (std::string dd = envStr("PARLAY_DATA_DIR"); !dd.empty())
        c.data_dir = dd;
    if (std::string tsd = envStr("TD_SESSION_DIR"); !tsd.empty())
        c.td_session_dir = tsd;
    else
        c.td_session_dir = c.data_dir + "/tdlib";
    c.td_phone = envStr("TD_PHONE");
    c.td_code = envStr("TD_CODE");
    c.td_password = envStr("TD_PASSWORD");
    c.td_email = envStr("TD_EMAIL");
    c.td_email_code = envStr("TD_EMAIL_CODE");
    c.td_first_name = envStr("TD_FIRST_NAME");
    c.td_last_name = envStr("TD_LAST_NAME");
    std::string td_qr = envStr("TD_QR");
    c.td_qr = (td_qr == "1" || td_qr == "true" || td_qr == "yes");
    c.log_file = envStr("PARLAY_LOG_FILE");
    c.telegram_api_base = envStr("TELEGRAM_API_BASE", c.telegram_api_base);
    c.support_chat = envStr("SUPPORT_CHAT");
    c.start_img = envStr("START_IMG");
    c.ping_img = envStr("PING_IMG");
    c.updates_url = envStr("PARLAY_UPDATES_URL");
    c.support_url = envStr("PARLAY_SUPPORT_URL");
    if (c.support_url.empty() && !c.support_chat.empty()) {
        std::string sc = c.support_chat;
        if (!sc.empty() && sc[0] == '@') sc = sc.substr(1);
        c.support_url = "https://t.me/" + sc;
    }
    c.source_url = envStr("PARLAY_SOURCE_URL");
}

}  // namespace

int QualityConfig::opusBitrateKbps() const {
    if (voice_profile == "ultra_320k") return 320;
    if (voice_profile == "high_256k") return 256;
    return 192;  // standard_192k (validated by Config::validate)
}

Config Config::defaults() {
    Config c;
    c.config_source = "(defaults: no parlay.toml found; see "
                      "parlay.toml.example)";
    c.config_file_found = false;
    return c;
}

std::string Config::defaultConfigPath() {
    std::string explicit_path = envStr("PARLAY_CONFIG");
    if (!explicit_path.empty()) return explicit_path;
    std::error_code ec;
    if (std::filesystem::exists("parlay.toml", ec)) return "parlay.toml";
    std::string exe = exeDir();
    if (!exe.empty()) {
        std::string p1 = exe + "/parlay.toml";
        if (std::filesystem::exists(p1, ec)) return p1;
        std::string p2 = exe + "/../parlay.toml";
        if (std::filesystem::exists(p2, ec)) return p2;
    }
    return "";
}

Config Config::load() {
    std::string path = defaultConfigPath();
    if (path.empty()) {
        Config c = defaults();
        applyLegacyEnv(c);
        applyEnvOverrides(c);
        syncMirrors(c);
        c.validate();
        return c;
    }
    return loadFromFile(path);
}

Config Config::loadFromFile(const std::string& path) {
    toml::Document doc;
    try {
        doc = toml::parseFile(path);
    } catch (const toml::ParseError& e) {
        throw ConfigError(std::string("config: ") + e.what());
    } catch (const std::runtime_error& e) {
        throw ConfigError(std::string("config: ") + e.what());
    }
    Config c = defaults();
    c.config_source = path;
    c.config_file_found = true;
    applyToml(doc, c);
    applyLegacyEnv(c);
    applyEnvOverrides(c);
    syncMirrors(c);
    c.validate();
    return c;
}

Config Config::loadFromText(const std::string& text,
                            const std::string& source_name) {
    toml::Document doc;
    try {
        doc = toml::parse(text, source_name);
    } catch (const toml::ParseError& e) {
        throw ConfigError(std::string("config: ") + e.what());
    }
    Config c = defaults();
    c.config_source = source_name;
    applyToml(doc, c);
    applyLegacyEnv(c);
    applyEnvOverrides(c);
    syncMirrors(c);
    c.validate();
    return c;
}

Config Config::fromEnv() {
    Config c = defaults();
    applyLegacyEnv(c);
    applyEnvOverrides(c);
    syncMirrors(c);
    c.validate();
    return c;
}

void Config::validate() const {
    auto checkBucket = [](const BucketConfig& b, const char* name) {
        if (b.capacity < 1 || b.capacity > 10000)
            cfgFail(std::string("rate_limit.") + name + ".capacity",
                    "must be an integer in 1..10000",
                    std::to_string(b.capacity));
        if (!(b.refill_rate_per_sec > 0.0) ||
            b.refill_rate_per_sec > 10000.0)
            cfgFail(std::string("rate_limit.") + name +
                        ".refill_rate_per_sec",
                    "must be a number in (0, 10000]",
                    std::to_string(b.refill_rate_per_sec));
    };
    checkBucket(rate_limit.chat, "chat");
    checkBucket(rate_limit.user, "user");

    auto checkTimeout = [](int v, const char* name, int lo, int hi) {
        if (v < lo || v > hi)
            cfgFail(std::string("timeouts.") + name,
                    "must be an integer in " + std::to_string(lo) + ".." +
                        std::to_string(hi) + " ms",
                    std::to_string(v));
    };
    checkTimeout(timeouts.resolve_ms, "resolve_ms", 100, 3600000);
    checkTimeout(timeouts.join_ms, "join_ms", 100, 3600000);
    checkTimeout(timeouts.ipc_cmd_ms, "ipc_cmd_ms", 100, 3600000);
    checkTimeout(timeouts.download_ms, "download_ms", 1000, 3600000);
    checkTimeout(timeouts.healthz_ms, "healthz_ms", 100, 3600000);

    if (quality.voice_profile != "ultra_320k" &&
        quality.voice_profile != "high_256k" &&
        quality.voice_profile != "standard_192k")
        cfgFail("quality.voice_profile",
                "must be one of ultra_320k, high_256k, standard_192k",
                quality.voice_profile);
    if (quality.resampler_quality != "sinc_256" &&
        quality.resampler_quality != "sinc_128" &&
        quality.resampler_quality != "fast")
        cfgFail("quality.resampler_quality",
                "must be one of sinc_256, sinc_128, fast",
                quality.resampler_quality);
    if (quality.source_ladder.empty())
        cfgFail("quality.source_ladder", "must list at least one source");
    {
        std::set<std::string> seen;
        for (const std::string& s : quality.source_ladder) {
            if (std::find(knownSources().begin(), knownSources().end(),
                          s) == knownSources().end())
                cfgFail("quality.source_ladder",
                        "unknown source '" + s +
                            "' (known: navidrome, soulseek, deezer, "
                            "netease, jiosaavn, youtube, spotify, "
                            "tiktok, radio)",
                        s);
            if (!seen.insert(s).second)
                cfgFail("quality.source_ladder",
                        "duplicate source '" + s + "'", s);
        }
    }

    if (limits.max_duration_minutes < 1 || limits.max_duration_minutes > 1440)
        cfgFail("limits.max_duration_minutes",
                "must be an integer in 1..1440",
                std::to_string(limits.max_duration_minutes));
    if (limits.max_queue_size < 1 || limits.max_queue_size > 10000)
        cfgFail("limits.max_queue_size", "must be an integer in 1..10000",
                std::to_string(limits.max_queue_size));
    if (limits.prefetch_lead_time_sec < 0 ||
        limits.prefetch_lead_time_sec > 3600)
        cfgFail("limits.prefetch_lead_time_sec",
                "must be an integer in 0..3600",
                std::to_string(limits.prefetch_lead_time_sec));
    if (limits.url_cache_ttl_sec < 0 || limits.url_cache_ttl_sec > 86400)
        cfgFail("limits.url_cache_ttl_sec",
                "must be an integer in 0..86400",
                std::to_string(limits.url_cache_ttl_sec));
}

std::vector<std::string> Config::missingRequired() const {
    std::vector<std::string> missing;
    if (bot_token.empty()) missing.push_back("BOT_TOKEN");
    if (parlay_token.empty()) missing.push_back("PARLAY_TOKEN");
    return missing;
}

std::string Config::summary() const {
    std::string s = "parlay-core port=" + std::to_string(port);
    s += " config=" + config_source;
    s += " rate_limit=chat(" +
         std::to_string(rate_limit.chat.capacity) + "@" +
         std::to_string(rate_limit.chat.refill_rate_per_sec) + "/s)"
         " user(" +
         std::to_string(rate_limit.user.capacity) + "@" +
         std::to_string(rate_limit.user.refill_rate_per_sec) + "/s)";
    s += " timeouts=resolve:" + std::to_string(timeouts.resolve_ms) +
         "ms join:" + std::to_string(timeouts.join_ms) +
         "ms ipc:" + std::to_string(timeouts.ipc_cmd_ms) +
         "ms dl:" + std::to_string(timeouts.download_ms) + "ms";
    s += " voice=" + quality.voice_profile + "(" +
         std::to_string(quality.opusBitrateKbps()) + "kbps)"
         " resampler=" +
         quality.resampler_quality + " ladder=" +
         std::to_string(quality.source_ladder.size()) + " sources";
    s += " limits=duration:" +
         std::to_string(limits.max_duration_minutes) +
         "min queue:" + std::to_string(limits.max_queue_size) +
         " prefetch:" + std::to_string(limits.prefetch_lead_time_sec) +
         "s cache_ttl:" + std::to_string(limits.url_cache_ttl_sec) + "s";
    s += " ipc_socket=" +
         (ipc_socket_path.empty() ? std::string("(default)") : ipc_socket_path);
    s += " owner_id=" + (owner_id ? std::to_string(owner_id) : std::string("(unset)"));
    s += " sudo_ids=" + std::to_string(sudo_ids.size());
    s += " logger_id=" + (logger_id ? std::to_string(logger_id) : std::string("(unset)"));
    s += " assistant_ids=" + std::to_string(assistant_ids.size());
    s += " allow_chats=" + std::to_string(allow_chats.size());
    s += " block_chats=" + std::to_string(block_chats.size());
    s += " api_id=" + std::string(api_id ? "set" : "unset");
    s += " tdlib=" + std::string(td_api_id && !td_api_hash.empty() &&
                                         !td_session_dir.empty()
                                     ? "configured"
                                     : "unset");
    return s;
}

}  // namespace parlay
