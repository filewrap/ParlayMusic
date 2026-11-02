# Parlay — Port Map

Scaffold-only port map. No code was copied from the reference repos into
`services/` — the skeletons are original stubs. Porting means **re-implementing
the behaviour** behind Parlay's own IPC contract (see `contracts/ipc.json`),
not vendoring upstream code.

> **License warning — read before porting anything.** Most refs are
> GPLv3 / AGPLv3 (YukkiMusic-Go, TinaMusic, remix, wavetune, lemonic,
> Spotify-Loader, Music163bot-Go, ArcMusic, TgMusicBot, sanila). Copying
> their code into Parlay would make Parlay a derivative work under that
> license. MIT/Apache refs (anonx-cpp, TGVCBot, rustify, daslui,
> ParvatiMusic, openradiobot, navidrome, beetube) are more permissive, but
> still keep attribution. Repos with **no LICENSE file** in the clone
> (NoxBeats, telegram-music-bot-rust, feel-me) are all-rights-reserved by
> default — port the *idea* only, never the code. If Parlay ever ships,
> pick one license for the whole tree and re-check each port against it.

Stars are the 2026-11-02 snapshot from the earlier search; rustify was not
in that snapshot. All 21 refs cloned OK (`refs/`, shallow `--depth 1`).

## 1. Reference inventory

| # | Ref dir | Upstream | Lang | Stars ≈ | License | Size / files | Key files / features |
|---|---|---|---|---|---|---|---|
| 1 | `YukkiMusic-Go` | TheTeamVivek/YukkiMusic | Go | 57 | GPLv3 | 1.5M / 133 | `cmd/app/main.go`, `internal/core/{assistant,queue,room}.go`, `internal/database/*` — full VC bot: queue, rooms, MongoDB, channel-play, autoleave |
| 2 | `anonx-cpp` | NALLA933/anonx-cpp | C++20 | 0 | MIT (README badge; no LICENSE file in clone) | 408K / 41 | `src/main.cpp`, `src/telegram/{dispatcher,tdlib_client}.cpp`, `src/audio/{audio_streamer,ffmpeg_pipeline,ntgcalls_client}.cpp`, `src/database/mongo_client.cpp` — **core base candidate**: TDLib client, ntgcalls, FFmpeg pipeline, thread pool, rate limiter |
| 3 | `NoxBeats` | Badmunda05/NoxBeats | C++20 | 1 | none found | 2.1M / 110 | `Bad/cmd/{play,queue_ops,playlist,playback,settings,sudo,broadcast}.cpp`, `Bad/core/{app,calls,autoplay}.cpp`, `Bad/{voice,mtp,tg,platforms}` — **core base candidate**: full command set, group management, autoplay, RTMP |
| 4 | `TGVCBot` | ArnabXD/TGVCBot | TypeScript | 98 | Apache-2.0 | 948K / 68 | `apps/tgvcbot/src/{bot,app,queue,tgcalls,userbot}.ts`, `handlers/{play,queue,radio,youtube,jiosaavn}.ts`, `providers/{youtube,jiosaavn}.ts` — TS VC engine, monorepo (turbo/pnpm) |
| 5 | `remix` | callsmusic/remix | TypeScript | 189 | AGPLv3 | 556K / 59 | `src/bot/{index,queues,session}.ts`, `src/bot/handlers/{stream,controls,panel,playlist,search,lyrics}.ts`, `src/bot/streamer/{audio,youtube,stream}.ts` — TS VC engine, live panel, lyrics |
| 6 | `TinaMusic` | SUDEEPBOTS/TinaMusic | C++20 | 3 | AGPLv3 | 2.8M / 74 | `src/{bot_engine,audio_engine,queue_manager,tg_client}.cpp`, `include/tinaproto/{mtproto,voice_binding,tl_codec}.hpp` — **core base candidate**: pure-C++ MTProto + WebRTC voice, no Go/Python, ~30MB RAM claim |
| 7 | `rustify` | vtvz/rustify | Rust (nightly) | — | MIT | 8.8M / 226 | `src/{profanity,lyrics/{genius,lrclib,musixmatch},spotify,queue,workers}.rs`, `src/entity/*` (SeaORM) — Spotify playback monitor, profanity/AI-lyrics detection, auto-skip disliked tracks |
| 8 | `telegram-music-bot-rust` | cakartamas2010/telegram-music-bot-rust | Rust | 1 | none found | 244K / 4 | `main.rs` (single file) — YT/TikTok download via yt-dlp + FFmpeg audio FX: Slow+Reverb, Nightcore, Bass Boost, 8D |
| 9 | `feel-me` | 0xNima/feel-me | Rust | 4 | none found | 232K / 9 | `src/{main,lib,models,schema}.rs`, teloxide + Diesel — anonymous music sharing via unique link |
| 10 | `telegram-music-bot-daslui` | daslui/telegram-music-bot | Rust | 3 | MIT | 2.1M / 16 | `src/{main,lib,spotify}.rs` — crowd-curated queue: Spotify link → voting chat → approve/decline → Spotify playback queue |
| 11 | `ParvatiMusic` | Iotatg/ParvatiMusic | Rust | 0 | MIT | 5.6M / 289 | `bot/src/{ferogram_bot,assistant}.rs`, `bot/src/modules/{play,queuecmd,playlists,admin,moderation}.rs`, `mtproto/`, `ntg/` — Rust VC engine, native Ferogram MTProto + ntgcalls, live message editing, cancel download |
| 12 | `wavetune` | 4nuxd/wavetune | JS/TS (Next.js) | 12 | GPLv3 | 5.2M / 181 | `app/(player)/`, `app/(root)/page.js`, `components/{music-provider,cards,ui}` — web GUI + Telegram Mini App player |
| 13 | `openradiobot` | Yonle/openradiobot | JS (Node) | 12 | MIT | 164K / 6 | `index.js` (single file) — radio via openradio + ytdl-core |
| 14 | `Telegram-Song-Downloader-Bot` | sanila2007/Telegram-Song-Downloader-Bot | Python | 16 | GPLv3 | 2.8M / 17 | `bot.py`, `text_msg.py` — free YouTube song downloads |
| 15 | `music-bot-navidrome` | eeegoloauq/music-bot | Python | 9 | MIT | 3.6M / 95 | `src/{bot,inline,navidrome}.py`, `src/metadata/{deezer,lastfm,resolver}.py`, `src/soulseek/{client,downloader,matcher,scorer}.py`, `src/library/tagger.py` — Navidrome companion: link → Soulseek → tag → library |
| 16 | `lemonic-telegram-bot` | viperadnan-git/lemonic-telegram-bot | TypeScript | 23 | GPLv3 | 392K / 31 | `bot.ts`, `handlers/{inline_query,search_command,url_message}.ts`, `modules/{lemonic,spotify,songlink}.ts` — Lemonic API search/stream/download, inline mode |
| 17 | `Spotify-Loader` | xgorn/Spotify-Loader | Python | 29 | GPLv3 | 216K / 8 | `bot.py` (single file) — Spotify playlist/artist/album/track → ZIP or one-by-one upload; `spotify-link | youtube-link` metadata mode |
| 18 | `beetube-bot` | kodjunkie/beetube-bot | JavaScript | 152 | MIT | 15M / 47 | `bot.js`, `commands/{search,list,settings}.js`, `providers/{music,movie,anime,torrent}.js`, `handlers/callback-query.js` — multi-downloader: music, movies, anime, EDM, torrents |
| 19 | `Music163bot-Go` | XiaoMengXinX/Music163bot-Go | Go | 1182 | GPLv3 | 676K / 64 | `cmd/music163bot/main.go`, `internal/bot/handlers_{music,search,lyric,recognize,inline}.go` — NetEase Cloud Music download/share/search, lyrics, audio recognition |
| 20 | `ArcMusic` | tusar404/ArcMusic | Go | 11 | GPLv3 | 1.5M / 164 | `cmd/app/main.go`, `internal/core/{room_queue,room_control,assistant}.go`, `internal/database/*` — high-performance Go VC, MongoDB caching, channel-play |
| 21 | `TgMusicBot` | AshokShau/TgMusicBot | Go | 101 | GPLv3 | 1.1M / 125 | `main.go`, `internal/bot/{play,queue,loop,seek,playlist,autoplay,stats}.go`, `ntgcalls/` — low-RAM Go VC, YouTube/Spotify/Apple/M3U8 |

Clone status: **21 / 21 OK** (all via HTTPS, shallow `--depth 1`, 2026-11-02).
See `clone-log.txt` for the per-repo log.

## 2. Feature → source → Parlay target

`core` = `services/core-cpp` (C++ router/bot core).
`svc:<name>` = a service under `services/`. `route` = how the core reaches it
(all via the IPC contract in `contracts/ipc.json`, never by linking code).

| Feature the user wants | Source repo(s) | Source file(s) | Ports into Parlay as | How |
|---|---|---|---|---|
| VC streaming (play/pause/skip/seek/loop/queue) | YukkiMusic-Go, ArcMusic, TgMusicBot | `internal/core/queue.go`, `internal/core/room*.go`, `internal/bot/play.go`, `queue.go`, `loop.go`, `seek.go` | `svc:vc-go` (primary) + `svc:vc-rust` (alt) | Go service owns ntgcalls voice; core sends `play/pause/skip/seek/queue` IPC. Rust ParvatiMusic is the fallback VC engine behind the same contract |
| VC streaming, TS engine | TGVCBot, remix | `apps/tgvcbot/src/tgcalls.ts`, `handlers/play.ts`, `queue.ts`; `src/bot/streamer/stream.ts`, `handlers/stream.ts`, `queues.ts` | `svc:vc-ts` | TS service for groups that want the grammy/TS stack; same IPC, core picks engine per-chat in `services/core-cpp` config |
| Group management (admin, sudo, auth users, blacklist, chat settings, broadcast) | NoxBeats, TgMusicBot, ParvatiMusic | `Bad/cmd/{sudo,settings,broadcast,chat_settings}.cpp`, `Bad/core/app.cpp`; `internal/bot/{admins,auth,filters}.go`; `bot/src/modules/{admin,moderation}.rs` | `core` | Port the *command table + permission model* into the C++ core; admin state lives in the core DB, not in VC services |
| Anonymous music sharing | feel-me | `src/{main,lib,models}.rs` (teloxide + Diesel) | `svc:share-rust` | Rust service: `share.create` → unique link + token; `share.resolve` → track. Core exposes `/share`; no identity attached to the link |
| Downloads — YouTube (free) | sanila2007 Telegram-Song-Downloader-Bot, telegram-music-bot-rust | `bot.py`; `main.rs` (yt-dlp call) | `svc:downloaders` | Python worker: `download.youtube` IPC → yt-dlp → audio file → core sends it (50 MB Bot API cap noted; larger files queued for MTProto later) |
| Downloads — Spotify | Spotify-Loader, beetube-bot | `bot.py` (Spotify-Loader); `providers/music.js` (beetube) | `svc:downloaders` | Python worker: `download.spotify` → resolve Spotify → YouTube audio match → tag → ZIP or one-by-one, as in Spotify-Loader |
| Downloads — NetEase | Music163bot-Go | `internal/bot/handlers_music.go`, `handlers_search.go`, `handlers_lyric.go` | `svc:netease-go` | Go service: `netease.search/download/lyrics` IPC; cookie/login handling stays inside this service only |
| Downloads — Deezer / TikTok / multi-source | telegram-music-bot-rust, beetube-bot, lemonic | `main.rs`; `providers/{music,movie,anime,torrent}.js`; `modules/lemonic.ts` | `svc:downloaders` + `svc:vc-ts` (Lemonic) | `download.deezer/tiktok` in the Python worker; beetube's provider table becomes the downloader's provider registry |
| Streaming (non-VC: send audio in chat, inline) | lemonic-telegram-bot, remix | `handlers/inline_query.ts`, `modules/lemonic.ts`; `src/bot/handlers/stream.ts` | `svc:vc-ts` | Inline query path: `search.inline` IPC → Lemonic/YouTube results → core answers inline query |
| GUI + webapp playing | wavetune | `app/(player)/`, `app/(root)/page.js`, `components/music-provider.jsx` | `svc:gui-js` | Next.js Mini App: now-playing, queue, search, player controls — talks to the core over HTTP (`/api/*` in `contracts/ipc.json`), never directly to voice services |
| Radio | openradiobot | `index.js` | `svc:radio-js` | Node service: `radio.start/stop/stations` IPC; openradio + ytdl-core stream fed into the active VC engine as a live source |
| Navidrome companion | music-bot-navidrome | `src/navidrome.py`, `src/soulseek/*.py`, `src/metadata/{deezer,lastfm,resolver}.py`, `src/library/tagger.py` | `svc:downloaders` (navidrome route) | Python worker route: link → metadata resolve (Deezer/Last.fm) → Soulseek fetch → tag → Navidrome library import. Separate opt-in service, own config |
| Lemonic API | lemonic-telegram-bot | `modules/{lemonic,spotify,songlink}.ts`, `handlers/search_command.ts` | `svc:vc-ts` | TS service module: `lemonic.search/download/stream` IPC; Spotify→Lemonic link resolution |
| Spotify loader (playlists as ZIP / one-by-one) | Spotify-Loader | `bot.py` | `svc:downloaders` | Same worker as Spotify downloads; `download.spotify_playlist` with `mode: zip \| files` |
| Beetube multi-download | beetube-bot | `providers/{music,movie,anime,torrent}.js`, `commands/{search,list}.js` | `svc:downloaders` | Provider registry pattern: each provider is a `download.*` handler; music is the default, others behind a flag |
| Audio FX — Slow+Reverb / Nightcore / Bass Boost / 8D | telegram-music-bot-rust | `main.rs` (FFmpeg filter chains) | `svc:audio-fx-rust` | Rust service: `fx.apply {effect, input}` → FFmpeg filter graph → processed file. Effects are pure functions on audio; no bot logic inside |
| Spotify profanity filter / auto-skip disliked | rustify | `src/profanity.rs`, `src/lyrics/{genius,lrclib,musixmatch}.rs`, `src/spotify/*`, `src/queue/*` | `svc:spotify-rust` | Rust service: watches Spotify playback, checks lyrics, skips profane/AI-flagged/disliked tracks; `spotify.skip` IPC to the VC engine |
| Low-RAM Go engine | TgMusicBot | `main.go`, `internal/bot/*.go`, `ntgcalls/` | `svc:vc-go` (profile: low-ram) | Same Go service, `VC_PROFILE=low-ram` (TgMusicBot tuning: single ntgcalls instance, minimal cache) |
| High-perf Go engine | ArcMusic | `internal/core/room_queue.go`, `internal/database/*` (MongoDB caching) | `svc:vc-go` (profile: high-perf) | Same Go service, `VC_PROFILE=high-perf` (ArcMusic tuning: MongoDB queue cache, prefetch) |
| C++ core / router (base) | anonx-cpp + NoxBeats + TinaMusic | `src/telegram/dispatcher.cpp`, `src/audio/ntgcalls_client.cpp`; `Bad/core/app.cpp`, `Bad/cmd/*.cpp`; `src/bot_engine.cpp`, `include/tinaproto/*.hpp` | `core` | anonx-cpp gives the skeleton (dispatcher + ntgcalls client + thread pool); NoxBeats gives the command/permission surface; TinaMusic gives the native MTProto/WebRTC voice path and the ~30MB-RAM design goal. Core routes every command to a service via IPC and owns no download/stream logic itself |
| Queue / room state model | YukkiMusic-Go, ArcMusic, remix | `internal/core/queue.go`, `room_state.go`; `src/bot/queues.ts` | `core` (schema) + `svc:vc-go` (runtime) | Queue schema defined once in `contracts/ipc.json` (`QueueItem`); runtime state lives in the VC service, core only mirrors now-playing for the GUI |

## 3. What is NOT ported

- Any upstream `config.py` / `.env` / session files, bot tokens, API keys,
  cookies, or MongoDB credentials — none exist in this scaffold, none are
  in `refs/` by design (shallow clones contain only public code).
- Whole-repo vendoring: `refs/` is reference-only and gitignored from any
  future Parlay repo; `services/` contains only original stubs.
- NoxBeats / telegram-music-bot-rust / feel-me code: no LICENSE file, so
  ideas only (see license warning above).
