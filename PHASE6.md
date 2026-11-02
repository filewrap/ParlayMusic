# Parlay — Phase 6

Date: 2026-11-03. Branch: `main` (HEAD `9457865a` = Phase 5, pushed).
Base: Phase 5 + uncommitted Workers A+B work, finished by this phase.

Phase 6 closes the last non-key gaps in `REMAINING.md`. Scope rules
(user order 2026-11-03 01:50 IST): key-based gaps stay env-gated
only (the key is the only missing piece — never faked); Mongo stays
aside (A7 SCRAM, A11 shared store); English only; no stubs, no stale
docs. Live Telegram testing was done in this phase with the
user-supplied credentials (see §5); the token is in `.env` only
(mode 600, gitignored) and is left for the user to revoke.

## 1. A2 — core HTTP API (Worker A, verified by coordinator)

- core-cpp now serves its GUI HTTP API (`src/http_server.*` std-only
  sockets on its own thread, `src/core_api.*`): `GET /healthz`,
  `GET /api/now-playing` and `GET /api/queue` (real engine state via
  `Dispatcher::apiNowPlaying` / `apiQueueList`, same vcGet fail-over
  as the Telegram commands; no state →
  `404 {"ok":false,"error":"no playback state"}`).
- The core starts its HTTP server even with no `BOT_TOKEN`
  (HTTP-only mode, no polling).
- gui-js never serves demo data when `CORE_URL` is set (dead core →
  `{"ok":false,"error":"core unreachable"}`; core errors pass
  through verbatim); demo state is dev-only (`CORE_URL` unset,
  labelled `local-demo`).
- No `ipc.json` change (core HTTP endpoints, not IPC actions —
  noted in `contracts/README.md`). Core Dockerfile + compose gained
  a `/healthz` healthcheck.
- Tests: core-cpp `http_api_test` 37/37, gui-js 11/11 (was 7).
- **Coordinator verification (live, this phase):** vc-ts on :18002
  played Kesariya (YouTube, 172 s) + queued Rick Astley (213 s);
  core on :18080 proxied real state —
  `/api/now-playing?chat_id=12345` returned the Kesariya track with
  a live ticker (`position_sec` 14) and the Rick Astley queue entry,
  `/api/queue` returned the real queue, an unknown chat honestly
  returned `{"error":"no playback state","ok":false}`; gui-js on
  :18030 proxied the same real track through the core
  (`position_sec` 22, no `local-demo` anywhere in the response).
  A2 is CLOSED for real, not just by unit test.

## 2. Stub re-audit + docs drift + /autoplay-skip (Worker B)

- **`/autoplay-skip` / `/askip` command did not exist**
  (COMMANDS.md listed it, the route table did not). FIXED: route
  `{"autoplay-skip", "askip"}` + `Dispatcher::onAutoplaySkip`
  (mirrors `onSkip`: `canManageVc` + `callActive` gates,
  `vcCall "autoplay-skip"`, honest `replyError` on the engine's 400
  "autoplay is off" / 502 "no autoplay recommendation") + channel
  alias `caskip` + help line. `dispatcher_test` 53/53 at that point
  (now 81/81, see §3). The `/volume` row next to it was the
  opposite error — listed but never routed anywhere: removed from
  the table, recorded under "Not yet" as vc-ts-IPC-only.
- **`.env.example`:** 84 vars read in code, 84 listed, 0 missing,
  0 dead (Phase 5's 80/82 claim was wrong — `MAX_BIT_DEPTH` /
  `MAX_SAMPLE_RATE_HZ` from the Soulseek scorer were missing).
- **Docs drift:** root README, core-cpp README (mute flag-only +
  replied-media claims removed), COMMANDS.md (channel count 34 →
  35), PORT_MAP.md §2/§3, contracts/README.md (404 fail-over rule),
  ARCHITECTURE.md (GUI reads now-playing from the core HTTP API),
  vc-ts/vc-rust/spotify-rust/netease-go READMEs, stale "Still
  stubbed" sections in Phase 3 PROGRESS logs (SUPERSEDED notes).
- **Grep (Worker B):** 593 lines; every added hit classified
  (Worker A's stub-engine tests, demo gating, SUPERSEDED notes).
  Key-gated paths verified to fail honestly naming the var
  (Deezer/NetEase/Genius/Musixmatch/Gemini/Spotify/Lemonic/Soulseek/
  TDLib/yt-dlp).

## 3. Honest limits — B11 (Worker C, two runs reconciled)

A first Worker C (C1) implemented the broadcast `errors.txt` +
`sendDocument` and the speedtest upload probe with tests, and ran
every suite (442 passed). A second Worker C (C2, same brief)
verified C1's work, closed gaps, and did the rest. Nothing was
re-done blindly; the code below is the merged result.

- **`/broadcast` errors file — DONE.** `TelegramApi::sendDocument`
  (multipart via `HttpClient::postMultipart`), `onBroadcast` sends
  `errors.txt` on failures with an inline fallback if the upload
  itself fails. Gap closed by C2: the file now starts with a header
  line stating no per-chat error text exists (Bot API
  forward/copy return bool only), then one failed id per line.
  Proven offline by `telegram_api_test` (§A–C) + dispatcher §F.
- **`/speedtest` upload — DONE.** `onSpeedtest` POSTs a generated
  payload to Cloudflare `__up` via `HttpClient::postRaw` and
  reports up+down; failure is reported honestly. C2 tightened the
  bound 8 MB/60 s → **4 MB/30 s** and extracted pure
  `speedMbps`/`formatSpeedtest` helpers (5 new tests).
  Live caveat: the sandbox egress stalls POST bodies (100 KB POSTs
  to `__up` and httpbin.org both time out; GETs work) — no live
  upload number is provable from this sandbox; it needs a host
  with open egress.
- **`/speed` + `/bass` — DONE for core-voice, documented limit for
  engines.** Per-engine verdict (evidence in `phase6/worker-c.md`):
  vc-go has no audio path at all (0 ffmpeg hits; seek is
  state-only); vc-ts `opusPipeline` and vc-rust `opus_pipeline`
  run fixed ffmpeg args with no `-af`; their `/v1/seek` only moves
  `position_sec`. Applying filters there needs a new audio-filter
  subsystem, not a core change — no IPC action was added (there is
  nowhere to send the value to). The core backend CAN apply it, and
  now does: `buildFfmpegCmd` (atempo + lowpass bass mix),
  `VoiceCall::play(…, seek, speed, bass)`,
  `Dispatcher::applyCoreFilter` re-attaches the recorded
  `core_source` at `voice_.time()` with the new values;
  `playRepliedFile` passes stored speed/bass and records the
  source. Engine-path replies now say "saved, but the engine
  cannot apply audio filters" instead of falsely claiming success.
  13 new dispatcher checks (§G) + 5 speedtest-helper checks (§H).
  COMMANDS.md + core-cpp README state the split exactly.
- **Stub re-grep (C2) — 540 lines, 0 still-stubbed.** 203 non-test
  code / 188 test / 149 doc hits, classified line-by-line: real
  `engine_unavailable` error code, honesty/deletion comments,
  `#[cfg(test)]` doubles, gui-js labelled dev-only demo state,
  substring false positives (`demote_*`, mkstemp `XXXXXX`). Zero
  non-key-gated stubs found. Key-gated items stay env-gated and
  honest.

## 4. Verification (coordinator, 2026-11-03)

Rebuilt core-cpp (`cmake --build`, exit 0) and re-ran every core
suite from freshly built binaries; re-ran gui-js. Worker B's
11-service re-run + smoke stand as reported (no core-cpp touched).

| Suite | Result |
|---|---|
| core-cpp `dispatcher_test` | 81/81 |
| core-cpp `http_api_test` | 37/37 |
| core-cpp `telegram_api_test` | 24/24 |
| core-cpp `router_test` | 20/20 |
| core-cpp `tdlib_test` | 24 PASS lines on stdout, 0 failed (a 25th PASS line goes to stderr; source has 26 `CHECK` invocations incl. the macro def) |
| core-cpp `store_test` (live mongod :27017 + redis :6399) | 40/40 |
| **core-cpp total** | **226, 0 failed** |
| vc-go `go test ./...` + `go vet` clean | 27 pass + 3 live-skipped |
| vc-ts `npm test` | 32/32 |
| vc-rust `cargo test --release --offline` | 31/31 |
| spotify-rust `cargo test --offline` | 40/40 |
| audio-fx-rust / share-rust / queue-rust | 6/6, 5/5, 8/8 |
| downloaders `unittest discover` | 42 pass + 8 live-skipped |
| netease-go `go test ./...` + `go vet` clean | 23/23 |
| gui-js `npm test` (coordinator re-run) | 11/11 |
| radio-js `npm test` | 7/7 |
| **Grand total** | **458 passed / 0 failed / 11 skipped** |

Combined smoke: **12/12** — Worker B's 11/11 `/healthz` 200 on
compose ports (vc-go 8001, vc-ts 8002, vc-rust 8003, audio-fx
8004, share 8005, queue 8006, spotify 8007, radio 8008,
downloaders 8009, netease 8010, gui 3000) + the core's
`/healthz` 200 on :18080 verified by the coordinator in the A2
spot-check and the live run. All smoke processes stopped cleanly;
all ports free afterwards (verified).

## 5. Live Telegram test (coordinator, 2026-11-03)

Credentials from `~/workspace/parlay/.env` only (mode 600,
gitignored; never printed, never committed, never written to any
`.md`/log). No `OWNER_ID` was supplied; no assistant session /
phone was supplied.

- **Bot is live.** `getMe` via the egress proxy:
  `{"ok":true,"result":{"id":8526093166,"is_bot":true,
  "first_name":"punlo","username":"punlobot",…}}`.
  `getUpdates` returned `{"ok":true,"result":[]}` — no pending
  updates, no chat exists yet.
- **Core polls with the real token.** Started with `BOT_TOKEN` +
  `API_ID`/`API_HASH` (as `TD_API_ID`/`TD_API_HASH`) + vc-go:8001
  + vc-ts:8002. Startup log: `parlay-core: logged in as
  @punlobot (8526093166)`; `[voice] backend=unavailable …`
  (see below); `WARNING: missing env vars: PARLAY_TOKEN`
  (engine calls go out unauthenticated — D13, needs the shared
  token set on a real deployment). The poll loop runs: the first
  30 s long-poll timed out once at the 40 s curl bound (proxy
  flakiness, 0 bytes), then recovered — proof the core holds a
  live long-poll is that a competing direct `getUpdates` call
  from this shell was answered `409 Conflict: terminated by other
  getUpdates request`. The core ran 1 min 42 s+ with no fatal
  error and was then stopped cleanly.
- **`/play` end-to-end via Telegram: NOT possible from here.**
  No user account, no group, no chat id exists — nobody has ever
  messaged the bot (`getUpdates` empty) and a bot cannot message
  first. The closest live proof is §1 (real YouTube resolve +
  real engine state through the core HTTP API) plus the
  dispatcher tests (replied-media file_id, engine fail-over).
  A real `/play` in a group needs the user (or anyone) to send
  it to @punlobot in a group.
- **VC join attempt — exact failure point, with evidence.**
  The core reports at startup:
  `[voice] backend=unavailable (tdlib configured but tdlib
  unavailable: TD_API_ID/TD_API_HASH/TD_SESSION_DIR not all set;
  ntgcalls: libntgcalls.so not found (dlopen: libntgcalls.so:
  cannot open shared object file: No such file or directory))`.
  With the real `API_ID`/`API_HASH` + a session dir,
  `tdlib_test` proves `authorize()` reaches
  `authorizationStateWaitPhoneNumber` and stops honestly:
  `not authorized: no credentials (TD_PHONE not set and TD_QR
  not set; authorizationStateWaitPhoneNumber needs a phone
  number, or TD_QR=1 for QR login)`. So the join stops at
  **first-auth: no phone/QR and no pre-authorized session dir**.
  Behind that, the sandbox blocks the rest anyway: UDP `sendto`
  → `PermissionError: [Errno 1] Operation not permitted`
  (re-verified in Python); direct TLS to a Telegram DC
  (149.154.167.50:443) fails instantly (HTTP 000 in 0.003 s —
  the egress middlebox kills non-proxy TLS), while Bot API
  through the proxy works (302 on api.telegram.org). TDLib/MTProto
  cannot reach the DCs from this sandbox. No `joined:true` was
  faked anywhere.
- **Fail-over drill: not possible from here.** It needs a live
  group call with audio flowing; the live path stops at the
  auth/network points above. The fail-over logic itself is proven
  by `router_test` (20/20) + the A2 spot-check (core failed over
  across the vc-go/vc-ts URLs it was given).

## 6. What remains (key-gated + host-gated only)

- **A1 live voice join: never happened.** Needs, in order: a
  phone/QR or pre-authorized `TD_SESSION_DIR` for first auth, a
  host with open network (direct TCP to Telegram DCs + working
  UDP/RTP), `NTGCALLS_LIB` pointed at the built
  `libntgcalls.so` (`refs/ntgcalls/shared-output/lib/`), and a
  group to join. Then one `/play` with a real `joined:true` and
  audible audio.
- **A5/A6 compose-up: still never run** (no Docker in the
  sandbox). 12 Dockerfiles exist; static validation 222/222
  (Phase 5). Needs `docker compose config` + `up` + healthz on a
  Docker host.
- **A7 Mongo SCRAM auth + A11 shared durable store: ASIDE** per
  user decision. JSON-file stores stand.
- **Engine audio-filter subsystem:** `/speed`/`/bass` cannot be
  applied by vc-go/vc-ts/vc-rust (see §3) — needs a filter
  parameter through each opus pipeline + re-encode on seek/play.
  Core-voice sessions are done.
- **Key-gated paths (only the key is missing):** `GEMINI_API_KEY`
  (classifier falls back to wordlist, says so), Spotify OAuth
  creds (`SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` — auth login
  without creds → 503 naming the vars), Deezer `DEEZER_ARL`
  (previews without), NetEase `NETEASE_MUSIC_U` (mirror fallback
  without), Genius `GENIUS_ACCESS_TOKEN` / Musixmatch
  `MUSIXMATCH_API_KEY` (LRCLib only without), Lemonic key
  (optional; unauthenticated-first works), Soulseek account
  (username + password for slskd login; daemon 0.26.2 runs on
  :5030 in the sandbox and fails honestly when logged out).
  Sandbox egress also rejects Spotify's token endpoint and
  `api.lemonic.xyz` is unreachable from here — live paths need
  the user's host.
- **Deployment vars still unset in `.env`:** `OWNER_ID` /
  `SUDO_IDS` (no owner configured — public commands only),
  `PARLAY_TOKEN` (shared IPC token, must match across all 12
  services; core warned about it at startup).
- **Scope decisions (unchanged):** B6 Beetube non-music providers
  music-only; B7 `netease.recognize` dropped; `/lang`
  English-only; `/speedtest` live upload number needs open
  egress; TDLib `WaitPremiumPurchase` needs a full client login
  once.
- **Known limitations (not regressions):** `/join` works only on
  the TDLib backend; replied-file playback lives in the core
  voice session (engine now-playing/queue don't reflect it;
  `/pause`/`/skip` act on the engine session; `/stop` stops the
  core session); `/mute` on engine-owned sessions honestly
  refuses; vc-go autoplay mix picks carry no `stream_url` (core
  resolves on demand); vc-go `autoplay-skip` is a plain skip vs
  vc-ts/vc-rust mix-pick-now; core-cpp 8 libstdc++
  `-Wmaybe-uninitialized` warnings on clean builds (false
  positives inside `std::variant`/`stl_tree.h`, zero in service
  code — the coordinator's incremental build showed 0 warnings).

## 7. Files

- Reports: `phase6/worker-b-rerun.md`, `phase6/worker-c.md`
  (gitignored, `phase6/`).
- A2: `services/core-cpp/src/{core_api,http_server}.{hpp,cpp}`,
  `services/core-cpp/tests/http_api_test.cpp`,
  `services/gui-js/app/main.js` + tests.
- B11: `services/core-cpp/src/dispatcher.{hpp,cpp}`
  (`sendDocument`, `onBroadcast` errors file, `onSpeedtest`
  upload probe, `speedMbps`/`formatSpeedtest`,
  `applyCoreFilter`, `chatSpeedField`, `playRepliedFile`
  filter plumbing), `src/telegram_api.{hpp,cpp}`
  (`sendDocument`, `postRaw`, `postMultipart`),
  `tests/{dispatcher,telegram_api}_test.cpp`.
- Docs/env: `README.md`, `ARCHITECTURE.md`, `PORT_MAP.md`,
  `COMMANDS.md`, `REMAINING.md` (B11 updated),
  `contracts/README.md`, `.env.example` (84/84 vars),
  `.gitignore` (`phase6/`, `phase6-mongo-data/`).
