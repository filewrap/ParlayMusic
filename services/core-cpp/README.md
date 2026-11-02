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
goes through the system `curl` CLI via popen (`src/http.*`), Redis speaks
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
ASSISTANT_IDS, ASSISTANT_SESSION_FILE, NTGCALLS_LIB, VOICE_BRIDGE,
VOICE_RTP_SINK, PARLAY_DATA_DIR, PARLAY_LOG_FILE, TELEGRAM_API_BASE,
DURATION_LIMIT, QUEUE_LIMIT, SUPPORT_CHAT
```

Defaults match `docker-compose.yml` (vc-go :8001, vc-ts :8002, vc-rust
:8003, audio-fx-rust :8004, share-rust :8005, queue-rust :8006,
radio-js :8008, downloaders :8009). `BOT_TOKEN` and `PARLAY_TOKEN` are
required; the process exits with a clear message if either is missing.
`API_ID`/`API_HASH` are required only by the voice bridge (assistant
session). Exit code 42 from `/restart` is the supervisor-restart signal.

## Implemented (real)

- **Telegram Bot API client** (`src/telegram_api.*`) — HTTPS long-poll
  `getUpdates` (30 s, exponential back-off 1 s → 30 s), `sendMessage`
  (returns message id), `getChatMember`, `getChatAdministrators`,
  `getMe`, `deleteMessage`, `leaveChat`, `forwardMessage`, `copyMessage`,
  `exportChatInviteLink`. Plain-text replies on purpose (track titles
  must never break formatting). Base URL from `TELEGRAM_API_BASE`.
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
- **Voice** (`src/voice_call.*`, `voice_bridge.py`) — the refs' pipeline,
  not a stub. Who plays: the **assistant/userbot account**, never the bot
  account (NoxBeats `ensure_assistant`, TinaMusic `Play`). `VoiceCall`
  interface with three backends, picked by `makeVoiceCall(cfg)`:
  `VoiceBridge` (assistant session configured) → `NtgCallsVoiceCall`
  (dlopen'd `libntgcalls.so`) → `UnavailableVoiceCall` (reports the exact
  reason, never fakes success). `OpusPipeline`: yt-dlp `-g` resolve →
  ffmpeg `-c:a libopus -b:a 128k -ar 48000 -ac 2` → Ogg Opus file
  (verified by ffprobe packet count + duration) + optional second RTP
  output to a UDP sink (best-effort; a blocked sink cannot lose the
  file). `buildFfmpegCmd` is the TinaMusic/NoxBeats shape (reconnect
  flags, `-ss`, `atempo`, lowpass bass boost, `s16le -ac 2 -ar 48000
  pipe:1`). `voice_bridge.py` (Telethon + ntgcalls, TinaMusic `Play`
  steps): `create_call` → offer → `set_stream_sources` → `get_call`
  (creates the group call if none) → `JoinGroupCallRequest` with
  `DataJSON(offer)` → `find_connection` from `UpdateGroupCallConnection`
  → `ntg.connect(remote)`; `join-chat` joins via `@channel` or invite
  hash. Errors are JSON (`{"ok": false, "error": "<type>"}`), never a
  traceback, never session material.
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
  `g++ -std=c++20 -I src tests/store_test.cpp src/store.cpp src/json.cpp
  src/config.cpp -o /tmp/store_test && /tmp/store_test
  "mongodb://127.0.0.1:27017/parlay_test" "redis://127.0.0.1:6399/0"`.
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
  `play`/`joinChat` return false, no secrets in errors; ffmpeg command
  shape verified (reconnect flags, s16le 48k stereo, atempo, `-ss`).
- Bridge error path: `voice_bridge.py join-call` / `join-chat` with a
  missing session file → exit 1, `{"ok": false, "error":
  "FileNotFoundError"}` (no traceback).
- Binary smoke: no env → exit 1 in 0.00 s,
  `missing required env vars: BOT_TOKEN PARLAY_TOKEN`.
- TDLib: clone + configure pass (needed gperf 3.1, installed via
  azure-only apt — the cogentco mirror was too slow through the proxy);
  full build running, log `phase4/tdlib-build.log`. ntgcalls wheel
  3.0.5 has **no C `ntg_*` symbols** (`nm -D` shows only
  `PyInit_ntgcalls`), so the C++ dlopen path needs a real
  `libntgcalls.so` build; the Python bridge path (Telethon + ntgcalls
  3.0.5, proven offline: `create_call` returns a 476-char offer) is
  the working voice route until then.

## Honest limits (not stubs — reported as limits)

- **Mute is flag-only**: no vc mute action exists in the IPC contract,
  so `/mute`/`/unmute` store the per-chat flag and say so.
- **Speed/bass filters are not applied by vc services**: `/speed` and
  `/bass` seek-restart at the same position and store the value; the
  filter itself is not yet applied downstream.
- **`/play` from a replied-to media file** reports that the downloaders
  file_id hand-off is not implemented yet (link/search/reply-text
  paths work).
- **`/lang` is English-only**: non-`en` is not stored (no locale files).
- **`/broadcast`** lists failed ids inline; there is no `sendDocument`
  `errors.txt` attachment yet.
- **`/speedtest`** is download-only (Cloudflare probe); upload is
  skipped honestly.
- **`/eval`** is a real owner shell (`sh -c`, 60 s) — owner-only, as in
  NoxBeats.
- **Mongo SCRAM auth** is not implemented; the wire client rejects
  authenticated URIs with an exact message.
- **Voice joining needs a real assistant session + network to Telegram
  DCs**: the sandbox has no direct TCP route to Telegram (proxy-only
  egress), so Telethon connect fails here with a connection error. Every
  step up to the network call is exercised; live join is a
  live-test-time check.

## Not yet implemented

- **Core HTTP port (`PORT`, GUI `/api/*`)** — not served yet; the core
  currently only long-polls Telegram. The GUI contract
  (`gui.now_playing`, `gui.queue`) is a follow-up.
- **File delivery for `/download`** — the core reports the downloader's
  `file_path`; sending the audio file into chat (Bot API `sendAudio` /
  MTProto for oversize) is a follow-up.
- **TDLib C++ client** — `src/tdlib_client` does not exist yet; the
  TDLib build (see above) is the prerequisite. The voice bridge covers
  the join path meanwhile.
