// core_api.hpp — the core's HTTP API (Phase 6, A2; vc.join in Phase 7).
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
// POST /v1/join is the IPC action vc.join (contracts/ipc.json), so it
// uses the IPC envelope instead of the flat GUI shape:
//   POST /v1/join  {"chat_id":<id>}  + X-Parlay-Token when configured
//     200 {"ok":true,"data":{"joined":true,"chat_id":<id>,
//         "now_playing":{...}|null}} — the core's TDLib backend joined
//         the chat's voice call (authorize -> createOffer ->
//         getChat/createVideoChat -> joinVideoChat -> ntg.connect).
//         now_playing is the engine's live state when one exists.
//     502 {"ok":false,"error":{"code":"engine_unavailable",
//         "message":"<exact reason>"}} — the backend cannot join
//         (no session/credentials, no .so, TDLib/ntgcalls error).
//         joined:true is NEVER reported without a real join.
//     400 bad/missing chat_id; 401 missing or wrong X-Parlay-Token
//         (only when a token is configured); 405 non-POST.
// The vc-ts / vc-rust engines call this endpoint from their join():
// they hold no MTProto client and must never claim a join the core
// did not make.
//
// POST /v1/mute is the IPC action vc.mute (contracts/ipc.json), same
// envelope and same auth gate:
//   POST /v1/mute  {"chat_id":<id>,"muted":<bool>}  + X-Parlay-Token
//     200 {"ok":true,"data":{"muted":<bool>,"chat_id":<id>,
//         "now_playing":{...}|null}} — the core's TDLib backend muted
//         (muted:true) or unmuted (muted:false) the chat's voice
//         call. "muted" defaults to true when absent. now_playing is
//         the engine's live state when one exists.
//     502 {"ok":false,"error":{"code":"engine_unavailable",
//         "message":"<exact reason>"}} — the backend cannot mute:
//         the session is owned by a vc engine (the core cannot mute
//         a session it does not hold), nothing is playing, or the
//         backend mute/unmute failed. muted:true is NEVER reported
//         without a real backend mute.
//     400 bad/missing chat_id, or "muted" present but not a boolean;
//     401 missing or wrong X-Parlay-Token (only when a token is
//     configured); 405 non-POST.
//
// VcStateProvider is the seam between these endpoints and the engines:
// the production provider asks the Dispatcher (vcGet with engine
// fail-over, see dispatcher.cpp); tests script it, or point the
// Dispatcher at a stub engine.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

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
    // Join the chat's voice call on the core's TDLib backend
    // (vc.join). False + error text when the backend cannot join;
    // true only after a real join (or when the chat is already in a
    // call). Default: not available — providers that do not override
    // it report so honestly instead of faking a join.
    virtual bool joinVoice(int64_t chat_id, std::string& error) {
        (void)chat_id;
        error = "voice join not available on this provider";
        return false;
    }
    // Mute (muted=true) or unmute (muted=false) the chat's voice
    // call on the core's TDLib backend (vc.mute). False + error text
    // when the backend cannot mute (engine-owned session, nothing
    // playing, backend failure); true only after a real backend
    // mute/unmute. Default: not available, same as joinVoice.
    virtual bool muteVoice(int64_t chat_id, bool muted,
                           std::string& error) {
        (void)chat_id;
        (void)muted;
        error = "voice mute not available on this provider";
        return false;
    }
};

class CoreApi {
public:
    // parlay_token: when non-empty, POST /v1/join and POST /v1/mute
    // require the X-Parlay-Token header to match (the IPC auth
    // rule); the read-only GET endpoints stay public. Empty = no
    // check (the core warns at startup when PARLAY_TOKEN is unset).
    explicit CoreApi(VcStateProvider& provider,
                     std::string parlay_token = "")
        : provider_(provider), parlay_token_(std::move(parlay_token)) {}
    HttpResponse handle(const HttpRequest& req) const;

private:
    HttpResponse handleJoin(const HttpRequest& req) const;
    HttpResponse handleMute(const HttpRequest& req) const;

    VcStateProvider& provider_;
    std::string parlay_token_;
};

}  // namespace parlay
