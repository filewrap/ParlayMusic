// ipc.cpp — persistent duplex Unix Domain Socket IPC client.
// Wire spec: contracts/uds-protocol.md. Replaces the old
// subprocess-based HTTP transport (http.* deleted); see ipc.hpp
// for the design.
#include "ipc.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "telemetry.hpp"

namespace parlay {

namespace {

// Backoff steps between reconnect attempts (ms). Capped at 5 s; the
// sequence is bounded by the caller's deadline, not by a fixed retry
// count, so a short-deadline call fails fast and a long one keeps
// trying through a backend restart.
constexpr int kBackoffMs[] = {100, 200, 400, 800, 1600, 3200, 5000};

int statusForError(const std::string& code) {
    if (code == "not_found") return 404;
    if (code == "unauthorized") return 401;
    if (code == "rate_limited") return 429;
    if (code == "engine_unavailable") return 503;
    if (code == "bad_request") return 400;
    return 500;
}

IpcResult resultFromEnvelope(const Json& env) {
    IpcResult out;
    const Json* ok = env.find("ok");
    if (ok && ok->asBool()) {
        out.ok = true;
        out.http_status = 200;
        const Json* data = env.find("data");
        out.data = data ? *data : Json(Json::Object{});
        return out;
    }
    const Json* err = env.find("error");
    if (err) {
        if (const Json* c = err->find("code")) out.error_code = c->asString();
        if (const Json* m = err->find("message"))
            out.error_message = m->asString();
    }
    if (out.error_code.empty()) out.error_code = "internal";
    if (out.error_message.empty()) out.error_message = "backend error";
    out.http_status = statusForError(out.error_code);
    return out;
}

IpcResult transportError(const std::string& message) {
    IpcResult out;
    out.error_code = "engine_unavailable";
    out.error_message = message;
    out.http_status = 0;  // transport failure, same as the old code
    return out;
}

// Read exactly n bytes (looped), honouring a deadline via poll().
// False on timeout, EOF, or error.
bool readExactUntil(int fd, char* buf, size_t n,
                    std::chrono::steady_clock::time_point deadline) {
    size_t got = 0;
    while (got < n) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        int remain_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());
        struct pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, remain_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) return false;  // timeout
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  // EOF
        got += static_cast<size_t>(r);
    }
    return true;
}

bool readExact(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  // EOF
        got += static_cast<size_t>(r);
    }
    return true;
}

uint32_t decodeLe32(const char* b) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[3])) << 24);
}

void encodeLe32(uint32_t v, char* b) {
    b[0] = static_cast<char>(v & 0xff);
    b[1] = static_cast<char>((v >> 8) & 0xff);
    b[2] = static_cast<char>((v >> 16) & 0xff);
    b[3] = static_cast<char>((v >> 24) & 0xff);
}

// Split "path?query" into the path part and key=value query pairs.
void splitQuery(const std::string& path, std::string& base,
                std::map<std::string, std::string>& query) {
    size_t q = path.find('?');
    base = (q == std::string::npos) ? path : path.substr(0, q);
    if (q == std::string::npos) return;
    std::string qs = path.substr(q + 1);
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string kv = qs.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
            query[kv.substr(0, eq)] = kv.substr(eq + 1);
        else if (!kv.empty())
            query[kv] = "";
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

// Query values that are plain integers become JSON ints, the rest
// stay strings (chat_id / user_id / run_id are ints in the contract).
Json queryValue(const std::string& v) {
    if (v.empty()) return Json("");
    size_t i = 0;
    if (v[0] == '-') i = 1;
    bool numeric = i < v.size();
    for (; i < v.size(); ++i)
        if (v[i] < '0' || v[i] > '9') numeric = false;
    if (numeric) {
        try {
            return Json(static_cast<int64_t>(std::stoll(v)));
        } catch (...) {
        }
    }
    return Json(v);
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

namespace uds {

std::string encodeFrame(const std::string& payload) {
    if (payload.empty())
        throw std::invalid_argument("uds: empty frame payload");
    if (payload.size() > kMaxFrameBytes)
        throw std::invalid_argument("uds: frame payload exceeds 16 MiB");
    std::string frame;
    frame.resize(kFrameHeaderBytes + payload.size());
    encodeLe32(static_cast<uint32_t>(payload.size()), frame.data());
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(),
                payload.size());
    return frame;
}

bool writeFrame(int fd, const std::string& payload) {
    std::string frame;
    try {
        frame = encodeFrame(payload);
    } catch (...) {
        return false;
    }
    size_t sent = 0;
    while (sent < frame.size()) {
        ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent,
                           MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool readFrame(int fd, std::string& payload_out) {
    char header[kFrameHeaderBytes];
    if (!readExact(fd, header, kFrameHeaderBytes)) return false;
    uint32_t len = decodeLe32(header);
    if (len == 0 || len > kMaxFrameBytes) return false;
    payload_out.resize(len);
    return readExact(fd, payload_out.data(), len);
}

bool readFrameUntil(int fd, std::string& payload_out,
                    std::chrono::steady_clock::time_point deadline) {
    char header[kFrameHeaderBytes];
    if (!readExactUntil(fd, header, kFrameHeaderBytes, deadline))
        return false;
    uint32_t len = decodeLe32(header);
    if (len == 0 || len > kMaxFrameBytes) return false;
    payload_out.resize(len);
    return readExactUntil(fd, payload_out.data(), len, deadline);
}

}  // namespace uds

// ---------------------------------------------------------------------------
// Action translation (legacy HTTP-era names -> dotted contract actions)
// ---------------------------------------------------------------------------

std::string IpcClient::canonicalAction(const std::string& action) {
    static const std::map<std::string, std::string> kMap = {
        // vc.* short names (engine_router / dispatcher vcCall)
        {"play", "vc.play"},         {"pause", "vc.pause"},
        {"resume", "vc.resume"},     {"skip", "vc.skip"},
        {"stop", "vc.stop"},         {"seek", "vc.seek"},
        {"queue", "vc.queue"},       {"loop", "vc.loop"},
        {"shuffle", "vc.shuffle"},   {"autoplay", "vc.autoplay"},
        {"play_next", "vc.play_next"},
        {"autoplay-skip", "vc.autoplay_skip"},
        {"radio", "vc.radio"},       {"join", "vc.join"},
        {"mute", "vc.mute"},         {"volume", "vc.volume"},
        {"now-playing", "vc.now_playing"},
        {"progress", "vc.progress"}, {"panel", "vc.panel"},
        {"lyrics", "vc.lyrics"},
        // search / inline (vc-ts)
        {"search", "search.query"},  {"inline", "search.inline"},
        // netease-go
        {"netease/search", "netease.search"},
        {"netease/lyrics", "netease.lyrics"},
        {"netease/download", "netease.download"},
        // downloaders
        {"download/youtube", "download.youtube"},
        {"download/spotify", "download.spotify"},
        {"download/spotify-playlist", "download.spotify_playlist"},
        {"download/deezer", "download.deezer"},
        {"download/navidrome", "download.navidrome"},
        {"download/soulseek", "download.soulseek"},
        {"download/tiktok", "download.tiktok"},
        // radio-js
        {"radio/start", "radio.start"},
        {"radio/stop", "radio.stop"},
        {"radio/status", "radio.status"},
        {"radio/stations", "radio.list"},
        // share-rust / audio-fx-rust / queue-rust / spotify-rust
        {"share", "share.create"},   {"fx", "fx.apply"},
        {"vote", "queue.vote"},      {"dislike", "spotify.dislike"},
    };
    auto it = kMap.find(action);
    return it == kMap.end() ? action : it->second;
}

bool IpcClient::getPathToAction(const std::string& path,
                                std::string& action_out, Json& payload_out) {
    std::string base;
    std::map<std::string, std::string> query;
    splitQuery(path, base, query);
    // Strip a leading "/v1/" or "/api/" (both appear in-tree).
    for (const char* prefix : {"/v1/", "/api/"}) {
        size_t plen = std::strlen(prefix);
        if (base.rfind(prefix, 0) == 0) {
            base = base.substr(plen);
            break;
        }
    }
    static const std::map<std::string, std::string> kGetMap = {
        {"now-playing", "vc.now_playing"},
        {"progress", "vc.progress"},
        {"panel", "vc.panel"},
        {"lyrics", "vc.lyrics"},
        {"radio/stations", "radio.list"},
        {"radio/status", "radio.status"},
        {"download/progress", "download.progress"},
        {"chat/settings", "chat.settings"},
        {"spotify/details", "spotify.details"},
        {"spotify/now-playing", "spotify.now_playing"},
        {"spotify/watches", "spotify.watches"},
        {"spotify/status", "spotify.status"},
        {"queue", "gui.queue"},
    };
    std::string action;
    if (base == "healthz") {
        action = "ping";
    } else if (base.rfind("share/", 0) == 0 && base.size() > 6) {
        action = "share.resolve";
        payload_out = Json::objectOf(
            {{"token", Json(base.substr(6))}});
        action_out = action;
        return true;
    } else {
        auto it = kGetMap.find(base);
        if (it == kGetMap.end()) return false;
        action = it->second;
        // "/api/now-playing" is the GUI surface, not vc.now_playing.
        if (path.rfind("/api/", 0) == 0 && base == "now-playing")
            action = "gui.now_playing";
    }
    Json::Object obj;
    for (const auto& kv : query) obj[kv.first] = queryValue(kv.second);
    payload_out = Json(std::move(obj));
    action_out = action;
    return true;
}

// ---------------------------------------------------------------------------
// Construction / deadlines
// ---------------------------------------------------------------------------

namespace {
std::string defaultSocketPath(const Config& cfg) {
    // Precedence (Q6, Phase 4): [ipc] socket_path / PARLAY_UDS_PATH
    // (already folded into cfg.ipc_socket_path by the config load
    // paths) wins; the env var is re-checked here so clients built
    // from a bare Config::defaults() still honour it; otherwise the
    // socket lives next to the state file.
    if (!cfg.ipc_socket_path.empty()) return cfg.ipc_socket_path;
    const char* env = std::getenv("PARLAY_UDS_PATH");
    if (env && *env) return std::string(env);
    std::string dir = cfg.data_dir.empty() ? std::string("data") : cfg.data_dir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir + "/parlay-backend.sock";
}
}  // namespace

IpcClient::IpcClient(const Config& cfg)
    : IpcClient(cfg, defaultSocketPath(cfg)) {}

IpcClient::IpcClient(const Config& cfg, std::string socket_path)
    : cfg_(cfg), socket_path_(std::move(socket_path)) {
    // Seed the deadline table with the old hardcoded values so the
    // transport swap changes no timing behaviour. GET-style actions
    // ran on the 10 s budget, ping/healthz on 5 s, everything else
    // on 30 s. The config rewrite (Worker C) replaces these from
    // parlay.toml [timeouts] via setDeadline().
    static const char* kGetStyle[] = {
        "vc.now_playing", "vc.progress", "vc.panel", "vc.lyrics",
        "radio.list", "radio.status", "download.progress",
        "share.resolve", "chat.settings", "spotify.details",
        "spotify.now_playing", "spotify.watches", "spotify.status",
        "gui.now_playing", "gui.queue",
    };
    for (const char* a : kGetStyle) deadlines_[a] = 10000;
    deadlines_["ping"] = 5000;
}

IpcClient::~IpcClient() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }
    for (auto& t : readers_)
        if (t.joinable()) t.join();
}

void IpcClient::setSocketPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    socket_path_ = path;
}

void IpcClient::setDeadline(const std::string& action, int timeout_ms) {
    std::lock_guard<std::mutex> lk(deadlines_mu_);
    deadlines_[canonicalAction(action)] = timeout_ms;
}

void IpcClient::setDefaultDeadlineMs(int timeout_ms) {
    std::lock_guard<std::mutex> lk(deadlines_mu_);
    default_deadline_ms_ = timeout_ms;
}

int IpcClient::deadlineMsFor(const std::string& action,
                             int timeout_sec) const {
    std::lock_guard<std::mutex> lk(deadlines_mu_);
    auto it = deadlines_.find(canonicalAction(action));
    if (it != deadlines_.end()) return it->second;
    return timeout_sec * 1000;
}

bool IpcClient::connected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fd_ >= 0;
}

void IpcClient::setJoinRequestHandler(JoinRequestHandler handler) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    join_handler_ = std::move(handler);
}

void IpcClient::setEventHandler(EventHandler handler) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    event_handler_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

bool IpcClient::ensureConnectedLocked(
    std::chrono::steady_clock::time_point deadline) const {
    if (fd_ >= 0) return true;
    if (stop_.load()) return false;

    size_t attempt = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return false;
        }
        std::memcpy(addr.sun_path, socket_path_.data(), socket_path_.size());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            fd_ = fd;
            readers_.emplace_back([this, fd] { readerLoop(fd); });
            return true;
        }
        ::close(fd);
        // Exponential backoff, bounded by the caller's deadline.
        int step = kBackoffMs[attempt < sizeof(kBackoffMs) / sizeof(int)
                                  ? attempt
                                  : sizeof(kBackoffMs) / sizeof(int) - 1];
        ++attempt;
        auto now = std::chrono::steady_clock::now();
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - now)
                          .count();
        if (remain <= 0) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min<int64_t>(step, remain)));
    }
    return false;
}

void IpcClient::dropConnectionLocked(const std::string& reason) const {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    for (auto& kv : pending_)
        failPendingLocked(kv.first, reason);
    pending_.clear();
}

void IpcClient::failPendingLocked(uint64_t id,
                                  const std::string& message) const {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    try {
        it->second->promise.set_value(transportError(message));
    } catch (...) {
    }
    pending_.erase(it);
}

// Reader thread: one frame at a time, dispatch by "type".
void IpcClient::readerLoop(int fd) const {
    while (!stop_.load()) {
        std::string frame;
        if (!uds::readFrame(fd, frame)) break;
        dispatchFrame(frame, fd);
    }
    // Socket died: fail everything still waiting, mark disconnected
    // so the next call reconnects with backoff.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ == fd) {
            ::close(fd_);
            fd_ = -1;
        }
        for (auto& kv : pending_)
            failPendingLocked(kv.first, "backend connection lost");
        pending_.clear();
    }
}

void IpcClient::dispatchFrame(const std::string& frame, int fd) const {
    Json msg;
    try {
        msg = Json::parse(frame);
    } catch (...) {
        return;  // corrupt frame: ignore, keep the socket alive
    }
    const Json* type = msg.find("type");
    std::string t = type ? type->asString() : "";
    if (t == "response") {
        const Json* rid = msg.find("request_id");
        if (!rid || !rid->isInt()) return;
        uint64_t id = static_cast<uint64_t>(rid->asInt());
        std::shared_ptr<PendingCall> call;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_.find(id);
            if (it == pending_.end()) return;  // late / unknown: ignore
            call = it->second;
            pending_.erase(it);
        }
        try {
            call->promise.set_value(resultFromEnvelope(msg));
        } catch (...) {
        }
        return;
    }
    if (t == "event") {
        EventHandler handler;
        {
            std::lock_guard<std::mutex> lk(handlers_mu_);
            handler = event_handler_;
        }
        if (!handler) return;
        const Json* ev = msg.find("event");
        const Json* data = msg.find("data");
        handler(ev ? ev->asString() : "",
                data ? *data : Json(Json::Object{}));
        return;
    }
    if (t == "join_request") {
        // Handled on a worker thread: the handler may block on TDLib
        // (Wave 2) and must never stall the reader — responses for
        // other in-flight requests keep flowing.
        const Json* rid = msg.find("request_id");
        uint64_t id = (rid && rid->isInt())
                          ? static_cast<uint64_t>(rid->asInt())
                          : 0;
        Json request = msg;
        std::thread([this, fd, id, request] {
            Json response = Json::objectOf(
                {{"type", Json("response")},
                 {"request_id", Json(static_cast<int64_t>(id))}});
            JoinRequestHandler handler;
            {
                std::lock_guard<std::mutex> lk(handlers_mu_);
                handler = join_handler_;
            }
            try {
                if (!handler) throw std::runtime_error(
                    "no JoinRequest handler registered");
                Json data = handler(request);
                response.set("ok", Json(true));
                response.set("data", std::move(data));
            } catch (const std::exception& e) {
                response.set("ok", Json(false));
                response.set("error",
                             Json::objectOf(
                                 {{"code", Json("internal")},
                                  {"message", Json(e.what())}}));
            }
            std::lock_guard<std::mutex> lk(send_mu_);
            // The socket may have dropped and reconnected while the
            // handler ran: never write a stale response into a new
            // connection (the fd number may have been reused).
            {
                std::lock_guard<std::mutex> lk2(mu_);
                if (fd_ != fd) return;
            }
            uds::writeFrame(fd, response.dump());
        }).detach();
        return;
    }
    // Unknown frame type: ignore.
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

IpcResult IpcClient::call(const std::string& action, const Json& payload,
                          int timeout_ms) const {
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeout_ms);
    uint64_t id = next_request_id_.fetch_add(1);

    Json envelope = Json::objectOf(
        {{"type", Json("request")},
         {"request_id", Json(static_cast<int64_t>(id))},
         {"action", Json(action)},
         {"deadline_ms", Json(static_cast<int64_t>(timeout_ms))},
         {"payload", payload}});
    // Promote the routing ids to envelope top level when the body
    // carries them, so the backend can route without parsing payload.
    if (const Json* c = payload.find("chat_id"))
        if (c->isInt()) envelope.set("chat_id", *c);
    const Json* uid = payload.find("user_id");
    if (!uid) uid = payload.find("requested_by");
    if (!uid) uid = payload.find("voter_id");
    if (!uid) uid = payload.find("created_by");
    if (uid && uid->isInt()) envelope.set("user_id", *uid);

    auto pending = std::make_shared<PendingCall>();
    std::future<IpcResult> fut = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ensureConnectedLocked(deadline)) {
            return transportError("backend unreachable at " + socket_path_ +
                                  " (connect failed)");
        }
        pending_[id] = pending;
    }
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        int fd;
        {
            std::lock_guard<std::mutex> lk2(mu_);
            fd = fd_;  // capture under mu_: the reader may have
                       // replaced it since ensureConnectedLocked
        }
        if (fd < 0 || !uds::writeFrame(fd, envelope.dump())) {
            std::lock_guard<std::mutex> lk2(mu_);
            // Only drop the connection we actually wrote to; a
            // concurrent reconnect must not be torn down.
            if (fd_ == fd) dropConnectionLocked("backend write failed");
            return transportError("backend write failed");
        }
    }
    if (fut.wait_until(deadline) != std::future_status::ready) {
        // Deadline hit: the call is abandoned and the connection is
        // dropped — a peer that stops answering is not trusted again.
        // A late response (if one ever arrives) finds no pending id
        // and is ignored.
        std::lock_guard<std::mutex> lk(mu_);
        pending_.erase(id);
        dropConnectionLocked("deadline exceeded for action " + action);
        return transportError("deadline exceeded for action " + action +
                              " after " + std::to_string(timeout_ms) +
                              " ms");
    }
    IpcResult result = fut.get();
    last_call_us_.store(telemetry::elapsedUs(start));
    return result;
}

IpcResult IpcClient::post(const std::string& action, const Json& body,
                          int timeout_sec) const {
    return post("", action, body, timeout_sec);
}

IpcResult IpcClient::post(const std::string& base_url,
                          const std::string& action, const Json& body,
                          int timeout_sec) const {
    (void)base_url;  // single backend: the URL is signature compat only
    std::string canonical = canonicalAction(action);
    return call(canonical, body, deadlineMsFor(canonical, timeout_sec));
}

IpcResult IpcClient::get(const std::string& path, int timeout_sec) const {
    return get("", path, timeout_sec);
}

IpcResult IpcClient::get(const std::string& base_url,
                         const std::string& path, int timeout_sec) const {
    (void)base_url;  // single backend: the URL is signature compat only
    std::string action;
    Json payload;
    if (!getPathToAction(path, action, payload)) {
        IpcResult out;
        out.error_code = "not_found";
        out.error_message = "no UDS action for path " + path;
        out.http_status = 404;
        return out;
    }
    return call(action, payload, deadlineMsFor(action, timeout_sec));
}

bool IpcClient::healthz(int timeout_sec) const {
    return healthz("", timeout_sec);
}

bool IpcClient::healthz(const std::string& base_url, int timeout_sec) const {
    (void)base_url;  // single backend: the URL is signature compat only
    IpcResult r = call("ping", Json(Json::Object{}),
                       deadlineMsFor("ping", timeout_sec));
    return r.ok;
}

}  // namespace parlay
