// store_test.cpp — validates the JSON-file-only Store (Phase 4, Q1).
// The old database wire-protocol clients were deleted with the
// Phase 4 Q1 ruling; the store has exactly one
// durable backend (the JSON file under <data_dir>/state.json) and an
// in-memory TTL cache. No servers are needed: every check runs
// against a scratch data dir.
// Usage: ./store_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "config.hpp"
#include "store.hpp"

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            std::cout << "PASS " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << "\n";                           \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/parlay-store-test-file";
    std::error_code ec;
    fs::remove_all(dir, ec);

    // --- Store over the JSON file: the only durable backend ---
    {
        parlay::Config cfg;
        cfg.data_dir = dir;
        parlay::Store s(cfg);
        CHECK(s.backend() == "file",
              "store backend=file (got " + s.backend() + ")");
        CHECK(s.ok(), "store ok");

        // Key/value round-trip, overwrite, delete, missing.
        s.set("engine:-1002", "rust");
        auto e = s.get("engine:-1002");
        CHECK(e && *e == "rust", "store kv round-trip (file)");
        s.set("engine:-1002", "ts");
        CHECK(s.get("engine:-1002") ==
                  std::optional<std::string>("ts"),
              "store kv overwrite (file)");
        s.del("engine:-1002");
        CHECK(!s.get("engine:-1002"), "store kv delete (file)");
        CHECK(!s.get("engine:missing"), "store kv missing (file)");

        // Sets: add / membership / size / remove.
        s.sadd("sudoers", "333");
        s.sadd("sudoers", "444");
        CHECK(s.sismember("sudoers", "333"), "store set member (file)");
        CHECK(s.smembers("sudoers").size() == 2,
              "store set size (file)");
        s.srem("sudoers", "333");
        CHECK(!s.sismember("sudoers", "333"),
              "store set remove (file)");
        CHECK(s.smembers("sudoers").size() == 1,
              "store set size after remove (file)");
        CHECK(s.smembers("no-such-set").empty(),
              "store set missing is empty (file)");

        // Hashes: round-trip, all, delete field, missing.
        s.hset("chat:-1002", "lang", "en");
        s.hset("chat:-1002", "play_mode", "everyone");
        CHECK(s.hget("chat:-1002", "lang") ==
                  std::optional<std::string>("en"),
              "store hash round-trip (file)");
        auto all = s.hgetall("chat:-1002");
        CHECK(all.size() == 2 && all["play_mode"] == "everyone",
              "store hash all (file)");
        s.hdel("chat:-1002", "lang");
        CHECK(!s.hget("chat:-1002", "lang"),
              "store hash delete field (file)");
        CHECK(!s.hget("chat:-1002", "nope"),
              "store hash missing field (file)");
        CHECK(s.hgetall("chat:-9999").empty(),
              "store hash missing key is empty (file)");

        // keys(prefix): only kv keys with the prefix, in order.
        s.set("engine:-1003", "go");
        s.set("other:-1003", "x");
        auto k = s.keys("engine:");
        bool found = false;
        for (auto& kk : k)
            if (kk == "engine:-1003") found = true;
        CHECK(found && k.size() == 1, "store keys prefix (file)");

        // Ephemeral cache: hit, miss, TTL expiry (in-memory only).
        s.cacheSet("admins:-1001", "1,2,3", 60);
        auto c = s.cacheGet("admins:-1001");
        CHECK(c && *c == "1,2,3", "store cache hit (memory)");
        CHECK(!s.cacheGet("admins:missing"), "store cache miss (memory)");
        s.cacheSet("short-lived", "v", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(!s.cacheGet("short-lived"), "store cache TTL expiry");

        // Persistence across instances (same data dir).
        parlay::Store s2(cfg);
        CHECK(s2.get("engine:-1003") ==
                  std::optional<std::string>("go"),
              "store file persistence kv across instances");
        CHECK(s2.sismember("sudoers", "444"),
              "store file persistence set across instances");
        CHECK(s2.hget("chat:-1002", "play_mode") ==
                  std::optional<std::string>("everyone"),
              "store file persistence hash across instances");
        // The cache is ephemeral: a new instance does not see it.
        CHECK(!s2.cacheGet("admins:-1001"),
              "store cache does not persist across instances");
    }

    // --- Corrupt state.json: starts empty, never crashes ---
    {
        parlay::Config cfg;
        cfg.data_dir = dir;
        {
            std::ofstream f(dir + "/state.json", std::ios::trunc);
            f << "{ this is not json";
        }
        parlay::Store s(cfg);
        CHECK(s.ok() && s.backend() == "file",
              "store corrupt state.json starts empty");
        CHECK(!s.get("engine:-1003"),
              "store corrupt state.json has no keys");
        // ... and recovers: a write after the corrupt load persists.
        s.set("engine:-1003", "recovered");
        parlay::Store s2(cfg);
        CHECK(s2.get("engine:-1003") ==
                  std::optional<std::string>("recovered"),
              "store recovers after corrupt state.json");
    }

    // --- Empty data_dir falls back to "data" (never "/state.json") ---
    {
        parlay::Config cfg;
        cfg.data_dir = "";
        // Run in a scratch CWD so the fallback "data/" dir it creates
        // does not pollute the build tree.
        const std::string cwd_dir = "/tmp/parlay-store-test-cwd";
        fs::remove_all(cwd_dir, ec);
        fs::create_directories(cwd_dir, ec);
        fs::current_path(cwd_dir, ec);
        parlay::Store s(cfg);
        CHECK(s.backend() == "file", "store empty data_dir still file");
        s.set("probe", "1");
        CHECK(fs::exists(cwd_dir + "/data/state.json"),
              "store empty data_dir falls back to ./data/state.json");
        fs::current_path("/tmp", ec);
        fs::remove_all(cwd_dir, ec);
    }

    fs::remove_all(dir, ec);
    std::cout << (failures == 0 ? "ALL PASS"
                                : "FAILURES: " + std::to_string(failures))
              << "\n";
    return failures == 0 ? 0 : 1;
}
