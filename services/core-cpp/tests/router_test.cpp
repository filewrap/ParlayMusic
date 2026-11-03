// router_test.cpp — EngineRouter fail-over on 404 (Phase 5, Worker B).
//
// A 404 for a KNOWN contract action (contracts/ipc.json vc.*) means the
// engine does not implement the action: the router tries the next engine.
// A 404 for an unknown action is a real not-found and stays a 404.
// 5xx / transport errors / engine_unavailable still fail over (regression).
//
// Build: g++ -std=c++20 -I src tests/router_test.cpp src/engine_router.cpp
//   src/ipc.cpp src/http.cpp src/json.cpp src/config.cpp src/store.cpp
//   src/scram.cpp -o /tmp/router_test && /tmp/router_test
// Exit 0 = all checks pass.
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "engine_router.hpp"
#include "ipc.hpp"

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

namespace {

parlay::IpcResult res(bool ok, int status, const std::string& code,
                      const std::string& msg) {
    parlay::IpcResult r;
    r.ok = ok;
    r.http_status = status;
    r.error_code = code;
    r.error_message = msg;
    return r;
}

// Scripted IPC client: per-base-url, per-action answers; records calls.
class FakeIpc : public parlay::IpcClient {
public:
    explicit FakeIpc(const parlay::Config& cfg) : parlay::IpcClient(cfg) {}
    std::map<std::string, std::map<std::string, parlay::IpcResult>> script;
    mutable std::vector<std::string> calls;  // "base|action"
    parlay::IpcResult post(const std::string& base_url,
                           const std::string& action, const parlay::Json& body,
                           int timeout_sec = 30) const override {
        (void)body;
        (void)timeout_sec;
        calls.push_back(base_url + "|" + action);
        auto b = script.find(base_url);
        if (b != script.end()) {
            auto a = b->second.find(action);
            if (a != b->second.end()) return a->second;
        }
        return res(false, 404, "not_found",
                   "no script for " + base_url + " " + action);
    }
    bool healthz(const std::string& base_url, int timeout_sec = 5) const override {
        (void)base_url;
        (void)timeout_sec;
        return true;  // fallback engines are up; the 404 is the trigger
    }
};

}  // namespace

int main() {
    parlay::Config cfg;  // defaults: go :8001, ts :8002, rust :8003
    const std::string go = cfg.vc_go_url;
    const std::string ts = cfg.vc_ts_url;
    const std::string rust = cfg.vc_rust_url;

    // --- IpcResult::unavailable() unit checks ---
    {
        parlay::IpcResult r404 = res(false, 404, "not_found", "unknown route");
        CHECK(r404.unavailable("loop"),
              "404 for known action loop is unavailable");
        CHECK(!r404.unavailable("bogus-action"),
              "404 for unknown action stays a 404");
        CHECK(!r404.unavailable(),
              "404 with no action context is not unavailable");
        CHECK(res(false, 500, "internal", "boom").unavailable("loop"),
              "500 still unavailable");
        CHECK(res(false, 0, "", "").unavailable("loop"),
              "transport error still unavailable");
        CHECK(res(false, 200, "engine_unavailable", "down")
                  .unavailable("loop"),
              "engine_unavailable still unavailable");
    }

    // --- 1. known action 404s on preferred engine -> next engine wins ---
    {
        FakeIpc ipc(cfg);
        ipc.script[go]["loop"] =
            res(false, 404, "not_found", "unknown route");
        ipc.script[ts]["loop"] = res(true, 200, "", "");
        parlay::EngineRouter router(cfg, nullptr);  // default engine: go
        parlay::Engine used = parlay::Engine::Go;
        bool fell_back = false;
        parlay::IpcResult r = router.callVc(
            ipc, -1001, "loop",
            parlay::Json::objectOf({{"chat_id", parlay::Json((int64_t)-1001)},
                                    {"count", parlay::Json((int64_t)2)}}),
            used, fell_back);
        CHECK(r.ok, "404 loop on go fails over to ts (ok)");
        CHECK(used == parlay::Engine::Ts, "404 loop on go fails over to ts (used)");
        CHECK(fell_back, "404 loop on go fails over to ts (fell_back)");
        CHECK(ipc.calls.size() == 2 &&
                  ipc.calls[0] == go + "|loop" &&
                  ipc.calls[1] == ts + "|loop",
              "404 loop on go calls go then ts only");
    }

    // --- 2. unknown action 404 stays a 404: no fail-over ---
    {
        FakeIpc ipc(cfg);
        ipc.script[go]["bogus-action"] =
            res(false, 404, "not_found", "unknown route");
        ipc.script[ts]["bogus-action"] = res(true, 200, "", "");
        parlay::EngineRouter router(cfg, nullptr);
        parlay::Engine used = parlay::Engine::Go;
        bool fell_back = false;
        parlay::IpcResult r = router.callVc(
            ipc, -1001, "bogus-action",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json((int64_t)-1001)}}),
            used, fell_back);
        CHECK(!r.ok && r.http_status == 404,
              "404 for unknown action stays a 404");
        CHECK(!fell_back, "404 for unknown action does not fall back");
        CHECK(used == parlay::Engine::Go,
              "404 for unknown action keeps preferred engine");
        CHECK(ipc.calls.size() == 1 && ipc.calls[0] == go + "|bogus-action",
              "404 for unknown action calls only the preferred engine");
    }

    // --- 3. known action 404 on every engine -> last 404 returned ---
    {
        FakeIpc ipc(cfg);
        ipc.script[go]["shuffle"] =
            res(false, 404, "not_found", "unknown route");
        ipc.script[ts]["shuffle"] =
            res(false, 404, "not_found", "unknown route");
        ipc.script[rust]["shuffle"] =
            res(false, 404, "not_found", "unknown route");
        parlay::EngineRouter router(cfg, nullptr);
        parlay::Engine used = parlay::Engine::Go;
        bool fell_back = false;
        parlay::IpcResult r = router.callVc(
            ipc, -1001, "shuffle",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json((int64_t)-1001)},
                 {"enabled", parlay::Json(true)}}),
            used, fell_back);
        CHECK(!r.ok && r.http_status == 404,
              "404 on all engines returns the last 404");
        CHECK(!fell_back, "404 on all engines does not report a fallback");
        CHECK(ipc.calls.size() == 3, "404 on all engines tries all three");
    }

    // --- 4. 5xx still fails over (regression) ---
    {
        FakeIpc ipc(cfg);
        ipc.script[go]["play"] = res(false, 500, "internal", "boom");
        ipc.script[ts]["play"] = res(true, 200, "", "");
        parlay::EngineRouter router(cfg, nullptr);
        parlay::Engine used = parlay::Engine::Go;
        bool fell_back = false;
        parlay::IpcResult r = router.callVc(
            ipc, -1001, "play",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json((int64_t)-1001)},
                 {"query", parlay::Json("test")}}),
            used, fell_back);
        CHECK(r.ok && used == parlay::Engine::Ts && fell_back,
              "500 on go still fails over to ts");
    }

    // --- 5. known action ok on preferred engine: no fail-over ---
    {
        FakeIpc ipc(cfg);
        ipc.script[go]["pause"] = res(true, 200, "", "");
        parlay::EngineRouter router(cfg, nullptr);
        parlay::Engine used = parlay::Engine::Ts;  // sentinel
        bool fell_back = true;                      // sentinel
        parlay::IpcResult r = router.callVc(
            ipc, -1001, "pause",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json((int64_t)-1001)}}),
            used, fell_back);
        CHECK(r.ok && used == parlay::Engine::Go && !fell_back,
              "ok on preferred engine: no fallback");
        CHECK(ipc.calls.size() == 1, "ok on preferred engine: one call");
    }

    if (failures == 0) {
        std::cout << "router_test: ALL PASS\n";
        return 0;
    }
    std::cout << "router_test: " << failures << " FAILURE(S)\n";
    return 1;
}
