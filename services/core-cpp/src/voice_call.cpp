// voice_call.cpp
#include "voice_call.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "ntgcalls.h"
#include "telemetry.hpp"

namespace parlay {
namespace {

// POSIX single-quote a string so it is one inert shell word (NoxBeats
// media_common.cpp::shell_quote).
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string runCapture(const std::string& cmd, int* exit_code = nullptr) {
    std::array<char, 4096> buf{};
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        if (exit_code) *exit_code = -1;
        return "";
    }
    while (size_t n = fread(buf.data(), 1, buf.size(), p))
        out.append(buf.data(), n);
    int rc = pclose(p);
    if (exit_code) *exit_code = WEXITSTATUS(rc);
    return out;
}

// Phase 1 telemetry: run an ffmpeg command, teeing its stdout into a
// file (empty path = discard), and timestamp the first Ogg audio page
// leaving the encoder — the first 20 ms Opus packet. Ogg pages are
// parsed incrementally as bytes arrive: "OggS" + 22-byte fixed header
// + segment table + data. Header (BOS) pages carry granule_position 0;
// the first page with granule_position > 0 is the first audio page
// (granule = end sample of its last packet, 960 per 20 ms frame at
// 48 kHz). Returns the child exit code (-1 when the spawn itself
// failed); first_audio_us is -1 when no audio page ever arrived
// (encode died first), first_byte_us -1 when nothing arrived at all.
// Times are microseconds from `origin` (the caller's run start, so
// they line up with the caller's own stage totals).
struct FfmpegPipeResult {
    int exit_code = -1;
    int64_t first_byte_us = -1;
    int64_t first_audio_us = -1;
};

// Parse complete Ogg pages out of `buf`, consuming them. Returns the
// granule_position of the first audio page found, -1 when no audio
// page is complete yet, or -2 when the stream is not Ogg at all (stop
// parsing). `consumed` is set to the bytes parsed (a trailing partial
// page stays buffered).
int64_t scanOggAudioPage(std::string& buf, size_t& consumed) {
    consumed = 0;
    size_t i = 0;
    while (i + 27 <= buf.size()) {
        if (buf.compare(i, 4, "OggS") != 0) return -2;  // not Ogg
        unsigned nseg = static_cast<unsigned char>(buf[i + 26]);
        if (i + 27 + nseg > buf.size()) break;  // partial segment table
        size_t dlen = 0;
        for (size_t s = 0; s < nseg; ++s)
            dlen += static_cast<unsigned char>(buf[i + 27 + s]);
        if (i + 27 + nseg + dlen > buf.size()) break;  // partial page
        int64_t granule = 0;
        for (int b = 0; b < 8; ++b)
            granule |= static_cast<int64_t>(
                           static_cast<unsigned char>(buf[i + 6 + b]))
                       << (8 * b);
        i += 27 + nseg + dlen;
        consumed = i;
        if (granule > 0) return granule;
    }
    return -1;
}

FfmpegPipeResult runFfmpegPipe(const std::string& cmd,
                               const std::string& tee_path,
                               std::chrono::steady_clock::time_point origin) {
    FfmpegPipeResult res;
    int pipefd[2];
    if (pipe(pipefd) != 0) return res;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return res;
    }
    if (pid == 0) {
        // Child: stdout -> pipe, stderr inherited (the old
        // runCapture encode leg did not suppress ffmpeg errors),
        // then the shell command. _exit on exec failure.
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    std::ofstream tee;
    if (!tee_path.empty()) {
        tee.open(tee_path, std::ios::binary | std::ios::trunc);
    }
    std::string buf;
    bool parse_ogg = true;  // false once the first audio page is found
                            // or the stream proves not to be Ogg
    std::array<char, 65536> chunk{};
    while (true) {
        ssize_t n = read(pipefd[0], chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;  // EOF: child closed stdout (exiting)
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - origin)
                          .count();
        if (res.first_byte_us < 0) res.first_byte_us = now_us;
        if (tee.is_open()) tee.write(chunk.data(), n);
        if (!parse_ogg) continue;
        buf.append(chunk.data(), static_cast<size_t>(n));
        size_t consumed = 0;
        int64_t granule = scanOggAudioPage(buf, consumed);
        if (consumed > 0) buf.erase(0, consumed);
        if (granule > 0) {
            res.first_audio_us = now_us;
            parse_ogg = false;
        } else if (granule == -2) {
            parse_ogg = false;  // not an Ogg stream: nothing to time
        }
    }
    close(pipefd[0]);
    if (tee.is_open()) tee.close();
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    res.exit_code =
        WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

}  // namespace

std::string buildFfmpegCmd(const std::string& src, int seek_sec, double speed,
                           int bass, bool video) {
    if (src.empty()) throw std::invalid_argument("empty audio source");
    std::string cmd = "ffmpeg ";
    if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0)
        cmd += "-reconnect 1 -reconnect_at_eof 1 -reconnect_streamed 1 "
               "-reconnect_delay_max 2 ";
    if (seek_sec > 0) cmd += "-ss " + std::to_string(seek_sec) + " ";
    cmd += "-v quiet -i " + shellQuote(src);
    if (video) {
        // Video track for ntgcalls camera input (NoxBeats
        // build_ffmpeg_video_cmd shape): 1280x720 yuv420p 30 fps.
        char pts[32];
        std::snprintf(pts, sizeof pts, "%.6f",
                      speed > 0 ? 1.0 / speed : 1.0);
        cmd += " -an -map 0:v:0 -vf \"setpts=" + std::string(pts) +
               "*PTS,scale=1280:720:force_original_aspect_ratio=decrease,"
               "pad=1280:720:(ow-iw)/2:(oh-ih)/2\" -f rawvideo -pix_fmt "
               "yuv420p -s 1280x720 -r 30 pipe:1";
        return cmd;
    }
    cmd += " -vn";
    if (speed != 1.0 || bass > 0) {
        cmd += " -af \"";
        bool need_comma = false;
        if (speed != 1.0) {
            char b[32];
            std::snprintf(b, sizeof b, "atempo=%.3f", speed);
            cmd += b;
            need_comma = true;
        }
        if (bass > 0) {
            // Low-frequency boost: mix the lowpassed signal back in.
            if (need_comma) cmd += ",";
            cmd += "lowpass=f=150,volume=" +
                   std::to_string(1.0 + bass / 100.0);
        }
        cmd += "\"";
    }
    // NoxBeats uses 96 kHz stereo for ntgcalls; TinaMusic uses 48 kHz.
    // The ntgcalls C API takes the rate from the description struct, so
    // the shell command just has to match the rate the caller declares.
    cmd += " -f s16le -ac 2 -ar 48000 pipe:1";
    return cmd;
}

// --- NtgCallsVoiceCall ------------------------------------------------------
// ntgcalls 3.0.5 C API (generated header ntgcalls.h, from the C build in
// refs/ntgcalls/shared-output/include/): SYNCHRONOUS. No ntg_async_struct
// any more — every call returns ntg_result directly (NTG_OK = 0) and the
// error text comes from ntg_last_error(). The instance is an opaque
// ntg_instance* from ntg_instance_create(). Out strings (create_call
// offer) are freed with ntg_string_free().
namespace {
typedef ntg_instance* (*fn_ntg_instance_create)();
typedef void (*fn_ntg_instance_destroy)(ntg_instance*);
typedef ntg_result (*fn_ntg_create_call)(ntg_instance*, int64_t, char**);
typedef ntg_result (*fn_ntg_connect)(ntg_instance*, int64_t, const char*,
                                     bool);
typedef ntg_result (*fn_ntg_set_stream_sources)(ntg_instance*, int64_t,
                                                ntg_stream_mode,
                                                ntg_media_description);
typedef ntg_result (*fn_ntg_pause)(ntg_instance*, int64_t, bool*);
typedef ntg_result (*fn_ntg_resume)(ntg_instance*, int64_t, bool*);
typedef ntg_result (*fn_ntg_mute)(ntg_instance*, int64_t, bool*);
typedef ntg_result (*fn_ntg_unmute)(ntg_instance*, int64_t, bool*);
typedef ntg_result (*fn_ntg_stop)(ntg_instance*, int64_t);
typedef ntg_result (*fn_ntg_time)(ntg_instance*, int64_t, ntg_stream_mode,
                                  uint64_t*);
typedef const char* (*fn_ntg_get_version)();
typedef const char* (*fn_ntg_last_error)();
typedef void (*fn_ntg_string_free)(char*);
typedef ntg_result (*fn_ntg_on_stream_end)(ntg_instance*,
                                           ntg_stream_end_callback_cb,
                                           void*);
typedef ntg_result (*fn_ntg_on_connection_change)(
    ntg_instance*, ntg_connection_change_callback_cb, void*);

struct NtgApi {
    fn_ntg_instance_create instance_create = nullptr;
    fn_ntg_instance_destroy instance_destroy = nullptr;
    fn_ntg_create_call create_call = nullptr;
    fn_ntg_connect connect = nullptr;
    fn_ntg_set_stream_sources set_stream_sources = nullptr;
    fn_ntg_pause pause = nullptr;
    fn_ntg_resume resume = nullptr;
    fn_ntg_mute mute = nullptr;
    fn_ntg_unmute unmute = nullptr;
    fn_ntg_stop stop = nullptr;
    fn_ntg_time time = nullptr;
    fn_ntg_get_version get_version = nullptr;
    fn_ntg_last_error last_error = nullptr;
    fn_ntg_string_free string_free = nullptr;
    fn_ntg_on_stream_end on_stream_end = nullptr;
    fn_ntg_on_connection_change on_connection_change = nullptr;
};

NtgApi g_ntg{};

// Format a failed ntg_result with the library's own error text.
std::string ntgError(const char* what, ntg_result rc) {
    std::string msg = std::string("ntgcalls ") + what + " failed (" +
                      std::to_string(static_cast<int>(rc)) + ")";
    if (g_ntg.last_error) {
        const char* e = g_ntg.last_error();
        if (e && *e) msg += ": " + std::string(e);
    }
    return msg;
}

// Phase 1 telemetry: one t_voice_join line for a voice-join attempt.
// `stages` are (name, us) in execution order; stages that never ran
// are omitted by the caller. On failure the total is time-to-failure,
// not a join time — the line always carries ok=0, the stage it
// failed at, and the exact reason, never a fabricated join.
void emitVoiceJoin(
    int64_t chat_id, const std::string& backend, int64_t total_us,
    const std::vector<std::pair<std::string, int64_t>>& stages, bool ok,
    const std::string& failed_stage, const std::string& error) {
    std::vector<std::pair<std::string, std::string>> f;
    for (const auto& [name, us] : stages)
        f.emplace_back(name + "_us", std::to_string(us));
    f.emplace_back("chat_id", std::to_string(chat_id));
    f.emplace_back("backend", backend);
    f.emplace_back("ok", ok ? "1" : "0");
    if (!ok) {
        f.emplace_back("failed_stage", failed_stage);
        f.emplace_back("error", telemetry::sanitize(error));
    }
    telemetry::emit("t_voice_join", total_us, f);
}

int64_t stageUs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}
}  // namespace

NtgCallsVoiceCall::NtgCallsVoiceCall(const std::string& explicit_path) {
    std::vector<std::string> candidates;
    if (!explicit_path.empty()) candidates.push_back(explicit_path);
    // Release layout first: lib/ next to the binary, and lib/ next
    // to bin/ (parlay-vX.Y.Z/{bin/parlay-core, lib/libntgcalls.so}).
    // A proper host needs nothing but the session dir + the built
    // .so in lib/ — no NTGCALLS_LIB, no repo paths.
    std::string exe = exeDir();
    if (!exe.empty()) {
        candidates.push_back(exe + "/lib/libntgcalls.so");
        candidates.push_back(exe + "/../lib/libntgcalls.so");
    }
    candidates.push_back("./lib/libntgcalls.so");
    candidates.push_back("../../refs/ntgcalls/shared-output/lib/libntgcalls.so");  // repo build output, when run from services/core-cpp
    candidates.push_back("/usr/local/lib/libntgcalls.so");
    candidates.push_back("/usr/lib/libntgcalls.so");
    candidates.push_back("/usr/lib/x86_64-linux-gnu/libntgcalls.so");
    candidates.push_back("libntgcalls.so");
    for (const auto& path : candidates) {
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle_) {
            lib_path_ = path;
            break;
        }
    }
    if (!handle_) {
        last_error_ = std::string("libntgcalls.so not found (dlopen: ") +
                      dlerror() + ")";
        return;
    }
    auto sym = [&](const char* name) {
        return dlsym(handle_, name);
    };
    g_ntg.instance_create = reinterpret_cast<fn_ntg_instance_create>(
        sym("ntg_instance_create"));
    g_ntg.instance_destroy = reinterpret_cast<fn_ntg_instance_destroy>(
        sym("ntg_instance_destroy"));
    g_ntg.create_call =
        reinterpret_cast<fn_ntg_create_call>(sym("ntg_create_call"));
    g_ntg.connect = reinterpret_cast<fn_ntg_connect>(sym("ntg_connect"));
    g_ntg.set_stream_sources = reinterpret_cast<fn_ntg_set_stream_sources>(
        sym("ntg_set_stream_sources"));
    g_ntg.pause = reinterpret_cast<fn_ntg_pause>(sym("ntg_pause"));
    g_ntg.resume = reinterpret_cast<fn_ntg_resume>(sym("ntg_resume"));
    g_ntg.mute = reinterpret_cast<fn_ntg_mute>(sym("ntg_mute"));
    g_ntg.unmute = reinterpret_cast<fn_ntg_unmute>(sym("ntg_unmute"));
    g_ntg.stop = reinterpret_cast<fn_ntg_stop>(sym("ntg_stop"));
    g_ntg.time = reinterpret_cast<fn_ntg_time>(sym("ntg_time"));
    g_ntg.get_version =
        reinterpret_cast<fn_ntg_get_version>(sym("ntg_get_version"));
    g_ntg.last_error =
        reinterpret_cast<fn_ntg_last_error>(sym("ntg_last_error"));
    g_ntg.string_free =
        reinterpret_cast<fn_ntg_string_free>(sym("ntg_string_free"));
    g_ntg.on_stream_end = reinterpret_cast<fn_ntg_on_stream_end>(
        sym("ntg_on_stream_end_callback"));
    g_ntg.on_connection_change = reinterpret_cast<fn_ntg_on_connection_change>(
        sym("ntg_on_connection_change_callback"));
    if (!g_ntg.instance_create || !g_ntg.create_call || !g_ntg.connect) {
        last_error_ = "libntgcalls.so at " + lib_path_ +
                      " lacks the ntg_instance_create/create_call/connect "
                      "symbols (wrong ntgcalls version?)";
        dlclose(handle_);
        handle_ = nullptr;
        return;
    }
    ntg_ = g_ntg.instance_create();
    if (!ntg_) {
        last_error_ = "ntg_instance_create() failed";
        dlclose(handle_);
        handle_ = nullptr;
    }
}

NtgCallsVoiceCall::~NtgCallsVoiceCall() {
    if (ntg_ && g_ntg.instance_destroy) g_ntg.instance_destroy(ntg_);
    if (handle_) dlclose(handle_);
}

std::string NtgCallsVoiceCall::describe() const {
    if (!handle_) return "ntgcalls unavailable: " + last_error_;
    std::string v;
    if (g_ntg.get_version) {
        const char* ver = g_ntg.get_version();
        if (ver) v = std::string(" ") + ver;
    }
    return "ntgcalls via " + lib_path_ + v;
}

bool NtgCallsVoiceCall::createOffer(int64_t chat_id,
                                    std::string& offer_out) {
    if (!available()) return false;
    char* buf = nullptr;
    ntg_result rc = g_ntg.create_call(ntg_, chat_id, &buf);
    if (rc != NTG_OK) {
        last_error_ = ntgError("create_call", rc);
        return false;
    }
    offer_out = buf ? buf : "";
    if (g_ntg.string_free && buf) g_ntg.string_free(buf);
    if (offer_out.empty()) {
        last_error_ = "ntg_create_call returned no params";
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::setSources(int64_t chat_id,
                                   const std::string& stream_url, bool video,
                                   int seek_sec, double speed, int bass) {
    if (!available()) return false;
    std::string cmd = buildFfmpegCmd(stream_url, seek_sec, speed, bass);
    std::string video_cmd;
    if (video) video_cmd = buildFfmpegCmd(stream_url, seek_sec, speed, 0,
                                          true);
    // New header field order: media_source, sample_rate, channel_count,
    // input, keep_open. SHELL = NTG_MEDIA_SOURCE_SHELL (2).
    ntg_audio_description a{};
    a.media_source = NTG_MEDIA_SOURCE_SHELL;
    a.sample_rate = 48000;
    a.channel_count = 2;
    a.input = const_cast<char*>(cmd.c_str());
    a.keep_open = false;
    ntg_video_description v{};
    v.media_source = NTG_MEDIA_SOURCE_SHELL;
    v.width = 1280;
    v.height = 720;
    v.fps = 30;
    v.input = const_cast<char*>(video_cmd.c_str());
    v.keep_open = false;
    ntg_media_description d{};
    d.microphone = &a;
    if (!video_cmd.empty()) d.camera = &v;
    ntg_result rc = g_ntg.set_stream_sources(ntg_, chat_id,
                                             NTG_STREAM_MODE_CAPTURE, d);
    if (rc != NTG_OK) {
        last_error_ = ntgError("set_stream_sources", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::join(int64_t chat_id) {
    // This backend has no MTProto session: it can build the local SDP
    // offer (createOffer) and attach sources (setSources), but the
    // join itself — phone.joinGroupCall — needs TDLib. Say so with
    // the exact reason instead of the base class's silent false.
    // Phase 1 telemetry: the attempt is still timed and emitted
    // (ok=0, failed_stage=join) — a blocked join is data, not silence.
    auto t0 = std::chrono::steady_clock::now();
    last_error_ =
        "no MTProto session: this backend can build a local SDP offer "
        "and attach sources, but only the tdlib backend can join a "
        "Telegram voice chat (TD_API_ID/TD_API_HASH/TD_SESSION_DIR + "
        "libtdjson.so)";
    emitVoiceJoin(chat_id, "ntgcalls-dlopen", stageUs(t0), {}, false,
                  "join", last_error_);
    return false;
}

bool NtgCallsVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                             const std::string&, bool video, int seek_sec,
                             double speed, int bass) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, int64_t>> stages;
    if (!available()) {
        // last_error_ already holds the constructor's dlopen reason.
        emitVoiceJoin(chat_id, "ntgcalls-dlopen", stageUs(t0), stages,
                      false, "available", last_error_);
        return false;
    }
    // TinaMusic Play() step 1-2: create the call (local SDP offer),
    // attach the ffmpeg shell command as the audio source.
    std::string offer;
    {
        auto ts = std::chrono::steady_clock::now();
        if (!createOffer(chat_id, offer)) {
            stages.emplace_back("create_call", stageUs(ts));
            emitVoiceJoin(chat_id, "ntgcalls-dlopen", stageUs(t0), stages,
                          false, "create_call", last_error_);
            return false;
        }
        stages.emplace_back("create_call", stageUs(ts));
    }
    if (!setSources(chat_id, stream_url, video, seek_sec, speed, bass)) {
        emitVoiceJoin(chat_id, "ntgcalls-dlopen", stageUs(t0), stages,
                      false, "set_sources", last_error_);
        return false;
    }
    // Step 3 (TinaMusic): the caller must now phone.joinGroupCall with
    // this offer via MTProto and call connect() with the remote
    // transport. Without an MTProto session this backend can create
    // the offer but cannot join — report that honestly.
    last_error_ =
        "local SDP offer created (" + std::to_string(offer.size()) +
        " bytes) but no MTProto join happened: call connect() with the "
        "remote transport from joinVideoChat, or use the tdlib "
        "backend which does the join in-process";
    emitVoiceJoin(chat_id, "ntgcalls-dlopen", stageUs(t0), stages, false,
                  "join", last_error_);
    return false;
}

bool NtgCallsVoiceCall::connect(int64_t chat_id,
                                const std::string& remote_params) {
    if (!available()) return false;
    ntg_result rc = g_ntg.connect(ntg_, chat_id, remote_params.c_str(), false);
    if (rc != NTG_OK) {
        last_error_ = ntgError("connect", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::stop(int64_t chat_id) {
    if (!available()) return false;
    ntg_result rc = g_ntg.stop(ntg_, chat_id);
    if (rc != NTG_OK) {
        last_error_ = ntgError("stop", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::pause(int64_t chat_id) {
    if (!available()) return false;
    bool ok = false;
    ntg_result rc = g_ntg.pause(ntg_, chat_id, &ok);
    if (rc != NTG_OK) {
        last_error_ = ntgError("pause", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::resume(int64_t chat_id) {
    if (!available()) return false;
    bool ok = false;
    ntg_result rc = g_ntg.resume(ntg_, chat_id, &ok);
    if (rc != NTG_OK) {
        last_error_ = ntgError("resume", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::mute(int64_t chat_id) {
    if (!available()) return false;
    bool ok = false;
    ntg_result rc = g_ntg.mute(ntg_, chat_id, &ok);
    if (rc != NTG_OK) {
        last_error_ = ntgError("mute", rc);
        return false;
    }
    return true;
}

bool NtgCallsVoiceCall::unmute(int64_t chat_id) {
    if (!available()) return false;
    bool ok = false;
    ntg_result rc = g_ntg.unmute(ntg_, chat_id, &ok);
    if (rc != NTG_OK) {
        last_error_ = ntgError("unmute", rc);
        return false;
    }
    return true;
}

int64_t NtgCallsVoiceCall::time(int64_t chat_id) {
    if (!available() || !g_ntg.time) return -1;
    uint64_t t = 0;
    ntg_result rc = g_ntg.time(ntg_, chat_id, NTG_STREAM_MODE_CAPTURE, &t);
    if (rc != NTG_OK) return -1;
    return static_cast<int64_t>(t);
}

// --- OpusPipeline ------------------------------------------------------------
std::string OpusPipeline::resolveUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return url;  // local file: nothing to resolve
    // yt-dlp -g: print the direct audio URL (best audio only).
    int rc = 0;
    std::string out = runCapture(
        "yt-dlp -g -f 'bestaudio/best' --no-playlist " + shellQuote(url) +
            " 2>/dev/null",
        &rc);
    if (rc != 0) return "";
    // First non-empty line is the audio URL.
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) return line;
    }
    return "";
}

OpusStats OpusPipeline::run(const std::string& source,
                            const std::string& opus_out,
                            const std::string& rtp_sink) {
    // Phase 1 telemetry (t_first_frame): every run emits one line,
    // success or failure, with the stages reached. Headline value =
    // stream open -> first 20 ms Opus packet out of the encoder
    // (first Ogg audio page on the encode leg's stdout pipe). -1
    // means the stage never produced (encode died first). The RTP
    // leg reports its own wall time + ok flag; the sender cannot
    // observe a UDP packet on the wire, so no RTP first-packet time
    // is fabricated — in production with working UDP the first RTP
    // packet leaves ~first_packet_us after the RTP leg spawns (same
    // encoder, same input, same settings as the encode leg).
    auto t0 = std::chrono::steady_clock::now();
    auto us_since = [&]() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    };
    OpusStats st;
    int64_t resolve_us = -1;        // yt-dlp -g, URL inputs only
    int64_t stream_open_us = -1;    // origin -> first output byte
    int64_t first_packet_us = -1;   // origin -> first Ogg audio page
    int64_t encode_total_us = -1;   // origin -> encode process exit
    int64_t rtp_us = -1;            // RTP leg wall time
    auto emit_line = [&]() {
        std::vector<std::pair<std::string, std::string>> f;
        if (resolve_us >= 0)
            f.emplace_back("resolve_us", std::to_string(resolve_us));
        if (stream_open_us >= 0)
            f.emplace_back("stream_open_us",
                           std::to_string(stream_open_us));
        if (first_packet_us >= 0)
            f.emplace_back("first_packet_us",
                           std::to_string(first_packet_us));
        if (encode_total_us >= 0)
            f.emplace_back("encode_total_us",
                           std::to_string(encode_total_us));
        if (rtp_us >= 0) {
            f.emplace_back("rtp_us", std::to_string(rtp_us));
            f.emplace_back("rtp_ok", st.rtp_ok ? "1" : "0");
        }
        f.emplace_back("frames", std::to_string(st.frames));
        f.emplace_back("bytes", std::to_string(st.bytes));
        f.emplace_back("ok", st.ok ? "1" : "0");
        if (!st.error.empty())
            f.emplace_back("error", telemetry::sanitize(st.error));
        else if (!st.rtp_error.empty())
            f.emplace_back("rtp_error", telemetry::sanitize(st.rtp_error));
        telemetry::emit("t_first_frame", first_packet_us, f);
    };

    std::string input = source;
    if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        auto t_resolve = std::chrono::steady_clock::now();
        st.resolved_url = resolveUrl(source);
        resolve_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t_resolve)
                         .count();
        if (!st.resolved_url.empty()) input = st.resolved_url;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(opus_out).parent_path(), ec);
    // Encode leg: same ffmpeg invocation as before, but the Ogg
    // output goes to a pipe (teed into opus_out) so the first audio
    // page — the first 20 ms Opus packet — can be timestamped as it
    // leaves the encoder. The file bytes are identical.
    std::string cmd = "ffmpeg -y -v error -i " + shellQuote(input) +
                      " -map 0:a -c:a libopus -b:a 128k -ar 48000 -ac 2 -f "
                      "opus pipe:1";
    FfmpegPipeResult enc = runFfmpegPipe(cmd, opus_out, t0);
    encode_total_us = us_since();
    stream_open_us = enc.first_byte_us;
    first_packet_us = enc.first_audio_us;
    if (enc.exit_code != 0 || !std::filesystem::exists(opus_out)) {
        st.error = "ffmpeg opus encode failed (exit " +
                   std::to_string(enc.exit_code) + ")";
        emit_line();
        return st;
    }
    if (!rtp_sink.empty()) {
        // Second output: real RTP packets to the UDP sink (ffmpeg writes
        // the SDP to stdout; we discard it — the receiver only needs the
        // packets). Accept "host:port" or a full "rtp://host:port" sink.
        // Run separately so a blocked UDP sink cannot lose the file encode.
        std::string sink = rtp_sink;
        const std::string prefix = "rtp://";
        if (sink.rfind(prefix, 0) == 0) sink = sink.substr(prefix.size());
        std::string rtp_cmd =
            "ffmpeg -y -v error -i " + shellQuote(input) +
            " -map 0:a -c:a libopus -b:a 128k -ar 48000 -ac 2 -f rtp rtp://" +
            sink + " 2>/dev/null";
        auto t_rtp = std::chrono::steady_clock::now();
        int rtp_rc = 0;
        runCapture(rtp_cmd, &rtp_rc);
        rtp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t_rtp)
                     .count();
        st.rtp_ok = (rtp_rc == 0);
        if (!st.rtp_ok)
            st.rtp_error = "ffmpeg rtp output failed (exit " +
                           std::to_string(rtp_rc) +
                           ") — UDP may be blocked in this sandbox";
    }
    st.bytes = static_cast<int64_t>(std::filesystem::file_size(opus_out, ec));
    // Verify with ffprobe: packet count + duration (proves real Opus
    // frames, not an empty container).
    std::string frames = runCapture(
        "ffprobe -v error -select_streams a:0 -show_entries "
        "packet=pts_time -of csv=p=0 " +
        shellQuote(opus_out) + " 2>/dev/null | wc -l");
    try {
        st.frames = std::stoi(frames);
    } catch (...) {
        st.frames = 0;
    }
    std::string dur = runCapture(
        "ffprobe -v error -show_entries format=duration -of "
        "csv=p=0 " +
        shellQuote(opus_out) + " 2>/dev/null");
    try {
        st.duration_sec = std::stod(dur);
    } catch (...) {
        st.duration_sec = 0;
    }
    st.ok = st.frames > 0 && st.bytes > 0;
    if (!st.ok) st.error = "opus file has no frames";
    emit_line();
    return st;
}

// --- TdLibVoiceCall ----------------------------------------------------------
// TDLib (MTProto signaling) + ntgcalls (WebRTC). TDLib does NOT do audio
// itself: it authorizes, resolves the chat's video chat, and performs
// joinVideoChat (the phone.joinGroupCall equivalent from TinaMusic's
// mtproto.cpp); the returned Text.text is the remote transport payload
// fed to ntgcalls connect(), exactly like the voice bridge does with
// Telethon.
TdLibVoiceCall::TdLibVoiceCall(const Config& cfg)
    : cfg_(cfg),
      tdlib_(std::make_unique<TdLibClient>(cfg.tdlib_lib)),
      ntg_(cfg.ntgcalls_lib) {}

TdLibVoiceCall::TdLibVoiceCall(const Config& cfg,
                               std::unique_ptr<TdLibClient> tdlib)
    : cfg_(cfg), tdlib_(std::move(tdlib)), ntg_(cfg.ntgcalls_lib) {}

bool TdLibVoiceCall::available() const {
    return tdlib_->available() && cfg_.td_api_id != 0 &&
           !cfg_.td_api_hash.empty() && !cfg_.td_session_dir.empty() &&
           ntg_.available();
}

std::string TdLibVoiceCall::describe() const {
    if (!tdlib_->available())
        return "tdlib unavailable: " + tdlib_->lastError();
    if (cfg_.td_api_id == 0 || cfg_.td_api_hash.empty() ||
        cfg_.td_session_dir.empty())
        return "tdlib unavailable: TD_API_ID/TD_API_HASH/TD_SESSION_DIR "
               "not all set";
    if (!ntg_.available())
        return "tdlib unavailable: ntgcalls: " + ntg_.lastError();
    return "tdlib via " + tdlib_->libPath() + " + ntgcalls via " +
           ntg_.describe();
}

int32_t TdLibVoiceCall::groupCallId(int64_t chat_id) {
    Json req = Json::objectOf({
        {"@type", Json("getChat")},
        {"chat_id", Json(chat_id)},
    });
    Json resp = tdlib_->call(req, 30.0);
    if (resp.isNull()) {
        last_error_ = "getChat timed out for chat " +
                      std::to_string(chat_id);
        return -1;
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        last_error_ = "getChat failed: " +
                      (msg ? msg->asString() : std::string("?"));
        return -1;
    }
    const Json* vc = resp.find("video_chat");
    if (!vc) {
        last_error_ = "getChat response has no video_chat";
        return -1;
    }
    const Json* gcid = vc->find("group_call_id");
    return gcid ? static_cast<int32_t>(gcid->asInt(0)) : 0;
}

int32_t TdLibVoiceCall::createVideoChat(int64_t chat_id,
                                        const std::string& title) {
    Json req = Json::objectOf({
        {"@type", Json("createVideoChat")},
        {"chat_id", Json(chat_id)},
        {"title", Json(title)},
        {"start_date", Json(0)},
        {"is_rtmp_stream", Json(false)},
    });
    Json resp = tdlib_->call(req, 30.0);
    if (resp.isNull()) {
        last_error_ = "createVideoChat timed out for chat " +
                      std::to_string(chat_id);
        return 0;
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        last_error_ = "createVideoChat failed: " +
                      (msg ? msg->asString() : std::string("?"));
        return 0;
    }
    const Json* id = resp.find("id");
    return id ? static_cast<int32_t>(id->asInt(0)) : 0;
}

std::string TdLibVoiceCall::joinVideoChat(int32_t group_call_id,
                                          const std::string& local_offer,
                                          bool video) {
    Json join_params = Json::objectOf({
        {"@type", Json("groupCallJoinParameters")},
        {"audio_source_id", Json(0)},
        {"payload", Json(local_offer)},
        {"is_muted", Json(false)},
        {"is_my_video_enabled", Json(video)},
    });
    Json req = Json::objectOf({
        {"@type", Json("joinVideoChat")},
        {"group_call_id", Json(static_cast<int64_t>(group_call_id))},
        {"participant_id", Json(nullptr)},
        {"join_parameters", join_params},
        {"invite_hash", Json("")},
    });
    Json resp = tdlib_->call(req, 30.0);
    if (resp.isNull()) {
        last_error_ = "joinVideoChat timed out for group call " +
                      std::to_string(group_call_id);
        return "";
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        last_error_ = "joinVideoChat failed: " +
                      (msg ? msg->asString() : std::string("?"));
        return "";
    }
    // joinVideoChat returns Text; .text is the remote transport payload
    // for tgcalls (td_api.tl: "Returns join response payload for tgcalls").
    const Json* text = resp.find("text");
    if (!text || text->asString().empty()) {
        last_error_ =
            "joinVideoChat returned no remote payload (type: " +
            (type ? type->asString() : std::string("?")) + ")";
        return "";
    }
    return text->asString();
}

// Phase 1 telemetry: the join sequence shared by join() and play()
// — authorize -> ntgcalls create_call (local SDP offer) ->
// getChat / createVideoChat -> joinVideoChat -> ntg.connect — timed
// stage by stage and emitted as one t_voice_join line on every
// attempt, success or failure. On failure the line carries the
// stages reached, ok=0, the failing stage, and the exact reason;
// the total is time-to-failure, never a fabricated join time.
bool TdLibVoiceCall::joinCall(int64_t chat_id, const std::string& title,
                              bool video) {
    auto t0 = std::chrono::steady_clock::now();
    auto us_since = [&]() { return stageUs(t0); };
    std::vector<std::pair<std::string, int64_t>> stages;
    auto fail = [&](const std::string& stage, const std::string& err) {
        emitVoiceJoin(chat_id, "tdlib", us_since(), stages, false, stage,
                      err);
        return false;
    };
    if (!available()) {
        last_error_ = describe();
        return fail("available", last_error_);
    }
    // 1. Authorize the TDLib session (honest stop without credentials).
    {
        auto ts = std::chrono::steady_clock::now();
        if (!tdlib_->authorized() && !tdlib_->authorize(60.0)) {
            last_error_ = tdlib_->lastError();
            stages.emplace_back("authorize", stageUs(ts));
            return fail("authorize", last_error_);
        }
        stages.emplace_back("authorize", stageUs(ts));
    }
    // 2. ntgcalls create_call -> local SDP offer.
    std::string offer;
    {
        auto ts = std::chrono::steady_clock::now();
        if (!ntg_.createOffer(chat_id, offer)) {
            last_error_ = "ntgcalls create failed: " + ntg_.lastError();
            stages.emplace_back("create_call", stageUs(ts));
            return fail("create_call", last_error_);
        }
        stages.emplace_back("create_call", stageUs(ts));
    }
    // 3. Resolve the chat's video chat; create one when none exists.
    int32_t gcid = 0;
    {
        auto ts = std::chrono::steady_clock::now();
        gcid = groupCallId(chat_id);
        stages.emplace_back("get_chat", stageUs(ts));
        if (gcid < 0) return fail("get_chat", last_error_);
        if (gcid == 0) {
            auto ts2 = std::chrono::steady_clock::now();
            gcid = createVideoChat(chat_id, title);
            stages.emplace_back("create_video_chat", stageUs(ts2));
            if (gcid == 0) return fail("create_video_chat", last_error_);
        }
    }
    // 4. joinVideoChat(local SDP) -> remote transport payload.
    std::string remote;
    {
        auto ts = std::chrono::steady_clock::now();
        remote = joinVideoChat(gcid, offer, video);
        stages.emplace_back("join_video_chat", stageUs(ts));
        if (remote.empty()) return fail("join_video_chat", last_error_);
    }
    // 5. ntgcalls connect(remote).
    {
        auto ts = std::chrono::steady_clock::now();
        if (!ntg_.connect(chat_id, remote)) {
            last_error_ = "ntgcalls connect failed: " + ntg_.lastError();
            stages.emplace_back("connect", stageUs(ts));
            return fail("connect", last_error_);
        }
        stages.emplace_back("connect", stageUs(ts));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        joined_[chat_id] = gcid;
    }
    emitVoiceJoin(chat_id, "tdlib", us_since(), stages, true, "", "");
    return true;
}

bool TdLibVoiceCall::join(int64_t chat_id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (joined_.count(chat_id)) return true;  // already in the call
    }
    // Same steps as play() minus the audio source: the call is joined
    // silent until a track plays. joinCall times + emits t_voice_join.
    return joinCall(chat_id, "Parlay", false);
}

bool TdLibVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                          const std::string& title, bool video, int seek_sec,
                          double speed, int bass) {
    if (!available()) {
        last_error_ = describe();
        emitVoiceJoin(chat_id, "tdlib", 0, {}, false, "available",
                      last_error_);
        return false;
    }
    // Already in this chat's call (via /join)? Skip the join steps and
    // attach the new source to the live call; re-joining would fail.
    bool already_joined = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        already_joined = joined_.count(chat_id) != 0;
    }
    if (!already_joined) {
        // 1-5. The join sequence (joinCall), timed as t_voice_join.
        if (!joinCall(chat_id, title, video)) return false;
    }
    // 6. set_stream_sources(ffmpeg cmd).
    if (!ntg_.setSources(chat_id, stream_url, video, seek_sec, speed,
                         bass)) {
        last_error_ = "ntgcalls set_stream_sources failed: " +
                      ntg_.lastError();
        return false;
    }
    return true;
}

bool TdLibVoiceCall::stop(int64_t chat_id) {
    bool ok = ntg_.stop(chat_id);
    int32_t gcid = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = joined_.find(chat_id);
        if (it != joined_.end()) {
            gcid = it->second;
            joined_.erase(it);
        }
    }
    if (gcid != 0 && tdlib_->authorized()) {
        // Best effort: leave the group call on the signaling side.
        Json req = Json::objectOf({
            {"@type", Json("leaveGroupCall")},
            {"group_call_id", Json(static_cast<int64_t>(gcid))},
        });
        tdlib_->call(req, 10.0);
    }
    return ok;
}

bool TdLibVoiceCall::pause(int64_t chat_id) { return ntg_.pause(chat_id); }
bool TdLibVoiceCall::resume(int64_t chat_id) { return ntg_.resume(chat_id); }
bool TdLibVoiceCall::mute(int64_t chat_id) { return ntg_.mute(chat_id); }
bool TdLibVoiceCall::unmute(int64_t chat_id) { return ntg_.unmute(chat_id); }
int64_t TdLibVoiceCall::time(int64_t chat_id) { return ntg_.time(chat_id); }

// --- factory -------------------------------------------------------------------
// In-process only: TDLib (signaling + ntgcalls media) when TD_* are set
// and both .so files load, else bare dlopen'd ntgcalls (offer/sources,
// no join without an MTProto session), else unavailable with the exact
// reason. No bridge fallback exists any more (Phase 5: deleted).
std::unique_ptr<VoiceCall> makeVoiceCall(const Config& cfg) {
    TdLibVoiceCall tdlib_probe(cfg);
    if (tdlib_probe.available()) {
        std::cout << "[voice] backend=tdlib (" << tdlib_probe.describe()
                  << ")\n";
        return std::make_unique<TdLibVoiceCall>(cfg);
    }
    NtgCallsVoiceCall ntg(cfg.ntgcalls_lib);
    if (ntg.available()) {
        std::cout << "[voice] backend=ntgcalls-dlopen (" << ntg.describe()
                  << ")\n";
        return std::make_unique<NtgCallsVoiceCall>(cfg.ntgcalls_lib);
    }
    std::string reason;
    if (cfg.td_api_id != 0) {
        reason = "tdlib configured but " + tdlib_probe.describe() +
                 "; ntgcalls: " + ntg.lastError();
    } else {
        // TD_SESSION_DIR now defaults to <data_dir>/tdlib, so only
        // the api id/hash can be missing here.
        reason = "no TD_API_ID/TD_API_HASH (tdlib off); "
                 "ntgcalls: " +
                 ntg.lastError();
    }
    std::cout << "[voice] backend=unavailable (" << reason << ")\n";
    return std::make_unique<UnavailableVoiceCall>(reason);
}

}  // namespace parlay
