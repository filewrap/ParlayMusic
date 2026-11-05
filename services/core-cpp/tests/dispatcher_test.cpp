// dispatcher_test.cpp — Dispatcher: /play from replied-to media (honest
// gap, Phase 2), /mute, /join (backend vc.* calls, Phase 2);
// /autoplay-skip, /broadcast errors.txt (Phase 6, Worker B);
// /speed + /bass honest saved-not-applied replies (Phase 2, backend
// DSP is Phase 5); /play through the backend (no core audio attach,
// Phase 2); /speedtest helpers (Phase 6, Worker C); backend-driven
// events + duplex join handshake + token-bucket limiter (Phase 2).
// (Phase 1 telemetry, §J) Every /play emits one t_dispatch_to_ipc
// line: dispatch_to_post_us + post_to_response_us == total,
// transport=uds / ok, or ipc=none on the usage-error path (no
// backend IPC exists there).
//
// - onPlay: reply with audio/document file_id downloads (fake) and
//   hands the local path to the backend with vc.play_file (§3.3);
//   no media + no query answers the honest usage error;
//   download failures are reported honestly, never faked.
// - onMute: playing session -> vc.mute through the backend; nothing
//   playing -> "Nothing is playing."
// - onJoin: admin-gated; backend vc.join or the honest reason.
//
// Build: g++ -std=c++20 -I src tests/dispatcher_test.cpp
//   src/dispatcher.cpp src/ipc.cpp src/store.cpp src/scram.cpp src/json.cpp
//   src/config.cpp src/toml.cpp src/rate_limiter.cpp src/tdlib_client.cpp
//   <path>/libtdjson.so.1.8.69 -Wl,-rpath,<dir>
//   -o /tmp/dispatcher_test && /tmp/dispatcher_test
// Exit 0 = all checks pass. No network: Telegram/IPC/TDLib are faked,
// the Store uses its JSON file backend in a temp dir.
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "dispatcher.hpp"
#include "ipc.hpp"
#include "json.hpp"
#include "store.hpp"
#include "tdlib_client.hpp"
#include "ui_texts.hpp"

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

class FakeTelegramSender : public parlay::TelegramSender {
public:
    // Replies (message text, one entry per send) and one serialised
    // keyboard per sendMessageHtml/sendPhoto/sendMessageReplyKb call,
    // so tests can assert a button is present by its callback_data.
    std::vector<std::string> replies;
    std::vector<std::string> markups;
    std::vector<std::string> photos;
    std::vector<int64_t> admins = {111};
    bool download_ok = true;
    std::string download_saved =
        "/tmp/parlay-dispatcher-test/replied/BQACAgAAA.ogg";
    std::set<int64_t> forward_fail;
    bool doc_ok = true;
    struct DocCall {
        int64_t chat;
        std::string filename, content, caption;
    };
    std::vector<DocCall> docs;
    struct EditCall {
        int64_t chat, msg;
        std::string text, kind;  // kind: text | caption | markup
    };
    std::vector<EditCall> edits;
    // Serialised keyboard of each editMessageText / editMessageCaption
    // call, in order (Package 3: the retry button on an edited card
    // is only visible through the markup).
    std::vector<std::string> edit_markups;
    struct ToastCall {
        std::string id, text;
        bool alert;
    };
    std::vector<ToastCall> toasts;
    std::vector<std::pair<int64_t, int64_t>> deleted;
    struct AudioCall {
        int64_t chat;
        std::string audio, caption, kind;  // audiofile | file | audio
    };
    std::vector<AudioCall> audios;
    struct InlineCall {
        std::string id, results_json;
        int cache_time;
        std::string switch_pm_text, switch_pm_parameter;
    };
    std::vector<InlineCall> inlines;
    std::string member_status_for_other = "member";
    int64_t next_id = 1000;

    static std::string serialiseKb(const parlay::InlineKeyboard& kb) {
        std::string out;
        for (const auto& row : kb) {
            out += "[";
            for (const auto& b : row) {
                out += b.text + "=";
                if (!b.callback_data.empty()) out += b.callback_data;
                else if (!b.copy_text.empty()) out += b.copy_text;
                else if (!b.url.empty()) out += b.url;
                else if (b.has_switch) out += b.switch_inline_query;
                out += "|";
            }
            out += "]";
        }
        return out;
    }

    bool sendMessage(int64_t chat_id, const std::string& text,
                     std::string& error_out) override {
        (void)chat_id;
        (void)error_out;
        replies.push_back(text);
        return true;
    }
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to = 0,
                          std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageHtml(int64_t chat_id, const std::string& text,
                            const parlay::InlineKeyboard& kb,
                            int64_t reply_to = 0,
                            std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        markups.push_back(serialiseKb(kb));
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageReplyKb(int64_t chat_id, const std::string& text,
                               const parlay::ReplyKeyboard& kb,
                               int64_t reply_to = 0,
                               std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        std::string flat;
        for (const auto& row : kb.rows)
            for (const auto& label : row) flat += label + "|";
        markups.push_back(flat);
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageRemoveKb(int64_t chat_id, const std::string& text,
                                int64_t reply_to = 0,
                                std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)reply_to;
        (void)error_out;
        replies.push_back(text);
        markups.push_back("");
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendPhoto(int64_t chat_id, const std::string& photo,
                      const std::string& caption,
                      const parlay::InlineKeyboard& kb,
                      std::string* error_out = nullptr) override {
        (void)chat_id;
        (void)error_out;
        photos.push_back(photo);
        replies.push_back(caption);
        markups.push_back(serialiseKb(kb));
        return static_cast<int64_t>(replies.size());
    }
    bool editMessageText(int64_t chat_id, int64_t message_id,
                         const std::string& text,
                         const parlay::InlineKeyboard& kb,
                         std::string* error_out = nullptr) override {
        (void)error_out;
        edits.push_back({chat_id, message_id, text, "text"});
        edit_markups.push_back(serialiseKb(kb));
        return true;
    }
    bool editMessageCaption(int64_t chat_id, int64_t message_id,
                            const std::string& caption,
                            const parlay::InlineKeyboard& kb,
                            std::string* error_out = nullptr) override {
        (void)error_out;
        edits.push_back({chat_id, message_id, caption, "caption"});
        edit_markups.push_back(serialiseKb(kb));
        return true;
    }
    bool editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                const parlay::InlineKeyboard& kb,
                                std::string* error_out = nullptr) override {
        (void)error_out;
        edits.push_back(
            {chat_id, message_id, serialiseKb(kb), "markup"});
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
    bool leaveChat(int64_t chat_id) override {
        (void)chat_id;
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
                      const std::string& caption,
                      const parlay::InlineKeyboard& kb,
                      std::string* error_out = nullptr) override {
        (void)kb;
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
    std::vector<int64_t> getChatAdministrators(int64_t chat_id) override {
        (void)chat_id;
        return admins;
    }
    std::string getChatMemberStatus(int64_t chat_id,
                                    int64_t user_id) override {
        (void)chat_id;
        return user_id == 111 ? "administrator" : member_status_for_other;
    }
    int64_t getChatId(const std::string& username_or_id) override {
        try {
            return std::stoll(username_or_id);
        } catch (...) {
            return 0;
        }
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
    // Scripted Package 2 answers (empty = fall through to the 404
    // below, per the FakeIpc contract above): mix.generate /
    // audio.recognize success payloads and failure messages, and a
    // vc.volume failure message. mix_error / recognize_error are
    // reported as bad_request / engine_unavailable respectively (the
    // codes the real backend uses for a refused mix and an
    // unconfigured recognition provider).
    mutable std::string mix_response;
    mutable std::string mix_error;
    mutable std::string recognize_response;
    mutable std::string recognize_error;
    mutable std::string volume_error;
    // Scripted Phase 9-gaps answers (empty = fall through to the 404
    // below, per the FakeIpc contract above): search.inline tracks,
    // vc.lyrics text, NetEase search/lyrics, download POSTs, and the
    // radio status GET. last_post_body records the most recent POST
    // body so tests can assert the chat_id the core passed.
    mutable std::string inline_response;
    // Scripted /search answer (Package 3, R4): when set, the search
    // POST succeeds with this JSON ({"results":[...]}).
    mutable std::string search_response;
    mutable std::string lyrics_response;
    mutable std::string netease_search_response;
    mutable std::string netease_lyrics_response;
    mutable std::string download_response;
    // Scripted download failure (Package 3, R6): when set, every
    // download/* POST fails with this message instead of succeeding.
    mutable std::string download_error;
    // Scripted spotify.collection answer (Package 3, R5): when set,
    // the collection metadata call succeeds with this JSON.
    mutable std::string spotify_collection_response;
    mutable std::string radio_status_response;
    // Scripted Phase 4 (Q4) answers (empty = fall through to the 404
    // below): download.cancel / library.delete success payloads.
    mutable std::string download_cancel_response;
    mutable std::string library_delete_response;
    // Scripted Package 1 answers (empty = fall through to the 404
    // below): library.scan / library.sharescan / cache.clear success
    // payloads.
    mutable std::string library_scan_response;
    mutable std::string library_sharescan_response;
    mutable std::string cache_clear_response;
    mutable std::string last_post_body;
    // Every POST body, in order (last_post_body keeps the latest).
    mutable std::vector<std::string> post_bodies;
    // Scripted Wave 2 answers (empty = fall through to the 404
    // below): vc.loop / vc.play_file success payloads, fx.apply,
    // spotify.analyze, spotify.details (GET), and the vc.queue list.
    mutable std::string loop_response;
    mutable std::string play_file_response;
    mutable std::string fx_response;
    mutable std::string spotify_analyze_response;
    mutable std::string spotify_details_response;
    mutable std::string queue_list_response;
    // Scripted vc.join / vc.mute answers (Phase 2): join_ok=false
    // fails the join with join_error; mute_error fails a mute.
    mutable bool join_ok = true;
    mutable std::string join_error = "backend join failed";
    mutable std::string mute_error;
    // Scripted playback state (Phase 6, B11 speed/bass engine path):
    // when true, GET now-playing returns a playing track and POST
    // seek succeeds; when false (default) the engines know nothing.
    mutable bool np_ok = false;
    // Scripted now-playing track (Phase 7, A1b follow tests): the id /
    // stream URL / position the GET now-playing answer reports.
    mutable std::string np_track_id = "t1";
    mutable std::string np_stream_url = "https://example.invalid/audio.webm";
    mutable int64_t np_position = 30;
    // Scripted chat.settings store (Phase 6, Q7): chat_id -> full
    // 15-key settings JSON object. GET /v1/chat/settings returns it
    // (or the backend defaults when unseeded); POST chat.settings
    // applies one key and returns the updated object, like the real
    // backend. settings_fail=true simulates the backend down: GET
    // and POST both fail with engine_unavailable.
    mutable std::map<int64_t, std::string> settings_store;
    mutable bool settings_fail = false;
    static std::string defaultSettingsJson() {
        return R"({"admin_mode":0,"play_mode":"everyone","cmd_delete":false,"clean_mode":false,"clean_delay_min":0,"thumbnails":true,"autoplay":false,"autoplay_lang":"","autoplay_mood":"","silent_mode":false,"auto_delete":false,"auto_delete_min":5,"language":"en","paginate_purge_old":true,"chat_music_download":true})";
    }
    // Seed one settings key for a chat (creates the stored object
    // from the defaults when absent). value is a JSON literal.
    void seedSetting(int64_t chat_id, const std::string& key,
                     const std::string& value_json) const {
        auto it = settings_store.find(chat_id);
        parlay::Json obj = parlay::Json::parse(
            it == settings_store.end() ? defaultSettingsJson() : it->second);
        obj.set(key, parlay::Json::parse(value_json));
        settings_store[chat_id] = obj.dump();
    }
    parlay::IpcResult post(const std::string& base_url,
                           const std::string& action, const parlay::Json& body,
                           int timeout_sec = 30) const override {
        (void)timeout_sec;
        posts.push_back(base_url + "|" + action);
        last_post_body = body.dump();
        post_bodies.push_back(body.dump());
        parlay::IpcResult r;
        if (action == "inline" && !inline_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(inline_response);
            return r;
        }
        if (action == "search" && !search_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(search_response);
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
            !download_error.empty()) {
            r.ok = false;
            r.http_status = 400;
            r.error_code = "download_failed";
            r.error_message = download_error;
            return r;
        }
        if (action.rfind("download/", 0) == 0 &&
            !download_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(download_response);
            return r;
        }
        if (action == "spotify.collection" &&
            !spotify_collection_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(spotify_collection_response);
            return r;
        }
        if (action == "loop") {
            r.ok = true;
            r.http_status = 200;
            r.data = loop_response.empty()
                         ? parlay::Json::parse(R"({})")
                         : parlay::Json::parse(loop_response);
            return r;
        }
        if (action == "dislike") {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(R"({})");
            return r;
        }
        if (action == "vc.play_file" && !play_file_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(play_file_response);
            return r;
        }
        if (action == "download.cancel" &&
            !download_cancel_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(download_cancel_response);
            return r;
        }
        if (action == "library.delete" &&
            !library_delete_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(library_delete_response);
            return r;
        }
        if (action == "library.scan" &&
            !library_scan_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(library_scan_response);
            return r;
        }
        if (action == "library.sharescan" &&
            !library_sharescan_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(library_sharescan_response);
            return r;
        }
        if (action == "cache.clear" &&
            !cache_clear_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(cache_clear_response);
            return r;
        }
        if (action == "fx" && !fx_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(fx_response);
            return r;
        }
        if (action == "spotify.analyze" &&
            !spotify_analyze_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(spotify_analyze_response);
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
        if (action == "stop") {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(R"({})");
            return r;
        }
        if (action == "volume") {
            if (!volume_error.empty()) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = volume_error;
                return r;
            }
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(R"({})");
            return r;
        }
        if (action == "mix.generate") {
            if (!mix_error.empty()) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = mix_error;
                return r;
            }
            if (!mix_response.empty()) {
                r.ok = true;
                r.http_status = 200;
                r.data = parlay::Json::parse(mix_response);
                return r;
            }
        }
        if (action == "audio.recognize") {
            if (!recognize_error.empty()) {
                r.http_status = 502;
                r.error_code = "engine_unavailable";
                r.error_message = recognize_error;
                return r;
            }
            if (!recognize_response.empty()) {
                r.ok = true;
                r.http_status = 200;
                r.data = parlay::Json::parse(recognize_response);
                return r;
            }
        }
        // Queue actions (Phase 9 §K): list returns the scripted
        // waiting queue when set, else an empty queue; clear/add/
        // remove succeed. Enough for /clear's empty-queue reply and
        // for the queued-play fallback (which guards on non-empty).
        if (action == "queue") {
            const parlay::Json* a = body.find("action");
            std::string act = a ? a->asString() : "";
            r.ok = true;
            r.http_status = 200;
            if (act == "list" && !queue_list_response.empty())
                r.data = parlay::Json::parse(queue_list_response);
            else
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
        if (action == "join") {
            if (!join_ok) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = join_error;
                return r;
            }
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(R"({})");
            return r;
        }
        if (action == "mute") {
            if (!mute_error.empty()) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = mute_error;
                return r;
            }
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(R"({})");
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
        if (action == "chat.settings") {
            if (settings_fail) {
                r.http_status = 503;
                r.error_code = "engine_unavailable";
                r.error_message = "fake: backend down";
                return r;
            }
            // One-key set, like the real backend: validate the key,
            // apply it to the stored object, return the full object.
            const parlay::Json* cid = body.find("chat_id");
            const parlay::Json* key = body.find("key");
            const parlay::Json* value = body.find("value");
            if (!cid || !key || !value || !key->isString()) {
                r.http_status = 400;
                r.error_code = "bad_request";
                r.error_message = "fake: chat.settings needs chat_id, key, value";
                return r;
            }
            int64_t chat_id = cid->asInt();
            auto it = settings_store.find(chat_id);
            parlay::Json obj = parlay::Json::parse(
                it == settings_store.end() ? defaultSettingsJson()
                                           : it->second);
            obj.set(key->asString(), *value);
            // The backend keeps clean_mode in sync with the delay.
            if (key->asString() == "clean_delay_min") {
                const parlay::Json* d = obj.find("clean_delay_min");
                obj.set("clean_mode",
                        parlay::Json(d && d->asInt() > 0));
            }
            settings_store[chat_id] = obj.dump();
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::objectOf({
                {"chat_id", parlay::Json(chat_id)},
                {"settings", obj},
            });
            return r;
        }
        if (action == "quality.get") {
            // Opus quality surface (2026-11-05): the effective config
            // for the chat, resolved like the real backend at the
            // store level only — a stored quality_* override wins
            // (source "store"), anything unset falls through to the
            // locked default (source "default"). The fake has no
            // parlay.toml / profile level, so those two never win
            // here; the library readback equals the effective config.
            if (settings_fail) {
                r.http_status = 503;
                r.error_code = "engine_unavailable";
                r.error_message = "fake: backend down";
                return r;
            }
            const parlay::Json* cid = body.find("chat_id");
            int64_t chat_id = cid ? cid->asInt() : 0;
            auto it = settings_store.find(chat_id);
            parlay::Json st = parlay::Json::parse(
                it == settings_store.end() ? defaultSettingsJson()
                                           : it->second);
            parlay::Json eff = parlay::Json::objectOf({});
            parlay::Json src = parlay::Json::objectOf({});
            auto take_int = [&](const char* key, const char* field,
                                int64_t def) {
                const parlay::Json* v = st.find(key);
                if (v != nullptr && !v->isNull()) {
                    eff.set(field, *v);
                    src.set(field, parlay::Json("store"));
                } else {
                    eff.set(field, parlay::Json(def));
                    src.set(field, parlay::Json("default"));
                }
            };
            auto take_bool = [&](const char* key, const char* field,
                                 bool def) {
                const parlay::Json* v = st.find(key);
                if (v != nullptr && !v->isNull()) {
                    eff.set(field, *v);
                    src.set(field, parlay::Json("store"));
                } else {
                    eff.set(field, parlay::Json(def));
                    src.set(field, parlay::Json("default"));
                }
            };
            take_int("quality_bitrate_bps", "bitrate_bps", 192000);
            take_int("quality_frame_ms", "frame_ms", 20);
            take_int("quality_complexity", "complexity", 10);
            take_bool("quality_vbr", "vbr", true);
            take_bool("quality_fec", "fec", false);
            take_bool("quality_dtx", "dtx", false);
            eff.set("max_playback_rate_hz", parlay::Json(48000));
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::objectOf({
                {"chat_id", parlay::Json(chat_id)},
                {"effective", eff},
                {"sources", src},
                {"library", eff},
                {"active_call", parlay::Json(false)},
                {"applies_on", parlay::Json("next join")},
            });
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
        if (path.rfind("/v1/chat/settings", 0) == 0) {
            if (settings_fail) {
                r.http_status = 503;
                r.error_code = "engine_unavailable";
                r.error_message = "fake: backend down";
                return r;
            }
            int64_t chat_id = 0;
            size_t at = path.find("chat_id=");
            if (at != std::string::npos) {
                try {
                    chat_id = std::stoll(path.substr(at + 8));
                } catch (...) {
                }
            }
            auto it = settings_store.find(chat_id);
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::objectOf({
                {"chat_id", parlay::Json(chat_id)},
                {"settings",
                 parlay::Json::parse(it == settings_store.end()
                                         ? defaultSettingsJson()
                                         : it->second)},
            });
            return r;
        }
        if (path.rfind("/v1/spotify/details", 0) == 0 &&
            !spotify_details_response.empty()) {
            r.ok = true;
            r.http_status = 200;
            r.data = parlay::Json::parse(spotify_details_response);
            return r;
        }
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

// Fake assistant session: a real TdLibClient object (creates a real
// td_json client, as tdlib_test does) with scripted call() answers for
// the duplex join handshake (Phase 2): getChat -> the video chat's
// group_call_id, createVideoChat -> a fresh call, joinVideoChat -> the
// remote SDP (or a scripted error).
class FakeAssistant : public parlay::TdLibClient {
public:
    FakeAssistant() : parlay::TdLibClient(parlay::SessionKind::Assistant) {}
    bool avail = true;
    bool authed = true;
    std::string fail = "assistant unavailable";
    bool has_video_chat = true;
    int32_t group_call_id = 12345;
    int32_t created_call_id = 777;
    std::string join_error;  // non-empty -> joinVideoChat fails
    std::string remote_sdp = "v=0\r\nfake-remote-sdp";
    mutable std::vector<std::string> calls;  // @type of each call()
    bool available() const override { return avail; }
    bool authorized() const override { return authed; }
    std::string lastError() const override { return fail; }
    parlay::Json call(const parlay::Json& req,
                      double timeout_sec = 30.0) override {
        (void)timeout_sec;
        const parlay::Json* t = req.find("@type");
        std::string type = t ? t->asString() : "";
        calls.push_back(type);
        if (type == "getChat") {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("chat")},
                {"video_chat",
                 parlay::Json::objectOf(
                     {{"group_call_id",
                       parlay::Json(static_cast<int64_t>(
                           has_video_chat ? group_call_id : 0))}})},
            });
        }
        if (type == "createVideoChat") {
            return parlay::Json::objectOf({
                {"@type", parlay::Json("groupCall")},
                {"id", parlay::Json(static_cast<int64_t>(created_call_id))},
            });
        }
        if (type == "joinVideoChat") {
            if (!join_error.empty()) {
                return parlay::Json::objectOf({
                    {"@type", parlay::Json("error")},
                    {"code", parlay::Json(static_cast<int64_t>(400))},
                    {"message", parlay::Json(join_error)},
                });
            }
            return parlay::Json::objectOf({
                {"@type", parlay::Json("text")},
                {"text", parlay::Json(remote_sdp)},
            });
        }
        return parlay::Json::objectOf({
            {"@type", parlay::Json("error")},
            {"code", parlay::Json(static_cast<int64_t>(400))},
            {"message",
             parlay::Json("fake assistant: unexpected call " + type)},
        });
    }
};

// --- helpers -----------------------------------------------------------------

// --- helpers -----------------------------------------------------------------

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
    FakeTelegramSender tg;
    FakeIpc ipc;
    FakeAssistant assistant;
    parlay::RateLimiter limiter;
    parlay::Dispatcher d;
    Rig()
        : cfg([] {
              parlay::Config c;
              c.data_dir = "/tmp/parlay-dispatcher-test";
              c.owner_id = 999;
              return c;
          }()),
          store(cfg),
          ipc(cfg),
          limiter(cfg.rate_limit),
          d(cfg, tg, ipc, store, assistant, limiter, "parlaybot") {}
};

}  // namespace

int main() {

    auto has_post = [](const Rig& rig, const std::string& action) {
        for (const auto& p : rig.ipc.posts)
            if (p.find("|" + action) != std::string::npos) return true;
        return false;
    };
    auto last_post_body = [](const Rig& rig) {
        return rig.ipc.last_post_body;
    };

    // --- A. parseMessage: deleted with telegram_api.cpp
    // (Phase 2). Updates now arrive as TDLib JSON via main.cpp.
    // --- B. onPlay: replied media (Phase 3 vc.play_file) ---------------
    // The file is downloaded via the TDLib bot session, then handed
    // to the backend with vc.play_file (§3.3): the action carries the
    // local path, not bytes. A successful play renders the Now
    // playing card; a backend refusal reports the exact error, never
    // a fake "Now playing".
    const std::string play_file_np =
        R"({"now_playing":{"track":{"id":"file:AUDIO123","title":"Artist - Song","artist":"Artist","duration_sec":200,"source":"file"},"position_sec":0,"paused":false}})";
    {
        Rig rig;
        rig.ipc.play_file_response = play_file_np;
        parlay::TgMessage m = makeMsg(-1001, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        rig.d.handleMessage(m);
        CHECK(has_post(rig, "vc.play_file"),
              "play reply audio: vc.play_file issued to the backend");
        {
            parlay::Json b = parlay::Json::parse(rig.ipc.last_post_body);
            CHECK(b.find("file_id") &&
                      b.find("file_id")->asString() == "AUDIO123" &&
                      b.find("file_path") &&
                      contains(b.find("file_path")->asString(),
                               "/replied/") &&
                      b.find("kind") &&
                      b.find("kind")->asString() == "audio" &&
                      !b.find("video") && !b.find("force"),
                  "play reply audio: play_file carries path + file_id + "
                  "kind, no video/force");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing") &&
                  contains(rig.tg.replies[0], "Artist - Song"),
              "play reply audio: Now playing card, no gap reply");
        auto c = rig.store.get("call:-1001");
        CHECK(c && *c == "1", "play reply audio: call marked active");
    }
    {
        Rig rig;
        rig.ipc.play_file_response = play_file_np;
        parlay::TgMessage m = makeMsg(-1002, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 6;
        m.has_reply_media = true;
        m.reply_media_file_id = "DOC123";
        m.reply_media_kind = "document";
        m.reply_media_title = "track.mp3";
        rig.d.handleMessage(m);
        CHECK(has_post(rig, "vc.play_file"),
              "play reply document: vc.play_file issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing"),
              "play reply document: Now playing card");
    }
    {
        // Backend refuses vc.play_file: the exact error, never a
        // "Now playing" over a failed play.
        Rig rig;
        parlay::TgMessage m = makeMsg(-1005, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 8;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        rig.d.handleMessage(m);
        CHECK(has_post(rig, "vc.play_file"),
              "play reply audio, backend refuses: vc.play_file issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "not_found") &&
                  !contains(rig.tg.replies[0], "Now playing"),
              "play reply audio, backend refuses: exact error, no "
              "fake Now playing");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1003, 111, "/play"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Whoops! Nothing to play") &&
                  contains(rig.tg.replies[0], "/play [song name or link]"),
              "play no media + no query: usage error, no IPC");
        CHECK(rig.ipc.posts.empty(),
              "play no media + no query: no engine call issued");
    }
    {
        Rig rig;
        rig.tg.download_ok = false;
        parlay::TgMessage m = makeMsg(-1004, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 7;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not fetch"),
              "play reply audio, download fails: honest error, no fake "
              "play");
        CHECK(rig.ipc.posts.empty(),
              "play reply audio, download fails: no engine call");
    }
    // --- C. onMute ----------------------------------------------------------
    // Mute goes to the backend (vc.mute) after the nothing-playing
    // check; the already-muted short-circuit is the stored flag.
    {
        Rig rig;
        rig.store.set("call:-1011", "1");
        rig.store.hset("chatrt:-1011", "muted", "0");
        rig.d.handleMessage(makeMsg(-1011, 111, "/mute"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|mute") != std::string::npos,
              "mute session: vc.mute issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "<b>Muted.</b>",
              "mute session: exact reply is the emoji-free string");
        auto f = rig.store.hgetall("chatrt:-1011");
        CHECK(f["muted"] == "1", "mute session: muted flag stored");
    }
    {
        Rig rig;
        rig.store.set("call:-1012", "1");
        rig.store.hset("chatrt:-1012", "muted", "1");
        rig.d.handleMessage(makeMsg(-1012, 111, "/mute"));
        CHECK(rig.ipc.posts.empty(),
              "mute already muted: no vc.mute issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "<b>Already muted.</b>",
              "mute already muted: exact reply is the emoji-free "
              "string");
    }
    {
        Rig rig;
        rig.store.set("call:-1013", "1");
        rig.store.hset("chatrt:-1013", "muted", "1");
        rig.d.handleMessage(makeMsg(-1013, 111, "/unmute"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|mute") != std::string::npos,
              "unmute session: vc.mute issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "<b>Unmuted.</b>",
              "unmute session: exact reply is the emoji-free string");
        auto f = rig.store.hgetall("chatrt:-1013");
        CHECK(f["muted"] == "0", "unmute session: muted flag cleared");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1014, 111, "/mute"));
        CHECK(rig.ipc.posts.empty(),
              "mute nothing playing: no vc.mute issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "mute nothing playing: honest reply");
    }
    {
        // Playing track (no call flag): the now-playing leg of the
        // nothing-playing check lets the mute through.
        Rig rig;
        rig.ipc.np_ok = true;
        rig.store.hset("chatrt:-1015", "muted", "0");
        rig.d.handleMessage(makeMsg(-1015, 111, "/mute"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|mute") != std::string::npos,
              "mute playing track: vc.mute issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "<b>Muted.</b>",
              "mute playing track: Muted reply");
    }
    {
        Rig rig;
        rig.store.set("call:-1016", "1");
        rig.store.hset("chatrt:-1016", "muted", "0");
        rig.ipc.mute_error = "ntgcalls mute failed";
        rig.d.handleMessage(makeMsg(-1016, 111, "/mute"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Could not mute") &&
                  contains(rig.tg.replies[0], "ntgcalls mute failed"),
              "mute backend fails: exact backend error reported");
        auto f = rig.store.hgetall("chatrt:-1016");
        CHECK(f["muted"] == "0",
              "mute backend fails: muted flag stays 0");
    }
    // --- D. onJoin ----------------------------------------------------------
    // The backend owns the call: /join issues vc.join, and the backend
    // drives the duplex JoinRequest handshake from there. An already
    // active call short-circuits before any IPC.
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1021, 111, "/join"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|join") != std::string::npos,
              "join: vc.join issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "<b>Joined the voice chat.</b>",
              "join: exact reply is the emoji-free string");
        auto c = rig.store.get("call:-1021");
        CHECK(c && *c == "1", "join: call marked active");
    }
    {
        Rig rig;
        rig.ipc.join_ok = false;
        rig.ipc.join_error = "tdlib not authorized";
        rig.d.handleMessage(makeMsg(-1022, 111, "/join"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "Could not join the voice chat") &&
                  contains(rig.tg.replies[0], "tdlib not authorized"),
              "join backend unavailable: exact backend error reported");
        auto c = rig.store.get("call:-1022");
        CHECK(!c || *c != "1",
              "join backend unavailable: call stays inactive");
    }
    {
        Rig rig;
        rig.store.set("call:-1023", "1");
        rig.d.handleMessage(makeMsg(-1023, 111, "/join"));
        CHECK(rig.ipc.posts.empty(),
              "join already in call: no vc.join issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] ==
                      "<b>Already in the voice chat.</b>",
              "join already in call: exact reply is the emoji-free "
              "string");
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

    // --- G. /speed + /bass: backend DSP is Phase 5 ---------------------
    // The value is stored and the stream restarts at the current
    // position; the reply says plainly the filter did not apply.
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.d.handleMessage(makeMsg(-1045, 111, "/speed 1.5"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|seek") != std::string::npos,
              "speed playing: seek-restart issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "1.5x saved") &&
                  contains(rig.tg.replies[0],
                           "cannot apply audio filters") &&
                  contains(rig.tg.replies[0], "Phase 5"),
              "speed playing: honest saved-not-applied reply");
    }
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.d.handleMessage(makeMsg(-1046, 111, "/bass 40"));
        CHECK(!rig.ipc.posts.empty() &&
                  rig.ipc.posts[0].find("|seek") != std::string::npos,
              "bass playing: seek-restart issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "40 saved") &&
                  contains(rig.tg.replies[0],
                           "cannot apply audio filters") &&
                  contains(rig.tg.replies[0], "Phase 5"),
              "bass playing: honest saved-not-applied reply");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1047, 111, "/speed 1.5"));
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "speed nothing playing: honest reply, no seek");
        CHECK(rig.ipc.posts.empty(),
              "speed nothing playing: no IPC issued");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1048, 111, "/bass 40"));
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Nothing is playing.",
              "bass nothing playing: honest reply, no seek");
        CHECK(rig.ipc.posts.empty(),
              "bass nothing playing: no IPC issued");
    }
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.d.handleMessage(makeMsg(-1049, 111, "/speed 4.5"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "0.5x to 4.0x"),
              "speed out of range: range error, no seek");
        CHECK(rig.ipc.posts.empty(),
              "speed out of range: no IPC issued");
    }
    // --- I. /play through the backend (Phase 2) -------------------------
    // The backend owns the call and the audio: vc.play is the play.
    // A successful play renders the Now playing card; a backend
    // failure reports the exact error, never a "Now playing" lie.
    // The core never sees the stream URL and keeps no core-stream
    // session fields (voice_owner / core_source / core_follow are
    // gone). Skip / pause / resume / seek / stop go to vc.* the same
    // way; the call-active flag is set by a successful play and
    // cleared by /stop and the TrackEnded/QueueEmpty events.
    const std::string play_np =
        R"({"now_playing":{"track":{"id":"t1","title":"Test Song","artist":"Test Artist","duration_sec":200,"stream_url":"https://example.invalid/audio.webm"},"stream_url":"https://example.invalid/audio.webm","position_sec":0,"paused":false}})";
    const std::string skip_np =
        R"({"now_playing":{"track":{"id":"t2","title":"Next Song","artist":"Next Artist","duration_sec":180,"stream_url":"https://example.invalid/next.webm"},"position_sec":0,"paused":false}})";
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1051, 111, "/play Test Song"));
        CHECK(has_post(rig, "play"),
              "play query: vc.play issued to the backend");
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(b.find("speed") && b.find("speed")->asDouble() == 1.0 &&
                      b.find("bass") && b.find("bass")->asInt() == 0 &&
                      !b.find("video") && !b.find("force"),
                  "play query: vc.play carries default speed 1.0 / "
                  "bass 0, no video/force");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing") &&
                  contains(rig.tg.replies[0], "Test Song"),
              "play query: Now playing reply");
        auto c = rig.store.get("call:-1051");
        CHECK(c && *c == "1", "play query: call marked active");
    }
    {
        // Stored speed/bass ride the vc.play body into the backend
        // room (§4.4): the room carries them for the Phase 5 DSP.
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.store.hset("chatrt:-1052", "speed", "1.5");
        rig.store.hset("chatrt:-1052", "bass", "40");
        rig.d.handleMessage(makeMsg(-1052, 111, "/play Test Song"));
        CHECK(has_post(rig, "play"),
              "play query: vc.play issued with stored speed/bass");
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(b.find("speed") && b.find("speed")->asDouble() == 1.5 &&
                      b.find("bass") && b.find("bass")->asInt() == 40 &&
                      !b.find("video") && !b.find("force"),
                  "play query: vc.play carries stored speed 1.5 / "
                  "bass 40 into the room, no video/force");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing"),
              "play query: Now playing reply with stored speed/bass");
        auto f = rig.store.hgetall("chatrt:-1052");
        CHECK(f["speed"] == "1.5" && f["bass"] == "40",
              "play query: stored speed/bass survive the play");
    }
    {
        // Engine refuses vc.play (stub 404): the exact error, never
        // a "Now playing" over a silent call.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1053, 111, "/play Test Song"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Service error") &&
                  contains(rig.tg.replies[0], "not_found"),
              "play backend 404: exact error reported, no Now playing");
        auto c = rig.store.get("call:-1053");
        CHECK(!c || *c != "1",
              "play backend 404: call stays inactive");
    }
    {
        // No stream_url in the response: the core needs no URL any
        // more (the backend owns the audio), so the play succeeds.
        Rig rig;
        rig.ipc.play_response =
            R"({"now_playing":{"track":{"id":"t3","title":"No URL Song","duration_sec":120},"position_sec":0,"paused":false}})";
        rig.d.handleMessage(makeMsg(-1054, 111, "/play No URL Song"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing") &&
                  contains(rig.tg.replies[0], "No URL Song"),
              "play no stream_url: play succeeds without a URL");
    }
    {
        // /vplay is accepted but the video flag is dropped from the
        // vc.play payload (D13): the backend owns the audio now and
        // there is no video lane.
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1055, 111, "/vplay Test Song"));
        CHECK(has_post(rig, "play") &&
                  !contains(last_post_body(rig), "\"video\"") &&
                  !contains(last_post_body(rig), "\"force\""),
              "vplay: vc.play issued, video/force dropped");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing"),
              "vplay: Now playing reply");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1056, 111, "/playforce Test Song"));
        CHECK(has_post(rig, "stop") && has_post(rig, "play"),
              "playforce: vc.stop then vc.play issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now playing"),
              "playforce: Now playing reply");
    }
    {
        Rig rig;
        rig.ipc.skip_response = skip_np;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.store.set("call:-1057", "1");
        rig.d.handleMessage(makeMsg(-1057, 111, "/skip"));
        CHECK(has_post(rig, "skip"),
              "skip: vc.skip issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Skipped") &&
                  contains(rig.tg.replies[0], "Next Song"),
              "skip: Skipped reply with the next track");
    }
    {
        Rig rig;
        rig.ipc.skip_response =
            R"({"now_playing":{"track":null},"queue":[]})";
        rig.store.set("call:-1058", "1");
        rig.d.handleMessage(makeMsg(-1058, 111, "/skip"));
        CHECK(has_post(rig, "skip"),
              "skip to empty: vc.skip issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "queue is now empty"),
              "skip to empty: queue-empty reply");
        auto c = rig.store.get("call:-1058");
        CHECK(!c || *c != "1",
              "skip to empty: call marked inactive");
    }
    {
        Rig rig;
        rig.ipc.autoplay_skip_response = skip_np;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.store.set("call:-1059", "1");
        rig.d.handleMessage(makeMsg(-1059, 111, "/autoplay-skip"));
        CHECK(has_post(rig, "autoplay-skip"),
              "autoplay-skip: vc.autoplay-skip issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Autoplay skipped") &&
                  contains(rig.tg.replies[0], "Next Song"),
              "autoplay-skip: card renders the next track");
    }
    {
        Rig rig;
        rig.ipc.pause_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":true}})";
        rig.store.set("call:-1060", "1");
        rig.d.handleMessage(makeMsg(-1060, 111, "/pause"));
        CHECK(has_post(rig, "pause"),
              "pause: vc.pause issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Paused"),
              "pause: Paused reply");
    }
    {
        // A fresh Rig per command: the token bucket would allow the
        // burst, but the store is shared, so the seeded call survives.
        Rig rig;
        rig.ipc.resume_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":false}})";
        rig.store.set("call:-1061", "1");
        rig.d.handleMessage(makeMsg(-1061, 111, "/resume"));
        CHECK(has_post(rig, "resume"),
              "resume: vc.resume issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Test Song"),
              "resume: Now playing card rendered for the resumed track");
    }
    {
        // Backend refuses the pause (stub 404): the exact error.
        Rig rig;
        rig.store.set("call:-1062", "1");
        rig.d.handleMessage(makeMsg(-1062, 111, "/pause"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Service error") &&
                  contains(rig.tg.replies[0], "not_found"),
              "pause backend 404: exact error reported");
    }
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.store.set("call:-1063", "1");
        rig.d.handleMessage(makeMsg(-1063, 111, "/seek 10"));
        CHECK(has_post(rig, "seek"),
              "seek: vc.seek issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Seeked forward to 0:40"),
              "seek: Seeked reply with the new position");
    }
    {
        Rig rig;
        rig.store.set("call:-1064", "1");
        rig.store.hset("chatrt:-1064", "loop", "1");
        rig.store.hset("chatrt:-1064", "speed", "1.5");
        rig.store.hset("chatrt:-1064", "bass", "40");
        rig.store.hset("chatrt:-1064", "muted", "1");
        rig.d.handleMessage(makeMsg(-1064, 111, "/stop"));
        CHECK(has_post(rig, "stop"),
              "stop: vc.stop issued to the backend");
        auto c = rig.store.get("call:-1064");
        CHECK(!c || *c != "1", "stop: call marked inactive");
        auto f = rig.store.hgetall("chatrt:-1064");
        CHECK(f["loop"] == "0" && f["speed"] == "1.0" &&
                  f["bass"] == "0" && f["muted"] == "0",
              "stop: loop/speed/bass/muted reset");
    }
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.store.set("playlist:111",
                      R"([{"title":"Song A"},{"title":"Song B"}])");
        rig.d.handleMessage(makeMsg(-1065, 111, "/playplaylist"));
        CHECK(has_post(rig, "queue"),
              "playplaylist: vc.queue issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Playing your playlist"),
              "playplaylist: playlist reply");
        auto c = rig.store.get("call:-1065");
        CHECK(c && *c == "1", "playplaylist: call marked active");
    }
    // --- J. watchCoreFollow: deleted with the 5-second watcher
    // (Phase 2). Track transitions are backend-driven events now;
    // see section N.
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
                  rig.tg.toasts[0].text == "🖐️" &&
                  !rig.tg.toasts[0].alert,
              "callback: unknown domain gets the hand toast, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq(""));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text == "🖐️" &&
                  !rig.tg.toasts[0].alert,
              "callback: empty data gets the hand toast, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:player:pause:" +
                                    std::string(70, '9')));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text == "🖐️" &&
                  !rig.tg.toasts[0].alert,
              "callback: over-64-byte data gets the hand toast, no crash");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("not-a-callback"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text == "🖐️" &&
                  !rig.tg.toasts[0].alert,
              "callback: non-pl data gets the hand toast, no crash");
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
        rig.ipc.seedSetting(-1001, "play_mode", "\"admins\"");
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
        // One Rig per call: the token bucket would allow the
        // burst, but the cooldown map is static (survives a fresh
        // Rig).
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
        // One Rig per call (fresh Rig keeps the token buckets full).
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
        // One Rig per call (fresh Rig keeps the token buckets
        // full); the RTMP target lives in the file store and survives
        // a fresh Rig.
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
        // One Rig per call (fresh Rig keeps the token buckets full).
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
        rig3.store.hset("chatrt:-1001", "rtmp_url", "rtmp://x/app");
        rig3.store.hset("chatrt:-1001", "rtmp_key", "k");
        Rig rig4;
        rig4.d.handleMessage(makeMsg(-1001, 111, "/stream"));
        CHECK(rig4.tg.replies.size() == 1 &&
                  contains(rig4.tg.replies[0], "Usage:") &&
                  contains(rig4.tg.replies[0], "/stream"),
              "stream: no query + nothing playing shows usage");
        rig4.store.hset("chatrt:-1001", "rtmp_url", "");
        rig4.store.hset("chatrt:-1001", "rtmp_key", "");
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
        // One Rig per call (fresh Rig keeps the token buckets full).
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
        // The skip re-checks now-playing after the backend call, so
        // the fake needs a playing track.
        Rig rig;
        rig.ipc.skip_response = skip_np;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.store.set("call:-1001", "1");
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
        {
            // Q7: the toggle posts paginate_purge_old=false to the
            // backend (FakeIpc store) — never to the state.json
            // chat: hash.
            auto it = rig.ipc.settings_store.find(-1001);
            bool off = false;
            if (it != rig.ipc.settings_store.end()) {
                parlay::Json st = parlay::Json::parse(it->second);
                const parlay::Json* pg = st.find("paginate_purge_old");
                off = pg && !pg->asBool(true);
            }
            CHECK(off && !rig.tg.toasts.empty() &&
                      contains(rig.tg.toasts.back().text, "updated"),
                  "set page:0 persists off + confirms");
        }
        rig.d.handleCallback(makeCq("pl:set:musicdl", -1001, 111));
        CHECK(contains(rig.tg.edits.back().text,
                       "Download music via chat"),
              "set musicdl: screen renders");
    }
    {
        // /quality + the quality sub-screen (Opus quality surface,
        // routing rows A96–A99, C102). The FakeIpc resolves
        // quality.get from the scripted settings store, so a set
        // followed by a re-render proves the write landed.
        Rig rig;
        rig.ipc.seedSetting(-1001, "quality_bitrate_bps", "256000");
        rig.d.handleMessage(makeMsg(-1001, 111, "/quality"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies[0], "Audio Quality") &&
                  contains(rig.tg.replies[0], "256 kbps (chat setting)") &&
                  contains(rig.tg.replies[0], "20 ms (default)") &&
                  contains(rig.tg.replies[0], "next join"),
              "quality: /quality renders the card with the store "
              "bitrate + source per field + next-join note");
        CHECK(!rig.tg.markups.empty() &&
                  contains(rig.tg.markups[0], "pl:set:quality:br:256") &&
                  contains(rig.tg.markups[0], "pl:set:quality:reset"),
              "quality: /quality card carries the bitrate/reset buttons");
        // DM refusal (C102).
        Rig rig_dm;
        parlay::TgMessage dm = makeMsg(111, 111, "/quality");
        dm.chat.type = "private";
        rig_dm.d.handleMessage(dm);
        CHECK(!rig_dm.tg.replies.empty() &&
                  contains(rig_dm.tg.replies.back(), "only in groups"),
              "quality: /quality in DM refuses with the group-only text");
        // Sub-screen via the settings panel button.
        rig.d.handleCallback(makeCq("pl:set:quality", -1001, 111));
        CHECK(!rig.tg.edits.empty() &&
                  contains(rig.tg.edits.back().text, "Audio Quality") &&
                  contains(rig.tg.edits.back().text, "256 kbps"),
              "quality: pl:set:quality opens the sub-screen");
        // Bitrate pick: posts quality_bitrate_bps=320000, confirms,
        // and re-renders with the new effective value. (The set is
        // found by scanning the posted bodies: the re-render's
        // quality.get read overwrites last_post_body.)
        rig.d.handleCallback(makeCq("pl:set:quality:br:320", -1001, 111));
        {
            bool posted = false;
            for (size_t i = 0; i < rig.ipc.post_bodies.size(); ++i) {
                parlay::Json b =
                    parlay::Json::parse(rig.ipc.post_bodies[i]);
                const parlay::Json* k = b.find("key");
                const parlay::Json* v = b.find("value");
                if (k && k->asString() == "quality_bitrate_bps" && v &&
                    v->asInt() == 320000)
                    posted = true;
            }
            CHECK(posted,
                  "quality: br:320 posts quality_bitrate_bps=320000");
        }
        CHECK(!rig.tg.toasts.empty() &&
                  contains(rig.tg.toasts.back().text, "updated") &&
                  contains(rig.tg.edits.back().text,
                           "320 kbps (chat setting)"),
              "quality: br:320 confirms + re-renders at 320 kbps");
        // Frame pick: posts quality_frame_ms=40.
        rig.d.handleCallback(makeCq("pl:set:quality:fr:40", -1001, 111));
        {
            bool posted = false;
            for (size_t i = 0; i < rig.ipc.post_bodies.size(); ++i) {
                parlay::Json b =
                    parlay::Json::parse(rig.ipc.post_bodies[i]);
                const parlay::Json* k = b.find("key");
                const parlay::Json* v = b.find("value");
                if (k && k->asString() == "quality_frame_ms" && v &&
                    v->asInt() == 40)
                    posted = true;
            }
            CHECK(posted, "quality: fr:40 posts quality_frame_ms=40");
        }
        CHECK(contains(rig.tg.edits.back().text, "40 ms (chat setting)"),
              "quality: fr:40 re-renders at 40 ms");
        // Out-of-range values are Unknown action, never stored.
        rig.d.handleCallback(makeCq("pl:set:quality:br:999", -1001, 111));
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text == "Unknown action.",
              "quality: br:999 rejected with Unknown action");
        rig.d.handleCallback(makeCq("pl:set:quality:fr:25", -1001, 111));
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text == "Unknown action.",
              "quality: fr:25 rejected with Unknown action");
        // Reset: clears all six overrides with JSON null; the card
        // falls back to the defaults.
        size_t posts_before = rig.ipc.post_bodies.size();
        rig.d.handleCallback(makeCq("pl:set:quality:reset", -1001, 111));
        int null_clears = 0;
        for (size_t i = posts_before; i < rig.ipc.post_bodies.size(); ++i) {
            parlay::Json b = parlay::Json::parse(rig.ipc.post_bodies[i]);
            const parlay::Json* k = b.find("key");
            const parlay::Json* v = b.find("value");
            if (k && k->asString().rfind("quality_", 0) == 0 && v &&
                v->isNull())
                ++null_clears;
        }
        CHECK(null_clears == 6,
              "quality: reset clears all six quality_* keys with null");
        CHECK(contains(rig.tg.edits.back().text, "192 kbps (default)") &&
                  contains(rig.tg.edits.back().text, "20 ms (default)"),
              "quality: reset re-renders at the defaults");
        // Backend down: /quality and the sub-screen fail honestly.
        Rig rig_down;
        rig_down.ipc.settings_fail = true;
        rig_down.d.handleMessage(makeMsg(-1001, 111, "/quality"));
        CHECK(!rig_down.tg.replies.empty() &&
                  contains(rig_down.tg.replies.back(), "Could not load"),
              "quality: /quality with the backend down fails honestly");
        rig_down.d.handleCallback(makeCq("pl:set:quality", -1001, 111));
        CHECK(!rig_down.tg.edits.empty() &&
                  contains(rig_down.tg.edits.back().text,
                           "Could not load"),
              "quality: sub-screen with the backend down fails honestly "
              "(Back button proven in ui_test)");
    }

    // --- Q7. Settings rewire (Phase 6): backend bbolt is the single
    // --- home for all 15 settings keys; the core's state.json keeps
    // --- only runtime state (chatrt:) and never a second copy. ----
    {
        // (a) Settings fetch renders the panel from backend values
        // and writes nothing to the state.json chat: hash.
        Rig rig;
        rig.ipc.seedSetting(-1201, "play_mode", "\"admins\"");
        rig.ipc.seedSetting(-1201, "clean_delay_min", "30");
        rig.d.handleMessage(makeMsg(-1201, 111, "/settings"));
        bool got_settings = false;
        for (const auto& g : rig.ipc.gets)
            if (g == "/v1/chat/settings?chat_id=-1201") got_settings = true;
        auto all = rig.store.hgetall("chat:-1201");
        CHECK(got_settings && !rig.tg.replies.empty() &&
                  contains(rig.tg.replies[0], "Chat Settings") &&
                  all.count("play_mode") == 0 &&
                  all.count("clean_delay_min") == 0,
              "q7 fetch: panel from backend GET, no chat: hash writes");
    }
    {
        // (b) A panel toggle posts only the changed key to the
        // backend and confirms; nothing lands in state.json.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:set:playmode", -1202, 111));
        int settings_posts = 0;
        std::string settings_body;
        for (size_t i = 0; i < rig.ipc.posts.size(); ++i)
            if (rig.ipc.posts[i].find("|chat.settings") !=
                std::string::npos) {
                ++settings_posts;
                settings_body = rig.ipc.post_bodies[i];
            }
        parlay::Json b = parlay::Json::parse(settings_body);
        auto it = rig.ipc.settings_store.find(-1202);
        bool stored_admins = false;
        if (it != rig.ipc.settings_store.end()) {
            parlay::Json st = parlay::Json::parse(it->second);
            const parlay::Json* pm = st.find("play_mode");
            stored_admins = pm && pm->asString() == "admins";
        }
        CHECK(settings_posts == 1 && b.find("key") &&
                  b.find("key")->asString() == "play_mode" &&
                  b.find("value") &&
                  b.find("value")->asString() == "admins" &&
                  stored_admins &&
                  !rig.tg.toasts.empty() &&
                  contains(rig.tg.toasts.back().text,
                           "Settings updated") &&
                  rig.store.hgetall("chat:-1202")
                          .count("play_mode") == 0,
              "q7 toggle: one-key post, stored in backend, no state copy");
    }
    {
        // (c) clean_mode shape split: the core posts only the int
        // minutes (clean_delay_min); the backend derives clean_mode.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1203, 111, "/cleanmode 30"));
        auto it = rig.ipc.settings_store.find(-1203);
        bool on = false, off = false;
        if (it != rig.ipc.settings_store.end()) {
            parlay::Json st = parlay::Json::parse(it->second);
            const parlay::Json* cm = st.find("clean_mode");
            const parlay::Json* cd = st.find("clean_delay_min");
            on = cm && cm->asBool(false) && cd && cd->asInt() == 30;
        }
        CHECK(on && !rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(), "Clean mode on"),
              "q7 cleanmode: /cleanmode 30 stores delay 30 + mode true");
        rig.d.handleMessage(makeMsg(-1203, 111, "/cleanmode off"));
        it = rig.ipc.settings_store.find(-1203);
        if (it != rig.ipc.settings_store.end()) {
            parlay::Json st = parlay::Json::parse(it->second);
            const parlay::Json* cm = st.find("clean_mode");
            const parlay::Json* cd = st.find("clean_delay_min");
            off = cm && !cm->asBool(true) && cd && cd->asInt() == 0;
        }
        CHECK(off && !rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(), "Clean mode off"),
              "q7 cleanmode: /cleanmode off stores delay 0 + mode false");
    }
    {
        // (d) Backend down: reads fall back to locked defaults with
        // the degraded note; writes fail honestly, nothing is
        // claimed saved, and nothing is written anywhere.
        Rig rig;
        rig.ipc.settings_fail = true;
        rig.d.handleMessage(makeMsg(-1204, 111, "/playmode"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(), "everyone") &&
                  contains(rig.tg.replies.back(),
                           "settings unreachable"),
              "q7 down: /playmode shows defaults + degraded note");
        rig.d.handleMessage(makeMsg(-1204, 111, "/playmode admins"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(),
                           "Could not save play mode") &&
                  rig.ipc.settings_store.find(-1204) ==
                      rig.ipc.settings_store.end(),
              "q7 down: /playmode admins fails honestly, nothing stored");
    }
    {
        // (e) Runtime fields live in the chatrt: hash; a legacy
        // chat: cplay is still read (read-only fallback) and a new
        // write lands only in chatrt:.
        Rig rig;
        rig.store.set("call:-1205", "1");
        rig.store.hset("chatrt:-1205", "loop", "3");
        rig.d.handleMessage(makeMsg(-1205, 111, "/loop"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(), "Turned loop on"),
              "q7 runtime: loop reads from chatrt:");
        rig.store.hset("chat:-1206", "cplay", "-1207");
        rig.d.handleMessage(makeMsg(-1206, 111, "/channelplay"));
        CHECK(!rig.tg.replies.empty() &&
                  contains(rig.tg.replies.back(), "-1207") &&
                  !rig.store.hget("chatrt:-1206", "cplay").has_value(),
              "q7 runtime: legacy chat: cplay read, not migrated");
        rig.store.set("call:-1208", "1");
        rig.d.handleMessage(makeMsg(-1208, 111, "/loop 2"));
        auto rt = rig.store.hget("chatrt:-1208", "loop");
        CHECK(rt && *rt == "2" &&
                  !rig.store.hget("chat:-1208", "loop").has_value(),
              "q7 runtime: new loop write lands in chatrt: only");
    }
    {
        // (f) After a full settings sequence (toggle, clean mode,
        // thumbnail) the state.json chat: hash holds no settings
        // keys at all — the backend is the only copy.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:set:playmode", -1209, 111));
        rig.d.handleMessage(makeMsg(-1209, 111, "/cleanmode 30"));
        rig.d.handleMessage(makeMsg(-1209, 111, "/thumbnail off"));
        auto all = rig.store.hgetall("chat:-1209");
        CHECK(all.empty(), "q7 no-copy: chat: hash stays empty after "
                           "playmode + cleanmode + thumbnail");
    }

    // --- Q7b. >50MB upload path (Phase 6): the bot->Telegram path
    // --- passes a file PATH to TDLib (inputFileLocal); the core
    // --- never reads the bytes and no size cap rejects it. The
    // --- file here is a 60 MiB sparse synthetic file: the size is
    // --- real, the bytes are zeros, and the proof is the path-only
    // --- request, not a live Telegram send. -------------------------
    {
        const std::string dir = "/tmp/parlay-upload-test";
        std::filesystem::create_directories(dir);
        const std::string big = dir + "/big-60m.bin";
        {
            std::ofstream out(big, std::ios::binary | std::ios::trunc);
            out.seekp(60LL * 1024 * 1024 - 1);
            out.put('\0');
        }
        CHECK(std::filesystem::file_size(big) == 60ULL * 1024 * 1024,
              "q7b upload: 60 MiB synthetic file exists");
        // Fake bot-session client: answers parseTextEntities with a
        // formattedText and captures every sendMessage request.
        class FakeBotClient : public parlay::TdLibClient {
        public:
            FakeBotClient()
                : parlay::TdLibClient(parlay::SessionKind::Bot) {}
            mutable std::vector<std::string> requests;
            mutable std::string fail_message;
            parlay::Json call(const parlay::Json& req,
                              double timeout_sec = 30.0) override {
                (void)timeout_sec;
                requests.push_back(req.dump());
                const parlay::Json* type = req.find("@type");
                if (type && type->asString() == "parseTextEntities") {
                    const parlay::Json* t = req.find("text");
                    return parlay::Json::objectOf({
                        {"@type", parlay::Json("formattedText")},
                        {"text", t ? *t : parlay::Json("")},
                        {"entities",
                         parlay::Json(parlay::Json::Array{})},
                    });
                }
                if (!fail_message.empty()) {
                    return parlay::Json::objectOf({
                        {"@type", parlay::Json("error")},
                        {"code", parlay::Json(static_cast<int64_t>(400))},
                        {"message", parlay::Json(fail_message)},
                    });
                }
                return parlay::Json::objectOf({
                    {"@type", parlay::Json("message")},
                    {"id", parlay::Json(static_cast<int64_t>(777))},
                });
            }
        };
        FakeBotClient bot;
        parlay::TdLibTelegramSender sender(bot, dir);
        std::string err;
        int64_t id = sender.sendFile(-1201, big, "", &err);
        CHECK(id == 777 && err.empty(),
              "q7b upload: sendFile >50MB succeeds via TDLib");
        CHECK(bot.requests.size() == 2,
              "q7b upload: exactly one parse + one sendMessage");
        parlay::Json req = parlay::Json::parse(bot.requests.back());
        const parlay::Json* content = req.find("input_message_content");
        const parlay::Json* doc = content ? content->find("document") : nullptr;
        bool path_only = content && content->find("@type") &&
                         content->find("@type")->asString() ==
                             "inputMessageDocument" &&
                         doc && doc->find("@type") &&
                         doc->find("@type")->asString() ==
                             "inputFileLocal" &&
                         doc->find("path") &&
                         doc->find("path")->asString() == big;
        CHECK(path_only && bot.requests.back().size() < 2048,
              "q7b upload: request carries the path only (<2 KiB, no "
              "file bytes embedded)");
        // Audio variant: same file, inputMessageAudio.
        bot.requests.clear();
        id = sender.sendAudioFile(-1201, big, "", &err);
        CHECK(id == 777 && err.empty(),
              "q7b upload: sendAudioFile >50MB succeeds via TDLib");
        parlay::Json areq = parlay::Json::parse(bot.requests.back());
        const parlay::Json* acontent =
            areq.find("input_message_content");
        CHECK(acontent && acontent->find("@type") &&
                  acontent->find("@type")->asString() ==
                      "inputMessageAudio",
              "q7b upload: audio request is inputMessageAudio");
        // Honest failure: a TDLib error surfaces verbatim, no fake
        // success and no silent drop.
        bot.requests.clear();
        bot.fail_message = "FILE_TOO_BIG (test)";
        id = sender.sendFile(-1201, big, "", &err);
        CHECK(id == -1 && err == "400: FILE_TOO_BIG (test)",
              "q7b upload: TDLib error surfaces verbatim");
        std::filesystem::remove(big);
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
                  artMsg(res, 0) == "Parlay v0.3.1" &&
                  artMsg(res, 1) == "Parlay v0.3.1" &&
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

    // --- J. Phase 1 telemetry: t_dispatch_to_ipc on /play --------------
    // The stubbed engine answers vc.play instantly, so the timer
    // values are tiny — the checks assert the line's presence, its
    // fields, and that dispatch_to_post + post_to_response == total.
    // (Real magnitudes come from the live path; see
    // phase1/timers-core.md.)
    {
        Rig rig;
        rig.ipc.play_response = play_np;
        std::string out;
        {
            CoutCapture cap;
            rig.d.handleMessage(makeMsg(-1070, 111, "/play Test Song"));
            out = cap.str();
        }
        std::string line = telemetryLine(out, "t_dispatch_to_ipc");
        CHECK(!line.empty(), "telemetry /play: t_dispatch_to_ipc emitted");
        CHECK(contains(line, "action=play") &&
                  contains(line, "chat_id=-1070") &&
                  contains(line, "transport=uds") &&
                  contains(line, "ok=1"),
              "telemetry /play: line names the action, chat, UDS "
              "transport, ok=1");
        int64_t total = fieldInt(line, "t_dispatch_to_ipc_us");
        int64_t d2p = fieldInt(line, "dispatch_to_post_us");
        int64_t p2r = fieldInt(line, "post_to_response_us");
        CHECK(total >= 0 && d2p >= 0 && p2r >= 0 && total == d2p + p2r,
              "telemetry /play: dispatch_to_post + post_to_response "
              "== total");
    }
    {
        // Engine refuses vc.play (stub 404): the line still fires,
        // with ok=0 and the exact error code.
        Rig rig;
        std::string out;
        {
            CoutCapture cap;
            rig.d.handleMessage(makeMsg(-1071, 111, "/play Test Song"));
            out = cap.str();
        }
        std::string line = telemetryLine(out, "t_dispatch_to_ipc");
        CHECK(!line.empty() && contains(line, "ok=0") &&
                  contains(line, "error=not_found"),
              "telemetry /play engine 404: line emitted with ok=0 and "
              "the exact error code");
    }
    {
        // Replied-file /play: vc.play_file is a real backend IPC
        // post, so the line carries dispatch_to_post +
        // post_to_response == total with ok=1 — no ipc=none any more.
        Rig rig;
        rig.ipc.play_file_response = play_file_np;
        parlay::TgMessage m = makeMsg(-1072, 111, "/play");
        m.has_reply_to = true;
        m.reply_to_message_id = 5;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO123";
        m.reply_media_kind = "audio";
        m.reply_media_title = "Artist - Song";
        std::string out;
        {
            CoutCapture cap;
            rig.d.handleMessage(m);
            out = cap.str();
        }
        std::string line = telemetryLine(out, "t_dispatch_to_ipc");
        CHECK(!line.empty() && contains(line, "ok=1") &&
                  !contains(line, "ipc=none"),
              "telemetry /play replied file: line emitted with ok=1, "
              "real IPC post (no ipc=none)");
        int64_t total = fieldInt(line, "t_dispatch_to_ipc_us");
        int64_t d2p = fieldInt(line, "dispatch_to_post_us");
        int64_t p2r = fieldInt(line, "post_to_response_us");
        CHECK(total >= 0 && d2p >= 0 && p2r >= 0 && total == d2p + p2r,
              "telemetry /play replied file: dispatch_to_post + "
              "post_to_response == total");
    }
    {
        // /play with no query and no media: usage error, still timed.
        Rig rig;
        std::string out;
        {
            CoutCapture cap;
            rig.d.handleMessage(makeMsg(-1073, 111, "/play"));
            out = cap.str();
        }
        std::string line = telemetryLine(out, "t_dispatch_to_ipc");
        CHECK(!line.empty() && contains(line, "ipc=none") &&
                  contains(line, "ok=0") &&
                  contains(line, "error=no_query"),
              "telemetry /play no query: line emitted with ipc=none, "
              "ok=0, error=no_query");
    }

    // --- M. token-bucket limiter (Phase 2) ------------------------------
    // Per-chat + per-user buckets from the config (chat burst 5,
    // user burst 3, both 1.0/s): the tighter user bucket denies the
    // 4th command, the denial carries the limiter's reason string,
    // other users and other chats are unaffected, and the refill lets
    // the same user through again after ~1.2s.
    {
        Rig rig;
        for (int i = 0; i < 3; ++i)
            rig.d.handleMessage(makeMsg(-1080, 111, "/id"));
        CHECK(rig.tg.replies.size() == 3 &&
                  contains(rig.tg.replies[2], "-1080"),
              "limiter: 3 commands inside the user burst all pass");
        rig.d.handleMessage(makeMsg(-1080, 111, "/id"));
        CHECK(rig.tg.replies.size() == 4 &&
                  contains(rig.tg.replies[3],
                           "rate limit exceeded for user 111") &&
                  contains(rig.tg.replies[3], "retry after"),
              "limiter: 4th command denied with the limiter's reason "
              "string");
        rig.d.handleMessage(makeMsg(-1080, 333, "/id"));
        CHECK(rig.tg.replies.size() == 5 &&
                  contains(rig.tg.replies[4], "-1080"),
              "limiter: a different user in the same chat is allowed");
        rig.d.handleMessage(makeMsg(-1081, 111, "/id"));
        CHECK(rig.tg.replies.size() == 6 &&
                  contains(rig.tg.replies[5],
                           "rate limit exceeded for user 111"),
              "limiter: the same user in another chat is still "
              "denied (per-user bucket)");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        rig.d.handleMessage(makeMsg(-1080, 111, "/id"));
        CHECK(rig.tg.replies.size() == 7 &&
                  contains(rig.tg.replies[6], "-1080"),
              "limiter: the user passes again after the refill");
    }

    // --- N. backend-driven events (Phase 2) -----------------------------
    // Track transitions arrive over UDS (setEventHandler); the
    // dispatcher edits the recorded card in place or sends a fresh
    // card, and ends the call record on TrackEnded/QueueEmpty.
    {
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.ipc.np_position = 5;
        rig.store.set("call:-1091", "1");
        rig.store.set("card:-1091", "text|55");
        rig.d.onBackendEvent(
            "TrackStarted",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json(static_cast<int64_t>(-1091))}}));
        rig.d.processPendingWork();
        CHECK(rig.tg.edits.size() == 1 && rig.tg.edits[0].chat == -1091 &&
                  rig.tg.edits[0].msg == 55 &&
                  contains(rig.tg.edits[0].text, "T"),
              "event TrackStarted: recorded card edited in place");
        CHECK(rig.tg.replies.empty(),
              "event TrackStarted: no fresh card when one is recorded");
    }
    {
        // No recorded card: a fresh card is sent instead of an edit.
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.np_track_id = "t2";
        rig.ipc.np_stream_url = "https://example.invalid/next.webm";
        rig.store.set("call:-1092", "1");
        rig.d.onBackendEvent(
            "TrackStarted",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json(static_cast<int64_t>(-1092))}}));
        rig.d.processPendingWork();
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Now Playing"),
              "event TrackStarted no card: fresh card sent");
    }
    {
        Rig rig;
        rig.store.set("call:-1093", "1");
        rig.store.set("card:-1093", "text|56");
        rig.d.onBackendEvent(
            "TrackEnded",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json(static_cast<int64_t>(-1093))}}));
        rig.d.processPendingWork();
        CHECK(rig.tg.edits.size() == 1 &&
                  rig.tg.edits[0].text == parlay::ui_texts::kNowIdle,
              "event TrackEnded: recorded card goes idle");
        auto c = rig.store.get("call:-1093");
        CHECK(!c || *c != "1",
              "event TrackEnded: call marked inactive");
    }
    {
        Rig rig;
        rig.d.onBackendEvent(
            "VoiceError",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json(static_cast<int64_t>(-1094))},
                 {"code", parlay::Json("join_failed")},
                 {"message",
                  parlay::Json("ntgcalls connect failed")}}));
        rig.d.processPendingWork();
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Voice error") &&
                  contains(rig.tg.replies[0], "join_failed") &&
                  contains(rig.tg.replies[0], "ntgcalls connect failed"),
              "event VoiceError: error text delivered to the chat");
    }
    {
        // Unknown / no-UI events (DownloadReady) change nothing.
        Rig rig;
        rig.d.onBackendEvent(
            "DownloadReady",
            parlay::Json::objectOf(
                {{"chat_id", parlay::Json(static_cast<int64_t>(-1095))},
                 {"track_id", parlay::Json("t9")}}));
        rig.d.processPendingWork();
        CHECK(rig.tg.edits.empty() && rig.tg.replies.empty(),
              "event DownloadReady: no UI action");
    }

    // --- O. duplex join handshake (Phase 2) -----------------------------
    // The backend sends JoinRequest(chat_id, local_sdp) over UDS; the
    // core runs TDLib joinVideoChat(local_sdp) on the SHARED assistant
    // session and answers JoinResponse(remote_sdp, group_call_id).
    // onJoinRequest runs on the IpcClient worker thread and blocks
    // until the main thread drains the pending work, so the tests
    // drive it on a worker thread + processPendingWork() here.
    {
        Rig rig;
        parlay::Json req = parlay::Json::objectOf(
            {{"chat_id", parlay::Json(static_cast<int64_t>(-1080))},
             {"local_sdp", parlay::Json("v=0\r\nfake-local-sdp")}});
        parlay::Json resp;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            resp = rig.d.onJoinRequest(req);
            done.store(true);
        });
        for (int i = 0; i < 200 && !done.load(); ++i) {
            rig.d.processPendingWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done.load(), "join handshake: answered without hanging");
        if (done.load()) worker.join(); else worker.detach();
        const parlay::Json* ok = resp.find("ok");
        CHECK(ok && ok->asBool(), "join handshake: ok=true");
        const parlay::Json* data = resp.find("data");
        CHECK(data && data->find("remote_sdp") &&
                  data->find("remote_sdp")->asString() ==
                      "v=0\r\nfake-remote-sdp",
              "join handshake: remote_sdp returned");
        CHECK(data && data->find("group_call_id") &&
                  data->find("group_call_id")->asInt() == 12345,
              "join handshake: group_call_id returned");
        CHECK(rig.assistant.calls.size() == 2 &&
                  rig.assistant.calls[0] == "getChat" &&
                  rig.assistant.calls[1] == "joinVideoChat",
              "join handshake: getChat then joinVideoChat on the "
              "shared assistant session");
        auto c = rig.store.get("call:-1080");
        CHECK(c && *c == "1", "join handshake: call marked active");
    }
    {
        // No video chat exists yet: createVideoChat runs first.
        Rig rig;
        rig.assistant.has_video_chat = false;
        parlay::Json req = parlay::Json::objectOf(
            {{"chat_id", parlay::Json(static_cast<int64_t>(-1081))},
             {"local_sdp", parlay::Json("v=0\r\nfake-local-sdp")}});
        parlay::Json resp;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            resp = rig.d.onJoinRequest(req);
            done.store(true);
        });
        for (int i = 0; i < 200 && !done.load(); ++i) {
            rig.d.processPendingWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done.load(),
              "join handshake no video chat: answered without hanging");
        if (done.load()) worker.join(); else worker.detach();
        const parlay::Json* data = resp.find("data");
        CHECK(data && data->find("group_call_id") &&
                  data->find("group_call_id")->asInt() == 777,
              "join handshake no video chat: created call id returned");
        CHECK(rig.assistant.calls.size() == 3 &&
                  rig.assistant.calls[0] == "getChat" &&
                  rig.assistant.calls[1] == "createVideoChat" &&
                  rig.assistant.calls[2] == "joinVideoChat",
              "join handshake no video chat: getChat, createVideoChat, "
              "joinVideoChat in order");
    }
    {
        // joinVideoChat refused: ok=false with the exact TDLib error.
        Rig rig;
        rig.assistant.join_error = "GROUPCALL_FORBIDDEN";
        parlay::Json req = parlay::Json::objectOf(
            {{"chat_id", parlay::Json(static_cast<int64_t>(-1082))},
             {"local_sdp", parlay::Json("v=0\r\nfake-local-sdp")}});
        parlay::Json resp;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            resp = rig.d.onJoinRequest(req);
            done.store(true);
        });
        for (int i = 0; i < 200 && !done.load(); ++i) {
            rig.d.processPendingWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done.load(),
              "join handshake failure: answered without hanging");
        if (done.load()) worker.join(); else worker.detach();
        const parlay::Json* ok = resp.find("ok");
        const parlay::Json* err = resp.find("error");
        CHECK(ok && !ok->asBool() && err &&
                  err->find("message") &&
                  contains(err->find("message")->asString(),
                           "GROUPCALL_FORBIDDEN"),
              "join handshake failure: ok=false with the exact error");
    }
    {
        // Assistant session down: the handshake refuses without
        // attempting any TDLib call.
        Rig rig;
        rig.assistant.avail = false;
        parlay::Json req = parlay::Json::objectOf(
            {{"chat_id", parlay::Json(static_cast<int64_t>(-1083))},
             {"local_sdp", parlay::Json("v=0\r\nfake-local-sdp")}});
        parlay::Json resp;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            resp = rig.d.onJoinRequest(req);
            done.store(true);
        });
        for (int i = 0; i < 200 && !done.load(); ++i) {
            rig.d.processPendingWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done.load(),
              "join handshake no assistant: answered without hanging");
        if (done.load()) worker.join(); else worker.detach();
        const parlay::Json* ok = resp.find("ok");
        CHECK(ok && !ok->asBool(),
              "join handshake no assistant: ok=false");
        CHECK(rig.assistant.calls.empty(),
              "join handshake no assistant: no TDLib call attempted");
    }
    {
        // Bad request (no local_sdp): answered immediately, no thread,
        // no TDLib call.
        Rig rig;
        parlay::Json bad = parlay::Json::objectOf(
            {{"chat_id", parlay::Json(static_cast<int64_t>(-1084))}});
        parlay::Json resp = rig.d.onJoinRequest(bad);
        const parlay::Json* ok = resp.find("ok");
        CHECK(ok && !ok->asBool(),
              "join handshake bad request: ok=false immediately");
        CHECK(rig.assistant.calls.empty(),
              "join handshake bad request: no TDLib call attempted");
    }

    // --- W. Phase 3 Wave 2 core fixes (D3/D4/D13/D14/D16/E8) -----------
    auto queue_posts = [](const Rig& rig, const std::string& act) {
        std::vector<parlay::Json> out;
        for (const auto& b : rig.ipc.post_bodies) {
            parlay::Json j = parlay::Json::parse(b);
            const parlay::Json* a = j.find("action");
            if (a && a->asString() == act) out.push_back(j);
        }
        return out;
    };
    const std::string queue3 =
        R"({"queue":[
          {"position":0,"requested_by":111,"track":{"id":"w0","title":"First Waiting","duration_sec":200}},
          {"position":1,"requested_by":111,"track":{"id":"w1","title":"Second Waiting","duration_sec":210}},
          {"position":2,"requested_by":111,"track":{"id":"w2","title":"Third Waiting","duration_sec":220}}]})";
    const std::string queue2 =
        R"({"queue":[
          {"position":0,"requested_by":111,"track":{"id":"w0","title":"First Waiting","duration_sec":200}},
          {"position":1,"requested_by":111,"track":{"id":"w1","title":"Second Waiting","duration_sec":210}}]})";
    const std::string dl_ok =
        R"({"file_path":"/tmp/parlay-dispatcher-test/dl/song.mp3","track":{"id":"t1","title":"Song","artist":"A","duration_sec":200},"run_id":"abcdef123456"})";

    // /loop (D3): vc.loop with the count, nothing queued.
    {
        Rig rig;
        rig.store.set("call:-1101", "1");
        rig.d.handleMessage(makeMsg(-1101, 111, "/loop 3"));
        CHECK(has_post(rig, "loop"), "/loop 3: vc.loop issued");
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(b.find("count") && b.find("count")->asInt() == 3,
                  "/loop 3: vc.loop carries count 3");
        }
        CHECK(queue_posts(rig, "add").empty(),
              "/loop 3: no queue copies enqueued (no 2N looping)");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Turned loop on"),
              "/loop 3: loop-on reply");
        auto f = rig.store.hgetall("chatrt:-1101");
        CHECK(f["loop"] == "3", "/loop 3: count recorded in the room");
    }
    {
        Rig rig;
        rig.store.set("call:-1102", "1");
        rig.store.hset("chatrt:-1102", "loop", "3");
        rig.d.handleMessage(makeMsg(-1102, 111, "/loop off"));
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(has_post(rig, "loop") && b.find("count") &&
                      b.find("count")->asInt() == 0,
                  "/loop off: vc.loop carries count 0");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Turned loop off"),
              "/loop off: loop-off reply");
        auto f = rig.store.hgetall("chatrt:-1102");
        CHECK(f["loop"] == "0", "/loop off: count cleared in the room");
    }
    {
        Rig rig;
        rig.store.set("call:-1103", "1");
        rig.store.hset("chatrt:-1103", "loop", "2");
        rig.d.handleMessage(makeMsg(-1103, 111, "/loop"));
        CHECK(!has_post(rig, "loop"),
              "/loop no args: no vc.loop call, state only");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Turned loop on"),
              "/loop no args: reports the stored loop-on state");
    }
    {
        Rig rig;
        rig.store.set("call:-1104", "1");
        rig.d.handleMessage(makeMsg(-1104, 111, "/loop 99"));
        parlay::Json b = parlay::Json::parse(last_post_body(rig));
        CHECK(has_post(rig, "loop") && b.find("count") &&
                  b.find("count")->asInt() == 10,
              "/loop 99: count clamped to the backend limit 10");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1105, 111, "/loop 3"));
        CHECK(rig.ipc.posts.empty() &&
                  rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Not streaming"),
              "/loop with nothing streaming: honest reply, no IPC");
    }

    // Queue display N -> backend N-1 (D4).
    {
        Rig rig;
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1111, 111, "/remove 2"));
        auto removes = queue_posts(rig, "remove");
        CHECK(removes.size() == 1 && removes[0].find("position") &&
                  removes[0].find("position")->asInt() == 1,
              "/remove 2: backend position 1");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Removed #2") &&
                  contains(rig.tg.replies[0], "Second Waiting"),
              "/remove 2: reply names the 2nd waiting track");
    }
    {
        Rig rig;
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1112, 111, "/remove 1"));
        auto removes = queue_posts(rig, "remove");
        CHECK(removes.size() == 1 && removes[0].find("position") &&
                  removes[0].find("position")->asInt() == 0,
              "/remove 1: backend position 0");
        CHECK(contains(rig.tg.replies[0], "Removed #1") &&
                  contains(rig.tg.replies[0], "First Waiting"),
              "/remove 1: reply names the 1st waiting track");
    }
    {
        Rig rig;
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1113, 111, "/move 1 3"));
        auto removes = queue_posts(rig, "remove");
        CHECK(removes.size() == 3 &&
                  removes[0].find("position")->asInt() == 2 &&
                  removes[1].find("position")->asInt() == 1 &&
                  removes[2].find("position")->asInt() == 0,
              "/move 1 3: removes 0-based positions from the end");
        auto adds = queue_posts(rig, "add");
        CHECK(adds.size() == 3 &&
                  adds[0].find("track")->find("id")->asString() == "w1" &&
                  adds[1].find("track")->find("id")->asString() == "w2" &&
                  adds[2].find("track")->find("id")->asString() == "w0",
              "/move 1 3: re-adds in the new order (B, C, A)");
        CHECK(contains(rig.tg.replies[0], "Moved #1 to #3"),
              "/move 1 3: moved reply");
    }
    {
        // Display 1 used to be refused as "the currently playing
        // track" — it is a waiting track now (0 is current).
        Rig rig;
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1114, 111, "/move 1 2"));
        auto adds = queue_posts(rig, "add");
        CHECK(adds.size() == 3 &&
                  adds[0].find("track")->find("id")->asString() == "w1" &&
                  adds[1].find("track")->find("id")->asString() == "w0",
              "/move 1 2: moving the first waiting track is allowed");
    }
    {
        Rig rig;
        rig.store.set("call:-1115", "1");
        rig.ipc.queue_list_response = queue2;
        rig.d.handleMessage(makeMsg(-1115, 111, "/shuffle"));
        auto removes = queue_posts(rig, "remove");
        CHECK(removes.size() == 2 &&
                  removes[0].find("position")->asInt() == 1 &&
                  removes[1].find("position")->asInt() == 0,
              "/shuffle with 2 waiting tracks: removes positions 1, 0");
        auto adds = queue_posts(rig, "add");
        std::vector<std::string> ids;
        for (const auto& a : adds)
            ids.push_back(a.find("track")->find("id")->asString());
        std::sort(ids.begin(), ids.end());
        CHECK(adds.size() == 2 && ids.size() == 2 && ids[0] == "w0" &&
                  ids[1] == "w1",
              "/shuffle with 2 waiting tracks: both re-added "
              "(was blocked by the old size < 3 guard)");
    }
    {
        Rig rig;
        rig.store.set("call:-1116", "1");
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1116, 111, "/clear"));
        CHECK(contains(rig.tg.replies[0], "Cleared <b>3</b> queued tracks"),
              "/clear: counts all waiting tracks, not size-1");
        rig.d.handleMessage(makeMsg(-1116, 111, "/restore"));
        auto adds = queue_posts(rig, "add");
        CHECK(adds.size() == 3 &&
                  adds[0].find("track")->find("id")->asString() == "w0" &&
                  adds[1].find("track")->find("id")->asString() == "w1" &&
                  adds[2].find("track")->find("id")->asString() == "w2",
              "/restore: restores all 3 saved tracks, not 2");
        CHECK(contains(rig.tg.replies[1], "Queue restored"),
              "/restore: restored reply");
    }
    {
        // /queue shows every waiting track, numbered from 1, and the
        // first waiting track is no longer skipped.
        Rig rig;
        rig.ipc.np_ok = true;
        rig.ipc.queue_list_response = queue3;
        rig.d.handleMessage(makeMsg(-1117, 111, "/queue"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Up Next") &&
                  contains(rig.tg.replies[0], "3 tracks") &&
                  contains(rig.tg.replies[0], "First Waiting") &&
                  contains(rig.tg.replies[0], "Second Waiting") &&
                  contains(rig.tg.replies[0], "Third Waiting") &&
                  contains(rig.tg.replies[0], "1.") &&
                  contains(rig.tg.replies[0], "2.") &&
                  contains(rig.tg.replies[0], "3."),
              "/queue: all waiting tracks listed, numbered 1..3");
    }

    // /download routes by link type (D14).
    {
        Rig rig;
        rig.ipc.download_response = dl_ok;
        rig.d.handleMessage(makeMsg(
            -1121, 111, "/download https://www.deezer.com/track/123"));
        CHECK(has_post(rig, "download/deezer"),
              "/download Deezer URL: download.deezer issued");
        parlay::Json b = parlay::Json::parse(last_post_body(rig));
        CHECK(b.find("query") && contains(b.find("query")->asString(),
                                           "deezer.com/track/123"),
              "/download Deezer URL: query carries the link");
    }
    {
        Rig rig;
        rig.ipc.download_response = dl_ok;
        rig.d.handleMessage(makeMsg(
            -1122, 111,
            "/download https://www.tiktok.com/@user/video/123"));
        CHECK(has_post(rig, "download/tiktok"),
              "/download TikTok URL: download.tiktok issued");
        parlay::Json b = parlay::Json::parse(last_post_body(rig));
        CHECK(b.find("url") && contains(b.find("url")->asString(),
                                         "tiktok.com/@user/video/123"),
              "/download TikTok URL: url carries the link");
    }
    {
        Rig rig;
        rig.ipc.download_response = dl_ok;
        rig.d.handleMessage(makeMsg(-1123, 111, "/download navidrome:abc123"));
        CHECK(has_post(rig, "download/navidrome"),
              "/download navidrome: id: download.navidrome issued");
        parlay::Json b = parlay::Json::parse(last_post_body(rig));
        CHECK(b.find("url") && b.find("url")->asString() == "navidrome:abc123",
              "/download navidrome: id: url carries the id");
    }
    {
        Rig rig;
        rig.ipc.download_response = dl_ok;
        rig.d.handleMessage(
            makeMsg(-1124, 111, "/download soulseek:Artist - Title"));
        CHECK(has_post(rig, "download/soulseek"),
              "/download soulseek: query: download.soulseek issued");
        parlay::Json b = parlay::Json::parse(last_post_body(rig));
        CHECK(b.find("query") && b.find("query")->asString() == "Artist - Title",
              "/download soulseek: query: soulseek: prefix stripped");
    }
    {
        Rig rig;
        rig.ipc.download_response = dl_ok;
        rig.d.handleMessage(makeMsg(-1125, 111, "/download some song"));
        CHECK(has_post(rig, "download/youtube"),
              "/download plain query: still falls through to YouTube");
    }

    // cbDl FX honours applied:false (D16, Phase 5: recorded for
    // the next track — no live stream to apply to).
    {
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.ipc.fx_response =
            R"({"output_path":"/tmp/in.ogg","applied":false,"effect":"slow-reverb","live_applied":false,"detail":"recorded for the next track; no live stream to apply to"})";
        rig.d.handleCallback(makeCq("pl:dl:fx:run1:slow"));
        CHECK(rig.tg.audios.empty(),
              "cbDl FX applied:false: no audio sent for a "
              "recorded-only effect");
        CHECK(rig.tg.toasts.size() >= 2 &&
                  contains(rig.tg.toasts.back().text, "recorded") &&
                  contains(rig.tg.toasts.back().text, "next track"),
              "cbDl FX applied:false: honest recorded/next-track toast");
    }
    // cbDl FX honours live_applied:true (Phase 5): the effect is
    // on the live stream, so the unprocessed input file must NOT
    // be sent as if it were processed audio.
    {
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.ipc.fx_response =
            R"({"output_path":"/tmp/in.ogg","applied":true,"effect":"nightcore","live_applied":true,"detail":"applied live to the voice chat"})";
        rig.d.handleCallback(makeCq("pl:dl:fx:run1:nightcore"));
        CHECK(rig.tg.audios.empty(),
              "cbDl FX live_applied:true: no audio sent (the file "
              "is unprocessed; the effect is on the live stream)");
        CHECK(rig.tg.toasts.size() >= 2 &&
                  contains(rig.tg.toasts.back().text, "live"),
              "cbDl FX live_applied:true: honest live toast");
    }
    {
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.ipc.fx_response =
            R"({"output_path":"/tmp/in.ogg","applied":true,"effect":"original","live_applied":false})";
        rig.d.handleCallback(makeCq("pl:dl:fx:run1:orig"));
        CHECK(rig.tg.audios.size() == 1 &&
                  rig.tg.audios[0].audio == "/tmp/in.ogg",
              "cbDl FX applied:true (original): file still sent");
    }

    // cbDl cancel / cbDel delete wired to the backend (Phase 4, Q4).
    {
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.ipc.download_cancel_response =
            R"({"cancelled":true,"run_id":"run1","phase":"cancelled"})";
        rig.d.handleCallback(makeCq("pl:dl:cancel:run1"));
        CHECK(has_post(rig, "download.cancel"),
              "cbDl cancel: download.cancel POST issued");
        CHECK(!rig.store.get("dlrun:run1"),
              "cbDl cancel: dlrun key cleared on success");
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text ==
                      parlay::ui_texts::kToastDlCancelled,
              "cbDl cancel: cancelled toast");
        CHECK(!rig.tg.edits.empty() &&
                  rig.tg.edits.back().text ==
                      parlay::ui_texts::kToastDlCancelled,
              "cbDl cancel: progress message retired in place");
    }
    {
        // Finished run: backend answers cancelled:false (200) — the
        // core says so honestly and clears the stale key.
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.ipc.download_cancel_response =
            R"({"cancelled":false,"run_id":"run1","phase":"done"})";
        rig.d.handleCallback(makeCq("pl:dl:cancel:run1"));
        CHECK(!rig.store.get("dlrun:run1"),
              "cbDl cancel finished: stale dlrun key cleared");
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text ==
                      parlay::ui_texts::kToastDlNothingToCancel,
              "cbDl cancel finished: nothing-to-cancel toast");
    }
    {
        // No run at all: nothing to cancel, no backend call.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:dl:cancel:run1"));
        CHECK(!has_post(rig, "download.cancel"),
              "cbDl cancel no-run: no backend call");
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text ==
                      parlay::ui_texts::kToastDlNothingToCancel,
              "cbDl cancel no-run: nothing-to-cancel toast");
    }
    {
        // Backend 404 (unknown run): the exact backend error is the toast.
        Rig rig;
        rig.store.set("dlrun:run1", "/tmp/in.ogg\n-1001");
        rig.d.handleCallback(makeCq("pl:dl:cancel:run1"));
        CHECK(has_post(rig, "download.cancel"),
              "cbDl cancel 404: download.cancel POST issued");
        CHECK(!rig.tg.toasts.empty() && rig.tg.toasts.back().alert,
              "cbDl cancel 404: error toast (alert)");
    }
    {
        Rig rig;
        rig.ipc.library_delete_response =
            R"({"deleted":true,"id":"track123"})";
        rig.d.handleCallback(makeCq("pl:del:yes:track123"));
        CHECK(has_post(rig, "library.delete"),
              "cbDel yes: library.delete POST issued");
        CHECK(!rig.tg.toasts.empty() &&
                  rig.tg.toasts.back().text ==
                      parlay::ui_texts::kToastDelDeleted,
              "cbDel yes: Deleted toast");
        CHECK(rig.tg.deleted.size() == 1 &&
                  rig.tg.deleted[0].second == 7,
              "cbDel yes: confirm card deleted");
    }
    {
        // Backend 404 (unknown id): exact backend error, card stays.
        Rig rig;
        rig.d.handleCallback(makeCq("pl:del:yes:track123"));
        CHECK(has_post(rig, "library.delete"),
              "cbDel yes 404: library.delete POST issued");
        CHECK(!rig.tg.toasts.empty() && rig.tg.toasts.back().alert,
              "cbDel yes 404: error toast (alert)");
        CHECK(rig.tg.deleted.empty(),
              "cbDel yes 404: confirm card kept");
    }

    // cbSpotify details/analyze (E8).
    const std::string spot_details =
        R"({"track":{"id":"track1","title":"Test Track","artist":"Test Artist","album":"Test Album","duration_sec":200,"url":"https://open.spotify.com/track/track1"},"features":{"key":"C","modality":"Major","tempo_bpm":120,"acousticness":0.1,"danceability":0.8,"energy":0.9,"instrumentalness":0.0,"liveness":0.2,"speechiness":0.05,"valence":0.7},"genres":["pop"],"disliked_by":[111,222],"ignored_by":[],"profanity":null,"language":null,"lyrics":"la la la","lyrics_link":"https://example.invalid/lyrics"})";
    {
        Rig rig;
        rig.ipc.spotify_details_response = spot_details;
        rig.d.handleCallback(makeCq("pl:spotify:details:track1"));
        bool got = false;
        for (const auto& g : rig.ipc.gets)
            if (contains(g, "spotify/details") &&
                contains(g, "track_id=track1"))
                got = true;
        CHECK(got, "cbSpotify details: spotify.details GET issued");
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Test Track") &&
                  contains(rig.tg.edits[0].text, "Test Album") &&
                  contains(rig.tg.edits[0].text, "Key C Major") &&
                  contains(rig.tg.edits[0].text, "120 BPM") &&
                  contains(rig.tg.edits[0].text, "Disliked by 2 people") &&
                  contains(rig.tg.edits[0].text, "Ignored by 0 people") &&
                  contains(rig.tg.edits[0].text, "Genres: pop") &&
                  contains(rig.tg.edits[0].text, "la la la"),
              "cbSpotify details: card shows title, album, features, "
              "counts, genres, lyrics");
    }
    {
        Rig rig;
        rig.ipc.spotify_analyze_response =
            R"({"analysis":"Clean lyrics, upbeat song.","profane_words":[]})";
        rig.d.handleCallback(makeCq("pl:spotify:analyze:track1"));
        CHECK(has_post(rig, "spotify.analyze"),
              "cbSpotify analyze: spotify.analyze POST issued");
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Lyrics analysis") &&
                  contains(rig.tg.edits[0].text, "Clean lyrics") &&
                  contains(rig.tg.edits[0].text, "No profanity found"),
              "cbSpotify analyze: analysis card rendered in place");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:spotify:links:track1"));
        CHECK(rig.tg.toasts.size() == 1 && rig.tg.toasts[0].alert &&
                  contains(rig.tg.toasts[0].text, "not available yet"),
              "cbSpotify unknown verb: honest unavailable toast");
    }
    {
        Rig rig;
        rig.d.handleCallback(makeCq("pl:spotify:dislike:track1"));
        CHECK(rig.tg.toasts.size() == 1 &&
                  contains(rig.tg.toasts[0].text, "Disliked"),
              "cbSpotify dislike: still routed to spotify.dislike");
    }

    // --- Package 1 (S0–S5): silent denials + sudo gaps ---------------
    // S0 (per user ruling 2026-11-05, superseding the silent drop):
    // unknown commands answer the refined kUnknownCommand card — the
    // command doesn't exist, use /help. No IPC post.
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 111, "/nosuchcommand"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "doesn't exist") &&
                  contains(rig.tg.replies[0], "/help") &&
                  rig.ipc.posts.empty(),
              "pkg1 unknown command: refined reply, no post");
    }
    // S1: /rmcache — sudo-only gate, cache.clear post, cleared
    // result, honest refusal when the backend refuses.
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 112, "/rmcache"));
        CHECK(rig.tg.replies.empty() && rig.ipc.posts.empty(),
              "pkg1 rmcache: non-sudo is denied silently, no post");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.cache_clear_response =
            R"({"cleared": true, "names": ["Song A", "Song B"]})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/rmcache"));
        CHECK(has_post(rig, "cache.clear"),
              "pkg1 rmcache: cache.clear issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Cache cleared.") &&
                  contains(rig.tg.replies[0], "2 entries removed."),
              "pkg1 rmcache: cleared result with entry count");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.cache_clear_response =
            R"({"cleared": true, "names": ["Song A"]})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/rmcache Song A"));
        CHECK(has_post(rig, "cache.clear"),
              "pkg1 rmcache named: cache.clear issued");
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(b.find("scope") &&
                      b.find("scope")->asString() == "track" &&
                      b.find("name") &&
                      b.find("name")->asString() == "Song A",
                  "pkg1 rmcache named: scope track + name carried");
        }
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.d.handleMessage(makeMsg(-1001, 111, "/rmcache"));
        CHECK(has_post(rig, "cache.clear"),
              "pkg1 rmcache, backend refuses: cache.clear issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "fake: no engine here") &&
                  !contains(rig.tg.replies[0], "Cache cleared"),
              "pkg1 rmcache, backend refuses: honest error, no fake "
              "success");
    }
    // S2: /scan — sudo-only gate, library.scan post, trigger result,
    // honest not-configured text, honest refusal.
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 112, "/scan"));
        CHECK(rig.tg.replies.empty() && rig.ipc.posts.empty(),
              "pkg1 scan: non-sudo is denied silently, no post");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.library_scan_response =
            R"({"triggered": true, "tracks_found": 5})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/scan"));
        CHECK(has_post(rig, "library.scan"),
              "pkg1 scan: library.scan issued to the backend");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Library scan triggered.") &&
                  contains(rig.tg.replies[0], "5 tracks found."),
              "pkg1 scan: trigger result with track count");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.library_scan_response =
            R"({"triggered": false, "error": "not configured"})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/scan"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "not configured") &&
                  !contains(rig.tg.replies[0], "Library scan triggered."),
              "pkg1 scan, not configured: honest text, no fake success");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.d.handleMessage(makeMsg(-1001, 111, "/scan"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "fake: no engine here") &&
                  !contains(rig.tg.replies[0], "Library scan triggered."),
              "pkg1 scan, backend refuses: honest error, no fake success");
    }
    // S3: /sharescan — sudo-only gate, library.sharescan post,
    // triggered / already-running / scheduled results, honest refusal.
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 112, "/sharescan"));
        CHECK(rig.tg.replies.empty() && rig.ipc.posts.empty(),
              "pkg1 sharescan: non-sudo is denied silently, no post");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.library_sharescan_response = R"({"triggered": true})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/sharescan"));
        CHECK(has_post(rig, "library.sharescan"),
              "pkg1 sharescan: library.sharescan issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "slskd share scan triggered."),
              "pkg1 sharescan: triggered result");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.library_sharescan_response = R"({"triggered": false})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/sharescan"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "already in progress") &&
                  !contains(rig.tg.replies[0], "triggered."),
              "pkg1 sharescan, already running: honest text, no fake "
              "success");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.ipc.library_sharescan_response =
            R"({"triggered": false, "scheduled_in_min": 7})";
        rig.d.handleMessage(makeMsg(-1001, 111, "/sharescan"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "scheduled in ~7m."),
              "pkg1 sharescan, scheduled: scheduled result with minutes");
    }
    {
        Rig rig;
        rig.store.sadd("sudoers", "111");
        rig.d.handleMessage(makeMsg(-1001, 111, "/sharescan"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "fake: no engine here") &&
                  !contains(rig.tg.replies[0], "triggered."),
              "pkg1 sharescan, backend refuses: honest error, no fake "
              "success");
    }
    // S4: admin-list refresh on admin transitions (remix
    // chat_member_updated), observed through /settings admin-gating:
    // a promotion re-fetches the list so the new admin passes the
    // gate at once; an ordinary member join refreshes nothing, so
    // the newcomer is still denied; a demotion re-fetches, so the
    // demoted admin is denied again. (User ids 5222/5333 are fresh:
    // an earlier test blacklists 222 in the shared file store.)
    {
        Rig rig;
        rig.tg.admins = {111};
        rig.d.handleMessage(makeMsg(-100777, 5222, "/settings"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Access Denied"),
              "pkg1 admin transition: non-admin is denied before "
              "promotion (cache seeded {111})");
        rig.tg.admins = {111, 5222};
        CHECK(rig.d.handleAdminTransition(
                  parlay::TgChatMemberUpdate{-100777, 5222, "member",
                                             "administrator"}),
              "pkg1 admin transition: promotion refreshes");
        rig.d.handleMessage(makeMsg(-100777, 5222, "/settings"));
        CHECK(rig.tg.replies.size() == 2 &&
                  contains(rig.tg.replies[1], "Chat Settings"),
              "pkg1 admin transition: promoted admin opens settings "
              "without waiting for the TTL");
        rig.tg.admins = {111, 5222, 5333};
        CHECK(!rig.d.handleAdminTransition(
                  parlay::TgChatMemberUpdate{-100777, 5333, "left",
                                             "member"}),
              "pkg1 admin transition: member join does not refresh");
        rig.d.handleMessage(makeMsg(-100777, 5333, "/settings"));
        CHECK(rig.tg.replies.size() == 3 &&
                  contains(rig.tg.replies[2], "Access Denied"),
              "pkg1 admin transition: member join leaves the cache "
              "unchanged, newcomer still denied");
        rig.tg.admins = {111};
        CHECK(rig.d.handleAdminTransition(
                  parlay::TgChatMemberUpdate{-100777, 5222, "administrator",
                                             "member"}),
              "pkg1 admin transition: demotion refreshes");
        rig.d.handleMessage(makeMsg(-100777, 5222, "/settings"));
        CHECK(rig.tg.replies.size() == 4 &&
                  contains(rig.tg.replies[3], "Access Denied"),
              "pkg1 admin transition: demoted admin is denied again");
    }
    // S5: the owner is exempt from the blacklist-user refusal on
    // Everyone commands; a blacklisted non-owner is still refused
    // silently, and a non-blacklisted non-owner still runs them.
    // The blacklisted non-owner is 4444, a fresh id: 111 is the
    // workhorse user for later package tests and must not be
    // blacklisted in the shared file store (merge fix: it was 111,
    // which silently refused every later Everyone command from 111
    // and segfaulted the /mix tests on an empty post_bodies[0]).
    {
        Rig rig;
        rig.store.sadd("bl_users", "999");
        rig.d.handleMessage(makeMsg(-1001, 999, "/id"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "This chat has ID"),
              "pkg1 blacklist: blacklisted owner still runs /id");
    }
    {
        Rig rig;
        rig.store.sadd("bl_users", "4444");
        rig.d.handleMessage(makeMsg(-1001, 4444, "/id"));
        CHECK(rig.tg.replies.empty(),
              "pkg1 blacklist: blacklisted non-owner still refused "
              "silently");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1001, 113, "/id"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "This chat has ID"),
              "pkg1 blacklist: non-blacklisted non-owner still runs /id");
    }

    // --- P2. /mix, /volume, /recognize, s-aliases (Package 2) -------------
    const std::string mix_tracks =
        R"({"tracks":[{"id":"yt:mix1","title":"Mix Song One","artist":"Mix Artist","source":"youtube","url":"https://www.youtube.com/watch?v=mix1","duration_sec":200,"duration_str":"3:20"},{"id":"yt:mix2","title":"Mix Song Two","artist":"Mix Artist","source":"youtube","url":"https://www.youtube.com/watch?v=mix2","duration_sec":180,"duration_str":"3:00"}],"source_track":{"id":"yt:seed","title":"Seed Song","source":"youtube"}})";
    {
        // Bare /mix with nothing playing: usage, no backend call.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1301, 111, "/mix"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage:") &&
                  contains(rig.tg.replies[0], "/mix [song or URL]"),
              "mix no-query: usage card, no mix.generate post");
        CHECK(!has_post(rig, "mix.generate"),
              "mix no-query: no mix.generate posted");
    }
    {
        // /mix with a query: mix.generate posted, tracks enqueued
        // (vc.queue add per track), first track's queued card
        // rendered by editing the fetching message in place.
        Rig rig;
        rig.ipc.mix_response = mix_tracks;
        rig.ipc.queue_list_response = R"({"queue":[{},{}]})";
        rig.d.handleMessage(makeMsg(-1302, 111, "/mix Seed Song"));
        CHECK(has_post(rig, "mix.generate"),
              "mix query: mix.generate issued to the backend");
        CHECK(!rig.ipc.post_bodies.empty(),
              "mix query: a post body was recorded");
        if (!rig.ipc.post_bodies.empty()) {
            parlay::Json b = parlay::Json::parse(rig.ipc.post_bodies[0]);
            CHECK(b.find("query") &&
                      b.find("query")->asString() == "Seed Song",
                  "mix query: mix.generate carries the query");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Fetching mix tracks...",
              "mix query: fetching reply sent");
        int queue_posts = 0;
        for (const auto& p : rig.ipc.posts)
            if (p.find("|queue") != std::string::npos) ++queue_posts;
        CHECK(queue_posts == 3,
              "mix query: two vc.queue adds + one list");
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Added to Queue:") &&
                  contains(rig.tg.edits[0].text, "#1") &&
                  contains(rig.tg.edits[0].text, "Mix Song One"),
              "mix query: first mix track queued card edited in place");
    }
    {
        // Bare /mix while a track is playing: seeds from the current
        // song (empty query posted).
        Rig rig;
        rig.ipc.mix_response = mix_tracks;
        rig.ipc.queue_list_response = R"({"queue":[{},{}]})";
        rig.store.set("call:-1303", "1");
        rig.d.handleMessage(makeMsg(-1303, 111, "/mix"));
        CHECK(has_post(rig, "mix.generate"),
              "mix bare while playing: mix.generate issued");
        CHECK(!rig.ipc.post_bodies.empty(),
              "mix bare while playing: a post body was recorded");
        if (!rig.ipc.post_bodies.empty()) {
            parlay::Json b = parlay::Json::parse(rig.ipc.post_bodies[0]);
            CHECK(b.find("query") && b.find("query")->asString().empty(),
                  "mix bare while playing: empty query seeds from current");
        }
    }
    {
        // Backend refuses the mix: the exact error, edited in place.
        Rig rig;
        rig.ipc.mix_error = "Seed is not a YouTube track.";
        rig.d.handleMessage(makeMsg(-1304, 111, "/mix Seed Song"));
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text, "Service error") &&
                  contains(rig.tg.edits[0].text,
                           "Seed is not a YouTube track."),
              "mix failure: exact backend error edited in place");
    }
    {
        // Empty mix: every track already queued or playing.
        Rig rig;
        rig.ipc.mix_response =
            R"({"tracks":[],"source_track":{"id":"yt:seed","title":"Seed Song","source":"youtube"}})";
        rig.d.handleMessage(makeMsg(-1305, 111, "/mix Seed Song"));
        CHECK(rig.tg.edits.size() == 1 &&
                  contains(rig.tg.edits[0].text,
                           "already in the queue or playing"),
              "mix empty: already-queued text edited in place");
        CHECK(!has_post(rig, "queue"),
              "mix empty: nothing enqueued");
    }
    {
        // /volume: usage, non-numeric, out of range, nothing
        // playing, success, boundaries, backend refusal.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1310, 111, "/volume"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /volume"),
              "volume usage: no argument answers the usage line");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1311, 111, "/volume loud"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "must be a number"),
              "volume non-numeric: refused honestly");
        CHECK(!has_post(rig, "volume"),
              "volume non-numeric: no vc.volume posted");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1312, 111, "/volume 201"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "must be between 0 and 200"),
              "volume out-of-range: 201 refused");
        Rig rig2;
        rig2.d.handleMessage(makeMsg(-1313, 111, "/volume -5"));
        CHECK(rig2.tg.replies.size() == 1 &&
                  contains(rig2.tg.replies[0],
                           "must be between 0 and 200"),
              "volume out-of-range: -5 refused");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1314, 111, "/volume 80"));
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Not in call.",
              "volume nothing-playing: Not in call.");
        CHECK(!has_post(rig, "volume"),
              "volume nothing-playing: no vc.volume posted");
    }
    {
        Rig rig;
        rig.store.set("call:-1315", "1");
        rig.d.handleMessage(makeMsg(-1315, 111, "/volume 80"));
        CHECK(has_post(rig, "volume"),
              "volume success: vc.volume issued to the backend");
        {
            parlay::Json b = parlay::Json::parse(last_post_body(rig));
            CHECK(b.find("volume") && b.find("volume")->asInt() == 80,
                  "volume success: vc.volume carries 80");
        }
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] == "Volume set to 80.",
              "volume success: Volume set to 80.");
        auto f = rig.store.hgetall("chatrt:-1315");
        CHECK(f["volume"] == "80",
              "volume success: applied value kept for the +/- buttons");
    }
    {
        Rig rig;
        rig.store.set("call:-1316", "1");
        rig.d.handleMessage(makeMsg(-1316, 111, "/volume 0"));
        rig.d.handleMessage(makeMsg(-1316, 111, "/volume 200"));
        CHECK(rig.tg.replies.size() == 2 &&
                  rig.tg.replies[0] == "Volume set to 0." &&
                  rig.tg.replies[1] == "Volume set to 200.",
              "volume boundaries: 0 and 200 both accepted");
    }
    {
        Rig rig;
        rig.store.set("call:-1317", "1");
        rig.ipc.volume_error = "engine down";
        rig.d.handleMessage(makeMsg(-1317, 111, "/volume 80"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "engine down"),
              "volume backend refusal: exact error reported");
        auto f = rig.store.hgetall("chatrt:-1317");
        CHECK(f.count("volume") == 0,
              "volume backend refusal: no value recorded");
    }
    {
        // /splay: same play, success card suppressed, call still
        // marked active; usage and backend errors stay visible.
        Rig rig;
        rig.ipc.play_response = play_np;
        rig.d.handleMessage(makeMsg(-1321, 111, "/splay Test Song"));
        CHECK(has_post(rig, "play"),
              "splay: vc.play issued to the backend");
        CHECK(rig.tg.replies.empty(),
              "splay: success card suppressed");
        auto c = rig.store.get("call:-1321");
        CHECK(c && *c == "1", "splay: call marked active");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1322, 111, "/splay Test Song"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Service error"),
              "splay error: backend refusal stays visible");
    }
    {
        Rig rig;
        rig.d.handleMessage(makeMsg(-1323, 111, "/splay"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Whoops! Nothing to play."),
              "splay usage: no-query usage stays visible");
    }
    {
        // Plain /pause still renders its card: suppression is
        // per-invocation, not a mode.
        Rig rig;
        rig.store.set("call:-1324", "1");
        rig.ipc.pause_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":true}})";
        rig.d.handleMessage(makeMsg(-1324, 111, "/pause"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Paused"),
              "pause control: plain /pause renders its card");
    }
    {
        Rig rig;
        rig.store.set("call:-1325", "1");
        rig.ipc.skip_response = skip_np;
        rig.d.handleMessage(makeMsg(-1325, 111, "/sskip"));
        CHECK(has_post(rig, "skip"), "sskip: vc.skip issued");
        CHECK(rig.tg.replies.empty(),
              "sskip: success card suppressed");
    }
    {
        Rig rig;
        rig.store.set("call:-1326", "1");
        rig.d.handleMessage(makeMsg(-1326, 111, "/sstop"));
        CHECK(has_post(rig, "stop"), "sstop: vc.stop issued");
        CHECK(rig.tg.replies.empty(),
              "sstop: success reply suppressed");
        auto c = rig.store.get("call:-1326");
        CHECK(!c || *c != "1", "sstop: call marked inactive");
    }
    {
        Rig rig;
        rig.store.set("call:-1327", "1");
        rig.ipc.pause_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":true}})";
        rig.d.handleMessage(makeMsg(-1327, 111, "/spause"));
        CHECK(has_post(rig, "pause"), "spause: vc.pause issued");
        CHECK(rig.tg.replies.empty(),
              "spause: success card suppressed");
    }
    {
        Rig rig;
        rig.store.set("call:-1328", "1");
        rig.ipc.resume_response =
            R"({"now_playing":{"track":{"id":"t1","title":"Test Song","duration_sec":200},"position_sec":30,"paused":false}})";
        rig.d.handleMessage(makeMsg(-1328, 111, "/sresume"));
        CHECK(has_post(rig, "resume"), "sresume: vc.resume issued");
        CHECK(rig.tg.replies.empty(),
              "sresume: success card suppressed");
    }
    {
        // /recognize: usage without a replied audio/voice message.
        Rig rig;
        rig.d.handleMessage(makeMsg(-1331, 111, "/recognize"));
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /recognize"),
              "recognize no-reply: usage");
        CHECK(!has_post(rig, "audio.recognize"),
              "recognize no-reply: no audio.recognize posted");
    }
    {
        Rig rig;
        parlay::TgMessage m = makeMsg(-1332, 111, "/recognize");
        m.has_reply_to = true;
        m.reply_to_message_id = 7;
        m.has_reply_media = true;
        m.reply_media_file_id = "PHOTO1";
        m.reply_media_kind = "photo";
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Usage: /recognize"),
              "recognize non-audio reply: usage");
    }
    {
        // Recognized: the identified track is named.
        Rig rig;
        rig.ipc.recognize_response =
            R"({"recognized":true,"name":"Song Title","artists":"Some Artist","album":"Some Album","source":"audd"})";
        parlay::TgMessage m = makeMsg(-1333, 111, "/recognize");
        m.has_reply_to = true;
        m.reply_to_message_id = 7;
        m.has_reply_media = true;
        m.reply_media_file_id = "VOICE1";
        m.reply_media_kind = "voice";
        rig.d.handleMessage(m);
        CHECK(has_post(rig, "audio.recognize"),
              "recognize match: audio.recognize issued");
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0], "Recognized:") &&
                  contains(rig.tg.replies[0], "Song Title") &&
                  contains(rig.tg.replies[0], "Some Artist"),
              "recognize match: identified track named");
    }
    {
        // Unconfigured provider: honest feature-unavailable reply,
        // never a fabricated identification, no setup text leaked.
        Rig rig;
        rig.ipc.recognize_error =
            "Music recognition is not configured: set AUDD_API_TOKEN "
            "(from dashboard.audd.io) in the environment";
        parlay::TgMessage m = makeMsg(-1334, 111, "/recognize");
        m.has_reply_to = true;
        m.reply_to_message_id = 7;
        m.has_reply_media = true;
        m.reply_media_file_id = "AUDIO1";
        m.reply_media_kind = "audio";
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  rig.tg.replies[0] ==
                      "Music recognition is not configured.",
              "recognize unconfigured: honest unavailable reply");
    }
    {
        // Provider finds no match: honest not-recognized reply.
        Rig rig;
        rig.ipc.recognize_response = R"({"recognized":false})";
        parlay::TgMessage m = makeMsg(-1335, 111, "/recognize");
        m.has_reply_to = true;
        m.reply_to_message_id = 7;
        m.has_reply_media = true;
        m.reply_media_file_id = "VOICE1";
        m.reply_media_kind = "voice";
        rig.d.handleMessage(m);
        CHECK(rig.tg.replies.size() == 1 &&
                  contains(rig.tg.replies[0],
                           "Could not recognize this track."),
              "recognize no-match: honest not-recognized reply");
    }

    // --- Package 3, R2: configured default-thumbnail fallback -----
    // Exercised through /play -> finishPlay -> sendNowPlayingCard.
    {
        auto npNoThumb = std::string(
            R"({"now_playing":{"track":{"id":"t1","title":"No Thumb Song","artist":"A","duration_sec":200},"position_sec":0,"paused":false}})");
        {
            Rig rig;
            rig.cfg.default_thumbnail =
                "https://example.invalid/default.jpg";
            rig.ipc.play_response = npNoThumb;
            rig.d.handleMessage(makeMsg(-1101, 111, "/play No Thumb Song"));
            CHECK(rig.tg.photos.size() == 1 &&
                      rig.tg.photos[0] ==
                          "https://example.invalid/default.jpg",
                  "R2: default thumbnail used when the track has none");
        }
        {
            Rig rig;
            rig.ipc.play_response = npNoThumb;
            rig.d.handleMessage(makeMsg(-1102, 111, "/play No Thumb Song"));
            CHECK(rig.tg.photos.empty(),
                  "R2: text card when neither track nor default "
                  "thumbnail exists");
        }
        {
            Rig rig;
            rig.cfg.default_thumbnail =
                "https://example.invalid/default.jpg";
            rig.ipc.seedSetting(-1103, "thumbnails", "false");
            rig.ipc.play_response = npNoThumb;
            rig.d.handleMessage(makeMsg(-1103, 111, "/play No Thumb Song"));
            CHECK(rig.tg.photos.empty(),
                  "R2: thumbnails-off still forces a text card");
        }
        {
            Rig rig;
            rig.cfg.default_thumbnail =
                "https://example.invalid/default.jpg";
            rig.ipc.play_response =
                R"({"now_playing":{"track":{"id":"t1","title":"Thumb Song","artist":"A","duration_sec":200,"thumbnail":"https://example.invalid/track.jpg"},"position_sec":0,"paused":false}})";
            rig.d.handleMessage(makeMsg(-1104, 111, "/play Thumb Song"));
            CHECK(rig.tg.photos.size() == 1 &&
                      rig.tg.photos[0] ==
                          "https://example.invalid/track.jpg",
                  "R2: track thumbnail takes precedence over the "
                  "default");
        }
    }

    // --- Package 3, R4: italic page footer on search pages ---------
    // Exercised through /search -> searchResultsText, then the page
    // callback -> the same text edited in place.
    {
        auto searchJson = [](int n) {
            std::string arr = "{\"results\":[";
            for (int i = 0; i < n; ++i) {
                if (i) arr += ",";
                arr += "{\"id\":\"t" + std::to_string(i) +
                       "\",\"title\":\"Song " + std::to_string(i) +
                       "\",\"url\":\"https://example.invalid/" +
                       std::to_string(i) + "\",\"artist\":\"A\"}";
            }
            arr += "]}";
            return arr;
        };
        {
            Rig rig;
            rig.ipc.search_response = searchJson(25);
            rig.d.handleMessage(makeMsg(-1001, 111, "/search q"));
            CHECK(rig.tg.replies.size() == 1 &&
                      contains(rig.tg.replies[0], "<i>Page 1 of 3</i>"),
                  "R4: search page 1 carries the italic Page 1 of 3 "
                  "footer");
            rig.d.handleCallback(makeCq(
                "pl:search:page:111:2:" +
                    parlay::ui_texts::queryHash("q"),
                -1001, 111, 1));
            CHECK(!rig.tg.edits.empty() &&
                      contains(rig.tg.edits[0].text, "<i>Page 2 of 3</i>"),
                  "R4: search page 2 carries the italic Page 2 of 3 "
                  "footer");
            rig.d.handleCallback(makeCq(
                "pl:search:page:111:3:" +
                    parlay::ui_texts::queryHash("q"),
                -1001, 111, 1));
            CHECK(rig.tg.edits.size() >= 2 &&
                      contains(rig.tg.edits[1].text, "<i>Page 3 of 3</i>"),
                  "R4: last search page carries the italic Page 3 of "
                  "3 footer");
        }
        {
            Rig rig;
            rig.ipc.search_response = searchJson(3);
            rig.d.handleMessage(makeMsg(-1001, 111, "/search q"));
            CHECK(rig.tg.replies.size() == 1 &&
                      !contains(rig.tg.replies[0], "Page "),
                  "R4: single-page search carries no page footer");
        }
    }

    // --- Package 3, R5: collection photo card + Download N songs ---
    {
        std::filesystem::create_directories("/tmp/parlay-dispatcher-test");
        {
            std::ofstream a("/tmp/parlay-dispatcher-test/coll-a.mp3");
            std::ofstream b("/tmp/parlay-dispatcher-test/coll-b.mp3");
        }
        Rig rig;
        rig.store.set("dlpick:run123456",
                      "https://open.spotify.com/playlist/xyz\n-1001");
        rig.ipc.spotify_collection_response =
            R"({"kind":"playlist","name":"Chill Mix","total":2,"tracks":[{"title":"A","artist":"X","thumbnail":"https://example.invalid/cover.jpg"},{"title":"B","artist":"Y","thumbnail":"https://example.invalid/cover2.jpg"}],"truncated":false})";
        rig.d.handleCallback(makeCq("pl:dl:1by1:run123456"));
        CHECK(rig.tg.photos.size() == 1 &&
                  rig.tg.photos[0] == "https://example.invalid/cover.jpg",
              "R5: collection card sent as a photo with the cover");
        bool card_ok = false;
        for (const auto& r : rig.tg.replies)
            if (contains(r, "Chill Mix") && contains(r, "2 songs"))
                card_ok = true;
        CHECK(card_ok && !rig.tg.markups.empty() &&
                  contains(rig.tg.markups.back(), "pl:dl:go:run123456"),
              "R5: collection card caption + Download N songs button");
        rig.ipc.download_response =
            R"({"count":2,"file_paths":["/tmp/parlay-dispatcher-test/coll-a.mp3","/tmp/parlay-dispatcher-test/coll-b.mp3"],"run_id":"x"})";
        size_t toasts_before = rig.tg.toasts.size();
        rig.d.handleCallback(makeCq("pl:dl:go:run123456"));
        CHECK(rig.tg.audios.size() == 2,
              "R5: Download N songs sends the per-track files");
        CHECK(rig.tg.toasts.size() > toasts_before &&
                  contains(rig.tg.toasts.back().text,
                           "Successfully uploaded 2 from a Spotify "
                           "playlist"),
              "R5: collection completion report follows the files");
    }

    // --- Package 3, R6: Check Again retry on not-available ---------
    {
        Rig rig;
        rig.ipc.download_error = "Track is not available yet";
        rig.d.handleMessage(
            makeMsg(-1121, 111, "/download some song"));
        CHECK(!rig.tg.edits.empty() &&
                  contains(rig.tg.edits[0].text, "not available"),
              "R6: not-available failure renders the retry card");
        CHECK(!rig.tg.edit_markups.empty() &&
                  contains(rig.tg.edit_markups[0], "pl:dl:retry:"),
              "R6: retry card carries the Check Again button");
        std::string run;
        {
            const std::string& m = rig.tg.edit_markups[0];
            size_t p = m.find("pl:dl:retry:");
            if (p != std::string::npos) {
                size_t e = m.find('|', p);
                run = m.substr(p + std::string("pl:dl:retry:").size(),
                               e == std::string::npos
                                   ? std::string::npos
                                   : e - p -
                                         std::string("pl:dl:retry:").size());
            }
        }
        CHECK(!run.empty(), "R6: retry run id recoverable from the card");
        size_t edits_before = rig.tg.edits.size();
        rig.d.handleCallback(
            makeCq("pl:dl:retry:" + run, -1121, 111, 1));
        CHECK(rig.tg.toasts.size() == 1 &&
                  rig.tg.toasts[0].text ==
                      "Track is still being processed. Please try "
                      "again later" &&
                  !rig.tg.toasts[0].alert,
              "R6: still-unavailable tap answers the non-alert "
              "still-processing toast");
        CHECK(rig.tg.edits.size() == edits_before,
              "R6: still-unavailable tap leaves the card in place");
        // Now the track becomes available: success replaces the card.
        rig.ipc.download_error.clear();
        rig.ipc.download_response =
            R"({"track":{"title":"Some Song","artist":"A"},"file_path":"/tmp/parlay-dispatcher-test/retry-song.mp3"})";
        {
            std::ofstream f("/tmp/parlay-dispatcher-test/retry-song.mp3");
        }
        rig.d.handleCallback(
            makeCq("pl:dl:retry:" + run, -1121, 111, 1));
        CHECK(rig.tg.audios.size() == 1,
              "R6: retry success delivers the audio file");
        CHECK(rig.tg.edits.size() > edits_before &&
                  contains(rig.tg.edits.back().text,
                           "Song downloaded successfully!"),
              "R6: retry success replaces the failure card");
    }
    {
        // A generic download failure is NOT a retry card.
        Rig rig;
        rig.ipc.download_error = "YouTube lookup failed";
        rig.d.handleMessage(
            makeMsg(-1121, 111, "/download some song"));
        CHECK(!rig.tg.edits.empty() &&
                  contains(rig.tg.edits[0].text, "Download failed") &&
                  (rig.tg.edit_markups.empty() ||
                   !contains(rig.tg.edit_markups[0], "pl:dl:retry:")),
              "R6: other failures render a plain failure card, no "
              "retry button");
    }

    // --- Package 3, R1/R7: inline result card buttons --------------
    // Exercised through the inline query: the FakeSender records the
    // Bot API-shaped results, so the test sees the exact markup the
    // core hands to answerInlineQuery.
    {
        Rig rig;
        rig.ipc.inline_response =
            R"({"results":[{"id":"yt:abc","title":"Song","artist":"Artist","url":"https://www.youtube.com/watch?v=abc","duration_sec":200,"source":"youtube"}]})";
        rig.d.handleInlineQuery(makeIq("bohemian"));
        CHECK(rig.tg.inlines.size() == 1,
              "R1/R7: inline search answers one result set");
        std::string dump = rig.tg.inlines[0].results_json;
        CHECK(contains(dump, "Copy link") &&
                  contains(dump, "copy_text") &&
                  contains(dump, "https://www.youtube.com/watch?v=abc"),
              "R1: inline search result carries a Copy link button "
              "with the track URL");
        CHECK(contains(dump, "Open in Parlay Bot") &&
                  contains(dump, "https://t.me/parlaybot?start=spotify----yt:abc"),
              "R7: inline search result carries the deep-link button "
              "with a well-formed bot link");
    }
    {
        // np results carry no deep-link button (with_button=false).
        Rig rig;
        rig.ipc.inline_response =
            R"({"results":[{"id":"t1","title":"T","artist":"A","duration_sec":200,"stream_url":"https://example.invalid/audio.webm","thumbnail":"https://example.invalid/t.jpg","source":"youtube"}]})";
        rig.d.handleMessage(makeMsg(-1001, 5001, "hello"));
        rig.d.handleInlineQuery(makeIq("np", 5001));
        CHECK(rig.tg.inlines.size() == 1 &&
                  !contains(rig.tg.inlines[0].results_json,
                            "Open in Parlay Bot"),
              "R7: np inline result carries no deep-link button");
    }


    std::filesystem::remove_all("/tmp/parlay-dispatcher-test");
    if (failures == 0) {
        std::cout << "dispatcher_test: ALL PASS\n";
        return 0;
    }
    std::cout << "dispatcher_test: " << failures << " FAILURE(S)\n";
    return 1;
}
