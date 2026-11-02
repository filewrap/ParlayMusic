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
#include "voice_call.hpp"

namespace parlay {

struct CommandContext {
    TgMessage message;
    std::string command;  // lowercased, without leading '/' or @bot suffix
    std::string args;     // raw argument text after the command
    bool via_channel = false;  // c-command: run against the linked channel
};

class Dispatcher {
public:
    Dispatcher(const Config& cfg, TelegramApi& tg, IpcClient& ipc,
               EngineRouter& router, Store& store, VoiceCall& voice,
               const std::string& bot_username);

    // Handle one incoming message. Non-commands and denied commands are
    // answered (or ignored) here, never by the caller.
    void handleMessage(const TgMessage& msg);

    // Background loops: clean-mode sweeper + loop/autoplay watcher.
    void startBackground();
    void stopBackground();
    void setBotId(int64_t id) { bot_id_ = id; }

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
    void replyError(const CommandContext& ctx, const IpcResult& r);
    void logToLogger(const std::string& text);
    void scheduleDelete(int64_t chat_id, int64_t message_id, int minutes);
    void deleteCommandMessage(const CommandContext& ctx);

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
    void onReplay(const CommandContext& ctx);
    void onPlayer(const CommandContext& ctx);
    void onLyrics(const CommandContext& ctx);
    // Handlers: search / download / radio / share / engine.
    void onSearch(const CommandContext& ctx);
    void onDownload(const CommandContext& ctx);
    void onRadio(const CommandContext& ctx);
    void onShare(const CommandContext& ctx);
    void onEngine(const CommandContext& ctx);
    // Handlers: playlists.
    void onAddPlaylist(const CommandContext& ctx);
    void onPlaylist(const CommandContext& ctx);
    void onRmPlaylist(const CommandContext& ctx);
    void onPlayPlaylist(const CommandContext& ctx);
    // Handlers: settings / chat settings / autoplay.
    void onPlaymode(const CommandContext& ctx);
    void onLang(const CommandContext& ctx);
    void onAdminmode(const CommandContext& ctx);
    void onCmdDelete(const CommandContext& ctx);
    void onCleanmode(const CommandContext& ctx);
    void onThumb(const CommandContext& ctx);
    void onAutoplay(const CommandContext& ctx);
    void onChannelplay(const CommandContext& ctx);
    void onUserbot(const CommandContext& ctx);
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
    };
    std::mutex rtmp_mu_;
    std::map<int64_t, RtmpStream> rtmp_streams_;

    // Recent command log for /logs (last 100 lines, in-memory).
    std::mutex log_mu_;
    std::vector<std::string> recent_log_;

    std::atomic<bool> bg_stop_{false};
    std::thread bg_thread_;
};

}  // namespace parlay
