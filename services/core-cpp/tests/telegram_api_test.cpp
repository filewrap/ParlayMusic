// telegram_api_test.cpp — TelegramApi::sendDocument (Phase 6, B11) +
// HttpClient::postRaw / postMultipart.
//
// A local HttpServer stub (127.0.0.1, ephemeral port) plays Telegram:
// it captures the request (path, headers, body) and answers a canned
// Bot API envelope. No api.telegram.org traffic, no credentials.
//   * sendDocument builds a real multipart/form-data body: chat_id +
//     caption text fields, a "document" part with the filename and
//     the exact file bytes, Content-Type multipart/form-data with a
//     boundary. The returned message_id is parsed from the envelope.
//   * sendDocument sanitizes hostile filenames (no "/" or quotes)
//     and reports a Telegram ok:false envelope honestly (-1 + error).
//   * postRaw sends binary bytes verbatim with the caller's
//     Content-Type (the /speedtest upload probe); postJson still
//     sends application/json (regression check for the run() refactor).
//
// Build: g++ -std=c++20 -I src tests/telegram_api_test.cpp
//   src/telegram_api.cpp src/http.cpp src/http_server.cpp src/json.cpp
//   src/config.cpp -o /tmp/telegram_api_test && /tmp/telegram_api_test
// Exit 0 = all checks pass.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "config.hpp"
#include "http.hpp"
#include "http_server.hpp"
#include "json.hpp"
#include "telegram_api.hpp"

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

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Captured request from the stub, plus the scripted response.
struct Stub {
    std::string path;
    std::string content_type;
    std::string body;
    int status = 200;
    std::string response = R"({"ok":true,"result":{"message_id":77}})";
};

parlay::HttpResponse stubHandler(Stub& s, const parlay::HttpRequest& req) {
    s.path = req.path;
    auto ct = req.headers.find("content-type");
    s.content_type = ct == req.headers.end() ? "" : ct->second;
    s.body = req.body;
    return parlay::HttpResponse{s.status, s.response};
}

parlay::Config stubConfig(int port) {
    parlay::Config c;
    c.telegram_api_base = "http://127.0.0.1:" + std::to_string(port);
    c.bot_token = "TESTTOKEN";
    return c;
}

}  // namespace

int main() {
    // --- A. sendDocument: multipart body, parsed message_id --------------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) {
            std::cout << "FAIL stub server would not start\n";
            return 1;
        }
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        std::string content = "-1002\n-1005\n";
        int64_t id = tg.sendDocument(-1001, "errors.txt", content,
                                     "Broadcast failures (2 chats):", &err);
        CHECK(id == 77, "sendDocument: message_id parsed from envelope");
        CHECK(err.empty(), "sendDocument: no error on success");
        CHECK(s.path == "/botTESTTOKEN/sendDocument",
              "sendDocument: hits /bot<token>/sendDocument");
        CHECK(s.content_type.rfind("multipart/form-data; boundary=", 0) == 0,
              "sendDocument: Content-Type is multipart/form-data + boundary");
        std::string boundary =
            s.content_type.substr(strlen("multipart/form-data; boundary="));
        CHECK(!boundary.empty() && s.body.find("--" + boundary) == 0,
              "sendDocument: body starts with the boundary delimiter");
        CHECK(contains(s.body, "name=\"chat_id\"\r\n\r\n-1001"),
              "sendDocument: chat_id text field");
        CHECK(contains(s.body,
                       "name=\"caption\"\r\n\r\nBroadcast failures (2 chats):"),
              "sendDocument: caption text field");
        CHECK(contains(s.body, "name=\"document\"; filename=\"errors.txt\""),
              "sendDocument: document part with filename errors.txt");
        CHECK(contains(s.body, "Content-Type: text/plain"),
              "sendDocument: document part is text/plain");
        CHECK(contains(s.body, "-1002\n-1005\n"),
              "sendDocument: file bytes verbatim in the body");
        CHECK(contains(s.body, "--" + boundary + "--"),
              "sendDocument: body ends with the closing boundary");
        server.stop();
    }

    // --- B. sendDocument: hostile filename sanitized ----------------------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        int64_t id = tg.sendDocument(-1001, "../../etc/passwd", "x", "", &err);
        CHECK(id == 77, "sendDocument hostile name: still uploads");
        CHECK(contains(s.body, "filename=\"....etcpasswd\"") &&
                  !contains(s.body, "filename=\"../../"),
              "sendDocument hostile name: slashes stripped, no path");
        id = tg.sendDocument(-1001, "///", "x", "", &err);
        CHECK(contains(s.body, "filename=\"file.txt\""),
              "sendDocument empty-after-sanitize name: falls back to file.txt");
        server.stop();
    }

    // --- C. sendDocument: Telegram ok:false / HTTP 500 --------------------
    {
        Stub s;
        s.response = R"({"ok":false,"description":"Forbidden: bot was blocked by the user"})";
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        int64_t id = tg.sendDocument(-1001, "errors.txt", "x", "", &err);
        CHECK(id == -1 && contains(err, "blocked"),
              "sendDocument ok:false: -1 + Telegram description");
        s.status = 500;
        s.response = "boom";
        id = tg.sendDocument(-1001, "errors.txt", "x", "", &err);
        CHECK(id == -1 && contains(err, "HTTP 500"),
              "sendDocument HTTP 500: -1 + status in error");
        server.stop();
    }

    // --- D. postRaw: binary body verbatim (upload probe mechanism) -------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        std::string base = "http://127.0.0.1:" + std::to_string(server.port());
        std::string body(512 * 1024, '\0');
        body[0] = 'A';
        body[100] = 'B';
        body[body.size() - 1] = 'Z';
        parlay::HttpResult r = parlay::HttpClient::postRaw(
            base + "/__up", body, "application/octet-stream", {}, 30);
        CHECK(r.ok() && r.status == 200, "postRaw: stub answers 200");
        CHECK(s.content_type == "application/octet-stream",
              "postRaw: caller Content-Type sent");
        CHECK(s.body.size() == body.size() && s.body == body,
              "postRaw: 512 KiB binary body (incl. NULs) byte-exact");
        server.stop();
    }

    // --- E. postMultipart direct + postJson regression --------------------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        std::string base = "http://127.0.0.1:" + std::to_string(server.port());
        parlay::HttpResult r = parlay::HttpClient::postMultipart(
            base + "/upload", {{"k", "v with spaces & \"quotes\""}},
            "document", "a.bin", std::string("x\0y", 3),
            "application/octet-stream", {}, 30);
        CHECK(r.ok(), "postMultipart direct: 200");
        CHECK(contains(s.body, "name=\"k\"\r\n\r\nv with spaces & \"quotes\""),
              "postMultipart direct: text field with quotes intact");
        CHECK(contains(s.body, "name=\"document\"; filename=\"a.bin\""),
              "postMultipart direct: file part + filename");
        CHECK(s.body.find(std::string("x\0y", 3)) != std::string::npos,
              "postMultipart direct: NUL bytes survive in the file part");
        parlay::HttpResult j = parlay::HttpClient::postJson(
            base + "/post", R"({"a":1})", {}, 30);
        CHECK(j.ok() && s.content_type == "application/json" &&
                  s.body == R"({"a":1})",
              "postJson: still application/json with the exact body");
        server.stop();
    }

    std::cout << (failures == 0 ? "ALL PASS\n"
                                : "FAILURES: " + std::to_string(failures) +
                                      "\n");
    return failures == 0 ? 0 : 1;
}
