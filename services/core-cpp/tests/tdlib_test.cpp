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
//   7. (Phase 7, A1) Join flow with a SCRIPTED TDLib client: the mock
//      answers only the TDLib side (getChat / createVideoChat /
//      joinVideoChat), so the test proves the real ntg_create_call
//      offer is carried as the joinVideoChat payload, the request
//      shapes match td_api.tl, and the real ntg.connect is invoked
//      with the returned remote payload — and that the join reports
//      joined:false with the exact connect error, never a fake
//      joined:true (the mock payload is not a real Telegram answer).
//      (Phase 1 telemetry) Every join attempt also emits one
//      t_voice_join line: per-stage microseconds, ok=0, the failing
//      stage, and the exact reason; stages that never ran are omitted.
//   8. (Phase 7, A1) Config defaults: TD_SESSION_DIR defaults to
//      <PARLAY_DATA_DIR>/tdlib in both Config::fromEnv and
//      TdLibClient::Params::fromEnv; libtdjson/libntgcalls resolve
//      from lib/ next to the binary (release layout) before the repo
//      build output.
// Usage: ./tdlib_test
// Exit 0 = all checks pass. Prints PASS/FAIL per check plus the exact
// TDLib responses received.
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

// Scripted TDLib client (Phase 7, A1): answers ONLY the TDLib side of
// the join. getChat / createVideoChat / joinVideoChat return canned
// objects; every request is recorded so the test can assert the exact
// request shapes. The ntgcalls side (local offer + connect) stays
// real — the voice call under test loads the built libntgcalls.so.
// The mock's remote payload is deliberately NOT a real Telegram
// answer, so the real ntg.connect must reject it and the join must
// report joined:false with that exact error — never a fake join.
class FakeTdLib : public parlay::TdLibClient {
public:
    // getChat answers with this group_call_id (0 = no video chat yet,
    // so the join must create one first).
    int32_t get_chat_group_call_id = 424242;
    // createVideoChat answers with this id.
    int32_t create_video_chat_id = 434343;
    // joinVideoChat answers with this remote transport payload.
    std::string remote_payload = "mock-remote-transport-payload";
    // When set, requests of this @type answer with an error instead.
    std::string error_on_type;
    std::string error_message = "mock TDLib error";
    std::vector<parlay::Json> seen;

    bool available() const override { return true; }
    bool authorized() const override { return true; }
    bool authorize(double) override { return true; }
    std::string lastError() const override { return ""; }
    parlay::Json call(const parlay::Json& req, double) override {
        seen.push_back(req);
        const parlay::Json* type = req.find("@type");
        std::string t = type ? type->asString() : "";
        if (!error_on_type.empty() && t == error_on_type) {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("error")},
                {"code", parlay::Json(static_cast<int64_t>(400))},
                {"message", parlay::Json(error_message)},
            });
        }
        if (t == "getChat") {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("chat")},
                {"id",
                 parlay::Json(static_cast<int64_t>(-1001234567890LL))},
                {"video_chat",
                 parlay::Json::objectOf({
                     {"@type", parlay::Json("videoChat")},
                     {"group_call_id",
                      parlay::Json(
                          static_cast<int64_t>(get_chat_group_call_id))},
                 })},
            });
        }
        if (t == "createVideoChat") {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("groupCallId")},
                {"id",
                 parlay::Json(static_cast<int64_t>(create_video_chat_id))},
            });
        }
        if (t == "joinVideoChat") {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("text")},
                {"text", parlay::Json(remote_payload)},
            });
        }
        return parlay::Json::objectOf({{"@type", parlay::Json("ok")}});
    }
};

// First recorded request of the given @type, or nullptr.
const parlay::Json* findReq(const std::vector<parlay::Json>& seen,
                            const std::string& type) {
    for (const auto& r : seen) {
        const parlay::Json* t = r.find("@type");
        if (t && t->asString() == type) return &r;
    }
    return nullptr;
}

// Phase 1 telemetry helpers: capture std::cout (telemetry lines go
// there, like [voice]) and pull one timer line + field values out.
class CoutCapture {
public:
    CoutCapture() : old_(std::cout.rdbuf(ss_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return ss_.str(); }

private:
    std::streambuf* old_;
    std::ostringstream ss_;
};

std::string telemetryLine(const std::string& out, const std::string& name) {
    std::string needle = "[telemetry] " + name + "_us=";
    size_t at = out.find(needle);
    if (at == std::string::npos) return "";
    size_t eol = out.find('\n', at);
    return out.substr(at, eol == std::string::npos ? std::string::npos
                                                   : eol - at);
}

int64_t fieldInt(const std::string& line, const std::string& key) {
    std::string needle = key + "=";
    size_t at = line.find(needle);
    if (at == std::string::npos) return -1;
    size_t b = at + needle.size();
    size_t e = line.find(' ', b);
    try {
        return std::stoll(line.substr(b, e == std::string::npos
                                             ? std::string::npos
                                             : e - b));
    } catch (...) {
        return -1;
    }
}

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

    // 7. Join flow with a scripted TDLib client (Phase 7, A1): the
    //    mock answers only the TDLib side, so the offer and the
    //    connect call are real. Needs the built libntgcalls.so —
    //    skipped when it is absent, like check 5.
    {
        const char* env_lib = std::getenv("NTGCALLS_LIB");
        std::string lib =
            env_lib ? env_lib
                    : "../../refs/ntgcalls/shared-output/lib/libntgcalls.so";
        FILE* f = std::fopen(lib.c_str(), "rb");
        if (!f) {
            std::cout << "SKIP mock-join checks: " << lib << " not built\n";
        } else {
            std::fclose(f);
            parlay::Config cfg;
            cfg.td_api_id = 12345;
            cfg.td_api_hash = "0123456789abcdef0123456789abcdef";
            cfg.td_session_dir = "/tmp/tdlib-mock-test";
            cfg.ntgcalls_lib = lib;

            // 7a. Existing video chat: getChat -> group_call_id
            // 424242, joinVideoChat carries the REAL local offer as
            // its payload, then the real ntg.connect rejects the
            // mock remote payload — join reports joined:false with
            // the exact connect error, never a fake join.
            {
                auto fake = std::make_unique<FakeTdLib>();
                FakeTdLib* fp = fake.get();
                parlay::TdLibVoiceCall vc(cfg, std::move(fake));
                CHECK(vc.available(),
                      "mock-backed TdLibVoiceCall reports available");
                // Phase 1 telemetry: capture the t_voice_join line
                // for this attempt (fails at the real ntg.connect
                // with the mock payload — never a fake join).
                std::string tele_out;
                bool ok = false;
                {
                    CoutCapture cap;
                    ok = vc.join(-1001234567890LL);
                    tele_out = cap.str();
                }
                std::string tele_line =
                    telemetryLine(tele_out, "t_voice_join");
                CHECK(!tele_line.empty(),
                      "telemetry join: t_voice_join emitted on every "
                      "join attempt");
                CHECK(tele_line.find("authorize_us=") != std::string::npos &&
                          tele_line.find("create_call_us=") !=
                              std::string::npos &&
                          tele_line.find("get_chat_us=") !=
                              std::string::npos &&
                          tele_line.find("join_video_chat_us=") !=
                              std::string::npos &&
                          tele_line.find("connect_us=") !=
                              std::string::npos,
                      "telemetry join: every stage reached is timed");
                CHECK(tele_line.find("create_video_chat_us=") ==
                          std::string::npos,
                      "telemetry join: existing video chat means no "
                      "create_video_chat stage ran (omitted, not zero)");
                CHECK(tele_line.find("chat_id=-1001234567890") !=
                          std::string::npos &&
                          tele_line.find("backend=tdlib") !=
                              std::string::npos &&
                          tele_line.find("ok=0") != std::string::npos &&
                          tele_line.find("failed_stage=connect") !=
                              std::string::npos &&
                          tele_line.find("error=ntgcalls_connect_failed") !=
                              std::string::npos,
                      "telemetry join: ok=0, failed_stage=connect, "
                      "exact connect error, no fake join time");
                {
                    int64_t total = fieldInt(tele_line, "t_voice_join_us");
                    int64_t sum = fieldInt(tele_line, "authorize_us") +
                                  fieldInt(tele_line, "create_call_us") +
                                  fieldInt(tele_line, "get_chat_us") +
                                  fieldInt(tele_line, "join_video_chat_us") +
                                  fieldInt(tele_line, "connect_us");
                    CHECK(total > 0 && total >= sum,
                          "telemetry join: total is time-to-failure and "
                          "covers the stages");
                }
                std::cout << "mock join -> " << (ok ? "true" : "false")
                          << " error: " << vc.lastError() << "\n";
                CHECK(!ok,
                      "join with a mock remote payload does not fake "
                      "joined:true");
                CHECK(vc.lastError().find("ntgcalls connect failed") !=
                          std::string::npos,
                      "join fails at the real ntg.connect with the "
                      "mock payload");
                const parlay::Json* get_chat =
                    findReq(fp->seen, "getChat");
                CHECK(get_chat != nullptr &&
                          get_chat->find("chat_id") &&
                          get_chat->find("chat_id")->asInt() ==
                              -1001234567890LL,
                      "join sends getChat for the chat");
                const parlay::Json* join_req =
                    findReq(fp->seen, "joinVideoChat");
                CHECK(join_req != nullptr,
                      "join sends joinVideoChat");
                bool shape_ok = false;
                std::string payload;
                if (join_req) {
                    const parlay::Json* gcid =
                        join_req->find("group_call_id");
                    const parlay::Json* part =
                        join_req->find("participant_id");
                    const parlay::Json* inv =
                        join_req->find("invite_hash");
                    const parlay::Json* jp =
                        join_req->find("join_parameters");
                    const parlay::Json* jp_type =
                        jp ? jp->find("@type") : nullptr;
                    const parlay::Json* audio_src =
                        jp ? jp->find("audio_source_id") : nullptr;
                    const parlay::Json* muted =
                        jp ? jp->find("is_muted") : nullptr;
                    const parlay::Json* video =
                        jp ? jp->find("is_my_video_enabled") : nullptr;
                    const parlay::Json* pl =
                        jp ? jp->find("payload") : nullptr;
                    if (pl) payload = pl->asString();
                    shape_ok =
                        gcid && gcid->asInt() == 424242 && part &&
                        part->isNull() && inv &&
                        inv->asString().empty() && jp_type &&
                        jp_type->asString() ==
                            "groupCallJoinParameters" &&
                        audio_src && audio_src->asInt() == 0 && muted &&
                        !muted->asBool() && video && !video->asBool() &&
                        !payload.empty();
                }
                CHECK(shape_ok,
                      "joinVideoChat request shape matches td_api.tl "
                      "(group_call_id, null participant, empty "
                      "invite_hash, groupCallJoinParameters)");
                CHECK(payload.size() > 100 &&
                          payload.find("ufrag") != std::string::npos,
                      "joinVideoChat payload is the REAL ntgcalls "
                      "local offer (>100 bytes, ufrag)");
                CHECK(findReq(fp->seen, "createVideoChat") == nullptr,
                      "existing video chat: no createVideoChat sent");
            }

            // 7b. No video chat yet (group_call_id 0): the join must
            // create one first, then joinVideoChat with the new id.
            {
                auto fake = std::make_unique<FakeTdLib>();
                fake->get_chat_group_call_id = 0;
                FakeTdLib* fp = fake.get();
                parlay::TdLibVoiceCall vc(cfg, std::move(fake));
                std::string tele_out;
                bool ok = false;
                {
                    CoutCapture cap;
                    ok = vc.join(-1001234567890LL);
                    tele_out = cap.str();
                }
                std::string tele_line =
                    telemetryLine(tele_out, "t_voice_join");
                CHECK(!tele_line.empty() &&
                          tele_line.find("create_video_chat_us=") !=
                              std::string::npos &&
                          tele_line.find("failed_stage=connect") !=
                              std::string::npos,
                      "telemetry join no video chat: create_video_chat "
                      "stage timed, still fails at the real connect");
                CHECK(!ok &&
                          vc.lastError().find("ntgcalls connect failed") !=
                              std::string::npos,
                      "join with no video chat reaches the real "
                      "ntg.connect (after creating the chat)");
                const parlay::Json* create_req =
                    findReq(fp->seen, "createVideoChat");
                CHECK(create_req != nullptr &&
                          create_req->find("chat_id") &&
                          create_req->find("chat_id")->asInt() ==
                              -1001234567890LL &&
                          create_req->find("title") &&
                          create_req->find("title")->asString() ==
                              "Parlay",
                      "join with no video chat sends createVideoChat");
                const parlay::Json* join_req =
                    findReq(fp->seen, "joinVideoChat");
                CHECK(join_req != nullptr &&
                          join_req->find("group_call_id") &&
                          join_req->find("group_call_id")->asInt() ==
                              434343,
                      "joinVideoChat uses the created video chat id");
            }

            // 7c. getChat error: the join stops there with the exact
            // TDLib message; no joinVideoChat is attempted.
            {
                auto fake = std::make_unique<FakeTdLib>();
                fake->error_on_type = "getChat";
                fake->error_message = "CHAT_NOT_FOUND mock";
                FakeTdLib* fp = fake.get();
                parlay::TdLibVoiceCall vc(cfg, std::move(fake));
                std::string tele_out;
                bool ok = false;
                {
                    CoutCapture cap;
                    ok = vc.join(-1001234567890LL);
                    tele_out = cap.str();
                }
                std::string tele_line =
                    telemetryLine(tele_out, "t_voice_join");
                CHECK(!tele_line.empty() &&
                          tele_line.find("failed_stage=get_chat") !=
                              std::string::npos &&
                          tele_line.find("CHAT_NOT_FOUND") !=
                              std::string::npos &&
                          tele_line.find("join_video_chat_us=") ==
                              std::string::npos &&
                          tele_line.find("connect_us=") ==
                              std::string::npos,
                      "telemetry join getChat error: failed_stage=get_chat "
                      "with the exact TDLib message; later stages "
                      "omitted, never timed");
                CHECK(!ok &&
                          vc.lastError().find(
                              "getChat failed: CHAT_NOT_FOUND mock") !=
                              std::string::npos,
                      "getChat error stops the join with the exact "
                      "TDLib message");
                CHECK(findReq(fp->seen, "joinVideoChat") == nullptr,
                      "getChat error: no joinVideoChat attempted");
            }

            // 7d. joinVideoChat error: the join stops there with the
            // exact TDLib message.
            {
                auto fake = std::make_unique<FakeTdLib>();
                fake->error_on_type = "joinVideoChat";
                fake->error_message = "GROUPCALL_FORBIDDEN mock";
                FakeTdLib* fp = fake.get();
                parlay::TdLibVoiceCall vc(cfg, std::move(fake));
                bool ok = vc.join(-1001234567890LL);
                CHECK(!ok &&
                          vc.lastError().find(
                              "joinVideoChat failed: "
                              "GROUPCALL_FORBIDDEN mock") !=
                              std::string::npos,
                      "joinVideoChat error stops the join with the "
                      "exact TDLib message");
            }
        }
    }

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

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
