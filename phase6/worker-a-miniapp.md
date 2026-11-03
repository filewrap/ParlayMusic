# Phase 6 — Worker A: TypeScript Lossless Mini App + FLAC streaming endpoint

Date: 2026-11-03. Branch: `main` (HEAD `7b95637`), working tree shared
with concurrently-running sibling workers — everything below is
uncommitted, nothing pushed, `.env` untouched. All work is Worker A's
unless marked otherwise.

## What was built

### 1. TypeScript Mini App — `services/miniapp/` (new)

Typed single-page player, no framework, no runtime dependencies.
TypeScript 5.9.3 is a dev-only build tool (`npm install` once after
checkout; `node_modules/` and `dist/` are gitignored).

- `src/app.ts` — the player: now-playing (title/artist/duration/
  position/status), queue list, play/pause (pause/resume toggle),
  skip, volume slider (0–200, debounced POST), and a Stream FLAC
  button driving an `<audio>` element whose `src` is the FLAC
  endpoint. `chat_id` comes from
  `Telegram.WebApp.initDataUnsafe.chat.id`, falling back to
  `?chat_id=` (dev). State refreshes by polling
  `/api/v1/now-playing` every 2 s (see Gaps: no SSE yet).
- `src/telegram.d.ts` — minimal WebApp typings (only what the app
  reads).
- `index.html`, `styles.css` — plain, zero emojis (§7.3).
- `build.sh` — typecheck (`tsc --noEmit`) + `tsc` + copy
  HTML/CSS into `dist/`. `README.md` documents every endpoint
  used and the known gaps.
- Build output verified: `dist/` = `app.js` (10,374 bytes),
  `index.html`, `styles.css`.

### 2. FLAC streaming endpoint — `services/backend-go/internal/httpapi/` (new)

`GET /api/v1/stream/:chat_id/flac`, served by a new HTTP server
in the backend (the backend previously had **no HTTP server at
all** — only the UDS socket).

- **FLAC source (real, Phase 4 store):** the room Manager names
  the current track; the library index (bbolt `library` bucket,
  persisted since Phase 4, recorded by the download package for
  every completed download) names the cached file for that track
  id; the file is verified on disk (exists, `.flac` extension,
  `fLaC` magic bytes) before serving. Served with
  `http.ServeContent` straight from the open `*os.File` — full
  Range support (206, `Content-Range`, `Accept-Ranges: bytes`,
  416 on unsatisfiable ranges), `Content-Type: audio/flac`, no
  size cap, no full-file read into memory, no re-encode:
  bit-perfect by construction.
- **Honest errors (real codes, no placeholder body):** nothing
  playing → 404 `nothing playing in this chat`; track never
  downloaded → 404 `no cached file for the current track`;
  cached file not FLAC → 404 `current track has no cached FLAC
  file`; `.flac` name with non-FLAC bytes → 404 `cached file for
  the current track is not FLAC`; malformed chat id → 400.
- **State + control endpoints (no second API):** `GET
  /api/v1/now-playing`, `GET /api/v1/queue`, `POST
  /api/v1/pause|resume|skip|volume`, `GET /api/v1/healthz`.
  These are a transport into the exact `gui.*` / `vc.*` UDS
  actions the core uses, via a new `uds.Server.Dispatch`
  method (`internal/uds/uds.go`) — same handler, same per-chat
  lock, same voice coupling, same honest errors, mapped to HTTP
  statuses (`not_found`→404, `bad_request`→400,
  `engine_unavailable`/`download_failed`→502, `rate_limited`→429).
  `POST /api/v1/volume` clamps 0–200 exactly as `vc.volume`
  does; `POST /api/v1/resume` with a current track but no voice
  call answers the honest 502 `no active voice call in this
  chat — use vc.join first` (same as Telegram).
- **Static serving:** `/` serves the built bundle from
  `miniapp_dir` with `Cache-Control: no-cache`. No bundle built
  → honest 503 `miniapp_not_built` naming the build step, never
  demo data. The stream endpoint works with no bundle at all
  (tested).

| Mini App UI | Endpoint | UDS action behind it |
|---|---|---|
| Now playing + position | `GET /api/v1/now-playing?chat_id=` | `gui.now_playing` |
| Queue list | `GET /api/v1/queue?chat_id=` | `gui.queue` |
| Pause / Resume | `POST /api/v1/pause`, `POST /api/v1/resume` | `vc.pause`, `vc.resume` |
| Skip | `POST /api/v1/skip` | `vc.skip` |
| Volume slider | `POST /api/v1/volume` | `vc.volume` |
| Stream FLAC / `<audio>` | `GET /api/v1/stream/:chat_id/flac` | library-index FLAC lookup |

### 3. Config + wiring

- `[backend] http_addr` (default `127.0.0.1:8081`, env
  `PARLAY_HTTP_ADDR`; empty disables the HTTP server) and
  `[backend] miniapp_dir` (default auto-search
  `services/miniapp/dist`, env `PARLAY_MINIAPP_DIR`), with
  validation (`host:port`, port 1–65535) and
  `Config.ResolveMiniAppDir()`. Keys added (commented) to
  `parlay.toml.example`.
- `cmd/backend/main.go` starts the HTTP server in a goroutine
  beside the UDS server; a bind failure is logged loudly but
  does not take the voice path down; clean shutdown closes it.

### 4. Auth — the gap, documented not faked

**No Telegram WebApp initData validation exists anywhere
in-tree** (verified by grep: zero `initData`/WebApp/auth
helpers in `services/backend-go`). The bot token lives in the
core, not the backend, so no helper could be built from what
the backend holds — and per the brief I invented no crypto
scheme. The honest gate is the bind: **loopback-only by
default** (`127.0.0.1:8081`); a non-loopback address logs a
loud warning at startup and must sit behind a reverse proxy
the operator trusts. Real initData validation (HMAC over the
bot token) is a **live-test item for the VPS deployment**.

## Verified counts (all fresh runs, this session)

- `tsc --noEmit` (Mini App): **clean**. `services/miniapp/build.sh`:
  builds `dist/` (3 files).
- `go vet ./...` (backend): **clean**. `gofmt -l` on all touched
  packages: **clean**.
- `go test` on the touched packages (`httpapi`, `uds`, `config`,
  `cmd/backend`): **all pass**.
- New tests: **11** (10 in `internal/httpapi`, 1
  `TestDispatchReusesHandlers` in `internal/uds`).
- Full backend suite (final run, `GOTMPDIR` on home disk — see
  incident below): **323 passed / 3 failed / 12 skipped,
  28/29 packages ok**. Phase 5 baseline was 28/28 packages,
  307 passed / 0 failed / 12 skipped; the +16 passed is my 11
  plus 5 the sibling workers added concurrently. The 3 failures
  are **not mine** — see "Flags" below.
- Grep over all my code: **zero** `TODO` / `FIXME` / `HACK` /
  `unimplemented`.

### Streaming proof (real FLAC bytes, ffmpeg-rendered 5 s 440 Hz sine, 48 kHz stereo)

Fixture: **75,226 bytes**, sha256
`383e079add403e1ed0b12b4d043ce964058a2c3ead77b0acefdff528b36a812b`.

- Full `GET`: **200**, `Content-Type: audio/flac`,
  `Accept-Ranges: bytes`, body **byte-identical** (sha256 match).
- Ranges, all **206** with correct `Content-Range` and
  byte-identical bodies: `bytes=0-99` (100 B), `bytes=1000-1999`
  (1,000 B), open-ended `bytes=74226-` (1,000 B), last byte
  `bytes=75225-75225` (1 B).
- Out-of-range `bytes=75326-`: **416**, `Content-Range: bytes
  */75226`.
- Error cases (all 404 with the exact messages above): no room,
  track with no cached file, cached WAV instead of FLAC, `.flac`
  file with non-FLAC bytes; malformed chat id → 400.

### Concurrency proof (stream while playing)

`TestStreamWhilePlaying`: a real ntgcalls client + fake DSP
engine decoding a 30 s WAV tone through `voice.Manager.PlayStream`
(feed pump pushing 10 ms frames), while the HTTP server answers
**60 concurrent requests** (6 workers × 10, mixing full-body and
ranged GETs) for the same chat. Result: **60/60 byte-identical**,
feed consumed **8,192 bytes across the HTTP window**, no feed-end
event fired, call record still live, and `POST /api/v1/pause`
mid-stream answered **200** with `is_paused: true`.

The sandbox cannot do a real Telegram join (no peer), so the
test uses a **synthesized remote transport** as the JoinFunc
stand-in for TDLib's `joinVideoChat` return value: the exact
JSON shape ntgcalls parses (`transport` with ufrag/pwd, one
host candidate, DTLS fingerprints, Opus/VP8 payload types),
with the candidate pointed at `127.0.0.1:9` so no media leaves
the box. Verified first in throwaway probes (echoing local
params fails `-102 Transport not found`; the synthesized
transport reaches `StateConnected`). This is a test double for
the join handshake only — production joins come from the core
over the UDS JoinRequest, unchanged.

### Real-binary smoke test

Built the backend binary and ran it with a temp config
(`data_dir` + `http_addr 127.0.0.1:18081` + `miniapp_dir` =
real `dist/`): clean startup (store open, yt-dlp warm,
ntgcalls 3.0.5, UDS listening, HTTP listening), then
`GET /api/v1/healthz` → **200** `{"ok":true,...}`,
`GET /` → **200** real `index.html`, `GET /app.js` → **200**,
`GET /api/v1/now-playing?chat_id=-1001` → **404** honest,
`GET /api/v1/stream/-1001/flac` → **404** honest, clean
shutdown on SIGTERM. (Binary built without `-tags parlaydsp`,
so the log's `parlay-dsp NOT linked` warning is the
documented default-build behaviour, not a regression.)

## Files changed (Worker A only — uncommitted)

New:

- `services/backend-go/internal/httpapi/httpapi.go` — HTTP server, static + state/control via `Dispatch`
- `services/backend-go/internal/httpapi/flac.go` — FLAC resolution + range streaming
- `services/backend-go/internal/httpapi/httpapi_test.go` — 9 tests (stream, ranges, errors, state, controls, static, oversize body)
- `services/backend-go/internal/httpapi/stream_concurrent_test.go` — stream-while-playing proof
- `services/backend-go/internal/uds/dispatch_test.go` — `Dispatch` test
- `services/miniapp/package.json`, `tsconfig.json`, `src/app.ts`, `src/telegram.d.ts`, `index.html`, `styles.css`, `build.sh`, `README.md` (`dist/` + `node_modules/` built, gitignored)

Modified:

- `services/backend-go/internal/uds/uds.go` — `Server.Dispatch` (+~20 lines)
- `services/backend-go/internal/config/config.go` — `http_addr`, `miniapp_dir`, `ResolveMiniAppDir`, validation, env overrides
- `services/backend-go/internal/config/config_test.go` — default assertions for the two new keys (a sibling's `SocketPathSplitWarning` test in the same file is theirs, not mine)
- `services/backend-go/cmd/backend/main.go` — HTTP server start/shutdown (+~20 lines)
- `parlay.toml.example` — 2 commented keys

NOT mine (sibling workers' concurrent work in the same tree —
left untouched): `services/core-cpp/*`,
`services/backend-go/internal/voice/{manager,voice,voice_test}.go`,
`internal/voice/connect_timeout_test.go`,
`internal/uds/uds_test.go`, the `SocketPathSplitWarning` parts
of `internal/config/`.

## Honest gaps / what remains unmeasured (needs the live VPS)

1. **No initData validation** (above). The VPS deployment must
   put the HTTP server behind a trusted reverse proxy (TLS +
   initData check) before any real user opens the Mini App.
   The backend cannot do this itself without the bot token.
2. **No WebSocket/SSE instant sync.** The locked spec wants
   instant state sync; the app polls `/api/v1/now-playing`
   every 2 s. Polling hits the real state, so nothing is faked
   — SSE is a follow-up, not a stub.
3. **Live VPS proof not done** (per the brief: sandbox first,
   VPS is the user's test afterwards). Unproven live: Mini App
   opening from a real Telegram client; `<audio>` streaming a
   real FLAC while a real group call plays; `vc.resume` /
   `vc.skip` through the Mini App against a live call (sandbox
   proves the honest 502s without a call, not the 200s with
   one); Opus bitrate ceiling (OD-1, separate track); >50 MB
   upload (§6-R8).
4. **Resume/skip success path** needs a live call: the sandbox
   proves pause (200) and volume (200, clamped) mid-stream,
   but resume-after-pause rebinds through the handlers' feeding
   map, which a raw `PlayStream` does not populate — the
   Mini App will show the backend's honest error there until
   a real play (via `vc.play`) populates it. Not a bug in this
   work; the live test will exercise the real path.

## Flags — things I could not resolve (do not guess)

1. **3 `handlers` tests fail in the working tree, 0 of them
   mine.** `TestFeedEndAdvance`, `TestTransitionAdvancesWithoutRebind`,
   `TestTransitionMismatchRebinds`. I proved the split with a
   pristine-HEAD worktree (`git worktree add`, HEAD `7b95637`,
   `refs/` symlinked):
   - `TestFeedEndAdvance` fails **at pristine HEAD too** (2/2 in
     one run) — pre-existing flake, predates all Phase 6 work.
   - The two Transition tests **pass 2/2 at pristine HEAD** and
     fail consistently in the working tree. The only working-tree
     changes on their path are the sibling's in-flight voice
     rework (`Client.mu` Mutex→RWMutex, `Connect` moved to a
     worker goroutine with a timeout). I did not touch `voice/`
     or `handlers/` and did not attempt a fix — that would
     collide with the sibling's active work. Route to the
     coordinator / Worker B.
2. **`TestYtDlpDaemonSurvivesCallerContextExpiry` flakes under
   full-suite load** (failed once at 3.01 s, passes 4/4 in
   isolation). Timing-sensitive daemon test, untouched by me.
3. **`/tmp` is a 512 MB tmpfs; it filled up** (`no space left
   on device` at link time) during repeated full-suite runs,
   failing `radio`/`recognize`/`voice` with `[build failed]`.
   Freed my own scratch and re-ran with
   `GOTMPDIR=/home/hatch/workspace/.gotmp` — all three pass.
   The final suite numbers above are from that run. (The
   `.gotmp` dir has been removed since.)
4. **Transient sibling scratch:** a sibling's
   `internal/voice/probe_connect_test.go` briefly broke
   `go test ./internal/voice/` (`use of cgo in test ... not
   supported`); it was deleted by its owner mid-run, and a
   half-written `connect_timeout_test.go` later broke the whole
   suite with a syntax error until its owner finished writing
   it. Both resolved themselves; neither is mine. The shared
   tree is live — the coordinator should expect totals to move
   while workers run concurrently.
5. **Probes deleted:** the throwaway echo/synth-join probe test
   files I used to establish the synthesized-transport join
   were deleted after use; the surviving form is
   `synthJoinFunc` in `stream_concurrent_test.go`. `git status`
   is clean of my scratch.
