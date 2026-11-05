// dispatcher.cpp — part 1: routing, permissions, helpers, background loops.
#include "dispatcher.hpp"

#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "scram.hpp"  // base64Encode (TDLib `bytes` fields are base64)
#include "telemetry.hpp"

namespace parlay {
namespace {

// Mint a 12-hex-char run id (§1: pl:dl:cancel:<run_id>, 12 hex).
// The downloaders mint their own server run id per POST; the core
// mints its own id for the progress/cancel UI and maps it to the
// server run id once the POST returns.
std::string mintRunId() {
    static std::random_device rd;
    static std::mt19937_64 rng(rd() ^ (std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count()));
    std::uniform_int_distribution<unsigned long long> dist(0, 0xffffffffffffULL);
    unsigned long long v = dist(rng);
    char buf[13];
    std::snprintf(buf, sizeof(buf), "%012llx", v);
    return buf;
}

// Number of UTF-8 code points (JS string length counts UTF-16 units;
// close enough for the inline-mode <3-char rule — the core and vc-ts
// must agree on hint vs search for short queries).
size_t utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Strip [mm:ss.xx] timestamp prefixes, one per line (LRC -> plain
// lyrics, navidrome §7.9.4 / Music163bot §6.5). Used by the inline
// `l` article; onLyrics keeps its own inline copy (tested path).
std::string stripLrcTimestamps(const std::string& text) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        while (line.size() > 1 && line[0] == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) break;
            line = line.substr(close + 1);
        }
        if (!line.empty() && line[0] == ' ') line.erase(0, 1);
        out += line + "\n";
    }
    return out;
}

// Cut to max_chars UTF-8 chars with NO ellipsis (the inventory's
// "first 100 chars" description is literal; truncUtf8's "…" would
// make it 101).
std::string headUtf8(const std::string& s, size_t max_chars) {
    std::string cut = ui_texts::truncUtf8(s, max_chars);
    if (cut.size() >= 3 && cut.substr(cut.size() - 3) == "…")
        cut = cut.substr(0, cut.size() - 3);
    return cut;
}

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

// --- Spotify companion cards (§9-E8, §6.10–§6.11) --------------------------
// Rendered here in the core (ui_texts.hpp owns no companion-card
// builder — the cards were owned by spotify-rust). Emoji-free, Bot API
// HTML only. Every field is read defensively: the contract shape and
// the old service shape differ in names (`audio_features` vs
// `features`, `lyrics_url` vs `lyrics_link`, `profane_words` vs
// `profanity`) and in `disliked_by` / `ignored_by` ([UserId] arrays
// in the contract, integer counts in the old code, §10-D6) — the card
// shows the count either way and never a faked value.
std::string spotifyDetailsText(const Json& d) {
    const Json* track = d.find("track");
    std::string title = track && track->find("title")
                            ? track->find("title")->asString()
                            : "?";
    std::string artist = track && track->find("artist")
                             ? track->find("artist")->asString()
                             : "";
    std::string album = track && track->find("album")
                            ? track->find("album")->asString()
                            : "";
    std::string out = "<b>" + ui_texts::htmlEscape(title) + "</b>";
    if (!artist.empty())
        out += "\n" + ui_texts::htmlEscape(artist);
    if (!album.empty())
        out += "\nAlbum: " + ui_texts::htmlEscape(album);
    const Json* feat = d.find("audio_features");
    if (!feat) feat = d.find("features");
    if (feat && !feat->isNull()) {
        std::string key = feat->find("key") ? feat->find("key")->asString()
                                            : "Unknown";
        std::string mod = feat->find("modality")
                              ? feat->find("modality")->asString()
                              : "Unknown";
        double tempo = feat->find("tempo_bpm")
                           ? feat->find("tempo_bpm")->asDouble()
                           : 0.0;
        auto pct = [&](const char* k) {
            double v = feat->find(k) ? feat->find(k)->asDouble() : 0.0;
            return static_cast<int>(v * 100.0 + 0.5);
        };
        char tb[32];
        std::snprintf(tb, sizeof tb, "%.0f", tempo);
        out += "\n\nKey " + ui_texts::htmlEscape(key) + " " +
               ui_texts::htmlEscape(mod) + " · " + tb + " BPM";
        out += "\nAcoustic " + std::to_string(pct("acousticness")) + "%";
        out += "\nDanceability " + std::to_string(pct("danceability")) + "%";
        out += "\nEnergy " + std::to_string(pct("energy")) + "%";
        out += "\nInstrumentalness " +
               std::to_string(pct("instrumentalness")) + "%";
        out += "\nLive performance " + std::to_string(pct("liveness")) + "%";
        out += "\nSpeech-like " + std::to_string(pct("speechiness")) + "%";
        out += "\nPositivity " + std::to_string(pct("valence")) + "%";
    }
    auto countOf = [](const Json* v) -> int64_t {
        if (!v || v->isNull()) return 0;
        if (v->isArray()) return static_cast<int64_t>(v->size());
        return v->asInt();
    };
    out += "\n\nDisliked by " +
           std::to_string(countOf(d.find("disliked_by"))) + " people";
    out += "\nIgnored by " +
           std::to_string(countOf(d.find("ignored_by"))) + " people";
    const Json* genres = d.find("genres");
    if (genres && genres->isArray() && genres->size() > 0) {
        out += "\nGenres: ";
        bool first = true;
        for (const auto& g : genres->asArray()) {
            if (!first) out += ", ";
            first = false;
            out += ui_texts::htmlEscape(g.asString());
        }
    }
    const Json* prof = d.find("profanity");
    if (!prof) prof = d.find("profane_words");
    if (prof && !prof->isNull()) {
        if (prof->isArray() && prof->size() > 0) {
            out += "\nProfanity: ";
            bool first = true;
            for (const auto& w : prof->asArray()) {
                if (!first) out += ", ";
                first = false;
                out += ui_texts::htmlEscape(w.asString());
            }
        } else if (!prof->isArray() && !prof->asString().empty()) {
            out += "\nProfanity " + ui_texts::htmlEscape(prof->asString());
        }
    }
    const Json* lang = d.find("language");
    if (lang && !lang->isNull() && !lang->asString().empty())
        out += "\nLanguage: " + ui_texts::htmlEscape(lang->asString());
    const Json* lyrics = d.find("lyrics");
    if (lyrics && !lyrics->isNull() && !lyrics->asString().empty())
        out += "\n\n" + ui_texts::htmlEscape(lyrics->asString());
    else
        out += "\n\nNo lyrics found";
    const Json* link = d.find("lyrics_url");
    if (!link) link = d.find("lyrics_link");
    if (link && !link->isNull() && !link->asString().empty())
        out += "\n<a href=\"" + ui_texts::htmlEscape(link->asString()) +
               "\">Lyrics</a>";
    return out;
}

std::string spotifyAnalyzeText(const Json& d) {
    std::string analysis = d.find("analysis") ? d.find("analysis")->asString()
                                              : "";
    std::string out = "<b>Lyrics analysis</b>";
    if (!analysis.empty())
        out += "\n\n" + ui_texts::htmlEscape(analysis);
    const Json* words = d.find("profane_words");
    if (words && words->isArray() && words->size() > 0) {
        out += "\n\nProfane words: ";
        bool first = true;
        for (const auto& w : words->asArray()) {
            if (!first) out += ", ";
            first = false;
            out += ui_texts::htmlEscape(w.asString());
        }
    } else {
        out += "\n\nNo profanity found.";
    }
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

Dispatcher::Dispatcher(const Config& cfg, TelegramSender& tg, IpcClient& ipc,
                       Store& store, TdLibClient& assistant,
                       RateLimiter& limiter,
                       const std::string& bot_username)
    : cfg_(cfg),
      tg_(tg),
      ipc_(ipc),
      store_(store),
      assistant_(assistant),
      limiter_(limiter),
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
    // Named playlists (Parvati/TgMusicBot model, §17).
    add({"createplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onCreatePlaylist(c); });
    add({"deleteplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onDeletePlaylist(c); });
    add({"addtoplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onAddToPlaylist(c); });
    add({"removefromplaylist"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onRemoveFromPlaylist(c); });
    add({"playlistinfo"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onPlaylistInfo(c); });
    add({"myplaylists"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onMyPlaylists(c); });
    // Radio lifecycle (§21).
    add({"new"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onNewRadio(c); });
    add({"destroy"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onDestroyRadio(c); });
    add({"manage"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onManageRadio(c); });
    // New Phase 9 commands (§2/§16/§9).
    add({"menu"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onMenu(c); });
    add({"id"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onId(c); });
    add({"keyboard"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onKeyboard(c); });
    add({"about"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onAbout(c); });
    add({"status"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onStatus(c); });
    add({"now"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onNow(c); });
    add({"repo"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onRepo(c); });
    add({"download-settings"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onDownloadSettings(c); });
    // Settings / chat settings / autoplay / channel play.
    add({"start"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onStart(c); });
    add({"help"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onHelp(c); });
    add({"playmode"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onPlaymode(c); });
    add({"settings"}, Scope::Group, Perm::Everyone,
        [this](const CommandContext& c) { onSettings(c); });
    add({"quality"}, Scope::Any, Perm::Everyone,
        [this](const CommandContext& c) { onQuality(c); });
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
        {"fvplay", {"vplayforce", true}}, {"vcplayforce", {"vplayforce", true}},
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

bool Dispatcher::forwardShareMedia(const TgMessage& msg) {
    if (!msg.has_media || msg.chat.type != "private") return false;
    auto target = store_.get("sharetarget:" + std::to_string(msg.from.id));
    if (!target) return false;
    int64_t target_id = 0;
    try {
        target_id = std::stoll(*target);
    } catch (...) {
        return false;
    }
    if (target_id == 0) return false;
    CommandContext ctx;
    ctx.message = msg;
    ctx.command = "share";
    if (store_.sismember("shareblocked", std::to_string(msg.from.id))) {
        replyHtml(ctx, ui_texts::fmt(
                           ui_texts::kShareBlocked,
                           {{"name", ui_texts::htmlEscape(
                                         tg_.getUserMention(target_id))}}));
        return true;
    }
    std::string err;
    bool ok = false;
    if (msg.media_kind == "audio" || msg.media_kind == "voice") {
        int64_t sent = tg_.sendAudio(
            target_id, msg.media_file_id, "",
            ui_texts::shareReceivedKeyboard(
                std::to_string(msg.from.id),
                std::to_string(msg.message_id),
                msg.media_file_unique_id),
            &err);
        ok = sent > 0;
    } else {
        // Video / video note / document: no send-by-file_id call
        // exists for these, so forward the message itself (no
        // feedback buttons).
        ok = tg_.forwardMessage(target_id, msg.chat.id, msg.message_id);
    }
    if (ok) {
        store_.del("sharetarget:" + std::to_string(msg.from.id));
        replyHtml(ctx, ui_texts::kShareSent);
    } else {
        reply(ctx, "Could not send the music" +
                       (err.empty() ? "." : ": " + err));
    }
    return true;
}

// Phase 1 telemetry helpers (t_dispatch_to_ipc).
// Receipt timestamp for a context, falling back to "now" when the
// context was built outside handleMessage (no receipt recorded).
static int64_t receiptUs(const CommandContext& ctx) {
    return ctx.receipt_us != 0 ? ctx.receipt_us : telemetry::nowUs();
}

// One t_dispatch_to_ipc line for a /play. dispatch_to_post_us /
// post_to_response_us are present only when a backend IPC post was
// issued; paths with no IPC post (replied file, usage error) carry
// ipc=none instead. The single backend is identified by
// transport=uds.
static void emitDispatchToIpc(int64_t total_us, int64_t dispatch_to_post_us,
                       int64_t post_to_response_us, int64_t chat_id,
                       bool ok, const std::string& error) {
    std::vector<std::pair<std::string, std::string>> f;
    if (dispatch_to_post_us >= 0) {
        f.emplace_back("dispatch_to_post_us",
                       std::to_string(dispatch_to_post_us));
        f.emplace_back("post_to_response_us",
                       std::to_string(post_to_response_us));
    } else {
        f.emplace_back("ipc", "none");
    }
    f.emplace_back("action", "play");
    f.emplace_back("chat_id", std::to_string(chat_id));
    f.emplace_back("transport", "uds");
    f.emplace_back("ok", ok ? "1" : "0");
    if (!error.empty()) f.emplace_back("error", telemetry::sanitize(error));
    telemetry::emit("t_dispatch_to_ipc", total_us, f);
}

void Dispatcher::handleMessage(const TgMessage& msg) {
    // Phase 1 telemetry: command receipt. The routing CommandContext
    // below carries this into the handlers; t_dispatch_to_ipc
    // measures from here to the engine IPC post.
    const int64_t receipt_us = telemetry::nowUs();
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
                // Greet the group with the start card (§2). The
                // service message id is not a reply target.
                CommandContext ctx;
                ctx.message = msg;
                ctx.message.message_id = 0;
                ctx.command = "start";
                replyKb(ctx, ui_texts::kBotAdded,
                        ui_texts::startGroupKeyboard(cfg_.updates_url,
                                                     cfg_.support_url));
            } else if (u.id == cfg_.owner_id) {
                CommandContext ctx;
                ctx.message = msg;
                ctx.message.message_id = 0;
                ctx.command = "start";
                replyHtml(ctx, ui_texts::kOwnerJoined);
            } else if (isSudo(u.id)) {
                CommandContext ctx;
                ctx.message = msg;
                ctx.message.message_id = 0;
                ctx.command = "start";
                replyHtml(ctx, ui_texts::kSudoJoined);
            }
        }
    }
    if (msg.video_chat_started || msg.video_chat_ended) {
        // Auto-end: a voice chat ending kills the session (tools.cpp).
        // The backend owns the call, so a vc.stop tells it to end the
        // session; the core holds no audio of its own to stop.
        setCallActive(msg.chat.id, false);
        vcCall({msg, "stop", "", false}, "stop",
               Json::objectOf({{"chat_id", Json(msg.chat.id)}}));
        CommandContext ctx;
        ctx.message = msg;
        ctx.message.message_id = 0;
        ctx.command = "start";
        replyHtml(ctx, msg.video_chat_started ? ui_texts::kVcStarted
                                              : ui_texts::kVcEndedStop);
        return;
    }
    if (!msg.has_text) {
        // Media with no caption: the share-target forward (§22) is a
        // DM flow. Other media is ignored.
        if (forwardShareMedia(msg)) return;
        return;
    }

    // Plain-text triggers that are not commands.
    std::string text0 = msg.text;
    // A captioned media message goes to the pending share target
    // first (§22); the caption is not a pending-state reply.
    if (msg.has_media && forwardShareMedia(msg)) return;
    // Reply-a-number search pick (§7): a bare number with an active
    // search session plays that result (DM or group).
    if (!text0.empty() && text0.front() != '/' && isDigits(trim(text0))) {
        std::string query;
        Json results;
        if (loadSearchSession(msg.from.id, query, results)) {
            int n = std::stoi(trim(text0));
            if (n >= 1 && n <= static_cast<int>(results.size())) {
                TgCallbackQuery cq;
                cq.chat_id = msg.chat.id;
                cq.from = msg.from;
                auto sm = store_.get("searchmsg:" +
                                     std::to_string(msg.from.id));
                if (sm) {
                    try {
                        cq.has_message = true;
                        cq.message_id = std::stoll(*sm);
                    } catch (...) {
                    }
                }
                std::string err;
                if (playSearchPick(cq, msg.from.id, n, err)) return;
            }
        }
    }
    // Pending-state text flows (DM only): the download-settings
    // template prompt, share feedback, and a pending share target
    // that got text instead of music. Commands (leading '/') are
    // never swallowed by these.
    if (msg.chat.type == "private" && !text0.empty() &&
        text0.front() != '/') {
        int64_t user = msg.from.id;
        CommandContext ctx;
        ctx.message = msg;
        // 1. Download-settings template prompt (§9).
        if (store_.get("dlset_tpl_pending:" + std::to_string(user))) {
            store_.del("dlset_tpl_pending:" + std::to_string(user));
            auto cur = store_.get("dlset:" + std::to_string(user));
            std::string fmt_v = "mp3", yt_v = "0", tpl_v = text0;
            if (cur) {
                std::istringstream in(*cur);
                std::getline(in, fmt_v);
                std::getline(in, yt_v);
            }
            store_.set("dlset:" + std::to_string(user),
                       fmt_v + "\n" + yt_v + "\n" + tpl_v);
            ctx.command = "download-settings";
            replyKb(ctx, ui_texts::fmt(ui_texts::kDlSettingsTemplate,
                                       {{"template",
                                         ui_texts::htmlEscape(tpl_v)}}),
                    ui_texts::dlSettingsIndexKeyboard());
            return;
        }
        // 2. Share feedback text (§22): forward to the original
        // sender as a reply to the shared audio.
        auto fb = store_.get("sharefb:" + std::to_string(user));
        if (fb) {
            std::string sender, msg_id, fuid;
            {
                size_t a = fb->find(':');
                size_t b = a == std::string::npos
                               ? std::string::npos
                               : fb->find(':', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    sender = fb->substr(0, a);
                    msg_id = fb->substr(a + 1, b - a - 1);
                    fuid = fb->substr(b + 1);
                }
            }
            store_.del("sharefb:" + std::to_string(user));
            ctx.command = "share";
            if (sender.empty() || msg_id.empty()) {
                replyHtml(ctx, ui_texts::kShareFeedbackSent);
                return;
            }
            int64_t sender_id = 0, orig_msg = 0;
            try {
                sender_id = std::stoll(sender);
                orig_msg = std::stoll(msg_id);
            } catch (...) {
            }
            if (sender_id == 0 || orig_msg == 0) {
                replyHtml(ctx, ui_texts::kShareFeedbackSent);
                return;
            }
            // No dedup (ruling 8): no feedback store/endpoint exists,
            // so feedback is sent every time — never "already sent".
            // Dedup is future work; kShareFeedbackAlready is kept in
            // ui_texts.hpp for it.
            std::string err;
            std::string fwd = "Feedback from " +
                              ui_texts::htmlEscape(
                                  displayName(msg.from)) +
                              ":\n" + ui_texts::htmlEscape(text0);
            if (tg_.sendMessageHtml(sender_id, fwd, InlineKeyboard{},
                                    orig_msg, &err) < 0) {
                reply(ctx, "Could not send the feedback: " + err);
                return;
            }
            replyHtml(ctx, ui_texts::kShareFeedbackSent);
            return;
        }
        // 3. A pending share target that got text instead of music.
        if (store_.get("sharetarget:" + std::to_string(user))) {
            if (lowerStr(trim(text0)) == "cancel") {
                store_.del("sharetarget:" + std::to_string(user));
                reply(ctx, "Sharing cancelled.");
                return;
            }
            ctx.command = "share";
            replyHtml(ctx, ui_texts::fmt(
                               ui_texts::kUnhandledText,
                               {{"list", availableCommandsList(
                                             isSudo(user), isOwner(user))}}));
            return;
        }
    }
    // Delete trigger (any chat): "delete:<path>" (§25).
    if (text0.rfind("delete:", 0) == 0) {
        std::string path = trim(text0.substr(7));
        if (path.empty()) return;
        CommandContext ctx;
        ctx.message = msg;
        ctx.command = "delete";
        if (path.find("..") != std::string::npos) {
            reply(ctx, ui_texts::kDelInvalidPath);
            return;
        }
        if (path.find('/') == std::string::npos) {
            reply(ctx, ui_texts::kDelTopLevel);
            return;
        }
        replyKb(ctx, ui_texts::deleteConfirmTrackText(path),
                ui_texts::deleteConfirmKeyboard(path));
        return;
    }
    // In a DM, any non-command text gets the start card (§2, Tina).
    if (msg.chat.type == "private" &&
        (text0.empty() || text0.front() != '/')) {
        CommandContext ctx;
        ctx.message = msg;
        ctx.command = "start";
        replyKb(ctx, ui_texts::kStartPm,
                ui_texts::startPmKeyboard(bot_username_, cfg_.updates_url,
                                          cfg_.support_url,
                                          cfg_.source_url));
        return;
    }

    // Inline np/s/l need the chat the user is playing in, and an
    // inline_query carries no chat id — so the core remembers the
    // user's most recent group chat (last_chat:<user>, §13). Only
    // recorded for group/supergroup activity, and only written when
    // the value changes.
    if (msg.chat.type == "group" || msg.chat.type == "supergroup")
        noteLastChat(msg.from.id, msg.chat.id);

    // Reply-keyboard button taps in a group/supergroup (§14, §B.1;
    // gap G1): the persistent keyboard sends its label as plain
    // text. In a DM the same tap returns the start card; in a group
    // the tap now runs the claimed action instead of being silently
    // dropped. Music returns the start card (the music hub, same
    // card the DM path sends for it), Global Search opens the search
    // prompt, Settings opens the group settings panel (admin-gated
    // inside onSettings, with an honest denial for non-admins),
    // About returns the about card. Any other plain text still
    // falls through to the silent drop below, exactly as before.
    // (DM non-command text already returned above, so anything
    // reaching here is a group/supergroup message.)
    if (!text0.empty() && text0.front() != '/') {
        std::string label = trim(text0);
        if (label == "Music" || label == "Global Search" ||
            label == "Settings" || label == "About") {
            CommandContext ctx;
            ctx.message = msg;
            if (label == "Music") {
                ctx.command = "start";
                replyKb(ctx, ui_texts::kStartPm,
                        ui_texts::startPmKeyboard(
                            bot_username_, cfg_.updates_url,
                            cfg_.support_url, cfg_.source_url));
            } else if (label == "Global Search") {
                ctx.command = "search";
                onSearch(ctx);
            } else if (label == "Settings") {
                ctx.command = "settings";
                onSettings(ctx);
            } else {
                ctx.command = "about";
                onAbout(ctx);
            }
            return;
        }
    }

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
    ctx.receipt_us = receipt_us;
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
    if (!checkRateLimit(ctx)) return;

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
            auto reason = store_.get("maint_reason");
            std::string text = ui_texts::kMaintUser;
            if (reason && !reason->empty())
                text += "\n" +
                        ui_texts::fmt(ui_texts::kMaintReasonLine,
                                      {{"reason", ui_texts::htmlEscape(
                                                      *reason)}});
            replyHtml(ctx, text);
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
            int mins = settingsFor(msg.chat.id).clean_delay_min;
            if (mins > 0)
                scheduleDelete(msg.chat.id, msg.message_id, mins);
        }
        return;  // first match wins
    }
    // Unknown command: the §3 reply (never silent, never a crash).
    replyHtml(ctx, ui_texts::fmt(ui_texts::kUnknownCommand,
                                 {{"command", "/" + ctx.command},
                                  {"list", availableCommandsList(
                                               isSudo(msg.from.id),
                                               isOwner(msg.from.id))}}));
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
    replyHtml(ctx, ui_texts::kDenyAdmin);
    return false;
}

bool Dispatcher::canManageVc(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    int64_t user = ctx.message.from.id;
    if (isSudo(user)) return true;
    int mode = settingsFor(chat).admin_mode;
    if (mode == 2) return true;  // everyone
    if (isChatAdmin(chat, user)) return true;
    if (mode == 0 && isAuthUser(chat, user)) return true;
    replyHtml(ctx, ui_texts::kDenyAdminOrAuth);
    return false;
}

bool Dispatcher::playAllowed(const CommandContext& ctx, bool force) {
    int64_t chat = vcTarget(ctx);
    bool admin_only = settingsFor(chat).play_mode == "admins";
    if (!admin_only && !force) return true;
    int64_t user = ctx.message.from.id;
    if (isSudo(user) || isChatAdmin(chat, user) || isAuthUser(chat, user))
        return true;
    replyHtml(ctx, ui_texts::kGuardPlayAdmin);
    return false;
}

// Token-bucket gate: one token from the chat bucket AND the user
// bucket. A denial answers honestly with the limiter's reason string
// (including the retry-after) instead of dropping the command
// silently — the old 1 cmd/sec silent drop (dispatcher.cpp:911) is
// gone. The decision takes nothing on a denial, so a denied user is
// not charged for the attempt.
bool Dispatcher::checkRateLimit(const CommandContext& ctx) {
    RateLimitDecision d =
        limiter_.tryAcquire(ctx.message.chat.id, ctx.message.from.id);
    if (d.allowed) return true;
    std::string reason = d.reason.empty()
                             ? "Rate limit exceeded, try again shortly."
                             : d.reason;
    reply(ctx, reason);
    return false;
}


// --- chat settings (Q7 rewire) ------------------------------------------------
// The backend's bbolt store is the single home for the 15
// chat.settings keys; the core keeps only an in-memory cache, never
// a second copy in state.json.
namespace {

// Locked defaults, as the backend's DefaultChatSettings (Go) renders
// them. Used when the backend is down and no snapshot is cached.
ui_texts::ChatSettings defaultChatSettings() {
    return ui_texts::ChatSettings{};
}

// Parse a backend "settings" object (all 15 keys) into the panel
// snapshot. Unknown/absent keys keep the locked defaults; the
// backend validator is the authority on ranges, so no second
// validation happens here.
ui_texts::ChatSettings parseSettingsJson(const Json& settings) {
    ui_texts::ChatSettings s;
    const Json* v = nullptr;
    if ((v = settings.find("admin_mode")) != nullptr)
        s.admin_mode = static_cast<int>(v->asInt(s.admin_mode));
    if ((v = settings.find("play_mode")) != nullptr && v->isString())
        s.play_mode = v->asString();
    if ((v = settings.find("cmd_delete")) != nullptr)
        s.cmd_delete = v->asBool(s.cmd_delete);
    if ((v = settings.find("clean_delay_min")) != nullptr)
        s.clean_delay_min = static_cast<int>(v->asInt(s.clean_delay_min));
    if ((v = settings.find("thumbnails")) != nullptr)
        s.thumbnails = v->asBool(s.thumbnails);
    if ((v = settings.find("autoplay")) != nullptr)
        s.autoplay = v->asBool(s.autoplay);
    if ((v = settings.find("autoplay_lang")) != nullptr && v->isString())
        s.autoplay_lang = v->asString();
    if ((v = settings.find("autoplay_mood")) != nullptr && v->isString())
        s.autoplay_mood = v->asString();
    if ((v = settings.find("silent_mode")) != nullptr)
        s.silent = v->asBool(s.silent);
    if ((v = settings.find("auto_delete")) != nullptr)
        s.auto_delete = v->asBool(s.auto_delete);
    if ((v = settings.find("auto_delete_min")) != nullptr)
        s.auto_delete_min =
            static_cast<int>(v->asInt(s.auto_delete_min));
    if ((v = settings.find("language")) != nullptr && v->isString())
        s.language = v->asString();
    if ((v = settings.find("paginate_purge_old")) != nullptr)
        s.paginate_purge_old = v->asBool(s.paginate_purge_old);
    if ((v = settings.find("chat_music_download")) != nullptr)
        s.chat_music_download = v->asBool(s.chat_music_download);
    return s;
}

// Patch one key of a cached snapshot from a set value (the fallback
// when the backend's set response carries no settings object).
void applySettingKey(ui_texts::ChatSettings& s, const std::string& key,
                     const Json& value) {
    if (key == "admin_mode") s.admin_mode = static_cast<int>(value.asInt(s.admin_mode));
    else if (key == "play_mode" && value.isString()) s.play_mode = value.asString();
    else if (key == "cmd_delete") s.cmd_delete = value.asBool(s.cmd_delete);
    else if (key == "clean_delay_min") s.clean_delay_min = static_cast<int>(value.asInt(s.clean_delay_min));
    else if (key == "thumbnails") s.thumbnails = value.asBool(s.thumbnails);
    else if (key == "autoplay") s.autoplay = value.asBool(s.autoplay);
    else if (key == "autoplay_lang" && value.isString()) s.autoplay_lang = value.asString();
    else if (key == "autoplay_mood" && value.isString()) s.autoplay_mood = value.asString();
    else if (key == "silent_mode") s.silent = value.asBool(s.silent);
    else if (key == "auto_delete") s.auto_delete = value.asBool(s.auto_delete);
    else if (key == "auto_delete_min") s.auto_delete_min = static_cast<int>(value.asInt(s.auto_delete_min));
    else if (key == "language" && value.isString()) s.language = value.asString();
    else if (key == "paginate_purge_old") s.paginate_purge_old = value.asBool(s.paginate_purge_old);
    else if (key == "chat_music_download") s.chat_music_download = value.asBool(s.chat_music_download);
    // clean_mode has no field: it is derived from clean_delay_min > 0.
}

// Parse one quality.get Opus values object (effective or library)
// into QualityValues. Absent/wrong-typed fields keep the locked
// defaults; the backend validator is the authority on ranges.
ui_texts::QualityValues parseQualityValues(const Json& o) {
    ui_texts::QualityValues v;
    const Json* f = nullptr;
    if ((f = o.find("bitrate_bps")) != nullptr)
        v.bitrate_bps = static_cast<int>(f->asInt(v.bitrate_bps));
    if ((f = o.find("frame_ms")) != nullptr)
        v.frame_ms = static_cast<int>(f->asInt(v.frame_ms));
    if ((f = o.find("complexity")) != nullptr)
        v.complexity = static_cast<int>(f->asInt(v.complexity));
    if ((f = o.find("vbr")) != nullptr) v.vbr = f->asBool(v.vbr);
    if ((f = o.find("fec")) != nullptr) v.fec = f->asBool(v.fec);
    if ((f = o.find("dtx")) != nullptr) v.dtx = f->asBool(v.dtx);
    if ((f = o.find("max_playback_rate_hz")) != nullptr)
        v.max_playback_rate_hz =
            static_cast<int>(f->asInt(v.max_playback_rate_hz));
    return v;
}

// Appended to status replies when the settings snapshot is served
// from cache/defaults because the backend is down (Q7 fallback):
// the "current" value shown may not be the chat's stored value.
const char* kSettingsDegradedSuffix =
    " (settings unreachable — showing defaults)";

}  // namespace

ui_texts::ChatSettings Dispatcher::settingsFor(int64_t chat_id) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        auto it = settings_cache_.find(chat_id);
        if (it != settings_cache_.end()) {
            auto ttl = it->second.live ? seconds(60) : seconds(5);
            if (now - it->second.fetched_at < ttl)
                return it->second.settings;
        }
    }
    // Cache miss / stale: one UDS GET (chat.settings get-all).
    IpcResult r = ipc_.get("/v1/chat/settings?chat_id=" +
                               std::to_string(chat_id),
                           10);
    std::lock_guard<std::mutex> lk(settings_mu_);
    if (r.ok) {
        const Json* settings = r.data.find("settings");
        ui_texts::ChatSettings s =
            settings ? parseSettingsJson(*settings) : defaultChatSettings();
        settings_cache_[chat_id] = SettingsCacheEntry{s, now, true};
        return s;
    }
    // Backend down (fallback semantics, Q7 blocker (d)): keep serving
    // the last snapshot when there is one (stale but best-known),
    // else the locked defaults — never a second copy in state.json.
    // The 5 s TTL above re-probes soon. Gates fail OPEN on the
    // defaults (play_mode "everyone", admin_mode 0) while the backend
    // is down; that policy is flagged for the user in
    // phase6/worker-c-parity.md — a fail-closed gate is a one-line
    // change at the playAllowed/canManageVc call sites.
    std::cerr << "[settings] chat.settings fetch failed for chat "
              << chat_id << " (" << r.describe()
              << "); serving cached/default settings\n";
    auto it = settings_cache_.find(chat_id);
    if (it != settings_cache_.end()) {
        it->second.live = false;
        it->second.fetched_at = now;
        return it->second.settings;
    }
    ui_texts::ChatSettings def = defaultChatSettings();
    settings_cache_[chat_id] = SettingsCacheEntry{def, now, false};
    return def;
}

bool Dispatcher::settingsLive(int64_t chat_id) {
    using namespace std::chrono;
    std::lock_guard<std::mutex> lk(settings_mu_);
    auto it = settings_cache_.find(chat_id);
    if (it == settings_cache_.end() || !it->second.live) return false;
    return steady_clock::now() - it->second.fetched_at < seconds(60);
}

bool Dispatcher::setSetting(int64_t chat_id, const std::string& key,
                            const Json& value) {
    Json body = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"key", Json(key)},
        {"value", value},
    });
    IpcResult r = ipc_.post("chat.settings", body, 10);
    if (!r.ok) {
        std::cerr << "[settings] chat.settings set " << key << " for chat "
                  << chat_id << " failed (" << r.describe() << ")\n";
        return false;
    }
    // Write-through: the cache matches the backend for this key. The
    // set response carries the updated settings object; prefer it
    // (it also carries the backend's clean_mode / clean_delay_min
    // sync), else patch the one key locally.
    std::lock_guard<std::mutex> lk(settings_mu_);
    const Json* settings = r.data.find("settings");
    if (settings) {
        ui_texts::ChatSettings s = parseSettingsJson(*settings);
        settings_cache_[chat_id] =
            SettingsCacheEntry{s, std::chrono::steady_clock::now(), true};
        return true;
    }
    auto it = settings_cache_.find(chat_id);
    if (it != settings_cache_.end()) {
        applySettingKey(it->second.settings, key, value);
        it->second.fetched_at = std::chrono::steady_clock::now();
        it->second.live = true;
    }
    return true;
}

// --- Opus quality (quality.get surface, 2026-11-05) ---------------------
// One UDS read; no caching: the card must show the values in force
// now (a set just landed, or another admin changed them). The
// backend resolves the precedence (store > config > profile >
// default); the core never recomputes it.
bool Dispatcher::fetchQuality(int64_t chat_id,
                              ui_texts::QualityInfo& out) {
    Json body = Json::objectOf({{"chat_id", Json(chat_id)}});
    IpcResult r = ipc_.post("quality.get", body, 10);
    if (!r.ok) {
        std::cerr << "[quality] quality.get failed for chat " << chat_id
                  << " (" << r.describe() << ")\n";
        return false;
    }
    const Json* eff = r.data.find("effective");
    if (eff == nullptr || !eff->isObject()) {
        std::cerr << "[quality] quality.get for chat " << chat_id
                  << " has no effective object\n";
        return false;
    }
    out = ui_texts::QualityInfo{};
    out.effective = parseQualityValues(*eff);
    const Json* lib = r.data.find("library");
    if (lib != nullptr && lib->isObject())
        out.library = parseQualityValues(*lib);
    else
        out.library = out.effective;
    static const char* kFields[] = {"bitrate_bps", "frame_ms",
                                    "complexity", "vbr", "fec", "dtx"};
    const Json* sources = r.data.find("sources");
    if (sources != nullptr && sources->isObject()) {
        for (const char* f : kFields) {
            const Json* v = sources->find(f);
            if (v != nullptr && v->isString())
                out.sources[f] = v->asString();
        }
    }
    const Json* active = r.data.find("active_call");
    if (active != nullptr) out.active_call = active->asBool(false);
    const Json* applies = r.data.find("applies_on");
    if (applies != nullptr && applies->isString())
        out.applies_on = applies->asString();
    return true;
}

// Edit the tapped message to the quality sub-screen. Answers
// nothing: the caller answers the callback query exactly once
// before calling this. On a backend failure the screen is the
// honest load-failure text with a Back button — never a silent
// no-op, never a dead end.
void Dispatcher::renderQualityScreen(const TgCallbackQuery& cq) {
    ui_texts::QualityInfo q;
    if (!fetchQuality(cq.chat_id, q)) {
        editCbCard(cq, ui_texts::kQualityLoadFailed,
                   ui_texts::qualityBackKeyboard());
        return;
    }
    editCbCard(cq, ui_texts::qualityCardText(q),
               ui_texts::settingsQualityKeyboard(q));
}

// --- per-chat runtime fields (NOT settings) ------------------------------
// Core-voice session + routing state with no backend home. Reads fall
// back to the legacy "chat:" hash once (pre-rewire values survive the
// upgrade); writes go to "chatrt:" only.
std::string Dispatcher::chatRtField(int64_t chat_id,
                                    const std::string& field,
                                    const std::string& def) const {
    auto v = store_.hget("chatrt:" + std::to_string(chat_id), field);
    if (v) return *v;
    auto legacy = store_.hget("chat:" + std::to_string(chat_id), field);
    return legacy ? *legacy : def;
}

void Dispatcher::setChatRtField(int64_t chat_id, const std::string& field,
                                const std::string& value) {
    store_.hset("chatrt:" + std::to_string(chat_id), field, value);
}

int Dispatcher::chatRtIntField(int64_t chat_id, const std::string& field,
                               int def) const {
    std::string v = chatRtField(chat_id, field, "");
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

double Dispatcher::chatRtSpeedField(int64_t chat_id) const {
    std::string v = chatRtField(chat_id, "speed", "");
    if (v.empty()) return 1.0;
    char* end = nullptr;
    double s = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !std::isfinite(s) ||
        s < 0.5 || s > 4.0)
        return 1.0;
    return s;
}



bool Dispatcher::isAuthUser(int64_t chat_id, int64_t user_id) const {
    return store_.sismember("auth:" + std::to_string(chat_id),
                            std::to_string(user_id));
}

int64_t Dispatcher::vcTarget(const CommandContext& ctx) const {
    if (!ctx.via_channel) return ctx.message.chat.id;
    std::string v = chatRtField(ctx.message.chat.id, "cplay", "0");
    try {
        int64_t ch = std::stoll(v);
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
    int mins = settingsFor(ctx.message.chat.id).clean_delay_min;
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

void Dispatcher::replyError(const CommandContext& ctx, const IpcResult& r) {
    reply(ctx, "Service error (" + r.error_code + "): " + r.error_message);
}

// --- Phase 9 reply helpers (HTML + keyboards) ------------------------------
int64_t Dispatcher::replyHtml(const CommandContext& ctx,
                              const std::string& text) {
    int64_t id = tg_.sendMessageHtml(ctx.message.chat.id, text,
                                     InlineKeyboard{}, ctx.message.message_id);
    if (id < 0) {
        std::cerr << "[tg] sendMessageHtml failed\n";
        return -1;
    }
    int mins = settingsFor(ctx.message.chat.id).clean_delay_min;
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

int64_t Dispatcher::replyKb(const CommandContext& ctx, const std::string& text,
                            const InlineKeyboard& kb) {
    int64_t id = tg_.sendMessageHtml(ctx.message.chat.id, text,
                                     kb,
                                     ctx.message.message_id);
    if (id < 0) {
        std::cerr << "[tg] sendMessageHtml (kb) failed\n";
        return -1;
    }
    int mins = settingsFor(ctx.message.chat.id).clean_delay_min;
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

int64_t Dispatcher::replyReplyKb(const CommandContext& ctx,
                                 const std::string& text,
                                 const ReplyKeyboard& kb, bool remove_kb) {
    int64_t id = remove_kb
                     ? tg_.sendMessageRemoveKb(ctx.message.chat.id, text,
                                               ctx.message.message_id)
                     : tg_.sendMessageReplyKb(ctx.message.chat.id, text, kb,
                                              ctx.message.message_id);
    if (id < 0) {
        std::cerr << "[tg] sendMessageHtml (reply kb) failed\n";
        return -1;
    }
    int mins = settingsFor(ctx.message.chat.id).clean_delay_min;
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

int64_t Dispatcher::replyPhoto(const CommandContext& ctx,
                               const std::string& photo,
                               const std::string& caption,
                               const InlineKeyboard& kb) {
    int64_t id = tg_.sendPhoto(ctx.message.chat.id, photo, caption, kb);
    if (id < 0) {
        std::cerr << "[tg] sendPhoto failed\n";
        return -1;
    }
    int mins = settingsFor(ctx.message.chat.id).clean_delay_min;
    if (mins > 0) scheduleDelete(ctx.message.chat.id, id, mins);
    return id;
}

bool Dispatcher::answerCb(const TgCallbackQuery& cq, const std::string& text,
                          bool alert) {
    std::string err;
    return tg_.answerCallbackQuery(cq.id, text, alert, 0, &err);
}

bool Dispatcher::editCbCard(const TgCallbackQuery& cq,
                            const std::string& text,
                            const InlineKeyboard& kb) {
    if (!cq.has_message) return false;
    int64_t card_id = 0;
    bool is_photo = false;
    std::string err;
    // The card kind decides the edit method: photo cards carry their
    // text as a caption. The recorded card info is authoritative; when
    // there is none (e.g. a settings panel, which is always text) fall
    // back to editMessageText.
    bool have_card = cardInfo(cq.chat_id, card_id, is_photo) &&
                     card_id == cq.message_id;
    InlineKeyboard markup = kb;
    if (have_card && is_photo)
        return tg_.editMessageCaption(cq.chat_id, cq.message_id, text,
                                      markup, &err);
    return tg_.editMessageText(cq.chat_id, cq.message_id, text, markup,
                               &err);
}

bool Dispatcher::editCbKeyboard(const TgCallbackQuery& cq,
                                const InlineKeyboard& kb) {
    if (!cq.has_message) return false;
    std::string err;
    return tg_.editMessageReplyMarkup(
        cq.chat_id, cq.message_id,
        kb, &err);
}

// --- Phase 9 card rendering -------------------------------------------------
std::string Dispatcher::displayName(const TgUser& u) {
    std::string name = u.first_name;
    if (name.empty()) name = std::to_string(u.id);
    if (!u.username.empty()) name += " (@" + u.username + ")";
    return name;
}

ui_texts::CardTrack Dispatcher::cardTrack(const Json& track,
                                         const std::string& requester) {
    ui_texts::CardTrack t;
    std::string title =
        track.find("title") ? track.find("title")->asString() : "?";
    t.title = ui_texts::truncUtf8(ui_texts::htmlEscape(title), 25);
    t.url = track.find("url") ? track.find("url")->asString() : "";
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    t.duration = ui_texts::durationStr(dur);
    t.requester = requester;
    std::string id = track.find("id") ? track.find("id")->asString() : "";
    t.token = ui_texts::trackToken(id);
    return t;
}

ui_texts::CardTrack Dispatcher::cardTrack(const Json& track,
                                         int64_t requested_by) {
    return cardTrack(track, std::to_string(requested_by));
}

ui_texts::ChatSettings Dispatcher::chatSettings(int64_t chat_id) {
    return settingsFor(chat_id);
}

bool Dispatcher::saveChatSettings(int64_t chat_id,
                                  const ui_texts::ChatSettings& s) {
    // Diffed write-back: only keys that changed vs the fetched
    // snapshot are posted, so a panel toggle is one UDS call and a
    // stale read can never clobber an untouched key. clean_mode has
    // no field of its own: the backend derives it from
    // clean_delay_min > 0, so the delay is posted first and the bool
    // follows when it changed. Stops at the first failed write and
    // returns false — the caller must not claim the save worked.
    ui_texts::ChatSettings cur = settingsFor(chat_id);
    bool ok = true;
    auto postIfChanged = [&](const std::string& key, const Json& value,
                             bool changed) {
        if (!changed || !ok) return;
        if (!setSetting(chat_id, key, value)) ok = false;
    };
    postIfChanged("admin_mode", Json(static_cast<int64_t>(s.admin_mode)),
                  s.admin_mode != cur.admin_mode);
    postIfChanged("play_mode", Json(s.play_mode),
                  s.play_mode != cur.play_mode);
    postIfChanged("cmd_delete", Json(s.cmd_delete),
                  s.cmd_delete != cur.cmd_delete);
    postIfChanged("clean_delay_min",
                  Json(static_cast<int64_t>(s.clean_delay_min)),
                  s.clean_delay_min != cur.clean_delay_min);
    postIfChanged("clean_mode", Json(s.clean_delay_min > 0),
                  (s.clean_delay_min > 0) != (cur.clean_delay_min > 0));
    postIfChanged("thumbnails", Json(s.thumbnails),
                  s.thumbnails != cur.thumbnails);
    postIfChanged("autoplay", Json(s.autoplay), s.autoplay != cur.autoplay);
    postIfChanged("autoplay_lang", Json(s.autoplay_lang),
                  s.autoplay_lang != cur.autoplay_lang);
    postIfChanged("autoplay_mood", Json(s.autoplay_mood),
                  s.autoplay_mood != cur.autoplay_mood);
    postIfChanged("silent_mode", Json(s.silent), s.silent != cur.silent);
    postIfChanged("auto_delete", Json(s.auto_delete),
                  s.auto_delete != cur.auto_delete);
    postIfChanged("auto_delete_min",
                  Json(static_cast<int64_t>(s.auto_delete_min)),
                  s.auto_delete_min != cur.auto_delete_min);
    postIfChanged("language", Json(s.language),
                  s.language != cur.language);
    postIfChanged("paginate_purge_old", Json(s.paginate_purge_old),
                  s.paginate_purge_old != cur.paginate_purge_old);
    postIfChanged("chat_music_download", Json(s.chat_music_download),
                  s.chat_music_download != cur.chat_music_download);
    return ok;
}

void Dispatcher::recordCard(int64_t chat_id, int64_t message_id,
                            bool is_photo) {
    store_.set("card:" + std::to_string(chat_id),
               std::string(is_photo ? "photo|" : "text|") +
                   std::to_string(message_id));
}

bool Dispatcher::cardInfo(int64_t chat_id, int64_t& message_id,
                          bool& is_photo) {
    auto v = store_.get("card:" + std::to_string(chat_id));
    if (!v || v->empty()) return false;
    size_t bar = v->find('|');
    if (bar == std::string::npos) return false;
    is_photo = v->substr(0, bar) == "photo";
    try {
        message_id = std::stoll(v->substr(bar + 1));
    } catch (...) {
        return false;
    }
    return message_id != 0;
}

int64_t Dispatcher::sendNowPlayingCard(int64_t chat_id, const Json& np,
                                       const std::string& state_line) {
    const Json* track = np.find("track");
    if (!track || track->isNull()) return -1;
    std::string requester;
    auto rn = store_.get("last_requester_name:" + std::to_string(chat_id));
    requester = rn ? *rn : "";
    ui_texts::CardTrack ct = cardTrack(*track, requester);
    int64_t pos = np.find("position_sec") ? np.find("position_sec")->asInt()
                                          : 0;
    int64_t dur =
        track->find("duration_sec") ? track->find("duration_sec")->asInt()
                                    : 0;
    bool paused = false;
    if (const Json* p = np.find("paused")) paused = p->asBool();
    if (const Json* p = np.find("is_paused")) paused = p->asBool();
    bool autoplay = false;
    if (const Json* a = np.find("autoplay")) autoplay = a->asBool();
    if (const Json* a = np.find("autoplay_enabled")) autoplay = a->asBool();
    int loop = 0;
    if (const Json* l = np.find("loop")) loop = static_cast<int>(l->asInt());
    if (const Json* l = np.find("loop_count"))
        loop = static_cast<int>(l->asInt());
    if (loop == 0) loop = chatRtIntField(chat_id, "loop", 0);
    std::string text = ui_texts::nowPlayingText(ct);
    if (!state_line.empty()) text = state_line + "\n\n" + text;
    InlineKeyboard kb = ui_texts::playerKeyboard(
        chat_id, paused, autoplay, loop, ct.token, true, pos, dur);
    std::string thumb = effectiveThumb(
        chat_id, track->find("thumbnail")
                     ? track->find("thumbnail")->asString()
                     : "");
    CommandContext fake;
    fake.message.chat.id = chat_id;
    int64_t id = -1;
    bool is_photo = false;
    if (!thumb.empty()) {
        id = tg_.sendPhoto(chat_id, thumb, text, kb);
        if (id > 0) is_photo = true;
    }
    if (id < 0) {
        // No thumbnail (track or configured default), thumbnails off,
        // or sendPhoto failed: text card.
        id = tg_.sendMessageHtml(chat_id, text, kb, 0);
    }
    if (id > 0) recordCard(chat_id, id, is_photo);
    return id;
}

bool Dispatcher::buildQueueCard(int64_t chat_id, bool show_all,
                                std::string& text_out, InlineKeyboard& kb_out,
                                std::string& thumb_out) {
    Json np, track;
    if (!nowPlaying(chat_id, np, track)) return false;
    std::string requester;
    auto rn = store_.get("last_requester_name:" + std::to_string(chat_id));
    requester = rn ? *rn : "";
    ui_texts::CardTrack now = cardTrack(track, requester);
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Json body = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(fake, "queue", body);
    if (!q.ok) return false;
    const Json* items = q.data.find("queue");
    std::vector<ui_texts::CardTrack> up;
    size_t total = 0;
    if (items) {
        // The list is the waiting queue only (§1.7, §10-D4): the
        // current track is never in it, so every item is up-next,
        // numbered 1..N in display order (display N is backend
        // waiting position N−1).
        total = items->size();
        size_t shown = show_all ? total
                                : std::min<size_t>(
                                      total,
                                      static_cast<size_t>(
                                          ui_texts::kQueuePageSize));
        for (size_t i = 0; i < shown; ++i) {
            const Json& item = items->asArray()[i];
            const Json* t = item.find("track");
            if (!t) continue;
            int64_t by = item.find("requested_by")
                             ? item.find("requested_by")->asInt()
                             : 0;
            up.push_back(cardTrack(*t, by));
        }
    }
    bool has_more = !show_all && total > up.size();
    text_out = ui_texts::queueText(now, up, total);
    kb_out = ui_texts::queueKeyboard(chat_id, up, has_more,
                                     total - up.size());
    thumb_out = effectiveThumb(
        chat_id, track.find("thumbnail")
                     ? track.find("thumbnail")->asString()
                     : "");
    return true;
}

void Dispatcher::renderQueueCard(const CommandContext& ctx, bool show_all) {
    int64_t chat = vcTarget(ctx);
    std::string text;
    InlineKeyboard kb;
    std::string thumb;
    if (!buildQueueCard(chat, show_all, text, kb, thumb)) {
        replyHtml(ctx, ui_texts::kQueueEmpty);
        return;
    }
    if (!thumb.empty() && thumbnailsOn(chat)) {
        if (replyPhoto(ctx, thumb, text, kb) > 0) return;
    }
    replyKb(ctx, text, kb);
}

bool Dispatcher::resolveQueueToken(int64_t chat_id,
                                   const std::string& token, Json& item_out,
                                   int64_t& position_out) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Json body = Json::objectOf({
        {"chat_id", Json(chat_id)},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(fake, "queue", body);
    if (!q.ok) return false;
    const Json* items = q.data.find("queue");
    if (!items) return false;
    // The list is the waiting queue only (§1.7, §10-D4): positions
    // are 0-based waiting indices, the current track is never in it.
    int64_t pos = 0;
    for (const auto& item : items->asArray()) {
        const Json* t = item.find("track");
        if (t) {
            std::string id = t->find("id") ? t->find("id")->asString() : "";
            if (ui_texts::trackToken(id) == token) {
                item_out = item;
                // The engine's remove wants the 0-based waiting
                // position: prefer the item's own "position" field
                // when the engine provides one, else the 0-based
                // index in the list.
                if (const Json* p = item.find("position"))
                    position_out = p->asInt();
                else
                    position_out = pos;
                return true;
            }
        }
        ++pos;
    }
    return false;
}

void Dispatcher::storeSearchSession(int64_t user_id,
                                    const std::string& query,
                                    const Json& results,
                                    int64_t message_id) {
    store_.set("search:" + std::to_string(user_id),
               query + "\n" + results.dump());
    store_.set("searchmsg:" + std::to_string(user_id),
               std::to_string(message_id));
}

bool Dispatcher::loadSearchSession(int64_t user_id, std::string& query_out,
                                   Json& results_out) {
    auto v = store_.get("search:" + std::to_string(user_id));
    if (!v || v->empty()) return false;
    size_t nl = v->find('\n');
    if (nl == std::string::npos) return false;
    query_out = v->substr(0, nl);
    try {
        results_out = Json::parse(v->substr(nl + 1));
    } catch (...) {
        return false;
    }
    return results_out.isArray();
}

bool Dispatcher::playSearchPick(const TgCallbackQuery& cq, int64_t user_id,
                                int n, std::string& error) {
    std::string query;
    Json results;
    if (!loadSearchSession(user_id, query, results)) {
        error = "expired";
        return false;
    }
    if (n < 1 || n > static_cast<int>(results.size())) {
        error = "range";
        return false;
    }
    const Json& t = results.asArray()[static_cast<size_t>(n) - 1];
    std::string id = t.find("id") ? t.find("id")->asString() : "";
    std::string url = t.find("url") ? t.find("url")->asString() : "";
    std::string title = t.find("title") ? t.find("title")->asString() : "";
    std::string q = !url.empty() ? url : (!id.empty() ? id : title);
    if (q.empty()) {
        error = "empty";
        return false;
    }
    // Delete the search message on a valid tap (§7).
    if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
    CommandContext fake;
    fake.message.chat.id = cq.chat_id;
    fake.message.from.id = user_id;
    fake.message.from = cq.from;
    fake.command = "play";
    fake.args = q;
    onPlay(fake);
    return true;
}

std::string Dispatcher::availableCommandsList(bool is_sudo,
                                             bool is_owner) const {
    std::string list;
    auto add = [&list](const char* c) {
        if (!list.empty()) list += ", ";
        list += c;
    };
    for (const auto& h : ui_texts::kCmdHelpTable) {
        std::string cmd = h.cmd;
        if (cmd == "eval" || cmd == "sh" || cmd == "maintenance" ||
            cmd == "addsudo" || cmd == "delsudo") {
            if (!is_owner) continue;
        } else if (cmd == "blacklist" || cmd == "unblacklist" ||
                   cmd == "blacklisted" || cmd == "ac" ||
                   cmd == "activevc" || cmd == "broadcast" ||
                   cmd == "autoleave" || cmd == "speedtest" ||
                   cmd == "logs" || cmd == "logger" || cmd == "restart") {
            if (!is_sudo && !is_owner) continue;
        }
        add(h.cmd);
    }
    return list;
}

std::vector<std::string> Dispatcher::splitCb(const std::string& data) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : data) {
        if (c == ':') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool Dispatcher::callbackCanControl(int64_t chat_id, int64_t user_id) {
    if (isSudo(user_id)) return true;
    if (isChatAdmin(chat_id, user_id)) return true;
    if (isAuthUser(chat_id, user_id)) return true;
    return settingsFor(chat_id).admin_mode == 2;
}

void Dispatcher::refreshPanel(const TgCallbackQuery& cq, int64_t chat_id) {
    Json np, track;
    std::string now_title;
    std::string now_url;
    std::string next_title;
    std::string next_url;
    if (nowPlaying(chat_id, np, track)) {
        now_title = track.find("title") ? track.find("title")->asString()
                                        : "";
        now_url = track.find("url") ? track.find("url")->asString() : "";
        CommandContext fake;
        fake.message.chat.id = chat_id;
        Json body = Json::objectOf({
            {"chat_id", Json(chat_id)},
            {"action", Json("list")},
        });
        IpcResult q = vcCall(fake, "queue", body);
        if (q.ok) {
            const Json* items = q.data.find("queue");
            // Waiting-only list (§1.7): the next track is items[0],
            // never items[1] — the current track is not in the list.
            if (items && items->size() > 0) {
                const Json* nt =
                    items->asArray()[0].find("track");
                if (nt) {
                    next_title =
                        nt->find("title") ? nt->find("title")->asString()
                                          : "";
                    next_url =
                        nt->find("url") ? nt->find("url")->asString() : "";
                }
            }
        }
    }
    std::string text = ui_texts::menuPanelText(
        ui_texts::htmlEscape(now_title), now_url,
        ui_texts::htmlEscape(next_title), next_url);
    std::string err;
    tg_.editMessageText(chat_id, cq.message_id, text,
                        ui_texts::menuPanelKeyboard(chat_id),
                        &err);
}

void Dispatcher::refreshCard(const TgCallbackQuery& cq, int64_t chat_id) {
    Json np, track;
    if (!nowPlaying(chat_id, np, track)) return;
    int64_t card_id = 0;
    bool is_photo = false;
    if (!cardInfo(chat_id, card_id, is_photo) || card_id != cq.message_id)
        return;
    refreshCardText(chat_id, cq.message_id, is_photo, np);
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
    if (settingsFor(ctx.message.chat.id).cmd_delete)
        tg_.deleteMessage(ctx.message.chat.id, ctx.message.message_id);
}

// One POST to the single unified backend over the persistent UDS
// socket (contracts/uds-protocol.md). There is exactly one backend,
// so there is no per-service URL. No fallback, no /healthz pre-probe
// — a failed call reports the backend's exact error, never a silent
// retry elsewhere.
IpcResult Dispatcher::vcCall(const CommandContext& ctx, const std::string& action,
                             const Json& body) {
    (void)ctx;
    return ipc_.post(action, body, 30);
}

IpcResult Dispatcher::vcGet(const CommandContext& ctx, const std::string& action) {
    // GET /v1/<action>?chat_id=... on the one backend (10 s budget).
    // IpcClient::get translates the legacy path to the dotted contract
    // action.
    std::string path = "/v1/" + action +
                       "?chat_id=" + std::to_string(vcTarget(ctx));
    return ipc_.get(path, 10);
}

bool Dispatcher::nowPlaying(int64_t chat_id, Json& np_out, Json& track_out) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    IpcResult r = vcGet(fake, "now-playing");
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
// from the backend (same vcGet as the Telegram commands).
bool Dispatcher::apiNowPlaying(int64_t chat_id, Json& np_out,
                               std::string& error) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    IpcResult r = vcGet(fake, "now-playing");
    if (!r.ok) {
        error = r.describe();
        return false;
    }
    const Json* np = r.data.find("now_playing");
    if (!np || !np->isObject()) {
        error = "engine response has no now_playing object";
        return false;
    }
    // The backend answers 200 with an empty room when nothing plays:
    // a null track AND an empty queue is no playback state, not a track.
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

// HTTP API seam: the backend's queue list (POST /v1/queue action=list).
bool Dispatcher::apiQueueList(int64_t chat_id, Json& queue_out,
                              std::string& error) {
    CommandContext fake;
    fake.message.chat.id = chat_id;
    Json body = Json::objectOf({{"action", Json("list")},
                                {"chat_id", Json(chat_id)}});
    IpcResult r = vcCall(fake, "queue", body);
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

// HTTP API seam: vc.join (POST /v1/join). The backend owns the call:
// a vc.join asks the backend to join, and the backend drives the
// duplex JoinRequest handshake (onJoinRequest) for the TDLib
// joinVideoChat half. Same join as /join, minus the Telegram
// permission check and reply.
bool Dispatcher::joinVoice(int64_t chat_id, std::string& error) {
    // Already in a call: nothing to join.
    if (callActive(chat_id)) return true;
    CommandContext fake;
    fake.message.chat.id = chat_id;
    IpcResult r = vcCall(fake, "join",
                         Json::objectOf({{"chat_id", Json(chat_id)}}));
    if (!r.ok) {
        error = r.describe();
        return false;
    }
    setCallActive(chat_id, true);
    return true;
}

// HTTP API seam: vc.mute (POST /v1/mute). Same mute as /mute, minus
// the Telegram permission check and reply. The "already muted/unmuted"
// short-circuit of /mute is NOT reproduced here — the endpoint reports
// the requested state after a real backend mute/unmute (or an honest
// error), so a repeated mute is a success, not a refusal.
bool Dispatcher::muteVoice(int64_t chat_id, bool mute,
                           std::string& error) {
    Json np, track;
    if (!nowPlaying(chat_id, np, track) && !callActive(chat_id)) {
        error = "Nothing is playing.";
        return false;
    }
    CommandContext fake;
    fake.message.chat.id = chat_id;
    IpcResult r = vcCall(fake, "mute",
                         Json::objectOf({{"chat_id", Json(chat_id)},
                                         {"muted", Json(mute)}}));
    if (!r.ok) {
        error = r.describe();
        return false;
    }
    setChatRtField(chat_id, "muted", mute ? "1" : "0");
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
// Clean-mode sweeper only. The 5-second loop/autoplay watcher and the
// core-follow pass are DELETED (Phase 2): track transitions are
// backend-driven events (onBackendEvent), and autoplay is the
// backend's job — the core no longer polls engine state on a timer.
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
    // The sweeper runs on the background thread, and TDLib calls belong
    // on the main thread (see the header note): queue the deletes and
    // let processPendingWork() run them.
    for (auto& d : due) {
        std::lock_guard<std::mutex> lk(work_mu_);
        pending_work_.push_back(
            [this, chat = d.chat, message = d.message] {
                tg_.deleteMessage(chat, message);
            });
    }
}

// --- backend-driven duplex hooks (Phase 2) ----------------------------------
// onBackendEvent runs on the IpcClient reader thread: enqueue only,
// never touch TDLib here.
void Dispatcher::onBackendEvent(const std::string& event, const Json& data) {
    std::lock_guard<std::mutex> lk(work_mu_);
    pending_work_.push_back(
        [this, event, data] { onTrackEvent(event, data); });
}

// onJoinRequest runs on an IpcClient worker thread: execute the TDLib join
// directly on the shared assistant session and return the response.
// Previously enqueued to main thread, which caused a distributed deadlock
// when main thread was blocked inside vcCall("play").
Json Dispatcher::onJoinRequest(const Json& request) {
    const Json* chat = request.find("chat_id");
    const Json* sdp = request.find("local_sdp");
    if (!chat || !sdp || sdp->asString().empty()) {
        return Json::objectOf(
            {{"ok", Json(false)},
             {"error",
              Json::objectOf(
                  {{"code", Json("bad_request")},
                   {"message",
                    Json("JoinRequest needs chat_id + local_sdp")}})}});
    }
    Json resp;
    std::string err;
    if (doJoinVideoChat(chat->asInt(), sdp->asString(), resp, err)) {
        return Json::objectOf(
            {{"ok", Json(true)}, {"data", resp}});
    }
    return Json::objectOf(
        {{"ok", Json(false)},
         {"error",
          Json::objectOf({{"code", Json("internal")},
                          {"message", Json(err)}})}});
}

// MAIN THREAD ONLY: run queued outbound work, then pending joins.
void Dispatcher::processPendingWork() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lk(work_mu_);
        work.swap(pending_work_);
    }
    for (auto& w : work) {
        try {
            w();
        } catch (const std::exception& e) {
            std::cerr << "[dispatcher] queued work threw: " << e.what()
                      << "\n";
        }
    }
    std::vector<PendingJoin> joins;
    {
        std::lock_guard<std::mutex> lk(joins_mu_);
        joins.swap(pending_joins_);
    }
    for (auto& j : joins) {
        Json resp;
        std::string err;
        if (doJoinVideoChat(j.chat_id, j.local_sdp, resp, err)) {
            j.done->set_value(Json::objectOf(
                {{"ok", Json(true)}, {"data", resp}}));
        } else {
            j.done->set_value(Json::objectOf(
                {{"ok", Json(false)},
                 {"error",
                  Json::objectOf({{"code", Json("internal")},
                                  {"message", Json(err)}})}}));
        }
    }
}

// Main-thread handling of one dequeued backend event.
void Dispatcher::onTrackEvent(const std::string& event, const Json& data) {
    const Json* chat = data.find("chat_id");
    int64_t chat_id = chat ? chat->asInt() : 0;
    if (chat_id == 0) return;
    if (event == "TrackStarted" || event == "TrackAdvanced" ||
        event == "TrackChanged") {
        // The backend advanced the track: refresh the Telegram card
        // from the backend's now-playing (the event is the trigger;
        // the backend is the source of truth for the track).
        Json np, track;
        if (!nowPlaying(chat_id, np, track)) return;
        editNowPlayingCard(chat_id, np);
        return;
    }
    if (event == "TrackEnded" || event == "QueueEmpty") {
        // Nothing follows this track: the backend stopped, so the call
        // is inactive, and the recorded card goes idle instead of
        // showing a stale track.
        setCallActive(chat_id, false);
        int64_t card_id = 0;
        bool is_photo = false;
        if (!cardInfo(chat_id, card_id, is_photo)) return;
        std::string err;
        InlineKeyboard empty;
        if (is_photo)
            tg_.editMessageCaption(chat_id, card_id, ui_texts::kNowIdle,
                                   empty, &err);
        else
            tg_.editMessageText(chat_id, card_id, ui_texts::kNowIdle, empty,
                                &err);
        return;
    }
    if (event == "VoiceError") {
        const Json* msg = data.find("message");
        std::string text = msg ? msg->asString() : "voice error";
        const Json* code = data.find("code");
        std::string err;
        tg_.sendMessage(chat_id,
                        "Voice error" +
                            (code ? " (" + code->asString() + ")" : "") +
                            ": " + text,
                        err);
        return;
    }
    // DownloadReady and unknown events: no UI action (forward
    // compatibility — unknown names are ignored per the protocol).
}

// Edit the recorded now-playing card for a chat in place; send a fresh
// card when none is recorded.
void Dispatcher::editNowPlayingCard(int64_t chat_id, const Json& np) {
    int64_t card_id = 0;
    bool is_photo = false;
    if (cardInfo(chat_id, card_id, is_photo)) {
        refreshCardText(chat_id, card_id, is_photo, np);
        return;
    }
    sendNowPlayingCard(chat_id, np, "");
}

// Render the now-playing card text + keyboard for a chat and edit the
// given card message with it.
void Dispatcher::refreshCardText(int64_t chat_id, int64_t message_id,
                                 bool is_photo, const Json& np) {
    const Json* track = np.find("track");
    if (!track || track->isNull()) return;
    std::string requester;
    auto rn = store_.get("last_requester_name:" + std::to_string(chat_id));
    requester = rn ? *rn : "";
    ui_texts::CardTrack ct = cardTrack(*track, requester);
    int64_t pos = np.find("position_sec") ? np.find("position_sec")->asInt()
                                          : 0;
    int64_t dur =
        track->find("duration_sec") ? track->find("duration_sec")->asInt()
                                    : 0;
    bool paused = false;
    if (const Json* p = np.find("paused")) paused = p->asBool();
    if (const Json* p = np.find("is_paused")) paused = p->asBool();
    bool autoplay = false;
    if (const Json* a = np.find("autoplay")) autoplay = a->asBool();
    if (const Json* a = np.find("autoplay_enabled")) autoplay = a->asBool();
    int loop = 0;
    if (const Json* l = np.find("loop")) loop = static_cast<int>(l->asInt());
    if (const Json* l = np.find("loop_count"))
        loop = static_cast<int>(l->asInt());
    if (loop == 0) loop = chatRtIntField(chat_id, "loop", 0);
    std::string text = ui_texts::nowPlayingText(ct);
    InlineKeyboard kb = ui_texts::playerKeyboard(
        chat_id, paused, autoplay, loop, ct.token, true, pos, dur);
    std::string err;
    if (is_photo)
        tg_.editMessageCaption(chat_id, message_id, text, kb, &err);
    else
        tg_.editMessageText(chat_id, message_id, text, kb, &err);
}

// --- duplex join: TDLib signalling on the shared assistant session --------
// Ported from the deleted TdLibVoiceCall (voice_call.cpp) minus the
// ntgcalls half: the backend owns the call and the audio, the core
// only runs getChat / createVideoChat / joinVideoChat and hands the
// remote transport payload back.
int32_t Dispatcher::groupCallId(int64_t chat_id, std::string& error) {
    Json req = Json::objectOf({
        {"@type", Json("getChat")},
        {"chat_id", Json(chat_id)},
    });
    Json resp = assistant_.call(req, 30.0);
    if (resp.isNull()) {
        error = "getChat timed out for chat " + std::to_string(chat_id);
        return -1;
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        error = "getChat failed: " +
                (msg ? msg->asString() : std::string("?"));
        return -1;
    }
    const Json* vc = resp.find("video_chat");
    if (!vc) {
        error = "getChat response has no video_chat";
        return -1;
    }
    const Json* gcid = vc->find("group_call_id");
    return gcid ? static_cast<int32_t>(gcid->asInt(0)) : 0;
}

int32_t Dispatcher::createVideoChat(int64_t chat_id, const std::string& title,
                                    std::string& error) {
    Json req = Json::objectOf({
        {"@type", Json("createVideoChat")},
        {"chat_id", Json(chat_id)},
        {"title", Json(title)},
        {"start_date", Json(0)},
        {"is_rtmp_stream", Json(false)},
    });
    Json resp = assistant_.call(req, 30.0);
    if (resp.isNull()) {
        error = "createVideoChat timed out for chat " +
                std::to_string(chat_id);
        return 0;
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        error = "createVideoChat failed: " +
                (msg ? msg->asString() : std::string("?"));
        return 0;
    }
    const Json* id = resp.find("id");
    return id ? static_cast<int32_t>(id->asInt(0)) : 0;
}

std::string Dispatcher::joinVideoChat(int32_t group_call_id,
                                      const std::string& local_sdp,
                                      bool video, std::string& error) {
    int32_t audio_source_id = 0;
    try {
        Json sdp_json = Json::parse(local_sdp);
        const Json* ssrc_val = sdp_json.find("ssrc");
        if (ssrc_val) {
            audio_source_id = static_cast<int32_t>(ssrc_val->asInt(0));
        }
    } catch (...) {}
    if (audio_source_id == 0) {
        audio_source_id = 1;
    }

    Json join_params = Json::objectOf({
        {"@type", Json("groupCallJoinParameters")},
        {"audio_source_id", Json(static_cast<int64_t>(audio_source_id))},
        {"payload", Json(local_sdp)},
        {"is_muted", Json(false)},
        {"is_my_video_enabled", Json(video)},
    });
    Json req = Json::objectOf({
        {"@type", Json("joinVideoChat")},
        {"group_call_id", Json(static_cast<int64_t>(group_call_id))},
        {"participant_id", Json(nullptr)},
        {"join_parameters", join_params},
        {"invite_hash", Json("")},
    });
    Json resp = assistant_.call(req, 30.0);
    if (resp.isNull()) {
        error = "joinVideoChat timed out for group call " +
                std::to_string(group_call_id);
        return "";
    }
    const Json* type = resp.find("@type");
    if (type && type->asString() == "error") {
        const Json* msg = resp.find("message");
        error = "joinVideoChat failed: " +
                (msg ? msg->asString() : std::string("?"));
        return "";
    }
    // joinVideoChat returns Text; .text is the remote transport payload
    // for tgcalls (td_api.tl: "Returns join response payload for tgcalls").
    const Json* text = resp.find("text");
    if (!text || text->asString().empty()) {
        error = "joinVideoChat returned no remote payload (type: " +
                (type ? type->asString() : std::string("?")) + ")";
        return "";
    }
    return text->asString();
}

bool Dispatcher::doJoinVideoChat(int64_t chat_id,
                                 const std::string& local_sdp,
                                 Json& response_out, std::string& error) {
    if (!assistant_.available()) {
        error = "assistant session unavailable: " +
                assistant_.lastError();
        return false;
    }
    if (!assistant_.authorized()) {
        error = "assistant session not authorized: " +
                assistant_.lastError();
        return false;
    }
    // Resolve the chat's video chat; create one when none exists.
    int32_t gcid = groupCallId(chat_id, error);
    if (gcid < 0) return false;
    if (gcid == 0) {
        gcid = createVideoChat(chat_id, "Parlay", error);
        if (gcid == 0) return false;
    }
    std::string remote = joinVideoChat(gcid, local_sdp, false, error);
    if (remote.empty()) return false;
    response_out = Json::objectOf(
        {{"remote_sdp", Json(remote)},
         {"group_call_id", Json(static_cast<int64_t>(gcid))}});
    setCallActive(chat_id, true);
    return true;
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
    std::string arg = lowerStr(trim(ctx.args));
    // Inline deep link: /start spotify----{id} (§13 lemonic) — the
    // [Open in Parlay Bot] button on an inline search result. The
    // id is case-sensitive (yt:/lemonic: ids), so the raw args are
    // used, not the lowercased arg above.
    std::string raw_arg = trim(ctx.args);
    if (raw_arg.rfind("spotify----", 0) == 0) {
        std::string track_id = raw_arg.substr(11);
        if (!track_id.empty()) {
            deepLinkDownload(ctx, track_id);
            return;
        }
    }
    // Deep links: /start help | /start pm_help -> the help menu.
    if (arg == "help" || arg == "pm_help") {
        if (isPrivate(ctx)) {
            replyKb(ctx, ui_texts::kHelpMenu,
                    ui_texts::helpMenuKeyboard());
        } else {
            replyHtml(ctx, ui_texts::kHelpGroup);
        }
        return;
    }
    // Share deep link: /start share_<token> (share-rust mints
    // share_…) — also accept the hyphen form the inventory writes.
    if (arg.rfind("share_", 0) == 0 || arg.rfind("share-", 0) == 0) {
        std::string token = arg.substr(6);
        if (!token.empty()) {
            // (1) A token the core minted at /share time maps to the
            // link owner: the send-music flow.
            auto target = store_.get("sharetok:" + token);
            if (target) {
                int64_t target_id = 0;
                try {
                    target_id = std::stoll(*target);
                } catch (...) {
                }
                if (target_id != 0) {
                    if (target_id == uid) {
                        replyHtml(ctx, ui_texts::kShareSelf);
                        return;
                    }
                    store_.set("sharetarget:" + std::to_string(uid),
                               std::to_string(target_id));
                    InlineKeyboard kb = {{InlineButton::callback(
                        ui_texts::kShareCancel, ui_texts::kCbClose)}};
                    replyKb(ctx,
                            ui_texts::fmt(ui_texts::kShareSendPrompt,
                                          {{"name", ui_texts::htmlEscape(
                                                         tg_.getUserMention(
                                                             target_id))}}),
                            kb);
                    return;
                }
            }
            // (2) Otherwise it is a track token: resolve it via
            // the backend (share.resolve) and present the track.
            IpcResult r = ipc_.get("/v1/share/" + token, 15);
            if (r.ok) {
                const Json* track = r.data.find("track");
                if (track) {
                    ui_texts::CardTrack ct =
                        cardTrack(*track, "");
                    replyHtml(ctx,
                              ui_texts::fmt(
                                  ui_texts::kShareTrackOpened,
                                  {{"track", ui_texts::trackLink(ct)},
                                   {"title",
                                    track->find("title")
                                        ? track->find("title")->asString()
                                        : ""}}));
                    return;
                }
            }
            replyHtml(ctx, ui_texts::kShareInvalidLink);
            return;
        }
    }
    if (isPrivate(ctx)) {
        // The navidrome private-bot notice only applies when Parlay
        // runs in private mode; there is no such config yet, so the
        // normal start card is shown (kPrivateBotNotice is kept in
        // ui_texts.hpp for that mode — see phase9/port-core.md).
        if (!cfg_.start_img.empty()) {
            replyPhoto(ctx, cfg_.start_img, ui_texts::kStartPm,
                       ui_texts::startPmKeyboard(
                           bot_username_, cfg_.updates_url, cfg_.support_url,
                           cfg_.source_url));
        } else {
            replyKb(ctx, ui_texts::kStartPm,
                    ui_texts::startPmKeyboard(bot_username_, cfg_.updates_url,
                                              cfg_.support_url,
                                              cfg_.source_url));
        }
        return;
    }
    replyKb(ctx, ui_texts::kStartGroup,
            ui_texts::startGroupKeyboard(cfg_.updates_url, cfg_.support_url));
}

void Dispatcher::onHelp(const CommandContext& ctx) {
    // In a group, /help points at the DM (all four refs agree).
    if (isGroup(ctx)) {
        std::string url = "https://t.me/" + bot_username_ +
                          "?start=pm_help";
        InlineKeyboard kb = {
            {InlineButton::link("Commands", url)}};
        replyKb(ctx, ui_texts::kHelpGroup, kb);
        return;
    }
    std::string arg = lowerStr(trim(ctx.args));
    if (!arg.empty()) {
        // /help <topic> or /help <command>: topic pages first, then
        // the per-command table (§3).
        std::string topic_text = ui_texts::helpTopicText(arg);
        if (!topic_text.empty()) {
            replyKb(ctx, topic_text, ui_texts::helpTopicKeyboard());
            return;
        }
        const ui_texts::CmdHelp* h = ui_texts::findCmdHelp(arg);
        if (h) {
            replyHtml(ctx, ui_texts::perCommandHelpText(h->cmd));
            return;
        }
        replyHtml(ctx, ui_texts::fmt(ui_texts::kHelpUnknownCmd,
                                     {{"cmd", arg}}));
        return;
    }
    replyKb(ctx, ui_texts::kHelpMenu, ui_texts::helpMenuKeyboard());
}

// --- handlers: playback ------------------------------------------------------
void Dispatcher::onPlay(const CommandContext& ctx) {
    if (!isGroup(ctx)) {
        reply(ctx, "Music only plays in groups (supergroups).");
        return;
    }
    // Flags: -f (force) -v (video) -s (shuffle, playplaylist only).
    // -v is accepted and ignored: the backend plays audio in Phase 3
    // (no video stream), so no `video` field is sent (§10-D13).
    auto words = splitArgs(ctx.args);
    bool flag_f = false;
    std::string query;
    for (const auto& w : words) {
        if (w == "-f") flag_f = true;
        else if (w == "-v") { /* video: no backend effect */
        } else if (w == "-s") { /* shuffle: only for /playplaylist */
        } else {
            if (!query.empty()) query += " ";
            query += w;
        }
    }
    // `force` is core-side only (stop-then-play below): it is NOT
    // sent in the play body (§10-D13 — the contract lists no
    // `force` field, and the backend ignores unknown fields).
    bool force = flag_f ||
                 ctx.command == "playforce" || ctx.command == "vplayforce";
    // Reply-to media (or media sent with the command as its caption):
    // /play on a replied audio/video/voice/video_note/document plays
    // that Telegram file (source "file"). The core downloads it via
    // the TDLib bot session and hands the local path to the backend
    // with vc.play_file (§3.3) — the file is never re-uploaded over
    // UDS. No query is needed.
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
            // The replied-file path issues its own backend IPC
            // (vc.play_file) and its own telemetry line.
            playRepliedFile(ctx, file_id, title, kind);
            return;
        }
        emitDispatchToIpc(telemetry::nowUs() - receiptUs(ctx), -1, -1,
                          vcTarget(ctx), false, "no_query");
        replyHtml(ctx, ui_texts::fmt(ui_texts::kNoSongQuery,
                                     {{"cmd", "/play"}}));
        return;
    }
    if (!playAllowed(ctx, force)) return;
    if (ctx.via_channel && chatRtField(ctx.message.chat.id, "cplay", "0") == "0") {
        reply(ctx, "No channel linked. Use /channelplay first.");
        return;
    }
    // Duration limit (NoxBeats DURATION_LIMIT, default 60 min): enforced
    // on the resolved track after the play call when the engine reports
    // duration_sec; a too-long track is stopped right away.
    if (force) {
        // playforce: replace the current track (queue().force_add shape).
        Json stop_body =
            Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
        vcCall(ctx, "stop", stop_body);
    }
    // What was playing before this call: a play that lands while
    // something is already playing queues the new track instead, and
    // the reply is the queued card (§4), not the now-playing card.
    Json np_before, track_before;
    bool was_active = nowPlaying(vcTarget(ctx), np_before, track_before);
    std::string old_id =
        track_before.find("id") ? track_before.find("id")->asString() : "";
    Json body = vcPlayBody(vcTarget(ctx), trim(query),
                           ctx.message.from.id);
    // Phase 1 telemetry: dispatch -> IPC POST issued, and POST
    // issued -> response received. "POST issued" is measured at the
    // vcCall entry (the core handing the play to the IPC layer); the
    // UDS round-trip runs inside vcCall, so it falls in
    // post_to_response_us; the total is exact either way. The line
    // fires whether the backend answers or not (ok=0 + error).
    const int64_t t_post = telemetry::nowUs();
    IpcResult r = vcCall(ctx, "play", body);
    const int64_t t_resp = telemetry::nowUs();
    emitDispatchToIpc(t_resp - receiptUs(ctx), t_post - receiptUs(ctx),
                      t_resp - t_post, vcTarget(ctx), r.ok,
                      r.ok ? ""
                           : r.error_code + ": " + r.error_message);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    finishPlay(ctx, vcTarget(ctx), r, query, was_active, old_id);
}

// --- shared vc.play / vc.play_file rendering -------------------------------
// The vc.play and vc.play_file responses are the same shape
// ({now_playing}, plus the `queued` superset field), so both handlers
// share this tail. `r` must be ok — the caller reports the backend
// error itself.
Json Dispatcher::vcPlayBody(int64_t chat, const std::string& query,
                            int64_t requested_by) {
    // §4.4: the room carries the chat's stored speed/bass on every
    // play (record-only until the Phase 5 DSP); the backend reads
    // them off this body. No `video` / `force` fields (§10-D13).
    return Json::objectOf({
        {"chat_id", Json(chat)},
        {"query", Json(query)},
        {"requested_by", Json(requested_by)},
        {"speed", Json(chatRtSpeedField(chat))},
        {"bass", Json(static_cast<int64_t>(chatRtIntField(chat, "bass", 0)))},
    });
}

void Dispatcher::finishPlay(const CommandContext& ctx, int64_t chat,
                            const IpcResult& r, const std::string& query,
                            bool was_active, const std::string& old_id) {
    const Json* np = r.data.find("now_playing");
    Json now = np ? *np : r.data;
    const Json* track = now.find("track");
    if (track) {
        int64_t dur =
            track->find("duration_sec") ? track->find("duration_sec")->asInt() : 0;
        if (dur > 0 && dur > cfg_.duration_limit_min * 60) {
            vcCall(ctx, "stop",
                   Json::objectOf({{"chat_id", Json(chat)}}));
            replyHtml(ctx, ui_texts::playTooLongText(cfg_.duration_limit_min));
            return;
        }
    }
    // The backend's play is the play: it owns the call and the audio,
    // so a successful vc.play means the track is really playing — no
    // core-side attach step exists any more. On failure the exact
    // backend error was already reported above, never a "Now playing"
    // over a silent call. The backend accepted the play, so it is in
    // the call for this chat: record that (the /pause-style gates read
    // it); /stop and the TrackEnded/QueueEmpty events clear it.
    setCallActive(chat, true);
    if (track) {
        setChatRtField(chat, "last_track",
                     track->find("title") ? track->find("title")->asString() : "");
        store_.set("last_requester_name:" + std::to_string(chat),
                   displayName(ctx.message.from));
        store_.set("last_requester_id:" + std::to_string(chat),
                   std::to_string(ctx.message.from.id));
    }
    // The per-chat logger_on field was never written anywhere (only
    // the global logger_on key is, by /logger) — the read is dropped
    // with the Q7 rewire; the global key stays the toggle.
    if (store_.get("logger_on") == std::optional<std::string>("1"))
        logToLogger("Play in " + std::to_string(chat) + ": " +
                    (track && track->find("title")
                         ? track->find("title")->asString()
                         : query));
    // Queued or playing? The backend says so outright ("queued":
    // true); it also queues whenever something was already playing
    // and the current track did not change.
    bool queued = false;
    if (const Json* q = r.data.find("queued")) queued = q->asBool();
    if (!queued && was_active && track) {
        std::string new_id =
            track->find("id") ? track->find("id")->asString() : "";
        queued = !old_id.empty() && new_id == old_id;
    }
    if (queued) {
        // The new track is the tail of the waiting queue: render the
        // queued card for it (Yukki §4) with the Play Now button.
        // The queue list is waiting-only (§1.7), so the tail's
        // display position is the list size.
        Json qbody = Json::objectOf({
            {"chat_id", Json(chat)},
            {"action", Json("list")},
        });
        IpcResult qr = vcCall(ctx, "queue", qbody);
        const Json* items = qr.ok ? qr.data.find("queue") : nullptr;
        if (items && items->size() > 0) {
            const Json& last = items->asArray()[items->size() - 1];
            const Json* lt = last.find("track");
            if (lt) {
                int64_t by = last.find("requested_by")
                                 ? last.find("requested_by")->asInt()
                                 : ctx.message.from.id;
                ui_texts::CardTrack ct = cardTrack(*lt, by);
                std::string text = ui_texts::queuedText(
                    static_cast<int>(items->size()), ct);
                std::string thumb = effectiveThumb(
                    chat, lt->find("thumbnail")
                              ? lt->find("thumbnail")->asString()
                              : "");
                InlineKeyboard kb = ui_texts::queuedKeyboard(ct.token);
                if (!thumb.empty()) {
                    if (replyPhoto(ctx, thumb, text, kb) > 0) {
                        deleteCommandMessage(ctx);
                        return;
                    }
                }
                replyKb(ctx, text, kb);
                deleteCommandMessage(ctx);
                return;
            }
        }
        // Queue list unavailable: the current track is still what
        // is playing, so show its card (the queued track is visible
        // in /queue). Never claim the queued track is playing.
        sendNowPlayingCard(chat, now, "Now playing");
        deleteCommandMessage(ctx);
        return;
    }
    std::string state_line = "Now playing";
    if (chatRtIntField(chat, "muted", 0))
        state_line += "\n(muted — use /unmute)";
    sendNowPlayingCard(chat, now, state_line);
    deleteCommandMessage(ctx);
}

// --- /play from a replied-to Telegram file ----------------------------------
// The file is downloaded via the TDLib bot session, then handed to
// the backend with vc.play_file (§3.3, §9-E12): the core already has
// the file on disk, so the action carries a path, not bytes (the UDS
// frame cap is 16 MiB). Duration is unknown for replied media, so it
// is omitted (0 when unknown). Play-now-if-idle else enqueue, exactly
// like vc.play — the backend takes ownership of the file.
bool Dispatcher::playRepliedFile(const CommandContext& ctx,
                                 const std::string& file_id,
                                 const std::string& title,
                                 const std::string& kind) {
    int64_t chat = vcTarget(ctx);
    std::string saved, err;
    if (!tg_.downloadFile(file_id, cfg_.data_dir + "/replied", saved, err)) {
        reply(ctx, "Could not fetch the replied file: " + err);
        return false;
    }
    std::string track_title = title.empty() ? "Replied file" : title;
    setChatRtField(chat, "last_track", track_title);
    // What was playing before this call: a play_file that lands while
    // something is already playing queues the file instead, and the
    // reply is the queued card (§4), not the now-playing card.
    Json np_before, track_before;
    bool was_active = nowPlaying(chat, np_before, track_before);
    std::string old_id =
        track_before.find("id") ? track_before.find("id")->asString() : "";
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"file_path", Json(saved)},
        {"file_id", Json(file_id)},
        {"title", Json(track_title)},
        {"kind", Json(kind)},
        {"requested_by", Json(ctx.message.from.id)},
    });
    const int64_t t_post = telemetry::nowUs();
    IpcResult r = vcCall(ctx, "vc.play_file", body);
    const int64_t t_resp = telemetry::nowUs();
    emitDispatchToIpc(t_resp - receiptUs(ctx), t_post - receiptUs(ctx),
                      t_resp - t_post, chat, r.ok,
                      r.ok ? ""
                           : r.error_code + ": " + r.error_message);
    if (!r.ok) {
        replyError(ctx, r);
        return false;
    }
    finishPlay(ctx, chat, r, track_title, was_active, old_id);
    return true;
}

void Dispatcher::onPause(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = vcCall(ctx, "pause", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    Json state = np ? *np : r.data;
    std::string state_line = ui_texts::kCtlPaused;
    // The backend owns the call and the audio: its pause is the
    // pause. No core-side voice step remains.
    if (state.find("track"))
        sendNowPlayingCard(vcTarget(ctx), state, state_line);
    else
        replyHtml(ctx, state_line);
}

void Dispatcher::onResume(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = vcCall(ctx, "resume", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    Json state = np ? *np : r.data;
    std::string state_line = ui_texts::kCtlResumed;
    // Mirror of onPause: the backend's resume is the resume.
    if (state.find("track"))
        sendNowPlayingCard(vcTarget(ctx), state, state_line);
    else
        replyHtml(ctx, state_line);
}

void Dispatcher::onSkip(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = vcCall(ctx, "skip", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    const Json* track = np ? np->find("track") : nullptr;
    if (np && track && !track->isNull()) {
        // The backend moved to a new track and owns its audio: render
        // the new card. A backend failure would already have been
        // reported above via replyError.
        sendNowPlayingCard(vcTarget(ctx), *np, ui_texts::kCtlSkipped);
    } else {
        // Queue empty: the backend stopped, so the call is inactive.
        setCallActive(vcTarget(ctx), false);
        reply(ctx, "Skipped — the queue is now empty.");
    }
}

// /autoplay-skip, /askip (Phase 6): force an autoplay mix pick now.
// Contract vc.autoplay_skip (POST /v1/autoplay-skip). Same shape as
// onSkip: the backend answers {now_playing} with the picked track;
// a 400 "autoplay is off" or a 502 "no autoplay recommendation"
// surfaces honestly via replyError.
void Dispatcher::onAutoplaySkip(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        reply(ctx, "Nothing is playing.");
        return;
    }
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = vcCall(ctx, "autoplay-skip", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* np = r.data.find("now_playing");
    const Json* track = np ? np->find("track") : nullptr;
    if (np && track && !track->isNull()) {
        sendNowPlayingCard(vcTarget(ctx), *np,
                           "Autoplay skipped — now playing");
    } else {
        setCallActive(vcTarget(ctx), false);
        reply(ctx, "Autoplay skipped — the queue is now empty.");
    }
}

void Dispatcher::onStop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    bool was_active = callActive(vcTarget(ctx));
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = vcCall(ctx, "stop", body);
    setCallActive(vcTarget(ctx), false);
    // The backend owns the call: its stop ends the session. Reset the
    // per-chat playback flags the core keeps.
    setChatRtField(vcTarget(ctx), "loop", "0");
    setChatRtField(vcTarget(ctx), "speed", "1.0");
    setChatRtField(vcTarget(ctx), "bass", "0");
    setChatRtField(vcTarget(ctx), "muted", "0");
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    if (was_active)
        replyHtml(ctx, ui_texts::fmt(ui_texts::kStoppedCard,
                                     {{"user", ui_texts::htmlEscape(
                                                   userLabel(ctx))}}));
    else
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
}

void Dispatcher::onQueue(const CommandContext& ctx) {
    if (trim(ctx.args) == "help") {
        replyHtml(ctx, ui_texts::kQueueHelpOps);
        return;
    }
    renderQueueCard(ctx, false);
}

void Dispatcher::onLoop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    auto args = splitArgs(ctx.args);
    int64_t chat = vcTarget(ctx);
    if (args.empty()) {
        int n = chatRtIntField(chat, "loop", 0);
        replyHtml(ctx, n > 0 ? ui_texts::kCtlLoopOn
                             : "Usage: /loop <1-10|off>");
        return;
    }
    std::string a = lowerStr(args[0]);
    int64_t count = -1;
    if (a == "off" || a == "disable") {
        count = 0;
    } else if (isDigits(a)) {
        long n = std::stol(a.substr(0, 9));
        if (n < 1) n = 0;
        if (n > 10) n = 10;
        count = n;
    } else {
        reply(ctx, "Usage: /loop <1-10|off>");
        return;
    }
    // Native vc.loop (§1.8, §10-D3): the backend replays the current
    // track `count` times at the natural track end (0 = off). The old
    // copy-queueing path is deleted — queueing N copies AND setting
    // the loop count would loop the track 2N times.
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"count", Json(count)},
    });
    IpcResult r = vcCall(ctx, "loop", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    setChatRtField(chat, "loop", std::to_string(count));
    replyHtml(ctx, count > 0 ? ui_texts::kCtlLoopOn
                             : ui_texts::kCtlLoopOff);
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
    IpcResult r = vcCall(ctx, "seek", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    std::string text = std::string("Seeked ") +
                       (ctx.command == "seekback" ? "backward" : "forward") +
                       " to " + fmtSeconds(target) + ".";
    // The backend owns the call and the audio: its seek is the seek.
    // No core-side re-attach step remains.
    reply(ctx, text);
}

void Dispatcher::onClear(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(ctx, "queue", list_body);
    if (q.ok) {
        const Json* items = q.data.find("queue");
        // Waiting-only list (§1.7): every item is a queued track,
        // so a non-empty list is worth saving for /restore.
        if (items && items->size() > 0)
            store_.set("cleared:" + std::to_string(vcTarget(ctx)),
                       items->dump());
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("clear")},
    });
    IpcResult r = vcCall(ctx, "queue", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    int cleared = 0;
    {
        // The snapshot saved above (before the clear) is the waiting
        // queue: every item in it was cleared (the current track
        // keeps playing — it was never in the list, §1.7).
        auto snap = store_.get("cleared:" + std::to_string(vcTarget(ctx)));
        if (snap) {
            try {
                Json arr = Json::parse(*snap);
                cleared = static_cast<int>(arr.size());
            } catch (...) {
            }
        }
    }
    if (cleared == 0) {
        replyHtml(ctx, ui_texts::kQueueAlreadyEmpty);
        return;
    }
    replyHtml(ctx, ui_texts::queueClearedText(cleared, userLabel(ctx)));
}

void Dispatcher::onRestore(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto saved = store_.get("cleared:" + std::to_string(vcTarget(ctx)));
    if (!saved || saved->empty()) {
        replyHtml(ctx, ui_texts::kQueueRestoreNone);
        return;
    }
    Json arr;
    try {
        arr = Json::parse(*saved);
    } catch (...) {
        replyHtml(ctx, ui_texts::kQueueRestoreNone);
        return;
    }
    int added = 0;
    // The saved queue is the waiting queue (§1.7): restore every
    // item — nothing is skipped, the current track was never in it.
    for (size_t i = 0; i < arr.asArray().size(); ++i) {
        const Json& item = arr.asArray()[i];
        const Json* track = item.find("track");
        if (!track) continue;
        Json body = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", *track},
            {"requested_by", Json(ctx.message.from.id)},
        });
        IpcResult r = vcCall(ctx, "queue", body);
        if (!r.ok) break;
        ++added;
    }
    if (added) {
        store_.del("cleared:" + std::to_string(vcTarget(ctx)));
        replyHtml(ctx, ui_texts::queueRestoredText(userLabel(ctx)));
    } else {
        reply(ctx, "Could not restore the queue (engine error).");
    }
}

void Dispatcher::onRemove(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        replyHtml(ctx, ui_texts::kRemoveUsage);
        return;
    }
    // Positions are the 1-based up-next numbers from /queue. The
    // backend queue is waiting-only and 0-based (§1.7, §10-D4), so
    // display position N is backend position N−1 — the current track
    // is never in the list and can never be removed this way.
    // Syntax (§5, NoxBeats usage line): a single position, a range
    // "1-5", or a list "1,3,5". Ranges/lists remove from the highest
    // position down, so earlier positions stay valid.
    std::vector<int64_t> positions;
    std::string spec = args[0];
    auto parse_num = [](const std::string& t, int64_t& out) {
        if (t.empty()) return false;
        for (char c : t)
            if (c < '0' || c > '9') return false;
        try {
            out = std::stoll(t);
        } catch (...) {
            return false;
        }
        return out >= 1;
    };
    if (spec.find('-') != std::string::npos) {
        size_t dash = spec.find('-');
        int64_t lo = 0, hi = 0;
        if (!parse_num(spec.substr(0, dash), lo) ||
            !parse_num(spec.substr(dash + 1), hi) || hi < lo) {
            replyHtml(ctx, ui_texts::kRemoveUsage);
            return;
        }
        for (int64_t p = hi; p >= lo; --p) positions.push_back(p);
    } else if (spec.find(',') != std::string::npos) {
        std::string cur;
        for (char c : spec + ",") {
            if (c == ',') {
                int64_t p = 0;
                if (!parse_num(cur, p)) {
                    replyHtml(ctx, ui_texts::kRemoveUsage);
                    return;
                }
                positions.push_back(p);
                cur.clear();
            } else {
                cur += c;
            }
        }
        std::sort(positions.begin(), positions.end(),
                  std::greater<int64_t>());
        positions.erase(std::unique(positions.begin(), positions.end()),
                        positions.end());
    } else {
        int64_t p = 0;
        if (!parse_num(spec, p)) {
            replyHtml(ctx, ui_texts::kRemoveUsage);
            return;
        }
        positions.push_back(p);
    }
    // The title for the single-remove reply comes from the queue list.
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(ctx, "queue", list_body);
    if (!q.ok) {
        replyError(ctx, q);
        return;
    }
    const Json* items = q.data.find("queue");
    if (!items) {
        replyHtml(ctx, ui_texts::kRemoveBadPos);
        return;
    }
    std::string first_title;
    for (int64_t p : positions) {
        if (p < 1 || p > static_cast<int64_t>(items->size())) {
            replyHtml(ctx, ui_texts::kRemoveBadPos);
            return;
        }
        if (positions.size() == 1) {
            const Json* t =
                items->asArray()[static_cast<size_t>(p) - 1].find("track");
            if (t && t->find("title"))
                first_title = t->find("title")->asString();
        }
    }
    int removed = 0;
    for (int64_t p : positions) {
        Json body = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("remove")},
            // Display N -> backend waiting position N−1 (§10-D4).
            {"position", Json(p - 1)},
        });
        IpcResult r = vcCall(ctx, "queue", body);
        if (!r.ok) {
            replyError(ctx, r);
            return;
        }
        ++removed;
    }
    if (removed == 1)
        replyHtml(ctx, ui_texts::queueRemovedText(
                           static_cast<int>(positions[0]),
                           ui_texts::htmlEscape(first_title), userLabel(ctx)));
    else
        replyHtml(ctx, ui_texts::queueRemovedManyText(removed,
                                                      userLabel(ctx)));
}

void Dispatcher::onMove(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    auto args = splitArgs(ctx.args);
    if (args.size() < 2 || !isDigits(args[0]) || !isDigits(args[1])) {
        replyHtml(ctx, ui_texts::kMoveUsage);
        return;
    }
    int64_t from = std::stoll(args[0]);
    int64_t to = std::stoll(args[1]);
    // The vc queue contract has no move: implement it as list -> remove
    // -> re-add at the target position (real IPC calls, same effect).
    // Positions are the 1-based up-next numbers from /queue; the
    // backend list is waiting-only and 0-based (§1.7, §10-D4), so
    // display N is waiting index N−1. Display 1 is the first waiting
    // track — the current track is never in the list and can never be
    // moved (the old "Cannot move the currently playing track" guard
    // for from==1 was the index-0 bug).
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(ctx, "queue", list_body);
    if (!q.ok) {
        replyError(ctx, q);
        return;
    }
    const Json* items = q.data.find("queue");
    if (!items || from < 1 || to < 1 ||
        from > static_cast<int64_t>(items->size()) ||
        to > static_cast<int64_t>(items->size())) {
        replyHtml(ctx, ui_texts::kRemoveBadPos);
        return;
    }
    std::vector<Json> tracks;
    for (const auto& item : items->asArray()) {
        const Json* t = item.find("track");
        if (t) tracks.push_back(*t);
    }
    if (from == to) {
        reply(ctx, "Already at position " + std::to_string(to) + ".");
        return;
    }
    Json moved = tracks[static_cast<size_t>(from) - 1];
    tracks.erase(tracks.begin() + (from - 1));
    tracks.insert(tracks.begin() + (to - 1), moved);
    // Rebuild the whole waiting queue: remove every waiting item
    // (0-based positions, from the end so positions stay valid),
    // then re-add in the new order.
    for (int64_t pos = static_cast<int64_t>(items->size()) - 1; pos >= 0;
         --pos) {
        Json rm = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("remove")},
            {"position", Json(pos)},
        });
        vcCall(ctx, "queue", rm);
    }
    for (const auto& t : tracks) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", t},
            {"requested_by", Json(ctx.message.from.id)},
        });
        vcCall(ctx, "queue", add);
    }
    replyHtml(ctx, ui_texts::queueMovedText(static_cast<int>(from),
                                              static_cast<int>(to),
                                              userLabel(ctx)));
}

void Dispatcher::onShuffle(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    if (!callActive(vcTarget(ctx))) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    Json list_body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"action", Json("list")},
    });
    IpcResult q = vcCall(ctx, "queue", list_body);
    if (!q.ok) {
        replyError(ctx, q);
        return;
    }
    const Json* items = q.data.find("queue");
    // Waiting-only list (§1.7): every item is a waiting track, so
    // two waiting tracks are enough to shuffle.
    if (!items || items->size() < 2) {
        replyHtml(ctx, ui_texts::kShuffleFew);
        return;
    }
    std::vector<Json> pending;
    for (size_t i = 0; i < items->size(); ++i) {
        const Json* t = items->asArray()[i].find("track");
        if (t) pending.push_back(*t);
    }
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(pending.begin(), pending.end(), rng);
    // Rebuild the waiting queue in shuffled order: remove every
    // waiting item (0-based positions, from the end so positions
    // stay valid), then re-add.
    for (int64_t pos = static_cast<int64_t>(pending.size()) - 1; pos >= 0;
         --pos) {
        Json rm = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("remove")},
            {"position", Json(pos)},
        });
        vcCall(ctx, "queue", rm);
    }
    for (const auto& t : pending) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", t},
            {"requested_by", Json(ctx.message.from.id)},
        });
        vcCall(ctx, "queue", add);
    }
    replyHtml(ctx, ui_texts::queueShuffledText(
                        static_cast<int>(pending.size()), userLabel(ctx)));
}

void Dispatcher::onPos(const CommandContext& ctx) {
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    int64_t dur =
        track.find("duration_sec") ? track.find("duration_sec")->asInt() : 0;
    std::string speed = chatRtField(vcTarget(ctx), "speed", "1.0");
    replyHtml(ctx, ui_texts::fmt(ui_texts::kPositionText,
                                 {{"title",
                                   ui_texts::htmlEscape(
                                       track.find("title")
                                           ? track.find("title")->asString()
                                           : "?")},
                                  {"position", fmtSeconds(pos)},
                                  {"duration", dur ? fmtSeconds(dur) : "∞"},
                                  {"speed", speed}}));
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
    IpcResult r = vcCall(ctx, "seek", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    std::string text = "Jumped to " + fmtSeconds(to) + ".";
    // The backend owns the call and the audio: its seek is the seek.
    reply(ctx, text);
}

void Dispatcher::onSpeed(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Current speed: " + chatRtField(chat, "speed", "1.0") +
                       "x. Usage: /speed <0.5-4.0|normal>");
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
    // atempo accepts 0.5–100.0 in a single filter (§9: 0.50x–4.00x),
    // so no chaining is needed for the widened range.
    if (s < 0.5 || s > 4.0) {
        reply(ctx, "Speed must be 0.5x to 4.0x.");
        return;
    }
    setChatRtField(chat, "speed", std::to_string(s));
    // The backend owns the audio pipeline: live speed filtering is a
    // backend DSP job (Phase 5). The value is stored (what /pos and
    // /speed report) and the stream is restarted at the current
    // position; the reply says plainly that the speed did not change.
    char b[16];
    std::snprintf(b, sizeof b, "%.2g", s);
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
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"position_sec", Json(std::max<int64_t>(1, pos))},
    });
    IpcResult r = vcCall(ctx, "seek", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, std::string("Speed ") + b +
                   "x saved, but the backend cannot apply audio filters "
                   "yet — the stream restarted at the current position "
                   "at the old speed. Live filtering lands with the "
                   "Phase 5 backend DSP.");
}

void Dispatcher::onBass(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        reply(ctx, "Current bass: " + std::to_string(chatRtIntField(chat, "bass", 0)) +
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
    setChatRtField(chat, "bass", std::to_string(lvl));
    // Same honest limit as /speed: live bass filtering is a backend
    // DSP job (Phase 5). The value is stored and the stream is
    // restarted at the current position; the reply says plainly that
    // the bass did not change.
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
    int64_t pos =
        np.find("position_sec") ? np.find("position_sec")->asInt() : 0;
    Json body = Json::objectOf({
        {"chat_id", Json(chat)},
        {"position_sec", Json(std::max<int64_t>(1, pos))},
    });
    IpcResult r = vcCall(ctx, "seek", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    reply(ctx, "Bass " + std::to_string(lvl) +
                   " saved, but the backend cannot apply audio filters "
                   "yet — the stream restarted at the current position "
                   "unchanged. Live filtering lands with the Phase 5 "
                   "backend DSP.");
}

void Dispatcher::onMute(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    bool mute = ctx.command == "mute";
    int64_t chat = vcTarget(ctx);
    int cur = chatRtIntField(chat, "muted", 0);
    if ((cur != 0) == mute) {
        replyHtml(ctx, mute ? ui_texts::kCtlAlreadyMuted
                            : ui_texts::kCtlNotMuted);
        return;
    }
    // The backend work lives in muteVoice (shared with POST /v1/mute).
    // Its "Nothing is playing." error is the reply verbatim, matching
    // the old wording; any other failure is "Could not mute: <reason>".
    std::string err;
    if (!muteVoice(chat, mute, err)) {
        if (err == "Nothing is playing.") {
            reply(ctx, err);
        } else {
            reply(ctx, std::string("Could not ") +
                           (mute ? "mute: " : "unmute: ") + err);
        }
        return;
    }
    replyHtml(ctx, mute ? ui_texts::kCtlMuted : ui_texts::kCtlUnmuted);
}

void Dispatcher::onJoin(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    if (callActive(chat)) {
        replyHtml(ctx, ui_texts::kCtlAlreadyJoined);
        return;
    }
    // The backend owns the call: vc.join asks it to join, and the
    // backend drives the duplex JoinRequest handshake (the core runs
    // TDLib joinVideoChat on the shared assistant session there).
    std::string err;
    if (!joinVoice(chat, err)) {
        reply(ctx, "Could not join the voice chat: " + err);
        return;
    }
    replyHtml(ctx, ui_texts::kCtlJoined);
}

void Dispatcher::onReplay(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"position_sec", Json(static_cast<int64_t>(1))},
    });
    IpcResult r = vcCall(ctx, "seek", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    std::string text = ui_texts::kToastReplayOk;
    // The backend owns the call and the audio: its seek is the seek.
    reply(ctx, text);
}

void Dispatcher::onPlayer(const CommandContext& ctx) {
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        replyHtml(ctx, ui_texts::kCtlNotStreaming);
        return;
    }
    // /player re-sends the current card and deletes the old one (§4).
    int64_t old_id = 0;
    bool was_photo = false;
    if (cardInfo(vcTarget(ctx), old_id, was_photo) && old_id != 0)
        tg_.deleteMessage(vcTarget(ctx), old_id);
    sendNowPlayingCard(vcTarget(ctx), np, "Now playing");
}

void Dispatcher::onLyrics(const CommandContext& ctx) {
    std::string query = trim(ctx.args);
    bool named = !query.empty();
    std::string card_title;
    if (!named) {
        // Current track (remix §2.6): no args -> the playing song.
        Json np, track;
        if (!nowPlaying(vcTarget(ctx), np, track)) {
            replyHtml(ctx, ui_texts::kLyricsNotStreaming);
            return;
        }
        query = track.find("title") ? track.find("title")->asString() : "";
        card_title = query;
        if (query.empty()) {
            replyHtml(ctx, ui_texts::kLyricsNotFound);
            return;
        }
    }
    // NetEase search -> lyrics (netease.lyrics needs a track id).
    Json sbody = Json::objectOf({
        {"query", Json(query)},
        {"limit", Json(static_cast<int64_t>(1))},
    });
    IpcResult s = ipc_.post("netease/search", sbody, 20);
    if (!s.ok) {
        replyError(ctx, s);
        return;
    }
    const Json* results = s.data.find("results");
    if (!results || results->size() == 0) {
        replyHtml(ctx, named ? ui_texts::kLyricsNoResults
                             : ui_texts::kLyricsNotFound);
        return;
    }
    const Json* first = &results->asArray()[0];
    std::string tid = first->find("id") ? first->find("id")->asString() : "";
    if (tid.empty()) {
        replyHtml(ctx, named ? ui_texts::kLyricsNoResults
                             : ui_texts::kLyricsNotFound);
        return;
    }
    Json lbody = Json::objectOf({{"track_id", Json(tid)}});
    IpcResult l = ipc_.post("netease/lyrics", lbody, 20);
    if (!l.ok) {
        replyHtml(ctx, ui_texts::kLyricsFail);
        return;
    }
    const Json* lyrics = l.data.find("lyrics");
    std::string text = lyrics ? lyrics->asString() : "";
    if (text.empty()) {
        replyHtml(ctx, named ? ui_texts::kLyricsNoResults
                             : ui_texts::kLyricsNotFound);
        return;
    }
    if (named) {
        // Named song (Music163bot §6.5): the .lrc as a document with
        // the lyric as an expandable quote caption (timestamps
        // stripped, truncated to 1024 characters + "…" when longer).
        std::string title =
            first->find("title") ? first->find("title")->asString() : query;
        std::string stripped;
        {
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                // Drop [mm:ss.xx] timestamp prefixes.
                while (line.size() > 1 && line[0] == '[') {
                    size_t close = line.find(']');
                    if (close == std::string::npos) break;
                    line = line.substr(close + 1);
                }
                if (!line.empty() && line[0] == ' ') line.erase(0, 1);
                stripped += line + "\n";
            }
        }
        std::string safe;
        for (char c : title) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ')
                safe += c;
        }
        if (safe.empty()) safe = "lyrics";
        // The service writes the raw .lrc itself and returns its path
        // (IPC-5 lrc_path, ruling 4): send that file when present — a
        // real .lrc keeps its timestamps. Otherwise fall back to
        // writing our own stripped copy under <data_dir>/lyrics. The
        // caption is the stripped lyric either way (§6: timestamps
        // stripped in the caption, not the file).
        std::string path;
        const Json* lrc = l.data.find("lrc_path");
        if (lrc && !lrc->asString().empty()) {
            path = lrc->asString();
        } else {
            std::string dir = cfg_.data_dir + "/lyrics";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            path = dir + "/" + safe + ".lrc";
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                replyHtml(ctx, ui_texts::kLyricsFail);
                return;
            }
            out << stripped;
        }
        std::string cap_lyric =
            ui_texts::truncUtf8(ui_texts::htmlEscape(stripped), 1024);
        std::string caption = "<blockquote expandable>" + cap_lyric +
                              "</blockquote>";
        std::string err;
        if (tg_.sendFile(ctx.message.chat.id, path, caption, &err) < 0)
            replyError(ctx, IpcResult{false, Json(), "send_failed", err});
        return;
    }
    // Current track (remix): the card, chunked at 4096 characters.
    std::string header = ui_texts::fmt(
        ui_texts::kLyricsCard,
        {{"title", ui_texts::htmlEscape(card_title)}, {"lyrics", ""}});
    size_t first_room = 4096 > header.size() ? 4096 - header.size() : 0;
    std::string first_chunk = ui_texts::truncUtf8(
        ui_texts::htmlEscape(text), static_cast<int>(first_room));
    replyHtml(ctx, ui_texts::fmt(ui_texts::kLyricsCard,
                                 {{"title", ui_texts::htmlEscape(card_title)},
                                  {"lyrics", first_chunk}}));
    // Remaining chunks (plain text, 4096 chars each). The first chunk
    // is a prefix of the escaped text, so its byte size is the exact
    // offset to resume from.
    std::string escaped = ui_texts::htmlEscape(text);
    size_t done = first_chunk.size();
    while (done < escaped.size()) {
        std::string chunk = ui_texts::truncUtf8(
            escaped.substr(done), 4096);
        if (chunk.empty()) break;
        replyHtml(ctx, chunk);
        done += chunk.size();
    }
}

// --- handlers: search / download / radio / share / engine -------------------
// A download failure means "the track is not available yet" when the
// backend says so in these words (lemonic TRACK_NOT_AVAILABLE):
// the card then offers a Check Again retry instead of a dead end.
static bool isNotAvailableYet(const std::string& error) {
    std::string low;
    low.reserve(error.size());
    for (char c : error)
        low += static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
    return low.find("not available") != std::string::npos ||
           low.find("not yet available") != std::string::npos ||
           low.find("still being processed") != std::string::npos;
}
std::string Dispatcher::searchResultsText(const std::string& query,
                                          const Json& results, int page) {
    std::string text = ui_texts::searchHeaderText(query);
    size_t start = static_cast<size_t>((page - 1) * 10);
    size_t end = std::min(results.size(), start + 10);
    for (size_t i = start; i < end; ++i) {
        const Json& t = results.asArray()[i];
        auto str_field = [&t](const char* key) {
            const Json* f = t.find(key);
            return f ? f->asString() : std::string();
        };
        std::string dur = str_field("duration_str");
        if (dur.empty()) {
            const Json* ds = t.find("duration_sec");
            if (ds && ds->asInt() > 0) dur = ui_texts::mmss(ds->asInt());
        }
        std::string views;
        if (const Json* v = t.find("views"))
            views = std::to_string(v->asInt());
        bool is_live = false;
        if (const Json* l = t.find("is_live")) is_live = l->asBool();
        std::string artists = str_field("artist");
        if (artists.empty()) artists = str_field("uploader");
        text += "\n" + ui_texts::searchResultLine(
                          static_cast<int>(i + 1),
                          ui_texts::htmlEscape(str_field("title")),
                          str_field("url"),
                          ui_texts::htmlEscape(artists), dur, views,
                          str_field("upload_time"),
                          ui_texts::htmlEscape(str_field("uploader")),
                          is_live);
    }
    text += "\n";
    text += ui_texts::kSearchFooter;
    int total_pages = static_cast<int>((results.size() + 9) / 10);
    std::string footer = ui_texts::searchPageFooterText(page, total_pages);
    if (!footer.empty()) text += "\n" + footer;
    return text;
}

InlineKeyboard Dispatcher::searchResultsKeyboard(int64_t user_id,
                                                 const std::string& query,
                                                 const Json& results,
                                                 int page) {
    size_t start = static_cast<size_t>((page - 1) * 10);
    size_t end = std::min(results.size(), start + 10);
    std::vector<std::pair<int, std::string>> picks;
    for (size_t i = start; i < end; ++i) {
        const Json& t = results.asArray()[i];
        std::string id = t.find("id") ? t.find("id")->asString() : "";
        picks.emplace_back(static_cast<int>(i + 1),
                           ui_texts::cbSearchPick(static_cast<int>(i + 1),
                                                  user_id,
                                                  ui_texts::trackToken(id)));
    }
    InlineKeyboard kb = ui_texts::searchKeyboard(picks);
    int total_pages = static_cast<int>((results.size() + 9) / 10);
    if (total_pages > 1) {
        InlineKeyboard nav = ui_texts::searchPageKeyboard(
            user_id, page, ui_texts::queryHash(query), page > 1,
            end < results.size(), total_pages);
        for (auto& row : nav) kb.push_back(std::move(row));
    }
    return kb;
}

void Dispatcher::onSearch(const CommandContext& ctx) {
    std::string query = trim(ctx.args);
    if (query.empty()) {
        replyHtml(ctx, ui_texts::kSearchNoQuery);
        return;
    }
    // Query cap (§7): 50 chars for the command form.
    if (query.size() > 50) query = query.substr(0, 50);
    // Per-user cooldown (lemonic "Too frequent"): 5 s between searches.
    auto cool = store_.get("searchcool:" + std::to_string(ctx.message.from.id));
    long long now_s =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (cool) {
        try {
            long long last = std::stoll(*cool);
            if (now_s - last < 5) {
                replyHtml(ctx, ui_texts::searchTooFrequentText(
                                   static_cast<int>(5 - (now_s - last))));
                return;
            }
        } catch (...) {
        }
    }
    store_.set("searchcool:" + std::to_string(ctx.message.from.id),
               std::to_string(now_s));
    // Optional source prefix: ne: -> NetEase, else vc-ts (all sources).
    bool netease = query.rfind("ne:", 0) == 0 ||
                   query.rfind("netease:", 0) == 0;
    std::string q = netease ? trim(query.substr(query.find(':') + 1))
                            : query;
    if (q.empty()) {
        replyHtml(ctx, ui_texts::kSearchNoQuery);
        return;
    }
    Json body;
    IpcResult r;
    if (netease) {
        body = Json::objectOf({
            {"query", Json(q)},
            {"limit", Json(static_cast<int64_t>(10))},
        });
        r = ipc_.post("netease/search", body, 20);
    } else {
        body = Json::objectOf({
            {"query", Json(q)},
            {"source", Json("all")},
            {"limit", Json(static_cast<int64_t>(10))},
        });
        r = ipc_.post("search", body, 20);
    }
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        replyHtml(ctx, ui_texts::kSearchNoResults);
        return;
    }
    int64_t msg = replyKb(ctx, searchResultsText(q, *results, 1),
                          searchResultsKeyboard(ctx.message.from.id, q,
                                                *results, 1));
    if (msg > 0)
        storeSearchSession(ctx.message.from.id, q, *results, msg);
}

void Dispatcher::onDownload(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    std::string query = trim(ctx.args);
    if (query.empty()) {
        replyHtml(ctx, ui_texts::kDlUsage);
        return;
    }
    // Route by link type (§3.1 row 7, §10-D14 — mirrors the backend's
    // ClassifyQuery: source prefixes plus the providers' URL shapes).
    // Spotify links go to download.spotify (playlist links to the
    // playlist action); Deezer / TikTok / Navidrome / Soulseek links
    // go to their own download.* actions; everything else is a
    // YouTube search/download.
    std::string low = lowerStr(query);
    bool is_spotify = low.find("spotify.com") != std::string::npos ||
                      low.rfind("spotify:", 0) == 0;
    // Deezer: deezer.com/(track|album|playlist|artist)/<id> (the old
    // providers.py _DEEZER_RE) or the deezer: prefix.
    bool is_deezer = low.find("deezer.com") != std::string::npos ||
                     low.rfind("deezer:", 0) == 0;
    // TikTok: tiktok.com/@…/video/, tiktok.com/v/, vm./vt.tiktok.com/
    // (the old providers.py _TIKTOK_RE) or the tiktok: prefix.
    bool is_tiktok = low.find("tiktok.com") != std::string::npos ||
                     low.rfind("tiktok:", 0) == 0;
    // Navidrome: a self-hosted server URL has no fixed shape, so the
    // routable form is the navidrome: prefix (the backend's
    // ClassifyQuery routes navidrome:<id> the same way).
    bool is_navidrome = low.rfind("navidrome:", 0) == 0;
    // Soulseek: no URLs exist in Soulseek; the routable form is the
    // soulseek: prefix, and the text after it is the search query.
    bool is_soulseek = low.rfind("soulseek:", 0) == 0;
    bool playlist = is_spotify &&
                    (low.find("/playlist/") != std::string::npos ||
                     low.find(":playlist:") != std::string::npos ||
                     low.find("/album/") != std::string::npos ||
                     low.find(":album:") != std::string::npos);
    if (playlist) {
        // Spotify playlist/album: the user picks the delivery mode
        // first (Spotify-Loader §5.3) — the choice runs in cbDl.
        std::string run_id = mintRunId();
        store_.set("dlpick:" + run_id,
                   query + "\n" + std::to_string(ctx.message.chat.id));
        replyKb(ctx, ui_texts::kDlSpotifyOptions,
                ui_texts::dlSpotifyOptionsKeyboard(run_id));
        return;
    }
    std::string action = "download/youtube";
    Json body;
    if (is_spotify) {
        action = "download/spotify";
        body = Json::objectOf({
            {"url", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    } else if (is_deezer) {
        action = "download/deezer";
        body = Json::objectOf({
            {"query", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    } else if (is_tiktok) {
        action = "download/tiktok";
        body = Json::objectOf({
            {"url", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    } else if (is_navidrome) {
        action = "download/navidrome";
        body = Json::objectOf({
            {"url", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    } else if (is_soulseek) {
        // The "soulseek:" prefix is a routing marker, not search
        // text: the old provider splits the query on " - " into
        // artist/title, so the prefix must not ride along.
        action = "download/soulseek";
        body = Json::objectOf({
            {"query", Json(query.substr(std::string("soulseek:").size()))},
            {"requested_by", Json(ctx.message.from.id)},
        });
    } else {
        body = Json::objectOf({
            {"query", Json(query)},
            {"requested_by", Json(ctx.message.from.id)},
        });
    }
    // Progress message, edited in place (§8): the download POST is
    // synchronous, so the core cannot poll live percentages — the
    // downloaders only reveal their run_id in the POST response, by
    // design (server.py). The message is edited to the outcome, and
    // the final IPC-4 snapshot is fetched for the album checklist
    // and the lossy prompt.
    std::string run_id = mintRunId();
    store_.set("dlrun:" + run_id,
               "pending\n" + std::to_string(ctx.message.chat.id));
    InlineKeyboard cancel_kb = {{InlineButton::callback(
        ui_texts::kDlCancelBtn, ui_texts::cbDlCancel(run_id))}};
    int64_t progress_msg = replyKb(ctx, ui_texts::kDlFetching, cancel_kb);
    auto edit_progress = [&](const std::string& text,
                             const InlineKeyboard& kb) {
        if (progress_msg <= 0) return;
        std::string err;
        tg_.editMessageText(ctx.message.chat.id, progress_msg, text,
                            kb.empty() ? InlineKeyboard{}
                                       : kb,
                            &err);
    };
    IpcResult r = ipc_.post(action, body, 120);
    if (!r.ok) {
        if (isNotAvailableYet(r.error_message)) {
            // Not available yet: keep the request so a Check Again
            // tap can re-run exactly this download (lemonic
            // replyWithSongNotAvailable).
            store_.set("dlretry:" + run_id,
                       action + "\n" + query + "\n" +
                           std::to_string(ctx.message.chat.id) + "\n" +
                           std::to_string(ctx.message.from.id));
            edit_progress(ui_texts::kDlNotAvailable,
                          ui_texts::dlRetryKeyboard(run_id));
            return;
        }
        edit_progress(ui_texts::dlFailedText(r.error_message),
                      InlineKeyboard{});
        return;
    }
    const Json* oversize = r.data.find("oversize");
    if (oversize && oversize->asBool()) {
        edit_progress("That file is over the 50 MB send cap — not "
                      "sent. There is no large-file send yet.",
                      InlineKeyboard{});
        return;
    }
    const Json* track = r.data.find("track");
    const Json* path = r.data.find("file_path");
    std::string file_path = path ? path->asString() : "";
    std::string artist =
        track && track->find("artist") ? track->find("artist")->asString()
                                       : "";
    std::string title =
        track && track->find("title") ? track->find("title")->asString()
                                      : query;
    // The server run id (for the IPC-4 final snapshot).
    std::string server_run;
    if (const Json* rid = r.data.find("run_id"))
        server_run = rid->asString();
    store_.set("dlrun:" + run_id,
               file_path + "\n" + std::to_string(ctx.message.chat.id) +
                   "\n" + artist + "\n" + title);
    if (!server_run.empty())
        store_.set("dlsrv:" + run_id, server_run);
    // Final IPC-4 snapshot: album checklist + lossy prompt trigger.
    bool lossy = false;
    std::string lossy_summary;
    if (!server_run.empty()) {
        IpcResult snap = ipc_.get("/v1/download/progress?run_id=" +
                                      server_run,
                                  10);
        if (snap.ok) {
            if (const Json* la = snap.data.find("lossy_available"))
                lossy = la->asBool();
            if (const Json* ls = snap.data.find("lossy_summary"))
                lossy_summary = ls->asString();
        }
    }
    if (lossy) {
        edit_progress(ui_texts::fmt(ui_texts::kLossyPrompt,
                                    {{"artist", ui_texts::htmlEscape(artist)},
                                     {"title", ui_texts::htmlEscape(title)},
                                     {"summary", ui_texts::htmlEscape(
                                                    lossy_summary)}}),
                      ui_texts::lossyKeyboard(run_id, lossy_summary));
        return;
    }
    if (file_path.empty() || !std::filesystem::exists(file_path)) {
        // The file is not on a filesystem the core can read (the
        // shared /data volume is a compose property): report the
        // path honestly instead of claiming a delivery.
        std::string text = ui_texts::kDlSuccess;
        if (track) text += " " + fmtTrack(*track);
        if (!file_path.empty()) text += "\nFile: " + file_path;
        edit_progress(text, InlineKeyboard{});
        return;
    }
    // Deliver the file with the Music163bot caption (§8), then offer
    // the FX picker on its own message.
    std::string caption =
        audioCaption(track, file_path, title, artist, bot_username_);
    std::string err;
    if (tg_.sendAudioFile(ctx.message.chat.id, file_path, caption, &err) <
        0) {
        edit_progress(ui_texts::dlFailedText(err), InlineKeyboard{});
        return;
    }
    edit_progress(ui_texts::kDlSuccess, InlineKeyboard{});
    replyKb(ctx,
            ui_texts::fmt(ui_texts::kDlFxPrompt,
                          {{"title", ui_texts::htmlEscape(title)}}),
            ui_texts::dlFxKeyboard(run_id));
}

// The Music163bot audio caption (§8), shared by /download and the
// /start spotify---- deep link: name/artists/album/ext/size/kbps/bot.
// sizeMB/kbps are pre-formatted here (ui_texts::fmt does plain
// substitution, no float formatting).
std::string Dispatcher::audioCaption(const Json* track,
                                     const std::string& file_path,
                                     const std::string& title,
                                     const std::string& artist,
                                     const std::string& bot_username) {
    return ui_texts::fmt(
        ui_texts::kAudioCaption,
        {{"name", ui_texts::htmlEscape(title)},
         {"artists", ui_texts::htmlEscape(artist)},
         {"album",
          ui_texts::htmlEscape(track && track->find("album")
                                   ? track->find("album")->asString()
                                   : "")},
         {"ext", [&] {
              size_t dot = file_path.find_last_of('.');
              return dot == std::string::npos
                         ? std::string("mp3")
                         : file_path.substr(dot + 1);
          }()},
         {"sizeMB", [&] {
              std::error_code ec;
              auto bytes = std::filesystem::file_size(file_path, ec);
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%.2f",
                            ec ? 0.0 : bytes / 1000000.0);
              return std::string(buf);
          }()},
         {"kbps", [&] {
              std::error_code ec;
              auto bytes = std::filesystem::file_size(file_path, ec);
              int64_t dur = track && track->find("duration_sec")
                                ? track->find("duration_sec")->asInt()
                                : 0;
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%.2f",
                            dur > 0 && !ec ? bytes * 8.0 / dur / 1000.0
                                           : 0.0);
              return std::string(buf);
          }()},
         {"bot", bot_username}});
}

// /start spotify----{id} (§13 lemonic): the [Open in Parlay Bot]
// button on an inline search result. Resolves the track to a URL —
// the stored inline results first (they carry url + source), else
// the id prefix — then downloads it like /download and sends the
// audio file in this chat. One-shot: no lossy prompt and no FX
// picker (those belong to the /download flow).
void Dispatcher::deepLinkDownload(const CommandContext& ctx,
                                  const std::string& track_id) {
    Json track;
    std::string url;
    if (loadInlineTrack(ctx.message.from.id, track_id, track)) {
        url = track.find("url") ? track.find("url")->asString() : "";
        if (url.empty() && track.find("stream_url"))
            url = track.find("stream_url")->asString();
    }
    if (url.empty()) {
        // Prefix fallback (the store may have been cleared since the
        // inline query). lemonic: ids cannot be resolved without the
        // stored result — they carry no URL of their own.
        if (track_id.rfind("spotify:", 0) == 0)
            url = "https://open.spotify.com/track/" + track_id.substr(8);
        else if (track_id.rfind("yt:", 0) == 0)
            url = "https://www.youtube.com/watch?v=" + track_id.substr(3);
    }
    if (url.empty()) {
        replyHtml(ctx, ui_texts::kDlDeepLinkExpired);
        return;
    }
    // Route by link type (same rule as /download): Spotify links to
    // download.spotify, everything else to download.youtube.
    std::string low = lowerStr(url);
    bool is_spotify = low.find("spotify.com") != std::string::npos ||
                      low.rfind("spotify:", 0) == 0;
    std::string action =
        is_spotify ? "download/spotify" : "download/youtube";
    Json body = is_spotify
                    ? Json::objectOf({
                          {"url", Json(url)},
                          {"requested_by", Json(ctx.message.from.id)},
                      })
                    : Json::objectOf({
                          {"query", Json(url)},
                          {"requested_by", Json(ctx.message.from.id)},
                      });
    int64_t progress_msg =
        replyKb(ctx, ui_texts::kDlFetching, InlineKeyboard{});
    auto edit_progress = [&](const std::string& text) {
        if (progress_msg <= 0) return;
        std::string err;
        tg_.editMessageText(ctx.message.chat.id, progress_msg, text,
                            InlineKeyboard{}, &err);
    };
    IpcResult r = ipc_.post(action, body, 120);
    if (!r.ok) {
        edit_progress(ui_texts::dlFailedText(r.error_message));
        return;
    }
    const Json* oversize = r.data.find("oversize");
    if (oversize && oversize->asBool()) {
        edit_progress("That file is over the 50 MB send cap — not "
                      "sent. There is no large-file send yet.");
        return;
    }
    const Json* resp_track = r.data.find("track");
    const Json* path = r.data.find("file_path");
    std::string file_path = path ? path->asString() : "";
    std::string title =
        resp_track && resp_track->find("title")
            ? resp_track->find("title")->asString()
            : (track.find("title") ? track.find("title")->asString() : "");
    std::string artist =
        resp_track && resp_track->find("artist")
            ? resp_track->find("artist")->asString()
            : (track.find("artist") ? track.find("artist")->asString()
                                    : "");
    if (file_path.empty() || !std::filesystem::exists(file_path)) {
        std::string text = ui_texts::kDlSuccess;
        if (resp_track) text += " " + fmtTrack(*resp_track);
        if (!file_path.empty()) text += "\nFile: " + file_path;
        edit_progress(text);
        return;
    }
    std::string err;
    if (tg_.sendAudioFile(ctx.message.chat.id, file_path,
                          audioCaption(resp_track, file_path, title, artist,
                                       bot_username_),
                          &err) < 0) {
        edit_progress(ui_texts::dlFailedText(err));
        return;
    }
    edit_progress(ui_texts::kDlSuccess);
}

// Deliver one Spotify playlist/album pick (cbDl zip / 1by1, §8).
void Dispatcher::deliverSpotifyCollection(const TgCallbackQuery& cq,
                                          const std::string& url,
                                          const std::string& mode) {
    Json body = Json::objectOf({
        {"url", Json(url)},
        {"mode", Json(mode)},
        {"requested_by", Json(cq.from.id)},
    });
    IpcResult r = ipc_.post("download/spotify-playlist", body,
                            300);
    if (!r.ok) {
        answerCb(cq, r.error_message, true);
        return;
    }
    const Json* oversize = r.data.find("oversize");
    if (oversize && oversize->asBool()) {
        answerCb(cq,
                 "That collection is over the 50 MB send cap — not "
                 "sent. There is no large-file send yet.",
                 true);
        return;
    }
    std::string kind = url.find("/album/") != std::string::npos ||
                               url.find(":album:") != std::string::npos
                           ? "album"
                           : "playlist";
    const Json* count = r.data.find("count");
    int total = count ? static_cast<int>(count->asInt()) : 0;
    if (mode == "zip") {
        const Json* path = r.data.find("file_path");
        std::string zip = path ? path->asString() : "";
        if (zip.empty() || !std::filesystem::exists(zip)) {
            answerCb(cq, "The ZIP file is not available.", true);
            return;
        }
        std::string err;
        if (tg_.sendFile(cq.chat_id, zip, "", &err) < 0) {
            answerCb(cq, err, true);
            return;
        }
        answerCb(cq, ui_texts::dlSpotifySuccessText(total, kind));
        return;
    }
    // One by one: every file in file_paths, in order.
    const Json* files = r.data.find("file_paths");
    int sent = 0;
    int skipped = 0;
    if (files) {
        for (const auto& f : files->asArray()) {
            std::string fp = f.asString();
            if (fp.empty() || !std::filesystem::exists(fp)) {
                ++skipped;
                continue;
            }
            std::error_code ec;
            if (std::filesystem::file_size(fp, ec) > 50 * 1024 * 1024) {
                ++skipped;
                continue;
            }
            std::string err;
            if (tg_.sendAudioFile(cq.chat_id, fp, "", &err) < 0)
                ++skipped;
            else
                ++sent;
        }
    }
    std::string text =
        ui_texts::dlSpotifySuccessText(sent, kind);
    if (skipped > 0)
        text += " (" + std::to_string(skipped) + " skipped — over the "
                "50 MB cap or unavailable)";
    answerCb(cq, text);
}

void Dispatcher::onRadio(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    std::string station = trim(ctx.args);
    if (station.empty()) {
        // List stations (radio.list). The service returns objects
        // (id/name/genre/stream_url), not bare strings.
        IpcResult r = ipc_.get("/v1/radio/stations", 10);
        if (!r.ok) {
            replyError(ctx, r);
            return;
        }
        std::string out = ui_texts::kRadioStationsHeader;
        const Json* stations = r.data.find("stations");
        if (stations) {
            for (const auto& s : stations->asArray()) {
                std::string name =
                    s.find("name") ? s.find("name")->asString() : "";
                std::string genre =
                    s.find("genre") ? s.find("genre")->asString() : "";
                std::string id =
                    s.find("id") ? s.find("id")->asString() : "";
                out += "\n" + ui_texts::radioStationLine(name, genre, id);
            }
        }
        out += "\n\n" + std::string(ui_texts::kRadioStationsFooter);
        replyHtml(ctx, out);
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"station", Json(station)},
    });
    IpcResult r = ipc_.post("radio/start", body, 15);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* url = r.data.find("stream_url");
    if (!url || url->asString().empty()) {
        reply(ctx, "Radio service returned no stream URL.");
        return;
    }
    Json play = vcPlayBody(vcTarget(ctx), url->asString(),
                           ctx.message.from.id);
    IpcResult pr = vcCall(ctx, "play", play);
    if (!pr.ok) {
        replyError(ctx, pr);
        return;
    }
    // The backend's play is the play: it owns the call and the audio,
    // so a successful vc.play means the radio is really streaming.
    setCallActive(vcTarget(ctx), true);
    std::string name =
        r.data.find("station_name") ? r.data.find("station_name")->asString()
                                    : station;
    replyHtml(ctx,
              ui_texts::radioNowPlayingText(name, url->asString()));
}

void Dispatcher::onNewRadio(const CommandContext& ctx) {
    // Group only (§21): a radio belongs to a group voice chat.
    if (ctx.message.chat.id >= 0) {
        replyHtml(ctx, ui_texts::kRadioGroupOnly);
        return;
    }
    if (!canManageVc(ctx)) return;
    std::string station = trim(ctx.args);
    if (station.empty()) {
        replyHtml(ctx, ui_texts::kRadioNeedLink);
        return;
    }
    // A radio already exists for this chat -> point at /manage.
    IpcResult st = ipc_.get("/v1/radio/status?chat_id=" +
                                std::to_string(vcTarget(ctx)),
                            10);
    if (st.ok) {
        replyHtml(ctx, ui_texts::kRadioExists);
        return;
    }
    Json body = Json::objectOf({
        {"chat_id", Json(vcTarget(ctx))},
        {"station", Json(station)},
    });
    IpcResult r = ipc_.post("radio/start", body, 15);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    std::string name =
        r.data.find("station_name") ? r.data.find("station_name")->asString()
                                    : station;
    std::string stream =
        r.data.find("stream_url") ? r.data.find("stream_url")->asString()
                                  : "";
    std::string out = ui_texts::kRadioCreated;
    if (!stream.empty())
        out += "\n" + ui_texts::radioNowPlayingText(name, stream);
    replyHtml(ctx, out);
}

void Dispatcher::onDestroyRadio(const CommandContext& ctx) {
    if (ctx.message.chat.id >= 0) {
        replyHtml(ctx, ui_texts::kRadioGroupOnly);
        return;
    }
    if (!canManageVc(ctx)) return;
    Json body = Json::objectOf({{"chat_id", Json(vcTarget(ctx))}});
    IpcResult r = ipc_.post("radio/stop", body, 15);
    if (!r.ok) {
        if (r.error_code == "not_found") {
            replyHtml(ctx, ui_texts::kRadioNoRadio);
            return;
        }
        replyError(ctx, r);
        return;
    }
    replyHtml(ctx, ui_texts::kRadioDestroyed);
}

void Dispatcher::onManageRadio(const CommandContext& ctx) {
    if (ctx.message.chat.id >= 0) {
        replyHtml(ctx, ui_texts::kRadioGroupOnly);
        return;
    }
    if (!canManageVc(ctx)) return;
    IpcResult r = ipc_.get("/v1/radio/status?chat_id=" +
                               std::to_string(vcTarget(ctx)),
                           10);
    if (!r.ok) {
        if (r.error_code == "not_found") {
            replyHtml(ctx, ui_texts::kRadioNoRadio);
            return;
        }
        replyError(ctx, r);
        return;
    }
    std::string now_playing = "—";
    if (const Json* np = r.data.find("now_playing")) {
        if (np->isObject() && np->find("title"))
            now_playing = np->find("title")->asString();
        else if (np->isString())
            now_playing = np->asString();
    }
    if (now_playing == "—") {
        if (const Json* s = r.data.find("station"))
            now_playing = s->asString();
    }
    // "Live on" is the station itself: there is no per-chat live
    // page in the locked topology (no radio web UI), so the old
    // radio-js host URL rendered a dead link. The backend's status
    // response carries the station name.
    std::string station_name = "—";
    if (const Json* sn = r.data.find("station_name"))
        station_name = sn->asString();
    if (station_name == "—" || station_name.empty()) {
        if (const Json* stn = r.data.find("station"))
            station_name = stn->asString();
    }
    if (station_name.empty()) station_name = "—";
    replyHtml(ctx, ui_texts::fmt(ui_texts::kRadioManageCard,
                                 {{"loopType", "—"},
                                  {"created", "—"},
                                  {"nowPlaying",
                                   ui_texts::htmlEscape(now_playing)},
                                  {"autoplay", "—"},
                                  {"queueLen", "—"},
                                  {"host",
                                   ui_texts::htmlEscape(station_name)}}));
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
    IpcResult r = ipc_.post("share", body, 15);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* link = r.data.find("link");
    const Json* token = r.data.find("token");
    std::string text = "Anonymous share link created:\n";
    if (link) text += link->asString() + "\n";
    if (token)
        text += "Token: <code>" + token->asString() + "</code>\n";
    text += "(No identity is attached to the link.)";
    // The share service is anonymous by design (token -> track only),
    // so the core keeps the creator mapping itself: /start share_<token>
    // and pl:share:mylink both need it (§22).
    if (token)
        store_.set("sharetok:" + token->asString(),
                   std::to_string(ctx.message.from.id));
    if (link)
        store_.set("sharelink:" + std::to_string(ctx.message.from.id),
                   link->asString());
    replyKb(ctx, text, ui_texts::shareHelpKeyboard());
}

void Dispatcher::onEngine(const CommandContext& ctx) {
    if (!isPrivate(ctx) &&
        !isChatAdmin(ctx.message.chat.id, ctx.message.from.id)) {
        replyHtml(ctx, ui_texts::kEngineOnlyAdmin);
        return;
    }
    // Engine selection is retired (Phase 2): there is one unified
    // backend over the persistent UDS socket, so there is no engine
    // to switch to or report. Any argument is ignored.
    reply(ctx, "Engine selection is retired — Parlay now runs a single "
               "unified backend, so there is no engine to switch. "
               "Playback, queue and voice all go through that backend.");
}

// --- handlers: playlists -----------------------------------------------------
namespace {
constexpr size_t kPlaylistMax = 100;  // NoxBeats DB::kPlaylistMax
// Max authorized users per chat: ArcMusic / TinaMusic / YukkiMusic-Go
// all default MAX_AUTH_USERS to 25 (NoxBeats defines no such limit;
// the 20 the core used before was an unverified assumption).
constexpr size_t kAuthMaxUsers = 25;

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

// Named playlists (Parvati/TgMusicBot model, §17): one list per user,
// each entry {id, name, songs: [track, ...]}. The list is user-scoped,
// so every playlist in it belongs to the caller by construction.
constexpr size_t kNamedPlaylistMax = 10;

Json namedPlaylistsOf(Store& store, int64_t user) {
    auto v = store.get("npl:" + std::to_string(user));
    if (!v) return Json(Json::Array{});
    try {
        return Json::parse(*v);
    } catch (...) {
        return Json(Json::Array{});
    }
}
void saveNamedPlaylists(Store& store, int64_t user, const Json& list) {
    store.set("npl:" + std::to_string(user), list.dump());
}
// Find a named playlist by id or by name (case-insensitive); returns
// the index, or -1 when absent.
long namedPlaylistIndex(const Json& list, const std::string& ref) {
    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string want = lower(ref);
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& p = list.asArray()[i];
        const Json* id = p.find("id");
        const Json* name = p.find("name");
        if ((id && id->asString() == ref) ||
            (name && lower(name->asString()) == want))
            return static_cast<long>(i);
    }
    return -1;
}
}  // namespace

void Dispatcher::onAddPlaylist(const CommandContext& ctx) {
    int64_t user = ctx.message.from.id;
    std::string query = trim(ctx.args);
    Json list = playlistOf(store_, user);
    if (query.empty()) {
        // No argument: add the song playing now (group only).
        if (isPrivate(ctx)) {
            replyHtml(ctx, ui_texts::kAddPlaylistUsage);
            return;
        }
        Json np, track;
        if (!nowPlaying(vcTarget(ctx), np, track)) {
            replyHtml(ctx, ui_texts::kCtlNotStreaming);
            return;
        }
        std::string id = track.find("id") ? track.find("id")->asString() : "";
        if (id.empty()) {
            replyHtml(ctx, ui_texts::kPlaylistUnsupported);
            return;
        }
        for (const auto& item : list.asArray()) {
            const Json* iid = item.find("id");
            if (iid && iid->asString() == id) {
                replyHtml(ctx, ui_texts::kPlaylistDup);
                return;
            }
        }
        if (list.size() >= kPlaylistMax) {
            replyHtml(ctx,
                      ui_texts::playlistFullText(
                          static_cast<int>(kPlaylistMax)));
            return;
        }
        Json::Array arr = list.asArray();
        arr.push_back(track);
        savePlaylist(store_, user, Json(arr));
        replyHtml(ctx, ui_texts::playlistAddedText(
                           ui_texts::htmlEscape(fmtTrack(track)),
                           static_cast<int>(arr.size())));
        return;
    }
    // Resolve the query to a track via vc-ts search, then save it.
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"source", Json("all")},
        {"limit", Json(static_cast<int64_t>(1))},
    });
    IpcResult r = ipc_.post("search", body, 20);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        replyHtml(ctx, ui_texts::kSearchNoResults);
        return;
    }
    Json track = results->asArray()[0];
    std::string id = track.find("id") ? track.find("id")->asString() : "";
    for (const auto& item : list.asArray()) {
        const Json* iid = item.find("id");
        if (iid && !id.empty() && iid->asString() == id) {
            replyHtml(ctx, ui_texts::kPlaylistDup);
            return;
        }
    }
    if (list.size() >= kPlaylistMax) {
        replyHtml(ctx,
                  ui_texts::playlistFullText(static_cast<int>(kPlaylistMax)));
        return;
    }
    Json::Array arr = list.asArray();
    arr.push_back(track);
    savePlaylist(store_, user, Json(arr));
    replyHtml(ctx, ui_texts::playlistAddedText(
                       ui_texts::htmlEscape(fmtTrack(track)),
                       static_cast<int>(arr.size())));
}

void Dispatcher::onPlaylist(const CommandContext& ctx) {
    Json list = playlistOf(store_, ctx.message.from.id);
    if (list.size() == 0) {
        replyHtml(ctx, ui_texts::kPlaylistEmpty);
        return;
    }
    std::string text = ui_texts::playlistHeaderText(
        displayName(ctx.message.from),
        static_cast<int>(list.size()));
    size_t i = 0;
    for (const auto& item : list.asArray()) {
        std::string line = std::to_string(++i) + ". " +
                           ui_texts::htmlEscape(fmtTrack(item)) + "\n";
        if (text.size() + line.size() > 3800) {
            --i;
            break;
        }
        text += line;
    }
    if (i < list.size())
        text += ui_texts::playlistMoreText(
            static_cast<int>(list.size() - i));
    text += ui_texts::kPlaylistFooter;
    replyHtml(ctx, text);
}

void Dispatcher::onRmPlaylist(const CommandContext& ctx) {
    int64_t user = ctx.message.from.id;
    std::string arg = lowerStr(trim(ctx.args));
    Json list = playlistOf(store_, user);
    bool all = ctx.command == "clearplaylist" || arg == "all";
    if (all) {
        size_t n = list.size();
        savePlaylist(store_, user, Json(Json::Array{}));
        replyHtml(ctx, n ? ui_texts::playlistClearedText(
                               static_cast<int>(n))
                         : ui_texts::kPlaylistEmpty);
        return;
    }
    if (arg.empty()) {
        replyHtml(ctx, ui_texts::kRmPlaylistUsage);
        return;
    }
    if (list.size() == 0) {
        replyHtml(ctx, ui_texts::kPlaylistEmpty);
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
        replyHtml(ctx, ui_texts::kRmPlaylistNone);
        return;
    }
    Json::Array arr = list.asArray();
    std::string title = arr[pos - 1].find("title")
                            ? arr[pos - 1].find("title")->asString()
                            : "?";
    arr.erase(arr.begin() + (pos - 1));
    savePlaylist(store_, user, Json(arr));
    replyHtml(ctx, ui_texts::playlistRemovedText(
                       ui_texts::htmlEscape(title),
                       static_cast<int>(arr.size())));
}

void Dispatcher::onPlayPlaylist(const CommandContext& ctx) {
    if (!isGroup(ctx)) {
        reply(ctx, "Music only plays in groups (supergroups).");
        return;
    }
    if (!playAllowed(ctx, false)) return;
    Json list = playlistOf(store_, ctx.message.from.id);
    if (list.size() == 0) {
        replyHtml(ctx, ui_texts::kPlaylistEmpty);
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
    Json body = vcPlayBody(
        vcTarget(ctx),
        first.find("title") ? first.find("title")->asString() : "",
        ctx.message.from.id);
    IpcResult r = vcCall(ctx, "play", body);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    // The backend's play is the play (see onPlay): a successful
    // vc.play means the first track is really playing.
    setCallActive(vcTarget(ctx), true);
    int queued = 0;
    for (size_t i = 1; i < tracks.size(); ++i) {
        Json add = Json::objectOf({
            {"chat_id", Json(vcTarget(ctx))},
            {"action", Json("add")},
            {"track", tracks[i]},
            {"requested_by", Json(ctx.message.from.id)},
        });
        IpcResult ar = vcCall(ctx, "queue", add);
        if (!ar.ok) break;
        ++queued;
    }
    reply(ctx, "Playing your playlist: " + fmtTrack(first) +
                   (queued ? " (+" + std::to_string(queued) + " queued)" : ""));
}

// --- handlers: named playlists (Parvati/TgMusicBot model, §17) -------------
void Dispatcher::onCreatePlaylist(const CommandContext& ctx) {
    std::string name = trim(ctx.args);
    if (name.empty()) {
        replyHtml(ctx, ui_texts::kCreatePlaylistUsage);
        return;
    }
    int64_t user = ctx.message.from.id;
    Json list = namedPlaylistsOf(store_, user);
    if (list.size() >= kNamedPlaylistMax) {
        replyHtml(ctx, ui_texts::kNamedPlaylistLimit);
        return;
    }
    if (namedPlaylistIndex(list, name) >= 0) {
        replyHtml(ctx, ui_texts::namedPlaylistCreateFailText(
                           "a playlist with that name already exists"));
        return;
    }
    std::string id = mintRunId();
    Json::Array arr = list.asArray();
    arr.push_back(Json::objectOf({
        {"id", Json(id)},
        {"name", Json(name)},
        {"songs", Json(Json::Array{})},
    }));
    saveNamedPlaylists(store_, user, Json(arr));
    replyHtml(ctx, ui_texts::namedPlaylistCreatedText(
                       ui_texts::htmlEscape(name), id));
}

void Dispatcher::onDeletePlaylist(const CommandContext& ctx) {
    std::string ref = trim(ctx.args);
    if (ref.empty()) {
        replyHtml(ctx, "Usage: <code>/deleteplaylist &lt;name or ID&gt;</code>");
        return;
    }
    int64_t user = ctx.message.from.id;
    Json list = namedPlaylistsOf(store_, user);
    long idx = namedPlaylistIndex(list, ref);
    if (idx < 0) {
        replyHtml(ctx, ui_texts::kNamedPlaylistNotFound);
        return;
    }
    Json::Array arr = list.asArray();
    std::string name = arr[static_cast<size_t>(idx)].find("name")
                           ? arr[static_cast<size_t>(idx)]
                                 .find("name")
                                 ->asString()
                           : ref;
    arr.erase(arr.begin() + idx);
    saveNamedPlaylists(store_, user, Json(arr));
    replyHtml(ctx,
              ui_texts::namedPlaylistDeletedText(ui_texts::htmlEscape(name)));
}

void Dispatcher::onAddToPlaylist(const CommandContext& ctx) {
    // /addtoplaylist <playlist name or ID> <song name or link>
    std::string args = trim(ctx.args);
    size_t sp = args.find(' ');
    if (sp == std::string::npos) {
        replyHtml(ctx, "Usage: <code>/addtoplaylist &lt;playlist&gt; "
                       "&lt;song name or link&gt;</code>");
        return;
    }
    std::string ref = args.substr(0, sp);
    std::string query = trim(args.substr(sp + 1));
    int64_t user = ctx.message.from.id;
    Json list = namedPlaylistsOf(store_, user);
    long idx = namedPlaylistIndex(list, ref);
    if (idx < 0) {
        replyHtml(ctx, ui_texts::kNamedPlaylistNotFound);
        return;
    }
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"source", Json("all")},
        {"limit", Json(static_cast<int64_t>(1))},
    });
    IpcResult r = ipc_.post("search", body, 20);
    if (!r.ok) {
        replyError(ctx, r);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        replyHtml(ctx, ui_texts::kSearchNoResults);
        return;
    }
    Json track = results->asArray()[0];
    Json::Array arr = list.asArray();
    Json& pl = arr[static_cast<size_t>(idx)];
    Json songs = pl.find("songs") ? *pl.find("songs") : Json(Json::Array{});
    std::string id = track.find("id") ? track.find("id")->asString() : "";
    for (const auto& item : songs.asArray()) {
        const Json* iid = item.find("id");
        if (iid && !id.empty() && iid->asString() == id) {
            replyHtml(ctx, ui_texts::kPlaylistDup);
            return;
        }
    }
    if (songs.size() >= kPlaylistMax) {
        replyHtml(ctx,
                  ui_texts::playlistFullText(static_cast<int>(kPlaylistMax)));
        return;
    }
    Json::Array sarr = songs.asArray();
    sarr.push_back(track);
    pl.set("songs", Json(sarr));
    saveNamedPlaylists(store_, user, Json(arr));
    std::string pname = pl.find("name") ? pl.find("name")->asString() : ref;
    replyHtml(ctx, ui_texts::namedPlaylistAddedText(
                       ui_texts::htmlEscape(fmtTrack(track)),
                       ui_texts::htmlEscape(pname)));
}

void Dispatcher::onRemoveFromPlaylist(const CommandContext& ctx) {
    // /removefromplaylist <playlist name or ID> <song number>
    auto words = splitArgs(ctx.args);
    if (words.size() < 2) {
        replyHtml(ctx, "Usage: <code>/removefromplaylist &lt;playlist&gt; "
                       "&lt;song number&gt;</code>");
        return;
    }
    int64_t user = ctx.message.from.id;
    Json list = namedPlaylistsOf(store_, user);
    long idx = namedPlaylistIndex(list, words[0]);
    if (idx < 0) {
        replyHtml(ctx, ui_texts::kNamedPlaylistNotFound);
        return;
    }
    if (!isDigits(words[1])) {
        replyHtml(ctx, ui_texts::kNamedPlaylistBadNumber);
        return;
    }
    size_t n = std::stoul(words[1]);
    Json::Array arr = list.asArray();
    Json& pl = arr[static_cast<size_t>(idx)];
    Json songs = pl.find("songs") ? *pl.find("songs") : Json(Json::Array{});
    if (n < 1 || n > songs.size()) {
        replyHtml(ctx, ui_texts::kNamedPlaylistSongNotFound);
        return;
    }
    Json::Array sarr = songs.asArray();
    sarr.erase(sarr.begin() + (n - 1));
    pl.set("songs", Json(sarr));
    saveNamedPlaylists(store_, user, Json(arr));
    std::string pname = pl.find("name") ? pl.find("name")->asString() : words[0];
    replyHtml(ctx,
              ui_texts::namedPlaylistRemovedText(ui_texts::htmlEscape(pname)));
}

void Dispatcher::onPlaylistInfo(const CommandContext& ctx) {
    std::string ref = trim(ctx.args);
    if (ref.empty()) {
        replyHtml(ctx, "Usage: <code>/playlistinfo &lt;name or ID&gt;</code>");
        return;
    }
    int64_t user = ctx.message.from.id;
    Json list = namedPlaylistsOf(store_, user);
    long idx = namedPlaylistIndex(list, ref);
    if (idx < 0) {
        replyHtml(ctx, ui_texts::kNamedPlaylistNotFound);
        return;
    }
    const Json& pl = list.asArray()[static_cast<size_t>(idx)];
    std::string name = pl.find("name") ? pl.find("name")->asString() : ref;
    Json songs = pl.find("songs") ? *pl.find("songs") : Json(Json::Array{});
    std::string songs_text;
    if (songs.size() == 0) {
        songs_text = ui_texts::kPlaylistInfoEmpty;
    } else {
        size_t i = 0;
        for (const auto& item : songs.asArray()) {
            std::string line = std::to_string(++i) + ". " +
                               ui_texts::htmlEscape(fmtTrack(item)) + "\n";
            if (songs_text.size() + line.size() > 3800) {
                --i;
                break;
            }
            songs_text += line;
        }
        if (i < songs.size())
            songs_text += ui_texts::playlistMoreText(
                static_cast<int>(songs.size() - i));
    }
    replyHtml(ctx, ui_texts::fmt(ui_texts::kPlaylistInfoCard,
                                 {{"name", ui_texts::htmlEscape(name)},
                                  {"owner", ui_texts::htmlEscape(
                                                displayName(ctx.message.from))},
                                  {"count", std::to_string(songs.size())},
                                  {"songs", songs_text}}));
}

void Dispatcher::onMyPlaylists(const CommandContext& ctx) {
    Json list = namedPlaylistsOf(store_, ctx.message.from.id);
    if (list.size() == 0) {
        replyHtml(ctx, ui_texts::kMyPlaylistsNone);
        return;
    }
    std::string text;
    for (size_t i = 0; i < list.asArray().size(); ++i) {
        const auto& p = list.asArray()[i];
        std::string name = p.find("name") ? p.find("name")->asString() : "?";
        std::string id = p.find("id") ? p.find("id")->asString() : "";
        if (i == 0) {
            text = ui_texts::fmt(ui_texts::kMyPlaylistsHeader,
                                 {{"name", ui_texts::htmlEscape(name)},
                                  {"id", id}});
        } else {
            text += "\n" + ui_texts::htmlEscape(name) + " (<code>" + id +
                    "</code>)";
        }
    }
    replyHtml(ctx, text);
}

// --- handlers: settings / chat settings / autoplay --------------------------
void Dispatcher::onPlaymode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    if (args.empty()) {
        reply(ctx, "Usage: /playmode admin|everyone (current: " +
                       s.play_mode + ")" + degraded);
        return;
    }
    std::string a = lowerStr(args[0]);
    bool admins = a == "admin" || a == "admins" || a == "adminonly";
    bool all = a == "everyone" || a == "all";
    if (!admins && !all) {
        reply(ctx, "Usage: /playmode admin|everyone (current: " +
                       s.play_mode + ")" + degraded);
        return;
    }
    if (!adminCheck(ctx)) return;
    if (!setSetting(chat, "play_mode",
                    Json(admins ? "admins" : "everyone"))) {
        reply(ctx, "Could not save play mode — the settings backend is "
                   "unreachable. Your change was not stored.");
        return;
    }
    replyHtml(ctx, admins ? ui_texts::kPlayModeAdmin
                          : ui_texts::kPlayModeEveryone);
}

void Dispatcher::onSettings(const CommandContext& ctx) {
    // Group only (§9): the panel edits this chat's settings.
    if (isPrivate(ctx)) {
        replyHtml(ctx, ui_texts::kSettingsGroupOnly);
        return;
    }
    if (!adminCheck(ctx)) return;
    int64_t chat = vcTarget(ctx);
    std::string title = ctx.message.chat.title;
    if (title.empty()) title = std::to_string(chat);
    store_.set("chat_title:" + std::to_string(chat), title);
    ui_texts::ChatSettings s = chatSettings(chat);
    std::string text = ui_texts::fmt(ui_texts::kSettingsText,
                                     {{"title", ui_texts::htmlEscape(title)},
                                      {"id", std::to_string(chat)}});
    if (!settingsLive(chat)) text += kSettingsDegradedSuffix;
    replyKb(ctx, text, ui_texts::settingsKeyboard(s));
}

void Dispatcher::onQuality(const CommandContext& ctx) {
    // Group only (routing row C102): the quality card describes this
    // chat's effective Opus config. Everyone may read it; changing a
    // value goes through the settings panel (admin-only callbacks).
    if (isPrivate(ctx)) {
        replyHtml(ctx, ui_texts::kSettingsGroupOnly);
        return;
    }
    int64_t chat = vcTarget(ctx);
    ui_texts::QualityInfo q;
    if (!fetchQuality(chat, q)) {
        replyHtml(ctx, ui_texts::kQualityLoadFailed);
        return;
    }
    replyKb(ctx, ui_texts::qualityCardText(q),
            ui_texts::settingsQualityKeyboard(q));
}

void Dispatcher::onLang(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    int64_t chat = vcTarget(ctx);
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    if (args.empty()) {
        reply(ctx, "Current language: " + s.language +
                       ". This build ships English only; /lang <code> "
                       "stores your choice for when more locales land." +
                       degraded);
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
    if (!setSetting(chat, "language", Json(code))) {
        reply(ctx, "Could not save the language — the settings backend "
                   "is unreachable. Your change was not stored.");
        return;
    }
    reply(ctx, "Language set to " + code + ".");
}

void Dispatcher::onAdminmode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    if (args.empty()) {
        int mode = s.admin_mode;
        reply(ctx, std::string("Admin mode: ") +
                       (mode == 1 ? "admins only"
                                  : mode == 2 ? "everyone"
                                              : "admins + authorized") +
                       ". Usage: /adminmode admin|adminauth|everyone" +
                       degraded);
        return;
    }
    if (!adminCheck(ctx)) return;
    std::string a = lowerStr(args[0]);
    int mode = -1;
    if (a == "admin" || a == "admins" || a == "adminonly") mode = 1;
    else if (a == "adminauth" || a == "auth" || a == "dj") mode = 0;
    else if (a == "everyone" || a == "all") mode = 2;
    if (mode < 0) {
        replyHtml(ctx, ui_texts::kAdminModeInvalid);
        return;
    }
    if (!setSetting(chat, "admin_mode", Json(static_cast<int64_t>(mode)))) {
        reply(ctx, "Could not save admin mode — the settings backend "
                   "is unreachable. Your change was not stored.");
        return;
    }
    replyHtml(ctx, ui_texts::adminModeUpdatedText(
                       mode == 1 ? "admins only"
                                 : mode == 2 ? "everyone"
                                             : "admins + authorized"));
}

void Dispatcher::onCmdDelete(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    auto args = splitArgs(ctx.args);
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    if (args.empty() || (!flagOn(args[0]) && !flagOff(args[0]))) {
        reply(ctx, "Cmd delete is " + std::string(s.cmd_delete ? "on" : "off") +
                       ". Usage: /cmddelete on|off" + degraded);
        return;
    }
    if (!adminCheck(ctx)) return;
    if (!setSetting(chat, "cmd_delete", Json(flagOn(args[0])))) {
        reply(ctx, "Could not save cmd delete — the settings backend "
                   "is unreachable. Your change was not stored.");
        return;
    }
    reply(ctx, std::string("Cmd delete: ") +
                   (flagOn(args[0]) ? "on" : "off") + ".");
}

void Dispatcher::onCleanmode(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    // clean_mode split (Q7 blocker (b)): the core keeps ONE number —
    // the delay in minutes, 0 = off. The backend stores it as bool
    // clean_mode + int clean_delay_min and keeps the two in sync, so
    // the core posts only clean_delay_min.
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    int cur = s.clean_delay_min;
    auto args = splitArgs(ctx.args);
    if (args.empty()) {
        replyHtml(ctx, ui_texts::fmt(ui_texts::kCleanModeStatus,
                                     {{"action", cur ? "on" : "off"},
                                      {"duration", std::to_string(cur)}}) +
                       degraded);
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
    if (!setSetting(chat, "clean_delay_min",
                    Json(static_cast<int64_t>(off ? 0 : minutes)))) {
        reply(ctx, "Could not save clean mode — the settings backend "
                   "is unreachable. Your change was not stored.");
        return;
    }
    if (off) {
        reply(ctx, "Clean mode off.");
        return;
    }
    reply(ctx, "Clean mode on: bot messages are deleted after " +
                   std::to_string(minutes) + " minute(s).");
}

void Dispatcher::onThumb(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    bool is_nothumb = ctx.command == "nothumb" || ctx.command == "nothumbs";
    auto args = splitArgs(ctx.args);
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    if (args.empty() || (!flagOn(args[0]) && !flagOff(args[0]))) {
        bool on = s.thumbnails;
        replyHtml(ctx, ui_texts::fmt(ui_texts::kThumbStatus,
                                     {{"on", on ? "on" : "off"}}) +
                       degraded);
        return;
    }
    if (!canManageVc(ctx)) return;
    // /nothumb on -> thumbnails off; /thumbnail on -> thumbnails on.
    bool thumb_on = is_nothumb ? flagOff(args[0]) : flagOn(args[0]);
    if (!setSetting(chat, "thumbnails", Json(thumb_on))) {
        reply(ctx, "Could not save thumbnails — the settings backend "
                   "is unreachable. Your change was not stored.");
        return;
    }
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
    ui_texts::ChatSettings s = settingsFor(chat);
    std::string degraded = settingsLive(chat) ? "" : kSettingsDegradedSuffix;
    // Wire "" means "any"; the status text displays it as auto/any.
    std::string lang = s.autoplay_lang.empty() ? "auto" : s.autoplay_lang;
    std::string mood = s.autoplay_mood.empty() ? "any" : s.autoplay_mood;
    if (args.empty()) {
        replyHtml(ctx, ui_texts::autoplayStatusText(s.autoplay, lang, mood) +
                       degraded);
        return;
    }
    if (!canManageVc(ctx)) return;
    std::string a = lowerStr(args[0]);
    if (a == "on" || a == "enable" || a == "start") {
        if (!setSetting(chat, "autoplay", Json(true))) {
            reply(ctx, "Could not save autoplay — the settings backend "
                       "is unreachable. Your change was not stored.");
            return;
        }
        replyKb(ctx, ui_texts::autoplayCardText(true),
                ui_texts::autoplayCardKeyboard(true));
    } else if (a == "off" || a == "disable" || a == "stop") {
        if (!setSetting(chat, "autoplay", Json(false))) {
            reply(ctx, "Could not save autoplay — the settings backend "
                       "is unreachable. Your change was not stored.");
            return;
        }
        replyKb(ctx, ui_texts::autoplayCardText(false),
                ui_texts::autoplayCardKeyboard(false));
    } else if ((a == "lang" || a == "language") && args.size() >= 2 &&
               std::find(langs.begin(), langs.end(), lowerStr(args[1])) !=
                   langs.end()) {
        // "auto" is the wire "" (any language).
        std::string v = lowerStr(args[1]);
        if (!setSetting(chat, "autoplay_lang",
                        Json(v == "auto" ? "" : v))) {
            reply(ctx, "Could not save autoplay language — the settings "
                       "backend is unreachable. Your change was not "
                       "stored.");
            return;
        }
        replyHtml(ctx, ui_texts::autoplayStatusText(
                           settingsFor(chat).autoplay, v, mood));
    } else if (a == "mood" && args.size() >= 2 &&
               std::find(moods.begin(), moods.end(), lowerStr(args[1])) !=
                   moods.end()) {
        // "any" is the wire "" (any mood).
        std::string v = lowerStr(args[1]);
        if (!setSetting(chat, "autoplay_mood",
                        Json(v == "any" ? "" : v))) {
            reply(ctx, "Could not save autoplay mood — the settings "
                       "backend is unreachable. Your change was not "
                       "stored.");
            return;
        }
        replyHtml(ctx, ui_texts::autoplayStatusText(
                           settingsFor(chat).autoplay, lang, v));
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
            cur = std::stoll(chatRtField(chat, "cplay", "0"));
        } catch (...) {
        }
        if (cur) {
            replyHtml(ctx, ui_texts::cplayStatusText(cur));
        } else {
            reply(ctx, "Usage: /channelplay <@channel | channel id | "
                       "disable>");
        }
        return;
    }
    if (!adminCheck(ctx)) return;
    std::string a = lowerStr(args[0]);
    if (a == "disable" || a == "off" || a == "none") {
        setChatRtField(chat, "cplay", "0");
        replyHtml(ctx, ui_texts::kCplayDisabled);
        return;
    }
    int64_t ch = 0;
    if (!args[0].empty() && args[0][0] == '@') {
        ch = tg_.getChatId(args[0]);
        if (ch == 0) {
            replyHtml(ctx, ui_texts::kCplayResolveFail);
            return;
        }
    } else {
        try {
            ch = std::stoll(args[0]);
        } catch (...) {
            replyHtml(ctx, ui_texts::kCplayInvalidTarget);
            return;
        }
    }
    if (ch >= 0 || ch == chat) {
        replyHtml(ctx, ui_texts::kCplayInvalidTarget);
        return;
    }
    // The bot must be an admin of the channel, and so must the linker.
    auto admins = tg_.getChatAdministrators(ch);
    if (admins.empty()) {
        replyHtml(ctx, ui_texts::kCplayNotAccessible);
        return;
    }
    int64_t me = bot_id_;
    bool bot_admin =
        std::find(admins.begin(), admins.end(), me) != admins.end();
    bool user_admin =
        std::find(admins.begin(), admins.end(), ctx.message.from.id) !=
        admins.end();
    if (!bot_admin) {
        replyHtml(ctx, ui_texts::kCplayNotAccessible);
        return;
    }
    if (!isSudo(ctx.message.from.id) && !user_admin) {
        replyHtml(ctx, ui_texts::kCplayOwnerRequired);
        return;
    }
    setChatRtField(chat, "cplay", std::to_string(ch));
    replyHtml(ctx, ui_texts::cplayEnabledText(ch));
}

void Dispatcher::onUserbot(const CommandContext& ctx) {
    int64_t chat = ctx.message.chat.id;
    int64_t target = 0;
    if (ctx.via_channel) {
        try {
            target = std::stoll(chatRtField(chat, "cplay", "0"));
        } catch (...) {
        }
        if (!target) {
            replyHtml(ctx, ui_texts::kCplayNotLinked);
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
        if (st == "kicked" || st == "restricted") {
            replyHtml(ctx, ui_texts::assistantRestrictedText("The assistant",
                                                            ub));
            return;
        }
        if (st == "member" || st == "administrator" || st == "creator") {
            reply(ctx, ui_texts::userbotAlreadyText("The assistant"));
            return;
        }
    }
    if (!assistant_.available() || !assistant_.authorized()) {
        reply(ctx, "The assistant session is not available here: " +
                       assistant_.lastError() +
                       ". Add the assistant user account to the chat "
                       "manually, or configure TD_API_ID/TD_API_HASH "
                       "for the assistant TDLib session.");
        return;
    }
    // The core cannot add the assistant account to a chat on its own
    // — it must already be a member. /join (vc.join) puts the
    // assistant's voice session into the call once it is a member.
    reply(ctx, "The assistant is not in this chat yet. Add the assistant "
               "user account to the chat manually, then use /join to put "
               "it in the voice call.");
}

// --- handlers: admin / sudo / broadcast / tools --------------------------------
void Dispatcher::onAuth(const CommandContext& ctx) {
    if (!adminCheck(ctx)) return;
    auto user = extractUserId(ctx);
    if (!user) {
        replyHtml(ctx, ui_texts::fmt(ui_texts::kAuthNoUser,
                                     {{"cmd", "/" + ctx.command}}));
        return;
    }
    int64_t chat = vcTarget(ctx);
    if (ctx.command == "auth") {
        if (isChatAdmin(chat, *user)) {
            replyHtml(ctx, ui_texts::kAuthIsAdmin);
            return;
        }
        if (*user == ctx.message.from.id || isOwner(*user)) {
            replyHtml(ctx, ui_texts::kAuthCannot);
            return;
        }
        auto auth = store_.smembers("auth:" + std::to_string(chat));
        if (std::find(auth.begin(), auth.end(), std::to_string(*user)) !=
            auth.end()) {
            replyHtml(ctx, ui_texts::kAuthAlready);
            return;
        }
        if (auth.size() >= kAuthMaxUsers) {
            replyHtml(ctx, ui_texts::authLimitText(
                               static_cast<int>(kAuthMaxUsers)));
            return;
        }
        store_.sadd("auth:" + std::to_string(chat), std::to_string(*user));
        replyHtml(ctx, ui_texts::authAddedText(
                           ui_texts::htmlEscape(tg_.getUserMention(*user)),
                           static_cast<int>(auth.size()) + 1,
                           static_cast<int>(kAuthMaxUsers)));
    } else {
        auto auth = store_.smembers("auth:" + std::to_string(chat));
        if (std::find(auth.begin(), auth.end(), std::to_string(*user)) ==
            auth.end()) {
            reply(ctx, "That user is not authorized in this chat.");
            return;
        }
        store_.srem("auth:" + std::to_string(chat), std::to_string(*user));
        replyHtml(ctx, ui_texts::authRemovedText(
                           ui_texts::htmlEscape(tg_.getUserMention(*user))));
    }
}

void Dispatcher::onAuthlist(const CommandContext& ctx) {
    if (!adminCheck(ctx)) return;
    auto auth = store_.smembers("auth:" + std::to_string(vcTarget(ctx)));
    if (auth.empty()) {
        replyHtml(ctx, ui_texts::kAuthlistEmpty);
        return;
    }
    std::vector<std::pair<std::string, int64_t>> users;
    for (const auto& u : auth) {
        try {
            int64_t id = std::stoll(u);
            users.emplace_back(tg_.getUserMention(id), id);
        } catch (...) {
        }
        if (users.size() >= kAuthMaxUsers) break;
    }
    replyKb(ctx, ui_texts::authlistText(
                     users, static_cast<int>(kAuthMaxUsers)),
            ui_texts::authlistKeyboard(vcTarget(ctx), users));
}

void Dispatcher::onAdmincache(const CommandContext& ctx) {
    static std::mutex mu;
    static std::map<int64_t, std::chrono::steady_clock::time_point> hist;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = hist.find(ctx.message.from.id);
        auto now = std::chrono::steady_clock::now();
        if (it != hist.end() && now < it->second) {
            auto remain = std::chrono::duration_cast<std::chrono::seconds>(
                              it->second - now)
                              .count();
            reply(ctx, ui_texts::reloadCooldownText(
                           ui_texts::mmss(remain)));
            return;
        }
        hist[ctx.message.from.id] = now + std::chrono::minutes(10);
    }
    cachedAdmins(ctx.message.chat.id, true);
    reply(ctx, ui_texts::kReloadShortOk);
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
        if (is_chat) {
            replyHtml(ctx, "Chat blocked: <code>" + std::to_string(id) +
                               "</code>");
        } else {
            replyHtml(ctx, ui_texts::blBlockedUserText(id));
        }
    } else {
        if (!isBlChat(id) && !isBlUser(id)) {
            reply(ctx, "Not blacklisted.");
            return;
        }
        if (!is_chat) store_.srem("bl_users", std::to_string(id));
        store_.srem("bl_chats", std::to_string(id));
        replyHtml(ctx, "Removed <code>" + std::to_string(id) +
                           "</code> from the blacklist.");
    }
}

void Dispatcher::onBlacklisted(const CommandContext& ctx) {
    auto chats = store_.smembers("bl_chats");
    auto users = store_.smembers("bl_users");
    std::string out = ui_texts::kBlacklistTitle;
    out += "\nChats (" + std::to_string(chats.size()) + ") and users (" +
           std::to_string(users.size()) + "):\n";
    for (const auto& c : chats) out += "chat <code>" + c + "</code>\n";
    for (const auto& u : users) out += "user <code>" + u + "</code>\n";
    if (out.size() > 3500) out = out.substr(0, 3500) + "\n...";
    replyHtml(ctx, out);
}

void Dispatcher::onAddSudo(const CommandContext& ctx) {
    auto user = extractUserId(ctx);
    if (!user) {
        reply(ctx, "Reply to a user or give a user id/@username.");
        return;
    }
    if (ctx.command == "addsudo") {
        if (*user == ctx.message.from.id) {
            replyHtml(ctx, ui_texts::kSudoSelfJoke);
            return;
        }
        if (isSudo(*user)) {
            reply(ctx, "That user is already a sudo user.");
            return;
        }
        store_.sadd("sudoers", std::to_string(*user));
        reply(ctx, "Added sudo user " + std::to_string(*user) + ".");
    } else {
        if (isOwner(*user)) {
            replyHtml(ctx, ui_texts::kSudoOwnerRemoveBlock);
            return;
        }
        if (!isSudo(*user)) {
            reply(ctx, "That user is not a sudo user (or is the owner).");
            return;
        }
        store_.srem("sudoers", std::to_string(*user));
        reply(ctx, "Removed sudo user " + std::to_string(*user) + ".");
    }
}

void Dispatcher::onListSudo(const CommandContext& ctx) {
    std::string out = ui_texts::kSudoHeader;
    out += "\nOwner: ";
    out += cfg_.owner_id ? std::to_string(cfg_.owner_id) : "(unset)";
    out += "\n";
    auto sudoers = store_.smembers("sudoers");
    if (sudoers.empty()) {
        out += ui_texts::kSudoNoExtra;
    } else {
        out += "Sudo users:\n";
        for (const auto& s : sudoers) out += "- " + s + "\n";
    }
    replyHtml(ctx, out);
}

void Dispatcher::onActiveVc(const CommandContext& ctx) {
    auto keys = store_.keys("call:");
    if (keys.empty()) {
        reply(ctx, ui_texts::kActiveStreamsEmpty);
        return;
    }
    if (ctx.command == "ac") {
        replyHtml(ctx, ui_texts::activeChatsText(
                           static_cast<int>(keys.size())));
        return;
    }
    std::string out = std::string(ui_texts::kActiveStreamsHeader) + "\n";
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
               ui_texts::htmlEscape(title) + "\n";
        if (out.size() > 3500) break;
    }
    replyHtml(ctx, out);
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
    // Attach the cancel keyboard (§18); cbBcast sets bcast_cancel_.
    if (sent_id > 0) {
        std::string kb_err;
        tg_.editMessageReplyMarkup(
            ctx.message.chat.id, sent_id,
            ui_texts::bcastKeyboard(), &kb_err);
    }
    bcast_running_.store(true);
    bcast_cancel_.store(false);
    int count = 0, failed = 0;
    // One id per line, after a header that says what the failure
    // was: the TDLib forward/copy calls return only
    // success/failure, so no per-chat error text exists to list —
    // each id below is a chat that rejected the broadcast.
    std::string failures =
        "# chats that rejected the broadcast (forward/copy failed; "
        "TDLib returns no per-chat error text)\n";
    for (int64_t target : targets) {
        if (bcast_cancel_.load()) break;
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
    bool cancelled = bcast_cancel_.load();
    bcast_running_.store(false);
    bcast_cancel_.store(false);
    // Clear the cancel keyboard from the progress message.
    if (sent_id > 0) {
        std::string kb_err;
        tg_.editMessageReplyMarkup(ctx.message.chat.id, sent_id,
                                   InlineKeyboard{}, &kb_err);
    }
    std::string text = cancelled
                           ? ui_texts::kBcastCancelDone
                           : "Broadcast done: " + std::to_string(count) +
                                 " delivered, " + std::to_string(failed) +
                                 " failed.";
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
        replyHtml(ctx, ui_texts::fmt(
                           ui_texts::kMaintUsage,
                           {{"state", maintenanceOn() ? "ON" : "OFF"},
                            {"reason", reason && !reason->empty() ? *reason
                                                                  : "none"}}));
        return;
    }
    if (flagOff(args[0])) {
        if (!maintenanceOn()) {
            replyHtml(ctx, ui_texts::kMaintAlreadyOff);
            return;
        }
        store_.set("maintenance", "0");
        store_.set("maint_reason", "");
        replyHtml(ctx, ui_texts::kMaintDisabled);
    } else if (flagOn(args[0])) {
        if (maintenanceOn()) {
            replyHtml(ctx, ui_texts::kMaintAlreadyOn);
            return;
        }
        std::string reason;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!reason.empty()) reason += " ";
            reason += args[i];
        }
        store_.set("maintenance", "1");
        store_.set("maint_reason", reason);
        replyHtml(ctx, ui_texts::maintEnabledText(
                           reason.empty() ? "none" : reason));
    } else {
        auto reason = store_.get("maint_reason");
        replyHtml(ctx, ui_texts::fmt(
                           ui_texts::kMaintUsage,
                           {{"state", maintenanceOn() ? "ON" : "OFF"},
                            {"reason", reason && !reason->empty() ? *reason
                                                                  : "none"}}));
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
    // HONEST GAP (Phase 2): the network probes are gone. They ran
    // over HttpClient (deleted with telegram_api/http.cpp), and the
    // core no longer ships an HTTPS client — spawning curl per call
    // is prohibited (FINAL-ARCHITECTURE.md §1.2.3). The pure helpers
    // speedMbps / formatSpeedtest stay (and stay tested); a live
    // probe needs a backend action (Phase 3+).
    reply(ctx, "Speed test is not available yet — the network probes "
               "need a backend action that does not exist yet. "
               "Run a speed test from the host shell instead.");
}

void Dispatcher::onBug(const CommandContext& ctx) {
    std::string text = trim(ctx.args);
    if (text.empty()) {
        reply(ctx, ui_texts::fmt(ui_texts::kBugUsage,
                                 {{"cmd", "/" + ctx.command}}));
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
    std::string report = ui_texts::fmt(
        ui_texts::kBugReport,
        {{"from", ui_texts::htmlEscape(displayName(ctx.message.from))},
         {"id", std::to_string(ctx.message.from.id)},
         {"where", std::to_string(ctx.message.chat.id)},
         {"text", ui_texts::htmlEscape(text)}});
    if (tg_.sendMessageHtml(dest, report, InlineKeyboard{}, 0, &err)) {
        reply(ctx, ui_texts::kBugThanks);
    } else {
        reply(ctx, ui_texts::kBugFailed);
    }
}

void Dispatcher::onPrivacy(const CommandContext& ctx) {
    replyHtml(ctx, ui_texts::kPrivacy);
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
    std::string lat_str = b;
    std::snprintf(b, sizeof b, "%.1f", cpu);
    std::string cpu_str = std::string(b) + "%";
    std::snprintf(b, sizeof b, "%.1f", mem_used_pct);
    std::string ram_str = std::string(b) + "% of ";
    std::snprintf(b, sizeof b, "%.1f", mem_total_gb);
    ram_str += std::string(b) + " GB (RSS ";
    std::snprintf(b, sizeof b, "%.1f", rss_mb);
    ram_str += std::string(b) + " MB)";
    std::snprintf(b, sizeof b, "%.1f", disk_pct);
    std::string disk_str = std::string(b) + "%";
    std::string card = ui_texts::pingCardText(
        bot_username_.empty() ? "Parlay" : "@" + bot_username_, lat_str,
        up_str, ram_str, cpu_str, disk_str, "");
    if (!cfg_.ping_img.empty()) {
        replyPhoto(ctx, cfg_.ping_img, card, InlineKeyboard{});
    } else {
        replyHtml(ctx, card);
    }
}

void Dispatcher::onStats(const CommandContext& ctx) {
    int64_t up = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - boot_time_)
            .count());
    std::string up_str = std::to_string(up / 3600) + "h " +
                         std::to_string((up / 60) % 60) + "m " +
                         std::to_string(up % 60) + "s";
    double mem_used_pct = 0, mem_total_gb = 0, disk_pct = 0, rss_mb = 0;
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
    {
        struct statvfs s {};
        if (statvfs("/", &s) == 0) {
            double total = static_cast<double>(s.f_blocks) * s.f_frsize;
            double free = static_cast<double>(s.f_bfree) * s.f_frsize;
            if (total > 0) disk_pct = (total - free) / total * 100.0;
        }
    }
    {
        std::ifstream f("/proc/self/statm");
        long pages = 0, rss = 0;
        f >> pages >> rss;
        rss_mb = rss * static_cast<double>(sysconf(_SC_PAGESIZE)) /
                 (1024.0 * 1024.0);
    }
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
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", cpu);
    std::string system_block = "Uptime: " + up_str + "\nCPU: " + b + "%\nCores: " +
                               std::to_string(std::thread::hardware_concurrency());
    std::snprintf(b, sizeof b, "%.1f", mem_used_pct);
    std::string memory_block = "RAM: " + std::string(b) + "% of ";
    std::snprintf(b, sizeof b, "%.1f", mem_total_gb);
    memory_block += std::string(b) + " GB\nRSS: ";
    std::snprintf(b, sizeof b, "%.1f", rss_mb);
    memory_block += std::string(b) + " MB";
    std::snprintf(b, sizeof b, "%.1f", disk_pct);
    // (No "Store:" line: the core's only store is the JSON file —
    // see store.hpp. There is no database backend to report.)
    std::string server_block = "Disk: " + std::string(b) + "%";
    std::string bot_block =
        "Chats: " + std::to_string(store_.smembers("chats").size()) +
        "\nUsers: " + std::to_string(store_.smembers("users").size()) +
        "\nSudoers: " +
        std::to_string(store_.smembers("sudoers").size() +
                       (cfg_.owner_id ? 1 : 0)) +
        "\nBlacklisted chats: " +
        std::to_string(store_.smembers("bl_chats").size()) +
        "\nBlacklisted users: " +
        std::to_string(store_.smembers("bl_users").size()) +
        "\nActive voice chats: " +
        std::to_string(store_.keys("call:").size()) +
        "\nCommands registered: " + std::to_string(routes_.size()) +
        " routes";
    std::string versions_block =
        "Build: C++20, MTProto (TDLib) + UDS backend\nBackend: " +
        (ipc_.healthz("") ? std::string("reachable")
                          : std::string("unreachable"));
    replyHtml(ctx, ui_texts::fmt(ui_texts::kStatsCard,
                                 {{"system", system_block},
                                  {"memory", memory_block},
                                  {"server", server_block},
                                  {"bot", bot_block},
                                  {"versions", versions_block},
                                  {"library", ""}}));
}

void Dispatcher::onMenu(const CommandContext& ctx) {
    // Admin-only remix panel (§2.3): the current track + next track
    // with the merged control keyboard. Panel presses are requester-
    // only (see cbPlayer).
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    Json np, track;
    std::string now_title, now_url, next_title, next_url;
    if (nowPlaying(chat, np, track)) {
        now_title = track.find("title") ? track.find("title")->asString() : "";
        now_url = track.find("url") ? track.find("url")->asString() : "";
        Json q;
        std::string qerr;
        // Waiting-only list (§1.7): the next track is items[0] —
        // the current track is not in the list.
        if (apiQueueList(chat, q, qerr) && q.size() > 0) {
            const Json* nt = q.asArray()[0].find("track");
            if (nt) {
                next_title =
                    nt->find("title") ? nt->find("title")->asString() : "";
                next_url =
                    nt->find("url") ? nt->find("url")->asString() : "";
            }
        }
    }
    int64_t msg = replyKb(ctx,
                          ui_texts::menuPanelText(now_title, now_url,
                                                  next_title, next_url),
                          ui_texts::menuPanelKeyboard(chat));
    if (msg > 0) {
        store_.set("panelmsg:" + std::to_string(chat), std::to_string(msg));
        store_.set("paneluser:" + std::to_string(chat),
                   std::to_string(ctx.message.from.id));
    }
}

void Dispatcher::onId(const CommandContext& ctx) {
    replyHtml(ctx, ui_texts::idText(ctx.message.chat.id, 0));
}

void Dispatcher::onKeyboard(const CommandContext& ctx) {
    std::string a = lowerStr(trim(ctx.args));
    if (a == "off" || a == "remove" || a == "hide") {
        replyReplyKb(ctx, "Keyboard removed.", ReplyKeyboard{}, true);
        return;
    }
    replyReplyKb(ctx, ui_texts::kKeyboardEnabled,
                 ui_texts::persistentKeyboard());
}

void Dispatcher::onAbout(const CommandContext& ctx) {
    // The channel handle comes from the updates URL when it is a
    // t.me link; otherwise the repo name stands in.
    std::string channel = "ParlayMusic";
    if (cfg_.updates_url.rfind("https://t.me/", 0) == 0)
        channel = cfg_.updates_url.substr(13);
    replyHtml(ctx, ui_texts::fmt(ui_texts::kAbout, {{"channel", channel}}));
}

void Dispatcher::onStatus(const CommandContext& ctx) {
    // Music163bot's /status counts cached songs; the core keeps no
    // song cache, so the counts are honestly zero.
    replyHtml(ctx, ui_texts::fmt(ui_texts::kStatusText,
                                 {{"total", "0"},
                                  {"chat", std::to_string(ctx.message.chat.id)},
                                  {"chatCount", "0"},
                                  {"userID", std::to_string(ctx.message.from.id)},
                                  {"userCount", "0"}}));
}

void Dispatcher::onNow(const CommandContext& ctx) {
    Json np, track;
    if (!nowPlaying(vcTarget(ctx), np, track)) {
        replyHtml(ctx, ui_texts::kNowIdle);
        return;
    }
    std::string title =
        track.find("title") ? track.find("title")->asString() : "";
    std::string url = track.find("url") ? track.find("url")->asString() : "";
    auto req = store_.get("last_requester_name:" +
                          std::to_string(vcTarget(ctx)));
    auto req_id = store_.get("last_requester_id:" +
                             std::to_string(vcTarget(ctx)));
    std::string requester = req ? *req : "someone";
    std::string requester_url =
        req_id ? "tg://user?id=" + *req_id : url;
    replyHtml(ctx, ui_texts::nowText(ui_texts::htmlEscape(title), url,
                                     ui_texts::htmlEscape(requester),
                                     requester_url));
}

void Dispatcher::onRepo(const CommandContext& ctx) {
    std::string url = cfg_.source_url.empty()
                          ? "https://github.com/filewrap/ParlayMusic"
                          : cfg_.source_url;
    InlineKeyboard kb = {
        {InlineButton::link("Source Code", url)},
    };
    replyKb(ctx, "Source code: <a href=\"" + url + "\">Parlay</a>", kb);
}

void Dispatcher::onDownloadSettings(const CommandContext& ctx) {
    replyKb(ctx, ui_texts::kDlSettingsIndex,
            ui_texts::dlSettingsIndexKeyboard());
}

void Dispatcher::onEval(const CommandContext& ctx) {
    // Owner-only remote shell (NoxBeats tools.cpp /eval). 60 s cap,
    // last 3000 chars of output.
    std::string cmd = trim(ctx.args);
    if (cmd.empty()) {
        reply(ctx, ui_texts::kEvalNoInput);
        return;
    }
    reply(ctx, "Running...");
    int code = 0;
    std::string out = runShell(cmd, 60, code);
    if (out.size() > 3000) out = "..." + out.substr(out.size() - 3000);
    if (out.empty()) out = "(no output)";
    std::string body =
        "<b>Input:</b>\n<code>" +
        ui_texts::htmlEscape(cmd.substr(0, 800)) + "</code>\n" +
        ui_texts::evalOutputText(ui_texts::htmlEscape(out)) + "\nExit: " +
        std::to_string(code);
    if (code != 0) body = std::string(ui_texts::kEvalError) + body;
    replyHtml(ctx, body);
}

void Dispatcher::onLogs(const CommandContext& ctx) {
    std::string out;
    bool file_missing = false;
    if (!cfg_.log_file.empty()) {
        std::ifstream f(cfg_.log_file);
        if (!f) {
            file_missing = true;
        } else {
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
    }
    if (out.empty()) {
        std::lock_guard<std::mutex> lk(log_mu_);
        if (recent_log_.empty()) {
            reply(ctx, file_missing ? ui_texts::kLogsMissing
                                    : ui_texts::kLogsNone);
            return;
        }
        out = "Recent commands (in-memory log):\n";
        for (const auto& line : recent_log_) out += line + "\n";
    }
    if (out.size() > 3500) out = out.substr(0, 3500) + "\n...";
    reply(ctx, out);
}

void Dispatcher::onLogger(const CommandContext& ctx) {
    auto args = splitArgs(ctx.args);
    auto on = store_.get("logger_on");
    bool enabled = !(on && *on == "0");
    if (args.empty() || (args[0] != "on" && args[0] != "off" &&
                        args[0] != "enable" && args[0] != "disable")) {
        reply(ctx, ui_texts::fmt(ui_texts::kLoggerUsage,
                                 {{"status", enabled ? "ON" : "OFF"}}));
        return;
    }
    bool want_on = args[0] == "on" || args[0] == "enable";
    if (want_on && cfg_.logger_id == 0) {
        reply(ctx, ui_texts::kLoggerNeedId);
        return;
    }
    store_.set("logger_on", want_on ? "1" : "0");
    reply(ctx, want_on ? ui_texts::kLoggerEnabled
                       : ui_texts::kLoggerDisabled);
}

void Dispatcher::onRestart(const CommandContext& ctx) {
    // A song playing in this chat gets the confirm card (§25);
    // otherwise restart at once.
    Json np, track;
    if (nowPlaying(vcTarget(ctx), np, track)) {
        replyKb(ctx, ui_texts::kRestartConfirm,
                ui_texts::restartConfirmKeyboard(vcTarget(ctx)));
        return;
    }
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
    // RTMP targets carry a stream key: set them in a private chat only
    // (§20) — a key typed in a group is visible to every member.
    if (ctx.message.chat.type != "private") {
        reply(ctx, ui_texts::kRtmpDmOnly);
        return;
    }
    auto args = splitArgs(ctx.args);
    int64_t chat = ctx.message.chat.id;
    if (args.empty()) {
        std::string url = chatRtField(chat, "rtmp_url", "");
        reply(ctx, url.empty()
                       ? ui_texts::kRtmpNotSet
                       : "RTMP target is set (key hidden). Use /setrtmp "
                         "<url> <key> to replace it.");
        return;
    }
    if (args.size() == 1 && lowerStr(args[0]) == "off") {
        setChatRtField(chat, "rtmp_url", "");
        setChatRtField(chat, "rtmp_key", "");
        reply(ctx, ui_texts::kRtmpRemoved);
        return;
    }
    if (args.size() < 2) {
        reply(ctx, "Usage: /setrtmp <rtmp url> <stream key>");
        return;
    }
    if (args[0].rfind("rtmp://", 0) != 0 &&
        args[0].rfind("rtmps://", 0) != 0) {
        replyHtml(ctx, ui_texts::kRtmpInvalidUrl);
        return;
    }
    // The key is stored but never echoed back (NoxBeats rtmp.cpp scrub()).
    setChatRtField(chat, "rtmp_url", args[0]);
    setChatRtField(chat, "rtmp_key", args[1]);
    reply(ctx, ui_texts::kRtmpSaved);
}

void Dispatcher::onStream(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    std::string url = chatRtField(chat, "rtmp_url", "");
    std::string key = chatRtField(chat, "rtmp_key", "");
    if (url.empty()) {
        reply(ctx, ui_texts::kRtmpNotSet);
        return;
    }
    std::string query = trim(ctx.args);
    if (query.empty()) {
        Json np, track;
        if (!nowPlaying(chat, np, track)) {
            replyHtml(ctx, ui_texts::kRtmpStreamUsage);
            return;
        }
        query = track.find("title") ? track.find("title")->asString() : "";
    }
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        if (rtmp_streams_.count(chat)) {
            reply(ctx, ui_texts::kRtmpAlreadyRunning);
            return;
        }
    }
    // Resolve the video to a file via the downloaders service.
    Json body = Json::objectOf({
        {"query", Json(query)},
        {"requested_by", Json(ctx.message.from.id)},
    });
    IpcResult r = ipc_.post("download/youtube", body, 120);
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
        reply(ctx, ui_texts::kRtmpStartFailed);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        rtmp_streams_[chat] = {pid, log_path,
                               std::chrono::steady_clock::now(), query};
    }
    replyHtml(ctx, ui_texts::rtmpStreamStartedText(
                       ui_texts::htmlEscape(query),
                       displayName(ctx.message.from)) +
                       "\n\n" + ui_texts::kRtmpDelayNote);
}

void Dispatcher::onStreamStop(const CommandContext& ctx) {
    if (!canManageVc(ctx)) return;
    int64_t chat = vcTarget(ctx);
    pid_t pid = 0;
    {
        std::lock_guard<std::mutex> lk(rtmp_mu_);
        auto it = rtmp_streams_.find(chat);
        if (it == rtmp_streams_.end()) {
            reply(ctx, ui_texts::kRtmpNone);
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
    replyHtml(ctx, ui_texts::rtmpStoppedText(displayName(ctx.message.from)));
}

void Dispatcher::onStreamStatus(const CommandContext& ctx) {
    int64_t chat = vcTarget(ctx);
    std::lock_guard<std::mutex> lk(rtmp_mu_);
    auto it = rtmp_streams_.find(chat);
    if (it == rtmp_streams_.end()) {
        reply(ctx, ui_texts::kRtmpNoneActive);
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
    std::string key = chatRtField(chat, "rtmp_key", "");
    if (!key.empty()) {
        size_t p = 0;
        while ((p = tail.find(key, p)) != std::string::npos) {
            tail.replace(p, key.size(), "***");
            p += 3;
        }
    }
    replyHtml(ctx, ui_texts::rtmpStatusText(
                        ui_texts::htmlEscape(it->second.what),
                        ui_texts::mmss(secs)) +
                        " (ffmpeg pid " + std::to_string(it->second.pid) +
                        ").\n" + ui_texts::htmlEscape(tail));
}

// --- Phase 9: callback router (UI_INVENTORY.md §1) -------------------------
// One namespace, colon-separated, 64-byte hard limit:
//   pl:<domain>:<verb>[:<arg>…]
// Unknown callbacks get a polite alert, never a crash. Player/queue
// buttons are admin-or-requester gated (§28.2): sudo, chat admin, auth
// user, or admin_mode==2. Search buttons are requester-only. The remix
// /menu panel is the exception: other users' taps are silently ignored.
void Dispatcher::handleCallback(const TgCallbackQuery& cq) {
    if (cq.data.empty() || cq.data.size() > 64) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    std::vector<std::string> parts = splitCb(cq.data);
    if (parts.size() < 2 || parts[0] != "pl") {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    // A button press proves the user is looking at this chat — same
    // last_chat signal as a group message (§13). Group ids are
    // negative; inline-mode buttons carry no message (chat_id 0).
    if (cq.chat_id < 0) noteLastChat(cq.from.id, cq.chat_id);
    const std::string& domain = parts[1];
    try {
        if (domain == "help") cbHelp(cq, parts);
        else if (domain == "start") cbStart(cq);
        else if (domain == "close") cbClose(cq);
        else if (domain == "player") cbPlayer(cq, parts);
        else if (domain == "queue") cbQueue(cq, parts);
        else if (domain == "set") cbSet(cq, parts);
        else if (domain == "lang") cbLang(cq, parts);
        else if (domain == "dl") cbDl(cq, parts);
        else if (domain == "bcast") cbBcast(cq);
        else if (domain == "auth") cbAuth(cq, parts);
        else if (domain == "search") cbSearch(cq, parts);
        else if (domain == "vote") cbVote(cq, parts);
        else if (domain == "share") cbShare(cq, parts);
        else if (domain == "del") cbDel(cq, parts);
        else if (domain == "lossy") cbLossy(cq, parts);
        else if (domain == "restart") cbRestart(cq, parts);
        else if (domain == "dlset") cbDlset(cq, parts);
        else if (domain == "spotify") cbSpotify(cq, parts);
        else
            answerCb(cq, ui_texts::kToastUnknownAction, true);
    } catch (const std::exception& e) {
        std::cerr << "[cb] handler for " << cq.data
                  << " threw: " << e.what() << "\n";
        answerCb(cq, ui_texts::kToastFeatureUnavailable, true);
    }
}

void Dispatcher::handleMyChatMember(const TgChatMemberUpdate& upd) {
    // Demotion goodbye (§2): the bot was an admin and is now a plain
    // member — it can no longer manage the voice chat, so it says so
    // and leaves. Any other transition is not ours to act on.
    if (upd.old_status != "administrator" || upd.new_status != "member")
        return;
    if (bot_id_ != 0 && upd.user_id != bot_id_) return;
    CommandContext ctx;
    ctx.message.chat.id = upd.chat_id;
    ctx.message.message_id = 0;
    ctx.command = "start";
    replyHtml(ctx, ui_texts::kDemoted);
    tg_.leaveChat(upd.chat_id);
}

// --- inline queries (§13, Phase 9 gaps) ------------------------------------

void Dispatcher::noteLastChat(int64_t user_id, int64_t chat_id) {
    std::string key = "last_chat:" + std::to_string(user_id);
    std::string val = std::to_string(chat_id);
    auto cur = store_.get(key);
    if (cur && *cur == val) return;  // already current: no disk write
    store_.set(key, val);
}

int64_t Dispatcher::inlineChatId(int64_t user_id) const {
    auto v = store_.get("last_chat:" + std::to_string(user_id));
    if (!v) return 0;
    try {
        return std::stoll(*v);
    } catch (...) {
        return 0;
    }
}

std::string Dispatcher::inlineModeOf(const std::string& query,
                                     std::string& arg_out) {
    // Mirrors vc-ts resolvers.js inlineMode exactly (same order):
    // exact np/s/l/help; "lib|del|search <arg>"; bare lib/del/search;
    // <3 chars -> hint; otherwise a search for the whole query.
    std::string q = trim(query);
    arg_out.clear();
    if (q.empty()) return "hint";
    std::string lower = lowerStr(q);
    if (lower == "np") return "np";
    if (lower == "s") return "s";
    if (lower == "l") return "l";
    if (lower == "help") return "help";
    size_t space = q.find_first_of(" \t\r\n");
    if (space != std::string::npos) {
        std::string head = lowerStr(q.substr(0, space));
        std::string rest = trim(q.substr(space + 1));
        if ((head == "lib" || head == "del" || head == "search") &&
            !rest.empty()) {
            arg_out = rest;
            return head;
        }
    }
    if (lower == "lib" || lower == "del" || lower == "search")
        return lower;
    if (utf8Len(q) < 3) return "hint";
    arg_out = q;
    return "search";
}

Json Dispatcher::inlineArticleResult(const std::string& id,
                                     const std::string& title,
                                     const std::string& description,
                                     const std::string& message_text,
                                     const std::string& thumb_url) {
    // Plain-text message (no parse_mode): titles and lyrics can
    // contain '<' '>' '&' that would break HTML parsing.
    Json content = Json::objectOf({{"message_text", Json(message_text)}});
    Json r = Json::objectOf({
        {"type", Json("article")},
        {"id", Json(id)},
        {"title", Json(title)},
        {"input_message_content", content},
    });
    if (!description.empty()) r.set("description", Json(description));
    if (!thumb_url.empty()) r.set("thumb_url", Json(thumb_url));
    return r;
}

Json Dispatcher::inlineTrackResult(const Json& track, const std::string& id,
                                   bool with_button) {
    std::string title =
        track.find("title") ? track.find("title")->asString() : "";
    std::string artist =
        track.find("artist") ? track.find("artist")->asString() : "";
    std::string stream =
        track.find("stream_url") ? track.find("stream_url")->asString()
                                 : "";
    std::string url =
        track.find("url") ? track.find("url")->asString() : "";
    std::string thumb =
        track.find("thumbnail") ? track.find("thumbnail")->asString() : "";
    int64_t dur = track.find("duration_sec")
                      ? track.find("duration_sec")->asInt()
                      : 0;
    Json open_kb = Json(nullptr);
    if (with_button && !bot_username_.empty()) {
        // Search-result card keyboard (NoxBeats yt_key + Music163bot
        // guestKeyboard): a Copy link button for the track URL plus
        // the deep-link button that opens the bot with the track.
        // Serialised in the Bot API shape; answerInlineQuery's
        // inlineResult translation carries both kinds through to
        // TDLib (copy_text + url), so the sent result keeps them.
        std::string link_url = !url.empty() ? url : stream;
        InlineKeyboard card_kb = ui_texts::inlineSearchResultKeyboard(
            link_url, bot_username_, id);
        Json::Array rows;
        for (const auto& row : card_kb) {
            Json::Array brow;
            for (const auto& b : row) {
                Json bj = Json::objectOf({{"text", Json(b.text)}});
                if (!b.copy_text.empty())
                    bj.set("copy_text", Json::objectOf(
                                            {{"text", Json(b.copy_text)}}));
                else if (!b.url.empty())
                    bj.set("url", Json(b.url));
                else if (!b.callback_data.empty())
                    bj.set("callback_data", Json(b.callback_data));
                brow.push_back(bj);
            }
            rows.push_back(Json(brow));
        }
        if (!rows.empty())
            open_kb = Json::objectOf(
                {{"inline_keyboard", Json(rows)}});
    }
    if (!stream.empty()) {
        // Audio result (lemonic §6.7, navidrome §7.9.2/§7.9.7): the
        // audio's own title/performer/duration/thumbnail are used.
        // NOT a cached-audio result: the Track carries no file_id
        // (see phase9/port-core.md) — a plain audio result from the
        // direct stream URL is the honest form.
        Json r = Json::objectOf({
            {"type", Json("audio")},
            {"id", Json(id)},
            {"audio_url", Json(stream)},
            {"title", Json(title)},
            {"performer", Json(artist)},
        });
        if (dur > 0) r.set("audio_duration", Json(dur));
        if (!thumb.empty()) r.set("thumb_url", Json(thumb));
        r.set("caption", Json("<b>" + ui_texts::htmlEscape(title) +
                              "</b> - " + ui_texts::htmlEscape(artist)));
        r.set("parse_mode", Json("HTML"));
        if (!open_kb.isNull()) r.set("reply_markup", open_kb);
        return r;
    }
    // Article fallback: a page URL is not playable audio. YouTube /
    // lemonic search results carry url only; JioSaavn carries a
    // stream_url and takes the audio branch above. The [Open in
    // Parlay Bot] button is the functional path to the track.
    std::string desc = artist;
    std::string dur_str = track.find("duration_str")
                              ? track.find("duration_str")->asString()
                              : "";
    if (dur_str.empty() && dur > 0) dur_str = ui_texts::mmss(dur);
    if (!dur_str.empty()) desc += " · " + dur_str;
    std::string msg = artist + " — " + title;
    if (!url.empty()) msg += "\n" + url;
    Json r = inlineArticleResult(id, title, desc, msg, thumb);
    if (!open_kb.isNull()) r.set("reply_markup", open_kb);
    return r;
}

void Dispatcher::storeInlineResults(int64_t user_id, const Json& results) {
    store_.set("inlinesearch:" + std::to_string(user_id), results.dump());
}

bool Dispatcher::loadInlineTrack(int64_t user_id, const std::string& track_id,
                                 Json& track_out) {
    auto v = store_.get("inlinesearch:" + std::to_string(user_id));
    if (!v || v->empty()) return false;
    Json results;
    try {
        results = Json::parse(*v);
    } catch (...) {
        return false;
    }
    if (!results.isArray()) return false;
    for (const auto& t : results.asArray()) {
        if (t.find("id") && t.find("id")->asString() == track_id) {
            track_out = t;
            return true;
        }
    }
    return false;
}

void Dispatcher::handleInlineQuery(const TgInlineQuery& iq) {
    // Silent auth rule (§13): non-allowed users get no results at all.
    if (isBlUser(iq.from.id) ||
        (maintenanceOn() && !isSudo(iq.from.id))) {
        std::string err;
        tg_.answerInlineQuery(iq.id, Json(Json::Array{}), 0, "", "", &err);
        return;
    }
    auto answer_empty = [&](int cache_time) {
        std::string err;
        tg_.answerInlineQuery(iq.id, Json(Json::Array{}), cache_time, "",
                              "", &err);
    };
    std::string arg;
    std::string mode = inlineModeOf(iq.query, arg);
    if (mode == "hint") {
        std::string err;
        tg_.answerInlineQuery(iq.id, Json(Json::Array{}), 30,
                              ui_texts::kInlineHintText, "help", &err);
        return;
    }
    if (mode == "help") {
        Json results = Json(Json::Array{
            inlineArticleResult("help1", ui_texts::kInlineHelpTitle1, "",
                                ui_texts::kInlineHelpMessage, ""),
            inlineArticleResult("help2", ui_texts::kInlineHelpTitle2, "",
                                ui_texts::kInlineHelpMessage, ""),
        });
        std::string err;
        tg_.answerInlineQuery(iq.id, results, 3600, "", "", &err);
        return;
    }
    if (mode == "search" && arg.empty()) {
        Json results = Json(Json::Array{inlineArticleResult(
            "keyword", ui_texts::kInlineNeedKeyword, "",
            ui_texts::kInlineNeedKeyword, "")});
        std::string err;
        tg_.answerInlineQuery(iq.id, results, 3600, "", "", &err);
        return;
    }
    // np/s/l need the chat the user is playing in (ruling 2). No
    // known chat -> the engine cannot answer them: empty, honestly.
    int64_t chat_id = 0;
    bool need_chat = mode == "np" || mode == "s" || mode == "l";
    if (need_chat) {
        chat_id = inlineChatId(iq.from.id);
        if (chat_id == 0) {
            answer_empty(0);
            return;
        }
    }
    Json body = Json::objectOf({
        {"query", Json(iq.query)},
        {"user_id", Json(iq.from.id)},
    });
    if (need_chat) body.set("chat_id", Json(chat_id));
    IpcResult r = ipc_.post("inline", body, 15);
    if (!r.ok) {
        // Engine down: answer empty rather than leaving the query
        // hanging (Telegram shows "no results" instead of an error).
        answer_empty(0);
        return;
    }
    const Json* results = r.data.find("results");
    if (!results || results->size() == 0) {
        if (mode == "search") {
            Json art = Json(Json::Array{inlineArticleResult(
                "noresults", ui_texts::kInlineNoResults, "",
                ui_texts::kInlineNoResults, "")});
            std::string err;
            tg_.answerInlineQuery(iq.id, art, 3600, "", "", &err);
            return;
        }
        // np/s/l/lib/del with nothing: empty results (§13).
        answer_empty(0);
        return;
    }
    if (mode == "l") {
        // The engine returns the current track only — the lyrics text
        // comes from /v1/lyrics with the same chat_id (ruling 3).
        const Json& track = results->asArray()[0];
        std::string title = track.find("title")
                                ? track.find("title")->asString()
                                : "";
        std::string artist = track.find("artist")
                                 ? track.find("artist")->asString()
                                 : "";
        // vc.lyrics is POST /v1/lyrics in the service (body
        // {chat_id}); the contract writes GET, but the GET form 404s
        // — the route sits behind vc-ts's POST-only gate (main.js).
        // Flagged for the coordinator in phase9/port-core.md.
        Json lbody = Json::objectOf({{"chat_id", Json(chat_id)}});
        IpcResult lr = ipc_.post("lyrics", lbody, 15);
        std::string lyrics;
        if (lr.ok) {
            const Json* lx = lr.data.find("lyrics");
            if (lx) lyrics = lx->asString();
        }
        if (lyrics.empty()) {
            answer_empty(0);  // no lyrics for this track: empty (§13)
            return;
        }
        std::string plain = stripLrcTimestamps(lyrics);
        // The stripper ends every line with "\n", including the last:
        // drop the trailing blank lines so the article does not end
        // on an empty line.
        while (!plain.empty() &&
               (plain.back() == '\n' || plain.back() == '\r'))
            plain.pop_back();
        // Truncated to 4000 chars + "\n..." (§13): truncUtf8 cuts
        // safely at a UTF-8 boundary and appends "…", which is
        // swapped for the inventory's literal suffix.
        std::string cut = ui_texts::truncUtf8(plain, 4000);
        if (cut.size() >= 3 && cut.substr(cut.size() - 3) == "…")
            cut = cut.substr(0, cut.size() - 3) + "\n...";
        std::string desc = artist + " — " + headUtf8(plain, 100);
        Json art = Json(Json::Array{inlineArticleResult(
            "l", ui_texts::inlineLyricsTitle(title), desc,
            artist + " — " + title + "\n\n" + cut, "")});
        std::string err;
        tg_.answerInlineQuery(iq.id, art, 0, "", "", &err);
        return;
    }
    if (mode == "s") {
        // Share message for the current track (§13): "{artist} —
        // {title}\n{share url}". The track's own URL is the share URL
        // available here — minting a share.create link would name the
        // track, not a sender's share flow, so it is not used.
        const Json& track = results->asArray()[0];
        std::string title = track.find("title")
                                ? track.find("title")->asString()
                                : "";
        if (title.empty()) {
            answer_empty(0);
            return;
        }
        std::string artist = track.find("artist")
                                 ? track.find("artist")->asString()
                                 : "";
        std::string url =
            track.find("url") ? track.find("url")->asString() : "";
        if (url.empty() && track.find("stream_url"))
            url = track.find("stream_url")->asString();
        std::string thumb = track.find("thumbnail")
                                ? track.find("thumbnail")->asString()
                                : "";
        std::string msg = artist + " — " + title;
        if (!url.empty()) msg += "\n" + url;
        Json art = Json(
            Json::Array{inlineArticleResult("s", title, artist, msg, thumb)});
        std::string err;
        tg_.answerInlineQuery(iq.id, art, 0, "", "", &err);
        return;
    }
    if (mode == "np") {
        const Json& track = results->asArray()[0];
        std::string id =
            track.find("id") ? track.find("id")->asString() : "";
        if (id.empty()) {
            answer_empty(0);
            return;
        }
        Json res = Json(Json::Array{inlineTrackResult(track, id, false)});
        std::string err;
        tg_.answerInlineQuery(iq.id, res, 0, "", "", &err);
        return;
    }
    // Search results (lib/del reach here only if a future library
    // backend returns tracks): one result per track, audio when a
    // stream_url is known, article otherwise.
    Json::Array out;
    for (const auto& t : results->asArray()) {
        std::string id = t.find("id") ? t.find("id")->asString() : "";
        std::string title =
            t.find("title") ? t.find("title")->asString() : "";
        if (id.empty() || title.empty()) continue;
        out.push_back(inlineTrackResult(t, id, true));
    }
    if (out.empty()) {
        Json art = Json(Json::Array{inlineArticleResult(
            "noresults", ui_texts::kInlineNoResults, "",
            ui_texts::kInlineNoResults, "")});
        std::string err;
        tg_.answerInlineQuery(iq.id, art, 3600, "", "", &err);
        return;
    }
    storeInlineResults(iq.from.id, *results);
    std::string err;
    tg_.answerInlineQuery(iq.id, Json(out), 3600, "", "", &err);
}

// One player verb for a chat, shared by the /menu panel and the card
// buttons. No replies here — the caller answers the toast and
// re-renders the card. Returns false + error when nothing is playing
// or the backend refuses.
bool Dispatcher::playerAction(int64_t chat_id, const std::string& verb,
                              int64_t user_id, const TgCallbackQuery* cq,
                              std::string& error) {
    CommandContext ctx;
    ctx.message.chat.id = chat_id;
    ctx.message.from.id = user_id;
    if (cq) ctx.message.from = cq->from;
    ctx.command = "player";
    Json np, track;
    auto need_now = [&]() {
        return nowPlaying(chat_id, np, track);
    };
    if (verb == "pause") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        IpcResult r = vcCall(ctx, "pause",
                             Json::objectOf({{"chat_id", Json(chat_id)}}));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        // The backend's pause is the pause (it owns the audio).
        return true;
    }
    if (verb == "resume") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        IpcResult r = vcCall(ctx, "resume",
                             Json::objectOf({{"chat_id", Json(chat_id)}}));
        if (!r.ok) {
            error = ui_texts::kToastResumeFailed;
            return false;
        }
        // The backend's resume is the resume (it owns the audio).
        return true;
    }
    if (verb == "replay") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        IpcResult r = vcCall(ctx, "seek",
                             Json::objectOf({
                                 {"chat_id", Json(chat_id)},
                                 {"position_sec", Json(static_cast<int64_t>(0))},
                             }));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        // The backend's seek is the seek (it owns the audio).
        return true;
    }
    if (verb == "skip") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        IpcResult r = vcCall(ctx, "skip",
                             Json::objectOf({{"chat_id", Json(chat_id)}}));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        Json np2, track2;
        if (!nowPlaying(chat_id, np2, track2)) {
            // Queue ran dry: the backend stopped, so the call is
            // inactive (onSkip behaviour).
            setCallActive(chat_id, false);
            error = ui_texts::kToastSkipEmpty;
            return false;
        }
        // The backend's skip is the skip (it owns the audio).
        return true;
    }
    if (verb == "stop") {
        IpcResult r = vcCall(ctx, "stop",
                             Json::objectOf({{"chat_id", Json(chat_id)}}));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        setCallActive(chat_id, false);
        setChatRtField(chat_id, "loop", "0");
        setChatRtField(chat_id, "speed", "1.0");
        setChatRtField(chat_id, "bass", "0");
        setChatRtField(chat_id, "muted", "0");
        return true;
    }
    if (verb == "mute" || verb == "unmute") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        std::string be;
        if (!muteVoice(chat_id, verb == "mute", be)) {
            error = be;
            return false;
        }
        return true;
    }
    if (verb == "seek15" || verb == "seekback15") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        int64_t pos = np.find("position_sec")
                          ? np.find("position_sec")->asInt()
                          : 0;
        int64_t dur = track.find("duration_sec")
                          ? track.find("duration_sec")->asInt()
                          : 0;
        int64_t next = verb == "seek15" ? pos + 15 : pos - 15;
        if (next < 0) next = 0;
        if (dur > 0 && next >= dur - 2) {
            error = ui_texts::toastSeekNearEnd(15);
            return false;
        }
        IpcResult r = vcCall(ctx, "seek",
                             Json::objectOf({
                                 {"chat_id", Json(chat_id)},
                                 {"position_sec", Json(next)},
                             }));
        if (!r.ok) {
            error = ui_texts::fmt(ui_texts::kToastSeekFailed,
                                  {{"error", r.error_message}});
            return false;
        }
        // The backend's seek is the seek (it owns the audio).
        return true;
    }
    if (verb == "autoplay") {
        bool on = !settingsFor(chat_id).autoplay;
        // The setting is written first (chat.settings autoplay also
        // writes through to the backend room); the vc.autoplay call
        // then confirms the engine side. Either failure rolls the
        // setting back, honestly — never a toast claiming success
        // over a refused toggle.
        if (!setSetting(chat_id, "autoplay", Json(on))) {
            error = ui_texts::kSettingsSaveFailed;
            return false;
        }
        IpcResult r = vcCall(ctx, "autoplay",
                             Json::objectOf({
                                 {"chat_id", Json(chat_id)},
                                 {"enabled", Json(on)},
                             }));
        if (!r.ok) {
            // The engine refused: roll the toggle back, honestly.
            setSetting(chat_id, "autoplay", Json(!on));
            error = r.error_message;
            return false;
        }
        error = on ? ui_texts::kToastAutoplayOn
                   : ui_texts::kToastAutoplayOff;
        return true;
    }
    if (verb == "thumb") {
        bool on = !settingsFor(chat_id).thumbnails;
        if (!setSetting(chat_id, "thumbnails", Json(on))) {
            error = ui_texts::kSettingsSaveFailed;
            return false;
        }
        error = on ? ui_texts::kToastThumbOn : ui_texts::kToastThumbOff;
        return true;
    }
    if (verb == "shuffle") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        IpcResult r = vcCall(ctx, "shuffle",
                             Json::objectOf({{"chat_id", Json(chat_id)}}));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        error = ui_texts::kToastShuffle;
        return true;
    }
    if (verb == "voldec" || verb == "volinc") {
        // Volume is NOT one of the 15 settings keys — the backend
        // room owns the live volume. The core keeps only the last
        // stepped value (runtime hash) so the +/- buttons can step
        // from it; the vc.volume call is the authority.
        int v = static_cast<int>(chatRtIntField(chat_id, "volume", 100));
        v += verb == "volinc" ? 10 : -10;
        if (v < 0) v = 0;
        if (v > 200) v = 200;
        IpcResult r = vcCall(ctx, "volume",
                             Json::objectOf({
                                 {"chat_id", Json(chat_id)},
                                 {"volume", Json(static_cast<int64_t>(v))},
                             }));
        if (!r.ok) {
            error = r.error_message;
            return false;
        }
        setChatRtField(chat_id, "volume", std::to_string(v));
        error = ui_texts::toastVolumeSet(v);
        return true;
    }
    if (verb == "refresh") {
        if (!need_now()) {
            error = ui_texts::kToastNothingPlaying;
            return false;
        }
        error = ui_texts::kToastPanelUpdated;
        return true;
    }
    error = ui_texts::kToastUnknownAction;
    return false;
}

// --- callback sub-handlers: help / start / close / player / queue ---------
void Dispatcher::cbHelp(const TgCallbackQuery& cq,
                        const std::vector<std::string>& parts) {
    std::string topic = parts.size() > 2 ? parts[2] : "";
    if (topic.empty() || topic == "main") {
        answerCb(cq, ui_texts::kHelpBackToast);
        editCbCard(cq, ui_texts::kHelpMenu, ui_texts::helpMenuKeyboard());
        return;
    }
    std::string text = ui_texts::helpTopicText(topic);
    if (text.empty()) {
        answerCb(cq, ui_texts::kHelpUnknownCategory, true);
        return;
    }
    answerCb(cq, ui_texts::helpTopicTitle(topic));
    editCbCard(cq, text, ui_texts::helpTopicKeyboard());
}

void Dispatcher::cbStart(const TgCallbackQuery& cq) {
    // Back from the help menu (or any card) to the start card.
    editCbCard(cq, ui_texts::kStartPm,
               ui_texts::startPmKeyboard(bot_username_, cfg_.updates_url,
                                         cfg_.support_url, cfg_.source_url));
    answerCb(cq, "");
}

void Dispatcher::cbClose(const TgCallbackQuery& cq) {
    if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
    answerCb(cq, "");
}

void Dispatcher::cbPlayer(const TgCallbackQuery& cq,
                          const std::vector<std::string>& parts) {
    // pl:player:<verb>:<chat_id>[:<token>]
    // pl:player:progress:<chat_id> — no-op label button.
    // pl:player:confirm:<verb>:<chat_id>
    // pl:player:playnow:<chat_id>:<token> / addlist:<chat_id>:<token>
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verb = parts[2];
    if (verb == "progress") {
        answerCb(cq, "");
        return;
    }
    int64_t chat_id = cq.chat_id;
    std::string token;
    if (verb == "confirm") {
        // pl:player:confirm:<verb>:<chat_id>
        if (parts.size() < 5) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        const std::string& cverb = parts[3];
        try {
            chat_id = std::stoll(parts[4]);
        } catch (...) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        if (!callbackCanControl(chat_id, cq.from.id)) {
            answerCb(cq, ui_texts::kToastNotAllowed, true);
            return;
        }
        if (cverb == "resume") {
            std::string err;
            if (!playerAction(chat_id, "resume", cq.from.id, &cq, err)) {
                answerCb(cq, err, true);
                return;
            }
            answerCb(cq, ui_texts::toastResumed(0));
            refreshCard(cq, chat_id);
            return;
        }
        if (cverb == "unmute") {
            std::string err;
            if (!playerAction(chat_id, "unmute", cq.from.id, &cq, err)) {
                answerCb(cq, err, true);
                return;
            }
            answerCb(cq, ui_texts::kToastUnmuted);
            refreshCard(cq, chat_id);
            return;
        }
        if (cverb == "stop") {
            std::string err;
            if (!playerAction(chat_id, "stop", cq.from.id, &cq, err)) {
                answerCb(cq, err, true);
                return;
            }
            answerCb(cq, ui_texts::kToastStopped);
            editCbCard(cq, ui_texts::kStoppedCard, InlineKeyboard{});
            return;
        }
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    // Plain verbs carry the chat id in parts[3]. Token verbs
    // (playnow/addlist) carry only the token — cbPlayerToken emits
    // "pl:player:<verb>:<token>" — and the chat comes from the
    // callback's own message.
    if (verb == "playnow" || verb == "addlist") {
        if (parts.size() < 4) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        token = parts[3];
        chat_id = cq.chat_id;
    } else if (parts.size() > 3) {
        try {
            chat_id = std::stoll(parts[3]);
        } catch (...) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
    }
    // The /menu control panel shares these callbacks (§4, remix
    // §2.3) but has its own rule: only the user who ran /menu may
    // press, and everyone else is silently ignored (empty answer,
    // no toast). The panel is the stored panel message for the chat.
    bool is_panel = false;
    if (cq.has_message) {
        auto pm = store_.get("panelmsg:" + std::to_string(chat_id));
        if (pm) {
            try {
                is_panel = std::stoll(*pm) == cq.message_id;
            } catch (...) {
            }
        }
    }
    if (is_panel) {
        int64_t panel_user = 0;
        auto pu = store_.get("paneluser:" + std::to_string(chat_id));
        if (pu) {
            try {
                panel_user = std::stoll(*pu);
            } catch (...) {
            }
        }
        if (panel_user != 0 && cq.from.id != panel_user) {
            answerCb(cq, "");
            return;
        }
    } else if (!callbackCanControl(chat_id, cq.from.id)) {
        answerCb(cq, ui_texts::kToastNotAllowed, true);
        return;
    }
    if (verb == "playnow") {
        Json item;
        int64_t pos = 0;
        if (!resolveQueueToken(chat_id, token, item, pos)) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        const Json* t = item.find("track");
        if (!t) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        std::string url = t->find("url") ? t->find("url")->asString() : "";
        std::string id = t->find("id") ? t->find("id")->asString() : "";
        std::string q = !url.empty() ? url : id;
        if (q.empty()) {
            answerCb(cq, ui_texts::kToastInvalidRequest, true);
            return;
        }
        CommandContext ctx;
        ctx.message.chat.id = chat_id;
        ctx.message.from = cq.from;
        ctx.command = "play";
        ctx.args = q;
        // Play-now interrupts: stop the current track first, then
        // play the chosen one (onPlay attaches the audio).
        vcCall(ctx, "stop",
               Json::objectOf({{"chat_id", Json(chat_id)}}));
        onPlay(ctx);
        answerCb(cq, ui_texts::kToastSkipped);
        refreshCard(cq, chat_id);
        return;
    }
    if (verb == "addlist") {
        Json item;
        int64_t pos = 0;
        if (!resolveQueueToken(chat_id, token, item, pos)) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        const Json* t = item.find("track");
        if (!t) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        Json list = playlistOf(store_, cq.from.id);
        std::string id = t->find("id") ? t->find("id")->asString() : "";
        for (const auto& it : list.asArray()) {
            const Json* iid = it.find("id");
            if (iid && iid->asString() == id) {
                answerCb(cq, ui_texts::kToastAlreadyInPlaylist);
                return;
            }
        }
        if (list.size() >= kPlaylistMax) {
            answerCb(cq, ui_texts::kToastPlaylistFull, true);
            return;
        }
        Json::Array arr = list.asArray();
        arr.push_back(*t);
        savePlaylist(store_, cq.from.id, Json(arr));
        answerCb(cq, ui_texts::kToastAddedToPlaylist);
        return;
    }
    if (verb == "stop") {
        // Stop asks first — but only when there is something to ask
        // about: paused (resume?) or muted (unmute?). Playing
        // normally, the stop goes through at once.
        Json np, track;
        bool paused = false;
        bool muted = false;
        if (nowPlaying(chat_id, np, track)) {
            if (const Json* p = np.find("paused")) paused = p->asBool();
            if (const Json* p = np.find("is_paused")) paused = p->asBool();
            if (const Json* m = np.find("is_muted")) muted = m->asBool();
            if (chatRtField(chat_id, "muted", "0") == "1") muted = true;
        }
        if (!paused && !muted) {
            std::string err;
            if (!playerAction(chat_id, "stop", cq.from.id, &cq, err)) {
                answerCb(cq, err, true);
                return;
            }
            answerCb(cq, ui_texts::kToastStopped);
            editCbCard(cq, ui_texts::kStoppedCard, InlineKeyboard{});
            return;
        }
        answerCb(cq, "");
        if (paused)
            editCbCard(cq, ui_texts::kStopConfirmPaused,
                       ui_texts::stopConfirmKeyboard(chat_id, true));
        else
            editCbCard(cq, ui_texts::kStopConfirmMuted,
                       ui_texts::stopConfirmKeyboard(chat_id, false));
        return;
    }
    // Plain control verbs.
    Json np, track;
    int64_t pos = 0;
    if (nowPlaying(chat_id, np, track)) {
        pos = np.find("position_sec") ? np.find("position_sec")->asInt()
                                      : 0;
    }
    std::string err;
    if (!playerAction(chat_id, verb, cq.from.id, &cq, err)) {
        answerCb(cq, err, true);
        if (err == ui_texts::kToastSkipEmpty) {
            if (is_panel)
                refreshPanel(cq, chat_id);
            else
                editCbCard(cq, ui_texts::kStoppedCard, InlineKeyboard{});
        }
        return;
    }
    // Success toast per verb (playerAction puts the toggle/vol toast
    // in err; the rest have their own).
    if (verb == "pause") answerCb(cq, ui_texts::toastPaused(pos));
    else if (verb == "resume") answerCb(cq, ui_texts::toastResumed(pos));
    else if (verb == "replay")
        answerCb(cq, ui_texts::fmt(ui_texts::kToastReplayedBy,
                                   {{"user", displayName(cq.from)}}));
    else if (verb == "skip")
        answerCb(cq, ui_texts::fmt(ui_texts::kToastSkippedBy,
                                   {{"user", displayName(cq.from)}}));
    else if (verb == "mute") answerCb(cq, ui_texts::kToastMuted);
    else if (verb == "unmute") answerCb(cq, ui_texts::kToastUnmuted);
    else if (verb == "seek15") answerCb(cq, ui_texts::toastSeekFwd(15));
    else if (verb == "seekback15") answerCb(cq, ui_texts::toastSeekBack(15));
    else answerCb(cq, err);  // autoplay/thumb/shuffle/vol/refresh
    if (is_panel)
        refreshPanel(cq, chat_id);
    else
        refreshCard(cq, chat_id);
}

void Dispatcher::cbQueue(const TgCallbackQuery& cq,
                         const std::vector<std::string>& parts) {
    // pl:queue:more:<chat_id> / pl:queue:del:<chat_id>:<token>
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verb = parts[2];
    int64_t chat_id = 0;
    try {
        chat_id = std::stoll(parts[3]);
    } catch (...) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (!callbackCanControl(chat_id, cq.from.id)) {
        answerCb(cq, ui_texts::kToastNotAllowed, true);
        return;
    }
    if (verb == "more") {
        std::string text;
        InlineKeyboard kb;
        std::string thumb;
        if (!buildQueueCard(chat_id, true, text, kb, thumb)) {
            answerCb(cq, ui_texts::kToastNothingPlaying, true);
            return;
        }
        answerCb(cq, "");
        // The card kind decides the edit method (photo caption vs
        // text); editCbCard reads the recorded card info.
        editCbCard(cq, text, kb);
        return;
    }
    if (verb == "del") {
        if (parts.size() < 5) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        Json item;
        int64_t pos = 0;
        if (!resolveQueueToken(chat_id, parts[4], item, pos)) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        CommandContext ctx;
        ctx.message.chat.id = chat_id;
        ctx.message.from = cq.from;
        ctx.command = "queue";
        IpcResult r = vcCall(ctx, "queue",
                             Json::objectOf({
                                 {"chat_id", Json(chat_id)},
                                 {"action", Json("remove")},
                                 {"position", Json(pos)},
                             }));
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        answerCb(cq, ui_texts::kToastQueueRemoved);
        std::string text;
        InlineKeyboard kb;
        std::string thumb;
        if (buildQueueCard(chat_id, false, text, kb, thumb))
            editCbCard(cq, text, kb);
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbSet(const TgCallbackQuery& cq,
                       const std::vector<std::string>& parts) {
    // pl:set:<key> / pl:set:info:<key> / pl:set:timer:<n> / pl:set:main
    // Settings are admin-only (sudo or chat admin) and group-only.
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (cq.chat_id > 0) {
        answerCb(cq, ui_texts::kSettingsGroupOnly, true);
        return;
    }
    auto panel_text = [&]() {
        std::string title;
        auto t = store_.get("chat_title:" + std::to_string(cq.chat_id));
        title = t ? *t : std::to_string(cq.chat_id);
        std::string text = ui_texts::fmt(ui_texts::kSettingsText,
                                         {{"title", ui_texts::htmlEscape(title)},
                                          {"id", std::to_string(cq.chat_id)}});
        if (!settingsLive(cq.chat_id)) text += kSettingsDegradedSuffix;
        return text;
    };
    if (parts[2] == "main") {
        answerCb(cq, "");
        editCbCard(cq, panel_text(),
                   ui_texts::settingsKeyboard(chatSettings(cq.chat_id)));
        return;
    }
    if (parts[2] == "info") {
        if (parts.size() < 4) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        answerCb(cq, ui_texts::settingInfoText(parts[3]), true);
        return;
    }
    if (!isSudo(cq.from.id) && !isChatAdmin(cq.chat_id, cq.from.id)) {
        answerCb(cq, ui_texts::kDenyAdminCb, true);
        return;
    }
    ui_texts::ChatSettings s = chatSettings(cq.chat_id);
    ui_texts::ChatSettings before = s;  // pre-toggle snapshot: a failed
    // save re-renders the panel from this, never from the toggled copy.
    const std::string& key = parts[2];
    // Pagination / chat-music-download sub-screens (§9, IPC-6 keys):
    // bare tap shows the screen, the toggle keyboard sends :0/:1.
    if (key == "page" || key == "musicdl") {
        bool is_page = key == "page";
        if (parts.size() > 3) {
            if (parts[3] != "0" && parts[3] != "1") {
                answerCb(cq, ui_texts::kToastUnknownAction, true);
                return;
            }
            if (is_page)
                s.paginate_purge_old = parts[3] == "1";
            else
                s.chat_music_download = parts[3] == "1";
            if (!saveChatSettings(cq.chat_id, s)) {
                s = before;
                answerCb(cq, ui_texts::kSettingsSaveFailed, true);
            } else {
                answerCb(cq, ui_texts::kSettingsUpdated);
            }
        } else {
            answerCb(cq, "");
        }
        editCbCard(cq,
                   is_page ? ui_texts::kSettingsPageScreen
                           : ui_texts::kSettingsMusicDlScreen,
                   ui_texts::settingsToggleKeyboard(
                       key, is_page ? s.paginate_purge_old
                                    : s.chat_music_download));
        return;
    }
    // Quality sub-screen (routing rows A96–A99): pl:set:quality shows
    // the screen; :br:<kbps> / :fr:<ms> write one per-chat override
    // via the generic chat.settings set path (quality_bitrate_bps /
    // quality_frame_ms); :reset clears all six quality_* overrides
    // (JSON null) so the chat falls back to parlay.toml / profile /
    // default. Out-of-range values are Unknown action, never stored.
    // Every change applies on the NEXT join (the card says so); the
    // screen is re-rendered from a fresh quality.get after the write.
    if (key == "quality") {
        if (parts.size() == 3) {
            answerCb(cq, "");
            renderQualityScreen(cq);
            return;
        }
        if (parts.size() == 4 && parts[3] == "reset") {
            static const char* kQualityKeys[] = {
                "quality_bitrate_bps", "quality_frame_ms",
                "quality_complexity", "quality_vbr", "quality_fec",
                "quality_dtx"};
            bool ok = true;
            for (const char* k : kQualityKeys)
                ok = setSetting(cq.chat_id, k, Json(nullptr)) && ok;
            answerCb(cq, ok ? ui_texts::kSettingsUpdated
                            : ui_texts::kSettingsSaveFailed,
                     !ok);
            renderQualityScreen(cq);
            return;
        }
        if (parts.size() == 5 &&
            (parts[3] == "br" || parts[3] == "fr")) {
            bool is_br = parts[3] == "br";
            int v = -1;
            try {
                v = std::stoi(parts[4]);
            } catch (...) {
            }
            bool valid =
                is_br ? (v == 128 || v == 160 || v == 192 || v == 256 ||
                         v == 320)
                      : (v == 10 || v == 20 || v == 40 || v == 60);
            if (!valid) {
                answerCb(cq, ui_texts::kToastUnknownAction, true);
                return;
            }
            bool ok = setSetting(
                cq.chat_id,
                is_br ? "quality_bitrate_bps" : "quality_frame_ms",
                Json(static_cast<int64_t>(is_br ? v * 1000 : v)));
            answerCb(cq, ok ? ui_texts::kSettingsUpdated
                            : ui_texts::kSettingsSaveFailed,
                     !ok);
            renderQualityScreen(cq);
            return;
        }
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (key == "timer") {
        if (parts.size() < 4) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        try {
            int n = std::stoi(parts[3]);
            if (n < 2 || n > 7) throw std::out_of_range("timer");
            s.auto_delete_min = n;
        } catch (...) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
    } else if (key == "adminmode") {
        s.admin_mode = (s.admin_mode + 1) % 3;
    } else if (key == "playmode") {
        s.play_mode = s.play_mode == "admins" ? "everyone" : "admins";
    } else if (key == "cmddelete") {
        s.cmd_delete = !s.cmd_delete;
    } else if (key == "cleanmode") {
        s.clean_delay_min = s.clean_delay_min == 0 ? 15 : 0;
    } else if (key == "cleanduration") {
        s.clean_delay_min = s.clean_delay_min == 15   ? 30
                            : s.clean_delay_min == 30 ? 60
                            : s.clean_delay_min == 60 ? 5
                                                      : 15;
    } else if (key == "nothumb") {
        s.thumbnails = !s.thumbnails;
    } else if (key == "autoplay") {
        s.autoplay = !s.autoplay;
    } else if (key == "silent") {
        s.silent = !s.silent;
    } else if (key == "autodelete") {
        s.auto_delete = !s.auto_delete;
    } else {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (!saveChatSettings(cq.chat_id, s)) {
        answerCb(cq, ui_texts::kSettingsSaveFailed, true);
        editCbCard(cq, panel_text(),
                   ui_texts::settingsKeyboard(before));
        return;
    }
    answerCb(cq, ui_texts::kSettingsUpdated);
    editCbCard(cq, panel_text(), ui_texts::settingsKeyboard(s));
}

void Dispatcher::cbLang(const TgCallbackQuery& cq,
                        const std::vector<std::string>& parts) {
    // pl:lang:select[:<origin>] / pl:lang:<code>[:<origin>]
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    std::string origin = parts.size() > 3 ? parts[3] : "start";
    if (parts[2] == "select") {
        answerCb(cq, "");
        editCbCard(cq, ui_texts::kLangPicker,
                   ui_texts::langPickerKeyboard(origin));
        return;
    }
    if (parts[2] != "en") {
        answerCb(cq, ui_texts::kToastFeatureUnavailable, true);
        return;
    }
    // Parlay is English-only; the picker offers English alone.
    if (settingsFor(cq.chat_id).language == "en") {
        answerCb(cq, ui_texts::kLangAlready);
    } else if (setSetting(cq.chat_id, "language", Json("en"))) {
        answerCb(cq, ui_texts::langUpdatedText("English"));
    } else {
        answerCb(cq, ui_texts::kSettingsSaveFailed, true);
    }
    // Return to the menu the picker came from.
    if (origin == "help") {
        editCbCard(cq, ui_texts::kHelpMenu, ui_texts::helpMenuKeyboard());
    } else if (origin == "settings") {
        std::string title;
        auto t = store_.get("chat_title:" + std::to_string(cq.chat_id));
        title = t ? *t : std::to_string(cq.chat_id);
        editCbCard(cq, ui_texts::fmt(ui_texts::kSettingsText,
                                     {{"title", ui_texts::htmlEscape(title)},
                                      {"id", std::to_string(cq.chat_id)}}),
                   ui_texts::settingsKeyboard(chatSettings(cq.chat_id)));
    } else {
        editCbCard(cq, ui_texts::kStartPm,
                   ui_texts::startPmKeyboard(bot_username_, cfg_.updates_url,
                                             cfg_.support_url,
                                             cfg_.source_url));
    }
}

void Dispatcher::cbDl(const TgCallbackQuery& cq,
                      const std::vector<std::string>& parts) {
    // pl:dl:cancel:<run_id> / pl:dl:fx:<run_id>:<effect>
    // pl:dl:zip:<run_id> / pl:dl:1by1:<run_id> (Spotify picker, §8)
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verb = parts[2];
    const std::string& run_id = parts[3];
    if (verb == "go") {
        // pl:dl:go:<run_id> — the "Download N songs" button on a
        // collection card: start the bulk download stored by the
        // zip/1by1 pick (value: url\nchat_id\nmode).
        auto go = store_.get("dlgo:" + run_id);
        if (!go) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        std::string url = go->substr(0, go->find('\n'));
        std::string rest = go->substr(go->find('\n') + 1);
        int64_t chat_id = cq.chat_id;
        std::string mode = "files";
        size_t nl = rest.find('\n');
        try {
            chat_id = std::stoll(nl == std::string::npos
                                     ? rest
                                     : rest.substr(0, nl));
        } catch (...) {
        }
        if (nl != std::string::npos) mode = rest.substr(nl + 1);
        if (chat_id != cq.chat_id) {
            answerCb(cq, ui_texts::kToastDlWrongChat, true);
            return;
        }
        store_.del("dlgo:" + run_id);
        answerCb(cq, ui_texts::kDlFetching);
        deliverSpotifyCollection(cq, url, mode);
        return;
    }
    if (verb == "retry") {
        // pl:dl:retry:<run_id> — the "Check Again" button on a
        // track-not-available card: re-run the stored download
        // (value: action\nquery\nchat_id\nrequested_by).
        auto saved = store_.get("dlretry:" + run_id);
        if (!saved) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        std::vector<std::string> lines;
        {
            std::string rest = *saved;
            while (true) {
                size_t e = rest.find('\n');
                if (e == std::string::npos) {
                    lines.push_back(rest);
                    break;
                }
                lines.push_back(rest.substr(0, e));
                rest = rest.substr(e + 1);
            }
        }
        if (lines.size() < 4) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        std::string action = lines[0];
        std::string query = lines[1];
        int64_t chat_id = 0;
        int64_t requested_by = 0;
        try {
            chat_id = std::stoll(lines[2]);
            requested_by = std::stoll(lines[3]);
        } catch (...) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        if (chat_id != cq.chat_id) {
            answerCb(cq, ui_texts::kToastDlWrongChat, true);
            return;
        }
        bool url_action = action == "download/spotify" ||
                          action == "download/tiktok" ||
                          action == "download/navidrome";
        Json body = url_action
                        ? Json::objectOf({
                              {"url", Json(query)},
                              {"requested_by", Json(requested_by)},
                          })
                        : Json::objectOf({
                              {"query", Json(query)},
                              {"requested_by", Json(requested_by)},
                          });
        IpcResult r = ipc_.post(action, body, 120);
        if (!r.ok) {
            if (isNotAvailableYet(r.error_message)) {
                // Still unavailable: non-alert toast, the card and
                // its Check Again button stay for the next tap.
                answerCb(cq, ui_texts::kToastDlStillProcessing);
                return;
            }
            answerCb(cq, r.error_message, true);
            editCbCard(cq, ui_texts::dlFailedText(r.error_message),
                       InlineKeyboard{});
            return;
        }
        const Json* oversize = r.data.find("oversize");
        if (oversize && oversize->asBool()) {
            answerCb(cq, "That file is over the 50 MB send cap — not "
                         "sent. There is no large-file send yet.",
                     true);
            editCbCard(cq, "That file is over the 50 MB send cap — not "
                           "sent. There is no large-file send yet.",
                       InlineKeyboard{});
            return;
        }
        const Json* track = r.data.find("track");
        const Json* path = r.data.find("file_path");
        std::string file_path = path ? path->asString() : "";
        std::string title =
            track && track->find("title") ? track->find("title")->asString()
                                          : query;
        std::string artist =
            track && track->find("artist")
                ? track->find("artist")->asString()
                : "";
        if (!file_path.empty() && std::filesystem::exists(file_path)) {
            std::string err;
            if (tg_.sendAudioFile(cq.chat_id, file_path,
                                  audioCaption(track, file_path, title,
                                               artist, bot_username_),
                                  &err) < 0) {
                answerCb(cq, err, true);
                editCbCard(cq, ui_texts::dlFailedText(err),
                           InlineKeyboard{});
                return;
            }
        }
        // Success replaces the failure card.
        store_.del("dlretry:" + run_id);
        answerCb(cq, ui_texts::kDlSuccess);
        std::string text = ui_texts::kDlSuccess;
        if (track) text += " " + fmtTrack(*track);
        editCbCard(cq, text, InlineKeyboard{});
        return;
    }
    if (verb == "zip" || verb == "1by1") {
        auto pick = store_.get("dlpick:" + run_id);
        if (!pick) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        std::string url = pick->substr(0, pick->find('\n'));
        int64_t chat_id = cq.chat_id;
        size_t nl = pick->find('\n');
        if (nl != std::string::npos) {
            try {
                chat_id = std::stoll(pick->substr(nl + 1));
            } catch (...) {
            }
        }
        if (chat_id != cq.chat_id) {
            answerCb(cq, ui_texts::kToastDlWrongChat, true);
            return;
        }
        store_.del("dlpick:" + run_id);
        answerCb(cq, ui_texts::kDlFetching);
        std::string mode = verb == "zip" ? "zip" : "files";
        // Collection card first (lemonic albumMessage /
        // playlistMessage): spotify.collection gives the name, kind
        // and track count; the card carries the cover and one
        // "Download N songs" button that starts the bulk download.
        // When the metadata call fails (e.g. no Spotify
        // credentials) deliver directly, exactly as before.
        IpcResult col = ipc_.post(
            "spotify.collection",
            Json::objectOf({{"url", Json(url)},
                            {"requested_by", Json(cq.from.id)}}),
            30);
        int total = 0;
        std::string kind;
        std::string name;
        std::string cover;
        if (col.ok) {
            if (const Json* t = col.data.find("total"))
                total = static_cast<int>(t->asInt());
            if (const Json* tr = col.data.find("tracks")) {
                if (total <= 0)
                    total = static_cast<int>(tr->size());
                if (!tr->asArray().empty()) {
                    const Json& first = tr->asArray()[0];
                    if (const Json* th = first.find("thumbnail"))
                        cover = th->asString();
                }
            }
            if (const Json* k = col.data.find("kind"))
                kind = k->asString();
            if (const Json* n = col.data.find("name"))
                name = n->asString();
        }
        if (col.ok && total > 0) {
            if (kind.empty())
                kind = url.find("/album/") != std::string::npos ||
                               url.find(":album:") != std::string::npos
                           ? "album"
                           : "playlist";
            if (name.empty()) name = "Spotify " + kind;
            store_.set("dlgo:" + run_id,
                       url + "\n" + std::to_string(chat_id) + "\n" + mode);
            std::string text =
                ui_texts::collectionCardText(kind, name, total);
            InlineKeyboard kb =
                ui_texts::dlCollectionCardKeyboard(run_id, total);
            std::string photo = effectiveThumb(chat_id, cover);
            // Retire the picker message, then send the collection
            // card (photo when a cover or a configured default
            // exists, text otherwise).
            editCbCard(cq, "Fetching collection...", InlineKeyboard{});
            std::string err;
            if (!photo.empty()) {
                if (tg_.sendPhoto(chat_id, photo, text, kb, &err) < 0)
                    tg_.sendMessageHtml(chat_id, text, kb, 0, &err);
            } else {
                tg_.sendMessageHtml(chat_id, text, kb, 0, &err);
            }
            return;
        }
        deliverSpotifyCollection(cq, url, mode);
        return;
    }
    auto run = store_.get("dlrun:" + run_id);
    if (verb == "cancel") {
        if (!run) {
            answerCb(cq, ui_texts::kToastDlNothingToCancel);
            return;
        }
        // Only the chat that requested the download may cancel it
        // (same guard as the zip/1by1 branch below). The run value
        // is "<path|pending>\n<chat_id>"; a value with no parsable
        // chat does not block the cancel.
        size_t nl = run->find('\n');
        if (nl != std::string::npos) {
            try {
                if (std::stoll(run->substr(nl + 1)) != cq.chat_id) {
                    answerCb(cq, ui_texts::kToastDlWrongChat, true);
                    return;
                }
            } catch (...) {
            }
        }
        // download.cancel (Phase 3, OD-3): {run_id} ->
        // {cancelled, run_id, phase}. Unknown run_id is a 404; a
        // finished run answers cancelled:false (200, not an error).
        IpcResult r = ipc_.post("download.cancel",
                                Json::objectOf({{"run_id", Json(run_id)}}),
                                30);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        store_.del("dlrun:" + run_id);
        bool cancelled = true;
        if (const Json* c = r.data.find("cancelled"))
            cancelled = c->asBool();
        if (cancelled) {
            answerCb(cq, ui_texts::kToastDlCancelled);
            // Retire the progress message the button sits on so the
            // dead Cancel button does not linger.
            editCbCard(cq, ui_texts::kToastDlCancelled, InlineKeyboard{});
        } else {
            answerCb(cq, ui_texts::kToastDlNothingToCancel);
        }
        return;
    }
    if (verb == "fx") {
        if (parts.size() < 5) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        if (!run) {
            answerCb(cq, ui_texts::kToastDlNothingToCancel);
            return;
        }
        std::string effect = parts[4];
        std::string fx_effect;
        if (effect == "orig") fx_effect = "original";
        else if (effect == "slow") fx_effect = "slow-reverb";
        else if (effect == "nightcore") fx_effect = "nightcore";
        else if (effect == "bass") fx_effect = "bass-boost";
        else if (effect == "8d") fx_effect = "8d";
        else {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        // dlrun value: "<file_path>\n<chat_id>".
        std::string input_path = run->substr(0, run->find('\n'));
        answerCb(cq, ui_texts::kDlFxApplying);
        IpcResult r = ipc_.post("fx",
                                Json::objectOf({
                                    {"effect", Json(fx_effect)},
                                    {"input_path", Json(input_path)},
                                    {"requested_by", Json(cq.from.id)},
                                }),
                                120);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        const Json* out = r.data.find("output_path");
        std::string out_path = out ? out->asString() : "";
        if (out_path.empty()) {
            answerCb(cq, ui_texts::kToastFeatureUnavailable, true);
            return;
        }
        // §7.8 / §10-D16 (Phase 5: the DSP is live). fx.apply
        // answers one of three ways, and the file at output_path
        // is sent ONLY in the third:
        //   - live_applied:true — the effect was applied live to
        //     the voice-chat stream (applied:true). No file was
        //     written: output_path is the unprocessed input and
        //     must never be sent as processed audio.
        //   - applied:false — no live stream to apply to; the
        //     effect is recorded for the next track. The file is
        //     unprocessed: say so honestly instead of sending it.
        //   - applied:true, live_applied absent/false — `original`
        //     only (a no-op by definition): the input file IS the
        //     output, so sending it is correct.
        // `applied` absent is treated as false — only an explicit
        // true means the effect is in force somewhere.
        bool applied = false;
        if (const Json* a = r.data.find("applied")) applied = a->asBool();
        bool live_applied = false;
        if (const Json* l = r.data.find("live_applied")) live_applied = l->asBool();
        if (live_applied) {
            answerCb(cq, fx_effect + " applied live to the voice chat.",
                     true);
            return;
        }
        if (!applied) {
            answerCb(cq, fx_effect +
                             " recorded for the next track. The file "
                             "is unprocessed, so nothing was sent.",
                     true);
            return;
        }
        std::string err;
        if (tg_.sendAudioFile(cq.chat_id, out_path, "", &err) < 0)
            answerCb(cq, err, true);
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbBcast(const TgCallbackQuery& cq) {
    // pl:bcast:cancel — owner-only; sets the flag the fan-out loop
    // checks between targets.
    if (!isOwner(cq.from.id)) {
        answerCb(cq, ui_texts::kDenyOwner, true);
        return;
    }
    if (!bcast_running_.load()) {
        answerCb(cq, ui_texts::kBcastCancelled);
        return;
    }
    bcast_cancel_.store(true);
    answerCb(cq, ui_texts::kBcastCancelOk);
}

void Dispatcher::cbAuth(const TgCallbackQuery& cq,
                        const std::vector<std::string>& parts) {
    // pl:auth:del:<chat_id>:<user_id> — revoke one authorized user
    // from the /authlist card. Admin-only.
    if (parts.size() < 5 || parts[2] != "del") {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    int64_t chat_id = 0;
    int64_t user_id = 0;
    try {
        chat_id = std::stoll(parts[3]);
        user_id = std::stoll(parts[4]);
    } catch (...) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (!isSudo(cq.from.id) && !isChatAdmin(chat_id, cq.from.id)) {
        answerCb(cq, ui_texts::kDenyAdminCb, true);
        return;
    }
    store_.srem("auth:" + std::to_string(chat_id),
                std::to_string(user_id));
    answerCb(cq, ui_texts::kToastAuthRevoked);
    // Re-render the auth list in place. Names resolve best-effort
    // (getUserMention falls back to the bare id); the limit shown is
    // kAuthMaxUsers (25, the ArcMusic/TinaMusic/YukkiMusic-Go
    // MAX_AUTH_USERS default).
    auto members = store_.smembers("auth:" + std::to_string(chat_id));
    std::vector<std::pair<std::string, int64_t>> users;
    for (const auto& m : members) {
        try {
            int64_t id = std::stoll(m);
            users.emplace_back(tg_.getUserMention(id), id);
        } catch (...) {
        }
    }
    editCbCard(cq, ui_texts::authlistText(
                       users, static_cast<int>(kAuthMaxUsers)),
               ui_texts::authlistKeyboard(chat_id, users));
}

void Dispatcher::cbSearch(const TgCallbackQuery& cq,
                          const std::vector<std::string>& parts) {
    // pl:search:<n>:<req_user_id>:<track_id> — pick result n.
    // pl:search:page:<req_user_id>:<page>:<hash> — next/previous page.
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (parts[2] == "page") {
        if (parts.size() < 6) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        int64_t req_user = 0;
        int page = 0;
        try {
            req_user = std::stoll(parts[3]);
            page = std::stoi(parts[4]);
        } catch (...) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        if (cq.from.id != req_user) {
            answerCb(cq, ui_texts::kToastNotAllowed, true);
            return;
        }
        std::string query;
        Json results;
        if (!loadSearchSession(req_user, query, results) ||
            ui_texts::queryHash(query) != parts[5]) {
            answerCb(cq, ui_texts::kToastExpired, true);
            return;
        }
        int total_pages =
            static_cast<int>((results.size() + 9) / 10);
        if (page < 1 || page > total_pages) {
            answerCb(cq, ui_texts::kSearchNoMore, true);
            return;
        }
        answerCb(cq, "");
        editCbCard(cq, searchResultsText(query, results, page),
                   searchResultsKeyboard(req_user, query, results, page));
        return;
    }
    // Pick: pl:search:<n>:<req_user_id>:<track_id>
    if (parts.size() < 5) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    int n = 0;
    int64_t req_user = 0;
    try {
        n = std::stoi(parts[2]);
        req_user = std::stoll(parts[3]);
    } catch (...) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (cq.from.id != req_user) {
        answerCb(cq, ui_texts::kToastNotAllowed, true);
        return;
    }
    std::string query;
    Json results;
    if (!loadSearchSession(req_user, query, results)) {
        answerCb(cq, ui_texts::kToastExpired, true);
        editCbCard(cq, ui_texts::kSearchExpired, InlineKeyboard{});
        return;
    }
    if (n < 1 || n > static_cast<int>(results.size())) {
        answerCb(cq, ui_texts::kToastInvalidRequest, true);
        return;
    }
    const Json& t = results.asArray()[static_cast<size_t>(n) - 1];
    std::string id = t.find("id") ? t.find("id")->asString() : "";
    // The button carries the queue token, not the raw id (§1).
    if (!parts[4].empty() && ui_texts::trackToken(id) != parts[4]) {
        answerCb(cq, ui_texts::kToastExpired, true);
        editCbCard(cq, ui_texts::kSearchExpired, InlineKeyboard{});
        return;
    }
    std::string err;
    if (!playSearchPick(cq, req_user, n, err)) {
        answerCb(cq,
                 err == "expired" ? ui_texts::kToastExpired
                                  : ui_texts::kToastInvalidRequest,
                 true);
        if (err == "expired")
            editCbCard(cq, ui_texts::kSearchExpired, InlineKeyboard{});
        return;
    }
    answerCb(cq, ui_texts::kSearchSuccessToast);
}

void Dispatcher::cbVote(const TgCallbackQuery& cq,
                        const std::vector<std::string>& parts) {
    // pl:vote:<approve|decline>:<track_id> (daslui §23). Only
    // callbacks from the voting chat are handled. Every path
    // answers the callback query (§7.5: never leave a callback
    // un-answered): a counted vote gets a confirmation toast after
    // the card edit, an unknown vote gets the "Unknown action."
    // alert, a foreign-chat vote gets the "You aren't allowed"
    // alert, and a backend failure gets an alert with the failure.
    // The track id is rejoined from parts[3:] — ids are opaque and
    // a ':' inside one must not truncate it.
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verdict = parts[2];
    std::string track_id = parts[3];
    for (size_t i = 4; i < parts.size(); ++i) track_id += ":" + parts[i];
    if (verdict != "approve" && verdict != "decline") {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    auto chat = store_.get("vote:" + track_id);
    if (!chat) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;  // no vote open for this track
    }
    int64_t vote_chat = 0;
    try {
        vote_chat = std::stoll(*chat);
    } catch (...) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (vote_chat != cq.chat_id) {
        answerCb(cq, ui_texts::kToastNotAllowed, true);
        return;
    }
    if (verdict == "decline") {
        editCbCard(cq, ui_texts::voteDeclinedText(displayName(cq.from), ""),
                   InlineKeyboard{});
        store_.del("vote:" + track_id);
        answerCb(cq, ui_texts::kToastVoteRecorded);
        return;
    }
    CommandContext ctx;
    ctx.message.chat.id = vote_chat;
    ctx.message.from = cq.from;
    ctx.command = "vote";
    IpcResult r = ipc_.post("vote",
                            Json::objectOf({
                                {"chat_id", Json(vote_chat)},
                                {"track_id", Json(track_id)},
                                {"user_id", Json(cq.from.id)},
                                {"verdict", Json("approve")},
                            }),
                            15);
    if (!r.ok) {
        editCbCard(cq, ui_texts::voteQueueFailText(r.error_message),
                   InlineKeyboard{});
        answerCb(cq, ui_texts::voteQueueFailText(r.error_message), true);
        return;
    }
    editCbCard(cq,
               ui_texts::voteAcceptedText(displayName(cq.from), ""),
               InlineKeyboard{});
    store_.del("vote:" + track_id);
    answerCb(cq, ui_texts::kToastVoteRecorded);
}

void Dispatcher::cbShare(const TgCallbackQuery& cq,
                         const std::vector<std::string>& parts) {
    // pl:share:mylink | pl:share:cancel
    // pl:share:feedback:<sender>:<msg>:<file_unique>
    // pl:share:report:<sender>:<msg>
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verb = parts[2];
    if (verb == "mylink") {
        auto link = store_.get("sharelink:" + std::to_string(cq.from.id));
        if (!link) {
            answerCb(cq, ui_texts::kShareNoLink, true);
            return;
        }
        answerCb(cq, "");
        editCbCard(cq, *link, InlineKeyboard{});
        return;
    }
    if (verb == "cancel") {
        if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
        answerCb(cq, "");
        return;
    }
    if (verb == "feedback") {
        if (parts.size() < 6) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        // No dedup (ruling 8): a blocked user cannot send feedback,
        // but there is no "already sent" gate — every tap opens the
        // prompt and every text is forwarded.
        if (store_.sismember("shareblocked", std::to_string(cq.from.id))) {
            answerCb(cq, ui_texts::kShareFeedbackBlocked, true);
            return;
        }
        store_.set("sharefb:" + std::to_string(cq.from.id),
                   parts[3] + ":" + parts[4] + ":" + parts[5]);
        answerCb(cq, "");
        editCbCard(cq, ui_texts::kShareFeedbackPrompt, InlineKeyboard{});
        return;
    }
    if (verb == "report") {
        if (parts.size() < 5) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        // No dedup (ruling 8): reporting twice just re-blocks (the
        // sadd is idempotent) and answers the same confirmation —
        // never "already reported". kShareReportAlready is kept in
        // ui_texts.hpp for the future dedup work.
        store_.sadd("shareblocked", parts[3]);
        answerCb(cq, ui_texts::kShareReported);
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbDel(const TgCallbackQuery& cq,
                       const std::vector<std::string>& parts) {
    // pl:del:yes:<id> / pl:del:no:<id> — the §25 delete confirm.
    // "yes" posts library.delete (Phase 3, OD-3) and deletes the
    // confirm card on success; a backend refusal is answered with
    // the backend's exact error instead of pretending.
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verdict = parts[2];
    if (verdict == "no") {
        answerCb(cq, ui_texts::kToastDelCancelled);
        if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
        return;
    }
    if (verdict == "yes") {
        // library.delete (Phase 3, OD-3): {id} -> {deleted, id}.
        // Unknown id is a 404; the backend deletes the record AND
        // the file on disk when it has one.
        IpcResult r = ipc_.post("library.delete",
                                Json::objectOf({{"id", Json(parts[3])}}),
                                30);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        bool deleted = true;
        if (const Json* d = r.data.find("deleted"))
            deleted = d->asBool();
        if (!deleted) {
            answerCb(cq, ui_texts::kToastDelNothing, true);
            return;
        }
        answerCb(cq, ui_texts::kToastDelDeleted);
        if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbLossy(const TgCallbackQuery& cq,
                         const std::vector<std::string>& parts) {
    // pl:lossy:<accept|skip>:<id> — lossy re-encode prompt (§8).
    // "accept" takes the lossy fallback (delivers the stored file);
    // "skip" drops the download without delivering it.
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verdict = parts[2];
    const std::string& id = parts[3];
    if (verdict == "skip") {
        // dlrun value: "<file_path>\n<chat_id>[\n<artist>\n<title>]".
        std::string artist;
        std::string title;
        auto run = store_.get("dlrun:" + id);
        if (run) {
            size_t n1 = run->find('\n');
            size_t n2 = n1 == std::string::npos
                            ? std::string::npos
                            : run->find('\n', n1 + 1);
            size_t n3 = n2 == std::string::npos
                            ? std::string::npos
                            : run->find('\n', n2 + 1);
            if (n2 != std::string::npos)
                artist = n3 == std::string::npos
                             ? run->substr(n2 + 1)
                             : run->substr(n2 + 1, n3 - n2 - 1);
            if (n3 != std::string::npos) title = run->substr(n3 + 1);
        }
        store_.del("dlrun:" + id);
        answerCb(cq, artist.empty() && title.empty()
                         ? ui_texts::kToastLossySkipped
                         : ui_texts::lossySkippedText(artist, title));
        if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
        return;
    }
    if (verdict == "accept") {
        // Accept the lossy fallback: deliver the stored file as-is.
        auto run = store_.get("dlrun:" + id);
        if (!run) {
            answerCb(cq, ui_texts::kToastLossyExpired, true);
            return;
        }
        std::string file_path = run->substr(0, run->find('\n'));
        std::string err;
        answerCb(cq, "");
        if (tg_.sendAudioFile(cq.chat_id, file_path, "", &err) < 0) {
            answerCb(cq, err, true);
            return;
        }
        store_.del("dlrun:" + id);
        if (cq.has_message) tg_.deleteMessage(cq.chat_id, cq.message_id);
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbRestart(const TgCallbackQuery& cq,
                           const std::vector<std::string>& parts) {
    // pl:restart:<bot|replay>:<chat_id> — owner-only.
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (!isOwner(cq.from.id)) {
        answerCb(cq, ui_texts::kDenyOwner, true);
        return;
    }
    const std::string& what = parts[2];
    int64_t chat_id = 0;
    try {
        chat_id = std::stoll(parts[3]);
    } catch (...) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    if (what == "replay") {
        std::string err;
        if (!playerAction(chat_id, "replay", cq.from.id, &cq, err)) {
            answerCb(cq, err, true);
            return;
        }
        answerCb(cq, ui_texts::fmt(ui_texts::kToastReplayedBy,
                                   {{"user", displayName(cq.from)}}));
        refreshCard(cq, chat_id);
        return;
    }
    if (what == "bot") {
        answerCb(cq, ui_texts::kToastRestarting);
        std::exit(42);  // the supervisor restarts the core (onRestart)
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbDlset(const TgCallbackQuery& cq,
                         const std::vector<std::string>& parts) {
    // pl:dlset:of|uy|pt|fmt:<fmt>|uy:1|0|back — /download-settings
    // (per-user prefs in "dlset:<user>": format \n youtube \n template).
    if (parts.size() < 3) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    auto load = [&]() {
        std::string fmt = "mp3";
        std::string yt = "1";
        std::string tpl;
        auto v = store_.get("dlset:" + std::to_string(cq.from.id));
        if (v) {
            size_t n1 = v->find('\n');
            size_t n2 = n1 == std::string::npos
                            ? std::string::npos
                            : v->find('\n', n1 + 1);
            if (n1 != std::string::npos) {
                fmt = v->substr(0, n1);
                yt = n2 == std::string::npos
                         ? v->substr(n1 + 1)
                         : v->substr(n1 + 1, n2 - n1 - 1);
                if (n2 != std::string::npos) tpl = v->substr(n2 + 1);
            }
        }
        return std::tuple<std::string, std::string, std::string>(fmt, yt,
                                                                 tpl);
    };
    auto save = [&](const std::string& fmt, const std::string& yt,
                    const std::string& tpl) {
        store_.set("dlset:" + std::to_string(cq.from.id),
                   fmt + "\n" + yt + "\n" + tpl);
    };
    const std::string& verb = parts[2];
    auto [fmt, yt, tpl] = load();
    if (verb == "of") {
        answerCb(cq, "");
        editCbCard(cq,
                   ui_texts::fmt(ui_texts::kDlSettingsFormat,
                                 {{"format", fmt}}),
                   ui_texts::dlSettingsFormatKeyboard());
        return;
    }
    if (verb == "uy" && parts.size() < 4) {
        answerCb(cq, "");
        editCbCard(cq,
                   ui_texts::fmt(ui_texts::kDlSettingsYoutube,
                                 {{"value", yt == "1" ? "True" : "False"}}),
                   ui_texts::dlSettingsYoutubeKeyboard());
        return;
    }
    if (verb == "uy" && parts.size() >= 4) {
        yt = parts[3] == "1" ? "1" : "0";
        save(fmt, yt, tpl);
        answerCb(cq, ui_texts::kSettingsUpdated);
        editCbCard(cq,
                   ui_texts::fmt(ui_texts::kDlSettingsYoutube,
                                 {{"value", yt == "1" ? "True" : "False"}}),
                   ui_texts::dlSettingsYoutubeKeyboard());
        return;
    }
    if (verb == "pt") {
        store_.set("dlset_tpl_pending:" + std::to_string(cq.from.id), "1");
        answerCb(cq, "");
        editCbCard(cq, ui_texts::kDlSettingsTemplatePrompt,
                   InlineKeyboard{});
        return;
    }
    if (verb == "fmt") {
        if (parts.size() < 4) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        static const char* kFmts[] = {"mp3", "m4a", "flac",
                                      "opus", "ogg", "wav"};
        bool ok = false;
        for (const char* f : kFmts)
            if (parts[3] == f) ok = true;
        if (!ok) {
            answerCb(cq, ui_texts::kToastUnknownAction, true);
            return;
        }
        fmt = parts[3];
        save(fmt, yt, tpl);
        answerCb(cq, ui_texts::kSettingsUpdated);
        editCbCard(cq,
                   ui_texts::fmt(ui_texts::kDlSettingsFormat,
                                 {{"format", fmt}}),
                   ui_texts::dlSettingsFormatKeyboard());
        return;
    }
    if (verb == "back") {
        answerCb(cq, "");
        editCbCard(cq, ui_texts::kDlSettingsIndex,
                   ui_texts::dlSettingsIndexKeyboard());
        return;
    }
    answerCb(cq, ui_texts::kToastUnknownAction, true);
}

void Dispatcher::cbSpotify(const TgCallbackQuery& cq,
                           const std::vector<std::string>& parts) {
    // pl:spotify:<verb>:<id> — companion cards owned by spotify-rust
    // (engines worker). `dislike` already has its IPC
    // (spotify.dislike); `details` / `analyze` route to the existing
    // spotify.details / spotify.analyze actions (§9-E8). Any other
    // verb still answers honestly (§7.5: never leave a callback
    // un-answered). The track id is rejoined from parts[3:] for
    // details/analyze — ids are opaque and a ':' inside one must not
    // truncate it (cbVote rule); `dislike` keeps its exact current
    // parsing (parts[3]) — it is Live and out of scope here.
    if (parts.size() < 4) {
        answerCb(cq, ui_texts::kToastUnknownAction, true);
        return;
    }
    const std::string& verb = parts[2];
    if (verb == "dislike") {
        IpcResult r = ipc_.post("dislike",
                                Json::objectOf({
                                    {"user_id", Json(cq.from.id)},
                                    {"track_id", Json(parts[3])},
                                }),
                                15);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        answerCb(cq, ui_texts::kToastDisliked);
        return;
    }
    std::string track_id = parts[3];
    for (size_t i = 4; i < parts.size(); ++i) track_id += ":" + parts[i];
    if (verb == "details") {
        // GET /v1/spotify/details?user_id=&track_id= (§6.10, 10 s):
        // IpcClient::get translates the path to spotify.details and
        // carries the query params as the request payload.
        std::string path = "/v1/spotify/details?user_id=" +
                           std::to_string(cq.from.id) +
                           "&track_id=" + track_id;
        IpcResult r = ipc_.get(path, 10);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        answerCb(cq, "");
        InlineKeyboard kb = {{InlineButton::callback(
            "Analyze text", "pl:spotify:analyze:" + track_id)}};
        editCbCard(cq, spotifyDetailsText(r.data), kb);
        return;
    }
    if (verb == "analyze") {
        IpcResult r = ipc_.post("spotify.analyze",
                                Json::objectOf({
                                    {"user_id", Json(cq.from.id)},
                                    {"track_id", Json(track_id)},
                                }),
                                30);
        if (!r.ok) {
            answerCb(cq, r.error_message, true);
            return;
        }
        answerCb(cq, "");
        editCbCard(cq, spotifyAnalyzeText(r.data), InlineKeyboard{});
        return;
    }
    answerCb(cq, ui_texts::kToastFeatureUnavailable, true);
}


// --- TdLibTelegramSender ----------------------------------------------------
// The dispatcher's Telegram transport over the TDLib bot session
// (TdLibClient::call). Every method builds the TDLib request JSON and
// returns TDLib's exact error (code + message) on failure — nothing is
// faked, and a failed send is reported, never swallowed. All calls run
// on the thread that invokes the sender; the dispatcher guarantees
// that is the main thread (see dispatcher.hpp header note).

TdLibTelegramSender::TdLibTelegramSender(TdLibClient& bot,
                                         std::string data_dir)
    : bot_(bot), data_dir_(std::move(data_dir)) {}

// parseTextEntities (HTML/Markdown) or a plain formattedText when
// parse_mode is empty. File-local so the static inlineResult helper
// can use it too; the member formattedText below delegates to it.
static Json tdFormattedText(TdLibClient& bot, const std::string& text,
                            const std::string& parse_mode,
                            std::string& error_out) {
    if (parse_mode.empty()) {
        return Json::objectOf({
            {"@type", Json("formattedText")},
            {"text", Json(text)},
        });
    }
    Json mode;
    if (parse_mode == "HTML") {
        mode = Json::objectOf({{"@type", Json("textParseModeHTML")}});
    } else if (parse_mode == "Markdown") {
        mode = Json::objectOf({
            {"@type", Json("textParseModeMarkdown")},
            {"version", Json(static_cast<int64_t>(2))},
        });
    } else {
        error_out = "unknown parse_mode \"" + parse_mode +
                    "\" (expected \"\", \"HTML\" or \"Markdown\")";
        return Json();
    }
    Json req = Json::objectOf({
        {"@type", Json("parseTextEntities")},
        {"text", Json(text)},
        {"parse_mode", mode},
    });
    // parseTextEntities is a local computation: 10 s is ample, and a
    // timeout is reported as a parse failure (nothing is sent).
    Json resp = bot.call(req, 10.0);
    if (resp.isNull()) {
        error_out = "parseTextEntities timed out";
        return Json();
    }
    if (TdLibClient::isError(resp)) {
        error_out = "parseTextEntities failed: " +
                    TdLibClient::errorMessage(resp);
        return Json();
    }
    return resp;  // parseTextEntities returns the formattedText object
}

Json TdLibTelegramSender::formattedText(const std::string& text,
                                        const std::string& parse_mode,
                                        std::string& error_out) {
    return tdFormattedText(bot_, text, parse_mode, error_out);
}

Json TdLibTelegramSender::inputMessageText(const std::string& text,
                                           const std::string& parse_mode,
                                           std::string& error_out) {
    Json ft = formattedText(text, parse_mode, error_out);
    if (ft.isNull()) return Json();
    return Json::objectOf({
        {"@type", Json("inputMessageText")},
        {"text", ft},
        {"disable_web_page_preview", Json(true)},
        {"clear_draft", Json(true)},
    });
}

// One button -> its TDLib inlineKeyboardButton. callback_data travels
// as base64 (TDLib `bytes`); a URL button is a plain URL; a copy
// button is inlineKeyboardButtonTypeCopyText (client-side copy, no
// callback query); a switch button targets the current chat or a
// chosen chat.
static Json tdButton(const InlineButton& b) {
    Json type;
    if (!b.callback_data.empty()) {
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeCallback")},
            {"data", Json(scram::base64Encode(b.callback_data))},
        });
    } else if (!b.copy_text.empty()) {
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeCopyText")},
            {"text", Json(b.copy_text)},
        });
    } else if (!b.url.empty()) {
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeUrl")},
            {"url", Json(b.url)},
        });
    } else if (b.has_switch) {
        Json target = b.switch_current_chat
                          ? Json::objectOf(
                                {{"@type", Json("targetChatCurrent")}})
                          : Json::objectOf({
                                {"@type", Json("targetChatChosen")},
                                {"allow_user_chats", Json(true)},
                                {"allow_bot_chats", Json(true)},
                                {"allow_group_chats", Json(true)},
                                {"allow_channel_chats", Json(true)},
                            });
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeSwitchInline")},
            {"query", Json(b.switch_inline_query)},
            {"target_chat", target},
        });
    } else {
        // A button with no action: a no-op callback with empty data.
        type = Json::objectOf({
            {"@type", Json("inlineKeyboardButtonTypeCallback")},
            {"data", Json("")},
        });
    }
    return Json::objectOf({
        {"text", Json(b.text)},
        {"type", type},
    });
}

Json TdLibTelegramSender::inlineMarkup(const InlineKeyboard& kb) {
    if (kb.empty()) return Json(nullptr);
    Json::Array rows;
    for (const auto& row : kb) {
        Json::Array buttons;
        for (const auto& b : row) buttons.push_back(tdButton(b));
        rows.push_back(Json(buttons));
    }
    return Json::objectOf({
        {"@type", Json("replyMarkupInlineKeyboard")},
        {"rows", Json(rows)},
    });
}

Json TdLibTelegramSender::replyMarkup(const ReplyKeyboard& kb) {
    if (kb.rows.empty()) return Json(nullptr);
    Json::Array rows;
    for (const auto& row : kb.rows) {
        Json::Array buttons;
        for (const auto& label : row) {
            buttons.push_back(Json::objectOf({
                {"text", Json(label)},
                {"type", Json::objectOf(
                             {{"@type",
                               Json("keyboardButtonTypeText")}})},
            }));
        }
        rows.push_back(Json(buttons));
    }
    return Json::objectOf({
        {"@type", Json("replyMarkupShowKeyboard")},
        {"rows", Json(rows)},
        {"is_persistent", Json(true)},
        {"resize_keyboard", Json(kb.resize)},
        {"one_time", Json(kb.one_time)},
        {"is_personal", Json(true)},
    });
}

int64_t TdLibTelegramSender::sendWithContent(int64_t chat_id,
                                             const Json& content,
                                             const Json& reply_markup,
                                             int64_t reply_to,
                                             std::string* error_out) {
    if (content.isNull()) {
        if (error_out) *error_out = "could not build message content";
        return -1;
    }
    Json req = Json::objectOf({
        {"@type", Json("sendMessage")},
        {"chat_id", Json(chat_id)},
        {"reply_markup", reply_markup},
        {"input_message_content", content},
    });
    if (reply_to != 0) {
        req.set("reply_to", Json::objectOf({
            {"@type", Json("inputMessageReplyToMessage")},
            {"message_id", Json(reply_to)},
        }));
    }
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        if (error_out) *error_out = "sendMessage timed out";
        return -1;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return -1;
    }
    const Json* id = resp.find("id");
    if (!id) {
        if (error_out) *error_out = "sendMessage: no id in response";
        return -1;
    }
    return id->asInt();
}

bool TdLibTelegramSender::sendMessage(int64_t chat_id,
                                      const std::string& text,
                                      std::string& error_out) {
    return sendMessageId(chat_id, text, 0, &error_out) >= 0;
}

int64_t TdLibTelegramSender::sendMessageId(int64_t chat_id,
                                           const std::string& text,
                                           int64_t reply_to,
                                           std::string* error_out) {
    std::string err;
    Json content = inputMessageText(text, "", err);
    if (content.isNull()) {
        if (error_out) *error_out = err;
        return -1;
    }
    return sendWithContent(chat_id, content, Json(nullptr), reply_to,
                           error_out);
}

int64_t TdLibTelegramSender::sendMessageHtml(int64_t chat_id,
                                             const std::string& text,
                                             const InlineKeyboard& kb,
                                             int64_t reply_to,
                                             std::string* error_out) {
    std::string err;
    Json content = inputMessageText(text, "HTML", err);
    if (content.isNull()) {
        if (error_out) *error_out = err;
        return -1;
    }
    return sendWithContent(chat_id, content, inlineMarkup(kb), reply_to,
                           error_out);
}

int64_t TdLibTelegramSender::sendMessageReplyKb(int64_t chat_id,
                                                const std::string& text,
                                                const ReplyKeyboard& kb,
                                                int64_t reply_to,
                                                std::string* error_out) {
    std::string err;
    Json content = inputMessageText(text, "HTML", err);
    if (content.isNull()) {
        if (error_out) *error_out = err;
        return -1;
    }
    return sendWithContent(chat_id, content, replyMarkup(kb), reply_to,
                           error_out);
}

int64_t TdLibTelegramSender::sendMessageRemoveKb(int64_t chat_id,
                                                 const std::string& text,
                                                 int64_t reply_to,
                                                 std::string* error_out) {
    std::string err;
    Json content = inputMessageText(text, "HTML", err);
    if (content.isNull()) {
        if (error_out) *error_out = err;
        return -1;
    }
    Json markup = Json::objectOf({
        {"@type", Json("replyMarkupRemoveKeyboard")},
        {"is_personal", Json(true)},
    });
    return sendWithContent(chat_id, content, markup, reply_to, error_out);
}

int64_t TdLibTelegramSender::sendPhoto(int64_t chat_id,
                                       const std::string& photo,
                                       const std::string& caption,
                                       const InlineKeyboard& kb,
                                       std::string* error_out) {
    std::string err;
    Json ft = formattedText(caption, "HTML", err);
    if (ft.isNull()) {
        if (error_out) *error_out = err;
        return -1;
    }
    // A remote HTTP(S) URL is NOT a valid inputFileRemote id: TDLib
    // answers with its exact error, and the caller (sendNowPlayingCard)
    // falls back to a text card. A local path becomes inputFileLocal;
    // anything else is treated as a TDLib remote file id.
    Json file;
    std::error_code ec;
    if (!photo.empty() && photo[0] == '/' &&
        std::filesystem::exists(photo, ec)) {
        file = Json::objectOf({
            {"@type", Json("inputFileLocal")},
            {"path", Json(photo)},
        });
    } else {
        file = Json::objectOf({
            {"@type", Json("inputFileRemote")},
            {"id", Json(photo)},
        });
    }
    Json content = Json::objectOf({
        {"@type", Json("inputMessagePhoto")},
        {"photo", file},
        {"caption", ft},
    });
    return sendWithContent(chat_id, content, inlineMarkup(kb), 0, error_out);
}

bool TdLibTelegramSender::editMessageText(int64_t chat_id,
                                          int64_t message_id,
                                          const std::string& text,
                                          const InlineKeyboard& kb,
                                          std::string* error_out) {
    std::string err;
    Json content = inputMessageText(text, "HTML", err);
    if (content.isNull()) {
        if (error_out) *error_out = err;
        return false;
    }
    Json req = Json::objectOf({
        {"@type", Json("editMessageText")},
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"reply_markup", inlineMarkup(kb)},
        {"input_message_content", content},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        if (error_out) *error_out = "editMessageText timed out";
        return false;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return false;
    }
    return true;
}

bool TdLibTelegramSender::editMessageCaption(int64_t chat_id,
                                             int64_t message_id,
                                             const std::string& caption,
                                             const InlineKeyboard& kb,
                                             std::string* error_out) {
    std::string err;
    Json ft = formattedText(caption, "HTML", err);
    if (ft.isNull()) {
        if (error_out) *error_out = err;
        return false;
    }
    Json req = Json::objectOf({
        {"@type", Json("editMessageCaption")},
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"reply_markup", inlineMarkup(kb)},
        {"caption", ft},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        if (error_out) *error_out = "editMessageCaption timed out";
        return false;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return false;
    }
    return true;
}

bool TdLibTelegramSender::editMessageReplyMarkup(int64_t chat_id,
                                                 int64_t message_id,
                                                 const InlineKeyboard& kb,
                                                 std::string* error_out) {
    Json req = Json::objectOf({
        {"@type", Json("editMessageReplyMarkup")},
        {"chat_id", Json(chat_id)},
        {"message_id", Json(message_id)},
        {"reply_markup", inlineMarkup(kb)},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        if (error_out) *error_out = "editMessageReplyMarkup timed out";
        return false;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return false;
    }
    return true;
}

bool TdLibTelegramSender::answerCallbackQuery(
    const std::string& callback_query_id, const std::string& text,
    bool show_alert, int cache_time_sec, std::string* error_out) {
    Json resp = bot_.answerCallbackQuery(callback_query_id, text, show_alert,
                                         cache_time_sec);
    if (resp.isNull()) {
        if (error_out) *error_out = "answerCallbackQuery timed out";
        return false;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return false;
    }
    return true;
}

// One Bot API-shaped InlineQueryResult -> its TDLib
// inputInlineQueryResult. Audio results keep their audio_url (a native
// TDLib field); the message sent on pick is the caption as
// inputMessageText — an audio URL is not a TDLib InputFile, so it
// cannot be the inputMessageAudio payload. Article results keep their
// message_text (plain, no parse mode — titles can contain '<' '>' '&').
Json TdLibTelegramSender::inlineResult(const Json& r, TdLibClient& bot) {
    const Json* type = r.find("type");
    std::string kind = type ? type->asString() : "";
    const Json* id = r.find("id");
    std::string rid = id ? id->asString() : "";
    const Json* title = r.find("title");
    std::string rtitle = title ? title->asString() : "";
    // reply_markup arrives in the Bot API shape
    // {inline_keyboard: [[{text, url|callback_data|copy_text}]]};
    // translate it through the same button builder as the live
    // keyboards. A copy button carries copy_text:{text} (or a plain
    // string); it produces no callback query.
    Json markup = Json(nullptr);
    if (const Json* rm = r.find("reply_markup")) {
        if (const Json* rows = rm->find("inline_keyboard")) {
            InlineKeyboard kb;
            for (const auto& row : rows->asArray()) {
                std::vector<InlineButton> brow;
                for (const auto& b : row.asArray()) {
                    const Json* bt = b.find("text");
                    std::string btext = bt ? bt->asString() : "";
                    if (const Json* c = b.find("copy_text")) {
                        std::string copy;
                        if (c->isString()) copy = c->asString();
                        else if (const Json* t = c->find("text"))
                            copy = t->asString();
                        if (!copy.empty())
                            brow.push_back(
                                InlineButton::copy(btext, copy));
                    } else if (const Json* u = b.find("url"))
                        brow.push_back(
                            InlineButton::link(btext, u->asString()));
                    else if (const Json* d = b.find("callback_data"))
                        brow.push_back(InlineButton::callback(
                            btext, d->asString()));
                }
                kb.push_back(brow);
            }
            markup = inlineMarkup(kb);
        }
    }
    if (kind == "audio") {
        const Json* stream = r.find("audio_url");
        const Json* perf = r.find("performer");
        const Json* dur = r.find("audio_duration");
        const Json* cap = r.find("caption");
        std::string caption = cap ? cap->asString() : rtitle;
        std::string err;
        Json ft = tdFormattedText(bot, caption, "HTML", err);
        if (ft.isNull())
            ft = tdFormattedText(bot, caption, "", err);
        Json content = Json::objectOf({
            {"@type", Json("inputMessageText")},
            {"text", ft},
            {"disable_web_page_preview", Json(true)},
            {"clear_draft", Json(true)},
        });
        return Json::objectOf({
            {"@type", Json("inputInlineQueryResultAudio")},
            {"id", Json(rid)},
            {"title", Json(rtitle)},
            {"performer", Json(perf ? perf->asString() : "")},
            {"audio_url", Json(stream ? stream->asString() : "")},
            {"audio_duration",
             Json(dur ? dur->asInt() : static_cast<int64_t>(0))},
            {"reply_markup", markup},
            {"input_message_content", content},
        });
    }
    // Article (and anything unrecognised): message_text as plain text.
    std::string msg;
    if (const Json* imc = r.find("input_message_content")) {
        if (const Json* mt = imc->find("message_text"))
            msg = mt->asString();
    }
    if (msg.empty()) msg = rtitle;
    std::string err;
    Json ft = tdFormattedText(bot, msg, "", err);
    Json content = Json::objectOf({
        {"@type", Json("inputMessageText")},
        {"text", ft},
        {"disable_web_page_preview", Json(true)},
        {"clear_draft", Json(true)},
    });
    const Json* desc = r.find("description");
    const Json* thumb = r.find("thumb_url");
    return Json::objectOf({
        {"@type", Json("inputInlineQueryResultArticle")},
        {"id", Json(rid)},
        {"url", Json("")},
        {"title", Json(rtitle)},
        {"description", Json(desc ? desc->asString() : "")},
        {"thumbnail_url", Json(thumb ? thumb->asString() : "")},
        {"thumbnail_width", Json(static_cast<int64_t>(0))},
        {"thumbnail_height", Json(static_cast<int64_t>(0))},
        {"reply_markup", markup},
        {"input_message_content", content},
    });
}

bool TdLibTelegramSender::answerInlineQuery(
    const std::string& inline_query_id, const Json& results,
    int cache_time_sec, const std::string& switch_pm_text,
    const std::string& switch_pm_parameter, std::string* error_out) {
    int64_t qid = 0;
    try {
        qid = std::stoll(inline_query_id);
    } catch (...) {
        if (error_out)
            *error_out = "inline_query_id is not numeric: " + inline_query_id;
        return false;
    }
    Json::Array out;
    if (results.isArray()) {
        for (const auto& r : results.asArray())
            out.push_back(inlineResult(r, bot_));
    }
    Json button = Json(nullptr);
    if (!switch_pm_text.empty()) {
        button = Json::objectOf({
            {"@type", Json("inlineQueryResultsButton")},
            {"text", Json(switch_pm_text)},
            {"type", Json::objectOf({
                {"@type", Json("inlineQueryResultsButtonTypeStartBot")},
                {"parameter", Json(switch_pm_parameter)},
            })},
        });
    }
    Json req = Json::objectOf({
        {"@type", Json("answerInlineQuery")},
        {"inline_query_id", Json(qid)},
        {"is_personal", Json(false)},
        {"button", button},
        {"results", Json(out)},
        {"cache_time", Json(static_cast<int64_t>(cache_time_sec))},
        {"next_offset", Json("")},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        if (error_out) *error_out = "answerInlineQuery timed out";
        return false;
    }
    if (TdLibClient::isError(resp)) {
        if (error_out) *error_out = TdLibClient::errorMessage(resp);
        return false;
    }
    return true;
}

bool TdLibTelegramSender::downloadFile(const std::string& file_id,
                                       const std::string& dest_dir,
                                       std::string& saved_path_out,
                                       std::string& error_out) {
    // Resolve the remote id, then download synchronously: TDLib
    // blocks until the file is on disk (or fails). No 20 MB Bot API
    // cap — TDLib downloads any size.
    Json req = Json::objectOf({
        {"@type", Json("getRemoteFile")},
        {"remote_file_id", Json(file_id)},
        {"file_type", Json::objectOf({{"@type", Json("fileTypeNone")}})},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull()) {
        error_out = "getRemoteFile timed out for " + file_id;
        return false;
    }
    if (TdLibClient::isError(resp)) {
        error_out = "getRemoteFile failed: " +
                    TdLibClient::errorMessage(resp);
        return false;
    }
    const Json* fid = resp.find("id");
    if (!fid) {
        error_out = "getRemoteFile response has no file id";
        return false;
    }
    Json dl = Json::objectOf({
        {"@type", Json("downloadFile")},
        {"file_id", Json(fid->asInt())},
        {"priority", Json(static_cast<int64_t>(1))},
        {"offset", Json(static_cast<int64_t>(0))},
        {"limit", Json(static_cast<int64_t>(0))},
        {"synchronous", Json(true)},
    });
    Json done = bot_.call(dl, 300.0);
    if (done.isNull()) {
        error_out = "downloadFile timed out for " + file_id;
        return false;
    }
    if (TdLibClient::isError(done)) {
        error_out = "downloadFile failed: " +
                    TdLibClient::errorMessage(done);
        return false;
    }
    const Json* local = done.find("local");
    const Json* path = local ? local->find("path") : nullptr;
    if (!path || path->asString().empty()) {
        error_out = "downloadFile returned no local path for " + file_id;
        return false;
    }
    // Keep the old contract: the caller gets a path under dest_dir
    // named after the (sanitised) file id, extension from the TDLib
    // local path.
    std::string safe;
    for (char c : file_id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            safe += c;
        else
            safe += '_';
    }
    std::string ext;
    std::string local_path = path->asString();
    size_t dot = local_path.rfind('.');
    if (dot != std::string::npos) ext = local_path.substr(dot);
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    std::string dest = dest_dir + "/" + safe + ext;
    std::filesystem::copy_file(local_path, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        error_out = "could not copy downloaded file to " + dest + ": " +
                    ec.message();
        return false;
    }
    saved_path_out = dest;
    return true;
}

std::string TdLibTelegramSender::getChatMemberStatus(int64_t chat_id,
                                                     int64_t user_id) {
    Json req = Json::objectOf({
        {"@type", Json("getChatMember")},
        {"chat_id", Json(chat_id)},
        {"member_id", Json::objectOf({
            {"@type", Json("messageSenderUser")},
            {"user_id", Json(user_id)},
        })},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull() || TdLibClient::isError(resp)) return "";
    const Json* status = resp.find("status");
    if (!status) return "";
    const Json* type = status->find("@type");
    if (!type) return "";
    std::string t = type->asString();
    const std::string prefix = "chatMemberStatus";
    if (t.rfind(prefix, 0) == 0) t = t.substr(prefix.size());
    if (t == "Creator") return "creator";
    if (t == "Administrator") return "administrator";
    if (t == "Member") return "member";
    if (t == "Restricted") return "restricted";
    if (t == "Left") return "left";
    if (t == "Banned") return "kicked";
    return "";
}

std::vector<int64_t> TdLibTelegramSender::getChatAdministrators(
    int64_t chat_id) {
    Json req = Json::objectOf({
        {"@type", Json("getChatAdministrators")},
        {"chat_id", Json(chat_id)},
    });
    Json resp = bot_.call(req, 30.0);
    std::vector<int64_t> out;
    if (resp.isNull() || TdLibClient::isError(resp)) return out;
    const Json* admins = resp.find("administrators");
    if (!admins) return out;
    for (const auto& a : admins->asArray()) {
        const Json* uid = a.find("user_id");
        if (uid) out.push_back(uid->asInt());
    }
    return out;
}

int64_t TdLibTelegramSender::getChatId(const std::string& username_or_id) {
    try {
        size_t used = 0;
        int64_t id = std::stoll(username_or_id, &used);
        if (used == username_or_id.size()) return id;
    } catch (...) {
    }
    std::string name = username_or_id;
    if (!name.empty() && name[0] == '@') name = name.substr(1);
    Json req = Json::objectOf({
        {"@type", Json("searchPublicChat")},
        {"username", Json(name)},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull() || TdLibClient::isError(resp)) return 0;
    const Json* id = resp.find("id");
    return id ? id->asInt() : 0;
}

bool TdLibTelegramSender::deleteMessage(int64_t chat_id,
                                        int64_t message_id) {
    Json req = Json::objectOf({
        {"@type", Json("deleteMessages")},
        {"chat_id", Json(chat_id)},
        {"message_ids", Json(Json::Array{Json(message_id)})},
        {"revoke", Json(true)},
    });
    Json resp = bot_.call(req, 30.0);
    return !resp.isNull() && !TdLibClient::isError(resp);
}

bool TdLibTelegramSender::leaveChat(int64_t chat_id) {
    Json req = Json::objectOf({
        {"@type", Json("leaveChat")},
        {"chat_id", Json(chat_id)},
    });
    Json resp = bot_.call(req, 30.0);
    return !resp.isNull() && !TdLibClient::isError(resp);
}

static bool tdForward(TdLibClient& bot, int64_t to_chat, int64_t from_chat,
                      int64_t message_id, bool send_copy) {
    Json req = Json::objectOf({
        {"@type", Json("forwardMessages")},
        {"chat_id", Json(to_chat)},
        {"topic_id", Json(nullptr)},
        {"from_chat_id", Json(from_chat)},
        {"message_ids", Json(Json::Array{Json(message_id)})},
        {"options", Json::objectOf({
            {"@type", Json("messageSendOptions")},
            {"disable_notification", Json(false)},
            {"from_background", Json(false)},
            {"protect_content", Json(false)},
            {"update_order_of_installed_sticker_sets", Json(false)},
        })},
        {"send_copy", Json(send_copy)},
        {"remove_caption", Json(false)},
    });
    Json resp = bot.call(req, 30.0);
    return !resp.isNull() && !TdLibClient::isError(resp);
}

bool TdLibTelegramSender::forwardMessage(int64_t to_chat, int64_t from_chat,
                                         int64_t message_id) {
    return tdForward(bot_, to_chat, from_chat, message_id, false);
}

bool TdLibTelegramSender::copyMessage(int64_t to_chat, int64_t from_chat,
                                      int64_t message_id) {
    return tdForward(bot_, to_chat, from_chat, message_id, true);
}

int64_t TdLibTelegramSender::sendDocument(int64_t chat_id,
                                          const std::string& filename,
                                          const std::string& content,
                                          const std::string& caption,
                                          std::string* error_out) {
    // TDLib sends documents from disk: stage the content under
    // data_dir, send, then unlink the temp file.
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    std::string tmp = data_dir_ + "/send-" +
                      std::to_string(chat_id) + "-" + filename;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error_out) *error_out = "could not write " + tmp;
            return -1;
        }
        out << content;
    }
    std::string err;
    Json ft = formattedText(caption, "HTML", err);
    if (ft.isNull()) ft = formattedText(caption, "", err);
    Json doc = Json::objectOf({
        {"@type", Json("inputMessageDocument")},
        {"document", Json::objectOf({
            {"@type", Json("inputFileLocal")},
            {"path", Json(tmp)},
        })},
        {"caption", ft},
    });
    int64_t id = sendWithContent(chat_id, doc, Json(nullptr), 0, error_out);
    std::filesystem::remove(tmp, ec);
    return id;
}

int64_t TdLibTelegramSender::sendLocalFile(int64_t chat_id,
                                           const std::string& file_path,
                                           const std::string& caption,
                                           bool audio,
                                           std::string* error_out) {
    std::string err;
    Json ft = formattedText(caption, "HTML", err);
    if (ft.isNull()) ft = formattedText(caption, "", err);
    Json file = Json::objectOf({
        {"@type", Json("inputFileLocal")},
        {"path", Json(file_path)},
    });
    Json content;
    if (audio) {
        content = Json::objectOf({
            {"@type", Json("inputMessageAudio")},
            {"audio", file},
            {"duration", Json(static_cast<int64_t>(0))},
            {"title", Json("")},
            {"performer", Json("")},
            {"caption", ft},
        });
    } else {
        content = Json::objectOf({
            {"@type", Json("inputMessageDocument")},
            {"document", file},
            {"caption", ft},
        });
    }
    return sendWithContent(chat_id, content, Json(nullptr), 0, error_out);
}

int64_t TdLibTelegramSender::sendAudioFile(int64_t chat_id,
                                           const std::string& file_path,
                                           const std::string& caption,
                                           std::string* error_out) {
    return sendLocalFile(chat_id, file_path, caption, true, error_out);
}

int64_t TdLibTelegramSender::sendFile(int64_t chat_id,
                                      const std::string& file_path,
                                      const std::string& caption,
                                      std::string* error_out) {
    return sendLocalFile(chat_id, file_path, caption, false, error_out);
}

int64_t TdLibTelegramSender::sendAudio(int64_t chat_id,
                                       const std::string& audio,
                                       const std::string& caption,
                                       const InlineKeyboard& kb,
                                       std::string* error_out) {
    std::string err;
    Json ft = formattedText(caption, "HTML", err);
    if (ft.isNull()) ft = formattedText(caption, "", err);
    Json content = Json::objectOf({
        {"@type", Json("inputMessageAudio")},
        {"audio", Json::objectOf({
            {"@type", Json("inputFileRemote")},
            {"id", Json(audio)},
        })},
        {"duration", Json(static_cast<int64_t>(0))},
        {"title", Json("")},
        {"performer", Json("")},
        {"caption", ft},
    });
    return sendWithContent(chat_id, content, inlineMarkup(kb), 0, error_out);
}

std::string TdLibTelegramSender::getUserMention(int64_t user_id) {
    Json req = Json::objectOf({
        {"@type", Json("getUser")},
        {"user_id", Json(user_id)},
    });
    Json resp = bot_.call(req, 30.0);
    if (resp.isNull() || TdLibClient::isError(resp))
        return std::to_string(user_id);
    const Json* first = resp.find("first_name");
    const Json* username = resp.find("username");
    std::string name = first ? first->asString() : "";
    std::string uname = username ? username->asString() : "";
    if (name.empty()) return std::to_string(user_id);
    if (!uname.empty()) return name + " (@" + uname + ")";
    return name;
}

}  // namespace parlay
