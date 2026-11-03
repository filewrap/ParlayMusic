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
#include <string>
#include <utility>
#include <vector>

#include "scram.hpp"
#include "telegram_api.hpp"

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

// House status-line prefixes (remix §2.1, §15): errors "❌ | <b>…</b>",
// successes "✅ | <b>…</b>", info "{emoji} | <b>…</b>".
inline std::string errLine(const std::string& text) {
    return "❌ | <b>" + text + "</b>";
}
inline std::string okLine(const std::string& text) {
    return "✅ | <b>" + text + "</b>";
}
inline std::string infoLine(const std::string& emoji,
                            const std::string& text) {
    return emoji + " | <b>" + text + "</b>";
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
    "<b>🎵 Parlay</b>\n"
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
    "<i>Try a command… if you think you can handle me 😉</i>";

inline const char* kBotAdded =
    "🎵 Thanks for adding me! Use /play [song] to start playing music.";
inline const char* kBotAddedMaintenance =
    "⚠️ I'm currently under maintenance and will be back soon. Thanks for "
    "your patience! ⏰";
inline const char* kDemoted =
    "<b>⚠️ Permission Lost</b>\n"
    "\n"
    "I've been <b>demoted from admin</b> and can't perform my tasks "
    "anymore. Without proper rights, I'm unable to function here, so I'll "
    "be <i>leaving the chat now</i>.\n"
    "\n"
    "<b>Goodbye 👋</b>";
inline const char* kOwnerJoined =
    "👑 {user} — owner of Parlay — just entered the chat.";
inline const char* kSudoJoined =
    "🛡️ {user}, a sudo user of Parlay, just joined the chat.";

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
    "ℹ <b>About</b>\n"
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
    "<code>If you find this bot useful, feel free to give us a ⭐</code>\n"
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
        "⚡ Add Me to Your Group",
        "https://t.me/" + bot_username + "?startgroup&admin=invite_users")});
    kb.push_back(
        {InlineButton::callback("❓ Help & Commands", kCbHelpMain)});
    std::vector<InlineButton> row;
    if (!updates_url.empty()) row.push_back(InlineButton::link("📢 Updates", updates_url));
    if (!support_url.empty()) row.push_back(InlineButton::link("💬 Support", support_url));
    if (!row.empty()) kb.push_back(std::move(row));
    std::vector<InlineButton> row2;
    row2.push_back(InlineButton::callback("🌐 Language", kCbLangSelect));
    if (!source_url.empty())
        row2.push_back(InlineButton::link("🔗 Source Code", source_url));
    kb.push_back(std::move(row2));
    // Inline shortcuts (navidrome §7.2.2 + lemonic §6.1, §13).
    kb.push_back({InlineButton::switchInline("🔎 Search", "", true),
                  InlineButton::switchInline("📚 Library", "lib ", true),
                  InlineButton::switchInline("🗑 Delete", "del ", true)});
    kb.push_back({InlineButton::switchInline("▶️ Playing", "np", true),
                  InlineButton::switchInline("📝 Lyrics", "l", true),
                  InlineButton::switchInline("🔗 Share", "s", true)});
    kb.push_back(
        {InlineButton::switchInline("📤 Send playing to…", "np", false)});
    return kb;
}

inline InlineKeyboard startGroupKeyboard(const std::string& updates_url,
                                         const std::string& support_url) {
    InlineKeyboard kb;
    std::vector<InlineButton> row;
    if (!updates_url.empty()) row.push_back(InlineButton::link("📢 Updates", updates_url));
    if (!support_url.empty()) row.push_back(InlineButton::link("💬 Support", support_url));
    if (!row.empty()) kb.push_back(std::move(row));
    return kb;
}

// --- §3 /help (menu + topics) ----------------------------------------------
// Menu text (Yukki wins, §3). {support} = support URL.
inline const char* kHelpMenu =
    "<b>📚 Help Menu</b>\n"
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
        {B::callback("⬅️ Back", kCbStart)},
    };
}

// Back row on every topic page (§3).
inline InlineKeyboard helpTopicKeyboard() {
    return {{InlineButton::callback("⬅️ Back", kCbHelpMain)}};
}

// Topic pages: Parvati/Yukki merged, small-caps converted to normal
// English + Bot API HTML (tables → lines), §3. Channel variants note
// (Yukki): "Channel variants (e.g. /cpause, /cskip) act on the linked
// channel's voice chat."
inline const char* kHelpPlay =
    "⊚ <b><u>Play &amp; Channel Play:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/play</b> : start audio streaming in chat.\n"
    "➻ <b>/vplay</b> : start video streaming in chat.\n"
    "➻ <b>/playforce</b> : instant play (overrides queue).</blockquote>\n"
    "\n"
    "⊚ <b><u>Channel Play:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/cplay</b> : audio stream in linked channel.\n"
    "➻ <b>/cvplay</b> : video stream in linked channel.\n"
    "➻ <b>/cplayforce</b> : force stream in linked channel.\n"
    "➻ <b>/channelplay [id]</b> : connect channel to this group.</blockquote>\n"
    "\n"
    "Send a music link to download it. Add <b>re</b> after the link to "
    "force re-download. Every download and upload import carries a ✖ "
    "Cancel button until it finishes.";

inline const char* kHelpAdmins =
    "⊚ <b><u>Admin Commands:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Just add c as a prefix to the commands to control "
    "channel streaming.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>/pause</b> : halt the currently active stream.\n"
    "➻ <b>/resume</b> : restart the paused playback.\n"
    "➻ <b>/skip</b> : play the next track in the queue.\n"
    "➻ <b>/askip</b> : play the next track in the autoplay queue.\n"
    "➻ <b>/stop</b> : cease the stream and empty queue.\n"
    "➻ <b>/mute</b> : mute playback in videochat.\n"
    "➻ <b>/unmute</b> : unmute playback in videochat.\n"
    "➻ <b>/queue</b> : view the list of upcoming tracks.\n"
    "➻ <b>/jump</b> : jump to a given time in track.\n"
    "➻ <b>/move</b> : move a queued track to another position.\n"
    "➻ <b>/clear</b> : clear all songs from queue.\n"
    "➻ <b>/remove</b> : remove a specific track from queue.\n"
    "➻ <b>/loop [1-10]</b> : repeat the currently playing song.\n"
    "➻ <b>/shuffle</b> : randomize the queued playlist.\n"
    "➻ <b>/seek [secs]</b> : forward seek to a specific time.\n"
    "➻ <b>/seekback [secs]</b> : reverse seek to a specific time.\n"
    "➻ <b>/speed</b> : modify playback speed (0.50x - 4.00x).\n"
    "➻ <b>/playmode</b> : control who can use /play cmd.\n"
    "➻ <b>/cmddelete</b> : toggle automatic deletion of bot commands.\n"
    "➻ <b>/join</b> : join the assistant to the voice chat.\n"
    "➻ <b>/reload</b> : refresh the admin cache.</blockquote>\n"
    "\n"
    "Channel variants (e.g. /cpause, /cskip) act on the linked channel's "
    "voice chat.";

inline const char* kHelpOwner =
    "⊚ <b><u>Owner Commands:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/addsudo</b> : add a new sudo user to bot.\n"
    "➻ <b>/delsudo</b> : remove a user from sudo list.\n"
    "➻ <b>/blockuser [username | id]</b> : stop user from using bot.\n"
    "➻ <b>/unblockuser [username | id]</b> : allow the blocked user.\n"
    "➻ <b>/blacklistchat [chat id]</b> : ban a chat from using bot.\n"
    "➻ <b>/unblockchat [chat id]</b> : unban a blacklisted chat.\n"
    "➻ <b>/blacklisted</b> : show all blacklisted groups &amp; users.\n"
    "➻ <b>/broadcast</b> : send a message to all chats.\n"
    "➻ <b>/maintenance</b> : [enable | disable] maintenance mode.\n"
    "➻ <b>/restart</b> : restart the bot.\n"
    "➻ <b>/sh</b> : run shell commands.</blockquote>";

inline const char* kHelpSudoers =
    "⊚ <b><u>System &amp; Sudo Management:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Manage logs, and administrative privileges of your "
    "bot.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>Bot logs &amp; maintenance</b> :\n"
    "➻ <b>/logs</b> : get real-time logs of the bot.\n"
    "➻ <b>/logger</b> : [enable | disable] bot activity logging.\n"
    "➻ <b>/maintenance</b> : [enable | disable] maintenance mode.\n"
    "➻ <b>/eval</b> : execute code snippets.\n"
    "➻ <b>/sh</b> : run shell commands.</blockquote>\n"
    "\n"
    "⊚ <b><u>Active Streams:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/activevoice</b> : view ongoing audio sessions.\n"
    "➻ <b>/ac</b> : view all combined active streams.</blockquote>\n"
    "\n"
    "<blockquote>➻ <b>Sudo list management</b> :\n"
    "➻ <b>/addsudo</b> : add a new sudo user to bot.\n"
    "➻ <b>/rmsudo</b> : remove a user from sudo list.\n"
    "➻ <b>/sudolist</b> : check all authorized sudo users.\n"
    "\n"
    "❏ These commands are highly sensitive, use with care.</blockquote>";

inline const char* kHelpAuth =
    "⊚ <b><u>Auth Users:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Auth users are granted admin privileges without "
    "requiring group admin status to manage streams.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>/auth [user]</b> : add user to authorized list.\n"
    "➻ <b>/unauth [user]</b> : remove user from authorized list.\n"
    "➻ <b>/authlist</b> : display all authorized members.</blockquote>";

inline const char* kHelpBcast =
    "⊚ <b><u>Broadcast Feature [only for sudoers]:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/broadcast [message or reply]</b> : send to all "
    "chats.\n"
    "\n"
    "➻ <b>Modes</b> :\n"
    "<b>-pin</b> : pin the message\n"
    "<b>-pinloud</b> : pin + notify members\n"
    "<b>-user</b> : broadcast to started users\n"
    "<b>-assistant</b> : send via assistant\n"
    "<b>-nobot</b> : don't send from bot\n"
    "\n"
    "⊚ Example : /broadcast -user -pin testing broadcast</blockquote>";

inline const char* kHelpRestrict =
    "⊚ <b><u>Blacklist &amp; Global Ban:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Restrict specific chats or users from using the bot "
    "globally.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>Chat blacklist</b> :\n"
    "➻ <b>/blacklistchat [chat id]</b> : ban a chat from using bot.\n"
    "➻ <b>/unblockchat [chat id]</b> : unban a blacklisted chat.\n"
    "➻ <b>/blacklisted</b> : show all blacklisted groups &amp; "
    "users.</blockquote>\n"
    "\n"
    "<blockquote>➻ <b>User block</b> :\n"
    "➻ <b>/blockuser [username | id]</b> : stop user from using bot.\n"
    "➻ <b>/unblockuser [username | id]</b> : allow the blocked user.\n"
    "➻ <b>/blacklisted</b> : view the list of all blocked users.</blockquote>";

inline const char* kHelpThumb =
    "⊚ <b><u>Thumbnail Settings:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/nothumb</b> : manage stream visual settings.\n"
    "\n"
    "💮 <b>Toggle album art display. Disabling it reduces data load and "
    "maintains a minimalist chat look.</b>\n"
    "\n"
    "➻ <b>Options</b> :\n"
    "• <b>enable</b> : display artwork with stream info.\n"
    "• <b>disable</b> : send pure text-based updates.\n"
    "• <b>close</b> : dismiss the configuration.\n"
    "\n"
    "❏ Settings are saved individually per chat.</blockquote>";

inline const char* kHelpStart =
    "⊚ <b><u>Basic Commands:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/start</b> : initialize the music service.\n"
    "➻ <b>/help</b> : access the command guidelines.\n"
    "➻ <b>/ping</b> : measure system latency and ping.\n"
    "➻ <b>/repo</b> : get the source code repo.\n"
    "➻ <b>/position</b> : show current track's timestamp.\n"
    "➻ <b>/reload</b> : refresh admin data cache.\n"
    "➻ <b>/settings</b> : adjust configuration options.\n"
    "➻ <b>/lang</b> : select your preferred language.\n"
    "➻ <b>/bug</b> : report an issue or problem.\n"
    "➻ <b>/json</b> : show message JSON structure.\n"
    "➻ <b>/sudolist</b> : view the list of sudo admins.\n"
    "➻ <b>/stats</b> : view comprehensive bot metrics.</blockquote>";

inline const char* kHelpAutoplay =
    "⊚ <b><u>Autoplay Control:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/autoplay</b> : toggle the autoplay settings panel.\n"
    "➻ <b>/skip</b> : play the next track in the autoplay queue.</blockquote>\n"
    "\n"
    "⊚ <b><u>What is Autoplay?</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Autoplay seamlessly adds related tracks to the queue "
    "based on your current playlist for non-stop music.</b>\n"
    "\n"
    "➻ <b>Panel options</b> :\n"
    "• <b>enable</b> : activate smart recommendations.\n"
    "• <b>disable</b> : deactivate smart recommendations.\n"
    "• <b>close</b> : exit the control panel.\n"
    "\n"
    "➻ <b>Note</b> : requires at least one active song to function.</blockquote>";

inline const char* kHelpPlaylist =
    "⊚ <b><u>Playlist Commands:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Create, organize, and manage your personal "
    "playlists.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>/createplaylist</b> : create a new playlist.\n"
    "➻ <b>/deleteplaylist</b> : delete one of your playlists.\n"
    "➻ <b>/addtoplaylist</b> : add a track to a playlist.\n"
    "➻ <b>/removefromplaylist</b> : remove a track from a playlist.\n"
    "➻ <b>/playlistinfo</b> : display information about a playlist.\n"
    "➻ <b>/myplaylists</b> : list all of your playlists.</blockquote>\n"
    "\n"
    "Your personal playlist: <code>/addplaylist [song]</code> saves a song, "
    "<code>/playlist</code> lists it, <code>/playplaylist</code> plays it "
    "(add <code>-s</code> to shuffle, <code>-v</code> for video).";

inline const char* kHelpVclogs =
    "⊚ <b><u>VC Logger Module:</u></b>\n"
    "\n"
    "<blockquote>➻ <b>/vclogger [on|off]</b> : toggle join / leave alerts.\n"
    "➻ <b>/vcstatus</b> : verify the current logger state.\n"
    "\n"
    "• Tracks user IDs during VC activity.\n"
    "• Alerts are cleaned automatically after 5 sec.</blockquote>";

inline const char* kHelpInline =
    "⊚ <b><u>Bot Play Button Details:</u></b>\n"
    "\n"
    "<blockquote>💮 <b>Manage your music playback directly through inline "
    "buttons with ease.</b></blockquote>\n"
    "\n"
    "<blockquote>➻ <b>Playback controls</b> :\n"
    "➻ <b>▷</b> : resume the paused stream.\n"
    "➻ <b>II</b> : pause the ongoing stream.\n"
    "➻ <b>↻</b> : replay the current song from start.\n"
    "➻ <b>‣‣I</b> : skip the current track and play next.\n"
    "➻ <b>▢</b> : stop the playback and clear queue.\n"
    "\n"
    "➻ <b>Seek and navigation</b> :\n"
    "➻ <b>-15s</b> : seek backward by 15 seconds.\n"
    "➻ <b>15s+</b> : seek forward by 15 seconds.\n"
    "\n"
    "❏ All buttons only for group admins.</blockquote>\n"
    "\n"
    "Send a music link to download it. Add <b>re</b> after the link to "
    "force re-download. In inline mode, type <code>@{bot} </code> followed "
    "by a song name to search, or use <code>np</code> for now playing, "
    "<code>s</code> to share, <code>l</code> for lyrics, <code>lib</code> "
    "to search the library, <code>del</code> to delete.";

// Radio topic page (openradiobot §2.1, §3/§21; corrected English).
inline const char* kHelpRadio =
    "<b>📻 Radio Commands</b>\n"
    "\n"
    "<blockquote>➻ <b>/new</b> : create your radio.\n"
    "➻ <b>/destroy</b> : destroy your radio.\n"
    "➻ <b>/manage</b> : check your radio status.\n"
    "➻ <b>/play [song|URL]</b> : play a song or stream.\n"
    "➻ <b>/pause</b> / <b>/resume</b> : pause / resume playback.\n"
    "➻ <b>/skip</b> : skip the current song.\n"
    "➻ <b>/stop</b> : stop the player.\n"
    "➻ <b>/queue</b> : view the radio queue.\n"
    "➻ <b>/autoplay [on|off]</b> : toggle autoplay.\n"
    "➻ <b>/loop [queue|single|none]</b> : set the loop type.</blockquote>";

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
    "🤖 Hi! For bot help and commands, please DM me directly — I'm more "
    "responsive in private chats!";
inline const char* kHelpUnknownCmd =
    "⚠️ <i>No help found for command <code>{cmd}</code></i>";
inline const char* kHelpOpeningToast = "Opening help menu...";
inline const char* kHelpBackToast = "Returning to main menu...";
inline const char* kHelpUnknownCategory = "Unknown help category.";

// Unknown command / unhandled text (rustify §7.1, §3). {list} is the
// command list for the caller's access level.
inline const char* kUnknownCommand =
    "❓ Command <code>{command}</code> not found.\n"
    "\n"
    "📋 Commands available to you:\n"
    "{list}";
inline const char* kUnhandledText =
    "😔 Your request was not handled\n"
    "\n"
    "📋 Commands available to you:\n"
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
    return fmt("<b>🎵 Now Playing:</b>\n"
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
inline std::string stateCardText(const std::string& emoji,
                                 const std::string& state,
                                 const CardTrack& t,
                                 const std::string& by_user) {
    return fmt("{emoji} <b>{state}</b>\n"
               "\n"
               "<b>▫ Track:</b> {track}\n"
               "<b>▫ Duration:</b> {duration}\n"
               "<b>▫ Requested by:</b> {by}\n"
               "{trailer}",
               {{"emoji", emoji},
                {"state", state},
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
    return fmt("<b>🎵 Added to Queue:</b> #{index}\n"
               "\n"
               "<b>▫ Track:</b> {track}\n"
               "<b>▫ Duration:</b> {duration}\n"
               "<b>▫ Requested by:</b> {by}",
               {{"index", std::to_string(index)},
                {"track", trackLink(t)},
                {"duration", t.duration},
                {"by", t.requester}});
}

inline std::string queuedManyText(int count, const std::string& user,
                                  bool hit_limit) {
    std::string out = fmt("➕ <b>Added {count} tracks</b> by {user}",
                          {{"count", std::to_string(count)},
                           {"user", user}});
    if (hit_limit)
        out += "\n⚠️ <i>Queue limit reached — some tracks were skipped.</i>";
    out += "\n<i>Use /queue to view full list.</i>";
    return out;
}

// Merged 5-row control keyboard (Tina seek row + Yukki pause toggle +
// Parvati autoplay row, §4). Row 1 is the no-op progress button (only
// when NOT queued — the caller omits it for queued cards). The pause
// button shows ▷ when paused, II while playing. Row 5 is Close +
// Add-to-Playlist (TgMusicBot). The loop state is shown on the pause
// row's replay button label when looping ("⟳ ×n").
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
                             ? "⟳ ×" + std::to_string(loop_count)
                             : "⟳";
    kb.push_back({
        B::callback(paused ? "▷" : "II",
                    cbPlayer(paused ? "resume" : "pause", chat_id)),
        B::callback(replay, cbPlayer("replay", chat_id)),
        B::callback("‣‣I", cbPlayer("skip", chat_id)),
        B::callback("▢", cbPlayer("stop", chat_id)),
    });
    kb.push_back({
        B::callback("-15s", cbPlayer("seekback15", chat_id)),
        B::callback("+15s", cbPlayer("seek15", chat_id)),
    });
    kb.push_back({B::callback(
        std::string("♫ Autoplay: ") + (autoplay_on ? "Enabled" : "Disabled"),
        cbPlayer("autoplay", chat_id))});
    std::vector<B> row5;
    row5.push_back(B::callback("🗑 Close", kCbClose));
    if (!token.empty())
        row5.push_back(
            B::callback("➕ Add to Playlist", cbPlayerToken("addlist", token)));
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
    return "⏸️ Track paused at " + mmss(pos_sec);
}
inline const char* kCardPaused =
    "<b>⏸️ Track Paused</b>\n"
    "\n"
    "<b>▫ Track:</b> {track}\n"
    "<b>▫ Position:</b> {position} / {duration}\n"
    "<b>▫ Paused by:</b> {user}";
inline std::string toastResumed(int64_t pos_sec) {
    return "▶️ Resumed at " + mmss(pos_sec) + ".";
}
inline const char* kCardResumed =
    "<b>▶️ Track Resumed</b>\n"
    "\n"
    "<b>▫ Track:</b> {track}\n"
    "<b>▫ Position:</b> {position} / {duration}\n"
    "<b>▫ Resumed by:</b> {user}";
inline const char* kToastReplay = "🔁 Replaying current track...";
inline const char* kToastReplayed = "🔁 Track replayed.";
inline const char* kToastReplayedBy = "🔁 Track replayed by {user}.";
inline const char* kToastSkipped = "⏭️ Track skipped.";
inline const char* kToastSkippedBy = "⏭️ Skipped by {user}";
inline const char* kToastStopped = "⏹️ Playback stopped.";
inline const char* kToastMuted = "🔇 Muted.";
inline const char* kToastUnmuted = "🔊 Unmuted.";
inline const char* kCardMuted =
    "<b>🔇 Track Muted</b>\n"
    "\n"
    "<b>▫ Track:</b> {track}\n"
    "<b>▫ Muted by:</b> {user}";
inline const char* kCardUnmuted =
    "<b>🔊 Track Unmuted</b>\n"
    "\n"
    "<b>▫ Track:</b> {track}\n"
    "<b>▫ Unmuted by:</b> {user}";
inline std::string toastSeekFwd(int seconds) {
    return "⏩ Sought forward " + std::to_string(seconds) + " seconds";
}
inline std::string toastSeekBack(int seconds) {
    return "⏪ Sought back " + std::to_string(seconds) + " seconds";
}
inline std::string toastSeekNearEnd(int seconds) {
    return "⚠️ Cannot seek forward " + std::to_string(seconds) +
           " seconds — about to reach end.";
}
inline const char* kToastAlreadyPlaying = "ℹ️ Track is already playing.";
inline const char* kToastResumeFailed = "❌ Failed to resume playback.";
inline const char* kToastSeekFailed = "❌ Failed to seek.\nError: <code>{error}</code>";
inline const char* kToastSkipEmpty = "⏹️ Playback stopped — queue empty.";
inline const char* kToastNextDownloadFailed = "❌ Failed to download next track.";
inline const char* kToastNextPlayFailed = "❌ Failed to play next track.";
inline const char* kToastInvalidRequest = "⚠️ Invalid request.";
inline const char* kToastUnknownAction = "⚠️ Unknown action.";
inline const char* kToastNothingPlaying = "⚠️ Nothing playing right now.";
inline const char* kToastExpired = "This button has expired.";
inline const char* kToastNotAllowed = "You aren't allowed";
inline const char* kToastFeatureUnavailable =
    "🚧 This feature is not available yet.";
// Toggle toasts (player row + settings panel): plain text, no HTML —
// answerCallbackQuery shows the text verbatim.
inline const char* kToastAutoplayOn = "♫ Autoplay enabled.";
inline const char* kToastAutoplayOff = "♫ Autoplay disabled.";
inline const char* kToastLoopOn = "🔁 Loop enabled.";
inline const char* kToastLoopOff = "🔁 Loop disabled.";
inline const char* kToastThumbOn = "🖼 Thumbnails enabled.";
inline const char* kToastThumbOff = "🖼 Thumbnails disabled.";
inline const char* kToastShuffle = "🔀 Queue shuffled.";
inline const char* kToastAddedToPlaylist = "➕ Added to your playlist.";
inline const char* kToastAlreadyInPlaylist = "Already in your playlist.";
inline const char* kToastPlaylistFull = "Your playlist is full.";
inline const char* kToastDisliked = "Disliked.";

// Stop confirm (Yukki wins, §4).
inline const char* kStopConfirmPaused =
    "⏸️ <b>Playback is currently paused.</b>\n"
    "\n"
    "Do you want to resume playing or stop it completely?";
inline const char* kStopConfirmMuted =
    "🔇 <b>Playback is currently muted.</b>\n"
    "\n"
    "Do you want to unmute or stop it completely?";
inline InlineKeyboard stopConfirmKeyboard(int64_t chat_id, bool paused) {
    using B = InlineButton;
    std::vector<B> row;
    if (paused)
        row.push_back(B::callback("▶️ Resume",
                                  "pl:player:confirm:resume:" +
                                      std::to_string(chat_id)));
    else
        row.push_back(B::callback("🔊 Unmute",
                                  "pl:player:confirm:unmute:" +
                                      std::to_string(chat_id)));
    row.push_back(B::callback("⏹️ Stop",
                              "pl:player:confirm:stop:" +
                                  std::to_string(chat_id)));
    return {std::move(row)};
}

// Restart confirm (Yukki §3.3, §4).
inline const char* kRestartConfirm =
    "⚠️ A song is currently playing in this chat.\nChoose an action below.";
inline InlineKeyboard restartConfirmKeyboard(int64_t chat_id) {
    using B = InlineButton;
    return {{B::callback("Restart the Bot", cbRestart("bot", chat_id)),
             B::callback("Replay the Song", cbRestart("replay", chat_id))}};
}
inline const char* kToastRestarting = "♻️ Restarting the bot...";
inline const char* kToastReplayOk = "⏮️ Current song replayed successfully.";

// Text-command control replies (remix wins, §4). The three-way select:
// true -> done, false -> not streaming, null -> not in call.
inline const char* kCtlPaused = "⏸ | <b>Paused.</b>";
inline const char* kCtlResumed = "▶️ | <b>Resumed.</b>";
inline const char* kCtlSkipped = "⏩ | <b>Skipped.</b>";
inline const char* kCtlStopped = "⏹ | <b>Stopped.</b>";
inline const char* kCtlNotStreaming = "❌ | <b>Not streaming.</b>";
inline const char* kCtlNotInCall = "❌ | <b>Not in call.</b>";
inline const char* kCtlMuted = "🔇 | <b>Muted.</b>";
inline const char* kCtlUnmuted = "🔊 | <b>Unmuted.</b>";
inline const char* kCtlAlreadyMuted = "❌ | <b>Already muted.</b>";
inline const char* kCtlJoined = "✅ | <b>Joined the voice chat.</b>";
inline const char* kCtlAlreadyJoined =
    "❌ | <b>Already in the voice chat.</b>";
inline const char* kCtlNotMuted = "❌ | <b>Not muted.</b>";
inline const char* kCtlLoopOn = "🔁 | <b>Turned loop on.</b>";
inline const char* kCtlLoopOff = "🔁 | <b>Turned loop off.</b>";
inline std::string ctlVolume(int amount) {
    return fmt("🔈 | <b>Volume set to {amount}.</b>",
               {{"amount", std::to_string(amount)}});
}
inline const char* kCtlVolumeInvalid =
    "❌ | <b>Pass a number between 0 and 200.</b>";
inline const char* kCtlShuffling = "🔀 | <b>Shuffling...</b>";
inline const char* kCtlShuffleFew =
    "❌ | <b>Not enough items to shuffle.</b>";

// Control panel /menu (remix §2.3, §4). Admin-only; only the user who
// sent the replied-to command may press (others silently ignored).
inline std::string menuPanelText(const std::string& now_emoji,
                                 const std::string& now_title,
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
    return now_emoji + " | " + linked(now, now_url) + "\n➡️ | " +
           linked(next, next_url);
}
inline InlineKeyboard menuPanelKeyboard(int64_t chat_id) {
    using B = InlineButton;
    auto p = [chat_id](const std::string& v) {
        return cbPlayer(v, chat_id);
    };
    return {
        {B::callback("🔀", p("shuffle")), B::callback("⏩", p("skip")),
         B::callback("⏸", p("pause")), B::callback("▶️", p("resume"))},
        {B::callback("🔇", p("mute")), B::callback("🔈", p("unmute")),
         B::callback("🔉", p("voldec")), B::callback("🔊", p("volinc"))},
        {B::callback("🔄", p("refresh"))},
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
    return fmt("🎵 | Currently streaming <b><a href=\"{titleUrl}\">{title}</a></b> "
               "by <b><a href=\"{requesterUrl}\">{requester}</a></b>...",
               {{"titleUrl", title_url},
                {"title", title},
                {"requesterUrl", requester_url},
                {"requester", requester}});
}
inline const char* kNowIdle = "❌ | <b>Not streaming.</b>";

// /position (Parvati/Yukki §3.6/§3.7, §4).
inline const char* kPositionText =
    "🎵 <b>{title}</b>\n"
    "📍 <code>{position} / {duration}</code>\n"
    "⚙️ Speed: <b>{speed}x</b>";

// Voice-chat events (Tina/Yukki §2.9/§3.9, §4).
inline const char* kVcStarted =
    "<b>♻️ Voice chat started!</b>\n"
    "\n"
    "<i>Use /play [song name] to play music.</i>";
inline const char* kVcEnded =
    "<b>⚠️ Video chat ended</b>\n"
    "\n"
    "<i>⏰ Duration: {duration}</i>";
inline const char* kQueueFinished =
    "🎵 <b>Queue Finished!</b>\n"
    "\n"
    "No more songs to play. Use /play to add more tracks! 🎶";
inline const char* kNextDownloading = "📥 Downloading your next track...";
inline const char* kAutoplayNext = "🔁 Autoplay: <b>{title}</b>";
inline const char* kVcEndedStop =
    "📴 The voice chat was ended, so playback has stopped.\n"
    "\n"
    "💐 Start a new voice chat and use /play again.";

// --- §5 queue display ------------------------------------------------------
// Yukki wins — the only rich queue in any ref (table + per-track delete
// + show-more), Bot-API-HTML translation (§5). First page shows 15
// up-next tracks; a [➕ Show {count} more] button reveals the rest.
inline const int kQueuePageSize = 15;

inline std::string queueText(const CardTrack& now,
                             const std::vector<CardTrack>& up_next,
                             size_t total_up_next) {
    std::string out = "<b>🎶 Current Queue</b>\n\n";
    out += "<b>▶️ Now Playing</b>\n";
    out += trackLink(now) + "\n";
    out += "⏱️ " + now.duration + "  •  👤 " + now.requester + "\n";
    out += "\n<b>⏭️ Up Next • " + std::to_string(total_up_next) +
           " tracks</b>\n";
    if (up_next.empty()) {
        out += "📭 <i>No more songs in queue.</i>";
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
        kb.push_back({B::callback("➕ Show " + std::to_string(remaining) +
                                      " more",
                                  cbQueueMore(chat_id))});
    }
    return kb;
}

inline const char* kQueueFetching = "Fetching queue...";
inline const char* kQueueEmpty =
    "⚠️ <b>No active playback.</b>\n"
    "\n"
    "<i>Nothing is queued right now.</i>";
inline const char* kQueueAlreadyEmpty =
    "⚠️ <b>The queue is already empty.</b>";
inline std::string queueLimitText(int limit) {
    return fmt("⚠️ Queue limit reached ({limit} tracks max). Use /clear to "
               "clear queue.",
               {{"limit", std::to_string(limit)}});
}
inline const char* kToastQueueRemoved = "🗑️ Removed from queue.";

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
inline std::string queueFullText(int limit) {
    return fmt("» The queue limit ({limit}) has been reached. Please wait for "
               "the currently queued tracks to finish playing, then try "
               "again.",
               {{"limit", std::to_string(limit)}});
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
inline const char* kLyricsNotStreaming = "❌ | <b>Not streaming.</b>";
inline const char* kLyricsNotFound = "❌ | <b>Lyrics not found.</b>";
inline const char* kLyricsCard = "<b>{title}</b> #lyrics\n\n{lyrics}";
inline const char* kLyricsNeedQuery =
    "Please enter a song keyword, song share link, or song ID";
inline const char* kLyricsFetching = "Fetching lyrics…";
inline const char* kLyricsFail =
    "Failed to get lyrics — the song may not exist or may be instrumental";
inline const char* kLyricsNoProgram = "Radio programs don't support lyrics";
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
    out += "\n  ├ 🕓 " + dur;
    out += "\n  ├ 👁 " + (views.empty() ? "N/A" : views);
    out += "\n  ├ 📅 " + (upload_time.empty() ? "N/A" : upload_time);
    out += "\n  └ 👤 " + (uploader.empty() ? "N/A" : uploader);
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
            "⬅️ Previous page",
            cbSearchPage(user_id, page - 1, hash)));
    if (has_next)
        row.push_back(B::callback(
            "Next page ➡️", cbSearchPage(user_id, page + 1, hash)));
    if (row.empty()) return {};
    (void)total_pages;
    return {std::move(row)};
}

inline const char* kSearchActive = "❌ | <b>There is an active search.</b>";
inline const char* kSearchNoQuery =
    "❔ | <b>What do you want to search for?</b>";
inline const char* kSearchNoResults = "❌ | <b>No results found.</b>";
inline const char* kSearchCancelled = "✅ | <b>Search canceled.</b>";
inline const char* kSearchNoActive = "❌ | <b>There is no search active.</b>";
inline const char* kSearchExpired = "Search expired, please search again";
inline const char* kSearchOnlyStarter =
    "Only the person who started the search can use it";
inline std::string searchTooFrequentText(int seconds) {
    return "Too frequent, please try again in " + std::to_string(seconds) +
           " seconds";
}
inline const char* kSearchNoMore = "No more results.";
inline const char* kSearchSuccessToast = "Success";
inline const char* kSearching = "🔎 Searching for track...";
inline const char* kChooseTrack = "Choose a track:";
inline const char* kLinkAccepted =
    "🔗 Link accepted! What exactly to download?";
inline const char* kSearchLinkOutdated = "❌ Link is outdated.";
inline const char* kSearchResultsOutdated = "❌ Results are outdated.";
inline const char* kSearchNothingFound = "I've found nothing.";

// /search help text (lemonic §6.2, typo corrected, §7).


// /play search flow lifecycle (NoxBeats/Tina/Yukki/Arc/Parvati, §7).
inline const char* kPlaySearching = "🔍🎶 Searching... ⚡✨";
inline std::string playSearchingForText(const std::string& query) {
    return fmt("🔍🎶 Searching for: <b>{query}</b>... ⚡✨",
               {{"query", htmlEscape(query)}});
}
inline std::string playDownloadingText(const std::string& title) {
    return fmt("📥 Downloading song \"{title}\"",
               {{"title", htmlEscape(title)}});
}
inline const char* kNoSongQuery =
    "<b>🎵 Whoops! Nothing to play.</b>\n"
    "\n"
    "<i>Send <b>{cmd} [song name or link]</b>, or reply to a <b>media</b> "
    "message to start the music.</i>";
inline const char* kPlayNotFound = "❌ No tracks found.";
inline std::string playAlreadyQueuedText(const std::string& title) {
    return fmt("✅ <b>Track already added!</b>\n\n<i>{title}</i> is already "
               "playing or in the queue.",
               {{"title", htmlEscape(title)}});
}
inline const char* kPlayAlreadyPlaying =
    "⚠️ <b>A song is already playing.</b>\n"
    "\n"
    "Please wait for it to finish or use <code>/stop</code> to stop it "
    "first.";
inline std::string playFailedText(const std::string& error) {
    return fmt("❌ Failed to play\nError: {error}",
               {{"error", htmlEscape(error)}});
}
inline const char* kPlayUnable =
    "❌ Unable to play — please restart the voice chat.";
inline std::string playTooLongText(int minutes) {
    return fmt("Sorry, song exceeds max duration of {n} minutes.",
               {{"n", std::to_string(minutes)}});
}
inline std::string playAllTooLongText(int minutes) {
    return fmt("❌ All found tracks were skipped due to duration limits "
               "({n} min).",
               {{"n", std::to_string(minutes)}});
}
inline const char* kDownloadTooLong =
    "‼️ <b>Oops! Video too long to convert!</b> Order something 30 minutes "
    "or less.";
inline const char* kFoundLine = "🎵 {title}\n🔗 {link}";
inline const char* kSpotifyLinkUnsupported =
    "‼️ <b>Oops! The bot does not support Spotify links!</b>";

// --- §8 download progress ---------------------------------------------------
// Live progress message (Parvati fields + navidrome checklist, §8),
// edited in place. {done}/{total} byte line is added when known.
inline const char* kDlProgress =
    "📥 Downloading your track...\n"
    "\n"
    "<b>Progress:</b> {pct}%\n"
    "<b>Speed:</b> {speed}\n"
    "<b>ETA:</b> {eta}\n"
    "<b>Elapsed:</b> {elapsed}";
inline const char* kDlProgressBytes =
    "<b>Progress:</b> {done} / {total} [{pct:.1f}%]";
inline const char* kDlCancelBtn = "🚦 Cancel Downloading";
inline const char* kDlUsage = "Usage: /download &lt;song title or link&gt;";
inline const char* kDlCancelled = "✅ Download cancelled.";
inline const char* kDlNoDownload = "ℹ️ No download to cancel.";
inline std::string dlCancelledByText(const std::string& user) {
    return fmt("⚠️ Download cancelled by {user}", {{"user", user}});
}
inline const char* kDlCancelling = "Hang on, cancelling the download now!";
inline const char* kDlFetching = "Fetching song info...";
inline const char* kDlCacheHit = "…\nCache hit, sending...";
inline const char* kDlWaiting = "Waiting to download...";
inline const char* kDlDownloading = "Downloading...";
inline const char* kDlComplete = "Download complete, sending...";
inline const char* kDlResolving = "Resolving link…";
inline std::string dlResolveFailText(const std::string& error) {
    std::string e = error;
    if (e.size() > 120) e = e.substr(0, 120);
    return fmt("❌ Failed to resolve link: {error}",
               {{"error", htmlEscape(e)}});
}
inline std::string dlCouldNotResolveText(const std::string& bot) {
    return fmt("❌ Could not resolve link. Search by name instead: @{bot} "
               "artist title",
               {{"bot", bot}});
}
inline const char* kDlQueued =
    "Queued — will start after the current download finishes";
inline const char* kDlFetchAlbum = "Fetching album info…";
inline const char* kDlFetchTrack = "Fetching track info…";
inline std::string dlFailedText(const std::string& error) {
    return fmt("❌ Download failed: {error}",
               {{"error", htmlEscape(error)}});
}
inline const char* kDlAlreadyQueued = "Already queued.";
inline std::string dlCompleteProcessingText(int seconds) {
    return fmt("Download completed. Processing file...\n\nTime elapsed: {s}s",
               {{"s", std::to_string(seconds)}});
}
inline const char* kDlSuccess = "✅ Song downloaded successfully!";
inline const char* kDlSizeLimit =
    "» Failed to process the file. The maximum download limit is "
    "<b>200 MB</b>.";

// Lossy-fallback prompt (navidrome §7.6.7, §8).
inline const char* kLossyPrompt =
    "FLAC not found for {artist} — {title}\n"
    "\n"
    "Lossy fallback available: {summary}";
inline InlineKeyboard lossyKeyboard(const std::string& id,
                                    const std::string& summary) {
    using B = InlineButton;
    return {{B::callback("✓ Accept " + summary, cbLossy("accept", id)),
             B::callback("✗ Skip", cbLossy("skip", id)),
             B::callback("✖ Cancel", cbDlCancel(id))}};
}
inline const char* kLossyExpired = "Prompt expired (5 min).";
inline const char* kToastLossyExpired = "Prompt expired.";
inline const char* kToastLossySkipped = "Skipped.";
inline std::string lossySkippedText(const std::string& artist,
                                    const std::string& title) {
    return fmt("Skipped — FLAC not available for {artist} — {title}",
               {{"artist", artist}, {"title", title}});
}
inline std::string lossyCancelledText(const std::string& artist,
                                      const std::string& title) {
    return fmt("✖ Cancelled — FLAC not available for {artist} — {title}, no "
               "fallback taken",
               {{"artist", artist}, {"title", title}});
}

// Cancel final tallies (navidrome §7.6.2, §8).
inline const char* kToastDlNothingToCancel =
    "Nothing to cancel — already finished.";
inline const char* kToastDlWrongChat =
    "Only the chat that requested this can cancel it.";
inline const char* kToastDlCancelling = "Cancelling…";
inline const char* kDlCancelSaved =
    "✖ Cancelled — the track was already saved";
inline const char* kDlCancelNothing = "✖ Cancelled — nothing was downloaded";
inline std::string dlCancelAlbumText(int saved, int total) {
    return fmt("✖ Cancelled — {saved}/{total} tracks were already saved",
               {{"saved", std::to_string(saved)},
                {"total", std::to_string(total)}});
}

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
inline const char* kDlWrongSpotifyUrl =
    "Looks like you've sent the wrong Spotify URL.";

// Audio-effect picker (tmb-rust §4.4, §8).
inline const char* kDlFxPrompt =
    "📌 Track: <b>{title}</b>\n\nChoose a processing option:";
inline InlineKeyboard dlFxKeyboard(const std::string& run_id) {
    using B = InlineButton;
    return {
        {B::callback("🎵 Original", cbDlFx(run_id, "orig")),
         B::callback("🌌 Slow + Reverb", cbDlFx(run_id, "slow"))},
        {B::callback("⚡ Nightcore", cbDlFx(run_id, "nightcore")),
         B::callback("🔊 Bass Boost", cbDlFx(run_id, "bass"))},
        {B::callback("🌀 8D Audio", cbDlFx(run_id, "8d"))},
    };
}
inline const char* kDlFxApplying = "📥 Downloading and applying effect...";
inline const char* kDlFxError = "❌ Error downloading track.";

// Download-delivery audio caption (Music163bot §6.3, §4/§8).
inline const char* kAudioCaption =
    "「{name}」- {artists}\n"
    "Album: {album}\n"
    "#{ext} {sizeMB:.2f}MB {kbps:.2f}kbps\n"
    "via @{bot}";

// Upload intake (navidrome §7.10, §8/§24).
inline const char* kUploadReceived =
    "📦 {name}: {n} audio file(s) received. Identifying release…";
inline const char* kUploadIdentified =
    "📦 {name}: release identified. Import queued until the current "
    "download finishes…";
inline const char* kUploadImporting =
    "📦 Importing upload: {artist} — {title}…";
inline const char* kUploadNoMatch =
    "❓ {name}: couldn't match this to a release — {diagnosis}. Files are "
    "kept in uploads staging; nothing was filed.";
inline const char* kUploadImportFailed =
    "❌ Upload import failed: {error}. Files kept in staging.";
inline const char* kUploadRejected =
    "❌ Upload rejected: {name}\nReason: {error}\nMoved to {_REJECTED}/ — "
    "fix and re-drop.";

// Resume after restart (navidrome §7.11, §8).
inline const char* kDlResume = "🔁 Bot restarted — resuming this download…";
inline const char* kDlGiveUp =
    "A download kept dying across bot restarts — giving up on it. Re-send "
    "the link to try again.";

// --- §9 settings menus ------------------------------------------------------
// Per-chat settings snapshot the panel renders from. The dispatcher
// fills it from the Store (chat:<id> fields); toggles write back.
struct ChatSettings {
    int admin_mode = 0;  // 0 admins+auth, 1 admins only, 2 everyone
    std::string play_mode = "everyone";
    bool cmd_delete = false;
    int clean_delay_min = 0;  // 0 = off; the clean delay cycles 15→30→60→5
    bool thumbnails = true;
    bool autoplay = false;
    bool silent = false;
    bool auto_delete = false;
    int auto_delete_min = 5;  // valid 2–7
    std::string language = "en";
    bool paginate_purge_old = true;
    bool chat_music_download = true;
};

inline const char* kSettingsText =
    "⚙️ <b>Chat Settings</b>\n"
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
                                 ? "Admins 🔐"
                                 : s.admin_mode == 2 ? "Everyone 🌍"
                                                     : "Auth Users 🎧";
    std::string play_mode =
        s.play_mode == "admin" ? "Admins 🔐" : "Everyone 🌍";
    std::string delay = s.clean_delay_min == 0
                            ? "15m"
                            : std::to_string(s.clean_delay_min) + "m";
    std::string timer_label = "Delete after: " +
                              std::to_string(s.auto_delete_min) +
                              " minutes";
    InlineKeyboard kb = {
        {B::callback("🛡️ Admin Mode", cbSetInfo("adminmode")),
         B::callback(admin_mode, cbSet("adminmode"))},
        {B::callback("🎵 Play Mode", cbSetInfo("playmode")),
         B::callback(play_mode, cbSet("playmode"))},
        {B::callback("🗑️ Cmd Delete", cbSetInfo("cmddelete")),
         B::callback(on_off(s.cmd_delete), cbSet("cmddelete"))},
        {B::callback("🧹 Clean Mode", cbSetInfo("cleanmode")),
         B::callback(on_off(s.clean_delay_min > 0), cbSet("cleanmode"))},
        {B::callback("⏱️ Clean Delay", cbSetInfo("cleanduration")),
         B::callback(delay, cbSet("cleanduration"))},
        {B::callback("🖼️ Thumbnails", cbSetInfo("nothumb")),
         B::callback(on_off(s.thumbnails), cbSet("nothumb"))},
        {B::callback("📻 Autoplay", cbSetInfo("autoplay")),
         B::callback(on_off(s.autoplay), cbSet("autoplay"))},
        {B::callback("🌐 Language", cbSetInfo("lang")),
         B::callback(s.language, kCbLangSelect)},
        {B::callback(std::string("Auto-delete: ") + on_off(s.auto_delete),
                     cbSet("autodelete"))},
        {B::callback(timer_label, cbSetInfo("timer"))},
        {B::callback(std::string("Silent mode: ") + on_off(s.silent), cbSet("silent"))},
        {B::callback("🗑 Close", kCbClose)},
    };
    // Timer buttons 2–7 min (TgMusicBot §5.7): current value prefixed ✓.
    std::vector<B> timers;
    for (int n = 2; n <= 7; ++n) {
        timers.push_back(B::callback(
            (n == s.auto_delete_min ? "✓ " : "") + std::to_string(n),
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

inline const char* kSettingsUpdated = "✅ Settings updated!";
inline const char* kSettingsGroupOnly =
    "These settings are available only in groups.";
inline const char* kSettingsOutdated =
    "This settings button is outdated. Open /settings again.";
inline const char* kSettingsLoadFail =
    "Unable to load settings. Please try again later.";
inline const char* kSettingsSaveFail =
    "Unable to save settings. Please try again.";
inline const char* kSettingsNoDeletePerm =
    "I don't have permission to delete messages in this chat.";
inline const char* kSettingsNotStreaming = "Bot is not streaming.";

// Dashboard toggle cards (Yukki §3.7, §9): command-form equivalent of
// the panel rows.
inline std::string dashboardCardText(const std::string& title,
                                     const std::string& description,
                                     bool enabled) {
    return "<b>" + title + "</b>\n\n<i>" + description + "</i>\n\nℹ️ Status: <b>" +
           (enabled ? "Enabled ✅" : "Disabled ❌") + "</b>";
}
inline InlineKeyboard dashboardKeyboard(const std::string& key,
                                        bool enabled) {
    using B = InlineButton;
    return {{B::callback(enabled ? "Enabled ✅" : "Disabled ❌",
                         cbSet(key)),
             B::callback("Close", kCbClose)}};
}
inline std::string settingUpdatedText(bool enabled) {
    return fmt("✅ Setting updated to {status}.",
               {{"status", enabled ? "Enabled" : "Disabled"}});
}
inline const char* kSettingUpdateFail = "⚠️ Failed to update the setting.";

inline const char* kDashAutoplayDesc =
    "Once the queue ends, the bot automatically keeps playing recommended "
    "songs similar to the last played track.";
inline const char* kDashAutoleaveDesc =
    "When enabled, the assistant automatically leaves inactive or "
    "unnecessary chats every 10 minutes.";
inline const char* kDashMaintenanceDesc =
    "When enabled, all active rooms are stopped and only the owner/sudo "
    "users can use the bot.";
inline const char* kDashLoggerDesc =
    "When enabled, bot activity is logged to the logger channel.";
inline const char* kDashCmdDeleteDesc =
    "When enabled, command messages are deleted after they run.";
inline const char* kDashCleanModeDesc =
    "When enabled, the bot's messages are auto-deleted after the clean "
    "delay.";
inline const char* kDashPlayModeDesc =
    "When enabled, only admins and authorized users can use /play.";
inline const char* kDashNothumbDesc =
    "When enabled, album art is hidden from now-playing and queue cards.";

// Autoplay card (Tina wins, §9).
inline std::string autoplayCardText(bool enabled) {
    std::string out =
        "📻 <b>Autoplay Mode Settings</b>\n\n• <b>Current Status:</b> ";
    out += enabled ? "🟢 <b>Enabled (ON)</b>" : "🔴 <b>Disabled (OFF)</b>";
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
    return {{B::callback(enabled ? "🟢 Turn OFF" : "🔴 Turn ON",
                         cbSet("autoplay")),
             B::callback("🗑 Close", kCbClose)}};
}
inline std::string autoplayStatusText(bool on, const std::string& lang,
                                      const std::string& mood) {
    return fmt(" <b>Autoplay:</b> {on}\n<b>Language:</b> <code>{lang}</code>\n"
               "<b>Mood:</b> <code>{mood}</code>",
               {{"on", on ? "on" : "off"}, {"lang", lang}, {"mood", mood}});
}
inline const char* kAskipDisabled =
    "⚠️ Autoplay is disabled. Use /autoplay to enable it.";
inline const char* kAskipNone =
    "⚠️ No autoplay recommendations found to skip to.";
inline std::string askipSuccessText(const std::string& user,
                                    const std::string& title) {
    return fmt("⏭️ Autoplay skipped by {user}\n🔁 Playing: <b>{title}</b>",
               {{"user", user}, {"title", title}});
}

// Language picker (merged, English-only today, §9).
inline const char* kLangPicker = "🌐 <b>Select Bot Language:</b>";
inline InlineKeyboard langPickerKeyboard(const std::string& origin) {
    using B = InlineButton;
    std::string back = kCbStart;
    if (origin == "help") back = kCbHelpMain;
    if (origin == "settings") back = kCbSetMain;
    return {
        {B::callback("✔️ English 🇺🇸", std::string(kCbLangEn) + ":" + origin)},
        {B::callback("⬅️ Back", back)},
    };
}
inline std::string langUpdatedText(const std::string& lang_name) {
    return fmt("✅ Language successfully updated to {lang}.",
               {{"lang", lang_name}});
}
inline const char* kLangAlready = "ℹ️ You're already using this language.";
inline const char* kLangFailed =
    "❌ Failed to update language. Please try again later.";

// Command-form settings (§9).
inline const char* kAdminModeInvalid =
    "⚠️ Invalid mode. Use: <code>admin</code>, <code>adminauth</code>, or "
    "<code>everyone</code>.";
inline std::string adminModeUpdatedText(const std::string& status) {
    return fmt("✅ <b>Admin command mode</b> updated to: {status}",
               {{"status", status}});
}
inline const char* kPlayModeAdmin =
    "🛡️ <b>Play mode</b> is now restricted to <b>Admins &amp; DJs</b> only. 🎧";
inline const char* kPlayModeEveryone =
    "✅ <b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow! 🎶";
inline const char* kPlayModeDenied =
    "🚫 Play mode is enabled. Only admins and authorized users can play.";
inline const char* kCleanModeStatus =
    "🧹 <b>Clean mode</b>\n"
    "\n"
    "Auto-cleanup of command and service messages.\n"
    "Current state: {action}\n"
    "Delete after: {duration}m";
inline const char* kCleanModeHint = "Use /settings to change auto-delete duration.";
inline const char* kLoggerNeedId = "Please set LOGGER_ID in .env first.";
inline const char* kLoggerUsage =
    "Usage: /logger [enable|disable|on|off]\nCurrent status: {status}";
inline const char* kLoggerEnabled = "Logger Enabled";
inline const char* kLoggerDisabled = "Logger disabled";
inline const char* kThumbStatus =
    "<b>Thumbnail:</b> {on}\n\n<code>/thumbnail on</code> • "
    "<code>/thumbnail off</code>";
inline const char* kInvalidBool =
    "⚠️ <b>Invalid value.</b>\nUse 'enable' or 'disable'.";

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
inline const char* kSettingsIndex =
    "⚙ <b>Settings</b>\n"
    "Use this section to apply personalized custom tweaks to Parlay";
inline InlineKeyboard settingsIndexKeyboard() {
    using B = InlineButton;
    return {{B::callback("⏩ Pagination", cbSet("page")),
             B::callback("💀 Music Download", cbSet("musicdl"))}};
}
inline const char* kSettingsPageScreen =
    "⏩ <b>Auto delete old messages</b>\n"
    "By default previous messages are deleted during pagination to ease "
    "chat scrolling, use this section to change this behaviour.";
inline const char* kSettingsMusicDlScreen =
    "⬇ <b>Download music via chat</b>\n"
    "Enabling this will allow you to download music directly from chat.\n"
    "<b>Note:</b> The actual size is compressed and you can't download a "
    "music file of more than <em>200MB</em> via chat.\n"
    "✅ Supported formats\n"
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
    return {{B::callback(enabled ? "✅ Enabled" : "❌ Disabled",
                         cbSet(key + ":" + (enabled ? "0" : "1"))),
             B::callback("🔙 Back", kCbSetMain)}};
}

// --- §10 admin/sudo panels ---------------------------------------------------
// Permission denials (Yukki §3.9 wins, §10).
inline const char* kDenyAdmin =
    "<b>🚫 Access Denied</b>\n"
    "<i>🔒 This action is limited to <b>chat admins</b>.</i>\n"
    "\n"
    "You can still <code>/play</code> songs and enjoy the music.\n"
    "\n"
    "<i>💡 Admin? Run <code>/reload</code> to refresh permissions.</i>";
inline const char* kDenyAdminCb = "⚠️ Only admins can do these actions.";
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
    "<b>🚫 Access Denied</b>\n"
    "<i>🔒 This action is limited to <b>chat admins and authorized "
    "users</b>.</i>\n"
    "\n"
    "You can still <code>/play</code> songs and enjoy the music.";
inline const char* kDenySupergroup =
    "⚠️ This command can only be used in groups.";
inline const char* kDenyOwner = "⚠️ Only the bot owner can use this command.";
inline const char* kDenySudo =
    "⚠️ Only sudo users or the bot owner can use this command.";
inline const char* kSupergroupGuide =
    "⚠️ <b>This bot only works in supergroups.</b>\n"
    "\n"
    "Convert this group to a supergroup to use it.";
inline const char* kPlayChatInvalid =
    "This bot can only be used in <b>supergroups</b>… Leaving this chat...";

// Active streams (NoxBeats §1.7 + Tina/Yukki §2.7/§3.7, §10).
inline const char* kActiveStreamsHeader = "<u><b>List of active streams:</b></u>";
inline std::string activeStreamsCountText(int count) {
    return fmt("Active streams on the bot: <b>{n}</b>",
               {{"n", std::to_string(count)}});
}
inline const char* kActiveStreamsEmpty =
    "» Looks like there aren't any active streams on the bot.";
inline std::string activeChatsText(int count) {
    return fmt("📊 <b>Active Chats:</b>\n\nThere are <i>{n}</i> active chats "
               "across the bot system.",
               {{"n", std::to_string(count)}});
}
inline std::string activeChatsBrokenText(int count, int broken) {
    return fmt("📊 <b>Active Chats:</b>\n\nThere are <i>{n}</i> active chats "
               "across the bot system.\n<i>{b}</i> of them are broken.",
               {{"n", std::to_string(count)},
                {"b", std::to_string(broken)}});
}

// /reload (Parvati/Yukki/Tina + TgMusicBot, §10).
inline const char* kReloadStart =
    "⚙️ Reloading admin cache, voice chat status, and assistant status...";
inline const char* kReloadDoneHeader = "<b>⚙️ Reload complete:</b>\n\n{summary}";
inline const char* kReloadShortOk =
    "Administrator cache reloaded successfully.";
inline const char* kReloadShortFail =
    "Failed to reload administrator cache.";
inline std::string reloadCooldownText(const std::string& mmss) {
    return fmt("Please wait {t} before using this command again.",
               {{"t", mmss}});
}
inline const char* kReloadCacheRule =
    "» You can only refresh the admin cache once every 10 minutes.";
inline const char* kReloadCacheStart = "» Reloading admin cache...";
inline const char* kReloadCacheOk = "» Admin cache refreshed successfully!";

// /logs + logger channel (NoxBeats §1.9, Parvati/Yukki §3.8, Arc §4.9, §10).
inline const char* kLogsFetching = "» Fetching logs...";
inline std::string logsCaptionText(const std::string& bot) {
    return fmt("Log file of {bot}", {{"bot", bot}});
}
inline const char* kLogsMissing = "» Log file doesn't exist.";
inline const char* kLogsNone = "No logs found";
inline std::string loggerBotJoinedText(int64_t chat_id) {
    return fmt("<b>🤖 Bot Joined a New Chat</b>\n📌 <b>Chat ID:</b> "
               "<code>{id}</code>",
               {{"id", std::to_string(chat_id)}});
}
inline std::string loggerStartedText(const std::string& mention,
                                     const std::string& username,
                                     int64_t user_id) {
    return fmt("▶️ <b>{mention} started the bot</b>\n@{username} "
               "(<code>{id}</code>)",
               {{"mention", mention},
                {"username", username},
                {"id", std::to_string(user_id)}});
}
inline std::string loggerPlaybackText(const std::string& track,
                                      const std::string& track_url,
                                      const std::string& source,
                                      const std::string& group,
                                      int64_t group_id,
                                      const std::string& by,
                                      int64_t by_id,
                                      const std::string& state) {
    return fmt("<b>🎧 Track:</b> <a href=\"{url}\">{track}</a>\n"
               "<b>🔗 Source:</b> {source}\n"
               "<b>📌 Group:</b> {group} ({gid})\n"
               "<b>👤 Requested by:</b> {by} (<code>{bid}</code>)\n"
               "{state}",
               {{"url", track_url},
                {"track", track},
                {"source", source},
                {"group", group},
                {"gid", std::to_string(group_id)},
                {"by", by},
                {"bid", std::to_string(by_id)},
                {"state", state}});
}

// /restart (Yukki/Tina/Parvati, §10).
inline const char* kRestarting = "♻️ Restarting...";
inline const char* kRestartOk = "✅ Restart initiated successfully!";
inline std::string restartFailedText(const std::string& error) {
    return fmt("❌ Failed to restart: {error}", {{"error", error}});
}
inline std::string restartNoticeText(const std::string& bot) {
    return fmt("{bot} has just restarted. Sorry for the issues.\n\nStart "
               "playing again after 10–15 seconds.",
               {{"bot", bot}});
}

// /eval + /sh (NoxBeats §1.9, §10).
inline const char* kEvalError = "⚠️ Error executing snippet\n\n";
inline const char* kEvalNoInput = "What?";
inline std::string evalOutputText(const std::string& out) {
    return fmt("<b>Output:</b>\n<code>{out}</code>", {{"out", out}});
}

// /speedtest result (Tina staged progress + NoxBeats form, §10).
inline const char* kSpeedtestDl = "🚀 Running download test...";
inline const char* kSpeedtestUp = "📤 Running upload test...";
inline std::string speedtestResultText(const std::string& down,
                                       const std::string& up,
                                       const std::string& ping) {
    return fmt("<b>Speed test</b>\n\nDownload: <b>{down}</b> Mbps\nUpload: "
               "<b>{up}</b> Mbps\nPing: <b>{ping}</b> ms",
               {{"down", down}, {"up", up}, {"ping", ping}});
}
inline const char* kSpeedtestFailed = "» The speed test failed.";

// /bug (NoxBeats/Tina/Parvati, §10).
inline const char* kBugUsage =
    "⚠️ Please describe the bug. Usage: {cmd} <problem> or reply to a "
    "message.";
inline const char* kBugThanks =
    "✅ Thank you for your bug report! It has been sent to the developers.";
inline const char* kBugReport =
    "<b>Bug report</b>\n\nFrom: {from} (<code>{id}</code>)\nWhere: {where}\n\n{text}";
inline const char* kBugFailed = "» Could not send the report. Try again later.";

// /privacy (Yukki §3.8 full page, §10; bot name + links substituted).
inline const char* kPrivacy =
    "<b>🛡 Privacy Policy &amp; Data Handling</b>\n"
    "\n"
    "Parlay is built with privacy in mind. Below is exactly what it "
    "stores, why it is stored, and how you stay in control.\n"
    "\n"
    "<b>📦 Data We Store</b>\n"
    "Only the information required to run the bot — nothing more:\n"
    "• <b>User &amp; chat IDs</b> — Identify groups and users so your "
    "settings apply\n"
    "• <b>Preferences</b> — Language, play mode, clean mode, admin mode, "
    "thumbnails\n"
    "• <b>Access lists</b> — Authorized users, sudo users, and the "
    "blacklist\n"
    "\n"
    "<b>💬 Messages</b>\n"
    "The bot reads <b>only</b> messages that begin with one of its "
    "commands (for example <code>/play</code>) or button presses on its "
    "own messages. It <b>never</b> reads, stores, or monitors your private "
    "chats or ordinary group conversations.\n"
    "\n"
    "<b>🌐 Third-Party Services</b>\n"
    "To find and stream the music you request, the bot sends the search "
    "query to services such as <b>YouTube</b> and <b>Spotify</b>. Only the "
    "query itself is sent — <b>no personal data</b> is shared with these "
    "services.\n"
    "\n"
    "<b>🔒 Data Sharing</b>\n"
    "Your data is <b>never</b> sold, shared, or traded with third parties. "
    "It is used strictly to operate and improve the bot's music streaming "
    "features.\n"
    "\n"
    "<b>🗑 Data Removal</b>\n"
    "Access can be cleaned up at any time: the owner can manage authorized "
    "users, sudo users, and the blacklist. You may also contact support to "
    "request removal of stored data.\n"
    "\n"
    "<i>Parlay is an open-source project. If you have any questions, feel "
    "free to join our Support Chat.</i>";

// Assistant errors (Tina/Yukki/Parvati err_* family, §10).
inline const char* kErrAssistantBanned =
    "⚠️ <b>Assistant Banned</b>\n"
    "\n"
    "The assistant has been banned in this chat.\n"
    "\n"
    "✅ Unbanned them already? Use /reload to refresh.";
inline const char* kErrAssistantAdminPerm =
    "⚠️ <b>Assistant Needs Admin</b>\n"
    "\n"
    "The assistant must be an admin in this chat to play music.";
inline const char* kErrAssistantGetFailed =
    "⚠️ Could not get the assistant account. Try again later.";
inline const char* kErrJoinRequestSent =
    "⚠️ A join request was sent for the assistant. Approve it, then use "
    "/reload.";
inline const char* kErrInviteLinkFetch =
    "⚠️ Could not fetch an invite link for this chat.";
inline const char* kErrInviteFailed =
    "⚠️ Failed to invite the assistant to this chat.";
inline const char* kErrConnectionTimeout =
    "⚠️ Connection timed out while reaching the assistant.";
inline const char* kErrNoActiveVc =
    "🎙️ <b>No Active Voice Chat</b>\n"
    "\n"
    "Start a voice chat first, then use /play.\n"
    "\n"
    "Use /reload to refresh.";
inline const char* kErrFetchFailed = "⚠️ Failed to fetch. Try again later.";
inline const char* kErrUnknown = "⚠️ An unknown error occurred.";
inline const char* kErrFetchTracks = "⚠️ Failed to fetch tracks.";
inline const char* kErrDownloadInternal =
    "⚠️ Internal download error. Try again later.";
inline const char* kErrRtmpMissingParams =
    "⚠️ RTMP is missing parameters. Use /setrtmp in DM first.";
inline std::string assistantRestrictedText(const std::string& assistant,
                                          int64_t id) {
    return fmt("⚠️ <b>Assistant Restricted</b>\n"
               "\n"
               "The assistant {name} (ID: <code>{id}</code>) has been "
               "<b>banned</b> or restricted in this chat.\n"
               "I cannot play music, manage the queue, or clean up tracks "
               "while this continues.\n"
               "\n"
               "<i>Unban the assistant to restore all music features 🎵</i>",
               {{"name", assistant}, {"id", std::to_string(id)}});
}
inline std::string playBannedText(const std::string& name, int64_t id,
                                  const std::string& username) {
    return fmt("{name} is banned in your chat\nID: <code>{id}</code>\n"
               "Name: {name}\nUsername: @{username}",
               {{"name", name},
                {"id", std::to_string(id)},
                {"username", username}});
}

// Flood / rate limit (Tina/Yukki/Arc, §10).
inline std::string floodSecondsText(int seconds) {
    return fmt("⏳ Slow down! Try again in {n} seconds.",
               {{"n", std::to_string(seconds)}});
}
inline std::string floodMinutesText(int minutes) {
    return fmt("⏳ Please wait {n} minutes before using this command again.",
               {{"n", std::to_string(minutes)}});
}

// /userbot (NoxBeats §1.7, §10).
inline const char* kUserbotReady = "» The assistant is ready.";
inline std::string userbotAlreadyText(const std::string& name) {
    return fmt("» {name} is already in this chat.", {{"name", name}});
}
inline std::string userbotJoinedText(const std::string& name) {
    return fmt("» {name} joined the chat.", {{"name", name}});
}

// Anti-spam ban (tmb-rust §4.5, §10).
inline const char* kSpamBlocked = "🚫 You are blocked for spam.";

// Admin notifications (rustify §7.9, §10).
inline const char* kAdminNewUser =
    "🆕 <b>New user joined</b>\n\n<code>{id}</code> {link} {name} {surname} "
    "{username}\nRef Code: {ref_code}";
inline const char* kAdminSpotifyConnected =
    "✅ <b>User connected Spotify</b>\n\nName: {name}\nID: <code>{id}</code> "
    "<a href=\"tg://user?id={id}\">link</a>";

// --- §11 auth lists ----------------------------------------------------------
inline std::string authlistText(const std::vector<std::pair<std::string, int64_t>>& users,
                                int limit) {
    std::string out = fmt("<b>🔐 Authorized Users</b>\n\n👥 {count}/{limit} "
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
    "ℹ️ There are no authorized users in this chat.";
inline const char* kAuthlistFetching = "⏳ Fetching authorized users list...";
inline const char* kToastAuthRevoked = "🗑️ Authorization revoked.";
inline const char* kAuthNoUser =
    "⚠️ Please provide a user — use:\n{cmd} [user_id] or reply to a user's "
    "message.";
inline std::string authExtractFailText(const std::string& error) {
    return fmt("⚠️ Unable to get that user: <i>{error}</i>",
               {{"error", error}});
}
inline std::string authLimitText(int limit) {
    return fmt("⚠️ This chat has reached the maximum authorized users limit "
               "({limit}). Please remove someone before adding a new one.",
               {{"limit", std::to_string(limit)}});
}
inline const char* kAuthAlready =
    "⚠️ That user is already authorized — no need to add them again.";
inline const char* kAuthCannot =
    "⚠️ This user cannot be authorized.\nThe bot owner, the bot itself, or "
    "yourself do not require authorization.";
inline const char* kAuthIsAdmin =
    "⚠️ This user is already a chat administrator. No authorization is "
    "required.";
inline const char* kAuthBotUser =
    "🤖 You can't add a bot to the auth list — auth users must be human!";
inline std::string authAddedText(const std::string& user, int count,
                                 int limit) {
    return fmt("✅ Added {user}.\nNow {count}/{limit} authorized users.",
               {{"user", user},
                {"count", std::to_string(count)},
                {"limit", std::to_string(limit)}});
}
inline std::string authAddedAltText(const std::string& user) {
    return fmt("✅ Successfully added {user} to the authorized users list.",
               {{"user", user}});
}
inline std::string authRemovedText(const std::string& user) {
    return fmt("✅ Removed {user} from the authorized users list.",
               {{"user", user}});
}

// Sudo list (Tina/Yukki/Parvati, §11).
inline const char* kSudoHeader = "👑 <b>Current Sudoers:</b>";
inline const char* kSudoNoExtra = "⚠️ No additional sudoers found.";
inline const char* kSudoSelfJoke =
    "😂 Haha, you're the boss already! Why are you even trying to add "
    "yourself?";
inline const char* kSudoBotSelfJoke =
    "🤖 Haha, I'm the bot! I can't add myself to the sudo list — even the "
    "owner can't cheat me 😎";
inline const char* kSudoBotUserJoke =
    "🤖 You can't add a bot to the sudo list — sudoers must be human!";
inline const char* kSudoOwnerRemoveBlock =
    "😎 Nice try! You can't remove the owner — they're untouchable.";

// Blacklist (Tina/Yukki/Parvati, §11).
inline const char* kBlacklistTitle = "📛 <b>Blacklisted</b>";
inline const char* kBlChatBlocked =
    "🚫 This chat is blocked from using this bot.";
inline const char* kBlUserBlocked = "🚫 You are blocked from using this bot.";
inline const char* kBlChatOwnerBlocked =
    "🚫 This chat owner is blocked from using this bot. Leaving chat.";
inline std::string blBlockedUserText(int64_t id) {
    return fmt("✅ User blocked: <code>{id}</code>",
               {{"id", std::to_string(id)}});
}
inline std::string blFailedUnblockText(const std::string& error) {
    return fmt("⚠️ Failed to unblock user: {error}", {{"error", error}});
}

// --- §12 error messages -----------------------------------------------------
// Error catalogue (remix §2.1 house style, §12).
inline const char* kErrNoActiveCall = "No active call.";
inline const char* kErrNoVideo = "No video found.";
inline const char* kErrGeneric = "An error occurred.";
inline const char* kErrFileTooBig = "This file is too big.";
inline const char* kErrAssistantNotHere = "My assistant is not here.";
inline std::string errWithMessageText(const std::string& message) {
    return fmt("An error occurred: {message}", {{"message", message}});
}
inline const char* kErrNotStreaming =
    "» The bot isn't streaming in the video chat.";
inline const char* kErrNoActiveVcChat =
    "<b>No active video chat found.</b>\n\nPlease start one and <b>try "
    "again</b>.";
inline const char* kErrDownloadFailedSupport =
    "Download failed.\n\nIf the issue persists, report it to the <a "
    "href=\"{support}\">support chat</a>";
inline const char* kErrAudioSourceMissing =
    "» Moving to the next track...\n\nAudio source not found in the file.";
inline const char* kErrTelegramServer =
    "<u><b>Telegram server error</b></u>\n\nTelegram is experiencing "
    "internal issues. Clearing the queue and leaving the video chat.";
inline const char* kErrRtmpUnsupported =
    "» RTMP streaming is not supported. Please stop the RTMP stream and "
    "start a video chat instead.";

// Play guards (NoxBeats §1.9, §12).
inline const char* kGuardUserNotAdmin =
    "» Oh sure, go ahead and manage the video chat... oh wait, you're not "
    "an admin.";
inline const char* kGuardUserNoPerms =
    "» You? Manage the video chat? Not with those permissions, sweetie.";
inline const char* kGuardUserNotFound =
    "» I'll need a user ID — or would you mind replying to someone's "
    "message?";
inline const char* kGuardPlayAdmin =
    "<u><b>Admin only play</b></u>\n\nOnly admins are allowed to play in "
    "this chat.";
inline const char* kGuardPlayUserInvalid =
    "» You're a hidden admin.\n\nRevert back to user account.";
inline const char* kGuardPlayExpired = "» This button has expired.";
inline std::string guardInviteErrorText(const std::string& reason) {
    return fmt("Failed to invite assistant to the chat.\n\nReason: "
               "<code>{reason}</code>",
               {{"reason", reason}});
}
inline const char* kGuardAutoLeft =
    "» I waited, I watched, I yawned... and then I left the video chat.";

// Download / stream failures (TgMusicBot §5.5, Parvati §3.5, §12).
inline std::string errFetchTrackText(const std::string& err) {
    return fmt("❌ Error fetching track info: {err}", {{"err", err}});
}
inline const char* kErrNoTracks = "No tracks found.";
inline const char* kErrNoValidMedia = "No valid media found in the message.";
inline std::string errFileTooLargeText(int mb) {
    return fmt("File too large. Max size: {n} MB.",
               {{"n", std::to_string(mb)}});
}
inline const char* kErrTrackInQueue = "Track already in queue or playing.";
inline std::string errDownloadFailedText(const std::string& err) {
    return fmt("Download failed: {err}", {{"err", err}});
}
inline std::string errSearchFailedText(const std::string& err) {
    return fmt("❌ Search failed: {err}", {{"err", err}});
}
inline const char* kErrNoResultsTry =
    "😕 No results found. Try a different query.";
inline const char* kErrVideoDisabled =
    "Video playback is disabled in this chat. Audio-only playback is used "
    "instead.";
inline const char* kSupportedPlatforms =
    "Invalid URL or unsupported platform.\n"
    "\n"
    "<b>Supported Platforms:</b>\n"
    "- YouTube\n"
    "- Spotify\n"
    "- JioSaavn\n"
    "- Apple Music";

// Generic errors (beetube §3.9, lemonic §6.11, openradiobot §2.6, §12).
inline const char* kErrTryAgain = "🚫 An error occurred, try again later.";
inline const char* kErrListEmpty = "🚫 The list is empty or limit reached.";
inline const char* kErrNoResultsPlain = "🚫 No results found.";
inline const char* kErrInputTooShort = "🚫 Input must be more than a character.";
inline const char* kErrSomethingWrong = "Something went wrong";
inline const char* kErrUncaught =
    "An error has occurred. Please try again later. If this issue persists, "
    "please contact the bot developer.";
inline std::string errOccurredText(const std::string& error) {
    return fmt("An error occurred: {error}", {{"error", error}});
}
inline const char* kErrRadioNoResult = "🙅 No Result.";
inline const char* kErrRegionLocked =
    "❌ Sorry. We can't play this video due to our server region lock.";
inline const char* kErrUnsupportedAttachment = "Unsupported Formats";
inline const char* kErrReportIssue =
    "<b>Sorry, an error has occurred :(</b>\n\n<a href=\"https://github.com/"
    "filewrap/ParlayMusic/issues/new\">Report an issue on GitHub</a>";

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
inline const char* kInlineHelpMessage = "Parlay v0.2.0";
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
        {{"💀 Music"}, {"🔍 Global Search"}, {"⚙ Settings", "📌 About"}},
        true, false};
}
inline const char* kKeyboardEnabled = "⌨ Keyboard enabled.";
// Start reply keyboard (rustify §7.2, §14) — shown after /start when the
// user is logged in to Spotify; presses match by exact text.
inline ReplyKeyboard spotifyStartKeyboard() {
    return ReplyKeyboard{{{{"👎 Dislike playing track"}},
                          {{"📈 Stats"}, {"🎤 Details of track"}}}};
}
inline const char* kLangChoose = "Choose your language";
inline const char* kLangNowEnglish = "Your language is now English 🇺🇸";

// --- §16 stats / ping / alive ----------------------------------------------
inline std::string pingCardText(const std::string& bot,
                                const std::string& latency_ms,
                                const std::string& uptime,
                                const std::string& ram_info,
                                const std::string& cpu_usage,
                                const std::string& disk_usage,
                                const std::string& ntgcalls_ms) {
    std::string out = fmt("<b>🏓 Pong!</b> • <code>{lat}ms</code>\n\n<b>🤖 "
                          "{bot}</b>\n<blockquote>⏱️ <b>Uptime:</b> "
                          "<code>{up}</code>\n💾 <b>RAM:</b> "
                          "<code>{ram}</code>\n🖥️ <b>CPU:</b> "
                          "<code>{cpu}</code>\n💽 <b>Disk:</b> "
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
inline const char* kPingFirst = "🏓 Pinging...";

// /stats card (Yukki §3.8 wins, §16). {system}/{memory}/{server}/{bot}
// are pre-built blocks; {library} is the navidrome section (empty when
// no library is present).
inline const char* kStatsCard =
    "<b>📊 Bot Statistics</b>\n"
    "\n"
    "<i>Live overview of the host, resources and database.</i>\n"
    "\n"
    "<b>🔧 System</b>\n"
    "{system}\n"
    "\n"
    "<b>📦 Memory</b>\n"
    "{memory}\n"
    "\n"
    "<b>💻 Server</b>\n"
    "{server}\n"
    "\n"
    "<b>👥 Bot Overview</b>\n"
    "{bot}\n"
    "\n"
    "<b>🧩 Versions</b>\n"
    "{versions}"
    "{library}";

inline std::string libraryStatsText(int artists, int albums, int tracks,
                                    const std::string& size_gb) {
    return fmt("\n\n<b>📚 Library</b>\nArtists: {a}\nAlbums: {al}\nTracks: "
               "{t}\nSize: {s} GB",
               {{"a", std::to_string(artists)},
                {"al", std::to_string(albums)},
                {"t", std::to_string(tracks)},
                {"s", size_gb}});
}

// /status (Music163bot §6.9, §16).
inline const char* kStatusText =
    "<b>[Statistics]</b>\n"
    "Total cached songs in database: {total}\n"
    "Cached songs in current chat [{chat}]: {chatCount}\n"
    "Cached songs for current user [<a href=\"tg://user?id={userID}\">"
    "{userID}</a>]: {userCount}";

// /about (Music163bot §6.9, §16; Parlay values substituted).
inline const char* kAboutBuild =
    "<b>Parlay v0.2.0</b>\n"
    "Github: https://github.com/filewrap/ParlayMusic\n"
    "\n"
    "[Build environment] {runtime}\n"
    "[Build version] {version}\n"
    "[Build hash] {commit}\n"
    "[Build date] {buildTime}\n"
    "[Runtime] {arch}";

// --- §17 playlists ----------------------------------------------------------
// Personal playlist, single (NoxBeats model, §17).
inline const char* kPlaylistEmpty =
    "» Your playlist is empty. Add songs with <code>/addplaylist [song name "
    "or link]</code>.";
inline const char* kAddPlaylistUsage =
    "» Usage: <code>/addplaylist [song name | YouTube / Spotify / Apple "
    "Music / JioSaavn / SoundCloud link]</code>\nWhile a song is playing, "
    "<code>/addplaylist</code> alone saves that song.";
inline const char* kPlaylistAdding = "» Adding to your playlist...";
inline std::string playlistAddedText(const std::string& title, int total) {
    return fmt("» Added <b>{title}</b> to your playlist. You now have "
               "{total} songs.",
               {{"title", title}, {"total", std::to_string(total)}});
}
inline std::string playlistAddedManyText(int added, int dup, int total) {
    return fmt("» Added <b>{added}</b> songs to your playlist ({dup} were "
               "already there). You now have {total} songs.",
               {{"added", std::to_string(added)},
                {"dup", std::to_string(dup)},
                {"total", std::to_string(total)}});
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
inline const char* kPlaylistFetching =
    "» Fetching the playlist...\n\nPlease hold on.";
inline const char* kPlaylistFetchError =
    "» Something went wrong while fetching the playlist.";
inline std::string playlistQueuedText(int count) {
    return fmt("<u><b>Added {n} tracks from the playlist to queue:</b></u>\n\n",
               {{"n", std::to_string(count)}});
}

// Named playlists (Parvati/TgMusicBot model, §17).
inline const char* kCreatePlaylistUsage =
    "Usage: <code>/createplaylist &lt;name&gt;</code>";
inline const char* kNamedPlaylistLimit =
    "⚠️ You have reached the maximum limit of 10 playlists.";
inline std::string namedPlaylistCreatedText(const std::string& name,
                                            const std::string& id) {
    return fmt("✅ Playlist <b>{name}</b> has been created successfully.\n"
               "ID: <code>{id}</code>",
               {{"name", name}, {"id", id}});
}
inline std::string namedPlaylistCreateFailText(const std::string& error) {
    return fmt("❌ Failed to create playlist: {error}", {{"error", error}});
}
inline const char* kNamedPlaylistNotFound =
    "❌ The specified playlist could not be found. Please check the "
    "playlist ID.";
inline const char* kNamedPlaylistNotOwner =
    "⚠️ You can only manage playlists that you created.";
inline std::string namedPlaylistDeletedText(const std::string& name) {
    return fmt("🗑️ Playlist <b>{name}</b> has been deleted successfully.",
               {{"name", name}});
}
inline const char* kNamedPlaylistBadNumber = "⚠️ Invalid song number.";
inline const char* kNamedPlaylistSongNotFound = "❌ Song not found in playlist.";
inline std::string namedPlaylistAddedText(const std::string& song,
                                          const std::string& playlist) {
    return fmt("✅ Track <b>{song}</b> has been added to playlist "
               "<b>{playlist}</b>.",
               {{"song", song}, {"playlist", playlist}});
}
inline std::string namedPlaylistRemovedText(const std::string& name) {
    return fmt("🗑️ Song removed from playlist <b>{name}</b>.",
               {{"name", name}});
}
inline const char* kPlaylistInfoCard =
    "<b>Playlist Info</b>\n\n<b>Name:</b> {name}\n<b>Owner:</b> {owner}\n"
    "<b>Songs:</b> {count}\n\n{songs}";
inline const char* kPlaylistInfoEmpty =
    "<i>This playlist has no songs yet.</i>";
inline const char* kMyPlaylistsNone =
    "📭 You do not have any playlists yet. Use /createplaylist to make one.";
inline const char* kMyPlaylistsHeader = "<b>My Playlists</b>\n\n{name} (<code>{id}</code>)";

// Playlist fetch from link (NoxBeats §1.10 + remix §2.6, §17).
inline std::string playlistLinkStreamText(int count) {
    return fmt("🎶 | <b>Streaming and queuing {x} items...</b>",
               {{"x", std::to_string(count)}});
}
inline std::string playlistLinkQueueText(int count) {
    return fmt("🎶 | <b>Queuing {x} items...</b>",
               {{"x", std::to_string(count)}});
}

// /mix (TgMusicBot §5.5, §17).
inline const char* kMixUsage =
    "<b>Usage:</b>\n/mix [song or URL]\n\nOr run /mix while a song is "
    "currently playing to create a mix based on it.";
inline const char* kMixFetching = "Fetching mix tracks...";
inline const char* kMixFailed = "Failed to get mix: Please try again later.";
inline const char* kMixNoTracks = "No tracks found for mix.";
inline const char* kMixAllQueued =
    "All mix tracks are already in the queue or playing.";

// --- §18 broadcast ----------------------------------------------------------
// Progress card (Parvati wins, §18), edited in place.
inline const char* kBcastInit =
    "📡 Initializing broadcast...\n\nPlease wait...";
inline const char* kBcastProgress =
    "📡 Broadcasting...\n"
    "📊 Total Chats: {doneC}/{totalC} ({pctC}%)\n"
    "👥 Total Users: {doneU}/{totalU} ({pctU}%)\n"
    "❌ Failed Chats: {failC}\n"
    "❌ Failed Users: {failU}\n"
    "⏱ Delay: {delay}s\n"
    "⏰ Elapsed: {elapsed}\n"
    "⏳ ETA: {eta}\n"
    "✨ Success Rate: {rate}% ({sent}/{total})";
inline const char* kBcastDone = "✅ Broadcast Completed";
inline const char* kBcastCancelled = "🚫 Broadcast Cancelled";
inline const char* kBcastCancelBtn = "🚫 Cancel Broadcast";
inline InlineKeyboard bcastKeyboard() {
    return {{InlineButton::callback(kBcastCancelBtn, kCbBcastCancel)}};
}
inline const char* kBcastCancelOk = "🚫 Broadcast cancelled successfully.";
inline const char* kBcastCancelOwner =
    "⚠️ Only the owner can cancel broadcasts.";
inline const char* kBcastCancelNone = "ℹ️ No broadcast is currently running.";
inline const char* kBcastCancelDone = "🚫 Broadcast cancelled.";
inline const char* kBcastAlreadyRunning =
    "⚠️ A broadcast is already running.\n"
    "\n"
    "Please wait for it to complete or cancel it first using:\n"
    "• <code>/broadcast -cancel</code>\n"
    "• Or click the Cancel button on the progress message";
inline std::string bcastParseFailText(const std::string& error) {
    return fmt("❌ Failed to parse broadcast command:\n<code>{error}</code>",
               {{"error", error}});
}
inline const char* kBcastNoContent =
    "⚠️ No content provided for broadcast.\n"
    "\n"
    "<b>Usage:</b> <code>{cmd} [text or reply to message]</code>\n"
    "\n"
    "<b>Example:</b>\n"
    "<code>{cmd} Hello everyone!</code>";
inline const char* kBcastNoTargets =
    "⚠️ No targets found for broadcast.\n"
    "\n"
    "Make sure you haven't excluded all targets with flags.";
inline const char* kBcastReplyUsage =
    "» Reply to a message for broadcasting the message.";
inline const char* kBcastActive = "» Please wait for the ongoing broadcast to finish.";
inline const char* kBcastStarted = "» Broadcasting started.";
inline std::string bcastEndText(int groups, int users) {
    return fmt("» Broadcasted the message to {g} groups and {u} users.",
               {{"g", std::to_string(groups)},
                {"u", std::to_string(users)}});
}
inline const char* kBcastNoProgress = "Broadcast started.";
inline const char* kBcastStopped = "Broadcast stopped.";

// --- §19 maintenance --------------------------------------------------------
inline const char* kMaintUser =
    "⚠️ I'm under maintenance at the moment and temporarily unavailable. "
    "Please check back later.\n\n{reason}";
inline const char* kMaintReasonLine = "<i>📝 Reason: {reason}</i>";
inline const char* kMaintUsage =
    "Maintenance is <b>{state}</b> (reason: {reason}).\nUsage: "
    "<code>/maintenance [on|off] [reason]</code>";
inline std::string maintEnabledText(const std::string& reason) {
    return fmt("Maintenance mode enabled. Reason: {reason}",
               {{"reason", reason}});
}
inline const char* kMaintDisabled = "» Maintenance mode disabled.";
inline const char* kMaintAlreadyOn =
    "ℹ️ Maintenance mode is already enabled 🟢.";
inline const char* kMaintAlreadyOff =
    "ℹ️ Maintenance mode is already disabled.";
inline const char* kMaintEntering = "⚠️ Bot is entering maintenance mode...";
inline const char* kMaintEnabledReason =
    "🟢 Enabled\n📝 Reason: {reason}";
inline const char* kMaintCardTitle = "🛠️ Maintenance";

// --- §20 channel-play -------------------------------------------------------
inline const char* kCplayUsage =
    "<b>Usage:</b> <code>/setcplay &lt;channel_id&gt;</code>";
inline const char* kCplayNotSet =
    "⚠️ cplay ID not set.\nUse <code>/setcplay &lt;chat_id&gt;</code> to set "
    "it.";
inline const char* kCplayResolveFail =
    " <b>Failed to resolve peer:</b> Could not fetch channel details. "
    "Ensure I can access this channel.";
inline const char* kCplayInvalidTarget =
    "<b>Invalid Target:</b> The provided ID is not a valid channel.";
inline const char* kCplayNotAccessible =
    "<b>Channel Not Accessible:</b> Could not retrieve channel information. "
    "Please ensure I am an administrator in the target channel.";
inline const char* kCplayMissingInvite =
    "⚠️ <b>Missing permission:</b> I am admin in target channel but "
    "<code>invite users</code> permission is disabled. Please enable it and "
    "try again.";
inline const char* kCplayNotLinked =
    "⚠️ <b>Linked channel not found:</b> This chat is not linked to any "
    "channel / group.";
inline const char* kCplaySaveError = "<b>Error:</b> Failed to save cplay settings.";
inline std::string cplayEnabledText(int64_t channel_id) {
    return fmt("✅ <b>Channel Play enabled.</b> All channel-play commands "
               "(like <code>/cplay</code>, <code>/cskip</code>) will now work "
               "in channel <code>{id}</code>.",
               {{"id", std::to_string(channel_id)}});
}
inline const char* kCplayDisabled =
    "✅ <b>Channel Play disabled.</b> Channel-play commands (like "
    "<code>/cplay</code>, <code>/cskip</code>) are now turned off for this "
    "chat.";
inline const char* kCplayOwnerRequired =
    "⚠️ Only the owner of this chat or the target channel can use this "
    "setup.";
inline const char* kCplayOwnerCheckFail =
    "⚠️ Could not verify chat ownership right now. Please try again.";
inline std::string cplayStatusText(int64_t channel_id) {
    return fmt("» Channel play is linked to <code>{id}</code>.",
               {{"id", std::to_string(channel_id)}});
}

// RTMP / /stream (Arc guide wins, NoxBeats short usage, §20).
inline const char* kRtmpUsageDm =
    "Usage (in private chat):\n"
    "<code>/setrtmp [chat id] [rtmp://host/app] [stream key]</code>\n"
    "<code>/setrtmp [chat id] off</code>";
inline const char* kRtmpGuide =
    "<b>RTMP Setup</b>\n"
    "\n"
    "<b>Parameters:</b>\n"
    "• <code>[chat id]</code> — the group chat ID to stream into\n"
    "• <code>[rtmp://host/app]</code> — your RTMP ingest URL\n"
    "• <code>[stream key]</code> — your stream key (never shown in chat)\n"
    "\n"
    "<b>Example:</b>\n"
    "<code>/setrtmp -1001234567890 rtmp://live.example.com/app "
    "my-secret-key</code>\n"
    "\n"
    "<b>How to get the chat ID:</b> add the bot to the group and run "
    "<code>/id</code> there.\n"
    "\n"
    "🔒 <b>Security Notice:</b> only set RTMP targets in a private chat "
    "with the bot. Never share your stream key in a group.";
inline const char* kRtmpDmOnly = "⚠️ Use this command in DM.";
inline std::string rtmpConfiguredText(int64_t chat_id,
                                      const std::string& url) {
    return fmt("✅ RTMP configured successfully for Chat ID: "
               "<code>{id}</code>\nURL: <code>{url}</code>",
               {{"id", std::to_string(chat_id)}, {"url", url}});
}
inline const char* kRtmpSaved =
    "» RTMP target saved. Use /stream [song/url] in that chat.";
inline const char* kRtmpRemoved = "» RTMP target removed.";
inline const char* kRtmpNotSet =
    "» No RTMP target is set for this chat. Use /setrtmp in my private chat.";
inline const char* kRtmpInvalidUrl =
    "» The URL must start with <code>rtmp://</code> or <code>rtmps://</code>.";
inline const char* kRtmpNotAdmin = "» You must be an admin of that chat.";
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
inline const char* kRtmpStatusCard =
    "📡 <b>RTMP Status</b>\n▫ State: ▶️ Streaming\n▫ Position: {position}";
inline std::string rtmpStoppedText(const std::string& by) {
    return fmt("Stream stopped.\n\n» By {by}", {{"by", by}});
}
inline const char* kRtmpNone = "» No stream is running here.";
inline const char* kRtmpNoneActive = "ℹ️ No active RTMP stream.";
inline const char* kRtmpDelayNote =
    "⚠️ Note: RTMP has ~15-30s buffering delay";
inline const char* kRtmpStreamUsage = "Usage: <code>/stream [name or url]</code>";
inline const char* kRtmpAlreadyRunning =
    "» A stream is already running here. Stop it with /streamstop.";
inline const char* kRtmpStartFailed = "» Could not start the stream.";
inline const char* kRtmpRefusal =
    "⚠️ <b>Can't play in an RTMP/live-stream voice chat.</b>\n\n<i>Playback "
    "stopped.</i>";

// --- §21 radio --------------------------------------------------------------
inline const char* kRadioCreated = "✔️ Radio Created";
inline const char* kRadioExists =
    "You already created your radio. To manage it, type /manage. To destroy "
    "it, type /destroy";
inline const char* kRadioDestroyed = "✔️ Radio destroyed.";
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
    "Live on: <a href=\"{host}/{chatId}\">{host}/{chatId}</a>\n"
    "\n"
    "To check song queue, type /queue";
inline const char* kRadioQueueEmpty = "🏜️ Nothing is in queue....";
inline const char* kRadioQueueHeader = "<b>Radio Queue</b>";
inline const char* kRadioQueueFooter =
    "\n\n⚠️ Some songs are hidden due to a large number of requests. We'll "
    "improve this soon.\n\nYou may also manage this queue. For more "
    "information, do <code>/queue help</code>";
inline const char* kRadioPlayUsage =
    "Usage: <code>/play [Song name|URL|Reply to Audio/Voice Message]</code>";
inline std::string radioSearchingText(const std::string& q) {
    return fmt("Searching <code>{q}</code>...", {{"q", htmlEscape(q)}});
}
inline const char* kRadioPreparing = "Preparing to play...";
inline std::string radioNowPlayingText(const std::string& title,
                                       const std::string& url) {
    return fmt("▶️ Now playing: <a href=\"{url}\">{title}</a>",
               {{"url", url}, {"title", title}});
}
inline std::string radioNowPlayingRawText(const std::string& url) {
    return fmt("▶️ Now Playing: <a href=\"{url}\">Raw Stream</a>",
               {{"url", url}});
}
inline const char* kRadioNowVoice = "▶️ Playing Voice/Audio Message....";
inline const char* kRadioQueuedVoice = "✔️ Voice has been added to queue";
inline std::string radioQueuedPlaylistText(int count) {
    return fmt("✔️ {n} songs have been added to queue.",
               {{"n", std::to_string(count)}});
}
inline const char* kRadioQueuedRaw =
    "✔️ A stream URL has been added to queue.";
inline std::string radioQueuedVideoText(const std::string& title,
                                        const std::string& url) {
    return fmt("✔️ <a href=\"{url}\">{title}</a> has been added to queue.",
               {{"url", url}, {"title", title}});
}
inline const char* kRadioPaused = "⏸️ Paused";
inline const char* kRadioResumed = "▶️ Resumed";
inline const char* kRadioSkipping = "⏩ Skipping...";
inline const char* kRadioStopped = "⏹️ Player Stopped";
inline const char* kRadioNothingPlaying =
    "There's nothing playing. Glitched? Do /destroy";
inline const char* kRadioSkipEmpty = "There's nothing in queue!";
inline const char* kRadioAutoplayOn = "✔️ Autoplay enabled";
inline const char* kRadioAutoplayOff = "✔️ Autoplay disabled";
inline const char* kRadioAutoplayRaw =
    "Sorry. You can't use autoplay right now.";
inline const char* kRadioLoopUsage = "Usage: <code>/loop [queue|single|none]</code>";
inline std::string radioLoopSetText(const std::string& loop_type) {
    return fmt("✔️ Loop Type has been set as <code>{t}</code>",
               {{"t", loop_type}});
}
inline const char* kRadioGroupOnly = "This Command works on Group Only";
inline const char* kRadioNeedLink = "Please provide a radio/stream link";
inline const char* kRadioInvalid = "Invalid";
inline const char* kRadioStationsHeader = "📻 <b>Radio stations</b>";
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
    "🎵 Shared track: {track}\n\nUse /play {title} in a group to listen.";
inline const char* kShareHelp =
    "Send Music: You can send your music by clicking on a user's unique "
    "link\n"
    "\n"
    "Get your own link: You can get your link by clicking on the 'Get Link' "
    "button\n"
    "\n"
    "Available commands:\n"
    "\n"
    "/start: Start the bot and send music or get your link\n"
    "\n"
    "/help: See this Description again :)";
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
inline const char* kShareServerError =
    "Something went wrong on server side\nPlease try again";
inline const char* kShareInvalidCommand = "Invalid Command";
inline const char* kShareInvalidMessage = "Invalid Message";
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
inline std::string voteRequestedText(const std::string& track_card) {
    return fmt("✅ Successfully requested track.\n\n{card}",
               {{"card", track_card}});
}
inline std::string votePostText(const std::string& requester,
                                const std::string& track_card) {
    return fmt("User {name} requested:\n{card}",
               {{"name", requester}, {"card", track_card}});
}
inline std::string voteTrackCard(const std::string& name,
                                 const std::string& duration,
                                 const std::string& artists,
                                 const std::string& album,
                                 const std::string& spotify_url,
                                 const std::string& cover_url) {
    std::string out = fmt("🎵 <b>{name}</b> • ⏱️ {dur}\n👥 <b>{artists}</b>\n"
                          "💿 <b>{album}</b>\n<a href=\"{url}\">Listen</a>",
                          {{"name", name},
                           {"dur", duration},
                           {"artists", artists},
                           {"album", album},
                           {"url", spotify_url}});
    if (!cover_url.empty())
        out += " • <a href=\"" + cover_url + "\">Cover</a>";
    return out;
}
inline InlineKeyboard voteKeyboard(const std::string& track_id) {
    using B = InlineButton;
    return {{B::callback("✅ Add to queue", cbVote("approve", track_id)),
             B::callback("❌ Decline", cbVote("decline", track_id))}};
}
inline std::string voteAcceptedText(const std::string& voter,
                                    const std::string& original) {
    return fmt("✅ {voter} has added to queue:\n{orig}",
               {{"voter", voter}, {"orig", original}});
}
inline std::string voteQueueFailText(const std::string& error) {
    return fmt("Failed to queue track: {error}", {{"error", error}});
}
inline const char* kVoteBadId = "Invalid track ID";
inline std::string voteDeclinedText(const std::string& voter,
                                    const std::string& original) {
    if (original.empty())
        return fmt("❌ {voter} declined.", {{"voter", voter}});
    return fmt("❌ {voter} declined: {orig}",
               {{"voter", voter}, {"orig", original}});
}
inline const char* kVoteFetchFail = "❌ Failed to find track.";
inline const char* kSpotifyLoginPrompt =
    "Spotify Login\nOpen this URL in the browser and allow Spotify access: "
    "{auth_url}\nThen paste and send the redirected URL here.";
inline std::string spotifyLinkErrorText(const std::string& error) {
    return fmt("Spotify link error: {error}", {{"error", error}});
}
inline const char* kSpotifyTokenSaved = "Token saved";
inline const char* kSpotifyInvalidCode = "Invalid Code/URL";
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
inline std::string deleteConfirmAlbumText(const std::string& label, int n) {
    return fmt("Delete {label} ({n} tracks)?",
               {{"label", label}, {"n", std::to_string(n)}});
}
inline std::string deleteConfirmTrackText(const std::string& label) {
    return fmt("Delete {label}?", {{"label", label}});
}
inline InlineKeyboard deleteConfirmKeyboard(const std::string& id) {
    using B = InlineButton;
    return {{B::callback("🗑 Delete", cbDel("yes", id)),
             B::callback("✖ Cancel", cbDel("no", id))}};
}
inline const char* kToastDelNotAllowed = "Not allowed.";
inline const char* kToastDelExpired = "Prompt expired.";
inline const char* kToastDelCancelled = "Cancelled.";
inline const char* kDelCancelled = "Cancelled.";
inline std::string delChangedText(const std::string& path) {
    return fmt("Changed since the question, not deleted: {path}",
               {{"path", path}});
}
inline std::string delDoneText(const std::string& label,
                               const std::string& scan_note) {
    return fmt("Deleted: {label}\n{note}",
               {{"label", label}, {"note", scan_note}});
}
inline const char* kDelInvalidPath = "Invalid path.";
inline const char* kDelTopLevel = "Cannot delete top-level directories.";
inline std::string delNotFoundText(const std::string& path) {
    return fmt("Not found: {path}", {{"path", path}});
}

// /scan / /sharescan (navidrome §7.5, §25).
inline const char* kScanTriggered = "Library scan triggered.";
inline const char* kScanNotConfigured =
    "Library scan not configured (NAVIDROME_USER/NAVIDROME_PASS not set).";
inline const char* kScanBadCreds = "Library scan failed: wrong credentials.";
inline const char* kScanFailed = "Library scan failed.";
inline const char* kShareScanTriggered = "slskd share scan triggered.";
inline std::string shareScanScheduledText(int mins) {
    return fmt("slskd share scan scheduled in ~{n}m.",
               {{"n", std::to_string(mins)}});
}

// /recognize (Music163bot §6.8, §25).
inline const char* kRecognizeNeedVoice =
    "Please send a voice message containing a music clip first, then reply "
    "to it with /recognize.";
inline const char* kRecognizeStatus = "Recognizing music, please wait...";
inline const char* kRecognizeUnreadable =
    "Can't read this voice message right now\nPlease try again later, or "
    "re-send the voice message.";
inline const char* kRecognizeNone =
    "No song recognized yet\nPlease try recording a clearer, slightly "
    "longer music clip, then try again.";
inline const char* kRecognizeFailed =
    "Music recognition failed temporarily, please try again later.";
inline const char* kRecognizeOk =
    "🎵 Recognized\n「{name}」- {artists}\nAlbum: {album}";

// /rmcache (Music163bot §6.9, §25).
inline const char* kRmCacheNotCached = "Song not cached";
inline std::string rmCacheOkText(const std::string& name) {
    return fmt("Cleared [{name}] cache successfully", {{"name", name}});
}

}  // namespace parlay::ui_texts
