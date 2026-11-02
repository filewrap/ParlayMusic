# Parlay — Phase 5

Date: 2026-11-03. Branch: `main` (pushed). Base: Phase 4 + TDLib (`bb901d81`).

Phase 5 implements the user's 2026-11-03 corrections plus every fixable item
in `REMAINING.md`. Scope rules applied throughout (user order 2026-11-03
01:51 IST): key-based gaps are env-gated code paths only (the key is the
only missing piece — never faked, never hunted); live Telegram testing is
NOT this phase (BOT_TOKEN/API_ID/API_HASH supplied by the user were never
used, read, or written anywhere); Mongo stays aside (A7 SCRAM, A11 shared
store — JSON-file stores stand); English only.

## 1. Voice: no server, ntgcalls in-process (Worker A)

- Cloned `pytgcalls/ntgcalls` (v3.0.5) + `pytgcalls/pytgcalls` into
  `refs/` (reference only, gitignored).
- **`services/core-cpp/voice_bridge.py` DELETED** (file + `VoiceBridge`
  class + `VOICE_BRIDGE`/`ASSISTANT_SESSION_FILE` config). No fallback
  kept — user order. Remaining `voice_bridge` mentions in the tree are
  comments/docs/test strings explaining the deletion, plus the
  `docker-compose.yml` comment noting it; no code references the file.
- **`libntgcalls.so` C build: DONE** (`BUILD_EXIT=0`, 98,121,040 B,
  77 exported `ntg_*` symbols, ldd clean). `refs/ntgcalls/shared-output/lib/libntgcalls.so`.
  Key discovery: the 3.0.5 generated C header is a **synchronous** API
  (`ntg_instance_create`, `ntg_create_call`, `ntg_result` returns) —
  the old async header was replaced and all three bindings rewritten.
- Bindings, all proven offline with real local SDP offers:
  - core-cpp: `TdLibVoiceCall` (TDLib signaling + dlopen'd ntgcalls) is
    the only join path; `NtgCallsVoiceCall` is offer/sources only.
    Real offer: 476 B, ufrag + sha-256 fingerprint.
  - vc-ts: hand-written N-API addon `src/ntg_addon.cc` (+
    `src/build-addon.sh`, dlopens the built `.so`, zero npm deps).
    Real offer: 466 B. Wheel probe `src/ntg_offer.py` is now fallback only.
  - vc-rust: std-only FFI module `src/ntg.rs` (libc `dlopen`/`dlsym`,
    zero new crates, `cargo --offline` still works). Real offer: 476 B.
  - vc-ts/vc-rust `join()` now honestly fails: "no MTProto client
    in-process (voice_bridge.py deleted Phase 5); join via the core-cpp
    tdlib backend". A Node/Rust MTProto client would be a new subsystem.
- **A10 — TDLib first-time auth: CLOSED.** Phone/code/password already
  existed env-gated; registration (`TD_FIRST_NAME`/`TD_LAST_NAME`),
  QR (`TD_QR=1`, link exposed via `otherDeviceLink()`), email
  (`TD_EMAIL`/`TD_EMAIL_CODE`) are now real env-gated paths, verified
  against the TDLib 1.8.69 TL schema. `WaitPremiumPurchase` (in-store
  purchase) fails honestly with the workaround; unknown states fail honestly.

## 2. Contract + core dispatcher (Worker B, coordinator fixes)

`contracts/ipc.json`: **0.1.0-scaffold → 0.2.0, 41 → 51 actions.**

- New: `vc.join` (`POST /v1/join`, core-cpp TDLib backend),
  `vc.mute` (`POST /v1/mute`, core voice layer), `spotify.auth`,
  `spotify.now_playing`, `download.soulseek`, `vc.panel`, `vc.volume`,
  `vc.lyrics`, `radio.status`, `radio.stop`.
- `stream_url` (optional string) on `Track` and `NowPlaying` (A8 contract side).
- `spotify.skip_check` response gains `classifier_provider`,
  `classifier_verdict`, `classifier_confidence`, `classifier_detail`;
  `reason` can be `ai-generated`. `netease.download` response gains
  `auth: music_u|anonymous`.
- **`/play` from replied media (B11):** dispatcher auto-takes `file_id`
  from `reply_to_message` (audio/document/voice/**video_note**),
  downloads via Bot API `getFile` (20 MB cap, traversal rejected) and
  plays through the core's own voice backend — never through `vc.play`
  (no engine can resolve a file_id; sending one degraded to a fake
  YouTube track). No media + no query keeps the honest usage error.
  Old hand-off refusal text removed.
- **`/join`:** real admin-gated route; `VoiceCall::join()` added,
  implemented in `TdLibVoiceCall` (authorize → offer → resolve/create
  video chat → joinVideoChat → connect, no audio). `play()` skips
  re-joining when already joined.
- **`/mute`:** flag-only code replaced — core-owned sessions call real
  `voice_.mute()`/`unmute()` (TdLib → ntgcalls); failure replies
  `Could not mute: <reason>` with the flag untouched. Engine-owned
  sessions get an honest refusal (engines expose no `vc.mute`).
  Session ownership tracked via `voice_owner` chat field; `/stop` also
  stops the core session.
- **Fail-over on 404:** a 404 for a known contract action (15 vc
  actions) now fails over like 5xx/`engine_unavailable`; 404 for an
  unknown action stays a 404 (`engine_router.cpp`, dispatcher `vcGet`).
- **A12 Navidrome:** one name everywhere — `NAVIDROME_PASSWORD`
  (code + `.env.example` + compose). Verified against
  demo.navidrome.org (public demo creds): `ping -> 0.64.2 (10114574)`;
  negative control with only the old var fails with
  `Navidrome is not configured; set NAVIDROME_PASSWORD`.
- Coordinator fixes: vc-rust `now-playing` now emits `loop` (was
  `loop_n`; load reads `loop` then `loop_n` for old state files);
  `.env.example` gained the 5 new TD_* vars and lost the dead
  `VOICE_BRIDGE`/`ASSISTANT_SESSION_FILE` lines; `COMMANDS.md` updated
  (`/mute`, `/play` replied media implemented, `/join` row added).

## 3. VC engines (Worker C)

- **vc-ts implements the 4 missing actions (A3):** `vc.loop` (0–10,
  ticker decrements per replay, skip resets), `vc.shuffle`
  (shuffle-in-place + random pop), `vc.autoplay` (YouTube mix pick on
  drain, history capped at 50), `vc.autoplay_skip` (ParvatiMusic
  mix-pick-now; 400 when autoplay off, 502 when no pick).
  `now-playing` reports `loop` (integer) + `shuffle` + `autoplay`;
  state persists; panel loop toggle sets 10/0. Live-tested: loop 2 →
  two replays then advance; autoplay picks real mix tracks.
- **vc-go StreamURL (A8):** tag `"-"` → `"stream_url,omitempty"`, and
  the resolver now runs `yt-dlp -f bestaudio/best` so the top-level
  `url` is populated — verified end to end, `now-playing.track.stream_url`
  is a real googlevideo URL. yt-dlp discovery (PATH, `$YT_DLP`,
  downloaders venv), one android-client retry on bot-check, watch-URL
  mix form. Autoplay mix picks carry no `stream_url` (flat entries have
  none; omitted, not faked).
- **Placeholders killed (A9):** vc-go `degradedTrack` deleted —
  failed resolve → **502 `engine_unavailable`** (was 400). vc-ts
  `resolveYouTube` throws instead of `duration_sec: 0` placeholders.
  vc-rust pattern fallback deleted; free text gets real `ytsearch1`
  metadata (fixed a real bug: `--dump-single-json` on a search returns
  a playlist wrapper whose id is the query string). `DEFAULT_DURATION_SEC`
  (180) removed everywhere; unknown duration is 0 and never fires
  `due_for_advance`. vc-rust Spotify URLs: real env-gated
  client-credentials path; missing creds → error naming
  `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET`.
- Divergences: vc-go `autoplay-skip` is a plain skip; vc-ts follows
  vc-rust/ParvatiMusic (mix-pick-now). vc-rust loop/shuffle/autoplay
  handlers create a room when missing; vc-ts returns 404 with no
  playback state.

## 4. Services (Worker D + Worker E scope pass)

All key-gated: real env-gated code, stubbed-endpoint tests, key is the
only missing piece.

- **Gemini classifier** (`spotify-rust/src/classifier.rs` — extended
  spotify-rust, no new service): `GEMINI_API_KEY` (+ `GEMINI_API_BASE`,
  `GEMINI_MODEL`, default `gemini-2.0-flash`), JSON-only prompt,
  temperature 0. Verdicts `clean`/`profanity`/`ai_generated`/`unknown`;
  Gemini verdict replaces wordlist when a key is set; Gemini failure →
  wordlist fallback (`wordlist-fallback`); no key + no lyrics →
  `unknown` honestly (`none`). AI-generated skips only at confidence
  ≥ 0.7 (`AI_SKIP_CONFIDENCE`). Tests use a local stub HTTP server.
- **Per-user Spotify auth** (`spotify-rust/src/auth.rs` + `crypto.rs`,
  port of rustify `auth.rs`): AES-256-GCM, 96-bit nonce per record,
  AAD `parlay:spotify-user-token:v1:<user_id>`; key = 32 random bytes
  from `/dev/urandom` generated once at first run, `spotify-user-key`
  mode 0600 — no hardcoded default. Tokens in
  `spotify-user-tokens.json` (hex nonce/ciphertext/tag, plaintext never
  on disk). Hand-rolled AES-256+GHASH (std-only); verified against NIST
  AES-256-GCM zero vector via the `cryptography` lib
  (CT `cea7403d…`, tag `d0d1c8a7…`). OAuth routes
  `POST /v1/spotify/auth` (login/callback/refresh/revoke); poller every
  `SPOTIFY_POLL_INTERVAL_SECS` (default 30, 0 disables) refreshes
  within 120 s of expiry and polls currently-playing for watched users;
  `GET /v1/spotify/now-playing?user_id=`. Live E2E proven against a stub
  Spotify on 127.0.0.1 (callback → encrypted store → poller → skip
  verdict; zero plaintext tokens in the token file).
- **Soulseek:** slskd **0.26.2** installed and running (:5030).
  `services/downloaders/src/soulseek/` ports the ref's
  client/downloader/matcher/scorer (search pacing, 429 backoff, scoring,
  selection thresholds, album/track download, file move into the dl dir).
  Route `POST /v1/download/soulseek`. **Exact blocker for a real
  search/download: a Soulseek account username + password** — the daemon
  answers every search `"The server connection must be connected and
  logged in to perform a search (currently: Disconnected)"` (verified
  via provider call and live HTTP route, both `engine_unavailable`).
  Secondary unprobed risk: direct TCP to the Soulseek server may be
  blocked by proxy-only egress.
- **Deezer ARL** (`downloaders/src/deezer.py`): `DEEZER_ARL` (192-char
  `arl` cookie, name only) + `DEEZER_FORMAT` (MP3_320 default, MP3_128,
  FLAC). Full-track flow re-implemented from deemix (gw-light
  `song.getData` → `media.deezer.com/v1/get_url`, legacy AES-ECB +
  Blowfish-CBC fallback); any ARL failure → preview fallback with an
  honest note. `pycryptodomex>=3.20` added (import guarded).
- **NetEase MUSIC_U** (`netease-go/internal/netease/`): `Client` reads
  `$NETEASE_MUSIC_U` once at startup; `Cookie: MUSIC_U=<value>` sent
  only to NetEase hosts (music.163.com / music.126.net + subdomains),
  never the mirror or any third party; `eapiCookie` swaps MUSIC_U in
  for anonymous MUSIC_A. Download responses carry `auth`.
- **Genius/Musixmatch lyrics** (`spotify-rust/src/main.rs`, public
  APIs — the ref's self-hosted proxy/desktop endpoints don't exist in
  Parlay): Genius search + `data-lyrics-container` page scrape,
  Musixmatch `matcher.lyrics.get` (restricted/instrumental → None, never
  classified on a fragment). Chain Genius → Musixmatch → LRCLib, wired
  into `decide_skip` step 3. Tokens via curl argv only, never logged.
  Only the keys are missing.

## 5. Compose, Dockerfiles, tests, docs, env (Worker E + coordinator)

- **A5 ports — one source of truth:** compose sets `PORT` explicitly
  for every service; code defaults match except the 4 documented
  overrides (audio-fx 8003→8004, share 8004→8005, queue 8005→8006,
  downloaders 8010→8009). Core `*_URL` vars now point at truly-running
  ports. `PARLAY_DATA_DIR=/data/<service>` per stateful service
  (vc-ts and radio-js both write `state.json`); downloaders
  `PARLAY_DL_DIR=/data/downloads`.
- **A6 Dockerfiles: 10 added** (core-cpp, vc-go, vc-rust,
  audio-fx-rust, share-rust, queue-rust, spotify-rust, gui-js,
  downloaders, netease-go) + vc-ts/radio-js updated (curl +
  HEALTHCHECK). Every Dockerfile: `ENV PORT` == compose port,
  `EXPOSE` == PORT, `CMD`, `HEALTHCHECK` (except core-cpp — no HTTP).
  Healthchecks on every HTTP service; core `depends_on` mongo/redis/
  vc-go/downloaders `service_healthy`; slskd service added (profile
  `full`, `slskd/slskd:latest`, 5030/5031, `SLSKD_URL=http://slskd:5030`).
  New env by name only: `TDJSON_LIB`, `NTGCALLS_LIB`, `TD_*`,
  `OWNER_ID`, `SUDO_IDS`, `ASSISTANT_IDS`, `SLSKD_*`.
  **Compose-up is UNVERIFIED here** — Docker is not installed in the
  sandbox. Validated statically: `phase5/worker-e-compose-check.py`
  **222/222 PASS**. `docker compose config` + `up` still need a host
  with Docker.
- **C1 tests:** audio-fx-rust **6/6**, share-rust **5/5**, queue-rust
  **8/8** (were 0 each; share/queue handlers extracted to testable fns).
- **C3 docs drift fixed:** root README ("Scaffold only" rewritten),
  ARCHITECTURE.md (no grammy/tgcalls/Next.js/ntgcalls-as-built/Soulseek/
  shared-Mongo claims), PORT_MAP.md §1/§2/§3, service READMEs,
  `COMMANDS.md` (see §2), PHASE4-TDLIB.md §5.
- **C4 `.env.example`:** rewritten — **80 vars read in code, 82 listed,
  0 missing** (coordinator re-sweep after all workers). Dead vars
  removed: `VC_ENGINE`, `NAVIDROME_PASS`, `VOICE_BRIDGE`,
  `ASSISTANT_SESSION_FILE`. `YT_DLP` unified (`YTDLP_BIN` → `YT_DLP`
  in vc-rust). `*.node` added to `.gitignore` (built N-API artifact).
- **Scope decisions (explicit):**
  - **B6 Beetube non-music providers: MUSIC-ONLY, not implemented.**
    Movie/anime/torrent providers depend on Beetube's external API
    backend + torrent clients; no quick real implementation exists, and
    a stub returning `engine_unavailable` does not count as a port.
  - **B7 `netease.recognize`: DROPPED.** Needs an external
    audio-fingerprint backend + login; no `netease.recognize` action
    exists in the contract and none was added.
  - **B9 `spotify.collection`: IMPLEMENTED** in downloaders
    (`POST /v1/spotify/collection`, contract shape, capped at 50;
    API path + yt-dlp/oEmbed fallback).
  - **B10 `/v1/search` in netease-go: IMPLEMENTED**
    (`POST /v1/search`, same NetEase search; `source` must be
    `netease`/`all`/absent, else 400).

## 6. Verification (coordinator, 2026-11-03)

Rebuilt all 12 services; re-ran every suite fresh. **361 passed /
0 failed / 11 skipped** (3 vc-go live-gated + 8 downloaders live-gated).

| Suite | Result |
|---|---|
| core-cpp `router_test` (new) | 20/20 |
| core-cpp `dispatcher_test` (new) | 47/47 |
| core-cpp `tdlib_test` | 26/26 (incl. real 466 B ntgcalls offer from `services/core-cpp`) |
| core-cpp `store_test` (live mongo+redis) | 40/40 |
| vc-go `go test ./...` (+ `go vet` clean) | 27 pass + 3 live-skipped; live run 30/30 |
| vc-ts `npm test` | 32/32 |
| vc-rust `cargo test --release --offline` | 31/31 |
| spotify-rust `cargo test --offline` | 40/40 |
| audio-fx-rust / share-rust / queue-rust | 6/6, 5/5, 8/8 |
| downloaders `unittest discover` | 50 run, OK (8 skipped) |
| netease-go `go test ./...` (+ `go vet` clean) | 23/23 |
| gui-js / radio-js `npm test` | 7/7, 7/7 |
| compose validator | 222/222 |

Combined live smoke: **11/11 healthz 200** (vc-go 8001, vc-ts 8002,
vc-rust 8003, audio-fx 8004, share 8005, queue 8006, spotify 8007,
radio 8008, downloaders 8009, netease 8010, gui 3000). core-cpp has no
HTTP server (A2) — verified by build + unit tests instead.
`voice_bridge` grep: 0 live references (only deletion-note comments/
docs/test strings). Secret scan of everything to be committed: clean
(no credential values; no `.env`, no session files; `phase5/` and
`refs/` gitignored, including the locally generated slskd API key).
One harness note: the coordinator's first `tdlib_test` link line omitted
`voice_call.cpp` (undefined vtable) — fixed by linking it; the test
itself passes 26/26.

## 7. What remains (with evidence)

- **A1 live voice join: never happened.** Blockers are environmental,
  not code: needs a host with open network + `TD_API_ID`/`TD_API_HASH`/
  `TD_SESSION_DIR` (+ phone/code/password for first auth, or a
  pre-authorized session dir) + the built `libntgcalls.so`; UDP sendto
  is EPERM even to loopback in the sandbox, so live RTP delivery cannot
  be proven here even after a join. Live-test phase (separate, after
  this one) owns it. Join-then-play re-join guard is also unproven live.
- **A2 gui now-playing/queue:** no real backend — core-cpp has no HTTP
  server; gui-js falls back to hardcoded demo state. Unchanged.
- **A5/A6 compose-up unverified** (no Docker in the sandbox) — needs
  `docker compose config` + `up` + healthz on a Docker host.
- **A7 Mongo SCRAM auth + A11 shared durable store: ASIDE** per user
  decision. JSON-file stores stand.
- **Soulseek real search/download: blocked on a Soulseek account**
  (username + password, user-supplied only). slskd 0.26.2 runs on :5030
  in the sandbox; code path is real and proven to fail honestly.
- **Key-gated paths (only the key is missing):** `GEMINI_API_KEY`
  (classifier falls back to wordlist, says so), Spotify OAuth creds
  (`SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` — auth login without
  creds → 503 naming the vars), Deezer `DEEZER_ARL` (previews without),
  NetEase `NETEASE_MUSIC_U` (mirror fallback without), Genius
  `GENIUS_ACCESS_TOKEN` / Musixmatch `MUSIXMATCH_API_KEY` (LRCLib
  only without), Lemonic key (optional; unauthenticated-first works).
  Sandbox egress also rejects Spotify's token endpoint (502 "not
  allowed through egress proxy") and `api.lemonic.xyz` is unreachable
  from the sandbox — live paths need the user's host.
- **Known limitations (not regressions):** `/join` works only on the
  TDLib backend; replied-file playback lives in the core voice session
  (engine now-playing/queue don't reflect it; `/pause`/`/skip` act on
  the engine session; `/stop` stops the core session); `/mute` on
  engine-owned sessions honestly refuses; vc-go autoplay mix picks
  carry no `stream_url` (core resolves on demand); vc-go
  `autoplay-skip` is a plain skip vs vc-ts/vc-rust mix-pick-now;
  core-cpp 8 libstdc++ `-Wmaybe-uninitialized` warnings (false
  positives inside `std::variant`/`stl_tree.h`, zero in service code);
  `WaitPremiumPurchase` needs a full client login once.
- **B6/B7 scope decisions** (music-only, recognize dropped) — see §5.

## 8. Files

- Reports: `phase5/worker-{a-voice,b-core,c-vc,d-services,e-compose}.md`
  (gitignored, `phase5/`), `phase5/ntg-build.log`,
  `phase5/worker-e-compose-check.py`.
- Contract: `contracts/ipc.json` (0.2.0, 51 actions).
- Voice: `services/core-cpp/src/{voice_call,tdlib_client}.{hpp,cpp}`,
  `services/vc-ts/src/{ntg_addon.cc,build-addon.sh,voicecall.js}`,
  `services/vc-rust/src/{ntg.rs,voice.rs,main.rs}`.
- Services: `services/spotify-rust/src/{classifier,auth,crypto}.rs`,
  `services/downloaders/src/{deezer.py,soulseek/*}`,
  `services/netease-go/internal/netease/{client,client_test,eapi,linuxapi,weapi}.go`.
- Compose/docs/env: `docker-compose.yml`, 12 Dockerfiles,
  `.env.example`, `README.md`, `ARCHITECTURE.md`, `PORT_MAP.md`,
  `COMMANDS.md`, `REMAINING.md` (follow-up section updated).
