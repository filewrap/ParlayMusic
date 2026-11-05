// ui_test.cpp — Phase 9: pure ui_texts helpers + keyboard layouts +
// callback-data strings. No network, no dispatcher: every check is a
// pure function of its inputs.
//
// Build: g++ -std=c++20 -I src tests/ui_test.cpp src/scram.cpp
//   src/json.cpp src/config.cpp src/toml.cpp
//   -o /tmp/ui_test && /tmp/ui_test
// Exit 0 = all checks pass.
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "ui_texts.hpp"

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            std::cout << "PASS " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << "\n";                           \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

using parlay::InlineKeyboard;
using namespace parlay::ui_texts;

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int countChar(const std::string& s, const std::string& needle) {
    int n = 0;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

bool isHex(const std::string& s) {
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

}  // namespace

int main() {
    // --- helpers ---------------------------------------------------------
    CHECK(mmss(0) == "0:00", "mmss: 0 -> 0:00");
    CHECK(mmss(40) == "0:40", "mmss: 40 -> 0:40");
    CHECK(mmss(185) == "3:05", "mmss: 185 -> 3:05");
    CHECK(mmss(3600) == "60:00", "mmss: 3600 -> 60:00");
    CHECK(durationStr(0) == "∞", "durationStr: 0 -> infinity");
    CHECK(durationStr(185) == "3:05", "durationStr: 185 -> 3:05");
    // Only &, <, > are escaped (Telegram HTML needs no more).
    CHECK(htmlEscape("<b>&") == "&lt;b&gt;&amp;",
          "htmlEscape: angle brackets and ampersand");
    CHECK(htmlEscape("a\"b'c") == "a\"b'c",
          "htmlEscape: quotes pass through unchanged");
    CHECK(htmlEscape("plain") == "plain", "htmlEscape: plain text unchanged");
    CHECK(fmt("Hi {name}, {n} songs",
              {{"name", "Bob"}, {"n", "3"}}) == "Hi Bob, 3 songs",
          "fmt: two placeholders");
    CHECK(fmt("No placeholders", {}) == "No placeholders",
          "fmt: no placeholders");
    CHECK(truncUtf8("abcdef", 3) == "abc\u2026", "truncUtf8: ascii cut + ellipsis");
    CHECK(truncUtf8("ab", 10) == "ab", "truncUtf8: short text unchanged");
    CHECK(truncUtf8("héllo", 2) == "hé\u2026",
          "truncUtf8: multibyte cut on boundary + ellipsis");

    // --- progress bar: 9 stages, exactly one ◉ --------------------------
    for (int i = 0; i <= 8; ++i) {
        // pos/dur chosen so the stage is exactly i (f = i/8).
        int64_t pos = i * 10, dur = 80;
        std::string bar = progressBar(pos, dur);
        CHECK(countChar(bar, "◉") == 1 && countChar(bar, "—") == 8,
              "progressBar: stage " + std::to_string(i) +
                  " has one marker, eight dashes");
    }
    CHECK(progressBar(0, 0) == "◉————————", "progressBar: zero duration -> stage 0");
    CHECK(progressBar(999, 100) == "————————◉", "progressBar: past end clamps to stage 8");
    CHECK(progressBar(-5, 100) == "◉————————", "progressBar: negative clamps to stage 0");
    CHECK(contains(progressLabel(90, 200), "1:30") &&
              contains(progressLabel(90, 200), "3:20"),
          "progressLabel: position + bar + duration");
    CHECK(progressLabel(90, 200).find("◉") != std::string::npos,
          "progressLabel: contains the marker");

    // --- tokens ----------------------------------------------------------
    CHECK(trackToken("abc123") == "abc123",
          "trackToken: short id without ':' passes through");
    CHECK(trackToken("a:b:c") != "a:b:c" && trackToken("a:b:c").size() == 10 &&
              isHex(trackToken("a:b:c")),
          "trackToken: id with ':' hashes to 10 hex");
    CHECK(trackToken(std::string(30, 'x')).size() == 10 &&
              isHex(trackToken(std::string(30, 'x'))),
          "trackToken: long id hashes to 10 hex");
    CHECK(trackToken("same") == trackToken("same"),
          "trackToken: deterministic");
    CHECK(trackToken("a") != trackToken("b"), "trackToken: distinct ids differ");
    CHECK(queryHash("hello").size() == 12 && isHex(queryHash("hello")),
          "queryHash: 12 hex chars");
    CHECK(queryHash("hello") == queryHash("hello"),
          "queryHash: deterministic");
    CHECK(queryHash("hello") != queryHash("world"),
          "queryHash: distinct queries differ");

    // --- callback data: exact strings + 64-byte limit -------------------
    CHECK(cbPlayer("pause", -1001234567890LL) == "pl:player:pause:-1001234567890",
          "cbPlayer: exact string");
    CHECK(cbPlayerToken("playnow", "a1b2c3d4e5") == "pl:player:playnow:a1b2c3d4e5",
          "cbPlayerToken: exact string");
    CHECK(cbQueueMore(-1001) == "pl:queue:more:-1001", "cbQueueMore: exact string");
    CHECK(cbQueueDel(-1001, "a1b2c3d4e5") == "pl:queue:del:-1001:a1b2c3d4e5",
          "cbQueueDel: exact string");
    CHECK(cbSet("adminmode") == "pl:set:adminmode", "cbSet: exact string");
    CHECK(cbSetInfo("adminmode") == "pl:set:info:adminmode",
          "cbSetInfo: exact string");
    CHECK(cbSetTimer(5) == "pl:set:timer:5", "cbSetTimer: exact string");
    CHECK(cbAuthDel(-1001, 42) == "pl:auth:del:-1001:42",
          "cbAuthDel: exact string");
    CHECK(cbSearchPick(3, 42, "a1b2c3d4e5") == "pl:search:3:42:a1b2c3d4e5",
          "cbSearchPick: exact string");
    CHECK(cbHelp("play") == "pl:help:play", "cbHelp: exact string");
    // Representative callbacks must all fit the 64-byte Bot API limit.
    std::vector<std::string> cbs = {
        cbPlayer("pause", -1001234567890LL),
        cbPlayer("seekback15", -1001234567890LL),
        cbPlayerToken("playnow", "a1b2c3d4e5"),
        cbPlayerToken("addlist", "a1b2c3d4e5"),
        cbQueueMore(-1001234567890LL),
        cbQueueDel(-1001234567890LL, "a1b2c3d4e5"),
        cbSet("cleanduration"),
        cbSetInfo("cleanduration"),
        cbSetTimer(7),
        cbAuthDel(-1001234567890LL, 123456789012LL),
        cbSearchPick(10, 123456789012LL, "a1b2c3d4e5"),
        cbHelp("download"),
        cbDlCancel("abcdef123456"),
        cbDlFx("abcdef123456", "nightcore"),
        cbVote("approve", "a1b2c3d4e5"),
        cbDel("yes", "/data/x.mp3"),
        cbLossy("accept", "abcdef123456"),
        cbRestart("bot", -1001234567890LL),
    };
    bool all_fit = true;
    for (const auto& c : cbs)
        if (c.size() > 64) all_fit = false;
    CHECK(all_fit, "callback_data: all representative callbacks fit 64 bytes");
    // The longest realistic one (search pick with a 12-digit user id).
    CHECK(cbSearchPick(10, 123456789012LL, "a1b2c3d4e5").size() <= 64,
          "callback_data: search pick fits 64 bytes");

    // --- player keyboard: merged 6-row layout ---------------------------
    {
        InlineKeyboard kb = playerKeyboard(-1001, false, true, 2, "tok123",
                                           true, 90, 200);
        CHECK(kb.size() == 6, "playerKeyboard: 6 rows with progress");
        CHECK(kb[0].size() == 1 && kb[0][0].callback_data == "pl:player:progress:-1001",
              "playerKeyboard: row 1 is the progress no-op button");
        CHECK(contains(kb[0][0].text, "1:30"),
              "playerKeyboard: progress label shows the position");
        CHECK(kb[1].size() == 4 && kb[1][0].text == "Pause" &&
                  kb[1][0].callback_data == "pl:player:pause:-1001",
              "playerKeyboard: row 2 pause button when playing");
        CHECK(kb[1][1].text == "Loop ×2",
              "playerKeyboard: loop button shows the remaining count");
        CHECK(kb[2].size() == 2 &&
                  kb[2][0].callback_data == "pl:player:seekback15:-1001" &&
                  kb[2][1].callback_data == "pl:player:seek15:-1001",
              "playerKeyboard: row 3 is the ±15s seek pair");
        CHECK(kb[3].size() == 1 &&
                  contains(kb[3][0].text, "Autoplay: Enabled"),
              "playerKeyboard: row 4 is the autoplay toggle");
        CHECK(kb[4].size() == 1 &&
                  contains(kb[4][0].text, "Thumbnails: Enabled") &&
                  kb[4][0].callback_data == "pl:player:thumb:-1001",
              "playerKeyboard: row 5 is the thumbnails toggle, on");
        CHECK(kb[5].size() == 2 &&
                  kb[5][1].callback_data == "pl:player:addlist:tok123",
              "playerKeyboard: row 6 has Close + Add to Playlist");
    }
    {
        InlineKeyboard kb = playerKeyboard(-1001, true, false, 0, "", false, 0, 0);
        CHECK(kb.size() == 5, "playerKeyboard: 5 rows without progress");
        CHECK(kb[0][0].text == "Resume" &&
                  kb[0][0].callback_data == "pl:player:resume:-1001",
              "playerKeyboard: paused state shows the resume button");
        CHECK(kb[3].size() == 1 &&
                  contains(kb[3][0].text, "Thumbnails: Enabled"),
              "playerKeyboard: thumbnails toggle on by default");
        CHECK(kb[4].size() == 1 && kb[4][0].text == "Close",
              "playerKeyboard: no Add to Playlist without a token");
    }
    {
        // Thumbnails toggle shows the current state, like the
        // autoplay row (Package 2, row A35).
        InlineKeyboard kb = playerKeyboard(-1001, false, true, 0, "", true,
                                           0, 0, false);
        CHECK(kb.size() == 6 &&
                  kb[4].size() == 1 &&
                  contains(kb[4][0].text, "Thumbnails: Disabled") &&
                  kb[4][0].callback_data == "pl:player:thumb:-1001",
              "playerKeyboard: thumbnails toggle shows Disabled when off");
        InlineKeyboard kb_on = playerKeyboard(-1001, false, true, 0, "",
                                              true, 0, 0, true);
        CHECK(kb_on[4].size() == 1 &&
                  contains(kb_on[4][0].text, "Thumbnails: Enabled"),
              "playerKeyboard: thumbnails toggle shows Enabled when on");
    }

    // --- menu panel keyboard ---------------------------------------------
    {
        InlineKeyboard kb = menuPanelKeyboard(-1001);
        CHECK(kb.size() == 3, "menuPanelKeyboard: 3 rows");
        CHECK(kb[0].size() == 4 && kb[1].size() == 4 && kb[2].size() == 1,
              "menuPanelKeyboard: 4 + 4 + 1 buttons");
        CHECK(kb[0][2].callback_data == "pl:player:pause:-1001",
              "menuPanelKeyboard: pause button");
        CHECK(kb[1][2].callback_data == "pl:player:voldec:-1001" &&
                  kb[1][3].callback_data == "pl:player:volinc:-1001",
              "menuPanelKeyboard: volume buttons");
        CHECK(kb[2][0].callback_data == "pl:player:refresh:-1001",
              "menuPanelKeyboard: refresh button");
    }
    {
        std::string t = menuPanelText("Song A", "https://x/a", "Song B",
                                      "https://x/b");
        CHECK(contains(t, "Song A") && contains(t, "Song B"),
              "menuPanelText: both titles present");
        CHECK(contains(t, "Now:") && contains(t, "Next:"),
              "menuPanelText: Now/Next labels");
        std::string empty = menuPanelText("", "", "", "");
        CHECK(contains(empty, "Nothing streaming now") &&
                  !contains(empty, "href=\"\""),
              "menuPanelText: empty URLs render plain bold, never empty href");
    }

    // --- search keyboard: 5 per row --------------------------------------
    {
        std::vector<std::pair<int, std::string>> picks;
        for (int i = 1; i <= 10; ++i)
            picks.emplace_back(i, cbSearchPick(i, 42, "tok"));
        InlineKeyboard kb = searchKeyboard(picks);
        CHECK(kb.size() == 2 && kb[0].size() == 5 && kb[1].size() == 5,
              "searchKeyboard: 10 picks -> two rows of 5");
        CHECK(kb[0][0].text == "1" && kb[1][4].text == "10",
              "searchKeyboard: numbers in order");
        // Prev/next only — no page-indicator button (total_pages unused).
        InlineKeyboard pg = searchPageKeyboard(42, 2, queryHash("q"), true,
                                               true, 3);
        CHECK(pg.size() == 1 && pg[0].size() == 2,
              "searchPageKeyboard: prev + next in one row");
        CHECK(contains(pg[0][0].text, "Previous") &&
                  contains(pg[0][1].text, "Next"),
              "searchPageKeyboard: prev/next labels");
        CHECK(pg[0][0].callback_data == "pl:search:page:42:1:" + queryHash("q") &&
                  pg[0][1].callback_data == "pl:search:page:42:3:" + queryHash("q"),
              "searchPageKeyboard: page callbacks carry the query hash");
    }

    // --- settings keyboard -----------------------------------------------
    {
        ChatSettings s;
        s.admin_mode = 0;
        s.play_mode = "everyone";
        s.cmd_delete = false;
        s.clean_delay_min = 15;
        s.thumbnails = true;
        s.autoplay = false;
        s.language = "en";
        s.auto_delete = false;
        s.auto_delete_min = 5;
        s.silent = false;
        InlineKeyboard kb = settingsKeyboard(s);
        CHECK(kb.size() == 14,
              "settingsKeyboard: 12 rows + timer row + index row = 14");
        CHECK(kb[0][1].callback_data == "pl:set:adminmode",
              "settingsKeyboard: admin mode toggle");
        CHECK(kb[0][1].text == "Auth Users",
              "settingsKeyboard: admin mode 0 shows Auth Users");
        CHECK(kb[10].size() == 6, "settingsKeyboard: timer row has 2-7");
        CHECK(kb[10][3].text == "5 (current)",
              "settingsKeyboard: current timer value is marked");
        CHECK(kb[12].size() == 3 &&
                  kb[12][0].callback_data == "pl:set:page" &&
                  kb[12][1].callback_data == "pl:set:musicdl" &&
                  kb[12][2].callback_data == "pl:set:quality",
              "settingsKeyboard: index row links page/musicdl/quality screens");
        CHECK(kb[13][0].callback_data == "pl:close",
              "settingsKeyboard: last row closes");
    }

    // --- help menu --------------------------------------------------------
    {
        InlineKeyboard kb = helpMenuKeyboard();
        CHECK(!kb.empty(), "helpMenuKeyboard: non-empty");
        // No close button: the last row goes back to /start.
        bool has_back = false;
        for (const auto& row : kb)
            for (const auto& b : row)
                if (b.callback_data == kCbStart) has_back = true;
        CHECK(has_back, "helpMenuKeyboard: last row goes back to start");
        CHECK(contains(kHelpMenu, "Commands") || contains(kHelpMenu, "Help"),
              "kHelpMenu: mentions commands/help");
        CHECK(contains(perCommandHelpText("play"), "/play"),
              "perCommandHelpText: play entry mentions /play");
        CHECK(contains(perCommandHelpText("nosuchcmd"), "Unknown") ||
                  perCommandHelpText("nosuchcmd").empty() ||
                  contains(perCommandHelpText("nosuchcmd"), "not"),
              "perCommandHelpText: unknown command handled");
    }

    // --- now-playing card text -------------------------------------------
    {
        CardTrack t;
        t.title = "Test Song";
        t.url = "https://x/test";
        t.duration = "3:20";
        t.requester = "Bob";
        t.token = "tok123";
        std::string card = nowPlayingText(t);
        CHECK(contains(card, "Test Song"), "nowPlayingText: title present");
        CHECK(contains(card, "3:20"), "nowPlayingText: duration present");
        CHECK(contains(card, "Bob"), "nowPlayingText: requester present");
        CHECK(contains(card, "https://x/test"),
              "nowPlayingText: title links to the track URL");
        std::string state = stateCardText("Paused", t, "Bob");
        CHECK(contains(state, "Paused") && contains(state, "Test Song"),
              "stateCardText: state line + track");
        CHECK(contains(progressLabel(90, 200), "◉"),
              "progressLabel: marker present on the card");
    }

    // --- start / settings templates --------------------------------------
    CHECK(contains(kStartPm, "Parlay") || contains(kStartPm, "music"),
          "kStartPm: mentions the bot");
    CHECK(contains(fmt(kSettingsText, {{"title", "G"}, {"id", "-1001"}}),
                  "Group:"),
          "kSettingsText: title + id placeholders fill");
    CHECK(contains(settingInfoText("adminmode"), "Admin Mode"),
          "settingInfoText: adminmode explainer");
    CHECK(settingInfoText("nosuchkey").empty(),
          "settingInfoText: unknown key returns empty");

    // --- Phase 9 wiring round 2: templates now used by the dispatcher ----
    CHECK(contains(helpTopicText("search"), "search for tracks"),
          "helpTopicText: search topic wired");
    CHECK(helpTopicTitle("search") == "Search Commands",
          "helpTopicTitle: search topic title");
    CHECK(contains(kSearchExpired, "expired"),
          "kSearchExpired: expiry wording");
    CHECK(contains(kQueueHelpOps, "remove") &&
              contains(kQueueHelpOps, "shuffle"),
          "kQueueHelpOps: queue ops listed");
    CHECK(contains(kDenyAdmin, "Access Denied") &&
              contains(kDenyAdmin, "chat admins"),
          "kDenyAdmin: admin denial card");
    CHECK(contains(kDenyAdminOrAuth, "authorized users"),
          "kDenyAdminOrAuth: admin-or-auth denial card");
    CHECK(contains(kGuardPlayAdmin, "Admin only play"),
          "kGuardPlayAdmin: admin-only play denial");
    CHECK(contains(kCtlPaused, "Paused") &&
              contains(kCtlResumed, "Resumed") &&
              contains(kCtlSkipped, "Skipped"),
          "kCtlPaused/Resumed/Skipped: control state lines");
    CHECK(contains(fmt(kToastSkippedBy, {{"user", "Ann"}}), "Ann"),
          "kToastSkippedBy: user placeholder fills");
    CHECK(contains(fmt(kToastReplayedBy, {{"user", "Ann"}}), "Ann"),
          "kToastReplayedBy: user placeholder fills");
    CHECK(contains(kQueueAlreadyEmpty, "already empty"),
          "kQueueAlreadyEmpty: clear-on-empty wording");
    CHECK(contains(kSpeedtestFailed, "speed test failed"),
          "kSpeedtestFailed: probe failure wording");
    CHECK(contains(kDelTopLevel, "top-level"),
          "kDelTopLevel: top-level guard wording");
    CHECK(contains(kDelInvalidPath, "Invalid path"),
          "kDelInvalidPath: path guard wording");
    CHECK(std::string(kShareCancel) == "Cancel sending music",
          "kShareCancel: share prompt cancel label");
    CHECK(contains(fmt(kMyPlaylistsHeader,
                       {{"name", "Road"}, {"id", "abc123"}}),
                  "Road (<code>abc123</code>)"),
          "kMyPlaylistsHeader: name + id line");
    CHECK(contains(kBcastCancelOk, "cancelled"),
          "kBcastCancelOk: cancel confirmation");
    CHECK(contains(kCplayDisabled, "Channel Play disabled"),
          "kCplayDisabled: disable confirmation");
    CHECK(contains(cplayEnabledText(-10042), "-10042"),
          "cplayEnabledText: channel id fills");
    CHECK(contains(cplayStatusText(-10042), "-10042"),
          "cplayStatusText: channel id fills");
    CHECK(contains(kCplayNotLinked, "not linked"),
          "kCplayNotLinked: unlinked-channel wording");
    CHECK(contains(kCplayNotAccessible, "Not Accessible"),
          "kCplayNotAccessible: channel access wording");
    CHECK(contains(kCplayOwnerRequired, "owner"),
          "kCplayOwnerRequired: linker ownership wording");
    CHECK(contains(fmt(kLoggerUsage, {{"status", "ON"}}), "ON"),
          "kLoggerUsage: status placeholder fills");
    CHECK(contains(kLoggerNeedId, "LOGGER_ID"),
          "kLoggerNeedId: logger id requirement");
    CHECK(contains(reloadCooldownText("9:30"), "9:30"),
          "reloadCooldownText: mmss fills");
    CHECK(contains(kActiveStreamsEmpty, "active streams"),
          "kActiveStreamsEmpty: no-streams wording");
    CHECK(contains(activeChatsText(3), "3"),
          "activeChatsText: count fills");
    CHECK(contains(fmt(kBugUsage, {{"cmd", "/bug"}}), "/bug"),
          "kBugUsage: command placeholder fills");
    CHECK(contains(fmt(kBugReport,
                       {{"from", "Ann"}, {"id", "7"}, {"where", "-100"},
                        {"text", "boom"}}),
                  "boom"),
          "kBugReport: placeholders fill");
    CHECK(contains(kBugThanks, "Thank you"),
          "kBugThanks: thanks wording");
    CHECK(contains(kEvalNoInput, "What?"), "kEvalNoInput: empty eval");
    CHECK(contains(evalOutputText("hi"), "hi"),
          "evalOutputText: output fills");
    CHECK(contains(kLogsNone, "No logs"), "kLogsNone: empty logs");
    CHECK(contains(kLogsMissing, "doesn't exist"),
          "kLogsMissing: missing log file");
    CHECK(contains(kSettingsPageScreen, "Auto delete old messages"),
          "kSettingsPageScreen: pagination screen");
    CHECK(contains(kSettingsMusicDlScreen, "Download music via chat"),
          "kSettingsMusicDlScreen: music-download screen");
    {
        InlineKeyboard idx = settingsIndexKeyboard();
        CHECK(idx.size() == 1 && idx[0].size() == 3 &&
                  idx[0][0].callback_data == "pl:set:page" &&
                  idx[0][1].callback_data == "pl:set:musicdl" &&
                  idx[0][2].callback_data == "pl:set:quality",
              "settingsIndexKeyboard: page + musicdl + quality buttons");
        InlineKeyboard tog = settingsToggleKeyboard("page", true);
        CHECK(tog[0][0].callback_data == "pl:set:page:0" &&
                  tog[0][1].callback_data == "pl:set:main",
              "settingsToggleKeyboard: enabled page offers :0 + back");
    }

    // --- Opus quality card + keyboard (quality.get surface) -----------
    {
        QualityInfo q;
        q.effective.bitrate_bps = 256000;
        q.effective.frame_ms = 40;
        q.effective.complexity = 7;
        q.effective.vbr = false;
        q.effective.fec = true;
        q.effective.dtx = false;
        q.library = q.effective;
        q.sources = {{"bitrate_bps", "store"},
                     {"frame_ms", "config"},
                     {"complexity", "profile"},
                     {"vbr", "default"},
                     {"fec", "store"},
                     {"dtx", "default"}};
        std::string card = qualityCardText(q);
        CHECK(contains(card, "Audio Quality") &&
                  contains(card, "256 kbps (chat setting)") &&
                  contains(card, "40 ms (config file)") &&
                  contains(card, "7 (voice profile)") &&
                  contains(card, "VBR:</b> off (default)") &&
                  contains(card, "FEC:</b> on (chat setting)") &&
                  contains(card, "next join"),
              "qualityCardText: effective values + source per field + "
              "next-join note");
        CHECK(!contains(card, "library currently holds"),
              "qualityCardText: no library note when library == effective");
        q.library.bitrate_bps = 192000;
        q.library.frame_ms = 20;
        std::string card2 = qualityCardText(q);
        CHECK(contains(card2, "library currently holds 192 kbps / 20 ms"),
              "qualityCardText: library note when library differs");
        q.active_call = true;
        CHECK(contains(qualityCardText(q), "live in this chat"),
              "qualityCardText: active-call note");
        InlineKeyboard kb = settingsQualityKeyboard(q);
        CHECK(kb.size() == 4 && kb[0].size() == 5 && kb[1].size() == 4,
              "settingsQualityKeyboard: bitrate row (5) + frame row (4) "
              "+ reset + back");
        CHECK(kb[0][3].callback_data == "pl:set:quality:br:256" &&
                  kb[0][3].text == "256 kbps (current)" &&
                  kb[0][0].text == "128 kbps",
              "settingsQualityKeyboard: bitrate buttons, current marked");
        CHECK(kb[1][2].callback_data == "pl:set:quality:fr:40" &&
                  kb[1][2].text == "40 ms (current)",
              "settingsQualityKeyboard: frame buttons, current marked");
        CHECK(kb[2][0].callback_data == "pl:set:quality:reset" &&
                  kb[3][0].callback_data == "pl:set:main",
              "settingsQualityKeyboard: reset + back to settings");
        CHECK(cbSetQualityBr(320).size() <= 64 &&
                  cbSetQualityFr(60) == "pl:set:quality:fr:60",
              "quality callbacks: builder shapes within 64 bytes");
        {
            InlineKeyboard back = qualityBackKeyboard();
            CHECK(back.size() == 1 &&
                      back[0][0].callback_data == "pl:set:main",
                  "qualityBackKeyboard: load-failure screen offers Back");
        }
    }
    CHECK(contains(kRtmpDmOnly, "DM"), "kRtmpDmOnly: DM-only refusal");
    CHECK(contains(kRtmpNotSet, "No RTMP target"),
          "kRtmpNotSet: unset target wording");
    CHECK(contains(kRtmpSaved, "RTMP target saved"),
          "kRtmpSaved: save confirmation");
    CHECK(contains(kRtmpRemoved, "removed"), "kRtmpRemoved: removal");
    CHECK(contains(kRtmpInvalidUrl, "rtmp://"),
          "kRtmpInvalidUrl: URL scheme wording");
    CHECK(contains(kRtmpAlreadyRunning, "already running"),
          "kRtmpAlreadyRunning: double-stream guard");
    CHECK(contains(kRtmpNone, "No stream is running"),
          "kRtmpNone: no-stream wording");
    CHECK(contains(kRtmpNoneActive, "No active RTMP stream"),
          "kRtmpNoneActive: no-active-stream wording");
    CHECK(contains(kRtmpStartFailed, "Could not start"),
          "kRtmpStartFailed: ffmpeg failure wording");
    CHECK(contains(rtmpStreamStartedText("Song", "Ann"), "Song"),
          "rtmpStreamStartedText: what + by fill");
    CHECK(contains(rtmpStoppedText("Ann"), "Ann"),
          "rtmpStoppedText: by fills");
    CHECK(contains(rtmpStatusText("Song", "1:05"), "1:05"),
          "rtmpStatusText: duration fills");
    CHECK(contains(userbotAlreadyText("The assistant"), "already in"),
          "userbotAlreadyText: already-here wording");
    CHECK(contains(assistantRestrictedText("The assistant", 42), "42"),
          "assistantRestrictedText: assistant id fills");
    CHECK(std::string(kCbLangEn) == "pl:lang:en",
          "kCbLangEn: lang callback base");

    // §13 inline-mode results (Phase 9 gaps): the core renders these
    // from the search.inline Tracks.
    CHECK(std::string(kInlineHintText) ==
              "np  s  l  lib  del — or type to search",
          "kInlineHintText: mode hint wording");
    CHECK(std::string(kInlineHelpTitle1) ==
              "1. Paste a music share URL or enter a Music ID",
          "kInlineHelpTitle1: first help article");
    CHECK(std::string(kInlineHelpTitle2) ==
              "2. Enter search + keyword to search songs",
          "kInlineHelpTitle2: second help article");
    CHECK(std::string(kInlineHelpMessage) == "Parlay v0.3.0",
          "kInlineHelpMessage: help articles send the version line");
    CHECK(std::string(kInlineNeedKeyword) == "Please enter a keyword",
          "kInlineNeedKeyword: bare search article");
    CHECK(std::string(kInlineNoResults) == "No results found",
          "kInlineNoResults: empty search article");
    CHECK(std::string(kInlineTypeHelp) == "Type help for help",
          "kInlineTypeHelp: empty query article");
    CHECK(inlineLyricsTitle("Test Song") == "Lyrics: Test Song",
          "inlineLyricsTitle: lyrics article title");
    CHECK(std::string(kInlineOpenInParlay) == "Open in Parlay Bot",
          "kInlineOpenInParlay: search-result button label");
    CHECK(inlineOpenUrl("parlaybot", "yt:abc") ==
              "https://t.me/parlaybot?start=spotify----yt:abc",
          "inlineOpenUrl: deep link carries the track id");
    CHECK(contains(kDlDeepLinkExpired, "expired"),
          "kDlDeepLinkExpired: unresolvable deep link wording");
    CHECK(!contains(kRadioManageCard, "Listener"),
          "kRadioManageCard: no listener rows (ruling 9)");

    // --- Package 3: queue blockquote, search page footer, copy
    // button + collection / retry keyboards -------------------------
    {
        CardTrack now{"Now Song", "https://x/now", "3:00", "Alice", "tok0"};
        std::vector<CardTrack> up = {
            {"One", "https://x/1", "2:00", "Bob", "tok1"},
            {"Two", "https://x/2", "4:00", "Carol", "tok2"},
        };
        std::string q = queueText(now, up, 2);
        CHECK(contains(q, "<blockquote expandable>") &&
                  contains(q, "</blockquote>"),
              "queueText: waiting list wrapped in expandable blockquote");
        CHECK(q.find("<blockquote expandable>") <
                  q.find("1. "),
              "queueText: blockquote opens before the waiting list");
        CHECK(contains(q, "Now Playing"),
              "queueText: now-playing header stays outside the blockquote");
        std::string empty = queueText(now, {}, 0);
        CHECK(!contains(empty, "<blockquote"),
              "queueText: empty queue carries no blockquote");
        CHECK(contains(empty, "No more songs in queue."),
              "queueText: empty queue keeps the empty line");
    }
    CHECK(searchPageFooterText(1, 1).empty(),
          "searchPageFooterText: single page carries no footer");
    CHECK(searchPageFooterText(1, 3) == "<i>Page 1 of 3</i>",
          "searchPageFooterText: page 1 of 3");
    CHECK(searchPageFooterText(2, 3) == "<i>Page 2 of 3</i>",
          "searchPageFooterText: page 2 of 3");
    CHECK(searchPageFooterText(3, 3) == "<i>Page 3 of 3</i>",
          "searchPageFooterText: last page footer");
    {
        InlineKeyboard kb = inlineSearchResultKeyboard(
            "https://www.youtube.com/watch?v=abc", "parlaybot", "yt:abc");
        CHECK(kb.size() == 2, "inlineSearchResultKeyboard: two rows");
        CHECK(kb[0].size() == 1 && kb[0][0].text == "Copy link" &&
                  kb[0][0].copy_text ==
                      "https://www.youtube.com/watch?v=abc" &&
                  kb[0][0].callback_data.empty() &&
                  kb[0][0].url.empty(),
              "inlineSearchResultKeyboard: copy button carries the link "
              "and no callback_data");
        CHECK(kb[1].size() == 1 &&
                  kb[1][0].text == "Open in Parlay Bot" &&
                  kb[1][0].url ==
                      "https://t.me/parlaybot?start=spotify----yt:abc",
              "inlineSearchResultKeyboard: deep-link button opens the "
              "bot with the track");
        InlineKeyboard plain = inlineSearchResultKeyboard("", "", "");
        CHECK(plain.empty(),
              "inlineSearchResultKeyboard: no link and no bot -> no "
              "buttons");
    }
    {
        parlay::InlineButton copy =
            parlay::InlineButton::copy("Copy link", "https://x/1");
        CHECK(copy.copy_text == "https://x/1" &&
                  copy.callback_data.empty() && copy.url.empty(),
              "InlineButton::copy: exactly one payload kind set");
    }
    CHECK(collectionCardText("playlist", "Chill Mix", 12) ==
              "<b>Chill Mix</b>\n\nplaylist • 12 songs",
          "collectionCardText: name, kind and count");
    CHECK(dlCollectionButtonText(12) == "Download 12 songs",
          "dlCollectionButtonText: count in the label");
    {
        InlineKeyboard kb = dlCollectionCardKeyboard("run123456", 12);
        CHECK(kb.size() == 1 && kb[0].size() == 1 &&
                  kb[0][0].text == "Download 12 songs" &&
                  kb[0][0].callback_data == "pl:dl:go:run123456",
              "dlCollectionCardKeyboard: single Download N songs button");
        CHECK(kb[0][0].callback_data.size() <= 64,
              "dlCollectionCardKeyboard: callback within 64 bytes");
        CHECK(cbDlGo("run123456") == "pl:dl:go:run123456",
              "cbDlGo: exact string");
        CHECK(cbDlRetry("run123456") == "pl:dl:retry:run123456",
              "cbDlRetry: exact string");
        CHECK(cbDlRetry("run123456").size() <= 64,
              "cbDlRetry: callback within 64 bytes");
    }
    {
        InlineKeyboard kb = dlRetryKeyboard("run123456");
        CHECK(kb.size() == 1 && kb[0].size() == 1 &&
                  kb[0][0].text == "Check Again" &&
                  kb[0][0].callback_data == "pl:dl:retry:run123456",
              "dlRetryKeyboard: single Check Again button");
    }
    CHECK(std::string(kDlNotAvailable).find("not available") !=
              std::string::npos,
          "kDlNotAvailable: names the not-available state");
    CHECK(std::string(kToastDlStillProcessing) ==
              "Track is still being processed. Please try again later",
          "kToastDlStillProcessing: exact still-processing toast");

    if (failures == 0) {
        std::cout << "ui_test: ALL PASS\n";
        return 0;
    }
    std::cout << "ui_test: " << failures << " FAILURE(S)\n";
    return 1;
}
