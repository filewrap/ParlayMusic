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

}  // namespace parlay
