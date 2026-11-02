// ipc.hpp — Parlay IPC client (contracts/ipc.json).
//
// Every service: POST /v1/<action> with header X-Parlay-Token, JSON body,
// envelope {ok:true, data:{...}} or {ok:false, error:{code, message}}.
// Health: GET /healthz -> {ok, service}.
#pragma once

#include <string>

#include "config.hpp"
#include "json.hpp"

namespace parlay {

// One IPC call result, mirroring the contract envelope.
struct IpcResult {
    bool ok = false;
    Json data;                 // present when ok
    std::string error_code;    // e.g. "engine_unavailable", "bad_request"
    std::string error_message;
    int http_status = 0;
    // True when the service could not be reached at all (transport error
    // or 5xx): the caller should try the next engine.
    bool unavailable() const {
        return http_status == 0 || http_status >= 500 ||
               error_code == "engine_unavailable";
    }
    std::string describe() const {
        if (ok) return "ok";
        return error_code + ": " + error_message;
    }
};

class IpcClient {
public:
    explicit IpcClient(const Config& cfg) : cfg_(cfg) {}

    // POST {base}/v1/<action> with the X-Parlay-Token header.
    IpcResult post(const std::string& base_url, const std::string& action,
                   const Json& body, int timeout_sec = 30) const;

    // GET {base}<path> with the X-Parlay-Token header (e.g.
    // "/v1/now-playing?chat_id=-1001", "/v1/radio/stations").
    // Same envelope as post().
    IpcResult get(const std::string& base_url, const std::string& path,
                  int timeout_sec = 30) const;

    // GET {base}/healthz. True only on {ok:true}.
    bool healthz(const std::string& base_url, int timeout_sec = 5) const;

    const Config& config() const { return cfg_; }

private:
    const Config& cfg_;
};

}  // namespace parlay
