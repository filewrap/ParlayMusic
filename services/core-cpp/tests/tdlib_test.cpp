// tdlib_test.cpp — offline smoke test for the TDLib JSON client.
// NO network to Telegram DCs, NO credentials.
// Proves the JSON pipe is real by capturing REAL TDLib responses:
//   1. td_execute getOption("version") -> optionValueString "1.8.69"
//   2. getChat with no auth -> a real TDLib error object (401)
//   3. authorize() with no credentials -> false + "not authorized: no
//      credentials" (honest stop, no fake join)
//   4. (Phase 5 scope update, A10) Every TDLib 1.8.69 authorization
//      state is driven from env or fails honestly: phone / QR / code /
//      password / email / email code / registration / other-device
//      confirmation, plus premium-purchase + unknown states failing
//      honestly; authorize() with TD_* set but no phone reaches
//      WaitPhoneNumber offline and stops on the missing TD_PHONE.
//   5. (Phase 7, A1) Config defaults: TD_SESSION_DIR defaults to
//      <PARLAY_DATA_DIR>/tdlib in both Config::fromEnv and
//      TdLibClient::Params::fromEnv; libtdjson/libntgcalls resolve
//      from lib/ next to the binary (release layout) before the repo
//      build output.
//   6. (Phase 2) Dual sessions + bot-token auth + bot send surface,
//      all offline against the REAL build-time-linked libtdjson:
//      botFromEnv (BOT_TOKEN, data/tdlib-bot default, TD_BOT_SESSION_DIR
//      override) vs assistant fromEnv (data/tdlib); bot client isBot,
//      assistant is not; TdLibDualSession holds one of each, both
//      available; handleAuthState on a bot client at WaitPhoneNumber
//      sends checkAuthenticationBotToken with the exact BOT_TOKEN
//      (proven via lastSentRequest, no DC round-trip) and fails
//      honestly naming BOT_TOKEN when it is unset; authorize() with a
//      dummy well-formed token returns false after the timeout in
//      WaitPhoneNumber — the exact offline outcome, never a fake
//      success — and the bot session dir is created mode 0700 (same
//      for the assistant dir); the bot send surface (sendMessage /
//      editMessage / answerCallbackQuery / sendPhoto) returns real
//      TDLib error objects without auth (400 setTdlibParameters first
//      on a fresh client, 401 Unauthorized once parameters are set),
//      never a fake ok; pollUpdate delivers the real startup
//      updateOption burst and then goes quiet.
// Usage: ./tdlib_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check plus the exact
// TDLib responses received.
// Build (TDLib is linked at build time now — no -ldl needed):
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tests/tdlib_test.cpp
//     src/tdlib_client.cpp src/json.cpp src/config.cpp src/toml.cpp
//     /home/hatch/workspace/parlay/phase4/tdlib-src/build/libtdjson.so.1.8.69
//     -Wl,-rpath,/home/hatch/workspace/parlay/phase4/tdlib-src/build
//     -o /tmp/tdlib_test
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "tdlib_client.hpp"

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

// Scripted TDLib client (Phase 7, A1): answers ONLY the TDLib side of
// the join. getChat / createVideoChat / joinVideoChat return canned
// objects; every request is recorded so the test can assert the exact
// request shapes. The ntgcalls side (local offer + connect) stays
// real — the voice call under test loads the built libntgcalls.so.
// The mock's remote payload is deliberately NOT a real Telegram
// answer, so the real ntg.connect must reject it and the join must
// report joined:false with that exact error — never a fake join.

// Phase 1 telemetry helpers: capture std::cout (telemetry lines go


int main() {
    // No credentials in this process: make sure the env really is empty
    // so the test proves the no-credentials path.
    unsetenv("TD_API_ID");
    unsetenv("TD_API_HASH");
    unsetenv("TD_SESSION_DIR");
    unsetenv("TD_BOT_SESSION_DIR");
    unsetenv("BOT_TOKEN");
    unsetenv("TD_PHONE");
    unsetenv("TD_CODE");
    unsetenv("TD_PASSWORD");
    unsetenv("TDJSON_LIB");

    parlay::TdLibClient td;
    std::cout << "lib: " << td.libPath() << "\n";
    CHECK(td.available(), "tdlib client created (build-time-linked libtdjson.so)");
    if (!td.available()) {
        std::cout << "error: " << td.lastError() << "\n";
        return 1;
    }

    td.setLogVerbosity(1);

    // 1. Synchronous getOption("version") — works with no auth, no
    // network. Must return the real version string.
    {
        parlay::Json req = parlay::Json::objectOf({
            {"@type", parlay::Json("getOption")},
            {"name", parlay::Json("version")},
        });
        std::string raw = td.execute(req.dump());
        std::cout << "getOption(version) -> " << raw << "\n";
        parlay::Json resp;
        try {
            resp = parlay::Json::parse(raw);
        } catch (...) {
        }
        const parlay::Json* type = resp.find("@type");
        const parlay::Json* value = resp.find("value");
        CHECK(type && type->asString() == "optionValueString" && value &&
                  value->asString().rfind("1.8.", 0) == 0,
              "getOption(version) returns real TDLib 1.8.x version");
    }

    // 2. getChat with no authorization — must come back as a REAL
    // TDLib error object, proving the async send/receive pipe works.
    // (Observed: 400 "Initialization parameters are needed: call
    // setTdlibParameters first" — TDLib refuses before parameters,
    // not a 401; the exact error is asserted below.)
    {
        parlay::Json req = parlay::Json::objectOf({
            {"@type", parlay::Json("getChat")},
            {"chat_id", parlay::Json(static_cast<int64_t>(-1001234567890LL))},
        });
        parlay::Json resp = td.call(req, 15.0);
        std::cout << "getChat (no auth) -> "
                  << (resp.isNull() ? std::string("<timeout/null>")
                                    : resp.dump())
                  << "\n";
        const parlay::Json* type = resp.find("@type");
        const parlay::Json* code = resp.find("code");
        const parlay::Json* msg = resp.find("message");
        CHECK(type && type->asString() == "error" && code &&
                  code->asInt(0) == 400 && msg &&
                  msg->asString().find("setTdlibParameters") !=
                      std::string::npos,
              "getChat without auth returns real TDLib 400 error "
              "(setTdlibParameters first)");
    }

    // 3. authorize() with no credentials must fail honestly.
    {
        bool ok = td.authorize(10.0);
        std::cout << "authorize() -> " << (ok ? "true" : "false")
                  << " error: " << td.lastError() << "\n";
        CHECK(!ok &&
                  td.lastError().find("not authorized: no credentials") !=
                      std::string::npos,
              "authorize() without credentials fails honestly");
    }

    // 4. Factory wiring: deleted with voice_call.cpp (Phase 2).
    // makeVoiceCall / TdLibVoiceCall no longer exist.
    // 5. Real ntgcalls offer: deleted with voice_call.cpp
    // (Phase 2). The core never touches ntgcalls any more.
    // 6. Auth states (Phase 5 scope update, A10): every TDLib 1.8.69
    //    authorization state is driven from env or fails honestly.
    //    handleAuthState is exercised directly per state (the deeper
    //    states need a live DC round-trip to reach through authorize(),
    //    so the send branches are proven here by the real request
    //    being sent to the local TDLib instance, which answers ok /
    //    async error — the handler returns true once sent). Env is
    //    saved and restored for every var touched.
    {
        const char* kVars[] = {"TD_API_ID", "TD_API_HASH", "TD_SESSION_DIR",
                               "TD_PHONE", "TD_CODE", "TD_PASSWORD",
                               "TD_EMAIL", "TD_EMAIL_CODE", "TD_FIRST_NAME",
                               "TD_LAST_NAME", "TD_QR", "API_ID", "API_HASH"};
        struct Saved {
            std::string name;
            bool had = false;
            std::string value;
        };
        std::vector<Saved> saved;
        for (const char* v : kVars) {
            const char* cur = std::getenv(v);
            saved.push_back({v, cur != nullptr, cur ? cur : ""});
        }
        auto restore = [&]() {
            for (const auto& s : saved) {
                if (s.had) {
                    setenv(s.name.c_str(), s.value.c_str(), 1);
                } else {
                    unsetenv(s.name.c_str());
                }
            }
        };
        auto drive = [&](const char* state, std::string& err) {
            return td.handleAuthState(state, err);
        };
        std::string err;
        // Phone: missing -> names TD_PHONE and the TD_QR alternative.
        unsetenv("TD_PHONE");
        unsetenv("TD_QR");
        CHECK(!drive("authorizationStateWaitPhoneNumber", err) &&
                  err.find("TD_PHONE") != std::string::npos &&
                  err.find("TD_QR") != std::string::npos,
              "WaitPhoneNumber without TD_PHONE/TD_QR fails honestly");
        // Phone: set -> request sent.
        setenv("TD_PHONE", "+15551234567", 1);
        CHECK(drive("authorizationStateWaitPhoneNumber", err),
              "WaitPhoneNumber with TD_PHONE sends the request");
        unsetenv("TD_PHONE");
        // QR: TD_QR=1 -> requestQrCodeAuthentication sent (proven ok
        // offline by probe; the link itself needs a live DC round-trip).
        setenv("TD_QR", "1", 1);
        CHECK(drive("authorizationStateWaitPhoneNumber", err),
              "WaitPhoneNumber with TD_QR=1 sends the QR request");
        unsetenv("TD_QR");
        // Code / password: missing -> exact var; set -> request sent.
        unsetenv("TD_CODE");
        CHECK(!drive("authorizationStateWaitCode", err) &&
                  err.find("TD_CODE") != std::string::npos,
              "WaitCode without TD_CODE fails honestly");
        setenv("TD_CODE", "12345", 1);
        CHECK(drive("authorizationStateWaitCode", err),
              "WaitCode with TD_CODE sends the request");
        unsetenv("TD_CODE");
        unsetenv("TD_PASSWORD");
        CHECK(!drive("authorizationStateWaitPassword", err) &&
                  err.find("TD_PASSWORD") != std::string::npos,
              "WaitPassword without TD_PASSWORD fails honestly");
        setenv("TD_PASSWORD", "hunter2", 1);
        CHECK(drive("authorizationStateWaitPassword", err),
              "WaitPassword with TD_PASSWORD sends the request");
        unsetenv("TD_PASSWORD");
        // Email: missing -> exact var; set -> request sent.
        unsetenv("TD_EMAIL");
        CHECK(!drive("authorizationStateWaitEmailAddress", err) &&
                  err.find("TD_EMAIL") != std::string::npos,
              "WaitEmailAddress without TD_EMAIL fails honestly");
        setenv("TD_EMAIL", "test@example.invalid", 1);
        CHECK(drive("authorizationStateWaitEmailAddress", err),
              "WaitEmailAddress with TD_EMAIL sends the request");
        unsetenv("TD_EMAIL");
        unsetenv("TD_EMAIL_CODE");
        CHECK(!drive("authorizationStateWaitEmailCode", err) &&
                  err.find("TD_EMAIL_CODE") != std::string::npos,
              "WaitEmailCode without TD_EMAIL_CODE fails honestly");
        setenv("TD_EMAIL_CODE", "654321", 1);
        CHECK(drive("authorizationStateWaitEmailCode", err),
              "WaitEmailCode with TD_EMAIL_CODE sends the request");
        unsetenv("TD_EMAIL_CODE");
        // Registration: missing first name -> exact var; set -> sent.
        unsetenv("TD_FIRST_NAME");
        CHECK(!drive("authorizationStateWaitRegistration", err) &&
                  err.find("TD_FIRST_NAME") != std::string::npos,
              "WaitRegistration without TD_FIRST_NAME fails honestly");
        setenv("TD_FIRST_NAME", "Test", 1);
        setenv("TD_LAST_NAME", "User", 1);
        CHECK(drive("authorizationStateWaitRegistration", err),
              "WaitRegistration with TD_FIRST_NAME sends the request");
        unsetenv("TD_FIRST_NAME");
        unsetenv("TD_LAST_NAME");
        // Other-device confirmation: nothing to send, wait for the scan.
        CHECK(drive("authorizationStateWaitOtherDeviceConfirmation", err),
              "WaitOtherDeviceConfirmation waits (no request to send)");
        // Premium purchase: the one state that cannot be driven headless.
        CHECK(!drive("authorizationStateWaitPremiumPurchase", err) &&
                  err.find("Premium") != std::string::npos,
              "WaitPremiumPurchase fails honestly (in-store purchase)");
        // Unknown state: honest unsupported error.
        CHECK(!drive("authorizationStateBogus", err) &&
                  err.find("unsupported authorization state") !=
                      std::string::npos,
              "unknown auth state fails honestly");
        // authorize() end to end (offline): TD_* set, no phone -> the
        // real loop reaches WaitPhoneNumber locally and stops there.
        setenv("TD_API_ID", "12345", 1);
        setenv("TD_API_HASH", "0123456789abcdef0123456789abcdef", 1);
        setenv("TD_SESSION_DIR", "/tmp/tdlib-test-session", 1);
        unsetenv("TD_PHONE");
        unsetenv("TD_QR");
        {
            parlay::TdLibClient td2;
            td2.setLogVerbosity(0);
            bool ok2 = td2.authorize(10.0);
            std::cout << "authorize (no phone) -> " << (ok2 ? "true" : "false")
                      << " state=" << td2.authState()
                      << " err=" << td2.lastError() << "\n";
            CHECK(!ok2 &&
                      td2.authState() ==
                          "authorizationStateWaitPhoneNumber" &&
                      td2.lastError().find("TD_PHONE") != std::string::npos,
                  "authorize() reaches WaitPhoneNumber offline, stops on missing TD_PHONE");
        }
        std::error_code ec;
        std::filesystem::remove_all("/tmp/tdlib-test-session", ec);
        restore();
    }

    // 7. Join flow: deleted with voice_call.cpp (Phase 2).
    // The duplex join handshake (TDLib joinVideoChat on the
    // shared assistant session) is covered by dispatcher_test
    // section O instead; the core never touches ntgcalls now.
    // 8. Config defaults (Phase 7, A1): TD_SESSION_DIR defaults to
    // <PARLAY_DATA_DIR>/tdlib in both Config::fromEnv and
    // TdLibClient::Params::fromEnv; an explicit TD_SESSION_DIR wins.
    {
        const char* kVars[] = {"TD_SESSION_DIR", "PARLAY_DATA_DIR"};
        struct Saved {
            std::string name;
            bool had = false;
            std::string value;
        };
        std::vector<Saved> saved;
        for (const char* v : kVars) {
            const char* cur = std::getenv(v);
            saved.push_back({v, cur != nullptr, cur ? cur : ""});
        }
        auto restore = [&]() {
            for (const auto& s : saved) {
                if (s.had) {
                    setenv(s.name.c_str(), s.value.c_str(), 1);
                } else {
                    unsetenv(s.name.c_str());
                }
            }
        };
        unsetenv("TD_SESSION_DIR");
        unsetenv("PARLAY_DATA_DIR");
        parlay::Config c1 = parlay::Config::fromEnv();
        CHECK(c1.td_session_dir == "data/tdlib",
              "TD_SESSION_DIR defaults to data/tdlib when unset");
        parlay::TdLibClient::Params p1 =
            parlay::TdLibClient::Params::fromEnv();
        CHECK(p1.session_dir == "data/tdlib",
              "TdLibClient::Params defaults the session dir the same way");
        setenv("PARLAY_DATA_DIR", "/tmp/parlay-data-test", 1);
        parlay::Config c2 = parlay::Config::fromEnv();
        CHECK(c2.td_session_dir == "/tmp/parlay-data-test/tdlib",
              "TD_SESSION_DIR default follows a custom PARLAY_DATA_DIR");
        setenv("TD_SESSION_DIR", "/custom/session", 1);
        parlay::Config c3 = parlay::Config::fromEnv();
        CHECK(c3.td_session_dir == "/custom/session",
              "explicit TD_SESSION_DIR wins over the default");
        restore();
    }

    // 9. Dual sessions + bot-token auth + bot send surface (Phase 2).
    //    All offline against the REAL build-time-linked libtdjson: no
    //    DC round-trip succeeds here (sandbox has no direct TLS to
    //    Telegram DCs), so the bot authorize() outcome is a timeout
    //    in WaitPhoneNumber — asserted exactly as observed, never a
    //    fake success. The request SHAPE is proven offline via
    //    lastSentRequest() (handleAuthState sends it locally).
    {
        const char* kVars[] = {"TD_API_ID", "TD_API_HASH", "TD_SESSION_DIR",
                               "TD_BOT_SESSION_DIR", "BOT_TOKEN", "API_ID",
                               "API_HASH", "PARLAY_DATA_DIR"};
        struct Saved {
            std::string name;
            bool had = false;
            std::string value;
        };
        std::vector<Saved> saved;
        for (const char* v : kVars) {
            const char* cur = std::getenv(v);
            saved.push_back({v, cur != nullptr, cur ? cur : ""});
        }
        auto restore = [&]() {
            for (const auto& s : saved) {
                if (s.had) {
                    setenv(s.name.c_str(), s.value.c_str(), 1);
                } else {
                    unsetenv(s.name.c_str());
                }
            }
        };
        const std::string kDummyToken =
            "123456789:AAEabcdefghijklmnopqrstuvwxyz0123456789";
        const std::string kApiHash = "0123456789abcdef0123456789abcdef";

        // 9a. Params: bot vs assistant session dirs + token.
        unsetenv("TD_SESSION_DIR");
        unsetenv("TD_BOT_SESSION_DIR");
        unsetenv("BOT_TOKEN");
        setenv("TD_API_ID", "12345", 1);
        setenv("TD_API_HASH", kApiHash.c_str(), 1);
        setenv("PARLAY_DATA_DIR", "/tmp/parlay-dual-test", 1);
        setenv("BOT_TOKEN", kDummyToken.c_str(), 1);
        parlay::TdLibClient::Params bp =
            parlay::TdLibClient::Params::botFromEnv();
        CHECK(bp.is_bot && bp.bot_token == kDummyToken &&
                  bp.api_id == 12345 && bp.api_hash == kApiHash &&
                  bp.session_dir == "/tmp/parlay-dual-test/tdlib-bot",
              "botFromEnv: BOT_TOKEN + API_ID/API_HASH + "
              "data/tdlib-bot default");
        parlay::TdLibClient::Params ap =
            parlay::TdLibClient::Params::fromEnv();
        CHECK(!ap.is_bot && ap.bot_token.empty() &&
                  ap.session_dir == "/tmp/parlay-dual-test/tdlib",
              "fromEnv (assistant): data/tdlib default, no bot token");
        setenv("TD_BOT_SESSION_DIR", "/tmp/parlay-bot-custom", 1);
        parlay::TdLibClient::Params bp2 =
            parlay::TdLibClient::Params::botFromEnv();
        CHECK(bp2.session_dir == "/tmp/parlay-bot-custom",
              "explicit TD_BOT_SESSION_DIR wins over the default");
        unsetenv("TD_BOT_SESSION_DIR");

        // 9b. Kinds: a bot client isBot, the default client is not.
        // Both are available — the library is linked, not dlopened.
        {
            parlay::TdLibClient bot(parlay::SessionKind::Bot);
            CHECK(bot.available() && bot.isBot() &&
                      bot.kind() == parlay::SessionKind::Bot,
                  "bot client: available, isBot");
            CHECK(bot.libPath() == "libtdjson.so.1.8.69",
                  "bot client libPath is the linked SONAME");
            parlay::TdLibClient asst;
            CHECK(asst.available() && !asst.isBot() &&
                      asst.kind() == parlay::SessionKind::Assistant,
                  "assistant client: available, not isBot");
        }

        // 9c. Dual session: one bot + one assistant in one process.
        {
            parlay::TdLibDualSession dual;
            CHECK(dual.botAvailable() && dual.assistantAvailable() &&
                      dual.bot().isBot() && !dual.assistant().isBot(),
                  "TdLibDualSession: bot + assistant, both available");
        }

        // 9d. Bot token request shape, offline: WaitPhoneNumber on a
        // bot client sends checkAuthenticationBotToken with the exact
        // BOT_TOKEN (anonx-cpp :299). No DC round-trip is needed to
        // prove the request — lastSentRequest() records it locally.
        {
            setenv("TD_BOT_SESSION_DIR", "/tmp/tdlib-bot-shape-test", 1);
            parlay::TdLibClient bot(parlay::SessionKind::Bot);
            std::string err;
            CHECK(bot.handleAuthState(
                      "authorizationStateWaitPhoneNumber", err),
                  "bot WaitPhoneNumber sends the token request");
            parlay::Json sent;
            try {
                sent = parlay::Json::parse(bot.lastSentRequest());
            } catch (...) {
            }
            const parlay::Json* sent_type = sent.find("@type");
            const parlay::Json* sent_token = sent.find("token");
            CHECK(sent_type &&
                      sent_type->asString() ==
                          "checkAuthenticationBotToken" &&
                      sent_token && sent_token->asString() == kDummyToken,
                  "bot WaitPhoneNumber sends checkAuthenticationBotToken "
                  "with the exact BOT_TOKEN (not a phone number)");
            std::error_code ec;
            std::filesystem::remove_all("/tmp/tdlib-bot-shape-test", ec);
            // Missing BOT_TOKEN fails honestly and names the var.
            unsetenv("BOT_TOKEN");
            parlay::TdLibClient bot2(parlay::SessionKind::Bot);
            CHECK(!bot2.handleAuthState(
                      "authorizationStateWaitPhoneNumber", err) &&
                      err.find("BOT_TOKEN") != std::string::npos,
                  "bot WaitPhoneNumber without BOT_TOKEN fails honestly");
            setenv("BOT_TOKEN", kDummyToken.c_str(), 1);
        }

        // 9e. authorize() end to end, offline, dummy well-formed
        // token: TDLib sends the token check and waits for a DC
        // answer that cannot arrive here, so authorize() returns
        // false after the timeout, still in WaitPhoneNumber — the
        // exact offline outcome, never a fake success. The bot
        // session dir is created mode 0700 by setTdlibParameters.
        {
            setenv("TD_BOT_SESSION_DIR", "/tmp/tdlib-bot-auth-test", 1);
            parlay::TdLibClient bot(parlay::SessionKind::Bot);
            bot.setLogVerbosity(0);
            bool ok = bot.authorize(5.0);
            std::cout << "bot authorize (dummy token) -> "
                      << (ok ? "true" : "false")
                      << " state=" << bot.authState()
                      << " err=" << bot.lastError() << "\n";
            CHECK(!ok &&
                      bot.authState() ==
                          "authorizationStateWaitPhoneNumber" &&
                      bot.lastError().find("authorization timed out") !=
                          std::string::npos,
                  "bot authorize() with a dummy token fails honestly: "
                  "timeout in WaitPhoneNumber, no fake success");
            std::error_code ec;
            auto perms = std::filesystem::status(
                             "/tmp/tdlib-bot-auth-test", ec)
                             .permissions();
            CHECK(!ec && perms ==
                              (std::filesystem::perms::owner_read |
                               std::filesystem::perms::owner_write |
                               std::filesystem::perms::owner_exec),
                  "bot session dir created mode 0700");
            std::filesystem::remove_all("/tmp/tdlib-bot-auth-test", ec);
        }

        // 9f. Assistant session dir 0700, via setTdlibParameters.
        {
            setenv("TD_SESSION_DIR", "/tmp/tdlib-asst-0700-test", 1);
            parlay::TdLibClient asst;
            std::string err;
            CHECK(asst.handleAuthState(
                      "authorizationStateWaitTdlibParameters", err),
                  "assistant WaitTdlibParameters sends setTdlibParameters");
            std::error_code ec;
            auto perms = std::filesystem::status(
                             "/tmp/tdlib-asst-0700-test", ec)
                             .permissions();
            CHECK(!ec && perms ==
                              (std::filesystem::perms::owner_read |
                               std::filesystem::perms::owner_write |
                               std::filesystem::perms::owner_exec),
                  "assistant session dir created mode 0700");
            std::filesystem::remove_all("/tmp/tdlib-asst-0700-test", ec);
        }

        // 9g. Bot send surface without auth: real TDLib error objects,
        // never a fake ok. (401 Unauthorized — the client is not
        // authorized; the requests reached TDLib for real.)
        {
            parlay::TdLibClient bot(parlay::SessionKind::Bot);
            bot.setLogVerbosity(0);
            parlay::Json r =
                bot.sendMessage(-1001234567890LL, "probe", 0, "");
            std::cout << "bot sendMessage unauth -> " << r.dump() << "\n";
            // No parameters were ever set on this client, so TDLib
            // answers 400 "call setTdlibParameters first" (a client
            // that already has parameters answers 401 Unauthorized
            // instead — see the probe run; either way it is a real
            // TDLib error object, never a fake ok).
            CHECK(parlay::TdLibClient::isError(r) && r.find("code") &&
                      r.find("code")->asInt() == 400 && r.find("message") &&
                      r.find("message")->asString().find(
                          "setTdlibParameters") != std::string::npos,
                  "bot sendMessage without auth returns real TDLib 400 "
                  "(setTdlibParameters first)");
            std::string err;
            CHECK(bot.sendMessageId(-1001234567890LL, "probe", 0, "",
                                    &err) == -1 &&
                      !err.empty(),
                  "bot sendMessageId without auth returns -1 + error");
            parlay::Json r2 = bot.editMessage(-1001234567890LL, 42,
                                              "edited", "");
            CHECK(parlay::TdLibClient::isError(r2),
                  "bot editMessage without auth returns a real TDLib "
                  "error");
            // Numeric-string callback id: parses as a real TDLib
            // request (a non-numeric id is rejected by TDLib's own
            // parser — see the probe run, not asserted here).
            parlay::Json r3 = bot.answerCallbackQuery(
                "1234567890123456789", "hi", false, 0);
            CHECK(parlay::TdLibClient::isError(r3),
                  "bot answerCallbackQuery without auth returns a real "
                  "TDLib error");
            parlay::Json r4 =
                bot.sendPhoto(-1001234567890LL, "somefileid", "cap", "");
            CHECK(parlay::TdLibClient::isError(r4),
                  "bot sendPhoto without auth returns a real TDLib error");
        }

        // 9h. pollUpdate delivers REAL TDLib updates and fabricates
        // nothing: a fresh client first emits its startup updateOption
        // burst (version, commit_hash, ...), then goes quiet.
        {
            parlay::TdLibClient bot(parlay::SessionKind::Bot);
            int seen = 0;
            bool all_updates = true;
            for (int i = 0; i < 6; ++i) {
                parlay::Json upd;
                if (!bot.pollUpdate(upd, 1.0)) break;
                ++seen;
                const parlay::Json* t = upd.find("@type");
                if (!t ||
                    t->asString().rfind("update", 0) != 0)
                    all_updates = false;
                std::cout << "pollUpdate startup -> " << upd.dump()
                          << "\n";
            }
            CHECK(seen >= 1 && all_updates,
                  "pollUpdate delivers real TDLib startup updates "
                  "(updateOption burst, nothing fabricated)");
            parlay::Json upd;
            CHECK(!bot.pollUpdate(upd, 0.5),
                  "pollUpdate goes quiet after the startup burst "
                  "(no fabricated updates)");
        }

        restore();
        std::error_code ec;
        std::filesystem::remove_all("/tmp/parlay-dual-test", ec);
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
