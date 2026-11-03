# Parlay Phase 4 — cache + lookahead prefetch (verification + reconciliation)

Date: 2026-11-03. Base: main at `d36cdd7` (pre-Phase 4 wiring audit, already
pushed) + uncommitted Phase 4 work. This is the Phase 4 commit.
Single source of truth: `FINAL-ARCHITECTURE.md` (locked — NOT edited).
`.env` untouched. No Phase 5 work. `vc.join` Connect timeout untouched
(Phase 6 work).

Verification was done by Worker E with fresh runs; counts below are
Worker E's own, not copied from worker reports. (Note: the completion
handoffs arrived crossed between Workers A and B; both reports exist on
disk and each was verified against the tree independently.)

## 1. What was built and changed (59 files)

### Worker A — core state (Q1/Q2/Q3/Q4/Q6/Q7 core side, Q8/Q9 follow-ups)
- **Q1 — core Store Mongo/Redis backends DELETED** (user-ruled, locked).
  `store.hpp/cpp` rewritten file-only (~1,615 → ~230 lines in store.cpp);
  `scram.{hpp,cpp}` split surgically — SCRAM Client/MD5/Mongo password
  code deleted, general crypto primitives kept (`sha256`, `base64Encode`
  are used by production code). `config.{hpp,cpp}`: `mongo_uri`/`redis_url`
  fields + `MONGO_URI`/`REDIS_URL` env reads gone. `store_test.cpp` /
  `scram_test.cpp` rewritten server-free (29 + 16 checks).
- **Q2 — 10 dead service-URL fields + 26 dispatcher call sites deleted.**
  `ipc.{hpp,cpp}` gained no-URL overloads (`post`/`get`/`healthz`);
  dispatcher uses them. Remaining `_url` fields in config are live
  UI-link fields (`updates_url`, `support_url`, `source_url`).
- **Q3 — radio-manage card host** (judgment call): dead `radio-js:8008`
  link replaced by the station name (`<code>{host}</code>`) from
  `radio.status`; any host string would be a dead link in the locked
  topology.
- **Q4 — `download.cancel` / `library.delete` wired into `cbDl`/`cbDel`.**
  Cancel: chat guard, backend `download.cancel` post, toast + progress
  message retired in place. Delete-yes: backend `library.delete` post,
  `Deleted.` toast, confirm card deleted. New toasts in `ui_texts.hpp`;
  16 new `dispatcher_test` checks.
- **Q6 — `[ipc] socket_path` added** (core side): `config` parses it,
  `PARLAY_UDS_PATH` env wins over file; `IpcClient::defaultSocketPath`
  precedence `[ipc]`/env → `<data_dir>/parlay-backend.sock`.
- **Q7 — partial: dead-write removal done, full rewire FLAGGED not done.**
  4 write-only `autoplay:<chat_id>` kv writes deleted (zero reads).
  Full `chat:<chat_id>` panel-settings → backend bbolt rewire NOT done
  (see §6 open items).
- **Q8 follow-up** (from Worker C): `DefaultChatSettings().AutoplayLang`
  `"en"` → `""`, test pin updated.
- **Q9 follow-up** (from Worker C): core stores `"admins"` now —
  `onPlaymode` writes `"admins"`, settings toggle flips
  `"admins"`/`"everyone"`, gate checks `== "admins"`; input parser still
  accepts `"admin"` / `"admins"` / `"adminonly"`. Test seed updated to
  `"admins"`. This resolves Worker A's §6 blocker (a) as written — the
  report text describing it as open is stale relative to the tree.
- `.env.example` note rewritten: the core no longer parses `MONGO_URI` /
  `REDIS_URL` or any service URLs.

### Worker B — cache + prefetch + backend state (Q10/Q11/Q12, [ipc] follow-through)
- **TTL-aware URL/metadata cache** (`internal/resolver/cache.go`, new):
  `Cache` over a new bbolt `cache` bucket; cache layer owns expiry.
  Canonical keys `<source>:<track_id>`; `ParseStreamExpiry` honours
  `expire=`/`expires=`/`exp=` signatures, falls back to
  `limits.url_cache_ttl_sec`, never serves stale (lazy delete on read).
  `Registry.Resolve` checks cache first on direct queries, stores after
  successful resolve; new `Registry.ResolveTrack` for the prefetch path.
- **N+1 prefetch / pre-open** (`internal/prefetch/`, new package):
  one pending stream per chat; `Prefetch` returns immediately, `Take`
  hands over only a ready, error-free handle. `PrefixBytesForLeadTime`
  = leadSec × 32,000 B/s, clamped [64 KiB, 4 MiB]. Consumed today by the
  voice play path (`Voice.PlayStream` + `startFeed` refactor) — the
  Phase 5 DSP plugs into the same `PrebufferedStream` handle type.
  Wired from `vcPlay`, `advancePlayback`, `vcAutoplaySkip`; `vcStop`
  cancels. Disabled when `prefetch_lead_time_sec == 0` (default 30 s).
- **In-memory singletons → bbolt buckets** (`cache`, `downloads`,
  `dlprefs`, `library` added to `topBuckets`): finished download runs
  persist (live runs cannot), download prefs persist before the
  in-memory write, library index persists (entries whose file vanished
  are dropped on load). `main.go` attaches all three after `store.Open`
  (failures logged, backend keeps running memory-only for that piece).
- **Q11**: hardcoded `/home/hatch/...` yt-dlp fallback deleted;
  `candidateRoots()` (CWD + exe dir) + `walkUp`; env overrides first.
- **Q12**: resolver `DataDir()` returns `"data"` when `PARLAY_DATA_DIR`
  is unset (was `~/.local/share/parlay`); one data dir everywhere.
- **Q10**: nine parsed-but-unused `[backend]` keys wired
  (`Registry.Configure`): `slskd_url`, `deezer_arl`, `netease_music_u`,
  `navidrome_url/user/pass`. `slskd_user`/`slskd_pass`/`ytdlp_path`
  DELETED from the schema (fail loudly naming the key).
  `NAVIDROME_PASSWORD` canonical, `NAVIDROME_PASS` legacy alias.
- **[ipc] follow-through**: Go parser gained `IPCConfig{SocketPath}`;
  `SocketPath()` precedence `PARLAY_UDS_PATH` env > `[backend] uds_path`
  > `[ipc] socket_path` > `<data_dir>/parlay-backend.sock`. This mirrors
  the core (which never reads `[backend]`). `TestExampleFileLoads` fixed
  (was a hardcoded `/home/hatch/...` path, now repo-relative).

### Worker C — contracts, config values, docs (Q5/Q8/Q9/Q14/Q15)
- **Q5**: `contracts/ipc.json` retargeted — 65/65 `service` fields now
  `backend-go:<package>`, 65/65 `method` fields now `UDS <action>`;
  transport `UDS + JSON`, auth note (socket 0600 in 0700 data dir);
  version stays `0.3.1`, action set identical. `uds-protocol.md` /
  `routing-table.md` / `contracts/README.md` retargeted the same way;
  `download.prefs` format list corrected (`aac` → `ogg`, matches the
  validator). Routing-table rows A63/A81 + §E entries now record the Q4
  wiring as Live (resolving Worker C §9 item 3).
- **Q8**: `autoplay_lang` canonical default `""` (any language) — wire
  default + inventory agree; docs annotated.
- **Q9**: `play_mode` vocabulary `"admins"` (with `"everyone"`); backend
  validator accepts only those two; docs annotated.
- **Q14**: `bin/` added to `.gitignore` (0 tracked files, nothing
  untracked).
- **Q15**: `COMMANDS.md` code-grounded pass — 83 table rows + 13
  "Not yet" bullets checked against `dispatcher.cpp` and backend
  `Register` calls; 42 replacements (Who-column corrections, 8
  Implements corrections, 8 "Not yet" corrections, header fixes).

### Worker D — fx persistence fix
- Root cause: `fx` wrote `FXPreset`/`FXParams` straight to the store
  via `GetRoom`/`PutRoom`, invisible to `room.Manager`, which owns the
  in-memory room and persists on every mutation — so the next Manager
  mutation silently clobbered the FX, or fx's stale snapshot clobbered
  the Manager.
- Fix: `room.Manager.SetFX` / `Manager.FX` (additive, same family as
  `SetSpeed`/`SetBass`); `fx.Service` now holds `rooms *room.Manager`
  instead of `store *store.Store`; `handleApply` is one `SetFX` call.
  No new bucket, no store-package change — the rooms bucket is the fx
  home. `TestApplySurvivesRestart` (reopen store, FX still there) and
  `TestApplyDoesNotClobberLiveRoom` (the exact old-shape regression)
  added. Deliberately not changed: per-file FX record stays in-memory
  (write-only today), no `fx.get` UDS action, `Stop` does not clear FX.

## 2. Verified counts (Worker E's own runs, 2026-11-03)

### Go — `services/backend-go` (vendored `.tools/go/bin/go` 1.26.0)
- `go vet ./...`: clean, exit 0. `gofmt -l cmd internal tests`: zero
  unformatted files.
- `go test ./... -count=1 -v`: **26/26 packages ok, 272 passed / 0 failed
  / 12 skipped** (the 12 skips are the known live-gated tests).
  Matches the worker claim exactly (baseline Phase 3: 25 pkgs, 245/0/12).
- `TestYtDlpDaemonSurvivesCallerContextExpiry`: **PASSED in 4.53 s** in
  this run — the flake Worker A saw once under full-suite load did NOT
  recur here.

### C++ — `services/core-cpp`
- `cmake --build . -j4`: exit 0.
- `ctest`: **10/10 passed, 0 failed**, 33.18 s (baseline Phase 3: 9/10 —
  the 2 pre-existing `store_test` mongo env failures are gone).
- PASS-line totals by suite (each binary run its correct way):

  | Suite | PASS | FAIL |
  |---|---|---|
  | config_test | 55 | 0 |
  | dispatcher_test | 294 | 0 |
  | rate_limiter_test | 28 | 0 |
  | routing_test | 944 | 0 |
  | scram_test | 16 | 0 |
  | store_test | 29 | 0 |
  | tdlib_test | 46 | 0 |
  | telemetry_test | 5 | 0 |
  | uds_test | 40 | 0 |
  | ui_test | 160 | 0 |
  | **Total** | **1,617** | **0** |

  (Worker A's 1,616–1,617 reproduces: the dispatcher 1-line timing
  flake accounts for the ±1. It did NOT recur in my runs — dispatcher
  gave 294 in 4 consecutive runs, ALL PASS.)
- `routing_test` invocation note: its two default relative paths are
  inconsistent (table path is repo-root-relative, dispatcher path is
  `services/core-cpp/`-relative), so a bare run fails from BOTH
  directories — from the repo root it aborts after 674 PASS
  (`dispatcher.cpp source readable` FAIL, then a `substr` throw on the
  empty source); from `services/core-cpp/` it reports the misleading
  310 failures (`routing table file readable` FAIL). Correct invocation
  is from the repo root with both paths explicit:
  `./services/core-cpp/build/routing_test contracts/routing-table.md
  services/core-cpp/src/dispatcher.cpp` → **944 PASS, 0 failures,
  83 templates, 116 harvested callbacks**. ctest passes absolute paths,
  which is why the suite is green. Pre-existing harness quirk (Phase 2
  test), not a Phase 4 regression — not fixed here.
- Same class of quirk: `uds_test` needs the `stub_backend` argument
  (bare run stops after 16 PASS, `stub did not start`); with the arg:
  40 PASS, `ALL UDS TESTS PASSED`. `config_test` needs the repo root as
  CWD (reads `parlay.toml.example`); from there: 55 PASS, ALL PASS.

### Dispatch — real C++ `IpcClient` vs real backend (dummy creds)
- Rebuilt `tests/interop/core_peer.cpp` + real backend binary from this
  tree; backend started on a scratch data dir, yt-dlp daemon warm.
- Result: **64/65 actions answered with a real response frame.**
  The single miss is `vc.join` → TRANSPORT FAILURE (no response), the
  known sandbox failure: `vc.join` Connect blocks past 60 s on the fake
  SDP with no timeout (Phase 6 work, explicitly not fixed here).
  Identical to the Phase 3 baseline (64/65, same single miss).

### Kill-9 / restart persistence proof — the new buckets (done for real)
Scratch data dir `/tmp/p4-kill9/data`, real backend binary from this
tree, real UDS calls via a minimal Python frame client + a helper
binary using the production store/cache/library code (built temporarily
inside the module tree, deleted before commit):
- Seeded through the running backend (UDS): `download.prefs`
  `{user_id: 4242, format: "flac", use_youtube: true}`; `fx.apply`
  `{chat_id: -100111222333, effect: "bass-boost", params: {gain: 2.5}}`
  (answered `pending_dsp: true`); `download.youtube` async with a bogus
  URL → run `2f0e70a7f49c`, reached terminal phase `failed` honestly
  (yt-dlp: video unavailable) in ~10.7 s.
- Seeded through production code while the backend was stopped:
  cache entry `jiosaavn:p4test123` (via `resolver.Cache.Put`) and a
  library index entry (via `library.Index.Record`, file
  `library/p4test.mp3` present).
- SIGTERM → restart on the same `parlay.db`: prefs + run read back live
  via UDS (`download.prefs` get → flac; `download.progress` → failed).
- **SIGKILL (`kill -9`) → restart on the same `parlay.db`**: backend
  reopened cleanly, listened again; prefs + run read back live via UDS
  with identical values.
- Final read-back via production code after stopping the backend:
  **all five items intact** — cache `ok=true` (title "P4 Kill9 Test",
  stream expiry 1793720295 = now+3600); library `ok=true` (path +
  size 12345); room fx `preset="bass-boost" params=map[gain:2.5]`;
  run `id="2f0e70a7f49c" phase="failed"` with the honest yt-dlp error;
  prefs `format="flac" use_youtube=true`.
- Side observation (correct behaviour, not a bug): with user 4242's
  prefs set to `flac`, a `download.youtube` call for that user is
  refused with `engine_unavailable` ("the yt-dlp daemon extracts mp3
  only; set format to mp3 in download.prefs") — the run above used a
  different user id with default mp3 prefs.

### Cache / prefetch exit benchmarks (re-measured from my runs)
- Cached `/play` resolve: `TestRegistryResolveUsesCache` —
  **uncached 237.967µs, cached mean 11.583µs, cached total 100×
  1.15839ms, stub resolver called exactly once** (the other 99 resolves
  hit cache). Sub-millisecond cached resolve, ~20× faster than uncached.
  (Worker B's earlier runs: 96.804µs and 10.158µs means — same shape,
  spread is sandbox load, not cache behaviour.)
- Prefetch never blocks: `TestPrefetchNeverBlocks` (resolver blocked on
  a channel) — **Prefetch returned in 1.823µs** while the prefetch ran
  in its own goroutine.
- Prefetch lead: `TestPrefetchBuffersOpeningSeconds` passes (Ready is
  the signal; byte-exact reader output, buffered prefix + live body);
  configured lead is `prefetch_lead_time_sec = 30` (default). The lead
  is proven at unit-test level, not against a real playing track
  (see §5).
- Expiry proof: `TestCachePutGetExpiry` (fake clock) and
  `TestRegistryResolveReResolvesAfterExpiry` pass — an expired entry is
  re-resolved (stub calls 2), fresh URL served, stale never served.

### Greps (all zero, as required)
- `TODO|FIXME|HACK|unimplemented` in `backend-go` production code
  (cmd + internal, excluding tests): **0**. In tests: **0**.
  In `core-cpp/src`: **0**.
- `mongo|redis` (case-insensitive) in `services/core-cpp/src`,
  `tests`, `CMakeLists.txt`: **0**.
- `/home/hatch` literals in `backend-go` production code: **0** (one
  intentional occurrence remains: the test assertion in
  `internal/resolver/cache_test.go:365` guarding against it).
- No HTTP listeners on 8001–8010 / 3000 / 8080 (`ss -ltnp`, none found;
  the backend binds the UDS socket only — verified in its startup log).
- `git ls-files data/`: **0 tracked files** — no migration needed under
  the OD-2 fresh start; `data/` is untracked runtime state.
- Legacy `play_mode`: core input parser accepts `"admin"` (also
  `"admins"`/`"adminonly"`), stores `"admins"`; gate reads `"admins"`.
- `[ipc]` fallback chain verified in code + passing tests
  (`TestIPCSectionQ6`, `TestExampleFileLoads`): env `PARLAY_UDS_PATH` >
  `[backend] uds_path` > `[ipc] socket_path` > `<data_dir>/parlay-backend.sock`;
  the shared example loads under both parsers (Go `TestExampleFileLoads`,
  core `config_test` from the repo root).

## 3. Reconciliation findings

All worker claims reproduced except the two invocation quirks in §2
(routing_test / uds_test bare runs — pre-existing, ctest is green) and
one stale paragraph: Worker A's §6 describes Q9 blocker (a) as open,
but the Q9 fix landed in the tree (core stores `"admins"`, §1) and all
tests pass. No code fixes were needed from Worker E; nothing was
changed beyond this report, the commit, and the push.

## 4. Commit

One commit on main with all 59 Phase 4 working-tree files
(51 modified + 8 new). `phase4/` is gitignored (old build artifacts /
tarballs); only these five files were force-added:
`phase4/PHASE4.md`, `phase4/worker-a-state.md`,
`phase4/worker-b-cache.md`, `phase4/worker-c-contracts.md`,
`phase4/worker-d-fx.md`.

## 5. Honestly unmeasured / not verified live

- No live voice call (no real group call, no real `vc.join` — sandbox
  UDP EPERM + no direct TLS to Telegram DCs, same as Phase 3).
- No live provider downloads: the only real download attempted was a
  bogus YouTube URL, which failed honestly at the resolver. No real
  CDN / JioSaavn / Spotify / Deezer / NetEase / Navidrome / Soulseek
  traffic in this verification.
- No live Telegram / MTProto verification of any UI change (Q3 card,
  Q4 toasts, settings panels).
- Cache timings are stub-resolver timings, not real CDN timings; the
  ~20× cached speedup is real but the absolute µs values will differ
  against a live resolver.
- Prefetch lead (30 s) proven at unit-test level only — no real
  playing track was pre-buffered end to end.
- The fx DSP consumer does not exist yet (Phase 5): `fx.apply`
  answers `pending_dsp: true` and stores the preset; nothing applies
  it to a stream today.

## 6. Open flags (need a user decision or a later phase)

1. **Q13 — VERSION stays `v0.2.0`.** Not decided, not bumped anywhere.
   Contract version stays `0.3.1`.
2. **FINAL-ARCHITECTURE.md §10 parity anchor still says 56 actions**
   ("The parity target is 56 actions (§10)") while the live contract
   is 65 (56 + 9 OD-3). Locked doc NOT edited — carried as a flag.
3. **Both-keys-set `[ipc]` hazard (no runtime warning implemented).**
   If `[backend] uds_path` and `[ipc] socket_path` are set to different
   values, the setup silently splits (backend listens on the former,
   core connects to the latter). Documented in the example comment and
   the `SocketPath()` doc comment ("set at most one file key, prefer
   `[ipc]`"); a startup warning when they differ was not implemented.
   Asymmetry stands: the core never reads `[backend] uds_path`.
4. **Q7 full rewire NOT done.** Panel settings still live in the core's
   `chat:<chat_id>` hash while the backend has `chat.settings` in bbolt.
   Blocker (a) (Q9 vocabulary) is now resolved, but blockers (b)–(d)
   remain: `clean_mode` shape split (core int minutes vs backend bool +
   int), no backend home for the `chat:` runtime fields (`loop`,
   `speed`, `bass`, `muted`, `rtmp_url`, `rtmp_key`, `cplay`,
   `last_track`, `logger_on`), and backend-down fallback semantics
   unruled. A panel-only rewire was explicitly rejected (live
   divergence would be worse than today's duplication).
5. **`settings.go:100` redundant override.** `st.AutoplayLang = ""` is
   now redundant since the default is `""`; left in place per the
   follow-up's scope.
6. **fx per-file record stays in-memory** (write-only `Service.files`);
   if Phase 5 wants per-file FX records to survive a restart, that
   needs a new store bucket. No `fx.get` UDS action exists.
7. **Legacy `IpcClient` URL-taking signatures retained** as thin
   wrappers for test doubles only; the dispatcher uses the no-URL forms.
8. **`scram` kept as a crypto module** — SCRAM client deleted, general
   primitives kept because production uses them.
9. **Q3 judgment call**: station name as the radio card's "host" — any
   host string would be a dead link in the locked topology.
