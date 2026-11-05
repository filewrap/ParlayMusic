# Parlay Phase 10 — Consolidated Gap Analysis

**Scope:** analysis only. No code was modified, no branch was changed, no
push was made. Repo `~/workspace/parlay`, branch `feat/ui-complete-all-refs`
(left alone). Nothing under `refs/`, `.env`, secrets, or
`FINAL-ARCHITECTURE.md` was touched.
**Sources:** `phase10/inventory-group-a.md` (7 refs), `phase10/inventory-group-b.md`
(7 refs), `phase10/inventory-group-c.md` (7 refs), re-verified against the
current Parlay tree: `services/core-cpp/src/dispatcher.cpp`,
`services/core-cpp/src/ui_texts.hpp`, `services/core-cpp/src/dispatcher.hpp`,
`services/core-cpp/src/tg_types.hpp`, `services/core-cpp/src/main.cpp`,
`services/core-cpp/src/store.hpp`, `services/backend-go/internal/` (download,
library, recognize, search, spotify, voice), `contracts/routing-table.md`,
`contracts/ipc.json`, and `FINAL-ARCHITECTURE.md` §1.2, §7.1–§7.6.
**Filter applied to every item:** it goes to the PORT list only if it is
(a) a real UI component in a source ref, (b) missing in Parlay, and
(c) compatible with the locked architecture and the user's 7 categories
(Sudos, Plays, Buttons, Thumbnails, Rich messages, Rich inline buttons /
NoxBeats, Silent denials for unknown commands). Everything else is
NOT-APPLICABLE with a specific reason, or still UNCERTAIN with the exact
reason it could not be decided.
**No emojis anywhere in this file**, per the locked §7.3 rule.

## 0. The headline ruling (read first)

**User's ruling, 2026-11-05 ~12:07 IST (supersedes the original "zero
silent-drop ports" stance below):** unknown commands and unknown callbacks
are NOT silently dropped. (1) Unknown command: reply that the command
doesn't exist and point the user to /help (wording like "Command doesn't
exist, use /help", in the rich-message style, no emojis) — refine the
existing unknown-command card (routing-table row C92), do not build a
second card. (2) Unknown callback: answer the callback query with the
exact text "🖐️" (hand emoji — the user's own string, it overrides the
no-emoji rule for this one answer) and do NOT show an alert
(show_alert=false — a toast, not a popup). The locked §7.5 requirement
that every unknown callback is answered still stands; only the answer
content changes to this. Package 1 therefore builds S0 on this basis
(silent drop was implemented first under the old reading, then reworked
to this ruling).

Original stance (kept on the record, now superseded for category 7):
nearly every category-7 MISSING row in the three inventories is a ref
that silently drops unknown commands or unknown callbacks while Parlay
answers them. That divergence was read as locked, not a gap:
`FINAL-ARCHITECTURE.md` §7.5 requires zero unhandled callbacks and an
explicit default branch that answers every unknown callback (never
silence, never an un-answered query), and routing-table row C92 makes
the unknown-command card live ("never silent, never a crash"). The
user's ruling above keeps §7.5's "every unknown callback is answered"
and changes only what the answers say. The silent behaviours Parlay
already has (permission gates, blacklist refusal, panel-owner ignore,
inline silent-auth rule, rate-limit drop) are verified present, not
rebuilt. A second headline correction: routing-table §E claims /mix,
/recognize, /scan, /sharescan and /rmcache have "no backend action".
That is stale. All five backend actions already exist and are registered
(see §5, item 22); the gap is core command registration only, which is
why those five are cheap PORT items rather than new-backend work.

## 1. Consolidated counts

### 1.1 Raw inventory totals (as reported / tallied, before dedup)

| Group | Inventoried | PRESENT | MISSING | UNCERTAIN | N/A / other |
|---|---|---|---|---|---|
| A (ArcMusic, Music163bot-Go, NoxBeats, ParvatiMusic, Spotify-Loader, TGVCBot, Telegram-Song-Downloader-Bot) | 222 (file's own summary) | 150 | 29 | 3 | 40 |
| B (TgMusicBot, TinaMusic, YukkiMusic-Go, anonx-cpp, beetube-bot, feel-me, lemonic-telegram-bot) | 242 verdict rows tallied by this analysis (no summary table in the file; N/A rows excluded from the tally) | 194 | 26 | 20 | 2 rows whose verdict cell did not parse cleanly |
| C (music-bot-navidrome, openradiobot, remix, rustify, telegram-music-bot-daslui, telegram-music-bot-rust, wavetune) | 198 (file's own totals table) | 123 | 54 | 21 | 0 (N/A notes excluded from that table) |

Raw sums double-count the same component across refs and therefore do not
add up to the consolidated table. Do not use them as a work estimate.

### 1.2 Consolidated gap universe (deduped MISSING + UNCERTAIN rows only)

Each distinct component appears once, in the category of its inventory row.
A component counted in one category is cross-referenced, never double-counted,
from the others. PRESENT rows are not in this universe except where an
inventory verdict was corrected (those appear under "resolved PRESENT").

| Category | Gap universe | To-port | Resolved PRESENT (misverdict corrected) | Not-applicable | Still-uncertain |
|---|---|---|---|---|---|
| 7. Silent denials for unknown commands | 11 | 0 | 0 | 10 | 1 |
| 1. Sudos | 22 | 3 | 4 | 15 | 0 |
| 2. Plays | 41 | 8 | 14 | 18 | 1 |
| 3. Buttons | 18 | 3 | 3 | 11 | 1 |
| 4. Thumbnails | 13 | 1 | 2 | 10 | 0 |
| 5. Rich messages | 17 | 2 | 2 | 13 | 0 |
| 6. Rich inline buttons (NoxBeats mechanism) | 0 new (collapses into Buttons; see note) | 0 (shared item R1 counted under Buttons) | 0 | 0 | 0 |
| **Total (unique items)** | **122** | **17** | **25** | **77** | **3** |

Notes on the table:
- Category 6 has no separate count: every NoxBeats mechanism row is either
  already PRESENT in Parlay (toggle-state buttons, no-op label buttons,
  expired-button alert, role-based tapper gate, card-plus-keyboard pairing)
  or is the same item as a Buttons item (copy_text kind = R1; 64-byte
  truncation and base64/space grammars = not-applicable). Counting it
  separately would double-count.
- Package assignment does not follow inventory category mechanically:
  /scan and /sharescan are inventoried under Plays but assigned to package 1
  because they are sudo-gated admin tools; the Check-Again and Download-N-songs
  buttons are inventoried under Plays but assigned to package 3 because they
  are rich-inline button work. The category counts above keep the inventory
  category; the package lists in §2 keep the work theme.
- 17 + 25 + 77 + 3 = 122. The 17 PORT items are §2. The 25 resolved-PRESENT
  items are §5. The 77 NOT-APPLICABLE items are §3 (grouped by reason, each
  item named). The 3 UNCERTAIN items are §4.

## 2. PORT list (17 items, in 3 work packages)

Conventions for every item: "Routing" means a row (new or status change) in
`contracts/routing-table.md`; "IPC" means an action in `contracts/ipc.json`.
No PORT item needs a new IPC action: every backend action it needs already
exists, and the remaining items are core-only rendering or wiring. The
64-byte callback rule (§7.4) applies to every new callback below; track ids
travel as tokens, never as full ids. Tests are C++ tests unless stated:
`services/core-cpp/tests/dispatcher_test.cpp` (command/callback behaviour),
`services/core-cpp/tests/routing_test.cpp` (every callback routed and
64-byte-legal), `services/core-cpp/tests/ui_test.cpp` (card/keyboard text).
No Go test changes are needed; the reused backend actions are already
covered (e.g. `download/mix_test.go`, `download/registry_test.go`).

### Package 1 — Silent denials (ruling, no build) + sudo gaps (5 items)

Files touched: `services/core-cpp/src/dispatcher.cpp` (admin command block,
blacklist gate), `services/core-cpp/src/main.cpp` (updateChatMember
forwarding, package-1-only file), `services/core-cpp/src/ui_texts.hpp`
(reply texts for the new commands), `contracts/routing-table.md` (rows
C89–C91 status changes), `services/core-cpp/tests/dispatcher_test.cpp`,
`services/core-cpp/tests/routing_test.cpp`. No backend changes. No new IPC
actions.

- **S1. /rmcache command (Music163bot-Go).** What to build: register
  `/rmcache` (sudo-gated) and a handler that posts the existing
  `cache.clear` action and replies with the cleared result, or an honest
  not-configured reply if the backend refuses. Source:
  `refs/Music163bot-Go/internal/bot/app.go:184-193` (adminOnly `/rmcache`).
  Parlay destination: `services/core-cpp/src/dispatcher.cpp`,
  `registerCommands` (new `add({"rmcache"}, Scope::Any, Perm::Sudo, …)`)
  plus a new `onRmcache` handler. Routing: update row C91 (today "Phase 3")
  to Live. IPC: none new (`cache.clear`, registered in
  `services/backend-go/internal/library/library.go:274`, handler
  `services/backend-go/internal/library/delete.go:110`). Test: extend
  `dispatcher_test.cpp` (sudo-only gate, backend refusal answered honestly)
  and `routing_test.cpp` (row C91 routed).
- **S2. /scan command (music-bot-navidrome).** What to build: register
  `/scan` (sudo-gated) and a handler that posts the existing `library.scan`
  action and replies with the trigger result; when the library scan is not
  configured, reply the honest not-configured text, never a fake success.
  Source: `refs/music-bot-navidrome/src/bot.py:443-453,992-1027`
  (`_trigger_scan` status lines). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp`, `registerCommands` plus a new
  `onScan` handler. Routing: update row C89 to Live. IPC: none new
  (`library.scan`, registered in
  `services/backend-go/internal/library/library.go:272`). Test: extend
  `dispatcher_test.cpp` (sudo gate, unconfigured reply) and
  `routing_test.cpp` (row C89 routed).
- **S3. /sharescan command (music-bot-navidrome).** What to build: the
  same shape as S2 for the share rescan: register `/sharescan`
  (sudo-gated), post the existing `library.sharescan` action, reply with
  the trigger/scheduled result or the honest not-configured reply.
  Source: `refs/music-bot-navidrome/src/bot.py:443-453,992-1027`.
  Parlay destination: `services/core-cpp/src/dispatcher.cpp`,
  `registerCommands` plus a new `onShareScan` handler. Routing: update row
  C90 to Live. IPC: none new (`library.sharescan`, registered in
  `services/backend-go/internal/library/library.go:273`). Test: extend
  `dispatcher_test.cpp` and `routing_test.cpp` as in S2.
- **S4. Admin-list refresh on chat-member admin transitions (remix).**
  What to build: when a member's admin status changes (promoted, demoted,
  removed), refresh Parlay's cached admin list for that chat instead of
  waiting out the 10-minute TTL. Today the core only forwards the bot's
  own membership changes (`main.cpp:472` updateChatMember branch returns
  early for every other user), and the admin cache is a plain TTL cache
  (`dispatcher.cpp` `cachedAdmins`, `store_.cacheSet` with 600s TTL; the
  store exposes `cacheGet`/`cacheSet` only, `store.hpp:102-103`, no
  cache-delete). Implementation shape: extend the `main.cpp`
  updateChatMember branch to forward non-bot administrator/creator
  transitions to the dispatcher, which re-fetches via the existing
  `getChatAdministrators` path (`dispatcher.cpp:1058,9192`) and
  `cacheSet`s the fresh list. No user-visible button and no new callback,
  so no new routing row is needed; record the behaviour as a note on the
  admin-mode rows. IPC: none. Source:
  `refs/remix/src/bot/handlers/chat_member_updated.ts:1-23` and
  `refs/remix/src/bot/handlers/index.ts:26-49` (creator-or-
  `can_manage_video_chats` rule; Parlay already implements that rule
  on demand via `cachedAdmins` + `isChatAdmin` + the live
  `getChatMemberStatus` fallback at `dispatcher.cpp:1073`, so only the
  event-driven refresh is missing). Test: extend `dispatcher_test.cpp`
  (admin list refreshed after a simulated admin transition; no refresh
  for ordinary member joins).
- **S5. Owner exempt from the blacklist-user refusal
  (telegram-music-bot-rust).** What to build: one gate change — the
  Everyone-command blacklist refusal in `handleMessage` (today: blacklisted
  users are refused silently on every Everyone command except `/start`
  and `/listsudo`) must not apply to the owner, mirroring the ref's
  hardcoded-admin ban exemption in a non-hardcoded form (the owner id
  already comes from config). Source:
  `refs/telegram-music-bot-rust/main.rs:20` (ADMIN_ID constant),
  `main.rs:168-180` (ban checks return false for it). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp`, the `isBlUser` refusal inside
  `handleMessage` (near the `isBlUser` definition at `:1029`). Routing:
  no new row (behaviour change to an existing gate, not a new action);
  note it on the blacklist rows. IPC: none. Test: extend
  `dispatcher_test.cpp` (owner in `bl_users` still runs Everyone commands;
  a non-owner in `bl_users` is still refused silently).

Silent-denial content of this package (no build, stated so it is not
re-litigated): Parlay's silent behaviours are already present and verified
— permission gates fail silently (`dispatcher.cpp:956-957`, comment cites
NoxBeats dispatch), blacklisted users are refused silently on Everyone
commands, `/menu` panel taps by non-owners are silently ignored with an
empty ack (routing-table gap note G3, deliberate), inline queries from
non-allowed users get zero results with no message
(`handleInlineQuery` silent-auth rule), and rate-limit drops are silent
(`checkRateLimit` path). Unknown commands and unknown callbacks must NOT
be made silent (locked §7.5 / C92; see §0 and §3).

### Package 2 — Plays / buttons / thumbnails gaps in the core (5 items)

Files touched: `services/core-cpp/src/dispatcher.cpp` (new command
registrations and handlers, playerAction thumb path already exists),
`services/core-cpp/src/ui_texts.hpp` (playerKeyboard row, reply texts),
`contracts/routing-table.md` (rows C87, C88, A35 status change, new rows
for /volume and the s-prefix aliases), `services/core-cpp/tests/`
(dispatcher/routing/ui tests as noted per item). No backend changes. No
new IPC actions. This package shares `dispatcher.cpp`, `ui_texts.hpp`
and `routing-table.md` with packages 1 and 3 by architecture (single core);
separation is by handler/function ownership: package 2 owns the play-adjacent
command block and `playerKeyboard`, package 1 owns the admin command block
and `main.cpp`, package 3 owns `tg_types.hpp` and the card-text builders.

- **P1. /mix command (TgMusicBot).** What to build: register `/mix`
  (group, everyone, like `/play`) and a handler that posts the existing
  `mix.generate` action with the query or the current song, then renders
  the mix result through the existing queued/played card path; usage,
  fetching, failure and already-queued texts follow the existing play
  lifecycle texts. Source: `refs/TgMusicBot/internal/bot/mix.go`
  (handler), `internal/bot/load.go:23-25` (registration),
  `internal/bot/help.go` (help table). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp`, `registerCommands` plus a new
  `onMix` handler. Routing: update row C87 (today "Phase 3") to Live.
  IPC: none new (`mix.generate`, registered in
  `services/backend-go/internal/download/download.go:91`, handler
  `services/backend-go/internal/download/mix.go:21`). Test: extend
  `dispatcher_test.cpp` (no-query usage, backend mix result rendered)
  and `routing_test.cpp` (row C87 routed).
- **P2. Thumbnails toggle button on the player card (NoxBeats).**
  What to build: add a "Thumbnails: Enabled/Disabled" row to
  `playerKeyboard` that emits the already-routed callback and flips the
  per-chat thumbnail setting, with the card refreshed in place. This is
  wiring only: the handler already exists (`dispatcher.cpp:7162`
  `verb == "thumb"` in the player-action path), the toasts already exist
  (`ui_texts.hpp` `kToastThumbOn`/`kToastThumbOff`), and routing-table
  row A35 already documents `pl:player:thumb:<chat_id>` as "No button".
  Source: `refs/NoxBeats/Bad/ui/buttons.cpp` (`controls` toggle row),
  `Bad/cmd/play_cb.cpp:41-62` (thumb toggle). Parlay destination:
  `services/core-cpp/src/ui_texts.hpp:963` `playerKeyboard` (new row;
  label shows current state like the autoplay row) — no new handler
  needed beyond the existing thumb branch. Routing: change row A35
  status from "No button" to Live. IPC: none (core chat field `thumb`).
  Test: extend `ui_test.cpp` (toggle row present with correct state
  label) and `routing_test.cpp` (A35 now emitted by a builder).
- **P3. Numeric /volume text command (remix, anonx-cpp).** What to build:
  register `/volume <0-200>` (group, everyone) with the usage line, the
  out-of-range reply, the non-numeric reply and the success reply; it
  calls the same `vc.volume` path the /menu Vol-/Vol+ buttons already use
  (`dispatcher.cpp:7187-7203`). The panel buttons exist (routing rows
  A32–A33); only the text command is missing. Source:
  `refs/remix/src/bot/handlers/controls.ts:30-43` ("Pass a number between
  0 and 200.", "Not in call.") and `refs/anonx-cpp/src/telegram/
  dispatcher.cpp` (`handle_volume`: usage, 1–200 range, "Volume set to
  N%"). Parlay destination: `services/core-cpp/src/dispatcher.cpp`,
  `registerCommands` plus a new `onVolume` handler reusing the panel
  volume helper. Routing: new row (command section). IPC: none new
  (`vc.volume` already exists). Test: extend `dispatcher_test.cpp`
  (usage, out-of-range, non-numeric, success, nothing-playing) and
  `routing_test.cpp` (new row routed).
- **P4. s-prefix silent aliases (TgMusicBot).** What to build: register
  `/splay`, `/sskip`, `/sstop`, `/spause`, `/sresume` as aliases of the
  existing play/skip/stop/pause/resume handlers with per-invocation
  silent success (progress and errors stay visible, matching the ref's
  rule and Parlay's existing silent-mode setting `pl:set:silent`).
  Source: `refs/TgMusicBot/internal/bot/load.go:23-25` (registers `s` +
  name for each command), `internal/bot/help.go` (user section).
  Parlay destination: `services/core-cpp/src/dispatcher.cpp`,
  `registerCommands` alias table (the `s`-forms map to the existing
  handlers with a silent-success flag on the context) — no new handlers.
  Routing: new alias rows (command section). IPC: none. Test: extend
  `dispatcher_test.cpp` (each s-alias runs its base command and
  suppresses only the success message) and `routing_test.cpp` (alias
  rows routed).
- **P5. /recognize command (Music163bot-Go entry point; TgMusicBot-era
  §7.5 gap).** What to build: register `/recognize` (replied audio/voice
  message, any chat) and a handler that posts the existing
  `audio.recognize` action and replies with the identified track (or the
  honest not-recognized / not-configured reply). Caveat for the
  implementer: the recognition provider token is env-gated in the
  backend config (`services/backend-go/internal/config/config.go:224`
  comment); when it is unset the handler must answer the honest
  feature-unavailable reply, never a fabricated identification. Source:
  `refs/Music163bot-Go/internal/bot/app.go:174-198` (`/recognize`
  registration). Parlay destination: `services/core-cpp/src/dispatcher.cpp`,
  `registerCommands` plus a new `onRecognize` handler. Routing: update
  row C88 (today "Phase 3") to Live. IPC: none new (`audio.recognize`,
  registered in `services/backend-go/internal/recognize/recognize.go:231`).
  Test: extend `dispatcher_test.cpp` (no-reply usage, recognized result,
  unconfigured-provider reply) and `routing_test.cpp` (row C88 routed).

### Package 3 — Rich messages + NoxBeats rich-inline gaps (7 items)

Files touched: `services/core-cpp/src/tg_types.hpp` (InlineButton,
package-3-only file), `services/core-cpp/src/dispatcher.cpp`
(`inlineMarkup`/`tdButton` serialisation at `:8668`, inline-result
translation at `:8944`, queue/search/download card paths),
`services/core-cpp/src/ui_texts.hpp` (`queueText`, search builders,
download-failure card), `contracts/routing-table.md` (new rows for the
copy button, the Download-N-songs button, the retry button and the inline
guest button; no row changes for R2–R4, which are rendering-rule changes),
`services/core-cpp/tests/` (ui/tdlib/routing tests as noted). No backend
changes except where a reused action is noted. No new IPC actions.

- **R1. copy_text button kind (NoxBeats — the one genuine NoxBeats
  mechanism gap).** What to build: (1) add a `copy_text` payload kind to
  the inline-button type, alongside the existing callback/url/
  switch-inline kinds; (2) serialise it in the TDLib keyboard builder
  (TDLib inlineKeyboardButtonCopyText) and in the inline-result markup
  translation, which today only passes url and callback_data through;
  (3) use it for a "Copy link" button on search/YouTube result cards
  (the NoxBeats `yt_key` shape: copy button + URL button). Copy buttons
  produce no callback query, so no callback handler is needed; the
  routing-table row documents the button with a "no callback, client-side
  copy" rendered update. Source: `refs/NoxBeats/Bad/core/types.hpp:42-48`
  (`struct Button`: text, callback data, URL, copy text — exactly one
  payload kind per button), `refs/NoxBeats/Bad/ui/buttons.cpp` (`yt_key`),
  `refs/NoxBeats/Bad/tg/botapi_client.cpp:110-128` (serialisation).
  Parlay destination: `services/core-cpp/src/tg_types.hpp:62`
  (`InlineButton`: new `copy_text` field + `copy()` factory),
  `services/core-cpp/src/dispatcher.cpp:8668` (`inlineMarkup`/`tdButton`)
  and `:8944` (`inlineResult` translation). Routing: new button row
  (no callback handler). IPC: none. Test: extend `ui_test.cpp`
  (copy button present on result cards) and `tdlib_test.cpp`
  (serialised button carries copy_text and no callback_data).
- **R2. Configured default-thumbnail fallback (NoxBeats, TGVCBot).**
  What to build: a `default_thumbnail` configuration value (empty =
  today's behaviour) and a fallback chain in every photo-card send path:
  track thumbnail, else configured default thumbnail, else text card.
  Today Parlay falls straight from "no track thumbnail" to a text card;
  there is no configured default anywhere (zero `DEFAULT_THUMB` /
  `default_thumb` hits in the core). Source:
  `refs/NoxBeats/Bad/cmd/playback.cpp:111-112` and
  `Bad/core/calls.cpp:111-112` (configured `DEFAULT_THUMB` gated by
  `THUMB_GEN`), `refs/TGVCBot/apps/tgvcbot/src/handlers/play.ts`
  (`audio.thumb?.file_id ?? env.THUMBNAIL`). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp` send paths — now-playing
  (`:1686-1691`), queue card (`renderQueueCard`), replied-media delivery
  (`:6730-6746`) — plus the config schema (`config.hpp`, new
  `default_thumbnail` key; the thumbnails on/off setting still gates
  the whole chain). Routing: no new row (rendering-rule change; note it
  on the now-playing/queue rows). IPC: none. Test: extend
  `ui_test.cpp`/`dispatcher_test.cpp` (default used when the track has
  none; text card when neither exists; thumbnails-off still forces text).
- **R3. Expandable blockquote around the queue waiting list
  (ArcMusic, NoxBeats, ParvatiMusic).** What to build: wrap the up-next
  list in `queueText` in `<blockquote expandable>…</blockquote>`, keeping
  the existing Show-more pagination and per-track delete buttons
  unchanged. The component already exists in Parlay (lyrics captions use
  `<blockquote expandable>`, `dispatcher.cpp:3884`); it is only missing
  on the queue list, where three refs wrap the waiting list and Parlay
  renders it flat. Source: `refs/ArcMusic/internal/modules/queue.go:200-230`,
  `refs/NoxBeats/Bad/cmd/playback.cpp:120-127`,
  `refs/ParvatiMusic/bot/src/modules/queuecmd.rs:16-95` (and the
  `blockquote expandable` detail blocks in `bot/locales/en.yml`).
  Parlay destination: `services/core-cpp/src/ui_texts.hpp:1180`
  `queueText` (wrap the up-next section only, not the now-playing
  header). Routing: no new row (rendering change to an existing card).
  IPC: none. Test: extend `ui_test.cpp` (waiting list wrapped; empty
  queue unchanged; Show-more still present).
- **R4. Italic "Page N of M" footer on search pages (rustify footer
  pattern; locked §7.1 page-index component).** What to build: append an
  italic page footer (`<i>Page N of M</i>`, the §7.1 `rich_page_index`
  rendering) to paginated search result pages. Search pages are
  paginated today (Previous/Next buttons, `ui_texts.hpp:1336`
  `searchPageKeyboard`) but carry no page index line; the queue card is
  out of scope for this item (it paginates via Show-more, and its header
  already states the total). Source: `refs/rustify/src/telegram/actions/
  admin_users/list.rs:84-96` ("Page N/M (Total: N users)" header/footer
  pattern, generalised to search pages) and `FINAL-ARCHITECTURE.md`
  §7.1 (Footer / page index, italic). Parlay destination:
  `services/core-cpp/src/ui_texts.hpp` search builders
  (`searchHeaderText` at `:1289`, `searchResultLine` at `:1296` and the
  page assembly around `searchPageKeyboard`). Routing: no new row
  (rendering change). IPC: none. Test: extend `ui_test.cpp` (footer
  present with correct N/M on pages 1, 2 and last; single-page results
  carry no misleading footer or carry "Page 1 of 1" per the implementer's
  consistent choice, asserted by the test).
- **R5. Album/playlist photo card with "Download N songs" button
  (lemonic-telegram-bot).** What to build: on the collection-download
  flow, send the album/playlist cover as a photo card with the caption
  and a single "Download N songs" button that starts the bulk download;
  per-track files then arrive as today, followed by the existing
  completion report. The backend actions already exist
  (`spotify.collection`, `download.spotify_playlist`); what is missing
  is the core card + button (the core collection path today surfaces
  only in the 50 MB cap note at `dispatcher.cpp:4393`). Source:
  `refs/lemonic-telegram-bot/modules/methods/spotifyHandlers.ts:42`
  (album-dl), `:117` (playlist-dl), `modules/methods/utils.ts`
  (`albumMessage`, `playlistMessage`). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp` collection-download path plus
  a new card/keyboard builder in `services/core-cpp/src/ui_texts.hpp`.
  Routing: new row for the Download-N-songs button. IPC: none new
  (reuse `spotify.collection` / `download.spotify_playlist`). Test:
  extend `ui_test.cpp` (photo card + button with the correct count) and
  `routing_test.cpp` (new row routed, callback within 64 bytes).
- **R6. "Check Again" retry button on track-not-available download cards
  (lemonic-telegram-bot).** What to build: when a download fails because
  the track is not available yet, render the failure card with a
  "Check Again" button that re-runs the same download request; while
  still unavailable the tap answers the non-alert toast "Track is still
  being processed. Please try again later" (the ref caches that toast
  10s; Parlay answers per tap, which is equivalent). Source:
  `refs/lemonic-telegram-bot/modules/methods/replyWithSongNotAvailable.ts`
  (card + `spotify----{id}----track` callback) and `constants.ts`
  (`TRACK_NOT_AVAILABLE_MESSAGE`, `TRACK_NOT_AVAILABLE_TEXT`). Parlay
  destination: `services/core-cpp/src/dispatcher.cpp` download-failure
  rendering (`onDownload` area, `:4047+`) plus a new `pl:dl:retry:
  <run_id>` callback branch in the download callback handler
  (`:7797+` family). Routing: new row for `pl:dl:retry:<run_id>`.
  IPC: none new (re-run the original download action). Test: extend
  `dispatcher_test.cpp` (retry button present on not-available cards
  only; still-unavailable toast; success replaces the card) and
  `routing_test.cpp` (new row routed, callback within 64 bytes).
- **R7. "Get this song in the bot" button on inline results
  (Music163bot-Go).** What to build: attach a URL deep-link button to
  Parlay inline search results that opens the bot with the track (the
  guest-keyboard shape). Today the core translates backend-supplied
  inline-result markup (`dispatcher.cpp:8944` `inlineResult`: url and
  callback_data only) but the backend attaches no such button, so no
  inline result carries one. Source:
  `refs/Music163bot-Go/internal/bot/keyboards.go:41-49`
  (`guestKeyboard`). Parlay destination:
  `services/core-cpp/src/dispatcher.cpp` `inlineResult` (append the
  deep-link button to search-result markup; bot username from the
  existing `bot_username_` config used by the audio-caption path) or,
  if the implementer prefers the backend to own it, the search inline
  result builder in `services/backend-go/internal/search/` — one place
  only, not both. Routing: new inline row (button inventory; URL
  buttons produce no callback). IPC: none new (`search.inline` already
  exists). Test: extend `tdlib_test.cpp`/`ui_test.cpp` (inline search
  results carry the button with a well-formed deep link) — and, if the
  backend owns it, the matching search test.

## 3. NOT-APPLICABLE list (77 items, grouped by reason)

Each item names the ref(s) and the specific reason. "Deliberate
divergence" means Parlay already decided the opposite and the inventories
confirm it; "locked conflict" means porting it would violate a locked
§1.2/§7 rule; "duplicate" means Parlay already has the same component
under its own model; "domain" means the component belongs to a different
kind of bot than Parlay; "defect/dead code" means the ref's behaviour is
a bug or commented-out code, not a feature.

### 3.1 Locked-architecture conflicts (porting would violate §1.2 or §7)

1. Unknown-command silent drop — ArcMusic, Music163bot-Go, NoxBeats,
   ParvatiMusic, TgMusicBot, TinaMusic, YukkiMusic-Go, anonx-cpp,
   beetube-bot, remix, music-bot-navidrome, telegram-music-bot-daslui
   (group branch), TGVCBot, Spotify-Loader. Locked conflict: §7.5 + row
   C92 require the unknown-command card, never silence.
2. Unknown-callback silent drop — NoxBeats, TGVCBot, telegram-music-bot-
   daslui, music-bot-navidrome, Spotify-Loader. Locked conflict: §7.5
   default branch answers `Unknown action.`, never silence, never an
   un-answered query.
3. Unknown callback answered as a chat message instead of an alert —
   beetube-bot ("This feature is not available yet" sent to the chat).
   Locked conflict: §7.5 answers callbacks with toasts/alerts; Parlay's
   alert covers it.
4. Malformed-command-format silent skip — rustify
   (`ParseError::IncorrectFormat` returns Skipped). Locked conflict: C92
   answers unknown/malformed commands with the card.
5. Thread-pool saturation silent shed — anonx-cpp (log line only).
   Locked conflict: §7.5 honest errors; Parlay answers "Something went
   wrong handling that command." on handler exceptions.
6. Lossy-fallback prompt and row (accept lossy / skip) —
   music-bot-navidrome (`bot.py:1593-1714`, `lossy:` callbacks). Locked
   conflict: §1.2 lossless-first — never send lossy audio when a lossless
   FLAC source is available on the ladder.
7. 64-byte callback truncation — NoxBeats
   (`botapi_client.cpp:110-128`, `substr(0, 64)`). Locked conflict: §7.4
   makes an over-64 callback a design error caught by
   `routing_test.cpp`; truncating would mask it.
8. Base64-encoded callbacks — ParvatiMusic (`buttons.rs:48-84`).
   Locked conflict: §7.4 keeps the plain-text `pl:` namespace only.
9. Space-separated callback grammar — NoxBeats (`controls <action>
   <chat_id>` etc.). Locked conflict: §7.4 colon namespace only; the
   routing-table §1 mapping already converts every NoxBeats form.
10. JSON `type`-discriminated callbacks — beetube-bot
    (`handlers/callback-query.js`). Locked conflict: §7.4 `pl:` only.
11. `----`-separated callback format and "Invalid provider" text —
    lemonic-telegram-bot (`handlers/callback_query.ts`). Locked conflict:
    §7.4 `pl:` only; Parlay's `Unknown action.` covers the toast.
12. Emoji-token substitution (`{emoji:…}` in locale strings) —
    ParvatiMusic. Locked conflict: §7.3 no emojis anywhere; Parlay strings
    are written directly.
13. Small-caps Unicode labels, spoiler/strikethrough styling as a ported
    style — TinaMusic, ParvatiMusic, NoxBeats. Locked conflict / deliberate
    divergence: §7.3 no emojis, and Phase 9 rule 1 renders in normal
    English Bot API HTML; Parlay keeps the structure, not the styling.
14. Native `<details>` collapsible elements — YukkiMusic-Go (stats,
    help, queue details blocks). Deliberate translation, not a gap:
    Phase 9 rule 1 translates them to expandable blockquotes or Show-more
    buttons; content is present.
15. In-text `{thumb}` thumbnail link instead of a photo card —
    TinaMusic (`bot_engine.cpp:683` etc.). Deliberate divergence: Phase 9
    rule 3 sends a photo with the card as caption instead.
16. Text-only thumbnail rule (thumbnail carried, never rendered) —
    TgMusicBot (`play.go:465-477`). Deliberate divergence: Parlay follows
    the Yukki photo-caption rule instead.
17. Hardcoded Join-Room URL button on the player card — TinaMusic
    (`MakePlayerMarkup` row 5). Deliberate divergence: Phase 9 rule 7
    drops hardcoded bot URLs; Parlay URLs come from config.
18. Generated banner images (600x300 composited banner) — TGVCBot
    (`utils/banner.ts`, `utils/text-to-image.ts`). Deliberate divergence:
    excluded by Phase 9 (no image generation in core); the track
    thumbnail photo replaces it.
19. "Standby, your session begins shortly" footer line on queued cards —
    ParvatiMusic (`en.yml:602`). Deliberate divergence: flowery text
    dropped per Phase 9; the queued card's fields are present.
20. Paste-link overflow for long queues (full list behind a "more" link)
    — ArcMusic (`queue.go:158-335`). Duplicate/superseded: Parlay's
    Show-more button (`pl:queue:more`) performs the same function.
21. "and N more" plain overflow line instead of pagination —
    ParvatiMusic (`queuecmd.rs:73-90`), YukkiMusic-Go queue form.
    Duplicate/superseded: Show-more + per-track delete supersede it.
22. Queue page footers (as opposed to the search-page footer in R4) —
    NoxBeats/ArcMusic notes ("neither ref nor Parlay has page footers
    for queues"). Not a component: the queue uses Show-more by design;
    R4 covers only paginated search pages under the §7.1 page-index
    component.
23. Blockquote wrapping on every card — ParvatiMusic (82 `blockquote`
    occurrences in `en.yml`). Style choice, not a component: Parlay uses
    blockquotes on help/queue/lyric cards; wrapping every card is not a
    distinct UI element.
24. Bare no-caption first send (audio sent with no caption, metadata
    only on fallback) — lemonic-telegram-bot
    (`modules/methods/replyWithSong.ts`). Downgrade, not a gap: Parlay
    always captions delivery (the Music163bot caption rule).

### 3.2 Provider-specific login / credential flows Parlay does not have

25. NetEase `/login` flow, start-login callbacks and login-cancel
    keyboard — Music163bot-Go (`handlers_login.go:78-88`, `app.go:184-193`).
    Provider-specific login needing NetEase credentials Parlay does not
    have; Parlay has no login UI by design.
26. `/setcachechat` (NetEase cache chat) — Music163bot-Go (`app.go:184-193`).
    Provider-specific admin for the NetEase cache; no Parlay surface.
27. Spotify login invite card and Spotify-linking command
    (`/spotifylogin`, login prompt) — rustify (`actions/login.rs`,
    `login.yml`), telegram-music-bot-daslui (`main.rs:119-137`).
    Provider-specific login needing Spotify credentials; Parlay's Spotify
    access is resolver-side, not a user login flow.
28. Spotify-companion track actions: like/dislike/ignore, Magic Playlist,
    per-track recommendations, skippage diversification, AI-slop
    detection row, profanity-check toggle, lyrics AI analysis button,
    word-definition admin commands, bare `+`/`-` shortcuts — rustify
    (`handlers/commands.rs`, `inline_buttons.rs`, `inline_buttons_admin.rs`).
    Domain: these act on the user's own Spotify playback, not on a voice
    chat; Parlay has no such surface (its `spotify.dislike` action serves
    the vote flow, a different feature).
29. Personal Spotify stats and global Spotify-companion stats — rustify
    (`actions/stats.rs`, `actions/global_stats.rs`). Domain: personal
    Spotify statistics; Parlay's `/stats` covers system/bot statistics.
30. Admin user browser, per-user detail screens and their
    pagination/sort/filter keyboards — rustify
    (`actions/admin_users/list.rs`, `details.rs`, `inline_buttons_admin.rs`).
    Duplicate/domain: Parlay's admin-list surfaces are `/sudolist`,
    `/blacklisted` and `/authlist` under the owner/sudo model; a
    Spotify-companion user manager is a different model.
31. Admin join/connect DM notifications ("New user joined", "User
    connected Spotify") — rustify (`services/notification.rs:32-78`).
    Notification, not a Telegram UI component in the 7 categories, and
    there is no Spotify-connect event in Parlay to notify about.
32. Resume-pending owner DM (background work reported to the first
    allowed id) — music-bot-navidrome (`bot.py:1945-1947`).
    Background-job notification, not a UI component; Parlay has no
    resume-notice surface to attach it to.
33. Bot-cloning flow and clone gate (forwarded @BotFather token) —
    lemonic-telegram-bot (`handlers/bot_token_handler.ts`, `bot.ts`).
    Not applicable per Phase 9; token handling is out of scope.
34. Clone Bot button on the start card — lemonic-telegram-bot
    (`handlers/start_command.ts:29-30`). Same reason as 33.
35. Clone flow messages ("Bot cloned successfully", webhook errors) —
    lemonic-telegram-bot. Same reason as 33.
36. Provider link button ("Open in NetEase Music") on delivery cards —
    Music163bot-Go (`keyboards.go:14-40`). Provider-specific; Parlay
    links track titles to their source, which is the same outcome in
    provider-neutral form.
37. Cover hyperlinks in track-card text (one "Cover" link per album
    image) — telegram-music-bot-daslui (`spotify.rs:175-190`).
    Superseded: Parlay renders the cover as a photo, not as text links.
38. Above-text / small-top link previews for track cards — rustify
    (`details.rs:143-152`), telegram-music-bot-daslui (`main.rs:193-199`).
    Superseded: Parlay sends a real photo card when a thumbnail exists.
39. Proxy-resized cover art (1137x640 via image proxy) — lemonic
    (`utils.ts` `create3x4Image`). Not a Telegram UI component: Parlay
    uses the source thumbnail URL directly; resizing is image
    processing, not UI.
40. Embedded cover art in tagged audio files — music-bot-navidrome
    (`library/tagger.py:174`). File tagging, not Telegram UI.
41. Local artwork cache file (`cache/art/<fnv1a-64>.jpg`, 15s timeout,
    6 MB cap) — ParvatiMusic (`platforms/artwork.rs:1-80`). Infrastructure,
    not a UI component: the visible outcome (photo card from the remote
    URL) is identical in Parlay.

### 3.3 Non-bot web UI (not Telegram UI at all)

42. Mobile bottom menu (Home / Search) — wavetune
    (`components/mobile-menu.jsx:8-9`). Web UI; Parlay's Mini-App serving
    (`gui.*`) is read-only state views with no bottom navigation.
43. Footer branding link ("Music hub", "Deployed with love by") —
    wavetune (`components/page/footer.jsx:7,9`). Web UI branding, not a
    bot component.
44. Blurred-backdrop player-page treatment — wavetune
    (`components/cards/player.jsx:112-113`,
    `app/(player)/_components/Player.jsx:180-181`). Web treatment; the
    thumbnail-on-card behaviour itself is present in Parlay.

### 3.4 Ref-specific dead code, defects, or quirks (not features)

45. `/delete` queue handler, fully commented out — TGVCBot
    (`handlers/queue.ts`). Dead code; Parlay has working per-track delete
    buttons (`pl:queue:del`).
46. Download-cancel label with no keyboard builder — TinaMusic
    (`DOWNLOAD_CANCEL_BTN` locale key, no `Make*Cancel*` builder).
    Defect; Parlay has a working cancel button (`pl:dl:cancel`).
47. Dead Global Search reply-keyboard button (no handler, silent
    nothing) — beetube-bot (`bot.js:18-23`). Defect; Parlay wires its
    Global Search button to search.
48. Over-30-minute silent stall (dedicated message string exists but is
    never sent) — Telegram-Song-Downloader-Bot (`text_msg.py`,
    `bot.py:103-119`). Defect; Parlay replies explicitly on duration-limit
    refusal.
49. Stop-word silent drop (greeting/thanks words return nothing) —
    telegram-music-bot-rust (`main.rs:27-30,360`). Quirk, not UI.
50. Mongo-gated silent settings no-op (settings silently do nothing with
    no Mongo configured) — Spotify-Loader (`bot.py:168-190`).
    Defect; Parlay has no Mongo gate.
51. Raw-query-as-title now-playing (the query string is the title,
    because the ref does no search) — anonx-cpp
    (`dispatcher.cpp:184-189`). Strictly worse than Parlay, which resolves
    real titles; not a port.
52. Anonx ping internals (engine/thread-pool/rate-limit lines) —
    anonx-cpp (`dispatcher.cpp:282-303`). Parlay-specific internals
    differ; the ping card itself is present.

### 3.5 Duplicates of Parlay components / deliberate Parlay choices

53. Separate DEVS permission tier — TgMusicBot (`config.DEVS`,
    `extras.go:59-61`). Duplicate: Parlay's owner + sudo tiers cover the
    developer tier; a third tier adds no UI.
54. `/clearass` and `/leaveall` (disconnect/clear all assistant clients)
    — TgMusicBot (`devs.go:84,99`). No equivalent subsystem: Parlay has a
    single shared assistant session (`/userbot` join) and per-chat `/end`;
    there is no multi-assistant fleet to clear.
55. Voting-chat gate (admin commands run only in one configured chat) —
    telegram-music-bot-daslui (`main.rs:75-99`). Duplicate/conflicting:
    Parlay gates admin actions by owner/sudo role, not by a single chat
    id; the vote callbacks themselves are already role-gated.
56. Whole-bot private mode from an env owner list (`OWNER_IDS`) —
    Spotify-Loader (`bot.py:81-86`). Duplicate: Parlay gates per command
    (owner/sudo/everyone) instead of gating the whole bot; no private-mode
    config exists in the core and none is needed for the same outcome.
57. Visible `only_owner` denial reply — ParvatiMusic
    (`admin.rs:154-166`). Deliberate divergence: Parlay's permission gates
    fail silently by design (NoxBeats dispatch pattern); the visible
    variant is the one ref that differs.
58. Open settings (no gate; per-user settings for everyone) —
    beetube-bot (`commands/settings.js:9`). Deliberate divergence: Parlay
    admin-gates `/settings` (group-admin rule).
59. Universal Help + My Link row on every message — feel-me
    (`main.rs:359-398`). Deliberate divergence: Parlay attaches help
    buttons to start/help cards only; a universal row on every message
    is noise, not a component Parlay lacks.
60. Private-chat "Send a Spotify URL." fallback for any non-link message
    — telegram-music-bot-daslui (`main.rs:346-350`). Domain prompt for a
    Spotify-request bot; Parlay's unknown-command card covers the role.
61. Unknown command treated as a search query — telegram-music-bot-rust
    (`main.rs:88-99,344-381`), lemonic-telegram-bot (private-chat
    fall-through, `bot.ts:38-47`; the grammy ordering itself was read
    from code, not runtime-verified). Deliberate divergence: Parlay
    answers C92 and never searches on an unknown command.
62. Unknown callback answered with help text — feel-me
    (`main.rs:359-398`). Deliberate divergence: Parlay answers the
    `Unknown action.` alert.
63. "Invalid Command" / "Invalid Message" bare replies for unknown input
    — feel-me (`strings.rs` `INVALID_CMD`, `INVALID_MSG`). Deliberate
    divergence: Parlay answers the richer unknown-command card plus the
    caller-level command list; the reply-don't-drop principle is present.
64. Identical text for /start, /help and /settings — lemonic-telegram-bot
    (`bot.ts:38`, `start_command.ts:8`). Deliberate divergence: Parlay
    has distinct, richer start, help and settings screens.
65. Standalone per-setting dashboard cards — YukkiMusic-Go
    (`dashboard.go:47-131`, `dashboard:<name>`). Duplicate/merged: the
    same settings are toggled on Parlay's settings panel; a separate card
    per setting adds no new control.
66. `croom:` callback prefix for channel play — YukkiMusic-Go.
    Duplicate/collapsed: Parlay uses the same `pl:player:` form with the
    channel id carried in the callback (Phase 9 rule).
67. 12-character progress bar — TinaMusic (`bot_engine.cpp:208-264`).
    Duplicate/merged: Parlay uses the Yukki 9-stage bar (routing A20);
    the bar style is a merged choice, not a missing component.
68. Multi-language picker (Tina 4 languages, Yukki N languages) —
    TinaMusic (`bot_engine.cpp:350-366`), YukkiMusic-Go
    (`language.go:66-97`). Deliberate divergence: Parlay ships English
    only, so the picker lists English.
69. Non-music provider surfaces: category picker, genre picker,
    movie/anime/torrent result buttons and prompts, music-list browser
    ("latest songs") — beetube-bot (`commands/search.js`,
    `providers/*.js`). Domain: Parlay is music-only; music discovery is
    covered by `/search`.
70. Audio/video choice picker on link messages — telegram-music-bot-rust
    (`main.rs:361-376`). Superseded: Parlay routes links directly to
    play or download by link type; a picker would add a tap to every
    link play.
71. Audio-effect remix picker as a separate pre-download screen —
    telegram-music-bot-rust (`main.rs:442-492`). Duplicate in function:
    Parlay's download FX row (`pl:dl:fx:<run_id>:<effect>`) already
    offers effect selection on the download flow (see §5, item 7).
72. Re-tag flow (dry-run + `/retag confirm` + summary) —
    music-bot-navidrome (`bot.py:536-820`). Out of scope: no re-tag
    backend action exists in `contracts/ipc.json`, and library tagging
    is not one of the 7 UI categories.
73. Per-card expandable blockquote on the stats card specifically
    (rustify languages blockquote) — rustify (`actions.yml`
    `actions.stats`). Not a missing component: the expandable-blockquote
    component exists in Parlay (lyrics captions); which cards use it is
    layout, and rustify's stats are personal Spotify stats, not Parlay
    system stats.
74. Feel-me help text wording and typos ("Desctiption", "Music Has Been
    send") — feel-me (`strings.rs` `HELPTEXT`). Wording, not a component;
    Parlay's share/start/help texts cover the same flows.
75. Lemomic "Invalid provider" / "Invalid Spotify ID" exact texts —
    lemonic-telegram-bot (`callback_query.ts`, `constants.ts`). Wording:
    Parlay's `Unknown action.` alert and invalid-link replies cover the
    same cases.
76. Single-action pause/mute toggle cards as separate cards —
    YukkiMusic-Go (`buttons.go:122-149`). Duplicate/merged: the same
    toggles live on Parlay's `/menu` panel.
77. Channel-variant (`cplay`) callback split and the `room:`/`croom:`
    split generally — ArcMusic, ParvatiMusic, YukkiMusic-Go. Duplicate/
    collapsed: one `pl:player:` form carries the channel id.

## 4. Still-UNCERTAIN list (3 items)

- **U1. Unmatched plain-text / non-text fallthrough in groups.**
  Refs: anonx-cpp (non-command messages ignored), openradiobot (short /
  non-command input silent), telegram-music-bot-rust (non-text fallback
  returns Ok), music-bot-navidrome (non-link plain text does nothing).
  Why uncertain: the `handleMessage` path was traced through the
  service-message, media, number-pick and pending-state-DM branches
  (`dispatcher.cpp:654-760`), but the final fallthrough for text that is
  neither a command, a number pick, nor a pending-state reply was not
  fully traced in this pass, so it cannot be stated whether unmatched
  group text is ignored silently or answered. This does not block any
  PORT item; it only decides whether the four ref rows resolve to
  PRESENT (ignored) or to a new silent-ignore rule. Resolution: read the
  post-pending-state path of `handleMessage` and add a test asserting
  the behaviour for unmatched group text.
- **U2. Album/playlist download per-track checklist + ETA.**
  Ref: music-bot-navidrome (`reporting.py:101-311` `AlbumProgress.render`:
  per-track checklist with pending/downloading percent + speed /
  remotely-queued position / done / failed-with-reason lines, collapsed
  counter above 28 tracks, ETA line). Why uncertain: the core download
  progress rendering (`onDownload`, `dispatcher.cpp:4047+`) shows
  single-track percent/speed/ETA, and whether the backend
  `download.progress` payload reports per-track states for album/playlist
  runs was not verified in this pass. If the backend already reports
  them, this becomes a core-rendering PORT item (package 3); if it does
  not, it needs a backend payload change first and is not a UI-only port.
  Resolution: inspect the `download.progress` payload for a playlist run
  before deciding.
- **U3. Share target-name fallback when the target user row is missing.**
  Ref: feel-me (`main.rs:77-135`: target name renders as "this user"
  when the target row is missing from the DB). Why uncertain: no "this
  user" fallback string exists anywhere in the core, and the share flow's
  missing-target rendering was not verified. The share flow itself is
  present (`shareReceivedKeyboard`, `pl:share:*`); only the missing-row
  fallback text is unverified. Resolution: read the share-target
  resolution path and add a test for a missing target row.

## 5. Corrections to the inventory verdicts

Items 1–21 are verdict changes from re-verification against the current
tree. Item 22 is a routing-table staleness correction that changes the
cost of five PORT items. Item 23 qualifies a package-3 assumption from the
task brief itself.

1. **Queue-card photo variant: MISSING → PRESENT.** NoxBeats
   (`Bad/cmd/playback.cpp:111-124`) was marked missing on the claim that
   Parlay's queue card is text-only. Wrong: `buildQueueCard` returns the
   current track's thumbnail (`dispatcher.cpp:1698+`, `thumb_out`), and
   `renderQueueCard` (`:1753`) sends a photo when a thumbnail exists and
   thumbnails are on, falling back to text otherwise. The NoxBeats queue
   photo rule is already Parlay's rule. Consequence: the queue-photo
   item is NOT in the PORT list (the task brief listed it as a package-3
   example; it needs no work).
2. **/logs viewer: UNCERTAIN → PRESENT.** YukkiMusic-Go. Parlay has
   `/logs` (sudo, silent denial otherwise), routing row C80, handler
   `onLogs` (`dispatcher.cpp:496`, help entry `ui_texts.hpp:846`).
3. **/new radio creation: UNCERTAIN → PRESENT.** openradiobot. Parlay
   has `/new` (group, everyone) backed by `radio.start`, routing row C38
   (radio-created / already-exists / no-link replies). The semantics
   differ (station link vs radio creation), but the command exists.
4. **/queue move subcommand: PRESENT-with-gap-note → PRESENT.**
   openradiobot. The gap note is wrong: Parlay has top-level `/move`
   (`dispatcher.cpp` queue-ops routes), so the move function exists even
   though it is not a `/queue` subcommand.
5. **Standalone autoplay card: UNCERTAIN → PRESENT.** TinaMusic. Parlay
   has `autoplayCardText`/`autoplayCardKeyboard`
   (`ui_texts.hpp:1596-1609`, sent at `dispatcher.cpp:5396-5405`).
6. **Clean-delay cycle: UNCERTAIN → PRESENT.** TinaMusic. Parlay's
   settings clean-delay value cycles 15 → 30 → 60 → 5 (routing row A48).
7. **Remix effects picker: MISSING → PRESENT (adapted).**
   telegram-music-bot-rust. Parlay's download FX row
   (`pl:dl:fx:<run_id>:<effect>`, `dispatcher.cpp:7797+`) already offers
   effect selection on the download flow.
8. **Download-and-send flow: UNCERTAIN → PRESENT.**
   telegram-music-bot-rust. Parlay's `/download` command
   (`dispatcher.cpp:4047` `onDownload`) plus audio delivery with caption
   and thumbnail is the user-facing flow.
9. **Banned-user silent drop: UNCERTAIN → PRESENT.**
   telegram-music-bot-rust. `handleMessage` refuses blacklisted users
   silently on Everyone commands (except `/start` and `/listsudo`) and
   leaves blacklisted chats silently (`dispatcher.cpp` gate near
   `:960-967`, `isBlUser` at `:1029`).
10. **Feel-me feedback + report flow: MISSING → PRESENT.**
    Parlay's `shareReceivedKeyboard` (`ui_texts.hpp:2464`) carries
    Feedback (`pl:share:feedback:…`) and Report User
    (`pl:share:report:…`) buttons, handled in `cbShare`
    (`dispatcher.cpp:8176+`) with blocked-feedback and reported-blocked
    replies. Deliberate nuance, not a gap: per the in-code ruling-8
    comment, there is deliberately no "already sent" / "already reported"
    dedup (every tap re-opens the prompt; re-reporting just re-blocks
    idempotently), and `kShareReportAlready` is kept unused for future
    dedup work.
11. **Share send-prompt Cancel button: UNCERTAIN → PRESENT.** feel-me.
    `pl:share:cancel` exists (routing row A19 notes the share send
    prompt's "Cancel sending music" button; handler deletes the message
    and acks silently).
12. **Inline np / del / hint modes: UNCERTAIN → PRESENT.**
    music-bot-navidrome. `inlineModeOf` (`dispatcher.cpp:6670-6700`)
    implements exact np/s/l/help, `lib|del|search <arg>`, bare lib/del/
    search, and the <3-char hint mode; `handleInlineQuery` (`:6806+`)
    answers the hint article with switch-pm text for empty/short queries.
13. **Inline article cover thumbnails: UNCERTAIN → PRESENT.**
    music-bot-navidrome. `inlineResult` (`dispatcher.cpp:8944+`) carries
    `thumbnail_url` (from `thumb_url`) with width/height on article
    results.
14. **/stats: UNCERTAIN → PRESENT.** music-bot-navidrome. Parlay has
    `/stats` (and `/status`) routes; the inventory's "not verified by
    grep" is resolved by the command registration and stats card.
15. **Pagination purge key: UNCERTAIN → PRESENT.** beetube-bot. Parlay
    has the `paginate_purge_old` setting (`ui_texts.hpp:1472`,
    `dispatcher.cpp:1172-1173,1195,1623-1624,7625`) — the same key family
    under a renamed key.
16. **Search foreign-tap toast wording: UNCERTAIN → PRESENT.**
    TGVCBot. Parlay's foreign-tap toast is `kToastNotAllowed`
    ("You aren't allowed"), the same wording as the ref's alert, used
    across callback handlers including the search/requester-gated paths
    (`dispatcher.cpp:7281,7364,7518,8032,8068,8138`).
17. **Play-from-reply: UNCERTAIN → PRESENT (Parlay side).**
    ParvatiMusic. The uncertainty was about the ref's reply-branch
    coverage, not Parlay's: Parlay plays replied audio/video/voice/
    document via `vc.play_file` (routing row C1).
18. **Standalone dashboard cards: UNCERTAIN → merged, not a gap.**
    YukkiMusic-Go. The settings themselves are all toggled on Parlay's
    settings panel; a separate card per setting is the §3 item 65
    duplicate, not a missing control.
19. **Sudo-gate per-handler variation: UNCERTAIN → covered, not a gap.**
    YukkiMusic-Go. Parlay's gate (silent for commands, not-allowed toast
    for callbacks) covers the variation; no per-handler component is
    missing.
20. **Rich-table presentations: UNCERTAIN → PRESENT (adapted).**
    TgMusicBot (active-VC rich table, playlist-info collapsible table).
    The content is present (`/ac` active-VC list, `/playlistinfo` at
    `dispatcher.cpp:5085`); the table form is translated to lines per
    Phase 9 rule 1, as with all TDLib rich tables.
21. **Album/playlist captions: UNCERTAIN → PRESENT (adapted).**
    lemonic-telegram-bot (`albumMessage`, `playlistMessage`). Parlay's
    download flows carry album/playlist captions; the exact three-line
    form is wording, not a missing component.
22. **Routing-table staleness (cost-changing).** Rows C87–C91 and §E
    items E3–E7 state that /mix, /recognize, /scan, /sharescan and
    /rmcache have "no backend action". All five actions exist and are
    registered: `mix.generate` (`download/download.go:91`,
    handler `download/mix.go:21`), `audio.recognize`
    (`recognize/recognize.go:231`), `library.scan` and
    `library.sharescan` and `cache.clear`
    (`library/library.go:272-274`, `library/delete.go:110`). The
    routing table also lists the same five as "implemented in Phase 3"
    while the actions are already registered; only the core command
    registration and handlers are missing. The implementer should update
    rows C87–C91 and §E together with the PORT items, not build new
    backend actions.
23. **Package-3 brief assumption corrected.** The brief listed the
    "queue photo variant" as a package-3 example. It is PRESENT (item 1
    above) and is excluded from package 3. Package 3's thumbnail work is
    instead the default-thumbnail fallback (R2), which is genuinely
    missing.
