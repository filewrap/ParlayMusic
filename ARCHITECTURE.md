# Parlay — Architecture (MoE)

## The idea

One bot, many experts. Each language does what it is best at, behind one
narrow contract. The C++ core never downloads, never streams, never talks
to Spotify — it routes. The services never talk to Telegram directly (except
the voice engines, which must hold the MTProto voice session) — they answer
IPC calls from the core.

```
Telegram ──► core-cpp (C++20, Bot API / MTProto)
               │  HTTP+JSON, X-Parlay-Token, contracts/ipc.json
               ├─► vc-go        (Go, ntgcalls)      play/pause/queue in VC
               ├─► vc-ts        (TS, grammy)        TS VC + Lemonic + inline
               ├─► vc-rust      (Rust, ntgcalls)    fallback VC engine
               ├─► audio-fx-rust (Rust, FFmpeg)     Slow+Reverb / Nightcore / …
               ├─► share-rust   (Rust)              anonymous share links
               ├─► queue-rust   (Rust)              crowd voting queue → Spotify
               ├─► spotify-rust (Rust)              Spotify watch + profanity skip
               ├─► gui-js       (Next.js Mini App)  web player, talks to core /api
               ├─► radio-js     (Node)              radio stations as live sources
               ├─► downloaders  (Python)            yt-dlp / spotdl / Soulseek
               └─► netease-go   (Go)                NetEase search/download/lyrics
               └─► MongoDB + Redis (shared state: queues, auth, shares)
```

## C++ core base — anonx-cpp + NoxBeats + TinaMusic

The core is the only piece that owns the Telegram surface, and it is built
from the three C++ refs, each contributing a different layer:

| Layer | From | What Parlay takes (re-implemented, not copied) |
|---|---|---|
| Skeleton: dispatcher, config, logging, thread pool, rate limiter | `anonx-cpp` — `src/telegram/dispatcher.cpp`, `src/core/{config,logger,thread_pool,rate_limiter}.cpp`, `src/audio/{audio_streamer,ffmpeg_pipeline,ntgcalls_client}.cpp` | Command dispatch table, per-chat rate limiting, worker pool, FFmpeg pipeline shape |
| Command + permission surface | `NoxBeats` — `Bad/cmd/*.cpp` (`play`, `queue_ops`, `playlist`, `playback`, `settings`, `sudo`, `broadcast`, `chat_settings`), `Bad/core/app.cpp` | The full command list, sudo/auth-user model, chat settings, autoplay + broadcast semantics |
| Native voice path + low-RAM goal | `TinaMusic` — `src/bot_engine.cpp`, `src/audio_engine.cpp`, `include/tinaproto/{mtproto,voice_binding,tl_codec}.hpp` | In-process MTProto + WebRTC voice (no sidecar process), queue manager shape, ~30 MB RAM design target |

Design rules for the core:
1. **Route, don't do.** A command handler validates permissions, then emits
   one IPC call and formats the result. No yt-dlp, no FFmpeg, no Spotify
   client in the core binary.
2. **One queue schema.** `QueueItem` is defined once in `contracts/ipc.json`.
   VC services own runtime queue state; the core only mirrors now-playing
   for the GUI.
3. **Engine selection is per-chat config.** `vc.engine: go | ts | rust`,
   `vc.profile: low-ram | high-perf` (Go service only). Default: `go/low-ram`.
4. **Fail-open to the next engine.** If the selected VC engine returns
   `error` / times out, the core retries the same `play` call on the next
   engine in the chat's fallback list and tells the user which engine won.

## Go VC engines — Yukki (Go) + ArcMusic + TgMusicBot

All three Go refs are the *same* lineage (YukkiMusicBot → Go ports) with
different tunings, so they land as **one service, two profiles**:

- `services/vc-go`, `VC_PROFILE=low-ram` — TgMusicBot tuning: single
  ntgcalls instance, minimal prefetch/cache, smallest RAM. The default.
- `services/vc-go`, `VC_PROFILE=high-perf` — ArcMusic tuning: MongoDB
  queue cache, prefetch, channel-play. For big groups.
- Shared behaviour (from YukkiMusic-Go): `play/pause/resume/skip/seek/
  loop/shuffle/queue/playlist/autoplay`, assistant account joins/leaves VC,
  autoleave, channel-play, per-chat settings.

## TS engines — TGVCBot + remix (+ Lemonic)

- `services/vc-ts` runs the TS VC path (grammy, `tgcalls.ts` /
  `streamer/stream.ts` shapes): YouTube + JioSaavn providers, radio
  handler, live control panel, lyrics handler.
- The same service hosts the Lemonic module (`lemonic.search/download/
  stream`, Spotify link resolution) and the inline-query path.

## Rust services — ParvatiMusic + FX + share + queue + Spotify

| Service | From | Owns |
|---|---|---|
| `vc-rust` | ParvatiMusic (`bot/src/modules/play.rs`, `mtproto/`, `ntg/`) | Fallback VC engine: MTProto + ntgcalls, live message editing, cancel-download |
| `audio-fx-rust` | telegram-music-bot-rust (`main.rs`) | Pure audio transforms: `fx.apply` with FFmpeg filter chains |
| `share-rust` | feel-me (`src/{main,lib,models}.rs`, teloxide + Diesel) | Anonymous share links: create/resolve, tokenised, no identity stored |
| `queue-rust` | daslui (`src/spotify.rs`) | Crowd queue: Spotify link → voting chat → approve/decline → Spotify playback queue |
| `spotify-rust` | Rustify (`src/{profanity,lyrics,spotify,queue}.rs`) | Spotify playback watch, lyrics fetch (Genius/LRCLib/Musixmatch), profanity + AI-lyrics detection, auto-skip disliked |

Rust services are deliberately stateless apart from their own tables
(shares, votes, Spotify auth) — queue/room state stays in the VC services.

## JS services — GUI/webapp + radio

- `services/gui-js` (wavetune: Next.js Mini App) — the only service users
  see in a browser. Talks **only** to the core (`/api/*`), never to voice
  or downloader services. Now-playing, queue, search, player controls.
- `services/radio-js` (openradiobot: `index.js`) — radio stations as live
  sources: `radio.start` hands a stream URL to the active VC engine.

## Python downloaders — YT / Spotify / Deezer / TikTok / Navidrome / Beetube

- `services/downloaders` is a worker pool behind one IPC surface:
  `download.youtube`, `download.spotify`, `download.spotify_playlist`
  (`mode: zip | files`), `download.deezer`, `download.tiktok`,
  `download.navidrome` (link → Deezer/Last.fm metadata → Soulseek →
  tag → Navidrome library import).
- Provider registry pattern from beetube (`providers/*.js`): each provider
  is one handler; music is the default, movies/anime/torrents behind a flag.
- 50 MB Bot API cap: files over the cap are queued, not failed — a later
  MTProto pass sends them (same rule as the user's @oldlibs pipeline).

## NetEase — `services/netease-go`

Music163bot-Go, isolated on purpose: its cookie/login handling and lyric +
audio-recognition handlers stay inside this service and never leak into
the core. IPC: `netease.search/download/lyrics/recognize`.

## Shared IPC contract

`contracts/ipc.json` is the source of truth. Every service:

- listens on one HTTP port (see `docker-compose.yml`),
- accepts `POST /v1/<action>` with a JSON body matching the contract,
- returns `{ "ok": true, "data": … }` or `{ "ok": false, "error": { "code", "message" } }`,
- requires header `X-Parlay-Token: $PARLAY_TOKEN` (same token everywhere,
  from `.env`, never committed),
- answers `GET /healthz` with `{ "ok": true, "service": "<name>" }`.

New actions go in the contract **first**, then in the service, then in the
core handler — never the other way round.

## Data

- **MongoDB** — chats, auth users, settings, queues (cached), shares, votes.
- **Redis** — now-playing mirror for the GUI, rate-limit counters, download
  job locks.
- Services never share a database table directly; they share it *through*
  the contract (e.g. the GUI reads now-playing from Redis via the core).

## Deploy

`docker-compose.yml` wires everything on one network (`parlay`), one
volume for downloads/cache, and per-service `profiles` so a minimal deploy
can run `core-cpp + vc-go + downloaders` only. See the compose file.
No secrets in the repo: `.env.example` lists every variable; `.env` is
gitignored.
