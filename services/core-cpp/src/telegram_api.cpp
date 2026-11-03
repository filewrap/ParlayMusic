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
// one; the first kind found wins. Also returns the file's unique id
// (stable across forwards), used by the §22 share-feedback buttons.
bool scanMedia(const Json& j, std::string& file_id, std::string& kind,
               std::string& title, std::string& file_unique_id) {
    for (const char* k : {"audio", "video", "voice", "video_note",
                          "document"}) {
        const Json* media = j.find(k);
        if (!media) continue;
        const Json* fid = media->find("file_id");
        if (!fid || fid->asString().empty()) continue;
        file_id = fid->asString();
        kind = k;
        const Json* fuid = media->find("file_unique_id");
        file_unique_id = fuid ? fuid->asString() : "";
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

Json inlineKeyboardJson(const InlineKeyboard& kb) {
    Json::Array rows;
    for (const auto& row : kb) {
        Json::Array buttons;
        for (const auto& b : row) {
            Json obj = Json::objectOf({{"text", Json(b.text)}});
            if (!b.callback_data.empty())
                obj.set("callback_data", Json(b.callback_data));
            else if (!b.url.empty())
                obj.set("url", Json(b.url));
            else if (b.has_switch) {
                if (b.switch_current_chat)
                    obj.set("switch_inline_query_current_chat",
                            Json(b.switch_inline_query));
                else
                    obj.set("switch_inline_query",
                            Json(b.switch_inline_query));
            }
            buttons.push_back(std::move(obj));
        }
        rows.push_back(Json(std::move(buttons)));
    }
    return Json::objectOf({{"inline_keyboard", Json(std::move(rows))}});
}

Json replyKeyboardJson(const ReplyKeyboard& kb) {
    Json::Array rows;
    for (const auto& row : kb.rows) {
        Json::Array buttons;
        for (const auto& label : row) {
            buttons.push_back(
                Json::objectOf({{"text", Json(label)}}));
        }
        rows.push_back(Json(std::move(buttons)));
    }
    return Json::objectOf({
        {"keyboard", Json(std::move(rows))},
        {"resize_keyboard", Json(kb.resize)},
        {"one_time_keyboard", Json(kb.one_time)},
    });
}

Json removeKeyboardJson() {
    return Json::objectOf({
        {"remove_keyboard", Json(true)},
        {"selective", Json(false)},
    });
}

TgCallbackQuery TelegramApi::parseCallbackQuery(const Json& j) {
    TgCallbackQuery q;
    if (const Json* id = j.find("id")) q.id = id->asString();
    if (const Json* from = j.find("from")) {
        if (const Json* uid = from->find("id")) q.from.id = uid->asInt();
        if (const Json* fn = from->find("first_name"))
            q.from.first_name = fn->asString();
        if (const Json* un = from->find("username"))
            q.from.username = un->asString();
        if (const Json* b = from->find("is_bot")) q.from.is_bot = b->asBool();
    }
    if (const Json* msg = j.find("message")) {
        q.has_message = true;
        if (const Json* mid = msg->find("message_id"))
            q.message_id = mid->asInt();
        if (const Json* chat = msg->find("chat")) {
            if (const Json* cid = chat->find("id")) q.chat_id = cid->asInt();
        }
    }
    if (const Json* data = j.find("data")) q.data = data->asString();
    return q;
}

TgChatMemberUpdate TelegramApi::parseMyChatMember(const Json& j) {
    TgChatMemberUpdate u;
    if (const Json* chat = j.find("chat")) {
        if (const Json* id = chat->find("id")) u.chat_id = id->asInt();
    }
    if (const Json* from = j.find("from")) {
        if (const Json* id = from->find("id")) u.user_id = id->asInt();
    }
    if (const Json* old_m = j.find("old_chat_member")) {
        if (const Json* s = old_m->find("status"))
            u.old_status = s->asString();
    }
    if (const Json* new_m = j.find("new_chat_member")) {
        if (const Json* s = new_m->find("status"))
            u.new_status = s->asString();
    }
    return u;
}

TgInlineQuery TelegramApi::parseInlineQuery(const Json& j) {
    TgInlineQuery q;
    if (const Json* id = j.find("id")) q.id = id->asString();
    if (const Json* from = j.find("from")) {
        if (const Json* uid = from->find("id")) q.from.id = uid->asInt();
        if (const Json* fn = from->find("first_name"))
            q.from.first_name = fn->asString();
        if (const Json* un = from->find("username"))
            q.from.username = un->asString();
        if (const Json* b = from->find("is_bot")) q.from.is_bot = b->asBool();
    }
    if (const Json* query = j.find("query")) q.query = query->asString();
    if (const Json* off = j.find("offset")) q.offset = off->asString();
    if (const Json* ct = j.find("chat_type"))
        q.chat_type = ct->asString();
    return q;
}

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
        std::string fid, kind, title, fuid;
        if (scanMedia(*rt, fid, kind, title, fuid)) {
            m.has_reply_media = true;
            m.reply_media_file_id = fid;
            m.reply_media_file_unique_id = fuid;
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
    std::string fid, kind, title, fuid;
    if (scanMedia(j, fid, kind, title, fuid)) {
        m.has_media = true;
        m.media_file_id = fid;
        m.media_file_unique_id = fuid;
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
        {"allowed_updates",
         Json(Json::Array{Json("message"), Json("callback_query"),
                          Json("my_chat_member"), Json("inline_query")})},
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
        if (const Json* cb = u.find("callback_query")) {
            upd.callback = parseCallbackQuery(*cb);
            upd.has_callback_query = true;
        }
        if (const Json* cm = u.find("my_chat_member")) {
            upd.my_chat_member = parseMyChatMember(*cm);
            upd.has_my_chat_member = true;
        }
        if (const Json* iq = u.find("inline_query")) {
            upd.inline_query = parseInlineQuery(*iq);
            upd.has_inline_query = true;
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

int64_t TelegramApi::sendMessageHtml(int64_t chat_id,
                                     const std::string& text,
                                     const Json& reply_markup,
                                     int64_t reply_to,
                                     std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"text", Json(text)},
        {"parse_mode", Json("HTML")},
        {"disable_web_page_preview", Json(true)},
    });
    if (!reply_markup.isNull()) params.set("reply_markup", reply_markup);
    if (reply_to > 0) params.set("reply_to_message_id", Json(reply_to));
    std::string err;
    Json result = call("sendMessage", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendMessage (HTML) failed: " << err << "\n";
        return -1;
    }
    const Json* id = result.find("message_id");
    return id ? id->asInt() : -1;
}

int64_t TelegramApi::sendPhoto(int64_t chat_id, const std::string& photo,
                               const std::string& caption,
                               const Json& reply_markup,
                               std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"photo", Json(photo)},
        {"caption", Json(caption)},
        {"parse_mode", Json("HTML")},
    });
    if (!reply_markup.isNull()) params.set("reply_markup", reply_markup);
    std::string err;
    Json result = call("sendPhoto", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendPhoto failed: " << err << "\n";
        return -1;
    }
    const Json* id = result.find("message_id");
    return id ? id->asInt() : -1;
}

bool TelegramApi::editMessageText(int64_t chat_id, int64_t message_id,
                                  const std::string& text,
                                  const Json& reply_markup,
                                  std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"text", Json(text)},
        {"parse_mode", Json("HTML")},
        {"disable_web_page_preview", Json(true)},
    });
    if (!reply_markup.isNull()) params.set("reply_markup", reply_markup);
    std::string err;
    Json result = call("editMessageText", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        return false;
    }
    return true;
}

bool TelegramApi::editMessageCaption(int64_t chat_id, int64_t message_id,
                                     const std::string& caption,
                                     const Json& reply_markup,
                                     std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"caption", Json(caption)},
        {"parse_mode", Json("HTML")},
    });
    if (!reply_markup.isNull()) params.set("reply_markup", reply_markup);
    std::string err;
    Json result = call("editMessageCaption", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        return false;
    }
    return true;
}

bool TelegramApi::editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                         const Json& reply_markup,
                                         std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
    });
    // An explicit null clears the keyboard (Bot API: reply_markup absent
    // leaves it, so "remove" is modelled as an empty inline keyboard).
    params.set("reply_markup",
               reply_markup.isNull()
                   ? Json::objectOf({{"inline_keyboard", Json(Json::Array{})}})
                   : reply_markup);
    std::string err;
    Json result = call("editMessageReplyMarkup", params, 30, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        return false;
    }
    return true;
}

bool TelegramApi::answerCallbackQuery(const std::string& callback_query_id,
                                      const std::string& text,
                                      bool show_alert, int cache_time_sec,
                                      std::string* error_out) {
    Json params = Json::objectOf({
        {"callback_query_id", Json(callback_query_id)},
    });
    if (!text.empty()) params.set("text", Json(text));
    if (show_alert) params.set("show_alert", Json(true));
    if (cache_time_sec > 0)
        params.set("cache_time", Json(static_cast<int64_t>(cache_time_sec)));
    std::string err;
    Json result = call("answerCallbackQuery", params, 15, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] answerCallbackQuery failed: " << err << "\n";
        return false;
    }
    return true;
}

bool TelegramApi::answerInlineQuery(const std::string& inline_query_id,
                                    const Json& results, int cache_time_sec,
                                    const std::string& switch_pm_text,
                                    const std::string& switch_pm_parameter,
                                    std::string* error_out) {
    Json params = Json::objectOf({
        {"inline_query_id", Json(inline_query_id)},
        {"results", results},
        {"cache_time", Json(static_cast<int64_t>(cache_time_sec))},
    });
    if (!switch_pm_text.empty()) {
        params.set("switch_pm_text", Json(switch_pm_text));
        params.set("switch_pm_parameter", Json(switch_pm_parameter));
    }
    std::string err;
    Json result = call("answerInlineQuery", params, 15, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] answerInlineQuery failed: " << err << "\n";
        return false;
    }
    return true;
}

int64_t TelegramApi::sendDocument(int64_t chat_id,
                                  const std::string& filename,
                                  const std::string& content,
                                  const std::string& caption,
                                  std::string* error_out) {
    // Bot API sendDocument is multipart: chat_id + caption are text
    // fields, the file is the "document" part. The filename is
    // sanitized to one inert token (no path separators, no quotes)
    // so a caller-supplied name cannot smuggle in a path or a
    // forged part header; the multipart builder itself also
    // guarantees the boundary never appears in any value.
    std::string safe;
    for (char c : filename) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            safe += c;
    }
    if (safe.empty() || safe == "." || safe == "..") safe = "file.txt";
    HttpResult r = HttpClient::postMultipart(
        base_ + "/sendDocument",
        {{"chat_id", std::to_string(chat_id)}, {"caption", caption}},
        "document", safe, content, "text/plain", {}, 60);
    if (!r.ok()) {
        std::string err = "HTTP " + std::to_string(r.status) +
                          (r.error.empty() ? "" : ": " + r.error);
        if (!r.body.empty()) err += " body=" + r.body;
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendDocument failed: " << err << "\n";
        return -1;
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        if (error_out)
            *error_out = std::string("bad JSON from Telegram: ") + e.what();
        return -1;
    }
    if (!parsed.find("ok") || !parsed.find("ok")->asBool()) {
        const Json* desc = parsed.find("description");
        if (error_out)
            *error_out = desc ? desc->asString()
                              : "Telegram API returned ok=false";
        return -1;
    }
    const Json* result = parsed.find("result");
    if (!result) return -1;
    const Json* id = result->find("message_id");
    return id ? id->asInt() : -1;
}

int64_t TelegramApi::sendAudioFile(int64_t chat_id,
                                   const std::string& file_path,
                                   const std::string& caption,
                                   std::string* error_out) {
    // Read the file off disk, then multipart it as the "audio" part.
    // The 50 MB Bot API cap applies here (downloaders enforce the
    // 200 MB download limit upstream; an FX result over 50 MB fails
    // with Telegram's own error, reported verbatim).
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "cannot open " + file_path;
        std::cerr << "[tg] sendAudioFile cannot open " << file_path << "\n";
        return -1;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::string ext;
    size_t dot = file_path.find_last_of('.');
    if (dot != std::string::npos) ext = file_path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    std::string content_type = "audio/mpeg";
    if (ext == "m4a") content_type = "audio/mp4";
    else if (ext == "flac") content_type = "audio/flac";
    else if (ext == "opus") content_type = "audio/opus";
    else if (ext == "ogg") content_type = "audio/ogg";
    else if (ext == "wav") content_type = "audio/wav";
    std::string filename =
        std::filesystem::path(file_path).filename().string();
    HttpResult r = HttpClient::postMultipart(
        base_ + "/sendAudio",
        {{"chat_id", std::to_string(chat_id)}, {"caption", caption}},
        "audio", filename, bytes, content_type, {}, 120);
    if (!r.ok()) {
        std::string err = "HTTP " + std::to_string(r.status) +
                          (r.error.empty() ? "" : ": " + r.error);
        if (!r.body.empty()) err += " body=" + r.body;
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendAudioFile failed: " << err << "\n";
        return -1;
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        if (error_out)
            *error_out = std::string("bad JSON from Telegram: ") + e.what();
        return -1;
    }
    if (!parsed.find("ok") || !parsed.find("ok")->asBool()) {
        const Json* desc = parsed.find("description");
        if (error_out)
            *error_out = desc ? desc->asString()
                              : "Telegram API returned ok=false";
        return -1;
    }
    const Json* result = parsed.find("result");
    if (!result) return -1;
    const Json* id = result->find("message_id");
    return id ? id->asInt() : -1;
}

int64_t TelegramApi::sendFile(int64_t chat_id,
                              const std::string& file_path,
                              const std::string& caption,
                              std::string* error_out) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "cannot open " + file_path;
        std::cerr << "[tg] sendFile cannot open " << file_path << "\n";
        return -1;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::string ext;
    size_t dot = file_path.find_last_of('.');
    if (dot != std::string::npos) ext = file_path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    std::string content_type = "application/octet-stream";
    if (ext == "zip") content_type = "application/zip";
    else if (ext == "lrc" || ext == "txt")
        content_type = "text/plain";
    else if (ext == "mp3") content_type = "audio/mpeg";
    std::string filename =
        std::filesystem::path(file_path).filename().string();
    HttpResult r = HttpClient::postMultipart(
        base_ + "/sendDocument",
        {{"chat_id", std::to_string(chat_id)}, {"caption", caption}},
        "document", filename, bytes, content_type, {}, 120);
    if (!r.ok()) {
        std::string err = "HTTP " + std::to_string(r.status) +
                          (r.error.empty() ? "" : ": " + r.error);
        if (!r.body.empty()) err += " body=" + r.body;
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendFile failed: " << err << "\n";
        return -1;
    }
    Json parsed;
    try {
        parsed = Json::parse(r.body);
    } catch (const std::exception& e) {
        if (error_out)
            *error_out = std::string("bad JSON from Telegram: ") + e.what();
        return -1;
    }
    if (!parsed.find("ok") || !parsed.find("ok")->asBool()) {
        const Json* desc = parsed.find("description");
        if (error_out)
            *error_out = desc ? desc->asString()
                              : "Telegram API returned ok=false";
        return -1;
    }
    const Json* result = parsed.find("result");
    if (!result) return -1;
    const Json* id = result->find("message_id");
    return id ? id->asInt() : -1;
}

int64_t TelegramApi::sendAudio(int64_t chat_id, const std::string& audio,
                               const std::string& caption,
                               const Json& reply_markup,
                               std::string* error_out) {
    Json params = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"audio", Json(audio)},
        {"caption", Json(caption)},
        {"parse_mode", Json("HTML")},
    });
    if (!reply_markup.isNull()) params.set("reply_markup", reply_markup);
    std::string err;
    Json result = call("sendAudio", params, 60, err);
    if (result.isNull()) {
        if (error_out) *error_out = err;
        std::cerr << "[tg] sendAudio failed: " << err << "\n";
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
