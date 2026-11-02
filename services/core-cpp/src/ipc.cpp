// ipc.cpp
#include "ipc.hpp"

#include "http.hpp"

namespace parlay {

IpcResult IpcClient::post(const std::string& base_url, const std::string& action,
                          const Json& body, int timeout_sec) const {
    IpcResult out;
    std::string url = base_url + "/v1/" + action;
    std::vector<std::string> headers = {"X-Parlay-Token: " + cfg_.parlay_token};
    HttpResult r = HttpClient::postJson(url, body.dump(), headers, timeout_sec);
    out.http_status = r.status;
    if (r.status == 0) {
        out.error_code = "engine_unavailable";
        out.error_message = "service unreachable: " +
                            (r.error.empty() ? std::string("no response") : r.error);
        return out;
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        out.error_code = "internal";
        out.error_message = std::string("non-JSON response: ") + e.what();
        return out;
    }
    const Json* ok = parsed.find("ok");
    if (ok && ok->asBool()) {
        out.ok = true;
        const Json* data = parsed.find("data");
        out.data = data ? *data : Json(Json::Object{});
        return out;
    }
    const Json* err = parsed.find("error");
    if (err) {
        if (const Json* c = err->find("code")) out.error_code = c->asString();
        if (const Json* m = err->find("message")) out.error_message = m->asString();
    }
    if (out.error_code.empty()) out.error_code = "internal";
    if (out.error_message.empty())
        out.error_message = "HTTP " + std::to_string(r.status);
    return out;
}

bool IpcClient::healthz(const std::string& base_url, int timeout_sec) const {
    std::vector<std::string> headers = {"X-Parlay-Token: " + cfg_.parlay_token};
    HttpResult r = HttpClient::get(base_url + "/healthz", headers, timeout_sec);
    if (!r.ok()) return false;
    try {
        Json parsed = Json::parse(r.body);
        const Json* ok = parsed.find("ok");
        return ok && ok->asBool();
    } catch (...) {
        return false;
    }
}

IpcResult IpcClient::get(const std::string& base_url, const std::string& path,
                         int timeout_sec) const {
    IpcResult out;
    std::string url = base_url + path;
    std::vector<std::string> headers = {"X-Parlay-Token: " + cfg_.parlay_token};
    HttpResult r = HttpClient::get(url, headers, timeout_sec);
    out.http_status = r.status;
    if (r.status == 0) {
        out.error_code = "engine_unavailable";
        out.error_message = "service unreachable: " +
                            (r.error.empty() ? std::string("no response") : r.error);
        return out;
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        out.error_code = "internal";
        out.error_message = std::string("non-JSON response: ") + e.what();
        return out;
    }
    const Json* ok = parsed.find("ok");
    if (ok && ok->asBool()) {
        out.ok = true;
        const Json* data = parsed.find("data");
        out.data = data ? *data : Json(Json::Object{});
        return out;
    }
    const Json* err = parsed.find("error");
    if (err) {
        if (const Json* c = err->find("code")) out.error_code = c->asString();
        if (const Json* m = err->find("message")) out.error_message = m->asString();
    }
    if (out.error_code.empty()) out.error_code = "internal";
    if (out.error_message.empty())
        out.error_message = "HTTP " + std::to_string(r.status);
    return out;
}

}  // namespace parlay
