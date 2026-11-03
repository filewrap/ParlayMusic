# Parlay Phase 9 — UI/UX Inventory, Group C (7 of 21 refs)

**Scope:** every user-visible Telegram UI/UX element harvested verbatim from these 7 refs under `~/workspace/parlay/refs/`. Port workers implement from this file — exact strings, emojis, formatting and `callback_data` values are preserved. Where a value is built at runtime (env var, user name, song title), the template is shown with the variable in `{braces}` / backticks as in source.

**Conventions used in this file**
- `U+XXXX` = the exact Unicode code point in source (e.g. `\u{1F680}` = 🚀). Rendered emoji is shown next to it where unambiguous.
- `callback_data` values are shown exactly as sent on the wire (JSON-stringified objects included verbatim).
- Parse mode is noted per message (`Markdown`, `HTML`, or none/plain).
- "No UI in this category" is stated explicitly per ref where applicable.

**Element counts (detail in sections below)**

| Ref | Elements harvested |
|---|---|
| 1. wavetune | 14 (web Mini-App UI only — **no Telegram bot code in this checkout**) |
| 2. openradiobot | 26 |
| 3. beetube-bot | 48 |
| 4. Telegram-Song-Downloader-Bot | 9 |
| 5. Spotify-Loader | 22 |
| 6. lemonic-telegram-bot | 30 |
| 7. music-bot-navidrome | 62 |
| **Total** | **211** |

---

# 1. wavetune — `refs/wavetune/`

**Nature of this ref — read first.** This checkout contains **no Telegram bot code at all**: no bot token, no commands, no `sendMessage`, no inline keyboards. It is a Next.js web app (a Telegram Mini-App / web GUI for a music bot, per its README). All elements below are **web-UI strings** (headings, placeholders, icon-only buttons). There is **no /start, /help, now-playing card, queue display, lyrics view, settings menu, admin panel, error message, inline-mode result, reply keyboard, or rich-message pattern in Telegram** in this ref — those categories are all **absent**.

README claims (not verified in code — the bot itself is not in this checkout): "Add WaveTune to your Telegram group. Use /play command to start streaming music. Control music via the Web GUI (Accessible via Telegram Mini Apps)."

## 1.1 Home / browse page — `app/(root)/page.js`

| # | Element | Verbatim text | Location |
|---|---|---|---|
| W-01 | Section heading | `Songs` + subtitle `Top new released songs.` | `app/(root)/page.js:40-41` |
| W-02 | Section heading | `Albums` + subtitle `Top new released albums.` | `app/(root)/page.js:66-67` |
| W-03 | Section heading | `Artists` + subtitle `Most searched artists.` | `app/(root)/page.js:92-93` |
| W-04 | Section heading | `Trending` + subtitle `Most played songs in this week.` | `app/(root)/page.js:148-149` |

## 1.2 Search UI

| # | Element | Verbatim text | Location |
|---|---|---|---|
| W-05 | Header search input placeholder | `Try Maharani..` | `components/page/search.jsx:38` |
| W-06 | Player-page search input placeholder | `Search for songs by name...` | `app/(player)/_components/AdvanceSearch.jsx:77` |
| W-07 | Search-in-progress label | `Searching...` | `app/(player)/_components/AdvanceSearch.jsx:93` |
| W-08 | No-results label | `No results found!` | `app/(player)/_components/AdvanceSearch.jsx:98` |
| W-09 | Results heading | `Search results for {query}` (query highlighted) | `app/(player)/_components/AdvanceSearch.jsx:109` |
| W-10 | Search page heading | `Search Results` | `app/(root)/search/_components/Search.jsx:37` |

## 1.3 Player UI — `app/(player)/_components/Player.jsx`, `components/cards/player.jsx`

Icon-only buttons (lucide icons, **no text labels**): Play, Pause, StepBack (previous), StepForward (next), Repeat / Repeat1 (loop toggle), RedoDot / UndoDot (seek ±), Download (spinner `Loader2` while `isDownloading`), Share2 (share), Bookmark / BookmarkCheck (save toggle). No verbatim button text exists for these.

| # | Element | Verbatim / behaviour | Location |
|---|---|---|---|
| W-11 | Player controls | Icon-only (see above); queue auto-fills from recommendations: on track end, `setQueue(queue.slice(1))` then appends `recommendedData.suggestions` | `app/(player)/_components/Player.jsx:102,116-120,226,244-246` |
| W-12 | Album view count line | `{data.songCount} songs` | `app/(root)/album/_components/Album.jsx:35` |
| W-13 | Album page title template | `Album - {data.data.name}` | `app/(root)/album/[id]/page.jsx:8` |

## 1.4 Navigation / chrome

| # | Element | Verbatim text | Location |
|---|---|---|---|
| W-14 | Mobile bottom menu | Icon-only: Home (`/`), Search (`/search/latest`) | `components/mobile-menu.jsx:8-9` |
| W-15 | Footer branding | `Music` + `hub` (styled) / `Deployed with ♥ by - Aɴᴜʀᴀɢ | ⌁` (link to `https://github.com/4nuxd`) | `components/page/footer.jsx:7,9` |

**Categories with NO UI in wavetune:** /start, /help, now-playing card (Telegram), queue display (Telegram), lyrics view, download progress messages, settings menus, admin panels, error messages, inline-mode results, reply keyboards, rich-message patterns, share/download confirmations, provider selection. Data source is JioSaavn (`lib/fetch.js` — `getSongsById`, `getSongsLyricsById`, `getSongsSuggestions`, `getAlbumById`).

---

# 2. openradiobot — `refs/openradiobot/index.js` (396 lines, single file)

A per-chat internet-radio bot (slimbot). **All messages are Markdown** (`parse_mode: "Markdown"` in the `message.reply` wrapper, `index.js` ~line 55). **No inline keyboards, no reply keyboards, no thumbnails, no inline mode, no settings menu, no admin panel, no lyrics, no download progress.** Everything is plain text commands. Long messages (>4096 chars) are split at 4096 and sent in sequence.

## 2.1 /start and /help — one shared text (default case)

Any unknown command (including `/start` and `/help` — there is no dedicated handler; the `default:` branch answers everything not matched) gets this exact text (`index.js`, `default:` case):

```
*OpenradioBot v0.0 Alpha*

__Radio Managing__
/new      - Create new radio
/destroy  - Destroy current radio
/manage   - Manage your radio

__Player Managing__
/play     - Play a song
/pause    - Pause a player
/resume   - Resume a player
/skip     - Skip current song
/stop     - Stop player
/queue    - See & Manage queue list.
/autoplay - Auto play next song from youtube **Related Videos** query.
/loop     - Loop queue
```

Note the literal spacing: command column is padded with spaces to align the `-` descriptions (e.g. `/new      - `, `/autoplay - `).

## 2.2 Radio lifecycle

| # | Command / trigger | Verbatim reply | Location |
|---|---|---|---|
| O-01 | `/new` success | `✔️Radio Created` | `index.js` case `"new"` |
| O-02 | `/new` when radio exists | `You already created your radio. To manage it, Type /manage. To destroy it, Type /destroy` | same |
| O-03 | `/destroy` success | `✔️Radio destroyed.` | case `"destroy"` |
| O-04 | `/destroy`, `/manage`, `/queue`, `/play`, `/pause`, `/resume`, `/skip`, `/stop`, `/autoplay`, `/loop` with **no radio** | `You didn't created radio yet. Did you mean /new ?` (sic — verbatim, including "didn't created") | every case's guard |
| O-05 | `/manage` — status card (Markdown, built line by line) | `*Your radio status*` then lines: `Listener: \`{listener.size}\`` / `Total Listener: \`{totalListener}\`` / `Loop Type: \`{loopType}\`` / `Created Since: \`{ms(...)}\`` (human duration, e.g. `5m`) / `Now Playing: [{title}]({url})` **or** `Now Playing: Voice/Audio Message` / `Autoplay Enabled?: \`Yes\`/\`No\`` / `Total Queue: \`{queue.length}\`` / `Live on: [{SERVER_HOST or http://localhost:3000}/{chat.id}](http://localhost:3000/{chat.id})` / blank line + `To check song queue, Type /queue` | case `"manage"` |

## 2.3 Queue display

| # | Element | Verbatim text | Location |
|---|---|---|---|
| O-06 | `/queue` empty | `🏜️Nothing is in queue....` | case `"queue"` |
| O-07 | `/queue` list (max 20 shown) | Header `*Radio Queue*`, then per song `1. [title](https://youtu.be/{id})` / `1. [Raw Stream]({url})` / `1. Voice/Audio Message` (numbered from 1). Footer: `\n\n⚠️Some song is hidden due to a lot of request value. We'll improve this soon.\n\nYou may also manage these queue. For more information, Do \`/queue help\`` | case `"queue"` |
| O-08 | `/queue help` | `*Queue Managing*` + `\nUsage: \`/queue [method] [argument]\`` + `\n\nAvailable Method:` + `\n  remove  - Remove a song in a queue` + `\n  move    - Move a song in a queue` + `\n  shuffle - Sort queue into random order` + `\n  random  - Alias of \`shuffle\`` | case `"queue"`, method `help` |
| O-09 | `/queue remove` — no arg | `Usage: \`/queue remove [Order number of song in /queue]\`` | same |
| O-10 | `/queue remove` — not found | `No song was found in Queue Order number {args}` | same |
| O-11 | `/queue remove` — ok | `✔️Song number {args} has been removed.` | same |
| O-12 | `/queue move` — no args | `Usage: \`/queue move [Order number] [To Order number]\`` | same |
| O-13 | `/queue move` — invalid | `Song not found or invalid value.` | same |
| O-14 | `/queue move` — ok | `✔️*{fromOrder.title}* order moved to *{toOrder.title}* order.` | same |
| O-15 | `/queue shuffle` / `/queue random` | `✔️Queue order has been sorted randomly.` | same |

## 2.4 Playback / now-playing

| # | Element | Verbatim text | Location |
|---|---|---|---|
| O-16 | `/play` — no argument and no replied audio | `Usage: \`/play [Song name\|URL\|Reply to Audio/Voice Message]\`` | case `"play"` |
| O-17 | `/play` — searching | ``Searching \`{str}\`...`` | same |
| O-18 | `/play` — nothing playing yet, preparing | `Preparing to play...` | same (all branches) |
| O-19 | Now playing (YouTube) | `▶️Now playing: [{videoDetails.title}]({video_url})` | `play()` stream `info` handler |
| O-20 | Now playing (raw stream URL) | `▶️Now Playing: [Raw Stream]({url})` | same |
| O-21 | Now playing (replied voice/audio) | `▶️Playing Voice/Audio Message....` | same |
| O-22 | Queued — replied voice/audio | `✔️Voice has been added to queue` | case `"play"`, audio branch |
| O-23 | Queued — YouTube playlist | `✔️{count} Song has been added to queue` | playlist branch |
| O-24 | Queued — raw stream URL | `✔️A stream URL has been added to queue.` | raw-URL branch |
| O-25 | Queued — YouTube video | `✔️[{title}]({video_url}) has been added to queue.` | ytdl branch |
| O-26 | Queued — search result | `✔️[{title}](https://youtu.be/{id}) has been added to queue.` | ytsr branch |
| O-27 | `/pause` | `⏸️Paused` | case `"pause"` |
| O-28 | `/resume` | `▶️Resumed` | case `"resume"` |
| O-29 | `/skip` | `⏩Skipping...` | case `"skip"` |
| O-30 | `/stop` | `⏹️Player Stopped` | case `"stop"` |
| O-31 | Pause/resume/skip/stop with nothing playing | `There's nothing playing. Glitched? Do /destroy` | guard in each case |
| O-32 | `/skip` with empty queue | `There's nothing in queue!` | case `"skip"` |

## 2.5 Autoplay / loop

| # | Element | Verbatim text | Location |
|---|---|---|---|
| O-33 | `/autoplay` enable | `✔️Autoplay enabled` | case `"autoplay"` |
| O-34 | `/autoplay` disable | `✔️Autoplay disabled` | same |
| O-35 | `/autoplay` on raw stream | `Sorry. You can't use autoplay right now.` | same (note: checks `radio.curSong.type` — likely a bug, `curSong` lives under `metadata`) |
| O-36 | `/loop` — bad/missing arg | `Usage: \`/loop [queue\|single\|none]\`` | case `"loop"` |
| O-37 | `/loop` — ok | ``✔️Loop Type has been set as \`{loopType}\``` | same |

## 2.6 Errors / misc

| # | Element | Verbatim text | Location |
|---|---|---|---|
| O-38 | Search no result | `🙅No Result.` | `play`, ytpl + ytsr branches |
| O-39 | Region-locked video | `❌Sorry. We can't Play this video due to our server region lock.` | ytdl branch |
| O-40 | Unsupported replied attachment | `Unsupported Formats` | audio branch |
| O-41 | Generic send error | `An error occured: {error}` (sic — "occured") | `message.reply` wrapper + ytsr `.catch` |
| O-42 | Player/stream errors | Raw `err.toString()` sent as the reply | `play()` error handlers |

**Categories with NO UI in openradiobot:** inline keyboards/buttons, reply keyboards, thumbnails, inline mode, settings menus, admin panels, lyrics, download progress messages, share/download confirmations, provider selection, /start distinct from /help.

---

# 3. beetube-bot — `refs/beetube-bot/` (Node.js, node-telegram-bot-api, MongoDB)

Provider-agnostic download bot (Music / Movie / Torrent / Anime). **Parse modes: Markdown for start/about/settings; HTML for search/list results.** Has both a **persistent reply keyboard** and **inline keyboards**. Callback data is a **JSON string** (`{"type": "...", ...}`) — the `type` field is the discriminator. Description text is truncated at **300 chars** (`config/index.js`: `message.textLimit: 300`).

## 3.1 Bot command list (BotFather `/setcommands`, `README.md:61-68`)

```
start - start a conversation with the bot
search - search through any category
keyboard - show custom keyboard
about - about this bot
```

## 3.2 /start — `commands/start.js`

- Chat action: `typing`.
- New/blocked users are (re)activated in DB; no visible text for that.
- Message (Markdown, `keyboard` reply markup attached):

```
Hi {username} 🚀

🐝 {BOT_NAME} offers free music, movies, anime, torrent, EDM tracks, series downloads, files, and more.

To stay up to date on new *features*, *bug fixes*, and *enhancements* happening here, join our official channel [@{BOT_CHANNEL}](https://t.me/{BOT_CHANNEL})
```

where `{username}` = `[First Name](tg://user?id={id})` if first name exists, else `@{username}`. (Note: the source has literal `\n\t\t` indentation inside the template — lines are indented with tabs in the sent text.)

## 3.3 Persistent reply keyboard — `utils/bot-helper.js` (`exports.keyboard`)

Attached to /start, /about, /keyboard, and most provider messages. `resize_keyboard: true`, `parse_mode: "Markdown"`.

| Row | Buttons (verbatim) |
|---|---|
| 1 | `🎬 Movie` · `💀 Music` · `🌍 Torrent` |
| 2 | `🎡 Anime` · `🔍 Global Search` |
| 3 | `⚙ Settings` · `📌 About` |

Routing (`bot.js`): `bot.onText(/about$/i, …)`, `/search$/i`, `/settings$/i`, `/\/keyboard/`, and `/(?<provider>(Movie|Music|Torrent|Anime)$)/` → list command. So tapping a row-1/row-2 category button (or typing e.g. `Music`) opens that provider's list; `Global Search` has **no handler** (falls through — nothing happens).

- `/keyboard` command reply text: `⌨ Keyboard enabled.` (with the keyboard attached) — `commands/keyboard.js`.

## 3.4 /about — `commands/about.js`

Message (Markdown + reply keyboard):

```
ℹ *About*

{BOT_NAME} is an open source bot developed to give users access to unlimited free downloads from within telegram. 

Official channel: @{BOT_CHANNEL}
Github: [kodjunkie/beetube-bot]({homepage})
For bug report create an [issue]({bugs.url})

`If you find this bot useful, feel free to give us a ⭐`

Want to contribute to the development of this project? PRs are welcomed.
```

## 3.5 /search — category picker — `commands/search.js`

- Text (HTML parse mode): `Select a category to proceed 👇`
- Inline keyboard (2×2):

| Row | Button text | `callback_data` (JSON) |
|---|---|---|
| 1 | `🎬 Movie` | `{"type":"find_movie"}` |
| 1 | `💀 Music` | `{"type":"find_music"}` |
| 2 | `🌍 Torrent` | `{"type":"find_torrent"}` |
| 2 | `🎡 Anime` | `{"type":"find_anime"}` |

Tapping one starts that provider's **interactive search** (force-reply prompt, §3.8).

## 3.6 /settings — `commands/settings.js` + `utils/bot-helper.js` (`exports.settings`)

- Text (Markdown): `⚙ *Settings*` + newline + `Use this section to apply personalized custom tweaks to 🐝 {BOT_NAME}`
- Inline keyboard (1 row, 2 buttons):

| Button text | `callback_data` (JSON) |
|---|---|
| `⏩ Pagination` | `{"type":"page_setting"}` |
| `💀 Music Download` | `{"type":"music_setting"}` |

### 3.6.1 Pagination settings screen — `settings/page.js`

- Text (HTML, edited in place via `editMessageText`):
```
⏩ <b>Auto delete old messages</b>
By default previous messages are deleted during pagination to ease chat scrolling, use this section to change this behaviour.
```
- Inline keyboard:

| Row | Button text | `callback_data` (JSON) |
|---|---|---|
| 1 | `✅ Enabled` (when on) / `❌ Disabled` (when off) — built by `toggleFeedbackText(value)`: `(value ? "✅ En" : "❌ Dis") + "abled"` | `{"type":"page_setting_1"}` (when currently on) / `{"type":"page_setting_0"}` (when currently off) — tapping flips `purge_old_pages` |
| 2 | `🔙 Back` | `{"type":"index_setting"}` |

### 3.6.2 Music-download settings screen — `settings/music.js`

- Text (HTML, edited in place):
```
⬇ <b>Download music via chat</b> 🚧
Enabling this will allow you download music directly from chat.
<b>Note:</b> The actual size is compressed and you can't download a music file of more than <em>200Mb</em> via chat.
✅ Supported formats
- 	 MPEG-1/2 Audio Layer III (MP3)
- 	 Free Lossless Audio Codec (FLAC)
- 	 Windows Media Audio (WMA)
- 	 Waveform Audio (WAV)
- 	 Ogg Vorbis Audio (OGG)
- 	 Audio Interchange File Format (AIFF)
- 	 Apple Lossless Audio Codec (ALAC)
```
- Inline keyboard: same shape as pagination — toggle button `✅ Enabled` / `❌ Disabled` with `callback_data` `{"type":"music_setting_1"}` / `{"type":"music_setting_0"}` (flips `chat_music_download`), plus row 2 `🔙 Back` → `{"type":"index_setting"}`.

## 3.7 Provider lists & search results

All providers share the pattern: send a transient "fetching/searching" message (deleted afterwards), then one message per result, each with its own inline button row; the **last** result message additionally carries the pagination row. Old result messages are deleted on pagination when `purge_old_pages` is on (default). Pagination state is stored in Mongo (`models/paginator.js`).

### 3.7.1 Status / progress messages (all providers)

| Text | When | File |
|---|---|---|
| `📡 Fetching latest songs` | music list | `providers/music.js` list |
| `📡 Fetching latest movies` | movie list | `providers/movie.js` list |
| `📡 Fetching latest anime` | anime list | `providers/anime.js` list |
| `📡 Fetching latest torrents` | torrent list | `providers/torrent.js` list |
| `📡 Searching for _{query}_` | any provider search (Markdown `_…_` italics inside a message sent with `parse_mode` inherited from keyboard object — note: sent with the `keyboard` options object, so parse_mode is Markdown) | all `search()` |
| `⬇ Preparing the music for download` | music in-chat download starts | `providers/music.js` download |
| `🔍 Tell me the music title or artiste name` | music interactive search (force_reply) | `providers/music.js` interactiveSearch |
| `🔍 Tell me the title of the movie` | movie interactive search (force_reply) | `providers/movie.js` |
| `🔍 Tell me the title of the anime` | anime interactive search (force_reply) | `providers/anime.js` |
| `🔍 Tell me the torrent name or title` | torrent interactive search (force_reply) | `providers/torrent.js` |

### 3.7.2 Music results — `providers/music.js`

- Genre picker (when listing without a genre): text `Select a genre to proceed 👇`, HTML; inline keyboard = genre names as buttons, **3 per row** (`_.chunk(..., 3)`), `callback_data` `{"type":"ls_music","genre":"{name ≤18 chars}","page":1}`.
- Result message (HTML): `💿 <b>{music.name}</b>`
- Download button (one-button row under each result) — two variants:
  - **URL button** (default, or file >200 MB / unsupported ext): text `Download ({size})`, `url: {music.url}`.
  - **In-chat download callback button** (only when `chat_music_download` setting is on AND `parseFloat(size) <= 200` AND ext ∈ `mp3, flac, wma, wav, ogg, aiff, alac`): text `Download ({size})`, `callback_data` `{"type":"dl_music","info":"{sanitised name ≤16 chars}_{host first label}_{key}"}`.
- In-chat download sends the audio with caption (Markdown): `Downloaded via 🐝 [{BOT_NAME} bot](https://t.me/{TELEGRAM_BOT_NAME})`, title `{name} - {key}`, filename `{name} - {key}.m4a`, `audio/m4a`, as a reply to the tapped message.
- Pagination row (on last result message): `Next ➡` → `{"type":"page_ls_music","page":{n+1},"genre":"{genre}"}` (list) or `{"type":"page_find_music","page":{n+1},"query":"{query ≤16 chars}"}` (search); `⬅ Previous` → same with `page-1`, **only shown when `page > 1`** and prepended before Next. Keypad labels (`utils/bot-helper.js` `exports.keypad`): `share: "Share"`, `download: "Download"`, `next: "Next ➡"`, `previous: "⬅ Previous"`.

### 3.7.3 Movie results — `providers/movie.js`

- Result message (HTML): `<a href="{banner}">🎬</a> <b>{name}</b>` + optional `\n\n<b>Description:</b> <em>{description truncated to 300 chars}...</em>` (truncation appends `...` only when over limit).
- Button row: single URL button `Download` → `{movie.url}`. Pagination row as in §3.7.2 with types `page_ls_movie` / `page_find_movie`.

### 3.7.4 Anime results — `providers/anime.js`

- Result message (HTML): `<a href="{banner}">🎡</a> <b>{name}</b>` + optional description block as movie.
- Button row: `Watch` (URL → `{anime.url}`) + `Share` (URL → `https://t.me/share/url?url={encoded anime.url}&text=Downloaded%20via%20@{botTGname}`). Pagination types `page_ls_anime` / `page_find_anime`.

### 3.7.5 Torrent results — `providers/torrent.js`

- Result message (HTML):
```
🌍 <b>{name}</b>

⬆ Seeds: {seeds} ⬇ leeches: {leeches}

<b>Description:</b> <em>{description ≤300 chars}...</em>
```
or, when no description: `<em>{magnetic_link}</em>` in place of the description line.
- Button row: single URL button `🧲 Download ({size})` → `{torrent.url}`. List view has **no pagination**; search pagination types `page_find_torrent` with `query` truncated to 14 chars.
- Note the keypad label is `Download` but the torrent button text is `🧲 Download ({size})` (magnet emoji U+1F9F2 + space + keypad.download).

## 3.8 Callback routing — `handlers/callback-query.js`

- Every callback query is answered immediately (`answerCallbackQuery(cbq.id)` with no text).
- `data.type` ending in `movie|music|torrent|anime` → that provider's `resolve()`; `data.type` containing `setting` → `settings/{name}` where name = type split on `_setting`; anything else → message `🚧 This feature is not available yet.` (sent to chat).
- Provider `resolve()` type table (all JSON `type` values):

| `type` | Meaning |
|---|---|
| `ls_{provider}` | list (page 1 / genre) |
| `page_ls_{provider}` | list pagination |
| `find_{provider}` | interactive search prompt |
| `page_find_{provider}` | search pagination |
| `dl_music` | in-chat music download (music only) |
| `page_setting`, `music_setting` | open settings screen |
| `page_setting_0` / `page_setting_1`, `music_setting_0` / `music_setting_1` | toggle + re-render screen |
| `index_setting` | back to settings index |

## 3.9 Errors / validation — `utils/error-handler.js`, `providers/index.js`

| # | Verbatim text | When |
|---|---|---|
| B-01 | `🚫 An error occurred, try again later.` | any caught handler error (generic error handler) |
| B-02 | `🚫 The list is empty or limit reached.` | list with zero results (`emptyAPIResponse` default) |
| B-03 | `🚫 No results found.` | search with zero results (`emptyAPIResponse(chatId, msgId, "No results found.")`) |
| B-04 | `🚫 Input must be more than a character.` | interactive-search reply of ≤1 character (`searchQueryValidator`) |
| B-05 | `🚧 This feature is not available yet.` | unknown callback type |

**Categories with NO UI in beetube-bot:** /help (no handler — falls through to nothing; `bot.js` registers no `/help`), now-playing card, queue display, lyrics view, inline-mode results, admin panels, provider selection beyond the 4-category picker, download progress bars / percentages (only the two transient status lines above), stats views.

---

# 4. Telegram-Song-Downloader-Bot — `refs/Telegram-Song-Downloader-Bot/` (`bot.py` + `text_msg.py`)

Minimal YouTube→MP3 downloader (telepotpro). **All messages Markdown, no buttons of any kind, no keyboards, no inline mode, no settings, no admin panel.** Command is `/song`, not /play. Audio filename pattern: `{title} - @songdownload597_bot {0-999999}.mp3` (quotes stripped). Songs over 30 min are silently ignored (the `min_duration < 30 and split_count < 3` guard just does nothing — the `TOO_LONG_ERROR_TEXT_MSG` constant exists but is **never sent**).

## 4.1 /start — `text_msg.py` `START_TEXT_MSG`

```
🤖 Hello user!

📩 I can download songs for you. Just send me the song name in below format:

*/song*  _song name_  or
*/song*  _musician name - song name_

to download some songs. 🎶

**■ ProTips**💡[ »Learn Bot](https://t.me/sanilaassistant_bot) 
                          [»Rate Bot](https://t.me/sanilaassistant_bot)
                          [»Get help](https://t.me/sanilaassistant_bot)
                          [»Give feedback](https://t.me/sanilaassistant_bot)
```

(The `**■ ProTips**💡` footer block with four identical links to `https://t.me/sanilaassistant_bot` is appended to **every** message below, verbatim, including the leading spaces before `[»Rate Bot]` etc.)

## 4.2 Download flow — `bot.py` `process_request`

| # | Element | Verbatim text |
|---|---|---|
| S-01 | Song found (sent before download) | `🎵 {title}\n🔗 {link}` |
| S-02 | Downloading status (deleted after send) | `⬇️ Downloading... \n_(this may take a while.)_` |
| S-03 | Success confirmation (`CONFIRMATION_TEXT_MSG`) | `✅ Song downloaded successfully!` + the ProTips footer block |
| S-04 | Send-audio failure path | Same confirmation text is sent even on exception (the `except:` branch also sends `CONFIRMATION_TEXT_MSG`) |

## 4.3 Errors — `text_msg.py`

| # | Constant / trigger | Verbatim text |
|---|---|---|
| S-05 | `SPOTIFY_INPUT_ERROR_TEXT_MSG` — `/song` argument contains `open.spotify.com` | `‼️ *Oops! The bot does not support Spotify links!*` + `Try: "*/song* _song name_"` / `or: "*/song* _musician name - song name_"` + ProTips footer |
| S-06 | `INVALID_COMMAND_ERROR_TEXT_MSG` — anything that isn't `/start` or a valid `/song …` | `‼️ *Oops! Invalid command!*` + same Try-lines + ProTips footer |
| S-07 | `TOO_LONG_ERROR_TEXT_MSG` — defined but **never sent** (dead string) | `‼️ *Oops! Video too long to convert!*` + `Order something 30 minutes or less.` + ProTips footer |

**Categories with NO UI in this ref:** /help, inline/reply keyboards, buttons, thumbnails, now-playing card, queue, lyrics, search-result lists, download progress (only the single `⬇️ Downloading...` line), settings, admin panel, inline mode, provider selection, share confirmations.

---

# 5. Spotify-Loader — `refs/Spotify-Loader/bot.py` (397 lines, Pyrogram + spotdl)

Spotify link/search → MP3 downloader. **Private chats only, owner-filtered** (`OWNER_IDS` env; if set, only those users). **No now-playing, no queue, no lyrics, no inline mode, no thumbnails, no download progress messages, no admin panel.** Settings exist but are per-user download preferences (output format / YouTube source / path template), gated on `MONGO_DB_URL` being set.

## 5.1 /start — `bot.py` `start()`

- Text (no parse mode specified on this send): `I'm Spotify-Loader\nYou can download spotify playlist/artist/album/track music using this bot!`
- Inline keyboard `START_BUTTONS` (also attached to /help):

| Row | Button text | Action |
|---|---|---|
| 1 | `Source` | URL `https://github.com/X-Gorn/Spotify-Loader` |
| 1 | `Project Channel` | URL `https://t.me/xTeamBots` |
| 2 | `Author` | URL `https://t.me/xgorn` |

## 5.2 /help — `bot.py` `help()` (Markdown)

```
How to use this bot?!

Send spotify url and select the options. example regex for spotify url `https://open.spotify.com/track/5HCyWlXFPP0ywqq8TgAc0`: 
   
Commands:
- `/settings` : Settings for more features. only avalable if you've filled MONGO_DB_URL
- `/s` or `/search` : Search music and download. example: `/s Justin Bieber - Stay`
- `/help` : Show this message 
- `/start` : Show start message
```

(Sic: "only avalable", trailing spaces after the example URL line and after `/help` line.) Same `START_BUTTONS` keyboard as /start.

## 5.3 Spotify URL flow

| # | Trigger | Verbatim text / buttons |
|---|---|---|
| SP-01 | Message matching `http.*://open.spotify.com/(track\|album\|artist\|playlist)` | `Select Options Below!` + inline keyboard `CB_BUTTONS`: row 1 = `Send as ZIP` (`callback_data: "zip"`) · `Send one by one` (`callback_data: "1by1"`) |
| SP-02 | Download success (after sending files) | `Successfully uploaded {x} from a Spotify {Track\|Album\|Artist\|Playlist} [ㅤ]({spotify_url})` (Markdown; `[ㅤ]` is an invisible-character link — the link text is U+3164 HANGUL FILLER). `{x}` = `zipped music files` / `all music files`, or the single file's basename without extension when exactly one file. |
| SP-03 | No files downloaded (zip or 1by1) | `Looks like you've send wrong spotify url.` (sic) |
| SP-04 | `/s` / `/search` with no results | `I've Found nothing.` |
| SP-05 | `/s` / `/search` success | `Successfully uploaded {x}` (Markdown; `{x}` as SP-02, without the Spotify-type suffix) |

Notes: the original message is **deleted** (`updatex.message.delete()`) when a `zip`/`1by1` button is tapped, before downloading. `spotdl` runs with per-user flags: `--output-format {mp3|m4a|flac|opus|ogg|wav}`, `--use-youtube` when enabled, `--path-template '{rndm}/{template}'`. Default path template for new users: `{artist}/{album}/{artist} - {title}.{ext}`. Default output format `mp3`, `use_youtube` `"False"` (string). If a URL contains `|`, the Spotify and YouTube URLs are swapped (`yturl|spurl` order) before running spotdl.

## 5.4 /settings — `bot.py` `settings()` + `callbacks()`

- Gated: if no MongoDB, the command silently does nothing (`else: pass`).
- Index screen text: `Select one of the settings below!` with inline keyboard (one button per row):

| Button text | `callback_data` |
|---|---|
| `Output Format` | `of` |
| `Use Youtube` | `uy` |
| `Path Template` | `pt` |

- **Output Format screen** (`of`): text (Markdown) `Current Output Format: \`{format}\`\n\nSelect one of the buttons below to change your output format.` Keyboard (2 per row, then Back):
  - Row 1: `MP3` (`mp3`) · `M4A` (`m4a`)
  - Row 2: `FLAC` (`flac`) · `OPUS` (`opus`)
  - Row 3: `OGG` (`ogg`) · `WAV` (`wav`)
  - Row 4: `Back` (`back`)
  - Tapping the already-selected format does nothing (early return).
- **Use Youtube screen** (`uy`): text (Markdown) `Is Using Youtube: \`{True|False}\`\n\nSelect one of the buttons below to change your is using youtube.` Keyboard: row 1 `True` (`True`) · `False` (`False`); row 2 `Back` (`back`).
- **Path Template screen** (`pt`): text (Markdown) `Current Path Template: \`{template}\`\n\nSelect one of the buttons below to change your current path template.` Keyboard: row 1 `Edit Current Path Template` (`edit_pt`); row 2 `Back` (`back`).
- **Edit Path Template flow** (`edit_pt`): deletes the settings message, then asks (via pyromod `bot.ask`, plain text prompt):
```
Send your custom path template.

Possible values:
{artist}
{artists}
{title}
{album}
{ext}
{playlist}

Example: `{artists}/{album}/{title} - {artist}.{ext}`
Note: "/" is used for making new directories.
```
  After the user replies, saves it and sends the Path Template screen again as a new message (same text + keyboard as `pt` above).
- **Back** (`back`): edits the message back to `Select one of the settings below!` with the index keyboard.

**Categories with NO UI in this ref:** now-playing card, queue display, lyrics view, inline-mode results, reply keyboards, thumbnails, download progress (no progress text at all — files are just sent when done), error messages beyond SP-03/SP-04, admin panels, provider selection, share confirmations.

---

# 6. lemonic-telegram-bot — `refs/lemonic-telegram-bot/` (TypeScript, grammy, HTML parse mode globally)

Spotify downloader via the Lemonic API. **Global parse mode is HTML** (`bot.api.config.use(parseMode("HTML"))`, `bot.ts`). Works in private + group chats. Commands registered in code: `start`, `help`, `settings` (all three route to the start handler — **/help and /settings show the same start text**), `search`. Plain-text `search …` is also matched in private chats (`privateChat.hears(/search ?((album|playlist|track) )?(.+)?/gim, …)`). Plain text = Spotify search by name; any URL entity = link handling. A forwarded message from @BotFather containing a bot token in a `code` entity triggers the clone flow.

## 6.1 /start (and /help, /settings — identical) — `handlers/start_command.ts`

- Plain /start text (no deep-link payload):
```
Hello, I'm Lemonic Bot. I can download songs from Spotify and other music platforms. Just send me a link or search for a song.
```
- Inline keyboard (1 row, 2 buttons):

| Button text | Action |
|---|---|
| `Clone Bot` | URL `https://telegra.ph/Create-Your-Own-Lemonic-Bot-A-Step-by-Step-Guide-12-16` |
| `Search for a song` | `switch_inline_query_current_chat: ""` (opens inline mode in current chat) |

- Deep-link `/start spotify----{id}`: downloads that Spotify track immediately (song / not-available flows below); no start text is shown.

## 6.2 /search help text — `constants.ts` `SEARCH_HELP_TEXT`

Shown when `/search` has no query:

```
You can search for tracks, albums and playlists.

Track search example:
<pre>search (track name)</pre><pre>search track (track name)</pre>
Album search example:
<pre>search album (album name)</pre>
Playlist search example:
<pre>search playlist (playlist name)</pre>

The search query is case-insensitive so you don't have to worry about captial and small letters. You can also search for tracks in inline mode.
```

(Sic: "captial".) Query prefixes: `search track …` / `search album …` / `search playlist …` (also bare `search …` = track). Search query is truncated to 50 chars; plain-text queries truncated to 100 chars.

## 6.3 Search results — `modules/methods/createSearchResults.ts` + `modules/methods/utils.ts`

- Result message text (`searchResultMessage`): `🔍 Search results for {track|album|playlist}: <b>{query}</b>` followed by blank line, then numbered lines `1. <b>{name}</b> - {artists}` (tracks), `1. <b>{name}</b> - {artists}` (albums), `1. <b>{name}</b> - {owner.display_name}` (playlists).
- No-results text: `No result found for {track|album|playlist} <b>{query}</b>` (empty inline keyboard).
- Inline keyboard: number buttons `1`…`n` (max 7 per page; 6 when paginating from offset>0), **4 per row**, `callback_data: "spotify----{id}----{track|album|playlist}"`. Pagination row appended after the numbers: `←` → `callback_data: "n----{query}----{t|a|p}-{offset}"` (only when offset>0) and `→` → `callback_data: "n----{query}----{t|a|p}-{offset}"` (only when more pages exist). Pagination edits the message in place (`editMessageText`).
- Plain-text (non-command) search with zero results: `No result found` (plain reply, `handlers/text_message.ts`).

## 6.4 Song delivery — `modules/methods/replyWithSong.ts`

- Sends the audio directly via `replyWithAudio(url)` — **no caption, no title/performer metadata on the first attempt, no progress message**.
- Fallback path (only when Telegram rejects the URL with `Bad Request: wrong file identifier/HTTP URL specified` or `Bad Request: failed to get HTTP URL content`): sends/edits a `Please wait...` message, then re-sends as an uploaded file with `title: {track name}`, `performer: {artists joined ", "}` (or show name for episodes), `duration: {duration_ms/1000}`. The `Please wait...` message is deleted afterwards (`finally` block).

## 6.5 Track not available — `modules/methods/replyWithSongNotAvailable.ts` + `constants.ts`

- Message text (`trackNotAvailableMessage`): `<b>{track name}</b> - {artists}` + newline + `This track will be added within 5 minutes.` (constant `TRACK_NOT_AVAILABLE_MESSAGE`).
- Inline keyboard: single button `Check Again` → `callback_data: "spotify----{id}----track"`. Tapping it while still unavailable answers the callback query with the toast (no alert): `Track is still being processed. Please try again later.` (constant `TRACK_NOT_AVAILABLE_TEXT`, `show_alert: false`, `cache_time: 10`).
- Invalid Spotify ID: message/edit text `Invalid Spotify ID` (constant `INVALID_SPOTIFY_ID_TEXT`).

## 6.6 Album & playlist views — `modules/methods/spotifyHandlers.ts` + `utils.ts`

Both send a **photo** (cover art resized via `https://wsrv.nl/?url={encoded}&w=1137&h=640&q=100&fit=contain&l=0&cbg=121212` — `create3x4Image`) with an HTML caption and one download button.

- Album caption (`albumMessage`):
```
<b><a href="{spotify album url}">{album name}</a></b>
<i>{artists, comma-separated}</i>

{label}
```
- Playlist caption (`playlistMessage`):
```
<b><a href="{spotify playlist url}">{playlist name}</a></b>
<i>{owner.display_name}</i>

{description}
```
- Album button: `Download {N} songs` → `callback_data: "spotify----{album id}----album-dl"`.
- Playlist button: `Download {N} songs` → `callback_data: "spotify----{playlist id}----playlist-dl"`.
- After a bulk download completes: `🎵 {album name} album downloaded` / `🎵 {playlist name} playlist downloaded` (plain reply). Individual tracks arrive as separate audios (silent, `disable_notification: true`).
- If the tapped message is part of a media group, the photo is swapped in place (`editMessageMedia`); otherwise the old message is deleted and a new photo message is sent.

## 6.7 Inline mode — `handlers/inline_query.ts`, `handlers/chosen_inline_result.ts`

- Trigger: any non-empty inline query; searches Spotify tracks (market `IN`, 10 per page, `next_offset` = offset+10, `cache_time: 3600`).
- Result type: **`documentPdf`** (grammy `InlineQueryResultBuilder.documentPdf`) per track — id = Spotify track id.
  - **Title:** `{track name}` (plain).
  - **Description:** `{artists, comma-separated}`.
  - **Thumbnail:** album image URL (`thumbnail_url`, with its height/width).
  - **Caption** (HTML): `<b>{track name}</b> - {artists}`.
  - **Button:** `Open in Lemonic Bot` → URL `https://t.me/{bot username}?start=spotify----{track id}`.
- Empty query: no results returned (handler returns early).
- **Chosen-inline-result upgrade:** when the user picks a result, if the Lemonic API has the file, the inline message media is replaced with the actual audio (`editMessageMediaInline` with `InputMediaBuilder.audio(url)`). Silently ignored errors: `Bad Request: MEDIA_EMPTY`, `Bad Request: failed to get HTTP URL content`.

## 6.8 Callback routing — `handlers/callback_query.ts`

`callback_data` format: `{provider}----{id}----{suffix}`, split on `----` (max 3 parts).

| `callback_data` | Action |
|---|---|
| `spotify----{id}----track` | Download track (or "still processing" toast, §6.5) |
| `spotify----{id}----album` | Show album photo card (§6.6) |
| `spotify----{id}----album-dl` | Download all album tracks |
| `spotify----{id}----playlist` | Show playlist photo card |
| `spotify----{id}----playlist-dl` | Download all playlist tracks |
| `n----{query}----{t\|a\|p}-{offset}` | Search pagination (edit in place) |
| anything else | Toast with alert: `Invalid provider: {suffix or provider}` (`INVALID_PROVIDER_TEXT = "Invalid provider: "`, `show_alert: true`, `cache_time: 10`) |

## 6.9 URL handling — `handlers/url_message.ts`

- Any message containing a URL entity: Spotify track/album/playlist links are handled; other music-platform links are resolved to Spotify via SongLink (`songlink.find_provider("spotify", url)`).
- While resolving an unknown URL: sends `Please wait...` (`PLEASE_WAIT_TEXT`); if unresolvable, edits that message to `No result found`.
- Spotify track link with no download available → not-available flow (§6.5).

## 6.10 Bot-clone flow — `handlers/bot_token_handler.ts`, `constants.ts` (`CLONE_BOT_BUTTON`)

Triggered by a private message forwarded from @BotFather containing the new bot's token inside a `` `code` `` entity.

| # | Verbatim text | When |
|---|---|---|
| L-01 | `I am a bot too!` | No `WEBHOOK_URL` configured |
| L-02 | `Bot cloned successfully` | Webhook set for the new token |
| L-03 | `An error has occurred when setting webhook: <code>{error.message}</code>\n\nPlease contact the bot developer or set the webhook manually to {WEBHOOK_URL}/bot{bot_token}` | Webhook set failed |
| L-04 | `Bot is already cloned` | Token already registered |
| L-05 | `Invalid message from @BotFather` | No token found in a `code` entity |

## 6.11 Errors — `modules/errors.ts`, `bot.ts`

| # | Verbatim text | When |
|---|---|---|
| L-06 | `Something went wrong` | Spotify error with no parseable message (default in `handleSpotifyError`); if the error JSON contains `"message": "…"`, that message is shown instead. Delivered as: edit of the tapped message, or callback-query alert (`show_alert: true`, `cache_time: 30`), or plain reply. |
| L-07 | `An error has occurred. Please try again later. If this issue persists, please contact the bot developer.` | Any uncaught handler error (wrapper in `bot.ts`; not sent for `chosenInlineResult` errors) |

**Categories with NO UI in lemonic:** now-playing card, queue display, lyrics view, download progress messages (none — only `Please wait...`), settings menus (the /settings command just shows /start), admin panels, reply keyboards, thumbnails beyond album art in inline results + album/playlist photo cards, stats views, provider selection menus (Spotify is the only provider; SongLink resolves other links to Spotify silently).

---

# 7. music-bot-navidrome — `refs/music-bot-navidrome/src/` (Python, python-telegram-bot v20+, HTML parse mode)

Private single-owner bot: paste a music link → Soulseek download → tagged files into a Navidrome library. **Auth is silent** — non-allowed users get no reply at all (`authorized` decorator returns without answering), except /start which explains. **Parse mode is HTML** wherever stated. This is the richest ref in the group for **progress rendering, cancel flows, lossy-fallback prompts, delete confirmations, inline modes (search / now-playing / lyrics / share / library / delete), and re-tag flows.**

## 7.1 Bot command list (set at startup, `bot.py` `_post_init`)

| Command | Description |
|---|---|
| `/help` | `Show all features` |
| `/scan` | `Trigger Navidrome library rescan` |
| `/sharescan` | `Trigger heavy slskd share rescan` |
| `/stats` | `Library statistics` |
| `/retag` | `Re-tag library from current Deezer + Last.fm metadata` |

(`/start` is handled but deliberately **not** in the command list.)

## 7.2 /start — `bot.py` `cmd_start`

### 7.2.1 Unauthorized user

```
This is a private bot for managing a Navidrome music library.

Your user ID: <code>{user_id}</code>

If you're the server admin, add this ID to <b>ALLOWED_USERS</b> in your bot configuration.

<a href="https://github.com/eeegoloauq/music-bot">GitHub</a>
```
(HTML, `disable_web_page_preview=True`.)

### 7.2.2 Authorized user

```
<b>Commands:</b>
/help — show all features
/scan — trigger Navidrome library rescan
/sharescan — trigger heavy slskd share rescan

Send a music link (Tidal, Spotify, Apple Music, Deezer, YouTube) to download.
Type <code>@{bot username}</code> in any chat for inline mode, or use the buttons below.
```

- Inline keyboard `_inline_shortcuts()` (attached to /start **and** /help):

| Row | Button text | Action |
|---|---|---|
| 1 | `🔎 Search` | `switch_inline_query_current_chat=""` |
| 1 | `📚 Library` | `switch_inline_query_current_chat="lib "` |
| 1 | `🗑 Delete` | `switch_inline_query_current_chat="del "` |
| 2 | `▶️ Playing` | `switch_inline_query_current_chat="np"` |
| 2 | `📝 Lyrics` | `switch_inline_query_current_chat="l"` |
| 2 | `🔗 Share` | `switch_inline_query_current_chat="s"` |
| 3 | `📤 Send playing to…` | `switch_inline_query="np"` (opens chat picker) |

- The /start message is **pinned** (silent pin, once — only if the currently pinned message is not already the bot's own).

### 7.2.3 Deep link `/start help` → runs /help instead.

## 7.3 /help — `bot.py` `cmd_help` (HTML + same shortcut keyboard as §7.2.2)

```
<b>Download</b>
Send a music link from Tidal, Spotify, Apple Music, Deezer, Shazam or YouTube. For other services, search by name.
Add <b>re</b> after the link to force re-download.
Every download and upload import carries a ✖ Cancel button until it finishes — tap it at any stage; tracks already saved stay.

<b>Inline mode</b>
<code>@{bot username} song name</code> — search Deezer
<code>@{bot username} np</code> — now playing as audio
<code>@{bot username} s</code> — share link for current track
<code>@{bot username} l</code> — lyrics for current track
<code>@{bot username} lib name</code> — search library
<code>@{bot username} del name</code> — delete an album or a track

<b>Commands</b>
/scan — trigger Navidrome library rescan
/sharescan — trigger heavy slskd share rescan
/stats — library statistics
/retag — refresh tags on every album from Deezer + Last.fm (dry-run, then <code>/retag confirm</code>)
```

## 7.4 /stats — `bot.py` `cmd_stats` (plain text, no parse mode)

```
Artists: {n}
Albums: {n}
Tracks: {n}
Size: {n.n} GB
```

## 7.5 /scan, /sharescan — `bot.py` `_trigger_scan`

Reply is one of these status lines (returned by `_trigger_scan`, sent as the /scan reply; same lines are appended to delete/retag/download outcomes):

| Verbatim line | When |
|---|---|
| `Library scan triggered.` | Navidrome scan started |
| `Library scan not configured (NAVIDROME_USER/NAVIDROME_PASS not set).` | No Navidrome creds |
| `Library scan failed: wrong credentials.` | Navidrome auth error |
| `Library scan failed.` | Other scan error |
| `slskd share scan triggered.` | `/sharescan` (immediate) |
| `slskd share scan scheduled in ~{mins}m.` | After delete / re-tag (delayed quiet-period scan) |
| (empty) | Normal downloads: no slskd note |

## 7.6 Download flow — status messages, progress, final reports

### 7.6.1 Entry / queueing status lines (`bot.py`)

| Verbatim text | When |
|---|---|
| `Resolving link…` | Link received, resolving to Deezer album/track id |
| `❌ Failed to resolve link: {error, ≤120 chars, URLs stripped to <url>}` | Resolver threw |
| `❌ Could not resolve link. Search by name instead: @{bot username} artist title` | Resolver returned nothing |
| `Queued — will start after the current download finishes` | Download semaphore busy (with ✖ Cancel button) |
| `Fetching album info…` | Album download starting (with ✖ Cancel button) |
| `Fetching track info…` | Track download starting (with ✖ Cancel button) |
| `❌ Failed to fetch album info: {error}` | Album metadata fetch failed |
| `❌ Failed to fetch track info: {error}` | Track metadata fetch failed |
| `❌ Download failed: {error}` | Soulseek download threw (album or track) |
| `Already queued.` | Same album/track already in flight (sent as a fresh reply; the duplicate run is dropped) |

### 7.6.2 ✖ Cancel — `ActiveRun` (`bot.py`)

- Every download/upload status message carries one inline button: `✖ Cancel` → `callback_data: "cancel:{12-hex run id}"`.
- Callback-query answers (toasts):
  - Run gone: `Nothing to cancel — already finished.` (button row removed from the message)
  - Wrong chat: `Only the chat that requested this can cancel it.` (alert)
  - Already cancelling: `Cancelling…`
  - Accepted: `Cancelling…`
- Final cancel texts (`cancel_text()`):
  - Track, saved already: `✖ Cancelled — the track was already saved`
  - Track, nothing saved: `✖ Cancelled — nothing was downloaded`
  - No total known: `✖ Cancelled — nothing was downloaded yet`
  - Album/upload with total: `✖ Cancelled — {saved}/{total} tracks were already saved` (+ `; the rest stays in uploads staging` for uploads)
  - With a restore note: `✖ Cancelled — {note}` (e.g. `the original copy was restored` / `restoring the original copy FAILED — check the library`)

### 7.6.3 Live album progress — `reporting.py` `AlbumProgress.render()` (HTML, single edited message)

Header: `<b>{artist} — {title}</b>`

- **Search phase:**
```
<b>{artist} — {title}</b> · {n} tracks
Searching Soulseek…
  «{query ≤40 chars}» — searching… | {files} files / {peers} peers | nothing
  [per-track prefix: {track ≤24 chars}: «{query}» — …]
  pacing — next search in ~{n}s
```
  (Last 4 searches shown.)
- **Download phase:**
```
<b>{artist} — {title}</b>
peer {peer} · {covered}/{total} tracks · {quality} · score {score:.0f}
```
  or, when no full-album peer: `no full-album peer — matching track by track`
  + `{skipped} already in library` (when >0)
  + `searching{ for {track ≤30 chars}}…` (while a fallback search runs)
  + `pacing — next search in ~{n}s`
  + blank line, then per-track checklist (≤28 tracks; above that, a counter line):
    - pending: `▫️ {title ≤48 chars}`
    - active, transferring: `⬇️ {title} · {peer} · {pct:.0f}% · {speed}` (speed: `{n.n} MB/s` or `{n} KB/s`)
    - active, remotely queued: `⏳ {title} · {peer} · waiting for peer · queue #{position} · {duration}` (position may be `position unavailable`)
    - done: `✅ {title}`
    - failed: `❌ {title} — {human reason}`
  + blank line + ETA line: `{done}/{total} saved` (+ ` · {failed} failed`, + ` · ~{duration} left` when ETA ≥ 5 s)
- Collapsed form (>28 tracks): `✅ {done} done · ❌ {failed} failed · ▫️ {queued} queued` then only the active lines.
- Human failure reasons (`humanize_failure` mapping): `waiting in the peer queue expired` · `transfer timed out` · `peer accepted but never sent the file` · `peer rejected the transfer` · `transfer was cancelled` · `transfer failed on the peer's side` · `peer refused the download request` · `transfer finished but the file never appeared` · `every matching peer had already failed on this album` · `all matching peers failed` · else the raw reason, or `unknown error`.

### 7.6.4 Live track progress — `reporting.py` `TrackProgress.render()` (HTML)

- Searching: `<b>{artist} — {title}</b>` + `Searching Soulseek…` + `  «{query}» — searching… / {files} files / {peers} peers / nothing` lines + pacing line.
- Matched: `<b>{artist} — {title}</b>` + `peer {peer} · {quality} · match {score:.0f}` (+ ` · {n} copies` when >1) + `{pct:.0f}% · {speed}` (speed omitted when 0) — or, when remotely queued: `⏳ Waiting for peer · queue #{position} · {duration}` (or `position unavailable`).

### 7.6.5 Final album report — `reporting.py` `render_album_final()` (HTML, ≤1024 chars — usually a **photo caption on the cover art** `cover.jpg` if present, else an edit of the status message)

```
{✅|⚠️|❌} <b>{artist} — {title}</b>
{downloaded}/{wanted} saved · {skipped} already in library · {format} · lyrics {n}/{downloaded} · {elapsed}
from {peer} ×{n}, {peer} +{m} more peers

❌ {n} not downloaded:
· {title ≤40} — {human reason}
   (or, >5 failures, grouped: · {reason}: {title}, {title}, {title} +{n} more)

{scan note}

{share url}
```
- Icon rule: `✅` all saved · `⚠️` partial · `❌` all failed.
- Already-in-library case (0 downloaded, 0 failed): `<b>{artist} — {title}</b>\nAlready in library.` (+ share URL if any).
- Force re-download incomplete (original restored): `Kept existing: {artist} — {title}\nRe-download was incomplete ({got} ok, {miss} unavailable), so the original copy was restored.`
- Upload import, already in library: `<b>{artist} — {title}</b>\nAlready in library — the uploaded copy was dropped as a duplicate.` (+ share URL).
- Upload leftovers: `\n\n⚠️ Didn't match any track, left in staging: {file}, {file}, {file} +{n} more` (first 3 filenames).

### 7.6.6 Final track report — `reporting.py` `render_track_final()` (HTML, ≤1024 chars)

```
✅ <b>{artist} — {title}</b>
{format} · from {peer} · {elapsed}

{scan note}

{share url}
```
- Already-in-library case: `<b>{artist} — {title}</b>\nAlready in library.` (+ share URL).
- No FLAC / no acceptable lossy: `❌ No FLAC or acceptable lossy fallback for {artist} — {title}`.
- Lossy search couldn't run: `Soulseek search couldn't run for {artist} — {title} ({rate-limited|search error}). Try again in a few minutes.`
- Lossy download failed: `❌ Lossy download failed: {error}`.
- Lossy download in progress (status edit): `Downloading: {artist} — {title} ({summary})` where summary = `mp3 {best}kbps from {n} peer{s}` and/or `m4a from {n} peer{s}`.

### 7.6.7 Lossy-fallback prompt — `bot.py` `_offer_mp3_fallback`

Shown when a track has no FLAC on Soulseek but mp3/m4a candidates exist. Edits the status message; prompt expires after **5 min** (`Prompt expired (5 min).` on a late tap; toast `Prompt expired.`).

- Text:
```
FLAC not found for {artist} — {title}

Lossy fallback available: {summary}
```
  where `{summary}` = e.g. `mp3 320kbps from 5 peers` / `m4a from 2 peers` / `mp3 320kbps from 5 peers · m4a from 2 peers`.
- Inline keyboard (3 rows):
  - `✓ Accept {summary}` → `callback_data: "lossy:accept:{12-hex id}"`
  - `✗ Skip` → `callback_data: "lossy:skip:{12-hex id}"`
  - `✖ Cancel` → `callback_data: "cancel:{12-hex prompt run id}"` (acts as Skip)
- Skip outcome (edit): `Skipped — FLAC not available for {artist} — {title}`
- Cancel-during-prompt outcome (edit): `✖ Cancelled — FLAC not available for {artist} — {title}, no fallback taken`
- Lossy accept while already in flight: `Already queued.`

## 7.7 Delete flow — `bot.py` `_handle_delete`, `_handle_delete_callback`, `_delete_album`, `_delete_track`

- Trigger: sending a message starting with `delete:` (produced by inline `del` mode, §7.9.5) followed by a path relative to the music dir.
- Confirm prompt: `Delete {Artist — Album} ({n} tracks)?` for albums, `Delete {Artist — Album — Title}?` for tracks. Inline keyboard (1 row): `🗑 Delete` → `callback_data: "rm:yes:{12-hex id}"` · `✖ Cancel` → `callback_data: "rm:no:{12-hex id}"`.
- Toasts: unauthorized user `Not allowed.`; expired prompt `Prompt expired.` (button row removed).
- Outcomes (message edit):
  - `Cancelled.`
  - `Changed since the question, not deleted: {path}`
  - `Deleted: {label}\n{scan note}`
  - Path errors (plain replies): `Invalid path.` / `Cannot delete top-level directories.` / `Not found: {path}`

## 7.8 Re-tag flow — `bot.py` `cmd_retag`, `_format_retag_summary`

- Start (status message): `Scanning library…`
- Dry-run progress (edited, HTML, throttled ≥4 s):
```
<b>Dry-run</b> · {idx} / {total} · {pct}% · ETA {eta}
last: {artist} / {album ≤60 chars} · {already canonical|Deezer match|Last.fm only|skipped}
to update <b>{n}</b> · genre-only <b>{n}</b> · skipped <b>{n}</b>
```
  ETA format: `{s}s` / `{m}m {s}s` / `{h}h {m}m`; `—` when none.
- Dry-run summary (HTML):
```
<b>Scan complete</b> · {total} albums · {elapsed}

<b>{n}</b> will update — {full} full + {lastfm} genre-only   (or just <b>{n}</b> will update)
<b>{n}</b> already canonical
<b>{n}</b> not found on Deezer

<b>Top updates</b> ({shown} of {n}):
  {Artist / Album ≤70 chars} — {change labels, ≤4, comma-separated}…
  +{n} more

<b>Not found</b> ({shown} of {n}):
  {Artist / Album — note}
  +{n} more

Send <code>/retag confirm</code> to apply or <code>/retag stop</code> to discard.
```
  Change labels: `comment` · `genres` · `folder rename` · `artist folder rename` · `track count` · `release date` · `ReplayGain` · `casing` · `genres only`.
- Apply progress (edited, ≥5 s):
```
<b>Re-tag apply</b> · {idx} / {total} · {pct}% · ETA {eta}
last: {artist} / {album} · {n} files
total: <b>{written}</b> files updated · <b>{skipped}</b> skipped
```
- Apply summary:
```
<b>Re-tag complete</b> · {n} albums · {elapsed}

<b>{n}</b> files updated · <b>{n}</b> skipped (no track match)

<b>{n}</b> album-level failures:
  {path} — {error}
  +{n} more

{scan note}
```
- Control replies: `No re-tag session pending.` · `Re-tag session dropped.` · `No re-tag session to confirm. Run /retag first.` · `Library changed since the scan ({reason}) — run /retag again.` · `Re-tag session expired (2 hours). Run /retag again.` · `Re-tag already running.` (×2 paths) · start status `Applying re-tag…` · failure edits `Re-tag failed: {error}` / `Apply failed: {error}`.

## 7.9 Inline mode — `src/inline.py` (`handle_inline_query`)

Auth: non-allowed users get **no results at all** (silent). Modes are dispatched on the query text:

| Query | Mode |
|---|---|
| (empty, or 1–2 chars, no keyword) | Hint button (§7.9.1) |
| `np` | Now playing as audio (§7.9.2) |
| `s` | Share link for current track (§7.9.3) |
| `l` | Lyrics for current track (§7.9.4) |
| `del …` / `del` | Delete search (§7.9.5) |
| `lib …` (≥2 chars) | Library search via Navidrome (§7.9.6) |
| ≥3 chars | Deezer search (§7.9.7) |

### 7.9.1 Hint

Empty results + a button above the results list (`InlineQueryResultsButton`): text `np  s  l  lib  del — or type to search`, `start_parameter="help"` (opens `/start help`).

### 7.9.2 Now playing (`np`) — `InlineQueryResultCachedAudio`

- Returns the currently playing track as a **cached audio** result (file uploaded to Telegram once, then reused by `file_id`; oversize >50 MB tracks are skipped and cached as failed for 1 h). No title/description of its own — the audio's own `title`/`performer`/`duration`/thumbnail (cover art) are used. Empty result set when nothing is playing or the upload fails.

### 7.9.3 Share (`s`) — `InlineQueryResultArticle`

- Title: `{track title}`; Description: `{artist} — {album}`; Thumbnail: cover art URL (320×320).
- Sent message text: `{artist} — {title}\n{share url}`.
- Empty result when no share URL can be created / nothing playing.

### 7.9.4 Lyrics (`l`) — `InlineQueryResultArticle`

- Lyrics sourced from the local file's `lyrics`/`unsyncedlyrics` (FLAC) or `©lyr` (M4A) tag, else lrclib (`fetch_lyrics`); LRC timestamps stripped (`[mm:ss.xx]` prefixes removed); truncated to 4000 chars + `\n...`.
- Title: `Lyrics: {track title}`; Description: `{artist} — {first 100 chars of lyrics}`.
- Sent message text: `{artist} — {title}\n\n{plain lyrics}`.
- Empty result when no lyrics found / nothing playing.

### 7.9.5 Delete (`del …`) — articles that send `delete:{path}`

- Albums (max 5): Title `{album} ({n} tracks)`, Description `{artist}`, Thumbnail = Deezer cover; sent message `delete:{relative album path}`.
- Tracks (max 10): Title `{title}`, Description `{artist} — {album} · {m}:{ss}`, Thumbnail = Deezer cover; sent message `delete:{relative track path}`.
- Query <2 chars or no matches: empty results.

### 7.9.6 Library search (`lib …`) — Navidrome

- Albums: Title `{name} ({songCount} tracks)`, Description `{artist}`, Thumbnail = Deezer cover; sent message = share URL if creatable, else `{artist} — {name}`.
- Songs: Title `{title}`, Description `{artist} — {album} · {m}:{ss}`, Thumbnail = Deezer cover; sent message = share URL if creatable, else `{artist} — {title}`.

### 7.9.7 Deezer search (≥3 chars)

- 3 albums + 5 tracks per page (`next_offset` set when a full page was returned); `cache_time: 30`.
- Albums: Title `{title} ({n} tracks)`, Description `{artist}`, Thumbnail = Deezer cover; sent message `https://www.deezer.com/album/{id}`.
- Tracks: Title `{title}`, Description `{artist} — {album} · {m}:{ss}`, Thumbnail = Deezer cover; sent message `https://www.deezer.com/track/{id}`.

## 7.10 Upload intake (local files) — `uploads.py`, `upload_import.py`

- Intake status: `📦 {name}: {n} audio file(s) received. Identifying release…` (with ✖ Cancel button).
- Queued import: `📦 {name}: release identified. Import queued until the current download finishes…`
- Importing: `📦 Importing upload: {artist} — {title}…`
- Identify failure: `❓ {name}: couldn't match this to a release — {diagnosis}. Files are kept in uploads staging; nothing was filed.` where diagnosis is e.g. `tags: artist+album; tag search "Artist — Title" — no Deezer hit` or `tags: none; name "X" has no "Artist - Title" shape — nothing to search Deezer with`.
- Import failure: `❌ Upload import failed: {error}. Files kept in staging.`
- Rejection (non-audio / bad entry): `❌ Upload rejected: {name}\nReason: {error}\nMoved to {_REJECTED}/ — fix and re-drop.`

## 7.11 Resume after restart — `bot.py` `_resume_pending`

- On a restarted download's old status message: `🔁 Bot restarted — resuming this download…`
- After max resume attempts: `A download kept dying across bot restarts — giving up on it. Re-send the link to try again.`

## 7.12 Docs-only (NOT in code — do not port as implemented behaviour)

`docs/multi-user.md` describes a **planned** invite flow that does not exist in `bot.py`: owner runs `/invite` → one-time code; newcomer sends `/start <code>`; bot replies with server URL, credentials and a client link, then deletes that message after a few minutes; `/revoke` deactivates the user. None of `/invite`, `/revoke`, or the code handling is implemented. Noted here so port workers do not mistake the doc for shipped UI.

**Categories with NO UI in music-bot-navidrome:** now-playing *card* in chat (now-playing exists only as an inline `np` audio result), queue display in chat (no /queue — downloads are serialized, one at a time, with the `Queued — will start after the current download finishes` line), settings menus (no /settings — behaviour is via env vars), admin panels, provider selection menus (link type decides the resolver silently), reply keyboards, share/download confirmations beyond the share URL lines in final reports.

---

# Cross-ref notes for port workers

1. **Progress rendering (richest source):** music-bot-navidrome `reporting.py` (`AlbumProgress`/`TrackProgress`, final reports) is the only ref with live-edited checklists, per-track states, peer/queue/speed/ETA lines and humanized failure reasons. openradiobot has only single-line now-playing notices; the downloaders have only a `⬇️ Downloading...` / `Please wait...` line.
2. **Cancel pattern:** music-bot-navidrome `✖ Cancel` (`cancel:{id}`) with per-run final tally texts is the only cancellable-flow pattern in the group.
3. **Lossy-fallback prompt:** music-bot-navidrome `✓ Accept {summary}` / `✗ Skip` (`lossy:accept|skip:{id}`) is the only quality-choice prompt in the group; Spotify-Loader's format choice (`mp3/m4a/flac/opus/ogg/wav`) is the only output-format picker.
4. **Settings patterns:** beetube-bot (two toggle screens with `✅ Enabled`/`❌ Disabled` + `🔙 Back` to `index_setting`) and Spotify-Loader (three screens + free-text path-template edit) are the only settings UIs; navidrome has none.
5. **Inline-mode patterns:** navidrome (mode keywords `np`/`s`/`l`/`lib`/`del` + `switch_inline_query_current_chat` shortcut buttons) and lemonic (`documentPdf` results + `Open in Lemonic Bot` deep link + chosen-result media upgrade) are the only inline implementations.
6. **Reply keyboards:** only beetube-bot has one (3-row category/settings keyboard). No other ref uses a reply keyboard.
7. **Rich-message conventions observed:** beetube-bot mixes Markdown (start/about/settings) and HTML (results); lemonic is HTML-global; navidrome is HTML; openradiobot + Song-Downloader + Spotify-Loader are Markdown. Bold artist — title headers, `<b>…</b> — {artists}` result lines, and emoji-prefixed status lines are the recurring patterns.
8. **wavetune contributes web/Mini-App UI only** (search placeholders, section headings, icon-only player controls) — no Telegram strings to port.

---

*End of Group C inventory. Sources: all file paths are relative to `~/workspace/parlay/refs/`. Line numbers cited inline per element where the file is short; for `music-bot-navidrome/src/bot.py` (2,000+ lines) the enclosing function name is cited instead.*
