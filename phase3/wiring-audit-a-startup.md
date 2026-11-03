# Pre-Phase 4 wiring audit — Worker A: startup wiring

Date: 2026-11-03. Base: main at f1a7211 (Phase 3 done + pushed).
Scope: launchers only. No Phase 4 work, nothing pushed, `.env` untouched.
Single source of truth: FINAL-ARCHITECTURE.md (2-process engine:
C++20 core + Go unified backend, UDS, bbolt, persistent yt-dlp daemon,
TS Mini App later; NO inter-service HTTP, NO Mongo/Redis, NO subprocess
per call, NO hardcoded limits).

## Verdict up front

Every launcher was stale: `start.sh`, `stop.sh`, `status.sh`,
`docker-compose.yml`, `install.sh`, `scripts/package-release.sh` and
`services/core-cpp/Dockerfile` all still described the deleted
12-service topology (ports 8001–8010/3000/8080, Mongo/Redis,
`/healthz` pre-probes, per-service `PORT`). All seven were rewritten
in place to the new engine. One new file was needed for the compose
path: `services/backend-go/Dockerfile` (the backend had no Dockerfile).

## What the new engine actually is (grounded in the tree)

- **Backend** `services/backend-go/cmd/backend` → binary
  `parlay-backend`. No HTTP port. Listens on UDS
  `<data_dir>/parlay-backend.sock` (default `data_dir` is `data`,
  relative to CWD; `PARLAY_DATA_DIR` / `PARLAY_UDS_PATH` override it —
  `services/backend-go/internal/config/config.go`). Store is
  `<data_dir>/parlay.db`. Spawns the yt-dlp daemon itself at boot and
  warms it (up to 30 s) before listening; locates the daemon python by
  walking up from its **working directory** for
  `scripts/yt-dlp-venv/bin/python` and `scripts/yt_dlp_worker.py`
  (`internal/resolver/ytdlp.go`; overrides `PARLAY_YTDLP_PYTHON` /
  `PARLAY_YTDLP_WORKER`). So the backend MUST run with CWD = install
  root in both layouts. Verified live: a scratch build listened on
  `/tmp/pl-test/parlay-backend.sock` (0600) in ~1 s, daemon warm,
  yt-dlp 2026.10.26, ntgcalls 3.0.5.
- **Core** `services/core-cpp/build/parlay-core` (prebuilt in-tree).
  Dual TDLib sessions: bot `data/tdlib-bot` (`TD_BOT_SESSION_DIR`),
  assistant `data/tdlib` (`TD_SESSION_DIR`); the core creates both
  mode 0700 itself (`tdlib_client.cpp:ensureSessionDir0700`). HTTP API
  still on `PORT`, default **8080**, `GET /healthz` answers even before
  bot authorization (`main.cpp`, `core_api.cpp:64`) — so 8080 is still
  a real core port, and it is the ONLY TCP port the engine owns.
  libtdjson is linked at build time; the binary's RUNPATH covers
  `$ORIGIN/../lib` + `$ORIGIN/lib` (tarball `lib/`). The core connects
  to the backend socket at startup, so start order is backend → core.
- **slskd** stays optional on :5030 (backend default
  `SLSKD_URL=http://127.0.0.1:5030`; compose service name `slskd`).
  Binary source unchanged: `bin/slskd`, fallback
  `phase5/slskd/slskd-bin/slskd`.
- **Spotify redirect URI** is exactly `http://127.0.0.1:8080/callback`
  (user's Spotify app). Nothing listens on 8080 for it: the backend's
  OAuth callback is manual (`action:callback` with the returned code,
  `internal/spotify/auth.go`). The 8080 in the URI therefore does NOT
  conflict with the core's HTTP port — no listener is bound for OAuth.
- **PARLAY_TOKEN** is still read by the core (startup warning only,
  `config.cpp:335`); the backend does not read it (UDS socket
  permissions are the access control). Kept in install.sh's generated
  `.env` and start.sh's warning.
- **ffmpeg** is still required (backend voice feeder spawns one
  persistent ffmpeg per room; core Opus pipeline). **Node is not**:
  no JS service remains in the engine (Mini App is Phase 6, not in
  the tree). **Go toolchain**: `go.mod` requires Go ≥ 1.26;
  `.tools/go` in the repo is 1.23.0 and resolves via
  `GOTOOLCHAIN=auto` (auto-downloaded 1.26.10 per
  `phase3/resolvers.md`).

## File-by-file changes

### `start.sh` — rewritten (was: 12 services, `/healthz` probes, Mongo/Redis pre-probes)
- Service table is now `backend` (socket) → `core` (http) → `slskd`
  (http, opt-in). All 10 deleted services, ports 8001–8010/3000, all
  `*_URL` env vars, and the Mongo/Redis `probe_uri` block are gone.
- Backend readiness = UDS socket file present (60 s budget: the
  backend warms the yt-dlp daemon up to 30 s before it listens) + PID
  alive. Core readiness = `GET 127.0.0.1:${PORT:-8080}/healthz`
  (30 s) + PID alive. slskd unchanged semantics (any HTTP response).
- Backend resolve: tarball `bin/parlay-backend`; repo: existing
  `bin/parlay-backend`, else **build once** with the Go toolchain
  (`go build -trimpath -o $ROOT/bin/parlay-backend ./cmd/backend`,
  WORKDIR `services/backend-go`) — mirrors the old script's `go run`
  fallback style, but caches a binary. Runs with `WORKDIR=$ROOT`
  (yt-dlp venv/worker discovery) and `PARLAY_DATA_DIR=$ROOT/data`,
  plus `LD_LIBRARY_PATH=$ROOT/lib` (the backend binary's baked rpath
  points at the repo's `refs/ntgcalls` dir, which does not exist in a
  tarball; `lib/libntgcalls.so` is the fallback — verified by `ldd`,
  see Verification).
- Core resolve: tarball `bin/parlay-core`, else
  `services/core-cpp/build/parlay-core`; missing binary fails with
  the exact cmake build command. No `*_URL`, `TDJSON_LIB`,
  `NTGCALLS_LIB` envs any more (build-time link / backend-owned).
  `PARLAY_DATA_DIR=$ROOT/data`, `WORKDIR=$ROOT` (parlay.toml
  discovery). Session dirs are NOT pre-created — the core makes them
  0700 itself.
- `mkdir -p` now only `run logs data data/downloads` (single shared
  data dir; downloads land in `<data_dir>/downloads` per
  `internal/download/download.go:115-118`).
- Final line reports the socket path + core `/healthz` URL (no GUI
  :3000).

### `stop.sh` — rewritten (was: 13 services × ports)
- Table is now `core|8080`, `backend|` (PID only — no TCP port),
  `slskd|5030`, reverse order. Port-owner fallback (`ss`) applies to
  core/slskd only. Added `STILL_RUNNING` tracking: a PID that survives
  SIGKILL now exits 1 (the old script only failed on bound ports,
  which could never catch the backend). Stale-socket note: the next
  start removes a stale `data/parlay-backend.sock` (`uds.Listen`).

### `status.sh` — rewritten (was: 13 × `/healthz`)
- Backend: PID alive + socket file present (`test -S`). Core: PID
  alive + `GET :8080/healthz`. slskd: unchanged (SKIPPED when not
  enabled). Exit 0 only when backend + core are UP.

### `install.sh` — stale sections rewritten (was: 12-service dirs, downloaders venv, Node, yt-dlp CLI binary)
- Header now describes the 2-process engine.
- Dropped the Node requirement entirely (no `nodejs` pkg, no
  NodeSource Node 20 install, no version check) — no JS service
  remains.
- `is_checkout`: repo = `services/backend-go` + `services/core-cpp`;
  tarball = `bin/parlay-core` + `bin/parlay-backend` (was: compose
  file or `bin/parlay-core`). The post-download check now **fails
  loudly** if the tree predates the unified engine (old tarball has
  `bin/parlay-core` but no backend) with a `--from-repo` pointer.
- Clone note now gives the exact core + backend build commands.
- Data dirs: single shared `data`, `data/downloads`, `data/slskd`
  (was: one dir per deleted service + `data/netease`).
- Venv: creates **`scripts/yt-dlp-venv`** and `pip install yt-dlp`
  into it (was: root `.venv` + deleted `services/downloaders/
  requirements.txt`). Verifies with `import yt_dlp`.
- Deleted the standalone `bin/yt-dlp` download section — the engine
  has no yt-dlp CLI path; the daemon imports the module from the venv.
- Fresh `.env` template gains
  `SPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback` (exact value).
  Existing `.env` files are still never overwritten. `PARLAY_TOKEN`
  generation kept (core still reads it).
- Unchanged: OS check, apt deps (minus node), slskd fetch, `.env`
  prompting/validation, `chmod 600`.

### `docker-compose.yml` — rewritten (was: 12 services + mongo + redis)
- Three services: `backend-go`, `core-cpp`, `slskd` (profile `full`).
  No mongo/redis services or volumes, no `*_URL` vars, no 8001–8010
  / 3000 mappings.
- One shared volume `parlay-data` mounted at `/data` in both
  containers; both set `PARLAY_DATA_DIR=/data` (the socket path must
  agree on both sides). Core publishes 8080 only. Backend healthcheck
  is `test -S /data/parlay-backend.sock`; core `depends_on`
  backend-go `service_healthy` + `/healthz` healthcheck.
- Credentials via `env_file: .env`. `parlay.toml` is deliberately NOT
  mounted (a missing file is not an error; mounting a nonexistent
  file would break `up`) — the file documents the override-file route.
- Build contexts are the repo root (`dockerfile:
  services/<svc>/Dockerfile`) because both Dockerfiles need files
  outside their service dir.

### `services/backend-go/Dockerfile` — NEW (the backend had none)
- Multi-stage: `golang:1.26-bookworm` build (needs
  `refs/ntgcalls/shared-output` for the cgo link — copied from the
  root context), `debian:bookworm-slim` runtime with python3 +
  ffmpeg, `libntgcalls.so` → `/app/lib` + `LD_LIBRARY_PATH=/app/lib`,
  `scripts/yt_dlp_worker.py` → `/app/scripts/` and a
  `/app/scripts/yt-dlp-venv` built in-image (`pip install yt-dlp`),
  so the backend's CWD-based discovery (`/app`) finds both.
  Healthcheck: socket file. `CMD ["parlay-backend"]`.

### `services/core-cpp/Dockerfile` — rewritten (was: "libtdjson NOT baked in, mount it, binary runs without it")
- That claim is now false (Phase 2 build-time link; the binary cannot
  start without the `.so`). Build stage copies
  `phase4/tdlib-src/build/libtdjson.so.1.8.69` from the root context
  (the CMakeLists default path) and the runtime stage carries it in
  `/usr/local/lib` (binary RUNPATH `$ORIGIN/../lib`, binary in
  `/usr/local/bin`). Build context is the repo root. `curl` kept for
  the healthcheck; the stale `src/http.cpp shells to curl` comment is
  gone. Proxy `ARG`s kept (Phase 7 A6).

### `scripts/package-release.sh` — rewritten build sections
- §2 vc-go + netease-go → **backend-go**: `CGO_ENABLED=1 go build
  -trimpath -o $STAGE/bin/parlay-backend ./cmd/backend`. Cargo
  requirement removed (no Rust service ships; audio-fx-rust is a
  Phase 5 port source, not a release artifact).
- §3 Rust services, §5 JS services, §6 downloaders: deleted.
  Replaced by §4: stage `scripts/yt_dlp_worker.py` → `$STAGE/scripts/`
  (venv is created by install.sh, never bundled).
- §4 native libs kept verbatim (size pins re-verified still exact:
  libtdjson 50166944, libntgcalls 98121040).
- §5 root files now also copies `parlay.toml.example`. Staging dirs
  are `bin lib scripts` (no `services/`).
- NEW §6 runtime-link check: `LD_LIBRARY_PATH=$STAGE/lib ldd` on
  both staged binaries must show zero `not found` (proves the
  tarball's `lib/` alone suffices: core via RUNPATH, backend via the
  `LD_LIBRARY_PATH` start.sh exports).
- Secret sweep kept; the `.env.example` awk allowlist in §7(c) still
  mentions `mongodb://` / `redis://` as *allowed placeholder shapes* —
  that is Worker B's file content, not a topology reference; left
  untouched (flagged, not fixed).

## Not launchers — left alone (out of scope)

- `phase6/smoke.sh`, `phase7/a6-native-sweep.sh`, `audit/run-one.sh`:
  historical phase harnesses that name the deleted services; they are
  not launchers for the new engine and were not edited.
- `.env.example`, `parlay.toml.example`, `README.md`, `INSTALL.md`:
  Worker B's config/docs territory (`.env.example` still lists
  Mongo/Redis + all 12 service URLs — it needs Worker B's rewrite;
  noted, not touched). `.env` itself was never read or written.
- No other files edited. `git status` shows pre-existing
  modifications from other workers (e.g. `.env.example`,
  `INSTALL.md`, `README.md`, `services/core-cpp/src/tdlib_client.hpp`,
  backend `config.go`); mine are only the files listed above.

## Verification performed

- `bash -n` passes on `install.sh`, `start.sh`, `stop.sh`,
  `status.sh`, `scripts/package-release.sh`. `install.sh --help`
  exits 0. `docker-compose.yml` parses as YAML; services are exactly
  `backend-go`, `core-cpp`, `slskd`.
- Backend build proven: `go build -trimpath -o <bin> ./cmd/backend`
  succeeds with the repo's Go toolchain (1.23.0 + GOTOOLCHAIN=auto →
  1.26); the built binary started standalone, created
  `<data_dir>/parlay-backend.sock` + `parlay.db`, warmed the yt-dlp
  daemon (2026.10.26), and listened.
- Core build: `cmake --build services/core-cpp/build` is green now
  (see transient failure note below); binary timestamp 13:12 IST.
- `ldd` simulation of the new package-release §6 check: with
  `LD_LIBRARY_PATH` pointed at a directory holding only the two staged
  `.so` files, both binaries report **0 `not found`**.
- End-to-end dry run of the new `start.sh` / `status.sh` / `stop.sh`
  in a scratch root (`/tmp/fake-parlay`, fake `.env`, symlinked core
  binary + yt-dlp venv, real backend binary — the real repo `.env`
  was never used): backend `OK` (socket), core `OK` (`/healthz`
  HTTP 200), slskd `SKIPPED`, `status.sh` exit 0 with both `UP`,
  `stop.sh` stopped both (exit 0), `status.sh` then exit 1 with both
  `DOWN`, double-start refused with "already running", restart after
  stop succeeded. Scratch root deleted afterwards.
- Grep sweep of all edited files: no remaining references to ports
  8001–8010/3000, Mongo/Redis, or any of the 10 deleted service dirs,
  except the §7(c) awk allowlist noted above.

## Could NOT fix / needs a user decision

1. **Next release version.** `VERSION` is still `v0.2.0` and
   `release/` still holds the old v0.2.0 tarball (12-service tree).
   `install.sh`'s default tarball mode still points at
   `parlay-v0.2.0-linux-x86_64.tar.gz`; I made it fail loudly if that
   old tree is downloaded, but a fresh VPS install currently has no
   working tarball route until a release is cut from the unified tree.
   **Question: what version should the first unified-engine release
   be (v0.3.0?), and should the old v0.2.0 release asset be left as-is?**
   I did not bump `VERSION` and did not run `package-release.sh`
   end-to-end (it would overwrite the v0.2.0 artifact under the same
   name) — that run belongs with the version decision.
2. **`bin/` is not gitignored.** `start.sh` (repo layout) builds the
   backend to `$ROOT/bin/parlay-backend`; `bin/` is neither tracked
   nor ignored, so that build leaves an untracked file in the repo.
   (`.gitignore` is config territory — Worker B's call — but the
   question is: **should `bin/` be added to `.gitignore`, or should
   the repo-layout backend build target a gitignored path instead?**)
3. **No live Docker verification.** Docker is not installed in the
   sandbox, so `docker compose config` / `up` for the rewritten
   compose file and both Dockerfiles was never run — same caveat the
   old file carried. Needs a Docker host before trusting it.
4. **No live VPS install run.** `install.sh` was syntax-checked and
   `--help`-checked only; the apt/venv/slskd path was not executed
   (would download + need root). Needs a VPS run.

## Ambiguities / things I could not ground

- **Transient core build failure.** During verification,
  `cmake --build services/core-cpp/build --parallel` failed once with
  `Error 2` (no error lines captured — the parallel output was
  truncated); an immediate re-run was clean (exit 0, no errors) and a
  later full build reached 100%. The working tree has concurrent
  modifications to core sources (other workers), so the likeliest
  cause is a mid-build edit, not a launcher problem. Not fixed (code
  is Worker C's scope); flagged so the parent can confirm the core
  tree is green before cutting a release.
- **Core `PORT` in `stop.sh`/`status.sh`.** Both hardcode 8080 for the
  core's port check while `start.sh` honours `${PORT:-8080}`; if a
  user sets `PORT` in `.env`, `start.sh` starts the core on that port
  but `stop.sh`/`status.sh` probe 8080. I kept 8080 in stop/status
  (they don't source `.env` by design — status.sh sources it only for
  the slskd gate) rather than inventing a new behaviour. If non-8080
  core ports must be supported, that needs a small design call.
- **`.env.example` still advertises the old world** (Mongo/Redis,
  12 service URLs, `PORT=8080` per-service list) and
  `parlay.toml.example` has no `[backend]` table (backend keys exist
  only in code + `SPOTIFY_REDIRECT_URI` in the new install.sh
  template). Both are Worker B's files; the launchers do not depend on
  them, but a fresh install's docs will contradict the scripts until
  Worker B lands.
- **`audio-fx-rust` is started by nothing.** Confirmed: no backend
  code references it and no launcher starts it now (the Phase 2/3
  flag). It remains in-tree only as the Phase 5 port source. No
  action taken; Phase 5 owns it.
