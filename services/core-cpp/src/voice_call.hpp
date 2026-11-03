// voice_call.hpp — voice-call binding, rebuilt on the real refs.
//
// The voice bridge is GONE (Phase 5, user order): no voice_bridge.py,
// no Telethon child process, no bridge server. ntgcalls is a C++
// NATIVE LIBRARY linked into the bot process — the assistant MTProto
// client + ntgcalls join directly in-process. Two real implementations
// behind one interface, in the order the core prefers them:
//
//   1. TdLibVoiceCall — TDLib 1.8.69 (MTProto signaling: authorize ->
//      getChat/createVideoChat -> joinVideoChat) + NtgCallsVoiceCall
//      (WebRTC, dlopen'd libntgcalls.so). This is the only backend
//      that can join a Telegram voice chat: the assistant session
//      lives in TD_SESSION_DIR (TD_API_ID/TD_API_HASH), no Telethon,
//      no Python, no child process.
//   2. NtgCallsVoiceCall — dlopen() of a real libntgcalls.so (the
//      anonx-cpp pattern: ./lib, /usr/local/lib, /usr/lib, plus
//      NTGCALLS_LIB), C API from ntgcalls.h. If the .so is absent the
//      backend reports unavailable instead of pretending. It can
//      build a local SDP offer and attach sources, but without an
//      MTProto session it cannot join on its own — play() says so
//      honestly.
//   3. OpusPipeline — the real audio pipeline the refs use, proven
//      standalone: source (file or URL via yt-dlp) -> ffmpeg -> Opus
//      48 kHz stereo -> Ogg Opus file AND RTP packets to a UDP sink
//      (VOICE_RTP_SINK). Used by the TDLib backend for its ffmpeg
//      shell command and by tests to prove Opus frames flow to a real
//      sink.
//
// The dispatcher still routes all playback through IPC to the vc-*
// services; the in-process voice path exists for /userbot (assistant join)
// and for the day the core owns a voice session itself (TinaMusic model).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

#include "config.hpp"
#include "ntgcalls.h"  // generated 3.0.5 C header — ntg_instance* is opaque
#include "tdlib_client.hpp"

namespace parlay {

// ffmpeg shell command producing raw s16le stereo PCM on stdout, the
// exact shape TinaMusic's BuildFfmpegCommand and NoxBeats'
// build_ffmpeg_cmd use (reconnect flags for http(s) inputs, -ss seek,
// -af atempo=speed + bass boost via lowpass mix when bass > 0).
// Throws std::invalid_argument on an empty source.
std::string buildFfmpegCmd(const std::string& src, int seek_sec = 0,
                           double speed = 1.0, int bass = 0,
                           bool video = false);

class VoiceCall {
public:
    virtual ~VoiceCall() = default;
    // Join the chat's voice call and stream `stream_url`. Mirrors
    // TinaMusic's Play(): create -> set sources -> join -> connect.
    virtual bool play(int64_t chat_id, const std::string& stream_url,
                      const std::string& title, bool video = false,
                      int seek_sec = 0, double speed = 1.0,
                      int bass = 0) = 0;
    // Join the chat's voice call without starting audio (vc.join).
    // Default: not supported — only backends with an MTProto session
    // can join (TDLib). Never fakes success: false + lastError() when
    // the backend cannot join.
    virtual bool join(int64_t /*chat_id*/) { return false; }
    virtual bool stop(int64_t chat_id) = 0;
    virtual bool pause(int64_t chat_id) = 0;
    virtual bool resume(int64_t chat_id) = 0;
    virtual bool mute(int64_t chat_id) = 0;
    virtual bool unmute(int64_t chat_id) = 0;
    // Seconds into the current stream, -1 when unknown.
    virtual int64_t time(int64_t chat_id) = 0;
    // True only when this backend can actually do voice work right now.
    virtual bool available() const = 0;
    // Human-readable backend state for logs and /voice diagnostics.
    virtual std::string describe() const = 0;
    virtual const char* name() const = 0;
    virtual std::string lastError() const { return ""; }
};

// Reports "unavailable" with the concrete reason. This is NOT a stub
// reply: callers surface describe() to the operator instead of faking
// success.
class UnavailableVoiceCall final : public VoiceCall {
public:
    explicit UnavailableVoiceCall(std::string reason)
        : reason_(std::move(reason)) {}
    bool play(int64_t, const std::string&, const std::string&, bool, int,
              double, int) override {
        return false;
    }
    bool stop(int64_t) override { return false; }
    bool pause(int64_t) override { return false; }
    bool resume(int64_t) override { return false; }
    bool mute(int64_t) override { return false; }
    bool unmute(int64_t) override { return false; }
    int64_t time(int64_t) override { return -1; }
    bool available() const override { return false; }
    std::string describe() const override {
        return "in-process voice unavailable: " + reason_;
    }
    const char* name() const override { return "unavailable"; }
    std::string lastError() const override { return reason_; }

private:
    std::string reason_;
};

// libntgcalls.so via dlopen (anonx-cpp DynamicNtgCalls pattern).
class NtgCallsVoiceCall final : public VoiceCall {
public:
    explicit NtgCallsVoiceCall(const std::string& explicit_path = "");
    ~NtgCallsVoiceCall() override;
    NtgCallsVoiceCall(const NtgCallsVoiceCall&) = delete;
    NtgCallsVoiceCall& operator=(const NtgCallsVoiceCall&) = delete;

    bool play(int64_t chat_id, const std::string& stream_url,
              const std::string& title, bool video, int seek_sec,
              double speed, int bass) override;
    // Join needs an MTProto session: this backend can build the local
    // offer and attach sources, but only the tdlib backend can join.
    // Reports that honestly instead of the base class's silent false.
    bool join(int64_t chat_id) override;
    bool stop(int64_t chat_id) override;
    bool pause(int64_t chat_id) override;
    bool resume(int64_t chat_id) override;
    bool mute(int64_t chat_id) override;
    bool unmute(int64_t chat_id) override;
    int64_t time(int64_t chat_id) override;
    bool available() const override { return handle_ != nullptr; }
    std::string describe() const override;
    const char* name() const override { return "ntgcalls-dlopen"; }
    std::string lastError() const override { return last_error_; }
    // The remote transport JSON from phone.joinGroupCall must be supplied
    // by the MTProto side before connect(); this is the TinaMusic step 5.
    bool connect(int64_t chat_id, const std::string& remote_params);
    // TinaMusic step 1: create the call, return the local SDP offer.
    // Split out of play() so the TDLib backend can do the MTProto join
    // between create and connect (ntgcalls cannot join on its own).
    bool createOffer(int64_t chat_id, std::string& offer_out);
    // TinaMusic step 2: attach the ffmpeg shell command(s) as sources.
    bool setSources(int64_t chat_id, const std::string& stream_url,
                    bool video, int seek_sec, double speed, int bass);

private:
    void* handle_ = nullptr;
    ntg_instance* ntg_ = nullptr;
    std::string lib_path_;
    std::string last_error_;
};

// The yt-dlp -> ffmpeg -> Opus pipeline, proven standalone.
// run() resolves `source` (URL via yt-dlp, else a local file), encodes
// Opus 48 kHz stereo, writes an Ogg Opus file, and — when rtp_sink is
// "host:port" — also emits real RTP packets to that UDP sink.
// Returns false + fills stats.error when ffmpeg/yt-dlp fails.
struct OpusStats {
    bool ok = false;
    std::string error;
    int frames = 0;          // Opus packets in the Ogg file (ffprobe count)
    int64_t bytes = 0;       // Ogg Opus file size
    double duration_sec = 0; // ffprobe duration
    std::string resolved_url;
    // RTP second output is best-effort and independent of the file encode:
    // a sandbox that blocks UDP (EPERM on sendto) must not lose the file.
    bool rtp_ok = false;
    std::string rtp_error;
};
class OpusPipeline {
public:
    OpusStats run(const std::string& source, const std::string& opus_out,
                  const std::string& rtp_sink = "");
    // Resolve a URL to a direct audio URL with yt-dlp (empty on failure).
    static std::string resolveUrl(const std::string& url);
};

// TdLibVoiceCall: TDLib 1.8.69 (MTProto signaling) + ntgcalls (WebRTC).
// Flow (TinaMusic JoinGroupCall, with TDLib in place of Telethon):
//   authorize (TD_API_ID/TD_API_HASH/TD_SESSION_DIR, TD_PHONE/TD_CODE/
//   TD_PASSWORD as the auth states demand) -> ntgcalls create_call ->
//   local SDP offer -> getChat -> video_chat.group_call_id (or
//   createVideoChat when none exists) -> joinVideoChat(local SDP) ->
//   Text.text = remote transport payload -> ntgcalls connect(remote) ->
//   set_stream_sources(ffmpeg cmd).
// TDLib does NOT do audio itself — it is the signaling side only; all
// audio stays in NtgCallsVoiceCall. Without credentials the backend
// reports "not authorized: no credentials" and never fakes a join.
class TdLibVoiceCall final : public VoiceCall {
public:
    explicit TdLibVoiceCall(const Config& cfg);
    // Test seam: takes over a scripted TdLibClient (see
    // tdlib_test.cpp) so the join flow is proven without a live
    // Telegram session. The ntgcalls side stays real: the offer and
    // the connect call come from the built libntgcalls.so.
    TdLibVoiceCall(const Config& cfg, std::unique_ptr<TdLibClient> tdlib);
    ~TdLibVoiceCall() override = default;
    TdLibVoiceCall(const TdLibVoiceCall&) = delete;
    TdLibVoiceCall& operator=(const TdLibVoiceCall&) = delete;

    bool play(int64_t chat_id, const std::string& stream_url,
              const std::string& title, bool video, int seek_sec,
              double speed, int bass) override;
    bool join(int64_t chat_id) override;
    bool stop(int64_t chat_id) override;
    bool pause(int64_t chat_id) override;
    bool resume(int64_t chat_id) override;
    bool mute(int64_t chat_id) override;
    bool unmute(int64_t chat_id) override;
    int64_t time(int64_t chat_id) override;
    bool available() const override;
    std::string describe() const override;
    const char* name() const override { return "tdlib"; }
    std::string lastError() const override { return last_error_; }

private:
    // Resolve the chat's group call id (0 = none). -1 + error on failure.
    int32_t groupCallId(int64_t chat_id);
    // Create the chat's video chat; returns the new group call id, 0 on
    // failure.
    int32_t createVideoChat(int64_t chat_id, const std::string& title);
    // joinVideoChat(local SDP) -> remote transport payload, "" on failure.
    std::string joinVideoChat(int32_t group_call_id,
                              const std::string& local_offer, bool video);

    const Config& cfg_;
    // Owned TDLib client (unique_ptr so tests can substitute a
    // scripted one; production always builds the real client).
    std::unique_ptr<TdLibClient> tdlib_;
    NtgCallsVoiceCall ntg_;
    std::map<int64_t, int32_t> joined_;  // chat_id -> group_call_id
    mutable std::mutex mu_;
    std::string last_error_;
};

// Factory: TDLib when TD_API_ID + TD_API_HASH + TD_SESSION_DIR are set
// (and libtdjson.so + libntgcalls.so load), else dlopen'd ntgcalls when
// the .so exists, else UnavailableVoiceCall with the exact reason (never
// a silent stub). There is no bridge fallback: the voice bridge was
// deleted in Phase 5 (user order) — ntgcalls is linked in-process.
std::unique_ptr<VoiceCall> makeVoiceCall(const Config& cfg);

}  // namespace parlay
