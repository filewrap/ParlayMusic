// store.hpp — Parlay core state store.
//
// NoxBeats keeps all bot state (sudoers, blacklists, auth users, per-chat
// settings, playlists, active calls) in a DB object persisted through
// libmongoc (Bad/db/mongo_store.cpp: collection `state`, one document per
// key, replace_one with upsert) with a JSON-file fallback when no Mongo URI
// is set. Parlay's core does the same, but the sandbox has no libmongoc
// dev headers and no mongocxx, so the Mongo client here is a minimal
// hand-rolled MongoDB wire-protocol client (OP_MSG + BSON, no driver), and
// the Redis client is a minimal hand-rolled RESP client over a TCP socket.
// Both speak the real server protocols and are tested against real
// mongod/redis-server (see phase4/worker-a.md). When neither server is
// configured or reachable, state persists to a JSON file under
// PARLAY_DATA_DIR — never silently in-memory-only.
//
// Backend priority: MongoDB (MONGO_URI) > Redis (REDIS_URL) > JSON file.
// The durable API is key/value + sets + hashes, mirroring NoxBeats' DB:
//   kv    : engine choice, loop/speed/bass/muted, maintenance, logger ...
//   sets  : sudoers, blacklisted users/chats, auth users, known chats/users
//   hashes: per-chat settings, per-user playlists (as JSON arrays), RTMP
// Ephemeral data (admin cache with TTL) prefers Redis when present and
// falls back to an in-memory map with expiry.
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

// --- Redis (RESP) ---------------------------------------------------------
class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    // url: redis://[:pass@]host[:port][/db]
    bool connect(const std::string& url, std::string& error_out);
    bool ping();
    void close();

    bool get(const std::string& key, std::string& value_out);
    bool set(const std::string& key, const std::string& value);
    bool setEx(const std::string& key, const std::string& value, int ttl_sec);
    bool del(const std::string& key);
    bool sadd(const std::string& key, const std::string& member);
    bool srem(const std::string& key, const std::string& member);
    std::vector<std::string> smembers(const std::string& key);
    bool hset(const std::string& key, const std::string& field,
              const std::string& value);
    bool hdel(const std::string& key, const std::string& field);
    bool hget(const std::string& key, const std::string& field,
              std::string& value_out);
    std::map<std::string, std::string> hgetall(const std::string& key);
    std::vector<std::string> keys(const std::string& pattern);

private:
    // Sends one command (argv) and parses one RESP reply. Returns false on
    // transport/protocol error. Reply kinds: simple/bulk string, integer,
    // array, error (error_out filled), nil.
    struct Reply {
        char kind = 0;  // '+', '-', ':', '$', '*', 0 = transport error
        std::string str;
        int64_t num = 0;
        std::vector<Reply> arr;
        bool isNil = false;
    };
    bool command(const std::vector<std::string>& argv, Reply& reply_out,
                 std::string& error_out);
    bool readLine(std::string& line_out);
    bool readExact(char* buf, size_t n);
    bool writeAll(const std::string& data);

    int fd_ = -1;
    std::string read_buf_;
};

// --- MongoDB (wire protocol, OP_MSG + minimal BSON) -----------------------
// Parsed mongodb:// URI. Credentials are SCRAM-authenticated (RFC 5802 /
// RFC 7677) against auth_source, which defaults to the database in the
// URI and then to "admin" (the driver default).
struct MongoUri {
    std::string host = "127.0.0.1";
    int port = 27017;
    std::string db = "parlay";
    std::string username;  // empty = no authentication
    std::string password;
    std::string auth_source;  // resolved by parseUri, never empty on success
    // "" = negotiate (try SCRAM-SHA-256, fall back to SCRAM-SHA-1),
    // else "SCRAM-SHA-1" or "SCRAM-SHA-256" (from ?authMechanism=).
    std::string auth_mechanism;
};

class MongoClient {
public:
    MongoClient() = default;
    ~MongoClient();
    MongoClient(const MongoClient&) = delete;
    MongoClient& operator=(const MongoClient&) = delete;

    // Parses a mongodb:// URI (single host; mongodb+srv:// and replica
    // sets are rejected with a clear error). Percent-decodes userinfo.
    // Query params honoured: authSource, authMechanism.
    static bool parseUri(const std::string& uri, MongoUri& out,
                         std::string& error_out);

    // uri: mongodb://[user:pass@]host[:port][/db][?authSource=..]
    // Credentials trigger a SCRAM handshake after TCP connect.
    bool connect(const std::string& uri, std::string& error_out);
    bool ping();
    void close();

    // One raw OP_MSG command round trip; returns the reply body even
    // when ok != 1 (empty + error_out only on transport failure).
    // Useful for admin commands (e.g. createUser) and the SCRAM exchange.
    std::string command(const std::string& body_bson, std::string& error_out);

    // Documents in collection `state`: {_id: <key>, v: <string>}.
    bool findValue(const std::string& key, std::string& value_out);
    bool upsertValue(const std::string& key, const std::string& value);
    bool deleteKey(const std::string& key);
    // Keys starting with `prefix` (via a $regex filter).
    std::vector<std::string> keysWithPrefix(const std::string& prefix);

    const std::string& dbName() const { return db_; }
    const std::string& authSource() const { return auth_source_; }
    bool authenticated() const { return authenticated_; }

private:
    // One OP_MSG command round trip. Returns the parsed body document as a
    // raw BSON byte string (empty on error, error_out filled).
    std::string runCommand(const std::string& body_bson,
                           std::string& error_out);
    // Same, but returns the body even when the server reports ok != 1
    // (empty + error_out only on transport/protocol failure). Used by
    // runCommand and by the SCRAM exchange, which must read error replies.
    std::string runCommandRaw(const std::string& body_bson,
                              std::string& error_out);
    // SCRAM handshake (RFC 5802 / RFC 7677) against auth_source_.
    // Negotiates the mechanism when auth_mechanism_ is empty.
    bool authenticate(std::string& error_out);
    bool sendAll(const std::string& data);
    bool recvExact(char* buf, size_t n);

    int fd_ = -1;
    std::string host_ = "127.0.0.1";
    int port_ = 27017;
    std::string db_ = "parlay";
    std::string username_;
    std::string password_;
    std::string auth_source_ = "admin";
    std::string auth_mechanism_;  // "" = negotiate
    bool authenticated_ = false;
    int32_t request_id_ = 1;
};

// Minimal BSON builder/parser for the value shapes the store uses:
// string, int32, int64, double, bool, document, array of docs/strings,
// regex. Enough for find/update/delete commands and their replies.
namespace bson {
std::string doc(std::vector<std::pair<std::string, std::string>> fields);
// Field encoders: each returns the raw element bytes (type + key + value).
std::string elString(const std::string& key, const std::string& value);
std::string elInt32(const std::string& key, int32_t value);
std::string elInt64(const std::string& key, int64_t value);
std::string elDouble(const std::string& key, double value);
std::string elBool(const std::string& key, bool value);
std::string elDoc(const std::string& key, const std::string& doc_bson);
std::string elArray(const std::string& key,
                    const std::vector<std::string>& string_items);
std::string elArrayOfDocs(const std::string& key,
                          const std::vector<std::string>& docs);
std::string elRegex(const std::string& key, const std::string& pattern);
// Binary element (type 0x05); subtype 0 = generic (used for SASL payloads).
std::string elBinary(const std::string& key, const std::string& bytes,
                     uint8_t subtype = 0);

// Field readers over a BSON document (raw bytes). Returns false if absent
// or of the wrong type.
bool getString(const std::string& doc, const std::string& key,
               std::string& out);
bool getDouble(const std::string& doc, const std::string& key, double& out);
bool getInt32(const std::string& doc, const std::string& key, int32_t& out);
// Binary element (type 0x05): raw bytes out (subtype ignored).
bool getBinary(const std::string& doc, const std::string& key,
               std::string& out);
// First document of an array field, raw BSON bytes.
bool getFirstArrayDoc(const std::string& doc, const std::string& key,
                      std::string& out);
std::vector<std::string> getStringArray(const std::string& doc,
                                        const std::string& key);
}  // namespace bson

// --- Store -----------------------------------------------------------------
class Store {
public:
    explicit Store(const Config& cfg);

    // Which durable backend won: "mongo", "redis" or "file". Never empty.
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

    // Keys starting with `prefix` (durable backend only).
    std::vector<std::string> keys(const std::string& prefix);

    // Ephemeral cache with TTL (Redis when available, else in-memory).
    std::optional<std::string> cacheGet(const std::string& key);
    void cacheSet(const std::string& key, const std::string& value,
                  int ttl_sec);

private:
    // Durable-backend primitives behind the public API.
    std::optional<std::string> mongoGet(const std::string& key);
    void mongoSet(const std::string& key, const std::string& value);
    void mongoDel(const std::string& key);
    std::optional<std::string> redisGet(const std::string& key);
    void redisSet(const std::string& key, const std::string& value);
    void redisDel(const std::string& key);

    // File backend: one JSON document {kv:{}, sets:{}, hashes:{}}.
    void fileLoad();
    void fileSave();

    const Config& cfg_;
    std::string backend_ = "unavailable";
    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<MongoClient> mongo_;
    // File-backend state (also used to persist the in-memory fallback).
    std::string file_path_;
    std::map<std::string, std::string> file_kv_;
    std::map<std::string, std::set<std::string>> file_sets_;
    std::map<std::string, std::map<std::string, std::string>> file_hashes_;
    // In-memory ephemeral cache fallback: key -> (value, expires_at).
    std::map<std::string,
             std::pair<std::string, std::chrono::steady_clock::time_point>>
        mem_cache_;
    mutable std::recursive_mutex mu_;
};

}  // namespace parlay
