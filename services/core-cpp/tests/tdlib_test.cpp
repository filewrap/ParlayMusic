// tdlib_test.cpp — offline smoke test for the TDLib JSON client +
// TdLibVoiceCall wiring. NO network to Telegram DCs, NO credentials.
// Proves the JSON pipe is real by capturing REAL TDLib responses:
//   1. td_execute getOption("version") -> optionValueString "1.8.69"
//   2. getChat with no auth -> a real TDLib error object (401)
//   3. authorize() with no credentials -> false + "not authorized: no
//      credentials" (honest stop, no fake join)
//   4. TdLibVoiceCall::available() == false without TD_* env, and
//      makeVoiceCall() does NOT pick tdlib in that case.
//   5. (Phase 5) When the built libntgcalls.so exists, NtgCallsVoiceCall
//      loads it in-process and ntg_create_call returns a real local SDP
//      offer (>100 bytes, contains ufrag); skipped when the .so is absent.
//   6. (Phase 5 scope update, A10) Every TDLib 1.8.69 authorization
//      state is driven from env or fails honestly: phone / QR / code /
//      password / email / email code / registration / other-device
//      confirmation, plus premium-purchase + unknown states failing
//      honestly; authorize() with TD_* set but no phone reaches
//      WaitPhoneNumber offline and stops on the missing TD_PHONE.
// Usage: ./tdlib_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check plus the exact
// TDLib responses received.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

    // 5. Real ntgcalls offer (Phase 5): when the built libntgcalls.so
    //    exists (NTGCALLS_LIB env, else the repo build output), the
    //    in-process NtgCallsVoiceCall must load it and create a real
    //    local SDP offer — synchronous 3.0.5 C API, no wheel, no
    //    subprocess, no credentials. Skipped when the .so is absent
    //    (the build is the gate, not this test).
    {
        const char* env_lib = std::getenv("NTGCALLS_LIB");
        std::string lib =
            env_lib ? env_lib
                    : "../../refs/ntgcalls/shared-output/lib/libntgcalls.so";
        FILE* f = std::fopen(lib.c_str(), "rb");
        if (f) {
            std::fclose(f);
            parlay::NtgCallsVoiceCall ntg(lib);
            CHECK(ntg.available(),
                  "NtgCallsVoiceCall loads built libntgcalls.so");
            std::cout << "ntgcalls describe: " << ntg.describe() << "\n";
            std::string offer;
            bool ok = ntg.createOffer(-1001234567890LL, offer);
            std::cout << "createOffer -> " << (ok ? "true" : "false")
                      << " (" << offer.size() << " bytes)"
                      << (ok ? "" : " error: " + ntg.lastError()) << "\n";
            CHECK(ok && offer.size() > 100 &&
                      offer.find("ufrag") != std::string::npos,
                  "ntg_create_call returns a real local SDP offer");
            if (ok) std::cout << "offer head: " << offer.substr(0, 120)
                              << "...\n";
        } else {
            std::cout << "SKIP real-offer check: " << lib << " not built\n";
        }
    }

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

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
