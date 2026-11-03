// rate_limiter.hpp — per-chat AND per-user token-bucket rate limiter.
//
// Replaces the old 1 cmd/sec silent drop (dispatcher.cpp:911
// rateLimited(): a command inside the 1 s window vanished with no
// reply). A token bucket allows a short burst (capacity) while the
// sustained rate is capped by the refill rate, both from config
// ([rate_limit.chat] / [rate_limit.user] in parlay.toml).
//
// A command is allowed only when BOTH its chat bucket and its user
// bucket hold a token; one token is taken from each. A denial takes
// nothing from either bucket and always carries an honest reason plus
// a retry-after in seconds, so the dispatcher can answer the user
// ("Rate limit: try again in 2s") instead of dropping the command
// silently. Thread-safe: all bucket state sits behind one mutex and a
// single tryAcquire() checks and deducts atomically.
//
// user_id == 0 means "no sender known" (system/internal calls): only
// the chat bucket applies.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "config.hpp"

namespace parlay {

struct RateLimitDecision {
    bool allowed = false;
    // Seconds until at least one token is available in the denying
    // bucket (0 when allowed). Always > 0 on a denial.
    double retry_after_sec = 0.0;
    // Human-readable denial reason, e.g. "rate limit exceeded for
    // chat -100123: burst capacity 5 at 1.0/s; retry after 2.0s".
    // Empty when allowed.
    std::string reason;
    // Which bucket denied: "chat", "user", or "" when allowed.
    std::string scope;
};

class RateLimiter {
public:
    explicit RateLimiter(const RateLimitConfig& cfg) : cfg_(cfg) {}

    // Atomically refill both buckets to now, then allow (deduct one
    // token from each) or deny (deduct nothing).
    RateLimitDecision tryAcquire(int64_t chat_id, int64_t user_id);

    // Tokens currently in each bucket (refilled to now). Test and
    // diagnostics introspection only.
    double chatTokens(int64_t chat_id);
    double userTokens(int64_t user_id);

    // Drop all bucket state. Tests only.
    void reset();

private:
    struct Bucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last;
        bool started = false;
    };

    RateLimitConfig cfg_;
    // One mutex for both maps: a single tryAcquire() must refill,
    // check, and deduct both buckets atomically.
    mutable std::mutex mutex_;
    std::unordered_map<int64_t, Bucket> chat_buckets_;
    std::unordered_map<int64_t, Bucket> user_buckets_;

    static void refill(Bucket& b, double capacity, double refill_per_sec,
                       std::chrono::steady_clock::time_point now);
    static std::string reasonFor(const std::string& scope, int64_t id,
                                 double capacity, double refill_per_sec,
                                 double retry_after_sec);
};

}  // namespace parlay
