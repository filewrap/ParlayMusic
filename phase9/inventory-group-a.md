# Parlay Phase 9 — UI/UX Inventory, Group A (7 of 21 refs)

**Scope:** every user-visible Telegram UI/UX element harvested from 7 refs under `~/workspace/parlay/refs/`:
`anonx-cpp`, `NoxBeats`, `TinaMusic`, `YukkiMusic-Go`, `ArcMusic`, `TgMusicBot`, `Music163bot-Go`.
**Date:** 2026-11-03. **Method:** read-only. Locale files are reproduced **verbatim in full** in the Appendices at the end of this file (byte-for-byte copies). The body below is the curated, categorised inventory with exact strings, button layouts and `callback_data` values, plus file:line provenance for every element.
**Not touched:** no `.env`, no code edits, no pushes, no core starts.

## Conventions / how to read this file

- `{0}`, `{1}` … = NoxBeats positional placeholders. `{name}`, `{title}`, `{user}` … = Tina/Yukki/Arc named placeholders.
- Yukki/Arc use `{emoji:🎵}` tokens: the loader substitutes the emoji at render time (see `YukkiMusic-Go/internal/locales/emoji.go`). Where a locale value shows `{emoji:X}`, the rendered text is literally `X`.
- TinaMusic uses a stylised small-caps font in many strings (e.g. `ᴧᴅᴅ ᴍє ᴛσ ʏσυꝛ ɢꝛσυᴘ`); the Unicode characters below are copied exactly — do not "correct" them to ASCII.
- NoxBeats likewise uses small-caps Unicode (e.g. `ᴀᴅᴅ ᴍᴇ ᴛᴏ ʏᴏᴜʀ ɢʀᴏᴜᴘ`).
- Rich-message dialects: NoxBeats/TgMusicBot use Bot-API HTML (`<b>`, `<i>`, `<u>`, `<code>`, `<a href>`, `<blockquote>`); Tina/Yukki/Arc use the same HTML **plus** TDLib rich elements (`<h2>/<h3>/<h4>`, `<p>`, `<aside>`, `<footer>`, `<table bordered striped compact>`, `<tg-button>`, `<tg-spoiler>`, `<details>/<summary>`, `<blockquote expandable>`); anonx-cpp uses Markdown (`**bold**`, `` `code` ``) via TDLib; Music163bot-Go uses Bot-API HTML.
- "No UI for this category" is stated explicitly per ref where true.
- Callback data is quoted exactly, including spaces (NoxBeats uses space-separated `controls <action> <chat_id>`; Yukki/Arc/Tina use colon-separated `room:<chat_id>:<action>`).

## Cross-ref quick map

| Feature | NoxBeats | TinaMusic | YukkiMusic-Go | ArcMusic | TgMusicBot | Music163bot-Go | anonx-cpp |
|---|---|---|---|---|---|---|---|
| /start PM + group | yes | yes | yes | yes | yes | no start text* | /start = /help |
| /help menu w/ categories | yes (9 cats) | yes (4 cats) | yes (4 cats) | yes (4 cats) | yes (6 cats) | no | yes (flat list) |
| Now-playing card | photo or text + controls | text + player markup | photo or text + progress bar btn | text + player markup | text + controls | audio file + caption | plain text |
| Queue display | `queue_curr`/`queue_item` | header + pending list | rich table + delete btns + show-more | plain list, >10 via paste link | plain list, max 14 shown | n/a (no VC) | plain list, max 10 |
| Lyrics | **none** | **none** | **none** (grep hits are false positives) | **none** | **none** | **yes — full lyric view** | **none** |
| Search results | via /play flow | via /play flow | via /play flow | via /play flow | via /play flow | **yes — numbered pages** | none |
| Download progress | `dl_progress` %/speed/ETA | cancel btn only | `download_progress` %/speed/ETA/elapsed | cancel btn only | "🔍 Searching and downloading..." | status-line progress | none |
| Settings panel | 3-row inline panel | 9-row inline panel | 8-row inline panel | **no panel — commands only** | 6-row + clean rows panel | n/a | none |
| Inline mode | **none** | **none** | **none** | **none** | **none** | **yes — full** | **none** |
| Reply keyboards | none | none | none | none | none | none | none |
| Channel play | /cplay family | /cplay family | /cplay family | /cplay family | n/a | n/a | n/a |
| Playlists (personal) | yes (/playlist…) | no | no | no | yes (/createplaylist…) | n/a (cache, not playlist) | no |
| RTMP /stream | yes | yes | no (refuses RTMP VC) | yes | no | n/a | no |
| Privacy text | `privacy_text` (1 para) | no locale key | full rich privacy page | no locale key | /privacy (see TgMusicBot §) | no | no |

\* Music163bot-Go has no /start handler text; its entry points are `/music`, `/netease`, `/search`, `/lyric`, `/program`, `/recognize`, `/status`, `/about` (+ admin `/login`, `/rmcache`, `/reload`, `/setcachechat`).

---

# 1. NoxBeats — `refs/NoxBeats/` (C++20, Bot API)

**Primary source:** `strings/en.json` (280 lines, 278 keys — reproduced verbatim in Appendix A).
**Keyboard source:** `Bad/ui/buttons.cpp`, `Bad/cmd/play_cb.cpp`, `Bad/cmd/settings.cpp`, `Bad/cmd/playback.cpp`, `Bad/cmd/queue_ops.cpp`, `Bad/cmd/tools.cpp`, `Bad/core/calls.cpp`.
**Style:** every string in small-caps Unicode; HTML parse mode. Start/stats/ping are sent as **photos** (`config().START_IMG` / `PING_IMG`) with the text as caption; now-playing is a photo when a thumbnail exists, else text (`Bad/core/calls.cpp:106-113`).

## 1.1 /start

- PM (`start_pm`, `strings/en.json:118`): verbatim —
  `ʜᴇʏ {0},\nᴛʜɪs ɪs {1} !\n\nᴀ ᴍᴜsɪᴄ ᴘʟᴀʏᴇʀ ʙᴏᴛ ᴡɪᴛʜ sᴏᴍᴇ ᴀᴡᴇsᴏᴍᴇ ᴀɴᴅ ᴜsᴇғᴜʟ ғᴇᴀᴛᴜʀᴇs.\n\n<b><i>ᴄʟɪᴄᴋ ᴏɴ ᴛʜᴇ ʜᴇʟᴘ ʙᴜᴛᴛᴏɴ ғᴏʀ ᴍᴏʀᴇ ɪɴғᴏ.</i></b>`
  Sent as photo caption with `start_key` (`Bad/cmd/settings.cpp:38`).
- Group (`start_gp`, `strings/en.json:119`): verbatim —
  `ʜᴇʏ,\nᴛʜɪs ɪs {0}\n\n<u><b>ᴀ ᴍᴜsɪᴄ ᴘʟᴀʏᴇʀ ʙᴏᴛ ᴡɪᴛʜ sᴏᴍᴇ ᴀᴡᴇsᴏᴍᴇ ᴀɴᴅ ᴜsᴇғᴜʟ ғᴇᴀᴛᴜʀᴇs.</b></u>`
- Start keyboard (`Bad/ui/buttons.cpp`, `start_key(L, is_private)`):
  - Row 1: `[ᴀᴅᴅ ᴍᴇ ᴛᴏ ʏᴏᴜʀ ɢʀᴏᴜᴘ]` → URL `https://t.me/<bot>?startgroup=true`
  - Row 2: `[ʜᴇʟᴘ]` → callback `help` (bare, no suffix)
  - Row 3: `[sᴜᴘᴘᴏʀᴛ]` → URL SUPPORT_CHAT, `[ᴄʜᴀɴɴᴇʟ]` → URL SUPPORT_CHANNEL
  - Row 4 (private only): `[sᴏᴜʀᴄᴇ]` → URL SOURCE_URL
  - Row 4 (group only): `[ʟᴀɴɢᴜᴀɢᴇ]` → callback `language`
- `/start help` (deep link arg `help`) opens the help menu directly (`Bad/cmd/settings.cpp:33`).

## 1.2 /help — full text

- Menu text (`help_menu`, `en.json:52`): `<b>ᴄʟɪᴄᴋ ᴛʜᴇ ʙᴜᴛᴛᴏɴs ʙᴇʟᴏᴡ ᴛᴏ ɢᴇᴛ ɪɴғᴏʀᴍᴀᴛɪᴏɴ ᴀʙᴏᴜᴛ ᴍʏ ᴄᴏᴍᴍᴀɴᴅs.</b>\n\n<i><b>ɴᴏᴛᴇ:</b> ᴀʟʟ ᴄᴏᴍᴍᴀɴᴅs ᴄᴀɴ ʙᴇ ᴜsᴇᴅ ᴡɪᴛʜ /</i>`
- Help keyboard (`Bad/ui/buttons.cpp`, `help_markup`): 9 category buttons, 3 per row, callback `help <cat>`, then a bottom row:
  - Row 1: `[ᴀᴅᴍɪɴs]` `help admins` · `[ᴀᴜᴛʜ]` `help auth` · `[ʙʟᴀᴄᴋʟɪsᴛ]` `help blist`
  - Row 2: `[ʟᴀɴɢᴜᴀɢᴇ]` `help lang` · `[ᴘɪɴɢ]` `help ping` · `[ᴘʟᴀʏ]` `help play`
  - Row 3: `[ǫᴜᴇᴜᴇ]` `help queue` · `[sᴛᴀᴛs]` `help stats` · `[sᴜᴅᴏᴇʀs]` `help sudo`
  - Row 4: `[ʜᴏᴍᴇ]` `help home` · `[ᴄʟᴏsᴇ]` `help close`
  - Category view adds a back row: `[ʙᴀᴄᴋ]` `help back` · `[ʜᴏᴍᴇ]` `help home` · `[ᴄʟᴏsᴇ]` `help close`. "Close" deletes the message (and its reply-to). "Home" returns to the start card.
- Category texts (verbatim, keys `help_admins`, `help_auth`, `help_blist`, `help_lang`, `help_ping`, `help_play`, `help_queue`, `help_stats`, `help_sudo` — see Appendix A lines for each; the full texts are in the appendix). `help_play` additionally documents `-f` force, `-v` video, `/autoplay`, `/thumbnail`, `/fplay`, `/forceplay`, `/fvplay`, Spotify links, `/cplay` family, `/addplaylist`, `/playlist`, `/rmplaylist`, `/playplaylist [-s] [-v]`, and Spotify/Apple Music/JioSaavn/SoundCloud links. `help_sudo` documents `/ac`, `/activevc`, `/broadcast` (flags `-ɴᴏᴄʜᴀᴛ`, `-ᴜsᴇʀ`, `-ᴄᴏᴘʏ`), `/eval`, `/logs`, `/logger`, `/restart`, `/addsudo`, `/rmsudo`, `/maintenance`, `/autoleave`, `/json`, `/speedtest`, `/blacklisted`, `/setrtmp`, `/bug`, `/privacy`.

## 1.3 Now-playing card

- Started streaming (`play_media`, `en.json:106`):
  `<u><b>| sᴛᴀʀᴛᴇᴅ sᴛʀᴇᴀᴍɪɴɢ</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href="{0}">{1}</a>\n\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {2} ᴍɪɴ\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {3}`
  Sent via `Bad/core/calls.cpp:106` as photo (track thumbnail, or `DEFAULT_THUMB` for YouTube tracks — `Bad/cmd/playback.cpp:199-204`) with caption, else plain text. `/player` re-sends this same card and deletes the old card message.
- Queued (`play_queued`, `en.json:109`):
  `<u><b>ᴀᴅᴅᴇᴅ ᴛᴏ ǫᴜᴇᴜᴇ: {0}</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href="{1}">{2}</a>\n\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {3} ᴍɪɴ\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {4}`
  With one button: `[ᴘʟᴀʏ ɴᴏᴡ]` (`play_now`) → callback `controls force <chat_id> <track_id>` (`Bad/ui/buttons.cpp`, `play_queued`).
- Player controls (`Bad/ui/buttons.cpp`, `controls(chat, status, timer, remove)`):
  - Row 1 (only if a status/timer string is set): `[<status text>]` → callback `controls status <chat_id>` (a no-op that just answers the callback — tapping it does nothing).
  - Row 2: `[▷]` `controls resume <chat>` · `[II]` `controls pause <chat>` · `[⥁]` `controls replay <chat>` · `[‣‣I]` `controls skip <chat>` · `[▢]` `controls stop <chat>`
  - Row 3 (player card only): `[ᴀᴜᴛᴏᴘʟᴀʏ | ᴏɴ]` / `[ᴀᴜᴛᴏᴘʟᴀʏ | ᴏғғ]` → `controls autoplay <chat>` · `[ᴛʜᴜᴍʙ | ᴏɴ]` / `[ᴛʜᴜᴍʙ | ᴏғғ]` → `controls thumb <chat>`
  - Note the **separate** queue-message toggle keyboard (`queue_markup`): a single button `[<sᴛʀᴇᴀᴍ ᴘᴀᴜsᴇᴅ|ᴘʟᴀʏɪɴɢ>]` → `controls pause|resume <chat> q` (trailing ` q` marks it as a queue-message action).
  - Callback answers (toasts, `Bad/cmd/play_cb.cpp`): autoplay on → `ᴀᴜᴛᴏᴘʟᴀʏ ᴏɴ: ʀᴇʟᴀᴛᴇᴅ sᴏɴɢs ᴡɪʟʟ ᴋᴇᴇᴘ ᴘʟᴀʏɪɴɢ` / off → `ᴀᴜᴛᴏᴘʟᴀʏ ᴏғғ`; thumb on → `ᴛʜᴜᴍʙɴᴀɪʟ ᴇɴᴀʙʟᴇᴅ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs` / off → `ᴛʜᴜᴍʙɴᴀɪʟ ᴅɪsᴀʙʟᴇᴅ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs`. Pause/resume/skip/replay/stop edit the card in place: the action line is appended as `\n\n<blockquote>{reply_text}</blockquote>` (previous blockquote stripped first); skip/replay/stop instead send the reply text as a new message and delete the card. `play_next` (force) uses message `» ʜᴏʟᴅ ᴏɴ...\n\nᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ɴᴇxᴛ ᴍᴇᴅɪᴀ ғʀᴏᴍ ᴛʜᴇ ǫᴜᴇᴜᴇ.`
- Action confirmations (all `en.json`): pause → `<b>sᴛʀᴇᴀᴍ ᴘᴀᴜsᴇᴅ ʙʏ</b> {0}`; resume → `<b>sᴛʀᴇᴀᴍ ʀᴇsᴜᴍᴇᴅ ʙʏ</b> {0}`; replay → `<b>sᴛʀᴇᴀᴍ ʀᴇᴘʟᴀʏᴇᴅ ʙʏ</b> {0}`; skip → `<b>sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ ʙʏ</b> {0}`; stop → `<b>sᴛʀᴇᴀᴍ ᴇɴᴅᴇᴅ ʙʏ</b> {0}`; seek → `<b>sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ {0} ᴀɴᴅ sᴛᴀʀᴛᴇᴅ ғʀᴏᴍ {1} sᴇᴄᴏɴᴅs ʙʏ</b> {2}`. Button-state labels: `paused` = `sᴛʀᴇᴀᴍ ᴘᴀᴜsᴇᴅ`, `replayed` = `sᴛʀᴇᴀᴍ ʀᴇᴘʟᴀʏᴇᴅ`, `skipped` = `sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ`, `stopped` = `sᴛʀᴇᴀᴍ ᴇɴᴅᴇᴅ`, `playing` = `ᴘʟᴀʏɪɴɢ`.
- Progress: the player card has **no progress-bar button** (unlike Tina/Yukki/Arc). Progress is shown only inside download messages (see 1.6) and the timer/status row of `controls()`.

## 1.4 Queue display

- `/queue` (`Bad/cmd/playback.cpp:107-118`): first replies `» ғᴇᴛᴄʜɪɴɢ ǫᴜᴇᴜᴇ...` (`queue_fetching`), then edits it to:
  - Currently playing (`queue_curr`): `<u><b>ᴄᴜʀʀᴇɴᴛʟʏ ᴘʟᴀʏɪɴɢ:</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href="{0}">{1}</a>\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {2}\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {3}\n\n`
  - Each waiting track (`queue_item`): `<b>{0}. ᴛɪᴛʟᴇ:</b> {1}\n     - {2} ᴍɪɴ\n\n`
  - Keyboard: single toggle button (see 1.3 `queue_markup`).
- Queue ops (`en.json`, all verbatim in Appendix A): `queue_nothing_waiting` = `» ᴛʜᴇʀᴇ ɪs ɴᴏᴛʜɪɴɢ ᴡᴀɪᴛɪɴɢ ɪɴ ᴛʜᴇ ǫᴜᴇᴜᴇ.`; `queue_cleared` = `ᴄʟᴇᴀʀᴇᴅ <b>{0}</b> ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs.\n\n» ʙʏ {1}`; `queue_restore_none` = `» ᴛʜᴇʀᴇ ɪs ɴᴏ ᴄʟᴇᴀʀᴇᴅ ǫᴜᴇᴜᴇ ᴛᴏ ʀᴇsᴛᴏʀᴇ.`; `queue_restored` = `ǫᴜᴇᴜᴇ ʀᴇsᴛᴏʀᴇᴅ.\n\n» ʙʏ {0}`; `queue_remove_usage` = `» ᴜsᴀɢᴇ: <code>/remove [position]</code> (ᴘᴏsɪᴛɪᴏɴ ᴀs sʜᴏᴡɴ ɪɴ /queue).`; `queue_bad_position` = `» ᴛʜᴀᴛ ᴘᴏsɪᴛɪᴏɴ ɪs ɴᴏᴛ ɪɴ ᴛʜᴇ ǫᴜᴇᴜᴇ.`; `queue_removed` = `ʀᴇᴍᴏᴠᴇᴅ #{0}: <b>{1}</b>\n\n» ʙʏ {2}`; `queue_move_usage` = `ᴜsᴀɢᴇ: <code>/move [from] [to]</code>`; `queue_moved` = `ᴍᴏᴠᴇᴅ #{0} ᴛᴏ #{1}.\n\n» ʙʏ {2}`; `queue_shuffle_few` = `» ɴᴇᴇᴅ ᴀᴛ ʟᴇᴀsᴛ ᴛᴡᴏ ᴡᴀɪᴛɪɴɢ ᴛʀᴀᴄᴋs ᴛᴏ sʜᴜғғʟᴇ.`; `queue_shuffled` = `sʜᴜғғʟᴇᴅ <b>{0}</b> ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs.\n\n» ʙʏ {1}`; `position_now` = `<b>{0}</b>\n\n{1} / {2}  •  sᴘᴇᴇᴅ {3}`; `play_queue_full` = `» ᴛʜᴇ ǫᴜᴇᴜᴇ ʟɪᴍɪᴛ ({0}) ʜᴀs ʙᴇᴇɴ ʀᴇᴀᴄʜᴇᴅ. ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ ғᴏʀ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛʟʏ ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs ᴛᴏ ғɪɴɪsʜ ᴘʟᴀʏɪɴɢ, ᴛʜᴇɴ ᴛʀʏ ᴀɢᴀɪɴ.`

## 1.5 Lyrics / search results

- **Lyrics: none.** No lyric command, no lyric strings, no lyric handler in the repo.
- **Search results:** no dedicated results list. `/play` flow: `sᴇᴀʀᴄʜɪɴɢ...` (`play_searching`) → `» ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ...` (`play_downloading`) → now-playing card or queued card. Failures: `play_not_found` = `» ғᴀɪʟᴇᴅ ᴛᴏ ᴘʀᴏᴄᴇss ᴛʜᴇ ǫᴜᴇʀʏ.\n\nɪғ ᴛʜᴇ ɪssᴜᴇ ᴘᴇʀsɪsᴛs, ʀᴇᴘᴏʀᴛ ɪᴛ ᴛᴏ ᴛʜᴇ <a href="{0}">sᴜᴘᴘᴏʀᴛ ᴄʜᴀᴛ</a>.`; `play_unsupported` = `» ʟɪᴠᴇ sᴛʀᴇᴀᴍs ᴀʀᴇ ɴᴏᴛ sᴜᴘᴘᴏʀᴛᴇᴅ.`; `play_duration_limit` = `» sᴛʀᴇᴀᴍs ʟᴏɴɢᴇʀ ᴛʜᴀɴ {0} ᴍɪɴᴜᴛᴇs ᴀʀᴇ ɴᴏᴛ ᴀʟʟᴏᴡᴇᴅ ᴛᴏ ᴘʟᴀʏ.`; `play_usage` = `<b>ᴜsᴀɢᴇ:</b>\n\n<code>/play attention</code>`.

## 1.6 Download progress

- (`dl_progress`, `en.json:31`): `<u><b>ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ғɪʟᴇ</b></u>\n\n<b>ᴘʀᴏɢʀᴇss:</b> {0} / {1} [{2:.1f}%]\n<b>sᴘᴇᴇᴅ:</b> {3}/s | <b>ᴇᴛᴀ:</b> {4}` — used for generic file downloads (not the /play audio download, which only shows `play_downloading`).
- Cancel keyboard: single button `[<DOWNLOAD_CANCEL_BTN text>]` → callback `cancel_dl` (`Bad/ui/buttons.cpp`, `cancel_dl`). Cancel flow (`Bad/cmd/play_cb.cpp:20-26`): on success edits the message to `ᴅᴏᴡɴʟᴏᴀᴅ ᴄᴀɴᴄᴇʟᴇᴅ ʙʏ {0}` (`dl_cancel`); if nothing to cancel, toast (alert) `» ᴅᴏᴡɴʟᴏᴀᴅ ᴀʟʀᴇᴀᴅʏ ᴄᴀɴᴄᴇʟʟᴇᴅ.` (`dl_not_found`). Other dl strings: `dl_complete` = `ᴅᴏᴡɴʟᴏᴀᴅ ᴄᴏᴍᴘʟᴇᴛᴇᴅ. ᴘʀᴏᴄᴇssɪɴɢ ғɪʟᴇ...\n\nᴛɪᴍᴇ ᴇʟᴀᴘsᴇᴅ: {0}s`; `dl_cancelling` = `» ʜᴀɴɢ ᴏɴ, ᴄᴀɴᴄᴇʟʟɪɴɢ ᴛʜᴇ ᴅᴏᴡɴʟᴏᴀᴅ ɴᴏᴡ!`; `dl_active` = `» ᴀʟʀᴇᴀᴅʏ ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ᴛʜᴀᴛ ғɪʟᴇ. ᴍɪɴᴅ ᴡᴀɪᴛɪɴɢ?`; `dl_limit` = `» ғᴀɪʟᴇᴅ ᴛᴏ ᴘʀᴏᴄᴇss ᴛʜᴇ ғɪʟᴇ. ᴛʜᴇ ᴍᴀxɪᴍᴜᴍ ᴅᴏᴡɴʟᴏᴀᴅ ʟɪᴍɪᴛ ɪs <b>200 ᴍʙ</b>.`

## 1.7 Settings menu + admin panels

- `/settings` and `/playmode` (no args) open the settings panel (`Bad/cmd/settings.cpp:57-64`): text `start_settings` = `<u><b>{0} sᴇᴛᴛɪɴɢs</b></u>\n\nᴄʟɪᴄᴋ ᴛʜᴇ ʙᴜᴛᴛᴏɴs ʙᴇʟᴏᴡ ᴛᴏ ᴄʜᴀɴɢᴇ ᴛʜɪs ᴄʜᴀᴛ's ᴄᴜʀʀᴇɴᴛ sᴇᴛᴛɪɴɢs.` with keyboard (`Bad/ui/buttons.cpp`, `settings_markup`):
  - Row 1: `[ᴄᴏᴍᴍᴀɴᴅ ᴅᴇʟᴇᴛᴇ ➜]` → callback `settings` (no-op toast `» ᴘʀᴏᴄᴇssɪɴɢ...`) · `[True|False]` → callback `settings delete`
  - Row 2: `[ᴀᴅᴍɪɴ ᴏɴʟʏ ᴘʟᴀʏ ➜]` → `settings` · `[True|False]` → `settings play`
  - Row 3: `[ʟᴀɴɢᴜᴀɢᴇ ➜]` → `settings` · `[<language name>]` → callback `language` (opens the language picker in place)
  - Tapping a value toggles it and edits the markup in place. `/playmode admin|everyone` (command form) replies `playmode_set` / `playmode_usage` instead.
- Language picker (`Bad/ui/buttons.cpp`, `lang_markup`): buttons `[<Lang name> (<code>) ✔️?]` two per row → callback `lang_change <code>`; current language gets a ` ✔️` suffix. Flow strings: `lang_choose` = `ᴘʟᴇᴀsᴇ ᴄʜᴏᴏsᴇ ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ʏᴏᴜ ᴡᴀɴᴛ ᴛᴏ sᴇᴛ ғᴏʀ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ:`; `lang_change` = `ᴄʜᴀɴɢɪɴɢ ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ᴛᴏ: {0}` (toast); `lang_changed` = `ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ʜᴀs ʙᴇᴇɴ ᴄʜᴀɴɢᴇᴅ ᴛᴏ: <i>{0}</i>`; `lang_same` = `ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ɪs ᴀʟʀᴇᴀᴅʏ sᴇᴛ ᴛᴏ: {0}`. Languages shipped: ar, de, en, es, fr, hi, ja, my, pa, pt, ru, tr, zh (`strings/`).
- Autoplay panel (command form `/autoplay`, strings in Appendix A): status text `autoplay_status` = `<b>ᴀᴜᴛᴏᴘʟᴀʏ:</b> {0}\n<b>ʟᴀɴɢᴜᴀɢᴇ:</b> <code>{1}</code>\n<b>ᴍᴏᴏᴅ:</b> <code>{2}</code>\n\n`; usage `autoplay_usage` documents `/autoplay on|off`, `/autoplay lang hindi` (or `auto`), `/autoplay mood sad` (or `any`); on/off toasts are the `autoplay_btn_on/off` strings (see 1.3).
- Thumbnail toggle: `thumb_status` = `<b>ᴛʜᴜᴍʙɴᴀɪʟ:</b> {0}\n\n<code>/thumbnail on</code> • <code>/thumbnail off</code>`; `thumb_enabled` = `<b>ᴛʜᴜᴍʙɴᴀɪʟ ᴇɴᴀʙʟᴇᴅ ʙʏ</b> {0}\nɪᴛ ᴡɪʟʟ ʙᴇ sʜᴏᴡɴ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs.`; `thumb_disabled` = `<b>ᴛʜᴜᴍʙɴᴀɪʟ ᴅɪsᴀʙʟᴇᴅ ʙʏ</b> {0}\nᴛʜᴇ ɴᴇxᴛ sᴏɴɢs ᴡɪʟʟ ʙᴇ sʜᴏᴡɴ ᴀs ᴛᴇxᴛ ᴏɴʟʏ.`
- Other admin command replies (all in Appendix A): adminmode (`adminmode_usage/invalid/set`, values `ᴀᴅᴍɪɴs ᴏɴʟʏ` / `ᴀᴅᴍɪɴs ᴀɴᴅ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs` / `ᴇᴠᴇʀʏᴏɴᴇ`), cmddelete, cleanmode (`cleanmode_on` = `ᴄʟᴇᴀɴ ᴍᴏᴅᴇ ᴏɴ: ʙᴏᴛ ᴍᴇssᴀɢᴇs ᴀʀᴇ ᴅᴇʟᴇᴛᴇᴅ ᴀғᴛᴇʀ <b>{0}</b> ᴍɪɴᴜᴛᴇs.\n\n» ʙʏ {1}`), nothumb, channelplay (`cplay_*`), userbot (`userbot_ready` = `» ᴛʜᴇ ᴀssɪsᴛᴀɴᴛ ɪs ʀᴇᴀᴅʏ.` etc.), autoleave, maintenance, speedtest, bug report, RTMP (`rtmp_*`, `/setrtmp` DM-only), stream (`stream_*`), bass (`bass_usage` = `ʙᴀss ʙᴏᴏsᴛ: <b>{0}</b>/100\nᴜsᴀɢᴇ: <code>/bass [0-100 | on | off]</code>`), speed (`speed_usage`/`speed_set`), jump, mute/unmute, loop.
- Auth list (`auth_list` = `<u><b>ʟɪsᴛ ᴏғ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ɪɴ {0}:</b></u>\n`; `auth_empty` = `» ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ ɪs ᴇᴍᴘᴛʏ.`), sudo list (`sudo_owner` = `<u><b>ᴏᴡɴᴇʀ:</b></u>\n- {0}\n\n`; `sudo_users` = `<u><b>sᴜᴅᴏ ᴜsᴇʀs:</b></u>`), blacklist list (`bl_list` = `<b>ʙʟᴀᴄᴋʟɪsᴛᴇᴅ:</b>\n{0}`), active streams (`vc_list` = `<u><b>ʟɪsᴛ ᴏғ ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs:</b></u>`; `vc_count` = `ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs ᴏɴ ᴛʜᴇ ʙᴏᴛ: <b>{0}</b>`; `vc_empty` = `» ʟᴏᴏᴋs ʟɪᴋᴇ ᴛʜᴇʀᴇ ᴀʀᴇɴ'ᴛ ᴀɴʏ ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs ᴏɴ ᴛʜᴇ ʙᴏᴛ.`).

## 1.8 Ping / stats / alive

- `/ping` or `/alive` (`Bad/cmd/tools.cpp`): first replies `» ᴘɪɴɢɪɴɢ… ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ…` (`pinging`), then edits the message into a **photo** (`PING_IMG`) captioned with `ping_pong`:
  `<u><b>ᴘᴏɴɢ!</b></u>\n\n<b>ʟᴀᴛᴇɴᴄʏ:</b> <code>{0}ms</code>\n\n<b>ᴜᴘᴛɪᴍᴇ:</b> {1}\n<b>ᴄᴘᴜ:</b> <code>{2}%</code>\n<b>ʀᴀᴍ:</b> <code>{3}%</code>\n<b>ᴅɪsᴋ:</b> <code>{4}%</code>\n<b>ɴᴛɢᴄᴀʟʟs ʟᴀᴛᴇɴᴄʏ:</b> <code>{5}ms</code>`
  with one URL button `[sᴜᴘᴘᴏʀᴛ]` → SUPPORT_CHAT (`ping_markup`).
- `/stats` (`Bad/cmd/tools.cpp`): text `stats_user` = `<u><b>{0} sᴛᴀᴛs</b></u>\n\n<b>ᴀssɪsᴛᴀɴᴛs:</b> {1}\n<b>ᴀᴜᴛᴏ ʟᴇᴀᴠᴇ:</b> {2}\n\n<b>ʙʟᴏᴄᴋᴇᴅ ᴄʜᴀᴛs:</b> {3}\n<b>ʙʟᴏᴄᴋᴇᴅ ᴜsᴇʀs:</b> {4}\n<b>sᴜᴅᴏ ᴜsᴇʀs:</b> {5}\n\n<b>sᴇʀᴠᴇᴅ ᴄʜᴀᴛs:</b> {6}\n<b>sᴇʀᴠᴇᴅ ᴜsᴇʀs:</b> {7}`; sudo users additionally get `stats_sudo` appended: `\n\n<b>ᴍᴏᴅᴜʟᴇs:</b> {0}\n<b>ᴘʟᴀᴛғᴏʀᴍ:</b> {1}\n<b>ʀᴀᴍ ᴜsᴀɢᴇ:</b> <code>{2}MB | {3}GB</code>\n<b>ᴄᴘᴜ ᴜsᴀɢᴇ:</b> <code>{4}% ({5} cores)</code>\n<b>sᴛᴏʀᴀɢᴇ:</b> <code>{6}GB | {7}GB</code>\n\n<b>ʟᴀɴɢᴜᴀɢᴇ:</b> <code>{8}</code>\n<b>ʟɪʙʀᴀʀʏ:</b> <code>{9}</code>\n<b>ᴠᴏɪᴄᴇ:</b> <code>{10}</code>` (values: modules count, `Linux`, RSS MB, total GB, CPU%, cores, disk used/total, `C++20`, `MTProto`, `ntgcalls`). Sent as photo (`PING_IMG`) with caption, plain-text fallback.

## 1.9 Errors & guards (verbatim, all in Appendix A)

- `error_no_audio` = `» ᴍᴏᴠɪɴɢ ᴛᴏ ᴛʜᴇ ɴᴇxᴛ ᴛʀᴀᴄᴋ...\n\nᴀᴜᴅɪᴏ sᴏᴜʀᴄᴇ ɴᴏᴛ ғᴏᴜɴᴅ ɪɴ ᴛʜᴇ ғɪʟᴇ.`; `error_no_call` = `<b>ɴᴏ ᴀᴄᴛɪᴠᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ғᴏᴜɴᴅ.</b>\n\nᴘʟᴇᴀsᴇ sᴛᴀʀᴛ ᴏɴᴇ ᴀɴᴅ <b>ᴛʀʏ ᴀɢᴀɪɴ</b>.`; `error_no_file` = `ᴅᴏᴡɴʟᴏᴀᴅ ғᴀɪʟᴇᴅ.\n\nɪғ ᴛʜᴇ ɪssᴜᴇ ᴘᴇʀsɪsᴛs, ʀᴇᴘᴏʀᴛ ɪᴛ ᴛᴏ ᴛʜᴇ <a href="{0}">sᴜᴘᴘᴏʀᴛ ᴄʜᴀᴛ</a>`; `error_tg_server` = `<u><b>ᴛᴇʟᴇɢʀᴀᴍ sᴇʀᴠᴇʀ ᴇʀʀᴏʀ</b></u>\n\nᴛᴇʟᴇɢʀᴀᴍ ɪs ᴇxᴘᴇʀɪᴇɴᴄɪɴɢ ɪɴᴛᴇʀɴᴀʟ ɪssᴜᴇs. ᴄʟᴇᴀʀɪɴɢ ᴛʜᴇ ǫᴜᴇᴜᴇ ᴀɴᴅ ʟᴇᴀᴠɪɴɢ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.`; `error_rtmp` = `» ʀᴛᴍᴘ sᴛʀᴇᴀᴍɪɴɢ ɪs ɴᴏᴛ sᴜᴘᴘᴏʀᴛᴇᴅ. ᴘʟᴇᴀsᴇ sᴛᴏᴘ ᴛʜᴇ ʀᴛᴍᴘ sᴛʀᴇᴀᴍ ᴀɴᴅ sᴛᴀʀᴛ ᴀ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪɴsᴛᴇᴀᴅ.`
- `not_playing` = `» ᴛʜᴇ ʙᴏᴛ ɪsɴ'ᴛ sᴛʀᴇᴀᴍɪɴɢ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.`; `user_not_admin` = `» ᴏʜ sᴜʀᴇ, ɢᴏ ᴀʜᴇᴀᴅ ᴀɴᴅ ᴍᴀɴᴀɢᴇ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ... ᴏʜ ᴡᴀɪᴛ, ʏᴏᴜ'ʀᴇ ɴᴏᴛ ᴀɴ ᴀᴅᴍɪɴ.`; `user_no_perms` = `» ʏᴏᴜ? ᴍᴀɴᴀɢᴇ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ? ɴᴏᴛ ᴡɪᴛʜ ᴛʜᴏsᴇ ᴘᴇʀᴍɪssɪᴏɴs, sᴡᴇᴇᴛɪᴇ.`; `user_not_found` = `» ɪ'ʟʟ ɴᴇᴇᴅ ᴀ ᴜsᴇʀ ɪᴅ — ᴏʀ ᴡᴏᴜʟᴅ ʏᴏᴜ ᴍɪɴᴅ ʀᴇᴘʟʏɪɴɢ ᴛᴏ sᴏᴍᴇᴏɴᴇ's ᴍᴇssᴀɢᴇ?`; `play_admin` = `<u><b>ᴀᴅᴍɪɴ ᴏɴʟʏ ᴘʟᴀʏ</b></u>\n\nᴏɴʟʏ ᴀᴅᴍɪɴs ᴀʀᴇ ᴀʟʟᴏᴡᴇᴅ ᴛᴏ ᴘʟᴀʏ ɪɴ ᴛʜɪs ᴄʜᴀᴛ.`; `play_chat_invalid` = `» ᴛʜɪs ʙᴏᴛ ᴄᴀɴ ᴏɴʟʏ ʙᴇ ᴜsᴇᴅ ɪɴ <b>sᴜᴘᴇʀɢʀᴏᴜᴘs</b>.\n\nᴛᴏ ᴄᴏɴᴠᴇʀᴛ ʏᴏᴜʀ ɢʀᴏᴜᴘ ᴛᴏ ᴀ sᴜᴘᴇʀɢʀᴏᴜᴘ, ᴍᴀᴋᴇ ᴛʜᴇ ᴄʜᴀᴛ ʜɪsᴛᴏʀʏ <b>ᴠɪsɪʙʟᴇ</b> ᴏɴᴄᴇ.\n\nʟᴇᴀᴠɪɴɢ ᴛʜɪs ᴄʜᴀᴛ...`; `play_user_invalid` = `» ʏᴏᴜ'ʀᴇ ᴀ ʜɪᴅᴅᴇɴ ᴀᴅᴍɪɴ.\n\nʀᴇᴠᴇʀᴛ ʙᴀᴄᴋ ᴛᴏ ᴜsᴇʀ ᴀᴄᴄᴏᴜɴᴛ.`; `play_banned` = `<u><b>{0} ᴀssɪsᴛᴀɴᴛ ɪs ʙᴀɴɴᴇᴅ ʏᴏᴜʀ ᴄʜᴀᴛ</b></u>\n\n<b>ɪᴅ:</b> <code>{1}</code>\n<b>ɴᴀᴍᴇ:</b> {2}\n<b>ᴜsᴇʀɴᴀᴍᴇ:</b> {3}`; `play_expired` = `» ᴛʜɪs ʙᴜᴛᴛᴏɴ ʜᴀs ᴇxᴘɪʀᴇᴅ.`; `play_invite_error` = `ғᴀɪʟᴇᴅ ᴛᴏ ɪɴᴠɪᴛᴇ ᴀssɪsᴛᴀɴᴛ ᴛᴏ ᴛʜᴇ ᴄʜᴀᴛ.\n\nʀᴇᴀsᴏɴ: <code>{0}</code>`; `auto_left` = `» ɪ ᴡᴀɪᴛᴇᴅ, ɪ ᴡᴀᴛᴄʜᴇᴅ, ɪ ʏᴀᴡɴᴇᴅ... ᴀɴᴅ ᴛʜᴇɴ ɪ ʟᴇғᴛ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.`; `maint` = `» ᴛʜᴇ ʙᴏᴛ ɪs ᴜɴᴅᴇʀ ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ.{0}\nᴘʟᴇᴀsᴇ ᴛʀʏ ᴀɢᴀɪɴ ʟᴀᴛᴇʀ.` (+ `maint_reason` = `\nʀᴇᴀsᴏɴ: {0}`).
- Broadcast (`gcast_*`): `gcast_usage` = `» ʀᴇᴘʟʏ ᴛᴏ ᴀ ᴍᴇssᴀɢᴇ ғᴏʀ ʙʀᴏᴀᴅᴄᴀsᴛɪɴɢ ᴛʜᴇ ᴍᴇssᴀɢᴇ.`; `gcast_active` = `» ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ ғᴏʀ ᴛʜᴇ ᴏɴɢᴏɪɴɢ ʙʀᴏᴀᴅᴄᴀsᴛ ᴛᴏ ғɪɴɪsʜ.`; `gcast_start` = `» ʙʀᴏᴀᴅᴄᴀsᴛɪɴɢ sᴛᴀʀᴛᴇᴅ.`; `gcast_end` = `» ʙʀᴏᴀᴅᴄᴀsᴛᴇᴅ ᴛʜᴇ ᴍᴇssᴀɢᴇ ᴛᴏ {0} ɢʀᴏᴜᴘs ᴀɴᴅ {1} ᴜsᴇʀs.`; cancel button text is locale key `cancel` = `ᴄᴀɴᴄᴇʟ` (NoxBeats has **no** broadcast-cancel keyboard — the broadcast is fire-and-forget; contrast Tina/Yukki/Arc which have one).
- Logs (`log_*`): `log_fetch` = `» ғᴇᴛᴄʜɪɴɢ ʟᴏɢs...`; `log_not_found` = `» ʟᴏɢ ғɪʟᴇ ᴅᴏᴇsɴ'ᴛ ᴇxɪsᴛ.`; `log_sent` = `ʟᴏɢ ғɪʟᴇ ᴏғ {0}`; logger channel join/leave notices `log_chat`, `log_user`, `log_removed`, `log_blocked` (Appendix A).
- Restart: `restarting` = `» ʀᴇsᴛᴀʀᴛɪɴɢ...`; `restarted` = `» ʀᴇsᴛᴀʀᴛ ɪɴ ᴘʀᴏɢʀᴇss. ᴅᴏɴ'ᴛ ᴡᴏʀʀʏ, ɪᴛ'ʟʟ ᴏɴʟʏ ᴛᴀᴋᴇ ᴀ ғᴇᴡ sᴇᴄᴏɴᴅs… ᴍᴀʏʙᴇ.`
- Speedtest: `speedtest_running` = `» ʀᴜɴɴɪɴɢ ᴛʜᴇ sᴘᴇᴇᴅ ᴛᴇsᴛ...`; `speedtest_failed` = `» ᴛʜᴇ sᴘᴇᴇᴅ ᴛᴇsᴛ ғᴀɪʟᴇᴅ.`; `speedtest_result` = `<b>sᴘᴇᴇᴅ ᴛᴇsᴛ</b>\n\nᴅᴏᴡɴʟᴏᴀᴅ: <b>{0}</b> ᴍʙᴘs\nᴜᴘʟᴏᴀᴅ: <b>{1}</b> ᴍʙᴘs\nᴘɪɴɢ: <b>{2}</b> ᴍs`.
- Bug report: `bug_usage` = `ᴜsᴀɢᴇ: <code>/bug [describe the problem]</code>`; `bug_report` = `<b>ʙᴜɢ ʀᴇᴘᴏʀᴛ</b>\n\nғʀᴏᴍ: {0} (<code>{1}</code>)\nᴡʜᴇʀᴇ: {2}\n\n{3}`; `bug_failed` = `» ᴄᴏᴜʟᴅ ɴᴏᴛ sᴇɴᴅ ᴛʜᴇ ʀᴇᴘᴏʀᴛ. ᴛʀʏ ᴀɢᴀɪɴ ʟᴀᴛᴇʀ.`; `bug_sent` = `» ᴛʜᴀɴᴋs! ʏᴏᴜʀ ʀᴇᴘᴏʀᴛ ᴡᴀs sᴇɴᴛ.`
- Privacy (`privacy_text`): `<b>ᴘʀɪᴠᴀᴄʏ</b>\n\n{0} sᴛᴏʀᴇs ᴏɴʟʏ ᴡʜᴀᴛ ɪᴛ ɴᴇᴇᴅs ᴛᴏ ʀᴜɴ: ᴄʜᴀᴛ ɪᴅs, ᴜsᴇʀ ɪᴅs ᴏғ ᴀᴅᴍɪɴs/ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs, ʟᴀɴɢᴜᴀɢᴇ ᴀɴᴅ ᴄʜᴀᴛ sᴇᴛᴛɪɴɢs. ᴍᴇssᴀɢᴇs ᴀʀᴇ ɴᴏᴛ sᴛᴏʀᴇᴅ. ᴅᴀᴛᴀ ɪs ɴᴇᴠᴇʀ sᴏʟᴅ ᴏʀ sʜᴀʀᴇᴅ.`
- Eval: `eval_error` = `⚠️ ᴇʀʀᴏʀ ᴇxᴇᴄᴜᴛɪɴɢ sɴɪᴘᴘᴇᴛ\n\n`; `eval_inp` = `ᴡʜᴀᴛ?`; `eval_out` = `<b>ᴏᴜᴛᴘᴜᴛ:</b>\n<code>{0}</code>`.

## 1.10 Playlists (personal)

- `pl_empty` = `» ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ɪs ᴇᴍᴘᴛʏ. ᴀᴅᴅ sᴏɴɢs ᴡɪᴛʜ <code>/addplaylist [song name or link]</code>.`; `pl_add_usage` = `» ᴜsᴀɢᴇ: <code>/addplaylist [song name | YouTube / Spotify / Apple Music / JioSaavn / SoundCloud link]</code>\nᴡʜɪʟᴇ ᴀ sᴏɴɢ ɪs ᴘʟᴀʏɪɴɢ, <code>/addplaylist</code> ᴀʟᴏɴᴇ sᴀᴠᴇs ᴛʜᴀᴛ sᴏɴɢ.`; `pl_adding` = `» ᴀᴅᴅɪɴɢ ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ...`; `pl_added` = `» ᴀᴅᴅᴇᴅ <b>{0}</b> ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. ʏᴏᴜ ɴᴏᴡ ʜᴀᴠᴇ {1} sᴏɴɢs.`; `pl_added_many` = `» ᴀᴅᴅᴇᴅ <b>{0}</b> sᴏɴɢs ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ({1} ᴡᴇʀᴇ ᴀʟʀᴇᴀᴅʏ ᴛʜᴇʀᴇ). ʏᴏᴜ ɴᴏᴡ ʜᴀᴠᴇ {2} sᴏɴɢs.`; `pl_dup` = `» ᴛʜᴀᴛ sᴏɴɢ ɪs ᴀʟʀᴇᴀᴅʏ ɪɴ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ.`; `pl_full` = `» ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ɪs ғᴜʟʟ ({0} sᴏɴɢs). ʀᴇᴍᴏᴠᴇ sᴏᴍᴇ ᴡɪᴛʜ /rmplaylist.`; `pl_unsupported` = `» ᴛʜᴀᴛ ʟɪɴᴋ ᴛʏᴘᴇ ᴄᴀɴ'ᴛ ʙᴇ sᴀᴠᴇᴅ ᴛᴏ ᴀ ᴘʟᴀʏʟɪsᴛ. ᴜsᴇ ᴀ sᴏɴɢ ɴᴀᴍᴇ ᴏʀ ᴀ ʏᴏᴜᴛᴜʙᴇ, sᴘᴏᴛɪғʏ, ᴀᴘᴘʟᴇ ᴍᴜsɪᴄ, ᴊɪᴏsᴀᴀᴠɴ ᴏʀ sᴏᴜɴᴅᴄʟᴏᴜᴅ ʟɪɴᴋ.`; `pl_header` = `<b>{0}'s ᴘʟᴀʏʟɪsᴛ</b> — {1} sᴏɴɢs\n\n`; `pl_more` = `» \n…ᴀɴᴅ {0} ᴍᴏʀᴇ.`; `pl_footer` = `\n\n/playplaylist ᴛᴏ ᴘʟᴀʏ ɪᴛ (ᴀᴅᴅ <code>-s</code> ᴛᴏ sʜᴜғғʟᴇ, <code>-v</code> ғᴏʀ ᴠɪᴅᴇᴏ)\n/rmplaylist [ɴᴜᴍʙᴇʀ | ɴᴀᴍᴇ | ᴀʟʟ] ᴛᴏ ʀᴇᴍᴏᴠᴇ`; `pl_rm_usage` = `ᴜsᴀɢᴇ: <code>/rmplaylist [number | song name | all]</code>`; `pl_rm_none` = `» ɪ ᴄᴏᴜʟᴅɴ'ᴛ ғɪɴᴅ ᴛʜᴀᴛ sᴏɴɢ ɪɴ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. ᴄʜᴇᴄᴋ /playlist.`; `pl_removed` = `» ʀᴇᴍᴏᴠᴇᴅ <b>{0}</b> ғʀᴏᴍ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. {1} sᴏɴɢs ʟᴇғᴛ.`; `pl_cleared` = `» ᴘʟᴀʏʟɪsᴛ ᴄʟᴇᴀʀᴇᴅ ({0} sᴏɴɢs ʀᴇᴍᴏᴠᴇᴅ).`
- Playlist fetch: `playlist_fetch` = `» ғᴇᴛᴄʜɪɴɢ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ...\n\nᴘʟᴇᴀsᴇ ʜᴏʟᴅ ᴏɴ.`; `playlist_error` = `» sᴏᴍᴇᴛʜɪɴɢ ᴡᴇɴᴛ ᴡʀᴏɴɢ ᴡʜɪʟᴇ ғᴇᴛᴄʜɪɴɢ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ.`; `playlist_queued` = `<u><b>ᴀᴅᴅᴇᴅ {0} ᴛʀᴀᴄᴋs ғʀᴏᴍ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ ᴛᴏ ǫᴜᴇᴜᴇ:</b></u>\n\n`.
- Link fetch: `link_fetch` = `» ʀᴇᴀᴅɪɴɢ ᴛʜᴇ {0} ʟɪɴᴋ...`; `link_error` = `» ᴄᴏᴜʟᴅ ɴᴏᴛ ʀᴇᴀᴅ ᴛʜᴀᴛ {0} ʟɪɴᴋ. ᴍᴀᴋᴇ sᴜʀᴇ ɪᴛ ɪs ᴘᴜʙʟɪᴄ ᴀɴᴅ ᴛʀʏ ᴀɢᴀɪɴ.`; `spotify_fetch` = `» ʀᴇᴀᴅɪɴɢ ᴛʜᴇ sᴘᴏᴛɪғʏ ʟɪɴᴋ...`; `spotify_error` = `» ᴄᴏᴜʟᴅ ɴᴏᴛ ʀᴇᴀᴅ ᴛʜᴀᴛ sᴘᴏᴛɪғʏ ʟɪɴᴋ. ᴍᴀᴋᴇ sᴜʀᴇ ɪᴛ ɪs ᴀ ᴘᴜʙʟɪᴄ ᴛʀᴀᴄᴋ, ᴀʟʙᴜᴍ ᴏʀ ᴘʟᴀʏʟɪsᴛ.`

## 1.11 Inline mode / reply keyboards / other

- **Inline mode: none.** No inline-query handler anywhere in the repo.
- **Reply keyboards: none.** All keyboards are inline.
- YouTube button (`Bad/ui/buttons.cpp`, `yt_key`): `[❐]` (URL button, link itself) + `[Youtube]` → track link. Used on YouTube result cards.
- Admin-cache: `admin_cache_wait` = `» ʏᴏᴜ ᴄᴀɴ ᴏɴʟʏ ʀᴇғʀᴇsʜ ᴛʜᴇ ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ ᴏɴᴄᴇ ᴇᴠᴇʀʏ 10 ᴍɪɴᴜᴛᴇs.`; `admin_cache_reloading` = `» ʀᴇʟᴏᴀᴅɪɴɢ ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ...`; `admin_cache_reloaded` = `» ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ ʀᴇғʀᴇsʜᴇᴅ sᴜᴄᴄᴇssғᴜʟʟʏ!`; `admin_required` = `» ʙᴏᴛ ʀᴇǫᴜɪʀᴇs ᴛʜᴇ <b>ɪɴᴠɪᴛᴇ ᴜsᴇʀs ᴠɪᴀ ʟɪɴᴋ</b> ᴘᴇʀᴍɪssɪᴏɴ ᴛᴏ ᴡᴏʀᴋ.`
- Channel play: `cplay_not_set` = `» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ɪs ɴᴏᴛ sᴇᴛ. ᴜsᴇ <code>/channelplay [channel id|username]</code> ɪɴ ᴛʜᴇ ɢʀᴏᴜᴘ ғɪʀsᴛ.`; `vc_busy_channel` = `» ᴛʜᴇ ᴄʜᴀɴɴᴇʟ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪs ᴘʟᴀʏɪɴɢ ʀɪɢʜᴛ ɴᴏᴡ. sᴛᴏᴘ ɪᴛ ᴡɪᴛʜ /cstop ᴏʀ ᴜsᴇ ᴛʜᴇ ᴄ-ᴄᴏᴍᴍᴀɴᴅs.`; `vc_busy_group` = `» ᴛʜᴇ ɢʀᴏᴜᴘ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪs ᴘʟᴀʏɪɴɢ ʀɪɢʜᴛ ɴᴏᴡ. sᴛᴏᴘ ɪᴛ ғɪʀsᴛ, ᴛʜᴇɴ ᴜsᴇ ᴛʜᴇ ᴄ-ᴄᴏᴍᴍᴀɴᴅs.`; `cplay_status` = `» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ɪs ʟɪɴᴋᴇᴅ ᴛᴏ <code>{0}</code>.`; `cplay_usage`, `cplay_disabled`, `cplay_invalid`, `cplay_not_accessible`, `cplay_not_channel_admin`, `cplay_enabled` — see Appendix A.
- RTMP: `rtmp_usage` = `ᴜsᴀɢᴇ (ɪɴ ᴘʀɪᴠᴀᴛᴇ ᴄʜᴀᴛ):\n<code>/setrtmp [chat id] [rtmp://host/app] [stream key]</code>\n<code>/setrtmp [chat id] off</code>`; `rtmp_saved` = `» ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ sᴀᴠᴇᴅ. ᴜsᴇ /stream [sᴏɴɢ/ᴜʀʟ] ɪɴ ᴛʜᴀᴛ ᴄʜᴀᴛ.`; `stream_started` = `sᴛʀᴇᴀᴍɪɴɢ <b>{0}</b> ᴛᴏ ʀᴛᴍᴘ.\n\n» ʙʏ {1}`; `stream_status` = `» sᴛʀᴇᴀᴍɪɴɢ <b>{0}</b> ғᴏʀ {1}.`; `stream_stopped` = `sᴛʀᴇᴀᴍ sᴛᴏᴘᴘᴇᴅ.\n\n» ʙʏ {0}`.
- Command inventory (from `on_command` registrations): `ac, activevc, addplaylist, addsudo, delsudo, rmsudo, admincache, reload, adminmode, alive, ping, auth, unauth, authlist, autoleave, autoplay, bass, blacklist, unblacklist, whitelist, blacklisted, broadcast, bug, channelplay, cleanmode, clear, cmddelete, end, stop, eval, ev, sh, exec, help, json, jump, lang, language, listsudo, sudolist, logger, logs, loop, maintenance, move, mute, unmute, nothumb, pause, play, playforce, vplay, vplayforce, playplaylist, player, playlist, playmode, settings, pos, privacy, remove, replay, resume, restore, seek, seekback, shuffle, skip, next, speed, speedtest, start, stats, stream, streamstatus, streamstop, thumbnail, thumb, userbot` (+ `cplay/cvplay/cfplay…` channel variants and `fplay/fvplay` force variants).
- Rich patterns: `<b>`, `<i>`, `<u>`, `<code>`, `<a href>`, `<blockquote>` (status appends). No `<h*>`, no `<table>`, no `<tg-spoiler>`, no `<details>`.

---

# 2. TinaMusic — `refs/TinaMusic/` (C++20, Bot API via own tg client)

**Primary source:** `locales/en.yml` (1,064 lines — reproduced verbatim in Appendix B). Other locales: ar, hi, tr.
**Keyboard source:** `src/bot_engine.cpp` (`MakeStartMarkup` :144, `MakeHelpMarkup` :166, `MakeBackMarkup` :185, `MakeSupportMarkup` :196, `MakePlayerMarkup` :208, `BuildAutoplayMarkup` :266, `MakeSettingsMarkup` :291, `MakeLanguageMarkup` :350).
**Style:** heavy small-caps Unicode + `<blockquote>` wrappers, `<tg-spoiler>`, `<s>` strikethrough, `<blockquote expandable>`. Note the `/play` now-playing text uses `{thumb}` placeholder — the thumbnail URL is inlined into the text (not a photo send) in `stream_now_playing` / `play_added_to_queue_single`.

## 2.1 /start

- PM (`start_private`, `locales/en.yml`): verbatim —
  ```
  <blockquote><a href="https://yukiapi.site/file/iCggpP12">👋</a> <b>ʜєʏ {user}</b>, <i>ᴍʏ ηᴧᴍє ɪs</i> {bot} 🎶

  ✨ <b><u>ᴧʙσυᴛ ᴍє</u>:</b>
  • ᴧ ᴘσᴡєꝛғᴜʟ & ʜɪɢʜ-ǫᴜᴧʟɪᴛʏ ᴛєʟєɢꝛᴧᴍ ᴍᴜsɪᴄ sᴛꝛєᴧᴍɪηɢ ʙσᴛ.
  • <s>ʟᴧɢɢʏ ᴧᴜᴅɪσ</s> ➜ <tg-spoiler><b>ᴜʟᴛꝛᴧ ʜᴅ ǫᴜᴧʟɪᴛʏ 🎧</b></tg-spoiler>
  • sᴜᴘᴘσꝛᴛs: <code>ʏσυᴛᴜʙє</code>, <code>sᴘσᴛɪғʏ</code>, <code>ᴊɪσsᴧᴧᴠη</code>, <code>sσυηᴅᴄʟσυᴅ</code> & ᴍσꝛє.
  • <b>sᴛᴧᴛᴜs:</b> <tg-spoiler>⚡ ᴏɴʟɪɴᴇ & ʀᴇᴀᴅʏ ᴛσ ᴘʟᴧʏ</tg-spoiler>

  💫 <i>ғєєʟ ғꝛєє ᴛσ ᴧᴅᴅ ᴍє ᴛσ ʏσυꝛ ɢꝛσυᴘs & єηᴊσʏ!</i> ✨</blockquote>
  <blockquote expandable>📖 <b><u>ǫᴜɪᴄᴋ ɢᴜɪᴅє & ᴄσϻϻᴧηᴅs</u>:</b>
  • <b>/play</b> <code>&lt;song&gt;</code> ➜ <i>ᴘʟᴧʏ ᴧᴜᴅɪσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/vplay</b> <code>&lt;query&gt;</code> ➜ <i>ᴘʟᴧʏ ᴠɪᴅєσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/settings</b> ➜ <i>ᴄᴜsᴛσᴍɪᴢє ʙσᴛ ᴘꝛєғєꝛєηᴄєs</i>
  • <b>/help</b> ➜ <i>ɢєᴛ ғᴜʟʟ ᴄσϻϻᴧηᴅ ʟɪsᴛ</i>
  • <b>sᴇᴄʀᴇᴛ:</b> <tg-spoiler>⚡ ᴜsє /speedtest ᴛσ ᴄʜєᴄᴋ ᴘɪηɢ</tg-spoiler>
  • <b>ᴛɪᴘ:</b> <tg-spoiler>💡 ᴜsє /cplay ғᴏʀ ᴄʜᴀɴɴᴇʟ ꜱᴛʀᴇᴀᴍɪɴɢ</tg-spoiler></blockquote>
  ```
- Group (`start_group`): `<b>💫 I’m here!</b> Try a command… if you think you can handle me 😉`
- Start keyboard (`MakeStartMarkup`, `src/bot_engine.cpp:144-164`):
  - Row 1: `[⚡ ᴧᴅᴅ ᴍє ᴛσ ʏσυꝛ ɢꝛσυᴘ]` → URL `https://t.me/<bot>?startgroup&admin=invite_users`
  - Row 2: `[❓ ʜєʟᴘ & ᴄσϻϻᴧηᴅs]` → callback `help:main`
  - Row 3: `[📢 ᴜᴘᴅᴧᴛєs]` → URL support_channel (default `https://t.me/Z0iiw`) · `[💬 sᴜᴘᴘσꝛᴛ]` → URL support_chat (default `https://t.me/Z0iiw`)
  - Note: **no Source button, no Language button** on the start card (contrast Yukki/Arc).
- Group /start sends the same start text + same markup (`bot_engine.cpp:596-600`).

## 2.2 /help — full text

- Menu text (`help_main`): verbatim —
  ```
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">📚</a> <b><u>ʜєʟᴘ &amp; ᴄσϻϻᴧηᴅs ᴍєηᴜ</u></b>

  ✨ <i>ᴄʟɪᴄᴋ ᴛʜє ᴄᴧᴛєɢσꝛʏ ʙᴜᴛᴛσηs ʙєʟσᴡ ᴛσ єxᴘʟσꝛє ᴧʟʟ ᴧᴠᴧɪʟᴧʙʟє ᴄσϻϻᴧηᴅs:</i>

  • 🛠 <b>ᴧᴅϻɪηs</b> ➜ <i>ᴍᴜsɪᴄ &amp; ᴘʟᴧʏʙᴧᴄᴋ ᴄσηᴛꝛσʟs</i>
  • 🌍 <b>ᴘᴜʙʟɪᴄ</b> ➜ <i>ɢєηєꝛᴧʟ &amp; ᴜsєꝛ ᴄσϻϻᴧηᴅs</i>
  • 👑 <b>σᴡηєꝛ</b> ➜ <i>ʙσᴛ ᴍᴧηᴧɢєᴍєηᴛ &amp; sʏsᴛєᴍ</i>
  • ⚡ <b>sᴜᴅσєꝛs</b> ➜ <i>ᴧᴅᴠᴧηᴄєᴅ ʙσᴛ ᴄσηᴛꝛσʟs</i>

  💡 <b>ᴛɪᴘ:</b> <tg-spoiler>ᴜsє /play &lt;song&gt; ᴛσ sᴛᴀʀᴛ ɪɴ ᴠᴄ</tg-spoiler></blockquote>
  ```
- Help keyboard (`MakeHelpMarkup`, `:166-183`):
  - Row 1: `[🛠 ᴧᴅϻɪηs]` `help:admins` · `[🌍 ᴘᴜʙʟɪᴄ]` `help:public`
  - Row 2: `[👑 σᴡηєꝛ]` `help:owner` · `[⚡ sᴜᴅσєꝛs]` `help:sudoers`
  - Row 3: `[⬅️ ʙᴧᴄᴋ]` `start`
  - Category pages use `MakeBackMarkup`: single row `[⬅️ ʙᴧᴄᴋ]` `help:main`.
- Category texts (verbatim in Appendix B): `help_admin` (22 commands: /playmode, /cplay, /adminmode, /cmddelete, /settings, /speed, /skip, /pause, /resume, /replay, /mute, /unmute, /seek, /seekback, /jump, /move, /clear, /remove, /shuffle, /loop, /stop, /nothumb), `help_public` (/play, /vplay, /queue, /ping, /start, /help, /bug, /position, /reload, /sudolist), `help_owner` (/addsudo, /delsudo, /blockuser, /unblockuser, /blockchat, /unblockchat, /blacklisted, /eval, /maintenance, /restart, /sh), `help_sudo` (/active, /autoleave, /logger, /stats, /logs). Each is a `<blockquote>` with an `<a href="https://yukiapi.site/file/awTXo6DL">` icon link and a `<tg-spoiler>` footer line.
- In-group /help: `help_private_only` = `🤖 Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!` (group help is a `[📒 ᴄσϻϻᴧηᴅs]` URL button → `https://t.me/<bot>?start=pm_help`, `GC_HELP_BTN`).

## 2.3 Now-playing card

- `stream_now_playing` (verbatim, `locales/en.yml`):
  ```
  <blockquote>{thumb} <b><u>ησᴡ ᴘʟᴧʏɪηɢ</u></b></blockquote>
  <blockquote>• <b>ᴛꝛᴧᴄᴋ:</b> <a href="{url}">{title}</a>
  • <b>ᴅᴜꝛᴧᴛɪση:</b> <code>{duration}</code>
  • <b>ꝛєǫᴜєsᴛєᴅ ʙʏ:</b> {by}</blockquote>
  ```
  `{thumb}` is the thumbnail URL text inlined at the start of the first blockquote (no photo send in the code paths at `bot_engine.cpp:682,815,907,1123,1159,1998,2021,2229,2255` — all `SendMessage` text). Sent with `MakePlayerMarkup`.
- Queued (`play_added_to_queue_single`):
  ```
  <blockquote>{thumb} <b><u>ᴧᴅᴅєᴅ ᴛσ ǫᴜєᴜє</u>:</b> #{index}</blockquote>
  <blockquote>• <b>ᴛꝛᴧᴄᴋ:</b> <a href="{url}">{title}</a>
  • <b>ᴅᴜꝛᴧᴛɪση:</b> <code>{duration}</code>
  • <b>ꝛєǫᴜєsᴛєᴅ ʙʏ:</b> {by}</blockquote>
  ```
- Multiple added: header `play_added_multiple_header` = `➕ <b>Added {count} tracks</b> by {user}`; hints `play_queue_limit_hint` = `⚠️ <i>Queue limit reached — some tracks were skipped.</i>`; `play_queue_view_hint` = `<i>Use /queue to view full list.</i>`
- Player markup (`MakePlayerMarkup`, `src/bot_engine.cpp:208-264`): progress bar built in code — 12 chars, `🔘` ball + `─` track, format `MM:SS 🔘────… MM:SS` (zero-padded, `formatTime`).
  - Row 1: `[<progress text>]` → callback `progress` (no-op)
  - Row 2: `[▷]` `room:<chat_id>:resume` · `[II]` `room:<chat_id>:pause` · `[‣‣I]` `room:<chat_id>:skip` · `[▢]` `room:<chat_id>:stop`
  - Row 3: `[15s -]` `room:<chat_id>:seekback_15` · `[⟳]` `room:<chat_id>:replay` · `[15s +]` `room:<chat_id>:seek_15`
  - Row 4: `[🗑 ᴄʟσsє]` `close`
  - Row 5: `[✨ ᴊσɪη ꝛσσϻ ✨]` → URL `https://t.me/Dollbymusicbot/room?startapp=<chat_id>` (hardcoded bot username `Dollbymusicbot` — a Tina-specific "join room" mini-app link)
  - Note: unlike Yukki, Tina shows **both** ▷ and II at once (no paused-state toggle), and has ±15s seek buttons. Prefix is `room:<chat_id>:` (channel play uses `croom:` — see Arc §4 for the same scheme; Tina's callback router handles both).
- Callback toasts / edits (`cb_*` keys, Appendix B): pause → toast `⏸️ Track paused at {position}` + edited card `cb_pause_message` (`<b>⏸️ Track Paused</b>` with Track/Position/Paused-by); resume → toast `▶️ Resumed at {position}.` + card `cb_resume_message` (`<b>🎵 Now Playing:</b>` …); replay → `🔁 <b>Replaying current track...</b>` then `🔁 Track replayed.` / edited `🔁 Track replayed by {user}.`; skip → `⏭️ Track skipped.` / `⏭️ Skipped by {user}`; stop → `⏹️ Playback stopped.`; mute → `🔇 Muted.` + card `cb_mute_message`; unmute → `🔊 Unmuted.`; seek → `⏩ Sought forward {seconds} seconds` / `⏪ Sought back {seconds} seconds` (+ `… — by {user}` edited variants); near-end guard `⚠️ Cannot seek forward {seconds} seconds — about to reach end.`
- Stop confirm: if paused → `stop_confirm_paused` = `⏸️ <b>Playback is currently paused.</b>\n\nDo you want to resume playing or stop it completely?`; if muted → `stop_confirm_muted` = `🔇 <b>Playback is currently muted.</b>\n\nDo you want to unmute or stop it completely?`; buttons `[▶️ ꝛєsᴜᴍє]` `CONFIRM_RESUME_BTN` / `[🔊 ᴜηᴍᴜᴛє]` `CONFIRM_UNMUTE_BTN` / `[⏹️ sᴛσᴘ]` `CONFIRM_STOP_BTN` (locale keys; callback data is the `room:<id>:resume|unmute|stop` scheme per Yukki's `GetStopConfirmMarkup` — Tina's locale defines the labels only).
- Restart confirm: text `restart_confirm_running` = `⚠️ A song is currently playing in this chat.\nChoose an action below.`; buttons `[🔄 ꝛєsᴛᴧꝛᴛ ʙσᴛ]` / `[🔁 ꝛєᴘʟᴧʏ sσηɢ]`; confirmations `♻️ Restarting the bot...` / `⏮️ Current song replayed successfully.`

## 2.4 Queue display

- `/queue` (`src/bot_engine.cpp:1200-1219`): text built as —
  ```
  {queue_header}\n\n
  {queue_now_playing} <a href="{url}">{title}</a> ({duration})\n\n
  📋 <b>Pending Tracks:</b>\n
  1. <a href="{url}">{title}</a> ({duration})\n
  2. …
  ```
  with `queue_header` = `🎶 <b>Current Queue</b>`, `queue_now_playing` = `▶️ <b>Now Playing:</b>`, `queue_up_next` = `⏭️ <b>Up Next:</b>` (locale keys exist but the code uses the literal `📋 <b>Pending Tracks:</b>` header for the list). No pagination, no delete buttons, no "show more". Empty: `queue_no_active` blockquote (see Appendix B). Other queue strings: `queue_empty` = `⚠️ <b>The queue is already empty.</b>`; `queue_restored` = `✅ Successfully restored <b>{count}</b> tracks to the queue.`; `queue_restore_no_data` = `❌ No cleared queue found to restore, or a new song has already been added.`; `queue_limit_reached` = `⚠️ Queue limit reached ({limit} tracks max). Use /clear to clear queue.`; `queue_more_line` = `\n… and {remaining} more` (defined but the render path above does not use it); `queue_empty_tail` = `📭 <i>No more songs in queue.</i>`
- Queue finished: `stream_queue_finished` = `<blockquote>🎵 <b><u>ǫᴜєᴜє ғɪηɪsʜєᴅ</u></b>\n\n<i>ησ ᴍσꝛє ᴛꝛᴧᴄᴋs ʟєғᴛ ᴛσ ᴘʟᴧʏ. ᴜsє /play ᴛσ ᴧᴅᴅ ᴍσɢє! 🎶</i></blockquote>`; next-track notice `stream_downloading_next` = `<blockquote>📥 <b><u>ᴅσᴡηʟσαᴅɪηɢ ηєхᴛ ᴛꝛᴧᴄᴋ...</u></b></blockquote>`; autoplay next `autoplay_fetching_next` = `<blockquote>🎵 <b><u>ᴧᴜᴛσᴘʟᴧʏ</u></b> ➜ <i>ғєᴛᴄʜɪηɢ ηєхᴛ ᴛꝛᴧᴄᴋ...</i></blockquote>`.

## 2.5 Lyrics / search results

- **Lyrics: none.**
- **Search results:** no results list. `/play` flow: `searching` = `🔍🎶 Searching... ⚡✨`; `searching_query` = `🔍🎶 Searching for: <b>{query}</b>... ⚡✨`; `play_downloading_song` = `📥 Downloading song "{title}"`; `play_download_canceled` = `⚠️ Download canceled by {user}.`; no-query blockquote `no_song_query` (verbatim in Appendix B, with Usage ` {cmd} &lt;song name&gt;`, Example `{cmd} closer`, tip spoiler about YouTube/Spotify links); not-found `no_song_found` = `<blockquote>❌ <b><u>ɴσ ᴛꝛᴧᴄᴋs ғσυηᴅ</u></b>\n\n<i>ᴄσυʟᴅ ησᴛ ғɪηᴅ ᴧηʏ ᴍᴧᴛᴄʜɪηɢ ꝛєsᴜʟᴛs. ᴘʟєᴧsє ᴛꝛʏ ᴧησᴛʜєꝛ ǫᴜєꝛʏ!</i></blockquote>`; already queued `play_already_in_queue` blockquote; too-long `play_single_track_too_long` / `play_multiple_tracks_too_long_*` / `play_all_tracks_skipped` (Appendix B); failure `play_failed` = `❌ Failed to play\nError: {error}`; `play_unable` = `<blockquote>❌ <b><u>ᴜηᴧʙʟє ᴛσ ᴘʟᴧʏ</u></b>\n\n<i>ᴘʟєᴧsє ᴍᴧᴋє sᴜꝛє ᴠσɪᴄє ᴄʜᴧᴛ ɪs ᴧᴄᴛɪᴠє σꝛ ꝛєsᴛᴧꝛᴛ ᴛʜє ᴠᴄ.</i></blockquote>`.

## 2.6 Download progress

- No percentage/progress-bar download message. Download cancel: locale key `DOWNLOAD_CANCEL_BTN` = `🚦 ᴄᴧηᴄєʟ ᴅσᴡηʟσᴧᴅ`; callback strings `download_cancelled` = `✅ Download cancelled.`, `no_download_to_cancel` = `ℹ️ No download to cancel.`, `invalid_request` = `⚠️ Invalid request.`, `unknown_action` = `⚠️ Unknown action.` (No `cancel_dl` keyboard builder was found in `bot_engine.cpp` — the cancel button text exists in the locale but no `Make*` function builds it; the /play flow shows only the `searching`/`play_downloading_song` text edits.)

## 2.7 Settings menu + admin panels

- `/settings` (`src/bot_engine.cpp:1306`): text `settings_main` —
  ```
  ⚙️ <b>Chat Settings</b>

  <b>Group:</b> {title}
  <b>Group ID:</b> <code>{id}</code>

  Configure the bot's behavior in this group using the buttons below.
  ```
  Keyboard (`MakeSettingsMarkup`, `:291-348`) — 9 rows, each `[label] [value]`:
  - Row 1: `[🛡️ ᴧᴅϻɪη ᴍσᴅє]` `info:adminmode` · `[<Admins 🔐|Auth Users 🎧|Everyone 🌍>]` `set:adminmode` (cycles admin → adminauth → everyone)
  - Row 2: `[🎵 ᴘʟᴧʏ ᴍσᴅє]` `info:playmode` · `[<Admins 🔐|Everyone 🌍>]` `set:playmode`
  - Row 3: `[🗑️ ᴄϻᴅ ᴅєʟєᴛє]` `info:cmddelete` · `[<Enabled|Disabled>]` `set:cmddelete`
  - Row 4: `[🧹 ᴄʟєᴧη ᴍσᴅє]` `info:cleanmode` · `[<Enabled|Disabled>]` `set:cleanmode`
  - Row 5: `[⏱️ ᴄʟєᴧη ᴅєʟᴧʏ]` `info:cleanduration` · `[<15m|30m|60m|5m>]` `set:cleanduration` (cycles 15m → 30m → 60m → 5m)
  - Row 6: `[🖼️ ᴛʜᴜϻʙηᴧɪʟs]` `info:nothumb` · `[<Enabled|Disabled>]` `set:nothumb`
  - Row 7: `[📻 Autoplay]` `info:autoplay` · `[<Enabled|Disabled>]` `set:autoplay` (**Tina-only row** — Yukki/Arc have no autoplay row in the settings panel)
  - Row 8: `[🌐 ʟᴧηɢᴜᴧɢє]` `info:lang` · `[<language name>]` `lang:select`
  - Row 9: `[🗑 ᴄʟσsє]` `close`
  - `info:*` taps answer an alert with the `settings_info_*` text (Appendix B: `settings_info_adminmode`, `settings_info_cmddelete`, `settings_info_cleanduration` = `Cycles auto-delete delay: 15m → 30m → 60m → 5m.`, `settings_info_cleanmode`, `settings_info_lang`, `settings_info_nothumb`, `settings_info_playmode`); `set:*` taps toggle and re-edit the panel; toast `settings_updated` = `✅ Settings updated!`
- Autoplay card (`GetAutoplayMessage`, `bot_engine.cpp:278-289` — hardcoded, not locale):
  `📻 <b>Autoplay Mode Settings</b>\n\n• <b>Current Status:</b> 🟢 <b>Enabled (ON)</b> | 🔴 <b>Disabled (OFF)</b>\n\n<i>When the current queue finishes, the bot will <b>automatically discover and stream related songs</b> seamlessly.</i> (or: <i>When the current queue finishes, music playback will <b>stop automatically</b>.</i>)\n\n<i>Click the button below to toggle the autoplay state:</i>`
  Keyboard (`BuildAutoplayMarkup`): single row `[🟢 Turn OFF|🔴 Turn ON]` `autoplay:off|on:<chat_id>` · `[🗑 Close]` `close`.
- Language picker (`MakeLanguageMarkup`, `:350-366`): Row 1 `[🇺🇸 English]` `lang:en` · `[🇮🇳 हिन्दी]` `lang:hi`; Row 2 `[🇸🇦 العربية]` `lang:ar` · `[🇹🇷 Türkçe]` `lang:tr`; Row 3 `[🔒 Close]` `close`. Prompt text: `🌐 <b>Select Bot Language:</b>` (hardcoded at `bot_engine.cpp:1318`). Replies: `lang_success` = `✅ Language successfully updated to {lang_name}.`; `lang_same` = `ℹ️ You’re already using this language.`; `lang_fail` = `❌ Failed to update language. Please try again later.`
- Command-form settings (Appendix B): `adminmode_help` (+ `adminmode_status_*`, `adminmode_updated` = `✅ <b>Admin command mode</b> updated to: {status}`); `playmode_help` (+ `playmode_success_admins` = `🛡️ <b>Play mode</b> is now restricted to <b>Admins & DJs</b> only. 🎧`, `playmode_success_everyone` = `✅ <b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow! 🎶`, `playmode_restricted` = `🚫 Play mode is enabled. Only admins and authorized users can play.`); `nothumb_status`/`nothumb_*`; `cmddelete_status`/`cmddelete_updated`; `cleanmode_status`/`cleanmode_hint`/`cleanmode_updated`/`cleanmode_duration_updated`; `autoleave_*`; `logger_*`; `loop_*` (0–10); `speed_*` (0.50x–4.00x, optional reset duration); `mute_*`/`unmute_*` (incl. auto-unmute duration 5–3600s); `pause_*` (incl. auto-resume); `seek_*/jump_*`; `move_*/remove_*`; `shuffle_*`; `reload_*` (per-line admin-cache / voice-chat / assistant status lines).
- Auth: `authlist_header` = `<b>Authorized Users:</b>`; entries `{index}. {user} (<code>{user_id}</code>)`; `authlist_total` = `<b>Total:</b> {count} users`; `authlist_empty` = `ℹ️ There are no authorized users in this chat.`; `authlist_fetching` = `⏳ Fetching authorized users list...`
- Sudo: `sudo_list_header` = `👑 <b>Current Sudoers:</b>`; `sudo_list_owner` = `{index}. {user} — <b>Owner</b>`; `sudo_list_entry` = `{index}. {user}`; `sudo_list_no_extra` = `⚠️ No additional sudoers found.`; jokes `sudo_owner_self` = `😂 Haha, you’re the boss already! Why are you even trying to add yourself?`, `sudo_bot_self` = `🤖 Haha, I’m the bot! I can’t add myself to the sudo list — even the owner can’t cheat me 😎`, `sudo_bot_user` = `🤖 You can’t add a bot to the sudo list — sudoers must be human!`
- Blacklist: `blacklist_list_title` = `📛 <b>Blacklisted</b>`; `blacklist_list_chats` = `💬 <b>Chats:</b>`; `blacklist_list_users` = `👤 <b>Users:</b>`; blocked notices `blacklist_chat_blocked` = `🚫 This chat is blocked from using this bot.`, `blacklist_user_blocked` = `🚫 You are blocked from using this bot.`, `blacklist_owner_blocked_leave` = `🚫 This chat owner is blocked from using this bot. Leaving chat.`
- Broadcast (`broadcast_*`, Appendix B): progress message is edited in place with header `📡 Broadcasting...` / `🚫 Broadcast Cancelled` / `✅ Broadcast Completed`, lines `📊 Total Chats: {done}/{total} ({progress}%)`, `👥 Total Users: {done}/{total} ({progress}%)`, `❌ Failed Chats: {count}`, `❌ Failed Users: {count}`, `⏱ Delay: {delay}s`, `⏰ Elapsed: {elapsed}`, `⏳ ETA: {eta}`, `✨ Success Rate: {rate}% ({sent}/{total})`; cancel button `[🚫 ᴄᴧηᴄєʟ ʙꝛσᴧᴅᴄᴧsᴛ]` → callback `bcast_cancel` (Tina's `BROADCAST_CANCEL_BTN`); `broadcast_cancel_owner_only` = `⚠️ Only the owner can cancel broadcasts.`
- Active chats: `active_chats_info` = `📊 <b>Active Chats:</b>\n\nThere are <i>{count}</i> active chats across the bot system.` (+ `..._with_broken` variant).
- RTMP: `rtmp_setup_usage` (full DM-only guide, Appendix B), `rtmp_configured_success` = `✅ RTMP configured successfully for Chat ID: <code>{chat_id}</code>`, `rtmp_now_streaming` (with `⚠️ <i>Note: RTMP has ~15-30s buffering delay</i>`), `rtmp_status_playing` (`📡 <b>RTMP Status</b>` / `▫ State: ▶️ Streaming` / `▫ Position: {position}`), `rtmp_dm_only` = `⚠️ Use this command in DM.`

## 2.8 Ping / stats

- `/ping`: first `ping_start` = `<blockquote>🏓 <b><u>ᴘɪηɢɪηɢ...</u></b>\n\n<i>ᴄʜєᴄᴋɪηɢ ʟᴧᴛєηᴄʏ &amp; sʏsᴛєᴍ sᴛᴧᴛs</i></blockquote>`, then `ping_result` (verbatim):
  ```
  <blockquote><a href="https://yukiapi.site/file/ICyh3CVm">🏓</a> <b><u>ᴘσηɢ!</u></b> ➜ <code>{latency}ms</code></blockquote>
  <blockquote>📊 <b><u>sʏsᴛєᴍ sᴛᴧᴛs</u>:</b>

  • ⏳ <b>ᴜᴘᴛɪᴍє:</b> <code>{uptime}</code>
  • 🧠 <b>ꝛᴧϻ ᴜsᴧɢє:</b> <code>{ram_info}</code>
  • ⚡ <b>ᴄᴘᴜ ᴜsᴧɢє:</b> <code>{cpu_usage}</code>
  • 💾 <b>ᴅɪsᴋ ᴜsᴧɢє:</b> <code>{disk_usage}</code>
  • 🤖 <b>ʙσᴛ:</b> {bot}

  💡 <b>sᴛᴧᴛᴜs:</b> <tg-spoiler>🟢 ᴀʟʟ sʏsᴛᴇᴍs ᴏᴘᴇʀᴀᴛɪᴏɴᴀʟ</tg-spoiler>
  🚀 <b>ᴘɪηɢ:</b> <tg-spoiler>⚡ {latency}ms (ᴜʟᴛʀᴀ-ꜰᴀsᴛ)</tg-spoiler></blockquote>
  ```
- `/stats`: `stats_overview` (verbatim in Appendix B) — sections 🔧 System (OS/Arch/CPUs/Goroutines), 📦 Internal Memory (Go) (Alloc/Sys/NumGC), 💻 Server Stats (CPU/RAM/Storage), 🍃 MongoDB Stats (Storage/Collections/Objects), 📊 Served (Chats/Users), 🧩 Runtime Versions (Go, Gogram API Layer, Gogram Version, NTgCalls Version) — all inside `<blockquote>`s.
- Speedtest: `spt_running_download` = `🚀 Running download test...`; `spt_running_upload` = `📤 Running upload test...`; `spt_result` (verbatim in Appendix B: Client IP/ISP/Lat/Lon, Server Name/Country/Sponsor/Distance/Latency, Speed Download/Upload Mbps).

## 2.9 Errors & guards (verbatim, Appendix B for full list)

- `only_admin` = `⚠️ <b>Permission Denied!</b>\nOnly <b>admins</b> can control these actions. You can still play songs 🎵.\n\nIf you believe you are an admin, please use /reload to refresh your admin status.`; `only_admin_cb` = `⚠️ Only admins can do these actions.`; `only_admin_or_auth` / `only_admin_or_auth_cb`; `only_supergroup` = `⚠️ This command can only be used in groups.`; `only_owner` = `⚠️ Only the bot owner can use this command.`; `only_sudo` = `⚠️ Only sudo users or the bot owner can use this command.`
- `supergroup_needed` (verbatim, Appendix B): regular-group notice with `<a href="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">🔗 Go to the guide</a>` and support-group link.
- `err_*` family (Appendix B): `err_assistant_banned` (with `✅ Unbanned them already? Use /reload to refresh.`), `err_admin_permission_required`, `err_assistant_get_failed`, `err_assistant_join_request_sent`, `err_assistant_invite_link_fetch`, `err_assistant_invite_failed`, `err_connection_timeout`, `err_no_active_voicechat` (`🎙️ <b>No Active Voice Chat</b>` … `Use /reload to refresh.`), `err_fetch_failed`, `err_rtmp_missing_params`, `err_unknown`, `err_fetch_tracks`, `err_download_internal`, `generic_error` = `❌ Something went wrong: <code>{error}</code>`.
- Maintenance: `maint` = `⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later.\n\n{reason}`; `maint_reason` = `<i>📝 Reason: {reason}</i>`; `maint_usage`, `maint_enabled/disabled`, `maint_already_enabled` = `ℹ️ Maintenance mode is already enabled 🟢.` etc.; bot-added-during-maintenance `bot_added_maintenance` = `⚠️ I'm currently under maintenance and will be back soon. Thanks for your patience! ⏰`; `maint_entering` = `⚠️ Bot is entering maintenance mode...`
- Bot added: `bot_added_normal` = `🎵 Thanks for adding me! Use /play [song] to start playing music.`; demotion `bot_demotion_goodbye` blockquote (`<b>⚠️ Permission Lost</b>` … `I've been <b>demoted from admin</b>` … `<b>Goodbye 👋</b>`); assistant restricted `assistant_restricted_warning` blockquote; owner/sudo join notices `sudo_join_owner` = `👑 {user} — owner of {bot} — just entered the chat.`, `sudo_join_sudo` = `🛡️ {user}, a sudo user of {bot}, just joined the chat.`
- Voice-chat events: `voicechat_ended` = `<b>⚠️ Video chat ended</b>\n\n<i>⏰ Duration: {duration}</i>`; `voicechat_started` = `<b>♻️ Voice chat started!</b>\n\n<i>Use /play [song name] to play music.</i>`
- Flood: `flood_seconds` = `⏳ Slow down! Try again in {duration} seconds.`; `flood_minutes` = `⏳ Please wait {duration} minutes before using this command again.`
- Logger channel messages: `logger_bot_added`, `logger_bot_removed`, `logger_bot_started`, `logger_playback_template` (Appendix B) — e.g. playback log: `<b>🎧 Track:</b> <a href="{track_url}">{track}</a>\n<b>🔗 Source:</b> {source}\n<b>📌 Group:</b> {group} ({group_id})\n<b>👤 Requested by:</b> {requested_by} (<code>{requested_by_id}</code>)`.
- Bug: `bug_usage` = `⚠️ Please describe the bug. Usage: {cmd} <problem> or reply to a message.`; `bug_thanks` = `✅ Thank you for your bug report! It has been sent to the developers.`; `bug_report_format` blockquote (From/Chat/Report in `<pre>`).
- Channel play (`cplay_*`, Appendix B): `cplay_enabled` = `✅ <b>Channel Play enabled.</b> All channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) will now work in channel <code>{channel_id}</code>.` etc.

## 2.10 Inline mode / reply keyboards / other

- **Inline mode: none.** No inline-query handler.
- **Reply keyboards: none.**
- `room_not_active_cb` = `⚠️ Nothing playing right now.`; `room_no_active` blockquote; `room_not_linked` = `⚠️ This channel isn't linked to any group.`
- `playmode_help`, `adminmode_help` (see 2.7); `invalid_bool` = `⚠️ <b>Invalid value.</b>\nUse 'enable' or 'disable'.`
- Restart: `restart` = `♻️ Restarting...`; `restart_service` = `{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds.`; `restart_initiated` = `✅ Restart initiated successfully!`; `restart_fail` = `❌ Failed to restart: {error}`.
- `stream_channelid_fail` = `⚠️ Failed to get your connected channel’s ID.`; `stream_download_fail` = `❌ Failed to download.\nError: <code>{error}</code>`; `stream_play_fail` = `❌ Failed to play the song.`
- Command inventory (from `locales/en.yml` help texts + `bot_engine.cpp` handlers): `/play /vplay /fplay /fvplay /cplay /cvplay /cfplay /pause /cpause /resume /cresume /skip /cskip /stop /cstop /mute /cmute /unmute /cunmute /seek /cseek /seekback /cseekback /jump /cjump /move /cmove /clear /cclear /remove /cremove /shuffle /cshuffle /loop /cloop /speed /cspeed /queue /cqueue /position /cposition /settings /playmode /adminmode /cmddelete /cleanmode /nothumb /autoplay /thumbnail /channelplay /setcplay /auth /delauth /authlist /reload /creload /start /help /ping /bug /sudolist /active /autoleave /logger /stats /logs /addsudo /delsudo /blockuser /unblockuser /blockchat /unblockchat /blacklisted /eval /maintenance /restart /sh /setrtmp /stream /streamstop /streamstatus /language /lang /json /privacy /speedtest`.

---

# 3. YukkiMusic-Go — `refs/YukkiMusic-Go/` (Go, gotdbot/TDLib rich messages)

**Primary source:** `internal/locales/en.yml` (882 lines — reproduced verbatim in Appendix C). Other locales: ar, hi, tr.
**Keyboard source:** `internal/core/buttons.go` (all markups), `internal/modules/dashboard.go`, `internal/modules/settings.go`, `internal/modules/language.go`, `internal/modules/queue.go`, `internal/modules/auth.go`.
**Command list:** `internal/modules/comm.go` (scoped BotCommands). **Callback routing:** `internal/modules/handlers.go:116-147`.
**Style:** TDLib rich messages — `<h2>/<h3>/<h4>`, `<p>`, `<aside>`, `<footer>`, `<table bordered striped compact>`, `<tg-button type="callback_data" style="danger|primary">`, `<tg-button-row>`. Emoji go through `{emoji:X}` tokens. Buttons can carry custom-emoji icons (`IconCustomEmojiId`) and colour styles (red/blue/green) unless `DisableColour`.

## 3.1 /start

- PM (`start_private`): verbatim —
  ```
  <h3>{emoji:🎵} {bot}</h3>
  <p>Hey {user} — I stream music in your Telegram voice &amp; video chats from YouTube, Spotify, SoundCloud, Apple Music and more. Queue, loop, shuffle, seek and control speed with a live queue.</p>
  <p><b>Enjoy high-quality audio, fast downloads and smooth playback everywhere.</b></p>
  <footer><i>Tap the buttons below to add me or explore commands.</i></footer>
  ```
  Sent with `GetStartMarkup` (`internal/modules/start.go:57,70`).
- Group (`start_group`): verbatim —
  ```
  <h2>{bot}</h2>
  <p>I stream music in your Telegram voice &amp; video chats from YouTube, Spotify, SoundCloud, Apple Music and more — with queue, loop, shuffle, seek and speed control.</p>
  <aside>Try a command… if you think you can handle me {emoji:😉}</aside>
  <tg-button-row align="center">
    <tg-button type="url" style="primary" url="{support_channel}">Updates Channel</tg-button>
    <tg-button type="url" style="link" url="{support_chat}">Support Group</tg-button>
  </tg-button-row>
  ```
  (Group start is a rich message with **in-body** tg-buttons, no inline keyboard — `internal/modules/start.go:38-43`.)
- Start keyboard (`GetStartMarkup`, `internal/core/buttons.go:218-264`):
  - Row 1: `[⚡ Add Me to Your group]` → URL `https://t.me/<bot>?startgroup&admin=invite_users`
  - Row 2: `[❓ Help & Commands]` → callback `help_cb`
  - Row 3: `[📢 Updates]` → URL SupportChannel · `[💬 Support]` → URL SupportChat
  - Row 4: `[🌐 Language]` → callback `lang:select` · `[🔗 Source Code]` → URL `https://github.com/TheTeamVivek/YukkiMusic`
- Group help button (`GetGroupHelpKeyboard`): single `[📒 Commands]` → URL `https://t.me/<bot>?start=pm_help`.

## 3.2 /help — full text

- Menu text (`help_main`): verbatim —
  ```
  <h3>{emoji:📚} Help Menu</h3>
  <p>Browse commands by category: <b>Public</b> for everyone, plus <b>Admin</b>, <b>Sudo</b> and <b>Owner</b> for higher access levels.</p>
  <aside>Tip: run <code>/help &lt;command&gt;</code> to get detailed help for any single command.</aside>
  <footer><i>For any issues or support, contact our <a href="{support}">support chat</a>.</i></footer>
  ```
- Help keyboard (`GetHelpKeyboard`, `buttons.go:267-299`):
  - Row 1: `[🛠 Admins]` `help:admins` · `[🌍 Public]` `help:public`
  - Row 2: `[👑 Owner]` `help:owner` · `[⚡ Sudoers]` `help:sudoers`
  - Row 3: `[⬅️ Back]` `start` (blue style)
  - Category pages use `GetBackKeyboard`: single `[⬅️ Back]` `help:main` (blue style).
- Category texts (verbatim in Appendix C): `help_owner` (table: /addsudo, /delsudo, /blockuser, /unblockuser, /blockchat, /unblockchat, /blacklisted, /broadcast, /maintenance, /restart, /sh), `help_sudo` (table: /active, /stats, /logs, /json, /autoleave, /logger), `help_admin` (4 grouped tables — 🎵 Playback, 📜 Queue, ⏩ Seek & Speed, ⚙️ Settings & Access; footer: `Channel variants (e.g. /cpause, /cskip) act on the linked channel's voice chat.`), `help_public` (table: /play, /queue, /position, /ping, /reload, /authlist, /sudolist, /language, /privacy, /help, /start).
- **Per-command help** (`/help <command>`, `internal/modules/help.go:150-153`): every command has a `helpTexts["/cmd"]` rich block (see Appendix E for the full extracted set). Examples, verbatim:
  - `/play`: `<i>Play a song in the voice chat from YouTube, Spotify, or other sources.</i>` + usage block (see Appendix E).
  - `/help`: `ℹ️ <b>Help Command</b>` + usage.
  - Aliases share texts: `/fplay`→`/playforce`, `/cfplay`, `/fcplay`; `/vplay`→`/vcplay`, `/cvplay`; `/setcplay`→`/channelplay`; `/maint`→`/maintenance`; `/gcast`,`/bcast`→`broadcast`; `/bash`,`/shell`→`/sh`.
  - Channel variants are auto-generated (`handlers.go:520-521`): `<i>Channel play variant of %s</i>` + base text.
- In-group /help: `help_private_only` = `{emoji:🤖} Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!`

## 3.3 Now-playing card

- `stream_now_playing` (verbatim):
  ```
  <b>{emoji:🎵} Now Playing:</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Requested by:</b> {by}
  ```
- **Thumbnail handling** (`internal/modules/helpers.go:239-310`, `showTrackMessage`): if `track.Artwork != ""` and thumbnails are not disabled for the chat → `SendPhoto` with the artwork as the photo and the text above as the **caption**; on edit, `EditMessageMedia` (photo + caption); if the photo send fails → falls back to plain `SendMessage` text. If thumbnails are disabled or there is no artwork → text message with `DisableWebPagePreview: true`. Title is truncated to 25 chars (`ShortTitle(t.Title, 25)`).
- Queued (`play_added_to_queue_single`):
  ```
  <b>{emoji:🎵} Added to Queue:</b> #{index}

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Requested by:</b> {by}
  ```
  Multiple: `play_added_multiple_header` = `{emoji:➕} <b>Added {count} tracks</b> by {user}`; `play_queue_limit_hint` = `{emoji:⚠️} <i>Queue limit reached — some tracks were skipped.</i>`; `play_queue_view_hint` = `<i>Use /queue to view full list.</i>`
- Player markup (`GetPlayMarkup`, `buttons.go:175-215`):
  - Row 1 (only when NOT queued): `[<HH:MM:SS or MM:SS> <progress bar> <duration>]` → callback `progress` (no-op; `emptyCBHandler`, `handlers.go:147`). Progress bar from `utils.GetProgressBar` (see §3.3 note: same bar helper as Arc).
  - Row 2: `[II|▷]` `room:<chat_id>:pause|resume` (toggle: shows `▷` when paused, `II` while playing) · `[⟳]` `room:<chat_id>:replay` · `[‣‣I]` `room:<chat_id>:skip` · `[▢]` `room:<chat_id>:stop`
  - Row 3: `[Close]` `close`
  - Note: **no ±15s seek row** (contrast Tina). Channel play uses prefix `croom:` — the same `roomHandle` regex `^room:-?\d+:\w+$` does **not** match `croom:`; channel callbacks are routed separately (see Arc §4.3 for the shared scheme).
- Single-action toggle keyboards (`getActionToggle`): pause toggle = `[⏸️ Pause]` / `[▶️ Resume]` → `room:<chat_id>:pause|resume`; mute toggle = `[🔇 Mute]` / `[🔊 Unmute]` → `room:<chat_id>:mute|unmute`.
- Stop confirm (`GetStopConfirmMarkup`, `buttons.go:140-173`): single row `[▶️ Resume|🔊 Unmute]` (green) + `[⏹️ Stop]` (red) → `room:<chat_id>:resume|unmute` and `room:<chat_id>:stop`. Texts: `stop_confirm_paused` / `stop_confirm_muted` (Appendix C).
- Room callback actions (`internal/modules/callback.go:103-115`): `pause`, `resume`, `replay`, `skip`, `stop`, `mute`, `unmute` (+ status cases `paused`/`playing`/`muted`/`unmuted` at :508-529). Toasts/edits (`cb_*`, Appendix C): pause → toast `⏸️ Track paused at {position}` + card `cb_pause_message` (`<b>⏸️ Track Paused</b>` … Position … Paused by); resume → `▶️ Resumed at {position}.` + card `cb_resume_message`; replay → `🔁 Track replayed.` / `🔁 Track replayed by {user}.`; skip → `⏭️ Track skipped.` / `⏭️ Skipped by {user}`; stop → `⏹️ Playback stopped.`; mute → `🔇 Muted.` + card `cb_mute_message`; unmute → `🔊 Unmuted.` + card `cb_unmute_message`; already-states `🔇 Track is already muted.` / `🔊 Track is not muted.`; failures `❌ Failed to pause/resume/mute/unmute playback.`
- Restart confirm (`GetRestartConfirmMarkup`): single row `[Restart the Bot]` `restart:bot` (red) · `[Replay the Song]` `restart:replay` (green). Text `restart_confirm_running` = `{emoji:⚠️} A song is currently playing in this chat.\nChoose an action below.`; `restart_confirm_bot` = `♻️ Restarting the bot...`; `restart_confirm_replay_done` = `⏮️ Current song replayed successfully.`

## 3.4 Queue display

- `/queue` rich card (`queueRich`, `internal/modules/queue.go:122-198`): first page shows **15** up-next tracks (`queueFirstPage = 15`).
  ```
  <h3>🎶 Current Queue</h3>
  <aside><b>▶️ Now Playing</b><br><a href="{url}">{title≤40}</a><br>⏱️ {duration}  •  👤 {requester}</aside>
  <p><b>⏭️ Up Next • {count} tracks</b></p>
  <table bordered striped compact>
  <tr><th>#</th><th>🎵 Track</th><th>⏱️</th><th>👤</th><th>🗑️</th></tr>
  <tr><td>{i+1}</td><td><a href="{url}">{title≤40}</a></td><td>{duration}</td><td>{requester}</td><td><tg-button type="callback_data" style="danger" data="qdel:{chat_id}:{token}:{shown}">⨯</tg-button></td></tr>
  …
  </table>
  <tg-button-row align="center"><tg-button type="callback_data" data="qmore:{chat_id}">➕ Show {count} more</tg-button></tg-button-row>   (only if more remain)
  <footer>📭 <i>No more songs in queue.</i></footer>   (only if queue empty)
  ```
  Delete token: track ID if ≤20 chars and no `:`, else `tok_` + first 10 bytes of SHA-256 hex (`trackDeleteToken`). Callbacks: `qdel:<chat_id>:<token>:<shown>` → `queueDeleteHandler` (toast `🗑️ Removed from queue.`), `qmore:<chat_id>` → `queueMoreHandler` (reveals the rest).
- Queue strings (Appendix C): `queue_header` = `{emoji:🎶} Current Queue`; `queue_now_playing` = `{emoji:▶️} Now Playing`; `queue_up_next` = `{emoji:⏭️} Up Next • {count} tracks`; `queue_more_button` = `{emoji:➕} Show {count} more`; `queue_empty_tail` = `{emoji:📭} <i>No more songs in queue.</i>`; `queue_empty` = `{emoji:⚠️} <b>The queue is already empty.</b>`; `queue_no_active` = `{emoji:⚠️} <b>No active playback.</b>\n\n<i>Nothing is queued right now.</i>`; `queue_limit_reached` = `{emoji:⚠️} Queue limit reached ({limit} tracks max). Use /clear to clear queue.`
- Queue finished: `stream_queue_finished` = `{emoji:🎵} <b>Queue Finished!</b>\n\nNo more songs to play. Use /play to add more tracks! {emoji:🎶}`; next-track `stream_downloading_next` = `{emoji:📥} Downloading your next track...`; autoplay next `stream_autoplay_next` = `{emoji:🔁} Autoplay: <b>{title}</b>`.

## 3.5 Lyrics / search results

- **Lyrics: none.** (The word "lyric" appears in `auth.go`, `dashboard.go`, `help.go`, `helpers.go`, `mute.go` only as part of other words/strings — no lyric feature.)
- **Search results:** no results list. `/play` flow: `searching` = `{emoji:🔍}{emoji:🎶} Searching... {emoji:⚡}{emoji:✨}`; `searching_query` = `{emoji:🔍}{emoji:🎶} Searching for: <b>{query}</b>... {emoji:⚡}{emoji:✨}`; `play_downloading_song` = `{emoji:📥} Downloading song "{title}"`; `play_download_canceled` = `{emoji:⚠️} Download canceled by {user}.`; no-query `no_song_query` = `<b>{emoji:🎵} Whoops! Nothing to play.</b>\n\n<i>Send <b>{cmd} [song name or link]</b>, or reply to a <b>media</b> message to start the music.</i>`; not-found `no_song_found` = `{emoji:❌} No tracks found.`; already-in-queue `play_already_in_queue` = `{emoji:✅} <b>Track already added!</b>\n\n<i>{title}</i> is already playing or in the queue.`; already-playing `play_already_playing` = `{emoji:⚠️} <b>A song is already playing.</b>\n\nPlease wait for it to finish or use <code>/stop</code> to stop it first.`; too-long / all-skipped variants (Appendix C); `play_failed` = `{emoji:❌} Failed to play\nError: {error}`; `play_unable` = `<b> {emoji:❌} Unable to Play </b>\n\n<i>Please <b>restart the voice</b> chat.</i>`; RTMP refusal `rtmp_play_unsupported` = `{emoji:⚠️} <b>Can't play in an RTMP/live-stream voice chat.</b>\n\n<i>Playback stopped.</i>`

## 3.6 Download progress

- (`download_progress`, `internal/modules/download.go:207-214` — edited in place on the status message, keeping its reply markup):
  ```
  {emoji:📥} Downloading your track...

  <b>Progress:</b> {percentage}%
  <b>Speed:</b> {speed}
  <b>ETA:</b> {eta}
  <b>Elapsed:</b> {elapsed}
  ```
  Percentage formatted `%.1f`, speed as `FormatBytes/s`, ETA/duration via `FormatDuration`, elapsed via `FormatTime`.
- Cancel keyboard (`GetCancelKeyboard`): single `[🚦 Cancel Downloading]` → callback `cancel` (`handlers.go:140`). Toasts: `download_cancelled` = `✅ Download cancelled.`; `no_download_to_cancel` = `ℹ️ No download to cancel.`

## 3.7 Settings menu + admin panels

- `/settings` (`internal/modules/settings.go:230-250`): text `settings_main` —
  ```
  <h2>{emoji:⚙️} Chat Settings</h2>
  <p><b>Title:</b> {title}<br/><b>ChatID:</b> <code>{id}</code></p>
  <p>Configure the bot's behavior in this group using the buttons below.</p>
  ```
  Keyboard (`buildSettingsMarkup`, `settings.go:333-456`) — 8 rows, each `[label] [value]`:
  - Row 1: `[🛡️ Admin Mode]` `info:adminmode` · `[<Admins 🔐|Auth Users 🎧|Everyone 🌍>]` `set:adminmode`
  - Row 2: `[🎵 Play Mode]` `info:playmode` · `[<Admins 🔐|Everyone 🌍>]` `set:playmode`
  - Row 3: `[🗑️ Cmd Delete]` `info:cmddelete` · `[<Enabled|Disabled>]` `set:cmddelete`
  - Row 4: `[🧹 Clean Mode]` `info:cleanmode` · `[<Enabled|Disabled>]` `set:cleanmode`
  - Row 5: `[⏱️ Clean Delay]` `info:cleanduration` · `[<15m|30m|60m|5m>]` `set:cleanduration`
  - Row 6: `[🖼️ Thumbnails]` `info:nothumb` · `[<Enabled|Disabled>]` `set:nothumb`
  - Row 7: `[🌐 Language]` `info:lang` · `[<current language name>]` `lang:select`
  - Row 8: `[Close]` `close`
  - `info:*` answers an alert with `settings_info_*` (Appendix C); `set:*` toggles/cycles and re-edits the panel; toast `settings_updated` = `✅ Settings updated!`. Settings callback is admin-gated (`only_admin_cb` alert on denial). Admin-mode cycles admins → adminauth → everyone.
- **Dashboards** (shared toggle-card pattern, `internal/modules/dashboard.go`): each of these commands opens a rich card —
  ```
  <h3>{title}</h3>
  <aside><i>{description}</i></aside>
  <p>ℹ️ Status: <b>Enabled ✅ | Disabled ❌</b></p>
  ```
  with keyboard single row `[Close]` `close` · `[Enabled ✅|Disabled ❌]` `dashboard:<name>`. Tapping the toggle flips the setting, refreshes the card in place, and answers toast `✅ Setting updated to {status}.` (`dashboard_toggled_cb`; failure → `⚠️ Failed to update the setting.`). Registered dashboards: `autoplay` (title `{emoji:🔁} Autoplay`, desc `Once the queue ends, the bot automatically keeps playing recommended songs similar to the last played track.`), `autoleave` (title `{emoji:🏃‍♂️} AutoLeave Control`, desc `When enabled, the assistant automatically leaves inactive or unnecessary chats every 10 minutes.`), `maintenance` (title `{emoji:🛠️} Maintenance`, desc `When enabled, all active rooms are stopped and only the owner/sudo users can use the bot.`), `logger` (title `{emoji:📝} Logger`, desc `When enabled, the bot logs playback activity and events to the logger channel.`), `cmddelete` (title `{emoji:🗑️} Command Delete`, desc `When enabled, I will automatically delete command messages after handling.`), `cleanmode` (title `{emoji:🧹} Clean Mode`, desc `When enabled, bot service messages and command messages are auto-deleted after a short delay.`), `playmode` (title `{emoji:🎵} Play Mode`, desc `When enabled, only admins and authorized users can use /play.`), `nothumb` (title `{emoji:🖼️} Thumbnails`, desc `When enabled, playback messages will not show artwork/thumbnails.`).
- Language picker (`buildLangMarkup`, `internal/modules/language.go:66-97`): one button per language, 2 per row, current language prefixed `✔️ `, callback `lang:<code>`; optional back row `[⬅️ Back]` → callback `start` (from start card) or `help:main` (from help). Prompt `lang_select` = `{emoji:🌐} Select your preferred language:`; `lang_success` = `{emoji:✅} Language successfully updated to {lang_name}.`; `lang_same` = `ℹ️ You’re already using this language.`; `lang_fail` = `❌ Failed to update language. Please try again later.`
- Auth list (rich table, `internal/modules/auth.go:238-278`):
  ```
  <h3>🔐 Authorized Users</h3>
  <p>👥 {count}/{limit} authorized</p>
  <table bordered striped compact>
  <tr><th>👤 User</th><th>🆔 ID</th><th>⨯</th></tr>
  <tr><td>{name|<i>Unknown</i>}</td><td>{id}</td><td><tg-button … data="authdel:{chat_id}:{user_id}">⨯</tg-button></td></tr>
  …
  </table>
  <footer>Tap ⨯ to revoke access.</footer>
  ```
  Delete toast: `🗑️ Authorization revoked.` (`authlist_delete_success`); failures `del_auth_not_authorized` / `del_auth_remove_fail` (Appendix C).
- Broadcast (`internal/modules/broadcast.go`): progress message edited in place — header `{emoji:📡} Broadcasting...`, lines `📊 Total Chats: {done}/{total} ({progress}%)`, `👥 Total Users: {done}/{total} ({progress}%)`, `❌ Failed Chats: {count}`, `❌ Failed Users: {count}`, `⏰ Elapsed: {elapsed}`, `✨ Success Rate: {rate}% ({sent}/{total})`; cancel keyboard `[🚫 Cancel Broadcast]` → `bcast_cancel` (`GetBroadcastCancelKeyboard`); `broadcast_cancel_owner_only` = `⚠️ Only the owner can cancel broadcasts.`; `broadcast_already_running` (with `/broadcast -cancel` hint).
- Command-form settings (Appendix C): `adminmode_help` (+ `adminmode_updated` = `{emoji:✅} <b>Admin command mode</b> updated to: {status}`), `playmode_*`, `loop_*` (0–10), `speed_*` (0.50x–4.00x; `speed_current`, `speed_already_set`, `speed_reset_success` = `{emoji:✅} Playback speed reset to <b>1.0x</b> by {user}`), `mute_*`/`unmute_*` (rich `<h3>` + `<table>` cards), `pause_*`/`resume_*` (rich `<h3>` + `<table>` cards), `seek_*/jump_*`, `move_*/remove_*`, `shuffle_*`, `reload_*` (same per-line summary pattern as Tina/Arc).
- Sudo list: `sudo_list_header` = `{emoji:👑} <b>Current Sudoers:</b>`; `sudo_list_owner` = `{index}. {user} — <b>Owner</b>`; `sudo_list_entry` = `{index}. {user}`; `sudo_list_no_extra` = `{emoji:⚠️} No additional sudoers found.`; same owner/bot jokes as Tina (`sudo_owner_self`, `sudo_bot_self`, `sudo_bot_user`).
- Blacklist: `blacklist_list_title` = `{emoji:📛} <b>Blacklisted</b>`; `blacklist_list_chats` = `{emoji:💬} <b>Chats:</b>`; `blacklist_list_users` = `{emoji:👤} <b>Users:</b>`.
- Channel play (`cplay_*`, Appendix C): `cplay_usage` = `<b>Usage:</b> <code>/setcplay &lt;channel_id | @username | linked | off&gt;</code>`; `cplay_enabled` = `{emoji:✅} <b>Channel Play enabled.</b> All channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) will now work in channel <code>{channel_id}</code>.`

## 3.8 Ping / stats / privacy

- `/ping`: first `ping_start` = `{emoji:🏓} Pinging...`, then `ping_result` (verbatim):
  ```
  <b>{emoji:🏓} Pong!</b> • <code>{latency}ms</code>

  <b>{emoji:🤖} {bot}</b>
  <blockquote>{emoji:⏱️} <b>Uptime:</b> <code>{uptime}</code>
  {emoji:💾} <b>RAM:</b> <code>{ram_info}</code>
  {emoji:🖥️} <b>CPU:</b> <code>{cpu_usage}</code>
  {emoji:💽} <b>Disk:</b> <code>{disk_usage}</code></blockquote>
  ```
- `/stats` (sudo-only; non-sudo gets no reply — `checkSudo` returns nil, `internal/modules/stats.go:77-87`): code-built rich message (`buildStats`, `stats.go:93-221`), no locale keys. Structure, verbatim:
  ```
  <h3>📊 Bot Statistics</h3>
  <aside><i>Live overview of the host, resources and database.</i></aside>

  🔧 System (table rows): OS | <code>{GOOS} · {GOARCH}</code>; Kernel | <code>{kernel}</code>; Host | <code>{hostname}</code>; CPUs | <code>{n}</code>; Goroutines | <code>{n}</code>; Uptime | <code>{duration}</code>
  📦 Memory (Go): Alloc | <code>{n} MB</code>; Heap | <code>{n} MB</code>; Stack | <code>{n} MB</code>; Sys | <code>{n} MB</code>; GC runs | <code>{n}</code> {emoji} <code>{x.x}/min</code>
  💻 Server: Load Avg | <code>{1m} · {5m} · {15m}</code>; CPU | {emoji} <code>{x.xx}%</code>; RAM | {emoji} <code>{used} / {total} GiB</code>; Swap | <code>{used} / {total} GiB</code>; Process RAM | {emoji} <code>{bytes} ({x.x}%)</code>; Threads | <code>{n}</code>; Disk | <code>{used} / {total} GiB</code>
  <details><summary>🌐 MongoDB</summary><table bordered striped compact> rows: Databases | <code>{n}</code>; per-database <code>{name}</code> | <code>{size}</code></details>
  <details><summary>📂 Collections · <code>{dbName}</code></summary><table …> rows: <code>{coll}</code> | <code>{storage}</code> | <code>{objects}</code></details>
  👥 Bot Overview: Served Chats | <code>{n}</code>; Served Users | <code>{n}</code>; Sudoers | <code>{n}</code>; Blocked Users | <code>{n}</code>; Blocked Chats | <code>{n}</code>; Auto Leave | {✅/❌}; Logger | {✅/❌}; Maintenance | {✅/❌}
  🧩 Versions: Go | <code>{ver}</code>; gotdbot | <code>{ver}</code>; TDLib | <code>{ver}</code>; (+ further rows past line 221 — see stats.go)
  ```
  (`section()` renders each group as a `<table bordered striped compact>` under a bold heading; `emoji()` picks 🟢/🟡/🔴 by warn/crit thresholds.)
- `/privacy` (`internal/modules/privacy.go:44-84`): full rich page, verbatim —
  ```
  <h3>🛡 Privacy Policy &amp; Data Handling</h3>
  <p>YukkiMusic is built with privacy in mind. Below is exactly what it stores, why it is stored, and how you stay in control.</p>

  <h4>📦 Data We Store</h4>
  <p>Only the information required to run the bot — nothing more:</p>
  <table bordered striped compact>
  <tr><th>Stored</th><th>Purpose</th></tr>
  <tr><td><b>User &amp; chat IDs</b></td><td>Identify groups and users so your settings apply</td></tr>
  <tr><td><b>Preferences</b></td><td>Language, play mode, clean mode, admin mode, thumbnails</td></tr>
  <tr><td><b>Access lists</b></td><td>Authorized users, sudo users, and the blacklist</td></tr>
  </table>

  <h4>💬 Messages</h4>
  <aside>The bot reads <b>only</b> messages that begin with one of its commands (for example <code>/play</code>) or button presses on its own messages. It <b>never</b> reads, stores, or monitors your private chats or ordinary group conversations.</aside>

  <h4>🌐 Third-Party Services</h4>
  <p>To find and stream the music you request, the bot sends the search query to services such as <b>YouTube</b> and <b>Spotify</b>. Only the query itself is sent — <b>no personal data</b> is shared with these services.</p>

  <h4>🔒 Data Sharing</h4>
  <p>Your data is <b>never</b> sold, shared, or traded with third parties. It is used strictly to operate and improve the bot's music streaming features.</p>

  <h4>🗑 Data Removal</h4>
  <p>Access can be cleaned up at any time: the owner can manage authorized users, sudo users, and the blacklist. You may also contact support to request removal of stored data.</p>

  <footer>YukkiMusic is an <a href="{repo}">open-source project</a>. If you have any questions, feel free to join our <a href="{support}">Support Chat</a>.</footer>
  ```
  (repo = `https://github.com/TheTeamVivek/YukkiMusic`; support = config SupportChat else SupportChannel else `https://t.me/TheTeamVk`.)
- `/active`: `active.go` helpText + count rendering (see Appendix E).
- Flood: `flood_seconds` = `⏳ Slow down! Try again in {duration} seconds.`; `flood_minutes` = `{emoji:⏳} Please wait {duration} minutes before using this command again.`

## 3.9 Errors & guards (verbatim, Appendix C for full list)

- `only_admin` (rich): `<h4>{emoji:🚫} Access Denied</h4>\n<aside>{emoji:🔒} This action is limited to <b>chat admins</b>.</aside>\n<p>You can still <code>/play</code> songs and enjoy the music.</p>\n<footer><i>{emoji:💡} Admin? Run <code>/reload</code> to refresh permissions.</i></footer>`; `only_admin_cb` = `⚠️ Only admins can do these actions.`; `only_admin_or_auth` (rich variant) + `only_admin_or_auth_cb`; `only_supergroup` (rich, with in-body `<tg-button type="url" style="primary" url="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">📖 How to Convert a Group to Supergroup</tg-button>` and a Support Group tg-button); `only_owner` = `⚠️ Only the bot owner can use this command.`; `only_sudo` / `only_sudo_cb` = `{emoji:⚠️} Only sudo users or the bot owner can use this command.`; `only_owner_cb` = `⚠️ Only the bot owner can use this command.`
- `supergroup_needed` (rich variant of `only_supergroup`, same tg-buttons).
- `err_*` family (Appendix C): `err_assistant_banned`, `err_admin_permission_required`, `err_assistant_get_failed`, `err_assistant_join_request_sent`, `err_assistant_invite_link_fetch`, `err_assistant_invite_failed`, `err_connection_timeout`, `err_no_active_voicechat`, `err_fetch_failed`, `err_unknown`, `err_fetch_tracks`, `err_download_internal`.
- Maintenance: `maint` = `⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later.` (no reason slot in Yukki — contrast Tina/Arc `maint_reason`); dashboard desc (see 3.7).
- Bot added: `bot_added_normal` = `{emoji:🎵} Thanks for adding me! Use /play [song] to start playing music.`; demotion `bot_demotion_goodbye` blockquote; assistant restricted `assistant_restricted_warning` blockquote.
- Voice-chat events: `voicechat_ended` = `<b>{emoji:⚠️} Video chat ended</b>\n\n<i>{emoji:⏰} Duration: {duration}</i>`; `voicechat_started` = `<b>{emoji:♻️} Voice chat started!</b>\n\n<i>Use /play [song name] to play music.</i>`
- Logger channel messages: `logger_bot_added`, `logger_bot_removed`, `logger_bot_started`, `logger_playback_template` (Appendix C).
- `logs_empty` = `No logs found`; `room_no_active` = `⚠️ Bot is not streaming in a voice chat. Start playing a song first.`; `room_not_active_cb` = `⚠️ Nothing playing right now.`; `room_already_paused` = `⏸️ Track is already paused. Tap ▶️ Resume to continue.`; `room_pause_failed` (rich `<h3>`).
- Restart: `restart` = `{emoji:♻️} Restarting...`; `restart_service` = `{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds.`; `restart_initiated` = `{emoji:✅} Restart initiated successfully!`; `restart_fail` = `{emoji:❌} Failed to restart: {error}`.

## 3.10 Callback inventory (complete, `internal/modules/handlers.go:116-147` + `buttons.go`)

| callback_data | handler | purpose |
|---|---|---|
| `help_cb` | helpCB | open help menu from start card |
| `help:<admins\|public\|owner\|sudoers\|main>` | helpCallbackHandler | help category pages |
| `start` | startCB | back to start card |
| `lang:select` | langCallbackHandler | open language picker |
| `lang:<code>` | langCallbackHandler | set language |
| `set:<adminmode\|playmode\|cmddelete\|cleanmode\|cleanduration\|nothumb>` | settingsCallbackHandler | toggle/cycle a setting |
| `info:<adminmode\|playmode\|cmddelete\|cleanmode\|cleanduration\|nothumb\|lang\|autoplay>` | settingsCallbackHandler | alert with the setting's explainer |
| `dashboard:<autoplay\|autoleave\|maintenance\|logger\|cmddelete\|cleanmode\|playmode\|nothumb>` | dashboardCallback | toggle a dashboard setting |
| `room:<chat_id>:<pause\|resume\|replay\|skip\|stop\|mute\|unmute>` | roomHandle | player controls |
| `progress` | emptyCBHandler | no-op (progress bar button) |
| `qdel:<chat_id>:<token>:<shown>` | queueDeleteHandler | delete one queued track |
| `qmore:<chat_id>` | queueMoreHandler | reveal full queue |
| `authdel:<chat_id>:<user_id>` | authDeleteHandler | revoke an authorized user |
| `restart:<bot\|replay>` | restartConfirmHandler | restart confirm |
| `cancel` | cancelHandler | cancel download |
| `bcast_cancel` | broadcastCancelCB | cancel broadcast |
| `close` | closeHandler | delete the message |

## 3.11 Inline mode / reply keyboards / other

- **Inline mode: none.** No inline-query handler (the word "inline" appears only in `app.json`, `config/README.md`, `buttons.go` for inline keyboards, `broadcast.go`, `dashboard.go`).
- **Reply keyboards: none.**
- Bot-command scopes (`internal/modules/comm.go`): PrivateUser (start, help, language, ping), PrivateSudo (ac, stats, logger, autoleave), PrivateOwner (addsudo, delsudo, blockuser, unblockuser, blockchat, unblockchat, blacklisted, maintenance), GroupUser (play, queue, position, reload, authlist), GroupAdmin (all GroupUser + play, cplay, fplay, cfplay, pause, cpause, resume, cresume, skip, cskip, replay, creplay, end, cstop, mute, unmute, cmute, cunmute, seek, seekback, cseek, cseekback, speed, cspeed, queue, cqueue, position, cposition, jump, cjump, remove, cremove, move, cmove, clear, cclear, shuffle, cshuffle, loop, cloop, setcplay, playmode, adminmode, cmddelete, autoplay, settings, addauth, delauth, reload, creload). Sudo users get PrivateUser+PrivateSudo scoped to their own chat; the owner gets all three private groups.
- Command descriptions (verbatim, `comm.go`): e.g. `start` → `🚀 Start the bot`; `help` → `📖 Show the help menu`; `language` → `🌐 Change the bot language`; `ping` → `🏓 Check if the bot is alive`; `play` → `🎵 Play a song`; `queue` → `📜 Show the queue`; `position` → `⏱️ Show the current position of the song`; `reload` → `🔄 Reload the admin cache`; `authlist` → `👥 List authorized users`; `fplay` → `⏩ Force play a song`; `end` → `⏹️ Stop the song and leave voice chat`; `speed` → `⚡ Set the playback speed`; `loop` → `🔁 Loop the current song`; `setcplay` → `⚙️ Configure channelplay for your chat`; `autoplay` → `🔁 Toggle autoplay recommendations`; `settings` → `⚙️ Configure chat settings`; (full list in §3.11 above / `comm.go`).

---

# 4. ArcMusic — `refs/ArcMusic/` (Go, gogram — plain HTML, no rich elements)

**Primary source:** `internal/locales/en.yml` (997 lines — reproduced verbatim in Appendix D).
**Keyboard source:** `internal/core/buttons.go` (all markups). **Callback routing:** `internal/modules/handlers.go` (command patterns) + `internal/modules/cb.go` (callback actions).
**Style:** plain HTML only (`<b>`, `<i>`, `<u>`, `<code>`, `<a href>`, `<blockquote>`). **No** `{emoji:}` tokens, **no** `<h*>`/`<table>`/`<tg-button>` rich elements, **no** custom-emoji icons or button styles — this is the plain-text sibling of Yukki/Tina. Button labels are plain words (`Add Me to Your group`, `Help & Commands`, `Close`). Arc is the only one of the four music bots with a **`/logs` viewer UI** and the most detailed **RTMP setup guide**.

## 4.1 /start

- PM (`start_private`): `Hello {user}, My name is {bot}.\n\nI'm a telegram streaming bot with some useful features. Supporting platforms like Youtube, Spotify, JioSaavn, Soundcloud etc.\n\nFeel free to add me to your groups.` (plain text, no HTML — `internal/modules/start.go:50`).
- Group (`start_group`): `<b>💫 I’m here!</b> Try a command… if you think you can handle me 😉`
- Start keyboard (`GetStartMarkup`, `internal/core/buttons.go`):
  - Row 1: `[Add Me to Your group]` → URL `https://t.me/<bot>?startgroup&admin=invite_users`
  - Row 2: `[Help & Commands]` → callback `help_cb`
  - Row 3: `[Updates]` → URL SupportChannel · `[Support]` → URL SupportChat
  - Row 4: `[Source]` → URL `https://github.com/tusar404/ArcMusic`
  - Note: **no Language button** on the start card (contrast Yukki row 4).

## 4.2 /help — full text

- Menu text (`help_main`): `📚 <b>Help Menu</b>\n\nSelect a category below to explore detailed commands and their usage for managing, controlling, and customizing the bot.`
- Help keyboard (`GetHelpKeyboard`): identical layout to Yukki — Row 1 `[🛠 Admins]` `help:admins` · `[🌍 Public]` `help:public`; Row 2 `[👑 Owner]` `help:owner` · `[⚡ Sudoers]` `help:sudoers`; Row 3 `[⬅️ Back]` `start`. Category pages use `GetBackKeyboard`: `[⬅️ Back]` `help:main`.
- Category texts (verbatim in Appendix D, plain-HTML versions of Yukki's): `help_owner` (bold list: /addsudo, /delsudo, /eval, /maintenance, /restart, /sh), `help_sudo` (/active, /autoleave, /logger, /stats, /logs), `help_admin` (bold list: /playmode, /cmddelete, /speed, /skip, /pause, /resume, /replay, /mute, /unmute, /seek, /seekback, /jump, /move, /clear, /remove, /shuffle, /loop, /stop, /nothumb), `help_public` (/play, /queue, /ping, /start, /help, /bug, /position, /reload, /json, /sudolist, /privacy — note the typo in the source: `/privacy<b> - open privacy policy` — unclosed `<b>` in the locale file).
- **Per-command help** (`/help <command>`): same `helpTexts` system as Yukki (see Appendix E for the extracted set; Arc's texts are the plain-HTML counterparts, e.g. `/active` = `<b>Show all active voice chat sessions.</b>`). Channel variants auto-generated the same way (`handlers.go:520-521`).
- In-group /help: `help_private_only` = `🤖 Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!` Group help button: `[📒 Commands]` → URL `https://t.me/<bot>?start=pm_help`.

## 4.3 Now-playing card

- `stream_now_playing` (verbatim):
  ```
  <b>🎵 Now Playing:</b>

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Requested by:</b> {by}
  ```
  (No `▫` bullets, no emoji token — plain labels. Thumbnail: Arc sends the same text card; the code path uses the same `showTrackMessage`-style photo logic as Yukki only if artwork exists — Arc's `buttons.go`/`cb.go` reference `room:`/`croom:` prefixes and the locale has `nothumb_*` keys; no separate photo-caption code was found in the sampled files, so port workers should treat Arc's card as **text-first with thumbnail optional**.)
- Queued (`play_added_to_queue_single`):
  ```
  <b>Added to Queue:</b> #{index}

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Requested by:</b> {by}
  ```
  Multiple: `play_added_multiple_header` = `➕ <b>Added {count} tracks</b> by {user}`; same limit/view hints as Yukki (plain, no emoji tokens).
- Player markup (`GetPlayMarkup`, `internal/core/buttons.go`):
  - Row 1 (only when NOT queued): `[<MM:SS> <progress bar> <MM:SS>]` → callback `progress`
  - Row 2: `[▷]` `room:resume` · `[II]` `room:pause` · `[‣‣I]` `room:skip` · `[⟳]` `room:replay` · `[▢]` `room:stop` — **note:** Arc's row 2 shows BOTH ▷ and II (5 buttons, no pause-state toggle) and puts replay in the main row (contrast Yukki's 4-button toggle row + replay in row 2). Also note the replay callback here is the bare `room:replay` (no chat-id prefix) while the others use the `room:`/`croom:` prefix variable — an Arc quirk to preserve.
  - Row 3: `[Close]` `close`
  - Prefix: `room:` normally, `croom:` when `r.ChannelPlayID() != 0`. Callback router (`internal/modules/cb.go:72-73,84`): strips `croom:` then `room:`; actions include `seek`/`seekback_` (line 121, 491) — Arc supports `seek_15`/`seekback_15`-style callbacks in the player card flow via `cb.go` (Tina builds those buttons; Arc's `GetPlayMarkup` does not, but the router handles them).
- Stop confirm (`GetStopConfirmMarkup`): **two rows, not one** (contrast Yukki): Row 1 `[▶️ Resume]` `room:resume` (or `[🔊 Unmute]` `room:unmute` when muted) · Row 2 `[⏹️ Stop]` `room:stop`. Same `stop_confirm_paused`/`stop_confirm_muted` texts (Appendix D).
- Callback toasts/edits (`cb_*`, Appendix D): pause → `⏸️ Track paused at {position}` + card `cb_pause_message` (`<b>⏸️ Track Paused</b>` … `▫ Position:` …); resume → `▶️ Resumed at {position}.` + card `cb_resume_message` (plain `Track:`/`Duration:`/`Resumed by:` labels, no `▫`); replay → `🔁 <b>Replaying current track...</b>` / `🔁 Track replayed.` / `🔁 Track replayed by {user}.`; skip → `⏭️ Track skipped.` / `⏭️ Skipped by {user}`; stop → `⏹️ Playback stopped.`; mute → `🔇 Muted.` + card `cb_mute_message` (plain labels); unmute → `🔊 Unmuted.`; seek → `⏩ Sought forward {seconds} seconds` / `⏪ Sought back {seconds} seconds` (+ `… — by {user}` edited variants); near-end guard `⚠️ Cannot seek forward {seconds} seconds — about to reach end.`
- **No restart-confirm markup** in Arc's `buttons.go` (Yukki/Tina have `GetRestartConfirmMarkup`; Arc has restart strings in the locale but no confirm keyboard — restart is immediate).

## 4.4 Queue display

- `/queue` (`internal/modules/queue.go:169-280`): plain text, built as —
  ```
  {queue_header}\n\n
  {queue_now_playing}\n
  🎧 <a href="{url}">{title≤35}</a> — {requester} [{duration}]\n\n
  {queue_up_next}\n
  1. 🎵 <a href="{url}">{title≤35}</a> — {requester} [{duration}]\n
  … (first 10 only)
  ```
  - If ≥3 waiting tracks, the up-next list is wrapped in `<blockquote>…</blockquote>`; if fewer, plain lines with a blank line.
  - If >10 tracks: builds a **full plain-text queue** (no links, no truncation: `{i}. {title} — {requester} [{duration}]`) and uploads it via `utils.CreatePaste` (pastebin-style link); the visible message then appends `queue_more_line` = `\n… and {remaining} more` where `{remaining}` is a **hyperlink** `<a href="{paste_link}">{count}</a>` when the paste succeeded, else the bare number. No buttons, no pagination, no per-track delete.
  - Empty tail: `queue_empty_tail` = `📭 No more songs in queue.`; `queue_header` = `🎶 <b>Current Queue</b>`; `queue_now_playing` = `▶️ <b>Now Playing:</b>`; `queue_up_next` = `⏭️ <b>Up Next:</b>`.
- Queue finished: `stream_queue_finished` = `No more songs in the queue.` (plain, no emoji); next-track `stream_downloading_next` = `Hold On !!\n\n📥 Downloading your next track...`; no autoplay-next string in Arc.
- Queue ops strings (Appendix D): `queue_restored` = `✅ Successfully restored <b>{count}</b> tracks to the queue.`; `queue_restore_no_data` = `❌ No cleared queue found to restore, or a new song has already been added.`; `queue_empty` = `⚠️ <b>The queue is already empty.</b>`; `queue_no_active` = `⚠️ <b>No active playback.</b>\n\nNothing is queued right now.`; `queue_limit_reached` = `⚠️ Queue limit reached ({limit} tracks max). Use /clear to clear queue.`

## 4.5 Lyrics / search results

- **Lyrics: none.**
- **Search results:** no results list. `/play` flow: `searching` = `🔍 Searching...`; `searching_query` = `🔍 Searching for: <b>{query}</b>...`; `play_downloading_song` = `📥 Downloading song "{title}"`; `play_download_canceled` = `⚠️ Download canceled by {user}.`; no-query `no_song_query` = `<b>🎵 Whoops! No song detected.</b>\n\nType <b>{cmd} [song name]</b> or reply to a <b>media</b> to start playing!`; not-found `no_song_found` = `❌ No tracks found.`; `play_failed` = `❌ Failed to play\nError: {error}`; `play_unable` = `<b> ❌ Unable to Play </b>\n\n<i>Please <b>restart the voice</b> chat.</i>`; `play_download_failed` (with title + error in `<code>`); too-long / all-skipped variants (Appendix D).

## 4.6 Download progress

- No percentage/progress message. Cancel: locale `DOWNLOAD_CANCEL_BTN` = `🚦 Cancel Downloading` → callback `cancel` (`GetCancelKeyboard`, single button); toasts `download_cancelled` = `✅ Download cancelled.` / `no_download_to_cancel` = `ℹ️ No download to cancel.`; `invalid_request` = `⚠️ Invalid request.`; `unknown_action` = `⚠️ Unknown action.`

## 4.7 Settings — commands only, NO panel

- **Arc has no `/settings` panel keyboard.** `internal/modules/settings.go` implements only two command-form settings:
  - `/playmode [enable|disable]`: `playmode_help` = `🛠️ <b>Play Mode</b>\n\n<b>Status:</b> {status}\n<b>Usage:</b> <code>/playmode [enable|disable]</code>`; statuses `playmode_status_admins` = `<b>Admins Only</b> 🔐`, `playmode_status_everyone` = `<b>Everyone</b> 🌍`; `playmode_success_admins` = `🛡️ <b>Play mode</b> is now restricted to <b>Admins & DJs</b> only. 🎧`; `playmode_success_everyone` = `✅ <b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow! 🎶`; `playmode_restricted` = `🚫 Play mode is enabled. Only admins and authorized users can play.`
  - `/cmddelete [enable|disable]`: `cmddelete_status` = `🗑️ <b>Command Delete Control</b>\n\nWhen enabled, I will automatically delete command messages.\nUsage: {cmd} [enable|disable]\nCurrent state: {action}`; `cmddelete_updated` = `🗑️ Command deletion has been {action}.`
  - `invalid_bool` = `⚠️ <b>Invalid value.</b>\nUse 'enable' or 'disable'.`
- **There is no settings panel, no `settings_main`, no `settings_btn_*`, no `set:/info:` callbacks, no dashboard system in Arc.** The locale keys `settings_btn_*` / `settings_info_*` / `settings_main` / `settings_updated` / `dashboard_*` / `cleanmode_*` / `autoplay_*` / `nothumb_title` / `cmddelete_title` / `maint_title` / `logger_title` / `authlist_title` **do not exist** in Arc's `en.yml`. What exists instead as command forms: `adminmode` (locale has no `adminmode_help` — Arc's adminmode is code-only? No: grep shows `adminmode` only in `help_admin` text and `comm.go`; there is no `adminmode_*` locale block in Arc), `nothumb_status`/`nothumb_*` (status/usage/fetch-fail/already/update-fail/updated), `autoleave_*` (status/already/update-fail/updated), `logger_*` (usage/status/check-fail/already/update-fail/updated), `maint_*` (+ `maint_reason`), `loop_*` (0–10), `speed_*` (0.50x–4.00x + optional reset duration), `mute_*`/`unmute_*` (incl. auto-unmute), `pause_*` (incl. auto-resume), `seek_*/jump_*`, `move_*/remove_*`, `shuffle_*`, `reload_*` (same per-line summary), `cplay_*` (note: `cplay_id_not_set` = `⚠️ cplay ID not set.\nUse <code>/cplay --set &lt;chat_id&gt;</code> to set it.` and `cplay_usage` = `<b>Usage:</b> <code>/cplay --set &lt;channel_id&gt;</code>` — Arc uses `/cplay --set`, **not** `/setcplay`; also `channel_play_depreciated` = `⚠️ This handler is deprecated. Use <code><a>/cplay --set channel_id </code></a> to set your channel for playback.` — sic, "depreciated" in the source).
- Auth list (plain text, `internal/modules/auth.go:197-238`): header `authlist_header` = `<b>Authorized Users:</b>`; `authlist_fetching` = `⏳ Fetching authorized users list...`; entries `authlist_user_entry` = `{index}. {user} (<code>{user_id}</code>)`; failures `authlist_user_fail` = `{index}. <code>{user_id}</code> (Could not fetch user info)`; `authlist_total` = `<b>Total:</b> {count} users`; `authlist_empty` = `ℹ️ There are no authorized users in this chat.` (no delete buttons, no table — contrast Yukki's rich table + `authdel:` callbacks).
- Language picker (`internal/modules/language.go:36`): buttons `[<name>]` → `lang:<code>`, 2 per row presumably (same builder pattern as Yukki minus custom emoji); prompt `lang_select` = `🌐 Select your preferred language:`; `lang_success` = `✅ Language successfully updated to {lang_name}.`; `lang_same` = `ℹ️ You’re already using this language.`; `lang_fail` = `❌ Failed to update language. Please try again later.` Arc ships **only `en.yml`** (plus a `README.md`) — no ar/hi/tr locale files.
- Broadcast (`internal/modules/broadcast.go`): same progress lines as Tina (`broadcast_progress_header` = `📡 Broadcasting...`, `broadcast_cancelled_header` = `🚫 Broadcast Cancelled`, `broadcast_completed_header` = `✅ Broadcast Completed`, `broadcast_total_chats/users`, `broadcast_failed_chats/users`, `broadcast_delay`, `broadcast_elapsed`, `broadcast_eta`, `broadcast_success_rate`); cancel button `[🚫 Cancel Broadcast]` → `bcast_cancel`; `broadcast_parse_failed` = `❌ Failed to parse broadcast command:\n<code>{error}</code>`.
- Sudo list: same keys/texts as Yukki (plain, no emoji tokens): `sudo_list_header` = `👑 <b>Current Sudoers:</b>` etc.; plus Arc-only `sudo_assistant_self` = `🤖 The assistant is always in the sudo list by default — it cannot be added again.` and `sudo_assistant_cannot_remove` = `🤖 The assistant is required and cannot be removed from the sudo list.`
- Blacklist: `blacklist_list_title` = `📛 <b>Blacklisted</b>` etc. (same as Yukki, plain).
- **Logs viewer (Arc-only, `logs_*` keys):** `logs_invalid_number` = `⚠️ <b>Invalid Number</b>\n\nThe value <code>{value}</code> is not a valid positive number.\n\n<b>Usage:</b> <code>/logs [number]</code> or <code>/logs -old [number]</code>`; `logs_invalid_flag` = `⚠️ <b>Invalid Flag</b>\n\nUnknown flag or invalid input: <code>{flag}</code>\n\n<b>Valid options:</b>\n• <code>/logs</code> - Full log\n• <code>/logs [n]</code> - Last N lines\n• <code>/logs -old [n]</code> - Oldest log\n• <code>/logs -clear</code> - Clear logs`; `logs_file_error`, `logs_send_error`, `logs_clear_failed`, `logs_empty` = `📭 <b>No Logs Available</b>\n\nThe log file either doesn’t exist yet or exists but contains no data.\n\nRun the bot for a while to generate logs.`; `logs_preview_header` = `📋 <b>Current Log Preview:</b>`; `logs_lines_header` = `📋 <b>Recent Log Lines</b> (Last {count})`; `logs_old_header` = `📋 <b>Oldest Log Lines</b> (First {count})`; `logs_uploading` = `⏳ <b>Uploading log file...</b>\n\nPlease wait while the file is being prepared.`; `logs_file_caption` = `📄 <b>Log File Details</b>\n\n<b>Filename:</b> <code>{filename}</code>\n<b>Size:</b> {size_mb} MB\n<b>Last Modified:</b> {modified}`; `logs_lines_caption` / `logs_old_caption`; `logs_cleared` = `✅ <b>Log File Cleared</b>\n\nSuccessfully cleared the log file.\n<b>Previous Size:</b> {size_mb} MB`.
- Stats (Arc-only split keys, no single `stats_overview`): `stats_system_header` = `🔧 <b>System:</b>`; `stats_go_mem_header` = `📦 <b>Internal Memory (Go):</b>`; `stats_server_header` = `💻 <b>Server Stats:</b>`; `stats_served_header` = `📊 <b>Served:</b>`; lines `stats_system_os_arch` = `• OS: <code>{os}</code>, Arch: <code>{arch}</code>`, `stats_system_cpu_goroutines` = `• CPUs: <code>{cpus}</code>, Goroutines: <code>{goroutines}</code>`, `stats_go_alloc` = `• Alloc: <code>{alloc} MB</code>`, `stats_go_sys`, `stats_go_gc` = `• NumGC: <code>{gc_count}</code> ({emoji} {gc_rate}/min)`, `stats_server_cpu/ram/storage`, `stats_served_chats/users` (+ `*_err` variants = `• Chats: <code>Error: {error}</code>`).

## 4.8 Ping

- `/ping`: `ping_start` = `🏓 Pinging...`; `ping_result` (verbatim):
  ```
  <b>🏓 PONG</b>: <code>{latency}ms</code>

  <b><u>🤖 {bot} system stat's :</u></b>

  <b>Uptime:</b> {uptime}
  <b>RAM Usage:</b> {ram_info}
  <b>CPU Usage:</b> {cpu_usage}
  <b>Disk Usage:</b> {disk_usage}
  ```
  (sic: `stat's` with apostrophe in the source.)

## 4.9 Errors & guards (verbatim, Appendix D for full list)

- `only_admin` = `⚠️ <b>Permission Denied!</b>\nOnly <b>admins</b> can control this actions. You can still play songs 🎵.\n\nIf you believe you are an admin, please use /reload to refresh your admin status.` (sic: `this actions`); `only_admin_or_auth` = `⚠️ <b>Access Denied</b>\nOnly <b>admins</b> or <b>authorized users</b> can control this actions.\n\nIf you recently became an admin, use /reload to refresh your permissions.`; `only_admin_or_auth_cb` = `⚠️ Only admins can do this actions.`; `only_supergroup` = `⚠️ This command can only be used in groups.`; `only_owner` = `⚠️ Only the bot owner can use this command.`; `only_sudo` = `⚠️ Only sudo users or the bot owner can use this command.`
- `supergroup_needed` (verbatim, Appendix D): plain text with `<a href="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">How to convert to a SuperGroup</a>` and a dangling `If you have any questions, join our support group:` with no link following (source ends there).
- `err_*` family (Appendix D): same set as Yukki plus `err_assistant_join_rate_limited` = `⚠️ Assistant cannot join this chat because it has reached the maximum number of allowed groups.` and `err_peer_resolve_failed` = `⚠️ Failed to resolve peer information. Try again later or re-add the assistant.`; `err_admin_permission_required` differs: `⚠️ <b>Admin Permission Required</b>\n\nI need admin access to manage and check members in this chat.\n\n➤ <b>Promote me with</b> <code>Manage Chat / Invite Users</code> permission.`
- Maintenance: `maint` = `⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later.\n\n{reason}`; `maint_reason` = `<i>📝 Reason: {reason}</i>`; `maint_usage`, `maint_enabled/disabled`, `maint_already_enabled` = `ℹ️ Maintenance mode is already enabled 🟢.` etc.; `bot_added_maintenance` = `⚠️ I'm currently under maintenance and will be back soon. Thanks for your patience! ⏰`; `maint_entering` = `⚠️ Bot is entering maintenance mode...`; `enabled_with_reason` = `🟢 Enabled\n📝 Reason: {reason}`.
- Bot added: `bot_added_normal` = `🎵 Thanks for adding me! Use /play [song] to start playing music.`; demotion `bot_demotion_goodbye`; assistant restricted `assistant_restricted_warning`.
- Voice-chat events: no `voicechat_started/ended` keys in Arc's locale (Arc has `voicechat.go` module but no locale strings for it — **gap**).
- Logger channel messages: `logger_bot_added`, `logger_bot_removed`, `logger_bot_started` (plain, no `<blockquote>` — contrast Yukki/Tina), plus `logger_track` = `Track:`, `logger_source` = `Source:`, `logger_group` = `Group:`, `logger_requested_by` = `Requested by:`, `logger_timestamp` = `Timestamp:` (label-only keys used to compose the log line in code).
- `logs_empty` — see 4.7; `room_no_active` = `⚠️ <b>No active playback.</b>\n\n<i>There's nothing playing right now.</i>`; `room_not_active_cb` = `⚠️ Nothing playing right now.`; `room_not_linked` = `⚠️ This channel isn't linked to any group.`; `room_already_paused` = `⏸️ Track is already paused. Tap ▶️ Resume to continue.`; `room_already_paused_auto` = `⏸️ Track is already paused — auto-resuming in {duration}.`; `room_pause_failed` = `❌ Failed to pause playback.\n\n<i>{error}</i>`.
- RTMP (most detailed of the four, Appendix D): `rtmp_setup_usage` (full guide with Parameters/Example/How-to-get-chat-ID/Security sections), `rtmp_dm_only` (🔒 Security Notice with 2-step DM instructions), `rtmp_invalid_chat_id`, `rtmp_check_admin_failed`, `rtmp_not_admin`, `rtmp_parse_failed`, `rtmp_configured_success` (with `🆔 Chat ID`, `🔗 URL`, `🔑 Key` — note: prints the stream key in chat), `rtmp_configured_not_started`, `rtmp_status_playing/paused/idle`, `rtmp_not_configured`, `rtmp_already_streaming`, `rtmp_init_failed`, `rtmp_starting_stream` = `📡 <b>Starting RTMP stream...</b>`, `rtmp_play_failed`, `rtmp_now_streaming` (with `⚠️ Note: RTMP has ~15-30s buffering delay`), `rtmp_not_streaming` = `ℹ️ No active RTMP stream.`, `rtmp_stop_failed`, `rtmp_stopped`, `rtmp_streaming_not_supported`.
- Speedtest: `spt_*` (same as Tina: running download/upload + `spt_result` with Client/Server/Speed sections).
- Bug: `bug_usasge` (sic — key is misspelled `bug_usasge` in Arc's locale) = `⚠️ Please describe the bug. Usage: {cmd} <problem> or reply to a message.`; `bug_thanks` = `✅ Thank you for your bug report! It has been sent to the developers.`; `bug_report_format` (same as Tina).
- Restart: `restart` = `♻️ Restarting...`; `restart_service` = `{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds.`; `restart_initiated` = `✅ Restart initiated successfully!`; `restart_fail` = `❌ Failed to restart: {error}`.
- Flood: `flood_seconds` / `flood_minutes` (same as Yukki).

## 4.10 Command inventory (from `internal/modules/comm.go` + `handlers.go` patterns)

- Private user: start, help, ping, sudolist. Private sudo: ac, stats, logger, autoleave. Private owner: addsudo, delsudo, maintenance.
- Group user: play, queue, position, reload, authlist, help, ping.
- Group admin: cplay, cqueue, cposition, fplay, speed, skip, pause, resume, replay, playmode, cmddelete, mute, unmute, seek, seekback, jump, clear, remove, move, shuffle, loop, end, addauth, delauth, channelplay, cfplay, cpause, cresume, cmute, cunmute, cstop, cskip, cloop, cseek, cseekback, cjump, cremove, cmove, cclear, cshuffle, + (from `handlers.go` patterns) json, eval, ev, bash, sh, restart, addsudo/addsudoer/sudoadd, delsudo/delsudoer/sudodel/remsudo/rmsudo/sudorem/dropsudo/unsudo, sudoers/listsudo/sudolist, speedtest/spt, broadcast/gcast/bcast, ac/active/activevc/activevoice, maintenance/maint, logger, autoleave, log/logs, help, ping, start, stats, bug, lang/language, stream, streamstop, streamstatus, rtmp/setrtmp, play, fplay/playforce, cplay, cfplay/fcplay/cplayforce, vplay, fvplay/vfplay/vplayforce, vcplay/cvplay, fvcplay/fvcpay/vcplayforce (sic: `fvcpay`), speed/setspeed/speedup, skip, pause, resume, replay, mute, unmute, …
- Command descriptions (verbatim, `comm.go`): e.g. `start` → `Start the bot.`; `help` → `Show help menu.`; `ping` → `Check if the bot is alive.`; `play` → `Play a song.`; `queue` → `Show the queue.`; `position` → `Show the current position of the song.`; `reload` → `Reload the admin cache.`; `authlist` → `List authorized users.`; `cplay` → `Play a song in the linked channel.`; `fplay` → `Force play a song.`; `speed` → `Set the speed of the song.`; `end` → `Stop the song.`; `loop` → `Loop the current song.`; `channelplay` → `Set a channel as the play channel.`; `maintenance` → `Enable/disable maintenance mode.`; `autoleave` → `Enable/disable auto leave.`; `logger` → `Enable/disable logger channel.`; `stats` → `Show bot stats.`; `ac` → `Show active voice chats.`

## 4.11 Inline mode / reply keyboards / other

- **Inline mode: none.** **Reply keyboards: none.**
- `sudo_join_owner` = `👑 {user} — owner of {bot} — just entered the chat.`; `sudo_join_sudo` = `🛡️ {user}, a sudo user of {bot}, just joined the chat.` (same as Tina).
- `stream_channelid_fail` = `⚠️ Failed to get your connected channel’s ID.`; `stream_download_fail` = `❌ Failed to download.\nError: <code>{error}</code>`; `stream_play_fail` = `❌ Failed to play the song.`
- `active_chats_info` = `📊 <b>Active Chats:</b>\n\nThere are {count} active chats across the bot system.` (+ `..._with_broken` variant — plain, no `<i>` unlike Tina).

---

# 5. TgMusicBot — `refs/TgMusicBot/` (Go, gotdbot, TDLib rich messages)

**Sources:** `internal/bot/*.go` (all strings inline in code — no locale file), `internal/utils/buttons.go` (all keyboards), `internal/messaging/messages.go` (silent/quiet logic).
**Style:** TDLib rich messages (`<h3>`, `<p>`, `<details open>`, `<summary>`, `<table bordered striped>`, `<tg-button>`) + plain HTML. Distinctive: **silent commands** — prefixing a playback command with `s` (`/splay`, `/sskip`, `/sstop`, `/spause`, `/sresume`, …) hides the final success message (progress and errors stay visible); plus a per-chat **Silent mode** setting that suppresses them by default (`internal/messaging/messages.go`).

## 5.1 /start

- PM (`internal/bot/start.go`, `startHandler`): rich message, verbatim template —
  ```
  <img src="{config.StartImg}"/>
  <h3>Welcome, {firstName}!</h3>
  <p><b>{botFirstName}</b> lets you stream high-quality music and video directly in Telegram voice and video chats.</p>

  <p><b>Supported platforms:</b> YouTube, Spotify, Apple Music, SoundCloud, Deezer, Twitch, and many more.</p>

  <p>Use the buttons below to add the bot to your group or explore the available commands.</p>
  ```
  Keyboard `AddMeMarkup` (`internal/utils/buttons.go`):
  - Row 1: `[Aᴅᴅ ᴍᴇ ᴛᴏ ʏᴏᴜʀ ɢʀᴏᴜᴘ]` → URL `https://t.me/<bot>?startgroup=true`
  - Row 2: `[Help]` → callback `help_all` (style Default)
  - Row 3: `[Updates]` → URL SupportChannel · `[Group]` → URL SupportGroup
  - Row 4: `[Source Code]` → URL `https://github.com/AshokShau/TgMusicBot` (style Primary)
- Group /start: rich message, verbatim template —
  ```
  <h3>{botFirstName} is ready!</h3>
  <p><b>Uptime:</b> <code>{uptime}</code></p>
  <p><i>A feature-rich music bot for your group video chats. Play your favorite tracks seamlessly.</i></p>

  <p><tg-button type="url" url="{SupportChannel}">Updates</tg-button> <tg-button type="url" url="{SupportGroup}">Group</tg-button></p>
  ```
  (In-body tg-buttons, no inline keyboard.)

## 5.2 /help — full text

- Help menu (`help_all` callback, `internal/bot/help.go`): rich message, verbatim template —
  ```
  <h3>Welcome, {firstName}!</h3>
  <p><b>{botFirstName}</b> is a fast, reliable, and feature-rich music bot for Telegram voice and video chats.</p>

  <p><b>Supported platforms:</b> YouTube, Spotify, Apple Music, SoundCloud, Deezer, Twitch, and many more.</p>

  <p>Select a category below to browse the available commands.</p>
  ```
  Keyboard `HelpMenuKeyboard` (`internal/utils/buttons.go`):
  - Row 1: `[Users]` `help_user` · `[Admins]` `help_admin` · `[Owner]` `help_owner`
  - Row 2: `[Playlist]` `help_playlist` · `[Devs]` `help_devs` · `[Autoplay]` `help_autoplay`
  - Row 3: `[Home]` `help_back` (Primary) · `[Close]` `vcplay_close` (Danger)
  - Callback toasts: `help_all` → `Opening help menu...`; `help_back` → `Returning to main menu...`; category tap → toast = the category Title; unknown → alert `Unknown help category.`
- Category pages (`getHelpCategories`, `help.go`): each is `<h3>{Title}</h3>\n\n{Content}\n\n<i>Use the buttons below to go back.</i>` with `BackHelpMenuKeyboard`:
  - Row 1: `[Help]` `help_all` · `[Home]` `help_back`
  - Row 2: `[Close]` `vcplay_close` · `[Source Code]` → `https://github.com/AshokShau/TgMusicBot`
  - Categories (verbatim content — full HTML in `help.go`):
    - **User Commands** (`help_user`): `Commands available to all members of the chat.` — Playback table: `/play [song]` (Play music from YouTube, Spotify, SoundCloud, and other supported platforms.), `/play` without arguments (When the group is idle, an administrator can play up to 10 songs from their Telegram profile.), `/mix [song]` (Create a mix playlist of related songs from supported platforms.), `/vplay [song]` (Play a video in the group video chat.), `/splay`, `/svplay`, `/sfplay`, `/sfvplay` (Play with final success messages hidden; progress and errors stay visible.), `/sskip`, `/sstop`, `/spause`, `/sresume` (Run playback controls silently. Add an s prefix to other supported playback commands for the same behavior.), `/fplay [song]` (Play a track immediately, skipping the current queue.), `/fvplay [song]` (Play a video immediately, skipping the current queue.) — General table: `/start`, `/help`, `/ping`, `/privacy`, `/queue`.
    - **Admin Commands** (`help_admin`): Playback Control table: `/skip`, `/pause`, `/resume`, `/seek [seconds]`, `/mute`, `/unmute`; Queue & Access table: `/remove [index]`, `/loop [0-10]`, `/auth`, `/unauth`, `/authlist`, `/join [link]` (Join the assistant account to the group (creates link if omitted).), `/reload`.
    - **Developer Commands** (`help_devs`): System table: `/stats`, `/av` (List all active voice and video chats.), `/clearass` (Disconnect and clear all active assistant clients.), `/leaveall` (Disconnect assistants from every active chat.), `/logger`.
    - **Chat Owner Commands** (`help_owner`): Chat Settings table: `/settings` (Manage chat settings, including play mode, administrator mode, command deletion, bot-message auto-delete (2–7 minutes), silent mode, and language preferences.)
    - **Playlist Commands** (`help_playlist`): `/createplaylist`, `/deleteplaylist`, `/addtoplaylist`, `/removefromplaylist`, `/playlistinfo`, `/myplaylists`.
    - **Autoplay Commands** (`help_autoplay`): `/autoplay` (Enable or disable autoplay. When enabled, recommended tracks are automatically queued when playback ends.)
  - All category tables use `<details open>` + `<summary>` + `<table bordered striped>` with Command/Description columns.

## 5.3 Now-playing card

- Started streaming (`sendStartedStreaming`, `internal/bot/play.go:465-477`): text message (edited in place over the progress message), verbatim template —
  `<u><b>| Started streaming</b></u>\n\n<b>Title:</b> <a href='{url}'>{name}</a>\n\n<b>Duration:</b> {mm:ss} min\n<b>Requested by:</b> {user}`
  Keyboard `ControlButtons("play")` (see below). **No thumbnail/photo** — text only, `DisableWebPagePreview: true`.
- Queued single (`enqueueTrack`, `play.go:444-458`): verbatim template —
  `<u><b>Added to queue: {qLen}</b></u>\n\n<b>Title:</b> <a href='{url}'>{name}</a>\n\n<b>Duration:</b> {mm:ss} min\n<b>Requested by:</b> {user}`
  Keyboard `QueueMarkup(trackID)`: single row `[Play Now]` `play_now_<trackID>` (Danger style) · `[Close]` `vcplay_close`. Callback data is capped at 64 bytes: if `play_now_<id>` is longer, it becomes `play_now_hash_<base64url(sha256(id)[:])>` (`PlayNowCallbackData`).
- Queued multiple (`play.go:388-421`): `<blockquote>` list —
  ```
  <b>{queuePosition}.</b> {trackName}
  └ Duration: {mm:ss}
  …
  </blockquote>
  \n<b>Queue Total:</b> {qLenAfter}\n<b>Duration:</b> {total mm:ss} min\n<b>Requested by:</b> {user}
  [\n\n<b>Skipped {n} tracks</b> (exceeded duration limit).]
  ```
  (If the message exceeds 4096 chars it degrades to just the summary.) Same `QueueMarkup` keyboard.
- Callback track message (`buildTrackMessage`, `internal/bot/callbacks.go:53`): verbatim template —
  `{emoji} <b>{state}</b>\n\n<b>Track:</b> <a href='{url}'>{name}</a>\n<b>Duration:</b> {mm:ss}\n<b>Requested by:</b> {user}`
  where state/emoji pairs are: `Now Playing`/`▶`, `Paused`/`⏸`, `Muted`/`🔇`; plus a trailing `\n\nPaused by {user}` / `\n\nResumed by {user}` / `\n\nMuted by {user}` / `\n\nUnmuted by {user}` line. Stopped message: `<b>Playback stopped.</b>\nRequested by: {user}`.
- Control buttons (`ControlButtons(mode)`, `internal/utils/buttons.go`):
  - `play`: Row 1 `[‣‣I]` `play_skip` · `[▢]` `play_stop` · `[II]` `play_pause`; Row 2 `[➕]` `play_add_to_list` (Primary) · `[Close]` `vcplay_close`
  - `pause`: Row 1 `[‣‣I]` `play_skip` · `[▢]` `play_stop` · `[▷]` `play_resume`; Row 2 `[Close]` `vcplay_close`
  - `resume`: Row 1 skip/stop/`[II]` `play_pause`; Row 2 `[Close]`
  - `mute`: Row 1 skip/stop/`[🔊]` `play_unmute`; Row 2 `[Close]`
  - `unmute`: Row 1 skip/stop/`[🔇]` `play_mute`; Row 2 `[Close]`
  - default (no playback): `[Close]` only.
  - Callback toasts: `Track skipped.` / `Playback stopped.` / `Playback paused.` / `Playback resumed.` / `Playback muted.` / `Playback unmuted.`; failures `Unable to skip/stop/pause/resume/mute/unmute playback.` / `There is no active playback.` (the latter also replaces the card text). Add-to-playlist: creates playlist `My Playlist (TgMusic)` if needed; toasts `Track "{name}" added to playlist "{playlist}".` / `Unable to add track to playlist.` / `Playlist not found.` / `Unable to create playlist.` / `Unable to fetch playlists.` Play-now: `Playing now.` / `Track not found in queue.` / `Unable to play the track.` Close: toast `Closing panel.`
- `/player` equivalent: none as a command (the card is re-sent by callbacks only).

## 5.4 Queue display

- `/queue` (`internal/bot/queue.go`): plain HTML text, verbatim structure —
  ```
  <b>Queue for {chatTitle}</b>

  <b>Now Playing:</b>
  • <b>Title:</b> <code>{title≤45}</code>
  • <b>By:</b> {user}
  • <b>Duration:</b> {mm:ss} min
  • <b>Loop:</b> On|Off
  • <b>Progress:</b> {played mm:ss} min

  <b>Next Up ({n-1}):</b>
  1. <code>{title≤45}</code> | {mm:ss} min
  … (first 14 only)
  [...and {n-15} more tracks]

  <b>Total:</b> {n} tracks
  ```
  (If >4096 chars: compact fallback `<b>Queue for {title}</b>\n\n<b>Now Playing:</b>\n• <code>{title≤45}</code>\n• {progress}/{duration} min\n\n<b>Total:</b> {n} tracks`.) Empty: `The queue is empty.`; not streaming: `The bot is not streaming in the video chat.`; chat fetch error: `Error fetching chat information.`
- Queue ops: `/remove` usage = `<b>Usage:</b> <code>/remove [track number or range]</code>\n\nExamples:\n- <code>/remove 1</code> (removes track #1)\n- <code>/remove 1-5</code> (removes tracks 1 to 5)\n- <code>/remove 1,3,5</code> (removes tracks 1, 3, and 5)`; `Please provide a valid track number or range.`; `No valid tracks to remove.`; `/loop` usage = `<b>Loop Control</b>\n\n<b>Usage:</b> <code>/loop [count]</code>\n0 to disable looping\n1-10 to set the number of repeats`; `Invalid loop value. Please provide a number between 0 and 10.`; `Loop count must be between 0 and 10.`; queue full: `Queue is full (max 10 tracks). Use /end to clear.`

## 5.5 Lyrics / search results

- **Lyrics: none.**
- **Search results:** no results list. `/play` flow (`internal/bot/play.go`): no args → `<b>Usage:</b>\n/play [song or URL]\n\n<b>Supported Platforms:</b>\n- YouTube\n- Spotify\n- JioSaavn\n- Apple Music` (with Support keyboard); then `🔍 Searching and downloading...` (edited in place); playlist mode → `🔍 Searching playlist...`; profile mode → `🔍 Loading profile songs...`; invalid URL → `Invalid URL or unsupported platform.\n\n<b>Supported Platforms:</b>\n- YouTube\n- Spotify\n- JioSaavn\n- Apple Music` (with Support keyboard); errors: `❌ Error fetching track info: {err}`; `No tracks found.`; `No valid media found in the message.`; `File too large. Max size: {n} MB.`; `Track already in queue or playing.`; `Download failed: {err}`; `❌ Search failed: {err}`; `😕 No results found. Try a different query.`; `Sorry, song exceeds max duration of {n} minutes.`; `All tracks were skipped (max duration {n} min).`; `No valid tracks found.`; `Invalid reply message.`; `❌ Playlist not found.`; `❌ Playlist is empty.`; `Invalid Telegram link.`; video disabled: `🎥 Video playback is currently disabled.\n\nAs more people use the bot, video streaming can sometimes cause lag and reduce music quality in voice chats. To ensure a smooth listening experience for everyone, this feature has been turned off for now.\n\nThanks for your support and understanding ❤️`
- `/mix`: usage `<b>Usage:</b>\n/mix [song or URL]\n\nOr run /mix while a song is currently playing to create a mix based on it.`; `Fetching mix tracks...`; `Failed to get mix: Please try again later.`; `No tracks found for mix.`; `All mix tracks are already in the queue or playing.`

## 5.6 Download progress

- No percentage/progress message. The only progress affordance is the `🔍 Searching and downloading...` status message edited in place into the final card (or deleted on silent success — `FinishProgress`). No download-cancel button.

## 5.7 Settings menu + admin panels

- `/settings` (`internal/bot/settings.go`, groups only): text `<u><b>{chatTitle} settings</b></u>\n\nClick the buttons below to change this chat's current settings.` Keyboard `SettingsKeyboard` (`internal/utils/buttons.go`) — 6 rows, each `[label ➜] [value]`:
  - Row 1: `[Play Mode ➜]` `settings_main` (no-op toast `Update your chat settings`) · `[Everyone|Admins]` `settings_play`
  - Row 2: `[Command Delete ➜]` `settings_main` · `[True|False]` `settings_delete`
  - Row 3: `[Admin Mode ➜]` `settings_main` · `[Everyone|Admins]` `settings_admin`
  - Row 4: `[Autoplay ➜]` `settings_main` · `[Enabled|Disabled]` `settings_autoplay`
  - Row 5: `[Language ➜]` `settings_main` · `[English|<code>]` `settings_lang` (tapping answers alert `Language selection is not yet implemented via this menu.`)
  - Row 6: `[Close]` `vcplay_close`
  - Prepended clean rows (`cleanSettingsKeyboard`): Row `[Auto-delete: ON|OFF]` `settings_autodelete_1|0`; Row `[Delete after: {n} minutes]` `settings_main` (no-op); Row of timer buttons `[2] [3] [4] [5] [6] [7]` → `settings_timer_{n}` (current value prefixed `✓ `); Row `[Silent mode: ON|OFF]` `settings_silent_1|0`. Timers valid 2–7 minutes (`db.ValidateAutoDeleteMinutes`).
  - Callback toasts: `Settings updated` (after a successful toggle + panel re-edit); errors: `These settings are available only in groups.`; `Invalid settings button. Open /settings again.`; `This settings button is outdated. Open /settings again.`; `Unable to load settings. Please try again later.` / `Unable to load settings. Try again later.`; `Unable to save settings. Please try again.`; `I don't have permission to delete messages in this chat.`; `Bot is not streaming.` (autoplay toggle); `Unknown setting`; `invalid setting value`; `invalid settings button. Open /settings again`.
  - Access errors (from `cache.SettingsAccess`): `You must be an administrator to use this command.` / `You are not authorized to use this command.` (message form) and `You must be an administrator to use this action.` / `You are not authorized to use this action.` (callback form).
- Support keyboards: `SupportKeyboard` = Row 1 `[Updates]` `[Group]` · Row 2 `[Close]`; `SupportBtn` = Row 1 `[Updates]` `[Group]` only.
- `/logger [enable|disable|on|off]`: `Please set LOGGER_ID in .env first.`; `Usage: /logger [enable|disable|on|off]\nCurrent status: {true|false}`; `Logger Enabled`; `Logger disabled`; `Invalid argument. Use 'enable', 'disable', 'on', or 'off'.`
- `/auth` family: `No authorized users found.`; `This user is already authorized.`; `Failed to authorize the user.`; `This user is not authorized.`; `Failed to remove authorized user.`; `User {id} has been removed from the authorized list.`; `Unable to verify administrator status.`; `You must be an administrator to use this command.`
- `/reload`: `Reloading administrator cache...` → `Administrator cache reloaded successfully.` / `Failed to reload administrator cache.`; `Please wait {mm:ss} before using this command again.` (cooldown); `/join`: `Attempting to join assistant...` → `Assistant (<b>{name}</b> | ID: <code>{id}</code>) joined successfully!` / `Failed to join assistant:\n\n{err}`; `This command can only be used in group chats.`
- `/clearass`: `failed to clear assistants: {err}`; `Removed assistant from {n} chats`; `/leaveall`: `Assistant is leaving all chats...` → `Assistant's Left {n} chats` / `Failed to leave all chats: {err}` (sic: `Assistant's Left`).

## 5.8 Ping / stats / broadcast

- `/ping` (`internal/bot/start.go`): first `Pinging… please wait…`, then edits to —
  ```
  <b>📊 System Performance Metrics</b>

  <b>Bot Latency:</b> <code>{n} ms</code>
  <b>Uptime:</b> <code>{duration}</code>
  <b>Go Routines:</b> <code>{n}</code>
  ```
- `/stats` (`internal/bot/stats.go`): first `Collecting system statistics...`, then edits to (verbatim template) —
  ```
  <b>{botName} — Runtime Status</b>
  ────────────────────────────────────

  <b>System</b>
  • CPU usage: {pct} ({n} cores)
  • Ram usage: {used} | {total}
  • Storage: {used} | {total}

  <b>Application</b>
  • Uptime: {duration}
  • Goroutines: {n}
  • Go Version: {ver}
  • CPU usage: {pct}
  • Ram usage: {used} | {limit}
  • Heap: {size}
  • GC Runs: {n} (pause {duration})

  <b>Database</b>
  • Chats: {n}
  • Users: {n}

  ────────────────────────────────────
  ```
- `/broadcast` (`internal/bot/broadcast.go`): `No broadcast in progress.`; `Broadcast stopped.`; `A broadcast is already in progress.`; `No targets found.`; `Broadcast started.` (no progress card, no cancel button in the sampled code — fire-and-forget with a `sentMsg` handle).
- Voice-chat events (`internal/bot/watcher.go`): VC started → `🎙️ Video chat started!\nUse /play <song name> to play music.`; VC ended → `🎧 Video chat ended!\nAll queues cleared.`; non-supergroup → `This chat ({id}) is not a supergroup yet.\n<b>⚠️ Please convert this chat to a supergroup and add me as admin.</b>\n\nIf you don't know how to convert, use this guide:\n🔗 https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02\n\nIf you have any questions, join our support group:` (with `AddMeMarkup` keyboard) then leaves the chat after 1s.
- Logger-channel notice (`internal/bot/load.go:276`): `<b>🤖 Bot Joined a New Chat</b>\n📌 <b>Chat ID:</b> <code>{id}</code>` (sent to `config.LoggerId`).

## 5.9 Errors & guards

- `There is no active playback in the video chat.` (mute/unmute/pause/resume/seek/loop/remove variants; skip uses `The bot is not streaming in the video chat.` / `The bot isn't streaming in the video chat.`); `Failed to mute/unmute/pause/resume the playback: {err}`; `This command can only be used in a supergroup.` (resume); `/seek` usage `<b>Usage:</b> /seek duration\n<b>Example:</b> <code>/seek 15</code>`; `Invalid seek time provided. Please use a valid number of seconds.`; `Minimum seek time is 10 seconds.`; `Failed to fetch the duration of the ongoing stream.`; `You cannot seek beyond the track duration. Maximum allowed is {mm:ss}.`; `An error occurred while seeking the track: {err}`.
- `Play mode is enabled. Only administrators and authorized users can start playback.`
- `/privacy`: text not found in the sampled files (the command is listed in help but its handler text was not in `internal/bot/*.go` strings sampled — **gap to verify**: grep `privacy` in the repo shows only the help-table row).
- `/av` (active VCs): handler text not sampled — same verification gap.

## 5.10 Playlists (personal, `internal/bot/playlist.go`)

- `/createplaylist`: usage `<b>Usage:</b> /createplaylist [playlist name]`; `Unable to fetch your playlists. Please try again later.`; `You have reached the maximum limit of 10 playlists.`; `Failed to create playlist: {err}`; success text not sampled (verify).
- `/myplaylists`: `Error fetching playlists: {err}`; `You do not have any playlists.`; list format `<b>My Playlists</b>\n\n- {name} (<code>{id}</code>)\n…`.
- `/playlistinfo`: `Playlist not found.`; `You do not own this playlist.`; short format `<b>Playlist Info</b>\n\n<b>Name:</b> {name}\n<b>Owner:</b> {owner}\n<b>Songs:</b> {n}\n\n1. {name} ({url})\n…`; long format (>3000 runes) is a rich message: `<h5>🎵 Playlist Info</h5><p><b>Name:</b> …<br><b>Owner:</b> …<br><b>Songs:</b> …</p><details><summary><b>📜 Click to View Songs</b></summary><br><table bordered striped><tr><th align='center'><b>#</b></th><th align='left'><b>Song Title</b></th></tr><tr><td align='center'>{i}</td><td align='left'><a href='{url}'>{name}</a></td></tr>…</table></details>`.
- `/deleteplaylist`: `Playlist <b>{name}</b> has been deleted successfully.`; `/removefromplaylist`: `Invalid song number.`; `Song not found in playlist.`; `Error removing song: {err}`; `Song removed from playlist '{name}'.`

## 5.11 Inline mode / reply keyboards / other

- **Inline mode: none.** **Reply keyboards: none.**
- Command inventory (from help tables + handlers): `/play /p? /fplay /fvplay /vplay /splay /svplay /sfplay /sfvplay /mix /queue /start /help /ping /privacy /skip /sskip /pause /spause /resume /sresume /seek /mute /unmute /remove /loop /auth /unauth /authlist /join /reload /stats /av /clearass /leaveall /logger /settings /createplaylist /deleteplaylist /addtoplaylist /removefromplaylist /playlistinfo /myplaylists /autoplay /stop /end` (+ silent `s`-prefixed variants of the playback commands).
- Rich patterns: `<b>`, `<i>`, `<code>`, `<a href>`, `<blockquote>`, `<h3>/<h5>`, `<p>`, `<details>/<summary>`, `<table bordered striped>`, `<tg-button>`, `<img src>`. No `<tg-spoiler>`, no `<u>`.

---

# 6. Music163bot-Go — `refs/Music163bot-Go/` (Go, NetEase Cloud Music downloader bot)

**Primary source:** `internal/render/render.go` (217 lines — ALL UI copy lives here as `const` strings + `func` builders; reproduced verbatim below in §6.9). **Keyboards:** `internal/bot/keyboards.go`. **Handlers:** `internal/bot/handlers_*.go`, `commands.go`, `app.go`, `startup_login.go`.
**Style:** Bot-API HTML, **all-Chinese UI** (Simplified). No voice chat, no queue, no playback — this bot downloads NetEase songs and sends audio files. **The only ref in Group A with inline mode and with lyrics.**
**Command menu** (`internal/bot/commands.go`, `setupCommands`): default scope — `/music` `点歌（ID 或关键词）`, `/netease` `点歌（ID 或关键词）`, `/search` `搜索歌曲`, `/lyric` `获取歌词`, `/program` `电台节目点歌`, `/recognize` `语音识别搜歌`, `/status` `缓存统计`, `/about` `关于本 bot`; admin scope (per admin chat, appended to the above) — `/login` `网易云扫码登录`, `/rmcache` `清除歌曲缓存`, `/reload` `重载配置`, `/setcachechat` `设置 Guest Mode 缓存群`.

## 6.1 /start

- **No /start handler text.** The bot has no start message; entry is via the commands above or via deep link `https://t.me/<bot>?start=<musicID>` (used by the guest keyboard, §6.8).

## 6.2 /help

- **No /help command.** Inline help exists instead (§6.7): typing `help` in inline mode returns two articles (see §6.7).

## 6.3 Now-playing card — n/a (audio file + caption instead)

- Sent audio caption (`render.MusicCaption`): `{SongInfo}\n#网易云音乐 #{ext} {sizeMB:.2f}MB {kbps:.2f}kbps\nvia @{botUsername}` where `SongInfo` = `「{name}」- {artists}\n专辑: {album}`.
- Status-line lifecycle (`internal/bot/handlers_music.go`): `正在获取歌曲信息...` (`FetchInfo`) → on cache hit `MusicStatus(…, HitCache)` = `{name}\n专辑: {album}\n{ext} {sizeMB:.2f}MB\n命中缓存, 正在发送中...` → else `MusicStatus(…, Downloading)` then live progress (see §6.6) → `MusicStatus(…, Uploading)` = `…\n下载完成, 发送中...` → audio sent with caption + `musicKeyboard`. Failures via `showDownloadFailed` → `MusicStatus(…, reason + "\n" + RetryLater)` where reason ∈ `DownloadFailed` / `Md5VerFailed` / `ProcessFailed` / `SendFailed`.
- Audio keyboard (`musicKeyboard`, `internal/bot/keyboards.go`):
  - Row 1: `[{songName}- {artists}]` → URL `https://music.163.com/song?id={musicID}` (or the program link `https://music.163.com/program?id={id}` when a radio program is open)
  - Row 2: `[Send me to...]` → `switch_inline_query` = the song URL (shares the song into another chat via inline mode)

## 6.4 Queue display

- **None.** No queue, no VC, no playback state.

## 6.5 Lyrics view — the only lyric UI in Group A

- `/lyric` (`internal/bot/handlers_lyric.go`): no args and no reply → `请输入歌曲关键词/歌曲分享链接/歌曲ID` (`InputContent`); else status `正在获取歌词中` (`FetchingLyric`, edited in place); failures: `获取歌词失败, 歌曲可能不存在或为纯音乐` (`GetLrcFailed`), `电台节目不支持获取歌词` (`LrcNoProgram`), `未找到结果` (`NoResults`).
- Success: the bot sends the **lyric file** (`.lrc` cached under the cache dir) as a document, with caption `render.LyricCaption(lyric)` = `<blockquote expandable>{escaped lyric, truncated to 1024 UTF-16 units + "…" if longer}</blockquote>`. The lyric text itself is processed by `textx.LrcText` (strips timestamps). No buttons on the lyric message.

## 6.6 Search results

- `/search <keyword>` (`internal/bot/handlers_search.go`): no args → `请输入搜索关键词` (`InputKeyword`); then status `搜索中...` (`Searching`, edited in place).
- Results page (verbatim builders, `render.go`):
  - Page header: `SearchPage(page, total)` = `<code>第[{page}/{total}]页</code>\n`
  - Each item: `SearchItem(n, name, artists)` = `<code>{n}.  </code> <code>{name} - {artists}</code>\n` (note: `%-3s` formatting — number left-padded to 3 chars inside the first code span)
  - No results → edit to `未找到结果` (`NoResults`).
- Search keyboard (`searchKeyboard`, `internal/bot/keyboards.go`):
  - Row 1: one numbered button per result on the page: `[{start+i}]` → callback `music {musicID}` (space-separated)
  - Row 2 (pagination, only the applicable buttons): `[⬅️ 上一页]` → `search {page-1} {hash}` (if page > 1) · `[下一页 ➡️]` → `search {page+1} {hash}` (if hasMore). `hash` is an FNV hash of the keyword; stale/foreign pages → `搜索已失效，请重新搜索` (`SearchExpired`); tapping another user's search → `只有发起搜索的人可以操作` (`SearchNotOwner`); rapid re-taps → `操作太频繁，请 {n} 秒后再试` (`Cooldown`); no more results → `没有更多结果了` (`NoMoreResults`).
  - Tapping a number answers callback `Success` (`CallbackText`) and starts the download flow for that song.
- `/music` / `/netease`: no args → `请输入歌曲ID或歌曲关键词` (`InputIDorKeyword`); no results → `未找到结果`; cookie expired → `Cookie 过期，请联系 bot 管理员` (`CookieExpired`).
- `/program` (radio): same flow with program links; lyric unsupported (see §6.5).

## 6.7 Download progress

- Progress status (verbatim, `render.DownloadStatus` + `MusicStatus`): the status message is edited live to —
  ```
  {songName}
  专辑: {album}
  {ext} {sizeMB:.2f}MB
  下载中... {speed}
  {writtenMB:.2f}MB/{totalMB:.2f}MB {percent}%
  ```
  (on retry: `下载失败，尝试重新下载中...` = `Redownloading`). Other status lines: `等待下载中...` (`WaitForDown`), `下载中...` (`Downloading`), `下载完成, 发送中...` (`Uploading`), `正在获取歌曲信息...` (`FetchInfo`), `获取歌曲信息失败` (`FetchInfoFailed`), `获取歌曲下载链接失败` (`GetURLFailed`), `MD5校验失败` (`Md5VerFailed`), `下载失败` (`DownloadFailed`), `发送失败` (`SendFailed`), `文件处理失败` (`ProcessFailed`), `请稍后重试` (`RetryLater`). **No cancel button, no percentage bar** — plain status-line edits.
- Cache admin: `/rmcache` → `歌曲未缓存` (`NoCache`) if nothing cached, else `清除 [{name}] 缓存成功` (`RmcacheReport`); `/reload` → `重新加载中` (`Reloading`).

## 6.8 Inline mode — the only inline UI in Group A

- Router (`internal/bot/handlers_inline.go`, `handleInline`): query `help` → help articles; query containing `search` → search articles; query parseable as a music ID / short link → music article; anything else → empty-hint article. All results `cache_time 3600` except the uncached-music article (`60`).
- Help (`answerInlineHelp`): two `InlineQueryResultArticle`s, both with `InputMessageContent` text = `Music163bot-Go v3` (`BotName`):
  - Article 1: title `1.粘贴音乐分享URL或输入MusicID` (`InlineHelpStep1`)
  - Article 2: title `2.输入 search+关键词 搜索歌曲` (`InlineHelpStep2`)
- Inline search (`answerInlineSearch`): query `search` with no keyword → one article, title `请输入关键词` (`InlineInputKeyword`); no results → one article, title `未找到结果` (`NoResults`), message text `未找到结果`; else up to 10 articles: title = song name, description = artist names joined, message text = `/netease {musicID}`.
- Inline music (`answerInlineMusic`): if cached → `cachedMusicResult` (audio result, details in code — title/description from the cached song info); if NOT cached → one article: title `歌曲未缓存` (`NoCache`), description `点击上方按钮缓存歌曲` (`TapToDownload`), message text = the raw query; plus a results button: text `点我缓存歌曲` (`TapMeToDown`), `start_parameter` = `{musicID}` (deep-links the user to the bot to trigger the download).
- Empty query (`answerInlineEmpty`): one article, title `输入 help 获取帮助` (`InlineHelpHint`), message text `Music163bot-Go v3`.
- Guest mode (group mentions, `internal/bot/handlers_guest.go`): reply to a NetEase share link or a voice message and @ the bot → inline-style article results: `guestArticle("网易云音乐", FetchInfoFailed, nil)` on fetch failure; `guestArticle("使用帮助", GuestUsage, nil)` for help, where `GuestUsage` = `回复一条网易云音乐分享链接或包含音乐片段的语音，并在回复中 @我，即可发送或识别歌曲。`; uncached song → text `GuestMusicResult` = `「{name}」- {artists}\n专辑: {album}\n歌曲未缓存，请点击下方按钮在 Bot 中获取` (`GuestNoCache`) with `guestKeyboard`: Row 1 `[在网易云音乐中打开]` → song link; Row 2 `[在 Bot 中获取歌曲]` → URL `https://t.me/{bot}?start={musicID}`.
- Voice recognition (`/recognize`, `internal/bot/handlers_recognize.go` + guest path): no voice / wrong usage → `请先发送一条包含音乐片段的语音，再回复该语音发送 /recognize。` (`ReplyVoiceFirst`); status `正在识别音乐，请稍候...` (`Recognizing`); failures: `暂时无法读取这条语音\n请稍后重试，或重新发送语音。` (`RecognizeReadFailed`), `暂未识别到歌曲\n请尝试录制更清晰、稍长一些的音乐片段，再试一次。` (`RecognizeNoResult`), `音乐识别暂时失败，请稍后重试。` (`RecognizeFailed`); success → `🎵 识别成功\n「{name}」- {artists}\n专辑: {album}` (`RecognizeResult` / `RecognizeSuccess` = `已识别到歌曲`) with `linkKeyboard` = single button `[在网易云音乐中打开]` → song link.

## 6.9 Settings / admin panels

- No settings panel, no user-facing settings. Admin commands:
  - `/status` → `render.StatusInfo`: `<b>[统计信息]</b>\n数据库中总缓存歌曲数量: {total}\n当前对话 [{chat}] 缓存歌曲数量: {chatCount}\n当前用户 [<a href="tg://user?id={userID}">{userID}</a>] 缓存歌曲数量: {userCount}` (chat shown as @username link, group title link, or plain title).
  - `/about` → `render.About`: `<b>Music163bot-Go v3</b>\nGithub: https://github.com/XiaoMengXinX/Music163bot-Go\n\n[编译环境] {runtime}\n[编译版本] {version}\n[编译哈希] {commit}\n[编译日期] {buildTime}\n[运行环境] {arch}`.
  - `/setcachechat` → bad input `缓存群 ID 须为数字，也可以在目标群内直接发送 /setcachechat` (`InputChatID`); test message to the target chat `已设为缓存群，Guest Mode 点歌时会先把未缓存的歌曲上传到这里` (`CacheChatTest`); unreachable `无法向该聊天发送消息，请确认 bot 已加入并可以发言` (`CacheChatUnreachable`); disable → `已关闭缓存群` (`CacheChatDisabled`); set → `缓存群已设置为 <code>{chatID}</code>` (`CacheChatSet`); config-write failure appends `\n配置文件写入失败: {err}\n本次设置已生效，重启后将失效`.
- Login (QR-code, `internal/bot/handlers_login.go` + `startup_login.go`): busy → `已有扫码登录会话进行中` (`LoginBusy`); key failure → `获取二维码失败，请稍后重试` (`LoginKeyFailed`); QR caption → `请使用网易云音乐 App 扫码\n剩余有效期 {n} 秒` (`LoginQRCaption`); scanned → `已扫码，等待确认: {nickname}` (`LoginScanned`); success → `扫码登录成功: {nickname}` (`LoginOK`); expired → `二维码已过期，请重新发送 /login` (`LoginExpired`); check failure → `查询扫码状态失败，请稍后重试` (`LoginCheckFailed`); cancel button `[取消登录]` (`CancelLogin`) → callback `cancellogin` → caption `已取消登录` (`LoginCancelled`); start-login button `[扫码登录]` (`LoginStartButton`) → callback `startlogin`; no session → `当前没有进行中的登录会话，可发送 /login` (`LoginNoSession`); session over → `登录会话已结束，可发送 /login` (`LoginSessionOver`); QR sent → `已发送登录二维码` (`LoginQRSent`); startup prompt → `Cookie 未配置，需登录\n点击下方按钮扫码登录，或扫描终端内二维码\n请在 {n} 秒内操作` (or `Cookie 已失效，需重新登录…`) (`LoginStartupPrompt`); terminal variant `…\n请使用网易云音乐 App 扫描下方二维码\n请在 {n} 秒内操作` (`LoginStartupTerminal`); timeout → `登录超时，bot 已停止` (`LoginTimeoutStopped`); cancelled-at-startup → `已取消登录，bot 已停止` (`LoginCancelledStopped`); cookie live → `Cookie 已生效` (`LoginStartupDone`); cookie expired (admin warn) → `Cookie 已失效，新歌下载已暂停，请发送 /login 重新登录` (`CookieExpiredAdminWarn`); resumed → `Cookie 已生效，新歌下载已恢复` (`CookieResumedAdmin`).

## 6.10 Errors & misc

- All render constants are listed verbatim in §6.11 below (the full `render.go` const block is the authoritative list — port workers should copy from there, not from paraphrase).
- `linkKeyboard(link)`: single button `[在网易云音乐中打开]` → URL `link`.
- No /start, no /help, no queue, no settings panel, no VC, no reply keyboards, no maintenance/broadcast/sudo UI, no ping/stats beyond `/status`.

## 6.11 Full verbatim copy of `internal/render/render.go` constants & builders

(See Appendix F for the complete file. Key constants, verbatim:)
`InputKeyword` = `请输入搜索关键词`; `InputIDorKeyword` = `请输入歌曲ID或歌曲关键词`; `InputIDorLink` = `请输入歌曲ID或歌曲分享链接`; `InputContent` = `请输入歌曲关键词/歌曲分享链接/歌曲ID`; `Searching` = `搜索中...`; `NoResults` = `未找到结果`; `NoCache` = `歌曲未缓存`; `TapToDownload` = `点击上方按钮缓存歌曲`; `TapMeToDown` = `点我缓存歌曲`; `HitCache` = `命中缓存, 正在发送中...`; `SendMeTo` = `Send me to...`; `GetLrcFailed` = `获取歌词失败, 歌曲可能不存在或为纯音乐`; `LrcNoProgram` = `电台节目不支持获取歌词`; `GetURLFailed` = `获取歌曲下载链接失败`; `FetchInfo` = `正在获取歌曲信息...`; `FetchInfoFailed` = `获取歌曲信息失败`; `WaitForDown` = `等待下载中...`; `Downloading` = `下载中...`; `Redownloading` = `下载失败，尝试重新下载中...`; `Uploading` = `下载完成, 发送中...`; `Md5VerFailed` = `MD5校验失败`; `RetryLater` = `请稍后重试`; `Reloading` = `重新加载中`; `CallbackText` = `Success`; `FetchingLyric` = `正在获取歌词中`; `DownloadFailed` = `下载失败`; `SendFailed` = `发送失败`; `ProcessFailed` = `文件处理失败`; `SearchExpired` = `搜索已失效，请重新搜索`; `NoMoreResults` = `没有更多结果了`; `SearchNotOwner` = `只有发起搜索的人可以操作`; `ReplyVoiceFirst` = `请先发送一条包含音乐片段的语音，再回复该语音发送 /recognize。`; `GuestUsage` = `回复一条网易云音乐分享链接或包含音乐片段的语音，并在回复中 @我，即可发送或识别歌曲。`; `GuestNoCache` = `歌曲未缓存，请点击下方按钮在 Bot 中获取`; `Recognizing` = `正在识别音乐，请稍候...`; `RecognizeNoResult` = `暂未识别到歌曲\n请尝试录制更清晰、稍长一些的音乐片段，再试一次。`; `RecognizeFailed` = `音乐识别暂时失败，请稍后重试。`; `RecognizeReadFailed` = `暂时无法读取这条语音\n请稍后重试，或重新发送语音。`; `RecognizeSuccess` = `已识别到歌曲`; `InlineHelpHint` = `输入 help 获取帮助`; `InlineHelpStep1` = `1.粘贴音乐分享URL或输入MusicID`; `InlineHelpStep2` = `2.输入 search+关键词 搜索歌曲`; `InlineInputKeyword` = `请输入关键词`; `BotName` = `Music163bot-Go v3`; `LoginBusy` = `已有扫码登录会话进行中`; `LoginKeyFailed` = `获取二维码失败，请稍后重试`; `LoginExpired` = `二维码已过期，请重新发送 /login`; `LoginCheckFailed` = `查询扫码状态失败，请稍后重试`; `LoginCancelled` = `已取消登录`; `CancelLogin` = `取消登录`; `LoginStartButton` = `扫码登录`; `LoginTimeoutStopped` = `登录超时，bot 已停止`; `LoginCancelledStopped` = `已取消登录，bot 已停止`; `LoginStartupDone` = `Cookie 已生效`; `LoginSessionOver` = `登录会话已结束，可发送 /login`; `LoginNoSession` = `当前没有进行中的登录会话，可发送 /login`; `LoginQRSent` = `已发送登录二维码`; `InputChatID` = `缓存群 ID 须为数字，也可以在目标群内直接发送 /setcachechat`; `CacheChatTest` = `已设为缓存群，Guest Mode 点歌时会先把未缓存的歌曲上传到这里`; `CacheChatUnreachable` = `无法向该聊天发送消息，请确认 bot 已加入并可以发言`; `CacheChatDisabled` = `已关闭缓存群`; `CookieExpired` = `Cookie 过期，请联系 bot 管理员`; `CookieExpiredAdminWarn` = `Cookie 已失效，新歌下载已暂停，请发送 /login 重新登录`; `CookieResumedAdmin` = `Cookie 已生效，新歌下载已恢复`.
Builders (verbatim templates in `render.go`): `LoginStartupPrompt` / `LoginStartupTerminal` / `LoginQRCaption` / `LoginScanned` / `LoginOK` / `MusicStatus` (`{name}\n专辑: {album}\n{ext} {size:.2f}MB\n{suffix}`) / `DownloadStatus` (` {speed}\n{written:.2f}MB/{total:.2f}MB {percent}%`) / `SongInfo` (`「{name}」- {artists}\n专辑: {album}`) / `RecognizeResult` (`🎵 识别成功\n` + SongInfo) / `GuestMusicResult` (SongInfo + `\n` + GuestNoCache) / `LyricCaption` (`<blockquote expandable>{lyric≤1024 UTF-16…}</blockquote>`) / `MusicCaption` (`{SongInfo}\n#网易云音乐 #{ext} {size:.2f}MB {kbps:.2f}kbps\nvia @{bot}`) / `About` / `StatusInfo` (`<b>[统计信息]</b>\n数据库中总缓存歌曲数量: {total}\n当前对话 [{chat}] 缓存歌曲数量: {chatCount}\n当前用户 [<a href="tg://user?id={uid}">{uid}</a>] 缓存歌曲数量: {userCount}`) / `CacheChatSet` (`缓存群已设置为 <code>{id}</code>`) / `RmcacheReport` (`清除 [{name}] 缓存成功`) / `Cooldown` (`操作太频繁，请 {n} 秒后再试`) / `SearchPage` (`<code>第[{page}/{total}]页</code>\n`) / `SearchItem` (`<code>{n}.  </code> <code>{name} - {artists}</code>\n`) / `SongLink` (`https://music.163.com/song/{id}`) / `ProgramLink` (`https://music.163.com/program?id={id}`).

---

# 7. anonx-cpp — `refs/anonx-cpp/` (C++20, TDLib, minimal)

**Only source:** `src/telegram/dispatcher.cpp` (340 lines) + `include/anonx/telegram/dispatcher.hpp`. **All UI is plain-text Markdown** sent via `TDLibClient::send_message` (reply-to the command message). **No keyboards of any kind, no callbacks, no inline mode, no thumbnails, no settings, no lyrics, no search results, no download progress, no auth/sudo, no channel play, no playlists, no maintenance/broadcast.**
**Commands** (`execute_command`, `dispatcher.cpp:137-158`): `/play`|`/p`, `/skip`|`/next`, `/pause`, `/resume`, `/stop`|`/end`, `/queue`|`/q`, `/volume`|`/vol`, `/ping`, `/help`|`/start`. Unknown commands are silently ignored (no reply). Non-`/` messages are ignored. Rate limiting is silent (token bucket; over-limit commands are dropped with only a log line).

## 7.1 /start and /help — one and the same

- `/start` and `/help` both call `handle_help` (`dispatcher.cpp:305-319`). Verbatim text:
  ```
  🎶 **AnonX-CPP Music Bot Commands**

  • `/play <name or URL>` - Stream YouTube audio or direct audio link
  • `/pause` - Pause currently playing stream
  • `/resume` - Resume paused stream
  • `/skip` - Skip to next song in queue
  • `/stop` - Stop streaming, clear queue, and leave voice chat
  • `/queue` - Display active song queue
  • `/volume <1-200>` - Change audio playback volume
  • `/ping` - Check bot status and uptime
  • `/help` - Show this help menu
  ```
  No buttons. No separate start/welcome text.

## 7.2 Now-playing card — plain text, no card

- Start playing (`handle_play`, `:184-189`): `▶️ **Now Playing:** {query-as-title}\n👤 **Requested by:** \`{user_id}\`` (note: the "title" is the raw query string — no search/resolution in this codebase; the track title is whatever the user typed).
- Queued (`:194-197`): `⏳ **Queued at position #{queue_size}:** {query-as-title}`
- Next track (`play_next_in_queue`, `:326`): `▶️ **Now Playing Next Track:** {title}`
- Queue finished (`:336`): `⏹️ Queue completed. Voice chat playback ended.`
- Errors: empty query → `❗ Please provide a song name, YouTube URL, or stream link.\nExample: \`/play Believer\``; start failure → `❌ Error starting audio stream pipeline.`

## 7.3 Queue display

- `/queue` (`handle_queue`, `:234-260`): verbatim structure —
  ```
  🎵 **Current Track & Queue:**

  ▶️ **Now Playing:** {title}

  📜 **Upcoming Tracks:**
  1. {title}
  2. {title}
  … (first 10 only)
  ... and {n} more tracks.
  ```
  Empty: `ℹ️ The queue is currently empty.` (both the now-playing line and the upcoming section are omitted when absent.)

## 7.4 Playback controls (all plain replies, no buttons)

- Skip: no playback → `ℹ️ Nothing is currently playing to skip.`; else `⏭️ Skipped current track.` (then auto-plays next).
- Pause: `⏸️ Playback paused.` / `ℹ️ No active playback to pause.`
- Resume: `▶️ Playback resumed.` / `ℹ️ Playback is not paused.`
- Stop: `⏹️ Playback stopped and queue cleared. Left voice chat.`
- Volume: no arg → `❗ Please provide a volume percentage (1 - 200).\nExample: \`/volume 120\``; out of range → `⚠️ Volume must be between 1% and 200%.`; ok → `🔊 Volume set to {n}%.`; non-numeric → `⚠️ Invalid volume value.`

## 7.5 Ping

- `/ping` (`handle_ping`, `:282-303`): verbatim template —
  ```
  🏓 **Pong!**
  ⏱️ **Uptime:** {h}h {m}m {s}s
  🚀 **Engine:** AnonX C++20 High-Performance Core
  ⚡ **ThreadPool:** {workers} workers | {completed} completed | {pending} queued
  🛡️ **RateLimiting:** Active (Token-Bucket Striped)
  ```

## 7.6 Everything else — explicitly absent

- Lyrics: none. Search results: none (no search; the query string IS the title). Download progress: none. Settings/admin panels: none. Inline mode: none. Reply keyboards: none. Errors beyond §7.2/7.4: none (no error strings for voice-chat absence, bans, permissions, maintenance). Stats: none beyond /ping. Playlists: none. Channel play: none. Auth/sudo: none. Broadcast: none.

---

# 8. Cross-ref notes for port workers

1. **Four dialects of the same bot.** NoxBeats (C++), TinaMusic (C++), YukkiMusic-Go and ArcMusic (Go) are the same lineage: same command set, same `help_*` categories, same `room:<id>:` callbacks, same settings keys. Differences that matter: (a) NoxBeats uses space-separated callbacks (`controls <action> <chat>`, `help <cat>`, `lang_change <code>`) and small-caps strings; (b) Tina adds ±15s seek buttons + a hardcoded "join room" mini-app URL button + an Autoplay row in settings + `<blockquote>`/`<tg-spoiler>` styling; (c) Yukki is the only one with TDLib rich elements (`<h3>`, `<table>`, `<tg-button>`), custom-emoji button icons, colour styles, per-track queue delete buttons, a `qmore:` show-more button, a rich-table auth list with `authdel:` revoke buttons, and the shared dashboard toggle-card pattern; (d) Arc is Yukki minus the rich layer (plain HTML), minus the settings panel, minus dashboards, plus a `/logs` viewer and the most detailed RTMP guide. **Port decision: Yukki's inventory is the superset for structure; Tina's strings are the most distinctive for tone; Arc's plain-HTML fallbacks are the safe baseline.**
2. **Now-playing thumbnails:** NoxBeats sends photo-when-thumbnail (track thumb or `DEFAULT_THUMB` for YouTube, else text); Yukki sends photo with the card as caption (edit via `EditMessageMedia`, fallback to text); Tina inlines `{thumb}` as text in the message; TgMusicBot never sends a thumbnail; Arc is text-first; Music163bot sends the audio file itself with a caption; anonx-cpp has no thumbnails.
3. **Progress bars:** Tina builds a 12-char `🔘`/`─` bar in code (`MM:SS <bar> MM:SS`); Yukki/Arc use `utils.GetProgressBar` as a button label; NoxBeats has no progress bar on the player card; TgMusicBot has no progress UI at all.
4. **Settings panels:** NoxBeats 3 rows (play-mode, cmd-delete, language — values `True/False`); Tina 9 rows (adds clean-mode, clean-delay, thumbnails, autoplay); Yukki 8 rows (adds clean-mode, clean-delay, thumbnails; autoplay lives in a dashboard card instead); TgMusicBot 6 rows + clean rows (adds autoplay + silent mode + auto-delete timers 2–7 min); Arc **none** (commands only); Music163bot **none**; anonx-cpp **none**.
5. **Callback-data namespaces (all distinct — do not mix):** NoxBeats: `controls …`, `help …`, `help`, `language`, `lang_change …`, `settings`, `settings play|delete`, `cancel_dl`. Tina/Yukki/Arc: `room:<id>:…`, `croom:…`, `help:…`, `help_cb`, `start`, `lang:…`, `lang:select`, `set:…`, `info:…`, `dashboard:…`, `close`, `progress`, `cancel`, `bcast_cancel`, `restart:…`, `autoplay:on|off:<id>` (Tina only), `qdel:/qmore:/authdel:` (Yukki only). TgMusicBot: `play_*`, `vcplay_close`, `help_all|help_user|help_admin|help_owner|help_devs|help_playlist|help_autoplay|help_back`, `settings_main|settings_play|settings_delete|settings_admin|settings_autoplay|settings_lang|settings_autodelete_0|1|settings_silent_0|1|settings_timer_2..7`, `play_now_<id>|play_now_hash_<b64>`. Music163bot: `music {id}`, `search {page} {hash}`, `cancellogin`, `startlogin`.
6. **Lyrics:** only Music163bot-Go has any lyric UI (file + expandable blockquote caption). All six music bots: none.
7. **Inline mode:** only Music163bot-Go (help/search/music/empty articles + results button + switch_inline_query share). All six music bots: none.
8. **Reply keyboards:** none in any of the 7 refs.
9. **Known source quirks to preserve or fix deliberately (port decision, not silent correction):** Tina `help_public` etc. use `&amp;` inside HTML; Arc `help_public` has unclosed `<b>` on the /privacy line and key `bug_usasge` (misspelled) and `channel_play_depreciated` (misspelled); Arc `/ping` says `system stat's`; Arc replay callback is bare `room:replay` while siblings carry the chat id; Tina's "join room" button hardcodes `https://t.me/Dollbymusicbot/room?startapp=`; Tina/Arc/Yukki `start_group`/`help_main` reference `https://yukiapi.site/file/…` image links; NoxBeats `help` callback is the bare word `help` while Tina/Yukki/Arc use `help:main`/`help_cb`; TgMusicBot's close callback is `vcplay_close` (not `close`); Music163bot search items use `%-3s` number padding.
10. **Gaps found during harvest (verify before porting):** TgMusicBot `/privacy` and `/av` handler texts were not located in the sampled files (only help-table rows mention them); NoxBeats `/queue` display handler lives in `Bad/cmd/playback.cpp` (queue text) while queue *ops* live in `queue_ops.cpp`; Arc has no `voicechat_started/ended` locale strings despite a `voicechat.go` module; Tina's `DOWNLOAD_CANCEL_BTN` locale key has no keyboard builder; Yukki `/stats` is now verified — code-built rich message in `internal/modules/stats.go:93-221`, sudo-only, no locale keys (see §3.8); NoxBeats broadcast has no cancel keyboard (fire-and-forget).

---

# Appendices — verbatim locale files and extracted help texts

Appendices A–D are byte-for-byte copies of the four locale files (appended by script, not retyped). Appendix E is the machine-extracted `helpTexts[...]` per-command help blocks from YukkiMusic-Go and ArcMusic (raw backtick literals, unedited). Appendix F is a byte-for-byte copy of `internal/render/render.go` (Music163bot-Go).


## Appendix A — VERBATIM: NoxBeats `strings/en.json`

```json
{
    "add_me": "ᴀᴅᴅ ᴍᴇ ᴛᴏ ʏᴏᴜʀ ɢʀᴏᴜᴘ",
    "admin_required": "» ʙᴏᴛ ʀᴇǫᴜɪʀᴇs ᴛʜᴇ <b>ɪɴᴠɪᴛᴇ ᴜsᴇʀs ᴠɪᴀ ʟɪɴᴋ</b> ᴘᴇʀᴍɪssɪᴏɴ ᴛᴏ ᴡᴏʀᴋ.",
    "admin_cache_wait": "» ʏᴏᴜ ᴄᴀɴ ᴏɴʟʏ ʀᴇғʀᴇsʜ ᴛʜᴇ ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ ᴏɴᴄᴇ ᴇᴠᴇʀʏ 10 ᴍɪɴᴜᴛᴇs.",
    "admin_cache_reloading": "» ʀᴇʟᴏᴀᴅɪɴɢ ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ...",
    "admin_cache_reloaded": "» ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ ʀᴇғʀᴇsʜᴇᴅ sᴜᴄᴄᴇssғᴜʟʟʏ!",
    "auth_added": "» ᴀᴅᴅᴇᴅ {0} ᴛᴏ ᴛʜᴇ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ.",
    "auth_removed": "» ʀᴇᴍᴏᴠᴇᴅ {0} ғʀᴏᴍ ᴛʜᴇ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ.",
    "auth_empty": "» ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ ɪs ᴇᴍᴘᴛʏ.",
    "auth_list": "<u><b>ʟɪsᴛ ᴏғ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ɪɴ {0}:</b></u>\n",
    "auth_is_admin": "» ᴛʜᴇ ᴜsᴇʀ ɪs ᴀʟʀᴇᴀᴅʏ ᴀɴ <b>ᴀᴅᴍɪɴ</b> ᴀɴᴅ ᴄᴀɴ'ᴛ ʙᴇ ᴀᴅᴅᴇᴅ ᴛᴏ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs.",
    "auto_left": "» ɪ ᴡᴀɪᴛᴇᴅ, ɪ ᴡᴀᴛᴄʜᴇᴅ, ɪ ʏᴀᴡɴᴇᴅ... ᴀɴᴅ ᴛʜᴇɴ ɪ ʟᴇғᴛ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.",
    "bl_usage": "<b>ᴜsᴀɢᴇ:</b>\n\n/{0} [chat_id|user_id]",
    "bl_invalid": "» ᴏɴʟʏ ᴄʜᴀᴛ ɪᴅs ᴀɴᴅ ᴜsᴇʀ ɪᴅs ᴀʀᴇ sᴜᴘᴘᴏʀᴛᴇᴅ.",
    "bl_already": "» ᴛʜɪs ᴄʜᴀᴛ ɪs ᴀʟʀᴇᴀᴅʏ ʙʟᴀᴄᴋʟɪsᴛᴇᴅ.",
    "bl_not": "» ᴛʜɪs ᴄʜᴀᴛ ɪs ɴᴏᴛ ʙʟᴀᴄᴋʟɪsᴛᴇᴅ.",
    "bl_added": "» ᴛʜɪs ᴄʜᴀᴛ ʜᴀs ʙᴇᴇɴ ʙʟᴀᴄᴋʟɪsᴛᴇᴅ.",
    "bl_removed": "» ᴛʜɪs ᴄʜᴀᴛ ʜᴀs ʙᴇᴇɴ ʀᴇᴍᴏᴠᴇᴅ ғʀᴏᴍ ᴛʜᴇ ʙʟᴀᴄᴋʟɪsᴛ.",
    "bl_user_notify": "» ʏᴏᴜ'ᴠᴇ ʙᴇᴇɴ ʙʟᴀᴄᴋʟɪsᴛᴇᴅ ғʀᴏᴍ ᴜsɪɴɢ ᴛʜɪs ʙᴏᴛ. ᴘʟᴇᴀsᴇ ʀᴇᴀᴄʜ ᴏᴜᴛ ɪɴ ᴛʜᴇ <a href=\"{0}\">sᴜᴘᴘᴏʀᴛ ᴄʜᴀᴛ</a> ғᴏʀ ᴍᴏʀᴇ ɪɴғᴏ.",
    "dl_cancel": "ᴅᴏᴡɴʟᴏᴀᴅ ᴄᴀɴᴄᴇʟᴇᴅ ʙʏ {0}",
    "dl_complete": "ᴅᴏᴡɴʟᴏᴀᴅ ᴄᴏᴍᴘʟᴇᴛᴇᴅ. ᴘʀᴏᴄᴇssɪɴɢ ғɪʟᴇ...\n\nᴛɪᴍᴇ ᴇʟᴀᴘsᴇᴅ: {0}s",
    "dl_cancelling": "» ʜᴀɴɢ ᴏɴ, ᴄᴀɴᴄᴇʟʟɪɴɢ ᴛʜᴇ ᴅᴏᴡɴʟᴏᴀᴅ ɴᴏᴡ!",
    "dl_active": "» ᴀʟʀᴇᴀᴅʏ ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ᴛʜᴀᴛ ғɪʟᴇ. ᴍɪɴᴅ ᴡᴀɪᴛɪɴɢ?",
    "dl_limit": "» ғᴀɪʟᴇᴅ ᴛᴏ ᴘʀᴏᴄᴇss ᴛʜᴇ ғɪʟᴇ. ᴛʜᴇ ᴍᴀxɪᴍᴜᴍ ᴅᴏᴡɴʟᴏᴀᴅ ʟɪᴍɪᴛ ɪs <b>200 ᴍʙ</b>.",
    "dl_progress": "<u><b>ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ғɪʟᴇ</b></u>\n\n<b>ᴘʀᴏɢʀᴇss:</b> {0} / {1} [{2:.1f}%]\n<b>sᴘᴇᴇᴅ:</b> {3}/s | <b>ᴇᴛᴀ:</b> {4}",
    "dl_not_found": "» ᴅᴏᴡɴʟᴏᴀᴅ ᴀʟʀᴇᴀᴅʏ ᴄᴀɴᴄᴇʟʟᴇᴅ.",
    "error_no_audio": "» ᴍᴏᴠɪɴɢ ᴛᴏ ᴛʜᴇ ɴᴇxᴛ ᴛʀᴀᴄᴋ...\n\nᴀᴜᴅɪᴏ sᴏᴜʀᴄᴇ ɴᴏᴛ ғᴏᴜɴᴅ ɪɴ ᴛʜᴇ ғɪʟᴇ.",
    "error_no_call": "<b>ɴᴏ ᴀᴄᴛɪᴠᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ғᴏᴜɴᴅ.</b>\n\nᴘʟᴇᴀsᴇ sᴛᴀʀᴛ ᴏɴᴇ ᴀɴᴅ <b>ᴛʀʏ ᴀɢᴀɪɴ</b>.",
    "error_no_file": "ᴅᴏᴡɴʟᴏᴀᴅ ғᴀɪʟᴇᴅ.\n\nɪғ ᴛʜᴇ ɪssᴜᴇ ᴘᴇʀsɪsᴛs, ʀᴇᴘᴏʀᴛ ɪᴛ ᴛᴏ ᴛʜᴇ <a href=\"{0}\">sᴜᴘᴘᴏʀᴛ ᴄʜᴀᴛ</a>",
    "error_tg_server": "<u><b>ᴛᴇʟᴇɢʀᴀᴍ sᴇʀᴠᴇʀ ᴇʀʀᴏʀ</b></u>\n\nᴛᴇʟᴇɢʀᴀᴍ ɪs ᴇxᴘᴇʀɪᴇɴᴄɪɴɢ ɪɴᴛᴇʀɴᴀʟ ɪssᴜᴇs. ᴄʟᴇᴀʀɪɴɢ ᴛʜᴇ ǫᴜᴇᴜᴇ ᴀɴᴅ ʟᴇᴀᴠɪɴɢ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.",
    "error_rtmp": "» ʀᴛᴍᴘ sᴛʀᴇᴀᴍɪɴɢ ɪs ɴᴏᴛ sᴜᴘᴘᴏʀᴛᴇᴅ. ᴘʟᴇᴀsᴇ sᴛᴏᴘ ᴛʜᴇ ʀᴛᴍᴘ sᴛʀᴇᴀᴍ ᴀɴᴅ sᴛᴀʀᴛ ᴀ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪɴsᴛᴇᴀᴅ.",
    "eval_error": "⚠️ ᴇʀʀᴏʀ ᴇxᴇᴄᴜᴛɪɴɢ sɴɪᴘᴘᴇᴛ\n\n",
    "eval_inp": "ᴡʜᴀᴛ?",
    "eval_out": "<b>ᴏᴜᴛᴘᴜᴛ:</b>\n<code>{0}</code>",
    "gcast_usage": "» ʀᴇᴘʟʏ ᴛᴏ ᴀ ᴍᴇssᴀɢᴇ ғᴏʀ ʙʀᴏᴀᴅᴄᴀsᴛɪɴɢ ᴛʜᴇ ᴍᴇssᴀɢᴇ.",
    "gcast_active": "» ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ ғᴏʀ ᴛʜᴇ ᴏɴɢᴏɪɴɢ ʙʀᴏᴀᴅᴄᴀsᴛ ᴛᴏ ғɪɴɪsʜ.",
    "gcast_start": "» ʙʀᴏᴀᴅᴄᴀsᴛɪɴɢ sᴛᴀʀᴛᴇᴅ.",
    "gcast_end": "» ʙʀᴏᴀᴅᴄᴀsᴛᴇᴅ ᴛʜᴇ ᴍᴇssᴀɢᴇ ᴛᴏ {0} ɢʀᴏᴜᴘs ᴀɴᴅ {1} ᴜsᴇʀs.",
    "help_menu": "<b>ᴄʟɪᴄᴋ ᴛʜᴇ ʙᴜᴛᴛᴏɴs ʙᴇʟᴏᴡ ᴛᴏ ɢᴇᴛ ɪɴғᴏʀᴍᴀᴛɪᴏɴ ᴀʙᴏᴜᴛ ᴍʏ ᴄᴏᴍᴍᴀɴᴅs.</b>\n\n<i><b>ɴᴏᴛᴇ:</b> ᴀʟʟ ᴄᴏᴍᴍᴀɴᴅs ᴄᴀɴ ʙᴇ ᴜsᴇᴅ ᴡɪᴛʜ /</i>",
    "help_admins": "<u><b>ᴀᴅᴍɪɴ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n\n/pause: ᴘᴜᴀsᴇ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ.\n/resume: ʀᴇsᴜᴍᴇ ᴛʜᴇ ᴘᴀᴜsᴇᴅ sᴛʀᴇᴀᴍ.\n/skip: sᴋɪᴘ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ sᴛʀᴇᴀᴍ.\n/stop: sᴛᴏᴘ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ.\n\n/seek [ᴅᴜʀᴀᴛɪᴏɴ ɪɴ sᴇᴄᴏɴᴅs]: sᴇᴇᴋ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ.\n/seekback [ᴅᴜʀᴀᴛɪᴏɴ ɪɴ sᴇᴄᴏɴᴅs]: sᴇᴇᴋ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ ʙᴀᴄᴋᴡᴀʀᴅ.\n\n/reload: ʀᴇʟᴏᴀᴅs ᴛʜᴇ ᴀᴅᴍɪɴ ᴄᴀᴄʜᴇ.\n\n/adminmode [admins|auth|everyone]: ᴡʜᴏ ᴄᴏɴᴛʀᴏʟs ᴘʟᴀʏʙᴀᴄᴋ.\n/playmode [admin|everyone]: ᴡʜᴏ ᴄᴀɴ sᴛᴀʀᴛ ᴘʟᴀʏʙᴀᴄᴋ.\n/cmddelete [on|off]: ᴅᴇʟᴇᴛᴇ ᴄᴏᴍᴍᴀɴᴅ ᴍᴇssᴀɢᴇs.\n/cleanmode [on|off|min]: ᴅᴇʟᴇᴛᴇ ʙᴏᴛ ᴍᴇssᴀɢᴇs ᴀғᴛᴇʀ ᴀ ᴡʜɪʟᴇ.\n/nothumb [on|off]: ʜɪᴅᴇ ᴛʜᴇ ᴛʜᴜᴍʙɴᴀɪʟ.\n/channelplay [ᴄʜᴀɴɴᴇʟ]: ᴘʟᴀʏ ɪɴ ᴀ ᴄʜᴀɴɴᴇʟ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n/userbot: ᴍᴀᴋᴇ ᴛʜᴇ ᴀssɪsᴛᴀɴᴛ ᴊᴏɪɴ ᴛʜɪs ᴄʜᴀᴛ.\n/stream [sᴏɴɢ]: ᴘᴜsʜ ᴀ ᴠɪᴅᴇᴏ ᴛᴏ ᴛʜɪs ᴄʜᴀᴛ's ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ; /streamstop, /streamstatus.",
    "help_auth": "<u><b>ᴀᴜᴛʜ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n<i>ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ᴄᴀɴ ᴄᴏɴᴛʀᴏʟ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ ᴡɪᴛʜᴏᴜᴛ ʙᴇɪɴɢ ᴀɴ ᴀᴅᴍɪɴ.</i>\n\n/auth: ᴀᴅᴅ ᴀ ᴜsᴇʀ ᴛᴏ ᴛʜᴇ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ.\n/unauth: ʀᴇᴍᴏᴠᴇ ᴀ ᴜsᴇʀ ғʀᴏᴍ ᴛʜᴇ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs ʟɪsᴛ.",
    "help_blist": "<u><b>ʙʟᴀᴄᴋʟɪsᴛ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n<i>ʙʟᴀᴄᴋʟɪsᴛᴇᴅ ᴄʜᴀᴛs ᴀɴᴅ ᴜsᴇʀs ᴄᴀɴɴᴏᴛ ᴜsᴇ ᴛʜᴇ ʙᴏᴛ.</i>\n\n/blacklist [chat_id|user_id]: ᴀᴅᴅ ᴀ ᴄʜᴀᴛ/ᴜsᴇʀ ᴛᴏ ᴛʜᴇ ʙʟᴀᴄᴋʟɪsᴛ.\n/unblacklist [chat_id|user_id]: ʀᴇᴍᴏᴠᴇ ᴀ ᴄʜᴀᴛ/ᴜsᴇʀ ғʀᴏᴍ ᴛʜᴇ ʙʟᴀᴄᴋʟɪsᴛ",
    "help_lang": "<u><b>ʟᴀɴɢᴜᴀɢᴇ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n<i>ᴛʜɪs ʙᴏᴛ sᴜᴘᴘᴏʀᴛs ᴍᴜʟᴛɪᴘʟᴇ ʟᴀɴɢᴜᴀɢᴇs.</i>\n\n/lang: ᴄʜᴀɴɢᴇ ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ʙᴏᴛ.",
    "help_ping": "<u><b>ᴘɪɴɢ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n\n/help: sʜᴏᴡs ᴛʜᴇ ʜᴇʟᴘ ᴍᴇɴᴜ ᴏғ ᴛʜᴇ ʙᴏᴛ.\n\n/ping: ᴄʜᴇᴄᴋ ᴛʜᴇ ᴘɪɴɢ ᴀɴᴅ ᴍᴇᴍᴏʀʏ ᴜsᴀɢᴇ ᴏғ ᴛʜᴇ ʙᴏᴛ.\n\n/start: sᴛᴀʀᴛ ᴛʜᴇ ʙᴏᴛ.\n\n/sudolist: sʜᴏᴡs ᴛʜᴇ ʟɪsᴛ ᴏғ sᴜᴅᴏ ᴜsᴇʀs ᴏғ ᴛʜᴇ ʙᴏᴛ.",
    "help_play": "<u><b>ᴘʟᴀʏ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n<i>ʏᴏᴜ ᴄᴀɴ ᴘʟᴀʏ ᴍᴜsɪᴄ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ᴜsɪɴɢ ᴛʜᴇ ғᴏʟʟᴏᴡɪɴɢ ᴄᴏᴍᴍᴀɴᴅs.</i>\n\n/play [sᴏɴɢ ɴᴀᴍᴇ/ʏᴏᴜᴛᴜʙᴇ ᴜʀʟ/ʀᴇᴘʟʏ ᴛᴏ ᴀᴜᴅɪᴏ]: ᴘʟᴀʏ ᴍᴜsɪᴄ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n/vplay [sᴏɴɢ ɴᴀᴍᴇ/ʏᴏᴜᴛᴜʙᴇ ᴜʀʟ/ʀᴇᴘʟʏ ᴛᴏ ᴠɪᴅᴇᴏ]: ᴘʟᴀʏ ᴍᴜsɪᴄ ᴠɪᴅᴇᴏ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n-f: ғᴏʀᴄᴇ ᴘʟᴀʏ ᴍᴜsɪᴄ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n-v: ᴘʟᴀʏs ᴍᴜsɪᴄ ᴠɪᴅᴇᴏ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n\n<b>ᴇxᴀᴍᴘʟᴇ:</b> <code>/play -f -v attention</code>\n\n/autoplay [on|off|lang|mood]: ᴋᴇᴇᴘ ʀᴇʟᴀᴛᴇᴅ sᴏɴɢs ᴘʟᴀʏɪɴɢ ᴡʜᴇɴ ᴛʜᴇ ǫᴜᴇᴜᴇ ᴇɴᴅs.\n/thumbnail [on|off]: sʜᴏᴡ ᴏʀ ʜɪᴅᴇ ᴛʜᴇ ᴛʜᴜᴍʙɴᴀɪʟ ᴏɴ ᴛʜᴇ ᴘʟᴀʏᴇʀ ᴄᴀʀᴅ.\n\n/fplay, /forceplay: ғᴏʀᴄᴇ ᴘʟᴀʏ. /fvplay: ғᴏʀᴄᴇ ᴘʟᴀʏ ᴠɪᴅᴇᴏ.\nsᴘᴏᴛɪғʏ ʟɪɴᴋs (ᴛʀᴀᴄᴋ, ᴀʟʙᴜᴍ, ᴘʟᴀʏʟɪsᴛ) ᴡᴏʀᴋ ᴡɪᴛʜ /play.\n/cplay, /cvplay, /cfplay...: sᴀᴍᴇ ᴀs /play, ʙᴜᴛ ɪɴ ᴛʜᴇ ʟɪɴᴋᴇᴅ ᴄʜᴀɴɴᴇʟ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.\n\n/addplaylist, /playlist, /rmplaylist: ʏᴏᴜʀ ᴘᴇʀsᴏɴᴀʟ ᴘʟᴀʏʟɪsᴛ.\n/playplaylist [-s] [-v]: ᴘʟᴀʏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ.\nʟɪɴᴋs ғʀᴏᴍ sᴘᴏᴛɪғʏ, ᴀᴘᴘʟᴇ ᴍᴜsɪᴄ, ᴊɪᴏsᴀᴀᴠɴ ᴀɴᴅ sᴏᴜɴᴅᴄʟᴏᴜᴅ ᴡᴏʀᴋ ᴡɪᴛʜ /play.",
    "help_queue": "<u><b>ǫᴜᴇᴜᴇ ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n\n/queue: sʜᴏᴡs ᴛʜᴇ ᴄᴜʀʀᴇɴᴛʟʏ ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs ɪɴ ᴛʜᴇ ǫᴜᴇᴜᴇ.\n\n/clear: ᴄʟᴇᴀʀ ᴛʜᴇ ᴡᴀɪᴛɪɴɢ ǫᴜᴇᴜᴇ.\n/restore: ʙʀɪɴɢ ʙᴀᴄᴋ ᴀ ᴄʟᴇᴀʀᴇᴅ ǫᴜᴇᴜᴇ.\n/remove [ɴ]: ʀᴇᴍᴏᴠᴇ ᴀ ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋ.\n/move [ғʀᴏᴍ] [ᴛᴏ]: ʀᴇᴏʀᴅᴇʀ ᴛʜᴇ ǫᴜᴇᴜᴇ.\n/shuffle: sʜᴜғғʟᴇ ᴛʜᴇ ǫᴜᴇᴜᴇ.\n/pos: ᴄᴜʀʀᴇɴᴛ ᴘᴏsɪᴛɪᴏɴ.\n/jump [ᴛɪᴍᴇ]: ᴊᴜᴍᴘ ᴛᴏ ᴀɴ ᴇxᴀᴄᴛ ᴛɪᴍᴇ.\n/speed [0.5-2.0]: ᴘʟᴀʏʙᴀᴄᴋ sᴘᴇᴇᴅ.\n/mute, /unmute: ᴍᴜᴛᴇ ᴛʜᴇ sᴛʀᴇᴀᴍ.\n/replay: ᴘʟᴀʏ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴛʀᴀᴄᴋ ᴀɢᴀɪɴ.\n/player: ʀᴇsᴇɴᴅ ᴛʜᴇ ᴘʟᴀʏᴇʀ ᴄᴀʀᴅ.",
    "help_stats": "<u><b>sᴛᴀᴛs ᴄᴏᴍᴍᴀɴᴅs:</b></u>\n\n/stats: sʜᴏᴡs ᴛʜᴇ ʙᴏᴛ's sᴛᴀᴛs.",
    "help_sudo": "<b><u>sᴜᴅᴏ ᴄᴏᴍᴍᴀɴᴅs:</u></b>\n\n/ac: sʜᴏᴡs ᴛʜᴇ ᴀᴄᴛɪᴠᴇ ᴄᴀʟʟs ᴄᴏᴜɴᴛ.\n\n/activevc: sʜᴏᴡs ᴛʜᴇ ʟɪsᴛ ᴏғ ᴛʜᴇ ᴀᴄᴛɪᴠᴇ ᴄᴀʟʟs.\n\n/broadcast [ʀᴇᴘʟʏ ᴛᴏ ᴍᴇssᴀɢᴇ]: ʙʀᴏᴀᴅᴄᴀsᴛs ᴛʜᴇ ᴍᴇssᴀɢᴇ ᴛᴏ ᴀʟʟ ᴛʜᴇ ᴄʜᴀᴛs.\n-ɴᴏᴄʜᴀᴛ: ᴇxᴄʟᴜᴅᴇs ɢʀᴏᴜᴘs ғʀᴏᴍ ᴛʜᴇ ʙʀᴏᴀᴅᴄᴀsᴛ.\n-ᴜsᴇʀ: ɪɴᴄʟᴜᴅᴇ ᴜsᴇʀs ɪɴ ᴛʜᴇ ʙʀᴏᴀᴅᴄᴀsᴛ.\n-ᴄᴏᴘʏ: ʀᴇᴍᴏᴠᴇs ᴛʜᴇ ғᴏʀᴡᴀʀᴅᴇᴅ ᴛᴀɢ ғʀᴏᴍ ᴛʜᴇ ʙʀᴏᴀᴅᴄᴀsᴛ ᴍᴇssᴀɢᴇ.\n<b>ᴇxᴀᴍᴘʟᴇ:</b> <code>/broadcast -user -copy</code>\n\n/eval: ᴇxᴇᴄᴜᴛᴇs ᴛʜᴇ ɢɪᴠᴇɴ ᴄᴏᴅᴇ.\n\n/logs: sᴇɴᴅs ᴛʜᴇ ʟᴏɢ ғɪʟᴇ.\n\n/logger [on|off]: ᴇɴᴀʙʟᴇs/ᴅɪsᴀʙʟᴇs ᴛʜᴇ ʟᴏɢɢᴇʀ.\n\n/restart: ʀᴇsᴛᴀʀᴛs ᴛʜᴇ ʙᴏᴛ.\n\n/addsudo: ᴀᴅᴅ ᴀ ᴜsᴇʀ ᴛᴏ ᴛʜᴇ sᴜᴅᴏ ᴜsᴇʀs ʟɪsᴛ.\n/rmsudo: ʀᴇᴍᴏᴠᴇ ᴀ ᴜsᴇʀ ғʀᴏᴍ ᴛʜᴇ sᴜᴅᴏ ᴜsᴇʀs ʟɪsᴛ.\n\n/maintenance [on|off] [ʀᴇᴀsᴏɴ]: ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ ᴍᴏᴅᴇ.\n/autoleave [on|off]: ᴀssɪsᴛᴀɴᴛ ʟᴇᴀᴠᴇs ɪᴅʟᴇ ᴄʜᴀᴛs.\n/json: ᴍᴇssᴀɢᴇ ᴊsᴏɴ.\n/speedtest: sᴇʀᴠᴇʀ sᴘᴇᴇᴅ.\n/blacklisted: ʙʟᴀᴄᴋʟɪsᴛ ᴏᴠᴇʀᴠɪᴇᴡ.\n/setrtmp [ᴄʜᴀᴛ ɪᴅ] [ᴜʀʟ] [ᴋᴇʏ]: ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ (ᴘʀɪᴠᴀᴛᴇ ᴄʜᴀᴛ).\n/bug [ᴛᴇxᴛ]: ʀᴇᴘᴏʀᴛ ᴀ ʙᴜɢ.\n/privacy: ᴘʀɪᴠᴀᴄʏ ᴘᴏʟɪᴄʏ.",
    "lang_choose": "ᴘʟᴇᴀsᴇ ᴄʜᴏᴏsᴇ ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ʏᴏᴜ ᴡᴀɴᴛ ᴛᴏ sᴇᴛ ғᴏʀ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ:",
    "lang_change": "ᴄʜᴀɴɢɪɴɢ ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ᴛᴏ: {0}",
    "lang_changed": "ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ʜᴀs ʙᴇᴇɴ ᴄʜᴀɴɢᴇᴅ ᴛᴏ: <i>{0}</i>",
    "lang_same": "ᴛʜᴇ ʟᴀɴɢᴜᴀɢᴇ ᴏғ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴄʜᴀᴛ ɪs ᴀʟʀᴇᴀᴅʏ sᴇᴛ ᴛᴏ: {0}",
    "log_fetch": "» ғᴇᴛᴄʜɪɴɢ ʟᴏɢs...",
    "log_not_found": "» ʟᴏɢ ғɪʟᴇ ᴅᴏᴇsɴ'ᴛ ᴇxɪsᴛ.",
    "log_sent": "ʟᴏɢ ғɪʟᴇ ᴏғ {0}",
    "log_chat": "<u><b>ɴᴇᴡ ᴄʜᴀᴛ ʟᴏɢ</b></u>\n\n<b>ᴄʜᴀᴛ:</b> <code>{0}</code> | {1}\n<b>ᴜsᴇʀ:</b> <code>{2}</code> | {3}",
    "log_user": "<u><b>ɴᴇᴡ ᴜsᴇʀ ʟᴏɢ</b></u>\n\n<b>ɪᴅ:</b> <code>{0}</code>\n<b>ɴᴀᴍᴇ:</b> {1} | {2}",
    "logger_on": "» ʟᴏɢɢᴇʀ ᴇɴᴀʙʟᴇᴅ.",
    "logger_off": "» ʟᴏɢɢᴇʀ ᴅɪsᴀʙʟᴇᴅ.",
    "logger_usage": "<b>ᴜsᴀɢᴇ:</b>\n\n/{0} [on|off]",
    "loop_count": "ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ʟᴏᴏᴘ ᴄᴏᴜɴᴛ ɪs: {0}",
    "loop_set": "ʟᴏᴏᴘ ᴄᴏᴜɴᴛ sᴇᴛ ᴛᴏ: {0}",
    "loop_off": "» ʟᴏᴏᴘ ᴅɪsᴀʙʟᴇᴅ.",
    "loop_usage": "<b>ᴜsᴀɢᴇ:</b>\n\n<code>/loop [count]</code>\n<code>/loop off</code>",
    "not_playing": "» ᴛʜᴇ ʙᴏᴛ ɪsɴ'ᴛ sᴛʀᴇᴀᴍɪɴɢ ɪɴ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.",
    "pinging": "» ᴘɪɴɢɪɴɢ… ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ…",
    "ping_pong": "<u><b>ᴘᴏɴɢ!</b></u>\n\n<b>ʟᴀᴛᴇɴᴄʏ:</b> <code>{0}ms</code>\n\n<b>ᴜᴘᴛɪᴍᴇ:</b> {1}\n<b>ᴄᴘᴜ:</b> <code>{2}%</code>\n<b>ʀᴀᴍ:</b> <code>{3}%</code>\n<b>ᴅɪsᴋ:</b> <code>{4}%</code>\n<b>ɴᴛɢᴄᴀʟʟs ʟᴀᴛᴇɴᴄʏ:</b> <code>{5}ms</code>",
    "play_admin": "<u><b>ᴀᴅᴍɪɴ ᴏɴʟʏ ᴘʟᴀʏ</b></u>\n\nᴏɴʟʏ ᴀᴅᴍɪɴs ᴀʀᴇ ᴀʟʟᴏᴡᴇᴅ ᴛᴏ ᴘʟᴀʏ ɪɴ ᴛʜɪs ᴄʜᴀᴛ.",
    "play_banned": "<u><b>{0} ᴀssɪsᴛᴀɴᴛ ɪs ʙᴀɴɴᴇᴅ ʏᴏᴜʀ ᴄʜᴀᴛ</b></u>\n\n<b>ɪᴅ:</b> <code>{1}</code>\n<b>ɴᴀᴍᴇ:</b> {2}\n<b>ᴜsᴇʀɴᴀᴍᴇ:</b> {3}",
    "play_media": "<u><b>| sᴛᴀʀᴛᴇᴅ sᴛʀᴇᴀᴍɪɴɢ</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href=\"{0}\">{1}</a>\n\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {2} ᴍɪɴ\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {3}",
    "play_log": "<u>{0} ᴘʟᴀʏ ʟᴏɢ</u>\n\n<b>sᴏɴɢ:</b> {6}\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {7} ᴍɪɴ\n\n<b>ɢʀᴏᴜᴘ ɪᴅ:</b> <code>{1}</code>\n<b>ɢʀᴏᴜᴘ:</b> {2}\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> <code>{3}</code> | {4}\n<b>ᴍᴇssᴀɢᴇ ʟɪɴᴋ:</b> {5}",
    "play_queued": "<u><b>ᴀᴅᴅᴇᴅ ᴛᴏ ǫᴜᴇᴜᴇ: {0}</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href=\"{1}\">{2}</a>\n\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {3} ᴍɪɴ\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {4}",
    "play_usage": "<b>ᴜsᴀɢᴇ:</b>\n\n<code>/play attention</code>",
    "play_seeking": "» sᴇᴇᴋɪɴɢ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ sᴛʀᴇᴀᴍ...",
    "play_again": "» ʀᴇᴘʟᴀʏɪɴɢ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛ ᴍᴇᴅɪᴀ...",
    "play_now": "ᴘʟᴀʏ ɴᴏᴡ",
    "play_next": "» ʜᴏʟᴅ ᴏɴ...\n\nᴅᴏᴡɴʟᴏᴀᴅɪɴɢ ɴᴇxᴛ ᴍᴇᴅɪᴀ ғʀᴏᴍ ᴛʜᴇ ǫᴜᴇᴜᴇ.",
    "play_invite": "» ʜᴏʟᴅ ᴏɴ ᴀ ᴍᴏᴍᴇɴᴛ...\n\nɪɴᴠɪᴛɪɴɢ {0} ᴀssɪsᴛᴀɴᴛ ᴛᴏ ʏᴏᴜʀ ᴄʜᴀᴛ.",
    "play_searching": "sᴇᴀʀᴄʜɪɴɢ...",
    "play_downloading": "» ᴅᴏᴡɴʟᴏᴀᴅɪɴɢ...",
    "play_paused": "<b>sᴛʀᴇᴀᴍ ᴘᴀᴜsᴇᴅ ʙʏ</b> {0}",
    "play_resumed": "<b>sᴛʀᴇᴀᴍ ʀᴇsᴜᴍᴇᴅ ʙʏ</b> {0}",
    "play_replayed": "<b>sᴛʀᴇᴀᴍ ʀᴇᴘʟᴀʏᴇᴅ ʙʏ</b> {0}",
    "play_skipped": "<b>sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ ʙʏ</b> {0}",
    "play_stopped": "<b>sᴛʀᴇᴀᴍ ᴇɴᴅᴇᴅ ʙʏ</b> {0}",
    "play_seeked": "<b>sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ {0} ᴀɴᴅ sᴛᴀʀᴛᴇᴅ ғʀᴏᴍ {1} sᴇᴄᴏɴᴅs ʙʏ</b> {2}",
    "play_expired": "» ᴛʜɪs ʙᴜᴛᴛᴏɴ ʜᴀs ᴇxᴘɪʀᴇᴅ.",
    "play_unsupported": "» ʟɪᴠᴇ sᴛʀᴇᴀᴍs ᴀʀᴇ ɴᴏᴛ sᴜᴘᴘᴏʀᴛᴇᴅ.",
    "play_already_paused": "» ᴅᴏ ʏᴏᴜ ʀᴇᴍᴇᴍʙᴇʀ ᴛʜᴀᴛ ʏᴏᴜ ʀᴇsᴜᴍᴇᴅ ᴛʜᴇ sᴛʀᴇᴀᴍ?",
    "play_not_paused": "» ᴅᴏ ʏᴏᴜ ʀᴇᴍᴇᴍʙᴇʀ ᴛʜᴀᴛ ʏᴏᴜ ᴘᴀᴜsᴇᴅ ᴛʜᴇ sᴛʀᴇᴀᴍ?",
    "play_seek_usage": "<b>ᴜsᴀɢᴇ:</b> /{0} ᴅᴜʀᴀᴛɪᴏɴ\n<b>ᴇxᴀᴍᴘʟᴇ:</b> <code>/{0} 15</code>",
    "play_seek_no_dur": "» ғᴀɪʟᴇᴅ ᴛᴏ ғᴇᴛᴄʜ ᴛʜᴇ ᴅᴜʀᴀᴛɪᴏɴ ᴏғ ᴛʜᴇ ᴏɴɢᴏɪɴɢ sᴛʀᴇᴀᴍ.",
    "play_seek_min": "» ᴍɪɴɪᴍᴜᴍ sᴇᴇᴋ ᴛɪᴍᴇ ɪs 10 sᴇᴄᴏɴᴅs — ᴛʀʏ ᴀ ʙɪᴛ ʟᴏɴɢᴇʀ!",
    "play_duration_limit": "» sᴛʀᴇᴀᴍs ʟᴏɴɢᴇʀ ᴛʜᴀɴ {0} ᴍɪɴᴜᴛᴇs ᴀʀᴇ ɴᴏᴛ ᴀʟʟᴏᴡᴇᴅ ᴛᴏ ᴘʟᴀʏ.",
    "play_queue_full": "» ᴛʜᴇ ǫᴜᴇᴜᴇ ʟɪᴍɪᴛ ({0}) ʜᴀs ʙᴇᴇɴ ʀᴇᴀᴄʜᴇᴅ. ᴘʟᴇᴀsᴇ ᴡᴀɪᴛ ғᴏʀ ᴛʜᴇ ᴄᴜʀʀᴇɴᴛʟʏ ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs ᴛᴏ ғɪɴɪsʜ ᴘʟᴀʏɪɴɢ, ᴛʜᴇɴ ᴛʀʏ ᴀɢᴀɪɴ.",
    "play_invite_error": "ғᴀɪʟᴇᴅ ᴛᴏ ɪɴᴠɪᴛᴇ ᴀssɪsᴛᴀɴᴛ ᴛᴏ ᴛʜᴇ ᴄʜᴀᴛ.\n\nʀᴇᴀsᴏɴ: <code>{0}</code>",
    "play_not_found": "» ғᴀɪʟᴇᴅ ᴛᴏ ᴘʀᴏᴄᴇss ᴛʜᴇ ǫᴜᴇʀʏ.\n\nɪғ ᴛʜᴇ ɪssᴜᴇ ᴘᴇʀsɪsᴛs, ʀᴇᴘᴏʀᴛ ɪᴛ ᴛᴏ ᴛʜᴇ <a href=\"{0}\">sᴜᴘᴘᴏʀᴛ ᴄʜᴀᴛ</a>.",
    "play_chat_invalid": "» ᴛʜɪs ʙᴏᴛ ᴄᴀɴ ᴏɴʟʏ ʙᴇ ᴜsᴇᴅ ɪɴ <b>sᴜᴘᴇʀɢʀᴏᴜᴘs</b>.\n\nᴛᴏ ᴄᴏɴᴠᴇʀᴛ ʏᴏᴜʀ ɢʀᴏᴜᴘ ᴛᴏ ᴀ sᴜᴘᴇʀɢʀᴏᴜᴘ, ᴍᴀᴋᴇ ᴛʜᴇ ᴄʜᴀᴛ ʜɪsᴛᴏʀʏ <b>ᴠɪsɪʙʟᴇ</b> ᴏɴᴄᴇ.\n\nʟᴇᴀᴠɪɴɢ ᴛʜɪs ᴄʜᴀᴛ...",
    "play_user_invalid": "» ʏᴏᴜ'ʀᴇ ᴀ ʜɪᴅᴅᴇɴ ᴀᴅᴍɪɴ.\n\nʀᴇᴠᴇʀᴛ ʙᴀᴄᴋ ᴛᴏ ᴜsᴇʀ ᴀᴄᴄᴏᴜɴᴛ.",
    "playlist_fetch": "» ғᴇᴛᴄʜɪɴɢ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ...\n\nᴘʟᴇᴀsᴇ ʜᴏʟᴅ ᴏɴ.",
    "playlist_error": "» sᴏᴍᴇᴛʜɪɴɢ ᴡᴇɴᴛ ᴡʀᴏɴɢ ᴡʜɪʟᴇ ғᴇᴛᴄʜɪɴɢ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ.",
    "playlist_queued": "<u><b>ᴀᴅᴅᴇᴅ {0} ᴛʀᴀᴄᴋs ғʀᴏᴍ ᴛʜᴇ ᴘʟᴀʏʟɪsᴛ ᴛᴏ ǫᴜᴇᴜᴇ:</b></u>\n\n",
    "queue_curr": "<u><b>ᴄᴜʀʀᴇɴᴛʟʏ ᴘʟᴀʏɪɴɢ:</b></u>\n\n<b>ᴛɪᴛʟᴇ:</b> <a href=\"{0}\">{1}</a>\n<b>ᴅᴜʀᴀᴛɪᴏɴ:</b> {2}\n<b>ʀᴇǫᴜᴇsᴛᴇᴅ ʙʏ:</b> {3}\n\n",
    "queue_item": "<b>{0}. ᴛɪᴛʟᴇ:</b> {1}\n     - {2} ᴍɪɴ\n\n",
    "queue_fetching": "» ғᴇᴛᴄʜɪɴɢ ǫᴜᴇᴜᴇ...",
    "restarting": "» ʀᴇsᴛᴀʀᴛɪɴɢ...",
    "restarted": "» ʀᴇsᴛᴀʀᴛ ɪɴ ᴘʀᴏɢʀᴇss. ᴅᴏɴ'ᴛ ᴡᴏʀʀʏ, ɪᴛ'ʟʟ ᴏɴʟʏ ᴛᴀᴋᴇ ᴀ ғᴇᴡ sᴇᴄᴏɴᴅs… ᴍᴀʏʙᴇ.",
    "start_pm": "ʜᴇʏ {0},\nᴛʜɪs ɪs {1} !\n\nᴀ ᴍᴜsɪᴄ ᴘʟᴀʏᴇʀ ʙᴏᴛ ᴡɪᴛʜ sᴏᴍᴇ ᴀᴡᴇsᴏᴍᴇ ᴀɴᴅ ᴜsᴇғᴜʟ ғᴇᴀᴛᴜʀᴇs.\n\n<b><i>ᴄʟɪᴄᴋ ᴏɴ ᴛʜᴇ ʜᴇʟᴘ ʙᴜᴛᴛᴏɴ ғᴏʀ ᴍᴏʀᴇ ɪɴғᴏ.</i></b>",
    "start_gp": "ʜᴇʏ,\nᴛʜɪs ɪs {0}\n\n<u><b>ᴀ ᴍᴜsɪᴄ ᴘʟᴀʏᴇʀ ʙᴏᴛ ᴡɪᴛʜ sᴏᴍᴇ ᴀᴡᴇsᴏᴍᴇ ᴀɴᴅ ᴜsᴇғᴜʟ ғᴇᴀᴛᴜʀᴇs.</b></u>",
    "start_settings": "<u><b>{0} sᴇᴛᴛɪɴɢs</b></u>\n\nᴄʟɪᴄᴋ ᴛʜᴇ ʙᴜᴛᴛᴏɴs ʙᴇʟᴏᴡ ᴛᴏ ᴄʜᴀɴɢᴇ ᴛʜɪs ᴄʜᴀᴛ's ᴄᴜʀʀᴇɴᴛ sᴇᴛᴛɪɴɢs.",
    "stats_fetching": "» ғᴇᴛᴄʜɪɴɢ sᴛᴀᴛs...",
    "stats_sudo": "\n\n<b>ᴍᴏᴅᴜʟᴇs:</b> {0}\n<b>ᴘʟᴀᴛғᴏʀᴍ:</b> {1}\n<b>ʀᴀᴍ ᴜsᴀɢᴇ:</b> <code>{2}MB | {3}GB</code>\n<b>ᴄᴘᴜ ᴜsᴀɢᴇ:</b> <code>{4}% ({5} cores)</code>\n<b>sᴛᴏʀᴀɢᴇ:</b> <code>{6}GB | {7}GB</code>\n\n<b>ʟᴀɴɢᴜᴀɢᴇ:</b> <code>{8}</code>\n<b>ʟɪʙʀᴀʀʏ:</b> <code>{9}</code>\n<b>ᴠᴏɪᴄᴇ:</b> <code>{10}</code>",
    "stats_user": "<u><b>{0} sᴛᴀᴛs</b></u>\n\n<b>ᴀssɪsᴛᴀɴᴛs:</b> {1}\n<b>ᴀᴜᴛᴏ ʟᴇᴀᴠᴇ:</b> {2}\n\n<b>ʙʟᴏᴄᴋᴇᴅ ᴄʜᴀᴛs:</b> {3}\n<b>ʙʟᴏᴄᴋᴇᴅ ᴜsᴇʀs:</b> {4}\n<b>sᴜᴅᴏ ᴜsᴇʀs:</b> {5}\n\n<b>sᴇʀᴠᴇᴅ ᴄʜᴀᴛs:</b> {6}\n<b>sᴇʀᴠᴇᴅ ᴜsᴇʀs:</b> {7}",
    "sudo_already": "» {0} ɪs ᴀʟʀᴇᴀᴅʏ ᴀɴ sᴜᴅᴏ ᴜsᴇʀ.",
    "sudo_added": "» ᴀᴅᴅᴇᴅ {0} ᴛᴏ ᴛʜᴇ sᴜᴅᴏ ᴜsᴇʀs ʟɪsᴛ.",
    "sudo_not": "» {0} ɪs ɴᴏᴛ ᴀɴ sᴜᴅᴏ ᴜsᴇʀ.",
    "sudo_removed": "» ʀᴇᴍᴏᴠᴇᴅ {0} ғʀᴏᴍ ᴛʜᴇ sᴜᴅᴏ ᴜsᴇʀs ʟɪsᴛ.",
    "sudo_fetching": "» ғᴇᴛᴄʜɪɴɢ sᴜᴅᴏ ᴜsᴇʀs ʟɪsᴛ...",
    "sudo_owner": "<u><b>ᴏᴡɴᴇʀ:</b></u>\n- {0}\n\n",
    "sudo_users": "<u><b>sᴜᴅᴏ ᴜsᴇʀs:</b></u>",
    "user_not_admin": "» ᴏʜ sᴜʀᴇ, ɢᴏ ᴀʜᴇᴀᴅ ᴀɴᴅ ᴍᴀɴᴀɢᴇ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ... ᴏʜ ᴡᴀɪᴛ, ʏᴏᴜ'ʀᴇ ɴᴏᴛ ᴀɴ ᴀᴅᴍɪɴ.",
    "user_no_perms": "» ʏᴏᴜ? ᴍᴀɴᴀɢᴇ ᴛʜᴇ ᴠɪᴅᴇᴏ ᴄʜᴀᴛ? ɴᴏᴛ ᴡɪᴛʜ ᴛʜᴏsᴇ ᴘᴇʀᴍɪssɪᴏɴs, sᴡᴇᴇᴛɪᴇ.",
    "user_not_found": "» ɪ'ʟʟ ɴᴇᴇᴅ ᴀ ᴜsᴇʀ ɪᴅ — ᴏʀ ᴡᴏᴜʟᴅ ʏᴏᴜ ᴍɪɴᴅ ʀᴇᴘʟʏɪɴɢ ᴛᴏ sᴏᴍᴇᴏɴᴇ's ᴍᴇssᴀɢᴇ?",
    "vc_empty": "» ʟᴏᴏᴋs ʟɪᴋᴇ ᴛʜᴇʀᴇ ᴀʀᴇɴ'ᴛ ᴀɴʏ ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs ᴏɴ ᴛʜᴇ ʙᴏᴛ.",
    "vc_count": "ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs ᴏɴ ᴛʜᴇ ʙᴏᴛ: <b>{0}</b>",
    "vc_fetching": "» ғᴇᴛᴄʜɪɴɢ ʟɪsᴛ ᴏғ ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs...",
    "vc_list": "<u><b>ʟɪsᴛ ᴏғ ᴀᴄᴛɪᴠᴇ sᴛʀᴇᴀᴍs:</b></u>",
    "back": "ʙᴀᴄᴋ",
    "backward": "ʙᴀᴄᴋᴡᴀʀᴅ",
    "cancel": "ᴄᴀɴᴄᴇʟ",
    "channel": "ᴄʜᴀɴɴᴇʟ",
    "home": "ʜᴏᴍᴇ",
    "close": "ᴄʟᴏsᴇ",
    "forward": "ғᴏʀᴡᴀʀᴅ",
    "help": "ʜᴇʟᴘ",
    "language": "ʟᴀɴɢᴜᴀɢᴇ",
    "paused": "sᴛʀᴇᴀᴍ ᴘᴀᴜsᴇᴅ",
    "cmd_delete": "ᴄᴏᴍᴍᴀɴᴅ ᴅᴇʟᴇᴛᴇ",
    "play_mode": "ᴀᴅᴍɪɴ ᴏɴʟʏ ᴘʟᴀʏ",
    "playing": "ᴘʟᴀʏɪɴɢ",
    "processing": "» ᴘʀᴏᴄᴇssɪɴɢ...",
    "replayed": "sᴛʀᴇᴀᴍ ʀᴇᴘʟᴀʏᴇᴅ",
    "skipped": "sᴛʀᴇᴀᴍ sᴋɪᴘᴘᴇᴅ",
    "stopped": "sᴛʀᴇᴀᴍ ᴇɴᴅᴇᴅ",
    "source": "sᴏᴜʀᴄᴇ",
    "support": "sᴜᴘᴘᴏʀᴛ",
    "help_0": "ᴀᴅᴍɪɴs",
    "help_1": "ᴀᴜᴛʜ",
    "help_2": "ʙʟᴀᴄᴋʟɪsᴛ",
    "help_3": "ʟᴀɴɢᴜᴀɢᴇ",
    "help_4": "ᴘɪɴɢ",
    "help_5": "ᴘʟᴀʏ",
    "help_6": "ǫᴜᴇᴜᴇ",
    "help_7": "sᴛᴀᴛs",
    "help_8": "sᴜᴅᴏᴇʀs",
    "autoplay_state_on": "ᴏɴ ✅",
    "autoplay_state_off": "ᴏғғ ❌",
    "autoplay_status": "<b>ᴀᴜᴛᴏᴘʟᴀʏ:</b> {0}\n<b>ʟᴀɴɢᴜᴀɢᴇ:</b> <code>{1}</code>\n<b>ᴍᴏᴏᴅ:</b> <code>{2}</code>\n\n",
    "autoplay_usage": "<u><b>ᴀᴜᴛᴏᴘʟᴀʏ</b></u>\n<i>ᴡʜᴇɴ ᴛʜᴇ ǫᴜᴇᴜᴇ ᴇɴᴅs, ʀᴇʟᴀᴛᴇᴅ sᴏɴɢs ᴋᴇᴇᴘ ᴘʟᴀʏɪɴɢ.</i>\n\n<code>/autoplay on</code> • <code>/autoplay off</code>\n<code>/autoplay lang hindi</code> (ᴏʀ <code>auto</code>)\n<code>/autoplay mood sad</code> (ᴏʀ <code>any</code>)\n\n<b>ʟᴀɴɢᴜᴀɢᴇs:</b> {0}\n<b>ᴍᴏᴏᴅs:</b> {1}",
    "autoplay_enabled": "<b>ᴀᴜᴛᴏᴘʟᴀʏ ᴇɴᴀʙʟᴇᴅ ʙʏ</b> {0}\nᴡʜᴇɴ ᴛʜᴇ ǫᴜᴇᴜᴇ ᴇɴᴅs, ʀᴇʟᴀᴛᴇᴅ sᴏɴɢs ᴡɪʟʟ ᴋᴇᴇᴘ ᴘʟᴀʏɪɴɢ.",
    "autoplay_disabled": "<b>ᴀᴜᴛᴏᴘʟᴀʏ ᴅɪsᴀʙʟᴇᴅ ʙʏ</b> {0}",
    "autoplay_lang_set": "<b>ᴀᴜᴛᴏᴘʟᴀʏ ʟᴀɴɢᴜᴀɢᴇ:</b> <code>{0}</code>",
    "autoplay_mood_set": "<b>ᴀᴜᴛᴏᴘʟᴀʏ ᴍᴏᴏᴅ:</b> <code>{0}</code>",
    "autoplay_btn_on": "ᴀᴜᴛᴏᴘʟᴀʏ ᴏɴ: ʀᴇʟᴀᴛᴇᴅ sᴏɴɢs ᴡɪʟʟ ᴋᴇᴇᴘ ᴘʟᴀʏɪɴɢ",
    "autoplay_btn_off": "ᴀᴜᴛᴏᴘʟᴀʏ ᴏғғ",
    "btn_autoplay_on": "ᴀᴜᴛᴏᴘʟᴀʏ | ᴏɴ",
    "btn_autoplay_off": "ᴀᴜᴛᴏᴘʟᴀʏ | ᴏғғ",
    "btn_thumb_on": "ᴛʜᴜᴍʙ | ᴏɴ",
    "btn_thumb_off": "ᴛʜᴜᴍʙ | ᴏғғ",
    "thumb_status": "<b>ᴛʜᴜᴍʙɴᴀɪʟ:</b> {0}\n\n<code>/thumbnail on</code> • <code>/thumbnail off</code>",
    "thumb_enabled": "<b>ᴛʜᴜᴍʙɴᴀɪʟ ᴇɴᴀʙʟᴇᴅ ʙʏ</b> {0}\nɪᴛ ᴡɪʟʟ ʙᴇ sʜᴏᴡɴ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs.",
    "thumb_disabled": "<b>ᴛʜᴜᴍʙɴᴀɪʟ ᴅɪsᴀʙʟᴇᴅ ʙʏ</b> {0}\nᴛʜᴇ ɴᴇxᴛ sᴏɴɢs ᴡɪʟʟ ʙᴇ sʜᴏᴡɴ ᴀs ᴛᴇxᴛ ᴏɴʟʏ.",
    "thumb_btn_on": "ᴛʜᴜᴍʙɴᴀɪʟ ᴇɴᴀʙʟᴇᴅ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs",
    "thumb_btn_off": "ᴛʜᴜᴍʙɴᴀɪʟ ᴅɪsᴀʙʟᴇᴅ ғᴏʀ ᴛʜᴇ ɴᴇxᴛ sᴏɴɢs",
    "queue_nothing_waiting": "» ᴛʜᴇʀᴇ ɪs ɴᴏᴛʜɪɴɢ ᴡᴀɪᴛɪɴɢ ɪɴ ᴛʜᴇ ǫᴜᴇᴜᴇ.",
    "queue_cleared": "ᴄʟᴇᴀʀᴇᴅ <b>{0}</b> ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs.\n\n» ʙʏ {1}",
    "queue_restore_none": "» ᴛʜᴇʀᴇ ɪs ɴᴏ ᴄʟᴇᴀʀᴇᴅ ǫᴜᴇᴜᴇ ᴛᴏ ʀᴇsᴛᴏʀᴇ.",
    "queue_restored": "ǫᴜᴇᴜᴇ ʀᴇsᴛᴏʀᴇᴅ.\n\n» ʙʏ {0}",
    "queue_remove_usage": "» ᴜsᴀɢᴇ: <code>/remove [position]</code> (ᴘᴏsɪᴛɪᴏɴ ᴀs sʜᴏᴡɴ ɪɴ /queue).",
    "queue_bad_position": "» ᴛʜᴀᴛ ᴘᴏsɪᴛɪᴏɴ ɪs ɴᴏᴛ ɪɴ ᴛʜᴇ ǫᴜᴇᴜᴇ.",
    "queue_removed": "ʀᴇᴍᴏᴠᴇᴅ #{0}: <b>{1}</b>\n\n» ʙʏ {2}",
    "queue_move_usage": "ᴜsᴀɢᴇ: <code>/move [from] [to]</code>",
    "queue_moved": "ᴍᴏᴠᴇᴅ #{0} ᴛᴏ #{1}.\n\n» ʙʏ {2}",
    "queue_shuffle_few": "» ɴᴇᴇᴅ ᴀᴛ ʟᴇᴀsᴛ ᴛᴡᴏ ᴡᴀɪᴛɪɴɢ ᴛʀᴀᴄᴋs ᴛᴏ sʜᴜғғʟᴇ.",
    "queue_shuffled": "sʜᴜғғʟᴇᴅ <b>{0}</b> ǫᴜᴇᴜᴇᴅ ᴛʀᴀᴄᴋs.\n\n» ʙʏ {1}",
    "position_now": "<b>{0}</b>\n\n{1} / {2}  •  sᴘᴇᴇᴅ {3}",
    "jump_usage": "» ᴜsᴀɢᴇ: <code>/jump [seconds | mm:ss]</code> — ᴊᴜᴍᴘ ᴛᴏ ᴀɴ ᴇxᴀᴄᴛ ᴛɪᴍᴇ ᴏғ ᴛʜᴇ ᴛʀᴀᴄᴋ.",
    "jump_range": "» ᴘɪᴄᴋ ᴀ ᴛɪᴍᴇ ɪɴsɪᴅᴇ ᴛʜᴇ ᴛʀᴀᴄᴋ (ʟᴇɴɢᴛʜ {0}).",
    "play_jumped": "ᴊᴜᴍᴘᴇᴅ ᴛᴏ <b>{0}</b>.\n\n» ʙʏ {1}",
    "speed_usage": "ᴄᴜʀʀᴇɴᴛ sᴘᴇᴇᴅ: <b>{0}</b>\nᴜsᴀɢᴇ: <code>/speed [0.5 - 2.0]</code>",
    "speed_set": "ᴘʟᴀʏʙᴀᴄᴋ sᴘᴇᴇᴅ sᴇᴛ ᴛᴏ <b>{0}</b>.\n\n» ʙʏ {1}",
    "mute_already": "» ᴛʜᴇ sᴛʀᴇᴀᴍ ɪs ᴀʟʀᴇᴀᴅʏ ᴍᴜᴛᴇᴅ.",
    "unmute_already": "» ᴛʜᴇ sᴛʀᴇᴀᴍ ɪs ɴᴏᴛ ᴍᴜᴛᴇᴅ.",
    "mute_done": "ᴍᴜᴛᴇᴅ <b>{0}</b>.\n\n» ʙʏ {1}",
    "unmute_done": "ᴜɴᴍᴜᴛᴇᴅ <b>{0}</b>.\n\n» ʙʏ {1}",
    "state_on": "ᴇɴᴀʙʟᴇᴅ",
    "state_off": "ᴅɪsᴀʙʟᴇᴅ",
    "adminmode_usage": "ᴡʜᴏ ᴄᴀɴ ᴄᴏɴᴛʀᴏʟ ᴘʟᴀʏʙᴀᴄᴋ: <b>{0}</b>\nᴜsᴀɢᴇ: <code>/adminmode [admins|auth|everyone]</code>",
    "adminmode_invalid": "» ᴄʜᴏᴏsᴇ ᴏɴᴇ ᴏғ: <code>admins</code>, <code>auth</code>, <code>everyone</code>.",
    "adminmode_set": "ᴘʟᴀʏʙᴀᴄᴋ ᴄᴏɴᴛʀᴏʟ ɪs ɴᴏᴡ: <b>{0}</b>\n\n» ʙʏ {1}",
    "adminmode_admin": "ᴀᴅᴍɪɴs ᴏɴʟʏ",
    "adminmode_adminauth": "ᴀᴅᴍɪɴs ᴀɴᴅ ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs",
    "adminmode_everyone": "ᴇᴠᴇʀʏᴏɴᴇ",
    "cmddelete_usage": "ᴄᴏᴍᴍᴀɴᴅ ᴅᴇʟᴇᴛɪᴏɴ ɪs <b>{0}</b>.\nᴜsᴀɢᴇ: <code>/cmddelete [on|off]</code>",
    "cmddelete_set": "ᴄᴏᴍᴍᴀɴᴅ ᴅᴇʟᴇᴛɪᴏɴ ɪs ɴᴏᴡ <b>{0}</b>.\n\n» ʙʏ {1}",
    "cleanmode_usage": "ᴄʟᴇᴀɴ ᴍᴏᴅᴇ ɪs <b>{0}</b> (ᴅᴇʟᴇᴛᴇ ᴀғᴛᴇʀ {1} ᴍɪɴ).\nᴜsᴀɢᴇ: <code>/cleanmode [on|off|minutes]</code>",
    "cleanmode_set": "ᴄʟᴇᴀɴ ᴍᴏᴅᴇ ɪs ɴᴏᴡ <b>{0}</b>.\n\n» ʙʏ {1}",
    "cleanmode_on": "ᴄʟᴇᴀɴ ᴍᴏᴅᴇ ᴏɴ: ʙᴏᴛ ᴍᴇssᴀɢᴇs ᴀʀᴇ ᴅᴇʟᴇᴛᴇᴅ ᴀғᴛᴇʀ <b>{0}</b> ᴍɪɴᴜᴛᴇs.\n\n» ʙʏ {1}",
    "nothumb_usage": "» ᴜsᴀɢᴇ: <code>/nothumb [on|off]</code> (ʜɪᴅᴇs ᴛʜᴇ ᴛʜᴜᴍʙɴᴀɪʟ ᴡʜᴇɴ ᴏɴ). ᴛʜᴜᴍʙɴᴀɪʟs ᴀʀᴇ ɴᴏᴡ <b>{0}</b>.",
    "playmode_usage": "ᴘʟᴀʏ ᴍᴏᴅᴇ: <b>{0}</b>\nᴜsᴀɢᴇ: <code>/playmode [admin|everyone]</code>",
    "playmode_set": "ᴘʟᴀʏ ᴍᴏᴅᴇ ɪs ɴᴏᴡ: <b>{0}</b>\n\n» ʙʏ {1}",
    "playmode_admins": "ᴀᴅᴍɪɴs ᴏɴʟʏ",
    "playmode_everyone": "ᴇᴠᴇʀʏᴏɴᴇ",
    "cplay_not_set": "» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ɪs ɴᴏᴛ sᴇᴛ. ᴜsᴇ <code>/channelplay [channel id|username]</code> ɪɴ ᴛʜᴇ ɢʀᴏᴜᴘ ғɪʀsᴛ.",
    "vc_busy_channel": "» ᴛʜᴇ ᴄʜᴀɴɴᴇʟ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪs ᴘʟᴀʏɪɴɢ ʀɪɢʜᴛ ɴᴏᴡ. sᴛᴏᴘ ɪᴛ ᴡɪᴛʜ /cstop ᴏʀ ᴜsᴇ ᴛʜᴇ ᴄ-ᴄᴏᴍᴍᴀɴᴅs.",
    "vc_busy_group": "» ᴛʜᴇ ɢʀᴏᴜᴘ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ ɪs ᴘʟᴀʏɪɴɢ ʀɪɢʜᴛ ɴᴏᴡ. sᴛᴏᴘ ɪᴛ ғɪʀsᴛ, ᴛʜᴇɴ ᴜsᴇ ᴛʜᴇ ᴄ-ᴄᴏᴍᴍᴀɴᴅs.",
    "cplay_status": "» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ɪs ʟɪɴᴋᴇᴅ ᴛᴏ <code>{0}</code>.",
    "cplay_usage": "» ᴜsᴀɢᴇ: <code>/channelplay [channel id|username|off]</code>\nᴛʜᴇɴ ᴜsᴇ /cplay, /cvplay, /cskip, /cstop... ᴛᴏ ᴘʟᴀʏ ɪɴ ᴛʜᴇ ᴄʜᴀɴɴᴇʟ's ᴠɪᴅᴇᴏ ᴄʜᴀᴛ.",
    "cplay_disabled": "» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ᴅɪsᴀʙʟᴇᴅ.",
    "cplay_invalid": "» ɪ ᴄᴏᴜʟᴅ ɴᴏᴛ ғɪɴᴅ ᴛʜᴀᴛ ᴄʜᴀɴɴᴇʟ.",
    "cplay_not_accessible": "» ɪ ᴄᴀɴ'ᴛ sᴇᴇ ᴛʜᴀᴛ ᴄʜᴀɴɴᴇʟ. ᴀᴅᴅ ᴍᴇ ᴀɴᴅ ᴛʜᴇ ᴀssɪsᴛᴀɴᴛ ᴛᴏ ɪᴛ ғɪʀsᴛ.",
    "cplay_not_channel_admin": "» ʏᴏᴜ ᴍᴜsᴛ ʙᴇ ᴀɴ ᴀᴅᴍɪɴ ᴏғ ᴛʜᴀᴛ ᴄʜᴀɴɴᴇʟ.",
    "cplay_enabled": "» ᴄʜᴀɴɴᴇʟ ᴘʟᴀʏ ᴇɴᴀʙʟᴇᴅ ғᴏʀ <code>{0}</code>. ᴜsᴇ /cplay, /cvplay, /cskip, /cstop...",
    "userbot_ready": "» ᴛʜᴇ ᴀssɪsᴛᴀɴᴛ ɪs ʀᴇᴀᴅʏ.",
    "userbot_present": "» {0} ɪs ᴀʟʀᴇᴀᴅʏ ɪɴ ᴛʜɪs ᴄʜᴀᴛ.",
    "userbot_joined": "» {0} ᴊᴏɪɴᴇᴅ ᴛʜᴇ ᴄʜᴀᴛ.",
    "maint": "» ᴛʜᴇ ʙᴏᴛ ɪs ᴜɴᴅᴇʀ ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ.{0}\nᴘʟᴇᴀsᴇ ᴛʀʏ ᴀɢᴀɪɴ ʟᴀᴛᴇʀ.",
    "maint_reason": "\nʀᴇᴀsᴏɴ: {0}",
    "maint_usage": "ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ ɪs <b>{0}</b> (ʀᴇᴀsᴏɴ: {1}).\nᴜsᴀɢᴇ: <code>/maintenance [on|off] [reason]</code>",
    "maint_disabled": "» ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ ᴍᴏᴅᴇ ᴅɪsᴀʙʟᴇᴅ.",
    "maint_enabled": "ᴍᴀɪɴᴛᴇɴᴀɴᴄᴇ ᴍᴏᴅᴇ ᴇɴᴀʙʟᴇᴅ. ʀᴇᴀsᴏɴ: {0}",
    "autoleave_usage": "ᴀᴜᴛᴏ ʟᴇᴀᴠᴇ ɪs <b>{0}</b>.\nᴜsᴀɢᴇ: <code>/autoleave [on|off]</code>",
    "autoleave_set": "» ᴀᴜᴛᴏ ʟᴇᴀᴠᴇ ɪs ɴᴏᴡ <b>{0}</b>.",
    "speedtest_running": "» ʀᴜɴɴɪɴɢ ᴛʜᴇ sᴘᴇᴇᴅ ᴛᴇsᴛ...",
    "speedtest_failed": "» ᴛʜᴇ sᴘᴇᴇᴅ ᴛᴇsᴛ ғᴀɪʟᴇᴅ.",
    "speedtest_result": "<b>sᴘᴇᴇᴅ ᴛᴇsᴛ</b>\n\nᴅᴏᴡɴʟᴏᴀᴅ: <b>{0}</b> ᴍʙᴘs\nᴜᴘʟᴏᴀᴅ: <b>{1}</b> ᴍʙᴘs\nᴘɪɴɢ: <b>{2}</b> ᴍs",
    "bug_usage": "ᴜsᴀɢᴇ: <code>/bug [describe the problem]</code>",
    "bug_report": "<b>ʙᴜɢ ʀᴇᴘᴏʀᴛ</b>\n\nғʀᴏᴍ: {0} (<code>{1}</code>)\nᴡʜᴇʀᴇ: {2}\n\n{3}",
    "bug_failed": "» ᴄᴏᴜʟᴅ ɴᴏᴛ sᴇɴᴅ ᴛʜᴇ ʀᴇᴘᴏʀᴛ. ᴛʀʏ ᴀɢᴀɪɴ ʟᴀᴛᴇʀ.",
    "bug_sent": "» ᴛʜᴀɴᴋs! ʏᴏᴜʀ ʀᴇᴘᴏʀᴛ ᴡᴀs sᴇɴᴛ.",
    "privacy_text": "<b>ᴘʀɪᴠᴀᴄʏ</b>\n\n{0} sᴛᴏʀᴇs ᴏɴʟʏ ᴡʜᴀᴛ ɪᴛ ɴᴇᴇᴅs ᴛᴏ ʀᴜɴ: ᴄʜᴀᴛ ɪᴅs, ᴜsᴇʀ ɪᴅs ᴏғ ᴀᴅᴍɪɴs/ᴀᴜᴛʜᴏʀɪᴢᴇᴅ ᴜsᴇʀs, ʟᴀɴɢᴜᴀɢᴇ ᴀɴᴅ ᴄʜᴀᴛ sᴇᴛᴛɪɴɢs. ᴍᴇssᴀɢᴇs ᴀʀᴇ ɴᴏᴛ sᴛᴏʀᴇᴅ. ᴅᴀᴛᴀ ɪs ɴᴇᴠᴇʀ sᴏʟᴅ ᴏʀ sʜᴀʀᴇᴅ.",
    "bl_empty": "» ᴛʜᴇ ʙʟᴀᴄᴋʟɪsᴛ ɪs ᴇᴍᴘᴛʏ.",
    "bl_list": "<b>ʙʟᴀᴄᴋʟɪsᴛᴇᴅ:</b>\n{0}",
    "rtmp_usage": "ᴜsᴀɢᴇ (ɪɴ ᴘʀɪᴠᴀᴛᴇ ᴄʜᴀᴛ):\n<code>/setrtmp [chat id] [rtmp://host/app] [stream key]</code>\n<code>/setrtmp [chat id] off</code>",
    "rtmp_not_admin": "» ʏᴏᴜ ᴍᴜsᴛ ʙᴇ ᴀɴ ᴀᴅᴍɪɴ ᴏғ ᴛʜᴀᴛ ᴄʜᴀᴛ.",
    "rtmp_invalid": "» ᴛʜᴇ ᴜʀʟ ᴍᴜsᴛ sᴛᴀʀᴛ ᴡɪᴛʜ <code>rtmp://</code> ᴏʀ <code>rtmps://</code>.",
    "rtmp_saved": "» ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ sᴀᴠᴇᴅ. ᴜsᴇ /stream [sᴏɴɢ/ᴜʀʟ] ɪɴ ᴛʜᴀᴛ ᴄʜᴀᴛ.",
    "rtmp_removed": "» ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ ʀᴇᴍᴏᴠᴇᴅ.",
    "rtmp_not_set": "» ɴᴏ ʀᴛᴍᴘ ᴛᴀʀɢᴇᴛ ɪs sᴇᴛ ғᴏʀ ᴛʜɪs ᴄʜᴀᴛ. ᴜsᴇ /setrtmp ɪɴ ᴍʏ ᴘʀɪᴠᴀᴛᴇ ᴄʜᴀᴛ.",
    "stream_usage": "ᴜsᴀɢᴇ: <code>/stream [name or url]</code>",
    "stream_running": "» ᴀ sᴛʀᴇᴀᴍ ɪs ᴀʟʀᴇᴀᴅʏ ʀᴜɴɴɪɴɢ ʜᴇʀᴇ. sᴛᴏᴘ ɪᴛ ᴡɪᴛʜ /streamstop.",
    "stream_failed": "» ᴄᴏᴜʟᴅ ɴᴏᴛ sᴛᴀʀᴛ ᴛʜᴇ sᴛʀᴇᴀᴍ.",
    "stream_started": "sᴛʀᴇᴀᴍɪɴɢ <b>{0}</b> ᴛᴏ ʀᴛᴍᴘ.\n\n» ʙʏ {1}",
    "stream_stopped": "sᴛʀᴇᴀᴍ sᴛᴏᴘᴘᴇᴅ.\n\n» ʙʏ {0}",
    "stream_none": "» ɴᴏ sᴛʀᴇᴀᴍ ɪs ʀᴜɴɴɪɴɢ ʜᴇʀᴇ.",
    "stream_status": "» sᴛʀᴇᴀᴍɪɴɢ <b>{0}</b> ғᴏʀ {1}.",
    "spotify_fetch": "» ʀᴇᴀᴅɪɴɢ ᴛʜᴇ sᴘᴏᴛɪғʏ ʟɪɴᴋ...",
    "spotify_error": "» ᴄᴏᴜʟᴅ ɴᴏᴛ ʀᴇᴀᴅ ᴛʜᴀᴛ sᴘᴏᴛɪғʏ ʟɪɴᴋ. ᴍᴀᴋᴇ sᴜʀᴇ ɪᴛ ɪs ᴀ ᴘᴜʙʟɪᴄ ᴛʀᴀᴄᴋ, ᴀʟʙᴜᴍ ᴏʀ ᴘʟᴀʏʟɪsᴛ.",
    "bass_usage": "ʙᴀss ʙᴏᴏsᴛ: <b>{0}</b>/100\nᴜsᴀɢᴇ: <code>/bass [0-100 | on | off]</code>",
    "bass_set": "ʙᴀss ʙᴏᴏsᴛ sᴇᴛ ᴛᴏ <b>{0}</b>.\n\n» ʙʏ {1}",
    "log_removed": "<u><b>ʀᴇᴍᴏᴠᴇᴅ ғʀᴏᴍ ᴄʜᴀᴛ</b></u>\n\n<b>ᴄʜᴀᴛ:</b> <code>{0}</code> | {1}\n<b>ʙʏ:</b> <code>{2}</code> | {3}\n<b>ᴀᴄᴛɪᴏɴ:</b> {4}",
    "log_blocked": "<u><b>ʙᴏᴛ ʙʟᴏᴄᴋᴇᴅ</b></u>\n\n<b>ᴜsᴇʀ:</b> <code>{0}</code> | {1} ʙʟᴏᴄᴋᴇᴅ ᴛʜᴇ ʙᴏᴛ.",
    "link_fetch": "» ʀᴇᴀᴅɪɴɢ ᴛʜᴇ {0} ʟɪɴᴋ...",
    "link_error": "» ᴄᴏᴜʟᴅ ɴᴏᴛ ʀᴇᴀᴅ ᴛʜᴀᴛ {0} ʟɪɴᴋ. ᴍᴀᴋᴇ sᴜʀᴇ ɪᴛ ɪs ᴘᴜʙʟɪᴄ ᴀɴᴅ ᴛʀʏ ᴀɢᴀɪɴ.",
    "pl_empty": "» ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ɪs ᴇᴍᴘᴛʏ. ᴀᴅᴅ sᴏɴɢs ᴡɪᴛʜ <code>/addplaylist [song name or link]</code>.",
    "pl_add_usage": "» ᴜsᴀɢᴇ: <code>/addplaylist [song name | YouTube / Spotify / Apple Music / JioSaavn / SoundCloud link]</code>\nᴡʜɪʟᴇ ᴀ sᴏɴɢ ɪs ᴘʟᴀʏɪɴɢ, <code>/addplaylist</code> ᴀʟᴏɴᴇ sᴀᴠᴇs ᴛʜᴀᴛ sᴏɴɢ.",
    "pl_adding": "» ᴀᴅᴅɪɴɢ ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ...",
    "pl_added": "» ᴀᴅᴅᴇᴅ <b>{0}</b> ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. ʏᴏᴜ ɴᴏᴡ ʜᴀᴠᴇ {1} sᴏɴɢs.",
    "pl_added_many": "» ᴀᴅᴅᴇᴅ <b>{0}</b> sᴏɴɢs ᴛᴏ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ({1} ᴡᴇʀᴇ ᴀʟʀᴇᴀᴅʏ ᴛʜᴇʀᴇ). ʏᴏᴜ ɴᴏᴡ ʜᴀᴠᴇ {2} sᴏɴɢs.",
    "pl_dup": "» ᴛʜᴀᴛ sᴏɴɢ ɪs ᴀʟʀᴇᴀᴅʏ ɪɴ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ.",
    "pl_full": "» ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ ɪs ғᴜʟʟ ({0} sᴏɴɢs). ʀᴇᴍᴏᴠᴇ sᴏᴍᴇ ᴡɪᴛʜ /rmplaylist.",
    "pl_unsupported": "» ᴛʜᴀᴛ ʟɪɴᴋ ᴛʏᴘᴇ ᴄᴀɴ'ᴛ ʙᴇ sᴀᴠᴇᴅ ᴛᴏ ᴀ ᴘʟᴀʏʟɪsᴛ. ᴜsᴇ ᴀ sᴏɴɢ ɴᴀᴍᴇ ᴏʀ ᴀ ʏᴏᴜᴛᴜʙᴇ, sᴘᴏᴛɪғʏ, ᴀᴘᴘʟᴇ ᴍᴜsɪᴄ, ᴊɪᴏsᴀᴀᴠɴ ᴏʀ sᴏᴜɴᴅᴄʟᴏᴜᴅ ʟɪɴᴋ.",
    "pl_header": "<b>{0}'s ᴘʟᴀʏʟɪsᴛ</b> — {1} sᴏɴɢs\n\n",
    "pl_more": "» \n…ᴀɴᴅ {0} ᴍᴏʀᴇ.",
    "pl_footer": "\n\n/playplaylist ᴛᴏ ᴘʟᴀʏ ɪᴛ (ᴀᴅᴅ <code>-s</code> ᴛᴏ sʜᴜғғʟᴇ, <code>-v</code> ғᴏʀ ᴠɪᴅᴇᴏ)\n/rmplaylist [ɴᴜᴍʙᴇʀ | ɴᴀᴍᴇ | ᴀʟʟ] ᴛᴏ ʀᴇᴍᴏᴠᴇ",
    "pl_rm_usage": "ᴜsᴀɢᴇ: <code>/rmplaylist [number | song name | all]</code>",
    "pl_rm_none": "» ɪ ᴄᴏᴜʟᴅɴ'ᴛ ғɪɴᴅ ᴛʜᴀᴛ sᴏɴɢ ɪɴ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. ᴄʜᴇᴄᴋ /playlist.",
    "pl_removed": "» ʀᴇᴍᴏᴠᴇᴅ <b>{0}</b> ғʀᴏᴍ ʏᴏᴜʀ ᴘʟᴀʏʟɪsᴛ. {1} sᴏɴɢs ʟᴇғᴛ.",
    "pl_cleared": "» ᴘʟᴀʏʟɪsᴛ ᴄʟᴇᴀʀᴇᴅ ({0} sᴏɴɢs ʀᴇᴍᴏᴠᴇᴅ)."
}
```


## Appendix B — VERBATIM: TinaMusic `locales/en.yml`

```yaml
name: "🇺🇸 English"

# buttons
CONFIRM_RESUME_BTN: "▶️ ꝛєsᴜᴍє"
CONFIRM_UNMUTE_BTN: "🔊 ᴜηᴍᴜᴛє"
CONFIRM_STOP_BTN: "⏹️ sᴛσᴘ"
ADD_ME_BTN: "⚡ ᴧᴅᴅ ᴍє ᴛσ ʏσυꝛ ɢꝛσυᴘ"
BROADCAST_CANCEL_BTN: "🚫 ᴄᴧηᴄєʟ ʙꝛσᴧᴅᴄᴧsᴛ"
CLOSE_BTN: "🗑 ᴄʟσsє"
DOWNLOAD_CANCEL_BTN: "🚦 ᴄᴧηᴄєʟ ᴅσᴡηʟσᴧᴅ"
GC_HELP_BTN: "📒 ᴄσϻϻᴧηᴅs"

START_HELP_BTN: "❓ ʜєʟᴘ & ᴄσϻϻᴧηᴅs"

UPDATES_BTN: "📢 ᴜᴘᴅᴧᴛєs"
SUPPORT_BTN: "💬 sᴜᴘᴘσꝛᴛ"

HELP_ADMINS_BTN: "🛠 ᴧᴅϻɪηs"
HELP_PUBLIC_BTN: "🌍 ᴘᴜʙʟɪᴄ"
HELP_OWNER_BTN: "👑 σᴡηєꝛ"
HELP_SUDOERS_BTN: "⚡ sᴜᴅσєꝛs"

BACK_BTN: "⬅️ ʙᴧᴄᴋ"

# basically this string used in /command [bool]
invalid_bool: "⚠️ <b>Invalid value.</b>\nUse 'enable' or 'disable'."

active_chats_info: "📊 <b>Active Chats:</b>\n\nThere are <i>{count}</i> active chats across the bot system."
active_chats_info_with_broken: "📊 <b>Active Chats:</b>\n\nThere are <i>{count}</i> active chats across the bot system, but <i>{broken}</i> of them may have issues displaying correctly."

adminmode_help: |
  🛡️ <b>Admin Command Mode</b>

  <b>Status:</b> {status}
  <b>Usage:</b> <code>/adminmode [admin|adminauth|everyone]</code>
adminmode_status_admin: "Admins 🔐"
adminmode_status_adminauth: "Auth Users 🎧"
adminmode_status_everyone: "Everyone 🌍"
adminmode_updated: "✅ <b>Admin command mode</b> updated to: {status}"
adminmode_invalid: "⚠️ Invalid mode. Use: <code>admin</code>, <code>adminauth</code>, or <code>everyone</code>."

# 🚫 Assistant Restricted / Banned
assistant_restricted_warning: |
  ⚠️ <b>Assistant Restricted</b>

  The assistant {assistant} (ID: <code>{id}</code>) has been <b>banned</b> or restricted in this chat.
  I cannot play music, manage the queue, or clean up tracks while this continues.

  <i>Unban the assistant to restore all music features 🎵</i>

# 🧩 Common auth messages
auth_no_user: "⚠️ Please provide a user — use:\n{cmd} [user_id] or reply to a user's message."
user_extract_fail: "⚠️ Unable to get that user: <i>{error}</i>"
auth_limit_reached: "⚠️ This chat has reached the maximum authorized users limit ({limit}). Please remove someone before adding a new one."
already_authed: "⚠️ That user is already authorized — no need to add them again."

# 🤖 Add auth
cannot_authorize_user: "⚠️ This user cannot be authorized.\nThe bot owner, the bot itself, or yourself do not require authorization."
addauth_user_is_admin: "⚠️ This user is already a chat administrator. No authorization is required."
addauth_bot_user: "🤖 You can’t add a bot to the auth list — authusers must be human!"
addauth_add_fail: "❌ Failed to add authorized user: {error}"
addauth_success_with_count: "✅ Added {user}.\nNow {count}/{limit} authorized users."
addauth_success: "✅ Successfully added {user} to the authorized users list."

# 🤖 Bot Added to Chat
bot_added_normal: "🎵 Thanks for adding me! Use /play [song] to start playing music."

# 🛠️ Bot Added During Maintenance
bot_added_maintenance: "⚠️ I'm currently under maintenance and will be back soon. Thanks for your patience! ⏰"
maint_reason_generic: "<i>📝 Reason: {reason}</i>"

# ⚠️ Bot Demotion
bot_demotion_goodbye: |
  <b>⚠️ Permission Lost</b>

  I’ve been <b>demoted from admin</b> and can’t perform my tasks anymore.
  Without proper rights, I’m unable to function here, so I’ll be <i>leaving the chat now</i>.

  <b>Goodbye 👋</b>

broadcast_already_running: "⚠️ A broadcast is already running.\n\nPlease wait for it to complete or cancel it first using:\n• <code>/broadcast -cancel</code>\n• Or click the Cancel button on the progress message"
broadcast_parse_failed: "❌ Failed to parse broadcast command:\n<code>{error}</code>"
broadcast_no_content: "⚠️ No content provided for broadcast.\n\n<b>Usage:</b> <code>{cmd} [text or reply to message]</code>\n\n<b>Example:</b>\n<code>{cmd} Hello everyone!</code>"
broadcast_fetch_chats_failed: "❌ Failed to fetch served chats:\n<code>{error}</code>"
broadcast_fetch_users_failed: "❌ Failed to fetch served users:\n<code>{error}</code>"
broadcast_no_targets: "⚠️ No targets found for broadcast.\n\nMake sure you haven't excluded all targets with flags."
broadcast_initializing: "📡 Initializing broadcast...\n\nPlease wait..."
broadcast_cancelled_header: "🚫 Broadcast Cancelled"
broadcast_completed_header: "✅ Broadcast Completed"
broadcast_progress_header: "📡 Broadcasting..."
broadcast_total_chats: "📊 Total Chats: {done}/{total} ({progress}%)"
broadcast_total_users: "👥 Total Users: {done}/{total} ({progress}%)"
broadcast_failed_chats: "❌ Failed Chats: {count}"
broadcast_failed_users: "❌ Failed Users: {count}"
broadcast_delay: "⏱ Delay: {delay}s"
broadcast_elapsed: "⏰ Elapsed: {elapsed}"
broadcast_eta: "⏳ ETA: {eta}"
broadcast_success_rate: "✨ Success Rate: {rate}% ({sent}/{total})"
broadcast_not_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_success: "🚫 Broadcast cancelled successfully."
broadcast_cancel_owner_only: "⚠️ Only the owner can cancel broadcasts."
broadcast_cancel_none_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_done: "🚫 Broadcast cancelled."

# Blocklist

blacklist_chat_blocked: "🚫 This chat is blocked from using this bot."
blacklist_owner_blocked_leave: "🚫 This chat owner is blocked from using this bot. Leaving chat."
blacklist_user_blocked: "🚫 You are blocked from using this bot."
blacklist_usage_blockchat: "Usage: <code>/blockchat [chat_id|@username]</code>"
blacklist_usage_unblockchat: "Usage: <code>/unblockchat [chat_id|@username]</code>"
blacklist_invalid_chat_identifier: "Invalid chat identifier: {error}"
blacklist_block_user_success: "✅ User blocked: <code>{id}</code>"
blacklist_unblock_user_success: "✅ User unblocked: <code>{id}</code>"
blacklist_block_chat_success: "✅ Chat blocked: <code>{id}</code>"
blacklist_unblock_chat_success: "✅ Chat unblocked: <code>{id}</code>"
blacklist_unblock_user_fail: "⚠️ Failed to unblock user: {error}"
blacklist_unblock_chat_fail: "⚠️ Failed to unblock chat: {error}"
blacklist_fetch_chats_fail: "⚠️ Failed to fetch blocked chats: {error}"
blacklist_fetch_users_fail: "⚠️ Failed to fetch blocked users: {error}"
blacklist_list_title: "📛 <b>Blacklisted</b>"
blacklist_list_chats: "💬 <b>Chats:</b>"
blacklist_list_users: "👤 <b>Users:</b>"

# 🎧 CPlay (Channel Play)

cplay_id_not_set: "⚠️ cplay ID not set.\nUse <code>/setcplay &lt;chat_id&gt;</code> to set it."
cplay_usage: "<b>Usage:</b> <code>/setcplay &lt;channel_id&gt;</code>"
cplay_invalid_chat_id: "<b>Invalid Chat ID:</b> Please provide a valid integer ID for the channel."
cplay_resolve_peer_fail: "<b>Failed to resolve peer:</b> Could not fetch channel details. Ensure I can access this channel."
cplay_invalid_target: "<b>Invalid Target:</b> The provided ID is not a valid channel."
cplay_channel_not_accessible: "<b>Channel Not Accessible:</b> Could not retrieve channel information. Please ensure I am an administrator in the target channel."
cplay_bot_invite_permission_missing: "⚠️ <b>Missing Permission:</b> I am admin in target channel but <code>Invite Users</code> permission is disabled. Please enable it and try again."
cplay_channel_not_linked: "⚠️ <b>Linked channel not found:</b> This chat is not linked to any channel/group."
cplay_save_error: "<b>Error:</b> Failed to save CPlay settings."
cplay_enabled: "✅ <b>Channel Play enabled.</b> All channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) will now work in channel <code>{channel_id}</code>."
cplay_owner_required: "⚠️ Only the owner of this chat or the target channel can use this setup."
cplay_owner_check_failed: "⚠️ Could not verify chat ownership right now. Please try again."
cplay_disabled: "✅ <b>Channel Play disabled.</b> Channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) are now turned off for this chat."

# Callback handler messages
invalid_request: "⚠️ Invalid request."
unknown_action: "⚠️ Unknown action."
download_cancelled: "✅ Download cancelled."
no_download_to_cancel: "ℹ️ No download to cancel."

# Pause callback
cb_pause_success: "⏸️ Track paused at {position}"
cb_pause_message: |
  <b>⏸️ Track Paused</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Position:</b> {position} / {duration}
  <b>▫ Paused by:</b> {user}

# Resume callback
cb_already_playing: "ℹ️ Track is already playing."
cb_resume_failed: "❌ Failed to resume playback."
cb_resume_success: "▶️ Resumed at {position}."
cb_resume_message: |
  <b>🎵 Now Playing:</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Resumed by:</b> {user}

# Replay callback
cb_replaying: "🔁 <b>Replaying current track...</b>"
cb_replay_failed: "❌ Failed to replay track."
cb_replay_success: "🔁 Track replayed."
cb_replay_edited: "🔁 Track replayed by {user}."

# Skip callback
cb_skip_queue_empty: "⏹️ Playback stopped — queue empty."
cb_skip_download_failed: "❌ Failed to download next track."
cb_skip_play_failed: "❌ Failed to play next track."
cb_skip_success: "⏭️ Track skipped."
cb_skip_edited: "⏭️ Skipped by {user}"

# Stop callback
cb_stop_success: "⏹️ Playback stopped."

# Mute/Unmute callback
cb_mute_success: "🔇 Muted."
cb_mute_message: |
  <b>🔇 Playback Muted</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Muted by:</b> {user}

cb_unmute_success: "🔊 Unmuted."

cb_seek_success: "⏩ Sought forward {seconds} seconds"
cb_seek_near_end: "⚠️ Cannot seek forward {seconds} seconds — about to reach end."
cb_seek_edited: "⏩ Sought {seconds} seconds — by {user}"

cb_seekback_success: "⏪ Sought back {seconds} seconds"
cb_seekback_edited: "⏪ Sought back {seconds} seconds — by {user}"

# 🗑️ Delete auth
del_auth_not_authorized: "⚠️ That user isn’t authorized — nothing to remove."
del_auth_remove_fail: "❌ Failed to remove authorized user: <code>{error}</code>"
del_auth_success: "✅ Successfully removed {user} from the authorized users list."

# 📜 Auth list
authlist_fetch_fail: "❌ Failed to get authorized users list: <code>{error}</code>"
authlist_empty: "ℹ️ There are no authorized users in this chat."
authlist_fetching: "⏳ Fetching authorized users list..."
authlist_header: "<b>Authorized Users:</b>"
authlist_user_fail: "{index}. <code>{user_id}</code> (Could not fetch user info)"
authlist_user_entry: "{index}. {user} (<code>{user_id}</code>)"
authlist_total: "<b>Total:</b> {count} users"

autoleave_fetch_fail: "⚠️ <b>Failed to fetch AutoLeave state.</b>"
autoleave_status: "🏃‍♂️ <b>AutoLeave Control</b>\n\nWhen enabled, the assistant will automatically leave the chat.\nUsage: {cmd} [enable|disable]\nCurrent state: {action}"
autoleave_already: "⚠️ AutoLeave is already {action}"
autoleave_update_fail: "⚠️ <b>Failed to update AutoLeave state.</b>"
autoleave_updated: "🏃‍♂️ AutoLeave has been {action}"

enabled: "Enabled"
disabled: "Disabled"

bug_usage: "⚠️ Please describe the bug. Usage: {cmd} <problem> or reply to a message."
bug_thanks: "✅ Thank you for your bug report! It has been sent to the developers."

bug_report_format: |
  <b>🐞 New Bug Report</b>

  <b>From:</b> {user} (<code>{user_id}</code>)
  <b>Chat:</b> {chat} (<code>{chat_id}</code>)

  <b>Report:</b>
  <pre>{report}</pre>
  
clear_no_active: "<blockquote>⚠️ <b><u>ɴσ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ</u></b>\n\n<i>ησ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ σꝛ ǫᴜєᴜє ᴛσ ᴄʟєᴧꝛ.</i></blockquote>"
clear_success: "✅ The queue has been cleared by {user}.\n<i>You can restore it using <code>/{cmd}</code> before adding new songs</i>"

err_assistant_banned: |
  <b>🚫 Assistant Restricted</b>

  I can't play music because {user} (UserID: <code>{id}</code>) is <b>banned</b> or <b>removed</b> from this chat.

  <i>✅ Unbanned them already? Use /reload to refresh.</i>

err_admin_permission_required: |
  ⚠️ <b>Admin Permission Required</b>

  I need admin access with Invite Users permission to manage and invite assistant in this chat.

err_assistant_get_failed: |
  ⚠️ The system could not assign an assistant to this chat.

  <i>This is usually a temporary issue and should resolve by trying again.</i>

  <b>➡️ Please retry your command.</b>

  <i>If this keeps happening, please contact support so we can assist you.</i>
  
err_assistant_join_request_sent: |
  ⚠️ Assistant sent a join request, but I could not auto-approve it.

  <i>✅ Please approve the request manually, then try again.</i>

err_assistant_invite_link_fetch: |
  ⚠️ Failed to fetch invite link:
  <i>{error}</i>

err_assistant_invite_failed: |
  ⚠️ Assistant failed to join this chat:
  <i>{error}</i>

err_connection_timeout: |
  ⚠️ <b>VoiceChat Connection Timeout</b>

  Telegram is not responding properly to the voice chat connection right now.

  <i>If this keeps happening, end the voice chat and start it again, then retry.</i>

err_no_active_voicechat: |
  🎙️ <b>No Active Voice Chat</b>

  I can't join — please start a voice chat first.

  <i>Already started one? Use /reload to refresh.</i>

err_fetch_failed: |
  ⚠️ Failed to fetch chat info:
  <i>{error}</i>

err_rtmp_missing_params: |
  <b>⚠️ RTMP Details Missing</b>

  <i>It looks like an <b>RTMP stream</b> is active in this chat, but you have not provided the required
  <b>RTMP URL</b> or <b>Stream Key</b>. Without these, I cannot start streaming music to RTMP.</i>

  <b>Use:</b> <code>/setrtmp [chat_id] [url+key]</code>

  <i>📌 Once the RTMP details are set, you may start streaming.</i>  
  <i>➡️ If you do not wish to use RTMP, switch to regular voice chat instead.</i>
  
err_unknown: |
  ⚠️ Unknown Error Occurred:
  <i>{error}</i>

err_fetch_tracks: |
  ⚠️ <b>Failed to fetch track details.</b>
  Please try again later.

err_download_internal: |
  ⚠️ <b>Download failed</b> due to an unexpected internal error.
  Please try again later.
  
flood_seconds: "⏳ Slow down! Try again in {duration} seconds."
flood_minutes: "⏳ Please wait {duration} minutes before using this command again."

help_private_only: "🤖 Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!"
help_main: |
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">📚</a> <b><u>ʜєʟᴘ &amp; ᴄσϻϻᴧηᴅs ᴍєηᴜ</u></b>

  ✨ <i>ᴄʟɪᴄᴋ ᴛʜє ᴄᴧᴛєɢσꝛʏ ʙᴜᴛᴛσηs ʙєʟσᴡ ᴛσ єxᴘʟσꝛє ᴧʟʟ ᴧᴠᴧɪʟᴧʙʟє ᴄσϻϻᴧηᴅs:</i>

  • 🛠 <b>ᴧᴅϻɪηs</b> ➜ <i>ᴍᴜsɪᴄ &amp; ᴘʟᴧʏʙᴧᴄᴋ ᴄσηᴛꝛσʟs</i>
  • 🌍 <b>ᴘᴜʙʟɪᴄ</b> ➜ <i>ɢєηєꝛᴧʟ &amp; ᴜsєꝛ ᴄσϻϻᴧηᴅs</i>
  • 👑 <b>σᴡηєꝛ</b> ➜ <i>ʙσᴛ ᴍᴧηᴧɢєᴍєηᴛ &amp; sʏsᴛєᴍ</i>
  • ⚡ <b>sᴜᴅσєꝛs</b> ➜ <i>ᴧᴅᴠᴧηᴄєᴅ ʙσᴛ ᴄσηᴛꝛσʟs</i>

  💡 <b>ᴛɪᴘ:</b> <tg-spoiler>ᴜsᴇ /play &lt;song&gt; ᴛᴏ sᴛᴀʀᴛ ɪɴ ᴠᴄ</tg-spoiler></blockquote>

jump_no_active: "⚠️ No active track to jump within."
jump_usage: "⚠️ Please provide seconds.\nExample: <code>{cmd} 120</code>"
jump_invalid_position: "⚠️ Invalid position.\nExample: <code>{cmd} 120</code>"
jump_too_close_end: "⚠️ Cannot jump to <b>{position}</b> — that’s too close to the end of the track."
jump_failed: "❌ Failed to jump to <b>{position}</b>.\nError: <code>{error}</code>"
jump_success: "⏩ Jumped to <u>{position}</u> of <u>{duration}</u>."

lang_same: "ℹ️ You’re already using this language."
lang_select: "🌐 Select your preferred language:"
lang_fail: "❌ Failed to update language. Please try again later."
lang_success: "✅ Language successfully updated to {lang_name}."

logger_playback_started: "Playback Started"
logger_playback_queued: "Playback Queued"

logger_bot_added: |
  🤖 <b>Bot Added to New Group</b>
  
  <blockquote>
  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Added By:</b> {added_by_name}
  <b>User ID:</b> {added_by_id}
  <b>User Username:</b> {added_by_username}

  <b>Time:</b> {date_time}
  </blockquote>
#  👥 <b>Members Count:</b> {members_count}

logger_bot_removed: |

  🤖 <b>Bot Removed from Group</b>
  <blockquote>
  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Removed By:</b> {removed_by_name}
  <b>User ID:</b> {removed_by_id}
  <b>User Username:</b> {removed_by_username}

  <b>Time:</b> {date_time}
  </blockquote>
  
logger_bot_started: |
  ▶️ <b>{mention} started the bot</b>

  <b>Username:</b> {user_username}
  <b>ID:</b> {user_id}
  

logger_playback_template: "<b>🎧 Track:</b> <a href=\"{track_url}\">{track}</a>\n<b>🔗 Source:</b> {source}\n<b>📌 Group:</b> {group} ({group_id})\n<b>👤 Requested by:</b> {requested_by} (<code>{requested_by_id}</code>)"

loop_usage: "🔁 <b>Loop Control</b>\n\nUsage: {cmd} [count]\n• 0 - Disable loop\n• 1-10 - Loop count\n\n<i>{count_line}</i>"
loop_invalid: "⚠️ <b>Invalid loop count.</b>\nUse 0 to disable or 1-10 to set loop count."
loop_already_set: "⚠️ Loop count is already set to <b>{count}</b> time(s)."
loop_disabled: "🔁 Loop has been <b>disabled</b> by {user}"
loop_set: "🔁 Set to loop <b>{count}</b> time(s)\n└ Changed by: {user}"
loop_current: "• Current loop: <b>{count}</b> time(s)"

logger_usage: "⚙️ Usage: <code>{cmd} [enable|disable]</code> - To enable or disable the logger\n\n{status}"
logger_status: "📜 Current status: {action}"
logger_check_fail: "❌ Failed to check logger status: {error}"
logger_already: "ℹ️ Logger is already {action}."
logger_update_fail: "❌ Failed to update logger setting: {error}"
logger_updated: "✅ Logger has been {action} successfully."

# Logs module
logs_empty: "No logs found"

# reason - if maintenance reason available so at the place of {reason} the maint_reason will provided
maint: "⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later.\n\n{reason}"
maint_reason: "<i>📝 Reason: {reason}</i>"

# ⚙️ Maintenance control
maint_usage: "⚙️ <b>Maintenance Control</b>\n\nUsage: {cmd} [enable|disable] [reason]\n📜 Current status: {status}"
maint_check_fail: "❌ Failed to check maintenance status: <code>{error}</code>"
maint_already_enabled: "ℹ️ Maintenance mode is already enabled 🟢."
maint_already_disabled: "ℹ️ Maintenance mode is already disabled 🔴."
maint_already_reason_same: "ℹ️ Maintenance is already enabled with the same reason."
maint_reason_removed: "✅ Maintenance reason removed successfully."
maint_reason_updated: "✅ Maintenance reason updated.\n📝 Reason: {reason}"
maint_enabled: "✅ Maintenance mode has been <b>enabled</b> successfully."
maint_enabled_reason: "✅ Maintenance mode enabled.\n📝 Reason: {reason}"
maint_disabled: "✅ Maintenance mode has been <b>disabled</b> successfully."
maint_entering: "⚠️ Bot is entering maintenance mode..."
enabled_with_reason: "🟢 Enabled\n📝 Reason: {reason}"

move_usage: "⚠️ Usage: <code>{cmd} [from] [to]</code>"
move_invalid_numbers: "⚠️ Invalid numbers. Example: <code>/{cmd} 2 1</code>"
move_invalid_indexes_min: "⚠️ Indexes must be greater than 0."
move_invalid_indexes_max: "⚠️ Queue has only {queue_len} tracks."
move_success: "✅ Moved track from position {from} to {to} by {user}."

# 🔇 Mute messages
mute_already_muted: "🔇 <b>Already Muted</b>\nThe music is already muted in this chat.\nWould you like to unmute it?"
mute_already_muted_with_time: "🔇 <b>Already Muted</b>\nThe music is muted.\nAuto-unmute in <b>{duration}</b>."
mute_invalid_duration: "⚠️ Invalid duration for auto-unmute.\nIt must be between <b>5</b> and <b>3600</b> seconds."
mute_invalid_format: "⚠️ Invalid format.\nUse: <code>/{cmd} 30</code> or <code>/{cmd} 30s</code>"
mute_failed: "❌ <b>Playback Mute Failed</b>\nError: <code>{error}</code>"
mute_success: |
  <blockquote>🔇 <b><u>ᴍᴜᴛєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <b>ᴛꝛᴧᴄᴋ:</b> {title}
  • <b>ᴍᴜᴛєᴅ ʙʏ:</b> {user}</blockquote>

mute_success_with_time: |
  <blockquote>🔇 <b><u>ᴍᴜᴛєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <b>ᴛꝛᴧᴄᴋ:</b> {title}
  • <b>ᴍᴜᴛєᴅ ʙʏ:</b> {user}
  • <i>⏳ ᴧᴜᴛσ-ᴜηᴍᴜᴛє ɪη <b>{duration}</b> sєᴄσηᴅs</i></blockquote>

no_song_query: |
  <blockquote>🎵 <b><u>ɴσ sσηɢ ǫᴜєꝛʏ ᴅєᴛєᴄᴛєᴅ</u></b>

  • <i>ᴘʟєᴧsє ᴘꝛσᴠɪᴅє ᴧ sσηɢ ηᴧᴍє σꝛ ꝛєᴘʟʏ ᴛσ ᴧη ᴧᴜᴅɪσ/ᴠɪᴅєσ.</i>
  • <b>ᴜsᴧɢє:</b> <code>{cmd} &lt;song name&gt;</code>
  • <b>єxᴧᴍᴘʟє:</b> <code>{cmd} closer</code>

  💡 <b>ᴛɪᴘ:</b> <tg-spoiler>ʏᴏᴜ ᴄᴀɴ ᴀʟsᴏ sᴇɴᴅ ʏᴏᴜᴛᴜʙᴇ/sᴘᴏᴛɪꜰʏ ʟɪɴᴋs</tg-spoiler></blockquote>

no_song_found: "<blockquote>❌ <b><u>ɴσ ᴛꝛᴧᴄᴋs ғσυηᴅ</u></b>\n\n<i>ᴄσυʟᴅ ησᴛ ғɪηᴅ ᴧηʏ ᴍᴧᴛᴄʜɪηɢ ꝛєsᴜʟᴛs. ᴘʟєᴧsє ᴛꝛʏ ᴧησᴛʜєꝛ ǫᴜєꝛʏ!</i></blockquote>"

nothumb_status: |
  🖼️ <b>Thumbnail Control</b>
  
  <i>When enabled, playback messages won't show artwork/thumbnails.</i>
  
  <b>📊 Usage: {cmd} [enable|disable]</b>
  
  <i>Thumbnail is {action} currently.</i>
  
nothumb_fetch_fail: "⚠️ <b>Failed to fetch thumbnail setting.</b>"
nothumb_already: "ℹ️ Thumbnail display is already {action}."
nothumb_update_fail: "⚠️ <b>Failed to update thumbnail setting.</b>"
nothumb_updated: "🖼️ Thumbnail display has been {action}."

cmddelete_status: "🗑️ <b>Command Delete Control</b>\n\nWhen enabled, I will automatically delete command messages.\nUsage: {cmd} [enable|disable]\nCurrent state: {action}"
cmddelete_updated: "🗑️ Command deletion has been {action}."

only_admin: "⚠️ <b>Permission Denied!</b>\nOnly <b>admins</b> can control these actions. You can still play songs 🎵.\n\nIf you believe you are an admin, please use /reload to refresh your admin status."
only_admin_cb: "⚠️ Only admins can do these actions."
only_admin_or_auth: "⚠️ <b>Access Denied</b>\nOnly <b>admins</b> or <b>authorized users</b> can control these actions.\n\nIf you recently became an admin, use /reload to refresh your permissions."
only_admin_or_auth_cb: "⚠️ Only admins or authorized users can do these actions."
only_supergroup: "⚠️ This command can only be used in groups."
only_owner: "⚠️ Only the bot owner can use this command."
only_sudo: "⚠️ Only sudo users or the bot owner can use this command."

pause_invalid_duration: "⚠️ Invalid duration for auto-resume.\nIt must be between <b>5</b> and <b>3600</b> seconds."
pause_invalid_format: "⚠️ Invalid format.\nUse: <code>/{cmd} 30</code> or <code>/{cmd} 30s</code>"
auto_resume_line: "\n<i>⏳ Auto-resume in <b>{seconds}</b> seconds</i>"

pause_already: |
  ⏸️ <b>Already Paused</b>
  The music is already paused.{auto_resume_line}

pause_success: |
  <blockquote>⏸️ <b><u>ᴘᴧᴜsєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <b>ᴛꝛᴧᴄᴋ:</b> {title}
  • <b>ᴘσsɪᴛɪση:</b> <code>{position} / {duration}</code>
  • <b>ᴘᴧᴜsєᴅ ʙʏ:</b> {user}{auto_resume_line}</blockquote>

play_single_track_too_long: |
  ⚠️ You can't play songs longer than {limit_mins} minutes.
  <i>{title}</i> was skipped.

play_download_failed: |
  ❌ Failed to download "<i>{title}</i>".
  Error: <code>{error}</code>

play_added_to_queue_single: |
  <blockquote>{thumb} <b><u>ᴧᴅᴅєᴅ ᴛσ ǫᴜєᴜє</u>:</b> #{index}</blockquote>
  <blockquote>• <b>ᴛꝛᴧᴄᴋ:</b> <a href="{url}">{title}</a>
  • <b>ᴅᴜꝛᴧᴛɪση:</b> <code>{duration}</code>
  • <b>ꝛєǫᴜєsᴛєᴅ ʙʏ:</b> {by}</blockquote>

play_multiple_tracks_too_long_header: "<b>⚠️ {count} tracks were skipped (max duration {limit_mins} mins):</b>"
play_multiple_tracks_too_long_item: "— <i>{title}</i>"
play_multiple_tracks_too_long_more: "... and {remaining} more."
play_all_tracks_skipped: "❌ All found tracks were skipped due to duration limits."
play_failed: "❌ Failed to play\nError: {error}"
play_unable: "<blockquote>❌ <b><u>ᴜηᴧʙʟє ᴛσ ᴘʟᴧʏ</u></b>\n\n<i>ᴘʟєᴧsє ᴍᴧᴋє sᴜꝛє ᴠσɪᴄє ᴄʜᴧᴛ ɪs ᴧᴄᴛɪᴠє σꝛ ꝛєsᴛᴧꝛᴛ ᴛʜє ᴠᴄ.</i></blockquote>"
play_downloading_song: "📥 Downloading song \"{title}\""
play_download_canceled: "⚠️ Download canceled by {user}."
play_already_in_queue: |
  <blockquote>ℹ️ <b><u>ᴧʟꝛєᴧᴅʏ ɪη ǫᴜєᴜє</u></b>

  <i><b>{title}</b> ɪs ᴧʟꝛєᴧᴅʏ ᴘʟᴧʏɪηɢ σꝛ ɪη ᴛʜє ǫᴜєᴜє.</i></blockquote>
play_added_multiple_header: "➕ <b>Added {count} tracks</b> by {user}"
play_queue_limit_hint: "⚠️ <i>Queue limit reached — some tracks were skipped.</i>"
play_queue_view_hint: "<i>Use /queue to view full list.</i>"

playmode_help: |
  🛠️ <b>Play Mode</b>

  <b>Status:</b> {status}
  <b>Usage:</b> <code>/playmode [enable|disable]</code>

playmode_status_admins: "Admins 🔐"
playmode_status_everyone: "Everyone 🌍"
playmode_success_admins: "🛡️ <b>Play mode</b> is now restricted to <b>Admins & DJs</b> only. 🎧"
playmode_success_everyone: "✅ <b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow! 🎶"
playmode_restricted: "🚫 Play mode is enabled. Only admins and authorized users can play."

# 🏓 Ping messages
ping_start: "<blockquote>🏓 <b><u>ᴘɪηɢɪηɢ...</u></b>\n\n<i>ᴄʜєᴄᴋɪηɢ ʟᴧᴛєηᴄʏ &amp; sʏsᴛєᴍ sᴛᴧᴛs</i></blockquote>"
ping_result: |
  <blockquote><a href="https://yukiapi.site/file/ICyh3CVm">🏓</a> <b><u>ᴘσηɢ!</u></b> ➜ <code>{latency}ms</code></blockquote>
  <blockquote>📊 <b><u>sʏsᴛєᴍ sᴛᴧᴛs</u>:</b>

  • ⏳ <b>ᴜᴘᴛɪᴍє:</b> <code>{uptime}</code>
  • 🧠 <b>ꝛᴧϻ ᴜsᴧɢє:</b> <code>{ram_info}</code>
  • ⚡ <b>ᴄᴘᴜ ᴜsᴧɢє:</b> <code>{cpu_usage}</code>
  • 💾 <b>ᴅɪsᴋ ᴜsᴧɢє:</b> <code>{disk_usage}</code>
  • 🤖 <b>ʙσᴛ:</b> {bot}

  💡 <b>sᴛᴧᴛᴜs:</b> <tg-spoiler>🟢 ᴀʟʟ sʏsᴛᴇᴍs ᴏᴘᴇʀᴀᴛɪᴏɴᴀʟ</tg-spoiler>
  🚀 <b>ᴘɪηɢ:</b> <tg-spoiler>⚡ {latency}ms (ᴜʟᴛʀᴀ-ꜰᴀsᴛ)</tg-spoiler></blockquote>
  

position_now: |
  🎵 <b>{title}</b>
  📍 <code>{position} / {duration}</code>
  ⚙️ Speed: <b>{speed}x</b>

queue_limit_reached: "⚠️ Queue limit reached ({limit} tracks max). Use /clear to clear queue."
queue_no_active: |
  <blockquote>⚠️ <b><u>ɴσ ᴧᴄᴛɪᴠє ǫᴜєᴜє</u></b>

  • <i>ησᴛʜɪηɢ ɪs ǫᴜєᴜєᴅ ꝛɪɢʜᴛ ησᴡ.</i>
  • <b>ᴜsᴧɢє:</b> <code>/play &lt;song name&gt;</code></blockquote>
queue_header: "🎶 <b>Current Queue</b>"
queue_now_playing: "▶️ <b>Now Playing:</b>"
queue_up_next: "⏭️ <b>Up Next:</b>"
queue_more_line: "\n… and {remaining} more"
queue_empty_tail: "📭 <i>No more songs in queue.</i>"
queue_empty: "⚠️ <b>The queue is already empty.</b>"
queue_restored: "✅ Successfully restored <b>{count}</b> tracks to the queue."
queue_restore_no_data: "❌ No cleared queue found to restore, or a new song has already been added."

# 🔄 Reload
reload_start: "⚙️ Reloading admin cache, voice chat status, and assistant status..."

reload_done: "<b>⚙️ Reload complete:</b>\n\n{summary}"

reload_admin_cache_ok: "<b>• Admin cache:</b> <i>✅ Done</i>"
reload_admin_cache_fail: "<b>• Admin cache:</b> <i>❌ ({error})</i>"

reload_voice_inactive: "<b>• Voice chat:</b> <i>⚪ Inactive</i>"
reload_voice_admin_required: "<b>• Voice chat:</b> <i>❌ Admin permission required</i>"
reload_voice_fail: "<b>• Voice chat:</b> <i>❌ ({error})</i>"
reload_voice_active: "<b>• Voice chat:</b> <i>✅ Active</i>"

reload_assistant_banned: "<b>• Assistant:</b> <i>❌ Banned</i>"
reload_assistant_admin_required: "<b>• Assistant:</b> <i>❌ Admin permission required</i>"
reload_assistant_fail: "<b>• Assistant:</b> <i>❌ ({error})</i>"
reload_assistant_present: "<b>• Assistant:</b> <i>✅ Present</i>"
reload_assistant_not_present: "<b>• Assistant:</b> <i>⚪ Not present</i>"
reload_room_reset: "<b>• Room:</b> <i>Reset ✅</i>"


remove_usage: "⚠️ <b>Invalid usage.</b>\nUsage: <code>{cmd} [index]</code>"
remove_invalid_index: "⚠️ <b>Invalid index.</b> It must be a <b>number</b>."
remove_index_too_small: "⚠️ <b>Index must be greater than 0.</b>"
remove_index_too_big: "⚠️ Invalid index. Queue only has <b>{total}</b> tracks."
remove_success: "✅ Removed track at position <b>{index}</b> from the queue.\n👤 By: {user}"

# 🔁 Replay
replay_failed: "❌ <b>Replay Failed</b>\nError: <code>{error}</code>"
replay_success: |
  🔁 <b>Now replaying</b>

  🎵 Title: {title}
  ⏱️ Duration: <code>{duration}</code>
  ⚙️ Speed: <b>{speed}x</b>

# ▶️ Resume
resume_already_playing: "ℹ️ <b>Already Playing</b>\nThe music is already playing in this chat.\nWould you like to pause it?"
resume_failed: "❌ <b>Playback Resume Failed</b>\nError: <code>{error}</code>"
resume_success: |
  <blockquote>▶️ <b><u>ꝛєsᴜᴍєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <b>ᴛꝛᴧᴄᴋ:</b> {title}
  • <b>ᴘσsɪᴛɪση:</b> <code>{position} / {duration}</code>
  • <b>ꝛєsᴜᴍєᴅ ʙʏ:</b> {user}
  {speed_line}</blockquote>

restart: "♻️ Restarting..."
restart_exepath_fail: "❌ Failed to get executable path: {error}"
restart_symlink_fail: "❌ Failed to resolve executable path: {error}"
restart_service: "{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds."
restart_initiated: "✅ Restart initiated successfully!"
restart_fail: "❌ Failed to restart: {error}"
restart_confirm_running: "⚠️ A song is currently playing in this chat.\nChoose an action below."
restart_btn_bot: "🔄 ꝛєsᴛᴧꝛᴛ ʙσᴛ"
restart_btn_replay: "🔁 ꝛєᴘʟᴧʏ sσηɢ"
restart_confirm_bot: "♻️ Restarting the bot..."
restart_confirm_replay_done: "⏮️ Current song replayed successfully."


room_not_active_cb: "⚠️ Nothing playing right now."
room_no_active: |
  <blockquote>⚠️ <b><u>ɴσ ᴧᴄᴛɪᴠє ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <i>ᴛʜєꝛє's ησᴛʜɪηɢ ᴘʟᴧʏɪηɢ ꝛɪɢʜᴛ ησᴡ.</i>
  • <b>ᴜsᴧɢє:</b> <code>/play &lt;song name&gt;</code></blockquote>
room_not_linked: "⚠️ This channel isn't linked to any group."

room_already_paused: "⏸️ Track is already paused. Tap ▶️ Resume to continue."
room_already_paused_auto: "⏸️ Track is already paused — auto-resuming in {duration}."
room_pause_failed: "❌ Failed to pause playback.\n\n<i>{error}</i>"

rtmp_not_configured: "⚠️ RTMP not configured. Use <code>/setrtmp &lt;chat_id&gt; &lt;rtmp_url&gt;</code> in DM."

rtmp_already_streaming: |
  📡 <b>Already streaming!</b>
  
  Use <code>/streamstop</code> to end current stream.



rtmp_play_failed: |
  ❌ <b>Failed to start stream:</b>
  <code>{error}</code>

rtmp_now_streaming: |
  📡 <b>Now Streaming (RTMP):</b>
  
  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Started by:</b> {by}
  
  ⚠️ <i>Note: RTMP has ~15-30s buffering delay</i>


rtmp_stop_failed: |
  ❌ <b>Failed to stop stream:</b>
  <code>{error}</code>

rtmp_stopped: |
  ⏹️ <b>RTMP stream stopped</b> by {user}


rtmp_status_playing: |
  📡 <b>RTMP Status</b>
  
  <b>▫ State:</b> ▶️ Streaming
  <b>▫ Position:</b> {position}




rtmp_dm_only: "⚠️ Use this command in DM."

rtmp_setup_usage: "⚙️ /setrtmp <chat_id> <rtmp_url>"

generic_error: "❌ Something went wrong: <code>{error}</code>"

rtmp_invalid_chat_id: "❌ Invalid chat ID. Example: <code>-1001234567890</code>"



rtmp_parse_failed: "❌ Invalid RTMP URL. Use: <code>/setrtmp <chat_id> <rtmp_url></code>"

rtmp_streaming_not_supported: |
  ⚠️ <b>Streaming not supported</b>
  
  This action is not available while using <b>RTMP mode</b>.
  
  Use <code>/stream</code> to play media via RTMP.
  
rtmp_configured_success: "✅ RTMP configured successfully for Chat ID: <code>{chat_id}</code>"


searching: "🔍🎶 Searching... ⚡✨"
searching_query: "🔍🎶 Searching for: <b>{query}</b>... ⚡✨"


settings_btn_adminmode: "🛡️ ᴧᴅϻɪη ᴍσᴅє"
settings_btn_cmddelete: "🗑️ ᴄϻᴅ ᴅєʟєᴛє"
settings_btn_lang: "🌐 ʟᴧηɢᴜᴧɢє"
settings_btn_nothumb: "🖼️ ᴛʜᴜϻʙηᴧɪʟs"
settings_btn_playmode: "🎵 ᴘʟᴧʏ ᴍσᴅє"

settings_info_adminmode: |
  🛡️ What is this?

  Controls who can use admin commands of the bot (skip, pause, etc.).
  
settings_info_cmddelete: |
  🗑️ What is this?

  When enabled, the bot will automatically delete command messages after processing them to keep the chat clean.

settings_btn_cleanduration: "⏱️ ᴄʟєᴧη ᴅєʟᴧʏ"
settings_btn_cleanmode: "🧹 ᴄʟєᴧη ᴍσᴅє"
settings_info_cleanduration: |
  Cycles auto-delete delay: 15m → 30m → 60m → 5m.

settings_info_cleanmode: |
  Automatically removes command and bot service messages after a short delay.
  Playing status messages are kept until they naturally expire.
cleanmode_status: "🧹 <b>Clean Mode</b>\n\nAuto-cleanup of command and service messages.\nCurrent state: {action}\nDelete after: {duration}m"
cleanmode_hint: "Use /settings to change auto-delete duration."
cleanmode_duration_updated: "🧹 Auto-delete duration set to {duration} minutes."
cleanmode_updated: "🧹 Clean mode has been {action}."

settings_info_lang: |
  🌐 What is this?

  Select the language for the bot's messages and interface in this group.

settings_info_nothumb: |
  🖼️ What is this?

  Toggle whether to show track artwork/thumbnails in playback messages.

settings_info_playmode: |
  🎵 What is this?

  Control who can use the /play command. If enabled, only administrators and authorized users can start playback.

settings_main: |
  ⚙️ <b>Chat Settings</b>

  <b>Group:</b> {title}
  <b>Group ID:</b> <code>{id}</code>

  Configure the bot's behavior in this group using the buttons below.

settings_updated: "✅ Settings updated!"


speed_line: "⚙️ Speed: <b>{speed}x</b>"

stats_overview: |
  🔧 <b>System:</b><blockquote>
  • OS: <code>{os}</code>, Arch: <code>{arch}</code>
  • CPUs: <code>{cpus}</code>, Goroutines: <code>{goroutines}</code></blockquote>

  📦 <b>Internal Memory (Go):</b><blockquote>
  • Alloc: <code>{alloc} MB</code>
  • Sys: <code>{sys} MB</code>
  • NumGC: <code>{gc_count}</code> ({gc_emoji} {gc_rate}/min)</blockquote>

  💻 <b>Server Stats:</b><blockquote>
  • CPU Usage: {cpu_emoji} <code>{cpu}%</code>
  • RAM Usage: {ram_emoji} <code>{ram_used_gib} GiB</code> | <code>{ram_total_gib} GiB</code>
  • Storage: <code>{storage_used_gib} GiB</code> | <code>{storage_total_gib} GiB</code></blockquote>

  🍃 <b>MongoDB Stats:</b><blockquote>
  • Storage: <code>{db_data} / {db_storage} MB</code>
  • Collections: <code>{db_collections}</code>
  • Objects: <code>{db_objects}</code></blockquote>

  📊 <b>Served:</b><blockquote>
  • Chats: <code>{served_chats}</code>
  • Users: <code>{served_users}</code></blockquote>

  🧩 <b>Runtime Versions:</b><blockquote>
  • Go: <code>{go_version}</code>
  • Gogram API Layer: <code>{gogram_api_layer}</code>
  • Gogram Version: <code>{gogram_version}</code>
  • NTgCalls Version: <code>{ntgcalls_version}</code></blockquote>

# 👑 Sudo users

sudo_owner_self: "😂 Haha, you’re the boss already! Why are you even trying to add yourself?"
sudo_bot_self: "🤖 Haha, I’m the bot! I can’t add myself to the sudo list — even the owner can’t cheat me 😎"
sudo_bot_user: "🤖 You can’t add a bot to the sudo list — sudoers must be human!"

sudo_fetch_user_fail: "❌ Failed to fetch user info. Maybe the user is inaccessible.\n<i>{error}</i>"
sudo_check_fail: "❌ Failed to check sudo existence: <i>{error}</i>"
sudo_already: "⚠️ User {user} (ID:<code>{id}</code>) is already a sudoer."
sudo_add_fail: "❌ Failed to add sudo: <i>{error}</i>"
sudo_add_success: "✅ Added {user} (<code>{id}</code>) to sudoers."

sudo_owner_remove_block: "😎 Nice try! You can’t remove the owner — they’re untouchable."
sudo_not_exists: "⚠️ User {user} (<code>{id}</code>) is not a sudoer."
sudo_remove_fail: "❌ Failed to remove sudo: <i>{error}</i>"
sudo_remove_success: "🗑️ Removed {user} (ID: <code>{id}</code>) from sudoers."

sudo_list_fetching: "⏳ Fetching sudoers list..."
sudo_list_fetch_fail: "❌ Failed to get sudoers: <code>{error}</code>"
sudo_list_header: "👑 <b>Current Sudoers:</b>"
sudo_list_owner: "{index}. {user} — <b>Owner</b>"
sudo_list_entry: "{index}. {user}"
sudo_list_no_extra: "⚠️ No additional sudoers found."

seek_no_active: "<blockquote>⚠️ <b><u>ɴσ ᴧᴄᴛɪᴠє ᴛꝛᴧᴄᴋ</u></b>\n\n<i>ησ ᴛꝛᴧᴄᴋ ɪs ᴄᴜꝛꝛєηᴛʟʏ ᴘʟᴧʏɪηɢ.</i></blockquote>"
seek_usage: "⚠️ Please provide seconds.\nExample: <code>{cmd} 40</code>"
seek_invalid_seconds: "⚠️ Invalid seconds value.\nExample: <code>{cmd} 40</code>"
seek_too_close_start: "⚠️ Cannot seek backward <b>{seconds}</b> seconds — that would be too close to the beginning of the track."
seek_too_close_end: "⚠️ Cannot seek forward <b>{seconds}</b> seconds — that would be too close to the end of the track."
seek_failed: "❌ Failed to seek {direction} <b>{seconds}</b> seconds.\nError: <code>{error}</code>"
seek_success: "{emoji} Jumped {direction} to <b>{position}</b> of <b>{duration}</b>."

stop_confirm_paused: |
  ⏸️ <b>Playback is currently paused.</b>

  Do you want to resume playing or stop it completely?

stop_confirm_muted: |
  🔇 <b>Playback is currently muted.</b>

  Do you want to unmute or stop it completely?

stopped: |
  <blockquote>⏹️ <b><u>sᴛσᴘᴘєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <i>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ &amp; ǫᴜєᴜє ᴄʟєᴧꝛєᴅ.</i>
  • <b>sᴛσᴘᴘєᴅ ʙʏ:</b> {user}</blockquote>

shuffle_current_state: "🔀 Currently shuffle is <b>{state}</b> for this chat.\n\nUse <code>{cmd}</code> to toggle it."
shuffle_updated: "🔀 Shuffle <b>{state}</b> by {user}."

skip_stopped: |
  <blockquote>⏹️ <b><u>ᴘʟᴧʏʙᴧᴄᴋ sᴛσᴘᴘєᴅ</u></b>

  • <i>ησ ᴍσꝛє ᴛꝛᴧᴄᴋs ɪη ǫᴜєᴜє.</i>
  • <b>sᴋɪᴘᴘєᴅ ʙʏ:</b> {user}</blockquote>
skip_invalid_number: "⚠️ Please provide a valid number after /skip.\nExample: <code>/skip 3</code>"
skip_queue_empty_for_count: "⚠️ Queue has no upcoming tracks to skip."
skip_count_exceeds_queue: "⚠️ You requested to skip <code>{requested}</code> queued tracks, but only <code>{available}</code> track(s) are in queue."

speed_usage: |
  💡 <b>Usage:</b>
  <code>{cmd} [speed] [duration]</code> — change playback speed
  
  Allowed speed range: <b>0.50x → 4.00x</b>
  Duration (optional) resets the speed after given seconds.
  
  Example:
  <code>{cmd} 1.25 45</code> — play at 1.25x for 45 seconds.

speed_current: "🎧 <b>Current Speed:</b> <code>{speed}x</code> for 🎵 <u>{title}</u>\n\n💡 Use <code>{cmd} reset</code> to restore normal playback."

speed_current_with_reset: |
  🎧 <b>Current Speed:</b> <code>{speed}x</code>

  🎵 <b>Track:</b> <i>{title}</i>
  ⏳ <b>Auto-reset in:</b> {duration}
  
  💡 Use <code>{cmd} reset</code> to restore normal playback instantly.

speed_invalid_value: "❌ Invalid speed value.\nExample: <code>{cmd} 1.5</code> or <code>{cmd} 2x</code>"
speed_invalid_range: "⚠️ Speed must be between <b>0.50x</b> and <b>4.00x</b>."
speed_invalid_duration: "⚠️ Invalid duration. It must be between <b>5</b> and <b>3600</b> seconds."

speed_already_set: "ℹ️ Playback speed is already <b>{speed}x</b> for 🎵 <b>{title}</b>"

speed_already_set_reset_hint: "ℹ️ Playback speed is already <b>{speed}x</b> for 🎵 <b>{title}</b>\n\n💡 Use <code>{cmd} reset</code> to return to normal speed."

speed_failed: |
  ❌ Failed to change speed to <b>{speed}x</b>.
  Error: <code>{error}</code>

speed_reset_success: "✅ Playback speed reset to <b>1.0x</b> by {user}"

speed_set: "🚀 Playback speed set to <b>{speed}x</b> by {user}."
speed_set_with_reset: |
  🚀 Playback speed set to <b>{speed}x</b> by {user}.
  ⏳ Will reset to normal after <b>{duration}</b> seconds.


spt_fetch_fail: "⚠️ Failed to fetch network info: <code>{error}</code>"
spt_servers_fetch_fail: "⚠️ Failed to fetch servers: <code>{error}</code>"
spt_best_server_fail: "⚠️ Failed to find best server: <code>{error}</code>"

spt_running_download: "🚀 Running download test..."
spt_running_upload: "📤 Running upload test..."

spt_result: |
  <b>📡 SpeedTest Result</b>

  <b><u>👤 Client</u></b>
  IP: <code>{ip}</code>
  ISP: {isp}
  Lat: <code>{lat}</code>
  Lon: <code>{lon}</code>

  <b><u>🛰️ Server</u></b>
  Name: {server_name}
  Country: {country}
  Sponsor: {sponsor}
  Distance: {distance_km} km
  Latency: {latency_ms} ms

  <b><u>⚡ Speed</u></b>
  Download: <code>{dl_mbps} Mbps</code>
  Upload: <code>{ul_mbps} Mbps</code>

start_private: |
  <blockquote><a href="https://yukiapi.site/file/iCggpP12">👋</a> <b>ʜєʏ {user}</b>, <i>ᴍʏ ηᴧᴍє ɪs</i> {bot} 🎶

  ✨ <b><u>ᴧʙσυᴛ ᴍє</u>:</b>
  • ᴧ ᴘσᴡєꝛғᴜʟ & ʜɪɢʜ-ǫᴜᴧʟɪᴛʏ ᴛєʟєɢꝛᴧᴍ ᴍᴜsɪᴄ sᴛꝛєᴧᴍɪηɢ ʙσᴛ.
  • <s>ʟᴧɢɢʏ ᴧᴜᴅɪσ</s> ➜ <tg-spoiler><b>ᴜʟᴛꝛᴧ ʜᴅ ǫᴜᴧʟɪᴛʏ 🎧</b></tg-spoiler>
  • sᴜᴘᴘσꝛᴛs: <code>ʏσυᴛᴜʙє</code>, <code>sᴘσᴛɪғʏ</code>, <code>ᴊɪσsᴧᴧᴠη</code>, <code>sσυηᴅᴄʟσυᴅ</code> & ᴍσꝛє.
  • <b>sᴛᴧᴛᴜs:</b> <tg-spoiler>⚡ ᴏɴʟɪɴᴇ & ʀᴇᴀᴅʏ ᴛᴏ ᴘʟᴀʏ</tg-spoiler>

  💫 <i>ғєєʟ ғꝛєє ᴛσ ᴧᴅᴅ ᴍє ᴛσ ʏσυꝛ ɢꝛσυᴘs & єηᴊσʏ!</i> ✨</blockquote>
  <blockquote expandable>📖 <b><u>ǫᴜɪᴄᴋ ɢᴜɪᴅє & ᴄσϻϻᴧηᴅs</u>:</b>
  • <b>/play</b> <code>&lt;song&gt;</code> ➜ <i>ᴘʟᴧʏ ᴧᴜᴅɪσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/vplay</b> <code>&lt;query&gt;</code> ➜ <i>ᴘʟᴧʏ ᴠɪᴅєσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/settings</b> ➜ <i>ᴄᴜsᴛσᴍɪᴢє ʙσᴛ ᴘꝛєғєꝛєηᴄєs</i>
  • <b>/help</b> ➜ <i>ɢєᴛ ғᴜʟʟ ᴄσϻϻᴧηᴅ ʟɪsᴛ</i>
  • <b>sᴇᴄʀᴇᴛ:</b> <tg-spoiler>⚡ ᴜsє /speedtest ᴛσ ᴄʜєᴄᴋ ᴘɪηɢ</tg-spoiler>
  • <b>ᴛɪᴘ:</b> <tg-spoiler>💡 ᴜsᴇ /cplay ғᴏʀ ᴄʜᴀɴɴᴇʟ ꜱᴛʀᴇᴀᴍɪɴɢ</tg-spoiler></blockquote>

start_group: "<b>💫 I’m here!</b> Try a command… if you think you can handle me 😉"

stream_channelid_fail: "⚠️ Failed to get your connected channel’s ID."
stream_queue_finished: "<blockquote>🎵 <b><u>ǫᴜєᴜє ғɪηɪsʜєᴅ</u></b>\n\n<i>ησ ᴍσꝛє ᴛꝛᴧᴄᴋs ʟєғᴛ ᴛσ ᴘʟᴧʏ. ᴜsє /play ᴛσ ᴧᴅᴅ ᴍσꝛє! 🎶</i></blockquote>"
stream_downloading_next: "<blockquote>📥 <b><u>ᴅσᴡηʟσαᴅɪηɢ ηєхᴛ ᴛꝛᴧᴄᴋ...</u></b></blockquote>"
autoplay_fetching_next: "<blockquote>🎵 <b><u>ᴧᴜᴛσᴘʟᴧʏ</u></b> ➜ <i>ғєᴛᴄʜɪηɢ ηєхᴛ ᴛꝛᴧᴄᴋ...</i></blockquote>"
stream_download_fail: "❌ Failed to download.\nError: <code>{error}</code>"
stream_play_fail: "❌ Failed to play the song."
stream_now_playing: |
  <blockquote>{thumb} <b><u>ησᴡ ᴘʟᴧʏɪηɢ</u></b></blockquote>
  <blockquote>• <b>ᴛꝛᴧᴄᴋ:</b> <a href="{url}">{title}</a>
  • <b>ᴅᴜꝛᴧᴛɪση:</b> <code>{duration}</code>
  • <b>ꝛєǫᴜєsᴛєᴅ ʙʏ:</b> {by}</blockquote>

# 👑 Sudo / Owner Join Messages
sudo_join_owner: "👑 {user} — owner of {bot} — just entered the chat."
sudo_join_sudo: "🛡️ {user}, a sudo user of {bot}, just joined the chat."

supergroup_needed: |
  This chat (ID: <code>{chat_id}</code>) is not a SuperGroup yet.

  <i>⚠️ Please convert this group into a SuperGroup, then add me again as an admin.</i>

  <b>How to convert?</b>
  <a href="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">🔗 Go to the guide</a>

  If you still face any issues, join our <a href="{support_group}">support group.</a>

unmute_already: "ℹ️ <b>Already Unmuted</b>\nThe music is not muted in this chat."
unmute_failed: "❌ <b>Playback Unmute Failed</b>\nError: <code>{error}</code>"
unmute_success: |
  <blockquote>🔊 <b><u>ᴜηᴍᴜᴛєᴅ ᴘʟᴧʏʙᴧᴄᴋ</u></b>

  • <b>ᴛꝛᴧᴄᴋ:</b> {title}
  • <b>ᴜηᴍᴜᴛєᴅ ʙʏ:</b> {user}
  {speed_line}</blockquote>


# 🔊 Voice Chat Events
voicechat_ended: "<b>⚠️ Video chat ended</b>\n\n<i>⏰ Duration: {duration}</i>"
voicechat_started: "<b>♻️ Voice chat started!</b>\n\n<i>Use /play [song name] to play music.</i>"

# Commands help
help_owner: |
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">👑</a> <b><u>σᴡηєꝛ ᴄσϻϻᴧηᴅs</u></b>

  <i>ᴇxᴄʟᴜsɪᴠє ᴛσσʟs ғσꝛ sʏsᴛєᴍ ᴧᴅᴍɪηɪsᴛꝛᴧᴛɪση &amp; sᴜᴅσ ᴍᴧηᴧɢєᴍєηᴛ.</i>

  • <b>/addsudo</b> ➜ <i>ᴧᴅᴅ ᴧ ᴜsєꝛ ᴛσ sᴜᴅσ ʟɪsᴛ</i>
  • <b>/delsudo</b> ➜ <i>ꝛєᴍσᴠє ᴧ ᴜsєꝛ ғꝛσᴍ sᴜᴅσ ʟɪsᴛ</i>
  • <b>/blockuser</b> ➜ <i>ᴘꝛєᴠєηᴛ ᴜsєꝛ ғꝛσᴍ ᴜsɪηɢ ʙσᴛ</i>
  • <b>/unblockuser</b> ➜ <i>ᴜηʙʟσᴄᴋ ᴜsєꝛ ғꝛσᴍ ʙσᴛ</i>
  • <b>/blockchat</b> ➜ <i>ʙʟσᴄᴋ ᴄʜᴧᴛ ғꝛσᴍ ᴜsɪηɢ ʙσᴛ</i>
  • <b>/unblockchat</b> ➜ <i>ᴜηʙʟσᴄᴋ ᴄʜᴧᴛ ғꝛσᴍ ʙσᴛ</i>
  • <b>/blacklisted</b> ➜ <i>ᴠɪєᴡ ɢʟσʙᴧʟ ʙʟᴧᴄᴋʟɪsᴛ</i>
  • <b>/eval</b> ➜ <i>єxєᴄᴜᴛє ɢσ ᴄσᴅє sηɪᴘᴘєᴛs</i>
  • <b>/maintenance</b> ➜ <i>ᴛσɢɢʟє ᴍᴧɪηᴛєηᴧηᴄє ᴍσᴅє</i>
  • <b>/restart</b> ➜ <i>ꝛєsᴛᴧꝛᴛ ᴛʜє ʙσᴛ єηɢɪηє</i>
  • <b>/sh</b> ➜ <i>ꝛᴜη sʜєʟʟ ᴄσϻϻᴧηᴅs</i>

  🔒 <b>sᴇᴄᴜʀɪᴛʏ:</b> <tg-spoiler>ᴏᴡɴᴇʀ ᴄᴏᴍᴍᴀɴᴅꜱ ᴀʀᴇ ꜱᴛʀɪᴄᴛʟʏ ʀᴇꜱᴛʀɪᴄᴛᴇᴅ</tg-spoiler></blockquote>

help_sudo: |
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">⚡</a> <b><u>sᴜᴅσєꝛ ᴄσϻϻᴧηᴅs</u></b>

  <i>ᴧᴅᴠᴧηᴄєᴅ ᴛσσʟs ғσꝛ ᴍσηɪᴛσꝛɪηɢ &amp; ᴄσηᴛꝛσʟʟɪηɢ ʙσᴛ ʙєʜᴧᴠɪσꝛ.</i>

  • <b>/active</b> ➜ <i>ᴠɪєᴡ ᴧᴄᴛɪᴠє ᴠᴄ ᴄʜᴧᴛs</i>
  • <b>/autoleave</b> ➜ <i>ᴛσɢɢʟє ᴧᴜᴛσ-ʟєᴧᴠє ɪηᴧᴄᴛɪᴠє ᴠᴄ</i>
  • <b>/logger</b> ➜ <i>єηᴧʙʟє σꝛ ᴅɪsᴧʙʟє ʟσɢɢєꝛ</i>
  • <b>/stats</b> ➜ <i>ᴠɪєᴡ ʙσᴛ &amp; sʏsᴛєᴍ sᴛᴧᴛs</i>
  • <b>/logs</b> ➜ <i>ɢєᴛ ʟɪᴠє sʏsᴛєᴍ ʟσɢs</i>

  ⚡ <b>sᴛᴧᴛᴜs:</b> <tg-spoiler>ꜱᴜᴅᴏᴇʀ ᴀᴄᴄᴇꜱꜱ ɢʀᴀɴᴛᴇᴅ</tg-spoiler></blockquote>

help_admin: |
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">🛠</a> <b><u>ᴧᴅϻɪη ᴄσϻϻᴧηᴅs</u></b>

  <i>ᴍᴜsɪᴄ &amp; ᴘʟᴧʏʙᴧᴄᴋ ᴄσηᴛꝛσʟs ғσꝛ ɢꝛσυᴘ ᴧᴅϻɪηs.</i>

  • <b>/playmode</b> ➜ <i>ᴄσηᴛꝛσʟ ᴡʜσ ᴄᴧη ᴜsє /play</i>
  • <b>/cplay</b> ➜ <i>ᴘʟᴧʏ ɪη ʟɪηᴋєᴅ ᴄʜᴧηηєʟ ᴠᴄ</i>
  • <b>/adminmode</b> ➜ <i>ᴄσηᴛꝛσʟ ᴧᴅϻɪη ᴄσϻϻᴧηᴅs ᴍσᴅє</i>
  • <b>/cmddelete</b> ➜ <i>ᴧᴜᴛσ-ᴅєʟєᴛє ʙσᴛ ᴄσϻϻᴧηᴅs</i>
  • <b>/settings</b> ➜ <i>ᴄσηғɪɢᴜꝛє ᴄʜᴧᴛ sєᴛᴛɪηɢs</i>
  • <b>/speed</b> ➜ <i>ᴄʜᴧηɢє ᴘʟᴧʏʙᴧᴄᴋ sᴘєєᴅ</i>
  • <b>/skip</b> ➜ <i>sᴋɪᴘ ᴄᴜꝛꝛєηᴛ sσηɢ</i>
  • <b>/pause</b> ➜ <i>ᴘᴧᴜsє ᴘʟᴧʏʙᴧᴄᴋ</i>
  • <b>/resume</b> ➜ <i>ꝛєsᴜᴍє ᴘʟᴧʏʙᴧᴄᴋ</i>
  • <b>/replay</b> ➜ <i>ꝛєᴘʟᴧʏ ᴄᴜꝛꝛєηᴛ sσηɢ</i>
  • <b>/mute</b> ➜ <i>ᴍᴜᴛє ᴠᴄ ᴘʟᴧʏʙᴧᴄᴋ</i>
  • <b>/unmute</b> ➜ <i>ᴜηᴍᴜᴛє ᴠᴄ ᴘʟᴧʏʙᴧᴄᴋ</i>
  • <b>/seek</b> ➜ <i>sєєᴋ ғσꝛᴡᴧꝛᴅ ɪη ᴛꝛᴧᴄᴋ</i>
  • <b>/seekback</b> ➜ <i>sєєᴋ ʙᴧᴄᴋᴡᴧꝛᴅ ɪη ᴛꝛᴧᴄᴋ</i>
  • <b>/jump</b> ➜ <i>ᴊᴜᴍᴘ ᴛσ sᴘєᴄɪғɪᴄ ᴛɪᴍє</i>
  • <b>/move</b> ➜ <i>ᴍσᴠє ǫᴜєᴜє ᴘσsɪᴛɪση</i>
  • <b>/clear</b> ➜ <i>ᴄʟєᴧꝛ ᴧʟʟ sσηɢs ғꝛσᴍ ǫᴜєᴜє</i>
  • <b>/remove</b> ➜ <i>ꝛєᴍσᴠє ᴛꝛᴧᴄᴋ ғꝛσᴍ ǫᴜєᴜє</i>
  • <b>/shuffle</b> ➜ <i>sʜᴜғғʟє ǫᴜєᴜє ᴛꝛᴧᴄᴋs</i>
  • <b>/loop</b> ➜ <i>єηᴧʙʟє/ᴅɪsᴧʙʟє ʟσσᴘɪηɢ</i>
  • <b>/stop</b> ➜ <i>sᴛσᴘ ᴘʟᴧʏʙᴧᴄᴋ &amp; ʟєᴧᴠє ᴠᴄ</i>
  • <b>/nothumb</b> ➜ <i>ᴛσɢɢʟє ᴧꝛᴛᴡσꝛᴋ ɪη ᴍєssᴧɢєs</i>

  💡 <b>ᴛɪᴘ:</b> <tg-spoiler>ᴜsᴇ /cplay &lt;song&gt; ᴛᴏ sᴛʀᴇᴀᴍ ɪɴ ᴄʜᴀɴɴᴇʟ</tg-spoiler></blockquote>

help_public: |
  <blockquote><a href="https://yukiapi.site/file/awTXo6DL">🌍</a> <b><u>ᴘᴜʙʟɪᴄ ᴄσϻϻᴧηᴅs</u></b>

  <i>ғєᴧᴛᴜꝛєs ᴧᴠᴧɪʟᴧʙʟє ғσꝛ ᴧʟʟ ɢꝛσυᴘ ᴍєᴍʙєꝛs.</i>

  • <b>/play</b> ➜ <i>ᴘʟᴧʏ ᴧᴜᴅɪσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/vplay</b> ➜ <i>ᴘʟᴧʏ ᴠɪᴅєσ ɪη ɢꝛσυᴘ ᴠᴄ</i>
  • <b>/queue</b> ➜ <i>ᴠɪєᴡ ᴄᴜꝛꝛєηᴛ ǫᴜєᴜєᴅ ᴛꝛᴧᴄᴋs</i>
  • <b>/ping</b> ➜ <i>ᴄʜєᴄᴋ ʙσᴛ ʟᴧᴛєηᴄʏ</i>
  • <b>/start</b> ➜ <i>sᴛᴧꝛᴛ ᴛʜє ʙσᴛ ᴍєηᴜ</i>
  • <b>/help</b> ➜ <i>sʜσᴡ ʜєʟᴘ &amp; ᴄσϻϻᴧηᴅs</i>
  • <b>/bug</b> ➜ <i>ꝛєᴘσꝛᴛ ᴧη ɪssᴜє/ʙᴜɢ</i>
  • <b>/position</b> ➜ <i>sʜσᴡ ᴛꝛᴧᴄᴋ ᴛɪᴍєsᴛᴧᴍᴘ</i>
  • <b>/reload</b> ➜ <i>ꝛєʟσᴧᴅ ᴧᴅϻɪη ᴄᴧᴄʜє</i>
  • <b>/sudolist</b> ➜ <i>ᴠɪєᴡ ʙσᴛ sᴜᴅσ ᴜsєꝛs</i>

  🎵 <b>ғᴜɴ:</b> <tg-spoiler>ꜱᴜᴘᴘᴏʀᴛꜱ ʏᴏᴜᴛᴜʙᴇ, ꜱᴘᴏᴛɪꜰʏ &amp; ᴍᴏʀᴇ!</tg-spoiler></blockquote>
```


## Appendix C — VERBATIM: YukkiMusic-Go `internal/locales/en.yml`

```yaml
name: "🇺🇸 English"

# buttons
CONFIRM_RESUME_BTN: "{emoji:▶️} Resume"
CONFIRM_UNMUTE_BTN: "{emoji:🔊} Unmute"
CONFIRM_STOP_BTN: "{emoji:⏹️} Stop"
PAUSE_BTN: "{emoji:⏸️} Pause"
RESUME_BTN: "{emoji:▶️} Resume"
MUTE_BTN: "{emoji:🔇} Mute"
UNMUTE_BTN: "{emoji:🔊} Unmute"
ADD_ME_BTN: "{emoji:⚡} Add Me to Your group"
BROADCAST_CANCEL_BTN: "{emoji:🚫} Cancel Broadcast"
CLOSE_BTN: "Close"
DOWNLOAD_CANCEL_BTN: "{emoji:🚦} Cancel Downloading"
GC_HELP_BTN: "{emoji:📒} Commands"

START_HELP_BTN: "{emoji:❓} Help & Commands"

UPDATES_BTN: "{emoji:📢} Updates"
SUPPORT_BTN: "{emoji:💬} Support"
SOURCE_BTN: "{emoji:🔗} Source Code"

HELP_ADMINS_BTN: "{emoji:🛠} Admins"
HELP_PUBLIC_BTN: "{emoji:🌍} Public"
HELP_OWNER_BTN: "{emoji:👑} Owner"
HELP_SUDOERS_BTN: "{emoji:⚡} Sudoers"

BACK_BTN: "{emoji:⬅️} Back"

# basically this string used in /command [bool]

adminmode_help: |
  {emoji:🛡️} <b>Admin Command Mode</b>

  <b>Status:</b> {status}
  <b>Usage:</b> <code>/adminmode [admin|adminauth|everyone]</code>
adminmode_status_admin: "Admins 🔐"
adminmode_status_adminauth: "Auth Users 🎧"
adminmode_status_everyone: "Everyone 🌍"
adminmode_updated: "{emoji:✅} <b>Admin command mode</b> updated to: {status}"
adminmode_invalid: "{emoji:⚠️} Invalid mode. Use: <code>admin</code>, <code>adminauth</code>, or <code>everyone</code>."

# 🚫 Assistant Restricted / Banned
assistant_restricted_warning: |
  {emoji:⚠️} <b>Assistant Restricted</b>

  The assistant {assistant} (ID: <code>{id}</code>) has been <b>banned</b> or restricted in this chat.
  I cannot play music, manage the queue, or clean up tracks while this continues.

  <i>Unban the assistant to restore all music features {emoji:🎵}</i>

# 🧩 Common auth messages
auth_no_user: "{emoji:⚠️} Please provide a user — use:\n{cmd} [user_id] or reply to a user's message."
user_extract_fail: "{emoji:⚠️} Unable to get that user: <i>{error}</i>"
auth_limit_reached: "{emoji:⚠️} This chat has reached the maximum authorized users limit ({limit}). Please remove someone before adding a new one."
already_authed: "{emoji:⚠️} That user is already authorized — no need to add them again."

# 🤖 Add auth
cannot_authorize_user: "{emoji:⚠️} This user cannot be authorized.\nThe bot owner, the bot itself, or yourself do not require authorization."
addauth_user_is_admin: "{emoji:⚠️} This user is already a chat administrator. No authorization is required."
addauth_bot_user: "{emoji:🤖} You can’t add a bot to the auth list — authusers must be human!"
addauth_add_fail: "{emoji:❌} Failed to add authorized user: {error}"
addauth_success_with_count: "{emoji:✅} Added {user}.\nNow {count}/{limit} authorized users."
addauth_success: "{emoji:✅} Successfully added {user} to the authorized users list."

# 🤖 Bot Added to Chat
bot_added_normal: "{emoji:🎵} Thanks for adding me! Use /play [song] to start playing music."

# ⚠️ Bot Demotion
bot_demotion_goodbye: |
  <b>{emoji:⚠️} Permission Lost</b>

  I’ve been <b>demoted from admin</b> and can’t perform my tasks anymore.
  Without proper rights, I’m unable to function here, so I’ll be <i>leaving the chat now</i>.

  <b>Goodbye {emoji:👋}</b>

broadcast_already_running: "{emoji:⚠️} A broadcast is already running.\n\nPlease wait for it to complete or cancel it first using:\n• <code>/broadcast -cancel</code>\n• Or click the Cancel button on the progress message"
broadcast_no_content: "{emoji:⚠️} No content provided for broadcast.\n\n<b>Usage:</b> <code>{cmd} [text or reply to message]</code>\n\n<b>Example:</b>\n<code>{cmd} Hello everyone!</code>"
broadcast_fetch_chats_failed: "{emoji:❌} Failed to fetch served chats:\n<code>{error}</code>"
broadcast_fetch_users_failed: "{emoji:❌} Failed to fetch served users:\n<code>{error}</code>"
broadcast_no_targets: "{emoji:⚠️} No targets found for broadcast.\n\nMake sure you haven't excluded all targets with flags."
broadcast_initializing: "{emoji:📡} Initializing broadcast...\n\nPlease wait..."
broadcast_progress_header: "{emoji:📡} Broadcasting..."
broadcast_total_chats: "{emoji:📊} Total Chats: {done}/{total} ({progress}%)"
broadcast_total_users: "{emoji:👥} Total Users: {done}/{total} ({progress}%)"
broadcast_failed_chats: "{emoji:❌} Failed Chats: {count}"
broadcast_failed_users: "{emoji:❌} Failed Users: {count}"
broadcast_elapsed: "{emoji:⏰} Elapsed: {elapsed}"
broadcast_success_rate: "{emoji:✨} Success Rate: {rate}% ({sent}/{total})"
broadcast_not_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_success: "{emoji:🚫} Broadcast cancelled successfully."
broadcast_cancel_owner_only: "⚠️ Only the owner can cancel broadcasts."
broadcast_cancel_none_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_done: "🚫 Broadcast cancelled."

# Blocklist

blacklist_chat_blocked: "{emoji:🚫} This chat is blocked from using this bot."
blacklist_owner_blocked_leave: "{emoji:🚫} This chat owner is blocked from using this bot. Leaving chat."
blacklist_user_blocked: "{emoji:🚫} You are blocked from using this bot."
blacklist_usage_blockchat: "Usage: <code>/blockchat [chat_id|@username]</code>"
blacklist_usage_unblockchat: "Usage: <code>/unblockchat [chat_id|@username]</code>"
blacklist_invalid_chat_identifier: "Invalid chat identifier: {error}"
blacklist_block_user_success: "{emoji:✅} User blocked: <code>{id}</code>"
blacklist_unblock_user_success: "{emoji:✅} User unblocked: <code>{id}</code>"
blacklist_block_chat_success: "{emoji:✅} Chat blocked: <code>{id}</code>"
blacklist_unblock_chat_success: "{emoji:✅} Chat unblocked: <code>{id}</code>"
blacklist_unblock_user_fail: "{emoji:⚠️} Failed to unblock user: {error}"
blacklist_unblock_chat_fail: "{emoji:⚠️} Failed to unblock chat: {error}"
blacklist_fetch_chats_fail: "{emoji:⚠️} Failed to fetch blocked chats: {error}"
blacklist_fetch_users_fail: "{emoji:⚠️} Failed to fetch blocked users: {error}"
blacklist_list_title: "{emoji:📛} <b>Blacklisted</b>"
blacklist_list_chats: "{emoji:💬} <b>Chats:</b>"
blacklist_list_users: "{emoji:👤} <b>Users:</b>"

# 🎧 CPlay (Channel Play)

cplay_id_not_set: "{emoji:⚠️} cplay ID not set.\nUse <code>/setcplay &lt;chat_id&gt;</code> to set it."
cplay_usage: "<b>Usage:</b> <code>/setcplay &lt;channel_id | @username | linked | off&gt;</code>"
cplay_resolve_peer_fail: "<b>Failed to resolve peer:</b> Could not fetch channel details. Ensure I can access this channel."
cplay_invalid_target: "<b>Invalid Target:</b> The provided ID is not a valid channel."
cplay_channel_not_accessible: "<b>Channel Not Accessible:</b> Could not retrieve channel information. Please ensure I am an administrator in the target channel."
cplay_bot_invite_permission_missing: "{emoji:⚠️} <b>Missing Permission:</b> I am admin in target channel but <code>Invite Users</code> permission is disabled. Please enable it and try again."
cplay_channel_not_linked: "{emoji:⚠️} <b>Linked channel not found:</b> This chat is not linked to any channel/group."
cplay_save_error: "<b>Error:</b> Failed to save CPlay settings."
cplay_enabled: "{emoji:✅} <b>Channel Play enabled.</b> All channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) will now work in channel <code>{channel_id}</code>."
cplay_disabled: "{emoji:✅} <b>Channel Play disabled.</b> Channel-play commands (like <code>/cplay</code>, <code>/cskip</code>) are now turned off for this chat."

# Callback handler messages
invalid_request: "⚠️ Invalid request."
unknown_action: "⚠️ Unknown action."
download_cancelled: "✅ Download cancelled."
no_download_to_cancel: "ℹ️ No download to cancel."

# Pause callback
cb_pause_success: "⏸️ Track paused at {position}"
cb_pause_message: |
  <b>{emoji:⏸️} Track Paused</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Position:</b> {position} / {duration}
  <b>▫ Paused by:</b> {user}

# Resume callback
cb_already_playing: "ℹ️ Track is already playing."
cb_resume_failed: "❌ Failed to resume playback."
cb_pause_failed: "❌ Failed to pause playback."
cb_resume_success: "▶️ Resumed at {position}."
cb_resume_message: |
  <b>{emoji:🎵} Now Playing:</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Resumed by:</b> {user}

# Replay status (loop replay)
cb_replaying: "{emoji:🔁} <b>Replaying current track...</b>"
cb_replay_failed: "❌ Failed to replay track."
cb_replay_success: "🔁 Track replayed."
cb_replay_edited: "{emoji:🔁} Track replayed by {user}."

# Skip callback
cb_skip_queue_empty: "⏹️ Playback stopped — queue empty."
cb_skip_download_failed: "❌ Failed to download next track."
cb_skip_play_failed: "❌ Failed to play next track."
cb_skip_success: "⏭️ Track skipped."
cb_skip_edited: "{emoji:⏭️} Skipped by {user}"

# Stop callback
cb_stop_success: "⏹️ Playback stopped."

# Mute/Unmute callback
cb_mute_success: "🔇 Muted."
cb_mute_message: |
  <b>{emoji:🔇} Playback Muted</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Muted by:</b> {user}

cb_unmute_success: "🔊 Unmuted."
cb_unmute_message: |
  <b>{emoji:🔊} Playback Unmuted</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Unmuted by:</b> {user}

cb_already_muted: "🔇 Track is already muted."
cb_already_unmuted: "🔊 Track is not muted."
cb_mute_failed: "❌ Failed to mute playback."
cb_unmute_failed: "❌ Failed to unmute playback."

# 🗑️ Delete auth
del_auth_not_authorized: "{emoji:⚠️} That user isn’t authorized — nothing to remove."
del_auth_remove_fail: "{emoji:❌} Failed to remove authorized user: <code>{error}</code>"
del_auth_success: "{emoji:✅} Successfully removed {user} from the authorized users list."

# 📜 Auth list
authlist_fetch_fail: "{emoji:❌} Failed to get authorized users list: <code>{error}</code>"
authlist_empty: "ℹ️ There are no authorized users in this chat."
authlist_title: "{emoji:🔐} Authorized Users"
authlist_count: "{emoji:👥} {count}/{limit} authorized"
authlist_col_user: "{emoji:👤} User"
authlist_col_id: "{emoji:🆔} ID"
authlist_unknown_user: "<i>Unknown</i>"
authlist_hint: "Tap ⨯ to revoke access."
authlist_delete_success: "{emoji:🗑️} Authorization revoked."

autoleave_fetch_fail: "{emoji:⚠️} <b>Failed to fetch AutoLeave state.</b>"
autoleave_title: "{emoji:🏃‍♂️} AutoLeave Control"
autoleave_desc: "When enabled, the assistant automatically leaves inactive or unnecessary chats every 10 minutes."
autoleave_update_fail: "{emoji:⚠️} <b>Failed to update AutoLeave state.</b>"

dashboard_status: "{emoji:ℹ️} Status: <b>{status}</b>"
dashboard_enabled: "Enabled {emoji:✅}"
dashboard_disabled: "Disabled {emoji:❌}"
dashboard_toggled_cb: "✅ Setting updated to {status}."
dashboard_fail_cb: "⚠️ Failed to update the setting."

enabled: "Enabled"
disabled: "Disabled"

clear_no_active: "{emoji:⚠️} <b>No active playback or queue to clear.</b>"
clear_success: "{emoji:✅} The queue has been cleared by {user}."

err_assistant_banned: |
  <b>{emoji:🚫} Assistant Restricted</b>

  I can't play music because {user} (UserID: <code>{id}</code>) is <b>banned</b> or <b>removed</b> from this chat.

  <i>{emoji:✅} Unbanned them already? Use /reload to refresh.</i>

err_admin_permission_required: |
  {emoji:⚠️} <b>Admin Permission Required</b>

  I need admin access with Invite Users permission to manage and invite assistant in this chat.

err_assistant_get_failed: |
  {emoji:⚠️} The system could not assign an assistant to this chat.

  <i>This is usually a temporary issue and should resolve by trying again.</i>

  <b>{emoji:➡️} Please retry your command.</b>

  <i>If this keeps happening, please contact support so we can assist you.</i>
  
err_assistant_join_request_sent: |
  {emoji:⚠️} Assistant sent a join request, but I could not auto-approve it.

  <i>{emoji:✅} Please approve the request manually, then try again.</i>

err_assistant_invite_link_fetch: |
  {emoji:⚠️} Failed to fetch invite link:
  <i>{error}</i>

err_assistant_invite_failed: |
  {emoji:⚠️} Assistant failed to join this chat:
  <i>{error}</i>

err_connection_timeout: |
  {emoji:⚠️} <b>VoiceChat Connection Timeout</b>

  Telegram is not responding properly to the voice chat connection right now.

  <i>If this keeps happening, end the voice chat and start it again, then retry.</i>

err_no_active_voicechat: |
  {emoji:🎙️} <b>No Active Voice Chat</b>

  I can't join — please start a voice chat first.

  <i>Already started one? Use /reload to refresh.</i>

err_fetch_failed: |
  {emoji:⚠️} Failed to fetch chat info:
  <i>{error}</i>

err_unknown: |
  {emoji:⚠️} Unknown Error Occurred:
  <i>{error}</i>

err_fetch_tracks: |
  {emoji:⚠️} <b>Failed to fetch track details.</b>
  Please try again later.

err_download_internal: |
  {emoji:⚠️} <b>Download failed</b> due to an unexpected internal error.
  Please try again later.
  
flood_seconds: "⏳ Slow down! Try again in {duration} seconds."
flood_minutes: "{emoji:⏳} Please wait {duration} minutes before using this command again."

help_private_only: "{emoji:🤖} Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!"
help_main: |
  <h3>{emoji:📚} Help Menu</h3>
  <p>Browse commands by category: <b>Public</b> for everyone, plus <b>Admin</b>, <b>Sudo</b> and <b>Owner</b> for higher access levels.</p>
  <aside>Tip: run <code>/help &lt;command&gt;</code> to get detailed help for any single command.</aside>
  <footer><i>For any issues or support, contact our <a href="{support}">support chat</a>.</i></footer>

jump_no_active: "{emoji:⚠️} No active track to jump within."
jump_usage: "{emoji:⚠️} Please provide seconds.\nExample: <code>{cmd} 120</code>"
jump_invalid_position: "{emoji:⚠️} Invalid position.\nExample: <code>{cmd} 120</code>"
jump_too_close_end: "{emoji:⚠️} Cannot jump to <b>{position}</b> — that’s too close to the end of the track."
jump_failed: "{emoji:❌} Failed to jump to <b>{position}</b>.\nError: <code>{error}</code>"
jump_success: "{emoji:⏩} Jumped to <u>{position}</u> of <u>{duration}</u>."

lang_same: "ℹ️ You’re already using this language."
lang_select: "{emoji:🌐} Select your preferred language:"
lang_fail: "❌ Failed to update language. Please try again later."
lang_success: "{emoji:✅} Language successfully updated to {lang_name}."

logger_title: "{emoji:📝} Logger"
logger_desc: "When enabled, the bot logs playback activity and events to the logger channel."
logger_playback_started: "Playback Started"
logger_playback_queued: "Playback Queued"

logger_bot_added: |
  {emoji:🤖} <b>Bot Added to New Group</b>
  
  <blockquote>
  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Added By:</b> {added_by_name}
  <b>User ID:</b> {added_by_id}
  <b>User Username:</b> {added_by_username}

  <b>Time:</b> {date_time}
  </blockquote>
#  👥 <b>Members Count:</b> {members_count}

logger_bot_removed: |

  {emoji:🤖} <b>Bot Removed from Group</b>
  <blockquote>
  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Removed By:</b> {removed_by_name}
  <b>User ID:</b> {removed_by_id}
  <b>User Username:</b> {removed_by_username}

  <b>Time:</b> {date_time}
  </blockquote>
  
logger_bot_started: |
  {emoji:▶️} <b>{mention} started the bot</b>

  <b>Username:</b> {user_username}
  <b>ID:</b> {user_id}
  

logger_playback_template: "<b>{emoji:🎧} Track:</b> <a href=\"{track_url}\">{track}</a>\n<b>{emoji:🔗} Source:</b> {source}\n<b>{emoji:📌} Group:</b> {group} ({group_id})\n<b>{emoji:👤} Requested by:</b> {requested_by} (<code>{requested_by_id}</code>)"

loop_usage: "{emoji:🔁} <b>Loop Control</b>\n\nUsage: {cmd} [count]\n• 0 - Disable loop\n• 1-10 - Loop count\n\n<i>{count_line}</i>"
loop_invalid: "{emoji:⚠️} <b>Invalid loop count.</b>\nUse 0 to disable or 1-10 to set loop count."
loop_already_set: "{emoji:⚠️} Loop count is already set to <b>{count}</b> time(s)."
loop_disabled: "{emoji:🔁} Loop has been <b>disabled</b> by {user}"
loop_set: "{emoji:🔁} Set to loop <b>{count}</b> time(s)\n└ Changed by: {user}"
loop_current: "• Current loop: <b>{count}</b> time(s)"

# Logs module
logs_empty: "No logs found"

maint: "⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later."
maint_title: "{emoji:🛠️} Maintenance"
maint_desc: "When enabled, all active rooms are stopped and only the owner/sudo users can use the bot."
only_owner_cb: "⚠️ Only the bot owner can use this command."

# ⚙️ Maintenance control

move_usage: "{emoji:⚠️} Usage: <code>{cmd} [from] [to]</code>"
move_invalid_numbers: "{emoji:⚠️} Invalid numbers. Example: <code>/{cmd} 2 1</code>"
move_invalid_indexes_min: "{emoji:⚠️} Indexes must be greater than 0."
move_invalid_indexes_max: "{emoji:⚠️} Queue has only {queue_len} tracks."
move_success: "{emoji:✅} Moved track from position {from} to {to} by {user}."

# 🔇 Mute messages
mute_already_muted: |
  <h3>{emoji:🔇} Already Muted</h3>
  <p>The music is already muted in this chat.</p>
  <p>Would you like to unmute it?</p>
mute_failed: |
  <h3>{emoji:❌} Playback Mute Failed</h3>
  <p><b>Error:</b> <code>{error}</code></p>
mute_success: |
  <h3>{emoji:🔇} Muted playback</h3>
  <table bordered striped compact>
  <tr><td>{emoji:🎵} Track</td><td><a href="{url}">{title}</a></td></tr>
  <tr><td>{emoji:👤} Muted by</td><td>{user}</td></tr>
  {speed_line}
  </table>

no_song_query: |
  <b>{emoji:🎵} Whoops! Nothing to play.</b>

  <i>Send <b>{cmd} [song name or link]</b>, or reply to a <b>media</b> message to start the music.</i>

no_song_found: "{emoji:❌} No tracks found."

nothumb_title: "{emoji:🖼️} Thumbnails"
nothumb_desc: "When enabled, playback messages will not show artwork/thumbnails."

  

cmddelete_title: "{emoji:🗑️} Command Delete"
cmddelete_desc: "When enabled, I will automatically delete command messages after handling."

only_admin: "<h4>{emoji:🚫} Access Denied</h4>\n<aside>{emoji:🔒} This action is limited to <b>chat admins</b>.</aside>\n<p>You can still <code>/play</code> songs and enjoy the music.</p>\n<footer><i>{emoji:💡} Admin? Run <code>/reload</code> to refresh permissions.</i></footer>"
only_admin_cb: "⚠️ Only admins can do these actions."
only_admin_or_auth: "<h4>{emoji:🚫} Access Denied</h4>\n<aside>{emoji:🔒} This action requires <b>admin</b> or <b>authorized user</b> rights.</aside>\n<p>You can still <code>/play</code> songs and enjoy the music.</p>"
only_admin_or_auth_cb: "⚠️ Only admins or authorized users can do these actions."
only_supergroup: |
  <h2>⚠️ Supergroup Required</h2>

  This feature only works in a <b>Supergroup</b> for full functionality.

  <aside>Convert this group to a Supergroup, then add me again as an admin to continue.</aside>

  <p><tg-button type="url" style="primary" url="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">📖 How to Convert a Group to Supergroup</tg-button></p>

  <footer>Need help? Join our support group.</footer>
  <p><tg-button type="url" url="{support_group}">💬 Support Group</tg-button></p>
only_owner: "⚠️ Only the bot owner can use this command."
only_sudo: "{emoji:⚠️} Only sudo users or the bot owner can use this command."
only_sudo_cb: "⚠️ Only sudo users or the bot owner can use this command."

pause_already: |
  <h3>{emoji:⏸️} Already Paused</h3>
  <p>The music is already paused.</p>

pause_success: |
  <h3>{emoji:⏸️} Paused playback</h3>
  <table bordered striped compact>
  <tr><td>{emoji:🎵} Track</td><td><a href="{url}">{title}</a></td></tr>
  <tr><td>{emoji:⏱️} Position</td><td>{position} / {duration}</td></tr>
  <tr><td>{emoji:👤} Paused by</td><td>{user}</td></tr>
  {speed_line}
  </table>

play_single_track_too_long: |
  {emoji:⚠️} You can't play songs longer than {limit_mins} minutes.
  <i>{title}</i> was skipped.

play_download_failed: "{emoji:❌} Download failed. Please try again."

play_added_to_queue_single: |
  <b>{emoji:🎵} Added to Queue:</b> #{index}

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Requested by:</b> {by}

play_multiple_tracks_too_long_header: "<b>{emoji:⚠️} {count} tracks were skipped (max duration {limit_mins} mins):</b>"
play_multiple_tracks_too_long_item: "— <i>{title}</i>"
play_multiple_tracks_too_long_more: "... and {remaining} more."
play_all_tracks_skipped: "{emoji:❌} All found tracks were skipped due to duration limits."
play_failed: "{emoji:❌} Failed to play\nError: {error}"
play_unable: "<b> {emoji:❌} Unable to Play </b>\n\n<i>Please <b>restart the voice</b> chat.</i>"
rtmp_play_unsupported: "{emoji:⚠️} <b>Can't play in an RTMP/live-stream voice chat.</b>\n\n<i>Playback stopped.</i>"
play_downloading_song: "{emoji:📥} Downloading song \"{title}\""
play_download_canceled: "{emoji:⚠️} Download canceled by {user}."
play_already_in_queue: "{emoji:✅} <b>Track already added!</b>\n\n<i>{title}</i> is already playing or in the queue."
play_already_playing: "{emoji:⚠️} <b>A song is already playing.</b>\n\nPlease wait for it to finish or use <code>/stop</code> to stop it first."
play_added_multiple_header: "{emoji:➕} <b>Added {count} tracks</b> by {user}"
play_queue_limit_hint: "{emoji:⚠️} <i>Queue limit reached — some tracks were skipped.</i>"
play_queue_view_hint: "<i>Use /queue to view full list.</i>"

playmode_title: "{emoji:🎵} Play Mode"
playmode_desc: "When enabled, only admins and authorized users can use /play."
playmode_status_admins: "Admins 🔐"
playmode_status_everyone: "Everyone 🌍"
playmode_restricted: "{emoji:🚫} Play mode is enabled. Only admins and authorized users can play."

# 🏓 Ping messages
ping_start: "{emoji:🏓} Pinging..."
ping_result: |
  <b>{emoji:🏓} Pong!</b> • <code>{latency}ms</code>

  <b>{emoji:🤖} {bot}</b>
  <blockquote>{emoji:⏱️} <b>Uptime:</b> <code>{uptime}</code>
  {emoji:💾} <b>RAM:</b> <code>{ram_info}</code>
  {emoji:🖥️} <b>CPU:</b> <code>{cpu_usage}</code>
  {emoji:💽} <b>Disk:</b> <code>{disk_usage}</code></blockquote>
  

position_now: |
  <h3>{emoji:⏱️} Playback Position</h3>
  <aside>
  {track}
  </aside>
  <blockquote>
  {emoji:⏱️} <b>Position:</b> <code>{position} / {duration}</code><br>
  {emoji:⏳} <b>Remaining:</b> <code>{remaining}</code><br>
  {emoji:⚡} <b>Speed:</b> <code>{speed}x</code><br>
  {emoji:👤} <b>Requested by:</b> {requester}
  </blockquote>

queue_limit_reached: "{emoji:⚠️} Queue limit reached ({limit} tracks max). Use /clear to clear queue."
queue_no_active: "{emoji:⚠️} <b>No active playback.</b>\n\n<i>Nothing is queued right now.</i>"
queue_header: "{emoji:🎶} Current Queue"
queue_now_playing: "{emoji:▶️} Now Playing"
queue_up_next: "{emoji:⏭️} Up Next • {count} tracks"
queue_more_button: "{emoji:➕} Show {count} more"
queue_empty_tail: "{emoji:📭} <i>No more songs in queue.</i>"
queue_empty: "{emoji:⚠️} <b>The queue is already empty.</b>"

# 🔄 Reload
reload_start: "{emoji:⚙️} Reloading admin cache, voice chat status, and assistant status..."

reload_done: "<b>{emoji:⚙️} Reload complete:</b>\n\n{summary}"

remove_usage: "{emoji:⚠️} <b>Invalid usage.</b>\nUsage: <code>{cmd} [index]</code>"
remove_invalid_index: "{emoji:⚠️} <b>Invalid index.</b> It must be a <b>number</b>."
remove_index_too_small: "{emoji:⚠️} <b>Index must be greater than 0.</b>"
remove_index_too_big: "{emoji:⚠️} Invalid index. Queue only has <b>{total}</b> tracks."
remove_success: "{emoji:✅} Removed track at position <b>{index}</b> from the queue.\n{emoji:👤} By: {user}"
queue_delete_success: "🗑️ Removed from queue."

# 🔁 Replay
replay_failed: "❌ <b>Replay Failed</b>\nError: <code>{error}</code>"
replay_success: |
  {emoji:🔁} <b>Now replaying</b>

  {emoji:🎵} Title: {title}
  {emoji:⏱️} Duration: <code>{duration}</code>
  {emoji:⚙️} Speed: <b>{speed}x</b>

# ▶️ Resume
resume_already_playing: |
  <h3>ℹ️ Already Playing</h3>
  <p>The music is already playing in this chat.</p>
  <p>Would you like to pause it?</p>

resume_failed: |
  <h3>{emoji:❌} Playback Resume Failed</h3>
  <p><b>Error:</b> <code>{error}</code></p>

resume_success: |
  <h3>{emoji:▶️} Resuming playback</h3>
  <table bordered striped compact>
  <tr><td>{emoji:🎵} Title</td><td><a href="{url}">{title}</a></td></tr>
  <tr><td>{emoji:📍} Position</td><td>{position} / {duration}</td></tr>
  <tr><td>{emoji:👤} Resumed by</td><td>{user}</td></tr>
  {speed_line}
  </table>

restart: "{emoji:♻️} Restarting..."
restart_exepath_fail: "{emoji:❌} Failed to get executable path: {error}"
restart_symlink_fail: "{emoji:❌} Failed to resolve executable path: {error}"
restart_service: "{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds."
restart_initiated: "{emoji:✅} Restart initiated successfully!"
restart_fail: "{emoji:❌} Failed to restart: {error}"
restart_confirm_running: "{emoji:⚠️} A song is currently playing in this chat.\nChoose an action below."
restart_btn_bot: "Restart the Bot"
restart_btn_replay: "Replay the Song"
restart_confirm_bot: "♻️ Restarting the bot..."
restart_confirm_replay_done: "⏮️ Current song replayed successfully."

room_not_active_cb: "⚠️ Nothing playing right now."
room_no_active: "⚠️ Bot is not streaming in a voice chat. Start playing a song first."

room_already_paused: "⏸️ Track is already paused. Tap ▶️ Resume to continue."
room_pause_failed: |
  <h3>❌ Failed to pause playback</h3>
  <p><i>{error}</i></p>

searching: "{emoji:🔍}{emoji:🎶} Searching... {emoji:⚡}{emoji:✨}"
searching_query: "{emoji:🔍}{emoji:🎶} Searching for: <b>{query}</b>... {emoji:⚡}{emoji:✨}"

settings_btn_adminmode: "🛡️ Admin Mode"
settings_btn_cmddelete: "🗑️ Cmd Delete"
settings_btn_lang: "🌐 Language"
settings_btn_nothumb: "🖼️ Thumbnails"
settings_btn_playmode: "🎵 Play Mode"

settings_info_adminmode: |
  🛡️ What is this?

  Controls who can use admin commands of the bot (skip, pause, etc.).
  
settings_info_cmddelete: |
  🗑️ What is this?

  When enabled, the bot will automatically delete command messages after processing them to keep the chat clean.

settings_btn_cleanduration: "⏱️ Clean Delay"
settings_btn_cleanmode: "🧹 Clean Mode"
settings_info_cleanduration: |
  Cycles auto-delete delay: 15m → 30m → 60m → 5m.

settings_info_cleanmode: |
  Automatically removes command and bot service messages after a short delay.
  Playing status messages are kept until they naturally expire.

settings_info_lang: |
  🌐 What is this?

  Select the language for the bot's messages and interface in this group.

settings_info_nothumb: |
  🖼️ What is this?

  Toggle whether to show track artwork/thumbnails in playback messages.

settings_info_playmode: |
  🎵 What is this?

  Control who can use the /play command. If enabled, only administrators and authorized users can start playback.

settings_main: |
  <h2>{emoji:⚙️} Chat Settings</h2>
  <p><b>Title:</b> {title}<br/><b>ChatID:</b> <code>{id}</code></p>
  <p>Configure the bot's behavior in this group using the buttons below.</p>

settings_updated: "✅ Settings updated!"

autoplay_title: "{emoji:🔁} Autoplay"
autoplay_desc: "Once the queue ends, the bot automatically keeps playing recommended songs similar to the last played track."
autoplay_requester: "Autoplay"

speed_line: "{emoji:⚙️} Speed: <b>{speed}x</b>"

# 👑 Sudo users

sudo_owner_self: "{emoji:😂} Haha, you’re the boss already! Why are you even trying to add yourself?"
sudo_bot_self: "{emoji:🤖} Haha, I’m the bot! I can’t add myself to the sudo list — even the owner can’t cheat me {emoji:😎}"
sudo_bot_user: "{emoji:🤖} You can’t add a bot to the sudo list — sudoers must be human!"

sudo_fetch_user_fail: "{emoji:❌} Failed to fetch user info. Maybe the user is inaccessible.\n<i>{error}</i>"
sudo_check_fail: "{emoji:❌} Failed to check sudo existence: <i>{error}</i>"
sudo_already: "{emoji:⚠️} User {user} (ID:<code>{id}</code>) is already a sudoer."
sudo_add_fail: "{emoji:❌} Failed to add sudo: <i>{error}</i>"
sudo_add_success: "{emoji:✅} Added {user} (<code>{id}</code>) to sudoers."

sudo_owner_remove_block: "{emoji:😎} Nice try! You can’t remove the owner — they’re untouchable."
sudo_not_exists: "{emoji:⚠️} User {user} (<code>{id}</code>) is not a sudoer."
sudo_remove_fail: "{emoji:❌} Failed to remove sudo: <i>{error}</i>"
sudo_remove_success: "{emoji:🗑️} Removed {user} (ID: <code>{id}</code>) from sudoers."

sudo_list_fetching: "{emoji:⏳} Fetching sudoers list..."
sudo_list_fetch_fail: "{emoji:❌} Failed to get sudoers: <code>{error}</code>"
sudo_list_header: "{emoji:👑} <b>Current Sudoers:</b>"
sudo_list_owner: "{index}. {user} — <b>Owner</b>"
sudo_list_entry: "{index}. {user}"
sudo_list_no_extra: "{emoji:⚠️} No additional sudoers found."

seek_no_active: "{emoji:⚠️} <b>No track is currently playing.</b>"
seek_usage: "{emoji:⚠️} Please provide seconds.\nExample: <code>{cmd} 40</code>"
seek_invalid_seconds: "{emoji:⚠️} Invalid seconds value.\nExample: <code>{cmd} 40</code>"
seek_too_close_start: "{emoji:⚠️} Cannot seek backward <b>{seconds}</b> seconds — that would be too close to the beginning of the track."
seek_too_close_end: "{emoji:⚠️} Cannot seek forward <b>{seconds}</b> seconds — that would be too close to the end of the track."
seek_failed: "{emoji:❌} Failed to seek {direction} <b>{seconds}</b> seconds.\nError: <code>{error}</code>"
seek_success: "{emoji} Jumped {direction} to <b>{position}</b> of <b>{duration}</b>."

stop_confirm_paused: |
  {emoji:⏸️} <b>Playback is currently paused.</b>

  Do you want to resume playing or stop it completely?

stop_confirm_muted: |
  {emoji:🔇} <b>Playback is currently muted.</b>

  Do you want to unmute or stop it completely?

stopped: "{emoji:⏹️} <b>Playback stopped and cleared by {user}.</b>"

shuffle_current_state: "{emoji:🔀} Currently shuffle is <b>{state}</b> for this chat.\n\nUse <code>{cmd}</code> to toggle it."
shuffle_updated: "{emoji:🔀} Shuffle <b>{state}</b> by {user}."

skip_stopped: "{emoji:⏹️} Playback stopped. Queue is empty.\nSkipped by: {user}"
skip_invalid_number: "{emoji:⚠️} Please provide a valid number after /skip.\nExample: <code>/skip 3</code>"
skip_queue_empty_for_count: "{emoji:⚠️} Queue has no upcoming tracks to skip."
skip_count_exceeds_queue: "{emoji:⚠️} You requested to skip <code>{requested}</code> queued tracks, but only <code>{available}</code> track(s) are in queue."

speed_usage: |
  {emoji:💡} <b>Usage:</b>
  <code>{cmd} [speed]</code> — change playback speed
  
  Allowed speed range: <b>0.50x → 4.00x</b>
  
  Example:
  <code>{cmd} 1.5</code> — play at 1.5x
  <code>{cmd} normal</code> — reset to normal speed

speed_current: "{emoji:🎧} <b>Current Speed:</b> <code>{speed}x</code> for {emoji:🎵} <u>{title}</u>\n\n{emoji:💡} Use <code>{cmd} reset</code> to restore normal playback."

speed_invalid_value: "{emoji:❌} Invalid speed value.\nExample: <code>{cmd} 1.5</code> or <code>{cmd} 2x</code>"
speed_invalid_range: "{emoji:⚠️} Speed must be between <b>0.50x</b> and <b>4.00x</b>."

speed_already_set: "ℹ️ Playback speed is already <b>{speed}x</b> for {emoji:🎵} <b>{title}</b>"

speed_failed: |
  {emoji:❌} Failed to change speed to <b>{speed}x</b>.
  Error: <code>{error}</code>

speed_reset_success: "{emoji:✅} Playback speed reset to <b>1.0x</b> by {user}"

speed_set: "{emoji:🚀} Playback speed set to <b>{speed}x</b> by {user}."

start_private: |
  <h3>{emoji:🎵} {bot}</h3>
  <p>Hey {user} — I stream music in your Telegram voice &amp; video chats from YouTube, Spotify, SoundCloud, Apple Music and more. Queue, loop, shuffle, seek and control speed with a live queue.</p>
  <p><b>Enjoy high-quality audio, fast downloads and smooth playback everywhere.</b></p>
  <footer><i>Tap the buttons below to add me or explore commands.</i></footer>

start_group: |
  <h2>{bot}</h2>
  <p>I stream music in your Telegram voice &amp; video chats from YouTube, Spotify, SoundCloud, Apple Music and more — with queue, loop, shuffle, seek and speed control.</p>
  <aside>Try a command… if you think you can handle me {emoji:😉}</aside>
  <tg-button-row align="center">
    <tg-button type="url" style="primary" url="{support_channel}">Updates Channel</tg-button>
    <tg-button type="url" style="link" url="{support_chat}">Support Group</tg-button>
  </tg-button-row>

stream_queue_finished: "{emoji:🎵} <b>Queue Finished!</b>\n\nNo more songs to play. Use /play to add more tracks! {emoji:🎶}"
stream_downloading_next: "{emoji:📥} Downloading your next track..."
stream_autoplay_next: "{emoji:🔁} Autoplay: <b>{title}</b>"
stream_download_fail: "{emoji:❌} Failed to download.\nError: <code>{error}</code>"
download_progress: |
  {emoji:📥} Downloading your track...

  <b>Progress:</b> {percentage}%
  <b>Speed:</b> {speed}
  <b>ETA:</b> {eta}
  <b>Elapsed:</b> {elapsed}
stream_play_fail: "{emoji:❌} Failed to play the song."
stream_now_playing: |
  <b>{emoji:🎵} Now Playing:</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Duration:</b> {duration}
  <b>▫ Requested by:</b> {by}

# 👑 Sudo / Owner Join Messages

supergroup_needed: |
  <h2>⚠️ Supergroup Required</h2>

  This chat (<code>{chat_id}</code>) is a <b>regular group</b>. I only work in <b>Supergroups</b> for full functionality.

  <aside>Convert this group to a Supergroup, then add me again as an admin to continue.</aside>

  <p><tg-button type="url" style="primary" url="https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02">📖 How to Convert a Group to Supergroup</tg-button></p>

  <footer>Need help? Join our support group.</footer>
  <p><tg-button type="url" url="{support_group}">💬 Support Group</tg-button></p>

unmute_already: |
  <h3>ℹ️ Already Unmuted</h3>
  <p>The music is not muted in this chat.</p>
  <p>Would you like to mute it?</p>
unmute_failed: |
  <h3>{emoji:❌} Playback Unmute Failed</h3>
  <p><b>Error:</b> <code>{error}</code></p>
unmute_success: |
  <h3>{emoji:🔊} Unmuted playback</h3>
  <table bordered striped compact>
  <tr><td>{emoji:🎵} Track</td><td><a href="{url}">{title}</a></td></tr>
  <tr><td>{emoji:👤} Unmuted by</td><td>{user}</td></tr>
  {speed_line}
  </table>

# 🔊 Voice Chat Events
voicechat_ended: "<b>{emoji:⚠️} Video chat ended</b>\n\n<i>{emoji:⏰} Duration: {duration}</i>"
voicechat_started: "<b>{emoji:♻️} Voice chat started!</b>\n\n<i>Use /play [song name] to play music.</i>"

# Commands help
help_owner: |
  <h3>{emoji:👑} Owner Commands</h3>
  <p><i>Restricted, bot-wide administration reserved for the owner.</i></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/addsudo</code></td><td>Grant a user access to sudo-only commands — reply to them or pass an ID.</td></tr>
  <tr><td><code>/delsudo</code></td><td>Revoke a user's sudo access.</td></tr>
  <tr><td><code>/blockuser</code></td><td>Stop a user from using the bot everywhere.</td></tr>
  <tr><td><code>/unblockuser</code></td><td>Restore a blocked user's access.</td></tr>
  <tr><td><code>/blockchat</code></td><td>Stop a chat from using the bot.</td></tr>
  <tr><td><code>/unblockchat</code></td><td>Restore a blocked chat's access.</td></tr>
  <tr><td><code>/blacklisted</code></td><td>List every blocked user and chat.</td></tr>
  <tr><td><code>/broadcast</code></td><td>Send a message to all served chats and users — reply to the message to broadcast.</td></tr>
  <tr><td><code>/maintenance</code></td><td>Pause the bot and block all non-owner commands.</td></tr>
  <tr><td><code>/restart</code></td><td>Restart the bot process.</td></tr>
  <tr><td><code>/sh</code></td><td>Run a shell command on the server.</td></tr>
  </table>
  <footer>Use /help &lt;command&gt; for details on any command.</footer>

help_sudo: |
  <h3>{emoji:⚡} Sudoer Commands</h3>
  <p><i>Bot-wide tools for monitoring, logging and automation, available to sudo users.</i></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/active</code></td><td>List every voice chat the bot is streaming in.</td></tr>
  <tr><td><code>/stats</code></td><td>Show detailed bot, system and database statistics.</td></tr>
  <tr><td><code>/logs</code></td><td>Download the current log file.</td></tr>
  <tr><td><code>/json</code></td><td>Show the raw JSON of a replied message.</td></tr>
  <tr><td><code>/autoleave</code></td><td>Automatically leave inactive chats.</td></tr>
  <tr><td><code>/logger</code></td><td>Log bot activity to the logger channel.</td></tr>
  </table>
  <footer>Use /help &lt;command&gt; for details on any command.</footer>

help_admin: |
  <h3>{emoji:🛠} Admin Commands</h3>
  <p><i>Full playback and configuration control for chat admins and authorized users.</i></p>

  <p><b>🎵 Playback</b></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/play</code></td><td>Play a song from a search query, a link or a replied-to file.</td></tr>
  <tr><td><code>/fplay</code></td><td>Force play — start immediately, skipping the queue.</td></tr>
  <tr><td><code>/vplay</code></td><td>Play the result as a video.</td></tr>
  <tr><td><code>/cplay</code></td><td>Play in the linked channel's voice chat.</td></tr>
  <tr><td><code>/pause</code></td><td>Pause the current track.</td></tr>
  <tr><td><code>/resume</code></td><td>Resume a paused track.</td></tr>
  <tr><td><code>/replay</code></td><td>Restart the current track from the beginning.</td></tr>
  <tr><td><code>/skip</code></td><td>Skip to the next track in the queue.</td></tr>
  <tr><td><code>/stop</code></td><td>Stop playback, clear the queue and leave the voice chat.</td></tr>
  <tr><td><code>/mute</code></td><td>Mute the assistant in the voice chat.</td></tr>
  <tr><td><code>/unmute</code></td><td>Unmute the assistant in the voice chat.</td></tr>
  </table>

  <p><b>📜 Queue</b></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/queue</code></td><td>Show every waiting track.</td></tr>
  <tr><td><code>/position</code></td><td>Show the current timestamp.</td></tr>
  <tr><td><code>/remove</code></td><td>Remove a specific track from the queue.</td></tr>
  <tr><td><code>/move</code></td><td>Move a queued track to another position.</td></tr>
  <tr><td><code>/clear</code></td><td>Remove every track from the queue.</td></tr>
  <tr><td><code>/shuffle</code></td><td>Randomly reorder the queue.</td></tr>
  <tr><td><code>/loop</code></td><td>Repeat the current track 0–10 times.</td></tr>
  <tr><td><code>/jump</code></td><td>Skip straight to a chosen position in the track.</td></tr>
  </table>

  <p><b>⏩ Seek &amp; Speed</b></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/seek</code></td><td>Jump forward by the given number of seconds.</td></tr>
  <tr><td><code>/seekback</code></td><td>Jump backward by the given number of seconds.</td></tr>
  <tr><td><code>/speed</code></td><td>Change playback speed (0.5x–4.0x).</td></tr>
  </table>

  <p><b>⚙️ Settings &amp; Access</b></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/settings</code></td><td>Open the interactive settings panel.</td></tr>
  <tr><td><code>/playmode</code></td><td>Choose who may use /play.</td></tr>
  <tr><td><code>/adminmode</code></td><td>Choose who may use admin music commands.</td></tr>
  <tr><td><code>/cmddelete</code></td><td>Auto-delete command messages after they run.</td></tr>
  <tr><td><code>/autoplay</code></td><td>Play recommendations automatically when the queue ends.</td></tr>
  <tr><td><code>/nothumb</code></td><td>Hide artwork in playback messages.</td></tr>
  <tr><td><code>/cleanmode</code></td><td>Auto-delete the bot's replies in the chat.</td></tr>
  <tr><td><code>/channelplay</code></td><td>Link a channel for channel play mode.</td></tr>
  <tr><td><code>/auth</code></td><td>Authorize a user to control playback.</td></tr>
  <tr><td><code>/delauth</code></td><td>Remove a user's authorization.</td></tr>
  <tr><td><code>/reload</code></td><td>Refresh the admin cache and voice-chat state.</td></tr>
  </table>
  <footer>Channel variants (e.g. /cpause, /cskip) act on the linked channel's voice chat.</footer>

help_public: |
  <h3>{emoji:🌍} Public Commands</h3>
  <p><i>Everything you need to play music, follow the queue and check on the bot.</i></p>
  <table bordered striped compact>
  <tr><th>Command</th><th>Description</th></tr>
  <tr><td><code>/play</code></td><td>Play a song from a search query, a link or a replied-to file.</td></tr>
  <tr><td><code>/queue</code></td><td>View all tracks currently waiting in the queue.</td></tr>
  <tr><td><code>/position</code></td><td>See how far the current track has played.</td></tr>
  <tr><td><code>/ping</code></td><td>Check the bot's latency and system health.</td></tr>
  <tr><td><code>/reload</code></td><td>Refresh the admin cache and voice-chat state.</td></tr>
  <tr><td><code>/authlist</code></td><td>List the chat's authorized users.</td></tr>
  <tr><td><code>/sudolist</code></td><td>Show the bot's sudo users.</td></tr>
  <tr><td><code>/language</code></td><td>Change the bot's language.</td></tr>
  <tr><td><code>/privacy</code></td><td>Read the bot's privacy policy.</td></tr>
  <tr><td><code>/help</code></td><td>Open this help menu.</td></tr>
  <tr><td><code>/start</code></td><td>Start the bot in a private chat.</td></tr>
  </table>
  <footer>Use /help &lt;command&gt; for details on any command.</footer>

cleanmode_title: "{emoji:🧹} Clean Mode"
cleanmode_desc: "When enabled, bot service messages and command messages are auto-deleted after a short delay."
```


## Appendix D — VERBATIM: ArcMusic `internal/locales/en.yml`

```yaml
name: "🇺🇸 English"

# buttons
CONFIRM_RESUME_BTN: "▶️ Resume"
CONFIRM_UNMUTE_BTN: "🔊 Unmute"
CONFIRM_STOP_BTN: "⏹️ Stop"
ADD_ME_BTN: "Add Me to Your group"
BROADCAST_CANCEL_BTN: "🚫 Cancel Broadcast"
CLOSE_BTN: "Close"
DOWNLOAD_CANCEL_BTN: "🚦 Cancel Downloading"
GC_HELP_BTN: "📒 Commands"

START_HELP_BTN: "Help & Commands"
SOURCE_BTN: "Source"
UPDATES_BTN: "Updates"
SUPPORT_BTN: "Support"

HELP_ADMINS_BTN: "🛠 Admins"
HELP_PUBLIC_BTN: "🌍 Public"
HELP_OWNER_BTN: "👑 Owner"
HELP_SUDOERS_BTN: "⚡ Sudoers"

BACK_BTN: "⬅️ Back"

# basically this string used in /command [bool]
invalid_bool: "⚠️ <b>Invalid value.</b>\nUse 'enable' or 'disable'."

active_chats_info: "📊 <b>Active Chats:</b>\n\nThere are {count} active chats across the bot system."
active_chats_info_with_broken: "📊 <b>Active Chats:</b>\n\nThere are {count} active chats across the bot system, but {broken} of them may have issues displaying correctly."


# 🚫 Assistant Restricted / Banned
assistant_restricted_warning: |
  ⚠️ <b>Assistant Restricted</b>

  The assistant {assistant} (ID: <code>{id}</code>) has been <b>banned</b> or restricted in this chat.
  I cannot play music, manage the queue, or clean up tracks while this continues.

  <i>Unban the assistant to restore all music features 🎵</i>

# 🧩 Common auth messages
auth_no_user: "⚠️ Please provide a user — use:\n{cmd} [user_id] or reply to a user's message."
user_extract_fail: "⚠️ Unable to get that user: {error}"
auth_limit_reached: "⚠️ This chat has reached the maximum authorized users limit ({limit}). Please remove someone before adding a new one."
already_authed: "⚠️ That user is already authorized — no need to add them again."

# 🤖 Add auth
cannot_authorize_user: "⚠️ This user cannot be authorized.\nThe bot owner, the bot itself, or yourself do not require authorization."
addauth_user_is_admin: "⚠️ This user is already a chat administrator. No authorization is required."
addauth_bot_user: "🤖 You can’t add a bot to the auth list — authusers must be human!"
addauth_add_fail: "❌ Failed to add authorized user: {error}"
addauth_success_with_count: "✅ Added {user}.\nNow {count}/{limit} authorized users."
addauth_success: "✅ Successfully added {user} to the authorized users list."

# 🤖 Bot Added to Chat
bot_added_normal: "🎵 Thanks for adding me! Use /play [song] to start playing music."

# 🛠️ Bot Added During Maintenance
bot_added_maintenance: "⚠️ I'm currently under maintenance and will be back soon. Thanks for your patience! ⏰"
maint_reason_generic: "<i>📝 Reason: {reason}</i>"

# ⚠️ Bot Demotion
bot_demotion_goodbye: |
  <b>⚠️ Permission Lost</b>

  I’ve been <b>demoted from admin</b> and can’t perform my tasks anymore.
  Without proper rights, I’m unable to function here, so I’ll be <i>leaving the chat now</i>.

  <b>Goodbye 👋</b>

broadcast_already_running: "⚠️ A broadcast is already running.\n\nPlease wait for it to complete or cancel it first using:\n• <code>/broadcast -cancel</code>\n• Or click the Cancel button on the progress message"
broadcast_parse_failed: "❌ Failed to parse broadcast command:\n<code>{error}</code>"
broadcast_no_content: "⚠️ No content provided for broadcast.\n\n<b>Usage:</b> <code>{cmd} [text or reply to message]</code>\n\n<b>Example:</b>\n<code>{cmd} Hello everyone!</code>"
broadcast_fetch_chats_failed: "❌ Failed to fetch served chats:\n<code>{error}</code>"
broadcast_fetch_users_failed: "❌ Failed to fetch served users:\n<code>{error}</code>"
broadcast_no_targets: "⚠️ No targets found for broadcast.\n\nMake sure you haven't excluded all targets with flags."
broadcast_initializing: "📡 Initializing broadcast...\n\nPlease wait..."
broadcast_cancelled_header: "🚫 Broadcast Cancelled"
broadcast_completed_header: "✅ Broadcast Completed"
broadcast_progress_header: "📡 Broadcasting..."
broadcast_total_chats: "📊 Total Chats: {done}/{total} ({progress}%)"
broadcast_total_users: "👥 Total Users: {done}/{total} ({progress}%)"
broadcast_failed_chats: "❌ Failed Chats: {count}"
broadcast_failed_users: "❌ Failed Users: {count}"
broadcast_delay: "⏱ Delay: {delay}s"
broadcast_elapsed: "⏰ Elapsed: {elapsed}"
broadcast_eta: "⏳ ETA: {eta}"
broadcast_success_rate: "✨ Success Rate: {rate}% ({sent}/{total})"
broadcast_not_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_success: "🚫 Broadcast cancelled successfully."
broadcast_cancel_owner_only: "⚠️ Only the owner can cancel broadcasts."
broadcast_cancel_none_running: "ℹ️ No broadcast is currently running."
broadcast_cancel_done: "🚫 Broadcast cancelled."

# 🎧 CPlay (Channel Play)

cplay_id_not_set: "⚠️ cplay ID not set.\nUse <code>/cplay --set &lt;chat_id&gt;</code> to set it."
cplay_usage: "<b>Usage:</b> <code>/cplay --set &lt;channel_id&gt;</code>"
cplay_invalid_chat_id: "<b>Invalid Chat ID:</b> Please provide a valid integer ID for the channel."
cplay_resolve_peer_fail: "<b>Failed to resolve peer:</b> Could not fetch channel details. Ensure I can access this channel."
cplay_invalid_target: "<b>Invalid Target:</b> The provided ID is not a valid channel."
cplay_channel_not_accessible: "<b>Channel Not Accessible:</b> Could not retrieve channel information. Please ensure I am an administrator in the target channel."
cplay_save_error: "<b>Error:</b> Failed to save CPlay settings."
cplay_enabled: "✅ <b>Channel Play enabled.</b> All <code>/c</code> commands will now work in channel <code>{channel_id}</code>."

# Callback handler messages
invalid_request: "⚠️ Invalid request."
unknown_action: "⚠️ Unknown action."
download_cancelled: "✅ Download cancelled."
no_download_to_cancel: "ℹ️ No download to cancel."

# Pause callback
cb_pause_success: "⏸️ Track paused at {position}"
cb_pause_message: |
  <b>⏸️ Track Paused</b>

  <b>▫ Track:</b> <a href="{url}">{title}</a>
  <b>▫ Position:</b> {position} / {duration}
  <b>▫ Paused by:</b> {user}

# Resume callback
cb_already_playing: "ℹ️ Track is already playing."
cb_resume_failed: "❌ Failed to resume playback."
cb_resume_success: "▶️ Resumed at {position}."
cb_resume_message: |
  <b>🎵 Now Playing:</b>

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Resumed by:</b> {user}

# Replay callback
cb_replaying: "🔁 <b>Replaying current track...</b>"
cb_replay_failed: "❌ Failed to replay track."
cb_replay_success: "🔁 Track replayed."
cb_replay_edited: "🔁 Track replayed by {user}."

# Skip callback
cb_skip_queue_empty: "⏹️ Playback stopped — queue empty."
cb_skip_download_failed: "❌ Failed to download next track."
cb_skip_play_failed: "❌ Failed to play next track."
cb_skip_success: "⏭️ Track skipped."
cb_skip_edited: "⏭️ Skipped by {user}"

# Stop callback
cb_stop_success: "⏹️ Playback stopped."

# Mute/Unmute callback
cb_mute_success: "🔇 Muted."
cb_mute_message: |
  <b>🔇 Playback Muted</b>

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Muted by:</b> {user}

cb_unmute_success: "🔊 Unmuted."

cb_seek_success: "⏩ Sought forward {seconds} seconds"
cb_seek_near_end: "⚠️ Cannot seek forward {seconds} seconds — about to reach end."
cb_seek_edited: "⏩ Sought {seconds} seconds — by {user}"

cb_seekback_success: "⏪ Sought back {seconds} seconds"
cb_seekback_edited: "⏪ Sought back {seconds} seconds — by {user}"

# 🗑️ Delete auth
del_auth_not_authorized: "⚠️ That user isn’t authorized — nothing to remove."
del_auth_remove_fail: "❌ Failed to remove authorized user: <code>{error}</code>"
del_auth_success: "✅ Successfully removed {user} from the authorized users list."

# 📜 Auth list
authlist_fetch_fail: "❌ Failed to get authorized users list: <code>{error}</code>"
authlist_empty: "ℹ️ There are no authorized users in this chat."
authlist_fetching: "⏳ Fetching authorized users list..."
authlist_header: "<b>Authorized Users:</b>"
authlist_user_fail: "{index}. <code>{user_id}</code> (Could not fetch user info)"
authlist_user_entry: "{index}. {user} (<code>{user_id}</code>)"
authlist_total: "<b>Total:</b> {count} users"

autoleave_fetch_fail: "⚠️ <b>Failed to fetch AutoLeave state.</b>"
autoleave_status: "🏃‍♂️ <b>AutoLeave Control</b>\n\nWhen enabled, the assistant will automatically leave the chat.\nUsage: {cmd} [enable|disable]\nCurrent state: {action}"
autoleave_already: "⚠️ AutoLeave is already {action}"
autoleave_update_fail: "⚠️ <b>Failed to update AutoLeave state.</b>"
autoleave_updated: "🏃‍♂️ AutoLeave has been {action}"

enabled: "Enabled"
disabled: "Disabled"

bug_usasge: "⚠️ Please describe the bug. Usage: {cmd} <problem> or reply to a message."
bug_thanks: "✅ Thank you for your bug report! It has been sent to the developers."

bug_report_format: |
  <b>🐞 New Bug Report</b>

  <b>From:</b> {user} (<code>{user_id}</code>)
  <b>Chat:</b> {chat} (<code>{chat_id}</code>)

  <b>Report:</b>
  <pre>{report}</pre>
  
clear_no_active: "⚠️ <b>No active playback or queue to clear.</b>"
clear_success: "✅ The queue has been cleared by {user}.\n<i>You can restore it using <code>/{cmd}</code> before adding new songs</i>"

err_assistant_banned: |
  <b>🚫 Assistant Restricted</b>

  I can't play music because {user} (UserID: <code>{id}</code>) is <b>banned</b> or <b>removed</b> from this chat.

  ✅ Unbanned them already? Use /reload to refresh.

err_admin_permission_required: |
  ⚠️ <b>Admin Permission Required</b>

  I need admin access to manage and check members in this chat.
  
  ➤ <b>Promote me with</b> <code>Manage Chat / Invite Users</code> permission.

err_assistant_get_failed: |
  ⚠️ The system could not assign an assistant to this chat.

  This is usually a temporary issue and should resolve by trying again.

  <b>➡️ Please retry your command.</b>

  If this keeps happening, please contact support so we can assist you.
  
err_assistant_join_rate_limited: |
  ⚠️ Assistant cannot join this chat because it has reached the maximum number of allowed groups.

err_assistant_join_request_sent: |
  ⚠️ Assistant sent a join request, but I could not auto-approve it.

  ✅ Please approve the request manually, then try again.

err_assistant_invite_link_fetch: |
  ⚠️ Failed to fetch invite link:
  {error}

err_assistant_invite_failed: |
  ⚠️ Assistant failed to join this chat:
  {error}

err_connection_timeout: |
  ⚠️ <b>VoiceChat Connection Timeout</b>

  Telegram is not responding properly to the voice chat connection right now.

  If this keeps happening, end the voice chat and start it again, then retry.

err_no_active_voicechat: |
  🎙️ <b>No Active Voice Chat</b>

  I can't join — please start a voice chat first.

  Already started one? Use /reload to refresh.

err_fetch_failed: |
  ⚠️ Failed to fetch chat info:
  {error}

err_peer_resolve_failed: |
  ⚠️ Failed to resolve peer information. Try again later or re-add the assistant.

err_rtmp_missing_params: |
  <b>⚠️ RTMP Details Missing</b>

  It looks like an <b>RTMP stream</b> is active in this chat, but you have not provided the required
  <b>RTMP URL</b> or <b>Stream Key</b>. Without these, I cannot start streaming music to RTMP.

  <b>Use:</b> <code>/setrtmp [chat_id] [url+key]</code>

  📌 Once the RTMP details are set, you may start streaming.  
  ➡️ If you do not wish to use RTMP, switch to regular voice chat instead.
  
err_unknown: |
  ⚠️ Unknown Error Occurred:
  {error}

err_fetch_tracks: |
  ⚠️ <b>Failed to fetch track details.</b>
  Please try again later.

err_download_internal: |
  ⚠️ <b>Download failed</b> due to an unexpected internal error.
  Please try again later.
  
flood_seconds: "⏳ Slow down! Try again in {duration} seconds."
flood_minutes: "⏳ Please wait {duration} minutes before using this command again."

help_private_only: "🤖 Hi! For bot help and commands, please DM me directly — I'm more responsive in private chats!"
help_main: "📚 <b>Help Menu</b>\n\nSelect a category below to explore detailed commands and their usage for managing, controlling, and customizing the bot."

jump_no_active: "⚠️ No active track to jump within."
jump_usage: "⚠️ Please provide seconds.\nExample: <code>{cmd} 120</code>"
jump_invalid_position: "⚠️ Invalid position.\nExample: <code>{cmd} 120</code>"
jump_too_close_end: "⚠️ Cannot jump to <b>{position}</b> — that’s too close to the end of the track."
jump_failed: "❌ Failed to jump to <b>{position}</b>.\nError: <code>{error}</code>"
jump_success: "⏩ Jumped to <u>{position}</u> of <u>{duration}</u>."

lang_same: "ℹ️ You’re already using this language."
lang_select: "🌐 Select your preferred language:"
lang_fail: "❌ Failed to update language. Please try again later."
lang_success: "✅ Language successfully updated to {lang_name}."

logger_playback_started: "Playback Started"
logger_playback_queued: "Playback Queued"

logger_bot_added: |
  🤖 <b>Bot Added to New Group</b>
  
  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Added By:</b> {added_by_name}
  <b>User ID:</b> {added_by_id}
  <b>User Username:</b> {added_by_username}


logger_bot_removed: |

  🤖 <b>Bot Removed from Group</b>

  <b>Group Name:</b> {group_name}
  <b>Group ID:</b> {group_id}
  <b>Group Username:</b> {group_username}

  <b>Removed By:</b> {removed_by_name}
  <b>User ID:</b> {removed_by_id}
  <b>User Username:</b> {removed_by_username}

  
logger_bot_started: |
  ▶️ <b>{mention} started the bot</b>

  <b>Username:</b> {user_username}
  <b>ID:</b> {user_id}
  
logger_track: "Track:"
logger_source: "Source:"
logger_group: "Group:"
logger_requested_by: "Requested by:"
logger_timestamp: "Timestamp:"

loop_usage: "🔁 <b>Loop Control</b>\n\nUsage: {cmd} [count]\n• 0 - Disable loop\n• 1-10 - Loop count\n\n<i>{count_line}</i>"
loop_invalid: "⚠️ <b>Invalid loop count.</b>\nUse 0 to disable or 1-10 to set loop count."
loop_already_set: "⚠️ Loop count is already set to <b>{count}</b> time(s)."
loop_disabled: "🔁 Loop has been <b>disabled</b> by {user}"
loop_set: "🔁 Set to loop <b>{count}</b> time(s)\n└ Changed by: {user}"
loop_current: "• Current loop: <b>{count}</b> time(s)"

logger_usage: "⚙️ Usage: <code>{cmd} [enable|disable]</code> - To enable or disable the logger\n\n{status}"
logger_status: "📜 Current status: {action}"
logger_check_fail: "❌ Failed to check logger status: {error}"
logger_already: "ℹ️ Logger is already {action}."
logger_update_fail: "❌ Failed to update logger setting: {error}"
logger_updated: "✅ Logger has been {action} successfully."

# Logs module
logs_invalid_number: "⚠️ <b>Invalid Number</b>\n\nThe value <code>{value}</code> is not a valid positive number.\n\n<b>Usage:</b> <code>/logs [number]</code> or <code>/logs -old [number]</code>"
logs_invalid_flag: "⚠️ <b>Invalid Flag</b>\n\nUnknown flag or invalid input: <code>{flag}</code>\n\n<b>Valid options:</b>\n• <code>/logs</code> - Full log\n• <code>/logs [n]</code> - Last N lines\n• <code>/logs -old [n]</code> - Oldest log\n• <code>/logs -clear</code> - Clear logs"
logs_file_error: "❌ <b>Log File Error</b>\n\nAn error occurred while accessing the log file:\n<code>{error}</code>"
logs_send_error: "❌ <b>Log Send Failed</b>\n\nUnable to prepare or send the log file.\n<code>{error}</code>"
logs_clear_failed: "❌ <b>Clear Failed</b>\n\nFailed to clear log file:\n<code>{error}</code>"
logs_empty: "📭 <b>No Logs Available</b>\n\nThe log file either doesn’t exist yet or exists but contains no data.\n\nRun the bot for a while to generate logs."
logs_preview_header: "📋 <b>Current Log Preview:</b>"
logs_lines_header: "📋 <b>Recent Log Lines</b> (Last {count})"
logs_old_header: "📋 <b>Oldest Log Lines</b> (First {count})"
logs_uploading: "⏳ <b>Uploading log file...</b>\n\nPlease wait while the file is being prepared."
logs_file_caption: "📄 <b>Log File Details</b>\n\n<b>Filename:</b> <code>{filename}</code>\n<b>Size:</b> {size_mb} MB\n<b>Last Modified:</b> {modified}"
logs_lines_caption: "📋 <b>Recent Log Lines</b>\n\nShowing last {count} lines from current log."
logs_old_caption: "📋 <b>Oldest Log Lines</b>\n\nShowing first {count} lines from the beginning of the log."
logs_cleared: "✅ <b>Log File Cleared</b>\n\nSuccessfully cleared the log file.\n<b>Previous Size:</b> {size_mb} MB"

# reason - if maintenance reason available so at the place of {reason} the maint_reason will provided
maint: "⚠️ I'm under maintenance at the moment and temporarily unavailable. Please check back later.\n\n{reason}"
maint_reason: "<i>📝 Reason: {reason}</i>"

# ⚙️ Maintenance control
maint_usage: "⚙️ <b>Maintenance Control</b>\n\nUsage: {cmd} [enable|disable] [reason]\n📜 Current status: {status}"
maint_check_fail: "❌ Failed to check maintenance status: <code>{error}</code>"
maint_already_enabled: "ℹ️ Maintenance mode is already enabled 🟢."
maint_already_disabled: "ℹ️ Maintenance mode is already disabled 🔴."
maint_already_reason_same: "ℹ️ Maintenance is already enabled with the same reason."
maint_reason_removed: "✅ Maintenance reason removed successfully."
maint_reason_updated: "✅ Maintenance reason updated.\n📝 Reason: {reason}"
maint_enabled: "✅ Maintenance mode has been <b>enabled</b> successfully."
maint_enabled_reason: "✅ Maintenance mode enabled.\n📝 Reason: {reason}"
maint_disabled: "✅ Maintenance mode has been <b>disabled</b> successfully."
maint_entering: "⚠️ Bot is entering maintenance mode..."
enabled_with_reason: "🟢 Enabled\n📝 Reason: {reason}"

move_usage: "⚠️ Usage: <code>{cmd} [from] [to]</code>"
move_invalid_numbers: "⚠️ Invalid numbers. Example: <code>/{cmd} 2 1</code>"
move_invalid_indexes_min: "⚠️ Indexes must be greater than 0."
move_invalid_indexes_max: "⚠️ Queue has only {queue_len} tracks."
move_success: "✅ Moved track from position {from} to {to} by {user}."

# 🔇 Mute messages
mute_already_muted: "🔇 <b>Already Muted</b>\nThe music is already muted in this chat.\nWould you like to unmute it?"
mute_already_muted_with_time: "🔇 <b>Already Muted</b>\nThe music is muted.\nAuto-unmute in <b>{duration}</b>."
mute_invalid_duration: "⚠️ Invalid duration for auto-unmute.\nIt must be between <b>5</b> and <b>3600</b> seconds."
mute_invalid_format: "⚠️ Invalid format.\nUse: <code>/{cmd} 30</code> or <code>/{cmd} 30s</code>"
mute_failed: "❌ <b>Playback Mute Failed</b>\nError: <code>{error}</code>"
mute_success: |
  🔇 <b>Muted playback</b>

  🎵 Track: {title}
  👤 Muted by: {user}

mute_success_with_time: |
  🔇 <b>Muted playback</b>

  🎵 Track: {title}
  👤 Muted by: {user}
  ⏳ Auto-unmute in <b>{duration}</b> seconds

no_song_query: |
  <b>🎵 Whoops! No song detected.</b>

  Type <b>{cmd} [song name]</b> or reply to a <b>media</b> to start playing!

no_song_found: "❌ No tracks found."

nothumb_status: |
  🖼️ <b>Thumbnail Control</b>
  
  When enabled, playback messages won't show artwork/thumbnails.
  
  <b>📊 Usage: {cmd} [enable|disable]</b>
  
  Thumbnail is {action} currently.
  
nothumb_fetch_fail: "⚠️ <b>Failed to fetch thumbnail setting.</b>"
nothumb_already: "ℹ️ Thumbnail display is already {action}."
nothumb_update_fail: "⚠️ <b>Failed to update thumbnail setting.</b>"
nothumb_updated: "🖼️ Thumbnail display has been {action}."

cmddelete_status: "🗑️ <b>Command Delete Control</b>\n\nWhen enabled, I will automatically delete command messages.\nUsage: {cmd} [enable|disable]\nCurrent state: {action}"
cmddelete_updated: "🗑️ Command deletion has been {action}."

only_admin: "⚠️ <b>Permission Denied!</b>\nOnly <b>admins</b> can control this actions. You can still play songs 🎵.\n\nIf you believe you are an admin, please use /reload to refresh your admin status."
only_admin_or_auth: "⚠️ <b>Access Denied</b>\nOnly <b>admins</b> or <b>authorized users</b> can control this actions.\n\nIf you recently became an admin, use /reload to refresh your permissions."
only_admin_or_auth_cb: "⚠️ Only admins can do this actions."
only_supergroup: "⚠️ This command can only be used in groups."
only_owner: "⚠️ Only the bot owner can use this command."
only_sudo: "⚠️ Only sudo users or the bot owner can use this command."

pause_invalid_duration: "⚠️ Invalid duration for auto-resume.\nIt must be between <b>5</b> and <b>3600</b> seconds."
pause_invalid_format: "⚠️ Invalid format.\nUse: <code>/{cmd} 30</code> or <code>/{cmd} 30s</code>"
auto_resume_line: "\n<i>⏳ Auto-resume in <b>{seconds}</b> seconds</i>"

pause_already: |
  ⏸️ <b>Already Paused</b>
  The music is already paused.{auto_resume_line}

pause_success: |
  ⏸️ <b>Paused playback</b>

  🎵 Track: {title}
  ⏱️ Position: {position} / {duration}
  👤 Paused by: {user}
  {auto_resume_line}

play_single_track_too_long: |
  ⚠️ You can't play songs longer than {limit_mins} minutes.
  {title} was skipped.

play_download_failed: |
  ❌ Failed to download "<i>{title}</i>".
  Error: <code>{error}</code>

play_added_to_queue_single: |
  <b>Added to Queue:</b> #{index}

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Requested by:</b> {by}

channel_play_depreciated: "⚠️ This handler is deprecated. Use <code><a>/cplay --set channel_id </code></a> to set your channel for playback."
play_multiple_tracks_too_long_header: "<b>⚠️ {count} tracks were skipped (max duration {limit_mins} mins):</b>"
play_multiple_tracks_too_long_item: "— <i>{title}</i>"
play_multiple_tracks_too_long_more: "... and {remaining} more."
play_all_tracks_skipped: "❌ All found tracks were skipped due to duration limits."
play_failed: "❌ Failed to play\nError: {error}"
play_unable: "<b> ❌ Unable to Play </b>\n\n<i>Please <b>restart the voice</b> chat.</i>"
play_downloading_song: "📥 Downloading song \"{title}\""
play_download_canceled: "⚠️ Download canceled by {user}."
playmode_help: |
  🛠️ <b>Play Mode</b>

  <b>Status:</b> {status}
  <b>Usage:</b> <code>/playmode [enable|disable]</code>

playmode_status_admins: "<b>Admins Only</b> 🔐"
playmode_status_everyone: "<b>Everyone</b> 🌍"
playmode_success_admins: "🛡️ <b>Play mode</b> is now restricted to <b>Admins & DJs</b> only. 🎧"
playmode_success_everyone: "✅ <b>Play mode</b> is now <b>Open to Everyone</b>. Let the music flow! 🎶"
playmode_restricted: "🚫 Play mode is enabled. Only admins and authorized users can play."
play_added_multiple_header: "➕ <b>Added {count} tracks</b> by {user}"
play_queue_limit_hint: "⚠️ Queue limit reached — some tracks were skipped."
play_queue_view_hint: "Use /queue to view full list."

# 🏓 Ping messages
ping_start: "🏓 Pinging..."
ping_result: |
  <b>🏓 PONG</b>: <code>{latency}ms</code>
  
  <b><u>🤖 {bot} system stat's :</u></b>
  
  <b>Uptime:</b> {uptime}
  <b>RAM Usage:</b> {ram_info}
  <b>CPU Usage:</b> {cpu_usage}
  <b>Disk Usage:</b> {disk_usage}

position_now: |
  🎵 <b>{title}</b>
  📍 <code>{position} / {duration}</code>
  ⚙️ Speed: <b>{speed}x</b>

queue_limit_reached: "⚠️ Queue limit reached ({limit} tracks max). Use /clear to clear queue."
queue_no_active: "⚠️ <b>No active playback.</b>\n\nNothing is queued right now."
queue_header: "🎶 <b>Current Queue</b>"
queue_now_playing: "▶️ <b>Now Playing:</b>"
queue_up_next: "⏭️ <b>Up Next:</b>"
queue_more_line: "\n… and {remaining} more"
queue_empty_tail: "📭 No more songs in queue."
queue_empty: "⚠️ <b>The queue is already empty.</b>"
queue_restored: "✅ Successfully restored <b>{count}</b> tracks to the queue."
queue_restore_no_data: "❌ No cleared queue found to restore, or a new song has already been added."

# 🔄 Reload
reload_start: "⚙️ Reloading admin cache, voice chat status, and assistant status..."

reload_done: "<b>⚙️ Reload complete:</b>\n\n{summary}"

reload_admin_cache_ok: "<b>• Admin cache:</b> <i>✅ Done</i>"
reload_admin_cache_fail: "<b>• Admin cache:</b> <i>❌ ({error})</i>"

reload_voice_inactive: "<b>• Voice chat:</b> <i>⚪ Inactive</i>"
reload_voice_admin_required: "<b>• Voice chat:</b> <i>❌ Admin permission required</i>"
reload_voice_fail: "<b>• Voice chat:</b> <i>❌ ({error})</i>"
reload_voice_active: "<b>• Voice chat:</b> <i>✅ Active</i>"

reload_assistant_banned: "<b>• Assistant:</b> <i>❌ Banned</i>"
reload_assistant_admin_required: "<b>• Assistant:</b> <i>❌ Admin permission required</i>"
reload_assistant_fail: "<b>• Assistant:</b> <i>❌ ({error})</i>"
reload_assistant_present: "<b>• Assistant:</b> <i>✅ Present</i>"
reload_assistant_not_present: "<b>• Assistant:</b> <i>⚪ Not present</i>"
reload_room_reset: "<b>• Room:</b> <i>Reset ✅</i>"


remove_usage: "⚠️ <b>Invalid usage.</b>\nUsage: <code>{cmd} [index]</code>"
remove_invalid_index: "⚠️ <b>Invalid index.</b> It must be a <b>number</b>."
remove_index_too_small: "⚠️ <b>Index must be greater than 0.</b>"
remove_index_too_big: "⚠️ Invalid index. Queue only has <b>{total}</b> tracks."
remove_success: "✅ Removed track at position <b>{index}</b> from the queue.\n👤 By: {user}"

# 🔁 Replay
replay_failed: "❌ <b>Replay Failed</b>\nError: <code>{error}</code>"
replay_success: |
  🔁 <b>Now replaying</b>

  🎵 Title: {title}
  ⏱️ Duration: <code>{duration}</code>
  ⚙️ Speed: <b>{speed}x</b>

# ▶️ Resume
resume_already_playing: "ℹ️ <b>Already Playing</b>\nThe music is already playing in this chat.\nWould you like to pause it?"
resume_failed: "❌ <b>Playback Resume Failed</b>\nError: <code>{error}</code>"
resume_success: |
  ▶️ <b>Resuming playback</b>

  🎵 Title: {title}
  📍 Position: {position} / {duration}
  👤 Resumed by: {user}
  {speed_line}

restart: "♻️ Restarting..."
restart_exepath_fail: "❌ Failed to get executable path: {error}"
restart_symlink_fail: "❌ Failed to resolve executable path: {error}"
restart_service: "{bot} has just restarted. Sorry for the issues.\n\nStart playing again after 10–15 seconds."
restart_initiated: "✅ Restart initiated successfully!"
restart_fail: "❌ Failed to restart: {error}"


room_not_active_cb: "⚠️ Nothing playing right now."
room_no_active: "⚠️ <b>No active playback.</b>\n\n<i>There's nothing playing right now.</i>"
room_not_linked: "⚠️ This channel isn't linked to any group."

room_already_paused: "⏸️ Track is already paused. Tap ▶️ Resume to continue."
room_already_paused_auto: "⏸️ Track is already paused — auto-resuming in {duration}."
room_pause_failed: "❌ Failed to pause playback.\n\n<i>{error}</i>"

rtmp_not_configured: |
  ⚠️ <b>RTMP not configured!</b>
  
  Please set RTMP server first:
  <code>{cmd} rtmps://server/s/streamkey</code>

rtmp_already_streaming: |
  📡 <b>Already streaming!</b>
  
  Use <code>/streamstop</code> to end current stream.

rtmp_init_failed: |
  ❌ <b>Failed to initialize RTMP stream:</b>
  <code>{error}</code>

rtmp_starting_stream: "📡 <b>Starting RTMP stream...</b>"

rtmp_play_failed: |
  ❌ <b>Failed to start stream:</b>
  <code>{error}</code>

rtmp_now_streaming: |
  📡 <b>Now Streaming (RTMP):</b>
  
  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Started by:</b> {by}
  
  ⚠️ Note: RTMP has ~15-30s buffering delay

rtmp_not_streaming: "ℹ️ No active RTMP stream."

rtmp_stop_failed: |
  ❌ <b>Failed to stop stream:</b>
  <code>{error}</code>

rtmp_stopped: |
  ⏹️ <b>RTMP stream stopped</b> by {user}

rtmp_configured_not_started: |
  ℹ️ <b>RTMP Status:</b> Not started
  
  <b>▫ Server:</b> <code>{server}</code>
  <b>▫ State:</b> Ready to stream
  
  Use <code>/stream &lt;query&gt;</code> to start streaming.

rtmp_status_playing: |
  📡 <b>RTMP Stream Status</b>
  
  <b>State:</b> ▶️ Streaming
  <b>Position:</b> {position}
  <b>Server:</b> <code>{server}</code>

rtmp_status_paused: |
  📡 <b>RTMP Stream Status</b>
  
  <b>State:</b> ⏸️ Paused
  <b>Server:</b> <code>{server}</code>

rtmp_status_idle: |
  📡 <b>RTMP Stream Status</b>
  
  <b>State:</b> ⏹️ Idle
  <b>Server:</b> <code>{server}</code>

rtmp_dm_only: |
  🔒 <b>Security Notice</b>
  
  RTMP credentials should NEVER be shared in groups!
  
  <b>📩 To configure RTMP:</b>
  1. Open my private chat: @{bot_username}
  2. Send: <code>{cmd} &lt;chat_id&gt; &lt;rtmp_url&gt;</code>
  
  <b>💡 Example:</b>
  <code>/setrtmp -1001234567890 rtmps://server/s/key</code>

rtmp_setup_usage: |
  ⚙️ <b>RTMP Configuration</b>
  
  <u>Usage:</u>
  <code>/setrtmp &lt;chat_id&gt; &lt;full_rtmp_url&gt;</code>
  
  <b>📋 Parameters:</b>
  • <code>chat_id</code> - Target chat/channel ID
  • <code>full_rtmp_url</code> - Complete RTMP URL with stream key
  
  <b>📝 Example:</b>
  <code>/setrtmp -1001234567890 rtmps://dc5-1.rtmp.t.me/s/123:key</code>
  
  <b>🔍 How to get chat ID:</b>
  • Forward any message from chat to @userinfobot
  • Or use <code>/id</code> command in the chat
  
  <b>🔐 Security:</b>
  • Configuration only works in DM (private chat)
  • You must be admin in target chat
  • Credentials are stored securely

rtmp_invalid_chat_id: |
  ❌ <b>Invalid chat ID!</b>
  
  Please provide a valid numeric chat ID.
  
  <b>💡 Example:</b>
  <code>/setrtmp -1001234567890 rtmps://...</code>

rtmp_check_admin_failed: |
  ❌ <b>Failed to verify admin status:</b>
  <code>{error}</code>
  
  <b>⚠️ Make sure:</b>
  • I am admin in the target chat
  • Chat ID is correct
  • You have admin permissions

rtmp_not_admin: |
  ⚠️ <b>Permission Denied</b>
  
  You must be an <b>administrator</b> in the target chat to configure RTMP.

rtmp_parse_failed: |
  ❌ <b>Failed to parse RTMP URL:</b>
  <code>{error}</code>
  
  <b>📋 Correct format:</b>
  <code>rtmp://server/app/streamkey</code>
  or
  <code>rtmps://server/s/streamkey</code>

rtmp_streaming_not_supported: |
  ⚠️ <b>Streaming not supported</b>
  
  This action is not available while using <b>RTMP mode</b>.
  
  Use <code>/stream</code> to play media via RTMP.
  
rtmp_configured_success: |
  ✅ <b>RTMP Configured Successfully!</b>
  
  <b>🆔 Chat ID:</b> <code>{chat_id}</code>
  <b>🔗 URL:</b> <code>{url}</code>
  <b>🔑 Key:</b> <code>{key}</code>
  
  <b>✨ Next Steps:</b>
  Go to your chat and use:
  <code>/stream &lt;song name&gt;</code>
  
  <b>🔒 Security Note:</b>
  Credentials are stored securely and never shown in the group.


searching: "🔍 Searching..."
searching_query: "🔍 Searching for: <b>{query}</b>..."

speed_line: "⚙️ Speed: <b>{speed}x</b>"

stats_system_header: "🔧 <b>System:</b>"
stats_go_mem_header: "📦 <b>Internal Memory (Go):</b>"
stats_server_header: "💻 <b>Server Stats:</b>"
stats_served_header: "📊 <b>Served:</b>"

stats_system_os_arch: "• OS: <code>{os}</code>, Arch: <code>{arch}</code>"
stats_system_cpu_goroutines: "• CPUs: <code>{cpus}</code>, Goroutines: <code>{goroutines}</code>"

stats_go_alloc: "• Alloc: <code>{alloc} MB</code>"
stats_go_sys: "• Sys: <code>{sys} MB</code>"
stats_go_gc: "• NumGC: <code>{gc_count}</code> ({emoji} {gc_rate}/min)"

stats_server_cpu: "• CPU Usage: {emoji} <code>{cpu}%</code>"
stats_server_ram: "• RAM Usage: {emoji} <code>{used_gib} GiB</code> | <code>{total_gib} GiB</code>"
stats_server_storage: "• Storage: <code>{used_gib} GiB</code> | <code>{total_gib} GiB</code>"

stats_served_chats: "• Chats: <code>{count}</code>"
stats_served_chats_err: "• Chats: <code>Error: {error}</code>"

stats_served_users: "• Users: <code>{count}</code>"
stats_served_users_err: "• Users: <code>Error: {error}</code>"


sudo_owner_self: "😂 Haha, you’re the boss already! Why are you even trying to add yourself?"
sudo_bot_self: "🤖 Haha, I’m the bot! I can’t add myself to the sudo list — even the owner can’t cheat me 😎"
sudo_bot_user: "🤖 You can’t add a bot to the sudo list — sudoers must be human!"

sudo_fetch_user_fail: "❌ Failed to fetch user info. Maybe the user is inaccessible.\n<i>{error}</i>"
sudo_check_fail: "❌ Failed to check sudo existence: <i>{error}</i>"
sudo_already: "⚠️ User {user} (ID:<code>{id}</code>) is already a sudoer."
sudo_add_fail: "❌ Failed to add sudo: <i>{error}</i>"
sudo_add_success: "✅ Added {user} (<code>{id}</code>) to sudoers."
sudo_assistant_self: "🤖 The assistant is always in the sudo list by default — it cannot be added again."

sudo_owner_remove_block: "😎 Nice try! You can’t remove the owner — they’re untouchable."
sudo_assistant_cannot_remove: "🤖 The assistant is required and cannot be removed from the sudo list."
sudo_not_exists: "⚠️ User {user} (<code>{id}</code>) is not a sudoer."
sudo_remove_fail: "❌ Failed to remove sudo: <i>{error}</i>"
sudo_remove_success: "🗑️ Removed {user} (ID: <code>{id}</code>) from sudoers."

sudo_list_fetching: "⏳ Fetching sudoers list..."
sudo_list_fetch_fail: "❌ Failed to get sudoers: <code>{error}</code>"
sudo_list_header: "👑 <b>Current Sudoers:</b>"
sudo_list_owner: "{index}. {user} — <b>Owner</b>"
sudo_list_entry: "{index}. {user}"
sudo_list_no_extra: "⚠️ No additional sudoers found."

seek_no_active: "⚠️ <b>No track is currently playing.</b>"
seek_usage: "⚠️ Please provide seconds.\nExample: <code>{cmd} 40</code>"
seek_invalid_seconds: "⚠️ Invalid seconds value.\nExample: <code>{cmd} 40</code>"
seek_too_close_start: "⚠️ Cannot seek backward <b>{seconds}</b> seconds — that would be too close to the beginning of the track."
seek_too_close_end: "⚠️ Cannot seek forward <b>{seconds}</b> seconds — that would be too close to the end of the track."
seek_failed: "❌ Failed to seek {direction} <b>{seconds}</b> seconds.\nError: <code>{error}</code>"
seek_success: "{emoji} Jumped {direction} to <u>{position}</u> of <u>{duration}</u>."

stop_confirm_paused: |
  ⏸️ <b>Playback is currently paused.</b>

  Do you want to resume playing or stop it completely?

stop_confirm_muted: |
  🔇 <b>Playback is currently muted.</b>

  Do you want to unmute or stop it completely?

stopped: "⏹️ <b>Playback stopped and cleared by {user}.</b>"

shuffle_current_state: "🔀 Currently shuffle is <b>{state}</b> for this chat.\n\nUse <code>{cmd}</code> to toggle it."
shuffle_updated: "🔀 Shuffle <b>{state}</b> by {user}."

skip_stopped: "⏹️ Playback stopped. Queue is empty.\nSkipped by: {user}"

speed_usage: |
  💡 <b>Usage:</b>
  <code>{cmd} [speed] [duration]</code> — change playback speed
  
  Allowed speed range: <b>0.50x → 4.00x</b>
  Duration (optional) resets the speed after given seconds.
  
  Example:
  <code>{cmd} 1.25 45</code> — play at 1.25x for 45 seconds.

speed_current: "🎧 <b>Current Speed:</b> <code>{speed}x</code> for 🎵 <u>{title}</u>\n\n💡 Use <code>{cmd} reset</code> to restore normal playback."

speed_current_with_reset: |
  🎧 <b>Current Speed:</b> <code>{speed}x</code>

  🎵 <b>Track:</b> <i>{title}</i>
  ⏳ <b>Auto-reset in:</b> {duration}
  
  💡 Use <code>{cmd} reset</code> to restore normal playback instantly.

speed_invalid_value: "❌ Invalid speed value.\nExample: <code>{cmd} 1.5</code> or <code>{cmd} 2x</code>"
speed_invalid_range: "⚠️ Speed must be between <b>0.50x</b> and <b>4.00x</b>."
speed_invalid_duration: "⚠️ Invalid duration. It must be between <b>5</b> and <b>3600</b> seconds."

speed_already_set: "ℹ️ Playback speed is already <b>{speed}x</b> for 🎵 <b>{title}</b>"

speed_already_set_reset_hint: "ℹ️ Playback speed is already <b>{speed}x</b> for 🎵 <b>{title}</b>\n\n💡 Use <code>{cmd} reset</code> to return to normal speed."

speed_failed: |
  ❌ Failed to change speed to <b>{speed}x</b>.
  Error: <code>{error}</code>

speed_reset_success: "✅ Playback speed reset to <b>1.0x</b> by {user}"

speed_set: "🚀 Playback speed set to <b>{speed}x</b> by {user}."
speed_set_with_reset: |
  🚀 Playback speed set to <b>{speed}x</b> by {user}.
  ⏳ Will reset to normal after <b>{duration}</b> seconds.


spt_fetch_fail: "⚠️ Failed to fetch network info: <code>{error}</code>"
spt_servers_fetch_fail: "⚠️ Failed to fetch servers: <code>{error}</code>"
spt_best_server_fail: "⚠️ Failed to find best server: <code>{error}</code>"

spt_running_download: "🚀 Running download test..."
spt_running_upload: "📤 Running upload test..."

spt_result: |
  <b>📡 SpeedTest Result</b>

  <b><u>👤 Client</u></b>
  IP: <code>{ip}</code>
  ISP: {isp}
  Lat: <code>{lat}</code>
  Lon: <code>{lon}</code>

  <b><u>🛰️ Server</u></b>
  Name: {server_name}
  Country: {country}
  Sponsor: {sponsor}
  Distance: {distance_km} km
  Latency: {latency_ms} ms

  <b><u>⚡ Speed</u></b>
  Download: <code>{dl_mbps} Mbps</code>
  Upload: <code>{ul_mbps} Mbps</code>

start_private: "Hello {user}, My name is {bot}.\n\nI'm a telegram streaming bot with some useful features. Supporting platforms like Youtube, Spotify, JioSaavn, Soundcloud etc.\n\nFeel free to add me to your groups."

start_group: "<b>💫 I’m here!</b> Try a command… if you think you can handle me 😉"

stream_channelid_fail: "⚠️ Failed to get your connected channel’s ID."
stream_queue_finished: "No more songs in the queue."
stream_downloading_next: "Hold On !!\n\n📥 Downloading your next track..."
stream_download_fail: "❌ Failed to download.\nError: <code>{error}</code>"
stream_play_fail: "❌ Failed to play the song."
stream_now_playing: |
  <b>🎵 Now Playing:</b>

  <b>Track:</b> <a href="{url}">{title}</a>
  <b>Duration:</b> {duration}
  <b>Requested by:</b> {by}


sudo_join_owner: "👑 {user} — owner of {bot} — just entered the chat."
sudo_join_sudo: "🛡️ {user}, a sudo user of {bot}, just joined the chat."

supergroup_needed: |
  This chat (ID: <code>{chat_id}</code>) is not a supergroup yet.
  <b>⚠️ Please convert this chat to a supergroup then add me as admin.</b>
  
  If you don't know how to convert, use this guide:
  🔗 <a href=\"https://te.legra.ph/How-to-Convert-a-Group-to-a-Supergroup-01-02\">How to convert to a SuperGroup</a>
  
  If you have any questions, join our support group:
  
unmute_already: "ℹ️ <b>Already Unmuted</b>\nThe music is not muted in this chat."
unmute_failed: "❌ <b>Playback Unmute Failed</b>\nError: <code>{error}</code>"
unmute_success: |
  🔊 <b>Unmuted playback</b>

  🎵 Track: {title}
  👤 Unmuted by: {user}
  {speed_line}


help_owner: |
  👑 <b>Owner Commands</b>

  Exclusive tools for managing sudoers, executing system code, and performing bot administration securely.

  <b>Commands:</b>
  <b>/addsudo</b> - Add a new user to bot's sudolist
  <b>/delsudo</b> - Remove a user from bot's sudolist
  <b>/eval</b> - Execute Go code snippets
  <b>/maintenance</b> — Manage the bot’s maintenance mode
  <b>/restart</b> — Restart the bot
  <b>/sh</b> - Run shell commands

help_sudo: |
  ⚡ <b>Sudoer Commands</b>

  Advanced tools for managing bot behavior, monitoring performance, and controlling logging and automation.

  <b>Commands:</b>
  <b>/active</b> - Shows total active chats
  <b>/autoleave</b> - Toggle automatic chat leaving
  <b>/logger</b> - Enable or disable logger
  <b>/stats</b> - Display the bot & system stats
  <b>/logs</b> - Get the system logs 

help_admin: |
  🛠 <b>Admin Commands</b>

  Music and playback control tools for admins to handle playback easily.

  <b>Commands:</b>
  <b>/playmode</b> - Control who can use /play
  <b>/cmddelete</b> - Toggle automatic deletion of bot commands
  <b>/speed</b> - Change playback speed
  <b>/skip</b> - Skip the current song
  <b>/pause</b> - Pause the current playback
  <b>/resume</b> - Resume paused playback
  <b>/replay</b> - Replay the current song
  <b>/mute</b> - Mute playback
  <b>/unmute</b> - Unmute playback
  <b>/seek</b> - Seek forward by a few seconds
  <b>/seekback</b> - Seek backward by a few seconds
  <b>/jump</b> - Jump to a given time in track
  <b>/move</b> - Move a queued track to another position
  <b>/clear</b> - Clear all songs from queue
  <b>/remove</b> - Remove a specific track from queue
  <b>/shuffle</b> - Shuffle all queued tracks
  <b>/loop</b> - Enable or disable looping
  <b>/stop</b> - Stop playback and leave VC
  <b>/nothumb</b> - Enable or disable artwork/thumbnail in playback messages

help_public: |
  🌍 <b>Public Commands</b>

  Features for playing songs, viewing queues, checking latency, and reporting bugs.

  <b>Commands:</b>
  <b>/play</b> - Play a song
  <b>/queue</b> - View all tracks currently queued
  <b>/ping</b> - Check bot’s network latency
  <b>/start</b> - Start the bot
  <b>/help</b> - Show help menu
  <b>/bug</b> - Report an issue or problem
  <b>/position</b> - Show current track’s timestamp
  <b>/reload</b> - Reload admin or cache data
  <b>/json</b> - Show message JSON structure
  <b>/sudolist</b> - View sudo user list
  <b>/privacy<b> - open privacy policy
```


## Appendix E (YukkiMusic-Go) — VERBATIM extracted `helpTexts` per-command help blocks (38 blocks)

> Extracted mechanically from `helpTexts[...] = \`...\`` literals (and `fmt.Sprintf(\`...\`)` variants). `+`-concatenated / alias assignments (e.g. `helpTexts["/fplay"] = helpTexts["/play"]`) are NOT duplicated here — they point at the base key. Sprintf `%s`/`%d` argument lists are noted, not expanded.

### YukkiMusic-Go — "/active"  (`refs/YukkiMusic-Go/internal/modules/active.go`)

```html
<i>Show all active voice chat sessions.</i>

<u>Usage:</u>
<b>/active</b> or <b>/ac</b> — List active chats

<b>📊 Information Shown:</b>
• Total active voice chats
• Playback status (playing/paused/muted)
• Now playing track and queue size

<b>🔒 Restrictions:</b>
• <b>Sudo users</b> only

<b>💡 Use Case:</b>
Monitor bot usage and identify issues.
```

### YukkiMusic-Go — "/delauth"  (`refs/YukkiMusic-Go/internal/modules/auth.go`)

```html
<i>Revoke permission from a user who was previously authorized to control playback.</i>

<u>Usage:</u>
<b>/delauth [reply to user]</b> — Remove by replying to their message.  
<b>/delauth &lt;user_id / username&gt; </b>— Remove by ID or @username.

<b>⚙️ Notes:</b>
• Only <b>chat admins</b> can use this command.  
• Use this to revoke access from misbehaving users.  
• To check who’s currently authorized, use <code>/authlist</code>.
```

### YukkiMusic-Go — "/authlist"  (`refs/YukkiMusic-Go/internal/modules/auth.go`)

```html
<u>Usage:</u>
<b>/authlist</b> - <i>Displays all users currently authorized to control playback in this chat.</i>

<b>⚙️ Notes:</b>
• Anyone in the chat can use this command.  
• Shows only manually added auth users — the Owner, Assistant, and Sudoers are not listed but are always authorized.
```

### YukkiMusic-Go — "/autoplay"  (`refs/YukkiMusic-Go/internal/modules/autoplay.go`)

```html
<i>Automatically play recommended songs when the queue ends.</i>

<u>Usage:</u>
<b>/autoplay</b> — Open the autoplay control panel
<b>/autoplay enable</b> — Enable autoplay
<b>/autoplay disable</b> — Disable autoplay

<b>⚙️ Behavior:</b>
• When enabled, once the queue finishes the bot keeps playing similar recommended tracks from YouTube.
• Recommendations are based on the last played song and skip tracks already in the queue.
• Autoplay is tied to the current session and resets once playback stops.

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this
```

### YukkiMusic-Go — "broadcast"  (`refs/YukkiMusic-Go/internal/modules/broadcast.go`)

```html
<i>Broadcast a message to all served chats and users.</i>

<u>Usage:</u>
<b>/broadcast [flags] [text]</b> — Broadcast a text message.
<b>/broadcast [flags]</b> (as a reply) — Broadcast the replied message.
<b>/broadcast -cancel</b> — Cancel the ongoing broadcast.

<blockquote>
<b>📋 Flags:</b>
• <code>-nochat</code> — Skip groups
• <code>-nouser</code> — Skip users
• <code>-copy</code> — Send the replied message without the forwarded tag
• <code>-pin</code> — Pin the message silently
• <code>-cancel</code> — Cancel the ongoing broadcast
</blockquote>
<blockquote>
<b>📌 Examples:</b>
/broadcast -nochat Important announcement
/broadcast -copy -pin [reply to message]
</blockquote>
<b>⚠️ Notes:</b>
• Only the <b>owner</b> can use this command
• Flags go first, before the text
• You can cancel with the inline button or <code>/broadcast -cancel</code>
• Only one broadcast can run at a time
```

### YukkiMusic-Go — "/sh"  (`refs/YukkiMusic-Go/internal/modules/dev.go`)

```html
<i>Execute shell commands on server.</i>

<u>Usage:</u>
<b>/sh [command]</b> — Run shell command

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>⚠️ Warning:</b>
Direct system access - extremely powerful.
```

### YukkiMusic-Go — "/json"  (`refs/YukkiMusic-Go/internal/modules/dev.go`)

```html
<i>Get JSON representation of message/user/chat.</i>

<u>Usage:</u>
<b>/json</b> — Current message JSON
<b>/json -s</b> — Sender JSON
<b>/json -c</b> — Chat JSON
<b>/json -m</b> — Media JSON
<b>/json [reply] -f</b> — File JSON

<b>🔒 Restrictions:</b>
• <b>Sudo users only</b>

<b>💡 Use Case:</b>
Debugging and development.
```

### YukkiMusic-Go — "/logs"  (`refs/YukkiMusic-Go/internal/modules/dev.go`)

```html
<i>Send current bot logs file.</i>

<u>Usage:</u>
<b>/logs</b> — Send current log file

<b>🔒 Restrictions:</b>
• <b>Sudo users only</b>
```

### YukkiMusic-Go — "/help"  (`refs/YukkiMusic-Go/internal/modules/help.go`)

```html
ℹ️ <b>Help Command</b>
<i>Displays general bot help or detailed information about a specific command.</i>

<u>Usage:</u>
<code>/help</code> — Show the main help menu.  
<code>/help &lt;command&gt;</code> — Show help for a specific command.

For more info, visit our <a href="%s">Support Chat</a>.
```

### YukkiMusic-Go — "/loop"  (`refs/YukkiMusic-Go/internal/modules/loop.go`)

```html
<i>Set loop count for the current track.</i>

<u>Usage:</u>
<b>/loop</b> — Show current loop count
<b>/loop [count]</b> — Set loop count (0-10)

<b>⚙️ Behavior:</b>
• 0 = No loop (play once)
• 1-10 = Repeat track that many times
• Loop counter decrements after each playback

<b>💡 Examples:</b>
<code>/loop 0</code> — Disable loop
<code>/loop 3</code> — Loop current track 3 times
<code>/loop 10</code> — Loop current track 10 times

<b>⚠️ Notes:</b>
• Maximum loop count: 10
• Loop affects only current track
• After loops complete, plays next in queue
• If the track is forcefully skipped using <code>/skip</code>, the loop will stop and reset automatically
```

### YukkiMusic-Go — "/maintenance"  (`refs/YukkiMusic-Go/internal/modules/maint.go`)

```html
<i>Toggle maintenance mode.</i>

<u>Usage:</u>
<b>/maintenance</b> — Open the maintenance panel
<b>/maintenance enable</b> — Enable maintenance
<b>/maintenance disable</b> — Disable maintenance

<b>⚙️ Behavior When Active:</b>
• Stops all active rooms
• Blocks non-owner/sudo commands
• Shows maintenance message to users

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command
```

### YukkiMusic-Go — "/mute"  (`refs/YukkiMusic-Go/internal/modules/mute.go`)

```html
<i>Mute the audio output in voice chat.</i>

<u>Usage:</u>
<b>/mute</b> — Mute indefinitely

<b>⚙️ Features:</b>
• Audio continues playing (progress tracked)

<b>💡 Examples:</b>
<code>/mute</code> — Mute until manual unmute

<b>⚠️ Notes:</b>
• Track continues playing in background
• Use <code>/unmute</code> to restore audio
```

### YukkiMusic-Go — "/pause"  (`refs/YukkiMusic-Go/internal/modules/pause.go`)

```html
<i>Pause the current playback.</i>

<u>Usage:</u>
<b>/pause</b> — Pause playback

<b>⚙️ Features:</b>
• Manual pause/resume control

<b>💡 Examples:</b>
<code>/pause</code> — Pause indefinitely
```

### YukkiMusic-Go — "/ping"  (`refs/YukkiMusic-Go/internal/modules/ping.go`)

```html
<i>Check bot responsiveness and system stats.</i>

<u>Usage:</u>
<b>/ping</b> — Get bot status

<b>📊 Information Shown:</b>
• Response latency (ms)
• Uptime
• RAM usage
• CPU usage
• Disk usage

<b>💡 Use Case:</b>
Check if bot is responsive and view system health.
```

### YukkiMusic-Go — "/play"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Play a song in the voice chat from YouTube, Spotify, or other sources.</i>

<u>Usage:</u>
<b>/play [query/URL]</b> — Search and play a song
<b>/play [reply to audio/video]</b> — Play replied media

<b>🎵 Supported Sources:</b>
• YouTube (videos, playlists)
• Spotify (tracks, albums, playlists)
• SoundCloud
• Direct audio/video links

<b>⚙️ Features:</b>
• Queue support - adds to end if already playing
• Auto-join voice chat if not present
• Duration limit check
• Multiple track support (playlists)

<b>💡 Examples:</b>
<code>/play never gonna give you up</code>
<code>/play https://youtu.be/dQw4w9WgXcQ</code>

<b>⚠️ Notes:</b>
• Bot must have proper permissions in voice chat
• Tracks exceeding duration limit will be skipped
• Use <code>/queue</code> to view upcoming tracks
• Use <code>/fplay</code> to force play (skip queue)
```

### YukkiMusic-Go — "/fplay"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Force play a song, skipping the current queue.</i>

<u>Usage:</u>
<b>/fplay [query/URL]</b> — Force play immediately
<b>/fplay [reply to audio/video]</b> — Force play replied media

<b>🎵 Behavior:</b>
• Stops current playback
• Clears queue
• Starts playing immediately

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Example:</b>
<code>/fplay urgent announcement track</code>

<b>⚠️ Note:</b>
This command is useful for urgent playback needs but will disrupt the current queue.
```

### YukkiMusic-Go — "/vplay"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Play video content in voice chat (video mode).</i>

<u>Usage:</u>
<b>/vplay [query/URL]</b> — Play video
<b>/vplay [reply to video]</b> — Play replied video

<b>📹 Features:</b>
• Full video playback support
• Audio + Video streaming
• Same queue system as audio

<b>⚠️ Notes:</b>
• Requires video streaming permissions
• Use <code>/fvplay</code> for force video play
```

### YukkiMusic-Go — "/fvplay"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Force play video content, skipping queue.</i>

<u>Usage:</u>
<b>/fvplay [query/URL]</b> — Force play video immediately

<b>🔒 Restrictions:</b>
• Admin/auth only command

<b>💡 Use Case:</b>
Immediate video playback when something urgent needs to be shown.
```

### YukkiMusic-Go — "/cplay"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Play in linked channel's voice chat.</i>

<u>Usage:</u>
<b>/cplay [query]</b> — Play in linked channel

<b>⚙️ Setup Required:</b>
First use <code>/setcplay [channel_id]</code>

<b>⚠️ Note:</b>
All c* commands work the same as regular commands but affect the linked channel.
```

### YukkiMusic-Go — "/channelplay"  (`refs/YukkiMusic-Go/internal/modules/play.go`)

```html
<i>Configure linked channel for channel play mode.</i>

<u>Usage:</u>
<b>/channelplay [channel_id | @username | linked]</b> — Set linked channel
<b>/channelplay off</b> — Turn channel play off

<b>⚙️ Behavior:</b>
• Links a channel to current group
• All <code>c*</code> commands affect linked channel
• Channel must be accessible by bot

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> can configure

<b>💡 Examples:</b>
<code>/setcplay -1001234567890</code>
<code>/setcplay @mychannel</code>
<code>/setcplay linked</code>

<b>⚠️ Notes:</b>
• Get channel ID using forward + @userinfobot
• Bot must be admin in linked channel
• Use <code>/cplay</code> after setup
```

### YukkiMusic-Go — "/position"  (`refs/YukkiMusic-Go/internal/modules/position.go`)

```html
<i>Show current playback position and track info.</i>

<u>Usage:</u>
<b>/position</b> — Show position
<b>/pos</b> — Alias for /position

<b>📊 Information Displayed:</b>
• Current track title & link
• Current playback position & duration
• Playback speed
• Requester
```

### YukkiMusic-Go — "/privacy"  (`refs/YukkiMusic-Go/internal/modules/privacy.go`)

```html
<i>Show the bot's privacy policy and data handling.</i>
```

### YukkiMusic-Go — "/reload"  (`refs/YukkiMusic-Go/internal/modules/reload.go`)

```html
<i>Reload admin cache and refresh voice chat state.</i>

<u>Usage:</u>
<b>/reload</b> — Refresh all cached data

<b>🔄 What Gets Reloaded:</b>
• Chat admin list
• Voice chat status
• Assistant presence status
• Assistant ban status (unbanned and rejoined when possible)

<b>🔒 Flood Protection:</b>
• Regular users: 5 minute cooldown
• Admins: 2 minute cooldown

<b>💡 When to Use:</b>
• After promoting/demoting admins
• Voice chat issues
• Permission problems
• Bot behaving incorrectly

<b>⚠️ Notes:</b>
• May reset room state if admin permissions required
```

### YukkiMusic-Go — "/replay"  (`refs/YukkiMusic-Go/internal/modules/replay.go`)

```html
<i>Restart the current track from the beginning.</i>

<u>Usage:</u>
<b>/replay</b> — Restart current track

<b>⚙️ Behavior:</b>
• Resets position to 0:00
• Maintains speed setting
• Continues playback immediately

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this
```

### YukkiMusic-Go — "/restart"  (`refs/YukkiMusic-Go/internal/modules/restart.go`)

```html
<i>Restart the bot process.</i>

<u>Usage:</u>
<b>/restart</b> — Restart bot

<b>⚙️ Behavior:</b>
• Stops all active rooms
• Notifies all active chats
• Restarts bot process
• Clears download cache

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>⚠️ Warning:</b>
All playback will be interrupted. Bot will be offline for a few seconds.
```

### YukkiMusic-Go — "/resume"  (`refs/YukkiMusic-Go/internal/modules/resume.go`)

```html
<i>Resume the paused playback.</i>

<u>Usage:</u>
<b>/resume</b> — Resume playback from pause

<b>⚙️ Behavior:</b>
• Continues from last paused position
• Now playing card updates instantly

<b>⚠️ Notes:</b>
• Can only resume if currently paused
• Position is preserved during pause
• Speed settings remain active after resume
```

### YukkiMusic-Go — "/seek"  (`refs/YukkiMusic-Go/internal/modules/seek.go`)

```html
<i>Seek forward in the currently playing track.</i>

<u>Usage:</u>
<b>/seek [seconds]</b> — Skip forward by specified seconds

<b>⚙️ Features:</b>
• Jump ahead in current track
• Position tracking updated
• Cannot seek past track end (10s buffer)

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/seek 30</code> — Skip forward 30 seconds
<code>/seek 120</code> — Skip forward 2 minutes

<b>⚠️ Notes:</b>
• Minimum: any positive value
• Maximum: track_duration - current_position - 10 seconds
```

### YukkiMusic-Go — "/seekback"  (`refs/YukkiMusic-Go/internal/modules/seek.go`)

```html
<i>Seek backward in the currently playing track.</i>

<u>Usage:</u>
<b>/seekback [seconds]</b> — Go back by specified seconds

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/seekback 15</code> — Go back 15 seconds
<code>/seekback 60</code> — Go back 1 minute
```

### YukkiMusic-Go — "/jump"  (`refs/YukkiMusic-Go/internal/modules/seek.go`)

```html
<i>Jump to a specific position in the track.</i>

<u>Usage:</u>
<b>/jump [seconds]</b> — Jump to exact position

<b>⚙️ Features:</b>
• Absolute position seeking
• Precise time control
• 10-second buffer from end

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/jump 90</code> — Jump to 1:30
<code>/jump 0</code> — Jump to start (same as /replay)

<b>⚠️ Notes:</b>
• Position must be within track duration - 10 seconds
• More precise than <code>/seek</code> and <code>/seekback</code>
```

### YukkiMusic-Go — "/playmode"  (`refs/YukkiMusic-Go/internal/modules/settings.go`)

```html
<i>Control who can use the /play command in this chat.</i>

<u>Usage:</u>
<b>/playmode [enable|disable]</b> — Set play mode restriction

<b>⚙️ Options:</b>
• <b>enable</b> — Only admins and authorized users can play
• <b>disable</b> — Everyone can play (default)
```

### YukkiMusic-Go — "/adminmode"  (`refs/YukkiMusic-Go/internal/modules/settings.go`)

```html
<i>Control who can use admin-level music commands in this chat.</i>

<u>Usage:</u>
<b>/adminmode [admin|adminauth|everyone]</b> — Set admin command access

<b>⚙️ Options:</b>
• <b>admin</b> — Only chat admins can use admin commands
• <b>adminauth</b> — Chat admins + authorized users can use admin commands (default)
• <b>everyone</b> — Everyone can use admin commands
```

### YukkiMusic-Go — "/shuffle"  (`refs/YukkiMusic-Go/internal/modules/shuffle.go`)

```html
<i>Toggle shuffle mode for the queue.</i>

<u>Usage:</u>
<b>/shuffle</b> — Show current shuffle state
<b>/shuffle on</b> — Enable shuffle
<b>/shuffle off</b> — Disable shuffle

<b>⚙️ Behavior:</b>
• Randomly reorders queue when enabled
• Affects track selection order
• Can be toggled at any time

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/shuffle on</code> — Enable shuffle mode
<code>/shuffle off</code> — Disable shuffle mode

<b>⚠️ Note:</b>
Shuffle only affects queue order, not currently playing track.
```

### YukkiMusic-Go — "/skip"  (`refs/YukkiMusic-Go/internal/modules/skip.go`)

```html
<i>Skip the currently playing track and play the next in queue.</i>

<u>Usage:</u>
<b>/skip</b> — Skip current track

<b>⚙️ Behavior:</b>
• Downloads next track in queue
• Starts playback automatically
• If queue is empty and loop is 0, stops playback

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>⚠️ Notes:</b>
• Cannot be undone
• If no tracks in queue, playback stops
• Loop count affects skip behavior
```

### YukkiMusic-Go — "/speed"  (`refs/YukkiMusic-Go/internal/modules/speed.go`)

```html
<i>Control playback speed (tempo).</i>

<u>Usage:</u>
<b>/speed</b> — Show current speed
<b>/speed [multiplier]</b> — Set speed (0.5-4.0x)
<b>/speed normal</b> or <b>/speed reset</b> — Reset to 1.0x

<b>⚙️ Features:</b>
• Range: 0.50x to 4.00x
• Pitch preservation
• Real-time adjustment

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/speed 1.5</code> — Play 1.5x faster
<code>/speed 0.75</code> — Play slower (0.75x)
<code>/speed normal</code> — Reset to normal speed

<b>⚠️ Notes:</b>
• Speed affects duration calculations
• Suffix 'x' is optional: <code>1.5</code> = <code>1.5x</code>
```

### YukkiMusic-Go — "/stats"  (`refs/YukkiMusic-Go/internal/modules/stats.go`)

```html
<i>View detailed bot statistics.</i>

<u>Usage:</u>
<b>/stats</b> — Show statistics

<b>📊 Information Shown:</b>
• System stats (OS, CPU, RAM, disk)
• Go runtime stats (memory, GC)
• Server resources
• MongoDB stats
• Served chats & users

<b>🔒 Restrictions:</b>
• <b>Sudo users</b> only
```

### YukkiMusic-Go — "/end"  (`refs/YukkiMusic-Go/internal/modules/stop.go`)

```html
<i>Stop playback and leave the voice chat.</i>

<u>Usage:</u>
<b>/stop</b> or <b>/end</b> — Stop playback

<b>⚙️ Behavior:</b>
• Stops current track
• Clears queue
• Assistant leaves voice chat
•
<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>⚠️ Note:</b>
This action cannot be undone. Use <code>/pause</code> for temporary stops.
```

### YukkiMusic-Go — "/nothumb"  (`refs/YukkiMusic-Go/internal/modules/thumbnail.go`)

```html
<i>Toggle thumbnail/artwork display in playback messages.</i>

<u>Usage:</u>
<b>/nothumb</b> — Show current status
<b>/nothumb [enable|disable]</b> — Change setting

<b>⚙️ Behavior:</b>
• <b>Disabled (default):</b> Shows track artwork/thumbnail
• <b>Enabled:</b> Hides artwork, text-only messages

<b>💡 Examples:</b>
<code>/nothumb enable</code> — Disable thumbnails
<code>/nothumb disable</code> — Enable thumbnails

<b>⚠️ Note:</b>
This setting affects all future playback messages in this chat.
```

### YukkiMusic-Go — "/unmute"  (`refs/YukkiMusic-Go/internal/modules/unmute.go`)

```html
<i>Unmute the audio output in voice chat.</i>

<u>Usage:</u>
<b>/unmute</b> — Restore audio

<b>⚙️ Behavior:</b>
• Restores audio immediately
• Shows current playback info
```


## Appendix E (ArcMusic) — VERBATIM extracted `helpTexts` per-command help blocks (50 blocks)

> Extracted mechanically from `helpTexts[...] = \`...\`` literals (and `fmt.Sprintf(\`...\`)` variants). `+`-concatenated / alias assignments (e.g. `helpTexts["/fplay"] = helpTexts["/play"]`) are NOT duplicated here — they point at the base key. Sprintf `%s`/`%d` argument lists are noted, not expanded.

### ArcMusic — "/active"  (`refs/ArcMusic/internal/modules/active.go`)

```html
<b>Show all active voice chat sessions.</b>

<u>Usage:</u>
<b>/active</b> or <b>/ac</b> — List active chats

<b>📊 Information Shown:</b>
• Total active voice chats

<b>🔒 Restrictions:</b>
• <b>Sudo users</b> only

<b>💡 Use Case:</b>
Monitor exact bot usage.
```

### ArcMusic — "/delauth"  (`refs/ArcMusic/internal/modules/auth.go`)

```html
<i>Revoke permission from a user who was previously authorized to control playback.</i>

<u>Usage:</u>
<b>/delauth [reply to user]</b> — Remove by replying to their message.  
<b>/delauth &lt;user_id / username&gt; </b>— Remove by ID or @username.

<b>⚙️ Notes:</b>
• Only <b>chat admins</b> can use this command.  
• Use this to revoke access from misbehaving users.  
• To check who’s currently authorized, use <code>/authlist</code>.
```

### ArcMusic — "/authlist"  (`refs/ArcMusic/internal/modules/auth.go`)

```html
<u>Usage:</u>
<b>/authlist</b> - <i>Displays all users currently authorized to control playback in this chat.</i>

<b>⚙️ Notes:</b>
• Anyone in the chat can use this command.  
• Shows only manually added auth users — the Owner, Assistant, and Sudoers are not listed but are always authorized.
```

### ArcMusic — "broadcast"  (`refs/ArcMusic/internal/modules/broadcast.go`)

```html
<i>Broadcast a message to all served chats and users.</i>

<u>Usage:</u>
<b>/broadcast [flags] [text] </b> — Broadcast text message.
<b>/broadcast [flags] [reply to message]</b> — Broadcast the replied message.
<b>/broadcast -cancel</b> — Cancel ongoing broadcast.

<blockquote>
<b>📋 Flags:</b>
• <code>--nochat</code> — Exclude groups from broadcast
• <code>--nouser</code> — Exclude users from broadcast
• <code>--copy</code> — Remove forwarded tag, when broadcasting a replied message (copy mode)
• <code>--limit [n]</code> — Limit total messages sent (default: 0 = no limit)
• <code>--delay [seconds]</code> — Delay between messages (default: 1.5s)
• <code>--pin</code> — Pin the message (silent)
• <code>--pinloud</code> — Pin the message (with notification)

• <code>-cancel</code> - Cancel a ongoing broadcast.
</blockquote>
<blockquote>
<b>📌 Examples:</b>
/broadcast -nochat -delay 2 Important announcement
/broadcast -copy -nochat -pin [reply to message]
/broadcast -limit 10 -delay 3 Limited broadcast
</blockquote>
<b>⚠️ Notes:</b>
• Only the <b>owner</b> can use this command
• After every 30 messages, there's an automatic 7.5s pause
• You can cancel ongoing broadcasts using the inline button or <code>/broadcast -cancel</code>
• Only one broadcast can run at a time
```

### ArcMusic — "bug"  (`refs/ArcMusic/internal/modules/bug.go`)

```html
<i>Report a bug, issue, or unexpected behavior directly to the bot developers.</i>

<u>Usage:</u>
<b>/bug &lt;description&gt;</b> — Send a bug report with a short explanation.  
<b>Reply + /bug</b> — Report a specific message or media as a bug.

<b>🧠 Details:</b>
When used, the bot automatically forwards your report (and the replied message if any) to the <b>owner</b> and <b>logger channels</b>.  
Flood protection is applied — you can only send one report every <b>5 minutes</b> per chat.

<b>⚠️ Note:</b>  
Reports are logged for debugging purposes only. Misuse (like spam) may restrict your access to this command.
```

### ArcMusic — "/sh"  (`refs/ArcMusic/internal/modules/dev.go`)

```html
<i>Execute shell commands on server.</i>

<u>Usage:</u>
<b>/sh [command]</b> — Run shell command

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>⚠️ Warning:</b>
Direct system access - extremely powerful.
```

### ArcMusic — "/ev"  (`refs/ArcMusic/internal/modules/dev.go`)

```html
<i>Execute Go code dynamically (eval mode).</i>

<u>Usage:</u>
<b>/eval [code]</b> — Run Go code

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>⚠️ Warning:</b>
Powerful command - use with caution.
```

### ArcMusic — "/json"  (`refs/ArcMusic/internal/modules/dev.go`)

```html
<i>Get JSON representation of message/user/chat.</i>

<u>Usage:</u>
<b>/json</b> — Current message JSON
<b>/json -s</b> — Sender JSON
<b>/json -c</b> — Chat JSON
<b>/json -m</b> — Media JSON
<b>/json [reply] -f</b> — File JSON

<b>💡 Use Case:</b>
Debugging and development.
```

### ArcMusic — cmd  (`refs/ArcMusic/internal/modules/handlers.go`)

```html
<i>Channel play variant of %s</i>

<b>⚙️ Requires:</b>
First configure channel using: <code>/channelplay --set [channel_id]</code>

%s

<b>💡 Note:</b>
This command affects the linked channel's voice chat, not the current group.
```

### ArcMusic — "/help"  (`refs/ArcMusic/internal/modules/help.go`)

```html
ℹ️ <b>Help Command</b>
<i>Displays general bot help or detailed information about a specific command.</i>

<u>Usage:</u>
<code>/help</code> — Show the main help menu.  
<code>/help &lt;command&gt;</code> — Show help for a specific command.

<b>💡 Tip:</b> You can view help for any command directly by adding a <code>-h</code> or <code>--help</code> flag, e.g. <code>/play -h</code>

<b>⚠️ Note:</b> Some commands are <b>restricted</b> to specific contexts (like <b>Groups</b>, <b>Admins</b>, <b>Sudoers</b>, or the <b>Owner</b>).  
If you try using <code>-h</code> or <code>--help</code> inside a restricted chat or PM, the bot may not respond.  
To still view help for those commands, use the global format instead:
<code>/help &lt;command&gt;</code>

For more info, visit our <a href="%s">Support Chat</a>.
```

### ArcMusic — "/logs"  (`refs/ArcMusic/internal/modules/logs.go`)

```html
<i>Download or view bot logs.</i>

<u>Usage:</u>
<b>/logs</b> — Send current log file or show content
<b>/logs [n]</b> — Get last N lines from recent logs
<b>/logs -old [n]</b> — Get first N lines from oldest logs
<b>/logs -clear</b> — Clear current log file

<b>📋 Examples:</b>
• <code>/logs</code> — Get full current log
• <code>/logs 50</code> — Last 50 lines from recent
• <code>/logs 100</code> — Last 100 lines from recent
• <code>/logs -old 50</code> — First 50 lines from oldest
• <code>/logs -clear</code> — Delete current log

<b>🔒 Restrictions:</b>
• <b>Sudo users only</b>

<b>⚠️ Notes:</b>
• If content < 2000 chars, shows as code preview
• Otherwise sends as file
• N can be any positive number (default: 50)
```

### ArcMusic — "/loop"  (`refs/ArcMusic/internal/modules/loop.go`)

```html
<i>Set loop count for the current track.</i>

<u>Usage:</u>
<b>/loop</b> — Show current loop count
<b>/loop [count]</b> — Set loop count (0-10)

<b>⚙️ Behavior:</b>
• 0 = No loop (play once)
• 1-10 = Repeat track that many times
• Loop counter decrements after each playback

<b>💡 Examples:</b>
<code>/loop 0</code> — Disable loop
<code>/loop 3</code> — Loop current track 3 times
<code>/loop 10</code> — Loop current track 10 times

<b>⚠️ Notes:</b>
• Maximum loop count: 10
• Loop affects only current track
• After loops complete, plays next in queue
• If the track is forcefully skipped using <code>/skip</code>, the loop will stop and reset automatically
```

### ArcMusic — "/maintenance"  (`refs/ArcMusic/internal/modules/maint.go`)

```html
<i>Toggle maintenance mode.</i>

<u>Usage:</u>
<b>/maintenance</b> — Show current status
<b>/maintenance on [reason]</b> — Enable maintenance
<b>/maintenance off</b> — Disable maintenance

<b>⚙️ Behavior When Active:</b>
• Stops all active rooms
• Blocks non-owner/sudo commands
• Shows maintenance message to users

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>💡 Examples:</b>
<code>/maintenance on Server upgrade</code>
<code>/maintenance off</code>

<b>⚠️ Notes:</b>
• Owner and sudoers can still use bot
• All rooms are destroyed when enabled
• Users see maintenance message with reason
```

### ArcMusic — "/mute"  (`refs/ArcMusic/internal/modules/mute.go`)

```html
<i>Mute the audio output in voice chat.</i>

<u>Usage:</u>
<b>/mute</b> — Mute indefinitely
<b>/mute [seconds]</b> — Mute with auto-unmute timer

<b>⚙️ Features:</b>
• Audio continues playing (progress tracked)
• Auto-unmute timer support (5-3600 seconds)

<b>💡 Examples:</b>
<code>/mute</code> — Mute until manual unmute
<code>/mute 60</code> — Mute for 60 seconds

<b>⚠️ Notes:</b>
• Track continues playing in background
• Use <code>/unmute</code> to restore audio
```

### ArcMusic — "/pause"  (`refs/ArcMusic/internal/modules/pause.go`)

```html
<i>Pause the current playback.</i>

<u>Usage:</u>
<b>/pause</b> — Pause playback
<b>/pause [seconds]</b> — Pause with auto-resume after specified seconds

<b>⚙️ Features:</b>
• Manual pause/resume control
• Auto-resume timer (5-3600 seconds)

<b>💡 Examples:</b>
<code>/pause</code> — Pause indefinitely
<code>/pause 30</code> — Pause for 30 seconds then auto-resume
```

### ArcMusic — "/ping"  (`refs/ArcMusic/internal/modules/ping.go`)

```html
<i>Check bot responsiveness and system stats.</i>

<u>Usage:</u>
<b>/ping</b> — Get bot status

<b>📊 Information Shown:</b>
• Response latency (ms)
• Uptime
• RAM usage
• CPU usage
• Disk usage

<b>💡 Use Case:</b>
Check if bot is responsive and view system health.
```

### ArcMusic — "/play"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Play a song in the voice chat from YouTube, Spotify, or other sources.</i>

<u>Usage:</u>
<b>/play [query/URL]</b> — Search and play a song
<b>/play [reply to audio/video]</b> — Play replied media

<b>🎵 Supported Sources:</b>
• YouTube (videos, playlists)
• Spotify (tracks, albums, playlists)
• SoundCloud
• Direct audio/video links

<b>⚙️ Features:</b>
• Queue support - adds to end if already playing
• Auto-join voice chat if not present
• Duration limit check
• Multiple track support (playlists)

<b>💡 Examples:</b>
<code>/play never gonna give you up</code>
<code>/play https://youtu.be/dQw4w9WgXcQ</code>
<code>/play https://open.spotify.com/track/...</code>

<b>⚠️ Notes:</b>
• Bot must have proper permissions in voice chat
• Tracks exceeding duration limit will be skipped
• Use <code>/queue</code> to view upcoming tracks
• Use <code>/fplay</code> to force play (skip queue)
```

### ArcMusic — "/fplay"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Force play a song, skipping the current queue.</i>

<u>Usage:</u>
<b>/fplay [query/URL]</b> — Force play immediately
<b>/fplay [reply to audio/video]</b> — Force play replied media

<b>🎵 Behavior:</b>
• Stops current playback
• Clears queue
• Starts playing immediately

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Example:</b>
<code>/fplay urgent announcement track</code>

<b>⚠️ Note:</b>
This command is useful for urgent playback needs but will disrupt the current queue.
```

### ArcMusic — "/vplay"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Play video content in voice chat (video mode).</i>

<u>Usage:</u>
<b>/vplay [query/URL]</b> — Play video
<b>/vplay [reply to video]</b> — Play replied video

<b>📹 Features:</b>
• Full video playback support
• Audio + Video streaming
• Same queue system as audio

<b>⚠️ Notes:</b>
• Requires video streaming permissions
• Use <code>/fvplay</code> for force video play
```

### ArcMusic — "/fvplay"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Force play video content, skipping queue.</i>

<u>Usage:</u>
<b>/fvplay [query/URL]</b> — Force play video immediately

<b>🔒 Restrictions:</b>
• Admin/auth only command

<b>💡 Use Case:</b>
Immediate video playback when something urgent needs to be shown.
```

### ArcMusic — "/cplay"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Play in linked channel's voice chat.</i>

<u>Usage:</u>
<b>/cplay [query]</b> — Play in linked channel

<b>⚙️ Setup Required:</b>
First use <code>/channelplay --set [channel_id]</code>

<b>⚠️ Note:</b>
All c* commands work the same as regular commands but affect the linked channel.
```

### ArcMusic — "/channelplay"  (`refs/ArcMusic/internal/modules/play.go`)

```html
<i>Configure linked channel for channel play mode.</i>

<u>Usage:</u>
<b>/channelplay --set [channel_id]</b> — Set linked channel

<b>⚙️ Behavior:</b>
• Links a channel to current group
• All <code>c*</code> commands affect linked channel
• Channel must be accessible by bot

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> can configure

<b>💡 Examples:</b>
<code>/channelplay --set -1001234567890</code>

<b>⚠️ Notes:</b>
• Get channel ID using forward + @userinfobot
• Bot must be admin in linked channel
• Use <code>/cplay</code> after setup
```

### ArcMusic — "/position"  (`refs/ArcMusic/internal/modules/position.go`)

```html
<i>Show current playback position and track info.</i>

<u>Usage:</u>
<b>/position</b> — Show position

<b>📊 Information Displayed:</b>
• Current track title
• Current position (MM:SS)
• Total duration (MM:SS)
• Playback speed (if not 1.0x)

<b>💡 Use Case:</b>
Quick position check without full queue display.
```

### ArcMusic — "/privacy"  (`refs/ArcMusic/internal/modules/privacy.go`)

```html
<i>Show the bot's privacy policy.</i>
```

### ArcMusic — "/queue"  (`refs/ArcMusic/internal/modules/queue.go`)

```html
<i>Display the current playback queue.</i>

<u>Usage:</u>
<b>/queue</b> — Show queue

<b>📋 Display Format:</b>
• Now Playing - Current track with position
• Up Next - Next 10 tracks in queue
• Track info: Title, requester, duration

<b>⚙️ Features:</b>
• Real-time queue status
• Requester attribution
• Duration display
• Queue length indicator

<b>💡 Related Commands:</b>
• <code>/position</code> - Current track position only
• <code>/remove</code> - Remove specific track
• <code>/clear</code> - Clear all tracks
• <code>/move</code> - Reorder tracks
```

### ArcMusic — "/restore"  (`refs/ArcMusic/internal/modules/queue.go`)

```html
<i>Restore a previously cleared music queue.</i>

<u>Usage:</u>
<b>/restore</b> — Recover tracks

<b>⚙️ Behavior:</b>
• Recovers tracks cleared by the last <code>/clear</code> command
• Only works if no new songs have been added since the clear
• Restored tracks are appended to the current queue

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this
```

### ArcMusic — "/remove"  (`refs/ArcMusic/internal/modules/queue.go`)

```html
<i>Remove a specific track from the queue.</i>

<u>Usage:</u>
<b>/remove [index]</b> — Remove track at position

<b>⚙️ Behavior:</b>
• Index starts from 1 (first track in queue)
• Cannot remove currently playing track
• Queue positions update automatically

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/remove 1</code> — Remove first track in queue
<code>/remove 5</code> — Remove 5th track

<b>⚠️ Notes:</b>
• Use <code>/queue</code> to see track indices
• Invalid index shows error with queue length
• Use <code>/clear</code> to remove all tracks
```

### ArcMusic — "/clear"  (`refs/ArcMusic/internal/modules/queue.go`)

```html
<i>Clear all tracks from the queue.</i>

<u>Usage:</u>
<b>/clear</b> — Remove all queued tracks

<b>⚙️ Behavior:</b>
• Removes all tracks from queue
• Current playing track continues
• Queue becomes empty after current track ends

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Tips:</b>
If you cleared the queue by mistake, use <code>/restore</code> or <code>/crestore</code> immediately to recover it (before adding any new songs).
```

### ArcMusic — "/move"  (`refs/ArcMusic/internal/modules/queue.go`)

```html
<i>Reorder tracks in the queue.</i>

<u>Usage:</u>
<b>/move [from] [to]</b> — Move track from position to position

<b>⚙️ Behavior:</b>
• Moves track at index 'from' to index 'to'
• Other tracks shift positions accordingly
• Indices start from 1

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/move 3 1</code> — Move 3rd track to 1st position
<code>/move 1 5</code> — Move 1st track to 5th position

<b>⚠️ Notes:</b>
• Both positions must be valid queue indices
• Use <code>/queue</code> to see current order
• Cannot move currently playing track
```

### ArcMusic — "/reload"  (`refs/ArcMusic/internal/modules/reload.go`)

```html
<i>Reload admin cache and refresh voice chat state.</i>

<u>Usage:</u>
<b>/reload</b> — Refresh all cached data

<b>🔄 What Gets Reloaded:</b>
• Chat admin list
• Voice chat status
• Assistant presence status
• Assistant ban status

<b>🔒 Flood Protection:</b>
• Regular users: 5 minute cooldown
• Admins: 2 minute cooldown

<b>💡 When to Use:</b>
• After promoting/demoting admins
• Voice chat issues
• Permission problems
• Bot behaving incorrectly

<b>⚠️ Notes:</b>
• May reset room state if admin permissions required
```

### ArcMusic — "/replay"  (`refs/ArcMusic/internal/modules/replay.go`)

```html
Restart the current track from the beginning.

<u>Usage:</u>
<b>/replay</b> — Restart current track

<b>⚙️ Behavior:</b>
• Resets position to 0:00
• Maintains speed setting
• Continues playback immediately

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this
```

### ArcMusic — "/restart"  (`refs/ArcMusic/internal/modules/restart.go`)

```html
<i>Restart the bot process.</i>

<u>Usage:</u>
<b>/restart</b> — Restart bot

<b>⚙️ Behavior:</b>
• Stops all active rooms
• Notifies all active chats
• Restarts bot process
• Clears download cache

<b>🔒 Restrictions:</b>
• <b>Owner only</b> command

<b>⚠️ Warning:</b>
All playback will be interrupted. Bot will be offline for a few seconds.
```

### ArcMusic — "/resume"  (`refs/ArcMusic/internal/modules/resume.go`)

```html
<i>Resume the paused playback.</i>

<u>Usage:</u>
<b>/resume</b> — Resume playback from pause

<b>⚙️ Behavior:</b>
• Continues from last paused position
• Cancels auto-resume timer if active

<b>⚠️ Notes:</b>
• Can only resume if currently paused
• Position is preserved during pause
• Speed settings remain active after resume
```

### ArcMusic — "stream"  (`refs/ArcMusic/internal/modules/rtmp_stream.go`)

```html
<i>Start RTMP live streaming to configured server.</i>

<u>Usage:</u>
<b>/stream &lt;query/URL&gt;</b> — Start streaming a track
<b>/stream [reply to audio/video]</b> — Stream replied media

<b>🎥 Features:</b>
• Live streaming to your RTMP server
• Supports audio and video
• Queue support (like /play)
• Real-time status monitoring

<b>⚙️ Setup Required:</b>
Before using this command, an admin must configure RTMP:
1. Open bot's private chat (DM)
2. Send: <code>/setrtmp &lt;chat_id&gt; &lt;rtmp_url&gt;</code>

<b>📝 Example Setup:</b>
In bot DM:
<code>/setrtmp -1001234567890 rtmps://dc5-1.rtmp.t.me/s/123:key</code>

Then in your chat:
<code>/stream never gonna give you up</code>

<b>⚠️ Important Notes:</b>
• RTMP streams have ~15-30s buffering delay
• Setup ONLY works in bot DM (for security)
• Use <code>/streamstop</code> to end stream
• Only admin/auth users can control streams
• We do NOT use Telegram's RTMP API - you provide your own server
```

### ArcMusic — "streamstop"  (`refs/ArcMusic/internal/modules/rtmp_stream.go`)

```html
<i>Stop current RTMP stream.</i>

<u>Usage:</u>
<b>/streamstop</b> — Stop the active stream

<b>⚠️ Note:</b>
Only admin/auth users can stop streams.
```

### ArcMusic — "streamstatus"  (`refs/ArcMusic/internal/modules/rtmp_stream.go`)

```html
<i>Check current RTMP stream status.</i>

<u>Usage:</u>
<b>/streamstatus</b> — Show stream information

<b>📊 Shows:</b>
• Stream state (playing/stopped)
• Current position
• RTMP server (masked for security)
• Configuration status
```

### ArcMusic — "setrtmp"  (`refs/ArcMusic/internal/modules/rtmp_stream.go`)

```html
<i>Configure RTMP streaming server (DM only).</i>

<u>Usage:</u>
<b>/setrtmp &lt;chat_id&gt; &lt;rtmp_url&gt;</b> — Set RTMP for a chat

<b>🔒 Security:</b>
• <b>This command ONLY works in DM</b> (private chat with bot)
• NEVER share RTMP credentials in groups
• Credentials are stored securely in database
• We do NOT use Telegram's RTMP API

<b>📋 URL Format:</b>
<code>rtmp://server/app/streamkey</code>
or
<code>rtmps://server/s/streamkey</code>

<b>📝 Examples:</b>

<b>Telegram Voice Chat:</b>
1. Start voice chat in your channel
2. Telegram gives you: <code>rtmps://dc5-1.rtmp.t.me/s/123:key</code>
3. In bot DM send: <code>/setrtmp -1001234567890 rtmps://dc5-1.rtmp.t.me/s/123:key</code>

<b>Custom RTMP Server:</b>
<code>/setrtmp -1001234567890 rtmp://live.example.com/stream/mykey</code>

<b>🔍 Getting Chat ID:</b>
• Forward message from chat to @userinfobot
• Or use <code>/id</code> command in the chat

<b>⚠️ Requirements:</b>
• You must be admin in target chat
• Bot must be member of target chat
• Command only works in bot's private chat

<b>💡 Why DM only?</b>
RTMP stream keys are like passwords. Configuring in DM prevents accidental exposure in group chats.
```

### ArcMusic — "/seek"  (`refs/ArcMusic/internal/modules/seek.go`)

```html
<i>Seek forward in the currently playing track.</i>

<u>Usage:</u>
<b>/seek [seconds]</b> — Skip forward by specified seconds

<b>⚙️ Features:</b>
• Jump ahead in current track
• Position tracking updated
• Cannot seek past track end (10s buffer)

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/seek 30</code> — Skip forward 30 seconds
<code>/seek 120</code> — Skip forward 2 minutes

<b>⚠️ Notes:</b>
• Minimum: any positive value
• Maximum: track_duration - current_position - 10 seconds
```

### ArcMusic — "/seekback"  (`refs/ArcMusic/internal/modules/seek.go`)

```html
<i>Seek backward in the currently playing track.</i>

<u>Usage:</u>
<b>/seekback [seconds]</b> — Go back by specified seconds

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/seekback 15</code> — Go back 15 seconds
<code>/seekback 60</code> — Go back 1 minute
```

### ArcMusic — "/jump"  (`refs/ArcMusic/internal/modules/seek.go`)

```html
<i>Jump to a specific position in the track.</i>

<u>Usage:</u>
<b>/jump [seconds]</b> — Jump to exact position

<b>⚙️ Features:</b>
• Absolute position seeking
• Precise time control
• 10-second buffer from end

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/jump 90</code> — Jump to 1:30
<code>/jump 0</code> — Jump to start (same as /replay)

<b>⚠️ Notes:</b>
• Position must be within track duration - 10 seconds
• More precise than <code>/seek</code> and <code>/seekback</code>
```

### ArcMusic — "/playmode"  (`refs/ArcMusic/internal/modules/settings.go`)

```html
<i>Control who can use the /play command in this chat.</i>

<u>Usage:</u>
<b>/playmode [enable|disable]</b> — Set play mode restriction

<b>⚙️ Options:</b>
• <b>enable</b> — Only admins and authorized users can play
• <b>disable</b> — Everyone can play (default)
```

### ArcMusic — "/shuffle"  (`refs/ArcMusic/internal/modules/shuffle.go`)

```html
<i>Toggle shuffle mode for the queue.</i>

<u>Usage:</u>
<b>/shuffle</b> — Show current shuffle state
<b>/shuffle on</b> — Enable shuffle
<b>/shuffle off</b> — Disable shuffle

<b>⚙️ Behavior:</b>
• Randomly reorders queue when enabled
• Affects track selection order
• Can be toggled at any time

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/shuffle on</code> — Enable shuffle mode
<code>/shuffle off</code> — Disable shuffle mode

<b>⚠️ Note:</b>
Shuffle only affects queue order, not currently playing track.
```

### ArcMusic — "/skip"  (`refs/ArcMusic/internal/modules/skip.go`)

```html
<i>Skip the currently playing track and play the next in queue.</i>

<u>Usage:</u>
<b>/skip</b> — Skip current track

<b>⚙️ Behavior:</b>
• Downloads next track in queue
• Starts playback automatically
• If queue is empty and loop is 0, stops playback

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>⚠️ Notes:</b>
• Cannot be undone
• If no tracks in queue, playback stops
• Loop count affects skip behavior
```

### ArcMusic — "/speed"  (`refs/ArcMusic/internal/modules/speed.go`)

```html
<i>Control playback speed (tempo).</i>

<u>Usage:</u>
<b>/speed</b> — Show current speed
<b>/speed [multiplier]</b> — Set speed (0.5-4.0x)
<b>/speed [multiplier] [seconds]</b> — Set with auto-reset timer
<b>/speed normal</b> or <b>/speed reset</b> — Reset to 1.0x

<b>⚙️ Features:</b>
• Range: 0.50x to 4.00x
• Auto-reset timer (5-3600 seconds)
• Pitch preservation
• Real-time adjustment

<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>💡 Examples:</b>
<code>/speed 1.5</code> — Play 1.5x faster
<code>/speed 0.75</code> — Play slower (0.75x)
<code>/speed 2.0 300</code> — 2x speed for 5 minutes, then reset
<code>/speed normal</code> — Reset to normal speed

<b>⚠️ Notes:</b>
• Speed affects duration calculations
• Auto-reset only works for non-1.0x speeds
• Suffix 'x' is optional: <code>1.5</code> = <code>1.5x</code>
```

### ArcMusic — "/speedtest"  (`refs/ArcMusic/internal/modules/speedtest.go`)

```html
<i>Run server network speed test.</i>

<u>Usage:</u>
<b>/speedtest</b> or <b>/spt</b> — Test network speed

<b>📊 Results Include:</b>
• Download speed (Mbps)
• Upload speed (Mbps)
• Server location
• Latency (ms)
• ISP information

<b>🔒 Restrictions:</b>
• <b>Sudo users</b> only

<b>⚠️ Note:</b>
Test may take 30-60 seconds to complete.
```

### ArcMusic — "/start"  (`refs/ArcMusic/internal/modules/start.go`)

```html
<i>Start the bot and show main menu.</i>
```

### ArcMusic — "/stats"  (`refs/ArcMusic/internal/modules/stats.go`)

```html
<i>View detailed bot statistics.</i>

<u>Usage:</u>
<b>/stats</b> — Show statistics

<b>📊 Information Shown:</b>
• System stats (OS, CPU, RAM, disk)
• Go runtime stats (memory, GC)
• Server resources
• Served chats count
• Served users count

<b>🔒 Restrictions:</b>
• <b>Sudo users</b> only
```

### ArcMusic — "/end"  (`refs/ArcMusic/internal/modules/stop.go`)

```html
<i>Stop playback and leave the voice chat.</i>

<u>Usage:</u>
<b>/stop</b> or <b>/end</b> — Stop playback

<b>⚙️ Behavior:</b>
• Stops current track
• Clears queue
• Assistant leaves voice chat
•
<b>🔒 Restrictions:</b>
• Only <b>chat admins</b> or <b>authorized users</b> can use this

<b>⚠️ Note:</b>
This action cannot be undone. Use <code>/pause</code> for temporary stops.
```

### ArcMusic — "/nothumb"  (`refs/ArcMusic/internal/modules/thumbnail.go`)

```html
Toggle thumbnail/artwork display in playback messages.

<u>Usage:</u>
<b>/nothumb</b> — Show current status
<b>/nothumb [enable|disable]</b> — Change setting

<b>⚙️ Behavior:</b>
• <b>Disabled (default):</b> Shows track artwork/thumbnail
• <b>Enabled:</b> Hides artwork, text-only messages

<b>💡 Examples:</b>
<code>/nothumb enable</code> — Disable thumbnails
<code>/nothumb disable</code> — Enable thumbnails

<b>⚠️ Note:</b>
This setting affects all future playback messages in this chat.
```

### ArcMusic — "/unmute"  (`refs/ArcMusic/internal/modules/unmute.go`)

```html
<i>Unmute the audio output in voice chat.</i>

<u>Usage:</u>
<b>/unmute</b> — Restore audio

<b>⚙️ Behavior:</b>
• Restores audio immediately
• Cancels auto-unmute timer if active
• Shows current playback info
```


## Appendix F — VERBATIM: Music163bot-Go `internal/render/render.go`

```go
// Package render 界面文案，统一 HTML parse mode，动态内容经转义
package render

import (
	"fmt"
	"html"
	"unicode/utf16"
)

const (
	InputKeyword     = "请输入搜索关键词"
	InputIDorKeyword = "请输入歌曲ID或歌曲关键词"
	InputIDorLink    = "请输入歌曲ID或歌曲分享链接"
	InputContent     = "请输入歌曲关键词/歌曲分享链接/歌曲ID"
	Searching        = "搜索中..."
	NoResults        = "未找到结果"
	NoCache          = "歌曲未缓存"
	TapToDownload    = "点击上方按钮缓存歌曲"
	TapMeToDown      = "点我缓存歌曲"
	HitCache         = "命中缓存, 正在发送中..."
	SendMeTo         = "Send me to..."
	GetLrcFailed     = "获取歌词失败, 歌曲可能不存在或为纯音乐"
	LrcNoProgram     = "电台节目不支持获取歌词"
	GetURLFailed     = "获取歌曲下载链接失败"
	FetchInfo        = "正在获取歌曲信息..."
	FetchInfoFailed  = "获取歌曲信息失败"
	WaitForDown      = "等待下载中..."
	Downloading      = "下载中..."
	Redownloading    = "下载失败，尝试重新下载中..."
	Uploading        = "下载完成, 发送中..."
	Md5VerFailed     = "MD5校验失败"
	RetryLater       = "请稍后重试"
	Reloading        = "重新加载中"
	CallbackText     = "Success"
	FetchingLyric    = "正在获取歌词中"
	DownloadFailed   = "下载失败"
	SendFailed       = "发送失败"
	ProcessFailed    = "文件处理失败"
	SearchExpired    = "搜索已失效，请重新搜索"
	NoMoreResults    = "没有更多结果了"
	SearchNotOwner   = "只有发起搜索的人可以操作"

	ReplyVoiceFirst     = "请先发送一条包含音乐片段的语音，再回复该语音发送 /recognize。"
	GuestUsage          = "回复一条网易云音乐分享链接或包含音乐片段的语音，并在回复中 @我，即可发送或识别歌曲。"
	GuestNoCache        = "歌曲未缓存，请点击下方按钮在 Bot 中获取"
	Recognizing         = "正在识别音乐，请稍候..."
	RecognizeNoResult   = "暂未识别到歌曲\n请尝试录制更清晰、稍长一些的音乐片段，再试一次。"
	RecognizeFailed     = "音乐识别暂时失败，请稍后重试。"
	RecognizeReadFailed = "暂时无法读取这条语音\n请稍后重试，或重新发送语音。"
	RecognizeSuccess    = "已识别到歌曲"
	InlineHelpHint      = "输入 help 获取帮助"
	InlineHelpStep1     = "1.粘贴音乐分享URL或输入MusicID"
	InlineHelpStep2     = "2.输入 search+关键词 搜索歌曲"
	InlineInputKeyword  = "请输入关键词"
	BotName             = "Music163bot-Go v3"

	LoginBusy        = "已有扫码登录会话进行中"
	LoginKeyFailed   = "获取二维码失败，请稍后重试"
	LoginExpired     = "二维码已过期，请重新发送 /login"
	LoginCheckFailed = "查询扫码状态失败，请稍后重试"
	LoginCancelled   = "已取消登录"
	CancelLogin      = "取消登录"

	LoginStartButton      = "扫码登录"
	LoginTimeoutStopped   = "登录超时，bot 已停止"
	LoginCancelledStopped = "已取消登录，bot 已停止"
	LoginStartupDone      = "Cookie 已生效"
	LoginSessionOver      = "登录会话已结束，可发送 /login"
	LoginNoSession        = "当前没有进行中的登录会话，可发送 /login"
	LoginQRSent           = "已发送登录二维码"

	InputChatID          = "缓存群 ID 须为数字，也可以在目标群内直接发送 /setcachechat"
	CacheChatTest        = "已设为缓存群，Guest Mode 点歌时会先把未缓存的歌曲上传到这里"
	CacheChatUnreachable = "无法向该聊天发送消息，请确认 bot 已加入并可以发言"
	CacheChatDisabled    = "已关闭缓存群"

	CookieExpired          = "Cookie 过期，请联系 bot 管理员"
	CookieExpiredAdminWarn = "Cookie 已失效，新歌下载已暂停，请发送 /login 重新登录"
	CookieResumedAdmin     = "Cookie 已生效，新歌下载已恢复"
)

func LoginStartupPrompt(expired bool, remainSec int) string {
	head := "Cookie 未配置，需登录"
	if expired {
		head = "Cookie 已失效，需重新登录"
	}
	return fmt.Sprintf("%s\n点击下方按钮扫码登录，或扫描终端内二维码\n请在 %d 秒内操作", head, remainSec)
}

func LoginStartupTerminal(expired bool, remainSec int) string {
	head := "Cookie 未配置，需登录"
	if expired {
		head = "Cookie 已失效，需重新登录"
	}
	return fmt.Sprintf("%s\n请使用网易云音乐 App 扫描下方二维码\n请在 %d 秒内操作", head, remainSec)
}

func LoginQRCaption(remainSec int) string {
	return fmt.Sprintf("请使用网易云音乐 App 扫码\n剩余有效期 %d 秒", remainSec)
}

func LoginScanned(nickname string) string {
	if nickname != "" {
		return "已扫码，等待确认: " + Escape(nickname)
	}
	return "已扫码，等待确认"
}

func LoginOK(nickname string) string {
	if nickname != "" {
		return "扫码登录成功: " + Escape(nickname)
	}
	return "扫码登录成功"
}

func Escape(s string) string { return html.EscapeString(s) }

func MusicStatus(name, album, ext string, sizeMB float64, suffix string) string {
	return fmt.Sprintf("%s\n专辑: %s\n%s %.2fMB\n%s", Escape(name), Escape(album), Escape(ext), sizeMB, suffix)
}

func DownloadStatus(speed string, writtenMB, totalMB float64, percent int64) string {
	return fmt.Sprintf(" %s\n%.2fMB/%.2fMB %d%%", Escape(speed), writtenMB, totalMB, percent)
}

func SongInfo(name, artists, album string) string {
	return fmt.Sprintf("「%s」- %s\n专辑: %s", Escape(name), Escape(artists), Escape(album))
}

func RecognizeResult(name, artists, album string) string {
	return "🎵 识别成功\n" + SongInfo(name, artists, album)
}

func GuestMusicResult(name, artists, album string) string {
	return SongInfo(name, artists, album) + "\n" + GuestNoCache
}

// LyricCaption caption 按 UTF-16 计算长度且不超过 1024
func LyricCaption(lyric string) string {
	const limit = 1024
	if truncateUTF16(lyric, limit) != lyric {
		lyric = truncateUTF16(lyric, limit-1) + "…"
	}
	return "<blockquote expandable>" + Escape(lyric) + "</blockquote>"
}

func truncateUTF16(s string, limit int) string {
	n := 0
	for i, r := range s {
		if n += utf16.RuneLen(r); n > limit {
			return s[:i]
		}
	}
	return s
}

func MusicCaption(name, artists, album, ext string, sizeMB, kbps float64, botName string) string {
	return fmt.Sprintf("%s\n#网易云音乐 #%s %.2fMB %.2fkbps\nvia @%s",
		SongInfo(name, artists, album), Escape(ext), sizeMB, kbps, Escape(botName))
}

func About(runtimeVer, versionName, commitSHA, buildTime, buildArch string) string {
	return fmt.Sprintf(`<b>Music163bot-Go v3</b>
Github: https://github.com/XiaoMengXinX/Music163bot-Go

[编译环境] %s
[编译版本] %s
[编译哈希] %s
[编译日期] %s
[运行环境] %s`,
		Escape(runtimeVer), Escape(versionName), Escape(commitSHA), Escape(buildTime), Escape(buildArch))
}

func StatusInfo(chatTitle, chatUsername string, chatID, userID, total, chatCount, userCount int64) string {
	var chatInfo string
	switch {
	case chatUsername != "" && chatTitle == "":
		chatInfo = fmt.Sprintf(`<a href="tg://user?id=%d">%s</a>`, chatID, Escape(chatUsername))
	case chatUsername != "":
		chatInfo = fmt.Sprintf(`<a href="https://t.me/%s">%s</a>`, Escape(chatUsername), Escape(chatTitle))
	default:
		chatInfo = Escape(chatTitle)
	}
	return fmt.Sprintf(`<b>[统计信息]</b>
数据库中总缓存歌曲数量: %d
当前对话 [%s] 缓存歌曲数量: %d
当前用户 [<a href="tg://user?id=%d">%d</a>] 缓存歌曲数量: %d`,
		total, chatInfo, chatCount, userID, userID, userCount)
}

func CacheChatSet(chatID int64) string {
	return fmt.Sprintf("缓存群已设置为 <code>%d</code>", chatID)
}

func RmcacheReport(name string) string {
	return fmt.Sprintf("清除 [%s] 缓存成功", Escape(name))
}

func Cooldown(seconds int) string {
	return fmt.Sprintf("操作太频繁，请 %d 秒后再试", seconds)
}

func SearchPage(page, total int) string {
	return fmt.Sprintf("<code>第[%d/%d]页</code>\n", page, total)
}

func SearchItem(n int, name, artists string) string {
	return fmt.Sprintf("<code>%-3s</code> <code>%s - %s</code>\n", fmt.Sprintf("%d.", n), Escape(name), Escape(artists))
}

func SongLink(musicID int) string {
	return fmt.Sprintf("https://music.163.com/song/%d", musicID)
}

func ProgramLink(programID int) string {
	return fmt.Sprintf("https://music.163.com/program?id=%d", programID)
}
```
