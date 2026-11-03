# Wiring audit — Worker C (code wiring greps), pre-Phase 4

Scope: code wiring only. Tree searched excluding `refs/`, `.git`, `.tools/`
(vendored Go toolchain), `build/`/`target/` artifacts, `scripts/yt-dlp-venv/`.
HEAD `f1a7211`, Phase 3 done and pushed. No push made. `.env` untouched.
No stubs, mocks, or placeholders added.

## Verdict up front

- **Exactly-once registration: PASS.** `TestRegisterAllExactlyOnce`
  (`services/backend-go/cmd/backend/main_test.go:60`) passes; the 65
  actions in `contracts/ipc.json` are byte-identical to the test's
  `allContractActions` list, each registered exactly once, plus `ping`
  (66 registered total, 0 unexpected).
- **Routing table: 223 data rows** (95 §A callbacks A1–A95 contiguous,
  15 §B, 101 §C, 12 §D; plus 11 §E index entries and 3 §G gap notes).
  Every handler named in a row exists in the dispatcher; no §A row
  points at a missing handler. Two rows (A63, A81) are stale in the
  other direction — see Q3.
- **8 stale-wiring items fixed in place** (5 files, all test-only or
  comment/dead-config; verified by rebuild + test runs below).
- **6 real stale-wiring groups left, all needing a decision** — none is
  a small safe fix (26 dispatcher call sites, a live Mongo/Redis store,
  two handlers answering "unavailable" for actions that now exist, a
  52-field contract retarget). Listed as Q1–Q6 below.
- **Docs / launcher / config-value hits were listed, not edited**
  (Workers D / A / B own them).

## 1. Hits by class

### (a) Real stale wiring — FIXED in place (8 items, 5 files)

| # | File:line | Exact text (before) | Fix |
|---|-----------|---------------------|-----|
| A1 | `services/core-cpp/tests/stub_backend.cpp:5` | `// answers all 56 actions from contracts/ipc.json` | 56 → 65 |
| A2 | `services/core-cpp/tests/stub_backend.cpp:29` | `// It is NOT one of the 56 contract actions and the real backend` | 56 → 65 |
| A3 | `services/core-cpp/tests/stub_backend.cpp:498–564` (inserted after the `spotify.status` branch) | no branches for the 9 OD-3 actions — the stub answered them `not_found` | added 9 canned branches with the `contracts/uds-protocol.md` §4 rows 57–65 response shapes: `download.cancel`, `library.delete`, `mix.generate`, `audio.recognize`, `library.scan`, `library.sharescan`, `cache.clear`, `download.prefs`, `vc.play_file` |
| A4 | `services/core-cpp/tests/uds_test.cpp:271–272,291,317–319` | `// Full sweep: every one of the 56 contract actions answers.` / `kAll56[]` (56 entries) / `CHECK(sweep_ok == 56, ... "/56)")` | `kAll65[]` with the 9 OD-3 actions appended; assert 65/65 |
| A5 | `services/core-cpp/src/config.hpp:147–148` | `// Shared state (reserved for later Mongo/Redis wiring; the core keeps its runtime state in memory today — see README.md).` | corrected: state lives in `Store` (MongoDB wire protocol > Redis RESP > JSON file, see `store.hpp`); Mongo/Redis are live backends when set |
| A6 | `services/core-cpp/src/config.hpp:156–158` | `// NOTE: the 11 HTTP services are deleted by the locked architecture (Phase 3); these URLs are kept until Wave 2 removes the HTTP IPC client, and are NOT in parlay.toml.` | corrected: Wave 2 is done, the HTTP IPC client is gone, the URLs are dead signature-compat args ignored by `IpcClient::post/get` (`ipc.cpp:662,669,683`); kept only until the 26 dispatcher call sites are re-pointed |
| A7 | `services/core-cpp/src/config.hpp:183–184` + `services/core-cpp/src/config.cpp:353` | `// NTGCALLS_LIB (explicit libntgcalls.so path). Empty = the dlopen search in voice_call.cpp (release lib/ next to the binary first).` / `std::string ntgcalls_lib;` / `c.ntgcalls_lib = envStr("NTGCALLS_LIB");` | field + env read + false dlopen comment deleted — `voice_call.cpp` does not exist, nothing dlopens, nothing consumed the field (verified: no other references in `src/`) |
| A8 | `services/core-cpp/src/config.hpp:196–198` + `services/core-cpp/src/config.cpp:359` | `// TDJSON_LIB (explicit libtdjson.so path). Empty = the dlopen search in tdlib_client.cpp (release lib/ next to the binary, then the repo phase4 build output).` / `std::string tdlib_lib;` / `c.tdlib_lib = envStr("TDJSON_LIB");` | field + env read + false dlopen comment deleted — libtdjson is linked at build time; `tdlib_client.hpp:79–80` already said the value was IGNORED |
| A9 | `services/core-cpp/src/tdlib_client.hpp:79–81` | `// compatibility (voice_call.cpp passes cfg.tdlib_lib) but is now IGNORED` | reworded: the `explicit_lib` arg is retained for source compatibility but ignored; no env var or config key selects a library (follow-on from A8 — the comment referenced a field that no longer exists) |

Verification of the fixes:
- `g++ -std=c++20 -I src tests/stub_backend.cpp src/ipc.cpp src/json.cpp -o /tmp/stub_backend` — builds clean.
- `g++ -std=c++20 -I src tests/uds_test.cpp src/ipc.cpp src/json.cpp src/config.cpp src/toml.cpp -o /tmp/uds_test -ldl` — builds clean.
- `/tmp/uds_test` against the rebuilt stub: **`PASS round-trip: all 65 contract actions answer (got 65/65)`**,
  `ALL UDS TESTS PASSED (0 failures)`, exit 0. Mean UDS round-trip 137 µs.
- `/tmp/config_test ../../parlay.toml.example`: `ALL PASS: 0 failed`, exit 0.

### (a) Real stale wiring — NOT fixed, needs a decision (6 groups)

**S1 — Dead service-URL config (10 fields + env reads + summary print).**
`services/core-cpp/src/config.hpp:159–168` — defaults still point at the
deleted services:
`vc_go_url = "http://vc-go:8001"`, `vc_ts_url = "http://vc-ts:8002"`,
`vc_rust_url = "http://vc-rust:8003"`, `fx_url = "http://audio-fx-rust:8004"`,
`share_url = "http://share-rust:8005"`, `queue_url = "http://queue-rust:8006"`,
`spotify_url = "http://spotify-rust:8007"`, `radio_url = "http://radio-js:8008"`,
`dl_url = "http://downloaders:8009"`, `netease_url = "http://netease-go:8010"`.
Read in `services/core-cpp/src/config.cpp:336–345` (`VC_GO_URL` … `NETEASE_URL`,
`DL_URL`/`DOWNLOADERS_URL`), printed in the startup summary
(`config.cpp:583`). Nothing on the network side exists at those addresses.
→ Q1.

**S2 — 26 dispatcher call sites pass the dead URLs to the UDS client.**
`services/core-cpp/src/dispatcher.cpp:2333, 3516, 3535, 3737, 3744, 3870,
3905, 4050, 4100, 4172, 4198, 4238, 4250, 4274, 4292, 4341, 4485, 4722,
6102, 6482, 6518, 7405, 7648, 7965, 7987, 7999` — e.g.
`IpcResult s = ipc_.post(cfg_.netease_url, "netease/search", sbody, 20);`
(`:3516`). Harmless at runtime: `IpcClient::post/get/healthz`
(`services/core-cpp/src/ipc.cpp:659–683`) explicitly ignore the first
argument (`(void)base_url;  // single backend: the URL is signature compat
only`) and route everything over the one UDS socket. Removing them means
changing the `IpcClient` API + all 26 sites + the `callAction`/`callGet`
helpers (`dispatcher.cpp:1712–1727`, which already pass `""`) — a
mechanical but non-trivial refactor, not a grep fix. → Q1.
Related single site: `dispatcher.cpp:4322` renders the dead URL into a
user-visible card: `{{"host", cfg_.radio_url},` inside the radio-manage
card (`kRadioManageCard`) — the card would show `http://radio-js:8008`
as "host". → Q2.

**S3 — Two handlers answer "unavailable" for actions that now exist.**
- `services/core-cpp/src/dispatcher.cpp:7373–7380` (`cbDl`, cancel verb):
  `// There is no download.cancel IPC action in contracts/ipc.json (and
  no cancel endpoint in the downloaders service), so the core cannot
  stop a running download — say so honestly instead of claiming a cancel
  that never happens.` → answers `kToastFeatureUnavailable`.
- `services/core-cpp/src/dispatcher.cpp:7731–7732` (`cbDel`, yes verdict):
  `// There is no library.delete IPC action in contracts/ipc.json, so
  the core cannot execute the deletion (see phase9/port-core.md): "yes"
  is answered honestly instead of pretending.` → answers
  `kToastFeatureUnavailable`.
Both actions ARE registered in the backend now (`download.cancel`,
`library.delete` — 65-action contract, verified §2). Wiring the calls
needs payload/error-handling decisions, not a comment edit. → Q3.
Routing-table rows A63 (`contracts/routing-table.md:127`) and A81
(`:341`) record the same stale claim ("no `download.cancel` /
`library.delete` action exists in `contracts/ipc.json`", status Phase 3)
— Worker D text, to update once Q3 is decided.

**S4 — `contracts/ipc.json` still describes the deleted topology.**
52 of the 65 actions carry a `"service"` field naming a deleted service
(`downloaders` ×8, `vc-ts` ×7, `vc-go | vc-ts | vc-rust` ×13, `spotify-rust`
×10, `radio-js` ×5, `netease-go` ×3, `share-rust` ×2, `queue-rust` ×1,
`core-cpp -> gui-js` ×2, `vc-ts | downloaders | netease-go` ×1,
`spotify-rust | downloaders` ×1, `radio-js -> vc-*` ×1).
Line 956 `description` still says: `"Single contract for all Parlay
services. HTTP + JSON. Every service: POST /v1/<action> with header
X-Parlay-Token: $PARLAY_TOKEN. Health: GET /healthz -> {ok, service}."`
— the HTTP wire protocol it describes is deleted. `"from"` provenance
fields still name deleted files / dead mechanisms: `:126`
(`downloaders server.py`), `:436,448` (`radio-js src/main.js`),
`:679,707` (`dlopen'd ntgcalls`), `:691` (`vc-ts src/main.js lyrics
panel`), `:744` (`vc-ts src/main.js`), `:944` (`vc-ts src/main.js volume
control`). Lines 960–961 changelog entries (v0.3.0/v0.3.1, "56 actions",
"the downloaders POST blocks until the transfer completes") are
historical version notes, not live wiring. → Q4.

**S5 — Core still ships a live hand-rolled MongoDB + Redis store.**
`services/core-cpp/src/store.hpp` / `store.cpp` implement a minimal
MongoDB wire-protocol client (OP_MSG + BSON, SCRAM) and a RESP Redis
client from scratch (`RedisClient`, `MongoClient`, `Store`). Instantiated
in `services/core-cpp/src/main.cpp:565` (`parlay::Store store(cfg);`) and
used all over the dispatcher (`store_.get/set/del/sadd/sismember/hget/
hset/cacheGet/cacheSet` — sudoers, auth, chat settings, share targets,
download picks/runs, maintenance, admin cache, etc.;
`dispatcher.cpp:305–317, 574–1349` and passim). Config still reads
`MONGO_URI` / `REDIS_URL` (`config.hpp:149–150`, `config.cpp:333–334`,
summary `config.cpp:595–596`). The locked architecture says NO
Mongo/Redis and backend-go bbolt owns state; the core's store defaults
to a JSON file when neither URI is set, so it works today with no
servers — but it is a second state home with its own wire clients, and
no Mongo/Redis dependency exists in `services/backend-go/go.mod`.
Deleting it means moving every `store_` call behind UDS to backend-go —
a Phase-4-sized job, not a grep fix. → Q5.

**S6 — `ipc_socket_path` config key promised to Worker C.**
`services/core-cpp/src/ipc.hpp:44–45`: `// default is
<data_dir>/parlay-backend.sock (Config::data_dir exists today),
overridable with the PARLAY_UDS_PATH env var until Worker C adds an
ipc_socket_path config key.` The key does not exist; the socket path is
env-only (`PARLAY_UDS_PATH`, `ipc.cpp` `defaultSocketPath`). Adding it
is a config-surface decision (parlay.toml key name, env precedence),
and the example entry would belong to Worker B. → Q6.

### (b) Intentional / known — left as-is

- **B1 — audio-fx-rust HTTP + ports.** `services/audio-fx-rust/src/common.rs:418`
  (`GET /healthz`), `README.md:17`, `PROGRESS.md:32,34,48`,
  `Dockerfile:38` (`curl -fsS http://localhost:8004/healthz`), port
  8003/8004 mentions. Phase 5 port source, not wired into the 2-process
  runtime; its own health endpoint is consistent within that service.
- **B2 — slskd loopback URLs in backend-go.** `internal/config/config.go:161`,
  `internal/resolver/soulseek.go:98`, `internal/library/sharescan.go:24`:
  `http://127.0.0.1:5030`. slskd is an external daemon, not a deleted
  Parlay service.
- **B3 — Provenance comments in backend-go.** `internal/model/model.go:108,
  123, 129, 146, 159, 172, 232` and `internal/resolver/*.go` (`netease/client.go:66`,
  `eapi.go:31`, `resolver.go:148–212, 274, 341, 397, 420`, `youtube.go:405`,
  `deezer_secret.go:1`) name deleted services as porting provenance
  ("ported from vc-ts / queue-rust / spotify-rust / downloaders …").
  Historical attribution, not wiring. `resolver.go`'s internal
  `downloaders map[Source]Downloader` is a Go field name, not a service.
- **B4 — Legacy-name translation shims in the UDS client.**
  `services/core-cpp/src/ipc.cpp:239–275` (`canonicalAction`: `play`→
  `vc.play`, `netease/search`→`netease.search`, `download/youtube`→
  `download.youtube`, …) and `:279–331` (`getPathToAction`: `/v1/…`,
  `/api/now-playing`→`gui.now_playing`, `healthz`→`ping`, …). Intentional
  compat layer so old slash-form call sites keep working; comments name
  deleted services as provenance. The 9 OD-3 actions need no legacy
  aliases (they never had slash forms).
- **B5 — `getUpdates` mentions in core-cpp.** `src/dispatcher.hpp:65`
  (comment: the new sender mirrors the old surface "minus the Bot
  API-only parts (getUpdates long-poll, getFilePath, exportChatInviteLink
  — gone with the Bot API client)"), `src/main.cpp:9–10` (comment: "There
  is no Bot API polling loop in this file"), `README.md:71`,
  `PROGRESS.md:29,45`. No `telegram_api.*` files exist in `src/`; the
  Bot API client is deleted. Historical notes only.
- **B6 — "No dlopen" notes.** `CMakeLists.txt:17,35,111–112`,
  `tests/tdlib_test.cpp:412` ("the library is linked, not dlopened").
  Accurate; `TDJSON_LIB` in `CMakeLists.txt:26–53` is a *build-time*
  CMake variable selecting the libtdjson to link, not the deleted env var.
- **B7 — Core's own HTTP API on :8080.** `src/main.cpp:524`
  (`HttpServer`), `src/core_api.cpp:64` (`/healthz`), `src/config.hpp:170`
  (`port = 8080`), `src/http_server.*`, `Dockerfile:47`
  (`curl -fsS http://localhost:8080/healthz`). GUI surface (Phase 6 A2),
  not inter-service HTTP; intentional under the locked architecture
  (Mini App talks to the core's `/api/*` later). `parlay.toml.example:43,52`
  `healthz_ms` is that surface's probe budget — Worker B's file, listed.
- **B8 — Spotify OAuth loopback in tests.**
  `services/backend-go/internal/spotify/spotify_test.go:212,237,263,276,294`:
  `http://localhost:8080/callback`. OAuth redirect URI, not a service port.
- **B9 — `tests/tdlib_test.cpp:92` `unsetenv("TDJSON_LIB");`** — now a
  no-op (env var no longer read after fix A8); harmless, left.
- **B10 — `tests/stub_backend.cpp` `test.sleep_ms`** — test-only action,
  intentionally not a contract action; the stub rejects nothing else
  new (all 65 now answer).
- **B11 — `phase6-mongo-data/`** — git-ignored local Mongo data dir
  (WiredTiger files), not tracked, not wiring. Incidental leftover.
- **B12 — Vendored trees.** `.tools/go/**` (Go toolchain) and
  `scripts/yt-dlp-venv/**` (yt-dlp/pip packages) contain `dlopen`,
  `:8080`, `:8000` hits — third-party code, not repo wiring.
- **B13 — Routing-table entry points with no row.**
  `handleMessage`, `handleMyChatMember`, `handleInlineQuery` are
  dispatcher entry points, not callback handlers; the table covers
  callbacks/commands/inline rows. `cbDlCancel` / `cbSearchPick`
  (`src/ui_texts.hpp:185,195`) are callback-*data builders*, not
  handlers — no row needed. All 18 `cb*` sub-handlers named in rows exist.
- **B14 — Known pre-existing `store_test` env failures** (Mongo/Redis
  down in the sandbox) — noted in the task as known; not re-verified,
  out of scope.
- **B15 — `contracts/ipc.json:960–961` changelog** — historical v0.3.0 /
  v0.3.1 notes ("56 actions", "downloaders POST blocks…"). History, not
  live wiring (see S4 for the live fields).

### (c) Listed, not edited — other workers' files

- **Worker A (launchers):** `docker-compose.yml` — the entire file is
  the old 12-service topology (service blocks `vc-go`, `vc-ts`,
  `vc-rust`, `audio-fx-rust`, `share-rust`, `queue-rust`, `spotify-rust`,
  `radio-js`, `downloaders`, `netease-go`, `mongo`, `redis`; `MONGO_URI`/
  `REDIS_URL`, `VC_GO_URL=http://vc-go:8001` … `NETEASE_URL=
  http://netease-go:8010`, ports 8001–8010/8080/3000, `/healthz`
  healthchecks — lines 8–22, 57–70, 88–90, 96–98, 100–102, 111–130,
  136–159, 170–185, 200+). Already flagged in the handoff as needing a
  release-pass rewrite. `start.sh:98,130,206,252`
  (`NETEASE_URL=http://127.0.0.1:8010`, generic `http://127.0.0.1:$port`
  health probes), `status.sh:30` (same probe).
- **Worker B (config values):** `parlay.toml.example:43,52` (`healthz_ms`,
  legacy name, live value — see B7), `:62,69` (`source_ladder` naming
  `netease` etc. — source names, not services; no action).
- **Worker D (docs):** `README.md:26,37–47,63–64,68,73,78,81,95,111–132`
  (old service table, ports, run commands, `/healthz`);
  `ARCHITECTURE.md:15–27,66–71,79,90–94,101–111,126,145,173–199,211`
  (old topology, Mongo/Redis store description);
  `PORT_MAP.md:60–80,96,102` (`svc:` assignments to deleted services);
  `FINAL-ARCHITECTURE.md:217,221,410–413,738` (historical 51→56 counts;
  §10 parity target says "56 actions" while the live contract is 65);
  `PHASE9.md:102,262`, `PHASE5.md:48,307`, `UI_INVENTORY.md:24`,
  `CURRENT-STATE.md:53`, `COMMANDS.md:12`, `phase9/port-core.md:188,526`
  ("56 actions" / "~51 actions"); `contracts/uds-protocol.md:6`
  ("the 56 actions" — §4 itself has 65 rows);
  `contracts/routing-table.md:8` ("56 actions, frozen") and `:127,341`
  (A63/A81 stale claims, see S3); `contracts/README.md:7,28` (per-service
  `/healthz` lines); `services/core-cpp/README.md:28,71,79,172,174,
  305–306,309,323`, `services/core-cpp/PROGRESS.md:29,45–46`;
  `phase3/scaffold.md:14`, `phase3/actions-inventory.md`,
  `phase2/ipc-cmake.txt:35` (historical "56 actions" notes).
  Note: while this audit ran, sibling workers were editing
  `README.md`, `INSTALL.md`, `RELEASE.md`, `install.sh`, `start.sh`,
  `status.sh`, `stop.sh` (they appear modified in `git status` but are
  not this worker's edits — this worker touched only the 5 files in §2).

## 2. Fixes made, file by file

1. `services/core-cpp/tests/stub_backend.cpp` — header "56 actions" → 65
   (2 comment lines); added 9 OD-3 canned branches (72 lines added).
2. `services/core-cpp/tests/uds_test.cpp` — sweep list `kAll56` → `kAll65`
   (+9 actions), assertion 56 → 65.
3. `services/core-cpp/src/config.hpp` — fixed 2 stale comments (shared
   state, service URLs); deleted dead `ntgcalls_lib` and `tdlib_lib`
   fields + their stale dlopen comments.
4. `services/core-cpp/src/config.cpp` — deleted the 2 dead env reads
   (`NTGCALLS_LIB`, `TDJSON_LIB`).
5. `services/core-cpp/src/tdlib_client.hpp` — fixed the comment that
   referenced the deleted `cfg.tdlib_lib`.
   Diff: 5 files, +91/−25 (scoped to the files above; the wider
   `git diff --stat` also shows sibling workers' in-flight edits to
   docs/launcher files, which are not mine).

## 3. Exactly-once registration test — exact result

`services/backend-go/cmd/backend/main_test.go:60`
`TestRegisterAllExactlyOnce`, run with the vendored toolchain
(`.tools/go/bin/go test ./cmd/backend/ -run TestRegisterAllExactlyOnce
-v -count=1`):

```
=== RUN   TestRegisterAllExactlyOnce
--- PASS: TestRegisterAllExactlyOnce (0.01s)
PASS
ok  	parlay/backend-go/cmd/backend	0.112s
```

- `contracts/ipc.json` actions: **65** (dict keys, sorted and diffed
  against the test's `allContractActions` — identical, modulo a trailing
  newline).
- Registered by `registerAll` (the exact production wiring `main()`
  calls): **66** (65 contract + `ping`), each with
  `RegistrationCount == 1`.
- **0 actions in ipc.json but not registered. 0 registered but not in
  ipc.json.** The test itself fails on either case, and it passes.
- The 46 literal `Register("…")` call sites in `services/backend-go`
  are not the full count — registrations also flow through package
  `registerAll` helpers; the count that matters is the test's 66, and
  it matches.
- Companion C++ sweep (after fix A3/A4): stub answers **65/65**,
  `ALL UDS TESTS PASSED (0 failures)`.

## 4. Routing-table match

`contracts/routing-table.md`:
- **Row counts:** 95 §A callback rows (A1–A95, contiguous, verified by
  script), 15 §B, 101 §C, 12 §D = **223 data rows** (matches the file's
  own footer count); plus 11 §E index entries and 3 §G gap notes
  (237 `| X## |` lines total).
- **Rows with no matching handler: 0.** Every `handleCallback` →
  `cb*` token in §A resolves to a real handler in
  `services/core-cpp/src/dispatcher.cpp`; every dotted action token in
  the Backend-action column resolves to `contracts/ipc.json`, except
  two prose mentions of `ipc.json` itself (A63, A81 — the filename
  matched the dotted-token regex, not a real action reference).
- **Handlers with no row:** `handleMessage`, `handleMyChatMember`,
  `handleInlineQuery` (entry points, not callback handlers — intentional,
  B13). `cbDlCancel` / `cbSearchPick` are `ui_texts.hpp` builders, not
  handlers (B13). No real callback handler lacks a row.
- **Stale rows (content, not linkage):** A63 (`:127`) and A81 (`:341`)
  claim `download.cancel` / `library.delete` do not exist in
  `contracts/ipc.json`; both actions exist since Phase 3 Wave 2. The
  rows' "Phase 3" status was correct when written (Phase 2) and is now
  spent — see Q3. The footer note `:29–32` ("Phase 3 — … the backend
  action does not exist yet; Phase 3 implements it") is likewise spent
  for these two rows.
- **Actions with no routing row (expected, not a mismatch):** 41 of 65
  ipc.json actions have no row — the table routes *UI inputs*
  (callbacks/commands/inline), not backend actions. Actions invoked only
  from command handlers or other paths (`download.*`, `vc.join`,
  `vc.loop`, `vc.panel`, `search.query`, `spotify.*` companions, etc.)
  legitimately have no §A row.

## 5. Decisions needed (exact questions)

- **Q1 — Delete the dead service-URL wiring?**
  Remove the 10 URL fields (`config.hpp:159–168`), their env reads
  (`config.cpp:336–345`), the summary print (`config.cpp:583`), and the
  URL first-argument at the 26 `ipc_.post/get` call sites in
  `dispatcher.cpp` (plus the `IpcClient::post/get/healthz` signatures in
  `ipc.hpp`/`ipc.cpp:659–683`), re-pointing them at the no-URL helpers?
  Or keep the fields as signature-compat dead config?
- **Q2 — What should the radio-manage card show as "host"?**
  `dispatcher.cpp:4322` renders `cfg_.radio_url`
  (`http://radio-js:8008`) as the `host` field of the user-visible
  radio-manage card. Station name? `—` like the other placeholder
  fields? Remove the field?
- **Q3 — Wire `download.cancel` and `library.delete` into the core, or
  keep answering "unavailable"?**
  `cbDl` (`dispatcher.cpp:7373–7380`) and `cbDel` (`:7731–7750`) answer
  `kToastFeatureUnavailable` on the claim the actions don't exist; both
  actions are registered in backend-go. If wiring: what payloads
  (`download.cancel` takes `run_id`; `library.delete` takes `id`/`path`),
  what success/failure edits, and do rows A63/A81 flip to Live?
- **Q4 — Retarget `contracts/ipc.json` to the new topology, or keep it
  frozen?**
  Retarget the 52 `"service"` fields naming deleted services to
  `backend-go`, rewrite the line-956 `description` (still HTTP +
  `POST /v1/<action>` + `GET /healthz`), and fix the `"from"`
  provenance fields (`:126,436,448,679,691,704,707,744,944`)? Or keep
  ipc.json frozen as the Phase 9 parity reference and record the new
  topology only in `uds-protocol.md`?
- **Q5 — Is core-cpp's Mongo/Redis `Store` the intended state home, or
  must it go?**
  `services/core-cpp/src/store.{hpp,cpp}` (hand-rolled Mongo wire +
  Redis RESP clients, live in production via `main.cpp:565`) vs the
  locked architecture (no Mongo/Redis; backend-go bbolt owns state).
  Is the core's store — JSON-file default when no URI is set — the
  intended home for dispatcher-local state (sudoers/auth/chat settings/
  share targets/download runs), or must every `store_` call move behind
  UDS to backend-go? Note the 2 pre-existing `store_test` env failures
  are this store's Mongo/Redis backends being down in the sandbox.
- **Q6 — Add the `ipc_socket_path` config key, or drop the note?**
  `services/core-cpp/src/ipc.hpp:44–45` says Worker C should add it.
  If adding: parlay.toml key name and precedence vs the `PARLAY_UDS_PATH`
  env var (and the example entry is Worker B's file)? If not: delete
  the note and keep the socket path env-only?
- **Q7 — Should `FINAL-ARCHITECTURE.md` §10 now say 65 actions?**
  §10 anchors the parity target at "56 actions" (Phase 9, v0.3.1) while
  the live contract is 65 (56 + 9 OD-3). Worker D owns the doc, but the
  ruling is architectural: is §10's parity target now 65, or does 56
  stay as the frozen anchor with the 9 OD-3 actions tracked separately?
