// telegram_api.cpp
#include "telegram_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "http.hpp"

namespace parlay {

namespace {
// One media attachment (audio/video/voice/video_note/document) on a
// message object -> (file_id, kind, title). A message carries at most
// one; the first kind found wins.
bool scanMedia(const Json& j, std::string& file_id, std::string& kind,
               std::string& title) {
    for (const char* k : {"audio", "video", "voice", "video_note",
                          "document"}) {
        const Json* media = j.find(k);
        if (!media) continue;
        const Json* fid = media->find("file_id");
        if (!fid || fid->asString().empty()) continue;
        file_id = fid->asString();
        kind = k;
        const Json* fn = media->find("file_name");
        if (std::string(k) == "audio") {
            const Json* t = media->find("title");
            const Json* p = media->find("performer");
            if (t && !t->asString().empty())
                title = (p && !p->asString().empty()
                             ? p->asString() + " - "
                             : "") +
                        t->asString();
            else if (fn)
                title = fn->asString();
            else
                title = "Audio";
        } else if (fn) {
            title = fn->asString();
        } else if (std::string(k) == "voice") {
            title = "Voice message";
        } else if (std::string(k) == "video_note") {
            title = "Video message";
        } else {
            title = "Video";
        }
        return true;
    }
    return false;
}

// Keep only filename-safe characters (file_id / extension sanitizing).
std::string safeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            out += c;
    }
    return out;
}
}  // namespace

TelegramApi::TelegramApi(const Config& cfg)
    : base_(cfg.telegram_api_base + "/bot" + cfg.bot_token),
      file_base_(cfg.telegram_api_base + "/file/bot" + cfg.bot_token) {}

Json TelegramApi::call(const std::string& method, const Json& params,
                       int timeout_sec, std::string& error_out) {
    HttpResult r = HttpClient::postJson(base_ + "/" + method, params.dump(), {},
                                        timeout_sec);
    if (!r.ok()) {
        error_out = "HTTP " + std::to_string(r.status) +
                    (r.error.empty() ? "" : ": " + r.error);
        if (!r.body.empty()) error_out += " body=" + r.body;
        return Json(nullptr);
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        error_out = std::string("bad JSON from Telegram: ") + e.what();
        return Json(nullptr);
    }
    if (!parsed.find("ok") || !parsed.find("ok")->asBool()) {
        const Json* desc = parsed.find("description");
        error_out = desc ? desc->asString() : "Telegram API returned ok=false";
        return Json(nullptr);
    }
    const Json* result = parsed.find("result");
    return result ? *result : Json(nullptr);
}

std::string TelegramApi::getFilePath(const std::string& file_id,
                                     std::string& error_out) {
    Json params = Json::objectOf({{"file_id", Json(file_id)}});
    Json result = call("getFile", params, 30, error_out);
    if (result.isNull()) return "";
    const Json* fp = result.find("file_path");
    if (!fp || fp->asString().empty()) {
        error_out = "getFile returned no file_path for this file_id";
        return "";
    }
    return fp->asString();
}

bool TelegramApi::downloadFile(const std::string& file_id,
                               const std::string& dest_dir,
                               std::string& saved_path_out,
                               std::string& error_out) {
    std::string err;
    std::string file_path = getFilePath(file_id, err);
    if (file_path.empty()) {
        error_out = "getFile failed: " + err;
        return false;
    }
    // file_path comes from Telegram, but never let it escape the file
    // endpoint: no absolute paths, no parent traversal.
    if (file_path.front() == '/' ||
        file_path.find("..") != std::string::npos) {
        error_out = "getFile returned an unsafe file_path";
        return false;
    }
    // Extension from the Telegram file_path, sanitized; "bin" fallback.
    std::string ext = "bin";
    if (auto dot = file_path.rfind('.'); dot != std::string::npos) {
        std::string cand = safeName(file_path.substr(dot + 1));
        if (!cand.empty() && cand.size() <= 10) ext = cand;
    }
    std::string safe_id = safeName(file_id);
    if (safe_id.empty()) {
        error_out = "file_id has no filename-safe characters";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        error_out = "cannot create download dir " + dest_dir + ": " +
                    ec.message();
        return false;
    }
    std::string dest = dest_dir + "/" + safe_id + "." + ext;
    HttpResult r = HttpClient::get(file_base_ + "/" + file_path, {}, 300);
    if (!r.ok()) {
        error_out = "file download HTTP " + std::to_string(r.status) +
                    (r.error.empty() ? "" : ": " + r.error);
        return false;
    }
    if (r.body.empty()) {
        error_out = "file download returned no bytes";
        return false;
    }
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error_out = "cannot write " + dest;
        return false;
    }
    out.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
    out.close();
    if (!out) {
        error_out = "failed writing " + dest;
        return false;
    }
    saved_path_out = dest;
    return true;
}

TgMessage TelegramApi::parseMessage(const Json& j) {
    TgMessage m;
    if (const Json* id = j.find("message_id")) m.message_id = id->asInt();
    if (const Json* from = j.find("from")) {
        if (const Json* id = from->find("id")) m.from.id = id->asInt();
        if (const Json* fn = from->find("first_name")) m.from.first_name = fn->asString();
        if (const Json* un = from->find("username")) m.from.username = un->asString();
        if (const Json* b = from->find("is_bot")) m.from.is_bot = b->asBool();
    }
    if (const Json* chat = j.find("chat")) {
        if (const Json* id = chat->find("id")) m.chat.id = id->asInt();
        if (const Json* t = chat->find("type")) m.chat.type = t->asString();
        if (const Json* ti = chat->find("title")) m.chat.title = ti->asString();
    }
    if (const Json* text = j.find("text")) {
        m.text = text->asString();
        m.has_text = true;
    }
    if (const Json* rt = j.find("reply_to_message")) {
        if (const Json* id = rt->find("message_id")) {
            m.reply_to_message_id = id->asInt();
            m.has_reply_to = true;
        }
        if (const Json* from = rt->find("from")) {
            if (const Json* id = from->find("id"))
                m.reply_from_id = id->asInt();
        }
        // Media on the replied-to message (for /play from a reply):
        // audio / video / voice / video_note / document.
        std::string fid, kind, title;
        if (scanMedia(*rt, fid, kind, title)) {
            m.has_reply_media = true;
            m.reply_media_file_id = fid;
            m.reply_media_kind = kind;
            m.reply_media_title = title;
        }
    }
    if (const Json* nm = j.find("new_chat_members")) {
        for (const auto& u : nm->asArray()) {
            TgUser user;
            if (const Json* id = u.find("id")) user.id = id->asInt();
            if (const Json* fn = u.find("first_name"))
                user.first_name = fn->asString();
            if (const Json* un = u.find("username"))
                user.username = un->asString();
            if (const Json* b = u.find("is_bot")) user.is_bot = b->asBool();
            m.new_chat_members.push_back(user);
        }
    }
    if (j.find("video_chat_started")) m.video_chat_started = true;
    if (j.find("video_chat_ended")) m.video_chat_ended = true;
    // Any media attachment on the message itself (audio sent with a
    // /play caption): audio/video/voice/video_note/document.
    std::string fid, kind, title;
    if (scanMedia(j, fid, kind, title)) {
        m.has_media = true;
        m.media_file_id = fid;
        m.media_kind = kind;
        m.media_title = title;
    }
    return m;
}

std::vector<TgUpdate> TelegramApi::getUpdates(int64_t offset, int timeout_sec,
                                              std::string& error_out) {
    Json params = Json::objectOf({
        {"offset", Json(offset)},
        {"timeout", Json(static_cast<int64_t>(timeout_sec))},
        {"allowed_updates", Json(Json::Array{Json("message")})},
    });
    // Long poll: give curl a little more time than Telegram's own timeout.
    Json result = call("getUpdates", params, timeout_sec + 10, error_out);
    std::vector<TgUpdate> out;
    if (result.isNull()) return out;
    for (const auto& u : result.asArray()) {
        TgUpdate upd;
        if (const Json* id = u.find("update_id")) upd.update_id = id->asInt();
        if (const Json* msg = u.find("message")) {
            upd.message = parseMessage(*msg);
            upd.has_message = true;
        }
        out.push_back(std::move(upd));
    }
    return out;
}

bool TelegramApi::sendMessage(int64_t chat_id, const std::string& text,
                              std::string& error_out) {
    return sendMessageId(chat_id, text, 0, &error_out) != -1;
}

int64_t TelegramApi::sendMessageId(int64_t chat_id, const std::string& text,
                                   int64_t reply_to) {
    return sendMessageId(chat_id, text, reply_to, nullptr);
}

int64_t TelegramApi::sendMessageId(int64_t chat_id, const std::string& text,
                                   int64_t reply_to,
                                   std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"text", Json(text)},
        // Plain text on purpose: command replies must never break on a stray
        // '*' or '_' in a track title.
        {"disable_web_page_preview", Json(true)},
    });
    if (reply_to > 0) params.set("reply_to_message_id", Json(reply_to));
    std::string err;
    Json result = call("sendMessage", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendMessage failed: " << err << "\n";
        return -1;
    }
    const Json* id = result.find("message_id");
    return id ? id->asInt() : -1;
}

std::string TelegramApi::getChatMemberStatus(int64_t chat_id, int64_t user_id) {
    std::string err;
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"user_id", Json(user_id)},
    });
    Json result = call("getChatMember", params, 15, err);
    if (result.isNull()) {
        std::cerr << "[tg] getChatMember failed: " << err << "\n";
        return "";
    }
    const Json* st = result.find("status");
    return st ? st->asString() : "";
}

std::string TelegramApi::getMeUsername() {
    std::string err;
    Json result = call("getMe", Json::objectOf({}), 15, err);
    if (result.isNull()) {
        std::cerr << "[tg] getMe failed: " << err << "\n";
        return "";
    }
    const Json* un = result.find("username");
    return un ? un->asString() : "";
}

int64_t TelegramApi::getMeId() {
    std::string err;
    Json result = call("getMe", Json::objectOf({}), 15, err);
    if (result.isNull()) return 0;
    const Json* id = result.find("id");
    return id ? id->asInt() : 0;
}

std::vector<int64_t> TelegramApi::getChatAdministrators(int64_t chat_id) {
    std::string err;
    Json params = Json::objectOf({{"chat_id", Json(chat_id)}});
    Json result = call("getChatAdministrators", params, 15, err);
    std::vector<int64_t> out;
    if (result.isNull()) {
        std::cerr << "[tg] getChatAdministrators failed: " << err << "\n";
        return out;
    }
    for (const auto& member : result.asArray()) {
        const Json* user = member.find("user");
        if (!user) continue;
        const Json* id = user->find("id");
        if (id) out.push_back(id->asInt());
    }
    return out;
}

int64_t TelegramApi::getChatId(const std::string& username_or_id) {
    std::string err;
    Json params = Json::objectOf({{"chat_id", Json(username_or_id)}});
    Json result = call("getChat", params, 15, err);
    if (result.isNull()) return 0;
    const Json* id = result.find("id");
    return id ? id->asInt() : 0;
}

bool TelegramApi::deleteMessage(int64_t chat_id, int64_t message_id) {
    std::string err;
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
    });
    return !call("deleteMessage", params, 15, err).isNull();
}

bool TelegramApi::leaveChat(int64_t chat_id) {
    std::string err;
    Json params = Json::objectOf({{"chat_id", Json(chat_id)}});
    return !call("leaveChat", params, 15, err).isNull();
}

bool TelegramApi::forwardMessage(int64_t to_chat, int64_t from_chat,
                                 int64_t message_id) {
    std::string err;
    Json params = Json::objectOf({
        {"chat_id", Json(to_chat)},
        {"from_chat_id", Json(from_chat)},
        {"message_id", Json(message_id)},
    });
    return !call("forwardMessage", params, 30, err).isNull();
}

bool TelegramApi::copyMessage(int64_t to_chat, int64_t from_chat,
                              int64_t message_id) {
    std::string err;
    Json params = Json::objectOf({
        {"chat_id", Json(to_chat)},
        {"from_chat_id", Json(from_chat)},
        {"message_id", Json(message_id)},
    });
    return !call("copyMessage", params, 30, err).isNull();
}

std::string TelegramApi::exportChatInviteLink(int64_t chat_id) {
    std::string err;
    Json params = Json::objectOf({{"chat_id", Json(chat_id)}});
    Json result = call("exportChatInviteLink", params, 15, err);
    if (result.isNull()) return "";
    const Json* link = result.find("invite_link");
    return link ? link->asString() : "";
}

std::string TelegramApi::getUserMention(int64_t user_id) {
    std::string err;
    Json params = Json::objectOf({{"user_id", Json(user_id)}});
    Json result = call("getChat", params, 15, err);
    if (result.isNull()) return std::to_string(user_id);
    const Json* fn = result.find("first_name");
    std::string name = fn ? fn->asString() : std::to_string(user_id);
    const Json* un = result.find("username");
    if (un && !un->asString().empty()) name += " (@" + un->asString() + ")";
    return name;
}

}  // namespace parlay
