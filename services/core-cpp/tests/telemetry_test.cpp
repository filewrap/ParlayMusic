// telemetry_test.cpp — Phase 1 stage timers (FINAL-ARCHITECTURE.md §9).
//
// - telemetry::Timer / sanitize / emit: unit checks on the log format.
// (The OpusPipeline / t_first_frame checks lived here in Phase 1;
// they were deleted with voice_call.cpp in Phase 2 — the core
// never touches Opus or ntgcalls any more, and t_first_frame is a
// backend concern.)
//
// No network, no Telegram, no credentials.
//
// Build: g++ -std=c++20 -I src tests/telemetry_test.cpp
//   src/tdlib_client.cpp src/json.cpp src/config.cpp src/toml.cpp
//   -o /tmp/telemetry_test && /tmp/telemetry_test
// Exit 0 = all checks pass.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "telemetry.hpp"

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

// Capture everything written to std::cout between construction and
// destruction (telemetry lines go to std::cout, like [cmd]/[voice]).
class CoutCapture {
public:
    CoutCapture() : old_(std::cout.rdbuf(ss_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return ss_.str(); }

private:
    std::streambuf* old_;
    std::ostringstream ss_;
};

// First "[telemetry] <name>_us=<n> ..." line in captured output.
std::string telemetryLine(const std::string& out, const std::string& name) {
    std::string needle = "[telemetry] " + name + "_us=";
    size_t at = out.find(needle);
    if (at == std::string::npos) return "";
    size_t eol = out.find('\n', at);
    return out.substr(at, eol == std::string::npos ? std::string::npos
                                                   : eol - at);
}

// Integer value of field "key=" inside one telemetry line, or -1.
int64_t fieldInt(const std::string& line, const std::string& key) {
    std::string needle = key + "=";
    size_t at = line.find(needle);
    if (at == std::string::npos) return -1;
    size_t b = at + needle.size();
    size_t e = line.find(' ', b);
    try {
        return std::stoll(line.substr(b, e == std::string::npos
                                             ? std::string::npos
                                             : e - b));
    } catch (...) {
        return -1;
    }
}



}  // namespace

int main() {
    // --- A. telemetry unit checks (no ffmpeg needed) -------------------
    {
        int64_t a = parlay::telemetry::nowUs();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        int64_t b = parlay::telemetry::nowUs();
        CHECK(b > a, "telemetry nowUs is monotonic and advances");
    }
    {
        CHECK(parlay::telemetry::sanitize("a b\tc\nd=e") == "a_b_c_d:e",
              "telemetry sanitize folds whitespace and '='");
        std::string long_err(300, 'x');
        CHECK(parlay::telemetry::sanitize(long_err).size() == 200,
              "telemetry sanitize truncates to 200 chars");
    }
    {
        std::string out;
        {
            CoutCapture cap;
            parlay::telemetry::Timer t;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            t.stage("alpha", 123);
            t.stage("beta", 456);
            t.emit("t_probe", {{"ok", "1"}});
            out = cap.str();
        }
        std::string line = telemetryLine(out, "t_probe");
        CHECK(!line.empty() && contains(line, "alpha_us=123") &&
                  contains(line, "beta_us=456") &&
                  contains(line, "ok=1"),
              "telemetry Timer emits [telemetry] name_us=<total> with "
              "stages in order");
        int64_t total = fieldInt(line, "t_probe_us");
        CHECK(total > 0, "telemetry Timer total is a positive integer");
    }

    // OpusPipeline checks (sections B/C) deleted with
    // voice_call.cpp (Phase 2): the core never touches Opus or
    // ntgcalls any more; t_first_frame is a backend concern.
    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
