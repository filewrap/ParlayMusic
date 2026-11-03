# Phase 6 — TypeScript lossless frontend + live VPS proof preparation

Date: 2026-11-03. Repo: `filewrap/ParlayMusic`, branch `main`.
Head before this phase: `7b95637` (Phase 5). `.env` untouched.
`FINAL-ARCHITECTURE.md` NOT edited (locked; §10 flag carried below).
VERSION stays `v0.2.0` (Q13 undecided — not decided here).

This report is Worker D's verification, reconciliation, packaging, and
push. Every count below is from Worker D's own fresh runs in this
session — no worker's numbers are copied. Where a worker's number
differs from a fresh run, the difference is reconciled in §5.

## 1. What was built and changed

Three workers built concurrently on the shared tree; Worker D verified,
reconciled, fixed two honest gaps, and packaged.

### Worker A — TypeScript Mini App + FLAC streaming endpoint

- `services/miniapp/` (new): typed single-page player, no framework,
  no runtime dependencies. `src/app.ts` (now-playing, queue,
  play/pause, skip, volume 0–200 debounced, Stream FLAC `<audio>`),
  `src/telegram.d.ts`, `index.html`, `styles.css`, `build.sh`
  (typecheck + build to `dist/`), `README.md`. State sync is a 2 s
  poll of `/api/v1/now-playing` (no SSE yet — documented gap).
- `services/backend-go/internal/httpapi/` (new): the backend's first
  HTTP server. `GET /api/v1/stream/:chat_id/flac` serves the cached
  FLAC for the current track from the real Phase 4 store (bbolt
  library index → file verified on disk: exists, `.flac`, `fLaC`
  magic) via `http.ServeContent` — full Range support, no size cap,
  no full-file read, no re-encode. Honest 404s (nothing playing / no
  cached file / not FLAC / bad bytes), 400 on malformed chat id.
  State/control endpoints reuse the exact `gui.*` / `vc.*` UDS
  actions via a new `uds.Server.Dispatch` (`internal/uds/uds.go`):
  `GET /api/v1/now-playing`, `GET /api/v1/queue`, `POST
  /api/v1/pause|resume|skip|volume`, `GET /api/v1/healthz`; `/`
  serves the built bundle (`miniapp_dir`, 503 `miniapp_not_built`
  when unbuilt).
- Config: `[backend] http_addr` (default `127.0.0.1:8081`, empty
  disables), `[backend] miniapp_dir` (auto-search
  `services/miniapp/dist`), validation, `ResolveMiniAppDir()`.
- Auth gap (documented, not faked): **no initData validation exists
  in-tree** — the bot token lives in the core, not the backend. The
  gate is the loopback-only default bind plus a loud warning on a
  non-loopback address. Real initData validation (HMAC over the bot
  token) is a live-test item for the VPS deployment.

### Worker B — vc.join timeout + wedged join handler cancellation

- Premise corrected by measurement: the 60 s vc.join wedge was NOT
  `ntg_connect` blocking (probes show it returns fast for every
  input found, including the fake SDP and valid-but-unreachable
  transports) — it was `serveConn` calling `handleRequest`
  synchronously in the UDS read loop, deadlocking against
  `RequestJoin` until the 60 s deadline. Fixed: per-request
  goroutines in `internal/uds/uds.go` (writes serialised by the
  existing per-connection `writeMu`).
- Defence in depth, as ordered: `Client.Connect(ctx, …)` in
  `internal/voice/voice.go` runs the cgo call in a worker goroutine
  with a done channel and a ctx/timeout select; `Mutex` →
  `RWMutex` (ops take the read lock, only `Close` takes the write
  lock); timeout is the existing `timeouts.join_ms` (30 s default),
  wired in `cmd/backend/main.go` — no new key. Honest documented
  limit: a cgo call that never returns cannot be killed from Go;
  the wrapper bounds the *wait* and detaches cleanly.
- Join-handler cancellation in `internal/voice/manager.go`: failed
  joins land in `StateFailed` with the stage named; the next
  voice-state action answers at once.
- Regression tests: 2 UDS tests (join response delivered while a
  handler waits; slow handler doesn't starve the next request) + 5
  connect-timeout tests, all passing (see §2).

### Worker C — Q7 settings rewire + parity re-verification + benchmarks

- Q7 resolved: backend bbolt is the single home for all 15 settings
  keys; core `state.json` keeps only boot/ephemeral state. New
  `Dispatcher::settingsFor(chat_id)` (UDS GET-all, 60 s/5 s cache),
  `setSetting` (POST one key, honest failure), `saveChatSettings`
  returns bool and posts only changed keys (diff vs snapshot,
  `clean_delay_min` first then derived `clean_mode`); runtime fields
  moved to a new `chatrt:<chat_id>` hash with a one-time legacy
  `chat:` read fallback; `autoplay_lang`/`autoplay_mood` backend
  `""` ↔ core display `"auto"`/`"any"`; backend-down fallback is
  **fail-open with locked defaults + degraded note** — policy
  flagged for the user (fail-closed is a one-line change;
  see §6, item 2).
- `uds_path` vs `socket_path` hazard: `SocketPathSplitWarning()` +
  startup warning, including a live-found false positive for
  env-set `PARLAY_UDS_PATH` (fixed: `UDSPathFromEnv`).
- §10 parity: contract 65 actions (56 + 9 OD-3), routing table 249
  rows, 0 blank cells; `FINAL-ARCHITECTURE.md` §10 still says 56 —
  NOT edited, flagged (§6, item 5).
- >50 MB upload proof: 60 MiB sparse synthetic file through the
  real `sendLocalFile` path (`inputFileLocal`, path-only request
  <2 KiB, no size cap, core never reads the file). Two gaps flagged
  (§3).

### Worker D — verification fixes (this session)

1. **Gap 1 fixed (trivially honest rewording, §3):** the "queued for
   a later send" promise removed from the 3 core messages
   (`dispatcher.cpp`), the `contracts/ipc.json` note, the
   `registry.go` comment, and the NetEase comment + response note.
   New wording states the file is not sent and no large-file send
   exists yet. No redesign; the recommended send-via-`sendLocalFile`
   fix is NOT implemented (needs a user order).
2. **envStr empty-shadows-fallback bug found by the scratch
   verification and fixed (§4):** `.env` ships `TD_API_ID=` /
   `TD_API_HASH=` empty; both `envStr` helpers returned `""`
   instead of the documented fallback, so a stock install.sh
   install could never authorize the core. Fixed in
   `services/core-cpp/src/config.cpp` and
   `services/core-cpp/src/tdlib_client.cpp` (empty == unset, same
   rule as `envInt` and the `PARLAY_DATA_DIR` handling); +3
   regression CHECKs.
3. **Packaging:** Mini App bundle added to `scripts/package-release.sh`
   (step 2c, staged at `services/miniapp/dist` so the backend
   auto-search finds it from the extract root).
4. **start.sh repo-layout build** now uses `-tags parlaydsp`
   (previously built the backend without the tag, so every Play
   would fail with `dsp.ErrUnavailable` on a `--from-repo`
   install); verified by building that exact command.
5. **`.gitignore` narrowed:** blanket `phase6/` → `phase6/*` +
   `!phase6/*.md`, so the Phase 6 reports commit while scratch
   artifacts (`mongod.log`, `smoke.sh`, `smoke-results.txt`,
   `smoke-run`) stay ignored. (Precedent: `phase4/` reports were
   force-added despite the same blanket rule; narrowing is cleaner.)

## 2. Verified sandbox counts and measurements (Worker D fresh runs)

### Rust DSP (`services/parlay-dsp`)

| Check | Result |
|---|---|
| `cargo test --release` | **13/13 passed** |
| `cargo clippy --release --all-targets` | **0 warnings** |
| Throughput (release, this run) | **12.8x realtime** (10.02 s audio in 780 ms; bar 5x; Worker C measured 31.8x on a quieter box — load-dependent, both ≫ bar) |
| First-frame latency (release) | **4.06 ms** (< 500 ms bar) |
| `cargo test` (debug) | **12/13** — `first_frame_latency_and_throughput` fails at ~1.0x vs the 5x bar (debug-build rubato slowness; production links `target/release/libparlay_dsp.so`). Decision: accepted as a debug-only artifact and noted; no DSP code touched, bar not changed (§6, item 4) |
| Gapless / zero-drop (release) | 201 frames per 4 s file, bit-exact concat (`max\|two-one\| = 0`), loop exactly one `[2]` transition, cross-rate max zero run `= 0`, streaming 201 frames, 1 underrun — as reported by Worker C, re-confirmed by the same release suite |

### Go backend (`services/backend-go`, Go 1.23.0, `GOTMPDIR` on home disk — `/tmp` is a 512 MB tmpfs and fills at link time)

| Check | Result |
|---|---|
| `go vet ./...` | **clean** |
| `gofmt -l` (`cmd` + `internal`, incl. Worker D's edited files) | **clean** |
| `go test ./... -count=1` (default) | **323 passed / 3 failed / 12 skipped, 28/29 packages** — only `handlers` fails |
| `go test ./... -count=1 -tags parlaydsp` (shipping config) | **331 passed / 3 failed / 12 skipped, 28/29 packages** — only `handlers` fails |
| The 3 failures (both runs) | `TestFeedEndAdvance`, `TestTransitionAdvancesWithoutRebind`, `TestTransitionMismatchRebinds` — pre-existing timing flakes, proven at pristine HEAD (§5) |
| `internal/voice` (tagged) | 26/26 incl. the 5 new connect-timeout tests |
| `internal/uds` (tagged) | 10/10 incl. the 2 new read-loop tests |
| `internal/httpapi` | 10/10 (see FLAC proof below) |

### C++ core (`services/core-cpp`, `ctest`, 31.7 s)

| Suite | PASS |
|---|---|
| uds_test | 40 |
| config_test | 57 (+2 Worker D regression checks) |
| rate_limiter_test | 28 |
| dispatcher_test | 313 |
| routing_test | 944 (absolute-path invocation; 83 templates, 116 harvested callbacks, 0 failures) |
| tdlib_test | 47 (+1 Worker D regression check) |
| telemetry_test | 5 |
| ui_test | 160 |
| scram_test | 16 |
| store_test | 29 |
| **Total** | **10/10 suites, 1,639 PASS lines** (Worker C's pre-fix total of 1,636 reproduced exactly before Worker D's +3) |

### Dispatch check (real C++ `IpcClient` vs real backend, dummy creds)

- Fresh-built `core_peer` + fresh `-tags parlaydsp` backend, two
  fresh backends: **65/65, twice, exit 0** (run 2: 2.36 s).
  `vc.join` → `PASS engine_unavailable (voice join failed: voice:
  join chat -1001: connect: ntgcalls error -102: Invalid JSON)` —
  fails fast with the exact library reason, no `abandoned past
  deadline` in the backend log. `vc.lyrics` answers honestly right
  after (`bad_request: no title to look lyrics up for`). No action
  answered `unknown action`.
- Same check against the **extracted tarball's backend**: **65/65,
  exit 0**.

### End-to-end audio-path probe (Phase 5 method, temporary tagged test, run then deleted)

Real `PrefbufferedStream` (Prefetcher against a local HTTP server
serving a 2 s WAV tone) → real parlay-dsp engine (`dsp.New()`,
cgo) → real ntgcalls sink (`CreateCall` + `SetExternalAudioSource`
+ `SendExternalFrame`):

| Measurement | Result |
|---|---|
| Frames DSP → sink | **202 in / 202 out** (matches Phase 5's 202/202 exactly) |
| Frame geometry | **0 bad-size frames** — every frame exactly 1920 bytes |
| Non-silent frames | **202/202** (tone present end to end) |
| Transitions | **0** (single track) |
| First frame (open → first `ReadFrame`) | **4.13 ms** open; first frame **3.99 ms** after feed start |
| Wall for 2 s audio | **2.01 s** (real-time pump, as designed) |

### FLAC streaming proof (fresh `internal/httpapi` run)

Fixture: real ffmpeg FLAC, 5 s 440 Hz sine, 48 kHz stereo,
**75,226 bytes**, sha256
`383e079add403e1ed0b12b4d043ce964058a2c3ead77b0acefdff528b36a812b`
(identical to Worker A's fixture hash).

- Full `GET`: **200**, `audio/flac`, `Accept-Ranges: bytes`,
  body **byte-identical** (sha256 match).
- Ranges, all **206** byte-identical: `0–99` (100 B),
  `1000–1999` (1,000 B), open-ended `74226–` (1,000 B), last byte
  `75225–75225` (1 B). Out-of-range → **416**,
  `Content-Range: bytes */75226`.
- Error cases: no room / no cached file / cached WAV / `.flac`
  with non-FLAC bytes → honest 404s; malformed chat id → 400.
- Stream-while-playing: **60/60 concurrent requests
  byte-identical** while a real `PlayStream` fed a 30 s tone;
  feed consumed **8,192 bytes** across the HTTP window; `POST
  /api/v1/pause` mid-stream → 200 `is_paused: true`.

### Mini App

- `tsc --noEmit`: **clean**. `services/miniapp/build.sh` builds
  `dist/` (`app.js` **10,385 bytes**, `index.html`, `styles.css`).
  Note: Worker A reported 10,374 bytes; the 11-byte drift is a
  rebuild artifact (sources untouched since A).

### Opus ladder (real backend binary, one restart per profile)

| Profile | Readback |
|---|---|
| `standard_192k` | `bitrate=192000 complexity=10 vbr=true fec=false dtx=false frame=60ms maxplay=48000` |
| `high_256k` | `bitrate=256000 …` (rest identical) |
| `ultra_320k` | `bitrate=320000 …` (rest identical) |

Bogus profile → fatal at startup, exit 1:
`quality.voice_profile: must be one of ultra_320k, high_256k, standard_192k`.

### Cache-hit resolve

`TestRegistryResolveUsesCache`: uncached **44.05 ms**, cached mean
**13.08 µs**, cached total (100x) **1.31 ms**, stub resolver calls
**1** (exactly-once fetch). (Worker C: 676.8 µs / 15.9 µs — the
uncached figure is first-call/load noise; the cached mean agrees.)

### Greps (production code only)

- `TODO`/`FIXME`/`HACK`/`unimplemented` in `services/` Go/C++/Rust/TS
  prod code: **0**.
- `exec.Command` in prod `voice/`/`dsp/`/`handlers/`: **0**
  (test-only `ffmpeg` fixture rendering + `nm -D` in test files).
- `mongo`/`redis` in `services/core-cpp`: **0**.
- `/home/hatch` in Go prod code: **0**.
- No subprocess per call in the audio path; no Mongo/Redis in core-cpp.

## 3. Reconciliation

### (a) The 3 handler failures — proven pre-existing at pristine HEAD

Worker B's proof reproduced by Worker D, same method (pristine-HEAD
worktree at `7b95637`, `refs/` symlinked):

- Isolated `-run` (3 reps): `TestFeedEndAdvance` fails **3/3**,
  `TestTransitionAdvancesWithoutRebind` fails **3/3**,
  `TestTransitionMismatchRebinds` fails **2/3** (passed once).
- Full `handlers` package at HEAD (2 runs): fails both runs, with a
  varying subset each time (run 1: Mismatch only; run 2: FeedEnd +
  Advances).
- Mechanism (read, not fixed — out of scope): `advanceFromTransition`
  sets the feeding record **after** the room advance, so `waitRoom`
  can observe snapshot=B while feeding is still A. Fixing it means
  changing the handlers transition ordering or the test — not this
  phase's call.

Reconciled totals: the suite is 338 top-level tests untagged
(323+3+12 in the final run); earlier runs showed 324/2/12 and
327/3/12 (JSON count, subtests included). The failing set varies
run to run but is always a subset of the same 3 flakes. Worker A's
323/3/12 and Worker B's 333/1/12 tagged are the same suite seen on
different flake draws; Worker C's 326/0/12 was a draw where all
three passed. Nothing here is a Phase 6 regression.

### (b) Rust DSP debug 12/13 — accepted as a debug-only artifact

`first_frame_latency_and_throughput` fails in debug at ~1.0x
realtime vs the 5x bar (debug-build rubato sinc slowness). Release
is 12.8x this run (31.8x on Worker C's quieter run) and the
production path links `target/release/libparlay_dsp.so`. Decision:
**note it, do not touch DSP code or the bar**. The release suite
is the shipping gate. If the user wants the debug suite green,
the bar needs a debug-specific relaxation — a spec call (§6).

### (c) Worker C's flagged gaps

1. **"Queued for a later send" — FIXED by Worker D** (trivially
   honest rewording; §1, item 1). Locations were
   `dispatcher.cpp:4065, 4243, 4295`, `contracts/ipc.json:958`,
   `internal/download/registry.go:10`,
   `internal/netease/netease.go:14, 156, 164`. Re-verified after
   the edit: `dispatcher_test` 313/313, netease + download
   packages pass, `go vet` clean. The recommended real fix (send
   oversize files immediately via the proven-uncapped
   `sendLocalFile` path) is NOT implemented — it needs a user order.
2. **NetEase oversize inconsistency — FLAGGED, not redesigned.**
   NetEase >50 MB returns an error and the partial file is deleted
   (`executeJob` snapshots the dest dir and `removeNewFiles` cleans
   it up); other providers return `oversize:true` with the file
   kept on disk. Same recommended fix as (1), or an explicit ruling
   that NetEase oversize is an error by design. (§6, item 3.)

### Other reconciliations

- Worker C's C++ total 1,636 reproduced exactly pre-fix; post-fix
  1,639 (+3 Worker D regression checks).
- Two Worker C reports exist in `phase6/` (`worker-c.md`:
  speed/bass/broadcast/speedtest; `worker-c-parity.md`: Q7/parity).
  Both are in the tree and covered by the suites above
  (dispatcher 313 includes the §G speed/bass and §H speedtest
  checks).

## 4. Packaging — fresh tarball proven in the sandbox

`scripts/package-release.sh` (with the Phase 6 Mini App step):

- Built: `release/parlay-v0.2.0-linux-x86_64.tar.gz`,
  **57,584,737 bytes**, **26 files**,
  SHA256 `a38169c1fb75d7c68c8de77ea917b473d7b94cbba1e61fe85c7d9ad9d1de65c7`.
  SHA256SUMS inside the tarball verifies (0 non-OK lines).
- Staged: patched `libntgcalls.so` (**98,123,704 B** — the Phase 5
  patched build, size-pinned), `libtdjson.so.1.8.69`
  (**50,166,944 B**), rebuilt `libparlay_dsp.so` (**2,552,488 B**),
  `services/miniapp/dist` (`app.js` 10,385 B), launchers,
  `parlay.toml.example`, `.env.example`, README, INSTALL.md, VERSION.
- Symbols: **21/21** required `ntg_*` symbols in the staged
  `libntgcalls.so` (incl. `ntg_opus_set_config` /
  `ntg_opus_get_config`); `libparlay_dsp.so` exports
  `parlay_dsp_create/destroy/push/pull_f32/pull_s16/…` (19 `T`
  symbols).
- Runtime-link check in the script passed (both binaries resolve
  every shared lib from the staged `lib/` alone); secret sweep
  passed (no `.env`/session files staged, no live token/hash
  values).

### Scratch-root verification (fake .env, dummy token `123456789:FAKE…`, API_ID `12345678`)

Extract to a scratch root, `./install.sh`, `./start.sh`,
checks, `./stop.sh`:

- `install.sh`: **exit 0**. yt-dlp venv created, yt-dlp installed,
  slskd **0.26.2.0** fetched, `.env` written mode 600.
- `start.sh`: **exit 0**. Backend OK (socket up), core OK
  (`/healthz` 200), slskd SKIPPED (opt-in, no account).
- Backend from the extract: Opus readback 192k, `parlay-dsp
  linked`, HTTP on `127.0.0.1:8081` with miniapp dir
  `services/miniapp/dist` found (no bind warning).
  `GET /` → **200**, `GET /app.js` → **200**,
  `GET /api/v1/healthz` → **200**
  `{"ok":true,"service":"backend-go","version":"0.1.0"}`,
  `GET /api/v1/now-playing?chat_id=-1001` → honest 404.
- Dispatch against the extract's backend: **65/65, exit 0**.
- Core from the extract: `/healthz` → `{"ok":true}`, `status.sh`
  shows both UP. With the dummy token the core reaches
  `authorizationStateWaitPhoneNumber` (past
  `setTdlibParameters`) and waits on the Telegram round-trip —
  it cannot complete auth in the sandbox (proxy-only egress, no
  direct TLS to Telegram DCs, invalid token by design).
- `stop.sh`: both stopped, ports 8080/8081 free.
- `docker-compose.yml`: YAML parses, 3 services
  (`backend-go`, `core-cpp`, `slskd`), shared `parlay-data`
  volume, core `depends_on` backend-go healthy; backend
  Dockerfile builds with `-tags parlaydsp`. `docker compose up`
  itself could NOT run in the sandbox — no Docker binary here
  (the compose file documents the same).

### The envStr bug (found by this verification, fixed)

Round 1 of the scratch test exposed a real packaging-blocking bug:
the core exited immediately with `could not authorize the bot
session: not authorized: no credentials`, even though
`API_ID`/`API_HASH` were set. Root cause: `.env.example` ships
`TD_API_ID=` / `TD_API_HASH=` empty, and both `envStr` helpers
returned `""` for a set-but-empty var instead of the documented
fallback — so `TD_API_HASH=""` shadowed `API_HASH`. Fixed in
`config.cpp` + `tdlib_client.cpp` (empty == unset, matching
`envInt` and the `PARLAY_DATA_DIR` handling); round 2 of the
scratch test proves the fix (core reaches `WaitPhoneNumber` with
a stock install.sh `.env`). An orphan `./backend` from Worker B's
dispatch work (pid 145638, `/tmp/phase6-b`, holding :8081) was
also found and killed during round 1 — it had made the scratch
backend's HTTP bind fail with "address already in use" and made
the Mini App answer 503 from the wrong backend.

## 5. What remains honestly unmeasured (needs the live VPS)

Everything below needs a real Telegram account, a real group call,
or open egress — none of it can be proven from this sandbox:

- Real `vc.join` on a real group call (success path,
  `ConnectionConnected` callback, audible playback). Sandbox only
  proves the fast honest failure (`-102 Invalid JSON` on the fake
  SDP).
- Opus bitrate actually on the wire (192k stereo) and the
  320→256→192 ladder ceiling. Encoder config is proven in-process
  only.
- Live >50 MB delivery to real Telegram (sandbox proof used a
  synthetic sparse file + fake client by design).
- Mini App opening from a real Telegram client; `<audio>` streaming
  a real FLAC while a real group call plays; `vc.resume` / `vc.skip`
  through the Mini App against a live call (sandbox proves the
  honest 502s without a call and pause/volume 200s mid-stream, not
  the 200s with a call).
- Live YouTube/JioSaavn `/play` resolve timings (sandbox TLS
  interception blocks them; no creds here).
- Live verification of the Q7 rewire on a real group (settings
  panel, toggles, clean-mode, backend-down behavior) — sandbox
  tests use FakeIpc.
- Live verification that the `uds_path` warning fires/doesn't fire
  in the real deployment layout (env vs file keys).
- A genuinely blocking `ntg_connect`: the timeout wrapper is proven
  via a stand-in, not against a real stuck library call. If a real
  group call's Connect blocks inside the library, the wrapper
  bounds the wait — but that path itself is unmeasured. Related:
  stop-while-Connect concurrency and library-internal serialisation
  for a genuinely stuck `ntg_connect` are unproven (watch for a
  crash on the live proof).
- `docker compose up` end to end (no Docker in the sandbox).
- Real initData validation for the Mini App (none exists in-tree;
  the VPS deployment must put the HTTP server behind a trusted
  reverse proxy with TLS + initData check before any real user
  opens it).
- `/speedtest` upload on a host with open egress (sandbox POSTs
  stall at the egress proxy); broadcast `errors.txt` to a real
  chat; `/speed` + `/bass` on a real core-voice session.

## 6. Live-test checklist for the user (deploy from the tarball)

Prerequisites: a VPS (Ubuntu 22.04+ / Debian 12, x86_64) with a
real `BOT_TOKEN`, `API_ID`, `API_HASH`, `OWNER_ID` (and a user
account / phone for the assistant session if voice joins are
wanted). Tarball:
`release/parlay-v0.2.0-linux-x86_64.tar.gz`
(SHA256 `a38169c1fb75d7c68c8de77ea917b473d7b94cbba1e61fe85c7d9ad9d1de65c7`).
Do NOT decide Q13 here — VERSION is still v0.2.0.

1. **Deploy**
   - Copy the tarball to the VPS, extract it, `cd` into the
     extracted dir.
   - Run `./install.sh` with the real credentials exported (or
     answer the prompts). It creates the yt-dlp venv, fetches
     slskd, and writes `.env` (mode 600, never overwritten on
     re-run). Confirm: `install.sh` exits 0 and prints
     `Parlay v0.2.0 installed`.
   - Optional: copy a `parlay.toml` next to the install root for
     non-default settings (e.g. `quality.voice_profile`).
2. **Start**
   - Run `./start.sh`. Expect: `backend OK` (socket
     `data/parlay-backend.sock`), `core OK`
     (`http://127.0.0.1:8080/healthz`), slskd SKIPPED unless
     Soulseek is configured. Check `logs/backend.log` for
     `opus encoder bitrate=192000 … (profile standard_192k)` and
     `parlay-dsp linked`, and `logs/core.log` for
     `logged in as @<bot>`. If the core exits, the log names the
     exact missing credential — fix `.env` and re-run
     `./install.sh` (it will not overwrite your values) then
     `./start.sh`.
   - `GET http://127.0.0.1:8080/healthz` → `{"ok":true}`.
   - `GET http://127.0.0.1:8081/api/v1/healthz` →
     `{"ok":true,"service":"backend-go",…}`.
3. **Real group-call join**
   - In a real group with the bot, start a group call and run
     `/play <song>`. Expect: the bot joins, and `/vc` / now-playing
     shows the track. If the join fails, the error must name the
     exact stage/reason (not a 60 s hang): watch for a
     `voice: join chat …: connect: …` line in `logs/backend.log`.
     A join that takes the full 30 s `join_ms` and then fails fast
     is the timeout working; a hang past that is a bug — report
     the log lines.
4. **Audible playback**
   - Listen in the group call: the track must be audible, in
     sync, with no stutter or early cut at track end. Try
     `/skip`, `/pause`, `/resume`, `/volume 150` — each must act
     immediately and show in now-playing. Try a second track:
     gapless handover (no silence gap, no replay of the tail).
5. **Opus ladder on the wire**
   - With a call active, check the negotiated Opus bitrate on the
     wire / in the call stats for each profile: restart the
     backend with `quality.voice_profile = standard_192k`, then
     `high_256k`, then `ultra_320k` (in `parlay.toml`; the startup
     log line must read back 192000 / 256000 / 320000). Confirm
     the audible quality changes as expected and the call stays
     stable at 320k. A bogus profile must still fail loudly at
     startup (not silently fall back).
6. **>50 MB upload**
   - Play/download a track known to be over 50 MB (e.g. a long
     FLAC via `/play` + download flow, or a replied file). Expect
     the honest message: `That file is over the 50 MB send cap —
     not sent. There is no large-file send yet.` (the old "queued
     for a later send" wording is gone — there is no later pass).
     Then confirm the file is on disk under `data/downloads`
     (other providers) and report whether it plays via `vc.play`
     from the local path.
7. **Mini App FLAC streaming during a call**
   - With a track playing in a group call, open the Mini App
     (bot menu button / WebApp link for the chat). Expect: the
     player shows the real now-playing track and queue; the
     Stream FLAC button plays the same audio in the browser
     `<audio>` element while the call keeps playing (no stutter
     in either). Pause/resume/skip/volume from the Mini App must
     act on the live call. NOTE: the HTTP server is loopback-only
     by default — before exposing it, put it behind a trusted
     reverse proxy with TLS + Telegram initData validation; the
     backend cannot validate initData itself (it has no bot token).
8. **vc.join timeout behavior**
   - With the bot NOT in a call, run `/play` in a group where the
     bot cannot join (e.g. no permission / no active call to join
     into, or the group call ended): the command must fail fast
     (seconds, with the exact reason) — never hang for 60 s, and
     the next command (`/lyrics`, `/queue`) must answer
     immediately afterwards, not starve behind the dead join.
     Check `logs/backend.log` for any `abandoned past deadline`
     line — there must be none.
9. **Settings (Q7)**
   - Open `/settings` in a real group: toggles must reflect and
     persist (play mode, clean mode with a delay, thumbnails).
     Set `/cleanmode 30` then `/cleanmode off` and confirm the
     stored values. Stop the backend, run `/playmode` — it must
     show defaults with the `(settings unreachable — showing
     defaults)` note and refuse writes honestly; restart the
     backend and confirm settings come back.
10. **Stop / restart**
    - `./stop.sh` stops both processes; `./start.sh` brings them
      back with the queue/store intact (`data/parlay.db`).

## 7. Open items needing a user decision

1. **Q13 (VERSION):** stays `v0.2.0` — not decided here.
2. **Fail-open vs fail-closed for backend-down settings gates:**
   implemented fail-open with defaults + degraded note. One-line
   change to fail closed if ruled so.
3. **NetEase oversize inconsistency:** error + delete vs keep +
   flag. Rule it by design, or order the send-via-`sendLocalFile`
   fix (which would also close Gap 1 properly).
4. **Debug DSP throughput bar:** release is green (12.8x–31.8x
   vs the 5x bar); debug is 12/13. Accept the debug-only red, or
   relax the bar for debug builds.
5. **`FINAL-ARCHITECTURE.md` §10 "56" vs live contract 65:**
   the locked doc still says 56. The doc is locked — how it gets
   corrected is the user's call.
6. **Carried over, still unruled:** `/recognize` has no provider
   anywhere in the tree (answers 502 honestly); G3 (`/menu`
   ignoring other users' taps by design); group Music opening the
   start card instead of going straight to search.

## 8. Files changed in Phase 6 (uncommitted → this push)

| Area | Files |
|---|---|
| Mini App (new) | `services/miniapp/{package.json,tsconfig.json,src/app.ts,src/telegram.d.ts,index.html,styles.css,build.sh,README.md}` (`dist/`, `node_modules/` built, gitignored) |
| FLAC + HTTP API (new) | `services/backend-go/internal/httpapi/{httpapi,flac,httpapi_test,stream_concurrent_test}.go`, `services/backend-go/internal/uds/dispatch_test.go` |
| vc.join fix | `services/backend-go/internal/{uds/uds,uds/uds_test,voice/voice,voice/manager,voice/voice_test,voice/connect_timeout_test}.go`, `services/backend-go/cmd/backend/main.go`, `parlay.toml.example` |
| Q7 + hazard + parity | `services/core-cpp/src/{dispatcher,dispatcher.hpp,store,store.hpp,ui_texts}.cpp/.hpp`, `services/core-cpp/tests/dispatcher_test.cpp`, `services/backend-go/internal/config/{config,config_test}.go` |
| Worker D fixes | Gap 1 wording: `services/core-cpp/src/dispatcher.cpp`, `contracts/ipc.json`, `services/backend-go/internal/{download/registry,netease/netease}.go`; envStr fallback: `services/core-cpp/src/{config,tdlib_client}.cpp` + regression checks in `services/core-cpp/tests/{config_test,tdlib_test}.cpp`; packaging: `scripts/package-release.sh`, `start.sh`; `.gitignore` (phase6 narrowing) |
| Reports | `phase6/PHASE6.md` (this file), `phase6/worker-a-miniapp.md`, `phase6/worker-b-join.md`, `phase6/worker-b-rerun.md`, `phase6/worker-c.md`, `phase6/worker-c-parity.md` |

No stubs, mocks, or placeholders in production code. No changes to
`FINAL-ARCHITECTURE.md`. Nothing pushed before this commit.
