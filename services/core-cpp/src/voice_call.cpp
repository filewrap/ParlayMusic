// voice_call.cpp
#include "voice_call.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "ntgcalls.h"

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
namespace {
typedef uintptr_t (*fn_ntg_init)();
typedef int (*fn_ntg_destroy)(uintptr_t);
typedef int (*fn_ntg_create)(uintptr_t, int64_t, char**, ntg_async_struct);
typedef int (*fn_ntg_connect)(uintptr_t, int64_t, char*, bool,
                              ntg_async_struct);
typedef int (*fn_ntg_set_stream_sources)(uintptr_t, int64_t,
                                         ntg_stream_mode_enum,
                                         ntg_media_description_struct,
                                         ntg_async_struct);
typedef int (*fn_ntg_pause)(uintptr_t, int64_t, ntg_async_struct);
typedef int (*fn_ntg_resume)(uintptr_t, int64_t, ntg_async_struct);
typedef int (*fn_ntg_mute)(uintptr_t, int64_t, ntg_async_struct);
typedef int (*fn_ntg_unmute)(uintptr_t, int64_t, ntg_async_struct);
typedef int (*fn_ntg_stop)(uintptr_t, int64_t, ntg_async_struct);
typedef int (*fn_ntg_time)(uintptr_t, int64_t, ntg_stream_mode_enum, int64_t*,
                           ntg_async_struct);
typedef int (*fn_ntg_on_stream_end)(uintptr_t, ntg_stream_callback, void*);
typedef int (*fn_ntg_on_connection_change)(uintptr_t, ntg_connection_callback,
                                           void*);

struct NtgApi {
    fn_ntg_init init = nullptr;
    fn_ntg_destroy destroy = nullptr;
    fn_ntg_create create = nullptr;
    fn_ntg_connect connect = nullptr;
    fn_ntg_set_stream_sources set_stream_sources = nullptr;
    fn_ntg_pause pause = nullptr;
    fn_ntg_resume resume = nullptr;
    fn_ntg_mute mute = nullptr;
    fn_ntg_unmute unmute = nullptr;
    fn_ntg_stop stop = nullptr;
    fn_ntg_time time = nullptr;
    fn_ntg_on_stream_end on_stream_end = nullptr;
    fn_ntg_on_connection_change on_connection_change = nullptr;
};

struct FutState {
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    int code = 0;
    char* msg = nullptr;
};
void onNtgDone(void* ud) {
    auto* sp = static_cast<std::shared_ptr<FutState>*>(ud);
    {
        std::lock_guard<std::mutex> lk((*sp)->mu);
        (*sp)->ready = true;
    }
    (*sp)->cv.notify_all();
    delete sp;
}

// Runs start(fut) and waits for the async callback (NoxBeats media_ntg.cpp
// run()). Throws std::runtime_error on failure/timeout.
template <class F>
void ntgRun(const char* what, F start, int timeout_sec = 60) {
    auto st = std::make_shared<FutState>();
    auto* cb = new std::shared_ptr<FutState>(st);
    ntg_async_struct f{};
    f.userData = cb;
    f.promise = onNtgDone;
    f.errorCode = &st->code;
    f.errorMessage = &st->msg;
    int rc = start(f);
    std::unique_lock<std::mutex> lk(st->mu);
    if (!st->cv.wait_for(lk, std::chrono::seconds(timeout_sec),
                         [&] { return st->ready; }))
        throw std::runtime_error(std::string("ntgcalls ") + what +
                                 " timed out");
    std::string m;
    if (st->msg) {
        m = st->msg;
        std::free(st->msg);
    }
    int code = st->code < 0 ? st->code : (rc < 0 ? rc : 0);
    if (code < 0)
        throw std::runtime_error(std::string("ntgcalls ") + what +
                                 " failed (" + std::to_string(code) + "): " +
                                 m);
}

NtgApi g_ntg{};
}  // namespace

NtgCallsVoiceCall::NtgCallsVoiceCall(const std::string& explicit_path) {
    std::vector<std::string> candidates;
    if (!explicit_path.empty()) candidates.push_back(explicit_path);
    candidates.push_back("./lib/libntgcalls.so");
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
    g_ntg.init = reinterpret_cast<fn_ntg_init>(sym("ntg_init"));
    g_ntg.destroy = reinterpret_cast<fn_ntg_destroy>(sym("ntg_destroy"));
    g_ntg.create = reinterpret_cast<fn_ntg_create>(sym("ntg_create"));
    g_ntg.connect = reinterpret_cast<fn_ntg_connect>(sym("ntg_connect"));
    g_ntg.set_stream_sources = reinterpret_cast<fn_ntg_set_stream_sources>(
        sym("ntg_set_stream_sources"));
    g_ntg.pause = reinterpret_cast<fn_ntg_pause>(sym("ntg_pause"));
    g_ntg.resume = reinterpret_cast<fn_ntg_resume>(sym("ntg_resume"));
    g_ntg.mute = reinterpret_cast<fn_ntg_mute>(sym("ntg_mute"));
    g_ntg.unmute = reinterpret_cast<fn_ntg_unmute>(sym("ntg_unmute"));
    g_ntg.stop = reinterpret_cast<fn_ntg_stop>(sym("ntg_stop"));
    g_ntg.time = reinterpret_cast<fn_ntg_time>(sym("ntg_time"));
    g_ntg.on_stream_end =
        reinterpret_cast<fn_ntg_on_stream_end>(sym("ntg_on_stream_end"));
    g_ntg.on_connection_change = reinterpret_cast<fn_ntg_on_connection_change>(
        sym("ntg_on_connection_change"));
    if (!g_ntg.init || !g_ntg.create || !g_ntg.connect) {
        last_error_ = "libntgcalls.so at " + lib_path_ +
                      " lacks the ntg_init/create/connect symbols";
        dlclose(handle_);
        handle_ = nullptr;
        return;
    }
    ntg_ = g_ntg.init();
    if (!ntg_) {
        last_error_ = "ntg_init() failed";
        dlclose(handle_);
        handle_ = nullptr;
    }
}

NtgCallsVoiceCall::~NtgCallsVoiceCall() {
    if (ntg_ && g_ntg.destroy) g_ntg.destroy(ntg_);
    if (handle_) dlclose(handle_);
}

std::string NtgCallsVoiceCall::describe() const {
    if (!handle_) return "ntgcalls unavailable: " + last_error_;
    return "ntgcalls via " + lib_path_;
}

bool NtgCallsVoiceCall::createOffer(int64_t chat_id,
                                    std::string& offer_out) {
    if (!available()) return false;
    try {
        char* buf = nullptr;
        ntgRun("create", [&](ntg_async_struct f) {
            return g_ntg.create(ntg_, chat_id, &buf, f);
        });
        offer_out = buf ? buf : "";
        std::free(buf);
        if (offer_out.empty()) {
            last_error_ = "ntg_create returned no params";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::setSources(int64_t chat_id,
                                   const std::string& stream_url, bool video,
                                   int seek_sec, double speed, int bass) {
    if (!available()) return false;
    try {
        std::string cmd = buildFfmpegCmd(stream_url, seek_sec, speed, bass);
        std::string video_cmd;
        if (video) video_cmd = buildFfmpegCmd(stream_url, seek_sec, speed, 0,
                                              true);
        ntg_audio_description_struct a{};
        a.mediaSource = NTG_SHELL;
        a.input = const_cast<char*>(cmd.c_str());
        a.sampleRate = 48000;
        a.channelCount = 2;
        a.keepOpen = false;
        ntg_video_description_struct v{};
        v.mediaSource = NTG_SHELL;
        v.input = const_cast<char*>(video_cmd.c_str());
        v.width = 1280;
        v.height = 720;
        v.fps = 30;
        v.keepOpen = false;
        ntg_media_description_struct d{};
        d.microphone = &a;
        if (!video_cmd.empty()) d.camera = &v;
        ntgRun("set_stream_sources", [&](ntg_async_struct f) {
            return g_ntg.set_stream_sources(ntg_, chat_id, NTG_STREAM_CAPTURE,
                                            d, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                             const std::string&, bool video, int seek_sec,
                             double speed, int bass) {
    if (!available()) return false;
    try {
        // TinaMusic Play() step 1-2: create the call (local SDP offer),
        // attach the ffmpeg shell command as the audio source.
        std::string offer;
        if (!createOffer(chat_id, offer)) return false;
        if (!setSources(chat_id, stream_url, video, seek_sec, speed, bass))
            return false;
        // Step 3 (TinaMusic): the caller must now phone.joinGroupCall with
        // this offer via MTProto and call connect() with the remote
        // transport. Without an MTProto session this backend can create
        // the offer but cannot join — report that honestly.
        last_error_ =
            "local SDP offer created (" + std::to_string(offer.size()) +
            " bytes) but no MTProto join happened: call connect() with the "
            "remote transport from phone.joinGroupCall, or use the "
            "tdlib / voice-bridge backend which does the join";
        return false;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::connect(int64_t chat_id,
                                const std::string& remote_params) {
    if (!available()) return false;
    try {
        ntgRun("connect", [&](ntg_async_struct f) {
            return g_ntg.connect(ntg_, chat_id,
                                 const_cast<char*>(remote_params.c_str()),
                                 false, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::stop(int64_t chat_id) {
    if (!available()) return false;
    try {
        ntgRun("stop", [&](ntg_async_struct f) {
            return g_ntg.stop(ntg_, chat_id, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::pause(int64_t chat_id) {
    if (!available()) return false;
    try {
        ntgRun("pause", [&](ntg_async_struct f) {
            return g_ntg.pause(ntg_, chat_id, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::resume(int64_t chat_id) {
    if (!available()) return false;
    try {
        ntgRun("resume", [&](ntg_async_struct f) {
            return g_ntg.resume(ntg_, chat_id, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::mute(int64_t chat_id) {
    if (!available()) return false;
    try {
        ntgRun("mute", [&](ntg_async_struct f) {
            return g_ntg.mute(ntg_, chat_id, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool NtgCallsVoiceCall::unmute(int64_t chat_id) {
    if (!available()) return false;
    try {
        ntgRun("unmute", [&](ntg_async_struct f) {
            return g_ntg.unmute(ntg_, chat_id, f);
        });
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

int64_t NtgCallsVoiceCall::time(int64_t chat_id) {
    if (!available() || !g_ntg.time) return -1;
    try {
        int64_t t = 0;
        ntgRun("time", [&](ntg_async_struct f) {
            return g_ntg.time(ntg_, chat_id, NTG_STREAM_CAPTURE, &t, f);
        });
        return t;
    } catch (...) {
        return -1;
    }
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
    OpusStats st;
    std::string input = source;
    if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        st.resolved_url = resolveUrl(source);
        if (!st.resolved_url.empty()) input = st.resolved_url;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(opus_out).parent_path(), ec);
    std::string cmd = "ffmpeg -y -v error -i " + shellQuote(input) +
                      " -map 0:a -c:a libopus -b:a 128k -ar 48000 -ac 2 -f "
                      "opus " +
                      shellQuote(opus_out);
    int rc = 0;
    runCapture(cmd, &rc);
    if (rc != 0 || !std::filesystem::exists(opus_out)) {
        st.error = "ffmpeg opus encode failed (exit " + std::to_string(rc) +
                   ")";
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
        int rtp_rc = 0;
        runCapture(rtp_cmd, &rtp_rc);
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
    return st;
}

// --- VoiceBridge ---------------------------------------------------------------
VoiceBridge::VoiceBridge(const Config& cfg) : cfg_(cfg) {
    // Prefer the phase4 voice venv (ntgcalls + telethon installed there);
    // fall back to system python3.
    std::string venv_py =
        std::filesystem::path(cfg_.voice_bridge).parent_path().string() +
        "/../phase4/voice-venv/bin/python";
    if (std::filesystem::exists(venv_py)) {
        python_ = venv_py;
    } else {
        python_ = "python3";
    }
}

VoiceBridge::~VoiceBridge() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [chat, pid] : children_) {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, WNOHANG);
        }
    }
}

bool VoiceBridge::available() const {
    if (cfg_.assistant_session_file.empty() || cfg_.api_id == 0 ||
        cfg_.api_hash.empty())
        return false;
    if (cfg_.voice_bridge.empty() ||
        !std::filesystem::exists(cfg_.voice_bridge))
        return false;
    return std::filesystem::exists(cfg_.assistant_session_file);
}

std::string VoiceBridge::describe() const {
    if (cfg_.assistant_session_file.empty())
        return "voice-bridge unavailable: ASSISTANT_SESSION_FILE not set";
    if (cfg_.api_id == 0 || cfg_.api_hash.empty())
        return "voice-bridge unavailable: API_ID/API_HASH not set";
    if (cfg_.voice_bridge.empty())
        return "voice-bridge unavailable: VOICE_BRIDGE path not set";
    return "voice-bridge via " + cfg_.voice_bridge + " (Telethon + ntgcalls)";
}

bool VoiceBridge::spawn(int64_t chat_id, const std::string& stream_url,
                        bool video, int seek_sec, double speed, int bass) {
    std::string cmd = buildFfmpegCmd(stream_url, seek_sec, speed, bass);
    std::string video_cmd;
    if (video) video_cmd = buildFfmpegCmd(stream_url, seek_sec, speed, 0, true);
    // Child: python voice_bridge.py join-call --chat-id N --session FILE
    // --api-id N --api-hash X --ffmpeg-cmd '...' [--video-cmd '...']
    // The bridge prints JSON lines; the core does not parse them here —
    // the child is tracked by pid and killed on stop().
    pid_t pid = fork();
    if (pid == 0) {
        // Child: silence stdin, redirect stdout/stderr to a per-chat log.
        std::string log = cfg_.data_dir + "/voice-" +
                          std::to_string(chat_id) + ".log";
        std::error_code ec;
        std::filesystem::create_directories(cfg_.data_dir, ec);
        FILE* lf = freopen(log.c_str(), "a", stdout);
        if (lf) dup2(fileno(stdout), STDERR_FILENO);
        execlp(python_.c_str(), python_.c_str(), cfg_.voice_bridge.c_str(),
               "join-call", "--chat-id", std::to_string(chat_id).c_str(),
               "--session", cfg_.assistant_session_file.c_str(), "--api-id",
               std::to_string(cfg_.api_id).c_str(), "--api-hash",
               cfg_.api_hash.c_str(), "--ffmpeg-cmd", cmd.c_str(),
               "--video-cmd", video_cmd.c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) {
        last_error_ = "fork() failed";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    children_[chat_id] = pid;
    return true;
}

bool VoiceBridge::play(int64_t chat_id, const std::string& stream_url,
                       const std::string& title, bool video, int seek_sec,
                       double speed, int bass) {
    (void)title;
    if (!available()) {
        last_error_ = describe();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = children_.find(chat_id);
        if (it != children_.end() && it->second > 0) {
            kill(it->second, SIGTERM);
            int status = 0;
            waitpid(it->second, &status, 0);
            children_.erase(it);
        }
    }
    return spawn(chat_id, stream_url, video, seek_sec, speed, bass);
}

bool VoiceBridge::stop(int64_t chat_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = children_.find(chat_id);
    if (it == children_.end()) return false;
    if (it->second > 0) {
        kill(it->second, SIGTERM);
        int status = 0;
        waitpid(it->second, &status, WNOHANG);
    }
    children_.erase(it);
    return true;
}

bool VoiceBridge::pause(int64_t chat_id) {
    // The bridge child owns the ntgcalls session; pause/resume/mute are
    // signalled by sending SIGUSR1/SIGUSR2 to the child, which the bridge
    // maps to ntgcalls pause/resume. (Not yet wired in the bridge: these
    // return false until the signal protocol lands.)
    (void)chat_id;
    last_error_ = "pause via bridge not wired yet (child signal protocol)";
    return false;
}
bool VoiceBridge::resume(int64_t chat_id) {
    (void)chat_id;
    last_error_ = "resume via bridge not wired yet (child signal protocol)";
    return false;
}
bool VoiceBridge::mute(int64_t chat_id) {
    (void)chat_id;
    last_error_ = "mute via bridge not wired yet (child signal protocol)";
    return false;
}
bool VoiceBridge::unmute(int64_t chat_id) {
    (void)chat_id;
    last_error_ = "unmute via bridge not wired yet (child signal protocol)";
    return false;
}

bool VoiceBridge::joinChat(int64_t chat_id, const std::string& invite_link,
                           std::string& error_out) {
    if (cfg_.assistant_session_file.empty() || cfg_.api_id == 0 ||
        cfg_.api_hash.empty()) {
        error_out = describe();
        return false;
    }
    if (invite_link.empty()) {
        error_out = "no invite link (bot must be admin to export one)";
        return false;
    }
    // One-shot: python voice_bridge.py join-chat --chat-id N ...
    // The bridge prints {"ok": true, ...} or {"ok": false, "error": ...}.
    std::string out = runCapture(
        shellQuote(python_) + " " + shellQuote(cfg_.voice_bridge) +
        " join-chat --chat-id " + std::to_string(chat_id) + " --session " +
        shellQuote(cfg_.assistant_session_file) + " --api-id " +
        std::to_string(cfg_.api_id) + " --api-hash " +
        shellQuote(cfg_.api_hash) + " --invite " + shellQuote(invite_link) +
        " 2>&1");
    if (out.find("\"ok\": true") != std::string::npos ||
        out.find("\"ok\":true") != std::string::npos)
        return true;
    error_out = out.empty() ? "voice bridge produced no output"
                            : out.substr(0, 500);
    return false;
}

// --- TdLibVoiceCall ----------------------------------------------------------
// TDLib (MTProto signaling) + ntgcalls (WebRTC). TDLib does NOT do audio
// itself: it authorizes, resolves the chat's video chat, and performs
// joinVideoChat (the phone.joinGroupCall equivalent from TinaMusic's
// mtproto.cpp); the returned Text.text is the remote transport payload
// fed to ntgcalls connect(), exactly like the voice bridge does with
// Telethon.
TdLibVoiceCall::TdLibVoiceCall(const Config& cfg)
    : cfg_(cfg), tdlib_(cfg.tdlib_lib), ntg_(cfg.ntgcalls_lib) {}

bool TdLibVoiceCall::available() const {
    return tdlib_.available() && cfg_.td_api_id != 0 &&
           !cfg_.td_api_hash.empty() && !cfg_.td_session_dir.empty() &&
           ntg_.available();
}

std::string TdLibVoiceCall::describe() const {
    if (!tdlib_.available())
        return "tdlib unavailable: " + tdlib_.lastError();
    if (cfg_.td_api_id == 0 || cfg_.td_api_hash.empty() ||
        cfg_.td_session_dir.empty())
        return "tdlib unavailable: TD_API_ID/TD_API_HASH/TD_SESSION_DIR "
               "not all set";
    if (!ntg_.available())
        return "tdlib unavailable: ntgcalls: " + ntg_.lastError();
    return "tdlib via " + tdlib_.libPath() + " + ntgcalls via " +
           ntg_.describe();
}

int32_t TdLibVoiceCall::groupCallId(int64_t chat_id) {
    Json req = Json::objectOf({
        {"@type", Json("getChat")},
        {"chat_id", Json(chat_id)},
    });
    Json resp = tdlib_.call(req, 30.0);
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
    Json resp = tdlib_.call(req, 30.0);
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
    Json resp = tdlib_.call(req, 30.0);
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

bool TdLibVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                          const std::string& title, bool video, int seek_sec,
                          double speed, int bass) {
    if (!available()) {
        last_error_ = describe();
        return false;
    }
    // 1. Authorize the TDLib session (honest stop without credentials).
    if (!tdlib_.authorized() && !tdlib_.authorize(60.0)) {
        last_error_ = tdlib_.lastError();
        return false;
    }
    // 2. ntgcalls create_call -> local SDP offer.
    std::string offer;
    if (!ntg_.createOffer(chat_id, offer)) {
        last_error_ = "ntgcalls create failed: " + ntg_.lastError();
        return false;
    }
    // 3. Resolve the chat's video chat; create one when none exists.
    int32_t gcid = groupCallId(chat_id);
    if (gcid < 0) return false;
    if (gcid == 0) {
        gcid = createVideoChat(chat_id, title);
        if (gcid == 0) return false;
    }
    // 4. joinVideoChat(local SDP) -> remote transport payload.
    std::string remote = joinVideoChat(gcid, offer, video);
    if (remote.empty()) return false;
    // 5. ntgcalls connect(remote) -> set_stream_sources(ffmpeg cmd).
    if (!ntg_.connect(chat_id, remote)) {
        last_error_ = "ntgcalls connect failed: " + ntg_.lastError();
        return false;
    }
    if (!ntg_.setSources(chat_id, stream_url, video, seek_sec, speed,
                         bass)) {
        last_error_ = "ntgcalls set_stream_sources failed: " +
                      ntg_.lastError();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        joined_[chat_id] = gcid;
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
    if (gcid != 0 && tdlib_.authorized()) {
        // Best effort: leave the group call on the signaling side.
        Json req = Json::objectOf({
            {"@type", Json("leaveGroupCall")},
            {"group_call_id", Json(static_cast<int64_t>(gcid))},
        });
        tdlib_.call(req, 10.0);
    }
    return ok;
}

bool TdLibVoiceCall::pause(int64_t chat_id) { return ntg_.pause(chat_id); }
bool TdLibVoiceCall::resume(int64_t chat_id) { return ntg_.resume(chat_id); }
bool TdLibVoiceCall::mute(int64_t chat_id) { return ntg_.mute(chat_id); }
bool TdLibVoiceCall::unmute(int64_t chat_id) { return ntg_.unmute(chat_id); }
int64_t TdLibVoiceCall::time(int64_t chat_id) { return ntg_.time(chat_id); }

// --- factory -------------------------------------------------------------------
std::unique_ptr<VoiceCall> makeVoiceCall(const Config& cfg) {
    TdLibVoiceCall tdlib_probe(cfg);
    if (tdlib_probe.available()) {
        std::cout << "[voice] backend=tdlib (" << tdlib_probe.describe()
                  << ")\n";
        return std::make_unique<TdLibVoiceCall>(cfg);
    }
    VoiceBridge bridge_probe(cfg);
    if (bridge_probe.available()) {
        std::cout << "[voice] backend=voice-bridge (Telethon + ntgcalls)\n";
        return std::make_unique<VoiceBridge>(cfg);
    }
    NtgCallsVoiceCall ntg(cfg.ntgcalls_lib);
    if (ntg.available()) {
        std::cout << "[voice] backend=ntgcalls-dlopen (" << ntg.describe()
                  << ")\n";
        return std::make_unique<NtgCallsVoiceCall>(cfg.ntgcalls_lib);
    }
    std::string reason;
    if (!cfg.assistant_session_file.empty()) {
        reason = "bridge session configured but " + bridge_probe.describe() +
                 "; tdlib: " + tdlib_probe.describe() +
                 "; ntgcalls: " + ntg.lastError();
    } else if (cfg.td_api_id != 0) {
        reason = "tdlib configured but " + tdlib_probe.describe() +
                 "; ntgcalls: " + ntg.lastError();
    } else {
        reason = "no ASSISTANT_SESSION_FILE (bridge off) and no "
                 "TD_API_ID/TD_API_HASH/TD_SESSION_DIR (tdlib off); "
                 "ntgcalls: " +
                 ntg.lastError();
    }
    std::cout << "[voice] backend=unavailable (" << reason << ")\n";
    return std::make_unique<UnavailableVoiceCall>(reason);
}

}  // namespace parlay
