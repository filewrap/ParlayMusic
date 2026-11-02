// tdlib_test.cpp — offline smoke test for the TDLib JSON client +
// TdLibVoiceCall wiring. NO network to Telegram DCs, NO credentials.
// Proves the JSON pipe is real by capturing REAL TDLib responses:
//   1. td_execute getOption("version") -> optionValueString "1.8.69"
//   2. getChat with no auth -> a real TDLib error object (401)
//   3. authorize() with no credentials -> false + "not authorized: no
//      credentials" (honest stop, no fake join)
//   4. TdLibVoiceCall::available() == false without TD_* env, and
//      makeVoiceCall() does NOT pick tdlib in that case.
// Usage: ./tdlib_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check plus the exact
// TDLib responses received.
#include <cstdlib>
#include <iostream>
#include <string>

#include "config.hpp"
#include "tdlib_client.hpp"
#include "voice_call.hpp"

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

int main() {
    // No credentials in this process: make sure the env really is empty
    // so the test proves the no-credentials path.
    unsetenv("TD_API_ID");
    unsetenv("TD_API_HASH");
    unsetenv("TD_SESSION_DIR");
    unsetenv("TD_PHONE");
    unsetenv("TD_CODE");
    unsetenv("TD_PASSWORD");
    unsetenv("TDJSON_LIB");

    parlay::TdLibClient td;
    std::cout << "lib: " << td.libPath() << "\n";
    CHECK(td.available(), "tdlib client created (dlopen libtdjson.so)");
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

    // 4. Factory wiring: no TD_* env -> tdlib backend must NOT be picked.
    {
        parlay::Config cfg = parlay::Config::fromEnv();
        parlay::TdLibVoiceCall vc(cfg);
        CHECK(!vc.available(),
              "TdLibVoiceCall unavailable without TD_* env");
        std::cout << "TdLibVoiceCall describe: " << vc.describe() << "\n";
        auto voice = parlay::makeVoiceCall(cfg);
        std::cout << "makeVoiceCall -> " << voice->name() << " ("
                  << voice->describe() << ")\n";
        CHECK(std::string(voice->name()) != "tdlib",
              "makeVoiceCall does not pick tdlib without TD_* env");
        // play() on the tdlib backend must fail honestly, never fake.
        CHECK(!vc.play(-1001234567890LL, "https://example.invalid/a.opus",
                       "test", false, 0, 1.0, 0),
              "TdLibVoiceCall::play without auth returns false");
        std::cout << "play error: " << vc.lastError() << "\n";
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
