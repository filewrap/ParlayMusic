# Pre-Phase 4 wiring audit — consolidated report

Date: 2026-11-03 ~18:26–19:25 IST. Base: main at `f1a7211` (Phase 3 done
and pushed). Audit coordinator + 5 workers (A startup, B config, C code,
D docs, E build+test). **Nothing pushed. All changes uncommitted in the
working tree for review. `.env` never read or touched. No Phase 4 work
started. No stubs, mocks, or placeholders added.**

Single source of truth: `FINAL-ARCHITECTURE.md` (locked 2-process
architecture: C++20 Core + Go Unified Backend, UDS, bbolt, persistent
yt-dlp daemon; no inter-service HTTP, no Mongo/Redis, no subprocess
per call, no hardcoded limits). Contracts: `contracts/ipc.json` (65
actions: 56 + 9 OD-3), `contracts/routing-table.md`,
`contracts/uds-protocol.md`.

Worker reports (detail, per worker):
- `phase3/wiring-audit-a-startup.md` — launchers/compose
- `phase3/wiring-audit-b-config.md` — config values
- `phase3/wiring-audit-c-code.md` — code greps + action registration
- `phase3/wiring-audit-d-docs.md` — docs
- `phase3/wiring-audit-e-buildtest.md` — fresh build/test/dispatch

Change set: 24 files modified, 7 untracked (`CURRENT-STATE.md`
pre-existing untracked; 5 worker reports; `services/backend-go/
Dockerfile` new). Diff: +1,113/−1,295 across the 24 tracked files.

## 0. Method note — concurrent workers

The four Wave 1 workers edited the same checkout concurrently. One
interleave is recorded: Worker C removed the `NTGCALLS_LIB` /
`TDJSON_LIB` runtime reads from `config.cpp` while Worker B was
editing the same file, and Worker B's `.env.example` follows the
end-of-audit tree. Worker E rebuilt and re-tested the final tree
after all edits landed — Go 245/0/12, core 9/10, dispatch 64/65 —
so the interleave resolved cleanly and nothing is left half-applied.

## 1. What was already correctly wired (no change needed)

- **UDS socket path** byte-identical on both sides:
  `<data_dir>/parlay-backend.sock`, `PARLAY_UDS_PATH` override on
  both, same `PARLAY_DATA_DIR` derivation (default `data`).
- **parlay.toml search path** identical: `$PARLAY_CONFIG` →
  `./parlay.toml` → `<exeDir>/parlay.toml` →
  `<exeDir>/../parlay.toml`. No `parlay.toml` exists at repo root;
  only `parlay.toml.example` (correct — a missing file is not an
  error on either side).
- **Unified-schema defaults** byte-identical in Go, C++, and the
  example: rate limit chat 5 @ 1.0/s, user 3 @ 1.0/s; timeouts
  30000/30000/10000/300000/5000 ms; voice `standard_192k`/192 kbps,
  resampler `sinc_256`; 9-source ladder
  `navidrome, soulseek, deezer, netease, jiosaavn, youtube, spotify,
  tiktok, radio`; limits 60/20/30/120.
- **Data dirs / 0700**: bot `<data>/tdlib-bot`, assistant
  `<data>/tdlib`, both forced 0700 by the core; backend
  `data/parlay.db` bbolt 0600 in a 0700 dir; socket chmod 0600.
- **Spotify redirect URI** exactly `http://127.0.0.1:8080/callback`,
  once, never `localhost`, config-driven, independent of core `PORT`.
  Nothing listens for it (manual `action:callback`) — no conflict
  with the core's own 8080.
- **Token-bucket limiter** 5 @ 1.0/s / 3 @ 1.0/s everywhere; enforced
  only in the core (correct — core is the only entry point).
- **yt-dlp daemon/venv lookup** matches disk
  (`scripts/yt_dlp_worker.py`, `scripts/yt-dlp-venv/bin/python`);
  daemon warms at backend boot (yt-dlp 2026.10.26, ntgcalls 3.0.5
  verified live by Workers A and E).
- **slskd** default `http://127.0.0.1:5030` on both sides; optional.
- **15 settings keys** present with matching names on the wire.
- **Action registration**: `contracts/ipc.json`'s 65 actions are
  byte-identical to the backend's registrations; exactly-once test
  PASS (66 registered = 65 + `ping`, each exactly once, 0 extras).
- **Routing table**: 223 data rows (95 §A + 15 §B + 101 §C + 12 §D),
  0 rows with no matching handler, 0 real handlers with no row.
- **phase3 working notes**: already correct (their "56 actions" is
  the `3d84fb6` anchor count, not a current claim; "Old impl."
  references are deliberate inventory).
- **Core's 8080**: the core's own HTTP API (`GET /healthz`, `/api/*`
  GUI surface, Phase 6 A2) — intentional, not inter-service HTTP.
  No backend-go code binds any TCP port.

## 2. Wiring mismatches fixed, file by file

### Launchers / compose (Worker A)

Every launcher was fully stale (12-service topology, ports
8001–8010/3000, Mongo/Redis, `/healthz` pre-probes). All rewritten
in place to the new engine (backend → core start order; backend
readiness = UDS socket file, core readiness = `GET :8080/healthz`):

- `start.sh` — service table now `backend` → `core` → `slskd`
  (opt-in). All 10 deleted services, ports, `*_URL` vars, and the
  Mongo/Redis probe block gone. Backend builds once on demand if no
  binary exists; runs with `WORKDIR=$ROOT`,
  `PARLAY_DATA_DIR=$ROOT/data`, `LD_LIBRARY_PATH=$ROOT/lib` (its
  baked rpath points at repo `refs/`, absent in tarballs).
- `stop.sh` — table now `core|8080`, `backend|` (PID-only),
  `slskd|5030`. Added `STILL_RUNNING` → exit 1 (old script could
  never catch the backend, which has no port).
- `status.sh` — backend = PID + socket file; core = PID +
  `/healthz`; slskd unchanged. Exit 0 only when backend + core UP.
- `install.sh` — Node dropped entirely; venv is now
  `scripts/yt-dlp-venv` with `pip install yt-dlp` (deleted the
  standalone `bin/yt-dlp` fetch and the downloaders `.venv`);
  single shared `data/`; fresh `.env` template gains
  `SPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback`; fails loudly
  if a pre-unified-engine tree/tarball is found, pointing at
  `--from-repo`.
- `docker-compose.yml` — 3 services (`backend-go`, `core-cpp`,
  `slskd` profile `full`), one shared `parlay-data` volume at
  `/data`, backend healthcheck = socket file, core `depends_on`
  backend healthy, 8080 only. No mongo/redis.
- `services/backend-go/Dockerfile` — NEW (none existed): Go 1.26
  cgo build against `refs/ntgcalls`, runtime with ffmpeg + in-image
  yt-dlp venv under `/app/scripts`, `libntgcalls.so` in `/app/lib`.
- `services/core-cpp/Dockerfile` — rewritten: the old "libtdjson NOT
  baked in, binary runs without it" is false since Phase 2
  (build-time link); runtime stage now carries the `.so` in
  `/usr/local/lib`.
- `scripts/package-release.sh` — builds `parlay-backend` +
  `parlay-core` only; stages `scripts/yt_dlp_worker.py` (venv
  created by install.sh, never bundled); new §6 `ldd` check proving
  the staged `lib/` alone resolves both binaries; `parlay.toml.
  example` now staged. Lib size pins re-verified exact
  (50166944 / 98121040).

Verification (Worker A): `bash -n` passes on all 5 scripts; compose
parses (exactly the 3 new services); `install.sh --help` exits 0;
backend + core both build green; `ldd` simulation 0 "not found";
end-to-end dry run in a scratch root with a fake `.env`: backend OK,
core OK (HTTP 200), slskd SKIPPED, stop/status/double-start guard all
correct. Scratch roots deleted.

### Config (Worker B)

- `parlay.toml.example` (+39) — added the missing `[backend]`
  table: 17 keys the Go backend parses were entirely undocumented.
  Live keys active; 9 parsed-but-unplugged keys commented with
  warnings (see §3 Q9).
- `.env.example` (86 → 88 assignment lines, full rewrite) —
  removed 24 stale vars (`MONGO_URI`, `REDIS_URL`, 11 service URLs,
  `CORE_URL`, `PARLAY_DL_DIR`, `PARLAY_VOICE_PYTHON`, `VC_PROFILE`,
  `YT_DLP`, `DEEZER_FORMAT`, 3 slskd pacing vars,
  `PARLAY_SPOTIFY_MAX_MATCH_SEC`, `PARLAY_BOT_USERNAME`); added 28
  (`PARLAY_CONFIG`, `PARLAY_UDS_PATH`, all 16 `PARLAY_*` overrides,
  `TD_BOT_SESSION_DIR`, `PARLAY_YTDLP_PYTHON/WORKER`,
  `TELEGRAM_BOT_USERNAME`, `START_IMG`/`PING_IMG`,
  `PARLAY_UPDATES_URL/SUPPORT_URL/SOURCE_URL`,
  `PARLAY_SPOTIFY_OAUTH_KEY`); corrected `SLSKD_URL` to
  `127.0.0.1:5030` and `SLSKD_DOWNLOAD_DIR` default. Mechanical
  cross-check now exact: 0 listed-but-unread vars.
- Empty `PARLAY_DATA_DIR` divergence (3 files) —
  `services/backend-go/internal/config/config.go`,
  `services/core-cpp/src/config.cpp`,
  `services/core-cpp/src/tdlib_client.cpp`: set-but-empty used to
  make Go fail validation while the core resolved session dirs to
  absolute `/tdlib`/`/tdlib-bot`. Set-but-empty now means "unset"
  (→ `"data"`) on both sides.
- `NTGCALLS_LIB`/`TDJSON_LIB` — moved to the REMOVED block in
  `.env.example` to match Worker C's removal of the dead fields/env
  reads (build-time link now; `TDJSON_LIB` survives only as a CMake
  variable).

Verification (Worker B): `go build ./...` clean; Go
config/settings/model/store tests all `ok`; C++ `config_test`
rebuilt and run against the edited example — ALL PASS, 0 failed.

### Code (Worker C)

8 items fixed in 5 files (+91/−25):

- `services/core-cpp/tests/stub_backend.cpp` — "56 actions" → 65
  (2 comment lines); added 9 canned branches for the OD-3 actions
  (stub previously answered them `not_found`).
- `services/core-cpp/tests/uds_test.cpp` — sweep `kAll56` → `kAll65`,
  assert 65/65.
- `services/core-cpp/src/config.hpp` — fixed 2 stale comments
  (shared-state "later Mongo/Redis wiring" claim; "kept until Wave 2
  removes the HTTP IPC client"); deleted dead `ntgcalls_lib` /
  `tdlib_lib` fields + false dlopen comments.
- `services/core-cpp/src/config.cpp` — deleted the 2 dead env reads
  (`NTGCALLS_LIB`, `TDJSON_LIB`).
- `services/core-cpp/src/tdlib_client.hpp` — fixed the comment that
  referenced the deleted `cfg.tdlib_lib`.

Verification (Worker C): stub + uds_test rebuilt standalone with g++
(clean); `/tmp/uds_test` against the rebuilt stub: **65/65 actions
answer, `ALL UDS TESTS PASSED (0 failures)`**, mean round-trip
137 µs; `config_test` against the edited example ALL PASS.

### Docs (Worker D)

- `README.md`, `INSTALL.md`, `CURRENT-STATE.md`, `ARCHITECTURE.md`
  — rewritten in place. All described the deleted 12-service HTTP
  topology throughout (ports, `vc-go`/`vc-ts`/`vc-rust`/
  `downloaders`/`netease-go`, Mongo/Redis, curl-CLI IPC,
  `getUpdates` long-poll, per-chat engine selection, 1 cmd/s limit,
  "56"/"51" actions). Now describe the locked 2-process engine:
  core-cpp + backend-go over UDS (`data/parlay-backend.sock`), bbolt
  `data/parlay.db`, persistent yt-dlp daemon, 65 actions,
  token-bucket limits, dual-TDLib core, Phase 3 verification numbers
  (Go 245/0/12, ctest 1,615 PASS, routing 944, 64/65 answered).
  v0.2.0 tarball marked do-not-use for fresh installs; repo-checkout
  build is the install route.
- `PORT_MAP.md` — §2 retargeted from deleted `svc:<name>` to
  verified `backend-go:<pkg>` targets; low-ram/high-perf rows marked
  deleted; §3 paths updated to `internal/resolver/soulseek.go`,
  `internal/spotify/auth.go`.
- `COMMANDS.md` — header facts fixed (65 actions, token bucket,
  dispatcher 8,880 lines, ui_texts 2,376 lines) + 12 passages
  (`/play`, `/autoplay`, `/search`, `/lyrics`, `/radio`, `/manage`,
  `/engine` retired, `/download-settings`, inline mode, `/volume`,
  `/mix`/`/recognize`/etc. actions exist but commands unrouted,
  download cancel/async, `vc.lyrics` mismatch closed, Spotify
  surfaces → `internal/spotify`, `/speed`/`/bass` stored-only).
- `RELEASE.md`, `REMAINING.md` — "Superseded note" header only;
  bodies kept verbatim as historical records.
- `phase3/resolvers.md` — 2 stale venv passages fixed
  (`services/downloaders/.venv` → `scripts/yt-dlp-venv/`).

Not edited, deliberately: `PHASE2.md` / `PHASE3.md` / `PHASE9.md` /
`BENCHMARKS.md` (dated historical records of superseded work — the
old Phase 3 report's title collides with the new Phase 3, but its
date scopes it); `phase6/smoke.sh`, `phase7/a6-native-sweep.sh`,
`audit/run-one.sh` (historical phase harnesses, not launchers).

## 3. What still points at the old architecture — needs a user decision

### Q1 — Core `Store`: delete the Mongo/Redis backends, or bless them? (THE central question; raised independently by B, C, and D)
`services/core-cpp/src/store.{hpp,cpp}` implement a hand-rolled
MongoDB wire-protocol client (OP_MSG + BSON, SCRAM) and a RESP Redis
client, instantiated in `main.cpp:565` and used dispatcher-wide
(sudoers, auth, chat settings, share targets, download picks/runs,
maintenance, admin cache). Config still reads `MONGO_URI` /
`REDIS_URL` (`config.hpp:149–150`, `config.cpp:333–334`). The locked
architecture says NO Mongo/Redis and backend-go bbolt owns state; the
core's store defaults to a JSON file when no URI is set, so it works
today with no servers — but it is a second state home, and setting
those URIs would split state away from bbolt. Worker C's reworded
`config.hpp` comment now calls Mongo/Redis "live backends when set",
which contradicts FINAL-ARCHITECTURE.md §1. Deleting them means
moving every `store_` call behind UDS to backend-go — a Phase-4-sized
job, not a grep fix.
**Exact question: delete the Mongo/Redis backends (and `scram.cpp` /
`store.cpp` Mongo/Redis clients) from the core store in Phase 4,
leaving JSON-file only — or is the core store intentionally exempt
from the no-Mongo/Redis rule?** This does not block starting Phase 4,
but Phase 4 touches state ownership, so rule on it early in Phase 4.

### Q2 — Delete the 10 dead service-URL fields + 26 dispatcher call sites, or keep as signature-compat?
`config.hpp:159–168` defaults still point at deleted services
(`http://vc-go:8001` … `http://netease-go:8010`); 26 dispatcher call
sites pass them to `ipc_.post/get` (`dispatcher.cpp:2333, 3516, 3535,
3737, 3744, 3870, 3905, 4050, 4100, 4172, 4198, 4238, 4250, 4274,
4292, 4341, 4485, 4722, 6102, 6482, 6518, 7405, 7648, 7965, 7987,
7999`). Harmless at runtime — `ipc.cpp:662,669,683` explicitly ignore
the URL — but removal is a 26-site + API-signature refactor.
**Exact question: remove the 10 URL fields, their env reads
(`config.cpp:336–345`), the summary print (`config.cpp:583`), and the
URL first-argument at the 26 call sites (plus the `IpcClient`
signatures), or keep the fields as dead signature-compat config?**

### Q3 — What should the radio-manage card show as "host"?
`dispatcher.cpp:4322` renders `cfg_.radio_url`
(`http://radio-js:8008`) as the `host` field of the user-visible
radio-manage card.
**Exact question: station name? `—` like the other placeholder
fields? Remove the field?**

### Q4 — Wire `download.cancel` / `library.delete` into the core, or keep answering "unavailable"?
`cbDl` (`dispatcher.cpp:7373–7380`) and `cbDel` (`:7731–7750`)
answer `kToastFeatureUnavailable` on the claim the actions don't
exist; both ARE registered in backend-go since Phase 3. Routing-table
rows A63 (`:127`) and A81 (`:341`) record the same stale claim.
**Exact question: wire the calls (what payloads —
`download.cancel` takes `run_id`, `library.delete` takes `id`/`path`
— what success/failure edits, and do rows A63/A81 flip to Live?) or
keep answering "unavailable"?**

### Q5 — Retarget `contracts/ipc.json` to the new topology, or keep it frozen?
52 of 65 actions carry a `"service"` field naming a deleted service;
line 956 `description` still says `"Single contract for all Parlay
services. HTTP + JSON. Every service: POST /v1/<action> … GET
/healthz"`; `"from"` provenance fields still name deleted files /
dead mechanisms (`:126, 436, 448, 679, 691, 704, 707, 744, 944`);
line 986 still says `"protocol": "HTTP/1.1 + JSON"`.
`contracts/uds-protocol.md:6` and `contracts/routing-table.md:8`
still say "56 actions" (live contract has 65).
**Exact question: retarget the 52 `"service"` fields to `backend-go`,
rewrite the line-956 description and line-986 protocol, and fix the
`"from"` fields — or keep ipc.json frozen as the Phase 9 parity
reference and record the new topology only in `uds-protocol.md`?**
(Related: should FINAL-ARCHITECTURE.md §10's parity target change
from 56 to 65 actions?)

### Q6 — Add the `ipc_socket_path` config key, or drop the note?
`services/core-cpp/src/ipc.hpp:44–45` says Worker C should add it.
The socket path is env-only today (`PARLAY_UDS_PATH`).
**Exact question: add the parlay.toml key (name + precedence vs
`PARLAY_UDS_PATH`, example entry in Worker B's file) or delete the
note and keep the socket path env-only?**

### Q7 — Settings dual-ownership: who owns them after Phase 4?
The core's settings panel reads/writes the core Store
(`dispatcher.cpp:chatSettings`/`saveChatSettings`, 12 fields); the
backend's `chat.settings` UDS action reads/writes bbolt (all 15
keys). The core never calls the backend's `chat.settings` (only a
deadline entry exists, `main.cpp:588`); the backend never writes the
core's store. A `/settings` toggle and a `chat.settings` UDS call can
diverge silently. The core's store file is `data/state.json` with
default perms (NOT forced 0700/0600 like the backend store).
**Exact question: after Phase 4, does the core panel call the
backend's `chat.settings` (backend owns settings), or does the
backend defer to the core?**

### Q8 — `autoplay_lang` canonical default: `"en"` or `""`?
`model.DefaultChatSettings()` returns `"en"`, pinned by
`model_test.go:95`; the `chat.settings` handler overrides it to `""`
(`settings.go:99-100`), pinned by `settings_test.go:45`. The
wire-visible default is `""`; any other future `GetSettings` caller
sees `"en"`.
**Exact question: which is canonical? (Fix is one line in `model.go`
+ updating `model_test.go:95`.)**

### Q9 — `play_mode` vocabulary: `"admin"` or `"admins"`?
The core writes `"admin"` / `"everyone"` (`dispatcher.cpp:4878`,
`:7262`; gate `:1099` checks `== "admin"`). The backend validates
`"everyone"` or `"admins"` only (`settings.go:169`) and rejects
`"admin"`. Harmless today (separate stores, Q7), fatal the day the
stores are unified.
**Exact question: which spelling wins?**

### Q10 — Nine `[backend]` keys parsed but never consumed: wire or drop?
`BackendConfig` fields `SlskdURL`, `SlskdUser`, `SlskdPass`,
`DeezerARL`, `NetEaseMusicU`, `NavidromeURL`, `NavidromeUser`,
`NavidromePass`, `YTDLPPath` are parsed from `parlay.toml` / env but
read by NOTHING else — the resolvers read the env vars directly.
Setting `slskd_url` / `deezer_arl` / `netease_music_u` /
`navidrome_*` in `parlay.toml` does NOTHING, while the same-named env
var works. Two further traps: `NAVIDROME_PASS` (config) vs
`NAVIDROME_PASSWORD` (what the resolver reads) are different names,
and `YTDLP_PATH` is dead in both forms (the daemon uses
`PARLAY_YTDLP_PYTHON` / `PARLAY_YTDLP_WORKER`).
**Exact question: wire the resolvers to `cfg.Backend`, or drop the 9
dead keys from the schema (loud unknown-key failure instead of silent
ignore)?**

### Q11 — Hardcoded sandbox path in the yt-dlp locator: fix the fallback?
`services/backend-go/internal/resolver/ytdlp.go:90` falls back to
`/home/hatch/workspace/parlay/scripts/yt-dlp-venv/bin/python`. It
only fires when the CWD walk-up fails, but on the VPS that path never
exists, so an out-of-tree backend start silently degrades to system
`python3` (which does NOT have `yt_dlp`) instead of finding the venv.
Mitigation is done (`PARLAY_YTDLP_PYTHON` / `PARLAY_YTDLP_WORKER`
documented in `.env.example`).
**Exact question: replace the absolute fallback with an exe-relative
lookup in `ytdlp.go`?**

### Q12 — `resolver.DataDir()` default splits backend data across two roots: align or thread cfg?
`config.Backend.DataDir` defaults to `"data"`, but
`resolver.DataDir()` defaults to `~/.local/share/parlay` when
`PARLAY_DATA_DIR` is unset (`resolver.go:443-452`). NetEase downloads
(`netease.go:150,218`) and the slskd download-dir default land under
`~/.local/share/parlay` while the store, socket, and all other
downloads live under `./data`.
**Exact question: align `resolver.DataDir()`'s fallback to `"data"`,
or thread `cfg` into the resolvers?**

### Q13 — Next release version?
`VERSION` is still v0.2.0 and `release/` still holds the old v0.2.0
tarball (12-service tree). `install.sh` now fails loudly on it.
**Exact question: what version should the first unified-engine
release be (v0.3.0?), and should the old v0.2.0 release asset be left
as-is?** (Worker A did not bump `VERSION` or run package-release
end-to-end — it would overwrite the v0.2.0 artifact under the same
name.)

### Q14 — `bin/` is not gitignored
`start.sh` (repo layout) builds the backend into
`$ROOT/bin/parlay-backend`; `bin/` is neither tracked nor ignored.
**Exact question: add `bin/` to `.gitignore`, or build the
repo-layout backend to a gitignored path instead?**

### Q15 — COMMANDS.md full-table pass: who verifies the rest?
Only the header facts and 12 plainly-wrong passages were fixed. The
rest of the 255-line Implements column was written against the old
engines.
**Exact question: should Worker C (code) do a code-grounded pass over
the whole COMMANDS.md table, or is the current header-plus-fixes
level acceptable until Phase 4?**

### Smaller items (no user decision strictly needed; recorded for Phase 4)
- `stop.sh`/`status.sh` hardcode core port 8080 while `start.sh`
  honours `${PORT:-8080}` — non-8080 core ports would confuse them.
  Needs a small design call in Phase 4 if non-8080 ports must work.
- Nits (one line each, Worker C scope): `spotify/crypto.go:53,56`
  OAuth-key error names `SPOTIFY_OAUTH_KEY` but the real var is
  `PARLAY_SPOTIFY_OAUTH_KEY`; `Config::missingRequired()`
  (`config.hpp:265`) is dead code; whitespace-only `PARLAY_DATA_DIR`
  unguarded in the core.
- `phase6-mongo-data/` — git-ignored local Mongo data dir, leftover,
  not wiring.
- No live Docker verification (Docker not in the sandbox) and no live
  VPS install run — both need a real host (see §4).
- One transient `cmake --build` failure (Error 2, no error lines)
  during Worker A's verification; immediate re-run clean — most
  likely a concurrent edit by another worker. Worker E's rebuild +
  full ctest afterwards was green, so this is closed as an artifact
  of the concurrent audit, not a tree problem.

## 4. Verified build / test / dispatch counts (Worker E, fresh runs on the final tree)

All live runs used dummy creds only (`env -i`).

- **Go backend** (vendored `.tools/go/bin/go` 1.26.0; system go is
  1.23.0 and is rejected by `go.mod` — do not use it):
  `go vet ./...` clean, exit 0, zero findings; `gofmt -l` zero
  unformatted files; `go test ./...` **25/25 packages ok, 0 FAIL**;
  verbose **245 passed / 0 failed / 12 skipped** — identical to the
  Phase 3 baseline (12 skips are the live-gated set);
  `TestRegisterAllExactlyOnce` **PASS** (65 actions + ping, each
  exactly once).
- **Core-cpp** (rebuilt first): `ctest` **9/10**, only `store_test`
  failing. **1,614 PASS lines** vs baseline 1,615 — the 1-line delta
  is a proven timing flake in `dispatcher_test` (277–278 PASS across
  5 direct runs, **0 FAIL lines every time**; the sometimes-absent
  line is `PASS play reply audio, backend refuses: vc.play_file
  issued`). Per-suite: uds 40, config 55, rate_limiter 28,
  dispatcher 277, routing 944, tdlib 46, telemetry 5, ui 160, scram
  40, store 19. The only 2 FAIL lines are the pre-existing env
  failures (`mongo connect: cannot connect to mongodb at
  127.0.0.1:27099`, `store backend=mongo (got file)`); redis is a
  SKIP, not a failure. Mongo/Redis are down in the sandbox — not a
  regression.
- **Dispatch: 64/65 answered.** Real backend (dummy creds, yt-dlp
  daemon warm, v2026.10.26) + real `IpcClient` via
  `tests/interop/core_peer.cpp`. Only `vc.join` failed — TRANSPORT
  FAILURE, same as Wave 2: the join handler ran and answered the fake
  SDP, then blocks in ntgcalls `Connect` with no response within 60 s
  (needs a real group call, Phase 6). No action answered `unknown
  action`. Honest answers observed: `chat.settings` / `gui.queue` /
  `search.inline` / `queue.vote` / `share.create` / `spotify.*` /
  `vc.stop` / `library.scan` / `cache.clear` / `download.prefs` ok;
  `audio.recognize` `engine_unavailable` (no `AUDD_API_TOKEN`);
  `library.sharescan` `engine_unavailable` (slskd down).
  Full log: `/tmp/audit-e/core_peer.log` (scratch, not committed).
- **Ports:** `ss -tlnp` zero TCP listeners; probes to 8001–8010,
  3000, 8080 all closed. 8080 confirmed in source as the core's own
  HTTP API — nothing listens on it because the core isn't running,
  which is expected.
- **TODO/FIXME/HACK/Unimplemented:** **0 hits** in backend-go Go
  files and core-cpp src+tests (excluding `refs/`, `.tools/`,
  `scripts/yt-dlp-venv/`).

## 5. Known open items from Phase 3 verification — current state (NOT fixed, per orders)

- **docker-compose.yml + install/start/stop scripts stale topology:
  RESOLVED by this audit (Worker A).** All rewritten to the new
  engine; verified by syntax checks, compose parse, and an end-to-end
  scratch dry run. Remaining on this item: the release-version
  question (Q13) and no live Docker/VPS verification.
- **`vc.join` Connect needs a context/timeout: STILL OPEN,
  unchanged.** `internal/voice/voice.go:312` `Connect(chatID,
  remoteSDP)` takes no context; `internal/voice/manager.go:249`
  calls it with only a pre-check `ctx.Err()`. Re-proven live by the
  dispatch probe (60 s, no response frame). A stuck Connect still
  leaks the handler goroutine and leaves the call `StateJoining`.
- **Download runs/prefs/library index are in-memory singletons:
  STILL OPEN, unchanged.** `internal/download/registry.go:95–113`
  (process-wide `sharedRegistry`), `internal/download/prefs.go:55–69`
  (`sharedPrefs`, "in-memory"), `internal/library/library.go:45–53`
  (map-based `Index`); store buckets are only `rooms, queue, votes,
  shares, spotify, settings, radio` (`internal/store/store.go:30–41`)
  — no `downloads`/`library`/`dlprefs` buckets. A restart still loses
  finished-run snapshots and resets prefs.
- **`fx` persists via store RoomState directly: STILL THE CASE.**
  `internal/fx/fx.go:57` holds `store *store.Store`;
  `fx.go:144,154` `GetRoom`/`PutRoom` in `Apply`; zero references to
  `room.Manager`. Same staleness shape the vote/gui seams had before
  Wave 2 rewired them.
- **Inventory open questions (§10):** Q1 (`np` oversize 50 MB vs
  2 GB — current behaviour 50 MB in core send path
  `dispatcher.cpp:4147` and backend `MaxBotAPIBYTES`), Q2
  (`spotify.ignore` — `ignored_by` hardcoded `[]int64{}`,
  `handlers.go:578–580`), Q3 (autoplay_lang/mood defaults —
  implemented per proposal as `""`, `settings.go:22`, user
  confirmation outstanding), Q5 (`vc.queue` `move` verb — rejected
  outside `add|remove|clear|list`, `handlers/vc.go:363`) — **still
  open decisions.** Q6 (speed/bass carrier) and Q7 (`ntg_time` vs
  wall-clock) are **resolved by the code** — `vc.play` carries
  speed/bass into the room (`handlers/vc.go:51–52,104–110`,
  `ipc.json:771–772`); position is wall-clock (`room.go:15,206–210`),
  `Manager.Time` has zero callers — only the inventory wording is
  stale.
- **Credentials still needed (names only, no values):** BOT_TOKEN,
  API_ID/API_HASH, PARLAY_TOKEN, SPOTIFY_CLIENT_ID/SECRET (+ optional
  PARLAY_SPOTIFY_OAUTH_KEY, auto-generated if unset), AUDD_API_TOKEN,
  GEMINI_API_KEY, GENIUS_ACCESS_TOKEN, MUSIXMATCH_API_KEY,
  LEMONIC_API_KEY (optional), NAVIDROME_URL/USER/PASSWORD (only if
  the user runs Navidrome), SLSKD_API_KEY (only if slskd auth
  enabled), TD_PHONE/CODE/PASSWORD (fresh first-auth only). All go in
  the VPS `.env` by the user, never via chat. Deezer ARL dropped,
  NetEase MUSIC_U skipped, per user rulings.

## 6. Verdict — is Phase 4 clear to start?

**Yes — Phase 4 is clear to start.** Nothing found in this audit
blocks it:

- The tree builds green on both sides (Go 25/25, 245/0/12, vet +
  gofmt clean; core 9/10 with only the 2 known env failures).
- Dispatch is 64/65 with the single miss being the known sandbox
  limit (`vc.join` fake-SDP Connect), not a wiring gap.
- No HTTP listener on any deleted-service port; 0 TODO/FIXME/HACK/
  Unimplemented hits; 65 actions registered exactly once; routing
  table 223 rows with zero linkage gaps.
- The startup path, config surface, code, and docs now all point at
  the locked 2-process architecture. The one item from the Phase 3
  verification list that was a wiring problem (stale launchers +
  compose) is resolved.

Caveats to carry into Phase 4 (not blockers):

1. **Rule on Q1 (core Store Mongo/Redis) early in Phase 4.** Phase 4
   is cache + prefetch — it touches state ownership, and whether the
   core keeps its own store (JSON-file only or with Mongo/Redis
   backends) shapes that work.
2. **Fix the `vc.join` Connect timeout before any live voice work
   in Phase 6** (a stuck Connect leaks a goroutine).
3. **Decide the in-memory singletons during Phase 4** — download
   runs/prefs/library index need store buckets if restarts must not
   lose them; that decision belongs to the cache work.
4. **User decisions Q2–Q5, Q7–Q10, Q13–Q15** remain open (see §3);
   Q6/Q7 need only inventory wording updates.
5. **No live Docker or VPS verification** was possible from the
   sandbox — the rewritten compose/Dockerfiles and install.sh need a
   real host run before trusting them.
