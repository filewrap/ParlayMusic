// stub_backend.cpp — TEST STUB backend for the Parlay UDS protocol.
//
// *** THIS IS A TEST PEER, NOT PRODUCTION CODE. ***
// It speaks contracts/uds-protocol.md over a Unix Domain Socket and
// answers all 65 actions from contracts/ipc.json with structurally
// valid (canned) responses, so the core's UDS client (services/
// core-cpp/src/ipc.*) can be proven against a real peer before the Go
// backend exists in Phase 3. The locked architecture explicitly
// allows the core to talk to a stub backend in this phase
// (FINAL-ARCHITECTURE.md §9 Phase 2). Nothing here ships: it is not
// wired into CMake for the production binary (see phase2/ipc-cmake.txt
// for the test-only target) and it must never be run as the backend
// for a real bot.
//
// Behaviour:
//   * Listens on the AF_UNIX socket path in argv[1] (default
//     /tmp/parlay-stub-backend.sock), one connection at a time.
//   * Answers every request frame {type:"request", request_id,
//     action, payload} with {type:"response", request_id, ok, data}
//     (or ok:false + {code:"not_found"} for unknown actions).
//   * vc.join exercises the duplex hook: before answering, the stub
//     sends the core a backend-initiated JoinRequest frame
//     {type:"join_request", request_id, chat_id, local_sdp} and waits
//     (5 s) for the core's JoinResponse, then answers vc.join with
//     joined:true and the group_call_id from that response.
//   * vc.play exercises the event hook: after answering, the stub
//     sends a TrackStarted event frame on the same socket.
//   * Test-only action "test.sleep_ms" {sleep_ms} sleeps before
//     answering, so the core's deadline behaviour can be proven.
//     It is NOT one of the 65 contract actions and the real backend
//     must reject it.
//   * All data is canned and clearly fake (track "Stub Song", ids
//     "stub-*", example.invalid URLs). No network, no state, no auth:
//     the PARLAY_TOKEN header concept does not exist on UDS — the
//     socket file's filesystem permissions are the access control.
//
// Build: g++ -std=c++20 -I src tests/stub_backend.cpp src/ipc.cpp
//          src/json.cpp -o /tmp/stub_backend
// Run:   /tmp/stub_backend /tmp/parlay-stub-backend.sock
// Logs on stderr; "listening on <path>" on stdout at startup.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc.hpp"
#include "json.hpp"

namespace {

using parlay::Json;
using parlay::uds::readFrame;
using parlay::uds::readFrameUntil;
using parlay::uds::writeFrame;

std::string g_socket_path;
volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

Json cannedTrack() {
    return Json::objectOf(
        {{"id", Json("stub-track-1")},
         {"title", Json("Stub Song")},
         {"artist", Json("Stub Artist")},
         {"source", Json("youtube")},
         {"duration_sec", Json((int64_t)268)},
         {"duration_str", Json("4:28")},
         {"url", Json("https://example.invalid/watch?v=stub")},
         {"stream_url", Json("https://example.invalid/audio.webm")},
         {"thumbnail", Json("")},
         {"views", Json((int64_t)1234)},
         {"uploader", Json("Stub Channel")},
         {"upload_time", Json("2020-01-01")},
         {"is_live", Json(false)}});
}

Json cannedNowPlaying(int64_t chat_id) {
    Json::Array queue;
    return Json::objectOf(
        {{"chat_id", Json(chat_id)},
         {"track", cannedTrack()},
         {"engine", Json("go")},
         {"loop", Json((int64_t)0)},
         {"loop_count", Json((int64_t)0)},
         {"paused", Json(false)},
         {"is_paused", Json(false)},
         {"is_muted", Json(false)},
         {"position_sec", Json((int64_t)0)},
         {"queue", Json(std::move(queue))},
         {"shuffle", Json(false)},
         {"autoplay", Json(false)},
         {"autoplay_enabled", Json(false)},
         {"volume", Json((int64_t)100)},
         {"speed", Json(1.0)},
         {"stream_url", Json("https://example.invalid/audio.webm")}});
}

Json cannedQueue() {
    Json::Array items;
    items.push_back(Json::objectOf(
        {{"position", Json((int64_t)0)},
         {"requested_by", Json((int64_t)111)},
         {"track", cannedTrack()}}));
    return Json::objectOf({{"queue", Json(std::move(items))}});
}

int64_t chatOf(const Json& payload, int64_t fallback = -1001) {
    const Json* c = payload.find("chat_id");
    return (c && c->isInt()) ? c->asInt() : fallback;
}

int64_t userOf(const Json& payload, int64_t fallback = 111) {
    const Json* u = payload.find("user_id");
    if (!u) u = payload.find("requested_by");
    return (u && u->isInt()) ? u->asInt() : fallback;
}

// Answer one contract action with a structurally valid response.
// Returns false when the action is unknown (caller sends not_found).
bool answer(const std::string& action, const Json& payload, Json& data_out) {
    if (action == "ping") {
        data_out = Json::objectOf({{"service", Json("stub-backend")},
                                   {"version", Json("0.1.0")}});
        return true;
    }
    if (action == "test.sleep_ms") {
        const Json* ms = payload.find("sleep_ms");
        int64_t sleep_ms = (ms && ms->isInt()) ? ms->asInt() : 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        data_out = Json::objectOf(
            {{"slept_ms", Json(sleep_ms)}});
        return true;
    }
    int64_t chat_id = chatOf(payload);
    int64_t user_id = userOf(payload);

    // --- vc.* ---
    if (action == "vc.play" || action == "vc.pause" ||
        action == "vc.resume" || action == "vc.skip" ||
        action == "vc.seek" || action == "vc.loop" ||
        action == "vc.shuffle" || action == "vc.autoplay" ||
        action == "vc.autoplay_skip" || action == "vc.radio" ||
        action == "vc.panel") {
        data_out = Json::objectOf(
            {{"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    if (action == "vc.stop") {
        data_out = Json(Json::Object{});
        return true;
    }
    if (action == "vc.join") {
        // Handled by the caller (duplex handshake first).
        return false;
    }
    if (action == "vc.mute") {
        const Json* m = payload.find("muted");
        data_out = Json::objectOf(
            {{"muted", Json(m ? m->asBool() : false)},
             {"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    if (action == "vc.volume") {
        data_out = Json::objectOf(
            {{"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    if (action == "vc.now_playing") {
        data_out = Json::objectOf(
            {{"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    if (action == "vc.progress") {
        data_out = Json::objectOf(
            {{"position_sec", Json((int64_t)30)},
             {"duration_sec", Json((int64_t)268)},
             {"is_paused", Json(false)},
             {"is_muted", Json(false)},
             {"loop_enabled", Json(false)},
             {"now", cannedTrack()},
             {"next", Json(nullptr)}});
        return true;
    }
    if (action == "vc.queue") {
        data_out = cannedQueue();
        return true;
    }
    if (action == "vc.lyrics") {
        data_out = Json::objectOf(
            {{"lyrics", Json("[00:00.00]stub lyrics")},
             {"source", Json("lrclib")},
             {"track_id", Json("stub-track-1")},
             {"title", Json("Stub Song")},
             {"artist", Json("Stub Artist")},
             {"truncated", Json(false)}});
        return true;
    }

    // --- chat.settings ---
    if (action == "chat.settings") {
        data_out = Json::objectOf(
            {{"chat_id", Json(chat_id)},
             {"settings",
              Json::objectOf(
                  {{"admin_mode", Json((int64_t)0)},
                   {"play_mode", Json("direct")},
                   {"cmd_delete", Json(false)},
                   {"clean_mode", Json(false)},
                   {"clean_delay_min", Json((int64_t)5)},
                   {"thumbnails", Json(true)},
                   {"autoplay", Json(false)},
                   {"autoplay_lang", Json("en")},
                   {"autoplay_mood", Json("")},
                   {"silent_mode", Json(false)},
                   {"auto_delete", Json(false)},
                   {"auto_delete_min", Json((int64_t)0)},
                   {"language", Json("en")},
                   {"paginate_purge_old", Json(false)},
                   {"chat_music_download", Json(true)}})}});
        return true;
    }

    // --- download.* ---
    if (action == "download.youtube" || action == "download.spotify" ||
        action == "download.deezer" || action == "download.navidrome" ||
        action == "download.tiktok") {
        data_out = Json::objectOf(
            {{"file_path", Json("/tmp/stub/stub-track-1.opus")},
             {"track", cannedTrack()},
             {"run_id", Json("abcdef123456")},
             {"oversize", Json(false)},
             {"size_bytes", Json((int64_t)1234567)}});
        return true;
    }
    if (action == "download.soulseek") {
        data_out = Json::objectOf(
            {{"file_path", Json("/tmp/stub/stub-track-1.flac")},
             {"track", cannedTrack()},
             {"oversize", Json(false)},
             {"size_bytes", Json((int64_t)2345678)},
             {"peer", Json("stub-peer")},
             {"format", Json("FLAC")},
             {"count", Json((int64_t)1)},
             {"total", Json((int64_t)1)},
             {"failed", Json(Json::Array{})},
             {"album_dir", Json("")},
             {"source", Json("soulseek")},
             {"run_id", Json("abcdef123456")}});
        return true;
    }
    if (action == "download.spotify_playlist") {
        Json::Array paths;
        paths.push_back(Json("/tmp/stub/stub-track-1.opus"));
        data_out = Json::objectOf(
            {{"count", Json((int64_t)1)},
             {"file_path", Json("/tmp/stub/stub-playlist.zip")},
             {"file_paths", Json(std::move(paths))},
             {"run_id", Json("abcdef123456")}});
        return true;
    }
    if (action == "download.progress") {
        Json::Array per_track;
        per_track.push_back(Json::objectOf(
            {{"title", Json("Stub Song")},
             {"state", Json("downloading")},
             {"pct", Json(42.5)},
             {"peer", Json(nullptr)},
             {"reason", Json(nullptr)}}));
        const Json* run = payload.find("run_id");
        data_out = Json::objectOf(
            {{"run_id", Json(run ? run->asString() : "abcdef123456")},
             {"percentage", Json(42.5)},
             {"done_bytes", Json((int64_t)524288)},
             {"total_bytes", Json((int64_t)1234567)},
             {"speed_bytes_per_sec", Json(65536.0)},
             {"eta_sec", Json((int64_t)11)},
             {"elapsed_sec", Json(8.0)},
             {"phase", Json("downloading")},
             {"per_track", Json(std::move(per_track))},
             {"lossy_available", Json(false)},
             {"lossy_summary", Json(nullptr)}});
        return true;
    }

    // --- fx / gui ---
    if (action == "fx.apply") {
        data_out = Json::objectOf(
            {{"output_path", Json("/tmp/stub/fx-out.opus")}});
        return true;
    }
    if (action == "gui.now_playing") {
        data_out = Json::objectOf(
            {{"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    if (action == "gui.queue") {
        data_out = cannedQueue();
        return true;
    }

    // --- lemonic / netease ---
    if (action == "lemonic.download") {
        data_out = Json::objectOf(
            {{"file_path", Json("/tmp/stub/stub-track-1.mp3")},
             {"track", cannedTrack()}});
        return true;
    }
    if (action == "lemonic.search" || action == "netease.search" ||
        action == "search.query" || action == "search.inline") {
        Json::Array results;
        results.push_back(cannedTrack());
        data_out = Json::objectOf({{"results", Json(std::move(results))}});
        return true;
    }
    if (action == "netease.download") {
        data_out = Json::objectOf(
            {{"file_path", Json("/tmp/stub/stub-track-1.flac")},
             {"track", cannedTrack()},
             {"auth", Json("anonymous")}});
        return true;
    }
    if (action == "netease.lyrics") {
        data_out = Json::objectOf(
            {{"lyrics", Json("[00:00.00]stub lyrics")},
             {"track_id", Json("stub-track-1")},
             {"title", Json("Stub Song")},
             {"artist", Json("Stub Artist")},
             {"source", Json("lrclib")},
             {"truncated", Json(false)},
             {"lrc_path", Json("")}});
        return true;
    }

    // --- queue / radio ---
    if (action == "queue.vote") {
        data_out = Json::objectOf({{"queued", Json(true)}});
        return true;
    }
    if (action == "radio.list") {
        Json::Array stations;
        stations.push_back(Json("stub-fm"));
        stations.push_back(Json("stub-rock"));
        data_out = Json::objectOf({{"stations", Json(std::move(stations))}});
        return true;
    }
    if (action == "radio.start") {
        data_out = Json::objectOf(
            {{"stream_url", Json("https://example.invalid/stub-fm")}});
        return true;
    }
    if (action == "radio.status") {
        data_out = Json::objectOf(
            {{"now_playing", Json(nullptr)},
             {"station", Json("stub-fm")},
             {"stream_url", Json("https://example.invalid/stub-fm")},
             {"reachable", Json(true)},
             {"vc_handoff", Json(nullptr)}});
        return true;
    }
    if (action == "radio.stop") {
        data_out = Json::objectOf({{"stopped", Json(true)}});
        return true;
    }

    // --- share ---
    if (action == "share.create") {
        data_out = Json::objectOf(
            {{"link", Json("https://t.me/parlaybot?start=share-stubtoken")},
             {"token", Json("stubtoken")}});
        return true;
    }
    if (action == "share.resolve") {
        data_out = Json::objectOf({{"track", cannedTrack()}});
        return true;
    }

    // --- spotify.* ---
    if (action == "spotify.analyze") {
        Json::Array profane;
        data_out = Json::objectOf(
            {{"analysis", Json("stub analysis")},
             {"profane_words", Json(std::move(profane))}});
        return true;
    }
    if (action == "spotify.details") {
        data_out = Json::objectOf(
            {{"track", cannedTrack()},
             {"features",
              Json::objectOf(
                  {{"key", Json("C")},
                   {"modality", Json((int64_t)1)},
                   {"tempo_bpm", Json(120.0)},
                   {"acousticness", Json(0.1)},
                   {"danceability", Json(0.5)},
                   {"energy", Json(0.7)},
                   {"instrumentalness", Json(0.0)},
                   {"liveness", Json(0.1)},
                   {"speechiness", Json(0.05)},
                   {"valence", Json(0.6)}})},
             {"genres", Json(Json::Array{Json("stub")})},
             {"disliked_by", Json(Json::Array{})},
             {"ignored_by", Json(Json::Array{})},
             {"profanity", Json(nullptr)},
             {"language", Json("en")},
             {"lyrics", Json(nullptr)},
             {"lyrics_link", Json(nullptr)}});
        return true;
    }
    if (action == "spotify.auth") {
        const Json* a = payload.find("action");
        std::string sub = a ? a->asString() : "login";
        Json inner = Json::objectOf(
            {{"authorized", Json(sub != "login" && sub != "revoke")},
             {"user_id", Json(user_id)}});
        if (sub == "login") {
            inner.set("authorize_url",
                      Json("https://example.invalid/auth"));
            inner.set("note", Json("stub: complete in a browser"));
            inner.set("authorized", Json(false));
        } else if (sub == "callback" || sub == "refresh") {
            inner.set("expires_at", Json((int64_t)9999999999));
        } else {  // revoke
            inner.set("was_authorized", Json(true));
        }
        data_out = Json(Json::Object{{sub, inner}});
        return true;
    }
    if (action == "spotify.dislike") {
        data_out = Json::objectOf({{"disliked", Json(true)}});
        return true;
    }
    if (action == "spotify.now_playing") {
        data_out = Json::objectOf(
            {{"playing", Json(true)},
             {"user_id", Json(user_id)},
             {"track", cannedTrack()},
             {"checked_at", Json((int64_t)1700000000)},
             {"last_skip", Json(false)},
             {"last_reason", Json(nullptr)},
             {"classifier_verdict", Json("clean")}});
        return true;
    }
    if (action == "spotify.resolve") {
        data_out = Json::objectOf(
            {{"kind", Json("track")},
             {"source", Json("spotify")},
             {"track_id", Json("stub")},
             {"url", Json("https://example.invalid/spotify/stub")}});
        return true;
    }
    if (action == "spotify.skip_check") {
        data_out = Json::objectOf(
            {{"reason", Json(nullptr)},
             {"skip", Json(false)},
             {"classifier_provider", Json("wordlist")},
             {"classifier_verdict", Json("clean")},
             {"classifier_confidence", Json(0.99)},
             {"classifier_detail", Json("")}});
        return true;
    }
    if (action == "spotify.watch") {
        data_out = Json(Json::Object{});
        return true;
    }
    if (action == "spotify.watches") {
        data_out = Json::objectOf(
            {{"watches", Json(Json::Array{Json((int64_t)111)})}});
        return true;
    }
    if (action == "spotify.collection") {
        Json::Array tracks;
        tracks.push_back(cannedTrack());
        data_out = Json::objectOf(
            {{"api", Json("spotify")},
             {"kind", Json("playlist")},
             {"name", Json("Stub Playlist")},
             {"tracks", Json(std::move(tracks))},
             {"truncated", Json(false)}});
        return true;
    }
    if (action == "spotify.status") {
        data_out = Json::objectOf(
            {{"api", Json("spotify")},
             {"credentials_configured", Json(false)},
             {"token_cached", Json(false)}});
        return true;
    }

    // --- OD-3 actions (Phase 3, Wave 2; contracts/uds-protocol.md §4
    // rows 57–65). Canned shapes match the §4 response columns.
    if (action == "download.cancel") {
        const Json* r = payload.find("run_id");
        data_out = Json::objectOf(
            {{"cancelled", Json(true)},
             {"run_id", Json(r ? r->asString() : "abcdef123456")},
             {"phase", Json("cancelled")}});
        return true;
    }
    if (action == "library.delete") {
        const Json* i = payload.find("id");
        data_out = Json::objectOf(
            {{"deleted", Json(true)},
             {"id", Json(i ? i->asString() : "stub-id")}});
        return true;
    }
    if (action == "mix.generate") {
        Json::Array tracks;
        tracks.push_back(cannedTrack());
        data_out = Json::objectOf(
            {{"tracks", Json(std::move(tracks))},
             {"source_track", cannedTrack()}});
        return true;
    }
    if (action == "audio.recognize") {
        data_out = Json::objectOf(
            {{"recognized", Json(true)},
             {"track", cannedTrack()},
             {"name", Json("Stub Song")},
             {"artists", Json(Json::Array{Json("Stub Artist")})},
             {"album", Json("Stub Album")},
             {"source", Json("stub")}});
        return true;
    }
    if (action == "library.scan") {
        data_out = Json::objectOf(
            {{"triggered", Json(true)},
             {"error", Json(nullptr)},
             {"tracks_found", Json((int64_t)0)}});
        return true;
    }
    if (action == "library.sharescan") {
        data_out = Json::objectOf(
            {{"triggered", Json(true)},
             {"scheduled_in_min", Json((int64_t)0)}});
        return true;
    }
    if (action == "cache.clear") {
        data_out = Json::objectOf(
            {{"cleared", Json(true)},
             {"names", Json(Json::Array{})}});
        return true;
    }
    if (action == "download.prefs") {
        data_out = Json::objectOf(
            {{"user_id", Json(user_id)},
             {"format", Json("mp3")},
             {"use_youtube", Json(true)},
             {"path_template", Json("{artist} - {title}")}});
        return true;
    }
    if (action == "vc.play_file") {
        data_out = Json::objectOf(
            {{"now_playing", cannedNowPlaying(chat_id)}});
        return true;
    }
    return false;
}

Json okResponse(int64_t request_id, const Json& data) {
    return Json::objectOf({{"type", Json("response")},
                           {"request_id", Json(request_id)},
                           {"ok", Json(true)},
                           {"data", data}});
}

Json errResponse(int64_t request_id, const std::string& code,
                 const std::string& message) {
    return Json::objectOf(
        {{"type", Json("response")},
         {"request_id", Json(request_id)},
         {"ok", Json(false)},
         {"error", Json::objectOf({{"code", Json(code)},
                                   {"message", Json(message)}})}});
}

// Serve one accepted connection until EOF or SIGTERM.
void serve(int fd) {
    int64_t next_join_id = 1000000;  // distinct from core request ids
    while (!g_stop) {
        std::string frame;
        if (!readFrame(fd, frame)) break;
        Json msg;
        try {
            msg = Json::parse(frame);
        } catch (...) {
            std::cerr << "[stub] corrupt frame, ignored\n";
            continue;
        }
        const Json* type = msg.find("type");
        if (!type || type->asString() != "request") continue;
        const Json* rid = msg.find("request_id");
        int64_t request_id = (rid && rid->isInt()) ? rid->asInt() : 0;
        const Json* action_j = msg.find("action");
        std::string action = action_j ? action_j->asString() : "";
        const Json* payload_j = msg.find("payload");
        Json payload = payload_j ? *payload_j : Json(Json::Object{});
        std::cerr << "[stub] request " << request_id << " action=" << action
                  << "\n";

        // vc.join: duplex handshake first — send the core a
        // JoinRequest and wait for its JoinResponse.
        if (action == "vc.join") {
            int64_t join_id = next_join_id++;
            int64_t chat_id = chatOf(payload);
            Json join_req = Json::objectOf(
                {{"type", Json("join_request")},
                 {"request_id", Json(join_id)},
                 {"chat_id", Json(chat_id)}});
            join_req.set("local_sdp",
                         Json("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=stub\r\n"
                              "t=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                              "a=mid:0\r\n"));
            if (!writeFrame(fd, join_req.dump())) break;
            std::string resp_frame;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(5);
            int64_t group_call_id = 12345;
            if (readFrameUntil(fd, resp_frame, deadline)) {
                try {
                    Json resp = Json::parse(resp_frame);
                    const Json* ok = resp.find("ok");
                    if (ok && ok->asBool()) {
                        const Json* data = resp.find("data");
                        if (data) {
                            const Json* g = data->find("group_call_id");
                            if (g && g->isInt())
                                group_call_id = g->asInt();
                        }
                        std::cerr << "[stub] JoinResponse ok, "
                                     "group_call_id="
                                  << group_call_id << "\n";
                    } else {
                        std::cerr << "[stub] JoinResponse not ok\n";
                    }
                } catch (...) {
                    std::cerr << "[stub] bad JoinResponse frame\n";
                }
            } else {
                std::cerr << "[stub] JoinResponse timeout\n";
            }
            Json data = Json::objectOf(
                {{"joined", Json(true)},
                 {"group_call_id", Json(group_call_id)},
                 {"now_playing", cannedNowPlaying(chat_id)}});
            if (!writeFrame(fd, okResponse(request_id, data).dump())) break;
            continue;
        }

        Json data;
        if (answer(action, payload, data)) {
            if (!writeFrame(fd, okResponse(request_id, data).dump())) break;
            // vc.play: push a TrackStarted event after answering.
            if (action == "vc.play") {
                Json track_data = Json::objectOf(
                    {{"track", cannedTrack()},
                     {"chat_id", Json(chatOf(payload))}});
                Json event = Json::objectOf(
                    {{"type", Json("event")},
                     {"event", Json("TrackStarted")},
                     {"chat_id", Json(chatOf(payload))}});
                event.set("data", std::move(track_data));
                if (!writeFrame(fd, event.dump())) break;
            }
        } else {
            std::cerr << "[stub] unknown action " << action << "\n";
            if (!writeFrame(fd,
                            errResponse(request_id, "not_found",
                                        "unknown action " + action)
                                .dump()))
                break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_socket_path = argc > 1 ? argv[1] : "/tmp/parlay-stub-backend.sock";
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    ::unlink(g_socket_path.c_str());

    int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (g_socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "socket path too long\n";
        return 1;
    }
    std::memcpy(addr.sun_path, g_socket_path.data(), g_socket_path.size());
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        std::perror("bind");
        return 1;
    }
    if (::listen(listener, 4) != 0) {
        std::perror("listen");
        return 1;
    }
    std::cout << "listening on " << g_socket_path << "\n";
    std::cout.flush();
    while (!g_stop) {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }
        std::cerr << "[stub] client connected\n";
        serve(fd);
        ::close(fd);
        std::cerr << "[stub] client disconnected\n";
    }
    ::close(listener);
    ::unlink(g_socket_path.c_str());
    return 0;
}
