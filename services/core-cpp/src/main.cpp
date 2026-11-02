// main.cpp — parlay-core entry point.
//
// Long-poll loop: getUpdates -> Dispatcher -> IPC calls -> replies.
// Config comes from the environment only (see config.hpp); no secrets are
// read from files and none are logged. State lives in Store (MongoDB wire
// protocol > Redis RESP > JSON file). Voice in-process goes through the
// VoiceCall factory (voice bridge > dlopen'd ntgcalls > unavailable with
// the exact reason); playback itself is IPC-routed to the vc-* services.
//
// The HTTP API (GET /healthz, GET /api/now-playing, GET /api/queue —
// see core_api.hpp) starts FIRST and unconditionally (Phase 6, A2):
// the GUI surface must not depend on Telegram polling. When BOT_TOKEN
// is unset the core runs HTTP-only (no polling); when the store is
// unusable the API still answers /healthz and honestly reports no
// playback state. A missing PARLAY_TOKEN only warns: engine calls go
// out unauthenticated and engines that require a token refuse honestly.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#include "config.hpp"
#include "core_api.hpp"
#include "dispatcher.hpp"
#include "engine_router.hpp"
#include "http_server.hpp"
#include "ipc.hpp"
#include "store.hpp"
#include "telegram_api.hpp"
#include "voice_call.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

// Adapts the Dispatcher to the HTTP API's provider seam. The pointer is
// null until the dispatcher is built, and stays null if the store is
// unusable (HTTP-only mode); the API then honestly reports no playback
// state. In HTTP-only mode (no BOT_TOKEN) the dispatcher is still built,
// so the API can proxy real engine state.
struct DispatcherProvider : parlay::VcStateProvider {
    parlay::Dispatcher* dispatcher = nullptr;
    bool nowPlaying(int64_t chat_id, parlay::Json& out,
                    std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->apiNowPlaying(chat_id, out, err);
    }
    bool queueList(int64_t chat_id, parlay::Json& out,
                   std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->apiQueueList(chat_id, out, err);
    }
};

void waitForSignal() {
    while (!g_stop) std::this_thread::sleep_for(std::chrono::seconds(1));
}
}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    parlay::Config cfg = parlay::Config::fromEnv();

    // HTTP API first: it serves even with no BOT_TOKEN (A2). If the
    // port cannot be bound the Telegram side still runs, but the
    // failure is loud — /healthz and /api/* are the GUI's only window
    // into the core.
    DispatcherProvider provider;
    parlay::CoreApi api(provider);
    parlay::HttpServer http(cfg.port, [&](const parlay::HttpRequest& req) {
        return api.handle(req);
    });
    if (http.start()) {
        std::cout << "parlay-core: http api on :" << http.port() << "\n";
    } else {
        std::cerr << "parlay-core: WARNING: could not bind HTTP port "
                  << cfg.port << " — /healthz and /api/* unavailable\n";
    }

    // Telegram polling needs BOT_TOKEN; the HTTP API + dispatcher do
    // not. A missing PARLAY_TOKEN only warns: engine calls go out
    // unauthenticated and engines that require a token refuse honestly.
    bool telegram_up = !cfg.bot_token.empty();
    if (!telegram_up || cfg.parlay_token.empty()) {
        std::cerr << "parlay-core: WARNING: missing env vars:";
        if (cfg.bot_token.empty()) std::cerr << " BOT_TOKEN";
        if (cfg.parlay_token.empty()) std::cerr << " PARLAY_TOKEN";
        std::cerr << "\n";
        if (!telegram_up)
            std::cerr << "parlay-core: Telegram polling disabled — HTTP "
                         "API only. Set BOT_TOKEN (see .env.example at the "
                         "Parlay root) and restart for full operation.\n";
        if (cfg.parlay_token.empty())
            std::cerr << "parlay-core: WARNING: engine calls go out "
                         "unauthenticated; engines with a token set will "
                         "refuse them.\n";
    }
    std::cout << "parlay-core starting: " << cfg.summary() << "\n";

    // Optional log file (for /logs). stdout/stderr are appended there too.
    if (!cfg.log_file.empty()) {
        FILE* lf = std::freopen(cfg.log_file.c_str(), "a", stdout);
        if (lf) {
            std::cerr << "parlay-core: logging to " << cfg.log_file << "\n";
        } else {
            std::cerr << "parlay-core: cannot open log file "
                      << cfg.log_file << "\n";
        }
    }

    parlay::Store store(cfg);
    if (!store.ok()) {
        // The dispatcher (and per-chat engine selection) needs the
        // store; without it the core is HTTP-only and /api/* honestly
        // reports no playback state. /healthz still answers.
        std::cerr << "parlay-core: no usable state backend — HTTP API "
                     "only\n";
        waitForSignal();
        http.stop();
        std::cout << "parlay-core: shutting down.\n";
        return 0;
    }

    parlay::TelegramApi tg(cfg);
    auto voice = parlay::makeVoiceCall(cfg);
    std::cout << "parlay-core: voice backend = " << voice->name() << " ("
              << voice->describe() << ")\n";

    parlay::IpcClient ipc(cfg);
    parlay::EngineRouter router(cfg, &store);
    parlay::Dispatcher dispatcher(cfg, tg, ipc, router, store, *voice, "");
    provider.dispatcher = &dispatcher;
    dispatcher.startBackground();

    if (!telegram_up) {
        // No BOT_TOKEN: the HTTP API above is live and can proxy real
        // engine state, but nothing polls Telegram.
        waitForSignal();
        dispatcher.stopBackground();
        http.stop();
        std::cout << "parlay-core: shutting down.\n";
        return 0;
    }

    std::string bot = tg.getMeUsername();
    if (bot.empty()) {
        std::cerr << "parlay-core: could not reach Telegram (getMe failed). "
                     "Check BOT_TOKEN and network, then restart.\n";
        dispatcher.stopBackground();
        http.stop();
        return 1;
    }
    int64_t bot_id = tg.getMeId();
    std::cout << "parlay-core: logged in as @" << bot << " (" << bot_id
              << ")\n";

    dispatcher.setBotId(bot_id);
    dispatcher.setBotUsername(bot);

    int64_t offset = 0;
    int backoff_sec = 1;
    while (!g_stop) {
        std::string err;
        auto updates = tg.getUpdates(offset, 30, err);
        if (!err.empty()) {
            // Transport/API error: back off (1s -> 30s cap), then retry.
            std::cerr << "[poll] getUpdates error: " << err << " — retrying in "
                      << backoff_sec << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(backoff_sec));
            if (backoff_sec < 30) backoff_sec *= 2;
            continue;
        }
        backoff_sec = 1;
        for (const auto& u : updates) {
            offset = std::max(offset, u.update_id + 1);
            if (u.has_message) dispatcher.handleMessage(u.message);
        }
    }
    dispatcher.stopBackground();
    http.stop();
    std::cout << "parlay-core: shutting down.\n";
    return 0;
}
