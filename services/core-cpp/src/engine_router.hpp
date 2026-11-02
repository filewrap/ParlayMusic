// engine_router.hpp — per-chat VC engine selection + fail-over.
//
// ARCHITECTURE.md rule 4: engine selection is per-chat config
// (vc.engine: go | ts | rust, default go/low-ram). If the selected engine
// is down or returns engine_unavailable, the core retries the same call on
// the next engine in the chat's fallback list and tells the user which
// engine won. /engine switches the chat's preference (owner/admin only).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"
#include "ipc.hpp"
#include "store.hpp"

namespace parlay {

enum class Engine { Go, Ts, Rust };

inline const char* engineName(Engine e) {
    switch (e) {
        case Engine::Go: return "go";
        case Engine::Ts: return "ts";
        case Engine::Rust: return "rust";
    }
    return "go";
}

inline bool engineFromName(const std::string& name, Engine& out) {
    if (name == "go") { out = Engine::Go; return true; }
    if (name == "ts") { out = Engine::Ts; return true; }
    if (name == "rust") { out = Engine::Rust; return true; }
    return false;
}

// Ordered fallback: preferred engine first, then the other two in a fixed
// rotation so a dead primary never blocks playback.
inline std::vector<Engine> fallbackOrder(Engine preferred) {
    switch (preferred) {
        case Engine::Go: return {Engine::Go, Engine::Ts, Engine::Rust};
        case Engine::Ts: return {Engine::Ts, Engine::Go, Engine::Rust};
        case Engine::Rust: return {Engine::Rust, Engine::Go, Engine::Ts};
    }
    return {Engine::Go, Engine::Ts, Engine::Rust};
}

class EngineRouter {
public:
    // store may be null (tests / early boot); with a store, the per-chat
    // engine choice is durable instead of in-memory only.
    explicit EngineRouter(const Config& cfg, Store* store = nullptr)
        : cfg_(cfg), store_(store) {}

    Engine engineFor(int64_t chat_id) const;
    void setEngine(int64_t chat_id, Engine e);
    std::string baseUrl(Engine e) const;

    // POST /v1/<action> to the chat's preferred engine, failing over to
    // the next engine when the result is unavailable(). Returns the first
    // non-unavailable result, or the last attempt's result when all fail.
    // On success via a fallback engine, `used_engine` names the winner.
    IpcResult callVc(const IpcClient& ipc, int64_t chat_id,
                     const std::string& action, const Json& body,
                     Engine& used_engine, bool& fell_back) const;

    // Cheap liveness probe used before a play call when the preferred
    // engine has recently failed (avoids paying the full timeout twice).
    bool healthz(const IpcClient& ipc, Engine e) const;

private:
    const Config& cfg_;
    Store* store_ = nullptr;
    mutable std::mutex mu_;
    mutable std::map<int64_t, Engine> per_chat_;  // in-memory cache of the store
};

}  // namespace parlay
