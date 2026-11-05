# Parlay Phase 10 — UI Inventory, Group A (7 refs)

**Scope:** fresh audit (2026-11-05) of every Telegram UI component in 7 categories
(Sudos, Plays, Buttons, Thumbnails, Rich messages, Rich inline buttons,
Silent denials for unknown commands) for 7 refs under `~/workspace/parlay/refs/`:
ArcMusic, Music163bot-Go, NoxBeats, ParvatiMusic, Spotify-Loader, TGVCBot,
Telegram-Song-Downloader-Bot.
**Method:** read-only. No code was modified, no branch changed, no push made.
Parlay branch at audit time: `feat/ui-complete-all-refs`.
**Starting maps read first, then verified against the actual trees:**
`phase9/inventory-group-a.md`, `UI_INVENTORY.md`, `PHASE9.md`. Where the old
maps and the tree disagreed, the tree wins and the disagreement is noted.
**Verdict basis (Parlay):** greps of `services/core-cpp/src/dispatcher.cpp`,
`services/core-cpp/src/ui_texts.hpp`, `services/core-cpp/src/dispatcher.hpp`,
`services/backend-go/internal/gui/gui.go`,
`services/backend-go/internal/handlers/`, `services/backend-go/internal/settings/`,
`services/backend-go/internal/room/`, `services/backend-go/internal/voice/`,
`contracts/routing-table.md`, `contracts/ipc.json`, `services/core-cpp/`.
**Verdicts:** PRESENT = Parlay has the component (same or merged equivalent;
notes say when the form differs). MISSING = Parlay lacks it. UNCERTAIN = could
not be settled from the greps named in the row. N/A = the ref itself has no
such component (reason given); N/A rows are not counted as present/missing.
**Style note:** ref button labels are paraphrased in normal English here.
Several refs render labels in small-caps Unicode; that styling is not copied.
Glyph labels are described in words, never pasted as emoji, per the no-emoji
rule for this file. Callback data strings are quoted exactly (ASCII).
**Web-search rule compliance:** no component was declared missing from a
local miss alone. Every "missing in ref" claim below rests on an exhaustive
local check: the ref's command/callback router tables (which enumerate every
handled command and callback) plus targeted greps. No web search was needed
for any ref, and for NoxBeats the local clone was treated as authoritative
as instructed (its rich inline button system is documented from
`Bad/ui/buttons.cpp`, `Bad/core/types.hpp`, `Bad/tg/botapi_client.cpp`,
`Bad/cmd/play_cb.cpp`, `Bad/cmd/settings.cpp`).
**Counts:** see the Summary table at the end, computed from the tables below.

---

## 1. ArcMusic — `refs/ArcMusic/` (Go, gogram/MTProto client)

Router: `internal/modules/handlers.go` (pattern table; filters in
`internal/modules/filters.go`). Strings: `internal/locales/en.yml`.
Keyboards: `internal/core/buttons.go`. No settings panel exists in this ref
(commands only); no reply keyboards exist (all keyboards are inline).

### 1.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Add sudo (`/addsudo`, aliases `addsudoer`, `sudoadd`) | Owner-only; adds a user by reply/username/id; refuses owner-self, bot-self, assistant-self, existing sudo, bot accounts, with a distinct reply per case | `internal/modules/handlers.go:57-60`, `internal/modules/sudoers.go:25-137` | PRESENT — `dispatcher.cpp:470` (`addsudo`, owner-gated) |
| Delete sudo (`/delsudo`, aliases `delsudoer`, `sudodel`, `remsudo`, `rmsudo`, `sudorem`, `dropsudo`, `unsudo`) | Owner-only; removes a sudo user; refuses removing the owner and non-sudo users | `internal/modules/handlers.go:62-65`, `internal/modules/sudoers.go:138-228` | PRESENT — `dispatcher.cpp:470` (`delsudo`, `rmsudo`, owner-gated) |
| List sudo (`/sudoers`, `/listsudo`, `/sudolist`) | Lists owner + sudo users; open to everyone (no owner/sudo filter on the route) | `internal/modules/handlers.go:67-70`, `internal/modules/sudoers.go:229+` | PRESENT — `dispatcher.cpp:472-473` (`listsudo`, `sudolist`, everyone) |
| Sudo denial message | Non-sudo caller gets the `only_sudo` reply, but ONLY in private chat or when the command mentions the bot username; in a plain group command the filter fails silently | `internal/modules/filters.go:80-93` (`filterSudo`) | PRESENT — Parlay perm gates fail silently in all cases (`dispatcher.cpp:956-957`); the silent half matches, the PM reply half is not reproduced |
| Owner denial message | Same conditional pattern for owner-only commands (`only_owner` reply in PM / @-mentioned, silent otherwise) | `internal/modules/filters.go:104-116` (`filterOwner`) | PRESENT — same note as above; Parlay is silent in all cases |
| Sudo/owner join notices | Announces when the owner or a sudo user joins a chat | `internal/locales/en.yml` (`sudo_join_owner`, `sudo_join_sudo`) | PRESENT — `dispatcher.cpp:679-690` (`kOwnerJoined`, `kSudoJoined`) |
| Sudo-only screens | None: there is no sudo menu or sudo panel; sudo tool commands (`/speedtest`, `/activevc`, `/logger`, `/autoleave`, `/logs`) are plain commands gated by `sudoOnlyFilter` | `internal/modules/handlers.go` (speedtest, broadcast, active, logger, autoleave, logs routes) | N/A — ref has no sudo menu; Parlay likewise has no sudo panel (only the `sudoers` help topic, `ui_texts.hpp` `kHelpSudoers`) |

### 1.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card | Heading "Now Playing", track link, duration, requester; sent with the player keyboard; title truncated to 25 chars | `internal/locales/en.yml:900` (`stream_now_playing`), `internal/modules/play.go:595-615` | PRESENT — `dispatcher.cpp` now-playing card + `playerKeyboard()` (`contracts/routing-table.md` §A) |
| Queued card | "Added to Queue: #N" with track link, duration, requester; deliberately text-only (thumbnail logic removed for queue messages, per code comment) | `internal/locales/en.yml:472` (`play_added_to_queue_single`), `internal/modules/play.go:640+` | PRESENT — Parlay queued card + `pl:player:playnow:<token>` (`routing-table.md` A36) |
| Player controls | Progress-bar no-op button (row 1, only when not queued), then resume/pause/skip/replay/stop row, then Close row; channel variant uses `croom:` prefix | `internal/core/buttons.go:96-124` (`GetPlayMarkup`) | PRESENT — Parlay `pl:player:*` rows incl. progress no-op, plus seek ±15s and autoplay rows the ref lacks |
| Progress bar | 9-stage bar, ball glyph + dash track, format `M:SS <bar> M:SS`, built from position/duration | `internal/utils/get_progress.go:50-95` (`GetProgressBar`), used at `internal/core/buttons.go:104-109` | PRESENT — Parlay uses the same 9-stage bar (`routing-table.md` A20) |
| Stop-confirm card | Two-row confirm: resume-or-unmute button on row 1, stop on row 2, when stopping a paused/muted stream | `internal/core/buttons.go:60-84` (`GetStopConfirmMarkup`) | PRESENT — Parlay stop-confirm card (`routing-table.md` A38-A40); Parlay uses one row, not two |
| Queue display | Header + now-playing line + up-next list, first 10 waiting tracks; 3+ waiting tracks wrapped in a blockquote; over 10, the rest is rendered as a full list linked from a "more" line (paste-style overflow link, not a button) | `internal/modules/queue.go:158-335` (`handleQueue`), `internal/locales/en.yml:522-523` | PRESENT — Parlay queue card with per-track delete + Show-more button (`routing-table.md` A41-A42); the paste-link overflow itself is MISSING (Parlay paginates with a button instead) |
| Queue ops replies | `/clear` (restorable), `/restore`, `/remove [index]`, `/move [from] [to]`, `/shuffle`, `/position` (position/duration/speed line) | `internal/modules/queue.go`, `internal/modules/position.go`, `internal/modules/shuffle.go`, `internal/modules/loop.go` | PRESENT — Parlay `/clear`, `/restore`, `/remove` (incl. ranges), `/move`, `/shuffle`, `/position` (`routing-table.md` §C) |
| Play from reply | `/play` with no query but replying to a media message plays the replied media; `/fplay [reply to audio/video]` force-plays replied media | `internal/modules/play.go:338` (reply check), `internal/modules/play.go:72` (help text) | PRESENT — Parlay replied-media play via `vc.play_file` (`routing-table.md` C1) |
| Play history UI | None found: no history command, no history card; queue/autoplay state is internal only | (absent; greps of `internal/modules/` for history return no UI) | N/A — ref has no history UI; Parlay also has no history UI (no history surface in `dispatcher.cpp`) |

### 1.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | Add-me URL button, Help callback button, Updates + Support URL buttons, Source URL button | `internal/core/buttons.go:133-165` (`GetStartMarkup`) | PRESENT — Parlay start card (`ui_texts.hpp` start keyboard; `routing-table.md` §B.2) |
| Help keyboard | Two rows of topic buttons (Admins, Public / Owner, Sudoers), then Back-to-start | `internal/core/buttons.go:167-199` (`GetHelpKeyboard`) | PRESENT — Parlay help menu, 12 topics (`ui_texts.hpp` help keyboard) |
| Back keyboard | Single Back button returning to the help menu | `internal/core/buttons.go:200+` (`GetBackKeyboard`) | PRESENT — Parlay help topic pages carry a Back row (`pl:help:main`) |
| Group-help URL button | In-group `/help` replies with a URL button deep-linking to `?start=pm_help` | `internal/core/buttons.go:121-130` (`GetGroupHelpKeyboard`) | PRESENT — Parlay group `/help` notice + Commands URL button (`UI_INVENTORY.md` §3) |
| Player keyboard | See §1.2 Player controls | `internal/core/buttons.go:96-124` | PRESENT |
| Stop-confirm keyboard | See §1.2 Stop-confirm card | `internal/core/buttons.go:60-84` | PRESENT |
| Download-cancel keyboard | Single cancel button, callback `cancel` | `internal/core/buttons.go:30-38` (`GetCancelKeyboard`) | PRESENT — Parlay `pl:dl:cancel:<run_id>` (`routing-table.md` §A) |
| Broadcast-cancel keyboard | Single cancel button, callback `bcast_cancel` | `internal/core/buttons.go:40-48` (`GetBroadcastCancelKeyboard`) | PRESENT — Parlay `pl:bcast:cancel` (`routing-table.md` §A) |
| Settings panel keyboard | None: settings are command-only (`/adminmode`, `/playmode`, `/cmddelete`, `/cleanmode`, `/nothumb`, `/autoleave`); no settings markup builder exists | (absent; `internal/modules/settings.go` contains no markup/keyboard builder) | N/A — ref has no settings panel; Parlay HAS a settings panel (`ui_texts.hpp:1491` `settingsKeyboard`), so the panel concept is present in Parlay but not from this ref |
| Reply keyboards | None: every keyboard in the ref is inline | (absent; all builders return inline markup) | N/A — ref has none; Parlay reply-keyboard support exists in the sender (`dispatcher.hpp` `sendMessageReplyKb`) but is only used to remove a keyboard (`dispatcher.cpp:6257`) |

### 1.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing artwork | Track artwork URL attached as the message media (via send options `Media`) when present and thumbnails are enabled for the chat | `internal/modules/play.go:600-612` (`opt.Media = CleanURL(mainTrack.Artwork)`) | PRESENT — Parlay `sendPhoto` with track thumbnail as caption photo (`dispatcher.cpp:1686-1691`) |
| Queued-card thumbnail | Deliberately absent: queue messages are text-only even when artwork exists | `internal/modules/play.go:640+` (code comment: thumbnail logic removed for queue messages) | PRESENT — Parlay queued card is likewise text-first; the queued card's Play-Now button is the interactive element |
| Queue-card thumbnail | Queue list is text-only (no photo) | `internal/modules/queue.go:158-335` | PRESENT — Parlay queue card is text + delete buttons (`routing-table.md` A41-A42) |
| Thumbnail toggle | `/nothumb` with enable/disable argument (inverted sense: enabled = hide artwork) with status/updated/already replies; per-chat setting | `internal/modules/thumbnail.go`, `internal/database/thumbnail.go`, `internal/locales/en.yml:424-436` | PRESENT — Parlay `/thumbnail` (on/off) + `/nothumb` + settings-panel Thumbnails row (`dispatcher.cpp:449`, `routing-table.md` A49) |
| Thumbnail generation | None: artwork is the platform-supplied URL (YouTube/Spotify/SoundCloud), never generated or composited locally | `internal/platforms/youtube.go`, `internal/platforms/spotify.go`, `internal/platforms/soundcloud.go` (thumbnail URL fields) | N/A — ref generates nothing; Parlay likewise uses the track thumbnail URL, no generation |

### 1.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing template | Bold heading + bold field labels + linked title + plain duration/requester lines | `internal/locales/en.yml:900` | PRESENT — Parlay now-playing caption, same fields |
| Queued template | Bold "Added to Queue: #N" header + linked title + duration + requester | `internal/locales/en.yml:472` | PRESENT |
| Queue template | Bold headers, linked titles truncated to 35 chars, requester + `[mm:ss]` duration per line, blockquote wrapper when 3+ waiting tracks | `internal/modules/queue.go:190-240`, `internal/locales/en.yml:522-523` | PRESENT — Parlay queue card (richer: delete buttons + show-more); Parlay does not use the blockquote wrapper for the queue list |
| Help templates | Per-command help blocks with italic intro, underlined Usage heading, bold command names, `<code>` usage lines, Restrictions and Related sections | `internal/modules/flag_help.go`, `internal/modules/help.go`, per-module `helpTexts` init blocks (e.g. `queue.go:118+`) | PRESENT — Parlay per-command help (`ui_texts.hpp` help registry + `kHelpSudoers` etc.); unknown-command help reply is `No help found for command <code>…</code>` (`flag_help.go:39`) |
| Copy pills / hints | `<code>` usage spans throughout (copyable by tap in clients) and hint lines such as "Use /queue to view full list" | Throughout `internal/locales/en.yml` (`<code>` tokens), `internal/locales/en.yml` (`play_queue_view_hint`) | PRESENT — Parlay uses `<code>` pills and hint lines the same way |
| Collapsibles / footers | Blockquote wrapper for long up-next lists (see Queue template); no `<details>`, no `<tg-spoiler>`, no expandable blockquote, no page footers ("Page N of M") in this ref | `internal/modules/queue.go:200-230`; zero `blockquote expandable` / `tg-spoiler` hits in `internal/locales/en.yml` | MISSING — Parlay queue uses show-more instead; Parlay DOES use `<blockquote expandable>` for lyrics captions (`dispatcher.cpp:3884`), so the element exists in Parlay but not for queue lists |

### 1.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | gogram `tg.NewKeyboard()` builder: rows of `Button.Data(label, callback)` / `Button.URL(label, url)`; callbacks are colon-namespaced (`room:<id>:<action>`, `croom:<id>:<action>`, `help:<topic>`, `progress`, `close`, `cancel`, `bcast_cancel`) and routed by regex pattern in `handlers.go:466` and `cb.go` | `internal/core/buttons.go`, `internal/modules/cb.go:72-135`, `internal/modules/handlers.go:466` | PRESENT — Parlay `pl:` namespace + `handleCallback` router (`routing-table.md` §A); the `room:`/`croom:` split is collapsed (channel id carried in the same `pl:player:` callback) |

### 1.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop: the router only fires on registered patterns; there is no catch-all command handler and no unknown-command reply in `handlers.go` or `cb.go` | `internal/modules/handlers.go` (pattern table only), `internal/modules/cb.go` | MISSING — Parlay deliberately answers unknown commands with the unknown-command card + available-commands list (`dispatcher.cpp:1008-1013`, `ui_texts.hpp:694` `kUnknownCommand`); Parlay is never silent here |
| Unknown callback | Answered with an alert toast (`unknown_action`); unmatched callback data is logged as a warning, not silently ignored | `internal/modules/cb.go:130-135` | PRESENT — Parlay unknown callbacks answer the alert `Unknown action.` (`ui_texts.hpp:1034` `kToastUnknownAction`, `routing-table.md` A95) |

---

## 2. Music163bot-Go — `refs/Music163bot-Go/` (Go, Bot API via go-telegram/bot)

No voice-chat playback at all: this is a NetEase downloader bot. Entry points:
`/music`, `/netease`, `/search`, `/lyric`, `/program`, `/recognize`, `/status`,
`/about`, plus admin `/login`, `/rmcache`, `/reload`, `/setcachechat`.
Registration: `internal/bot/app.go:174-198`. No sudo system exists; admin =
config `Bot.Admin` id list. Strings: `internal/render/render.go` (Chinese
source text; Parlay ports are translations).

### 2.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Admin list (sudo equivalent) | Static config list `Bot.Admin`; admin-only commands are scoped per-admin in the bot-command menu | `internal/bot/commands.go:29-40`, `internal/config/config.go` (`Bot.Admin`) | PRESENT — Parlay owner + dynamic sudo set (`dispatcher.cpp:315-317`, `1023-1025`); Parlay's set is runtime-managed, this ref's is config-static |
| Admin denial (`adminOnly` middleware) | Silent drop: non-admin messages and callbacks return without any reply | `internal/bot/middleware.go:23-42` | PRESENT — Parlay perm gates fail silently (`dispatcher.cpp:956-957`) |
| Add/remove/list sudo UI | None: no add/remove/list commands, no sudo menu; membership changes require editing config and restarting | (absent; `internal/bot/app.go:174-198` registers no sudo commands) | N/A — ref has no sudo UI; Parlay HAS add/del/list (`dispatcher.cpp:470-473`) |
| Admin commands | `/rmcache`, `/reload`, `/login`, `/setcachechat` (all `adminOnly`); login-cancel/start-login callbacks also `adminOnly` | `internal/bot/app.go:184-193` | MISSING — Parlay has `/reload` (admin-cache) but no `/rmcache`, NetEase `/login`, or `/setcachechat` equivalents (no login UI in `dispatcher.cpp` at all) |

### 2.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card | None: no VC, no now-playing concept | (absent) | N/A — ref has none; Parlay HAS a now-playing card |
| Queue display / controls | None: downloads are serialised per request, no queue UI | (absent) | N/A — ref has none; Parlay HAS queue + controls |
| Play from reply / search | None as playback; the analogue is song delivery: `/music <id>` or a NetEase link/ID in text delivers the audio file | `internal/bot/handlers_music.go:38-103` (`handleMusic`, `handleDefault`, `musicFromText`) | PRESENT — Parlay download-delivery flow (`services/downloaders/`, `dispatcher.cpp` download handlers); playback semantics differ |
| Audio delivery message | Audio file sent with a caption (name, artists, album, extension, size, bitrate, via-bot line) and a one-row keyboard (song title button + "Open in NetEase Music" URL button); cached songs are re-sent by file id | `internal/bot/handlers_music.go:439-475` (`sendMusic`), `internal/bot/keyboards.go:14-33` (`musicKeyboard`), `internal/render/render.go:157` (`MusicCaption`) | PRESENT — Parlay audio delivery with caption + equivalent metadata (`dispatcher.cpp` download delivery) |
| Download status lifecycle | Status-line edits: fetching info, cache hit, waiting, downloading with speed/size/percent, complete, failure reasons | `internal/render/render.go:118-126` (`MusicStatus`, `DownloadStatus`), `internal/bot/handlers_music.go:160-230`, `410-425` | PRESENT — Parlay download progress edits (`routing-table.md` download rows; `pl:dl:cancel`) |
| Play history UI | None | (absent) | N/A — ref has none; Parlay also has none |

### 2.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Song keyboard | Row 1: song title button (callback `music <id>`); row 2: "Open in NetEase Music" URL button | `internal/bot/keyboards.go:14-33` | PRESENT — Parlay search-pick buttons (`pl:search:<n>:<user>:<id>`, `ui_texts.hpp:188`); the NetEase URL button itself is provider-specific and MISSING as such |
| Link keyboard | Single URL button opening the song in NetEase Music | `internal/bot/keyboards.go:34-40` (`linkKeyboard`) | MISSING — no provider link button in Parlay's delivery card |
| Guest (inline-mode) keyboard | Appends a "Get this song in the bot" button to inline-mode results | `internal/bot/keyboards.go:41-49` (`guestKeyboard`) | UNCERTAIN — Parlay inline results exist via `vc-ts` (`search.inline`); whether an equivalent "get in bot" button exists was not verified in this audit |
| Search keyboard | Number buttons (one row, 5-per-page layout in code) with callback `music <id>`, plus Previous/Next page buttons (`search <page> <hash>`) when applicable | `internal/bot/keyboards.go:50-70` (`searchKeyboard`), `internal/bot/handlers_search.go:139-157` | PRESENT — Parlay `pl:search:<n>` + `pl:search:page:<user>:<page>:<hash>` (`ui_texts.hpp:188-193`, `1336`) |
| Login-cancel keyboard | Single "Cancel login" button (`cancellogin`) on the QR login photo, plus an empty-keyboard variant that strips buttons | `internal/bot/handlers_login.go:78-88` (`cancelLoginKeyboard`, `emptyKeyboard`) | MISSING — Parlay has no login flow, hence no login-cancel keyboard |
| Inline-query results | Inline search articles + cached-audio/document results with captions; inline help article when the query is empty | `internal/bot/handlers_inline.go:17-130`, `internal/bot/handlers_music.go:426-437` | PRESENT — Parlay inline search via `vc-ts`; provider and result types differ (NetEase vs Parlay providers) |

### 2.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Audio-file thumbnail | Downloaded cover art attached as the audio message thumbnail; cached songs reuse the stored thumbnail file id | `internal/bot/handlers_music.go:439-475` (`sendMusic`: `Thumbnail` from `ThumbFileID` or uploaded cover file) | PRESENT — Parlay audio delivery carries thumbnails (`dispatcher.cpp:6730-6746` uses audio title/performer/thumbnail) |
| Inline-result thumbnail | Cached audio/document inline results carry the song caption; cover art is the file thumbnail where the platform provides one | `internal/bot/handlers_music.go:426-437` (`cachedMusicResult`) | PRESENT — Parlay inline results carry thumbnails via the same track-thumbnail contract |
| Now-playing / card thumbnail | None (no cards) | (absent) | N/A — ref has no cards; Parlay HAS card thumbnails |
| Thumbnail generation | None: cover art is downloaded from NetEase, resized only as needed for upload; no compositing | `internal/bot/handlers_music.go:220-260` (`fetchMusic`) | N/A — ref generates nothing; Parlay likewise generates nothing |
| Thumbnail fallback | If no cover is available, the audio is sent without a thumbnail (no default image) | `internal/bot/handlers_music.go:464-473` (thumbnail set only when present) | PRESENT — Parlay falls back to a text card when no thumbnail exists (`dispatcher.cpp:1691`) |

### 2.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Song caption template | Name, artists, album, extension, size in MB, bitrate, via-bot line (Chinese source; Parlay translation drops the via line into its own form) | `internal/render/render.go:157-161` (`MusicCaption`) | PRESENT — Parlay delivery caption with the same fields |
| Lyric caption template | Lyric text in an expandable blockquote, truncated to 1024 UTF-16 units with an ellipsis when longer, timestamps stripped, sent as a document caption | `internal/render/render.go:139-156` (`LyricCaption`, `truncateUTF16`), `internal/bot/handlers_lyric.go` | PRESENT — Parlay lyric delivery uses `<blockquote expandable>` with truncation (`dispatcher.cpp:3884`) |
| Search page template | Page header `[page/total]` in code form plus numbered items `N. name - artists` in code spans | `internal/render/render.go:203-209` (`SearchPage`, `SearchItem`), `internal/bot/handlers_search.go:139-157` | PRESENT — Parlay search pages (`ui_texts.hpp` search builders); Parlay numbering/buttons differ in layout |
| Status / about templates | Status: chat title/username/id, user id, total/by-chat/by-user download counts. About: runtime version, version name, commit, build time, architecture | `internal/render/render.go:162-190` (`About`, `StatusInfo`), `internal/bot/handlers_misc.go:16-35` | PRESENT — Parlay `/stats` + `/version` equivalents (stats text, version text in `ui_texts.hpp`/`dispatcher.cpp`) |
| Hints / guards | Cooldown toasts ("Too frequent, try again in N seconds"), stale-search and foreign-user replies on search callbacks, "Search expired, please search again" | `internal/bot/handlers_search.go:89-132` (`checkSearch`, `cooldown`), `internal/render/render.go:199` (`Cooldown`) | PRESENT — Parlay search cooldown/stale/foreign handling (`routing-table.md` search rows); exact wording differs |

### 2.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | go-telegram/bot `models.InlineKeyboardMarkup` with plain-text callback data (`music <id>`, `search <page> <hash>`, `cancellogin`, `startlogin`); prefix-matched callback routing in `app.go:189-193` | `internal/bot/keyboards.go`, `internal/bot/app.go:189-193` | PRESENT — Parlay `pl:` namespace + `handleCallback` (same prefix-routing idea, colon namespace instead of space-separated) |

### 2.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop: go-telegram/bot only invokes a handler when its match function matches; unregistered `/commands` match nothing and produce no reply | `internal/bot/app.go:174-230` (`registerHandlers`, `matchCommand`) | MISSING — Parlay answers unknown commands with the unknown-command card (`dispatcher.cpp:1008-1013`); Parlay is never silent here |
| Unknown / stale callback | Search callbacks are validated (owner, cooldown, page hash); failures are answered with a toast (cooldown text or expiry text), not silently dropped | `internal/bot/handlers_search.go:55-132` (`handleCallbackSearch`, `checkSearch`) | PRESENT — Parlay stale/foreign callbacks answer toasts/alerts (`routing-table.md` A95, search rows) |

---

## 3. NoxBeats — `refs/NoxBeats/` (C++20, Bot API)

Primary strings: `strings/en.json` (278 keys). Keyboards: `Bad/ui/buttons.cpp`.
Bot-API client: `Bad/tg/botapi_client.cpp`. Player callbacks: `Bad/cmd/play_cb.cpp`.
Settings/help/language callbacks: `Bad/cmd/settings.cpp`. Sudo: `Bad/cmd/sudo.cpp`.
Dispatch: `Bad/cmd/common.cpp`. All labels in the ref use small-caps Unicode;
labels below are paraphrased in normal English.

### 3.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Add sudo (`/addsudo`) | Owner-only; adds by reply/mention/id; replies: already-sudo, added; no-user prompt when no target given | `Bad/cmd/sudo.cpp:16-33`, `strings/en.json` (`sudo_added`, `sudo_already`, `user_not_found`) | PRESENT — `dispatcher.cpp:470` (`addsudo`, owner-gated) |
| Delete sudo (`/delsudo`, `/rmsudo`) | Owner-only; replies: not-a-sudo, removed | `Bad/cmd/sudo.cpp:16-33`, `strings/en.json` (`sudo_not`, `sudo_removed`) | PRESENT — `dispatcher.cpp:470` (`delsudo`, `rmsudo`, owner-gated) |
| List sudo (`/listsudo`, `/sudolist`) | Everyone may run it; first replies "fetching sudo users list", then edits to owner line + sudo user lines (mentions resolved via get_user) | `Bad/cmd/sudo.cpp:35-50`, `strings/en.json` (`sudo_fetching`, `sudo_owner`, `sudo_users`) | PRESENT — `dispatcher.cpp:472-473`, `5735+` (`onListSudo`) |
| Sudo denial | Silent: perm gates use `continue` in the route loop, so a non-sudo/non-owner caller gets no reply at all (this is the exact behaviour Parlay copied) | `Bad/cmd/common.cpp:165-170` (`dispatch_message` perm checks) | PRESENT — Parlay perm gates fail silently (`dispatcher.cpp:956-957`, comment cites this dispatch) |
| Sudo-only screens | None: no sudo menu or sudo panel; sudo tools (`/ac`, `/activevc`, `/broadcast`, `/logs`, `/logger`, `/restart`, `/speedtest`, `/maintenance`, `/autoleave`, `/blacklisted`) are plain sudo-gated commands; `/activevc` lists active calls with current track titles (or a document when over 4000 chars) | `Bad/cmd/sudo.cpp:52-75` (`ac`/`activevc`), `Bad/cmd/bot_tools.cpp`, `Bad/cmd/broadcast.cpp` | N/A — ref has no sudo menu; Parlay likewise has no sudo panel (only the `sudoers` help topic). The `/activevc` list itself is PRESENT in Parlay as the active-streams text |
| Sudo denial on buttons | Distinct from command denial: playback-control callbacks check `can_manage_vc` and answer the alert toast `user_no_perms` when the tapper may not manage the VC | `Bad/cmd/common.cpp` (`can_manage_vc` callback overload), `Bad/cmd/play_cb.cpp:29` | PRESENT — Parlay player callbacks answer permission alerts for unauthorised tappers (`routing-table.md` §A player rows) |

### 3.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card | "Started streaming" header, linked title, duration in minutes, requester; sent as a photo when a thumbnail exists, else text; `/player` re-sends it and deletes the old card | `strings/en.json` (`play_media`), `Bad/core/calls.cpp:106-130`, `Bad/cmd/playback.cpp:199-204` | PRESENT — Parlay now-playing photo card + `/player` re-send equivalent |
| Queued card | "Added to queue: N" header, linked title, duration, requester, plus a single "Play Now" button (force-plays that queued track) | `strings/en.json` (`play_queued`), `Bad/ui/buttons.cpp` (`play_queued`) | PRESENT — Parlay queued card + `pl:player:playnow:<token>` |
| Player controls | Status/timer no-op row (only when a status string is set), then resume/pause/replay/skip/stop row, then autoplay on/off + thumb on/off toggle row; tapping a toggle flips the per-chat setting and re-edits the keyboard | `Bad/ui/buttons.cpp` (`controls`), `Bad/cmd/play_cb.cpp:41-62` (autoplay/thumb toggles) | PRESENT — Parlay player rows incl. progress no-op, seek ±15s, autoplay toggle (`routing-table.md` A20-A28); the thumb toggle on the card itself is MISSING as a card button in Parlay (thumbnail is a settings-panel row instead, `routing-table.md` A49) |
| Queue-message toggle | Separate single-button keyboard on the `/queue` message: shows "stream paused/playing" and flips pause/resume; callback carries a trailing ` q` marker distinguishing it from card actions | `Bad/ui/buttons.cpp` (`queue_markup`), `Bad/cmd/play_cb.cpp:66-77` (`qaction` branch) | PRESENT — merged: Parlay collapses the queue-message toggle into the same `pl:player:pause/resume` callbacks (no `q` marker) |
| Queue display | "Fetching queue" reply edited to: currently-playing block + waiting tracks (up to 14, i.e. indexes 1..14) inside an expandable blockquote, with the queue-message toggle keyboard; photo when a thumbnail exists | `Bad/cmd/playback.cpp:104-130`, `strings/en.json` (`queue_curr`, `queue_item`, `queue_fetching`) | PRESENT — Parlay queue card (per-track delete + show-more, `routing-table.md` A41-A42); Parlay does not wrap the waiting list in an expandable blockquote |
| Queue ops replies | `/clear` (count + by-user), `/restore` / no-cleared-queue, `/remove [position]` (usage, bad position, removed), `/move [from] [to]`, `/shuffle` (needs 2+ waiting), `/position` (title + position/duration + speed) | `Bad/cmd/queue_ops.cpp`, `Bad/cmd/playback.cpp`, `strings/en.json` (`queue_*`, `position_now`) | PRESENT — Parlay queue ops set (`routing-table.md` §C) |
| Play from reply | `/play` with no query but replying to a media message downloads and plays the replied media; `-f` force and `-v` video flags; playlist/Spotify/Apple/JioSaavn/SoundCloud link handling | `Bad/cmd/play.cpp:94-150` (reply-media branch at :106-107), `Bad/cmd/play.cpp:200+` | PRESENT — Parlay replied-media play + force/video variants (`routing-table.md` C1) |
| Stop-confirm card | None: `/stop` stops immediately; no paused/muted confirm step exists in this ref | (absent; `Bad/cmd/playback.cpp:42-50` stops directly) | N/A — ref has no stop-confirm; Parlay HAS one (`routing-table.md` A38-A40) |
| Play history UI | None: autoplay keeps an internal played-history deque for related-track selection, but no user-facing history list exists | `Bad/core/autoplay.hpp:62` (`history_`), `Bad/core/autoplay.cpp:222-233` (internal use only) | N/A — ref has no history UI; Parlay also has none |

### 3.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | Add-me URL row, Help callback row, Support + Channel URL row, then Source URL row (private only) or Language callback row (group only) | `Bad/ui/buttons.cpp` (`start_key`) | PRESENT — Parlay start card (same rows, `pl:help:main` / `pl:lang:select`) |
| Help keyboard | 9 category buttons, 3 per row (Admins, Auth, Blacklist / Language, Ping, Play / Queue, Stats, Sudoers), then Home + Close; category view adds Back + Home + Close; Close deletes the message and its reply-to | `Bad/ui/buttons.cpp` (`help_markup`), `Bad/cmd/settings.cpp:99-122` | PRESENT — Parlay help menu (12 topics) + Back/Close behaviour (`routing-table.md` A1-A17, `pl:close`) |
| Settings keyboard | 3 rows, each a label button (no-op toast "Processing...") + a value button: admin-only-play True/False, command-delete True/False, language name (opens the picker) | `Bad/ui/buttons.cpp` (`settings_markup`), `Bad/cmd/settings.cpp:50-62`, `124-135` | PRESENT — Parlay settings panel (`ui_texts.hpp:1491`), superset with 13 keys; Parlay label buttons answer explainer alerts instead of "Processing..." |
| Language picker | Language name + code buttons, 2 per row, current language marked with a check suffix; tap answers a "changing language" toast then edits to a "changed" message | `Bad/ui/buttons.cpp` (`lang_markup`), `Bad/cmd/settings.cpp:77-98` | PRESENT — Parlay `pl:lang:select` / `pl:lang:<code>`; Parlay ships English only, so the picker lists English |
| Player keyboard | See §3.2 Player controls | `Bad/ui/buttons.cpp` (`controls`) | PRESENT (with the thumb-toggle-on-card exception noted in §3.2) |
| Queue toggle keyboard | See §3.2 Queue-message toggle | `Bad/ui/buttons.cpp` (`queue_markup`) | PRESENT (merged form) |
| Download-cancel keyboard | Single cancel button (`cancel_dl`); success edits to "download cancelled by X", nothing-to-cancel answers an alert toast | `Bad/ui/buttons.cpp` (`cancel_dl`), `Bad/cmd/play_cb.cpp:20-26` | PRESENT — Parlay `pl:dl:cancel:<run_id>` |
| YouTube / copy keyboard | Two buttons on YouTube result cards: a copy button (copies the link text) and a Youtube URL button | `Bad/ui/buttons.cpp` (`yt_key`) | MISSING — Parlay has no `copy_text` button type anywhere in `services/core-cpp/src/` (grep for `copy_text` returns nothing); URL buttons exist, copy buttons do not |
| Reply keyboards | None: every keyboard in the ref is inline | (absent; `Bad/ui/buttons.cpp` builds only inline keyboards) | N/A — ref has none; Parlay reply-keyboard support exists but is only used to remove a keyboard |

### 3.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing photo | Track thumbnail (YouTube thumbnail when available) used as the photo; otherwise the configured default thumbnail when thumbnail generation is enabled; else text card | `Bad/core/calls.cpp:106-130`, `Bad/cmd/playback.cpp:111-124` (queue photo path) | PRESENT — Parlay `sendPhoto` thumbnail rule (`dispatcher.cpp:1686-1691`) |
| Default thumbnail | Configured `DEFAULT_THUMB` used when a YouTube track has no thumbnail, gated by `THUMB_GEN` | `Bad/cmd/playback.cpp:111-112`, `Bad/core/calls.cpp:111-112` | MISSING — Parlay has no configured default-thumbnail fallback; it falls back to a text card instead |
| Queue-card photo | Same thumbnail/default rule applied to the `/queue` message (edit_photo when a thumbnail exists) | `Bad/cmd/playback.cpp:111-124` | MISSING — Parlay queue card is text-only (no photo variant in `buildQueueCard`, `dispatcher.cpp:1698+`) |
| Thumbnail toggle | Per-chat thumb setting flipped from the player card (thumb on/off row) or `/thumbnail on|off`; status/updated replies describe the effect on next songs | `Bad/cmd/play_cb.cpp:41-62`, `Bad/cmd/autoplay.cpp:63+` (`thumbnail`/`thumb`), `strings/en.json` (`thumb_status`, `thumb_enabled`, `thumb_disabled`) | PRESENT — Parlay `/thumbnail` + settings-panel row; only the on-card toggle placement differs |
| Thumbnail generation | None beyond the default-image fallback: no compositing, no banner generation; thumbnails are remote URLs | `Bad/core/calls.cpp:106-130` | N/A — ref generates nothing; Parlay likewise generates nothing |

### 3.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start / help templates | Small-caps HTML: bold/underline/italic headers, linked titles, code spans; help menu text + 9 category texts | `strings/en.json` (`start_pm`, `start_gp`, `help_menu`, `help_*`) | PRESENT — Parlay start/help cards in normal English, same structure |
| Now-playing template | Underlined-bold "Started streaming" header, bold field labels, linked title, duration, requester | `strings/en.json` (`play_media`) | PRESENT |
| Status blockquote append | After pause/resume, the card is re-edited with the previous trailing blockquote stripped and a new `<blockquote>action by user</blockquote>` appended; skip/replay/stop instead send the action line as a new message and delete the card | `Bad/cmd/play_cb.cpp:100-127` | PRESENT — Parlay edits state cards with actor lines (`routing-table.md` player rows); the strip-and-append blockquote mechanic itself is Parlay-equivalent in effect |
| Queue expandable blockquote | Waiting tracks rendered inside `<blockquote expandable>…</blockquote>` on the queue message | `Bad/cmd/playback.cpp:120-127` | MISSING — Parlay queue list is not wrapped in an expandable blockquote (show-more button instead) |
| Copy pills | `<code>` spans for ids, commands and usage lines throughout (tap-to-copy in clients); plus the dedicated copy button (§3.3) | Throughout `strings/en.json` (72 `<code>` tokens); `Bad/ui/buttons.cpp` (`yt_key`) | PRESENT — `<code>` pills only; the dedicated copy button is MISSING (see §3.3) |
| Page footers / hints | No "Page N of M" footers anywhere; hints appear as trailing lines (e.g. queue-limit hint, "use /queue to view full list") | `strings/en.json` (`play_queue_limit_hint`, `play_queue_view_hint` equivalents) | MISSING — Parlay has no page footers either except the queue show-more pattern; hint lines ARE present in Parlay |

### 3.6 Rich inline buttons — DEEP DIVE (local clone authoritative)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Button value type | Plain struct with four fields: display text, callback data, URL, copy text — exactly one of the three payload kinds is used per button; no switch-inline, no web-app, no login buttons anywhere | `Bad/core/types.hpp:42-48` (`struct Button`), `Bad/ui/buttons.hpp:1-17` | PRESENT — Parlay inline buttons cover callback + URL kinds (`dispatcher.hpp` `InlineKeyboard`); the copy-text kind is MISSING |
| Keyboard value type | A keyboard is a vector of rows, each a vector of buttons; every builder in `buttons.cpp` returns this one type, so all surfaces share one rendering path | `Bad/core/types.hpp:48` (`using Keyboard = vector<vector<Button>>`) | PRESENT — Parlay `InlineKeyboard` type (`dispatcher.hpp`) |
| Serialisation + 64-byte rule | The Bot-API client serialises each button to `{text, url}` or `{text, copy_text:{text}}` or `{text, callback_data}`; callback data is hard-truncated to 64 bytes (`substr(0, 64)`) at send time, never rejected | `Bad/tg/botapi_client.cpp:110-128` (`keyboard_json`) | MISSING — Parlay enforces the 64-byte limit by construction (short `pl:` callbacks + token rule, proven by `tests/routing_test.cpp` per `routing-table.md:20`); Parlay never truncates a payload |
| Space-separated callback grammar | Player callbacks are space-separated: `controls <action> <chat_id>` with an optional trailing ` q` (queue-message) or `<track_id>` (force); help is `help` / `help <cat>`; settings are `settings` / `settings play|delete`; language is `language` / `lang_change <code>`; download cancel is the bare token `cancel_dl` | `Bad/ui/buttons.cpp` (all builders), `Bad/cmd/play_cb.cpp:28-33` (split on whitespace) | MISSING — by design: Parlay uses the colon `pl:` namespace (`routing-table.md` §1); the mapping table there explicitly converts every NoxBeats form |
| Callback routing | Regex routes: `cancel_dl` and `controls` in `play_cb.cpp`; `^lang(?:_change\|uage)`, `help`, `settings` in `settings.cpp`; dispatch tries routes in order, first regex match wins; unmatched callbacks are silently dropped (no fallback answer) | `Bad/cmd/play_cb.cpp:18-28`, `Bad/cmd/settings.cpp:82-135`, `Bad/cmd/common.cpp` (`dispatch_callback`) | PRESENT — Parlay `handleCallback` router with an explicit default branch; Parlay answers unmatched callbacks with the `Unknown action.` alert instead of dropping them |
| How a tap is built, sent, handled | Builder (e.g. `controls(chat)`) reads live per-chat state (autoplay/thumb flags) to pick toggle labels → attached as `reply_markup` on `send_message`/`send_photo`/`edit_text`/`edit_caption` → tap arrives as a callback query → `can_manage_vc` gate (alert on failure) → action executes → card is edited in place (`edit_caption` for photo cards with `edit_text` fallback, `edit_markup` for toggle-only refreshes) or deleted (skip/replay/stop) | `Bad/ui/buttons.cpp` (`controls`), `Bad/tg/botapi_client.cpp:195-270` (send/edit paths), `Bad/cmd/play_cb.cpp:28-127` | PRESENT — Parlay same lifecycle (`handleCallback` → `playerAction` → `refreshCard`, `editCbKeyboard`, photo/text card refresh in `dispatcher.cpp`) |
| Toggle buttons that show state | Autoplay and thumb buttons display their current state in the label ("Autoplay | On/Off", "Thumb | On/Off") and flip the stored setting on tap, then re-edit only the keyboard | `Bad/ui/buttons.cpp` (`controls`, toggle row), `Bad/cmd/play_cb.cpp:41-62` | PRESENT — Parlay autoplay toggle button shows Enabled/Disabled and flips on tap (`routing-table.md` A28); thumb state lives in the settings panel, not on the card |
| No-op label buttons | The status/timer row and the progress-style label button answer the callback with an empty answer (silent ack) and change nothing; the `settings` label buttons answer a "Processing..." toast | `Bad/cmd/play_cb.cpp:60` (`action == "status"`), `Bad/cmd/settings.cpp:124-135` | PRESENT — Parlay progress button is a silent-ack no-op (`routing-table.md` A20); settings label buttons answer explainer alerts instead of "Processing..." |
| Expired-button behaviour | Force-play ("Play Now") with a track id no longer in the queue edits the message to "This button has expired" | `Bad/cmd/play_cb.cpp:84-90` (`controls force` branch) | PRESENT — Parlay expired tokens answer the alert `This button has expired.` (`routing-table.md` A36, A42) |
| Foreign-tapper behaviour | No per-button requester check on player controls: any user who passes `can_manage_vc` (admin/auth/sudo per admin mode) may tap; the check is role-based, not tapper-vs-requester | `Bad/cmd/common.cpp` (`can_manage_vc`), `Bad/cmd/play_cb.cpp:29` | PRESENT — Parlay player callbacks are likewise role-gated, not requester-gated (contrast search/vote callbacks, which ARE requester-gated) |
| Rich-message + button pairing | Every rich card pairs its HTML text with exactly one keyboard builder: start→`start_key`, help→`help_markup`, settings→`settings_markup`, language→`lang_markup`, player→`controls`, queue→`queue_markup`, download→`cancel_dl`, ping→`ping_markup`, YouTube→`yt_key`; photo cards use `edit_caption`, text cards `edit_text`, keyboard-only refreshes `edit_markup` | `Bad/ui/buttons.hpp:1-17` (full builder list), `Bad/tg/client.hpp:39-51` (send/edit/answer surface) | PRESENT — Parlay pairs each card with one keyboard builder (`ui_texts.hpp` `*Keyboard` builders, `routing-table.md` §A) with the same photo/text/edit split |

### 3.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop: `dispatch_message` loops routes and returns after the first match; if no route matches, the function ends with no reply (confirmed by reading the full loop, not by a missing grep) | `Bad/cmd/common.cpp:150-185` (`dispatch_message`) | MISSING — Parlay answers with the unknown-command card (`dispatcher.cpp:1008-1013`); Parlay is never silent here |
| Unknown callback | Silent drop: `dispatch_callback` loops regex routes and ends with no answer when nothing matches | `Bad/cmd/common.cpp:187-198` (`dispatch_callback`) | MISSING — Parlay answers unmatched callbacks with the `Unknown action.` alert (`routing-table.md` A95) |

---

## 4. ParvatiMusic — `refs/ParvatiMusic/` (Rust, MTProto via own client + ferogram bot layer)

Router: `bot/src/modules/mod.rs` (registry + bus dispatch). Strings:
`bot/locales/en.yml` (small-caps Unicode + `{emoji:…}` tokens substituted at
render). Buttons: `bot/src/buttons.rs`. Settings: `bot/src/modules/settings_cb.rs`.
Sudo: `bot/src/modules/admin.rs:746+`. Queue: `bot/src/modules/queuecmd.rs`.
Artwork cache: `bot/src/platforms/artwork.rs`.

### 4.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Add sudo (`/addsudo`) | Owner-only via `require_owner`; refuses owner-self and bot-self, refuses existing sudo, otherwise adds and replies success/failure; no-args case prompts for a user | `bot/src/modules/admin.rs:748-802`, `bot/src/modules/admin.rs:154-166` (`require_owner`) | PRESENT — `dispatcher.cpp:470` (`addsudo`, owner-gated) |
| Delete sudo (`/delsudo`) | Owner-only; refuses removing the owner, refuses non-sudo targets, otherwise removes and replies | `bot/src/modules/admin.rs:804-857` | PRESENT — `dispatcher.cpp:470` (`delsudo`, `rmsudo`) |
| List sudo (`/sudolist`) | Header + owner entry (index 1) + one entry per extra sudoer + "no additional sudoers" line when empty; no gate stated on the handler (open list) | `bot/src/modules/admin.rs:859-890` (get_sudoers_handler), `bot/locales/en.yml` (`sudo_list_*`) | PRESENT — `dispatcher.cpp:472-473`, `5735+` |
| Sudo denial message | NOT silent: `require_owner` replies the `only_owner` text to the caller and logs a warning; this is the one ref in the group whose owner denial is a visible reply | `bot/src/modules/admin.rs:154-166` | MISSING — Parlay owner/sudo gates fail silently (`dispatcher.cpp:956-957`); Parlay has no `only_owner` denial reply |
| Sudo/owner join notices | Announces owner and sudo-user joins, same wording family as Arc | `bot/locales/en.yml` (`sudo_join_owner`, `sudo_join_sudo`) | PRESENT — `dispatcher.cpp:679-690` |
| Sudo-only screens | None: no sudo menu/panel; sudo tools are plain gated commands | `bot/src/modules/admin.rs`, `bot/src/modules/system.rs` | N/A — ref has no sudo menu; Parlay likewise has none |

### 4.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card | Blockquote header "Playback activated | Enjoy the music" + expandable blockquote with melody (title), length, requester; title handling and progress bar via the player markup | `bot/locales/en.yml:1020` (`stream_now_playing`), `bot/src/buttons.rs:100-135` (`play_markup`) | PRESENT — Parlay now-playing card (photo + caption form; Parlay does not use the "Playback activated" header line) |
| Queued card | Blockquote header "Incoming track detected: #N" + expandable blockquote with melody, length, requester, and a "Standby, your session begins shortly" footer line | `bot/locales/en.yml:602` (`play_added_to_queue_single`) | PRESENT — Parlay queued card; the "Standby…" footer line is MISSING in Parlay (dropped as flowery text per Phase 9) |
| Player controls | Progress no-op row (only when not queued), resume/pause/replay/skip/stop row, ±15s seek row (labels in small-caps digit glyphs), autoplay toggle row (state in label), Close row; channel variant uses `croom:`-style prefix via the room id | `bot/src/buttons.rs:100-135` (`play_markup`) | PRESENT — Parlay player rows match (progress, 5 controls, ±15s, autoplay, close); Parlay seek labels are plain "-15s"/"+15s" text |
| Stop-confirm card | One row: resume-or-unmute button + stop button, when stopping a paused/muted stream | `bot/src/buttons.rs:137-150` (`stop_confirm_markup`) | PRESENT — Parlay stop-confirm (`routing-table.md` A38-A40) |
| Queue display | Header with count, now-playing cell, up-next block: first 10 waiting tracks, "and N more" line when longer, empty-tail line when none; no per-track buttons, no show-more button | `bot/src/modules/queuecmd.rs:16-95` (`handle_queue`), `bot/locales/en.yml` (`queue_header`, `queue_now_playing`, `queue_up_next`, `queue_more_line`) | PRESENT — Parlay queue card is richer (per-track delete + show-more button); the plain 10-track + "and N more" form is superseded |
| Queue ops replies | Clear/restore/remove/move/shuffle/position/loop/speed/mute families with the same coverage as Arc/Nox (admin/auth-gated) | `bot/src/modules/queuecmd.rs`, `bot/src/modules/skip.rs`, `bot/src/modules/pause.rs`, `bot/src/modules/stop.rs`, `bot/src/modules/vc.rs` | PRESENT — Parlay queue ops set |
| Play from reply | `/play` with no query and no reply shows the no-song-query blockquote; a replied message is accepted as the play source (reply branch present in the handler) | `bot/src/modules/play.rs:239-246` | UNCERTAIN — Parlay replied-media play exists, but whether Parvati's reply branch covers audio/video files (vs links) was not fully traced in this audit |
| Playlists (personal) | `/createplaylist`, `/deleteplaylist`, `/addtoplaylist`, `/removefromplaylist`, `/playlist` info, `/myplaylists`, plus an autoplay menu with a toggle button | `bot/src/modules/playlists.rs:52-377` (handlers + `autoplay_menu_*` at :315-347) | PRESENT — Parlay personal playlist commands + add-to-playlist button (`routing-table.md` A37); the standalone autoplay menu card is merged into the player card's autoplay row |
| Play history UI | None user-facing; played history is kept internally for autoplay pool selection | `bot/src/modules/vc.rs:73-74`, `937` (`played_history`) | N/A — ref has no history UI; Parlay also has none |

### 4.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start keyboard | Add-me URL row, Help callback row, Updates + Support URL rows, Source URL row | `bot/src/buttons.rs:227-246` (`start_markup`) | PRESENT — Parlay start card |
| Help keyboard | 12 topic buttons, 3 per row (Admin, Auth, Broadcast / Play, Sudo, Restrict / Thumbnail, Start, Autoplay / Playlist, VC Logs, Playback Buttons), then Back row | `bot/src/buttons.rs:191-217` (`help_keyboard`) | PRESENT — Parlay help menu is this exact 12-topic set (`ui_texts.hpp` help keyboard) |
| Back keyboard | Single Back button to the help menu | `bot/src/buttons.rs:219-225` (`back_keyboard`) | PRESENT |
| Player keyboard | See §4.2 Player controls | `bot/src/buttons.rs:100-135` | PRESENT |
| Stop-confirm keyboard | See §4.2 Stop-confirm card | `bot/src/buttons.rs:137-150` | PRESENT |
| Settings keyboard | Label + value rows: Admin Mode, Play Mode, Cmd Delete, Clean Mode, Clean Delay (minutes), Thumbnails, Language; info buttons answer explainer alerts; value buttons cycle/toggle; Close row; no autoplay row (autoplay lives on the player card) | `bot/src/modules/settings_cb.rs:86-125` (`settings_markup`), `:167-242` (settings/lang handlers) | PRESENT — Parlay settings panel (`ui_texts.hpp:1491`) is a superset (adds autoplay, silent, auto-delete, timer, pagination, music-download rows) |
| Language picker | Language-name buttons in rows + Back-to-settings button; callback `lang:<code>`; picker opened from settings via `lang:select` | `bot/src/modules/settings_cb.rs:243-265` (`lang_markup`) | PRESENT — Parlay `pl:lang:select` / `pl:lang:<code>` |
| Download-cancel / broadcast-cancel keyboards | Single cancel buttons (`cancel`, `bcast_cancel` equivalents) on download-progress and broadcast-progress messages | `bot/src/buttons.rs:159-173` (`cancel_keyboard`, `broadcast_cancel_keyboard`) | PRESENT — Parlay `pl:dl:cancel` / `pl:bcast:cancel` |
| Reply keyboards | None | (absent; all builders return inline `Markup`) | N/A — ref has none; Parlay reply-keyboard support is send-side only |

### 4.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Artwork cache | Cover art is fetched over HTTP (15s timeout, 6 MB cap, max 5 redirects) and cached under `cache/art/<fnv1a-64-of-lowercased-url>.jpg`; fetch failure returns an empty path (no error surfaced to the card) | `bot/src/platforms/artwork.rs:1-80` (`artwork_local_path`, `hash_key`) | MISSING — Parlay uses the remote thumbnail URL directly (`sendPhoto`); it has no local artwork cache file |
| Now-playing media | Room status tracks whether the status message is a photo (`status_is_photo`); now-playing is sent with artwork media when available (gated by the thumbnails setting) | `bot/src/room.rs:31`, `65`, `bot/src/mod_tg.rs:51-61` (`send_photo` trait) | PRESENT — Parlay photo-or-text card rule (`dispatcher.cpp:1686-1691`) |
| Thumbnail toggle | Settings-panel Thumbnails row (value = enabled/disabled); no thumb toggle on the player card itself | `bot/src/modules/settings_cb.rs:114-115` | PRESENT — Parlay settings-panel Thumbnails row (`routing-table.md` A49) |
| Thumbnail generation | None: fetched cover art is used as-is; no compositing or banner generation | `bot/src/platforms/artwork.rs` | N/A — ref generates nothing; Parlay likewise generates nothing |

### 4.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Blockquote templates | Nearly every card is wrapped in `<blockquote>` (82 occurrences in en.yml), often with a second `<blockquote expandable>` for the detail block | `bot/locales/en.yml` (82 `blockquote`, 3 `expandable`) | PRESENT — Parlay uses blockquotes for help/queue/lyric cards; Parlay does not wrap every card in blockquotes |
| Emoji-token substitution | Locale strings contain `{emoji:…}` tokens that the loader substitutes with the literal emoji at render time | `bot/locales/en.yml` (e.g. `queue_now_playing`, `stream_now_playing`) | MISSING — Parlay strings contain no `{emoji:…}` tokens; Parlay text is written directly |
| Code pills / hints | `<code>` spans for ids, commands, usage; hint lines ("Use /queue to view full list" family, autoplay hints) | Throughout `bot/locales/en.yml` | PRESENT |
| Page footers | No "Page N of M" footers; long lists use "and N more" lines instead | `bot/src/modules/queuecmd.rs:73-90` | MISSING — Parlay queue uses a show-more button; neither ref nor Parlay has page footers for queues |
| Per-command help | 69 help arms with intro + Usage + Restrictions + Use Case per command, plus channel-variant auto-generation | `bot/src/modules/help_texts.rs` (69 arms, per Phase 9 map; verified present in tree) | PRESENT — Parlay per-command help registry (`ui_texts.hpp`) |

### 4.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | `Button { text, kind, style }` where kind is URL or Callback; callback payloads are base64-encoded at build time and base64-decoded on receipt; every button also carries a random style value (1-3, randomised per button at build) | `bot/src/buttons.rs:48-84` (`url_btn`, `data_btn`, `decode_btn_data`, `random_style`) | MISSING — by design: Parlay never base64-encodes callbacks (`pl:` plain text, `routing-table.md` §1) and has no per-button style field; the random style is dropped in translation |
| Callback routing | Callbacks are dispatched by decoded prefix against the registry; unknown actions answer the `unknown_action` alert toast | `bot/src/modules/roomcb.rs:528`, `bot/src/modules/mod.rs:143-160` (`dispatch`) | PRESENT — Parlay `handleCallback` + `Unknown action.` alert |

### 4.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop, twice over: the MTProto layer only registers handlers for known command names, and the registry `dispatch` ignores a Command event with no matching handler (empty `if let Some(h)` branch, no else) | `bot/src/modules/mod.rs:143-160` (`dispatch`), `bot/src/modules/mod.rs:230-290` (`init` registration loop) | MISSING — Parlay answers with the unknown-command card (`dispatcher.cpp:1008-1013`) |
| Unknown callback | Alert toast (`unknown_action`), not silent, when a decoded callback action matches no handler | `bot/src/modules/roomcb.rs:528` | PRESENT — Parlay `Unknown action.` alert |

---

## 5. Spotify-Loader — `refs/Spotify-Loader/` (Python, Pyrogram; single file `bot.py`, 397 lines)

Owner model: `OWNER_IDS` env list. When set, every handler also requires
`filters.chat(OWNER_IDS)`; when unset, the filter passes everyone. There is no
add/remove/list UI for owners. All handlers are private-chat only. No VC, no
queue, no thumbnails on cards. Markdown parse mode (not HTML).

### 5.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Owner filter (sudo equivalent) | Every message handler is AND-combined with an owner filter; non-owners never reach a handler when `OWNER_IDS` is set | `bot.py:81-86` (`OWNER_FILTER`), applied at `bot.py:89`, `97`, `117`, `125`, `168` | UNCERTAIN — Parlay has owner + sudo gates and a private-mode notice, but an env-list owner-only private mode equivalent to `OWNER_IDS` was not verified in `dispatcher.cpp` in this audit |
| Add/remove/list owner UI | None: owners are changed by editing the environment and restarting; no command, no menu, no list output | (absent; no owner-management handler in `bot.py`) | N/A — ref has no owner UI; Parlay HAS add/del/list |
| Non-owner denial | Silent drop: the filter prevents the handler from running, so a non-owner gets no reply at all | `bot.py:81-86` + handler decorators | PRESENT — Parlay perm gates fail silently (`dispatcher.cpp:956-957`) |

### 5.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing card | None: no VC playback | (absent) | N/A — ref has none; Parlay HAS one |
| Queue display / controls | None: downloads run sequentially per request, no queue UI | (absent) | N/A — ref has none; Parlay HAS queue + controls |
| Spotify-link flow | A Spotify track/album/artist/playlist URL (regex-matched) prompts "Select Options Below!" with the download-option picker; files are then sent one by one or as a ZIP | `bot.py:117-124` (`downloader`), `bot.py:41-51` (`CB_BUTTONS`) | PRESENT — Parlay Spotify download flow with option picker (ZIP vs one-by-one, `UI_INVENTORY.md` §8) |
| Search + deliver | `/s` / `/search <query>` runs spotdl with the user's stored settings and sends every downloaded audio file, then replies "Successfully uploaded X"; zero files replies "I've found nothing." | `bot.py:125-166` (`search`) | PRESENT — Parlay `/search` + download delivery; Parlay search shows a results list first, this ref downloads immediately |

### 5.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start buttons | Row 1: Source URL + Project Channel URL; row 2: Author URL; same keyboard is reused on `/help` | `bot.py:40-45` (`START_BUTTONS`), `bot.py:89-115` | PRESENT — Parlay start card URL rows (different URLs: Parlay config, not this bot's) |
| Download-option picker | "Send as ZIP" (`zip`) + "Send one by one" (`1by1`); the original message is deleted on tap | `bot.py:49-51` (`CB_BUTTONS`), `bot.py:117-124` | PRESENT — Parlay ZIP vs one-by-one picker (`pl:dl:zip`, `pl:dl:1by1` per `UI_INVENTORY.md` §8) |
| Settings menu | "Select one of the settings below!" with Output Format / Use Youtube / Path Template buttons; silently does nothing when no Mongo DB is configured (`else: pass`) | `bot.py:168-190` (`settings`) | PRESENT — Parlay download-settings screens (`ui_texts.hpp:1671+`); Parlay has no Mongo-gated silent no-op |
| Format picker | Current format shown in code ticks + 2-per-row MP3/M4A/FLAC/OPUS/OGG/WAV buttons + Back row; re-tapping the current format silently returns without editing | `bot.py:205-247` (`callbacks`, `of` branch + format branches) | PRESENT — Parlay `pl:dlset:fmt:<fmt>` format screen (`ui_texts.hpp:1682+`) |
| Yes/No toggle pickers | Use-Youtube and (in the same pattern) boolean settings use True/False button rows + Back row | `bot.py:257-281` (`callbacks`, `uy` branch) | PRESENT — Parlay settings value buttons (Enabled/Disabled) |
| Path-template editor | "Edit Current Path Template" button + Back row; the edit itself happens via a follow-up message (pyromod listen) | `bot.py:291-313` (`callbacks`, `pt`/`edit_pt` branches) | PRESENT — Parlay path-template setting exists in download settings (`ui_texts.hpp` dlset screens); the listen-for-next-message edit pattern is equivalent in effect |
| Reply keyboards | None | (absent) | N/A — ref has none; Parlay reply-keyboard support is send-side only |

### 5.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Card thumbnail | None: no cards carry photos; start/help/settings are text + inline keyboards | `bot.py:89-190` | N/A — ref has no card thumbnails; Parlay HAS them |
| Audio-file thumbnail | None passed explicitly: `send_audio` is called with only the file (cover art, if any, comes embedded in the file by spotdl, not as a Telegram thumbnail parameter) | `bot.py:160-163`, `389-392` (`send_audio` calls) | MISSING — Parlay passes thumbnails explicitly on audio delivery (`dispatcher.cpp:6730-6746`); this ref's embedded-art-only behaviour is not reproduced as a parameter |
| Thumbnail generation | None | (absent) | N/A — ref generates nothing; Parlay likewise generates nothing |

### 5.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start text | Plain sentence: bot name + "You can download spotify playlist/artist/album/track music using this bot!" (Markdown mode, no formatting) | `bot.py:89-94` | PRESENT — Parlay start card (richer, HTML) |
| Help text | Markdown block: "How to use this bot?!" + Spotify URL example in code ticks + command list (`/settings`, `/s`/`/search`, `/help`, `/start`) | `bot.py:97-115` | PRESENT — Parlay help menu + topics (HTML, richer) |
| Settings texts | "Current Output Format: `<fmt>` / Select one of the buttons below to change your output format." and the equivalent Use-Youtube / Path-Template screens | `bot.py:205-313` (`callbacks`) | PRESENT — Parlay download-settings screens |
| Success / failure texts | "Successfully uploaded X" (single file name or "all music files"), "I've found nothing." on zero files, "Looks like you've sent the wrong Spotify URL." on unusable URLs | `bot.py:160-166`, `bot.py:117-124` (downloader tail) | PRESENT — Parlay download success/failure texts (wording differs) |
| Copy pills / hints / footers | Code ticks for the format value and the Spotify URL example; no copy pills as buttons, no hints section, no page footers | `bot.py:97-115`, `205-247` | PRESENT — Parlay `<code>` pills; no footers in either |

### 5.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | Pyrogram `InlineKeyboardMarkup` with bare string callback data (`zip`, `1by1`, `of`, `uy`, `pt`, `mp3`…`wav`, `True`, `False`, `back`, `edit_pt`); one global `@on_callback_query` handler with an if-chain, no per-button routing table | `bot.py:40-51`, `bot.py:192-330` (`callbacks`) | PRESENT — Parlay `pl:` namespace + `handleCallback` router; the bare-string style is deliberately not copied |

### 5.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop: Pyrogram only invokes a registered handler; an unregistered `/command` (or any non-matching text) matches no handler and gets no reply | `bot.py:89-192` (only 5 message handlers exist) | MISSING — Parlay answers with the unknown-command card (`dispatcher.cpp:1008-1013`) |
| Unknown callback data | Silent return: the if-chain in `callbacks()` has no else branch, so unrecognised callback data (or a tap whose original message lost its reply-to) simply does nothing | `bot.py:192-330` (no else/fallback branch) | MISSING — Parlay answers unmatched callbacks with the `Unknown action.` alert (`routing-table.md` A95) |

---

## 6. TGVCBot — `refs/TGVCBot/apps/tgvcbot/` (TypeScript, grammy/Bot API)

No sudo, owner, or admin model exists anywhere in the bot package (zero
`sudo`/`owner`/`OWNER` hits in `src/`). No player or settings keyboards exist:
playback is controlled by text commands only. Search is the only inline
surface. Banner generation is the thumbnail story.

### 6.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner / admin system | None: no sudo list, no add/remove commands, no owner gate, no admin check; any group member may run every command | (absent; zero sudo/owner/admin hits in `apps/tgvcbot/src/`) | N/A — ref has no sudo system at all; Parlay HAS owner/sudo gates, which is a deliberate hardening over this ref |

### 6.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing banner card | Caption: "Playing <linked title>" + duration line + "Requested by <user link>"; sent as a photo of a generated 600x300 banner (blurred cover background + 180x180 cover + rendered title/artist/watermark text) when generation succeeds, else the same text as a plain message + error logged to the log channel | `src/utils/messages.ts` (`sendPlayingMessage`), `src/utils/banner.ts` (`generateBanner`), `src/utils/text-to-image.ts` | PRESENT — Parlay photo + caption now-playing card with the same fields; the generated banner itself is MISSING (Phase 9 rule: no image generation in core; the track thumbnail photo replaces it) |
| Play from reply | `/play` / `/pl` (group only) requires replying to an audio file; uses the audio's own thumbnail file id or the env default thumbnail, performer or fallback name, message link as the track link | `src/handlers/play.ts` | PRESENT — Parlay replied-media play (`routing-table.md` C1) |
| Text-command controls | `/pause` (`/p`) / `/resume` (`/r`) / `/skip` (`/next`) / `/shuffle` / `/stopvc` / `/leave`, all plain-text replies ("Paused" / "Not Playing", "Resumed" / "Not Paused", "Shuffled the playlist" / "No Playlist to shuffle", "Stopped"); controls pass through an inactive-VC middleware | `src/handlers/controls.ts`, `src/handlers/leave.ts:16`, `src/middlewares/inactiveVc.ts` | PRESENT — Parlay text-command controls + state replies (`routing-table.md` §C); Parlay adds inline-button controls this ref lacks |
| Inactive-VC / group-only denials | Middleware replies "This Command works on Group Only" in private chat and "Inactive VC" when no call is connected, and stops the chain (visible replies, not silent) | `src/middlewares/inactiveVc.ts` | PRESENT — Parlay equivalents ("Not streaming." / group-only replies); Parlay wording differs |
| Queue display | `/queue` / `/q`: bold-italic "Queue List :" header + numbered linked titles + "Requested by" user links, no durations, no buttons, no pagination | `src/handlers/queue.ts` | PRESENT — Parlay queue card (richer: durations, delete buttons, show-more) |
| Queue delete | Dead code: a `/delete`/`/remove`/`/d` handler exists but is fully commented out (example text "/del 2", "Invalid queue position", "Deleted … from queue" never run) | `src/handlers/queue.ts` (commented block) | N/A — ref's delete is dead code, not a component; Parlay HAS working per-track delete buttons (`pl:queue:del`) |
| Search-to-play | `/jiosaavn` / `/jsvn` plays the first search result immediately; `/ytsr`/`/ytsearch` equivalent for YouTube (first result, no list) | `src/handlers/jiosaavn.ts`, `src/handlers/youtube.ts` (first-result branches) | PRESENT — Parlay `/play` first-result behaviour |
| Progress display | None: no progress bar, no `/position`, no position line anywhere | (absent; no progress/position code in `src/`) | N/A — ref has none; Parlay HAS a progress-bar button + `/position` |
| Play history UI | None | (absent) | N/A — ref has none; Parlay also has none |

### 6.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start URL button | Single "View Source" URL button on the `/start` card | `src/handlers/start.ts` | PRESENT — Parlay start card URL rows |
| JioSaavn search keyboard | Number buttons 1-10, 5 per row, callback `jsvn:<requester_id>:<song_id>`; wrong-user taps get the alert toast "You aren't allowed"; valid tap deletes the search message and streams the pick | `src/handlers/jiosaavn.ts` (search + callbackQuery branches) | PRESENT — Parlay `pl:search:<n>:<user>:<id>` (same shape); the exact foreign-tap wording for Parlay search was not re-verified in this audit, so the toast text itself is UNCERTAIN |
| YouTube search keyboard | Same pattern as JioSaavn with `yt:<requester_id>:<video_id>` callbacks and the same wrong-user alert; result lines add `(duration)` after the title | `src/handlers/youtube.ts` | PRESENT — same Parlay search-pick mechanism; toast-text caveat as above |
| Player / settings keyboards | None: no inline controls, no settings panel, no language picker | (absent) | N/A — ref has none; Parlay HAS all three |
| Reply keyboards | None | (absent) | N/A — ref has none; Parlay reply-keyboard support is send-side only |

### 6.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Generated banner | 600x300 image: source cover blurred as background, 180x180 cover composited, title (26pt) + artist + watermark text rendered as images and composited on top; produced with sharp; failure falls back to the text card | `src/utils/banner.ts` (`generateBanner`), `src/utils/text-to-image.ts` (`getImage`) | MISSING — Parlay deliberately does not generate banners (Phase 9 rule); it sends the track thumbnail as the photo instead |
| Banner fallback | On any banner-generation error, the plain-text now-playing message is sent and the error is logged (to the log channel if configured) | `src/utils/messages.ts` (`sendPlayingMessage` catch branch) | PRESENT — Parlay sendPhoto failure falls back to a text card (`dispatcher.cpp:1691`) |
| Reply-audio thumbnail | Replied-audio play uses the audio message's own thumbnail file id, or the env `THUMBNAIL` default when the audio has none | `src/handlers/play.ts` (`image: audio.thumb?.file_id ?? env.THUMBNAIL`) | PRESENT — Parlay uses audio title/performer/thumbnail on replied-media play (`dispatcher.cpp:6730-6746`); a configured default thumbnail is MISSING in Parlay |
| Thumbnail toggle | None: no per-chat thumbnail setting | (absent) | N/A — ref has none; Parlay HAS `/thumbnail` + settings row |

### 6.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Playing caption template | "Playing <linked title>" + arrow-prefixed Duration line + "Requested by <user link>" (HTML, escaped titles) | `src/utils/messages.ts` (`sendPlayingMessage`) | PRESENT — Parlay now-playing caption, same fields |
| Queue list template | Bold-italic header + numbered linked titles + requester links, web-page preview disabled | `src/handlers/queue.ts` | PRESENT — Parlay queue card (adds durations, delete buttons) |
| Help template | Flat bold-headed command list (no categories, no buttons): jiosaavn, play-from-reply, queue, pause, resume, skip, stopvc, leave, help; notably omits shuffle and radio | `src/handlers/help.ts` | PRESENT — Parlay help menu supersedes it (categorised topics, complete command list) |
| Search result lines | Numbered lines `NN : <linked title>` + "By : singers" (JioSaavn) or title + `(duration)` + "By : artist" (YouTube); header "Search Results for <query>" | `src/handlers/jiosaavn.ts`, `src/handlers/youtube.ts` | PRESENT — Parlay search-result lists (same numbering + artist lines) |
| Failure / guard texts | "No Results Found", "Please Provide Search Keyword", "This Command works on Group Only", "Inactive VC", "Queue is empty", "Failed to stream the song" + backticked error (MarkdownV2) | `src/handlers/*.ts`, `src/utils/messages.ts` (`sendFailedToStreamMessage`) | PRESENT — Parlay equivalents (wording differs; Parlay failure texts are HTML, not MarkdownV2) |
| Copy pills / hints / footers / collapsibles | None: no code spans, no blockquotes, no expandable sections, no hints, no footers | (absent across `src/handlers/` and `src/utils/messages.ts`) | N/A — ref has none of these; Parlay HAS code pills and expandable lyric captions |

### 6.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | grammy `InlineKeyboard`: number buttons with colon callbacks `jsvn:<user>:<id>` / `yt:<user>:<id>`; callback routing by regex (`/^jsvn:\d+:[a-zA-Z0-9.\-_]/` and the yt equivalent); requester check inside the handler with an alert toast on mismatch; search message deleted on a valid pick | `src/handlers/jiosaavn.ts`, `src/handlers/youtube.ts` | PRESENT — Parlay `pl:search:<n>:<user>:<id>` with the same requester-gate + delete-on-pick pattern |

### 6.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown command | Silent drop: grammy only invokes registered composers; an unregistered `/command` matches no composer and gets no reply (no catch-all middleware exists in `bot.ts` / `handlers/index.ts`) | `src/bot.ts`, `src/handlers/index.ts` (`InitHandlers`) | MISSING — Parlay answers with the unknown-command card (`dispatcher.cpp:1008-1013`) |
| Unknown callback | Silent drop: grammy only invokes the two registered callbackQuery regexes; any other callback data matches nothing and is never answered | `src/handlers/jiosaavn.ts`, `src/handlers/youtube.ts` (only two `callbackQuery` registrations in the package) | MISSING — Parlay answers unmatched callbacks with the `Unknown action.` alert |

---

## 7. Telegram-Song-Downloader-Bot — `refs/Telegram-Song-Downloader-Bot/` (Python, telepotpro; `bot.py` 144 lines + `text_msg.py` 13 lines)

Every incoming message is funnelled through one class (`Chat.check_input`).
There are no buttons, no callbacks, no sudo, no VC, no queue, no thumbnails,
no help command. All text is Markdown. The bot token is loaded from the
environment; no credential value appears in this inventory.

### 7.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Sudo / owner / admin system | None: no owner list, no gates, no admin concept; every message is processed for every user | (absent; `bot.py` has no auth code) | N/A — ref has no sudo system; Parlay HAS owner/sudo gates |

### 7.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Now-playing / queue / controls | None: no VC playback, no queue, no controls of any kind | (absent) | N/A — ref has none; Parlay HAS all three |
| Song request flow | `/song <name>` or `/song <musician - song name>`: searches YouTube (top 1 result), replies "title + link" lines, then a "Downloading… (this may take a while.)" message, downloads best-audio as MP3 (256k), sends the audio, deletes the downloading message, sends a confirmation message, deletes the local file | `bot.py:95-119` (`process_request`), `bot.py:23-55` (`song` class) | PRESENT — Parlay download flow (search → status → file → confirmation); Parlay adds progress edits, cancel, and size/quality handling this ref lacks |
| Over-30-minute guard | Defined message "Video too long to convert! Order something 30 minutes or less." exists in `text_msg.py` but is NEVER sent: `process_request` has no else branch, so an over-long video silently does nothing (no reply, no error) | `text_msg.py` (`TOO_LONG_ERROR_TEXT_MSG`), `bot.py:103-119` (missing else) | PRESENT — Parlay enforces a duration limit with an explicit reply (`Sorry, song exceeds max duration of N minutes.`); Parlay replies where this ref silently stalls |
| Spotify-input guard | A `/song` argument containing a Spotify link gets the "bot does not support Spotify links" reply with retry-format lines | `bot.py:121-133` (`check_input` Spotify branch), `text_msg.py` (`SPOTIFY_INPUT_ERROR_TEXT_MSG`) | PRESENT — Parlay routes Spotify links to its Spotify downloader instead of refusing them (opposite handling, same trigger) |

### 7.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Any button surface | None: no inline keyboards, no reply keyboards, no callback handlers anywhere in the bot | (absent; zero keyboard/callback hits in `bot.py` and `text_msg.py`) | N/A — ref has no buttons; Parlay HAS full inline surfaces |

### 7.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Audio thumbnail | None: `sendAudio` is called with only the file; no thumbnail parameter, no cover download | `bot.py:90-93` (`send_audio`) | MISSING — Parlay passes thumbnails on audio delivery; this ref sends bare audio |
| Thumbnail generation | None | (absent) | N/A — ref generates nothing; Parlay likewise generates nothing |

### 7.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start text | Markdown greeting: what the bot does + `/song` format lines + four identical "ProTips" links (Learn / Rate / Get help / Give feedback, all pointing at the same assistant bot) | `text_msg.py` (`START_TEXT_MSG`), `bot.py:121-124` (`/start` branch) | PRESENT — Parlay start card (HTML, richer, config URLs instead of hardcoded ProTips links) |
| Invalid-command text | Markdown "Oops! Invalid command!" + the two `/song` format lines + the same four ProTips links; sent for EVERY message that is not `/start` or a valid `/song` (see §7.7) | `text_msg.py` (`INVALID_COMMAND_ERROR_TEXT_MSG`), `bot.py:134-136` (else branch) | PRESENT — Parlay unknown-command card (same role, different text, no ProTips links) |
| Found / downloading / confirmation texts | Found line: musical-note + title + link lines; downloading line: "Downloading… (this may take a while.)" in italics; confirmation message after a successful send (and, by a bare-except bug, also after a failed send) | `bot.py:107-116`, `text_msg.py` (`CONFIRMATION_TEXT_MSG`) | PRESENT — Parlay download status lifecycle (richer: progress, speed, ETA, cancel) |
| Copy pills / hints / footers / collapsibles | None beyond Markdown italics/bold; no code spans, no blockquotes, no expandable sections, no footers | `text_msg.py` (full file, 13 lines) | N/A — ref has none of these; Parlay HAS code pills and expandable captions |

### 7.6 Rich inline buttons (general mechanism; deep dive is NoxBeats §3.6)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Inline-button construction | None: the bot uses telepotpro `sendMessage`/`sendAudio`/`deleteMessage` only, with no `reply_markup` argument anywhere | `bot.py:80-93` | N/A — ref has no inline buttons; Parlay HAS the full `pl:` button system |

### 7.7 Silent denials for unknown commands

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Unknown / invalid command | NOT silent: the else branch of `check_input` replies the invalid-command text for any message that is not `/start` or a valid `/song` — this is the only ref in the group that answers unknown input with a dedicated error message rather than dropping it or showing a command list | `bot.py:121-136` (`check_input`) | PRESENT — Parlay likewise answers (unknown-command card + available-commands list); Parlay's answer is more helpful (a list, not just the `/song` format) |
| Over-long video (silent stall) | Silent: see §7.2 — an over-30-minute result produces no reply at all, despite a dedicated message string existing for it | `bot.py:103-119` | MISSING — Parlay replies explicitly on duration-limit refusal instead of stalling silently |

---

## Summary (computed from the tables above)

Per-category counts use: Inventoried = rows in the category tables (including
N/A rows). P/M/U count only rows whose verdict is PRESENT / MISSING /
UNCERTAIN. N/A rows are listed separately and excluded from P/M/U.

| Category | Inventoried | PRESENT | MISSING | UNCERTAIN | N/A |
|---|---|---|---|---|---|
| Sudos | 28 | 18 | 2 | 1 | 7 |
| Plays | 50 | 36 | 0 | 1 | 13 |
| Buttons | 47 | 35 | 3 | 1 | 8 |
| Thumbnails | 28 | 13 | 6 | 0 | 9 |
| Rich messages | 37 | 30 | 5 | 0 | 2 |
| Rich inline buttons | 18 | 14 | 3 | 0 | 1 |
| Silent denials | 14 | 4 | 10 | 0 | 0 |
| TOTAL | 222 | 150 | 29 | 3 | 40 |

How to read the MISSING column: most MISSING verdicts are deliberate
Parlay divergences, not gaps to fill blindly — Parlay answers unknown
commands/callbacks instead of dropping them (10 of the 14 Silent-denial rows
are MISSING for exactly that reason), never truncates or base64-encodes
callbacks, never generates banner images, and has no login flow. Genuine
candidate gaps surfaced by this audit: NoxBeats `copy_text` buttons,
NoxBeats default-thumbnail fallback, NoxBeats queue-card photo variant,
ParvatiMusic local artwork cache, TGVCBot generated banner (deliberately
excluded by Phase 9), Music163bot-Go provider link button and login flow,
and the Parvati/Arc `{emoji:…}` token style (not applicable to Parlay's
direct-text strings).

## What could not be found or verified

- ArcMusic bootstrap file: the gogram client registration file under
  `cmd/app/` was not located in this audit; Arc's unknown-command behaviour
  (silent drop) is inferred from the complete pattern table in
  `internal/modules/handlers.go` plus `internal/modules/cb.go`, which contain
  no catch-all handler. No Arc UI component depends on the bootstrap file.
- ParvatiMusic play-from-reply: the reply branch in
  `bot/src/modules/play.rs:239-246` was confirmed to exist, but whether it
  covers audio/video files (vs links) was not fully traced — verdict
  UNCERTAIN on that row.
- Spotify-Loader owner-only private mode: Parlay's owner/sudo gates are
  confirmed, but an exact env-list owner-only mode equivalent to
  `OWNER_IDS` was not verified in `dispatcher.cpp` — verdict UNCERTAIN.
- TGVCBot search foreign-tap toast: the ref's "You aren't allowed" alert is
  confirmed; Parlay's search-specific foreign-tap wording was not re-verified
  (Parlay's requester-gate pattern itself is confirmed from the vote/search
  router rows) — verdict UNCERTAIN on the toast text only.
- Music163bot-Go guest keyboard: Parlay inline results exist via `vc-ts`,
  but an equivalent "get this song in the bot" button on inline results was
  not verified — verdict UNCERTAIN.
- No web search was required: every suspected component was either found in
  the local clone or its absence was established from the ref's own router
  tables (which enumerate all handled commands/callbacks). No component was
  declared missing from a local miss alone.
