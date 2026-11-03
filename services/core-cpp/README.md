# core-cpp — Parlay bot core / router (C++20)

Base: anonx-cpp (dispatcher, TDLib client, ntgcalls client, FFmpeg pipeline,
thread pool, rate limiter) + NoxBeats (command/permission surface: play,
queue, playlist, settings, sudo, broadcast, chat settings, autoplay) +
TinaMusic (native MTProto/WebRTC voice path, low-RAM design).

Rules: route, don't do. No yt-dlp, no FFmpeg calls, no Spotify client here —
except the in-process voice path (below), which is exactly the pipeline the
refs use (yt-dlp → ffmpeg → Opus → ntgcalls/WebRTC). Every command validates
permissions, emits one IPC call (`contracts/ipc.json`), and formats the
result. Engine fallback: on `engine_unavailable`/timeout, retry the next
engine in the chat's list.

## Build

```sh
cmake -S . -B build && cmake --build build
```

No external libraries are linked: the sandbox has no libcurl dev headers,
no nlohmann/json and no libmongoc. JSON is hand-rolled (`src/json.*`), HTTP
goes through the system `curl` CLI via popen (`src/http.*`), and the
GUI-facing HTTP server is std-only POSIX sockets on its own thread
(`src/http_server.*`, `src/core_api.*`). Redis speaks
RESP over a raw socket and MongoDB speaks the OP_MSG wire protocol
(`src/store.cpp`) — both hand-rolled, both tested against real servers.
ntgcalls is dlopen'd at runtime when present (`src/voice_call.cpp`).
Nothing is downloaded at build time. Requires g++ ≥ 13 / clang with C++20
and cmake ≥ 3.25.

Measured 2026-11-02 (sandbox, 4 cores, 7 GB RAM): configure 0.3 s, clean
build `-j4` 35.1 s, incremental no-op 0.13 s, one-file rebuild
(`dispatcher.cpp`) 14.4 s. Binary 1,002,808 bytes, stripped 866,656 bytes.
0 errors, 8 warnings — all `-Wmaybe-uninitialized` false positives inside
libstdc++ `std::variant`/`std::map` internals when copying `parlay::Json`
(GCC 13, `-O2`); no warnings in our own code. See
`phase4/bench-core-cpp.md`.

## Run

Config is environment names only — no secret values in files:

```
BOT_TOKEN, API_ID, API_HASH, MONGO_URI, REDIS_URL, PARLAY_TOKEN,
VC_GO_URL, VC_TS_URL, VC_RUST_URL, FX_URL, SHARE_URL, QUEUE_URL,
DL_URL, RADIO_URL, PORT, OWNER_ID, SUDO_IDS, LOGGER_ID,
PARLAY_ALLOW_CHATS, PARLAY_BLOCK_CHATS,
ASSISTANT_IDS, NTGCALLS_LIB, VOICE_RTP_SINK, TD_API_ID, TD_API_HASH,
TD_SESSION_DIR, TDJSON_LIB, TD_PHONE, TD_CODE, TD_PASSWORD, TD_EMAIL,
TD_EMAIL_CODE, TD_FIRST_NAME, TD_LAST_NAME, TD_QR,
PARLAY_DATA_DIR, PARLAY_LOG_FILE, TELEGRAM_API_BASE,
DURATION_LIMIT, QUEUE_LIMIT, SUPPORT_CHAT
```

Defaults match `docker-compose.yml` (vc-go :8001, vc-ts :8002, vc-rust
:8003, audio-fx-rust :8004, share-rust :8005, queue-rust :8006,
spotify-rust :8007, radio-js :8008, downloaders :8009, netease-go
:8010). `BOT_TOKEN` gates Telegram polling
only: with no `BOT_TOKEN` the core still starts and serves its HTTP API
(HTTP-only mode, no polling). With no `PARLAY_TOKEN` it warns and engine
calls go out unauthenticated (engines that require a token refuse
honestly). `API_ID`/`API_HASH` are the fallback for `TD_API_ID`/`TD_API_HASH`
(TDLib voice backend); the Telethon voice bridge that used to need
them was deleted in Phase 5. Exit code 42 from `/restart` is the
supervisor-restart signal.

## Implemented (real)

- **Telegram Bot API client** (`src/telegram_api.*`) — HTTPS long-poll
  `getUpdates` (30 s, exponential back-off 1 s → 30 s), `sendMessage`
  (returns message id), `getChatMember`, `getChatAdministrators`,
  `getMe`, `deleteMessage`, `leaveChat`, `forwardMessage`, `copyMessage`,
  `exportChatInviteLink`. Plain-text replies on purpose (track titles
  must never break formatting). Base URL from `TELEGRAM_API_BASE`.
- **HTTP API** (`src/http_server.*`, `src/core_api.*`, Phase 6 A2) — the
  core's GUI-facing surface on `PORT` (default 8080), std-only POSIX
  sockets on its own thread so the Telegram long-poll loop is never
  blocked. `GET /healthz` → `{"ok":true}`;
  `GET /api/now-playing?chat_id=<id>` → the chat engine's live
  now-playing object (via `Dispatcher::apiNowPlaying`, same vcGet
  fail-over as the Telegram commands); `GET /api/queue?chat_id=<id>` →
  the live queue (now-playing's `queue` field, falling back to the
  engine's queue list when now-playing reports no state but the list
  is non-empty). No state / no engine reachable →
  `404 {"ok":false,"error":"no playback state"}` — never demo/fake
  data. Flat JSON, not the IPC envelope (see `contracts/README.md`).
  `POST /v1/join` is the exception: vc.join (contracts/ipc.json) for
  the vc-ts / vc-rust engines, which hold no MTProto client — the
  core's TDLib backend owns the join (authorize → createOffer →
  getChat/createVideoChat → joinVideoChat → ntg.connect), IPC
  envelope throughout, `X-Parlay-Token` required when a token is
  configured, `502 engine_unavailable` + the exact reason when the
  backend cannot join. `POST /v1/mute` is the same shape for vc.mute
  (`{"chat_id","muted"}` → `{"muted","chat_id","now_playing"}`,
  "muted" defaults to true): the core backend mutes/unmutes only
  sessions it holds; engine-owned sessions and nothing-playing fail
  honestly with 502. The server starts before any token check: with no `BOT_TOKEN` the
  core runs HTTP-only and the API can still proxy real engine state.
- **Command dispatcher** (`src/dispatcher.*`, 3,199 lines) — the full
  NoxBeats inventory from `Bad/cmd/register.hpp`, with the same route
  shape (Scope Any/Group/Private, Perm Everyone/Sudo/Owner), the same
  alias map, and the full `c*` channel-command map (34 commands,
  `via_channel=true`). Playback: `play/playforce/vplay/vplayforce`
  (`-f` force, `-v` video), `playplaylist`, `pause/resume/skip/stop/end`,
  `loop [n|off]`, `seek/seekback` (min 10 s), `queue/playing/player`,
  `clear/restore/remove/move/shuffle/pos/jump`, `speed` (0.5–2.0),
  `bass` (0–100), `mute/unmute`, `replay`. Queue-ops rebuild the vc
  queue through real IPC calls (list → rebuild), never a stub reply.
  Playlists: `addplaylist/playlist/rmplaylist/clearplaylist` (max
  100/user, stored in Store). Settings: `playmode`, `settings`,
  `lang/language`, `adminmode`, `cmddelete`, `cleanmode` (1–1440 min,
  background sweeper deletes bot + command messages), `thumbnail/nothumb`,
  `autoplay [on|off|lang|mood]` (background watcher queues a related
  track when a session drains). Channel-play: `channelplay`, `userbot`.
  `/autoplay-skip` / `/askip` (Phase 6): forces an autoplay mix pick now
  (`vc.autoplay_skip`, all three engines; 400 when autoplay is off).
- **`/mute` + replied-media `/play` (Phase 5, were limits)** —
  `/mute`/`/unmute` call the real voice backend on core-owned sessions
  (`vc.mute`, `VoiceCall::mute()`/`unmute()` → TDLib/ntgcalls; a backend
  failure replies honestly and leaves the flag untouched); engine-owned
  sessions honestly refuse. `/play` auto-takes the `file_id` from
  `reply_to_message` (audio/document/voice/video_note), downloads via
  Bot API `getFile` (20 MB cap, traversal rejected) and plays through
  the core voice backend; no media + no query keeps the usage error.
  Admin: `auth/unauth/authlist`, `admincache` (600 s per-user cooldown),
  `blacklist/unblacklist/whitelist`, `blacklisted`. Sudo:
  `addsudo/delsudo/listsudo`, `ac/activevc`, `broadcast` (reply-required,
  `-copy/-nochat/-user`, 200 ms spacing, failures listed inline),
  `maintenance`, `autoleave`, `json`, `speedtest`, `bug`, `privacy`,
  `logs`, `logger`, `restart`. Owner: `eval` (real `sh -c`, 60 s cap,
  last 3,000 chars). RTMP: `setrtmp` (Private), `stream/streamstop/
  streamstatus` (ffmpeg fork/exec, one child per chat, key scrubbed).
  Tools: `search` (vc-ts, `ne:` prefix → NetEase), `download` (Spotify
  links → downloaders, else YouTube), `lyrics` (NetEase), `radio`
  (list/start, stream URL fed to `vc.play`), `share`, `engine`
  (durable per-chat choice), `alive/ping/stats` (latency, uptime, CPU
  from `/proc/stat`, RAM, disk, RSS, voice backend). Service messages:
  bot added to a non-supergroup → leaves; video-chat start/end →
  session stop (auto-end).
- **Permission model** (`cmd/common.cpp` in NoxBeats, ported) — owner
  bypasses everything; sudo = owner or Store `sudoers` set; admin =
  cached DB admin list (600 s TTL) then live `getChatMember`;
  `admin_mode` 0 = admins+auth (default), 1 = admins only, 2 = everyone;
  `play_mode` admin-only gate on `/play`; blacklisted users blocked
  (except `start`/`listsudo`), blacklisted chat → bot leaves;
  maintenance → sudo only; per-chat rate limit (1 cmd/s).
- **State store** (`src/store.*`) — **real** clients, no driver needed:
  `RedisClient` (hand-rolled RESP over TCP, GET/SET/SETEX/DEL/SADD/
  SREM/SMEMBERS/HSET/HDEL/HGET/HGETALL/KEYS), `MongoClient` (hand-rolled
  OP_MSG wire protocol + minimal BSON, collection `state`, documents
  `{_id, v}`, find/upsert/delete/keys-with-prefix/ping). `Store` picks
  Mongo > Redis > JSON file (`PARLAY_DATA_DIR/state.json`, atomic
  rename) and exposes kv/set/hash + TTL cache. Tested against real
  mongod 8.0.4 and real redis-server (see below). Authenticated Mongo
  URIs are rejected honestly — SCRAM auth is not implemented; run
  mongod without auth or with auth disabled for local dev.
- **Per-chat engine selection + fail-over** (`src/engine_router.*`) —
  durable in Store key `engine:<chat_id>` (in-memory map is only a
  cache), `/engine` to switch, fallback order go→ts→rust (rotated per
  preference). A failed preferred engine is retried on the next healthy
  engine and the reply says which engine won.
- **Voice** (`src/voice_call.*`, `src/tdlib_client.*`) — the refs'
  pipeline, not a stub, and **in-process only since Phase 5**: the
  voice bridge (`voice_bridge.py`, Telethon child process) was deleted
  on the user's order — ntgcalls is a C++ native library linked into
  the bot process, not a server. Who plays: the **assistant/userbot
  account**, never the bot account (NoxBeats `ensure_assistant`,
  TinaMusic `Play`). `VoiceCall` interface with two real backends,
  picked by `makeVoiceCall(cfg)`: `TdLibVoiceCall` (TDLib 1.8.69
  signaling — authorize → `getChat`/`createVideoChat` →
  `joinVideoChat` — + dlopen'd `libntgcalls.so` for WebRTC; needs
  `TD_API_ID`/`TD_API_HASH`/`TD_SESSION_DIR` + both `.so` files) →
  `NtgCallsVoiceCall` (dlopen'd `libntgcalls.so`, C API from
  `ntgcalls.h`; builds the local SDP offer and attaches sources, but
  cannot join without an MTProto session — `join()`/`play()` say so
  honestly with the exact reason)
  → `UnavailableVoiceCall` (reports the exact reason, never fakes
  success). `OpusPipeline`: yt-dlp `-g` resolve → ffmpeg
  `-c:a libopus -b:a 128k -ar 48000 -ac 2` → Ogg Opus file (verified
  by ffprobe packet count + duration) + optional second RTP output to
  a UDP sink (best-effort; a blocked sink cannot lose the file).
  `buildFfmpegCmd` is the TinaMusic/NoxBeats shape (reconnect flags,
  `-ss`, `atempo`, lowpass bass boost, `s16le -ac 2 -ar 48000 pipe:1`).
  ntgcalls C build: **DONE 2026-11-02** (`phase5/ntg-build.log`,
  `BUILD_EXIT=0`, `refs/ntgcalls/shared-output/lib/libntgcalls.so`,
  98 MB, 77 exported `ntg_*` symbols, ldd clean). The 3.0.5 C API
  turned out to be SYNCHRONOUS, not the old async API the stale
  header described — `ntg_instance_create()` / `ntg_create_call()` /
  `ntg_connect()` return `ntg_result` directly, errors via
  `ntg_last_error()`; all three bindings were rewritten to it.
  `NtgCallsVoiceCall` loads the built `.so` in-process (version
  3.0.5) and `ntg_create_call` returns a real local SDP offer
  (476 bytes, ufrag + sha-256 fingerprint) — proven by test 8 in
  `tests/tdlib_test.cpp` (44/44 PASS). `libpulse-dev` 16.1 installed
  via apt afterwards (vendored headers replaced by the package).
- **Group management** — `getChatMember` admin check gates playback
  control and `/engine` in groups; private chats are open.
  `PARLAY_ALLOW_CHATS` / `PARLAY_BLOCK_CHATS` (comma-separated chat ids)
  and `OWNER_ID` from env. Owner bypasses the admin check.
- **Oversize rule** — `download.youtube` returning `oversize=true` is
  reported as queued for a later MTProto pass, not a failure (same rule
  as the 50 MB Bot API cap in `contracts/ipc.json`).
- **Hand-rolled JSON** (`src/json.*`) — full parser (incl. `\u` /
  surrogate pairs) + serializer, covering Bot API and IPC payloads.
- **Background threads** — clean-mode sweeper (deletes bot + command
  messages after N minutes) and autoplay watcher (queues a related track
  via vc-ts search when a session drains), both every 5 s.

## Tests (all run 2026-11-02, sandbox)

- `tests/store_test.cpp` — **ALL PASS** (BSON round-trip; Mongo wire
  protocol vs real mongod 8.0.4 on 127.0.0.1:27017: connect/ping/upsert/
  find/keys/delete; Redis RESP vs real redis-server on 127.0.0.1:6399:
  ping/set/get/sadd/smembers/hset/hget/hgetall/keys/del; Store over
  Mongo: kv/set/hash/keys/cache; Store over file + persistence across
  instances). Rebuild + run:
  `g++ -std=c++20 -I src tests/store_test.cpp src/store.cpp src/scram.cpp
  src/json.cpp src/config.cpp -o /tmp/store_test && /tmp/store_test
  "mongodb://127.0.0.1:27017/parlay_test" "redis://127.0.0.1:6399/0"`.
- `tests/scram_test.cpp` — **67/67 PASS** (Phase 7, A7): SCRAM-SHA-1/256
  auth in the hand-rolled Mongo client vs a REAL auth-enabled mongod
  8.0.4 on 127.0.0.1:27098 (dummy users only). Crypto primitives vs
  RFC vectors (SHA-1/SHA-256/HMAC/PBKDF2 incl. RFC 6070, MD5, base64);
  SCRAM message construction vs the RFC 5802 (SHA-1) and RFC 7677
  (SHA-256) worked examples; URI parsing (credentials, percent-decoding,
  authSource default, authMechanism, rejections); live SHA-256 connect +
  CRUD, forced `?authMechanism=SCRAM-SHA-256`, SHA-1-only user via
  automatic negotiation + forced `SCRAM-SHA-1`, wrong-password honest
  failure (`Authentication failed`), `?authSource=admin`, Store over
  authenticated Mongo, and no-auth URI still working (plain mongod
  :27097). Rebuild + run:
  `g++ -std=c++20 -I src tests/scram_test.cpp src/store.cpp src/scram.cpp
  src/json.cpp src/config.cpp -o /tmp/scram_test && /tmp/scram_test`
  (defaults point at the scratch mongods; live sections SKIP when they
  are not running). Setup: `phase7/mongo_setup_users.cpp` + the commands
  in `phase7/worker-a7-scram.md`.
- `tests/router_test.cpp` — **20/20 PASS** (fail-over on 5xx / transport
  errors / `engine_unavailable` / 404-for-known-action).
  `tests/dispatcher_test.cpp` — **81/81 PASS** (47 Phase 5 + 6 Phase 6
  `/autoplay-skip`/`/askip` + 10 Phase 6 `/broadcast`
  `errors.txt` + 13 Phase 6 Worker C `/speed`/`/bass` core-voice
  filter path + 5 `/speedtest` helpers): reply-media parsing, replied-media
  `/play`, mute/unmute core + engine sessions, join, autoplay-skip
  success/alias/nothing-playing/engine-400, broadcast attachment /
  all-delivered / upload-failure fallback / no-reply, speed/bass
  core re-attach at the stored position / no-source honest reply /
  engine saved-not-applied reply / stored values into replied-file
  plays, speedMbps + formatSpeedtest. Rebuild + run:
  `g++ -std=c++20 -I src tests/dispatcher_test.cpp src/dispatcher.cpp
  src/telegram_api.cpp src/ipc.cpp src/engine_router.cpp src/store.cpp
  src/scram.cpp src/json.cpp src/config.cpp src/http.cpp src/voice_call.cpp
  src/tdlib_client.cpp -o /tmp/dispatcher_test -ldl && /tmp/dispatcher_test`.
- Opus pipeline proof (`/tmp/opus_proof.cpp`, `/tmp/rtp_recv.py`): 5 s
  440 Hz tone → **251 Opus frames, 83,736 bytes, 5.0065 s** (ffprobe).
  RTP packetization proven to a file: 85,973 bytes, SDP
  `m=audio 0 RTP/AVP 97`, `a=rtpmap:97 opus/48000/2`, first bytes
  `80 c8` (RTCP Sender Report, version 2). UDP to a live receiver is
  **blocked by the sandbox** (EPERM on `sendto`, even to loopback) —
  the RTP output reports `rtp_ok=0` honestly and the file encode still
  succeeds.
- Voice-backend proof (`/tmp/voice_proof.cpp`): with no session and no
  `.so`, factory selects `unavailable` with the exact reason;
  `play` returns false, no secrets in errors; ffmpeg command
  shape verified (reconnect flags, s16le 48k stereo, atempo, `-ss`).
- Bridge error path: **deleted in Phase 5** — `voice_bridge.py` and the
  Telethon child are gone (user order: ntgcalls is a native library
  linked in-process). The paragraph below described the old route; it
  is kept only as history. Join now goes through the TDLib backend
  (`TD_SESSION_DIR` + `TDJSON_LIB`).
- **HTTP API** (`tests/http_api_test.cpp`, Phase 6 A2; vc.join in
  Phase 7, A1; vc.mute in Phase 7 residuals) — **71/71 PASS**:
  CoreApi routing with a scripted provider (healthz, now-playing /
  queue success + honest 404/400/405, queue-list fallback); POST
  /v1/join routing (200 joined:true + now_playing, 502 with the exact
  backend reason, 400 bad bodies, 405 non-POST, 401 token gate) and
  the full stack over a real Dispatcher with an unavailable voice
  backend (502, exact reason); POST /v1/mute routing (200 muted +
  now_playing, unmute + muted-default-true, 502 with the exact
  reason, 400 bad bodies incl. non-boolean "muted", 405 non-POST,
  401 token gate) and the full stack over a real Dispatcher
  (nothing playing -> 502 "Nothing is playing.", engine-owned
  session -> honest refusal); the server
  over a real socket on an ephemeral port (curl-CLI client); the
  Dispatcher proxying a stub VC engine on 127.0.0.1 (canned IPC
  envelopes) end to end; plus the Worker C stop-race check (25
  start/serve/stop cycles). Rebuild + run: the `g++` line in the test
  file's header comment.
- **Telegram API** (`tests/telegram_api_test.cpp`, Phase 6 Worker C) —
  **24/24 PASS** against a local stub server: `sendDocument`
  multipart body (chat_id/caption fields, `errors.txt` part, exact
  file bytes, boundary), hostile-filename sanitizing, ok:false and
  HTTP 500 honest failures, `postRaw` binary body byte-exact
  (the `/speedtest` upload mechanism), `postJson` still
  `application/json`. Rebuild + run: the `g++` line in the test
  file's header comment.
- **Shutdown (Phase 6 Worker C):** `SIGTERM` used to abort the core
  (exit 134, glibc `FD_SETSIZE` — `HttpServer::stop()` closed the
  listen socket and stored -1 while the accept thread was between
  `select()` and `FD_ISSET()`). Fixed by snapshotting the fd per
  iteration + an atomic exchange in `stop()`; verified old binary
  8/8 aborts vs fixed binary 8/8 clean exits 0 with
  `parlay-core: shutting down.` Teardown takes ~2 s (TDLib close
  pump); give the process a 5 s SIGTERM grace before SIGKILL.
- Binary smoke: no env → HTTP API on `:8080` starts and answers
  `/healthz`; Telegram polling stays off
  (`missing env vars: BOT_TOKEN PARLAY_TOKEN`) until a token is set.
  Live smoke 2026-11-03 (no tokens): core `:18080` + real vc-go
  `:18001` — `/healthz` 200, no-state `/api/now-playing` /
  `/api/queue` 404 `{"ok":false,"error":"no playback state"}`, and
  after a queue add to vc-go the core proxied the real track
  (`yt:smoke1`) on both endpoints.
- TDLib: build **finished 100%** 2026-11-02 (log `phase4/tdlib-build.log`,
  933 lines, 0 errors, `libtdjson.so.1.8.69`, 50 MB); the C++ client
  `src/tdlib_client.{hpp,cpp}` exists and passes 44/44 checks
  (TDLib responses + real-offer check via the built `libntgcalls.so`
  + all 17 auth-state checks: phone / QR / code / password / email /
  email code / registration / other-device / premium / unknown, +
  Phase 7 A1: the join flow against a scripted TDLib client — real
  offer carried as the joinVideoChat payload, exact request shapes,
  real ntg.connect wiring — and the TD_SESSION_DIR default).
  ntgcalls wheel 3.0.5 has **no C `ntg_*` symbols** (`nm -D` shows only
  `PyInit_ntgcalls`), so the C++ dlopen path uses the C build from
  `refs/ntgcalls` instead (see the Voice bullet above); without
  `TD_*` env the TDLib backend still reports `available()=false`
  honestly and no live join is possible.

## Honest limits (not stubs — reported as limits)

- **Speed/bass filters are applied only on core-voice sessions**:
  `/speed` and `/bass` re-attach the core backend's source with the
  filter (ffmpeg `atempo` / lowpass) at the current position, and
  stored values apply to later core plays. On vc-engine sessions
  they seek-restart at the same position and store the value only —
  vc-go/vc-ts/vc-rust expose no speed/pitch/bass control over IPC
  or in their playback paths, so applying it there needs a new
  audio-filter subsystem in the engines. The engine reply says the
  value was only saved instead of claiming it was set.
- **`/lang` is English-only**: non-`en` is not stored (no locale files).
- **`/broadcast`** sends failed ids as an `errors.txt` attachment
  (real Bot API `sendDocument`, Phase 6 Worker C); inline list only
  if the upload itself fails.
- **`/speedtest`** measures download (25 MB) + upload (4 MB POST to
  Cloudflare `__up`, 30 s timeout, Phase 6 Worker C); a failed upload probe is
  reported honestly next to the download number. Note: from this
  sandbox the upload probe times out (the sandbox egress stalls
  POSTs — verified: 100 KB POSTs to `speed.cloudflare.com/__up` and
  to httpbin.org both time out while GETs succeed), so a sandbox run
  reports the download number plus an honest upload failure — the
  probe needs a host with open egress to show an upload number.
- **`/eval`** is a real owner shell (`sh -c`, 60 s) — owner-only, as in
  NoxBeats.
- **Mongo SCRAM auth** is not implemented; the wire client rejects
  authenticated URIs with an exact message.
- **Voice joining needs a real assistant session + network to Telegram
  DCs**: the sandbox has no direct TCP route to Telegram (proxy-only
  egress), so the TDLib client cannot authorize here. Every step up to
  the network call is exercised; live join is a live-test-time check.

## Not yet implemented

- **File delivery for `/download`** — the core reports the downloader's
  `file_path`; sending the audio file into chat (Bot API `sendAudio` /
  MTProto for oversize) is a follow-up.
