// dispatcher_test.cpp — Dispatcher: /play from replied-to media, /mute,
// /join (Phase 5, Worker B); /autoplay-skip, /broadcast errors.txt
// (Phase 6, Worker B); /speed + /bass core-voice filter path,
// engine honest replies, /speedtest helpers (Phase 6, Worker C);
// /play through an engine attaches core audio, skip/pause/resume/
// seek/stop keep the core stream in step, watcher follow pass
// (Phase 7, Worker A1b).
//
// - TelegramApi::parseMessage: reply with audio / document / video_note
//   file_id is captured; a reply with no media captures nothing.
// - onPlay: reply with audio/document file_id downloads (fake) and plays
//   through the core voice backend (source "file"); no media + no query
//   answers the honest usage error; download/backend failures are
//   reported honestly, never faked.
// - onMute: core-owned session -> real voice_.mute()/unmute(); engine
//   session -> honest refusal (no vc.mute in the engines); nothing
//   playing -> "Nothing is playing."
// - onJoin: admin-gated; backend join or the honest reason.
//
// Build: g++ -std=c++20 -I src tests/dispatcher_test.cpp
//   src/dispatcher.cpp src/telegram_api.cpp src/ipc.cpp
//   src/engine_router.cpp src/store.cpp src/scram.cpp src/json.cpp
//   src/config.cpp src/http.cpp src/voice_call.cpp src/tdlib_client.cpp
//   -o /tmp/dispatcher_test -ldl && /tmp/dispatcher_test
// Exit 0 = all checks pass. No network: Telegram/IPC/voice are faked,
// the Store uses its JSON file backend in a temp dir.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "config.hpp"
#include "dispatcher.hpp"
#include "engine_router.hpp"
#include "ipc.hpp"
#include "json.hpp"
#include "store.hpp"
#include "telegram_api.hpp"
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

// --- fakes -------------------------------------------------------------------

class FakeTelegramApi : public parlay::TelegramApi {
public:
    explicit FakeTelegramApi(const parlay::Config& cfg)
        : parlay::TelegramApi(cfg) {}
    std::vector<std::string> replies;
    std::vector<int64_t> admins = {111};
    bool download_ok = true;
    std::string download_saved =
        "/tmp/parlay-dispatcher-test/replied/BQACAgAAA.ogg";
    // Broadcast fan-out scripting (Phase 6, B11): forward/copy fail
    // for the chats in forward_fail; sendDocument records the
    // attachment (or fails when doc_ok is false).
    std::set<int64_t> forward_fail;
    bool doc_ok = true;
    struct DocCall {
        int64_t chat;
        std::string filename;
        std::string content;
        std::string caption;
    };
    std::vector<DocCall> docs;
    // Phase 9 interactive layer: every new virtual records its call
    // instead of hitting the network. sendMessageHtml / sendPhoto push
    // the text (or caption) into replies so the existing replies[0]
    // assertions keep working; markups are recorded in parallel.
    std::vector<std::string> markups;  // reply_markup JSON per reply
    std::vector<std::string> photos;   // photo URL/file_id per sendPhoto
    struct EditCall {
        int64_t chat;
        int64_t msg;
        std::string text;
        std::string kind;  // text | caption | markup
    };
    std::vector<EditCall> edits;
    struct ToastCall {
        std::string id;
        std::string text;
        bool alert;
    };
    std::vector<ToastCall> toasts;
    std::vector<std::pair<int64_t, int64_t>> deleted;
    struct AudioCall {
        int64_t chat;
        std::string audio;  // file_id/URL (sendAudio) or path (sendAudioFile/sendFile)
        std::string caption;
        std::string kind;  // audio | audiofile | file
    };
    std::vector<AudioCall> audios;
    int64_t next_id = 1000;
    int64_t sendMessageHtml(int64_t chat_id, const std::string& text,
                            const parlay::Json& reply_markup, int64_t reply_to = 0,
                            std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        markups.push_back(reply_markup.dump());
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendPhoto(int64_t chat_id, const std::string& photo,
                      const std::string& caption, const parlay::Json& reply_markup,
                      std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)error_out;
        photos.push_back(photo);
        replies.push_back(caption);
        markups.push_back(reply_markup.dump());
        return static_cast<int64_t>(replies.size());
    }
    bool editMessageText(int64_t chat_id, int64_t message_id,
                         const std::string& text, const parlay::Json& reply_markup,
                         std::string* error_out = nullptr) override {
        (void)reply_markup;
        (void)error_out;
        edits.push_back({chat_id, message_id, text, "text"});
        return true;
    }
    bool editMessageCaption(int64_t chat_id, int64_t message_id,
                            const std::string& caption,
                            const parlay::Json& reply_markup,
                            std::string* error_out = nullptr) override {
        (void)reply_markup;
        (void)error_out;
        edits.push_back({chat_id, message_id, caption, "caption"});
        return true;
    }
    bool editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                const parlay::Json& reply_markup,
                                std::string* error_out = nullptr) override {
        (void)error_out;
        edits.push_back({chat_id, message_id, reply_markup.dump(), "markup"});
        return true;
    }
    bool answerCallbackQuery(const std::string& callback_query_id,
                             const std::string& text, bool show_alert,
                             int cache_time_sec = 0,
                             std::string* error_out = nullptr) override {
        (void)cache_time_sec;
        (void)error_out;
        toasts.push_back({callback_query_id, text, show_alert});
        return true;
    }
    // Inline answers (Phase 9 gaps, §13): the results JSON + cache
    // time + switch_pm button are recorded for the assertions.
    struct InlineCall {
        std::string id;
        std::string results_json;
        int cache_time;
        std::string switch_pm_text;
        std::string switch_pm_parameter;
    };
    std::vector<InlineCall> inlines;
    bool answerInlineQuery(const std::string& inline_query_id,
                           const parlay::Json& results, int cache_time_sec,
                           const std::string& switch_pm_text,
                           const std::string& switch_pm_parameter,
                           std::string* error_out = nullptr) override {
        (void)error_out;
        inlines.push_back({inline_query_id, results.dump(), cache_time_sec,
                           switch_pm_text, switch_pm_parameter});
        return true;
    }
    bool deleteMessage(int64_t chat_id, int64_t message_id) override {
        deleted.emplace_back(chat_id, message_id);
        return true;
    }
    int64_t sendAudioFile(int64_t chat_id, const std::string& file_path,
                          const std::string& caption,
                          std::string* error_out = nullptr) override {
        (void)error_out;
        audios.push_back({chat_id, file_path, caption, "audiofile"});
        return ++next_id;
    }
    int64_t sendFile(int64_t chat_id, const std::string& file_path,
                     const std::string& caption,
                     std::string* error_out = nullptr) override {
        (void)error_out;
        audios.push_back({chat_id, file_path, caption, "file"});
        return ++next_id;
    }
    int64_t sendAudio(int64_t chat_id, const std::string& audio,
                      const std::string& caption, const parlay::Json& reply_markup,
                      std::string* error_out = nullptr) override {
        (void)reply_markup;
        (void)error_out;
        audios.push_back({chat_id, audio, caption, "audio"});
        return ++next_id;
    }
    std::string getUserMention(int64_t user_id) override {
        return "User " + std::to_string(user_id);
    }
    bool forwardMessage(int64_t to_chat, int64_t from_chat,
                        int64_t message_id) override {
        (void)from_chat;
        (void)message_id;
        return forward_fail.count(to_chat) == 0;
    }
    bool copyMessage(int64_t to_chat, int64_t from_chat,
                     int64_t message_id) override {
        (void)from_chat;
        (void)message_id;
        return forward_fail.count(to_chat) == 0;
    }
    int64_t sendDocument(int64_t chat_id, const std::string& filename,
                         const std::string& content,
                         const std::string& caption,
                         std::string* error_out) override {
        if (!doc_ok) {
            if (error_out) *error_out = "simulated upload failure";
            return -1;
        }
        docs.push_back({chat_id, filename, content, caption});
        return 99;
    }
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to,
                          std::string* error_out) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        return static_cast<int64_t>(replies.size());
    }
    std::vector<int64_t> getChatAdministrators(int64_t chat_id) override {
        (void)chat_id;
        return admins;
    }
    // Scripted member status for non-admin users (Phase 9 §K): the
    // default "member" stands in for any non-admin state; tests set
    // it to "kicked"/"restricted" to exercise the assistant checks.
    std::string member_status_for_other = "member";
    std::string getChatMemberStatus(int64_t chat_id, int64_t user_id) override {
        (void)chat_id;
        return user_id == 111 ? "administrator" : member_status_for_other;
    }
    bool downloadFile(const std::string& file_id,
                      const std::string& dest_dir,
                      std::string& saved_path_out,
                      std::string& error_out) override {
        (void)file_id;
        (void)dest_dir;
        if (!download_ok) {
            error_out = "simulated download failure";
            return false;
        }
        saved_path_out = download_saved;
        return true;
    }
};

class FakeIpc : public parlay::IpcClient {
public:
    explicit FakeIpc(const parlay::Config& cfg) : parlay::IpcClient(cfg) {}
    mutable std::vector<std::string> posts;  // "base|action"
    mutable std::vector<std::string> gets;   // path
    // Scripted autoplay-skip answer (Phase 6): empty = success with a
    // canned mix pick; non-empty = fail with this message (bad_request).
    mutable std::string autoplay_skip_error;
    // Scripted autoplay-skip success payload (Phase 7, A1b): when
    // set, the canned pick below is replaced by this JSON (used to
    // give the pick a stream_url for the core re-attach test).
    mutable std::string autoplay_skip_response;
    // Scripted engine answers (Phase 7, A1b): empty = fall through
    // to the 404 below. Each is a full response body
    // ({"now_playing": ...}) returned verbatim on success.
    mutable std::string play_response;
    mutable std::string skip_response;
    mutable std::string pause_response;
    mutable std::string resume_response;
    // Scripted Phase 9-gaps answers (empty = fall through to the 404
    // below, per the FakeIpc contract above): search.inline tracks,
    // vc.lyrics text, NetEase search/lyrics, download POSTs, and the
    // radio status GET. last_post_body records the most recent POST
    // body so tests can assert the chat_id the core passed.
    mutable std::string inline_response;
    mutable std::string lyrics_response;
    mutable std::string netease_search_response;
    mutable std::string netease_lyrics_response;
    mutable std::string download_response;
    mutable std::string radio_status_response;
    mutable std::string last_post_body;
    // Scripted playback state (Phase 6, B11 speed/bass engine path):
    // when true, GET now-playing returns a playing track and POST
    // seek succeeds; when false (default) the engines know nothing.
    mutable bool np_ok = false;
    // Scripted now-playing track (Phase 7, A1b follow tests): the id /
    // stream URL / position the GET now-playing answer reports.
    mutable std::string np_track_id = "t1";
    mutable std::string np_stream_url = "https://example.invalid/audio.webm";
    mutable int64_t np_position = 30;
    parlay::IpcResult post(const std::string& base_url,
                           const std::string& action, const parlay::Json& body,
                           int timeout_sec = 30) const override {
        (void)timeout_sec;
        posts.push_back(base_url + "|" + action);
        last_post_body = body.dump();
        parlay::IpcResult r;
        if (action == "inline" && !inline_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(inline_response);
            return r;
        }
        if (action == "lyrics" && !lyrics_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(lyrics_response);
            return r;
        }
        if (action == "netease/search" &&
            !netease_search_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(netease_search_response);
            return r;
        }
        if (action == "netease/lyrics" &&
            !netease_lyrics_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(netease_lyrics_response);
            return r;
        }
        if (action.rfind("download/", 0) == 0 &&
            !download_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(download_response);
            return r;
        }
        if (action == "seek" && np_ok) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(
                R"({"now_playing":{"track":{"title":"T","duration_sec":200},"position_sec":30,"paused":false}})");
            return r;
        }
        if (action == "play" && !play_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(play_response);
            return r;
        }
        if (action == "skip" && !skip_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(skip_response);
            return r;
        }
        // Queue actions (Phase 9 §K): list returns an empty queue,
        // clear succeeds. Enough for /clear's empty-queue reply and
        // for the queued-play fallback (which guards size() > 1).
        if (action == "queue") {
            const parlay::Json* a = body.find("action");
            std::string act = a ? a->asString() : "";
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(
                act == "list" ? R"({"queue":[]})" : R"({})");
            return r;
        }
        if (action == "pause" && !pause_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(pause_response);
            return r;
        }
        if (action == "resume" && !resume_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(resume_response);
            return r;
        }
        if (action == "autoplay-skip") {
            if (!autoplay_skip_error.empty()) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = autoplay_skip_error;
                return r;
            }
            if (!autoplay_skip_response.empty()) {
                r.ok = true;
                r.http_status = 200;
                r.data = parlay::Json::parse(autoplay_skip_response);
                return r;
            }
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(
                R"({"now_playing":{"track":{"title":"Mix Pick","artist":"Mix Artist","duration_sec":200}}})");
            return r;
        }
        r.http_status = 404;
        r.error_code = "not_found";
        r.error_message = "fake: no engine here";
        return r;
    }
    parlay::IpcResult get(const std::string& base_url,
                          const std::string& path,
                          int timeout_sec = 30) const override {
        (void)base_url;
        (void)timeout_sec;
        gets.push_back(path);
        parlay::IpcResult r;
        if (path.rfind("/v1/radio/status", 0) == 0 &&
            !radio_status_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(radio_status_response);
            return r;
        }
        if (np_ok && path.rfind("/v1/now-playing", 0) == 0) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(
                "{\"now_playing\":{\"track\":{\"id\":\"" + np_track_id +
                "\",\"title\":\"T\",\"duration_sec\":200,\"stream_url\":\"" +
                np_stream_url +
                "\"},\"stream_url\":\"" + np_stream_url +
                "\",\"position_sec\":" + std::to_string(np_position) +
                ",\"paused\":false}}");
            return r;
        }
        // no now-playing state in this fake
        r.http_status = 404;
        r.error_code = "not_found";
        r.error_message = "fake: nothing playing";
        return r;
    }
    bool healthz(const std::string& base_url, int timeout_sec = 5) const override {
        (void)base_url;
        (void)timeout_sec;
        return true;
    }
};

class FakeVoice : public parlay::VoiceCall {
public:
    bool avail = true;
    bool play_ok = true, join_ok = true, mute_ok = true, unmute_ok = true;
    bool pause_ok = true, resume_ok = true;
    std::string fail_reason = "fake voice failure";
    struct PlayCall {
        int64_t chat;
        std::string url;
        std::string title;
        bool video;
        int seek_sec = 0;
        double speed = 1.0;
        int bass = 0;
    };
    std::vector<PlayCall> plays;
    std::vector<int64_t> joins, mutes, unmutes, stops, pauses, resumes;
    // Scripted position for time() (Phase 6, B11): -1 = unknown.
    int64_t time_value = -1;
    bool play(int64_t chat_id, const std::string& stream_url,
              const std::string& title, bool video, int seek_sec, double speed,
              int bass) override {
        plays.push_back({chat_id, stream_url, title, video, seek_sec, speed,
                         bass});
        if (!play_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    bool join(int64_t chat_id) override {
        joins.push_back(chat_id);
        if (!join_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    bool stop(int64_t chat_id) override {
        stops.push_back(chat_id);
        return true;
    }
    bool pause(int64_t chat_id) override {
        pauses.push_back(chat_id);
        if (!pause_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    bool resume(int64_t chat_id) override {
        resumes.push_back(chat_id);
        if (!resume_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    bool mute(int64_t chat_id) override {
        mutes.push_back(chat_id);
        if (!mute_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    bool unmute(int64_t chat_id) override {
        unmutes.push_back(chat_id);
        if (!unmute_ok) {
            last_error_ = fail_reason;
            return false;
        }
        return true;
    }
    int64_t time(int64_t chat_id) override {
        (void)chat_id;
        return time_value;
    }
    bool available() const override { return avail; }
    std::string describe() const override { return "fake-voice"; }
    const char* name() const override { return "fake"; }
    std::string lastError() const override { return last_error_; }

private:
    std::string last_error_;
};

// --- helpers -----------------------------------------------------------------

parlay::TgMessage makeMsg(int64_t chat, int64_t from,
                          const std::string& text) {
    parlay::TgMessage m;
    m.message_id = 1;
    m.from.id = from;
    m.chat.id = chat;
    m.chat.type = "supergroup";
    m.text = text;
    m.has_text = true;
    return m;
}

struct Rig {
    parlay::Config cfg;
    parlay::Store store;
    FakeTelegramApi tg;
    FakeIpc ipc;
    parlay::EngineRouter router;
    FakeVoice voice;
    parlay::Dispatcher d;
    Rig()
        : cfg([] {
              parlay::Config c;
              c.data_dir = "/tmp/parlay-dispatcher-test";
              c.owner_id = 999;
              return c;
          }()),
          store(cfg),
          tg(cfg),
          ipc(cfg),
          router(cfg, &store),
          d(cfg, tg, ipc, router, store, voice, "parlaybot") {}
};

}  // namespace

int main() {

    // --- A. parseMessage: reply media ------------------------------------
    {
        parlay::Json j = parlay::Json::parse(
            R"({"message_id":10,"from":{"id":111},"chat":{"id":-1001,"type":"supergroup"},"text":"/play","reply_to_message":{"message_id":5,"from":{"id":222},"audio":{"file_id":"AUDIO123","title":"Song","performer":"Artist"}}})");
        parlay::TgMessage m = parlay::TelegramApi::parseMessage(j);
        CHECK(m.has_reply_to, "parse: reply detected");
        CHECK(m.has_reply_media, "parse: reply audio detected");
        CHECK(m.reply_media_file_id == "AUDIO123",
              "parse: reply audio file_id");
        CHECK(m.reply_media_kind == "audio", "parse: reply audio kind");
        CHECK(m.reply_media_title == "Artist - Song",
              "parse: reply audio title (performer - title)");
    }
    {
        parlay::Json j = parlay::Json::parse(
            R"({"message_id":10,"from":{"id":111},"chat":{"id":-1001,"type":"supergroup"},"text":"/play","reply_to_message":{"message_id":5,"from":{"id":222},"document":{"file_id":"DOC456","file_name":"track.mp3"}}})");
        parlay::TgMessage m = parlay::TelegramApi::parseMessage(j);
        CHECK(m.has_reply_media && m.reply_media_file_id == "DOC456",
              "parse: reply document file_id");
        CHECK(m.reply_media_kind == "document",
              "parse: reply document kind");
        CHECK(m.reply_media_title == "track.mp3",
              "parse: reply document title (file_name)");
    }
    {
        parlay::Json j = parlay::Json::parse(
            R"({"message_id":10,"from":{"id":111},"chat":{"id":-1001,"type":"supergroup"},"text":"/play","reply_to_message":{"message_id":5,"from":{"id":222},"video_note":{"file_id":"VN789"}}})");
        parlay::TgMessage m = parlay::TelegramApi::parseMessage(j);
        CHECK(m.has_reply_media && m.reply_media_file_id == "VN789" &&
                  m.reply_media_kind == "video_note",
              "parse: reply video_note file_id + kind");
    }
    {
        parlay::Json j = parlay::Json::parse(
            R"({"message_id":10,"from":{"id":111},"chat":{"id":-1001,"type":"supergroup"},"text":"/play","reply_to_message":{"message_id":5,"from":{"id":222},"text":"hello"}})");
        parlay::TgMessage m = parlay::TelegramApi::parseMessage(j);
        CHECK(m.has_reply_to && !m.has_reply_media,
              "parse: reply with no media captures nothing");
    }
    {
        parlay::Json j = parlay::Json::parse(
            R"({"message_id":10,"from":{"id":111},"chat":{"id":-1001,"type":"supergroup"},"text":"/play"})");
        parlay::TgMessage m = parlay::TelegramApi::parseMessage(j);
        CHECK(!m.has_reply_to && !m.has_reply_media,
              "parse: no reply captures nothing");
    }

    // --- B. onPlay: replied media -----------------------------------------
    {
        Rig rig;
        parlay::TgMessage m = makeMsg(-1001, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        rig.d.handleMessage(m);
        CHECK(rig.voice.plays.size() == 1,
              "play reply audio: voice backend play called once");
        CHECK(!rig.voice.plays.empty() &&
                  rig.voice.plays[0].url == rig.tg.download_saved,
              "play reply audio: plays the downloaded file path");
        CHECK(!rig.voice.plays.empty() &&
                  rig.voice.plays[0].title == "Artist - Song",
              "play reply audio: track title from reply");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing") &&
                  contains(rig.tg.replies[0], "Artist - Song"),
              "play reply audio: Now playing reply");
        CHECK(rig.ipc.posts.empty(),
              "play reply audio: no vc.play IPC call (core voice path)");
    }
    {
        Rig rig;
        parlay::TgMessage m = makeMsg(-1002, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "DOC456";
        m.reply_media_kind = "document";
        m.reply_media_title = "track.mp3";
        rig.d.handleMessage(m);
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].title == "track.mp3",
              "play reply document: voice backend play with file title");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing"),
              "play reply document: Now playing reply");
    }
    {
        Rig rig;
        parlay::TgMessage m = makeMsg(-1003, 111, "/play");
        rig.d.handleMessage(m);
        // Phase 9: the usage error is the ported kNoSongQuery card.
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Whoops! Nothing to play") &&
                  contains(rig.tg.replies[0], "/play [song name or link]"),
              "play no media + no query: honest usage error");
        CHECK(rig.voice.plays.empty() && rig.ipc.posts.empty(),
              "play no media + no query: nothing played, no IPC");
    }
    {
        Rig rig;
        rig.tg.download_ok = false;
        parlay::TgMessage m = makeMsg(-1004, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Song";
        rig.d.handleMessage(m);
        CHECK(rig.voice.plays.empty(),
              "play reply audio, download fails: no voice play");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not fetch"),
              "play reply audio, download fails: honest error");
    }
    {
        Rig rig;
        rig.voice.play_ok = false;
        rig.voice.fail_reason = "fake voice failure";
        parlay::TgMessage m = makeMsg(-1005, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Song";
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not play") &&
                  contains(rig.tg.replies[0], "fake voice failure"),
              "play reply audio, backend fails: honest error, no fake success");
    }

    // --- C. onMute ----------------------------------------------------------
    {
        Rig rig;
        rig.store.hset("chat:-1011", "voice_owner", "core");
        rig.store.hset("chat:-1011", "muted", "0");
        rig.d.handleMessage(makeMsg(-1011, 111, "/mute"));
        CHECK(rig.voice.mutes.size() == 1,
              "mute core session: backend mute() called");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "\U0001F507 | <b>Muted.</b>",
              "mute core session: Muted reply");
        auto v = rig.store.hget("chat:-1011", "muted");
        CHECK(v && *v == "1", "mute core session: flag stored");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1012", "voice_owner", "core");
        rig.store.hset("chat:-1012", "muted", "1");
        rig.d.handleMessage(makeMsg(-1012, 111, "/mute"));
        CHECK(rig.voice.mutes.empty(),
              "mute when already muted: no backend call");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "\u274C | <b>Already muted.</b>",
              "mute when already muted: Already muted reply");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1013", "voice_owner", "core");
        rig.store.hset("chat:-1013", "muted", "1");
        rig.d.handleMessage(makeMsg(-1013, 111, "/unmute"));
        CHECK(rig.voice.unmutes.size() == 1,
              "unmute core session: backend unmute() called");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "\U0001F50A | <b>Unmuted.</b>",
              "unmute core session: Unmuted reply");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1014", "voice_owner", "go");
        rig.store.set("call:-1014", "1");
        rig.d.handleMessage(makeMsg(-1014, 111, "/mute"));
        CHECK(rig.voice.mutes.empty(),
              "mute engine session: backend NOT called");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "cannot mute"),
              "mute engine session: honest refusal, no fake mute");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1015, 111, "/mute"));
        CHECK(rig.voice.mutes.empty(),
              "mute nothing playing: no backend call");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "mute nothing playing: honest reply");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1016", "voice_owner", "core");
        rig.store.hset("chat:-1016", "muted", "0");
        rig.voice.mute_ok = false;
        rig.voice.fail_reason = "ntgcalls mute failed";
        rig.d.handleMessage(makeMsg(-1016, 111, "/mute"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not mute") &&
                  contains(rig.tg.replies[0], "ntgcalls mute failed"),
              "mute backend fails: honest error, no fake success");
        auto v = rig.store.hget("chat:-1016", "muted");
        CHECK(v && *v == "0", "mute backend fails: flag untouched");
    }

    // --- D. onJoin ----------------------------------------------------------
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1021, 111, "/join"));
        CHECK(rig.voice.joins.size() == 1,
              "join: backend join() called");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "\u2705 | <b>Joined the voice chat.</b>",
              "join: Joined reply");
        auto v = rig.store.hget("chat:-1021", "voice_owner");
        CHECK(v && *v == "core", "join: voice_owner recorded as core");
        auto c = rig.store.get("call:-1021");
        CHECK(c && *c == "1", "join: call marked active");
    }
    {
        Rig rig;
        rig.voice.avail = false;
        rig.d.handleMessage(makeMsg(-1022, 111, "/join"));
        CHECK(rig.voice.joins.empty(),
              "join backend unavailable: no join call");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Cannot join the voice chat"),
              "join backend unavailable: honest reason");
    }
    {
        Rig rig;
        rig.voice.join_ok = false;
        rig.voice.fail_reason = "tdlib not authorized";
        rig.d.handleMessage(makeMsg(-1023, 111, "/join"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not join") &&
                  contains(rig.tg.replies[0], "tdlib not authorized"),
              "join backend fails: honest error");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1024", "voice_owner", "core");
        rig.store.set("call:-1024", "1");
        rig.d.handleMessage(makeMsg(-1024, 111, "/join"));
        CHECK(rig.voice.joins.empty(),
              "join when already in call: no second join");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] ==
                      "\u274C | <b>Already in the voice chat.</b>",
              "join when already in call: Already reply");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1025", "voice_owner", "go");
        rig.store.set("call:-1025", "1");
        rig.d.handleMessage(makeMsg(-1025, 111, "/join"));
        CHECK(rig.voice.joins.empty(),
              "join when engine session active: no backend join");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Already in the voice chat"),
              "join when engine session active: Already reply");
    }

    // --- E. onAutoplaySkip (Phase 6) --------------------------------------
    {
        Rig rig;
        rig.store.set("call:-1031", "1");
        rig.d.handleMessage(makeMsg(-1031, 111, "/autoplay-skip"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|autoplay-skip") != std::string::npos,
              "autoplay-skip: vcCall autoplay-skip issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Autoplay skipped") &&
                  contains(rig.tg.replies[0], "Mix Pick"),
              "autoplay-skip: picked track reported");
    }
    {
        Rig rig;
        rig.store.set("call:-1032", "1");
        rig.d.handleMessage(makeMsg(-1032, 111, "/askip"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|autoplay-skip") != std::string::npos,
              "askip alias: same vcCall autoplay-skip");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1033, 111, "/autoplay-skip"));
        CHECK(rig.ipc.posts.empty(),
              "autoplay-skip nothing playing: no engine call");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "autoplay-skip nothing playing: honest reply");
    }
    {
        Rig rig;
        rig.store.set("call:-1034", "1");
        rig.ipc.autoplay_skip_error = "autoplay is off in this chat";
        rig.d.handleMessage(makeMsg(-1034, 111, "/autoplay-skip"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Service error") &&
                  contains(rig.tg.replies[0], "autoplay is off"),
              "autoplay-skip engine 400: honest error, no fake pick");
    }

    // --- F. onBroadcast: errors.txt attachment (Phase 6, B11) ------------
    // Each block wipes the shared store dir first: Rig instances share
    // /tmp/parlay-dispatcher-test, and "chats" is a set that would
    // otherwise leak targets between blocks. /broadcast does NOT add
    // the source chat itself (only /start and /play record chats), so
    // the fan-out targets are exactly the seeded chats.
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.store.sadd("chats", "-1036");
        rig.store.sadd("chats", "-1037");
        rig.tg.forward_fail = {-1036};
        parlay::TgMessage m = makeMsg(-1035, 999, "/broadcast");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 2 &&
                  contains(rig.tg.replies[0], "Broadcasting to 2"),
              "broadcast: announces 2 seeded targets");
        CHECK(contains(rig.tg.replies[1], "1 delivered, 1 failed"),
              "broadcast: summary counts delivered/failed");
        CHECK(!contains(rig.tg.replies[1], "Failed ids"),
              "broadcast: failures NOT inline in the summary");
        CHECK(rig.tg.docs.size() == 1,
              "broadcast: one errors.txt attachment sent");
        CHECK(!rig.tg.docs.empty() &&
                  rig.tg.docs[0].chat == -1035 &&
                  rig.tg.docs[0].filename == "errors.txt" &&
                  contains(rig.tg.docs[0].content, "-1036\n") &&
                  !contains(rig.tg.docs[0].content, "-1037\n") &&
                  contains(rig.tg.docs[0].content,
                           "no per-chat error text") &&
                  contains(rig.tg.docs[0].caption, "1 chats"),
              "broadcast: errors.txt to the source chat lists only the failed id, with the no-per-chat-error header");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.store.sadd("chats", "-1036");
        rig.store.sadd("chats", "-1037");
        parlay::TgMessage m = makeMsg(-1035, 999, "/broadcast");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        rig.d.handleMessage(m);
        CHECK(rig.tg.docs.empty(),
              "broadcast all delivered: no errors.txt sent");
        CHECK(rig.tg.replies.size() == 2 &&
                  contains(rig.tg.replies[1], "2 delivered, 0 failed"),
              "broadcast all delivered: summary 2/0");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.store.sadd("chats", "-1036");
        rig.tg.forward_fail = {-1036};
        rig.tg.doc_ok = false;
        parlay::TgMessage m = makeMsg(-1035, 999, "/broadcast");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        rig.d.handleMessage(m);
        CHECK(rig.tg.docs.empty(),
              "broadcast doc upload fails: no attachment recorded");
        CHECK(rig.tg.replies.size() == 2 &&
                  contains(rig.tg.replies[1], "Failed ids (errors.txt upload failed:") &&
                  contains(rig.tg.replies[1], "-1036") &&
                  contains(rig.tg.replies[1], "simulated upload failure"),
              "broadcast doc upload fails: honest inline fallback");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.d.handleMessage(makeMsg(-1035, 999, "/broadcast"));
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Reply to a message to broadcast it.",
              "broadcast without reply: usage error, no fan-out");
    }

    // --- G. /speed + /bass: core voice applies, engines honestly don't --
    // (Phase 6, B11). Core-owned session = voice_owner "core" + call
    // active + a recorded core_source; the filter is re-attached
    // through voice_.play with the stored speed/bass at the current
    // position (voice_.time). Engine sessions keep the seek-restart
    // but the reply says the value was only saved.
    {
        Rig rig;
        rig.store.set("call:-1041", "1");
        rig.store.hset("chat:-1041", "voice_owner", "core");
        rig.store.hset("chat:-1041", "core_source", "/tmp/song.ogg");
        rig.store.hset("chat:-1041", "core_title", "Song");
        rig.store.hset("chat:-1041", "core_video", "0");
        rig.voice.time_value = 42;
        rig.d.handleMessage(makeMsg(-1041, 111, "/speed 1.5"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url == "/tmp/song.ogg" &&
                  rig.voice.plays[0].speed == 1.5 &&
                  rig.voice.plays[0].seek_sec == 42 &&
                  rig.voice.plays[0].bass == 0,
              "speed core session: voice re-attached at 42s with 1.5x");
        CHECK(rig.ipc.posts.empty(),
              "speed core session: no engine seek call");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Speed set to 1.5x") &&
                  contains(rig.tg.replies[0], "core voice re-attached") &&
                  contains(rig.tg.replies[0], "0:42"),
              "speed core session: applied reply names the position");
    }
    {
        Rig rig;
        rig.store.set("call:-1042", "1");
        rig.store.hset("chat:-1042", "voice_owner", "core");
        rig.store.hset("chat:-1042", "core_source", "/tmp/song.ogg");
        rig.store.hset("chat:-1042", "core_title", "Song");
        rig.store.hset("chat:-1042", "speed", "1.25");
        rig.voice.time_value = 90;
        rig.d.handleMessage(makeMsg(-1042, 111, "/bass 40"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].bass == 40 &&
                  rig.voice.plays[0].speed == 1.25 &&
                  rig.voice.plays[0].seek_sec == 90,
              "bass core session: voice re-attached with bass 40 + stored speed 1.25x");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Bass set to 40"),
              "bass core session: applied reply");
    }
    {
        Rig rig;
        rig.store.set("call:-1043", "1");
        rig.store.hset("chat:-1043", "voice_owner", "core");
        rig.voice.time_value = 10;
        rig.d.handleMessage(makeMsg(-1043, 111, "/speed 1.5"));
        CHECK(rig.voice.plays.empty(),
              "speed core session, no source: no re-attach attempted");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "no source was recorded"),
              "speed core session, no source: honest reply, no fake success");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1044", "voice_owner", "core");
        rig.store.hset("chat:-1044", "core_source", "/tmp/song.ogg");
        rig.d.handleMessage(makeMsg(-1044, 111, "/bass 30"));
        CHECK(rig.voice.plays.empty() &&
                  rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "bass core session, call inactive: nothing playing");
    }
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.d.handleMessage(makeMsg(-1045, 111, "/speed 1.5"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|seek") != std::string::npos,
              "speed engine session: seek-restart issued to the engine");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "1.5x saved") &&
                  contains(rig.tg.replies[0],
                           "cannot apply audio filters"),
              "speed engine session: honest saved-not-applied reply");
        CHECK(rig.voice.plays.empty(),
              "speed engine session: core voice untouched");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1046", "speed", "1.5");
        rig.store.hset("chat:-1046", "bass", "40");
        parlay::TgMessage m = makeMsg(-1046, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        rig.d.handleMessage(m);
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].speed == 1.5 &&
                  rig.voice.plays[0].bass == 40,
              "play replied file: stored speed/bass passed to core voice");
        auto fields = rig.store.hgetall("chat:-1046");
        CHECK(fields["core_source"] == rig.tg.download_saved &&
                  fields["core_title"] == "Artist - Song" &&
                  fields["voice_owner"] == "core",
              "play replied file: core source recorded for later /speed+/bass");
    }

    // --- I. /play through an engine attaches core audio (Phase 7, A1b) --
    // The stub engine answers vc.play with a resolved track carrying
    // stream_url (top-level + track-level, A8). The dispatcher must
    // hand that URL to the core voice backend — the engine's own
    // "play" never reaches a call — and record the session as
    // core-owned. A failed attach stops the engine again and reports
    // the exact error instead of a "Now playing" lie. Skip /
    // autoplay-skip / pause / resume / seek / stop / the watcher
    // follow pass keep the core stream in step the same way.
    const std::string play_np =
        R"({"now_playing":{"track":{"id":"t1","title":"Test Song","artist":"Test Artist","duration_sec":200,"stream_url":"https://example.invalid/audio.webm"},"stream_url":"https://example.invalid/audio.webm","position_sec":0,"paused":false}})";
    const std::string skip_np =
        R"({"now_playing":{"track":{"id":"t2","title":"Next Song","artist":"Next Artist","duration_sec":180,"stream_url":"https://example.invalid/next.webm"},"position_sec":0,"paused":false}})";
    auto has_post = [](const Rig& rig, const std::string& action) {
        for (const auto& p : rig.ipc.posts)
            if (p.find("|" + action) != std::string::npos) return true;
        return false;
    };
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1051, 111, "/play Test Song"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/audio.webm" &&
                  rig.voice.plays[0].title == "Test Song" &&
                  !rig.voice.plays[0].video &&
                  rig.voice.plays[0].seek_sec == 0 &&
                  rig.voice.plays[0].speed == 1.0 &&
                  rig.voice.plays[0].bass == 0,
              "play query: core voice plays the engine-resolved stream_url");
        CHECK(has_post(rig, "play"),
              "play query: vc.play issued to the engine first");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing") &&
                  contains(rig.tg.replies[0], "Test Song"),
              "play query: Now playing reply");
        auto fields = rig.store.hgetall("chat:-1051");
        CHECK(fields["voice_owner"] == "core" &&
                  fields["core_source"] ==
                      "https://example.invalid/audio.webm" &&
                  fields["core_title"] == "Test Song" &&
                  fields["core_track_id"] == "t1" &&
                  fields["core_follow"] == "1",
              "play query: session recorded as core-owned with source + track id");
        auto c = rig.store.get("call:-1051");
        CHECK(c && *c == "1", "play query: call marked active");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.store.hset("chat:-1052", "speed", "1.5");
        rig.store.hset("chat:-1052", "bass", "40");
        rig.d.handleMessage(makeMsg(-1052, 111, "/play Test Song"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].speed == 1.5 &&
                  rig.voice.plays[0].bass == 40,
              "play query: stored speed/bass passed to core voice");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.voice.play_ok = false;
        rig.voice.fail_reason = "tdlib not authorized";
        rig.d.handleMessage(makeMsg(-1053, 111, "/play Test Song"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "Could not play in the voice chat") &&
                  contains(rig.tg.replies[0], "tdlib not authorized") &&
                  !contains(rig.tg.replies[0], "Now playing"),
              "play query, attach fails: honest error, no fake Now playing");
        CHECK(has_post(rig, "stop"),
              "play query, attach fails: engine playback stopped again");
        auto c = rig.store.get("call:-1053");
        CHECK(!c || *c != "1",
              "play query, attach fails: call not marked active");
        auto v = rig.store.hget("chat:-1053", "voice_owner");
        CHECK(!v || v->empty(),
              "play query, attach fails: no voice owner recorded");
    }
    {
        Rig rig;
        rig.ipc.play_response =
            R"({"now_playing":{"track":{"id":"t9","title":"No URL Song","duration_sec":200},"position_sec":0,"paused":false}})";
        rig.d.handleMessage(makeMsg(-1054, 111, "/play No URL Song"));
        CHECK(rig.voice.plays.empty(),
              "play query, no stream_url: core voice never called");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "Could not play in the voice chat") &&
                  contains(rig.tg.replies[0], "no stream_url"),
              "play query, no stream_url: honest error naming the missing URL");
        CHECK(has_post(rig, "stop"),
              "play query, no stream_url: engine playback stopped again");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1055, 111, "/vplay Test Song"));
        CHECK(rig.voice.plays.size() == 1 && rig.voice.plays[0].video,
              "vplay: core voice plays with video");
        auto v = rig.store.hget("chat:-1055", "core_video");
        CHECK(v && *v == "1", "vplay: video flag recorded");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1068, 111, "/playforce Test Song"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/audio.webm",
              "playforce: replacement track attached to core voice");
        CHECK(has_post(rig, "stop"),
              "playforce: old engine playback stopped first");
    }
    {
        Rig rig;
        rig.ipc.skip_response = skip_np;
        rig.store.set("call:-1056", "1");
        rig.store.hset("chat:-1056", "voice_owner", "core");
        rig.store.hset("chat:-1056", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1056", "core_title", "Test Song");
        rig.store.hset("chat:-1056", "core_video", "0");
        rig.store.hset("chat:-1056", "core_track_id", "t1");
        rig.store.hset("chat:-1056", "core_follow", "1");
        rig.d.handleMessage(makeMsg(-1056, 111, "/skip"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/next.webm" &&
                  rig.voice.plays[0].title == "Next Song",
              "skip: new track's stream_url re-attached to core voice");
        auto fields = rig.store.hgetall("chat:-1056");
        CHECK(fields["core_source"] ==
                      "https://example.invalid/next.webm" &&
                  fields["core_track_id"] == "t2",
              "skip: recorded source + track id updated to the new track");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Skipped") &&
                  contains(rig.tg.replies[0], "Next Song"),
              "skip: new track reported");
    }
    {
        Rig rig;
        rig.ipc.skip_response =
            R"({"now_playing":{"track":null,"queue":[],"position_sec":0,"paused":false}})";
        rig.store.set("call:-1057", "1");
        rig.store.hset("chat:-1057", "voice_owner", "core");
        rig.store.hset("chat:-1057", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1057", "core_title", "Test Song");
        rig.store.hset("chat:-1057", "core_follow", "1");
        rig.d.handleMessage(makeMsg(-1057, 111, "/skip"));
        CHECK(!rig.voice.stops.empty() && rig.voice.stops[0] == -1057,
              "skip to empty queue: core voice stopped");
        auto c = rig.store.get("call:-1057");
        CHECK(!c || *c != "1", "skip to empty queue: call inactive");
        auto v = rig.store.hget("chat:-1057", "core_source");
        CHECK(!v || v->empty(),
              "skip to empty queue: core source cleared");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "queue is now empty"),
              "skip to empty queue: honest reply");
    }
    {
        Rig rig;
        rig.ipc.autoplay_skip_response =
            R"({"now_playing":{"track":{"id":"m1","title":"Mix Pick","artist":"Mix Artist","duration_sec":200,"stream_url":"https://example.invalid/mix.webm"},"position_sec":0,"paused":false}})";
        rig.store.set("call:-1069", "1");
        rig.store.hset("chat:-1069", "voice_owner", "core");
        rig.store.hset("chat:-1069", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1069", "core_title", "Test Song");
        rig.store.hset("chat:-1069", "core_track_id", "t1");
        rig.store.hset("chat:-1069", "core_follow", "1");
        rig.d.handleMessage(makeMsg(-1069, 111, "/autoplay-skip"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/mix.webm",
              "autoplay-skip: picked track's stream_url re-attached");
    }
    {
        Rig rig;
        rig.ipc.pause_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":true}})";
        rig.store.set("call:-1058", "1");
        rig.store.hset("chat:-1058", "voice_owner", "core");
        rig.store.hset("chat:-1058", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1058", "core_title", "Test Song");
        rig.d.handleMessage(makeMsg(-1058, 111, "/pause"));
        CHECK(rig.voice.pauses.size() == 1 && rig.voice.pauses[0] == -1058,
              "pause core session: backend pause() called");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Paused"),
              "pause core session: Paused reply");
    }
    {
        // Fresh Rig: the dispatcher rate-limits to 1 cmd/s per chat,
        // so a second command in the same Rig right after /pause is
        // silently dropped. The store is shared, so the seeded
        // core-streaming session survives.
        Rig rig;
        rig.ipc.resume_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":false}})";
        rig.store.set("call:-1058", "1");
        rig.store.hset("chat:-1058", "voice_owner", "core");
        rig.store.hset("chat:-1058", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1058", "core_title", "Test Song");
        rig.d.handleMessage(makeMsg(-1058, 111, "/resume"));
        CHECK(rig.voice.resumes.size() == 1 &&
                  rig.voice.resumes[0] == -1058,
              "resume core session: backend resume() called");
    }
    {
        Rig rig;
        rig.ipc.pause_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":true}})";
        rig.store.set("call:-1059", "1");
        rig.store.hset("chat:-1059", "voice_owner", "core");
        rig.store.hset("chat:-1059", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1059", "core_title", "Test Song");
        rig.voice.pause_ok = false;
        rig.voice.fail_reason = "ntgcalls pause failed";
        rig.d.handleMessage(makeMsg(-1059, 111, "/pause"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Paused") &&
                  contains(rig.tg.replies[0], "could not pause") &&
                  contains(rig.tg.replies[0], "ntgcalls pause failed"),
              "pause core backend fails: honest note, no silent claim");
    }
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.store.set("call:-1060", "1");
        rig.store.hset("chat:-1060", "voice_owner", "core");
        rig.store.hset("chat:-1060", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1060", "core_title", "Test Song");
        rig.store.hset("chat:-1060", "core_video", "0");
        rig.d.handleMessage(makeMsg(-1060, 111, "/seek 10"));
        CHECK(has_post(rig, "seek"),
              "seek core session: engine seek issued");
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/audio.webm" &&
                  rig.voice.plays[0].seek_sec == 40,
              "seek core session: core voice re-attached at 40s");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Seeked forward to 0:40"),
              "seek core session: seek reply");
    }
    {
        Rig rig;
        rig.store.set("call:-1061", "1");
        rig.store.hset("chat:-1061", "voice_owner", "core");
        rig.store.hset("chat:-1061", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1061", "core_title", "Test Song");
        rig.store.hset("chat:-1061", "core_track_id", "t1");
        rig.store.hset("chat:-1061", "core_follow", "1");
        rig.d.handleMessage(makeMsg(-1061, 111, "/stop"));
        CHECK(!rig.voice.stops.empty(),
              "stop core session: backend stop() called");
        auto fields = rig.store.hgetall("chat:-1061");
        CHECK(fields["voice_owner"].empty() &&
                  fields["core_source"].empty() &&
                  fields["core_track_id"].empty(),
              "stop core session: core-stream fields cleared");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.store.set("playlist:111",
                      R"([{"title":"Test Song","artist":"Test Artist"},{"title":"Second Song","artist":"Other"}])");
        rig.d.handleMessage(makeMsg(-1067, 111, "/playplaylist"));
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/audio.webm",
              "playplaylist: first track attached to core voice");
        CHECK(has_post(rig, "queue"),
              "playplaylist: remaining tracks queued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Playing your playlist"),
              "playplaylist: playlist reply");
    }
    // --- J. watchCoreFollow: engine-internal auto-advance -------------
    // The follow pass runs over store keys("call:"), which the Rig
    // instances share — wipe the dir first (broadcast-test pattern)
    // so stale sessions from earlier blocks cannot interfere.
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.ipc.np_position = 5;
        rig.store.set("call:-1062", "1");
        rig.store.hset("chat:-1062", "voice_owner", "core");
        rig.store.hset("chat:-1062", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1062", "core_title", "Test Song");
        rig.store.hset("chat:-1062", "core_track_id", "t1");
        rig.store.hset("chat:-1062", "core_pos", "150");
        rig.store.hset("chat:-1062", "core_follow", "1");
        rig.d.watchCoreFollow();
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/next.webm" &&
                  rig.voice.plays[0].seek_sec == 5,
              "follow: engine advance to t2 re-attached at 5s");
        auto v = rig.store.hget("chat:-1062", "core_track_id");
        CHECK(v && *v == "t2",
              "follow: recorded track id updated to t2");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t1";
        rig.ipc.np_stream_url = "https://example.invalid/audio.webm";
        rig.ipc.np_position = 35;
        rig.store.set("call:-1063", "1");
        rig.store.hset("chat:-1063", "voice_owner", "core");
        rig.store.hset("chat:-1063", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1063", "core_title", "Test Song");
        rig.store.hset("chat:-1063", "core_track_id", "t1");
        rig.store.hset("chat:-1063", "core_pos", "30");
        rig.store.hset("chat:-1063", "core_follow", "1");
        rig.d.watchCoreFollow();
        CHECK(rig.voice.plays.empty(),
              "follow: same track, position forward — no re-attach");
        auto v = rig.store.hget("chat:-1063", "core_pos");
        CHECK(v && *v == "35",
              "follow: position scratch updated to 35");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t1";
        rig.ipc.np_stream_url = "https://example.invalid/audio.webm";
        rig.ipc.np_position = 8;
        rig.store.set("call:-1064", "1");
        rig.store.hset("chat:-1064", "voice_owner", "core");
        rig.store.hset("chat:-1064", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1064", "core_title", "Test Song");
        rig.store.hset("chat:-1064", "core_track_id", "t1");
        rig.store.hset("chat:-1064", "core_pos", "150");
        rig.store.hset("chat:-1064", "core_follow", "1");
        rig.d.watchCoreFollow();
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].seek_sec == 8,
              "follow: same id, position jumped back — loop copy re-attached at 8s");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.ipc.np_position = 5;
        rig.voice.play_ok = false;
        rig.voice.fail_reason = "tdlib not authorized";
        rig.store.set("call:-1065", "1");
        rig.store.hset("chat:-1065", "voice_owner", "core");
        rig.store.hset("chat:-1065", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1065", "core_title", "Test Song");
        rig.store.hset("chat:-1065", "core_track_id", "t1");
        rig.store.hset("chat:-1065", "core_pos", "150");
        rig.store.hset("chat:-1065", "core_follow", "1");
        rig.d.watchCoreFollow();
        CHECK(rig.voice.plays.size() == 1 &&
                  rig.voice.plays[0].url ==
                      "https://example.invalid/next.webm",
              "follow failure: re-attach was attempted with the new URL");
        CHECK(!rig.voice.stops.empty(),
              "follow failure: core voice stopped");
        auto c = rig.store.get("call:-1065");
        CHECK(!c || *c != "1", "follow failure: call not active");
        CHECK(has_post(rig, "stop"),
              "follow failure: engine playback stopped");
    }
    {
        std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.ipc.np_position = 5;
        rig.store.set("call:-1066", "1");
        rig.store.hset("chat:-1066", "voice_owner", "core");
        rig.store.hset("chat:-1066", "core_source", "/tmp/song.ogg");
        rig.store.hset("chat:-1066", "core_title", "Song");
        rig.store.hset("chat:-1066", "core_track_id", "");
        rig.store.hset("chat:-1066", "core_follow", "0");
        rig.d.watchCoreFollow();
        CHECK(rig.voice.plays.empty(),
              "follow: replied-file session (core_follow 0) never re-attached");
    }

    // --- H. /speedtest pure helpers (Phase 6, B11) ------------------------
    {
        CHECK(parlay::Dispatcher::speedMbps(25000000, 10.0) == 20.0,
              "speedMbps: 25 MB in 10 s = 20 Mbps");
        CHECK(parlay::Dispatcher::speedMbps(0, 5.0) == 0.0,
              "speedMbps: 0 bytes = 0 Mbps");
        CHECK(parlay::Dispatcher::speedMbps(1000000, 0.0) == 0.0,
              "speedMbps: 0 seconds = 0 Mbps, no division by zero");
        CHECK(parlay::Dispatcher::formatSpeedtest(20.0, true, 10.0, 200,
                                                  "") ==
                  "Speed test:\nDownload: 20.00 Mbps\nUpload: 10.00 Mbps",
              "formatSpeedtest: up+down both reported");
        CHECK(contains(parlay::Dispatcher::formatSpeedtest(
                           20.0, false, 0.0, 0, "curl timeout"),
                       "Upload probe failed (HTTP 0: curl timeout)"),
              "formatSpeedtest: failed upload reported honestly next to download");
    }

    // --- J. callback routing (Phase 9, pl: namespace) -------------------
    auto makeCq = [](const std::string& data, int64_t chat = -1001,
                     int64_t user = 111, int64_t msg_id = 7) {
        parlay::TgCallbackQuery cq;
        cq.id = "cq1";
        cq.from.id = user;
        cq.chat_id = chat;
        cq.message_id = msg_id;
        cq.has_message = true;
        cq.data = data;
        return cq;
    };
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:unknown:thing"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].alert &&
                  contains(rig.tg.toasts[0].text, "Unknown"),
              "callback: unknown domain gets a polite alert, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq(""));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert,
              "callback: empty data gets an alert, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:player:pause:" +
                                    std::string(70, '9')));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert,
              "callback: over-64-byte data gets an alert, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("not-a-callback"));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert,
              "callback: non-pl data gets an alert, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:help:main"));
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Help") &&
                  rig.tg.toasts.size() == 1 &&
                  !rig.tg.toasts[0].alert,
              "callback: pl:help:main re-renders the help menu in place");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:help:nosuchtopic"));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert,
              "callback: unknown help topic gets an alert");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:player:progress:-1001"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text.empty() &&
                  !rig.tg.toasts[0].alert && rig.tg.edits.empty(),
              "callback: progress button is a silent no-op ack");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:close"));
        CHECK(rig.tg.deleted.size() == 1 &&
                  rig.tg.deleted[0].first == -1001 &&
                  rig.tg.deleted[0].second == 7,
              "callback: pl:close deletes the message");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:set:info:adminmode"));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert &&
                  contains(rig.tg.toasts[0].text, "Admin Mode"),
              "callback: pl:set:info answers the explainer as an alert");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:player:pause:-1001"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  contains(rig.tg.toasts[0].text, "Nothing playing"),
              "callback: pause with nothing playing toasts, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:queue:more:-1001"));
        CHECK(rig.tg.toasts.size() == 1,
              "callback: queue more with no playback answers (no crash)");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:lang:select:help"));
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Language"),
              "callback: language picker renders in place");
    }
    {
        // Panel presses are requester-only and silently ignored.
        Rig rig;
        rig.store.set("panelmsg:-1001", "7");
        rig.store.set("paneluser:-1001", "111");
        rig.d.handleCallback(makeCq("pl:player:pause:-1001", -1001, 222));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text.empty() && rig.tg.edits.empty(),
              "callback: another user's panel press is silently ignored");
    }
    {
        // /menu panel creation stores panelmsg/paneluser.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/menu"));
        auto pm = rig.store.get("panelmsg:-1001");
        auto pu = rig.store.get("paneluser:-1001");
        CHECK(pm && *pm == "1" && pu && *pu == "111",
              "menu: panel message + requester recorded");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Nothing streaming now"),
              "menu: idle panel text");
        CHECK(!rig.tg.markups.empty() &&
                  contains(rig.tg.markups[0], "pl:player:pause:-1001"),
              "menu: panel keyboard carries player callbacks");
    }
    {
        // /start in a group renders the group card; /help in a group
        // replies with the DM prompt (scope is Any now).
        Rig rig;
        parlay::TgMessage m = makeMsg(-1001, 111, "/start");
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Parlay"),
              "start: group card renders");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "/help"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "DM"),
              "help: group reply points at the DM");
    }
    {
        // /settings renders the panel with the settings keyboard.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/settings"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Chat Settings"),
              "settings: panel text renders");
        CHECK(!rig.tg.markups.empty() &&
                  contains(rig.tg.markups[0], "pl:set:adminmode"),
              "settings: panel keyboard carries set callbacks");
    }
    {
        // /authlist with no authorized users renders the empty text.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/authlist"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "no authorized users"),
              "authlist: empty list renders");
    }
    {
        // /id renders the chat id.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/id"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "-1001"),
              "id: chat id rendered");
    }
    {
        // /speed range is 0.5–4.0 now (single atempo covers it).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/speed 3.5"));
        CHECK(rig.tg.replies.size() == 1 &&
                  !contains(rig.tg.replies[0], "0.5x to 2.0x"),
              "speed: 3.5x accepted (range is 0.5-4.0)");
    }

    // --- K. Phase 9 wiring round 2: inventory strings in handlers -------
    auto makeDm = [](int64_t chat, int64_t from, const std::string& text) {
        parlay::TgMessage m = makeMsg(chat, from, text);
        m.chat.type = "private";
        return m;
    };
    {
        // adminCheck denial uses the inventory card.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 222, "/settings"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Access Denied"),
              "adminCheck: non-admin gets the denial card");
    }
    {
        // canManageVc denial uses the admin-or-auth card.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 222, "/pause"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "authorized users"),
              "canManageVc: non-admin gets the admin-or-auth denial");
    }
    {
        // playAllowed denial uses the admin-only-play card.
        Rig rig;
        rig.store.hset("chat:-1001", "play_mode", "admin");
        rig.d.handleMessage(makeMsg(-1001, 222, "/play test song"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Admin only play"),
              "playAllowed: admin-only playmode denies non-admins");
    }
    {
        // /eval with no input answers the inventory one-word reply.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/eval"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "What?"),
              "eval: empty input answers What?");
    }
    {
        // /eval runs and renders the HTML output form.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/eval echo hi"));
        CHECK(rig.tg.replies.size() == 2 &&
                  contains(rig.tg.replies[1], "Output:") &&
                  contains(rig.tg.replies[1], "hi") &&
                  contains(rig.tg.replies[1], "Exit: 0"),
              "eval: output form renders input/output/exit");
    }
    {
        // /logs with no log file falls back to the in-memory log, which
        // already holds this very command (logged before dispatch).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/logs"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Recent commands") &&
                  contains(rig.tg.replies[0], "/logs"),
              "logs: memory log lists the recent commands");
    }
    {
        // /logger usage shows the status; enable needs LOGGER_ID.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/logger"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /logger") &&
                  contains(rig.tg.replies[0], "ON"),
              "logger: usage shows current status");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/logger on"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "LOGGER_ID"),
              "logger: enable without LOGGER_ID refuses");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/logger off"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Logger disabled"),
              "logger: disable confirms");
    }
    {
        // /admincache reloads once, then cools down for 10 minutes.
        // One Rig per call: the dispatcher rate-limits to 1 cmd/s per
        // chat, and the cooldown map is static (survives a fresh Rig).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/admincache"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "Administrator cache reloaded successfully"),
              "admincache: first reload succeeds");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "/admincache"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "Please wait"),
              "admincache: immediate re-run cools down");
    }
    {
        // /activevc + /ac with no calls, and with one call. The call
        // flag lives in the file store, so it survives a fresh Rig
        // (the store writes through on every set).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 999, "/activevc"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "active streams"),
              "activevc: no streams renders the empty text");
        rig.store.set("call:-1001", "1");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 999, "/activevc"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "List of active streams") &&
                  contains(rig2.tg.replies[0], "-1001"),
              "activevc: one call lists its chat");
        Rig rig3;
        rig3.d.handleMessage(makeMsg(-1001, 999, "/ac"));
        CHECK(rig3.tg.replies.size() == 1 &&
                  contains(rig3.tg.replies[0], "Active Chats:"),
              "ac: count form renders the active-chats card");
        rig3.store.del("call:-1001");
    }
    {
        // /bug usage + a report to the owner (owner_id 999 in the Rig).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/bug"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /bug"),
              "bug: empty report shows usage");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "/bug the queue broke"));
        CHECK(rig2.tg.replies.size() == 2 &&
                  contains(rig2.tg.replies[0], "Bug report") &&
                  contains(rig2.tg.replies[1], "Thank you"),
              "bug: report delivered + thanks");
    }
    {
        // /channelplay: usage, disable, invalid ids, inaccessible.
        // One Rig per call (1 cmd/s per chat rate limit).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/channelplay"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /channelplay"),
              "channelplay: bare shows usage");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "/channelplay disable"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "Channel Play disabled"),
              "channelplay: disable confirms");
        Rig rig3;
        rig3.d.handleMessage(makeMsg(-1001, 111, "/channelplay abc"));
        CHECK(rig3.tg.replies.size() == 1 &&
                  contains(rig3.tg.replies[0], "Invalid Target"),
              "channelplay: bad id is an invalid target");
        Rig rig4;
        rig4.d.handleMessage(makeMsg(-1001, 111, "/channelplay 123"));
        CHECK(rig4.tg.replies.size() == 1 &&
                  contains(rig4.tg.replies[0], "Invalid Target"),
              "channelplay: non-channel id is an invalid target");
        Rig rig5;
        rig5.d.handleMessage(makeMsg(-1001, 111, "/channelplay -10042"));
        CHECK(rig5.tg.replies.size() == 1 &&
                  contains(rig5.tg.replies[0], "Not Accessible"),
              "channelplay: bot-not-admin is not accessible");
    }
    {
        // /userbot: assistant already here / banned.
        Rig rig;
        rig.cfg.assistant_ids.insert(555);
        rig.d.handleMessage(makeMsg(-1001, 111, "/userbot"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "already in this chat"),
              "userbot: member assistant is already here");
        Rig rig2;
        rig2.cfg.assistant_ids.insert(555);
        rig2.tg.member_status_for_other = "kicked";
        rig2.d.handleMessage(makeMsg(-1001, 111, "/userbot"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "banned"),
              "userbot: kicked assistant is restricted");
    }
    {
        // /setrtmp is DM-only; bare / invalid / save / off in a DM.
        // One Rig per call (1 cmd/s per chat); the RTMP target lives
        // in the file store and survives a fresh Rig.
        Rig rig;
        rig.d.handleMessage(makeDm(111, 111, "/setrtmp"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "No RTMP target is set"),
              "setrtmp: bare shows the unset text");
        Rig rig2;
        rig2.d.handleMessage(makeDm(111, 111, "/setrtmp http://x/app key"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "rtmp://"),
              "setrtmp: non-rtmp URL refused");
        Rig rig3;
        rig3.d.handleMessage(
            makeDm(111, 111, "/setrtmp rtmp://live.example.com/app key1"));
        CHECK(rig3.tg.replies.size() == 1 &&
                  contains(rig3.tg.replies[0], "RTMP target saved"),
              "setrtmp: save confirms");
        Rig rig4;
        rig4.d.handleMessage(makeDm(111, 111, "/setrtmp off"));
        CHECK(rig4.tg.replies.size() == 1 &&
                  contains(rig4.tg.replies[0], "removed"),
              "setrtmp: off removes the target");
    }
    {
        // /stream / /streamstop / /streamstatus with no target/stream.
        // One Rig per call (1 cmd/s per chat rate limit).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/stream"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "No RTMP target is set"),
              "stream: no target refuses");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "/streamstop"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "No stream is running"),
              "streamstop: nothing running");
        Rig rig3;
        rig3.d.handleMessage(makeMsg(-1001, 111, "/streamstatus"));
        CHECK(rig3.tg.replies.size() == 1 &&
                  contains(rig3.tg.replies[0], "No active RTMP stream"),
              "streamstatus: nothing active");
        rig3.store.hset("chat:-1001", "rtmp_url", "rtmp://x/app");
        rig3.store.hset("chat:-1001", "rtmp_key", "k");
        Rig rig4;
        rig4.d.handleMessage(makeMsg(-1001, 111, "/stream"));
        CHECK(rig4.tg.replies.size() == 1 &&
                  contains(rig4.tg.replies[0], "Usage:") &&
                  contains(rig4.tg.replies[0], "/stream"),
              "stream: no query + nothing playing shows usage");
        rig4.store.hset("chat:-1001", "rtmp_url", "");
        rig4.store.hset("chat:-1001", "rtmp_key", "");
    }
    {
        // /queue help renders the ops card.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/queue help"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Queue Managing") &&
                  contains(rig.tg.replies[0], "remove"),
              "queue help: ops card renders");
    }
    {
        // /myplaylists renders the named-playlist line format.
        Rig rig;
        rig.store.set(
            "npl:111",
            R"([{"id":"abc123","name":"Road","songs":[]}])");
        rig.d.handleMessage(makeMsg(-1001, 111, "/myplaylists"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Road (<code>abc123</code>)"),
              "myplaylists: name + id line renders");
    }
    {
        // /clear with an empty queue answers the already-empty text.
        Rig rig;
        rig.store.set("call:-1001", "1");
        rig.d.handleMessage(makeMsg(-1001, 111, "/clear"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "already empty"),
              "clear: empty queue answers already-empty");
    }
    {
        // delete: trigger validates the path before confirming.
        // One Rig per call (1 cmd/s per chat rate limit).
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "delete:Song.mp3"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "top-level"),
              "delete trigger: bare name is top-level");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1001, 111, "delete:../x.mp3"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0], "Invalid path"),
              "delete trigger: traversal is an invalid path");
        Rig rig3;
        rig3.d.handleMessage(
            makeMsg(-1001, 111, "delete:Artist/Album/song.mp3"));
        CHECK(rig3.tg.replies.size() == 1 &&
                  contains(rig3.tg.replies[0], "song.mp3"),
              "delete trigger: library path shows the confirm card");
    }
    {
        // pl:bcast:cancel: non-owner refused, owner with no run.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:bcast:cancel", -1001, 111));
        CHECK(rig.tg.toasts.size() == 1 &&
                  contains(rig.tg.toasts[0].text, "Only the bot owner"),
              "bcast cancel: non-owner refused");
        rig.d.handleCallback(makeCq("pl:bcast:cancel", -1001, 999));
        CHECK(rig.tg.toasts.size() == 2 &&
                  contains(rig.tg.toasts[1].text, "Cancelled"),
              "bcast cancel: owner with no run gets the cancelled toast");
    }
    {
        // pl:player:skip success toasts the pressing user by name.
        // The skip re-checks now-playing after the engine call, so the
        // fake needs a playing track + a core session to re-attach.
        Rig rig;
        rig.ipc.skip_response = skip_np;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.store.set("call:-1001", "1");
        rig.store.hset("chat:-1001", "voice_owner", "core");
        rig.store.hset("chat:-1001", "core_source",
                       "https://example.invalid/audio.webm");
        rig.store.hset("chat:-1001", "core_title", "Test Song");
        rig.store.hset("chat:-1001", "core_video", "0");
        rig.store.hset("chat:-1001", "core_track_id", "t1");
        rig.store.hset("chat:-1001", "core_follow", "1");
        rig.d.handleCallback(makeCq("pl:player:skip:-1001"));
        CHECK(!rig.tg.toasts.empty() &&
                  contains(rig.tg.toasts[0].text, "Skipped by"),
              "player skip: success toasts the pressing user");
    }
    {
        // pl:search pick with no session edits the message to expired.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:search:1:111:tok123"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  contains(rig.tg.toasts[0].text, "expired") &&
                  !rig.tg.edits.empty() &&
                  contains(rig.tg.edits[0].text, "Search expired"),
              "search pick: expired session edits the message");
    }
    {
        // pl:set:page / pl:set:musicdl sub-screens + toggles.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:set:page", -1001, 111));
        CHECK(!rig.tg.edits.empty() &&
                  contains(rig.tg.edits[0].text,
                           "Auto delete old messages"),
              "set page: screen renders");
        rig.d.handleCallback(makeCq("pl:set:page:0", -1001, 111));
        auto v = rig.store.hget("chat:-1001", "page_purge");
        CHECK(v && *v == "off" &&
                  !rig.tg.toasts.empty() &&
                  contains(rig.tg.toasts.back().text, "updated"),
              "set page:0 persists off + confirms");
        rig.d.handleCallback(makeCq("pl:set:musicdl", -1001, 111));
        CHECK(contains(rig.tg.edits.back().text,
                       "Download music via chat"),
              "set musicdl: screen renders");
    }

    // --- L. Phase 9 gaps: inline queries, deep link, lrc_path, share
    // --- dedup removal, radio card (coordinator rulings) --------------
    auto makeIq = [](const std::string& query, int64_t user = 111,
                     const std::string& id = "iq1") {
        parlay::TgInlineQuery iq;
        iq.id = id;
        iq.from.id = user;
        iq.query = query;
        return iq;
    };
    auto posted = [](const Rig& rig, const std::string& action) {
        for (const auto& p : rig.ipc.posts)
            if (contains(p, "|" + action)) return true;
        return false;
    };
    // Field of one inline result ("" when missing), and the message
    // text of one article result.
    auto resField = [](const parlay::Json& res, size_t i,
                       const char* key) {
        if (!res.isArray() || i >= res.size()) return std::string("");
        const parlay::Json* v = res.asArray()[i].find(key);
        return v ? v->asString() : std::string("");
    };
    auto artMsg = [](const parlay::Json& res, size_t i) {
        if (!res.isArray() || i >= res.size()) return std::string("");
        const parlay::Json* c =
            res.asArray()[i].find("input_message_content");
        if (!c) return std::string("");
        const parlay::Json* m = c->find("message_text");
        return m ? m->asString() : std::string("");
    };
    auto inlineResults = [](const Rig& rig) {
        return parlay::Json::parse(rig.tg.inlines.back().results_json);
    };
    // A group text from this user records last_chat:<user> = -1001
    // (the np/s/l chat), without touching the command rate limiter.
    auto noteChat = [](Rig& rig, int64_t user) {
        rig.d.handleMessage(makeMsg(-1001, user, "hello"));
    };
    {
        // Hint: empty and 1-2-char queries get the hint button.
        Rig rig;
        rig.d.handleInlineQuery(makeIq(""));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]" &&
                  rig.tg.inlines[0].switch_pm_text ==
                      parlay::ui_texts::kInlineHintText &&
                  rig.tg.inlines[0].switch_pm_parameter == "help" &&
                  rig.tg.inlines[0].cache_time == 30,
              "inline hint: empty query -> hint button, cache 30");
        rig.d.handleInlineQuery(makeIq("ab"));
        CHECK(rig.tg.inlines.size() == 2 &&
                  rig.tg.inlines[1].switch_pm_text ==
                      parlay::ui_texts::kInlineHintText,
              "inline hint: 2-char query -> hint button");
        CHECK(rig.ipc.posts.empty(),
              "inline hint: no engine call for hint queries");
    }
    {
        // Help articles + bare "search".
        Rig rig;
        rig.d.handleInlineQuery(makeIq("help"));
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 2 &&
                  resField(res, 0, "title") ==
                      parlay::ui_texts::kInlineHelpTitle1 &&
                  resField(res, 1, "title") ==
                      parlay::ui_texts::kInlineHelpTitle2 &&
                  artMsg(res, 0) == "Parlay v0.2.0" &&
                  artMsg(res, 1) == "Parlay v0.2.0" &&
                  rig.tg.inlines[0].cache_time == 3600,
              "inline help: two articles sending the version line");
        rig.d.handleInlineQuery(makeIq("search"));
        res = inlineResults(rig);
        CHECK(res.size() == 1 &&
                  resField(res, 0, "title") ==
                      parlay::ui_texts::kInlineNeedKeyword,
              "inline search: bare search -> enter-a-keyword article");
    }
    {
        // np with no known chat: empty results, no engine call.
        Rig rig;
        rig.d.handleInlineQuery(makeIq("np", 4242));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]" &&
                  rig.ipc.posts.empty(),
              "inline np: unknown chat -> empty, engine not called");
    }
    {
        // np with a known chat: chat_id passed, audio result.
        Rig rig;
        noteChat(rig, 5001);
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","duration_sec":200,"stream_url":"https://example.invalid/audio.webm","thumbnail":"https://example.invalid/t.jpg","source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("np", 5001));
        CHECK(posted(rig, "inline") &&
                  contains(rig.ipc.last_post_body,
                           "\"chat_id\":-1001"),
              "inline np: chat_id passed to search.inline");
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 1 && resField(res, 0, "type") == "audio" &&
                  resField(res, 0, "audio_url") ==
                      "https://example.invalid/audio.webm" &&
                  resField(res, 0, "title") == "T" &&
                  rig.tg.inlines[0].cache_time == 0,
              "inline np: audio result from stream_url, cache 0");
    }
    {
        // np with nothing playing: empty results.
        Rig rig;
        noteChat(rig, 5002);
        rig.ipc.inline_response = R"({"results":[]})";
        rig.d.handleInlineQuery(makeIq("np", 5002));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]",
              "inline np: engine empty -> empty results");
    }
    {
        // s: share article with the track URL.
        Rig rig;
        noteChat(rig, 5003);
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","url":"https://example.invalid/watch","source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("s", 5003));
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 1 && resField(res, 0, "type") == "article" &&
                  artMsg(res, 0) ==
                      "A — T\nhttps://example.invalid/watch",
              "inline s: share article with track URL");
    }
    {
        // l: lyrics fetched via /v1/lyrics with the same chat_id.
        Rig rig;
        noteChat(rig, 5004);
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","source":"youtube"}]})";
        rig.ipc.lyrics_response =
            R"({"lyrics":"[00:01.00] la\n[00:02.00] la la\n","title":"T","artist":"A","source":"lrclib","truncated":false})";
        rig.d.handleInlineQuery(makeIq("l", 5004));
        CHECK(posted(rig, "inline") && posted(rig, "lyrics") &&
                  contains(rig.ipc.last_post_body,
                           "\"chat_id\":-1001"),
              "inline l: lyrics fetched with the same chat_id");
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 1 &&
                  resField(res, 0, "title") == "Lyrics: T" &&
                  resField(res, 0, "description") == "A — la\nla la" &&
                  artMsg(res, 0) == "A — T\n\nla\nla la",
              "inline l: lyrics article, timestamps stripped");
    }
    {
        // l: long lyrics truncate to 4000 chars + "\n...".
        Rig rig;
        noteChat(rig, 5005);
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","source":"youtube"}]})";
        rig.ipc.lyrics_response =
            std::string("{\"lyrics\":\"") + std::string(5000, 'x') +
            std::string("\",\"title\":\"T\",\"artist\":\"A\",\"source\":")
            + "\"lrclib\",\"truncated\":false}";
        rig.d.handleInlineQuery(makeIq("l", 5005));
        parlay::Json res = inlineResults(rig);
        std::string msg = artMsg(res, 0);
        CHECK(contains(msg, "\n...") && !contains(msg, "…") &&
                  msg.size() ==
                      std::string("A — T\n\n").size() + 4000 + 4,
              "inline l: 5000-char lyrics cut to 4000 + newline-ellipsis");
    }
    {
        // l: no lyrics found -> empty results.
        Rig rig;
        noteChat(rig, 5006);
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("l", 5006));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]",
              "inline l: lyrics 404 -> empty results");
    }
    {
        // Search: audio result when a stream_url is known, article +
        // Open button otherwise; results stored for the deep link.
        Rig rig;
        rig.ipc.inline_response =
            R"({"results":[{"id":"jsvn:1","title":"With Stream","artist":"A","duration_sec":180,"stream_url":"https://example.invalid/a.mp3","source":"jiosaavn"},{"id":"lemonic:9","title":"No Stream","artist":"B","duration_sec":200,"url":"https://example.invalid/watch9","source":"lemonic"}]})";
        rig.d.handleInlineQuery(makeIq("bohemian"));
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 2 &&
                  resField(res, 0, "type") == "audio" &&
                  resField(res, 1, "type") == "article" &&
                  contains(rig.tg.inlines[0].results_json,
                           "https://t.me/parlaybot?start=spotify----"
                           "lemonic:9") &&
                  rig.tg.inlines[0].cache_time == 3600,
              "inline search: audio + article with Open deep link");
    }
    {
        // Search with no matches -> "No results found" article.
        Rig rig;
        rig.ipc.inline_response = R"({"results":[]})";
        rig.d.handleInlineQuery(makeIq("bohemian"));
        parlay::Json res = inlineResults(rig);
        CHECK(res.size() == 1 &&
                  resField(res, 0, "title") ==
                      parlay::ui_texts::kInlineNoResults &&
                  artMsg(res, 0) == "No results found",
              "inline search: no matches -> No results found article");
    }
    {
        // lib/del: no library backend -> empty results (ruling 1).
        Rig rig;
        rig.ipc.inline_response = R"({"results":[]})";
        rig.d.handleInlineQuery(makeIq("lib foo"));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]",
              "inline lib: empty results, not rendered functional");
        rig.d.handleInlineQuery(makeIq("del foo"));
        CHECK(rig.tg.inlines.size() == 2 &&
                  rig.tg.inlines[1].results_json == "[]",
              "inline del: empty results, not rendered functional");
    }
    {
        // Deep link: inline search stores the track; /start
        // spotify----{id} downloads it via the stored URL.
        std::string mp3 = "/tmp/parlay-dl-test.mp3";
        {
            std::ofstream out(mp3, std::ios::binary);
            out << "FAKEMP3";
        }
        Rig rig;
        rig.ipc.inline_response =
            R"({"results":[{"id":"lemonic:9","title":"No Stream","artist":"B","duration_sec":200,"url":"https://example.invalid/watch9","source":"lemonic"}]})";
        rig.ipc.download_response =
            std::string("{\"file_path\":\"") + mp3 +
            std::string("\",\"track\":{\"id\":\"lemonic:9\",\"title\":")
            + "\"No Stream\",\"artist\":\"B\",\"duration_sec\":200}}";
        rig.d.handleInlineQuery(makeIq("bohemian"));
        parlay::TgMessage pm = makeMsg(111, 111, "/start spotify----lemonic:9");
        pm.chat.type = "private";
        rig.d.handleMessage(pm);
        CHECK(posted(rig, "download/youtube") &&
                  contains(rig.ipc.last_post_body,
                           "https://example.invalid/watch9"),
              "deep link: downloads the stored inline track URL");
        bool sent = false;
        for (const auto& a : rig.tg.audios)
            if (a.kind == "audiofile" && a.audio == mp3) sent = true;
        CHECK(sent, "deep link: audio file sent");
        CHECK(!rig.tg.edits.empty() &&
                  rig.tg.edits.back().text ==
                      parlay::ui_texts::kDlSuccess,
              "deep link: progress edited to download success");
    }
    {
        // Deep link: yt: prefix fallback when nothing was stored.
        std::string mp3 = "/tmp/parlay-dl-test.mp3";
        Rig rig;
        rig.ipc.download_response =
            std::string("{\"file_path\":\"") + mp3 +
            std::string("\",\"track\":{\"id\":\"yt:dQw4w9WgXcQ\",")
            + "\"title\":\"T\",\"artist\":\"A\",\"duration_sec\":200}}";
        parlay::TgMessage pm =
            makeMsg(111, 111, "/start spotify----yt:dQw4w9WgXcQ");
        pm.chat.type = "private";
        rig.d.handleMessage(pm);
        CHECK(posted(rig, "download/youtube") &&
                  contains(rig.ipc.last_post_body,
                           "https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
              "deep link: yt: id resolves to the watch URL");
    }
    {
        // Deep link: lemonic: id with no stored result cannot resolve.
        Rig rig;
        parlay::TgMessage pm =
            makeMsg(111, 111, "/start spotify----lemonic:zzz");
        pm.chat.type = "private";
        rig.d.handleMessage(pm);
        CHECK(rig.ipc.posts.empty() &&
                  !rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(),
                           parlay::ui_texts::kDlDeepLinkExpired),
              "deep link: unresolvable id -> expired, no download");
    }
    {
        // lrc_path: /lyric sends the service-written .lrc file.
        Rig rig;
        rig.ipc.netease_search_response =
            R"({"results":[{"id":"186016","title":"Test Song"}]})";
        rig.ipc.netease_lyrics_response =
            R"({"lyrics":"[00:01.00] la\n","track_id":"186016","title":"Test Song","artist":"A","source":"netease","truncated":false,"lrc_path":"/tmp/parlay-lrc-test.lrc"})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/lyric Test Song"));
        bool sent = false;
        for (const auto& a : rig.tg.audios)
            if (a.kind == "file" &&
                a.audio == "/tmp/parlay-lrc-test.lrc" &&
                contains(a.caption, "<blockquote expandable>") &&
                contains(a.caption, "la"))
                sent = true;
        CHECK(sent, "lyrics: lrc_path file sent with stripped caption");
    }
    {
        // lrc_path absent: the core writes its own stripped .lrc.
        Rig rig;
        rig.ipc.netease_search_response =
            R"({"results":[{"id":"186016","title":"Test Song"}]})";
        rig.ipc.netease_lyrics_response =
            R"({"lyrics":"[00:01.00] la\n","track_id":"186016","title":"Test Song","artist":"A","source":"netease","truncated":false})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/lyric Test Song"));
        bool sent = false;
        for (const auto& a : rig.tg.audios)
            if (a.kind == "file" && contains(a.audio, "/lyrics/") &&
                a.audio.size() > 4 &&
                a.audio.substr(a.audio.size() - 4) == ".lrc")
                sent = true;
        CHECK(sent, "lyrics: no lrc_path -> core writes its own .lrc");
    }
    {
        // Share feedback/report without dedup (ruling 8): the second
        // feedback and the second report both go through — no
        // "already sent" / "already reported" anywhere.
        Rig rig;
        auto pmText = [](const std::string& text) {
            parlay::TgMessage m = makeMsg(111, 111, text);
            m.chat.type = "private";
            return m;
        };
        for (int round = 0; round < 2; ++round) {
            rig.d.handleCallback(
                makeCq("pl:share:feedback:777:55:FILEUID"));
            rig.d.handleMessage(pmText("great track!"));
        }
        int sent = 0, already = 0;
        for (const auto& r : rig.tg.replies) {
            if (r == parlay::ui_texts::kShareFeedbackSent) ++sent;
            if (r == parlay::ui_texts::kShareFeedbackAlready) ++already;
        }
        CHECK(sent == 2 && already == 0,
              "share feedback: sent twice, never already-sent");
        Rig rig2;
        rig2.d.handleCallback(makeCq("pl:share:report:777:55"));
        rig2.d.handleCallback(makeCq("pl:share:report:777:55"));
        int reported = 0, already_rep = 0;
        for (const auto& t : rig2.tg.toasts) {
            if (t.text == parlay::ui_texts::kShareReported) ++reported;
            if (t.text == parlay::ui_texts::kShareReportAlready)
                ++already_rep;
        }
        CHECK(reported == 2 && already_rep == 0,
              "share report: reported twice, never already-reported");
    }
    {
        // Radio card omits listener counts (ruling 9).
        Rig rig;
        rig.ipc.radio_status_response =
            R"({"now_playing":{"title":"Station Song"},"station":"Test FM","stream_url":"https://example.invalid/stream","reachable":true,"vc_handoff":false})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/manage"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies[0], "Your radio status") &&
                  !contains(rig.tg.replies[0], "Listener"),
              "radio manage: card has no listener counts");
    }
    {
        // Blacklisted user: inline gets empty results, silently.
        Rig rig;
        rig.store.sadd("bl_users", "222");
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("bohemian", 222));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]" &&
                  rig.ipc.posts.empty(),
              "inline auth: blacklisted user -> empty, silent");
    }
    {
        // Maintenance: non-sudo gets empty results; the owner goes
        // through. (Last: it leaves the maintenance flag set.)
        Rig rig;
        rig.store.set("maintenance", "1");
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("bohemian", 111));
        CHECK(rig.tg.inlines.size() == 1 &&
                  rig.tg.inlines[0].results_json == "[]" &&
                  rig.ipc.posts.empty(),
              "inline auth: maintenance + non-sudo -> empty, silent");
        rig.d.handleInlineQuery(makeIq("bohemian", 999));
        CHECK(rig.ipc.posts.size() == 1 && posted(rig, "inline"),
              "inline auth: maintenance + owner -> engine called");
        rig.store.set("maintenance", "0");
    }
    {
        // Auth limit is 25 (ArcMusic / TinaMusic / YukkiMusic-Go
        // MAX_AUTH_USERS default — the 20 the core used before was
        // an unverified assumption; NoxBeats defines no such limit).
        // Pre-fill the store instead of driving 25 /auth commands
        // through the per-chat rate limiter. Separate chats per
        // sub-check: the file store is shared across Rigs.
        Rig rig;
        for (int i = 0; i < 25; ++i)
            rig.store.sadd("auth:-1001", std::to_string(2000 + i));
        rig.d.handleMessage(makeMsg(-1001, 111, "/auth 3000"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies[0],
                           "maximum authorized users limit") &&
                  contains(rig.tg.replies[0], "(25)"),
              "auth limit: 25 users -> 26th refused with the 25 limit");
        Rig rig2;
        for (int i = 0; i < 24; ++i)
            rig2.store.sadd("auth:-1002", std::to_string(2000 + i));
        rig2.d.handleMessage(makeMsg(-1002, 111, "/auth 3000"));
        CHECK(!rig2.tg.replies.empty() &&
                  contains(rig2.tg.replies[0], "Now 25/25"),
              "auth limit: 24 users -> 25th added, count shows 25/25");
    }

    std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
    if (failures == 0) {
        std::cout << "dispatcher_test: ALL PASS\n";
        return 0;
    }
    std::cout << "dispatcher_test: " << failures << " FAILURE(S)\n";
    return 1;
}
