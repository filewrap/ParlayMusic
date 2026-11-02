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
    // Media on a reply (for /play from a reply).
    bool has_media = false;
    std::string media_file_id;
};

struct TgUpdate {
    int64_t update_id = 0;
    TgMessage message;  // only message updates are consumed today
    bool has_message = false;
};

class TelegramApi {
public:
    explicit TelegramApi(const Config& cfg);

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
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to = 0);
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to, std::string* error_out);

    // getChatMember(chat_id, user_id) -> status string ("creator",
    // "administrator", "member", "restricted", "left", "kicked") or "".
    // Used for the NoxBeats-style admin check in the dispatcher.
    std::string getChatMemberStatus(int64_t chat_id, int64_t user_id);

    // getChatAdministrators -> ids of creator + administrators (NoxBeats
    // get_admins / /admincache). Empty on error.
    std::vector<int64_t> getChatAdministrators(int64_t chat_id);

    // getChat(id or @username) -> chat id, or 0 (for /blacklist @name,
    // /channelplay @channel).
    int64_t getChatId(const std::string& username_or_id);

    // deleteMessage / leaveChat / forwardMessage / copyMessage — used by
    // clean mode, cmddelete, /broadcast and bot-removed handling.
    bool deleteMessage(int64_t chat_id, int64_t message_id);
    bool leaveChat(int64_t chat_id);
    bool forwardMessage(int64_t to_chat, int64_t from_chat,
                        int64_t message_id);
    bool copyMessage(int64_t to_chat, int64_t from_chat, int64_t message_id);
    // Invite link for the assistant join (NoxBeats ensure_assistant).
    std::string exportChatInviteLink(int64_t chat_id);
    // Resolve a user id to a display name (best effort).
    std::string getUserMention(int64_t user_id);

    // Test the token against getMe; returns the bot username or "".
    std::string getMeUsername();
    int64_t getMeId();

private:
    std::string base_;
    Json call(const std::string& method, const Json& params, int timeout_sec,
              std::string& error_out);
    static TgMessage parseMessage(const Json& j);
};

}  // namespace parlay
