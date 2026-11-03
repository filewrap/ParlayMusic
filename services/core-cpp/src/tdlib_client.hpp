// tdlib_client.hpp — TDLib JSON client for core-cpp (Phase 2 rewrite).
//
// TDLib 1.8.69 (phase4/tdlib-src/build/libtdjson.so.1.8.69) is linked at
// BUILD TIME — there is no runtime loading anywhere in this module. The
// td_json_client_* entry points are declared directly in tdlib_client.cpp
// (prototypes verified against phase4/tdlib-src/td/telegram/
// td_json_client.h); CMake links the .so, sets rpath so the binary finds
// it at runtime, and fails the build when the .so is missing or any of
// the 14 td_ exports is absent. A binary without TDLib cannot link, and
// a binary whose .so is missing at runtime cannot start.
//
// DUAL sessions in one process (anonx-cpp shape: one client per session,
// is_bot_ flag selects the auth path):
//   BOT session — authenticates via checkAuthenticationBotToken
//     (anonx-cpp src/telegram/tdlib_client.cpp:299 — the exact call
//     Parlay's old client never had) using BOT_TOKEN + API_ID/API_HASH,
//     session dir <PARLAY_DATA_DIR>/tdlib-bot, mode 0700.
//   ASSISTANT session — the existing phone/code/2FA first-auth path
//     (TD_API_ID/TD_API_HASH, TD_PHONE/TD_CODE/TD_PASSWORD, TD_QR),
//     session dir <PARLAY_DATA_DIR>/tdlib, mode 0700.
// Each session owns one td_json_client_create() instance with its own
// receive loop; the two never share a session directory, so neither
// fights the other for the TDLib database lock.
//
// TDLib is the MTProto *signaling* side only: it does authorization,
// getChat / createVideoChat / joinVideoChat (the phone.joinGroupCall
// equivalent) and hands the remote transport payload to ntgcalls.
// TDLib does not do audio itself. WebRTC stays in NtgCallsVoiceCall.
//
// Auth state machine (td_api.tl, authorizationState*), assistant:
//   WaitTdlibParameters -> setTdlibParameters (TD_API_ID / TD_API_HASH /
//   TD_SESSION_DIR) -> WaitPhoneNumber -> setAuthenticationPhoneNumber
//   (TD_PHONE) — or, with TD_QR=1, requestQrCodeAuthentication, after
//   which TDLib reports WaitOtherDeviceConfirmation with the login
//   link (see otherDeviceLink()) -> WaitCode -> checkAuthenticationCode
//   (TD_CODE) -> WaitPassword -> checkAuthenticationPassword
//   (TD_PASSWORD) -> Ready. Email accounts go WaitEmailAddress ->
//   setAuthenticationEmailAddress (TD_EMAIL) -> WaitEmailCode ->
//   checkAuthenticationEmailCode (TD_EMAIL_CODE) -> Ready. A fresh
//   phone number goes WaitRegistration -> registerUser (TD_FIRST_NAME,
//   TD_LAST_NAME) -> Ready. A fresh session directory with no
//   credentials stops honestly at the first state that needs one:
//   "not authorized: no credentials". The one state this client
//   cannot drive is WaitPremiumPurchase (an in-store purchase) — it
//   fails honestly and names the workaround (log in once with a full
//   client, reuse the session dir). No values are ever logged; only
//   env var NAMES are recorded.
//
// Bot auth differs in exactly one place (anonx-cpp :296-302): at
// WaitPhoneNumber a bot session sends checkAuthenticationBotToken with
// the BOT_TOKEN instead of a phone number. A bad token fails with the
// exact TDLib error (401/400), never a fake success.
//
// Bot-session SEND surface (the dispatcher replacement for
// telegram_api.* — Wave 2 rewires the dispatcher onto these; they are
// implemented for real against td_json_client_send here):
//   sendMessage / editMessage / answerCallbackQuery / sendPhoto.
// All four return the raw TDLib response Json (a message object, "ok",
// or an "error" object) so the caller sees the exact TDLib outcome.
// sendMessageId / sendPhotoId are conveniences returning the new
// message id (-1 + error_out on failure).
#pragma once

#include <cstdint>
#include <queue>
#include <string>

#include "json.hpp"

namespace parlay {

// Which half of the dual session a client is. Selects the session
// directory default and the WaitPhoneNumber branch (bot token vs phone).
enum class SessionKind { Assistant, Bot };

class TdLibClient {
public:
    // Build-time link: the explicit_lib argument is retained for source
    // compatibility but is now IGNORED — the library is linked at
    // build time, and no env var or config key selects a library.
    // libPath() reports the linked SONAME.
    explicit TdLibClient(const std::string& explicit_lib = "");
    // Build a client for one half of the dual session.
    explicit TdLibClient(SessionKind kind);
    virtual ~TdLibClient();
    TdLibClient(const TdLibClient&) = delete;
    TdLibClient& operator=(const TdLibClient&) = delete;

    SessionKind kind() const { return kind_; }
    bool isBot() const { return kind_ == SessionKind::Bot; }

    // Virtual seam: tests substitute a scripted client (see
    // tdlib_test.cpp) so the join flow — getChat / createVideoChat /
    // joinVideoChat request shapes + the ntgcalls connect wiring —
    // is proven without a live Telegram session. The mock answers
    // only the TDLib side; the offer and the connect call are real.
    virtual bool available() const { return client_ != nullptr; }
    // The linked libtdjson SONAME (PARLAY_TDJSON_SONAME, default
    // "libtdjson.so.1.8.69"). Not a load-time search path: the library is a
    // link dependency of the binary.
    std::string libPath() const { return lib_path_; }
    virtual std::string lastError() const { return last_error_; }

    // Raw JSON pipe. receive() returns "" on timeout.
    void send(const std::string& json);
    void send(const Json& req) { send(req.dump()); }
    // The last request passed to send(), verbatim (test seam: proves
    // which auth request a state drove, e.g. checkAuthenticationBotToken
    // vs setAuthenticationPhoneNumber, without a live DC round-trip).
    std::string lastSentRequest() const { return last_sent_request_; }
    std::string receive(double timeout_sec);
    // Synchronous execution (only for requests documented as such,
    // e.g. getOption, setLogVerbosityLevel). "" on failure.
    std::string execute(const std::string& json);

    // Set TDLib log verbosity (0 = fatal only ... 5 = debug). Calls
    // the linked td_set_log_verbosity_level directly.
    void setLogVerbosity(int level);

    // Send a request and wait for the response with the matching
    // "@extra" id. Returns the response Json (type "error" on failure)
    // or Json() (null) on timeout. Updates (updateAuthorizationState
    // etc.) seen while waiting are queued in pending_updates_ and
    // surface through pollUpdate(), never dropped.
    // Virtual: see the seam note above.
    virtual Json call(const Json& req, double timeout_sec = 30.0);
    // Last update object received while pumping (may be null).
    const Json& lastUpdate() const { return last_update_; }

    // One pump step for the update loop: receive up to timeout_sec and
    // return true + fill `out` when an update object arrived (auth
    // updates are consumed for state tracking and NOT returned here;
    // every other update is). Returns false on timeout, with `out`
    // untouched. Updates that arrived while call() was waiting are
    // queued and returned first, in arrival order.
    bool pollUpdate(Json& out, double timeout_sec = 1.0);

    // Authorization state, as last seen in an updateAuthorizationState
    // update (or getAuthorizationState). Empty until the first update.
    std::string authState() const { return auth_state_; }
    // Virtual: see the seam note above.
    virtual bool authorized() const {
        return auth_state_ == "authorizationStateReady";
    }

    // Drive the auth state machine to Ready using env credentials.
    // Bot session: BOT_TOKEN + API_ID/API_HASH, session dir
    // <PARLAY_DATA_DIR>/tdlib-bot; the WaitPhoneNumber state is
    // answered with checkAuthenticationBotToken. Assistant session:
    //   TD_API_ID, TD_API_HASH, TD_SESSION_DIR (always required),
    //   TD_PHONE, TD_CODE, TD_PASSWORD (as each state demands),
    //   TD_EMAIL, TD_EMAIL_CODE (email login), TD_FIRST_NAME,
    //   TD_LAST_NAME (first registration), TD_QR=1 (QR login instead
    //   of phone at WaitPhoneNumber; the link arrives in
    //   otherDeviceLink()).
    // Returns false + lastError() when a needed credential is missing
    // ("not authorized: no credentials ..."), when TDLib reports an
    // error, or when timeout_sec elapses without reaching Ready.
    // An already-authorized session directory reaches Ready with no
    // phone/code/password/token at all. Virtual: see the seam note above.
    virtual bool authorize(double timeout_sec = 60.0);

    // getMe on this session: fills the account's user id and username
    // from the TDLib user object. False + lastError() on any failure
    // (including a TDLib error object — the exact message is kept).
    bool getMe(int64_t& id_out, std::string& username_out);

    // QR login link, captured from the authorization_state object
    // when TDLib reports authorizationStateWaitOtherDeviceConfirmation
    // (after requestQrCodeAuthentication). Empty until that state is
    // seen; the user scans it with Telegram on another device.
    // The link itself only arrives after a live DC round-trip, so it
    // is empty in offline runs — the request path is still real.
    std::string otherDeviceLink() const { return other_device_link_; }

    // Drive ONE auth state from env (what authorize() calls per state
    // change). Public so offline tests can exercise each state without
    // a live TDLib session: returns false + error_out when the state
    // needs a credential that is not set, or when the state cannot be
    // driven headless (WaitPremiumPurchase). Returns true when the
    // state was driven (request sent) or needs no action (Ready,
    // WaitOtherDeviceConfirmation — the user scans the link).
    bool handleAuthState(const std::string& state, std::string& error_out);

    // Best-effort graceful shutdown: send "close", wait for
    // authorizationStateClosed, then destroy the client.
    void close(double timeout_sec = 10.0);

    // --- Bot-session send surface (TDLib, replaces telegram_api.*) ---
    // Each returns the raw TDLib response Json: a message object (or
    // "ok" for answerCallbackQuery) on success, an "error" object on
    // failure. Nothing is faked: a response of type "error" means the
    // send did not happen. parse_mode "" = plain text; "HTML" =
    // parseTextEntities with parse_mode HTML (the dispatcher's cards
    // are HTML — Wave 2 passes "HTML"); "Markdown" likewise.
    // A parse failure returns the parse error Json, and nothing is sent.
    Json sendMessage(int64_t chat_id, const std::string& text,
                     int64_t reply_to_message_id = 0,
                     const std::string& parse_mode = "");
    // sendMessage returning the new message id (-1 + error_out on
    // failure, including a TDLib "error" response).
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to_message_id = 0,
                          const std::string& parse_mode = "",
                          std::string* error_out = nullptr);
    // Edit a text message's content (editMessageText).
    Json editMessage(int64_t chat_id, int64_t message_id,
                     const std::string& text,
                     const std::string& parse_mode = "");
    // Answer a callback query (answerCallbackQuery). text "" = silent
    // ack. cache_time_sec is TDLib's cache_time.
    Json answerCallbackQuery(const std::string& callback_query_id,
                             const std::string& text,
                             bool show_alert = false,
                             int cache_time_sec = 0);
    // Send a photo (sendMessage + inputMessagePhoto). `photo` is a
    // TDLib file_id (inputFileRemote) or a local file path
    // (inputFileLocal). A remote HTTP(S) URL is NOT a valid
    // inputFileRemote id — callers must download it first; passing a
    // URL returns the exact TDLib error instead of a fake success.
    Json sendPhoto(int64_t chat_id, const std::string& photo,
                   const std::string& caption,
                   const std::string& parse_mode = "");
    // sendPhoto returning the new message id (-1 + error_out on failure).
    int64_t sendPhotoId(int64_t chat_id, const std::string& photo,
                        const std::string& caption,
                        const std::string& parse_mode = "",
                        std::string* error_out = nullptr);

    // True when `resp` is a TDLib error object; errorMessage() extracts
    // its "code: message" text ("" when it is not an error).
    static bool isError(const Json& resp);
    static std::string errorMessage(const Json& resp);

    // Env-driven parameters (names only; values stay in the env).
    struct Params {
        int64_t api_id = 0;          // TD_API_ID (falls back to API_ID)
        std::string api_hash;        // TD_API_HASH (falls back to API_HASH)
        // Session directory (TDLib db + files). Assistant default:
        // <PARLAY_DATA_DIR>/tdlib (TD_SESSION_DIR overrides). Bot
        // default: <PARLAY_DATA_DIR>/tdlib-bot (TD_BOT_SESSION_DIR
        // overrides). Same default as Config::fromEnv for the
        // assistant, so a host needs nothing but the session dir
        // itself for the path.
        std::string session_dir;
        std::string phone;           // TD_PHONE
        std::string code;            // TD_CODE
        std::string password;        // TD_PASSWORD
        std::string email;           // TD_EMAIL (email login)
        std::string email_code;      // TD_EMAIL_CODE (email login code)
        std::string first_name;      // TD_FIRST_NAME (first registration)
        std::string last_name;       // TD_LAST_NAME (first registration)
        bool qr = false;             // TD_QR=1 (QR login, not phone)
        std::string bot_token;       // BOT_TOKEN (bot session only)
        bool is_bot = false;         // true for botFromEnv()
        static Params fromEnv();     // assistant params (as before)
        static Params botFromEnv();  // bot params
        static Params fromEnvFor(SessionKind kind);
        bool configured() const {
            return api_id != 0 && !api_hash.empty() && !session_dir.empty();
        }
    };

private:
    // One pump step: receive up to timeout_sec, record any
    // updateAuthorizationState, queue other updates in
    // pending_updates_. Returns the raw JSON string ("" on timeout)
    // and whether it was an update.
    std::string pump(double timeout_sec, bool* was_update);

    // Build a formattedText object for `text`. Plain when parse_mode
    // is empty; otherwise parseTextEntities via call() (10 s). On a
    // parse failure returns Json() (null) + error_out.
    Json formattedText(const std::string& text,
                       const std::string& parse_mode,
                       std::string& error_out);
    // inputMessageText content object for `text` (or Json() on parse
    // failure, with error_out set).
    Json inputMessageText(const std::string& text,
                          const std::string& parse_mode,
                          std::string& error_out);
    // Create the session directory (and files subdir when present)
    // with mode 0700. False + error_out when it cannot be created.
    static bool ensureSessionDir0700(const std::string& session_dir,
                                     std::string& error_out);

    SessionKind kind_ = SessionKind::Assistant;
    void* client_ = nullptr;
    std::string lib_path_;
    std::string last_error_;
    std::string auth_state_;
    std::string other_device_link_;
    Json last_update_;
    std::string last_sent_request_;
    std::queue<Json> pending_updates_;
    int64_t next_extra_ = 1;
};

// Dual TDLib sessions in one process: the bot session (MTProto updates
// in, sends out) plus the assistant session (voice-chat signaling:
// getChat / createVideoChat / joinVideoChat). Each half owns its
// TdLibClient and its own session directory; the two receive loops are
// pumped from one thread via pollUpdate(), which reports which session
// each update came from. Main owns one DualTdLibSession; Wave 2 shares
// its assistant half with the voice path instead of opening a second
// client on the same session directory (TDLib locks the database — a
// second client on data/tdlib fails honestly at setTdlibParameters).
class TdLibDualSession {
public:
    TdLibDualSession();
    TdLibDualSession(const TdLibDualSession&) = delete;
    TdLibDualSession& operator=(const TdLibDualSession&) = delete;

    TdLibClient& bot() { return bot_; }
    TdLibClient& assistant() { return assistant_; }
    const TdLibClient& bot() const { return bot_; }
    const TdLibClient& assistant() const { return assistant_; }

    bool botAvailable() const { return bot_.available(); }
    bool assistantAvailable() const { return assistant_.available(); }

    // Authorize each half (bot: BOT_TOKEN path; assistant: phone path).
    // False + lastError() of the failing half on failure.
    bool authorizeBot(double timeout_sec = 60.0) {
        return bot_.authorize(timeout_sec);
    }
    bool authorizeAssistant(double timeout_sec = 60.0) {
        return assistant_.authorize(timeout_sec);
    }
    std::string botError() const { return bot_.lastError(); }
    std::string assistantError() const { return assistant_.lastError(); }

    // Pump one update from either session. Returns false on timeout.
    // `from` reports which session delivered it. Auth updates are
    // consumed for state tracking and never returned.
    bool pollUpdate(Json& out, SessionKind& from,
                    double timeout_sec = 1.0);

    // Best-effort graceful shutdown of both halves.
    void close(double timeout_sec = 10.0);

private:
    TdLibClient bot_;
    TdLibClient assistant_;
};

}  // namespace parlay
