// store_test.cpp — validates Store + MongoClient + RedisClient + BSON.
// Usage: ./store_test [mongo_uri] [redis_url]
// Defaults: mongodb://127.0.0.1:27099/parlay_test, redis://127.0.0.1:6399/0
// Exit 0 = all checks pass. Prints PASS/FAIL per check.
#include <cassert>
#include <cstdio>
#include <iostream>

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

int main(int argc, char** argv) {
    std::string mongo_uri =
        argc > 1 ? argv[1] : "mongodb://127.0.0.1:27099/parlay_test";
    std::string redis_url =
        argc > 2 ? argv[2] : "redis://127.0.0.1:6399/0";

    // --- BSON round-trip (no server) ---
    {
        std::string d = parlay::bson::doc(
            {{"a", parlay::bson::elString("a", "hello")},
             {"n", parlay::bson::elInt32("n", 42)},
             {"b", parlay::bson::elBool("b", true)},
             {"f", parlay::bson::elDouble("f", 1.5)}});
        std::string s;
        int32_t n = 0;
        double f = 0;
        CHECK(parlay::bson::getString(d, "a", s) && s == "hello",
              "bson getString");
        CHECK(parlay::bson::getInt32(d, "n", n) && n == 42, "bson getInt32");
        CHECK(parlay::bson::getDouble(d, "f", f) && f == 1.5,
              "bson getDouble");
        std::string inner = parlay::bson::doc(
            {{"x", parlay::bson::elString("x", "y")}});
        std::string outer = parlay::bson::doc(
            {{"sub", parlay::bson::elDoc("sub", inner)},
             {"arr", parlay::bson::elArray("arr", {"one", "two"})}});
        std::string first;
        CHECK(parlay::bson::getFirstArrayDoc(
                  parlay::bson::doc({{"arr",
                                      parlay::bson::elArrayOfDocs(
                                          "arr", {inner})}}),
                  "arr", first),
              "bson getFirstArrayDoc");
        auto arr = parlay::bson::getStringArray(outer, "arr");
        CHECK(arr.size() == 2 && arr[0] == "one" && arr[1] == "two",
              "bson getStringArray");
    }

    // --- Mongo wire protocol vs real mongod ---
    {
        parlay::MongoClient m;
        std::string err;
        bool connected = m.connect(mongo_uri, err);
        CHECK(connected, std::string("mongo connect: ") + err);
        if (connected) {
            CHECK(m.ping(), "mongo ping");
            CHECK(m.upsertValue("kv:test-key", "hello-mongo"),
                  "mongo upsert");
            std::string v;
            CHECK(m.findValue("kv:test-key", v) && v == "hello-mongo",
                  "mongo find");
            CHECK(m.upsertValue("kv:test-key", "hello2"), "mongo upsert2");
            CHECK(m.findValue("kv:test-key", v) && v == "hello2",
                  "mongo find2");
            auto keys = m.keysWithPrefix("kv:test-");
            bool found = false;
            for (auto& k : keys)
                if (k == "kv:test-key") found = true;
            CHECK(found, "mongo keysWithPrefix");
            CHECK(m.deleteKey("kv:test-key"), "mongo delete");
            CHECK(!m.findValue("kv:test-key", v), "mongo find after delete");
        }
    }

    // --- Redis RESP vs real redis (optional: skip if not up) ---
    {
        parlay::RedisClient r;
        std::string err;
        bool connected = r.connect(redis_url, err);
        if (!connected) {
            std::cout << "SKIP redis tests (not up): " << err << "\n";
        } else {
            CHECK(r.ping(), "redis ping");
            CHECK(r.set("t:k", "v1"), "redis set");
            std::string v;
            CHECK(r.get("t:k", v) && v == "v1", "redis get");
            CHECK(r.sadd("t:s", "a") && r.sadd("t:s", "b"), "redis sadd");
            auto members = r.smembers("t:s");
            CHECK(members.size() == 2, "redis smembers");
            CHECK(r.hset("t:h", "f", "hv"), "redis hset");
            CHECK(r.hget("t:h", "f", v) && v == "hv", "redis hget");
            auto all = r.hgetall("t:h");
            CHECK(all.size() == 1 && all["f"] == "hv", "redis hgetall");
            auto keys = r.keys("t:*");
            CHECK(keys.size() >= 3, "redis keys");
            CHECK(r.del("t:k"), "redis del");
            CHECK(!r.get("t:k", v), "redis get after del");
        }
    }

    // --- Store over Mongo (durable backend) ---
    {
        parlay::Config cfg;
        cfg.mongo_uri = mongo_uri;
        cfg.redis_url = "";  // force mongo-only for this check
        cfg.data_dir = "/tmp/parlay-store-test";
        parlay::Store s(cfg);
        CHECK(s.backend() == "mongo",
              "store backend=mongo (got " + s.backend() + ")");
        s.set("engine:-1001", "ts");
        auto e = s.get("engine:-1001");
        CHECK(e && *e == "ts", "store kv round-trip (mongo)");
        s.sadd("sudoers", "111");
        s.sadd("sudoers", "222");
        CHECK(s.sismember("sudoers", "111"), "store set member (mongo)");
        CHECK(s.smembers("sudoers").size() == 2, "store set size (mongo)");
        s.srem("sudoers", "111");
        CHECK(!s.sismember("sudoers", "111"), "store set remove (mongo)");
        s.hset("chat:-1001", "play_mode", "admin");
        auto pm = s.hget("chat:-1001", "play_mode");
        CHECK(pm && *pm == "admin", "store hash round-trip (mongo)");
        auto all = s.hgetall("chat:-1001");
        CHECK(all.size() == 1, "store hash all (mongo)");
        auto k = s.keys("engine:");
        bool found = false;
        for (auto& kk : k)
            if (kk == "engine:-1001") found = true;
        CHECK(found, "store keys prefix (mongo)");
        s.cacheSet("admins:-1001", "1,2,3", 60);
        auto c = s.cacheGet("admins:-1001");
        CHECK(c && *c == "1,2,3", "store cache (memory fallback)");
    }

    // --- Store file fallback ---
    {
        parlay::Config cfg;
        cfg.mongo_uri = "";
        cfg.redis_url = "";
        cfg.data_dir = "/tmp/parlay-store-test-file";
        parlay::Store s(cfg);
        CHECK(s.backend() == "file", "store backend=file");
        s.set("engine:-1002", "rust");
        auto e = s.get("engine:-1002");
        CHECK(e && *e == "rust", "store kv round-trip (file)");
        s.sadd("sudoers", "333");
        CHECK(s.sismember("sudoers", "333"), "store set (file)");
        s.hset("chat:-1002", "lang", "en");
        CHECK(s.hget("chat:-1002", "lang") == std::optional<std::string>("en"),
              "store hash (file)");
        // Re-open: persistence across instances.
        parlay::Store s2(cfg);
        CHECK(s2.get("engine:-1002") == std::optional<std::string>("rust"),
              "store file persistence across instances");
        CHECK(s2.sismember("sudoers", "333"),
              "store file set persistence");
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES: " + std::to_string(failures))
              << "\n";
    return failures == 0 ? 0 : 1;
}
