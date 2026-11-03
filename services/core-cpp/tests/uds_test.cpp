// uds_test.cpp — tests for the persistent UDS IPC client
// (services/core-cpp/src/ipc.*) against the test stub backend
// (tests/stub_backend.cpp), per contracts/uds-protocol.md.
//
// Covers: frame encode/decode, round-trip play/pause/skip against
// the stub, legacy action/path translation, the duplex inbound hooks
// (JoinRequest + TrackStarted event), deadline timeout behaviour,
// and reconnect after a stub restart. Also measures the UDS
// round-trip in microseconds (Phase 1 telemetry::Timer) and prints
// it — the number recorded in contracts/uds-protocol.md §9.
//
// Build:
//   g++ -std=c++20 -I src tests/uds_test.cpp src/ipc.cpp src/json.cpp
//       src/config.cpp src/toml.cpp -o /tmp/uds_test
//   g++ -std=c++20 -I src tests/stub_backend.cpp src/ipc.cpp
//       src/json.cpp -o /tmp/stub_backend
// Run:
//   /tmp/uds_test /tmp/stub_backend [/tmp/parlay-uds-test.sock]
// (argv[1] defaults to /tmp/stub_backend; the socket path defaults
// to /tmp/parlay-uds-test.sock). No network, no credentials.
// Exit 0 = all checks pass.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"
#include "ipc.hpp"
#include "json.hpp"
#include "telemetry.hpp"

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

using parlay::Json;
using namespace std::chrono_literals;

std::string g_stub_bin = "/tmp/stub_backend";
std::string g_sock = "/tmp/parlay-uds-test.sock";

bool socketExists() {
    struct stat st;
    return ::stat(g_sock.c_str(), &st) == 0;
}

bool waitForSocket(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (socketExists()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

pid_t startStub() {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(g_stub_bin.c_str(), g_stub_bin.c_str(), g_sock.c_str(),
                (char*)nullptr);
        std::perror("execl stub_backend");
        ::_exit(127);
    }
    return pid;
}

void stopStub(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0) return;
        std::this_thread::sleep_for(20ms);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

Json bodyChat(int64_t chat_id) {
    return Json::objectOf({{"chat_id", Json(chat_id)},
                           {"requested_by", Json((int64_t)111)}});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_stub_bin = argv[1];
    if (argc > 2) g_sock = argv[2];
    ::unlink(g_sock.c_str());

    // --- 1. Frame encode/decode (no socket needed) ---
    {
        std::string payload = "{\"type\":\"request\",\"request_id\":1}";
        std::string frame = parlay::uds::encodeFrame(payload);
        CHECK(frame.size() == 4 + payload.size(), "frame: size = 4 + payload");
        CHECK(static_cast<unsigned char>(frame[0]) == (payload.size() & 0xff) &&
                  static_cast<unsigned char>(frame[1]) == 0 &&
                  static_cast<unsigned char>(frame[2]) == 0 &&
                  static_cast<unsigned char>(frame[3]) == 0,
              "frame: length prefix is little-endian uint32");
        int sv[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0,
              "frame: socketpair created");
        CHECK(parlay::uds::writeFrame(sv[0], payload),
              "frame: writeFrame ok");
        std::string got;
        CHECK(parlay::uds::readFrame(sv[1], got) && got == payload,
              "frame: readFrame round-trips the payload");
        ::close(sv[0]);
        ::close(sv[1]);

        bool threw_empty = false, threw_big = false;
        try {
            parlay::uds::encodeFrame("");
        } catch (const std::invalid_argument&) {
            threw_empty = true;
        }
        try {
            parlay::uds::encodeFrame(
                std::string(parlay::uds::kMaxFrameBytes + 1, 'x'));
        } catch (const std::invalid_argument&) {
            threw_big = true;
        }
        CHECK(threw_empty, "frame: empty payload rejected");
        CHECK(threw_big, "frame: >16 MiB payload rejected");
    }

    // --- 2. Legacy name translation (pure, no socket) ---
    {
        using parlay::IpcClient;
        CHECK(IpcClient::canonicalAction("search") == "search.query",
              "translate: search -> search.query");
        CHECK(IpcClient::canonicalAction("download/youtube") ==
                  "download.youtube",
              "translate: download/youtube -> download.youtube");
        CHECK(IpcClient::canonicalAction("loop") == "vc.loop",
              "translate: loop -> vc.loop");
        CHECK(IpcClient::canonicalAction("vc.play") == "vc.play",
              "translate: dotted names pass through");
        CHECK(IpcClient::canonicalAction("bogus") == "bogus",
              "translate: unknown names pass through");
        std::string action;
        Json payload;
        CHECK(IpcClient::getPathToAction("/v1/now-playing?chat_id=-1001",
                                         action, payload) &&
                  action == "vc.now_playing",
              "translate: GET /v1/now-playing -> vc.now_playing");
        const Json* chat = payload.find("chat_id");
        CHECK(chat && chat->asInt() == -1001,
              "translate: GET query chat_id parsed as int");
        CHECK(IpcClient::getPathToAction("/v1/share/abc123", action,
                                         payload) &&
                  action == "share.resolve",
              "translate: GET /v1/share/<token> -> share.resolve");
        CHECK(!IpcClient::getPathToAction("/v1/bogus", action, payload),
              "translate: unknown GET path rejected");
    }

    // --- 3. Round-trip against the stub ---
    pid_t stub = startStub();
    if (!waitForSocket(5000)) {
        std::cout << "FAIL stub did not start (no socket at " << g_sock
                  << ")\n";
        return 1;
    }

    parlay::Config cfg;
    parlay::IpcClient ipc(cfg, g_sock);

    // Inbound hooks: JoinRequest handler + event recorder.
    std::string join_local_sdp;
    bool join_handled = false;
    ipc.setJoinRequestHandler([&](const Json& req) {
        const Json* sdp = req.find("local_sdp");
        join_local_sdp = sdp ? sdp->asString() : "";
        join_handled = true;
        return Json::objectOf(
            {{"remote_sdp", Json("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=core\r\n")},
             {"group_call_id", Json((int64_t)777)}});
    });
    std::mutex ev_mu;
    std::condition_variable ev_cv;
    std::string last_event;
    int64_t last_event_chat = 0;
    ipc.setEventHandler([&](const std::string& event, const Json& data) {
        std::lock_guard<std::mutex> lk(ev_mu);
        last_event = event;
        const Json* c = data.find("chat_id");
        last_event_chat = c ? c->asInt() : 0;
        ev_cv.notify_all();
    });

    {
        CHECK(ipc.healthz("", 5), "round-trip: healthz (ping) ok");
        CHECK(ipc.connected(), "round-trip: connected after healthz");

        parlay::IpcResult play = ipc.post(
            "", "play", Json::objectOf(
                            {{"chat_id", Json((int64_t)-1001)},
                             {"query", Json("Stub Song")},
                             {"requested_by", Json((int64_t)111)}}));
        CHECK(play.ok, "round-trip: vc.play ok");
        const Json* np = play.data.find("now_playing");
        const Json* track = np ? np->find("track") : nullptr;
        const Json* title = track ? track->find("title") : nullptr;
        CHECK(title && title->asString() == "Stub Song",
              "round-trip: vc.play returns the stub track");

        // TrackStarted event arrives on the same socket.
        {
            std::unique_lock<std::mutex> lk(ev_mu);
            bool seen = ev_cv.wait_for(lk, 2s, [&] {
                return last_event == "TrackStarted";
            });
            CHECK(seen && last_event_chat == -1001,
                  "round-trip: TrackStarted event received with chat_id");
        }

        parlay::IpcResult pause =
            ipc.post("", "pause", bodyChat(-1001));
        CHECK(pause.ok, "round-trip: vc.pause ok");
        parlay::IpcResult skip = ipc.post("", "skip", bodyChat(-1001));
        CHECK(skip.ok, "round-trip: vc.skip ok");

        // Legacy GET path translation over the socket.
        parlay::IpcResult now =
            ipc.get("", "/v1/now-playing?chat_id=-1001", 10);
        CHECK(now.ok && now.data.find("now_playing") != nullptr,
              "round-trip: GET /v1/now-playing ok");

        // Legacy slash-form POST actions.
        parlay::IpcResult search =
            ipc.post("", "search",
                     Json::objectOf({{"query", Json("stub")}}), 15);
        CHECK(search.ok && search.data.find("results") != nullptr,
              "round-trip: legacy search -> search.query ok");
        parlay::IpcResult dl = ipc.post(
            "", "download/youtube",
            Json::objectOf({{"query", Json("stub")},
                            {"requested_by", Json((int64_t)111)}}),
            120);
        CHECK(dl.ok && dl.data.find("run_id") != nullptr,
              "round-trip: legacy download/youtube ok with run_id");

        // Full sweep: every one of the 56 contract actions answers.
        static const char* kAll56[] = {
            "chat.settings", "download.deezer", "download.navidrome",
            "download.progress", "download.soulseek", "download.spotify",
            "download.spotify_playlist", "download.tiktok",
            "download.youtube", "fx.apply", "gui.now_playing", "gui.queue",
            "lemonic.download", "lemonic.search", "netease.download",
            "netease.lyrics", "netease.search", "queue.vote", "radio.list",
            "radio.start", "radio.status", "radio.stop", "search.inline",
            "search.query", "share.create", "share.resolve",
            "spotify.analyze", "spotify.details", "spotify.auth",
            "spotify.dislike", "spotify.now_playing", "spotify.resolve",
            "spotify.skip_check", "spotify.watch", "spotify.watches",
            "vc.join", "vc.lyrics", "vc.mute", "vc.now_playing",
            "vc.progress", "vc.panel", "vc.pause", "vc.play", "vc.queue",
            "vc.radio", "vc.resume", "vc.seek", "vc.skip", "vc.stop",
            "vc.loop", "vc.shuffle", "vc.autoplay", "vc.autoplay_skip",
            "spotify.collection", "spotify.status", "vc.volume",
        };
        int sweep_ok = 0;
        for (const char* a : kAll56) {
            Json body = Json::objectOf(
                {{"chat_id", Json((int64_t)-1001)},
                 {"user_id", Json((int64_t)111)},
                 {"requested_by", Json((int64_t)111)},
                 {"query", Json("stub")},
                 {"url", Json("https://example.invalid/x")},
                 {"track_id", Json("stub-track-1")},
                 {"run_id", Json("abcdef123456")},
                 {"station", Json("stub-fm")},
                 {"token", Json("stubtoken")},
                 {"action", Json("login")},
                 {"effect", Json("nightcore")},
                 {"input_path", Json("/tmp/stub/in.opus")},
                 {"vote", Json("approve")},
                 {"muted", Json(true)},
                 {"volume", Json((int64_t)100)},
                 {"count", Json((int64_t)2)},
                 {"enabled", Json(true)},
                 {"position_sec", Json((int64_t)10)}});
            parlay::IpcResult r = ipc.post("", a, body);
            if (r.ok) ++sweep_ok;
            else
                std::cout << "  sweep FAIL " << a << ": " << r.describe()
                          << "\n";
        }
        CHECK(sweep_ok == 56, "round-trip: all 56 contract actions answer "
                              "(got " +
                                  std::to_string(sweep_ok) + "/56)");

        // vc.join exercises the duplex JoinRequest hook.
        join_handled = false;
        parlay::IpcResult join = ipc.post("", "join", bodyChat(-1001));
        CHECK(join.ok, "round-trip: vc.join ok");
        CHECK(join_handled, "duplex: JoinRequest handler ran");
        CHECK(join_local_sdp.rfind("v=0", 0) == 0,
              "duplex: JoinRequest carried a local_sdp");
        const Json* gc = join.data.find("group_call_id");
        CHECK(gc && gc->asInt() == 777,
              "duplex: vc.join returns the handler's group_call_id");

        // Unknown action -> honest not_found, not a transport error.
        parlay::IpcResult bogus =
            ipc.post("", "bogus.action", bodyChat(-1001));
        CHECK(!bogus.ok && bogus.error_code == "not_found" &&
                  bogus.http_status == 404,
              "round-trip: unknown action is a 404, not unavailable");
        CHECK(!bogus.unavailable("bogus.action"),
              "round-trip: unknown-action 404 is not unavailable");
    }

    // --- 4. Deadline timeout behaviour ---
    {
        auto t0 = std::chrono::steady_clock::now();
        parlay::IpcResult r =
            ipc.post("", "test.sleep_ms",
                     Json::objectOf({{"sleep_ms", Json((int64_t)5000)}}),
                     1);
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        CHECK(!r.ok && r.error_code == "engine_unavailable" &&
                  r.http_status == 0,
              "deadline: 5 s stub sleep with a 1 s deadline fails "
              "engine_unavailable");
        CHECK(elapsed_ms >= 900 && elapsed_ms < 3000,
              "deadline: call returned in ~1 s, not 5 s (took " +
                  std::to_string(elapsed_ms) + " ms)");
    }

    // --- 5. Reconnect after stub restart ---
    {
        stopStub(stub);
        std::this_thread::sleep_for(300ms);  // let the reader see EOF
        parlay::IpcResult down =
            ipc.post("", "vc.now_playing", bodyChat(-1001), 2);
        CHECK(!down.ok && down.error_code == "engine_unavailable",
              "reconnect: call while stub is down fails cleanly");
        stub = startStub();
        CHECK(waitForSocket(5000), "reconnect: stub restarted");
        parlay::IpcResult up =
            ipc.post("", "vc.now_playing", bodyChat(-1001));
        CHECK(up.ok, "reconnect: next call after restart succeeds");
    }

    // --- 6. UDS round-trip measurement (Phase 1 telemetry) ---
    {
        const int kSamples = 200;
        std::vector<int64_t> us;
        us.reserve(kSamples);
        parlay::telemetry::Timer timer;
        for (int i = 0; i < kSamples; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            bool ok = ipc.healthz("", 5);
            auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            if (!ok) {
                std::cout << "FAIL benchmark: healthz failed mid-run\n";
                ++failures;
                break;
            }
            us.push_back(dt);
        }
        if ((int)us.size() == kSamples) {
            std::vector<int64_t> sorted = us;
            std::sort(sorted.begin(), sorted.end());
            int64_t sum =
                std::accumulate(sorted.begin(), sorted.end(), (int64_t)0);
            int64_t mean = sum / kSamples;
            int64_t median = sorted[kSamples / 2];
            int64_t p95 = sorted[(kSamples * 95) / 100];
            std::cout << "[benchmark] uds_roundtrip_us n=" << kSamples
                      << " mean=" << mean << " median=" << median
                      << " p95=" << p95 << " min=" << sorted.front()
                      << " max=" << sorted.back() << "\n";
            timer.stage("uds_roundtrip", mean);
            timer.emit("t_ipc_benchmark");
            CHECK(mean < 5000,
                  "benchmark: mean UDS round-trip under 5 ms (mean=" +
                      std::to_string(mean) + " us)");
            CHECK(ipc.lastCallUs() > 0,
                  "benchmark: lastCallUs reports the last round-trip");
        }
    }

    stopStub(stub);
    std::cout << (failures == 0 ? "ALL UDS TESTS PASSED"
                                : "UDS TESTS FAILED") +
                     std::string(" (") + std::to_string(failures) +
                     " failures)\n";
    return failures == 0 ? 0 : 1;
}
