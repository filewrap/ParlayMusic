# Parlay — REMAINING.md (source of truth)

Date: 2026-11-03. Rewritten by the Phase 7 coordinator after re-verifying
every Section A item against the current tree (rebuilds + full test runs
+ combined smoke + fresh stub grep). This file contains ONLY what truly
remains. Everything else from the original audit is closed and is NOT
listed here — the per-item history lives in PHASE7.md.

Method: 7 workers (verification, A1 join, A1b audio, A6 compose, A7
SCRAM, A11 shared store, residuals) with reports in `phase7/` (gitignored);
coordinator re-ran every suite itself (see Appendix). No live Telegram
join was attempted from the sandbox; the user is live-testing on their
own host right now and that test is the remaining proof for A1.

## Verification totals (coordinator re-runs, 2026-11-03)

**717 passed / 0 failed / 13 skipped** across 12 services. Combined
smoke **12/12** `/healthz` 200. Stub grep: **0 real stubs** in non-test
code (0 TODO / FIXME / HACK / NotImplemented, case-sensitive; XXX is
only the mkstemp template in `core-cpp/src/http.cpp:109`; the remaining
"stub"/"placeholder"/"dummy"/"mock" hits are removal comments, test-only
stub cores/engines, and test credentials).

| Service | Tests pass / fail / skip |
|---|---|
| core-cpp dispatcher / router / http_api / tdlib / telegram_api / store / scram | 128 / 20 / 71 / 44 / 24 / 40 / 67 — 394 / 0 / 0 |
| vc-go | 30 / 0 / 5 (3 live YouTube + 2 store-gated; the 3 live pass with `PARLAY_LIVE_TEST=1`) |
| vc-ts | 52 / 0 / 0 |
| vc-rust | 50 / 0 / 0 |
| spotify-rust | 51 / 0 / 0 |
| audio-fx-rust / share-rust / queue-rust | 6 / 17 / 20 / 0 / 0 |
| downloaders | 42 / 0 / 8 (live-gated) |
| netease-go | 23 / 0 / 0 |
| gui-js / radio-js | 11 / 21 / 0 / 0 |

Toolchains are not on default PATH: Go `~/workspace/parlay/.tools/go/bin/go`,
cargo `~/.cargo/bin/cargo`. Mongo 8.0.4 is `.tools/mongodb/bin/mongod`
(the 7.0.14 binary in `.tools/mongo/bin/` cannot open 8.0.4 dbpaths —
exit 62); `tdlib_test` prints 44 PASS only when run from
`services/core-cpp` (CWD-relative `.so` resolution), 28 from elsewhere.

---

## (A) What remains — Section A

### A1. Live voice join — code complete, live proof pending (user testing now)
- What is DONE in code: the join is core-owned — `POST /v1/join` →
  `Dispatcher::joinVoice` → TDLib backend (`authorize → getChat /
  createVideoChat → joinVideoChat → ntg.connect`); vc-ts/vc-rust
  delegate their joins to the core over IPC and never claim a join
  they cannot do; `TD_SESSION_DIR` defaults under the data dir and
  `TDJSON_LIB`/`NTGCALLS_LIB` resolve from `lib/` next to the binary.
  The core streams engine-resolved audio through its own ntgcalls
  session (`attachEngineAudio()` in `dispatcher.cpp`: `/play`,
  `/playforce`, `/playplaylist`, `/radio`, autoplay picks; skip /
  pause / resume / seek / stop kept in step; a 5 s watcher follows
  engine-internal auto-advance). Attach failure stops the engine
  playback and replies the exact error — never a "Now playing" lie.
  Offline proof: tdlib 44/44, http_api 71/71, dispatcher 128/128
  (stub engine returning known `stream_url`s), vc-ts 52/52, vc-rust
  50/50. `libntgcalls.so` 3.0.5 built, `libtdjson.so` 1.8.69 built.
- What still needs PROOF (host + session-gated, not a code gap): one
  `/play` with audible audio in a real group call; track 2 of a queue
  audible too (watcher follow); `/skip` switching audio cleanly;
  `/pause` / `/seek` / `/speed` / `/bass` acting on the audible
  stream; `/play` with the backend down replying the honest error.
  Needs a host with open network + a pre-authorized assistant session
  (phone/QR first-auth or a session dir) + the built `.so`s in `lib/`.
  The sandbox cannot prove this: UDP `sendto` is EPERM even to
  loopback and there is no direct TCP to Telegram DCs (proven again
  in Phase 6).

### A6. `docker compose up` — never run (host-gated, proven impossible here)
- What is DONE: all 12 Dockerfiles exist (proxy `ARG`s added in
  Phase 7); `docker compose config` is clean; core + all services
  have healthchecks; compose is the single source of truth for ports.
  Proven instead: a native sweep with the exact compose env (dummy
  creds, Dockerfile CMDs, real mongo/redis/slskd) answers `GET
  /healthz` 200 on all 12 services (see `phase7/` sweep results).
- What still needs PROOF: one `docker compose --profile full up
  --build` + healthz sweep on a real Docker host. The sandbox cannot
  do it — proven, not assumed: every container start fails in runc
  with `setns: operation not permitted` (plain run, `--userns=host`,
  and a full rootless attempt all fail identically); the sandbox sets
  `NoNewPrivs: 1` on all processes, so no OCI runtime path can start
  a container here. Evidence in `phase7/worker-a6-compose.md`.

---

## (B) Key-gated only — code complete and env-gated, only the key is missing

No code work remains on these; each fails honestly naming the exact
env var when the key is absent, and works when it is set. Tests use
stubbed providers (no real keys exist here).

| What | Missing | Where it is read |
|---|---|---|
| Deezer full tracks (previews only without) | `DEEZER_ARL` (+ optional `DEEZER_FORMAT`) | `services/downloaders/src/deezer.py` |
| NetEase direct URLs (mirror fallback works) | `NETEASE_MUSIC_U` | `services/netease-go` |
| Genius / Musixmatch lyrics (LRCLib works) | `GENIUS_ACCESS_TOKEN` / `MUSIXMATCH_API_KEY` | `services/spotify-rust` |
| AI classifier (wordlist fallback works) | `GEMINI_API_KEY` | `services/spotify-rust/src/classifier.rs` |
| Per-user Spotify watch / OAuth | `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` (+ `SPOTIFY_REDIRECT_URI`) | `services/spotify-rust/src/auth.rs` |
| Lemonic live API (unauthenticated-first works) | `LEMONIC_API_KEY` (optional) | `services/vc-ts` |
| Soulseek live search/download (slskd daemon runs) | Soulseek username + password | `services/downloaders/src/soulseek/*` (via `SLSKD_*`) |
| Bot owner / sudo model | `OWNER_ID` / `SUDO_IDS` (still unset) | `services/core-cpp` |
| Shared IPC token | `PARLAY_TOKEN` (still unset; each service defaults independently) | all services |

---

## (C) Known limitations — not stubs, not blockers

- Mongo client: `mongodb+srv://` URIs and replica sets are rejected
  (single-host client only); no SASLprep (exact for ASCII passwords);
  no channel binding. SCRAM-SHA-1/256 itself is fully implemented in
  core-cpp, all 4 Rust `store.rs` copies, and both Node `store.js`
  copies, proven against live auth mongods.
- Thin live coverage (by design, live-gated): downloaders 8 tests
  skipped without `PARLAY_LIVE_TEST=1`; netease-go has no automated
  live download test (manual Phase 4 run only); spotify-rust has 2
  creds-gated no-op live tests.
- Build cost (not correctness): core-cpp 8 libstdc++
  `-Wmaybe-uninitialized` false positives on every clean build
  (~43 s); vc-rust 1 dead-code warning (`BsonValue::Int`); vc-go
  clean build ~92 s (Mongo driver compile; warm ~8 s); TDLib is a
  ~56 min one-off build (prebuilt `.so` ships in the release
  tarball instead).

---

## Appendix — per-service build/test snapshot (coordinator re-run 2026-11-03)

| Service | Build | Tests pass / fail / skip |
|---|---|---|
| core-cpp | cmake exit 0 | 394 / 0 / 0 (dispatcher 128, router 20, http_api 71, tdlib 44, telegram_api 24, store 40 vs live mongod 8.0.4 + redis, scram 67 vs live auth + plain mongods) |
| vc-go | go vet+build exit 0 | 30 / 0 / 5 (3 live YouTube tests pass with `PARLAY_LIVE_TEST=1`; 2 store tests need `MONGO_URI`/`REDIS_URL`) |
| vc-ts | n/a (stdlib Node) | 52 / 0 / 0 |
| vc-rust | cargo test --release --offline | 50 / 0 / 0 |
| spotify-rust | cargo test --release --offline | 51 / 0 / 0 |
| audio-fx-rust | cargo build --release --offline | 6 / 0 / 0 |
| share-rust | cargo build --release --offline | 17 / 0 / 0 |
| queue-rust | cargo build --release --offline | 20 / 0 / 0 |
| downloaders | n/a (.venv Python) | 42 / 0 / 8 (live-gated) |
| netease-go | go vet+build exit 0 | 23 / 0 / 0 |
| gui-js | n/a | 11 / 0 / 0 |
| radio-js | n/a | 21 / 0 / 0 |
| **Total** | **0 build failures** | **717 / 0 / 13** |
