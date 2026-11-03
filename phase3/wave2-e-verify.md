# Wave 2 — Worker E report: Verification, reconciliation, and push

Date: 2026-11-03. Base: main at `3d84fb6` (G1/G2 fix). Worker E is the
only worker who commits and pushes. `.env` never touched (mtime still
2026-11-02, 0600); all tests and live runs use dummy creds only (no
Spotify/Deezer/NetEase/AudD keys set anywhere); no service was started
with the user's token. `CURRENT-STATE.md`, `refs/`, `docker-compose.yml`
and `install/start/stop/status.sh` untouched (the last two flagged as
remaining for the release pass, see §8).

## 1. Reconciliation (all done)

### 1.1 Seams rewired to the shared instances in `app.Deps`

- **vote → `d.Room.Enqueue`** (`internal/vote/vote.go`): `Service` now
  holds the shared `room.Manager`; `enqueueOnThreshold` calls
  `s.room.Enqueue` (with the old dedup check preserved via
  `Room.Queue`). No more store-direct queue writes. Test strengthened:
  after threshold the track is asserted visible through both the
  store and the Manager's `Queue`.
- **gui → `d.Room.Snapshot`** (`internal/gui/gui.go`): `handleNowPlaying`
  uses `room.Snapshot` (live position clock), `handleQueue` uses
  `room.Queue`; the store-reading `loadRoom` is deleted. The exported
  `gui.Snapshot(room, queue)` helper is kept — `radio` renders its
  `now_playing` superset through it. gui tests now seed through the
  Manager (`Play` + `Enqueue`).
- **netease.lyrics → `d.Lyrics.FetchNetEase`** (`internal/netease/netease.go`):
  the `lyricsFetcher` interface is now the one-method `FetchNetEase`
  interface, satisfied by the shared `*lyrics.Fetcher`; `Register`
  takes `d.Lyrics` (with a nil guard — a nil `*Fetcher` stored in an
  interface would be non-nil and defeat the nil check). `Fetch` (with
  its LRCLib fallback) is never used here. `isNoLyrics` recognises
  both `netease.ErrNoLyrics` and `lyrics.ErrNoLyrics`.
  Spotify keeps its own Genius→Musixmatch→LRCLib chain — untouched.
- **Single construction verified:** production code has exactly one
  `room.NewManager` and one `lyrics.New`, both in `cmd/backend/main.go`
  (129–130). Test files construct their own per-test instances on
  their own temp stores, which is correct. `resolver.NewLyricsResolver`
  is still used inside `lyrics.New` and in Spotify's own chain (both
  intended).

### 1.2 `vc.play_file` duplicate deleted

- Deleted `internal/download/playfile.go` **and** `playfile_test.go`
  (246 + 210 lines), plus the `roomPlayer` field, `newRoomBackedPlayer`
  call, and `RoomPlayer` parameter from `download.Service` /
  `NewServiceWithRegistry` (one test call site updated).
  `download.Register` no longer registers `vc.play_file`; the only
  implementation is `handlers` (record-only per inventory §3.3).
- `cmd/backend/main.go` comments updated (the "registration order
  wins the slot" note is obsolete); `contracts/uds-protocol.md` §4.1
  note updated to record the resolution.
- Proof: `TestWiringPlayFileIsHandlersVersion` still passes with the
  handlers fingerprint after the deletion (run in the download +
  handlers suites, green).

### 1.3 `internal/uds` race fixed (pre-existing, Worker A flagged)

Root cause: `dialPeer` returns as soon as the socket connects, but
`acceptLoop` registers `s.conn` asynchronously; an `Emit` in that
window dropped silently (`s.conn == nil`).

Fix (no sleeps anywhere):
- `Server.WaitForPeer(ctx)` — blocks until a connection is registered
  (re-checks `s.conn` after every signal, so a stale registration
  signal cannot fake readiness); `acceptLoop` signals registration
  on a buffered channel.
- `Emit` stays fire-and-forget by design (events are never queued),
  but a drop is now **logged loudly** (`uds: Emit %q dropped: no
  core connected`) instead of vanishing silently.
- `TestEventsReachCore` now waits on `WaitForPeer` before emitting;
  new `TestWaitForPeer` (timeout-with-no-peer fails honestly; succeeds
  after dial; immediate with a live peer). The same 50 ms sleeps in
  `events_test.go` and `handlers_test.go:91` were replaced with
  `WaitForPeer` too.
- Result: `TestEventsReachCore` passes 5/5 runs (was 0/3).

### 1.4 Doc follow-ups

- (a) Inventory §10-U4 fixed to AudD (user decision 2026-11-03
  ~16:57 IST, free tier, env-gated on `AUDD_API_TOKEN`); the §9-E4
  "blocked on a user decision" line, the §10 open-questions item 4,
  and the footer "Undetermined" list updated to match.
- (b) `contracts/ipc.json`: `speed`/`bass` added to the `vc.play`
  request (record-only until the Phase 5 DSP); `vc.play_file` entry
  added (with the `voice_state` response shape); all 9 OD-3 actions
  added as entries, each marked `"ported": "Phase 3 Wave 2 (OD-3)"`.
  Validated JSON: **65 actions, 9 ported markers**.
- (c) `canonicalAction` `"play_file"` → `"vc.play_file"`: **NOT done,
  deliberately.** The core posts `"vc.play_file"` dotted directly
  (`dispatcher.cpp:2660`), and `canonicalAction` passes unknown names
  through unchanged — the mapping would change zero behaviour. No
  core-cpp change, no rebuild needed for it.

### 1.5 Spotify redirect URI verified

`.env.example:101` reads `SPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback`;
config loads it via env into `cfg.Backend.SpotifyRedirectURI` (no
default override anywhere). Verified with a temporary probe test
(`LoadFrom` with the env set → loaded value exactly
`http://127.0.0.1:8080/callback`, PASS; probe deleted afterwards).
Note: `spotify_test.go` uses `http://localhost:8080/callback` as a
dummy value in tests only — test-only, not a config value.

## 2. Bugs found by verification (fixed in this pass)

### 2.1 yt-dlp daemon killed 30 s after boot (Wave 1 bug, exit-benchmark blocker)

**Finding:** the "persistent" daemon went defunct ~30 s after backend
start; the next daemon call failed `write failed: broken pipe`.
Reproduced deterministically on two backends.

**Root cause:** `YtDlpClient.ensureStarted` spawned the daemon with
`exec.CommandContext(ctx, python, script)`, and the boot warm-up calls
`Ping` with a 30 s context (`main.go` `warmCtx`). `CommandContext`
kills the child when the context expires — so the daemon was killed
30 s after boot, every boot. The boot `Ping`/`Version` always
succeeded (inside the window), which is why the bug hid.

**Fix** (`internal/resolver/ytdlp.go`): `exec.Command` (no context
binding) for the daemon spawn; the ctx still bounds the ping
verification, a failed start is cleaned up by `stopLocked`, and
`Close()` kills the daemon at shutdown. The voice feeder's
`CommandContext` is **not** the same bug — its `feedCtx` is scoped to
the feed lifetime and is cancelled only when the feed stops.

**Regression test** (`TestYtDlpDaemonSurvivesCallerContextExpiry`):
starts the daemon with a 3 s context, sleeps past expiry, pings again
with a fresh context, asserts the same PID. (First version used a 1 s
starter context and flaked once under load — python + yt_dlp import
takes ~0.7 s — widened to 3 s; passes 3/3 since.)

### 2.2 `.venv` relocation (deletion consequence)

The yt-dlp runtime lived at `services/downloaders/.venv` — inside a
directory this pass deletes — and system python3 has no yt_dlp. Moved
to `scripts/yt-dlp-venv/` (next to the worker script it runs),
`findYtDlpPython` updated (walk-up + absolute fallback), worker
docstring updated, `.gitignore` gains `scripts/yt-dlp-venv/` (64 MB,
local-only). Verified: all 3 daemon tests pass via the new path, and
the real backend boots warm from it (§5).

## 3. Exactly-once registration (new test)

`uds.Server` gained a registration counter (`RegistrationCount`) and
`Actions()` — `Register` overwrites silently, so only the count can
prove exactly-once wiring. `ping` is now registered through `Register`
in `NewServer` so the count witnesses it too. `main.go`'s 13
`Register` calls were extracted into `registerAll` (same code main()
calls) so the test runs the **exact production wiring**:
`cmd/backend/main_test.go::TestRegisterAllExactlyOnce` asserts all 65
contract actions + ping, each count == 1, no extras. **PASS.**

## 4. Verification results

### 4.1 Go: `go vet` clean, `gofmt` clean, `go test ./...` green

- **25/25 packages ok, 245 tests passed, 0 failed, 12 skipped.**
  The 12 skips are all live-gated (`PARLAY_LIVE_TEST=1`): 11 resolver
  live tests + `TestFetchNetEaseLive` — network/creds-gated by design.
- Counts include the new tests added in this pass: exactly-once
  wiring (1), `TestWaitForPeer` (1), daemon-lifetime regression (1),
  rewire pins (vote Manager-queue assertion, netease nil-fetcher +
  `lyrics.ErrNoLyrics` cases).

### 4.2 Core-cpp: `ctest` 9/10, no new failures

- Clean configure + build, **0 warnings** (`-Wall -Wextra -Wpedantic`).
- **1,615 PASS lines** in the ctest log (baseline 1,555 + Worker D's
  +51 dispatcher / +9 routing = 1,615 exactly). Suites: dispatcher
  278 PASS, routing 944 PASS, ui/scram/tdlib/telemetry/rate-limiter/
  config/uds all green.
- Only `store_test` fails — **FAILURES: 2, the same 2 pre-existing
  env-only failures** (cannot connect to mongodb at 127.0.0.1:27099;
  redis also down). No core-cpp code was changed in this pass.

### 4.3 65/65 IPC actions against the REAL core UDS client

Method: the real backend binary (dummy creds, temp data dir) +
extended `tests/interop/core_peer.cpp` (Wave 2 rewrite: was a Wave 1
scaffold that expected `vc.play` to be *unwired*), which dispatches
all 65 actions through the real `parlay::IpcClient` with fast-failing
payloads (empty required fields, empty-state reads, key-gated honest
errors). A check passes on any real response frame (ok or honest
error); it fails only on transport failure or the `unknown action`
error.

**Result: 64/65 actions answered with a real response frame.**
Full per-action log: `/tmp/verify-e/core_peer_final.log`
(re-runs: `core_peer.log`, `core_peer2.log`). Representative honest
answers: `chat.settings`/`gui.queue`/`search.inline`/`queue.vote`/
`share.create`/`spotify.dislike`/`spotify.now_playing`/`vc.stop` ok;
`vc.now_playing`/`vc.progress`/`vc.pause`/`vc.loop`/`vc.volume`
`not_found` (nothing playing); `download.*`/`search.query`/
`lemonic.*`/`netease.*`/`vc.play` `bad_request` (empty query);
`spotify.*` `engine_unavailable` (no creds); `audio.recognize`
`engine_unavailable` (no `AUDD_API_TOKEN`); `library.sharescan`
`engine_unavailable` (slskd down); `download.progress`/`download.cancel`
`not_found` (unknown run).

**The 1 failure — `vc.join` — is an honest sandbox limit, not a
registration gap:**
- The action IS registered (exactly-once test, §3) and dispatched
  (backend log: `action vc.join request 39 …`).
- The `JoinRequest` half of the handshake is proven end-to-end: the
  probe's join handler ran (`join_request handler ran: yes`) and
  answered the fake remote SDP.
- The handler then blocks in `ntgcalls Connect` against that fake SDP
  (no media lines, no real group call): `client.Connect` takes no
  context, so it ignores the handler deadline — no response frame
  within 15 s, nor within 60 s on retry (backend log: `abandoned past
  deadline 60000ms`). A real group call is the only way to prove the
  Connect stage (Phase 6 live proof). Side note for Phase 5/6: a
  `vc.join` against an unresponsive peer leaves the handler goroutine
  blocked in `Connect` and the call in `StateJoining` until Connect
  returns — Connect needs a context or an internal timeout.

### 4.4 Embedded store: kill −9 + restart preserves state — PROVEN

Seeded via the live backend (chat −1001): `chat.settings` autoplay=true;
`queue.vote` ×3 voters → threshold 3 → `queued:true` (room enqueue
through the rewired vote path); `share.create` → token
`hZI2l9hLumr8PD5r`. Then `kill -9` on the backend, restart on the
same `data/parlay.db`, read back:
- `chat.settings` GET → autoplay still true;
- `gui.queue` → the voted track, position 0, `requested_by: 3`;
- `share.resolve` with the minted token → the track;
- `queue.vote` re-read → approvals 3, `queued: true`.
All four survived the SIGKILL. (The room itself is restored via
`Restore()` at startup — rooms come back paused by design.)

### 4.5 yt-dlp daemon warm — PROVEN (after the §2.1 fix)

- Boot log: `yt-dlp daemon warm, version 2026.10.26` (`Ping` +
  `Version` at boot work).
- Daemon PID 50892 (child of the backend, `scripts/yt-dlp-venv`
  python) alive 40 s after boot — past the old 30 s death window —
  and still PID 50892 after three daemon-touching resolves
  (`download.youtube` InnerTube attempt + two `download.tiktok`
  Extract round-trips through the daemon). Exactly 1 daemon process
  throughout; no fork per resolve. The TikTok extracts fail honestly
  on the sandbox TLS interception (`SSL: WRONG_VERSION_NUMBER`) and
  the daemon survives them.
- Both daemon fixes confirmed still in place in
  `scripts/yt_dlp_worker.py`: `"noprogress": True` on both
  `YoutubeDL` instances (lines 57, 84) and the `FFmpegExtractAudio`
  mp3 postprocessor via the Python API (line 99); the Go client still
  splits JSON-RPC lines on `\r` (`ytdlp.go` `readLoop`).

### 4.6 No HTTP servers, no Mongo/Redis, no stubs

- `ss -tln` shows no listeners; TCP probes to 127.0.0.1:8001–8010,
  3000, 8080 all closed; no `mongod`/`mongo`/`redis-server`/`slskd`
  process running.
- `grep -r "TODO|FIXME|HACK|XXX|unimplemented"` over the whole
  `services/backend-go` tree (code + tests): **zero matches**.

## 5. Deletions (after parity proven, §8.1)

Deleted (127 tracked files, `git rm -r`):
`services/vc-go`, `services/vc-ts`, `services/vc-rust`,
`services/share-rust`, `services/queue-rust`, `services/spotify-rust`,
`services/radio-js`, `services/netease-go`, `services/downloaders`,
`services/gui-js`.

Remaining `services/`: `audio-fx-rust` (KEPT as Phase 5 port source),
`backend-go`, `core-cpp`.

Notes:
- Untracked leftovers in those dirs (Rust `target/` build dirs,
  `__pycache__`, `vc-ts/src/ntg_addon.node`, `vc-rust/data/*` incl.
  115 MB of old downloaded audio + voice pipes, `*/data/state.json`)
  were removed with the dirs — regenerable build/runtime artifacts
  from the deleted services, not user data; OD-2 fresh start means no
  old state is migrated.
- The only live value in the deleted dirs was the yt-dlp venv — moved
  to `scripts/yt-dlp-venv/` (§2.2), not deleted.
- No backend-go code imports the deleted dirs (only provenance
  comments — "Ported from services/…" — which stay as history).
- `audio-fx-rust` is started by nothing: no backend code references
  it (only `fx` comments + the record-only effect names); the only
  references are `docker-compose.yml` (service def) and `start.sh`
  (binary path) — both untouched per this pass's orders, flagged in §8.
- NOT touched, per orders: `docker-compose.yml`,
  `install.sh`/`start.sh`/`stop.sh`/`status.sh`, `CURRENT-STATE.md`,
  `refs/`, `.env`.

## 6. What is honestly unmeasured / not live-verified

- **vc.join success / real voice call**: no real Telegram group call
  in the sandbox (UDP EPERM, no MTProto session, no direct TLS to
  Telegram DCs). JoinRequest handshake proven (§4.3); Connect stage
  unproven. Same for `vc.play` success, feed-end advance on a real
  call, radio handoff success, `vc.play_file` `voice_state.joined:
  true`. Handler/room transitions underneath are unit-covered.
- **All live provider paths**: no live download/resolve against any
  real provider (YouTube/InnerTube, JioSaavn, Deezer, TikTok,
  Navidrome, Spotify, Soulseek, NetEase lossless) — sandbox egress
  intercepts direct TLS and no creds exist. Verified only on
  validation/error paths + honest key-gated failures. Live
  `mix.generate` fetch, `search.query` fan-out timing, async
  `DownloadReady` over a live UDS connection: unmeasured.
- **Live AudD / Lemonic / Navidrome / slskd calls**: httptest fakes
  only; AudD token not supplied yet (user copies it to the VPS .env).
- **Live Spotify Web API / OAuth**: no client ID/secret in the
  sandbox; auth/refresh/callback end-to-end unmeasured.
- **Live lyrics**: LRCLib/NetEase/Genius/Musixmatch fetches unmeasured
  live (except the one anonymous NetEase fetch Worker A did).
- **Prefetch/cache (Phase 4), DSP (Phase 5), Mini App (Phase 6)**:
  not this phase; `fx.apply` is record-only, `vc.play_file` is
  record-only, speed/bass are stored-not-applied, seek >0 moves the
  room clock only (no `voice.Manager.Seek` yet).

## 7. Remaining / flagged (not done in this pass)

1. `docker-compose.yml` + `install.sh`/`start.sh`/`stop.sh`/`status.sh`
   still describe the deleted 12-service topology (incl. Mongo/Redis
   and the `audio-fx-rust` service) — release-pass rewrite needed.
2. `vc.join` Connect needs a context/internal timeout, or at least a
   documented bound — a stuck Connect leaks the handler goroutine and
   leaves the call `StateJoining` (§4.3).
3. Inventory open questions still open: Q1 `np` oversize 50 MB vs
   2 GB; Q2 `spotify.ignore`; Q3 `autoplay_lang`/`autoplay_mood`
   defaults; Q5 `vc.queue` `move` verb; Q6 `speed`/`bass` carrier
   (Worker D's D13 already resolves it in practice — `vc.play` carries
   them — but the inventory question text still lists it open);
   Q7 `ntg_time` vs wall-clock position source.
4. `fx` still persists preset/params via the store `RoomState`
   directly (not through `room.Manager`) — the same staleness shape
   the vote/gui seams had; the Manager should own it (small follow-up).
5. Download runs/prefs/library index are in-memory singletons (Worker
   B's gap #1): a restart loses finished-run snapshots and resets
   prefs — the store buckets (`downloads`, `library`, `dlprefs`) don't
   exist yet. Prefs/runs are not part of the kill-9 proof in §4.4.
6. Phase 4 entry is NOT claimed here: cache/prefetch work is
   untouched, and the `vc.join` live proof is outstanding (§4.3).
   Phase 4 starts on the parent's order, not this worker's.

## 8. Commit

Commit: `7460fed` — "Phase 3: unified Go backend, embedded store,
native resolvers — Wave 2 verification" (242 files changed, +33,047 /
−45,183), pushed to `origin/main` (verified `ls-remote` local ==
origin/main). No tag, no release. Phase 4 not started.
