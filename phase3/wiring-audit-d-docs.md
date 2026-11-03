# Parlay pre-Phase 4 wiring audit — Worker D (docs wiring)

Date: 2026-11-03. Scope: docs only. No Phase 4 work, no push, no
`.env` touched. No launchers/scripts, config files, or code edited.
Anchor: main `f1a7211` (Phase 3 done). Source of truth:
`FINAL-ARCHITECTURE.md` (locked 2-process engine: core-cpp + backend-go
over UDS, bbolt `data/parlay.db`, persistent yt-dlp daemon, 65 actions
in `contracts/ipc.json` v0.3.1). All replacement values were verified
against the tree (code, contracts, `parlay.toml.example`, test logs in
`phase3/wave2-e-verify.md`); nothing was guessed. Where a value could
not be verified, the passage was left alone and flagged in §2.

Note: other workers are editing the same tree concurrently
(`docker-compose.yml`, `install.sh`/`start.sh`/`stop.sh`,
`.env.example`, `parlay.toml.example`, core `config.*`,
`stub_backend.cpp` all show modifications not made by Worker D).
Docs below describe the state as verified at audit time.

## 1. Per-doc results

### README.md — REWRITTEN (was entirely old topology)

Stale: §"Install in one minute" (v0.2.0 tarball + `./install.sh` /
`./start.sh` / `./status.sh` with `/healthz`); the 12-service
architecture table (`services/vc-go` … `services/netease-go`,
ports 8001–8010/3000/8080); "All services talk through one contract:
`contracts/ipc.json` (HTTP + JSON, one endpoint per service,
`X-Parlay-Token` header)"; Status ("All 12 services build clean and
361 tests pass", "engine fail-over (go→ts→rust), Mongo/Redis store",
core `:8080` HTTP API); Quick start (`docker compose up --build`,
per-service runs for vc-go/vc-ts/vc-rust/audio-fx/share/queue/
spotify/downloaders/netease-go); port map paragraph (core 8080 …
gui 3000, slskd 5030/5031, `GET /healthz`); "`.env.example` lists
every variable the code reads (84…)".

Replaced with: repo-checkout install (backend `go build -o
parlay-backend ./cmd/backend` first, core cmake build second;
verified the backend build succeeds in this sandbox); 2-process
architecture table (core-cpp, backend-go, yt-dlp daemon,
audio-fx-rust as Phase 5 port source, slskd opt-in); UDS transport
(`data/parlay-backend.sock`, 65 actions); Phase 3 status (Go 245/0/12,
ctest 1,615 PASS 9/10, routing 944, 64/65 actions answered);
v0.2.0 tarball marked as old-topology, do-not-use; compose marked
unrewritten (no Docker route); `.env.example` flagged as still
listing legacy vars (Worker B); Spotify redirect URI
`http://127.0.0.1:8080/callback` stated.

### INSTALL.md — REWRITTEN (was entirely old topology)

Stale: intro ("C++ core + Go/Rust/TS/JS services", "release-tarball
install"); prereqs ("core + 11 services + slskd", `node` 18+,
`yt-dlp` system install); one-click + manual tarball paths
(`services/downloaders/.venv`, `services/downloaders/requirements.txt`,
slskd fetch, `LD_LIBRARY_PATH`/`TDJSON_LIB`/`NTGCALLS_LIB`); Ports
table (all 12 services + mongo/redis 27017/6379); "State, logs, and
Mongo" (JSON-file stores, "MongoDB and Redis are Compose-only");
systemd ("there are twelve" services, `parlay-v0.2.0-…` path);
Updating (tarball download/extract/re-run install.sh); Troubleshooting
("Port already in use" via `PORT`/`*_URL`; voice items 2–3
`NTGCALLS_LIB`/`TDJSON_LIB`; "`PARLAY_TOKEN` warnings at startup";
"A service exits immediately" naming `node`).

Replaced with: repo-checkout install (go 1.23+, cmake, g++, venv
`scripts/yt-dlp-venv` — creation command verified in `.gitignore`;
backend-first/core-second start); Ports/sockets table (UDS socket,
bbolt file, legacy `:8080`, slskd); bbolt state section; systemd
section kept as an explicitly untested sketch pending the launcher
rewrite; git-pull updating; troubleshooting rewritten (stale UDS
socket, `:8080` clash only, build-time libtdjson/ntgcalls with no
runtime `*_LIB` env vars, `PARLAY_TOKEN` missing = fatal, not a
warning — `missingRequired()`).

### RELEASE.md — HEADER NOTE ONLY (historical v0.2.0 report)

Body kept verbatim: it correctly records what v0.2.0 was. Added a
"Superseded note" header: v0.2.0 shipped the old 12-service HTTP
topology; the one-click install installs the old topology; no newer
release tarball exists yet; build from the repo per README/INSTALL.

### CURRENT-STATE.md — REWRITTEN IN PLACE, section by section (was entirely old topology)

- Header: HEAD `1aa39cc` → `f1a7211`, added "65 actions" + locked
  architecture pointer; dropped the v0.2.0 tarball/SHA line.
- §1: "Four languages, eleven services, one core. The bot account
  talks to Telegram over the Bot API" → two processes + daemon, bot
  over MTProto/TDLib (verified: `main.cpp` TDLib bot session,
  `pollUpdate`, no `getUpdates`).
- §2: 11-service port table → 2-process table (core-cpp, backend-go,
  yt-dlp daemon, audio-fx-rust port source, slskd); mongo/redis/
  healthcheck infra paragraph replaced.
- §3: "version 0.3.1, ~51 actions" → 65 actions; "Transport: HTTP/1.1
  + JSON … no Unix sockets" → persistent UDS socket
  `data/parlay-backend.sock`, 142 µs median; `X-Parlay-Token` per-call
  auth → socket-under-0700-dir boundary; curl-CLI flaw paragraph →
  fixed-by-UDS; action groups extended with the 9 OD-3 actions;
  `NowPlaying.engine ∈ go/ts/rust` → always `"backend-go"`
  (verified `room.go:242`).
- §4.1: `getUpdates` 30 s long-poll + backoff → TDLib
  `pollUpdate(..., 1.0)` loop, dual-session authorize, `getMe` via
  bot session.
- §4.2: "Rate limit: 1 command per second per chat, silent drop"
  → token bucket (chat 5/1.0/s, user 3/1.0/s, honest denial +
  retry-after).
- §4.3: `EngineRouter::callVc`, `engineFor`, Go→{Go,Ts,Rust}
  fallback, curl subprocess `POST http://<engine>:<port>/v1/play`,
  vc-go `handlePlay`, `attachEngineAudio()`, 5 s watcher → single
  UDS `vcCall("play")`, backend native-resolver fan-out +
  `room.Play` + bbolt persist, `finishPlay` ("the backend's play is
  the play … no core-side attach step exists any more" — verified
  in `dispatcher.cpp`), watcher DELETED in Phase 2 (verified
  `dispatcher.cpp:1910`), latencies InnerTube 0.61–0.81 s / JioSaavn
  0.32 s.
- §4.4: `makeVoiceCall` (voice_call.cpp — file deleted), dlopen
  candidate path list, Opus 128 kbit/s → backend-owned audio
  (ntgcalls via cgo, `internal/voice`), core-owned join (UDS
  `JoinRequest` → `joinVideoChat`), TDLib build-time linked,
  `standard_192k` default, sessions `data/tdlib-bot` + `data/tdlib`.
- §4.5: core `:8080` `/api/*` + gui-js `:3000` proxy → Mini App is
  Phase 6; backend already serves read-only `gui.*` UDS actions.
- §5: "Priority, everywhere: Mongo > Redis > JSON file" + A11
  canonical keys + vc-go→vc-ts→vc-rust fail-over → two stores:
  backend bbolt `data/parlay.db` (7 buckets, kill −9 proof) and the
  core's own `Store` (Mongo wire > Redis RESP > JSON — still wired
  in `main.cpp:565`, flagged as a residual, see §2 Q1).
- §6: full table replaced — dead `VC_*_URL` fields (verified
  `config.hpp` comment: "all 10 HTTP services are deleted … the UDS
  IpcClient ignores these URLs"), `MONGO_URI`/`REDIS_URL` as core-
  Store-only, `parlay.toml` tables (rate limits, timeouts, quality,
  limits), `SLSKD_URL` default `http://127.0.0.1:5030`,
  `SPOTIFY_REDIRECT_URI` exact value, `TD_SESSION_DIR` default,
  poll 1 s, no long-poll/backoff/watcher rows, `VC_PROFILE` /
  `NTGCALLS_LIB` / `TDJSON_LIB` rows dropped (verified gone).
- §7: "The bot account uses the Bot API … `getMe`, `getUpdates`" →
  both accounts on MTProto/TDLib; kept the bot-token correction
  paragraph, reworded (the Bot API cost is gone).
- §8: "Tests (Phase 7): 717/0/13 across 12 services; 12/12
  `/healthz`" + "Phase 9 is running now" + "Phase 8 (part) pushed
  as `1aa39cc`" → Phase 3 numbers (Go 245/0/12, ctest 1,615 PASS,
  routing 944, 64/65 actions), `f1a7211` push, AudD/Deezer/NetEase
  decisions, launchers+compose rewrite pending.
- §9: curl IPC / eleven processes / dlopen / Bot API limits /
  Python-slow-path weaknesses → closed-by-redesign framing +
  current leftovers (core `Store` Mongo/Redis, legacy `:8080`,
  launcher lag, no live voice proof, unmeasured live paths,
  `vc.join` context/timeout gap, in-memory download/library
  singletons, FX record-only).
- §10 (21 references): unchanged — still true.

### PORT_MAP.md — RETARGETED §2 + §3 (inventory §1 unchanged)

Stale: "`svc:<name>` = a service under `services/`" and every
"Ports into Parlay as" cell naming a deleted service (`svc:vc-go`,
`svc:vc-ts`, `svc:vc-rust`, `svc:downloaders`, `svc:netease-go`,
`svc:share-rust`, `svc:queue-rust`, `svc:spotify-rust`,
`svc:gui-js`, `svc:radio-js`, `svc:audio-fx-rust`).

Replaced with `backend-go:<pkg>` targets, each verified against
`services/backend-go/internal/`: VC streaming → `room`+`voice`;
TS engine → `resolver`+`search`+`room`; share → `share`;
YouTube/Spotify/Deezer/TikTok → `resolver`+`download` (+ yt-dlp
daemon); NetEase → `netease`; inline/streaming/Lemonic →
`search`; GUI → `gui` (Phase 6 serving, `gui.*` actions exist);
radio → `radio` (+ native ICY probe); Navidrome → `resolver`
(`navidrome.go`, Soulseek via slskd); Spotify loader → `download`;
Beetube registry → `resolver`; FX → `fx` (record-only) +
`services/audio-fx-rust` as port source; Spotify profanity →
`spotify`; low-ram/high-perf rows → **deleted** (`VC_PROFILE`
gone; no MongoDB queue cache; prefetch is
`limits.prefetch_lead_time_sec`); core/router row → UDS routing,
backend owns ntgcalls audio; queue model → `room`+`store`+bbolt.
§3: "Navidrome Soulseek — DONE in `services/downloaders`" →
`internal/resolver/soulseek.go`; "Per-user Spotify watch — DONE in
`services/spotify-rust`" → `internal/spotify/auth.go`+`crypto.go`.

### ARCHITECTURE.md — REWRITTEN (was entirely old topology)

Stale: 12-service HTTP diagram (+ MongoDB + Redis leaf); design
rules 3–4 (per-chat engine selection `vc.engine: go|ts|rust`,
`VC_PROFILE`, fail-open to next engine); "Go VC engines — one
service, two profiles"; "TS engines" (`services/vc-ts`); Rust
services table (`vc-rust`/`audio-fx-rust`/`share-rust`/`queue-rust`/
`spotify-rust`); JS services (`gui-js` proxies core `/api/*`,
`radio-js`); "Python downloaders" (`services/downloaders` worker
pool); NetEase (`services/netease-go`); "Shared IPC contract"
(one HTTP port per service, `POST /v1/<action>`, `X-Parlay-Token`,
`GET /healthz`); Data (A11 shared store, MongoDB collection
`state`, Redis `parlay:kv:<key>`, per-service JSON files,
SCRAM in 4 Rust `store.rs` copies + 2 Node `store.js` copies);
Deploy (`docker-compose.yml` wires everything, profiles).

Replaced with: locked-architecture header note; 2-process UDS
diagram (core-cpp → backend-go packages + yt-dlp daemon + slskd);
design rules (no engine selection, no fail-over); Go backend
(`room`/`voice`, no profiles); TS behaviour → `resolver`/`search`;
Rust behaviour → backend packages table; JS behaviour →
`internal/gui` (Phase 6) / `internal/radio` (ICY probe);
downloaders → `resolver`+`download`+daemon (no subprocess per
call); NetEase → `internal/netease` (env-gated, `/recognize` is
AudD `audio.recognize`); IPC contract → UDS (`ipc.json` frozen
parity reference, `ping` not `/healthz`); Data → bbolt
(`data/parlay.db`, 7 buckets, canonical room shape) + core
`Store` residual note; Deploy → hand-start until launcher rewrite,
compose unrewritten.

### phase3/resolvers.md — 2 PASSAGES FIXED

Stale: "Runs on `services/downloaders/.venv/bin/python`" and
"Python resolution: `$PARLAY_YTDLP_PYTHON`, else
`services/downloaders/.venv/bin/python` (walk-up from cwd)".
Both now read `scripts/yt-dlp-venv/bin/python` (verified:
`ytdlp.go:77`, `scripts/yt-dlp-venv/` exists,
`services/downloaders/.venv` does not; move documented in
wave2-e-verify.md §2.2).

### phase3/ other notes — ALREADY CORRECT (no edits)

- `actions-inventory.md`: "Old impl." references to vc-go/vc-ts/
  vc-rust/downloaders are deliberate — it inventories the old
  implementations Wave 2 replaced. Its "56 actions" is the
  anchor-commit (`3d84fb6`, pre-OD-3) count, not a current claim.
- `scaffold.md`: "registers the 56 actions" is a Wave 1 snapshot
  (true at the time); the inventory's §9 proposed the 9 OD-3
  actions Wave 2 added.
- `voice.md`, `wave2-a-room.md`, `wave2-b-download.md`,
  `wave2-c-services.md`, `wave2-d-core.md`, `wave2-e-verify.md`:
  no stale current-architecture claims. The `mongodb at
  127.0.0.1:27099` mentions (wave2-d-core.md:190,
  wave2-e-verify.md:174) are test-output context for the 2
  pre-existing `store_test` env failures, not architecture claims.
  wave2-e-verify.md:252 ("TCP probes to 127.0.0.1:8001–8010") is a
  verification that nothing listens there.

### COMMANDS.md — HEADER + 12 PASSAGES FIXED (skim scope)

Stale: "Contract `contracts/ipc.json` v0.3.1 (56 actions)" → 65;
"1 cmd/s per-chat rate limit" → token bucket (chat 5/1.0/s, user
3/1.0/s); "dispatcher.cpp (8,000 lines)" → 8,880 (verified `wc -l`);
"ui_texts.hpp (3,256 lines)" → 2,376; `/play` "→ vc-go/vc-ts/
vc-rust (per-chat engine)" → backend-go (single backend, UDS);
`/autoplay` "+ background watcher" → backend-owned, watcher
deleted Phase 2; `/search` "(vc-ts; …)" → backend-go
`search.query`; `/lyrics` "the service-written file" →
backend-written file (verified `netease.go:215`); `/radio` "→
radio-js → VC engine handoff" → backend radio package; `/manage`
"radio-js reports 0 always" → backend reports 0, no listener
accounting (verified `radio.go:372`); `/engine` "[go|ts|rust] …
per-chat engine switch" → retired, answers one unified backend
(verified `onEngine`, `dispatcher.cpp:4365`); `/download-settings`
"no contract fields" → `download.prefs` UDS action exists but the
core does not call it yet (verified: no `download.prefs` reference
in `dispatcher.cpp`); Inline mode "`getUpdates` asks for
`inline_query` … Modes mirror vc-ts `resolvers.js`" → TDLib poll
loop, backend `search.inline`; "`l` fetches … via `/v1/lyrics`" →
via `vc.lyrics` UDS action; `lib`/`del` "no library backend in
vc-ts" → real library backend (`internal/library`,
`library.scan`/`library.delete`); `/volume` "vc-ts IPC
(`POST /v1/volume`…)" → backend `vc.volume` UDS action (panel
`volinc`/`voldec` verified `dispatcher.cpp:6799`); "`/mix`,
`/recognize`, `/scan`, `/sharescan`, `/rmcache`: no IPC action
exists" → actions exist (`mix.generate`, `audio.recognize`,
`library.scan`, `library.sharescan`, `cache.clear`), commands
still unrouted (verified: no such command names in
`dispatcher.cpp`); "no `download.cancel` IPC action … the
downloaders POST blocks" → `download.cancel` exists, downloads
run async (`async` flag, `DownloadReady` event,
`download.progress`); "`vc.lyrics` method mismatch … vc-ts
POST-only" → closed by UDS (no HTTP methods); "Spotify companion
surfaces … owned by spotify-rust" → `internal/spotify`
(`spotify.details`/`spotify.analyze` registered, verified);
"`/speed`/`/bass` … applied for real on core-voice sessions" →
stored per room only, Phase 5 DSP applies them (verified
`room.go:937`, no `atempo` anywhere in backend-go).

Not fully verified in COMMANDS.md (left as-is, flagged in §2 Q3):
the per-command Implements column beyond the rows above still
names old paths in a few notes sections; a code-grounded pass over
the whole table was out of scope for this audit.

### REMAINING.md — HEADER NOTE ONLY (skim scope)

Body kept verbatim as the Phase 7 record. Added a "Superseded
note" header: it was the source of truth for the old 12-service
topology (717 tests, Mongo/Redis, per-service `/healthz`); that
topology is deleted. What still stands: A1 (live voice join, code
complete, live proof pending) and the key-gated table in spirit
(same keys, new package paths — see CURRENT-STATE.md §8).

### PHASE2.md / PHASE3.md / PHASE9.md / BENCHMARKS.md — NOT EDITED (skim scope, listed)

- `PHASE2.md` (2026-11-02): report of the old Phase 2 work
  (12 services, `vc-go` :8001, curl-CLI IPC, engine fail-over).
  True at the time; now a historical record of superseded work.
  Its Step 2–3 service inventory must not be read as current.
- `PHASE3.md` (2026-11-02): report of the OLD Phase 3 (vc-ts,
  radio-js, vc-rust, spotify-rust, gui-js, netease-go as separate
  services, "11/11 healthz", "35 actions"). True at the time; the
  NEW Phase 3 (unified backend, `f1a7211`) is documented in
  `phase3/` notes + CURRENT-STATE.md. Title collision is confusing
  but the date scopes it.
- `PHASE9.md`: "v0.2.0 → v0.3.1, 51 → 56 actions" — true at Phase 9
  time; the 65-action count came with OD-3 in the new Phase 3.
- `BENCHMARKS.md`: Phase 1 measurements of old code (vc-go :18001
  etc., Mongo driver build cliff). Historical measurements; the
  numbers are not current-architecture claims.

## 2. Left stale — needs a user decision (exact questions)

**Q1. Core `Store` still accepts Mongo/Redis — delete it or keep it?**
`services/core-cpp/src/config.hpp:147` and `main.cpp:565`
(`parlay::Store store(cfg)`) still wire MongoDB-wire > Redis-RESP >
JSON-file backends for the core's own keys (sudoers, chats, auth,
chat settings). `FINAL-ARCHITECTURE.md` forbids Mongo/Redis
outright, but Phase 2/3 did not remove this code, and the config
comment says Mongo/Redis are "live backends when set, not 'later
wiring'". The docs now describe this honestly as a residual
(CURRENT-STATE.md §5, §9; ARCHITECTURE.md Data). Question: should
Worker C delete the Mongo/Redis backends (and `scram.cpp` /
`store.cpp` Mongo/Redis clients) from the core store in Phase 4,
leaving JSON-file only — or is the core store intentionally out of
the no-Mongo/Redis rule?

**Q2. No new release tarball — what should README/INSTALL point at?**
The v0.2.0 tarball installs the deleted topology, so both docs now
say "do not use it, build from the repo". There is no v0.3.x
release. Question: should a new release be cut from main
(`f1a7211`) once the launchers are rewritten (Worker A), or is the
repo-checkout path the install route for now? Until that is
decided, the docs cannot offer a one-click install.

**Q3. COMMANDS.md full-table pass — who verifies the rest?**
Only the header facts and 12 plainly-wrong passages were fixed.
The rest of the 255-line Implements column was written against
the old engines; spot-checks show the remaining rows are mostly
action names (still valid) but some notes still assume old
internals. Question: should Worker C (code) do a code-grounded
pass over the whole COMMANDS.md table, or is the current
header-plus-fixes level acceptable until Phase 4?

## 3. Contradictions found but NOT fixed

1. **`contracts/ipc.json:986` — `"protocol": "HTTP/1.1 + JSON"`.**
   The live contract file still names the deleted HTTP wire
   protocol, contradicting `contracts/uds-protocol.md` ("Replaces
   the HTTP wire protocol described in `contracts/ipc.json`").
   Contract file, not a doc — Worker C's audit
   (`phase3/wiring-audit-c-code.md`) already lists it; left for
   Worker C.
2. **`contracts/uds-protocol.md:6` — "the 56 actions"** and
   **`contracts/routing-table.md:8` — "`contracts/ipc.json` v0.3.1
   (56 actions, frozen)"** (routing-table §4 has 65 rows). Both
   stale counts in contract files; Worker C's audit lists them
   (§10 parity target also says 56). Left for Worker C.
3. **`services/core-cpp/src/config.hpp` vs `FINAL-ARCHITECTURE.md`:**
   the config comment says Mongo/Redis are live backends, and the
   `VC_*_URL` defaults still name the deleted compose hostnames
   ("defaults match the deleted docker-compose topology … fields
   are dead and kept only until the 26 dispatcher call sites are
   re-pointed"). Code/config, not docs — Worker B/C scope. Docs
   describe the situation as-is.
4. **`.env.example` still lists removed variables** (`MONGO_URI`,
   `REDIS_URL`, `VC_GO_URL=http://vc-go:8001` … `NETEASE_URL`,
   ports 8001–8010/8080/3000, `/healthz`). Config file — Worker B
   scope; README/INSTALL flag it. (Worker C's audit lists it too.)
5. **`phase3/actions-inventory.md` vs live contract:** the
   inventory says 56 actions (anchor `3d84fb6`); the live contract
   has 65 (56 + 9 OD-3). Not a contradiction — a snapshot spec
   whose §9 proposed the 9 additions. Left untouched deliberately.
6. **Old phase reports vs current tree:** `PHASE2.md`/`PHASE3.md`/
   `PHASE9.md`/`BENCHMARKS.md`/`REMAINING.md` (body) describe the
   deleted topology. They are dated historical records; only
   `REMAINING.md` got a header note because it calls itself the
   "source of truth". The others were left untouched — renaming or
   rewriting historical phase reports would destroy the record.

## 4. Files changed (docs only)

- `README.md`, `INSTALL.md`, `CURRENT-STATE.md` (untracked),
  `PORT_MAP.md`, `ARCHITECTURE.md`, `COMMANDS.md`,
  `phase3/resolvers.md` — edited in place.
- `RELEASE.md`, `REMAINING.md` — header "Superseded note" only.
- `phase3/wiring-audit-d-docs.md` — this report.
- Not touched: launchers/scripts, config files (`.env.example`,
  `parlay.toml.example`), code, contracts, `docker-compose.yml`.
  Nothing pushed.
