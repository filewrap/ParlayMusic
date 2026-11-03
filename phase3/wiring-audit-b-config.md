# Wiring audit — Worker B (config wiring), pre-Phase 4

Date: 2026-11-03 ~18:30 IST. Scope: config wiring ONLY in `~/workspace/parlay/`
(main at `f1a7211`, Phase 3 done). No push made. `.env` untouched.
Files changed by this worker (5): `parlay.toml.example`, `.env.example`,
`services/backend-go/internal/config/config.go`,
`services/core-cpp/src/config.cpp`,
`services/core-cpp/src/tdlib_client.cpp`.

## 0. Shared-tree warning — read first

Sibling workers were editing the SAME checkout concurrently during this
audit. At the start the tree was clean except untracked `CURRENT-STATE.md`;
by the end, 14 further files were modified by others (Worker A launchers/
scripts: `start.sh`, `stop.sh`, `status.sh`, `install.sh`,
`docker-compose.yml`, `scripts/package-release.sh`,
`services/core-cpp/Dockerfile`, untracked
`services/backend-go/Dockerfile`; Worker D docs: `README.md`,
`INSTALL.md`, `RELEASE.md`; Worker C code: `services/core-cpp/src/config.hpp`,
`services/core-cpp/src/tdlib_client.hpp`,
`services/core-cpp/tests/stub_backend.cpp`,
`services/core-cpp/tests/uds_test.cpp`). One sibling edit interleaves
with mine in `services/core-cpp/src/config.cpp` (see §2.4): the
`NTGCALLS_LIB` / `TDJSON_LIB` runtime env reads were removed by Worker C
while I was working — my `.env.example` follows the tree as it stands
at the end of this audit, and §5.5 records the conflict. All "verified"
claims below are against the end-of-audit tree state; re-run the
cross-check in §6 if the tree moves again.

Verification method: every env var was enumerated by grep over
`services/backend-go` (`os.Getenv`/`os.LookupEnv`, `setStr`/`setInt`/
`setFloat`, plus the local `envInt` helper in `soulseek.go`) and
`services/core-cpp/src` (`envStr`/`envInt`/`envIdSet`/`envPresent`/
`getenv`), test files excluded; every path was checked with `ls`;
both TOML parsers were run against the edited example (see §6).

## 1. Already correctly wired (no change needed)

- **parlay.toml search path** — identical on both sides: `$PARLAY_CONFIG`,
  then `./parlay.toml`, `<exeDir>/parlay.toml`, `<exeDir>/../parlay.toml`
  (`config.go:defaultConfigPath`, `config.cpp:defaultConfigPath`). A
  missing file is not an error on either side; an explicit
  `$PARLAY_CONFIG` that cannot be read is an error on either side.
- **Unified-schema defaults** — byte-identical in `config.go:defaults()`,
  `config.hpp` struct defaults, and `parlay.toml.example`: rate limit
  chat 5 @ 1.0/s, user 3 @ 1.0/s; timeouts 30000/30000/10000/300000/5000
  ms; voice `standard_192k` (192 kbps), resampler `sinc_256`, 9-source
  ladder `navidrome, soulseek, deezer, netease, jiosaavn, youtube,
  spotify, tiktok, radio`; limits 60 min / 20 / 30 s / 120 s. Both
  parsers validate the same ranges (e.g. capacity 1..10000,
  `max_queue_size` 1..10000). C++ `config_test` pins the example values;
  Go `TestDefaultsMatchExample` pins the same.
- **UDS socket path** — byte-identical on both sides:
  `<data_dir>/parlay-backend.sock`, overridable by `PARLAY_UDS_PATH`
  on both sides (Go `Config.SocketPath()`, `cmd/backend/main.go:119`;
  C++ `ipc.cpp:defaultSocketPath`, `main.cpp:583` `IpcClient ipc(cfg)`).
  Both sides fall back identically when the override is unset or empty,
  and both derive it from the same `PARLAY_DATA_DIR` (default `data`).
  Trailing-slash handling agrees (`filepath.Join` vs explicit strip).
  Contract `contracts/uds-protocol.md §1` agrees.
- **Data dirs / 0700** — bot session `<data>/tdlib-bot`, assistant
  session `<data>/tdlib` on the core side (`tdlib_client.cpp:
  fromEnvFor`, `TD_BOT_SESSION_DIR` / `TD_SESSION_DIR` overrides);
  both created with mode 0700 enforced by
  `TdLibClient::ensureSessionDir0700` (dir + `files/` subdir).
  Backend store `<data>/parlay.db` (bbolt, 0600) in a 0700 data dir
  (`store.go:Open`, `MkdirAll 0o700`); backend UDS socket chmod 0600
  (`uds.go:314`). No `data/` dir exists at the repo root on disk — all
  of these are created at runtime, which is correct (nothing to fix).
- **Spotify redirect URI** — the exact value
  `http://127.0.0.1:8080/callback` appears exactly once in code/config
  (`.env.example`), never as `localhost`, and is config-driven: env
  `SPOTIFY_REDIRECT_URI` → `[backend] spotify_redirect_uri` → empty
  (OAuth refuses honestly when unset, `spotify.go:creds`,
  `oauthCredsOK`). Trap documented in `.env.example`: the URI is
  independent of core `PORT` — changing `PORT` does not move it, and
  it must match the Spotify app registration exactly.
- **Token-bucket limiter** — chat 5 @ 1.0/s, user 3 @ 1.0/s everywhere:
  both parsers' defaults, `parlay.toml.example`, and the core
  `RateLimiter(cfg.rate_limit)` (`main.cpp:610`). The backend parses
  `[rate_limit]` but does not enforce it — correct by design, the core
  is the only entry point and the backend only receives already-gated
  UDS calls. `ErrRateLimited` exists in the backend only as a code.
- **yt-dlp daemon + venv paths** — the backend locates them as
  documented: `$PARLAY_YTDLP_PYTHON`, else walk-up from CWD for
  `scripts/yt-dlp-venv/bin/python`, else `python3`
  (`resolver/ytdlp.go:findYtDlpPython`); `$PARLAY_YTDLP_WORKER`, else
  walk-up for `scripts/yt_dlp_worker.py` (`findWorkerScript`). Both
  files exist on disk (`scripts/yt_dlp_worker.py`,
  `scripts/yt-dlp-venv/bin/python`). See §5.6 for the one hardcoded
  path that is NOT fine.
- **slskd address** — code default `http://127.0.0.1:5030` in both
  places that define it (`config.go` `Backend.SlskdURL` default;
  `resolver/soulseek.go:slskdBaseURL` fallback), `SLSKD_HOST` accepted
  as alias, `SLSKD_URL` wins. Matches the documented optional slskd
  on :5030.
- **Settings keys** — the backend owns all 15 keys with matching
  names on the wire (`settings.go:knownKeys` /
  `contracts/ipc.json:chat.settings`): `admin_mode, play_mode,
  cmd_delete, clean_mode, clean_delay_min, thumbnails, autoplay,
  autoplay_lang, autoplay_mood, silent_mode, auto_delete,
  auto_delete_min, language, paginate_purge_old, chat_music_download`.
  Defaults match the core's panel struct (`ui_texts.hpp:ChatSettings`)
  on 12 of 12 shared keys (see §5.1–§5.3 for the 3 divergences).

## 2. Mismatches fixed

### 2.1 `parlay.toml.example` — the `[backend]` table was missing entirely

The Go backend parses 17 `[backend]` keys, but the example documented
none of them — a user copying the example could not discover that the
table exists, and the file disagreed with what the code parses.
Old: file ended at `[limits]` (84 lines, no `[backend]`).
New: appended a `[backend]` section (39 lines) with `data_dir = "data"`
and `spotify_redirect_uri = "http://127.0.0.1:8080/callback"` live, the
remaining keys commented, and an explicit LIVE vs PARSED-BUT-UNPLUGGED
split (see §5.4). The C++ parser ignores unknown top-level tables, so
the addition is invisible to the core; the Go parser decodes every key
shown. Verified by both parsers (§6).

### 2.2 `.env.example` — stale 12-service topology, 13 missing vars

Old: 86 assignment lines, header claiming "84 vars read in code, 84
listed, 0 missing, 0 extras" (stale since Phase 3).
New: 88 assignment lines; mechanical cross-check now exact — every var
in the file is read in code (0 extras), and every var read in code is
either in the file or named in the REMOVED comment block / test-only
(see §6).

Removed (24 assignment lines): `MONGO_URI`, `REDIS_URL`, `VC_GO_URL`,
`VC_TS_URL`, `VC_RUST_URL`, `FX_URL`, `SHARE_URL`, `QUEUE_URL`,
`SPOTIFY_URL`, `RADIO_URL`, `DL_URL`, `DOWNLOADERS_URL`, `NETEASE_URL`,
`CORE_URL`, `PARLAY_DL_DIR`, `PARLAY_VOICE_PYTHON`, `VC_PROFILE`,
`YT_DLP`, `DEEZER_FORMAT`, `SLSKD_SEARCH_MIN_INTERVAL_SECS`,
`SLSKD_MAX_PEER_ATTEMPTS`, `SLSKD_SLOW_MIN_MBPS`,
`PARLAY_SPOTIFY_MAX_MATCH_SEC`, `PARLAY_BOT_USERNAME`. All are
unread by any live code path (verified by grep over both trees, tests
excluded) except `MONGO_URI`/`REDIS_URL`/the 11 service URLs, which
are still read by `config.cpp:applyLegacyEnv` — see §5.5. The names
are preserved in a comment-only REMOVED block (no assignments remain)
so a user migrating from the old file can see what was deleted and why.

Added as live assignments (28): `PARLAY_CONFIG`, `PARLAY_UDS_PATH`,
the 16 `PARLAY_*` TOML overrides (`PARLAY_RATE_LIMIT_CHAT_CAPACITY`,
`PARLAY_RATE_LIMIT_CHAT_REFILL_PER_SEC`,
`PARLAY_RATE_LIMIT_USER_CAPACITY`,
`PARLAY_RATE_LIMIT_USER_REFILL_PER_SEC`, `PARLAY_TIMEOUT_RESOLVE_MS`,
`PARLAY_TIMEOUT_JOIN_MS`, `PARLAY_TIMEOUT_IPC_CMD_MS`,
`PARLAY_TIMEOUT_DOWNLOAD_MS`, `PARLAY_TIMEOUT_HEALTHZ_MS`,
`PARLAY_VOICE_PROFILE`, `PARLAY_SOURCE_LADDER`,
`PARLAY_RESAMPLER_QUALITY`, `PARLAY_MAX_DURATION_MINUTES`,
`PARLAY_MAX_QUEUE_SIZE`, `PARLAY_PREFETCH_LEAD_TIME_SEC`,
`PARLAY_URL_CACHE_TTL_SEC` — defaults match `parlay.toml.example`
exactly), `TD_BOT_SESSION_DIR`, `PARLAY_YTDLP_PYTHON`,
`PARLAY_YTDLP_WORKER`, `TELEGRAM_BOT_USERNAME` (replaces dead
`PARLAY_BOT_USERNAME` — the share service reads
`TELEGRAM_BOT_USERNAME`), `START_IMG`, `PING_IMG`,
`PARLAY_UPDATES_URL`, `PARLAY_SUPPORT_URL`, `PARLAY_SOURCE_URL`,
`PARLAY_SPOTIFY_OAUTH_KEY`.

Corrected defaults/comments (old → new):
- `SLSKD_URL`: `http://slskd:5030` (dead compose hostname) →
  `http://127.0.0.1:5030` (the code default).
- `SLSKD_DOWNLOAD_DIR`: `$PARLAY_DL_DIR/.slskd-downloads` (dead var,
  wrong path) → default `<PARLAY_DATA_DIR>/soulseek`
  (`soulseek.go:locateDownloaded`; note §5.7 fallback caveat).
- `PARLAY_TOKEN`: comment corrected — the UDS socket is guarded by
  filesystem perms (0600 in a 0700 dir), not by the token; the token
  authenticates the core HTTP API (`core_api.cpp`).
- `DURATION_LIMIT` / `QUEUE_LIMIT`: re-marked deprecated legacy
  aliases (still honoured after the `PARLAY_*` names on both sides).
- `TD_QR=1`, `PORT=8080`, `PARLAY_VOTE_THRESHOLD=3`,
  `SPOTIFY_POLL_INTERVAL_SECS=30`, `GEMINI_MODEL=gemini-2.0-flash`,
  `AUDD_API_BASE=https://api.audd.io`, `LEMONIC_API`,
  `SLSKD_API_TIMEOUT_SECS=30`, `SLSKD_DOWNLOAD_TIMEOUT=900`,
  `SLSKD_STALL_SECS=120`, `MAX_BIT_DEPTH=24`,
  `MAX_SAMPLE_RATE_HZ=96000`: kept, defaults re-verified against code
  (all match; `DEEZER_FORMAT`/`SLSKD_SEARCH_MIN_INTERVAL_SECS`/
  `SLSKD_MAX_PEER_ATTEMPTS`/`SLSKD_SLOW_MIN_MBPS`/
  `PARLAY_SPOTIFY_MAX_MATCH_SEC` are now hardcoded in code — Deezer
  format ladder FLAC→MP3_320→MP3_128 in `deezer.go`, slskd search
  pacing 10.0 s in `soulseek.go:39`, stall default 120 s — so the env
  vars were dead and are removed, not renamed).
- `PARLAY_SPOTIFY_OAUTH_KEY`: documented as base64 of 32 random
  bytes; unset = key generated once, stored
  `<data_dir>/spotify-user-key` (0600) (`crypto.go:resolveKey`).

### 2.3 Empty `PARLAY_DATA_DIR` diverged the two sides (fixed, 3 files)

Old: `PARLAY_DATA_DIR=""` (set-but-empty, e.g. `PARLAY_DATA_DIR=` in
`.env`) made the Go backend fail validation (`backend.data_dir: must
not be empty`) while the core silently proceeded with `data_dir=""` —
socket fell back to `data/parlay-backend.sock`, but the TDLib session
dirs resolved to absolute `/tdlib-bot` and `/tdlib`
(`tdlib_client.cpp:fromEnvFor`), and the dispatcher would have used
`/replied`, `/lyrics` under root.
New: set-but-empty now means "unset" on both sides (falls back to
`"data"`):
- `services/backend-go/internal/config/config.go:applyEnv` — guarded
  `PARLAY_DATA_DIR` application (trimmed non-empty only).
- `services/core-cpp/src/config.cpp:applyLegacyEnv` — guarded
  `data_dir` and `td_session_dir` assignments the same way.
- `services/core-cpp/src/tdlib_client.cpp:fromEnvFor` — guarded
  `data_dir`, `TD_BOT_SESSION_DIR`, and `TD_SESSION_DIR` the same way.
Go config tests + C++ `config_test` pass (§6).

### 2.4 `NTGCALLS_LIB` / `TDJSON_LIB` — removed from `.env.example` (sibling change, recorded here)

Old: listed as live runtime vars in `.env.example`.
New: moved to the REMOVED block. A concurrent Worker C edit removed
the `ntgcalls_lib` / `tdlib_lib` fields and their `envStr` reads from
`config.hpp` / `config.cpp` during this audit (both libraries are now
linked at build time; `TDJSON_LIB` survives only as a CMake variable,
`-DTDJSON_LIB=...`). I did not make that code change; I only updated
the example to match the tree as it stands. See §5.5.

## 3. Credentials the user still needs to supply (names only, no values)

All values go into the VPS `.env` by the user, never via chat (standing
rule). Status per variable:
- `BOT_TOKEN`, `API_ID`, `API_HASH` — required to run; already in the
  user's hands (bot `@punlobot` verified via getMe in Phase 3; the
  token in `~/workspace/parlay/.env` is to be revoked after testing).
- `PARLAY_TOKEN` — required for the core HTTP API auth; any value the
  user chooses (shared by core only now; the backend never reads it).
- `SPOTIFY_CLIENT_ID`, `SPOTIFY_CLIENT_SECRET` — Spotify app "Parlay"
  created by the user; user copies id/secret into the VPS `.env`.
  `SPOTIFY_REDIRECT_URI` is already exactly
  `http://127.0.0.1:8080/callback` in the example.
  `PARLAY_SPOTIFY_OAUTH_KEY` — optional; if unset, a key is generated
  and stored under the data dir automatically.
- `AUDD_API_TOKEN` — user has the AudD dashboard login; token to be
  copied into the VPS `.env` (empty = `audio.recognize` answers an
  honest 502).
- `GEMINI_API_KEY` — AI classifier; needed for Spotify lyrics
  classification.
- `GENIUS_ACCESS_TOKEN`, `MUSIXMATCH_API_KEY` — lyrics chain; optional
  (LRCLib is the free fallback).
- `LEMONIC_API_KEY` — optional (unauthenticated Lemonic path is
  upstream behaviour).
- `NAVIDROME_URL`, `NAVIDROME_USER`, `NAVIDROME_PASSWORD` — only if
  the user runs a Navidrome server (none measured in Phase 1).
- `SLSKD_API_KEY` — only if the user's slskd daemon has API-key auth
  enabled; `SLSKD_URL` defaults to the local daemon.
- `TD_PHONE` / `TD_CODE` / `TD_PASSWORD` (2FA) — only for a fresh
  TDLib first-auth; not needed if a pre-authorized session dir is
  dropped at `<data>/tdlib` (assistant) / `<data>/tdlib-bot` (bot).
- `DEEZER_ARL` — dropped by the user ("Deezer don't"); resolver stays
  env-gated/dormant. `NETEASE_MUSIC_U` — skipped by the user; anonymous
  search only.

## 4. Not in scope / not touched

Launchers/scripts (Worker A), non-config code (Worker C), docs
(Worker D). `~/workspace/parlay/.env` was not read or touched.
Nothing pushed. No stubs, mocks, or placeholders added.

## 5. Could not resolve — needs a ruling (exact questions)

### 5.1 `autoplay_lang` canonical default: `"en"` or `""`?
`model.DefaultChatSettings()` returns `AutoplayLang: "en"`, and
`store.GetSettings()` returns that for a chat with no stored settings
— pinned by `model_test.go:95` (`AutoplayLang != "en"` fails). But the
`chat.settings` handler overrides it to `""` before answering
(`settings.go:99-100`, citing inventory §8.1/U3), pinned by
`settings_test.go:45`. The wire-visible default is `""`; any other
future `GetSettings` caller sees `"en"`. QUESTION: which is canonical?
If `""`, the fix is one line in `model.go` plus updating
`model_test.go:95` (I did not change it — the two tests pin opposite
values and the call is the user's, not mine).

### 5.2 `play_mode` vocabulary: `"admin"` or `"admins"`?
The core writes `play_mode = "admin"` / `"everyone"` into its own
store (`dispatcher.cpp:4878`, toggle at `:7262`; gate at `:1099`
checks `== "admin"`). The backend validates `play_mode` as
`"everyone"` or `"admins"` only (`settings.go:169`) and rejects
`"admin"`. Harmless today (separate stores, §5.3), fatal the day the
stores are unified. QUESTION: which spelling wins?

### 5.3 Settings are dual-owned with no sync — who owns them?
The core's settings panel reads/writes the core Store
(`dispatcher.cpp:chatSettings`/`saveChatSettings`, hash
`chat:<id>`, fields `admin_mode`, `play_mode`, `cmd_delete`,
`clean_mode`, `thumb`, `autoplay`, `silent`, `autodelete`,
`autodelete_min`, `lang`, `page_purge`, `musicdl` — 12 fields, i.e.
the 15 minus `autoplay_lang`, `autoplay_mood`, and `clean_mode` as a
separate bool, which the core derives from `clean_delay_min > 0`).
The backend's `chat.settings` UDS action reads/writes bbolt bucket
`settings`, key `chat:<id>`, all 15 keys. The core never calls the
backend's `chat.settings` (only a deadline entry exists,
`main.cpp:588`); the backend never writes the core's store. A
`/settings` toggle and a `chat.settings` UDS call can diverge
silently. The core's store itself is file `data/state.json` by
default (created with default perms — NOT forced 0700/0600 like the
backend store; see also §5.5). QUESTION: after Phase 4, does the
core panel call the backend's `chat.settings` (backend owns settings),
or does the backend defer to the core? This is Worker C work; the
config-side facts are recorded here.

### 5.4 Nine `[backend]` keys are parsed but never consumed
`BackendConfig` fields `SlskdURL`, `SlskdUser`, `SlskdPass`,
`DeezerARL`, `NetEaseMusicU`, `NavidromeURL`, `NavidromeUser`,
`NavidromePass`, `YTDLPPath` are parsed from `parlay.toml` / env by
`config.go:applyEnv` but read by NOTHING else in `backend-go`
(verified: only `DataDir`, `UDSPath`/`SocketPath`, `AuddAPIToken`,
`GeminiAPIKey`, `SpotifyClientID`/`SpotifyClientSecret`/
`SpotifyRedirectURI`/`SpotifyOAuthKey` are consumed). The resolvers
read the env vars directly instead (`soulseek.go:slskdBaseURL`,
`deezer.go:deezerARL`, `netease.go`, `navidrome.go:navidromeEnv`,
`ytdlp.go:findYtDlpPython`). Net effect: setting `slskd_url` /
`deezer_arl` / `netease_music_u` / `navidrome_*` in `parlay.toml`
does NOTHING, while the same-named env var works. Two further traps:
`NAVIDROME_PASS` (config) vs `NAV[I]DROME_PASSWORD` (what the resolver
actually reads) are different names, and `YTDLP_PATH` is dead in both
forms (the daemon uses `PARLAY_YTDLP_PYTHON` / `PARLAY_YTDLP_WORKER`).
QUESTION: wire the resolvers to `cfg.Backend` (Worker C, resolver
changes) or drop the 9 dead keys from the schema (loud unknown-key
failure instead of silent ignore)? I documented them as
parsed-but-unplugged in both example files and did not remove them,
because removal changes the accepted schema and breaks
`TestFileLoadAndEnvOverride` (which pins `slskd_url` parsing).

### 5.5 Core still reads Mongo/Redis + dead service URLs — and a sibling comment now calls them "live"
`config.cpp:applyLegacyEnv` still reads `MONGO_URI`, `REDIS_URL`, and
all 11 service URLs (`VC_GO_URL` … `NETEASE_URL`); `store.cpp`
connects to Mongo, then Redis, then the JSON file when they are set;
`dispatcher.cpp` still posts to `cfg_.netease_url` / `vc_ts_url` /
`dl_url` / `radio_url` / `share_url` (26 call sites per the sibling's
`config.hpp` comment). The locked architecture says NO Mongo/Redis and
NO inter-service HTTP. A concurrent Worker C edit reworded the
`config.hpp` comment to say Mongo/Redis "are live backends when set"
and the URL fields are "dead… kept only until the 26 dispatcher call
sites are re-pointed" — which contradicts FINAL-ARCHITECTURE.md §1.
`.env.example` removes these vars (per this task's instruction) and
warns that `MONGO_URI`/`REDIS_URL` must NOT be set, because the core
would still split state away from the bbolt store if they were.
QUESTION: delete the Mongo/Redis store backends and the HTTP IPC
client + URL fields from the core (Worker C, per the locked
architecture), or amend the architecture to bless them? I did not
touch `store.cpp` / `dispatcher.cpp` / the `Config` struct fields —
removing the reads alone would break the build.

### 5.6 Hardcoded sandbox path in the yt-dlp locator (Worker C)
`services/backend-go/internal/resolver/ytdlp.go:90` falls back to the
absolute path `/home/hatch/workspace/parlay/scripts/yt-dlp-venv/bin/python`.
It only fires when the CWD walk-up fails, but on the VPS (or any
non-sandbox host) that path never exists, so an out-of-tree backend
start silently degrades to system `python3` (which does NOT have
`yt_dlp`) instead of finding the venv. The config-side mitigation is
done: `PARLAY_YTDLP_PYTHON` / `PARLAY_YTDLP_WORKER` are now documented
in `.env.example`. QUESTION: replace the absolute fallback with an
exe-relative lookup in `ytdlp.go` (Worker C, resolver code — not
touched here)?

### 5.7 `resolver.DataDir()` default splits backend data across two roots
`config.Backend.DataDir` defaults to `"data"`, but
`resolver.DataDir()` defaults to `~/.local/share/parlay` when
`PARLAY_DATA_DIR` is unset (`resolver.go:443-452`). In production
`download.go` prefers `cfg.DataDir`, but `netease.go:150,218` uses
`resolver.DefaultDownloadDir()` directly, and the slskd download-dir
default is `<resolver.DataDir()>/soulseek` — so with no
`PARLAY_DATA_DIR` set, NetEase downloads and the slskd lookup dir
land under `~/.local/share/parlay` while the store, socket, and all
other downloads live under `./data`. QUESTION: align
`resolver.DataDir()`'s fallback to `"data"`, or thread `cfg` into the
resolvers (Worker C)? Noted, not changed (resolver code).

### 5.8 Nits found, not fixed (Worker C, one line each)
- `spotify/crypto.go:53,56`: the OAuth-key error names the variable
  `SPOTIFY_OAUTH_KEY`, but the actual env var is
  `PARLAY_SPOTIFY_OAUTH_KEY` — a user following the error sets the
  wrong name.
- `Config::missingRequired()` (`config.hpp:265`) is dead code — nothing
  calls it. `PARLAY_TOKEN` "required" is only a startup warning
  (`main.cpp:538`); the backend never reads `PARLAY_TOKEN` at all
  (UDS auth is filesystem perms only). Kept as required in
  `.env.example` because `core_api.cpp` enforces it on the HTTP API.
- `PARLAY_DATA_DIR=" "` (whitespace-only): Go trims it to unset; the
  core does not trim, so it would use `" "` as the data dir. Degenerate
  input, not guarded.

## 6. Verification

- Go: `go build ./...` clean; `go test ./internal/config/
  ./internal/settings/ ./internal/model/ ./internal/store/` — all `ok`
  (incl. `TestExampleFileLoads`, which loads the edited
  `parlay.toml.example` through the Go parser, and
  `TestDefaultsMatchExample`, which pins `SocketPath` =
  `data/parlay-backend.sock` and `DBPath` = `data/parlay.db`).
- C++: rebuilt `config_test` in `services/core-cpp/build` and ran it
  against the edited example — `ALL PASS: 0 failed` (the `[backend]`
  table is correctly ignored by the core parser; all example values
  pinned: chat 5 @ 1.0/s, user 3 @ 1.0/s, timeouts, ladder, limits).
- Mechanical `.env.example` cross-check (end-of-audit tree): 88
  assignment lines, 0 vars listed-but-unread; every var read in code
  but absent from the file is either named in the REMOVED comment
  block (11 service URLs, `MONGO_URI`, `REDIS_URL`,
  `NAVIDROME_PASS`, `SLSKD_USER`, `SLSKD_PASS`, `YTDLP_PATH`) or
  test-only (`PARLAY_STORE_KILL9_HELPER`, `PARLAY_STORE_KILL9_DIR`,
  read only by `store_test.go`).
- `git diff --numstat` for this worker's 5 files:
  `parlay.toml.example +39/−0`, `.env.example +111/−103` (rewrite),
  `config.go +6/−1`, `config.cpp +11/−4` (the 4 deletions include 2
  lines removed by the concurrent sibling edit, §0, and 2 lines
  replaced by my guards), `tdlib_client.cpp +7/−0`.
