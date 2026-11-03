# Parlay — Phase 7

Date: 2026-11-03. Branch: `main` (pushed, see below). Base: Phase 6 +
v0.2.0 release (`02bdcda`).

Phase 7 fixes REMAINING.md Section A completely, with real code and
zero stubs left. Scope rules (user order 2026-11-03 ~07:03 IST): the
user is live-testing right now — no interference with that (no core
start with the user's token, nothing run against the live bot,
`.env` untouched; local tests use dummy creds only). The "Mongo
aside" decision is SUPERSEDED for A7/A11 by the direct order to fix
Section A. REMAINING.md is the source of truth: it now contains
only what truly remains (A1 live proof, A6 compose-up, key-gated
items, known limitations).

## 1. Verification first (Worker V)

Every Phase 5/6 closure claim re-checked against the current tree
with rebuilds + test runs (pristine-HEAD build where the tree was
mid-edit by a sibling). Verdict: **A2, A3, A4, A5, A9, A10, A12
verified fixed; A8 fixed with residuals; ipc.json is 0.2.0; the dead
sqlite DATABASE_URL is gone.** Residuals found: no HTTP POST
`/v1/join`/`/v1/mute`; no top-level `NowPlaying.stream_url`;
vc-rust emits no `stream_url`; stale "0.1.0-scaffold" comment in
vc-go `types.go`. All four fixed by Workers A1/R (below).
Report: `phase7/worker-v-verify.md`.

## 2. A1 — live voice join: join + audio, core-owned (Workers A1, A1b)

**Join (Worker A1).** New `POST /v1/join` (`CoreApi::handleJoin`,
IPC envelope, `X-Parlay-Token` gate when configured, 502 + exact
reason on backend failure) → `Dispatcher::joinVoice`
(already-in-call shortcut, sets `voice_owner=core`) → TDLib backend
(`authorize → getChat/createVideoChat → joinVideoChat →
ntg.connect`). vc-ts/vc-rust `join()` delegate to the core over IPC
and return the core's answer verbatim (unreachable → "core
unreachable … set CORE_URL …"). No engine can claim a join it
cannot do; `NtgCallsVoiceCall::join()` fails with the exact reason
instead of silent false. Also fixed a vc-rust bug: `attempt_join`
passed `""` as the offer, zeroing the real `offer_len`.
`TD_SESSION_DIR` defaults to `<PARLAY_DATA_DIR>/tdlib`;
`TDJSON_LIB`/`NTGCALLS_LIB` resolve from `lib/` next to the binary
(release-tarball layout). Stale "no MTProto client in-process" /
"voice_bridge" wording corrected everywhere (final grep: zero
hits). Report: `phase7/worker-a1-voice.md`.

**Audio (Worker A1b — found by the coordinator's own code read).**
In `onPlay`, after the engine resolved a track, the core never
called `voice_.play()` with the resolved source and never joined —
only the replied-file path streamed audio through the core's
ntgcalls session. Fixed: new `attachEngineAudio()` — after any
engine `play` succeeds (`/play`, `/playforce`, `/vplay`,
`/playplaylist`, `/radio`, autoplay picks), the core streams
`now_playing.stream_url` (else `track.stream_url`) through its own
ntgcalls session with the chat's stored speed/bass. Attach failure
stops the engine playback, silences any stale core stream, clears
state, and replies the exact error. Ownership flips to
`voice_owner="core"` (+ `core_source/core_title/core_video/
core_track_id/core_pos/core_follow` store fields); side effect, all
correct: `/mute`, `/speed`, `/bass` now genuinely work on engine
sessions via the existing core-owned branches. Kept in step:
`/skip`/`/askip` re-attach the new track (empty queue stops the
core backend); `/pause`/`/resume` pause/resume the core backend
too; `/seek`/`/jump`/`/replay` re-attach at the new position;
`/stop` and call-end clear the fields. A 5 s watcher pass
`watchCoreFollow()` follows engine-internal auto-advance
(track-id change, or same id with position jumped backwards =
`/loop` copy). Replied files opt out (`core_follow=0`). No
local-file fallback exists — engines deliberately strip `file_path`
from IPC, and an engine-host path isn't a core-host path under
compose. Report: `phase7/worker-a1b-audio.md`.

**Still to prove live (user's test):** one `/play` with audible
audio; track 2 audible too (watcher follow); `/skip` switching
audio cleanly; `/pause`/`/seek`/`/speed`/`/bass` acting on the
audible stream; `/play` with the backend down replying the honest
error. Offline proof only shows the core attaches the
engine-resolved URL to its own session — audibility is live-only.

## 3. A6 — compose (Worker A6)

`docker compose up` is **genuinely impossible in this sandbox —
proven, not assumed**: every container start fails in runc with
`setns: operation not permitted` (plain run, `--userns=host`, full
rootless attempt with rootlesskit 3.3.0 + slirp4netns 1.3.7); the
sandbox sets `NoNewPrivs: 1` on all processes, so no OCI runtime
path can start a container here. Two other blockers found and
worked around: no `CAP_NET_ADMIN` (dockerd restarted with
`--iptables=false --bridge=none`) and dockerd's registry client
timing out through the MITM proxy (worked around with `crane` —
all 9 base images pulled and loaded, 1.9 GB). Fixed: proxy `ARG
http_proxy/https_proxy/...` blocks in all 12 Dockerfiles.
`docker compose config` is clean (base + override). Proven instead:
a native sweep with the exact compose env (dummy creds, Dockerfile
CMDs, real mongo/redis/slskd) answers `GET /healthz` 200 on all 12
services, mongo up, redis PONG, slskd UI served. Cleanup done
(dockerd stopped, 4.3 GB daemon data removed, no containers ever
created). A real Docker host closes A6 with plain `docker compose
--profile full up --build` + the healthz sweep. Report:
`phase7/worker-a6-compose.md`.

## 4. A7 — Mongo SCRAM auth (Worker A7)

SCRAM-SHA-1 and SCRAM-SHA-256 in core-cpp's hand-rolled Mongo wire
client, zero new dependencies — SHA-1/SHA-256/MD5/HMAC/PBKDF2/base64
implemented in-tree (`src/scram.{hpp,cpp}`, 678 lines).
`MongoClient::connect()` parses
`mongodb://user:pass@host:port/db?authSource=admin` (percent-decoded
userinfo, `authSource` defaulting to db then admin,
`?authMechanism=` to force a mechanism), then runs the RFC 5802/7677
handshake (`saslStart`/`saslContinue` via OP_MSG, SHA-256 first with
fallback to SHA-1, server-signature verification). Key finding:
RFC-correct SCRAM-SHA-1 with the raw password FAILS against a real
mongod — MongoDB derives SHA-1 credentials from
`hex(MD5(username + ":mongo:" + password))`; the transform is
applied only for SHA-1. Tests: new `tests/scram_test.cpp` **67/67**
(crypto vectors incl. RFC 6070 PBKDF2 and RFC 5802/7677 examples,
URI parsing, live SHA-256 + SHA-1 handshakes, wrong-password honest
failure, `authSource=admin`, Store over authenticated Mongo,
no-auth URI still works) against real mongod 8.0.4; `store_test`
**40/40** vs the auth mongod and **40/40** vs a plain mongod.
Limitations: `mongodb+srv://` and replica sets rejected; no
SASLprep; no channel binding. Report: `phase7/worker-a7-scram.md`.

## 5. A11 — shared durable store (Worker A11)

One shared store for **all 8 stateful services**, backend priority
`MONGO_URI` > `REDIS_URL` > JSON file (no new env vars). Mongo: db
from URI, collection `state`, doc `{_id:"kv:<key>", v:"<string>"}`;
Redis: `parlay:kv:<key>`. Canonical keys: `vc:room:<chat_id>`,
`queue:votes`, `share:links`, `spotify:state`, `radio:state`.
Canonical VC room JSON v1, contract fields only, restored rooms
come back paused. Configured-but-unreachable store is fatal at
startup (no silent split-brain fallback). All clients std-only
(Rust `store.rs` in 4 services, Node `store.js` in vc-ts/radio-js,
vc-go std-only RESP client + canonical room mapping). Fail-over
proven live: **Mongo, vc-go → vc-ts** (same tracks, order,
position 72, loop 2, paused); **Redis-only, vc-go → vc-rust**
(same queue, position 38, loop 1, paused); restart survival for
votes, shares, Spotify watches, radio state; file fallback
unchanged (exact legacy JSON files, restore after restart).
Dead sqlite `DATABASE_URL` was already gone (Phase 5). Tests: 200
passed / 0 failed across the touched services. Report:
`phase7/worker-a11-store.md`.

## 6. Residuals (Worker R)

- **A8:** top-level `NowPlaying.stream_url` added in vc-go
  (`NowPlaying.StreamURL` + `snapshotLocked`), vc-ts
  (`nowPlaying()`), vc-rust (`Track.stream_url`, resolver now
  passes `-f bestaudio/best` so yt-dlp returns a real direct audio
  URL, serialized at both levels); stale "0.1.0-scaffold" comment
  fixed. Also fixed a vc-rust test flake (two `play_stream` tests
  racing on process-global `CORE_URL`/`PARLAY_TOKEN` — shared
  mutex).
- **A4:** `POST /v1/mute` added to the core HTTP API mirroring
  `/v1/join` (auth gate, 400/401/405/502 shapes, `muted` defaults
  true), wired to new `Dispatcher::muteVoice` — real mute on
  core-owned sessions, honest refusal on engine-owned sessions.
  Live smoke verified `/v1/join` still works and `/v1/mute`
  behaves (502 nothing-playing, 400 bad body, 405 GET).
- **A11 SCRAM in Rust/Node store clients:** full SCRAM-SHA-256
  (fallback SHA-1, MongoDB MD5-hex quirk, `authSource` defaulting,
  `?authMechanism=` force) std-only in all 4 Rust `store.rs`
  copies and both Node `store.js` copies (built-in `crypto`
  only). Live-verified against scratch auth + plain mongods
  (SHA-256/SHA-1 CRUD, wrong-password honest failure, no-auth
  regression, `authSource=admin`, `SharedStore` through an auth
  URI). Report: `phase7/worker-r-residuals.md`.

## 7. Coordinator verification (fresh re-runs, 2026-11-03)

- **717 passed / 0 failed / 13 skipped** (core-cpp 394: dispatcher
  128, router 20, http_api 71, tdlib 44, telegram_api 24, store 40
  vs live mongod 8.0.4 + redis, scram 67 vs live auth + plain
  mongods; vc-go 30 + 5 skipped; vc-ts 52; vc-rust 50; spotify-rust
  51; audio-fx 6; share 17; queue 20; downloaders 42 + 8 skipped;
  netease-go 23; gui-js 11; radio-js 21). With
  `PARLAY_LIVE_TEST=1` the 3 vc-go live tests also pass (real
  YouTube resolve, real autoplay pick, real stream_url) — this
  closes B12.
- Combined smoke **12/12** `/healthz` 200 (`phase7/smoke-results.txt`).
- Stub grep: **0 real stubs** in non-test code (0 TODO/FIXME/HACK/
  NotImplemented case-sensitive; XXX only the mkstemp template).
- Secret scan: clean (no credential values in the tree; `.env`
  600, gitignored, untouched).
- Fixes by the coordinator during verification: stale tdlib README
  "8/8 PASS" → 44/44; `phase7/` added to `.gitignore` (3.2 GB of
  worker scratch: docker images, mongo data, logs — never
  committed). Mongod version note: the 7.0.14 binary in
  `.tools/mongo/bin/` cannot open 8.0.4 dbpaths (exit 62); use
  `.tools/mongodb/bin/mongod` (8.0.4).

## 8. What remains

See REMAINING.md (source of truth): A1 live proof (user testing
now), A6 compose-up on a real Docker host, key-gated items only,
and the known limitations (no `mongodb+srv`/replica sets, no
SASLprep, no channel binding; thin live coverage by design; build
costs). No Section A code gap remains.
