// http_server.hpp — minimal std-only HTTP/1.1 server (Phase 6, A2).
//
// No new dependencies: POSIX sockets + std::thread. Serves the core's
// API (GET /healthz, GET /api/now-playing, GET /api/queue, POST
// /v1/join — vc.join for the vc-ts / vc-rust engines) on its own
// thread so the Telegram long-poll loop is never blocked.
// The request handler is injected (see core_api.hpp) so the server
// itself knows nothing about engines or playback state.
//
// Request bodies are read (up to 1 MiB) because POST /v1/join needs
// one; the connection is closed after each response.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace parlay {

struct HttpRequest {
    std::string method;  // e.g. "GET"
    std::string path;    // path without the query string, e.g. "/api/queue"
    std::map<std::string, std::string> query;  // URL-decoded query params
    // Request headers, keys lowercased (e.g. "content-type"). Test
    // stubs need this to assert a multipart boundary or an upload
    // content-type; the core API itself ignores headers.
    std::map<std::string, std::string> headers;
    // Raw request body (up to 1 MiB; empty for GET). POST /v1/join
    // reads it ({"chat_id": ...}); test stubs may need it too (e.g. a
    // stub engine answering POST /v1/queue per chat).
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    // port 0 = ephemeral (the OS picks a free port; see port()).
    HttpServer(int port, HttpHandler handler)
        : port_(port), handler_(std::move(handler)) {}
    ~HttpServer() { stop(); }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Bind + listen + spawn the accept thread. False when the port
    // cannot be bound (the caller logs it); the server stays stopped.
    bool start();
    void stop();
    bool running() const { return running_.load(); }
    // Actual bound port (== requested port, unless 0 was requested).
    int port() const { return port_.load(); }

private:
    void acceptLoop();
    void handleConnection(int fd);

    std::atomic<int> port_;
    HttpHandler handler_;
    // Atomic: the accept thread snapshots this once per iteration and
    // uses only the snapshot for FD_SET/FD_ISSET/select/accept. stop()
    // closes the socket and stores -1 from another thread; using the
    // member directly after select() returns (FD_ISSET(listen_fd_))
    // raced with that store and glibc aborted the process with
    // "*** bit out of range 0 - FD_SETSIZE on fd_set ***" on every
    // SIGTERM (exit 134, 8/8 runs) — the shutdown() wakes select(),
    // the main thread closes the fd and stores -1, and the accept
    // thread then evaluated FD_ISSET(-1). A snapshot can only ever
    // hold a valid fd (or -1, which breaks the loop before any
    // FD_* call); a closed fd surfaces as select/accept EBADF.
    std::atomic<int> listen_fd_{-1};
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
};

// URL-decode one query component (%XX, + -> space). Malformed %
// sequences are passed through verbatim rather than failing the
// whole request.
std::string urlDecode(const std::string& s);

}  // namespace parlay
