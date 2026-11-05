// ui_texts.hpp — every core-facing UI string for the Parlay Telegram UI
// (Phase 9, UI_INVENTORY.md). One namespace, one constant per string,
// {name} placeholders, Bot API HTML, English only, normal casing, typos
// corrected (§27). dispatcher.cpp includes this; no string literal is
// duplicated in a handler (§0 rule 2).
//
// Pure helpers in this file (progress bar, track tokens, duration
// formatting, keyboard builders) are unit-tested in tests/ui_test.cpp.
// Strings owned by another service (vc-ts inline results, downloaders
// progress internals, spotify-rust companion cards, share-rust flows)
// live in that service, not here — see phase9/port-core.md for the split.
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "scram.hpp"
#include "tg_types.hpp"

namespace parlay::ui_texts {

// --- helpers ---------------------------------------------------------------

// Escape user-derived text (& < >) before it goes into an HTML card.
// Track titles, usernames and chat titles all pass through this.
inline std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

// Replace {key} placeholders: fmt(tpl, {{"title", t}, {"by", u}}).
inline std::string fmt(
    std::string tpl,
    std::initializer_list<std::pair<std::string, std::string>> kv) {
    for (const auto& [k, v] : kv) {
        std::string ph = "{" + k + "}";
        size_t pos = 0;
        while ((pos = tpl.find(ph, pos)) != std::string::npos) {
            tpl.replace(pos, ph.size(), v);
            pos += v.size();
        }
    }
    return tpl;
}

// Truncate to max_chars Unicode code points (never splits a UTF-8
// sequence); appends "…" when cut. Used for card titles (25 / 40 chars).
inline std::string truncUtf8(const std::string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars >= max_chars) {
            return s.substr(0, i) + "…";
        }
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        i += len;
        ++chars;
    }
    return s;
}

// M:SS (0 -> "0:00"). Positions are always known numbers.
inline std::string mmss(int64_t sec) {
    if (sec < 0) sec = 0;
    return std::to_string(sec / 60) + ":" +
           (sec % 60 < 10 ? "0" : "") + std::to_string(sec % 60);
}

// Human duration for a track: "Xh Ym Zs" / "M:SS" / "∞" when unknown
// (TGVCBot §1.3 rule, §4).
inline std::string durationStr(int64_t sec) {
    if (sec <= 0) return "∞";
    if (sec >= 3600) {
        return std::to_string(sec / 3600) + "h " +
               std::to_string((sec % 3600) / 60) + "m " +
               std::to_string(sec % 60) + "s";
    }
    return mmss(sec);
}

// The ONE progress bar (§0 rule 4): Yukki/Arc GetProgressBar, 9 stages,
// "◉" ball, "—" track. stage = round(8 * pos / dur), clamped 0..8.
inline std::string progressBar(int64_t pos_sec, int64_t dur_sec) {
    int stage = 0;
    if (dur_sec > 0) {
        double f = static_cast<double>(pos_sec) /
                   static_cast<double>(dur_sec);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        stage = static_cast<int>(f * 8.0 + 0.5);
        if (stage > 8) stage = 8;
    }
    std::string bar;
    for (int i = 0; i < 9; ++i) bar += (i == stage ? "◉" : "—");
    return bar;
}

// "{position} {bar} {duration}" — the no-op progress button label (§4).
inline std::string progressLabel(int64_t pos_sec, int64_t dur_sec) {
    return mmss(pos_sec) + " " + progressBar(pos_sec, dur_sec) + " " +
           durationStr(dur_sec);
}

// Queue token (§1): the first 10 hex chars of SHA-256(track id) —
// exactly Yukki's trackDeleteToken rule. The full id is used only when
// it is ≤20 chars and contains no ':'.
inline std::string trackToken(const std::string& track_id) {
    if (!track_id.empty() && track_id.size() <= 20 &&
        track_id.find(':') == std::string::npos)
        return track_id;
    std::string digest = scram::sha256(track_id);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 5; ++i) {
        unsigned char c = static_cast<unsigned char>(digest[i]);
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

// FNV-1a hash of a search query, 12 hex chars — the search-pagination
// hash in pl:search:page:<user>:<page>:<hash> (§1, Music163bot pattern).
inline std::string queryHash(const std::string& q) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : q) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 6; ++i) {
        unsigned char c = static_cast<unsigned char>((h >> (8 * (5 - i))) &
                                                     0xFF);
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

// --- callback data (§1 namespace) ------------------------------------------
// Format: pl:<domain>:<verb>[:<arg>…], colon-separated, lowercase,
// 64-byte Bot API limit (callers keep payloads short; track ids go
// through trackToken, never raw).
inline std::string cbHelp(const std::string& topic) {
    return "pl:help:" + topic;
}
inline std::string cbPlayer(const std::string& verb, int64_t chat_id) {
    return "pl:player:" + verb + ":" + std::to_string(chat_id);
}
inline std::string cbPlayerToken(const std::string& verb,
                                 const std::string& token) {
    return "pl:player:" + verb + ":" + token;
}
inline std::string cbQueueMore(int64_t chat_id) {
    return "pl:queue:more:" + std::to_string(chat_id);
}
inline std::string cbQueueDel(int64_t chat_id, const std::string& token) {
    return "pl:queue:del:" + std::to_string(chat_id) + ":" + token;
}
inline std::string cbSet(const std::string& key) { return "pl:set:" + key; }
inline std::string cbSetInfo(const std::string& key) {
    return "pl:set:info:" + key;
}
inline std::string cbSetTimer(int n) {
    return "pl:set:timer:" + std::to_string(n);
}
inline std::string cbAuthDel(int64_t chat_id, int64_t user_id) {
    return "pl:auth:del:" + std::to_string(chat_id) + ":" +
           std::to_string(user_id);
}
inline std::string cbSearchPick(int n, int64_t user_id,
                                const std::string& token) {
    return "pl:search:" + std::to_string(n) + ":" +
           std::to_string(user_id) + ":" + token;
}
inline std::string cbSearchPage(int64_t user_id, int page,
                                const std::string& hash) {
    return "pl:search:page:" + std::to_string(user_id) + ":" +
           std::to_string(page) + ":" + hash;
}
inline std::string cbDlCancel(const std::string& run_id) {
    return "pl:dl:cancel:" + run_id;
}
inline std::string cbDlFx(const std::string& run_id,
                          const std::string& effect) {
    return "pl:dl:fx:" + run_id + ":" + effect;
}
inline std::string cbVote(const std::string& verdict,
                          const std::string& track_id) {
    return "pl:vote:" + verdict + ":" + track_id;
}
inline std::string cbDel(const std::string& verdict, const std::string& id) {
    return "pl:del:" + verdict + ":" + id;
}
inline std::string cbLossy(const std::string& verdict,
                           const std::string& id) {
    return "pl:lossy:" + verdict + ":" + id;
}
inline std::string cbRestart(const std::string& what, int64_t chat_id) {
    return "pl:restart:" + what + ":" + std::to_string(chat_id);
}

inline const char* kCbStart = "pl:start";
inline const char* kCbClose = "pl:close";
inline const char* kCbHelpMain = "pl:help:main";
inline const char* kCbLangSelect = "pl:lang:select";
inline const char* kCbLangEn = "pl:lang:en";
inline const char* kCbBcastCancel = "pl:bcast:cancel";
inline const char* kCbSetMain = "pl:set:main";

// --- §2 /start --------------------------------------------------------------

// PM start card (Yukki wins, §2). {user} = first name, HTML-escaped by
// the caller. {bot} = bot username (no @).
inline const char* kStartPm =
    "<h1>Parlay</h1>\n"
    "\n"
    "Hey {user} — I stream music in your Telegram voice &amp; video chats "
    "from YouTube, Spotify, SoundCloud, Apple Music, NetEase and more. "
    "Queue, loop, shuffle, seek and control speed with a live queue.\n"
    "\n"
    "<b>Enjoy high-quality audio, fast downloads and smooth playback "
    "everywhere.</b>\n"
    "\n"
    "<i>Tap the buttons below to add me or explore commands.</i>";

// Group start card (Yukki wins, §2).
inline const char* kStartGroup =
    "<b>Parlay</b>\n"
    "\n"
    "I stream music in your Telegram voice &amp; video chats from YouTube, "
    "Spotify, SoundCloud, Apple Music, NetEase and more — with queue, loop, "
    "shuffle, seek and speed control.\n"
    "\n"
    "<i>Try a command… if you think you can handle me.</i>";

inline const char* kBotAdded =
    "Thanks for adding me! Use /play [song] to start playing music.";
inline const char* kDemoted =
    "<h2>Permission Lost</h2>\n"
    "\n"
    "I've been <b>demoted from admin</b> and can't perform my tasks "
    "anymore. Without proper rights, I'm unable to function here, so I'll "
    "be <i>leaving the chat now</i>.\n"
    "\n"
    "<b>Goodbye</b>";
inline const char* kOwnerJoined =
    "<b>{user}</b> — owner of Parlay — just entered the chat.";
inline const char* kSudoJoined =
    "<b>{user}</b>, a sudo user of Parlay, just joined the chat.";

// Private-mode notice (navidrome §7.2, §2). Only when Parlay is in
// private mode; otherwise the normal start card.
inline const char* kPrivateBotNotice =
    "This is a private bot for managing a music library.\n"
    "\n"
    "Your user ID: <code>{user_id}</code>\n"
    "\n"
    "If you're the server admin, add this ID to <b>ALLOWED_USERS</b> in "
    "your bot configuration.";

// About text (beetube §3.4, rewritten music-only, §14). {channel} is
// the updates channel handle without @; the repo links are Parlay's own.
inline const char* kAbout =
    "<h2>About</h2>\n"
    "\n"
    "Parlay is an open source bot developed to give users access to "
    "unlimited free music downloads and streaming from within Telegram.\n"
    "\n"
    "Official channel: @{channel}\n"
    "Github: <a href=\"https://github.com/filewrap/ParlayMusic\">"
    "filewrap/ParlayMusic</a>\n"
    "For bug reports create an <a href=\"https://github.com/filewrap/"
    "ParlayMusic/issues\">issue</a>\n"
    "\n"
    "<code>If you find this bot useful, feel free to give us a star</code>\n"
    "\n"
    "Want to contribute to the development of this project? PRs are "
    "welcomed.";

// Start-card keyboards (§2, §13). Rows are built by the dispatcher so
// the Updates/Support/Source buttons can be omitted when unconfigured.
inline InlineKeyboard startPmKeyboard(const std::string& bot_username,
                                      const std::string& updates_url,
                                      const std::string& support_url,
                                      const std::string& source_url) {
    InlineKeyboard kb;
    kb.push_back({InlineButton::link(
        "Add Me to Your Group",
        "https://t.me/" + bot_username + "?startgroup&admin=invite_users")});
    kb.push_back(
        {InlineButton::callback("Help & Commands", kCbHelpMain)});
    std::vector<InlineButton> row;
    if (!updates_url.empty()) row.push_back(InlineButton::link("Updates", updates_url));
    if (!support_url.empty()) row.push_back(InlineButton::link("Support", support_url));
    if (!row.empty()) kb.push_back(std::move(row));
    std::vector<InlineButton> row2;
    row2.push_back(InlineButton::callback("Language", kCbLangSelect));
    if (!source_url.empty())
        row2.push_back(InlineButton::link("Source Code", source_url));
    kb.push_back(std::move(row2));
    // Inline shortcuts (navidrome §7.2.2 + lemonic §6.1, §13).
    kb.push_back({InlineButton::switchInline("Search", "", true),
                  InlineButton::switchInline("Library", "lib ", true),
                  InlineButton::switchInline("Delete", "del ", true)});
    kb.push_back({InlineButton::switchInline("Playing", "np", true),
                  InlineButton::switchInline("Lyrics", "l", true),
                  InlineButton::switchInline("Share", "s", true)});
    kb.push_back(
        {InlineButton::switchInline("Send playing to…", "np", false)});
    return kb;
}

inline InlineKeyboard startGroupKeyboard(const std::string& updates_url,
                                         const std::string& support_url) {
    InlineKeyboard kb;
    std::vector<InlineButton> row;
    if (!updates_url.empty()) row.push_back(InlineButton::link("Updates", updates_url));
    if (!support_url.empty()) row.push_back(InlineButton::link("Support", support_url));
    if (!row.empty()) kb.push_back(std::move(row));
    return kb;
}

// --- §3 /help (menu + topics) ----------------------------------------------
// Menu text (Yukki wins, §3). {support} = support URL.
inline const char* kHelpMenu =
    "<h1>Help Menu</h1>\n"
    "\n"
    "Browse commands by category: <b>Public</b> for everyone, plus "
    "<b>Admin</b>, <b>Sudo</b> and <b>Owner</b> for higher access levels.\n"
    "\n"
    "<i>Tip: run <code>/help &lt;command&gt;</code> to get detailed help "
    "for any single command.</i>\n"
    "\n"
    "<i>For any issues or support, contact our <a href=\"{support}\">"
    "support chat</a>.</i>";

// 12 topic buttons (Parvati's breadth, Yukki's 4 are a subset), 3 per
// row; back row returns to the start card (§3).
inline InlineKeyboard helpMenuKeyboard() {
    using B = InlineButton;
    return {
        {B::callback("Admin", cbHelp("admins")),
         B::callback("Auth", cbHelp("auth")),
         B::callback("Broadcast", cbHelp("bcast"))},
        {B::callback("Play", cbHelp("play")),
         B::callback("Sudo", cbHelp("sudoers")),
         B::callback("Restrict", cbHelp("restrict"))},
        {B::callback("Thumbnail", cbHelp("thumb")),
         B::callback("Start", cbHelp("start")),
         B::callback("Autoplay", cbHelp("autoplay"))},
        {B::callback("Playlist", cbHelp("playlist")),
         B::callback("VC Logs", cbHelp("vclogs")),
         B::callback("Playback Buttons", cbHelp("inline"))},
        {B::callback("Back", kCbStart)},
    };
}

// Back row on every topic page (§3).
inline InlineKeyboard helpTopicKeyboard() {
    return {{InlineButton::callback("Back", kCbHelpMain)}};
}

// Topic pages: Parvati/Yukki merged, small-caps converted to normal
// English + Bot API HTML (tables → lines), §3. Channel variants note
// (Yukki): "Channel variants (e.g. /cpause, /cskip) act on the linked
// channel's voice chat."
inline const char* kHelpPlay =
    "<h2>Play &amp; Channel Play</h2>\n"
    "\n"
    "<blockquote>• <b>/play</b> : start audio streaming in chat.\n"
    "• <b>/vplay</b> : start video streaming in chat.\n"
    "• <b>/playforce</b> : instant play (overrides queue).</blockquote>\n"
    "\n"
    "<h2>Channel Play</h2>\n"
    "\n"
    "<blockquote>• <b>/cplay</b> : audio stream in linked channel.\n"
    "• <b>/cvplay</b> : video stream in linked channel.\n"
    "• <b>/cplayforce</b> : force stream in linked channel.\n"
    "• <b>/channelplay [id]</b> : connect channel to this group.</blockquote>\n"
    "\n"
    "Send a music link to download it. Add <b>re</b> after the link to "
    "force re-download. Every download and upload import carries a "
    "Cancel button until it finishes.";

inline const char* kHelpAdmins =
    "<h2>Admin Commands</h2>\n"
    "\n"
    "<blockquote><b>Just add c as a prefix to the commands to control "
    "channel streaming.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>/pause</b> : halt the currently active stream.\n"
    "• <b>/resume</b> : restart the paused playback.\n"
    "• <b>/skip</b> : play the next track in the queue.\n"
    "• <b>/askip</b> : play the next track in the autoplay queue.\n"
    "• <b>/stop</b> : cease the stream and empty queue.\n"
    "• <b>/mute</b> : mute playback in videochat.\n"
    "• <b>/unmute</b> : unmute playback in videochat.\n"
    "• <b>/queue</b> : view the list of upcoming tracks.\n"
    "• <b>/jump</b> : jump to a given time in track.\n"
    "• <b>/move</b> : move a queued track to another position.\n"
    "• <b>/clear</b> : clear all songs from queue.\n"
    "• <b>/remove</b> : remove a specific track from queue.\n"
    "• <b>/loop [1-10]</b> : repeat the currently playing song.\n"
    "• <b>/shuffle</b> : randomize the queued playlist.\n"
    "• <b>/seek [secs]</b> : forward seek to a specific time.\n"
    "• <b>/seekback [secs]</b> : reverse seek to a specific time.\n"
    "• <b>/speed</b> : modify playback speed (0.50x - 4.00x).\n"
    "• <b>/playmode</b> : control who can use /play cmd.\n"
    "• <b>/cmddelete</b> : toggle automatic deletion of bot commands.\n"
    "• <b>/join</b> : join the assistant to the voice chat.\n"
    "• <b>/reload</b> : refresh the admin cache.</blockquote>\n"
    "\n"
    "Channel variants (e.g. /cpause, /cskip) act on the linked channel's "
    "voice chat.";

inline const char* kHelpOwner =
    "<h2>Owner Commands</h2>\n"
    "\n"
    "<blockquote>• <b>/addsudo</b> : add a new sudo user to bot.\n"
    "• <b>/delsudo</b> : remove a user from sudo list.\n"
    "• <b>/blockuser [username | id]</b> : stop user from using bot.\n"
    "• <b>/unblockuser [username | id]</b> : allow the blocked user.\n"
    "• <b>/blacklistchat [chat id]</b> : ban a chat from using bot.\n"
    "• <b>/unblockchat [chat id]</b> : unban a blacklisted chat.\n"
    "• <b>/blacklisted</b> : show all blacklisted groups &amp; users.\n"
    "• <b>/broadcast</b> : send a message to all chats.\n"
    "• <b>/maintenance</b> : [enable | disable] maintenance mode.\n"
    "• <b>/restart</b> : restart the bot.\n"
    "• <b>/sh</b> : run shell commands.</blockquote>";

inline const char* kHelpSudoers =
    "<h2>System &amp; Sudo Management</h2>\n"
    "\n"
    "<blockquote><b>Manage logs, and administrative privileges of your "
    "bot.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>Bot logs &amp; maintenance</b> :\n"
    "• <b>/logs</b> : get real-time logs of the bot.\n"
    "• <b>/logger</b> : [enable | disable] bot activity logging.\n"
    "• <b>/maintenance</b> : [enable | disable] maintenance mode.\n"
    "• <b>/eval</b> : execute code snippets.\n"
    "• <b>/sh</b> : run shell commands.</blockquote>\n"
    "\n"
    "<h2>Active Streams</h2>\n"
    "\n"
    "<blockquote>• <b>/activevoice</b> : view ongoing audio sessions.\n"
    "• <b>/ac</b> : view all combined active streams.</blockquote>\n"
    "\n"
    "<blockquote>• <b>Sudo list management</b> :\n"
    "• <b>/addsudo</b> : add a new sudo user to bot.\n"
    "• <b>/rmsudo</b> : remove a user from sudo list.\n"
    "• <b>/sudolist</b> : check all authorized sudo users.\n"
    "\n"
    "• These commands are highly sensitive, use with care.</blockquote>";

inline const char* kHelpAuth =
    "<h2>Auth Users</h2>\n"
    "\n"
    "<blockquote><b>Auth users are granted admin privileges without "
    "requiring group admin status to manage streams.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>/auth [user]</b> : add user to authorized list.\n"
    "• <b>/unauth [user]</b> : remove user from authorized list.\n"
    "• <b>/authlist</b> : display all authorized members.</blockquote>";

inline const char* kHelpBcast =
    "<h2>Broadcast Feature [only for sudoers]</h2>\n"
    "\n"
    "<blockquote>• <b>/broadcast [message or reply]</b> : send to all "
    "chats.\n"
    "\n"
    "• <b>Modes</b> :\n"
    "<b>-pin</b> : pin the message\n"
    "<b>-pinloud</b> : pin + notify members\n"
    "<b>-user</b> : broadcast to started users\n"
    "<b>-assistant</b> : send via assistant\n"
    "<b>-nobot</b> : don't send from bot\n"
    "\n"
    "<b>Example:</b> /broadcast -user -pin testing broadcast</blockquote>";

inline const char* kHelpRestrict =
    "<h2>Blacklist &amp; Global Ban</h2>\n"
    "\n"
    "<blockquote><b>Restrict specific chats or users from using the bot "
    "globally.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>Chat blacklist</b> :\n"
    "• <b>/blacklistchat [chat id]</b> : ban a chat from using bot.\n"
    "• <b>/unblockchat [chat id]</b> : unban a blacklisted chat.\n"
    "• <b>/blacklisted</b> : show all blacklisted groups &amp; "
    "users.</blockquote>\n"
    "\n"
    "<blockquote>• <b>User block</b> :\n"
    "• <b>/blockuser [username | id]</b> : stop user from using bot.\n"
    "• <b>/unblockuser [username | id]</b> : allow the blocked user.\n"
    "• <b>/blacklisted</b> : view the list of all blocked users.</blockquote>";

inline const char* kHelpThumb =
    "<h2>Thumbnail Settings</h2>\n"
    "\n"
    "<blockquote>• <b>/nothumb</b> : manage stream visual settings.\n"
    "\n"
    "<b>Toggle album art display. Disabling it reduces data load and "
    "maintains a minimalist chat look.</b>\n"
    "\n"
    "• <b>Options</b> :\n"
    "• <b>enable</b> : display artwork with stream info.\n"
    "• <b>disable</b> : send pure text-based updates.\n"
    "• <b>close</b> : dismiss the configuration.\n"
    "\n"
    "• Settings are saved individually per chat.</blockquote>";

inline const char* kHelpStart =
    "<h2>Basic Commands</h2>\n"
    "\n"
    "<blockquote>• <b>/start</b> : initialize the music service.\n"
    "• <b>/help</b> : access the command guidelines.\n"
    "• <b>/ping</b> : measure system latency and ping.\n"
    "• <b>/repo</b> : get the source code repo.\n"
    "• <b>/position</b> : show current track's timestamp.\n"
    "• <b>/reload</b> : refresh admin data cache.\n"
    "• <b>/settings</b> : adjust configuration options.\n"
    "• <b>/lang</b> : select your preferred language.\n"
    "• <b>/bug</b> : report an issue or problem.\n"
    "• <b>/json</b> : show message JSON structure.\n"
    "• <b>/sudolist</b> : view the list of sudo admins.\n"
    "• <b>/stats</b> : view comprehensive bot metrics.</blockquote>";

inline const char* kHelpAutoplay =
    "<h2>Autoplay Control</h2>\n"
    "\n"
    "<blockquote>• <b>/autoplay</b> : toggle the autoplay settings panel.\n"
    "• <b>/skip</b> : play the next track in the autoplay queue.</blockquote>\n"
    "\n"
    "<h2>What is Autoplay?</h2>\n"
    "\n"
    "<blockquote><b>Autoplay seamlessly adds related tracks to the queue "
    "based on your current playlist for non-stop music.</b>\n"
    "\n"
    "• <b>Panel options</b> :\n"
    "• <b>enable</b> : activate smart recommendations.\n"
    "• <b>disable</b> : deactivate smart recommendations.\n"
    "• <b>close</b> : exit the control panel.\n"
    "\n"
    "• <b>Note</b> : requires at least one active song to function.</blockquote>";

inline const char* kHelpPlaylist =
    "<h2>Playlist Commands</h2>\n"
    "\n"
    "<blockquote><b>Create, organize, and manage your personal "
    "playlists.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>/createplaylist</b> : create a new playlist.\n"
    "• <b>/deleteplaylist</b> : delete one of your playlists.\n"
    "• <b>/addtoplaylist</b> : add a track to a playlist.\n"
    "• <b>/removefromplaylist</b> : remove a track from a playlist.\n"
    "• <b>/playlistinfo</b> : display information about a playlist.\n"
    "• <b>/myplaylists</b> : list all of your playlists.</blockquote>\n"
    "\n"
    "Your personal playlist: <code>/addplaylist [song]</code> saves a song, "
    "<code>/playlist</code> lists it, <code>/playplaylist</code> plays it "
    "(add <code>-s</code> to shuffle, <code>-v</code> for video).";

inline const char* kHelpVclogs =
    "<h2>VC Logger Module</h2>\n"
    "\n"
    "<blockquote>• <b>/vclogger [on|off]</b> : toggle join / leave alerts.\n"
    "• <b>/vcstatus</b> : verify the current logger state.\n"
    "\n"
    "• Tracks user IDs during VC activity.\n"
    "• Alerts are cleaned automatically after 5 sec.</blockquote>";

inline const char* kHelpInline =
    "<h2>Bot Play Button Details</h2>\n"
    "\n"
    "<blockquote><b>Manage your music playback directly through inline "
    "buttons with ease.</b></blockquote>\n"
    "\n"
    "<blockquote>• <b>Playback controls</b> :\n"
    "• <b>Resume</b> : resume the paused stream.\n"
    "• <b>Pause</b> : pause the ongoing stream.\n"
    "• <b>Replay</b> : replay the current song from start.\n"
    "• <b>Skip</b> : skip the current track and play next.\n"
    "• <b>Stop</b> : stop the playback and clear queue.\n"
    "\n"
    "• <b>Seek and navigation</b> :\n"
    "• <b>-15s</b> : seek backward by 15 seconds.\n"
    "• <b>15s+</b> : seek forward by 15 seconds.\n"
    "\n"
    "• All buttons only for group admins.</blockquote>\n"
    "\n"
    "Send a music link to download it. Add <b>re</b> after the link to "
    "force re-download. In inline mode, type <code>@{bot} </code> followed "
    "by a song name to search, or use <code>np</code> for now playing, "
    "<code>s</code> to share, <code>l</code> for lyrics, <code>lib</code> "
    "to search the library, <code>del</code> to delete.";

// Radio topic page (openradiobot §2.1, §3/§21; corrected English).
inline const char* kHelpRadio =
    "<h2>Radio Commands</h2>\n"
    "\n"
    "<blockquote>• <b>/new</b> : create your radio.\n"
    "• <b>/destroy</b> : destroy your radio.\n"
    "• <b>/manage</b> : check your radio status.\n"
    "• <b>/play [song|URL]</b> : play a song or stream.\n"
    "• <b>/pause</b> / <b>/resume</b> : pause / resume playback.\n"
    "• <b>/skip</b> : skip the current song.\n"
    "• <b>/stop</b> : stop the player.\n"
    "• <b>/queue</b> : view the radio queue.\n"
    "• <b>/autoplay [on|off]</b> : toggle autoplay.\n"
    "• <b>/loop [queue|single|none]</b> : set the loop type.</blockquote>";

inline const char* kSearchHelp =
    "You can search for tracks, albums and playlists.\n"
    "\n"
    "Track search example:\n"
    "<pre>search (track name)</pre><pre>search track (track name)</pre>\n"
    "Album search example:\n"
    "<pre>search album (album name)</pre>\n"
    "Playlist search example:\n"
    "<pre>search playlist (playlist name)</pre>\n"
    "\n"
    "The search query is case-insensitive so you don't have to worry about "
    "capital and small letters. You can also search for tracks in inline "
    "mode.";

// Look a topic key up: returns the page text, or "" when unknown.
inline std::string helpTopicText(const std::string& topic) {
    if (topic == "play") return kHelpPlay;
    if (topic == "admins") return kHelpAdmins;
    if (topic == "owner") return kHelpOwner;
    if (topic == "sudoers") return kHelpSudoers;
    if (topic == "auth") return kHelpAuth;
    if (topic == "bcast") return kHelpBcast;
    if (topic == "restrict") return kHelpRestrict;
    if (topic == "thumb") return kHelpThumb;
    if (topic == "start") return kHelpStart;
    if (topic == "autoplay") return kHelpAutoplay;
    if (topic == "playlist") return kHelpPlaylist;
    if (topic == "vclogs") return kHelpVclogs;
    if (topic == "inline") return kHelpInline;
    if (topic == "radio") return kHelpRadio;
    if (topic == "search") return kSearchHelp;
    return "";
}

// Topic title for the "category tap answers with the category title"
// toast (§3).
inline std::string helpTopicTitle(const std::string& topic) {
    if (topic == "play") return "Play Commands";
    if (topic == "admins") return "Admin Commands";
    if (topic == "owner") return "Owner Commands";
    if (topic == "sudoers") return "Sudo Commands";
    if (topic == "auth") return "Auth Users";
    if (topic == "bcast") return "Broadcast";
    if (topic == "restrict") return "Restrict";
    if (topic == "thumb") return "Thumbnail Settings";
    if (topic == "start") return "Basic Commands";
    if (topic == "autoplay") return "Autoplay Control";
    if (topic == "playlist") return "Playlist Commands";
    if (topic == "vclogs") return "VC Logger";
    if (topic == "inline") return "Playback Buttons";
    if (topic == "radio") return "Radio Commands";
    if (topic == "search") return "Search Commands";
    return "";
}

inline const char* kHelpGroup =
    "Hi! For bot help and commands, please DM me directly — I'm more "
    "responsive in private chats!";
inline const char* kHelpUnknownCmd =
    "<i>No help found for command <code>{cmd}</code></i>";
inline const char* kHelpBackToast = "Returning to main menu...";
inline const char* kHelpUnknownCategory = "Unknown help category.";

// Unhandled text (rustify §7.1, §3). {list} is the
// command list for the caller's access level.
inline const char* kUnhandledText =
    "<b>Your request was not handled</b>\n"
    "\n"
    "<b>Commands available to you:</b>\n"
    "{list}";

// Per-command help (§3): intro + Usage + Restrictions + Use Case.
// The table covers every command the core routes; unknown commands fall
// back to kHelpUnknownCmd. Channel variants are auto-generated by the
// dispatcher ("Channel play variant of /{base}").
struct CmdHelp {
    const char* cmd;
    const char* usage;
    const char* restriction;
    const char* use_case;
};
inline const CmdHelp kCmdHelpTable[] = {
    {"play", "/play <song name, link or track id> [-f force] [-v video]",
     "play_mode (admin-only play needs admin/auth/sudo)",
     "Start music in the voice chat, or queue it when something is playing."},
    {"vplay", "/vplay <song name, link or track id>",
     "play_mode", "Play a video in the voice chat."},
    {"playforce", "/playforce <song name, link or track id>",
     "play_mode", "Play immediately, replacing the current track."},
    {"playplaylist", "/playplaylist [-s shuffle] [-v video]",
     "play_mode", "Play your saved personal playlist."},
    {"pause", "/pause", "admin_mode",
     "Pause the currently playing track."},
    {"resume", "/resume", "admin_mode", "Resume a paused track."},
    {"skip", "/skip", "admin_mode", "Skip to the next track in the queue."},
    {"stop", "/stop", "admin_mode", "Stop playback and clear the queue."},
    {"end", "/end", "admin_mode", "Alias of /stop."},
    {"loop", "/loop [0-10 | off]", "admin_mode",
     "Repeat the current track n times (0 = off)."},
    {"seek", "/seek <seconds>", "admin_mode",
     "Seek forward to a position in seconds (min 10)."},
    {"seekback", "/seekback <seconds>", "admin_mode",
     "Seek backward by a number of seconds."},
    {"jump", "/jump <mm:ss>", "admin_mode",
     "Jump to a timestamp in the current track."},
    {"speed", "/speed [0.50-4.00]", "admin_mode",
     "Set playback speed (0.50x–4.00x). 1.0 is normal speed."},
    {"bass", "/bass [0-100]", "admin_mode", "Set the bass boost level."},
    {"mute", "/mute", "admin",
     "Mute playback in the voice chat."},
    {"unmute", "/unmute", "admin", "Unmute playback in the voice chat."},
    {"replay", "/replay", "admin_mode", "Replay the current track from the start."},
    {"player", "/player", "everyone",
     "Re-send the current now-playing card."},
    {"queue", "/queue", "everyone",
     "Show the current queue with per-track delete buttons."},
    {"playing", "/playing", "everyone", "Alias of /queue."},
    {"clear", "/clear", "admin_mode",
     "Clear the queue (restorable with /restore)."},
    {"restore", "/restore", "admin_mode",
     "Restore the queue cleared by the last /clear."},
    {"remove", "/remove <position | 1-5 | 1,3,5>", "admin_mode",
     "Remove one track (or a range / list) from the queue."},
    {"move", "/move <from> <to>", "admin_mode",
     "Move a queued track to another position."},
    {"shuffle", "/shuffle", "admin_mode", "Shuffle the waiting queue."},
    {"pos", "/pos", "everyone", "Show the current track position and speed."},
    {"position", "/position", "everyone", "Alias of /pos."},
    {"autoplay", "/autoplay [on|off|lang <lang>|mood <mood>]", "admin",
     "Toggle autoplay, or set its language / mood."},
    {"autoplay-skip", "/autoplay-skip", "admin_mode",
     "Skip to the next autoplay recommendation."},
    {"askip", "/askip", "admin_mode", "Alias of /autoplay-skip."},
    {"lyrics", "/lyrics [song]", "everyone",
     "Lyrics for the current track, or the named song."},
    {"search", "/search <query> (ne: prefix for NetEase)", "everyone",
     "Search for tracks and pick one to play."},
    {"download", "/download <song or link>", "admin_mode",
     "Download the audio file of a song or link."},
    {"radio", "/radio [station]", "admin_mode",
     "List radio stations, or start one in the voice chat."},
    {"share", "/share <song>", "everyone",
     "Create an anonymous share link for a song."},
    {"engine", "/engine [go|ts|rust]", "admin",
     "Show or switch this chat's VC engine."},
    {"addplaylist", "/addplaylist [song name | link]",
     "everyone", "Save a song to your personal playlist."},
    {"playlist", "/playlist", "everyone",
     "List your personal playlist."},
    {"rmplaylist", "/rmplaylist <number | song name | all>", "everyone",
     "Remove a song from your personal playlist."},
    {"clearplaylist", "/clearplaylist", "everyone",
     "Clear your personal playlist."},
    {"playmode", "/playmode [admin|everyone]", "admin",
     "Control who can use /play in this chat."},
    {"settings", "/settings", "admin",
     "Open the chat settings panel."},
    {"adminmode", "/adminmode [admin|adminauth|everyone]", "admin",
     "Control who can manage playback in this chat."},
    {"cmddelete", "/cmddelete [on|off]", "admin",
     "Delete command messages after they run."},
    {"cleanmode", "/cleanmode [on|off|<minutes 1-1440>]", "admin",
     "Auto-delete bot messages after N minutes."},
    {"thumbnail", "/thumbnail [on|off]", "admin",
     "Toggle thumbnails (album art) on cards."},
    {"nothumb", "/nothumb [on|off]", "admin",
     "Alias of /thumbnail with inverted sense."},
    {"channelplay", "/channelplay <@channel | channel id | disable>", "admin",
     "Link a channel so /cplay commands drive its voice chat."},
    {"userbot", "/userbot", "admin",
     "Show the assistant (userbot) status for this chat."},
    {"join", "/join", "admin", "Join the voice chat."},
    {"auth", "/auth <user> (or reply)", "admin",
     "Authorize a user to control playback without admin status."},
    {"unauth", "/unauth <user> (or reply)", "admin",
     "Remove a user from the authorized list."},
    {"authlist", "/authlist", "admin",
     "List the authorized users of this chat."},
    {"admincache", "/admincache", "admin",
     "Refresh the admin list (once every 10 minutes)."},
    {"reload", "/reload", "admin", "Alias of /admincache."},
    {"blacklist", "/blacklist <user id | @username | chat id>", "sudo",
     "Block a user or chat from using the bot."},
    {"unblacklist", "/unblacklist <user id | @username | chat id>", "sudo",
     "Unblock a user or chat."},
    {"blacklisted", "/blacklisted", "sudo",
     "List all blacklisted chats and users."},
    {"addsudo", "/addsudo <user>", "owner", "Add a sudo user."},
    {"delsudo", "/delsudo <user>", "owner", "Remove a sudo user."},
    {"listsudo", "/listsudo", "everyone", "List the owner and sudo users."},
    {"sudolist", "/sudolist", "everyone", "Alias of /listsudo."},
    {"ac", "/ac", "sudo", "Count the active voice chats."},
    {"activevc", "/activevc", "sudo",
     "List the active voice chats with their current track."},
    {"broadcast", "/broadcast [-copy|-nochat|-user] (reply to a message)",
     "sudo", "Broadcast a message to all chats/users."},
    {"maintenance", "/maintenance [on|off] [reason]", "owner",
     "Put the bot into maintenance mode."},
    {"autoleave", "/autoleave [on|off]", "sudo",
     "Leave empty voice chats automatically."},
    {"json", "/json", "everyone",
     "Show the JSON of the replied-to message."},
    {"speedtest", "/speedtest", "sudo",
     "Run a download + upload speed test."},
    {"bug", "/bug <describe the problem>", "everyone",
     "Send a bug report to the developers."},
    {"privacy", "/privacy", "everyone", "Show the privacy policy."},
    {"ping", "/ping", "everyone", "Check latency and system stats."},
    {"alive", "/alive", "everyone", "Alias of /ping."},
    {"stats", "/stats", "everyone", "Show bot statistics."},
    {"eval", "/eval <shell>", "owner", "Run a shell snippet (owner only)."},
    {"sh", "/sh <shell>", "owner", "Alias of /eval."},
    {"logs", "/logs", "sudo", "Get the bot log file."},
    {"logger", "/logger [enable|disable|on|off]", "sudo",
     "Toggle activity logging to the log channel."},
    {"restart", "/restart", "sudo", "Restart the bot."},
    {"setrtmp", "/setrtmp [chat id] [rtmp://host/app] [stream key]", "admin (DM only)",
     "Configure an RTMP target for a chat."},
    {"stream", "/stream [name or url]", "admin",
     "Stream a song to the configured RTMP target."},
    {"streamstop", "/streamstop", "admin", "Stop the RTMP stream."},
    {"streamstatus", "/streamstatus", "admin",
     "Show the RTMP stream status."},
    {"start", "/start [help]", "everyone", "Show the start card."},
    {"help", "/help [command]", "everyone",
     "Show the help menu, a topic, or one command's help."},
    {"lang", "/lang [code]", "admin",
     "Show or set the chat language (English only today)."},
    {"id", "/id", "everyone", "Show this chat's ID (and thread, if any)."},
    {"now", "/now", "everyone", "Show what is currently streaming."},
    {"menu", "/menu", "admin",
     "Open the playback control panel for this chat."},
    {"keyboard", "/keyboard", "everyone",
     "Enable the persistent reply keyboard."},
    {"about", "/about", "everyone", "Show build and version info."},
    {"status", "/status", "everyone", "Show cached-song statistics."},
};

inline const CmdHelp* findCmdHelp(const std::string& cmd) {
    for (const auto& h : kCmdHelpTable) {
        if (cmd == h.cmd) return &h;
    }
    return nullptr;
}

// Render one command's help block: intro + Usage + Restrictions + Use
// Case (§3). Channel variants get the auto-generated note.
inline std::string perCommandHelpText(const std::string& cmd) {
    const CmdHelp* h = findCmdHelp(cmd);
    if (!h) return "";
    std::string out = "<b>/" + cmd + "</b>\n\n";
    out += "<b>Usage:</b> <code>" + htmlEscape(h->usage) + "</code>\n";
    out += "<b>Restrictions:</b> " + std::string(h->restriction) + "\n";
    out += "<b>Use case:</b> " + std::string(h->use_case);
    return out;
}

// --- §4 now-playing card ----------------------------------------------------

// One track as rendered on a card. title/url/duration/requester are
// already HTML-escaped by the caller (titles come from engines).
struct CardTrack {
    std::string title;      // display title, escaped, truncated by caller
    std::string url;        // track URL (may be empty)
    std::string duration;   // human duration ("3:24", "∞")
    std::string requester;  // display name of the requester, escaped
    std::string token;      // queue token (trackToken)
};

inline std::string trackLink(const CardTrack& t) {
    if (!t.url.empty()) return "<a href=\"" + t.url + "\">" + t.title + "</a>";
    return t.title;
}

// Started-streaming card (Yukki wins for text, §4). Title truncated to
// 25 chars in the card (Yukki ShortTitle).
inline std::string nowPlayingText(const CardTrack& t) {
    return fmt("<h2>Now Playing</h2>\n"
               "\n"
               "<b>▫ Track:</b> {track}\n"
               "<b>▫ Duration:</b> {duration}\n"
               "<b>▫ Requested by:</b> {by}",
               {{"track", trackLink(t)},
                {"duration", t.duration},
                {"by", t.requester}});
}

// State cards (TgMusicBot buildTrackMessage template, §4): the same
// card with a state line + a "by {user}" trailer.
inline std::string stateCardText(const std::string& state,
                                 const CardTrack& t,
                                 const std::string& by_user) {
    return fmt("<h2>{state}</h2>\n"
               "\n"
               "<b>▫ Track:</b> {track}\n"
               "<b>▫ Duration:</b> {duration}\n"
               "<b>▫ Requested by:</b> {by}\n"
               "{trailer}",
               {{"state", state},
                {"track", trackLink(t)},
                {"duration", t.duration},
                {"by", t.requester},
                {"trailer", by_user.empty()
                                ? ""
                                : state + " by " + by_user}});
}

inline const char* kStoppedCard =
    "<b>Playback stopped.</b>\nRequested by: {user}";

// Queued card (Yukki wins, §4).
inline std::string queuedText(int index, const CardTrack& t) {
    return fmt("<b>Added to Queue:</b> #{index}\n"
               "\n"
               "<b>▫ Track:</b> {track}\n"
               "<b>▫ Duration:</b> {duration}\n"
               "<b>▫ Requested by:</b> {by}",
               {{"index", std::to_string(index)},
                {"track", trackLink(t)},
                {"duration", t.duration},
                {"by", t.requester}});
}

// Merged 5-row control keyboard (Tina seek row + Yukki pause toggle +
// Parvati autoplay row, §4). Row 1 is the no-op progress button (only
// when NOT queued — the caller omits it for queued cards). The pause
// button reads Pause while playing, Resume while paused. Row 5 is Close +
// Add-to-Playlist (TgMusicBot). The loop state is shown on the replay
// button label when looping ("Loop ×n").
inline InlineKeyboard playerKeyboard(int64_t chat_id, bool paused,
                                     bool autoplay_on, int loop_count,
                                     const std::string& token,
                                     bool include_progress,
                                     int64_t pos_sec, int64_t dur_sec) {
    using B = InlineButton;
    InlineKeyboard kb;
    if (include_progress) {
        kb.push_back({B::callback(
            progressLabel(pos_sec, dur_sec),
            cbPlayer("progress", chat_id))});
    }
    std::string replay = loop_count > 0
                             ? "Loop ×" + std::to_string(loop_count)
                             : "Replay";
    kb.push_back({
        B::callback(paused ? "Resume" : "Pause",
                    cbPlayer(paused ? "resume" : "pause", chat_id)),
        B::callback(replay, cbPlayer("replay", chat_id)),
        B::callback("Skip", cbPlayer("skip", chat_id)),
        B::callback("Stop", cbPlayer("stop", chat_id)),
    });
    kb.push_back({
        B::callback("-15s", cbPlayer("seekback15", chat_id)),
        B::callback("+15s", cbPlayer("seek15", chat_id)),
    });
    kb.push_back({B::callback(
        std::string("Autoplay: ") + (autoplay_on ? "Enabled" : "Disabled"),
        cbPlayer("autoplay", chat_id))});
    std::vector<B> row5;
    row5.push_back(B::callback("Close", kCbClose));
    if (!token.empty())
        row5.push_back(
            B::callback("Add to Playlist", cbPlayerToken("addlist", token)));
    kb.push_back(std::move(row5));
    return kb;
}

// Queued card keyboard: Play Now (NoxBeats/TgMusicBot play_now, §4).
inline InlineKeyboard queuedKeyboard(const std::string& token) {
    if (token.empty()) return {};
    return {{InlineButton::callback("Play Now", cbPlayerToken("playnow", token))}};
}

// Callback toasts + edited cards (Parvati wins, corrected English, §4).
inline std::string toastPaused(int64_t pos_sec) {
    return "Track paused at " + mmss(pos_sec);
}
inline std::string toastResumed(int64_t pos_sec) {
    return "Resumed at " + mmss(pos_sec) + ".";
}
inline const char* kToastReplayedBy = "Track replayed by {user}.";
inline const char* kToastSkipped = "Track skipped.";
inline const char* kToastSkippedBy = "Skipped by {user}";
inline const char* kToastStopped = "Playback stopped.";
inline const char* kToastMuted = "Muted.";
inline const char* kToastUnmuted = "Unmuted.";
inline std::string toastSeekFwd(int seconds) {
    return "Sought forward " + std::to_string(seconds) + " seconds";
}
inline std::string toastSeekBack(int seconds) {
    return "Sought back " + std::to_string(seconds) + " seconds";
}
inline std::string toastSeekNearEnd(int seconds) {
    return "Cannot seek forward " + std::to_string(seconds) +
           " seconds — about to reach end.";
}
inline const char* kToastResumeFailed = "Failed to resume playback.";
inline const char* kToastSeekFailed = "Failed to seek.\nError: <code>{error}</code>";
inline const char* kToastSkipEmpty = "Playback stopped — queue empty.";
inline const char* kToastInvalidRequest = "Invalid request.";
inline const char* kToastUnknownAction = "Unknown action.";
inline const char* kToastNothingPlaying = "Nothing playing right now.";
inline const char* kToastExpired = "This button has expired.";
inline const char* kToastNotAllowed = "You aren't allowed";
inline const char* kToastVoteRecorded = "Vote recorded.";
inline const char* kToastFeatureUnavailable =
    "This feature is not available yet.";
// Toggle toasts (player row + settings panel): plain text, no HTML —
// answerCallbackQuery shows the text verbatim.
inline const char* kToastAutoplayOn = "Autoplay enabled.";
inline const char* kToastAutoplayOff = "Autoplay disabled.";
inline const char* kToastThumbOn = "Thumbnails enabled.";
inline const char* kToastThumbOff = "Thumbnails disabled.";
inline const char* kToastShuffle = "Queue shuffled.";
inline const char* kToastAddedToPlaylist = "Added to your playlist.";
inline const char* kToastAlreadyInPlaylist = "Already in your playlist.";
inline const char* kToastPlaylistFull = "Your playlist is full.";
inline const char* kToastDisliked = "Disliked.";

// Stop confirm (Yukki wins, §4).
inline const char* kStopConfirmPaused =
    "<h2>Playback is currently paused.</h2>\n"
    "\n"
    "Do you want to resume playing or stop it completely?";
inline const char* kStopConfirmMuted =
    "<h2>Playback is currently muted.</h2>\n"
    "\n"
    "Do you want to unmute or stop it completely?";
inline InlineKeyboard stopConfirmKeyboard(int64_t chat_id, bool paused) {
    using B = InlineButton;
    std::vector<B> row;
    if (paused)
        row.push_back(B::callback("Resume",
                                  "pl:player:confirm:resume:" +
                                      std::to_string(chat_id)));
    else
        row.push_back(B::callback("Unmute",
                                  "pl:player:confirm:unmute:" +
                                      std::to_string(chat_id)));
    row.push_back(B::callback("Stop",
                              "pl:player:confirm:stop:" +
                                  std::to_string(chat_id)));
    return {std::move(row)};
}

// Restart confirm (Yukki §3.3, §4).
inline const char* kRestartConfirm =
    "A song is currently playing in this chat.\nChoose an action below.";
inline InlineKeyboard restartConfirmKeyboard(int64_t chat_id) {
    using B = InlineButton;
    return {{B::callback("Restart the Bot", cbRestart("bot", chat_id)),
             B::callback("Replay the Song", cbRestart("replay", chat_id))}};
}
inline const char* kToastRestarting = "Restarting the bot...";
inline const char* kToastReplayOk = "Current song replayed successfully.";

// Text-command control replies (remix wins, §4). The three-way select:
// true -> done, false -> not streaming, null -> not in call.
inline const char* kCtlPaused = "<b>Paused.</b>";
inline const char* kCtlResumed = "<b>Resumed.</b>";
inline const char* kCtlSkipped = "<b>Skipped.</b>";
inline const char* kCtlNotStreaming = "<b>Not streaming.</b>";
inline const char* kCtlMuted = "<b>Muted.</b>";
inline const char* kCtlUnmuted = "<b>Unmuted.</b>";
inline const char* kCtlAlreadyMuted = "<b>Already muted.</b>";
inline const char* kCtlJoined = "<b>Joined the voice chat.</b>";
inline const char* kCtlAlreadyJoined =
    "<b>Already in the voice chat.</b>";
inline const char* kCtlNotMuted = "<b>Not muted.</b>";
inline const char* kCtlLoopOn = "<b>Turned loop on.</b>";
inline const char* kCtlLoopOff = "<b>Turned loop off.</b>";

// Control panel /menu (remix §2.3, §4). Admin-only; only the user who
// sent the replied-to command may press (others silently ignored).
inline std::string menuPanelText(const std::string& now_title,
                                 const std::string& now_url,
                                 const std::string& next_title,
                                 const std::string& next_url) {
    std::string now = now_title.empty() ? "Nothing streaming now" : now_title;
    std::string next =
        next_title.empty() ? "Nothing streaming next" : next_title;
    // An empty URL must not produce <a href=""> (Telegram rejects it):
    // the fallback titles render as plain bold.
    auto linked = [](const std::string& title, const std::string& url) {
        if (url.empty()) return "<b>" + title + "</b>";
        return "<b><a href=\"" + url + "\">" + title + "</a></b>";
    };
    // The state is carried by the Now/Next labels; no emoji is emitted
    // anywhere in the UI (§7.3).
    return "<b>Now:</b> " + linked(now, now_url) + "\n<b>Next:</b> " +
           linked(next, next_url);
}
inline InlineKeyboard menuPanelKeyboard(int64_t chat_id) {
    using B = InlineButton;
    auto p = [chat_id](const std::string& v) {
        return cbPlayer(v, chat_id);
    };
    return {
        {B::callback("Shuffle", p("shuffle")), B::callback("Skip", p("skip")),
         B::callback("Pause", p("pause")), B::callback("Resume", p("resume"))},
        {B::callback("Mute", p("mute")), B::callback("Unmute", p("unmute")),
         B::callback("Vol -", p("voldec")), B::callback("Vol +", p("volinc"))},
        {B::callback("Refresh", p("refresh"))},
    };
}
inline const char* kToastPanelUpdated = "Updated.";
inline std::string toastVolumeSet(int amount) {
    return "Volume set to " + std::to_string(amount) + ".";
}

// /now (remix §2.2, §4).
inline std::string nowText(const std::string& title,
                           const std::string& title_url,
                           const std::string& requester,
                           const std::string& requester_url) {
    return fmt("Currently streaming <b><a href=\"{titleUrl}\">{title}</a></b> "
               "by <b><a href=\"{requesterUrl}\">{requester}</a></b>...",
               {{"titleUrl", title_url},
                {"title", title},
                {"requesterUrl", requester_url},
                {"requester", requester}});
}
inline const char* kNowIdle = "<b>Not streaming.</b>";

// /position (Parvati/Yukki §3.6/§3.7, §4).
inline const char* kPositionText =
    "<b>{title}</b>\n"
    "<code>{position} / {duration}</code>\n"
    "Speed: <b>{speed}x</b>";

// Voice-chat events (Tina/Yukki §2.9/§3.9, §4).
inline const char* kVcStarted =
    "<h2>Voice chat started!</h2>\n"
    "\n"
    "<i>Use /play [song name] to play music.</i>";
inline const char* kVcEndedStop =
    "The voice chat was ended, so playback has stopped.\n"
    "\n"
    "Start a new voice chat and use /play again.";

// --- §5 queue display ------------------------------------------------------
// Yukki wins — the only rich queue in any ref (table + per-track delete
// + show-more), Bot-API-HTML translation (§5). First page shows 15
// up-next tracks; a [Show {count} more] button reveals the rest.
inline const int kQueuePageSize = 15;

inline std::string queueText(const CardTrack& now,
                             const std::vector<CardTrack>& up_next,
                             size_t total_up_next) {
    std::string out = "<h2>Current Queue</h2>\n\n";
    out += "<b>Now Playing</b>\n";
    out += trackLink(now) + "\n";
    out += now.duration + "  •  " + now.requester + "\n";
    out += "\n<b>Up Next • " + std::to_string(total_up_next) +
           " tracks</b>\n";
    if (up_next.empty()) {
        out += "<i>No more songs in queue.</i>";
        return out;
    }
    int i = 0;
    for (const auto& t : up_next) {
        out += std::to_string(++i) + ". " + trackLink(t) + " — " +
               t.duration + " — " + t.requester + "\n";
    }
    return out;
}

inline InlineKeyboard queueKeyboard(int64_t chat_id,
                                    const std::vector<CardTrack>& shown,
                                    bool has_more, size_t remaining) {
    using B = InlineButton;
    InlineKeyboard kb;
    for (const auto& t : shown) {
        if (t.token.empty()) continue;
        kb.push_back(
            {B::callback("⨯ " + truncUtf8(t.title, 30),
                         cbQueueDel(chat_id, t.token))});
    }
    if (has_more) {
        kb.push_back({B::callback("Show " + std::to_string(remaining) +
                                      " more",
                                  cbQueueMore(chat_id))});
    }
    return kb;
}

inline const char* kQueueEmpty =
    "<b>No active playback.</b>\n"
    "\n"
    "<i>Nothing is queued right now.</i>";
inline const char* kQueueAlreadyEmpty =
    "<b>The queue is already empty.</b>";
inline const char* kToastQueueRemoved = "Removed from queue.";

// Queue ops replies (NoxBeats wins, corrected English, §5).
inline std::string queueClearedText(int count, const std::string& by) {
    return fmt("Cleared <b>{count}</b> queued tracks.\n\n» By {by}",
               {{"count", std::to_string(count)}, {"by", by}});
}
inline std::string queueRestoredText(const std::string& by) {
    return fmt("Queue restored.\n\n» By {by}", {{"by", by}});
}
inline const char* kQueueRestoreNone = "» There is no cleared queue to restore.";
inline const char* kRemoveUsage =
    "» Usage: <code>/remove [position]</code> (position as shown in "
    "/queue).";
inline const char* kRemoveBadPos = "» That position is not in the queue.";
inline std::string queueRemovedText(int pos, const std::string& title,
                                    const std::string& by) {
    return fmt("Removed #{pos}: <b>{title}</b>\n\n» By {by}",
               {{"pos", std::to_string(pos)},
                {"title", title},
                {"by", by}});
}
inline std::string queueRemovedManyText(int count, const std::string& by) {
    return fmt("Removed <b>{count}</b> queued tracks.\n\n» By {by}",
               {{"count", std::to_string(count)}, {"by", by}});
}
inline const char* kMoveUsage = "Usage: <code>/move [from] [to]</code>";
inline std::string queueMovedText(int from, int to, const std::string& by) {
    return fmt("Moved #{from} to #{to}.\n\n» By {by}",
               {{"from", std::to_string(from)},
                {"to", std::to_string(to)},
                {"by", by}});
}
inline const char* kShuffleFew =
    "» Need at least two waiting tracks to shuffle.";
inline std::string queueShuffledText(int count, const std::string& by) {
    return fmt("Shuffled <b>{count}</b> queued tracks.\n\n» By {by}",
               {{"count", std::to_string(count)}, {"by", by}});
}

// /queue help block (openradiobot §2.3 queue ops, §5) — appended to the
// /queue help topic.
inline const char* kQueueHelpOps =
    "<b>Queue Managing</b>\n"
    "Usage: <code>/queue [method] [argument]</code>\n"
    "\n"
    "Available methods:\n"
    "  remove  - Remove a song in a queue\n"
    "  move    - Move a song in a queue\n"
    "  shuffle - Sort queue into random order";

// --- §6 lyrics --------------------------------------------------------------
inline const char* kLyricsNotStreaming = "<b>Not streaming.</b>";
inline const char* kLyricsNotFound = "<b>Lyrics not found.</b>";
inline const char* kLyricsCard = "<b>{title}</b> #lyrics\n\n{lyrics}";
inline const char* kLyricsFail =
    "Failed to get lyrics — the song may not exist or may be instrumental";
inline const char* kLyricsNoResults = "No results found";

// --- §7 search results ------------------------------------------------------
// Merged best-of (TGVCBot + lemonic, §7): header + per-result lines +
// number buttons 1..10, 5 per row; wrong-user tap -> "You aren't
// allowed"; valid tap deletes the search message.
inline std::string searchHeaderText(const std::string& query) {
    return fmt("Search Results for <b>{query}</b>",
               {{"query", htmlEscape(query)}});
}

// One result line: "01 : <b><a href>title</a></b>\n<b>By :</b> artists"
// (+ duration for YouTube) with remix metadata rows (§7).
inline std::string searchResultLine(int n, const std::string& title,
                                    const std::string& url,
                                    const std::string& artists,
                                    const std::string& duration,
                                    const std::string& views,
                                    const std::string& upload_time,
                                    const std::string& uploader,
                                    bool is_live) {
    std::string num = n < 10 ? "0" + std::to_string(n) : std::to_string(n);
    std::string out = num + " : <b><a href=\"" + url + "\">" + title +
                      "</a></b>";
    if (!duration.empty()) out += " (" + duration + ")";
    out += "\n<b>By :</b> " + artists;
    std::string dur = is_live ? "Live" : (duration.empty() ? "N/A" : duration);
    out += "\n  ├ <b>Duration:</b> " + dur;
    out += "\n  ├ <b>Views:</b> " + (views.empty() ? "N/A" : views);
    out += "\n  ├ <b>Uploaded:</b> " + (upload_time.empty() ? "N/A" : upload_time);
    out += "\n  └ <b>Uploader:</b> " + (uploader.empty() ? "N/A" : uploader);
    return out;
}

inline const char* kSearchFooter =
    "<i>Reply the number of the result you want to stream or /cancel.</i>";

inline InlineKeyboard searchKeyboard(
    const std::vector<std::pair<int, std::string>>& picks) {
    using B = InlineButton;
    InlineKeyboard kb;
    std::vector<B> row;
    for (const auto& [n, data] : picks) {
        row.push_back(B::callback(std::to_string(n), data));
        if (row.size() == 5) {
            kb.push_back(std::move(row));
            row.clear();
        }
    }
    if (!row.empty()) kb.push_back(std::move(row));
    return kb;
}

inline InlineKeyboard searchPageKeyboard(int64_t user_id, int page,
                                         const std::string& hash,
                                         bool has_prev, bool has_next,
                                         int total_pages) {
    using B = InlineButton;
    std::vector<B> row;
    if (has_prev)
        row.push_back(B::callback(
            "Previous page",
            cbSearchPage(user_id, page - 1, hash)));
    if (has_next)
        row.push_back(B::callback(
            "Next page", cbSearchPage(user_id, page + 1, hash)));
    if (row.empty()) return {};
    (void)total_pages;
    return {std::move(row)};
}

inline const char* kSearchNoQuery =
    "<b>What do you want to search for?</b>";
inline const char* kSearchNoResults = "<b>No results found.</b>";
inline const char* kSearchExpired = "Search expired, please search again";
inline std::string searchTooFrequentText(int seconds) {
    return "Too frequent, please try again in " + std::to_string(seconds) +
           " seconds";
}
inline const char* kSearchNoMore = "No more results.";
inline const char* kSearchSuccessToast = "Success";

// /search help text (lemonic §6.2, typo corrected, §7).

// /play search flow lifecycle (NoxBeats/Tina/Yukki/Arc/Parvati, §7).

inline const char* kNoSongQuery =
    "<b>Whoops! Nothing to play.</b>\n"
    "\n"
    "<i>Send <b>{cmd} [song name or link]</b>, or reply to a <b>media</b> "
    "message to start the music.</i>";
inline std::string playTooLongText(int minutes) {
    return fmt("Sorry, song exceeds max duration of {n} minutes.",
               {{"n", std::to_string(minutes)}});
}

// --- §8 download progress ---------------------------------------------------
// Live progress message (Parvati fields + navidrome checklist, §8),
// edited in place. {done}/{total} byte line is added when known.

inline const char* kDlCancelBtn = "Cancel Downloading";
inline const char* kDlUsage = "Usage: /download &lt;song title or link&gt;";
inline const char* kDlFetching = "Fetching song info...";
inline std::string dlFailedText(const std::string& error) {
    return fmt("Download failed: {error}",
               {{"error", htmlEscape(error)}});
}
inline const char* kDlSuccess = "Song downloaded successfully!";

// Lossy-fallback prompt (navidrome §7.6.7, §8).
inline const char* kLossyPrompt =
    "FLAC not found for {artist} — {title}\n"
    "\n"
    "Lossy fallback available: {summary}";
inline InlineKeyboard lossyKeyboard(const std::string& id,
                                    const std::string& summary) {
    using B = InlineButton;
    return {{B::callback("Accept " + summary, cbLossy("accept", id)),
             B::callback("Skip", cbLossy("skip", id)),
             B::callback("Cancel", cbDlCancel(id))}};
}
inline const char* kToastLossyExpired = "Prompt expired.";
inline const char* kToastLossySkipped = "Skipped.";
inline std::string lossySkippedText(const std::string& artist,
                                    const std::string& title) {
    return fmt("Skipped — FLAC not available for {artist} — {title}",
               {{"artist", artist}, {"title", title}});
}

// Cancel final tallies (navidrome §7.6.2, §8).
inline const char* kToastDlNothingToCancel =
    "Nothing to cancel — already finished.";
inline const char* kToastDlCancelled = "Download cancelled.";
inline const char* kToastDlWrongChat =
    "Only the chat that requested this can cancel it.";

// Spotify download option picker (Spotify-Loader §5.3, §8).
inline const char* kDlSpotifyOptions = "Select Options Below!";
inline InlineKeyboard dlSpotifyOptionsKeyboard(const std::string& run_id) {
    using B = InlineButton;
    return {{B::callback("Send as ZIP", "pl:dl:zip:" + run_id),
             B::callback("Send one by one", "pl:dl:1by1:" + run_id)}};
}
inline std::string dlSpotifySuccessText(int count, const std::string& kind) {
    return fmt("Successfully uploaded {x} from a Spotify {kind}",
               {{"x", std::to_string(count)}, {"kind", kind}});
}

// Audio-effect picker (tmb-rust §4.4, §8).
inline const char* kDlFxPrompt =
    "<b>Track:</b> {title}\n\nChoose a processing option:";
inline InlineKeyboard dlFxKeyboard(const std::string& run_id) {
    using B = InlineButton;
    return {
        {B::callback("Original", cbDlFx(run_id, "orig")),
         B::callback("Slow + Reverb", cbDlFx(run_id, "slow"))},
        {B::callback("Nightcore", cbDlFx(run_id, "nightcore")),
         B::callback("Bass Boost", cbDlFx(run_id, "bass"))},
        {B::callback("8D Audio", cbDlFx(run_id, "8d"))},
    };
}
inline const char* kDlFxApplying = "Downloading and applying effect...";

// Download-delivery audio caption (Music163bot §6.3, §4/§8).
inline const char* kAudioCaption =
    "「{name}」- {artists}\n"
    "Album: {album}\n"
    "#{ext} {sizeMB:.2f}MB {kbps:.2f}kbps\n"
    "via @{bot}";

// --- §9 settings menus ------------------------------------------------------
// Per-chat settings snapshot the panel renders from. The dispatcher
// fills it from the backend (chat.settings, all 15 keys — Q7 rewire,
// Phase 6); toggles write back. autoplay_lang/autoplay_mood are ""
// (any) on the wire; the /autoplay status text displays them as
// "auto"/"any".
struct ChatSettings {
    int admin_mode = 0;  // 0 admins+auth, 1 admins only, 2 everyone
    std::string play_mode = "everyone";
    bool cmd_delete = false;
    int clean_delay_min = 0;  // 0 = off; the clean delay cycles 15→30→60→5
    bool thumbnails = true;
    bool autoplay = false;
    std::string autoplay_lang;   // "" = any language (display "auto")
    std::string autoplay_mood;   // "" = any mood (display "any")
    bool silent = false;
    bool auto_delete = false;
    int auto_delete_min = 5;  // valid 2–7
    std::string language = "en";
    bool paginate_purge_old = true;
    bool chat_music_download = true;
};

inline const char* kSettingsText =
    "<h2>Chat Settings</h2>\n"
    "\n"
    "<b>Group:</b> {title}\n"
    "<b>Group ID:</b> <code>{id}</code>\n"
    "\n"
    "Configure the bot's behavior in this group using the buttons below.";

// Settings panel keyboard (Tina wins, 9 rows + TgMusicBot rows 10–12,
// §9). Label buttons -> pl:set:info:<key> (explainer alert), value
// buttons -> pl:set:<key> (toggle/cycle + re-edit in place).
// Forward: the settings index (Pagination + Music Download sub-screens,
// §9) is defined below; the panel appends its row.
inline InlineKeyboard settingsIndexKeyboard();

inline InlineKeyboard settingsKeyboard(const ChatSettings& s) {
    using B = InlineButton;
    auto on_off = [](bool v) { return v ? "Enabled" : "Disabled"; };
    std::string admin_mode = s.admin_mode == 1
                                 ? "Admins"
                                 : s.admin_mode == 2 ? "Everyone"
                                                     : "Auth Users";
    std::string play_mode =
        s.play_mode == "admins" ? "Admins" : "Everyone";
    std::string delay = s.clean_delay_min == 0
                            ? "15m"
                            : std::to_string(s.clean_delay_min) + "m";
    std::string timer_label = "Delete after: " +
                              std::to_string(s.auto_delete_min) +
                              " minutes";
    InlineKeyboard kb = {
        {B::callback("Admin Mode", cbSetInfo("adminmode")),
         B::callback(admin_mode, cbSet("adminmode"))},
        {B::callback("Play Mode", cbSetInfo("playmode")),
         B::callback(play_mode, cbSet("playmode"))},
        {B::callback("Cmd Delete", cbSetInfo("cmddelete")),
         B::callback(on_off(s.cmd_delete), cbSet("cmddelete"))},
        {B::callback("Clean Mode", cbSetInfo("cleanmode")),
         B::callback(on_off(s.clean_delay_min > 0), cbSet("cleanmode"))},
        {B::callback("Clean Delay", cbSetInfo("cleanduration")),
         B::callback(delay, cbSet("cleanduration"))},
        {B::callback("Thumbnails", cbSetInfo("nothumb")),
         B::callback(on_off(s.thumbnails), cbSet("nothumb"))},
        {B::callback("Autoplay", cbSetInfo("autoplay")),
         B::callback(on_off(s.autoplay), cbSet("autoplay"))},
        {B::callback("Language", cbSetInfo("lang")),
         B::callback(s.language, kCbLangSelect)},
        {B::callback(std::string("Auto-delete: ") + on_off(s.auto_delete),
                     cbSet("autodelete"))},
        {B::callback(timer_label, cbSetInfo("timer"))},
        {B::callback(std::string("Silent mode: ") + on_off(s.silent), cbSet("silent"))},
        {B::callback("Close", kCbClose)},
    };
    // Timer buttons 2–7 min (TgMusicBot §5.7): current value marked.
    std::vector<B> timers;
    for (int n = 2; n <= 7; ++n) {
        timers.push_back(B::callback(
            (n == s.auto_delete_min ? std::to_string(n) + " (current)"
                                  : std::to_string(n)),
            cbSetTimer(n)));
    }
    kb.insert(kb.begin() + 10, std::move(timers));
    // Pagination + chat-music-download sub-screens (IPC-6 keys, §9):
    // their buttons live in the settings index row, kept just above Close.
    auto index_row = settingsIndexKeyboard();
    kb.insert(kb.end() - 1, index_row.begin(), index_row.end());
    return kb;
}

// Setting explainers (pl:set:info:<key> alerts, §9).
inline std::string settingInfoText(const std::string& key) {
    if (key == "adminmode")
        return "Admin Mode controls who can manage playback: Admins only, "
               "Admins + authorized users, or Everyone.";
    if (key == "playmode")
        return "Play Mode controls who can use /play: Admins only, or "
               "Everyone.";
    if (key == "cmddelete")
        return "Cmd Delete removes command messages after they run, keeping "
               "the chat clean.";
    if (key == "cleanmode")
        return "Clean Mode auto-deletes the bot's messages after the clean "
               "delay.";
    if (key == "cleanduration")
        return "Clean Delay is how long bot messages stay before Clean Mode "
               "deletes them. Tap to cycle 15 → 30 → 60 → 5 minutes.";
    if (key == "nothumb")
        return "Thumbnails toggle album art on now-playing and queue cards.";
    if (key == "autoplay")
        return "Autoplay keeps playing recommended songs similar to the last "
               "played track once the queue ends.";
    if (key == "silent")
        return "Silent mode hides the final success message of playback "
               "commands (progress and errors stay visible).";
    if (key == "autodelete")
        return "Auto-delete removes the bot's playback messages after the "
               "delete-after timer.";
    if (key == "timer")
        return "Delete-after timer: how many minutes playback messages stay "
               "before auto-delete removes them (2–7 minutes).";
    if (key == "lang")
        return "Language of the bot's replies. Parlay ships English only "
               "today.";
    if (key == "page")
        return "Pagination: previous messages are deleted during pagination "
               "to ease chat scrolling.";
    if (key == "musicdl")
        return "Download music via chat: download music files directly from "
               "chat (max 200 MB).";
    return "";
}

inline const char* kSettingsUpdated = "Settings updated!";
inline const char* kSettingsSaveFailed =
    "Could not save settings — the backend is unreachable. Nothing "
    "was changed.";
inline const char* kSettingsGroupOnly =
    "These settings are available only in groups.";

// Autoplay card (Tina wins, §9).
inline std::string autoplayCardText(bool enabled) {
    std::string out =
        "<h2>Autoplay Mode Settings</h2>\n\n• <b>Current Status:</b> ";
    out += enabled ? "<b>Enabled (ON)</b>" : "<b>Disabled (OFF)</b>";
    out += "\n\n<i>When the current queue finishes, the bot will <b>";
    out += enabled
               ? "automatically discover and stream related songs</b> "
                 "seamlessly."
               : "stop automatically</b>.";
    out += "</i>\n\n<i>Click the button below to toggle the autoplay "
           "state:</i>";
    return out;
}
inline InlineKeyboard autoplayCardKeyboard(bool enabled) {
    using B = InlineButton;
    return {{B::callback(enabled ? "Turn OFF" : "Turn ON",
                         cbSet("autoplay")),
             B::callback("Close", kCbClose)}};
}
inline std::string autoplayStatusText(bool on, const std::string& lang,
                                      const std::string& mood) {
    return fmt(" <b>Autoplay:</b> {on}\n<b>Language:</b> <code>{lang}</code>\n"
               "<b>Mood:</b> <code>{mood}</code>",
               {{"on", on ? "on" : "off"}, {"lang", lang}, {"mood", mood}});
}

// Language picker (merged, English-only today, §9).
inline const char* kLangPicker = "<h2>Select Bot Language</h2>";
inline InlineKeyboard langPickerKeyboard(const std::string& origin) {
    using B = InlineButton;
    std::string back = kCbStart;
    if (origin == "help") back = kCbHelpMain;
    if (origin == "settings") back = kCbSetMain;
    return {
        {B::callback("English", std::string(kCbLangEn) + ":" + origin)},
        {B::callback("Back", back)},
    };
}
inline std::string langUpdatedText(const std::string& lang_name) {
    return fmt("Language successfully updated to {lang}.",
               {{"lang", lang_name}});
}
inline const char* kLangAlready = "You're already using this language.";

// Command-form settings (§9).
inline const char* kAdminModeInvalid =
    "Invalid mode. Use: <code>admin</code>, <code>adminauth</code>, or "
    "<code>everyone</code>.";
inline std::string adminModeUpdatedText(const std::string& status) {
    return fmt("<b>Admin command mode</b> updated to: {status}",
               {{"status", status}});
}
inline const char* kPlayModeAdmin =
    "<b>Play mode</b> is now restricted to <b>Admins &amp; DJs</b> only.";
inline const char* kPlayModeEveryone =
    "<b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow!";
inline const char* kCleanModeStatus =
    "<h2>Clean mode</h2>\n"
    "\n"
    "Auto-cleanup of command and service messages.\n"
    "Current state: {action}\n"
    "Delete after: {duration}m";
inline const char* kLoggerNeedId = "Please set LOGGER_ID in .env first.";
inline const char* kLoggerUsage =
    "Usage: /logger [enable|disable|on|off]\nCurrent status: {status}";
inline const char* kLoggerEnabled = "Logger Enabled";
inline const char* kLoggerDisabled = "Logger disabled";
inline const char* kThumbStatus =
    "<b>Thumbnail:</b> {on}\n\n<code>/thumbnail on</code> • "
    "<code>/thumbnail off</code>";

// /download-settings (Spotify-Loader §5.4, §9) — per-user download prefs.
inline const char* kDlSettingsIndex = "Select one of the settings below!";
inline InlineKeyboard dlSettingsIndexKeyboard() {
    using B = InlineButton;
    return {{B::callback("Output Format", "pl:dlset:of"),
             B::callback("Use Youtube", "pl:dlset:uy"),
             B::callback("Path Template", "pl:dlset:pt")}};
}
inline const char* kDlSettingsFormat =
    "Current Output Format: <code>{format}</code>\n"
    "\n"
    "Select one of the buttons below to change your output format.";
inline InlineKeyboard dlSettingsFormatKeyboard() {
    using B = InlineButton;
    return {
        {B::callback("MP3", "pl:dlset:fmt:mp3"),
         B::callback("M4A", "pl:dlset:fmt:m4a")},
        {B::callback("FLAC", "pl:dlset:fmt:flac"),
         B::callback("OPUS", "pl:dlset:fmt:opus")},
        {B::callback("OGG", "pl:dlset:fmt:ogg"),
         B::callback("WAV", "pl:dlset:fmt:wav")},
        {B::callback("Back", "pl:dlset:back")},
    };
}
inline const char* kDlSettingsYoutube =
    "Is Using Youtube: <code>{value}</code>";
inline InlineKeyboard dlSettingsYoutubeKeyboard() {
    using B = InlineButton;
    return {{B::callback("True", "pl:dlset:uy:1"),
             B::callback("False", "pl:dlset:uy:0")},
            {B::callback("Back", "pl:dlset:back")}};
}
inline const char* kDlSettingsTemplate =
    "Current Path Template: <code>{template}</code>";
inline const char* kDlSettingsTemplatePrompt =
    "Send your custom path template.\n"
    "\n"
    "Possible values:\n"
    "{artist}\n"
    "{artists}\n"
    "{title}\n"
    "{album}\n"
    "{ext}\n"
    "{playlist}\n"
    "\n"
    "Example: <code>{artists}/{album}/{title} - {artist}.{ext}</code>\n"
    "Note: \"/\" is used for making new directories.";

// beetube settings screens (§9): pagination + music download toggles.

inline InlineKeyboard settingsIndexKeyboard() {
    using B = InlineButton;
    return {{B::callback("Pagination", cbSet("page")),
             B::callback("Music Download", cbSet("musicdl")),
             B::callback("Quality", cbSet("quality"))}};
}
inline const char* kSettingsPageScreen =
    "<h2>Auto delete old messages</h2>\n"
    "By default previous messages are deleted during pagination to ease "
    "chat scrolling, use this section to change this behaviour.";
inline const char* kSettingsMusicDlScreen =
    "<h2>Download music via chat</h2>\n"
    "Enabling this will allow you to download music directly from chat.\n"
    "<b>Note:</b> The actual size is compressed and you can't download a "
    "music file of more than <em>200MB</em> via chat.\n"
    "<b>Supported formats</b>\n"
    "- MP3\n"
    "- FLAC\n"
    "- WMA\n"
    "- WAV\n"
    "- OGG\n"
    "- AIFF\n"
    "- ALAC";
inline InlineKeyboard settingsToggleKeyboard(const std::string& key,
                                             bool enabled) {
    using B = InlineButton;
    return {{B::callback(enabled ? "Enabled" : "Disabled",
                         cbSet(key + ":" + (enabled ? "0" : "1"))),
             B::callback("Back", kCbSetMain)}};
}

// --- Opus quality (quality.get surface, 2026-11-05) -----------------------
// Effective Opus encoder config for one chat, as quality.get reports
// it: the effective values, the winning level per field, and the
// library's stored config (what the next encoder will actually use).
// Rendered as a rich message (§7.1): heading + field lines + the
// next-join note. No emojis (§7.3). Every quality change applies on
// the NEXT join — the library cannot reconfigure a running encoder,
// so the card never promises a live change.
struct QualityValues {
    int bitrate_bps = 192000;
    int frame_ms = 20;
    int complexity = 10;
    bool vbr = true;
    bool fec = false;
    bool dtx = false;
    int max_playback_rate_hz = 48000;
};
struct QualityInfo {
    QualityValues effective;
    QualityValues library;
    // Winning level per field ("store" | "config" | "profile" |
    // "default"), keyed by the wire names (bitrate_bps, frame_ms,
    // complexity, vbr, fec, dtx).
    std::map<std::string, std::string> sources;
    bool active_call = false;
    std::string applies_on = "next join";
};

// Human label for a quality.get source level.
inline std::string qualitySourceLabel(const std::string& source) {
    if (source == "store") return "chat setting";
    if (source == "config") return "config file";
    if (source == "profile") return "voice profile";
    if (source == "default") return "default";
    return htmlEscape(source);
}

inline std::string qualityCardText(const QualityInfo& q) {
    auto src = [&](const char* field) {
        auto it = q.sources.find(field);
        return qualitySourceLabel(it == q.sources.end() ? "default"
                                                        : it->second);
    };
    auto on_off = [](bool v) { return v ? "on" : "off"; };
    auto bitrate = [](int bps) {
        if (bps % 1000 == 0) return std::to_string(bps / 1000) + " kbps";
        return std::to_string(bps) + " bps";
    };
    std::string out = "<h2>Audio Quality</h2>\n\n";
    out += "<b>Bitrate:</b> " + bitrate(q.effective.bitrate_bps) + " (" +
           src("bitrate_bps") + ")\n";
    out += "<b>Frame size:</b> " + std::to_string(q.effective.frame_ms) +
           " ms (" + src("frame_ms") + ")\n";
    out += "<b>Complexity:</b> " + std::to_string(q.effective.complexity) +
           " (" + src("complexity") + ")\n";
    out += "<b>VBR:</b> " + std::string(on_off(q.effective.vbr)) + " (" +
           src("vbr") + ")\n";
    out += "<b>FEC:</b> " + std::string(on_off(q.effective.fec)) + " (" +
           src("fec") + ")\n";
    out += "<b>DTX:</b> " + std::string(on_off(q.effective.dtx)) + " (" +
           src("dtx") + ")\n";
    // Library readback: the values the next encoder will actually use.
    // In steady state they equal the effective values; when they differ
    // (a change landed after the last join) say so, honestly.
    bool lib_differs =
        q.library.bitrate_bps != q.effective.bitrate_bps ||
        q.library.frame_ms != q.effective.frame_ms ||
        q.library.complexity != q.effective.complexity ||
        q.library.vbr != q.effective.vbr ||
        q.library.fec != q.effective.fec ||
        q.library.dtx != q.effective.dtx;
    if (lib_differs) {
        out += "\n<i>Note: the encoder library currently holds " +
               bitrate(q.library.bitrate_bps) + " / " +
               std::to_string(q.library.frame_ms) +
               " ms; the values above take effect on next join.</i>\n";
    }
    if (q.active_call) {
        out += "\n<i>A call is live in this chat right now.</i>\n";
    }
    out += "\n<i>Changes apply on next join — a running call keeps its "
           "current encoder settings.</i>";
    return out;
}

inline std::string cbSetQualityBr(int kbps) {
    return "pl:set:quality:br:" + std::to_string(kbps);
}
inline std::string cbSetQualityFr(int ms) {
    return "pl:set:quality:fr:" + std::to_string(ms);
}

// Quality sub-screen keyboard (routing rows A97–A99): bitrate and
// frame-size pickers with the current value marked, a reset that
// clears the per-chat overrides, and Back to the settings panel.
inline InlineKeyboard settingsQualityKeyboard(const QualityInfo& q) {
    using B = InlineButton;
    std::vector<B> bitrates;
    for (int kbps : {128, 160, 192, 256, 320}) {
        std::string label = std::to_string(kbps) + " kbps";
        if (q.effective.bitrate_bps == kbps * 1000) label += " (current)";
        bitrates.push_back(B::callback(label, cbSetQualityBr(kbps)));
    }
    std::vector<B> frames;
    for (int ms : {10, 20, 40, 60}) {
        std::string label = std::to_string(ms) + " ms";
        if (q.effective.frame_ms == ms) label += " (current)";
        frames.push_back(B::callback(label, cbSetQualityFr(ms)));
    }
    return {bitrates,
            frames,
            {B::callback("Reset to config file", cbSet("quality:reset"))},
            {B::callback("Back", kCbSetMain)}};
}

// Back-only keyboard for the quality-load failure screen: the user
// always has a way back to the settings panel (no dead ends, §7.5).
inline InlineKeyboard qualityBackKeyboard() {
    using B = InlineButton;
    return {{B::callback("Back", kCbSetMain)}};
}

inline const char* kQualityLoadFailed =
    "Could not load the audio quality settings — the backend is "
    "unreachable. Nothing was changed.";

// --- §10 admin/sudo panels ---------------------------------------------------
// Permission denials (Yukki §3.9 wins, §10).
inline const char* kDenyAdmin =
    "<h2>Access Denied</h2>\n"
    "<i>This action is limited to <b>chat admins</b>.</i>\n"
    "\n"
    "You can still <code>/play</code> songs and enjoy the music.\n"
    "\n"
    "<i>Admin? Run <code>/reload</code> to refresh permissions.</i>";
inline const char* kDenyAdminCb = "Only admins can do these actions.";
inline const char* kEngineOnlyAdmin =
    "Only group admins (or the bot owner) can switch engines.";
inline std::string engineCurrentText(const std::string& engine) {
    return fmt("Current VC engine for this chat: <b>{e}</b>\n"
               "Switch with /engine go|ts|rust",
               {{"e", htmlEscape(engine)}});
}
inline std::string engineUnknownText(const std::string& arg) {
    return fmt("Unknown engine \"{a}\". Use go, ts or rust.",
               {{"a", htmlEscape(arg)}});
}
inline std::string engineSetText(const std::string& engine,
                                 const std::string& fb1,
                                 const std::string& fb2) {
    return fmt("VC engine for this chat is now <b>{e}</b> "
               "(fallback: {f1} → {f2}).",
               {{"e", htmlEscape(engine)},
                {"f1", htmlEscape(fb1)},
                {"f2", htmlEscape(fb2)}});
}
inline const char* kDenyAdminOrAuth =
    "<h2>Access Denied</h2>\n"
    "<i>This action is limited to <b>chat admins and authorized "
    "users</b>.</i>\n"
    "\n"
    "You can still <code>/play</code> songs and enjoy the music.";
inline const char* kDenyOwner = "Only the bot owner can use this command.";

// Active streams (NoxBeats §1.7 + Tina/Yukki §2.7/§3.7, §10).
inline const char* kActiveStreamsHeader = "<u><b>List of active streams:</b></u>";
inline const char* kActiveStreamsEmpty =
    "» Looks like there aren't any active streams on the bot.";
inline std::string activeChatsText(int count) {
    return fmt("<b>Active Chats:</b>\n\nThere are <i>{n}</i> active chats "
               "across the bot system.",
               {{"n", std::to_string(count)}});
}

// /reload (Parvati/Yukki/Tina + TgMusicBot, §10).

inline const char* kReloadShortOk =
    "Administrator cache reloaded successfully.";
inline std::string reloadCooldownText(const std::string& mmss) {
    return fmt("Please wait {t} before using this command again.",
               {{"t", mmss}});
}

// /logs + logger channel (NoxBeats §1.9, Parvati/Yukki §3.8, Arc §4.9, §10).

inline const char* kLogsMissing = "» Log file doesn't exist.";
inline const char* kLogsNone = "No logs found";

// /eval + /sh (NoxBeats §1.9, §10).
inline const char* kEvalError = "Error executing snippet\n\n";
inline const char* kEvalNoInput = "What?";
inline std::string evalOutputText(const std::string& out) {
    return fmt("<b>Output:</b>\n<code>{out}</code>", {{"out", out}});
}

// /speedtest result (Tina staged progress + NoxBeats form, §10).
inline const char* kSpeedtestDl = "Running download test...";
inline const char* kSpeedtestUp = "Running upload test...";
inline const char* kSpeedtestFailed = "» The speed test failed.";

// /bug (NoxBeats/Tina/Parvati, §10).
inline const char* kBugUsage =
    "Please describe the bug. Usage: {cmd} <problem> or reply to a "
    "message.";
inline const char* kBugThanks =
    "Thank you for your bug report! It has been sent to the developers.";
inline const char* kBugReport =
    "<b>Bug report</b>\n\nFrom: {from} (<code>{id}</code>)\nWhere: {where}\n\n{text}";
inline const char* kBugFailed = "» Could not send the report. Try again later.";

// /privacy (Yukki §3.8 full page, §10; bot name + links substituted).
inline const char* kPrivacy =
    "<h1>Privacy Policy &amp; Data Handling</h1>\n"
    "\n"
    "Parlay is built with privacy in mind. Below is exactly what it "
    "stores, why it is stored, and how you stay in control.\n"
    "\n"
    "<h2>Data We Store</h2>\n"
    "Only the information required to run the bot — nothing more:\n"
    "• <b>User &amp; chat IDs</b> — Identify groups and users so your "
    "settings apply\n"
    "• <b>Preferences</b> — Language, play mode, clean mode, admin mode, "
    "thumbnails\n"
    "• <b>Access lists</b> — Authorized users, sudo users, and the "
    "blacklist\n"
    "\n"
    "<h2>Messages</h2>\n"
    "The bot reads <b>only</b> messages that begin with one of its "
    "commands (for example <code>/play</code>) or button presses on its "
    "own messages. It <b>never</b> reads, stores, or monitors your private "
    "chats or ordinary group conversations.\n"
    "\n"
    "<h2>Third-Party Services</h2>\n"
    "To find and stream the music you request, the bot sends the search "
    "query to services such as <b>YouTube</b> and <b>Spotify</b>. Only the "
    "query itself is sent — <b>no personal data</b> is shared with these "
    "services.\n"
    "\n"
    "<h2>Data Sharing</h2>\n"
    "Your data is <b>never</b> sold, shared, or traded with third parties. "
    "It is used strictly to operate and improve the bot's music streaming "
    "features.\n"
    "\n"
    "<h2>Data Removal</h2>\n"
    "Access can be cleaned up at any time: the owner can manage authorized "
    "users, sudo users, and the blacklist. You may also contact support to "
    "request removal of stored data.\n"
    "\n"
    "<i>Parlay is an open-source project. If you have any questions, feel "
    "free to join our Support Chat.</i>";

// Assistant errors (Tina/Yukki/Parvati err_* family, §10).

inline std::string assistantRestrictedText(const std::string& assistant,
                                          int64_t id) {
    return fmt("<h2>Assistant Restricted</h2>\n"
               "\n"
               "The assistant {name} (ID: <code>{id}</code>) has been "
               "<b>banned</b> or restricted in this chat.\n"
               "I cannot play music, manage the queue, or clean up tracks "
               "while this continues.\n"
               "\n"
               "<i>Unban the assistant to restore all music features</i>",
               {{"name", assistant}, {"id", std::to_string(id)}});
}

// /userbot (NoxBeats §1.7, §10).

inline std::string userbotAlreadyText(const std::string& name) {
    return fmt("» {name} is already in this chat.", {{"name", name}});
}

// --- §11 auth lists ----------------------------------------------------------
inline std::string authlistText(const std::vector<std::pair<std::string, int64_t>>& users,
                                int limit) {
    std::string out = fmt("<h2>Authorized Users</h2>\n\n{count}/{limit} "
                          "authorized\n\n",
                          {{"count", std::to_string(users.size())},
                           {"limit", std::to_string(limit)}});
    int i = 0;
    for (const auto& [name, id] : users) {
        out += std::to_string(++i) + ". " + name + " (<code>" +
               std::to_string(id) + "</code>)\n";
    }
    out += "\n<i>Tap ⨯ to revoke access.</i>";
    return out;
}
inline InlineKeyboard authlistKeyboard(
    int64_t chat_id,
    const std::vector<std::pair<std::string, int64_t>>& users) {
    using B = InlineButton;
    InlineKeyboard kb;
    for (const auto& [name, id] : users) {
        kb.push_back({B::callback("⨯ " + truncUtf8(name, 30),
                                  cbAuthDel(chat_id, id))});
    }
    return kb;
}
inline const char* kAuthlistEmpty =
    "There are no authorized users in this chat.";
inline const char* kToastAuthRevoked = "Authorization revoked.";
inline const char* kAuthNoUser =
    "Please provide a user — use:\n{cmd} [user_id] or reply to a user's "
    "message.";
inline std::string authLimitText(int limit) {
    return fmt("This chat has reached the maximum authorized users limit "
               "({limit}). Please remove someone before adding a new one.",
               {{"limit", std::to_string(limit)}});
}
inline const char* kAuthAlready =
    "That user is already authorized — no need to add them again.";
inline const char* kAuthCannot =
    "This user cannot be authorized.\nThe bot owner, the bot itself, or "
    "yourself do not require authorization.";
inline const char* kAuthIsAdmin =
    "This user is already a chat administrator. No authorization is "
    "required.";
inline std::string authAddedText(const std::string& user, int count,
                                 int limit) {
    return fmt("Added {user}.\nNow {count}/{limit} authorized users.",
               {{"user", user},
                {"count", std::to_string(count)},
                {"limit", std::to_string(limit)}});
}
inline std::string authRemovedText(const std::string& user) {
    return fmt("Removed {user} from the authorized users list.",
               {{"user", user}});
}

// Sudo list (Tina/Yukki/Parvati, §11).
inline const char* kSudoHeader = "<h2>Current Sudoers</h2>";
inline const char* kSudoNoExtra = "No additional sudoers found.";
inline const char* kSudoSelfJoke =
    "Haha, you're the boss already! Why are you even trying to add "
    "yourself?";
inline const char* kSudoOwnerRemoveBlock =
    "Nice try! You can't remove the owner — they're untouchable.";

// Blacklist (Tina/Yukki/Parvati, §11).
inline const char* kBlacklistTitle = "<h2>Blacklisted</h2>";
inline std::string blBlockedUserText(int64_t id) {
    return fmt("User blocked: <code>{id}</code>",
               {{"id", std::to_string(id)}});
}

// --- §12 error messages -----------------------------------------------------

// Play guards (NoxBeats §1.9, §12).

inline const char* kGuardPlayAdmin =
    "<u><b>Admin only play</b></u>\n\nOnly admins are allowed to play in "
    "this chat.";

// --- §13 inline-mode results -------------------------------------------------
// The core renders every inline result itself from the search.inline
// Tracks (vc-ts /v1/inline returns data only — see phase9/
// port-engines-gaps.md). Modes mirror vc-ts resolvers.js inlineMode:
// np/s/l/help match the whole query; lib/del/search take an argument
// after a space; empty / <3-char queries are the hint. lib/del return
// empty results (no library backend — ruling 1, future work).
// Hint button above an empty list (navidrome §7.9.1).
inline const char* kInlineHintText =
    "np  s  l  lib  del — or type to search";
// Music163bot §6.8 help articles: both send the bot version line.
inline const char* kInlineHelpTitle1 =
    "1. Paste a music share URL or enter a Music ID";
inline const char* kInlineHelpTitle2 =
    "2. Enter search + keyword to search songs";
inline const char* kInlineHelpMessage = "Parlay v0.3.0";
// Bare "search" with no keyword (Music163bot §6.8).
inline const char* kInlineNeedKeyword = "Please enter a keyword";
// A keyword search with no matches (Music163bot §6.8).
inline const char* kInlineNoResults = "No results found";
// Empty query (Music163bot §6.8).
inline const char* kInlineTypeHelp = "Type help for help";
// Inline lyrics article (navidrome §7.9.4): title + description
// prefix; the sent message is "{artist} — {title}\n\n{lyrics}".
inline std::string inlineLyricsTitle(const std::string& title) {
    return fmt("Lyrics: {title}", {{"title", title}});
}
// Per-track button on search results (lemonic §6.7): opens the bot
// with the deep link that downloads the track (/start spotify----{id}).
inline const char* kInlineOpenInParlay = "Open in Parlay Bot";
inline std::string inlineOpenUrl(const std::string& bot_username,
                                 const std::string& track_id) {
    return "https://t.me/" + bot_username + "?start=spotify----" +
           track_id;
}
// The spotify---- deep link names a track the core can no longer
// resolve (inline results are stored per user, but the store may have
// been cleared or the id never came from an inline result).
inline const char* kDlDeepLinkExpired =
    "That track link has expired — search for the song again.";

// --- §14 reply keyboards ----------------------------------------------------

// --- §14 reply keyboards ----------------------------------------------------
// Persistent category keyboard (beetube §3.3, music-only subset, §14).
inline ReplyKeyboard persistentKeyboard() {
    return ReplyKeyboard{
        {{"Music"}, {"Global Search"}, {"Settings", "About"}},
        true, false};
}
inline const char* kKeyboardEnabled = "Keyboard enabled.";

// --- §16 stats / ping / alive ----------------------------------------------
inline std::string pingCardText(const std::string& bot,
                                const std::string& latency_ms,
                                const std::string& uptime,
                                const std::string& ram_info,
                                const std::string& cpu_usage,
                                const std::string& disk_usage,
                                const std::string& ntgcalls_ms) {
    std::string out = fmt("<h2>Pong!</h2> • <code>{lat}ms</code>\n\n<b>"
                          "{bot}</b>\n<blockquote><b>Uptime:</b> "
                          "<code>{up}</code>\n<b>RAM:</b> "
                          "<code>{ram}</code>\n<b>CPU:</b> "
                          "<code>{cpu}</code>\n<b>Disk:</b> "
                          "<code>{disk}</code></blockquote>",
                          {{"lat", latency_ms},
                           {"bot", bot},
                           {"up", uptime},
                           {"ram", ram_info},
                           {"cpu", cpu_usage},
                           {"disk", disk_usage}});
    if (!ntgcalls_ms.empty())
        out += "\n<b>NTgCalls latency:</b> <code>" + ntgcalls_ms + "ms</code>";
    return out;
}

// /stats card (Yukki §3.8 wins, §16). {system}/{memory}/{server}/{bot}
// are pre-built blocks; {library} is the navidrome section (empty when
// no library is present).
inline const char* kStatsCard =
    "<h2>Bot Statistics</h2>\n"
    "\n"
    "<i>Live overview of the host, resources and database.</i>\n"
    "\n"
    "<h2>System</h2>\n"
    "{system}\n"
    "\n"
    "<h2>Memory</h2>\n"
    "{memory}\n"
    "\n"
    "<h2>Server</h2>\n"
    "{server}\n"
    "\n"
    "<h2>Bot Overview</h2>\n"
    "{bot}\n"
    "\n"
    "<h2>Versions</h2>\n"
    "{versions}"
    "{library}";

// /status (Music163bot §6.9, §16).
inline const char* kStatusText =
    "<b>[Statistics]</b>\n"
    "Total cached songs in database: {total}\n"
    "Cached songs in current chat [{chat}]: {chatCount}\n"
    "Cached songs for current user [<a href=\"tg://user?id={userID}\">"
    "{userID}</a>]: {userCount}";

// --- §17 playlists ----------------------------------------------------------
// Personal playlist, single (NoxBeats model, §17).
inline const char* kPlaylistEmpty =
    "» Your playlist is empty. Add songs with <code>/addplaylist [song name "
    "or link]</code>.";
inline const char* kAddPlaylistUsage =
    "» Usage: <code>/addplaylist [song name | YouTube / Spotify / Apple "
    "Music / JioSaavn / SoundCloud link]</code>\nWhile a song is playing, "
    "<code>/addplaylist</code> alone saves that song.";
inline std::string playlistAddedText(const std::string& title, int total) {
    return fmt("» Added <b>{title}</b> to your playlist. You now have "
               "{total} songs.",
               {{"title", title}, {"total", std::to_string(total)}});
}
inline const char* kPlaylistDup = "» That song is already in your playlist.";
inline std::string playlistFullText(int max) {
    return fmt("» Your playlist is full ({max} songs). Remove some with "
               "/rmplaylist.",
               {{"max", std::to_string(max)}});
}
inline const char* kPlaylistUnsupported =
    "» That link type can't be saved to a playlist. Use a song name or a "
    "YouTube, Spotify, Apple Music, JioSaavn or SoundCloud link.";
inline std::string playlistHeaderText(const std::string& owner, int count) {
    return fmt("<b>{owner}'s playlist</b> — {count} songs\n\n",
               {{"owner", owner}, {"count", std::to_string(count)}});
}
inline std::string playlistMoreText(int more) {
    return fmt("» \n…and {n} more.", {{"n", std::to_string(more)}});
}
inline const char* kPlaylistFooter =
    "\n\n/playplaylist to play it (add <code>-s</code> to shuffle, "
    "<code>-v</code> for video)\n/rmplaylist [number | name | all] to remove";
inline const char* kRmPlaylistUsage =
    "Usage: <code>/rmplaylist [number | song name | all]</code>";
inline const char* kRmPlaylistNone =
    "» I couldn't find that song in your playlist. Check /playlist.";
inline std::string playlistRemovedText(const std::string& title, int left) {
    return fmt("» Removed <b>{title}</b> from your playlist. {left} songs "
               "left.",
               {{"title", title}, {"left", std::to_string(left)}});
}
inline std::string playlistClearedText(int count) {
    return fmt("» Playlist cleared ({n} songs removed).",
               {{"n", std::to_string(count)}});
}

// Named playlists (Parvati/TgMusicBot model, §17).
inline const char* kCreatePlaylistUsage =
    "Usage: <code>/createplaylist &lt;name&gt;</code>";
inline const char* kNamedPlaylistLimit =
    "You have reached the maximum limit of 10 playlists.";
inline std::string namedPlaylistCreatedText(const std::string& name,
                                            const std::string& id) {
    return fmt("Playlist <b>{name}</b> has been created successfully.\n"
               "ID: <code>{id}</code>",
               {{"name", name}, {"id", id}});
}
inline std::string namedPlaylistCreateFailText(const std::string& error) {
    return fmt("Failed to create playlist: {error}", {{"error", error}});
}
inline const char* kNamedPlaylistNotFound =
    "The specified playlist could not be found. Please check the "
    "playlist ID.";
inline std::string namedPlaylistDeletedText(const std::string& name) {
    return fmt("Playlist <b>{name}</b> has been deleted successfully.",
               {{"name", name}});
}
inline const char* kNamedPlaylistBadNumber = "Invalid song number.";
inline const char* kNamedPlaylistSongNotFound = "Song not found in playlist.";
inline std::string namedPlaylistAddedText(const std::string& song,
                                          const std::string& playlist) {
    return fmt("Track <b>{song}</b> has been added to playlist "
               "<b>{playlist}</b>.",
               {{"song", song}, {"playlist", playlist}});
}
inline std::string namedPlaylistRemovedText(const std::string& name) {
    return fmt("Song removed from playlist <b>{name}</b>.",
               {{"name", name}});
}
inline const char* kPlaylistInfoCard =
    "<b>Playlist Info</b>\n\n<b>Name:</b> {name}\n<b>Owner:</b> {owner}\n"
    "<b>Songs:</b> {count}\n\n{songs}";
inline const char* kPlaylistInfoEmpty =
    "<i>This playlist has no songs yet.</i>";
inline const char* kMyPlaylistsNone =
    "You do not have any playlists yet. Use /createplaylist to make one.";
inline const char* kMyPlaylistsHeader = "<b>My Playlists</b>\n\n{name} (<code>{id}</code>)";

// --- §18 broadcast ----------------------------------------------------------
// Progress card (Parvati wins, §18), edited in place.

inline const char* kBcastCancelled = "Broadcast Cancelled";
inline const char* kBcastCancelBtn = "Cancel Broadcast";
inline InlineKeyboard bcastKeyboard() {
    return {{InlineButton::callback(kBcastCancelBtn, kCbBcastCancel)}};
}
inline const char* kBcastCancelOk = "Broadcast cancelled successfully.";
inline const char* kBcastCancelDone = "Broadcast cancelled.";

// --- §19 maintenance --------------------------------------------------------
inline const char* kMaintUser =
    "I'm under maintenance at the moment and temporarily unavailable. "
    "Please check back later.\n\n{reason}";
inline const char* kMaintReasonLine = "<i>Reason: {reason}</i>";
inline const char* kMaintUsage =
    "Maintenance is <b>{state}</b> (reason: {reason}).\nUsage: "
    "<code>/maintenance [on|off] [reason]</code>";
inline std::string maintEnabledText(const std::string& reason) {
    return fmt("Maintenance mode enabled. Reason: {reason}",
               {{"reason", reason}});
}
inline const char* kMaintDisabled = "» Maintenance mode disabled.";
inline const char* kMaintAlreadyOn =
    "Maintenance mode is already enabled.";
inline const char* kMaintAlreadyOff =
    "Maintenance mode is already disabled.";

// --- §20 channel-play -------------------------------------------------------
inline const char* kCplayResolveFail =
    " <b>Failed to resolve peer:</b> Could not fetch channel details. "
    "Ensure I can access this channel.";
inline const char* kCplayInvalidTarget =
    "<b>Invalid Target:</b> The provided ID is not a valid channel.";
inline const char* kCplayNotAccessible =
    "<b>Channel Not Accessible:</b> Could not retrieve channel information. "
    "Please ensure I am an administrator in the target channel.";
inline const char* kCplayNotLinked =
    "<b>Linked channel not found:</b> This chat is not linked to any "
    "channel / group.";
inline std::string cplayEnabledText(int64_t channel_id) {
    return fmt("<b>Channel Play enabled.</b> All channel-play commands "
               "(like <code>/cplay</code>, <code>/cskip</code>) will now work "
               "in channel <code>{id}</code>.",
               {{"id", std::to_string(channel_id)}});
}
inline const char* kCplayDisabled =
    "<b>Channel Play disabled.</b> Channel-play commands (like "
    "<code>/cplay</code>, <code>/cskip</code>) are now turned off for this "
    "chat.";
inline const char* kCplayOwnerRequired =
    "Only the owner of this chat or the target channel can use this "
    "setup.";
inline std::string cplayStatusText(int64_t channel_id) {
    return fmt("» Channel play is linked to <code>{id}</code>.",
               {{"id", std::to_string(channel_id)}});
}

// RTMP / /stream (Arc guide wins, NoxBeats short usage, §20).

inline const char* kRtmpDmOnly = "Use this command in DM.";
inline const char* kRtmpSaved =
    "» RTMP target saved. Use /stream [song/url] in that chat.";
inline const char* kRtmpRemoved = "» RTMP target removed.";
inline const char* kRtmpNotSet =
    "» No RTMP target is set for this chat. Use /setrtmp in my private chat.";
inline const char* kRtmpInvalidUrl =
    "» The URL must start with <code>rtmp://</code> or <code>rtmps://</code>.";
inline std::string rtmpStreamStartedText(const std::string& what,
                                         const std::string& by) {
    return fmt("Streaming <b>{what}</b> to RTMP.\n\n» By {by}",
               {{"what", what}, {"by", by}});
}
inline std::string rtmpStatusText(const std::string& what,
                                  const std::string& duration) {
    return fmt("» Streaming <b>{what}</b> for {dur}.",
               {{"what", what}, {"dur", duration}});
}
inline std::string rtmpStoppedText(const std::string& by) {
    return fmt("Stream stopped.\n\n» By {by}", {{"by", by}});
}
inline const char* kRtmpNone = "» No stream is running here.";
inline const char* kRtmpNoneActive = "No active RTMP stream.";
inline const char* kRtmpDelayNote =
    "Note: RTMP has ~15-30s buffering delay";
inline const char* kRtmpStreamUsage = "Usage: <code>/stream [name or url]</code>";
inline const char* kRtmpAlreadyRunning =
    "» A stream is already running here. Stop it with /streamstop.";
inline const char* kRtmpStartFailed = "» Could not start the stream.";

// --- §21 radio --------------------------------------------------------------
inline const char* kRadioCreated = "Radio Created";
inline const char* kRadioExists =
    "You already created your radio. To manage it, type /manage. To destroy "
    "it, type /destroy";
inline const char* kRadioDestroyed = "Radio destroyed.";
inline const char* kRadioNoRadio =
    "You haven't created a radio yet. Did you mean /new?";
inline const char* kRadioManageCard =
    "<b>Your radio status</b>\n"
    // No listener rows (ruling 9): radio-js reports listeners: 0 /
    // total_listeners: 0 always — no tracking exists — so the card
    // omits listener counts entirely instead of showing 0 (or "—").
    "Loop Type: <code>{loopType}</code>\n"
    "Created Since: <code>{created}</code>\n"
    "Now Playing: {nowPlaying}\n"
    "Autoplay Enabled?: <code>{autoplay}</code>\n"
    "Total Queue: <code>{queueLen}</code>\n"
    // "Live on" names the station itself (Phase 4, Q3): there is no
    // per-chat live page in the locked topology (no radio web UI), so
    // the old radio-js host URL rendered a dead link. The dispatcher
    // passes the station name as {host}.
    "Live on: <code>{host}</code>\n"
    "\n"
    "To check song queue, type /queue";
inline std::string radioNowPlayingText(const std::string& title,
                                       const std::string& url) {
    return fmt("Now playing: <a href=\"{url}\">{title}</a>",
               {{"url", url}, {"title", title}});
}
inline const char* kRadioGroupOnly = "This Command works on Group Only";
inline const char* kRadioNeedLink = "Please provide a radio/stream link";
inline const char* kRadioStationsHeader = "<h2>Radio stations</h2>";
inline std::string radioStationLine(const std::string& name,
                                    const std::string& genre,
                                    const std::string& id) {
    std::string line = "• <b>" + htmlEscape(name) + "</b>";
    if (!genre.empty()) line += " — " + htmlEscape(genre);
    line += " (<code>" + htmlEscape(id) + "</code>)";
    return line;
}
inline const char* kRadioStationsFooter =
    "Start one with /radio &lt;station&gt;.";

// --- §22 share ---------------------------------------------------------------
inline const char* kShareNoLink =
    "You haven't shared anything yet — use /share first.";
// A share_<token> link that resolves to a track (share-rust is
// anonymous by design: token -> track only, no owner stored). The
// user-target send flow is the sharetok mapping the core keeps at
// /share time; this text covers a track token opened directly.
inline const char* kShareTrackOpened =
    "Shared track: {track}\n\nUse /play {title} in a group to listen.";
inline InlineKeyboard shareHelpKeyboard() {
    using B = InlineButton;
    return {{B::callback("Help", kCbHelpMain),
             B::callback("My Link", "pl:share:mylink")}};
}
inline const char* kShareSendPrompt =
    "Please send the music that you want to share with {name}";
inline const char* kShareSelf =
    "Dude?! You have schizophrenia\nWhy you send music to yourself";
inline const char* kShareInvalidLink = "Oops!\nInvalid link";
inline const char* kShareSent = "Music Has Been Sent";
inline const char* kShareCancel = "Cancel sending music";
inline const char* kShareFeedbackSent = "Feedback is sent";
inline const char* kShareFeedbackAlready = "Feedback is sent before";
inline const char* kShareFeedbackBlocked =
    "You can't send Feedback\nYou are blocked";
inline const char* kShareFeedbackPrompt = "Send Feedback Text";
inline const char* kShareReportAlready = "User already is reported";
inline const char* kShareReported = "User is reported and blocked";
inline const char* kShareBlocked =
    "You can't send music to {name}\nYou are blocked";
inline InlineKeyboard shareReceivedKeyboard(const std::string& sender,
                                            const std::string& msg,
                                            const std::string& file_unique) {
    using B = InlineButton;
    return {{B::callback("Feedback",
                         "pl:share:feedback:" + sender + ":" + msg + ":" +
                             file_unique),
             B::callback("Report User",
                         "pl:share:report:" + sender + ":" + msg)}};
}

// --- §23 voting --------------------------------------------------------------
inline std::string voteAcceptedText(const std::string& voter,
                                    const std::string& original) {
    return fmt("{voter} has added to queue:\n{orig}",
               {{"voter", voter}, {"orig", original}});
}
inline std::string voteQueueFailText(const std::string& error) {
    return fmt("Failed to queue track: {error}", {{"error", error}});
}
inline std::string voteDeclinedText(const std::string& voter,
                                    const std::string& original) {
    if (original.empty())
        return fmt("{voter} declined.", {{"voter", voter}});
    return fmt("{voter} declined: {orig}",
               {{"voter", voter}, {"orig", original}});
}
inline std::string idText(int64_t chat_id, int64_t thread_id) {
    if (thread_id != 0)
        return fmt("This chat has ID {chat}, thread {thread}",
                   {{"chat", std::to_string(chat_id)},
                    {"thread", std::to_string(thread_id)}});
    return fmt("This chat has ID {chat}",
               {{"chat", std::to_string(chat_id)}});
}

// --- §25 delete flow / scan / recognize (core-routed) ------------------------
// Library delete confirm (navidrome §7.7, §25): trigger message is
// "delete:{path}" from inline del mode (§13).

inline std::string deleteConfirmTrackText(const std::string& label) {
    return fmt("Delete {label}?", {{"label", label}});
}
inline InlineKeyboard deleteConfirmKeyboard(const std::string& id) {
    using B = InlineButton;
    return {{B::callback("Delete", cbDel("yes", id)),
             B::callback("Cancel", cbDel("no", id))}};
}
inline const char* kToastDelCancelled = "Cancelled.";
inline const char* kToastDelDeleted = "Deleted.";
inline const char* kToastDelNothing = "Nothing to delete.";
inline const char* kDelInvalidPath = "Invalid path.";
inline const char* kDelTopLevel = "Cannot delete top-level directories.";

}  // namespace parlay::ui_texts

