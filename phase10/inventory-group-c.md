# Parlay Phase 10 — UI Inventory, Group C (7 refs)

**Scope:** fresh audit (not a copy of Phase 9) of every Telegram UI component in
7 categories for 7 refs under `~/workspace/parlay/refs/`. One section per ref,
one table per category. No emojis are used anywhere in this file: where a ref
button label is itself a single glyph, the label is described in words
(e.g. "shuffle glyph") instead of reproduced.

**Verdict key (checked against the Parlay tree, not against Phase 9 claims):**
checked paths are `services/backend-go/internal/gui/`,
`services/backend-go/internal/handlers/`,
`services/backend-go/internal/settings/`,
`services/backend-go/internal/room/`,
`services/backend-go/internal/voice/`,
`services/core-cpp/src/dispatcher.cpp` and `ui_texts.hpp`,
`contracts/routing-table.md`, `contracts/ipc.json`, and the resolver/model
thumbnail fields (`services/backend-go/internal/model/model.go`,
`services/backend-go/internal/resolver/`).

- **PRESENT** — Parlay already has this component or a direct equivalent
  (routing-table row or code path cited in the verdict note).
- **MISSING** — no equivalent found in the checked Parlay paths.
- **UNCERTAIN** — Parlay has a neighbouring pattern but the exact behaviour
  was not verified by grep; the reason is stated per row.

**Counting rule:** counts cover real components only. "Not applicable" notes
(no such component in the ref, with a reason) are listed under the relevant
category but are not counted.

**Clone note:** every ref checkout reports as a shallow git repository, but
all working trees are complete (handler/command registration files were read
in full, not sampled). A web search for the remix upstream was inconclusive
(the name is ambiguous and results were generic grammY documentation), so no
missing verdict in this file rests on shallow-clone status alone; each rests
on an exhaustive grep of the local registration/handler files. Where a
behaviour is inferred from a dispatch chain rather than a dedicated handler,
the row says so.

## Totals

| Category | Inventoried | PRESENT | MISSING | UNCERTAIN |
|---|---|---|---|---|
| 1. Sudos | 22 | 9 | 9 | 4 |
| 2. Plays | 72 | 52 | 14 | 6 |
| 3. Buttons | 34 | 19 | 13 | 2 |
| 4. Thumbnails | 12 | 6 | 2 | 4 |
| 5. Rich messages | 41 | 33 | 6 | 2 |
| 6/7. Silent denials for unknown commands (category 7; category 6 is Group A / NoxBeats, see note per ref) | 17 | 4 | 10 | 3 |
| **Total** | **198** | **123** | **54** | **21** |

Per-ref totals: remix 36 (32 / 3 / 1), rustify 48 (20 / 23 / 5),
telegram-music-bot-daslui 19 (10 / 8 / 1), telegram-music-bot-rust 21
(10 / 6 / 5), music-bot-navidrome 41 (22 / 12 / 7), openradiobot 20
(18 / 0 / 2), wavetune 13 (11 / 2 / 0). Order in each triple is
PRESENT / MISSING / UNCERTAIN.

---

# 1. remix — `refs/remix/` (TypeScript, grammY, HTML parse mode globally)

Group voice-chat bot. No `/start` and no `/help` command exist anywhere in
`src/bot/` (verified by grepping all `composer.command` registrations).
No sudo list / add-sudo / remove-sudo command exists. Admin rights are
derived from Telegram group administration, not from a sudo list.

## 1.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Admin gate for panel/cache/controls | Panel, cache and control handlers run only behind a filter that admits group creators and administrators with `can_manage_video_chats` (non-anonymous); everyone else is filtered out with no reply | `src/bot/handlers/index.ts:26-49` | UNCERTAIN — Parlay has admin-mode gating and an admin denial card (`kDenyAdmin`, routing-table row B3), but no live `getChatAdministrators` sync into a session admin list was found |
| Admin list sync on member change | `chat_member` updates add/remove users from the session admin list using the same creator-or-`can_manage_video_chats` rule | `src/bot/handlers/chat_member_updated.ts:1-23` | MISSING — no chat-member admin sync in the checked Parlay paths |
| `/cache` (admin-only) | Resets the session: clears the admin list (forcing a re-fetch), clears the active search, turns loop off; replies "Caches deleted." | `src/bot/handlers/cache.ts:7-12` | MISSING — no cache-reset command in Parlay routing |
| Not applicable: sudo list / add / remove / denial message | There is no sudo roster at all; non-admins are silently filtered, never told why | `src/bot/handlers/index.ts:26-49` (filter returns false) | — |

## 1.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/stream`, `/s`, `/play`, `/p` | Streams a YouTube URL/search, a replied audio/voice message, or a `custom` input; replies "Streaming..." when it starts at once, otherwise "Queued at position N." | `src/bot/handlers/stream.ts:8-31`, strings in `locales/en/messages.ftl` (`stream.*`) | PRESENT — routing-table row C1 (`/play`, replied media via `vc.play_file`) |
| Play from reply (audio/voice) | Replied audio/voice is the play input when present, ahead of any text argument | `src/bot/handlers/stream.ts:9-13` | PRESENT — routing-table row C1, replied-media path |
| `/playlist`, `/pl`, `/list` | Queues a YouTube playlist (URL from argument or replied text, capped by `MAX_PLAYLIST_SIZE`); replies "Streaming and queuing N items..." or "Queuing N items..." | `src/bot/handlers/playlist.ts:11-31` | PRESENT — routing-table row C2 (`/playplaylist`) |
| `/now`, `/ns`, `/cs`, `/np`, `/cp` | Now-playing card: linked title plus linked requester name; "Not streaming." when idle | `src/bot/handlers/now.ts:9-23` | PRESENT — `vc.now_playing`, `gui.now_playing` |
| Panel now/next line | Panel text shows the current track and the next queued track as links, with a loop glyph instead of the music glyph when loop is on | `src/bot/handlers/panel.ts:29-48` | PRESENT — `vc.panel` plus routing-table panel rows A21-A34 |
| `/search`, `/find` + number reply | YouTube search (10 videos) rendered as a numbered list; replying with a number streams that result and deletes the search message; `/cancel` deletes it | `src/bot/handlers/search.ts:14-93` | PRESENT — routing-table row A73 (search number buttons; Parlay uses buttons where remix uses a number reply) |
| Queued-at position reply | The queue position number returned when a track is queued rather than started | `src/bot/handlers/stream.ts:28-30`, `src/bot/streamer/stream.ts:43-44` | PRESENT — queued card in routing-table row C1 |
| `/pause` | Pauses; replies "Paused." / "Not streaming." / "Not in call." by result | `src/bot/handlers/controls.ts:10-13` | PRESENT — routing-table row C5 (`/pause`, `vc.pause`) |
| `/resume`, `/re`, `/res`, `/continue` | Resumes; same three-way reply shape | `src/bot/handlers/controls.ts:15-18` | PRESENT — routing-table row C6 (`/resume`, `vc.resume`) |
| `/skip`, `/next` | Advances to the next queued item; same three-way reply shape | `src/bot/handlers/controls.ts:20-23` | PRESENT — routing-table row C3 (`/skip`, `vc.skip`) |
| `/leave`, `/stop` | Leaves/stops the call and clears the queue; same three-way reply shape | `src/bot/handlers/controls.ts:25-28`, `src/bot/streamer/stop.ts:1-8` | PRESENT — routing-table row C7 (`/end`, `/stop`, `vc.stop`) |
| `/volume`, `/vol`, `/v` (0-200) | Sets volume; "Pass a number between 0 and 200." when invalid, "Not in call." when no call | `src/bot/handlers/controls.ts:30-43` | PRESENT — panel Vol - / Vol + rows A32-A33 (`vc.volume`); Parlay has no numeric `/volume` text command in routing |
| `/mute`, `/m` and `/unmute`, `/um` | Mute/unmute with already-muted / not-muted replies | `src/bot/handlers/controls.ts:45-54` | PRESENT — panel Mute/Unmute rows A30-A31 (`vc.mute`) |
| `/shuffle`, `/sh`, `/mix` | Shuffles the queue (current track re-queued first); "No enough items to shuffle." when too short | `src/bot/handlers/controls.ts:55-63`, `src/bot/queues.ts:66-85` | PRESENT — panel Shuffle row A29 (`vc.shuffle`) |
| `/loop`, `/repeat` | Toggles the per-chat loop flag; replies "Turned loop on/off." | `src/bot/handlers/controls.ts:65-68` | PRESENT — routing-table row C8 (`/loop`, `vc.loop`; Parlay uses a 1-10 count, remix a toggle) |
| `/ly`, `/lyrics` | Lyrics for the current track title, chunked at 4096 chars; "Not streaming." / "Lyrics not found." fallbacks | `src/bot/handlers/lyrics.ts:11-31` | PRESENT — `vc.lyrics` in `handlers/vc.go:777` |
| Not applicable: progress display, play history | No elapsed/total time, no seek, no history list; the queue itself is never rendered as a list (only now/next on the panel) | `src/bot/queues.ts` (no list renderer), `src/bot/handlers/` (no progress handler) | — |

## 1.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Panel button: shuffle glyph | Shuffles; toast "Shuffling..." or "No enough items to shuffle." | `src/bot/handlers/panel.ts:15-16`, callback `panel.ts:100-116` | PRESENT — routing-table row A29 (`pl:player:shuffle`) |
| Panel button: skip glyph | Skips; toast "Skipped." / "Not streaming." / "Not in call." | `src/bot/handlers/panel.ts:17`, callback `panel.ts:117-122` | PRESENT — row A24 (`pl:player:skip`) |
| Panel button: pause glyph | Pauses; toast per result | `src/bot/handlers/panel.ts:18`, callback `panel.ts:123-128` | PRESENT — row A21 (`pl:player:pause`) |
| Panel button: resume glyph | Resumes; toast per result | `src/bot/handlers/panel.ts:19`, callback `panel.ts:129-134` | PRESENT — row A22 (`pl:player:resume`) |
| Panel button: mute glyph | Mutes; toast per result | `src/bot/handlers/panel.ts:21`, callback `panel.ts:135-140` | PRESENT — row A30 (`pl:player:mute`) |
| Panel button: unmute glyph | Unmutes; toast per result | `src/bot/handlers/panel.ts:22`, callback `panel.ts:141-146` | PRESENT — row A31 (`pl:player:unmute`) |
| Panel button: volume-down glyph | Decreases volume in steps (1000 units, floor 1, from a 5000 start); "Not in call." when no call | `src/bot/handlers/panel.ts:23`, callback `panel.ts:147-159`, step helpers `panel.ts:50-58` | PRESENT — row A32 (`pl:player:voldec`) |
| Panel button: volume-up glyph | Increases volume in steps (1000 units, cap 20000, from a 10000 start) | `src/bot/handlers/panel.ts:24`, callback `panel.ts:160-173` | PRESENT — row A33 (`pl:player:volinc`) |
| Panel button: refresh glyph | Re-edits the panel text in place; toast "Updated." | `src/bot/handlers/panel.ts:25`, callback `panel.ts:94-99`, `updatePanel` at `panel.ts:60-71` | PRESENT — row A34 (`pl:player:refresh`, toast "Updated.") |
| Panel callback routing + requester-only filter | Callbacks match `panel_<command>`; taps are accepted only from the user who sent the message the panel replies to — other users' taps match no handler and get no answer | `src/bot/handlers/panel.ts:80-95` | PRESENT — Parlay answers foreign taps with the "You aren't allowed" alert (routing-table rows A75-A76 pattern; stricter than remix silence, same ownership idea) |
| Not applicable: reply keyboard, other inline surfaces | The panel keyboard is the only button surface; search uses plain number replies, not buttons | `src/bot/handlers/search.ts:14-93` | — |

## 1.4 Thumbnails

No thumbnail component exists in remix: no `sendPhoto`, no cover-art
fetch, no thumbnail field. The panel and now-playing messages are text with
hyperlinked titles, and the panel explicitly disables web-page preview
(`disable_web_page_preview: true`, `src/bot/handlers/panel.ts:14`). There is
therefore nothing to fall back from. Parlay's equivalent (photo card when a
thumbnail exists, text card otherwise — `dispatcher.cpp:1681-1691`) is richer
than remix, so this category is inventoried as zero components, not as a gap.

## 1.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Global HTML parse mode | Every API call gets `parse_mode: HTML` by default | `src/bot/index.ts:18-23` | PRESENT — Parlay sends HTML cards (`sendMessageHtml`, `dispatcher.cpp`) |
| Panel text template | "{now glyph} linked-now-title, arrow line, linked-next-title" with loop/music glyph switch | `src/bot/handlers/panel.ts:29-48`, strings `locales/en/messages.ftl` (`panel.text`) | PRESENT — panel card via `vc.panel` + routing panel rows |
| Now-playing text template | "Currently streaming linked-title by linked-requester..." | `src/bot/handlers/now.ts:9-23`, `locales/en/messages.ftl` (`now =`) | PRESENT — now-playing card |
| Search result template | Per result: number glyph + linked title, then tree lines for duration (live dot or clock glyph), views, upload date, uploader; header "Search results for {query}..." and footer "Reply the number of the result you want to stream or /cancel." in italics | `src/bot/handlers/search.ts:14-53`, `locales/en/messages.ftl` (`search.*`) | PRESENT — search results card (row A73); Parlay renders number buttons instead of a number-reply footer |
| Lyrics template | Bold title + `#lyrics` tag line, then the lyrics text, chunked at 4096 | `src/bot/handlers/lyrics.ts:11-31`, `locales/en/messages.ftl` (`lyrics.lyrics`) | PRESENT — `vc.lyrics` |
| Error/response templates | Shared prefixes: error = cross-mark + bold text, ok = check-mark + bold text, response = glyph + bold text; error-map for "No active group call", "No video id found", "Too big", "Could not find the input entity for" | `locales/en/messages.ftl` (`-error`, `-ok`, `-response`), `src/bot/errors.ts:1-8`, `src/bot/index.ts:24-44` (`bot.catch`) | PRESENT — Parlay answers backend errors as alerts/cards, never a fake success (routing-table row C1 note) |
| Not applicable: collapsibles, copy pills, page footers | None exist; messages are flat HTML, titles are truncated at 70 chars (`src/bot/helpers/text.ts:1-2`) | — | — |

## 1.6 Rich inline buttons (NoxBeats category)

Not this ref's category (Group A covers NoxBeats). Remix's rich-card-with-
buttons surface is the panel itself, recorded above under Buttons (panel
keyboard) and Rich messages (panel text template).

## 1.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| Silent drop: no fallback handler exists. An unmatched command (including `/start`, `/help`, or any typo) matches no `composer.command` registration and produces no reply; errors only surface through `bot.catch` for thrown errors, never for unknown commands. Non-admin taps on panel/controls are likewise silently filtered by the admin gate | `src/bot/handlers/index.ts:15-51` (no catch-all), `src/bot/handlers/panel.ts:80-95` (filter), `src/bot/index.ts:24-44` (`bot.catch` only) | MISSING — Parlay does the opposite: routing-table row C92 sends an unknown-command card ("Command /{name} not found." + the command list for the caller's level), never silent |

---

# 2. rustify — `refs/rustify/` (Rust, teloxide)

Spotify companion bot, not a voice-chat bot: it watches the user's own
Spotify playback and reacts (like/dislike/lyrics/profanity/AI-slop), rather
than playing audio into a call. There is therefore no VC play/pause/skip/
resume/stop, no queue, no progress bar and no play history in this ref; the
"Plays" rows below are its now-playing equivalents. Locales are
`locales/*.yml` (English strings quoted; Russian equivalents exist alongside).

## 2.1 Sudos

Admin status is a database role (`UserRole::Admin`, `src/entity/user.rs:64-65`,
`src/entity/user.rs:342`), not an env sudo list. There is no add-sudo,
remove-sudo or sudo-list command in the tree: the only user-management
surface is the admin-only `/users` browser below.

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Admin role gate | Admin command handlers run only when `state.user().is_admin()`; non-admins never reach them and fall through to the user command handler | `src/telegram/handlers/message.rs:26-28` | PRESENT — Parlay owner/sudo permission model (`dispatcher.cpp:459-473`, `Perm::Sudo`/`Perm::Owner`) |
| `/admin` help | Sends the admin command descriptions with the header "Admin Commands available to you" | `src/telegram/handlers/admin_commands.rs:30-38`, `src/telegram/commands_admin.rs:1-40` | PRESENT — Parlay help/unknown-command cards list commands for the caller's level (routing-table row C92) |
| `/users [user-id]` admin browser | Paginated user list, 10 per page: header "Recent Users", "Page N/M (Total: N users)", "Sorted by: CreatedAt/ LyricsChecked + Asc/Desc, Filter: All/status", then per user name, ID in a code span with a user link, status, lyrics-checked count, locale, created timestamp; footer hint to call `/users [user-id]` for one user | `src/telegram/actions/admin_users/list.rs:23-140` (page size line 23, header lines 84-96, row lines 105-124) | MISSING — Parlay has no admin user browser |
| User details screen (+ Back) | Full detail for one user (basic info, configuration toggles, language stats, AI-slop counts) with a Back button returning to the list page it came from; unknown id replies "User with ID {id} is not found" | `src/telegram/actions/admin_users/details.rs:25-75` (Back button), `format_user_details` from line 80 | MISSING — no per-user detail screen in Parlay |
| `/global_stats` | Global statistics message (per-status user counts with ratios) | `src/telegram/actions/global_stats.rs:45-135` | UNCERTAIN — Parlay stats surfaces were not verified by grep; do not assume present |
| `/broadcast {locale}` | Broadcasts the replied message to all users of a locale; requires a locale argument ("Pass right locale") and a replied message ("Reply to message"); reports "Sent to N users. Errors M" | `src/telegram/actions/broadcast.rs:22-63`, registered `src/telegram/commands_admin.rs:18-20` | PRESENT — Parlay `/broadcast` is a sudo command (`dispatcher.cpp:476`) |
| Word-definition admin commands | `/get_word_definition`, `/reset_word_definition`, `/list_word_definitions` (locale + word) plus inline regenerate / pagination buttons for definitions | `src/telegram/commands_admin.rs:22-32`, `src/telegram/inline_buttons_admin.rs:13-31` (`wd`, `wp` variants) | MISSING — no word-definition feature in Parlay |
| Admin inline buttons (users pagination/sort/filter) | Serialised JSON callbacks: users page (`aup`: page, sort, order, filter), user select (`aus`), back (`aub`); sort by created-at or lyrics-checked, ascending/descending, status filter | `src/telegram/inline_buttons_admin.rs:32-120`, list keyboard builder `src/telegram/actions/admin_users/list.rs:141+` | MISSING — Parlay has pagination keyboards for queue/settings, but no admin-users pagination surface |
| Admin inline denial alert | A non-admin tapping an admin inline button gets the alert "Button is broken. Try another one" | `src/telegram/handlers/inline_buttons.rs:27-37` | PRESENT — Parlay answers foreign taps with "You aren't allowed" (routing-table rows A75-A76 pattern) |
| Admin notifications | On a new user joining and on Spotify connect, admins receive a DM: "New user joined" (ID in code, user link, name, ref code) / "User connected Spotify" (name, ID in code with link) | `src/services/notification.rs:32-78` | MISSING — no admin join/connect DM in Parlay |

## 2.2 Plays (now-playing equivalents; no VC playback)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/details` current-track card | "Collecting information about the track" placeholder, then an edited card: track + album links, audio features (key/modality, BPM, acoustic/dance/energy/instrumental/live/speech/positivity percentages), disliked-by / ignored-by counts, genres as links, then lyrics (see Rich) | `src/telegram/actions/details.rs:41-160` (placeholder), `details.rs:287-350` (final edit), strings `locales/details.yml` | PRESENT — Parlay now-playing card + `vc.lyrics` |
| `/like` current track | Likes the currently playing Spotify track (undoes a dislike) | `src/telegram/handlers/commands.rs:62-64`, action `src/telegram/actions/like.rs` | MISSING — no track like/dislike status in Parlay |
| `/dislike` current track | Dislikes/blocks the current track; reply names the track link and points at the ignore button label | `src/telegram/handlers/commands.rs:59-61`, strings `locales/actions.yml` (`actions.dislike`) | MISSING — as above |
| Ignore / hide alerts for a track | Per-track ignore state, toggled from the track-status inline row (see Buttons) | `src/telegram/inline_buttons.rs:58-88` (`TrackStatus::Ignore` branch) | MISSING — no per-track ignore state in Parlay |
| Song-links button flow | "Finding links to other platforms..." then "Available on:" with links from SongLink, credited to odesli.co | `src/telegram/actions/song_links.rs`, strings `locales/song_links.yml` | PRESENT — Parlay share card (routing-table share rows; different mechanism, same user outcome) |
| Analyze lyrics button flow | AI analysis of the current track's lyrics, appended as a button on the details card when an AI backend is configured | `src/telegram/actions/details.rs:336-344` (button added), action `src/telegram/actions/analyze.rs` | MISSING — no lyrics AI analysis in Parlay |
| `/magic` Magic Playlist | Generates a "Magic Playlist" from the user's favourite tracks | `src/telegram/handlers/commands.rs:103-105`, strings `locales/magic.yml` | MISSING — no magic-playlist generator in Parlay |
| `/recommendasion` | Recommendation action for the current track | `src/telegram/handlers/commands.rs:106-108`, strings `locales/recommendasion.yml` | MISSING — no per-track recommendation action in Parlay |
| `/skippage {days}` | Skippage feature: diversifies listening by skipping over-familiar tracks, with an enable/disable inline button | `src/telegram/handlers/commands.rs:112-114`, strings `locales/skippage.yml` | PRESENT — closest equivalent is Parlay `/autoplay-skip` / `vc.autoplay_skip` (routing-table row C4); not the same feature, verdict is for the skip-diversification idea only |
| `/ai_slop_detection` | AI-generated-music detection with notify / ignore / auto-skip modes, shown as a three-option inline row with a check-mark on the selected mode | `src/telegram/handlers/commands.rs:109-111`, button labels `src/telegram/inline_buttons.rs:40-56` | MISSING — no AI-slop detection in Parlay |
| `/stats` personal stats | "Some nice stats for you": dislikes, skips, removals from liked songs/playlists, lyrics checked/analysed, ignored tracks, profane lyrics, AI-slop totals, plus an expandable languages blockquote | `src/telegram/actions/stats.rs:85-86`, strings `locales/actions.yml` (`actions.stats`) | UNCERTAIN — Parlay stats surfaces were not verified by grep |
| `/toggle_track_skip` | "Disliked tracks will be skipped" / "Disliked tracks won't be skipped" | `src/telegram/handlers/commands.rs:86-88`, strings `locales/settings.yml` (`settings.skip-*`) | PRESENT — Parlay autoplay/skip toggles (`vc.autoplay`, panel autoplay row A28) as the toggle-pattern equivalent |
| `/toggle_profanity_check` | "Tracks will be checked for profanity content" / "Tracks won't be checked for profanity content" | `src/telegram/handlers/commands.rs:89-91`, strings `locales/settings.yml` (`settings.profanity-check-*`) | MISSING — no profanity-check toggle in Parlay |
| `/login` Spotify login invite | Sends the Spotify login invite card (see Rich) when the user is not authed; inline taps by unauthed users get the alert "You need to login first" and the invite is re-sent | `src/telegram/handlers/commands.rs:69-71`, `src/telegram/handlers/inline_buttons.rs:100-112` | MISSING — Parlay has no Spotify login flow |
| Not applicable: VC play/pause/skip/stop, queue, progress, history | None exist; playback happens in the user's own Spotify client, not through the bot | `src/spotify/` (polling client only) | — |

## 2.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Start reply keyboard | Persistent reply keyboard, 3 buttons: row 1 "Dislike playing track" (wording per locale); row 2 "Stats" + "Details of track"; shown on `/start`, `/keyboard`, `/help` and unknown-command replies | `src/telegram/keyboards.rs:5-57`, attached in `src/telegram/actions/start.rs:34-35` and `src/telegram/handlers/commands.rs:37, 83` | PRESENT — Parlay has a persistent reply keyboard (routing-table section B, e.g. row B3 "Settings") |
| Language reply keyboard | One row: "English" / "Russian" (each labelled in its own language with a flag word); tapping switches locale and replies "Your language is now ..." | `src/telegram/keyboards.rs:59-101`, command `src/telegram/handlers/commands.rs:115-120` | PRESENT — Parlay language picker (`pl:lang:*`, routing-table rows A59-A61) |
| Track-status inline row | Status-dependent: no status shows "Block track" + "Hide alerts for this track" (one per row); disliked shows only hide; ignored shows only block. Callback data is serialised JSON of the button enum | `src/telegram/inline_buttons.rs:58-88` | MISSING — no per-track status inline row in Parlay |
| Song-links inline button | "Links to other platforms" button appended to the details card | `src/telegram/actions/details.rs:285-296` | PRESENT — Parlay share buttons on cards (routing-table share rows) |
| Analyze inline button | "Analyze text" button appended to the details card when AI is configured | `src/telegram/actions/details.rs:336-344` | MISSING — no lyrics-analysis button in Parlay |
| Magic inline button | Magic action button (serialised `Magic` callback) | `src/telegram/inline_buttons.rs:17` (`Magic` variant), handler `src/telegram/handlers/inline_buttons.rs:140-142` | MISSING — no magic button in Parlay |
| Recommendasion inline button | Recommendation button (serialised `Recommendasion` callback) | `src/telegram/inline_buttons.rs:19`, handler `src/telegram/handlers/inline_buttons.rs:143-145` | MISSING — no recommendation button in Parlay |
| Skippage enable/disable button | Single toggle button whose label flips between enable and disable wording | `src/telegram/inline_buttons.rs:33-39` | UNCERTAIN — Parlay has autoplay on/off toggles, but no skippage toggle was verified |
| AI-slop three-option row | Notify / Ignore / Skip buttons, selected one prefixed with a check-mark | `src/telegram/inline_buttons.rs:40-56` | MISSING — no AI-slop row in Parlay |
| Admin users pagination/sort/filter keyboard | Page, sort-by, sort-order and filter buttons built from the `aup`/`aus`/`aub` callbacks | `src/telegram/actions/admin_users/list.rs:141+`, `src/telegram/inline_buttons_admin.rs:32-120` | MISSING — no admin-users keyboard in Parlay (pagination pattern itself exists for queue/settings) |
| Word-definition pagination/regenerate keyboard | Definition list pages (`wp`) and regenerate (`wd`) buttons | `src/telegram/inline_buttons_admin.rs:13-31` | MISSING — no word-definition keyboard in Parlay |
| Raw text shortcuts `+` / `-` | A bare `+` message likes, a bare `-` message dislikes the current track | `src/telegram/handlers/raw_message.rs:15-24` | MISSING — no bare `+`/`-` shortcuts in Parlay |

## 2.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Small-top link preview on details card | The details placeholder and final card carry a small link preview placed above the text, pointing at the Spotify track URL (which Telegram renders with the album art) | `src/telegram/actions/details.rs:143-152` (`link_preview_small_top`), helper `src/telegram/utils.rs` | UNCERTAIN — Parlay sends a real photo card when a thumbnail exists (`dispatcher.cpp:1681-1691`); whether Parlay ever uses a small-top link preview on a details-style card was not verified |
| Not applicable: cover-art fetch, thumbnail generation, photo sends | No `sendPhoto`, no cover download, no generated thumbnail exists in the telegram layer; artwork arrives only via Telegram's own link-preview rendering | grep of `src/telegram/` for photo/cover returns nothing | — |

## 2.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Details header + features block | Track/album links, features line (key + modality in code, BPM, six percentages), disliked-by / ignored-by counts, genre links | `locales/details.yml` (`details.header`, `details.features`), built in `src/telegram/actions/details.rs:287-330` | PRESENT — Parlay now-playing / track cards carry the same header-plus-facts shape |
| Details with-lyrics card | Header, profanity verdict, language, genre line, then lyrics with profane lines highlighted; the message is shortened line by line until it fits Telegram's message limit | `locales/details.yml` (`details.with-lyrics`, `details.no-lyrics`), `src/telegram/actions/details.rs:308-350` | PRESENT — Parlay lyrics card via `vc.lyrics` |
| Stats card with expandable languages blockquote | Stats lines plus a collapsible blockquote ("Languages stats:") listing per-language counts | `locales/actions.yml` (`actions.stats`, `<blockquote expandable>`) | UNCERTAIN — Parlay rich cards exist, but an expandable blockquote was not verified by grep |
| Admin users page footer | "Page N/M (Total: N users)" header line plus sort/filter line, and a footer hint giving the `/users [user-id]` usage | `src/telegram/actions/admin_users/list.rs:84-140` | PRESENT — Parlay queue/settings surfaces carry the same page-footer pattern (queue "Show N more", settings pagination rows) |
| Admin user details sections | Labelled sections (Basic Info, Configuration, plus language/AI-slop stats) with code spans for IDs and check/cross marks for booleans | `src/telegram/actions/admin_users/details.rs:80+` | MISSING — no per-user detail sections in Parlay |
| Help / not-found / unhandled templates | `/help` and unknown-command replies both render the localised command list with a header ("Commands available to you:" / "Command {cmd} not found. There are commands available to you:") using custom-emoji glyphs; unhandled non-command input gets "Your request was not handled" plus the same list, always with the start reply keyboard attached | `src/telegram/handlers/commands.rs:23-44, 72-83`, `src/telegram/handlers/message.rs:36-49`, strings `locales/commands.yml`, `locales/errors.yml` (`error.unhandled-request`) | PRESENT — Parlay `/help` and the unknown-command card (routing-table row C92) serve the same role |
| Login invite card | "Welcome to Rustify!" plus a feature bullet list (block songs, profanity notifications, AI detection, lyrics + AI analysis, Magic Playlist, Skippage) and a "Login with Spotify" URL button | `locales/login.yml` (`login.invite`, `login.button`, `login.success`), sent from `src/telegram/actions/login.rs` | MISSING — no login invite card in Parlay |
| Not applicable: copy pills | No dedicated copy-pill buttons exist; IDs appear in `<code>` spans, which Telegram renders as tappable-to-copy text, but that is formatting, not a button surface | `src/telegram/actions/admin_users/list.rs:105-124` | — |

## 2.6 Rich inline buttons (NoxBeats category)

Not this ref's category. Rustify's rich-card-with-buttons surfaces are the
details card (track-status row + song-links + optional analyze button) and
the admin users list/details screens, recorded above under Buttons and Rich
messages.

## 2.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| User unknown command: NOT silent — replies with "Command {cmd} not found." plus the full localised command list and the start reply keyboard | `src/telegram/handlers/commands.rs:23-44`, string `locales/commands.yml` (`command.not-found`) | PRESENT — Parlay unknown-command card (routing-table row C92) is the same behaviour |
| Admin unknown command: falls through to the user handler, so it also ends in the not-found reply above (the admin handler returns Skipped for unknown admin commands) | `src/telegram/handlers/admin_commands.rs:22-23`, then `src/telegram/handlers/message.rs:29` | PRESENT — same as above; Parlay answers unknown commands for every caller level |
| Unhandled non-command input: NOT silent — replies "Your request was not handled" plus the command list and the start keyboard (after url/keyboard/raw handlers decline) | `src/telegram/handlers/message.rs:36-49`, string `locales/errors.yml` (`error.unhandled-request`) | PRESENT — Parlay unknown-command card covers this case |
| Malformed command format: silent — a `ParseError::IncorrectFormat` returns Skipped with no reply | `src/telegram/handlers/commands.rs:45`, `src/telegram/handlers/admin_commands.rs:24` | MISSING — Parlay answers unknown/malformed commands with a card (row C92), it does not silently skip them |

---

# 3. telegram-music-bot-daslui — `refs/telegram-music-bot-daslui/` (Rust, teloxide)

Spotify-request voting bot: private users send a Spotify link, the bot
forwards it to a voting chat where members approve or decline it into the
Spotify queue. Only 3 source files (`src/main.rs`, `src/lib.rs`,
`src/spotify.rs`).

## 3.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Voting-chat gate (the only admin control) | The admin command handler (Help, SpotifyLogin) runs only for messages in the configured voting chat (`TELEGRAM_VOTING_CHAT_ID` + optional thread); there is no per-user admin check at all | `src/main.rs:75-99` (`admin_command_handler`, `is_voting_chat`) | MISSING — Parlay gates admin actions by owner/sudo role, not by a single configured chat id |
| `/spotifylogin` described as admin-only | Command description says "link spotify (admin only)"; enforcement is purely the voting-chat gate above — any member of that chat can run it | `src/main.rs:364` (description), `src/main.rs:119-137` (handler: posts the Spotify authorise URL, waits for the pasted redirect URL, replies "Token saved" or "Invalid Code/URL") | MISSING — Parlay has no Spotify-linking command |
| Not applicable: sudo list / add / remove / denial message | None exist; outsiders simply never reach the admin branch | `src/main.rs:75-104` | — |

## 3.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Spotify link request (private chat) | A private message matching a Spotify/open.spotify.com or spotify.link URL fetches the track and sends the requester "Successfully requested track." plus the track card, and forwards the request to the voting chat | `src/main.rs:176-241` (`request_track`), link test `src/main.rs:356-362` | PRESENT — Parlay vote cards (routing-table rows A75-A76) are the same request-and-vote pattern |
| Approve vote | Tapping approve adds the track to the Spotify queue and edits the voting message to "{voter} has added to queue:" plus the original text; queue failure edits it to "Failed to queue track: {error}" | `src/main.rs:258-300` (`accept:` branch) | PRESENT — routing-table row A75 (`pl:vote:approve`) |
| Decline vote | Tapping decline edits the voting message to "{voter} hat abgelehnt: {original text}" (German wording in source) and disables the link preview; keyboard is cleared on either outcome | `src/main.rs:301-311`, edit path `src/main.rs:313-333` | PRESENT — routing-table row A76 (`pl:vote:decline`) |
| Track card text | "Track name, duration M:SS, artists, album name, Listen link, Cover links" (one Cover link per album image) in HTML | `src/spotify.rs:150-196` (`format_track_text`) | PRESENT — Parlay track/now-playing cards carry title, artists, album and links |
| Not applicable: VC play/pause/skip/stop, queue display, progress, history | The bot never joins a call and never shows a queue; "queue" here is Spotify's own queue, written by the approve action | `src/main.rs:258-300` | — |

## 3.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Approve button | Single-row button "Add to queue" (check-mark word + text), callback `accept:{spotify-track-urn}` | `src/main.rs:205-224` | PRESENT — routing-table row A75 |
| Decline button | Same row, button "Decline" (cross-mark word + text), callback `decline`; both buttons are removed by the edit that follows either tap | `src/main.rs:205-224`, edit path `src/main.rs:313-333` | PRESENT — routing-table row A76 |
| Not applicable: reply keyboard, other inline surfaces | No reply keyboard and no other inline keyboard exists | grep of `src/` for keyboard types returns only the two buttons above | — |

## 3.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Album image as link preview | Both the requester confirmation and the voting message carry a link preview for the first album image, shown above the text (`show_above_text: true`, preview not disabled) | `src/main.rs:193-199, 225-239`, helper `src/main.rs:364-371` (`link_preview_for_url`) | UNCERTAIN — Parlay sends a photo card when a thumbnail exists; whether it ever uses an above-text link preview for a track card was not verified |
| Cover links in text | The track card text itself contains one "Cover" hyperlink per album image (plain links, not images) | `src/spotify.rs:175-190` | MISSING — Parlay renders the thumbnail as a photo, not as Cover hyperlinks in text |
| Not applicable: thumbnail generation, photo sends, fallback | No `sendPhoto`, no resizing, no generated thumbnail; if the album has no images the preview URL is simply absent | `src/spotify.rs:175-190` (images list may be empty) | — |

## 3.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Track card (HTML) | Bold track name, duration, bold artists, bold album, Listen link, Cover links (see Thumbnails) | `src/spotify.rs:150-196` | PRESENT — Parlay track cards |
| Voting message | "User {first name (@username)} requested:" plus the track card, with the approve/decline row | `src/main.rs:225-239` | PRESENT — Parlay vote card (rows A75-A76) |
| Accepted / declined edits | Accepted: "{voter} has added to queue:" + original text. Declined: "{voter} hat abgelehnt: {original text}" with preview disabled. Both edits clear the keyboard | `src/main.rs:258-333` | PRESENT — Parlay vote-accepted / vote-declined card edits (rows A75-A76) |
| `/help` auto-descriptions | Sends the auto-generated command descriptions ("These commands are supported:") — Help, SpotifyLogin, Id | `src/main.rs:340-344`, enum `src/main.rs:356-368` | PRESENT — Parlay help card |
| Spotify login prompt | "Spotify Login. Open this URL in the browser and allow Spotify access: {url}. Then paste and send the redirected URL here." | `src/main.rs:119-137` | MISSING — no Spotify login prompt in Parlay |
| Private-chat fallback help | Any private message that is not a Spotify link gets "Send a Spotify URL." | `src/main.rs:346-350` (`user_help`), wired at `src/main.rs:100-104` | MISSING — Parlay answers with its unknown-command card, not a "send a URL" prompt |
| Not applicable: collapsibles, copy pills, page footers | None exist; messages are short HTML with no sections, no pagination | — | — |

## 3.6 Rich inline buttons (NoxBeats category)

Not this ref's category. Daslui's rich-card-with-buttons surface is the
voting message (track card + approve/decline row), recorded above under
Buttons and Rich messages. Parlay's vote card (rows A75-A76) is explicitly
noted in the routing table as "backend-go:vote, daslui §23 — no in-core
builder", i.e. Parlay already ports this surface.

## 3.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| Private chat, unknown command: NOT silent — falls through to the private-message endpoint and gets "Send a Spotify URL." (the command handler only matches Help/Id, and the Spotify-link filter fails, so the catch-all endpoint answers) | `src/main.rs:80-104` (dispatch chain), `src/main.rs:346-350` | MISSING — Parlay answers an unknown-command card, not a "send a URL" prompt |
| Voting chat / group, unknown command: silent drop (inferred from the dispatch chain, not a dedicated handler) — the voting branch matches only Help/SpotifyLogin, the general command branch only Help/Id, and the private branch filter fails outside private chats, so no endpoint answers | `src/main.rs:75-104` | MISSING — Parlay never silently drops an unknown command (row C92) |
| Unknown callback data: silent — any callback data that is neither `accept:*` nor `decline` returns Ok(()) with no answer and no edit | `src/main.rs:312` (`_ => return Ok(())`) | MISSING — Parlay answers unknown callbacks with the "Unknown action." alert (routing-table row A95) |

---

# 4. telegram-music-bot-rust — `refs/telegram-music-bot-rust/main.rs` (single file, Rust, teloxide)

YouTube/TikTok downloader with audio-effect remixes. All UI text is
Russian except the command name. The only registered command is `/start`.

## 4.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `ADMIN_ID` bypass | A hardcoded admin id is exempt from the ban checks (`is_banned`, `check_auto_ban` return false for it). There are no admin commands and no way to change this id at runtime | `main.rs:20` (constant), `main.rs:168-180` | UNCERTAIN — Parlay has an owner id with full permissions, but an exemption-from-ban rule for the owner was not verified |
| Ban system (files, not commands) | Banned ids and usernames persist in `ban.txt`, all users in `users.txt`; repeat-request spam auto-bans a user and sends them "You are blocked for spam." | `main.rs:124-201` (`load_bans`, `save_bans`, `check_auto_ban`) | PRESENT — Parlay has blacklist/unblacklist sudo commands (`dispatcher.cpp:466-468`) |
| Banned-user silent drop | Banned users get no reply at all from the command handler, the text handler and the callback handler (each returns early) | `main.rs:332`, `main.rs:351`, `main.rs:387` | UNCERTAIN — Parlay blacklists exist, but the exact reply-or-silence behaviour for a blacklisted sender was not verified |
| Not applicable: sudo list / add / remove / denial message | None exist; the admin concept is only the ban exemption above | `main.rs:20`, `main.rs:129-140` (only `Start` in the `Command` enum) | — |

## 4.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Plain-text search (yt-dlp, 5 results) | Any non-stop-word text is treated as a search query: "Searching for track..." is sent, then edited into "Choose track:" with one button per result | `main.rs:378-381`, `main.rs:494-524` (`process_query`), search `main.rs:204-244` | PRESENT — Parlay search results card (routing-table row A73) |
| Link audio/video choice | A message containing youtube.com / youtu.be / tiktok.com gets "Link accepted! What exactly to download?" plus an Audio (MP3) / Video (MP4) choice row; the link is cached per chat+message id | `main.rs:361-376` | MISSING — Parlay plays replied media directly (`vc.play_file`, row C1) with no audio/video choice picker |
| Download and send file | The chosen file is downloaded with yt-dlp, sent as audio or video, the prompt message is deleted and the temp file removed; failures edit the prompt to "Download error." | `main.rs:394-429` | UNCERTAIN — Parlay has a music-download setting and file handling, but a download-and-send-audio reply was not verified as a user-facing flow |
| Remix effects picker | After picking a search result, the message becomes "Track: {bold title}. Choose processing variant:" with Original / Slow + Reverb / Nightcore / Bass Boost / 8D Audio buttons; the chosen effect is applied with ffmpeg and the result sent as audio | `main.rs:442-492` (buttons at 442-452, processing at 464-492) | MISSING — no audio-effect remix picker in Parlay |
| Search result picker rows | One button per result: "{n}. {title truncated to 35 chars} [{duration}]", callback `dl_{index}_{message_id}`; stale results edit the message to "Results are outdated." | `main.rs:506-523`, callback `main.rs:431-441` | PRESENT — Parlay search number buttons (row A73) |
| Not applicable: VC playback, queue, progress, history | Nothing is played into a call; "play" here means download-and-send | `main.rs:1-527` (no call handling) | — |

## 4.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Audio / Video choice row | One row, two buttons: "Audio (MP3)" (`get_audio_{message_id}`) and "Video (MP4)" (`get_video_{message_id}`); attached by editing the prompt message after it is sent | `main.rs:368-376` | MISSING — no audio/video choice row in Parlay |
| Search result rows | One button per result (see Plays), each on its own row | `main.rs:506-523` | PRESENT — Parlay search number buttons (row A73) |
| Remix effect rows | Row 1: Original + Slow + Reverb. Row 2: Nightcore + Bass Boost. Row 3: 8D Audio. Callbacks `remix_{effect}_{index}_{message_id}` | `main.rs:442-452` | MISSING — no remix rows in Parlay |
| Not applicable: reply keyboard | `/start` explicitly removes any reply keyboard (`KeyboardRemove`) | `main.rs:335-341` | — |

## 4.4 Thumbnails

No thumbnail component exists: `send_audio` / `send_video` are called with
no thumbnail argument, no cover art is fetched, and no image is attached to
any message. This category is inventoried as zero components.

## 4.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/start` text | "Hello! I am your music bot. Write a song name or drop a link to a track/video!" (Russian in source) plus reply-keyboard removal | `main.rs:335-341` | PRESENT — Parlay start card |
| Link-accepted prompt | "Link accepted! What exactly to download?" (Russian in source), later edited to carry the choice row | `main.rs:361-376` | PRESENT — Parlay prompt/status cards (e.g. share send prompt, row A19 family) |
| Searching status | "Searching for track..." (Russian in source), edited in place into the result list | `main.rs:378-381`, `main.rs:523` | PRESENT — Parlay search status/result cards |
| Choose-track message | "Choose track:" (Russian in source) with the result rows | `main.rs:523` | PRESENT — Parlay search results card (row A73) |
| Track-effect prompt | "Track: {bold title}. Choose processing variant:" in HTML, with the remix rows | `main.rs:465-470` | PRESENT — Parlay track cards with action rows (player/queue keyboards) as the shape equivalent |
| Status edits and errors | "Downloading..." / "Downloading and applying effect..." edits; "Link outdated." / "Results are outdated." / "Download error." / "Track download error." on failure; the prompt is deleted on success | `main.rs:408-429`, `main.rs:469-490` | PRESENT — Parlay status cards and error alerts (never a fake success, row C1 note) |
| Not applicable: collapsibles, copy pills, page footers | None exist; all messages are short status lines | — | — |

## 4.6 Rich inline buttons (NoxBeats category)

Not this ref's category. This ref's rich-card-with-buttons surfaces are the
choice row, the result rows and the remix rows, recorded above under
Buttons.

## 4.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| Unknown `/command`: NOT silent and NOT an error — because the only registered command is Start, any other `/foo` fails the command branch, passes the "has text" filter, and is treated as a search query (see Plays) | `main.rs:88-99` (dispatcher branches), `main.rs:344-381` (text handler) | MISSING — Parlay answers an unknown-command card (row C92); it never searches on an unknown command |
| Stop-words silent drop | Greeting/thanks words (Russian and English list) return with no reply | `main.rs:27-30` (list), `main.rs:360` | MISSING — Parlay has no stop-word silent drop |
| Non-text messages silent drop | The fallback endpoint returns Ok(()) with no reply for anything that is neither a command nor text | `main.rs:526-527` (`handle_fallback`) | UNCERTAIN — Parlay handles replied media, but the behaviour for other non-text input was not verified |
| Banned-user silent drop | As in Sudos: banned senders get no reply from any handler | `main.rs:332`, `main.rs:351`, `main.rs:387` | UNCERTAIN — same gap as the Sudos row above |

---

# 5. music-bot-navidrome — `refs/music-bot-navidrome/src/` (Python, python-telegram-bot v20+, HTML)

Private single-owner library bot: paste a music link, download via
Soulseek, tag, and import into a Navidrome library. No voice-chat playback.
The richest ref in this group for progress rendering, cancel flows,
lossy-fallback prompts and delete confirmations.

## 5.1 Sudos

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `ALLOWED_USERS` gate, silent | Every command/message/callback handler except `/start` is wrapped in the `authorized` decorator, which returns with no reply at all for users not in the env list | `src/bot.py:352-360` (decorator), `src/config.py:15-16` (env list) | PRESENT — Parlay owner/sudo permission gating (`dispatcher.cpp:459-473`); Parlay additionally answers denials, navidrome stays silent |
| `/start` unauthorised explainer | For a non-allowed user: "This is a private bot for managing a Navidrome music library. Your user ID: {id in code}. If you're the server admin, add this ID to ALLOWED_USERS in your bot configuration." plus a GitHub link; the message is sent with web-preview disabled | `src/bot.py:362-374` | PRESENT — Parlay admin denial cards (`kDenyAdmin`, routing-table row B3) |
| Owner = first allowed id (resume notices) | Pending/resumed background work is reported by DM to the first id in `ALLOWED_USERS` | `src/bot.py:1945-1947` | MISSING — Parlay has no resume-pending owner DM |
| Callback "Not allowed." toasts | Delete, lossy-fallback and cancel callbacks from a non-allowed user answer the toast "Not allowed." (no alert) and do nothing | `src/bot.py:923`, `src/bot.py:1678`, `src/bot.py:1797` | PRESENT — Parlay "You aren't allowed" callback alerts (routing-table rows A75-A76 pattern) |
| Not applicable: sudo list / add / remove commands | None exist; membership is changed only by editing the env list and restarting | `src/config.py:15-16`, `src/bot.py:1981-1991` (registered handlers) | — |

## 5.2 Plays (download equivalents; no VC playback)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Link download entry | A message containing a Tidal/Spotify/Apple/Deezer/YouTube/Shazam link resolves it and starts an album or track download; appending `re` forces re-download; duplicate in-flight work replies "Already queued." | `src/bot.py:821-838` (`handle_message`), `src/bot.py:1028-1054` (resolve), help text `src/bot.py:415-441` | PRESENT — Parlay play/download entry (routing-table row C1) |
| Queued status | "Queued — will start after the current download finishes" with a Cancel button, while the download semaphore is busy | `src/bot.py` (status lines section, queued branch) | PRESENT — Parlay queued card (row C1) |
| Live album progress (edited message, HTML) | Header "artist — title, N tracks"; search phase shows the last 4 searches with query, file/peer counts and pacing; download phase shows peer, covered/total tracks, quality, score, already-in-library count, then a per-track checklist (pending / downloading with percent + speed / remotely queued with queue position / done / failed with a humanised reason) and an ETA line; above 28 tracks it collapses to a counter plus active lines only | `src/reporting.py:101-311` (`AlbumProgress.render`) | UNCERTAIN — Parlay has `vc.progress` and refreshing now-playing cards, but a live per-track checklist with ETA was not verified |
| Live track progress (edited message, HTML) | Header plus searching lines, then peer/quality/match score (and copy count), percent + speed, or remotely-queued position + duration | `src/reporting.py:312-448` (`TrackProgress.render`) | PRESENT — Parlay `vc.progress` (`handlers/vc.go:530`) |
| Final album report | Status glyph (done / partial / failed) + bold "artist — title", "downloaded/wanted saved", "skipped already in library", format, "lyrics n/downloaded", elapsed, source counts, a failure block, scan note and share URL; capped at 1024 chars because it usually rides as a photo caption | `src/reporting.py:449-502` (`render_album_final`) | PRESENT — Parlay final/state cards (e.g. stopped card, row A25 family) |
| Final track report | Check-mark + bold "artist — title", format, "from {peer}", elapsed, scan note, share URL; capped at the caption limit | `src/reporting.py:503-560` (`render_track_final`) | PRESENT — Parlay track/state cards |
| Cancel flow | Every download/upload status message carries a Cancel button; toasts: "Nothing to cancel — already finished.", "Only the chat that requested this can cancel it." (alert), "Cancelling…"; final text reports what was already saved ("Cancelled — the track was already saved" / "Cancelled — nothing was downloaded" / "Cancelled — n/total tracks were already saved", plus a restore note) | `src/bot.py:107-158` (`ActiveRun`, `cancel_text`), `src/bot.py:1788-1826` (callback) | PRESENT — Parlay cancel/stop-confirm cards (rows A19, A40) |
| Lossy-fallback prompt | When only a lossy source is available, a prompt with an "Accept {summary}" button and a "Skip" button; accept runs the lossy download, skip drops it; expired prompts answer "Prompt expired." | `src/bot.py:1593-1714` (`_offer_mp3_fallback`, `_handle_lossy_callback`) | MISSING — no lossy-fallback prompt in Parlay |
| Re-tag progress + summary | Dry-run then `/retag confirm`: per-plan progress edits, then a summary of changed tags (sample of 8 plans) with elapsed time | `src/bot.py:536-660` (progress/summary builders), `src/bot.py:662-820` (command) | MISSING — no re-tag flow in Parlay |
| Scan triggers + status lines | `/scan` (Navidrome rescan) and `/sharescan` (slskd share rescan) with status lines: "Library scan triggered.", "Library scan not configured (NAVIDROME_USER/NAVIDROME_PASS not set).", "Library scan failed: wrong credentials.", "Library scan failed.", "slskd share scan triggered.", "slskd share scan scheduled in ~{mins}m." | `src/bot.py:443-453`, `src/bot.py:992-1027` (`_trigger_scan`) | MISSING — no library/scan commands in Parlay |
| `/stats` | Plain-text "Artists: n / Albums: n / Tracks: n / Size: n.n GB" | `src/bot.py:454-491` | UNCERTAIN — Parlay stats surfaces were not verified by grep |
| Inline now-playing (`np`) | Inline query `np` returns the currently playing library track as an audio result with cover thumbnail (see Thumbnails) | `src/inline.py:245-320` (`_ensure_cached`, audio result) | UNCERTAIN — Parlay inline mode has search and lyrics (routing-table section D); an inline now-playing audio result was not verified |
| Inline lyrics (`l`) | Inline query `l` returns a lyrics article for the current track (local file lyrics first, lrclib fallback, LRC timestamps stripped, capped at 4000 chars + "...") | `src/inline.py:477-517` | PRESENT — Parlay inline lyrics article (routing-table row D6 family, row B13) |
| Inline share (`s`) | Inline query `s` returns share links for the current track with cover art | `src/inline.py:568-599` | PRESENT — Parlay share card |
| Inline library search (`lib {name}`) | Inline search of the local library returning album and track articles with cover thumbnails and share links | `src/inline.py:519-567` | PRESENT — Parlay inline search (routing-table section D) as the shape equivalent |
| Inline delete (`del {name}`) | Inline search returning album/track articles whose selection sends a `delete:{path}` message, which opens the delete confirmation (see Buttons) | `src/inline.py:394-429` | MISSING — no inline delete in Parlay |
| Not applicable: VC play/pause/skip/stop, queue display, progress-to-audio, history | None exist; playback is Navidrome's job, not the bot's | `src/bot.py:1981-1991` (registered handlers) | — |

## 5.3 Buttons

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Cancel button | Single button "Cancel" (cross-mark word + text), callback `cancel:{12-hex run id}`, on every download/upload status message | `src/bot.py:120-126` (`cancel_row`) | PRESENT — Parlay cancel/stop-confirm buttons (rows A19, A40) |
| Delete confirmation row | Two buttons: "Delete" (wastebasket word + text, `rm:yes:{id}`) and "Cancel" (cross-mark word + text, `rm:no:{id}`); wrong-user taps get "Not allowed.", expired prompts "Prompt expired." | `src/bot.py:898-949` | PRESENT — Parlay stop-confirm card pattern (row A40 family) as the two-button-confirm shape equivalent |
| Lossy-fallback row | "Accept {summary}" (check-mark word + text, `lossy:accept:{id}`) and "Skip" (cross-mark word + text, `lossy:skip:{id}`) | `src/bot.py:1654-1655` | MISSING — no lossy-fallback row in Parlay |
| Inline shortcut keyboard (start + help) | Row 1: "Search" / "Library" / "Delete" (switch-to-inline-current-chat with ``, `lib `, `del `). Row 2: "Playing" / "Lyrics" / "Share" (prefixes `np`, `l`, `s`). Row 3: "Send playing to…" (switch-to-inline with `np`, opening the chat picker) | `src/bot.py:399-412` (`_inline_shortcuts`) | PRESENT — Parlay start card carries switch-inline buttons (routing-table row B13 family) |
| Inline hint button | Empty/short inline queries answer with zero results plus a button reading "np  s  l  lib  del — or type to search" that re-opens the bot with the `help` start parameter | `src/inline.py:344-356` (`_inline_hint`) | UNCERTAIN — Parlay inline hint behaviour was not verified |
| Not applicable: reply keyboard | None exists; all interaction is commands, inline mode, or the three callback rows above | `src/bot.py:1981-1991` | — |

## 5.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Final-report cover photo | The final album/track report is sent as a photo caption on `cover.jpg` from the album folder when that file exists | `src/bot.py:1073-1084` (`_send_result`), photo helper `src/bot.py:1068-1070` | PRESENT — Parlay photo card when a thumbnail exists (`dispatcher.cpp:1681-1691`) |
| Fallback to plain text | When no `cover.jpg` exists, the same report text edits the status message instead (no photo, no placeholder image) | `src/bot.py:1073-1084` (else branch) | PRESENT — Parlay text card fallback (`dispatcher.cpp:1691` comment: "No thumbnail, thumbnails off, or sendPhoto failed: text card.") |
| Inline now-playing cover thumbnail | The inline audio result carries the Navidrome cover art bytes as its thumbnail (`cover.jpg` name) | `src/inline.py:280-320` | UNCERTAIN — Parlay inline results with thumbnails were not verified |
| Inline article cover thumbnails | Album/track/library/share inline articles carry a Deezer cover URL as `thumbnail_url` (batch-fetched, one cover per album, shared by its tracks) | `src/inline.py:357-390` (`_fetch_album_covers`, `_album_result`, `_track_result`), `src/inline.py:408-467`, `src/inline.py:531-561` | UNCERTAIN — same gap as above |
| Embedded cover in tagged files | The tagger embeds downloaded cover bytes into the audio file itself when present | `src/library/tagger.py:174` (cover branch) | MISSING — Parlay does not tag files with cover art |
| Not applicable: thumbnail generation/resizing | No resizing or generation step exists; covers are used as downloaded (Deezer CDN URL or `cover.jpg`) | `src/library/files.py:43-53` (URL resize helper for display only) | — |

## 5.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| HTML everywhere | All bot messages use HTML parse mode; values are escaped before interpolation | `src/bot.py` (parse_mode="HTML" on sends/edits throughout, e.g. lines 371, 388, 437) | PRESENT — Parlay HTML cards |
| Album progress checklist + ETA | See Plays: per-track checklist lines, collapsed counter form above 28 tracks, ETA line "{done}/{total} saved, {failed} failed, ~{duration} left" | `src/reporting.py:101-311` | UNCERTAIN — same gap as the Plays album-progress row |
| Track progress card | See Plays: peer/quality/score/percent/speed or queued-position rendering | `src/reporting.py:312-448` | PRESENT — Parlay `vc.progress` |
| Final reports capped at caption limit | Album and track final reports are truncated to the 1024-char photo-caption limit, with a failure block budgeted inside that cap | `src/reporting.py:449-560` | PRESENT — Parlay card texts are built to fit message/caption limits (card builders in `dispatcher.cpp`) |
| Re-tag progress + summary | Per-plan progress text and a sampled summary of tag changes with elapsed time | `src/bot.py:536-660` | MISSING — no re-tag flow in Parlay |
| Help text | Sections for Download (link sources, `re` force flag, Cancel note), Inline mode (six `@{bot} ...` usages in code spans), Commands (`/scan`, `/sharescan`, `/stats`, `/retag` with the dry-run note) | `src/bot.py:415-441` | PRESENT — Parlay help card |
| Delete confirmation text | "Delete {label}?" style prompt (path label) with the Delete/Cancel row; outcome lines report the deleted album/track or the failure | `src/bot.py:898-949`, `src/bot.py:950-1027` | PRESENT — Parlay confirm cards (row A40 family) |
| Lossy-fallback summary text | Candidate summary (format/quality) in the prompt text, with the accept/skip row | `src/bot.py:215-230` (`_format_lossy_summary`), `src/bot.py:1593-1656` | MISSING — no lossy-fallback text in Parlay |
| Not applicable: copy pills, page footers | No dedicated copy-pill buttons and no paginated list footers exist; user ids and commands appear in `<code>` spans, and inline results are unpaginated (capped at 5 albums / 10 tracks per query) | `src/bot.py:362-374`, `src/inline.py:145-185` | — |

## 5.6 Rich inline buttons (NoxBeats category)

Not this ref's category. Navidrome's rich-card-with-buttons surfaces are
the Cancel button on progress messages, the delete confirmation row and the
lossy-fallback row, recorded above under Buttons.

## 5.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| Unknown command: silent drop — no handler is registered for unknown commands, and the text message handler explicitly excludes commands (`filters.TEXT & ~filters.COMMAND`), so an unmatched `/foo` is never answered | `src/bot.py:1987` (message handler filter), `src/bot.py:1981-1991` (handler list) | MISSING — Parlay answers an unknown-command card (row C92) |
| Unknown callback data: silent drop — only three callback patterns are registered (`lossy:`, `cancel:`, `rm:yes/no:`); anything else matches no handler | `src/bot.py:1989-1991` | MISSING — Parlay answers unknown callbacks with "Unknown action." (row A95) |
| Non-link plain text: silent — the message handler does nothing when the text contains no music link and no delete prefix | `src/bot.py:821-838` | MISSING — Parlay answers unknown input via its command/plain-text handling rather than dropping it silently (row C92 family) |

---

# 6. openradiobot — `refs/openradiobot/index.js` (single file, 396 lines, slimbot, Markdown)

Per-chat internet-radio bot. No authentication, no keyboards, no
thumbnails, no inline mode, no settings menu, no admin panel. Long messages
are split at 4096 chars and sent in sequence (`index.js:55-76` reply wrapper).

## 6.1 Sudos

No sudo/owner/admin component exists at all: every command is available to
every sender in the chat, and no user id is checked anywhere in the file.
Inventoried as zero components.

## 6.2 Plays

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| `/new` | Creates the chat's radio; "Radio Created" on success, "You already created your radio. To manage it, Type /manage. To destroy it, Type /destroy" when one exists | `index.js:80-144` | UNCERTAIN — Parlay has VC join and radio start (`vc.join`, `vc.radio` start), but no `/new`-style radio-creation command was verified |
| `/destroy` | Destroys the radio; "Radio destroyed." | `index.js:145-150` | PRESENT — Parlay stop/radio-stop (`vc.stop`, `vc.radio` stop, rows C7 and A25 family) |
| `/manage` status card | Markdown card: "Your radio status", Listener count, Total Listener, Loop Type, Created Since (human duration), Now Playing (linked title or "Voice/Audio Message"), Autoplay Enabled Yes/No, Total Queue, Live-on link (server host + chat id), footer "To check song queue, Type /queue" | `index.js:151-168` | PRESENT — Parlay now-playing/status cards |
| `/play` (search / URL / reply) | Usage line when no argument and no replied audio; "Searching {query}..." then "Preparing to play..."; YouTube search, playlist, raw stream URL and replied audio/voice branches | `index.js:221-317` | PRESENT — routing-table row C1 |
| Now-playing lines | "Now playing: [linked title](url)" (YouTube), "Now Playing: [Raw Stream](url)" (raw URL), "Playing Voice/Audio Message...." (attachment) | `index.js` (play stream `info` handler, ~lines 300-317) | PRESENT — Parlay now-playing card |
| Queued lines | "Voice has been added to queue" (attachment), "{count} Song has been added to queue" (playlist), "A stream URL has been added to queue." (raw URL), "[linked title](url) has been added to queue." (video/search) | `index.js:221-317` (per branch) | PRESENT — Parlay queued card (row C1) |
| `/pause` | "Paused"; guard replies "There's nothing playing. Glitched? Do /destroy" when nothing streams | `index.js:318-323` | PRESENT — row C5 |
| `/resume` | "Resumed"; same nothing-playing guard | `index.js:324-329` | PRESENT — row C6 |
| `/skip` | "Skipping..."; guards: nothing playing, then "There's nothing in queue!" when the queue is empty | `index.js:330-336` | PRESENT — row C3 |
| `/stop` | "Player Stopped"; clears the queue and destroys the stream; same nothing-playing guard | `index.js:337-343` | PRESENT — row C7 |
| `/queue` list + management | Empty: "Nothing is in queue....". List (max 20 shown): header "Radio Queue", numbered linked titles / "Raw Stream" / "Voice/Audio Message", footer warning that some songs are hidden plus a pointer to `/queue help`. Subcommands: `help` (usage + remove/move/shuffle/random), `remove {n}` (usage / not-found / removed lines), `move {from} {to}` (usage / invalid / moved lines), `shuffle`/`random` ("Queue order has been sorted randomly.") | `index.js:169-220` | PRESENT — Parlay queue card with "Show N more" and per-track remove (rows A41-A42); the `move` subcommand itself has no Parlay equivalent (queue actions are add/remove/clear/list, `handlers/vc.go:364`), but the row as a whole is the queue-management surface, so the verdict is PRESENT with that one subcommand gap noted |
| `/autoplay` | Toggles autoplay from YouTube related videos; "Autoplay enabled" / "Autoplay disabled"; refused on raw streams ("Sorry. You can't use autoplay right now.") — note the source checks a field that does not exist on the current-song object, so the refusal path is likely dead | `index.js:344-357` | PRESENT — `vc.autoplay`, row A28 |
| `/loop {queue|single|none}` | Usage line when the argument is missing/bad; "Loop Type has been set as {loopType}" on success | `index.js:358-368` | PRESENT — row C8 (`/loop`, `vc.loop`) |
| Not applicable: progress display, play history | No elapsed/total time, no seek, no history; the only position information is the `/manage` card | `index.js:151-168` | — |

## 6.3 Buttons

No button surface exists: no inline keyboards, no reply keyboards, no
callback handling anywhere in the file. Inventoried as zero components.

## 6.4 Thumbnails

No thumbnail component exists: no photo sends, no cover art, no image
fields. The "Live on" link in `/manage` is a plain Markdown link, not a
preview card. Inventoried as zero components.

## 6.5 Rich messages

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Markdown everywhere | All sends use `parse_mode: "Markdown"` via the reply wrapper | `index.js:55-76` | PRESENT — Parlay sends rich HTML cards; the format differs (HTML vs Markdown) but the rich-text-message role is the same |
| Manage status card | See Plays: labelled lines with code spans for counts and linked titles | `index.js:151-168` | PRESENT — Parlay status/now-playing cards |
| Queue list template | Header, numbered linked lines, hidden-songs warning footer | `index.js:169-220` | PRESENT — Parlay queue card (rows A41-A42) |
| Queue help text | "Queue Managing", usage `/queue [method] [argument]`, method list (remove / move / shuffle / random as alias) | `index.js:169-220` (`help` method) | PRESENT — Parlay queue card hints/help pattern |
| Default help text (also `/start` and `/help`) | "OpenradioBot v0.0 Alpha", Radio Managing section (`/new`, `/destroy`, `/manage`), Player Managing section (`/play`, `/pause`, `/resume`, `/skip`, `/stop`, `/queue`, `/autoplay`, `/loop`), with space-padded command columns | `index.js:369-395` (default case) | PRESENT — Parlay help card |
| Not applicable: collapsibles, copy pills, page footers | None exist; the queue footer is a warning + pointer, not a page footer, and there is no pagination (only the first 20 queue items) | `index.js:169-220` | — |

## 6.6 Rich inline buttons (NoxBeats category)

Not this ref's category, and this ref has no buttons at all (see Buttons).

## 6.7 Silent denials for unknown commands

| Behaviour | Ref location | Parlay verdict |
|---|---|---|
| Unknown command: NOT silent — the `default:` case answers with the full help text (this is also how `/start` and `/help` are answered; no dedicated handler exists for either) | `index.js:369-395` | PRESENT — Parlay unknown-command card (row C92) is the same behaviour (help text instead of silence) |
| Non-command or very short input: silent — messages not starting with `/`, or shorter than 3 chars, return before the switch with no reply | `index.js:77-78` | UNCERTAIN — Parlay plain-text handling for short/non-command input was not verified |

---

# 7. wavetune — `refs/wavetune/` (Next.js web app, JioSaavn data)

This checkout contains no Telegram bot code at all: no bot token, no
commands, no `sendMessage`, no inline keyboards. It is a web Mini-App-style
GUI. All rows below are web-UI components, mapped to their Parlay
equivalents (Mini-App state views `gui.*`, player cards, thumbnails) where
one exists. The README claim ("Add WaveTune to a group, /play, control via
the Web GUI") is not backed by any bot code in this checkout.

## 7.1 Sudos

No sudo/owner/admin component exists (no bot, no auth). Inventoried as
zero components.

## 7.2 Plays (web player)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Web player controls (icon-only) | Play, Pause, Previous, Next, Loop / Loop-one toggle, Seek forward/back, Download (spinner while downloading), Share, Save toggle (bookmark states) | `app/(player)/_components/Player.jsx:102-246` (controls), `components/cards/player.jsx` (same control set in card form) | PRESENT — Parlay player keyboard (rows A21-A34) covers play/pause/skip/loop/seek; download/share/save are separate Parlay surfaces (music-download setting, share card) |
| Seek slider + time labels | Slider bound to current time / duration, with formatted current-time and duration labels; position persists in local storage and resumes on reload | `app/(player)/_components/Player.jsx:18-19, 73-75, 142-148, 217-220` | PRESENT — Parlay `vc.seek` (+/-15s rows A26-A27) and `vc.progress` (`handlers/vc.go:530`) |
| Queue auto-fill from recommendations | On track end the queue drops its head and appends recommended suggestions, so playback continues without user action | `app/(player)/_components/Player.jsx:102, 116-120, 226, 244-246` | PRESENT — Parlay autoplay (`vc.autoplay`, row A28) |
| Album view | Album page title "Album - {name}" and a "{songCount} songs" count line | `app/(root)/album/[id]/page.jsx:8`, `app/(root)/album/_components/Album.jsx:35` | PRESENT — Parlay playlist/album-style cards (row C2 `/playplaylist` family) |
| Not applicable: Telegram now-playing card, queue display, play history | No Telegram card exists; the web "Next" card (`components/cards/next.jsx`) is the only up-next surface | `components/cards/next.jsx` | — |

## 7.3 Buttons (web)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Mobile bottom menu | Icon-only: Home (`/`) and Search (`/search/latest`) | `components/mobile-menu.jsx:8-9` | MISSING — Parlay Mini-App serving (`gui.*`) is read-only state views; no bottom navigation menu exists |
| Footer branding link | "Music hub" branding plus a "Deployed with love by" credit link to the author's GitHub | `components/page/footer.jsx:7,9` | MISSING — no footer branding link in Parlay |
| Note: player icon buttons | Counted under Plays above, not double-counted here; header search inputs are recorded under Rich messages | `app/(player)/_components/Player.jsx`, `components/page/search.jsx` | — |

## 7.4 Thumbnails

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Song card image | Card image from the JioSaavn image set, hover zoom, play overlay button; when no image, a skeleton placeholder block renders instead | `components/cards/song.jsx:8-26` | PRESENT — Parlay photo card when a thumbnail exists (`dispatcher.cpp:1681-1691`) |
| Album card image | Same pattern for albums (linked image, hover zoom); skeleton placeholder when no image | `components/cards/album.jsx:5-17` | PRESENT — as above |
| Player card image + blurred backdrop | Card art plus a second, blurred, enlarged copy of the same image as a dark-mode backdrop | `components/cards/player.jsx:112-113` | PRESENT — Parlay photo card (backdrop treatment itself is not in Parlay, but the thumbnail-on-card behaviour is) |
| Player page image + blurred backdrop | Large cover image with the same blurred-backdrop treatment on the full player page | `app/(player)/_components/Player.jsx:180-181` | PRESENT — as above |
| Fallback rule | Missing image renders a skeleton block, never a broken image; missing title/artist likewise render skeleton blocks | `components/cards/song.jsx:17-26, 27-40`, `components/cards/album.jsx:9-17` | PRESENT — Parlay text-card fallback when no thumbnail exists (`dispatcher.cpp:1691`) |

## 7.5 Rich messages (web copy)

| Component | What it does | Ref location | Parlay verdict |
|---|---|---|---|
| Section headings | "Songs / Top new released songs.", "Albums / Top new released albums.", "Artists / Most searched artists.", "Trending / Most played songs in this week." | `app/(root)/page.js:40-41, 66-67, 92-93, 148-149` | PRESENT — Parlay card headings (now-playing, queue, settings headers) |
| Search UI copy | Header placeholder "Try Maharani..", player-page placeholder "Search for songs by name...", "Searching...", "No results found!", "Search results for {query}" (query highlighted), "Search Results" page heading | `components/page/search.jsx:38`, `app/(player)/_components/AdvanceSearch.jsx:77-109`, `app/(root)/search/_components/Search.jsx:37` | PRESENT — Parlay search status/results cards (row A73 family) |
| Album title + count | "Album - {name}" page title and "{songCount} songs" line | `app/(root)/album/[id]/page.jsx:8`, `app/(root)/album/_components/Album.jsx:35` | PRESENT — Parlay playlist/album card titles |
| Not applicable: collapsibles, copy pills, page footers, Telegram formatting | None exist in the web UI; pagination is by web routing, not message footers | — | — |

## 7.6 Rich inline buttons (NoxBeats category)

Not this ref's category, and this ref has no Telegram buttons at all. Its
web icon buttons are recorded under Plays and Buttons.

## 7.7 Silent denials for unknown commands

Not applicable: there is no bot and therefore no command handling, no
unknown-command behaviour, and no denial behaviour to inventory. The web
search path answers empty results with "No results found!" (see Rich
messages), which is the closest analogue and is not a denial.

---

# Cross-ref notes for the port worker

1. **Panel shape (remix) is already Parlay's panel.** Remix's 9-button panel
   maps 1:1 onto Parlay's `pl:player:*` rows A21-A34 (shuffle, skip, pause,
   resume, mute, unmute, voldec, volinc, refresh) plus `vc.panel`. The only
   remix panel behaviour Parlay changes is the requester-only filter:
   remix silently ignores other users' taps, Parlay answers "You aren't
   allowed".
2. **Unknown commands are the systematic divergence.** Remix, navidrome and
   (for groups) daslui drop unknown commands silently; Parlay answers an
   unknown-command card (row C92). Rustify, openradiobot and (for private
   chats) daslui answer help text instead of silence. Nothing in Group C
   needs a new silent-drop behaviour ported; the port direction is to keep
   Parlay's card.
3. **Sudo models differ and do not merge.** Remix uses live Telegram group
   admins; rustify uses a database role; navidrome uses an env allow-list;
   daslui uses a single voting chat; telegram-music-bot-rust uses one
   hardcoded admin id exempt from bans; openradiobot and wavetune have none.
   Parlay's owner/sudo/auth-user store already covers the navidrome and
   rustify shapes; the remix live-admin sync and the daslui voting-chat
   gate are the two unported shapes, both marked MISSING above.
4. **Thumbnails: Parlay is ahead of every ref except navidrome/wavetune.**
   Remix, openradiobot and telegram-music-bot-rust have no thumbnails at
   all; daslui and rustify rely on Telegram link previews. Navidrome's
   cover-photo-caption-with-text-fallback is exactly Parlay's
   `sendPhoto`-or-text-card rule, and wavetune's skeleton fallback is the
   web analogue of Parlay's text-card fallback.
5. **No ref in this group has copy pills or collapsible sections except
   rustify's expandable languages blockquote** (stats card) — that single
   pattern is UNCERTAIN for Parlay and is the only collapsible worth
   checking against Parlay's card builders.
