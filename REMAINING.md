# Parlay — REMAINING.md (stub-free audit)

Date: 2026-11-03. Auditor (subagent), audit-only: read + grep + re-ran builds/tests.
No service code modified, nothing pushed, no tokens requested. Live Telegram
tests are ASIDE per the user — they are listed only in (E) and nowhere else.

Method: read PHASE2.md, PHASE3.md, PHASE4.md, PHASE4-TDLIB.md, PORT_MAP.md,
DEEP_READ.md, COMMANDS.md, BENCHMARKS.md, ARCHITECTURE.md, contracts/ipc.json
(41 actions), docker-compose.yml, README.md; grep of services/ (excluding
refs/, .venv, node_modules, build/target dirs, phase4/tdlib-src) for stub,
TODO, FIXME, XXX, HACK, NotImplemented, unimplemented, engine_unavailable,
placeholder, dummy, mock, pass-through; three parallel workers (A grep+docs,
B builds+tests, C contract+cross-cutting+TDLib) with full reports in
`audit/worker-{a,b,c}-*.md` and raw logs in `audit/logs/`.

## Audit verdict (one paragraph)

The code is in much better shape than the docs say: all 12 services build
clean (0 build failures), 146 tests pass / 0 fail / 8 skipped on re-run, and
there are zero TODO/FIXME/HACK/mocks in the tree. The one project-level
blocker is that **no audio has ever reached a Telegram voice chat** — the
Opus/RTP pipeline is proven to a file in all three engines, but the live join
has never happened and cannot happen from this sandbox. The next-biggest
cluster is wiring, not code: docker-compose cannot build (10 missing
Dockerfiles) and 4 services listen on the wrong ports under compose, and the
core serves no HTTP so the GUI's now-playing/queue endpoints have no backend.
The third cluster is documentation drift: root README still says "scaffold
only", ARCHITECTURE.md describes a stack that was never built (grammy /
tgcalls / Next.js / ntgcalls / Soulseek / shared Mongo+Redis state), and
several service READMEs contradict their own implemented sections.

Two corrections to prior reports, verified by the auditor:
- Worker C claimed `phase4/tdlib-build.log` is 7 lines and not the TDLib
  build log. Wrong file: the 7-line log is `~/workspace/parlay/tdlib-build.log`
  (ntgcalls wheel extraction). The real build log is
  `phase4/tdlib-build.log`: 933 lines, 0 `error:` lines, ends
  `[100%] Built target run_all_tests`. TDLib 1.8.69 build SUCCEEDED.
- PHASE4-TDLIB.md §5.5 "NetEase download still `engine_unavailable`" is
  stale. PHASE4 is right: the mirror fallback is in code
  (`netease-go/internal/netease/client.go:336-360`, `SongURLWithLevel` step 6)
  and PHASE4 reports 3 ffprobe-verified live FLAC downloads. Caveat: no
  automated live NetEase test exists in the tree (only `crypto_test.go`).

Verified-good (not gaps, for the record): all 12 services build, 146 tests
pass; `download.*` 6/6, `netease.*` 3/3, `spotify.*` 7/7, `vc.radio`,
`radio.list/start`, `fx.apply`, `queue.vote`, `share.*`, `lemonic.*`,
`search.inline` all implemented; core-cpp `store_test` is now 40/40 with DBs
up (supersedes the old 30/2); `libtdjson.so.1.8.69` exists (50,166,944 B).

---

## (A) Stub-free blockers — still stubbed/faked, must be replaced with real code

### A1. No live voice join anywhere — audio never reaches a voice chat
- Evidence: `services/vc-ts/src/voicecall.js:399` (`opus-file-only:<reason>`),
  `services/vc-rust/src/voice.rs:957`, `services/core-cpp/src/voice_call.cpp:488-489`
  (`rtp_ok=0`). All three report `voice_joined:false` honestly. Opus/RTP
  pipeline proven to a file in all three (251 frames / 251 RTP packets,
  83,736 B, 5.0065 s); local ntgcalls offer (456–476 chars) proven offline.
- What blocks it here: no assistant session, no direct TCP to Telegram DCs
  (proxy-only egress), UDP `sendto` EPERM even to loopback (proven in
  Python/Node/Rust + ffmpeg), `libntgcalls.so` (C) absent — only the Python
  extension exists (`phase4/ntg-extract/ntgcalls.cpython-312-*.so`,
  135,970,920 B). `TdLibVoiceCall::available()` is therefore always false
  here (it requires both `.so`s).
- What done looks like: a host with open network + assistant session +
  a built `libntgcalls.so`, one `/play` through the TDLib backend (or the
  Telethon bridge) with a real `joined:true` and audible audio in the call.
  Building `libntgcalls.so` itself is the first sub-step (no C build has
  been attempted yet — only the Python wheel was extracted).

### A2. `gui.now_playing` / `gui.queue` have no real backend
- Evidence: `contracts/ipc.json` lists both as `core-cpp → gui-js`, but
  core-cpp has no HTTP server at all (zero `/api/` hits in
  `services/core-cpp/src/`; `services/core-cpp/src/config.hpp:40`:
  `port = 8080 // GUI /api surface, future`). gui-js
  `app/main.js:459-492` proxies to `CORE_URL` and falls back to hardcoded
  demo state (`note: "core unreachable or CORE_URL unset — serving local
  demo state"`, queue `source:"local-demo"`).
- What done looks like: core-cpp serves `GET /api/now-playing` and
  `GET /api/queue` (real engine state via IPC), gui-js never serves demo
  data when a core is configured.

### A3. vc-ts is missing 4 contract actions; core fail-over does not catch it
- Evidence: `vc.loop`, `vc.shuffle`, `vc.autoplay`, `vc.autoplay_skip`
  exist in vc-go (`internal/server/server.go:43-46`) and vc-rust
  (`src/main.rs:1339-1342`) but vc-ts `src/main.js` has no such routes
  (only panel toggles, `main.js:263-264,309-312`); unknown route → `404
  not_found` (`main.js:652`). Core `engine_router.cpp` retries only on
  `unavailable()` (`ipc.hpp:24-27`: transport error, HTTP ≥500, or
  `engine_unavailable`) — a 404 is not unavailable, so the core returns
  the 404 without trying the next engine. Default engine is Go, so this
  bites only ts-engine chats.
- What done looks like: the 4 routes implemented in vc-ts, plus the core
  treating a 404 for a known contract action as a fail-over trigger (or
  the router knowing per-engine capability).

### A4. `vc.join` and `vc.mute` exist nowhere in the contract or IPC
- Evidence: no `vc.join`/`vc.mute` key in `contracts/ipc.json`, no
  `/v1/join` or `/v1/mute` route in any service. Mute is a core-cpp flag
  only (`dispatcher.cpp:1666-1678`, stored per chat, reported by `/pos`);
  `voice_call.hpp:65-66` declares `mute()/unmute()` on the voice backend
  but nothing reaches it over IPC. Join lives only inside voice backends
  (TDLib `joinVideoChat`, bridge `join-call`).
- What done looks like: both actions added to `contracts/ipc.json` first
  (contract-first rule), then implemented in at least one VC engine and
  wired in the core dispatcher (`/mute`, `/join`).

### A5. Compose port mismatches — 4 services unreachable under compose
- Evidence: compose sets no `PORT` env anywhere, so code defaults apply:
  audio-fx-rust listens 8003 (`src/main.rs:163`) but compose maps 8004;
  share-rust 8004 (`src/main.rs:61`) vs 8005; queue-rust 8005
  (`src/main.rs:123`) vs 8006; downloaders 8010 (`src/server.py:47`) vs
  8009. audio-fx and vc-rust both default to 8003 (collision). Core
  `FX_URL`/`SHARE_URL`/`QUEUE_URL`/`DL_URL` point at the mapped ports.
- What done looks like: one source of truth — either set `PORT` in
  compose for each service or change code defaults to the mapped ports —
  and a compose smoke test proving core→each service IPC answers.

### A6. docker-compose cannot build as written; never run end-to-end
- Evidence: only 2 Dockerfiles exist (`services/radio-js/Dockerfile`,
  `services/vc-ts/Dockerfile`); missing for core-cpp, vc-go, vc-rust,
  audio-fx-rust, share-rust, queue-rust, spotify-rust, gui-js,
  downloaders, netease-go. `docker compose up --build` fails on the
  first missing Dockerfile. Docker is not installed in the sandbox, so
  compose has never run here; no `.env`, logs, or images exist. Default
  profile starts only core-cpp, vc-go, downloaders, mongo, redis — the
  other 7 services need `--profile full`, while the core's FX/Share/
  Queue/Spotify/Radio/NetEase URLs point at services that are not
  running. No healthchecks; core `depends_on` has no
  `condition: service_healthy`; core's `PARLAY_DATA_DIR` defaults to
  relative `data` while compose mounts `parlay-data:/data` (volume
  unused unless cwd is `/`); no `TDJSON_LIB`/`NTGCALLS_LIB`/assistant-
  session vars in compose at all.
- What done looks like: a Dockerfile per service, `docker compose config`
  clean, and one `docker compose up` on a host with Docker proving
  healthz on every mapped port.

### A7. Mongo SCRAM auth not implemented (core-cpp)
- Evidence: `services/core-cpp/src/store.cpp:709` — "mongodb URIs with
  credentials are not supported by the minimal [client]"; authenticated
  URIs rejected. Non-blocking under current no-auth compose, blocking
  for any auth-required Mongo (incl. Atlas/managed).
- What done looks like: SCRAM-SHA-1/256 in the hand-rolled Mongo wire
  client, tested against an auth-enabled mongod.

### A8. vc-go `StreamURL` never serialized over IPC
- Evidence: `services/vc-go/internal/types/types.go:20` —
  `StreamURL string \`json:"-"\`` ("Internal to vc-go; not part of the
  contract"). No `stream_url` in `server.go` output, so the core cannot
  fetch vc-go's resolved direct audio URL despite vc-go's README saying
  it can.
- What done looks like: `stream_url` in the `Track`/`NowPlaying` IPC
  payload (and in `contracts/ipc.json` types), or the README corrected
  and the gui `/api/stream-url` flow documented as vc-ts-only.

### A9. Degraded placeholder tracks (partial fidelity, not full stub)
- Evidence: `services/vc-ts/src/resolvers.js:125,136,149` — yt-dlp
  failure → placeholder track (`yt:<id>` / `query:<hash>`,
  `duration_sec: 0`, `degraded: true`, `resolve_note` says so); never
  auto-advances, no real metadata. `services/vc-rust/src/main.rs:49,564`
  — placeholder 180 s duration for unknown-duration tracks; ticker
  advances state-wise instead of stalling.
- What done looks like: no placeholder path in normal operation — a
  failed resolve surfaces `engine_unavailable` (or retries with the
  android client, as downloaders already do) instead of a fake track;
  if a placeholder is kept as a last resort, it must carry a real
  duration or be excluded from auto-advance timing.

### A10. TDLib first-time auth flows not implemented
- Evidence: `services/core-cpp/src/tdlib_client.cpp:298` —
  "(registration / QR / email flows are not implemented …)". Auth needs
  phone/code/password via env; a pre-authorized session dir is required.
- What done looks like: either QR/login-code interactive flow in the
  TDLib client, or documented support for importing a pre-authorized
  session dir (and the compose/docs path for it).

### A11. No shared durable store across engines
- Evidence: core-cpp = real Mongo → Redis → JSON file; vc-go = real
  Mongo driver when `MONGO_URI` set, else JSON. All other stateful
  services JSON-file only: vc-rust (`data/vc-state.json`), vc-ts
  (`data/state.json`), queue-rust (`data/votes.json`), share-rust
  (`data/shares.json`), spotify-rust (`data/spotify-state.json`),
  radio-js (`data/state.json`). A queue in vc-go/Mongo is invisible to
  vc-ts/vc-rust JSON files and vice versa — engine fail-over loses
  queue state. share-rust's compose `DATABASE_URL=sqlite:/data/share.db`
  is dead config (zero sqlite hits in the service).
- What done looks like: one shared store (Mongo/Redis) used by all
  stateful services, or an explicit documented decision that queues do
  not survive engine switches — with the dead sqlite var removed.

### A12. `NAVIDROME_PASS` vs `NAVIDROME_PASSWORD` name bug
- Evidence: `.env.example:20` and `docker-compose.yml:117` set
  `NAVIDROME_PASS`; code reads `NAVIDROME_PASSWORD`
  (`services/downloaders/src/navidrome.py:40-45`). Compose therefore
  never passes the password; Navidrome auth silently fails.
- What done looks like: one name everywhere (code + example + compose),
  verified by a Navidrome call with the compose env.

---

## (B) Working-code gaps — real features from the 21 refs never ported at all

### B1. Per-user Spotify playback monitoring (watch) is intent-only
- Evidence: `services/spotify-rust/src/main.rs:105-113,283-287` —
  `spotify.watch` only records intent in a registry; "this service has
  no user tokens … No per-user playback state: that still needs a user
  OAuth token". No OAuth/refresh-token code anywhere. rustify's
  `auth.rs` was never ported.
- What done looks like: per-user OAuth flow + token refresh + real
  currently-playing polling, or the watch action removed from the
  contract as unimplemented.

### B2. Deezer full tracks (need ARL cookie) — previews only
- Evidence: `services/downloaders/src/deezer.py:14-19,41,118-129` —
  30 s `track.preview` MP3 from the public API; `_PREVIEW_NOTE` says
  so. No `ARL` string anywhere in `services/downloaders/`.
- What done looks like: ARL-cookie full-track download (env-gated),
  or the downloader honestly advertising preview-only (it already does
  in the note — the gap is the feature, not the honesty).

### B3. NetEase direct URLs need `MUSIC_U` login — not implemented/read
- Evidence: mirror fallback works (see verdict); direct encrypted routes
  return `url:null` server-side. No `MUSIC_U` env read in netease-go;
  the cookie is only mentioned in comments (`eapi.go:16-18`,
  `client.go:245`).
- What done looks like: `MUSIC_U` cookie support in netease-go, or an
  explicit decision that mirror-only is the supported path.

### B4. Navidrome Soulseek path — zero code
- Evidence: grep `soulseek`/`slskd` = zero hits in
  `services/downloaders/src/*.py`. Only the Navidrome HTTP API path
  exists. The ref (`music-bot-navidrome`) has a full
  `src/soulseek/{client,downloader,matcher,scorer}.py` that was never
  ported.
- What done looks like: slskd-backed Soulseek fetch (needs a slskd
  daemon), or the README dropping the Soulseek claim.

### B5. Genius / Musixmatch lyrics keys — named in a comment only
- Evidence: `services/spotify-rust/src/main.rs:41-42` (doc comment
  only, never read). Lyrics fall back to LRCLib (keyless) only.
- What done looks like: real Genius/Musixmatch clients (env-gated) or
  the comment removed.

### B6. Beetube non-music providers never ported
- Evidence: `services/downloaders/src/providers.py` registers exactly
  five providers (youtube/spotify/deezer/tiktok/navidrome). Beetube's
  `providers/{movie,anime,torrent}.js` + EDM have no Parlay equivalent.
- What done looks like: movie/anime/torrent provider handlers in
  downloaders, or an explicit music-only scope decision.

### B7. NetEase audio recognition never ported
- Evidence: ref `Music163bot-Go` has `handlers_recognize.go`; contract
  has only `netease.search/download/lyrics`. No recognition endpoint
  in netease-go.
- What done looks like: `netease.recognize` action + handler, or
  dropped from scope.

### B8. Lemonic live API never verified
- Evidence: vc-ts Lemonic works unauthenticated-first with a Bearer
  header when a key is set, but the live API is unreachable from the
  sandbox (PHASE4); verification used a local stub API + the dead-API
  error path only. No live Lemonic search/download has ever succeeded.
- What done looks like: one live Lemonic call succeeding from a host
  with open egress (or a key, if the API ever requires one).

### B9. `spotify.collection` in downloaders is internal-only, not HTTP
- Evidence: `services/downloaders/src/providers.py:69` →
  `spotify.py:345 download_spotify_collection` exists as a function;
  no `/v1/spotify/collection` route in `server.py`. Contract lists
  "spotify-rust | downloaders" — only spotify-rust serves it over HTTP.
- What done looks like: the downloaders route added, or the contract
  corrected to spotify-rust only.

### B10. `search.query` — netease-go has no `/v1/search`
- Evidence: contract lists netease-go as a `search.query` service, but
  netease-go exposes only `/v1/netease/search`. vc-ts and downloaders
  serve `/v1/search`; netease-go does not.
- What done looks like: the route added, or the contract corrected.

### B11. Core honest limits (all verified in code — feature gaps, not stubs)
- `/mute` downstream: flag-only, no `vc.mute` action (see A4).
  `services/core-cpp/src/dispatcher.cpp:1666-1678`.
- `/speed` / `/bass`: seek-restart + stored value; VC services are
  never told the speed/bass value.
  `dispatcher.cpp:1553-1660`.
- `/play` from replied-to media: refuses with a file_id hand-off
  message. `dispatcher.cpp:946-953`.
- `/lang`: English-only; non-`en` refused, choice not stored.
  `dispatcher.cpp:2227-2246`.
- `/broadcast` failures inline, no `errors.txt` attachment
  (core has no sendDocument yet). `dispatcher.cpp:2677-2744`.
- `/speedtest`: download-only (Cloudflare down probe; upload skipped).
  `dispatcher.cpp:2811-2836`.
- What done looks like: each of the six implemented for real (or kept
  as documented limits — they are already honestly reported; the gap
  is the missing feature, and COMMANDS.md §"Not yet" lists four of
  them).

### B12. vc-go live autoplay picks need yt-dlp (unit-tested only)
- Evidence: PHASE4 — `resolver.Related` returns nil without yt-dlp in
  the sandbox, so a drained room idles; the pick path is unit-tested
  with an injected picker only.
- What done looks like: yt-dlp wired into vc-go's resolver (it exists
  at `services/downloaders/.venv/bin/yt-dlp`) and one live autoplay
  pick observed.

---

## (C) Correctness / quality

### C1. Test coverage holes (builds pass, but coverage is thin)
- audio-fx-rust, share-rust, queue-rust: **0 tests defined** each
  (`cargo test` runs 0 tests). Build passes; nothing verifies the
  endpoints. What done looks like: at least smoke tests per endpoint
  (fx output file, share create/resolve round-trip, vote threshold).
- spotify-rust 8/8 is partly hollow: 2 "live" tests early-return as
  no-ops without `SPOTIFY_CLIENT_ID/SECRET` (not counted as skipped by
  the harness). Real exercised tests without creds: 6.
- downloaders: 21 discovered, 13 passed, **8 skipped** (live-gated
  `PARLAY_LIVE_TEST=1`: YouTube, Deezer, TikTok, Spotify API,
  Navidrome). Live coverage never runs without a deliberate flag.
- netease-go: 5/5, all crypto round-trips only. No live NetEase tests;
  the "works live" claim rests on PHASE4's manual run, not a
  re-runnable test.
- Totals on re-run (worker B, 2026-11-03): **146 passed / 0 failed /
  8 skipped** across 12 services. Per-service: core-cpp 47 (tdlib 7/7
  + store 40/40), vc-go 18/18, vc-ts 24/24, vc-rust 17/17,
  spotify-rust 8/8 (see caveat), gui-js 7/7, radio-js 7/7 (startup
  probe 5/6 stations — KEXP unreachable, honestly reported),
  downloaders 13+8 skipped, netease-go 5/5.

### C2. Build warnings and slow builds
- core-cpp: 8 warnings, all libstdc++ `-Wmaybe-uninitialized` false
  positives inside `std::variant`/`std::map` internals when copying
  `parlay::Json` — none in service code, but they are real compiler
  output on every clean build (43 s clean).
- vc-go clean build 92.4 s (Mongo driver compile; warm 8.1 s),
  stripped binary 9.3 MB. Slowest clean build in the tree; no
  incremental-build cache is documented.
- TDLib one-off build: ~56 min, 518 MB of artifacts in
  `phase4/tdlib-src/build/` — not part of any service build, but any
  fresh checkout that needs it pays the cost again (no prebuilt
  artifact is published in the tree).
- All other services: 0 warnings. TS/JS have no build step.

### C3. Documentation drift (runtime is fine; the docs mislead)
- Root `README.md`: "Status: **Scaffold only.** The service
  directories contain original stubs" — false since Phases 2–4.
  Quick-start is not followable: `docker compose up --build` breaks
  (no Docker in sandbox; 10 missing Dockerfiles), gui-js `npm install
  && npm run dev` is misleading (no dependencies, no Next.js —
  `dev` = `node app/main.js`), no port map, no run commands for 8
  services, `cp .env.example .env` silently wrong (see C4).
- `ARCHITECTURE.md`: aspirational stack, several claims false against
  code — "vc-go (Go, ntgcalls)", "vc-ts (TS, grammy)", "vc-rust
  (Rust, ntgcalls)", "gui-js (Next.js Mini App)", "MongoDB + Redis
  (shared state: queues, auth, shares)". Grep: zero grammy/tgcalls/
  Next.js/slskd hits in `services/` code. "The C++ core never
  downloads, never streams" is now false in the narrow voice sense
  (core owns the Opus pipeline + voice bridge).
- `services/core-cpp/README.md`: "TDLib C++ client — `src/tdlib_client`
  does not exist yet" (false — it exists, 7/7 tests); "TDLib build
  running" (finished 100%); "Defaults match docker-compose.yml
  (audio-fx :8004, share :8005, queue :8006, downloaders :8009)"
  (false for all four — see A5).
- `services/core-cpp/PROGRESS.md`: Phase 2 snapshot never updated —
  "voice … logging stub", "`LoggingVoiceCall` stub",
  "MongoDB/Redis — env vars read, not connected", "vc-*/downloader
  services are stubs". All superseded.
- `services/vc-rust/README.md` tail: "No loop/shuffle/autoplay yet"
  and "`yt-dlp` is not installed … pattern-only" — both directly
  contradicted by the same file's Implemented section. `PROGRESS.md`
  L94–99 repeats the stale stub block above its own supersession note.
- `services/netease-go/README.md` "Known live limitation": download
  "honestly returns `engine_unavailable`", "No weapi-encrypted
  requests", "No end-to-end download test" — all false since Phase 4.
  Same wrong claim repeated in `PHASE4-TDLIB.md` §5.5.
- `PHASE2.md` / `PHASE3.md` "still stubbed" sections are partly stale
  (superseded by Phase 4 + TDLib wiring). `PORT_MAP.md` §3: "services/
  contains only original stubs" — stale; its license section is also
  stale relative to the built tree (note: per the user's order, do not
  re-raise licensing as a task — listed here only as docs drift).
- `contracts/ipc.json` still stamped `version: "0.1.0-scaffold"`
  although it now carries 41 actions.
- Extra endpoints not in the contract (docs/contract drift, both
  ways): vc-ts `/v1/panel`, `/v1/volume`, `/v1/lyrics`; radio-js
  `/v1/radio/status`, `/v1/radio/stop`; gui-js `/api/search`,
  `/api/stream-url`, `/api/audio`, `/api/play|pause|skip` — the last
  group talks directly to `VC_GO_URL`/`VC_TS_URL`, contradicting
  gui-js README's "Talks ONLY to the core … never directly to voice".

### C4. `.env.example` covers ~12 of ~60 vars actually read
- In the example but dead/wrong: `VC_ENGINE` (zero hits anywhere in
  code/compose/docs); `NAVIDROME_PASS` (code reads
  `NAVIDROME_PASSWORD` — see A12).
- Read in code but missing from the example (grouped): core-cpp 36 vars
  (`VC_GO_URL, VC_TS_URL, VC_RUST_URL, FX_URL, SHARE_URL, QUEUE_URL,
  SPOTIFY_URL, DL_URL, RADIO_URL, NETEASE_URL, PORT, OWNER_ID,
  SUDO_IDS, LOGGER_ID, PARLAY_ALLOW_CHATS, PARLAY_BLOCK_CHATS,
  ASSISTANT_IDS, ASSISTANT_SESSION_FILE, NTGCALLS_LIB, VOICE_BRIDGE,
  VOICE_RTP_SINK, TD_API_ID, TD_API_HASH, TD_SESSION_DIR, TDJSON_LIB,
  TD_PHONE, TD_CODE, TD_PASSWORD, PARLAY_DATA_DIR, PARLAY_LOG_FILE,
  TELEGRAM_API_BASE, DURATION_LIMIT, QUEUE_LIMIT, SUPPORT_CHAT`);
  vc-ts `LEMONIC_API`, `YT_DLP`; vc-rust `YTDLP_BIN` (different name
  from vc-ts's `YT_DLP`); downloaders `PARLAY_DL_DIR`,
  `PARLAY_SPOTIFY_MAX_MATCH_SEC`, `NAVIDROME_PASSWORD`; netease-go
  `PARLAY_DL_DIR`; queue-rust `PARLAY_VOTE_THRESHOLD`; share-rust
  `PARLAY_BOT_USERNAME`; `PARLAY_DATA_DIR`, `PARLAY_TOKEN`, `PORT` per
  service; radio-js/gui-js service URLs.
- Named but never read: `GENIUS_ACCESS_TOKEN`, `MUSIXMATCH_API_KEY`
  (doc comment only).
- What done looks like: `.env.example` listing every var the code
  reads (with a one-line comment each), dead vars removed, the
  `YT_DLP`/`YTDLP_BIN` naming unified.

### C5. `ipc.json` version + contract hygiene
- Version string `0.1.0-scaffold` (see C3). New actions are added
  here first per the contract note, but the extra endpoints in C3
  were never added — the contract is behind the code in both
  directions (missing implementations in A2–A4, unlisted endpoints
  in C3).

---

## (D) Needs-user-input — only the user can supply these (names only, no values)

| # | What | Why it blocks | Where it is read |
|---|---|---|---|
| D1 | Repo URL (where Parlay should live on GitHub) | Nothing can be pushed until this exists | — |
| D2 | `BOT_TOKEN` | Core cannot start / no Telegram surface at all | core-cpp |
| D3 | `API_ID` / `API_HASH` | Bot API + TDLib + bridge auth | core-cpp, vc-ts, vc-rust |
| D4 | Assistant session — `ASSISTANT_SESSION_FILE` (Telethon session) or `TD_SESSION_DIR` + `TD_PHONE`/`TD_CODE`/`TD_PASSWORD` (TDLib first auth) | The assistant/userbot holds the VC session (DEEP_READ headline); without it no voice join is possible by design | core-cpp voice bridge / TDLib client |
| D5 | `OWNER_ID` / `SUDO_IDS` | Owner/sudo permission model has no owner configured | core-cpp |
| D6 | `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` | Spotify metadata/collections/search; 2 spotify-rust live tests are no-ops without them | spotify-rust, downloaders, vc-ts |
| D7 | `LEMONIC_API_KEY` | Optional — Lemonic works unauthenticated first; key only adds a Bearer header | vc-ts |
| D8 | Deezer ARL cookie | Full Deezer tracks (previews only without it) | downloaders (not yet read — see B2) |
| D9 | NetEase `MUSIC_U` login cookie | Direct NetEase URLs (mirror fallback works without it) | netease-go (not yet read — see B3) |
| D10 | Navidrome `NAVIDROME_URL` / `NAVIDROME_USER` / `NAVIDROME_PASSWORD` | Navidrome downloads; also needs the A12 name fix first | downloaders |
| D11 | `GENIUS_ACCESS_TOKEN` / `MUSIXMATCH_API_KEY` | Genius/Musixmatch lyrics (LRCLib works without) | spotify-rust (not yet read — see B5) |
| D12 | MongoDB/Redis credentials (only if auth is enabled) | Core SCRAM auth is unimplemented (A7), so auth-enabled Mongo cannot be used even with creds | core-cpp store |
| D13 | `PARLAY_TOKEN` (shared IPC token) | Must match across all 12 services; currently each service defaults independently | all services |

---

## (E) Explicitly out of scope for now — live Telegram tests only

Per the user's instruction, live tests are aside. Not counted as
remaining work, not attempted in this audit:

- E1. Live voice join + audible audio in a real group call (any engine).
- E2. `/play` in a real group, through the core, end to end.
- E3. Fail-over drill: kill vc-go mid-play, prove vc-ts/vc-rust take over.
- E4. Live Lemonic search/download, live Spotify API calls, live NetEase
  download re-proven by an automated test (manual PHASE4 run only).
- E5. Live Deezer full-track / Soulseek / per-user Spotify watch checks
  (all need user creds from (D) anyway).

---

## Appendix — per-service build/test snapshot (re-run 2026-11-03, worker B)

| Service | Build | Tests pass / fail / skip | Warnings |
|---|---|---|---|
| core-cpp | cmake exit 0 (clean 43 s) | 47 / 0 / 0 (tdlib 7/7, store 40/40) | 8 (libstdc++ false positives) |
| vc-go | go vet+build exit 0 | 18 / 0 / 0 (incl. live Mongo round-trip) | 0 |
| vc-ts | n/a (stdlib Node) | 24 / 0 / 0 | 0 |
| vc-rust | cargo test --release | 17 / 0 / 0 | 0 |
| audio-fx-rust | cargo build exit 0 | 0 tests defined | 0 |
| share-rust | cargo build exit 0 | 0 tests defined | 0 |
| queue-rust | cargo build exit 0 | 0 tests defined | 0 |
| spotify-rust | cargo test exit 0 | 8 / 0 / 0 (2 are creds-gated no-ops) | 0 |
| gui-js | n/a | 7 / 0 / 0 | 0 |
| radio-js | n/a | 7 / 0 / 0 (probe 5/6 stations, KEXP down) | 0 |
| downloaders | n/a (.venv Python) | 13 / 0 / 8 skipped (live-gated) | 0 |
| netease-go | go vet+build exit 0 | 5 / 0 / 0 (crypto only) | 0 |
| **Total** | **0 build failures** | **146 / 0 / 8** | **8** |

Toolchains are not on default PATH: Go `~/workspace/parlay/.tools/go/bin/go`
(1.23.0), cargo `~/.cargo/bin/cargo` (1.99.1). Worker B started the Phase 4
mongod/redis for verification and stopped them afterwards; no test
processes left running.

---

## Phase 5 follow-up (Worker E, 2026-11-03) — A5/A6/C1/C3/C4, B6/B7/B9/B10

### Closed by Worker E
- **A5** — compose is now the single source of truth: explicit `PORT`
  per service (audio-fx 8004, share 8005, queue 8006, downloaders 8009
  override code defaults 8003/8004/8005/8010); core `*_URL` vars match;
  `PARLAY_DATA_DIR=/data/<service>` per service (vc-ts and radio-js both
  write `state.json`, so no shared `/data`); dead `DATABASE_URL` removed
  (A11); `NAVIDROME_PASSWORD` everywhere (A12).
- **A6** — all 10 missing Dockerfiles added (vc-ts/radio-js updated with
  curl + HEALTHCHECK). Healthchecks on every HTTP service except core
  (no HTTP server, A2); core `depends_on` with `service_healthy` for
  mongo/redis/vc-go/downloaders. slskd service added (profile full).
  Static validation: 222 checks pass
  (`phase5/worker-e-compose-check.py`). **Compose-up still unverified**
  — Docker is not installed in the sandbox.
- **C1 (partial)** — audio-fx-rust 6/6, share-rust 5/5, queue-rust 8/8
  tests added (were 0 each). Downloaders 26 run / 18 pass + 8 skipped
  (+5 new collection tests). Netease-go 11/11 (+6 new server tests).
- **C3** — root README, ARCHITECTURE.md, PORT_MAP.md §1–§3, core-cpp
  README + PROGRESS.md, vc-rust README tail + PROGRESS.md, netease-go
  README, PHASE4-TDLIB.md §5, gui-js README, downloaders README fixed
  (see `phase5/worker-e-compose.md` §4 for the list).
- **C4** — `.env.example` rewritten: 64/64 vars read in code covered,
  0 missing; `VC_ENGINE` + `NAVIDROME_PASS` removed; `YT_DLP` unified
  (vc-rust code changed `YTDLP_BIN` → `YT_DLP`).

### Scope decisions (explicit, do not silently re-open)
- **B6 — Beetube movie/anime/torrent: MUSIC-ONLY.** Not ported. The
  providers depend on Beetube's external API backend + torrent clients;
  no quick real implementation exists and an `engine_unavailable` stub
  does not count. Recorded in ARCHITECTURE.md, PORT_MAP.md, downloaders
  README.
- **B7 — `netease.recognize`: DROPPED from scope.** Needs an external
  fingerprint backend + login; no contract action exists. Recorded in
  ARCHITECTURE.md, netease-go README + `main.go`.
- **B9 — DONE:** `POST /v1/spotify/collection` implemented in
  downloaders (contract shape `{api, kind, name, tracks, truncated}`,
  metadata only, capped at 50; 5 offline tests).
- **B10 — DONE:** `POST /v1/search` implemented in netease-go
  (`source` must be netease/all/absent, else 400; 6 offline tests).
- **B2 — DONE (Worker D, verified by Worker E):** Deezer ARL wired —
  `DEEZER_ARL` + `DEEZER_FORMAT` (default MP3_320) read in
  `services/downloaders/src/deezer.py`; previews without ARL or on ARL
  failure; needs pycryptodomex for the ARL path. Tests
  `tests/test_deezer_arl.py` 8/8. Soulseek client reads all 8 `SLSKD_*`
  vars (`SLSKD_URL`/`SLSKD_HOST` default `http://slskd:5030`,
  `SLSKD_API_KEY`, `SLSKD_API_TIMEOUT_SECS` 30,
  `SLSKD_SEARCH_MIN_INTERVAL_SECS` 10, `SLSKD_DOWNLOAD_DIR` default
  `$PARLAY_DL_DIR/.slskd-downloads`, `SLSKD_STALL_SECS` 120,
  `SLSKD_DOWNLOAD_TIMEOUT` 900, `SLSKD_MAX_PEER_ATTEMPTS` 3,
  `SLSKD_SLOW_MIN_MBPS` 0); tests `tests/test_soulseek.py` 16/16.
  Downloaders suite now 50 run / 42 pass + 8 live-skipped, 0 fail.
- **B3 — DONE (Worker E, scope update):** `NETEASE_MUSIC_U` login
  wired in netease-go — `Client.musicU` + `base`, `setAuthHeaders()`
  sends `Cookie: MUSIC_U=<value>` only to NetEase hosts (never the
  mirror); eapi/weapi/linuxapi/downloads all covered; `eapiCookie()`
  swaps `MUSIC_U` in for anonymous `MUSIC_A`. 9 offline tests
  (`internal/netease/client_test.go`); netease-go suite 20/20. Only
  the user's cookie is missing.
- **B5 — DONE (Worker E, scope update):** Genius + Musixmatch lyrics
  wired in spotify-rust against the PUBLIC APIs (Genius
  `api.genius.com/search` + song-page `data-lyrics-container` scrape;
  Musixmatch `matcher.lyrics.get`; restricted/instrumental → None).
  Chain Genius → Musixmatch → LRCLib in `fetch_lyrics()`, wired into
  `decide_skip` step 3. 9 offline tests; spotify-rust suite 40/40.
  Only the keys are missing.
- **C4 (refresh) — DONE:** `.env.example` re-verified after the scope
  update: **77 vars read in code, 77/77 covered, 0 missing** (79
  listed; the only 2 extras are LEGACY `VOICE_BRIDGE` /
  `ASSISTANT_SESSION_FILE`). Added all 8 `SLSKD_*` vars,
  `DEEZER_FORMAT`, `GENIUS_ACCESS_TOKEN`, `MUSIXMATCH_API_KEY`;
  "(not yet read)" markers removed from `DEEZER_ARL`,
  `NETEASE_MUSIC_U`, `SLSKD_URL`, `SLSKD_API_KEY`.
- **C1 (final counts) —** audio-fx 6/6, share 5/5, queue 8/8,
  downloaders 50 run / 42 pass + 8 skipped, netease-go 20/20,
  spotify-rust 40/40, vc-rust 30/30, core-cpp build exit 0.

### Opened / handed over by Worker E (not closed)
- vc-rust not compiling (reported in the first pass) is now FIXED by
  the bridge-kill worker: `cargo check` clean, tests 30/30.
  (Supersedes the vc-rust item below.)
- `ipc.json` version still `0.1.0-scaffold` (C5, Worker B owns it).
- Compose-up on a Docker host still never run (see A6 above).

## Appendix — grep counts (worker A)

218 grep lines over `services/` (excluding refs/.venv/node_modules/build/
target/phase4/tdlib-src): stub 129, `engine_unavailable` 64, placeholder
10, "not implemented" 10, passthrough 5, dummy 4, NotImplemented 1,
"not supported" 1, XXX 1 (false positive: mkstemp template
`core-cpp/src/http.cpp:44`). TODO 0, FIXME 0, HACK 0, mock 0,
unimplemented 0. Split: doc-only 113, test files 51, non-test non-doc
code 59 — of which 34 are the legitimate `engine_unavailable` error
code, 6 are degraded placeholders (A9), and the rest are comments,
test-only code, or false positives.
