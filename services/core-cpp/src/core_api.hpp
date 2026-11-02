// core_api.hpp — the core's GUI-facing HTTP API (Phase 6, A2).
//
// Endpoints (flat JSON, NOT the IPC envelope — see contracts/README.md):
//   GET /healthz                  {"ok":true}
//   GET /api/now-playing?chat_id=<id>
//     200 {"ok":true,"now_playing":{...}} — the chat engine's live state
//     404 {"ok":false,"error":"no playback state"} — no chat state, or no
//         engine reachable. Never demo/fake data.
//     400 {"ok":false,"error":"chat_id query param is required"}
//   GET /api/queue?chat_id=<id>
//     200 {"ok":true,"queue":[...],"chat_id":<id>} — the queue field of
//         the live now-playing object; when now-playing reports no state
//         but the engine's queue list is non-empty, that list is returned.
//     404 {"ok":false,"error":"no playback state"}
//     502 {"ok":false,"error":"engine did not report a queue"} —
//         now-playing answered but carried no queue field (all three
//         engines include one, so this should not happen).
//
// VcStateProvider is the seam between these endpoints and the engines:
// the production provider asks the Dispatcher (vcGet with engine
// fail-over, see dispatcher.cpp); tests script it, or point the
// Dispatcher at a stub engine.
#pragma once

#include <cstdint>
#include <string>

#include "http_server.hpp"
#include "json.hpp"

namespace parlay {

class VcStateProvider {
public:
    virtual ~VcStateProvider() = default;
    // Live now-playing object for the chat, or false + error text.
    // False covers both "no playback state" and "no engine reachable":
    // the API answers both the same honest way.
    virtual bool nowPlaying(int64_t chat_id, Json& now_playing_out,
                            std::string& error) = 0;
    // The engine's queue list for the chat (POST /v1/queue action=list).
    virtual bool queueList(int64_t chat_id, Json& queue_out,
                           std::string& error) = 0;
};

class CoreApi {
public:
    explicit CoreApi(VcStateProvider& provider) : provider_(provider) {}
    HttpResponse handle(const HttpRequest& req) const;

private:
    VcStateProvider& provider_;
};

}  // namespace parlay
