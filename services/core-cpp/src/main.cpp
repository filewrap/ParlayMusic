// main.cpp — parlay-core entry point.
//
// Dual-session TDLib loops (Phase 2): the BOT session (BOT_TOKEN via
// checkAuthenticationBotToken) receives Telegram updates over MTProto
// and the ASSISTANT session (phone/code/2FA first-auth) runs alongside
// it in the same process for voice-chat signaling. Updates from the bot
// session are translated from TDLib JSON into the dispatcher's existing
// entry points (handleMessage / handleCallback / handleMyChatMember /
// handleInlineQuery); assistant-session updates are pumped and dropped
// (the assistant is not a command surface). There is no Bot API polling
// loop in this file and no Bot API traffic on the update path.
//
// Config comes from parlay.toml (file) + environment (see config.hpp);
// no secrets are read from files and none are logged. State lives in
// Store is one JSON file (data/state.json). Playback and
// voice go through the single unified backend over the persistent UDS
// socket (IpcClient); the core never touches ntgcalls or audio, and
// voice joins arrive as duplex JoinRequests the core answers with
// TDLib joinVideoChat on the shared assistant session.
//
// The HTTP API (GET /healthz, GET /api/now-playing, GET /api/queue —
// see core_api.hpp) starts FIRST and unconditionally (Phase 6, A2):
// the GUI surface must not depend on Telegram updates. When BOT_TOKEN
// is unset the core runs HTTP-only (no TDLib sessions); when the store
// is unusable the API still answers /healthz and honestly reports no
// playback state. A missing PARLAY_TOKEN only warns: engine calls go
// out unauthenticated and engines that require a token refuse honestly.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <thread>
#include <utility>

#include "config.hpp"
#include "core_api.hpp"
#include "dispatcher.hpp"
#include "http_server.hpp"
#include "ipc.hpp"
#include "rate_limiter.hpp"
#include "store.hpp"
#include "tdlib_client.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

// Adapts the Dispatcher to the HTTP API's provider seam. The pointer is
// null until the dispatcher is built, and stays null if the store is
// unusable (HTTP-only mode); the API then honestly reports no playback
// state. In HTTP-only mode (no BOT_TOKEN) the dispatcher is still built,
// so the API can proxy real engine state.
struct DispatcherProvider : parlay::VcStateProvider {
    parlay::Dispatcher* dispatcher = nullptr;
    bool nowPlaying(int64_t chat_id, parlay::Json& out,
                    std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->apiNowPlaying(chat_id, out, err);
    }
    bool queueList(int64_t chat_id, parlay::Json& out,
                   std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->apiQueueList(chat_id, out, err);
    }
    bool joinVoice(int64_t chat_id, std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->joinVoice(chat_id, err);
    }
    bool muteVoice(int64_t chat_id, bool muted, std::string& err) override {
        if (!dispatcher) {
            err = "telegram stack not started";
            return false;
        }
        return dispatcher->muteVoice(chat_id, muted, err);
    }
};

void waitForSignal() {
    while (!g_stop) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// --- TDLib update -> dispatcher translation -------------------------------
// The dispatcher speaks the plain Telegram shapes (TgMessage etc.,
// tg_types.hpp); the sends behind them run over the TDLib bot session
// (TdLibTelegramSender). This layer translates TDLib 1.8.69 update JSON
// into those shapes and nothing else: no command logic lives here, and
// anything TDLib reports that has no dispatcher entry (message edits,
// chosen inline results, ...) is ignored, never invented.

// Caches for the per-chat / per-user lookups the translation needs.
// getChat / getUser are called at most once per id for the life of the
// process; a failed lookup is cached as an id-only object so one bad id
// does not cost a round-trip per message.
struct LookupCache {
    std::map<int64_t, parlay::TgChat> chats;
    std::map<int64_t, parlay::TgUser> users;
};

std::string tdChatType(const parlay::Json* type_obj) {
    if (!type_obj) return "";
    const parlay::Json* t = type_obj->find("@type");
    std::string ts = t ? t->asString() : "";
    if (ts == "chatTypePrivate" || ts == "chatTypeSecret") return "private";
    if (ts == "chatTypeBasicGroup") return "group";
    if (ts == "chatTypeSupergroup") {
        const parlay::Json* ch = type_obj->find("is_channel");
        return (ch && ch->asBool()) ? "channel" : "supergroup";
    }
    return "";
}

parlay::TgChat tdChat(parlay::TdLibClient& bot, int64_t chat_id,
                      LookupCache& cache) {
    auto it = cache.chats.find(chat_id);
    if (it != cache.chats.end()) return it->second;
    parlay::TgChat chat;
    chat.id = chat_id;
    parlay::Json resp = bot.call(
        parlay::Json::objectOf({
            {"@type", parlay::Json("getChat")},
            {"chat_id", parlay::Json(chat_id)},
        }),
        10.0);
    if (!resp.isNull() && !parlay::TdLibClient::isError(resp)) {
        chat.type = tdChatType(resp.find("type"));
        const parlay::Json* title = resp.find("title");
        if (title) chat.title = title->asString();
    }
    cache.chats[chat_id] = chat;
    return chat;
}

parlay::TgUser tdUser(parlay::TdLibClient& bot, int64_t user_id,
                      LookupCache& cache) {
    auto it = cache.users.find(user_id);
    if (it != cache.users.end()) return it->second;
    parlay::TgUser user;
    user.id = user_id;
    parlay::Json resp = bot.call(
        parlay::Json::objectOf({
            {"@type", parlay::Json("getUser")},
            {"user_id", parlay::Json(user_id)},
        }),
        5.0);
    if (!resp.isNull() && !parlay::TdLibClient::isError(resp)) {
        const parlay::Json* fn = resp.find("first_name");
        if (fn) user.first_name = fn->asString();
        const parlay::Json* un = resp.find("username");
        if (un) user.username = un->asString();
        const parlay::Json* ty = resp.find("type");
        const parlay::Json* tt = ty ? ty->find("@type") : nullptr;
        user.is_bot = tt && tt->asString() == "userTypeBot";
    }
    cache.users[user_id] = user;
    return user;
}

// (file_id, unique_id) of a TDLib file object ("", "" when absent).
std::pair<std::string, std::string> tdFileIds(const parlay::Json* file) {
    if (!file) return {"", ""};
    const parlay::Json* remote = file->find("remote");
    if (!remote) return {"", ""};
    const parlay::Json* id = remote->find("id");
    const parlay::Json* uid = remote->find("unique_id");
    return {id ? id->asString() : "", uid ? uid->asString() : ""};
}

// Caption text of a media content object ("" when there is none).
std::string tdCaption(const parlay::Json& content) {
    const parlay::Json* cap = content.find("caption");
    if (!cap) return "";
    const parlay::Json* txt = cap->find("text");
    return txt ? txt->asString() : "";
}

// Media on a TDLib message content object -> the TgMessage media fields.
// Returns the caption text ("" when there is none). The file_id recorded
// is the TDLib remote file id (Wave 2 downloads it via TDLib, not the
// 20 MB Bot API getFile path).
std::string tdMedia(const parlay::Json& content, parlay::TgMessage& msg) {
    const parlay::Json* type = content.find("@type");
    std::string t = type ? type->asString() : "";
    if (t == "messagePhoto") {
        const parlay::Json* photo = content.find("photo");
        const parlay::Json* sizes = photo ? photo->find("sizes") : nullptr;
        // Sizes are ordered smallest first: the last is the largest.
        if (sizes && sizes->size() > 0) {
            const parlay::Json* biggest = sizes->at(sizes->size() - 1);
            auto ids = tdFileIds(biggest ? biggest->find("photo") : nullptr);
            if (!ids.first.empty()) {
                msg.has_media = true;
                msg.media_file_id = ids.first;
                msg.media_file_unique_id = ids.second;
                msg.media_kind = "photo";
                msg.media_title = "Photo";
            }
        }
        return tdCaption(content);
    }
    if (t == "messageAudio") {
        const parlay::Json* audio = content.find("audio");
        auto ids = tdFileIds(audio ? audio->find("audio") : nullptr);
        if (!ids.first.empty()) {
            msg.has_media = true;
            msg.media_file_id = ids.first;
            msg.media_file_unique_id = ids.second;
            msg.media_kind = "audio";
            std::string title;
            if (audio) {
                const parlay::Json* perf = audio->find("performer");
                const parlay::Json* ti = audio->find("title");
                if (ti && !ti->asString().empty())
                    title = (perf && !perf->asString().empty()
                                 ? perf->asString() + " - "
                                 : "") +
                            ti->asString();
                else {
                    const parlay::Json* fn = audio->find("file_name");
                    title = fn ? fn->asString() : "Audio";
                }
            }
            msg.media_title = title.empty() ? "Audio" : title;
        }
        return tdCaption(content);
    }
    if (t == "messageVideo") {
        const parlay::Json* video = content.find("video");
        auto ids = tdFileIds(video ? video->find("video") : nullptr);
        if (!ids.first.empty()) {
            msg.has_media = true;
            msg.media_file_id = ids.first;
            msg.media_file_unique_id = ids.second;
            msg.media_kind = "video";
            const parlay::Json* fn = video ? video->find("file_name") : nullptr;
            msg.media_title = fn ? fn->asString() : "Video";
        }
        return tdCaption(content);
    }
    if (t == "messageVoiceNote") {
        const parlay::Json* note = content.find("voice_note");
        auto ids = tdFileIds(note ? note->find("voice") : nullptr);
        if (!ids.first.empty()) {
            msg.has_media = true;
            msg.media_file_id = ids.first;
            msg.media_file_unique_id = ids.second;
            msg.media_kind = "voice";
            msg.media_title = "Voice message";
        }
        return tdCaption(content);
    }
    if (t == "messageVideoNote") {
        const parlay::Json* note = content.find("video_note");
        auto ids = tdFileIds(note ? note->find("video") : nullptr);
        if (!ids.first.empty()) {
            msg.has_media = true;
            msg.media_file_id = ids.first;
            msg.media_file_unique_id = ids.second;
            msg.media_kind = "video_note";
            msg.media_title = "Video message";
        }
        return tdCaption(content);
    }
    if (t == "messageDocument") {
        const parlay::Json* doc = content.find("document");
        auto ids = tdFileIds(doc ? doc->find("document") : nullptr);
        if (!ids.first.empty()) {
            msg.has_media = true;
            msg.media_file_id = ids.first;
            msg.media_file_unique_id = ids.second;
            msg.media_kind = "document";
            const parlay::Json* fn = doc ? doc->find("file_name") : nullptr;
            msg.media_title = fn ? fn->asString() : "Document";
        }
        return tdCaption(content);
    }
    return "";
}

// One TDLib message object -> TgMessage. The replied-to message (when
// any) is fetched with getMessage so reply media resolves exactly like
// the Bot API parser's reply_to_message did; a failed fetch leaves
// has_reply_media false instead of inventing media.
parlay::TgMessage tdMessage(parlay::TdLibClient& bot,
                            const parlay::Json& m, LookupCache& cache) {
    parlay::TgMessage msg;
    const parlay::Json* id = m.find("id");
    msg.message_id = id ? id->asInt() : 0;
    const parlay::Json* chat_id = m.find("chat_id");
    int64_t cid = chat_id ? chat_id->asInt() : 0;
    msg.chat = tdChat(bot, cid, cache);
    const parlay::Json* sender = m.find("sender_id");
    const parlay::Json* st = sender ? sender->find("@type") : nullptr;
    std::string sender_type = st ? st->asString() : "";
    if (sender_type == "messageSenderUser") {
        const parlay::Json* uid = sender->find("user_id");
        msg.from = tdUser(bot, uid ? uid->asInt() : 0, cache);
    } else if (sender_type == "messageSenderChat") {
        // Channel post: the sender is a chat, not a user.
        const parlay::Json* scid = sender->find("chat_id");
        msg.from.id = scid ? scid->asInt() : 0;
        parlay::TgChat sender_chat = tdChat(bot, msg.from.id, cache);
        msg.from.first_name = sender_chat.title;
    }
    const parlay::Json* reply = m.find("reply_to");
    const parlay::Json* rt = reply ? reply->find("@type") : nullptr;
    if (rt && rt->asString() == "messageReplyToMessage") {
        const parlay::Json* mid = reply->find("message_id");
        if (mid && mid->asInt() != 0) {
            msg.reply_to_message_id = mid->asInt();
            msg.has_reply_to = true;
            parlay::Json replied = bot.call(
                parlay::Json::objectOf({
                    {"@type", parlay::Json("getMessage")},
                    {"chat_id", parlay::Json(cid)},
                    {"message_id", parlay::Json(msg.reply_to_message_id)},
                }),
                10.0);
            if (!replied.isNull() &&
                !parlay::TdLibClient::isError(replied)) {
                const parlay::Json* rs = replied.find("sender_id");
                const parlay::Json* rst = rs ? rs->find("@type") : nullptr;
                if (rst && rst->asString() == "messageSenderUser") {
                    const parlay::Json* ruid = rs->find("user_id");
                    msg.reply_from_id = ruid ? ruid->asInt() : 0;
                }
                const parlay::Json* rcontent = replied.find("content");
                if (rcontent) {
                    parlay::TgMessage rmsg;
                    tdMedia(*rcontent, rmsg);
                    if (rmsg.has_media) {
                        msg.has_reply_media = true;
                        msg.reply_media_file_id = rmsg.media_file_id;
                        msg.reply_media_file_unique_id =
                            rmsg.media_file_unique_id;
                        msg.reply_media_kind = rmsg.media_kind;
                        msg.reply_media_title = rmsg.media_title;
                    }
                }
            }
        }
    }
    const parlay::Json* content = m.find("content");
    if (content) {
        const parlay::Json* ct = content->find("@type");
        std::string ct_str = ct ? ct->asString() : "";
        if (ct_str == "messageText") {
            const parlay::Json* text = content->find("text");
            const parlay::Json* txt = text ? text->find("text") : nullptr;
            msg.text = txt ? txt->asString() : "";
            msg.has_text = !msg.text.empty();
        } else if (ct_str == "messageChatAddMembers") {
            const parlay::Json* ids = content->find("member_user_ids");
            if (ids) {
                for (size_t i = 0; i < ids->size(); ++i) {
                    const parlay::Json* uid = ids->at(i);
                    parlay::TgUser u;
                    u.id = uid ? uid->asInt() : 0;
                    msg.new_chat_members.push_back(u);
                }
            }
        } else if (ct_str == "messageVideoChatStarted") {
            msg.video_chat_started = true;
        } else if (ct_str == "messageVideoChatEnded") {
            msg.video_chat_ended = true;
        } else {
            // Media message: the caption is the text (a /play caption
            // on an audio file is the command).
            std::string caption = tdMedia(*content, msg);
            msg.text = caption;
            msg.has_text = !caption.empty();
        }
    }
    return msg;
}

// Standard base64 (TDLib encodes `bytes` fields, e.g. callback payload
// data, as base64). False when the input is not valid base64.
bool base64Decode(const std::string& in, std::string& out) {
    static const std::string kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        size_t idx = kAlphabet.find(static_cast<char>(c));
        if (idx == std::string::npos) return false;
        val = (val << 6) + static_cast<int>(idx);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

std::string tdMemberStatus(const parlay::Json* status_obj) {
    if (!status_obj) return "";
    const parlay::Json* t = status_obj->find("@type");
    std::string ts = t ? t->asString() : "";
    if (ts == "chatMemberStatusCreator") return "creator";
    if (ts == "chatMemberStatusAdministrator") return "administrator";
    if (ts == "chatMemberStatusMember") return "member";
    if (ts == "chatMemberStatusRestricted") return "restricted";
    if (ts == "chatMemberStatusLeft") return "left";
    if (ts == "chatMemberStatusBanned") return "kicked";
    return ts;
}

// Route one bot-session update into the dispatcher. Assistant-session
// updates never reach here (the caller filters by session). Returns
// false only when the update was a callback with an unusable payload
// that was already answered here (nothing left for the dispatcher).
bool dispatchTdUpdate(const parlay::Json& upd, parlay::TdLibClient& bot,
                      int64_t bot_id, parlay::Dispatcher& dispatcher,
                      LookupCache& cache) {
    const parlay::Json* type = upd.find("@type");
    std::string t = type ? type->asString() : "";
    if (t == "updateNewMessage") {
        const parlay::Json* m = upd.find("message");
        if (!m) return true;
        // Own outgoing messages arrive here too: never feed them back
        // into the dispatcher (command loops).
        const parlay::Json* outgoing = m->find("is_outgoing");
        if (outgoing && outgoing->asBool()) return true;
        parlay::TgMessage msg = tdMessage(bot, *m, cache);
        if (msg.from.id == bot_id && bot_id != 0) return true;
        dispatcher.handleMessage(msg);
        return true;
    }
    if (t == "updateCallbackQuery") {
        parlay::TgCallbackQuery cq;
        const parlay::Json* id = upd.find("id");
        cq.id = id ? id->asString() : "";
        const parlay::Json* uid = upd.find("sender_user_id");
        cq.from.id = uid ? uid->asInt() : 0;
        const parlay::Json* chat_id = upd.find("chat_id");
        cq.chat_id = chat_id ? chat_id->asInt() : 0;
        const parlay::Json* mid = upd.find("message_id");
        cq.message_id = mid ? mid->asInt() : 0;
        cq.has_message = cq.message_id != 0;
        const parlay::Json* payload = upd.find("payload");
        const parlay::Json* pt = payload ? payload->find("@type") : nullptr;
        if (!pt || pt->asString() != "callbackQueryPayloadData") {
            // Password-protected (or future) payloads have no plain
            // data to route: answer honestly here so the spinner ends.
            bot.answerCallbackQuery(cq.id, "Unsupported button payload",
                                    false, 0);
            return false;
        }
        const parlay::Json* data = payload->find("data");
        std::string raw = data ? data->asString() : "";
        if (!base64Decode(raw, cq.data)) {
            bot.answerCallbackQuery(cq.id, "Unsupported button payload",
                                    false, 0);
            return false;
        }
        dispatcher.handleCallback(cq);
        return true;
    }
    if (t == "updateChatMember") {
        // The bot's own status change goes to the demotion goodbye;
        // a non-bot member gaining or losing administrator/creator
        // status refreshes the dispatcher's admin cache (remix
        // chat_member_updated, via handleAdminTransition); ordinary
        // member joins/leaves are not forwarded — the admin list
        // cannot have changed.
        const parlay::Json* nm = upd.find("new_chat_member");
        const parlay::Json* member_id =
            nm ? nm->find("member_id") : nullptr;
        const parlay::Json* uid =
            member_id ? member_id->find("user_id") : nullptr;
        int64_t user_id = uid ? uid->asInt() : 0;
        parlay::TgChatMemberUpdate mu;
        const parlay::Json* chat_id = upd.find("chat_id");
        mu.chat_id = chat_id ? chat_id->asInt() : 0;
        mu.user_id = user_id;
        const parlay::Json* om = upd.find("old_chat_member");
        mu.old_status = tdMemberStatus(om ? om->find("status") : nullptr);
        mu.new_status = tdMemberStatus(nm ? nm->find("status") : nullptr);
        if (user_id == bot_id) {
            dispatcher.handleMyChatMember(mu);
        } else if (mu.old_status == "administrator" ||
                   mu.old_status == "creator" ||
                   mu.new_status == "administrator" ||
                   mu.new_status == "creator") {
            dispatcher.handleAdminTransition(mu);
        }
        return true;
    }
    if (t == "updateNewInlineQuery") {
        parlay::TgInlineQuery iq;
        const parlay::Json* id = upd.find("id");
        iq.id = id ? id->asString() : "";
        const parlay::Json* uid = upd.find("sender_user_id");
        iq.from.id = uid ? uid->asInt() : 0;
        const parlay::Json* query = upd.find("query");
        iq.query = query ? query->asString() : "";
        const parlay::Json* offset = upd.find("offset");
        iq.offset = offset ? offset->asString() : "";
        iq.chat_type = tdChatType(upd.find("chat_type"));
        if (iq.chat_type.empty()) iq.chat_type = "sender";
        dispatcher.handleInlineQuery(iq);
        return true;
    }
    // Everything else (message edits, chosen inline results, chat
    // updates, ...) has no dispatcher entry and is ignored.
    return true;
}
}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    parlay::Config cfg = parlay::Config::load();

    // HTTP API first: it serves even with no BOT_TOKEN (A2). If the
    // port cannot be bound the Telegram side still runs, but the
    // failure is loud — /healthz and /api/* are the GUI's only window
    // into the core.
    DispatcherProvider provider;
    parlay::CoreApi api(provider, cfg.parlay_token);
    parlay::HttpServer http(cfg.port, [&](const parlay::HttpRequest& req) {
        return api.handle(req);
    });
    if (http.start()) {
        std::cout << "parlay-core: http api on :" << http.port() << "\n";
    } else {
        std::cerr << "parlay-core: WARNING: could not bind HTTP port "
                  << cfg.port << " — /healthz and /api/* unavailable\n";
    }

    // Telegram needs BOT_TOKEN; the HTTP API + dispatcher do not. A
    // missing PARLAY_TOKEN only warns: engine calls go out
    // unauthenticated and engines that require a token refuse honestly.
    bool telegram_up = !cfg.bot_token.empty();
    if (!telegram_up || cfg.parlay_token.empty()) {
        std::cerr << "parlay-core: WARNING: missing env vars:";
        if (cfg.bot_token.empty()) std::cerr << " BOT_TOKEN";
        if (cfg.parlay_token.empty()) std::cerr << " PARLAY_TOKEN";
        std::cerr << "\n";
        if (!telegram_up)
            std::cerr << "parlay-core: Telegram disabled — HTTP API "
                         "only. Set BOT_TOKEN (see .env.example at the "
                         "Parlay root) and restart for full operation.\n";
        if (cfg.parlay_token.empty())
            std::cerr << "parlay-core: WARNING: engine calls go out "
                         "unauthenticated; engines with a token set will "
                         "refuse them.\n";
    }
    std::cout << "parlay-core starting: " << cfg.summary() << "\n";

    // Optional log file (for /logs). stdout/stderr are appended there too.
    if (!cfg.log_file.empty()) {
        FILE* lf = std::freopen(cfg.log_file.c_str(), "a", stdout);
        if (lf) {
            std::cerr << "parlay-core: logging to " << cfg.log_file << "\n";
        } else {
            std::cerr << "parlay-core: cannot open log file "
                      << cfg.log_file << "\n";
        }
    }

    parlay::Store store(cfg);
    if (!store.ok()) {
        // The dispatcher needs the store; without it the core is
        // HTTP-only and /api/* honestly reports no playback state.
        // /healthz still answers.
        std::cerr << "parlay-core: no usable state backend — HTTP API "
                     "only\n";
        waitForSignal();
        http.stop();
        std::cout << "parlay-core: shutting down.\n";
        return 0;
    }

    // Single unified backend over the persistent UDS socket
    // (contracts/uds-protocol.md). Per-action deadlines from
    // parlay.toml [timeouts]: the IpcClient ctor already seeds the
    // GET-style table (10 s) + ping (5 s) at the same defaults, so
    // these assignments make the file's values authoritative.
    parlay::IpcClient ipc(cfg);
    ipc.setDefaultDeadlineMs(cfg.timeouts.resolve_ms);
    for (const char* a : {"vc.now_playing", "vc.progress", "vc.panel",
                          "vc.lyrics", "radio.list", "radio.status",
                          "download.progress", "share.resolve",
                          "chat.settings", "spotify.details",
                          "spotify.now_playing", "spotify.watches",
                          "spotify.status", "gui.now_playing",
                          "gui.queue"})
        ipc.setDeadline(a, cfg.timeouts.ipc_cmd_ms);
    ipc.setDeadline("ping", cfg.timeouts.healthz_ms);
    for (const char* a : {"download.deezer", "download.navidrome",
                          "download.soulseek", "download.spotify",
                          "download.spotify_playlist", "download.tiktok",
                          "download.youtube"})
        ipc.setDeadline(a, cfg.timeouts.download_ms);
    ipc.setDeadline("vc.join", cfg.timeouts.join_ms);

    // Dual TDLib sessions in one process (Phase 2): the bot session
    // authenticates with BOT_TOKEN + API_ID/API_HASH and delivers
    // updates over MTProto; the assistant session keeps the phone /
    // code / 2FA first-auth path for voice-chat signaling. The
    // sessions are built before the dispatcher because the dispatcher
    // holds references to both (bot for sends, assistant for joins);
    // they authorize below, after the dispatcher exists.
    parlay::TdLibDualSession sessions;
    parlay::TdLibTelegramSender tg(sessions.bot(), cfg.data_dir);
    parlay::RateLimiter limiter(cfg.rate_limit);
    parlay::Dispatcher dispatcher(cfg, tg, ipc, store,
                                  sessions.assistant(), limiter, "");
    provider.dispatcher = &dispatcher;
    // Backend-driven duplex hooks: events enqueue Telegram work for
    // the main thread; join requests block on the main thread's
    // processPendingWork() (see dispatcher.hpp header note).
    ipc.setEventHandler([&dispatcher](const std::string& event,
                                      const parlay::Json& data) {
        dispatcher.onBackendEvent(event, data);
    });
    ipc.setJoinRequestHandler([&dispatcher](const parlay::Json& request) {
        int64_t chat_id = 0;
        if (const auto* c = request.find("chat_id")) chat_id = c->asInt();
        std::cout << "parlay-core: incoming JoinRequest for chat " << chat_id << std::endl;
        parlay::Json res = dispatcher.onJoinRequest(request);
        const parlay::Json* ok = res.find("ok");
        if (ok && ok->asBool()) {
            const parlay::Json* data = res.find("data");
            if (data) {
                size_t sdp_len = 0;
                if (const auto* s = data->find("remote_sdp")) sdp_len = s->asString().size();
                std::cout << "parlay-core: JoinRequest succeeded for chat " << chat_id
                          << " (remote_sdp length: " << sdp_len << ")" << std::endl;
                return *data;
            }
            return parlay::Json::objectOf({});
        }
        const parlay::Json* err = res.find("error");
        std::string msg = "join failed";
        if (err) {
            const parlay::Json* m = err->find("message");
            if (m) msg = m->asString();
        }
        std::cerr << "parlay-core: JoinRequest failed for chat " << chat_id << ": " << msg << std::endl;
        throw std::runtime_error(msg);
    });
    dispatcher.startBackground();

    if (!telegram_up) {
        // No BOT_TOKEN: the HTTP API above is live and can proxy real
        // engine state, but no TDLib session starts.
        waitForSignal();
        dispatcher.stopBackground();
        http.stop();
        std::cout << "parlay-core: shutting down.\n";
        return 0;
    }

    // The bot session must authorize — without it there is no update
    // source and the core exits loudly, exactly like the old getMe
    // failure. The assistant is best-effort: without it the bot still
    // serves commands and only voice joins are unavailable.
    std::cout << "parlay-core: tdlib bot session ("
              << sessions.bot().libPath() << ") + assistant session\n";
    if (!sessions.authorizeBot(60.0)) {
        std::cerr << "parlay-core: could not authorize the bot session: "
                  << sessions.botError() << "\n";
        dispatcher.stopBackground();
        http.stop();
        return 1;
    }
    int64_t bot_id = 0;
    std::string bot_username;
    if (!sessions.bot().getMe(bot_id, bot_username)) {
        std::cerr << "parlay-core: could not reach Telegram (getMe failed: "
                  << sessions.botError() << "). Check BOT_TOKEN and "
                     "network, then restart.\n";
        dispatcher.stopBackground();
        http.stop();
        return 1;
    }
    std::cout << "parlay-core: logged in as @"
              << (bot_username.empty() ? std::string("(no username)")
                                       : bot_username)
              << " (" << bot_id << ")\n";
    dispatcher.setBotId(bot_id);
    dispatcher.setBotUsername(bot_username);

    if (!sessions.authorizeAssistant(60.0)) {
        std::cerr << "parlay-core: WARNING: assistant session not "
                     "authorized: "
                  << sessions.assistantError()
                  << " — voice joins unavailable until the assistant "
                     "logs in (TD_PHONE / TD_CODE / TD_PASSWORD, or a "
                     "pre-authorized session dir).\n";
    } else {
        std::cout << "parlay-core: assistant session authorized\n";
    }

    LookupCache cache;
    while (!g_stop) {
        parlay::Json upd;
        parlay::SessionKind from = parlay::SessionKind::Assistant;
        if (sessions.pollUpdate(upd, from, 1.0)) {
            if (from == parlay::SessionKind::Bot)
                dispatchTdUpdate(upd, sessions.bot(), bot_id, dispatcher,
                                 cache);
        }
        // Queued backend events / join requests / sweeper deletes run
        // here, on the main thread (the only thread that may call
        // TDLib). A pending join blocks its IpcClient worker thread
        // until this drain answers it.
        dispatcher.processPendingWork();
    }
    sessions.close();
    dispatcher.stopBackground();
    http.stop();
    std::cout << "parlay-core: shutting down.\n";
    return 0;
}
