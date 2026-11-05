# Parlay — Current State, Pixel-Perfect

Date: 2026-11-03. Written for the user's colleagues to design a new architecture
model. Every value below was read from the code on 2026-11-03 — no values are
assumed, rounded, or copied from memory. Where a value is a default, the file
that sets it is named.

Repo: https://github.com/filewrap/ParlayMusic.git — branch `main`.
HEAD at writing: `f1a7211` (Phase 3: unified Go backend, embedded store,
native resolvers). IPC contract version: **0.3.1** (`contracts/ipc.json`),
**65 actions**. Architecture: the locked 2-process engine
(`FINAL-ARCHITECTURE.md`) as implemented by Phase 2 (UDS transport,
dual-TDLib core, `parlay.toml`) and Phase 3 (unified backend-go,
bbolt store, native resolvers). The old 12-service HTTP topology is
deleted; `services/` holds only `core-cpp`, `backend-go`, and
`audio-fx-rust` (Phase 5 DSP port source).

## 1. What Parlay is

A polyglot mixture-of-experts Telegram music bot. Two processes plus a
warm daemon, one core. The bot account talks to Telegram over
MTProto/TDLib (bot token, build-time linked libtdjson) — no Bot API
long-poll anymore. The assistant (userbot) account also talks to
Telegram over MTProto/TDLib and is the only thing that joins voice
chats. Nothing talks to Telegram except the core. The backend never
talks to Telegram, and no process talks to another except through the
single persistent UDS socket or the shared bbolt store.

## 2. The two processes (+ daemon)

| # | Piece | Language | Binary | Socket / store | What it does | What it owns |
|---|---|---|---|---|---|---|
| 1 | core-cpp | C++20 | parlay-core | UDS client → `data/parlay-backend.sock` | TDLib bot + assistant sessions, command dispatcher, permissions, routing, voice backend (TDLib join + ntgcalls via the backend's UDS join handshake), Opus pipeline | All Telegram I/O, all routing, all audio output |
| 2 | backend-go | Go | parlay-backend | UDS server on `data/parlay-backend.sock`; bbolt `data/parlay.db` | All 65 IPC actions: rooms/queue, native resolvers, downloads, Spotify watch/classifier, share links, voting queue, FX (record-only), search, radio, Mini App state views; in-process ntgcalls via cgo | All playback/download/search state |
| — | yt-dlp daemon | Python | `scripts/yt_dlp_worker.py` (`scripts/yt-dlp-venv/bin/python`) | stdio pipes to the backend | Warm yt-dlp fallback: resolve + download when the native resolvers cannot; started once by the backend at boot | Nothing persistent |
| — | audio-fx-rust | Rust | — | — | Kept as the Phase 5 DSP port source only — not a running process | — |
| — | slskd | — | slskd | 5030 (HTTP) / 5031 (HTTPS) | Soulseek daemon for Soulseek search/download (opt-in) | Soulseek session |

No Mongo, no Redis, no per-service JSON stores, no HTTP healthchecks:
the backend answers a `ping` action over the same UDS socket, and the
only TCP listener left is the core's legacy `:8080` HTTP server, which
still starts at boot but nothing uses (FINAL-ARCHITECTURE.md §6-R1
schedules its deletion).

Default startup order: backend first (it creates the socket and opens
the store), core second (it connects). `install.sh` / `start.sh` /
`stop.sh` / `status.sh` still describe the deleted topology and are
being rewritten; `docker-compose.yml` likewise.

## 3. Communication — the IPC contract

File: `contracts/ipc.json`, version 0.3.1, **65 actions** (56 + 9 OD-3).
Wire spec: `contracts/uds-protocol.md`.

- **Transport:** one persistent Unix Domain Socket, `data/parlay-backend.sock`
  (`<PARLAY_DATA_DIR>/parlay-backend.sock`, overridable with
  `PARLAY_UDS_PATH`). Length-prefixed binary frames carrying JSON
  envelopes; one duplex connection, multiplexed by `request_id`. No
  HTTP, no per-call subprocess, no `/healthz` polling, no
  `X-Parlay-Token` header. The backend creates and listens on the
  socket; the core connects (reconnects with backoff if it drops).
  UDS round-trip median 142 µs (Phase 2). `ipc.json` is the live
  contract for request/response shapes (retargeted to the unified
  backend in Phase 4); the HTTP wire protocol it described is deleted.
- **Auth:** `PARLAY_TOKEN` is still required for the core to start
  (`missingRequired()` also requires `BOT_TOKEN`), but it is no longer
  sent per call — there is no HTTP header anymore. The socket lives
  under the 0700 `data/` dir, so local access is the trust boundary.
- **Envelope:** success `{ok:true, data:{...}}`; error
  `{ok:false, error:{code, message}}`. Error codes: `bad_request`,
  `unauthorized`, `not_found`, `engine_unavailable`, `download_failed`,
  `rate_limited`, `internal`.
- **Timeouts** (`parlay.toml` `[timeouts]`): resolve 30 s, join 30 s,
  generic IPC command 10 s, download 300 s, health-probe 5 s.
- **The old transport flaw, now fixed:** the C++ HTTP client used to
  be curl-CLI based — every IPC call spawned a `curl` subprocess.
  Phase 2 replaced it with the persistent UDS socket above; there is
  no curl subprocess anywhere in the IPC path anymore.
- **Action groups:** `vc.*` (play, play_file, pause, resume, skip, stop, seek, queue,
  loop 0–10, shuffle, autoplay, autoplay-skip, volume 0–200, now-playing,
  progress, panel, lyrics, join, mute, radio); `download.*` (youtube,
  deezer, tiktok, navidrome, spotify, spotify-playlist, soulseek, progress,
  prefs, cancel); `netease.*` (search, download, lyrics); `spotify.*` (resolve, collection,
  status, auth, watch, watches, dislike, skip-check, now-playing, details,
  analyze); `share.*`, `queue.vote`, `fx.apply`, `radio.*` (list, start,
  stop, status), `search.*` (query, inline), `lemonic.*` (search, download),
  `gui.*` (now-playing, queue — read-only state views for the Phase 6 Mini App),
  `chat.settings` (GET/POST, 15 keys), plus the 9 OD-3 actions:
  `audio.recognize` (AudD, env-gated), `cache.clear`, `library.scan`,
  `library.sharescan`, `library.delete`, `mix.generate`, `vc.play_file`,
  `download.prefs`, `download.cancel`.
- **Common types:** `Track` (required: `id`, `title`, `source`; source ∈
  youtube, spotify, netease, deezer, jiosaavn, soundcloud, radio, file,
  lemonic); `NowPlaying` (`track`, `queue`, `position_sec`, `paused`,
  `loop` 0–10, `shuffle`, `autoplay`, `volume`, `stream_url`, `engine`
  (always `"backend-go"` now — the go/ts/rust engine choice is retired),
  plus Phase 9 canonical UI fields `is_paused`, `is_muted`,
  `loop_count`, `speed`, `autoplay_enabled`); `QueueItem` (`track`,
  `requested_by`); `ChatId` (integer, negative for groups); `UserId`.
- **Files over 50 MB:** `download.*` returns `oversize:true`; the core
  queues them for a later MTProto pass instead of failing. (Bot API cap.)

## 4. How the core processes a command (exact path)

### 4.1 Receiving

`main.cpp`: both TDLib sessions authorize first — the bot session
(`BOT_TOKEN`, `authorizeBot`) then the assistant session (phone/code/2FA
or a pre-authorized `data/tdlib` dir; a failed assistant auth is a
loud warning, not fatal — voice joins are unavailable until it logs
in). Then `getMe` on the bot session (fails → exit 1, "could not reach
Telegram"). Then a loop: `sessions.pollUpdate(upd, from, 1.0)` — 1 s
TDLib poll, no `getUpdates` long-poll anymore — and bot updates go to
`dispatchTdUpdate` (message, callback, chat-member, inline query).
Queued backend events / join requests run on the main thread via
`dispatcher.processPendingWork()` (the only thread that may call
TDLib).

### 4.2 Gates (dispatcher.cpp, handleMessage)

In order, first match wins; failures are silent (no reply):

1. Service messages first (new member, video-chat start/end kills session).
2. Parse `/cmd@bot args`; aliases (e.g. `p`→`play`, `q`→`queue`,
   `bassboost`→`bass`) and channel-commands (`cplay`→`play`,
   `cpause`→`pause`, etc.) resolved.
3. Chat allow-list / block-list (`PARLAY_ALLOW_CHATS`,
   `PARLAY_BLOCK_CHATS`).
4. **Rate limit: token bucket** (Phase 2) — per-chat capacity 5,
   refill 1.0/s; per-user capacity 3, refill 1.0/s (`parlay.toml`
   `[rate_limit]`). A denial now carries an honest reason +
   retry-after instead of dropping silently.
5. Scope (Any / Private / Group) and permission (Owner > Sudo > Everyone;
   owner = `OWNER_ID`, sudos = `SUDO_IDS` comma list). Denials silent.
6. Blacklist, maintenance mode.

### 4.3 /play — the full call chain

`onPlay` (dispatcher.cpp): group-only. Replied media → `file_id` →
`vc.play_file` (Phase 3 fix). Otherwise:

1. `vcCall(ctx, "play", body)` — one UDS call to the backend, no
   engine router, no fallback: there is a single backend. The body
   carries `{chat_id, query, requested_by, speed, bass}` (speed/bass
   are record-only until the Phase 5 DSP).
2. The call itself: `IpcClient` over the persistent UDS socket —
   length-prefixed JSON frame, multiplexed by `request_id`, 30 s
   resolve budget (`parlay.toml` `[timeouts]`). No curl subprocess,
   no HTTP, no `X-Parlay-Token` header, no `/healthz` probe.
3. Backend `vc.play` handler: require `chat_id` and `query` (else
   `bad_request`) → native resolver fan-out (YouTube InnerTube first
   for YouTube, JioSaavn, NetEase, Deezer, Navidrome, Soulseek,
   Spotify, TikTok, radio; warm yt-dlp daemon as fallback) → track
   + `stream_url` (resolve failure → `engine_unavailable`) →
   `room.Play(track, requestedBy)` → persist to bbolt → `{ok:true,
   data:{now_playing:{...}}}`.
4. Back in the core: `finishPlay` renders from the backend's
   `now_playing`. **The backend's play is the play: it owns the call
   and the audio** (in-process ntgcalls via cgo), so a successful
   `vc.play` means the track is really playing — no core-side attach
   step exists anymore. On failure the exact backend error is
   reported, never a fake "Now playing". A track longer than
   `max_duration_minutes` (60) is stopped with the too-long reply.
5. Track transitions are backend-driven events (`onBackendEvent`);
   the core's old 5-second loop/autoplay watcher was DELETED in
   Phase 2 — the core no longer polls backend state on a timer.
   Pause/resume/skip/seek/stop/mute/speed/bass go to the backend as
   UDS actions.
6. Reads use the same UDS path (`vc.now_playing`, `vc.queue`, …),
   10 s generic IPC budget, no fallback order.

Measured latency (Phase 1 benchmarks): `/play` YouTube native
InnerTube 0.61–0.81 s (vs yt-dlp 5.7–6.7 s), JioSaavn 0.32 s at
320 kbps. The IPC hop is a 142 µs median UDS round-trip.

### 4.4 Voice pipeline (backend-owned audio, core-owned join)

The voice split changed in Phase 2/3: the **backend owns the audio**
(in-process ntgcalls via cgo, `services/backend-go/internal/voice`),
the **core owns the join** (TDLib `joinVideoChat` on the assistant
session). The handshake between them is the UDS `JoinRequest`
(`contracts/uds-protocol.md` §6): the backend's voice manager sends
`{chat_id, local_sdp}`, the core runs `getChat` / `createVideoChat` /
`joinVideoChat(local SDP)` on the assistant session and returns the
remote SDP, the backend connects ntgcalls with it. `/join` uses the
same path. `/engine` is retired — it answers that there is one
unified backend and no engine to switch.

Libraries: TDLib **1.8.69** (`libtdjson.so.1.8.69`, 50,166,944 B),
linked into the core at **build time** (no dlopen anywhere);
ntgcalls **3.0.5** (`libntgcalls.so`, ~98 MB), linked into the backend
via cgo. Audio: ffmpeg decodes the resolved `stream_url` to `s16le`,
2 channels, 48,000 Hz, into ntgcalls; Opus voice profile
`standard_192k` by default (`parlay.toml` `[quality]`).
`scripts/tdlib_login.py` (598 lines) does a fresh TDLib login
(phone → code → 2FA, or QR) into `data/tdlib` (mode 0700) for the
assistant; the bot session lives in `data/tdlib-bot` (token auth, no
phone). A Pyrogram/Telethon string session **cannot** be imported
into TDLib — no import API exists; fresh login is the only route.

### 4.5 The Mini App path (Phase 6, not built yet)

gui-js is deleted. The backend already serves the read-only state
views the Mini App will need (`gui.now_playing`, `gui.queue` UDS
actions — pure reads from the live room manager, never a persisted
frozen position), but no Mini App is served yet — that is Phase 6.
The core's legacy `:8080` `/api/*` surface still exists in code but
nothing uses it and it is slated for deletion (§6-R1).

## 5. Shared state

Two stores, split by owner — this is the one place the locked
architecture is not fully closed yet (see note below):

- **Backend state (playback, downloads, search): one bbolt file,
  `data/parlay.db`** (0600 in a 0700 `data/` dir), opened by
  `services/backend-go/internal/store`. 7 buckets: `rooms`, `queue`,
  `votes`, `shares`, `spotify`, `settings`, `radio`. ACID — a
  kill −9 between transactions loses nothing (proven in Phase 3).
  Restored rooms come back paused. No Mongo, no Redis, no JSON
  files, no per-chat engine choice to store (`engine:<chat_id>` is
  retired with the engine router).
- **Core state (sudoers, chats, users, auth, chat settings, block
  lists): the core's own `Store`** (`services/core-cpp/src/store.*`),
  backend chosen at startup as **MongoDB wire > Redis RESP > JSON
  file** — the pre-lock store code is still wired in `main.cpp`
  (`parlay::Store store(cfg)`; if no backend is usable the core
  falls back to HTTP-only mode). `MONGO_URI` / `REDIS_URL` still
  select those backends when set; unset means the JSON file under
  `PARLAY_DATA_DIR` (default `data`).

> **Residual (flagged, not fixed here):** FINAL-ARCHITECTURE.md
> forbids Mongo/Redis outright, but the core `Store` still accepts
> them. Nothing in the backend uses them — all playback state is
> bbolt — but a `MONGO_URI` in `.env` still changes where the core
> keeps its own keys. Removing the Mongo/Redis backends from the
> core store is a code change (Worker C / Phase 4 decision).

## 6. Config values (exact defaults)

Two sources: `parlay.toml` (behaviour — rate limits, timeouts,
quality, limits; see `parlay.toml.example`) and `.env` (secrets +
identity). Precedence: built-in defaults < `parlay.toml` < env
overrides (`PARLAY_*`). A missing `parlay.toml` is NOT an error (the
core runs on defaults and says so); a malformed key is a startup
error naming the exact key. Required to start: `BOT_TOKEN`,
`PARLAY_TOKEN`.

| Variable | Default | Notes |
|---|---|---|
| `PORT` | 8080 | core legacy HTTP port — server still starts, nothing uses it (§6-R1 deletion pending) |
| `VC_GO_URL` / `VC_TS_URL` / `VC_RUST_URL` / `FX_URL` / `SHARE_URL` / `QUEUE_URL` / `SPOTIFY_URL` / `RADIO_URL` / `DL_URL` / `NETEASE_URL` | `http://vc-go:8001` … `:8010` | **Dead fields** (config.hpp: all 10 HTTP services deleted by the locked architecture; the UDS client ignores them — signature compat only). Not in `parlay.toml`. Do not set them. |
| `MONGO_URI` / `REDIS_URL` | unset (JSON file) | Still select the core `Store` backend when set (§5 residual) — not used by the backend |
| `PARLAY_DATA_DIR` / `backend.data_dir` | `data` | bbolt `parlay.db`, UDS `parlay-backend.sock`, TDLib dirs all live under it |
| `PARLAY_UDS_PATH` / `backend.uds_path` | `<data_dir>/parlay-backend.sock` | UDS socket override |
| `TD_SESSION_DIR` | `<PARLAY_DATA_DIR>/tdlib` | Assistant TDLib session dir |
| `data/tdlib-bot` | (fixed path) | Bot TDLib session dir (token auth) |
| `TD_API_ID` / `TD_API_HASH` | fall back to `API_ID` / `API_HASH` | |
| `SLSKD_URL` | `http://127.0.0.1:5030` | Soulseek daemon (backend config) |
| `SPOTIFY_REDIRECT_URI` | `http://127.0.0.1:8080/callback` | must match the registered URI exactly — Spotify rejects `localhost` |
| `DURATION_LIMIT` (legacy) / `limits.max_duration_minutes` | 60 (minutes) | max track length |
| `QUEUE_LIMIT` (legacy) / `limits.max_queue_size` | 20 | max queue items |
| `limits.prefetch_lead_time_sec` / `limits.url_cache_ttl_sec` | 30 / 120 | prefetch lead; resolved-URL cache TTL (CDN URLs live ~2 min) |
| `quality.voice_profile` | `standard_192k` | `standard_192k` (192 kbps) \| `high_256k` \| `ultra_320k`; `ultra_320k` becomes default only after the Phase 6 live bitrate test |
| `quality.source_ladder` | navidrome, soulseek, deezer, netease, jiosaavn, youtube, spotify, tiktok, radio | resolution order, Tier 1 lossless first |
| `OWNER_ID` / `SUDO_IDS` / `LOGGER_ID` | unset | `SUDO_IDS` comma-separated ints |
| `PARLAY_ALLOW_CHATS` / `PARLAY_BLOCK_CHATS` | unset | comma-separated chat ids |
| `ASSISTANT_IDS` | unset | comma-separated user ids |
| `PARLAY_VOTE_THRESHOLD` | 3 | votes to queue a track |
| `TD_PHONE` / `TD_CODE` / `TD_PASSWORD` / `TD_EMAIL` / `TD_EMAIL_CODE` / `TD_FIRST_NAME` / `TD_LAST_NAME` / `TD_QR` | unset / false | TDLib first-auth inputs |
| `START_IMG` / `PING_IMG` / `PARLAY_UPDATES_URL` / `PARLAY_SUPPORT_URL` / `PARLAY_SOURCE_URL` / `SUPPORT_CHAT` | unset | UI links/images; empty = row omitted / text-only card |
| `VOICE_RTP_SINK` | unset | `host:port` for the Opus pipeline proof |
| TDLib poll | 1 s | `sessions.pollUpdate(upd, from, 1.0)` |
| Rate limit | chat 5 burst / 1.0/s refill; user 3 / 1.0/s | token bucket; denial carries reason + retry-after |
| IPC timeouts | resolve 30 s, join 30 s, ipc_cmd 10 s, download 300 s, healthz 5 s | `parlay.toml` `[timeouts]` |
| Loop count | 0–10 | 0 = off |
| Volume | 0–200 | stored per room; applied by the backend voice path |

## 7. Bot API vs MTProto — the actual split

Both accounts now use MTProto via TDLib. The **bot account**
authenticates with its bot token (`checkAuthenticationBotToken` —
session in `data/tdlib-bot`); the **assistant account** authenticates
with phone/code/2FA (session in `data/tdlib`) and is the only thing
that joins voice chats. There is no Bot API long-poll anymore
(`getUpdates` is gone from the core), and the Bot API base URL config
(`TELEGRAM_API_BASE`) is deleted.

Correction to an earlier claim: bot tokens **do** work over MTProto
(`checkAuthenticationBotToken` in TDLib; `auth.importBotAuthorization` in
Telethon/Pyrogram). anonx-cpp — one of the three C++ refs — runs its bot
over TDLib with a bot token. Parlay used the Bot API for the bot by choice
in the old topology (simplicity; plain HTTPS works through the sandbox
proxy where MTProto's direct DC connections are blocked), not by necessity.
Phase 2 moved the bot onto TDLib, so that cost is gone: no 50 MB Bot API
file cap on the bot face, no Bot API rate limits, MTProto-only methods
available. The three C++ refs split 1–2: anonx-cpp = MTProto for the bot;
NoxBeats and TinaMusic = Bot API for the bot.

## 8. Current state — what is proven, what is not

- **Tests (Phase 3 verification, 2026-11-03): Go 25/25 packages,
  245 passed / 0 failed / 12 skipped**; ctest 9/10 (**1,615 PASS** —
  the only failures are the 2 pre-existing `store_test` env
  failures, Mongo/Redis not running); routing table **944 pass**;
  **64/65** IPC actions answered against the real core UDS client
  (`vc.join` blocks past 60 s on fake SDP — it needs a real group
  call). Kill −9 store proof passed; daemon boot-warm bug fixed
  with a regression test.
- **Phase 3 (unified backend) pushed 2026-11-03 ~18:06 IST as
  `f1a7211`** (242 files, +33,047/−45,183; local HEAD == origin/main
  verified). The 10 deleted service dirs are gone; `services/`
  holds only `core-cpp`, `backend-go`, `audio-fx-rust`.
- **Phase 9 (Telegram UI/UX port, `d5065b9`) and the locked
  architecture (`FINAL-ARCHITECTURE.md`, 908 lines) are the base
  Phase 3 built on.** Phase 2 (`9b56b93`) delivered the UDS
  transport (median 142 µs), dual-session TDLib, `parlay.toml`,
  token-bucket limiter, and the routing table.
- **`/recognize` provider decided: AudD** (2026-11-03 ~16:57 IST) —
  token not supplied yet; `audio.recognize` is env-gated and
  answers honestly until configured. **Deezer dropped for now**
  (no ARL; sandbox geo-blocked signup) and **NetEase skipped**
  (no `MUSIC_U` obtainable without a +86 number) — both resolvers
  stay in code, env-gated/dormant. Spotify app "Parlay" exists
  (redirect `http://127.0.0.1:8080/callback`); the user copies the
  client ID/secret + AudD token into the VPS `.env` themselves.
- **Live proof still pending:** no audible audio in a real call has been
  proven from the sandbox (no phone/QR/session dir for first-auth, sandbox
  UDP sendto EPERM, no direct TLS to Telegram DCs). The user's live test on
  their VPS is the proof (Phase 6). Bot `@punlobot` verified via `getMe`.
- **Launchers + compose still describe the deleted topology:**
  `install.sh` / `start.sh` / `stop.sh` / `status.sh` and
  `docker-compose.yml` need the release-pass rewrite (Worker A).
  `docker compose up` has never run anywhere (no Docker in the
  sandbox; setns denied).

## 9. Known weaknesses (for the redesign discussion)

1. **The redesign happened — these are the leftovers.** Items 1–3
   and 5 and 7 below were the old topology's weaknesses; the locked
   architecture closed them. What remains:
2. **Core `Store` still accepts Mongo/Redis** (§5). The backend is
   bbolt-only, but the core's own keys can still land in Mongo/Redis
   when `MONGO_URI` / `REDIS_URL` are set. Also still in the core
   tree: the legacy `:8080` HTTP server (`http_server.cpp` /
   `core_api.cpp`, §6-R1 deletion pending) and the dead `VC_*_URL`
   config fields.
3. **Launchers + compose lag the code.** `install.sh` / `start.sh` /
   `stop.sh` / `status.sh` and `docker-compose.yml` still describe
   the deleted 12-service topology; there is no tested start/stop
   path except running the two processes by hand, and no new
   release tarball.
4. **No live voice proof yet.** See §8.
5. **Honestly unmeasured:** no live voice call, no live provider
   downloads, no live AudD/Spotify-OAuth calls. `vc.join` Connect
   blocks past 60 s on fake SDP and needs a context/timeout (leaks
   a goroutine when stuck). Download runs/prefs/library index are
   in-memory singletons — a backend restart loses them.
6. **FX is record-only until Phase 5.** `fx.apply` stores the preset
   and returns the input path unchanged; no DSP runs yet.
   Speed/bass are likewise stored, not applied, until the Phase 5
   DSP lands.

## 10. The 21 references (for the redesign)

Base C++: NALLA933/anonx-cpp, Badmunda05/NoxBeats, SUDEEPBOTS/TinaMusic.
Go: TheTeamVivek/YukkiMusic, tusar404/ArcMusic, AshokShau/TgMusicBot,
XiaoMengXinX/Music163bot-Go. TS: ArnabXD/TGVCBot, callsmusic/remix. Rust:
Iotatg/ParvatiMusic, cakartamas2010/telegram-music-bot-rust, 0xNima/feel-me,
daslui/telegram-music-bot, vtvz/rustify. JS: 4nuxd/wavetune, Yonle/openradiobot,
kodjunkie/beetube-bot. Downloaders: sanila2007/Telegram-Song-Downloader-Bot,
xgorn/Spotify-Loader, viperadnan-git/lemonic-telegram-bot,
eeegoloauq/music-bot (Navidrome). All under `~/workspace/parlay/refs/`
(19 LICENSE files deleted under `refs/` per user order — do not re-raise
licensing).
