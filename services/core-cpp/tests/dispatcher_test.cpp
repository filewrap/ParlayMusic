// dispatcher_test.cpp — Dispatcher: /play from replied-to media, /mute,
// /join (Phase 5, Worker B).
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
//   src/engine_router.cpp src/store.cpp src/json.cpp src/config.cpp
//   src/http.cpp src/voice_call.cpp src/tdlib_client.cpp -o
//   /tmp/dispatcher_test -ldl && /tmp/dispatcher_test
// Exit 0 = all checks pass. No network: Telegram/IPC/voice are faked,
// the Store uses its JSON file backend in a temp dir.
#include <cstdio>
#include <filesystem>
#include <iostream>
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
    std::string getChatMemberStatus(int64_t chat_id, int64_t user_id) override {
        (void)chat_id;
        return user_id == 111 ? "administrator" : "member";
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
    parlay::IpcResult post(const std::string& base_url,
                           const std::string& action, const parlay::Json& body,
                           int timeout_sec = 30) const override {
        (void)body;
        (void)timeout_sec;
        posts.push_back(base_url + "|" + action);
        parlay::IpcResult r;
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
        parlay::IpcResult r;  // no now-playing state in this fake
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
    std::string fail_reason = "fake voice failure";
    struct PlayCall {
        int64_t chat;
        std::string url;
        std::string title;
        bool video;
    };
    std::vector<PlayCall> plays;
    std::vector<int64_t> joins, mutes, unmutes, stops;
    bool play(int64_t chat_id, const std::string& stream_url,
              const std::string& title, bool video, int seek_sec, double speed,
              int bass) override {
        (void)seek_sec;
        (void)speed;
        (void)bass;
        plays.push_back({chat_id, stream_url, title, video});
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
        (void)chat_id;
        return true;
    }
    bool resume(int64_t chat_id) override {
        (void)chat_id;
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
        return -1;
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
    std::filesystem::remove_all("/tmp/parlay-dispatcher-test");

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
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] ==
                      "Usage: /play <song title, link or track id>",
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
        CHECK(rig.tg.replies.size() == 1 && rig.tg.replies[0] == "Muted.",
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
                  rig.tg.replies[0] == "Already muted.",
              "mute when already muted: Already muted reply");
    }
    {
        Rig rig;
        rig.store.hset("chat:-1013", "voice_owner", "core");
        rig.store.hset("chat:-1013", "muted", "1");
        rig.d.handleMessage(makeMsg(-1013, 111, "/unmute"));
        CHECK(rig.voice.unmutes.size() == 1,
              "unmute core session: backend unmute() called");
        CHECK(rig.tg.replies.size() == 1 && rig.tg.replies[0] == "Unmuted.",
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
                  rig.tg.replies[0] == "Joined the voice chat.",
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
                  rig.tg.replies[0] == "Already in the voice chat.",
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

    std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
    if (failures == 0) {
        std::cout << "dispatcher_test: ALL PASS\n";
        return 0;
    }
    std::cout << "dispatcher_test: " << failures << " FAILURE(S)\n";
    return 1;
}
