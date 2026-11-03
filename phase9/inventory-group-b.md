# Phase 9 — Telegram UI/UX Inventory, Group B (7 refs)

**Scope:** TGVCBot, remix, ParvatiMusic, telegram-music-bot-rust, feel-me,
telegram-music-bot-daslui, rustify — every user-visible Telegram UI/UX element
harvested verbatim. Source root: `~/workspace/parlay/refs/<ref>/`.
**Rules used:** exact strings incl. emojis/formatting; button labels + layout +
`callback_data`; file path + line numbers where known; "no UI for this category"
stated explicitly per ref. No code was edited, nothing pushed, no `.env` touched.

## Cross-ref conventions worth knowing before porting

- **ParvatiMusic** renders almost all text in Unicode small-caps (e.g. `ᴘʟᴧʏ`,
  `sᴛᴧʀᴛ`) — the strings below are copied exactly, small-caps included. Its
  locale files use `{emoji:🎵}` placeholders (replaced at render time with the
  emoji) and `{name}` placeholders. **All callback payloads are base64-encoded**
  (`buttons.rs`: `data_btn` → `base64_encode(cb)`); the decoded payloads are
  shown below (e.g. `room:<id>:pause`). Button style is random per button
  (1=blue primary, 2=red danger, 3=green success) unless `DISABLE_COLOUR`.
- **remix** uses Fluent (`.ftl`) with HTML parse mode; `{-error(t:…)}` renders
  `❌ | <b>…</b>` and `{-response(e,t)}` renders `{emoji} | <b>…</b>`.
- **rustify** uses `t!("key", locale = …)` with `en`/`ru` locales and `%{var}`
  placeholders; inline-button `callback_data` is a **JSON-serialized enum**
  (e.g. `{"Dislike":"<track_id>"}`), not a plain string.
- **telegram-music-bot-rust** and **feel-me** are Russian-language bots; strings
  are quoted in Russian as written.
- **telegram-music-bot-daslui** mixes English UI with one German decline string
  (verbatim below).

---

# 1. TGVCBot — `refs/TGVCBot/apps/tgvcbot/src/`

TypeScript / grammy. No settings menu, no admin panel, no inline mode, no
lyrics, no download-progress messages, no stats/ping, no auth/sudo, no
playlists, no broadcast, no maintenance mode. ~30 user-visible strings.

## 1.1 /start — `handlers/start.ts:17-33`

Private or group. HTML parse mode, `disable_web_page_preview: true`.
Text (verbatim, `{first_name} {last_name}` may be `null` → literal "null" if
last name missing):

```
Hi <a href="tg://user?id={id}">{first_name} {last_name}</a>
I Play Songs in Group Voice Chats.
If you like this bot consider adding a star to the <a href="https://github.com/ArnabXD/TGVCBot">repository</a>
```

Buttons: 1 row, 1 URL button:

| Label | Type | URL |
|---|---|---|
| `View Source` | url | `https://github.com/ArnabXD/TGVCBot` |

## 1.2 /help — `handlers/help.ts:5-15` (full text, HTML)

```
<b>Commands Available :</b>

/jiosaavn or /jsvn : Play songs from JioSaavn
/play : Reply this command to audio files to play the file
/queue or /q : Check queued songs list
/p : Pause the stream
/r : Resume the stream
/next or /skip : Skip the current song
/stopvc : Stop the stream
/leave : Leave VC (works if stream is finished but VC user not left)
/help : Show this Menu
```

Note: `/help` text omits `/shuffle`, `/radio`, `/youtube`, `/jiosaavnsearch`,
`/ytsr` which exist in code.

## 1.3 Now-playing card — `utils/messages.ts:10-38` (`sendPlayingMessage`)

Sent as a **photo** (`sendPhoto`, file name `thumbnail.png`) with the caption
below; on banner-generation failure falls back to plain `sendMessage` with the
same text. Banner is a generated 600×300 image: blurred cover background,
180×180 thumb at (30,60), title (Poppins 26px) at (220,55), artist below it,
watermark text (`env.WATERMARK`, 14px) bottom-right — `utils/banner.ts:22-68`.
Caption (HTML):

```
Playing <a href="{link}">{title}</a>
<b>&#10143;</b> Duration : {hhmmss(duration)}
<b>&#10143;</b> Requested by <a href="tg://user?id={id}">{first_name}</a>
```

Duration format (`utils/hhmmss.ts`): seconds → `Xh Ym Zs` (omits zero parts);
if input already contains `:` it is passed through; non-numeric → `∞`.
**No inline buttons on the now-playing card.**

## 1.4 Queue display — `handlers/queue.ts:8-28` (`/queue`, `/q`)

Empty: `Queue is empty` (plain, HTML parse mode set but no tags).
Non-empty (HTML, `disable_web_page_preview: true`):

```
<b><i>Queue List : </i></b>

<b>1 :</b> <a href="{link}">{title}</a>
Requested by : <a href="tg://user?id={req_by_id}">{req_by_fname}</a>

<b>2 :</b> …
```

Commented-out (dead) `/delete|/remove|/d` handler in the same file would have
replied `Command Example :\n/del 2`, `Invalid queue position`, and
`Deleted <a href="{link}">{title}</a> from queue`.

## 1.5 Search results — `handlers/jiosaavn.ts:47-79`, `handlers/youtube.ts:43-80`

Commands: `/jiosaavn|/jsvn` (plays first result immediately),
`/jiosaavnsearch|/jsvnsearch|/jsvnsr`, `/youtube|/yt`, `/ytsr|/ytsearch`
(top 10, numbered buttons). Shared strings:

- Private chat: `This Command works on Group Only`
- No query: `Please Provide Search Keyword`
- No results: `No Results Found`
- Wrong-user button tap (alert): `You aren't allowed` (`show_alert: true`)

JioSaavn search header + rows (HTML):

```
Search Results for <b>{query}</b>

01 : <b><a href="{perma_url}">{title}</a></b>
<b>By :</b> {singers}

02 : …
```

YouTube search header + rows:

```
Search Results for <b>{query}</b>

01 : <b><a href="https://youtu.be/{id}">{title}</a> ({durationFormatted})</b>
<b>By :</b> {artist}
```

Keyboard: number buttons `1`…`10` as text, `callback_data`
`jsvn:{requester_user_id}:{song_id}` or `yt:{requester_user_id}:{video_id}`;
5 buttons per row (`.row()` after every 5th). On valid tap the search message
is deleted (`ctx.deleteMessage()`).

## 1.6 Play / controls / radio / leave — exact replies

| Command | File:line | Reply |
|---|---|---|
| `/play|/pl` reply-to-audio | `handlers/play.ts:10-45` | Private: `This Command works on Group Only`; no reply: `Please reply this command to a audio file` (sic). Audio fallback title `Unknown`, artist fallback `TGVCBot`, provider `telegram`. |
| `/pause|/p` | `handlers/controls.ts:17-19` | `Paused` / `Not Playing` |
| `/resume|/r` | `handlers/controls.ts:21-23` | `Resumed` / `Not Paused` |
| `/skip|/next` (queue empty) | `handlers/controls.ts:25-44` | Stops stream, no message |
| `/shuffle` | `handlers/controls.ts:46-54` | `Shuffled the playlist` / `No Playlist to shuffle` |
| `/stopvc` | `handlers/controls.ts:56-61` | `Stopped` (only if stop succeeded) |
| `/radio|/stream` | `handlers/radio.ts:13-46` | Private: `This Command works on Group Only`; no link: `Please provide a radio/stream link`; non-http: `Invalid`. Stream title is literally `Radio`, artist `...`, duration empty. |
| `/leave` | `handlers/leave.ts:13-33` | Non-group: `This Command works on Group Only`; stream unfinished: `Stream isn't finished yet`; no call: `No Active Call`; success: `Left Voice Call` |

Middleware guard (`middlewares/inactiveVc.ts:9-22`, applies to pause/resume/
skip/shuffle): private → `This Command works on Group Only`; no VC →
`Inactive VC`.

## 1.7 Errors

- Stream failure (`utils/messages.ts:40-47`, `sendFailedToStreamMessage`,
  MarkdownV2): `Failed to stream the song\n` + `` ` `` + `{error.message}` + `` ` ``
- Global error handler (`middlewares/errorHandler.ts:11-28`): replies to the
  user with `{err.message}\n<code>{JSON.stringify(err, null, 2) escaped}</code>`
  (HTML); if the error matches `'sendChatAction' failed!` the bot leaves the
  chat and only logs to `LOG_CHANNEL` instead.
- Log channel: `bot.ts:14-21` — `log()` sends to `env.LOG_CHANNEL` if set,
  else `console.log`.

## 1.8 Absent in TGVCBot

No lyrics, no settings/admin panel, no inline-mode results, no reply
keyboard, no download progress, no stats/ping/alive, no playlists, no
broadcast, no maintenance, no auth/sudo messages, no channel-play, no voting.

---

# 2. remix — `refs/remix/src/` + `refs/remix/locales/en/messages.ftl`

TypeScript / grammy. **No /start, no /help, no inline mode, no settings menu,
no admin panel, no stats/ping, no playlists, no broadcast, no auth/sudo.**
All user text lives in `locales/en/messages.ftl` (167 lines, Fluent, HTML parse
mode assumed). Full file is reproduced verbatim in **Appendix B**.
Command inventory (from `src/bot/handlers/`):

- Stream: `/stream|/s|/play|/p` (`stream.ts`), `/playlist|/pl|/list`
  (`playlist.ts`), `/search|/find` + `/cancel` (`search.ts`)
- Controls (admin-only, see §2.6): `/pause`, `/resume|/re|/res|/continue`,
  `/skip|/next`, `/leave|/stop`, `/volume|/vol|/v`, `/mute|/m`, `/unmute|/um`,
  `/shuffle|/sh|/mix`, `/loop|/repeat`, `/menu|/control|/controls|/panel`
- Info: `/now|/ns|/cs|/np|/cp` (`now.ts`), `/ly|/lyrics` (`lyrics.ts`),
  `/cache|/caches` (`cache.ts`)

## 2.1 Shared wrappers — `locales/en/messages.ftl:1-12`

```
-raw-not-in-call   = Not in call.
-raw-not-streaming = Not streaming.
-error             = ❌ | <b>{$t}</b>
-ok                = ✅ | <b>{$t}</b>
-response          = {$e} | <b>{$t}</b>
-not-in-call       = {-error(t: "Not in call.")}
-not-streaming     = {-error(t: "Not streaming.")}
not-in-call     = {-not-in-call}        →  ❌ | <b>Not in call.</b>
not-streaming   = {-not-streaming}      →  ❌ | <b>Not streaming.</b>
errors =
    .no-call        = {-error(t: "No active call.")}
    .no-video-found = {-error(t: "No video found.")}
    .unknown        = {-error(t: "An error occurred.")}
    .file-too-big   = {-error(t: "This file is too big.")}
    .no-assistant   = {-error(t: "My assistant is not here.")}
    .error          = {-error(t: "An error occurred:")} {$message}
```

Error mapping (`src/bot/errors.ts`): `No active group call` →
`errors.no-call`; `No video id found` → `errors.no-video-found`;
`Too big` → `errors.file-too-big`; `Could not find the input entity for` →
`errors.no-assistant`.

## 2.2 Now-playing — `now.ts:10-22`, `messages.ftl`

`/now` reply when streaming:

```
🎵 | Currently streaming <b><a href="{$titleUrl}">{$title}</a></b> by <b><a href="{$requesterUrl}">{$requester}</a></b>...
```

When idle: `not-streaming` (`❌ | <b>Not streaming.</b>`). No thumbnail, no
buttons on this message. (Note: remix has **no now-playing card on track
start** — only `/now` and the control panel.)

## 2.3 Control panel — `handlers/panel.ts` (the richest UI in remix)

Command: `/menu|/control|/controls|/panel` (admin-only, reply-to the command
message). Text (`messages.ftl`, `panel.text`):

```
{$nowEmoji} | <b><a href="{$nowUrl}">{$now}</a></b>
➡️ | <b><a href="{$nextUrl}">{$next}</a></b>
```

`{$nowEmoji}` is `🔁` when loop is on, else `🎵`. Fallbacks:
`panel.nothing-now = Nothing streaming now`,
`panel.nothing-next = Nothing streaming next`. `disable_web_page_preview: true`.

Inline keyboard — 3 rows, emoji-only labels (same object duplicated in
`src/bot/constants.ts:16-33` as `panelOther`):

| Row | Buttons (label → `callback_data`) |
|---|---|
| 1 | `🔀` → `panel_shuffle`, `⏩` → `panel_skip`, `⏸` → `panel_pause`, `▶️` → `panel_resume` |
| 2 | `🔇` → `panel_mute`, `🔈` → `panel_unmute`, `🔉` → `panel_voldec`, `🔊` → `panel_volinc` |
| 3 | `🔄` → `panel_update` |

Callback behaviour (`panel.ts:87-178`): only the user who sent the replied-to
command message may press (others silently ignored — filter fails, no alert).
Answers are `answerCallbackQuery` toasts (not messages):

- update → toast `Updated.` (`panel.updated`); edits the panel text in place.
- shuffle (fail) → toast `No enough items to shuffle.`; (ok) → toast
  `Shuffling...`
- skip/pause/resume/stop/mute/unmute → toast is the `panel.<action>` select:
  `true` → `Paused.` / `Resumed.` / `Skipped.` / `Stopped.` /
  `Muted.` / `Unmuted.`; `false` → `Not streaming.` (raw, no ❌);
  `null` → `Not in call.` (raw).
- volinc/voldec → toast `Volume set to {$amount}.` (amount = internal volume
  /100, rounded; increment: current+1000, default 10000, cap 20000; decrement:
  current−1000, default 5000, floor 1); not in call → toast `Not in call.`

## 2.4 Text-command control replies — `handlers/controls.ts`, `messages.ftl`

Same three-way selects as the panel, but rendered as **messages** with emoji
prefixes:

```
pause:  true  → ⏸ | <b>Paused.</b>        false → ❌ | <b>Not streaming.</b>   null → ❌ | <b>Not in call.</b>
resume: true  → ▶️ | <b>Resumed.</b>       false → ❌ | <b>Not streaming.</b>   null → ❌ | <b>Not in call.</b>
skip:   true  → ⏩ | <b>Skipped.</b>       false → ❌ | <b>Not streaming.</b>   null → ❌ | <b>Not in call.</b>
stop:   true  → ⏹ | <b>Stopped.</b>       false → ❌ | <b>Not streaming.</b>   null → ❌ | <b>Not in call.</b>
mute:   true  → 🔇 | <b>Muted.</b>         false → ❌ | <b>Already muted.</b>    null → ❌ | <b>Not in call.</b>
unmute: true  → 🔈 | <b>Unmuted.</b>       false → ❌ | <b>Not muted.</b>       null → ❌ | <b>Not in call.</b>
loop:   true  → 🔁 | <b>Turned loop on.</b>   false → 🔁 | <b>Turned loop off.</b>
```

Volume (`/volume|/vol|/v <0–200>`): ok → `🔈 | <b>Volume set to {$amount}.</b>`;
invalid → `❌ | <b>Pass a number between 0 and 200.</b>`; not in call →
`❌ | <b>Not in call.</b>`.
Shuffle: ok → `🔀 | <b>Shuffling...</b>`; too few items →
`❌ | <b>No enough items to shuffle.</b>` (sic).

## 2.5 Search — `handlers/search.ts:11-93`, `messages.ftl`

`/search|/find <query>` (YouTube, max 10). Guard strings:

- Active search exists: `❌ | <b>There is an active search.</b>`
- No query: `❔ | <b>What do you want to search for?</b>`
- No results: `❌ | <b>No results found.</b>`

Header: `<b>🔍 | Search results for {$query}...</b>` then per result:

```
{$numberEmoji} <b><a href="{$url}">{$title}</a></b>
  ├ {$durationEmoji} {$duration}
  ├ 👁 {$views}
  ├ 📅 {$uploadTime}
  └ 👤 {$uploader}
```

`{$numberEmoji}` = `1️⃣`…`9️⃣`,`🔟` (`constants.ts:6-17`); duration emoji
`🔴` if live else `🕓`; live duration rendered `Live`; missing duration/views/
uploader → `N/A`; views humanized (`helpers/humanize.ts`: ≤3 digits raw,
`K`/`M`/`B`/`T` suffix by magnitude); titles truncated to 70 chars + `...`
(`helpers/text.ts`). Footer:
`<i>Reply the number of the result you want to stream or /cancel.</i>`
Interaction is **reply-a-number**, no buttons. Replying a valid number streams
it and deletes the search message; then replies `▶️ | <b>Streaming...</b>` or
` #️⃣ | <b>Queued at position {$position}.</b>`.

`/cancel`: with active search → deletes search message, replies
`✅ | <b>Search canceled.</b>` (note trailing comma in the .ftl source);
without → `❌ | <b>There is no search active.</b>`

## 2.6 Stream / playlist / lyrics / cache

- `/stream|/s|/play|/p` (`stream.ts:11-38`): no input →
  `❔ | <b>What do you want to stream?</b>`; success → `▶️ | <b>Streaming...</b>`;
  queued → ` #️⃣ | <b>Queued at position {$position}.</b>` (leading space in
  source). Input can be replied audio/voice/text or a URL/query; the literal
  word `custom` + second arg streams a custom input (`inputs.custom =
  Custom Input`; `inputs.audio-file = Audio File`;
  `inputs.voice-message = Voice Message`).
- `/playlist|/pl|/list <url>` (`playlist.ts:11-38`): first item →
  `🎶 | <b>Streaming and queuing {$x} items...</b>` if it started streaming,
  else `🎶 | <b>Queuing {$x} items...</b>`. No reply at all if no URL given.
  Capped at `env.MAX_PLAYLIST_SIZE`.
- `/ly|/lyrics` (`lyrics.ts:11-31`): not streaming → `❌ | <b>Not
  streaming.</b>`; lookup failure → `❌ | <b>Lyrics not found.</b>`; success
  (chunked at 4096 chars):
  ```
  <b>{$title}</b> #lyrics

  {$lyrics}
  ```
- `/cache|/caches` (`cache.ts:7-12`, admin-only): resets session
  (admins/search/loop) and replies `🗑 | <b>Caches deleted.</b>`

## 2.7 Admin gating — `handlers/index.ts:27-60`

- Stream/playlist/now/search/lyrics: group chats only (filter
  `chat.type.includes('group')`).
- Panel/cache/controls: only chat creator or administrator with
  `can_manage_video_chats`, non-anonymous; the admin list is cached in
  session and kept in sync by `chat_member_updated.ts` (no user message).
  Non-admins are silently ignored (no denial message).
- **No /start, /help, inline mode, settings, stats, playlists (persistent),
  broadcast, or error strings beyond §2.1.**

---

# 3. ParvatiMusic — `refs/ParvatiMusic/bot/`

Rust (own MTProto stack). **The largest UI surface in Group B: 439 locale keys
in `bot/locales/en.yml` (1112 lines) + a 69-entry per-command help registry in
`bot/src/modules/help_texts.rs`.** The full `en.yml` is reproduced verbatim in
**Appendix A**; the help registry key list is in §3.9. All strings below use
the small-caps font and `{emoji:…}` placeholders exactly as stored.
**No lyrics feature, no inline-query results** (the word "inline" here means
inline *buttons* only — there is no inline-mode result handler anywhere in
`bot/src/`).

## 3.1 /start — `modules/basic.rs:33-74`, locale `start_private` / `start_group`

Private chat (`start_private`, `en.yml:998`):

```
<blockquote><b>sᴧʟᴜᴛᴧᴛɪσɴs</b> {user},</blockquote>
<blockquote expandable><b>✧ ᴡᴇʟᴄσᴍᴇ ᴛσ {bot} — ᴧ ᴘσᴡᴇʀғᴜʟ ᴧɴᴅ ʜɪɢʜ-sᴘᴇᴇᴅ ᴛɢ ᴍᴜsɪᴄ ʙσᴛ</b>
<b>✧ ʙᴜɪʟᴛ ғσʀ sᴍσσᴛʜ • sᴛᴧʙʟᴇ • ʟᴧɢ-ғʀᴇᴇ ᴍᴜsɪᴄ sᴛʀєᴧᴍɪɴɢ</b>
<b>✧ ᴘσᴡᴇʀєᴅ ʙʏ ᴧɴ σᴘᴛɪᴍɪᴢєᴅ ʏσᴜᴛᴜʙє ᴧᴘɪ ғσʀ ɪɴsᴛᴧɴᴛ ᴘʟᴧʏʙᴧᴄᴋ</b>
<b>✧ єɴᴊσʏ ʜɪɢʜ ǫᴜᴧʟɪᴛʏ ᴧᴜᴅɪσ ᴡɪᴛʜ sєᴧᴍʟєss ᴄσɴᴛʀσʟ</b>
<b>•──────────────•</b>
<b>✧ ᴜsᴇ ʜᴇʟᴘ ᴛσ ᴠɪєᴡ ᴧʟʟ ᴄσᴍᴍᴧɴᴅs ᴧɴᴅ ғєᴧᴛᴜʀєs</b></blockquote>
```
(trailing space after the final `</blockquote>` in source.)

Group chat (`start_group`, `en.yml:1001`):
`<b>{emoji:💫} ɪ’ᴍ ʜєʀє!</b> ᴛʀʏ ᴧ ᴄσᴍᴍᴧɴᴅ… ɪғ ʏσᴜ ᴛʜɪηᴋ ʏσᴜ ᴄᴧη ʜᴧηᴅʟє ᴍє {emoji:😉}`

Start keyboard — `buttons.rs:229-248` (`start_markup`, 4 rows, random colour
per button):

| Row | Buttons |
|---|---|
| 1 | `⟡₊ ᴧᴅᴅ ϻє ᴛσ ʏσυꝛ ᴄʜᴧᴛ ₊⟡` (`ADD_ME_BTN`) → url `https://t.me/{bot}?startgroup&admin=invite_users` |
| 2 | ` ʜєʟᴘ & ᴄσᴍᴍᴧηᴅs` (`START_HELP_BTN`) → callback `help_cb` (base64-encoded) |
| 3 | ` ᴜᴘᴅᴧᴛєs` (`UPDATES_BTN`) → url `{cfg.support_channel}` ; `sᴜᴘᴘσʀᴛ` (`SUPPORT_BTN`) → url `{cfg.support_chat}` |
| 4 | ` sσᴜʀᴄє ᴄσᴅє` (`SOURCE_BTN`) → url `https://github.com/Iotatg/ParvatiMusic` |

Other shared markups (`buttons.rs`):

- `add_me_markup`: single URL button `ADD_ME_BTN` → same `?startgroup&admin=invite_users` URL.
- `supp_markup`: single URL button `SUPPORT_BTN` → `{cfg.support_chat}`.
- `repo_markup`: row [`ᴅєᴠєʟσᴘєʀ` (`REPO_DEV_BTN`) → dev URL fallback chain
  (explicit arg → `cfg.dev_url` → `https://t.me/{owner_username}` →
  `tg://openmessage?user_id={owner_id}` → `https://t.me/+Q493A_VBAUpkMmRl`),
  `ʀєᴘσ` (`REPO_BTN`) → `https://github.com/Iotatg/ParvatiMusic`].
- `group_help_keyboard`: single URL button ` ᴄσᴍᴍᴧηᴅs` (`GC_HELP_BTN`) →
  `https://t.me/{bot}?start=pm_help`.
- `cancel_keyboard`: `🚦 ᴄᴧηᴄєʟ ᴅσᴡηʟσᴧᴅɪηɢ` (`DOWNLOAD_CANCEL_BTN`) → callback `cancel`.
- `broadcast_cancel_keyboard`: `🚫 ᴄᴧηᴄєʟ ʙʀσᴧᴅᴄᴧsᴛ`
  (`BROADCAST_CANCEL_BTN`) → callback `bcast_cancel`.

## 3.2 /help — `modules/basic.rs:103-167`, `help_texts.rs`, locale `help_*`

- In a group, `/help` replies `help_private_only`:
  `{emoji:🤖} ʜɪ! ғσʀ ʙσᴛ ʜєʟᴘ ᴧηᴅ ᴄσᴍᴍᴧηᴅs, ᴘʟєᴧsє ᴅᴍ ᴍє ᴅɪʀєᴄᴛʟʏ — ɪ'ᴍ ᴍσʀє ʀєsᴘσηsɪᴠє ɪη ᴘʀɪᴠᴧᴛє ᴄʜᴧᴛs!`
  (with the group-help URL button, §3.1).
- `/help <command>` → per-command text from `help_texts.rs`
  (`format_help_text`); unknown command:
  `⚠️ <i>No help found for command <code>{cmd}</code></i>`.
- Main help screen (`help_main`, `en.yml:400`):
  ```
  <blockquote><b>🔮 єxᴘʟσʀє ᴛʜє ᴄσᴍᴘʟєᴛє ᴄσᴍᴍᴧηᴅ ɪηᴅєx ʙєʟσᴡ</b></blockquote>

  <blockquote><b>• ᴧᴄᴄєss єxᴘєʀᴛ ᴛєᴄʜηɪᴄᴧʟ ɢᴜɪᴅᴧηᴄє &amp; ʀєᴧʟ-ᴛɪᴍє sᴜᴘᴘσʀᴛ</b></blockquote>
  <blockquote><b>• єxєᴄᴜᴛє ᴧʟʟ ᴄσᴍᴍᴧηᴅs ᴜsɪηɢ sᴛᴧηᴅᴧʀᴅ ᴘʀєғɪx ➜</b></blockquote>
  ```
  with the help keyboard (`buttons.rs:197-227`, `help_keyboard`) — 4 topic rows
  of 3 + back row:

| Row | Buttons (label → callback, base64) |
|---|---|
| 1 | `ᴧᴅᴍɪη` → `help:admin`, `ᴧᴜᴛʜ` → `help:auth`, `ʙ-ᴄᴧsᴛ` → `help:bcast` |
| 2 | `ᴘʟᴧʏ` → `help:play`, `sᴜᴅσ` → `help:sudo`, `ʀєsᴛʀɪᴄᴛ` → `help:restrict` |
| 3 | `ᴛʜᴜᴍʙηᴧɪʟ` → `help:thumb`, `sᴛᴧʀᴛ` → `help:start`, `ᴧᴜᴛσᴘʟᴧʏ` → `help:autoplay` |
| 4 | `ᴘʟᴧʏʟɪsᴛ` → `help:playlist`, `ᴠᴄ-ʟσɢs` → `help:vclogs`, `ɪηʟɪηє` → `help:inline` |
| 5 | ` ʙᴧᴄᴋ` (`BACK_BTN`) → `start` |

Topic pages (locale keys `help_admin`, `help_auth`, `help_bcast`,
`help_play`, `help_sudo`, `help_restrict`, `help_thumb`, `help_start`,
`help_autoplay`, `help_playlist`, `help_vclogs`, `help_inline`) are long
`<blockquote>` command lists — reproduced verbatim in Appendix A
(`en.yml:1087-1111`). Their subjects:

- `help_admin`: `/pause /resume /skip /askip /stop /mute /unmute /queue /jump
  /move /clear /remove /loop [1-10] /shuffle /seek /seekback /speed /playmode
  /cmddelete`; header note `💮 <b>ᴊᴜsᴛ ᴧᴅᴅ ᴄ ᴧs ᴧ ᴘʀєғɪx ᴛσ ᴛʜє ᴄσᴍᴍᴧηᴅs ᴛσ
  ᴄσηᴛʀσʟ ᴄʜᴧηηєʟ sᴛʀєᴧᴍɪηɢ.</b>`
- `help_auth`: `/auth [ᴜsєʀ] /unauth [ᴜsєʀ] /authlist`
- `help_bcast`: `/broadcast [ᴍєssᴧɢє σʀ ʀєᴘʟʏ]` + modes `-pin -pinloud -user
  -assistant -nobot`; example `/broadcast -user -pin ᴛєsᴛɪηɢ ʙʀσᴧᴅᴄᴧsᴛ`
- `help_play`: `/play /vplay /playforce`; channel play `/cplay /cvplay
  /cplayforce /channelplay [ɪᴅ]`
- `help_sudo`: `/logs /logger /maintenance /eval /sh`; `/activevoice /ac`;
  `/addsudo /rmsudo /sudolist`; footer `❏ ᴛʜєsє ᴄσᴍᴍᴧηᴅs ᴧʀє ʜɪɢʜʟʏ
  sєηsɪᴛɪᴠє, ᴜsє ᴡɪᴛʜ ᴄᴧʀє.`
- `help_restrict`: `/blacklistchat /unblockchat /blacklisted`;
  `/blockuser /unblockuser`
- `help_thumb`: `/nothumb` — enable/disable/close options; per-chat setting
- `help_start`: `/start /help /ping /repo /position /reload /settings /lang
  /bug /json /sudolist /stats`
- `help_autoplay`: `/autoplay /skip`; enable/disable/close; requires ≥1 active song
- `help_playlist`: `/createplaylist /deleteplaylist /addtoplaylist
  /removefromplaylist /playlistinfo /myplaylists`
- `help_vclogs`: `/vclogger [ση|σғғ] /vcstatus`; alerts auto-cleared after 5 sec
- `help_inline`: documents the **playback buttons** (not inline mode):
  `▷` resume, `ɪɪ` pause, `↻` replay, `‣‣ɪ` skip, `▢` stop, `-𝟣𝟧ˢ` seek back,
  `𝟣𝟧ˢ+` seek forward; footer `❏ ᴧʟʟ ʙᴜᴛᴛσηs σηʟʏ ғσʀ ɢʀσᴜᴘ ᴧᴅᴍɪηs.`

Back navigation: `back_keyboard` → single button ` ʙᴧᴄᴋ` → callback
`help:main`.

## 3.3 Now-playing card — locale `stream_now_playing`, `buttons.rs:96-149`

Text (`en.yml:1020-1025`):

```
<blockquote><b>{emoji:💮} ᴘʟᴧʏʙᴧᴄᴋ ᴧᴄᴛɪᴠᴧᴛєᴅ. | єηᴊσʏ ᴛʜє ᴍᴜsɪᴄ |</b></blockquote>
<blockquote expandable>▫ <b>ᴍєʟσᴅʏ :</b> {title}
▫ <b>ʟєηɢᴛʜ :</b> {duration}
▫ <b>ʀєǫᴜєsᴛєʀ :</b> {by}</blockquote>
```

Thumbnail: artwork is fetched to a local path (`platforms/artwork.rs:
artwork_local_path`) and sent with the card unless thumbnails are disabled
for the chat (`nothumb`); see §3.7. Playback markup (`play_markup`, decoded
callbacks prefixed `room:{chat_id}:`):

| Row | Buttons |
|---|---|
| 1 (only when NOT queued) | progress button: `{position} {progress_bar} {duration}` → callback `progress` (no room prefix — global `progress` callback) |
| 2 | `▷` → `room:{id}:resume`, `II` → `room:{id}:pause`, `↻` → `room:{id}:replay`, `‣‣I` → `room:{id}:skip`, `▢` → `room:{id}:stop` |
| 3 | `-𝟣𝟧ˢ` → `room:{id}:seekback_15`, `𝟣𝟧ˢ+` → `room:{id}:seek_15` |
| 4 | `♫ ᴧᴜᴛσᴘʟᴧʏ: {enabled|disabled}` (`autoplay_btn`) → `room:{id}:autoplay_toggle` |
| 5 | `ᴄʟσsє` (`CLOSE_BTN`) → callback `close` |

Progress bar (`utils.rs:64-79`, `get_progress_bar`): 9 stages
`◉———————`, `—◉——————`, `——◉—————`, `———◉————`, `————◉———`,
`—————◉——`, `——————◉—`, `———————◉`, `————————◉` (index by
played/duration; 0 when duration ≤ 0; last when played ≥ duration).

Stop-confirm markup (`stop_confirm_markup`): if paused →
[`▶️ ʀєsᴜᴍє` → `room:{id}:resume`, `⏹️ sᴛσᴘ` → `room:{id}:stop`]; if muted →
[`🔊 ᴜηᴍᴜᴛє` → `room:{id}:unmute`, `⏹️ sᴛσᴘ` → `room:{id}:stop`].
Confirm texts: `stop_confirm_paused` / `stop_confirm_muted` (Appendix A).

Callback toasts / edited messages (locale `cb_*`, `en.yml:209-271`):

- pause: toast `⏸️ ᴛʀᴧᴄᴋ ᴘᴧᴜsєᴅ ᴧᴛ {position}`; edited card:
  `<b>{emoji:⏸️} ᴛʀᴧᴄᴋ ᴘᴧᴜsєᴅ</b>\n\n<b>▫ ᴛʀᴧᴄᴋ:</b> {title}\n<b>▫ ᴘσsɪᴛɪση:</b> {position} / {duration}\n<b>▫ ᴘᴧᴜsєᴅ ʙʏ:</b> {user}`
- resume: toast `▶️ ʀєsᴜᴍєᴅ ᴧᴛ {position}.`; already playing:
  `ℹ️ ᴛʀᴧᴄᴋ ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ.`; failed: `❌ ғᴧɪʟєᴅ ᴛσ ʀєsᴜᴍє ᴘʟᴧʏʙᴧᴄᴋ.`
  Edited card mirrors the pause card with `🎵 ησᴡ ᴘʟᴧʏɪηɢ:` / `ʀєsᴜᴍєᴅ ʙʏ:`.
- replay: `🔁 ʀєᴘʟᴧʏɪηɢ ᴄᴜʀʀєηᴛ ᴛʀᴧᴄᴋ...` / `❌ ғᴧɪʟєᴅ ᴛσ ʀєᴘʟᴧʏ ᴛʀᴧᴄᴋ.` /
  `🔁 ᴛʀᴧᴄᴋ ʀєᴘʟᴧʏєᴅ.` / edited `🔁 ᴛʀᴧᴄᴋ ʀєᴘʟᴧʏєᴅ ʙʏ {user}.`
- seek ±15s: `⏩ sσᴜɢʜᴛ ғσʀᴡᴧʀᴅ {seconds} sєᴄσηᴅs` /
  `⏪ sσᴜɢʜᴛ ʙᴧᴄᴋ {seconds} sєᴄσηᴅs`; near end:
  `⚠️ ᴄᴧηησᴛ sєєᴋ ғσʀᴡᴧʀᴅ {seconds} sєᴄσηᴅs — ᴧʙσᴜᴛ ᴛσ ʀєᴧᴄʜ єηᴅ.`;
  edited: `⏩ sσᴜɢʜᴛ {seconds} sєᴄσηᴅs — ʙʏ {user}` (same for back);
  failure: `❌ failed to seek.\nerror: <code>{error}</code>` (`cb_seek_failed`).
- skip: `⏭️ ᴛʀᴧᴄᴋ sᴋɪᴘᴘєᴅ.` / edited `⏭️ sᴋɪᴘᴘєᴅ ʙʏ {user}`; queue empty:
  `⏹️ ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ — ǫᴜєᴜє єᴍᴘᴛʏ.`; download fail:
  `❌ ғᴧɪʟєᴅ ᴛσ ᴅσᴡηʟσᴧᴅ ηєxᴛ ᴛʀᴧᴄᴋ.`; play fail:
  `❌ ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ ηєxᴛ ᴛʀᴧᴄᴋ.`
- stop: `⏹️ ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ.`
- mute: `🔇 ᴍᴜᴛєᴅ.`; edited card `<b>🔇 ᴘʟᴧʏʙᴧᴄᴋ ᴍᴜᴛєᴅ</b>` + track/muted-by lines.
  unmute: `🔊 ᴜηᴍᴜᴛєᴅ.`
- Generic callback errors: `invalid_request: ⚠️ ɪηᴠᴧʟɪᴅ ʀєǫᴜєsᴛ.`,
  `unknown_action: ⚠️ ᴜηᴋησᴡη ᴧᴄᴛɪση.`, `room_not_active_cb`,
  `room_no_active` (Appendix A).

## 3.4 Queue display — locale `queue_*` (`en.yml:672-686`)

Empty: `⚠️ <b>ησ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ.</b>\n\n<i>ησᴛʜɪηɢ ɪs ǫᴜєᴜєᴅ ʀɪɢʜᴛ ησᴡ.</i>`
(`queue_no_active`); already-empty clear: `⚠️ <b>ᴛʜє ǫᴜєᴜє ɪs ᴧʟʀєᴧᴅʏ єᴍᴘᴛʏ.</b>`
Header: `🎶 <b>ᴄᴜʀʀєηᴛ ǫᴜєᴜє</b>`; sections
`▶️ <b>ησᴡ ᴘʟᴧʏɪηɢ:</b>` / `⏭️ <b>ᴜᴘ ηєxᴛ:</b>`; overflow line
`\n… ᴧηᴅ {remaining} ᴍσʀє`; tail when nothing more:
`📭 <i>ησ ᴍσʀє sσηɢs ɪη ǫᴜєᴜє.</i>`. Queue limit:
`⚠️ ǫᴜєᴜє ʟɪᴍɪᴛ ʀєᴧᴄʜєᴅ ({limit} ᴛʀᴧᴄᴋs ᴍᴧx). ᴜsє /clear ᴛσ ᴄʟєᴧʀ ǫᴜєᴜє.`
(`/queue` rendering code: `modules/queuecmd.rs`.)

## 3.5 Play flow messages — locale `play_*`, `stream_*` (`en.yml:596-636, 1003-1020`)

- Searching: `🔍🎶 sєᴧʀᴄʜɪηɢ... ⚡✨` / with query:
  `🔍🎶 sєᴧʀᴄʜɪηɢ ғσʀ: <b>{query}</b>... ⚡✨`
- No query: `no_song_query` (Appendix A); no result: `no_song_found`.
- Downloading: `📥 ᴅσᴡηʟσᴧᴅɪηɢ sσηɢ "{title}"`
- Download progress (`download_progress`, edited in place):
  ```
  📥 ᴅσᴡηʟσᴧᴅɪηɢ ʏσᴜʀ ᴛʀᴧᴄᴋ...

  <b>ᴘʀσɢʀєss:</b> {percentage}%
  <b>sᴘєєᴅ:</b> {speed}
  <b>єᴛᴧ:</b> {eta}
  <b>єʟᴧᴘsєᴅ:</b> {elapsed}
  ```
  with the `🚦 ᴄᴧηᴄєʟ ᴅσᴡηʟσᴧᴅɪηɢ` button (§3.1). Cancelled:
  `✅ ᴅσᴡηʟσᴧᴅ ᴄᴧηᴄєʟʟєᴅ.` / `ℹ️ ησ ᴅσᴡηʟσᴧᴅ ᴛσ ᴄᴧηᴄєʟ.` /
  `⚠️ ᴅσᴡηʟσᴧᴅ ᴄᴧηᴄєʟєᴅ ʙʏ {user}.`
- Queued single (`play_added_to_queue_single`):
  ```
  <blockquote><b>💮 ɪηᴄσᴍɪηɢ ᴛʀᴧᴄᴋ ᴅєᴛєᴄᴛєᴅ : #{index}</b></blockquote>
  <blockquote expandable><b>🎋 ᴍєʟσᴅʏ :</b> {title}
  <b>✨ ʟєηɢᴛʜ :</b> {duration}
  <b>🥀 ʀєǫᴜєsᴛєʀ :</b> {by}

  💐 sᴛᴧηᴅʙʏ, ʏσᴜʀ sєssɪση ʙєɢɪηs sʜσʀᴛʟʏ</blockquote>
  ```
- Queued multiple: header `➕ <b>ᴧᴅᴅєᴅ {count} ᴛʀᴧᴄᴋs</b> ʙʏ {user}` +
  `⚠️ <i>ǫᴜєᴜє ʟɪᴍɪᴛ ʀєᴧᴄʜєᴅ — sσᴍє ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ.</i>` (if hit) +
  `<i>ᴜsє /queue ᴛσ ᴠɪєᴡ ғᴜʟʟ ʟɪsᴛ.</i>`
- Already in queue: `✅ <b>ᴛʀᴧᴄᴋ ᴧʟʀєᴧᴅʏ ᴧᴅᴅєᴅ!</b>\n\n<i>{title}</i> ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ σʀ ɪη ᴛʜє ǫᴜєᴜє.`
- Already playing (single-track mode): `⚠️ <b>ᴧ sσηɢ ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ.</b>\n\nᴘʟєᴧsє ᴡᴧɪᴛ ғσʀ ɪᴛ ᴛσ ғɪηɪsʜ σʀ ᴜsє <code>/stop</code> ᴛσ sᴛσᴘ ɪᴛ ғɪʀsᴛ.`
- Too long: `⚠️ ʏσᴜ ᴄᴧη'ᴛ ᴘʟᴧʏ sσηɢs ʟσηɢєʀ ᴛʜᴧη {limit_mins} ᴍɪηᴜᴛєs.\n<i>{title}</i> ᴡᴧs sᴋɪᴘᴘєᴅ.`; multi variant header `⚠️ {count} ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ (ᴍᴧx ᴅᴜʀᴧᴛɪση {limit_mins} ᴍɪηs):` + items `— <i>{title}</i>` + `... ᴧηᴅ {remaining} ᴍσʀє.`; all skipped: `❌ ᴧʟʟ ғσᴜηᴅ ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ ᴅᴜє ᴛσ ᴅᴜʀᴧᴛɪση ʟɪᴍɪᴛs.`
- Failures: `❌ ᴅσᴡηʟσᴧᴅ ғᴧɪʟєᴅ. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη.` / `❌ ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ\nєʀʀσʀ: {error}` / `<b> ❌ ᴜηᴧʙʟє ᴛσ ᴘʟᴧʏ </b>\n\n<i>ᴘʟєᴧsє <b>ʀєsᴛᴧʀᴛ ᴛʜє ᴠσɪᴄє</b> ᴄʜᴧᴛ.</i>` / RTMP: `⚠️ <b>ᴄᴧη'ᴛ ᴘʟᴧʏ ɪη ᴧη ʀᴛᴍᴘ / ʟɪᴠє-sᴛʀєᴧᴍ ᴠσɪᴄє ᴄʜᴧᴛ.</b>\n\n<i>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ.</i>` / next-track download: `❌ ғᴧɪʟєᴅ ᴛσ ᴅσᴡηʟσᴧᴅ.\nєʀʀσʀ: <code>{error}</code>` / next-track play: `❌ ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ ᴛʜє sσηɢ.`
- Queue finished: `💮 ᴛʜє ǫᴜєᴜє ʜᴧs ғɪηɪsʜєᴅ.\n\n💐 ᴜsє /play ᴛσ ᴧᴅᴅ ᴍσꝛє sσηɢs..!`
- Next downloading: `📥 ᴅσᴡηʟσᴧᴅɪηɢ ʏσᴜʀ ηєxᴛ ᴛʀᴧᴄᴋ...`
- Autoplay next: `🔁 ᴧᴜᴛσᴘʟᴧʏ: <b>{title}</b>`
- Voice chat ended: `📴 ᴛʜє ᴠσɪᴄє ᴄʜᴧᴛ ᴡᴧs єηᴅєᴅ, sσ ᴘʟᴧʏʙᴧᴄᴋ ʜᴧs sᴛσᴘᴘєᴅ.\n\n💐 sᴛᴧʀᴛ ᴧ ηєᴡ ᴠσɪᴄє ᴄʜᴧᴛ ᴧηᴅ ᴜsє /play ᴧɢᴧɪη.`
- VC started: `<b>♻️ ᴠσɪᴄє ᴄʜᴧᴛ sᴛᴧʀᴛєᴅ!</b>\n\n<i>ᴜsє /play [sσηɢ ηᴧᴍє] ᴛσ ᴘʟᴧʏ ᴍᴜsɪᴄ.</i>`
- VC ended: `<b>⚠️ ᴠɪᴅєσ ᴄʜᴧᴛ єηᴅєᴅ</b>\n\n<i>⏰ ᴅᴜʀᴧᴛɪση: {duration}</i>`

## 3.6 Text-command replies (pause/resume/stop/mute/unmute/skip/seek/speed/loop/shuffle/jump/move/remove/clear/replay)

All keys in Appendix A; the main ones:

- pause: already `pause_already` / success `pause_success` (Appendix A);
  resume: `resume_already_playing` / `resume_failed` / `resume_success`.
- stop: `⏹️ <b>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ ᴧηᴅ ᴄʟєᴧʀєᴅ ʙʏ {user}.</b>` (`stopped`).
- skip: stopped-empty `⏹️ ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ. ǫᴜєᴜє ɪs єᴍᴘᴛʏ.\nsᴋɪᴘᴘєᴅ ʙʏ: {user}`; invalid number `⚠️ ᴘʟєᴧsє ᴘʀσᴠɪᴅє ᴧ ᴠᴧʟɪᴅ ηᴜᴍʙєʀ ᴧғᴛєʀ /skip.\nєxᴧᴍᴘʟє: <code>/skip 3</code>`; count exceeds queue `⚠️ ʏσᴜ ʀєǫᴜєsᴛєᴅ ᴛσ sᴋɪᴘ <code>{requested}</code> ǫᴜєᴜєᴅ ᴛʀᴧᴄᴋs, ʙᴜᴛ σηʟʏ <code>{available}</code> ᴛʀᴧᴄᴋ(s) ᴧʀє ɪη ǫᴜєᴜє.`
- mute: already `mute_already_muted`, failed `mute_failed`, success `mute_success` (Appendix A); unmute: `ℹ️ <b>ᴧʟʀєᴧᴅʏ ᴜηᴍᴜᴛєᴅ</b>\nᴛʜє ᴍᴜsɪᴄ ɪs ησᴛ ᴍᴜᴛєᴅ ɪη ᴛʜɪs ᴄʜᴧᴛ.` / `❌ <b>ᴘʟᴧʏʙᴧᴄᴋ ᴜηᴍᴜᴛє ғᴧɪʟєᴅ</b>\nєʀʀσʀ: <code>{error}</code>` / success card with `🎵 ᴛʀᴧᴄᴋ: {title}`, `👤 ᴜηᴍᴜᴛєᴅ ʙʏ: {user}`, `{speed_line}` where `speed_line = ⚙️ sᴘєєᴅ: <b>{speed}x</b>`.
- seek/jump: usage `⚠️ ᴘʟєᴧsє ᴘʀσᴠɪᴅє sєᴄσηᴅs.\nєxᴧᴍᴘʟє: <code>{cmd} 40</code>`; too close to start/end variants; success `⏩ ᴊᴜᴍᴘєᴅ ᴛσ <u>{position}</u> σғ <u>{duration}</u>.` / seek success `{emoji} ᴊᴜᴍᴘєᴅ {direction} ᴛσ <b>{position}</b> σғ <b>{duration}</b>.`
- speed: usage block (range **0.50x → 4.00x**, examples `{cmd} 1.5`, `{cmd} normal`); current `🎧 <b>ᴄᴜʀʀєηᴛ sᴘєєᴅ:</b> <code>{speed}x</code> ғσʀ 🎵 <u>{title}</u>`; invalid `❌ ɪηᴠᴧʟɪᴅ sᴘєєᴅ ᴠᴧʟᴜє.`; out of range `⚠️ sᴘєєᴅ ᴍᴜsᴛ ʙє ʙєᴛᴡєєη <b>0.50x</b> ᴧηᴅ <b>4.00x</b>.`; set `🚀 ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ sєᴛ ᴛσ <b>{speed}x</b> ʙʏ {user}.`; reset `✅ ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ ʀєsєᴛ ᴛσ <b>1.0x</b> ʙʏ {user}`. (Note: `help_admin` says 0.5x–2.0x — the locale usage text says 0.50x–4.00x; both verbatim.)
- loop: usage `🔁 <b>ʟσσᴘ ᴄσηᴛʀσʟ</b>\n\nᴜsᴧɢє: {cmd} [ᴄσᴜηᴛ]\n• 0 - ᴅɪsᴧʙʟє ʟσσᴘ\n• 1-10 - ʟσσᴘ ᴄσᴜηᴛ` (+ count line); invalid/already-set/disabled/set/current keys in Appendix A.
- shuffle: state `🔀 ᴄᴜʀʀєηᴛʟʏ sʜᴜғғʟє ɪs <b>{state}</b> ғσʀ ᴛʜɪs ᴄʜᴧᴛ.\n\nᴜsє <code>{cmd}</code> ᴛσ ᴛσɢɢʟє ɪᴛ.`; updated `🔀 sʜᴜғғʟє <b>{state}</b> ʙʏ {user}.`
- move/remove/clear: usage + invalid-index + success keys in Appendix A
  (`move_success`, `remove_success`, `clear_success =
  ✅ ᴛʜє ǫᴜєᴜє ʜᴧs ʙєєη ᴄʟєᴧʀєᴅ ʙʏ {user}.`).
- position (`position_now`): `🎵 <b>{title}</b>\n📍 <code>{position} / {duration}</code>\n⚙️ sᴘєєᴅ: <b>{speed}x</b>`.
- replay: `replay_failed` / `replay_success` (Appendix A).

## 3.7 Settings — `/settings` + toggles (`modules/settings_cb.rs:85-131`, locale `settings_*`)

Main text (`settings_main`, `en.yml:835-843`):

```
⚙️ <b>ᴄʜᴧᴛ sєᴛᴛɪηɢs</b>

<b>ɢʀσᴜᴘ:</b> {title}
<b>ɢʀσᴜᴘ ɪᴅ:</b> <code>{id}</code>

ᴄσηғɪɢᴜʀє ᴛʜє ʙσᴛ's ʙєʜᴧᴠɪσʀ ɪη ᴛʜɪs ɢʀσᴜᴘ ᴜsɪηɢ ᴛʜє ʙᴜᴛᴛσηs ʙєʟσᴡ.
```

Keyboard — 8 rows, each `[label button → info:…, state button → set:…]`:

| Row | Label button (→ callback) | State button (→ callback) |
|---|---|---|
| 1 | `🛡️ ᴧᴅᴍɪη ᴍσᴅє` → `info:adminmode` | status word (`ᴧᴅᴍɪηs 🔐` / `ᴧᴜᴛʜ ᴜsєʀs 🎧` / `єᴠєʀʏσηє 🌍`) → `set:adminmode` |
| 2 | `🎵 ᴘʟᴧʏ ᴍσᴅє` → `info:playmode` | `ᴧᴅᴍɪηs 🔐` / `єᴠєʀʏσηє 🌍` → `set:playmode` |
| 3 | `🗑️ ᴄᴍᴅ ᴅєʟєᴛє` → `info:cmddelete` | `єηᴧʙʟєᴅ` / `ᴅɪsᴧʙʟєᴅ` → `set:cmddelete` |
| 4 | `🧹 ᴄʟєᴧη ᴍσᴅє` → `info:cleanmode` | `єηᴧʙʟєᴅ` / `ᴅɪsᴧʙʟєᴅ` → `set:cleanmode` |
| 5 | `⏱️ ᴄʟєᴧη ᴅєʟᴧʏ` → `info:cleanduration` | `{N}m` (cycles 15→30→60→5) → `set:cleanduration` |
| 6 | `🖼️ ᴛʜᴜᴍʙηᴧɪʟs` → `info:nothumb` | `єηᴧʙʟєᴅ` / `ᴅɪsᴧʙʟєᴅ` → `set:nothumb` |
| 7 | `🌐 ʟᴧηɢᴜᴧɢє` → `info:lang` | current language name (`name` key) → `lang:select` |
| 8 | `ᴄʟσsє` → `close` | — |

Info texts (`settings_info_*`, Appendix A): adminmode — `🛡️ ᴡʜᴧᴛ ɪs ᴛʜɪs?\n\nᴄσηᴛʀσʟs ᴡʜσ ᴄᴧη ᴜsє ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅs σғ ᴛʜє ʙσᴛ (sᴋɪᴘ, ᴘᴧᴜsє, єᴛᴄ.).`;
cmddelete — auto-deletes command messages after processing; cleanmode —
auto-removes command/service messages after a delay, playing-status messages
kept until natural expiry; cleanduration — cycles 15m → 30m → 60m → 5m;
nothumb — toggles track artwork/thumbnails in playback messages; playmode —
who can use /play; lang — select the language for the bot's messages in this group.
Update toast: `✅ sєᴛᴛɪηɢs ᴜᴘᴅᴧᴛєᴅ!`

Language picker (`settings_cb.rs:252-263`): language buttons in rows of 2
(callback `lang:{code}`) + back row [` ʙᴧᴄᴋ` → `set:main`].
`/lang` strings: `🌐 sєʟєᴄᴛ ʏσᴜʀ ᴘʀєғєʀʀєᴅ ʟᴧηɢᴜᴧɢє:` /
`ℹ️ ʏσᴜ’ʀє ᴧʟʀєᴧᴅʏ ᴜsɪηɢ ᴛʜɪs ʟᴧηɢᴜᴧɢє.` /
`✅ ʟᴧηɢᴜᴧɢє sᴜᴄᴄєssғᴜʟʟʏ ᴜᴘᴅᴧᴛєᴅ ᴛσ {lang_name}.` /
`❌ ғᴧɪʟєᴅ ᴛσ ᴜᴘᴅᴧᴛє ʟᴧηɢᴜᴧɢє. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη ʟᴧᴛєʀ.`

Per-command settings toggles (same keys, usable as commands):

- `/adminmode [admin|adminauth|everyone]` — help block + statuses above;
  updated `✅ <b>ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅ ᴍσᴅє</b> ᴜᴘᴅᴧᴛєᴅ ᴛσ: {status}`; invalid
  `⚠️ ɪηᴠᴧʟɪᴅ ᴍσᴅє. ᴜsє: <code>admin</code>, <code>adminauth</code>, σʀ <code>everyone</code>.`
- `/playmode [enable|disable]` — `playmode_success_admins =
  🛡️ <b>ᴘʟᴧʏ ᴍσᴅє</b> ɪs ησᴡ ʀєsᴛʀɪᴄᴛєᴅ ᴛσ <b>ᴧᴅᴍɪηs & ᴅᴊs</b> σηʟʏ. 🎧` /
  `playmode_success_everyone = ✅ <b>ᴘʟᴧʏ ᴍσᴅє</b> ɪs ησᴡ <b>σᴘєη ᴛσ єᴠєʀʏσηє</b>. ʟєᴛ ᴛʜє ᴍᴜsɪᴄ ғʟσᴡ! 🎶` /
  restricted denial `🚫 ᴘʟᴧʏ ᴍσᴅє ɪs єηᴧʙʟєᴅ. σηʟʏ ᴧᴅᴍɪηs ᴧηᴅ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ᴄᴧη ᴘʟᴧʏ.`
- `/nothumb`, `/cmddelete`, `/cleanmode` — status/updated keys in Appendix A;
  `cleanmode_status = 🧹 <b>ᴄʟєᴧη ᴍσᴅє</b>\n\nᴧᴜᴛσ-ᴄʟєᴧηᴜᴘ σғ ᴄσᴍᴍᴧηᴅ ᴧηᴅ sєʀᴠɪᴄє ᴍєssᴧɢєs.\nᴄᴜʀʀєηᴛ sᴛᴧᴛє: {action}\nᴅєʟєᴛє ᴧғᴛєʀ: {duration}ᴍ` + hint `ᴜsє /settings ᴛσ ᴄʜᴧηɢє ᴧᴜᴛσ-ᴅєʟєᴛє ᴅᴜʀᴧᴛɪση.`
- `/autoleave [enable|disable]` — status block + `🏃‍♂️ ᴧᴜᴛσʟєᴧᴠє ʜᴧs ʙєєη {action}` / already `⚠️ ᴧᴜᴛσʟєᴧᴠє ɪs ᴧʟʀєᴧᴅʏ {action}`.

Autoplay panel (`autoplay_menu`, `en.yml:847-860`): text
`🔁 <b>ᴧᴜᴛσᴘʟᴧʏ</b>\n\nᴡʜєη єηᴧʙʟєᴅ, σηᴄє ᴛʜє ǫᴜєᴜє єηᴅs ᴛʜє ʙσᴛ ᴡɪʟʟ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ᴋєєᴘ ᴘʟᴧʏɪηɢ ʀєᴄσᴍᴍєηᴅєᴅ sσηɢs sɪᴍɪʟᴧʀ ᴛσ ᴛʜє ʟᴧsᴛ ᴘʟᴧʏєᴅ ᴛʀᴧᴄᴋ.\n\n<b>sᴛᴧᴛᴜs:</b> {state}`;
button `♫ ᴧᴜᴛσᴘʟᴧʏ: {state}` (`autoplay_btn`); toasts
`✅ ᴧᴜᴛσᴘʟᴧʏ єηᴧʙʟєᴅ!` / `❌ ᴧᴜᴛσᴘʟᴧʏ ᴅɪsᴧʙʟєᴅ!`; no active stream:
`⚠️ ʙσᴛ ɪs ησᴛ sᴛʀєᴧᴍɪηɢ ɪη ᴧ ᴠσɪᴄє ᴄʜᴧᴛ. sᴛᴧʀᴛ ᴘʟᴧʏɪηɢ ᴧ sσηɢ ғɪʀsᴛ.`
`/askip`: disabled `⚠️ ᴧᴜᴛσᴘʟᴧʏ ɪs ᴅɪsᴧʙʟєᴅ. ᴜsє /autoplay ᴛσ єηᴧʙʟє ɪᴛ.`; none
`⚠️ ησ ᴧᴜᴛσᴘʟᴧʏ ʀєᴄσᴍᴍєηᴅᴧᴛɪσηs ғσᴜηᴅ ᴛσ sᴋɪᴘ ᴛσ.`; success
`⏭️ ᴧᴜᴛσᴘʟᴧʏ sᴋɪᴘᴘєᴅ ʙʏ {user}\n🔁 ᴘʟᴧʏɪηɢ: <b>{title}</b>`.

## 3.8 Admin / sudo / auth / broadcast / maintenance / stats / logs

- Permission denials: `only_admin`, `only_admin_cb`, `only_admin_or_auth`,
  `only_admin_or_auth_cb`, `only_supergroup`, `only_owner`, `only_sudo`
  (Appendix A).
- `/ping` (`ping_start`/`ping_result`, `en.yml:655-667`):
  `🏓 ᴘɪηɢɪηɢ...` then
  ```
  <b>🏓 ᴘσηɢ</b>: <code>{latency}ms</code>

  <b><u>🤖 {bot} sʏsᴛєᴍ sᴛᴧᴛ's :</u></b>

  <b>ᴜᴘᴛɪᴍє:</b> {uptime}
  <b>ʀᴧᴍ ᴜsᴧɢє:</b> {ram_info}
  <b>ᴄᴘᴜ ᴜsᴧɢє:</b> {cpu_usage}
  <b>ᴅɪsᴋ ᴜsᴧɢє:</b> {disk_usage}
  ```
- `/stats` (`stats_overview`, `en.yml:867-897`): sections **sʏsᴛєᴍ** (OS/arch,
  CPUs/threads), **ɪηᴛєʀηᴧʟ ᴍєᴍσʀʏ (ɢσ)** (alloc/sys/num GC + rate),
  **sєʀᴠєʀ sᴛᴧᴛs** (CPU %, RAM GiB used/total, storage GiB), **ᴍσηɢσᴅʙ
  sᴛᴧᴛs** (storage MB, collections, objects), **sєʀᴠєᴅ** (chats/users),
  **ʀᴜηᴛɪᴍє ᴠєʀsɪσηs** (core Go version, gogram API layer, ntgcalls version)
  — all in `<blockquote>` blocks with `<code>` values; full text in Appendix A.
- Broadcast (`broadcast_*`, `en.yml:95-137`): already-running, parse-failed,
  no-content (with usage + example), fetch failures, no-targets,
  initializing `📡 ɪηɪᴛɪᴧʟɪᴢɪηɢ ʙʀσᴧᴅᴄᴧsᴛ...\n\nᴘʟєᴧsє ᴡᴧɪᴛ...`,
  progress header `📡 ʙʀσᴧᴅᴄᴧsᴛɪηɢ...` + lines `📊 ᴛσᴛᴧʟ ᴄʜᴧᴛs: {done}/{total} ({progress}%)`,
  `👥 ᴛσᴛᴧʟ ᴜsєʀs: {done}/{total} ({progress}%)`,
  `❌ ғᴧɪʟєᴅ ᴄʜᴧᴛs: {count}`, `❌ ғᴧɪʟєᴅ ᴜsєʀs: {count}`,
  `⏱ ᴅєʟᴧʏ: {delay}s`, `⏰ єʟᴧᴘsєᴅ: {elapsed}`, `⏳ єᴛᴧ: {eta}`,
  `✨ sᴜᴄᴄєss ʀᴧᴛє: {rate}% ({sent}/{total})`; cancel:
  `🚫 ʙʀσᴧᴅᴄᴧsᴛ ᴄᴧηᴄєʟʟєᴅ sᴜᴄᴄєssғᴜʟʟʏ.` / owner-only
  `⚠️ σηʟʏ ᴛʜє σᴡηєʀ ᴄᴧη ᴄᴧηᴄєʟ ʙʀσᴧᴅᴄᴧsᴛs.` / none running
  `ℹ️ ησ ʙʀσᴧᴅᴄᴧsᴛ ɪs ᴄᴜʀʀєηᴛʟʏ ʀᴜηηɪηɢ.` / done `🚫 ʙʀσᴧᴅᴄᴧsᴛ ᴄᴧηᴄєʟʟєᴅ.`
- Auth (`auth_*`, `addauth_*`, `del_auth_*`, `authlist_*`): no-user usage,
  extract-fail, limit `ᴛʜɪs ᴄʜᴧᴛ ʜᴧs ʀєᴧᴄʜєᴅ ᴛʜє ᴍᴧxɪᴍᴜᴍ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪᴍɪᴛ ({limit})…`,
  already-authed, cannot-authorize (owner/bot/self), user-is-admin, bot-user
  `🤖 ʏσᴜ ᴄᴧη’ᴛ ᴧᴅᴅ ᴧ ʙσᴛ ᴛσ ᴛʜє ᴧᴜᴛʜ ʟɪsᴛ — ᴧᴜᴛʜᴜsєʀs ᴍᴜsᴛ ʙє ʜᴜᴍᴧη!`,
  success with count `✅ ᴧᴅᴅєᴅ {user}.\nησᴡ {count}/{limit} ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs.`,
  authlist header `<b>ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs:</b>`, entries
  `{index}. {user} (<code>{user_id}</code>)`, total `<b>ᴛσᴛᴧʟ:</b> {count} ᴜsєʀs`,
  empty `ℹ️ ᴛʜєʀє ᴧʀє ησ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ɪη ᴛʜɪs ᴄʜᴧᴛ.`
- Sudo (`sudo_*`, `en.yml:898-932`): owner-self
  `😂 ʜᴧʜᴧ, ʏσᴜ’ʀє ᴛʜє ʙσss ᴧʟʀєᴧᴅʏ! ᴡʜʏ ᴧʀє ʏσᴜ єᴠєη ᴛʀʏɪηɢ ᴛσ ᴧᴅᴅ ʏσᴜʀsєʟғ?`,
  bot-self `🤖 ʜᴧʜᴧ, ɪ’ᴍ ᴛʜє ʙσᴛ! ɪ ᴄᴧη’ᴛ ᴧᴅᴅ ᴍʏsєʟғ ᴛσ ᴛʜє sᴜᴅσ ʟɪsᴛ — єᴠєη ᴛʜє σᴡηєʀ ᴄᴧη’ᴛ ᴄʜєᴧᴛ ᴍє 😎`,
  bot-user, already, add/remove success/fail, owner-remove-block
  `😎 ηɪᴄє ᴛʀʏ! ʏσᴜ ᴄᴧη’ᴛ ʀєᴍσᴠє ᴛʜє σᴡηєʀ — ᴛʜєʏ’ʀє ᴜηᴛσᴜᴄʜᴧʙʟє.`,
  list header `👑 <b>ᴄᴜʀʀєηᴛ sᴜᴅσєʀs:</b>`, owner entry
  `{index}. {user} — <b>σᴡηєʀ</b>`, no-extra `⚠️ ησ ᴧᴅᴅɪᴛɪσηᴧʟ sᴜᴅσєʀs ғσᴜηᴅ.`
  Join announcements: owner `👑 {user} — σᴡηєʀ σғ {bot} — ᴊᴜsᴛ єηᴛєʀєᴅ ᴛʜє ᴄʜᴧᴛ.`;
  sudo `🛡️ {user}, ᴧ sᴜᴅσ ᴜsєʀ σғ {bot}, ᴊᴜsᴛ ᴊσɪηєᴅ ᴛʜє ᴄʜᴧᴛ.`
- Blacklist (`blacklist_*`, `en.yml:139-175`): chat blocked
  `🚫 ᴛʜɪs ᴄʜᴧᴛ ɪs ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ.`; owner-blocked-leave
  `🚫 ᴛʜɪs ᴄʜᴧᴛ σᴡηєʀ ɪs ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ. ʟєᴧᴠɪηɢ ᴄʜᴧᴛ.`;
  user blocked `🚫 ʏσᴜ ᴧʀє ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ.`; usage
  `ᴜsᴧɢє: <code>/blockchat [chat_id|@username]</code>` /
  `ᴜsᴧɢє: <code>/unblockchat [chat_id|@username]</code>`; success
  `✅ ᴜsєʀ ʙʟσᴄᴋєᴅ: <code>{id}</code>` (and unblock/chat variants); list
  title `📛 <b>ʙʟᴧᴄᴋʟɪsᴛєᴅ</b>` + `💬 <b>ᴄʜᴧᴛs:</b>` / `👤 <b>ᴜsєʀs:</b>`.
- Channel play (`cplay_*`, `en.yml:175-199`): id-not-set
  `⚠️ ᴄᴘʟᴧʏ ɪᴅ ησᴛ sєᴛ.\nᴜsє <code>/setcplay &lt;chat_id&gt;</code> ᴛσ sєᴛ ɪᴛ.`;
  usage `<b>ᴜsᴧɢє:</b> <code>/setcplay &lt;channel_id&gt;</code>`; resolve-peer
  fail, invalid target, channel-not-accessible, missing invite permission,
  not-linked, save error, enabled
  `✅ <b>ᴄʜᴧηηєʟ ᴘʟᴧʏ єηᴧʙʟєᴅ.</b> ᴧʟʟ ᴄʜᴧηηєʟ-ᴘʟᴧʏ ᴄσᴍᴍᴧηᴅs (ʟɪᴋє <code>/cplay</code>, <code>/cskip</code>) ᴡɪʟʟ ησᴡ ᴡσʀᴋ ɪη ᴄʜᴧηηєʟ <code>{channel_id}</code>.`;
  owner-required, owner-check-failed, disabled (mirror wording).
- Maintenance (`maint_*`, `en.yml:493-520`): `maint`, `maint_reason`,
  `maint_usage`, already-enabled/disabled, reason-same, reason-removed/updated,
  enabled (+reason variant), disabled, entering. Full text in Appendix A.
- Logger (`logger_*`, `en.yml:422-487`): usage/status/already/update-fail/updated;
  logger channel messages: `logger_bot_added` / `logger_bot_removed`
  (group name/id/username, added-or-removed-by name/id/username, time — in a
  `<blockquote>`), `logger_bot_started` (`▶️ <b>{mention} sᴛᴧʀᴛєᴅ ᴛʜє ʙσᴛ</b>`
  + username + id), `logger_playback_template`
  (`<b>🎧 ᴛʀᴧᴄᴋ:</b> {track}\n<b>🔗 sσᴜʀᴄє:</b> {source}\n<b>📌 ɢʀσᴜᴘ:</b> {group} ({group_id})\n<b>👤 ʀєǫᴜєsᴛєᴅ ʙʏ:</b> {requested_by} (<code>{requested_by_id}</code>)`),
  `logger_playback_started = ᴘʟᴧʏʙᴧᴄᴋ sᴛᴧʀᴛєᴅ`,
  `logger_playback_queued = ᴘʟᴧʏʙᴧᴄᴋ ǫᴜєᴜєᴅ`.
- `/reload` (`reload_*`, `en.yml:689-717`): start
  `⚙️ ʀєʟσᴧᴅɪηɢ ᴧᴅᴍɪη ᴄᴧᴄʜє, ᴠσɪᴄє ᴄʜᴧᴛ sᴛᴧᴛᴜs, ᴧηᴅ ᴧssɪsᴛᴧηᴛ sᴛᴧᴛᴜs...`;
  done `<b>⚙️ ʀєʟσᴧᴅ ᴄσᴍᴘʟєᴛє:</b>\n\n{summary}` with per-line
  `• ᴧᴅᴍɪη ᴄᴧᴄʜє: ✅ ᴅσηє` / `❌ ({error})`,
  `• ᴠσɪᴄє ᴄʜᴧᴛ: ⚪ ɪηᴧᴄᴛɪᴠє` / `❌ ᴧᴅᴍɪη ᴘєʀᴍɪssɪση ʀєǫᴜɪʀєᴅ` / fail / active,
  assistant lines (banned/admin-required/fail/present/not-present), room reset.
- `/restart` (`restart_*`, `en.yml:750-772`): `restart`, exepath/symlink fails,
  service, initiated, fail, confirm-running, buttons `restart_btn_bot` /
  `restart_btn_replay` (Appendix A for exact labels), confirm texts.
- `/bug` (`bug_*`, `en.yml:381-398`): usage, thanks, private-chat-only,
  reply-only, report format (with `<blockquote>{report}</blockquote>`).
- Assistant errors (`err_*`, `en.yml:313-380`): assistant banned
  (`🚫 ᴧssɪsᴛᴧηᴛ ʀєsᴛʀɪᴄᴛєᴅ` + unban hint `ᴜsє /reload ᴛσ ʀєғʀєsʜ.`),
  admin-permission-required, assistant-get-failed, join-request-sent,
  invite-link-fetch, invite-failed, connection-timeout, no-active-voicechat,
  fetch-failed, unknown, fetch-tracks, download-internal. Full text in
  Appendix A.
- Assistant restricted warning (`assistant_restricted_warning`):
  `⚠️ <b>ᴧssɪsᴛᴧηᴛ ʀєsᴛʀɪᴄᴛєᴅ</b>\n\nᴛʜє ᴧssɪsᴛᴧηᴛ {assistant} (ɪᴅ: <code>{id}</code>) ʜᴧs ʙєєη <b>ʙᴧηηєᴅ</b> σʀ ʀєsᴛʀɪᴄᴛєᴅ ɪη ᴛʜɪs ᴄʜᴧᴛ.\nɪ ᴄᴧηησᴛ ᴘʟᴧʏ ᴍᴜsɪᴄ, ᴍᴧηᴧɢє ᴛʜє ǫᴜєᴜє, σʀ ᴄʟєᴧη ᴜᴘ ᴛʀᴧᴄᴋs ᴡʜɪʟє ᴛʜɪs ᴄσηᴛɪηᴜєs.\n\n<i>ᴜηʙᴧη ᴛʜє ᴧssɪsᴛᴧηᴛ ᴛσ ʀєsᴛσʀє ᴧʟʟ ᴍᴜsɪᴄ ғєᴧᴛᴜʀєs 🎵</i>`
- Bot added/removed: `bot_added_normal =
  🎵 ᴛʜᴧηᴋs ғσʀ ᴧᴅᴅɪηɢ ᴍє! ᴜsє /play [sσηɢ] ᴛσ sᴛᴧʀᴛ ᴘʟᴧʏɪηɢ ᴍᴜsɪᴄ.`;
  demotion goodbye (`bot_demotion_goodbye`): `<b>⚠️ ᴘєʀᴍɪssɪση ʟσsᴛ</b>\n\nɪ’ᴠє ʙєєη <b>ᴅєᴍσᴛєᴅ ғʀσᴍ ᴧᴅᴍɪη</b> ᴧηᴅ ᴄᴧη’ᴛ ᴘєʀғσʀᴍ ᴍʏ ᴛᴧsᴋs ᴧηʏᴍσʀє.\nᴡɪᴛʜσᴜᴛ ᴘʀσᴘєʀ ʀɪɢʜᴛs, ɪ’ᴍ ᴜηᴧʙʟє ᴛσ ғᴜηᴄᴛɪση ʜєʀє, sσ ɪ’ʟʟ ʙє <i>ʟєᴧᴠɪηɢ ᴛʜє ᴄʜᴧᴛ ησᴡ</i>.\n\n<b>ɢσσᴅʙʏє 👋</b>`
- Supergroup needed (`supergroup_needed`): explains the chat is not a
  supergroup yet, links the conversion guide
  `https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02` and the
  support group (`{support_group}`).
- Flood: `flood_seconds` / `flood_minutes` (Appendix A).
- Generic: `generic_error = ❌ sσᴍєᴛʜɪηɢ ᴡєηᴛ ᴡʀσηɢ: <code>{error}</code>`.

## 3.9 Playlists — locale `playlist_*` (`en.yml:1059-1086`)

- Create: usage `<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ηᴧᴍє]`; limit
  `⚠️ ʏσᴜ ʜᴧᴠє ʀєᴧᴄʜєᴅ ᴛʜє ᴍᴧxɪᴍᴜᴍ ʟɪᴍɪᴛ σғ 10 ᴘʟᴧʏʟɪsᴛs.`; fetch fail;
  create fail `❌ ғᴧɪʟєᴅ ᴛσ ᴄʀєᴧᴛє ᴘʟᴧʏʟɪsᴛ: {error}`; success
  `✅ ᴘʟᴧʏʟɪsᴛ <b>{name}</b> ʜᴧs ʙєєη ᴄʀєᴧᴛєᴅ sᴜᴄᴄєssғᴜʟʟʏ.\nɪᴅ: <code>{id}</code>`
- Delete: usage `{cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ]`; not found
  `❌ ᴛʜє sᴘєᴄɪғɪєᴅ ᴘʟᴧʏʟɪsᴛ ᴄσᴜʟᴅ ησᴛ ʙє ғσᴜηᴅ. ᴘʟєᴧsє ᴄʜєᴄᴋ ᴛʜє ᴘʟᴧʏʟɪsᴛ ɪᴅ.`;
  not owner `⚠️ ʏσᴜ ᴄᴧη σηʟʏ ᴍᴧηᴧɢє ᴘʟᴧʏʟɪsᴛs ᴛʜᴧᴛ ʏσᴜ ᴄʀєᴧᴛєᴅ.`;
  success `🗑️ ᴘʟᴧʏʟɪsᴛ <b>{name}</b> ʜᴧs ʙєєη ᴅєʟєᴛєᴅ sᴜᴄᴄєssғᴜʟʟʏ.`
- Add: usage `{cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ] [sσηɢ ηᴧᴍє σʀ ᴜʀʟ]`; invalid query
  `❌ ησ ᴘʟᴧʏᴧʙʟє ᴛʀᴧᴄᴋ ᴡᴧs ғσᴜηᴅ ғσʀ ᴛʜᴧᴛ ǫᴜєʀʏ.`; success
  `✅ ᴛʀᴧᴄᴋ <b>{song}</b> ʜᴧs ʙєєη ᴧᴅᴅєᴅ ᴛσ ᴘʟᴧʏʟɪsᴛ <b>{playlist}</b>.`
- Remove: usage `{cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ] [sσηɢ ηᴜᴍʙєʀ σʀ ᴜʀʟ]`; invalid number
  `⚠️ ɪηᴠᴧʟɪᴅ sσηɢ ηᴜᴍʙєʀ.`; not found `❌ sσηɢ ησᴛ ғσᴜηᴅ ɪη ᴘʟᴧʏʟɪsᴛ.`;
  success `🗑️ sσηɢ ʀєᴍσᴠєᴅ ғʀσᴍ ᴘʟᴧʏʟɪsᴛ <b>{name}</b>.`
- Info: usage `{cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ]`; empty `<i>ᴛʜɪs ᴘʟᴧʏʟɪsᴛ ʜᴧs ησ sσηɢs ʏєᴛ.</i>`;
  song line `{index}. {name} ({url})`; info card
  `<b>ᴘʟᴧʏʟɪsᴛ ɪηғσ</b>\n\n<b>ηᴧᴍє:</b> {name}\n<b>σᴡηєʀ:</b> {owner}\n<b>sσηɢs:</b> {count}\n\n{songs}`
- List: none `📭 ʏσᴜ ᴅσ ησᴛ ʜᴧᴠє ᴧηʏ ᴘʟᴧʏʟɪsᴛs ʏєᴛ. ᴜsє /createplaylist ᴛσ ᴍᴧᴋє σηє.`;
  line `- {name} (<code>{id}</code>)`; list `<b>ᴍʏ ᴘʟᴧʏʟɪsᴛs</b>\n\n{playlists}`

Per-command help registry — `bot/src/modules/help_texts.rs` (909 lines,
`format_help_text`, 69 `=>` arms). Keys (verbatim, incl. the four without a
leading slash): `/ac /active /activevc /activevoice /addauth /addtoplaylist
/adminmode /askip /authlist /autoplay /bash /bug /channelplay /cleanmode
/cmddelete /commanddelete /cplay /createplaylist /cvplay /delauth
/deleteplaylist /end /ev /eval /fcplay /fplay /fvplay /help /json /jump /logs
/loop /maint /maintenance /mute /myplaylists /nothumb /pause /ping /play
/playforce /playlistinfo /playmode /position /privacy /reload
/removefromplaylist /replay /repo /restart /resume /seek /seekback /setcplay
/sh /shell /shuffle /skip /speed /stats /stop /unmute /vclogger /vplay
autoleave bcast broadcast gcast`. Each entry is a multi-line HTML help card
with `<i>` intro, `<u>Usage:</u>`, `<b>📊 Information Shown:</b>`,
`<b>🔒 Restrictions:</b>`, `<b>💡 Use Case:</b>` sections (see the `/ac` sample
in §3.9-note of the source file). Full file is the port source of record —
not re-typed here line-for-line; see Appendix A note.

## 3.10 Absent in ParvatiMusic

No lyrics view, no inline-mode (query) results, no reply keyboard, no search-
results list UI (search is one-shot `/play <query>`), no voting/share UI.

---

# 4. telegram-music-bot-rust — `refs/telegram-music-bot-rust/main.rs` (527 lines, single file)

Rust / teloxide. A **download bot, not a voice-chat bot**: no VC, no queue,
no now-playing card, no settings/admin panel, no lyrics, no playlists, no
inline mode, no reply keyboard (it *removes* keyboards), no stats/ping.
All strings are **Russian**. One command only: `/start`.

## 4.1 /start — `main.rs:147-160` (`Command::Start`)

BotCommands description (shown in the command list):
`Поддерживаемые команды:` / `Start` — `Запустить бота` ("Start the bot").
Reply (plain text) + `KeyboardRemove`:

```
Салам! 👋 Я твой музыкальный бот.

Напиши название песни или скинь ссылку на трек/видео!
```
("Hi! 👋 I'm your music bot.\n\nWrite a song name or drop a link to a track/video!")

## 4.2 Free-text search flow — `handle_text_message`, `process_query`

- Greeting/stop words are silently ignored: `привет, салам, здорово, хай,
  hello, как дела, спасибо, пока` (case-insensitive exact match).
- Any other text → first sends `🔎 Ищу трек...` ("Searching for track..."),
  then edits it. No results → edit to `Ничего не найдено.` ("Nothing found.").
- Results (max 5, yt-dlp `ytsearch10`, tracks >20 min / 1200 s skipped):
  message edited to `Выбери трек:` ("Choose a track:") with **one button per
  row**, label `{N}. {title≤35 chars + "…" if longer}{ [M:SS] if duration
  known}`, `callback_data = dl_{index}_{origin_message_id}`.

## 4.3 Link flow — `handle_text_message:190-208`

Text containing `youtube.com`, `youtu.be` or `tiktok.com` → sends
`🔗 Ссылка принята! Что именно скачать?` ("Link accepted! What exactly to
download?") then edits its reply markup to one row, two buttons:

| Label | `callback_data` |
|---|---|
| `🎵 Аудио (MP3)` | `get_audio_{message_id}` |
| `🎬 Видео (MP4)` | `get_video_{message_id}` |

## 4.4 Download / remix (audio-effect) flow — `handle_callback`

- Link stale (cache miss): edit to `❌ Ссылка устарела.` ("Link is outdated.")
- Downloading: edit to `📥 Скачиваю...` ("Downloading...")
- Download error: edit to `❌ Ошибка скачивания.` ("Download error.")
- On success the file is sent as audio/video, the temp file deleted, and the
  prompt message deleted (no confirmation text).
- Track chosen (`dl_*`): message edited to (HTML parse mode)
  `📌 Трек: <b>{title}</b>\n\nВыбери вариант обработки:` ("Choose a
  processing option:") with a 3-row keyboard:

| Row | Buttons (label → `callback_data`) |
|---|---|
| 1 | `🎵 Оригинал` → `remix_orig_{idx}_{msg_id}`, `🌌 Slow + Reverb` → `remix_slow_{idx}_{msg_id}` |
| 2 | `⚡ Nightcore` → `remix_nightcore_{idx}_{msg_id}`, `🔊 Bass Boost` → `remix_bass_{idx}_{msg_id}` |
| 3 | `🌀 8D Audio` → `remix_8d_{idx}_{msg_id}` |

  Effects (ffmpeg): slow `atempo=0.85,aecho=0.8:0.88:60:0.4`; nightcore
  `asetrate=44100*1.2,atempo=1/1.2`; bass `bass=g=12:f=110:w=0.6,volume=0.9`;
  8d `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`; output 320k MP3,
  file name `{title}.mp3`.
- Remix downloading: edit to `📥 Скачиваю и применяю эффект...`
  ("Downloading and applying effect..."); error:
  `❌ Ошибка скачивания трека.` ("Error downloading track.")
- Results stale on remix tap: edit to `❌ Результаты устарели.`
  ("Results are outdated.")

## 4.5 Anti-spam / ban — `check_auto_ban`, `is_banned`

- >5 requests in 60 s → user auto-banned (id + username persisted to
  `ban.txt`) and sent `🚫 Вы заблокированы за спам.` ("You are blocked for
  spam."). Banned users are otherwise silently ignored. Hard-coded
  `ADMIN_ID = 5893041927` is exempt.
- Download limits: video `b[filesize<50M][ext=mp4]/best[ext=mp4]/best`;
  audio extracted to MP3 quality 0.

## 4.6 Absent

No /help, no queue/now-playing, no settings, no lyrics, no inline mode, no
stats, no playlists, no broadcast, no auth/sudo UI (admin is a hard-coded id),
no channel-play, no voting. Console-only log lines (e.g.
`🔄 Запуск пуллинга сообщений...`) are not user-facing.

---

# 5. feel-me — `refs/feel-me/src/` (`strings.rs`, `lib.rs`, `main.rs`)

Rust / teloxide. An **anonymous music-sharing bot** (send a song to another
user via their unique link), not a VC player: no playback, queue, now-playing,
lyrics, settings, stats, playlists, inline mode, or voice. All user strings
live in `src/strings.rs` (66 lines) and are used verbatim (no parse mode set
anywhere — plain text). **Every** message that carries buttons uses the same
2-button row: [`Help` → `help`, `My Link` → `get_link`].

## 5.1 /start and /help — `lib.rs:219-284` (`handle_text`), `strings.rs:3-9`

`/start` with no argument and `/help` both reply with `HELPTEXT` (verbatim,
including the leading spaces and the typo "Desctiption"):

```
Send Music: You can send your music by clicking on user's uniqu link

Get your own link: You can get your link by click on `Get Link Button`

Available commands:

	/start: Start the bot and send music or get your link

	/help: See this Desctiption again :)
```

`/start <nickname>` (deep link `https://t.me/FeelMeBot?start=` + nickname):

- Target is self → `SEND_MSG_TO_SELF`:
  `Dude?! You have schizophrenia\n Why you send music to yourself`
- Target blocked the sender → `{BLOCKED_TEXT1} {name}\n{BLOCKED_TEXT2}` =
  `You can't send music to {name}\nYou are blocked`
- Target unknown nickname → `INVALID_LINK`: `Oops!\nInvalid link`
- OK → `SEND_MUSIC_TEXT + " " + {name}`:
  `Please Send the music that you want to share with {name}` **with a
  single-button row** [`Cancel` → `cancel`]. (Redis stores sender→target
  mapping until the audio arrives or Cancel is pressed.)
- DB/Redis failure → `WRONG`: `Something went wrong on server side\nPlease try again`
- Unknown command (`/anything-else`) → `INVALID_CMD`: `Invalid Command`
- Non-command text with no pending target → `INVALID_MSG`: `Invalid Message`
  (plain reply, no buttons). Text with a malformed pending-feedback state →
  `INVALID_MSG_FORMAT`: `Invalid message format`.

## 5.2 Sending music — `main.rs:77-135`

- Sender sends an audio file while a target is set: the audio is forwarded
  to the target **by file_id** (`send_audio`) with a 1-row keyboard:
  [`Feedback` → `feedback_{sender_chat_id}_{update_id}_{audio_file_unique_id}`,
  `Report User` → `report_{sender_chat_id}_{update_id}`].
  Sender then gets `HAS_BEEN_SENT`: `Music Has Been send` (sic).
- Target is blocked (blacklist check): sender gets
  `You can't send music to {name}\nYou are blocked` (same as §5.1).
- Target user row missing in DB: target name renders as `this user`.
- If the sender sends **text** while a feedback is pending (Redis value
  `{to_id}_{msg_id}_{file_unique_id}`), the text is forwarded as a reply to
  the original audio message in the target chat, and the sender gets
  `FEEDBACK_IS_SENT`: `Feedback is sent`.

## 5.3 Callback queries — `main.rs:163-337` (`q_callback`)

Callback data handled: `cancel`, `help`, `get_link`, `feedback_*`, `report_*`,
`unreport_*` (constants `LOG=0, FEEDBACK=1, REPORT=2, UNREPORT=3`).

- `cancel` → the cancelling user is sent `CANCEL_SENDING`:
  `Cancel sendig music` (sic), with the standard [Help | My Link] row.
- `help` → `HELPTEXT` (§5.1) with [Help | My Link] row.
- `get_link` → (via `handle_text`/callback) the user's own link
  `https://t.me/FeelMeBot?start={nickname}` — the exact reply text is built
  in `lib.rs` (link string constant `BOT_URL`); the button row is again
  [Help | My Link].
- `feedback_*` when the sender already sent feedback → `FEEDBACK_SENT`:
  `Feedback is sent before`; when the sender is blocked from feedback →
  `FEEDBACK_BLOCKED`: `You can't send Feedback\nYou are blocked`; prompt →
  `FEEDBACK_SEND`: `Send Feedback Text`.
- `report_*` when already reported → `REPORTED_BEFORE`:
  `User already is reported`; on success → `USER_REPORTED_BLOCKED`:
  `User is reported and blocked`.
- Any other/unknown callback data → `HELPTEXT` with [Help | My Link] row.
- DB errors anywhere → `WRONG` (`Something went wrong on server side\nPlease
  try again`); user-fetch errors → `ERROR_ON_GETTING_USER`:
  `Erro on getting Users` (sic); user-create `USER_CREATING`:
  `Error on creating new user`; history `HISTORY_CREATING`:
  `Error while storing history`; blacklist set/search `SET_BL_ERR`:
  `Error on set to blacklist` / `SEARCH_BL_ERR`:
  `Error on searching for blacklist`; DB connect `DB_CONN_ERROR`:
  `Error on connecting to`. (These constants exist in `strings.rs`; most are
  returned as plain messages only in error paths — see `main.rs` `WRONG`
  usage at lines 89, 105, 183, 192, 204, 218, 225, 251, 260, 272, 287, 297,
  304, 316, 323, 336.)

## 5.4 Absent

No playback/queue/now-playing, no search, no lyrics, no settings/admin panel,
no stats/ping, no playlists, no broadcast, no inline mode, no reply keyboard
(only the inline [Help | My Link] and [Cancel] rows above), no channel-play,
no voting.

---

# 6. telegram-music-bot-daslui — `refs/telegram-music-bot-daslui/src/` (`main.rs`, `spotify.rs`)

Rust / teloxide + rspotify. A **Spotify queue-request bot with group voting**:
users DM the bot a Spotify link; the bot posts it to a configured voting chat
where members vote accept/decline. No VC playback, no queue UI of its own, no
lyrics, no settings, no stats, no playlists, no inline mode, no reply keyboard.

## 6.1 Commands — `main.rs:344-368` (`Command`, `BotCommands`)

Auto-generated help (`Command::descriptions()`, prefix `These commands are
supported:`) — `/help` sends exactly that generated text:

| Command | Description |
|---|---|
| `/help` | `display this text.` |
| `/spotify_login` | `link spotify (admin only)` |
| `/id` | `get chat/thread id` |

`/help` works in the voting chat (admin handler) and everywhere else
(`command_handler`). `/spotify_login` only in the voting chat; `/id` anywhere.

## 6.2 /start — none

There is **no /start handler**; a DM that is not a Spotify link gets
`user_help`: `Send a Spotify URL.` (plain text).

## 6.3 Track request flow — `main.rs:160-226` (`request_track`)

DM with a Spotify link (`https://open.spotify.com/…` or `https://spotify.link/…`):

- Track card text (`spotify.rs:172-191`, `format_track_text`, HTML parse mode,
  link preview **enabled, shown above text**, with the album cover as preview
  URL — `link_preview_for_url`, `main.rs:334-342`):
  ```
  🎵 <b>{track name}</b> • ⏱️ {M:SS — note: seconds are NOT zero-padded, format is "{}:{}" with num_minutes and num_seconds % 60}
  👥 <b>{artists, comma-separated}</b>
  💿 <b>{album name}</b>
  <a href="{spotify url}">Listen</a> • <a href="{cover1}">Cover</a> <a href="{cover2}">Cover</a> …
  ```
- Requester confirmation (DM, HTML + same preview):
  `✅ Successfully requested track.\n\n{track card}`
- Voting-chat post (same card, HTML + preview, optional forum thread):
  `User {FirstName (@username) or FirstName or "Unknown"} requested:\n{track card}`
  with a 1-row keyboard:
  [`✅ Add to queue` → `accept:{spotify_track_id}`,
  `❌ Decline` → `decline`]
  (`accept:` payload is the track id; `SpotifyTrackId::from_urn` also accepts
  `spotify:track:{id}`.)
- Fetch failure → requester DM: `❌ Failed to find track.` (and the error is
  returned as a handler error).

## 6.4 Voting callbacks — `main.rs:228-300` (`handle_callback`)

Only callback queries **from the voting chat** are handled. The voting message
is **edited in place** (HTML), keyboard cleared
(`InlineKeyboardMarkup::default()`):

- Accept → Spotify `add_item_to_queue` succeeds:
  `✅ {voter name} has added to queue:\n{original message text or track URL}`
  (sic — "has added").
  Queue failure: `Failed to queue track: {error}`. Bad id: `Invalid track ID`
  (twice — from `from_urn` failure and from `TrackId::from_id` failure).
- Decline → link preview disabled on the edit; text:
  `❌ {voter name} hat abgelehnt: {original message text}` (German, verbatim)
  or `❌ {voter name} hat abgelehnt.` if the message had no text.
- Unknown callback data → silently ignored (`return Ok(())`).

## 6.5 Spotify login (admin, voting chat only) — `main.rs:128-159`

- `/spotify_login` → DM-style message in the voting chat (as a reply if in a
  thread):
  `Spotify Login\nOpen this URL in the browser and allow Spotify access: {auth_url}\nThen paste and send the redirected URL here.`
  Dialogue state → `SpotifyLogin`.
- Auth URL generation failure → `Spotify link error: {error}`.
- Pasted redirect URL/code → token saved: `Token saved`; invalid →
  `Invalid Code/URL`. Dialogue state returns to `Start`.
- OAuth scopes: `user-read-private user-read-email user-read-playback-state
  user-modify-playback-state`; redirect URI
  `https://127.0.0.1:8888/callback` (`spotify.rs:1-59`).

## 6.6 /id — `main.rs:314-322`

`This chat has ID {chat_id}, thread {thread_id}` (with thread) or
`This chat has ID {chat_id}` (without). Sent as a reply (thread-aware).

## 6.7 Absent

No /start, no now-playing/queue/controls, no search UI, no lyrics, no
settings/admin panel beyond the login, no stats/ping, no playlists, no
broadcast, no inline mode, no reply keyboard, no channel-play. Config is
env-only: `TELEGRAM_VOTING_CHAT_ID`, optional `TELEGRAM_VOTING_THREAD_ID`.

---

# 7. rustify — `refs/rustify/` (`locales/*.yml`, `src/telegram/`)

Rust / teloxide. A **Spotify companion bot** (polls the user's Spotify,
sends profanity/AI-slop alerts, likes/dislikes, lyrics + AI analysis,
Magic Playlist™, Recommendasion™, Skippage™). **No voice-chat playback at
all** — there is no now-playing card, queue, or playback controls. 19 locale
files (1135 lines total, `en` + `ru` each, `_version: 2`, `%{var}`
placeholders, HTML parse mode, `<tg-emoji emoji-id="…">` premium-emoji
wrappers). English strings are quoted below; Russian originals are in the
locale files (Appendix D lists the files; en+ru are both reproduced there).
Commands are localised BotCommands (`src/telegram/commands.rs`).

## 7.1 Commands (user) — `commands.rs:10-63`, descriptions in `locales/commands.yml`

| Command | Description (en) |
|---|---|
| `/help` | `Show this help` |
| `/start [ref]` | `Start` |
| `/language` | `Change language` |
| `/keyboard` | `Show keyboard` |
| `/dislike` | `Dislike current track` |
| `/like` | `Like current track` |
| `/recommendasion` | `Call Recommendasion™` |
| `/details` | `Show details about the currently playing track` |
| `/stats` | `Show statistics about tracks` |
| `/login` | `Log in to Spotify` |
| `/toggle_track_skip` | `Toggle the setting for skipping disliked tracks` |
| `/toggle_profanity_check` | `Toggle the profanity check setting` |
| `/magic` | `Create or refresh Magic playlist` |
| `/add_word_to_whitelist <word>` | `Add word to whitelist` |
| `/remove_word_from_whitelist <word>` | `Remove word from whitelist` |
| `/list_words_in_whitelist` | `List words in whitelist` |
| `/skippage <days>` | `Allows you to skip tracks you've already listened to. Pass the number of days to remember` |
| `/ai_slop_detection` | (description key `command.ai-slop-detection` — value not present in `commands.yml`; see note in §7.9) |

Admin commands (`commands_admin.rs`, NOT localised, plain English
descriptions): `/admin` `Show this help`; `/global_stats` `Show global
statistics`; `/broadcast <locale>` `Broadcast a message to all users`;
`/get_word_definition <locale> <word>`; `/reset_word_definition <locale>
<word>`; `/list_word_definitions <locale>`; `/users <user_id>` `List users`;
`/build_info` `Build Info`.

Unknown command (`handlers/commands.rs:30,78`):
```
<tg-emoji emoji-id="5348337458754894202">❓</tg-emoji> Command <code>%{command}</code> not found.

<tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> There are commands available to you:
```
followed by the localised command list (header `command.available-header`:
`📋 Commands available to you:` with the same 📋 premium emoji).
Unhandled non-command text (`handlers/message.rs:41`):
```
<tg-emoji emoji-id="6043973168291384891">😔</tg-emoji> Your request was not handled

<tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> There are commands available to you:
```
+ command list.

## 7.2 /start + reply keyboard — `actions/start.rs`, `keyboards.rs`, `locales/keyboards.yml`

- **Not logged in to Spotify:** sends `language.command` — `Choose your
  language` — with a reply keyboard, 1 row, 2 buttons:
  [`Русский 🇷🇺` (`language.change` ru), `English 🇺🇸` (`language.change` en)]
  (`.resize_keyboard()`). After choosing, `language.changed`:
  `Your language is now English 🇺🇸` / `Теперь Ваш язык Русский 🇷🇺`; if
  authed, the start keyboard is attached; if not, the keyboard is removed
  (`KeyboardRemove`) and the login invite (§7.3) follows.
- **Logged in:** sends `actions.here-is-your-keyboard`:
  `Here is your keyboard ⌨️` (or, if the account was inactive,
  `status.reactivated` — §7.8) with the **start reply keyboard**
  (`StartKeyboard::markup`, `.resize_keyboard()`), 2 rows:

| Row | Buttons |
|---|---|
| 1 | `👎 Dislike playing track` |
| 2 | `📈 Stats`, `🎤 Details of track` |

  Button presses are matched by exact localised text
  (`StartKeyboard::from_str`); they trigger the same actions as
  `/dislike`, `/stats`, `/details`.

## 7.3 Login — `actions/login.rs`, `locales/login.yml`

Invite text (`login.invite`):
```
<b>Welcome to Rustify!</b>

🚀 To get started, connect your <i>Spotify</i> account:

• Click the button below 👇
• Authenticate through Spotify 🔐
• Enjoy! 🎵

<b>Features:</b>
• Block unwanted songs ("Dislike")
• Notifications for profanity in lyrics (English only)
• Detect AI-generated music (notify/ignore/auto-skip)
• Get song lyrics and AI-powered analysis
• Create <i>Magic Playlist™</i>✨ from your favorite tracks
• <i>Skippage™</i> function will diverse your listening experience
```
with a 1-row inline keyboard: [`🎵 Login with Spotify` → URL (Spotify
authorize URL)]. Success (`login.success`):
```
🎉 Great! You registered successfully!

Now you can:
• Press '%{dislike_button}' to block a song
• Press '%{details_button}' to get the lyrics of the currently playing song and additional information
• There you can also analyze lyrics using AI
• Use the /%{magic_command} command to generate a <i>Magic Playlist™</i>✨
• Use the /%{skippage_command} command to activate the <i>Skippage™</i> function
```

## 7.4 Track alerts (the "now playing" equivalent) — `locales/profanity_check.yml`, `ai_slop.yml`, `currently_playing_none_reason.yml`

There is **no now-playing card**. When a new track starts, the user only
hears from the bot if something is wrong with it:

- Profanity alert (`profanity-check.message`):
  ```
  🚨 Current song (%{track_name}) <b>probably</b> has bad words:

  %{bad_lines}

  <a href="%{lyrics_link}">%{lyrics_link_text}</a>

  Press '%{ignore_button_label}' to never see this notification for <b>this song</b> again
  ```
  with inline buttons per current track status (`inline_buttons.rs:
  from_track_status`): status None → rows [`Block track 👎`],
  [`Hide alerts for this track 🙈`]; Disliked → only [`Hide alerts…`];
  Ignored → only [`Block track 👎`].
- AI-slop alert (`ai-slop.alert`):
  ```
  🎵 %{track_name}
  Album: %{album_name}

  💩 This track is most likely <b>%{prediction}</b>. You can block track or ignore similar notifications for this track

  To permanently block this artist from recommendations:
  🔗 Open the artist's page in Spotify
  ➕ Tap the three dots near the <i>"Follow"</i> button
  ❌ Select <i>"Don't play this artist"</i>

  <i>Information provided by "%{ai_check_provider}"</i>

  <b>Support real organic hand-made music instead of AI-slop!</b>

  <blockquote>Want to disable these alerts or auto-skip AI tracks? Try /%{config_command}</blockquote>
  ```
  (`%{prediction}` = `fully AI-generated` / `partially generated or
  processed by AI`; the 🔗/➕/❌/💩 emojis are wrapped in `<tg-emoji
  emoji-id="…">` in source.) Same Dislike/Ignore buttons as above.
- No-track reasons (`currently-playing-none-reason.*`): `⏸️ Current track is
  on pause` / `🔇 Nothing is currently playing` / `🎙️ It's a podcast` /
  `📁 It's a local file`.

Inline-button labels (`locales/inline_buttons.yml`):
`Block track 👎`, `Hide alerts for this track 🙈`, `Analyze text 🔍`,
`Go to artist's page 🔗`; alert toasts (callback answers):
`You need to login first`, `Button is broken. Try another one`.
**`callback_data` is a JSON-serialized enum** (`inline_buttons.rs`):
`{"Dislike":"<track_id>"}`, `{"Ignore":"<track_id>"}`,
`{"Analyze":"<track_id>"}`, `{"SongLinks":"<track_id>"}`,
`"Magic"`, `{"AISlopDetection":["Notify"|"Ignore"|"Skip",true|false]}`,
`{"SkippageEnable":true|false}`, `"Recommendasion"`.

## 7.5 /details + lyrics — `actions/details.rs` (458 lines), `locales/details.yml`

- Collecting (message, later edited): `🎵 %{track_name}\nAlbum:
  %{album_name}\n\n⏳ Collecting information about the track 🔍` (⏳/🔍/🎵
  are premium-emoji-wrapped in source).
- Audio features block (`details.features`):
  ```
  🎶 <code>%{key} %{modality}</code> ⌛ %{tempo} BPM
  🎻 Acoustic %{acousticness}%
  🕺 Danceability %{danceability}%
  ⚡️ Energy %{energy}%
  🤐 Instrumentalness %{instrumentalness}%
  🏟 Live performance %{liveness}%
  🎤 Speech-like %{speechiness}%
  ☺️ Positivity %{valence}%
  ```
  (`%{modality}` = `Minor` / `Major`.)
- Full card (`details.header`):
  ```
  🎵 %{track_name}
  Album: %{album_name}

  %{features}👎 Disliked by %{disliked_by} people
  🙈 Ignored by %{ignored_by} people
  ```
  + `🎭 Genres: %{genres}` if any.
- With lyrics (`details.with-lyrics`): header + `🤬 Profanity
  <code>%{profanity}</code>` + `🌐 Language: %{language}` + genres line +
  blank line + `%{lyrics}` + blank line +
  `<a href="%{lyrics_link}">%{lyrics_link_text}</a>`.
- No lyrics (`details.no-lyrics`): header + genres line + blank line +
  `<code>No lyrics found</code>`.
- Details inline buttons (from `details.rs`): Analyze (`Analyze text 🔍`),
  SongLinks (`Links to other platforms 🔗`), artist page
  (`Go to artist's page 🔗`).

## 7.6 /analyze, /like, /dislike, /stats — `locales/analyze.yml`, `actions.yml`

- Analyze waiting: `🎵 %{track_name}\nAlbum: %{album_name}\n\n⏳ Wait for
  analysis to finish 🔍`; failed: same header +
  `❌ Analysis failed. This happens from time to time. Try again later 🤷`;
  disabled: `❌ Analysis is disabled`; lyrics not found:
  `🚫 Lyrics not found`. Result (`analysis.result`): header +
  `<blockquote>ℹ️ You can transcribe and analyze any audio in the
  <a href="https://t.me/insightify_tgbot?start=ref-228571569_utm-invlink">Insightify</a> bot</blockquote>` +
  `%{analysis_result}` + profane-words block. Profane-words overflow:
  `<blockquote expandable>🚷 <b>Profane words analysis section doesn't fit in the results</b></blockquote>`;
  profane-words block:
  `<blockquote expandable>⚠️ <b>Definitions of words that are considered profane.</b> This analysis is not precise and I would recommend manually checking the meanings of the words using Google\n\n%{profane_words}</blockquote>`.
  (The two AI *prompts* `analysis.prompt` / `analysis.word-analyzer-prompt`
  are not user-visible; they are in `locales/analyze.yml` for reference.)
- Like/dislike confirmations (`actions.dislike` / `actions.ignore`):
  `👎 Disliked %{track_link}\n\nIf you change your mind, press
  '%{ignore_button_label}'\n\n💡 Do not forget you can send a link to a song
  to find current status` /
  `🙈 Bad words of %{track_link} will be forever ignored\n\nIf you change
  your mind, press '%{dislike_button_label}'\n\n💡 Do not forget you can send
  a link to a song to find current status`.
  (Sending the bot a Spotify track link shows the track's current
  like/dislike/ignore status — handler in `handlers/url.rs`.)
- Stats (`actions.stats`):
  ```
  📉 <b>Some nice stats for you</b> 📈

  👎 You disliked <code>%{dislikes}</code> songs
  ⏭ Skipped <code>%{skips}</code> times
  💔 Removed <code>%{removed_collection}</code> from liked songs
  🗑 Removed <code>%{removed_playlists}</code> from playlists
  🔬 Checked lyrics <code>%{lyrics_checked}</code> times
  🔍 Analyzed lyrics <code>%{lyrics_analyzed}</code> times
  🙈 You ignored <code>%{ignored}</code> track lyrics
  🤬 <code>%{lyrics_profane}</code> lyrics were considered profane
  💩 <code>%{ai_slop_total}</code> tracks were detected as AI Slop

  <blockquote expandable><b>Languages stats:</b>
  %{languages}</blockquote>
  ```
- Cleanup (disliked-track removal): start
  `Started cleanup. Please wait, it can take a bit of time 🕐`; finish
  `Deleted %{removed_playlists} tracks in %{count_playlists} playlists and
  %{removed_collection} in favorite songs 🗑`.

## 7.7 Magic Playlist™, Recommendasion™, Skippage™, song links — `locales/magic.yml`, `recommendasion.yml`, `skippage.yml`, `song_links.yml`

- Magic (`magic.*`): header
  `<i>Magic Playlist™</i> ✨ is made of your shuffled favorite songs that will
  be removed from this playlist as you listen to them. This allows you to
  listen to everything you love one by one without any repetition. You'll love
  it! 😊`; description = header + `\n\n👇 Click the button below to create or
  update <i>Magic Playlist™</i> ✨`; button `✨ Generate Magic Playlist™ ✨`
  (single inline button, callback `"Magic"`); generating = header +
  `\n\n⏳ Generating <i>Magic Playlist™</i> ✨`; generated = header +
  `\n\n✨ Created <a href="%{url}">Magic Playlist™</a> ✨`; failed = header +
  `\n\n❌ Something went wrong. Please try again later 🤷`.
- Recommendasion (`recommendasion.*`): welcome
  `🤖 <i>Recommendasion™</i> is an experimental feature that uses your
  preferences to suggest something new with AI.\n\nClick <i>"🤖 Generate
  recommendations"</i> and AI will analyze your likes and add something fresh
  to your Spotify queue. AI can be silly sometimes, so don't judge too harshly
  🙂`; button `🤖 Generate recommendations` (callback `"Recommendasion"`);
  disabled `🚫 <i>Recommendasion™</i> is temporarily disabled. Work is
  underway to restore it`; device-not-found
  `❌ Device not found. Start listening to something and try again`;
  progress edits: `⌛️ Generating recommendations...\n\n❤️ Collecting your
  favorite songs` → `…\n\n🤖 Asking AI for track recommendations` →
  `…\n\n🎵 Adding tracks to Spotify queue`; result
  `✅ New tracks added to Spotify queue. Skip to the next track to start
  listening.\n\n🎯 Recommendations:\n\n%{recommendation_links}\n\n📊 Slop
  rate: %{slop_rate}% tracks not found on Spotify\n\nLegend: 👎 dislike,
  🔍 not found\n\n<blockquote expandable>%{slop_links}</blockquote>`.
- Skippage (`skippage.*`): main
  `⏭️ <i>Skippage™</i> allows you to skip tracks you've listened to for a set
  amount of days. It makes your music experience more diverse. <b>Try
  it!</b>\n\nPass number of days to remember played tracks and enable this
  function like that:\n<code>/%{command} 7</code>\n\nCurrent amount of days:
  <b>%{setting}</b>` (`%{setting}` = `unset` / `%{days} day(s)`); updated
  `⏭️ All tracks you've listened to within <b>%{days} days</b> will be
  skipped`; disabled `⏭️ <i>Skippage™</i> is disabled`; validation
  `⚠️ Number should be positive and 365 or less`; inline buttons
  `✅ Enable Skippage™` / `❌ Disable Skippage™` (callback
  `{"SkippageEnable":true|false}`).
- Song links (`song_links.*`): button `Links to other platforms 🔗`
  (callback `{"SongLinks":"<track_id>"}`); fetching
  `🎵 %{track_name}\nAlbum: %{album_name}\n\n🔍 Finding links to other
  platforms...`; result `🎵 %{track_name}\nAlbum: %{album_name}\n\n🎧
  Available on:\n%{links}\n\n⚡ Links provided by
  <a href="https://odesli.co/">SongLink</a>`.

## 7.8 Settings toggles, AI-slop config, whitelist, status, errors, rate limits

- `/toggle_profanity_check`: on
  `🚫 Tracks will be checked for profanity content`; off
  `✅ Tracks won't be checked for profanity content`.
- `/toggle_track_skip`: on `⏭️ Disliked tracks will be skipped`; off
  `▶️ Disliked tracks won't be skipped`.
- `/ai_slop_detection`: description
  `💩 AI Music Detection\n\nBot automatically checks tracks for AI
  generation\n\nConfigure bot's reaction to such tracks:` with 3 inline
  buttons, selected one prefixed `✅ `:
  `✅ Notify` / `Don't notify` / `Skip automatically` (labels
  `%{mark}Notify` etc.; callback
  `{"AISlopDetection":["Notify"|"Ignore"|"Skip",true|false]}`).
- Whitelist (`user_word_whitelist.*`): add-provide
  `📝 Provide word <code>/%{command} your-word</code>`; limit-length
  `❌ Word must not be longer than %{limit} characters`; limit-amount
  `❌ You can add no more than %{limit} words`; added
  `✅ Word <code>'%{word}'</code> added to whitelist\n\n📋 To list all words
  in whitelist /%{command}`; exists
  `⚠️ Word <code>'%{word}'</code> is already in whitelist\n\n📋 To list all
  words in whitelist /%{command}`; remove-provide
  `🗑️ Provide word <code>/%{command} your-word</code>`; removed
  `🗑️ Word <code>'%{word}'</code> removed from whitelist`; doesn't-exist
  `❌ Word <code>'%{word}'</code> is not in whitelist\n\n📋 To list all words
  in whitelist /%{command}`; empty
  `📭 Your whitelist is empty\n\n➕ Add new word with
  <code>/%{command} your-word</code>`; list
  `📝 Words you added to whitelist:\n\n%{words}\n\n🗑️ You can remove a word
  with <code>/%{command} your-word</code> command`.
- Status (`status.*`): inactive
  `💤 Your account has been marked as inactive due to inactivity. Use
  /%{start} to reactivate and resume all bot functions.`; reactivated
  `<b>🎉 Welcome back!</b>\n\nYour account is now active again. All bot
  functions have been resumed.`
- Errors (`errors.yml`): cannot-skip
  `❌ Current song (%{track_name}) was disliked, but I cannot skip it...`;
  unavailable-in-country
  `🚫 Your Spotify account says 'Spotify is unavailable in this country'.
  Please fix this issue and relogin by calling /%{command} command to
  continue using Rustify`; spotify-auth-failed
  `❌ Failed to Authenticate user to Spotify. Please relogin by calling
  /%{command} command.\n\n<pre>%{error}</pre>`; spotify-invalid-token
  `Relogin to Spotify by calling /%{command} command`; general
  `<b>Sorry, an error has occurred :(</b>\n\n<a
  href="https://github.com/vtvz/rustify/issues/new">Report an issue on
  GitHub</a>`.
- Rate limits (`rate_limit.yml`, all `⏱️ Wait %{duration} before … again`):
  analysis `…analyzing again`; magic `…generating Magic Playlist™ ✨ again`;
  details `…checking track details again`; recommendasion `…using 🤖
  Recommendasion again`; dislike `…disliking another track`; like `…liking
  another track`.

## 7.9 Admin UI — `actions/broadcast.rs`, `admin_users/`, `global_stats.rs`, `word_definition.rs`

- Broadcast (admin, reply-to-message required): no locale → `Reply to
  message`; bad locale → `Pass right locale`. Sends the replied message to
  all users of that locale (via `backon` retry). No progress/completion text
  in code (only logs).
- Admin notifications (`services/notification.rs:38-68`): new user
  `🆕 <b>New user joined</b>\n\n<code>{id}</code> {link} {name} {surname}
  {username}\nRef Code: {ref_code}` (ref code rendered `<i>None</i>` when
  absent); Spotify connected
  `✅ <b>User connected Spotify</b>\n\nName: {name}\nID: <code>{id}</code>
  <a href="tg://user?id={id}">link</a>`.
- `/users`, `/global_stats`, word-definition admin commands: no dedicated
  locale strings found (outputs are built inline in
  `actions/admin_users/list.rs` (286 lines), `admin_users/details.rs`
  (211 lines), `global_stats.rs` (140 lines), `word_definition.rs`
  (277 lines) — port workers should read those files directly; they render
  user lists / global counters / word definitions as plain HTML text).
- Note: the user-command description key `command.ai-slop-detection` is
  referenced in `commands.rs` but **has no entry in `locales/commands.yml`**
  (missing translation — the bot would render the key or empty text).
- No playback queue/now-playing, no voice, no inline-mode results, no
  download progress, no playlists UI beyond Magic Playlist™.

---

# Coverage matrix (which ref has UI for which category)

| Category | TGVCBot | remix | ParvatiMusic | tmb-rust | feel-me | daslui | rustify |
|---|---|---|---|---|---|---|---|
| /start | ✅ | ❌ | ✅ | ✅ | ✅ | ❌ (DM hint only) | ✅ |
| /help (full) | ✅ | ❌ | ✅ (topics) | ❌ | ✅ | ✅ (auto) | ✅ (auto) |
| Now-playing card | ✅ photo banner | ❌ (/now only) | ✅ + buttons | ❌ | ❌ | ❌ (vote card) | ❌ (alerts only) |
| Queue display | ✅ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ |
| Lyrics view | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ (in /details) |
| Search results | ✅ buttons | ✅ reply-number | ❌ | ✅ buttons | ❌ | ❌ | ❌ |
| Download progress | ❌ | ❌ | ✅ %/speed/ETA | ✅ status edits | ❌ | ❌ | ❌ |
| Settings menu | ❌ | ❌ | ✅ 7 toggles | ❌ | ❌ | ❌ | ✅ toggles |
| Admin panel | ❌ | ❌ (admin-only cmds) | ✅ sudo/auth/bcast/maint/logs | ❌ (hard-coded admin id) | ❌ | ✅ login only | ✅ broadcast/users/stats |
| Error messages | ✅ | ✅ | ✅ (439 keys) | ✅ | ✅ | ✅ | ✅ |
| Inline-mode results | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Reply keyboard | ❌ | ❌ | ❌ | ❌ (removes) | ❌ | ❌ | ✅ start + language |
| Rich patterns | HTML links/bold | HTML | HTML + blockquote | HTML (1 msg) | none | HTML links | HTML + blockquote + tg-emoji |
| Playlists | ❌ | ❌ | ✅ 6 cmds | ❌ | ❌ | ❌ | ✅ Magic only |
| Broadcast | ❌ | ❌ | ✅ progress | ❌ | ❌ | ❌ | ✅ (no progress text) |
| Maintenance | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ |
| Auth/sudo | ❌ | ❌ | ✅ | ❌ | ❌ (blocklist) | ❌ | ❌ |
| Channel-play | ❌ | ❌ | ✅ /cplay | ❌ | ❌ | ❌ | ❌ |
| Voting/share | ❌ | ❌ | ❌ | ❌ | ✅ share-by-link | ✅ accept/decline | ❌ |
| Ping/alive | ❌ | ❌ | ✅ /ping + /stats | ❌ | ❌ | ❌ | ❌ |
| Audio effects | ❌ | ❌ | ❌ | ✅ slow/nightcore/bass/8D | ❌ | ❌ | ❌ |

---

# Appendix A — ParvatiMusic `bot/locales/en.yml` (VERBATIM, complete)

Source: `refs/ParvatiMusic/bot/locales/en.yml` (1112 lines, 439 keys). This is
the authoritative text for every ParvatiMusic string; §3 quotes the important
ones, this appendix is the complete record. `{emoji:X}` = placeholder replaced
with emoji X at render; `{name}` = template variable. Note the file uses
Unicode small-caps throughout — do not "fix" the casing when porting.

```yaml
name: "🇺🇸 єηɢʟɪsʜ"

# buttons
CONFIRM_RESUME_BTN: "▶️ ʀєsᴜᴍє"
CONFIRM_UNMUTE_BTN: "🔊 ᴜηᴍᴜᴛє"
CONFIRM_STOP_BTN: "⏹️ sᴛσᴘ"
ADD_ME_BTN: "⟡₊ ᴧᴅᴅ ϻє ᴛσ ʏσυꝛ ᴄʜᴧᴛ ₊⟡"
BROADCAST_CANCEL_BTN: "🚫 ᴄᴧηᴄєʟ ʙʀσᴧᴅᴄᴧsᴛ"
CLOSE_BTN: "ᴄʟσsє"
DOWNLOAD_CANCEL_BTN: "🚦 ᴄᴧηᴄєʟ ᴅσᴡηʟσᴧᴅɪηɢ"
GC_HELP_BTN: " ᴄσᴍᴍᴧηᴅs"
START_HELP_BTN: " ʜєʟᴘ & ᴄσᴍᴍᴧηᴅs"
UPDATES_BTN: " ᴜᴘᴅᴧᴛєs"
SUPPORT_BTN: "sᴜᴘᴘσʀᴛ"
SOURCE_BTN: " sσᴜʀᴄє ᴄσᴅє"
REPO_DEV_BTN: "ᴅєᴠєʟσᴘєʀ"
REPO_BTN: "ʀєᴘσ"

HELP_ADMIN_BTN: "ᴧᴅᴍɪη"
HELP_AUTH_BTN: "ᴧᴜᴛʜ"
HELP_BCAST_BTN: "ʙ-ᴄᴧsᴛ"
HELP_PLAY_BTN: "ᴘʟᴧʏ"
HELP_SUDO_BTN: "sᴜᴅσ"
HELP_RESTRICT_BTN: "ʀєsᴛʀɪᴄᴛ"
HELP_THUMB_BTN: "ᴛʜᴜᴍʙηᴧɪʟ"
HELP_START_BTN: "sᴛᴧʀᴛ"
HELP_AUTOPLAY_BTN: "ᴧᴜᴛσᴘʟᴧʏ"
HELP_PLAYLIST_BTN: "ᴘʟᴧʏʟɪsᴛ"
HELP_VCLOGS_BTN: "ᴠᴄ-ʟσɢs"
HELP_INLINE_BTN: "ɪηʟɪηє"

BACK_BTN: " ʙᴧᴄᴋ"

# basically this string used in /command [bool]
invalid_bool: "{emoji:⚠️} <b>ɪηᴠᴧʟɪᴅ ᴠᴧʟᴜє.</b>\nᴜsє 'єηᴧʙʟє' σʀ 'ᴅɪsᴧʙʟє'."

adminmode_help: |
  {emoji:🛡️} <b>ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅ ᴍσᴅє</b>

  <b>sᴛᴧᴛᴜs:</b> {status}
  <b>ᴜsᴧɢє:</b> <code>/adminmode [admin|adminauth|everyone]</code>
adminmode_status_admin: "ᴧᴅᴍɪηs 🔐"

adminmode_status_adminauth: "ᴧᴜᴛʜ ᴜsєʀs 🎧"

adminmode_status_everyone: "єᴠєʀʏσηє 🌍"

adminmode_updated: "{emoji:✅} <b>ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅ ᴍσᴅє</b> ᴜᴘᴅᴧᴛєᴅ ᴛσ: {status}"

adminmode_invalid: "{emoji:⚠️} ɪηᴠᴧʟɪᴅ ᴍσᴅє. ᴜsє: <code>admin</code>, <code>adminauth</code>, σʀ <code>everyone</code>."

# 🚫 Assistant Restricted / Banned
assistant_restricted_warning: |
  {emoji:⚠️} <b>ᴧssɪsᴛᴧηᴛ ʀєsᴛʀɪᴄᴛєᴅ</b>

  ᴛʜє ᴧssɪsᴛᴧηᴛ {assistant} (ɪᴅ: <code>{id}</code>) ʜᴧs ʙєєη <b>ʙᴧηηєᴅ</b> σʀ ʀєsᴛʀɪᴄᴛєᴅ ɪη ᴛʜɪs ᴄʜᴧᴛ.
  ɪ ᴄᴧηησᴛ ᴘʟᴧʏ ᴍᴜsɪᴄ, ᴍᴧηᴧɢє ᴛʜє ǫᴜєᴜє, σʀ ᴄʟєᴧη ᴜᴘ ᴛʀᴧᴄᴋs ᴡʜɪʟє ᴛʜɪs ᴄσηᴛɪηᴜєs.

  <i>ᴜηʙᴧη ᴛʜє ᴧssɪsᴛᴧηᴛ ᴛσ ʀєsᴛσʀє ᴧʟʟ ᴍᴜsɪᴄ ғєᴧᴛᴜʀєs {emoji:🎵}</i>

# 🧩 Common auth messages
auth_no_user: "{emoji:⚠️} ᴘʟєᴧsє ᴘʀσᴠɪᴅє ᴧ ᴜsєʀ — ᴜsє:\n{cmd} [ᴜsєʀ_ɪᴅ] σʀ ʀєᴘʟʏ ᴛσ ᴧ ᴜsєʀ's ᴍєssᴧɢє."

user_extract_fail: "{emoji:⚠️} ᴜηᴧʙʟє ᴛσ ɢєᴛ ᴛʜᴧᴛ ᴜsєʀ: <i>{error}</i>"

auth_limit_reached: "{emoji:⚠️} ᴛʜɪs ᴄʜᴧᴛ ʜᴧs ʀєᴧᴄʜєᴅ ᴛʜє ᴍᴧxɪᴍᴜᴍ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪᴍɪᴛ ({limit}). ᴘʟєᴧsє ʀєᴍσᴠє sσᴍєσηє ʙєғσʀє ᴧᴅᴅɪηɢ ᴧ ηєᴡ σηє."

already_authed: "{emoji:⚠️} ᴛʜᴧᴛ ᴜsєʀ ɪs ᴧʟʀєᴧᴅʏ ᴧᴜᴛʜσʀɪᴢєᴅ — ησ ηєєᴅ ᴛσ ᴧᴅᴅ ᴛʜєᴍ ᴧɢᴧɪη."

# 🤖 Add auth
cannot_authorize_user: "{emoji:⚠️} ᴛʜɪs ᴜsєʀ ᴄᴧηησᴛ ʙє ᴧᴜᴛʜσʀɪᴢєᴅ.\nᴛʜє ʙσᴛ σᴡηєʀ, ᴛʜє ʙσᴛ ɪᴛsєʟғ, σʀ ʏσᴜʀsєʟғ ᴅσ ησᴛ ʀєǫᴜɪʀє ᴧᴜᴛʜσʀɪᴢᴧᴛɪση."

addauth_user_is_admin: "{emoji:⚠️} ᴛʜɪs ᴜsєʀ ɪs ᴧʟʀєᴧᴅʏ ᴧ ᴄʜᴧᴛ ᴧᴅᴍɪηɪsᴛʀᴧᴛσʀ. ησ ᴧᴜᴛʜσʀɪᴢᴧᴛɪση ɪs ʀєǫᴜɪʀєᴅ."

addauth_bot_user: "{emoji:🤖} ʏσᴜ ᴄᴧη’ᴛ ᴧᴅᴅ ᴧ ʙσᴛ ᴛσ ᴛʜє ᴧᴜᴛʜ ʟɪsᴛ — ᴧᴜᴛʜᴜsєʀs ᴍᴜsᴛ ʙє ʜᴜᴍᴧη!"

addauth_add_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴧᴅᴅ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀ: {error}"

addauth_success_with_count: "{emoji:✅} ᴧᴅᴅєᴅ {user}.\nησᴡ {count}/{limit} ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs."

addauth_success: "{emoji:✅} sᴜᴄᴄєssғᴜʟʟʏ ᴧᴅᴅєᴅ {user} ᴛσ ᴛʜє ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪsᴛ."

# 🤖 Bot Added to Chat
bot_added_normal: "{emoji:🎵} ᴛʜᴧηᴋs ғσʀ ᴧᴅᴅɪηɢ ᴍє! ᴜsє /play [sσηɢ] ᴛσ sᴛᴧʀᴛ ᴘʟᴧʏɪηɢ ᴍᴜsɪᴄ."

# ⚠️ Bot Demotion
bot_demotion_goodbye: |
  <b>{emoji:⚠️} ᴘєʀᴍɪssɪση ʟσsᴛ</b>

  ɪ’ᴠє ʙєєη <b>ᴅєᴍσᴛєᴅ ғʀσᴍ ᴧᴅᴍɪη</b> ᴧηᴅ ᴄᴧη’ᴛ ᴘєʀғσʀᴍ ᴍʏ ᴛᴧsᴋs ᴧηʏᴍσʀє.
  ᴡɪᴛʜσᴜᴛ ᴘʀσᴘєʀ ʀɪɢʜᴛs, ɪ’ᴍ ᴜηᴧʙʟє ᴛσ ғᴜηᴄᴛɪση ʜєʀє, sσ ɪ’ʟʟ ʙє <i>ʟєᴧᴠɪηɢ ᴛʜє ᴄʜᴧᴛ ησᴡ</i>.

  <b>ɢσσᴅʙʏє {emoji:👋}</b>

broadcast_already_running: "{emoji:⚠️} ᴧ ʙʀσᴧᴅᴄᴧsᴛ ɪs ᴧʟʀєᴧᴅʏ ʀᴜηηɪηɢ.\n\nᴘʟєᴧsє ᴡᴧɪᴛ ғσʀ ɪᴛ ᴛσ ᴄσᴍᴘʟєᴛє σʀ ᴄᴧηᴄєʟ ɪᴛ ғɪʀsᴛ ᴜsɪηɢ:\n• <code>/broadcast -cancel</code>\n• σʀ ᴄʟɪᴄᴋ ᴛʜє ᴄᴧηᴄєʟ ʙᴜᴛᴛση ση ᴛʜє ᴘʀσɢʀєss ᴍєssᴧɢє"

broadcast_parse_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴘᴧʀsє ʙʀσᴧᴅᴄᴧsᴛ ᴄσᴍᴍᴧηᴅ:\n<code>{error}</code>"

broadcast_no_content: "{emoji:⚠️} ησ ᴄσηᴛєηᴛ ᴘʀσᴠɪᴅєᴅ ғσʀ ʙʀσᴧᴅᴄᴧsᴛ.\n\n<b>ᴜsᴧɢє:</b> <code>{cmd} [ᴛєxᴛ σʀ ʀєᴘʟʏ ᴛσ ᴍєssᴧɢє]</code>\n\n<b>єxᴧᴍᴘʟє:</b>\n<code>{cmd} ʜєʟʟσ єᴠєʀʏσηє!</code>"

broadcast_fetch_chats_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ sєʀᴠєᴅ ᴄʜᴧᴛs:\n<code>{error}</code>"

broadcast_fetch_users_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ sєʀᴠєᴅ ᴜsєʀs:\n<code>{error}</code>"

broadcast_no_targets: "{emoji:⚠️} ησ ᴛᴧʀɢєᴛs ғσᴜηᴅ ғσʀ ʙʀσᴧᴅᴄᴧsᴛ.\n\nᴍᴧᴋє sᴜʀє ʏσᴜ ʜᴧᴠєη'ᴛ єxᴄʟᴜᴅєᴅ ᴧʟʟ ᴛᴧʀɢєᴛs ᴡɪᴛʜ ғʟᴧɢs."

broadcast_initializing: "{emoji:📡} ɪηɪᴛɪᴧʟɪᴢɪηɢ ʙʀσᴧᴅᴄᴧsᴛ...\n\nᴘʟєᴧsє ᴡᴧɪᴛ..."

broadcast_progress_header: "{emoji:📡} ʙʀσᴧᴅᴄᴧsᴛɪηɢ..."

broadcast_total_chats: "{emoji:📊} ᴛσᴛᴧʟ ᴄʜᴧᴛs: {done}/{total} ({progress}%)"

broadcast_total_users: "{emoji:👥} ᴛσᴛᴧʟ ᴜsєʀs: {done}/{total} ({progress}%)"

broadcast_failed_chats: "{emoji:❌} ғᴧɪʟєᴅ ᴄʜᴧᴛs: {count}"

broadcast_failed_users: "{emoji:❌} ғᴧɪʟєᴅ ᴜsєʀs: {count}"

broadcast_delay: "{emoji:⏱} ᴅєʟᴧʏ: {delay}s"

broadcast_elapsed: "{emoji:⏰} єʟᴧᴘsєᴅ: {elapsed}"

broadcast_eta: "{emoji:⏳} єᴛᴧ: {eta}"

broadcast_success_rate: "{emoji:✨} sᴜᴄᴄєss ʀᴧᴛє: {rate}% ({sent}/{total})"

broadcast_not_running: "ℹ️ ησ ʙʀσᴧᴅᴄᴧsᴛ ɪs ᴄᴜʀʀєηᴛʟʏ ʀᴜηηɪηɢ."

broadcast_cancel_success: "{emoji:🚫} ʙʀσᴧᴅᴄᴧsᴛ ᴄᴧηᴄєʟʟєᴅ sᴜᴄᴄєssғᴜʟʟʏ."

broadcast_cancel_owner_only: "⚠️ σηʟʏ ᴛʜє σᴡηєʀ ᴄᴧη ᴄᴧηᴄєʟ ʙʀσᴧᴅᴄᴧsᴛs."

broadcast_cancel_none_running: "ℹ️ ησ ʙʀσᴧᴅᴄᴧsᴛ ɪs ᴄᴜʀʀєηᴛʟʏ ʀᴜηηɪηɢ."

broadcast_cancel_done: "🚫 ʙʀσᴧᴅᴄᴧsᴛ ᴄᴧηᴄєʟʟєᴅ."

# Blocklist

blacklist_chat_blocked: "{emoji:🚫} ᴛʜɪs ᴄʜᴧᴛ ɪs ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ."

blacklist_owner_blocked_leave: "{emoji:🚫} ᴛʜɪs ᴄʜᴧᴛ σᴡηєʀ ɪs ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ. ʟєᴧᴠɪηɢ ᴄʜᴧᴛ."

blacklist_user_blocked: "{emoji:🚫} ʏσᴜ ᴧʀє ʙʟσᴄᴋєᴅ ғʀσᴍ ᴜsɪηɢ ᴛʜɪs ʙσᴛ."

blacklist_usage_blockchat: "ᴜsᴧɢє: <code>/blockchat [chat_id|@username]</code>"

blacklist_usage_unblockchat: "ᴜsᴧɢє: <code>/unblockchat [chat_id|@username]</code>"

blacklist_invalid_chat_identifier: "ɪηᴠᴧʟɪᴅ ᴄʜᴧᴛ ɪᴅєηᴛɪғɪєʀ: {error}"

blacklist_block_user_success: "{emoji:✅} ᴜsєʀ ʙʟσᴄᴋєᴅ: <code>{id}</code>"

blacklist_unblock_user_success: "{emoji:✅} ᴜsєʀ ᴜηʙʟσᴄᴋєᴅ: <code>{id}</code>"

blacklist_block_chat_success: "{emoji:✅} ᴄʜᴧᴛ ʙʟσᴄᴋєᴅ: <code>{id}</code>"

blacklist_unblock_chat_success: "{emoji:✅} ᴄʜᴧᴛ ᴜηʙʟσᴄᴋєᴅ: <code>{id}</code>"

blacklist_unblock_user_fail: "{emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ᴜηʙʟσᴄᴋ ᴜsєʀ: {error}"

blacklist_unblock_chat_fail: "{emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ᴜηʙʟσᴄᴋ ᴄʜᴧᴛ: {error}"

blacklist_fetch_chats_fail: "{emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ʙʟσᴄᴋєᴅ ᴄʜᴧᴛs: {error}"

blacklist_fetch_users_fail: "{emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ʙʟσᴄᴋєᴅ ᴜsєʀs: {error}"

blacklist_list_title: "{emoji:📛} <b>ʙʟᴧᴄᴋʟɪsᴛєᴅ</b>"

blacklist_list_chats: "{emoji:💬} <b>ᴄʜᴧᴛs:</b>"

blacklist_list_users: "{emoji:👤} <b>ᴜsєʀs:</b>"

# 🎧 CPlay (Channel Play)

cplay_id_not_set: "{emoji:⚠️} ᴄᴘʟᴧʏ ɪᴅ ησᴛ sєᴛ.\nᴜsє <code>/setcplay &lt;chat_id&gt;</code> ᴛσ sєᴛ ɪᴛ."

cplay_usage: "<b>ᴜsᴧɢє:</b> <code>/setcplay &lt;channel_id&gt;</code>"

cplay_resolve_peer_fail: "<b>ғᴧɪʟєᴅ ᴛσ ʀєsσʟᴠє ᴘєєʀ:</b> ᴄσᴜʟᴅ ησᴛ ғєᴛᴄʜ ᴄʜᴧηηєʟ ᴅєᴛᴧɪʟs. єηsᴜʀє ɪ ᴄᴧη ᴧᴄᴄєss ᴛʜɪs ᴄʜᴧηηєʟ."

cplay_invalid_target: "<b>ɪηᴠᴧʟɪᴅ ᴛᴧʀɢєᴛ:</b> ᴛʜє ᴘʀσᴠɪᴅєᴅ ɪᴅ ɪs ησᴛ ᴧ ᴠᴧʟɪᴅ ᴄʜᴧηηєʟ."

cplay_channel_not_accessible: "<b>ᴄʜᴧηηєʟ ησᴛ ᴧᴄᴄєssɪʙʟє:</b> ᴄσᴜʟᴅ ησᴛ ʀєᴛʀɪєᴠє ᴄʜᴧηηєʟ ɪηғσʀᴍᴧᴛɪση. ᴘʟєᴧsє єηsᴜʀє ɪ ᴧᴍ ᴧη ᴧᴅᴍɪηɪsᴛʀᴧᴛσʀ ɪη ᴛʜє ᴛᴧʀɢєᴛ ᴄʜᴧηηєʟ."

cplay_bot_invite_permission_missing: "{emoji:⚠️} <b>ᴍɪssɪηɢ ᴘєʀᴍɪssɪση:</b> ɪ ᴧᴍ ᴧᴅᴍɪη ɪη ᴛᴧʀɢєᴛ ᴄʜᴧηηєʟ ʙᴜᴛ <code>ɪηᴠɪᴛє ᴜsєʀs</code> ᴘєʀᴍɪssɪση ɪs ᴅɪsᴧʙʟєᴅ. ᴘʟєᴧsє єηᴧʙʟє ɪᴛ ᴧηᴅ ᴛʀʏ ᴧɢᴧɪη."

cplay_channel_not_linked: "{emoji:⚠️} <b>ʟɪηᴋєᴅ ᴄʜᴧηηєʟ ησᴛ ғσᴜηᴅ:</b> ᴛʜɪs ᴄʜᴧᴛ ɪs ησᴛ ʟɪηᴋєᴅ ᴛσ ᴧηʏ ᴄʜᴧηηєʟ / ɢʀσᴜᴘ."

cplay_save_error: "<b>єʀʀσʀ:</b> ғᴧɪʟєᴅ ᴛσ sᴧᴠє ᴄᴘʟᴧʏ sєᴛᴛɪηɢs."

cplay_enabled: "{emoji:✅} <b>ᴄʜᴧηηєʟ ᴘʟᴧʏ єηᴧʙʟєᴅ.</b> ᴧʟʟ ᴄʜᴧηηєʟ-ᴘʟᴧʏ ᴄσᴍᴍᴧηᴅs (ʟɪᴋє <code>/cplay</code>, <code>/cskip</code>) ᴡɪʟʟ ησᴡ ᴡσʀᴋ ɪη ᴄʜᴧηηєʟ <code>{channel_id}</code>."

cplay_owner_required: "{emoji:⚠️} σηʟʏ ᴛʜє σᴡηєʀ σғ ᴛʜɪs ᴄʜᴧᴛ σʀ ᴛʜє ᴛᴧʀɢєᴛ ᴄʜᴧηηєʟ ᴄᴧη ᴜsє ᴛʜɪs sєᴛᴜᴘ."

cplay_owner_check_failed: "{emoji:⚠️} ᴄσᴜʟᴅ ησᴛ ᴠєʀɪғʏ ᴄʜᴧᴛ σᴡηєʀsʜɪᴘ ʀɪɢʜᴛ ησᴡ. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη."

cplay_disabled: "{emoji:✅} <b>ᴄʜᴧηηєʟ ᴘʟᴧʏ ᴅɪsᴧʙʟєᴅ.</b> ᴄʜᴧηηєʟ-ᴘʟᴧʏ ᴄσᴍᴍᴧηᴅs (ʟɪᴋє <code>/cplay</code>, <code>/cskip</code>) ᴧʀє ησᴡ ᴛᴜʀηєᴅ σғғ ғσʀ ᴛʜɪs ᴄʜᴧᴛ."

# Callback handler messages
invalid_request: "⚠️ ɪηᴠᴧʟɪᴅ ʀєǫᴜєsᴛ."

unknown_action: "⚠️ ᴜηᴋησᴡη ᴧᴄᴛɪση."

download_cancelled: "✅ ᴅσᴡηʟσᴧᴅ ᴄᴧηᴄєʟʟєᴅ."

no_download_to_cancel: "ℹ️ ησ ᴅσᴡηʟσᴧᴅ ᴛσ ᴄᴧηᴄєʟ."

# Pause callback
cb_pause_success: "⏸️ ᴛʀᴧᴄᴋ ᴘᴧᴜsєᴅ ᴧᴛ {position}"

cb_pause_message: |
  <b>{emoji:⏸️} ᴛʀᴧᴄᴋ ᴘᴧᴜsєᴅ</b>

  <b>▫ ᴛʀᴧᴄᴋ:</b> {title}
  <b>▫ ᴘσsɪᴛɪση:</b> {position} / {duration}
  <b>▫ ᴘᴧᴜsєᴅ ʙʏ:</b> {user}

# Resume callback
cb_already_playing: "ℹ️ ᴛʀᴧᴄᴋ ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ."

cb_resume_failed: "❌ ғᴧɪʟєᴅ ᴛσ ʀєsᴜᴍє ᴘʟᴧʏʙᴧᴄᴋ."

cb_resume_success: "▶️ ʀєsᴜᴍєᴅ ᴧᴛ {position}."

cb_resume_message: |
  <b>{emoji:🎵} ησᴡ ᴘʟᴧʏɪηɢ:</b>

  <b>▫ ᴛʀᴧᴄᴋ:</b> {title}
  <b>▫ ᴅᴜʀᴧᴛɪση:</b> {duration}
  <b>▫ ʀєsᴜᴍєᴅ ʙʏ:</b> {user}

# Replay status (loop replay)
cb_replaying: "{emoji:🔁} <b>ʀєᴘʟᴧʏɪηɢ ᴄᴜʀʀєηᴛ ᴛʀᴧᴄᴋ...</b>"

cb_replay_failed: "❌ ғᴧɪʟєᴅ ᴛσ ʀєᴘʟᴧʏ ᴛʀᴧᴄᴋ."

cb_replay_success: "🔁 ᴛʀᴧᴄᴋ ʀєᴘʟᴧʏєᴅ."

cb_replay_edited: "{emoji:🔁} ᴛʀᴧᴄᴋ ʀєᴘʟᴧʏєᴅ ʙʏ {user}."

# Seek callback (15s buttons)
cb_seek_success: "⏩ sσᴜɢʜᴛ ғσʀᴡᴧʀᴅ {seconds} sєᴄσηᴅs"
cb_seek_near_end: "⚠️ ᴄᴧηησᴛ sєєᴋ ғσʀᴡᴧʀᴅ {seconds} sєᴄσηᴅs — ᴧʙσᴜᴛ ᴛσ ʀєᴧᴄʜ єηᴅ."
cb_seek_edited: "⏩ sσᴜɢʜᴛ {seconds} sєᴄσηᴅs — ʙʏ {user}"
cb_seekback_success: "⏪ sσᴜɢʜᴛ ʙᴧᴄᴋ {seconds} sєᴄσηᴅs"
cb_seekback_edited: "⏪ sσᴜɢʜᴛ ʙᴧᴄᴋ {seconds} sєᴄσηᴅs — ʙʏ {user}"

# Skip callback
cb_skip_queue_empty: "⏹️ ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ — ǫᴜєᴜє єᴍᴘᴛʏ."

cb_skip_download_failed: "❌ ғᴧɪʟєᴅ ᴛσ ᴅσᴡηʟσᴧᴅ ηєxᴛ ᴛʀᴧᴄᴋ."

cb_skip_play_failed: "❌ ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ ηєxᴛ ᴛʀᴧᴄᴋ."

cb_skip_success: "⏭️ ᴛʀᴧᴄᴋ sᴋɪᴘᴘєᴅ."

cb_skip_edited: "{emoji:⏭️} sᴋɪᴘᴘєᴅ ʙʏ {user}"

# Stop callback
cb_stop_success: "⏹️ ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ."

# Mute/Unmute callback
cb_mute_success: "🔇 ᴍᴜᴛєᴅ."

cb_mute_message: |
  <b>{emoji:🔇} ᴘʟᴧʏʙᴧᴄᴋ ᴍᴜᴛєᴅ</b>

  <b>▫ ᴛʀᴧᴄᴋ:</b> {title}
  <b>▫ ᴍᴜᴛєᴅ ʙʏ:</b> {user}

cb_unmute_success: "🔊 ᴜηᴍᴜᴛєᴅ."

# 🗑️ Delete auth
del_auth_not_authorized: "{emoji:⚠️} ᴛʜᴧᴛ ᴜsєʀ ɪsη’ᴛ ᴧᴜᴛʜσʀɪᴢєᴅ — ησᴛʜɪηɢ ᴛσ ʀєᴍσᴠє."

del_auth_remove_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ʀєᴍσᴠє ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀ: <code>{error}</code>"

del_auth_success: "{emoji:✅} sᴜᴄᴄєssғᴜʟʟʏ ʀєᴍσᴠєᴅ {user} ғʀσᴍ ᴛʜє ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪsᴛ."

# 📜 Auth list
authlist_fetch_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ɢєᴛ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪsᴛ: <code>{error}</code>"

authlist_empty: "ℹ️ ᴛʜєʀє ᴧʀє ησ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ɪη ᴛʜɪs ᴄʜᴧᴛ."

authlist_fetching: "{emoji:⏳} ғєᴛᴄʜɪηɢ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ʟɪsᴛ..."

authlist_header: "<b>ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs:</b>"

authlist_user_fail: "{index}. <code>{user_id}</code> (ᴄσᴜʟᴅ ησᴛ ғєᴛᴄʜ ᴜsєʀ ɪηғσ)"

authlist_user_entry: "{index}. {user} (<code>{user_id}</code>)"

authlist_total: "<b>ᴛσᴛᴧʟ:</b> {count} ᴜsєʀs"

autoleave_fetch_fail: "{emoji:⚠️} <b>ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ᴧᴜᴛσʟєᴧᴠє sᴛᴧᴛє.</b>"

autoleave_status: "{emoji:🏃‍♂️} <b>ᴧᴜᴛσʟєᴧᴠє ᴄσηᴛʀσʟ</b>\n\nᴡʜєη єηᴧʙʟєᴅ, ᴛʜє ᴧssɪsᴛᴧηᴛ ᴡɪʟʟ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ʟєᴧᴠє ᴛʜє ᴄʜᴧᴛ.\nᴜsᴧɢє: {cmd} [єηᴧʙʟє|ᴅɪsᴧʙʟє]\nᴄᴜʀʀєηᴛ sᴛᴧᴛє: {action}"

autoleave_already: "{emoji:⚠️} ᴧᴜᴛσʟєᴧᴠє ɪs ᴧʟʀєᴧᴅʏ {action}"

autoleave_update_fail: "{emoji:⚠️} <b>ғᴧɪʟєᴅ ᴛσ ᴜᴘᴅᴧᴛє ᴧᴜᴛσʟєᴧᴠє sᴛᴧᴛє.</b>"

autoleave_updated: "{emoji:🏃‍♂️} ᴧᴜᴛσʟєᴧᴠє ʜᴧs ʙєєη {action}"

enabled: "єηᴧʙʟєᴅ"

disabled: "ᴅɪsᴧʙʟєᴅ"

clear_no_active: "{emoji:⚠️} <b>ησ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ σʀ ǫᴜєᴜє ᴛσ ᴄʟєᴧʀ.</b>"

clear_success: "{emoji:✅} ᴛʜє ǫᴜєᴜє ʜᴧs ʙєєη ᴄʟєᴧʀєᴅ ʙʏ {user}."

err_assistant_banned: |
  <b>{emoji:🚫} ᴧssɪsᴛᴧηᴛ ʀєsᴛʀɪᴄᴛєᴅ</b>

  ɪ ᴄᴧη'ᴛ ᴘʟᴧʏ ᴍᴜsɪᴄ ʙєᴄᴧᴜsє {user} (ᴜsєʀɪᴅ: <code>{id}</code>) ɪs <b>ʙᴧηηєᴅ</b> σʀ <b>ʀєᴍσᴠєᴅ</b> ғʀσᴍ ᴛʜɪs ᴄʜᴧᴛ.

  <i>{emoji:✅} ᴜηʙᴧηηєᴅ ᴛʜєᴍ ᴧʟʀєᴧᴅʏ? ᴜsє /reload ᴛσ ʀєғʀєsʜ.</i>

err_admin_permission_required: |
  {emoji:⚠️} <b>ᴧᴅᴍɪη ᴘєʀᴍɪssɪση ʀєǫᴜɪʀєᴅ</b>

  ɪ ηєєᴅ ᴧᴅᴍɪη ᴧᴄᴄєss ᴡɪᴛʜ ɪηᴠɪᴛє ᴜsєʀs ᴘєʀᴍɪssɪση ᴛσ ᴍᴧηᴧɢє ᴧηᴅ ɪηᴠɪᴛє ᴧssɪsᴛᴧηᴛ ɪη ᴛʜɪs ᴄʜᴧᴛ.

err_assistant_get_failed: |
  {emoji:⚠️} ᴛʜє sʏsᴛєᴍ ᴄσᴜʟᴅ ησᴛ ᴧssɪɢη ᴧη ᴧssɪsᴛᴧηᴛ ᴛσ ᴛʜɪs ᴄʜᴧᴛ.

  <i>ᴛʜɪs ɪs ᴜsᴜᴧʟʟʏ ᴧ ᴛєᴍᴘσʀᴧʀʏ ɪssᴜє ᴧηᴅ sʜσᴜʟᴅ ʀєsσʟᴠє ʙʏ ᴛʀʏɪηɢ ᴧɢᴧɪη.</i>

  <b>{emoji:➡️} ᴘʟєᴧsє ʀєᴛʀʏ ʏσᴜʀ ᴄσᴍᴍᴧηᴅ.</b>

  <i>ɪғ ᴛʜɪs ᴋєєᴘs ʜᴧᴘᴘєηɪηɢ, ᴘʟєᴧsє ᴄσηᴛᴧᴄᴛ sᴜᴘᴘσʀᴛ sσ ᴡє ᴄᴧη ᴧssɪsᴛ ʏσᴜ.</i>

err_assistant_join_request_sent: |
  {emoji:⚠️} ᴧssɪsᴛᴧηᴛ sєηᴛ ᴧ ᴊσɪη ʀєǫᴜєsᴛ, ʙᴜᴛ ɪ ᴄσᴜʟᴅ ησᴛ ᴧᴜᴛσ-ᴧᴘᴘʀσᴠє ɪᴛ.

  <i>{emoji:✅} ᴘʟєᴧsє ᴧᴘᴘʀσᴠє ᴛʜє ʀєǫᴜєsᴛ ᴍᴧηᴜᴧʟʟʏ, ᴛʜєη ᴛʀʏ ᴧɢᴧɪη.</i>

err_assistant_invite_link_fetch: |
  {emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ɪηᴠɪᴛє ʟɪηᴋ:
  <i>{error}</i>

err_assistant_invite_failed: |
  {emoji:⚠️} ᴧssɪsᴛᴧηᴛ ғᴧɪʟєᴅ ᴛσ ᴊσɪη ᴛʜɪs ᴄʜᴧᴛ:
  <i>{error}</i>

err_connection_timeout: |
  {emoji:⚠️} <b>ᴠσɪᴄєᴄʜᴧᴛ ᴄσηηєᴄᴛɪση ᴛɪᴍєσᴜᴛ</b>

  ᴛєʟєɢʀᴧᴍ ɪs ησᴛ ʀєsᴘσηᴅɪηɢ ᴘʀσᴘєʀʟʏ ᴛσ ᴛʜє ᴠσɪᴄє ᴄʜᴧᴛ ᴄσηηєᴄᴛɪση ʀɪɢʜᴛ ησᴡ.

  <i>ɪғ ᴛʜɪs ᴋєєᴘs ʜᴧᴘᴘєηɪηɢ, єηᴅ ᴛʜє ᴠσɪᴄє ᴄʜᴧᴛ ᴧηᴅ sᴛᴧʀᴛ ɪᴛ ᴧɢᴧɪη, ᴛʜєη ʀєᴛʀʏ.</i>

err_no_active_voicechat: |
  {emoji:🎙️} <b>ησ ᴧᴄᴛɪᴠє ᴠσɪᴄє ᴄʜᴧᴛ</b>

  ɪ ᴄᴧη'ᴛ ᴊσɪη — ᴘʟєᴧsє sᴛᴧʀᴛ ᴧ ᴠσɪᴄє ᴄʜᴧᴛ ғɪʀsᴛ.

  <i>ᴧʟʀєᴧᴅʏ sᴛᴧʀᴛєᴅ σηє? ᴜsє /reload ᴛσ ʀєғʀєsʜ.</i>

err_fetch_failed: |
  {emoji:⚠️} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ᴄʜᴧᴛ ɪηғσ:
  <i>{error}</i>

err_unknown: |
  {emoji:⚠️} ᴜηᴋησᴡη єʀʀσʀ σᴄᴄᴜʀʀєᴅ:
  <i>{error}</i>

err_fetch_tracks: |
  {emoji:⚠️} <b>ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ᴛʀᴧᴄᴋ ᴅєᴛᴧɪʟs.</b>
  ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη ʟᴧᴛєʀ.

err_download_internal: |
  {emoji:⚠️} <b>ᴅσᴡηʟσᴧᴅ ғᴧɪʟєᴅ</b> ᴅᴜє ᴛσ ᴧη ᴜηєxᴘєᴄᴛєᴅ ɪηᴛєʀηᴧʟ єʀʀσʀ.
  ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη ʟᴧᴛєʀ.

flood_seconds: "⏳ sʟσᴡ ᴅσᴡη! ᴛʀʏ ᴧɢᴧɪη ɪη {duration} sєᴄσηᴅs."

flood_minutes: "{emoji:⏳} ᴘʟєᴧsє ᴡᴧɪᴛ {duration} ᴍɪηᴜᴛєs ʙєғσʀє ᴜsɪηɢ ᴛʜɪs ᴄσᴍᴍᴧηᴅ ᴧɢᴧɪη."

bug_usage: "{emoji:⚠️} <b>ᴘʟєᴧsє ᴅєsᴄʀɪʙє ᴛʜє ʙᴜɢ.</b>\n\nᴜsᴧɢє: {cmd} &lt;ᴘʀσʙʟєᴍ&gt; σʀ ʀєᴘʟʏ ᴛσ ᴧ ᴍєssᴧɢє."

bug_thanks: "{emoji:✅} <b>ᴛʜᴧηᴋ ʏσᴜ ғσʀ ʏσᴜʀ ʙᴜɢ ʀєᴘσʀᴛ!</b>\nɪᴛ ʜᴧs ʙєєη sєηᴛ ᴛσ ᴛʜє ᴅєᴠєʟσᴘєʀs."

bug_private_chat: "ᴘʀɪᴠᴧᴛє ᴄʜᴧᴛ"

bug_reply_only: "<i>ʀєᴘʟʏ ʀєᴘσʀᴛ — sєє ғσʀᴡᴧʀᴅєᴅ ᴍєssᴧɢє.</i>"

bug_report_format: |
  {emoji:🐞} <b>ηєᴡ ʙᴜɢ ʀєᴘσʀᴛ</b>

  <b>ғʀσᴍ:</b> {user} (<code>{user_id}</code>)
  <b>ᴄʜᴧᴛ:</b> {chat} (<code>{chat_id}</code>)

  <b>ʀєᴘσʀᴛ:</b>
  <blockquote>{report}</blockquote>

help_private_only: "{emoji:🤖} ʜɪ! ғσʀ ʙσᴛ ʜєʟᴘ ᴧηᴅ ᴄσᴍᴍᴧηᴅs, ᴘʟєᴧsє ᴅᴍ ᴍє ᴅɪʀєᴄᴛʟʏ — ɪ'ᴍ ᴍσʀє ʀєsᴘσηsɪᴠє ɪη ᴘʀɪᴠᴧᴛє ᴄʜᴧᴛs!"

help_main: "<blockquote><b>🔮 єxᴘʟσʀє ᴛʜє ᴄσᴍᴘʟєᴛє ᴄσᴍᴍᴧηᴅ ɪηᴅєx ʙєʟσᴡ</b></blockquote>\n\n<blockquote><b>• ᴧᴄᴄєss єxᴘєʀᴛ ᴛєᴄʜηɪᴄᴧʟ ɢᴜɪᴅᴧηᴄє &amp; ʀєᴧʟ-ᴛɪᴍє sᴜᴘᴘσʀᴛ</b></blockquote>\n<blockquote><b>• єxєᴄᴜᴛє ᴧʟʟ ᴄσᴍᴍᴧηᴅs ᴜsɪηɢ sᴛᴧηᴅᴧʀᴅ ᴘʀєғɪx ➜</b></blockquote>\n"

jump_no_active: "{emoji:⚠️} ησ ᴧᴄᴛɪᴠє ᴛʀᴧᴄᴋ ᴛσ ᴊᴜᴍᴘ ᴡɪᴛʜɪη."

jump_usage: "{emoji:⚠️} ᴘʟєᴧsє ᴘʀσᴠɪᴅє sєᴄσηᴅs.\nєxᴧᴍᴘʟє: <code>{cmd} 120</code>"

jump_invalid_position: "{emoji:⚠️} ɪηᴠᴧʟɪᴅ ᴘσsɪᴛɪση.\nєxᴧᴍᴘʟє: <code>{cmd} 120</code>"

jump_too_close_end: "{emoji:⚠️} ᴄᴧηησᴛ ᴊᴜᴍᴘ ᴛσ <b>{position}</b> — ᴛʜᴧᴛ’s ᴛσσ ᴄʟσsє ᴛσ ᴛʜє єηᴅ σғ ᴛʜє ᴛʀᴧᴄᴋ."

jump_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴊᴜᴍᴘ ᴛσ <b>{position}</b>.\nєʀʀσʀ: <code>{error}</code>"

jump_success: "{emoji:⏩} ᴊᴜᴍᴘєᴅ ᴛσ <u>{position}</u> σғ <u>{duration}</u>."

lang_same: "ℹ️ ʏσᴜ’ʀє ᴧʟʀєᴧᴅʏ ᴜsɪηɢ ᴛʜɪs ʟᴧηɢᴜᴧɢє."

lang_select: "{emoji:🌐} sєʟєᴄᴛ ʏσᴜʀ ᴘʀєғєʀʀєᴅ ʟᴧηɢᴜᴧɢє:"

lang_fail: "❌ ғᴧɪʟєᴅ ᴛσ ᴜᴘᴅᴧᴛє ʟᴧηɢᴜᴧɢє. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη ʟᴧᴛєʀ."

lang_success: "{emoji:✅} ʟᴧηɢᴜᴧɢє sᴜᴄᴄєssғᴜʟʟʏ ᴜᴘᴅᴧᴛєᴅ ᴛσ {lang_name}."

logger_playback_started: "ᴘʟᴧʏʙᴧᴄᴋ sᴛᴧʀᴛєᴅ"

logger_playback_queued: "ᴘʟᴧʏʙᴧᴄᴋ ǫᴜєᴜєᴅ"

logger_bot_added: |
  {emoji:🤖} <b>ʙσᴛ ᴧᴅᴅєᴅ ᴛσ ηєᴡ ɢʀσᴜᴘ</b>
  
  <blockquote>
  <b>ɢʀσᴜᴘ ηᴧᴍє:</b> {group_name}
  <b>ɢʀσᴜᴘ ɪᴅ:</b> {group_id}
  <b>ɢʀσᴜᴘ ᴜsєʀηᴧᴍє:</b> {group_username}

  <b>ᴧᴅᴅєᴅ ʙʏ:</b> {added_by_name}
  <b>ᴜsєʀ ɪᴅ:</b> {added_by_id}
  <b>ᴜsєʀ ᴜsєʀηᴧᴍє:</b> {added_by_username}

  <b>ᴛɪᴍє:</b> {date_time}
  </blockquote>
#  👥 <b>Members Count:</b> {members_count}

logger_bot_removed: |

  {emoji:🤖} <b>ʙσᴛ ʀєᴍσᴠєᴅ ғʀσᴍ ɢʀσᴜᴘ</b>
  <blockquote>
  <b>ɢʀσᴜᴘ ηᴧᴍє:</b> {group_name}
  <b>ɢʀσᴜᴘ ɪᴅ:</b> {group_id}
  <b>ɢʀσᴜᴘ ᴜsєʀηᴧᴍє:</b> {group_username}

  <b>ʀєᴍσᴠєᴅ ʙʏ:</b> {removed_by_name}
  <b>ᴜsєʀ ɪᴅ:</b> {removed_by_id}
  <b>ᴜsєʀ ᴜsєʀηᴧᴍє:</b> {removed_by_username}

  <b>ᴛɪᴍє:</b> {date_time}
  </blockquote>

logger_bot_started: |
  {emoji:▶️} <b>{mention} sᴛᴧʀᴛєᴅ ᴛʜє ʙσᴛ</b>

  <b>ᴜsєʀηᴧᴍє:</b> {user_username}
  <b>ɪᴅ:</b> {user_id}

logger_playback_template: "<b>{emoji:🎧} ᴛʀᴧᴄᴋ:</b> {track}\n<b>{emoji:🔗} sσᴜʀᴄє:</b> {source}\n<b>{emoji:📌} ɢʀσᴜᴘ:</b> {group} ({group_id})\n<b>{emoji:👤} ʀєǫᴜєsᴛєᴅ ʙʏ:</b> {requested_by} (<code>{requested_by_id}</code>)"

loop_usage: "{emoji:🔁} <b>ʟσσᴘ ᴄσηᴛʀσʟ</b>\n\nᴜsᴧɢє: {cmd} [ᴄσᴜηᴛ]\n• 0 - ᴅɪsᴧʙʟє ʟσσᴘ\n• 1-10 - ʟσσᴘ ᴄσᴜηᴛ\n\n<i>{count_line}</i>"

loop_invalid: "{emoji:⚠️} <b>ɪηᴠᴧʟɪᴅ ʟσσᴘ ᴄσᴜηᴛ.</b>\nᴜsє 0 ᴛσ ᴅɪsᴧʙʟє σʀ 1-10 ᴛσ sєᴛ ʟσσᴘ ᴄσᴜηᴛ."

loop_already_set: "{emoji:⚠️} ʟσσᴘ ᴄσᴜηᴛ ɪs ᴧʟʀєᴧᴅʏ sєᴛ ᴛσ <b>{count}</b> ᴛɪᴍє(s)."

loop_disabled: "{emoji:🔁} ʟσσᴘ ʜᴧs ʙєєη <b>ᴅɪsᴧʙʟєᴅ</b> ʙʏ {user}"

loop_set: "{emoji:🔁} sєᴛ ᴛσ ʟσσᴘ <b>{count}</b> ᴛɪᴍє(s)\n└ ᴄʜᴧηɢєᴅ ʙʏ: {user}"

loop_current: "• ᴄᴜʀʀєηᴛ ʟσσᴘ: <b>{count}</b> ᴛɪᴍє(s)"

logger_usage: "{emoji:⚙️} ᴜsᴧɢє: <code>{cmd} [enable|disable]</code> - ᴛσ єηᴧʙʟє σʀ ᴅɪsᴧʙʟє ᴛʜє ʟσɢɢєʀ\n\n{status}"

logger_status: "{emoji:📜} ᴄᴜʀʀєηᴛ sᴛᴧᴛᴜs: {action}"

logger_check_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴄʜєᴄᴋ ʟσɢɢєʀ sᴛᴧᴛᴜs: {error}"

logger_already: "ℹ️ ʟσɢɢєʀ ɪs ᴧʟʀєᴧᴅʏ {action}."

logger_update_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴜᴘᴅᴧᴛє ʟσɢɢєʀ sєᴛᴛɪηɢ: {error}"

logger_updated: "{emoji:✅} ʟσɢɢєʀ ʜᴧs ʙєєη {action} sᴜᴄᴄєssғᴜʟʟʏ."

# Logs module
logs_empty: "ησ ʟσɢs ғσᴜηᴅ"

# reason - if maintenance reason available so at the place of {reason} the maint_reason will provided
maint: "⚠️ ɪ'ᴍ ᴜηᴅєʀ ᴍᴧɪηᴛєηᴧηᴄє ᴧᴛ ᴛʜє ᴍσᴍєηᴛ ᴧηᴅ ᴛєᴍᴘσʀᴧʀɪʟʏ ᴜηᴧᴠᴧɪʟᴧʙʟє. ᴘʟєᴧsє ᴄʜєᴄᴋ ʙᴧᴄᴋ ʟᴧᴛєʀ.\n\n{reason}"

maint_reason: "<i>{emoji:📝} ʀєᴧsση: {reason}</i>"

# ⚙️ Maintenance control
maint_usage: "{emoji:⚙️} <b>ᴍᴧɪηᴛєηᴧηᴄє ᴄσηᴛʀσʟ</b>\n\nᴜsᴧɢє: {cmd} [єηᴧʙʟє|ᴅɪsᴧʙʟє] [ʀєᴧsση]\n{emoji:📜} ᴄᴜʀʀєηᴛ sᴛᴧᴛᴜs: {status}"

maint_check_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴄʜєᴄᴋ ᴍᴧɪηᴛєηᴧηᴄє sᴛᴧᴛᴜs: <code>{error}</code>"

maint_already_enabled: "ℹ️ ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє ɪs ᴧʟʀєᴧᴅʏ єηᴧʙʟєᴅ {emoji:🟢}."

maint_already_disabled: "ℹ️ ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє ɪs ᴧʟʀєᴧᴅʏ ᴅɪsᴧʙʟєᴅ {emoji:🔴}."

maint_already_reason_same: "ℹ️ ᴍᴧɪηᴛєηᴧηᴄє ɪs ᴧʟʀєᴧᴅʏ єηᴧʙʟєᴅ ᴡɪᴛʜ ᴛʜє sᴧᴍє ʀєᴧsση."

maint_reason_removed: "{emoji:✅} ᴍᴧɪηᴛєηᴧηᴄє ʀєᴧsση ʀєᴍσᴠєᴅ sᴜᴄᴄєssғᴜʟʟʏ."

maint_reason_updated: "{emoji:✅} ᴍᴧɪηᴛєηᴧηᴄє ʀєᴧsση ᴜᴘᴅᴧᴛєᴅ.\n{emoji:📝} ʀєᴧsση: {reason}"

maint_enabled: "{emoji:✅} ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє ʜᴧs ʙєєη <b>єηᴧʙʟєᴅ</b> sᴜᴄᴄєssғᴜʟʟʏ."

maint_enabled_reason: "{emoji:✅} ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє єηᴧʙʟєᴅ.\n{emoji:📝} ʀєᴧsση: {reason}"

maint_disabled: "{emoji:✅} ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє ʜᴧs ʙєєη <b>ᴅɪsᴧʙʟєᴅ</b> sᴜᴄᴄєssғᴜʟʟʏ."

maint_entering: "{emoji:⚠️} ʙσᴛ ɪs єηᴛєʀɪηɢ ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє..."

enabled_with_reason: "{emoji:🟢} єηᴧʙʟєᴅ\n{emoji:📝} ʀєᴧsση: {reason}"

move_usage: "{emoji:⚠️} ᴜsᴧɢє: <code>{cmd} [from] [to]</code>"

move_invalid_numbers: "{emoji:⚠️} ɪηᴠᴧʟɪᴅ ηᴜᴍʙєʀs. єxᴧᴍᴘʟє: <code>/{cmd} 2 1</code>"

move_invalid_indexes_min: "{emoji:⚠️} ɪηᴅєxєs ᴍᴜsᴛ ʙє ɢʀєᴧᴛєʀ ᴛʜᴧη 0."

move_invalid_indexes_max: "{emoji:⚠️} ǫᴜєᴜє ʜᴧs σηʟʏ {queue_len} ᴛʀᴧᴄᴋs."

move_success: "{emoji:✅} ᴍσᴠєᴅ ᴛʀᴧᴄᴋ ғʀσᴍ ᴘσsɪᴛɪση {from} ᴛσ {to} ʙʏ {user}."

# 🔇 Mute messages
mute_already_muted: "🔇 <b>ᴧʟʀєᴧᴅʏ ᴍᴜᴛєᴅ</b>\nᴛʜє ᴍᴜsɪᴄ ɪs ᴧʟʀєᴧᴅʏ ᴍᴜᴛєᴅ ɪη ᴛʜɪs ᴄʜᴧᴛ.\nᴡσᴜʟᴅ ʏσᴜ ʟɪᴋє ᴛσ ᴜηᴍᴜᴛє ɪᴛ?"

mute_failed: "❌ <b>ᴘʟᴧʏʙᴧᴄᴋ ᴍᴜᴛє ғᴧɪʟєᴅ</b>\nєʀʀσʀ: <code>{error}</code>"

mute_success: |
  {emoji:🔇} <b>ᴍᴜᴛєᴅ ᴘʟᴧʏʙᴧᴄᴋ</b>

  {emoji:🎵} ᴛʀᴧᴄᴋ: {title}
  {emoji:👤} ᴍᴜᴛєᴅ ʙʏ: {user}

no_song_query: |
  <b>{emoji:🎵} ᴡʜσσᴘs! ησ sσηɢ ᴅєᴛєᴄᴛєᴅ.</b>

  <i>ᴛʏᴘє <b>{cmd} [sσηɢ ηᴧᴍє]</b> σʀ ʀєᴘʟʏ ᴛσ ᴧ <b>ᴍєᴅɪᴧ</b> ᴛσ sᴛᴧʀᴛ ᴘʟᴧʏɪηɢ!</i>

no_song_found: "{emoji:❌} ησ ᴛʀᴧᴄᴋs ғσᴜηᴅ."

nothumb_status: |
  {emoji:🖼️} <b>ᴛʜᴜᴍʙηᴧɪʟ ᴄσηᴛʀσʟ</b>
  
  <i>ᴡʜєη єηᴧʙʟєᴅ, ᴘʟᴧʏʙᴧᴄᴋ ᴍєssᴧɢєs ᴡση'ᴛ sʜσᴡ ᴧʀᴛᴡσʀᴋ / ᴛʜᴜᴍʙηᴧɪʟs.</i>
  
  <b>{emoji:📊} ᴜsᴧɢє: {cmd} [єηᴧʙʟє|ᴅɪsᴧʙʟє]</b>
  
  <i>ᴛʜᴜᴍʙηᴧɪʟ ɪs {action} ᴄᴜʀʀєηᴛʟʏ.</i>

nothumb_fetch_fail: "{emoji:⚠️} <b>ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ᴛʜᴜᴍʙηᴧɪʟ sєᴛᴛɪηɢ.</b>"

nothumb_already: "ℹ️ ᴛʜᴜᴍʙηᴧɪʟ ᴅɪsᴘʟᴧʏ ɪs ᴧʟʀєᴧᴅʏ {action}."

nothumb_update_fail: "{emoji:⚠️} <b>ғᴧɪʟєᴅ ᴛσ ᴜᴘᴅᴧᴛє ᴛʜᴜᴍʙηᴧɪʟ sєᴛᴛɪηɢ.</b>"

nothumb_updated: "{emoji:🖼️} ᴛʜᴜᴍʙηᴧɪʟ ᴅɪsᴘʟᴧʏ ʜᴧs ʙєєη {action}."

cmddelete_status: "{emoji:🗑️} <b>ᴄσᴍᴍᴧηᴅ ᴅєʟєᴛє ᴄσηᴛʀσʟ</b>\n\nᴡʜєη єηᴧʙʟєᴅ, ɪ ᴡɪʟʟ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ᴅєʟєᴛє ᴄσᴍᴍᴧηᴅ ᴍєssᴧɢєs.\nᴜsᴧɢє: {cmd} [єηᴧʙʟє|ᴅɪsᴧʙʟє]\nᴄᴜʀʀєηᴛ sᴛᴧᴛє: {action}"

cmddelete_updated: "{emoji:🗑️} ᴄσᴍᴍᴧηᴅ ᴅєʟєᴛɪση ʜᴧs ʙєєη {action}."

only_admin: "{emoji:⚠️} <b>ᴘєʀᴍɪssɪση ᴅєηɪєᴅ!</b>\nσηʟʏ <b>ᴧᴅᴍɪηs</b> ᴄᴧη ᴄσηᴛʀσʟ ᴛʜєsє ᴧᴄᴛɪσηs. ʏσᴜ ᴄᴧη sᴛɪʟʟ ᴘʟᴧʏ sσηɢs {emoji:🎵}.\n\nɪғ ʏσᴜ ʙєʟɪєᴠє ʏσᴜ ᴧʀє ᴧη ᴧᴅᴍɪη, ᴘʟєᴧsє ᴜsє /reload ᴛσ ʀєғʀєsʜ ʏσᴜʀ ᴧᴅᴍɪη sᴛᴧᴛᴜs."

only_admin_cb: "⚠️ σηʟʏ ᴧᴅᴍɪηs ᴄᴧη ᴅσ ᴛʜєsє ᴧᴄᴛɪσηs."

only_admin_or_auth: "{emoji:⚠️} <b>ᴧᴄᴄєss ᴅєηɪєᴅ</b>\nσηʟʏ <b>ᴧᴅᴍɪηs</b> σʀ <b>ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs</b> ᴄᴧη ᴄσηᴛʀσʟ ᴛʜєsє ᴧᴄᴛɪσηs.\n\nɪғ ʏσᴜ ʀєᴄєηᴛʟʏ ʙєᴄᴧᴍє ᴧη ᴧᴅᴍɪη, ᴜsє /reload ᴛσ ʀєғʀєsʜ ʏσᴜʀ ᴘєʀᴍɪssɪσηs."

only_admin_or_auth_cb: "⚠️ σηʟʏ ᴧᴅᴍɪηs σʀ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ᴄᴧη ᴅσ ᴛʜєsє ᴧᴄᴛɪσηs."

only_supergroup: "{emoji:⚠️} ᴛʜɪs ᴄσᴍᴍᴧηᴅ ᴄᴧη σηʟʏ ʙє ᴜsєᴅ ɪη ɢʀσᴜᴘs."

only_owner: "⚠️ σηʟʏ ᴛʜє ʙσᴛ σᴡηєʀ ᴄᴧη ᴜsє ᴛʜɪs ᴄσᴍᴍᴧηᴅ."

only_sudo: "{emoji:⚠️} σηʟʏ sᴜᴅσ ᴜsєʀs σʀ ᴛʜє ʙσᴛ σᴡηєʀ ᴄᴧη ᴜsє ᴛʜɪs ᴄσᴍᴍᴧηᴅ."

pause_already: |
  {emoji:⏸️} <b>ᴧʟʀєᴧᴅʏ ᴘᴧᴜsєᴅ</b>
  ᴛʜє ᴍᴜsɪᴄ ɪs ᴧʟʀєᴧᴅʏ ᴘᴧᴜsєᴅ.

pause_success: |
  {emoji:⏸️} <b>ᴘᴧᴜsєᴅ ᴘʟᴧʏʙᴧᴄᴋ</b>

  {emoji:🎵} ᴛʀᴧᴄᴋ: {title}
  {emoji:⏱️} ᴘσsɪᴛɪση: {position} / {duration}
  {emoji:👤} ᴘᴧᴜsєᴅ ʙʏ: {user}

play_single_track_too_long: |
  {emoji:⚠️} ʏσᴜ ᴄᴧη'ᴛ ᴘʟᴧʏ sσηɢs ʟσηɢєʀ ᴛʜᴧη {limit_mins} ᴍɪηᴜᴛєs.
  <i>{title}</i> ᴡᴧs sᴋɪᴘᴘєᴅ.

play_download_failed: "{emoji:❌} ᴅσᴡηʟσᴧᴅ ғᴧɪʟєᴅ. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη."

play_added_to_queue_single: |
  <blockquote><b>{emoji:💮} ɪηᴄσᴍɪηɢ ᴛʀᴧᴄᴋ ᴅєᴛєᴄᴛєᴅ : #{index}</b></blockquote>
  <blockquote expandable><b>{emoji:🎋} ᴍєʟσᴅʏ :</b> {title}
  <b>{emoji:✨} ʟєηɢᴛʜ :</b> {duration}
  <b>{emoji:🥀} ʀєǫᴜєsᴛєʀ :</b> {by}

  {emoji:💐} sᴛᴧηᴅʙʏ, ʏσᴜʀ sєssɪση ʙєɢɪηs sʜσʀᴛʟʏ</blockquote>

play_multiple_tracks_too_long_header: "<b>{emoji:⚠️} {count} ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ (ᴍᴧx ᴅᴜʀᴧᴛɪση {limit_mins} ᴍɪηs):</b>"

play_multiple_tracks_too_long_item: "— <i>{title}</i>"

play_multiple_tracks_too_long_more: "... ᴧηᴅ {remaining} ᴍσʀє."

play_all_tracks_skipped: "{emoji:❌} ᴧʟʟ ғσᴜηᴅ ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ ᴅᴜє ᴛσ ᴅᴜʀᴧᴛɪση ʟɪᴍɪᴛs."

play_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ\nєʀʀσʀ: {error}"

play_unable: "<b> {emoji:❌} ᴜηᴧʙʟє ᴛσ ᴘʟᴧʏ </b>\n\n<i>ᴘʟєᴧsє <b>ʀєsᴛᴧʀᴛ ᴛʜє ᴠσɪᴄє</b> ᴄʜᴧᴛ.</i>"

rtmp_play_unsupported: "{emoji:⚠️} <b>ᴄᴧη'ᴛ ᴘʟᴧʏ ɪη ᴧη ʀᴛᴍᴘ / ʟɪᴠє-sᴛʀєᴧᴍ ᴠσɪᴄє ᴄʜᴧᴛ.</b>\n\n<i>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ.</i>"

play_downloading_song: "{emoji:📥} ᴅσᴡηʟσᴧᴅɪηɢ sσηɢ \"{title}\""

play_download_canceled: "{emoji:⚠️} ᴅσᴡηʟσᴧᴅ ᴄᴧηᴄєʟєᴅ ʙʏ {user}."

play_already_in_queue: "{emoji:✅} <b>ᴛʀᴧᴄᴋ ᴧʟʀєᴧᴅʏ ᴧᴅᴅєᴅ!</b>\n\n<i>{title}</i> ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ σʀ ɪη ᴛʜє ǫᴜєᴜє."

play_already_playing: "{emoji:⚠️} <b>ᴧ sσηɢ ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ.</b>\n\nᴘʟєᴧsє ᴡᴧɪᴛ ғσʀ ɪᴛ ᴛσ ғɪηɪsʜ σʀ ᴜsє <code>/stop</code> ᴛσ sᴛσᴘ ɪᴛ ғɪʀsᴛ."

play_added_multiple_header: "{emoji:➕} <b>ᴧᴅᴅєᴅ {count} ᴛʀᴧᴄᴋs</b> ʙʏ {user}"

play_queue_limit_hint: "{emoji:⚠️} <i>ǫᴜєᴜє ʟɪᴍɪᴛ ʀєᴧᴄʜєᴅ — sσᴍє ᴛʀᴧᴄᴋs ᴡєʀє sᴋɪᴘᴘєᴅ.</i>"

play_queue_view_hint: "<i>ᴜsє /queue ᴛσ ᴠɪєᴡ ғᴜʟʟ ʟɪsᴛ.</i>"

playmode_help: |
  {emoji:🛠️} <b>ᴘʟᴧʏ ᴍσᴅє</b>

  <b>sᴛᴧᴛᴜs:</b> {status}
  <b>ᴜsᴧɢє:</b> <code>/playmode [enable|disable]</code>

playmode_status_admins: "ᴧᴅᴍɪηs 🔐"

playmode_status_everyone: "єᴠєʀʏσηє 🌍"

playmode_success_admins: "{emoji:🛡️} <b>ᴘʟᴧʏ ᴍσᴅє</b> ɪs ησᴡ ʀєsᴛʀɪᴄᴛєᴅ ᴛσ <b>ᴧᴅᴍɪηs & ᴅᴊs</b> σηʟʏ. {emoji:🎧}"

playmode_success_everyone: "{emoji:✅} <b>ᴘʟᴧʏ ᴍσᴅє</b> ɪs ησᴡ <b>σᴘєη ᴛσ єᴠєʀʏσηє</b>. ʟєᴛ ᴛʜє ᴍᴜsɪᴄ ғʟσᴡ! {emoji:🎶}"

playmode_restricted: "{emoji:🚫} ᴘʟᴧʏ ᴍσᴅє ɪs єηᴧʙʟєᴅ. σηʟʏ ᴧᴅᴍɪηs ᴧηᴅ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ᴄᴧη ᴘʟᴧʏ."

# 🏓 Ping messages
ping_start: "{emoji:🏓} ᴘɪηɢɪηɢ..."

ping_result: |
  <b>{emoji:🏓} ᴘσηɢ</b>: <code>{latency}ms</code>
  
  <b><u>{emoji:🤖} {bot} sʏsᴛєᴍ sᴛᴧᴛ's :</u></b>
  
  <b>ᴜᴘᴛɪᴍє:</b> {uptime}
  <b>ʀᴧᴍ ᴜsᴧɢє:</b> {ram_info}
  <b>ᴄᴘᴜ ᴜsᴧɢє:</b> {cpu_usage}
  <b>ᴅɪsᴋ ᴜsᴧɢє:</b> {disk_usage}

position_now: |
  {emoji:🎵} <b>{title}</b>
  {emoji:📍} <code>{position} / {duration}</code>
  {emoji:⚙️} sᴘєєᴅ: <b>{speed}x</b>

queue_limit_reached: "{emoji:⚠️} ǫᴜєᴜє ʟɪᴍɪᴛ ʀєᴧᴄʜєᴅ ({limit} ᴛʀᴧᴄᴋs ᴍᴧx). ᴜsє /clear ᴛσ ᴄʟєᴧʀ ǫᴜєᴜє."

queue_no_active: "{emoji:⚠️} <b>ησ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ.</b>\n\n<i>ησᴛʜɪηɢ ɪs ǫᴜєᴜєᴅ ʀɪɢʜᴛ ησᴡ.</i>"

queue_header: "{emoji:🎶} <b>ᴄᴜʀʀєηᴛ ǫᴜєᴜє</b>"

queue_now_playing: "{emoji:▶️} <b>ησᴡ ᴘʟᴧʏɪηɢ:</b>"

queue_up_next: "{emoji:⏭️} <b>ᴜᴘ ηєxᴛ:</b>"

queue_more_line: "\n… ᴧηᴅ {remaining} ᴍσʀє"

queue_empty_tail: "{emoji:📭} <i>ησ ᴍσʀє sσηɢs ɪη ǫᴜєᴜє.</i>"

queue_empty: "{emoji:⚠️} <b>ᴛʜє ǫᴜєᴜє ɪs ᴧʟʀєᴧᴅʏ єᴍᴘᴛʏ.</b>"

# 🔄 Reload
reload_start: "{emoji:⚙️} ʀєʟσᴧᴅɪηɢ ᴧᴅᴍɪη ᴄᴧᴄʜє, ᴠσɪᴄє ᴄʜᴧᴛ sᴛᴧᴛᴜs, ᴧηᴅ ᴧssɪsᴛᴧηᴛ sᴛᴧᴛᴜs..."

reload_done: "<b>{emoji:⚙️} ʀєʟσᴧᴅ ᴄσᴍᴘʟєᴛє:</b>\n\n{summary}"

reload_admin_cache_ok: "<b>• ᴧᴅᴍɪη ᴄᴧᴄʜє:</b> <i>{emoji:✅} ᴅσηє</i>"

reload_admin_cache_fail: "<b>• ᴧᴅᴍɪη ᴄᴧᴄʜє:</b> <i>{emoji:❌} ({error})</i>"

reload_voice_inactive: "<b>• ᴠσɪᴄє ᴄʜᴧᴛ:</b> <i>{emoji:⚪} ɪηᴧᴄᴛɪᴠє</i>"

reload_voice_admin_required: "<b>• ᴠσɪᴄє ᴄʜᴧᴛ:</b> <i>{emoji:❌} ᴧᴅᴍɪη ᴘєʀᴍɪssɪση ʀєǫᴜɪʀєᴅ</i>"

reload_voice_fail: "<b>• ᴠσɪᴄє ᴄʜᴧᴛ:</b> <i>{emoji:❌} ({error})</i>"

reload_voice_active: "<b>• ᴠσɪᴄє ᴄʜᴧᴛ:</b> <i>{emoji:✅} ᴧᴄᴛɪᴠє</i>"

reload_assistant_banned: "<b>• ᴧssɪsᴛᴧηᴛ:</b> <i>{emoji:❌} ʙᴧηηєᴅ</i>"

reload_assistant_admin_required: "<b>• ᴧssɪsᴛᴧηᴛ:</b> <i>{emoji:❌} ᴧᴅᴍɪη ᴘєʀᴍɪssɪση ʀєǫᴜɪʀєᴅ</i>"

reload_assistant_fail: "<b>• ᴧssɪsᴛᴧηᴛ:</b> <i>{emoji:❌} ({error})</i>"

reload_assistant_present: "<b>• ᴧssɪsᴛᴧηᴛ:</b> <i>{emoji:✅} ᴘʀєsєηᴛ</i>"

reload_assistant_not_present: "<b>• ᴧssɪsᴛᴧηᴛ:</b> <i>{emoji:⚪} ησᴛ ᴘʀєsєηᴛ</i>"

reload_room_reset: "<b>• ʀσσᴍ:</b> <i>ʀєsєᴛ {emoji:✅}</i>"

remove_usage: "{emoji:⚠️} <b>ɪηᴠᴧʟɪᴅ ᴜsᴧɢє.</b>\nᴜsᴧɢє: <code>{cmd} [index]</code>"

remove_invalid_index: "{emoji:⚠️} <b>ɪηᴠᴧʟɪᴅ ɪηᴅєx.</b> ɪᴛ ᴍᴜsᴛ ʙє ᴧ <b>ηᴜᴍʙєʀ</b>."

remove_index_too_small: "{emoji:⚠️} <b>ɪηᴅєx ᴍᴜsᴛ ʙє ɢʀєᴧᴛєʀ ᴛʜᴧη 0.</b>"

remove_index_too_big: "{emoji:⚠️} ɪηᴠᴧʟɪᴅ ɪηᴅєx. ǫᴜєᴜє σηʟʏ ʜᴧs <b>{total}</b> ᴛʀᴧᴄᴋs."

remove_success: "{emoji:✅} ʀєᴍσᴠєᴅ ᴛʀᴧᴄᴋ ᴧᴛ ᴘσsɪᴛɪση <b>{index}</b> ғʀσᴍ ᴛʜє ǫᴜєᴜє.\n{emoji:👤} ʙʏ: {user}"

# 🔁 Replay
replay_failed: "❌ <b>ʀєᴘʟᴧʏ ғᴧɪʟєᴅ</b>\nєʀʀσʀ: <code>{error}</code>"

replay_success: |
  {emoji:🔁} <b>ησᴡ ʀєᴘʟᴧʏɪηɢ</b>

  {emoji:🎵} ᴛɪᴛʟє: {title}
  {emoji:⏱️} ᴅᴜʀᴧᴛɪση: <code>{duration}</code>
  {emoji:⚙️} sᴘєєᴅ: <b>{speed}x</b>

# ▶️ Resume
resume_already_playing: "ℹ️ <b>ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ</b>\nᴛʜє ᴍᴜsɪᴄ ɪs ᴧʟʀєᴧᴅʏ ᴘʟᴧʏɪηɢ ɪη ᴛʜɪs ᴄʜᴧᴛ.\nᴡσᴜʟᴅ ʏσᴜ ʟɪᴋє ᴛσ ᴘᴧᴜsє ɪᴛ?"

resume_failed: "{emoji:❌} <b>ᴘʟᴧʏʙᴧᴄᴋ ʀєsᴜᴍє ғᴧɪʟєᴅ</b>\nєʀʀσʀ: <code>{error}</code>"

resume_success: |
  {emoji:▶️} <b>ʀєsᴜᴍɪηɢ ᴘʟᴧʏʙᴧᴄᴋ</b>

  {emoji:🎵} ᴛɪᴛʟє: {title}
  {emoji:📍} ᴘσsɪᴛɪση: {position} / {duration}
  {emoji:👤} ʀєsᴜᴍєᴅ ʙʏ: {user}
  {speed_line}

restart: "{emoji:♻️} ʀєsᴛᴧʀᴛɪηɢ..."

restart_exepath_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ɢєᴛ єxєᴄᴜᴛᴧʙʟє ᴘᴧᴛʜ: {error}"

restart_symlink_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ʀєsσʟᴠє єxєᴄᴜᴛᴧʙʟє ᴘᴧᴛʜ: {error}"

restart_service: "{bot} ʜᴧs ᴊᴜsᴛ ʀєsᴛᴧʀᴛєᴅ. sσʀʀʏ ғσʀ ᴛʜє ɪssᴜєs.\n\nsᴛᴧʀᴛ ᴘʟᴧʏɪηɢ ᴧɢᴧɪη ᴧғᴛєʀ 10–15 sєᴄσηᴅs."

restart_initiated: "{emoji:✅} ʀєsᴛᴧʀᴛ ɪηɪᴛɪᴧᴛєᴅ sᴜᴄᴄєssғᴜʟʟʏ!"

restart_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ʀєsᴛᴧʀᴛ: {error}"

restart_confirm_running: "{emoji:⚠️} ᴧ sσηɢ ɪs ᴄᴜʀʀєηᴛʟʏ ᴘʟᴧʏɪηɢ ɪη ᴛʜɪs ᴄʜᴧᴛ.\nᴄʜσσsє ᴧη ᴧᴄᴛɪση ʙєʟσᴡ."

restart_btn_bot: "ʀєsᴛᴧʀᴛ ᴛʜє ʙσᴛ"

restart_btn_replay: "ʀєᴘʟᴧʏ ᴛʜє sσηɢ"

restart_confirm_bot: "♻️ ʀєsᴛᴧʀᴛɪηɢ ᴛʜє ʙσᴛ..."

restart_confirm_replay_done: "⏮️ ᴄᴜʀʀєηᴛ sσηɢ ʀєᴘʟᴧʏєᴅ sᴜᴄᴄєssғᴜʟʟʏ."

room_not_active_cb: "⚠️ ησᴛʜɪηɢ ᴘʟᴧʏɪηɢ ʀɪɢʜᴛ ησᴡ."

room_no_active: "⚠️ <b>ησ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ.</b>\n\n<i>ᴛʜєʀє's ησᴛʜɪηɢ ᴘʟᴧʏɪηɢ ʀɪɢʜᴛ ησᴡ.</i>"

room_already_paused: "⏸️ ᴛʀᴧᴄᴋ ɪs ᴧʟʀєᴧᴅʏ ᴘᴧᴜsєᴅ. ᴛᴧᴘ ▶️ ʀєsᴜᴍє ᴛσ ᴄσηᴛɪηᴜє."

room_pause_failed: "❌ ғᴧɪʟєᴅ ᴛσ ᴘᴧᴜsє ᴘʟᴧʏʙᴧᴄᴋ.\n\n<i>{error}</i>"

generic_error: "{emoji:❌} sσᴍєᴛʜɪηɢ ᴡєηᴛ ᴡʀσηɢ: <code>{error}</code>"

searching: "{emoji:🔍}{emoji:🎶} sєᴧʀᴄʜɪηɢ... {emoji:⚡}{emoji:✨}"

searching_query: "{emoji:🔍}{emoji:🎶} sєᴧʀᴄʜɪηɢ ғσʀ: <b>{query}</b>... {emoji:⚡}{emoji:✨}"

settings_btn_adminmode: "🛡️ ᴧᴅᴍɪη ᴍσᴅє"

settings_btn_cmddelete: "🗑️ ᴄᴍᴅ ᴅєʟєᴛє"

settings_btn_lang: "🌐 ʟᴧηɢᴜᴧɢє"

settings_btn_nothumb: "🖼️ ᴛʜᴜᴍʙηᴧɪʟs"

settings_btn_playmode: "🎵 ᴘʟᴧʏ ᴍσᴅє"

settings_info_adminmode: |
  🛡️ ᴡʜᴧᴛ ɪs ᴛʜɪs?

  ᴄσηᴛʀσʟs ᴡʜσ ᴄᴧη ᴜsє ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅs σғ ᴛʜє ʙσᴛ (sᴋɪᴘ, ᴘᴧᴜsє, єᴛᴄ.).

settings_info_cmddelete: |
  🗑️ ᴡʜᴧᴛ ɪs ᴛʜɪs?

  ᴡʜєη єηᴧʙʟєᴅ, ᴛʜє ʙσᴛ ᴡɪʟʟ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ᴅєʟєᴛє ᴄσᴍᴍᴧηᴅ ᴍєssᴧɢєs ᴧғᴛєʀ ᴘʀσᴄєssɪηɢ ᴛʜєᴍ ᴛσ ᴋєєᴘ ᴛʜє ᴄʜᴧᴛ ᴄʟєᴧη.

settings_btn_cleanduration: "⏱️ ᴄʟєᴧη ᴅєʟᴧʏ"

settings_btn_cleanmode: "🧹 ᴄʟєᴧη ᴍσᴅє"

settings_info_cleanduration: |
  ᴄʏᴄʟєs ᴧᴜᴛσ-ᴅєʟєᴛє ᴅєʟᴧʏ: 15ᴍ → 30ᴍ → 60ᴍ → 5ᴍ.

settings_info_cleanmode: |
  ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ʀєᴍσᴠєs ᴄσᴍᴍᴧηᴅ ᴧηᴅ ʙσᴛ sєʀᴠɪᴄє ᴍєssᴧɢєs ᴧғᴛєʀ ᴧ sʜσʀᴛ ᴅєʟᴧʏ.
  ᴘʟᴧʏɪηɢ sᴛᴧᴛᴜs ᴍєssᴧɢєs ᴧʀє ᴋєᴘᴛ ᴜηᴛɪʟ ᴛʜєʏ ηᴧᴛᴜʀᴧʟʟʏ єxᴘɪʀє.
cleanmode_status: "{emoji:🧹} <b>ᴄʟєᴧη ᴍσᴅє</b>\n\nᴧᴜᴛσ-ᴄʟєᴧηᴜᴘ σғ ᴄσᴍᴍᴧηᴅ ᴧηᴅ sєʀᴠɪᴄє ᴍєssᴧɢєs.\nᴄᴜʀʀєηᴛ sᴛᴧᴛє: {action}\nᴅєʟєᴛє ᴧғᴛєʀ: {duration}ᴍ"

cleanmode_hint: "ᴜsє /settings ᴛσ ᴄʜᴧηɢє ᴧᴜᴛσ-ᴅєʟєᴛє ᴅᴜʀᴧᴛɪση."

settings_info_lang: |
  🌐 ᴡʜᴧᴛ ɪs ᴛʜɪs?

  sєʟєᴄᴛ ᴛʜє ʟᴧηɢᴜᴧɢє ғσʀ ᴛʜє ʙσᴛ's ᴍєssᴧɢєs ᴧηᴅ ɪηᴛєʀғᴧᴄє ɪη ᴛʜɪs ɢʀσᴜᴘ.

settings_info_nothumb: |
  🖼️ ᴡʜᴧᴛ ɪs ᴛʜɪs?

  ᴛσɢɢʟє ᴡʜєᴛʜєʀ ᴛσ sʜσᴡ ᴛʀᴧᴄᴋ ᴧʀᴛᴡσʀᴋ / ᴛʜᴜᴍʙηᴧɪʟs ɪη ᴘʟᴧʏʙᴧᴄᴋ ᴍєssᴧɢєs.

settings_info_playmode: |
  🎵 ᴡʜᴧᴛ ɪs ᴛʜɪs?

  ᴄσηᴛʀσʟ ᴡʜσ ᴄᴧη ᴜsє ᴛʜє /play ᴄσᴍᴍᴧηᴅ. ɪғ єηᴧʙʟєᴅ, σηʟʏ ᴧᴅᴍɪηɪsᴛʀᴧᴛσʀs ᴧηᴅ ᴧᴜᴛʜσʀɪᴢєᴅ ᴜsєʀs ᴄᴧη sᴛᴧʀᴛ ᴘʟᴧʏʙᴧᴄᴋ.

settings_main: |
  {emoji:⚙️} <b>ᴄʜᴧᴛ sєᴛᴛɪηɢs</b>

  <b>ɢʀσᴜᴘ:</b> {title}
  <b>ɢʀσᴜᴘ ɪᴅ:</b> <code>{id}</code>

  ᴄσηғɪɢᴜʀє ᴛʜє ʙσᴛ's ʙєʜᴧᴠɪσʀ ɪη ᴛʜɪs ɢʀσᴜᴘ ᴜsɪηɢ ᴛʜє ʙᴜᴛᴛσηs ʙєʟσᴡ.

settings_updated: "✅ sєᴛᴛɪηɢs ᴜᴘᴅᴧᴛєᴅ!"

autoplay_no_active: "⚠️ ʙσᴛ ɪs ησᴛ sᴛʀєᴧᴍɪηɢ ɪη ᴧ ᴠσɪᴄє ᴄʜᴧᴛ. sᴛᴧʀᴛ ᴘʟᴧʏɪηɢ ᴧ sσηɢ ғɪʀsᴛ."

autoplay_menu: |
  {emoji:🔁} <b>ᴧᴜᴛσᴘʟᴧʏ</b>

  ᴡʜєη єηᴧʙʟєᴅ, σηᴄє ᴛʜє ǫᴜєᴜє єηᴅs ᴛʜє ʙσᴛ ᴡɪʟʟ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ᴋєєᴘ ᴘʟᴧʏɪηɢ ʀєᴄσᴍᴍєηᴅєᴅ sσηɢs sɪᴍɪʟᴧʀ ᴛσ ᴛʜє ʟᴧsᴛ ᴘʟᴧʏєᴅ ᴛʀᴧᴄᴋ.

  <b>sᴛᴧᴛᴜs:</b> {state}

autoplay_btn: "♫ ᴧᴜᴛσᴘʟᴧʏ: {state}"

autoplay_enabled: "{emoji:✅} ᴧᴜᴛσᴘʟᴧʏ єηᴧʙʟєᴅ!"

autoplay_disabled: "{emoji:❌} ᴧᴜᴛσᴘʟᴧʏ ᴅɪsᴧʙʟєᴅ!"

autoplay_requester: "ᴧᴜᴛσᴘʟᴧʏ"
askip_disabled: "{emoji:⚠️} ᴧᴜᴛσᴘʟᴧʏ ɪs ᴅɪsᴧʙʟєᴅ. ᴜsє /autoplay ᴛσ єηᴧʙʟє ɪᴛ."
askip_no_recommendations: "{emoji:⚠️} ησ ᴧᴜᴛσᴘʟᴧʏ ʀєᴄσᴍᴍєηᴅᴧᴛɪσηs ғσᴜηᴅ ᴛσ sᴋɪᴘ ᴛσ."
askip_success: "{emoji:⏭️} ᴧᴜᴛσᴘʟᴧʏ sᴋɪᴘᴘєᴅ ʙʏ {user}\n{emoji:🔁} ᴘʟᴧʏɪηɢ: <b>{title}</b>"

speed_line: "{emoji:⚙️} sᴘєєᴅ: <b>{speed}x</b>"

stats_overview: |
  {emoji:🔧} <b>sʏsᴛєᴍ:</b><blockquote>
  • σs: <code>{os}</code>, ᴧʀᴄʜ: <code>{arch}</code>
  • ᴄᴘᴜs: <code>{cpus}</code>, ᴛʜʀєᴧᴅs: <code>{goroutines}</code></blockquote>

  {emoji:📦} <b>ɪηᴛєʀηᴧʟ ᴍєᴍσʀʏ (ɢσ):</b><blockquote>
  • ᴧʟʟσᴄ: <code>{alloc} MB</code>
  • sʏs: <code>{sys} MB</code>
  • ηᴜᴍɢᴄ: <code>{gc_count}</code> ({gc_emoji} {gc_rate} / ᴍɪη)</blockquote>

  {emoji:💻} <b>sєʀᴠєʀ sᴛᴧᴛs:</b><blockquote>
  • ᴄᴘᴜ ᴜsᴧɢє: {cpu_emoji} <code>{cpu}%</code>
  • ʀᴧᴍ ᴜsᴧɢє: {ram_emoji} <code>{ram_used_gib} GiB</code> | <code>{ram_total_gib} GiB</code>
  • sᴛσʀᴧɢє: <code>{storage_used_gib} GiB</code> | <code>{storage_total_gib} GiB</code></blockquote>

  {emoji:🍃} <b>ᴍσηɢσᴅʙ sᴛᴧᴛs:</b><blockquote>
  • sᴛσʀᴧɢє: <code>{db_data} / {db_storage} MB</code>
  • ᴄσʟʟєᴄᴛɪσηs: <code>{db_collections}</code>
  • σʙᴊєᴄᴛs: <code>{db_objects}</code></blockquote>

  {emoji:📊} <b>sєʀᴠєᴅ:</b><blockquote>
  • ᴄʜᴧᴛs: <code>{served_chats}</code>
  • ᴜsєʀs: <code>{served_users}</code></blockquote>

  {emoji:🧩} <b>ʀᴜηᴛɪᴍє ᴠєʀsɪσηs:</b><blockquote>
  • ᴄσʀє: <code>{go_version}</code>
  • ᴧᴘɪ: <code>{gogram_api_layer}</code>
  • ηᴛɢᴄᴧʟʟs ᴠєʀsɪση: <code>{ntgcalls_version}</code></blockquote>

# 👑 Sudo users

sudo_owner_self: "{emoji:😂} ʜᴧʜᴧ, ʏσᴜ’ʀє ᴛʜє ʙσss ᴧʟʀєᴧᴅʏ! ᴡʜʏ ᴧʀє ʏσᴜ єᴠєη ᴛʀʏɪηɢ ᴛσ ᴧᴅᴅ ʏσᴜʀsєʟғ?"

sudo_bot_self: "{emoji:🤖} ʜᴧʜᴧ, ɪ’ᴍ ᴛʜє ʙσᴛ! ɪ ᴄᴧη’ᴛ ᴧᴅᴅ ᴍʏsєʟғ ᴛσ ᴛʜє sᴜᴅσ ʟɪsᴛ — єᴠєη ᴛʜє σᴡηєʀ ᴄᴧη’ᴛ ᴄʜєᴧᴛ ᴍє {emoji:😎}"

sudo_bot_user: "{emoji:🤖} ʏσᴜ ᴄᴧη’ᴛ ᴧᴅᴅ ᴧ ʙσᴛ ᴛσ ᴛʜє sᴜᴅσ ʟɪsᴛ — sᴜᴅσєʀs ᴍᴜsᴛ ʙє ʜᴜᴍᴧη!"

sudo_fetch_user_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ғєᴛᴄʜ ᴜsєʀ ɪηғσ. ᴍᴧʏʙє ᴛʜє ᴜsєʀ ɪs ɪηᴧᴄᴄєssɪʙʟє.\n<i>{error}</i>"

sudo_check_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴄʜєᴄᴋ sᴜᴅσ єxɪsᴛєηᴄє: <i>{error}</i>"

sudo_already: "{emoji:⚠️} ᴜsєʀ {user} (ɪᴅ:<code>{id}</code>) ɪs ᴧʟʀєᴧᴅʏ ᴧ sᴜᴅσєʀ."

sudo_add_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴧᴅᴅ sᴜᴅσ: <i>{error}</i>"

sudo_add_success: "{emoji:✅} ᴧᴅᴅєᴅ {user} (<code>{id}</code>) ᴛσ sᴜᴅσєʀs."

sudo_owner_remove_block: "{emoji:😎} ηɪᴄє ᴛʀʏ! ʏσᴜ ᴄᴧη’ᴛ ʀєᴍσᴠє ᴛʜє σᴡηєʀ — ᴛʜєʏ’ʀє ᴜηᴛσᴜᴄʜᴧʙʟє."

sudo_not_exists: "{emoji:⚠️} ᴜsєʀ {user} (<code>{id}</code>) ɪs ησᴛ ᴧ sᴜᴅσєʀ."

sudo_remove_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ʀєᴍσᴠє sᴜᴅσ: <i>{error}</i>"

sudo_remove_success: "{emoji:🗑️} ʀєᴍσᴠєᴅ {user} (ɪᴅ: <code>{id}</code>) ғʀσᴍ sᴜᴅσєʀs."

sudo_list_fetching: "{emoji:⏳} ғєᴛᴄʜɪηɢ sᴜᴅσєʀs ʟɪsᴛ..."

sudo_list_fetch_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ɢєᴛ sᴜᴅσєʀs: <code>{error}</code>"

sudo_list_header: "{emoji:👑} <b>ᴄᴜʀʀєηᴛ sᴜᴅσєʀs:</b>"

sudo_list_owner: "{index}. {user} — <b>σᴡηєʀ</b>"

sudo_list_entry: "{index}. {user}"

sudo_list_no_extra: "{emoji:⚠️} ησ ᴧᴅᴅɪᴛɪσηᴧʟ sᴜᴅσєʀs ғσᴜηᴅ."

seek_no_active: "{emoji:⚠️} <b>ησ ᴛʀᴧᴄᴋ ɪs ᴄᴜʀʀєηᴛʟʏ ᴘʟᴧʏɪηɢ.</b>"

seek_usage: "{emoji:⚠️} ᴘʟєᴧsє ᴘʀσᴠɪᴅє sєᴄσηᴅs.\nєxᴧᴍᴘʟє: <code>{cmd} 40</code>"

seek_invalid_seconds: "{emoji:⚠️} ɪηᴠᴧʟɪᴅ sєᴄσηᴅs ᴠᴧʟᴜє.\nєxᴧᴍᴘʟє: <code>{cmd} 40</code>"

seek_too_close_start: "{emoji:⚠️} ᴄᴧηησᴛ sєєᴋ ʙᴧᴄᴋᴡᴧʀᴅ <b>{seconds}</b> sєᴄσηᴅs — ᴛʜᴧᴛ ᴡσᴜʟᴅ ʙє ᴛσσ ᴄʟσsє ᴛσ ᴛʜє ʙєɢɪηηɪηɢ σғ ᴛʜє ᴛʀᴧᴄᴋ."

seek_too_close_end: "{emoji:⚠️} ᴄᴧηησᴛ sєєᴋ ғσʀᴡᴧʀᴅ <b>{seconds}</b> sєᴄσηᴅs — ᴛʜᴧᴛ ᴡσᴜʟᴅ ʙє ᴛσσ ᴄʟσsє ᴛσ ᴛʜє єηᴅ σғ ᴛʜє ᴛʀᴧᴄᴋ."

seek_failed: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ sєєᴋ {direction} <b>{seconds}</b> sєᴄσηᴅs.\nєʀʀσʀ: <code>{error}</code>"

seek_success: "{emoji} ᴊᴜᴍᴘєᴅ {direction} ᴛσ <b>{position}</b> σғ <b>{duration}</b>."

stop_confirm_paused: |
  {emoji:⏸️} <b>ᴘʟᴧʏʙᴧᴄᴋ ɪs ᴄᴜʀʀєηᴛʟʏ ᴘᴧᴜsєᴅ.</b>

  ᴅσ ʏσᴜ ᴡᴧηᴛ ᴛσ ʀєsᴜᴍє ᴘʟᴧʏɪηɢ σʀ sᴛσᴘ ɪᴛ ᴄσᴍᴘʟєᴛєʟʏ?

stop_confirm_muted: |
  {emoji:🔇} <b>ᴘʟᴧʏʙᴧᴄᴋ ɪs ᴄᴜʀʀєηᴛʟʏ ᴍᴜᴛєᴅ.</b>

  ᴅσ ʏσᴜ ᴡᴧηᴛ ᴛσ ᴜηᴍᴜᴛє σʀ sᴛσᴘ ɪᴛ ᴄσᴍᴘʟєᴛєʟʏ?

stopped: "{emoji:⏹️} <b>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ ᴧηᴅ ᴄʟєᴧʀєᴅ ʙʏ {user}.</b>"

shuffle_current_state: "{emoji:🔀} ᴄᴜʀʀєηᴛʟʏ sʜᴜғғʟє ɪs <b>{state}</b> ғσʀ ᴛʜɪs ᴄʜᴧᴛ.\n\nᴜsє <code>{cmd}</code> ᴛσ ᴛσɢɢʟє ɪᴛ."

shuffle_updated: "{emoji:🔀} sʜᴜғғʟє <b>{state}</b> ʙʏ {user}."

skip_stopped: "{emoji:⏹️} ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ. ǫᴜєᴜє ɪs єᴍᴘᴛʏ.\nsᴋɪᴘᴘєᴅ ʙʏ: {user}"

skip_invalid_number: "{emoji:⚠️} ᴘʟєᴧsє ᴘʀσᴠɪᴅє ᴧ ᴠᴧʟɪᴅ ηᴜᴍʙєʀ ᴧғᴛєʀ /skip.\nєxᴧᴍᴘʟє: <code>/skip 3</code>"

skip_queue_empty_for_count: "{emoji:⚠️} ǫᴜєᴜє ʜᴧs ησ ᴜᴘᴄσᴍɪηɢ ᴛʀᴧᴄᴋs ᴛσ sᴋɪᴘ."

skip_count_exceeds_queue: "{emoji:⚠️} ʏσᴜ ʀєǫᴜєsᴛєᴅ ᴛσ sᴋɪᴘ <code>{requested}</code> ǫᴜєᴜєᴅ ᴛʀᴧᴄᴋs, ʙᴜᴛ σηʟʏ <code>{available}</code> ᴛʀᴧᴄᴋ(s) ᴧʀє ɪη ǫᴜєᴜє."

speed_usage: |
  {emoji:💡} <b>ᴜsᴧɢє:</b>
  <code>{cmd} [speed]</code> — ᴄʜᴧηɢє ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ
  
  ᴧʟʟσᴡєᴅ sᴘєєᴅ ʀᴧηɢє: <b>0.50x → 4.00x</b>
  
  єxᴧᴍᴘʟє:
  <code>{cmd} 1.5</code> — ᴘʟᴧʏ ᴧᴛ 1.5x
  <code>{cmd} normal</code> — ʀєsєᴛ ᴛσ ησʀᴍᴧʟ sᴘєєᴅ

speed_current: "{emoji:🎧} <b>ᴄᴜʀʀєηᴛ sᴘєєᴅ:</b> <code>{speed}x</code> ғσʀ {emoji:🎵} <u>{title}</u>\n\n{emoji:💡} ᴜsє <code>{cmd} reset</code> ᴛσ ʀєsᴛσʀє ησʀᴍᴧʟ ᴘʟᴧʏʙᴧᴄᴋ."

speed_invalid_value: "{emoji:❌} ɪηᴠᴧʟɪᴅ sᴘєєᴅ ᴠᴧʟᴜє.\nєxᴧᴍᴘʟє: <code>{cmd} 1.5</code> σʀ <code>{cmd} 2x</code>"

speed_invalid_range: "{emoji:⚠️} sᴘєєᴅ ᴍᴜsᴛ ʙє ʙєᴛᴡєєη <b>0.50x</b> ᴧηᴅ <b>4.00x</b>."

speed_already_set: "ℹ️ ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ ɪs ᴧʟʀєᴧᴅʏ <b>{speed}x</b> ғσʀ {emoji:🎵} <b>{title}</b>"

speed_failed: |
  {emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴄʜᴧηɢє sᴘєєᴅ ᴛσ <b>{speed}x</b>.
  єʀʀσʀ: <code>{error}</code>

speed_reset_success: "{emoji:✅} ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ ʀєsєᴛ ᴛσ <b>1.0x</b> ʙʏ {user}"

speed_set: "{emoji:🚀} ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ sєᴛ ᴛσ <b>{speed}x</b> ʙʏ {user}."

start_private: "<blockquote><b>sᴧʟᴜᴛᴧᴛɪσɴs</b> {user},</blockquote>\n<blockquote expandable><b>✧ ᴡᴇʟᴄσᴍᴇ ᴛσ {bot} — ᴧ ᴘσᴡᴇʀғᴜʟ ᴧɴᴅ ʜɪɢʜ-sᴘᴇᴇᴅ ᴛɢ ᴍᴜsɪᴄ ʙσᴛ</b>\n<b>✧ ʙᴜɪʟᴛ ғσʀ sᴍσσᴛʜ • sᴛᴧʙʟᴇ • ʟᴧɢ-ғʀᴇᴇ ᴍᴜsɪᴄ sᴛʀᴇᴧᴍɪɴɢ</b>\n<b>✧ ᴘσᴡᴇʀᴇᴅ ʙʏ ᴧɴ σᴘᴛɪᴍɪᴢᴇᴅ ʏσᴜᴛᴜʙᴇ ᴧᴘɪ ғσʀ ɪɴsᴛᴧɴᴛ ᴘʟᴧʏʙᴧᴄᴋ</b>\n<b>✧ ᴇɴᴊσʏ ʜɪɢʜ ǫᴜᴧʟɪᴛʏ ᴧᴜᴅɪσ ᴡɪᴛʜ sᴇᴧᴍʟᴇss ᴄσɴᴛʀσʟ</b>\n<b>•──────────────•</b>\n<b>✧ ᴜsᴇ ʜᴇʟᴘ ᴛσ ᴠɪᴇᴡ ᴧʟʟ ᴄσᴍᴍᴧɴᴅs ᴧɴᴅ ғᴇᴧᴛᴜʀᴇs</b></blockquote> "
call_discarded: "📴 ᴛʜє ᴠσɪᴄє ᴄʜᴧᴛ ᴡᴧs єηᴅєᴅ, sσ ᴘʟᴧʏʙᴧᴄᴋ ʜᴧs sᴛσᴘᴘєᴅ.\n\n💐 sᴛᴧʀᴛ ᴧ ηєᴡ ᴠσɪᴄє ᴄʜᴧᴛ ᴧηᴅ ᴜsє /play ᴧɢᴧɪη."

start_group: "<b>{emoji:💫} ɪ’ᴍ ʜєʀє!</b> ᴛʀʏ ᴧ ᴄσᴍᴍᴧηᴅ… ɪғ ʏσᴜ ᴛʜɪηᴋ ʏσᴜ ᴄᴧη ʜᴧηᴅʟє ᴍє {emoji:😉}"

stream_queue_finished: "💮 ᴛʜє ǫᴜєᴜє ʜᴧs ғɪηɪsʜєᴅ.\n\n💐 ᴜsє /play ᴛσ ᴧᴅᴅ ᴍσꝛє sσηɢs..!"

stream_downloading_next: "{emoji:📥} ᴅσᴡηʟσᴧᴅɪηɢ ʏσᴜʀ ηєxᴛ ᴛʀᴧᴄᴋ..."

stream_autoplay_next: "{emoji:🔁} ᴧᴜᴛσᴘʟᴧʏ: <b>{title}</b>"

stream_download_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴅσᴡηʟσᴧᴅ.\nєʀʀσʀ: <code>{error}</code>"

download_progress: |
  {emoji:📥} ᴅσᴡηʟσᴧᴅɪηɢ ʏσᴜʀ ᴛʀᴧᴄᴋ...

  <b>ᴘʀσɢʀєss:</b> {percentage}%
  <b>sᴘєєᴅ:</b> {speed}
  <b>єᴛᴧ:</b> {eta}
  <b>єʟᴧᴘsєᴅ:</b> {elapsed}
stream_play_fail: "{emoji:❌} ғᴧɪʟєᴅ ᴛσ ᴘʟᴧʏ ᴛʜє sσηɢ."

stream_now_playing: |
  <blockquote><b>{emoji:💮} ᴘʟᴧʏʙᴧᴄᴋ ᴧᴄᴛɪᴠᴧᴛєᴅ. | єηᴊσʏ ᴛʜє ᴍᴜsɪᴄ |</b></blockquote>
  <blockquote expandable>▫ <b>ᴍєʟσᴅʏ :</b> {title}
  ▫ <b>ʟєηɢᴛʜ :</b> {duration}
  ▫ <b>ʀєǫᴜєsᴛєʀ :</b> {by}</blockquote>

# 👑 Sudo / Owner Join Messages
sudo_join_owner: "{emoji:👑} {user} — σᴡηєʀ σғ {bot} — ᴊᴜsᴛ єηᴛєʀєᴅ ᴛʜє ᴄʜᴧᴛ."

sudo_join_sudo: "{emoji:🛡️} {user}, ᴧ sᴜᴅσ ᴜsєʀ σғ {bot}, ᴊᴜsᴛ ᴊσɪηєᴅ ᴛʜє ᴄʜᴧᴛ."

supergroup_needed: |
  ᴛʜɪs ᴄʜᴧᴛ (ɪᴅ: <code>{chat_id}</code>) ɪs ησᴛ ᴧ sᴜᴘєʀɢʀσᴜᴘ ʏєᴛ.

  <i>{emoji:⚠️} ᴘʟєᴧsє ᴄσηᴠєʀᴛ ᴛʜɪs ɢʀσᴜᴘ ɪηᴛσ ᴧ sᴜᴘєʀɢʀσᴜᴘ, ᴛʜєη ᴧᴅᴅ ᴍє ᴧɢᴧɪη ᴧs ᴧη ᴧᴅᴍɪη.</i>

  <b>ʜσᴡ ᴛσ ᴄσηᴠєʀᴛ?</b>
  <a href="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">{emoji:🔗} ɢσ ᴛσ ᴛʜє ɢᴜɪᴅє</a>

  ɪғ ʏσᴜ sᴛɪʟʟ ғᴧᴄє ᴧηʏ ɪssᴜєs, ᴊσɪη σᴜʀ <a href="{support_group}">sᴜᴘᴘσʀᴛ ɢʀσᴜᴘ.</a>

unmute_already: "ℹ️ <b>ᴧʟʀєᴧᴅʏ ᴜηᴍᴜᴛєᴅ</b>\nᴛʜє ᴍᴜsɪᴄ ɪs ησᴛ ᴍᴜᴛєᴅ ɪη ᴛʜɪs ᴄʜᴧᴛ."

unmute_failed: "❌ <b>ᴘʟᴧʏʙᴧᴄᴋ ᴜηᴍᴜᴛє ғᴧɪʟєᴅ</b>\nєʀʀσʀ: <code>{error}</code>"

unmute_success: |
  {emoji:🔊} <b>ᴜηᴍᴜᴛєᴅ ᴘʟᴧʏʙᴧᴄᴋ</b>

  {emoji:🎵} ᴛʀᴧᴄᴋ: {title}
  {emoji:👤} ᴜηᴍᴜᴛєᴅ ʙʏ: {user}
  {speed_line}

# 🔊 Voice Chat Events
voicechat_ended: "<b>{emoji:⚠️} ᴠɪᴅєσ ᴄʜᴧᴛ єηᴅєᴅ</b>\n\n<i>{emoji:⏰} ᴅᴜʀᴧᴛɪση: {duration}</i>"

voicechat_started: "<b>{emoji:♻️} ᴠσɪᴄє ᴄʜᴧᴛ sᴛᴧʀᴛєᴅ!</b>\n\n<i>ᴜsє /play [sσηɢ ηᴧᴍє] ᴛσ ᴘʟᴧʏ ᴍᴜsɪᴄ.</i>"


 # Playlist commands
playlist_usage_create: "<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ηᴧᴍє]"
playlist_limit_reached: "⚠️ ʏσᴜ ʜᴧᴠє ʀєᴧᴄʜєᴅ ᴛʜє ᴍᴧxɪᴍᴜᴍ ʟɪᴍɪᴛ σғ 10 ᴘʟᴧʏʟɪsᴛs."
playlist_fetch_fail: "❌ ᴜηᴧʙʟє ᴛσ ғєᴛᴄʜ ʏσᴜʀ ᴘʟᴧʏʟɪsᴛs. ᴘʟєᴧsє ᴛʀʏ ᴧɢᴧɪη ʟᴧᴛєʀ."
playlist_create_fail: "❌ ғᴧɪʟєᴅ ᴛσ ᴄʀєᴧᴛє ᴘʟᴧʏʟɪsᴛ: {error}"
playlist_create_success: "✅ ᴘʟᴧʏʟɪsᴛ <b>{name}</b> ʜᴧs ʙєєη ᴄʀєᴧᴛєᴅ sᴜᴄᴄєssғᴜʟʟʏ.\nɪᴅ: <code>{id}</code>"
playlist_usage_delete: "<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ]"
playlist_not_found: "❌ ᴛʜє sᴘєᴄɪғɪєᴅ ᴘʟᴧʏʟɪsᴛ ᴄσᴜʟᴅ ησᴛ ʙє ғσᴜηᴅ. ᴘʟєᴧsє ᴄʜєᴄᴋ ᴛʜє ᴘʟᴧʏʟɪsᴛ ɪᴅ."
playlist_not_owner: "⚠️ ʏσᴜ ᴄᴧη σηʟʏ ᴍᴧηᴧɢє ᴘʟᴧʏʟɪsᴛs ᴛʜᴧᴛ ʏσᴜ ᴄʀєᴧᴛєᴅ."
playlist_delete_fail: "❌ ғᴧɪʟєᴅ ᴛσ ᴅєʟєᴛє ᴛʜє ᴘʟᴧʏʟɪsᴛ: {error}"
playlist_delete_success: "🗑️ ᴘʟᴧʏʟɪsᴛ <b>{name}</b> ʜᴧs ʙєєη ᴅєʟєᴛєᴅ sᴜᴄᴄєssғᴜʟʟʏ."
playlist_usage_add: "<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ] [sσηɢ ηᴧᴍє σʀ ᴜʀʟ]"
playlist_invalid_query: "❌ ησ ᴘʟᴧʏᴧʙʟє ᴛʀᴧᴄᴋ ᴡᴧs ғσᴜηᴅ ғσʀ ᴛʜᴧᴛ ǫᴜєʀʏ."
playlist_add_fail: "❌ ғᴧɪʟєᴅ ᴛσ ᴧᴅᴅ ᴛʜє ᴛʀᴧᴄᴋ ᴛσ ᴛʜє ᴘʟᴧʏʟɪsᴛ: {error}"
playlist_add_success: "✅ ᴛʀᴧᴄᴋ <b>{song}</b> ʜᴧs ʙєєη ᴧᴅᴅєᴅ ᴛσ ᴘʟᴧʏʟɪsᴛ <b>{playlist}</b>."
playlist_usage_remove: "<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ] [sσηɢ ηᴜᴍʙєʀ σʀ ᴜʀʟ]"
playlist_invalid_song_number: "⚠️ ɪηᴠᴧʟɪᴅ sσηɢ ηᴜᴍʙєʀ."
playlist_song_not_found: "❌ sσηɢ ησᴛ ғσᴜηᴅ ɪη ᴘʟᴧʏʟɪsᴛ."
playlist_remove_fail: "❌ єʀʀσʀ ʀєᴍσᴠɪηɢ sσηɢ: {error}"
playlist_remove_success: "🗑️ sσηɢ ʀєᴍσᴠєᴅ ғʀσᴍ ᴘʟᴧʏʟɪsᴛ <b>{name}</b>."
playlist_usage_info: "<b>ᴜsᴧɢє:</b> {cmd} [ᴘʟᴧʏʟɪsᴛ ɪᴅ]"
playlist_songs_empty: "<i>ᴛʜɪs ᴘʟᴧʏʟɪsᴛ ʜᴧs ησ sσηɢs ʏєᴛ.</i>"
playlist_song_line: "{index}. {name} ({url})"
playlist_info: "<b>ᴘʟᴧʏʟɪsᴛ ɪηғσ</b>\n\n<b>ηᴧᴍє:</b> {name}\n<b>σᴡηєʀ:</b> {owner}\n<b>sσηɢs:</b> {count}\n\n{songs}"
playlist_none: "📭 ʏσᴜ ᴅσ ησᴛ ʜᴧᴠє ᴧηʏ ᴘʟᴧʏʟɪsᴛs ʏєᴛ. ᴜsє /createplaylist ᴛσ ᴍᴧᴋє σηє."
playlist_list_line: "- {name} (<code>{id}</code>)"
playlist_list: "<b>ᴍʏ ᴘʟᴧʏʟɪsᴛs</b>\n\n{playlists}"

# Commands help
help_admin: "⊚ <b><u>ᴧᴅᴍɪη ᴄσᴍᴍᴧηᴅs :</u></b>\n\n<blockquote>💮 <b>ᴊᴜsᴛ ᴧᴅᴅ ᴄ ᴧs ᴧ ᴘʀєғɪx ᴛσ ᴛʜє ᴄσᴍᴍᴧηᴅs ᴛσ ᴄσηᴛʀσʟ ᴄʜᴧηηєʟ sᴛʀєᴧᴍɪηɢ.</b></blockquote>\n\n<blockquote>➻ <b>/pause</b> : ʜᴧʟᴛ ᴛʜє ᴄᴜʀʀєηᴛʟʏ ᴧᴄᴛɪᴠє sᴛʀєᴧᴍ.\n➻ <b>/resume</b> : ʀєsᴛᴧʀᴛ ᴛʜє ᴘᴧᴜsєᴅ ᴘʟᴧʏʙᴧᴄᴋ.\n➻ <b>/skip</b> : ᴘʟᴧʏ ᴛʜє ηєxᴛ ᴛʀᴧᴄᴋ ɪη ᴛʜє ǫᴜєᴜє.\n➻ <b>/askip</b> : ᴘʟᴧʏ ᴛʜє ηєxᴛ ᴛʀᴧᴄᴋ ɪη ᴛʜє ᴧᴜᴛσᴘʟᴧʏ ǫᴜєᴜє.\n➻ <b>/stop</b> : ᴄєᴧsє ᴛʜє sᴛʀєᴧᴍ ᴧηᴅ єᴍᴘᴛʏ ǫᴜєᴜє.\n➻ <b>/mute</b> : ᴍᴜᴛє ᴘʟᴧʏʙᴧᴄᴋ ɪη ᴠɪᴅєσᴄʜᴧᴛ.\n➻ <b>/unmute</b> : ᴜηᴍᴜᴛє ᴘʟᴧʏʙᴧᴄᴋ ɪη ᴠɪᴅєσᴄʜᴧᴛ.\n➻ <b>/queue</b> : ᴠɪєᴡ ᴛʜє ʟɪsᴛ σғ ᴜᴘᴄσᴍɪηɢ ᴛʀᴧᴄᴋs.\n➻ <b>/jump</b> : ᴊᴜᴍᴘ ᴛσ ᴧ ɢɪᴠєη ᴛɪᴍє ɪη ᴛʀᴧᴄᴋ.\n➻ <b>/move</b> : ᴍσᴠє ᴧ ǫᴜєᴜєᴅ ᴛʀᴧᴄᴋ ᴛσ ᴧησᴛʜєʀ ᴘσsɪᴛɪση.\n➻ <b>/clear</b> : ᴄʟєᴧʀ ᴧʟʟ sσηɢs ғʀσᴍ ǫᴜєᴜє.\n➻ <b>/remove</b> : ʀєᴍσᴠє ᴧ sᴘєᴄɪғɪᴄ ᴛʀᴧᴄᴋ ғʀσᴍ ǫᴜєᴜє.\n➻ <b>/loop [1-10]</b> : ʀєᴘєᴧᴛ ᴛʜє ᴄᴜʀʀєηᴛʟʏ ᴘʟᴧʏɪηɢ sσηɢ.\n➻ <b>/shuffle</b> : ʀᴧηᴅσᴍɪᴢє ᴛʜє ǫᴜєᴜєᴅ ᴘʟᴧʏʟɪsᴛ.\n➻ <b>/seek [sєᴄs]</b> : ғσʀᴡᴧʀᴅ sєєᴋ ᴛσ ᴧ sᴘєᴄɪғɪᴄ ᴛɪᴍє.\n➻ <b>/seekback [sєᴄs]</b> : ʀєᴠєʀsє sєєᴋ ᴛσ ᴧ sᴘєᴄɪғɪᴄ ᴛɪᴍє.\n➻ <b>/speed</b> : ᴍσᴅɪғʏ ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ (0.5x - 2.0x).\n➻ <b>/playmode</b> : ᴄσηᴛʀσʟ ᴡʜσ ᴄᴧη ᴜsє /play ᴄᴍᴅ.\n➻ <b>/cmddelete</b> : ᴛσɢɢʟє ᴧᴜᴛσᴍᴧᴛɪᴄ ᴅєʟєᴛɪση σғ ʙσᴛ ᴄσᴍᴍᴧηᴅs.</blockquote>\n"

help_auth: "⊚ <b><u>ᴧᴜᴛʜ ᴜsєʀs :</u></b>\n\n<blockquote>💮 <b>ᴧᴜᴛʜ ᴜsєʀs ᴧʀє ɢʀᴧηᴛєᴅ ᴧᴅᴍɪη ᴘʀɪᴠɪʟєɢєs ᴡɪᴛʜσᴜᴛ ʀєǫᴜɪʀɪηɢ ɢʀσᴜᴘ ᴧᴅᴍɪη sᴛᴧᴛᴜs ᴛσ ᴍᴧηᴧɢє sᴛʀєᴧᴍs.</b></blockquote>\n\n<blockquote>➻ <b>/auth [ᴜsєʀ]</b> : ᴧᴅᴅ ᴜsєʀ ᴛσ ᴧᴜᴛʜσʀɪᴢєᴅ ʟɪsᴛ.\n➻ <b>/unauth [ᴜsєʀ]</b> : ʀєᴍσᴠє ᴜsєʀ ғʀσᴍ ᴧᴜᴛʜσʀɪᴢєᴅ ʟɪsᴛ.\n➻ <b>/authlist</b> : ᴅɪsᴘʟᴧʏ ᴧʟʟ ᴧᴜᴛʜσʀɪᴢєᴅ ᴍєᴍʙєʀs.</blockquote>\n"

help_bcast: "⊚ <b><u>ʙʀσᴧᴅᴄᴧsᴛ ғєᴧᴛᴜʀє [σηʟʏ ғσʀ sᴜᴅσєʀs] :</u></b>\n\n<blockquote>➻ <b>/broadcast [ᴍєssᴧɢє σʀ ʀєᴘʟʏ]</b> : sєηᴅ ᴛσ ᴧʟʟ ᴄʜᴧᴛs.\n\n➻ <b>ᴍσᴅєs</b> :\n<b>-pin</b> : ᴘɪη ᴛʜє ᴍєssᴧɢє\n<b>-pinloud</b> : ᴘɪη + ησᴛɪғʏ ᴍєᴍʙєʀs\n<b>-user</b> : ʙʀσᴧᴅᴄᴧsᴛ ᴛσ sᴛᴧʀᴛєᴅ ᴜsєʀs\n<b>-assistant</b> : sєηᴅ ᴠɪᴧ ᴧssɪsᴛᴧηᴛ\n<b>-nobot</b> : ᴅση'ᴛ sєηᴅ ғʀσᴍ ʙσᴛ\n\n⊚ єxᴧᴍᴘʟє : /broadcast -user -pin ᴛєsᴛɪηɢ ʙʀσᴧᴅᴄᴧsᴛ</blockquote>\n"

help_play: "⊚ <b><u>ᴘʟᴧʏ & ᴄʜᴧηηєʟ ᴘʟᴧʏ :</u></b>\n\n<blockquote>➻ <b>/play</b> : sᴛᴧʀᴛ ᴧᴜᴅɪσ sᴛʀєᴧᴍɪηɢ ɪη ᴄʜᴧᴛ.\n➻ <b>/vplay</b> : sᴛᴧʀᴛ ᴠɪᴅєσ sᴛʀєᴧᴍɪηɢ ɪη ᴄʜᴧᴛ.\n➻ <b>/playforce</b> : ɪηsᴛᴧηᴛ ᴘʟᴧʏ (σᴠєʀʀɪᴅєs ǫᴜєᴜє).</blockquote>\n\n⊚ <b><u>ᴄʜᴧηηєʟ ᴘʟᴧʏ :</u></b>\n\n<blockquote>➻ <b>/cplay</b> : ᴧᴜᴅɪσ sᴛʀєᴧᴍ ɪη ʟɪηᴋєᴅ ᴄʜᴧηηєʟ.\n➻ <b>/cvplay</b> : ᴠɪᴅєσ sᴛʀєᴧᴍ ɪη ʟɪηᴋєᴅ ᴄʜᴧηηєʟ.\n➻ <b>/cplayforce</b> : ғσʀᴄє sᴛʀєᴧᴍ ɪη ʟɪηᴋєᴅ ᴄʜᴧηηєʟ.\n➻ <b>/channelplay [ɪᴅ]</b> : ᴄσηηєᴄᴛ ᴄʜᴧηηєʟ ᴛσ ᴛʜɪs ɢʀσᴜᴘ.</blockquote>\n"

help_sudo: "⊚ <b><u>sʏsᴛєᴍ & sᴜᴅσ ᴍᴧηᴧɢєᴍєηᴛ :</u></b>\n\n<blockquote>💮 <b>ᴍᴧηᴧɢє ʟσɢs, ᴧηᴅ ᴧᴅᴍɪηɪsᴛʀᴧᴛɪᴠє ᴘʀɪᴠɪʟєɢєs σғ ʏσᴜʀ ʙσᴛ.</b></blockquote>\n\n<blockquote>➻ <b>ʙσᴛ ʟσɢs & ᴍᴧɪηᴛєηᴧηᴄє</b> :\n➻ <b>/logs</b> : ɢєᴛ ʀєᴧʟ-ᴛɪᴍє ʟσɢs σғ ᴛʜє ʙσᴛ.\n➻ <b>/logger</b> : [єηᴧʙʟє | ᴅɪsᴧʙʟє] ʙσᴛ ᴧᴄᴛɪᴠɪᴛʏ ʟσɢɢɪηɢ.\n➻ <b>/maintenance</b> : [єηᴧʙʟє | ᴅɪsᴧʙʟє] ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє.\n➻ <b>/eval</b> : єxєᴄᴜᴛє ɢσ ᴄσᴅє sηɪᴘᴘєᴛs.\n➻ <b>/sh</b> : ʀᴜη sʜєʟʟ ᴄσᴍᴍᴧηᴅs.</blockquote>\n\n⊚ <b><u>ᴧᴄᴛɪᴠє sᴛʀєᴧᴍs :</u></b>\n\n<blockquote>➻ <b>/activevoice</b> : ᴠɪєᴡ σηɢσɪηɢ ᴧᴜᴅɪσ sєssɪσηs.\n➻ <b>/ac</b> : ᴠɪєᴡ ᴧʟʟ ᴄσᴍʙɪηєᴅ ᴧᴄᴛɪᴠє sᴛʀєᴧᴍs.</blockquote>\n\n<blockquote>➻ <b>sᴜᴅσ ʟɪsᴛ ᴍᴧηᴧɢєᴍєηᴛ</b> :\n➻ <b>/addsudo</b> : ᴧᴅᴅ ᴧ ηєᴡ sᴜᴅσ ᴜsєʀ ᴛσ ʙσᴛ.\n➻ <b>/rmsudo</b> : ʀєᴍσᴠє ᴧ ᴜsєʀ ғʀσᴍ sᴜᴅσ ʟɪsᴛ.\n➻ <b>/sudolist</b> : ᴄʜєᴄᴋ ᴧʟʟ ᴧᴜᴛʜσʀɪᴢєᴅ sᴜᴅσ ᴜsєʀs.\n\n❏ ᴛʜєsє ᴄσᴍᴍᴧηᴅs ᴧʀє ʜɪɢʜʟʏ sєηsɪᴛɪᴠє, ᴜsє ᴡɪᴛʜ ᴄᴧʀє.</blockquote>\n"

help_restrict: "⊚ <b><u>ʙʟᴧᴄᴋʟɪsᴛ & ɢʟσʙᴧʟ ʙᴧη :</u></b>\n\n<blockquote>💮 <b>ʀєsᴛʀɪᴄᴛ sᴘєᴄɪғɪᴄ ᴄʜᴧᴛs σʀ ᴜsєʀs ғʀσᴍ ᴜsɪηɢ ᴛʜє ʙσᴛ ɢʟσʙᴧʟʟʏ.</b></blockquote>\n\n<blockquote>➻ <b>ᴄʜᴧᴛ ʙʟᴧᴄᴋʟɪsᴛ</b> :\n➻ <b>/blacklistchat [ᴄʜᴧᴛ ɪᴅ]</b> : ʙᴧη ᴧ ᴄʜᴧᴛ ғʀσᴍ ᴜsɪηɢ ʙσᴛ.\n➻ <b>/unblockchat [ᴄʜᴧᴛ ɪᴅ]</b> : ᴜηʙᴧη ᴧ ʙʟᴧᴄᴋʟɪsᴛєᴅ ᴄʜᴧᴛ.\n➻ <b>/blacklisted</b> : sʜσᴡ ᴧʟʟ ʙʟᴧᴄᴋʟɪsᴛєᴅ ɢʀσᴜᴘs & ᴜsєʀs.</blockquote>\n\n<blockquote>➻ <b>ᴜsєʀ ʙʟσᴄᴋ</b> :\n➻ <b>/blockuser [ᴜsєʀηᴧᴍє | ɪᴅ]</b> : sᴛσᴘ ᴜsєʀ ғʀσᴍ ᴜsɪηɢ ʙσᴛ.\n➻ <b>/unblockuser [ᴜsєʀηᴧᴍє | ɪᴅ]</b> : ᴧʟʟσᴡ ᴛʜє ʙʟσᴄᴋєᴅ ᴜsєʀ.\n➻ <b>/blacklisted</b> : ᴠɪєᴡ ᴛʜє ʟɪsᴛ σғ ᴧʟʟ ʙʟσᴄᴋєᴅ ᴜsєʀs.</blockquote>\n"

help_thumb: "⊚ <b><u>ᴛʜᴜᴍʙηᴧɪʟ sєᴛᴛɪηɢs :</u></b>\n\n<blockquote>➻ <b>/nothumb</b> : ᴍᴧηᴧɢє sᴛʀєᴧᴍ ᴠɪsᴜᴧʟ sєᴛᴛɪηɢs.\n\n💮 <b>ᴛσɢɢʟє ᴧʟʙᴜᴍ ᴧʀᴛ ᴅɪsᴘʟᴧʏ. ᴅɪsᴧʙʟɪηɢ ɪᴛ ʀєᴅᴜᴄєs ᴅᴧᴛᴧ ʟσᴧᴅ ᴧηᴅ ᴍᴧɪηᴛᴧɪηs ᴧ ᴍɪηɪᴍᴧʟɪsᴛ ᴄʜᴧᴛ ʟσσᴋ.</b>\n\n➻ <b>σᴘᴛɪσηs</b> :\n• <b>єηᴧʙʟє</b> : ᴅɪsᴘʟᴧʏ ᴧʀᴛᴡσʀᴋ ᴡɪᴛʜ sᴛʀєᴧᴍ ɪηғσ.\n• <b>ᴅɪsᴧʙʟє</b> : sєηᴅ ᴘᴜʀє ᴛєxᴛ-ʙᴧsєᴅ ᴜᴘᴅᴧᴛєs.\n• <b>ᴄʟσsє</b> : ᴅɪsᴍɪss ᴛʜє ᴄσηғɪɢᴜʀᴧᴛɪση.\n\n❏ sєᴛᴛɪηɢs ᴧʀє sᴧᴠєᴅ ɪηᴅɪᴠɪᴅᴜᴧʟʟʏ ᴘєʀ ᴄʜᴧᴛ.</blockquote>\n"

help_start: "⊚ <b><u>ʙᴧsɪᴄ ᴄσᴍᴍᴧηᴅs :</u></b>\n\n<blockquote>➻ <b>/start</b> : ɪηɪᴛɪᴧʟɪᴢє ᴛʜє ᴍᴜsɪᴄ sєʀᴠɪᴄє.\n➻ <b>/help</b> : ᴧᴄᴄєss ᴛʜє ᴄσᴍᴍᴧηᴅ ɢᴜɪᴅєʟɪηєs.\n➻ <b>/ping</b> : ᴍєᴧsᴜʀє sʏsᴛєᴍ ʟᴧᴛєηᴄʏ ᴧηᴅ ᴘɪηɢ.\n➻ <b>/repo</b> : ɢєᴛ ᴛʜє sσᴜʀᴄє ᴄσᴅє ʀєᴘσ.\n➻ <b>/position</b> : sʜσᴡ ᴄᴜʀʀєηᴛ ᴛʀᴧᴄᴋ's ᴛɪᴍєsᴛᴧᴍᴘ.\n➻ <b>/reload</b> : ʀєғʀєsʜ ᴧᴅᴍɪη ᴅᴧᴛᴧ ᴄᴧᴄʜє.\n➻ <b>/settings</b> : ᴧᴅᴊᴜsᴛ ᴄσηғɪɢᴜʀᴧᴛɪση σᴘᴛɪσηs.\n➻ <b>/lang</b> : sєʟєᴄᴛ ʏσᴜʀ ᴘʀєғєʀʀєᴅ ʟᴧηɢᴜᴧɢє.\n➻ <b>/bug</b> : ʀєᴘσʀᴛ ᴧη ɪssᴜє σʀ ᴘʀσʙʟєᴍ.\n➻ <b>/json</b> : sʜσᴡ ᴍєssᴧɢє ᴊsση sᴛʀᴜᴄᴛᴜʀє.\n➻ <b>/sudolist</b> : ᴠɪєᴡ ᴛʜє ʟɪsᴛ σғ sᴜᴅσ ᴧᴅᴍɪηs.\n➻ <b>/stats</b> : ᴠɪєᴡ ᴄσᴍᴘʀєʜєηsɪᴠє ʙσᴛ ᴍєᴛʀɪᴄs.</blockquote>\n"

help_autoplay: "⊚ <b><u>ᴧᴜᴛσᴘʟᴧʏ ᴄσηᴛʀσʟ :</u></b>\n\n<blockquote>➻ <b>/autoplay</b> : ᴛσɢɢʟє ᴛʜє ᴧᴜᴛσᴘʟᴧʏ sєᴛᴛɪηɢs ᴘᴧηєʟ.\n➻ <b>/skip</b> : ᴘʟᴧʏ ᴛʜє ηєxᴛ ᴛʀᴧᴄᴋ ɪη ᴛʜє ᴧᴜᴛσᴘʟᴧʏ ǫᴜєᴜє.</blockquote>\n\n⊚ <b><u>ᴡʜᴧᴛ ɪs ᴧᴜᴛσᴘʟᴧʏ?</u></b>\n\n<blockquote>💮 <b>ᴧᴜᴛσᴘʟᴧʏ sєᴧᴍʟєssʟʏ ᴧᴅᴅs ʀєʟᴧᴛєᴅ ᴛʀᴧᴄᴋs ᴛσ ᴛʜє ǫᴜєᴜє ʙᴧsєᴅ ση ʏσᴜʀ ᴄᴜʀʀєηᴛ ᴘʟᴧʏʟɪsᴛ ғσʀ ηση-sᴛσᴘ ᴍᴜsɪᴄ.</b>\n\n➻ <b>ᴘᴧηєʟ σᴘᴛɪσηs</b> :\n• <b>єηᴧʙʟє</b> : ᴧᴄᴛɪᴠᴧᴛє sᴍᴧʀᴛ ʀєᴄσᴍᴍєηᴅᴧᴛɪσηs.\n• <b>ᴅɪsᴧʙʟє</b> : ᴅєᴧᴄᴛɪᴠᴧᴛє sᴍᴧʀᴛ ʀєᴄσᴍᴍєηᴅᴧᴛɪσηs.\n• <b>ᴄʟσsє</b> : єxɪᴛ ᴛʜє ᴄσηᴛʀσʟ ᴘᴧηєʟ.\n\n➻ <b>ησᴛє</b> : ʀєǫᴜɪʀєs ᴧᴛ ʟєᴧsᴛ σηє ᴧᴄᴛɪᴠє sσηɢ ᴛσ ғᴜηᴄᴛɪση.</blockquote>\n"

help_playlist: "⊚ <b><u>ᴘʟᴧʏʟɪsᴛ ᴄσᴍᴍᴧηᴅs :</u></b>\n\n<blockquote>💮 <b>ᴄʀєᴧᴛє, σʀɢᴧηɪᴢє, ᴧηᴅ ᴍᴧηᴧɢє ʏσᴜʀ ᴘєʀsσηᴧʟ ᴘʟᴧʏʟɪsᴛs.</b></blockquote>\n\n<blockquote>➻ <b>/createplaylist</b> : ᴄʀєᴧᴛє ᴧ ηєᴡ ᴘʟᴧʏʟɪsᴛ.\n➻ <b>/deleteplaylist</b> : ᴅєʟєᴛє σηє σғ ʏσᴜʀ ᴘʟᴧʏʟɪsᴛs.\n➻ <b>/addtoplaylist</b> : ᴧᴅᴅ ᴧ ᴛʀᴧᴄᴋ ᴛσ ᴧ ᴘʟᴧʏʟɪsᴛ.\n➻ <b>/removefromplaylist</b> : ʀєᴍσᴠє ᴧ ᴛʀᴧᴄᴋ ғʀσᴍ ᴧ ᴘʟᴧʏʟɪsᴛ.\n➻ <b>/playlistinfo</b> : ᴅɪsᴘʟᴧʏ ɪηғσʀᴍᴧᴛɪση ᴧʙσᴜᴛ ᴧ ᴘʟᴧʏʟɪsᴛ.\n➻ <b>/myplaylists</b> : ʟɪsᴛ ᴧʟʟ σғ ʏσᴜʀ ᴘʟᴧʏʟɪsᴛs.</blockquote>\n"

help_vclogs: "⊚ <b><u>ᴠᴄ ʟσɢɢєʀ ᴍσᴅᴜʟє :</u></b>\n\n<blockquote>➻ <b>/vclogger [ση|σғғ]</b> : ᴛσɢɢʟє ᴊσɪη / ʟєᴧᴠє ᴧʟєʀᴛs.\n➻ <b>/vcstatus</b> : ᴠєʀɪғʏ ᴛʜє ᴄᴜʀʀєηᴛ ʟσɢɢєʀ sᴛᴧᴛє.\n\n• ᴛʀᴧᴄᴋs ᴜsєʀ ɪᴅs ᴅᴜʀɪηɢ ᴠᴄ ᴧᴄᴛɪᴠɪᴛʏ.\n• ᴧʟєʀᴛs ᴧʀє ᴄʟєᴧηєᴅ ᴧᴜᴛσᴍᴧᴛɪᴄᴧʟʟʏ ᴧғᴛєʀ 𝟓 sєᴄ.</blockquote>\n"

help_inline: "⊚ <b><u>ʙσᴛ ᴘʟᴧʏ ʙᴜᴛᴛση ᴅєᴛᴧɪʟs :</u></b>\n\n<blockquote>💮 <b>ᴍᴧηᴧɢє ʏσᴜʀ ᴍᴜsɪᴄ ᴘʟᴧʏʙᴧᴄᴋ ᴅɪʀєᴄᴛʟʏ ᴛʜʀσᴜɢʜ ɪηʟɪηє ʙᴜᴛᴛσηs ᴡɪᴛʜ єᴧsє.</b></blockquote>\n\n<blockquote>➻ <b>ᴘʟᴧʏʙᴧᴄᴋ ᴄσηᴛʀσʟs</b> :\n➻ <b>▷</b> : ʀєsᴜᴍє ᴛʜє ᴘᴧᴜsєᴅ sᴛʀєᴧᴍ.\n➻ <b>ɪɪ</b> : ᴘᴧᴜsє ᴛʜє σηɢσɪηɢ sᴛʀєᴧᴍ.\n➻ <b>↻</b> : ʀєᴘʟᴧʏ ᴛʜє ᴄᴜʀʀєηᴛ sσηɢ ғʀσᴍ sᴛᴧʀᴛ.\n➻ <b>‣‣ɪ</b> : sᴋɪᴘ ᴛʜє ᴄᴜʀʀєηᴛ ᴛʀᴧᴄᴋ ᴧηᴅ ᴘʟᴧʏ ηєxᴛ.\n➻ <b>▢</b> : sᴛσᴘ ᴛʜє ᴘʟᴧʏʙᴧᴄᴋ ᴧηᴅ ᴄʟєᴧʀ ǫᴜєᴜє.\n\n➻ <b>sєєᴋ ᴧηᴅ ηᴧᴠɪɢᴧᴛɪση</b> :\n➻ <b>-𝟣𝟧ˢ</b> : sєєᴋ ʙᴧᴄᴋᴡᴧʀᴅ ʙʏ 15 sєᴄσηᴅs.\n➻ <b>𝟣𝟧ˢ+</b> : sєєᴋ ғσʀᴡᴧʀᴅ ʙʏ 15 sєᴄσηᴅs.\n\n❏ ᴧʟʟ ʙᴜᴛᴛσηs σηʟʏ ғσʀ ɢʀσᴜᴘ ᴧᴅᴍɪηs.</blockquote>\n"


cb_seek_failed: "❌ failed to seek.\nerror: <code>{error}</code>"

```

---

# Appendix B — remix `locales/en/messages.ftl` (VERBATIM, complete)

Source: `refs/remix/locales/en/messages.ftl` (167 lines). Fluent syntax; `{-error(t:…)}` → `❌ | <b>…</b>`, `{-ok(t:…)}` → `✅ | <b>…</b>`, `{-response(e,t)}` → `{emoji} | <b>…</b>`.

```ftl
-raw-not-in-call   = Not in call.
-raw-not-streaming = Not streaming.
-error             = ❌ | <b>{$t}</b>
-ok                = ✅ | <b>{$t}</b>
-response          = {$e} | <b>{$t}</b>
-not-in-call       = {-error(t: "Not in call.")}
-not-streaming     = {-error(t: "Not streaming.")}

not-in-call     = {-not-in-call}
not-streaming   = {-not-streaming}
raw-not-in-call = {-raw-not-in-call}

errors =
    .no-call        = {-error(t: "No active call.")}
    .no-video-found = {-error(t: "No video found.")}
    .unknown        = {-error(t: "An error occurred.")}
    .file-too-big   = {-error(t: "This file is too big.")}
    .no-assistant   = {-error(t: "My assistant is not here.")}
    .error          = {-error(t: "An error occurred:")} {$message}

inputs =
    .audio-file    = Audio File
    .custom        = Custom Input
    .voice-message = Voice Message

shuffle =
    .shuffling      = {-response(e: "🔀", t: "Shuffling...")}
    .no-enough-items = {-error(t: "No enough items to shuffle.")}

cache = {-response(e: "🗑", t: "Caches deleted.")}

now = 🎵 | Currently streaming <b><a href="{$titleUrl}">{$title}</a></b> by <b><a href="{$requesterUrl}">{$requester}</a></b>...

panel = 
    .text = {$nowEmoji} | <b><a href="{$nowUrl}">{$now}</a></b>
    ➡️ | <b><a href="{$nextUrl}">{$next}</a></b>
    .updated = Updated.
    .nothing-now   = Nothing streaming now
    .nothing-next  = Nothing streaming next

    .pause = {
        $result ->
        [true] Paused.
        [false] {-raw-not-streaming}
        *[null] {-raw-not-in-call}
    }

    .resume = {
        $result ->
        [true] Resumed.
        [false] {-raw-not-streaming}
        *[null] {-raw-not-in-call}
    }

    .skip = {
        $result ->
        [true] Skipped.
        [false] {-raw-not-streaming}
        *[null] {-raw-not-in-call}
    }

    .stop = {
        $result ->
        [true] Stopped.
        [false] {-raw-not-streaming}
        *[null] {-raw-not-in-call}
    }

    .mute = {
        $result ->
        [true] Muted.
        [false] Already muted.
        *[null] {-raw-not-in-call}
    }

    .unmute = {
        $result ->
        [true] Unmuted.
        [false] Not muted.
        *[null] {-raw-not-in-call}
    }

    .shuffling       = Shuffling...
    .volume          = Volume set to {$amount}.
    .no-enough-items = No enough items to shuffle.

playlist =
    .queuing           = 🎶 | <b>Queuing {$x} items...</b>
    .streaming-queuing = 🎶 | <b>Streaming and queuing {$x} items...</b>

volume =
    .set     = 🔈 | <b>Volume set to {$amount}.</b>
    .invalid = {-error(t: "Pass a number between 0 and 200.")}

lyrics =
    .not-found = {-error(t: "Lyrics not found.")}
    .lyrics    = <b>{$title}</b> #lyrics
    
    {$lyrics}

search =
    .canceled         = {-ok(t: "Search canceled.")},
    .no-results-found = {-error(t: "No results found.")}
    .active           = {-error(t: "There is an active search.")}
    .not_active       = {-error(t: "There is no search active.")}
    .header           = <b>🔍 | Search results for {$query}...</b>
    .no-query         = {-response(e: "❔", t: "What do you want to search for?")}
    .footer           = <i>Reply the number of the result you want to stream or /cancel.</i>
    .result           = 
        {$numberEmoji} <b><a href="{$url}">{$title}</a></b>
        {"  "}├ {$durationEmoji} {$duration}
        {"  "}├ 👁 {$views}
        {"  "}├ 📅 {$uploadTime}
        {"  "}└ 👤 {$uploader}

stream =
    .streaming = {-response(e: "▶️", t: "Streaming...")}
    .queued-at =  #️⃣ | <b>Queued at position {$position}.</b>
    .no-input = {-response(e: "❔", t: "What do you want to stream?")}

pause = {
    $result ->
    [true] {-response(e: "⏸", t: "Paused.")}
    [false] {-not-streaming}
    *[null] {-not-in-call}
}

resume = {
    $result ->
    [true] {-response(e: "▶️", t: "Resumed.")}
    [false] {-not-streaming}
    *[null] {-not-in-call}
}

skip = {
    $result ->
    [true] {-response(e: "⏩", t: "Skipped.")}
    [false] {-not-streaming}
    *[null] {-not-in-call}
}

stop = {
    $result ->
    [true] {-response(e: "⏹", t: "Stopped.")}
    [false] {-not-streaming}
    *[null] {-not-in-call}
}

mute = {
    $result ->
    [true] {-response(e: "🔇", t: "Muted.")}
    [false] {-error(t: "Already muted.")}
    *[null] {-not-in-call}
}

unmute = {
    $result ->
    [true] {-response(e: "🔈", t: "Unmuted.")}
    [false] {-error(t: "Not muted.")}
    *[null] {-not-in-call}
}

loop = {
    $result ->
    [true] {-response(e: "🔁", t: "Turned loop on.")}
    *[false] {-response(e: "🔁", t: "Turned loop off.")}
}

```

---

# Appendix C — rustify `locales/*.yml` (VERBATIM, complete, en + ru)

Source: `refs/rustify/locales/` (19 files, 1135 lines). English (`en:`) strings are the port source; Russian (`ru:`) originals are kept here for reference. Placeholders are `%{var}`.


## refs/rustify/locales/actions.yml

```yaml
_version: 2

actions.here-is-your-keyboard:
  en: |-
    Here is your keyboard ⌨️
  ru: |-
    Вот ваша клавиатура ⌨️

actions.stats:
  en: |-
    📉 <b>Some nice stats for you</b> 📈

    👎 You disliked <code>%{dislikes}</code> songs
    ⏭ Skipped <code>%{skips}</code> times
    💔 Removed <code>%{removed_collection}</code> from liked songs
    🗑 Removed <code>%{removed_playlists}</code> from playlists
    🔬 Checked lyrics <code>%{lyrics_checked}</code> times
    🔍 Analyzed lyrics <code>%{lyrics_analyzed}</code> times
    🙈 You ignored <code>%{ignored}</code> track lyrics
    🤬 <code>%{lyrics_profane}</code> lyrics were considered profane
    💩 <code>%{ai_slop_total}</code> tracks were detected as AI Slop

    <blockquote expandable><b>Languages stats:</b>
    %{languages}</blockquote>
  ru: |-
    📉 <b>Немного статистики для вас</b> 📈

    👎 Вы дизлайкнули <code>%{dislikes}</code> песен
    ⏭ Пропущено <code>%{skips}</code> раз
    💔 Удалено <code>%{removed_collection}</code> из понравившихся песен
    🗑 Удалено <code>%{removed_playlists}</code> из плейлистов
    🔬 Проверено текстов <code>%{lyrics_checked}</code> раз
    🔍 Проанализировано текстов <code>%{lyrics_analyzed}</code> раз
    🙈 Вы проигнорировали <code>%{ignored}</code> текстов треков
    🤬 <code>%{lyrics_profane}</code> текстов были признаны нецензурными
    💩 <code>%{ai_slop_total}</code> треков были определены как ИИ шлак

    <blockquote expandable><b>Статистика по языкам (названия на английском):</b>
    %{languages}</blockquote>

actions.cleanup-start:
  en: |-
    Started cleanup. Please wait, it can take a bit of time 🕐
  ru: |-
    Начата очистка. Пожалуйста, подождите, это может занять некоторое время 🕐

actions.cleanup-finish:
  en: |-
    Deleted %{removed_playlists} tracks in %{count_playlists} playlists and %{removed_collection} in favorite songs 🗑
  ru: |-
    Удалено %{removed_playlists} треков в %{count_playlists} плейлистах и %{removed_collection} из любимых песен 🗑

actions.dislike:
  en: |-
    👎 Disliked %{track_link}

    If you change your mind, press '%{ignore_button_label}'

    💡 Do not forget you can send a link to a song to find current status
  ru: |-
    👎 Дизлайкнул %{track_link}

    Если передумаете, нажмите '%{ignore_button_label}'

    💡 Не забывайте, что можете отправить ссылку на песню, чтобы узнать текущий статус

actions.ignore:
  en: |-
    🙈 Bad words of %{track_link} will be forever ignored

    If you change your mind, press '%{dislike_button_label}'

    💡 Do not forget you can send a link to a song to find current status
  ru: |-
    🙈 Плохие слова из %{track_link} будут навсегда проигнорированы

    Если передумаете, нажмите '%{dislike_button_label}'

    💡 Не забывайте, что можете отправить ссылку на песню, чтобы узнать текущий статус

```

## refs/rustify/locales/ai_slop.yml

```yaml
_version: 2

ai-slop.alert:
  en: |-
    🎵 %{track_name}
    Album: %{album_name}

    <tg-emoji emoji-id="5429334864010681294">💩</tg-emoji> This track is most likely <b>%{prediction}</b>. You can block track or ignore similar notifications for this track

    To permanently block this artist from recommendations:
    <tg-emoji emoji-id="6028171274939797252">🔗</tg-emoji> Open the artist's page in Spotify
    <tg-emoji emoji-id="6032924188828767321">➕</tg-emoji> Tap the three dots near the <i>"Follow"</i> button
    <tg-emoji emoji-id="5774077015388852135">❌</tg-emoji> Select <i>"Don't play this artist"</i>

    <i>Information provided by "%{ai_check_provider}"</i>

    <b>Support real organic hand-made music instead of AI-slop!</b>

    <blockquote>Want to disable these alerts or auto-skip AI tracks? Try /%{config_command}</blockquote>
  ru: |-
    🎵 %{track_name}
    Альбом: %{album_name}

    <tg-emoji emoji-id="5429334864010681294">💩</tg-emoji> Этот трек скорее всего <b>%{prediction}</b>. Вы можете заблокировать трек или отключить подобные уведомления для этого трека

    Чтобы навсегда скрыть этого исполнителя из рекомендаций:
    <tg-emoji emoji-id="6028171274939797252">🔗</tg-emoji> Откройте страницу исполнителя в Spotify
    <tg-emoji emoji-id="6032924188828767321">➕</tg-emoji> Нажмите на три точки рядом с кнопкой <i>«Подписаться»</i>
    <tg-emoji emoji-id="5774077015388852135">❌</tg-emoji> Выберите <i>«Не включать треки этого исполнителя»</i>

    <i>Информация предоставлена "%{ai_check_provider}"</i>

    <b>Поддерживайте настоящую живую музыку вместо ИИ-шлака!</b>

    <blockquote>Хотите отключить уведомления или настроить автопропуск AI-треков? Попробуйте /%{config_command}</blockquote>

ai-slop.alert-pure-ai:
  en: fully AI-generated
  ru: полностью сгенерирован ИИ

ai-slop.alert-processed-ai:
  en: partially generated or processed by AI
  ru: частично сгенерирован или обработан ИИ

ai-slop.setting-description:
  en: |-
    <tg-emoji emoji-id="5429334864010681294">💩</tg-emoji> AI Music Detection

    Bot automatically checks tracks for AI generation

    Configure bot's reaction to such tracks:
  ru: |-
    <tg-emoji emoji-id="5429334864010681294">💩</tg-emoji> Обнаружение ИИ-музыки

    Бот автоматически проверяет треки на ИИ-генерацию

    Настройте реакцию бота на такие треки:

ai-slop.button-notify:
  en: |-
    %{mark}Notify
  ru: |-
    %{mark}Уведомлять

ai-slop.button-ignore:
  en: |-
    %{mark}Don't notify
  ru: |-
    %{mark}Не уведомлять

ai-slop.button-skip:
  en: |-
    %{mark}Skip automatically
  ru: |-
    %{mark}Пропускать автоматически

```

## refs/rustify/locales/analyze.yml

```yaml
_version: 2

analysis.word-analyzer-prompt:
  en: |-
    You are given a word from a song: %{profane_word}.
    1. Assume by default that the word is likely to be profane.
    2. Censored profane words (with asterisks, symbols, or letter replacements) are still considered profane.
    3. Always explain what it means or how it translates into English, considering it appears in song lyrics.
    4. If the word is profane, also classify its profaneness as one of:
    - normal word
    - mildly profane
    - highly profane
    5. If the word is not profane, mark it as normal word without mentioning offensiveness.
    6. The answer must be between 50 and 150 characters.
    7. The answer must be a single line, no line breaks.
    8. Do not include the given word itself or any other offensive words.
    9. Keep it clean and suitable for all audiences.
    10. Respond with no formatting.
    11. ONLY as a final check: if the word contains special characters (apostrophes, hyphens, etc.), extra or missing letters, or looks like a fragment of a larger word - AND it doesn't match any known profane words - then it may be a false positive from the profanity detection library. Mark such cases as normal word.
  ru: |-
    Тебе дано слово из песни: %{profane_word}.
    1. По умолчанию предполагай, что слово скорее всего нецензурное.
    2. Зацензуренные нецензурные слова (со звездочками, символами или заменами букв) все еще считаются нецензурными.
    3. Всегда объясняй, что оно означает или как переводится на русский, учитывая, что оно встречается в тексте песни.
    4. Если слово нецензурное, также классифицируй его нецензурность как одну из:
    - обычное слово
    - слегка нецензурное
    - сильно нецензурное
    5. Если слово не нецензурное, отметь его как обычное слово, не упоминая оскорбительность.
    6. Ответ должен быть от 50 до 150 символов.
    7. Ответ должен быть в одну строку, без переносов.
    8. Не включай само данное слово или любые другие оскорбительные слова.
    9. Делай ответ чистым и подходящим для всех аудиторий.
    10. Отвечай без форматирования.
    11. ТОЛЬКО в качестве финальной проверки: если слово содержит специальные символы (апострофы, дефисы и т.д.), лишние или недостающие буквы, или выглядит как фрагмент более крупного слова - И при этом не совпадает ни с одним известным нецензурным словом - тогда это может быть ложное срабатывание библиотеки обнаружения нецензурной лексики. Отметь такие случаи как обычное слово.

analysis.prompt:
  en: |-
    Provide a detailed description, meaning, and storyline of the following song lyrics: "%{song_name}" and answer these questions:

    1. Does this song relate to any religion, and if so, which religion? Provide details.
    2. Does this song contain profane or explicit content or phrases? If yes, list them.
    3. Does this song include any sexual amorality, actions, or even hints? If yes, specify.
    4. Does this song reference any form of occultism or spiritism? If yes, explain.
    5. Are there any mentions of violence in this song? If yes, describe them.

    IMPORTANT: Respond in English. When quoting parts of lyrics in a foreign language, always provide English translation in parentheses immediately after the quote. Keep response strictly within 1500 characters. Respond with no formatting. Here are the lyrics:

    %{lyrics}
  ru: |-
    Предоставь подробное описание, значение и сюжетную линию следующего текста песни: "%{song_name}" и ответь на эти вопросы:

    1. Связана ли эта песня с какой-либо религией, и если да, то с какой? Предоставь подробности.
    2. Содержит ли эта песня нецензурный или откровенный контент или фразы? Если да, перечисли их.
    3. Включает ли эта песня какую-либо сексуальную аморальность, действия или даже намеки? Если да, укажи.
    4. Ссылается ли эта песня на какую-либо форму оккультизма или спиритизма? Если да, объясни.
    5. Есть ли в этой песне упоминания о насилии? Если да, опиши их.

    ВАЖНО: Отвечай на русском языке. При цитировании частей текста на иностранном языке, всегда предоставляй перевод на русский в скобках сразу после цитаты. Ответ должен быть строго в пределах 1500 символов. Отвечай без форматирования. Вот текст песни:

    %{lyrics}

analysis.disabled:
  en: >-
    ❌ Analysis is disabled
  ru: >-
    ❌ Анализ отключен

analysis.lyrics-not-found:
  en: >-
    🚫 Lyrics not found
  ru: >-
    🚫 Текст песни не найден

analysis.waiting:
  en: |-
    <tg-emoji emoji-id="5188621441926438751">🎵</tg-emoji> %{track_name}
    Album: %{album_name}

    <tg-emoji emoji-id="5451646226975955576">⏳</tg-emoji> Wait for analysis to finish <tg-emoji emoji-id="5188217332748527444">🔍</tg-emoji>
  ru: |-
    <tg-emoji emoji-id="5188621441926438751">🎵</tg-emoji> %{track_name}
    Альбом: %{album_name}

    <tg-emoji emoji-id="5451646226975955576">⏳</tg-emoji> Ждите завершения анализа <tg-emoji emoji-id="5188217332748527444">🔍</tg-emoji>

analysis.failed:
  en: |-
    🎵 %{track_name}
    Album: %{album_name}

    ❌ Analysis failed. This happens from time to time. Try again later 🤷
  ru: |-
    🎵 %{track_name}
    Альбом: %{album_name}

    ❌ Анализ не удался. Такое иногда случается. Попробуйте позже 🤷

analyze.profane-words-doesnt-fit:
  en: |-
    <blockquote expandable>🚷 <b>Profane words analysis section doesn't fit in the results</b></blockquote>
  ru: |-
    <blockquote expandable>🚷 <b>Раздел анализа нецензурных слов не помещается в результаты</b></blockquote>

analyze.profane-words:
  en: |-
    <blockquote expandable>⚠️ <b>Definitions of words that are considered profane.</b> This analysis is not precise and I would recommend manually checking the meanings of the words using Google

    %{profane_words}</blockquote>
  ru: |-
    <blockquote expandable>⚠️ <b>Определения слов, которые считаются нецензурными.</b> Этот анализ не точный и я бы рекомендовал самостоятельно проверить значения слов с помощью поисковика

    %{profane_words}</blockquote>

analysis.result:
  en: |-
    🎵 %{track_name}
    Album: %{album_name}

    <blockquote>ℹ️ You can transcribe and analyze any audio in the <a href="https://t.me/insightify_tgbot?start=ref-228571569_utm-invlink">Insightify</a> bot</blockquote>

    %{analysis_result}

    %{profane_words_block}
  ru: |-
    🎵 %{track_name}
    Альбом: %{album_name}

    <blockquote>ℹ️ Вы можете расшифровать и проанализировать любое аудио в <a href="https://t.me/insightify_tgbot?start=ref-228571569_utm-invlink">Insightify</a> боте</blockquote>

    %{analysis_result}

    %{profane_words_block}

```

## refs/rustify/locales/commands.yml

```yaml
_version: 2

command.not-found:
  en: |-
    <tg-emoji emoji-id="5348337458754894202">❓</tg-emoji> Command <code>%{command}</code> not found.

    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> There are commands available to you:
  ru: |-
    <tg-emoji emoji-id="5348337458754894202">❓</tg-emoji> Команда <code>%{command}</code> не найдена.

    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> Вам доступны следующие команды:

command.available-header:
  en: |-
    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> Commands available to you:
  ru: |-
    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> Доступные вам команды:

command.help:
  en: |-
    Show this help
  ru: |-
    Показать эту справку

command.start:
  en: |-
    Start
  ru: |-
    Старт

command.keyboard:
  en: |-
    Show keyboard
  ru: |-
    Показать клавиатуру

command.dislike:
  en: |-
    Dislike current track
  ru: |-
    Не нравится текущий трек

command.like:
  en: |-
    Like current track
  ru: |-
    Нравится текущий трек

command.details:
  en: |-
    Show details about the currently playing track
  ru: |-
    Показать детали о текущем треке

command.stats:
  en: |-
    Show statistics about tracks
  ru: |-
    Показать статистику треков

command.login:
  en: |-
    Log in to Spotify
  ru: |-
    Войти в Spotify

command.toggle-track-skip:
  en: |-
    Toggle the setting for skipping disliked tracks
  ru: |-
    Переключить настройку пропуска нелюбимых треков

command.toggle-profanity-check:
  en: |-
    Toggle the profanity check setting
  ru: |-
    Переключить настройку проверки нецензурной лексики

command.magic:
  en: |-
    Create or refresh Magic playlist
  ru: |-
    Создать или обновить Magic Playlist™

command.add-whitelist-word:
  en: |-
    Add word to whitelist
  ru: |-
    Добавить слово в белый список

command.remove-whitelist-word:
  en: |-
    Remove word from whitelist
  ru: |-
    Удалить слово из белого списка

command.list-whitelist-words:
  en: |-
    List words in whitelist
  ru: |-
    Показать слова в белом списке

command.skippage:
  en: |-
    Allows you to skip tracks you've already listened to. Pass the number of days to remember
  ru: |-
    Позволяет пропускать треки, которые вы уже слушали. Укажите количество дней для запоминания

command.language:
  en: |-
    Change language
  ru: |-
    Изменить язык

command.recommendasion:
  en: |-
    Call Recommendasion™
  ru: |-
    Вызвать Recommendasion™

```

## refs/rustify/locales/currently_playing_none_reason.yml

```yaml
_version: 2

currently-playing-none-reason.pause:
  en: |-
    ⏸️ Current track is on pause
  ru: |-
    ⏸️ Текущий трек на паузе

currently-playing-none-reason.nothing:
  en: |-
    🔇 Nothing is currently playing
  ru: |-
    🔇 Сейчас ничего не играет

currently-playing-none-reason.podcast:
  en: |-
    🎙️ It's a podcast
  ru: |-
    🎙️ Это подкаст

currently-playing-none-reason.local:
  en: |-
    📁 It's a local file
  ru: |-
    📁 Это локальный файл

```

## refs/rustify/locales/details.yml

```yaml
_version: 2

details.collecting-info:
  en: |-
    <tg-emoji emoji-id="5188621441926438751">🎵</tg-emoji> %{track_name}
    Album: %{album_name}

    <tg-emoji emoji-id="5451646226975955576">⏳</tg-emoji> Collecting information about the track <tg-emoji emoji-id="5188217332748527444">🔍</tg-emoji>
  ru: |-
    <tg-emoji emoji-id="5188621441926438751">🎵</tg-emoji> %{track_name}
    Альбом: %{album_name}

    <tg-emoji emoji-id="5451646226975955576">⏳</tg-emoji> Собираем информацию о треке <tg-emoji emoji-id="5188217332748527444">🔍</tg-emoji>

details.minor:
  en: >-
    Minor
  ru: >-
    Минор

details.major:
  en: >-
    Major
  ru: >-
    Мажор

details.no-result:
  en: >-
    Something
  ru: >-
    Что-то

details.features:
  en: |
    🎶 <code>%{key} %{modality}</code> ⌛ %{tempo} BPM
    🎻 Acoustic %{acousticness}%
    🕺 Danceability %{danceability}%
    ⚡️ Energy %{energy}%
    🤐 Instrumentalness %{instrumentalness}%
    🏟 Live performance %{liveness}%
    🎤 Speech-like %{speechiness}%
    ☺️ Positivity %{valence}%
  ru: |
    🎶 <code>%{key} %{modality}</code> ⌛ %{tempo} BPM
    🎻 Акустичность %{acousticness}%
    🕺 Подходит для танцев %{danceability}%
    ⚡️ Энергичность %{energy}%
    🤐 Без вокала %{instrumentalness}%
    🏟 Исполнено вживую %{liveness}%
    🎤 Речевая составляющая %{speechiness}%
    ☺️ Позитивность %{valence}%

details.header:
  en: |-
    🎵 %{track_name}
    Album: %{album_name}

    %{features}👎 Disliked by %{disliked_by} people
    🙈 Ignored by %{ignored_by} people
  ru: |-
    🎵 %{track_name}
    Альбом: %{album_name}

    %{features}👎 Не нравится %{disliked_by} людям
    🙈 Игнорируется %{ignored_by} людьми

details.genres:
  en: |-
    🎭 Genres: %{genres}
  ru: |-
    🎭 Жанры: %{genres}

details.no-lyrics:
  en: |-
    %{header}
    %{genres_line}

    <code>No lyrics found</code>
  ru: |-
    %{header}
    %{genres_line}

    <code>Текст песни не найден</code>

details.with-lyrics:
  en: |-
    %{header}
    🤬 Profanity <code>%{profanity}</code>
    🌐 Language: %{language}
    %{genres_line}

    %{lyrics}

    <a href="%{lyrics_link}">%{lyrics_link_text}</a>
  ru: |-
    %{header}
    🤬 Ненормативная лексика <code>%{profanity}</code>
    🌐 Язык: %{language}
    %{genres_line}

    %{lyrics}

    <a href="%{lyrics_link}">%{lyrics_link_text}</a>

```

## refs/rustify/locales/errors.yml

```yaml
_version: 2

error.cannot-skip:
  en: |-
    ❌ Current song (%{track_name}) was disliked, but I cannot skip it...
  ru: |-
    ❌ Текущая песня (%{track_name}) была дизлайкнута, но я не могу её пропустить...

error.unavailable-in-country:
  en: |-
    🚫 Your Spotify account says 'Spotify is unavailable in this country'.
    Please fix this issue and relogin by calling /%{command} command to continue using Rustify
  ru: |-
    🚫 Ваш аккаунт Spotify сообщает 'Spotify недоступен в этой стране'.
    Пожалуйста, исправьте эту проблему и войдите заново, вызвав команду /%{command}, чтобы продолжить использовать Rustify

error.spotify-auth-failed:
  en: |-
    ❌ Failed to Authenticate user to Spotify. Please relogin by calling /%{command} command.

    <pre>%{error}</pre>
  ru: |-
    ❌ Не удалось аутентифицировать пользователя в Spotify. Пожалуйста, войдите заново, вызвав команду /%{command}.

    <pre>%{error}</pre>

error.spotify-invalid-token:
  en: |-
    Relogin to Spotify by calling /%{command} command
  ru: |-
    Войдите заново в Spotify, вызвав команду /%{command}

error.general:
  en: |-
    <b>Sorry, an error has occurred :(</b>

    <a href="https://github.com/vtvz/rustify/issues/new">Report an issue on GitHub</a>
  ru: |-
    <b>Извините, произошла ошибка :(</b>

    <a href="https://github.com/vtvz/rustify/issues/new">Сообщить о проблеме на GitHub</a>

error.unhandled-request:
  en: |-
    <tg-emoji emoji-id="6043973168291384891">😔</tg-emoji> Your request was not handled

    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> There are commands available to you:
  ru: |-
    <tg-emoji emoji-id="6043973168291384891">😔</tg-emoji> Ваш запрос не был обработан

    <tg-emoji emoji-id="5424780918776671920">📋</tg-emoji> Вам доступны следующие команды:

```

## refs/rustify/locales/inline_buttons.yml

```yaml
_version: 2

inline-buttons.dislike:
  en: |-
    Block track 👎
  ru: |-
    Заблокировать трек 👎

inline-buttons.ignore:
  en: |-
    Hide alerts for this track 🙈
  ru: |-
    Скрыть уведомления для этого трека 🙈

inline-buttons.analyze:
  en: |-
    Analyze text 🔍
  ru: |-
    Анализировать текст 🔍

inline-buttons.artist-page:
  en: |-
    Go to artist's page 🔗
  ru: |-
    Перейти на страницу исполнителя 🔗

inline-buttons.alert-login:
  en: |-
    You need to login first
  ru: |-
    Сначала нужно войти в систему

inline-buttons.alert-broken:
  en: |-
    Button is broken. Try another one
  ru: |-
    Кнопка не работает. Попробуйте другую

```

## refs/rustify/locales/keyboards.yml

```yaml
_version: 2

start-keyboard.dislike:
  en: |-
    👎 Dislike playing track
  ru: |-
    👎 Не нравится трек

start-keyboard.stats:
  en: |-
    📈 Stats
  ru: |-
    📈 Статистика

start-keyboard.details:
  en: |-
    🎤 Details of track
  ru: |-
    🎤 Детали трека

language.change:
  en: |-
    English 🇺🇸
  ru: |-
    Русский 🇷🇺

language.changed:
  en: |-
    Your language is now English 🇺🇸
  ru: |-
    Теперь Ваш язык Русский 🇷🇺

language.command:
  en: |-
    Choose your language
  ru: |-
    Выберите язык

```

## refs/rustify/locales/login.yml

```yaml
_version: 2

login.success:
  en: |-
    🎉 Great! You registered successfully!

    Now you can:
    • Press '%{dislike_button}' to block a song
    • Press '%{details_button}' to get the lyrics of the currently playing song and additional information
    • There you can also analyze lyrics using AI
    • Use the /%{magic_command} command to generate a <i>Magic Playlist™</i>✨
    • Use the /%{skippage_command} command to activate the <i>Skippage™</i> function
  ru: |-
    🎉 Отлично! Вы успешно зарегистрировались!

    Теперь вы можете:
    • Нажать '%{dislike_button}', чтобы заблокировать песню
    • Нажать '%{details_button}', чтобы получить текст проигрываемой песни и дополнительную информацию
    • Там же вы можете проанализировать текст с помощью ИИ
    • Вызвать команду /%{magic_command}, чтобы сгенерировать <i>Magic Playlist™</i>✨
    • Вызвать команду /%{skippage_command}, чтобы активировать функцию <i>Skippage™</i>

login.invite:
  en: |-
    <b>Welcome to Rustify!</b>

    🚀 To get started, connect your <i>Spotify</i> account:

    • Click the button below 👇
    • Authenticate through Spotify 🔐
    • Enjoy! 🎵

    <b>Features:</b>
    • Block unwanted songs ("Dislike")
    • Notifications for profanity in lyrics (English only)
    • Detect AI-generated music (notify/ignore/auto-skip)
    • Get song lyrics and AI-powered analysis
    • Create <i>Magic Playlist™</i>✨ from your favorite tracks
    • <i>Skippage™</i> function will diverse your listening experience
  ru: |-
    <b>Добро пожаловать в Rustify!</b>

    🚀 Для начала работы подключите ваш <i>Spotify</i> аккаунт:

    • Нажмите кнопку ниже 👇
    • Авторизуйтесь через Spotify 🔐
    • Наслаждайтесь! 🎵

    <b>Возможности:</b>
    • Блокировка нежелательных песен ("Дизлайк")
    • Уведомление на нецензурную лексику в тексте (только английский)
    • Обнаружение ИИ-музыки (уведомление/игнорирование/авто-пропуск)
    • Получение текстов песен и их анализ с помощью ИИ
    • Создание <i>Magic Playlist™</i>✨ из любимых треков
    • Функция <i>Skippage™</i> разнообразит ваши впечатления от прослушивания

login.button:
  en: |-
    🎵 Login with Spotify
  ru: |-
    🎵 Войти через Spotify

```

## refs/rustify/locales/magic.yml

```yaml
_version: 2

magic.header:
  en: >-
    <i>Magic Playlist™</i> ✨ is made of your shuffled favorite songs that will be removed from this playlist as you listen to them.
    This allows you to listen to everything you love one by one without any repetition. You'll love it! 😊
  ru: >-
    <i>Magic Playlist™</i> ✨ состоит из ваших любимых песен в случайном порядке, которые будут удаляться из плейлиста по мере их прослушивания.
    Это позволяет вам слушать всё, что вы любите, по одному треку без повторений. Вам понравится! 😊

magic.description:
  en: |-
    %{header}

    👇 Click the button below to create or update <i>Magic Playlist™</i> ✨
  ru: |-
    %{header}

    👇 Нажмите на кнопку ниже чтобы создать или обновить <i>Magic Playlist™</i> ✨

magic.generating:
  en: |-
    %{header}

    ⏳ Generating <i>Magic Playlist™</i> ✨
  ru: |-
    %{header}

    ⏳ Создаю <i>Magic Playlist™</i> ✨

magic.button:
  en: |-
    ✨ Generate Magic Playlist™ ✨
  ru: |-
    ✨ Создать Magic Playlist™ ✨

magic.generated:
  en: |-
    %{header}

    ✨ Created <a href="%{url}">Magic Playlist™</a> ✨
  ru: |-
    %{header}

    ✨ Создан <a href="%{url}">Magic Playlist™</a> ✨

magic.failed:
  en: |-
    %{header}

    ❌ Something went wrong. Please try again later 🤷
  ru: |-
    %{header}

    ❌ Что-то пошло не так. Попробуйте позже 🤷

```

## refs/rustify/locales/profanity_check.yml

```yaml
_version: 2

profanity-check.message:
  en: |-
    🚨 Current song (%{track_name}) <b>probably</b> has bad words:

    %{bad_lines}

    <a href="%{lyrics_link}">%{lyrics_link_text}</a>

    Press '%{ignore_button_label}' to never see this notification for <b>this song</b> again
  ru: |-
    🚨 Текущая песня (%{track_name}) <b>вероятно</b> содержит нецензурные слова:

    %{bad_lines}

    <a href="%{lyrics_link}">%{lyrics_link_text}</a>

    Нажмите '%{ignore_button_label}', чтобы больше не видеть это уведомление для <b>этой песни</b>

```

## refs/rustify/locales/rate_limit.yml

```yaml
_version: 2

rate-limit.analysis:
  en: >-
    ⏱️ Wait %{duration} before analyzing again
  ru: >-
    ⏱️ Подождите %{duration} перед следующим анализом

rate-limit.magic:
  en: >-
    ⏱️ Wait %{duration} before generating Magic Playlist™ ✨ again
  ru: >-
    ⏱️ Подождите %{duration} перед следующей генерацией Magic плейлиста™ ✨

rate-limit.details:
  en: >-
    ⏱️ Wait %{duration} before checking track details again
  ru: >-
    ⏱️ Подождите %{duration} перед следующим просмотром информации о треке

rate-limit.recommendasion:
  en: >-
    ⏱️ Wait %{duration} before using 🤖 Recommendasion again
  ru: >-
    ⏱️ Подождите %{duration} перед следующим запуском 🤖 Recommendasion

rate-limit.dislike:
  en: >-
    ⏱️ Wait %{duration} before disliking another track
  ru: >-
    ⏱️ Подождите %{duration} перед следующим дизлайком

rate-limit.like:
  en: >-
    ⏱️ Wait %{duration} before liking another track
  ru: >-
    ⏱️ Подождите %{duration} перед следующим лайком

```

## refs/rustify/locales/recommendasion.yml

```yaml
_version: 2

recommendasion.button:
  en: |-
    🤖 Generate recommendations
  ru: |-
    🤖 Сгенерировать рекомендации

recommendasion.welcome:
  en: |-
    🤖 <i>Recommendasion™</i> is an experimental feature that uses your preferences to suggest something new with AI.

    Click <i>"🤖 Generate recommendations"</i> and AI will analyze your likes and add something fresh to your Spotify queue. AI can be silly sometimes, so don't judge too harshly 🙂
  ru: |-
    🤖 <i>Recommendasion™</i> экспериментальная функция, которая использует ваши предпочтения, чтобы с помощью ИИ предложить что-то новое.

    Нажмите <i>"🤖 Сгенерировать рекомендации"</i> и ИИ проанализирует ваши лайки и добавит в очередь Spotify что-нибудь новенькое. ИИ бывает глуп, так что не судите строго 🙂

recommendasion.disabled:
  en: |-
    🚫 <i>Recommendasion™</i> is temporarily disabled. Work is underway to restore it
  ru: |-
    🚫 <i>Recommendasion™</i> пока отключен. Ведутся работы по его восстановлению

recommendasion.device-not-found:
  en: |-
    ❌ Device not found. Start listening to something and try again
  ru: |-
    ❌ Устройство не найдено. Начните что-то слушать и попробуйте снова

recommendasion.collecting-favorites:
  en: |-
    ⌛️ Generating recommendations...

    ❤️ Collecting your favorite songs
  ru: |-
    ⌛️ Генерирую рекомендации...

    ❤️ Собираю ваши любимые песни

recommendasion.ask-ai:
  en: |-
    ⌛️ Generating recommendations...

    🤖 Asking AI for track recommendations
  ru: |-
    ⌛️ Генерирую рекомендации...

    🤖 Спрашиваю ИИ о рекомендуемых треков для вас

recommendasion.queue:
  en: |-
    ⌛️ Generating recommendations...

    🎵 Adding tracks to Spotify queue
  ru: |-
    ⌛️ Генерирую рекомендации...

    🎵 Добавляю треки в очередь Spotify

recommendasion.result:
  en: |-
    ✅ New tracks added to Spotify queue. Skip to the next track to start listening.

    🎯 Recommendations:

    %{recommendation_links}

    📊 Slop rate: %{slop_rate}% tracks not found on Spotify

    Legend: 👎 dislike, 🔍 not found

    <blockquote expandable>%{slop_links}</blockquote>
  ru: |-
    ✅ Новые треки добавлены в очередь Spotify. Переключитесь на следующий трек, чтобы начать слушать.

    🎯 Рекомендации:

    %{recommendation_links}

    📊 Процент шлака: %{slop_rate}% треков не найдено в Spotify

    Условные обозначения: 👎 дизлайк, 🔍 не найден

    <blockquote expandable>%{slop_links}</blockquote>

```

## refs/rustify/locales/settings.yml

```yaml
_version: 2

settings.profanity-check-on:
  en: |-
    🚫 Tracks will be checked for profanity content
  ru: |-
    🚫 Треки будут проверяться на нецензурный контент

settings.profanity-check-off:
  en: |-
    ✅ Tracks won't be checked for profanity content
  ru: |-
    ✅ Треки не будут проверяться на нецензурный контент

settings.skip-on:
  en: |-
    ⏭️ Disliked tracks will be skipped
  ru: |-
    ⏭️ Нелюбимые треки будут пропускаться

settings.skip-off:
  en: |-
    ▶️ Disliked tracks won't be skipped
  ru: |-
    ▶️ Нелюбимые треки не будут пропускаться

```

## refs/rustify/locales/skippage.yml

```yaml
_version: 2

skippage.main:
  en: |-
    ⏭️ <i>Skippage™</i> allows you to skip tracks you've listened to for a set amount of days. It makes your music experience more diverse. <b>Try it!</b>

    Pass number of days to remember played tracks and enable this function like that:
    <code>/%{command} 7</code>

    Current amount of days: <b>%{setting}</b>
  ru: |-
    ⏭️ <i>Skippage™</i> позволяет пропускать треки, которые вы слушали в течение заданного количества дней. Это делает ваш музыкальный опыт более разнообразным. <b>Попробуйте!</b>

    Укажите количество дней для запоминания прослушанных треков и включите эту функцию следующим образом:
    <code>/%{command} 7</code>

    Текущее количество дней: <b>%{setting}</b>

skippage.main.unset-days:
  en: unset
  ru: не указано

skippage.main.set-days:
  en: "%{days} day(s)"
  ru: "%{days} дн."

skippage.updated:
  en: >-
    ⏭️ All tracks you've listened to within <b>%{days} days</b> will be skipped
  ru: >-
    ⏭️ Все треки, которые вы слушали в течение <b>%{days} дней</b>, будут пропущены

skippage.disabled:
  en: >-
    ⏭️ <i>Skippage™</i> is disabled
  ru: >-
    ⏭️ <i>Skippage™</i> отключен

skippage.validation:
  en: >-
    ⚠️ Number should be positive and 365 or less
  ru: >-
    ⚠️ Число должно быть положительным и не больше 365

skippage.enable-button:
  en: |-
    ✅ Enable Skippage™
  ru: |-
    ✅ Включить Skippage™

skippage.disable-button:
  en: |-
    ❌ Disable Skippage™
  ru: |-
    ❌ Отключить Skippage™

```

## refs/rustify/locales/song_links.yml

```yaml
_version: 2

song-links.button:
  en: |
    Links to other platforms 🔗
  ru: |
    Ссылки на другие платформы 🔗

song-links.fetch:
  en: |
    🎵 %{track_name}
    Album: %{album_name}

    🔍 Finding links to other platforms...
  ru: |
    🎵 %{track_name}
    Альбом: %{album_name}

    🔍 Поиск ссылок на другие платформы...

song-links.result:
  en: |
    🎵 %{track_name}
    Album: %{album_name}

    🎧 Available on:
    %{links}

    ⚡ Links provided by <a href="https://odesli.co/">SongLink</a>
  ru: |
    🎵 %{track_name}
    Альбом: %{album_name}

    🎧 Доступно на:
    %{links}

    ⚡ Ссылки предоставлены <a href="https://odesli.co/">SongLink</a>

```

## refs/rustify/locales/status.yml

```yaml
_version: 2

status.inactive-notification:
  en: |-
    💤 Your account has been marked as inactive due to inactivity. Use /%{start} to reactivate and resume all bot functions.
  ru: |-
    💤 Ваш аккаунт был помечен как неактивный из-за бездействия. Используйте /%{start}, чтобы реактивировать и возобновить все функции бота.

status.reactivated:
  en: |-
    <b>🎉 Welcome back!</b>

    Your account is now active again. All bot functions have been resumed.
  ru: |-
    <b>🎉 С возвращением!</b>

    Ваш аккаунт снова активен. Все функции бота возобновлены.

```

## refs/rustify/locales/user_word_whitelist.yml

```yaml
_version: 2

user-word-whitelist.add-provide-word:
  en: |-
    📝 Provide word <code>/%{command} your-word</code>
  ru: |-
    📝 Укажите слово <code>/%{command} ваше-слово</code>

user-word-whitelist.limit-length:
  en: |-
    ❌ Word must not be longer than %{limit} characters
  ru: |-
    ❌ Слово должно быть не длиннее %{limit} символов

user-word-whitelist.limit-amount:
  en: |-
    ❌ You can add no more than %{limit} words
  ru: |-
    ❌ Вы можете добавить не больше %{limit} слов

user-word-whitelist.word-added:
  en: |-
    ✅ Word <code>'%{word}'</code> added to whitelist

    📋 To list all words in whitelist /%{command}
  ru: |-
    ✅ Слово <code>'%{word}'</code> добавлено в белый список

    📋 Чтобы посмотреть все слова в белом списке /%{command}

user-word-whitelist.word-exist:
  en: |-
    ⚠️ Word <code>'%{word}'</code> is already in whitelist

    📋 To list all words in whitelist /%{command}
  ru: |-
    ⚠️ Слово <code>'%{word}'</code> уже в белом списке

    📋 Чтобы посмотреть все слова в белом списке /%{command}

user-word-whitelist.remove-provide-word:
  en: |-
    🗑️ Provide word <code>/%{command} your-word</code>
  ru: |-
    🗑️ Укажите слово <code>/%{command} ваше-слово</code>

user-word-whitelist.removed:
  en: |-
    🗑️ Word <code>'%{word}'</code> removed from whitelist
  ru: |-
    🗑️ Слово <code>'%{word}'</code> удалено из белого списка

user-word-whitelist.doesnt-exist:
  en: |-
    ❌ Word <code>'%{word}'</code> is not in whitelist

    📋 To list all words in whitelist /%{command}
  ru: |-
    ❌ Слово <code>'%{word}'</code> не в белом списке

    📋 Чтобы посмотреть все слова в белом списке /%{command}

user-word-whitelist.empty:
  en: |-
    📭 Your whitelist is empty

    ➕ Add new word with <code>/%{command} your-word</code>
  ru: |-
    📭 Ваш белый список пуст

    ➕ Добавьте новое слово с помощью <code>/%{command} ваше-слово</code>

user-word-whitelist.list:
  en: |-
    📝 Words you added to whitelist:

    %{words}

    🗑️ You can remove a word with <code>/%{command} your-word</code> command
  ru: |-
    📝 Слова, которые вы добавили в белый список:

    %{words}

    🗑️ Вы можете удалить слово командой <code>/%{command} ваше-слово</code>

```

---

# Appendix D — feel-me `src/strings.rs` (VERBATIM, complete)

Source: `refs/feel-me/src/strings.rs` (66 lines) — every user-facing string in feel-me.

```rust
lazy_static! {
    pub static ref HELPTEXT: &'static str = "
        Send Music: You can send your music by clicking on user's uniqu link\
        \nGet your own link: You can get your link by click on `Get Link Button`\
        \nAvailable commands:\
            \n\t/start: Start the bot and send music or get your link\
            \n\t/help: See this Desctiption again :)
    ";

    pub static ref WRONG: &'static str = "Something went wrong on server side\nPlease try again";

    pub static ref INVALID_MSG: &'static str = "Invalid Message";

    pub static ref INVALID_MSG_FORMAT: &'static str = "Invalid message format";

    pub static ref BOT_URL: &'static str = "https://t.me/FeelMeBot?start=";

    pub static ref CANCEL_SENDING: &'static str = "Cancel sendig music";

    pub static ref FEEDBACK_BLOCKED: &'static str = "You can't send Feedback\nYou are blocked";

    pub static ref FEEDBACK_SENT: &'static str = "Feedback is sent before";

    pub static ref FEEDBACK_SEND: &'static str = "Send Feedback Text";

    pub static ref USER_REPORTED_BLOCKED: &'static str = "User is reported and blocked";

    pub static ref ERROR_ON_GETTING_USER: &'static str = "Erro on getting Users";

    pub static ref DB_CONN_ERROR: &'static str = "Error on connecting to";

    pub static ref USER_CREATING: &'static str = "Error on creating new user";

    pub static ref HISTORY_CREATING: &'static str = "Error while storing history";

    pub static ref SET_BL_ERR: &'static str = "Error on set to blacklist";

    pub static ref SEARCH_BL_ERR: &'static str = "Error on searching for blacklist";

    pub static ref SEND_MSG_TO_SELF: &'static str = "Dude?! You have schizophrenia\n Why you send music to yourself";

    pub static ref SEND_MUSIC_TEXT: &'static str = "Please Send the music that you want to share with";

    pub static ref BLOCKED_TEXT1: &'static str = "You can't send music to";

    pub static ref BLOCKED_TEXT2: &'static str = "You are blocked";

    pub static ref INVALID_LINK: &'static str = "Oops!\nInvalid link";

    pub static ref INVALID_CMD: &'static str = "Invalid Command";

    pub static ref FEEDBACK_IS_SENT: &'static str = "Feedback is sent";

    pub static ref UNKNOWN_TARGET: &'static str = "The target is unknown\nClick on target user's link and then send the music";

    pub static ref REPORTED_BEFORE: &'static str = "User already is reported";

    pub static ref HAS_BEEN_SENT: &'static str = "Music Has Been send";
    
    pub static ref LOG: i16 = 0;

    pub static ref FEEDBACK: i16 = 1;

    pub static ref REPORT: i16 = 2;

    pub static ref UNREPORT: i16 = 3;
}
```

---

# Appendix E — telegram-music-bot-rust: all user strings (VERBATIM list)

Source: `refs/telegram-music-bot-rust/main.rs`. Every user-visible string in the file, in order of appearance (Russian, as written):

```
Салам! 👋 Я твой музыкальный бот.

Напиши название песни или скинь ссылку на трек/видео!
🚫 Вы заблокированы за спам.
🔗 Ссылка принята! Что именно скачать?
🎵 Аудио (MP3)  |  🎬 Видео (MP4)
🔎 Ищу трек...
❌ Ссылка устарела.
📥 Скачиваю...
❌ Ошибка скачивания.
📌 Трек: <b>{title}</b>

Выбери вариант обработки:
🎵 Оригинал  |  🌌 Slow + Reverb  |  ⚡ Nightcore  |  🔊 Bass Boost  |  🌀 8D Audio
📥 Скачиваю и применяю эффект...
❌ Ошибка скачивания трека.
❌ Результаты устарели.
Ничего не найдено.
Выбери трек:
```
(Buttons in the search list: `{N}. {title≤35 chars}{ [M:SS]}` → `dl_{idx}_{msg_id}`.)

---

*End of inventory. Port workers: prefer the appendices for exact text; §1–§7 for layout, callback_data, and behaviour. Known gaps/quirks flagged in the text: ParvatiMusic help_admin says speed 0.5x–2.0x while its speed usage text says 0.50x–4.00x; rustify `command.ai-slop-detection` has no locale entry; TGVCBot /help omits several real commands; feel-me strings contain typos kept verbatim ("Desctiption", "sendig", "Has Been send", "Erro").*
