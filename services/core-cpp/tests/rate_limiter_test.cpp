// rate_limiter_test.cpp — per-chat AND per-user token-bucket limiter.
//
// - Burst allowed up to capacity, then denied; the denial carries a
//   retry-after (> 0) and a non-empty honest reason naming the scope.
// - Sustained rate enforced: after the burst, acquires stay denied
//   until the bucket refills.
// - Refill over time: tokens come back at refill_rate_per_sec.
// - Per-chat isolation: exhausting one chat does not touch another.
// - Per-user isolation: exhausting one user does not touch another
//   user in the same chat (and vice versa).
// - A denial deducts nothing: a denied attempt does not make the
//   next retry-after longer.
// - Thread-safety smoke: concurrent acquires never allow more than
//   capacity tokens out of a fresh bucket.
//
// Build: g++ -std=c++20 -I src tests/rate_limiter_test.cpp
//   src/rate_limiter.cpp src/config.cpp src/toml.cpp
//   -o /tmp/rate_limiter_test && /tmp/rate_limiter_test
// Exit 0 = all checks pass. No network, no Telegram, no credentials.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "rate_limiter.hpp"

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            std::cout << "PASS " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << "\n";                           \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

parlay::RateLimitConfig bucketCfg(int chat_cap, double chat_rate,
                                  int user_cap, double user_rate) {
    parlay::RateLimitConfig cfg;
    cfg.chat.capacity = chat_cap;
    cfg.chat.refill_rate_per_sec = chat_rate;
    cfg.user.capacity = user_cap;
    cfg.user.refill_rate_per_sec = user_rate;
    return cfg;
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

int main() {
    // --- A. burst allowed, then denied with retry-after + reason ----
    {
        parlay::RateLimiter lim(bucketCfg(3, 1.0, 10, 10.0));
        int allowed = 0;
        for (int i = 0; i < 3; ++i)
            if (lim.tryAcquire(-1001, 11).allowed) ++allowed;
        CHECK(allowed == 3, "limiter: burst of 3 allowed at capacity 3");
        parlay::RateLimitDecision d = lim.tryAcquire(-1001, 11);
        CHECK(!d.allowed, "limiter: 4th acquire in the burst denied");
        CHECK(d.retry_after_sec > 0.0 && d.retry_after_sec <= 1.0,
              "limiter: denial retry-after is in (0, 1.0]s, got " +
                  std::to_string(d.retry_after_sec));
        CHECK(!d.reason.empty() && contains(d.reason, "chat") &&
                  contains(d.reason, "retry after"),
              "limiter: denial reason names the chat scope and the "
              "retry: " +
                  d.reason);
        CHECK(d.scope == "chat", "limiter: denial scope is chat");
    }

    // --- B. sustained rate enforced ----------------------------------
    {
        // Capacity 1, refill 2/s: one immediate token, then ~0.5 s
        // per further token. Five rapid acquires must yield exactly 1.
        parlay::RateLimiter lim(bucketCfg(1, 2.0, 10, 10.0));
        int allowed = 0;
        for (int i = 0; i < 5; ++i)
            if (lim.tryAcquire(-1002, 22).allowed) ++allowed;
        CHECK(allowed == 1,
              "limiter: sustained rate enforced (1 of 5 rapid acquires "
              "allowed at capacity 1, refill 2/s)");
    }

    // --- C. refill over time ------------------------------------------
    {
        parlay::RateLimiter lim(bucketCfg(2, 5.0, 10, 10.0));
        CHECK(lim.tryAcquire(-1003, 33).allowed &&
                  lim.tryAcquire(-1003, 33).allowed,
              "limiter: refill test drains the burst of 2");
        CHECK(!lim.tryAcquire(-1003, 33).allowed,
              "limiter: drained bucket denies immediately");
        sleepMs(250);  // 5/s * 0.25 s = 1.25 tokens
        CHECK(lim.tryAcquire(-1003, 33).allowed,
              "limiter: ~1 token refilled after 250 ms at 5/s");
        CHECK(!lim.tryAcquire(-1003, 33).allowed,
              "limiter: only ~1 token refilled (fraction left < 1)");
        sleepMs(250);
        CHECK(lim.tryAcquire(-1003, 33).allowed,
              "limiter: second token refilled after another 250 ms");
    }

    // --- D. per-chat isolation ----------------------------------------
    {
        parlay::RateLimiter lim(bucketCfg(1, 0.5, 10, 10.0));
        CHECK(lim.tryAcquire(-1004, 44).allowed,
              "limiter: chat A first acquire allowed");
        CHECK(!lim.tryAcquire(-1004, 44).allowed,
              "limiter: chat A drained");
        CHECK(lim.tryAcquire(-1005, 44).allowed,
              "limiter: chat B unaffected by chat A (per-chat buckets)");
        CHECK(!lim.tryAcquire(-1005, 44).allowed,
              "limiter: chat B drains independently");
    }

    // --- E. per-user isolation ----------------------------------------
    {
        // Chat bucket is roomy (10); the user bucket (1) is the binding
        // constraint, so users in the same chat are independent.
        parlay::RateLimiter lim(bucketCfg(10, 10.0, 1, 0.5));
        CHECK(lim.tryAcquire(-1006, 55).allowed,
              "limiter: user 55 first acquire allowed");
        parlay::RateLimitDecision d = lim.tryAcquire(-1006, 55);
        CHECK(!d.allowed && d.scope == "user",
              "limiter: user 55 denied on the user bucket, scope=user"
              + (d.allowed ? "" : ": " + d.reason));
        CHECK(lim.tryAcquire(-1006, 66).allowed,
              "limiter: user 66 unaffected by user 55 (per-user buckets)");
        CHECK(!lim.tryAcquire(-1006, 66).allowed,
              "limiter: user 66 drains independently");
    }

    // --- F. user bucket binds before the chat bucket -------------------
    {
        // Chat bucket roomy, user bucket tight: the denial must blame
        // the user scope, not the chat.
        parlay::RateLimiter lim(bucketCfg(10, 10.0, 2, 1.0));
        CHECK(lim.tryAcquire(-1007, 77).allowed &&
                  lim.tryAcquire(-1007, 77).allowed,
              "limiter: user bucket burst of 2 allowed");
        parlay::RateLimitDecision d = lim.tryAcquire(-1007, 77);
        CHECK(!d.allowed && d.scope == "user" &&
                  contains(d.reason, "user 77"),
              "limiter: user-scope denial names the user: " + d.reason);
    }

    // --- G. a denial deducts nothing -----------------------------------
    {
        parlay::RateLimiter lim(bucketCfg(1, 2.0, 10, 10.0));
        CHECK(lim.tryAcquire(-1008, 88).allowed,
              "limiter: no-deduct test drains the single token");
        parlay::RateLimitDecision d1 = lim.tryAcquire(-1008, 88);
        parlay::RateLimitDecision d2 = lim.tryAcquire(-1008, 88);
        CHECK(!d1.allowed && !d2.allowed &&
                  d2.retry_after_sec <= d1.retry_after_sec + 0.05,
              "limiter: repeated denials do not push retry-after out "
              "(no tokens deducted on denial)");
    }

    // --- H. user_id 0: chat bucket only -------------------------------
    {
        parlay::RateLimiter lim(bucketCfg(1, 0.5, 1, 0.5));
        CHECK(lim.tryAcquire(-1009, 0).allowed,
              "limiter: user_id 0 (no sender) allowed on chat bucket");
        CHECK(!lim.tryAcquire(-1009, 0).allowed,
              "limiter: user_id 0 denied once the chat bucket is dry");
    }

    // --- I. defaults from Config match today's sustained rate ---------
    {
        parlay::Config cfg = parlay::Config::defaults();
        parlay::RateLimiter lim(cfg.rate_limit);
        int allowed = 0;
        // Distinct users per acquire so the user bucket (capacity 3)
        // never binds; the chat bucket (capacity 5) is what is tested.
        for (int i = 0; i < 5; ++i)
            if (lim.tryAcquire(-1010, 99 + i).allowed) ++allowed;
        CHECK(allowed == 5,
              "limiter: default config allows the burst of 5 (chat "
              "capacity 5)");
        CHECK(!lim.tryAcquire(-1010, 200).allowed,
              "limiter: default config denies the 6th (burst spent)");
    }

    // --- J. thread-safety smoke ---------------------------------------
    {
        parlay::RateLimiter lim(bucketCfg(5, 0.01, 100, 0.01));
        std::atomic<int> allowed{0};
        auto worker = [&] {
            for (int i = 0; i < 50; ++i)
                if (lim.tryAcquire(-1011, 111).allowed)
                    allowed.fetch_add(1);
        };
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
        CHECK(allowed.load() == 5,
              "limiter: 8 threads x 50 acquires allow exactly the "
              "burst of 5, got " +
                  std::to_string(allowed.load()));
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
