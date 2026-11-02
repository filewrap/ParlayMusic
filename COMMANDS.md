# Parlay — Unified Command Inventory

Date: 2026-11-02 (Phase 4). Deduped from NoxBeats (15 modules, 34
`c*` channel commands), Yukki-Go / ArcMusic / TgMusicBot, TGVCBot /
remix, ParvatiMusic. Implemented in `services/core-cpp/src/dispatcher.cpp`
(3,199 lines, route table with Scope/Perm + full alias map). Every
command dispatches to a real IPC call or a real Store action — no
stub replies.

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
| `/loop` | `[n 0-10 \| off]` | play_mode | `vc.loop` |
| `/seek` | `<sec>` (min 10) | play_mode | `vc.seek` |
| `/seekback` | `<sec>` | play_mode | `vc.seek` (back) |
| `/jump` | `<mm:ss>` | play_mode | `vc.seek` |
| `/speed` | `[0.5–2.0]` | play_mode | stored + seek-restart |
| `/bass` | `[0–100]` | play_mode | stored + seek-restart (lowpass) |
| `/mute`, `/unmute` | — | admin | flag-only (no vc mute IPC action yet) |
| `/replay` | — | play_mode | `vc.seek` 0 |
| `/player` | — | E | now-playing panel |
| `/volume` | `[0–200]` | play_mode | `vc-ts` volume / panel (remix scale) |

## Queue

| Command | Args | Who | Implements |
|---|---|---|---|
| `/queue`, `/playing` | — | E | `vc.queue` list / `vc.now_playing` |
| `/clear` | — | play_mode | lists queue first, then `vc.queue` clear (so `/restore` works) |
| `/restore` | — | play_mode | re-adds cleared queue, skips still-playing track |
| `/remove` | `<n>` | play_mode | `vc.queue` remove |
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

## Search / download / lyrics / radio / share

| Command | Args | Who | Implements |
|---|---|---|---|
| `/search` | `<query>` | E | `search.query` (vc-ts; `ne:` → NetEase) |
| `/download` | `<song or link>` | E | `download.youtube` / `download.spotify` / `download.deezer` / `download.tiktok` / `download.navidrome` |
| `/lyrics` | `[song]` | E | `vc-ts` lyrics (LRCLib) / NetEase lyrics |
| `/radio` | `[station]` | play_mode | `vc.radio` → radio-js → VC engine handoff |
| `/share` | `<song>` | E | `share.create` → anonymous link |
| `/engine` | `[go\|ts\|rust]` | admin | per-chat engine switch (durable, Store) |

## Settings (chat)

| Command | Args | Who | Implements |
|---|---|---|---|
| `/playmode` | `[admin\|everyone]` | admin | Store |
| `/settings` | — | admin | Store dump |
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
| `/auth`, `/unauth` | `<user>` (reply) | admin | Store auth users |
| `/authlist` | — | admin | Store list |
| `/admincache` | — | admin | refresh admin list (600 s cooldown) |
| `/blacklist`, `/unblacklist`, `/whitelist` | `<user\|chat>` | sudo | Store |
| `/blacklisted` | — | sudo | Store list |
| `/addsudo`, `/delsudo`, `/listsudo` | `<user>` | owner | Store sudoers |
| `/ac`, `/activevc` | — | sudo | list active voice chats |
| `/broadcast` | reply + `[-copy\|-nochat\|-user]` | sudo | fan-out, 200 ms spacing, failures inline |
| `/maintenance` | `[on\|off]` | sudo | Store |
| `/restart` | — | owner | exit 42 |
| `/eval` | `<shell>` | owner | real `sh -c`, 60 s timeout |
| `/logger`, `/logs` | — | sudo | log tail |

## Tools / stream

| Command | Args | Who | Implements |
|---|---|---|---|
| `/ping`, `/alive` | — | E | core latency + stats |
| `/stats` | — | E | core + engine stats |
| `/json` | — | sudo | raw update JSON |
| `/bug` | `<text>` | E | report |
| `/privacy` | — | E | privacy note |
| `/speedtest` | — | sudo | download-only speed test |
| `/setrtmp`, `/stream`, `/streamstop`, `/streamstatus` | `<key\|url>` | admin | ffmpeg fork/exec, one child per chat, key scrubbed |
| `/start`, `/help` | — | E | help text (full list in dispatcher) |

## TS / Rust extras (same commands, engine-local)

- TGVCBot/remix: reply-to-audio play (`telegram` provider),
  per-user search buttons, search sessions, admin-gated panel
  (`panel_shuffle/skip/pause/resume/mute/unmute/voldec/volinc/update`).
- ParvatiMusic: `c*`/`f*`/`v*` command variants, VC ops,
  moderation, lists, system, sticker commands — mapped onto the
  table above (variants are aliases in the core route table).

## Not yet (named, with reason)

- `/mute` downstream: flag-only — no `vc.mute` action exists in
  `contracts/ipc.json` yet (contract-first rule: add it there first).
- `/play` from replied-to media: needs the `file_id` hand-off
  (core ↔ Telegram file path not implemented).
- `/speed` / `/bass` audio filter: seek-restart + stored value;
  VC services do not apply the filter yet.
- `/broadcast` errors file: failures inline, no `errors.txt`
  attachment yet.
