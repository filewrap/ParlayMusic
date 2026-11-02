# Parlay audit — Worker B: builds + tests

Audit-only run. No service code was modified; no pushes made; no tokens requested.
Run: 2026-11-02 ~19:53–19:56 UTC (2026-11-03 ~01:23–01:26 IST).
Raw logs: `~/workspace/parlay/audit/logs/` (one file per service, plus `mongod.log`, `redis.log`, `redis2.log`).

## Toolchains used

`go` and `cargo` are NOT on the default PATH. Used via explicit PATH export:

- Go: `~/workspace/parlay/.tools/go/bin/go` — go1.23.0
- Rust: `~/.cargo/bin/cargo` — cargo/rustc 1.99.1
- g++ 13.3.0, cmake 3.28.3, node v24.22.1, python 3.12.3 (downloaders `.venv`), ffmpeg present
- mongod/redis were **not running** at audit start. Started the Phase 4 binaries for
  verification (`~/workspace/parlay/.tools/mongodb/bin/mongod` :27017,
  `~/workspace/parlay/phase4/redis-build/redis-stable/src/redis-server` :6399),
  verified PONG, and stopped both afterwards (`SHUTDOWN NOSAVE` / `--shutdown`).
  No shared DBs were killed — none were running before the audit.

## Summary table

| Service | Build | Build exit | Tests | Pass | Fail | Skip | Warnings | Duration |
|---|---|---|---|---|---|---|---|---|
| core-cpp | `cmake -S . -B build && cmake --build build -j4` (incremental) + clean build in `/tmp/parlay-audit-core-build` | 0 / 0 | `tdlib_test` 7/7; `store_test` 40/40 | 47 | 0 | 0 | 8 | clean build 43 s; tdlib compile+run 12 s; store compile+run 12 s |
| vc-go | `go vet ./... && go build ./...` | 0 | `go test ./... -count=1` (+ `-v` recount) | 18 | 0 | 0 | 0 | 24 s |
| vc-ts | n/a (stdlib-only Node, no build step) | n/a | `npm test` (`node --test tests/run.js`) | 24 | 0 | 0 | 0 | 56 s |
| vc-rust | `cargo test --release --offline` (builds as part of test) | 0 | same | 17 | 0 | 0 | 0 | 17 s |
| audio-fx-rust | `cargo build --release --offline` | 0 | `cargo test --release --offline` | 0 (no tests defined) | 0 | 0 | 0 | 1 s |
| share-rust | `cargo build --release --offline` | 0 | `cargo test --release --offline` | 0 (no tests defined) | 0 | 0 | 0 | 1 s |
| queue-rust | `cargo build --release --offline` | 0 | `cargo test --release --offline` | 0 (no tests defined) | 0 | 0 | 0 | 1 s |
| spotify-rust | `cargo test --release --offline` | 0 | same | 8 (see caveat) | 0 | 0 | 0 | 1 s |
| gui-js | n/a (stdlib-only Node, no build step) | n/a | `npm test` | 7 | 0 | 0 | 0 | 29 s |
| radio-js | n/a (stdlib-only Node, no build step) | n/a | `npm test` | 7 | 0 | 0 | 0 | 26 s |
| downloaders | n/a (no build; `.venv` Python) | n/a | `.venv/bin/python -m unittest discover -s tests -v` | 13 | 0 | 8 | 0 | 3 s |
| netease-go | `go vet ./... && go build ./...` | 0 | `go test ./... -count=1` | 5 | 0 | 0 | 0 | 23 s |

**Totals: 0 build failures, 0 test failures across all 12 services.**
Exercised tests: 47 + 18 + 24 + 17 + 8 + 7 + 7 + 13 + 5 = **146 passed, 0 failed, 8 skipped**
(audio-fx/share/queue have no test suites — 0 tests each.)

## Per-service detail

### core-cpp — PASS (build 0, tdlib 7/7, store 40/40, 8 warnings)

- Incremental build in existing `build/`: exit 0, `[100%] Built target parlay-core`.
  (Note: the first log line shows `/usr/bin/time: No such file or directory` — the
  `time -v` wrapper in my ad-hoc command did not exist; the build itself ran and
  passed. A clean from-scratch configure+build was then done properly, see below.)
- Clean build: `cmake -S . -B /tmp/parlay-audit-core-build && cmake --build
  /tmp/parlay-audit-core-build -j4` — configure 0.3 s, build 43 s, exit 0.
  **8 warnings**, all `-Wmaybe-uninitialized` false positives inside libstdc++
  `std::variant`/`std::map` internals when copying `parlay::Json` (bool/double/
  long/map members, 2 each) — matches the README's prior claim of 8 warnings, none
  in service code.
- `tests/tdlib_test.cpp` — compiled with
  `g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tests/tdlib_test.cpp
  src/tdlib_client.cpp src/voice_call.cpp src/json.cpp src/config.cpp -o /tmp/tdlib_test -ldl`
  (compile exit 0, 0 warnings), run exit 0, **7/7 PASS** (`ALL PASS: 0 failed`):
  real TDLib 1.8.69 `getOption(version)`, real 400 error for `getChat` without auth,
  honest `authorize()` failure without credentials, no-tdlib voice-call refusal.
- `tests/store_test.cpp` — compiled per README (`g++ -std=c++20 -Wall -Wextra -I src
  tests/store_test.cpp src/store.cpp src/json.cpp src/config.cpp -o /tmp/store_test`),
  run against live mongod :27017 + redis :6399:
  **40 PASS / 0 FAIL / 0 SKIP, `ALL PASS`, exit 0** (BSON round-trip, Mongo
  connect/ping/upsert/find/delete, Redis kv/set/hash, store backend=mongo + backend=file,
  file persistence across instances).
  - Prior claim was 30 PASS / 2 FAIL (mongod down on :27099). The 2 old failures were
    environmental and are gone with the DBs up; the count is now 40 because the Redis
    checks run instead of failing/skipping.
  - First run of this audit showed 29 PASS + 1 SKIP (`SKIP redis tests (not up)`):
    that skip was an artifact of my own session cleanup killing the redis process I
    had just started. Redis was restarted (verified `+PONG`) and the test re-run:
    40/40, 0 skips. Full output in `logs/core-cpp-store-test-rerun.log`.

### vc-go — PASS (vet+build 0, 18/18, 0 warnings, 24 s)

- `go vet ./... && go build ./... && go test ./... -count=1` — exit 0.
- `go test -v -count=1 ./...` recount: **18 PASS, 0 FAIL, 0 SKIP**
  (queue 14, resolver 2, store 2). `TestMongoStoreRoundTrip` ran **live** against
  mongod :27017 (PASS) — it only skips when no mongod answers.
- Matches the README claim of 18 tests.

### vc-ts — PASS (24/24, 0 warnings, 56 s)

- `npm test` (`node --test tests/run.js`) — exit 0.
- `tests 24, pass 24, fail 0, cancelled 0, skipped 0`. Live-network tests hit real
  JioSaavn/LRCLib/yt-dlp; Lemonic tests hit a local stub. Self-spawns servers on
  ports 18102–18109, data under `/tmp/p4-vcts/`.

### vc-rust — PASS (17/17, 0 warnings, 17 s)

- `cargo test --release --offline` — exit 0, `17 passed; 0 failed; 0 ignored`.
  Includes the Opus/RTP proof tests and the UDP-EPERM probe test. Matches the
  claimed 17/17.

### audio-fx-rust — BUILD PASS, no test suite (0 tests, 0 warnings, 1 s)

- `cargo build --release --offline` — exit 0 (`Finished release profile`).
- `cargo test --release --offline` — exit 0, `running 0 tests`. No `#[test]` in `src/`.
  The README claims only a build for this service, which is accurate.

### share-rust — BUILD PASS, no test suite (0 tests, 0 warnings, 1 s)

- Same as audio-fx-rust: build exit 0, `cargo test` runs 0 tests. No `#[test]` in `src/`.

### queue-rust — BUILD PASS, no test suite (0 tests, 0 warnings, 1 s)

- Same as audio-fx-rust: build exit 0, `cargo test` runs 0 tests. No `#[test]` in `src/`.

### spotify-rust — PASS (8/8, 0 warnings, 1 s) — with a caveat

- `cargo test --release --offline` — exit 0, `8 passed; 0 failed`.
- **Caveat:** 2 of the 8 (`live_spotify_api_track`, `live_spotify_api_playlist`)
  early-return with `eprintln!("skipping live Spotify API test (no
  SPOTIFY_CLIENT_ID/SECRET in env)")` when no credentials are set — they pass as
  no-ops and are **not** counted as skipped by the harness. Real exercised tests
  without credentials: 6. Live Spotify API coverage was not verified (no creds
  supplied, per task rules).

### gui-js — PASS (7/7, 0 warnings, 29 s)

- `npm test` (`node --test tests/run.js`) — exit 0.
- `tests 7, pass 7, fail 0, skipped 0`. Self-spawns vc-ts :18122, gui :18130, stub
  audio server :18131; data under `/tmp/p4-gui/`.

### radio-js — PASS (7/7, 0 warnings, 26 s)

- `npm test` (`node --test tests/run.js`) — exit 0.
- `tests 7, pass 7, fail 0, skipped 0`. Self-spawns vc-ts :18112, radio :18118/:18119.
- Startup probe: **5/6 stations reachable** — KEXP unreachable, reported honestly
  (`groovesalad=ok lush=ok radioparadise=ok fip=ok bbcws=ok kexp=unreachable`).
  Matches the README's note that sandbox egress is flaky per-host.

### downloaders — PASS (21 ran: 13 pass, 8 skip, 0 warnings, 3 s)

- `.venv/bin/python -m unittest discover -s tests -v` (Python 3.12.3 venv) — exit 0.
- `Ran 21 tests in 0.146s — OK (skipped=8)`.
- The 8 skipped are the `TestLiveDownloads` cases, each skipping with
  `'live tests need PARLAY_LIVE_TEST=1'` (YouTube, Deezer, TikTok, Spotify API,
  Navidrome). Live-download tests were **not** run (no token/credential request,
  per task rules). So the "unittest 21" claim is 21 discovered, 13 actually exercised.

### netease-go — PASS (vet+build 0, 5/5, 0 warnings, 23 s)

- `go vet ./... && go build ./... && go test ./... -count=1` — exit 0.
- 5/5 PASS, all in `internal/netease/crypto_test.go` (eapi/weapi crypto round-trips).
  The suite contains **no live NetEase tests** — the README's live search/lyrics
  verification is not covered by the automated suite. Download limitation (no
  anonymous audio URLs from sandbox egress) is a README-documented behaviour, not
  something the test suite exercises.

## Checks against prior claims

| Prior claim | Verified now |
|---|---|
| core-cpp build, 8 warnings | Confirmed: clean build 43 s, 8 warnings, all libstdc++ `-Wmaybe-uninitialized` |
| core-cpp tdlib_test 7/7 | Confirmed: 7/7, exit 0 |
| core-cpp store_test 30/2 | **Superseded: 40/40 with DBs up**; old 2 fails were environmental (no mongod on :27099) |
| vc-go 18/18 | Confirmed: 18/18 (incl. live Mongo round-trip) |
| vc-ts 24/24 | Confirmed: 24/24 |
| vc-rust 17/17 | Confirmed: 17/17 |
| spotify-rust 8/8 | Confirmed as reported, but 2 are credential-gated no-ops (see caveat) |
| audio-fx/share/queue cargo build release | Confirmed: all build exit 0; none has a test suite |
| gui-js 7/7 | Confirmed: 7/7 |
| radio-js 7/7 | Confirmed: 7/7 (5/6 stations reachable at probe; KEXP down) |
| downloaders unittest 21 | Confirmed as 21 discovered / 13 passed / 8 skipped (live-gated) |
| netease-go go test | Confirmed: 5/5 crypto tests |

## Cleanup / leftovers

- mongod (pid 30996) and redis-server (pid 33822) started by this audit were stopped
  afterwards; `pgrep` confirms neither is running. No test servers left on 18xxx
  ports; no node/go/cargo test processes remain.
- Test artifacts: `/tmp/tdlib_test`, `/tmp/store_test`, `/tmp/parlay-audit-core-build/`
  (clean-build dir), `/tmp/p4-vcts/`, `/tmp/p4-gui/`, `/tmp/p4-radio/` (JS test data
  dirs) — all under `/tmp`, safe to delete.
- No service code modified. Only files written: this report, `audit/run-one.sh`,
  and `audit/logs/*`.
