# vc-go — progress

Worker B — Go VC engine service. 2026-11-02 ~19:20 IST.

## Go toolchain

- Go was NOT installed. Installed Go 1.23.0 linux-amd64 from go.dev
  (first `curl` without `-L` returned only a 302 redirect body; retry
  with `-L` downloaded the real 73.6 MB tarball) into
  `~/workspace/parlay/.tools/go`; `go version` → `go1.23.0 linux/amd64`.
- `yt-dlp` is NOT present in the sandbox (`which yt-dlp` → nothing);
  `ffmpeg` is present at `/usr/bin/ffmpeg`.

## Files written (all under services/vc-go/)

- `cmd/parlay-vc/main.go` — wiring: env, store restore, runner, HTTP
  server, graceful shutdown + final persist. (Replaced the stub.)
- `internal/types/types.go` — Track / QueueItem / NowPlaying / Envelope
  per contracts/ipc.json.
- `internal/queue/room.go` — per-chat Room + Manager, mutex-guarded;
  play/enqueue/remove/clear/pause/resume/seek/skip/stop, wall-clock
  position, persist/restore (restored rooms come back paused).
- `internal/queue/room_test.go` — 6 unit tests.
- `internal/resolver/resolver.go` — yt-dlp `-j` resolve (30 s timeout,
  argv exec, no shell); degraded URL-pattern fallback when yt-dlp is
  missing or fails.
- `internal/runner/runner.go` — 1 s advance ticker + prefetch workers;
  low-ram (0 prefetchers, 256 KB buffer) vs high-perf (2 prefetchers,
  4096 KB buffer).
- `internal/server/server.go` — all 8 endpoints + auth + envelopes.
- `internal/store/store.go` — JSON-file queue store, atomic writes.
- `README.md` — rewritten: real vs stubbed, endpoints, run instructions.
- `go.mod` — unchanged: `module parlay/vc-go`, `go 1.23`, stdlib only.

## Verification (exact commands, all run 2026-11-02 ~19:35 IST)

```sh
export PATH=$HOME/workspace/parlay/.tools/go/bin:$PATH
cd ~/workspace/parlay/services/vc-go
go vet ./... && go build ./...        # OK (one vet fix: Room.Seek renamed SeekTo, io.Seeker clash)
go test ./...                          # ok parlay/vc-go/internal/queue 0.004s (6 tests)
go build -o /tmp/vcgo-smoke/parlay-vc ./cmd/parlay-vc
```

Smoke test 1 — low-ram, no token, PORT 8001, `PARLAY_DATA_DIR=/tmp/vcgo-smoke/data`:

- `GET /healthz` →
  `{"ok":true,"data":{"buffer_kb":256,"degraded":true,"engine":"go","ok":true,"prefetch":false,"profile":"low-ram","service":"vc-go","yt_dlp":false}}`
- `POST /v1/play {"chat_id":-100123,"query":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","requested_by":42}` →
  now_playing with `track.id "yt:dQw4w9WgXcQ"`, `paused:false`, `position_sec:0`, `volume:100` (degraded resolve).
- Second play (search string) enqueued at `position:0`,
  `track.id "query:d0f8d96ef806c440"`.
- `GET /v1/now-playing?chat_id=-100123` → current + 1 queued item.
- `POST /v1/queue {"action":"list"}` → the queued item.
- pause → `paused:true`; seek 30 → `position_sec:30` (paused);
  resume → `paused:false, position_sec:30`; skip → now playing the
  queued track; stop → `{"ok":true,"data":{}}`; now-playing after stop →
  `{"ok":false,"error":{"code":"not_found","message":"no playback state for this chat"}}`.
- `POST /v1/play {"query":"x"}` (no chat_id) →
  `{"ok":false,"error":{"code":"bad_request","message":"chat_id is required"}}`.
- `queues.json` on disk after stop: `[]` (idle rooms are not persisted).

Smoke test 2 — high-perf + auth, PORT 8002, `PARLAY_TOKEN=s3cret`:

- `GET /healthz` → `profile:"high-perf", buffer_kb:4096, prefetch:true, degraded:true`.
- Play without token → `401 {"code":"unauthorized","message":"bad or missing X-Parlay-Token"}`.
- Play with `X-Parlay-Token: s3cret` → 200, now_playing.
- `queues.json` before kill contained the room (`chat_id:-55`,
  current track, `paused:false`).
- After kill + restart: `GET /v1/now-playing?chat_id=-55` →
  same track, `paused:true` (restored, never auto-resumes); server log:
  `store: restored 1 room(s) from /tmp/vcgo-smoke2/data/queues.json (paused)`.

Smoke test 3 — runner auto-advance, PORT 8003:

- Queued a track with `duration_sec:2` via `/v1/queue add`, skipped to
  it, waited 3.5 s → `GET /v1/now-playing` → `not_found` (room idle);
  server log: `vc-go: chat -77 queue drained, idle`.

## Real vs stubbed

- REAL: HTTP IPC + auth + envelopes; per-chat queue/playback state
  (thread-safe); both profile paths in the runner (prefetch workers
  exist and are wired — they only resolve when yt-dlp is present);
  auto-advance ticker; JSON persistence + restore; degraded resolver.
- STUBBED / out of scope by design: ntgcalls voice joining and audio
  output (core's VoiceCall job); MongoDB persistence (JSON file used,
  swap point is `internal/store`); yt-dlp itself (absent in sandbox —
  install it and the resolver + prefetch go live with no code change).
- NOT VERIFIED live: yt-dlp resolution path and high-perf prefetch
  (both need yt-dlp installed); actual voice streaming (needs the core).
