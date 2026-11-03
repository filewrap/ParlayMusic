# Phase 3 Wave 1 — Scaffold report (backend-go)

Worker 1 — Scaffold. 2026-11-03. Repo `~/workspace/parlay/`, main at
3d84fb6. No commit/push (Wave 2 owns that). No `.env` touched, no
service started with the user's token, all tests use dummy creds
only. No Phase 4 work.

## What was built

`services/backend-go/`, module `parlay/backend-go`:

| Path | Contents |
|---|---|
| `cmd/backend/main.go` | config load → store open → UDS listen → signal handling. Handler table is empty except `ping` (registered by `uds.NewServer`); a comment marks where Wave 2 registers the 56 actions. |
| `internal/model/model.go` | Track, NowPlaying, QueueItem, RoomState, VoteState, ShareLink, SpotifyState, SkipCheckRecord, ChatSettings, RadioState. |
| `internal/config/config.go` | parlay.toml parser + env overrides (BurntSushi/toml, see below). |
| `internal/store/store.go` | bbolt store, 7 buckets, full CRUD for every model struct. |
| `internal/uds/uds.go` | UDS server: frame codec, handler registry, events, JoinRequest. |
| `internal/uds/uds_test.go`, `internal/{model,config,store}_test.go` | 17 tests, all green (see Verification). |
| `tests/interop/core_peer.cpp` | Real-C++-client interop probe (see UDS interop). |

## Module layout

```
services/backend-go/
  go.mod / go.sum
  cmd/backend/main.go
  internal/
    model/   model.go, model_test.go
    config/  config.go, config_test.go
    store/   store.go, store_test.go
    uds/     uds.go, uds_test.go
  tests/interop/core_peer.cpp
```

## Config keys

Same file, same keys, same validation as the C++ core
(`services/core-cpp/src/config.cpp` — ranges copied verbatim):
defaults < parlay.toml < env. Missing file runs on honest
defaults (`Source` records that); malformed file fails loudly
naming the exact key; unknown keys fail; an explicit
`$PARLAY_CONFIG` that cannot be read is an error. Search order
mirrors the core: `$PARLAY_CONFIG`, `./parlay.toml`,
`<exeDir>/parlay.toml`, `<exeDir>/../parlay.toml`.

Shared schema (from `parlay.toml.example`):

- `[rate_limit.chat]` / `[rate_limit.user]`: `capacity`,
  `refill_rate_per_sec` (defaults 5/1.0, 3/1.0).
- `[timeouts]`: `resolve_ms` 30000, `join_ms` 30000,
  `ipc_cmd_ms` 10000, `download_ms` 300000, `healthz_ms` 5000.
- `[quality]`: `voice_profile` (`standard_192k`, opus 192 kbps;
  `high_256k` 256, `ultra_320k` 320), `source_ladder`
  (navidrome, soulseek, deezer, netease, jiosaavn, youtube,
  spotify, tiktok, radio), `resampler_quality` (`sinc_256`).
- `[limits]`: `max_duration_minutes` 60, `max_queue_size` 20,
  `prefetch_lead_time_sec` 30, `url_cache_ttl_sec` 120.

Backend keys (`[backend]` table and/or env):

| Key | Env | Default |
|---|---|---|
| `data_dir` | `PARLAY_DATA_DIR` | `data` |
| `uds_path` | `PARLAY_UDS_PATH` | `<data_dir>/parlay-backend.sock` |
| `slskd_url` / `slskd_user` / `slskd_pass` | `SLSKD_URL` / `SLSKD_USER` / `SLSKD_PASS` | `http://127.0.0.1:5030` / empty |
| `deezer_arl` | `DEEZER_ARL` | empty |
| `netease_music_u` | `NETEASE_MUSIC_U` | empty |
| `spotify_client_id` / `spotify_client_secret` / `spotify_redirect_uri` | `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` / `SPOTIFY_REDIRECT_URI` | empty |
| `spotify_oauth_key` (AES-256-GCM key, base64, 32 bytes) | `PARLAY_SPOTIFY_OAUTH_KEY` | empty |
| `navidrome_url` / `navidrome_user` / `navidrome_pass` | `NAVIDROME_URL` / `NAVIDROME_USER` / `NAVIDROME_PASS` | empty |
| `gemini_api_key` | `GEMINI_API_KEY` | empty |
| `ytdlp_path` | `YTDLP_PATH` | `yt-dlp` |

Finding: a `[backend]` table in the shared parlay.toml is safe —
the core only validates keys inside the tables it owns and its own
comment says the file "is shared with the Go backend and other
components, which own their own sections". Secrets must still come
from env in production; the file form exists for local dev only and
must never be committed with real values.

TOML library: `go get github.com/BurntSushi/toml` worked first
try (v1.4.0 at scaffold time, see the go.mod note below) — no
in-tree parser was needed. Strictness comes from
`MetaData.Undecoded()` (unknown keys fail) plus the mirrored
range validation.

## Store buckets

One file, `<data_dir>/parlay.db` (0600 in a 0700 dir), bbolt.
FRESH START (OD-2): no migration code of any kind.

| Bucket | Key | Value |
|---|---|---|
| `rooms` | chat id | `RoomState` |
| `queue` | per-chat sub-bucket, keys = 8-byte BE position | `QueueItem` (play order = key order) |
| `votes` | `<chatID>:<trackID>` | `VoteState` |
| `shares` | token | `ShareLink` |
| `spotify` | `oauth:<userID>` | `SpotifyState` (`token_ciphertext` = base64 AES-256-GCM ciphertext; key from config/env, never hardcoded, never stored) |
| `spotify` | `watch:<userID>` | `"1"` while watched |
| `spotify` | `dislike:<userID>:<trackID>` | `"1"` while disliked |
| `spotify` | `skipcheck:<trackID>` | `SkipCheckRecord` |
| `settings` | chat id | `ChatSettings` (15 keys; unknown chat reads `DefaultChatSettings()`) |
| `radio` | chat id | `RadioState` (`reachable` is `*bool`: nil = never probed) |

CRUD: `Put/Get/DeleteRoom`, `ListRoomIDs`, `Put/GetQueue`
(`DeleteRoom` also drops the queue), `Put/Get/DeleteVote`,
`Put/Get/DeleteShare`, `Put/Get/DeleteSpotify`,
`SetWatch/IsWatching/ListWatches`,
`SetDislike/IsDisliked`, `Put/GetSkipCheck`,
`Put/GetSettings`, `Put/Get/DeleteRadio`.

## UDS interop evidence

Wire: 4-byte LE length + JSON, 16 MiB max, byte-compatible with
`parlay::uds::encodeFrame` (unit test pins the exact bytes
`03 00 00 00 01 02 03` for payload `{1,2,3}` and rejects 0 /
>16 MiB prefixes). One active connection (a new accept closes the
old one); responses echo `request_id`; backend-initiated ids start
at 1,000,000; `test.sleep_ms` and unknown actions answer
`not_found`; handler deadlines enforced via context — work past
`deadline_ms` is abandoned with no late response written.

Real-core proof — `tests/interop/core_peer.cpp` links the REAL
`parlay::IpcClient` (`ipc.cpp`, `json.cpp`, `config.cpp`,
`toml.cpp`) against the live Go backend (built with
`g++ -std=c++20`, run 2026-11-03 ~10:38 IST):

```
PASS healthz (ping) ok over the real IpcClient
PASS connected after healthz
PASS post ping ok
PASS ping data.service == backend-go
PASS vc.play (unwired) answers not_found/404
PASS GET /v1/now-playing (unwired) answers not_found
PASS test.sleep_ms answers not_found (stub-only)
CORE_PEER: ALL PASS
```

Same binary also verified with a raw Python frame client
(`ping` → `{service: "backend-go", version: "0.1.0"}`,
`vc.play` → `not_found`, `test.sleep_ms` → `not_found`) and
with `parlay.toml.example` as the config file. Go unit tests
additionally prove the duplex direction against a raw peer:
`JoinRequest` frame shape (`request_id` ≥ 1,000,000,
`local_sdp`, `chat_id`) + `JoinResponse` matching, and event
delivery (`TrackStarted` with top-level `chat_id`).

Not yet proven end-to-end (needs Wave 2): a backend-initiated
`JoinRequest` arriving at the real C++ client's registered
handler — no Wave 1 code path emits one. The frame format and
the response-matching logic are proven on the Go side; the C++
handler hook is proven by the core's own `uds_test` against the
stub. Wave 2's `vc.join` wiring closes this loop.

## Verification (2026-11-03 ~10:38 IST)

Toolchain: `~/workspace/parlay/.tools/go/bin/go` (Go 1.23.0);
module cache `/home/hatch/go/pkg/mod`.

- `gofmt -l` on all scaffold files: clean.
- `go vet` on `internal/model`, `internal/config`,
  `internal/store`, `internal/uds`, `cmd/backend`: clean.
- `go test -count=1` on those five packages: **17 tests, all
  pass** (model 5, config 5, store 2, uds 5) — including
  `TestStoreSurvivesKill9` (writes room + queue + settings,
  SIGKILLs the holder process, reopens, state intact) and
  `TestDeadlineAbandonsWork` (slow handler + 50 ms deadline →
  no response frame).
- `go build -o /tmp/backend-go ./cmd/backend`: clean; binary
  runs, listens on `<data>/parlay-backend.sock` (0600),
  answers ping, shuts down cleanly on SIGTERM.
- No `TODO` / `FIXME` / `HACK` / `Unimplemented` in any
  scaffold file (grep, zero hits).

## What Wave 2 must wire

1. Register the 56 contract actions in `cmd/backend/main.go`
   via `server.Register(action, handler)` — the table is empty
   except `ping` by design. `Handler` signature:
   `func(ctx context.Context, req uds.Request) (any, error)`;
   return `*uds.HandlerError` for contract error codes
   (`bad_request`, `unauthorized`, `not_found`,
   `engine_unavailable`, `download_failed`, `rate_limited`,
   `internal`); any other error becomes `internal`.
2. Resolvers (YouTube InnerTube, JioSaavn, Deezer, NetEase,
   Navidrome, Soulseek/slskd, Spotify, TikTok, lyrics, radio
   verifier) + the persistent yt-dlp daemon; honour
   `req.DeadlineMs` and `cfg.Quality.SourceLadder`.
3. Voice: ntgcalls cgo binding; use `server.RequestJoin(ctx,
   chatID, localSDP)` for the duplex handshake (returns
   `remote_sdp, group_call_id`) and `server.Emit(...)` for
   `TrackStarted` / `TrackAdvanced` / `TrackEnded` /
   `QueueEmpty` / `DownloadReady` / `VoiceError`.
4. Spotify OAuth crypto: encrypt tokens with AES-256-GCM
   under `cfg.Backend.SpotifyOAuthKey` before
   `store.PutSpotify`; the store only ever holds ciphertext.
5. Per-room playback state machine on top of
   `store.RoomState`/`PutQueue` (loop 0–10, volume 0–200,
   speed, FX preset/params are already in the model).
6. The `chat.settings` POST path: validate the key against
   the 15 known keys, store via `PutSettings`, return the full
   15-key object (GET returns stored settings or defaults).
7. Re-run `tests/interop/core_peer.cpp` after wiring `vc.play`
   (its not_found expectation for `vc.play`/`vc.now_playing`
   will then flip to ok) and close the JoinRequest loop
   against the real C++ handler.

## Coordination notes (for the parent, not Wave 2's code)

- **Concurrent writer in the same module.** While this
  scaffold was being written, a second worker landed
  `internal/resolver/` and `internal/voice/` plus a rewritten
  `go.mod` in `services/backend-go/` (go 1.26.0, bbolt v1.5.0,
  BurntSushi/toml v1.6.0 — versus this task's spec of go 1.23,
  bbolt v1.3.10). I did not touch their files or revert
  `go.mod`. Consequences:
  - `go vet ./...` / `go test ./...` at module root currently
    FAIL on their packages (vet: `illegal rune literal` in
    `internal/resolver/deezer_secret.go:5` and
    `youtube_key.go:7`; voice tests fail on SDP shape and
    feed errors). Those failures are theirs, not the
    scaffold's — all scaffold packages are green in
    isolation (see Verification).
  - The scaffold code compiles and passes under the rewritten
    `go.mod` (verified: the green runs above used it, with
    the Go 1.26 toolchain auto-downloaded). The spec's
    bbolt v1.3.10 / go 1.23 pin is therefore unverified in
    the current tree — the parent should decide the pinned
    versions (go 1.23 + bbolt v1.3.10 per spec, or accept the
    1.26 / 1.5.0 the other worker introduced) before Wave 2
    commits.
  - Two scaffold test failures found during the first
    full-tree run were mine and are fixed: a 0700-dir
    assertion on a pre-existing `t.TempDir()` (now asserts
    on a directory `Open` creates) and a race in
    `TestJoinRequestHandshake` (now pings first so the
    accept loop has registered the connection).
- The task's "go get BurntSushi/toml" and "bbolt v1.3.10
  verified downloadable" claims both held (proxy works),
  but the live `go.mod` no longer reflects them — see above.
