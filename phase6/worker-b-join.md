# Phase 6 — Worker B: vc.join Connect timeout + wedged join handler cancellation

- Date: 2026-11-03 ~16:30–17:30 IST (dispatch + suites run 16:59–17:25 IST)
- Repo: `~/workspace/parlay`, branch `main` at `7b95637` + uncommitted work (see §6).
  Other Phase 6 workers' in-flight changes were already in the tree when
  this work started (config.go, core-cpp, httpapi/, miniapp/,
  dispatch_test.go) — none of those files were touched here. `.env` was
  never read or touched. Nothing committed, nothing pushed.
- Scope: `Client.Connect` timeout + join-handler cancellation, per the
  brief. One correction to the brief's premise (below): the measured
  60 s wedge was not `ntg_connect` blocking — it was the UDS read loop
  deadlocking against `RequestJoin`. Both are fixed.

## 1. What was actually wrong (measured, premise corrected)

The brief (and the Phase 3/5 reports) attributed the vc.join wedge to
`Client.Connect` blocking past 60 s inside `ntg_connect` against the
fake SDP. Direct probes against the real `libntgcalls.so` 3.0.5 prove
otherwise:

| Probe (standalone cgo program, `/tmp/connect-probe`, same .so) | Result |
|---|---|
| `ntg_connect` with the dispatch fake SDP (`v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=core\r\n`) | Returns FAST, rc `-102 Invalid JSON` |
| `ntg_connect` with a valid transport-shaped remote params object (unreachable ICE candidate `192.0.2.1:50000`, opus/VP8 payload types) | Returns FAST, rc `0` — the handshake runs asynchronously; failure surfaces via the connection-change callback, not the call |
| `ntg_stop` after a failed connect | FAST, `0.27–1.1 ms` |
| Same, but with empty video `payload-types` | **Process abort**: WebRTC `CHECK failed: source == nullptr` (`webrtc_video_engine.cc:1533`) — a library landmine for malformed remote params; not pursued, flagged here |

So `ntg_connect` never blocks for any input found. The real wedge is in
`services/backend-go/internal/uds/uds.go:serveConn`: it called
`handleRequest` **synchronously in the read loop**. The vc.join handler
waits in `RequestJoin` for the core's join response — but that response
frame can only be read by the same loop, which is stuck inside
`handleRequest` waiting for the handler. Deadlock until the 60 s UDS
deadline. Baseline dispatch reproduced it exactly: `vc.join` abandoned
at precisely 60.0 s (`backend-baseline.log: action vc.join request 39
abandoned past deadline 60000ms`), 64/65 in 62 s.

The `Connect` timeout fix is still implemented as ordered (defence in
depth: the library's synchronous `create_channels` path cannot be
proven non-blocking for every real-group-call input, and the brief
requires it). But the fix that actually makes vc.join fail fast is the
read-loop one.

## 2. What was fixed and how

### 2a. UDS read loop dispatches concurrently (`internal/uds/uds.go`)

`serveConn` now runs `go s.handleRequest(c, env)` per request. The read
loop keeps reading while a handler runs, so a `RequestJoin` response is
delivered while the join handler waits for it, and a later request on
the same connection is not starved behind a wedged handler (the Phase 5
`vc.lyrics` flake shape). Responses are multiplexed by `request_id`
(§3), so inter-request order is not significant; concurrent writes are
serialised by the existing per-connection `writeMu`; each handler is
still bounded by its own deadline ctx in `handleRequest`.

### 2b. `Client.Connect` takes a context and an internal timeout (`internal/voice/voice.go`)

- Signature: `Connect(ctx context.Context, chatID int64, remoteSDP string) error`.
  Only caller is `Manager.Join` (`manager.go:309`), updated to pass its ctx.
- The cgo call runs in a worker goroutine with a `done` channel; `Connect`
  selects on call-complete / `ctx.Done()` / an internal timer. The error
  is converted by `resultError` on the worker thread (so `ntg_last_error()`
  is read on the thread that ran the call).
- On timeout/cancellation `Connect` returns at once; the worker keeps
  running detached. It holds the client's **read** lock until the library
  call returns, so the instance cannot be destroyed under it, and the
  params string is freed by `realConnect` when the call ends. Honest
  limit, documented in the code: a cgo call that never returns cannot be
  killed from Go — the wrapper bounds the *wait* and detaches cleanly, it
  does not pretend to kill the call.
- Locking restructured: `mu sync.Mutex` → `sync.RWMutex`. Every voice
  operation holds the read lock, so a stuck Connect does not block other
  operations — they proceed on the shared lock. Only `Close` takes the
  write lock, so it waits for in-flight calls instead of destroying the
  instance under a running cgo call (bounded in practice by the connect
  timeout; documented).
- Timeout value: **`timeouts.join_ms` (30 s default)** — no new key, no
  new magic number. Connect is a stage of the join, so the join budget is
  its cap; the join's context (UDS deadline) bounds it earlier whenever
  the core set a shorter deadline. A separate `connect_ms` key was
  considered and rejected: the C++ core's parser enforces a known-keys
  list for `[timeouts]` (`services/core-cpp/src/config.cpp`
  `checkKnownKeys`), so a Go-only key would break the shared file.
  `DefaultConnectTimeout = 30s` mirrors the `join_ms` default for direct
  `New()` users; `NewWithConnectTimeout` applies the configured value.
  `main.go` wires `time.Duration(cfg.Timeouts.JoinMs) * time.Millisecond`.
  `parlay.toml.example` documents the `join_ms` → Connect-cap link.
- Test seam: `connectFn func(chatID int64, remoteSDP string) error` field
  on `Client`, defaulting to a closure over `realConnect` (the real cgo
  call). Pure-Go signature because the go tool rejects cgo in test files
  (verified in the toolchain source: `modindex/read.go` — "use of cgo in
  test not supported"). Production code never sets it; it exists only so
  the timeout path can be exercised with a blocking stand-in, since no
  input found makes the real `ntg_connect` block.

### 2c. Join-handler cancellation (`internal/voice/manager.go`)

`Join` passes its ctx to `Connect` (in addition to the existing ctx
checks before `create_call`/`connect` and the ctx-aware `JoinFunc` /
`RequestJoin` path). On a timed-out/failed join the call record lands in
`StateFailed` with the stage named, the per-chat handler lock is released
when `Join` returns, and the next voice-state action (retry join, Get,
Stop) answers at once instead of starving behind the dead join.

## 3. Verification — measured numbers

### Regression tests (new, all passing)

`internal/uds/uds_test.go` — both **failed pre-fix** (5 s read timeout)
and **pass post-fix**:

| Test | Post-fix measurement |
|---|---|
| `TestJoinResponseDeliveredWhileHandlerWaits` | join response arrived **54 µs** after the JoinRequest answer (was: never within 5 s) |
| `TestSlowHandlerDoesNotStarveNextRequest` | ping answered **1.24 ms** after the slow handler wedged (was: never within 5 s) |

`internal/voice/connect_timeout_test.go` (new file, 5 tests):

| Test | Measurement |
|---|---|
| `TestConnectAgainstFakeSDPReturnsWithinTimeout` | **6.7 ms**, `ntgcalls error -102: Invalid JSON`, timeout 30 s |
| `TestConnectContextCancellation` | cancelled ctx: **2.2 µs**, `context.Canceled`; 1 ms ctx vs blocking stand-in: **1.19 ms**, `DeadlineExceeded` |
| `TestConnectTimeoutWithBlockingCall` (200 ms timeout) | returned in **200.45 ms**, `timed out after 200ms`; goroutines **2 → 3** while stuck (exactly one detached worker), **back to 2** after release — no leak; `CreateCall` on another chat proceeded while stuck |
| `TestOtherOpsProceedDuringStuckConnect` (30 s timeout) | `GetState` other chat **35 µs**, `Stop` other chat **634 µs** while Connect wedged |
| `TestManagerJoinTimeoutFreesNextAction` (200 ms ctx, hanging JoinFunc) | Join returned in **201.1 ms**, `voice: join chat …: join: context deadline exceeded`, record `failed/join`; retry Join **1.39 ms**; Stop after **4.0 µs** |

### Dispatch check (real C++ `IpcClient` vs real backend, `-tags parlaydsp`)

- Baseline (pre-fix binary): **64/65 in 62 s** — `vc.join → TRANSPORT FAILURE`,
  `vc.lyrics → PASS`; backend log: `vc.join request 39 abandoned past deadline 60000ms`.
- Fixed binary: **65/65, 0 failures, in 1.8 s** —
  `vc.join → PASS engine_unavailable (voice join failed: voice: join chat -1001: connect: ntgcalls error -102: Invalid JSON)`,
  `join_request handler ran: yes`,
  `vc.lyrics → PASS bad_request (no title to look lyrics up for)` immediately after.
  No `abandoned` line in the fixed backend log. `vc.join` still cannot
  *succeed* without a real group call (the fake SDP is rejected by the
  library) — it now fails fast with the exact library reason instead of
  hanging. Full per-action log: `/tmp/phase6-b/core_peer-fixed.log`
  (backend log `/tmp/phase6-b/backend-fixed.log`).

### Full suites (`go vet` + `gofmt` + `go test`)

- `gofmt -l` on touched packages: clean. `go vet ./...`: clean.
- Tagged (`-tags parlaydsp`, the shipping config): **333 passed / 1 failed /
  12 skipped** across 29 packages; per-package voice **26/26** (Phase 5
  baseline 21 + 5 new), uds **10/10**, dsp **16** (= baseline), dsp/fake
  **10** (= baseline). The 1 failure is `TestTransitionMismatchRebinds` —
  see §5; the failing set varies run to run (earlier tagged run: all 3
  Transition/FeedEnd tests failed; this run: only 1).
- Untagged: **28/29 packages ok**; only `handlers` fails, with the same 3
  tests as §5.
- Greps: **zero** `TODO`/`FIXME`/`HACK`/`unimplemented` in the changed files.

### Environment notes

- `/tmp` (512 MB tmpfs) filled up mid-run (`No space left on device`
  linking two test binaries — not a code failure). Re-ran with
  `TMPDIR=/home/hatch/.tmp-golink` (deleted after use is not required;
  it holds only Go link temp files). `/tmp` is full of other workers'
  artifacts (`p5-tarball` 158 MB, `ci-repro` 88 MB, …) — none deleted.
- The Go toolchain is `~/workspace/parlay/.tools/go/bin/go` (1.23.0);
  `go` is not on `PATH` in this shell.

## 4. Files changed (mine, all uncommitted)

| File | Change |
|---|---|
| `services/backend-go/internal/voice/voice.go` | `Connect(ctx, …)` with worker goroutine + timeout select; `RWMutex` (read lock for ops, write lock for `Close`); `DefaultConnectTimeout` (30 s, mirrors `join_ms`); `NewWithConnectTimeout`; `realConnect`; `connectFn` test seam |
| `services/backend-go/internal/voice/manager.go` | `Connect(ctx, chatID, remoteSDP)` call site + comment |
| `services/backend-go/internal/voice/voice_test.go` | `Connect` call-site update (1 line) |
| `services/backend-go/internal/voice/connect_timeout_test.go` | New: 5 regression tests (§3) |
| `services/backend-go/internal/uds/uds.go` | `serveConn` dispatches requests concurrently (`go s.handleRequest`) |
| `services/backend-go/internal/uds/uds_test.go` | New: 2 regression tests (§3) |
| `services/backend-go/cmd/backend/main.go` | Wire `cfg.Timeouts.JoinMs` into `voice.NewWithConnectTimeout`. NOTE: this file's diff vs HEAD also carries Worker A's Mini App hunks (httpapi import/server, `SocketPathSplitWarning`) — already in the file when this edit was made, not touched here |
| `parlay.toml.example` | `join_ms` comment documents the Connect cap; no new key |

`git diff --stat` (my paths): 7 files, +365/−36, plus the new
`connect_timeout_test.go` (untracked).

## 5. Follow-up: Transition tests — NOT my rework (proven)

Worker A reported `TestTransitionAdvancesWithoutRebind` +
`TestTransitionMismatchRebinds` pass 2/2 at HEAD but fail consistently
in-tree, with my voice rework as the only change on their path. Checked
with the same pristine-HEAD comparison (identical commands, 3 reps each,
full `handlers` package, untagged):

| State | Full package, untagged (3 runs) | Isolated `-run` (untagged) |
|---|---|---|
| S1 in-tree (my changes + others') | Transition tests fail **3/3** (`TestFeedEndAdvance` flips — a flake) | Passed 2/2 once |
| S2 my files stashed (others' remain) | Transition tests fail **3/3** | — |
| S3 full pristine HEAD (everything stashed) | Transition tests fail **3/3** (Advances flipped once across the runs) | Failed 2/3 once |

Conclusion: **the two Transition failures are pre-existing at HEAD in the
full-package run and are not caused by my rework** (identical failures
with my files reverted, and on full pristine HEAD). Worker A's "pass 2/2
at HEAD" reproduces only for the isolated `-run` invocation, which flips
pass/fail run-to-run in both states — it is a timing flake, not a
deterministic break.

Mechanism (read, not fixed — outside this brief): `advanceFromTransition`
sets the feeding record **after** the room advance (`handlers.go`: room
`Advance` → events emit → `maybePrefetch` → return → `setFeeding`).
`waitRoom` polls the room snapshot every 10 ms and the test checks
feeding immediately after the snapshot shows track B, so it can observe
snapshot=B while feeding is still A. Under full-package load the window
is hit reliably; in isolation it is a coin flip. `TestFeedEndAdvance`
is the same class of flake (Worker A already proved it fails on pristine
HEAD; left alone as instructed). No fix applied here — fixing the race
means changing the handlers transition ordering or the test, which is
not this brief and not my call.

Restore note: the stash dance for S2/S3 created two overlapping
"other workers" stashes (a pathspec error on the first push); the
duplicate was dropped after verifying its contents were already in the
tree, and my stash was popped. Final state verified: **15 modified
files + 4 untracked entries, stash list empty**, exactly the
pre-experiment tree plus my work. No other worker's file was modified
or lost.

## 6. What remains honestly unmeasured / needs the live VPS

- **Real join on a real group call.** Still the only way to prove the
  `Connect` success path, the `ConnectionConnected` callback, and audible
  playback. In the sandbox `vc.join` now fails fast and honestly
  (`-102 Invalid JSON` for the fake SDP); the live VPS proof is the
  user's test afterwards, as ordered.
- **A genuinely blocking `ntg_connect` was not found.** The timeout
  wrapper is proven via the stand-in, not against a real stuck library
  call; the library returns fast for every probed input (fake SDP,
  valid-but-unreachable transport). If a real group call's Connect blocks
  inside the library, the wrapper bounds the wait — but that path itself
  is unmeasured in the sandbox.
- **Library-internal serialisation for a genuinely stuck `ntg_connect`
  is unproven** for the same reason; "other ops proceed" is proven at the
  Go-lock level only.
- **Stop-while-Connect concurrency is unproven.** The Manager's fail path
  can now call `Stop` while an orphaned `ntg_connect` worker is still
  inside the library call for the same chat; whether the library
  tolerates that concurrent teardown could not be tested without a stuck
  input. Watch for it in the live VPS proof (a crash there would be
  visible and attributable).
- **`Close` waits for in-flight Connect workers** (write lock). A library
  call that never returns would hold `Close` up at shutdown — same as the
  old behaviour, documented in the code.
- The empty-video-`payload-types` process abort (§1) is a library
  landmine worth knowing about if remote params are ever constructed
  by hand; the real TDLib params always carry payload types.
