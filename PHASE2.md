# Parlay — Phase 2 Report (real porting)

Date: 2026-11-02. Work in `~/workspace/parlay/`. Nothing pushed to GitHub. No tokens requested or stored. No secrets in the tree (secret scan: no credential values found; env var names only).

## Step 1 — LICENSE files deleted (as ordered by the user)

19 files deleted under `~/workspace/parlay/refs/`, logged in `clone-log.txt`. Verify: `find refs -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*'` returns 0. Nothing in `services/` or the Parlay root was deleted (no LICENSE files there).

- refs/ArcMusic/LICENSE
- refs/Music163bot-Go/LICENSE
- refs/ParvatiMusic/LICENSE
- refs/ParvatiMusic/vendor/ferogram/LICENSE-APACHE
- refs/ParvatiMusic/vendor/ferogram/LICENSE-MIT
- refs/Spotify-Loader/LICENSE
- refs/TGVCBot/LICENSE
- refs/Telegram-Song-Downloader-Bot/LICENSE
- refs/TgMusicBot/LICENSE
- refs/TinaMusic/LICENSE
- refs/YukkiMusic-Go/LICENSE
- refs/beetube-bot/LICENSE
- refs/lemonic-telegram-bot/LICENSE
- refs/music-bot-navidrome/LICENSE
- refs/openradiobot/LICENSE
- refs/remix/LICENSE
- refs/rustify/LICENSE
- refs/telegram-music-bot-daslui/LICENSE
- refs/wavetune/LICENSE

## Step 2 — core-cpp (C++20 router) — Worker A

Ported from refs/anonx-cpp + refs/NoxBeats + refs/TinaMusic. Files: `src/main.cpp` (replaced), `src/json.{hpp,cpp}`, `src/http.{hpp,cpp}`, `src/config.{hpp,cpp}`, `src/telegram_api.{hpp,cpp}`, `src/ipc.{hpp,cpp}`, `src/engine_router.{hpp,cpp}`, `src/voice_call.hpp`, `src/dispatcher.{hpp,cpp}`; `CMakeLists.txt`, `README.md`, `PROGRESS.md`.

Real: Telegram Bot API long-poll client (`getUpdates` 30s, `sendMessage`, `getChatMember`, `getMe`); dispatcher for `/start /help /play /pause /resume /skip /queue /stop /search /download /radio /share /engine` (anonx-cpp parse shape, NoxBeats command surface, 1 cmd/s per-chat rate limit); IPC client `POST /v1/<action>` + `X-Parlay-Token` with `{ok:true,data}` / `{ok:false,error}` envelopes (vc.play/pause/resume/skip/queue/stop, search.query, download.youtube, radio.start, share.create); per-chat engine selection (go|ts|rust, default go, `/engine` to switch) with healthz-probed fail-over to the next engine; group management via `getChatMember` admin check, `PARLAY_ALLOW_CHATS` / `PARLAY_BLOCK_CHATS`, `OWNER_ID`; oversize rule (download `oversize=true` reported as queued for a later MTProto pass); env-only config with required-var check at startup.

Build (verified by worker and re-verified by coordinator): `cmake -S . -B build && cmake --build build` → exit 0, `[100%] Built target parlay-core`, zero warnings with `-Wall -Wextra -Wpedantic` (g++ 13.3.0, cmake 3.28.3). Smoke: binary with no env exits 1 with `missing required env vars: BOT_TOKEN PARLAY_TOKEN`; JSON harness in /tmp verified getUpdates parsing, IPC envelopes, `\u`/surrogate decoding, round-trips — all pass.

Environment-forced choices: no libcurl dev headers and no nlohmann/json in the sandbox → HTTP via system `curl` CLI through `popen()`, JSON hand-rolled. Nothing downloaded at build time.

Still stubbed: voice-call binding (isolated behind a `VoiceCall` interface with a logging stub — TDLib/ntgcalls unbuildable here); MongoDB/Redis (env read, not connected; engine choice in-memory); MTProto (`API_ID`/`API_HASH` read, unused); core HTTP port / GUI `/api/*`; `/download` file delivery (reports `file_path` only); inline queries, callbacks, playlists, loop/seek, autoplay, broadcast, sudo lists.

## Step 2 — vc-go (Go VC engine) — Worker B

Ported from refs/YukkiMusic-Go + refs/ArcMusic + refs/TgMusicBot. Files: `cmd/parlay-vc/main.go` (replaced), `internal/types/types.go`, `internal/queue/room.go`, `internal/queue/room_test.go`, `internal/resolver/resolver.go`, `internal/runner/runner.go`, `internal/server/server.go`, `internal/store/store.go`; `go.mod` unchanged (stdlib only), `README.md`, `PROGRESS.md`.

Toolchain: Go was NOT installed. Worker installed Go 1.23.0 linux-amd64 from go.dev into `~/workspace/parlay/.tools/go` (first curl without `-L` returned only a 302; retry with `-L` fetched the 73.6 MB tarball). Verified `go version go1.23.0 linux/amd64`.

Real: HTTP IPC server (default :8001) — `GET /healthz`, `POST /v1/play|pause|resume|skip|stop|queue|seek`, `GET /v1/now-playing`, `X-Parlay-Token` auth when set; thread-safe per-chat queue/playback state; `VC_PROFILE=low-ram|high-perf` (low-ram: 0 prefetchers, 256 KB buffer; high-perf: 2 prefetchers, 4096 KB buffer — both paths real in the runner); 1s auto-advance ticker; JSON-file queue store with atomic writes + restore (restored rooms come back paused); yt-dlp `-j` resolver with degraded URL-pattern fallback when yt-dlp is missing.

Build (re-verified by coordinator): `go vet ./... && go build ./...` → OK (one vet fix: `Room.Seek` renamed `SeekTo`, io.Seeker clash). `go test ./...` → 6/6 pass (`ok parlay/vc-go/internal/queue`).

Smoke (worker + coordinator re-run): low-ram :8001 healthz shows `profile:"low-ram", buffer_kb:256, prefetch:false, yt_dlp:false, degraded:true`; play of a YouTube URL resolved degraded (`yt:dQw4w9WgXcQ`); pause/seek 30/resume/skip/stop all returned correct envelopes; now-playing after stop → `not_found`; play without `chat_id` → `bad_request`; high-perf + `PARLAY_TOKEN=s3cret` :8002 — play without token → 401, with token → 200, queue persisted and restored paused after restart; auto-advance drained a 2s track in 3.5s.

Still stubbed by design: ntgcalls voice joining / audio output — that is the C++ core's `VoiceCall` job; this service is the queue/playback-state engine. MongoDB persistence — `MONGO_URI` accepted and logged, store stays JSON-file (a driver would break the stdlib-only offline build; swap point is `internal/store` only). Not verified live: yt-dlp resolution and high-perf prefetch (need yt-dlp installed; they go live with no code change once it is).

## Step 3 — Rust services — Worker C

Toolchain: cargo/rustc 1.99.1 at `~/.cargo/bin` (on PATH via export). All three std-only, zero crates downloaded, `cargo build --release --offline` clean, 0 warnings each. Shared `src/common.rs` per service (identical copy): std-only JSON, tiny `TcpListener` HTTP server, `GET /healthz`, IPC envelopes, `X-Parlay-Token` check when set (verified: no/wrong header → 401, correct → 200; `/healthz` unauthenticated by design).

### audio-fx-rust (:8003) — from refs/telegram-music-bot-rust
Real `POST /v1/fx` running real ffmpeg 8.1.2 subprocess. Filters: slow-reverb `atempo=0.8,aecho=0.8:0.88:60:0.4`, nightcore `asetrate=44100*1.25,atempo=1/1.25`, bass-boost `bass=g=10:f=110:w=0.6,volume=0.9`, 8d `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`, original `-c:a copy`. `..` paths and missing inputs rejected with `bad_request`. Smoke: 2s 440 Hz tone → all 5 effects returned `{ok:true,data:{output_path}}` with real files (nightcore 219,608 B, 1.991 s). Gaps: no ffmpeg subprocess timeout, no loudness normalisation.

### share-rust (:8004) — from refs/feel-me
Real `POST /v1/share` → `{"link":"https://t.me/ParlayBot?start=share_<16-char token>","token":"..."}` (token from `/dev/urandom`, bot from `$PARLAY_BOT_USERNAME` default `ParlayBot`); `GET /v1/share/:token` returns the exact track; unknown token → 404 `not_found`. Persistence to `./data/shares.json` (atomic tmp+rename) verified across restart. `created_by` accepted but deliberately not stored (anonymous by design). No Telegram call — minting only. Stubbed: no token expiry/revocation, no per-user listing, no Postgres/teloxide backend (intentionally not ported).

### queue-rust (:8005) — from refs/telegram-music-bot-daslui
Real `POST /v1/queue/vote` — 3 distinct approves flipped `queued` false→true at threshold 3 (default, `$PARLAY_VOTE_THRESHOLD`); duplicate approve did not double-count; a decline cancelled an approval (`queued:false, approvals:2, declines:1`); bad vote → `bad_request`. State persisted in `./data/votes.json` and reloaded across restart. Stubbed: no queue-listing endpoint, no vote expiry, no live Spotify playback-queue push (reference's rspotify OAuth intentionally not ported — core owns playback).

## Step 3 — downloaders (Python) — Worker D

Ported from refs/Telegram-Song-Downloader-Bot (sanila) + refs/Spotify-Loader + refs/beetube-bot (provider registry). Files: `src/main.py`, `src/server.py` (stdlib `ThreadingHTTPServer`), `src/ytdlp.py`, `src/spotify.py`, `src/providers.py`, `src/util.py`; `requirements.txt` (`yt-dlp>=2024.1.7`, spotdl commented out as optional); `README.md`, `PROGRESS.md`.

Toolchain: venv `.venv/` created, `yt-dlp 2026.10.26` installed from PyPI via proxy. spotdl deliberately not installed (fallback path is what runs). Python 3.12.3, ffmpeg present.

Real (live tests, server :8010, `PARLAY_DL_DIR=/tmp/parlay-dl-test`): `GET /healthz` → ok; `POST /v1/search` "test" youtube limit 2 → 2 real YouTube tracks in ~3s; search "rick astley never gonna give you up" spotify → 2 tracks `source=spotify` with honest note (matched via YouTube, no Spotify API creds); `POST /v1/download/youtube` (jNQXAC9IVRw "Me at the zoo") → 230,602-byte mp3, `oversize=false`, ~15–23s; `POST /v1/download/spotify` (track 4uLU6hMCjMI75M1A2tKUQC) → 7,045,581-byte mp3, `oversize=false`, Spotify id preserved, resolved via oEmbed, ~24s; `POST /v1/download/deezer` → `engine_unavailable` (stub as designed); `{}` → `bad_request`; unknown route → `not_found`; auth on :8011 (`PARLAY_TOKEN=test-token-123`) — no/wrong token → 401, right token → 200, `/healthz` unauthenticated.

Two real findings, both fixed in code: (1) YouTube bot-gates the web player client from this egress IP ("Sign in to confirm you're not a bot") — downloads retry once with the android player client, no cookies/credentials; search unaffected. (2) Spotify playlist title fallback matched a multi-hour compilation (285 MB mp4 downloading, curl timed out at 240s) — added a song-length match guard (20 min default, `PARLAY_SPOTIFY_MAX_MATCH_SEC`): skips livestreams/long compilations, fails fast (~4s) with an explicit error.

Still stubbed: deezer, tiktok, navidrome routes exist and honestly return `engine_unavailable`. Known limit, not verified end-to-end: Spotify playlist/album track lists cannot be resolved here — yt-dlp's Spotify extractor returns nothing for those URLs and oEmbed gives only the page title. The playlist ZIP/files assembly code is written but unproven for a real multi-track list; full support needs Spotify API credentials or spotdl installed. Test servers stopped, /tmp test downloads removed.

## Coordinator verification (independent re-run)

- LICENSE check: 0 files under `refs/` (find, all three patterns).
- Rebuilds, all PASS: core-cpp `cmake --build build` → `[100%] Built target parlay-core`; vc-go `go vet ./... && go build ./...` OK, `go test ./...` 6/6 pass; all three Rust services `cargo build --release --offline` → `Finished release profile`; downloaders `python3 -m py_compile src/*.py` → OK.
- Combined live smoke (all started together, then stopped; no stray processes after): vc-go :18001 healthz + `POST /v1/play` (YouTube URL, degraded resolve `yt:dQw4w9WgXcQ`) + `GET /v1/now-playing` all returned correct envelopes; audio-fx-rust :18003 healthz + nightcore on a generated 2s tone → real 219,608-byte file; share-rust :18004 healthz + share create → `https://t.me/ParlayBot?start=share_gY2U17Kf75YjwZr6`; queue-rust :18005 healthz + vote → `queued:false, approvals:1, threshold:3`; downloaders :18010 healthz OK (worker's earlier live download tests stand). Core-cpp binary with no env exits with `missing required env vars: BOT_TOKEN PARLAY_TOKEN` (no network attempted).
- One verification hiccup, mine not the workers': my first combined-run command backgrounded the `cd` so the Rust binaries were invoked from the wrong cwd ("No such file or directory"). Re-ran with absolute paths — all three Rust services answered correctly. No worker code was changed to fix this.
- Secret scan: no credential values in services (env names only).

## Still stubbed (whole project)

- Voice: TDLib/ntgcalls binding in core-cpp (`VoiceCall` interface + logging stub); ntgcalls voice joining in vc-go (queue/playback-state engine only, by design).
- vc-ts, vc-rust, spotify-rust, gui-js, radio-js, netease-go: still Phase 1 skeletons — not in Phase 2 scope.
- MongoDB/Redis persistence (JSON-file stores used instead); deezer/tiktok/navidrome downloaders (`engine_unavailable`); Spotify playlist track-list resolution (needs Spotify API creds or spotdl).
- No live Telegram test yet: needs a real BOT_TOKEN + a vc engine up, then `/play` in a group and killing vc-go mid-play to prove fail-over to vc-ts. No live voice streaming verified anywhere — that needs the core's VoiceCall binding built against real TDLib/ntgcalls on a host that can build them.

## Next steps

1. vc-ts (TGVCBot/remix) + vc-rust (ParvatiMusic) as real engines, so core fail-over has somewhere to fail over to.
2. spotify-rust (Rustify watch/profanity), gui-js (Wavetune webapp), radio-js (openradiobot), netease-go (Music163bot-Go).
3. Live Telegram test with a real bot token; then voice-call binding on a host that can build TDLib/ntgcalls.
4. Push to the user's repo once they share the URL (not pushed yet).
