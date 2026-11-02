# Audit worker C — contract + cross-cutting + TDLib

Date: 2026-11-03. Scope: `~/workspace/parlay/`, read-only audit (no code changed, no push, no live Telegram join).
Contract version: `contracts/ipc.json` v0.1.0-scaffold, **41 actions**.

## 1. Contract coverage (41 actions)

### 1a. Contract actions with NO implementing service

| Action | Contract service | Evidence |
|---|---|---|
| `gui.now_playing` — `GET /api/now-playing?chat_id=` | core-cpp → gui-js | **No real implementation.** core-cpp has no HTTP server at all and no `/api/` route (`grep /api/` in `services/core-cpp/src/` = zero hits; `config.hpp:40` calls port 8080 "GUI /api surface, future"). gui-js `app/main.js:459-475` proxies to `CORE_URL /api/now-playing`; when core is unreachable it serves **hardcoded demo state** (`note: "core unreachable or CORE_URL unset — serving local demo state"`). A new user following the contract gets demo data, not live state. |
| `gui.queue` — `GET /api/queue?chat_id=` | core-cpp → gui-js | Same as above (`app/main.js:477-492` → demo queue `source:"local-demo"`). |
| `vc.loop` (`POST /v1/loop`) | vc-go \| vc-ts \| vc-rust | **vc-ts missing.** vc-go `internal/server/server.go:43` ✓, vc-rust `src/main.rs:1339` ✓, vc-ts `src/main.js` has **no** `/v1/loop` route — only a panel toggle (`main.js:309-312`, `panel_loop` button). Unknown route → `404 not_found` (`main.js:652`). |
| `vc.shuffle` (`POST /v1/shuffle`) | vc-go \| vc-ts \| vc-rust | **vc-ts missing.** vc-go `:44` ✓, vc-rust `:1340` ✓, vc-ts only panel shuffle (`main.js:263-264`). Same 404. |
| `vc.autoplay` (`POST /v1/autoplay`) | vc-go \| vc-ts \| vc-rust | **vc-ts missing.** vc-go `:45` ✓, vc-rust `:1341` ✓, vc-ts has no `/v1/autoplay` route at all. |
| `vc.autoplay_skip` (`POST /v1/autoplay-skip`) | vc-go \| vc-ts \| vc-rust | **vc-ts missing.** vc-go `:46` ✓, vc-rust `:1342` ✓, vc-ts none. |

**Mitigation that partially saves the vc-ts gap:** core-cpp `engine_router.cpp` `callVc` tries engines in `fallbackOrder` (Go→Ts→Rust, etc.) and retries on `unavailable()` — but `IpcResult::unavailable()` (`ipc.hpp:24-27`) is true only for transport error, HTTP ≥500, or `engine_unavailable`. A vc-ts **404 `not_found` is not unavailable**, so `callVc` returns the 404 immediately without trying the next engine. Consequence: any chat whose engine is set to ts gets `404 not_found` on `/loop`, `/shuffle`, `/autoplay`, `/autoplay-skip` from the core. Default engine is Go (`engine_router.cpp:28`), so the gap only bites ts-engine chats — but it is a real contract violation for 4 of 41 actions on one of three engines.

**Not in the contract at all (and not implemented as IPC either): `vc.join`, `vc.mute`.**
- No `vc.join`/`vc.mute` key in `ipc.json`, no `/v1/join` or `/v1/mute` route in any service. `PHASE4-TDLIB.md:113` already flags the missing `vc.join` as an "IPC gap (coordinator decision)".
- Mute exists only as a **flag in core-cpp**: `dispatcher.cpp:1666-1678` (`onMute`) with the honest comment "The vc services expose no mute action: the flag is stored per chat … and reported by /pos". `voice_call.hpp:65-66` declares a `mute()/unmute()` interface on the voice backend, but `LoggingVoiceCall` returns `false` (`:91-92`) and no IPC action reaches it.
- Join exists only inside the voice backends (TDLib `joinVideoChat`, bridge `join-call`); there is no HTTP join endpoint in core-cpp, as `PHASE4-TDLIB.md:113` notes.

### 1b. Contract actions that ARE implemented (verified by route)

- **download.\*** (6/6) — `services/downloaders/src/server.py:35-41`: youtube, spotify, spotify-playlist, deezer, tiktok, navidrome + `/v1/search`. ✓
- **netease.\*** (3/3) — `services/netease-go/internal/server/server.go:43-45`: search, download, lyrics. ✓
- **spotify.\*** (7/7) — `services/spotify-rust/src/main.rs:930-954`: watches (GET), status (GET), watch, dislike, resolve, collection, skip-check. ✓
- **vc.radio** (`POST /v1/radio`) — `services/radio-js/src/main.js:398-414` (start\|stop, returns `now_playing`). ✓
- **radio.list / radio.start** — `radio-js/src/main.js:358` (`/v1/radio/stations`), `:385` (`/v1/radio/start`, returns `stream_url` + `now_playing`). ✓
- **vc.play/pause/resume/skip/stop/seek/queue/now-playing** — vc-go `server.go:36-47` (all), vc-rust `main.rs:1290-1342` (all), vc-ts `main.js:427-515` (all). ✓
- **fx.apply** — `audio-fx-rust/src/main.rs:71` (`POST /v1/fx`). ✓
- **queue.vote** — `queue-rust/src/main.rs:136` (`POST /v1/queue/vote`). ✓
- **share.create / share.resolve** — `share-rust/src/main.rs:70,109` (`POST /v1/share`, `GET /v1/share/:token`). ✓
- **lemonic.search / lemonic.download** — `vc-ts/src/main.js:598,605`. ✓
- **search.inline** — `vc-ts/src/main.js:635` (`POST /v1/inline`). ✓
- **search.query** (`POST /v1/search`) — vc-ts `:593` ✓, downloaders `server.py:38` ✓, **netease-go ✗** (only `/v1/netease/search`; no `/v1/search` route). Contract lists netease-go as a search.query service — 2 of 3.
- **spotify.collection** — spotify-rust `:953` ✓; **downloaders ✗ as HTTP**: `providers.py:69` → `spotify.py:345 download_spotify_collection` exists as an internal function only, no `/v1/spotify/collection` route in `server.py`. Contract lists "spotify-rust | downloaders" — 1 of 2 over HTTP.

### 1c. Service endpoints/handlers NOT in the contract

| Service | Extra endpoint | Notes |
|---|---|---|
| vc-ts | `GET /v1/panel`, `POST /v1/panel` | Live control panel (remix button ids). `main.js:435,551`. |
| vc-ts | `POST /v1/volume` | Volume 0–200. `main.js:560`. |
| vc-ts | `POST /v1/lyrics` | LRCLib lyrics. `main.js:574`. |
| radio-js | `GET /v1/radio/status` | `main.js:374`. |
| radio-js | `POST /v1/radio/stop` | `main.js:391` (also reachable via `POST /v1/radio` action=stop). |
| gui-js | `GET /api/search`, `GET /api/stream-url`, `GET /api/audio`, `POST /api/play\|pause\|skip` | `app/main.js:662-671`. `search` falls back CORE → NETEASE_URL; `stream-url`/`play` talk **directly to VC_GO_URL/VC_TS_URL**, contradicting gui-js README's "Talks ONLY to the core … never directly to voice" (`services/gui-js/README.md:5`). |
| all services | `GET /healthz` | Per contract transport note, not an action — fine. |

## 2. Cross-cutting: stores per service

| Service | Store | Evidence |
|---|---|---|
| core-cpp | **Real Mongo → real Redis → JSON file** (in that priority) | `store.cpp:948-973`: tries Mongo (hand-rolled OP_MSG wire client, `store.cpp:705+`), then Redis (hand-rolled RESP, `:79+`), else `state.json` in `PARLAY_DATA_DIR`. `store.hpp:161` reports winning backend. |
| vc-go | **Real Mongo (official driver) when `MONGO_URI` set, else JSON file** | `internal/store/mongo.go` (go.mongodb.org/mongo-driver/v2, ping on connect, "must not silently fall back" `:43-45`); `internal/store/store.go:31` JSON `queues.json` fallback only when `MONGO_URI` unset. |
| vc-rust | **JSON file only** | `src/main.rs:16,48,379,386`: in-memory + `data/vc-state.json`, 1 s flush. No mongo/redis code. |
| vc-ts | **JSON file only** | `src/main.js:16,36`: `data/state.json`, atomic write. |
| queue-rust | **JSON file only** | `src/main.rs:18,45,107,129`: `data/votes.json`. |
| share-rust | **JSON file only** | `src/main.rs:20,39,53,63`: `data/shares.json`. **Dead config:** compose sets `DATABASE_URL=sqlite:/data/share.db` but no sqlite code exists anywhere in the service (grep sqlite/DATABASE_URL in `src/` = zero). |
| spotify-rust | **JSON file only** | `src/main.rs:16,56,196,207`: `data/spotify-state.json`. |
| radio-js | **JSON file only** (+ hardcoded station list) | `src/main.js:42,181`: `data/state.json`; stations from `src/stations.js`. |
| downloaders | Stateless (JSON only as wire parsing) | No store. |
| netease-go | Stateless | No store. |
| audio-fx-rust | Stateless | No store. |
| gui-js | Stateless proxy (+ demo state) | No store. |

**Verdict on the stale doc claim:** `PHASE4-TDLIB.md:114` says "MongoDB/Redis still JSON-file stores" — **outdated**. core-cpp and vc-go now have real Mongo/Redis paths (verified above, and `phase4/mongo-data` + `mongod.log` show a local mongod was run). The other 6 stateful services remain JSON-file only, so there is **no shared durable store across engines**: a queue in vc-go/Mongo is invisible to vc-ts/vc-rust JSON files and vice versa.

## 3. Creds-gated features

| Feature | Gate (env var) | Behaviour without it | Evidence |
|---|---|---|---|
| Spotify client-credentials (metadata, track lists, search, collection) | `SPOTIFY_CLIENT_ID` + `SPOTIFY_CLIENT_SECRET` | spotify-rust `/v1/spotify/status` reports `credentials_configured:false`; downloaders `spotify_api.py` app-token flow unavailable; vc-ts resolver falls back to Lemonic/JioSaavn (`resolvers.js:309,352-353,430`) | `spotify-rust/src/main.rs:321-341`, `downloaders/src/spotify_api.py:22` |
| Spotify per-user playback monitoring (watch) | **Per-user OAuth token — NOT implemented** | `spotify.watch` only records intent in the registry (`main.rs:105-113`, watches map); `main.rs:283-287`: "this service has no user tokens … No per-user playback state: that still needs a user OAuth token". No OAuth/refresh-token code anywhere. | `spotify-rust/src/main.rs:11,32-33,283-287` |
| Lemonic API | `LEMONIC_API_KEY` (+ optional `LEMONIC_API` base, default `https://api.lemonic.xyz`) | Works unauthenticated first (upstream has no API key); key only adds a Bearer header when set. Lemonic unreachable from sandbox per PHASE4. | `vc-ts/src/resolvers.js:285-315` |
| Deezer full tracks | **ARL cookie — NOT implemented** | Previews only: 30 s `track.preview` MP3 from the public API; `_PREVIEW_NOTE` says so. No `ARL` string anywhere in `services/downloaders/`. | `services/downloaders/src/deezer.py:14-19,41,118-129` |
| NetEase direct URLs | `MUSIC_U` login cookie — **NOT implemented, NOT read** | Anonymous eapi/weapi/linuxapi only + public mirror fallback (see §3.1). No `MUSIC_U` env read in netease-go; the cookie is only mentioned in comments (`eapi.go:16-18`, `client.go:245`). | `services/netease-go/internal/netease/` |
| Navidrome download | `NAVIDROME_URL` + `NAVIDROME_USER` + `NAVIDROME_PASSWORD` | Missing vars → `engine_unavailable`. **Wiring bug:** `.env.example:20` and `docker-compose.yml:117` use `NAVIDROME_PASS`; code reads `NAVIDROME_PASSWORD` (`navidrome.py:40-45`). Compose therefore never passes the password. | `services/downloaders/src/navidrome.py:10-11,40-45` |
| Navidrome Soulseek path | **slskd daemon — NOT implemented** | No `soulseek`/`slskd` string anywhere in `services/downloaders/src/*.py`. README advertises "Soulseek/Navidrome" downloads; only the Navidrome HTTP API path exists. | grep soulseek/slskd = zero hits in downloaders |
| Genius / Musixmatch lyrics (spotify-rust skip-check) | `GENIUS_ACCESS_TOKEN` / `MUSIXMATCH_API_KEY` — **named in a doc comment only, never read** | Lyrics fall back to LRCLib (keyless). | `spotify-rust/src/main.rs:41-42` (comment only) |
| Voice join (assistant session) | `API_ID`/`API_HASH` + `ASSISTANT_SESSION_FILE` (bridge) or `TD_API_ID`/`TD_API_HASH`/`TD_SESSION_DIR` + `TD_PHONE`/`TD_CODE`/`TD_PASSWORD` (TDLib) | No join possible; `UnavailableVoiceCall` with exact reason. | §7 |
| vc-go autoplay live picks | `yt-dlp` binary installed | Unit-tested with injected picker only; live path needs yt-dlp. | PHASE4.md:247; `vc-rust` uses `YTDLP_BIN`, vc-ts `YT_DLP` |

### 3.1 NetEase discrepancy — PHASE4 vs PHASE4-TDLIB.md §5: PHASE4 is right NOW

- `PHASE4-TDLIB.md:114` (§5.5): "NetEase download still `engine_unavailable` (weapi encryption, Phase 3) — untouched by this task."
- `PHASE4.md:82-89`: "Download works live: 210049 … 34,918,981 B FLAC (294.6 s), 509781655 27.4 MB, 490595315 22.2 MB — all ffprobe-verified. … the public mirror fallback is what delivers."
- **Code today supports PHASE4.** `services/netease-go/internal/netease/client.go:230-335` `SongURLWithLevel` tries 5 direct encrypted routes (eapi v1 → weapi v1 → weapi legacy → eapi download URL → linuxapi) and then step 6, `mirrorURL` (`:336-360`, `https://music-api.gdstudio.xyz/api.php?types=url`, host allow-listed to `*.music.126.net` / `*.music.163.com`). `handleDownload` (`internal/server/server.go:212-262`) writes the real file and returns `level:"mirror"` in the response; failures still surface as `engine_unavailable` (never faked). The TDLib doc line is **stale** — it predates worker B2's mirror work. Caveat: there is **no automated live test in the tree** (only `crypto_test.go`; no `server` tests), so the "works live" claim rests on PHASE4.md's reported manual run, not a re-runnable test. Direct (non-mirror) URLs still need `MUSIC_U`, which is not implemented.

## 4. Core-cpp honest limits — verified in code

All six PHASE4 §6 limits check out, with file:line evidence:

1. **Mute flag-only** — `services/core-cpp/src/dispatcher.cpp:1666-1678` (`onMute`): stores `muted` 1/0 per chat, replies "Muted (applies to the next track the engine starts.)"; comment at `:1672-1675`: "The vc services expose no mute action". No `vc.mute` contract action exists (§1a).
2. **Speed/bass seek-restart** — `dispatcher.cpp:1553-1611` (`onSpeed`) and `:1612-1660` (`onBass`): validate range (0.5–2.0 / 0–100), store the value per chat, then call the `seek` IPC action at the current position (`:1589-1605`, comment "Restart the stream at the same position with the new speed … the atempo filter itself is applied by engines that support it — the stored speed is what /pos and /speed report"). No engine is told the speed/bass value directly.
3. **`/play` from replied media needs file_id hand-off** — `dispatcher.cpp:946-953`: if query is empty and the message replies to media, replies "Playing a replied-to file needs the downloaders service (file_id hand-off); use /play <title or link> for now." and returns.
4. **`/lang` English-only** — `dispatcher.cpp:2227-2246` (`onLang`): non-`en` code → "Language \"x\" is not shipped in this build (English only). Your choice was not stored."; `en` stored. Note: `/autoplay lang` (`:2342-2371`) accepts `auto|hindi|english` as a separate `ap_lang` field — unrelated to UI locale.
5. **Broadcast failures inline** — `dispatcher.cpp:2677-2744` (`onBroadcast`): failed target ids appended to the summary message (`:2733-2737`, "Failed ids:\n" + first 3000 chars), comment `:2736-2738`: "NoxBeats sends errors.txt; the core has no sendDocument yet, so the ids go inline".
6. **Speedtest download-only** — `dispatcher.cpp:2811-2836` (`onSpeedtest`): GETs `https://speed.cloudflare.com/__down?bytes=25000000`, reports download Mbps, "(Upload probe skipped: the core measures download only; NoxBeats also uploads 8 MB.)".

## 5. docker-compose.yml

- **Never run end-to-end: no evidence of any run.** No `.env` in the tree, no compose logs, no built images. `docker` binary is **not installed** in this sandbox (`which docker` → not found), so `docker compose config` / `up` cannot even be attempted here.
- **It cannot build as written: 10 of 12 build services have no Dockerfile.** Only `services/radio-js/Dockerfile` and `services/vc-ts/Dockerfile` exist. Missing Dockerfiles: core-cpp, vc-go, vc-rust, audio-fx-rust, share-rust, queue-rust, spotify-rust, gui-js, downloaders, netease-go. `docker compose up --build` would fail on the first missing Dockerfile.
- **Default profile is partial by design:** only core-cpp, vc-go, downloaders, mongo, redis start without `--profile full`; vc-ts, vc-rust, audio-fx-rust, share-rust, queue-rust, spotify-rust, gui-js, netease-go need `full`. So the default `up` starts a core whose FX/Share/Queue/Spotify/Radio/NetEase URLs point at services that are not running (core URLs are env-configured, not health-gated).
- **PORT mismatches — compose sets no `PORT` for any Rust/Python service, so code defaults apply, and 4 of them disagree with the compose mapping and the core's URLs:**
  | Service | Compose maps | Core calls | Service actually listens (default) | Result |
  |---|---|---|---|---|
  | audio-fx-rust | 8004:8004 | `FX_URL` :8004 | **8003** (`src/main.rs:163`) | broken |
  | share-rust | 8005:8005 | `SHARE_URL` :8005 | **8004** (`src/main.rs:61`) | broken |
  | queue-rust | 8006:8006 | `QUEUE_URL` :8006 | **8005** (`src/main.rs:123`) | broken |
  | downloaders | 8009:8009 | `DL_URL` :8009 | **8010** (`src/server.py:47`) | broken |
  | vc-rust | 8003:8003 | :8003 | 8003 (`:1493`) | OK |
  | spotify-rust | 8007:8007 | :8007 | 8007 (`:967`) | OK |
  | vc-go / vc-ts / radio-js / netease-go / gui-js / core-cpp | as mapped | as mapped | 8001 / 8002 / 8008 / 8010 / 3000 / 8080 | OK |
  The three Rust defaults look systematically shifted by one (fx 8003, share 8004, queue 8005 vs contract 8004/8005/8006). `services/core-cpp/README.md` claims "Defaults match docker-compose.yml (… audio-fx-rust :8004, share-rust :8005, queue-rust :8006 …)" — that claim is **false** for these three, and for downloaders (:8009 claimed vs 8010 actual).
- **Other compose gaps:**
  - `NAVIDROME_PASS` passed, code reads `NAVIDROME_PASSWORD` (§3) — Navidrome auth silently broken in compose.
  - No `PORT` env set anywhere, so the four mismatches above are live, not theoretical.
  - `VC_ENGINE` is in `.env.example` but read nowhere (§6) — the engine is chosen per-chat in core, default Go.
  - mongo/redis have no `healthcheck` and core-cpp's `depends_on` has no `condition: service_healthy`; core can start before mongo/redis are ready (core does fall back to file store, so this degrades silently rather than crashing).
  - `core-cpp` mounts `parlay-data:/data` but its default `PARLAY_DATA_DIR` is `data` (relative) — no `PARLAY_DATA_DIR=/data` is set in compose, so the volume is unused by core unless the binary runs with cwd `/`.
  - `TDJSON_LIB`/`NTGCALLS_LIB`/assistant-session vars are absent from compose entirely, so the TDLib/bridge voice path can never activate under compose (see §7).
  - No `LEMONIC_API_KEY`, `SPOTIFY_CLIENT_ID/SECRET`, `TD_*`, `OWNER_ID`, `SUDO_IDS` passed to any service except the two Spotify vars to queue-rust/spotify-rust.

## 6. .env.example vs env vars actually read

`.env.example` (14 vars): `BOT_TOKEN, API_ID, API_HASH, PARLAY_TOKEN, MONGO_URI, REDIS_URL, VC_ENGINE, VC_PROFILE, SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, LEMONIC_API_KEY, NAVIDROME_URL, NAVIDROME_USER, NAVIDROME_PASS`.

**In `.env.example` but NOT read in any service code:**
- `VC_ENGINE` — zero hits in `services/`, `contracts/`, `*.md`, compose. Dead variable.
- `NAVIDROME_PASS` — code reads `NAVIDROME_PASSWORD` instead (§3, §5).

**Read in code but missing from `.env.example` (grouped):**
- core-cpp (`src/config.cpp:48-87`): `VC_GO_URL, VC_TS_URL, VC_RUST_URL, FX_URL, SHARE_URL, QUEUE_URL, SPOTIFY_URL, DL_URL` (+ alias `DOWNLOADERS_URL`), `RADIO_URL, NETEASE_URL, PORT, OWNER_ID, SUDO_IDS, LOGGER_ID, PARLAY_ALLOW_CHATS, PARLAY_BLOCK_CHATS, ASSISTANT_IDS, ASSISTANT_SESSION_FILE, NTGCALLS_LIB, VOICE_BRIDGE, VOICE_RTP_SINK, TD_API_ID, TD_API_HASH, TD_SESSION_DIR, TDJSON_LIB, TD_PHONE, TD_CODE, TD_PASSWORD, PARLAY_DATA_DIR, PARLAY_LOG_FILE, TELEGRAM_API_BASE, DURATION_LIMIT, QUEUE_LIMIT, SUPPORT_CHAT` (36 vars; only `BOT_TOKEN, API_ID, API_HASH, PARLAY_TOKEN, MONGO_URI, REDIS_URL` of core's reads are in the example).
- vc-go: `MONGO_URI, PARLAY_TOKEN, PARLAY_DATA_DIR, VC_PROFILE, PORT`.
- vc-ts: `API_ID, API_HASH, ASSISTANT_SESSION_FILE, LEMONIC_API` (base URL, no KEY), `PARLAY_VOICE_PYTHON, VOICE_BRIDGE, VOICE_RTP_SINK, YT_DLP` (+ `PORT, PARLAY_TOKEN, PARLAY_DATA_DIR, SPOTIFY_CLIENT_ID/SECRET, LEMONIC_API_KEY` — in example except `LEMONIC_API`).
- vc-rust: `API_ID, API_HASH, ASSISTANT_SESSION_FILE, PARLAY_VOICE_PYTHON, VOICE_BRIDGE, VOICE_RTP_SINK, YTDLP_BIN` (note: different name from vc-ts's `YT_DLP`), `PARLAY_LIVE_TEST, PARLAY_DATA_DIR, PARLAY_TOKEN, PORT`.
- downloaders: `NAVIDROME_PASSWORD` (not `PASS`), `PARLAY_DL_DIR, PARLAY_SPOTIFY_MAX_MATCH_SEC, PORT` (+ `NAVIDROME_URL/USER, PARLAY_TOKEN, SPOTIFY_CLIENT_ID/SECRET`).
- netease-go: `PORT, PARLAY_TOKEN, PARLAY_DL_DIR`.
- queue-rust: `PARLAY_VOTE_THRESHOLD, PARLAY_DATA_DIR, PARLAY_TOKEN, PORT`.
- share-rust: `PARLAY_BOT_USERNAME, PARLAY_DATA_DIR, PARLAY_TOKEN, PORT`.
- spotify-rust: `PARLAY_DATA_DIR, PARLAY_TOKEN, PORT` (+ Spotify creds).
- audio-fx-rust: `PARLAY_DATA_DIR, PARLAY_TOKEN, PORT`.
- radio-js: `VC_GO_URL, VC_TS_URL, PARLAY_DATA_DIR, PARLAY_TOKEN, PORT`.
- gui-js: `CORE_URL, NETEASE_URL, VC_GO_URL, VC_TS_URL, PARLAY_TOKEN, PORT`.
- Named-but-never-read: `GENIUS_ACCESS_TOKEN`, `MUSIXMATCH_API_KEY` (doc comment only, §3).

Net: `.env.example` covers 12 of ~60 env vars actually read; a user copying it gets a core that can start (BOT_TOKEN/PARLAY_TOKEN) but no owner/sudo config, no TDLib/bridge voice config, no per-service URLs, and a broken Navidrome password name.

## 7. TDLib wiring

- **`libtdjson.so` exists and is the real 1.8.69 build:** `phase4/tdlib-src/build/libtdjson.so -> libtdjson.so.1.8.69`, **50,166,944 bytes**, dated 2026-11-02 16:41. Also `libtdjson_private.a` and `libtdjson_static.a` in the same dir.
- **`libntgcalls.so` is absent.** No file named `libntgcalls.so` anywhere in the tree (excluding `refs/`). The only ntgcalls binaries are the **Python extension**: `phase4/ntg-extract/ntgcalls.cpython-312-x86_64-linux-gnu.so` (135,970,920 B) and its installed copy in `phase4/voice-venv/lib/python3.12/site-packages/` — a CPython module, **not** the C `libntgcalls.so` the C++ dlopen path needs.
- **`phase4/tdlib-build.log` is 7 lines and is NOT the TDLib build log.** Full contents: a timestamp header "extract ntgcalls wheel", an `ls` of the extracted wheel dir, nothing else. It documents only the ntgcalls wheel extraction (2026-11-02T15:13:28Z). The TDLib build itself (claimed 100%, ~56 min in PHASE4-TDLIB.md) has no log file in the tree; the only evidence is the built `.so` + `phase4/tdlib-src/build/CMakeCache.txt` (2026-11-02 15:46).
- **Files referencing `libntgcalls.so` / `TDJSON_LIB` / `TD_API_ID` and what happens when absent:**
  - `services/core-cpp/src/tdlib_client.cpp:35-36`: default lib path is **hardcoded to the sandbox absolute path** `/home/hatch/workspace/parlay/phase4/tdlib-src/build/libtdjson.so` (`kDefaultLib`); candidates are explicit ctor arg → `$TDJSON_LIB` → `kDefaultLib` → `libtdjson.so` (`:61-66`). If none dlopens: `last_error_ = "libtdjson.so not found (dlopen: …)"` (`:76-79`); missing `td_json_client_*` symbols → same honest failure (`:95-101`).
  - `services/core-cpp/src/tdlib_client.cpp:49-56`: `Params::fromEnv` reads `TD_API_ID` (falls back to `API_ID`), `TD_API_HASH` (falls back to `API_HASH`), `TD_SESSION_DIR`, `TD_PHONE`, `TD_CODE`, `TD_PASSWORD`. Auth state machine `WaitTdlibParameters → … → Ready` (`phase4/tdlib-wiring-a.md:20`); missing credential at any step → `false` + `"not authorized: no credentials (…)"` (`tdlib_client.cpp:215`).
  - `services/core-cpp/src/voice_call.cpp:191-204`: `NtgCallsVoiceCall` searches `./lib/libntgcalls.so`, `/usr/local/lib/libntgcalls.so`, `/usr/lib/libntgcalls.so`, `/usr/lib/x86_64-linux-gnu/libntgcalls.so`, `libntgcalls.so` (plus `$NTGCALLS_LIB` via `config.cpp:72`); absent → `last_error_ = "libntgcalls.so not found (dlopen: …)"`.
  - `services/core-cpp/src/voice_call.cpp:886-916` (`makeVoiceCall` factory order): **TdLib** (probe `available()`: creds set + both `.so` load) → **VoiceBridge** (Telethon + ntgcalls via `services/core-cpp/voice_bridge.py`, needs `ASSISTANT_SESSION_FILE`) → **dlopen ntgcalls** → **`UnavailableVoiceCall`** carrying the exact combined reason (`:904-916`: which of bridge/tdlib/ntgcalls failed and why). `voice_call.hpp:259-260` documents the same order.
  - `services/core-cpp/src/config.hpp:52,57-60` + `config.cpp:72,75-78`: `NTGCALLS_LIB`, `TD_API_ID`, `TD_API_HASH`, `TDJSON_LIB` env bindings.
  - `services/core-cpp/tests/tdlib_test.cpp:35-41`: tests unset `TD_API_ID`/`TD_API_HASH`/`TDJSON_LIB` to prove the honest-off path.
  - `phase4/tdlib-wiring-a.md:22`, `phase4/bench-core-cpp.md:84,110`, `phase4/deepread-cpp.md:19,62,78,83`: design docs confirming the same fallback chain; bench note confirms "With no assistant session and no `libntgcalls.so`: factory selects [Unavailable]".
- **Net effect today:** TDLib *library* is present and loadable, but the voice path is dead in practice: `libntgcalls.so` (C) is absent, so `TdLibVoiceCall` can never become `available()` (it requires **both** `.so`s), and the bridge path needs an assistant session that was never supplied. The failure mode is honest (exact reason string, no fake join), but **no live voice join has happened or can happen from this tree as-is** — consistent with PHASE4-TDLIB.md §5. Note the TDLib default lib path is sandbox-absolute, so even the TDLib half would not resolve under `docker compose` (no mount, no `TDJSON_LIB` set).

## 8. README quick-start — mental walkthrough

Root `README.md` (69 lines) is **stale and not followable as a quick-start**:

1. `docker compose up --build` — **breaks immediately**: docker isn't installed in the sandbox, and 10/12 services have no Dockerfile (§5). The "Status" section still says "Scaffold only… stubs (a README plus a `main` that prints its name and exits)" — false since Phases 2–4; services are real.
2. `cd services/core-cpp && cmake -B build && cmake --build build` — **works** (CMakeLists.txt exists; measured 35.1 s clean in PHASE4). But running the binary then exits `missing required env vars: BOT_TOKEN` unless env is set; the README's config section (`cp .env.example .env`) doesn't mention that `BOT_TOKEN` + `PARLAY_TOKEN` are the only hard requirements, nor that most of the example vars are unused/dead (§6).
3. `cd services/vc-go && go run ./cmd/parlay-vc` — **works** (go.mod exists). Default port 8001, token optional (warns).
4. `cd services/vc-rust && cargo run` — **works** (Cargo.toml exists). Default port 8003.
5. `cd services/gui-js && npm install && npm run dev` — **misleading**: there are **no npm dependencies** (`package.json` has no `dependencies`; `dev` = `node app/main.js`); `npm install` is a no-op and there is no Next.js despite the README's "Next.js Mini App" claim — the GUI is a plain stdlib Node server (`services/gui-js/README.md` admits this; root README doesn't). It will start on :3000 but serve **demo state** unless `CORE_URL` points at a core that doesn't implement `/api/*` (§1a).
6. **Missing from the quick-start entirely:** downloaders (needs `python3 -m src.main` / venv at `services/downloaders/.venv`, `requirements.txt`), netease-go (`go run ./cmd/parlay-netease`), radio-js/vc-ts (`node src/main.js`), the five Rust services' `cargo run`, the `PARLAY_TOKEN` that must match across services, the mongo/redis the core prefers, and the fact that the voice path needs an assistant session + `libntgcalls.so` that doesn't exist.
7. Ports: README never lists the port map (8001–8010, 3000, 8080); the only port table is in `docker-compose.yml` and `services/core-cpp/README.md` (which misstates 4 defaults, §5).
8. `cp .env.example .env` — **silently wrong** in two places (§6): `VC_ENGINE` is dead, `NAVIDROME_PASS` is never read.

Per-service READMEs are better: `services/core-cpp/README.md` (build/run/env, modulo the 4 wrong port defaults) and `services/gui-js/README.md` (accurate endpoint table, but contradicts the root README on "talks ONLY to the core"). `services/vc-go/README.md` documents `VC_PROFILE` correctly.

## 9. Summary of gaps (for the coordinator)

**Must-fix before any "it runs" claim:**
1. Compose cannot build (10 missing Dockerfiles) and 4 services listen on the wrong default port (audio-fx 8003≠8004, share 8004≠8005, queue 8005≠8006, downloaders 8010≠8009) — core's IPC calls to those four fail.
2. `NAVIDROME_PASS` vs `NAVIDROME_PASSWORD` — name mismatch in `.env.example` + compose vs code.
3. vc-ts lacks `/v1/loop`, `/v1/shuffle`, `/v1/autoplay`, `/v1/autoplay-skip`; core returns the 404 without fallback (404 ≠ unavailable).
4. `gui.now_playing` / `gui.queue` have no real backend — core has no `/api/*` surface; gui-js serves demo data.
5. `vc.join` / `vc.mute` absent from contract and IPC (mute is a core flag only; join lives only inside voice backends).
6. `libntgcalls.so` (C) absent; only the Python extension exists. TDLib `.so` present (50,166,944 B) but its default path is sandbox-absolute; no live join possible from this tree.
7. `.env.example` covers ~12 of ~60 vars read; `VC_ENGINE` dead; `TD_*`/owner/sudo/URLs all missing.
8. Root README quick-start is stale (scaffold text, broken compose command, wrong gui-js instructions).

**Resolved / not-a-gap (verified):** download.* 6/6, netease.* 3/3, spotify.* 7/7, vc.radio, radio.list/start, fx, queue.vote, share, lemonic, search.inline all implemented. **NetEase: PHASE4 is right, PHASE4-TDLIB.md §5 is stale** — mirror fallback is in code (`client.go` step 6) and PHASE4 reports 3 ffprobe-verified live FLAC downloads; no automated live test exists to re-prove it. **Stores:** core-cpp and vc-go have real Mongo (Redis too for core); the other six stateful services are JSON-file only with no shared store. **Core honest limits:** all six verified in code with exact lines (§4). **Creds:** Spotify OAuth (per-user), Deezer ARL, NetEase MUSIC_U, Soulseek/slskd, Genius/Musixmatch keys are all unimplemented/named-only; Spotify client-creds, Lemonic key, Navidrome creds are the only live gates.
