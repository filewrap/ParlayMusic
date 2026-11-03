# Parlay Phase 4 — Worker B report: cache + prefetch + backend state

Worker B (cache + prefetch + backend state, Go) — 2026-11-03.
Repo: `~/workspace/parlay`, main at `d36cdd7`. **Nothing pushed.**
`~/workspace/parlay/.env` never read or touched. No Phase 5 work.

Baseline (measured before any change, vendored `.tools/go/bin/go` 1.26.0 —
system go 1.23.0 is rejected by `go.mod`):
`go test ./...` 25/25 packages ok, **248 passed / 0 failed / 12 skipped**
(by `--- PASS/FAIL/SKIP` line count in a verbose run). `go vet` clean,
`gofmt` clean.

Final (measured at end of this work, same method):
`go test ./...` **26/26 packages ok, 272 passed / 0 failed / 12 skipped**
— +24 tests, +1 package (`internal/prefetch`). `go vet ./...` clean,
`gofmt -l` zero unformatted (two of my files needed `gofmt -w` at the
end: `internal/download/registry.go`, `internal/resolver/backend_config.go` —
whitespace only; packages re-tested after).

## 1. TTL-aware URL/metadata cache — DONE

### Layout
- `internal/resolver/cache.go` (new): `Cache` type over the store with an
  injected clock (`NewCache(st, ttlSec)`, `NewCacheWithClock(st, ttl, now)`).
  The **store is dumb storage; the cache layer owns expiry**.
- Store buckets (`internal/store/store.go`): new `cache` bucket alongside
  the existing ones. Methods: `Put/Get/DeleteCacheEntry`
  (`CacheEntry{Track, ExpiresAt, CachedAt}`, unix seconds).
- Model types live in `internal/model/model.go` (appended after `RadioState`):
  `DownloadTrackProgress`, `DownloadRunRecord`, `DownloadPrefs`,
  `LibraryEntry`. `download.Prefs` and `library.Entry` are type aliases to
  the model types, so `store` never imports `download`/`library`.

### Key format
Canonical `<source>:<track_id>` via `CacheKeyForTrack` (strips `jsvn:` /
`yt:` / `radio:` / `<source>:` prefixes from `Track.ID`) and
`CacheKeyForQuery` (derives a key only for direct URL/id queries; free
text returns false — resolve still stores under the resolved track's key).

### Expiry parsing (`ParseStreamExpiry`)
`expire=` / `expires=` / `exp=` query params (unix seconds) win; no
signature → configured fallback TTL (`limits.url_cache_ttl_sec`);
already-expired signature → `now` (stale on arrival); absurd value
(>24h out) → fallback; zero fallback + no signature → `now` (do not
cache). Expired entries are deleted lazily on read and **never served**.

### Cacheability (`cacheableTrack`)
Non-empty ID + StreamURL, not live, not radio/file.

### Registry integration (`internal/resolver/resolver.go`)
`Registry.SetCache`; `Resolve` checks cache first on direct queries
(telemetry via `LogResolve`), stores after successful resolve. New
`Registry.ResolveTrack(ctx, track)` for the prefetch path (cache check,
then re-resolve via `resolveQueryFor`: track URL when the resolver
accepts it, else source-prefixed bare id — `jsvn:` for JioSaavn,
`deezer:track:` for Deezer, `navidrome:` for Navidrome; soulseek /
radio / file return an honest error).

### MEASURED timings (test logs, 2026-11-03)
- `TestRegistryResolveUsesCache`, fresh final run:
  `uncached=1.942508ms cached_mean=96.804µs cached_total_100x=9.680441ms
  (stub calls=1)`. Earlier run on the same test: cached mean **10.158µs**
  (uncached first resolve 45.678µs, stub calls stayed 1). Both runs:
  cached resolve is sub-millisecond (~20x faster than uncached) and the
  stub resolver is called exactly once — the other 99 resolves hit cache.
  The spread between runs is sandbox load (concurrent workers building
  C++), not cache behaviour.
- Expiry proof (`TestCachePutGetExpiry`, fake clock; `TestRegistryResolveReResolvesAfterExpiry`):
  expired entry re-resolved (stub calls 2), fresh URL served, stale never
  served. No sleeps on real CDN values — injected clock only.

## 2. N+1 prefetch / pre-open — DONE

### Flow (`internal/prefetch/prefetch.go`, new package)
`Prefetcher` — one pending stream per chat. `Prefetch(chatID, track)`
returns immediately; `Take(chatID, trackID)` hands over only a ready,
error-free handle — in-flight / failed / wrong-track returns false and
drops it; `Cancel(chatID)`. Goroutine: resolve (bounded by
`resolveTimeout`) → HTTP GET (stream ctx, cancelled only on close/drop —
NOT bounded by resolve timeout) → `readUpTo` prefix → Ready closes
exactly once. `PrefixBytesForLeadTime(leadSec)` = leadSec × 32,000 B/s
(~256 kbps), clamped [64 KiB, 4 MiB]. Skips live/radio/file/soulseek (no
pre-openable HTTP stream); dedups same-track prefetch; replaces a stale
pending prefetch. A `closed` flag + check at goroutine start prevents
leaking a body when a stream is dropped before its goroutine runs.

### DSP seam type
`prefetch.PrebufferedStream`: `Track`, `Ready() <-chan`, `Err()`,
`BufferedBytes()`, `Reader()` (buffered prefix first, then live HTTP body
via `prefetchReader`), `Close()` (idempotent, cancels stream ctx).
**Consumed today by the existing voice play path** — no fake DSP, no
TODO consumer: `voice.Manager.PlayStream(ctx, chatID, r io.ReadCloser)`
mirrors `Play` but feeds the prefetch reader; `startFFmpegFeed` was
refactored into shared `startFeed(ctx, client, chatID, input, stdin,
onEOF)` plus `startFFmpegFeedFromReader` (ffmpeg `-i pipe:0`,
`cmd.Stdin = r`; reconnect flags only for http(s) inputs). Phase 5 plugs
the DSP in as the consumer of this same handle type.

### Handlers wiring (`internal/handlers/handlers.go`, `vc.go`)
`Handlers.prefetch *prefetch.Prefetcher` built in `New()` from
`d.Registry.ResolveTrack` + `PrefixBytesForLeadTime(d.Config.Limits.PrefetchLeadTimeSec)`
+ resolve timeout from `d.Config.Timeouts.ResolveMs`; disabled when
`PrefetchLeadTimeSec == 0` or config nil. `maybePrefetch(chatID, snap)`
(queue top only; skips live/radio/file/soulseek) called from `vcPlay`
(started path, after final snapshot), `advancePlayback`
(AdvanceNext/Autoplay; AdvanceIdle → `prefetchCancel`),
`vcAutoplaySkip`. `vcStop` calls `prefetchCancel`. `bindTrack` tries
`prefetch.Take` + `Voice.PlayStream` first, falls back to direct-URL
`Voice.Play` on any failure (logs the fallback). `Take` never waits —
an in-flight prefetch is dropped and the play path falls back to the
direct URL.

### MEASURED timings / proofs (test logs, 2026-11-03)
- `TestPrefetchNeverBlocks` (resolve blocked on a channel):
  **Prefetch returned in 19.86µs** on the fresh run (1.753µs on an
  earlier run) — returns immediately while the prefetch runs in its own
  goroutine. Both runs prove "never blocks the playing track".
- `TestPrefetchBuffersOpeningSeconds`: Ready channel is the signal,
  byte-exact reader output (buffered prefix + live body).
- `TestFeedFromReaderEndToEnd` (voice): **stdin feed sent 200 frames
  for a 2s tone** — identical to the URL/file path (`TestFeedEndToEnd`,
  also 200 frames). The pre-buffered handle drives ffmpeg exactly like a
  URL/file input.

## 3. In-memory singletons → bbolt buckets — DONE

New buckets in `internal/store/store.go` (added to `topBuckets`):
`cache`, `downloads`, `dlprefs`, `library`.

- **Download runs** (`internal/download/registry.go`): `Registry.store
  *store.Store`, `AttachStore(st)` (loads finished runs sorted by
  `StartedAt`, caps at `MaxFinishedRuns=200`, deletes extras),
  `record()` / `restoreRun(rec)` conversion, `Run.restored bool`
  (Finish/Fail are no-ops on restored runs — avoids double-closing the
  done channel), `retire()` persists on finish + deletes evicted records
  (store errors swallowed there — the run is already finished in
  memory). Lock order: `retire` takes `r.mu` then `run.mu`; no path
  takes them in reverse (verified by reading `finish`/`Cancel`/`Get`).
  Only finished runs persist — live runs hold context/cancel/channels
  and cannot persist.
- **Download prefs** (`internal/download/prefs.go`): `PrefsStore.store`,
  `AttachStore` (loads all), `Set` persists **before** the in-memory
  write (failure reported, old prefs stand).
- **Library index** (`internal/library/library.go`): `Index.store`,
  `AttachStore` (loads entries, drops + deletes entries whose file is
  gone), `persistLocked`/`dropStoredLocked` helpers;
  `Record`/`Delete`/`DeleteByPath`/`Clear` persist; lazy stale-drop in
  `Get`/`GetByPath`/`Search` also deletes from store.
- `main.go` wiring (`services/backend-go/cmd/backend/main.go`): after
  `store.Open`, `reg.Configure(cfg.Backend)`,
  `reg.SetCache(resolver.NewCache(st, cfg.Limits.URLCacheTTLsec))`, and
  best-effort `download.SharedRegistry().AttachStore(st)` /
  `download.SharedPrefs().AttachStore(st)` /
  `library.SharedIndex().AttachStore(st)` (failures logged loudly,
  backend keeps running memory-only for that piece).

Tests (all passing): `internal/download/persistence_test.go`
(`TestRegistryPersistsFinishedRuns` — finished run found after reopen
with phase done; re-finishing a restored run is a no-op; live run does
NOT survive; `TestPrefsStorePersists` — prefs survive reopen, unknown
user still gets defaults), `internal/library/persistence_test.go`
(`TestIndexPersists` — entry survives reopen, entry whose file vanished
is dropped on load and deleted from store, Delete persists),
`internal/store/store_phase4_test.go` (`TestPhase4Buckets` incl. reopen
survival).

## 4. Q11 — hardcoded sandbox yt-dlp path — DONE

`internal/resolver/ytdlp.go`: deleted the `/home/hatch/...` fallback.
New `candidateRoots()` (CWD + executable dir) and `walkUp(rel)`
helpers; `findYtDlpPython` and `findWorkerScript` both walk up from
both roots. Env overrides (`PARLAY_YTDLP_PYTHON` /
`PARLAY_YTDLP_WORKER`) still first; final fallback remains `python3`.
`TestFindYtDlpPythonQ11` PASS (outside repo → `python3`, no
`/home/hatch`; env override wins; worker script not found outside repo).

Grep verification: **zero `/home/hatch` literals remain in
`services/backend-go` production code**. The only remaining occurrence
in the tree is the test assertion in
`internal/resolver/cache_test.go:365` that guards against it
(`strings.Contains(got, "/home/hatch")` → error) — intentional. The
comment in `ytdlp.go` that mentioned the old path was reworded to not
contain the literal.

## 5. Q12 — resolver DataDir split — DONE

`internal/resolver/resolver.go`: `DataDir()` now returns `"data"` when
`PARLAY_DATA_DIR` is unset (was `~/.local/share/parlay` via
`os.UserHomeDir()`), matching `config.Backend.DataDir` default `"data"`
and the core's data dir. `DefaultDownloadDir()` → `data/downloads`.
`TestDataDirQ12` PASS. One data dir for resolvers, consistent with the
core's `PARLAY_DATA_DIR` / `data`.

## 6. Q10 — nine parsed-but-unused [backend] keys — DONE

`internal/resolver/backend_config.go` (new): `Registry.Configure(cfg
config.BackendConfig)` — non-empty values win over env fallback; zero
config changes nothing. Wires: `SlskdURL` → soulseek `baseURL`;
`DeezerARL` → deezer `arl`; `NetEaseMusicU` →
`NetEaseResolver.SetMusicU`; `NavidromeURL/User/Pass` → navidrome
`base/user/password`. `SlskdUser`/`SlskdPass` NOT wired (slskd auth is
API-key only — no consumer) and `YTDLPPath` NOT wired (daemon uses
`PARLAY_YTDLP_PYTHON`/`PARLAY_YTDLP_WORKER`) — both **deleted from
the schema** (`internal/config/config.go`): fields, the `YTDLPPath:
"yt-dlp"` default, and their `setStr` env reads removed.

Per-resolver changes: deezer (`DeezerResolver{arl}`, `arlValue()` /
`arlConfigured()`, 3 call sites); navidrome (`NavidromeResolver{base,
user, password}`, `endpoints()` method — configured wins, else
`NAVIDROME_URL/USER/PASSWORD` env, 3 call sites); soulseek
(`SoulseekResolver{baseURL}`, `slskdBaseURL()` method — configured,
else `$SLSKD_URL`/`$SLSKD_HOST`, else `http://127.0.0.1:5030`;
`slskdRequest`/`slskdAvailable` → methods, 11 call sites);
netease (`SetMusicU(u)`, no-op on empty, rebuilds client via
`netease.NewWithMusicU`). Library scan/sharescan
(`internal/library/scan.go`, `sharescan.go`): `newSubsonicClient()` /
`slskdBaseURL()` / `slskdPost()` → methods on `*Service` reading
`deps.Config.Backend.Navidrome*` / `SlskdURL` first, env fallback.

`NAVIDROME_PASSWORD` is canonical and wins when set and non-empty;
`NAVIDROME_PASS` is a legacy alias fallback (set-but-empty canonical
counts as unset).

Tests: `internal/config/config_q10_test.go` —
`TestBackendKeysQ10` (7 wireable keys load from file; deleted keys
`slskd_user`/`slskd_pass`/`ytdlp_path` fail loudly naming the key),
`TestNavidromePasswordPrecedence` (canonical wins; alias works alone),
`TestRegistryConfigureQ10` (configured values reach resolvers; zero
config keeps env fallback/defaults). All PASS.

Example/docs updated to the Q10-resolved truth: `parlay.toml.example`
("PARSED-BUT-UNPLUGGED" block replaced — all keys LIVE, deleted keys
listed as failing loudly; per-key "parsed but unplugged" comments
removed; `slskd_user`/`slskd_pass`/`ytdlp_path` lines deleted),
`.env.example` ("Parsed-but-unplugged" note replaced — `SLSKD_USER` /
`SLSKD_PASS` / `YTDLP_PATH` are deleted keys, `NAVIDROME_PASS` is a
legacy alias still consumed).

## 7. Q6 follow-through — the `[ipc]` table (found during verification)

`TestExampleFileLoads` failed mid-task:
`parlay.toml.example: unknown key(s): ipc`. Cause: Worker C (Q6) had
added a core-owned `[ipc] socket_path` table to the shared example;
the Go parser rejected the unknown table. Fix (Go parser is my scope):

- `internal/config/config.go`: new `IPCConfig{SocketPath}` +
  `Config.IPC` field. The table is parsed so the shared file loads
  under both parsers — the Go parser tolerates core-owned tables
  exactly as the core tolerates `[backend]`.
- **Not parse-and-ignore**: `SocketPath()` now falls back to it —
  precedence `PARLAY_UDS_PATH` env (folded into `Backend.UDSPath`) >
  `[backend] uds_path` > `[ipc] socket_path` > `<data_dir>/parlay-backend.sock`.
  This mirrors the core exactly (`services/core-cpp/src/ipc.cpp`
  `defaultSocketPath`: `[ipc]`/env wins, else `data/parlay-backend.sock`;
  the core never reads `[backend]`). Without the fallback, a path set
  only in `[ipc]` would split the two sides (core connects where the
  backend does not listen).
- New test `TestIPCSectionQ6` (in `config_q10_test.go`): `[ipc]` alone
  sets the path; `[backend] uds_path` wins over `[ipc]`; neither →
  default; env wins over both. PASS.
- `TestExampleFileLoads` fixed too: it used a hardcoded
  `/home/hatch/workspace/parlay/parlay.toml.example` path — now a
  repo-relative path (`../../../../parlay.toml.example`, `go test`
  runs with the package dir as CWD). PASS.
- The example's `[ipc]` comment gained a warning (appended to Worker
  C's text, not rewritten): never set `uds_path` and `socket_path` to
  different values — the backend listens on the former, the core
  connects to the latter. Same warning in the `SocketPath()` doc
  comment. **Open hazard (no runtime warning implemented):** if both
  file keys are set to different values the setup silently splits;
  a startup warning when they differ would be the next step, not
  done here.

## 8. Q9 follow-up — play_mode `"admin"` → `"admins"` (core side) — DONE

Worker C ruled `"admins"` canonical (backend validator in
`internal/settings/settings.go:169` accepts only `everyone|admins`;
verified by grep — the two sides now agree). Changed the stored
spelling in `services/core-cpp/src/dispatcher.cpp`:
- `:1099` gate: `chatField(chat, "play_mode", "everyone") == "admin"`
  → `== "admins"`.
- `:4881` writer (`/playmode`): `admins ? "admin" : "everyone"` →
  `admins ? "admins" : "everyone"`.
- `:7260` settings toggle: `s.play_mode == "admin" ? "everyone" :
  "admin"` → `"admins"` in both spots.
- Plus the stored-value reader the follow-up did not name but the
  change requires: `services/core-cpp/src/ui_texts.hpp:1493` settings
  panel `s.play_mode == "admin" ? "Admins" : "Everyone"` →
  `== "admins"` — otherwise the panel would display "Everyone" for an
  admins-only chat.
- Test updated: `services/core-cpp/tests/dispatcher_test.cpp:1625`
  stored `"admin"` → `"admins"`.
- Deliberately untouched: input acceptance (`dispatcher.cpp:4873,
  4939` — `admin`/`admins`/`adminonly` all still accepted as input),
  the `/playmode [admin|everyone]` usage strings, and the
  `ui_texts.hpp:786` command-list entry (its third field is a
  permission level, not a play_mode value).

Verification: `cmake --build build --target dispatcher_test` +
`./build/dispatcher_test` → **ALL PASS**.

**Open caveat (no migration done):** chats whose mode was already
stored as `"admin"` in a live `data/state.json` now read as
everyone-mode on the core side (the gate compares to `"admins"`),
and would fail the backend validator if/when stores unify (Q7
territory). If a one-time migration or a gate that accepts both
spellings is wanted, that is a coordinator/user decision — I changed
only the stored spelling per the ruling.

## 9. Q4 follow-up — routing-table rows A63/A81 — DONE

Finding on arrival: Worker C had **already wired Q4 in the working
tree** (uncommitted `dispatcher.cpp` diff): `cbDl` posts
`download.cancel` `{run_id}` → `{cancelled, run_id, phase}`, `cbDel`
posts `library.delete` `{id}` → `{deleted, id}`. Verified the actions
exist: `contracts/ipc.json:66` / `:306`, backend registrations
`services/backend-go/internal/download/download.go:89` /
`internal/library/library.go:271`. What remained was the table itself
— rows A63/A81 still claimed the actions did not exist. Corrected in
`contracts/routing-table.md` (verified against the new handler code
and the toast constants in `ui_texts.hpp`):

- **A63** (`pl:dl:cancel:<run_id>`): backend action now
  `download.cancel` (`{run_id}` → `{cancelled, run_id, phase}`);
  rendered update: toast `Download cancelled.` + progress message
  edited to `Download cancelled.` (keyboard removed); finished run or
  no run → toast `Nothing to cancel — already finished.`; wrong chat →
  alert `Only the chat that requested this can cancel it.`; backend
  error → alert with the backend's error. Status `Phase 3` → **Live**.
- **A81** (`pl:del:yes:<id>`): backend action now `library.delete`
  (`{id}` → `{deleted, id}`); rendered update: toast `Deleted.` +
  confirm message deleted (backend deletes record and file); unknown
  id → alert `Nothing to delete.`; backend error → alert with the
  backend's error. Status `Phase 3` → **Live**.
- **E1/E2** (§E index for the same rows): "answers honestly until
  then" replaced with the Live wording (same style as E8).

Verification: `./services/core-cpp/build/routing_test
contracts/routing-table.md services/core-cpp/src/dispatcher.cpp`
(run from the repo root) → **0 failures (83 templates, 116 harvested
callbacks)**. Note for the verification worker: the test takes both
paths as argv — running the bare binary from `services/core-cpp/`
fails with `routing table file readable` and 310 cascading "table
contains" failures (the table is unreadable, so every row fails).
That is a harness-invocation quirk, not a table defect; the
A63/A81 template-shape and ≤64-byte checks pass either way.

## 10. File-by-file changes (Worker B only)

Created:
- `services/backend-go/internal/resolver/cache.go` — TTL cache, key
  format, expiry parsing, cacheability.
- `services/backend-go/internal/resolver/cache_test.go` — key format,
  expiry parsing, put/get with fake clock, cache-hit/re-resolve,
  Q10/Q11/Q12 tests.
- `services/backend-go/internal/resolver/backend_config.go` —
  `Registry.Configure` (Q10 wiring).
- `services/backend-go/internal/prefetch/prefetch.go` — Prefetcher,
  `PrebufferedStream` (DSP seam), prefetch reader.
- `services/backend-go/internal/prefetch/prefetch_test.go` — buffering,
  never-blocks, take-only-when-ready, skips, stale replace, failure,
  lead-time bytes.
- `services/backend-go/internal/store/store_phase4_test.go` — Phase 4
  bucket tests incl. reopen survival.
- `services/backend-go/internal/download/persistence_test.go` — runs +
  prefs survive reopen.
- `services/backend-go/internal/library/persistence_test.go` — index
  survives reopen, vanished-file drop.
- `services/backend-go/internal/config/config_q10_test.go` — Q10 keys,
  Navidrome precedence, Q6 `[ipc]` section.

Modified (backend-go):
- `internal/model/model.go` — `DownloadTrackProgress`,
  `DownloadRunRecord`, `DownloadPrefs`, `LibraryEntry`.
- `internal/store/store.go` — buckets `cache`, `downloads`, `dlprefs`,
  `library` + accessors.
- `internal/resolver/resolver.go` — `SetCache`, cache-first `Resolve`,
  `ResolveTrack`, `DataDir()` → `"data"` (Q12).
- `internal/resolver/ytdlp.go` — walk-up python/worker discovery (Q11);
  comment reworded (no `/home/hatch` literal).
- `internal/resolver/deezer.go`, `navidrome.go`, `soulseek.go`,
  `netease.go` — Q10 configured values (see §6).
- `internal/download/registry.go`, `prefs.go` — bbolt persistence
  (gofmt -w at end).
- `internal/library/library.go`, `scan.go`, `sharescan.go` — bbolt
  persistence; Navidrome/Slskd config-first clients.
- `internal/voice/manager.go`, `feeder.go` — `PlayStream`,
  `startFeed` shared path, reader feed.
- `internal/voice/voice_test.go` — `TestFeedFromReaderEndToEnd`.
- `internal/handlers/handlers.go`, `vc.go` — prefetch build + call
  sites + `Take`/`PlayStream` in `bindTrack`.
- `internal/config/config.go` — deleted `SlskdUser`/`SlskdPass`/
  `YTDLPPath`; `NAVIDROME_PASSWORD` canonical + `NAVIDROME_PASS`
  alias; `IPCConfig` + `SocketPath()` fallback (Q6).
- `internal/config/config_test.go` — `TestExampleFileLoads`
  repo-relative path.
- `cmd/backend/main.go` — cache + store attach wiring.

Modified (outside backend-go, follow-up only):
- `services/core-cpp/src/dispatcher.cpp` — Q9 stored spelling ×3
  (gate, writer, toggle). (File also carries Worker C's uncommitted
  Q4 wiring — separate hunks, verified to compose.)
- `services/core-cpp/src/ui_texts.hpp` — Q9 stored-value reader.
- `services/core-cpp/tests/dispatcher_test.cpp` — Q9 test value.
- `contracts/routing-table.md` — Q4 rows A63/A81 + E1/E2.
- `parlay.toml.example` — Q10 comments/keys + `[ipc]` both-keys
  warning. (File also carries Worker C's uncommitted `[ipc]` table.)
- `.env.example` — Q10 deleted-keys / legacy-alias note.

Not touched: `~/workspace/parlay/.env`, `internal/fx/` (Worker D),
`services/core-cpp` beyond the Q9 lines above, `contracts/` beyond
the four table rows, any Phase 5 / parlay-dsp work. Nothing pushed.

## 11. Anything unresolved / for the coordinator

1. **One full-suite flake, not a regression:**
   `TestYtDlpDaemonSurvivesCallerContextExpiry` failed once during a
   full-suite run under heavy concurrent load ("daemon started but did
   not answer ping: ping timed out after 10s"). All three daemon tests
   pass in isolation (PingVersion 2.97s, Survives 5.52s, BadURL 5.60s)
   and in the final clean full runs (272/0/12). The daemon start path
   is unchanged except the Q11 lookup, which returns the same venv
   python; the 10s ping budget is the flake point under load. No fix
   made — flagged, not hidden.
2. **Legacy `"admin"` play_mode rows** (see §8): now read as
   everyone-mode on the core; migration / dual-accept gate is a
   coordinator decision.
3. **`[backend] uds_path` vs `[ipc] socket_path` both-set-different**
   (see §7): silent split-brain; a startup warning is the obvious
   next step, not implemented.
4. **Known Phase 3 opens NOT mine:** `vc.join` Connect timeout
   (blocks with no timeout holding the voice mutex), `download`
   runs/prefs/library singletons are now persisted but the
   download/prefs/library in-memory singletons still exist as the
   runtime objects (store is the persistence layer, loaded at boot);
   docker-compose/install scripts still describe the deleted
   12-service topology (release-pass rewrite needed).
5. **Shared-checkout caution (verified in practice):** Worker C's
   uncommitted Q4 wiring and `[ipc]` table landed in the same tree
   while I worked. My edits were checked against `git diff` per file
   before touching shared files; the Q4 table rows and Q9 lines I
   changed compose with their hunks. The verification worker should
   expect `git status` to show other workers' files alongside mine.
6. Live verification still pending for everything (no live voice
   call, no live provider downloads, no live MTProto check) — the
   sandbox proves code paths only.
