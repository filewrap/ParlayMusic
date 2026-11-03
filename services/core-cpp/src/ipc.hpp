// ipc.hpp — Parlay IPC client: ONE persistent duplex Unix Domain Socket
// to the Go backend (FINAL-ARCHITECTURE.md §1.3 Part 1, §9 Phase 2;
// wire spec: contracts/uds-protocol.md).
//
// This REPLACES the old HTTP transport (services/core-cpp/src/http.*,
// deleted): one subprocess fork per call, 30/10/5 s timeouts, and
// the per-engine base URLs are gone. There is exactly one backend now,
// reached over a single AF_UNIX socket. The public API keeps the old
// method names/signatures (post/get/healthz) so engine_router.cpp and
// the FakeIpc test doubles keep compiling — the fallback router itself
// is deleted in Wave 2, not here.
//
//   * post(action, body, timeout_sec): the no-URL form — every call
//     goes to the one backend on the UDS socket. This is the form the
//     dispatcher uses (Phase 4, Q2: the dead per-service base URLs
//     are gone). The base_url forms below are kept only for the
//     FakeIpc test doubles, which override the virtuals; they forward
//     to the same call with the URL ignored.
//   * get(path, timeout_sec): the no-URL form of the legacy GET.
//   * healthz(timeout_sec): the no-URL form of the ping.
//   * post(base_url, action, body, timeout_sec): the base_url argument
//     is accepted for signature compatibility and IGNORED — every call
//     goes to the one backend on the UDS socket. Legacy slash-form
//     action names ("search", "netease/search", "download/youtube",
//     "radio/start", "share", "fx", "vote", "dislike", vc short names)
//     are translated to the dotted contract actions
//     (contracts/ipc.json) by canonicalAction().
//   * get(base_url, path, timeout_sec): the legacy "/v1/<action>?..."
//     path is translated to a contract action + payload (see
//     getPathToAction()); the base_url is likewise ignored.
//   * healthz(base_url, timeout_sec): a "ping" request over the socket.
//     True only on {ok:true}.
//
// Transport: length-prefixed frames (parlay::uds), request/response
// multiplexing by request_id, per-action deadlines, reconnect with
// exponential backoff. Backend-initiated traffic on the same socket:
// events (TrackStarted, TrackAdvanced, ...) via setEventHandler(), and
// JoinRequest(chat_id, local_sdp) via setJoinRequestHandler() — the
// handler runs on a worker thread and its returned Json becomes the
// JoinResponse data {remote_sdp, group_call_id}. Wave 2 wires the real
// TDLib joinVideoChat handler; this file only provides the hook.
//
// Deadlines (CONFIG SEAM — resolved in Phase 4, Q6): the socket
// path comes from Config::ipc_socket_path — parlay.toml [ipc]
// socket_path, overridable with the PARLAY_UDS_PATH env var — and
// falls back to <data_dir>/parlay-backend.sock (see defaultSocketPath
// in ipc.cpp). The per-action deadline table lives here with defaults
// equal to the old hardcoded values (30 s general, 10 s GET-style,
// 5 s ping) so behaviour does not change. The config rewrite should
// populate it from parlay.toml [timeouts] (resolve_ms, join_ms,
// ipc_cmd_ms, download_ms) via setDeadline()/setDefaultDeadlineMs().
// Until then the table is the source of truth; the legacy timeout_sec
// parameter governs only actions with no table entry.
//
// Threading: one reader thread per connection. Calls block on a
// std::future until their deadline; a timed-out or failed call tears
// the connection down (a peer that stops answering is not trusted
// again) and the next call reconnects with backoff.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "json.hpp"

namespace parlay {

// ---------------------------------------------------------------------------
// Wire framing (contracts/uds-protocol.md §2). Shared by the core client
// and the test stub backend (services/core-cpp/tests/stub_backend.cpp).
// ---------------------------------------------------------------------------
namespace uds {

// Max payload bytes in one frame. 16 MiB: far above any envelope (the
// largest is a download.progress snapshot), small enough to bound a
// hostile or corrupt length prefix.
inline constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;
// Frame header: 4-byte little-endian uint32 payload length.
inline constexpr size_t kFrameHeaderBytes = 4;

// Encode one frame: 4-byte LE length + payload. Throws
// std::invalid_argument when the payload is empty or over the max.
std::string encodeFrame(const std::string& payload);
// Write one whole frame (looped send, MSG_NOSIGNAL). False on error.
bool writeFrame(int fd, const std::string& payload);
// Read one whole frame, blocking. False on EOF, error, or an invalid
// (0 or > kMaxFrameBytes) length prefix.
bool readFrame(int fd, std::string& payload_out);
// Read one whole frame, giving up at `deadline`. False on timeout,
// EOF, error, or an invalid length prefix. Used by peers that must
// not block forever (the stub backend waiting on a JoinResponse).
bool readFrameUntil(int fd, std::string& payload_out,
                    std::chrono::steady_clock::time_point deadline);

}  // namespace uds

// Actions in contracts/ipc.json (vc.*). A 404 for one of these means
// the engine does not implement the action — the router may try the
// next engine. A 404 for anything else is a real not-found and stays
// a 404. (Kept verbatim: engine_router.cpp and router_test.cpp rely on
// these exact short names.)
inline bool isKnownVcAction(const std::string& action) {
    static const char* kKnown[] = {
        "play", "pause", "resume", "skip", "stop", "seek", "queue",
        "now-playing", "loop", "shuffle", "autoplay", "autoplay-skip",
        "radio", "join", "mute", "volume",
    };
    for (const char* k : kKnown)
        if (action == k) return true;
    return false;
}

// One IPC call result, mirroring the contract envelope.
struct IpcResult {
    bool ok = false;
    Json data;                 // present when ok
    std::string error_code;    // e.g. "engine_unavailable", "bad_request"
    std::string error_message;
    // HTTP status kept for caller compatibility (engine_router /
    // dispatcher branch on it). Mapped from the UDS error code:
    // 200 ok, 400 bad_request, 401 unauthorized, 404 not_found,
    // 429 rate_limited, 503 engine_unavailable, 500 everything else,
    // 0 when the transport itself failed (socket down / deadline).
    int http_status = 0;
    // True when the service could not be reached at all (transport error
    // or 5xx), when it reports engine_unavailable, or when it 404s a
    // known contract action (the engine does not implement it): the
    // caller should try the next engine. A 404 for an unknown action is
    // a real not-found and is NOT unavailable.
    bool unavailable(const std::string& action = "") const {
        if (http_status == 0 || http_status >= 500 ||
            error_code == "engine_unavailable")
            return true;
        return http_status == 404 && !action.empty() &&
               isKnownVcAction(action);
    }
    std::string describe() const {
        if (ok) return "ok";
        return error_code + ": " + error_message;
    }
};

class IpcClient {
public:
    // Socket path: Config::ipc_socket_path when set ([ipc]
    // socket_path in parlay.toml, or the PARLAY_UDS_PATH env var),
    // else <cfg.data_dir>/parlay-backend.sock.
    explicit IpcClient(const Config& cfg);
    IpcClient(const Config& cfg, std::string socket_path);
    ~IpcClient();
    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = delete;
    IpcClient& operator=(IpcClient&&) = delete;

    // POST an action to the backend — the no-URL form (Phase 4, Q2).
    // Forwards to the virtual base_url form with "" (ignored).
    IpcResult post(const std::string& action, const Json& body,
                   int timeout_sec = 30) const;

    // GET a legacy "/v1/<action>?..." path — the no-URL form.
    IpcResult get(const std::string& path, int timeout_sec = 30) const;

    // "ping" over the socket — the no-URL form. True only on {ok:true}.
    bool healthz(int timeout_sec = 5) const;

    // POST an action to the backend. `base_url` is ignored (single
    // backend); `action` may be a legacy slash-form name or a dotted
    // contract action (canonicalAction() translates). Virtual so
    // tests can script engine answers without a socket.
    virtual IpcResult post(const std::string& base_url,
                           const std::string& action, const Json& body,
                           int timeout_sec = 30) const;

    // GET a legacy "/v1/<action>?..." (or "/api/...") path, translated
    // to the contract action + payload. Same envelope as post().
    virtual IpcResult get(const std::string& base_url,
                          const std::string& path,
                          int timeout_sec = 30) const;

    // "ping" over the socket. True only on {ok:true}.
    virtual bool healthz(const std::string& base_url,
                         int timeout_sec = 5) const;

    const Config& config() const { return cfg_; }
    const std::string& socketPath() const { return socket_path_; }
    // Takes effect on the next connect (an already-open socket is
    // left alone until it drops).
    void setSocketPath(const std::string& path);

    // --- Per-action deadlines (config seam, see header note) ---
    // Override one action's deadline (milliseconds). The table is
    // seeded with the old hardcoded values: 30 000 ms general,
    // 10 000 ms GET-style actions, 5 000 ms ping.
    void setDeadline(const std::string& action, int timeout_ms);
    void setDefaultDeadlineMs(int timeout_ms);
    // Effective deadline for an action: the table entry when set,
    // otherwise timeout_sec * 1000 (the legacy call-site parameter).
    int deadlineMsFor(const std::string& action, int timeout_sec) const;

    // Round-trip microseconds of the most recent call (-1 when no call
    // has completed yet). Phase 1 telemetry hook: the dispatcher
    // emits t_dispatch_to_ipc from this; the benchmark in
    // tests/uds_test.cpp measures with telemetry::Timer.
    int64_t lastCallUs() const { return last_call_us_.load(); }
    bool connected() const;

    // --- Backend-initiated messages (duplex hook) ---
    // JoinRequest handler: receives the request envelope's payload
    // ({chat_id, local_sdp}); return the JoinResponse data
    // ({remote_sdp, group_call_id}). Runs on a worker thread — it may
    // block (Wave 2: the TDLib joinVideoChat call). Throwing, or no
    // handler registered, answers {ok:false, error:{code:"internal"}}.
    using JoinRequestHandler = std::function<Json(const Json& request)>;
    void setJoinRequestHandler(JoinRequestHandler handler);
    // Event handler: (event_name, data) for TrackStarted,
    // TrackAdvanced, TrackEnded, QueueEmpty, DownloadReady, VoiceError.
    // Runs on the reader thread — keep it fast and non-blocking.
    using EventHandler =
        std::function<void(const std::string& event, const Json& data)>;
    void setEventHandler(EventHandler handler);

    // Translate a legacy slash-form / short action name to the dotted
    // contract action ("search" -> "search.query",
    // "download/youtube" -> "download.youtube", "loop" -> "vc.loop").
    // Already-dotted names pass through unchanged; unknown names pass
    // through unchanged (the backend answers not_found).
    static std::string canonicalAction(const std::string& action);
    // Translate a legacy GET path ("/v1/now-playing?chat_id=-1001",
    // "/v1/share/<token>", "/api/queue?chat_id=...") to a contract
    // action + payload. False when the path maps to nothing.
    static bool getPathToAction(const std::string& path,
                                std::string& action_out, Json& payload_out);

private:
    struct PendingCall {
        std::promise<IpcResult> promise;
    };

    IpcResult call(const std::string& action, const Json& payload,
                   int timeout_ms) const;
    // Connect (with backoff bounded by `deadline`) and start the
    // reader thread when needed. Caller holds mu_.
    bool ensureConnectedLocked(
        std::chrono::steady_clock::time_point deadline) const;
    // Tear the connection down and fail every pending call with
    // engine_unavailable. Caller holds mu_ unless noted.
    void dropConnectionLocked(const std::string& reason) const;
    void readerLoop(int fd) const;
    void dispatchFrame(const std::string& frame, int fd) const;
    void failPendingLocked(uint64_t id, const std::string& message) const;

    const Config& cfg_;
    std::string socket_path_;

    mutable std::mutex mu_;
    mutable int fd_ = -1;
    // One reader thread per connection. Reconnects spawn a fresh one;
    // a superseded reader exits on its own when its socket dies, and
    // is joined only in the destructor. (A single reusable thread
    // slot deadlocks here: joining the old reader under mu_ while it
    // waits for mu_ to run its exit cleanup.)
    mutable std::vector<std::thread> readers_;
    mutable std::atomic<bool> stop_{false};
    mutable std::mutex send_mu_;  // serializes writeFrame callers
    mutable std::atomic<uint64_t> next_request_id_{1};
    mutable std::map<uint64_t, std::shared_ptr<PendingCall>> pending_;
    mutable std::mutex deadlines_mu_;
    mutable std::map<std::string, int> deadlines_;
    mutable int default_deadline_ms_ = 30000;

    mutable std::mutex handlers_mu_;
    JoinRequestHandler join_handler_;
    EventHandler event_handler_;

    mutable std::atomic<int64_t> last_call_us_{-1};
};

}  // namespace parlay
