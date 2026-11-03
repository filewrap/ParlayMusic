// telegram_api.hpp — Telegram Bot API client (HTTPS long polling).
//
// This is the core's only Telegram surface. It uses Bot API HTTP, not
// MTProto/TDLib: anonx-cpp's TDLib client and TinaMusic's in-process MTProto
// voice path are NOT ported here — the voice path is isolated behind the
// VoiceCall interface (voice_call.hpp) because TDLib/ntgcalls cannot build
// in this sandbox. The router, dispatcher and IPC below are real.
//
// Phase 9 (UI port): the interactive layer — sendPhoto, HTML messages with
// inline/reply keyboards, editMessageText/Caption/ReplyMarkup,
// answerCallbackQuery and callback_query parsing (UI_INVENTORY.md §28.1).
// All new methods are virtual so dispatcher tests can script them without
// a network, like sendMessageId / sendDocument before them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "json.hpp"

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
// switch_inline_query is set; the others stay empty.
struct InlineButton {
    std::string text;
    std::string callback_data;       // callback_data=...
    std::string url;                 // url=...
    std::string switch_inline_query; // switch_inline_query=...
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

// Build the Bot API reply_markup JSON for an inline keyboard:
// {"inline_keyboard": [[{"text":..,"callback_data":..}, ...], ...]}.
// A button's callback_data is validated by the caller (64-byte limit,
// UI_INVENTORY.md §1); this builder just serializes.
Json inlineKeyboardJson(const InlineKeyboard& kb);

// Persistent reply keyboard (beetube / rustify §14). Rows are plain
// button labels; resize_keyboard is always true in Parlay.
struct ReplyKeyboard {
    std::vector<std::vector<std::string>> rows;
    bool resize = true;
    bool one_time = false;
};

Json replyKeyboardJson(const ReplyKeyboard& kb);
// {"remove_keyboard": true} — Parlay never leaves a reply keyboard
// behind after /start or when leaving a flow that used one.
Json removeKeyboardJson();

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

class TelegramApi {
public:
    explicit TelegramApi(const Config& cfg);
    virtual ~TelegramApi() = default;

    // Parse one Bot API message object. Public so tests can exercise
    // reply-media parsing without a network round-trip.
    static TgMessage parseMessage(const Json& j);
    // Parse one Bot API callback_query object. Public so tests can
    // exercise callback routing without a network round-trip.
    static TgCallbackQuery parseCallbackQuery(const Json& j);
    // Parse one Bot API my_chat_member object (bot status change).
    static TgChatMemberUpdate parseMyChatMember(const Json& j);
    // Parse one Bot API inline_query object. Public so tests can
    // exercise inline routing without a network round-trip.
    static TgInlineQuery parseInlineQuery(const Json& j);

    // Long-poll getUpdates. Returns updates with update_id > offset's last
    // seen id; empty vector on timeout (normal) or on a transport error
    // (error string filled, caller decides whether to back off).
    std::vector<TgUpdate> getUpdates(int64_t offset, int timeout_sec,
                                     std::string& error_out);

    // sendMessage(chat_id, text). Returns false + error on failure.
    bool sendMessage(int64_t chat_id, const std::string& text,
                     std::string& error_out);
    // sendMessage returning the new message id (-1 on failure). Needed
    // for clean-mode deletion scheduling (NoxBeats reply()).
    virtual int64_t sendMessageId(int64_t chat_id, const std::string& text,
                                  int64_t reply_to = 0);
    virtual int64_t sendMessageId(int64_t chat_id, const std::string& text,
                                  int64_t reply_to, std::string* error_out);

    // --- Phase 9 interactive layer (all virtual for tests) -------------
    // sendMessage with Bot API HTML parse mode + an optional
    // reply_markup (inline keyboard JSON from inlineKeyboardJson, reply
    // keyboard JSON from replyKeyboardJson, or removeKeyboardJson()).
    // Pass Json(nullptr) for no markup. Returns the message id, -1 on
    // failure. The dispatcher sends all UI text through this so a stray
    // '<' in a track title cannot break a card — callers escape
    // user-derived text with ui_texts::htmlEscape first.
    virtual int64_t sendMessageHtml(int64_t chat_id, const std::string& text,
                                    const Json& reply_markup,
                                    int64_t reply_to = 0,
                                    std::string* error_out = nullptr);
    // sendPhoto(chat_id, photo URL or file_id, caption, reply_markup).
    // The photo rule (UI_INVENTORY.md §0.3): now-playing, queue, search
    // and album/playlist cards go out as a photo with the card as the
    // caption when a thumbnail URL is known and thumbnails are on for
    // the chat; the caller falls back to sendMessageHtml otherwise.
    // Returns the message id, -1 on failure.
    virtual int64_t sendPhoto(int64_t chat_id, const std::string& photo,
                              const std::string& caption,
                              const Json& reply_markup,
                              std::string* error_out = nullptr);
    // Edit a message's text (HTML) and/or its inline keyboard. False +
    // error when the message is gone or unchanged ("message is not
    // modified" is reported as an error by the caller, not here).
    virtual bool editMessageText(int64_t chat_id, int64_t message_id,
                                 const std::string& text,
                                 const Json& reply_markup,
                                 std::string* error_out = nullptr);
    // Edit a photo message's caption (HTML) and/or its keyboard.
    virtual bool editMessageCaption(int64_t chat_id, int64_t message_id,
                                    const std::string& caption,
                                    const Json& reply_markup,
                                    std::string* error_out = nullptr);
    // Replace only the inline keyboard of a message (used to remove a
    // button row, e.g. after a download is cancelled). Pass
    // Json(nullptr) to drop the keyboard entirely.
    virtual bool editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                        const Json& reply_markup,
                                        std::string* error_out = nullptr);
    // answerCallbackQuery: the toast/alert after a button tap. text
    // empty = silent ack (progress-bar no-op button). show_alert=true
    // pops the alert dialog (denials, expired buttons). cache_time_sec
    // is passed through to the API (0 = default).
    virtual bool answerCallbackQuery(const std::string& callback_query_id,
                                     const std::string& text, bool show_alert,
                                     int cache_time_sec = 0,
                                     std::string* error_out = nullptr);
    // answerInlineQuery: answer an inline query with a JSON array of
    // InlineQueryResult objects (built by the dispatcher from the
    // search.inline Tracks). switch_pm_text/parameter render the hint
    // button above an empty list (§13: empty / 1-2-char queries);
    // empty strings omit them. False + error on failure.
    virtual bool answerInlineQuery(const std::string& inline_query_id,
                                   const Json& results, int cache_time_sec,
                                   const std::string& switch_pm_text,
                                   const std::string& switch_pm_parameter,
                                   std::string* error_out = nullptr);

    // getFile(file_id) -> Telegram file_path ("audio/file_1.mp3"), or ""
    // + error when the file_id is unknown to the Bot API.
    virtual std::string getFilePath(const std::string& file_id,
                                    std::string& error_out);
    // Download a Telegram file by file_id into dest_dir/<file_id>.<ext>
    // (extension from the file_path). saved_path_out receives the local
    // path. False + error on any failure (unknown file_id, HTTP error,
    // empty body, write failure). Bot API downloads cap at 20 MB.
    virtual bool downloadFile(const std::string& file_id,
                              const std::string& dest_dir,
                              std::string& saved_path_out,
                              std::string& error_out);

    // getChatMember(chat_id, user_id) -> status string ("creator",
    // "administrator", "member", "restricted", "left", "kicked") or "".
    // Used for the NoxBeats-style admin check in the dispatcher.
    virtual std::string getChatMemberStatus(int64_t chat_id, int64_t user_id);

    // getChatAdministrators -> ids of creator + administrators (NoxBeats
    // get_admins / /admincache). Empty on error.
    virtual std::vector<int64_t> getChatAdministrators(int64_t chat_id);

    // getChat(id or @username) -> chat id, or 0 (for /blacklist @name,
    // /channelplay @channel).
    virtual int64_t getChatId(const std::string& username_or_id);

    // deleteMessage / leaveChat / forwardMessage / copyMessage — used by
    // clean mode, cmddelete, /broadcast and bot-removed handling.
    // forward/copy are virtual so dispatcher tests can script fan-out
    // outcomes without touching the network (like sendMessageId).
    // deleteMessage is virtual for the same reason (pl:close, /player
    // re-send, search-message cleanup).
    virtual bool deleteMessage(int64_t chat_id, int64_t message_id);
    virtual bool leaveChat(int64_t chat_id);
    virtual bool forwardMessage(int64_t to_chat, int64_t from_chat,
                                int64_t message_id);
    virtual bool copyMessage(int64_t to_chat, int64_t from_chat,
                             int64_t message_id);
    // sendDocument(chat_id, filename, content, caption) -> the new
    // message id, or -1 + error. Multipart upload (Bot API
    // sendDocument): the /broadcast failure list goes out as an
    // errors.txt attachment instead of inline text. Virtual so
    // dispatcher tests can record the attachment without a network.
    virtual int64_t sendDocument(int64_t chat_id,
                                 const std::string& filename,
                                 const std::string& content,
                                 const std::string& caption,
                                 std::string* error_out = nullptr);
    // Send an audio file from disk (Bot API sendAudio, multipart).
    // Used for FX results (§8): the audio-fx service returns a file
    // path and the bot must deliver that file to the user. The
    // content type follows the extension (mp3/m4a/flac/opus/ogg/wav).
    // Returns the message id, or -1 + error.
    virtual int64_t sendAudioFile(int64_t chat_id,
                                  const std::string& file_path,
                                  const std::string& caption,
                                  std::string* error_out = nullptr);
    // Send a file from disk as a document (Bot API sendDocument,
    // multipart). Used for Spotify playlist ZIPs and .lrc lyric
    // files (§8, §6). The content type follows the extension.
    virtual int64_t sendFile(int64_t chat_id, const std::string& file_path,
                             const std::string& caption,
                             std::string* error_out = nullptr);
    // Send audio by file_id (or URL) with an optional caption and
    // inline keyboard (Bot API sendAudio, JSON). Used for the §22
    // share flow: the sender's audio is delivered to the target by
    // file_id, with the Feedback / Report User buttons attached.
    virtual int64_t sendAudio(int64_t chat_id, const std::string& audio,
                              const std::string& caption,
                              const Json& reply_markup,
                              std::string* error_out = nullptr);
    // Invite link for the assistant join (NoxBeats ensure_assistant).
    std::string exportChatInviteLink(int64_t chat_id);
    // Resolve a user id to a display name (best effort).
    virtual std::string getUserMention(int64_t user_id);

    // Test the token against getMe; returns the bot username or "".
    std::string getMeUsername();
    int64_t getMeId();

private:
    std::string base_;
    std::string file_base_;  // <api_base>/file/bot<token> (getFile downloads)
    Json call(const std::string& method, const Json& params, int timeout_sec,
              std::string& error_out);
};

}  // namespace parlay
