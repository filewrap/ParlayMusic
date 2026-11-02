// engine_router.cpp
#include "engine_router.hpp"

namespace parlay {

Engine EngineRouter::engineFor(int64_t chat_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = per_chat_.find(chat_id);
    if (it != per_chat_.end()) return it->second;
    if (store_) {
        auto v = store_->get("engine:" + std::to_string(chat_id));
        if (v) {
            Engine e;
            if (engineFromName(*v, e)) {
                per_chat_[chat_id] = e;
                return e;
            }
        }
    }
    return Engine::Go;
}

void EngineRouter::setEngine(int64_t chat_id, Engine e) {
    std::lock_guard<std::mutex> lock(mu_);
    per_chat_[chat_id] = e;
    if (store_) store_->set("engine:" + std::to_string(chat_id), engineName(e));
}

std::string EngineRouter::baseUrl(Engine e) const {
    switch (e) {
        case Engine::Go: return cfg_.vc_go_url;
        case Engine::Ts: return cfg_.vc_ts_url;
        case Engine::Rust: return cfg_.vc_rust_url;
    }
    return cfg_.vc_go_url;
}

bool EngineRouter::healthz(const IpcClient& ipc, Engine e) const {
    return ipc.healthz(baseUrl(e));
}

IpcResult EngineRouter::callVc(const IpcClient& ipc, int64_t chat_id,
                               const std::string& action, const Json& body,
                               Engine& used_engine, bool& fell_back) const {
    Engine preferred = engineFor(chat_id);
    fell_back = false;
    IpcResult last;
    bool first = true;
    for (Engine e : fallbackOrder(preferred)) {
        // Only probe health before a non-preferred engine: the preferred
        // engine gets the real call first; its own timeout is the probe.
        if (!first && !healthz(ipc, e)) {
            last.error_code = "engine_unavailable";
            last.error_message = std::string("engine ") + engineName(e) +
                                 " failed healthz";
            last.http_status = 0;
            continue;
        }
        first = false;
        last = ipc.post(baseUrl(e), action, body);
        if (!last.unavailable()) {
            used_engine = e;
            fell_back = (e != preferred);
            return last;
        }
        // unavailable: try the next engine in the fallback order.
    }
    used_engine = preferred;
    fell_back = false;
    return last;
}

}  // namespace parlay
