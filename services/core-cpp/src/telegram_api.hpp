// telegram_api.hpp — Telegram Bot API client (HTTPS long polling).
//
// This is the core's only Telegram surface. It uses Bot API HTTP, not
// MTProto/TDLib: anonx-cpp's TDLib client and TinaMusic's in-process MTProto
// voice path are NOT ported here — the voice path is isolated behind the
// VoiceCall interface (voice_call.hpp) because TDLib/ntgcalls cannot build
// in this sandbox. The router, dispatcher and IPC below are real.
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
    std::string media_kind;   // audio|video|voice|video_note|document
    std::string media_title;  // audio title / file name, when present
    // Media on the replied-to message (for /play from a reply).
    bool has_reply_media = false;
    std::string reply_media_file_id;
    std::string reply_media_kind;   // audio|video|voice|video_note|document
    std::string reply_media_title;  // audio title / file name, when present
};

struct TgUpdate {
    int64_t update_id = 0;
    TgMessage message;  // only message updates are consumed today
    bool has_message = false;
};

class TelegramApi {
public:
    explicit TelegramApi(const Config& cfg);
    virtual ~TelegramApi() = default;

    // Parse one Bot API message object. Public so tests can exercise
    // reply-media parsing without a network round-trip.
    static TgMessage parseMessage(const Json& j);

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
    int64_t getChatId(const std::string& username_or_id);

    // deleteMessage / leaveChat / forwardMessage / copyMessage — used by
    // clean mode, cmddelete, /broadcast and bot-removed handling.
    // forward/copy are virtual so dispatcher tests can script fan-out
    // outcomes without touching the network (like sendMessageId).
    bool deleteMessage(int64_t chat_id, int64_t message_id);
    bool leaveChat(int64_t chat_id);
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
    // Invite link for the assistant join (NoxBeats ensure_assistant).
    std::string exportChatInviteLink(int64_t chat_id);
    // Resolve a user id to a display name (best effort).
    std::string getUserMention(int64_t user_id);

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
