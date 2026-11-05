# Phase 10 - Telegram UI Inventory, Group B (7 refs)

Scope: TgMusicBot, TinaMusic, YukkiMusic-Go, anonx-cpp, beetube-bot,
feel-me, lemonic-telegram-bot. Every row was checked against the actual
ref tree under `~/workspace/parlay/refs/<name>/` (Phase 9 inventories were
used only as a starting map; line numbers below were re-verified by grep
in this audit unless noted as function-level). Parlay verdicts were checked
by grep against the `feat/ui-complete-all-refs` tree, mainly
`services/core-cpp/src/dispatcher.cpp`, `services/core-cpp/src/ui_texts.hpp`,
`services/backend-go/internal/gui/gui.go`,
`services/backend-go/internal/handlers/`,
`services/backend-go/internal/settings/`,
`services/backend-go/internal/room/`,
`services/backend-go/internal/voice/`, `contracts/routing-table.md`, and
`contracts/ipc.json`. Verdicts: PRESENT (equivalent exists in Parlay),
MISSING (no equivalent found), UNCERTAIN (partial equivalent, or the
Parlay side could not be confirmed by grep alone).

Conventions used in this file, to keep it free of pictographs: button
labels that are pure glyphs in the ref are described in words (for
example "pause glyph", "skip glyph"); the words in quotes are the ref's
text labels, not its glyphs. "NA" rows name a real absence with a reason;
they are not placeholders.

Parlay baseline found during this audit (so verdicts below read
correctly): Parlay's core dispatcher already implements the Phase 9
callback namespace (`pl:player:`, `pl:queue:`, `pl:set:`, `pl:dl:`,
`pl:bcast:`, `pl:help:`, `pl:close`), a player keyboard with progress,
pause/resume, replay, skip, stop, plus/minus-15s seek, autoplay and
Close/Add-to-Playlist rows (`ui_texts.hpp` `playerKeyboard`), a queue
card with per-track delete and Show-more (`queueKeyboard`, page size 15),
a settings panel with admin/play/cmd-delete/clean/clean-delay/thumbnail/
autoplay/language/auto-delete/timer/silent rows plus a settings-index row
(Pagination, Music Download, Quality), a control-panel keyboard
(`/menu`), a persistent reply keyboard (`persistentKeyboard`), search
number buttons 5 per row plus Previous/Next page buttons, a share flow
with a My-Link button, and an explicit unknown-command reply (never
silent). Where a ref feature differs only in wording or layout, the
verdict is PRESENT with the difference noted.

---

# 1. TgMusicBot - `refs/TgMusicBot/` (Go, gotdbot, TDLib rich messages)

No locale file; strings are inline in `internal/bot/*.go`. Keyboards in
`internal/utils/buttons.go`. No sudo list anywhere in this ref: the
privileged tier is a developer list (`config.DEVS`), not sudoers.

## 1.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Developer gate (`isDev`) | Only users in `config.DEVS` may run developer commands; others are refused | `internal/bot/extras.go:59-61` (gate); used in `internal/bot/devs.go:25,85,100,120`, `internal/bot/broadcast.go:53,68`, `internal/bot/stats.go:173`, `internal/bot/shell.go:128` | MISSING - Parlay has owner and sudo permission tiers (`dispatcher.cpp` routes with `Perm::Owner` / `Perm::Sudo`, lines 459-498) but no separate DEVS list |
| `/av` / `/active_vc` active-chats screen (dev-only) | Rich table of active voice/video chats: index, chat id, queue length, now-playing track link; empty state "No Active Chats" blockquote | `internal/bot/devs.go:24-82`; registered in `internal/bot/load.go:41-42` | PRESENT - Parlay `/ac` / active-VC list in `dispatcher.cpp` (`onActiveVc`, `kActiveStreamsHeader`); Parlay gate is sudo, not DEVS |
| `/stats` (dev-only) | System/runtime/database status card, edited in place from a "Collecting system statistics..." message | `internal/bot/stats.go:173` (gate); handler in same file | PRESENT - Parlay has a stats card (`ui_texts.hpp` stats section) and a `/status` command; gate differs (Parlay sudo/owner, not DEVS) |
| `/clearass` and `/leaveall` (dev-only) | Disconnect and clear all assistant clients; disconnect assistants from every chat | `internal/bot/devs.go:84,99`; registered in `internal/bot/load.go:43-45` | UNCERTAIN - Parlay has assistant/voice management in `services/backend-go/internal/voice/`, but no `/clearass` or `/leaveall` command was found by grep in the dispatcher routes |
| `/logger` (dev-only) | Enable/disable logger channel; requires LOGGER_ID set first | `internal/bot/devs.go:119`; usage/status texts in same handler | PRESENT - Parlay `/logger` route exists (`dispatcher.cpp` line 498, `Perm::Sudo`) |
| `/broadcast` and `/stop_broadcast` (dev-only) | Fire-and-forget broadcast; "Broadcast started", "No broadcast in progress", "Broadcast stopped", "A broadcast is already in progress", "No targets found"; no progress card, no cancel button | `internal/bot/broadcast.go:52-67` (gates), handler at `:67`; cancel at `:52` | PRESENT - Parlay `/broadcast` (sudo) with a `pl:bcast:cancel` owner-only cancel callback (`ui_texts.hpp:223`, `dispatcher.cpp:7953`); Parlay adds the cancel button TgMusicBot lacks |
| Admin denial messages | "You must be an administrator to use this command" / "...to use this action" and "You are not authorized to use this command/action" | `internal/bot/filters.go:79-82`; `internal/bot/settings.go` access errors via `cache.SettingsAccess` | PRESENT - Parlay has admin-gated settings callbacks answering a not-allowed toast (`dispatcher.cpp:7277-7280` area, `kToastNotAllowed`) and silent permission gates for commands (`dispatcher.cpp:955`) |
| Sudo list / add-sudo / remove-sudo | No such feature: grep for addsudo/sudolist across `internal/` returns nothing | Absent from `internal/bot/*.go` and `internal/bot/load.go` | NA - no equivalent to port; Parlay already has `/addsudo`, `/delsudo`, `/sudolist` (`dispatcher.cpp:470-472`, `5710-5756`) from the Tina/Yukki lineage |

## 1.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Started-streaming card | Text message (edited in place over the progress message): "Started streaming" header, title link, duration in mm:ss, requested-by; no photo | `internal/bot/play.go:465-477` | PRESENT - Parlay now-playing card (`dispatcher.cpp:1678-1699`, `playerKeyboard`); Parlay adds a photo when a thumbnail exists, which TgMusicBot never does |
| Queued-single card with Play Now | "Added to queue: N" with title, duration, requested-by; keyboard row: Play Now + Close | `internal/bot/play.go:444-458`; keyboard `internal/utils/buttons.go:221-225` (`QueueMarkup`) | PRESENT - Parlay queued card keyboard (`ui_texts.hpp:1002`, `queuedKeyboard`, Play Now token callback) |
| Queued-multiple card | Blockquote list of position/title/duration, queue total, total duration, requested-by, skipped-tracks note; degrades to summary over 4096 chars | `internal/bot/play.go:388-421` | PRESENT - Parlay multiple-added header plus queue-limit and view hints (`ui_texts.hpp` queued/multiple texts, Phase 9 section 4 pattern) |
| Player control callbacks and toasts | Skip/stop/pause/resume/mute/unmute callbacks with toasts "Track skipped", "Playback stopped/paused/resumed/muted/unmuted", failures "Unable to ... playback", "There is no active playback" (also replaces card text) | `internal/bot/callbacks.go:23` (`playCallbackHandler`); state card builder at `:53` (`buildTrackMessage`); buttons `internal/utils/buttons.go:143-201` (`ControlButtons`) | PRESENT - Parlay `pl:player:` callbacks (`dispatcher.cpp:7001-7210`) with matching toasts (`ui_texts.hpp:1002-1031`); Parlay adds replay, seek-15s, autoplay and progress rows TgMusicBot lacks |
| Add-to-playlist button | Plus button on the playing card; creates playlist "My Playlist (TgMusic)" if needed; toasts for added / not found / create failure | `internal/utils/buttons.go:143-201` (plus button in play mode); handler in `internal/bot/callbacks.go` | PRESENT - Parlay "Add to Playlist" button on the player card row 5 (`ui_texts.hpp` `playerKeyboard`) |
| Silent `s`-prefixed commands | `/splay`, `/sskip`, `/sstop`, `/spause`, `/sresume` (and other `s` variants) hide the final success message; progress and errors stay visible | `internal/bot/load.go:23-25` (registers `s` + name for each command); help table `internal/bot/help.go` user section | PRESENT - Parlay silent mode setting (`pl:set:silent`, `dispatcher.cpp:7732-7733`) and silent handling in settings; the `s`-prefix alias form itself was not found in Parlay routes, so the alias spelling is MISSING while the behaviour is present - verdict recorded as PRESENT for the behaviour, see note |
| `/mix` mix-playlist flow | Builds a mix of related tracks from a query or the current song; usage, "Fetching mix tracks...", failure and already-queued texts | `internal/bot/mix.go` (handler); help table `internal/bot/help.go` | UNCERTAIN - Parlay has autoplay recommendations and playlist play, but no `/mix` command was found in dispatcher routes |
| Play from reply / media / link | `/play` on replied media, profile songs (up to 10 from own profile when idle), URLs and queries; status edits "Searching and downloading...", "Searching playlist...", "Loading profile songs..."; errors for invalid URL, no results, too long, file too large | `internal/bot/play.go` (flow); usage text in same file | PRESENT - Parlay `/play` on replied audio/video/voice/document (`dispatcher.cpp:2718-2736`, `2910-2929`) and query/link play |
| `/queue` display | Plain HTML: queue title, now-playing block (title capped at 45 chars, by, duration, loop on/off, progress), next-up first 14, "...and N more", total; compact fallback over 4096 chars; empty "The queue is empty", not-streaming text | `internal/bot/queue.go:24` (`queueHandler`) | PRESENT - Parlay queue card supersedes this (`dispatcher.cpp:1748-1770` `renderQueueCard`, `buildQueueCard`); Parlay adds per-track delete and Show-more |
| Queue ops (`/remove`, `/loop`) | `/remove` with single/range/list syntax and usage examples; `/loop` 0-10 usage and range errors; queue-full "Queue is full (max 10 tracks). Use /end to clear" | `internal/bot/remove.go`; `internal/bot/loop.go`; queue-full text in `internal/bot/play.go` | PRESENT - Parlay `/remove`, `/move`, `/clear`, `/restore`, `/loop` (`dispatcher.cpp` routes lines 326-352) with matching usage texts |
| `/seek` | Usage "Usage: /seek duration, Example: /seek 15"; minimum 10 seconds; cannot-seek-beyond-duration guard with max mm:ss | `internal/bot/seek.go` | PRESENT - Parlay `/seek` / `/seekback` with the same 10-second minimum (`dispatcher.cpp:3166-3183`) |
| `/position` / now state | No dedicated `/now` command in this ref; position is shown inside `/queue` (progress line) | Absent from `internal/bot/load.go` command registrations | NA for TgMusicBot itself; Parlay has `/now` and `/pos` from other refs |
| Play history | No play-history view or command | Absent from `internal/bot/*.go` | NA - no equivalent to port; Parlay also has no play-history command (grep found no history route) |

## 1.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Add-Me keyboard (start card) | Row 1 Add-Me URL button (startgroup), row 2 Help callback, row 3 Updates + Group URL buttons, row 4 Source Code URL button | `internal/utils/buttons.go:203-219` (`AddMeMarkup`) | PRESENT - Parlay start keyboard (`dispatcher.cpp:2542-2660` `onStart`, `onHelp`); Parlay adds a Language button and drops the hardcoded repo URL |
| Help menu keyboard | Row 1 Users / Admins / Owner, row 2 Playlist / Devs / Autoplay, row 3 Home + Close; category pages use Help + Home row, then Close + Source Code row | `internal/utils/buttons.go:124-141` (`HelpMenuKeyboard`, `BackHelpMenuKeyboard`); toasts in `internal/bot/help.go:158` | PRESENT - Parlay help menu with topic buttons and Back (`dispatcher.cpp:2660-2692`); Parlay topic set is the merged Parvati/Yukki set, not TgMusicBot's 6 |
| Player control keyboards (per mode) | Play mode: skip glyph, stop glyph, pause glyph on row 1; plus button + Close on row 2. Pause mode swaps in resume glyph; mute mode swaps in unmute glyph; unmute mode swaps in mute glyph; idle: Close only | `internal/utils/buttons.go:143-201` (`ControlButtons`) | PRESENT - Parlay `playerKeyboard` (pause/resume toggle, replay, skip, stop, seek row, autoplay, Close/Add-to-Playlist); Parlay mute/unmute live on the `/menu` panel, not the player card |
| Queue keyboard (Play Now / Close) | Single row on queued cards; Play Now callback is `play_now_<id>`, or a hash form when over 64 bytes | `internal/utils/buttons.go:221-225`; hash rule in `internal/bot/play.go` / callbacks | PRESENT - Parlay `queuedKeyboard` with token form (`ui_texts.hpp:1002`); Parlay token rule replaces the hash form |
| Settings keyboard | 6 rows of label + value: Play Mode, Command Delete, Admin Mode, Autoplay, Language, Close; language tap answers "Language selection is not yet implemented via this menu" | `internal/utils/buttons.go:71-122` (`SettingsKeyboard`); handler `internal/bot/settings.go:24,71` | PRESENT - Parlay `settingsKeyboard` (`ui_texts.hpp:1491`) covers all six rows and implements language selection (English-only picker) |
| Clean/auto-delete settings rows | Prepended rows: Auto-delete on/off toggle, "Delete after: N minutes" no-op label, timer buttons 2-7 (current prefixed with a check mark), Silent mode on/off | `internal/bot/settings.go:213` (`cleanSettingsKeyboard`); toasts in `settingsCallbackHandler` at `:71` | PRESENT - Parlay settings panel has auto-delete toggle, timer buttons 2-7 and silent toggle (`ui_texts.hpp:1491`, `dispatcher.cpp:7732`) |
| Support keyboards | Updates + Group URL buttons, with or without a Close row | `internal/utils/buttons.go:54-69` (`SupportKeyboard`, `SupportBtn`) | PRESENT - Parlay start/help cards carry Updates/Support URL buttons from config |
| Close callback (`vcplay_close`) | Deletes/closes the panel; toast "Closing panel" | `internal/bot/callbacks.go:200` (`vcPlayHandler`); registered `internal/bot/load.go:89` | PRESENT - Parlay `pl:close` (`ui_texts.hpp`, callback router `dispatcher.cpp:6584-6600`) |
| Help/autoplay callbacks | `help_all`, `help_back`, `help_user/admin/owner/devs/playlist/autoplay` with toasts "Opening help menu...", "Returning to main menu...", category title, "Unknown help category" alert | `internal/bot/help.go:158`; registrations `internal/bot/load.go:87` (`help_` prefix) | PRESENT - Parlay `pl:help:main` / `pl:help:<topic>` with matching toasts; unknown-category alert exists (`dispatcher.cpp:7230`) |

## 1.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Track thumbnail field (carried, never rendered) | `Thumbnail` is stored on the track model and set from resolver results, but the now-playing and queue messages are text-only with web-page preview disabled | `internal/utils/models.go:18,39`; set in `internal/bot/play.go:296,349`; send path `internal/bot/play.go:465-477` | MISSING as TgMusicBot does it (text-only by design) - Parlay instead renders the thumbnail as a photo caption (`dispatcher.cpp:1687-1691`), which is the Yukki rule, not TgMusicBot's |
| Start-card in-body image | TDLib rich start message embeds an `<img src>` start image above the heading | `internal/bot/start.go:46` (`startHandler` template) | PRESENT in adapted form - Parlay sends the PM start card as a photo with the text as caption when a start image is configured (`dispatcher.cpp:6112` ping/start photo path; Phase 9 section 2 rule) |
| Thumbnail fallback | No fallback artwork: with no thumbnail the message is simply text; there is no default-thumb constant | Absent from `internal/utils/buttons.go` and `internal/bot/play.go` | NA - nothing to port; Parlay falls back to a text card when there is no thumbnail or thumbnails are off |

## 1.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start rich card (PM and group) | PM: in-body image, "Welcome, {firstName}!" heading, platform list, buttons paragraph. Group: "is ready!" heading, uptime line, in-body Updates/Group buttons | `internal/bot/start.go:46` | PRESENT - Parlay start cards (PM and group) with the same structure in Bot API HTML (`dispatcher.cpp:2542-2660`) |
| Help category rich pages | Heading, description, `<details open>` + `<summary>` collapsible sections, bordered striped tables of command/description | `internal/bot/help.go:20` (`getHelpCategories`), `:158` (handler) | PRESENT - Parlay help topic pages with tables converted to lines and expandable blockquotes (Phase 9 section 0 rule 1); native TDLib `<details>` is not available in Bot API HTML |
| Active-VC rich table | Heading, count paragraph, collapsible "Click to Show Active Chats" details block with a bordered striped table (index, chat id, queue, now-playing link) | `internal/bot/devs.go:24-82` | PRESENT in adapted form - Parlay active-VC list is plain lines, not a rich table; the collapsible-table form itself is MISSING, so this row is UNCERTAIN: content present, rich-table presentation not verified in Parlay |
| Playlist-info long rich page | Short format for small playlists; over 3000 runes, a rich page with heading, name/owner/song-count paragraph and a collapsible "Click to View Songs" table | `internal/bot/playlist.go:278` (`playlistInfoHandler`) | UNCERTAIN - Parlay has `/playlistinfo` (`dispatcher.cpp:5085`), but the long-form collapsible-table variant was not verified by grep |
| Privacy page | Full privacy-policy rich text: data stored, messages, third parties, sharing, removal | `internal/bot/admins.go:122` (`privacyHandler`); registered `internal/bot/load.go:47` | PRESENT - Parlay `/privacy` from the Yukki privacy page (same content family); TgMusicBot's exact text was not re-verified line by line, only the handler's existence |
| Voice-chat event messages | VC started: "Video chat started! Use /play ..."; VC ended: "Video chat ended! All queues cleared"; non-supergroup notice with conversion guide link, then leaves after 1 second | `internal/bot/watcher.go:21` (`handleVoiceChatMessage`) | PRESENT - Parlay VC started/ended messages (`dispatcher.cpp` voice-chat event texts) |
| Copy pills / hints / page footers | No copy-pill element and no page footer in this ref; copyable values appear as `<code>` spans (chat ids, command usage) | Throughout `internal/bot/*.go` (`<code>` usage) | PRESENT in adapted form - Parlay uses `<code>` spans for ids/commands and italic hints; Bot API has no native copy-pill element |
| Queued-multiple blockquote list | Blockquote list with per-track lines plus totals (see Plays) | `internal/bot/play.go:388-421` | PRESENT - Parlay multiple-added text with the same fields |

## 1.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons (general pattern) | Every rich card above (start, help, player, settings, playlist-info) carries a real inline keyboard; TDLib rich in-body `<tg-button>` elements are used only on the group start card | `internal/bot/start.go:46`; `internal/utils/buttons.go` | PRESENT - Parlay renders all such cards with real inline keyboards (`ui_texts.hpp` keyboard builders); in-body tg-buttons are translated to keyboards per Phase 9 section 0 rule 1 |

## 1.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Silent drop: commands are registered only via `OnCommand` in `load.go`; there is no `OnMessage` text fallback in this ref (the only `OnUpdateNewMessage` handler is for voice-chat messages), so an unregistered `/command` produces no reply | `internal/bot/load.go:23-47` (registrations); `internal/bot/load.go:97` (voice-chat-only message handler); no `OnMessage` handler found by grep | MISSING by design - Parlay deliberately answers unknown commands with an explicit reply plus the available-command list (`dispatcher.cpp:1007-1012`, `kUnknownCommand` in `ui_texts.hpp:694`); Parlay's behaviour is the opposite of TgMusicBot's |
| Developer denial on commands | Silent: non-developer handlers return `EndGroups` with no message (for example `/av`) | `internal/bot/devs.go:25-27` | PRESENT in spirit - Parlay permission gates fail silently for sudo/owner commands (`dispatcher.cpp:955` comment, `return` with no reply) |
| Unknown callback query | Unmatched callback prefixes have no handler registered, so no answer is sent (framework default); not directly observed in code | `internal/bot/load.go:87-93` (prefix registrations only) | UNCERTAIN - Parlay answers unknown callbacks with an alert toast (`dispatcher.cpp:6587-6598`), so Parlay's behaviour differs; whether TgMusicBot truly stays silent here was not runtime-verified |

---

# 2. TinaMusic - `refs/TinaMusic/` (C++20, Bot API via own client)

Primary sources: `locales/en.yml` (1064 lines) and `src/bot_engine.cpp`
(keyboard builders at lines 144-366, command dispatch at lines 434-566).
Style: small-caps Unicode, `<blockquote>` wrappers, spoiler and
strikethrough elements. Tina, Yukki and Arc share one lineage; Tina's
differences are the plus/minus-15s seek row, the hardcoded Join-Room URL
button, the Autoplay row in settings, and the `{thumb}`-in-text pattern.

## 2.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/addsudo` / `/delsudo` (owner-only, silent denial) | Owner-only: a non-owner call returns with no message at all; success replies "Added `<id>` to sudoers list" / "Removed `<id>` from sudoers list" | `src/bot_engine.cpp:508-510` (dispatch); handlers at `:1379-1398` | PRESENT - Parlay `/addsudo` / `/delsudo` are owner-gated and fail silently for non-owners (`dispatcher.cpp:955`, routes at `:470`); Parlay adds the self-add joke and already-sudo texts from the locale family |
| `/sudolist` / `/sudoers` / `/listsudo` | Lists owner entry plus sudo entries, or "No additional sudoers found" | `src/bot_engine.cpp:506-507` (dispatch); handler `:1369-1378`; locale keys `sudo_list_header`, `sudo_list_owner`, `sudo_list_entry`, `sudo_list_no_extra` in `locales/en.yml` | PRESENT - Parlay `/sudolist` / `/listsudo` (`dispatcher.cpp:472`, handler `onListSudo`, `kSudoHeader` / `kSudoNoExtra` in `ui_texts.hpp:2078-2079`) |
| Sudo join notices | Owner joining: "owner of {bot} just entered the chat"; sudo joining: "a sudo user of {bot} just joined the chat" | `locales/en.yml` keys `sudo_join_owner`, `sudo_join_sudo` | PRESENT - Parlay owner/sudo join notices (same texts, Phase 9 section 2) |
| Sudo denial strings (`only_sudo`, `only_owner`, `only_admin`) | Locale denial texts exist for sudo-only, owner-only and admin-only use | `locales/en.yml:477-478` (`only_owner`, `only_sudo`); `only_admin` family in same file | PRESENT - Parlay has matching denial texts and silent permission gates; the exact Tina wording is superseded by Parlay's texts |
| Sudo jokes (self-add, bot-add) | "You are the boss already, why are you even trying to add yourself?" and the bot-self joke; bot-user rejection ("sudoers must be human") | `locales/en.yml` keys `sudo_owner_self`, `sudo_bot_self`, `sudo_bot_user` | PRESENT - Parlay self-add joke (`ui_texts.hpp:2080` `kSudoSelfJoke`, used at `dispatcher.cpp:5710-5714`) and owner-remove block (`kSudoOwnerRemoveBlock`) |
| Sudo-gated blacklist commands | `/blockuser`, `/unblockuser`, `/blockchat`, `/unblockchat`, `/blacklisted` require sudo (owner counts); non-sudo returns silently | `src/bot_engine.cpp:512-524` (dispatch); handlers at `:1400-1454` (sudo check then return) | PRESENT - Parlay `/blacklist` / `/unblacklist` / `/blacklisted` are sudo-gated (`dispatcher.cpp:466-468`) |
| Owner/sudo-only system commands | `/eval`, `/sh`, `/maintenance`, `/restart`, `/logs`, `/stats` restricted to owner/sudo tiers | `src/bot_engine.cpp:496-498,524-548` (dispatch entries) | PRESENT - Parlay `/restart` (sudo), `/maintenance` (owner), `/eval`/`/sh` handling (`dispatcher.cpp:459-498`, `:1868-1869`) |

## 2.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card (text with inlined thumbnail link) | Blockquote header with `{thumb}` link plus "Now Playing" label; second blockquote with track link, duration in code, requested-by; sent as plain text with the player keyboard, never as a photo | `locales/en.yml` key `stream_now_playing`; send sites `src/bot_engine.cpp:682,815,907,1123,1159,1998,2021,2229,2255` (all text sends) | PRESENT in adapted form - Parlay now-playing card has the same fields but sends a photo caption when a thumbnail exists (`dispatcher.cpp:1687-1691`); Tina's text-inlined `{thumb}` pattern itself is MISSING by Phase 9 decision, so this row is UNCERTAIN in the strict sense: fields present, Tina's exact rendering not ported |
| Queued-single and queued-multiple cards | Queued-single mirrors the now-playing card with "Added to Queue: #N"; multiple: "Added N tracks by {user}" header plus queue-limit and view-queue hints | `locales/en.yml` keys `play_added_to_queue_single`, `play_added_multiple_header`, `play_queue_limit_hint`, `play_queue_view_hint` | PRESENT - Parlay queued cards with the same fields and a Play Now button |
| Player keyboard | Row 1: progress label button (no-op callback `progress`). Row 2: resume glyph, pause glyph, skip glyph, stop glyph (`room:<id>:...`). Row 3: seek-back-15s, replay glyph, seek-15s. Row 4: Close. Row 5: Join Room URL button to a hardcoded bot username | `src/bot_engine.cpp:208-264` (`MakePlayerMarkup`) | PRESENT except the Join-Room row - Parlay `playerKeyboard` has progress, pause/resume toggle (single toggle, not both glyphs), replay, skip, stop, seek rows, autoplay and Close/Add-to-Playlist; the hardcoded Join-Room URL button is MISSING by design (Phase 9 section 0 rule 7 drops hardcoded bot URLs) |
| Progress bar (12-char) | 12-character bar, ball glyph plus dash track, format "MM:SS [bar] MM:SS", zero-padded | `src/bot_engine.cpp:208-264` (bar built in `MakePlayerMarkup`) | MISSING as Tina builds it - Parlay uses the Yukki 9-stage bar (`ui_texts.hpp:95-114`, `progressBar`), not Tina's 12-char ball/dash bar |
| Callback toasts and edited state cards | Pause toast "Track paused at {position}" plus paused card (track, position, paused-by); resume, replay, skip, stop, mute, unmute, seek forward/back toasts and edited cards; near-end seek guard | `locales/en.yml` keys `cb_pause_message`, `cb_resume_message`, `cb_*` family | PRESENT - Parlay toasts and edited cards match (`ui_texts.hpp:1002-1031`, `dispatcher.cpp:7483-7492`) |
| Stop-confirm card | If paused: "Playback is currently paused. Do you want to resume playing or stop it completely?" with Resume + Stop buttons; if muted: the unmute variant | `locales/en.yml` keys `stop_confirm_paused`, `stop_confirm_muted`; label keys `CONFIRM_RESUME_BTN`, `CONFIRM_UNMUTE_BTN`, `CONFIRM_STOP_BTN` | PRESENT - Parlay stop-confirm (`ui_texts.hpp:1059-1074`, `pl:player:confirm:` callbacks, `dispatcher.cpp:7267-7314`) |
| Restart-confirm card | "A song is currently playing in this chat. Choose an action below." with Restart-Bot and Replay-Song buttons; confirmations "Restarting the bot..." / "Current song replayed successfully" | `locales/en.yml` keys `restart_confirm_running`, `restart_btn_bot`, `restart_btn_replay` (labels) | PRESENT - Parlay restart confirm (`pl:restart:bot|replay`, `dispatcher.cpp` restart-confirm handling) |
| `/player` re-send | Re-sends the current player card | `src/bot_engine.cpp:440` (dispatch `/player`, `/cplayer`); handler `:672` (`HandlePlayer`) | PRESENT - Parlay `/player` re-sends the current card and deletes the old card message (`dispatcher.cpp:370`, `3764`) |
| Queue display | Header "Current Queue", now-playing line, "Pending Tracks" numbered list; no pagination, no delete buttons, no Show-more; overflow line "...and N more" exists as a locale key but the render path does not use it; empty and already-empty texts | `src/bot_engine.cpp:1193-1219` (`HandleQueue`); locale keys `queue_header`, `queue_now_playing`, `queue_empty`, `queue_more_line`, `queue_empty_tail` | PRESENT - Parlay queue card supersedes this with pagination (15 per page), per-track delete and Show-more (`ui_texts.hpp:1178-1220`) |
| Queue ops | `/clear`, `/remove`, `/move`, `/restore`, `/shuffle`, `/loop` (0-10), `/seek`, `/seekback`, `/jump`, `/speed` (0.50x-4.00x) with usage, invalid-index and success texts | `src/bot_engine.cpp:464-490` (dispatch); locale keys `move_*`, `remove_*`, `clear_success`, `shuffle_*`, `loop_*`, `speed_*`, `seek_*`, `jump_*` | PRESENT - Parlay routes for clear/remove/move/shuffle/loop/seek/jump/speed (`dispatcher.cpp:326-352`) |
| Play flow statuses | "Searching...", "Searching for: {query}...", "Downloading song {title}", "Download canceled by {user}", no-query blockquote, "No tracks found", already-in-queue, too-long variants, "Failed to play / Error: {error}", "Unable to play - restart the voice chat" | `locales/en.yml` keys `searching`, `searching_query`, `play_downloading_song`, `play_download_canceled`, `no_song_query`, `no_song_found`, `play_already_in_queue`, `play_failed`, `play_unable` | PRESENT - Parlay play lifecycle texts match (searching, downloading, no-query, not-found, already-in-queue, failure) |
| Download progress message | No percentage/progress-bar download message exists; only the cancel label `DOWNLOAD_CANCEL_BTN` ("Cancel Downloading") and the cancelled/no-download/invalid-request/unknown-action texts exist, with no keyboard builder found for the cancel button | `locales/en.yml` key `DOWNLOAD_CANCEL_BTN`; no `Make*Cancel*` builder among `src/bot_engine.cpp:144-366` | PRESENT in Parlay (and more complete) - Parlay has a live download progress message with a working cancel button (`pl:dl:cancel:`, `dispatcher.cpp:4143-4229`); Tina's label-only, builder-less state is not reproduced |
| `/position` | "Track title, Position / Duration in code, Speed: Nx" | `locales/en.yml` key `position_now`; dispatch `src/bot_engine.cpp:472` | PRESENT - Parlay `/pos` (`dispatcher.cpp:338`) with the same three fields |
| Autoplay card | "Autoplay Mode Settings" card with current status (enabled/disabled), explanation, and a single-row toggle button plus Close | `src/bot_engine.cpp:266-289` (`BuildAutoplayMarkup`, `GetAutoplayMessage`) | PRESENT - Parlay autoplay row on the player card and settings panel, plus `/autoplay` toggle; the standalone autoplay card is UNCERTAIN (not verified as a separate Parlay card) |
| Play history | No play-history view or command | Absent from `locales/en.yml` help texts and `src/bot_engine.cpp:434-566` | NA - no equivalent to port; Parlay also has no play-history command |
| Voice-chat events | VC started: "Voice chat started! Use /play [song name] to play music"; VC ended: "Video chat ended, Duration: {duration}"; queue finished and next-downloading notices | `locales/en.yml` keys `voicechat_started`, `voicechat_ended`, `stream_queue_finished`, `stream_downloading_next` | PRESENT - Parlay VC started/ended and queue-finished texts |

## 2.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | Row 1 Add-Me URL button; row 2 Help and Commands callback (`help:main`); row 3 Updates + Support URL buttons; no Source button, no Language button | `src/bot_engine.cpp:144-164` (`MakeStartMarkup`) | PRESENT - Parlay start keyboard adds Language and Source rows; Tina's exact 3-row layout is a subset of Parlay's |
| Help keyboard | Row 1 Admins + Public; row 2 Owner + Sudoers; row 3 Back to start; category pages use a single Back-to-help-main button | `src/bot_engine.cpp:166-183` (`MakeHelpMarkup`); back builder at `:185` (`MakeBackMarkup`) | PRESENT - Parlay help keyboard (merged 12-topic set) with Back to start and Back to help-main |
| Settings keyboard (9 rows) | Label + value rows: Admin Mode, Play Mode, Cmd Delete, Clean Mode, Clean Delay (cycles 15m, 30m, 60m, 5m), Thumbnails, Autoplay (Tina-only row), Language, Close; info taps answer an alert with the explainer; set taps toggle and re-edit; toast "Settings updated!" | `src/bot_engine.cpp:291-348` (`MakeSettingsMarkup`); handler `:1299` (`HandleSettings`); locale keys `settings_info_*`, `settings_updated` | PRESENT - Parlay `settingsKeyboard` has the same rows plus auto-delete, timer and silent rows (`ui_texts.hpp:1491`); Parlay clean-delay is a value, not the 15/30/60/5 cycle - the cycle behaviour is UNCERTAIN |
| Language picker | Rows of two: English, Hindi, Arabic, Turkish; Close row; prompt "Select Bot Language:"; success / already-using / failure replies | `src/bot_engine.cpp:350-366` (`MakeLanguageMarkup`); prompt at `:1318`; locale keys `lang_success`, `lang_same`, `lang_fail` | PRESENT in adapted form - Parlay language picker lists English only (`dispatcher.cpp` language handling, "This build ships English only"); the 4-language picker itself is MISSING |
| Support markup and group-help button | Support markup: single Support URL button. Group-help: single Commands URL button to `?start=pm_help` | `src/bot_engine.cpp:196` (`MakeSupportMarkup`); group-help button per `locales/en.yml` `GC_HELP_BTN` | PRESENT - Parlay group-help URL button and support buttons on start/help cards |
| Broadcast cancel button | "Cancel Broadcast" button, callback `bcast_cancel`; owner-only cancel text | `locales/en.yml` key `BROADCAST_CANCEL_BTN`; broadcast handler `src/bot_engine.cpp:1321` | PRESENT - Parlay `pl:bcast:cancel` (`ui_texts.hpp:223`) |
| Download-cancel label (no builder) | Cancel-Downloading label exists in locale but no keyboard builder constructs it | `locales/en.yml` key `DOWNLOAD_CANCEL_BTN`; no builder in `src/bot_engine.cpp:144-366` | MISSING as Tina has it (label without a button) - Parlay instead has a working cancel button, so the feature is PRESENT and the defect is not reproduced |
| Reply keyboards | None in this ref | Absent from `src/bot_engine.cpp` builders | NA - nothing to port; Parlay's persistent reply keyboard comes from beetube, not Tina |

## 2.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| In-text thumbnail link (`{thumb}`) | Now-playing, queued and next-track texts start with a link on a music-note glyph pointing at the track thumbnail URL; the message itself is text, never a photo | `src/bot_engine.cpp:683` (now-playing thumb build), `:811-816,830-835,905-908,1119-1124,1155` (queued/next variants); locale `stream_now_playing`, `play_added_to_queue_single` | MISSING by Phase 9 decision - Parlay sends a photo with the card as caption instead (`dispatcher.cpp:1687-1691`); the `{thumb}`-in-text pattern is explicitly not ported (Phase 9 section 0 rule 3) |
| Thumbnail fallback | When thumbnails are off or the track has no thumbnail URL, the `{thumb}` slot is a bare music-note glyph | `src/bot_engine.cpp:811-813,905` (fallback assignment) | PRESENT in adapted form - Parlay falls back to a text card under the same conditions (`dispatcher.cpp:1691`) |
| `/nothumb` toggle and settings row | Per-chat thumbnail on/off: status, already-set, update-failure and updated texts; settings row "Thumbnails" with Enabled/Disabled value | `src/bot_engine.cpp:486` (dispatch `/nothumb`, `/nothumbs`); settings row at `:332-333`; locale keys `nothumb_status`, `nothumb_updated`, `settings_info_nothumb` | PRESENT - Parlay `/nothumb` command and Thumbnails settings row (`ui_texts.hpp:1491`, `dispatcher.cpp:5347`) |
| Start/help in-text image links | Start and help texts embed links to hosted image files as the leading glyph link | `locales/en.yml` keys `start_private`, `help_main` (image links in text) | PRESENT in adapted form - Parlay uses a configured start photo, not hardcoded hosted image links |

## 2.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start card (PM) | Blockquote greeting with about lines, strikethrough "laggy audio" replaced by a spoiler "ultra HD quality", platform code list, spoiler status line; expandable blockquote quick guide with play/vplay/settings/help lines and spoiler tips | `locales/en.yml` key `start_private` | PRESENT in adapted form - Parlay PM start card keeps the structure in normal English; spoiler/strikethrough/small-caps styling is not ported (Phase 9 section 0 rule 1 converts to normal English Bot API HTML) |
| Help menu and topic pages | Blockquote help menu with category lines and a spoiler tip; four topic pages (Admins, Public, Owner, Sudoers) as blockquotes with per-command lines and spoiler footers | `locales/en.yml` keys `help_main`, `help_admin`, `help_public`, `help_owner`, `help_sudo` | PRESENT - Parlay help menu and topic pages (merged set); spoiler styling dropped, blockquote structure kept |
| Expandable blockquotes (collapsible equivalent) | Expandable blockquotes used for the quick guide, queue-finished notice, autoplay notice and similar long texts | `locales/en.yml` keys `start_private` (guide block), `stream_queue_finished`, `autoplay_fetching_next` | PRESENT - Parlay uses `<blockquote expandable>` for long texts (Phase 9 section 0 rule 1) |
| Ping and stats cards | Ping: "Pinging..." blockquote, then "Pong!" blockquote with uptime/RAM/CPU/disk/bot lines and spoiler status/ping lines. Stats: blockquoted sections for system, Go memory, server, MongoDB, served counts, runtime versions | `locales/en.yml` keys `ping_start`, `ping_result`, `stats_overview` | PRESENT - Parlay ping card (`ui_texts.hpp` `pingCardText`) and stats card; spoiler lines dropped |
| Settings text | "Chat Settings" heading, group title and id in code, "Configure the bot's behavior in this group using the buttons below" | `locales/en.yml` key `settings_main`; handler `src/bot_engine.cpp:1299` | PRESENT - Parlay settings panel text with the same fields |
| Copy pills / hints / page footers | No native copy-pill element; copyable values are `<code>` spans (ids, usages, examples); hints appear as italic lines and spoiler tips; no page footers on lists | Throughout `locales/en.yml` (`<code>` usage, `<i>` hints) | PRESENT in adapted form - Parlay uses `<code>` spans and italic hints; there is no Bot API copy-pill element to port |
| Per-screen layouts (queue, broadcast, active, RTMP) | Queue: header + now-playing + pending list. Broadcast: edited-in-place progress lines (total chats/users, failed counts, delay, elapsed, ETA, success rate). Active: count line. RTMP: DM-only setup guide, status card | `src/bot_engine.cpp:1193` (queue), `:1321` (broadcast), `:1455` (active); locale keys `broadcast_*`, `active_chats_info`, `rtmp_*` | PRESENT - Parlay queue card, broadcast progress and active list; RTMP status exists as `/streamstatus` (`dispatcher.cpp` stream routes) |

## 2.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons (general pattern) | Start, help, player, autoplay, settings and language cards are all rich texts (blockquote/spoiler) carrying real inline keyboards | `src/bot_engine.cpp:144-366` (all `Make*Markup` builders) | PRESENT - Parlay renders the same cards with real inline keyboards and Bot API HTML |

## 2.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Silent drop: the dispatch chain is a long if/else-if over exact command strings with no trailing else and no fallback reply, so an unrecognised command simply ends the function | `src/bot_engine.cpp:434-566` (dispatch chain ends at `:564-566` with no else) | MISSING by design - Parlay answers unknown commands explicitly (`dispatcher.cpp:1007-1012`); Parlay's behaviour is the opposite of Tina's |
| Non-owner sudo denial | Silent: `HandleAddSudo` / `HandleDelSudo` return immediately for non-owners with no message | `src/bot_engine.cpp:1379-1398` (owner check then return) | PRESENT - Parlay permission gates fail silently (`dispatcher.cpp:955`) |
| Unknown callback action | Locale keys `invalid_request` ("Invalid request") and `unknown_action` ("Unknown action") exist for bad callbacks | `locales/en.yml` keys `invalid_request`, `unknown_action` | PRESENT - Parlay answers unknown callbacks with an alert toast (`dispatcher.cpp:6587-6598`, `kToastUnknownAction`) |
| Rate-limit (flood) denial | Not silent: "Slow down! Try again in N seconds" / "Please wait N minutes before using this command again" | `locales/en.yml` keys `flood_seconds`, `flood_minutes` | PRESENT - Parlay rate-limit handling exists (`dispatcher.cpp` `checkRateLimit`); exact wording differs |

---

# 3. YukkiMusic-Go - `refs/YukkiMusic-Go/` (Go, gotdbot, TDLib rich messages)

Primary sources: `internal/locales/en.yml` (882 lines), `internal/core/buttons.go`
(all markups), `internal/modules/handlers.go` (command registrations lines
36-115, callback registrations lines 116-147), `internal/modules/queue.go`,
`internal/modules/settings.go`, `internal/modules/dashboard.go`,
`internal/modules/auth.go`, `internal/modules/callback.go`.
Style: TDLib rich messages - headings, paragraphs, asides, footers, bordered
striped compact tables, in-body tg-buttons, custom-emoji button icons and
colour styles unless colour is disabled. Emoji in locale strings go through
`{emoji:X}` tokens.

## 3.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/addsudo` (+ aliases addsudoer, sudoadd) | Owner adds a user to the sudo list, with self/bot/already checks and success/failure texts | `internal/modules/handlers.go:72`; handler `internal/modules/sudoers.go:34-146` | PRESENT - Parlay `/addsudo` with aliases addsudoer/sudoadd (`dispatcher.cpp:470`, aliases at `:520-521`) |
| `/delsudo` (+ aliases delsudoer, sudodel, remsudo, rmsudo, sudorem, dropsudo, unsudo) | Owner removes a sudo user, with owner-remove block and not-sudo texts | `internal/modules/handlers.go:94`; handler `internal/modules/sudoers.go:147-232` | PRESENT - Parlay `/delsudo` with the same alias family (`dispatcher.cpp:470`, `:522-524`) |
| `/sudoers` / `/listsudo` / `/sudolist` list screen | "Fetching..." status, then header "Current Sudoers", owner entry marked Owner, numbered entries, "No additional sudoers found" when empty; fetch-failure text | `internal/modules/handlers.go:97`; handler `internal/modules/sudoers.go:233-312`; locale keys `sudo_list_fetching`, `sudo_list_header`, `sudo_list_owner`, `sudo_list_entry`, `sudo_list_no_extra`, `sudo_list_fetch_fail` | PRESENT - Parlay `/sudolist` / `/listsudo` (`dispatcher.cpp:472`, `onListSudo`); Parlay lists ids without the fetching status edit |
| Sudo gate (`checkSudo`) and denial forms | Gate helper used by stats, logs, logger, autoleave and sudo commands; callback form answers the `only_sudo_cb` alert; command-form denial varies by handler (for example `/stats` returns without reply for non-sudo) | `internal/modules/helpers.go:46-63` (`checkSudo`, `allowed`); callback use `internal/modules/autoleave.go:72`, `internal/modules/logger.go:37`; `/stats` gate `internal/modules/stats.go:77-87` | UNCERTAIN - Parlay has the gate (silent for commands, not-allowed toast for callbacks), but the exact per-handler variation in Yukki (silent vs message) was not mapped handler by handler |
| Sudo join notices | Same owner/sudo join texts as Tina | `internal/locales/en.yml` keys `sudo_join_owner`, `sudo_join_sudo` | PRESENT - Parlay join notices |
| Sudo-only commands (`/stats`, `/logs`, `/logger`, `/autoleave`, `/active`) | Stats (rich, see Rich), logs viewer, logger toggle, autoleave toggle, active-chats - all sudo-gated | `internal/modules/handlers.go:71,75,100,103,109`; handlers `stats.go`, `logger.go`, `autoleave.go`, `active.go` | PRESENT - Parlay `/stats`/`/status`, `/logger`, `/ac` exist with sudo gates; `/logs` viewer is UNCERTAIN (not found as a Parlay command by grep) |
| Owner-only commands (blocklist, broadcast, maintenance, restart, shell) | `/blockuser`, `/unblockuser`, `/blockchat`, `/unblockchat`, `/blacklisted`, `/broadcast`, `/maintenance`, `/restart`, `/sh` owner-gated | `internal/modules/handlers.go:90-96,107` | PRESENT - Parlay owner/sudo routes for blacklist, broadcast, maintenance, restart (`dispatcher.cpp:459-498`) |

## 3.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card (photo caption) | "Now Playing:" heading, track link, duration, requested-by; sent as a photo with the card as caption when artwork exists and thumbnails are on; edit via EditMessageMedia; photo failure falls back to text with web-page preview disabled; title truncated to 25 chars | `internal/modules/helpers.go:239-310` (`showTrackMessage`); locale key `stream_now_playing`; truncation `ShortTitle(t.Title, 25)` | PRESENT - Parlay now-playing card follows exactly this rule (`dispatcher.cpp:1678-1699`) |
| Queued-single and queued-multiple cards | Queued-single mirrors now-playing with "Added to Queue: #N"; multiple: "Added N tracks by {user}" plus queue-limit and view hints | `internal/locales/en.yml` keys `play_added_to_queue_single`, `play_added_multiple_header`, `play_queue_limit_hint`, `play_queue_view_hint` | PRESENT - Parlay queued cards with the same fields |
| Player keyboard (toggle form) | Row 1 (only when not queued): progress label button (no-op callback `progress`). Row 2: pause/resume toggle (shows resume glyph when paused, pause glyph while playing), replay glyph, skip glyph, stop glyph (`room:<id>:...`). Row 3: Close. No plus/minus-15s row | `internal/core/buttons.go:180-215` (`GetPlayMarkup`); progress bar at `:188` | PRESENT - Parlay `playerKeyboard` has the same rows plus the Tina seek row and an autoplay row; Parlay toggle shows Pause/Resume text |
| Single-action toggle keyboards | Pause toggle card: Pause / Resume button; mute toggle card: Mute / Unmute button | `internal/core/buttons.go:122-149` (`getActionToggle`, `GetPauseToggle`, `GetMuteToggle`) | PRESENT - Parlay `/menu` panel carries Mute/Unmute and Pause/Resume buttons (`ui_texts.hpp:1126`); standalone single-action toggle cards are UNCERTAIN |
| Stop-confirm keyboard | Single row: Resume-or-Unmute button (green) plus Stop button (red); paused and muted confirm texts | `internal/core/buttons.go:151-179` (`GetStopConfirmMarkup`); locale keys `stop_confirm_paused`, `stop_confirm_muted` | PRESENT - Parlay stop-confirm (`ui_texts.hpp:1059-1074`) |
| Restart-confirm keyboard | Single row: Restart-the-Bot (red) plus Replay-the-Song (green); texts "A song is currently playing in this chat. Choose an action below", "Restarting the bot...", "Current song replayed successfully" | `internal/core/buttons.go:305-312` (`GetRestartConfirmMarkup`); locale keys `restart_confirm_running`, `restart_confirm_bot`, `restart_confirm_replay_done` | PRESENT - Parlay restart confirm |
| Callback toasts and edited state cards | Pause/resume/replay/skip/stop/mute/unmute toasts and edited cards (track, position, paused/muted/resumed-by lines); already-muted / not-muted / already-paused texts; pause/resume/mute/unmute failure texts | `internal/modules/callback.go:103-115` (actions), `:259-330` (pause/resume/replay handlers), `:508-529` (status cases); locale keys `cb_*` | PRESENT - Parlay toasts and edited cards match |
| Queue rich card (table + delete + Show-more) | Heading "Current Queue", now-playing aside (title capped at 40 chars, duration, requester), "Up Next - N tracks" paragraph, bordered striped compact table (index, track link, duration, requester, delete button per row), Show-more button when more remain, "No more songs in queue" footer when empty; first page 15 tracks | `internal/modules/queue.go:119-198` (`queueRich`); page size constant at `:122` area (`queueFirstPage = 15`); delete token rule at `:97-117` (`trackDeleteToken`: full id if 20 chars or fewer and no colon, else first 10 hex chars of SHA-256) | PRESENT - Parlay queue card is this design translated to Bot API HTML (`dispatcher.cpp:1698-1770`, `ui_texts.hpp:1178-1220`, token rule in `trackToken`) |
| Queue ops and queue strings | Empty, already-empty, no-active, limit-reached texts; queue finished, next-downloading, autoplay-next texts; remove/move/clear/shuffle/loop/seek/speed handlers | `internal/modules/queue.go:35-67` (handlers); `internal/locales/en.yml` keys `queue_*`, `stream_queue_finished`, `stream_downloading_next`, `stream_autoplay_next` | PRESENT - Parlay queue ops and lifecycle texts |
| Download progress message | Edited-in-place status: "Downloading your track...", Progress percent (1 decimal), Speed (bytes per second), ETA, Elapsed; cancel keyboard with a single Cancel-Downloading button (callback `cancel`); cancelled / no-download toasts | `internal/modules/download.go:207-214` (progress edit; full handler context in same file); cancel keyboard `internal/core/buttons.go:89-96` (`GetCancelKeyboard`); handler `internal/modules/callback.go:37` | PRESENT - Parlay download progress with the same fields and a working cancel button (`dispatcher.cpp:4143-4229`) |
| Play flow statuses | "Searching...", "Searching for: {query}...", "Downloading song {title}", "Download canceled by {user}", no-query block, "No tracks found", already-in-queue, already-playing, too-long variants, "Failed to play / Error: {error}", "Unable to Play - restart the voice chat", RTMP refusal | `internal/locales/en.yml` keys `searching`, `searching_query`, `play_downloading_song`, `play_download_canceled`, `no_song_query`, `no_song_found`, `play_already_in_queue`, `play_already_playing`, `play_failed`, `play_unable`, `rtmp_play_unsupported` | PRESENT - Parlay play lifecycle texts match |
| `/position` | Title, position/duration in code, speed line | `internal/modules/position.go` (handler); locale key `position_now` (family) | PRESENT - Parlay `/pos` |
| Play from reply / media / link / channel play | Reply-to-media play, link play, channel-play variants (`/cplay`, `/cskip`, etc. with `croom:` callbacks) and channel-play setup (`/setcplay`) | `internal/modules/handlers.go:40-89` (c-prefixed registrations); `internal/modules/cplay.go`; locale keys `cplay_*` | PRESENT - Parlay reply-media play and channel-play variants exist in routes; the `croom:` prefix itself is MISSING (Parlay uses the same `pl:player:` form with the channel id, per Phase 9 section 1) |
| Play history | No play-history view or command | Absent from `internal/modules/handlers.go:36-115` and `internal/locales/en.yml` | NA - no equivalent to port; Parlay also has no play-history command |
| Voice-chat events | VC started / VC ended rich messages with duration | `internal/locales/en.yml` keys `voicechat_started`, `voicechat_ended`; handler `internal/modules/actions.go:35-50` | PRESENT - Parlay VC event messages |

## 3.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | Row 1 Add-Me URL button; row 2 Help and Commands callback (`help_cb`); row 3 Updates + Support URL buttons; row 4 Language callback (`lang:select`) + Source Code URL button | `internal/core/buttons.go:231-264` (`GetStartMarkup`) | PRESENT - Parlay start keyboard (same rows; Parlay source URL comes from config, not a hardcoded repo link) |
| Group start in-body buttons | Group start is a rich message with in-body Updates and Support URL buttons, no inline keyboard | `internal/modules/start.go:38-43` | PRESENT in adapted form - Parlay group start carries real inline-keyboard URL buttons (in-body tg-buttons are translated per Phase 9 section 0 rule 1) |
| Group help button | Single Commands URL button to `?start=pm_help` | `internal/core/buttons.go:222-230` (`GetGroupHelpKeyboard`) | PRESENT - Parlay group-help URL button |
| Help keyboard | Row 1 Admins + Public; row 2 Owner + Sudoers; row 3 Back (blue) to start; category pages use a single Back button to help-main | `internal/core/buttons.go:266-303` (`GetHelpKeyboard`, `GetBackKeyboard`) | PRESENT - Parlay help keyboard (merged 12-topic set) with the same Back behaviour |
| Settings keyboard (8 rows) | Label + value rows: Admin Mode, Play Mode, Cmd Delete, Clean Mode, Clean Delay, Thumbnails, Language, Close; info taps answer an explainer alert; set taps toggle/cycle and re-edit; toast "Settings updated!"; settings callbacks are admin-gated with an `only_admin_cb` alert on denial | `internal/modules/settings.go:334-456` (`buildSettingsMarkup`); handler at `:224-250` and `:250-333`; locale keys `settings_info_*`, `settings_updated` | PRESENT - Parlay `settingsKeyboard` covers all eight rows plus auto-delete, timer, silent and settings-index rows |
| Language picker | One button per language, two per row, current language prefixed with a check mark, callback `lang:<code>`; optional Back row to start or help-main; select/success/already/failure texts | `internal/modules/language.go:66-97` (`buildLangMarkup`); locale keys `lang_select`, `lang_success`, `lang_same`, `lang_fail` | PRESENT in adapted form - Parlay picker lists English only; the multi-language picker is MISSING |
| Dashboard toggle cards (shared pattern) | Rich card per setting: title heading, description aside, status line (Enabled/Disabled), single row with Close + toggle button (`dashboard:<name>`); tap flips the setting, refreshes the card in place, toast "Setting updated to {status}", failure toast; registered dashboards: autoplay, autoleave, maintenance, logger, cmddelete, cleanmode, playmode, nothumb | `internal/modules/dashboard.go:47-131` (`dashboardRich`, `dashboardMarkup`, `dashboardCallback`); callback registration `internal/modules/handlers.go:144` | PRESENT in adapted form - Parlay settings panel toggles cover the same settings; the standalone dashboard-card presentation per setting is UNCERTAIN (not verified as separate Parlay cards) |
| Auth list rich table with revoke buttons | Heading "Authorized Users", count/limit line, bordered striped compact table (user, id, delete button per row, callback `authdel:<chat>:<user>`), footer "Tap delete to revoke access"; delete toast "Authorization revoked" | `internal/modules/auth.go:235-303` (`authRich`, `authDeleteHandler`); registration `internal/modules/handlers.go:145` | PRESENT - Parlay `/authlist` with revoke behaviour; the rich-table presentation is translated to lines in Bot API HTML |
| Broadcast cancel keyboard | Single Cancel-Broadcast button, callback `bcast_cancel`; owner-only cancel text; already-running text with cancel hint | `internal/core/buttons.go:98-106` (`GetBroadcastCancelKeyboard`); handler `internal/modules/broadcast.go:121-204` | PRESENT - Parlay `pl:bcast:cancel` |
| Progress no-op button | Progress label button answers nothing (empty callback handler) | `internal/modules/callback.go:58` (`emptyCBHandler`); registration `internal/modules/handlers.go:147` (progress regex) | PRESENT - Parlay `pl:player:progress:<chat>` no-op (`dispatcher.cpp:7261`) |
| Close button | Deletes the message; callback `close` | `internal/modules/callback.go:52` (`closeHandler`) | PRESENT - Parlay `pl:close` |
| Complete callback inventory | `help_cb`, `help:<cat>`, `start`, `lang:select`, `lang:<code>`, `set:<key>`, `info:<key>`, `dashboard:<name>`, `room:<id>:<action>`, `progress`, `qdel:<chat>:<token>:<shown>`, `qmore:<chat>`, `authdel:<chat>:<user>`, `restart:<bot\|replay>`, `cancel`, `bcast_cancel`, `close` | `internal/modules/handlers.go:116-147` | PRESENT as a set - Parlay `pl:` namespace covers every verb (Phase 9 section 1 mapping table); raw namespaces themselves are MISSING by design |

## 3.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Photo now-playing card | Artwork sent as a photo with the card as caption; edits use EditMessageMedia; photo failure falls back to text | `internal/modules/helpers.go:239-310` (`showTrackMessage`) | PRESENT - Parlay follows exactly this rule (`dispatcher.cpp:1687-1699`) |
| Thumbnail fallback | No artwork or thumbnails disabled: plain text message with web-page preview disabled | `internal/modules/helpers.go:239-310` (branches) | PRESENT - Parlay text-card fallback (`dispatcher.cpp:1691`) |
| `/nothumb` toggle, settings row, dashboard card | Per-chat thumbnail on/off via command, settings row and dashboard card; nothumb status and updated texts | `internal/modules/handlers.go:88` (`/nothumb`); `internal/modules/settings.go:334-456` (row); `internal/modules/dashboard.go` (nothumb dashboard) | PRESENT - Parlay `/nothumb` and Thumbnails settings row |
| Title truncation on cards | Card title truncated to 25 chars; queue titles to 40 chars | `internal/modules/helpers.go:239-310` (`ShortTitle(t.Title, 25)`); `internal/modules/queue.go:119-198` (40-char queue titles) | PRESENT - Parlay truncates card titles the same way (Phase 9 section 4) |
| Start image (rich start) | Start rich message may carry a photo (photo-message detection helper) | `internal/modules/start.go:101` (`isPhotoMessage`) | PRESENT in adapted form - Parlay configured start photo |

## 3.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start card (PM) | Heading with bot name, paragraph describing streaming sources and controls, bold quality line, footer tip | `internal/locales/en.yml` key `start_private`; builder `internal/modules/start.go:69-100` | PRESENT - Parlay PM start card (same structure, normal English) |
| Help menu and topic pages | Heading, browse paragraph, tip aside, support footer; topic pages as grouped tables (playback, queue, seek and speed, settings and access; owner/sudo tables) with channel-variant footer | `internal/locales/en.yml` keys `help_main`, `help_owner`, `help_sudo`, `help_admin`, `help_public`; builder `internal/modules/help.go:76-110` | PRESENT - Parlay help menu and topic pages (tables converted to lines) |
| Per-command help | Every command has a rich help block (intro, usage, restrictions, use case); channel variants auto-generated as "Channel play variant of ..." plus base text; unknown command: "No help found for command" | `internal/modules/help.go:149-153` (`showHelpFor`); registrations `internal/modules/handlers.go:520-521` area (channel variant generation, per Phase 9) | PRESENT - Parlay per-command help (`ui_texts.hpp` `kCmdHelpTable`, `dispatcher.cpp:2684`) |
| Rich queue card | See Plays: heading, now-playing aside, up-next paragraph, bordered striped compact table, Show-more in-body button, empty footer | `internal/modules/queue.go:119-198` | PRESENT translated to Bot API HTML (lines + inline delete/Show-more buttons) |
| Rich settings and dashboard cards | Settings: heading, title/chat-id paragraph, instruction paragraph. Dashboards: title heading, description aside, status paragraph | `internal/modules/settings.go:224-250`; `internal/modules/dashboard.go:47-65` | PRESENT - Parlay settings panel text and toggle rows |
| Privacy page | Full privacy page: stored-data table (ids, preferences, access lists), messages aside, third-party paragraph, sharing paragraph, removal paragraph, open-source/support footer | `internal/modules/privacy.go:44-84` | PRESENT - Parlay `/privacy` (Yukki text is the chosen source, Phase 9 section 3) |
| Stats rich card (sudo-only) | Heading, overview aside, system table, Go memory rows, server rows, collapsible MongoDB details block, collections details block, bot-overview rows, versions rows; status glyphs by warn/crit thresholds | `internal/modules/stats.go:93-221` (`buildStats`); gate at `:77-87` | PRESENT in adapted form - Parlay stats card with the same sections; collapsible details blocks are translated (Bot API has no `<details>`) |
| Mute / pause / resume rich cards | Mute, unmute, pause and resume actions render rich heading-plus-table state cards, not plain replies | `internal/modules/callback.go:259-330` and mute/unmute handlers; locale keys `cb_*` | PRESENT - Parlay edited state cards (paused/muted cards with track and by-lines) |
| Collapsibles (`<details>`) | MongoDB and collections sections of stats, and help/queue details blocks, use collapsible details/summary | `internal/modules/stats.go:93-221`; `internal/modules/queue.go:119-198` | MISSING as native elements - Parlay translates them to expandable blockquotes or Show-more buttons (Phase 9 section 0 rule 1); content is PRESENT |
| Copy pills / hints / page footers | No native copy-pill element; copyable values are `<code>` spans; hints are asides/footers; queue card has an empty footer line; help has a tip aside and support footer | Throughout `internal/locales/en.yml` and `internal/modules/*.go` | PRESENT in adapted form - Parlay uses `<code>` spans, italic hints and footer lines |

## 3.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons (general pattern) | Start, help, settings, dashboard, queue, auth and player rich messages all carry real inline keyboards; group start additionally embeds in-body tg-buttons | `internal/core/buttons.go`; `internal/modules/start.go:38-43` | PRESENT - Parlay renders all such cards with real inline keyboards |

## 3.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Silent drop: commands are registered only via `OnCommand`; the `OnMessage` handler only processes member-add and voice-chat started/ended events and returns nil for everything else, so an unregistered command produces no reply | `internal/modules/handlers.go:36-115` (command registrations), `:124` (`OnMessage` with `handleActions`); `internal/modules/actions.go:35-50` (`handleActions` switch: member-add, VC started, VC ended, default nil) | MISSING by design - Parlay answers unknown commands explicitly (`dispatcher.cpp:1007-1012`) |
| Unknown callback action | Unknown room actions answer an alert toast "Unknown action"; unknown dashboard callbacks are logged (no user toast observed) | `internal/modules/callback.go:118-119` (unknown action alert); `internal/modules/dashboard.go:120` (unknown dashboard log) | PRESENT for the alert form - Parlay answers unknown callbacks with an alert toast (`dispatcher.cpp:6587-6598`) |
| Admin callback denial | Non-admin settings/dashboard taps answer the `only_admin_cb` alert ("Only admins can do these actions") | `internal/modules/helpers.go:144` (`canUseAdminCallback`); locale key `only_admin_cb` | PRESENT - Parlay not-allowed toast for non-controller taps (`dispatcher.cpp:7277-7280`) |
| Panel requester-only taps | Control-panel presses by anyone other than the panel owner are silently ignored (empty answer) in the remix-style panel; Yukki's own player callbacks instead check admin-or-auth and answer a denial toast | `internal/modules/callback.go:223` (`checkAdminOrAuth`) | PRESENT both forms - Parlay `/menu` panel presses by others are silently ignored (`dispatcher.cpp:7338` comment), while player callbacks answer a not-allowed toast |

---

# 4. anonx-cpp - `refs/anonx-cpp/` (C++20, TDLib, minimal)

Only source: `src/telegram/dispatcher.cpp` (340 lines) plus
`include/anonx/telegram/dispatcher.hpp`. All UI is plain-text Markdown sent
via `TDLibClient::send_message`, replying to the command message. No
keyboards of any kind, no callbacks, no inline mode, no thumbnails, no
settings, no lyrics, no search, no download progress, no auth/sudo, no
channel play, no playlists, no maintenance/broadcast. This ref is the
smallest surface in the group, and that smallness is itself verified:
the whole command set fits in one if/else chain.

## 4.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner management | No sudo, owner, admin or auth concept anywhere in the dispatcher | Absent from `src/telegram/dispatcher.cpp:137-158` (complete command list) | NA - no equivalent to port; Parlay has full sudo/owner/auth from the Tina/Yukki lineage |
| Sudo denial behaviour | No denial exists because no gated command exists; every command is open to every user | `src/telegram/dispatcher.cpp:137-158` | NA - nothing to port |
| Sudo list screen | None | Absent from `src/telegram/dispatcher.cpp` | NA |

## 4.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing plain reply | "Now Playing: {query-as-title}" plus "Requested by: `{user_id}`" - the title is the raw query string, because this codebase does no search or resolution | `src/telegram/dispatcher.cpp:184-189` (`handle_play`, start branch) | PRESENT in adapted form - Parlay now-playing card resolves real titles and adds duration, thumbnail and buttons; the raw-query-as-title behaviour is MISSING (Parlay resolves, which is strictly better) |
| Queued plain reply | "Queued at position #{queue_size}: {query-as-title}" | `src/telegram/dispatcher.cpp:194-197` | PRESENT - Parlay queued card with real title, duration and Play Now button |
| Next-track plain reply | "Now Playing Next Track: {title}" when the queue advances | `src/telegram/dispatcher.cpp:326` (`play_next_in_queue`) | PRESENT - Parlay next-track card on advance |
| Queue finished reply | "Queue completed. Voice chat playback ended." | `src/telegram/dispatcher.cpp:336` | PRESENT - Parlay queue-finished text |
| Queue display | "Current Track and Queue:" heading, "Now Playing: {title}" line, "Upcoming Tracks:" numbered list capped at the first 10, "...and N more tracks" overflow line; empty: "The queue is currently empty" (both sections omitted when absent) | `src/telegram/dispatcher.cpp:234-260` (`handle_queue`) | PRESENT - Parlay queue card supersedes this (15 per page, delete buttons, Show-more) |
| Pause / resume plain replies | Pause: "Playback paused" / "No active playback to pause". Resume: "Playback resumed" / "Playback is not paused" | `src/telegram/dispatcher.cpp` (`handle_pause`, `handle_resume`, in the 137-340 handler range) | PRESENT - Parlay pause/resume command replies and callback toasts |
| Skip plain reply | No playback: "Nothing is currently playing to skip"; else "Skipped current track" and auto-plays next | `src/telegram/dispatcher.cpp` (`handle_skip`) | PRESENT - Parlay skip reply and toast |
| Stop plain reply | "Playback stopped and queue cleared. Left voice chat." | `src/telegram/dispatcher.cpp` (`handle_stop`) | PRESENT - Parlay stop reply and stopped card |
| Volume plain replies | No arg: usage with example "/volume 120"; out of range: "Volume must be between 1% and 200%"; ok: "Volume set to N%"; non-numeric: "Invalid volume value" | `src/telegram/dispatcher.cpp` (`handle_volume`) | PRESENT - Parlay `/volume` handling and `/menu` Vol-plus/minus buttons; Parlay volume scale is 0-200 like anonx |
| Play errors | Empty query: "Please provide a song name, YouTube URL, or stream link. Example: /play Believer"; start failure: "Error starting audio stream pipeline" | `src/telegram/dispatcher.cpp:160-189` (`handle_play`) | PRESENT - Parlay no-query usage and play-failure texts |
| Play from reply / search / history | No search (the query is the title), no reply-to-media play, no play history | Absent from `src/telegram/dispatcher.cpp:160-197` | NA - nothing to port; Parlay has all three from other refs |
| Progress display | None: no progress bar, no position command | Absent from `src/telegram/dispatcher.cpp` | NA - nothing to port; Parlay has the Yukki progress bar and `/pos` |

## 4.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline keyboards | None anywhere | Absent from `src/telegram/dispatcher.cpp` and `include/anonx/telegram/dispatcher.hpp` | NA - nothing to port; Parlay has full inline keyboards from other refs |
| Reply keyboards | None anywhere | Absent from `src/telegram/dispatcher.cpp` | NA - nothing to port; Parlay's reply keyboard comes from beetube |
| Button callbacks | None: there is no callback-query handling at all | Absent from `src/telegram/dispatcher.cpp` | NA |

## 4.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Song/album thumbnails | None: no thumbnail field, no photo send, no cover fetch | Absent from `src/telegram/dispatcher.cpp` and `include/anonx/telegram/tdlib_client.hpp` send surface (`send_message` only, per Phase 9) | NA - nothing to port; Parlay thumbnails come from Yukki |
| Thumbnail fallback | None (no thumbnail feature to fall back from) | Absent from `src/telegram/dispatcher.cpp` | NA |

## 4.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Help/start combined text | `/start` and `/help` both show the same Markdown command list: play, pause, resume, skip, stop, queue, volume, ping, help - with one-line descriptions; no buttons, no separate welcome text | `src/telegram/dispatcher.cpp:305-319` (`handle_help`); dispatched at `:156` for both `help` and `start` | PRESENT in adapted form - Parlay has separate start and help cards; the combined flat list is superseded by the topic menu |
| Plain Markdown replies (whole surface) | Every message is plain Markdown: bold labels, code spans for ids/examples, no headings, tables, blockquotes, collapsibles or in-body buttons | Throughout `src/telegram/dispatcher.cpp:160-340` | PRESENT in adapted form - Parlay uses HTML; the plain-Markdown style itself is not ported (Phase 9 uses HTML everywhere) |
| Ping card | "Pong!" plus uptime (hours/minutes/seconds), engine line ("AnonX C++20 High-Performance Core"), thread-pool line (workers, completed, queued), rate-limiting line ("Active (Token-Bucket Striped)") | `src/telegram/dispatcher.cpp:282-303` (`handle_ping`) | PRESENT in adapted form - Parlay ping card has latency, uptime, RAM, CPU, disk and ntgcalls latency; the engine/thread-pool/rate-limit lines are MISSING (Parlay-specific internals differ) |
| Collapsibles / copy pills / hints / page footers | None: no collapsible element, no copy pill, no hint block, no footer on any list | Absent from `src/telegram/dispatcher.cpp` | NA - nothing to port |

## 4.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons | None: this ref has no rich cards and no inline buttons at all | Absent from `src/telegram/dispatcher.cpp` | NA |

## 4.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Silent drop: `execute_command` is an if/else-if chain over the nine known commands with no trailing else, so an unrecognised command produces no reply | `src/telegram/dispatcher.cpp:137-158` (full chain, verified) | MISSING by design - Parlay answers unknown commands explicitly (`dispatcher.cpp:1007-1012`) |
| Non-command messages | Silently ignored: messages not starting with "/" return before dispatch | `src/telegram/dispatcher.cpp:109-111` (empty or non-slash text returns) | PRESENT in part - Parlay also ignores plain non-command text in groups (it only acts on commands, links and share targets); the exact Parlay rule was not fully traced, so this is UNCERTAIN rather than PRESENT - see note in report |
| Rate-limit denial | Silent: over-limit commands are dropped with only a log line ("Rate limit exceeded for entity ..."), no user message; token bucket 3.0 refill, 6.0 burst, 64 striped buckets | `src/telegram/dispatcher.cpp:57` (limiter construction), `:113-118` (drop with log only) | PRESENT - Parlay rate-limit drops are silent (`dispatcher.cpp` `checkRateLimit` path, and the old 1 cmd/sec silent drop comment at `:1113`) |
| Thread-pool saturation denial | Silent: a saturated pool sheds the command with only a log line ("Backpressure triggered ... shedding command") | `src/telegram/dispatcher.cpp:130` (catch with log only) | UNCERTAIN - Parlay's dispatcher catches handler exceptions with a user-visible "Something went wrong" reply (`dispatcher.cpp:1000-1005`), so the silent-shed behaviour is not confirmed in Parlay |

---

# 5. beetube-bot - `refs/beetube-bot/` (Node.js, node-telegram-bot-api, MongoDB)

Provider-agnostic download bot (Music, Movie, Torrent, Anime) - not a
voice-chat player. Parse modes: Markdown for start/about/settings, HTML
for search/list results. Callback data is a JSON string with a `type`
discriminator. Description text truncates at 300 chars
(`config/index.js` `message.textLimit: 300`). Has both a persistent reply
keyboard and inline keyboards. No sudo, no admin panel, no playback.

## 5.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner management | None: no sudo, owner or admin command, gate or list anywhere; the only user record is activation state in Mongo | Absent from `bot.js`, `commands/*.js`, `handlers/*.js` (full command set is start, search, keyboard, about, settings) | NA - no equivalent to port; Parlay sudo/owner come from Tina/Yukki |
| Sudo denial behaviour | None (no gated command exists to deny) | Absent from `bot.js` and `commands/*.js` | NA |
| Settings access gate | None: `/settings` is open to every user; settings are per-user documents in Mongo | `commands/settings.js:9` (no gate before render) | MISSING as a gate - Parlay `/settings` is group-admin-gated; beetube's open settings are not reproduced |

## 5.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Voice-chat playback (now-playing, queue, controls, progress) | None: this bot never joins a voice chat and never plays anything; "play" does not exist as a concept here | Absent from `providers/*.js` and `commands/*.js` | NA - nothing to port; Parlay playback comes from Tina/Yukki/TgMusicBot |
| Music list and "Fetching latest songs" status | Transient status message "Fetching latest songs" (deleted afterwards), then one message per result | `providers/music.js` (list method; status text per Phase 9 section 3.7.1, function-level verified) | PRESENT in adapted form - Parlay download/search flows use fetching status edits; the music-list browser itself is MISSING (Parlay has no latest-songs list) |
| In-chat music download | Status "Preparing the music for download", then sends the audio with caption "Downloaded via [bot name] bot", title "{name} - {key}", filename "{name} - {key}.m4a", as a reply to the tapped message; only when the chat-music-download setting is on, size 200 MB or under, and extension in mp3/flac/wma/wav/ogg/aiff/alac | `providers/music.js` (download method); setting in `settings/music.js` | PRESENT - Parlay download flow with the same 200 MB chat-download limit (`ui_texts.hpp` `kSettingsMusicDlScreen`, `kDlSettingsTemplate` area) and format list |
| Download progress | None: only the two transient status lines above, no percentage, bar or live edit | Absent from `providers/*.js` | NA - nothing to port; Parlay download progress comes from Parvati/Yukki |
| Play history | None | Absent from `providers/*.js` and `models/*.js` | NA |

## 5.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Persistent reply keyboard | 7 buttons, resize enabled: row 1 Movie / Music / Torrent; row 2 Anime / Global Search; row 3 Settings / About; attached to /start, /about, /keyboard and most provider messages | `utils/bot-helper.js` (`exports.keyboard`); attached in `commands/start.js:8`, `commands/about.js`, `commands/keyboard.js` | PRESENT - Parlay `persistentKeyboard` (`ui_texts.hpp:2146`) with buttons Music / Global Search / Settings / About, and `/keyboard` with off/remove support (`dispatcher.cpp:6254-6261`); Parlay's keyboard has 4 buttons, not beetube's 7, and no Movie/Torrent/Anime buttons |
| `/keyboard` command | Reply "Keyboard enabled" with the reply keyboard attached | `commands/keyboard.js` | PRESENT - Parlay `/keyboard` replies "Keyboard enabled" (`ui_texts.hpp` `kKeyboardEnabled`) and supports `/keyboard off|remove|hide` |
| Global Search reply button (dead button) | The Global Search reply-keyboard button has no handler: tapping it falls through and nothing happens | `bot.js:18-23` (onText registrations - no Global Search handler); `utils/bot-helper.js` keyboard definition | PRESENT in adapted form - Parlay's Global Search button is wired (it opens search); beetube's dead-button behaviour is MISSING by design (a bug, not a feature) |
| Category picker (`/search`) | Text "Select a category to proceed" with a 2x2 inline keyboard: Movie, Music, Torrent, Anime; each callback is JSON `{"type":"find_<provider>"}` and starts that provider's interactive search | `commands/search.js` | MISSING - Parlay has no 4-category picker (Parlay is music-only); the music-search part is covered by Parlay `/search` |
| Settings index keyboard | Text "Settings" plus "Use this section to apply personalized custom tweaks to [bot]"; one row of two buttons: Pagination, Music Download (JSON `page_setting`, `music_setting`) | `commands/settings.js:9`; `utils/bot-helper.js` (`exports.settings`) | PRESENT - Parlay settings-index row has Pagination, Music Download and Quality buttons (`ui_texts.hpp:1717` `settingsIndexKeyboard`) |
| Pagination settings screen | Text "Auto delete old messages - By default previous messages are deleted during pagination to ease chat scrolling, use this section to change this behaviour"; toggle button "Enabled"/"Disabled" (JSON `page_setting_1` / `page_setting_0`, flips `purge_old_pages`); Back button (JSON `index_setting`) | `settings/page.js:5` (class `PageSettings`); toggle builder `toggleFeedbackText` in `utils/bot-helper.js` | PRESENT - Parlay pagination settings screen with the same text and toggle keyboard (`ui_texts.hpp` `kSettingsPageScreen`, `settingsToggleKeyboard`); the `purge_old_pages` store key itself is UNCERTAIN (Parlay's pagination cleanup is handled by its own message-cleanup settings, not verified under that key name) |
| Music-download settings screen | Text "Download music via chat" with a construction note, 200 MB limit note, and supported-format list (MP3, FLAC, WMA, WAV, OGG, AIFF, ALAC); same toggle + Back shape as pagination (JSON `music_setting_1` / `music_setting_0`, flips `chat_music_download`) | `settings/music.js:5` (class `MusicSettings`) | PRESENT - Parlay music-download settings screen with the same text, limit and format list (`ui_texts.hpp` `kSettingsMusicDlScreen`) |
| Genre picker (music list) | Text "Select a genre to proceed"; genre names as buttons, 3 per row; callback JSON `{"type":"ls_music","genre":"{name, max 18 chars}","page":1}` | `providers/music.js` (list method) | MISSING - Parlay has no genre picker |
| Result download buttons (music) | Per-result one-button row: URL button "Download ({size})" by default (or file over 200 MB / unsupported extension); callback button "Download ({size})" with JSON `{"type":"dl_music","info":"{sanitised name, max 16 chars}_{host}_{key}"}` only when chat-download is on, size fits and extension is supported | `providers/music.js` (result rendering) | PRESENT in adapted form - Parlay download buttons carry the same size-gated URL-vs-callback choice in its download flow; the exact JSON callback form is MISSING (Parlay uses `pl:dl:` callbacks, never JSON) |
| Result buttons (movie, anime, torrent) | Movie: single URL button "Download". Anime: "Watch" URL button plus "Share" URL button (Telegram share link with "Downloaded via @bot" text). Torrent: single URL button "Download ({size})" with magnet glyph; list view has no pagination | `providers/movie.js`, `providers/anime.js`, `providers/torrent.js` (result rendering) | MISSING - Parlay is music-only and has no movie/anime/torrent result buttons; the Share-URL pattern is PRESENT in Parlay's share flow (different feature) |
| Pagination row (last result message) | "Next" button, plus "Previous" prepended when page is over 1; list pagination JSON `page_ls_<provider>` and search pagination `page_find_<provider>` with page/genre/query (query truncated to 16 chars, torrent query to 14); old result messages deleted on pagination when `purge_old_pages` is on (default); pagination state in Mongo | `providers/music.js` (pagination row); `models/paginator.js` (state); keypad labels in `utils/bot-helper.js` (`exports.keypad`: Share, Download, Next, Previous) | PRESENT in adapted form - Parlay search pagination uses Previous/Next page buttons (`ui_texts.hpp:1336` `searchPageKeyboard`); beetube's per-result-message pagination with old-message purging is MISSING (Parlay paginates a single list message) |
| Callback router (JSON types) | Every callback is answered immediately with no text; type ending in movie/music/torrent/anime goes to that provider's resolve; type containing "setting" goes to the matching settings class; anything else sends "This feature is not available yet" to the chat | `handlers/callback-query.js` (full file, verified) | PRESENT in adapted form - Parlay callback router answers every query and routes by `pl:` prefix (`dispatcher.cpp:6584-6600`); the JSON `type` form itself is MISSING by design |
| Interactive search force-reply prompts | "Tell me the music title or artiste name" (music), "Tell me the title of the movie" (movie), "Tell me the title of the anime" (anime), "Tell me the torrent name or title" (torrent); search status "Searching for _{query}_"; validation "Input must be more than a character" for 1-char replies | `providers/music.js`, `providers/movie.js`, `providers/anime.js`, `providers/torrent.js` (interactiveSearch methods) | PRESENT in adapted form - Parlay `/search` no-query prompt uses the same force-reply pattern (Phase 9 section 7); the per-provider prompts are MISSING (music-only) |
| About keyboard attachment | About text (Markdown, channel link, GitHub link, issue link, star line, contribute line) sent with the reply keyboard attached | `commands/about.js` | PRESENT in adapted form - Parlay `/about` with the reply keyboard; beetube's own channel/GitHub links are not ported |

## 5.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Movie/anime banner link | Result text starts with a link on a glyph pointing at the banner image URL, followed by the bold name and an optional description block (truncated to 300 chars with "..." appended when over) | `providers/movie.js`, `providers/anime.js` (result rendering) | MISSING - Parlay has no movie/anime results; the banner-as-link pattern is not used in Parlay (Parlay sends real photos for music cards) |
| Music result thumbnails | None: music results are a disc glyph plus bold name, with no cover image | `providers/music.js` (result rendering) | NA - nothing to port |
| Torrent thumbnails | None: torrent results are name plus seeds/leeches counts plus description or magnet link in italics | `providers/torrent.js` (result rendering) | NA |
| Thumbnail fallback | None: no cover fetch, no default image, no fallback branch | Absent from `providers/*.js` | NA |

## 5.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start message | Markdown: greeting with username link, bot description (music, movies, anime, torrent, EDM, series, files), channel link line; reply keyboard attached; typing chat action first; new/blocked users reactivated in DB with no visible text | `commands/start.js:8` | PRESENT in adapted form - Parlay start message with reply keyboard; Parlay's text is the music-bot start card, not beetube's multi-provider pitch |
| About message | Markdown about text with channel, GitHub and issue links, star line and contribute line, reply keyboard attached | `commands/about.js` | PRESENT in adapted form - Parlay `/about` |
| Settings message | Markdown "Settings" plus "Use this section to apply personalized custom tweaks to [bot]" with the settings-index keyboard | `commands/settings.js:9` | PRESENT - Parlay settings panel (superset) |
| Result messages (HTML) | Music: disc glyph + bold name. Movie/anime: banner link + bold name + optional description block. Torrent: globe glyph + bold name, seeds/leeches line, description or magnet link | `providers/music.js`, `providers/movie.js`, `providers/anime.js`, `providers/torrent.js` | MISSING - Parlay has no beetube result messages (different domain); Parlay's own result messages are the music search list |
| Transient status messages | "Fetching latest songs/movies/anime/torrents", "Searching for _{query}_", "Preparing the music for download" - sent, then deleted after results arrive | `providers/*.js` (status sends) | PRESENT - Parlay fetching/searching/downloading status edits |
| Collapsibles / copy pills / hints / page footers | None: no collapsible element, no copy pill, no hint block, no page footer; pagination state lives in Mongo, not in the message | Absent from `providers/*.js` and `commands/*.js` | NA - nothing to port |
| Error texts | Generic "An error occurred, try again later" (caught handler errors); "The list is empty or limit reached" (empty list); "No results found" (empty search); "Input must be more than a character" (short search reply); "This feature is not available yet" (unknown callback type) | `utils/error-handler.js`; `providers/index.js` (`emptyAPIResponse`); `handlers/callback-query.js` | PRESENT in adapted form - Parlay has matching generic/empty/no-results texts; the unknown-callback text differs (Parlay uses an alert toast) |

## 5.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons (general pattern) | Category picker, settings screens, genre picker, result rows and pagination rows are all plain-text messages carrying real inline keyboards (JSON callbacks) | `commands/search.js`, `commands/settings.js`, `settings/page.js`, `settings/music.js`, `providers/*.js` | PRESENT - Parlay uses the same pattern (text + inline keyboard) with the `pl:` callback namespace |

## 5.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command / unmatched text | Silent fall-through: `bot.js` registers only onText patterns for start, about, search, settings, keyboard and provider names, plus a callback handler; any other text - including `/help`, which has no handler at all - matches nothing and produces no reply | `bot.js:18-23` (complete onText list, verified); no `/help` registration anywhere in `commands/*.js` | MISSING by design - Parlay answers unknown commands explicitly (`dispatcher.cpp:1007-1012`); Parlay's `/help` exists |
| Dead Global Search button | Tapping Global Search on the reply keyboard does nothing (no handler), silently | `bot.js:18-23` (no Global Search handler) | MISSING by design - Parlay wires its Global Search button to search |
| Unknown callback type | Not silent: sends "This feature is not available yet" to the chat | `handlers/callback-query.js` (else branch, verified) | MISSING as a message - Parlay answers unknown callbacks with an alert toast instead of a chat message |
| Generic handler error | Not silent: sends "An error occurred, try again later" | `utils/error-handler.js` | PRESENT - Parlay handler-exception reply "Something went wrong handling that command" (`dispatcher.cpp:1000-1005`) |

---

# 6. feel-me - `refs/feel-me/src/` (Rust, teloxide, anonymous music sharing)

Not a voice-chat player: users send a song to another user through that
user's unique link. All user strings live in `src/strings.rs` (66 lines)
and are used verbatim; grep confirms no parse mode is set anywhere, so
every message is plain text. Every message that carries buttons uses the
same 2-button row (Help, My Link) unless noted. Only files: `lib.rs`,
`main.rs`, `models.rs`, `schema.rs`, `strings.rs`.

## 6.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner management | None: no sudo, owner or admin concept; the only access control is the per-user blocklist (see below) | Absent from `src/lib.rs`, `src/main.rs`, `src/strings.rs` | NA - no equivalent to port; Parlay sudo/owner come from Tina/Yukki |
| Blocklist (user-level) | A reported user is blocked from sending to (and receiving feedback from) the reporter; blocked-send reply: "You can't send music to {name} / You are blocked"; blocked-feedback reply: "You can't send Feedback / You are blocked"; already-reported: "User already is reported"; report success: "User is reported and blocked" | `src/strings.rs` (`BLOCKED_TEXT1`, `BLOCKED_TEXT2`, `FEEDBACK_BLOCKED`, `REPORTED_BEFORE`, `USER_REPORTED_BLOCKED`); handling `src/main.rs:243-358` (`feedback_*`, `report_*`, `unreport_*` branches) | PRESENT in adapted form - Parlay has user/chat blocklists (`/blacklist`, sudo-gated); feel-me's per-pair report-to-block flow is MISSING (Parlay blocks globally, not per reporter pair) |
| Sudo denial behaviour | None (no gated command exists) | Absent from `src/lib.rs` | NA |

## 6.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Voice-chat playback (now-playing, queue, controls, progress) | None: no playback, no queue, no now-playing, no voice chat at all | Absent from `src/main.rs` and `src/lib.rs` | NA - nothing to port; Parlay playback comes from Tina/Yukki/TgMusicBot |
| Anonymous song send (the core "play" equivalent) | Sender with a pending target sends an audio file; the audio is forwarded to the target by file_id with a Feedback / Report User button row; sender then gets "Music Has Been send" (sic) | `src/main.rs:77-135` (audio branch); `src/strings.rs` (`HAS_BEEN_SENT`) | PRESENT in adapted form - Parlay share flow forwards media to a pending share target (`dispatcher.cpp:576-613`, `shareReceivedKeyboard` in `ui_texts.hpp:2464`); feel-me's anonymous-link targeting is the share-link pattern, not a VC play |
| Send prompt with Cancel | After a valid deep link, the sender gets "Please Send the music that you want to share with {name}" with a single-button Cancel row; Redis stores the sender-to-target mapping until the audio arrives or Cancel is pressed | `src/lib.rs:219-284` (`handle_text`, deep-link branch); cancel handling `src/main.rs:212` | PRESENT in adapted form - Parlay share-target pending state with cancel behaviour; the exact Cancel-button row is UNCERTAIN in Parlay (share cancel was not verified as a button) |
| Feedback send | If the sender sends text while a feedback is pending, the text is forwarded as a reply to the original audio message in the target chat, and the sender gets "Feedback is sent" | `src/main.rs:145-165` (feedback-pending text branch); `src/strings.rs` (`FEEDBACK_IS_SENT`) | UNCERTAIN - Parlay share flow handles captioned/uncaptioned media to a target (`dispatcher.cpp:710-718`); a text-feedback-to-original-audio flow was not verified |
| Play history | None: history rows exist in the database (history type constants LOG, FEEDBACK, REPORT, UNREPORT) but no user-facing history view | `src/strings.rs` (LOG/FEEDBACK/REPORT/UNREPORT constants); `src/lib.rs:124` (`set_history`) | NA - no user-facing equivalent to port; Parlay also has no play-history view |

## 6.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Standard row: Help + My Link | Two-button row attached to help text, cancel confirmation, link replies, unknown-callback fallbacks and error fallbacks: Help (callback `help`), My Link (callback `get_link`) | `src/main.rs:166-398` (`q_callback`, final fallback block at `:359-398` builds exactly this row); `src/lib.rs` link handling | PRESENT in adapted form - Parlay share flow has a My-Link button (`pl:share:mylink`, `ui_texts.hpp:2446`); the universal Help + My Link row on every message is MISSING (Parlay attaches help buttons only to start/help cards) |
| My Link (get_link) | Replies with the user's own link `https://t.me/FeelMeBot?start={nickname}` plus the standard row | `src/lib.rs` (link built from `BOT_URL` in `src/strings.rs`); callback `get_link` in `src/main.rs:166-398` | PRESENT - Parlay `pl:share:mylink` (`ui_texts.hpp:2446`) |
| Help button | Replies with the full help text plus the standard row | `src/main.rs:166-398` (`help` branch) | PRESENT - Parlay help menu (richer topic menu, not the feel-me help text) |
| Cancel button (send prompt) | Single-button row on the send prompt; pressing it sends "Cancel sendig music" (sic) with the standard row | `src/strings.rs` (`CANCEL_SENDING`); `src/main.rs:212` (`cancel` branch) | UNCERTAIN - Parlay share cancel as a button was not verified (see Plays) |
| Feedback + Report User row (received audio) | On the forwarded audio: Feedback (callback `feedback_{sender_chat_id}_{update_id}_{audio_file_unique_id}`) and Report User (callback `report_{sender_chat_id}_{update_id}`) | `src/main.rs:77-135` (audio forward with keyboard) | PRESENT in adapted form - Parlay `shareReceivedKeyboard` (sender-named row on received share, `ui_texts.hpp:2464`); the Feedback/Report pair itself is MISSING (Parlay share has no feedback/report buttons) |
| Feedback prompt and guards | Feedback tap: already-sent gives "Feedback is sent before"; blocked gives "You can't send Feedback / You are blocked"; otherwise prompt "Send Feedback Text" | `src/strings.rs` (`FEEDBACK_SENT`, `FEEDBACK_BLOCKED`, `FEEDBACK_SEND`); `src/main.rs:243-293` | MISSING - Parlay has no feedback flow |
| Report / unreport handling | Report tap: already-reported gives "User already is reported", else blocks and replies "User is reported and blocked"; unreport callbacks (`unreport_*`) are handled in the same switch | `src/main.rs:326-358` (`report_*` branch), `:359` (unreport fallthrough to help text) | MISSING as a user flow - Parlay has global blocklist commands, not per-pair report buttons |
| Reply keyboards | None: only the inline rows above | Absent from `src/main.rs` and `src/lib.rs` | NA - nothing to port; Parlay's reply keyboard comes from beetube |

## 6.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Song/album thumbnails | None: audio is forwarded by file_id with no cover art, no photo, no thumbnail fetch | `src/main.rs:77-135` (send_audio by file_id, no thumb argument) | NA - nothing to port |
| Thumbnail fallback | None (no thumbnail feature to fall back from) | Absent from `src/main.rs` | NA |

## 6.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Help text (start and help identical) | Plain text with leading spaces and the typo "Desctiption": how to send music via a user's unique link, how to get your own link via the Get Link button, command list for /start and /help | `src/strings.rs` (`HELPTEXT`); served by `src/lib.rs:219-284` (`handle_text`) for both `/start` with no argument and `/help` | PRESENT in adapted form - Parlay start/help cards explain the share flow; feel-me's exact plain text and typos are not ported |
| Deep-link start (`/start <nickname>`) | Self-target: "Dude?! You have schizophrenia / Why you send music to yourself". Blocked: "You can't send music to {name} / You are blocked". Unknown nickname: "Oops! / Invalid link". OK: "Please Send the music that you want to share with {name}" plus Cancel row. DB/Redis failure: "Something went wrong on server side / Please try again" | `src/lib.rs:219-284` (deep-link branch); `src/strings.rs` (`SEND_MSG_TO_SELF`, `BLOCKED_TEXT1/2`, `INVALID_LINK`, `SEND_MUSIC_TEXT`, `WRONG`) | PRESENT in adapted form - Parlay share deep links with self-target, blocked and invalid-target handling (Phase 9 section 22); feel-me's exact jokes/wording are not ported |
| Send confirmation and target-missing name | Sender confirmation "Music Has Been send" (sic); when the target user row is missing from the DB, the target name renders as "this user" | `src/strings.rs` (`HAS_BEEN_SENT`); `src/main.rs:77-135` (name fallback) | PRESENT in adapted form - Parlay share delivery confirmation; the "this user" fallback is UNCERTAIN |
| Unknown-target prompt | "The target is unknown / Click on target user's link and then send the music" | `src/strings.rs` (`UNKNOWN_TARGET`) | PRESENT in adapted form - Parlay share-target resolution errors |
| Error strings (server side) | "Something went wrong on server side / Please try again" (DB errors everywhere); "Erro on getting Users" (sic); "Error on creating new user"; "Error while storing history"; "Error on set to blacklist"; "Error on searching for blacklist"; "Error on connecting to" (DB connect, truncated in source) | `src/strings.rs` (all `*_ERR` / `WRONG` constants); used in `src/main.rs` at lines 89, 105, 183, 192, 204, 218, 225, 251, 260, 272, 287, 297, 304, 316, 323, 336 (per Phase 9, spot-checked against the file) | PRESENT in adapted form - Parlay generic error replies; feel-me's exact strings and typos are not ported |
| Collapsibles / copy pills / hints / page footers | None: plain text only, no parse mode, no formatting elements at all | Grep for parse mode across `src/` returns nothing; `src/strings.rs` is plain text throughout | NA - nothing to port |
| Per-screen layouts | Only two layouts exist: plain text with the standard Help/My Link row, and plain text with the single Cancel row (send prompt) | `src/main.rs:166-398`; `src/lib.rs:219-284` | PRESENT in adapted form - Parlay share screens use the same two-shape pattern (info + action row) |

## 6.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Plain-text messages with inline buttons (general pattern) | Every buttoned message in feel-me is plain text plus a real inline keyboard (standard row, Cancel row, Feedback/Report row) - no rich formatting at all | `src/main.rs:166-398`; `src/lib.rs:219-284` | PRESENT as a pattern - Parlay share messages pair text with inline keyboards; Parlay's are HTML, feel-me's are plain text |

## 6.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Not silent: any `/anything-else` replies "Invalid Command" | `src/strings.rs` (`INVALID_CMD`); `src/lib.rs:282` (unknown-command branch in `handle_text`) | MISSING as an exact behaviour - Parlay also replies to unknown commands, but with its own unknown-command text plus the available-command list (`dispatcher.cpp:1007-1012`), not a bare "Invalid Command"; the "reply, don't drop" principle is PRESENT |
| Non-command text with no pending target | Not silent: replies "Invalid Message" (plain, no buttons) | `src/strings.rs` (`INVALID_MSG`); `src/main.rs:78` | MISSING as an exact behaviour - Parlay ignores plain non-command text in groups rather than replying "Invalid Message"; in Parlay's share flow, unmatched text goes to the share/search handling, not to an invalid-message reply |
| Malformed pending-feedback text | Not silent: replies "Invalid message format" | `src/strings.rs` (`INVALID_MSG_FORMAT`); `src/main.rs:149` | UNCERTAIN - no equivalent malformed-feedback state was verified in Parlay's share flow |
| Unknown callback data | Not silent: any callback data that matches none of cancel/help/get_link/feedback_*/report_*/unreport_* falls through to the help text with the standard row | `src/main.rs:359-398` (final else builds help text + Help/My Link row, verified) | MISSING as an exact behaviour - Parlay answers unknown callbacks with an alert toast (`dispatcher.cpp:6587-6598`), not with help text |
| Blocked / self / invalid-link denials | All explicit replies (see Rich), never silent drops | `src/lib.rs:219-284` | PRESENT in principle - Parlay share denials are explicit replies, not silent drops |

---

# 7. lemonic-telegram-bot - `refs/lemonic-telegram-bot/` (TypeScript, grammy, HTML parse mode globally)

Spotify downloader via the Lemonic API. Global parse mode is HTML
(`bot.ts` `bot.api.config.use(parseMode("HTML"))`). Works in private and
group chats. Commands registered in code: start, help, settings (all three
route to the start handler - `/help` and `/settings` show the same start
text), search. Plain-text `search ...` is also matched in private chats.
Plain text is Spotify search by name; any URL entity is link handling. A
forwarded message from @BotFather containing a bot token in a code entity
triggers the clone flow.

## 7.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner management | None: no sudo, owner or admin command, gate or list anywhere | Absent from `bot.ts` (complete composer: start/help/settings, search, hears, url, botfather-forward, text, inline, callback, chosen-inline) | NA - no equivalent to port; Parlay sudo/owner come from Tina/Yukki |
| Sudo denial behaviour | None (no gated command exists) | Absent from `bot.ts` | NA |
| Clone-bot gate (token handling) | Clone flow is triggered only by a private message forwarded from @BotFather containing a token in a code entity; other forwards are ignored by the filter | `bot.ts` (botfather-forward filter on `privateChat.on("message:text")`); handler `handlers/bot_token_handler.ts` | MISSING - Parlay has no bot-cloning feature (Phase 9 section 2 marks it not-applicable) |

## 7.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Voice-chat playback (now-playing, queue, controls, progress) | None: this bot downloads Spotify tracks and sends audio files; it never joins a voice chat | Absent from `handlers/*.ts` and `modules/*.ts` | NA - nothing to port; Parlay playback comes from Tina/Yukki/TgMusicBot |
| Song delivery (audio send) | Sends the audio directly via reply-with-audio with no caption, no title/performer metadata on the first attempt, no progress message | `modules/methods/replyWithSong.ts` | PRESENT in adapted form - Parlay download delivery sends audio with a caption (the Music163bot caption rule); lemonic's bare no-caption first attempt is MISSING (Parlay always captions) |
| Song delivery fallback | Only when Telegram rejects the URL ("wrong file identifier/HTTP URL specified" or "failed to get HTTP URL content"): sends/edits a "Please wait..." message, then re-sends as an uploaded file with title, performer (artists joined, or show name for episodes) and duration; the "Please wait..." message is deleted afterwards | `modules/methods/replyWithSong.ts` (fallback path) | UNCERTAIN - Parlay downloaders retry and re-upload on URL failure, but this exact fallback sequence (bare URL first, metadata upload second, status deleted) was not verified in Parlay's download path |
| Track not available flow | Message: bold track name + artists, then "This track will be added within 5 minutes"; single button "Check Again" (callback `spotify----{id}----track`); tapping while still unavailable answers a non-alert toast "Track is still being processed. Please try again later" (cache 10s) | `modules/methods/replyWithSongNotAvailable.ts`; constants `TRACK_NOT_AVAILABLE_MESSAGE`, `TRACK_NOT_AVAILABLE_TEXT` in `constants.ts` | UNCERTAIN - Parlay has not-available and retry handling in downloads, but this exact Check-Again button flow was not verified |
| Album and playlist views | Photo (cover art, see Thumbnails) with HTML caption and one "Download N songs" button; after a bulk download completes, a plain reply "N songs album/playlist downloaded"; individual tracks arrive as separate audios, silent (no notification) | `modules/methods/spotifyHandlers.ts` (album handler, playlist handler); captions `albumMessage`, `playlistMessage` in `modules/methods/utils.ts` | UNCERTAIN - Parlay has album/playlist download flows, but a photo card plus "Download N songs" button plus silent per-track delivery was not verified as one flow in the dispatcher |
| Bulk download completion | Plain reply "{album name} album downloaded" / "{playlist name} playlist downloaded" | `modules/methods/spotifyHandlers.ts` | PRESENT in adapted form - Parlay album/track completion reports exist (navidrome-style reports); exact wording differs |
| Play history | None | Absent from `handlers/*.ts` and `modules/*.ts` | NA |

## 7.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | One row, two buttons: "Clone Bot" (URL to a telegra.ph clone guide) and "Search for a song" (switch-inline-query-current-chat with an empty query, opens inline mode in the current chat) | `handlers/start_command.ts:8-30` (handler; buttons at `:29-30`) | PRESENT in part - Parlay supports switch-inline-query buttons (`tg_types.hpp:61-84`, `dispatcher.cpp:8630-8643`) and start-card buttons; the Clone Bot button is MISSING (no cloning feature) |
| Search result number buttons | Number buttons `1` to `n` (max 7 per page; 6 when paginating from offset over 0), 4 per row; callback `spotify----{id}----{track|album|playlist}` | `modules/methods/createSearchResults.ts:85` (number buttons), `:12` (builder) | PRESENT - Parlay search number buttons (`ui_texts.hpp:1320` `searchKeyboard`), 5 per row instead of lemonic's 4, with `pl:search:` callbacks instead of `spotify----` |
| Search pagination row | Appended after the numbers: Previous button (only when offset over 0) and Next button (only when more pages exist); callback `n----{query}----{t|a|p}-{offset}`; pagination edits the message in place | `modules/methods/createSearchResults.ts:101-110` (pagination buttons) | PRESENT - Parlay Previous/Next page buttons (`ui_texts.hpp:1336` `searchPageKeyboard`) with hash-based page callbacks |
| Check Again button | Single button on the not-available card (see Plays); callback `spotify----{id}----track` | `modules/methods/replyWithSongNotAvailable.ts:41` | UNCERTAIN - see Plays (not-available flow) |
| Album / playlist Download buttons | "Download N songs" button on album and playlist photo cards; callbacks `spotify----{album id}----album-dl` and `spotify----{playlist id}----playlist-dl` | `modules/methods/spotifyHandlers.ts:42` (album-dl), `:117` (playlist-dl) | UNCERTAIN - Parlay album/playlist download exists, but this exact photo-card button was not verified |
| Inline result button | Inline results carry a button "Open in Lemonic Bot" linking to `https://t.me/{bot}?start=spotify----{track id}` | `handlers/inline_query.ts:6` (handler; button in result builder) | PRESENT in adapted form - Parlay inline results carry bot deep links (Phase 9 section 13); the `spotify----` deep-link form is MISSING (Parlay uses its own link forms) |
| Callback router | Callback data format `{provider}----{id}----{suffix}`, split on `----` (max 3 parts): spotify track/album/album-dl/playlist/playlist-dl actions, `n` search pagination, anything else answers an alert toast "Invalid provider: {suffix or provider}" (cache 10s) | `handlers/callback_query.ts` (full file, verified) | PRESENT in adapted form - Parlay callback router splits `pl:` callbacks and answers unknown ones with an alert toast; the `----` format and "Invalid provider" text are MISSING |
| Reply keyboards | None | Absent from `handlers/*.ts` and `bot.ts` | NA - nothing to port; Parlay's reply keyboard comes from beetube |

## 7.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Album / playlist cover photo | Photo sent with the album/playlist caption; cover art resized through an image proxy to 1137x640, contain fit, dark background | `modules/methods/spotifyHandlers.ts` (photo send); proxy transform `create3x4Image` in `modules/methods/utils.ts` | PRESENT in adapted form - Parlay sends photo cards with cover art for now-playing/queue/album contexts; the proxy-resize step (1137x640 via wsrv.nl) is MISSING (Parlay uses the source thumbnail URL directly) |
| Inline result thumbnail | Each inline document result carries the album image URL as thumbnail, with its height and width | `handlers/inline_query.ts:6` (result builder) | PRESENT - Parlay inline results carry thumbnails (search.inline IPC, `contracts/ipc.json:488`) |
| Song audio thumbnail | None on the first (bare URL) send; metadata upload fallback carries title/performer/duration but no cover argument is set in the observed code | `modules/methods/replyWithSong.ts` | UNCERTAIN - Parlay audio delivery captions include the track, but a cover thumbnail on the audio file itself was not verified |
| Thumbnail fallback | No fallback artwork: unresolvable links edit the "Please wait..." message to "No result found"; unavailable tracks use the not-available card, not a placeholder image | `handlers/url_message.ts` (unresolvable branch); `modules/methods/replyWithSongNotAvailable.ts` | PRESENT in adapted form - Parlay falls back to text cards and not-found texts under the same conditions |

## 7.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start text (also /help and /settings) | Plain sentence: "Hello, I'm Lemonic Bot. I can download songs from Spotify and other music platforms. Just send me a link or search for a song." - identical for /start, /help and /settings | `handlers/start_command.ts:8`; registered for all three in `bot.ts:38` (`allChats.command(["start", "help", "settings"], ...)`) | PRESENT in adapted form - Parlay has distinct start, help and settings screens (richer); lemonic's identical-text shortcut is MISSING by design |
| Deep-link start (`/start spotify----{id}`) | Downloads that Spotify track immediately with no start text shown | `handlers/start_command.ts:8` (deep-link branch) | PRESENT in adapted form - Parlay deep links resolve to downloads/shares; the `spotify----` form is MISSING |
| Search help text | "You can search for tracks, albums and playlists." plus track/album/playlist examples in `<pre>` blocks, plus "The search query is case-insensitive so you don't have to worry about captial and small letters. You can also search for tracks in inline mode." (sic: "captial"); query truncated to 50 chars (command) / 100 chars (plain text) | `constants.ts` (`SEARCH_HELP_TEXT`); handler `handlers/search_command.ts` | PRESENT - Parlay search help with the same examples and case-insensitivity note, typo corrected (Phase 9 section 7) |
| Search results message | "Search results for {track|album|playlist}: {query}" in bold, blank line, then numbered lines "N. {name} - {artists}" (tracks/albums) or "N. {name} - {owner}" (playlists); no-results text "No result found for {track|album|playlist} {query}" with an empty keyboard; plain-text search with zero results replies "No result found" | `modules/methods/createSearchResults.ts:12` (`searchResultMessage`); `handlers/text_message.ts` (plain-text zero-result reply) | PRESENT - Parlay search results list with header, numbered lines and no-results text; Parlay adds duration/artist metadata lines from the TGVCBot/remix pattern |
| Album caption | Bold linked album name, italic comma-separated artists, blank line, label | `modules/methods/utils.ts` (`albumMessage`) | UNCERTAIN - Parlay album captions exist in download flows, but this exact three-line caption was not verified |
| Playlist caption | Bold linked playlist name, italic owner display name, blank line, description | `modules/methods/utils.ts` (`playlistMessage`) | UNCERTAIN - same as album caption |
| Inline results (document type) | Any non-empty inline query searches Spotify tracks (market IN, 10 per page, next offset +10, cache 3600s); each result is a document-type inline result: title = track name, description = artists, thumbnail = album image, caption = bold track name + artists, button = Open-in-bot deep link; empty query returns no results | `handlers/inline_query.ts:6` | PRESENT - Parlay inline search (`search.inline` IPC, `contracts/ipc.json:488`; inline answer path `dispatcher.cpp:9035-9064`); Parlay result types and paging follow its own inline design, not lemonic's document-type form |
| Chosen-inline-result upgrade | When the user picks an inline result, if the API has the file, the inline message media is replaced with the actual audio; MEDIA_EMPTY and failed-to-get-URL errors are silently ignored | `handlers/chosen_inline_result.ts` | UNCERTAIN - Parlay inline chosen-result handling was not verified by grep in the dispatcher |
| URL handling messages | While resolving an unknown URL: "Please wait..."; if unresolvable, that message is edited to "No result found"; Spotify links handled directly, other music-platform links resolved to Spotify via SongLink silently | `handlers/url_message.ts`; `modules/songlink.ts` | PRESENT - Parlay link handling with Please-wait / No-result-found states and cross-platform resolution |
| Clone flow messages | No webhook configured: "I am a bot too!". Success: "Bot cloned successfully". Webhook failure: "An error has occurred when setting webhook: {error} / Please contact the bot developer or set the webhook manually to {WEBHOOK_URL}/bot{token}". Already cloned: "Bot is already cloned". No token in code entity: "Invalid message from @BotFather" | `handlers/bot_token_handler.ts`; constants in `constants.ts` (`CLONE_BOT_BUTTON` family) | MISSING - Parlay has no bot-cloning feature |
| Error messages | Spotify error with no parseable message: "Something went wrong" (shown as message edit, callback alert with cache 30s, or plain reply; a parseable error JSON message is shown instead). Any uncaught handler error: "An error has occurred. Please try again later. If this issue persists, please contact the bot developer." (not sent for chosen-inline-result errors) | `modules/errors.ts` (`handleSpotifyError`); `bot.ts` (wrapper error reply) | PRESENT in adapted form - Parlay generic error replies; lemonic's exact texts are not ported |
| Collapsibles / copy pills / page footers | None: no collapsible element, no copy pill, no hint block, no page footer; pagination is a button row, not a footer | Absent from `handlers/*.ts` and `modules/methods/*.ts` | NA - nothing to port |

## 7.6 Rich inline buttons (not this ref's category - recorded under Buttons/Rich)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Rich cards with inline buttons (general pattern) | Start card, search results, not-available card and album/playlist photo cards are all HTML texts carrying real inline keyboards; inline results additionally carry a URL button | `handlers/start_command.ts`, `modules/methods/createSearchResults.ts`, `modules/methods/replyWithSongNotAvailable.ts`, `modules/methods/spotifyHandlers.ts`, `handlers/inline_query.ts` | PRESENT - Parlay renders the same pattern (HTML text or photo caption plus inline keyboard) |

## 7.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command handling | Not cleanly silent and not explicitly handled: grammy `command()` only matches start/help/settings/search, and there is no unknown-command handler; in a private chat an unmatched `/command` falls through to the private-chat text handler, which treats the whole text (including the slash) as a Spotify search query; in a group there is no text handler, so it is silently ignored | `bot.ts:38-47` (composer order: command handlers, then `privateChat.hears` search, then `privateChat.on("message:text")` text handler); `handlers/text_message.ts` (no slash filter observed) | UNCERTAIN - the grammy fall-through ordering was read from code, not runtime-verified; Parlay answers unknown commands explicitly (`dispatcher.cpp:1007-1012`), so neither lemonic branch matches Parlay |
| Unknown provider callback | Not silent: alert toast "Invalid provider: {suffix or provider}" with cache 10s | `handlers/callback_query.ts` (else branch, verified) | PRESENT in adapted form - Parlay answers unknown callbacks with an alert toast (different text) |
| Unknown Spotify id | Not silent: message/edit text "Invalid Spotify ID" | `constants.ts` (`INVALID_SPOTIFY_ID_TEXT`); used in `modules/methods/spotifyHandlers.ts` / not-available flow | PRESENT in adapted form - Parlay invalid-link/id replies |
| Chosen-inline-result errors | Silent: MEDIA_EMPTY and failed-to-get-URL errors on the chosen-result upgrade are ignored with no user message | `handlers/chosen_inline_result.ts` (silently ignored errors, per Phase 9) | UNCERTAIN - Parlay's chosen-inline error path was not verified |
| Uncaught handler errors | Not silent (except chosen-inline): plain reply with the "An error has occurred..." text | `bot.ts` (wrapper) | PRESENT - Parlay handler-exception reply |

---

# Notes and verification gaps

- Web-search rule: no web search was needed. Every component suspected
  from the Phase 9 starting map was found in the local clone, except the
  two Phase 9 gaps it had already flagged for TgMusicBot (`/privacy` and
  `/av` handler texts): both were resolved locally in this audit -
  `/privacy` is `internal/bot/admins.go:122` (`privacyHandler`) and `/av`
  is `internal/bot/devs.go:24` (`activeVcHandler`), so neither required a
  web search either. No component was declared missing from a local miss
  alone: every MISSING verdict above names a Parlay-side absence (checked
  by grep in the Parlay tree), not a ref-side absence, except the
  explicitly marked NA rows, which were confirmed by grepping the ref's
  complete command/handler registrations.
- Line numbers for TinaMusic dispatch entries are the `else if` chain in
  `src/bot_engine.cpp:434-566`; handler function line numbers for Tina
  (`HandleQueue` 1193, `HandleSettings` 1299, `HandleAddSudo` 1379, and so
  on) were re-verified by grep. YukkiMusic-Go callback registrations are
  `internal/modules/handlers.go:116-147`, command registrations `:36-115`.
  TgMusicBot button builders are `internal/utils/buttons.go:54-225`.
  beetube-bot, feel-me and lemonic-bot line numbers are file-level plus
  the function/class lines shown; their small trees were read in full
  (beetube `bot.js` onText list, feel-me `strings.rs` 66 lines, lemonic
  `bot.ts` composer) so function-level locations are exact even where a
  single line number is not quoted.
- Parlay verdicts rest on grep, not on running Parlay: a PRESENT verdict
  means the equivalent code path was found (file + function/route cited);
  it does not mean the feature was live-tested. UNCERTAIN rows name the
  exact reason (partial equivalent, untranslated rich presentation, or a
  Parlay path not confirmed by grep).
- Two Parlay-side ambiguities worth a follow-up grep by the port worker:
  (1) anonx "non-command messages silently ignored" is marked UNCERTAIN
  because Parlay's plain-text handling (share targets, link handling,
  search) was not fully traced in this audit; (2) lemonic "unknown
  command falls through to the private-chat text handler" rests on
  grammy middleware ordering read from `bot.ts`, not on a runtime test.
- No code was modified, no branch was changed (still
  `feat/ui-complete-all-refs`), nothing was pushed, and nothing under
  `refs/` or any `.env`/secret file was touched.
