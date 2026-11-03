// tdlib_client.cpp — TDLib 1.8.69 JSON client, BUILD-TIME linked.
//
// See tdlib_client.hpp. No runtime loading: the td_json_client_* entry points are
// called directly (prototypes below, verified against
// phase4/tdlib-src/td/telegram/td_json_client.h) and resolved by the
// linker against phase4/tdlib-src/build/libtdjson.so.1.8.69. A missing
// .so fails the link; a missing .so at runtime fails process start.
// No network is touched until authorize() is called with real
// credentials; merely creating the client and calling
// getOption/setLogVerbosityLevel works fully offline.
#include "tdlib_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <vector>

namespace parlay {
namespace {

// Linked TDLib entry points (td_json_client.h). The CMake build verifies
// all 14 td_ exports exist in the .so; these six are the ones this
// module calls. td_set_log_verbosity_level is the global log control.
extern "C" {
void* td_json_client_create();
void td_json_client_send(void* client, const char* request);
const char* td_json_client_receive(void* client, double timeout);
const char* td_json_client_execute(void* client, const char* request);
void td_json_client_destroy(void* client);
void td_set_log_verbosity_level(int verbosity_level);
}

#ifndef PARLAY_TDJSON_SONAME
#define PARLAY_TDJSON_SONAME "libtdjson.so.1.8.69"
#endif

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

std::string nowStamp() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                  .count();
    return std::to_string(ms);
}

}  // namespace

// --- Params ---------------------------------------------------------------

TdLibClient::Params TdLibClient::Params::fromEnv() {
    return fromEnvFor(SessionKind::Assistant);
}

TdLibClient::Params TdLibClient::Params::botFromEnv() {
    return fromEnvFor(SessionKind::Bot);
}

TdLibClient::Params TdLibClient::Params::fromEnvFor(SessionKind kind) {
    Params p;
    p.is_bot = (kind == SessionKind::Bot);
    p.api_id = envInt("TD_API_ID", envInt("API_ID"));
    p.api_hash = envStr("TD_API_HASH", envStr("API_HASH"));
    std::string data_dir = envStr("PARLAY_DATA_DIR", "data");
    // A set-but-empty PARLAY_DATA_DIR means "unset": an empty
    // data_dir would otherwise resolve the session dirs to absolute
    // "/tdlib-bot" / "/tdlib". The core Config applies the same rule.
    if (data_dir.empty()) data_dir = "data";
    if (p.is_bot) {
        // Bot session dir: data/tdlib-bot (TD_BOT_SESSION_DIR overrides).
        p.session_dir = envStr("TD_BOT_SESSION_DIR", data_dir + "/tdlib-bot");
        if (p.session_dir.empty())
            p.session_dir = data_dir + "/tdlib-bot";
        p.bot_token = envStr("BOT_TOKEN");
    } else {
        // Assistant session dir: data/tdlib (TD_SESSION_DIR overrides).
        // Same default as Config::fromEnv, so a host needs nothing but
        // the session dir itself for the path.
        p.session_dir =
            envStr("TD_SESSION_DIR", data_dir + "/tdlib");
        if (p.session_dir.empty()) p.session_dir = data_dir + "/tdlib";
    }
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

// --- construction ---------------------------------------------------------

TdLibClient::TdLibClient(const std::string&)
    : TdLibClient(SessionKind::Assistant) {}

TdLibClient::TdLibClient(SessionKind kind) : kind_(kind) {
    // Build-time link: the client is created directly. There is no
    // fallback when the library is absent — the binary cannot link
    // without it, by design (Phase 2 exit benchmark).
    lib_path_ = PARLAY_TDJSON_SONAME;
    client_ = td_json_client_create();
    if (!client_) {
        last_error_ = "td_json_client_create() returned null";
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
        if (client_) td_json_client_destroy(client_);
        client_ = nullptr;
    }
}

// --- raw pipe -------------------------------------------------------------

void TdLibClient::setLogVerbosity(int level) {
    td_set_log_verbosity_level(level);
}

void TdLibClient::send(const std::string& json) {
    last_sent_request_ = json;
    if (!client_) return;
    td_json_client_send(client_, json.c_str());
}

std::string TdLibClient::receive(double timeout_sec) {
    if (!client_) return "";
    const char* r = td_json_client_receive(client_, timeout_sec);
    return r ? std::string(r) : std::string();
}

std::string TdLibClient::execute(const std::string& json) {
    // td_json_client_execute ignores the client argument (NULL is
    // allowed); pass the client when there is one.
    const char* r = td_json_client_execute(client_, json.c_str());
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
        pending_updates_.push(parsed);
    }
    return raw;
}

bool TdLibClient::pollUpdate(Json& out, double timeout_sec) {
    if (!pending_updates_.empty()) {
        out = pending_updates_.front();
        pending_updates_.pop();
        return true;
    }
    bool was_update = false;
    pump(timeout_sec, &was_update);
    if (!pending_updates_.empty()) {
        out = pending_updates_.front();
        pending_updates_.pop();
        return true;
    }
    return false;
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

// --- auth -----------------------------------------------------------------

bool TdLibClient::ensureSessionDir0700(const std::string& session_dir,
                                       std::string& error_out) {
    std::error_code ec;
    std::filesystem::create_directories(session_dir, ec);
    if (ec) {
        error_out = "cannot create session directory " + session_dir +
                    ": " + ec.message();
        return false;
    }
    // Mode 0700 on the session dir (and the files subdir when TDLib
    // has already created it): the database holds the account's auth
    // keys and must not be readable by other local users.
    std::filesystem::permissions(
        session_dir,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        error_out = "cannot set mode 0700 on session directory " +
                    session_dir + ": " + ec.message();
        return false;
    }
    std::string files_dir = session_dir + "/files";
    if (std::filesystem::is_directory(files_dir, ec)) {
        std::error_code ec2;
        std::filesystem::permissions(
            files_dir,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, ec2);
        // Best effort: a files dir that cannot be tightened does not
        // block auth — the database dir itself is already 0700.
    }
    return true;
}

bool TdLibClient::handleAuthState(const std::string& state,
                                  std::string& error_out) {
    // Re-read env on every call: tests drive states by changing env
    // after construction, and a long-running process picks up creds
    // set between restarts the same way.
    Params p = Params::fromEnvFor(kind_);
    if (state == "authorizationStateWaitTdlibParameters") {
        if (!p.configured()) {
            error_out =
                "not authorized: no credentials (TD_API_ID / TD_API_HASH / "
                "TD_SESSION_DIR must all be set for setTdlibParameters)";
            return false;
        }
        if (!ensureSessionDir0700(p.session_dir, error_out)) {
            error_out = "not authorized: " + error_out;
            return false;
        }
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
        if (p.is_bot) {
            // anonx-cpp src/telegram/tdlib_client.cpp:296-302: a bot
            // session answers WaitPhoneNumber with
            // checkAuthenticationBotToken, not a phone number.
            if (p.bot_token.empty()) {
                error_out =
                    "not authorized: no credentials (BOT_TOKEN not set; "
                    "authorizationStateWaitPhoneNumber on a bot session "
                    "needs checkAuthenticationBotToken)";
                return false;
            }
            Json req = Json::objectOf({
                {"@type", Json("checkAuthenticationBotToken")},
                {"token", Json(p.bot_token)},
            });
            send(req);
            return true;
        }
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
                "with a full client and reuse the session directory)";
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
            // error answering one of our auth requests — including
            // checkAuthenticationBotToken rejecting a bad token).
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

bool TdLibClient::getMe(int64_t& id_out, std::string& username_out) {
    Json resp = call(Json::objectOf({{"@type", Json("getMe")}}), 30.0);
    if (resp.isNull()) {
        last_error_ = "getMe timed out";
        return false;
    }
    if (isError(resp)) {
        last_error_ = "getMe failed: " + errorMessage(resp);
        return false;
    }
    const Json* id = resp.find("id");
    if (!id) {
        last_error_ = "getMe failed: no id in TDLib user object";
        return false;
    }
    id_out = id->asInt();
    const Json* username = resp.find("username");
    username_out = username ? username->asString() : std::string();
    return true;
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
    td_json_client_destroy(client_);
    client_ = nullptr;
}

// --- bot-session sends ----------------------------------------------------

bool TdLibClient::isError(const Json& resp) {
    const Json* type = resp.find("@type");
    return type && type->asString() == "error";
}

std::string TdLibClient::errorMessage(const Json& resp) {
    const Json* code = resp.find("code");
    const Json* msg = resp.find("message");
    std::string out;
    if (code) out += std::to_string(code->asInt());
    if (msg && !msg->asString().empty()) {
        if (!out.empty()) out += ": ";
        out += msg->asString();
    }
    return out;
}

Json TdLibClient::formattedText(const std::string& text,
                                const std::string& parse_mode,
                                std::string& error_out) {
    if (parse_mode.empty()) {
        return Json::objectOf({
            {"@type", Json("formattedText")},
            {"text", Json(text)},
        });
    }
    Json mode;
    if (parse_mode == "HTML") {
        mode = Json::objectOf({{"@type", Json("textParseModeHTML")}});
    } else if (parse_mode == "Markdown") {
        mode = Json::objectOf({
            {"@type", Json("textParseModeMarkdown")},
            {"version", Json(static_cast<int64_t>(2))},
        });
    } else {
        error_out = "unknown parse_mode \"" + parse_mode +
                    "\" (expected \"\", \"HTML\" or \"Markdown\")";
        return Json();
    }
    Json req = Json::objectOf({
        {"@type", Json("parseTextEntities")},
        {"text", Json(text)},
        {"parse_mode", mode},
    });
    // parseTextEntities is a local computation: 10 s is ample, and a
    // timeout is reported as a parse failure (nothing is sent).
    Json resp = call(req, 10.0);
    if (resp.isNull()) {
        error_out = "parseTextEntities timed out";
        return Json();
    }
    if (isError(resp)) {
        error_out = "parseTextEntities failed: " + errorMessage(resp);
        return Json();
    }
    return resp;  // parseTextEntities returns the formattedText object
}

Json TdLibClient::inputMessageText(const std::string& text,
                                   const std::string& parse_mode,
                                   std::string& error_out) {
    Json ft = formattedText(text, parse_mode, error_out);
    if (ft.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageText")},
        {"text", ft},
        {"disable_web_page_preview", Json(true)},
        {"clear_draft", Json(true)},
    });
}

namespace {
// A synthetic TDLib-shaped error object for failures that happen
// before anything is sent (e.g. a parse failure): callers treat it
// exactly like a TDLib "error" response.
Json localError(int64_t code, const std::string& message) {
    return Json::objectOf({
        {"@type", Json("error")},
        {"code", Json(code)},
        {"message", Json(message)},
    });
}
}  // namespace

Json TdLibClient::sendMessage(int64_t chat_id, const std::string& text,
                              int64_t reply_to_message_id,
                              const std::string& parse_mode) {
    std::string err;
    Json content = inputMessageText(text, parse_mode, err);
    if (content.isNull()) return localError(400, err);
    Json req = Json::objectOf({
        {"@type", Json("sendMessage")},
        {"chat_id", Json(chat_id)},
        {"input_message_content", content},
    });
    if (reply_to_message_id != 0) {
        req.set("reply_to", Json::objectOf({
            {"@type", Json("inputMessageReplyToMessage")},
            {"message_id", Json(reply_to_message_id)},
        }));
    }
    return call(req, 30.0);
}

int64_t TdLibClient::sendMessageId(int64_t chat_id, const std::string& text,
                                   int64_t reply_to_message_id,
                                   const std::string& parse_mode,
                                   std::string* error_out) {
    Json resp = sendMessage(chat_id, text, reply_to_message_id, parse_mode);
    if (resp.isNull()) {
        if (error_out) *error_out = "sendMessage timed out";
        return -1;
    }
    if (isError(resp)) {
        if (error_out) *error_out = errorMessage(resp);
        return -1;
    }
    const Json* id = resp.find("id");
    if (!id) {
        if (error_out) *error_out = "sendMessage: no id in response";
        return -1;
    }
    return id->asInt();
}

Json TdLibClient::editMessage(int64_t chat_id, int64_t message_id,
                              const std::string& text,
                              const std::string& parse_mode) {
    std::string err;
    Json content = inputMessageText(text, parse_mode, err);
    if (content.isNull()) return localError(400, err);
    Json req = Json::objectOf({
        {"@type", Json("editMessageText")},
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"input_message_content", content},
    });
    return call(req, 30.0);
}

Json TdLibClient::answerCallbackQuery(const std::string& callback_query_id,
                                      const std::string& text,
                                      bool show_alert,
                                      int cache_time_sec) {
    Json req = Json::objectOf({
        {"@type", Json("answerCallbackQuery")},
        {"callback_query_id", Json(callback_query_id)},
        {"text", Json(text)},
        {"show_alert", Json(show_alert)},
        {"url", Json("")},
        {"cache_time", Json(static_cast<int64_t>(cache_time_sec))},
    });
    return call(req, 30.0);
}

Json TdLibClient::sendPhoto(int64_t chat_id, const std::string& photo,
                            const std::string& caption,
                            const std::string& parse_mode) {
    std::string err;
    Json ft = formattedText(caption, parse_mode, err);
    if (ft.isNull()) return localError(400, err);
    Json file;
    std::error_code ec;
    if (!photo.empty() && photo[0] == '/' &&
        std::filesystem::exists(photo, ec)) {
        file = Json::objectOf({
            {"@type", Json("inputFileLocal")},
            {"path", Json(photo)},
        });
    } else {
        // A TDLib file_id. A remote HTTP(S) URL is NOT valid here —
        // TDLib answers with its exact error, which the caller sees.
        file = Json::objectOf({
            {"@type", Json("inputFileRemote")},
            {"id", Json(photo)},
        });
    }
    Json content = Json::objectOf({
        {"@type", Json("inputMessagePhoto")},
        {"photo", file},
        {"caption", ft},
    });
    Json req = Json::objectOf({
        {"@type", Json("sendMessage")},
        {"chat_id", Json(chat_id)},
        {"input_message_content", content},
    });
    return call(req, 30.0);
}

int64_t TdLibClient::sendPhotoId(int64_t chat_id, const std::string& photo,
                                 const std::string& caption,
                                 const std::string& parse_mode,
                                 std::string* error_out) {
    Json resp = sendPhoto(chat_id, photo, caption, parse_mode);
    if (resp.isNull()) {
        if (error_out) *error_out = "sendPhoto timed out";
        return -1;
    }
    if (isError(resp)) {
        if (error_out) *error_out = errorMessage(resp);
        return -1;
    }
    const Json* id = resp.find("id");
    if (!id) {
        if (error_out) *error_out = "sendPhoto: no id in response";
        return -1;
    }
    return id->asInt();
}

// --- dual session ---------------------------------------------------------

TdLibDualSession::TdLibDualSession()
    : bot_(SessionKind::Bot), assistant_(SessionKind::Assistant) {}

bool TdLibDualSession::pollUpdate(Json& out, SessionKind& from,
                                  double timeout_sec) {
    // Split the budget so neither session can starve the other: each
    // half returns immediately when it already has a queued update.
    double half = timeout_sec / 2.0;
    if (bot_.pollUpdate(out, half)) {
        from = SessionKind::Bot;
        return true;
    }
    if (assistant_.pollUpdate(out, half)) {
        from = SessionKind::Assistant;
        return true;
    }
    return false;
}

void TdLibDualSession::close(double timeout_sec) {
    bot_.close(timeout_sec);
    assistant_.close(timeout_sec);
}

}  // namespace parlay
