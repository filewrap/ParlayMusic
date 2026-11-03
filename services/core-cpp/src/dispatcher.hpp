// dispatcher.hpp — command dispatcher.
//
// Port shape: anonx-cpp's CommandDispatcher (parse_command_and_args,
// dispatch table, per-chat rate limiter) + NoxBeats' full command surface
// (Bad/cmd/*.cpp, Bad/cmd/common.cpp): a route table with Scope + Perm,
// the alias map, the c-command map, the owner/sudo/auth/admin-mode model,
// clean mode / cmddelete, maintenance + blacklist gates, and the
// background loops (track watcher for loop/autoplay, clean-mode sweeper).
// Every handler validates permissions, emits real IPC calls (vc-*,
// downloaders, netease-go, radio-js, share-rust) or a real store action,
// and formats the result. No handler replies with a stub.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "engine_router.hpp"
#include "ipc.hpp"
#include "store.hpp"
#include "telegram_api.hpp"
#include "ui_texts.hpp"
#include "voice_call.hpp"

namespace parlay {

struct CommandContext {
    TgMessage message;
    std::string command;  // lowercased, without leading '/' or @bot suffix
    std::string args;     // raw argument text after the command
    bool via_channel = false;  // c-command: run against the linked channel
    // Phase 1 telemetry: steady-clock microseconds when
    // Dispatcher::handleMessage received this command (0 = unknown,
    // e.g. a context built outside handleMessage). t_dispatch_to_ipc
    // measures from here to the engine IPC post.
    int64_t receipt_us = 0;
};

class Dispatcher {
public:
    Dispatcher(const Config& cfg, TelegramApi& tg, IpcClient& ipc,
               EngineRouter& router, Store& store, VoiceCall& voice,
               const std::string& bot_username);

    // Handle one incoming message. Non-commands and denied commands are
    // answered (or ignored) here, never by the caller.
    void handleMessage(const TgMessage& msg);

    // Handle one callback_query update (Phase 9, UI_INVENTORY.md §1):
    // the "pl:<domain>:<verb>[:<arg>…]" namespace only. Unknown
    // callbacks get a polite alert, never a crash. Wrong-user taps get
    // the "You aren't allowed" toast (admin-or-requester gate per row),
    // except the remix /menu panel rule where other users are silently
    // ignored.
    void handleCallback(const TgCallbackQuery& cq);
    // Handle one my_chat_member update (bot status change): the
    // demotion goodbye (§2) — the bot sends the notice and leaves when
    // it loses admin rights in a group.
    void handleMyChatMember(const TgChatMemberUpdate& upd);
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

    // Background loops: clean-mode sweeper + loop/autoplay watcher.
    void startBackground();
    void stopBackground();
    void setBotId(int64_t id) { bot_id_ = id; }
    void setBotUsername(const std::string& name) { bot_username_ = name; }

    // HTTP API seam (core_api.hpp VcStateProvider): live engine state
    // for one chat, for GET /api/now-playing and GET /api/queue.
    // apiNowPlaying returns the whole now-playing object; false when
    // the engines report no state (or none is reachable).
    bool apiNowPlaying(int64_t chat_id, Json& np_out, std::string& error);
    // The engine's queue list (POST /v1/queue action=list).
    bool apiQueueList(int64_t chat_id, Json& queue_out, std::string& error);
    // vc.join (POST /v1/join): join the chat's voice call on the
    // core's TDLib backend. True only after a real join (or when the
    // chat is already in a call); false + error when the backend
    // cannot join. Called by the vc-ts / vc-rust engines, which hold
    // no MTProto client of their own.
    bool joinVoice(int64_t chat_id, std::string& error);
    // vc.mute (POST /v1/mute): mute (mute=true) or unmute the chat's
    // voice call on the core's TDLib backend. Same mute as /mute,
    // minus the Telegram permission check and reply. False + error
    // on a session the core does not hold (a stale/pre-fix
    // engine-owned session — since Phase 7 A1b, engine plays are
    // core-attached, so /mute reaches them through the backend), when
    // nothing is playing, or when the backend mute/unmute fails. True
    // only after a real backend mute/unmute.
    bool muteVoice(int64_t chat_id, bool mute, std::string& error);

    // One core-stream follow pass (background-loop step, public so
    // tests can drive it synchronously): when a vc engine advances
    // its own state machine to a new track, re-attach the new
    // track's audio to the core voice backend. See the .cpp for the
    // full note. No reply channel — failures are logged to stderr
    // and the session is stopped cleanly.
    void watchCoreFollow();

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
    bool rateLimited(int64_t chat_id);
    bool maintenanceOn() const;

    // --- chat settings (NoxBeats DB per-chat fields, stored in Store) ---
    std::string chatField(int64_t chat_id, const std::string& field,
                          const std::string& def = "");
    void setChatField(int64_t chat_id, const std::string& field,
                      const std::string& value);
    int chatIntField(int64_t chat_id, const std::string& field, int def = 0);
    // Stored playback speed (0.5-2.0) for the chat, 1.0 when unset or
    // unparseable. Used by /speed reporting and by the core voice
    // backend, which is the only playback path that applies it.
    double chatSpeedField(int64_t chat_id);
    // Core-voice filter path for /speed and /bass: when the chat's
    // audio is on the core voice backend (voice_owner == "core"), the
    // stored speed/bass ARE applied — the source is re-attached at the
    // current position with the new filter values (buildFfmpegCmd
    // atempo/bass). Since Phase 7 A1b that covers engine sessions too:
    // attachEngineAudio records engine-resolved tracks as core-owned,
    // so /speed + /bass apply to them as well. Returns true when it
    // handled the command (core session, including the nothing-playing
    // / no-source honest replies); false when the session belongs to
    // a vc engine that was never core-attached (stale / pre-fix
    // state), and the caller falls through to the engine seek-restart,
    // which moves the position but cannot apply the filter.
    bool applyCoreFilter(const CommandContext& ctx, double speed, int bass,
                         const std::string& done_msg);
    // Core-stream attach (Phase 7, A1b): the core streams
    // engine-resolved audio through its own ntgcalls session — the
    // only session that can actually be in the call. The engine's
    // "play" is state only (its ntgcalls instance never joins: no
    // MTProto client in-process), so after a vc.play succeeds the
    // dispatcher attaches the resolved track here, or the call stays
    // silent behind a "Now playing" lie.
    //
    // The playable source for an engine-resolved track: the top-level
    // now_playing.stream_url (A8) first, then track.stream_url. A
    // caller-known URL (the /radio stream URL) may be passed as a
    // fallback. Empty when the engine gave the core nothing to play:
    // no local file path is usable here — the engines deliberately
    // strip file_path from IPC (engine-local; vc-rust's leak check),
    // and a path on an engine host is not a path on the core host
    // under compose.
    static std::string engineTrackSource(const Json& np,
                                         const std::string& fallback = "");
    // Attach an engine-resolved track to the core voice backend:
    // voice_.play(chat, source, title, video, seek, speed, bass) with
    // the chat's stored speed/bass. On success records voice_owner =
    // "core" + core_source/core_title/core_video/core_track_id/
    // core_pos + core_follow=1 + call active, and returns true. On
    // failure returns false + error (no source, or the backend
    // refused); the caller stops the engine playback and reports the
    // error honestly — nothing is marked playing.
    bool attachEngineAudio(int64_t chat, const Json& np, bool video,
                           int seek_sec, const std::string& fallback_source,
                           std::string& error);
    // Re-attach the recorded core source at a new position (seek /
    // jump / replay follow-up): voice_.play with the stored source,
    // title, video flag and the chat's stored speed/bass. False +
    // error when nothing is recorded or the backend refuses.
    bool reattachCoreAudio(int64_t chat, int seek_sec, std::string& error);
    // Clear every core-stream field for a chat (stop / call end /
    // attach failure): voice_owner, core_source, core_title,
    // core_video, core_track_id, core_pos, core_follow.
    void clearCoreStream(int64_t chat);
    // True when the core voice backend is the one streaming this chat:
    // voice_owner "core" + call active + a recorded core_source.
    // Covers replied-file sessions and engine sessions attached via
    // attachEngineAudio alike.
    bool coreStreaming(int64_t chat);
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

    // --- replies / IPC helpers ---
    // Replies and returns the new message id (-1 on failure); schedules
    // clean-mode deletion of the bot's message.
    int64_t reply(const CommandContext& ctx, const std::string& text);
    // HTML reply (parse_mode=HTML, web preview off) + clean-mode
    // scheduling. All Phase 9 UI text goes through this; user-derived
    // text inside must already be HTML-escaped (ui_texts::htmlEscape).
    int64_t replyHtml(const CommandContext& ctx, const std::string& text);
    // HTML reply with an inline keyboard.
    int64_t replyKb(const CommandContext& ctx, const std::string& text,
                    const InlineKeyboard& kb);
    // HTML reply with a persistent reply keyboard (or the remove-
    // keyboard markup). Pass removeKeyboardJson() to clear one.
    int64_t replyReplyKb(const CommandContext& ctx, const std::string& text,
                         const Json& reply_markup);
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
    // Per-chat settings snapshot (Store chat:<id> fields) + write-back.
    ui_texts::ChatSettings chatSettings(int64_t chat_id);
    void saveChatSettings(int64_t chat_id, const ui_texts::ChatSettings& s);
    bool thumbnailsOn(int64_t chat_id) {
        return chatField(chat_id, "thumb", "on") == "on";
    }
    // Display name for a user: "FirstName (@username)" / "FirstName" /
    // the bare id when nothing else is known.
    static std::string displayName(const TgUser& u);
    std::string userLabel(const CommandContext& ctx) const {
        return displayName(ctx.message.from);
    }
    // Send the now-playing card for a chat: photo with caption when the
    // track has a thumbnail and thumbnails are on for the chat, text
    // fallback otherwise (§0.3). state_line is prepended when non-empty
    // ("▶ Now playing", "⏭ Skipped — now playing", ...). The sent
    // message id + kind are recorded for /player re-send and for
    // callback edits. Returns the message id, -1 on failure.
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
    // when nothing is playing or the engine refuses; the caller
    // answers the toast. On success the card is re-rendered in place
    // when edit_msg is set (callback path).
    bool playerAction(int64_t chat_id, const std::string& verb,
                      int64_t user_id, const TgCallbackQuery* cq,
                      std::string& error);
    // Re-render the now-playing card in place on the callback's
    // message (after a player action). No-op when there is no card.
    void refreshCard(const TgCallbackQuery& cq, int64_t chat_id);
    // Re-render the /menu control panel in place on the callback's
    // message (remix §2.3): now/next lines + the emoji panel
    // keyboard. No-op when there is no current track.
    void refreshPanel(const TgCallbackQuery& cq, int64_t chat_id);

    // Shared IPC helpers.
    IpcResult vcCall(const CommandContext& ctx, const std::string& action,
                     const Json& body, Engine& used, bool& fell_back);
    IpcResult vcGet(const CommandContext& ctx, const std::string& action,
                    Engine& used, bool& fell_back);
    // Current track for the chat's session (vc.now_playing), if any.
    bool nowPlaying(int64_t chat_id, Json& np_out, Json& track_out);
    // Mark/unmark the chat as having an active call (for /ac).
    void setCallActive(int64_t chat_id, bool active);
    bool callActive(int64_t chat_id) const;

    // Formatting for contract types (contracts/ipc.json: Track, NowPlaying).
    static std::string fmtTrack(const Json& track);
    static std::string fmtNowPlaying(const Json& np, const std::string& verb);
    static std::string fmtQueue(const Json& data);
    static std::string fmtSeconds(int64_t sec);
    static std::vector<std::string> splitArgs(const std::string& args);
    static std::string lowerStr(std::string s);
    // Extract a target user id: reply, @username (via getChat), or digits.
    std::optional<int64_t> extractUserId(const CommandContext& ctx);

    // --- background work ---
    void backgroundLoop();
    void sweepClean();
    void watchLoopAutoplay();

    // Handlers: playback / queue.
    void onStart(const CommandContext& ctx);
    void onHelp(const CommandContext& ctx);
    void onPlay(const CommandContext& ctx);
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
    // getFile, then play through the core's in-process voice backend.
    // Returns true only after the voice backend accepted the file.
    bool playRepliedFile(const CommandContext& ctx,
                         const std::string& file_id,
                         const std::string& title, const std::string& kind);
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
    TelegramApi& tg_;
    IpcClient& ipc_;
    EngineRouter& router_;
    Store& store_;
    VoiceCall& voice_;
    std::string bot_username_;
    int64_t bot_id_ = 0;
    std::chrono::steady_clock::time_point boot_time_ =
        std::chrono::steady_clock::now();

    std::vector<Route> routes_;
    std::map<std::string, std::string> aliases_;
    std::map<std::string, std::pair<std::string, bool>> channel_cmds_;
    std::map<int64_t, std::chrono::steady_clock::time_point> last_cmd_;

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

    std::atomic<bool> bg_stop_{false};
    std::thread bg_thread_;
};

}  // namespace parlay
