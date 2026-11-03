# Parlay — Unified Command Inventory

Date: 2026-11-02 (Phase 4); updated 2026-11-03 (Phase 6, Phase 9,
Phase 9 gaps — coordinator rulings on port-engines-gaps.md).
Deduped from NoxBeats (15 modules, 35 `c*` channel commands — 34 +
`/caskip`, Phase 6), Yukki-Go / ArcMusic / TgMusicBot, TGVCBot /
remix, ParvatiMusic. Implemented in
`services/core-cpp/src/dispatcher.cpp` (8,000 lines, route table
with Scope/Perm + full alias map) with all core-facing strings in
`services/core-cpp/src/ui_texts.hpp` (3,256 lines). Every command
dispatches to a real IPC call or a real Store action — no stub
replies. Contract `contracts/ipc.json` v0.3.1 (56 actions).

Permission levels: **O**wner > **S**udo > **A**dmin/auth-user >
**E**veryone. `admin_mode` 0/1/2 and `play_mode` (admin/everyone)
gate playback control; maintenance → sudo only; 1 cmd/s per-chat
rate limit. Channel commands (`c*`) run the same command against a
linked channel (`via_channel=true`, 34 commands).

## Playback

| Command | Args | Who | Implements (IPC / service) |
|---|---|---|---|
| `/play` | `<song, link or id>` (`-f` force, `-v` video) | play_mode | `vc.play` → vc-go/vc-ts/vc-rust (per-chat engine) |
| `/playforce` | same as play, forces | play_mode | `vc.play` (force) |
| `/vplay`, `/vplayforce` | `<...>` video in VC | play_mode | `vc.play` (video) |
| `/playplaylist` | — | play_mode | playlist → `vc.play` |
| `/pause`, `/resume` | — | play_mode | `vc.pause` / `vc.resume` |
| `/skip`, `/next` | — | play_mode | `vc.skip` (resets loop) |
| `/stop`, `/end` | — | play_mode | `vc.stop` |
| `/join` | — | admin | `vc.join` (core-cpp TDLib backend; needs TD_* + libntgcalls.so) |
| `/loop` | `[n 0-10 \| off]` | play_mode | `vc.loop` |
| `/seek` | `<sec>` (min 10) | play_mode | `vc.seek` |
| `/seekback` | `<sec>` | play_mode | `vc.seek` (back) |
| `/jump` | `<mm:ss>` | play_mode | `vc.seek` |
| `/speed` | `[0.5–4.0]` | play_mode | stored; applied for real on core-voice sessions (replied files — ffmpeg `atempo` re-attach at the current position; a single `atempo` covers 0.5–4.0, no chaining), seek-restart only on engine sessions |
| `/bass` | `[0–100]` | play_mode | stored; applied for real on core-voice sessions (lowpass mix), seek-restart only on engine sessions |
| `/mute`, `/unmute` | — | admin | `vc.mute` on core-owned sessions (real ntgcalls mute); engine-owned sessions honestly refuse |
| `/replay` | — | play_mode | `vc.seek` 0 |
| `/player` | — | E | now-playing card (photo when thumbnail on, 5-row control keyboard, 9-stage `◉` progress bar) |
| `/menu` | — | admin | remix control panel (current + next track, merged control keyboard; panel presses are requester-only) |
| `/now` | — | E | current track + requester |

## Queue

| Command | Args | Who | Implements |
|---|---|---|---|
| `/queue`, `/playing` | — | E | `vc.queue` list / `vc.now_playing` |
| `/clear` | — | play_mode | lists queue first, then `vc.queue` clear (so `/restore` works) |
| `/restore` | — | play_mode | re-adds cleared queue, skips still-playing track |
| `/remove` | `<n\|n-m\|n,m,...>` | play_mode | `vc.queue` remove (single, range, or list; 1-based up-next numbers) |
| `/move` | `<a> <b>` | play_mode | `vc.queue` move |
| `/shuffle` | — | play_mode | `vc.shuffle` |
| `/pos` | — | E | current position |
| `/autoplay` | `[on\|off\|lang\|mood]` | admin | `vc.autoplay` + background watcher |
| `/autoplay-skip`, `/askip` | — | play_mode | `vc.autoplay_skip` |

## Playlists

| Command | Args | Who | Implements |
|---|---|---|---|
| `/addplaylist` | `<song\|url>` | E | Store (max 100/user) |
| `/playlist` | — | E | Store list |
| `/rmplaylist` | `<n\|name\|all>` | E | Store remove |
| `/clearplaylist` | — | E | Store clear |
| `/createplaylist` | `<name>` | E | Store named playlist (max 10/user, id = 12-hex) |
| `/deleteplaylist` | `<name\|id>` | E | Store named playlist delete |
| `/addtoplaylist` | `<playlist> <song\|link>` | E | Store named playlist add (resolved via search) |
| `/removefromplaylist` | `<playlist> <n>` | E | Store named playlist remove |
| `/playlistinfo` | `<name\|id>` | E | Store named playlist info card |
| `/myplaylists` | — | E | Store named playlist list |

## Search / download / lyrics / radio / share

| Command | Args | Who | Implements |
|---|---|---|---|
| `/search` | `<query>` | E | `search.query` (vc-ts; `ne:` → NetEase); numbered buttons (5/row), 10/page pagination, tap-to-play, or reply with the result number (5 s per-user cooldown) |
| `/download` | `<song or link>` | E | `download.youtube` / `download.spotify` / `download.deezer` / `download.tiktok` / `download.navidrome`; in-place progress message, Spotify playlist/album → zip-or-one-by-one picker, lossy-fallback prompt, FX prompt after delivery |
| `/lyrics` | `[song]` | E | current track → card (chunked at 4096); named song → `.lrc` document with expandable lyric caption (NetEase); when `netease.lyrics` returns `lrc_path` the service-written file is sent, otherwise the core writes its own stripped copy |
| `/radio` | `[station]` | play_mode | `vc.radio` → radio-js → VC engine handoff |
| `/new` | `<station>` | admin | `radio.start` (group only) |
| `/destroy` | — | admin | `radio.stop` (group only) |
| `/manage` | — | admin | `radio.status` card (group only; no listener counts — radio-js reports 0 always, so the card omits them) |
| `/share` | `<song>` | E | `share.create` → anonymous link; core keeps creator→token mapping for `/start share_<token>` and My Link |
| `/engine` | `[go\|ts\|rust]` | admin | per-chat engine switch (durable, Store) |

## Settings (chat)

| Command | Args | Who | Implements |
|---|---|---|---|
| `/playmode` | `[admin\|everyone]` | admin | Store |
| `/settings` | — | admin | inline settings panel (toggles + timer 2–7 min, group only) |
| `/download-settings` | — | E | per-user download prefs (format / YouTube mode / path template); template set by free text afterwards (prefs stored, not yet sent to the downloaders — no contract fields) |
| `/adminmode` | `[admin\|adminauth\|everyone]` | admin | Store |
| `/cmddelete` | `[on\|off]` | admin | Store |
| `/cleanmode` | `[on\|off\|minutes 1–1440]` | admin | Store + background sweeper |
| `/thumbnail`, `/nothumb` | `[on\|off]` | admin | Store |
| `/channelplay` | `<@channel\|id\|disable>` | admin | Store |
| `/userbot` | — | admin | assistant status |
| `/autoleave` | — | admin | Store |
| `/lang` | — | admin | English-only (no locale files yet) |

## Admin / sudo / auth

| Command | Args | Who | Implements |
|---|---|---|---|
| `/auth`, `/unauth` | `<user>` (reply) | admin | Store auth users (max 25/chat, revoke buttons in `/authlist`) |
| `/authlist` | — | admin | Store list (rich, with per-user revoke buttons) |
| `/admincache` | — | admin | refresh admin list (600 s cooldown) |
| `/blacklist`, `/unblacklist`, `/whitelist` | `<user\|chat>` | sudo | Store |
| `/blacklisted` | — | sudo | Store list |
| `/addsudo`, `/delsudo`, `/listsudo` | `<user>` | owner | Store sudoers |
| `/ac`, `/activevc` | — | sudo | list active voice chats |
| `/broadcast` | reply + `[-copy\|-nochat\|-user]` | sudo | fan-out, 200 ms spacing, failures as `errors.txt` attachment (inline fallback if the upload fails); cancel button on the progress message |
| `/maintenance` | `[on\|off]` | sudo | Store |
| `/restart` | — | owner | confirm card (Restart / Replay) while a song plays, else exit 42 |
| `/eval` | `<shell>` | owner | real `sh -c`, 60 s timeout |
| `/logger`, `/logs` | — | sudo | log tail |

## Tools / stream

| Command | Args | Who | Implements |
|---|---|---|---|
| `/ping`, `/alive` | — | E | ping card (latency, uptime, RAM/CPU/disk; PING_IMG photo when set) |
| `/stats` | — | E | stats card (system / memory / server / bot / versions) |
| `/id` | — | E | this chat's id |
| `/keyboard` | `[on\|off]` | E | persistent reply keyboard |
| `/about` | — | E | about card |
| `/status` | — | E | cached-song status (core keeps no song cache — counts are 0) |
| `/repo` | — | E | source-code link button |
| `/json` | — | sudo | raw update JSON |
| `/bug` | `<text>` | E | report |
| `/privacy` | — | E | privacy note |
| `/speedtest` | — | sudo | download + upload speed test (Cloudflare `__down`/`__up`); staged "Running download/upload test..." notices; a failed upload probe is reported honestly next to the download number |
| `/setrtmp`, `/stream`, `/streamstop`, `/streamstatus` | `<key\|url>` | admin | ffmpeg fork/exec, one child per chat, key scrubbed |
| `/start`, `/help` | — | E | start card / help menu + topics + per-command help (`/help` works in groups too — it replies with a DM prompt); `/start spotify----<id>` downloads the track named by an inline search result (deep link from the `[Open in Parlay Bot]` button) |

## Callback namespace (Phase 9)

Every inline button the core sends uses `pl:<domain>:<verb>[:<arg>…]`
(UI_INVENTORY.md §1, 64-byte limit, colon-separated). Domains:
`help`, `start`, `close`, `player`, `queue`, `set`, `lang`, `dl`,
`bcast`, `auth`, `search`, `vote`, `share`, `del`, `lossy`,
`restart`, `dlset`, `spotify`. Unknown or over-long callbacks get a
polite alert, never a crash. Card presses are admin-or-requester
gated (sudo / chat admin / auth user / `admin_mode` 2); `/menu`
panel presses are requester-only and silently ignored for others.

## Inline mode (Phase 9 gaps, §13)

The core answers `inline_query` updates itself (`answerInlineQuery`,
`getUpdates` asks for `inline_query`). Modes mirror vc-ts
`resolvers.js inlineMode`: `np` / `s` / `l` / `help` match the whole
query; `lib` / `del` / `search` take an argument after a space; empty
or <3-char queries get the hint button (`np  s  l  lib  del — or type
to search`, `start_parameter=help`). `search.inline` request carries
optional `chat_id` (contract v0.3.1) — the core passes it for `np` /
`s` / `l` only, taken from `last_chat:<user>` (the user's most recent
group chat, recorded on group text/commands and callback presses; an
inline query carries no chat id). `l` fetches the lyrics text via
`/v1/lyrics` with the same `chat_id` and renders the article
(`Lyrics: {title}`, first 100 chars in the description, plain lyrics
truncated to 4000 chars + `\n...`). `lib` / `del` return empty results
(no library backend). `help` → two articles sending `Parlay v0.2.0`;
bare `search` → `Please enter a keyword`; no matches → `No results
found`. Search tracks render as audio results when a `stream_url` is
known, otherwise as articles — both with the `[Open in Parlay Bot]`
button (`https://t.me/{bot}?start=spotify----{track id}`); the last
search results are kept per user (`inlinesearch:<user>`) so the deep
link can resolve the track to a URL. Blacklisted users and (under
maintenance) non-sudo users get empty results, silently. `np` renders
a plain audio result, not cached audio — the Track carries no
`file_id`.

## TS / Rust extras (same commands, engine-local)

- TGVCBot/remix: reply-to-audio play (`telegram` provider),
  per-user search buttons, search sessions, admin-gated panel
  (`panel_shuffle/skip/pause/resume/mute/unmute/voldec/volinc/update`).
- ParvatiMusic: `c*`/`f*`/`v*` command variants, VC ops,
  moderation, lists, system, sticker commands — mapped onto the
  table above (variants are aliases in the core route table).

## Not yet (named, with reason)

- `/volume`: no core command exists (removed from the table above, Phase 6
  — it was listed but never routed). Volume lives as vc-ts IPC
  (`POST /v1/volume`, 0–200 remix scale) and the `/menu` panel +
  now-playing card (`volinc`/`voldec` buttons, `vc.volume` IPC);
  a core `/volume` command is a small follow-up.
- `/mix`, `/recognize`, `/scan`, `/sharescan`, `/rmcache`: no IPC
  action exists for any of them (Phase 9) — the UI strings are in
  `ui_texts.hpp` but no command is routed; they need an engine-side
  action first.
- Download cancel (`pl:dl:cancel`): the button is shown, but no
  `download.cancel` IPC action exists — taps answer "feature
  unavailable" honestly. Live per-second progress polling is still
  impossible by design: the downloaders POST blocks until the
  transfer completes, so the run id is only known after the POST
  returns. Contract v0.3.1 now documents `run_id` (12-hex) on all
  seven download POST responses, and the core already reads it for
  the post-POST IPC-4 snapshot — the remaining step for live polling
  is an async download POST on the engines side, not a core change.
  The progress message is edited in place (fetching → outcome) and
  the final snapshot is fetched after the POST.
- Inline `lib` / `del` modes return empty results (no library
  backend in vc-ts); the core does not render them as functional.
  Needs a `library.search` / `library.delete` action first.
- Spotify Hide-alerts button: not rendered — `ignored_by` stays 0,
  no ignore store or endpoint exists. Needs a `spotify.ignore`
  action or a `spotify.dislike` extension.
- Share feedback/report dedup: the buttons send feedback and block
  on report with no dedup (no "already sent" / "already reported"
  message) — no feedback/report store or endpoint exists. Dedup is
  future work.
- `vc.lyrics` method mismatch (flagged, not changed): the contract
  writes `GET /v1/lyrics`, but vc-ts implements it POST-only (the
  route sits behind the POST-only gate in `main.js`) — the core calls
  POST with body `{chat_id}`, the only form that works.
- Spotify `language` stays null and `features` is null when the
  deprecated audio-features endpoint 403s — the core renders no
  `/details` card (spotify-rust owns §25), so the null-hiding rules
  apply to that renderer, not the core.
- Inline `Song not cached` article: not rendered — the cache-on-tap
  flow (button → `/start` with a music ID) does not exist in the
  core; empty searches render `No results found` instead.
- Granular download progress is yt-dlp / Soulseek only; other
  providers report phase transitions. The core renders what the
  IPC-4 payload gives and never estimates a percentage (it renders
  no live percentage today — the progress message is edited to the
  outcome only).
- Library delete execution (`pl:del:yes`): the confirm card is real,
  but no `library.delete` IPC action exists — taps answer "feature
  unavailable" honestly.
- Spotify companion surfaces (`/details`, `/analyze`, Magic /
  Recommendation toggles): owned by spotify-rust; the core only
  routes `spotify.dislike` (`pl:spotify:dislike:<id>`).
- `/settings` download prefs (format / YouTube mode / template) are
  stored per user by the core but not sent to the downloaders — the
  contract's download actions take no such fields (needs a contract
  addition).
- Private-bot notice: the string exists but no private-mode config
  exists in core-cpp, so `/start` shows the normal card.
- `/status` counts are 0: the core keeps no song cache.
- `/play` from replied-to media: implemented (Phase 5) — core takes the `file_id` from `reply_to_message` (audio/document/voice/video_note) and plays it through the core voice backend.
- `/speed` / `/bass` audio filter on engine sessions: seek-restart +
  stored value; vc-go/vc-ts/vc-rust do not apply the filter (see
  REMAINING.md B11 — vc-go has no audio pipeline at all, and vc-ts /
  vc-rust run fixed ffmpeg args with no `-af`). On core-voice
  sessions (replied-file playback) the filter IS applied: the core
  backend re-attaches the source with ffmpeg `atempo` / lowpass at
  the current position, and stored values apply to later core plays.
- `/broadcast` errors file: implemented (Phase 6 Worker C) — failures
  go out as an `errors.txt` attachment via real Bot API `sendDocument`
  (multipart upload); inline list only if the upload itself fails.
