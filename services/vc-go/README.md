# vc-go — Go VC streaming engine

From: YukkiMusic-Go (`internal/core/queue.go`, `room.go`) + ArcMusic
(high-perf: `internal/core/room_queue.go`) + TgMusicBot (low-RAM:
`internal/bot/{play,queue,seek,skip}.go`). Behaviour ported, code written
fresh — no upstream code vendored (see `PORT_MAP.md` license note).

One service, two profiles via `VC_PROFILE`:

- `low-ram` (default, TgMusicBot tuning): single runner goroutine,
  sequential queue, no prefetch, 256 KB buffer.
- `high-perf` (ArcMusic tuning): 2 prefetch workers resolve the next
  track's direct stream URL while the current track plays, 4096 KB buffer.

Implements `contracts/ipc.json` actions: `vc.play/pause/resume/skip/
stop/queue/seek/loop/shuffle/autoplay/autoplay-skip/now-playing`, plus
`GET /healthz`.

## What is implemented (real)

- HTTP IPC server on `$PORT` (default 8001), envelopes
  `{ok:true,data}` / `{ok:false,error:{code,message}}` per contract.
  `X-Parlay-Token` auth when `PARLAY_TOKEN` is set; empty = allow all
  with a startup warning.
- Per-chat queue state: play (starts immediately when idle, enqueues
  otherwise), add/remove/clear/list, skip, stop, pause/resume, seek
  (clamped to duration), volume, wall-clock position tracking,
  auto-advance when a track's known duration elapses. All guarded by
  `sync.Mutex`.
- **Loop** (`POST /v1/loop {chat_id, count 0-10}`): replays the current
  track `count` times, counter decremented per replay; `/skip` resets
  it. **Shuffle** (`POST /v1/shuffle {chat_id, enabled}`): the pop from
  the waiting queue is random; the current track is untouched and the
  stored queue order is never reordered. **Autoplay**
  (`POST /v1/autoplay {chat_id, enabled}`): on drain, picks the next
  track from the YouTube mix playlist `RD<video-id>` via yt-dlp
  (`resolver.Related`), skipping the last track, the queued tracks and
  the autoplay history (capped at 50; history cleared and playback
  stops at the cap, TgMusicBot behaviour). Only YouTube tracks autoplay.
  `POST /v1/autoplay-skip` advances like skip (loop reset).
  **Stream URL** (`Track.stream_url`, A8): the resolver runs yt-dlp
  with `-f bestaudio/best`, so the direct audio URL lands in every
  resolved track — `GET /v1/now-playing` and queue responses carry it
  (omitted when a track has none, e.g. an autoplay mix pick, which the
  core resolves on demand). `NowPlaying` now reports `loop`, `shuffle`,
  `autoplay`.
- YouTube resolve via `yt-dlp -j -f bestaudio/best --no-playlist`
  (exec, argv only, 30 s timeout). Search strings use `ytsearch1:`,
  URLs are passed through. yt-dlp is discovered at startup (PATH,
  `$YT_DLP`, then the downloaders service venv); a YouTube bot check
  is retried once with the android player client. No degraded
  fallback: a query yt-dlp cannot resolve is an error, served as 502
  `engine_unavailable` so the core can fail over — never a fake track.
  `-f bestaudio/best` matters: without a format selection yt-dlp's
  `-j` output has no top-level `url` for multi-format videos, so
  `stream_url` would stay empty.
- Both profile paths are real in the queue runner: low-ram resolves on
  demand only; high-perf runs 2 prefetch workers + a 1 s advance ticker
  (verified: a 2 s track auto-advanced the room to idle).
- Persistence — **MongoDB when `MONGO_URI` is set** (official driver
  `go.mongodb.org/mongo-driver/v2` v2.3.0, database `parlay`,
  collection `state`, one document per room, `_id` =
  `kv:vc:room:<chat_id>`, `v` = canonical room JSON shared with
  vc-ts/vc-rust; upsert on save, stale chats pruned). **Redis when
  only `REDIS_URL` is set** (`parlay:kv:vc:room:<chat_id>`, std-only
  RESP client). A set-but-unreachable `MONGO_URI`/`REDIS_URL` is
  fatal at startup — the service never silently falls back.
  **JSON file when neither is set**
  (`$PARLAY_DATA_DIR/queues.json`, atomic write on every mutation).
  Both backends save loop/shuffle/autoplay/history; rooms restore
  **paused** (never auto-resume audio after a restart). Verified live
  across restarts on both backends.
- Tests: 30 (`go test ./...`): 14 queue (loop/shuffle/autoplay/
  skip-reset/stop-reset/persist), 6 resolver (venv discovery,
  honest-error resolve, 2 live-gated: YouTube resolve + mix pick),
  3 server (502 without yt-dlp, healthz degraded flag, live-gated
  stream_url end-to-end), 5 types (stream_url serialization),
  2 store (JSON round-trip + live Mongo round-trip, incl. prune).
  `go vet` clean. Live-gated tests need `PARLAY_LIVE_TEST=1`
  (network + yt-dlp); without it they skip (27 pass + 3 skip).
- Bench (see `phase4/bench-go.md`): clean build 92 s, stripped binary
  9.3 MB, startup-to-healthz 0.03 s.

## What is still stubbed, and why

- **Voice joining / audio output (ntgcalls).** This service never opens
  a Telegram voice chat and never feeds audio frames. That is the C++
  core's job via its `VoiceCall` interface; vc-go is the queue /
  playback-state engine. The core asks this service what is playing
  (`/v1/now-playing`) and which stream URL to feed (`Track.stream_url`,
  serialized on every resolved track).
- **yt-dlp missing.** If no yt-dlp binary is found at startup
  (`/healthz` reports `degraded:true, yt_dlp:false`), resolve returns
  `engine_unavailable` (HTTP 502) — there is no pattern fallback any
  more. Set `YT_DLP` or install yt-dlp (the downloaders venv is probed
  automatically). Autoplay on drain with no yt-dlp simply idles the
  room.

## Run

```sh
# Go 1.23 lives at ~/workspace/parlay/.tools/go (installed 2026-11-02)
export PATH=$HOME/workspace/parlay/.tools/go/bin:$PATH
cd services/vc-go
go build ./...            # needs module cache / proxy for the mongo driver
PORT=8001 PARLAY_TOKEN=secret VC_PROFILE=high-perf \
  MONGO_URI=mongodb://127.0.0.1:27017 \
  PARLAY_DATA_DIR=./data ./parlay-vc   # build with: go build -o parlay-vc ./cmd/parlay-vc
```

A local mongod is available at `~/workspace/parlay/.tools/mongodb/bin/mongod`
with data in `~/workspace/parlay/phase4/mongo-data` (start:
`mongod --dbpath phase4/mongo-data --bind_ip 127.0.0.1 --port 27017
--fork --logpath phase4/mongod.log --logappend`).

## Endpoints

| Method | Path | Body / query |
|---|---|---|
| GET | `/healthz` | — (unauthenticated; includes `profile`, `yt_dlp`, `degraded`, `buffer_kb`, `prefetch`) |
| POST | `/v1/play` | `{chat_id, query, requested_by, volume?}` |
| POST | `/v1/pause`, `/v1/resume`, `/v1/skip`, `/v1/stop` | `{chat_id}` |
| POST | `/v1/queue` | `{action: add\|remove\|clear\|list, chat_id, position?, track?, requested_by?}` |
| POST | `/v1/seek` | `{chat_id, position_sec}` |
| POST | `/v1/loop` | `{chat_id, count 0-10}` (400 outside range) |
| POST | `/v1/shuffle`, `/v1/autoplay` | `{chat_id, enabled}` |
| POST | `/v1/autoplay-skip` | `{chat_id}` |
| GET | `/v1/now-playing?chat_id=` | — (includes `loop`, `shuffle`, `autoplay`) |
