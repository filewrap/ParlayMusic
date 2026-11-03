# Phase 6 — Worker B: Full Test Re-run + Live Smoke (11 non-core services)

- Date: 2026-11-03 ~03:42–04:20 IST (run started 03:42 IST; smoke ~04:16–04:18 IST)
- Scope: all services EXCEPT core-cpp (not built, not run, not touched).
- Repo state: HEAD `9457865a` (Phase 5) + uncommitted Phase 6 Worker A+B working-tree
  changes. Nothing committed, no service code modified.
- Environment: no live Telegram/Spotify/Deezer calls; `PARLAY_LIVE_TEST` NOT set
  (live-gated tests stayed skipped); Mongo NOT started or required by this worker
  (a mongod on :27017 was already running from other Phase 6 work — not started
  by this worker, not used by these suites except where a suite handles it itself).
  `.env` was never read. Smoke used a throwaway dummy token, never a real credential.
- Toolchains: Go 1.23.0 (`~/workspace/parlay/.tools/go/bin/go`), Node stdlib
  (`node --test`), cargo (`~/.cargo/bin/cargo`), downloaders venv
  (`services/downloaders/.venv`).

## Test re-run results — 232 passed / 0 failed / 11 skipped

| Service | Command | Passed | Failed | Skipped | Notes |
|---|---|---|---|---|---|
| vc-go | `go test -count=1 ./...` + `go vet ./...` | 27 | 0 | 3 | vet clean. Skips are live-gated: `TestResolveYouTubeURLLive`, `TestRelatedLivePick`, `TestPlayLiveYouTubeCarriesStreamURL` |
| vc-ts | `npm test` | 32 | 0 | 0 | node --test, 71.3 s |
| vc-rust | `cargo test --release --offline` | 31 | 0 | 0 | single test binary |
| spotify-rust | `cargo test --offline` | 40 | 0 | 0 | |
| audio-fx-rust | `cargo test --offline` | 6 | 0 | 0 | |
| share-rust | `cargo test --offline` | 5 | 0 | 0 | |
| queue-rust | `cargo test --offline` | 8 | 0 | 0 | |
| downloaders | `.venv/bin/python -m unittest discover -s tests` | 42 | 0 | 8 | "Ran 50 tests … OK (skipped=8)"; skips are live-gated provider tests |
| netease-go | `go test -count=1 ./...` + `go vet ./...` | 23 | 0 | 0 | vet clean |
| gui-js | `npm test` | 11 | 0 | 0 | node --test |
| radio-js | `npm test` | 7 | 0 | 0 | node --test |
| **TOTAL** | | **232** | **0** | **11** | |

- **Grand total: 232 passed / 0 failed / 11 skipped.**
- Reference: Phase 5 was 361 passed / 0 failed / 11 skipped across 12 services
  (i.e. core-cpp accounted for the other 129 passes; this re-run covers the
  other 11 services only, and matches Phase 5's 232 + 11 skipped exactly).
- **No failures at all** — no regression triage was needed. No test was fixed,
  edited, or skipped by hand.

## Live smoke — 11/11 services HTTP 200 on /healthz

Each service started on its compose port with a dummy token and a scratch
`PARLAY_DATA_DIR` under `/tmp/parlay-smoke/data/<service>` (state discarded
afterwards), then `GET /healthz`, then stopped.

| Service | Port | HTTP | /healthz body (truncated) |
|---|---|---|---|
| vc-go | 8001 | 200 | `{"ok":true,"data":{"service":"vc-go","engine":"go","degraded":true,"yt_dlp":false,...}}` |
| vc-ts | 8002 | 200 | `{"ok":true,"service":"vc-ts","engine":"ts","yt_dlp":true,...}` |
| vc-rust | 8003 | 200 | `{"ok":true,"service":"vc-rust","data":{"service":"vc-rust","status":"ok"}}` |
| audio-fx-rust | 8004 | 200 | `{"ok":true,"service":"audio-fx-rust","data":{"status":"ok"}}` |
| share-rust | 8005 | 200 | `{"ok":true,"service":"share-rust","data":{"status":"ok"}}` |
| queue-rust | 8006 | 200 | `{"ok":true,"service":"queue-rust","data":{"status":"ok"}}` |
| spotify-rust | 8007 | 200 | `{"ok":true,"service":"spotify-rust","data":{"status":"ok"}}` |
| radio-js | 8008 | 200 | `{"ok":true,"service":"radio-js","stations":6,"active_radios":0,...}` |
| downloaders | 8009 | 200 | `{"ok": true, "service": "downloaders"}` |
| netease-go | 8010 | 200 | `{"ok":true,"service":"netease-go"}` |
| gui-js | 3000 | 200 | `{"ok":true,"service":"gui-js"}` |

Notes / observations (not failures):
- vc-go reports `degraded:true, yt_dlp:false` in its health body: the smoke env
  has no yt-dlp on vc-go's PATH (it resolves yt-dlp only from the downloaders
  venv, as in its tests). HTTP is still 200 and the service runs; resolve
  returns honest `engine_unavailable`. Same degraded state as its own tests
  assert (`healthz must report degraded:true, yt_dlp:false`).
- vc-ts health shows `yt_dlp:true` (it finds the downloaders venv yt-dlp) and
  `lemonic_configured:false` (no key in smoke env, expected).
- radio-js health shows `vc_go:null, vc_ts:null` (no VC URLs wired in smoke env,
  expected).
- The smoke harness's first run died on a shell bug (`$!` unbound under
  `set -u`) after orphaning a vc-go instance (PID 6023, reparented to PID 1)
  that kept :8001. The second run's vc-go therefore failed to bind
  ("address already in use") and its 200 came from that orphan. The orphan was
  identified (same binary path + smoke env), killed, and vc-go was re-verified
  standalone: fresh instance on :8001, HTTP 200, stopped cleanly. The 200 in
  the table above for vc-go is from that clean re-run.
- Node/python services needed an explicit second kill pass: killing the wrapper
  subshell did not kill the child `node`/`python` process. All four (vc-ts,
  radio-js, downloaders, gui-js) were killed individually afterwards.
- Final verification: all smoke ports (8001–8010, 3000) free, `pgrep` shows no
  stray Parlay service processes. Pre-existing Phase 4/6 infra (redis :6399,
  mongod :27017) was left untouched.

## Files / logs

- Smoke script: `/tmp/parlay-smoke/smoke.sh` (ephemeral; not in the repo)
- Smoke logs: `/tmp/parlay-smoke/logs/<service>.log` and `<service>.health`
- This report: `~/workspace/parlay/phase6/worker-b-rerun.md`
