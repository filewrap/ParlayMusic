// routing_test.cpp — Phase 2 (§7.5) routing-table test.
//
// What it proves, derived from the same sources the routing table
// (contracts/routing-table.md) was built from — the callback builders
// + keyboard builders in services/core-cpp/src/ui_texts.hpp, the
// callback router in services/core-cpp/src/dispatcher.cpp
// (handleCallback + the 18 cb* sub-handlers), and registerCommands —
// so the table and the test cannot drift apart silently:
//
//  1. Every callback_data value the UI can emit is <= 64 bytes
//     (the Bot API limit), including the 64-byte boundary itself.
//  2. Every emitted callback routes to a handler that ANSWERS the
//     callback query (toast recorded) — zero unhandled callbacks.
//     One deliberate exception, recorded in the table:
//     (a) pl:vote:* — gap G2, FIXED 2026-11-03: cbVote answers
//         every tap (confirmation toast "Vote recorded." on a
//         counted vote, honest alert on unknown/foreign votes) and
//         still edits the card; the test asserts the toast (and the
//         card edit), and asserts an unseeded vote is answered
//         with "Unknown action." instead of being ignored.
//     (b) pl:restart:bot:<chat_id> exits the process by design
//         (supervisor restart) — it is length- and table-checked
//         only, NEVER dispatched.
//  3. The default branch answers every unknown callback (unknown
//     domain, non-pl: data, empty data, >64 bytes, unknown verb)
//     with the "Unknown action." alert — never silence, never a crash.
//  4. Every persistent reply-keyboard button is routed: in a DM it
//     returns the start card; in a group it runs the claimed action
//     (gap G1, FIXED 2026-11-03) — Music returns the start card,
//     Global Search asks for a query, Settings opens the settings
//     panel, About returns the about card.
//  5. Table coverage: every callback template, every keyboard button
//     label, and every command name / alias / channel command
//     re-derived from dispatcher.cpp appears in
//     contracts/routing-table.md. A callback, button, or command that
//     exists in code but not in the table fails this test.
//  6. Anti-drift net: the test harvests every callback_data out of
//     every *Keyboard builder by calling it, and requires each
//     harvested value to match a template in the list below. A button
//     added to an existing keyboard builder is picked up
//     automatically and fails the test until its template + table row
//     exist.
//
// Build: g++ -std=c++20 -I src tests/routing_test.cpp
//   src/dispatcher.cpp src/ipc.cpp src/store.cpp src/scram.cpp src/json.cpp
//   src/config.cpp src/toml.cpp src/rate_limiter.cpp src/tdlib_client.cpp
//   <path>/libtdjson.so.1.8.69 -Wl,-rpath,<dir>
//   -o /tmp/routing_test && /tmp/routing_test <routing-table.md>
//   <dispatcher.cpp>
// (CMake target `routing_test` passes both paths; the defaults below
// cover a manual run from services/core-cpp.)
// Exit 0 = all checks pass. No network: Telegram/IPC/TDLib are faked,
// the Store uses its JSON file backend in a temp dir.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "dispatcher.hpp"
#include "ipc.hpp"
#include "json.hpp"
#include "rate_limiter.hpp"
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

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Split on ':'; the last template segment may absorb the remainder.
std::vector<std::string> splitColon(const std::string& s) {
    std::vector<std::string> out;
    size_t b = 0;
    while (true) {
        size_t e = s.find(':', b);
        if (e == std::string::npos) {
            out.push_back(s.substr(b));
            break;
        }
        out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// True when `concrete` matches `tpl`: same segments, except a
// template segment "<...>" matches anything, and a trailing
// placeholder segment absorbs any remaining concrete segments.
bool templateMatches(const std::string& tpl, const std::string& concrete) {
    std::vector<std::string> t = splitColon(tpl);
    std::vector<std::string> c = splitColon(concrete);
    if (c.size() < t.size()) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        bool wild = t[i].size() >= 2 && t[i].front() == '<' &&
                    t[i].back() == '>';
        if (wild) {
            if (i == t.size() - 1) return true;  // absorbs remainder
            continue;
        }
        if (i >= c.size() || t[i] != c[i]) return false;
    }
    return c.size() == t.size();
}

// --- fakes -------------------------------------------------------------------

class FakeTelegramSender : public parlay::TelegramSender {
public:
    std::vector<std::string> replies;
    std::vector<std::string> markups;
    std::vector<std::string> photos;
    struct EditCall {
        int64_t chat, msg;
        std::string text, kind;
    };
    std::vector<EditCall> edits;
    struct ToastCall {
        std::string id, text;
        bool alert;
    };
    std::vector<ToastCall> toasts;
    std::vector<std::pair<int64_t, int64_t>> deleted;
    std::vector<std::string> inlines;
    int64_t next_id = 1000;

    static std::string serialiseKb(const parlay::InlineKeyboard& kb) {
        std::string out;
        for (const auto& row : kb) {
            out += "[";
            for (const auto& b : row) {
                out += b.text + "=";
                if (!b.callback_data.empty()) out += b.callback_data;
                else if (!b.url.empty()) out += b.url;
                else if (b.has_switch) out += b.switch_inline_query;
                out += "|";
            }
            out += "]";
        }
        return out;
    }

    bool sendMessage(int64_t, const std::string& text,
                     std::string&) override {
        replies.push_back(text);
        return true;
    }
    int64_t sendMessageId(int64_t, const std::string& text, int64_t = 0,
                          std::string* = nullptr) override {
        replies.push_back(text);
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageHtml(int64_t, const std::string& text,
                            const parlay::InlineKeyboard& kb, int64_t = 0,
                            std::string* = nullptr) override {
        replies.push_back(text);
        markups.push_back(serialiseKb(kb));
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageReplyKb(int64_t, const std::string& text,
                               const parlay::ReplyKeyboard& kb, int64_t = 0,
                               std::string* = nullptr) override {
        replies.push_back(text);
        std::string flat;
        for (const auto& row : kb.rows)
            for (const auto& label : row) flat += label + "|";
        markups.push_back(flat);
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendMessageRemoveKb(int64_t, const std::string& text,
                                int64_t = 0,
                                std::string* = nullptr) override {
        replies.push_back(text);
        markups.push_back("");
        return static_cast<int64_t>(replies.size());
    }
    int64_t sendPhoto(int64_t, const std::string& photo,
                      const std::string& caption,
                      const parlay::InlineKeyboard& kb,
                      std::string* = nullptr) override {
        photos.push_back(photo);
        replies.push_back(caption);
        markups.push_back(serialiseKb(kb));
        return static_cast<int64_t>(replies.size());
    }
    bool editMessageText(int64_t chat, int64_t msg, const std::string& text,
                         const parlay::InlineKeyboard& kb,
                         std::string* = nullptr) override {
        (void)kb;
        edits.push_back({chat, msg, text, "text"});
        return true;
    }
    bool editMessageCaption(int64_t chat, int64_t msg,
                            const std::string& caption,
                            const parlay::InlineKeyboard& kb,
                            std::string* = nullptr) override {
        (void)kb;
        edits.push_back({chat, msg, caption, "caption"});
        return true;
    }
    bool editMessageReplyMarkup(int64_t chat, int64_t msg,
                                const parlay::InlineKeyboard& kb,
                                std::string* = nullptr) override {
        edits.push_back({chat, msg, serialiseKb(kb), "markup"});
        return true;
    }
    bool answerCallbackQuery(const std::string& id, const std::string& text,
                             bool alert, int = 0,
                             std::string* = nullptr) override {
        toasts.push_back({id, text, alert});
        return true;
    }
    bool answerInlineQuery(const std::string& id, const parlay::Json& results,
                           int, const std::string&, const std::string&,
                           std::string* = nullptr) override {
        inlines.push_back(id + ":" + results.dump());
        return true;
    }
    bool downloadFile(const std::string&, const std::string&,
                      std::string& saved, std::string&) override {
        saved = "/tmp/parlay-routing-test/dl.ogg";
        return true;
    }
    std::string getChatMemberStatus(int64_t, int64_t) override {
        return "member";
    }
    std::vector<int64_t> getChatAdministrators(int64_t) override {
        return {999};
    }
    int64_t getChatId(const std::string&) override { return 0; }
    bool deleteMessage(int64_t chat, int64_t msg) override {
        deleted.emplace_back(chat, msg);
        return true;
    }
    bool leaveChat(int64_t) override { return true; }
    bool forwardMessage(int64_t, int64_t, int64_t) override { return true; }
    bool copyMessage(int64_t, int64_t, int64_t) override { return true; }
    int64_t sendDocument(int64_t, const std::string&, const std::string&,
                         const std::string&,
                         std::string* = nullptr) override {
        return ++next_id;
    }
    int64_t sendAudioFile(int64_t, const std::string&, const std::string&,
                          std::string* = nullptr) override {
        return ++next_id;
    }
    int64_t sendFile(int64_t, const std::string&, const std::string&,
                     std::string* = nullptr) override {
        return ++next_id;
    }
    int64_t sendAudio(int64_t, const std::string&, const std::string&,
                      const parlay::InlineKeyboard&,
                      std::string* = nullptr) override {
        return ++next_id;
    }
    std::string getUserMention(int64_t user_id) override {
        return "User " + std::to_string(user_id);
    }
};

// Fake backend: a playing track exists, every vc.* action succeeds,
// vote/fx/dislike succeed, Spotify-playlist delivery succeeds empty.
// Anything else 404s, like a backend that does not implement it.
class FakeIpc : public parlay::IpcClient {
public:
    explicit FakeIpc(const parlay::Config& cfg) : parlay::IpcClient(cfg) {}
    parlay::IpcResult ok(parlay::Json data) const {
        parlay::IpcResult r;
        r.ok = true;
        r.http_status = 200;
        r.data = std::move(data);
        return r;
    }
    parlay::IpcResult fail404() const {
        parlay::IpcResult r;
        r.ok = false;
        r.http_status = 404;
        r.error_code = "not_found";
        r.error_message = "not found";
        return r;
    }
    parlay::Json nowPlaying() const {
        return parlay::Json::parse(
            R"({"now_playing":{"track":{"id":"t1","title":"T","duration_sec":200,"url":"https://example.invalid/t"},"position_sec":30,"paused":false,"is_muted":false},"queue":[]})");
    }
    parlay::IpcResult post(const std::string&, const std::string& action,
                           const parlay::Json&,
                           int = 30) const override {
        if (action == "vote" || action == "dislike") return ok(parlay::Json::parse(R"({})"));
        if (action == "spotify.analyze")
            return ok(parlay::Json::parse(
                R"({"analysis":"Clean lyrics, upbeat song.","profane_words":[]})"));
        if (action == "fx")
            return ok(parlay::Json::parse(
                R"({"output_path":"/tmp/parlay-routing-test/fx-out.ogg"})"));
        // quality.get (Opus quality surface, 2026-11-05): canned
        // effective config at the locked defaults, library in sync.
        if (action == "quality.get")
            return ok(parlay::Json::parse(
                R"({"chat_id":-1001234567890,"effective":{"bitrate_bps":192000,"frame_ms":20,"complexity":10,"vbr":true,"fec":false,"dtx":false,"max_playback_rate_hz":48000},"sources":{"bitrate_bps":"default","frame_ms":"default","complexity":"default","vbr":"default","fec":"default","dtx":"default"},"library":{"bitrate_bps":192000,"frame_ms":20,"complexity":10,"vbr":true,"fec":false,"dtx":false,"max_playback_rate_hz":48000},"active_call":false,"applies_on":"next join"})"));
        if (action == "download/spotify-playlist")
            return ok(parlay::Json::parse(
                R"({"count":0,"file_path":"","file_paths":[],"run_id":"run123456"})"));
        if (action == "pause" || action == "resume" || action == "seek" ||
            action == "skip" || action == "stop" || action == "autoplay" ||
            action == "shuffle" || action == "volume" || action == "queue" ||
            action == "play" || action == "mute" || action == "loop")
            return ok(nowPlaying());
        return fail404();
    }
    parlay::IpcResult get(const std::string&, const std::string& path,
                          int = 10) const override {
        if (contains(path, "now-playing")) return ok(nowPlaying());
        if (contains(path, "queue"))
            return ok(parlay::Json::parse(R"({"queue":[]})"));
        // spotify.details (E8): the Phase 3 companion surface.
        if (contains(path, "spotify/details"))
            return ok(parlay::Json::parse(
                R"({"track":{"id":"track1","title":"Test Track","artist":"Test Artist","album":"Test Album","duration_sec":200,"url":"https://open.spotify.com/track/track1"},"features":{"key":"C","modality":"Major","tempo_bpm":120,"acousticness":0.1,"danceability":0.8,"energy":0.9,"instrumentalness":0.0,"liveness":0.2,"speechiness":0.05,"valence":0.7},"genres":["pop"],"disliked_by":[111,222],"ignored_by":[],"profanity":null,"language":null,"lyrics":"la la la","lyrics_link":"https://example.invalid/lyrics"})"));
        return fail404();
    }
};

class FakeAssistant : public parlay::TdLibClient {
public:
    FakeAssistant() : parlay::TdLibClient(parlay::SessionKind::Assistant) {}
    bool available() const override { return true; }
    bool authorized() const override { return true; }
    std::string lastError() const override { return ""; }
    parlay::Json call(const parlay::Json& req, double = 30.0) override {
        const parlay::Json* t = req.find("@type");
        std::string type = t ? t->asString() : "";
        if (type == "getChat")
            return parlay::Json::parse(
                R"({"@type":"chat","video_chat":{"group_call_id":0}})");
        return parlay::Json::parse(
            R"({"@type":"error","code":400,"message":"fake assistant: unexpected call"})");
    }
};

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
              c.data_dir = "/tmp/parlay-routing-test";
              c.owner_id = 999;
              return c;
          }()),
          store(cfg),
          ipc(cfg),
          limiter(cfg.rate_limit),
          d(cfg, tg, ipc, store, assistant, limiter, "parlaybot") {}
};

parlay::TgCallbackQuery makeCq(const std::string& data, int64_t chat = -1001234567890) {
    // Monotonic callback-query ids (avoid collisions across dispatches).
    static int n = 0;
    parlay::TgCallbackQuery cq;
    cq.id = "cq-" + std::to_string(++n);
    cq.from.id = 999;  // owner: passes every control/admin gate
    cq.from.first_name = "Owner";
    cq.chat_id = chat;
    cq.message_id = 5;
    cq.has_message = true;
    cq.data = data;
    return cq;
}

parlay::TgMessage makeMsg(int64_t chat, const std::string& type,
                          const std::string& text) {
    parlay::TgMessage m;
    m.message_id = 1;
    m.from.id = 999;
    m.chat.id = chat;
    m.chat.type = type;
    m.text = text;
    m.has_text = true;
    return m;
}

// One callback template: the shape as it appears in
// contracts/routing-table.md, one concrete example built by calling
// the real builder, and whether dispatching it is safe (false only
// for pl:restart:bot, which exits the process by design).
struct TemplateCase {
    std::string tpl;
    std::string concrete;
    bool dispatch = true;
};

void harvestKb(const parlay::InlineKeyboard& kb,
               std::set<std::string>& out) {
    for (const auto& row : kb)
        for (const auto& b : row)
            if (!b.callback_data.empty()) out.insert(b.callback_data);
}

}  // namespace

int main(int argc, char** argv) {
    std::string table_path = argc > 1 ? argv[1]
                                      : "contracts/routing-table.md";
    std::string dispatcher_path = argc > 2 ? argv[2]
                                           : "src/dispatcher.cpp";
    std::string table = readFile(table_path);
    CHECK(!table.empty(), "routing table file readable");
    std::string dispatcher_src = readFile(dispatcher_path);
    CHECK(!dispatcher_src.empty(), "dispatcher.cpp source readable");

    std::error_code ec;
    std::filesystem::remove_all("/tmp/parlay-routing-test", ec);

    using namespace parlay::ui_texts;
    const int64_t kChat = -1001234567890LL;
    const int64_t kUser = 999;
    const std::string kToken = "abcdef0123";
    const std::string kRun = "run123456";
    const std::string kHash = queryHash("test query");

    // --- A. Harvest every callback_data out of every keyboard builder.
    std::set<std::string> harvested;
    harvestKb(helpMenuKeyboard(), harvested);
    harvestKb(helpTopicKeyboard(), harvested);
    harvestKb(startPmKeyboard("parlaybot", "https://t.me/updates",
                              "https://t.me/support",
                              "https://example.invalid/src"),
              harvested);
    harvestKb(startGroupKeyboard("https://t.me/updates",
                                 "https://t.me/support"),
              harvested);
    harvestKb(playerKeyboard(kChat, false, true, 0, kToken, true, 90, 200),
              harvested);
    harvestKb(playerKeyboard(kChat, true, false, 3, "", false, 0, 0),
              harvested);
    harvestKb(queuedKeyboard(kToken), harvested);
    harvestKb(queuedKeyboard(""), harvested);
    harvestKb(stopConfirmKeyboard(kChat, true), harvested);
    harvestKb(stopConfirmKeyboard(kChat, false), harvested);
    harvestKb(restartConfirmKeyboard(kChat), harvested);
    harvestKb(menuPanelKeyboard(kChat), harvested);
    {
        std::vector<CardTrack> shown = {
            {"Song One", "https://x/1", "3:24", "Alice", kToken},
            {"Song Two", "https://x/2", "2:00", "Bob", "0123456789"},
        };
        harvestKb(queueKeyboard(kChat, shown, true, 5), harvested);
    }
    {
        std::vector<std::pair<int, std::string>> picks;
        for (int i = 1; i <= 10; ++i)
            picks.emplace_back(i, cbSearchPick(i, kUser, "tok"));
        harvestKb(searchKeyboard(picks), harvested);
    }
    harvestKb(searchPageKeyboard(kUser, 2, kHash, true, true, 5), harvested);
    harvestKb(searchPageKeyboard(kUser, 1, kHash, false, true, 5), harvested);
    harvestKb(lossyKeyboard(kRun, "320kbps MP3"), harvested);
    harvestKb(dlSpotifyOptionsKeyboard(kRun), harvested);
    harvestKb(dlFxKeyboard(kRun), harvested);
    harvestKb(settingsKeyboard(ChatSettings{}), harvested);
    harvestKb(settingsQualityKeyboard(QualityInfo{}), harvested);
    harvestKb(autoplayCardKeyboard(true), harvested);
    harvestKb(autoplayCardKeyboard(false), harvested);
    harvestKb(langPickerKeyboard("start"), harvested);
    harvestKb(langPickerKeyboard("help"), harvested);
    harvestKb(langPickerKeyboard("settings"), harvested);
    harvestKb(dlSettingsIndexKeyboard(), harvested);
    harvestKb(dlSettingsFormatKeyboard(), harvested);
    harvestKb(dlSettingsYoutubeKeyboard(), harvested);
    harvestKb(settingsIndexKeyboard(), harvested);
    harvestKb(settingsToggleKeyboard("page", true), harvested);
    harvestKb(settingsToggleKeyboard("musicdl", false), harvested);
    harvestKb(authlistKeyboard(
                  kChat, {{"Alice", 111}, {"Bob", 222}}),
              harvested);
    harvestKb(shareHelpKeyboard(), harvested);
    harvestKb(shareReceivedKeyboard("111", "222", "unique1"), harvested);
    harvestKb(deleteConfirmKeyboard("track123"), harvested);
    harvestKb(bcastKeyboard(), harvested);

    // --- B. Templates: one per distinct callback shape in the table.
    // Concrete examples are built by calling the real builders, so a
    // builder that stops emitting the table's shape fails here.
    std::vector<TemplateCase> templates = {
        {"pl:help:main", kCbHelpMain},
        {"pl:help:<topic>", cbHelp("admins")},
        {"pl:help:<topic>", cbHelp("owner")},
        {"pl:help:<topic>", cbHelp("radio")},
        {"pl:help:<topic>", cbHelp("search")},
        {"pl:start", kCbStart},
        {"pl:close", kCbClose},
        {"pl:player:progress:<chat_id>", cbPlayer("progress", kChat)},
        {"pl:player:pause:<chat_id>", cbPlayer("pause", kChat)},
        {"pl:player:resume:<chat_id>", cbPlayer("resume", kChat)},
        {"pl:player:replay:<chat_id>", cbPlayer("replay", kChat)},
        {"pl:player:skip:<chat_id>", cbPlayer("skip", kChat)},
        {"pl:player:stop:<chat_id>", cbPlayer("stop", kChat)},
        {"pl:player:seekback15:<chat_id>", cbPlayer("seekback15", kChat)},
        {"pl:player:seek15:<chat_id>", cbPlayer("seek15", kChat)},
        {"pl:player:autoplay:<chat_id>", cbPlayer("autoplay", kChat)},
        {"pl:player:shuffle:<chat_id>", cbPlayer("shuffle", kChat)},
        {"pl:player:mute:<chat_id>", cbPlayer("mute", kChat)},
        {"pl:player:unmute:<chat_id>", cbPlayer("unmute", kChat)},
        {"pl:player:voldec:<chat_id>", cbPlayer("voldec", kChat)},
        {"pl:player:volinc:<chat_id>", cbPlayer("volinc", kChat)},
        {"pl:player:refresh:<chat_id>", cbPlayer("refresh", kChat)},
        {"pl:player:thumb:<chat_id>", cbPlayer("thumb", kChat)},
        {"pl:player:playnow:<token>", cbPlayerToken("playnow", kToken)},
        {"pl:player:addlist:<token>", cbPlayerToken("addlist", kToken)},
        {"pl:player:confirm:resume:<chat_id>",
         "pl:player:confirm:resume:" + std::to_string(kChat)},
        {"pl:player:confirm:unmute:<chat_id>",
         "pl:player:confirm:unmute:" + std::to_string(kChat)},
        {"pl:player:confirm:stop:<chat_id>",
         "pl:player:confirm:stop:" + std::to_string(kChat)},
        {"pl:queue:more:<chat_id>", cbQueueMore(kChat)},
        {"pl:queue:del:<chat_id>:<token>", cbQueueDel(kChat, kToken)},
        {"pl:set:main", kCbSetMain},
        {"pl:set:<key>", cbSet("adminmode")},
        {"pl:set:<key>", cbSet("page")},
        {"pl:set:<key>", cbSet("musicdl")},
        {"pl:set:info:<key>", cbSetInfo("adminmode")},
        {"pl:set:info:<key>", cbSetInfo("timer")},
        {"pl:set:timer:<n>", cbSetTimer(2)},
        {"pl:set:timer:<n>", cbSetTimer(7)},
        {"pl:set:page", cbSet("page")},
        {"pl:set:page:<0|1>", cbSet("page:1")},
        {"pl:set:musicdl", cbSet("musicdl")},
        {"pl:set:musicdl:<0|1>", cbSet("musicdl:0")},
        {"pl:set:quality", cbSet("quality")},
        {"pl:set:quality:br:<kbps>", cbSetQualityBr(192)},
        {"pl:set:quality:br:<kbps>", cbSetQualityBr(320)},
        {"pl:set:quality:fr:<ms>", cbSetQualityFr(20)},
        {"pl:set:quality:fr:<ms>", cbSetQualityFr(60)},
        {"pl:set:quality:reset", cbSet("quality:reset")},
        {"pl:lang:select", kCbLangSelect},
        {"pl:lang:select:<origin>", std::string(kCbLangSelect) + ":help"},
        {"pl:lang:select:<origin>",
         std::string(kCbLangSelect) + ":settings"},
        {"pl:lang:en", kCbLangEn},
        {"pl:lang:en:<origin>", std::string(kCbLangEn) + ":start"},
        {"pl:lang:en:<origin>", std::string(kCbLangEn) + ":help"},
        {"pl:lang:<code>", "pl:lang:fr"},
        {"pl:dl:cancel:<run_id>", cbDlCancel(kRun)},
        {"pl:dl:fx:<run_id>:orig", cbDlFx(kRun, "orig")},
        {"pl:dl:fx:<run_id>:slow", cbDlFx(kRun, "slow")},
        {"pl:dl:fx:<run_id>:nightcore", cbDlFx(kRun, "nightcore")},
        {"pl:dl:fx:<run_id>:bass", cbDlFx(kRun, "bass")},
        {"pl:dl:fx:<run_id>:8d", cbDlFx(kRun, "8d")},
        {"pl:dl:zip:<run_id>", "pl:dl:zip:" + kRun},
        {"pl:dl:1by1:<run_id>", "pl:dl:1by1:" + kRun},
        {"pl:bcast:cancel", kCbBcastCancel},
        {"pl:auth:del:<chat_id>:<user_id>", cbAuthDel(kChat, 111)},
        {"pl:search:<n>:<user_id>:<token>",
         cbSearchPick(3, kUser, kToken)},
        {"pl:search:page:<user_id>:<page>:<hash>",
         cbSearchPage(kUser, 2, kHash)},
        {"pl:vote:approve:<track_id>", cbVote("approve", "track1")},
        {"pl:vote:decline:<track_id>", cbVote("decline", "track1")},
        {"pl:share:mylink", "pl:share:mylink"},
        {"pl:share:cancel", "pl:share:cancel"},
        {"pl:share:feedback:<sender>:<msg>:<file_unique>",
         "pl:share:feedback:111:222:unique1"},
        {"pl:share:report:<sender>:<msg>", "pl:share:report:111:222"},
        {"pl:del:yes:<id>", cbDel("yes", "track123")},
        {"pl:del:no:<id>", cbDel("no", "track123")},
        {"pl:lossy:accept:<id>", cbLossy("accept", kRun)},
        {"pl:lossy:skip:<id>", cbLossy("skip", kRun)},
        {"pl:restart:bot:<chat_id>", cbRestart("bot", kChat), false},
        {"pl:restart:replay:<chat_id>", cbRestart("replay", kChat)},
        {"pl:dlset:of", "pl:dlset:of"},
        {"pl:dlset:uy", "pl:dlset:uy"},
        {"pl:dlset:pt", "pl:dlset:pt"},
        {"pl:dlset:back", "pl:dlset:back"},
        {"pl:dlset:fmt:<fmt>", "pl:dlset:fmt:mp3"},
        {"pl:dlset:fmt:<fmt>", "pl:dlset:fmt:flac"},
        {"pl:dlset:uy:<0|1>", "pl:dlset:uy:1"},
        {"pl:dlset:uy:<0|1>", "pl:dlset:uy:0"},
        {"pl:spotify:dislike:<id>", "pl:spotify:dislike:track1"},
        {"pl:spotify:<verb>", "pl:spotify:details"},
    };

    // --- C. Every template's concrete example matches its shape and
    // is <= 64 bytes; every template (or its concrete example)
    // appears in the routing table.
    for (const auto& t : templates) {
        CHECK(templateMatches(t.tpl, t.concrete),
              "template shape: " + t.tpl + " matches " + t.concrete);
        CHECK(t.concrete.size() <= 64,
              "callback <=64 bytes: " + t.concrete + " (" +
                  std::to_string(t.concrete.size()) + ")");
        CHECK(contains(table, t.tpl) || contains(table, t.concrete),
              "table contains template/concrete: " + t.tpl);
    }
    for (const auto& cb : harvested) {
        CHECK(cb.size() <= 64,
              "harvested callback <=64 bytes: " + cb + " (" +
                  std::to_string(cb.size()) + ")");
        bool covered = false;
        for (const auto& t : templates)
            if (templateMatches(t.tpl, cb)) covered = true;
        CHECK(covered, "harvested callback covered by a template: " + cb);
    }

    // --- D. Dispatch every template concrete through the real router
    // and assert the query is answered (and, for votes, the card
    // edit too — gap G2 fixed: votes answer with a toast now).
    {
        // Fresh store: earlier sections must not leak pending-state
        // keys (dlset_tpl_pending / sharefb) into this rig.
        std::error_code ec2;
        std::filesystem::remove_all("/tmp/parlay-routing-test", ec2);
        Rig rig;
        // Seeds for the handlers that need stored state to reach
        // their answering branch.
        rig.store.set("dlrun:" + kRun,
                      "/tmp/parlay-routing-test/fx-in.ogg\n-1001234567890");
        rig.store.set("dlpick:" + kRun,
                      "https://open.spotify.com/playlist/xyz\n-1001234567890");
        for (const auto& t : templates) {
            if (!t.dispatch) continue;  // pl:restart:bot exits (42)
            bool is_vote = t.tpl.rfind("pl:vote:", 0) == 0;
            size_t toasts_before = rig.tg.toasts.size();
            size_t edits_before = rig.tg.edits.size();
            if (is_vote)
                rig.store.set("vote:track1", "-1001234567890");
            rig.d.handleCallback(makeCq(t.concrete));
            CHECK(rig.tg.toasts.size() > toasts_before,
                  "callback answered: " + t.concrete);
            if (is_vote) {
                CHECK(rig.tg.edits.size() > edits_before,
                      "vote answered by card edit too: " + t.concrete);
                CHECK(rig.tg.toasts.back().text == "Vote recorded.",
                      "counted vote confirmed: " + t.concrete);
            }
        }
        // Every harvested callback is answered too (same set, but
        // this proves the keyboard output itself routes, not just the
        // hand-listed templates).
        for (const auto& cb : harvested) {
            bool is_vote = cb.rfind("pl:vote:", 0) == 0;
            bool is_restart_bot =
                cb.rfind("pl:restart:bot:", 0) == 0;
            if (is_restart_bot) continue;  // exits the process
            size_t toasts_before = rig.tg.toasts.size();
            size_t edits_before = rig.tg.edits.size();
            if (is_vote) rig.store.set("vote:track1", "-1001234567890");
            // Harvested vote callbacks carry track ids, not the
            // seeded "track1": seed the harvested id too.
            if (is_vote) {
                std::string tid = cb.substr(std::string("pl:vote:approve:").size());
                if (cb.rfind("pl:vote:decline:", 0) == 0)
                    tid = cb.substr(std::string("pl:vote:decline:").size());
                rig.store.set("vote:" + tid, "-1001234567890");
            }
            rig.d.handleCallback(makeCq(cb));
            CHECK(rig.tg.toasts.size() > toasts_before,
                  "harvested callback answered: " + cb);
            if (is_vote) {
                CHECK(rig.tg.edits.size() > edits_before,
                      "harvested vote answered by card edit too: " + cb);
                CHECK(rig.tg.toasts.back().text == "Vote recorded.",
                      "harvested counted vote confirmed: " + cb);
            }
        }
        // Unseeded vote: answered with "Unknown action." (gap G2
        // fixed) — never silently ignored.
        {
            size_t toasts_before = rig.tg.toasts.size();
            size_t edits_before = rig.tg.edits.size();
            rig.d.handleCallback(
                makeCq("pl:vote:approve:nothing-open"));
            CHECK(rig.tg.toasts.size() > toasts_before &&
                      rig.tg.toasts.back().text == "Unknown action." &&
                      rig.tg.edits.size() == edits_before,
                  "unseeded vote answered with Unknown action.");
        }
        // Foreign-chat vote: answered with "You aren't allowed".
        {
            rig.store.set("vote:track1", "-1009999999999");
            size_t toasts_before = rig.tg.toasts.size();
            size_t edits_before = rig.tg.edits.size();
            rig.d.handleCallback(makeCq("pl:vote:approve:track1"));
            CHECK(rig.tg.toasts.size() > toasts_before &&
                      rig.tg.toasts.back().text == "You aren't allowed" &&
                      rig.tg.edits.size() == edits_before,
                  "foreign-chat vote answered with You aren't allowed");
        }
    }

    // --- E. Default branch: unknown callbacks are answered, never
    // silent, never a crash. Includes the 64-byte boundary.
    {
        std::error_code ec2;
        std::filesystem::remove_all("/tmp/parlay-routing-test", ec2);
        Rig rig;
        const std::vector<std::pair<std::string, std::string>> unknowns = {
            {"pl:bogus:verb", "unknown domain"},
            {"hello", "non-pl: data"},
            {"", "empty data"},
            {"pl:player:unknownverb:-1001234567890", "unknown verb"},
        };
        for (const auto& [data, why] : unknowns) {
            size_t before = rig.tg.toasts.size();
            rig.d.handleCallback(makeCq(data));
            CHECK(rig.tg.toasts.size() > before,
                  "unknown callback answered: " + why);
            CHECK(rig.tg.toasts.back().text == "Unknown action." ||
                      rig.tg.toasts.back().text ==
                          "This feature is not available yet.",
                  "unknown callback toast is honest: " + why);
        }
        // spotify details/analyze are routed now (E8, Phase 3):
        // details renders the card in place, analyze renders the
        // analysis — neither is an unknown callback any more.
        {
            size_t edits_before = rig.tg.edits.size();
            rig.d.handleCallback(makeCq("pl:spotify:details:track1"));
            CHECK(rig.tg.edits.size() > edits_before &&
                      contains(rig.tg.edits.back().text, "Test Track") &&
                      contains(rig.tg.edits.back().text, "Key C Major") &&
                      contains(rig.tg.edits.back().text,
                               "Disliked by 2 people"),
                  "spotify details routed: card rendered in place");
            size_t edits_before_a = rig.tg.edits.size();
            rig.d.handleCallback(makeCq("pl:spotify:analyze:track1"));
            CHECK(rig.tg.edits.size() > edits_before_a &&
                      contains(rig.tg.edits.back().text,
                               "Lyrics analysis") &&
                      contains(rig.tg.edits.back().text, "Clean lyrics"),
                  "spotify analyze routed: analysis rendered in place");
        }
        // Exactly 64 bytes is accepted by the guard (routed, not
        // rejected); 65 bytes is rejected with the same alert.
        std::string sixty_four = "pl:set:info:" + std::string(52, 'x');
        CHECK(sixty_four.size() == 64, "64-byte probe is 64 bytes");
        {
            size_t before = rig.tg.toasts.size();
            rig.d.handleCallback(makeCq(sixty_four));
            CHECK(rig.tg.toasts.size() > before,
                  "64-byte callback answered (guard accepts 64)");
        }
        std::string sixty_five = "pl:set:info:" + std::string(53, 'x');
        CHECK(sixty_five.size() == 65, "65-byte probe is 65 bytes");
        {
            size_t before = rig.tg.toasts.size();
            rig.d.handleCallback(makeCq(sixty_five));
            CHECK(rig.tg.toasts.size() > before &&
                      rig.tg.toasts.back().text == "Unknown action.",
                  "65-byte callback rejected with Unknown action.");
        }
    }

    // --- F. Reply-keyboard buttons: every label is routed.
    {
        // Fresh store: section D dispatched pl:dlset:pt and
        // pl:share:feedback, which set pending-state keys for user
        // 999 — without this reset the first two labels would be
        // swallowed as pending-state text instead of reaching the
        // start-card path.
        std::error_code ec2;
        std::filesystem::remove_all("/tmp/parlay-routing-test", ec2);
        Rig rig;
        auto kb = persistentKeyboard();
        std::vector<std::string> labels;
        for (const auto& row : kb.rows)
            for (const auto& label : row) labels.push_back(label);
        CHECK(labels.size() == 4, "persistent keyboard has 4 buttons");
        for (const auto& label : labels) {
            CHECK(contains(table, label),
                  "table contains keyboard button: " + label);
            // DM: the start card is returned.
            size_t replies_before = rig.tg.replies.size();
            rig.d.handleMessage(makeMsg(999, "private", label));
            CHECK(rig.tg.replies.size() > replies_before &&
                      contains(rig.tg.replies.back(), "Parlay"),
                  "keyboard button routed in DM: " + label);
            // Group: the claimed action runs (gap G1 fixed) — the
            // reply lands in the group chat, never dropped.
            size_t replies_before_g = rig.tg.replies.size();
            rig.d.handleMessage(
                makeMsg(-1001234567890, "supergroup", label));
            CHECK(rig.tg.replies.size() > replies_before_g,
                  "keyboard button answered in group (gap G1 fixed): " +
                      label);
            const std::string& g = rig.tg.replies.back();
            if (label == "Music")
                CHECK(contains(g, "Parlay"),
                      "Music in group returns the start card");
            else if (label == "Global Search")
                CHECK(contains(g, "What do you want to search for?"),
                      "Global Search in group asks for a query");
            else if (label == "Settings")
                CHECK(contains(g, "Chat Settings"),
                      "Settings in group opens the settings panel");
            else if (label == "About")
                CHECK(contains(g, "Parlay is an open source bot"),
                      "About in group returns the about card");
        }
        // /keyboard off removes the keyboard.
        {
            Rig rig2;
            rig2.d.handleMessage(makeMsg(999, "private", "/keyboard off"));
            CHECK(!rig2.tg.replies.empty() &&
                      contains(rig2.tg.replies.back(), "Keyboard removed."),
                  "/keyboard off removes the keyboard");
        }
    }

    // --- G. Table coverage for commands, aliases, channel commands,
    // re-derived from dispatcher.cpp source.
    {
        size_t reg = dispatcher_src.find("void Dispatcher::registerCommands()");
        CHECK(reg != std::string::npos, "registerCommands found in source");
        size_t end = dispatcher_src.find("bool Dispatcher::forwardShareMedia",
                                         reg);
        std::string body = dispatcher_src.substr(
            reg, end == std::string::npos ? std::string::npos : end - reg);

        auto has_command = [&](const std::string& name) {
            std::string needle = "/" + name;
            size_t at = 0;
            while ((at = table.find(needle, at)) != std::string::npos) {
                size_t after = at + needle.size();
                char c = after < table.size() ? table[after] : '\0';
                if (c == '\0' || c == ',' || c == ' ' || c == '`' ||
                    c == '>' || c == '|' || c == '<' || c == '[' ||
                    c == '/' || c == '\n')
                    return true;
                ++at;
            }
            return false;
        };

        // add({"a", "b", ...}, ...) groups.
        std::vector<std::string> commands;
        size_t at = 0;
        while ((at = body.find("add({", at)) != std::string::npos) {
            size_t p = at + 5;
            size_t close = body.find('}', p);
            size_t q = p;
            while (q < close) {
                size_t a = body.find('"', q);
                if (a == std::string::npos || a >= close) break;
                size_t b = body.find('"', a + 1);
                if (b == std::string::npos || b > close) break;
                commands.push_back(body.substr(a + 1, b - a - 1));
                q = b + 1;
            }
            at = close + 1;
        }
        CHECK(commands.size() == 113,
              "113 registered command names in source (got " +
                  std::to_string(commands.size()) + ")");
        for (const auto& c : commands)
            CHECK(has_command(c), "table contains command: /" + c);

        // aliases_: {"key", "value"} pairs — keys only.
        std::vector<std::string> aliases;
        size_t al = body.find("aliases_ = {");
        size_t al_end = body.find("};", al);
        {
            size_t p = al;
            while ((p = body.find("{\"", p)) != std::string::npos &&
                   p < al_end) {
                size_t a = p + 2;
                size_t b = body.find('"', a);
                aliases.push_back(body.substr(a, b - a));
                p = b + 1;
            }
        }
        CHECK(aliases.size() == 70,
              "70 aliases in source (got " +
                  std::to_string(aliases.size()) + ")");
        for (const auto& c : aliases)
            CHECK(has_command(c), "table contains alias: /" + c);

        // channel_cmds_: {"name", {"base", bool}} — names only (the
        // inner {"base", ...} is skipped: it follows ", {" not "}").
        std::vector<std::string> channel;
        size_t ch = body.find("channel_cmds_ = {");
        size_t ch_end = body.find("};", ch);
        {
            size_t p = ch;
            while ((p = body.find("{\"", p)) != std::string::npos &&
                   p < ch_end) {
                size_t a = p + 2;
                size_t b = body.find('"', a);
                std::string name = body.substr(a, b - a);
                // An entry name is followed by `", {`; a base name by
                // `", true|false`.
                size_t after = body.find(',', b);
                if (after != std::string::npos &&
                    body.compare(after + 1, 2, " {") == 0)
                    channel.push_back(name);
                p = b + 1;
            }
        }
        CHECK(channel.size() == 40,
              "40 channel commands in source (got " +
                  std::to_string(channel.size()) + ")");
        for (const auto& c : channel)
            CHECK(has_command(c), "table contains channel command: /" + c);
    }

    std::cout << "routing_test: " << templates.size() << " templates, "
              << harvested.size() << " harvested callbacks, " << failures
              << " failures\n";
    return failures == 0 ? 0 : 1;
}
