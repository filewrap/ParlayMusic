# Worker C — Q7 settings rewire + parity re-verification + sandbox benchmarks

Parlay Phase 6. Worker C brief: Q7 full settings rewire, the `uds_path` vs
`socket_path` hazard, §10 parity re-verification, sandbox benchmarks, and the
>50 MB upload path proof. All work uncommitted, nothing pushed, `.env`
untouched. `FINAL-ARCHITECTURE.md` not edited (locked).

**Headline numbers**

| Check | Result |
|---|---|
| Q7 settings rewire | Done, tested (10 new CHECKs) |
| `uds_path`/`socket_path` hazard | Warning implemented + one false positive found and fixed live (6 test cases) |
| Dispatch, real C++ `IpcClient` vs real backend | **65/65, twice** (two fresh backends, exit 0) |
| `ctest` (core-cpp) | **10/10, 0 failed** (32.85 s), 1,636 PASS lines |
| Go `go vet` + `gofmt` + `go test ./...` | vet clean, fmt clean, **29/29 pkgs, 326 passed, 0 failed, 12 skipped** |
| Rust DSP, release | **13/13 passed**, clippy 0 warnings; throughput **31.8x realtime** |
| Rust DSP, debug | 12/13 — throughput test fails at 1.0x realtime (debug-build artifact, see §5) |
| Opus ladder | 192k / 256k / 320k readbacks all match config; bogus profile fails loudly |
| >50 MB upload proof | Path-only `inputFileLocal` request proven by test (7 new CHECKs) |
| TODO/FIXME/HACK/unimplemented in my code | **0** |

Note on the tree: this run shared the working tree with parallel Phase 6
workers. Their uncommitted files are present (`services/backend-go/internal/voice/*`
connect-timeout work, `internal/uds/*`, `internal/httpapi/`, `services/miniapp/`,
`cmd/backend/main.go` HTTP + timeout wiring) and were **not touched** by this
worker. The Go suite numbers (326 passed) therefore cover the shared tree;
the C++ numbers and the config-package numbers are this worker's. The
`vc.join` dispatch pass (§3) is attributable to the parallel worker's
in-tree connect-timeout work — see §3.

## 1. Q7 — full settings rewire: RESOLVED

Locked rule applied: backend bbolt is the single home for settings, all 15
keys; core `state.json` keeps only boot/ephemeral state, never a second copy
of settings.

### What the survey found

- Backend `chat.settings` already implements all 15 keys
  (`services/backend-go/internal/settings/settings.go`): `admin_mode` 0–2,
  `play_mode` everyone|admins, `cmd_delete`, `clean_mode` bool,
  `clean_delay_min` 0–1440 (sets `CleanMode = n > 0`), `thumbnails`,
  `autoplay`, `autoplay_lang` (default `""`), `autoplay_mood` (default `""`),
  `silent_mode`, `auto_delete`, `auto_delete_min` 2–7, `language` must be
  `"en"`, `paginate_purge_old`, `chat_music_download`. Store: bbolt
  `settings` bucket, key `chat:<chat_id>`. **No backend change needed.**
- Core still read/wrote all settings from the `chat:<chat_id>` hash in
  `state.json` via `chatField`/`setChatField`/`chatIntField`/`chatSpeedField`
  (~111 call sites). Core's `ChatSettings` had 12 fields (missing
  `autoplay_lang`/`autoplay_mood`); core stored `ap_lang`/`ap_mood`
  separately (defaults `"auto"`/`"any"`, vs backend `""` = any).

### Design (as implemented)

- New `Dispatcher::settingsFor(chat_id)` — UDS GET-all
  (`GET /v1/chat/settings?chat_id=X`), in-memory cache only (live entries
  TTL 60 s, non-live 5 s), mutex-guarded. Backend down → cached snapshot if
  present, else locked defaults; `settingsLive(chat_id)` reports false so
  status replies append `"(settings unreachable — showing defaults)"`.
- `setSetting(chat_id, key, Json value)` → bool; POSTs one key, refreshes the
  cache from the set response's settings object. All command/panel writes
  report honest failure instead of claiming success.
- `saveChatSettings` now returns bool and posts only changed keys (diff vs
  fetched snapshot), posting `clean_delay_min` first then derived
  `clean_mode`. `cbSet` keeps a `before` snapshot and re-renders from it on
  save failure; new toast `kSettingsSaveFailed = "Could not save settings —
  the backend is unreachable. Nothing was changed."`
- Runtime fields moved to a new `chatrt:<chat_id>` hash in `state.json` via
  `chatRtField`/`setChatRtField`/`chatRtIntField`/`chatRtSpeedField`; reads
  fall back to the legacy `chat:` hash once (pre-rewire cplay/rtmp survive
  upgrade); writes go to `chatrt:` only. `volume` also moved to `chatrt:`.
  Chat-level `logger_on` read dropped (dead — never written as a chat field
  anywhere; only the global `logger_on` key is set by `/logger`).
- `autoplay_lang`/`autoplay_mood`: backend `""` ↔ core display `"auto"`/`"any"`;
  core posts `""` when the user picks auto/any.
- `vcTarget()` reads `cplay` via `chatRtField`; `thumbnailsOn()` uses
  `settingsFor(chat_id).thumbnails`.

### Q7 items, one by one

- **(a) Q9 vocabulary — RESOLVED (Phase 4, confirmed this run).** Core
  stores `"admins"`, backend validator accepts only `"everyone"`/`"admins"`;
  exercised by the `play_mode="admins"` tests, which pass.
- **(b) `clean_mode` shape split — RESOLVED.** Core posts only
  `clean_delay_min` (int minutes, 0 = off); backend derives `clean_mode`.
  `/cleanmode 30` → stored `clean_delay_min=30`, `clean_mode=true`;
  `/cleanmode off` → `0` / `false`. `onCleanmode` keeps its 1–1440
  validation and 15-min default.
- **(c) No backend home for `chat:` runtime fields — RESOLVED.** `loop` /
  `speed` / `bass` / `muted` are core-voice session state (the backend room
  is engine-session state — a different session, not conflated); `cplay` =
  channel-play link; `rtmp_url`/`rtmp_key` = RTMP ingest; `last_track` =
  last played title; `volume` = player-callback stepping only. All now live
  in the `chatrt:` hash with a one-time legacy `chat:` read fallback.
- **(d) Backend-down fallback semantics — RESOLVED as fail-open-with-defaults,
  policy FLAGGED.** Gates fail OPEN on locked defaults while the backend is
  down, with a stderr warning (`[settings] chat.settings fetch failed for
  chat …; serving cached/default settings`) and the degraded note on replies.
  No second copy in `state.json`, per the lock. **Flag for the user:**
  fail-open vs fail-closed is still a policy call — fail-closed would be a
  one-line change at `playAllowed`/`canManageVc`. This worker implemented
  fail-open because the gates previously worked offline from `state.json`
  and failing closed would newly deny every group while the backend is down;
  the user may rule otherwise.

### Q7 tests (10 new CHECKs, all passing)

FakeIpc gained a scripted settings store (`settings_store` map chat→JSON,
`settings_fail` flag): GET `/v1/chat/settings` returns stored-or-defaults
15-key object; POST `chat.settings` applies one key verbatim (and keeps
`clean_mode` in sync with the delay, like the real backend); `seedSetting`
helper. Seeds updated: muted/speed/bass/loop/rtmp → `chatrt:`;
`play_mode="admins"` and `page_purge` → FakeIpc settings seeds. New §Q7
section:

- (a) `/settings` renders from the backend GET; `chat:` hash holds no
  `play_mode`/`clean_delay_min`.
- (b) Panel toggle posts exactly one `chat.settings` with
  `key=play_mode, value=admins`; stored in the backend; toast
  "Settings updated"; nothing in `state.json`.
- (c) Clean-mode split, both directions (see above).
- (d) Backend down: `/playmode` shows defaults + degraded note;
  `/playmode admins` fails honestly ("Could not save play mode"), nothing
  stored.
- (e) Runtime hash + legacy fallback: `/loop` reads `chatrt:`; legacy
  `chat:` cplay is read but NOT migrated; new `/loop 2` write lands in
  `chatrt:` only.
- (f) After playmode + cleanmode + thumbnail, the `chat:` hash is empty —
  the backend is the only copy.

Grep: zero remaining `chatField`/`chatIntField`/`setChatField`/`chatSpeedField`
calls in `dispatcher.cpp`/`dispatcher.hpp`.

## 2. `uds_path` vs `socket_path` hazard: RESOLVED (with a false positive found live)

Implementation (`services/backend-go/internal/config/config.go`,
`cmd/backend/main.go`):

- `Config.SocketPathSplitWarning() string` — `""` when consistent, else a
  message naming both paths. Startup logs
  `backend-go: WARNING: <warning>` (warning, not fatal — a split setup still
  starts).
- `parlay.toml.example` documents the hazard in both the `[ipc]` and
  `[backend]` sections and now mentions the startup warning.

**False positive found and fixed during the live dispatch run:** the first
implementation warned whenever `PARLAY_UDS_PATH` was set and disagreed with
the `[ipc]` default — but the C++ core reads `PARLAY_UDS_PATH` too
(`services/core-cpp/src/config.cpp:326`, env wins over `[ipc] socket_path`),
so an env-set path is consistent on both sides. Fix: new
`BackendConfig.UDSPathFromEnv` bool, set in the env pass; the warning returns
`""` when the path came from the env. Caveat documented in the code: this
assumes the core runs with the same env; a split env is invisible to the
check. Verified live: backend started with `PARLAY_UDS_PATH` set → **no
warning**; the file-only split case still warns (unit test).

`TestSocketPathSplitWarning` now has **6 cases** (no backend key, agreeing
keys, backend-only, differing keys, ipc-only, env path) — all passing.
Core side cannot warn (it never reads `[backend] uds_path`); the backend
startup warning + example comment is the clean fit.

## 3. Parity re-verification (§10)

- `contracts/ipc.json`: **65 actions, 65 unique names, 0 duplicates**,
  version 0.3.1.
- Exactly-once test `TestRegisterAllExactlyOnce`: **PASS** (65 contract
  actions + ping = 66 registered).
- `contracts/routing-table.md`: **249 data rows, 0 blank cells**.
- `routing_test` (ctest invocation, absolute paths): **944 PASS, 0 failures**
  (83 templates, 116 harvested callbacks). A repo-root relative invocation
  gives 943 PASS + 0 failures; a bare invocation fails (pre-existing harness
  quirk, same class as `uds_test`/`config_test` needing argv paths — not a
  regression).
- **Dispatch check, real C++ `IpcClient` vs real backend (dummy creds,
  scratch data dir, `-tags parlaydsp`): 65/65, twice.**
  - Run 1: `CORE_PEER: 65/65 actions answered, 0 failures`, exit 0, ~2 s.
  - Run 2 (fresh backend): same, exit 0.
  - `vc.join` answers fast with an honest error:
    `engine_unavailable (voice join failed: voice: join chat -1001:
    connect: ntgcalls error -102: Invalid JSON)`; `join_request handler
    ran: yes`; `vc.lyrics` answers honestly right after
    (`bad_request: no title to look lyrics up for`) — no wedge, no
    `abandoned past deadline` in the backend log. No action answered
    `unknown action` in either run.
  - **Attribution:** the `vc.join` fast-fail is the parallel worker's
    in-tree connect-timeout work (`voice.go`: `DefaultConnectTimeout =
    30s`, `ConnectionTimeout` terminal state; `main.go`:
    `voice.NewWithConnectTimeout(timeouts.join_ms)`; new
    `connect_timeout_test.go`), present uncommitted in the shared tree when
    this was measured — not this worker's code. Phase 4/5 measured 64/65
    with the same single miss; with that fix in-tree the count is 65/65.
    A real group-call join is still VPS-only (the probe's fake SDP is
    rejected as invalid JSON; a live call was never attempted here).

**§10 flag (do not fix the locked doc):** `FINAL-ARCHITECTURE.md` §10 still
says the parity target is 56 while the live contract is 65 (56 + 9 OD-3).
Flagged only, per instructions.

## 4. Sandbox benchmarks (measured honestly)

### (a) Cold `/play` resolve — cache-hit path

`TestRegistryResolveUsesCache` (`-count=1 -v`), fresh run:

- uncached resolve: **676.807 µs**
- cached mean: **15.882 µs**
- cached total (100x): **1.588276 ms**
- stub resolver calls: **1** (exactly-once fetch)

(Phase 4 numbers were ~237 µs / ~11.6 µs — same shape, ~40x cached speedup;
absolute values are machine noise.) **Not measured:** live YouTube/JioSaavn
resolves — sandbox TLS interception blocks them and there are no creds here.
Live numbers must come from the VPS.

### (b) Opus bitrate ladder — config honored, readback matches

Real backend binary, one restart per profile (startup log line):

| Profile | Readback |
|---|---|
| `standard_192k` (default) | `bitrate=192000 complexity=10 vbr=true fec=false dtx=false frame=60ms maxplay=48000` |
| `high_256k` | `bitrate=256000 complexity=10 vbr=true fec=false dtx=false frame=60ms maxplay=48000` |
| `ultra_320k` | `bitrate=320000 complexity=10 vbr=true fec=false dtx=false frame=60ms maxplay=48000` |

Bogus profile → fatal at startup, no UDS listen:
`quality.voice_profile: must be one of ultra_320k, high_256k, standard_192k`.
(Opus `signal` field: OPUS_AUTO + APPLICATION_AUDIO, no signal field —
documented deviation from Phase 5 §6b, unchanged.)

### (c) Gapless / zero-drop — real DSP + ntgcalls sink path (release)

`cargo test --release` in `services/parlay-dsp` (Phase 5 E2E-probe approach):

- All formats, 4 s files: **201 frames**, 192,960 samples/ch, RMS
  0.059–0.063, transitions `[]` (wav/flac/mp3/ogg/opus); m4a 202 frames /
  193,920 samples (one extra frame, encoder priming).
- Gapless two-track: `max|two-one| = 0` (bit-exact concatenation).
- Gapless cross-rate: max zero run `= 0` (no dropped samples at rate change).
- Loop: exactly one transition `[2]` (loop restart), `max|loop-once| = 0`.
- Streaming: frames `= 201`, RMS `0.0592`, underruns `= 1`.
- Opus: ffmpeg 192,000 vs engine 192,960 samples/ch, diff `= 960` (one frame
  of priming).
- Frame geometry 3840 = 2 × 1920 bytes (exact-1920 pushes), as in Phase 5.

### Rust DSP suite status (flag, not a regression)

- **Release: 13/13 passed** (4.02 s), clippy **0 warnings**.
- **Debug: 12/13** — `first_frame_latency_and_throughput` fails:
  throughput **1.0x realtime** (10.02 s audio in 9.44–9.98 s across 4 runs)
  vs the 5x bar; first-frame latency fine (34.6–36.8 ms < 500 ms).
  **Release run of the same test: 31.8x realtime** (10.02 s in 315 ms),
  first-frame 3.8 ms. Root cause is debug-build slowness of the rubato sinc
  resampler (`SincFixedOut`, unoptimized), not a code change — `git status`
  shows `services/parlay-dsp/` untouched since Phase 5, and the production
  path links `target/release/libparlay_dsp.so` (cgo LDFLAGS in
  `internal/dsp/cgo.go`). The debug failure has zero production impact, but
  the debug suite is red until the 5x bar is either relaxed for debug builds
  or the test is run in release. **Flagged for the coordinator, not fixed
  here** (changing the bar is a spec call, and this worker owns no DSP
  changes).

## 5. >50 MB upload path: proven (synthetic), two gaps flagged

### The path (verified in code + test)

Bot → Telegram is `TdLibTelegramSender::sendLocalFile` (`dispatcher.cpp`)
→ `sendWithContent` → TDLib `sendMessage` with `inputMessageDocument` /
`inputMessageAudio` + `inputFileLocal{path}`. **No size cap in this path;
the core never reads the file** (passes a path; TDLib streams/chunks it
internally via MTProto). Errors are honest: `sendWithContent` returns -1 +
TDLib's exact error (`"400: <message>"`) on timeout/error/no-id.

### Proof test (7 new CHECKs, `dispatcher_test.cpp` §Q7b)

60 MiB **sparse synthetic file** (real size, zero bytes on disk) through the
real sender against a fake bot-session client that answers
`parseTextEntities` and captures `sendMessage`:

- `sendFile` >50 MB succeeds (message id 777); exactly one parse + one
  sendMessage; request is `inputMessageDocument` / `inputFileLocal` with
  the path only, **<2 KiB total — no file bytes embedded**.
- `sendAudioFile` >50 MB succeeds; request is `inputMessageAudio`.
- TDLib error surfaces verbatim (`"400: FILE_TOO_BIG (test)"`, id -1).
- The file is **streamed, not loaded into memory**: the request carries the
  path string only, and the core never opens the file (no read of the 60 MiB
  anywhere in the path). Live >50 MB delivery to real Telegram is the
  user's test, not this one.

### The 50 MB cap, where it actually lives

Upstream of the upload: backend `MaxBotAPIBYTES = 50 * 1024 * 1024`
(`internal/download/registry.go`); `finishFile` flags `oversize` (file IS
kept on disk, path returned). NetEase differs: its `Download` **aborts**
past 50 MB (truncated file + error `file exceeds the 50 MB Bot API cap
(%d bytes written, truncated)`).

### Gap 1 — FLAGGED, not redesigned: "queued for a later send" promises a send that never happens

Core replies "That file is over the 50 MB send cap — queued for a later send
instead of failing" on `oversize:true`, and `contracts/ipc.json` documents
the queue-for-later-MTProto behavior — but **no later MTProto pass exists in
code**: `dlrun:` consumers are only the lossy accept/skip flow and cbDl
fx/zip/1by1; nothing ever sends an oversize-flagged file. Recommended fix
(for the parent to order): send oversize files immediately via the
proven-uncapped `sendLocalFile` path.

### Gap 2 — FLAGGED: NetEase oversize behaves differently from the other providers

NetEase >50 MB returns an **error** (other providers return `oversize:true`
with the file kept). The truncated partial file **is** cleaned up — verified
by code path: `executeJob` snapshots the dest dir before the job and calls
`removeNewFiles(dest, before)` on error, and the NetEase partial is a new
top-level file in destDir (`filepath.Join(destDir, name)`), so it is deleted.
Not live-proven (needs a real >50 MB NetEase file + login; sandbox can't).
So a NetEase oversize file is deleted, never kept for a later MTProto pass —
inconsistent with the other providers' keep-file behavior. Same recommended
fix as Gap 1 (send via `sendLocalFile` instead of erroring), or an explicit
decision that NetEase oversize is an error by design.

## 6. Verified counts (fresh runs this session)

### C++ (`services/core-cpp`, `ctest`, 10/10, 32.85 s)

| Suite | PASS |
|---|---|
| config_test | 55 |
| uds_test | 40 (needs the stub path argv, as ctest passes it; bare run fails — pre-existing harness quirk) |
| rate_limiter_test | 28 |
| dispatcher_test | 313 (was 305 + 8 failures mid-run; all 8 were stale `chat:`-hash assertions, fixed — 7 `hgetall` renames + 1 wrong error-string expectation) |
| routing_test | 944 (ctest absolute-path invocation; 0 failures) |
| tdlib_test | 46 |
| telemetry_test | 5 |
| ui_test | 160 |
| scram_test | 16 |
| store_test | 29 |
| **Total** | **1,636** (Phase 5 total 1,628; +8 net. Dispatcher is +17 new checks — Q7 10 + upload 7 — so 9 checks differ elsewhere vs the Phase 5 total; this worker removed no checks, and the Phase 4→5 per-suite arithmetic doesn't reconcile from the reports alone. Reported as measured.) |

### Go (`services/backend-go`)

`go vet ./...` clean, `gofmt -l` clean, `go test ./... -count=1`:
**29/29 packages ok, 326 passed, 0 failed, 12 skipped.** (Phase 5 baseline
28/28 pkgs, 307 passed, 12 skipped — the extra package is
`internal/dsp/fake`, +19 tests; the suite also covers parallel workers'
uncommitted voice/uds/httpapi work.)

### Rust (`services/parlay-dsp`)

Release: **13/13 passed**, clippy **0 warnings**. Debug: 12/13 (throughput
test only — see §4c).

## 7. Files changed (this worker only, 9 files, uncommitted)

| File | Change |
|---|---|
| `services/core-cpp/src/dispatcher.cpp` | Q7 rewire (~590 lines diff): `settingsFor`/`settingsLive`/`setSetting`, `chatrt:` runtime accessors, all call-site rewrites, `saveChatSettings` bool + diff-only posts, `cbSet` snapshot/rollback, degraded suffix |
| `services/core-cpp/src/dispatcher.hpp` | New settings/runtime declarations; `saveChatSettings` → bool; `SettingsCacheEntry` + mutex/cache members |
| `services/core-cpp/src/ui_texts.hpp` | `ChatSettings` + `autoplay_lang`/`autoplay_mood`; `kSettingsSaveFailed` |
| `services/core-cpp/src/store.cpp` + `store.hpp` | Key-inventory comments to post-rewire state (`chat:` legacy read-only fallback; `chatrt:` runtime; 15 settings keys never in `state.json`) |
| `services/core-cpp/tests/dispatcher_test.cpp` | FakeIpc settings store + `seedSetting`; seed renames (`chat:` → `chatrt:`); settings-seed conversions; 7 `hgetall` assertion fixes; new §Q7 (10 CHECKs) + §Q7b upload proof (7 CHECKs) |
| `services/backend-go/internal/config/config.go` | `SocketPathSplitWarning()` + `UDSPathFromEnv` (env false-positive fix) |
| `services/backend-go/internal/config/config_test.go` | `TestSocketPathSplitWarning`, 6 cases |
| `parlay.toml.example` | Hazard comment now mentions the startup warning |
| `services/backend-go/cmd/backend/main.go` | Startup warning call (other workers' httpapi/timeout changes in the same file untouched) |

Not changed: `FINAL-ARCHITECTURE.md` (locked; §10 flag above), `~/workspace/parlay/.env`
(untouched), `services/parlay-dsp/` (untouched — git clean).

## 8. What remains / needs the live VPS

- Live YouTube/JioSaavn `/play` resolve timings (sandbox TLS interception).
- Live >50 MB delivery to real Telegram (sandbox proof used a synthetic
  sparse file + fake client by design).
- Live group-call `vc.join` (probe's fake SDP is rejected as invalid JSON;
  a real call was never attempted here).
- Live verification of the Q7 rewire on a real group (settings panel,
  toggles, clean-mode, backend-down behavior) — sandbox tests use FakeIpc.
- Live verification that the `uds_path` warning fires/doesn't fire in the
  real deployment layout (env vs file keys).
- The debug-mode DSP throughput test (12/13) — release is green at 31.8x;
  the bar-vs-debug-build question is a coordinator call.

## 9. Flagged, not resolved (do not guess)

1. **Fail-open vs fail-closed for backend-down gates** (§1d) — implemented
   fail-open with defaults + degraded note; one-line change to fail closed if
   the user rules so.
2. **§10 "56" vs live 65** (§3) — locked doc, flag only.
3. **"Queued for a later send" with no later pass** (§5 Gap 1) — recommend
   sending oversize files immediately via `sendLocalFile`.
4. **NetEase oversize = error + delete, others = keep + flag** (§5 Gap 2) —
   inconsistent; same fix as (3), or rule it by design.
5. **Debug DSP throughput 12/13** (§4c) — release is 31.8x; bar-vs-debug is
   a coordinator call.
6. **vc.join 65/65 attribution** (§3) — measured with the parallel worker's
   connect-timeout fix in-tree; not this worker's code.
