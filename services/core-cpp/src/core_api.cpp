// core_api.cpp — the core's GUI-facing HTTP API (see core_api.hpp).
#include "core_api.hpp"

namespace parlay {
namespace {

HttpResponse jsonResponse(int status, const Json& body) {
    return HttpResponse{status, body.dump()};
}

HttpResponse errorResponse(int status, const std::string& message) {
    return jsonResponse(status, Json::objectOf({{"ok", Json(false)},
                                                {"error", Json(message)}}));
}

// IPC envelope error (contracts/ipc.json): {ok:false, error:{code,
// message}}. Used by the /v1/* IPC actions, unlike the flat GUI
// errors above.
HttpResponse ipcErrorResponse(int status, const std::string& code,
                              const std::string& message) {
    return jsonResponse(status, Json::objectOf({
                                    {"ok", Json(false)},
                                    {"error", Json::objectOf({
                                                  {"code", Json(code)},
                                                  {"message", Json(message)},
                                              })},
                                }));
}

// Strict int64 parse: optional leading '-', digits only. Chat ids are
// negative for groups, so "-100123" is valid; "12x" and "" are not.
bool parseChatId(const std::string& raw, int64_t& out) {
    if (raw.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (raw[0] == '-') {
        neg = true;
        i = 1;
        if (raw.size() == 1) return false;
    }
    int64_t v = 0;
    for (; i < raw.size(); ++i) {
        char c = raw[i];
        if (c < '0' || c > '9') return false;
        // Overflow guard: refuse rather than wrap.
        if (v > (INT64_MAX - (c - '0')) / 10) return false;
        v = v * 10 + (c - '0');
    }
    out = neg ? -v : v;
    return true;
}

}  // namespace

HttpResponse CoreApi::handle(const HttpRequest& req) const {
    // vc.join / vc.mute are POST IPC actions (their own envelope, see
    // handleJoin / handleMute); everything else on this server is
    // GET-only.
    if (req.path == "/v1/join") return handleJoin(req);
    if (req.path == "/v1/mute") return handleMute(req);
    if (req.method != "GET")
        return errorResponse(405, "method not allowed");

    if (req.path == "/healthz")
        return jsonResponse(200, Json::objectOf({{"ok", Json(true)}}));

    if (req.path != "/api/now-playing" && req.path != "/api/queue")
        return errorResponse(404, "not found");

    auto it = req.query.find("chat_id");
    int64_t chat_id = 0;
    if (it == req.query.end() || !parseChatId(it->second, chat_id))
        return errorResponse(400, "chat_id query param is required");

    if (req.path == "/api/now-playing") {
        Json np;
        std::string err;
        if (!provider_.nowPlaying(chat_id, np, err))
            return errorResponse(404, "no playback state");
        return jsonResponse(
            200, Json::objectOf({{"ok", Json(true)},
                                 {"now_playing", np}}));
    }

    // /api/queue: the queue field of the live now-playing object.
    Json np;
    std::string err;
    if (provider_.nowPlaying(chat_id, np, err)) {
        const Json* q = np.find("queue");
        if (!q || !q->isArray())
            return errorResponse(502, "engine did not report a queue");
        return jsonResponse(200, Json::objectOf({{"ok", Json(true)},
                                                 {"queue", *q},
                                                 {"chat_id", Json(chat_id)}}));
    }
    // Now-playing reports no state (e.g. a vc-ts room with a queue but
    // no current track 404s there): the queue list may still be real.
    // Only a non-empty list counts — an empty list is "no playback state".
    Json q;
    if (provider_.queueList(chat_id, q, err) && q.isArray() &&
        q.size() > 0) {
        return jsonResponse(200, Json::objectOf({{"ok", Json(true)},
                                                 {"queue", q},
                                                 {"chat_id", Json(chat_id)}}));
    }
    return errorResponse(404, "no playback state");
}

// POST /v1/join — vc.join (contracts/ipc.json). The vc-ts / vc-rust
// engines call this from their join(): they hold no MTProto client,
// so the core's TDLib backend owns the join. IPC envelope throughout.
HttpResponse CoreApi::handleJoin(const HttpRequest& req) const {
    if (req.method != "POST")
        return ipcErrorResponse(405, "method_not_allowed",
                                "method not allowed");
    if (!parlay_token_.empty()) {
        auto it = req.headers.find("x-parlay-token");
        if (it == req.headers.end() || it->second != parlay_token_)
            return ipcErrorResponse(401, "unauthorized",
                                    "missing or wrong X-Parlay-Token");
    }
    int64_t chat_id = 0;
    bool have_chat = false;
    try {
        Json body = Json::parse(req.body);
        const Json* c = body.find("chat_id");
        if (c && c->isInt()) {
            chat_id = c->asInt();
            have_chat = true;
        }
    } catch (...) {
        have_chat = false;
    }
    if (!have_chat)
        return ipcErrorResponse(400, "bad_request",
                                "chat_id is required (integer)");
    std::string err;
    if (!provider_.joinVoice(chat_id, err))
        return ipcErrorResponse(502, "engine_unavailable",
                                err.empty() ? "voice join failed" : err);
    // The contract's now_playing: the engine's live state when one
    // exists (e.g. the chat was already playing), null otherwise.
    Json np;
    std::string np_err;
    Json np_out = Json(nullptr);
    if (provider_.nowPlaying(chat_id, np, np_err)) np_out = np;
    return jsonResponse(200, Json::objectOf({
                                {"ok", Json(true)},
                                {"data", Json::objectOf({
                                             {"joined", Json(true)},
                                             {"chat_id", Json(chat_id)},
                                             {"now_playing", np_out},
                                         })},
                            }));
}

// POST /v1/mute — vc.mute (contracts/ipc.json). Mirrors handleJoin:
// same IPC envelope, same X-Parlay-Token gate, same error shape.
// "muted" defaults to true (mute) when absent; a non-boolean value
// is a 400, never a mute attempt.
HttpResponse CoreApi::handleMute(const HttpRequest& req) const {
    if (req.method != "POST")
        return ipcErrorResponse(405, "method_not_allowed",
                                "method not allowed");
    if (!parlay_token_.empty()) {
        auto it = req.headers.find("x-parlay-token");
        if (it == req.headers.end() || it->second != parlay_token_)
            return ipcErrorResponse(401, "unauthorized",
                                    "missing or wrong X-Parlay-Token");
    }
    int64_t chat_id = 0;
    bool have_chat = false;
    bool muted = true;
    try {
        Json body = Json::parse(req.body);
        const Json* c = body.find("chat_id");
        if (c && c->isInt()) {
            chat_id = c->asInt();
            have_chat = true;
        }
        const Json* m = body.find("muted");
        if (m) {
            if (!m->isBool())
                return ipcErrorResponse(400, "bad_request",
                                        "\"muted\" must be a boolean");
            muted = m->asBool();
        }
    } catch (...) {
        have_chat = false;
    }
    if (!have_chat)
        return ipcErrorResponse(400, "bad_request",
                                "chat_id is required (integer)");
    std::string err;
    if (!provider_.muteVoice(chat_id, muted, err))
        return ipcErrorResponse(502, "engine_unavailable",
                                err.empty() ? "voice mute failed" : err);
    // The contract's now_playing: the engine's live state when one
    // exists, null otherwise.
    Json np;
    std::string np_err;
    Json np_out = Json(nullptr);
    if (provider_.nowPlaying(chat_id, np, np_err)) np_out = np;
    return jsonResponse(200, Json::objectOf({
                                {"ok", Json(true)},
                                {"data", Json::objectOf({
                                             {"muted", Json(muted)},
                                             {"chat_id", Json(chat_id)},
                                             {"now_playing", np_out},
                                         })},
                            }));
}

}  // namespace parlay
