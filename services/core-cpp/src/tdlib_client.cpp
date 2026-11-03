// tdlib_client.cpp — TDLib 1.8.69 JSON client via dlopen.
// See tdlib_client.hpp. No network is touched until authorize() is
// called with real credentials; merely creating the client and calling
// getOption/setLogVerbosityLevel works fully offline.
#include "tdlib_client.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "config.hpp"

namespace parlay {
namespace {

std::string envStr(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

int64_t envInt(const char* name, int64_t fallback = 0) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stoll(v);
    } catch (...) {
        return fallback;
    }
}

// Default libtdjson.so: the repo phase4 build output (verified
// 2026-11-02: libtdjson.so.1.8.69, 50,166,944 B, all deps resolvable
// per ldd). Dev fallback only — the release layout (lib/ next to the
// binary) is tried first in the constructor.
const char* kDefaultLib =
    "/home/hatch/workspace/parlay/phase4/tdlib-src/build/libtdjson.so";

std::string nowStamp() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                  .count();
    return std::to_string(ms);
}

}  // namespace

TdLibClient::Params TdLibClient::Params::fromEnv() {
    Params p;
    p.api_id = envInt("TD_API_ID", envInt("API_ID"));
    p.api_hash = envStr("TD_API_HASH", envStr("API_HASH"));
    // Same default as Config::fromEnv: <PARLAY_DATA_DIR>/tdlib.
    p.session_dir = envStr(
        "TD_SESSION_DIR",
        envStr("PARLAY_DATA_DIR", "data") + "/tdlib");
    p.phone = envStr("TD_PHONE");
    p.code = envStr("TD_CODE");
    p.password = envStr("TD_PASSWORD");
    p.email = envStr("TD_EMAIL");
    p.email_code = envStr("TD_EMAIL_CODE");
    p.first_name = envStr("TD_FIRST_NAME");
    p.last_name = envStr("TD_LAST_NAME");
    std::string qr = envStr("TD_QR");
    p.qr = (qr == "1" || qr == "true" || qr == "yes");
    return p;
}

TdLibClient::TdLibClient(const std::string& explicit_lib) {
    std::vector<std::string> candidates;
    if (!explicit_lib.empty()) candidates.push_back(explicit_lib);
    std::string env_lib = envStr("TDJSON_LIB");
    if (!env_lib.empty()) candidates.push_back(env_lib);
    // Release layout first: lib/ next to the binary, and lib/ next
    // to bin/ (parlay-vX.Y.Z/{bin/parlay-core, lib/libtdjson.so*}).
    // A proper host needs nothing but the session dir + the built
    // .so in lib/ — no TDJSON_LIB, no repo paths.
    std::string exe = exeDir();
    if (!exe.empty()) {
        candidates.push_back(exe + "/lib/libtdjson.so");
        candidates.push_back(exe + "/lib/libtdjson.so.1.8.69");
        candidates.push_back(exe + "/../lib/libtdjson.so");
        candidates.push_back(exe + "/../lib/libtdjson.so.1.8.69");
    }
    candidates.push_back(kDefaultLib);
    candidates.push_back("libtdjson.so");
    for (const auto& path : candidates) {
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle_) {
            lib_path_ = path;
            break;
        }
    }
    if (!handle_) {
        last_error_ = std::string("libtdjson.so not found (dlopen: ") +
                      dlerror() + ")";
        return;
    }
    auto sym = [&](const char* name) { return dlsym(handle_, name); };
    fn_create_ = reinterpret_cast<fn_create>(sym("td_json_client_create"));
    fn_send_ = reinterpret_cast<fn_send>(sym("td_json_client_send"));
    fn_receive_ =
        reinterpret_cast<fn_receive>(sym("td_json_client_receive"));
    fn_execute_ =
        reinterpret_cast<fn_execute>(sym("td_json_client_execute"));
    fn_destroy_ =
        reinterpret_cast<fn_destroy>(sym("td_json_client_destroy"));
    // Optional: td_set_log_verbosity_level (global, in libtdjson).
    fn_set_log_verbosity_ = reinterpret_cast<fn_set_log_verbosity>(
        sym("td_set_log_verbosity_level"));
    if (!fn_create_ || !fn_send_ || !fn_receive_ || !fn_execute_ ||
        !fn_destroy_) {
        last_error_ = "libtdjson.so at " + lib_path_ +
                      " lacks the td_json_client_* symbols";
        dlclose(handle_);
        handle_ = nullptr;
        return;
    }
    client_ = fn_create_();
    if (!client_) {
        last_error_ = "td_json_client_create() returned null";
        dlclose(handle_);
        handle_ = nullptr;
    }
}

TdLibClient::~TdLibClient() {
    if (client_) {
        // Best effort: close first so TDLib flushes its databases.
        // (close() pumps; the destructor must never throw, so guard.)
        try {
            close(2.0);
        } catch (...) {
        }
        if (client_ && fn_destroy_) fn_destroy_(client_);
        client_ = nullptr;
    }
    if (handle_) dlclose(handle_);
}

void TdLibClient::setLogVerbosity(int level) {
    if (fn_set_log_verbosity_) {
        fn_set_log_verbosity_(level);
        return;
    }
    // Fallback: the synchronous setLogVerbosityLevel request.
    Json req = Json::objectOf({
        {"@type", Json("setLogVerbosityLevel")},
        {"new_verbosity_level", Json(static_cast<int64_t>(level))},
    });
    execute(req.dump());
}

void TdLibClient::send(const std::string& json) {
    if (!client_) return;
    fn_send_(client_, json.c_str());
}

std::string TdLibClient::receive(double timeout_sec) {
    if (!client_) return "";
    const char* r = fn_receive_(client_, timeout_sec);
    return r ? std::string(r) : std::string();
}

std::string TdLibClient::execute(const std::string& json) {
    if (!fn_execute_) return "";
    const char* r = fn_execute_(client_, json.c_str());
    return r ? std::string(r) : std::string();
}

std::string TdLibClient::pump(double timeout_sec, bool* was_update) {
    if (was_update) *was_update = false;
    std::string raw = receive(timeout_sec);
    if (raw.empty()) return raw;
    Json parsed;
    try {
        parsed = Json::parse(raw);
    } catch (...) {
        return raw;  // not JSON: pass through verbatim
    }
    const Json* type = parsed.find("@type");
    if (type && type->asString() == "updateAuthorizationState") {
        const Json* st = parsed.find("authorization_state");
        if (st) {
            const Json* st_type = st->find("@type");
            if (st_type) {
                auth_state_ = st_type->asString();
                // QR login: the confirmation state carries the link
                // the user scans on another device.
                if (auth_state_ ==
                    "authorizationStateWaitOtherDeviceConfirmation") {
                    const Json* link = st->find("link");
                    if (link && !link->asString().empty())
                        other_device_link_ = link->asString();
                }
            }
        }
        if (was_update) *was_update = true;
        last_update_ = parsed;
        return raw;
    }
    if (type && type->asString().rfind("update", 0) == 0) {
        if (was_update) *was_update = true;
        last_update_ = parsed;
    }
    return raw;
}

Json TdLibClient::call(const Json& req_in, double timeout_sec) {
    if (!client_) return Json();
    Json req = req_in;
    std::string extra = "parlay-" + nowStamp() + "-" +
                        std::to_string(next_extra_++);
    req.set("@extra", Json(extra));
    send(req.dump());
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration<double>(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        double remaining =
            std::chrono::duration<double>(deadline -
                                          std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0) break;
        bool was_update = false;
        std::string raw = pump(std::min(remaining, 1.0), &was_update);
        if (raw.empty()) continue;
        if (was_update) continue;
        Json parsed;
        try {
            parsed = Json::parse(raw);
        } catch (...) {
            continue;
        }
        const Json* ex = parsed.find("@extra");
        if (ex && ex->asString() == extra) return parsed;
        // A response for another (concurrent) request: ignore.
    }
    return Json();  // timeout: null
}

bool TdLibClient::handleAuthState(const std::string& state,
                                  std::string& error_out) {
    Params p = Params::fromEnv();
    if (state == "authorizationStateWaitTdlibParameters") {
        if (!p.configured()) {
            error_out =
                "not authorized: no credentials (TD_API_ID / TD_API_HASH / "
                "TD_SESSION_DIR must all be set for setTdlibParameters)";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(p.session_dir, ec);
        Json params = Json::objectOf({
            {"@type", Json("setTdlibParameters")},
            {"use_test_dc", Json(false)},
            {"database_directory", Json(p.session_dir)},
            {"files_directory", Json(p.session_dir + "/files")},
            {"database_encryption_key", Json("")},
            {"use_file_database", Json(true)},
            {"use_chat_info_database", Json(true)},
            {"use_message_database", Json(true)},
            {"use_secret_chats", Json(false)},
            {"api_id", Json(p.api_id)},
            {"api_hash", Json(p.api_hash)},
            {"system_language_code", Json("en")},
            {"device_model", Json("Parlay core-cpp")},
            {"system_version", Json("Linux")},
            {"application_version", Json("0.1.0")},
        });
        send(params);
        return true;
    }
    if (state == "authorizationStateWaitPhoneNumber") {
        if (!p.phone.empty()) {
            Json settings = Json::objectOf({
                {"@type", Json("phoneNumberAuthenticationSettings")},
                {"allow_flash_call", Json(false)},
                {"allow_missed_call", Json(false)},
                {"is_current_phone_number", Json(false)},
                {"has_unknown_phone_number", Json(false)},
                {"allow_sms_retriever_api", Json(false)},
                {"firebase_authentication_settings", Json(nullptr)},
                {"authentication_tokens", Json(Json::Array{})},
            });
            Json req = Json::objectOf({
                {"@type", Json("setAuthenticationPhoneNumber")},
                {"phone_number", Json(p.phone)},
                {"settings", settings},
            });
            send(req);
            return true;
        }
        if (p.qr) {
            // QR login instead of a phone number: TDLib answers with
            // authorizationStateWaitOtherDeviceConfirmation carrying
            // the link (captured in pump() -> otherDeviceLink()).
            // The scan itself is a live-test-time human step; the
            // request is real and was accepted offline (ok response).
            Json req = Json::objectOf({
                {"@type", Json("requestQrCodeAuthentication")},
                {"other_user_ids", Json(Json::Array{})},
            });
            send(req);
            return true;
        }
        error_out =
            "not authorized: no credentials (TD_PHONE not set and "
            "TD_QR not set; authorizationStateWaitPhoneNumber needs a "
            "phone number, or TD_QR=1 for QR login)";
        return false;
    }
    if (state == "authorizationStateWaitEmailAddress") {
        if (p.email.empty()) {
            error_out =
                "not authorized: no credentials (TD_EMAIL not set; "
                "authorizationStateWaitEmailAddress needs an email "
                "address)";
            return false;
        }
        Json req = Json::objectOf({
            {"@type", Json("setAuthenticationEmailAddress")},
            {"email_address", Json(p.email)},
        });
        send(req);
        return true;
    }
    if (state == "authorizationStateWaitEmailCode") {
        if (p.email_code.empty()) {
            error_out =
                "not authorized: no credentials (TD_EMAIL_CODE not set; "
                "authorizationStateWaitEmailCode needs the code sent "
                "to the email address)";
            return false;
        }
        // checkAuthenticationEmailCode takes an
        // EmailAddressAuthentication object, not a bare string.
        Json code = Json::objectOf({
            {"@type", Json("emailAddressAuthenticationCode")},
            {"code", Json(p.email_code)},
        });
        Json req = Json::objectOf({
            {"@type", Json("checkAuthenticationEmailCode")},
            {"code", code},
        });
        send(req);
        return true;
    }
    if (state == "authorizationStateWaitRegistration") {
        if (p.first_name.empty()) {
            error_out =
                "not authorized: no credentials (TD_FIRST_NAME not set; "
                "authorizationStateWaitRegistration needs a first name "
                "to finish registering the account; TD_LAST_NAME is "
                "optional)";
            return false;
        }
        Json req = Json::objectOf({
            {"@type", Json("registerUser")},
            {"first_name", Json(p.first_name)},
            {"last_name", Json(p.last_name)},
            {"disable_notification", Json(false)},
        });
        send(req);
        return true;
    }
    if (state == "authorizationStateWaitOtherDeviceConfirmation") {
        // Nothing to send: the user confirms by scanning the link
        // (otherDeviceLink()) on another device. Keep pumping until
        // Ready or the authorize() timeout.
        return true;
    }
    if (state == "authorizationStateWaitCode") {
        if (p.code.empty()) {
            error_out =
                "not authorized: no credentials (TD_CODE not set; "
                "authorizationStateWaitCode needs the login code)";
            return false;
        }
        Json req = Json::objectOf({
            {"@type", Json("checkAuthenticationCode")},
            {"code", Json(p.code)},
        });
        send(req);
        return true;
    }
    if (state == "authorizationStateWaitPassword") {
        if (p.password.empty()) {
            error_out =
                "not authorized: no credentials (TD_PASSWORD not set; "
                "authorizationStateWaitPassword needs the 2FA password)";
            return false;
        }
        Json req = Json::objectOf({
            {"@type", Json("checkAuthenticationPassword")},
            {"password", Json(p.password)},
        });
        send(req);
        return true;
    }
    if (state == "authorizationStateReady") return true;
    // The one state this client cannot drive headless: an in-store
    // Premium purchase. Everything else TDLib 1.8.69 can present is
    // handled above; an unknown future state fails honestly.
    error_out = "not authorized: unsupported authorization state " + state +
                " (authorizationStateWaitPremiumPurchase is an in-store "
                "purchase and cannot be driven headless; log in once "
                "with a full client and reuse TD_SESSION_DIR)";
    return false;
}

bool TdLibClient::authorize(double timeout_sec) {
    if (!client_) {
        if (last_error_.empty()) last_error_ = "TDLib client not created";
        return false;
    }
    if (authorized()) return true;
    // Kick the machine: ask for the current state if none seen yet.
    if (auth_state_.empty()) {
        Json req = Json::objectOf({{"@type", Json("getAuthorizationState")}});
        send(req);
    }
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration<double>(timeout_sec);
    std::string last_state;
    while (std::chrono::steady_clock::now() < deadline) {
        if (authorized()) return true;
        bool was_update = false;
        std::string raw = pump(1.0, &was_update);
        if (!raw.empty() && !was_update) {
            // A direct response (e.g. to getAuthorizationState, or an
            // error answering one of our auth requests).
            Json parsed;
            try {
                parsed = Json::parse(raw);
            } catch (...) {
                continue;
            }
            const Json* type = parsed.find("@type");
            if (type && type->asString() == "error") {
                const Json* msg = parsed.find("message");
                last_error_ = "TDLib auth request failed: " +
                              (msg ? msg->asString() : std::string("?"));
                return false;
            }
            if (type && type->asString() == "authorizationStateReady") {
                auth_state_ = "authorizationStateReady";
                return true;
            }
            const Json* st = parsed.find("authorization_state");
            if (st) {
                const Json* st_type = st->find("@type");
                if (st_type) auth_state_ = st_type->asString();
            }
        }
        if (auth_state_.empty()) continue;
        if (auth_state_ == "authorizationStateReady") return true;
        if (auth_state_ == "authorizationStateClosed" ||
            auth_state_ == "authorizationStateClosing" ||
            auth_state_ == "authorizationStateLoggingOut") {
            last_error_ = "not authorized: TDLib is " + auth_state_;
            return false;
        }
        if (auth_state_ != last_state) {
            last_state = auth_state_;
            std::string err;
            if (!handleAuthState(auth_state_, err)) {
                last_error_ = err;
                return false;
            }
        }
    }
    last_error_ = "authorization timed out after " +
                  std::to_string(static_cast<int>(timeout_sec)) +
                  "s in state " +
                  (auth_state_.empty() ? std::string("(none seen)")
                                       : auth_state_);
    return false;
}

void TdLibClient::close(double timeout_sec) {
    if (!client_) return;
    Json req = Json::objectOf({{"@type", Json("close")}});
    send(req);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration<double>(timeout_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        bool was_update = false;
        std::string raw = pump(1.0, &was_update);
        if (auth_state_ == "authorizationStateClosed") break;
        if (raw.empty()) continue;
        if (!was_update) {
            Json parsed;
            try {
                parsed = Json::parse(raw);
            } catch (...) {
                continue;
            }
            const Json* type = parsed.find("@type");
            if (type && type->asString() == "ok") break;
        }
    }
    // Destroy regardless: TDLib frees resources once closed; if the
    // close handshake did not finish, destroy still tears the client
    // down (the docs require destroy after close, best effort here).
    if (fn_destroy_) fn_destroy_(client_);
    client_ = nullptr;
}

}  // namespace parlay
