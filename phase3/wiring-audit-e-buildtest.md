# Wiring audit — Worker E: build + test + final verification

Date: 2026-11-03 ~18:45–19:20 IST. Base: main at `f1a7211` (Phase 3 done
and pushed), plus the uncommitted Wave 1 audit edits (startup, config,
code, docs workers). Tree verified as it stands; **no source, config,
docs, or launchers were edited by this worker, nothing was pushed,
`.env` was not touched.** All live runs used dummy creds only
(`env -i`, no Spotify/Deezer/NetEase/AudD keys set anywhere).

## 1. Go backend (`services/backend-go`)

Toolchain: repo-vendored `.tools/go/bin/go` (Go 1.26.0). System `go` is
1.23.0 and is rejected by `go.mod` (`requires go >= 1.26.0`); do not
use it, and do not use `GOTOOLCHAIN=local` with it.

- `go vet ./...`: **clean, exit 0, zero findings.**
- `gofmt -l .`: **zero unformatted files.**
- `go test ./...`: **25/25 packages ok, 0 FAIL.**
- `go test -count=1 -v ./...`: **245 passed, 0 failed, 12 skipped.**
  The 12 skips are exactly the Phase 3 live-gated set
  (`PARLAY_LIVE_TEST=1`): 11 resolver live tests +
  `TestFetchNetEaseLive`. Counts are identical to the Phase 3 baseline
  (245/0/12) — no test gained, lost, or newly skipped by the Wave 1
  audit edits.
- `TestRegisterAllExactlyOnce` (`cmd/backend/main_test.go`): **PASS**
  — all 65 contract actions + ping, each registered exactly once, no
  extras.

## 2. Core-cpp (`services/core-cpp`)

Rebuilt first (`cmake --build .`, exit 0; Wave 1 touched
`config.cpp`, `tdlib_client.cpp/.hpp`, `stub_backend.cpp`,
`uds_test.cpp`, so a rebuild was required). `ctest`: **9/10 passed,
1 failed** — `store_test` only.

PASS-line count (verbose log, `^[0-9]+: PASS `): **1,614** total
(baseline 1,615), per suite: uds 40, config 55, rate_limiter 28,
dispatcher 277, routing 944, tdlib 46, telemetry 5, ui 160, scram 40,
store 19. FAIL lines: **2**, SKIP lines: **3**.

The 1,614 vs 1,615 delta is a **flake, not a regression**:
`dispatcher_test` prints 277 or 278 PASS lines run-to-run with **0
FAIL lines every time** (5 consecutive direct runs: 278, 277, 278,
278, 278, 277). The line that sometimes does not appear is
`PASS play reply audio, backend refuses: vc.play_file issued` — a
timing-dependent async assertion that is skipped, never failed, when
the path does not complete in time. No dispatcher code changed in
Wave 1 (last change `f1a7211`).

Failing test, by name — `store_test` only, with exactly the 2
pre-existing env failures:

- `FAIL mongo connect: cannot connect to mongodb at 127.0.0.1:27099`
- `FAIL store backend=mongo (got file)` (falls back to file store)
- `SKIP redis tests (not up): cannot connect to redis at 127.0.0.1:6399`
  (a skip, not a failure)

Mongo/Redis are down in the sandbox. These are the same 2 failures
as the Phase 3 baseline; **nothing else fails.**

## 3. Dispatch check — 65/65 actions, real core client vs real backend

Method (per `phase3/wave2-e-verify.md` §4.3): real backend binary
(`go build ./cmd/backend`, dummy creds, temp data dir
`/tmp/audit-e/data`, `PARLAY_YTDLP_WORKER` pointed at the repo
script so the daemon warms — boot log confirms `yt-dlp daemon warm,
version 2026.10.26`) + `tests/interop/core_peer.cpp` built with
`g++ -std=c++20` against the real `parlay::IpcClient`
(`ipc.cpp`, `json.cpp`, `config.cpp`, `toml.cpp`).

**Result: 64/65 actions answered with a real response frame.
1 failure: `vc.join` — `TRANSPORT FAILURE (no response)`.**

This is the exact Wave 2 result, same cause, not a regression:

- The action IS registered (exactly-once test, §1) and dispatched.
- The `JoinRequest` half of the handshake is proven end-to-end:
  the probe's join handler ran (`join_request handler ran: yes`) and
  answered the fake remote SDP.
- The handler then blocks in ntgcalls `Connect` against that fake
  SDP (no media lines, no real group call): no response frame within
  the 60 s probe timeout. A real group call is the only way to prove
  the Connect stage (Phase 6 live proof).

Representative honest answers (full log: `/tmp/audit-e/core_peer.log`):
`chat.settings`/`gui.queue`/`search.inline`/`queue.vote`/
`share.create`/`spotify.dislike`/`spotify.now_playing`/`vc.stop`/
`spotify.status`/`library.scan`/`cache.clear`/`download.prefs` ok;
`vc.now_playing`/`vc.progress`/`vc.pause`/`vc.loop`/`vc.volume`
`not_found` (nothing playing); `download.*`/`search.query`/
`lemonic.*`/`netease.*`/`vc.play` `bad_request` (empty query);
`spotify.*` `engine_unavailable` (no creds); `audio.recognize`
`engine_unavailable` (no `AUDD_API_TOKEN`); `library.sharescan`
`engine_unavailable` (slskd down); `download.progress`/
`download.cancel` `not_found` (unknown run); `vc.play_file`
`not_found` (replied file not found on disk). No action answered
`unknown action`.

## 4. Port check — deleted services are gone

- `ss -tlnp`: **zero TCP listeners** in the sandbox at check time.
- TCP probes to `127.0.0.1`: **8001–8010 closed, 3000 closed,
  8080 closed.** No HTTP listener on any deleted-service port.
- Port 8080: confirmed in source as the core's own HTTP API —
  `services/core-cpp/src/config.hpp:173` (`int port = 8080; // PORT —
  core HTTP port (GUI /api surface, Phase 6 A2)`) and
  `services/core-cpp/src/main.cpp:21,520–531,569` (`GET /healthz`,
  `/api/*`). Nothing is listening on it right now because the core
  binary is not running; that is the expected state, not a finding.
  No backend-go code binds any TCP port (grep over
  `services/backend-go` for `8080`: zero hits outside tests).

## 5. TODO/FIXME/HACK/Unimplemented grep

Case-insensitive, production + test code, excluding `refs/`,
`.tools/`, `scripts/yt-dlp-venv/` (none of those paths were in the
searched trees):

- `services/backend-go` (`--include='*.go'`, all files): **0 hits.**
- `services/core-cpp/src` + `services/core-cpp/tests`
  (`--include='*.cpp'`, `--include='*.hpp'`): **0 hits.**

## 6. Known open items — current state (not fixed, per orders)

### (a) `vc.join` Connect needs a context/timeout — STILL OPEN, unchanged

- `services/backend-go/internal/voice/voice.go:312`:
  `func (c *Client) Connect(chatID int64, remoteSDP string) error` —
  still takes **no context**, calls `C.ntg_connect` directly.
- `services/backend-go/internal/voice/manager.go:249`:
  `m.client.Connect(chatID, remoteSDP)` — still no timeout; the
  only context check is `ctx.Err()` *before* the call
  (`manager.go:247`). A stuck Connect still leaks the handler
  goroutine and leaves the call in `StateJoining` (re-proven live
  by the §3 `vc.join` probe: 60 s with no response frame).
- One-line fix shape (reported, not applied): give `Connect` a
  `context.Context` or an internal deadline and fail the join on
  expiry.

### (b) Download runs / prefs / library index are in-memory singletons — STILL OPEN, unchanged

- Download runs: `services/backend-go/internal/download/registry.go:95–113`
  — `Registry{live map[string]*Run, finished []*Run}`, process-wide
  `sharedRegistry = NewRegistry()` / `SharedRegistry()`.
- Prefs: `services/backend-go/internal/download/prefs.go:55–69` —
  `PrefsStore` ("in-memory until the [store bucket exists]",
  `prefs map[int64]Prefs`), process-wide `sharedPrefs`.
- Library index: `services/backend-go/internal/library/library.go:45–53`
  — `Index{byID map[string]*Entry, byPath map[string]*Entry}`.
- Store buckets: `services/backend-go/internal/store/store.go:30–41`
  — exactly `rooms, queue, votes, shares, spotify, settings, radio`.
  **No `downloads`, `library`, or `dlprefs` bucket exists.** A restart
  still loses finished-run snapshots and resets prefs.

### (c) `fx` persists via store RoomState directly — STILL THE CASE

- `services/backend-go/internal/fx/fx.go:57`: `Service` holds
  `store *store.Store` directly.
- `fx.go:144,154`: `s.store.GetRoom(chatID)` / `s.store.PutRoom(room)`
  in `Apply` — no `room.Manager` involvement (zero references to
  `room.Manager` in the file). Per-file FX records are an in-memory
  map (`files map[string]fileRecord`, `fx.go` ~line 60). Same
  staleness shape the vote/gui seams had before Wave 2 rewired them.

### (d) Inventory open questions (§10) — status after Wave 1

| Q | Question | Status now |
|---|---|---|
| Q1 | `np` oversize: 50 MB (Bot API) vs 2 GB (MTProto)? | **Still open as a decision.** Inventory §10 still lists it verbatim. Current behaviour is 50 MB in the core send path: `services/core-cpp/src/dispatcher.cpp:4147` (`file_size > 50 * 1024 * 1024` → skip) and the backend NetEase cap `MaxBotAPIBYTES = 50 * 1024 * 1024` (`internal/resolver/netease/client.go:46`). No 2 GB path exists anywhere. |
| Q2 | `spotify.ignore` — confirm or drop? | **Still open.** `services/backend-go/internal/spotify/handlers.go:578–580`: `// No ignore store exists (§9-E8: spotify.ignore is a [decision])`, `ignored_by` hardcoded to `[]int64{}`. |
| Q3 | `autoplay_lang` / `autoplay_mood` defaults | **Implemented per proposal, user confirmation still outstanding.** `services/backend-go/internal/settings/settings.go:22` defaults both to `""` ("inventory §8.1 / U3 proposal — no value exists anywhere in-tree"); fields exist at `internal/model/model.go:199–200`. Inventory U3 still says "Wave 2 / user to confirm". |
| Q5 | `vc.queue` `move` verb — add, or keep core list→remove→re-add? | **Still open.** `services/backend-go/internal/handlers/vc.go:363` rejects anything outside `add\|remove\|clear\|list`; no `move` verb, no `Manager.Move` (`internal/room/room.go` has only `Remove` at :379). |
| Q6 | `speed`/`bass` carrier — which call carries them into the room? | **Resolved by implementation (Wave 2/Phase 3); inventory text just never updated.** `vc.play` carries them: `contracts/ipc.json:771–772` (`speed`/`bass`, "carried into the room, record-only until the Phase 5 DSP applies it"), `internal/handlers/vc.go:51–52,104–110` (`Speed`/`Bass` validated then `rooms.SetSpeed`/`SetBass`). |
| Q7 | `ntg_time` vs wall-clock as position source | **Resolved by implementation: wall-clock.** `internal/room/room.go:15` ("Position is wall-clock derived (§4.4)") and `room.go:206–210` (`positionLocked`, wall-clock). The `ntg_time` accessor exists (`internal/voice/voice.go:468–480`, `Manager.Time` at `internal/voice/manager.go:460–464`) but **has zero callers** in production code. Inventory text still lists it open. |

Net: Q6 and Q7 are answered by the code (only the inventory wording
is stale); Q1, Q2, Q3, Q5 remain genuinely open decisions for the
user. Q4 (`/recognize` provider) was already decided pre–Wave 1
(AudD, OD-10) and is not re-listed here.

## Verdict: green enough for Phase 4 to start — with the two standing caveats

- **Go: green** (25/25 packages, 245/0/12, vet + gofmt clean,
  exactly-once registration PASS).
- **Core-cpp: green modulo the known env failures** (9/10; only the
  2 pre-existing mongo/redis `store_test` failures; the 1-line PASS
  delta is a proven timing flake with 0 FAILs).
- **Dispatch: 64/65** with the single `vc.join` miss being the known
  sandbox limit (fake SDP, no real group call), not a registration
  gap.
- **Ports, TODO grep: clean** (0 listeners on deleted-service ports,
  0 TODO/FIXME/HACK/Unimplemented hits).

**Nothing found here blocks Phase 4.** The items that remain are the
already-known ones, unchanged since Phase 3: (a) `vc.join` Connect
has no timeout (a stuck Connect leaks a goroutine — worth fixing
before any live voice work in Phase 6, not a Phase 4 blocker);
(b) download runs/prefs/library index are in-memory (Phase 4's
cache/prefetch work should decide whether to add the store buckets
then); (c) `fx` still writes RoomState directly (small follow-up,
same shape as the already-fixed vote/gui seams); (d) inventory
decisions Q1, Q2, Q3, Q5 still await the user, and Q6/Q7 only need
their inventory wording updated to match the code.

Artifacts from this run (scratch, `/tmp`, not committed):
`/tmp/audit-e/gotest.log`, `/tmp/audit-e/gotest-v.log`,
`/tmp/audit-e/ctest.log`, `/tmp/audit-e/ctest-v.log`,
`/tmp/audit-e/core_peer.log`, `/tmp/audit-e/backend2.log`.
The probe backend + its temp data dir (`/tmp/audit-e/data`) were
stopped/left in `/tmp` only; no repo files were created outside
this report.
