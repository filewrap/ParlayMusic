// dispatcher.hpp — command dispatcher.
//
// Port shape: anonx-cpp's CommandDispatcher (parse_command_and_args,
// dispatch table, per-chat rate limiter) + NoxBeats' full command surface
// (Bad/cmd/*.cpp, Bad/cmd/common.cpp): a route table with Scope + Perm,
// the alias map, the c-command map, the owner/sudo/auth/admin-mode model,
// clean mode / cmddelete, maintenance + blacklist gates, and the
// background clean-mode sweeper.
//
// Phase 2 (FINAL-ARCHITECTURE.md §9): the transport is the persistent
// UDS IpcClient — one unified backend, no engine fallback router, no
// /healthz pre-probes. All engine calls go through vcCall/vcGet, which
// post straight to the one backend. The core never touches ntgcalls or
// audio: voice ops (play/pause/resume/skip/seek/stop/mute/speed/bass)
// are backend actions, and voice joins arrive as duplex JoinRequests
// over UDS (onJoinRequest) that the core answers by running TDLib
// joinVideoChat on the shared assistant session (TdLibDualSession —
// never a second client on data/tdlib, which TDLib locks). Track
// transitions are backend-driven events (onBackendEvent), not a
// 5-second watcher. Sends go through the TelegramSender interface,
// implemented over the TDLib bot session (TdLibTelegramSender) — no
// Bot API HTTP traffic remains anywhere in the dispatcher. Rate
// limiting is the token-bucket RateLimiter (per-chat + per-user,
// from parlay.toml); denials are answered honestly, never silently
// dropped.
//
// Threading: the dispatcher runs on the main thread (handleMessage /
// handleCallback / handleInlineQuery are called from main's TDLib pump
// loop). The IpcClient's reader/worker threads must never call TDLib
// directly (TdLibClient::call/pollUpdate share unguarded state with
// main's pump): onBackendEvent and onJoinRequest only enqueue work,
// and the sweeper's deletes go through the same queue; main drains
// everything with processPendingWork() once per loop iteration.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "ipc.hpp"
#include "rate_limiter.hpp"
#include "store.hpp"
#include "tdlib_client.hpp"
#include "tg_types.hpp"
#include "ui_texts.hpp"

namespace parlay {

// Forward declaration for the Rich Messages engine
// (rich_message.hpp). The sender interface only passes it by
// reference; dispatcher.cpp includes the full header.
namespace rich {
class RichMessage;
}  // namespace rich

// --- Telegram transport -----------------------------------------------------
// The dispatcher's only Telegram surface. The production implementation
// (TdLibTelegramSender, below) drives the TDLib bot session; tests
// substitute a recording fake. Method-for-method this mirrors the old
// TelegramApi surface the dispatcher used, minus the Bot API-only
// parts (getUpdates long-poll, getFilePath, exportChatInviteLink —
// gone with the Bot API client). Keyboards arrive as typed values,
// not Bot API reply_markup JSON: the sender builds the TDLib
// replyMarkup objects itself.
//
// Conventions (match the old client, so handlers need no rework):
//   * send* returning int64_t: the new message id, -1 + error_out on
//     failure. An empty InlineKeyboard means "no markup".
//   * editMessageText/editMessageCaption with an empty keyboard pass
//     null markup (TDLib: "pass null if none").
//   * editMessageReplyMarkup with an empty keyboard removes the
//     keyboard entirely.
//   * downloadFile resolves a TDLib remote file id (getRemoteFile +
//     synchronous downloadFile) and returns the local path in
//     saved_path_out. No 20 MB Bot API cap: TDLib downloads any size.
class TelegramSender {
public:
    virtual ~TelegramSender() = default;

    // Plain-text send. False + error_out on failure.
    virtual bool sendMessage(int64_t chat_id, const std::string& text,
                             std::string& error_out) = 0;
    // Plain-text send returning the new message id (-1 on failure).
    // Needed for clean-mode deletion scheduling (NoxBeats reply()).
    virtual int64_t sendMessageId(int64_t chat_id, const std::string& text,
                                  int64_t reply_to = 0,
                                  std::string* error_out = nullptr) = 0;
    // HTML send with an optional inline keyboard. The text is sent
    // as a TDLib Rich Message (inputMessageRichMessage with a
    // richMessageSourceHtml source), so headings <h1>..<h6> and the
    // rest of the Rich HTML tag list are legal here — the classic
    // parseTextEntities path this used to take rejected them. All
    // Phase 9 UI text goes through this; user-derived text inside
    // must already be HTML-escaped (ui_texts::htmlEscape).
    virtual int64_t sendMessageHtml(int64_t chat_id, const std::string& text,
                                    const InlineKeyboard& kb,
                                    int64_t reply_to = 0,
                                    std::string* error_out = nullptr) = 0;
    // Rich Message send: a built parlay::rich::RichMessage (HTML or
    // Blocks source) + a prebuilt reply_markup JSON (use
    // TdLibTelegramSender::inlineMarkup(kb), or Json(nullptr) for
    // none). The default implementation reports "not supported" so
    // other TelegramSender implementations keep compiling;
    // TdLibTelegramSender (the only production sender) implements
    // it for real. Returns the new message id, -1 + error_out on
    // failure.
    virtual int64_t sendRichMessage(int64_t chat_id,
                                    const rich::RichMessage& msg,
                                    const Json& reply_markup,
                                    int64_t reply_to = 0,
                                    std::string* error_out = nullptr);
    // HTML send with a persistent reply keyboard. Like
    // sendMessageHtml, the text goes out as a Rich Message.
    virtual int64_t sendMessageReplyKb(int64_t chat_id,
                                       const std::string& text,
                                       const ReplyKeyboard& kb,
                                       int64_t reply_to = 0,
                                       std::string* error_out = nullptr) = 0;
    // HTML send removing any reply keyboard. Like sendMessageHtml,
    // the text goes out as a Rich Message.
    virtual int64_t sendMessageRemoveKb(int64_t chat_id,
                                        const std::string& text,
                                        int64_t reply_to = 0,
                                        std::string* error_out = nullptr) = 0;
    // Photo send (caption is classic HTML formattedText — captions
    // cannot be Rich Messages in TDLib, so <h1>..<h6> in a caption
    // are downgraded to bold on retry; see sendPhoto). `photo` is a
    // TDLib remote file id (inputFileRemote) or a local file path
    // (inputFileLocal). A remote HTTP(S) URL is NOT valid: the
    // sender fails it honestly and the caller falls back to a text
    // card (sendNowPlayingCard).
    virtual int64_t sendPhoto(int64_t chat_id, const std::string& photo,
                              const std::string& caption,
                              const InlineKeyboard& kb,
                              std::string* error_out = nullptr) = 0;
    // Edit a message's text (Rich Message, like sendMessageHtml)
    // and/or its inline keyboard.
    virtual bool editMessageText(int64_t chat_id, int64_t message_id,
                                 const std::string& text,
                                 const InlineKeyboard& kb,
                                 std::string* error_out = nullptr) = 0;
    // Edit a photo message's caption (classic HTML formattedText)
    // and/or its keyboard.
    virtual bool editMessageCaption(int64_t chat_id, int64_t message_id,
                                    const std::string& caption,
                                    const InlineKeyboard& kb,
                                    std::string* error_out = nullptr) = 0;
    // Replace only the inline keyboard of a message (empty = drop it).
    virtual bool editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                        const InlineKeyboard& kb,
                                        std::string* error_out = nullptr) = 0;
    // answerCallbackQuery: the toast/alert after a button tap. text
    // empty = silent ack. show_alert=true pops the alert dialog.
    virtual bool answerCallbackQuery(const std::string& callback_query_id,
                                     const std::string& text, bool show_alert,
                                     int cache_time_sec = 0,
                                     std::string* error_out = nullptr) = 0;
    // answerInlineQuery: `results` are the Bot API-shaped
    // InlineQueryResult objects the dispatcher builds (inlineTrackResult
    // / inlineArticleResult); the sender translates them to TDLib
    // inputInlineQueryResult values (audio_url is a native TDLib field,
    // so URL audio results survive the translation).
    virtual bool answerInlineQuery(const std::string& inline_query_id,
                                   const Json& results, int cache_time_sec,
                                   const std::string& switch_pm_text,
                                   const std::string& switch_pm_parameter,
                                   std::string* error_out = nullptr) = 0;

    // getFile/download: download a Telegram file by remote file id
    // into the TDLib files dir (synchronous downloadFile); saved_path_out
    // receives the local path. False + error on any failure.
    virtual bool downloadFile(const std::string& file_id,
                              const std::string& dest_dir,
                              std::string& saved_path_out,
                              std::string& error_out) = 0;
    // getChatMember(chat_id, user_id) -> status string ("creator",
    // "administrator", "member", "restricted", "left", "kicked") or "".
    virtual std::string getChatMemberStatus(int64_t chat_id,
                                            int64_t user_id) = 0;
    // getChatAdministrators -> ids of creator + administrators.
    virtual std::vector<int64_t> getChatAdministrators(int64_t chat_id) = 0;
    // getChat(id or @username) -> chat id, or 0.
    virtual int64_t getChatId(const std::string& username_or_id) = 0;

    // deleteMessage / leaveChat / forwardMessage / copyMessage — used by
    // clean mode, cmddelete, /broadcast and bot-removed handling.
    virtual bool deleteMessage(int64_t chat_id, int64_t message_id) = 0;
    virtual bool leaveChat(int64_t chat_id) = 0;
    virtual bool forwardMessage(int64_t to_chat, int64_t from_chat,
                                int64_t message_id) = 0;
    virtual bool copyMessage(int64_t to_chat, int64_t from_chat,
                             int64_t message_id) = 0;
    // sendDocument(chat_id, filename, content, caption): the /broadcast
    // failure list goes out as an errors.txt attachment.
    virtual int64_t sendDocument(int64_t chat_id,
                                 const std::string& filename,
                                 const std::string& content,
                                 const std::string& caption,
                                 std::string* error_out = nullptr) = 0;
    // Send an audio file from disk. Used for FX results (§8).
    virtual int64_t sendAudioFile(int64_t chat_id,
                                  const std::string& file_path,
                                  const std::string& caption,
                                  std::string* error_out = nullptr) = 0;
    // Send a file from disk as a document. Used for Spotify playlist
    // ZIPs and .lrc lyric files (§8, §6).
    virtual int64_t sendFile(int64_t chat_id, const std::string& file_path,
                             const std::string& caption,
                             std::string* error_out = nullptr) = 0;
    // Send audio by remote file_id with an optional caption and inline
    // keyboard. Used for the §22 share flow.
    virtual int64_t sendAudio(int64_t chat_id, const std::string& audio,
                              const std::string& caption,
                              const InlineKeyboard& kb,
                              std::string* error_out = nullptr) = 0;
    // Invite-link-era helper kept for the share flow: resolve a user id
    // to a display name (best effort).
    virtual std::string getUserMention(int64_t user_id) = 0;
};

// TelegramSender over the TDLib bot session (TdLibClient::call).
// All calls run on the thread that invokes the sender — the dispatcher
// guarantees that is the main thread (see the file header note).
class TdLibTelegramSender : public TelegramSender {
public:
    // data_dir is used for the sendDocument temp file only.
    TdLibTelegramSender(TdLibClient& bot, std::string data_dir);
    bool sendMessage(int64_t chat_id, const std::string& text,
                     std::string& error_out) override;
    int64_t sendMessageId(int64_t chat_id, const std::string& text,
                          int64_t reply_to = 0,
                          std::string* error_out = nullptr) override;
    int64_t sendMessageHtml(int64_t chat_id, const std::string& text,
                            const InlineKeyboard& kb, int64_t reply_to = 0,
                            std::string* error_out = nullptr) override;
    int64_t sendRichMessage(int64_t chat_id, const rich::RichMessage& msg,
                            const Json& reply_markup, int64_t reply_to = 0,
                            std::string* error_out = nullptr) override;
    int64_t sendMessageReplyKb(int64_t chat_id, const std::string& text,
                               const ReplyKeyboard& kb, int64_t reply_to = 0,
                               std::string* error_out = nullptr) override;
    int64_t sendMessageRemoveKb(int64_t chat_id, const std::string& text,
                                int64_t reply_to = 0,
                                std::string* error_out = nullptr) override;
    int64_t sendPhoto(int64_t chat_id, const std::string& photo,
                      const std::string& caption, const InlineKeyboard& kb,
                      std::string* error_out = nullptr) override;
    bool editMessageText(int64_t chat_id, int64_t message_id,
                         const std::string& text, const InlineKeyboard& kb,
                         std::string* error_out = nullptr) override;
    bool editMessageCaption(int64_t chat_id, int64_t message_id,
                            const std::string& caption,
                            const InlineKeyboard& kb,
                            std::string* error_out = nullptr) override;
    bool editMessageReplyMarkup(int64_t chat_id, int64_t message_id,
                                const InlineKeyboard& kb,
                                std::string* error_out = nullptr) override;
    bool answerCallbackQuery(const std::string& callback_query_id,
                             const std::string& text, bool show_alert,
                             int cache_time_sec = 0,
                             std::string* error_out = nullptr) override;
    bool answerInlineQuery(const std::string& inline_query_id,
                           const Json& results, int cache_time_sec,
                           const std::string& switch_pm_text,
                           const std::string& switch_pm_parameter,
                           std::string* error_out = nullptr) override;
    bool downloadFile(const std::string& file_id,
                      const std::string& dest_dir,
                      std::string& saved_path_out,
                      std::string& error_out) override;
    std::string getChatMemberStatus(int64_t chat_id,
                                    int64_t user_id) override;
    std::vector<int64_t> getChatAdministrators(int64_t chat_id) override;
    int64_t getChatId(const std::string& username_or_id) override;
    bool deleteMessage(int64_t chat_id, int64_t message_id) override;
    bool leaveChat(int64_t chat_id) override;
    bool forwardMessage(int64_t to_chat, int64_t from_chat,
                        int64_t message_id) override;
    bool copyMessage(int64_t to_chat, int64_t from_chat,
                     int64_t message_id) override;
    int64_t sendDocument(int64_t chat_id, const std::string& filename,
                         const std::string& content,
                         const std::string& caption,
                         std::string* error_out = nullptr) override;
    int64_t sendAudioFile(int64_t chat_id, const std::string& file_path,
                          const std::string& caption,
                          std::string* error_out = nullptr) override;
    int64_t sendFile(int64_t chat_id, const std::string& file_path,
                     const std::string& caption,
                     std::string* error_out = nullptr) override;
    int64_t sendAudio(int64_t chat_id, const std::string& audio,
                      const std::string& caption, const InlineKeyboard& kb,
                      std::string* error_out = nullptr) override;
    std::string getUserMention(int64_t user_id) override;

    // TDLib replyMarkup JSON for an inline keyboard (Json(nullptr)
    // when empty = "pass null if none"). Public so the serialisation
    // is directly testable (tdlib_test): a copy button must serialise
    // as inlineKeyboardButtonTypeCopyText with no callback data.
    static Json inlineMarkup(const InlineKeyboard& kb);

private:
    // parseTextEntities (HTML) or a plain formattedText when
    // parse_mode is empty. Json() (null) + error_out on parse failure.
    // TdLibClient has the same helpers but they are private, so the
    // sender builds the formattedText itself via bot_.call().
    Json formattedText(const std::string& text, const std::string& parse_mode,
                       std::string& error_out);
    // inputMessageText content object for `text` (Json() on failure).
    Json inputMessageText(const std::string& text,
                          const std::string& parse_mode,
                          std::string& error_out);
    static Json replyMarkup(const ReplyKeyboard& kb);
    // One Bot API-shaped InlineQueryResult -> its TDLib
    // inputInlineQueryResult (audio keeps audio_url; article keeps its
    // message_text).
    static Json inlineResult(const Json& r, TdLibClient& bot);
    // sendMessage with a prebuilt content object + markup. Returns the
    // new message id, -1 + error_out on failure.
    int64_t sendWithContent(int64_t chat_id, const Json& content,
                            const Json& reply_markup, int64_t reply_to,
                            std::string* error_out);
    // Send a local file as audio (audio=true) or document (false).
    int64_t sendLocalFile(int64_t chat_id, const std::string& file_path,
                          const std::string& caption, bool audio,
                          std::string* error_out);

    TdLibClient& bot_;
    std::string data_dir_;
};

struct CommandContext {
    TgMessage message;
    std::string command;  // lowercased, without leading '/' or @bot suffix
    std::string args;     // raw argument text after the command
    bool via_channel = false;  // c-command: run against the linked channel
    // s-alias invocation (TgMusicBot SupportsSilence: /splay, /sskip,
    // /sstop, /spause, /sresume): the base handler runs exactly as
    // for the plain command, except its SUCCESS message is
    // suppressed. Usage replies, denials, progress and backend
    // errors stay visible — silence covers only the success outcome.
    bool silent_success = false;
    // Phase 1 telemetry: steady-clock microseconds when
    // Dispatcher::handleMessage received this command (0 = unknown,
    // e.g. a context built outside handleMessage). t_dispatch_to_ipc
    // measures from here to the engine IPC post.
    int64_t receipt_us = 0;
};

class Dispatcher {
public:
    Dispatcher(const Config& cfg, TelegramSender& tg, IpcClient& ipc,
               Store& store, TdLibClient& assistant, RateLimiter& limiter,
               const std::string& bot_username);

    // Handle one incoming message. Non-commands and denied commands are
    // answered (or ignored) here, never by the caller.
    void handleMessage(const TgMessage& msg);

    // Handle one callback_query update (Phase 9, UI_INVENTORY.md §1):
    // the "pl:<domain>:<verb>[:<arg>…]" namespace only. Unknown
    // callbacks (empty / over-64-byte / non-pl: data, unknown domain)
    // get a silent ack — empty text, query still answered — by direct
    // user order 2026-11-05, overriding the locked §7.5 "never
    // silent" rule for this point only (see phase10/PHASE10.md). A
    // known domain keeps its own honest alerts for a malformed verb
    // or bad arguments. Wrong-user taps get the "You aren't allowed"
    // toast (admin-or-requester gate per row), except the remix /menu
    // panel rule where other users are silently ignored.
    void handleCallback(const TgCallbackQuery& cq);
    // Handle one my_chat_member update (bot status change): the
    // demotion goodbye (§2) — the bot sends the notice and leaves when
    // it loses admin rights in a group.
    void handleMyChatMember(const TgChatMemberUpdate& upd);
    // Handle one chat_member update for a non-bot member whose
    // transition touches administrator/creator status (remix
    // chat_member_updated): re-fetch the chat admin list so the admin
    // cache (10 min TTL) does not serve a stale list after a
    // promotion/demotion. Returns true when the cache was refreshed;
    // ordinary member joins/leaves return false and refresh nothing.
    bool handleAdminTransition(const TgChatMemberUpdate& upd);
    // Handle one inline_query update (Phase 9 gaps, §13): the core
    // calls search.inline (vc-ts /v1/inline) and renders the
    // InlineQueryResult objects itself — vc-ts returns Tracks only.
    // Modes mirror vc-ts resolvers.js inlineMode. np/s/l pass the
    // chat_id from last_chat:<user> (ruling 2); l fetches the lyrics
    // text via /v1/lyrics with the same chat_id (ruling 3). lib/del
    // return empty results (no library backend — ruling 1).
    // Blacklisted users and (under maintenance) non-sudo users get
    // empty results, silently — the same rule as chat auth (§13).
    void handleInlineQuery(const TgInlineQuery& iq);
    // Forward a DM media message to the pending share target (§22).
    // Audio/voice go by file_id with the feedback buttons; other
    // media is forwarded as-is (no send-by-file_id call exists for
    // it). Returns true when a pending target consumed the message.
    bool forwardShareMedia(const TgMessage& msg);

    // --- Backend-driven duplex hooks (Phase 2) ---------------------------
    // Backend event (TrackStarted / TrackAdvanced / TrackEnded /
    // QueueEmpty / DownloadReady / VoiceError). Called on the IpcClient
    // reader thread: it only enqueues the event (fast, non-blocking).
    // Main drains it with processPendingWork().
    void onBackendEvent(const std::string& event, const Json& data);
    // Backend JoinRequest (duplex handshake, contracts/uds-protocol.md
    // §6): {chat_id, local_sdp}. Called on an IpcClient worker thread:
    // it enqueues the request and blocks on the future — main drains it
    // with processPendingWork(), runs the TDLib joinVideoChat on the
    // shared assistant session, and the returned Json becomes the
    // JoinResponse data {remote_sdp, group_call_id} (or an error object
    // when the join fails).
    Json onJoinRequest(const Json& request);
    // Drain everything queued by onBackendEvent / onJoinRequest / the
    // background sweeper. MAIN THREAD ONLY (it calls TDLib). Main calls
    // this once per loop iteration; tests call it directly after
    // enqueueing.
    void processPendingWork();

    // Background loop: clean-mode sweeper only (the 5-second
    // loop/autoplay watcher and the core-follow pass are deleted —
    // track transitions are backend-driven events now).
    void startBackground();
    void stopBackground();
    void setBotId(int64_t id) { bot_id_ = id; }
    void setBotUsername(const std::string& name) { bot_username_ = name; }

    // HTTP API seam (core_api.hpp VcStateProvider): live engine state
    // for one chat, for GET /api/now-playing and GET /api/queue.
    // apiNowPlaying returns the whole now-playing object; false when
    // the backend reports no state (or none is reachable).
    bool apiNowPlaying(int64_t chat_id, Json& np_out, std::string& error);
    // The backend's queue list (POST /v1/queue action=list).
    bool apiQueueList(int64_t chat_id, Json& queue_out, std::string& error);
    // vc.join (POST /v1/join): join the chat's voice call through the
    // unified backend. True only after a real join (or when the chat is
    // already in a call); false + error when the backend cannot join.
    // Called by the HTTP API; the Telegram /join command is onJoin.
    bool joinVoice(int64_t chat_id, std::string& error);
    // vc.mute (POST /v1/mute): mute (mute=true) or unmute the chat's
    // voice call through the unified backend. Same mute as /mute,
    // minus the Telegram permission check and reply. False + error
    // when nothing is playing or the backend mute/unmute fails.
    bool muteVoice(int64_t chat_id, bool mute, std::string& error);

    // Pure /speedtest helpers (no I/O; unit-tested in
    // dispatcher_test.cpp): Mbps from a byte count + elapsed seconds,
    // and the reply text from a download result + an upload outcome.
    static double speedMbps(size_t bytes, double secs);
    static std::string formatSpeedtest(double down_mbps, bool up_ok,
                                       double up_mbps, int up_status,
                                       const std::string& up_error);

private:
    enum class Scope { Any, Group, Private };
    enum class Perm { Everyone, Sudo, Owner };
    using Handler = std::function<void(const CommandContext&)>;
    struct Route {
        std::vector<std::string> cmds;
        Scope scope;
        Perm perm;
        Handler fn;
    };
    void registerCommands();

    // --- permission model (NoxBeats cmd/common.cpp) ---
    bool chatAllowed(int64_t chat_id) const;
    bool isOwner(int64_t user_id) const {
        return cfg_.owner_id != 0 && user_id == cfg_.owner_id;
    }
    bool isSudo(int64_t user_id) const;
    bool isBlUser(int64_t user_id) const;
    bool isBlChat(int64_t chat_id) const;
    // Group admin check: cached admin list (10 min TTL) then live
    // getChatMember (creator/administrator).
    bool isChatAdmin(int64_t chat_id, int64_t user_id);
    std::vector<int64_t> cachedAdmins(int64_t chat_id, bool reload = false);
    // admin_check: private chat -> true; sudo/admin -> true; else reply.
    bool adminCheck(const CommandContext& ctx);
    // can_manage_vc: admin_mode 0 = admins+auth, 1 = admins only,
    // 2 = everyone; sudo always passes.
    bool canManageVc(const CommandContext& ctx);
    // /play gate: play_mode admin-only (or force) needs admin/auth/sudo.
    bool playAllowed(const CommandContext& ctx, bool force);
    // Token-bucket gate (RateLimiter, per-chat + per-user). True when
    // the command may run; false when the limiter denied it — the
    // denial is answered HERE with the limiter's honest reason string
    // (no silent drops, ever), so the caller just stops.
    bool checkRateLimit(const CommandContext& ctx);
    bool maintenanceOn() const;

    // --- chat settings (Q7 rewire, Phase 6) -------------------------
    // The backend's bbolt store is the SINGLE home for the 15
    // chat.settings keys (FINAL-ARCHITECTURE.md §1.3 Part 2); the
    // core never keeps a second copy in state.json. settingsFor()
    // fetches them over UDS (chat.settings get-all) and caches the
    // snapshot in memory (60 s TTL, write-through on set); when the
    // backend is down it serves the cached snapshot if one exists,
    // else the locked defaults, and settingsLive() reports false so
    // status replies can say so honestly. setSetting() posts one key
    // and updates the cache on success. chatSettings() /
    // saveChatSettings() are the panel snapshot + diffed write-back
    // (only changed keys are posted); saveChatSettings() returns
    // false when a write failed so the caller never claims success.
    ui_texts::ChatSettings settingsFor(int64_t chat_id);
    bool settingsLive(int64_t chat_id);
    bool setSetting(int64_t chat_id, const std::string& key,
                    const Json& value);
    // Per-chat RUNTIME fields (NOT settings): core-voice session
    // state (loop/speed/bass/muted), channel-play link (cplay),
    // RTMP ingest (rtmp_url/rtmp_key), last played track
    // (last_track). They live in state.json under the "chatrt:"
    // hash — never in the settings hash — because no backend home
    // exists for them (they describe the core's own voice session
    // and routing, not the backend room). Reads fall back to the
    // legacy "chat:" hash once, so a pre-rewire cplay/rtmp link
    // survives the upgrade; writes go to "chatrt:" only.
    std::string chatRtField(int64_t chat_id, const std::string& field,
                            const std::string& def = "") const;
    void setChatRtField(int64_t chat_id, const std::string& field,
                        const std::string& value);
    int chatRtIntField(int64_t chat_id, const std::string& field,
                       int def = 0) const;
    // Stored playback speed (0.5-2.0) for the chat, 1.0 when unset or
    // unparseable. Used by /speed reporting; the backend owns live
    // playback, so the value is stored for the backend's FX path.
    double chatRtSpeedField(int64_t chat_id) const;
    bool isAuthUser(int64_t chat_id, int64_t user_id) const;
    // Effective voice-chat target: linked channel when via_channel.
    int64_t vcTarget(const CommandContext& ctx) const;
    bool isPrivate(const CommandContext& ctx) const {
        return ctx.message.chat.type == "private";
    }
    bool isGroup(const CommandContext& ctx) const {
        return ctx.message.chat.type == "group" ||
               ctx.message.chat.type == "supergroup";
    }

    // --- duplex join (assistant session, shared — never a second
    // client on data/tdlib) -------------------------------------------------
    // getChat -> video_chat.group_call_id (0 = none, -1 + error on
    // failure); createVideoChat when none exists; joinVideoChat(local
    // SDP) -> remote transport payload. Ported from the deleted
    // TdLibVoiceCall (voice_call.cpp) minus the ntgcalls half: the
    // backend owns the call and the audio, the core only signals.
    int32_t groupCallId(int64_t chat_id, std::string& error);
    int32_t createVideoChat(int64_t chat_id, const std::string& title,
                            std::string& error);
    std::string joinVideoChat(int32_t group_call_id,
                              const std::string& local_sdp, bool video,
                              std::string& error);
    // Full join for one JoinRequest: resolve/create the video chat,
    // joinVideoChat, fill response_out {remote_sdp, group_call_id}.
    // False + error when any step fails (no fake success).
    bool doJoinVideoChat(int64_t chat_id, const std::string& local_sdp,
                         Json& response_out, std::string& error);
    // Main-thread handling of one dequeued backend event.
    void onTrackEvent(const std::string& event, const Json& data);
    // Re-render the recorded now-playing card for a chat in place
    // (event path): edit the recorded card when one exists, else send
    // a fresh card. No-op when the backend reports no track.
    void editNowPlayingCard(int64_t chat_id, const Json& np);
    // Render the now-playing card text + keyboard for a chat and edit
    // the given card message with it (shared by refreshCard and the
    // event path).
    void refreshCardText(int64_t chat_id, int64_t message_id, bool is_photo,
                         const Json& np);

    // --- replies / IPC helpers ---
    // Replies and returns the new message id (-1 on failure); schedules
    // clean-mode deletion of the bot's message.
    int64_t reply(const CommandContext& ctx, const std::string& text);
    // HTML reply (Rich Message, like sendMessageHtml) + clean-mode
    // scheduling. All Phase 9 UI text goes through this; user-derived
    // text inside must already be HTML-escaped (ui_texts::htmlEscape).
    // On failure the TDLib error is reported to the owner (see
    // reportOwnerError) instead of being dropped silently.
    int64_t replyHtml(const CommandContext& ctx, const std::string& text);
    // HTML reply with an inline keyboard.
    int64_t replyKb(const CommandContext& ctx, const std::string& text,
                    const InlineKeyboard& kb);
    // HTML reply with a persistent reply keyboard (or remove the
    // keyboard when remove_kb is true).
    int64_t replyReplyKb(const CommandContext& ctx, const std::string& text,
                         const ReplyKeyboard& kb, bool remove_kb = false);
    // Photo reply (thumbnail rule §0.3): the card as the caption, HTML,
    // optional inline keyboard. Returns the message id, -1 on failure.
    int64_t replyPhoto(const CommandContext& ctx, const std::string& photo,
                       const std::string& caption, const InlineKeyboard& kb);
    // Answer a callback query: toast (alert=false) or alert dialog.
    bool answerCb(const TgCallbackQuery& cq, const std::string& text,
                  bool alert = false);
    // Edit the message a button sits on, in place. The card kind
    // (photo vs text) is read from the recorded card info so a photo
    // card is edited via editMessageCaption, a text card via
    // editMessageText. False when the edit fails (message gone, or
    // unchanged — "message is not modified" is a caller-level no-op).
    bool editCbCard(const TgCallbackQuery& cq, const std::string& text,
                    const InlineKeyboard& kb);
    // Edit only the keyboard of the message a button sits on.
    bool editCbKeyboard(const TgCallbackQuery& cq, const InlineKeyboard& kb);
    void replyError(const CommandContext& ctx, const IpcResult& r);
    void logToLogger(const std::string& text);
    // Report an engine/send failure to the owner: one Rich Message
    // DM to cfg_.owner_id (works from any chat, including groups)
    // with the action, chat id and the TDLib error, plus the same
    // line on stderr. A failure to notify the owner is logged only
    // and never triggers another notification (no spam loops).
    // No-op when no owner is configured (owner_id == 0).
    void reportOwnerError(const std::string& action, int64_t chat_id,
                          const std::string& error);
    void scheduleDelete(int64_t chat_id, int64_t message_id, int minutes);
    void deleteCommandMessage(const CommandContext& ctx);

    // --- Phase 9 card rendering (ui_texts.hpp templates) -----------------
    // One Track -> CardTrack (title truncated to 25 chars, escaped,
    // duration humanized, requester label, queue token).
    static ui_texts::CardTrack cardTrack(const Json& track,
                                         const std::string& requester);
    // The Music163bot audio caption (§8), shared by /download and the
    // /start spotify---- deep link.
    static std::string audioCaption(const Json* track,
                                    const std::string& file_path,
                                    const std::string& title,
                                    const std::string& artist,
                                    const std::string& bot_username);
    static ui_texts::CardTrack cardTrack(const Json& track, int64_t requested_by);
    // Per-chat settings snapshot (backend chat.settings, Q7) +
    // diffed write-back. False when a backend write failed.
    ui_texts::ChatSettings chatSettings(int64_t chat_id);
    bool saveChatSettings(int64_t chat_id, const ui_texts::ChatSettings& s);
    // Opus quality (quality.get surface, 2026-11-05): fetchQuality()
    // reads the effective config + per-field sources + library
    // readback over UDS; false when the backend is down or the
    // response is unusable. renderQualityScreen() edits the tapped
    // message to the quality sub-screen (or the load-failure screen
    // with a Back button); it answers nothing — the caller answers
    // the callback query exactly once first.
    bool fetchQuality(int64_t chat_id, ui_texts::QualityInfo& out);
    void renderQualityScreen(const TgCallbackQuery& cq);
    bool thumbnailsOn(int64_t chat_id) {
        return settingsFor(chat_id).thumbnails;
    }
    // Effective card photo for a chat (NoxBeats DEFAULT_THUMB gated
    // by THUMB_GEN, §0.3): the track thumbnail, else the configured
    // default thumbnail, else "" (text card). The thumbnails setting
    // gates the whole chain: thumbnails off always yields "".
    std::string effectiveThumb(int64_t chat_id,
                               const std::string& track_thumb) {
        if (!thumbnailsOn(chat_id)) return "";
        if (!track_thumb.empty()) return track_thumb;
        return cfg_.default_thumbnail;
    }
    // Display name for a user: "FirstName (@username)" / "FirstName" /
    // the bare id when nothing else is known.
    static std::string displayName(const TgUser& u);
    std::string userLabel(const CommandContext& ctx) const {
        return displayName(ctx.message.from);
    }
    // Send the now-playing card for a chat: photo with caption when the
    // track has a thumbnail and thumbnails are on for the chat, text
    // fallback otherwise (§0.3). state_line is prepended when non-empty.
    // The sent message id + kind are recorded for /player re-send and
    // for callback edits. Returns the message id, -1 on failure.
    int64_t sendNowPlayingCard(int64_t chat_id, const Json& np,
                               const std::string& state_line = "");
    // Record / read the last now-playing card message for a chat:
    // store "card:<chat>" = "photo|<id>" or "text|<id>".
    void recordCard(int64_t chat_id, int64_t message_id, bool is_photo);
    bool cardInfo(int64_t chat_id, int64_t& message_id, bool& is_photo);
    // Render the rich /queue card (Yukki §3.4, §5): now-playing + up to
    // 15 up-next tracks with per-track delete buttons, Show-more after
    // 15. show_all=true renders the whole tail (after pl:queue:more).
    void renderQueueCard(const CommandContext& ctx, bool show_all);
    // Build the /queue card content without sending it, so the
    // callback path can edit the card in place. False when there is
    // no active playback (or the queue fetch fails).
    bool buildQueueCard(int64_t chat_id, bool show_all,
                        std::string& text_out, InlineKeyboard& kb_out,
                        std::string& thumb_out);
    // Resolve a queue token to its queue item (vc.queue list) + the
    // 1-based position the engine's remove action wants. False when
    // the token is unknown/expired.
    bool resolveQueueToken(int64_t chat_id, const std::string& token,
                           Json& item_out, int64_t& position_out);
    // Search sessions (pl:search pick/page, §7): the core stores the
    // last search per user ("search:<user>" = query \n results JSON)
    // plus the results message id ("searchmsg:<user>"), so a tap or a
    // reply with a number can resolve the pick without a new search.
    void storeSearchSession(int64_t user_id, const std::string& query,
                            const Json& results, int64_t message_id);
    bool loadSearchSession(int64_t user_id, std::string& query_out,
                           Json& results_out);
    // Inline-query support (§13, Phase 9 gaps): the chat the user was
    // last active in ("last_chat:<user>") — an inline_query carries no
    // chat id, so np/s/l use the user's most recent group chat.
    // Recorded on group text/commands and on callback presses; only
    // written when the value changes (no disk write per message).
    void noteLastChat(int64_t user_id, int64_t chat_id);
    // The chat to pass to search.inline for np/s/l: the stored last
    // chat, or 0 when unknown (the engine then returns empty).
    int64_t inlineChatId(int64_t user_id) const;
    // Inline mode of a raw query (mirrors vc-ts resolvers.js
    // inlineMode): np|s|l|lib|del|help|search|hint. arg is the text
    // after "lib "/"del "/"search ", or the whole query for search.
    static std::string inlineModeOf(const std::string& query,
                                    std::string& arg_out);
    // One Track -> one InlineQueryResult object. Audio when the track
    // carries a direct stream_url, otherwise an article (a page URL
    // is not playable audio: YouTube/lemonic search results carry
    // url only, JioSaavn carries stream_url). with_button adds the
    // [Open in Parlay Bot] deep-link button (search results only).
    Json inlineTrackResult(const Json& track, const std::string& id,
                           bool with_button);
    // One static article result (help / keyword / no-results /
    // type-help). description/thumb_url empty = omitted.
    static Json inlineArticleResult(const std::string& id,
                                    const std::string& title,
                                    const std::string& description,
                                    const std::string& message_text,
                                    const std::string& thumb_url);
    // Remember the last inline search results per user
    // ("inlinesearch:<user>" = results JSON) so the /start
    // spotify----{id} deep link can resolve the track to a URL.
    void storeInlineResults(int64_t user_id, const Json& results);
    bool loadInlineTrack(int64_t user_id, const std::string& track_id,
                         Json& track_out);
    // /start spotify----{id} (§13 lemonic): download the track the
    // inline result named and send the audio file in this chat.
    void deepLinkDownload(const CommandContext& ctx,
                          const std::string& track_id);
    // Deliver one Spotify playlist/album pick (cbDl zip / 1by1, §8):
    // mode "zip" sends one ZIP, mode "files" sends each track.
    void deliverSpotifyCollection(const TgCallbackQuery& cq,
                                  const std::string& url,
                                  const std::string& mode);
    // Search page rendering (§7, shared by /search and the
    // pl:search:page callback): header + 10 result lines + footer,
    // and the number-button keyboard (5/row) + prev/next page row.
    static std::string searchResultsText(const std::string& query,
                                         const Json& results, int page);
    static InlineKeyboard searchResultsKeyboard(int64_t user_id,
                                                const std::string& query,
                                                const Json& results,
                                                int page);
    // Play the search result at 1-based index n from the stored session.
    // False + error when there is no session or the index is out of range.
    bool playSearchPick(const TgCallbackQuery& cq, int64_t user_id, int n,
                        std::string& error);
    // The command list for the unknown-command / unhandled-text replies
    // (§3, rustify rule), filtered by the caller's access level.
    std::string availableCommandsList(bool is_sudo, bool is_owner) const;
    // Split a callback_data string on ':' ("pl:player:pause:-1001" ->
    // ["pl","player","pause","-1001"]).
    static std::vector<std::string> splitCb(const std::string& data);
    // Player/queue button gate (§28.2): sudo, chat admin, auth user, or
    // admin_mode==2 (everyone). False -> the caller answers the
    // "You aren't allowed" toast.
    bool callbackCanControl(int64_t chat_id, int64_t user_id);
    // Callback sub-handlers (one per §1 domain).
    void cbHelp(const TgCallbackQuery& cq,
                const std::vector<std::string>& parts);
    void cbStart(const TgCallbackQuery& cq);
    void cbClose(const TgCallbackQuery& cq);
    void cbPlayer(const TgCallbackQuery& cq,
                  const std::vector<std::string>& parts);
    void cbQueue(const TgCallbackQuery& cq,
                 const std::vector<std::string>& parts);
    void cbSet(const TgCallbackQuery& cq,
               const std::vector<std::string>& parts);
    void cbLang(const TgCallbackQuery& cq,
                const std::vector<std::string>& parts);
    void cbDl(const TgCallbackQuery& cq,
              const std::vector<std::string>& parts);
    void cbBcast(const TgCallbackQuery& cq);
    void cbAuth(const TgCallbackQuery& cq,
                const std::vector<std::string>& parts);
    void cbSearch(const TgCallbackQuery& cq,
                  const std::vector<std::string>& parts);
    void cbVote(const TgCallbackQuery& cq,
                const std::vector<std::string>& parts);
    void cbShare(const TgCallbackQuery& cq,
                 const std::vector<std::string>& parts);
    void cbDel(const TgCallbackQuery& cq,
               const std::vector<std::string>& parts);
    void cbLossy(const TgCallbackQuery& cq,
                 const std::vector<std::string>& parts);
    void cbRestart(const TgCallbackQuery& cq,
                   const std::vector<std::string>& parts);
    void cbDlset(const TgCallbackQuery& cq,
                 const std::vector<std::string>& parts);
    void cbSpotify(const TgCallbackQuery& cq,
                  const std::vector<std::string>& parts);
    // Run one player action for a chat (shared by /commands and
    // callbacks): pause/resume/replay/skip/stop/mute/unmute/seek±15/
    // autoplay/shuffle/refresh/voldec/volinc. Returns false + error
    // when nothing is playing or the backend refuses; the caller
    // answers the toast. On success the card is re-rendered in place
    // when edit_msg is set (callback path).
    bool playerAction(int64_t chat_id, const std::string& verb,
                      int64_t user_id, const TgCallbackQuery* cq,
                      std::string& error);
    // Shared vc.volume step (the /menu Vol-/Vol+ panel buttons and
    // the /volume text command): clamps the target to 0-200, posts
    // vc.volume, records the applied value in the runtime hash, and
    // returns the success toast in toast_out. False + the backend's
    // error message in toast_out when the backend refuses.
    bool applyVolume(const CommandContext& ctx, int64_t chat_id,
                     int target, std::string& toast_out);
    // Re-render the now-playing card in place on the callback's
    // message (after a player action). No-op when there is no card.
    void refreshCard(const TgCallbackQuery& cq, int64_t chat_id);
    // Re-render the /menu control panel in place on the callback's
    // message (remix §2.3): now/next lines + the panel
    // keyboard. No-op when there is no current track.
    void refreshPanel(const TgCallbackQuery& cq, int64_t chat_id);

    // Shared IPC helpers: one POST / one GET to the single unified
    // backend over the persistent UDS socket (no fallback, no
    // pre-probes). vcCall posts the dotted contract action with a
    // 30 s budget; vcGet translates "/v1/<action>?chat_id=..." with
    // a 10 s budget.
    IpcResult vcCall(const CommandContext& ctx, const std::string& action,
                     const Json& body);
    IpcResult vcGet(const CommandContext& ctx, const std::string& action);
    // Current track for the chat's session (vc.now_playing), if any.
    bool nowPlaying(int64_t chat_id, Json& np_out, Json& track_out);
    // Mark/unmark the chat as having an active call (for /ac).
    void setCallActive(int64_t chat_id, bool active);
    bool callActive(int64_t chat_id) const;

    // Formatting for contract types (contracts/ipc.json: Track, NowPlaying).
    static std::string fmtTrack(const Json& track);
    static std::string fmtQueue(const Json& data);
    static std::string fmtSeconds(int64_t sec);
    static std::vector<std::string> splitArgs(const std::string& args);
    static std::string lowerStr(std::string s);
    // Extract a target user id: reply, @username (via getChat), or digits.
    std::optional<int64_t> extractUserId(const CommandContext& ctx);

    // --- background work ---
    void backgroundLoop();
    void sweepClean();

    // Handlers: playback / queue.
    void onStart(const CommandContext& ctx);
    void onHelp(const CommandContext& ctx);
    void onPlay(const CommandContext& ctx);
    void onMix(const CommandContext& ctx);
    void onVolume(const CommandContext& ctx);
    void onRecognize(const CommandContext& ctx);
    void onPause(const CommandContext& ctx);
    void onResume(const CommandContext& ctx);
    void onSkip(const CommandContext& ctx);
    void onAutoplaySkip(const CommandContext& ctx);
    void onQueue(const CommandContext& ctx);
    void onStop(const CommandContext& ctx);
    void onLoop(const CommandContext& ctx);
    void onSeek(const CommandContext& ctx);
    void onClear(const CommandContext& ctx);
    void onRestore(const CommandContext& ctx);
    void onRemove(const CommandContext& ctx);
    void onMove(const CommandContext& ctx);
    void onShuffle(const CommandContext& ctx);
    void onPos(const CommandContext& ctx);
    void onJump(const CommandContext& ctx);
    void onSpeed(const CommandContext& ctx);
    void onBass(const CommandContext& ctx);
    void onMute(const CommandContext& ctx);
    void onJoin(const CommandContext& ctx);
    // Play a replied-to (or captioned) Telegram file: download via
    // TDLib, then hand it to the backend with vc.play_file (§3.3).
    // The backend owns playback; the core never claims a play it
    // did not get back as now_playing. Returns false when the
    // download or the backend call fails (and the failure is
    // already reported); true when the file is playing or queued.
    bool playRepliedFile(const CommandContext& ctx,
                         const std::string& file_id,
                         const std::string& title, const std::string& kind);
    // Shared post-play rendering for onPlay and playRepliedFile (the
    // vc.play / vc.play_file response is the same shape): duration-
    // limit stop, call-active marking, last-track bookkeeping, logger
    // line, and the queued vs now-playing card. Replies exactly once.
    // `r` must be ok (the caller reports the backend error itself).
    void finishPlay(const CommandContext& ctx, int64_t chat,
                    const IpcResult& r, const std::string& query,
                    bool was_active, const std::string& old_id);
    // vc.play request body (§1.1 + §4.4): the contract fields plus
    // the chat's stored speed/bass, so the backend room carries them
    // on every play (record-only until the Phase 5 DSP).
    Json vcPlayBody(int64_t chat, const std::string& query,
                    int64_t requested_by);
    void onReplay(const CommandContext& ctx);
    void onPlayer(const CommandContext& ctx);
    void onLyrics(const CommandContext& ctx);
    // Handlers: search / download / radio / share / engine.
    void onSearch(const CommandContext& ctx);
    void onDownload(const CommandContext& ctx);
    void onRadio(const CommandContext& ctx);
    // Radio lifecycle (§21): /new starts the chat's radio from a
    // station id, /destroy stops it, /manage shows its status card.
    // radio-js tracks listeners/queue/loop only in memory and does
    // not expose them over /v1/radio/status, so /manage shows "—"
    // for those fields.
    void onNewRadio(const CommandContext& ctx);
    void onDestroyRadio(const CommandContext& ctx);
    void onManageRadio(const CommandContext& ctx);
    void onShare(const CommandContext& ctx);
    void onEngine(const CommandContext& ctx);
    // Handlers: playlists.
    void onAddPlaylist(const CommandContext& ctx);
    void onPlaylist(const CommandContext& ctx);
    void onRmPlaylist(const CommandContext& ctx);
    void onPlayPlaylist(const CommandContext& ctx);
    // Handlers: settings / chat settings / autoplay.
    void onPlaymode(const CommandContext& ctx);
    void onSettings(const CommandContext& ctx);
    void onQuality(const CommandContext& ctx);
    void onLang(const CommandContext& ctx);
    void onAdminmode(const CommandContext& ctx);
    void onCmdDelete(const CommandContext& ctx);
    void onCleanmode(const CommandContext& ctx);
    void onThumb(const CommandContext& ctx);
    void onAutoplay(const CommandContext& ctx);
    void onChannelplay(const CommandContext& ctx);
    void onUserbot(const CommandContext& ctx);
    void onDownloadSettings(const CommandContext& ctx);
    // Handlers: named playlists (Parvati/TgMusicBot model, §17).
    void onCreatePlaylist(const CommandContext& ctx);
    void onDeletePlaylist(const CommandContext& ctx);
    void onAddToPlaylist(const CommandContext& ctx);
    void onRemoveFromPlaylist(const CommandContext& ctx);
    void onPlaylistInfo(const CommandContext& ctx);
    void onMyPlaylists(const CommandContext& ctx);
    // Handlers: new Phase 9 commands.
    void onMenu(const CommandContext& ctx);
    void onId(const CommandContext& ctx);
    void onKeyboard(const CommandContext& ctx);
    void onAbout(const CommandContext& ctx);
    void onStatus(const CommandContext& ctx);
    void onNow(const CommandContext& ctx);
    void onRepo(const CommandContext& ctx);
    // Handlers: admin / sudo / broadcast / tools.
    void onAuth(const CommandContext& ctx);
    void onAuthlist(const CommandContext& ctx);
    void onAdmincache(const CommandContext& ctx);
    void onRmcache(const CommandContext& ctx);
    void onScan(const CommandContext& ctx);
    void onShareScan(const CommandContext& ctx);
    void onBlacklist(const CommandContext& ctx);
    void onBlacklisted(const CommandContext& ctx);
    void onAddSudo(const CommandContext& ctx);
    void onListSudo(const CommandContext& ctx);
    void onActiveVc(const CommandContext& ctx);
    void onBroadcast(const CommandContext& ctx);
    void onMaintenance(const CommandContext& ctx);
    void onAutoleave(const CommandContext& ctx);
    void onJson(const CommandContext& ctx);
    void onSpeedtest(const CommandContext& ctx);
    void onBug(const CommandContext& ctx);
    void onPrivacy(const CommandContext& ctx);
    void onPing(const CommandContext& ctx);
    void onStats(const CommandContext& ctx);
    void onEval(const CommandContext& ctx);
    void onLogs(const CommandContext& ctx);
    void onLogger(const CommandContext& ctx);
    void onRestart(const CommandContext& ctx);
    // Handlers: RTMP.
    void onSetRtmp(const CommandContext& ctx);
    void onStream(const CommandContext& ctx);
    void onStreamStop(const CommandContext& ctx);
    void onStreamStatus(const CommandContext& ctx);

    const Config& cfg_;
    TelegramSender& tg_;
    IpcClient& ipc_;
    Store& store_;
    TdLibClient& assistant_;
    RateLimiter& limiter_;
    std::string bot_username_;
    int64_t bot_id_ = 0;
    // Reentrancy guard for reportOwnerError: while an owner
    // notification is in flight, a nested failure only logs.
    bool notifying_owner_ = false;
    std::chrono::steady_clock::time_point boot_time_ =
        std::chrono::steady_clock::now();

    std::vector<Route> routes_;
    std::map<std::string, std::string> aliases_;
    // s-aliases (TgMusicBot SupportsSilence): the alias names whose
    // invocations set CommandContext::silent_success.
    std::set<std::string> silent_aliases_;
    std::map<std::string, std::pair<std::string, bool>> channel_cmds_;

    // Settings cache (Q7): one snapshot per chat, fetched from the
    // backend over UDS. live=true means the snapshot came from the
    // backend (fetch or write-through); live=false means locked
    // defaults (or a stale snapshot) served while the backend is
    // down. Live entries are fresh for 60 s; non-live entries for
    // 5 s, so a down backend is re-probed promptly without spamming
    // a connect per reply.
    struct SettingsCacheEntry {
        ui_texts::ChatSettings settings;
        std::chrono::steady_clock::time_point fetched_at;
        bool live = false;
    };
    std::mutex settings_mu_;
    std::map<int64_t, SettingsCacheEntry> settings_cache_;

    // Clean-mode deletion queue: (chat, message, delete_at).
    struct PendingDelete {
        int64_t chat;
        int64_t message;
        std::chrono::steady_clock::time_point at;
    };
    std::mutex clean_mu_;
    std::vector<PendingDelete> clean_queue_;

    // RTMP streams: chat -> child pid + log path.
    struct RtmpStream {
        pid_t pid = 0;
        std::string log_path;
        std::chrono::steady_clock::time_point started;
        std::string what;  // query/title being streamed (status card)
    };
    std::mutex rtmp_mu_;
    std::map<int64_t, RtmpStream> rtmp_streams_;

    // Broadcast state (§18): onBroadcast runs synchronously, so the
    // cancel button works through these flags — pl:bcast:cancel sets
    // bcast_cancel_ and the fan-out loop stops at the next target.
    std::atomic<bool> bcast_running_{false};
    std::atomic<bool> bcast_cancel_{false};

    // Recent command log for /logs (last 100 lines, in-memory).
    std::mutex log_mu_;
    std::vector<std::string> recent_log_;

    // Main-thread work queue (see the file header note): outbound
    // Telegram work queued from the background sweeper and backend
    // events, drained by processPendingWork() on the main thread.
    std::mutex work_mu_;
    std::vector<std::function<void()>> pending_work_;
    // Pending duplex JoinRequests, drained by processPendingWork().
    struct PendingJoin {
        int64_t chat_id = 0;
        std::string local_sdp;
        std::shared_ptr<std::promise<Json>> done;
    };
    std::mutex joins_mu_;
    std::vector<PendingJoin> pending_joins_;

    std::atomic<bool> bg_stop_{false};
    std::thread bg_thread_;
};

}  // namespace parlay
