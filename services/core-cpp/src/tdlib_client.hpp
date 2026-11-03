// tdlib_client.hpp — TDLib JSON client for core-cpp.
//
// TDLib 1.8.69 (built in phase4/tdlib-src/build) via dlopen of
// libtdjson.so, using the old JSON interface from
// td/telegram/td_json_client.h (td_json_client_create/send/receive/
// execute/destroy). dlopen, not a link dependency, so the binary still
// runs when TDLib is absent — the client then reports unavailable.
//
// TDLib is the MTProto *signaling* side only: it does authorization,
// getChat / createVideoChat / joinVideoChat (the phone.joinGroupCall
// equivalent) and hands the remote transport payload to ntgcalls.
// TDLib does not do audio itself. WebRTC stays in NtgCallsVoiceCall.
//
// Auth state machine (td_api.tl, authorizationState*):
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
//   client, reuse TD_SESSION_DIR). No values are ever logged; only
//   env var NAMES are recorded.
#pragma once

#include <cstdint>
#include <string>

#include "json.hpp"

namespace parlay {

class TdLibClient {
public:
    // dlopen candidates, in order: TDJSON_LIB, then lib/ next to the
    // binary and lib/ next to the release bin/ (the release tarball
    // layout), then the repo phase4 build output, then a bare
    // libtdjson.so on the loader path.
    explicit TdLibClient(const std::string& explicit_lib = "");
    virtual ~TdLibClient();
    TdLibClient(const TdLibClient&) = delete;
    TdLibClient& operator=(const TdLibClient&) = delete;

    // Virtual seam: tests substitute a scripted client (see
    // tdlib_test.cpp) so the join flow — getChat / createVideoChat /
    // joinVideoChat request shapes + the ntgcalls connect wiring —
    // is proven without a live Telegram session. The mock answers
    // only the TDLib side; the offer and the connect call are real.
    virtual bool available() const { return client_ != nullptr; }
    std::string libPath() const { return lib_path_; }
    virtual std::string lastError() const { return last_error_; }

    // Raw JSON pipe. receive() returns "" on timeout.
    void send(const std::string& json);
    void send(const Json& req) { send(req.dump()); }
    std::string receive(double timeout_sec);
    // Synchronous execution (only for requests documented as such,
    // e.g. getOption, setLogVerbosityLevel). "" on failure.
    std::string execute(const std::string& json);

    // Set TDLib log verbosity (0 = fatal only ... 5 = debug). Best
    // effort: a missing export is ignored, never fatal.
    void setLogVerbosity(int level);

    // Send a request and wait for the response with the matching
    // "@extra" id. Returns the response Json (type "error" on failure)
    // or Json() (null) on timeout. Updates (updateAuthorizationState
    // etc.) seen while waiting are stashed in last_update_.
    // Virtual: see the seam note above.
    virtual Json call(const Json& req, double timeout_sec = 30.0);
    // Last update object received while pumping (may be null).
    const Json& lastUpdate() const { return last_update_; }

    // Authorization state, as last seen in an updateAuthorizationState
    // update (or getAuthorizationState). Empty until the first update.
    std::string authState() const { return auth_state_; }
    // Virtual: see the seam note above.
    virtual bool authorized() const {
        return auth_state_ == "authorizationStateReady";
    }

    // Drive the auth state machine to Ready using env credentials:
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
    // phone/code/password at all. Virtual: see the seam note above.
    virtual bool authorize(double timeout_sec = 60.0);

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

    // Env-driven parameters (names only; values stay in the env).
    struct Params {
        int64_t api_id = 0;          // TD_API_ID
        std::string api_hash;        // TD_API_HASH
        // TD_SESSION_DIR (TDLib db + files). Defaults to
        // <PARLAY_DATA_DIR>/tdlib when unset (same default as
        // Config::fromEnv), so a host needs nothing but the session
        // dir itself for the path.
        std::string session_dir;
        std::string phone;           // TD_PHONE
        std::string code;            // TD_CODE
        std::string password;        // TD_PASSWORD
        std::string email;           // TD_EMAIL (email login)
        std::string email_code;      // TD_EMAIL_CODE (email login code)
        std::string first_name;      // TD_FIRST_NAME (first registration)
        std::string last_name;       // TD_LAST_NAME (first registration)
        bool qr = false;             // TD_QR=1 (QR login, not phone)
        static Params fromEnv();
        bool configured() const {
            return api_id != 0 && !api_hash.empty() && !session_dir.empty();
        }
    };

private:
    // One pump step: receive up to timeout_sec, record any
    // updateAuthorizationState, stash other updates. Returns the raw
    // JSON string ("" on timeout) and whether it was an update.
    std::string pump(double timeout_sec, bool* was_update);

    void* handle_ = nullptr;
    void* client_ = nullptr;
    std::string lib_path_;
    std::string last_error_;
    std::string auth_state_;
    std::string other_device_link_;
    Json last_update_;
    int64_t next_extra_ = 1;

    // Resolved td_json_client_* symbols (null when the lib is absent).
    using fn_create = void* (*)();
    using fn_send = void (*)(void*, const char*);
    using fn_receive = const char* (*)(void*, double);
    using fn_execute = const char* (*)(void*, const char*);
    using fn_destroy = void (*)(void*);
    using fn_set_log_verbosity = void (*)(int);
    fn_create fn_create_ = nullptr;
    fn_send fn_send_ = nullptr;
    fn_receive fn_receive_ = nullptr;
    fn_execute fn_execute_ = nullptr;
    fn_destroy fn_destroy_ = nullptr;
    fn_set_log_verbosity fn_set_log_verbosity_ = nullptr;
};

}  // namespace parlay
