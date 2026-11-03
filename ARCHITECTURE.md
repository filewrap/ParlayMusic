# Parlay — Architecture (MoE)

> **Locked architecture (FINAL-ARCHITECTURE.md, implemented by
> Phases 2–3):** the 12-service HTTP topology this file used to
> describe is deleted. What follows describes the current 2-process
> engine. The per-reference provenance (which ref each behaviour
> came from) is unchanged — only the target changed, from a deleted
> service to a package in the unified backend.

## The idea

One bot, many experts. Each language does what it is best at, behind one
narrow contract. The C++ core routes and owns the TDLib sessions (bot +
assistant) — it never downloads via yt-dlp and never talks to Spotify.
The Go backend answers all 65 IPC actions and owns the voice audio
(in-process ntgcalls via cgo) plus all state in one bbolt store. The
backend never talks to Telegram directly — it answers UDS calls from
the core.

```
Telegram ──► core-cpp (C++20, dual TDLib sessions, build-time libtdjson)
               │  UDS, length-prefixed JSON frames, contracts/uds-protocol.md
               │  socket: data/parlay-backend.sock
               └─► backend-go (Go, all 65 actions)
                     ├─ room/voice    queue + playback state, ntgcalls via cgo
                     ├─ resolver      native resolvers (InnerTube, JioSaavn,
                     │                 NetEase, Deezer, Navidrome, Soulseek,
                     │                 Spotify, TikTok, radio)
                     ├─ download      downloads, prefs, mixes, playlists
                     ├─ spotify       watch + classifier + OAuth (AES-256-GCM)
                     ├─ share/vote    anonymous links, crowd voting queue
                     ├─ fx            FX record-only (DSP is Phase 5)
                     ├─ search        query, inline, Lemonic
                     ├─ radio         stations as live sources
                     ├─ gui           Mini App state views (serving: Phase 6)
                     ├─ store         bbolt, data/parlay.db (0600)
                     └─ yt-dlp daemon (Python, warm fallback — started once
                                        at backend boot, scripts/yt_dlp_worker.py)
                     (+ slskd daemon, opt-in, for Soulseek)
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
   one UDS call and formats the result. No yt-dlp, no Spotify client in
   the core binary. (Exception, narrow and deliberate: the core owns the
   TDLib join — `joinVideoChat` on the assistant session — because the
   assistant MTProto client must join the voice chat itself; the backend
   owns the ntgcalls audio and all queue/playback state.)
2. **One queue schema.** `QueueItem` is defined once in `contracts/ipc.json`.
   The backend's room manager owns runtime queue state; the core only
   renders now-playing from UDS replies.
3. **No engine selection.** There is one backend. `/engine` answers that
   selection is retired. `VC_PROFILE` (low-ram/high-perf) is deleted.
4. **No fail-over.** There is no second engine to fail over to. A backend
   error is reported exactly, never masked by retrying elsewhere.

## Go backend — Yukki (Go) + ArcMusic + TgMusicBot

All three Go refs are the *same* lineage (YukkiMusicBot → Go ports) with
different tunings, so they land as **one backend, no profiles**
(`VC_PROFILE` low-ram/high-perf is deleted with the engine split):

- `services/backend-go/internal/room` — per-chat queue/playback state
  (`play/pause/resume/skip/seek/loop/shuffle/queue/playlist/autoplay`),
  persisted in bbolt; restored rooms come back paused.
- `services/backend-go/internal/voice` — the voice session itself
  (ntgcalls via cgo); the join handshake goes over UDS to the core's
  TDLib backend. Prefetch lead is `limits.prefetch_lead_time_sec`
  (30 s) in `parlay.toml`; there is no MongoDB queue cache.
- Shared behaviour (from YukkiMusic-Go): assistant account joins/leaves VC,
  autoleave, channel-play, per-chat settings.

## TS engines — TGVCBot + remix (+ Lemonic)

The TS engine is deleted as a service; its behaviour lives in the
backend:

- `services/backend-go/internal/resolver` — YouTube (InnerTube) +
  JioSaavn providers, radio as a live source.
- `services/backend-go/internal/search` — the Lemonic module
  (`lemonic.search/download/stream`, Spotify link resolution) and the
  inline-query path (`search.inline`), plus lyrics (LRCLib) and the
  live control panel state.

## Rust services — ParvatiMusic + FX + share + queue + Spotify

The Rust services are deleted; their behaviour lives in backend
packages (all state in the one bbolt store):

| Backend package | From | Owns |
|---|---|---|
| `internal/voice` (+ `internal/room`) | ParvatiMusic (`bot/src/modules/play.rs`, `mtproto/`, `ntg/`) | The VC engine: Opus/RTP pipeline + ntgcalls via cgo; join via the core's TDLib backend over the UDS `JoinRequest` handshake |
| `internal/fx` (+ `services/audio-fx-rust` kept as port source) | telegram-music-bot-rust (`main.rs`) | `fx.apply` — record-only until the Phase 5 in-process DSP: stores the preset, returns the input path unchanged (no FFmpeg subprocess) |
| `internal/share` | feel-me (`src/{main,lib,models}.rs`, teloxide + Diesel) | Anonymous share links: create/resolve, tokenised, no identity stored |
| `internal/vote` | daslui (`src/spotify.rs`) | Crowd queue: Spotify link → voting chat → approve/decline → Spotify playback queue (threshold `PARLAY_VOTE_THRESHOLD`, default 3) |
| `internal/spotify` | Rustify (`src/{profanity,lyrics,spotify,queue}.rs`) | Spotify playback watch, lyrics fetch (Genius/LRCLib/Musixmatch), profanity + AI-lyrics detection, auto-skip disliked |

## JS services — GUI/webapp + radio

Both JS services are deleted; their behaviour lives in backend
packages:

- `internal/gui` (wavetune) — read-only Mini App state views
  (`gui.now_playing`, `gui.queue` UDS actions). Mini App serving
  itself is Phase 6 (not built yet); there is no web player process.
- `internal/radio` (openradiobot: `index.js`) — radio stations as live
  sources: `radio.start` hands a stream URL to the room manager.
  Native HTTP/ICY probe (10 s budget) replaces the ffprobe subprocess.

## Python downloaders — YT / Spotify / Deezer / TikTok / Navidrome / Beetube

The Python downloaders service is deleted. Its behaviour lives in the
backend plus one warm daemon:

- `internal/resolver` + `internal/download` own the download surface:
  `download.youtube`, `download.spotify`, `download.spotify_playlist`
  (`mode: zip | files`), `download.deezer`, `download.tiktok`,
  `download.navidrome` (link → Deezer/Last.fm metadata → Soulseek →
  tag → Navidrome library import; Soulseek goes through the separate
  slskd daemon, opt-in), `spotify.collection` (metadata only).
- The persistent yt-dlp daemon (`scripts/yt_dlp_worker.py`, venv
  `scripts/yt-dlp-venv/`) is the warm fallback: native resolvers
  first (YouTube InnerTube, JioSaavn, NetEase, Deezer, Navidrome,
  Soulseek, Spotify, TikTok), daemon only when they cannot resolve
  or download. No yt-dlp subprocess per call on the primary path.
- Provider registry pattern from beetube (`providers/*.js`): each provider
  is one resolver. **Scope decision (Phase 5, B6): music only.**
  Beetube's movie/anime/torrent providers were NOT ported — they depend
  on Beetube's external API backend and torrent clients, and Parlay is a
  music bot. No movie/anime/torrent handlers exist; do not add a stub
  that returns `engine_unavailable` and call it a port.
- 50 MB Bot API cap: files over the cap are queued, not failed — a later
  MTProto pass sends them (same rule as the user's @oldlibs pipeline).

## NetEase — `internal/netease`

Music163bot-Go, isolated on purpose: its cookie/login handling and lyric
handlers stay inside this package and never leak into the core. UDS:
`netease.search/download/lyrics` + the generic `search.query` route.
NetEase is env-gated on `NETEASE_MUSIC_U` and dormant for lossless
without it (anonymous search works); audio recognition
(`handlers_recognize.go` in the ref) was NOT ported — dropped from scope
in Phase 5 (B7): it needs an external fingerprint backend and the ref's
recognize flow needs login; no `netease.recognize` action exists.
`/recognize` is served by `audio.recognize` (AudD, OD-10) instead.

## Shared IPC contract

`contracts/ipc.json` (v0.3.1, **65 actions**) is the frozen parity
reference for request/response shapes. The wire is the persistent
UDS socket (`contracts/uds-protocol.md`): length-prefixed binary
frames carrying JSON envelopes, one duplex connection, multiplexed
by `request_id`. The backend creates and listens on
`data/parlay-backend.sock`; the core connects.

- Every action is one UDS call with a JSON body matching the contract,
- returns `{ "ok": true, "data": … }` or `{ "ok": false, "error": { "code", "message" } }`,
- no `X-Parlay-Token` header (the socket under the 0700 `data/` dir
  is the trust boundary; `PARLAY_TOKEN` is still required at core
  startup but is not sent per call),
- liveness is the `ping` UDS action, not `GET /healthz`.

New actions go in the contract **first**, then in the backend, then in the
core handler — never the other way round.

## Data

- **Single embedded store (OD-5: bbolt)** — one file,
  `<PARLAY_DATA_DIR>/parlay.db` (0600 in a 0700 dir), opened by the
  backend only. 7 buckets: `rooms`, `queue`, `votes`, `shares`,
  `spotify`, `settings`, `radio`. ACID: a kill −9 between
  transactions loses nothing (proven in Phase 3). Restored rooms
  come back **paused** (reader policy, never auto-resume).
  - **Canonical VC room** (bucket `rooms`, key `<chat_id>`):
    `{version, chat_id, current: <track|null>, queue: [{position,
    requested_by, track}], paused, position_sec, volume, loop,
    shuffle, autoplay, speed, bass, history: [<track id>],
    updated_at_ms}`. Track is contract fields only
    (`id/title/source/url/artist/duration_sec/thumbnail/
    stream_url`, optionals omitted when empty).
  - **Failure rule**: there is no fallback store. If bbolt cannot
    open, the backend fails at startup, loudly.
- **Core-internal store (residual).** The core keeps its own
  `Store` (`services/core-cpp/src/store.*`, Mongo wire > Redis
  RESP > JSON file) for core-internal keys (`sudoers`, `chats`,
  `users`, `bl_users`, `bl_chats`, `auth:<chat>`, `chat:<id>` hash,
  `call:<id>`, `cleared:<id>`, `autoplay:<id>`, `maintenance`,
  `maint_reason`, `logger_on`). This predates the locked
  architecture and was not removed by Phase 2/3 — a `MONGO_URI` /
  `REDIS_URL` in `.env` still selects those backends for the core's
  own keys. Removing them is a code decision (see CURRENT-STATE.md
  §5). Nothing in the backend uses them.
- **No shared tables.** The two processes never share a database
  table directly; the backend owns all playback state, the core
  owns its own keys, and they talk through the UDS contract (e.g.
  the Mini App reads now-playing via the backend's `gui.*` UDS
  actions in Phase 6).

## Deploy

Two processes + the yt-dlp daemon, started by hand until the
launcher rewrite lands: backend first (creates the socket, opens
the store), core second (connects). See `README.md` / `INSTALL.md`
for the exact commands. `docker-compose.yml` still describes the
deleted 12-service topology and has no Docker route until it is
rewritten. No secrets in the repo: `.env.example` lists the env
variables; `.env` is gitignored; behaviour config lives in
`parlay.toml` (see `parlay.toml.example`).
