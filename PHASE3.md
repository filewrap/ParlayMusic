# Parlay — Phase 3 Report (remaining services, real code)

Date: 2026-11-02. Work in `~/workspace/parlay/`. Nothing pushed to GitHub. No tokens requested or stored. No secrets in the tree (secret scan: env var names only, no credential values).

## What was built

Three workers in parallel, each verified live; coordinator re-verified everything together (rebuilds + combined live smoke). Ports below are the docker-compose defaults; coordinator smoke used test ports 18001–18011 + 13000 with token `p3-test` and isolated `/tmp/p3` data dirs.

### Worker A — vc-ts + radio-js (Node 24, stdlib only, zero npm installs)

**vc-ts** (`services/vc-ts/`, :8002) — from refs/TGVCBot + refs/remix + refs/lemonic-telegram-bot.
Files: `src/main.js` (rewritten), `src/resolvers.js` (new), `src/voicecall.js` (new, voice stub), `Dockerfile` (new, node:24-slim + yt-dlp), `README.md`, `PROGRESS.md`.
Real: `GET /healthz`; `POST /v1/play|pause|resume|skip|stop|seek|queue` (add/remove/clear/list); `GET /v1/now-playing?chat_id=`; `POST /v1/search` (youtube|jiosaavn|lemonic|all, per-source status); `POST /v1/lemonic/search|download`; `POST /v1/inline`. Per-chat rooms, 1s auto-advance ticker, JSON persistence (restored rooms come back paused), token auth (401, healthz exempt). Play accepts YouTube URL, free text, `jiosaavn:<query>`, `jsvn:<id>`. YouTube resolves live via yt-dlp; JioSaavn via TGVCBot's public proxy; Lemonic key-gated.
Worker tests: 20/20 pass — live yt-dlp resolve (Rick Astley, 213s, real title), live JioSaavn queue add ("Kesariya"), auto-advance (seek to 212/213s → next track in 3s), persistence across kill+restart, Lemonic honest `engine_unavailable` without key. Bugs fixed: yt-dlp `ytsearchN:` JSONL parsing (line-by-line, sources now report `"error"` instead of silent empty), wrong yt-dlp path.

**radio-js** (`services/radio-js/`, :8008) — from refs/openradiobot.
Files: `src/main.js` (rewritten), `src/stations.js` (new, 6 curated stations — upstream has no station list, this one is Parlay-original), `Dockerfile` (new), `README.md`, `PROGRESS.md`.
Real: `GET /healthz`; `GET /v1/radio/stations`; `POST /v1/radio/start`, `POST /v1/radio` (start|stop), `POST /v1/radio/stop`; extra `GET /v1/radio/status?chat_id=`. ffprobe stream probing at startup + per start, 180s rotating fallback playlist, per-chat radio state, persistence, token auth.
Worker tests: 8/8 pass — stations list (5/6 reachable; KEXP unreachable, reported honestly), start reachable station (ffprobe-confirmed mp3), start unreachable → `fallback_playlist:true`, stop via both routes, unauth 401.

### Worker B — vc-rust + spotify-rust (Rust, cargo 1.99.1, std-only, zero crates, --offline)

**vc-rust** (`services/vc-rust/`, :8003) — from refs/ParvatiMusic.
Files: `src/main.rs` (rewritten), `src/common.rs` (new, copied from audio-fx-rust pattern + query params), `README.md`, `PROGRESS.md`, `Cargo.lock` (zero deps).
Real: `GET /healthz`; `POST /v1/play|pause|resume|skip|stop|seek`; `POST /v1/queue` (add|remove|clear|list); `GET /v1/now-playing?chat_id=`. Per-chat queue state, auto-advance ticker, JSON persistence, token auth, `engine:"rust"`. Play on idle room starts at once; on busy room queues (`queued:true`). yt-dlp subprocess resolve if installed, else pattern resolve.
Worker tests: 24/24 pass — auto-advance verified (next track at position 2 after 3s), restart persistence, 7 error cases (400s/404), 401s, voice-stub log lines. Bugs fixed: Spotify URL parse ate first 2 id chars; failed pause/seek/skip no longer create+ persist empty rooms; empty dislike sets pruned.
Contract change (contract-first rule): `contracts/ipc.json` gained `spotify.dislike`, `spotify.resolve`, `spotify.watches` (35 actions total, re-validated as JSON); `.gitignore` gained `**/data/`.

**spotify-rust** (`services/spotify-rust/`, :8007) — from refs/rustify.
Files: `src/main.rs` (rewritten), `src/common.rs` (new), `README.md`, `PROGRESS.md`, `Cargo.lock` (zero deps).
Real: `GET /healthz`; `POST /v1/spotify/watch` (start|stop), `GET /v1/spotify/watches`; `POST /v1/spotify/dislike` (add|remove); `POST /v1/spotify/skip-check` → `{skip, reason, matched_word?, disliked_by?, lyrics_checked, lyrics_available, lyrics_source}`; `POST /v1/spotify/resolve` (URL/URI → id, case preserved). 54-word profanity list (title+artist, case-insensitive); real LRCLib lyrics fetch with short timeout, no faked verdicts when unavailable; per-user + any-user dislike skip.
Worker tests: 24/24 pass — clean classic with real LRCLib lyrics, profane title matched, no-lyrics fallback, dislike add/remove round-trip, resolve URL+URI, 401s, restart persistence. Bugs fixed: `spotify:` URI resolve lower-cased ids; "Imagine" false-positive (`hell` in real lyrics) — mild tier removed from wordlist, 60→54 words.

### Worker C — gui-js + netease-go

**gui-js** (`services/gui-js/`, :3000) — from refs/wavetune. Plain Node stdlib, no Next.js build, no npm deps.
Files: `app/main.js` (rewritten, ~470 lines), `README.md`, `PROGRESS.md`. `package.json` unchanged.
Real: `GET /` single-page player (now-playing, queue, search box, play/pause/skip, volume; polls `/api/*` every 15s, never hardcodes data); `GET /healthz`; `GET /api/now-playing`, `/api/queue` (proxy `CORE_URL`, else labelled `local-demo` state); `GET /api/search` (proxy `CORE_URL`, else `NETEASE_URL`, else `engine_unavailable`); `POST /api/play|pause|skip` (proxy `VC_GO_URL` → `VC_TS_URL`, else demo state + honest note). Telegram WebApp hook (`Telegram.WebApp.ready()/expand()`, guarded; Mini App URL is `/`).
Worker tests: 9/9 pass on :13000 demo mode; proxy paths verified live — `/api/play` via `VC_GO_URL` returned `engine:"go"`, `/api/search` via `NETEASE_URL` returned 10 real NetEase tracks.

**netease-go** (`services/netease-go/`, :8010) — from refs/Music163bot-Go. Go 1.23.0 (`~/workspace/parlay/.tools/go`), stdlib only.
Files: `cmd/parlay-netease/main.go`, `internal/netease/client.go` (new), `internal/server/server.go` (new), `README.md`, `PROGRESS.md`. `go vet` + `go build` clean.
Real: `GET /healthz`; `POST /v1/netease/search|download|lyrics`, token auth (healthz exempt). Search via NetEase public cloudsearch API (no login/cookie); lyrics via lyric API; download path implemented (detail → URL → streamed write to `PARLAY_DL_DIR`, 50 MB oversize rule, partial-file cleanup). Client handles NetEase's intermittently duplicated response bodies (decodes first JSON value only).
Worker tests: search "Jay Chou" → 5 real results, "周杰伦" → 3; lyrics for 210049 → real timed lyrics; auth 401s correct; empty query → `bad_request`. **Download honestly blocked: `engine_unavailable`.** The v1 song-URL endpoint rejects plain requests (`{"msg":"参数错误","code":400}` — needs encrypted weapi payload); the non-v1 endpoint returns `url:null, code:404` for every track tried, once a `-462` verification challenge. No audio-recognition port; no `MUSIC_U`/QR login (anonymous by design); no weapi encryption (the route to unlock v1 URLs).

## Coordinator verification (independent re-run)

### Rebuilds — all PASS
- core-cpp: `cmake -S . -B build && cmake --build build` → `[100%] Built target parlay-core`, 0 warnings.
- vc-go: `go vet ./... && go build ./...` OK; `go test -count=1 ./internal/queue/` → **6/6 pass**.
- Rust (all five): `cargo build --release --offline` → `Finished release profile`, 0 warnings each (audio-fx-rust, share-rust, queue-rust, vc-rust, spotify-rust).
- netease-go: `go vet ./... && go build ./...` OK.
- downloaders: `python3 -m py_compile src/*.py` OK.
- Node: `node --check` OK for vc-ts (3 files), radio-js (2), gui-js (1).
- `contracts/ipc.json` valid JSON, 35 actions.

### Combined live smoke — 11 services together, then stopped, no strays left
Test ports 18001–18011 + 13000, token `p3-test`. Healthz: **11/11 pass**.
(Note: one stray vc-go `p2-vcbin` pid 28709 from a worker's earlier test was holding 18001; killed by pid before the run. My first smoke-script start of netease-go failed on my own path mistake — binary path collided with its data dir; rebuilt as `netease-bin`, healthz then passed. No worker code changed.)

| Check | Result |
|---|---|
| vc-go :18001 play (YT URL, degraded resolve `yt:dQw4w9WgXcQ`) | ok, `engine:"go"` |
| vc-go now-playing without token → 401; with token → ok, `position_sec` advancing | ok |
| vc-ts :18002 play (YT URL) | ok, **live yt-dlp resolve**: "Rick Astley - Never Gonna Give You Up (Official Video) (4K Remaster)", 213s |
| vc-ts queue list | ok, `[]` (playing, nothing queued) |
| vc-rust :18003 play "Test Song" | ok, `engine:"rust"`, `resolved_by:"pattern"`, `voice:"stub-logging-only"` |
| vc-rust queue list | ok |
| audio-fx :18004 nightcore on generated 2s 440 Hz tone | ok, real 219,608-byte file |
| share-rust :18005 share create → `https://t.me/ParlayBot?start=share_SeBl6grYWWuE8vRP`; resolve with token → exact track; without token → 401 | ok |
| queue-rust :18006 vote approve | ok, `queued:false, approvals:1, threshold:3` |
| spotify-rust :18007 skip-check "Imagine" | ok, `skip:false`, real LRCLib lyrics (`lyrics_checked:true`) |
| spotify-rust skip-check "Fuck The Police" | ok, `skip:true, reason:"profanity", matched_word:"fuck"` |
| radio-js :18008 stations | ok, 6 stations, 5 reachable, KEXP `reachable:false` |
| radio-js start Radio Paradise | ok, ffprobe-confirmed `aac`, `fallback_playlist:false` |
| radio-js stop | ok |
| netease-go :18010 search "Jay Chou" | ok, **10 real results** (210049 布拉格广场 – 蔡依林, 周杰伦, 294s, …) |
| netease-go lyrics 210049 | ok, real timed lyrics from `[00:00.00] 作词 : 方文山` |
| gui-js :13000 `/` | ok, HTML player page, 29 matches for play/queue/now controls |
| gui-js `/api/now-playing` (no CORE_URL) | ok, labelled `local-demo` state with honest note |
| downloaders :18011 search "test" youtube limit 2 | ok, 2 real YouTube results (no regression from Phase 2) |
| core-cpp binary, no env | exits with `missing required env vars: BOT_TOKEN PARLAY_TOKEN` (no network attempted) |

Worker test totals (their own runs, re-verified in substance above): vc-ts 20/20, radio-js 8/8, vc-rust 24/24, spotify-rust 24/24, gui-js 9/9, netease-go search+lyrics live / download blocked (see above).

## Still stubbed (whole project)

- **Voice joining / audio into a voice chat — everywhere.** core-cpp `VoiceCall` interface + logging stub (TDLib/ntgcalls unbuildable in sandbox); vc-ts `src/voicecall.js` stub; vc-rust `VoiceCall` trait with `LoggingVoiceCall` only; vc-go is a queue/playback-state engine by design. Playback state (queue/position/auto-advance) is real; no audio reaches a voice chat anywhere. Needs a host that can build TDLib/ntgcalls.
- **MongoDB/Redis** — env read, not connected; JSON-file stores used instead (queues, shares, votes, watches, dislikes).
- **Netease download** — `engine_unavailable` (v1 URL needs encrypted weapi payload; non-v1 returns `url:null`). Search + lyrics are live.
- **Lemonic** — keyed path untested, returns `engine_unavailable` without `LEMONIC_API_KEY`; never fakes results.
- **Spotify** — playback monitoring needs `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` + per-user OAuth (watch registry is intent-only); Genius/Musixmatch lyrics need their keys (only keyless LRCLib wired); no AI-lyrics classifier, so `reason:"ai-generated"` is never emitted. Spotify playlist track-list resolution still needs creds or spotdl (Phase 2 finding, unchanged).
- **Downloaders** — deezer/tiktok/navidrome routes return `engine_unavailable` (Phase 2).
- **gui-js** — no Next.js/wavetune React components or album/artist browse; no in-browser audio playback (no stream URL yet); volume slider display-only; demo state in-memory; no GUI-side auth.
- **vc-ts** — JioSaavn results have `duration_sec: 0` (upstream payload has none) → no auto-advance for those tracks; lyrics + live control panel (remix) not built (no contract action). Dockerfiles added for vc-ts/radio-js, not container-tested.
- **vc-rust** — no download path; no loop/shuffle/autoplay. yt-dlp not installed in sandbox, so live resolve is pattern-only there (subprocess path implemented, activates if installed).
- **No live Telegram test yet** — needs a real BOT_TOKEN + a VC engine up, then `/play` in a group and killing vc-go mid-play to prove fail-over to vc-ts/vc-rust. No live voice streaming verified anywhere.
- No push to GitHub yet — awaiting the user's repo URL.

## Next steps

1. Live Telegram test with a real bot token; fail-over drill (kill vc-go mid-play → vc-ts/vc-rust takes over).
2. Voice-call binding on a host that can build TDLib/ntgcalls.
3. Netease download via weapi-encrypted requests, or accept search+lyrics only.
4. Push to the user's repo once they share the URL.
