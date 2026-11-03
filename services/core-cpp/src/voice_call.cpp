// voice_call.cpp
#include "voice_call.hpp"

#include <dlfcn.h>
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
#include <utility>
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

bool NtgCallsVoiceCall::join(int64_t /*chat_id*/) {
    // This backend has no MTProto session: it can build the local SDP
    // offer (createOffer) and attach sources (setSources), but the
    // join itself — phone.joinGroupCall — needs TDLib. Say so with
    // the exact reason instead of the base class's silent false.
    last_error_ =
        "no MTProto session: this backend can build a local SDP offer "
        "and attach sources, but only the tdlib backend can join a "
        "Telegram voice chat (TD_API_ID/TD_API_HASH/TD_SESSION_DIR + "
        "libtdjson.so)";
    return false;
}

bool NtgCallsVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                             const std::string&, bool video, int seek_sec,
                             double speed, int bass) {
    if (!available()) return false;
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
        "remote transport from joinVideoChat, or use the tdlib "
        "backend which does the join in-process";
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

bool TdLibVoiceCall::join(int64_t chat_id) {
    if (!available()) {
        last_error_ = describe();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (joined_.count(chat_id)) return true;  // already in the call
    }
    // Same steps as play() minus the audio source: authorize ->
    // ntgcalls create_call (local SDP offer) -> resolve/create the
    // video chat -> joinVideoChat -> ntgcalls connect. No setSources:
    // the call is joined silent until a track plays.
    if (!tdlib_->authorized() && !tdlib_->authorize(60.0)) {
        last_error_ = tdlib_->lastError();
        return false;
    }
    std::string offer;
    if (!ntg_.createOffer(chat_id, offer)) {
        last_error_ = "ntgcalls create failed: " + ntg_.lastError();
        return false;
    }
    int32_t gcid = groupCallId(chat_id);
    if (gcid < 0) return false;
    if (gcid == 0) {
        gcid = createVideoChat(chat_id, "Parlay");
        if (gcid == 0) return false;
    }
    std::string remote = joinVideoChat(gcid, offer, false);
    if (remote.empty()) return false;
    if (!ntg_.connect(chat_id, remote)) {
        last_error_ = "ntgcalls connect failed: " + ntg_.lastError();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        joined_[chat_id] = gcid;
    }
    return true;
}

bool TdLibVoiceCall::play(int64_t chat_id, const std::string& stream_url,
                          const std::string& title, bool video, int seek_sec,
                          double speed, int bass) {
    if (!available()) {
        last_error_ = describe();
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
        // 1. Authorize the TDLib session (honest stop without credentials).
        if (!tdlib_->authorized() && !tdlib_->authorize(60.0)) {
            last_error_ = tdlib_->lastError();
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
        // 5. ntgcalls connect(remote).
        if (!ntg_.connect(chat_id, remote)) {
            last_error_ = "ntgcalls connect failed: " + ntg_.lastError();
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            joined_[chat_id] = gcid;
        }
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
