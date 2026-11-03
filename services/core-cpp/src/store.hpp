// store.hpp — Parlay core state store: JSON file ONLY.
//
// Locked architecture (FINAL-ARCHITECTURE.md §1): the Go backend's
// bbolt store is the single home for shared state (settings, queues,
// votes, shares, rooms). The core keeps only the boot/ephemeral keys
// it needs before the backend is up, in one JSON file under
// PARLAY_DATA_DIR (data/state.json) — never in a database.
// The hand-rolled database wire-protocol clients that used to live
// here were deleted in Phase 4 (Q1, user-ruled): they were a second
// state home, and setting the old database env vars would have split
// state away from bbolt. The SCRAM client class went with them; the general crypto
// primitives (SHA-256, base64, HMAC, PBKDF2) stay in scram.{hpp,cpp}
// — sha256 digests track ids and base64 encodes TDLib `bytes` fields.
//
// The durable API is key/value + sets + hashes, mirroring NoxBeats'
// DB object (sudoers, blacklists, auth users, per-chat runtime
// fields, playlists, active calls) — now backed by the JSON file:
//   kv    : engine choice, maintenance, logger ...
//   sets  : sudoers, blacklisted users/chats, auth users, known chats/users
//   hashes: per-chat runtime fields, per-user playlists (as JSON arrays)
// Ephemeral data (admin cache with TTL) is an in-memory map with
// expiry.
//
// Key inventory — what state.json holds after the Phase 6 Q7 rewire
// (see store.cpp for the same list with per-key notes):
//   BOOT / EPHEMERAL (stay here — the core needs them with the
//   backend down or before it is up): sudoers, chats, users,
//   bl_users, bl_chats, auth:<user_id>, shareblocked, maintenance,
//   maint_reason, logger_on, call:<chat_id> (+ keys("call:") scans),
//   card:<chat_id>, chat_title:<chat_id>, cleared:<...>,
//   searchmsg:/searchcool:/search:/inlinesearch: (transient search
//   state), last_chat:<user_id> (last chat an inline query came
//   from), panelmsg:/paneluser: (transient UI state),
//   dlrun:/dlset:/dlpick:/dlset_tpl_pending: (transient download
//   state), sharetarget:/sharetok:/sharelink:/sharefb: (transient
//   share state), vote:<track_id> (transient), dlsrv:,
//   last_requester_name:/last_requester_id:<chat_id>, autoleave,
//   and the in-memory TTL admin cache (cacheGet/cacheSet).
//   SETTINGS (single home is backend bbolt via chat.settings —
//   NEVER stored here; Dispatcher::settingsFor fetches them over
//   UDS and caches the snapshot in memory only): the 15 keys
//   (admin_mode, play_mode, cmd_delete, clean_mode, clean_delay_min,
//   thumbnails, autoplay, autoplay_lang, autoplay_mood, silent_mode,
//   auto_delete, auto_delete_min, language, paginate_purge_old,
//   chat_music_download).
//   RUNTIME (stay here, under the "chatrt:" hash — runtime state,
//   not settings-panel settings, with no backend home): loop, speed,
//   bass, muted, volume, rtmp_url, rtmp_key, cplay, last_track.
//   The legacy "chat:" hash is read-only fallback for those fields
//   (pre-rewire values survive the upgrade); nothing writes it.
//   The chat-level logger_on field was dead (never written) and was
//   dropped in the rewire; only the global logger_on key remains.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config.hpp"

namespace parlay {

// --- Store -----------------------------------------------------------------
class Store {
public:
    explicit Store(const Config& cfg);

    // Durable backend: always "file" (the JSON file is the core's
    // only durable store; see the header note).
    const std::string& backend() const { return backend_; }
    bool ok() const { return backend_ != "unavailable"; }

    // Key/value.
    std::optional<std::string> get(const std::string& key);
    void set(const std::string& key, const std::string& value);
    void del(const std::string& key);

    // Sets (members are strings).
    std::set<std::string> smembers(const std::string& key);
    void sadd(const std::string& key, const std::string& member);
    void srem(const std::string& key, const std::string& member);
    bool sismember(const std::string& key, const std::string& member);

    // Hashes.
    std::optional<std::string> hget(const std::string& key,
                                    const std::string& field);
    void hset(const std::string& key, const std::string& field,
              const std::string& value);
    void hdel(const std::string& key, const std::string& field);
    std::map<std::string, std::string> hgetall(const std::string& key);

    // Keys starting with `prefix` (file backend only).
    std::vector<std::string> keys(const std::string& prefix);

    // Ephemeral cache with TTL (in-memory only).
    std::optional<std::string> cacheGet(const std::string& key);
    void cacheSet(const std::string& key, const std::string& value,
                  int ttl_sec);

private:
    // File backend: one JSON document {kv:{}, sets:{}, hashes:{}}.
    void fileLoad();
    void fileSave();

    const Config& cfg_;
    std::string backend_ = "unavailable";
    // File-backend state.
    std::string file_path_;
    std::map<std::string, std::string> file_kv_;
    std::map<std::string, std::set<std::string>> file_sets_;
    std::map<std::string, std::map<std::string, std::string>> file_hashes_;
    // In-memory ephemeral cache: key -> (value, expires_at).
    std::map<std::string,
             std::pair<std::string, std::chrono::steady_clock::time_point>>
        mem_cache_;
    mutable std::recursive_mutex mu_;
};

}  // namespace parlay
