// store.cpp — JSON-file-only core store (Phase 4, Q1: the old
// database wire-protocol clients were deleted; the JSON file is
// the core's only durable store — see store.hpp).
#include "store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "json.hpp"

namespace parlay {

// --- Store -----------------------------------------------------------------
// Key inventory (what state.json holds; the same list is in
// store.hpp). BOOT / EPHEMERAL keys — the core needs these with the
// backend down or before it is up, so they stay in the JSON file:
//   sudoers, chats, users, bl_users, bl_chats (sets seeded from env
//     + learned at runtime; the dispatcher gates on them per command)
//   auth:<user_id> (sets: per-chat authorized users)
//   shareblocked (set: reported share senders)
//   maintenance, maint_reason (kv: owner maintenance mode + reason)
//   logger_on (kv: log-channel mirroring toggle)
//   call:<chat_id> (kv: active-call markers; keys("call:") counts
//     them for the /stats card)
//   card:<chat_id> (kv: last now-playing card id + photo/text kind,
//     so callback edits use the right edit method)
//   chat_title:<chat_id> (kv: cached chat titles for the settings
//     panel header)
//   cleared:<...> (kv: per-chat clear markers)
//   searchmsg:, searchcool:, search:, inlinesearch: (kv: transient
//     inline-search state — message ids, cooldowns, tokens),
//     last_chat:<user_id> (kv: last chat an inline query came from)
//   panelmsg:, paneluser: (kv: transient UI-panel state)
//   dlrun:, dlset:, dlpick:, dlset_tpl_pending: (kv: transient
//     download state — run records, format picks, pending templates)
//   sharetarget:, sharetok:, sharelink:, sharefb: (kv: transient
//     share-link state)
//   vote:<track_id> (kv: transient vote state — which chat opened it)
//   dlsrv: (kv: download-server pick)
//   last_requester_name:, last_requester_id: (kv: last track
//     requester, for the now-playing card)
//   autoleave (kv: auto-leave toggle)
//   + the in-memory TTL admin cache (cacheGet/cacheSet; never
//     persisted).
// SETTINGS live in backend bbolt (chat.settings) — the 15 keys are
// NEVER duplicated here (Q7 rewire, Phase 6: Dispatcher::settingsFor
// fetches them over UDS and caches the snapshot in memory only).
// Per-chat RUNTIME fields with no backend home live under the
// "chatrt:" hash — loop, speed, bass, muted, volume, rtmp_url,
// rtmp_key, cplay, last_track — core-voice session + routing state,
// not settings. The legacy "chat:" hash is a read-only fallback for
// those runtime fields (pre-rewire values survive the upgrade);
// nothing writes it. The chat-level logger_on field was dead (never
// written) and was dropped in the rewire; only the global logger_on
// key above remains.
// DELETED in Phase 4 (Q1): the autoplay:<chat_id> kv was write-only
// (never read anywhere — the autoplay flag is read from the
// chat:<chat_id> hash) and duplicated the hash field; its writes
// were removed from dispatcher.cpp.
Store::Store(const Config& cfg) : cfg_(cfg) {
    backend_ = "file";
    std::string dir = cfg.data_dir.empty() ? "data" : cfg.data_dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    file_path_ = dir + "/state.json";
    fileLoad();
    std::cout << "[store] backend=" << backend_ << " path=" << file_path_
              << "\n";
}

void Store::fileLoad() {
    std::ifstream f(file_path_);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return;
    Json root;
    try {
        root = Json::parse(content);
    } catch (...) {
        std::cerr << "[store] state.json unparsable; starting empty\n";
        return;
    }
    if (const Json* kv = root.find("kv")) {
        for (const auto& [k, v] : kv->asObject()) file_kv_[k] = v.asString();
    }
    if (const Json* sets = root.find("sets")) {
        for (const auto& [k, arr] : sets->asObject()) {
            for (const auto& el : arr.asArray())
                file_sets_[k].insert(el.asString());
        }
    }
    if (const Json* hashes = root.find("hashes")) {
        for (const auto& [k, h] : hashes->asObject()) {
            for (const auto& [field, v] : h.asObject())
                file_hashes_[k][field] = v.asString();
        }
    }
}

void Store::fileSave() {
    Json kv = Json::objectOf({});
    for (auto& [k, v] : file_kv_) kv.set(k, Json(v));
    Json sets = Json::objectOf({});
    for (auto& [k, s] : file_sets_) {
        Json::Array arr;
        for (auto& m : s) arr.push_back(Json(m));
        sets.set(k, Json(arr));
    }
    Json hashes = Json::objectOf({});
    for (auto& [k, h] : file_hashes_) {
        Json obj = Json::objectOf({});
        for (auto& [f, v] : h) obj.set(f, Json(v));
        hashes.set(k, obj);
    }
    Json root = Json::objectOf(
        {{"kv", kv}, {"sets", sets}, {"hashes", hashes}});
    std::string tmp = file_path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << root.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_path_, ec);
    if (ec)
        std::cerr << "[store] state.json rename failed: " << ec.message()
                  << "\n";
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = file_kv_.find(key);
    if (it == file_kv_.end()) return std::nullopt;
    return it->second;
}

void Store::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_kv_[key] = value;
    fileSave();
}

void Store::del(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_kv_.erase(key);
    fileSave();
}

std::set<std::string> Store::smembers(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = file_sets_.find(key);
    if (it == file_sets_.end()) return {};
    return it->second;
}

void Store::sadd(const std::string& key, const std::string& member) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_sets_[key].insert(member);
    fileSave();
}

void Store::srem(const std::string& key, const std::string& member) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_sets_[key].erase(member);
    fileSave();
}

bool Store::sismember(const std::string& key, const std::string& member) {
    return smembers(key).count(member) > 0;
}

std::optional<std::string> Store::hget(const std::string& key,
                                       const std::string& field) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = file_hashes_.find(key);
    if (it == file_hashes_.end()) return std::nullopt;
    auto fit = it->second.find(field);
    if (fit == it->second.end()) return std::nullopt;
    return fit->second;
}

void Store::hset(const std::string& key, const std::string& field,
                 const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_hashes_[key][field] = value;
    fileSave();
}

void Store::hdel(const std::string& key, const std::string& field) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    file_hashes_[key].erase(field);
    fileSave();
}

std::map<std::string, std::string> Store::hgetall(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = file_hashes_.find(key);
    if (it == file_hashes_.end()) return {};
    return it->second;
}

std::vector<std::string> Store::keys(const std::string& prefix) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<std::string> out;
    for (auto& [k, _] : file_kv_)
        if (k.rfind(prefix, 0) == 0) out.push_back(k);
    return out;
}

std::optional<std::string> Store::cacheGet(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = mem_cache_.find(key);
    if (it == mem_cache_.end()) return std::nullopt;
    if (std::chrono::steady_clock::now() > it->second.second) {
        mem_cache_.erase(it);
        return std::nullopt;
    }
    return it->second.first;
}

void Store::cacheSet(const std::string& key, const std::string& value,
                     int ttl_sec) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    mem_cache_[key] = {value, std::chrono::steady_clock::now() +
                                  std::chrono::seconds(ttl_sec)};
}

}  // namespace parlay
