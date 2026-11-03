// core_peer.cpp — interop probe: the REAL C++ core UDS client
// (parlay::IpcClient, services/core-cpp/src/ipc.*) speaking to the
// Go backend. This is the byte-compatibility proof for Phase 3:
// the frames the Go server reads/writes must be exactly what
// parlay::uds::encodeFrame / readFrame / writeFrame produce and
// accept.
//
// Wave 2 (Phase 3 verification): dispatches ALL 65 contract
// actions (contracts/uds-protocol.md §4 rows 1–65: the 56 IPC
// actions + the 9 OD-3 actions) against the real running backend
// and counts them. Payloads are chosen to answer fast — empty
// required fields (bad_request before any network/voice work),
// empty-state reads (not_found / empty 200), or key-gated honest
// errors (502/503 with no creds). A check PASSES when the backend
// answers with a real response frame — ok:true, or any honest
// error code (bad_request / not_found / engine_unavailable /
// download_failed …). It FAILS on a transport failure
// (http_status 0 / timeout) or on the "unknown action" error,
// which is the only signal that the action is not registered.
// A not_found for an empty room ("nothing playing") is a pass:
// the handler ran and answered honestly.
//
// Build (from the repo root):
//   g++ -std=c++20 -I services/core-cpp/src \
//       services/backend-go/tests/interop/core_peer.cpp \
//       services/core-cpp/src/ipc.cpp services/core-cpp/src/json.cpp \
//       services/core-cpp/src/config.cpp services/core-cpp/src/toml.cpp \
//       -o /tmp/core_peer
// Run (backend must already be listening, dummy creds):
//   /tmp/core_peer <socket-path>
// Exits 0 only when every check passes.
#include <iostream>
#include <string>
#include <vector>

#include "config.hpp"
#include "ipc.hpp"
#include "json.hpp"

namespace {
int failures = 0;
int answered = 0;
void check(bool ok, const std::string& name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
    if (!ok) ++failures;
}

// Dispatch one action and report its outcome. Returns true when
// the backend answered with a real response (ok or honest error);
// false on transport failure or "unknown action".
bool dispatch(parlay::IpcClient& ipc, const std::string& action,
              const parlay::Json& body, int timeout_sec,
              std::string* outcome) {
    parlay::IpcResult r = ipc.post("", action, body, timeout_sec);
    if (r.ok) {
        *outcome = "ok";
        ++answered;
        return true;
    }
    if (r.http_status == 0) {
        *outcome = "TRANSPORT FAILURE (no response)";
        return false;
    }
    if (r.error_code == "not_found" &&
        r.error_message.rfind("unknown action", 0) == 0) {
        *outcome = "UNKNOWN ACTION: " + r.error_message;
        return false;
    }
    *outcome = r.error_code + " (" + r.error_message + ")";
    ++answered;
    return true;
}

void probe(parlay::IpcClient& ipc, const std::string& action,
           const parlay::Json& body, int timeout_sec = 10) {
    std::string outcome;
    bool ok = dispatch(ipc, action, body, timeout_sec, &outcome);
    std::cout << (ok ? "PASS " : "FAIL ") << action << " -> " << outcome
              << "\n";
    if (!ok) ++failures;
}

parlay::Json obj(std::initializer_list<std::pair<const char*, parlay::Json>> kv) {
    return parlay::Json::objectOf(kv);
}
parlay::Json s(const char* v) { return parlay::Json(std::string(v)); }
parlay::Json i(int64_t v) { return parlay::Json(v); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: core_peer <socket-path>\n";
        return 2;
    }
    parlay::Config cfg;
    parlay::IpcClient ipc(cfg, argv[1]);

    bool join_handled = false;
    ipc.setJoinRequestHandler([&](const parlay::Json& req) {
        join_handled = true;
        (void)req;
        return parlay::Json::objectOf(
            {{"remote_sdp",
              parlay::Json("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=core\r\n")},
             {"group_call_id", parlay::Json((int64_t)777)}});
    });
    std::string last_event;
    ipc.setEventHandler([&](const std::string& event, const parlay::Json&) {
        last_event = event;
    });

    check(ipc.healthz("", 5), "healthz (ping) ok over the real IpcClient");
    check(ipc.connected(), "connected after healthz");

    parlay::IpcResult ping = ipc.post(
        "", "ping", parlay::Json::objectOf({}));
    check(ping.ok, "post ping ok");
    const parlay::Json* svc =
        ping.ok ? ping.data.find("service") : nullptr;
    check(svc && svc->asString() == "backend-go",
          "ping data.service == backend-go");

    // test.sleep_ms is stub-only: the real backend answers not_found.
    parlay::IpcResult sleep = ipc.post(
        "", "test.sleep_ms",
        parlay::Json::objectOf({{"sleep_ms", parlay::Json((int64_t)1)}}));
    check(!sleep.ok && sleep.error_code == "not_found",
          "test.sleep_ms answers not_found (stub-only)");

    // --- The 65 contract actions (uds-protocol.md §4 rows 1–65) ---
    const parlay::Json chat = obj({{"chat_id", i(-1001)}});
    const parlay::Json empty = parlay::Json::objectOf({});
    const parlay::Json track =
        obj({{"id", s("yt:abc")}, {"title", s("Song")}});

    // Rows 1–56: the 56 IPC actions.
    probe(ipc, "chat.settings", chat);
    probe(ipc, "download.deezer", obj({{"query", s("")}}));
    probe(ipc, "download.navidrome", obj({{"url", s("")}}));
    probe(ipc, "download.progress", obj({{"run_id", s("nope")}}));
    probe(ipc, "download.soulseek", obj({{"query", s("")}}));
    probe(ipc, "download.spotify", obj({{"url", s("")}}));
    probe(ipc, "download.spotify_playlist",
          obj({{"url", s("")}, {"mode", s("zip")}}));
    probe(ipc, "download.tiktok", obj({{"url", s("")}}));
    probe(ipc, "download.youtube", obj({{"query", s("")}}));
    probe(ipc, "fx.apply",
          obj({{"effect", s("bogus")},
               {"input_path", s("/tmp/x.mp3")},
               {"requested_by", i(1)}}));
    probe(ipc, "gui.now_playing", chat);
    probe(ipc, "gui.queue", chat);
    probe(ipc, "lemonic.download", obj({{"track_id", s("")}}));
    probe(ipc, "lemonic.search", obj({{"query", s("")}}));
    probe(ipc, "netease.download", obj({{"track_id", s("")}}));
    probe(ipc, "netease.lyrics", obj({{"track_id", s("")}}));
    probe(ipc, "netease.search", obj({{"query", s("")}}));
    probe(ipc, "queue.vote",
          obj({{"track", track},
               {"vote", s("approve")},
               {"voter_id", i(1)},
               {"chat_id", i(-1001)}}));
    probe(ipc, "radio.list", empty);
    probe(ipc, "radio.start",
          obj({{"chat_id", i(-1001)}, {"station", s("nope")}}));
    probe(ipc, "radio.status", chat);
    probe(ipc, "radio.stop", chat);
    probe(ipc, "search.inline",
          obj({{"query", s("")}, {"user_id", i(1)}, {"chat_id", i(-1001)}}));
    probe(ipc, "search.query", obj({{"query", s("")}}));
    probe(ipc, "share.create",
          obj({{"track", track}, {"created_by", i(1)}}));
    probe(ipc, "share.resolve", obj({{"token", s("nope")}}));
    probe(ipc, "spotify.analyze",
          obj({{"user_id", i(1)}, {"track_id", s("x")}}));
    probe(ipc, "spotify.details",
          obj({{"user_id", i(1)}, {"track_id", s("x")}}));
    probe(ipc, "spotify.auth",
          obj({{"action", s("login")}, {"user_id", i(1)}}));
    probe(ipc, "spotify.dislike",
          obj({{"action", s("add")},
               {"track_id", s("x")},
               {"user_id", i(1)}}));
    probe(ipc, "spotify.now_playing", obj({{"user_id", i(1)}}));
    probe(ipc, "spotify.resolve", obj({{"url", s("not-a-url")}}));
    probe(ipc, "spotify.skip_check", obj({{"track", empty}}));
    probe(ipc, "spotify.watch",
          obj({{"action", s("start")}, {"user_id", i(1)}}));
    probe(ipc, "spotify.watches", empty);
    // vc.join performs a real JoinRequest handshake against this
    // probe's handler (fake remote SDP) and a real ntgcalls
    // connect against it — allow the longer budget. ntg_connect
    // against a fake SDP (no media lines, no real group call)
    // may block past any deadline; see the report if it fails.
    probe(ipc, "vc.join", chat, 60);
    std::cout << "join_request handler ran: "
              << (join_handled ? "yes" : "no") << "\n";
    probe(ipc, "vc.lyrics", chat);
    probe(ipc, "vc.mute", obj({{"chat_id", i(-1001)}, {"muted", true}}));
    probe(ipc, "vc.now_playing", chat);
    probe(ipc, "vc.progress", chat);
    probe(ipc, "vc.panel", chat);
    probe(ipc, "vc.pause", chat);
    probe(ipc, "vc.play",
          obj({{"chat_id", i(-1001)},
               {"query", s("")},
               {"requested_by", i(111)}}));
    probe(ipc, "vc.queue",
          obj({{"action", s("list")}, {"chat_id", i(-1001)}}));
    probe(ipc, "vc.radio",
          obj({{"action", s("start")},
               {"chat_id", i(-1001)},
               {"station", s("nope")}}));
    probe(ipc, "vc.resume", chat);
    probe(ipc, "vc.seek",
          obj({{"chat_id", i(-1001)}, {"position_sec", i(-1)}}));
    probe(ipc, "vc.skip", chat);
    probe(ipc, "vc.stop", chat);
    probe(ipc, "vc.loop", obj({{"chat_id", i(-1001)}, {"count", i(3)}}));
    probe(ipc, "vc.shuffle",
          obj({{"chat_id", i(-1001)}, {"enabled", true}}));
    probe(ipc, "vc.autoplay",
          obj({{"chat_id", i(-1001)}, {"enabled", true}}));
    probe(ipc, "vc.autoplay_skip", chat);
    probe(ipc, "spotify.collection",
          obj({{"url", s("https://open.spotify.com/playlist/xxx")}}));
    probe(ipc, "spotify.status", empty);
    probe(ipc, "vc.volume",
          obj({{"chat_id", i(-1001)}, {"volume", i(50)}}));

    // Rows 57–65: the 9 OD-3 actions.
    probe(ipc, "download.cancel", obj({{"run_id", s("nope")}}));
    probe(ipc, "library.delete", obj({{"id", s("nope")}}));
    probe(ipc, "mix.generate", chat);
    probe(ipc, "audio.recognize", obj({{"file_path", s("/tmp/x.mp3")}}));
    probe(ipc, "library.scan", empty);
    probe(ipc, "library.sharescan", empty);
    probe(ipc, "cache.clear", obj({{"scope", s("all")}}));
    probe(ipc, "download.prefs", obj({{"user_id", i(42)}}));
    // Missing file on disk: not_found before any voice join.
    probe(ipc, "vc.play_file",
          obj({{"chat_id", i(-1001)}, {"file_path", s("missing.mp3")}}));

    (void)last_event;
    std::cout << "CORE_PEER: " << answered
              << "/65 actions answered, " << failures << " failures\n";
    std::cout << (failures == 0 ? "CORE_PEER: ALL PASS\n"
                                : "CORE_PEER: FAILURES\n");
    return failures == 0 ? 0 : 1;
}
