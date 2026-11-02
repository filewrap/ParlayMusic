# radio-js — internet radio as a live source

Ported behaviour (re-implemented in plain Node, stdlib only — no npm
dependencies) from Yonle/openradiobot (`index.js`, single file: openradio
+ ytdl-core). A chat owns a radio; the radio plays a queue of streams;
listeners attach to the live output. This service is pure IPC (the core
owns Telegram), and — unlike upstream, which has **no station list**
(its queue is user-supplied) — it ships a curated list of public live
streams in `src/stations.js` so `radio.start` works out of the box.

## Run

```sh
PORT=8008 PARLAY_TOKEN=<token> node src/main.js
# env: PORT (default 8008), PARLAY_TOKEN (optional; when set, every
# route except /healthz requires header X-Parlay-Token),
# PARLAY_DATA_DIR (default ./data)
# VC_GO_URL / VC_TS_URL — VC engines the live stream is handed to on
# start (contract vc.radio, "radio-js -> vc-*"). Unset = no handoff;
# the radio still starts and reports vc_handoff honestly.
# Tests: npm test  (node --test tests/run.js; 7 tests, live vc-ts)
```

## Endpoints

| Method | Route | Notes |
|---|---|---|
| GET | `/healthz` | `{ok:true, service:"radio-js", ...}` — public, no token |
| GET | `/v1/radio/stations` | Curated list: `{id, name, genre, stream_url, reachable, format}` |
| POST | `/v1/radio/start` | `{chat_id, station}` (id or name) → `{stream_url, now_playing, reachable, vc_handoff, ...}`; re-probes the stream on start, then hands it to a VC engine (see below) |
| POST | `/v1/radio` | `{action: start\|stop, chat_id, station?}` — contract `vc.radio` shape; stop returns `{vc_cleared}` |
| POST | `/v1/radio/stop` | `{chat_id}` → `{vc_cleared}` |
| GET | `/v1/radio/status?chat_id=` | Extra: current radio + now-playing + `vc_handoff` for a chat |

All responses are `{ok:true, data}` or `{ok:false, error:{code, message}}`.
Unknown station → `bad_request`; no active radio → `not_found`;
missing/wrong token → `unauthorized`.

## Stations (curated, 6)

SomaFM Groove Salad, SomaFM Lush, Radio Paradise (Main Mix), FIP Paris,
BBC World Service, KEXP 90.3 FM Seattle — see `src/stations.js` for
stream URLs. Reachability is **probed, never assumed**: at startup all
six are probed in parallel with ffprobe (10 s timeout) and on every
start; the result is reported as `reachable:true/false` with the probed
`format`.

## What is real (verified 2026-11-02, see PROGRESS.md)

- Station list + per-station ffprobe probe: 5/6 reachable from the
  sandbox in the final run (KEXP unreachable — reported honestly;
  Radio Paradise and FIP were unreachable in an earlier probe run,
  reachable in the later one: sandbox egress is flaky per-host).
- `radio.start` returns the real `stream_url` + `now_playing` panel
  state; starting an unreachable station still succeeds but flags
  `fallback_playlist: true`.
- **Queue-based fallback**: while a station is active, a rotating
  playlist (from `src/stations.js`, entries labelled as fallback
  playlist entries — not a claim about the live broadcast) advances
  every 180 s in now-playing state, mirroring openradiobot's
  queue-keeps-playing loop.
- Active radios persist to `data/state.json` (atomic write) and are
  restored on restart. Restored radios are **not** re-handed-off to
  a VC engine (the engine keeps its own persisted room; re-playing
  could double-queue a stream) — `vc_handoff` stays null until a
  fresh start.
- Token auth (401 without token), unknown-station `bad_request`,
  stop-via-both-routes — all verified by curl.
- **VC handoff is real (Phase 4).** On start, the live stream URL is
  handed to a VC engine via `POST /v1/play` — vc-go first, then
  vc-ts (same fallback order as gui-js) — so the radio actually
  sounds in the group's voice chat. That is contract action
  `vc.radio` ("radio-js -> vc-*"), which existed in the contract but
  had no implementation: before Phase 4 radio-js only kept its own
  state. vc-ts takes a raw URL as a `radio` track directly
  (duration 0, plays until stopped); vc-go takes it through yt-dlp's
  generic extractor (verified: direct stream, no duration, ~2 s).
  The result is reported as `vc_handoff: {engine, ok, now_playing? |
  error}`; a failed handoff never fails the start. On stop, the
  engine's room is cleared **only if it is still on the radio
  track** (checked via `GET /v1/now-playing` first) — a user's later
  music is never killed. Verified end to end by the test suite
  (`npm test`, 7 tests): start → vc-ts now-playing is the radio
  stream; stop → vc-ts room stopped; stop-after-skip-to-a-song →
  song keeps playing, `vc_cleared=false`; no VC configured → radio
  still starts with an honest `vc_handoff.error`.

## What is stubbed / not done, and why

- **No audio is actually relayed by this service itself.**
  openradiobot pipes the stream through its `openradio` player to
  HTTP listeners; this service hands the `stream_url` to the VC
  engine, which owns the voice call (vc-ts/vc-go voice is itself
  stubbed in the sandbox — see vc-ts README), so there is nothing to
  pipe into yet. The handoff above is the implemented half: the
  stream reaches the engine's room as a live source.
- **No listener endpoint.** openradiobot serves listeners over HTTP
  (`/<chat-id>` → audio/mp3). No IPC action for that exists in
  `contracts/ipc.json`; if the core wants it, add `radio.listen`
  to the contract first.
- The fallback playlist entries are Parlay's own labels (e.g.
  "Groove Salad — ambient beats mix"), not live now-playing metadata
  from the stream. Real ICY metadata parsing is a future step.
- Plain Node, not the upstream `openradio`/`ytdl-core`/`slimbot`
  stack — the port is behavioural.

## Files

- `src/main.js` — HTTP server, probing, per-chat radio state, VC handoff, persistence.
- `src/stations.js` — curated station list + fallback playlists.
- `tests/run.js` — 7 tests (`npm test`): handoff, stop guard, no-VC mode, errors.
- `Dockerfile` — node:24-slim + ffmpeg (ffprobe) (compose `build:` target).
- `PROGRESS.md` — build + verification log.
