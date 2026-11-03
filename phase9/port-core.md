# Parlay Phase 9 — core-cpp port report

**Worker:** core-cpp port worker · **Date:** 2026-11-03 · **Scope:** `services/core-cpp/**`,
`contracts/ipc.json`, `COMMANDS.md` · **Status:** DONE, uncommitted, NOT pushed
(no push, no `.env` touch, core never started with the user token).

The engines worker's uncommitted changes in `services/downloaders`,
`services/netease-go`, `services/queue-rust`, `services/radio-js`,
`services/share-rust`, `services/spotify-rust`, `services/vc-go`,
`services/vc-rust`, `services/vc-ts` were not touched.

## What changed per file

### `services/core-cpp/src/telegram_api.hpp` / `.cpp` — interactive layer
- `InlineButton` (text + exactly one of callback_data/url/switch_inline_query;
  factories `callback`/`link`/`switchInline`), `InlineKeyboard`,
  `inlineKeyboardJson()`; `ReplyKeyboard` + `replyKeyboardJson()`,
  `removeKeyboardJson()`.
- `TgCallbackQuery`, `TgChatMemberUpdate`; `TgUpdate` gains `callback` +
  `has_callback_query`, `has_my_chat_member`.
- New virtuals: `sendMessageHtml` (parse_mode HTML), `sendPhoto`,
  `editMessageText`, `editMessageCaption`, `editMessageReplyMarkup`
  (null markup sends an empty inline keyboard), `answerCallbackQuery`
  (alert + cache time), `deleteMessage` made virtual; `getUserMention`,
  `getChatId`, `leaveChat` made virtual (the test Fake overrides them).
- `getUpdates`: `allowed_updates` now
  `["message", "callback_query", "my_chat_member"]`; parses both new update
  kinds. `scanMedia` gains `file_unique_id`; `TgMessage` gains
  `media_file_unique_id` / `reply_media_file_unique_id` (§22 share feedback),
  `media_kind`, `media_title`, `reply_media_kind`, `reply_media_title`.
- `sendAudioFile` (multipart sendAudio, content-type by extension),
  `sendAudio` by file_id (JSON + keyboard), `sendFile` (multipart
  sendDocument from disk, zip→application/zip, lrc/txt→text/plain).
  Existing `sendDocument` (text content only) is unchanged — it is for
  errors.txt, not audio/binary.

### `services/core-cpp/src/config.hpp` / `.cpp`
- New fields: `start_img` (START_IMG), `ping_img` (PING_IMG),
  `updates_url` (PARLAY_UPDATES_URL), `support_url` (PARLAY_SUPPORT_URL,
  falls back to `https://t.me/<SUPPORT_CHAT>` with `@` stripped),
  `source_url` (PARLAY_SOURCE_URL). Empty = button/row omitted (source) or
  text-only card (images). There is NO private-mode / ALLOWED_USERS config
  and no `bot_username` in Config (the dispatcher keeps `bot_username_`).

### `services/core-cpp/src/ui_texts.hpp` — NEW, 3,212 lines
Namespace `parlay::ui_texts`. One constant per string, `{name}`
placeholders, Bot API HTML, English only, no small-caps Unicode, typos
corrected per inventory §27. Contents: helpers (`htmlEscape` — escapes only
`&`, `<`, `>`; `fmt`; `truncUtf8` — appends `…` when cut; `mmss`;
`durationStr` — 0 → "∞"; `progressBar` 9 stages; `progressLabel`;
`trackToken` via `scram::sha256`; `queryHash` FNV-1a 12 hex), callback
builders (`cbHelp`, `cbPlayer`, `cbPlayerToken`, `cbQueueMore`, `cbQueueDel`,
`cbSet`, `cbSetInfo`, `cbSetTimer`, `cbAuthDel`, `cbSearchPick`,
`cbSearchPage`, `cbDlCancel`, `cbDlFx`, `cbVote`, `cbDel`, `cbLossy`,
`cbRestart`; `kCbStart`, `kCbClose`, `kCbHelpMain`, `kCbLangSelect`,
`kCbLangEn`, `kCbBcastCancel`, `kCbSetMain`), and the §2–§25 string
families (start, help menu + 12 topics + per-command table `kCmdHelpTable`
+ `findCmdHelp` + `perCommandHelpText`, now-playing, queue, lyrics, search,
download, settings, admin/sudo, auth, errors, reply keyboards, stats/ping/
alive, playlists, broadcast, maintenance, channel-play, radio, share,
voting, core-routed).
- This round: `kCbLangEn` is now used by `langPickerKeyboard` (was
  hardcoded); `helpTopicText`/`helpTopicTitle` gained the `"search"` topic
  (`kSearchHelp` moved above the lookup so it is in scope); the settings
  panel appends the settings-index row (Pagination + Music Download) via
  `settingsIndexKeyboard()` — the panel is now 14 rows (was 13).

### `services/core-cpp/src/dispatcher.hpp` / `.cpp` — 7,482 lines
- Reply helpers `replyHtml`/`replyKb`/`replyReplyKb`/`replyPhoto`/
  `answerCb`/`editCbCard`/`editCbKeyboard`; `sendNowPlayingCard` (photo
  with caption when the track has a thumbnail and thumbnails are on, text
  fallback; records `card:<chat>` = `photo|<id>` / `text|<id>`);
  `renderQueueCard`/`buildQueueCard`; `playerAction` (shared by the /menu
  panel and the card buttons; on success `error` holds the toast text);
  `refreshCard`/`refreshPanel`; `resolveQueueToken`; search session
  helpers; `searchResultsText`/`searchResultsKeyboard` (shared by the
  command and the callback paths).
- `handleCallback`: rejects empty or >64-byte data with alert
  `kToastUnknownAction`; splits on `:`; dispatches by domain
  (help/start/close/player/queue/set/lang/dl/bcast/auth/search/vote/share/
  del/lossy/restart/dlset/spotify); try/catch → alert
  `kToastFeatureUnavailable`, never a crash. Token verbs
  (`pl:player:playnow|addlist:<token>`) use `cq.chat_id` — `cbPlayerToken`
  emits no chat id. Panel presses are requester-only and silently ignored
  for others; card presses are admin-or-requester (`callbackCanControl`)
  else toast `kToastNotAllowed`. Stop asks for confirmation only when
  paused or muted; direct stop when playing normally.
- `handleMyChatMember`: demotion goodbye — only old=administrator →
  new=member for the bot: replies `kDemoted`, leaves the chat.
- `handleMessage`: bot-added / owner-join / sudo-join cards, video-chat
  start/end, `delete:<path>` confirm trigger (with path validation — see
  below), DM start card, pending-state flows (download-settings template
  prompt, share feedback, pending share target, reply-a-number search
  pick), maintenance gate, unknown-command list.
- Command handlers reworked to the inventory templates: `onStart` (deep
  links `help`/`pm_help`, `share_<token>` and `share-<token>`, PM photo
  when START_IMG is set), `onHelp` (group → DM prompt, topic pages,
  per-command help), `onPlay` (queued detection via `queued` or
  same-track-id, queued card with Play Now, photo fallback),
  `onPause`/`onResume`/`onSkip`/`onAutoplaySkip` (state lines via
  `kCtlPaused`/`kCtlResumed`/`kCtlSkipped`), `onStop`, `onQueue` (+
  `/queue help` → `kQueueHelpOps`), `onPlayer`, `onLoop`, `onSeek`
  (pinned text kept), `onClear` (0 cleared → `kQueueAlreadyEmpty`),
  `onRestore`, `onRemove` (single / `1-5` range / `1,3,5` list, highest
  first), `onMove`, `onShuffle`, `onPos`, `onJump` (kept), `onMute`,
  `onReplay`, `onJoin`, `onDownload` (progress message, Spotify picker,
  lossy prompt, FX prompt; timeout 60→120 s), `onLyrics` (current track
  via `vc.lyrics`; named song → NetEase → `.lrc` file via `sendFile`),
  `onSearch` (50-char cap, 5 s per-user cooldown, 10/page, `ne:` prefix,
  shared render helpers), `onRadio` (+ `onNewRadio`/`onDestroyRadio`/
  `onManageRadio`), `onShare` (stores `sharetok:<token>` + `sharelink`),
  `onEngine`, personal + named playlists, settings split (`onSettings`
  panel vs `onPlaymode` command), `onAdminmode`, `onCleanmode`,
  `onThumb`, `onAutoplay`, new commands `onMenu`/`onId`/`onKeyboard`/
  `onAbout`/`onStatus`/`onNow`/`onRepo`/`onDownloadSettings`, `onPing`
  (card + PING_IMG photo), `onStats` (card), `onAuth`/`onAuthlist`
  (20-limit display), `onBlacklist`/`onBlacklisted`, `onAddSudo`/
  `onListSudo`, `onBroadcast` (pinned two-message flow + cancel keyboard
  via `editMessageReplyMarkup`, `bcast_running_`/`bcast_cancel_` flags),
  `onMaintenance`, `onSpeedtest` (staged notices; `formatSpeedtest`
  byte-identical, pinned), `onBug`, `onPrivacy`, `onRestart` (confirm
  card when a song is playing), RTMP handlers, `onEval`, `onLogs`,
  `onLogger`, `onAdmincache`, `onActiveVc`, `onChannelplay`, `onUserbot`.
- `registerCommands`: `help` scope Private → Any; `settings` split;
  new routes (menu/id/keyboard/about/status/now/repo/download-settings,
  named playlists, radio lifecycle); alias fix `fvcpay` → `fvplay`.
- This round's wiring (previously unreferenced inventory strings now
  used — 52 consts + 12 helpers):
  - Denials: `adminCheck` → `kDenyAdmin`, `canManageVc` →
    `kDenyAdminOrAuth`, `playAllowed` → `kGuardPlayAdmin`.
  - Control replies: `onPause`/`onResume`/`onSkip` state lines →
    `kCtlPaused`/`kCtlResumed`/`kCtlSkipped`; card skip/replay toasts →
    `kToastSkippedBy`/`kToastReplayedBy` (with the pressing user's name);
    `/clear` with 0 cleared → `kQueueAlreadyEmpty`.
  - Search expiry: `cbSearch` now edits the search message to
    `kSearchExpired` on session-missing, token-mismatch, and expired
    pick (in addition to the toast).
  - Settings sub-screens: `cbSet` handles `pl:set:page` /
    `pl:set:musicdl` (bare tap shows `kSettingsPageScreen` /
    `kSettingsMusicDlScreen` + `settingsToggleKeyboard`; `:0`/`:1`
    persists via the already-existing `page_purge`/`musicdl` fields in
    `saveChatSettings`); the panel links them via the index row.
  - Channel-play: bare → `cplayStatusText`; disable →
    `kCplayDisabled`; resolve/invalid → `kCplayResolveFail` /
    `kCplayInvalidTarget`; inaccessible / bot-not-admin →
    `kCplayNotAccessible`; linker-not-admin → `kCplayOwnerRequired`;
    success → `cplayEnabledText`. `onUserbot` via-channel no-link →
    `kCplayNotLinked`; member assistant → `userbotAlreadyText`;
    kicked/restricted assistant → `assistantRestrictedText`.
  - RTMP: `/setrtmp` is DM-gated (`kRtmpDmOnly` — the router already
    enforces Scope::Private; the handler gate is defence in depth);
    bare-empty → `kRtmpNotSet`; `off` clears the target →
    `kRtmpRemoved`; non-rtmp:// URL → `kRtmpInvalidUrl`; save →
    `kRtmpSaved`. `/stream`: no target → `kRtmpNotSet`; no query +
    nothing playing → `kRtmpStreamUsage`; already running →
    `kRtmpAlreadyRunning`; ffmpeg spawn failure → `kRtmpStartFailed`;
    success → `rtmpStreamStartedText` + `kRtmpDelayNote`. The started
    query is now stored in `RtmpStream.what` (new field in
    `dispatcher.hpp`). `/streamstop`: none → `kRtmpNone`; success →
    `rtmpStoppedText`. `/streamstatus`: none → `kRtmpNoneActive`;
    running → `rtmpStatusText` + pid + scrubbed, HTML-escaped log tail.
  - Misc: `onEval` empty → `kEvalNoInput`, output via
    `evalOutputText` (HTML-escaped) with `kEvalError` prefix on
    non-zero exit; `onLogs` empty → `kLogsNone`, configured-but-missing
    file → `kLogsMissing`; `onLogger` → `kLoggerUsage` (with status),
    `kLoggerNeedId` (enable with LOGGER_ID unset), `kLoggerEnabled` /
    `kLoggerDisabled`, accepts `enable`/`disable`; `onAdmincache` →
    `kReloadShortOk` + `reloadCooldownText(mmss(remaining))`;
    `onActiveVc` → `kActiveStreamsEmpty`, `activeChatsText` (`/ac`),
    `kActiveStreamsHeader` (list, titles HTML-escaped); `onBug` →
    `kBugUsage`, `kBugReport` (HTML, via `sendMessageHtml`),
    `kBugThanks`, `kBugFailed`; `/queue help` → `kQueueHelpOps`;
    `/myplaylists` first line → `kMyPlaylistsHeader`; share-prompt
    cancel button → `kShareCancel`; broadcast cancel landing →
    `kBcastCancelOk`; download-probe failure → `kSpeedtestFailed`;
    `delete:` trigger → `kDelInvalidPath` (`..`) and `kDelTopLevel`
    (no `/`); help `"search"` topic wired (`kSearchHelp`).

### `services/core-cpp/src/ipc.hpp`
- `"volume"` added to `isKnownVcAction` — `vc.volume` is a real contract
  action (POST /v1/volume, 0..200) but was missing from the fallback
  list. The /menu vol buttons need it.

### `services/core-cpp/src/main.cpp`
- Poll loop dispatches `callback_query` → `handleCallback` and
  `my_chat_member` → `handleMyChatMember`.

### `contracts/ipc.json` — v0.2.0 → **v0.3.0**, 51 → **56 actions**
- IPC-1: `NowPlaying` gains optional `is_paused`, `is_muted`,
  `loop_count`, `speed`, `autoplay_enabled`. Existing `position_sec`,
  `paused`, `loop`, `autoplay`, `volume` kept (engine-native names).
- IPC-2 NEW `vc.progress`: `GET /v1/progress?chat_id=` →
  `position_sec`, `duration_sec`, `is_paused`, `is_muted`,
  `loop_enabled`, `now: Track|null`, `next: Track|null`.
  Service vc-go | vc-ts | vc-rust. (vc-ts already implements it.)
- IPC-3: `Track` gains optional `duration_str`, `views` (integer|null),
  `uploader` (string|null), `upload_time` (string|null), `is_live`.
- IPC-4 NEW `download.progress`: `GET /v1/download/progress?run_id=` →
  `run_id`, `percentage`, `done_bytes`, `total_bytes` (integer|null),
  `speed_bytes_per_sec`, `eta_sec` (integer|null), `elapsed_sec`,
  `phase`, `per_track`, `lossy_available`, `lossy_summary`
  (string|null). Service downloaders. (Downloaders already implement it.)
- IPC-5: `vc.lyrics` + `netease.lyrics` gain optional `track_id`,
  `title`, `artist`, `source` (lrclib|netease|file-tag), `truncated`.
- IPC-6 NEW `chat.settings`: `GET /v1/chat/settings?chat_id=` |
  `POST /v1/chat/settings` (`chat_id`, `key`, `value`), response
  `chat_id`, `settings` object (admin_mode, play_mode, cmd_delete,
  clean_mode, clean_delay_min, thumbnails, autoplay, autoplay_lang,
  autoplay_mood, silent_mode, auto_delete, auto_delete_min, language,
  paginate_purge_old, chat_music_download). Service core-cpp.
- IPC-7 NEW `spotify.details` (`GET /v1/spotify/details?user_id=&track_id=`
  → track, features, genres, disliked_by, ignored_by, profanity,
  language, lyrics, lyrics_link) and `spotify.analyze`
  (`POST /v1/spotify/analyze` → analysis, profane_words).
  Service spotify-rust.
- Extra: `download.spotify_playlist` response gains optional
  `file_paths: string[]` (a real downloaders response field the core's
  files-mode delivery needs). `radio.status` response CORRECTED to the
  real radio-js shape (`now_playing`, `station`, `stream_url`,
  `reachable`, `vc_handoff`; `GET /v1/radio/status?chat_id=`) — the old
  shape was stale. A v0.3.0 note appended to `notes`.

### `COMMANDS.md`
- Header: Phase 9, 2026-11-03; dispatcher 7,482 lines, ui_texts 3,212
  lines. `/speed` 0.5–4.0 (single atempo note). `/remove` range/list
  syntax. `/player` card + `/menu` + `/now`. Named-playlist rows.
  `/search` (numbered buttons, pagination, reply-a-number, 5 s
  cooldown), `/download` (progress message, Spotify picker, lossy
  prompt, FX prompt), `/lyrics` (.lrc file), `/new`/`/destroy`/`/manage`
  radio, `/share` creator-mapping note. `/settings` panel (now 14 rows
  incl. the Pagination / Music Download index row) + `/download-settings`
  (prefs stored, not yet sent). `/auth` 20-limit + `/authlist` rich list.
  `/broadcast` cancel button. `/restart` confirm card. Tools: `/ping`
  card + PING_IMG, `/stats` card, `/id`, `/keyboard`, `/about`,
  `/status` (counts 0), `/repo`, `/speedtest` staged notices,
  `/start`/`/help` (group /help → DM prompt). New "Callback namespace
  (Phase 9)" section (`pl:<domain>:<verb>[:<arg>…]`, domains, gating).
  "Not yet" section extended (see below).

## Tests

### Core-cpp (all run this session, after the final edits)

| File | Result |
|---|---|
| `tests/dispatcher_test.cpp` | **195 PASS, ALL PASS** (was 81 at Phase 6, 151 before this round; +44 §K checks) |
| `tests/ui_test.cpp` (new) | **147 PASS, ALL PASS** (was 92; +55 checks incl. the 14-row settings panel + every newly wired template) |
| `tests/telegram_api_test.cpp` | **61 PASS, ALL PASS** |
| `tests/http_api_test.cpp` | **71 PASS** |
| `tests/router_test.cpp` | **20 PASS** |
| `tests/scram_test.cpp` | **40 checks, ALL PASS** (no-auth tests skipped — mongod down) |
| `tests/tdlib_test.cpp` | **44 PASS, ALL PASS** |
| `tests/store_test.cpp` | **2 failures, both environmental** (see below) |

**Core-cpp total: 578 passed, 0 code failures.** `parlay-core` rebuilt
via CMake (`cmake --build services/core-cpp/build/`) — clean, no warnings
from the full `g++ -Wall -Wextra` syntax check on dispatcher.cpp either.

- `store_test` 2 failures are pre-existing/environmental, not a
  regression: `FAIL mongo connect: cannot connect to mongodb at
  127.0.0.1:27099` and `FAIL store backend=mongo (got file)`; redis is
  also down (`SKIP redis tests (not up)`). `store.cpp`/`store.hpp` are
  untouched by this work. Phase 7 notes say store_test is 40/40 only
  against a live mongod/redis.
- Test lesson found this round (worth keeping): the dispatcher
  rate-limits to **1 command/s per chat, silent drop**
  (`rateLimited`, in-memory `last_cmd_`). The first version of the §K
  tests reused one Rig for several `handleMessage` calls on the same
  chat — every call after the first was silently dropped, which
  surfaced as missing replies and an out-of-bounds `replies[2]`
  segfault in the test itself (not in production code). The §K tests
  now use one Rig per `handleMessage` call (the file store writes
  through on every set, so cross-Rig state works when the next Rig is
  constructed after the set; the admincache cooldown map is static and
  survives a fresh Rig). Callback tests (`handleCallback`) are not
  rate-limited and share one Rig.
- `/logs` expectation corrected in the same pass: the command is appended
  to the in-memory log *before* dispatch, so `/logs` always shows at
  least itself — the test now asserts the memory-log path, not
  `kLogsNone`.

### Repo-wide (read-only runs; engines worker's tree untouched)
- `services/vc-ts`: **44/44 pass** (two runs; a first run showed one
  live-JioSaavn assertion diff — `jsvn:EnLspV_g` vs `jsvn:rjkrTnma` —
  a live-network flake, green on both re-runs).
- `services/radio-js`: **22/22 pass**.
- `services/downloaders` (Python unittest): **73 pass / 4 errors /
  8 skipped**. The 4 errors are all
  `ModuleNotFoundError: No module named 'Cryptodome'` (pycryptodome not
  installed in the sandbox) in the Deezer ARL tests — an environment
  gap, not code.
- Go (`services/netease-go`, `services/vc-go`) and Rust (`services/vc-rust`,
  `services/share-rust`, `services/spotify-rust`, `services/queue-rust`):
  **not runnable in the sandbox** — no `go` or `cargo` toolchain.
  Phase 7's 717 passed / 0 failed / 13 skipped therefore isn't directly
  comparable from here; the runnable subset is 562 (core-cpp) + 44 (vc-ts)
  + 22 (radio-js) + 73 (downloaders) = **701 passed**, plus the 2
  environmental store_test failures and 4 Cryptodome import errors.
- Fake extensions in `tests/dispatcher_test.cpp` (test-only):
  `member_status_for_other` on the Fake Telegram API (scripted
  kicked/restricted assistant), and a FakeIpc `queue` action (list →
  empty queue, clear → ok) so `/clear`'s empty-queue reply is testable.
  The queued-play path guards `size() > 1`, so the empty queue is safe
  there.

## Inventory items NOT implemented, and why

- **Delete execution** — the `delete:<path>` trigger shows the confirm
  card, but `pl:del:yes` answers `kToastFeatureUnavailable`: no
  `library.delete` IPC action exists. Needs an engines-worker action.
- **`/mix`, `/recognize`, `/scan`, `/sharescan`, `/rmcache`** — no IPC
  action exists for any of them; do not invent one.
- **Download cancel** — the cancel button is shown (ported UI) but no
  `download.cancel` IPC action exists in the contract or the
  downloaders service; taps get the honest `kToastFeatureUnavailable`.
  Needs an engines-worker action.
- **Live download progress polling is impossible by design** — the
  downloaders create the run server-side per POST and the run_id only
  rides back in the POST response, so the core cannot know the run_id
  while the transfer runs. The progress message is still edited in
  place (fetching → outcome); the final IPC-4 snapshot is fetched
  after the POST for the album checklist + lossy prompt trigger.
- **Spotify companion surfaces** (`/details`, `/analyze`, Magic/
  Recommendation, toggles) — spotify-rust owns them (engines worker);
  the core only owns `spotify.dislike` via `pl:spotify:dislike:<id>`.
  IPC-7 is contracted but the core does not render those cards.
- **Download format/youtube/template prefs** (`dlset:<user>`) — stored
  by the core's download-settings UI, but the contract's download
  actions take no format/youtube/template fields. Applying them needs
  a contract addition (engines worker).
- **Private-bot notice** — `kPrivateBotNotice` exists in ui_texts but
  no private-mode config exists in core-cpp, so `onStart` shows the
  normal card.
- **Auth-limit 20 (assumption)** — `authlistText` needs a limit
  denominator; no limit is enforced in current `onAuth` and no
  constant exists anywhere in code. 20 is NoxBeats' conventional cap,
  assumed, not verified.
- **Share-token semantics** — share-rust mints
  `https://t.me/<bot>?start=share_<token>` (underscore) and stores
  token→track ONLY, anonymous by design; the inventory's user-target
  send flow therefore cannot come from share-rust. The core stores
  `sharetok:<token>` = creator id at `/share` time and `sharelink:<user>`
  = link text; `/start share_<token>` (and the hyphen form) → sharetok
  lookup → send flow, else `share.resolve` GET → track →
  `kShareTrackOpened`, else `kShareInvalidLink`.
- **`/status` counts are 0** — the core keeps no song cache; honest zeros.
- **Radio `/manage` "—" fields** — radio-js doesn't expose listeners,
  loop, created, autoplay, queue over the status endpoint; host =
  `cfg_.radio_url`.
- **Search cooldown 5 s** — a Parlay choice; the inventory's "Too
  frequent" text gives no duration.
- **`kLyricsNoProgram` guard not implemented** — no reliable signal in
  the core for "a radio program is playing".
- **`kNamedPlaylistNotOwner` unreachable** — the named-playlist store is
  user-scoped (`npl:<user>`), so the owner check is structural.
- **`formatSpeedtest` deviation** — kept byte-identical (test-pinned)
  even though the inventory's `<b>Speed test</b>` result form conflicts.
- **`/speed` 4.00x** — single `atempo` covers 0.5–100.0 (verified in
  `voice_call.cpp`), so no chaining is needed.
- **Vote card creation** — no `/vote` command in the core; the vote card
  is created by queue-rust (§24, engines worker). The core only handles
  `pl:vote:*` callbacks from the stored voting chat, silently ignoring
  unknown/foreign ones per daslui.
- **Share media forward** — audio/voice via `sendAudio` by file_id +
  feedback buttons; video/video_note/document via `forwardMessage`
  (no send-by-file_id call exists for those; no feedback buttons on
  those).
- **`/menu` panel gating** — panel presses are requester-only and
  silently ignored for others; card presses are admin-or-requester.
- **`/setrtmp` form** — the core's form is per-chat
  `/setrtmp <url> <key>` (+ `off`); the inventory's `[chat id]` form
  (`kRtmpUsageDm`, `kRtmpConfiguredText`) is not used. The stream key
  is never printed (stored, scrubbed from the status log tail).
- **`/volume` stays NOT a core command** — reachable via the /menu panel
  and the card vol buttons (`vc.volume` IPC).

## Dead-string audit (final)

Re-audit after this round (counting references in `ui_texts.hpp` +
`dispatcher.cpp` + both test files): **245 of 497 consts and 70 of 199
helpers are still unreferenced** — down from 297/82 at the start of this
round (52 consts + 12 helpers wired). (Fresh re-run 2026-11-03
confirmed the dead counts 245/70; totals corrected 488→497 and
197→199 — the 9 extra consts + 2 extra helpers are all referenced.) The remainder fall into groups
that have no core call site to wire them to, and were deliberately left
in the catalogue rather than deleted (the inventory ported them; deleting
would lose the reference text):

- **Other services' surfaces** (engines worker owns the rendering):
  Spotify (`kSpotify*`, `spotifyLinkErrorText`, `spotifyStartKeyboard`,
  `kAdminSpotifyConnected`), Mix/Recognize/Scan/ShareScan/RmCache
  (`kMix*`, `kRecognize*`, `kScan*`, `kShareScanTriggered`,
  `shareScanScheduledText`, `kRmCacheNotCached`, `rmCacheOkText`),
  radio queue flow (`kRadio*` ~20), upload intake (`kUpload*`),
  dashboard (`kDash*`, `dashboardCardText`, `dashboardKeyboard`),
  vote card (`voteKeyboard`, `votePostText`, `voteRequestedText`,
  `voteTrackCard`, `kVoteBadId`, `kVoteFetchFail`), the engine
  error catalogue (`kErr*` ~40 — the core uses `replyError`),
  downloaders' progress lifecycle (`kDl*` variants, `kDlProgress` /
  `kDlProgressBytes` — no live progress by design), search lifecycle
  (`kSearchActive`, `kSearchCancelled`, …), broadcast progress
  (`kBcast*` — the two-message flow is test-pinned).
- **Defensive / unreachable-via-command**: `kLogsNone`,
  `kLogsMissing` (the current command is always in the memory log
  before dispatch); `kSettingsLoadFail` / `kSettingsSaveFail` /
  `kSettingUpdateFail` / `kSettingsOutdated` (the store layer doesn't
  surface write failures to the dispatcher); `kCplaySaveError` (same);
  `kReloadShortFail` / `kReloadCacheOk` / `kReloadCacheRule` /
  `kReloadCacheStart` / `kReloadDoneHeader` (admincache is a single
  reply); `kPingFirst` (no staging — the ping computes in ~200 ms);
  `kVcEnded` (duration variant; the core doesn't track call duration);
  `kAboutBuild` (needs build-time version/commit info the core doesn't
  embed); `kAuthBotUser` / `kSudoBotSelfJoke` / `kSudoBotUserJoke` (no
  bot detection — getChat doesn't return `is_bot`); `kAuthlistFetching`
  (synchronous store read); `kHelpOpeningToast` (cbHelp answers the
  back toast); `kLangChoose` / `kLangFailed` / `kLangNowEnglish`
  (English-only; the picker sets it silently); `kCplayMissingInvite`
  (no invite-permission check), `kCplayOwnerCheckFail`,
  `kCplayNotSet`, `kCplayUsage` (names `/setcplay`; Parlay's command
  is `/channelplay`).
- **Superseded by a wired form**: `kCardMuted` / `kCardPaused` /
  `kCardResumed` / `kCardUnmuted` (card state comes via the `kCtl*`
  state line), `kCtlStopped` (`/stop` replies `kStoppedCard`),
  `kCtlNotInCall` (the uniform test-pinned `"Nothing is playing."`
  inactive reply, exact-pinned for /mute, /autoplay-skip, /bass),
  `kToastReplayed` / `kToastReplay` (superseded by the `*By` forms),
  `kToastLoopOn` / `kToastLoopOff` (no loop button on the card — loop
  is `/loop` only), `kAskipDisabled` / `kAskipNone` / `askipSuccessText`
  (the engine decides; the test pins the "Autoplay skipped" wording),
  `kAutoplayNext` / `kNextDownloading` (no background watcher),
  `kQueueFetching` / `kQueueFinished` (sync fetch; the empty-queue
  reply is test-pinned), `kDelCancelled` (cancel deletes the message +
  toast), `kSettingsIndex` (the panel IS the index),
  `activeStreamsCountText` (near-duplicate of `activeChatsText`),
  `kRtmpGuide` / `kRtmpNotAdmin` / `kRtmpRefusal` / `kRtmpStatusCard` /
  `kRtmpUsageDm` / `rtmpConfiguredText` (RTMP form variants),
  `kDenySudo` / `kDenySupergroup` (the router fails Perm/Scope
  silently per NoxBeats), `kShareInvalidMessage` / `kShareHelp` /
  `kShareInvalidCommand` / `kShareServerError`, `kChooseTrack`,
  `kLinkAccepted`, `kFoundLine`, `kDlSizeLimit`, `kDownloadTooLong`,
  `kErrNoResultsPlain` / `kErrNoResultsTry`, `kPlaySearching` /
  `playSearchingForText`, `kToastAlreadyPlaying`,
  `kToastDelExpired` / `kToastDelNotAllowed`, `kToastDlCancelling`,
  `kToastNextDownloadFailed` / `kToastNextPlayFailed`,
  `kGuardUserNoPerms` / `kGuardUserNotAdmin` / `kGuardUserNotFound` /
  `kGuardPlayExpired` / `kGuardPlayUserInvalid` / `kGuardAutoLeft`,
  `kErrAssistant*`, `kUserbotReady` / `userbotJoinedText`,
  `kAdminNewUser`, `kBotAddedMaintenance`, `kSpamBlocked`,
  `kSupergroupGuide`, `kSupportedPlatforms`, `kCtlShuffleFew` /
  `kCtlShuffling` / `kCtlVolumeInvalid`, `kCleanModeHint`,
  `kInvalidBool`, `kBlChatBlocked` / `kBlChatOwnerBlocked` /
  `kBlUserBlocked` / `blFailedUnblockText`, `delChangedText` /
  `delDoneText` / `delNotFoundText` / `deleteConfirmAlbumText`,
  `dlCancelAlbumText` / `dlCancelledByText` /
  `dlCompleteProcessingText` / `dlCouldNotResolveText` /
  `dlResolveFailText`, `errDownloadFailedText` / `errFetchTrackText` /
  `errFileTooLargeText` / `errOccurredText` / `errSearchFailedText` /
  `errWithMessageText`, `errLine` / `okLine` / `infoLine`,
  `floodMinutesText` / `floodSecondsText`, `guardInviteErrorText`,
  `libraryStatsText`, `loggerBotJoinedText` / `loggerPlaybackText` /
  `loggerStartedText` / `logsCaptionText`, `lossyCancelledText`,
  `lossyExpired`, `playlistAddedManyText` / `playlistLinkQueueText` /
  `playlistLinkStreamText` / `playlistQueuedText` / `queueFullText` /
  `queueLimitText` / `queuedManyText` / `playAllTooLongText` /
  `playAlreadyQueuedText` / `playBannedText` / `playDownloadingText` /
  `playFailedText`, `playlistAdding` / `playlistFetchError` /
  `playlistFetching`, `radioLoopSetText` / `radioNowPlayingRawText` /
  `radioQueuedPlaylistText` / `radioQueuedVideoText` /
  `radioSearchingText`, `restartFailedText` / `restartNoticeText` /
  `kRestartOk` / `kRestarting`, `settingUpdatedText` (cbSet uses
  `kSettingsUpdated`), `ctlVolume` (vol buttons use
  `toastVolumeSet`), `kLyricsFetching` / `kLyricsNeedQuery`,
  `kMaintCardTitle` / `kMaintEnabledReason` / `kMaintEntering`,
  `kSearchLinkOutdated` / `kSearchNoActive` / `kSearchNothingFound` /
  `kSearchOnlyStarter` / `kSearchResultsOutdated`.

## Decisions carried over (not in the inventory)

- Language picker origin: `pl:lang:select` takes an optional origin,
  `pl:lang:en:<origin>` returns to that menu (start|help|settings).
- Settings timer row: timer buttons 2–7 as their own row after the
  "Delete after" label row; panel is now 14 rows with the index row.
- Stop confirm only when paused or muted; direct stop when playing.
- Lossy accept = deliver the stored lossy file (no re-encode needed);
  skip = drop it.
- Broadcast keeps its exact two-message plain-text flow (test-pinned);
  the cancel keyboard is attached via `editMessageReplyMarkup` only.
- `/menu` is a new admin-only command (remix §2.3 panel).
- Named playlists are Store-based, user-scoped (`npl:<user>`), 10 max.
- `kLinkAccepted` unused: links sent to /search go through the search
  path (vc-ts resolves them).
- `/setrtmp` DM gate added (inventory §20; the router already enforces
  Scope::Private — the handler gate is defence in depth). The stream
  key is never printed.
- `delete:` trigger validates the path (`..` → `kDelInvalidPath`,
  no `/` → `kDelTopLevel`) before showing the confirm card.
- Search-expiry edits the message to `kSearchExpired` in addition to
  the toast (button path only — the reply-a-number path does not
  reply on expiry, since a bare number with no session may be an
  ordinary message).

## Open items for the coordinator

1. `download.cancel` + `library.delete` IPC actions (engines worker).
2. Spotify companion surfaces (spotify-rust, engines worker).
3. Download format/youtube/template prefs need contract fields
   (engines worker).
4. Auth-limit 20 is an assumption — **resolved 2026-11-03** (late
   background grep from this port): ArcMusic, TinaMusic and
   YukkiMusic-Go all default `MAX_AUTH_USERS` to **25**
   (`app.json` value `"25"`; ArcMusic `config.go:115`
   `MaxAuthUsers = int(getInt64("MAX_AUTH_USERS", 25))`, enforced in
   `auth.go:72`); NoxBeats defines no such limit. The core now uses
   `kAuthMaxUsers = 25` (one `constexpr` in `dispatcher.cpp`, six
   call sites: enforcement, limit message, added-count, list
   truncation, both list denominators) with a §L boundary test
   (25 pre-filled → 26th refused with "(25)"; 24 pre-filled → 25th
   added, "Now 25/25").
5. Live verification pending for everything (pull + restart + real
   Telegram): the sandbox cannot exercise MTProto voice, callbacks
   from a real client, or the /play-via-Telegram path.

## Coordinator rulings on the engines worker's 11 gaps — applied 2026-11-03

Source: `phase9/port-engines-gaps.md`. Contract `contracts/ipc.json`
is now **v0.3.1** (56 actions). Dispatcher is 8,000 lines,
`ui_texts.hpp` 3,256 lines.

### Accepted contract additions (applied by the core worker)

- **(2) `search.inline` request gains optional `chat_id`** (`ChatId`).
  The core passes it for `np`/`s`/`l` only, taken from
  `last_chat:<user>` — the user's most recent group chat, recorded on
  group text/commands (`handleMessage`, before command parsing) and on
  callback presses (`handleCallback`, group ids are negative), and only
  written when the value changes (no disk write per message). An
  inline query carries no chat id, so this is the only signal
  available; when it is unknown the core answers `np`/`s`/`l` empty
  without calling the engine.
- **(4) `netease.lyrics` response gains `lrc_path`** (`string`, "" when
  the service could not write the file). `onLyrics` named-song path
  now sends the service-written file when `lrc_path` is present and
  non-empty (a real `.lrc` keeps its timestamps) and falls back to
  writing its own stripped copy under `<data_dir>/lyrics` otherwise.
  The caption stays the stripped lyric either way (§6: timestamps
  stripped in the caption, not the file).
- **(10) `run_id` (12-hex) on all seven download POST responses**
  (`download.youtube`, `download.spotify`, `download.spotify_playlist`,
  `download.deezer`, `download.tiktok`, `download.navidrome`,
  `download.soulseek`). The core already reads `run_id` from the POST
  response (`dlsrv:<run_id>`) and fetches the IPC-4 snapshot after the
  POST — that part is now contract-backed for all seven actions.
  **Live polling during the transfer is still NOT possible**: the
  downloaders `do_POST` (`services/downloaders/src/server.py`) runs
  the transfer synchronously in the request thread and attaches
  `run_id` only after the handler returns, so the core cannot know
  the run id while the transfer runs — despite the server comment
  claiming the run lets the core "poll it while the file transfers".
  The remaining step is an async download POST on the engines side
  (return `run_id` immediately, transfer in the background); the core
  will then be able to poll `GET /v1/download/progress?run_id=` and
  edit the progress message with `kDlProgress`. No core change is
  needed for that step beyond the polling loop itself.

### Core behaviour applied

- **(1) Inline `lib` / `del`: empty results.** The engine returns
  `[]` and the core renders exactly that — no library results, no
  delete buttons. Future work: needs a `library.search` /
  `library.delete` action or a Navidrome-backed endpoint.
- **(2)+(3) Inline queries are now handled by the core** (they were
  not handled at all before this round). New `TgInlineQuery` +
  `parseInlineQuery` + `answerInlineQuery` in `telegram_api.{hpp,cpp}`
  (`getUpdates` asks for `inline_query`; `main.cpp` dispatches to
  `Dispatcher::handleInlineQuery`). The core calls `search.inline`
  (vc-ts `POST /v1/inline`, `{query, user_id, chat_id?}`) and renders
  the `InlineQueryResult` objects itself from the returned Tracks —
  vc-ts returns data only (its `/v1/inline` comment assigns the
  rendering to the core). Modes mirror vc-ts `resolvers.js inlineMode`
  exactly (same order, UTF-8 char count for the <3-char hint rule):
  `np` → audio result from `stream_url` (cache 0); `s` → article
  `{artist} — {title}\n{share url}` where the share URL is the
  track's own `url` (no `share.create` call — that would mint a link
  naming the track, not a sender's share flow); `l` → the engine's
  current track, then `POST /v1/lyrics {chat_id}` for the text, then
  the article `Lyrics: {title}` / `{artist} — {first 100 chars}` /
  `{artist} — {title}\n\n{plain lyrics}` (timestamps stripped,
  truncated to 4000 chars + `\n...`, trailing blank lines trimmed);
  `help` → two articles sending `Parlay v0.2.0`; bare `search` →
  `Please enter a keyword`; empty/<3 chars → empty results + the hint
  button (`switch_pm_text`, `switch_pm_parameter=help`, cache 30);
  keyword search → one result per track (audio when `stream_url` is
  known, article otherwise) with the `[Open in Parlay Bot]` button,
  cache 3600; no matches → `No results found` article. Engine
  failure → empty results (never leave the query hanging).
  Blacklisted users and (under maintenance) non-sudo users get empty
  results, silently (§13 auth rule, same as chat auth).
- **Inline search rendering deviation (recorded, not a bug):** the
  inventory's "audio results" form is only possible when the engine
  supplies a direct audio URL. YouTube flat search and lemonic
  results carry `url` only (no `stream_url`); only JioSaavn carries
  `stream_url`. Tracks without `stream_url` therefore render as
  **article** results (title, `{artist} · {m:ss}` description, the
  track URL as the message) with the `[Open in Parlay Bot]` button as
  the functional path — a page URL as `audio_url` would be a broken
  result. Same fallback for `np` when the playing track has no
  `stream_url`. **And `np` is a plain audio result, not cached
  audio:** the inventory's cached-audio form needs a `file_id`, and
  the Track carries none (`room.current` has no file_id anywhere in
  vc-ts) — the core cannot build `InlineQueryResultCachedAudio`.
- **`/start spotify----{id}` deep link implemented** (the button on
  inline search results would otherwise be dead). Resolves via the
  stored inline results (`inlinesearch:<user>`, the raw Tracks JSON)
  first, else the id prefix (`spotify:` → Spotify track URL,
  `yt:` → YouTube watch URL; `lemonic:` ids cannot resolve without
  the stored result). Then downloads like `/download` (same
  link-type routing, 120 s POST, oversize → 50 MB message, file
  delivered via `sendAudioFile` with the Music163bot caption) and
  edits the progress message to the outcome. One-shot: no lossy
  prompt and no FX picker (those belong to the `/download` flow).
  The caption builder was extracted from `onDownload` into the
  shared `Dispatcher::audioCaption` helper (byte-identical output).
  Unresolvable ids answer `kDlDeepLinkExpired` ("That track link has
  expired — search for the song again.").
- **(5) Spotify `ignored_by`: stays 0, no Hide-alerts button.**
  The core renders no such button (verified: no `Hide` string in the
  core). Future work: needs a `spotify.ignore` action or a
  `spotify.dislike` extension.
- **(6)+(7) Spotify `language` / `features`:** the core renders no
  `/details` card at all (inventory §25 assigns it to spotify-rust;
  the core only owns `pl:spotify:dislike:<id>`), so there is no
  language row or features block in the core to hide. The rules are
  recorded here for the rendering owner: hide the language row when
  `language` is null; render the features block only when `features`
  is present, never with placeholder values.
- **(8) Share feedback/report dedup REMOVED.** `cbShare` feedback no
  longer checks `sharefb_done:` and the `handleMessage` feedback flow
  no longer checks or records it; `cbShare` report no longer checks
  or records `sharereported` (the `shareblocked` sadd stays — the
  report still blocks the sender, and the sadd is idempotent, so a
  second report just re-blocks and re-confirms). No "already sent" /
  "already reported" message is ever shown now. `kShareFeedbackAlready`
  and `kShareReportAlready` stay in `ui_texts.hpp` as the future-work
  catalogue for when dedup lands (no dispatcher reply uses them any
  more; the §L tests assert exactly that by comparing against them).
- **(9) Radio listener counts omitted from the card.** `kRadioManageCard`
  lost its two listener lines and `onManageRadio` no longer passes
  `listeners`/`totalListeners`. The other "—" fields (loop, created,
  autoplay, queue) are untouched — the ruling covers listener counts
  only. (The old form displayed "—", not 0; the ruling asks for
  omission, so the rows are gone entirely.)
- **(11) Granular progress: render what the payload gives.** No code
  change — the core renders no live percentage today (the progress
  message is edited fetching → outcome only; `kDlProgress` /
  `kDlProgressBytes` remain unwired). When live polling lands (see
  (10)), the snapshot's `percentage`/`speed`/`eta` will be rendered
  as-is, including 0 for phase-only providers — never estimated.

### New open items for the coordinator (from this round)

6. **`vc.lyrics` method mismatch: contract says GET, service is
   POST-only.** vc-ts `main.js` routes `/v1/lyrics` behind the
   POST-only gate (line 630 returns 404 for non-POST), and the
   handler reads `body.chat_id` / `body.title`. The core calls
   `POST /v1/lyrics {chat_id}` — the only form that works. Either
   the contract should say POST, or vc-ts should accept GET with a
   `chat_id` query param. Not changed by the core (the ruling
   accepted no change here).
7. **Inline `Song not cached` article not rendered.** The
   Music163bot row wants an article + a `Tap me to cache the song`
   button (`start_parameter={musicID}`) for uncached music, but no
   cache-on-tap flow exists in the core — the button would open a
   `/start` the core cannot act on. Empty searches render
   `No results found` instead.
8. **Inline `np` oversize:** the inventory says >50 MB tracks are
   "skipped and cached as failed for 1 h", but the Track carries no
   size, so the core cannot skip oversize tracks — Telegram fails
   the result itself if the audio URL is oversize.
9. **`/details` card buttons (`pl:spotify:analyze:<id>`,
   `pl:spotify:links:<id>`) still route to the core's
   `kToastFeatureUnavailable`.** The card is rendered by
   spotify-rust but its callbacks land in the core's router, and the
   core has no forwarding path for them (pre-existing gap, not one
   of the 11 rulings — noted here because rulings 5–7 touch the
   same card).

### Store keys added / removed this round

- Added: `last_chat:<user>` (most recent group chat, for inline
  `np`/`s`/`l`), `inlinesearch:<user>` (last inline search Tracks
  JSON, for the `spotify----` deep link).
- Removed: `sharefb_done:*`, `sharereported` (dedup gone, ruling 8).

### Tests this round

- `dispatcher_test.cpp` §L (31 checks): hint/help/bare-search
  articles, `np` with/without a known chat (chat_id asserted on the
  POST body), `np` engine-empty, `s` article, `l` lyrics fetch with
  the same chat_id + timestamp stripping + 4000-char truncation +
  404 → empty, search audio+article+Open-button + no-matches,
  `lib`/`del` empty, deep link via stored results + `yt:` fallback
  + unresolvable `lemonic:` id, `lrc_path` send + fallback to the
  core-written file, share feedback/report sent twice with no
  already-sent/already-reported, radio card with no Listener line,
  blacklisted + maintenance inline auth.
- `ui_test.cpp` §13 strings (12 checks) and `telegram_api_test.cpp`
  §H (12 checks: `parseInlineQuery`, `answerInlineQuery` body /
  switch_pm omission + presence).

### Test totals after this round

| Test file | Result |
|---|---|
| `tests/dispatcher_test.cpp` | **228 PASS, ALL PASS** (was 195; +31 §L, +2 auth-limit boundary) |
| `tests/ui_test.cpp` | **159 PASS, ALL PASS** (was 147; +12) |
| `tests/telegram_api_test.cpp` | **73 PASS, ALL PASS** (was 61; +12) |
| `tests/http_api_test.cpp` | **71 PASS** |
| `tests/router_test.cpp` | **20 PASS** |
| `tests/scram_test.cpp` | **40 checks, ALL PASS** |
| `tests/tdlib_test.cpp` | **44 PASS, ALL PASS** |
| `tests/store_test.cpp` | 2 failures, both environmental (mongo/redis down — pre-existing, store.cpp untouched) |

**Core-cpp total: 635 passed, 0 code failures.** `parlay-core`
rebuilt via CMake — clean. Repo-wide runnable subset: vc-ts **61/0**
(first run 60/1 — the known live-JioSaavn flake, green on re-run),
radio-js **22/22**, downloaders **73 pass / 4 errors / 8 skipped**
(the 4 errors are the pre-existing `Cryptodome` import gap).
Go/Rust toolchains still absent from the sandbox, so those suites
remain unrunnable here.

Correction (2026-11-03, fresh re-run): `tdlib_test` is **44 PASS**, not
28 as recorded in both tables above — the file is untouched since
Phase 7 (`git log` shows last change 9706d06), so 28 was a miscount;
the 617 total was also arithmetically short (rows sum to 619 with 28).
Both tables and totals are now corrected (578 / 635).
