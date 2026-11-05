// tg_types.hpp — plain Telegram message shapes (no transport).
//
// These structs are the dispatcher's view of a Telegram update: the
// message, callback query, inline query and member update the core
// routes, plus the two keyboard shapes. They carry no Bot API or
// TDLib behaviour — the transport lives behind the TelegramSender
// interface (dispatcher.hpp), implemented over the TDLib bot session
// (TdLibTelegramSender). Moved here from the deleted telegram_api.hpp
// (Phase 2, Wave 2) so ui_texts.hpp (keyboard builders) and the
// dispatcher share them without depending on a deleted HTTP client.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace parlay {

struct TgUser {
    int64_t id = 0;
    std::string first_name;
    std::string username;
    bool is_bot = false;
};

struct TgChat {
    int64_t id = 0;
    std::string type;  // "private" | "group" | "supergroup" | "channel"
    std::string title;
};

struct TgMessage {
    int64_t message_id = 0;
    TgUser from;
    TgChat chat;
    std::string text;
    bool has_text = false;
    // Reply target (for /auth, /bug, broadcast source).
    int64_t reply_to_message_id = 0;
    int64_t reply_from_id = 0;
    bool has_reply_to = false;
    // Service events (NoxBeats on_new_members / on_video_chat).
    std::vector<TgUser> new_chat_members;
    bool video_chat_started = false;
    bool video_chat_ended = false;
    // Media on the message itself (audio sent with a /play caption).
    bool has_media = false;
    std::string media_file_id;
    std::string media_file_unique_id;  // stable id for share feedback (§22)
    std::string media_kind;   // audio|video|voice|video_note|document
    std::string media_title;  // audio title / file name, when present
    // Media on the replied-to message (for /play from a reply).
    bool has_reply_media = false;
    std::string reply_media_file_id;
    std::string reply_media_file_unique_id;
    std::string reply_media_kind;   // audio|video|voice|video_note|document
    std::string reply_media_title;  // audio title / file name, when present
};

// One inline-keyboard button. Exactly one of callback_data / url /
// switch_inline_query / copy_text is set; the others stay empty.
// copy_text is a client-side copy button (NoxBeats Button.copy_text):
// tapping it copies the text, it produces no callback query.
struct InlineButton {
    std::string text;
    std::string callback_data;       // callback_data=...
    std::string url;                 // url=...
    std::string switch_inline_query; // switch_inline_query=...
    std::string copy_text;           // copy_text=... (client-side copy)
    bool switch_current_chat = false;  // ..._current_chat variant
    bool has_switch = false;           // switch_inline_query is set

    static InlineButton callback(const std::string& text,
                                 const std::string& data) {
        InlineButton b;
        b.text = text;
        b.callback_data = data;
        return b;
    }
    static InlineButton link(const std::string& text, const std::string& u) {
        InlineButton b;
        b.text = text;
        b.url = u;
        return b;
    }
    // Copy button: tapping it copies `copy` to the clipboard; no
    // callback query is produced, so no handler is needed for it.
    static InlineButton copy(const std::string& text,
                             const std::string& copy) {
        InlineButton b;
        b.text = text;
        b.copy_text = copy;
        return b;
    }
    // switch_inline_query button; current_chat=true -> the
    // switch_inline_query_current_chat variant (stays in this chat).
    static InlineButton switchInline(const std::string& text,
                                     const std::string& query,
                                     bool current_chat) {
        InlineButton b;
        b.text = text;
        b.switch_inline_query = query;
        b.switch_current_chat = current_chat;
        b.has_switch = true;
        return b;
    }
};

using InlineKeyboard = std::vector<std::vector<InlineButton>>;

// Persistent reply keyboard (beetube / rustify §14). Rows are plain
// button labels; resize_keyboard is always true in Parlay.
struct ReplyKeyboard {
    std::vector<std::vector<std::string>> rows;
    bool resize = true;
    bool one_time = false;
};

struct TgCallbackQuery {
    std::string id;
    TgUser from;
    // The message the button sits on (absent for inline-mode buttons).
    int64_t chat_id = 0;
    int64_t message_id = 0;
    bool has_message = false;
    std::string data;  // callback_data, "pl:..." namespace (§1)
};

struct TgInlineQuery {
    std::string id;
    TgUser from;
    std::string query;     // raw query text ("" = hint)
    std::string offset;    // pagination offset (unused: one page)
    std::string chat_type; // "sender"|"private"|"group"|"supergroup"|"channel"
};

// Bot status change in a chat (my_chat_member update): used for the
// demotion goodbye (UI_INVENTORY.md §2) — the bot notices when it loses
// admin rights and leaves.
struct TgChatMemberUpdate {
    int64_t chat_id = 0;
    int64_t user_id = 0;  // the member whose status changed (the bot)
    std::string old_status;  // e.g. "administrator"
    std::string new_status;  // e.g. "member"
};

struct TgUpdate {
    int64_t update_id = 0;
    TgMessage message;  // only message updates are consumed today
    bool has_message = false;
    TgCallbackQuery callback;
    bool has_callback_query = false;
    TgChatMemberUpdate my_chat_member;
    bool has_my_chat_member = false;
    TgInlineQuery inline_query;
    bool has_inline_query = false;
};

}  // namespace parlay
