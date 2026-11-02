# radio-js — progress

Worker A — Node services, Phase 3. 2026-11-02 ~19:20–20:00 IST.

## Toolchain

- Node v24.22.1 at /usr/bin/node, ffmpeg 8.1.2 / ffprobe at /usr/bin.
- Stdlib only: **no npm install, no new dependencies**. `package.json`
  unchanged (`"start": "node src/main.js"`).
- Note: upstream openradiobot has **no curated station list** (its
  radio queue is user-supplied); the 6-station list in
  `src/stations.js` is original to Parlay.

## Files written (all under services/radio-js/)

- `src/main.js` — replaced stub. HTTP server, ffprobe stream probing
  (10 s timeout, startup parallel probe + re-probe on start), per-chat
  radio state, rotating fallback playlist (180 s ticker), JSON-file
  persistence (atomic tmp+rename, `data/state.json`), all routes,
  token auth, contract envelopes.
- `src/stations.js` — 6 curated stations + per-station fallback playlists.
- `Dockerfile` — node:24-slim + ffmpeg (compose `build:` target; no
  Dockerfile existed anywhere in the tree before this).
- `README.md` — rewritten: real vs stubbed, endpoints, stations.
- `PROGRESS.md` — this file.

## Verification (exact commands, all run 2026-11-02)

```sh
node --check src/main.js src/stations.js   # OK
PARLAY_DATA_DIR=/tmp/radio-test2/data PARLAY_TOKEN=testtoken123 PORT=18008 node src/main.js
```

Startup log: `[radio-js] startup probe: groovesalad=ok lush=ok
radioparadise=ok fip=ok bbcws=ok kexp=unreachable` — 5/6 reachable.
(An earlier probe run had radioparadise + fip unreachable too: sandbox
egress is flaky per-host; the service reports whatever ffprobe finds,
honestly, each run.)

`GET /healthz` (no token) →
`{"ok":true,"service":"radio-js","ffprobe":"/usr/bin/ffprobe","stations":6,"active_radios":0}`

- `GET /v1/radio/stations` → 6 stations with id/name/genre/stream_url/
  reachable/format: groovesalad, lush, radioparadise, fip, bbcws
  `reachable:true`; kexp `reachable:false`.
- `POST /v1/radio/start {"chat_id":-100123,"station":"groovesalad"}` →
  `{station:"groovesalad", station_name:"SomaFM Groove Salad",
  stream_url:"https://ice1.somafm.com/groovesalad-128-mp3",
  reachable:true, now_playing:{track:{title:"SomaFM Groove Salad —
  Groove Salad — ambient beats mix", source:"radio"}}}` — live ffprobe
  probe on start confirmed the stream (`format_name: mp3`).
- `POST /v1/radio/start {"chat_id":-100124,"station":"kexp"}` →
  `reachable:false, fallback_playlist:true` — unreachable station
  still starts, in playlist-fallback mode, and says so.
- `POST /v1/radio/start` with unknown station → `bad_request
  "unknown station \"nosuch\"; see GET /v1/radio/stations"`.
- `POST /v1/radio {"action":"stop","chat_id":-100124}` → `{"ok":true,"data":{}}`.
- `POST /v1/radio/stop {"chat_id":-100123}` → `{"ok":true,"data":{}}`.
- `POST /v1/radio/start` with no token → `unauthorized` (401).

## Test tally

- radio-js: 8 checks, 8 pass (healthz, stations list + probe values,
  start reachable, start unreachable/fallback, bad station, generic
  radio stop, dedicated stop, unauth 401).

## Still stubbed / untested

- No audio is relayed to listeners or into a voice call (the VC
  engine's voice path is itself stubbed; nothing to pipe into yet).
- No HTTP listener endpoint (`/<chat-id>` audio/mp3 in upstream) —
  no contract action for it.
- Fallback playlist entries are Parlay's own labels, not live ICY
  now-playing metadata from the stream.
- KEXP was unreachable from the sandbox in every probe run (may be
  reachable from a normal host; the probe reports per-run reality).

## Phase 4 — Worker C (2026-11-02)

- **VC handoff implemented** (contract `vc.radio` existed, code did
  not): on start, `POST /v1/play` with the stream URL to VC_GO_URL
  then VC_TS_URL; vc-ts takes it as a `radio` track directly, vc-go
  via yt-dlp generic extractor (verified: direct stream, ~2 s).
  Reported as `vc_handoff: {engine, ok, now_playing?|error}`; a failed
  handoff never fails the start. On stop, the engine room is cleared
  only if `GET /v1/now-playing` still shows the radio track.
  Restored radios are not re-handed-off (`vc_handoff: null`).
- Env: `VC_GO_URL`, `VC_TS_URL` (unset = no handoff, honest).
  Healthz reports both. `stop` responses carry `{vc_cleared}`.
- **Tests**: `tests/run.js`, `npm test` → **7/7 pass** (live vc-ts:
  start→engine plays radio; stop→engine cleared; stop after
  skip-to-song→song survives; no-VC mode; errors).
- Benchmarks: `phase4/bench-tsjs.md` (start 0.9–1.8 s incl. ffprobe
  probe + handoff; stop 181 ms with VC clear).
