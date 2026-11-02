# gui-js — web GUI + Telegram Mini App player

From: 4nuxd/wavetune (Next.js, `app/(player)/`, `app/(root)/page.js`,
`components/music-provider.jsx`). Now-playing, queue, search, controls.
Now-playing/queue proxy the core (`/api/*` in contracts/ipc.json);
play/pause/skip/search talk directly to the VC engines (`VC_GO_URL` /
`VC_TS_URL`, `NETEASE_URL` as a search fallback) — the old "talks ONLY
to the core, never directly to voice" line was false (see
`app/main.js`).

## What is actually implemented (Phase 3, 2026-11-02)

Plain Node.js **stdlib-only** HTTP server (`app/main.js`, no npm deps, no
Next.js build — the task brief explicitly forbids trying to build Next.js
here). Single-page player at `GET /`:

- Now-playing card (title, artist, duration, paused/playing, volume slider)
- Queue list, search box (results clickable → play), Play / Pause / Skip
- The page calls the service's own `/api/*` endpoints every 15 s — it
  never hardcodes track data.
- Telegram WebApp hook: loads `telegram-web-app.js` and calls
  `Telegram.WebApp.ready()` / `.expand()` **guarded**, so the page also
  works in a plain browser. Mini App URL is simply `/` (optionally
  `/?chat_id=<id>`).

Endpoints (all `/api/*` use the Parlay envelope
`{ok:true,data}` / `{ok:false,error:{code,message}}`):

| Route | Behaviour |
|---|---|
| `GET /healthz` | `{ok:true, service:"gui-js"}` (top-level, per contract) |
| `GET /api/now-playing?chat_id=` | Proxies `CORE_URL` if set and reachable, else local demo state labelled `source:"local-demo"` |
| `GET /api/queue?chat_id=` | Same as above |
| `GET /api/search?q=` | Proxies `CORE_URL` (`POST /v1/search`), else `NETEASE_URL` (`POST /v1/netease/search`), else `engine_unavailable` |
| `GET /api/stream-url?chat_id=` | **(Phase 4)** Direct audio URL for the current track, from the VC engines (`VC_GO_URL` then `VC_TS_URL`, then `CORE_URL`): `{playable, url, direct_url, source, track}`. Playable only when the track carries a real media URL (`stream_url`, or a radio/file `url`). YouTube watch pages are not audio — `{playable:false, reason}` says so honestly. `url` is the same-origin `/api/audio?src=…` proxy URL. |
| `GET /api/audio?src=<url>` | **(Phase 4)** Same-origin proxy for that audio URL (Range passthrough, 206). This is what makes WebAudio gain work: media routed through a `MediaElementAudioSourceNode` is silenced when cross-origin without CORS headers, so the player loads the proxy instead of the CDN directly. Bad `src` → 400; dead upstream → 502 `download_failed`. |
| `POST /api/play`, `/api/pause`, `/api/skip` | Proxies `VC_GO_URL`, then `VC_TS_URL` (fallback order per ARCHITECTURE.md). If no engine is reachable, applies the action to the local demo state and says so in `data.note` — never claims a real engine acted |

## In-browser playback (Phase 4 — the stub this phase killed)

Same core as wavetune's player (`components/cards/player.jsx`):
a plain `<audio>` element whose `src` is the track's download URL,
`timeupdate` driving the seek bar and current/duration display,
`ended` advancing to whatever is now playing. Additions over upstream:

- **"Listen in browser" toggle**: fetches `/api/stream-url` and
  plays the current track's real stream/download URL (JioSaavn
  `stream_url`, radio stream, file URL) through the same-origin
  `/api/audio` proxy. On `ended` it reloads the stream URL for
  whatever the VC engine is now playing — it never skips the VC
  queue. When no direct audio exists (YouTube), the status line says
  so instead of failing silently.
- **WebAudio volume that actually changes gain**: `AudioContext` +
  `createMediaElementSource(audio)` → `GainNode` →
  `destination`; the slider sets `gainNode.gain.value = v / 100`
  (persisted in `localStorage`, restored on load). If WebAudio is
  unavailable the slider falls back to `audio.volume`. The mode is
  shown under the slider. (Upstream wavetune has no volume control
  at all — no slider, no `audio.volume`, no GainNode.)
- **Seek bar + time display** (`currentTime`/`duration`, `0:00`
  format), previous/next are the VC Skip button and the queue.
- The VC Play/Pause/Skip buttons are unchanged: they remote-control
  the voice-chat engine. The volume slider is now the *browser*
  volume and is no longer overwritten by the engine's reported
  volume.

## Env

- `PORT` (default `3000`)
- `CORE_URL` — core-cpp base URL, e.g. `http://localhost:8000`
- `VC_GO_URL` / `VC_TS_URL` — VC engine base URLs
- `NETEASE_URL` — netease-go base URL (search fallback)
- `PARLAY_TOKEN` — forwarded as `X-Parlay-Token` when proxying
- Tests: `npm test` (`node --test tests/run.js`; 7 tests)

## Still stubbed / not implemented

- No Next.js build, no wavetune React components, no album/artist cards,
  no Saavn-style browse pages (`latest`/`popular`/`albums` from wavetune's
  root page). The player is one page, not the full wavetune UI.
- No Mongo/Redis: demo state is in-memory only and resets on restart.
- No auth on the GUI itself; `PARLAY_TOKEN` only guards upstream calls.
- **Sandbox limit on the audio bytes**: `aac.saavncdn.com` returns 404
  for every fresh media URL through the sandbox egress proxy (images
  on `c.saavncdn.com` return 200), so end-to-end Saavn bytes cannot be
  fetched from here. The `/api/audio` proxy is verified byte-exact
  (full + Range) against a local stub MP3, and `/api/stream-url` is
  verified live against vc-ts/JioSaavn metadata; on a normal network
  the same code serves the Saavn stream.

## Verified 2026-11-02 (see PROGRESS.md)

- `/` returns the player page (controls, queue, search, WebApp hook all
  present); `/healthz`, `/api/now-playing`, `/api/queue`,
  `/api/play|pause|skip` all return correct envelopes in demo mode.
- Proxy verified live: `/api/play` through `VC_GO_URL` reached a real
  vc-go and returned `engine:"go"`; `/api/search` through `NETEASE_URL`
  returned 10 real NetEase results.
- **Phase 4 (`npm test`, 7 tests)**: page ships `<audio>` +
  `createMediaElementSource`/`GainNode` wiring (`gainNode.gain.value
  = v / 100`); `/api/stream-url` returns a real Saavn direct URL for
  a live JioSaavn track and `playable:false` with a YouTube reason for
  a YouTube track; `/api/audio` proxies a real MP3 byte-exact
  (full 200 + Range 206 with exact slice) and rejects bad/dead
  upstreams honestly.
