# Worker A — core state (C++) — Phase 4 report

Date: 2026-11-03 (IST). Branch: main at `d36cdd7` + uncommitted Phase 4 work.
Scope: Q1, Q2, Q3, Q4, Q6, Q7 (core side), plus Q8 folded in from Worker C.
No pushes. `.env` untouched. No Phase 5 work. `vc.join` Connect timeout untouched (Phase 6).

Note on the tree: other Phase 4 workers were editing concurrently
(51 files modified at report time, incl. `contracts/`, `parlay.toml.example`,
backend-go resolver/config files). Diff stats below are for my files only;
where my change touches a shared file, the shared-file diff also contains
their work and I say so explicitly.

## 1. Q1 — Delete core Store Mongo/Redis backends (user-ruled, locked)

### `services/core-cpp/src/store.hpp` — rewritten (248 lines changed in diff)
- Deleted: `RedisClient`, `MongoUri`, `MongoClient`, `bson` namespace
  (~165 lines of declarations), `mongoGet/redisGet/mongoSet/redisSet/
  mongoDel/redisDel`, `redis_`/`mongo_` members, `backend_` member.
- `Store` keeps the same public API (`get/set/del`, sets, hashes,
  `keys`, `cacheGet/cacheSet`); `backend()` now returns `"file"`,
  `ok()` returns true.
- Header comment documents JSON-file-only + the boot/ephemeral key
  inventory (see §7).

### `services/core-cpp/src/store.cpp` — rewritten (1,496 lines changed in diff)
- Deleted Redis client (~334 lines), BSON helpers (~320), MongoClient
  (~534), socket helpers. Was 1,615 lines, now ~230.
- Constructor is file-only; all methods are file branches only;
  `cacheGet/cacheSet` in-memory only.

### `scram.{hpp,cpp}` — surgical split, NOT full deletion (272 + 103 lines)
- An initial full `rm` was reverted after discovering production uses
  `scram::sha256` (`ui_texts.hpp:124`, track-id digests) and
  `scram::base64Encode` (`dispatcher.cpp:8101`, TDLib `bytes` fields).
- Deleted only the Mongo-specific parts: SCRAM `Client` class,
  `Mechanism` enum, `mechanismName`, `parseMechanism`, `md5`/`md5Impl`,
  `mongoSha1Password`, `hexEncode`, `parseAttrs`, `xorBytes`.
- Kept the general primitives (`sha1`, `sha256`, `hmacSha1/256`,
  `pbkdf2Sha1/256`, `base64Encode/Decode`, `randomNonceBase64`) as the
  tested crypto-utility module; header comments reworded (no Mongo
  framing). `scram.cpp` now ~296 lines.

### `services/core-cpp/src/config.{hpp,cpp}` (31 + 27 lines)
- Removed `mongo_uri`/`redis_url` fields, `MONGO_URI`/`REDIS_URL` env
  reads (`config.cpp:333–334`), and the `mongo=`/`redis=` summary
  lines. Shared-state comment replaced with JSON-file note.

### `services/core-cpp/src/dispatcher.cpp` (part of 179 lines)
- Removed the `"Store: " + store_.backend()` line from the /stats
  `server_block` (was line 5806). `main.cpp:565`-area comment did NOT
  name the old priority — no change needed there. `main.cpp:15`
  reworded (was a stale live claim: "MongoDB wire protocol > Redis
  RESP > JSON file").

### Tests — `services/core-cpp/tests/store_test.cpp` (260 lines), `scram_test.cpp` (337 lines)
- `store_test.cpp` fully rewritten. No servers, no args. Covers:
  `backend()=="file"`, `ok()`, kv round-trip/overwrite/delete/missing,
  sets add/membership/size/remove/missing-empty, hashes
  round-trip/all/delete-field/missing, `keys(prefix)`, cache
  hit/miss/TTL expiry (1 s sleep), persistence across instances
  (kv/set/hash), cache NOT persisting across instances, corrupt
  `state.json` (starts empty, recovers on next write), empty `data_dir`
  falls back to `./data/state.json` (scratch CWD
  `/tmp/parlay-store-test-cwd`, cleaned up). 29 checks.
- `scram_test.cpp` fully rewritten. Keeps only known-answer vectors
  (sha1/sha256 incl. empty, hmac-sha1/256 fox, pbkdf2 RFC 6070 c=1/
  c=4096, base64 encode/padding/decode/rejects/round-trip binary) + a
  random-nonce uniqueness/size check. Dropped md5, `mongoSha1Password`,
  both RFC Client sections, URI parsing, all live-Mongo sections.
  16 checks.

### `services/core-cpp/CMakeLists.txt` (21 lines)
- Header comment rewritten (no Redis/Mongo live claims); stale
  "store.cpp and scram.cpp stay in the build — they die in Phase 3/6"
  note replaced; `scram_test` target trimmed to `scram_test.cpp` +
  `src/scram.cpp` only; `store_test` target dropped `src/scram.cpp`.

### Verification
- `grep -rni "mongo\|redis" services/core-cpp/src services/core-cpp/tests
  services/core-cpp/CMakeLists.txt` → **zero hits**. Historical notes
  in `store.hpp/cpp`, `config.hpp`, `CMakeLists.txt`, `store_test.cpp`,
  `scram_test.cpp` were reworded to avoid the literal words (history
  lives in `phase3/wiring-audit.md` and this report; `phase*/` docs
  untouched).

## 2. Q2 — Delete 10 dead service-URL fields + 26 dispatcher call sites

### `services/core-cpp/src/ipc.{hpp,cpp}` (46 + 19 lines)
- Added no-URL overloads — `post(action, body, timeout_sec=30)`,
  `get(path, timeout_sec=30)`, `healthz(timeout_sec=5)` — non-virtual
  forwarders to the existing virtual 4-arg forms with `""` (so
  `FakeIpc` overrides in tests still intercept). Header comment
  updated (no-URL form is the dispatcher form; base_url forms kept
  only for test doubles). The 4-arg virtuals were kept deliberately.

### `dispatcher.cpp` (part of 179 lines)
- All 26 dead-URL call sites retargeted (verified: 26 replacements;
  `grep cfg_.*_url` shows only the radio-card line, handled under Q3).
  Also retargeted the two helpers that already passed `""`: `vcCall`
  (line 1718) and `vcGet` (1727). Multi-line sites verified formatted
  correctly (2333 share, 3905 progress snapshot, 4100
  spotify-playlist, 4238/4292 radio status, 7405 fx, 7648 vote, 7965
  dislike, 7999 spotify.analyze).

### `config.{hpp,cpp}` (part of 31 + 27 lines)
- Deleted all 10 URL fields (`vc_go_url`, `vc_ts_url`, `vc_rust_url`,
  `fx_url`, `share_url`, `queue_url`, `spotify_url`, `radio_url`,
  `dl_url`, `netease_url`) + their env reads (`config.cpp:336–345`) +
  the `vc_go=/vc_ts=/vc_rust=` summary line. Summary now prints
  `ipc_socket=` instead (see §5). Legacy-env comment fixed
  ("service URLs" removed). Remaining `_url` hits in config are the
  live UI-link fields (`updates_url`, `support_url`, `source_url`) —
  correct.

### Verification
- `grep -rn "cfg_\.\(vc_go_url\|vc_ts_url\|vc_rust_url\|fx_url\|share_url\|
  queue_url\|spotify_url\|radio_url\|dl_url\|netease_url\)" services/core-cpp/src
  services/core-cpp/tests` → **zero hits**.
- Stale comments fixed: `vcCall` ("base_url argument is gone" →
  "exactly one backend, no per-service URL"), `vcGet` ("base URL is
  ignored" clause removed), line ~2332 "resolve it via share-rust" →
  "resolve it via the backend (share.resolve)". Other service-name
  mentions in comments (e.g. "mirrors vc-ts resolvers.js") are
  provenance notes, deliberately left alone.

## 3. Q4 — Wire `download.cancel` / `library.delete` into `cbDl`/`cbDel`

Contract shapes verified (read-only) from `contracts/ipc.json`:
`download.cancel` request `{run_id}`, response `{cancelled, run_id,
phase}`; `library.delete` request `{id}` or `{path}`, response
`{deleted, id}`. Backend behavior verified (read-only):
`services/backend-go/internal/download/handlers.go` `handleCancel` —
unknown run_id → 404; finished run → 200 `{cancelled:false,
phase:"done"|"failed"}`; `services/backend-go/internal/library/delete.go`
`handleDelete` — unknown id → 404; success → `{deleted:true, id}`;
backend's own comment says the core flow ends with `"Deleted."`.

### `dispatcher.cpp` — `cbDl` cancel branch
Replaced the "unavailable" answer. Now: no run →
`kToastDlNothingToCancel`; chat guard (parse chat from run value
`"<path|pending>\n<chat_id>"`, mismatch → `kToastDlWrongChat`,
unparsable value does not block); `ipc_.post("download.cancel",
{run_id}, 30)`; failure → `answerCb(cq, r.error_message, true)`;
success → delete `dlrun:` key, `cancelled==true` → toast
`kToastDlCancelled` + `editCbCard` retiring the progress message
(removes dead Cancel button), `cancelled==false` →
`kToastDlNothingToCancel`.

### `dispatcher.cpp` — `cbDel` "yes" branch
Replaced the "unavailable" answer. Now
`ipc_.post("library.delete", {id: parts[3]}, 30)`; failure → error
toast; `deleted==false` → `kToastDelNothing` alert; success → toast
`kToastDelDeleted` + `tg_.deleteMessage` of the confirm card (same
pattern as the "no" branch). Stale header comment updated.

### `services/core-cpp/src/ui_texts.hpp` (11 lines)
Added: `kToastDlCancelled = "Download cancelled."`,
`kToastDelDeleted = "Deleted."`, `kToastDelNothing = "Nothing to
delete."`.

### Tests — `services/core-cpp/tests/dispatcher_test.cpp` (103 lines)
- `FakeIpc`: added scripted `download_cancel_response` /
  `library_delete_response` fields + branches before the 404
  fallthrough (empty = fall through, like the other scripted
  responses).
- 16 new checks, all passing: cancel success (POST issued, `dlrun:`
  key gone, cancelled toast, progress message retired in place),
  cancel of a finished run (`cancelled:false` 200 → stale key
  cleared, nothing-to-cancel toast), cancel with no run (no backend
  call, nothing-to-cancel toast), cancel backend 404 (POST issued,
  alert toast with the exact backend error), delete success (POST
  issued, Deleted toast, confirm card deleted), delete backend 404
  (POST issued, alert toast, card kept).
- Test-safety note: neither `FakeIpc` answers these actions unless
  scripted, so the 404 failure path is exercised by the unscripted
  cases above. The two existing "not available yet" assertions are
  unrelated (spotify unknown verb at `dispatcher_test.cpp:3021`;
  unknown callbacks at `routing_test.cpp:730`).

## 4. Q3 — Radio-manage card host (judgment call, documented)

Decision: the "Live on:" link pointed at a dead per-chat web page
(`http://radio-js:8008/<chatId>`); no live page exists in the locked
topology, so any host string would render a dead link. Chose the
audit's "station itself" option: template line in `ui_texts.hpp`
`kRadioManageCard` changed from
`<a href="{host}/{chatId}">{host}/{chatId}</a>` to
`<code>{host}</code>` ("Live on: <station name>"), and `onManageRadio`
in `dispatcher.cpp` now passes `station_name` from the `radio.status`
response (fallback `station`, fallback `"—"`), HTML-escaped. The
now-unused `{"chatId",...}` pair was removed from the `fmt()` call.
Backend `radio.status` response shape verified in
`services/backend-go/internal/radio/radio.go` (`handleStatus` returns
`station`, `station_name`, `stream_url`, `now_playing`, `loop_type`,
`autoplay_enabled`, `queue_length`, etc.). Existing
`dispatcher_test` radio-manage test only asserts "Your radio status"
+ no "Listener" — unaffected.

## 5. Q6 — `ipc_socket_path` config key (added, not dropped)

### `services/core-cpp/src/config.{hpp,cpp}` (part of 31 + 27 lines)
- `config.hpp`: added `std::string ipc_socket_path;` (`[ipc]
  socket_path` / `PARLAY_UDS_PATH`; empty = default).
- `config.cpp`: `applyToml` now allows `[ipc]` with only
  `socket_path` (`checkKnownKeys`) and reads it; `applyEnvOverrides`
  applies `PARLAY_UDS_PATH` (env wins over file; set-but-empty =
  absent, like `PARLAY_DATA_DIR`).

### `services/core-cpp/src/ipc.{cpp,hpp}` (part of 19 + 46 lines)
- `ipc.cpp` `defaultSocketPath`: precedence `cfg.ipc_socket_path` →
  `PARLAY_UDS_PATH` env (re-checked so clients built from bare
  `Config::defaults()` still honour it) → `<data_dir>/
  parlay-backend.sock`.
- `ipc.hpp`: stale "Worker C should add it" note replaced with the
  resolved description.

### `parlay.toml.example` (my part: the `[ipc]` section, ~8 lines)
- Added `[ipc]` with commented `socket_path` (showing the default),
  placed after `[limits]`, before `[backend]`. Note the `[backend]`
  table already documents `uds_path` for the Go side; the core key is
  separate. (The file's full 52-line diff also contains other
  workers' concurrent edits.)

### Verification — scratch program (`/tmp/ipc_cfg_check.cpp`, not committed)
Compiled `config.cpp + toml.cpp + ipc.cpp + json.cpp` standalone
with g++ -std=c++20 and ran 5 checks, all passing:
- `[ipc] socket_path` from TOML parses.
- `PARLAY_UDS_PATH` env wins over the file key.
- `fromEnv()` picks up env-only `PARLAY_UDS_PATH`.
- Unknown key in `[ipc]` is rejected (`config: ipc.bogus: unknown
  key (allowed here: socket_path)`).
- `IpcClient` built from a `Config` with `ipc_socket_path` set
  returns it from `socketPath()`.

### Cross-worker seam (flagged, not mine to fix unilaterally)
- The Go side already honours `[ipc] socket_path` as a fallback:
  `services/backend-go/internal/config/config.go` `SocketPath()`
  precedence is `PARLAY_UDS_PATH` env → `[backend] uds_path` →
  `[ipc] socket_path` → `<data_dir>/parlay-backend.sock`. That file
  was edited by a concurrent worker during this session (their
  `IPCConfig` + `TestIPCSectionQ6`, which passes).
- Asymmetry: the core never reads `[backend] uds_path`. Setting
  `[backend] uds_path` and `[ipc] socket_path` to DIFFERENT values
  is a misconfiguration (backend listens on the former, core
  connects to the latter) — their comment says so and says to set at
  most one file key (prefer `[ipc]`). A `[backend]`-only user is
  also split from the core; that predates this change (the core
  previously had no file key at all). Left as documented, not
  re-engineered.
- Race note: my `[ipc]` example addition briefly broke the Go
  `TestExampleFileLoads` ("unknown key(s): ipc") because the
  concurrent Go-side edit landed mid-test-run. Re-run after their
  edit landed: config package passes. Nothing of mine was changed
  to fix it; the shared example now loads under both parsers.

## 6. Q7 — Core-side settings (partial: dead-write removal done; full rewire FLAGGED, not done)

### Done
Deleted the 4 write-only `autoplay:<chat_id>` kv writes (verified
zero reads anywhere: `grep '"autoplay:'` excluding `store_.set`
returned nothing; the flag is read from the `chat:<chat_id>` hash
field). Sites: `saveChatSettings` (1349–1351), `/autoplay` on/off
(~5031/5036), `cbPlayer` autoplay toggle + its rollback
(~6759/6769). Documented in `store.cpp` comment.

### Flagged, NOT rewired
The `chat:<chat_id>` hash panel settings vs backend bbolt
(`chat.settings`). Blockers found and verified:
(a) Q9 unruled — backend `settings.go` `setKey` rejects `play_mode`
`"admin"` (only `"everyone"`/`"admins"`), while core gate
`dispatcher.cpp:1099` checks `== "admin"`; a rewire today would
break the play-mode toggle and gate.
(b) `clean_mode` shape split — core stores int minutes in one field;
backend splits bool `clean_mode` + int `clean_delay_min`; also
`auto_delete_min` 2..7 and `language` must be `"en"` on the backend.
(c) `chatField`/`setChatField`/`chatIntField` also serve non-settings
runtime state with NO backend key (`loop`, `speed`, `bass`, `muted`,
`rtmp_url`, `rtmp_key`, `cplay`, `last_track`, `logger_on`) —
rewiring `chatField` would break those reads and ~15 test seed sites
(e.g. `dispatcher_test.cpp:1607` seeds `play_mode="admin"`, lines
864–927 seed `muted`, 1195/1360 seed `speed`/`bass`/`loop`,
1808/1816 seed `rtmp_url`).
(d) Backend-down fallback semantics unruled (gates currently work
offline from `state.json`).
A panel-only rewire was rejected: panel writing to backend while
gates read the core store would create live divergence, worse than
today's duplication.

## 7. Boot-only keys remaining in `state.json`

Documented in `store.hpp` + `store.cpp` comments. Verified against
`grep` of `store_.` call sites in `dispatcher.cpp` (every key below
has ≥1 live use; `autoplay:` is 0 — deleted).

- Boot / identity / ACL: `sudoers`, `chats`, `users`, `bl_users`,
  `bl_chats`, `auth:<user_id>`, `shareblocked`, `maintenance`,
  `maint_reason`, `logger_on`.
- Voice / card routing: `call:<chat_id>`, `card:<chat_id>`,
  `chat_title:<chat_id>`, `cleared:<chat_id>`.
- Transient search / inline state: `searchmsg:`, `searchcool:`,
  `search:`, `inlinesearch:`, `last_chat:<user_id>` (last chat an
  inline query came from — found during the final inventory sweep,
  added to both comments).
- Transient UI state: `panelmsg:`, `paneluser:`.
- Download / share flow: `dlrun:`, `dlset:`, `dlpick:`,
  `dlset_tpl_pending:`, `sharetarget:`, `sharetok:`, `sharelink:`,
  `sharefb:`, `dlsrv:`, `last_requester_name:`, `last_requester_id:`.
- Votes: `vote:<track_id>`. Misc: `autoleave`.
- In-memory TTL admin cache (not persisted).
- `chat:<chat_id>` hash — MIXED (see §6): panel settings fields
  (`admin_mode`, `play_mode`, `cmd_delete`, `clean_mode`, `thumb`,
  `autoplay`, `silent`, `autodelete`, `autodelete_min`, `lang`,
  `page_purge`, `musicdl`) belong in backend bbolt; runtime fields
  of the same hash (`loop`, `speed`, `bass`, `muted`, `rtmp_url`,
  `rtmp_key`, `cplay`, `last_track`, `logger_on`) stay core-side
  until a backend home exists.

## 8. Q8 — folded in from Worker C (follow-up)

- `services/backend-go/internal/model/model.go:220`:
  `AutoplayLang: "en"` → `""`. `model_test.go:95`: pin updated to
  `s.AutoplayLang != ""`, with a comment that the wire-visible
  default is `""` (`settings.go:99-100` overrides to `""`;
  `settings_test.go:45` pins `autoplay_lang: ""`) and the core has
  no `autoplay_lang` anywhere (its `/autoplay` uses `ap_lang`/
  `ap_mood` hash fields) — so `"en"` in `DefaultChatSettings` was
  ungrounded. Verified before changing: only `model.go:220`,
  `model_test.go:95`, and `settings.go:100` reference the default.
- (The files' full diffs — model.go 63 lines, model_test.go 5 —
  also contain other workers' concurrent edits; my Q8 touch is
  1 line + 1 line + 3 comment lines.)
- Follow-up note: `settings.go:100`'s `st.AutoplayLang = ""`
  override is now redundant (the default is `""`). Left in place
  per the follow-up's scope ("no other change"); Worker C may
  remove it.
- Verified: `go test ./internal/model/ ./internal/settings/` pass,
  `go vet` clean, `gofmt` clean.

## 9. `.env.example` REMOVED note (task item 1)

Lines ~137–142 rewritten: the old text said the core "still tries
them first when present" and "Some are still parsed by
config.cpp/config.go pending Worker C removal". Now says Phase 4
(Q1/Q2) deleted the last readers — the core no longer parses
`MONGO_URI`/`REDIS_URL` or any of the service URLs (the backend
never did) — so setting them has no effect; state lives in the
backend's bbolt store. Verified `services/backend-go/internal/config/
config.go` has NO `MONGO_URI`/`REDIS_URL`/`VC_GO_URL`/`VC_TS_URL`
reads, so the backend side of the old sentence was also false.

## 10. Verification — exact test counts

Build: `cmake --build . -j4` in `services/core-cpp/build` (needs
`libtdjson.so.1.8.69` at `phase4/tdlib-src/build/`, already present).
Exit 0. A full rebuild of the touched translation units produced
**0 warnings** (`-Wall -Wextra -Wpedantic`).

### ctest: **10/10 passed, 0 failed** (baseline was 9/10)
Total time ~32 s. The `store_test` mongo env failures are gone, as
predicted — the suite shape changed, not forced.

| Suite | PASS | FAIL | Notes |
|---|---|---|---|
| uds_test | 40 | 0 | |
| config_test | 55 | 0 | loads `parlay.toml.example` incl. new `[ipc]` |
| rate_limiter_test | 28 | 0 | |
| dispatcher_test | 293–294 | 0 | 1-line timing flake across runs (baseline had the same flake at 277–278); +16 = the new Q4 checks |
| routing_test | 944 | 0 | 83 templates, 116 harvested callbacks, 0 failures |
| tdlib_test | 46 | 0 | |
| telemetry_test | 5 | 0 | |
| ui_test | 160 | 0 | |
| scram_test | 16 | 0 | was SKIP-based before; now server-free |
| store_test | 29 | 0 | was 19 with 2 mongo env failures |
| **Total** | **1,616–1,617** | **0** | baseline 1,614 |

### Go (for the Q8 change + the shared example)
- `go test ./...`: **26/26 packages ok, 272 passed / 0 failed /
  12 skipped** (baseline 25 pkgs, 245/0/12 — the +27 passed are
  other workers' concurrent Q-test additions, e.g.
  `TestIPCSectionQ6`, `TestBackendKeysQ10`, `TestDataDirQ12`;
  `go vet` clean). The 12 skips are the known live-gated tests.
- One flake observed: `TestYtDlpDaemonSurvivesCallerContextExpiry`
  failed once under full-suite load, then passed in isolation twice
  (5.39 s, 5.88 s). Timing-sensitive daemon test in the
  concurrently-edited resolver area; unrelated to my change (model
  default + example file). Reported honestly, not chased.
- `TestExampleFileLoads` passes (see §5 race note).

### Not run
- The audit's 64/65 live dispatch probe (Worker E) needs a running
  backend + core peer; out of my brief (routing/dispatcher tests
  + ctest). `vc.join` remains the known sandbox failure (Phase 6).

## 11. Flagged / open items

1. **Q7 full rewire** — blocked on §6 (a)–(d); needs Q9 ruling
   (`play_mode` `"admin"` vs `"admins"`), the `clean_mode` shape
   split, a backend home for the `chat:` runtime fields, and a
   backend-down fallback decision. Do not rewire until those land.
2. **Q3 judgment call** — station name as the card's "host" (§4);
   any host string would be a dead link in the locked topology.
3. **scram kept as crypto module** — SCRAM client deleted, general
   primitives kept (production uses `sha256`/`base64Encode`).
4. **Legacy `IpcClient` URL-taking signatures retained** — thin
   wrappers for test doubles only; the dispatcher uses the no-URL
   forms.
5. **`[backend] uds_path` vs `[ipc] socket_path`** — see §5; set at
   most one file key (prefer `[ipc]`); the Go side documents the
   misconfiguration.
6. **`settings.go:100` redundant override** — see §8.
7. **Concurrent-tree caution** — other workers are landing in the
   same tree; this report's diff stats are scoped to my files, and
   nothing here was committed or pushed.
