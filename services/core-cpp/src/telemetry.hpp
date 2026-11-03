// telemetry.hpp — Phase 1 stage timers (FINAL-ARCHITECTURE.md §9).
//
// Additive instrumentation only: microsecond monotonic timers
// (std::chrono::steady_clock) emitted as structured log lines on
// stdout, the same channel as the existing [cmd] / [voice] lines:
//
//   [telemetry] <name>_us=<integer> [key=value ...]
//
// Timer names (exact — the engine workers use the same names):
//   t_dispatch_to_ipc — Dispatcher::handleMessage receipt -> the
//     engine request issued via IpcClient::post, and post issued ->
//     response received, as two fields of one line.
//   t_voice_join — TdLibVoiceCall join stages: authorize ->
//     create_call (local SDP offer) -> get_chat -> create_video_chat
//     (when needed) -> join_video_chat -> connect (remote SDP).
//   t_first_frame — OpusPipeline: stream open -> first 20 ms Opus
//     packet out of the encoder (first Ogg audio page), plus the RTP
//     leg wall time and ok flag.
//
// Honesty rule: a stage that never ran is omitted from the line, a
// failed attempt emits with ok=0, the stage it failed at, and the
// exact failure reason — never a fabricated success or time. A
// *_us value of -1 means "no such packet/stage was produced" (e.g.
// the encode died before its first audio page, or the RTP leg never
// emitted a frame). Totals on a failed attempt are time-to-failure,
// not a successful-stage time.
#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace parlay::telemetry {

using Clock = std::chrono::steady_clock;

// Microseconds since the steady clock's epoch (monotonic, never
// wall-clock: immune to NTP jumps and TZ changes).
inline int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch())
        .count();
}

inline int64_t elapsedUs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - start)
        .count();
}

// One field value, made safe for a space-separated key=value line:
// whitespace runs become '_', '=' becomes ':', truncated to 200
// chars so a long error cannot flood the log.
inline std::string sanitize(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    bool last_ws = false;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_ws) out += '_';
            last_ws = true;
        } else if (c == '=') {
            out += ':';
            last_ws = false;
        } else {
            out += c;
            last_ws = false;
        }
    }
    if (out.size() > 200) out.resize(200);
    return out;
}

// Emit one structured line. Serialized so concurrent callers (the
// HTTP API thread can join while the dispatcher thread plays) never
// interleave mid-line.
inline void emit(
    const std::string& name, int64_t total_us,
    const std::vector<std::pair<std::string, std::string>>& fields) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    std::cout << "[telemetry] " << name << "_us=" << total_us;
    for (const auto& [k, v] : fields) std::cout << ' ' << k << '=' << v;
    std::cout << '\n';
    std::cout.flush();
}

// Accumulating stage timer: record each stage's microseconds as it
// completes, then emit the total plus every stage in order.
class Timer {
public:
    Timer() : start_(Clock::now()) {}
    int64_t totalUs() const { return elapsedUs(start_); }
    // Record a stage that just finished; us is measured by the
    // caller (e.g. a lap around one TDLib call).
    void stage(const std::string& name, int64_t us) {
        stages_.emplace_back(name + "_us", std::to_string(us));
    }
    // Record a stage as "not run" — omitted from the line.
    void emit(const std::string& name,
              const std::vector<std::pair<std::string, std::string>>&
                  extra = {}) const {
        std::vector<std::pair<std::string, std::string>> fields = stages_;
        fields.insert(fields.end(), extra.begin(), extra.end());
        parlay::telemetry::emit(name, totalUs(), fields);
    }

private:
    Clock::time_point start_;
    std::vector<std::pair<std::string, std::string>> stages_;
};

}  // namespace parlay::telemetry
