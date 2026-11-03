// telegram_api_test.cpp — TelegramApi::sendDocument (Phase 6, B11) +
// HttpClient::postRaw / postMultipart; Phase 9 interactive layer
// (sendMessageHtml, sendPhoto, editMessageText/Caption/ReplyMarkup,
// answerCallbackQuery, deleteMessage, sendAudioFile, sendAudio,
// sendFile) — request shapes against the same stub server.
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
#include <filesystem>
#include <fstream>
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

    // --- F. Phase 9 interactive layer: JSON shapes ----------------------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        // sendMessageHtml: JSON, HTML parse mode, reply_markup, reply_to.
        parlay::Json kb = parlay::Json::parse(
            R"({"inline_keyboard":[[{"text":"Pause","callback_data":"pl:player:pause:-1001"}]]})");
        int64_t id = tg.sendMessageHtml(-1001, "<b>Hi</b>", kb, 42, &err);
        CHECK(id == 77, "sendMessageHtml: message_id parsed");
        CHECK(s.path == "/botTESTTOKEN/sendMessage",
              "sendMessageHtml: hits sendMessage");
        CHECK(s.content_type == "application/json",
              "sendMessageHtml: JSON content type");
        CHECK(contains(s.body, "\"parse_mode\":\"HTML\""),
              "sendMessageHtml: parse_mode HTML");
        CHECK(contains(s.body, "\"text\":\"<b>Hi</b>\""),
              "sendMessageHtml: text verbatim");
        CHECK(contains(s.body, "\"reply_to_message_id\":42"),
              "sendMessageHtml: reply_to sent");
        CHECK(contains(s.body, "\"callback_data\":\"pl:player:pause:-1001\""),
              "sendMessageHtml: inline keyboard JSON in body");
        // No markup + no reply_to: those fields are omitted.
        id = tg.sendMessageHtml(-1001, "plain", parlay::Json(nullptr), 0, &err);
        CHECK(!contains(s.body, "reply_markup") &&
                  !contains(s.body, "reply_to_message_id"),
              "sendMessageHtml: null markup / no reply_to omitted");
        // sendPhoto: JSON with photo URL + HTML caption + keyboard.
        id = tg.sendPhoto(-1001, "https://x/thumb.jpg", "<b>Now</b>", kb, &err);
        CHECK(id == 77 && s.path == "/botTESTTOKEN/sendPhoto",
              "sendPhoto: hits sendPhoto, id parsed");
        CHECK(contains(s.body, "\"photo\":\"https://x/thumb.jpg\"") &&
                  contains(s.body, "\"caption\":\"<b>Now</b>\"") &&
                  contains(s.body, "\"parse_mode\":\"HTML\""),
              "sendPhoto: photo + caption + HTML in body");
        // editMessageText / editMessageCaption.
        CHECK(tg.editMessageText(-1001, 7, "<b>New</b>", kb, &err) &&
                  s.path == "/botTESTTOKEN/editMessageText",
              "editMessageText: hits editMessageText");
        CHECK(contains(s.body, "\"message_id\":7") &&
                  contains(s.body, "\"text\":\"<b>New</b>\""),
              "editMessageText: id + text in body");
        CHECK(tg.editMessageCaption(-1001, 8, "<b>Cap</b>", kb, &err) &&
                  s.path == "/botTESTTOKEN/editMessageCaption",
              "editMessageCaption: hits editMessageCaption");
        CHECK(contains(s.body, "\"caption\":\"<b>Cap</b>\""),
              "editMessageCaption: caption in body");
        // editMessageReplyMarkup: null clears to an empty keyboard.
        CHECK(tg.editMessageReplyMarkup(-1001, 7, parlay::Json(nullptr),
                                        &err) &&
                  s.path == "/botTESTTOKEN/editMessageReplyMarkup",
              "editMessageReplyMarkup: hits editMessageReplyMarkup");
        CHECK(contains(s.body, "\"inline_keyboard\":[]"),
              "editMessageReplyMarkup: null markup -> empty inline keyboard");
        CHECK(tg.editMessageReplyMarkup(-1001, 7, kb, &err) &&
                  contains(s.body, "\"callback_data\":\"pl:player:pause:-1001\""),
              "editMessageReplyMarkup: real markup sent");
        // answerCallbackQuery: alert + text; empty text omits the field.
        CHECK(tg.answerCallbackQuery("cq9", "Denied", true, 0, &err) &&
                  s.path == "/botTESTTOKEN/answerCallbackQuery",
              "answerCallbackQuery: hits answerCallbackQuery");
        CHECK(contains(s.body, "\"callback_query_id\":\"cq9\"") &&
                  contains(s.body, "\"text\":\"Denied\"") &&
                  contains(s.body, "\"show_alert\":true"),
              "answerCallbackQuery: id + text + alert in body");
        CHECK(tg.answerCallbackQuery("cq9", "", false, 0, &err) &&
                  !contains(s.body, "\"text\"") &&
                  !contains(s.body, "show_alert"),
              "answerCallbackQuery: silent ack omits text/alert");
        // deleteMessage.
        CHECK(tg.deleteMessage(-1001, 7) &&
                  s.path == "/botTESTTOKEN/deleteMessage",
              "deleteMessage: hits deleteMessage");
        CHECK(contains(s.body, "\"chat_id\":-1001") &&
                  contains(s.body, "\"message_id\":7"),
              "deleteMessage: chat + message id in body");
        server.stop();
    }

    // --- G. sendAudioFile / sendAudio / sendFile (Phase 9) ---------------
    {
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        // sendAudioFile: multipart, audio part, content type by extension.
        {
            std::string dir = "/tmp/parlay-tg-test";
            std::filesystem::create_directories(dir);
            std::string path = dir + "/song.mp3";
            std::ofstream out(path, std::ios::binary);
            out << "FAKEAUDIO";
            out.close();
            int64_t id = tg.sendAudioFile(-1001, path, "cap", &err);
            CHECK(id == 77, "sendAudioFile: message_id parsed");
            CHECK(s.path == "/botTESTTOKEN/sendAudio",
                  "sendAudioFile: hits sendAudio");
            CHECK(s.content_type.rfind("multipart/form-data; boundary=", 0) == 0,
                  "sendAudioFile: multipart content type");
            CHECK(contains(s.body, "name=\"audio\"; filename=\"song.mp3\""),
                  "sendAudioFile: audio part with filename");
            CHECK(contains(s.body, "Content-Type: audio/mpeg"),
                  "sendAudioFile: mp3 -> audio/mpeg");
            CHECK(contains(s.body, "FAKEAUDIO"),
                  "sendAudioFile: file bytes in body");
            // Missing file: -1 + error, no request.
            id = tg.sendAudioFile(-1001, dir + "/nope.mp3", "", &err);
            CHECK(id == -1 && !err.empty(),
                  "sendAudioFile: missing file fails honestly");
        }
        // sendAudio by file_id: JSON, audio + caption + keyboard.
        {
            parlay::Json kb = parlay::Json::parse(
                R"({"inline_keyboard":[[{"text":"Feedback","callback_data":"pl:share:feedback:1:2:abc"}]]})");
            int64_t id = tg.sendAudio(-1001, "AUDIOFILEID", "", kb, &err);
            CHECK(id == 77 && s.path == "/botTESTTOKEN/sendAudio",
                  "sendAudio: hits sendAudio, id parsed");
            CHECK(contains(s.body, "\"audio\":\"AUDIOFILEID\""),
                  "sendAudio: file_id in body");
            CHECK(contains(s.body, "\"callback_data\":\"pl:share:feedback:1:2:abc\""),
                  "sendAudio: inline keyboard in body");
        }
        // sendFile: multipart document, content type by extension.
        {
            std::string dir = "/tmp/parlay-tg-test";
            std::string path = dir + "/pack.zip";
            std::ofstream out(path, std::ios::binary);
            out << "FAKEZIP";
            out.close();
            int64_t id = tg.sendFile(-1001, path, "zip cap", &err);
            CHECK(id == 77 && s.path == "/botTESTTOKEN/sendDocument",
                  "sendFile: hits sendDocument, id parsed");
            CHECK(contains(s.body, "name=\"document\"; filename=\"pack.zip\""),
                  "sendFile: document part with filename");
            CHECK(contains(s.body, "Content-Type: application/zip"),
                  "sendFile: zip -> application/zip");
            CHECK(contains(s.body, "FAKEZIP"),
                  "sendFile: file bytes in body");
            std::string lrc = dir + "/song.lrc";
            std::ofstream out2(lrc, std::ios::binary);
            out2 << "[00:01.00] la\n";
            out2.close();
            id = tg.sendFile(-1001, lrc, "", &err);
            CHECK(contains(s.body, "filename=\"song.lrc\"") &&
                      contains(s.body, "Content-Type: text/plain"),
                  "sendFile: lrc -> text/plain");
        }
        server.stop();
    }

    // --- H. inline queries (Phase 9 gaps, §13) -------------------------
    {
        // parseInlineQuery: id/from/query/offset/chat_type.
        parlay::Json j = parlay::Json::parse(
            R"({"id":"iq9","from":{"id":111,"first_name":"Ann","username":"ann","is_bot":false},"query":"np","offset":"","chat_type":"supergroup"})");
        parlay::TgInlineQuery iq =
            parlay::TelegramApi::parseInlineQuery(j);
        CHECK(iq.id == "iq9" && iq.from.id == 111 &&
                  iq.from.first_name == "Ann" &&
                  iq.from.username == "ann" && !iq.from.is_bot &&
                  iq.query == "np" && iq.offset.empty() &&
                  iq.chat_type == "supergroup",
              "parseInlineQuery: id/from/query/offset/chat_type");
        // Empty query parses cleanly (hint mode).
        parlay::Json j2 = parlay::Json::parse(
            R"({"id":"iq10","from":{"id":222},"query":"","offset":""})");
        parlay::TgInlineQuery iq2 =
            parlay::TelegramApi::parseInlineQuery(j2);
        CHECK(iq2.id == "iq10" && iq2.from.id == 222 &&
                  iq2.query.empty() && iq2.chat_type.empty(),
              "parseInlineQuery: empty query, missing chat_type");
    }
    {
        // answerInlineQuery: JSON body, results array, cache_time, and
        // the switch_pm hint button; empty switch texts are omitted.
        Stub s;
        parlay::HttpServer server(0, [&](const parlay::HttpRequest& req) {
            return stubHandler(s, req);
        });
        if (!server.start()) return 1;
        parlay::TelegramApi tg(stubConfig(server.port()));
        std::string err;
        parlay::Json results = parlay::Json::parse(
            R"([{"type":"article","id":"help1","title":"1. Paste a music share URL or enter a Music ID","input_message_content":{"message_text":"Parlay v0.2.0"}}])");
        bool ok = tg.answerInlineQuery("iq9", results, 3600, "", "", &err);
        CHECK(ok && err.empty(), "answerInlineQuery: success, no error");
        CHECK(s.path == "/botTESTTOKEN/answerInlineQuery",
              "answerInlineQuery: hits answerInlineQuery");
        CHECK(contains(s.body, "\"inline_query_id\":\"iq9\""),
              "answerInlineQuery: query id in body");
        CHECK(contains(s.body, "\"cache_time\":3600"),
              "answerInlineQuery: cache_time in body");
        CHECK(contains(s.body, "\"type\":\"article\"") &&
                  contains(s.body, "\"title\":\"1. Paste a music share"),
              "answerInlineQuery: results array in body");
        CHECK(!contains(s.body, "switch_pm_text"),
              "answerInlineQuery: empty switch_pm omitted");
        // With the hint button: switch_pm_text + parameter present.
        ok = tg.answerInlineQuery(
            "iq10", parlay::Json(parlay::Json::Array{}), 30,
            "np  s  l  lib  del — or type to search", "help", &err);
        CHECK(ok, "answerInlineQuery: hint answer succeeds");
        CHECK(contains(s.body, "\"switch_pm_text\":\"np  s  l  lib  del"),
              "answerInlineQuery: switch_pm_text in body");
        CHECK(contains(s.body, "\"switch_pm_parameter\":\"help\""),
              "answerInlineQuery: switch_pm_parameter in body");
        CHECK(contains(s.body, "\"results\":[]"),
              "answerInlineQuery: empty results array in body");
        server.stop();
    }

    std::cout << (failures == 0 ? "ALL PASS\n"
                                : "FAILURES: " + std::to_string(failures) +
                                      "\n");
    return failures == 0 ? 0 : 1;
}
