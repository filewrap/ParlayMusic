// rate_limiter.cpp — token-bucket limiter. See rate_limiter.hpp.
#include "rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace parlay {

namespace {

// One decimal place, e.g. 2.0 / 0.5 — the retry-after the user sees.
std::string formatSeconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", seconds);
    return buf;
}

}  // namespace

void RateLimiter::refill(Bucket& b, double capacity, double refill_per_sec,
                         std::chrono::steady_clock::time_point now) {
    if (!b.started) {
        b.tokens = capacity;  // a fresh bucket starts full (burst ready)
        b.last = now;
        b.started = true;
        return;
    }
    double elapsed =
        std::chrono::duration<double>(now - b.last).count();
    if (elapsed <= 0.0) return;
    b.tokens =
        std::min(capacity, b.tokens + elapsed * refill_per_sec);
    b.last = now;
}

std::string RateLimiter::reasonFor(const std::string& scope, int64_t id,
                                   double capacity, double refill_per_sec,
                                   double retry_after_sec) {
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.2g", refill_per_sec);
    return "rate limit exceeded for " + scope + " " + std::to_string(id) +
           ": burst capacity " + std::to_string(static_cast<int>(capacity)) +
           " at " + rate + "/s; retry after " +
           formatSeconds(retry_after_sec) + "s";
}

RateLimitDecision RateLimiter::tryAcquire(int64_t chat_id, int64_t user_id) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    Bucket& chat = chat_buckets_[chat_id];
    refill(chat, static_cast<double>(cfg_.chat.capacity),
           cfg_.chat.refill_rate_per_sec, now);

    Bucket* user = nullptr;
    if (user_id != 0) {
        user = &user_buckets_[user_id];
        refill(*user, static_cast<double>(cfg_.user.capacity),
               cfg_.user.refill_rate_per_sec, now);
    }

    RateLimitDecision denied;
    denied.allowed = false;
    if (chat.tokens < 1.0) {
        denied.scope = "chat";
        denied.retry_after_sec =
            (1.0 - chat.tokens) / cfg_.chat.refill_rate_per_sec;
        denied.reason = reasonFor("chat", chat_id,
                                  static_cast<double>(cfg_.chat.capacity),
                                  cfg_.chat.refill_rate_per_sec,
                                  denied.retry_after_sec);
        return denied;
    }
    if (user != nullptr && user->tokens < 1.0) {
        denied.scope = "user";
        denied.retry_after_sec =
            (1.0 - user->tokens) / cfg_.user.refill_rate_per_sec;
        denied.reason = reasonFor("user", user_id,
                                  static_cast<double>(cfg_.user.capacity),
                                  cfg_.user.refill_rate_per_sec,
                                  denied.retry_after_sec);
        return denied;
    }

    // Both buckets hold a token: deduct from both, allow. A denial
    // above deducted nothing.
    chat.tokens -= 1.0;
    if (user != nullptr) user->tokens -= 1.0;
    RateLimitDecision ok;
    ok.allowed = true;
    ok.retry_after_sec = 0.0;
    return ok;
}

double RateLimiter::chatTokens(int64_t chat_id) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    Bucket& b = chat_buckets_[chat_id];
    refill(b, static_cast<double>(cfg_.chat.capacity),
           cfg_.chat.refill_rate_per_sec, now);
    return b.tokens;
}

double RateLimiter::userTokens(int64_t user_id) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    Bucket& b = user_buckets_[user_id];
    refill(b, static_cast<double>(cfg_.user.capacity),
           cfg_.user.refill_rate_per_sec, now);
    return b.tokens;
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    chat_buckets_.clear();
    user_buckets_.clear();
}

}  // namespace parlay
