# gui-js — PROGRESS (Phase 3, 2026-11-02)

Worker C. Plain Node stdlib server, no npm deps, no Next.js build.

## Files changed

- `services/gui-js/app/main.js` — rewritten from the 1-line stub into the
  full server (~470 lines): player page + `/healthz` + `/api/*`.
- `services/gui-js/README.md` — rewritten: implemented vs stubbed.
- `services/gui-js/PROGRESS.md` — this file.
- `services/gui-js/package.json` — unchanged (`start: node app/main.js`).

## Endpoints

| Route | Status |
|---|---|
| `GET /` | Player page: now-playing, queue, search box, play/pause/skip, volume slider, guarded `Telegram.WebApp.ready()/expand()` hook, Mini App entry is `/` |
| `GET /healthz` | `{ok:true,service:"gui-js"}` |
| `GET /api/now-playing?chat_id=` | Proxy `CORE_URL`, else labelled local-demo state |
| `GET /api/queue?chat_id=` | Proxy `CORE_URL`, else labelled local-demo state |
| `GET /api/search?q=` | Proxy `CORE_URL`, else `NETEASE_URL`, else `engine_unavailable` |
| `POST /api/play`, `/api/pause`, `/api/skip` | Proxy `VC_GO_URL` → `VC_TS_URL`; no engine → local demo state + honest `note` |

## Exact verification (2026-11-02 ~19:59 IST)

Commands (from `~/workspace/parlay/services/gui-js`):

```
node --check app/main.js
PORT=13000 node app/main.js
curl http://127.0.0.1:13000/                       # HTML, controls present
curl http://127.0.0.1:13000/healthz
curl "http://127.0.0.1:13000/api/now-playing?chat_id=-100123"
curl "http://127.0.0.1:13000/api/queue?chat_id=-100123"
curl -X POST http://127.0.0.1:13000/api/play  -d '{"chat_id":-100123,"query":"Demo Song"}'
curl -X POST http://127.0.0.1:13000/api/pause -d '{"chat_id":-100123}'
curl -X POST http://127.0.0.1:13000/api/skip  -d '{"chat_id":-100123}'
curl "http://127.0.0.1:13000/api/search?q=Jay%20Chou"
curl -o /dev/null -w "%{http_code}" http://127.0.0.1:13000/nope
```

Results — **9/9 pass**:

1. `GET /` — HTML contains `Parlay Player`, `btnPlay`/`btnPause`/`btnSkip`,
   `id="q"`, `id="queue"`, `id="vol"`, `telegram-web-app` script and 3
   `Telegram.WebApp` references. PASS.
2. `/healthz` → `{"ok":true,"service":"gui-js"}`. PASS.
3. `/api/now-playing` → `ok:true`, demo track `demo-1` "Demo Track — Raag
   Bhairavi (local demo)", `source:"local-demo"`, with the note that the
   core is unreachable. PASS.
4. `/api/queue` → `ok:true`, 2 demo items, `source:"local-demo"`. PASS.
5. `POST /api/play` → `ok:true`, `paused:false`, new demo track "Demo
   Song", note "no VC engine configured/reachable … applied to local demo
   state only". PASS.
6. `POST /api/pause` → `paused:true`. PASS.
7. `POST /api/skip` → advanced to demo-1, `paused:false`. PASS.
8. `/api/search` with no backend → `ok:false`,
   `engine_unavailable: "no search backend configured (set CORE_URL or
   NETEASE_URL)"`. PASS (honest, nothing fake).
9. Unknown route → HTTP 404 envelope. PASS.

Proxy-path verification (same day):

```
PORT=18001 /tmp/parlay-vc            # NOTE: bind failed — a pre-existing
                                     # vc-go (pid 28709, "p2-vcbin", another
                                     # worker's) already held 18001; the
                                     # proxy test below used THAT instance
PORT=13001 VC_GO_URL=http://127.0.0.1:18001 node app/main.js
curl -X POST http://127.0.0.1:13001/api/play -d '{"chat_id":-100123,"query":"test song"}'
# → {"ok":true,"data":{"now_playing":{"chat_id":-100123,"track":{"id":"query:e6273e1234eaf23c","title":"test song","source":"youtube"},"paused":false,...,"engine":"go"},"engine":"go"}}
PORT=13002 NETEASE_URL=http://127.0.0.1:18010 PARLAY_TOKEN=test-token-123 node app/main.js
curl "http://127.0.0.1:13002/api/search?q=Jay%20Chou"
# → {"ok":true,"data":{"results":[...10 real NetEase tracks...],"source":"netease"}}
```

Both proxies PASS. All test servers killed afterwards (ports 13000/13001/
13002 confirmed free). The pre-existing vc-go on 18001 was left alone.

## Still stubbed

> **SUPERSEDED in part (Phase 4, 2026-11-02).** The in-browser-audio
> bullet below is Phase 3 state — real in-browser playback shipped in
> Phase 4 (see the "Phase 4" section below: `/api/stream-url` +
> `/api/audio` proxy + WebAudio gain volume). Still true: no Next.js
> build / wavetune React components / album & artist browse; demo
> state is in-memory only, no Mongo/Redis, no GUI-side auth.

- No Next.js build / wavetune React components / album & artist browse.
- No in-browser audio playback (no stream URL exposed yet); controls are
  remote controls for the VC engine; volume slider is display-only.
- Demo state is in-memory only; no Mongo/Redis; no GUI-side auth.

## Phase 4 — Worker C (2026-11-02)

- **Real in-browser playback**: `<audio id="player">` fed by
  `GET /api/stream-url?chat_id=` (resolves the current track's direct
  audio URL from VC_GO_URL → VC_TS_URL → CORE_URL; playable only for
  `stream_url` / radio / file URLs — YouTube watch pages honestly
  report `playable:false`). "Listen in browser" toggle; on `ended`
  reloads the stream URL for whatever is now playing (never skips
  the VC). Seek bar + time display (wavetune Player.jsx shape).
- **WebAudio volume that changes gain**: `AudioContext` +
  `createMediaElementSource` → `GainNode` → destination; slider sets
  `gainNode.gain.value = v/100` (localStorage-persisted), `audio.volume`
  fallback when WebAudio is unavailable; mode shown in the page.
  The audio is loaded through the same-origin `GET /api/audio?src=`
  proxy (Range passthrough) because cross-origin media through a
  media-element source is silenced without CORS headers.
- **Tests**: `tests/run.js`, `npm test` → **7/7 pass** (page wiring;
  live stream-url for a JioSaavn track; proxy byte-exact full +
  Range 206 against a real ffmpeg MP3 stub; honest errors).
- Sandbox limit: `aac.saavncdn.com` 404s every fresh media URL
  through the egress proxy, so Saavn bytes are verified via the stub
  proxy, not live bytes; on a normal network the same path serves
  the Saavn stream. Benchmarks: `phase4/bench-tsjs.md` (page 2–11 ms,
  stream-url ~0.8 s, proxy 15–176 ms for a 5 KB tone).
