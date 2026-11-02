// dispatcher.cpp — part 1: routing, permissions, helpers, background loops.
#include "dispatcher.hpp"

#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "http.hpp"

namespace parlay {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string fmtDuration(int64_t sec) {
    if (sec <= 0) return "";
    return std::to_string(sec / 60) + ":" +
           (sec % 60 < 10 ? "0" : "") + std::to_string(sec % 60);
}

// "90", "1:30", "1:02:03" -> seconds, -1 when not a time (NoxBeats
// queue_ops.cpp::parse_time).
int parseTime(const std::string& s) {
    if (s.empty() || s.size() > 9) return -1;
    int total = 0, cur = 0, digits = 0;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (ch - '0');
            ++digits;
        } else if (ch == ':' && digits) {
            total = total * 60 + cur;
            cur = 0;
            digits = 0;
        } else {
            return -1;
        }
    }
    return digits ? total * 60 + cur : -1;
}

bool isDigits(const std::string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

bool flagOn(const std::string& s) {
    std::string a = s;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return a == "on" || a == "enable" || a == "enabled" || a == "true" ||
           a == "yes" || a == "1";
}
bool flagOff(const std::string& s) {
    std::string a = s;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return a == "off" || a == "disable" || a == "disabled" || a == "false" ||
           a == "no" || a == "0";
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string runShell(const std::string& cmd, int timeout_sec, int& code_out) {
    // Bounded shell run (NoxBeats util::exec shape): timeout + capture.
    std::string full = "timeout " + std::to_string(timeout_sec) + " sh -c " +
                       shellQuote(cmd) + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    if (!p) {
        code_out = -1;
        return "";
    }
    std::string out;
    std::array<char, 4096> buf{};
    while (size_t n = fread(buf.data(), 1, buf.size(), p))
        out.append(buf.data(), n);
    int rc = pclose(p);
    code_out = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return out;
}

}  // namespace

std::string Dispatcher::lowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> Dispatcher::splitArgs(const std::string& args) {
    std::vector<std::string> out;
    std::istringstream ss(args);
    std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}

Dispatcher::Dispatcher(const Config& cfg, TelegramApi& tg, IpcClient& ipc,
                       EngineRouter& router, Store& store, VoiceCall& voice,
                       const std::string& bot_username)
    : cfg_(cfg),
      tg_(tg),
      ipc_(ipc),
      router_(router),
      store_(store),
      voice_(voice),
      bot_username_(bot_username) {
    // Seed env sudoers into the store (NoxBeats load_runtime_sets()).
    for (int64_t id : cfg_.sudo_ids)
        store_.sadd("sudoers", std::to_string(id));
    registerCommands();
}

void Dispatcher::registerCommands() {
    auto add = [&](std::vector<std::string> cmds, Scope scope, Perm perm,
                   Handler fn) {
        routes_.push_back({std::move(cmds), scope, perm, std::move(fn)});
    };
    // Playback / queue (NoxBeats cmd/play.cpp, playback.cpp, queue_ops.cpp).
    add({"play", "playforce", "vplay", "vplayforce"}, Scope::Group,
        Perm::Everyone, [this](const CommandContext& c) { onPlay(c); });
    add({"playplaylist"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPlayPlaylist(c); });
    add({"skip", "next"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onSkip(c); });
    add({"autoplay-skip", "askip"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAutoplaySkip(c); });
    add({"pause"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPause(c); });
    add({"resume"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onResume(c); });
    add({"end", "stop"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onStop(c); });
    add({"loop"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onLoop(c); });
    add({"seek", "seekback"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onSeek(c); });
    add({"queue", "playing"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onQueue(c); });
    add({"clear"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onClear(c); });
    add({"restore"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onRestore(c); });
    add({"remove"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onRemove(c); });
    add({"move"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onMove(c); });
    add({"shuffle"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onShuffle(c); });
    add({"pos"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPos(c); });
    add({"jump"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onJump(c); });
    add({"speed"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onSpeed(c); });
    add({"bass"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onBass(c); });
    add({"mute", "unmute"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onMute(c); });
    add({"replay"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onReplay(c); });
    add({"player"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPlayer(c); });
    add({"lyrics", "lyric"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onLyrics(c); });
    // Search / download / radio / share / engine.
    add({"search"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onSearch(c); });
    add({"download"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onDownload(c); });
    add({"radio"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onRadio(c); });
    add({"share"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onShare(c); });
    add({"engine"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onEngine(c); });
    // Playlists.
    add({"addplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onAddPlaylist(c); });
    add({"playlist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onPlaylist(c); });
    add({"rmplaylist", "clearplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onRmPlaylist(c); });
    // Settings / chat settings / autoplay / channel play.
    add({"start"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onStart(c); });
    add({"help"}, Scope::Private, Perm::Everyone,
        [this](const CommandContext& c) { onHelp(c); });
    add({"playmode", "settings"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPlaymode(c); });
    add({"lang", "language"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onLang(c); });
    add({"adminmode"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAdminmode(c); });
    add({"cmddelete"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onCmdDelete(c); });
    add({"cleanmode"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onCleanmode(c); });
    add({"nothumb"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onThumb(c); });
    add({"thumbnail", "thumb"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onThumb(c); });
    add({"autoplay"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAutoplay(c); });
    add({"channelplay"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onChannelplay(c); });
    add({"userbot"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onUserbot(c); });
    add({"join"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onJoin(c); });
    // Admin / sudo / broadcast / tools.
    add({"auth", "unauth"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAuth(c); });
    add({"authlist"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAuthlist(c); });
    add({"admincache", "reload"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onAdmincache(c); });
    add({"blacklist", "unblacklist", "whitelist"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onBlacklist(c); });
    add({"blacklisted"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onBlacklisted(c); });
    add({"addsudo", "delsudo", "rmsudo"}, Scope::Any, Perm::Owner,
        [this](const CommandContext& c) { onAddSudo(c); });
    add({"listsudo", "sudolist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onListSudo(c); });
    add({"ac", "activevc"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onActiveVc(c); });
    add({"broadcast"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onBroadcast(c); });
    add({"maintenance"}, Scope::Any, Perm::Owner,
        [this](const CommandContext& c) { onMaintenance(c); });
    add({"autoleave"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onAutoleave(c); });
    add({"json"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onJson(c); });
    add({"speedtest"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onSpeedtest(c); });
    add({"bug"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onBug(c); });
    add({"privacy"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onPrivacy(c); });
    add({"alive", "ping"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onPing(c); });
    add({"stats"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onStats(c); });
    add({"eval", "ev", "sh", "exec"}, Scope::Any, Perm::Owner,
        [this](const CommandContext& c) { onEval(c); });
    add({"logs"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onLogs(c); });
    add({"logger"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onLogger(c); });
    add({"restart"}, Scope::Any, Perm::Sudo,
        [this](const CommandContext& c) { onRestart(c); });
    // RTMP.
    add({"setrtmp"}, Scope::Private, Perm::Everyone,
        [this](const CommandContext& c) { onSetRtmp(c); });
    add({"stream"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onStream(c); });
    add({"streamstop"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onStreamStop(c); });
    add({"streamstatus"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onStreamStatus(c); });

    // Aliases (NoxBeats cmd/common.cpp::aliases()).
    aliases_ = {
        {"p", "play"}, {"fplay", "playforce"}, {"forceplay", "playforce"},
        {"fvplay", "vplayforce"}, {"vfplay", "vplayforce"},
        {"q", "queue"}, {"aplay", "autoplay"}, {"setloop", "loop"},
        {"setspeed", "speed"}, {"speedup", "speed"},
        {"position", "pos"}, {"addauth", "auth"}, {"delauth", "unauth"},
        {"removeauth", "unauth"}, {"sudoers", "sudolist"},
        {"listsudo", "sudolist"}, {"sudoadd", "addsudo"},
        {"addsudoer", "addsudo"}, {"delsudoer", "delsudo"},
        {"sudodel", "delsudo"}, {"remsudo", "delsudo"},
        {"sudorem", "delsudo"}, {"dropsudo", "delsudo"},
        {"unsudo", "delsudo"}, {"blockuser", "blacklist"},
        {"blacklistuser", "blacklist"}, {"blackuser", "blacklist"},
        {"bluser", "blacklist"}, {"blockchat", "blacklist"},
        {"blacklistchat", "blacklist"}, {"blackchat", "blacklist"},
        {"blchat", "blacklist"}, {"unblockuser", "unblacklist"},
        {"unblacklistuser", "unblacklist"}, {"unbluser", "unblacklist"},
        {"whitelistuser", "unblacklist"}, {"unblockchat", "unblacklist"},
        {"unblacklistchat", "unblacklist"}, {"unblackchat", "unblacklist"},
        {"whitechat", "unblacklist"}, {"unblchat", "unblacklist"},
        {"blocked", "blacklisted"}, {"gcast", "broadcast"},
        {"bcast", "broadcast"}, {"active", "ac"}, {"activevoice", "ac"},
        {"log", "logs"}, {"bash", "sh"}, {"shell", "sh"},
        {"nothumbs", "nothumb"}, {"commanddelete", "cmddelete"},
        {"bassboost", "bass"}, {"listplaylist", "playlist"},
        {"myplaylist", "playlist"}, {"playlists", "playlist"},
        {"mylist", "playlist"}, {"delplaylist", "rmplaylist"},
        {"removeplaylist", "rmplaylist"}, {"pplaylist", "playplaylist"},
        {"plplay", "playplaylist"}, {"playmylist", "playplaylist"},
        {"saveplaylist", "addplaylist"}, {"addsong", "addplaylist"},
        {"maint", "maintenance"}, {"spt", "speedtest"},
        {"assistant", "userbot"}, {"userbotjoin", "userbot"},
        {"assistantjoin", "userbot"},
        {"setcplay", "channelplay"}, {"rtmp", "setrtmp"},
    };
    // c-commands (NoxBeats channel_commands()): name -> (base, is_play).
    channel_cmds_ = {
        {"cplay", {"play", true}}, {"vcplay", {"vplay", true}},
        {"cvplay", {"vplay", true}}, {"cfplay", {"playforce", true}},
        {"fcplay", {"playforce", true}}, {"cforceplay", {"playforce", true}},
        {"cplayforce", {"playforce", true}}, {"fvcplay", {"vplayforce", true}},
        {"fvcpay", {"vplayforce", true}}, {"vcplayforce", {"vplayforce", true}},
        {"cpause", {"pause", false}}, {"cresume", {"resume", false}},
        {"cmute", {"mute", false}}, {"cunmute", {"unmute", false}},
        {"cskip", {"skip", false}}, {"caskip", {"autoplay-skip", false}},
        {"cstop", {"stop", false}},
        {"cend", {"stop", false}}, {"cqueue", {"queue", false}},
        {"cclear", {"clear", false}}, {"cremove", {"remove", false}},
        {"cmove", {"move", false}}, {"crestore", {"restore", false}},
        {"cpos", {"pos", false}}, {"cposition", {"pos", false}},
        {"cseek", {"seek", false}}, {"cseekback", {"seekback", false}},
        {"cjump", {"jump", false}}, {"cbass", {"bass", false}},
        {"cbassboost", {"bass", false}}, {"cspeed", {"speed", false}},
        {"csetspeed", {"speed", false}}, {"cspeedup", {"speed", false}},
        {"creplay", {"replay", false}}, {"cloop", {"loop", false}},
        {"csetloop", {"loop", false}}, {"cshuffle", {"shuffle", false}},
        {"cplayer", {"player", false}}, {"cjoin", {"userbot", false}},
        {"creload", {"reload", false}},
    };
}

void Dispatcher::handleMessage(const TgMessage& msg) {
    // Service messages first (NoxBeats on_new_members / on_video_chat).
    if (!msg.new_chat_members.empty()) {
        if (msg.chat.type != "supergroup") {
            tg_.leaveChat(msg.chat.id);
            return;
        }
        for (const auto& u : msg.new_chat_members) {
            if (bot_id_ != 0 && u.id == bot_id_) {
                store_.sadd("chats", std::to_string(msg.chat.id));
                logToLogger("Bot added to chat " +
                            std::to_string(msg.chat.id));
            }
        }
    }
    if (msg.video_chat_started || msg.video_chat_ended) {
        // Auto-end: a voice chat ending kills the session (tools.cpp).
        setCallActive(msg.chat.id, false);
        setChatField(msg.chat.id, "voice_owner", "");
        Engine used;
        bool fb = false;
        vcCall({msg, "stop", "", false}, "stop",
               Json::objectOf({{"chat_id", Json(msg.chat.id)}}), used, fb);
        return;
    }
    if (!msg.has_text) return;

    // Parse "/cmd@bot args" (NoxBeats parse_command): @bot must be us.
    std::string text = msg.text;
    if (text.empty() || text.front() != '/') return;
    std::string rest = text.substr(1);
    size_t space = rest.find(' ');
    std::string head = space == std::string::npos ? rest : rest.substr(0, space);
    std::string args =
        space == std::string::npos ? "" : trim(rest.substr(space + 1));
    if (auto at = head.find('@'); at != std::string::npos) {
        std::string target = lowerStr(head.substr(at + 1));
        if (!bot_username_.empty() && target != lowerStr(bot_username_))
            return;  // addressed to another bot
        head = head.substr(0, at);
    }
    std::string cmd = lowerStr(head);
    if (cmd.empty()) return;

    CommandContext ctx{msg, cmd, args, false};
    // c-commands run the same handler against the linked channel.
    if (auto ch = channel_cmds_.find(cmd); ch != channel_cmds_.end()) {
        ctx.command = ch->second.first;
        ctx.via_channel = true;
    } else if (auto al = aliases_.find(cmd); al != aliases_.end()) {
        ctx.command = al->second;
    }

    if (!chatAllowed(msg.chat.id)) {
        reply(ctx, "This chat is not allowed to use Parlay.");
        return;
    }
    if (rateLimited(msg.chat.id)) return;  // silent drop, 1 cmd/s per chat

    for (const auto& r : routes_) {
        if (std::find(r.cmds.begin(), r.cmds.end(), ctx.command) ==
            r.cmds.end())
            continue;
        if (r.scope == Scope::Group && !isGroup(ctx)) continue;
        if (r.scope == Scope::Private && !isPrivate(ctx)) continue;
        // Perm gates fail silently (NoxBeats dispatch_message).
        if (r.perm == Perm::Sudo && !isSudo(msg.from.id)) return;
        if (r.perm == Perm::Owner && !isOwner(msg.from.id)) return;
        // Blacklisted users are refused on Everyone commands (except
        // /start and /listsudo).
        if (r.perm == Perm::Everyone && ctx.command != "start" &&
            ctx.command != "listsudo" && ctx.command != "sudolist" &&
            isBlUser(msg.from.id))
            return;
        // Blacklisted chat: leave.
        if (isBlChat(msg.chat.id)) {
            std::cout << "[cmd] chat " << msg.chat.id
                      << " is blacklisted, leaving\n";
            tg_.leaveChat(msg.chat.id);
            return;
        }
        // Maintenance: only sudo passes.
        if (maintenanceOn() && !isSudo(msg.from.id)) {
            std::string why;
            auto reason = store_.get("maint_reason");
            reply(ctx, "Parlay is under maintenance." +
                           (reason && !reason->empty()
                                ? " Reason: " + *reason
                                : ""));
            return;
        }
        {
            std::lock_guard<std::mutex> lk(log_mu_);
            recent_log_.push_back("/" + ctx.command + " chat=" +
                                  std::to_string(msg.chat.id) + " user=" +
                                  std::to_string(msg.from.id));
            if (recent_log_.size() > 100) recent_log_.erase(recent_log_.begin());
        }
        std::cout << "[cmd] /" << ctx.command << " chat=" << msg.chat.id
                  << " user=" << msg.from.id << " args=" << ctx.args << "\n";
        try {
            r.fn(ctx);
        } catch (const std::exception& e) {
            std::cerr << "[cmd] handler /" << ctx.command
                      << " threw: " << e.what() << "\n";
            reply(ctx, "Something went wrong handling that command.");
        }
        // Clean mode: the command message goes too (Everyone commands).
        if (r.perm == Perm::Everyone) {
            int mins = chatIntField(msg.chat.id, "clean_mode", 0);
            if (mins > 0)
                scheduleDelete(msg.chat.id, msg.message_id, mins);
        }
        return;  // first match wins
    }
    reply(ctx, "Unknown command. Try /help.");
}

// --- permissions -----------------------------------------------------------
bool Dispatcher::chatAllowed(int64_t chat_id) const {
    if (cfg_.block_chats.count(chat_id)) return false;
    if (!cfg_.allow_chats.empty() && !cfg_.allow_chats.count(chat_id))
        return false;
    return true;
}

bool Dispatcher::isSudo(int64_t user_id) const {
    if (isOwner(user_id)) return true;
    return store_.sismember("sudoers", std::to_string(user_id));
}

bool Dispatcher::isBlUser(int64_t user_id) const {
    return store_.sismember("bl_users", std::to_string(user_id));
}

bool Dispatcher::isBlChat(int64_t chat_id) const {
    return store_.sismember("bl_chats", std::to_string(chat_id));
}

bool Dispatcher::maintenanceOn() const {
    auto v = store_.get("maintenance");
    return v && *v == "1";
}

std::vector<int64_t> Dispatcher::cachedAdmins(int64_t chat_id, bool reload) {
    std::string key = "admins:" + std::to_string(chat_id);
    if (!reload) {
        auto cached = store_.cacheGet(key);
        if (cached) {
            std::vector<int64_t> out;
            std::stringstream ss(*cached);
            std::string item;
            while (std::getline(ss, item, ',')) {
                try {
                    out.push_back(std::stoll(item));
                } catch (...) {
                }
            }
            return out;
        }
    }
    auto admins = tg_.getChatAdministrators(chat_id);
    std::string joined;
    for (size_t i = 0; i < admins.size(); ++i) {
        if (i) joined += ",";
        joined += std::to_string(admins[i]);
    }
    store_.cacheSet(key, joined, 600);  // 10 min TTL (NoxBeats admincache)
    return admins;
}

bool Dispatcher::isChatAdmin(int64_t chat_id, int64_t user_id) {
    if (isOwner(user_id)) return true;
    auto admins = cachedAdmins(chat_id);
    if (std::find(admins.begin(), admins.end(), user_id) != admins.end())
        return true;
    // Live fallback (NoxBeats is_admin): getChatMember status.
    std::string status = tg_.getChatMemberStatus(chat_id, user_id);
    return status == "creator" || status == "administrator";
}

bool Dispatcher::adminCheck(const CommandContext& ctx) {
    if (isPrivate(ctx)) return true;
    if (isSudo(ctx.message.from.id) ||
        isChatAdmin(ctx.message.chat.id, ctx.message.from.id))
        return true;
    reply(ctx, "You don't have permission to do that here.");
    return false;
}

bool Dispatcher::canManageVc(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    int64_t user = ctx.message.from.id;
    if (isSudo(user)) return true;
    int mode = chatIntField(chat, "admin_mode", 0);
    if (mode == 2) return true;  // everyone
    if (isChatAdmin(chat, user)) return true;
    if (mode == 0 && isAuthUser(chat, user)) return true;
    reply(ctx, "Only group admins (or authorized users) can control "
               "playback. See /adminmode.");
    return false;
}

bool Dispatcher::playAllowed(const CommandContext& ctx, bool force) {
    int64_t chat = vcTarget(ctx);
    bool admin_only = chatField(chat, "play_mode", "everyone") == "admin";
    if (!admin_only && !force) return true;
    int64_t user = ctx.message.from.id;
    if (isSudo(user) || isChatAdmin(chat, user) || isAuthUser(chat, user))
        return true;
    reply(ctx, "Only group admins can use /play in this chat (playmode is "
               "admin-only).");
    return false;
}

bool Dispatcher::rateLimited(int64_t chat_id) {
    auto now = std::chrono::steady_clock::now();
    auto it = last_cmd_.find(chat_id);
    if (it != last_cmd_.end() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second)
                .count() < 1000) {
        return true;  // anonx-cpp RateLimiter shape: 1 cmd/sec per chat
    }
    last_cmd_[chat_id] = now;
    return false;
}

// --- chat settings ---------------------------------------------------------
std::string Dispatcher::chatField(int64_t chat_id, const std::string& field,
                                  const std::string& def) {
    auto v = store_.hget("chat:" + std::to_string(chat_id), field);
    return v ? *v : def;
}

void Dispatcher::setChatField(int64_t chat_id, const std::string& field,
                              const std::string& value) {
    store_.hset("chat:" + std::to_string(chat_id), field, value);
}

int Dispatcher::chatIntField(int64_t chat_id, const std::string& field,
                             int def) {
    std::string v = chatField(chat_id, field, "");
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

double Dispatcher::chatSpeedField(int64_t chat_id) {
    std::string v = chatField(chat_id, "speed", "");
    if (v.empty()) return 1.0;
    char* end = nullptr;
    double s = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !std::isfinite(s) ||
        s < 0.5 || s > 2.0)
        return 1.0;
    return s;
}

bool Dispatcher::applyCoreFilter(const CommandContext& ctx, double speed,
                                 int bass, const std::string& done_msg) {
    int64_t chat = vcTarget(ctx);
    if (chatField(chat, "voice_owner", "") != "core") return false;
    if (!callActive(chat)) {
        reply(ctx, "Nothing is playing.");
        return true;
    }
    std::string source = chatField(chat, "core_source", "");
    if (source.empty()) {
        // A core session with no recorded source (e.g. joined with
        // /join but never played a replied file through this build):
        // there is nothing to re-attach, so say so instead of
        // claiming the filter was applied.
        reply(ctx, "This chat's audio is on the core voice backend, but "
                   "no source was recorded for it, so the filter cannot "
                   "be re-applied. Play a replied file again, then set "
                   "the speed/bass.");
        return true;
    }
    std::string title = chatField(chat, "core_title", "Replied file");
    bool video = chatField(chat, "core_video", "0") == "1";
    int64_t pos = voice_.time(chat);
    int seek = pos > 0 ? static_cast<int>(pos) : 0;
    if (!voice_.play(chat, source, title, video, seek, speed, bass)) {
        std::string why = voice_.lastError();
        if (why.empty()) why = voice_.describe();
        reply(ctx, "Could not apply the filter: " + why);
        return true;
    }
    reply(ctx, done_msg + " (core voice re-attached" +
                   (seek > 0 ? " at " + fmtSeconds(seek)
                             : " from the start (position unknown)") +
                   " with the filter applied).");
    return true;
}

bool Dispatcher::isAuthUser(int64_t chat_id, int64_t user_id) const {
    return store_.sismember("auth:" + std::to_string(chat_id),
                            std::to_string(user_id));
}

int64_t Dispatcher::vcTarget(const CommandContext& ctx) const {
    if (!ctx.via_channel) return ctx.message.chat.id;
    auto v = store_.hget("chat:" + std::to_string(ctx.message.chat.id),
                         "cplay");
    if (!v) return ctx.message.chat.id;
    try {
        int64_t ch = std::stoll(*v);
        return ch != 0 ? ch : ctx.message.chat.id;
    } catch (...) {
        return ctx.message.chat.id;
    }
}

// --- replies / IPC helpers -------------------------------------------------
int64_t Dispatcher::reply(const CommandContext& ctx, const std::string& text) {
    int64_t id = tg_.sendMessageId(ctx.message.chat.id, text,
                                   ctx.message.message_id);
    if (id < 0) {
        std::cerr << "[tg] sendMessage failed\n";
        return -1;
    }
    int mins = chatIntField(ctx.message.chat.id, "clean_mode", 0);
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

void Dispatcher::replyError(const CommandContext& ctx, const IpcResult& r) {
    reply(ctx, "Service error (" + r.error_code + "): " + r.error_message);
}

void Dispatcher::logToLogger(const std::string& text) {
    if (cfg_.logger_id == 0) return;
    auto on = store_.get("logger_on");
    if (on && *on == "0") return;
    std::string err;
    tg_.sendMessage(cfg_.logger_id, text, err);
}

void Dispatcher::scheduleDelete(int64_t chat_id, int64_t message_id,
                                int minutes) {
    std::lock_guard<std::mutex> lk(clean_mu_);
    clean_queue_.push_back(
        {chat_id, message_id,
         std::chrono::steady_clock::now() + std::chrono::minutes(minutes)});
}

void Dispatcher::deleteCommandMessage(const CommandContext& ctx) {
    if (chatField(ctx.message.chat.id, "cmd_delete", "off") == "on")
        tg_.deleteMessage(ctx.message.chat.id, ctx.message.message_id);
}

IpcResult Dispatcher::vcCall(const CommandContext& ctx, const std::string& action,
                             const Json& body, Engine& used, bool& fell_back) {
    return router_.callVc(ipc_, vcTarget(ctx), action, body, used, fell_back);
}

IpcResult Dispatcher::vcGet(const CommandContext& ctx, const std::string& action,
                            Engine& used, bool& fell_back) {
    // GET /v1/<action>?chat_id=... with the same fallback order as POST.
    Engine preferred = router_.engineFor(vcTarget(ctx));
    fell_back = false;
    IpcResult last;
    for (Engine e : fallbackOrder(preferred)) {
        std::string path = "/v1/" + action +
                           "?chat_id=" + std::to_string(vcTarget(ctx));
        last = ipc_.get(router_.baseUrl(e), path, 10);
        if (!last.unavailable(action)) {
            used = e;
            fell_back = (e != preferred);
            return last;
        }
    }
    used = preferred;
    return last;
}

bool Dispatcher::nowPlaying(int64_t chat_id, Json& np_out, Json& track_out) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Engine used;
    bool fb = false;
    IpcResult r = vcGet(fake, "now-playing", used, fb);
    if (!r.ok) return false;
    const Json* np = r.data.find("now_playing");
    if (!np) return false;
    np_out = *np;
    const Json* track = np->find("track");
    if (!track) return false;
    track_out = *track;
    return true;
}

// HTTP API seam: the whole now-playing object for one chat, straight
// from the engines (same vcGet fail-over as the Telegram commands).
bool Dispatcher::apiNowPlaying(int64_t chat_id, Json& np_out,
                               std::string& error) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Engine used;
    bool fb = false;
    IpcResult r = vcGet(fake, "now-playing", used, fb);
    if (!r.ok) {
        error = r.describe();
        return false;
    }
    const Json* np = r.data.find("now_playing");
    if (!np || !np->isObject()) {
        error = "engine response has no now_playing object";
        return false;
    }
    // vc-rust answers 200 with an empty room where go/ts 404: a null
    // track AND an empty queue is no playback state, not a track.
    const Json* track = np->find("track");
    const Json* queue = np->find("queue");
    bool has_track = track && !track->isNull();
    bool has_queue = queue && queue->isArray() && queue->size() > 0;
    if (!has_track && !has_queue) {
        error = "no playback state";
        return false;
    }
    np_out = *np;
    return true;
}

// HTTP API seam: the engine's queue list (POST /v1/queue action=list).
bool Dispatcher::apiQueueList(int64_t chat_id, Json& queue_out,
                              std::string& error) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Engine used;
    bool fb = false;
    Json body = Json::objectOf({{"action", Json("list")},
                                {"chat_id", Json(chat_id)}});
    IpcResult r = vcCall(fake, "queue", body, used, fb);
    if (!r.ok) {
        error = r.describe();
        return false;
    }
    const Json* q = r.data.find("queue");
    if (!q || !q->isArray()) {
        error = "engine response has no queue array";
        return false;
    }
    queue_out = *q;
    return true;
}

void Dispatcher::setCallActive(int64_t chat_id, bool active) {
    if (active)
        store_.set("call:" + std::to_string(chat_id), "1");
    else
        store_.del("call:" + std::to_string(chat_id));
}

bool Dispatcher::callActive(int64_t chat_id) const {
    auto v = store_.get("call:" + std::to_string(chat_id));
    return v && *v == "1";
}

std::optional<int64_t> Dispatcher::extractUserId(const CommandContext& ctx) {
    if (ctx.message.has_reply_to && ctx.message.reply_from_id != 0)
        return ctx.message.reply_from_id;
    auto args = splitArgs(ctx.args);
    if (args.empty()) return std::nullopt;
    const std::string& a = args[0];
    if (!a.empty() && a[0] == '@') {
        int64_t id = tg_.getChatId(a);
        if (id != 0) return id;
        return std::nullopt;
    }
    if (isDigits(a)) {
        try {
            return std::stoll(a);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// --- formatting ------------------------------------------------------------
std::string Dispatcher::fmtTrack(const Json& track) {
    std::string title =
        track.find("title") ? track.find("title")->asString() : "?";
    std::string artist =
        track.find("artist") ? track.find("artist")->asString() : "";
    std::string out = title;
    if (!artist.empty()) out += " — " + artist;
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    std::string d = fmtDuration(dur);
    if (!d.empty()) out += " [" + d + "]";
    return out;
}

std::string Dispatcher::fmtNowPlaying(const Json& np, const std::string& verb) {
    std::string out = verb + "\n";
    const Json* track = np.find("track");
    if (track) out += "🎵 " + fmtTrack(*track) + "\n";
    const Json* engine = np.find("engine");
    if (engine) out += "Engine: " + engine->asString() + "\n";
    const Json* paused = np.find("paused");
    if (paused && paused->asBool()) out += "Paused\n";
    return out;
}

std::string Dispatcher::fmtQueue(const Json& data) {
    const Json* q = data.find("queue");
    if (!q || q->size() == 0) return "Queue is empty.";
    std::string out = "Queue (" + std::to_string(q->size()) + "):\n";
    size_t i = 0;
    for (const auto& item : q->asArray()) {
        const Json* track = item.find("track");
        if (!track) continue;
        out += std::to_string(++i) + ". " + fmtTrack(*track);
        const Json* by = item.find("requested_by");
        if (by) out += " (by " + std::to_string(by->asInt()) + ")";
        out += "\n";
    }
    return out;
}

std::string Dispatcher::fmtSeconds(int64_t sec) {
    return fmtDuration(sec);
}

// --- background loops --------------------------------------------------------
void Dispatcher::startBackground() {
    bg_stop_ = false;
    bg_thread_ = std::thread([this] { backgroundLoop(); });
}

void Dispatcher::stopBackground() {
    bg_stop_ = true;
    if (bg_thread_.joinable()) bg_thread_.join();
}

void Dispatcher::backgroundLoop() {
    while (!bg_stop_) {
        for (int i = 0; i < 5 && !bg_stop_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (bg_stop_) break;
        sweepClean();
        watchLoopAutoplay();
    }
}

void Dispatcher::sweepClean() {
    std::vector<PendingDelete> due;
    {
        std::lock_guard<std::mutex> lk(clean_mu_);
        auto now = std::chrono::steady_clock::now();
        auto it = clean_queue_.begin();
        while (it != clean_queue_.end()) {
            if (it->at <= now) {
                due.push_back(*it);
                it = clean_queue_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& d : due) tg_.deleteMessage(d.chat, d.message);
}

void Dispatcher::watchLoopAutoplay() {
    // Autoplay: when a chat's session drains (nothing playing, queue
    // empty) and autoplay is on, queue a related track via vc-ts search
    // on the last track's title. Loop is enforced at /loop time by
    // queueing copies, so the watcher only handles autoplay.
    auto chats = store_.keys("autoplay:");
    for (const auto& key : chats) {
        if (bg_stop_) return;
        std::string chat_s = key.substr(std::string("autoplay:").size());
        int64_t chat = 0;
        try {
            chat = std::stoll(chat_s);
        } catch (...) {
            continue;
        }
        auto on = store_.get(key);
        if (!on || *on != "1") continue;
        if (!callActive(chat)) continue;
        Json np, track;
        if (nowPlaying(chat, np, track)) continue;  // still playing
        std::string last = chatField(chat, "last_track", "");
        if (last.empty()) continue;
        Json body = Json::objectOf({
            {"query", Json(last)},
            {"source", Json("all")},
            {"limit", Json(static_cast<int64_t>(3))},
        });
        IpcResult r = ipc_.post(cfg_.vc_ts_url, "search", body, 15);
        if (!r.ok) continue;
        const Json* results = r.data.find("results");
        if (!results || results->size() == 0) continue;
        // First result that is not the track that just ended.
        const Json* pick = nullptr;
        for (const auto& t : results->asArray()) {
            const Json* title = t.find("title");
            if (title && title->asString() != last) {
                pick = &t;
                break;
            }
        }
        if (!pick) continue;
        Json play = Json::objectOf({
            {"chat_id", Json(chat)},
            {"query", Json(pick->find("title") ? pick->find("title")->asString() : "")},
            {"requested_by", Json(bot_id_)},
        });
        Engine used;
        bool fb = false;
        CommandContext fake;
        fake.message.chat.id = chat;
        IpcResult pr = vcCall(fake, "play", play, used, fb);
        if (pr.ok) {
            setChatField(chat, "last_track",
                         pick->find("title") ? pick->find("title")->asString()
                                             : "");
        }
    }
}

// --- handlers: start / help -------------------------------------------------
void Dispatcher::onStart(const CommandContext& ctx) {
    int64_t uid = ctx.message.from.id;
    if (isBlUser(uid)) {
        reply(ctx, "You are blocked from using Parlay.");
        return;
    }
    if (isPrivate(ctx)) {
        if (!store_.sismember("users", std::to_string(uid))) {
            store_.sadd("users", std::to_string(uid));
            logToLogger("New user: " + std::to_string(uid));
        }
    } else {
        if (!store_.sismember("chats", std::to_string(ctx.message.chat.id))) {
            store_.sadd("chats", std::to_string(ctx.message.chat.id));
            logToLogger("Bot added to chat " +
                        std::to_string(ctx.message.chat.id));
        }
    }
    reply(ctx,
          "Hi, I'm Parlay — one bot, many music engines.\n"
          "Use /play <song or link> to start music in a voice chat.\n"
          "Use /help for all commands.");
}

void Dispatcher::onHelp(const CommandContext& ctx) {
    reply(ctx,
          "Parlay commands:\n"
          "/play <song, link or id> — play in voice chat (-f force, -v video)\n"
          "/vplay <...> — play video in voice chat\n"
          "/playplaylist — play your saved playlist\n"
          "/pause /resume /skip /autoplay-skip /stop /end — playback\n"
          "/loop [n|off] — repeat the current track n times (0-10)\n"
          "/seek <sec> /seekback <sec> /jump <mm:ss> — seek\n"
          "/queue /playing — show the queue\n"
          "/clear /restore /remove <n> /move <a> <b> /shuffle — queue edits\n"
          "/pos — current position  /speed [0.5-2.0] /bass [0-100]\n"
          "/mute /unmute /replay /player /join — more playback\n"
          "/lyrics [song] — lyrics for the current (or named) song\n"
          "/search <query> — search YouTube / NetEase / Lemonic\n"
          "/download <song or link> — download the audio file\n"
          "/radio [station] — start a radio station in voice chat\n"
          "/share <song> — create an anonymous share link\n"
          "/engine [go|ts|rust] — show or switch this chat's VC engine\n"
          "/addplaylist <song|url> /playlist /rmplaylist <n|name|all>\n"
          "/playmode [admin|everyone] /settings — chat settings\n"
          "/adminmode [admin|adminauth|everyone] — who controls playback\n"
          "/cmddelete [on|off] /cleanmode [on|off|minutes]\n"
          "/thumbnail [on|off] /autoplay [on|off|lang|mood]\n"
          "/channelplay <@channel|id|disable> /userbot — channel play\n"
          "/auth /unauth /authlist — authorized users\n"
          "/admincache — refresh the admin list\n"
          "/ping /stats /json /bug /privacy — tools\n"
          "Sudo: /blacklist /broadcast /ac /logs /logger /restart "
          "/speedtest /autoleave\n"
          "Owner: /addsudo /delsudo /listsudo /maintenance /eval");
}

// --- handlers: playback ------------------------------------------------------
void Dispatcher::onPlay(const CommandContext& ctx) {
    if (!isGroup(ctx)) {
        reply(ctx, "Music only plays in groups (supergroups).");
        return;
    }
    // Flags: -f (force) -v (video) -s (shuffle, playplaylist only).
    auto words = splitArgs(ctx.args);
    bool flag_f = false, flag_v = false;
    std::string query;
    for (const auto& w : words) {
        if (w == "-f") flag_f = true;
        else if (w == "-v") flag_v = true;
        else if (w == "-s") { /* shuffle: only for /playplaylist */
        } else {
            if (!query.empty()) query += " ";
            query += w;
        }
    }
    bool force = flag_f ||
                 ctx.command == "playforce" || ctx.command == "vplayforce";
    bool video = (ctx.command == "vplay" || ctx.command == "vplayforce" ||
                  flag_v);
    // Reply-to media (or media sent with the command as its caption):
    // /play on a replied audio/video/voice/video_note/document plays
    // that Telegram file (source "file"). The core downloads it via
    // the Bot API getFile and plays it through its own in-process
    // voice backend — the vc engines cannot resolve a Telegram
    // file_id. No query is needed; a missing backend fails honestly.
    if (query.empty() && ctx.command != "playplaylist") {
        std::string file_id, title, kind;
        if (ctx.message.has_reply_media) {
            file_id = ctx.message.reply_media_file_id;
            title = ctx.message.reply_media_title;
            kind = ctx.message.reply_media_kind;
        } else if (ctx.message.has_media) {
            file_id = ctx.message.media_file_id;
            title = ctx.message.media_title;
            kind = ctx.message.media_kind;
        }
        if (!file_id.empty()) {
            if (!playAllowed(ctx, force)) return;
            playRepliedFile(ctx, file_id, title, kind);
            return;
        }
        reply(ctx, "Usage: /play <song title, link or track id>");
        return;
    }
    if (!playAllowed(ctx, force)) return;
    if (ctx.via_channel && chatField(ctx.message.chat.id, "cplay", "0") == "0") {
        reply(ctx, "No channel linked. Use /channelplay first.");
        return;
    }
    // Duration limit (NoxBeats DURATION_LIMIT, default 60 min): enforced
    // on the resolved track after the play call when the engine reports
    // duration_sec; a too-long track is stopped right away.
    if (force) {
        // playforce: replace the current track (queue().force_add shape).
        Engine used;
        bool fb = false;
        Json stop_body =
            Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
        vcCall(ctx, "stop", stop_body, used, fb);
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"query", Json(trim(query))},
        {"requested_by", Json(ctx.message.from.id)},
        {"video", Json(video)},
        {"force", Json(force)},
    });
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "play", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    Json now = np ? *np : r.data;
    const Json* track = now.find("track");
    if (track) {
        int64_t dur =
            track->find("duration_sec") ? track->find("duration_sec")->asInt() : 0;
        if (dur > 0 && dur > cfg_.duration_limit_min * 60) {
            Engine u2;
            bool fb2 = false;
            vcCall(ctx, "stop",
                   Json::objectOf({{"chat_id", Json(vcTarget(ctx))}}), u2, fb2);
            reply(ctx, "That track is longer than the " +
                           std::to_string(cfg_.duration_limit_min) +
                           " minute limit.");
            return;
        }
        setChatField(vcTarget(ctx), "last_track",
                     track->find("title") ? track->find("title")->asString() : "");
    }
    setCallActive(vcTarget(ctx), true);
    setChatField(vcTarget(ctx), "voice_owner", engineName(used));
    if (chatField(vcTarget(ctx), "logger_on", "") == "1" ||
        store_.get("logger_on") == std::optional<std::string>("1"))
        logToLogger("Play in " + std::to_string(vcTarget(ctx)) + ": " +
                    (track && track->find("title")
                         ? track->find("title")->asString()
                         : query));
    std::string text = fmtNowPlaying(now, "▶ Now playing");
    if (fell_back)
        text += "(fell back to engine " + std::string(engineName(used)) +
                " — your preferred engine is unavailable)\n";
    if (chatIntField(vcTarget(ctx), "muted", 0))
        text += "(muted — use /unmute)\n";
    reply(ctx, text);
    deleteCommandMessage(ctx);
}

// --- /play from a replied-to Telegram file ----------------------------------
void Dispatcher::playRepliedFile(const CommandContext& ctx,
                                 const std::string& file_id,
                                 const std::string& title,
                                 const std::string& kind) {
    int64_t chat = vcTarget(ctx);
    std::string saved, err;
    if (!tg_.downloadFile(file_id, cfg_.data_dir + "/replied", saved, err)) {
        reply(ctx, "Could not fetch the replied file: " + err);
        return;
    }
    // The downloaded file is the stream source for the core's own voice
    // backend (ffmpeg input): a local path, not a vc.play query — the
    // engines have no Telegram access and would degrade a file_id to a
    // fake track.
    std::string track_title = title.empty() ? "Replied file" : title;
    bool video = kind == "video" || kind == "video_note";
    // The chat's stored speed/bass apply here: the core voice
    // backend is the only playback path with the filter
    // (buildFfmpegCmd atempo/bass), so a /speed or /bass set earlier
    // takes effect on this play, and /speed + /bass later re-attach
    // this same source (core_source below) with new values.
    if (!voice_.play(chat, saved, track_title, video, 0,
                     chatSpeedField(chat), chatIntField(chat, "bass", 0))) {
        std::string why = voice_.lastError();
        if (why.empty()) why = voice_.describe();
        reply(ctx, "Could not play the replied file: " + why);
        return;
    }
    setChatField(chat, "last_track", track_title);
    setChatField(chat, "voice_owner", "core");
    setChatField(chat, "core_source", saved);
    setChatField(chat, "core_title", track_title);
    setChatField(chat, "core_video", video ? "1" : "0");
    setCallActive(chat, true);
    reply(ctx, "▶ Now playing " + track_title +
                   " (replied file, via core voice).");
    deleteCommandMessage(ctx);
}

void Dispatcher::onPause(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "pause", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    reply(ctx, fmtNowPlaying(np ? *np : r.data, "⏸ Paused"));
}

void Dispatcher::onResume(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "resume", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    reply(ctx, fmtNowPlaying(np ? *np : r.data, "▶ Resumed"));
}

void Dispatcher::onSkip(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "skip", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    if (np && np->find("track")) {
        reply(ctx, fmtNowPlaying(*np, "⏭ Skipped — now playing"));
    } else {
        setCallActive(vcTarget(ctx), false);
        reply(ctx, "⏭ Skipped — the queue is now empty.");
    }
}

// /autoplay-skip, /askip (Phase 6): force an autoplay mix pick now.
// Contract vc.autoplay_skip (POST /v1/autoplay-skip) exists in all
// three VC engines, but the core exposed no command for it —
// COMMANDS.md listed it, the route table did not. Same shape as
// onSkip: the engines answer {now_playing} with the picked track
// (vc-go: plain skip, same envelope); a 400 "autoplay is off" or a
// 502 "no autoplay recommendation" surfaces honestly via replyError.
void Dispatcher::onAutoplaySkip(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "autoplay-skip", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    if (np && np->find("track")) {
        reply(ctx, fmtNowPlaying(*np, "⏭ Autoplay skipped — now playing"));
    } else {
        setCallActive(vcTarget(ctx), false);
        reply(ctx, "⏭ Autoplay skipped — the queue is now empty.");
    }
}

void Dispatcher::onStop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    bool was_active = callActive(vcTarget(ctx));
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "stop", body, used, fell_back);
    setCallActive(vcTarget(ctx), false);
    setChatField(vcTarget(ctx), "voice_owner", "");
    // Also stop a session the core voice backend itself holds
    // (replied-file playback, /join): /stop must end that too.
    // Best effort — false when the backend holds no call here.
    voice_.stop(vcTarget(ctx));
    setChatField(vcTarget(ctx), "loop", "0");
    setChatField(vcTarget(ctx), "speed", "1.0");
    setChatField(vcTarget(ctx), "bass", "0");
    setChatField(vcTarget(ctx), "muted", "0");
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, was_active ? "⏹ Stopped and cleared the queue."
                          : "Nothing was playing.");
}

void Dispatcher::onQueue(const CommandContext& ctx) {
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    Engine used;
    bool fell_back = false;
    IpcResult r = vcCall(ctx, "queue", body, used, fell_back);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, fmtQueue(r.data));
}

void Dispatcher::onLoop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    auto args = splitArgs(ctx.args);
    int64_t chat = vcTarget(ctx);
    if (args.empty()) {
        int n = chatIntField(chat, "loop", 0);
        reply(ctx, n > 0 ? "Looping the current track " + std::to_string(n) +
                               " more time(s)."
                         : "Usage: /loop <1-10|off>");
        return;
    }
    std::string a = lowerStr(args[0]);
    if (a == "off" || a == "disable") {
        // Remove the queued copies of the current track (loop-off).
        Json np, track;
        if (nowPlaying(chat, np, track)) {
            std::string tid = track.find("id") ? track.find("id")->asString() : "";
            Json body = Json::objectOf({
                {"chat_id", Json(chat)},
                {"action", Json("list")},
            });
            Engine used;
            bool fb = false;
            IpcResult q = vcCall(ctx, "queue", body, used, fb);
            if (q.ok) {
                const Json* items = q.data.find("queue");
                if (items) {
                    // Remove from the end so positions stay valid.
                    std::vector<int64_t> positions;
                    int64_t pos = 0;
                    for (const auto& item : items->asArray()) {
                        ++pos;
                        const Json* t = item.find("track");
                        if (t && t->find("id") &&
                            t->find("id")->asString() == tid && pos > 1)
                            positions.push_back(pos);
                    }
                    for (auto it = positions.rbegin(); it != positions.rend();
                         ++it) {
                        Json rm = Json::objectOf({
                            {"chat_id", Json(chat)},
                            {"action", Json("remove")},
                            {"position", Json(*it)},
                        });
                        Engine u2;
                        bool fb2 = false;
                        vcCall(ctx, "queue", rm, u2, fb2);
                    }
                }
            }
        }
        setChatField(chat, "loop", "0");
        reply(ctx, "Loop off.");
        return;
    }
    if (!isDigits(a)) {
        reply(ctx, "Usage: /loop <1-10|off>");
        return;
    }
    long n = std::stol(a.substr(0, 9));
    if (n < 1) n = 0;
    if (n > 10) n = 10;
    if (n == 0) {
        setChatField(chat, "loop", "0");
        reply(ctx, "Loop off.");
        return;
    }
    // Loop is enforced by queueing n copies of the current track after it:
    // when the track ends the engine auto-advances to an identical copy.
    // (The vc services have no native loop action; this uses only the
    // queue contract, so the loop is real, not a stored flag.)
    Json np, track;
    if (!nowPlaying(chat, np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    int added = 0;
    for (long i = 0; i < n; ++i) {
        Json body = Json::objectOf({
            {"chat_id", Json(chat)},
            {"action", Json("add")},
            {"track", track},
            {"requested_by", Json(ctx.message.from.id)},
        });
        Engine used;
        bool fb = false;
        IpcResult r = vcCall(ctx, "queue", body, used, fb);
        if (!r.ok) break;
        ++added;
    }
    setChatField(chat, "loop", std::to_string(added));
    reply(ctx, added > 0
                   ? "Looping the current track " + std::to_string(added) +
                         " more time(s)."
                   : "Could not queue loop copies (engine error).");
}

void Dispatcher::onSeek(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    if (args.empty() || !isDigits(args[0])) {
        reply(ctx, std::string("Usage: /") + ctx.command + " <seconds>");
        return;
    }
    int to_seek = std::stoi(args[0]);
    if (to_seek < 10) {
        reply(ctx, "Seek at least 10 seconds.");
        return;
    }
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    const Json* paused = np.find("paused");
    if (paused && paused->asBool()) {
        reply(ctx, "Playback is paused.");
        return;
    }
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    if (!dur) {
        reply(ctx, "This track has no known duration, cannot seek.");
        return;
    }
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    int64_t target = 0;
    if (ctx.command == "seekback") {
        target = pos - to_seek;
        if (target < 1) target = 1;
    } else {
        target = pos + to_seek;
        if (target + 10 > dur) target = dur - 5;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"position_sec", Json(target)},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "seek", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, std::string("Seeked ") +
                   (ctx.command == "seekback" ? "backward" : "forward") +
                   " to " + fmtSeconds(target) + ".");
}

void Dispatcher::onClear(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    Engine used;
    bool fb = false;
    IpcResult q = vcCall(ctx, "queue", list_body, used, fb);
    if (q.ok) {
        const Json* items = q.data.find("queue");
        if (items && items->size() > 1)
            store_.set("cleared:" + std::to_string(vcTarget(ctx)),
                       items->dump());
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("clear")},
    });
    IpcResult r = vcCall(ctx, "queue", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Queue cleared (current track keeps playing).");
}

void Dispatcher::onRestore(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto saved = store_.get("cleared:" + std::to_string(vcTarget(ctx)));
    if (!saved || saved->empty()) {
        reply(ctx, "Nothing to restore.");
        return;
    }
    Json arr;
    try {
        arr = Json::parse(*saved);
    } catch (...) {
        reply(ctx, "Nothing to restore.");
        return;
    }
    int added = 0;
    // The saved queue includes the then-current track at index 0; skip it
    // (it is still playing) and restore only the pending tail.
    for (size_t i = 1; i < arr.asArray().size(); ++i) {
        const Json& item = arr.asArray()[i];
        const Json* track = item.find("track");
        if (!track) continue;
        Json body = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", *track},
            {"requested_by", Json(ctx.message.from.id)},
        });
        Engine used;
        bool fb = false;
        IpcResult r = vcCall(ctx, "queue", body, used, fb);
        if (!r.ok) break;
        ++added;
    }
    if (added) {
        store_.del("cleared:" + std::to_string(vcTarget(ctx)));
        reply(ctx, "Restored " + std::to_string(added) + " track(s).");
    } else {
        reply(ctx, "Could not restore the queue (engine error).");
    }
}

void Dispatcher::onRemove(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    if (args.empty() || !isDigits(args[0])) {
        reply(ctx, "Usage: /remove <position>");
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("remove")},
        {"position", Json(static_cast<int64_t>(std::stoll(args[0])))},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "queue", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Removed position " + args[0] + " from the queue.");
}

void Dispatcher::onMove(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    if (args.size() < 2 || !isDigits(args[0]) || !isDigits(args[1])) {
        reply(ctx, "Usage: /move <from> <to>");
        return;
    }
    int64_t from = std::stoll(args[0]);
    int64_t to = std::stoll(args[1]);
    // The vc queue contract has no move: implement it as list -> remove
    // -> re-add at the target position (real IPC calls, same effect).
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    Engine used;
    bool fb = false;
    IpcResult q = vcCall(ctx, "queue", list_body, used, fb);
    if (!q.ok) {
        replyError(ctx, q);
        return;
    }
    const Json* items = q.data.find("queue");
    if (!items || from < 1 || to < 1 ||
        from > static_cast<int64_t>(items->size()) ||
        to > static_cast<int64_t>(items->size())) {
        reply(ctx, "Bad position.");
        return;
    }
    std::vector<Json> tracks;
    for (const auto& item : items->asArray()) {
        const Json* t = item.find("track");
        if (t) tracks.push_back(*t);
    }
    if (from == 1) {
        reply(ctx, "Cannot move the currently playing track.");
        return;
    }
    if (from == to) {
        reply(ctx, "Already at position " + std::to_string(to) + ".");
        return;
    }
    Json moved = tracks[static_cast<size_t>(from) - 1];
    tracks.erase(tracks.begin() + (from - 1));
    tracks.insert(tracks.begin() + (to - 1), moved);
    // Rebuild the pending tail: remove all pending (from the end, so
    // positions stay valid), then re-add in the new order.
    std::vector<Json> pending(tracks.begin() + 1, tracks.end());
    for (int64_t pos = static_cast<int64_t>(items->size()); pos >= 2; --pos) {
        Json rm = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("remove")},
            {"position", Json(pos)},
        });
        vcCall(ctx, "queue", rm, used, fb);
    }
    for (const auto& t : pending) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", t},
            {"requested_by", Json(ctx.message.from.id)},
        });
        vcCall(ctx, "queue", add, used, fb);
    }
    reply(ctx, "Moved position " + std::to_string(from) + " to " +
                   std::to_string(to) + ".");
}

void Dispatcher::onShuffle(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    Engine used;
    bool fb = false;
    IpcResult q = vcCall(ctx, "queue", list_body, used, fb);
    if (!q.ok) {
        replyError(ctx, q);
        return;
    }
    const Json* items = q.data.find("queue");
    if (!items || items->size() < 3) {
        reply(ctx, "Need at least 3 tracks in the queue to shuffle.");
        return;
    }
    std::vector<Json> pending;
    for (size_t i = 1; i < items->size(); ++i) {
        const Json* t = items->asArray()[i].find("track");
        if (t) pending.push_back(*t);
    }
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(pending.begin(), pending.end(), rng);
    // Rebuild the pending tail in shuffled order (remove all, re-add).
    for (size_t i = pending.size(); i >= 1; --i) {
        Json rm = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("remove")},
            {"position", Json(static_cast<int64_t>(i) + 1)},
        });
        vcCall(ctx, "queue", rm, used, fb);
        if (i == 1) break;
    }
    for (const auto& t : pending) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", t},
            {"requested_by", Json(ctx.message.from.id)},
        });
        vcCall(ctx, "queue", add, used, fb);
    }
    reply(ctx, "Shuffled " + std::to_string(pending.size()) +
                   " queued track(s).");
}

void Dispatcher::onPos(const CommandContext& ctx) {
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    std::string speed = chatField(vcTarget(ctx), "speed", "1.0");
    reply(ctx, fmtTrack(track) + "\nPosition: " + fmtSeconds(pos) +
                   (dur ? " / " + fmtSeconds(dur) : "") +
                   "  Speed: " + speed + "x");
}

void Dispatcher::onJump(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    int to = args.empty() ? -1 : parseTime(args[0]);
    if (to < 0) {
        reply(ctx, "Usage: /jump <mm:ss>");
        return;
    }
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    const Json* paused = np.find("paused");
    if (paused && paused->asBool()) {
        reply(ctx, "Playback is paused.");
        return;
    }
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    if (!dur) {
        reply(ctx, "This track has no known duration, cannot jump.");
        return;
    }
    if (to + 5 > dur) {
        reply(ctx, "That is past the end of the track (" + fmtSeconds(dur) +
                       ").");
        return;
    }
    if (to < 1) to = 1;
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"position_sec", Json(static_cast<int64_t>(to))},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "seek", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Jumped to " + fmtSeconds(to) + ".");
}

void Dispatcher::onSpeed(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Current speed: " + chatField(chat, "speed", "1.0") +
                       "x. Usage: /speed <0.5-2.0|normal>");
        return;
    }
    if (!canManageVc(ctx)) return;
    std::string a = lowerStr(args[0]);
    double s = 0;
    if (a == "normal" || a == "reset" || a == "off") {
        s = 1.0;
    } else {
        if (!a.empty() && a.back() == 'x') a.pop_back();
        char* end = nullptr;
        s = std::strtod(a.c_str(), &end);
        if (a.empty() || end == a.c_str() || *end != '\0' ||
            !std::isfinite(s))
            s = 0;
    }
    if (s < 0.5 || s > 2.0) {
        reply(ctx, "Speed must be 0.5x to 2.0x.");
        return;
    }
    setChatField(chat, "speed", std::to_string(s));
    // Core-owned session first: nowPlaying() below only asks the vc
    // engines, which know nothing about a core session, so the core
    // branch must run before it. It applies the speed for real
    // (buildFfmpegCmd atempo) by re-attaching the source.
    char b[16];
    std::snprintf(b, sizeof b, "%.2g", s);
    if (applyCoreFilter(ctx, s, chatIntField(chat, "bass", 0),
                        std::string("Speed set to ") + b + "x"))
        return;
    Json np, track;
    if (!nowPlaying(chat, np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    const Json* paused = np.find("paused");
    if (paused && paused->asBool()) {
        reply(ctx, "Playback is paused.");
        return;
    }
    // Engine-owned session (NoxBeats play_media restart shape):
    // restart the stream at the same position. Honest limit
    // (REMAINING.md B11): NO vc engine applies the value — vc-go
    // keeps queue state only and never touches audio, and vc-ts /
    // vc-rust run their ffmpeg pipeline once per track and never
    // re-encode on seek, so the seek-restart below only moves the
    // position. The value is stored (what /pos and /speed report)
    // and takes effect on core-voice playback; the reply says so
    // instead of claiming the speed was set.
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"position_sec", Json(std::max<int64_t>(1, pos))},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "seek", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, std::string("Speed ") + b +
                   "x saved, but the " + std::string(engineName(used)) +
                   " engine cannot apply audio filters — the stream "
                   "restarted at the current position at the old speed. "
                   "It applies to core-voice playback (replied files).");
}

void Dispatcher::onBass(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Current bass: " + std::to_string(chatIntField(chat, "bass", 0)) +
                       ". Usage: /bass <0-100|on|off|max>");
        return;
    }
    if (!canManageVc(ctx)) return;
    std::string a = lowerStr(args[0]);
    int lvl = -1;
    if (a == "off" || a == "reset" || a == "normal") lvl = 0;
    else if (a == "on") lvl = 50;
    else if (a == "max") lvl = 100;
    else if (isDigits(a) && a.size() <= 3) lvl = std::stoi(a);
    if (lvl < 0 || lvl > 100) {
        reply(ctx, "Bass must be 0-100.");
        return;
    }
    setChatField(chat, "bass", std::to_string(lvl));
    // Core-owned session first: nowPlaying() below only asks the vc
    // engines, which know nothing about a core session. The core
    // backend applies the bass boost for real (buildFfmpegCmd lowpass
    // mix) — see onSpeed.
    if (applyCoreFilter(ctx, chatSpeedField(chat), lvl,
                        "Bass set to " + std::to_string(lvl)))
        return;
    Json np, track;
    if (!nowPlaying(chat, np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    const Json* paused = np.find("paused");
    if (paused && paused->asBool()) {
        reply(ctx, "Playback is paused.");
        return;
    }
    // Engine-owned session: same honest limit as /speed — the seek
    // below only moves the position; no engine applies the value.
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"position_sec", Json(std::max<int64_t>(1, pos))},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "seek", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Bass " + std::to_string(lvl) +
                   " saved, but the " + std::string(engineName(used)) +
                   " engine cannot apply audio filters — the stream "
                   "restarted at the current position unchanged. "
                   "It applies to core-voice playback (replied files).");
}

void Dispatcher::onMute(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    bool mute = ctx.command == "mute";
    int64_t chat = vcTarget(ctx);
    int cur = chatIntField(chat, "muted", 0);
    if ((cur != 0) == mute) {
        reply(ctx, mute ? "Already muted." : "Already unmuted.");
        return;
    }
    // Mute reaches the voice session the core backend itself holds
    // (/join, replied-file playback) through the backend's mute() /
    // unmute() — not a stored flag. A session owned by a vc engine
    // cannot be muted from the core: the engines expose no vc.mute
    // action, so claiming otherwise would be a fake success.
    std::string owner = chatField(chat, "voice_owner", "");
    if (owner != "core") {
        Json np, track;
        if (nowPlaying(chat, np, track) || callActive(chat)) {
            reply(ctx, "The " +
                           (owner.empty() ? std::string("engine") : owner) +
                           " session is playing this chat; the core cannot "
                           "mute it — the voice engines expose no vc.mute "
                           "action yet. Mute works on sessions the core "
                           "voice backend holds (/join, replied-file "
                           "playback).");
            return;
        }
        reply(ctx, "Nothing is playing.");
        return;
    }
    bool ok = mute ? voice_.mute(chat) : voice_.unmute(chat);
    if (!ok) {
        std::string why = voice_.lastError();
        if (why.empty()) why = voice_.describe();
        reply(ctx, std::string("Could not ") +
                       (mute ? "mute: " : "unmute: ") + why);
        return;
    }
    setChatField(chat, "muted", mute ? "1" : "0");
    reply(ctx, mute ? "Muted." : "Unmuted.");
}

void Dispatcher::onJoin(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    if (callActive(chat)) {
        std::string owner = chatField(chat, "voice_owner", "");
        if (owner == "core") {
            reply(ctx, "Already in the voice chat.");
        } else {
            reply(ctx, "Already in the voice chat (the " +
                           (owner.empty() ? std::string("engine") : owner) +
                           " session is playing here).");
        }
        return;
    }
    if (!voice_.available()) {
        reply(ctx, "Cannot join the voice chat: " + voice_.describe());
        return;
    }
    if (!voice_.join(chat)) {
        std::string why = voice_.lastError();
        if (why.empty()) why = voice_.describe();
        reply(ctx, "Could not join the voice chat: " + why);
        return;
    }
    setChatField(chat, "voice_owner", "core");
    setCallActive(chat, true);
    reply(ctx, "Joined the voice chat.");
}

void Dispatcher::onReplay(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"position_sec", Json(static_cast<int64_t>(1))},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "seek", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Replaying from the start.");
}

void Dispatcher::onPlayer(const CommandContext& ctx) {
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    reply(ctx, fmtNowPlaying(np, "▶ Now playing"));
}

void Dispatcher::onLyrics(const CommandContext& ctx) {
    std::string query = trim(ctx.args);
    if (query.empty()) {
        Json np, track;
        if (!nowPlaying(vcTarget(ctx), np, track)) {
            reply(ctx, "Nothing is playing. Usage: /lyrics <song>");
            return;
        }
        query = track.find("title") ? track.find("title")->asString() : "";
        if (query.empty()) {
            reply(ctx, "Could not tell what is playing.");
            return;
        }
    }
    // NetEase search -> lyrics (netease.lyrics needs a track id).
    Json sbody = Json::objectOf({
        {"query", Json(query)},
        {"limit", Json(static_cast<int64_t>(1))},
    });
    IpcResult s = ipc_.post(cfg_.netease_url, "netease/search", sbody, 20);
    if (!s.ok) {
        replyError(ctx, s);
        return;
    }
    const Json* results = s.data.find("results");
    if (!results || results->size() == 0) {
        reply(ctx, "No lyrics found for \"" + query + "\".");
        return;
    }
    const Json* first = &results->asArray()[0];
    std::string tid = first->find("id") ? first->find("id")->asString() : "";
    if (tid.empty()) {
        reply(ctx, "No lyrics found for \"" + query + "\".");
        return;
    }
    Json lbody = Json::objectOf({{"track_id", Json(tid)}});
    IpcResult l = ipc_.post(cfg_.netease_url, "netease/lyrics", lbody, 20);
    if (!l.ok) {
        replyError(ctx, l);
        return;
    }
    const Json* lyrics = l.data.find("lyrics");
    std::string text = lyrics ? lyrics->asString() : "";
    if (text.empty()) {
        reply(ctx, "No lyrics available for \"" + query + "\".");
        return;
    }
    if (text.size() > 3500) text = text.substr(0, 3500) + "\n...";
    reply(ctx, "Lyrics for " + fmtTrack(*first) + ":\n" + text);
}

// --- handlers: search / download / radio / share / engine -------------------
void Dispatcher::onSearch(const CommandContext& ctx) {
    std::string query = trim(ctx.args);
    if (query.empty()) {
        reply(ctx, "Usage: /search <query>  (prefix ne: for NetEase)");
        return;
    }
    // Optional source prefix: ne: -> NetEase, else vc-ts (all sources).
    if (query.rfind("ne:", 0) == 0 || query.rfind("netease:", 0) == 0) {
        std::string q = query.substr(query.find(':') + 1);
        Json body = Json::objectOf({
            {"query", Json(trim(q))},
            {"limit", Json(static_cast<int64_t>(5))},
        });
        IpcResult r = ipc_.post(cfg_.netease_url, "netease/search", body, 20);
        if (!r.ok) {
            replyError(ctx, r);
            return;
        }
        const Json* results = r.data.find("results");
        if (!results || results->size() == 0) {
            reply(ctx, "No results for \"" + trim(q) + "\".");
            return;
        }
        std::string out = "NetEase results for \"" + trim(q) + "\":\n";
        size_t i = 0;
        for (const auto& t : results->asArray())
            out += std::to_string(++i) + ". " + fmtTrack(t) + "\n";
        out += "Use /play <title or link> to play one.";
        reply(ctx, out);
        return;
    }
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"source", Json("all")},
        {"limit", Json(static_cast<int64_t>(5))},
    });
    IpcResult r = ipc_.post(cfg_.vc_ts_url, "search", body, 20);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        reply(ctx, "No results for \"" + query + "\".");
        return;
    }
    std::string out = "Results for \"" + query + "\":\n";
    size_t i = 0;
    for (const auto& t : results->asArray())
        out += std::to_string(++i) + ". " + fmtTrack(t) + "\n";
    out += "Use /play <title or link> to play one.";
    reply(ctx, out);
}

void Dispatcher::onDownload(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    std::string query = trim(ctx.args);
    if (query.empty()) {
        reply(ctx, "Usage: /download <song title or link>");
        return;
    }
    // Route by link type (NoxBeats links::source_of): Spotify links go
    // to download.spotify (playlist links to the playlist action), everything
    // else to download.youtube.
    std::string low = lowerStr(query);
    std::string action = "download/youtube";
    Json body;
    if (low.find("spotify.com") != std::string::npos ||
        low.rfind("spotify:", 0) == 0) {
        bool playlist = low.find("/playlist/") != std::string::npos ||
                        low.find(":playlist:") != std::string::npos;
        action = playlist ? "download/spotify-playlist" : "download/spotify";
        body = Json::objectOf({
            {"url", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
        if (playlist) body.set("mode", Json("files"));
    } else {
        body = Json::objectOf({
            {"query", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    }
    IpcResult r = ipc_.post(cfg_.dl_url, action, body, 60);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* oversize = r.data.find("oversize");
    if (oversize && oversize->asBool()) {
        reply(ctx,
              "That file is over the 50 MB Bot API cap — queued for a later "
              "MTProto send instead of failing. (See contracts/ipc.json.)");
        return;
    }
    const Json* track = r.data.find("track");
    const Json* path = r.data.find("file_path");
    const Json* count = r.data.find("count");
    std::string text = "Download ready";
    if (track) text += ": " + fmtTrack(*track);
    if (count) text += " (" + std::to_string(count->asInt()) + " files)";
    if (path) text += "\nFile: " + path->asString();
    text += "\n(Sending the file into chat is the downloaders service's job "
            "via the core; this build reports the path.)";
    reply(ctx, text);
}

void Dispatcher::onRadio(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    std::string station = trim(ctx.args);
    if (station.empty()) {
        // List stations (radio.list).
        IpcResult r = ipc_.get(cfg_.radio_url, "/v1/radio/stations", 10);
        if (!r.ok) {
            replyError(ctx, r);
            return;
        }
        const Json* stations = r.data.find("stations");
        std::string out = "Radio stations:\n";
        if (stations) {
            for (const auto& s : stations->asArray())
                out += "- " + s.asString() + "\n";
        }
        out += "Start one with /radio <station>.";
        reply(ctx, out);
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"station", Json(station)},
    });
    IpcResult r = ipc_.post(cfg_.radio_url, "radio/start", body, 15);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* url = r.data.find("stream_url");
    if (!url || url->asString().empty()) {
        reply(ctx, "Radio service returned no stream URL.");
        return;
    }
    Json play = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"query", Json(url->asString())},
        {"requested_by", Json(ctx.message.from.id)},
    });
    Engine used;
    bool fell_back = false;
    IpcResult pr = vcCall(ctx, "play", play, used, fell_back);
    if (!pr.ok) {
        replyError(ctx, pr);
        return;
    }
    setCallActive(vcTarget(ctx), true);
    setChatField(vcTarget(ctx), "voice_owner", engineName(used));
    reply(ctx, "📻 Radio: " + station + " is now playing in the voice chat.");
}

void Dispatcher::onShare(const CommandContext& ctx) {
    if (trim(ctx.args).empty()) {
        reply(ctx, "Usage: /share <song title or link>");
        return;
    }
    Json track = Json::objectOf({
        {"id", Json("query:" + trim(ctx.args))},
        {"title", Json(trim(ctx.args))},
        {"source", Json("youtube")},
    });
    Json body = Json::objectOf({
        {"created_by", Json(ctx.message.from.id)},
        {"track", track},
    });
    IpcResult r = ipc_.post(cfg_.share_url, "share", body, 15);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* link = r.data.find("link");
    const Json* token = r.data.find("token");
    std::string text = "Anonymous share link created:\n";
    if (link) text += link->asString() + "\n";
    if (token) text += "Token: " + token->asString() + "\n";
    text += "(No identity is attached to the link.)";
    reply(ctx, text);
}

void Dispatcher::onEngine(const CommandContext& ctx) {
    if (!isPrivate(ctx) &&
        !isChatAdmin(ctx.message.chat.id, ctx.message.from.id)) {
        reply(ctx, "Only group admins (or the bot owner) can switch engines.");
        return;
    }
    std::string arg = lowerStr(trim(ctx.args));
    if (arg.empty()) {
        reply(ctx, std::string("Current VC engine for this chat: ") +
                       engineName(router_.engineFor(vcTarget(ctx))) +
                       "\nSwitch with /engine go|ts|rust");
        return;
    }
    Engine e;
    if (!engineFromName(arg, e)) {
        reply(ctx, "Unknown engine \"" + arg + "\". Use go, ts or rust.");
        return;
    }
    router_.setEngine(vcTarget(ctx), e);
    reply(ctx, std::string("VC engine for this chat is now ") + engineName(e) +
                   " (fallback: " + engineName(fallbackOrder(e)[1]) + " → " +
                   engineName(fallbackOrder(e)[2]) + ").");
}

// --- handlers: playlists -----------------------------------------------------
namespace {
constexpr size_t kPlaylistMax = 100;  // NoxBeats DB::kPlaylistMax

Json playlistOf(Store& store, int64_t user) {
    auto v = store.get("playlist:" + std::to_string(user));
    if (!v) return Json(Json::Array{});
    try {
        return Json::parse(*v);
    } catch (...) {
        return Json(Json::Array{});
    }
}
void savePlaylist(Store& store, int64_t user, const Json& list) {
    store.set("playlist:" + std::to_string(user), list.dump());
}
}  // namespace

void Dispatcher::onAddPlaylist(const CommandContext& ctx) {
    int64_t user = ctx.message.from.id;
    std::string query = trim(ctx.args);
    Json list = playlistOf(store_, user);
    if (query.empty()) {
        // No argument: add the song playing now (group only).
        if (isPrivate(ctx)) {
            reply(ctx, "Usage: /addplaylist <song name or link>");
            return;
        }
        Json np, track;
        if (!nowPlaying(vcTarget(ctx), np, track)) {
            reply(ctx, "Nothing is playing. Usage: /addplaylist <song>");
            return;
        }
        std::string id = track.find("id") ? track.find("id")->asString() : "";
        if (id.empty()) {
            reply(ctx, "That track cannot be saved (no id).");
            return;
        }
        for (const auto& item : list.asArray()) {
            const Json* iid = item.find("id");
            if (iid && iid->asString() == id) {
                reply(ctx, "Already in your playlist.");
                return;
            }
        }
        if (list.size() >= kPlaylistMax) {
            reply(ctx, "Your playlist is full (" +
                           std::to_string(kPlaylistMax) + " songs).");
            return;
        }
        Json::Array arr = list.asArray();
        arr.push_back(track);
        savePlaylist(store_, user, Json(arr));
        reply(ctx, "Added to your playlist (" +
                       std::to_string(arr.size()) + " songs).");
        return;
    }
    // Resolve the query to a track via vc-ts search, then save it.
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"source", Json("all")},
        {"limit", Json(static_cast<int64_t>(1))},
    });
    IpcResult r = ipc_.post(cfg_.vc_ts_url, "search", body, 20);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        reply(ctx, "No results for \"" + query + "\".");
        return;
    }
    Json track = results->asArray()[0];
    std::string id = track.find("id") ? track.find("id")->asString() : "";
    for (const auto& item : list.asArray()) {
        const Json* iid = item.find("id");
        if (iid && !id.empty() && iid->asString() == id) {
            reply(ctx, "Already in your playlist.");
            return;
        }
    }
    if (list.size() >= kPlaylistMax) {
        reply(ctx, "Your playlist is full (" + std::to_string(kPlaylistMax) +
                       " songs).");
        return;
    }
    Json::Array arr = list.asArray();
    arr.push_back(track);
    savePlaylist(store_, user, Json(arr));
    reply(ctx, "Added \"" + fmtTrack(track) + "\" to your playlist (" +
                   std::to_string(arr.size()) + " songs).");
}

void Dispatcher::onPlaylist(const CommandContext& ctx) {
    Json list = playlistOf(store_, ctx.message.from.id);
    if (list.size() == 0) {
        reply(ctx, "Your playlist is empty. Add songs with /addplaylist.");
        return;
    }
    std::string text = "Your playlist (" + std::to_string(list.size()) +
                       " songs):\n";
    size_t i = 0;
    for (const auto& item : list.asArray()) {
        std::string line = std::to_string(++i) + ". " + fmtTrack(item) + "\n";
        if (text.size() + line.size() > 3800) break;
        text += line;
    }
    if (i < list.size())
        text += "... and " + std::to_string(list.size() - i) + " more.\n";
    text += "Play it with /playplaylist.";
    reply(ctx, text);
}

void Dispatcher::onRmPlaylist(const CommandContext& ctx) {
    int64_t user = ctx.message.from.id;
    std::string arg = lowerStr(trim(ctx.args));
    Json list = playlistOf(store_, user);
    bool all = ctx.command == "clearplaylist" || arg == "all";
    if (all) {
        size_t n = list.size();
        savePlaylist(store_, user, Json(Json::Array{}));
        reply(ctx, n ? "Cleared your playlist (" + std::to_string(n) +
                           " songs removed)."
                     : "Your playlist is already empty.");
        return;
    }
    if (arg.empty()) {
        reply(ctx, "Usage: /rmplaylist <number | song name | all>");
        return;
    }
    if (list.size() == 0) {
        reply(ctx, "Your playlist is empty.");
        return;
    }
    size_t pos = 0;
    if (isDigits(arg) && arg.size() <= 4) {
        pos = std::stoul(arg);
    } else {
        for (size_t i = 0; i < list.size(); ++i) {
            const Json* title = list.asArray()[i].find("title");
            if (title && lowerStr(title->asString()).find(arg) !=
                             std::string::npos) {
                pos = i + 1;
                break;
            }
        }
    }
    if (pos < 1 || pos > list.size()) {
        reply(ctx, "No matching song in your playlist.");
        return;
    }
    Json::Array arr = list.asArray();
    std::string title = arr[pos - 1].find("title")
                            ? arr[pos - 1].find("title")->asString()
                            : "?";
    arr.erase(arr.begin() + (pos - 1));
    savePlaylist(store_, user, Json(arr));
    reply(ctx, "Removed \"" + title + "\" (" + std::to_string(arr.size()) +
                   " songs left).");
}

void Dispatcher::onPlayPlaylist(const CommandContext& ctx) {
    if (!isGroup(ctx)) {
        reply(ctx, "Music only plays in groups (supergroups).");
        return;
    }
    if (!playAllowed(ctx, false)) return;
    Json list = playlistOf(store_, ctx.message.from.id);
    if (list.size() == 0) {
        reply(ctx, "Your playlist is empty. Add songs with /addplaylist.");
        return;
    }
    auto words = splitArgs(ctx.args);
    bool shuffle = std::find(words.begin(), words.end(), "-s") != words.end();
    std::vector<Json> tracks;
    for (const auto& item : list.asArray()) tracks.push_back(item);
    if (shuffle) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::shuffle(tracks.begin(), tracks.end(), rng);
    }
    if (static_cast<int>(tracks.size()) > cfg_.queue_limit)
        tracks.resize(static_cast<size_t>(cfg_.queue_limit));
    // First track via /play, the rest queued (NoxBeats playlist_to_queue).
    Json first = tracks[0];
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"query", Json(first.find("title") ? first.find("title")->asString() : "")},
        {"requested_by", Json(ctx.message.from.id)},
    });
    Engine used;
    bool fb = false;
    IpcResult r = vcCall(ctx, "play", body, used, fb);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    setCallActive(vcTarget(ctx), true);
    setChatField(vcTarget(ctx), "voice_owner", engineName(used));
    int queued = 0;
    for (size_t i = 1; i < tracks.size(); ++i) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", tracks[i]},
            {"requested_by", Json(ctx.message.from.id)},
        });
        Engine u2;
        bool fb2 = false;
        IpcResult ar = vcCall(ctx, "queue", add, u2, fb2);
        if (!ar.ok) break;
        ++queued;
    }
    reply(ctx, "Playing your playlist: " + fmtTrack(first) +
                   (queued ? " (+" + std::to_string(queued) + " queued)" : ""));
}

// --- handlers: settings / chat settings / autoplay --------------------------
void Dispatcher::onPlaymode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (ctx.command == "playmode" && !args.empty()) {
        std::string a = lowerStr(args[0]);
        bool admins = a == "admin" || a == "admins" || a == "adminonly";
        bool all = a == "everyone" || a == "all";
        if (!admins && !all) {
            reply(ctx, "Usage: /playmode admin|everyone (current: " +
                           chatField(chat, "play_mode", "everyone") + ")");
            return;
        }
        if (!adminCheck(ctx)) return;
        setChatField(chat, "play_mode", admins ? "admin" : "everyone");
        reply(ctx, std::string("Play mode: ") +
                       (admins ? "admins only" : "everyone") + ".");
        return;
    }
    // /settings: show the current settings panel as text.
    std::string out = "Settings for this chat:\n";
    out += "Play mode: " + chatField(chat, "play_mode", "everyone") + "\n";
    out += "Admin mode: ";
    int am = chatIntField(chat, "admin_mode", 0);
    out += am == 1 ? "admins only" : am == 2 ? "everyone" : "admins + authorized";
    out += "\nCmd delete: " + chatField(chat, "cmd_delete", "off") + "\n";
    out += "Clean mode: ";
    int cm = chatIntField(chat, "clean_mode", 0);
    out += cm ? std::to_string(cm) + " min" : "off";
    out += "\nThumbnails: " + chatField(chat, "thumb", "on") + "\n";
    out += "Autoplay: " + chatField(chat, "autoplay", "off") + " (" +
           chatField(chat, "ap_lang", "auto") + "/" +
           chatField(chat, "ap_mood", "any") + ")\n";
    out += "Language: " + chatField(chat, "lang", "en") + "\n";
    out += "VC engine: " + std::string(engineName(router_.engineFor(chat)));
    reply(ctx, out);
}

void Dispatcher::onLang(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    int64_t chat = vcTarget(ctx);
    if (args.empty()) {
        reply(ctx, "Current language: " + chatField(chat, "lang", "en") +
                       ". This build ships English only; /lang <code> "
                       "stores your choice for when more locales land.");
        return;
    }
    std::string code = lowerStr(args[0]);
    if (code != "en") {
        reply(ctx, "Language \"" + code +
                       "\" is not shipped in this build (English only). "
                       "Your choice was not stored.");
        return;
    }
    if (!adminCheck(ctx)) return;
    setChatField(chat, "lang", code);
    reply(ctx, "Language set to " + code + ".");
}

void Dispatcher::onAdminmode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        int mode = chatIntField(chat, "admin_mode", 0);
        reply(ctx, std::string("Admin mode: ") +
                       (mode == 1 ? "admins only"
                                  : mode == 2 ? "everyone"
                                              : "admins + authorized") +
                       ". Usage: /adminmode admin|adminauth|everyone");
        return;
    }
    if (!adminCheck(ctx)) return;
    std::string a = lowerStr(args[0]);
    int mode = -1;
    if (a == "admin" || a == "admins" || a == "adminonly") mode = 1;
    else if (a == "adminauth" || a == "auth" || a == "dj") mode = 0;
    else if (a == "everyone" || a == "all") mode = 2;
    if (mode < 0) {
        reply(ctx, "Usage: /adminmode admin|adminauth|everyone");
        return;
    }
    setChatField(chat, "admin_mode", std::to_string(mode));
    reply(ctx, std::string("Admin mode: ") +
                   (mode == 1 ? "admins only"
                              : mode == 2 ? "everyone"
                                          : "admins + authorized") +
                   ".");
}

void Dispatcher::onCmdDelete(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty() || (!flagOn(args[0]) && !flagOff(args[0]))) {
        reply(ctx, "Cmd delete is " + chatField(chat, "cmd_delete", "off") +
                       ". Usage: /cmddelete on|off");
        return;
    }
    if (!adminCheck(ctx)) return;
    setChatField(chat, "cmd_delete", flagOn(args[0]) ? "on" : "off");
    reply(ctx, std::string("Cmd delete: ") +
                   (flagOn(args[0]) ? "on" : "off") + ".");
}

void Dispatcher::onCleanmode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    int cur = chatIntField(chat, "clean_mode", 0);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Clean mode: " + std::string(cur ? "on (" +
                       std::to_string(cur) + " min)" : "off") +
                       ". Usage: /cleanmode on|off|<minutes 1-1440>");
        return;
    }
    std::string a = lowerStr(args[0]);
    int minutes = 0;
    bool off = flagOff(a);
    if (flagOn(a)) minutes = cur ? cur : 15;
    else if (!off && isDigits(a)) minutes = std::stoi(a);
    if (!off && (minutes < 1 || minutes > 1440)) {
        reply(ctx, "Usage: /cleanmode on|off|<minutes 1-1440>");
        return;
    }
    if (!adminCheck(ctx)) return;
    if (off) {
        setChatField(chat, "clean_mode", "0");
        reply(ctx, "Clean mode off.");
        return;
    }
    setChatField(chat, "clean_mode", std::to_string(minutes));
    reply(ctx, "Clean mode on: bot messages are deleted after " +
                   std::to_string(minutes) + " minute(s).");
}

void Dispatcher::onThumb(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    bool is_nothumb = ctx.command == "nothumb" || ctx.command == "nothumbs";
    auto args = splitArgs(ctx.args);
    if (args.empty() || (!flagOn(args[0]) && !flagOff(args[0]))) {
        bool on = chatField(chat, "thumb", "on") == "on";
        reply(ctx, std::string("Thumbnails are ") + (on ? "on" : "off") +
                       ". Usage: /" + ctx.command + " on|off");
        return;
    }
    if (!canManageVc(ctx)) return;
    // /nothumb on -> thumbnails off; /thumbnail on -> thumbnails on.
    bool thumb_on = is_nothumb ? flagOff(args[0]) : flagOn(args[0]);
    setChatField(chat, "thumb", thumb_on ? "on" : "off");
    reply(ctx, thumb_on ? "Thumbnails enabled." : "Thumbnails disabled.");
}

void Dispatcher::onAutoplay(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    static const std::vector<std::string> langs = {"auto", "hindi", "english",
                                                   "punjabi", "tamil",
                                                   "telugu", "bengali",
                                                   "marathi"};
    static const std::vector<std::string> moods = {
        "any", "happy", "sad", "romantic", "party", "chill", "devotional"};
    if (args.empty()) {
        reply(ctx, "Autoplay: " + chatField(chat, "autoplay", "off") +
                       " (lang " + chatField(chat, "ap_lang", "auto") +
                       ", mood " + chatField(chat, "ap_mood", "any") +
                       "). Usage: /autoplay on|off|lang <lang>|mood "
                       "<mood>");
        return;
    }
    if (!canManageVc(ctx)) return;
    std::string a = lowerStr(args[0]);
    if (a == "on" || a == "enable" || a == "start") {
        setChatField(chat, "autoplay", "on");
        store_.set("autoplay:" + std::to_string(chat), "1");
        reply(ctx, "Autoplay on: when the queue drains, a related track "
                   "is queued automatically.");
    } else if (a == "off" || a == "disable" || a == "stop") {
        setChatField(chat, "autoplay", "off");
        store_.set("autoplay:" + std::to_string(chat), "0");
        reply(ctx, "Autoplay off.");
    } else if ((a == "lang" || a == "language") && args.size() >= 2 &&
               std::find(langs.begin(), langs.end(), lowerStr(args[1])) !=
                   langs.end()) {
        setChatField(chat, "ap_lang", lowerStr(args[1]));
        reply(ctx, "Autoplay language: " + lowerStr(args[1]) + ".");
    } else if (a == "mood" && args.size() >= 2 &&
               std::find(moods.begin(), moods.end(), lowerStr(args[1])) !=
                   moods.end()) {
        setChatField(chat, "ap_mood", lowerStr(args[1]));
        reply(ctx, "Autoplay mood: " + lowerStr(args[1]) + ".");
    } else {
        reply(ctx, "Usage: /autoplay on|off|lang <lang>|mood <mood>");
    }
}

void Dispatcher::onChannelplay(const CommandContext& ctx) {
    int64_t chat = ctx.message.chat.id;
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        int64_t cur = 0;
        try {
            cur = std::stoll(chatField(chat, "cplay", "0"));
        } catch (...) {
        }
        reply(ctx, cur ? "Linked channel: " + std::to_string(cur) +
                             ". Use /channelplay disable to unlink."
                       : "Usage: /channelplay <@channel | channel id | "
                         "disable>");
        return;
    }
    if (!adminCheck(ctx)) return;
    std::string a = lowerStr(args[0]);
    if (a == "disable" || a == "off" || a == "none") {
        setChatField(chat, "cplay", "0");
        reply(ctx, "Channel play disabled.");
        return;
    }
    int64_t ch = 0;
    if (!args[0].empty() && args[0][0] == '@') {
        ch = tg_.getChatId(args[0]);
        if (ch == 0) {
            reply(ctx, "Cannot resolve that channel.");
            return;
        }
    } else {
        try {
            ch = std::stoll(args[0]);
        } catch (...) {
            reply(ctx, "Invalid channel id.");
            return;
        }
    }
    if (ch >= 0 || ch == chat) {
        reply(ctx, "Invalid channel id.");
        return;
    }
    // The bot must be an admin of the channel, and so must the linker.
    auto admins = tg_.getChatAdministrators(ch);
    if (admins.empty()) {
        reply(ctx, "Cannot access that channel (bot must be an admin).");
        return;
    }
    int64_t me = bot_id_;
    bool bot_admin =
        std::find(admins.begin(), admins.end(), me) != admins.end();
    bool user_admin =
        std::find(admins.begin(), admins.end(), ctx.message.from.id) !=
        admins.end();
    if (!bot_admin) {
        reply(ctx, "The bot must be an admin of that channel.");
        return;
    }
    if (!isSudo(ctx.message.from.id) && !user_admin) {
        reply(ctx, "You must be an admin of that channel to link it.");
        return;
    }
    setChatField(chat, "cplay", std::to_string(ch));
    reply(ctx, "Channel play linked to " + std::to_string(ch) +
                   ". Use /cplay, /cskip, ... to drive that channel's "
                   "voice chat.");
}

void Dispatcher::onUserbot(const CommandContext& ctx) {
    int64_t chat = ctx.message.chat.id;
    int64_t target = 0;
    if (ctx.via_channel) {
        try {
            target = std::stoll(chatField(chat, "cplay", "0"));
        } catch (...) {
        }
        if (!target) {
            reply(ctx, "No channel linked. Use /channelplay first.");
            return;
        }
    }
    if (callActive(chat)) {
        reply(ctx, "The assistant is already in a call here.");
        return;
    }
    int64_t vc = target ? target : chat;
    // If assistant ids are configured, check membership first.
    if (!cfg_.assistant_ids.empty()) {
        int64_t ub = *cfg_.assistant_ids.begin();
        std::string st = tg_.getChatMemberStatus(vc, ub);
        if (st == "member" || st == "administrator" || st == "creator") {
            reply(ctx, "The assistant is already in this chat.");
            return;
        }
    }
    if (!voice_.available()) {
        reply(ctx, "The assistant cannot join from here: " +
                       voice_.describe() +
                       ". Add the assistant user account to the chat "
                       "manually, or configure TD_API_ID/TD_API_HASH/"
                       "TD_SESSION_DIR for the TDLib voice backend.");
        return;
    }
    // The Telethon voice bridge that used to join via an invite link
    // was deleted in Phase 5 (user order: ntgcalls is now a native
    // library linked in-process). There is no in-process path that
    // adds the assistant account to a chat anymore — it must already
    // be a member. /join (vc.join) puts the assistant's voice session
    // into the call once it is a member.
    reply(ctx, "The assistant is not in this chat yet. Add the assistant "
               "user account to the chat manually, then use /join to put "
               "it in the voice call.");
}

// --- handlers: admin / sudo / broadcast / tools --------------------------------
void Dispatcher::onAuth(const CommandContext& ctx) {
    if (!adminCheck(ctx)) return;
    auto user = extractUserId(ctx);
    if (!user) {
        reply(ctx, "Reply to a user or give a user id/@username.");
        return;
    }
    int64_t chat = vcTarget(ctx);
    if (ctx.command == "auth") {
        if (isChatAdmin(chat, *user)) {
            reply(ctx, "That user is already an admin.");
            return;
        }
        store_.sadd("auth:" + std::to_string(chat), std::to_string(*user));
        reply(ctx, "Authorized user " + std::to_string(*user) +
                       " to control playback here.");
    } else {
        store_.srem("auth:" + std::to_string(chat), std::to_string(*user));
        reply(ctx, "Removed authorization for user " + std::to_string(*user) +
                       ".");
    }
}

void Dispatcher::onAuthlist(const CommandContext& ctx) {
    if (!adminCheck(ctx)) return;
    auto auth = store_.smembers("auth:" + std::to_string(vcTarget(ctx)));
    if (auth.empty()) {
        reply(ctx, "No authorized users in this chat.");
        return;
    }
    std::string out = "Authorized users:\n";
    int i = 1;
    for (const auto& u : auth) out += std::to_string(i++) + ". " + u + "\n";
    reply(ctx, out);
}

void Dispatcher::onAdmincache(const CommandContext& ctx) {
    static std::mutex mu;
    static std::map<int64_t, std::chrono::steady_clock::time_point> hist;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = hist.find(ctx.message.from.id);
        auto now = std::chrono::steady_clock::now();
        if (it != hist.end() && now < it->second) {
            reply(ctx, "Admin cache was refreshed recently. Wait 10 minutes.");
            return;
        }
        hist[ctx.message.from.id] = now + std::chrono::minutes(10);
    }
    cachedAdmins(ctx.message.chat.id, true);
    reply(ctx, "Admin cache refreshed.");
}

void Dispatcher::onBlacklist(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Usage: /blacklist <user id | @username | chat id>");
        return;
    }
    int64_t id = 0;
    const std::string& a = args[0];
    if (!a.empty() && a[0] == '@') {
        id = tg_.getChatId(a);
        if (id == 0) {
            reply(ctx, "Cannot resolve that user/chat.");
            return;
        }
    } else {
        try {
            id = std::stoll(a);
        } catch (...) {
            reply(ctx, "Invalid id.");
            return;
        }
    }
    bool is_chat = std::to_string(id).rfind("-100", 0) == 0;
    if (ctx.command == "blacklist") {
        if (isBlChat(id) || isBlUser(id)) {
            reply(ctx, "Already blacklisted.");
            return;
        }
        if (!is_chat) store_.sadd("bl_users", std::to_string(id));
        store_.sadd("bl_chats", std::to_string(id));
        reply(ctx, "Blacklisted " + std::to_string(id) + ".");
    } else {
        if (!isBlChat(id) && !isBlUser(id)) {
            reply(ctx, "Not blacklisted.");
            return;
        }
        if (!is_chat) store_.srem("bl_users", std::to_string(id));
        store_.srem("bl_chats", std::to_string(id));
        reply(ctx, "Removed " + std::to_string(id) + " from the blacklist.");
    }
}

void Dispatcher::onBlacklisted(const CommandContext& ctx) {
    auto chats = store_.smembers("bl_chats");
    auto users = store_.smembers("bl_users");
    std::string out = "Blacklisted chats (" + std::to_string(chats.size()) +
                      ") and users (" + std::to_string(users.size()) + "):\n";
    for (const auto& c : chats) out += "chat " + c + "\n";
    for (const auto& u : users) out += "user " + u + "\n";
    if (out.size() > 3500) out = out.substr(0, 3500) + "\n...";
    reply(ctx, out);
}

void Dispatcher::onAddSudo(const CommandContext& ctx) {
    auto user = extractUserId(ctx);
    if (!user) {
        reply(ctx, "Reply to a user or give a user id/@username.");
        return;
    }
    if (ctx.command == "addsudo") {
        if (isSudo(*user)) {
            reply(ctx, "That user is already a sudo user.");
            return;
        }
        store_.sadd("sudoers", std::to_string(*user));
        reply(ctx, "Added sudo user " + std::to_string(*user) + ".");
    } else {
        if (!isSudo(*user) || isOwner(*user)) {
            reply(ctx, "That user is not a sudo user (or is the owner).");
            return;
        }
        store_.srem("sudoers", std::to_string(*user));
        reply(ctx, "Removed sudo user " + std::to_string(*user) + ".");
    }
}

void Dispatcher::onListSudo(const CommandContext& ctx) {
    std::string out = "Owner: ";
    out += cfg_.owner_id ? std::to_string(cfg_.owner_id) : "(unset)";
    out += "\n";
    auto sudoers = store_.smembers("sudoers");
    if (!sudoers.empty()) {
        out += "Sudo users:\n";
        for (const auto& s : sudoers) out += "- " + s + "\n";
    }
    reply(ctx, out);
}

void Dispatcher::onActiveVc(const CommandContext& ctx) {
    auto keys = store_.keys("call:");
    if (keys.empty()) {
        reply(ctx, "No active voice chats.");
        return;
    }
    if (ctx.command == "ac") {
        reply(ctx, "Active voice chats: " + std::to_string(keys.size()) + ".");
        return;
    }
    std::string out = "Active voice chats:\n";
    int i = 1;
    for (const auto& key : keys) {
        int64_t chat = 0;
        try {
            chat = std::stoll(key.substr(5));
        } catch (...) {
            continue;
        }
        Json np, track;
        std::string title = "-";
        if (nowPlaying(chat, np, track) && track.find("title"))
            title = track.find("title")->asString();
        out += std::to_string(i++) + ". " + std::to_string(chat) + " — " +
               title + "\n";
        if (out.size() > 3500) break;
    }
    reply(ctx, out);
}

void Dispatcher::onBroadcast(const CommandContext& ctx) {
    if (!ctx.message.has_reply_to) {
        reply(ctx, "Reply to a message to broadcast it.");
        return;
    }
    auto words = splitArgs(ctx.args);
    bool copy = std::find(words.begin(), words.end(), "-copy") != words.end();
    bool nochat =
        std::find(words.begin(), words.end(), "-nochat") != words.end();
    bool to_users =
        std::find(words.begin(), words.end(), "-user") != words.end();
    std::set<int64_t> targets;
    if (!nochat) {
        for (const auto& c : store_.smembers("chats")) {
            try {
                targets.insert(std::stoll(c));
            } catch (...) {
            }
        }
    }
    if (to_users) {
        for (const auto& u : store_.smembers("users")) {
            try {
                targets.insert(std::stoll(u));
            } catch (...) {
            }
        }
    }
    int64_t sent_id = reply(ctx, "Broadcasting to " +
                                     std::to_string(targets.size()) +
                                     " chats/users...");
    int count = 0, failed = 0;
    // One id per line, after a header that says what the failure
    // was: the Bot API forward/copy calls return only
    // success/failure, so no per-chat error text exists to list —
    // each id below is a chat that rejected the broadcast.
    std::string failures =
        "# chats that rejected the broadcast (forward/copy failed; "
        "the Bot API returns no per-chat error text)\n";
    for (int64_t target : targets) {
        bool ok = false;
        if (copy)
            ok = tg_.copyMessage(target, ctx.message.chat.id,
                                 ctx.message.reply_to_message_id);
        else
            ok = tg_.forwardMessage(target, ctx.message.chat.id,
                                    ctx.message.reply_to_message_id);
        if (ok) {
            ++count;
        } else {
            ++failed;
            failures += std::to_string(target) + "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::string text = "Broadcast done: " + std::to_string(count) +
                       " delivered, " + std::to_string(failed) + " failed.";
    if (failed && sent_id > 0) {
        // Failures go out as an errors.txt attachment (NoxBeats
        // behaviour), not inline: a big fan-out can fail on hundreds
        // of chats and the inline list was truncated at 3000 chars.
        // If the upload itself fails, fall back to the inline list
        // rather than silently losing the ids.
        std::string doc_err;
        std::string caption =
            "Broadcast failures (" + std::to_string(failed) + " chats):";
        if (tg_.sendDocument(ctx.message.chat.id, "errors.txt", failures,
                             caption, &doc_err) < 0) {
            text += "\nFailed ids (errors.txt upload failed: " + doc_err +
                    "):\n" + failures.substr(0, 3000);
        }
    }
    if (sent_id > 0)
        tg_.sendMessageId(ctx.message.chat.id, text);
    else
        reply(ctx, text);
}

void Dispatcher::onMaintenance(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        auto reason = store_.get("maint_reason");
        reply(ctx, std::string("Maintenance is ") +
                       (maintenanceOn() ? "ON" : "OFF") +
                       (reason && !reason->empty() ? " (" + *reason + ")"
                                                    : "") +
                       ". Usage: /maintenance on|off [reason]");
        return;
    }
    if (flagOff(args[0])) {
        store_.set("maintenance", "0");
        store_.set("maint_reason", "");
        reply(ctx, "Maintenance off.");
    } else if (flagOn(args[0])) {
        std::string reason;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!reason.empty()) reason += " ";
            reason += args[i];
        }
        store_.set("maintenance", "1");
        store_.set("maint_reason", reason);
        reply(ctx, "Maintenance on." +
                       (reason.empty() ? "" : " Reason: " + reason));
    } else {
        reply(ctx, "Usage: /maintenance on|off [reason]");
    }
}

void Dispatcher::onAutoleave(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    auto on = store_.get("autoleave");
    if (args.empty() || (!flagOn(args[0]) && !flagOff(args[0]))) {
        reply(ctx, std::string("Autoleave is ") +
                       ((on && *on == "1") ? "ON" : "OFF") +
                       ". Usage: /autoleave on|off");
        return;
    }
    bool want = flagOn(args[0]);
    store_.set("autoleave", want ? "1" : "0");
    reply(ctx, std::string("Autoleave ") + (want ? "ON" : "OFF") +
                   ". (Enforced by the voice services: when a voice chat "
                   "empties, the assistant leaves.)");
}

void Dispatcher::onJson(const CommandContext& ctx) {
    const TgMessage& m = ctx.message;
    Json j = Json::objectOf({
        {"message_id", Json(m.message_id)},
        {"chat", Json::objectOf({
                     {"id", Json(m.chat.id)},
                     {"title", Json(m.chat.title)},
                     {"type", Json(m.chat.type)},
                 })},
        {"from", Json::objectOf({
                     {"id", Json(m.from.id)},
                     {"first_name", Json(m.from.first_name)},
                     {"username", Json(m.from.username)},
                     {"is_bot", Json(m.from.is_bot)},
                 })},
    });
    if (m.has_text) j.set("text", Json(m.text));
    if (m.has_reply_to) {
        j.set("reply_to_message_id", Json(m.reply_to_message_id));
        j.set("reply_from_id", Json(m.reply_from_id));
    }
    std::string out = j.dump();
    if (out.size() > 3500) out = out.substr(0, 3500) + "\n...";
    reply(ctx, out);
}

double Dispatcher::speedMbps(size_t bytes, double secs) {
    if (secs <= 0) return 0;
    return static_cast<double>(bytes) * 8.0 / (secs * 1e6);
}

std::string Dispatcher::formatSpeedtest(double down_mbps, bool up_ok,
                                        double up_mbps, int up_status,
                                        const std::string& up_error) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", down_mbps);
    std::string out = std::string("Speed test:\nDownload: ") + b + " Mbps";
    if (!up_ok) {
        out += "\nUpload probe failed (HTTP " + std::to_string(up_status) +
               (up_error.empty() ? "" : ": " + up_error) + ").";
    } else {
        std::snprintf(b, sizeof b, "%.2f", up_mbps);
        out += std::string("\nUpload: ") + b + " Mbps";
    }
    return out;
}

void Dispatcher::onSpeedtest(const CommandContext& ctx) {
    reply(ctx, "Running speed test...");
    // Cloudflare speed endpoints (NoxBeats bot_tools.cpp::run_speedtest):
    // download probe (25 MB) + upload probe (4 MB POST to __up, the
    // endpoint Cloudflare's own speed test uses). Each probe is timed
    // separately and bounded — the upload is 4 MB / 30 s so a slow
    // uplink never hangs the chat; an upload failure is reported
    // honestly next to the download number instead of failing the
    // whole test.
    auto t0 = std::chrono::steady_clock::now();
    HttpResult down = HttpClient::get(
        "https://speed.cloudflare.com/__down?bytes=25000000", {}, 40);
    double down_secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (!down.ok() || down.body.empty()) {
        reply(ctx, "Speed test failed (download probe error).");
        return;
    }
    double down_mbps = speedMbps(down.body.size(), down_secs);
    auto u0 = std::chrono::steady_clock::now();
    std::string up_body(4 * 1024 * 1024, '\0');
    HttpResult up = HttpClient::postRaw(
        "https://speed.cloudflare.com/__up", up_body,
        "application/octet-stream", {}, 30);
    double up_secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - u0)
            .count();
    reply(ctx, formatSpeedtest(down_mbps, up.ok(),
                               speedMbps(up_body.size(), up_secs), up.status,
                               up.error));
}

void Dispatcher::onBug(const CommandContext& ctx) {
    std::string text = trim(ctx.args);
    if (text.empty()) {
        reply(ctx, "Usage: /bug <describe the problem>");
        return;
    }
    if (text.size() > 1500) text.resize(1500);
    if (cfg_.logger_id == 0 && cfg_.owner_id == 0) {
        reply(ctx, "No log chat / owner is configured to receive bug "
                   "reports.");
        return;
    }
    int64_t dest = cfg_.logger_id ? cfg_.logger_id : cfg_.owner_id;
    std::string err;
    std::string report = "Bug report from user " +
                         std::to_string(ctx.message.from.id) + " in chat " +
                         std::to_string(ctx.message.chat.id) + ":\n" + text;
    if (tg_.sendMessage(dest, report, err)) {
        reply(ctx, "Bug report sent. Thanks!");
    } else {
        reply(ctx, "Could not send the bug report: " + err);
    }
}

void Dispatcher::onPrivacy(const CommandContext& ctx) {
    reply(ctx,
          "Parlay privacy: the bot stores per-chat settings (engine, "
          "play mode, admin mode), authorized users, playlists you save "
          "with /addplaylist, and the list of chats/users it serves "
          "(for /broadcast). No audio you play is kept after the stream "
          "ends. Use /bug to report a privacy concern.");
}

void Dispatcher::onPing(const CommandContext& ctx) {
    auto t0 = std::chrono::steady_clock::now();
    int64_t up = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - boot_time_)
            .count());
    double latency =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count();
    // /proc-based stats (NoxBeats tools.cpp: cpu_percent, mem_total_gb,
    // disk, rss_mb).
    double cpu = 0;
    {
        std::ifstream f("/proc/stat");
        std::string k;
        unsigned long long v[8] = {0};
        f >> k;
        for (auto& x : v) f >> x;
        unsigned long long idle = v[3] + v[4], total = 0;
        for (auto x : v) total += x;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::ifstream f2("/proc/stat");
        f2 >> k;
        for (auto& x : v) f2 >> x;
        unsigned long long idle2 = v[3] + v[4], total2 = 0;
        for (auto x : v) total2 += x;
        double dt = static_cast<double>(total2 - total);
        if (dt > 0) cpu = (1.0 - static_cast<double>(idle2 - idle) / dt) * 100.0;
    }
    double mem_used_pct = 0, mem_total_gb = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string k, unit;
        double total = 0, avail = 0, v = 0;
        while (f >> k >> v >> unit) {
            if (k == "MemTotal:") total = v;
            if (k == "MemAvailable:") avail = v;
        }
        if (total > 0) {
            mem_total_gb = total / (1024.0 * 1024.0);
            mem_used_pct = (1.0 - avail / total) * 100.0;
        }
    }
    double disk_pct = 0;
    {
        struct statvfs s {};
        if (statvfs("/", &s) == 0) {
            double total = static_cast<double>(s.f_blocks) * s.f_frsize;
            double free = static_cast<double>(s.f_bfree) * s.f_frsize;
            if (total > 0) disk_pct = (total - free) / total * 100.0;
        }
    }
    double rss_mb = 0;
    {
        std::ifstream f("/proc/self/statm");
        long pages = 0, rss = 0;
        f >> pages >> rss;
        rss_mb = rss * static_cast<double>(sysconf(_SC_PAGESIZE)) /
                 (1024.0 * 1024.0);
    }
    char b[64];
    std::string up_str = std::to_string(up / 3600) + "h " +
                         std::to_string((up / 60) % 60) + "m " +
                         std::to_string(up % 60) + "s";
    std::snprintf(b, sizeof b, "%.1f", latency);
    std::string out = "Pong!\nLatency: " + std::string(b) +
                      " ms\nUptime: " + up_str + "\nCPU: ";
    std::snprintf(b, sizeof b, "%.1f", cpu);
    out += b;
    std::snprintf(b, sizeof b, "%.1f", mem_used_pct);
    out += "%\nRAM: " + std::string(b) + "% of ";
    std::snprintf(b, sizeof b, "%.1f", mem_total_gb);
    out += std::string(b) + " GB\nDisk: ";
    std::snprintf(b, sizeof b, "%.1f", disk_pct);
    out += std::string(b) + "%\nRSS: ";
    std::snprintf(b, sizeof b, "%.1f", rss_mb);
    out += std::string(b) + " MB\nVoice: " + voice_.describe();
    reply(ctx, out);
}

void Dispatcher::onStats(const CommandContext& ctx) {
    std::string out = "Parlay stats:\n";
    out += "Store backend: " + store_.backend() + "\n";
    out += "Chats: " + std::to_string(store_.smembers("chats").size()) + "\n";
    out += "Users: " + std::to_string(store_.smembers("users").size()) + "\n";
    out += "Sudoers: " +
           std::to_string(store_.smembers("sudoers").size() +
                          (cfg_.owner_id ? 1 : 0)) +
           "\n";
    out += "Blacklisted chats: " +
           std::to_string(store_.smembers("bl_chats").size()) + "\n";
    out += "Blacklisted users: " +
           std::to_string(store_.smembers("bl_users").size()) + "\n";
    out += "Active voice chats: " +
           std::to_string(store_.keys("call:").size()) + "\n";
    out += "Commands registered: " + std::to_string(routes_.size()) + " routes\n";
    out += "Voice: " + voice_.describe();
    if (isSudo(ctx.message.from.id)) {
        double rss_mb = 0;
        std::ifstream f("/proc/self/statm");
        long pages = 0, rss = 0;
        f >> pages >> rss;
        rss_mb = rss * static_cast<double>(sysconf(_SC_PAGESIZE)) /
                 (1024.0 * 1024.0);
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", rss_mb);
        out += "\nRSS: " + std::string(b) + " MB\nCores: " +
               std::to_string(std::thread::hardware_concurrency()) +
               "\nBuild: C++20, Bot API + MTProto bridge";
    }
    reply(ctx, out);
}

void Dispatcher::onEval(const CommandContext& ctx) {
    // Owner-only remote shell (NoxBeats tools.cpp /eval). 60 s cap,
    // last 3000 chars of output.
    std::string cmd = trim(ctx.args);
    if (cmd.empty()) {
        reply(ctx, "Usage: /eval <shell command>");
        return;
    }
    reply(ctx, "Running...");
    int code = 0;
    std::string out = runShell(cmd, 60, code);
    if (out.size() > 3000) out = "..." + out.substr(out.size() - 3000);
    if (out.empty()) out = "(no output)";
    reply(ctx, "Input:\n" + cmd.substr(0, 800) + "\nOutput:\n" + out +
                   "\nExit: " + std::to_string(code));
}

void Dispatcher::onLogs(const CommandContext& ctx) {
    std::string out;
    if (!cfg_.log_file.empty()) {
        std::ifstream f(cfg_.log_file);
        std::vector<std::string> lines;
        std::string l;
        while (std::getline(f, l)) {
            lines.push_back(l);
            if (lines.size() > 200) lines.erase(lines.begin());
        }
        for (size_t i = lines.size() > 40 ? lines.size() - 40 : 0;
             i < lines.size(); ++i)
            out += lines[i] + "\n";
    }
    if (out.empty()) {
        std::lock_guard<std::mutex> lk(log_mu_);
        out = "Recent commands (in-memory log):\n";
        for (const auto& line : recent_log_) out += line + "\n";
        if (recent_log_.empty()) out += "(none yet)\n";
    }
    if (out.size() > 3500) out = out.substr(0, 3500) + "\n...";
    reply(ctx, out);
}

void Dispatcher::onLogger(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    if (args.empty() || (args[0] != "on" && args[0] != "off")) {
        auto on = store_.get("logger_on");
        reply(ctx, std::string("Logger is ") +
                       ((on && *on == "0") ? "OFF" : "ON") +
                       " (log chat " +
                       (cfg_.logger_id ? std::to_string(cfg_.logger_id)
                                       : "(unset)") +
                       "). Usage: /logger on|off");
        return;
    }
    store_.set("logger_on", args[0] == "on" ? "1" : "0");
    reply(ctx, std::string("Logger ") + (args[0] == "on" ? "ON" : "OFF") +
                   ".");
}

void Dispatcher::onRestart(const CommandContext& ctx) {
    reply(ctx, "Restarting... (exit 42; the supervisor should bring me "
               "back.)");
    std::cout << "[cmd] /restart requested by " << ctx.message.from.id
              << "; exiting 42\n";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::exit(42);
}

// --- handlers: RTMP ------------------------------------------------------------
void Dispatcher::onSetRtmp(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    int64_t chat = ctx.message.chat.id;
    if (args.empty()) {
        std::string url = chatField(chat, "rtmp_url", "");
        reply(ctx, url.empty()
                       ? "No RTMP target set. Usage: /setrtmp <rtmp url> "
                         "<stream key>"
                       : "RTMP target is set (key hidden). Use /setrtmp "
                         "<url> <key> to replace it.");
        return;
    }
    if (args.size() < 2) {
        reply(ctx, "Usage: /setrtmp <rtmp url> <stream key>");
        return;
    }
    // The key is stored but never echoed back (NoxBeats rtmp.cpp scrub()).
    setChatField(chat, "rtmp_url", args[0]);
    setChatField(chat, "rtmp_key", args[1]);
    reply(ctx, "RTMP target saved (key stored, never shown).");
}

void Dispatcher::onStream(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    std::string url = chatField(chat, "rtmp_url", "");
    std::string key = chatField(chat, "rtmp_key", "");
    if (url.empty()) {
        reply(ctx, "No RTMP target set. Use /setrtmp in DM first.");
        return;
    }
    std::string query = trim(ctx.args);
    if (query.empty()) {
        Json np, track;
        if (!nowPlaying(chat, np, track)) {
            reply(ctx, "Usage: /stream <song or link>");
            return;
        }
        query = track.find("title") ? track.find("title")->asString() : "";
    }
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        if (rtmp_streams_.count(chat)) {
            reply(ctx, "A stream is already running here. /streamstop "
                       "first.");
            return;
        }
    }
    // Resolve the video to a file via the downloaders service.
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"requested_by", Json(ctx.message.from.id)},
    });
    IpcResult r = ipc_.post(cfg_.dl_url, "download/youtube", body, 120);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* path = r.data.find("file_path");
    if (!path || path->asString().empty()) {
        reply(ctx, "Downloader returned no file for that query.");
        return;
    }
    std::string target = url;
    if (!key.empty()) target += (target.back() == '/' ? "" : "/") + key;
    std::string log_path =
        (cfg_.data_dir.empty() ? "data" : cfg_.data_dir) + "/rtmp-" +
        std::to_string(chat) + ".log";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        FILE* lf = freopen(log_path.c_str(), "w", stdout);
        if (lf) dup2(fileno(stdout), STDERR_FILENO);
        execlp("ffmpeg", "ffmpeg", "-re", "-i", path->asString().c_str(),
               "-c:v", "libx264", "-preset", "veryfast", "-c:a", "aac",
               "-f", "flv", target.c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) {
        reply(ctx, "Could not start ffmpeg.");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        rtmp_streams_[chat] = {pid, log_path,
                               std::chrono::steady_clock::now()};
    }
    reply(ctx, "Streaming to RTMP (ffmpeg pid " + std::to_string(pid) +
                   "). /streamstatus for progress, /streamstop to end it.");
}

void Dispatcher::onStreamStop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    pid_t pid = 0;
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        auto it = rtmp_streams_.find(chat);
        if (it == rtmp_streams_.end()) {
            reply(ctx, "No stream is running here.");
            return;
        }
        pid = it->second.pid;
        rtmp_streams_.erase(it);
    }
    if (pid > 0) {
        kill(-pid, SIGTERM);  // whole process group (setsid in onStream)
        int status = 0;
        waitpid(pid, &status, WNOHANG);
    }
    reply(ctx, "Stream stopped.");
}

void Dispatcher::onStreamStatus(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    std::lock_guard<std::mutex> lk(rtmp_mu_);
    auto it = rtmp_streams_.find(chat);
    if (it == rtmp_streams_.end()) {
        reply(ctx, "No stream is running here.");
        return;
    }
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - it->second.started)
                    .count();
    std::string tail;
    {
        std::ifstream f(it->second.log_path);
        std::vector<std::string> lines;
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty()) lines.push_back(l);
            if (lines.size() > 200) lines.erase(lines.begin());
        }
        for (size_t i = lines.size() > 5 ? lines.size() - 5 : 0;
             i < lines.size(); ++i)
            tail += lines[i] + "\n";
    }
    // Scrub the stream key out of the log tail before showing it.
    std::string key = chatField(chat, "rtmp_key", "");
    if (!key.empty()) {
        size_t p = 0;
        while ((p = tail.find(key, p)) != std::string::npos) {
            tail.replace(p, key.size(), "***");
            p += 3;
        }
    }
    reply(ctx, "Stream running for " + std::to_string(secs) +
                   "s (ffmpeg pid " + std::to_string(it->second.pid) +
                   ").\n" + tail);
}

}  // namespace parlay
