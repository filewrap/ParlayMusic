// http_server.cpp — std-only HTTP/1.1 server (see http_server.hpp).
#include "http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <iostream>

namespace parlay {
namespace {

// 64 KiB cap on the request head: the API is GET-only and its query
// strings are short; anything bigger is a 400, not a buffer.
constexpr size_t kMaxHeadBytes = 64 * 1024;
constexpr int kReadTimeoutSec = 5;

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 502: return "Bad Gateway";
        default: return "OK";
    }
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: a client that went away must not kill the core.
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
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

}  // namespace

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 &&
                   hexVal(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexVal(s[i + 1]) * 16 +
                                            hexVal(s[i + 2])));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool HttpServer::start() {
    if (running_.load()) return true;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_.load()));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "parlay-core: http bind :" << port_.load() << " failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return false;
    }
    // Report the real port back (matters when 0/ephemeral was requested).
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
        port_.store(ntohs(bound.sin_port));
    listen_fd_.store(fd);
    running_.store(true);
    accept_thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    // Snapshot first: the accept thread may be between select() and
    // FD_ISSET() (see acceptLoop); closing here must not change the
    // fd it is about to use — it uses its own snapshot.
    int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        // Shutdown unblocks the select() in the accept loop promptly.
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        // Snapshot the listen fd once per iteration and use only this
        // local below: stop() closes the socket and stores -1
        // concurrently, and FD_SET/FD_ISSET on -1 aborts the process
        // (glibc FD_SETSIZE fortify check). A closed fd surfaces as
        // select()/accept() EBADF instead, which breaks the loop.
        int fd = listen_fd_.load();
        if (fd < 0) break;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 100000};  // 100 ms: stop() stays responsive.
        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;  // timeout: re-check running_
        if (!FD_ISSET(fd, &rfds)) continue;
        int conn = ::accept(fd, nullptr, nullptr);
        if (conn < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!running_.load()) break;
            continue;
        }
        timeval timeout{kReadTimeoutSec, 0};
        ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
        std::thread([this, conn] { handleConnection(conn); }).detach();
    }
}

void HttpServer::handleConnection(int fd) {
    auto respond = [&](const HttpResponse& r) {
        std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " +
                           reasonPhrase(r.status) +
                           "\r\nContent-Type: " + r.content_type +
                           "\r\nContent-Length: " +
                           std::to_string(r.body.size()) +
                           "\r\nConnection: close\r\n\r\n";
        sendAll(fd, head + r.body);
    };

    std::string head;
    char buf[4096];
    bool got_head = false;
    while (head.size() < kMaxHeadBytes) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return;
        }
        if (n == 0) break;  // client closed before sending a full head
        head.append(buf, static_cast<size_t>(n));
        if (head.find("\r\n\r\n") != std::string::npos) {
            got_head = true;
            break;
        }
    }
    if (!got_head) {
        respond(HttpResponse{400, R"({"ok":false,"error":"bad request"})"});
        ::close(fd);
        return;
    }

    // The API is GET-only, but a client may still send a body (e.g. a
    // stray POST). Read it into req.body before responding: closing a
    // socket with unread bytes in the receive buffer makes the kernel
    // send RST, which can destroy the response the client is waiting
    // for. Bodies over 1 MiB are refused rather than read.
    size_t head_end = head.find("\r\n\r\n") + 4;
    size_t content_length = 0;
    {
        std::string lower;
        lower.reserve(head_end);
        for (size_t i = 0; i < head_end; ++i)
            lower.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(head[i]))));
        const std::string key = "content-length:";
        size_t cl = lower.find(key);
        if (cl != std::string::npos && cl < head_end) {
            size_t v = cl + key.size();
            while (v < head_end && (lower[v] == ' ' || lower[v] == '\t'))
                ++v;
            size_t num_start = v;
            while (v < head_end && lower[v] >= '0' && lower[v] <= '9') ++v;
            if (v > num_start) {
                try {
                    content_length = static_cast<size_t>(
                        std::stoul(head.substr(num_start, v - num_start)));
                } catch (...) {
                    content_length = 0;
                }
            }
        }
    }
    if (content_length > 1024 * 1024) {
        respond(HttpResponse{400, R"({"ok":false,"error":"bad request"})"});
        ::close(fd);
        return;
    }
    size_t already = head.size() > head_end ? head.size() - head_end : 0;
    size_t remaining = content_length > already ? content_length - already : 0;
    std::string body;
    if (content_length > 0) {
        body.reserve(std::min(content_length, size_t(1024 * 1024)));
        if (already > 0) body.append(head.substr(head_end, already));
    }
    while (remaining > 0) {
        ssize_t n = ::recv(fd, buf, std::min(sizeof(buf), remaining), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return;
        }
        if (n == 0) break;  // client went away mid-body
        body.append(buf, static_cast<size_t>(n));
        remaining -= static_cast<size_t>(n);
    }

    // Request line: METHOD SP TARGET SP HTTP/x.y
    size_t line_end = head.find("\r\n");
    std::string line = head.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string::npos
                     ? std::string::npos
                     : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        respond(HttpResponse{400, R"({"ok":false,"error":"bad request"})"});
        ::close(fd);
        return;
    }
    HttpRequest req;
    req.method = line.substr(0, sp1);
    req.body = std::move(body);
    // Headers: one "Name: value" per line between the request line
    // and the blank line; keys lowercased, values trimmed.
    {
        size_t pos = line_end + 2;
        while (pos + 2 <= head_end) {
            size_t eol = head.find("\r\n", pos);
            if (eol == std::string::npos || eol >= head_end || eol == pos)
                break;
            std::string hline = head.substr(pos, eol - pos);
            size_t colon = hline.find(':');
            if (colon != std::string::npos) {
                std::string key = hline.substr(0, colon);
                for (char& c : key)
                    c = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c)));
                size_t v = colon + 1;
                while (v < hline.size() &&
                       (hline[v] == ' ' || hline[v] == '\t'))
                    ++v;
                size_t vend = hline.size();
                while (vend > v &&
                       (hline[vend - 1] == ' ' || hline[vend - 1] == '\t'))
                    --vend;
                if (!key.empty())
                    req.headers[key] = hline.substr(v, vend - v);
            }
            pos = eol + 2;
        }
    }
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string raw_path = target;
    std::string raw_query;
    size_t q = target.find('?');
    if (q != std::string::npos) {
        raw_path = target.substr(0, q);
        raw_query = target.substr(q + 1);
    }
    req.path = urlDecode(raw_path);
    size_t pos = 0;
    while (pos <= raw_query.size()) {
        size_t amp = raw_query.find('&', pos);
        std::string pair = raw_query.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        std::string key = urlDecode(eq == std::string::npos
                                        ? pair
                                        : pair.substr(0, eq));
        std::string value = eq == std::string::npos
                                ? ""
                                : urlDecode(pair.substr(eq + 1));
        if (!key.empty()) req.query[key] = value;
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }

    HttpResponse r = handler_(req);
    respond(r);
    ::close(fd);
}

}  // namespace parlay
