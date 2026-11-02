// http_api_test.cpp — Core HTTP API (Phase 6, A2).
//
//   * CoreApi routing with a scripted VcStateProvider (no sockets):
//     /healthz, /api/now-playing and /api/queue success + honest
//     errors (no state -> 404 "no playback state", missing/bad
//     chat_id -> 400, unknown route -> 404, non-GET -> 405), the
//     queue-list fallback when now-playing reports no state.
//   * HttpServer over a real socket (ephemeral port): the same API
//     answers a real HTTP client (the curl-CLI HttpClient).
//   * Dispatcher proxy path: a stub VC engine on 127.0.0.1 (a real
//     HTTP server returning canned IPC envelopes) + the real Dispatcher
//     and IpcClient. apiNowPlaying returns the canned track, an
//     unknown chat honestly reports no state, and the full stack
//     (CoreApi + HttpServer) serves the canned state over HTTP.
//
// Build: g++ -std=c++20 -I src tests/http_api_test.cpp
//   src/core_api.cpp src/http_server.cpp src/dispatcher.cpp
//   src/telegram_api.cpp src/ipc.cpp src/engine_router.cpp src/store.cpp
//   src/json.cpp src/config.cpp src/http.cpp src/voice_call.cpp
//   src/tdlib_client.cpp -o /tmp/http_api_test -ldl && /tmp/http_api_test
// Exit 0 = all checks pass. No live services, no Telegram, no network
// beyond 127.0.0.1: the engine is the stub below, the store uses its
// JSON file backend in a temp dir.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "config.hpp"
#include "core_api.hpp"
#include "dispatcher.hpp"
#include "engine_router.hpp"
#include "http.hpp"
#include "http_server.hpp"
#include "store.hpp"
#include "telegram_api.hpp"
#include "voice_call.hpp"

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

// Canned now-playing for chat -100777, as the stub engine returns it
// (IPC envelope data) and as the core API returns it (flat JSON).
const char* kNowPlayingJson = R"({
  "chat_id": -100777, "engine": "go",
  "track": {"id": "yt:test123", "title": "Test Track",
            "artist": "Test Artist", "duration_sec": 200,
            "source": "youtube"},
  "paused": false, "position_sec": 42,
  "queue": [{"position": 0, "requested_by": 123,
             "track": {"id": "yt:next1", "title": "Next Track",
                       "artist": "Next Artist", "duration_sec": 180,
                       "source": "youtube"}}],
  "volume": 80, "loop": 0, "shuffle": false, "autoplay": false
})";

// Canned queue list for chats -100777 and -100888 (POST /v1/queue).
const char* kQueueJson = R"([
  {"position": 0, "requested_by": 123,
   "track": {"id": "yt:next1", "title": "Next Track",
             "artist": "Next Artist", "duration_sec": 180,
             "source": "youtube"}},
  {"position": 1, "requested_by": 456,
   "track": {"id": "yt:next2", "title": "Second Track",
             "artist": "Second Artist", "duration_sec": 240,
             "source": "youtube"}}
])";

parlay::Json cannedNp() { return parlay::Json::parse(kNowPlayingJson); }

parlay::HttpRequest getReq(const std::string& path,
                           const std::string& chat_id = "") {
    parlay::HttpRequest r;
    r.method = "GET";
    r.path = path;
    if (!chat_id.empty()) r.query["chat_id"] = chat_id;
    return r;
}

// Scripted provider for the CoreApi routing checks.
class FakeProvider : public parlay::VcStateProvider {
public:
    parlay::Json np = cannedNp();
    bool np_ok = true;
    parlay::Json qlist = parlay::Json::parse(kQueueJson);
    bool qlist_ok = true;
    mutable int64_t last_chat = 0;

    bool nowPlaying(int64_t chat_id, parlay::Json& out,
                    std::string& err) override {
        last_chat = chat_id;
        if (!np_ok) {
            err = "no playback state";
            return false;
        }
        out = np;
        return true;
    }
    bool queueList(int64_t chat_id, parlay::Json& out,
                   std::string& err) override {
        last_chat = chat_id;
        if (!qlist_ok) {
            err = "no queue";
            return false;
        }
        out = qlist;
        return true;
    }
};

// Adapter used for the full-stack check: CoreApi over a real Dispatcher.
class DispatchProvider : public parlay::VcStateProvider {
public:
    explicit DispatchProvider(parlay::Dispatcher& d) : d_(d) {}
    bool nowPlaying(int64_t c, parlay::Json& o, std::string& e) override {
        return d_.apiNowPlaying(c, o, e);
    }
    bool queueList(int64_t c, parlay::Json& o, std::string& e) override {
        return d_.apiQueueList(c, o, e);
    }

private:
    parlay::Dispatcher& d_;
};

bool bodyHas(const std::string& body, const std::string& needle) {
    return body.find(needle) != std::string::npos;
}

void testCoreApiRouting() {
    FakeProvider p;
    parlay::CoreApi api(p);

    auto r = api.handle(getReq("/healthz"));
    CHECK(r.status == 200 && r.body == R"({"ok":true})",
          "healthz -> 200 {\"ok\":true}");

    r = api.handle(getReq("/api/now-playing", "-100777"));
    CHECK(r.status == 200 && bodyHas(r.body, "\"yt:test123\"") &&
              bodyHas(r.body, "\"now_playing\""),
          "now-playing ok -> 200 with the canned track");
    CHECK(p.last_chat == -100777, "group chat_id parses as negative int64");

    p.np_ok = false;
    r = api.handle(getReq("/api/now-playing", "-100777"));
    CHECK(r.status == 404 &&
              bodyHas(r.body, "\"error\":\"no playback state\""),
          "now-playing with no state -> 404 honest error, never demo");
    p.np_ok = true;

    r = api.handle(getReq("/api/now-playing"));
    CHECK(r.status == 400 &&
              bodyHas(r.body, "chat_id query param is required"),
          "now-playing without chat_id -> 400");
    r = api.handle(getReq("/api/now-playing", "12x"));
    CHECK(r.status == 400, "now-playing with bad chat_id -> 400");

    r = api.handle(getReq("/api/queue", "-100777"));
    CHECK(r.status == 200 && bodyHas(r.body, "\"yt:next1\"") &&
              bodyHas(r.body, "\"chat_id\":-100777"),
          "queue ok -> 200 with the now-playing queue field + chat_id");

    // Queue-list fallback: now-playing reports no state, but the
    // engine's queue list is non-empty (vc-ts queue-only room).
    p.np_ok = false;
    r = api.handle(getReq("/api/queue", "-100777"));
    CHECK(r.status == 200 && bodyHas(r.body, "\"yt:next2\""),
          "queue falls back to the engine queue list when now-playing "
          "has no state");
    // Empty list is not a queue: still "no playback state".
    p.qlist = parlay::Json::parse("[]");
    r = api.handle(getReq("/api/queue", "-100777"));
    CHECK(r.status == 404 &&
              bodyHas(r.body, "\"error\":\"no playback state\""),
          "queue with empty list -> 404 no playback state");
    p.qlist = parlay::Json::parse(kQueueJson);
    p.qlist_ok = false;
    r = api.handle(getReq("/api/queue", "-100777"));
    CHECK(r.status == 404, "queue with both paths failing -> 404");
    p.qlist_ok = true;
    p.np_ok = true;

    // Now-playing answered but carried no queue field.
    p.np = parlay::Json::parse(
        R"({"chat_id":-100777,"engine":"go","track":{"id":"yt:x"}})");
    r = api.handle(getReq("/api/queue", "-100777"));
    CHECK(r.status == 502 &&
              bodyHas(r.body, "engine did not report a queue"),
          "queue with no queue field in now-playing -> 502");
    p.np = cannedNp();

    r = api.handle(getReq("/nope"));
    CHECK(r.status == 404 && bodyHas(r.body, "\"error\":\"not found\""),
          "unknown route -> 404");
    parlay::HttpRequest post;
    post.method = "POST";
    post.path = "/healthz";
    r = api.handle(post);
    CHECK(r.status == 405, "non-GET -> 405 method not allowed");
}

void testHttpServerSocket() {
    FakeProvider p;
    parlay::CoreApi api(p);
    parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
        return api.handle(req);
    });
    CHECK(server.start(), "http server starts on an ephemeral port");
    CHECK(server.port() != 0, "ephemeral port is reported back");
    CHECK(server.running(), "server reports running");

    std::string base = "http://127.0.0.1:" + std::to_string(server.port());
    auto h = parlay::HttpClient::get(base + "/healthz");
    CHECK(h.status == 200 && h.body == R"({"ok":true})",
          "socket: GET /healthz -> 200 {\"ok\":true}");
    auto np = parlay::HttpClient::get(base + "/api/now-playing?chat_id=-100777");
    CHECK(np.status == 200 && bodyHas(np.body, "yt:test123"),
          "socket: GET /api/now-playing -> 200 canned track over HTTP");
    auto bad = parlay::HttpClient::get(base + "/api/now-playing");
    CHECK(bad.status == 400, "socket: missing chat_id -> 400 over HTTP");
    auto q = parlay::HttpClient::get(base + "/api/queue?chat_id=-100777");
    CHECK(q.status == 200 && bodyHas(q.body, "yt:next1"),
          "socket: GET /api/queue -> 200 canned queue over HTTP");
    server.stop();
    CHECK(!server.running(), "server stops cleanly");
}

// Stub VC engine: canned IPC envelopes. -100777 has a track,
// -100888 has a queue but no current track (now-playing 404s,
// queue list is real), anything else has nothing.
parlay::HttpResponse stubEngine(const parlay::HttpRequest& req) {
    if (req.method == "GET" && req.path == "/v1/now-playing") {
        auto it = req.query.find("chat_id");
        if (it != req.query.end() && it->second == "-100777") {
            return {200, std::string(R"({"ok":true,"data":{"now_playing":)") +
                           kNowPlayingJson + "}}"};
        }
        return {404, R"({"ok":false,"error":{"code":"not_found","message":"no playback state for this chat"}})"};
    }
    if (req.method == "POST" && req.path == "/v1/queue") {
        int64_t chat = 0;
        try {
            parlay::Json body = parlay::Json::parse(req.body);
            const parlay::Json* c = body.find("chat_id");
            if (c) chat = c->asInt();
        } catch (...) {
            // Unparseable body: no queue.
        }
        if (chat == -100777 || chat == -100888) {
            return {200, std::string(R"({"ok":true,"data":{"queue":)") +
                           kQueueJson + "}}"};
        }
        return {200, R"({"ok":true,"data":{"queue":[]}})"};
    }
    return {404, R"({"ok":false,"error":{"code":"not_found","message":"route not found"}})"};
}

void testDispatcherProxy() {
    parlay::HttpServer stub(0, stubEngine);
    CHECK(stub.start(), "stub engine starts on an ephemeral port");
    std::string stub_base =
        "http://127.0.0.1:" + std::to_string(stub.port());

    std::filesystem::create_directories("/tmp/p6-http-api/store");
    parlay::Config cfg;
    cfg.vc_go_url = stub_base;               // preferred engine = stub
    cfg.vc_ts_url = "http://127.0.0.1:9";    // dead: fallback must not help
    cfg.vc_rust_url = "http://127.0.0.1:9";  // dead
    cfg.data_dir = "/tmp/p6-http-api/store";
    parlay::Store store(cfg);
    CHECK(store.ok(), "store ok on the JSON file backend");
    parlay::TelegramApi tg(cfg);  // ctor does no network
    parlay::UnavailableVoiceCall voice("test");
    parlay::EngineRouter router(cfg, &store);
    parlay::IpcClient ipc(cfg);
    parlay::Dispatcher d(cfg, tg, ipc, router, store, voice, "");

    parlay::Json np;
    std::string err;
    CHECK(d.apiNowPlaying(-100777, np, err),
          "dispatcher proxies now-playing from the stub engine");
    const parlay::Json* track = np.find("track");
    CHECK(track && track->find("id") &&
              track->find("id")->asString() == "yt:test123",
          "proxied now-playing carries the canned track id");
    CHECK(np.find("engine") && np.find("engine")->asString() == "go",
          "proxied now-playing names the winning engine");

    CHECK(!d.apiNowPlaying(-100999, np, err),
          "unknown chat -> apiNowPlaying false (no state anywhere)");
    CHECK(!d.apiNowPlaying(-100888, np, err),
          "queue-only chat -> apiNowPlaying false (no current track)");

    parlay::Json q;
    CHECK(d.apiQueueList(-100777, q, err) && q.isArray() && q.size() == 2,
          "apiQueueList returns the stub engine's 2-item queue");
    CHECK(d.apiQueueList(-100999, q, err) && q.isArray() && q.size() == 0,
          "apiQueueList for an unknown chat returns an empty queue");

    // Full stack: CoreApi + HttpServer over the real Dispatcher.
    DispatchProvider provider(d);
    parlay::CoreApi api(provider);
    parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
        return api.handle(req);
    });
    CHECK(server.start(), "core api server starts on an ephemeral port");
    std::string base =
        "http://127.0.0.1:" + std::to_string(server.port());
    auto r = parlay::HttpClient::get(base + "/api/now-playing?chat_id=-100777");
    CHECK(r.status == 200 && bodyHas(r.body, "yt:test123"),
          "full stack: /api/now-playing serves the stub engine's track");
    r = parlay::HttpClient::get(base + "/api/queue?chat_id=-100777");
    CHECK(r.status == 200 && bodyHas(r.body, "yt:next1"),
          "full stack: /api/queue serves the stub engine's queue");
    r = parlay::HttpClient::get(base + "/api/queue?chat_id=-100888");
    CHECK(r.status == 200 && bodyHas(r.body, "yt:next2"),
          "full stack: /api/queue falls back to the queue list for a "
          "queue-only chat");
    r = parlay::HttpClient::get(base + "/api/now-playing?chat_id=-100999");
    CHECK(r.status == 404 && bodyHas(r.body, "no playback state"),
          "full stack: unknown chat -> 404 no playback state, never demo");
    r = parlay::HttpClient::get(base + "/healthz");
    CHECK(r.status == 200 && r.body == R"({"ok":true})",
          "full stack: /healthz -> 200");
    server.stop();
    stub.stop();
}

// Shutdown race (Phase 6 Worker C): stop() used to close the listen
// socket and store -1 in listen_fd_ while the accept thread was
// between select() returning and FD_ISSET(listen_fd_); glibc aborted
// the whole process with "*** bit out of range 0 - FD_SETSIZE on
// fd_set ***" (exit 134). The accept loop now snapshots the fd once
// per iteration and stop() exchanges it atomically, so FD_* never
// sees -1. Honest note on this test: it exercises the exact failing
// shape (serve, idle in select(), stop) 25 times, but it did NOT
// reproduce the abort against the old code (250+ cycles pass — in
// this bare process the woken accept thread wins the race; in the
// real core, with its ntgcalls/store threads competing for the CPU,
// the main thread won 8/8). The proof is the binary-level loop, not
// this test: old parlay-core aborted 8/8 SIGTERMs (exit 134), fixed
// parlay-core exits 0 8/8. This test guards the stop path against
// future breakage (deadlock, crash) — it is not the proof itself.
void testHttpServerStopRace() {
    for (int i = 0; i < 25; ++i) {
        FakeProvider p;
        parlay::CoreApi api(p);
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return api.handle(req);
        });
        if (!server.start()) {
            CHECK(false, "stop-race: server starts");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::string base =
            "http://127.0.0.1:" + std::to_string(server.port());
        auto h = parlay::HttpClient::get(base + "/healthz");
        if (h.status != 200) {
            CHECK(false, "stop-race: healthz answers before stop");
            server.stop();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        server.stop();  // idle in select() -> shutdown wakes it
        if (server.running()) {
            CHECK(false, "stop-race: server stops");
            return;
        }
    }
    CHECK(true, "stop-race: 25 start/serve/stop cycles, no abort");
}

}  // namespace

int main() {
    testCoreApiRouting();
    testHttpServerSocket();
    testHttpServerStopRace();
    testDispatcherProxy();
    std::cout << (failures == 0 ? "ALL PASS\n"
                                : "FAILURES: " + std::to_string(failures) + "\n");
    return failures == 0 ? 0 : 1;
}
