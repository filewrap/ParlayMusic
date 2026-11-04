# Parlay — Unified Command Inventory

Date: 2026-11-02 (Phase 4); updated 2026-11-03 (Phase 6, Phase 9,
Phase 9 gaps — coordinator rulings on port-engines-gaps.md);
updated 2026-11-03 (Phase 4, Worker C — code-grounded pass against
`services/core-cpp/src/dispatcher.cpp` + `services/backend-go`).
Deduped from NoxBeats (15 modules, 35 `c*` channel commands,
including `/caskip`, Phase 6), Yukki-Go / ArcMusic / TgMusicBot, TGVCBot /
remix, ParvatiMusic. Implemented in
`services/core-cpp/src/dispatcher.cpp` (8,880 lines, route table
with Scope/Perm + full alias map) with all core-facing strings in
`services/core-cpp/src/ui_texts.hpp` (2,376 lines). Every command
dispatches to a real UDS call or a real Store action — no stub
replies — except `/speedtest` (answers honestly unavailable: no
backend action exists yet) and retired `/engine`. Contract
`contracts/ipc.json` v0.3.1 (65 actions).

Permission levels: **O**wner > **S**udo > **A**dmin/auth-user >
**E**veryone. `admin_mode` 0/1/2 and `play_mode` (admins/everyone)
gate playback control; maintenance → owner only; token-bucket rate
limit (per-chat 5 burst / 1.0/s refill, per-user 3 / 1.0/s). Channel commands (`c*`) run the same command against a
linked channel (`via_channel=true`, 35 commands).

`play_mode` gates `/play` (and its force/video variants) only.
Every other playback/queue command uses the admin gate
(`canManageVc`: sudo, chat admin, auth user, or everyone when
`admin_mode` is 2). Secondary names also registered but not listed
below: `/lyric`, `/language`, `/thumb`, `/reload`, `/rmsudo`,
`/sudolist`, `/ev`, `/sh`, `/exec` (plus ~70 aliases — see the
`aliases_` map in `registerCommands`).

## Playback

| Command | Args | Who | Implements (IPC / service) |
|---|---|---|---|
| `/play` | `<song, link or id>` (`-f` force, `-v` video) | play_mode | `vc.play` → backend-go (single backend, UDS) |
| `/playforce` | same as play, forces | play_mode | `vc.play` (force) |
| `/vplay`, `/vplayforce` | `<...>` video in VC | play_mode | `vc.play` (video) |
| `/playplaylist` | — | play_mode | playlist → `vc.play` |
| `/pause`, `/resume` | — | admin | `vc.pause` / `vc.resume` |
| `/skip`, `/next` | — | admin | `vc.skip` (resets loop) |
| `/stop`, `/end` | — | admin | `vc.stop` |
| `/join` | — | admin | `vc.join` UDS; the backend drives the JoinRequest handshake and the core runs TDLib joinVideoChat on the assistant session |
| `/loop` | `[n 0-10 \| off]` | admin | `vc.loop` |
| `/seek` | `<sec>` (min 10) | admin | `vc.seek` |
| `/seekback` | `<sec>` | admin | `vc.seek` (back) |
| `/jump` | `<mm:ss>` | admin | `vc.seek` |
| `/speed` | `[0.5–4.0]` | admin | stored per chat; NOT applied — the stream restarts at the current position at the old speed; live filtering is the Phase 5 backend DSP |
| `/bass` | `[0–100]` | admin | stored per chat; NOT applied — the stream restarts at the current position with the old bass; live filtering is the Phase 5 backend DSP |
| `/mute`, `/unmute` | — | admin | `vc.mute` UDS (backend ntgcalls mute) |
| `/replay` | — | admin | `vc.seek` 0 |
| `/player` | — | E | now-playing card (photo when thumbnail on, 5-row control keyboard, 9-stage `◉` progress bar) |
| `/menu` | — | admin | remix control panel (current + next track, merged control keyboard; panel presses are requester-only) |
| `/now` | — | E | current track + requester |

## Queue

| Command | Args | Who | Implements |
|---|---|---|---|
| `/queue`, `/playing` | — | E | `vc.queue` list / `vc.now_playing` |
| `/clear` | — | admin | lists queue first, then `vc.queue` clear (so `/restore` works) |
| `/restore` | — | admin | re-adds cleared queue, skips still-playing track |
| `/remove` | `<n\|n-m\|n,m,...>` | admin | `vc.queue` remove (single, range, or list; 1-based up-next numbers) |
| `/move` | `<a> <b>` | admin | no `move` verb in the contract — the core implements it as `vc.queue` list → remove → re-add at the target position |
| `/shuffle` | — | admin | `vc.shuffle` |
| `/pos` | — | E | current position |
| `/autoplay` | `[on\|off\|lang\|mood]` | admin | Store only (core `autoplay` / `ap_lang` / `ap_mood` chat fields); never calls `vc.autoplay` — the backend room is not told |
| `/autoplay-skip`, `/askip` | — | admin | `vc.autoplay_skip` |

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
| `/search` | `<query>` | E | `search.query` (backend-go; `ne:` → NetEase); numbered buttons (5/row), 10/page pagination, tap-to-play, or reply with the result number (5 s per-user cooldown) |
| `/download` | `<song or link>` | admin | `download.youtube` / `download.spotify` / `download.deezer` / `download.tiktok` / `download.navidrome`; in-place progress message, Spotify playlist/album → zip-or-one-by-one picker, lossy-fallback prompt, FX prompt after delivery |
| `/lyrics` | `[song]` | E | current track → card (chunked at 4096); named song → `.lrc` document with expandable lyric caption (NetEase); when `netease.lyrics` returns `lrc_path` the backend-written file is sent, otherwise the core writes its own stripped copy |
| `/radio` | `[station]` | admin | `radio.list` (no arg) / `radio.start` + `vc.play` of the stream URL (station) |
| `/new` | `<station>` | admin | `radio.start` (group only) |
| `/destroy` | — | admin | `radio.stop` (group only) |
| `/manage` | — | admin | `radio.status` card (group only; no listener counts — the backend reports 0 always, no listener accounting exists, so the card omits them) |
| `/share` | `<song>` | E | `share.create` → anonymous link; core keeps creator→token mapping for `/start share_<token>` and My Link |
| `/engine` | `[anything]` | admin | retired — answers that there is one unified backend and no engine to switch (arguments ignored) |

## Settings (chat)

| Command | Args | Who | Implements |
|---|---|---|---|
| `/playmode` | `[admin\|everyone]` | admin | Store |
| `/settings` | — | admin | inline settings panel (toggles + timer 2–7 min, group only); Quality section (bitrate / frame size) renders from `quality.get` and writes `quality_*` overrides via `chat.settings` |
| `/quality` | — | admin | quality card: effective Opus values (bitrate / frame size / complexity / VBR / FEC / DTX) with source per field (store / config file / profile / default) + "applies on next join" note; group only; backend `quality.get` (core UI: routing-table C102, Contract) |
| `/download-settings` | — | E | per-user download prefs (format / YouTube mode / path template); template set by free text afterwards (prefs stored, not yet sent to the backend — the `download.prefs` UDS action exists but the core does not call it yet) |
| `/adminmode` | `[admin\|adminauth\|everyone]` | admin | Store |
| `/cmddelete` | `[on\|off]` | admin | Store |
| `/cleanmode` | `[on\|off\|minutes 1–1440]` | admin | Store + background sweeper |
| `/thumbnail`, `/nothumb` | `[on\|off]` | admin | Store |
| `/channelplay` | `<@channel\|id\|disable>` | admin | Store |
| `/userbot` | — | E | assistant status |
| `/autoleave` | — | sudo | Store |
| `/lang` | — | admin | English-only (no locale files yet) |

## Admin / sudo / auth

| Command | Args | Who | Implements |
|---|---|---|---|
| `/auth`, `/unauth` | `<user>` (reply) | admin | Store auth users (max 25/chat, revoke buttons in `/authlist`) |
| `/authlist` | — | admin | Store list (rich, with per-user revoke buttons) |
| `/admincache` | — | admin | refresh admin list (600 s cooldown) |
| `/blacklist`, `/unblacklist`, `/whitelist` | `<user\|chat>` | sudo | Store |
| `/blacklisted` | — | sudo | Store list |
| `/addsudo`, `/delsudo` | `<user>` | owner | Store sudoers |
| `/listsudo` | — | E | Store sudoers (anyone may list) |
| `/ac`, `/activevc` | — | sudo | list active voice chats |
| `/broadcast` | reply + `[-copy\|-nochat\|-user]` | sudo | fan-out, 200 ms spacing, failures as `errors.txt` attachment (inline fallback if the upload fails); cancel button on the progress message |
| `/maintenance` | `[on\|off]` | owner | Store |
| `/restart` | — | sudo | confirm card (Restart / Replay) while a song plays, else exit 42 |
| `/eval` | `<shell>` | owner | real `sh -c`, 60 s timeout |
| `/logs` | — | sudo | log tail |
| `/logger` | `[on\|off]` | sudo | toggles log forwarding to the configured logger chat (on with no logger id answers honestly) |

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
| `/json` | — | E | raw update JSON |
| `/bug` | `<text>` | E | report |
| `/privacy` | — | E | privacy note |
| `/speedtest` | — | sudo | answers honestly unavailable — the network probes need a backend action that does not exist yet; run a speed test from the host shell instead |
| `/setrtmp` | `<url> <key>` | E (DM only) | stores the RTMP target + key (key never echoed back) |
| `/stream`, `/streamstop` | `<key\|url>` | admin | ffmpeg fork/exec, one child per chat, key scrubbed |
| `/streamstatus` | — | E | reports the per-chat ffmpeg child, if any |
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
via the TDLib poll loop — `getUpdates` is gone). Modes mirror the
backend's `search.inline` (`internal/search/inline.go`): `np` / `s` /
`l` / `help` match the whole
query; `lib` / `del` / `search` take an argument after a space; empty
or <3-char queries get the hint button (`np  s  l  lib  del — or type
to search`, `start_parameter=help`). `search.inline` request carries
optional `chat_id` (contract v0.3.1) — the core passes it for `np` /
`s` / `l` only, taken from `last_chat:<user>` (the user's most recent
group chat, recorded on group text/commands and callback presses; an
inline query carries no chat id). `l` fetches the lyrics text via
the `vc.lyrics` UDS action with the same `chat_id` and renders the article
(`Lyrics: {title}`, first 100 chars in the description, plain lyrics
truncated to 4000 chars + `\n...`). `lib` / `del` return empty results
(no library backend). `help` → two articles sending `Parlay v0.3.0`;
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
  — it was listed but never routed). Volume lives as the backend
  `vc.volume` UDS action (0–200 scale) and the `/menu` panel +
  now-playing card (`volinc`/`voldec` buttons, `vc.volume` UDS);
  a core `/volume` command is a small follow-up.
- `/mix`, `/recognize`, `/scan`, `/sharescan`, `/rmcache`: no command
  is routed for any of them — but the UDS actions now exist
  (`mix.generate`, `audio.recognize` (AudD, env-gated),
  `library.scan`, `library.sharescan`, `cache.clear`); the remaining
  step is routing the commands, not building the actions.
- Download cancel (`pl:dl:cancel`): the button is shown, and the
  `download.cancel` UDS action now exists (OD-3). Downloads still
  run synchronously (no `async` flag, no `DownloadReady` event —
  nothing emits one); the progress message is edited in place
  (fetching → outcome) and the final snapshot is fetched after the
  call returns. The remaining step is wiring `cbDl` to
  `download.cancel` (Q4), not a backend change.
- Inline `lib` / `del` modes return empty results (no library
  backend behind them yet — the code only reaches the search-results
  path `if a future library backend returns tracks`). The library
  actions exist (`library.scan` / `library.delete` UDS), but the
  inline path does not call them.
- Spotify Hide-alerts button: not rendered — `ignored_by` stays 0,
  no ignore store or endpoint exists. Needs a `spotify.ignore`
  action or a `spotify.dislike` extension.
- Share feedback/report dedup: the buttons send feedback and block
  on report with no dedup (no "already sent" / "already reported"
  message) — no feedback/report store or endpoint exists. Dedup is
  future work.
- `vc.lyrics` method mismatch: closed by the UDS transport — there
  are no HTTP methods anymore; `vc.lyrics` is one UDS action
  (`internal/lyrics` in the backend).
- Spotify `language` stays null and `features` is null when the
  deprecated audio-features endpoint 403s — the core DOES render the
  details card (`cbSpotify` → `spotifyDetailsText`, backend-go:spotify
  data) and hides the nulls itself.
- Inline `Song not cached` article: not rendered — the cache-on-tap
  flow (button → `/start` with a music ID) does not exist in the
  core; empty searches render `No results found` instead.
- Granular download progress is yt-dlp / Soulseek only; other
  providers report phase transitions. The core renders what the
  IPC-4 payload gives and never estimates a percentage (it renders
  no live percentage today — the progress message is edited to the
  outcome only).
- Library delete execution (`pl:del:yes`): the confirm card is real,
  and the `library.delete` UDS action exists (OD-3) — but the core
  does not call it yet, so taps answer "feature unavailable"
  honestly (Q4: wire it or keep answering unavailable).
- Spotify companion surfaces (`/details`, `/analyze`): owned by the
  backend's `internal/spotify` (`spotify.details` /
  `spotify.analyze` UDS actions); the core routes all three verbs
  (`cbSpotify`: `dislike`, `details`, `analyze`) and renders the
  details/analyze cards in place.
- `/settings` download prefs (format / YouTube mode / template) are
  stored per user by the core but not sent to the backend — the
  contract HAS `download.prefs` (OD-3), but the core does not call it
  yet (same gap as the `/download-settings` row above).
- Private-bot notice: the string exists but no private-mode config
  exists in core-cpp, so `/start` shows the normal card.
- `/status` counts are 0: the core keeps no song cache.
- `/play` from replied-to media: implemented — core takes the `file_id` from `reply_to_message` (audio/document/voice/video_note) and plays it through the `vc.play_file` UDS action.
- `/speed` / `/bass` audio filter: stored per room only, applied by
  nothing yet — the Phase 5 DSP applies them (see
  `internal/room` `SetSpeed`: "Phase 3 stores and reports it only").
  There is no `atempo` / lowpass path in the backend today, and the
  old core-side re-attach is gone with `attachEngineAudio`.
- `/broadcast` errors file: implemented — failures
  go out as an `errors.txt` attachment via real Bot API `sendDocument`
  (multipart upload); inline list only if the upload itself fails.
