// telemetry_test.cpp — Phase 1 stage timers (FINAL-ARCHITECTURE.md §9).
//
// - telemetry::Timer / sanitize / emit: unit checks on the log format.
// - OpusPipeline::run (t_first_frame): a locally generated 5 s tone is
//   encoded to Opus with an RTP sink. The encode leg is real (ffmpeg):
//   the first 20 ms Opus packet is timestamped as the first Ogg audio
//   page leaves the encoder. The RTP leg is honestly reported failed
//   in this sandbox (UDP sendto is EPERM, even to loopback — the
//   README's opus proof records the same): rtp_ok=0 with the exact
//   error, and NO fabricated RTP first-packet time.
// - Failure case: a missing source file emits ok=0 with
//   t_first_frame_us=-1 (no packet was ever produced).
//
// No network, no Telegram, no credentials. ffmpeg/ffprobe are the only
// external tools; when they are absent the pipeline checks SKIP (the
// pure telemetry unit checks still run).
//
// Build: g++ -std=c++20 -I src tests/telemetry_test.cpp
//   src/voice_call.cpp src/tdlib_client.cpp src/json.cpp src/config.cpp
//   -o /tmp/telemetry_test -ldl && /tmp/telemetry_test
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
#include "voice_call.hpp"

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

bool hasFfmpeg() {
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0 &&
           std::system("ffprobe -version >/dev/null 2>&1") == 0;
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

    if (!hasFfmpeg()) {
        std::cout << "SKIP OpusPipeline checks: ffmpeg/ffprobe absent\n";
        std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": "
                  << failures << " failed\n";
        return failures == 0 ? 0 : 1;
    }

    const std::string dir = "/tmp/parlay-telemetry-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string tone = dir + "/tone.wav";
    const std::string opus = dir + "/out.opus";

    // --- B. t_first_frame on a real local encode + RTP sink ------------
    {
        int rc = std::system(
            ("ffmpeg -y -v error -f lavfi -i "
             "\"sine=frequency=440:duration=5\" -ar 48000 -ac 2 " +
             tone + " 2>/dev/null")
                .c_str());
        CHECK(rc == 0 && std::filesystem::exists(tone),
              "telemetry fixture: 5 s 440 Hz tone generated");
    }
    {
        parlay::OpusPipeline pipe;
        std::string out;
        parlay::OpusStats st;
        {
            CoutCapture cap;
            // RTP sink is loopback: the sandbox blocks UDP sendto
            // with EPERM even to 127.0.0.1, so the RTP leg must fail
            // honestly while the file encode succeeds.
            st = pipe.run(tone, opus, "127.0.0.1:5004");
            out = cap.str();
        }
        CHECK(st.ok, "t_first_frame: local tone encodes to Opus");
        CHECK(st.frames > 200 && st.bytes > 0,
              "t_first_frame: real Opus frames in the file");
        CHECK(st.duration_sec > 4.9 && st.duration_sec < 5.1,
              "t_first_frame: ffprobe duration is the 5 s tone");
        CHECK(!st.rtp_ok && !st.rtp_error.empty(),
              "t_first_frame: RTP leg honestly fails in the sandbox "
              "(UDP EPERM), file encode unaffected");
        std::string line = telemetryLine(out, "t_first_frame");
        CHECK(!line.empty(), "t_first_frame: line emitted on every run");
        int64_t first = fieldInt(line, "t_first_frame_us");
        int64_t first_pkt = fieldInt(line, "first_packet_us");
        int64_t opened = fieldInt(line, "stream_open_us");
        int64_t total = fieldInt(line, "encode_total_us");
        int64_t rtp = fieldInt(line, "rtp_us");
        CHECK(first > 0 && first == first_pkt,
              "t_first_frame: headline equals first_packet_us");
        CHECK(opened > 0 && opened <= first,
              "t_first_frame: stream open precedes first packet");
        CHECK(total > 0 && first < total,
              "t_first_frame: first packet lands before encode ends");
        CHECK(rtp > 0 && contains(line, "rtp_ok=0"),
              "t_first_frame: RTP leg timed, rtp_ok=0, no fabricated "
              "RTP first-packet time");
        CHECK(contains(line, "rtp_error="),
              "t_first_frame: RTP failure reason on the line");
        CHECK(contains(line, "ok=1"),
              "t_first_frame: ok=1 with the file encode succeeding");
        std::cout << "measured: " << line << "\n";
    }

    // --- C. failure case: missing source -------------------------------
    {
        parlay::OpusPipeline pipe;
        std::string out;
        parlay::OpusStats st;
        {
            CoutCapture cap;
            st = pipe.run(dir + "/no-such-file.wav", dir + "/never.opus",
                          "");
            out = cap.str();
        }
        CHECK(!st.ok && !st.error.empty(),
              "t_first_frame failure: missing source is an honest "
              "error, not a fake encode");
        std::string line = telemetryLine(out, "t_first_frame");
        CHECK(!line.empty() && fieldInt(line, "t_first_frame_us") == -1 &&
                  contains(line, "ok=0") && contains(line, "error="),
              "t_first_frame failure: line emitted with -1 (no packet "
              "produced), ok=0, exact error");
    }

    std::filesystem::remove_all(dir, ec);
    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures
              << " failed\n";
    return failures == 0 ? 0 : 1;
}
