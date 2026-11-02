// main.cpp — parlay-core entry point.
//
// Long-poll loop: getUpdates -> Dispatcher -> IPC calls -> replies.
// Config comes from the environment only (see config.hpp); no secrets are
// read from files and none are logged. State lives in Store (MongoDB wire
// protocol > Redis RESP > JSON file). Voice in-process goes through the
// VoiceCall factory (voice bridge > dlopen'd ntgcalls > unavailable with
// the exact reason); playback itself is IPC-routed to the vc-* services.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

#include "config.hpp"
#include "dispatcher.hpp"
#include "engine_router.hpp"
#include "ipc.hpp"
#include "store.hpp"
#include "telegram_api.hpp"
#include "voice_call.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    parlay::Config cfg = parlay::Config::fromEnv();
    auto missing = cfg.missingRequired();
    if (!missing.empty()) {
        std::cerr << "parlay-core: missing required env vars:";
        for (const auto& m : missing) std::cerr << " " << m;
        std::cerr << "\nSet them (see .env.example at the Parlay root) and "
                     "restart.\n";
        return 1;
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
        std::cerr << "parlay-core: no usable state backend\n";
        return 1;
    }

    parlay::TelegramApi tg(cfg);
    std::string bot = tg.getMeUsername();
    if (bot.empty()) {
        std::cerr << "parlay-core: could not reach Telegram (getMe failed). "
                     "Check BOT_TOKEN and network, then restart.\n";
        return 1;
    }
    int64_t bot_id = tg.getMeId();
    std::cout << "parlay-core: logged in as @" << bot << " (" << bot_id
              << ")\n";

    auto voice = parlay::makeVoiceCall(cfg);
    std::cout << "parlay-core: voice backend = " << voice->name() << " ("
              << voice->describe() << ")\n";

    parlay::IpcClient ipc(cfg);
    parlay::EngineRouter router(cfg, &store);
    parlay::Dispatcher dispatcher(cfg, tg, ipc, router, store, *voice, bot);
    dispatcher.setBotId(bot_id);
    dispatcher.startBackground();

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
    std::cout << "parlay-core: shutting down.\n";
    return 0;
}
