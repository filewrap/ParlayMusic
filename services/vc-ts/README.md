# vc-ts — TypeScript VC engine + Lemonic + inline

Ported behaviour (re-implemented in plain Node, stdlib only — no npm
dependencies, no TypeScript compiler step; the "TS" in the name is the
upstream stack it mirrors) from ArnabXD TGVCBot
(`apps/tgvcbot/src/{tgcalls,queue}.ts`, `handlers/{play,queue,radio,
youtube,jiosaavn}.ts`, `providers/{youtube,jiosaavn}.ts`) + callsmusic
remix (`src/bot/{queues,session}.ts`, `handlers/{stream,controls,panel,
playlist,search,lyrics}.ts`, `streamer/*`) + lemonic-telegram-bot
(`modules/{lemonic,spotify,songlink}.ts`, `handlers/inline_query.ts`).

Implements the vc.* actions plus `lemonic.search/download` and
`search.inline`, behind the same IPC contract as vc-go
(`contracts/ipc.json`).

## Run

```sh
PORT=8002 PARLAY_TOKEN=<token> node src/main.js
# env: PORT (default 8002), PARLAY_TOKEN (optional; when set, every
# route except /healthz requires header X-Parlay-Token),
# PARLAY_DATA_DIR (default ./data), YT_DLP (yt-dlp path; default
# services/downloaders/.venv/bin/yt-dlp), LEMONIC_API_KEY (optional —
# see Lemonic below), LEMONIC_API (base URL, default
# https://api.lemonic.xyz), SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET
# (optional — Lemonic search fallback, client-credentials flow)
# Tests: npm test  (node --test tests/run.js; 24 tests, live network)
```

## Endpoints

| Method | Route | Notes |
|---|---|---|
| GET | `/healthz` | `{ok:true, service:"vc-ts", ...}` — public, no token |
| POST | `/v1/play` | `{chat_id, query, requested_by}` — plays now, or enqueues if busy. Query: YouTube URL, free text (YouTube), `jiosaavn:<query>`, `jsvn:<id>`, raw `http(s)` audio URL or `radio:<url>` (radio lane, duration 0) |
| POST | `/v1/pause`, `/v1/resume`, `/v1/skip`, `/v1/stop`, `/v1/seek` | `{chat_id[, position_sec]}` — seek is a contract extra (`vc.seek`) |
| POST | `/v1/volume` | `{chat_id, volume}` — 0–200 (remix scale: 100 = 100%) |
| GET | `/v1/panel?chat_id=` | Live control panel state (remix `panel.ts`): now/next, loop, volume, queue count, buttons with `panel_*` callback_data |
| POST | `/v1/panel` | `{chat_id, action}` — `shuffle|skip|pause|resume|mute|unmute|volinc|voldec|loop|update`; volinc/voldec step 10 (remix ±1000 on its 0–20000 scale) |
| POST | `/v1/lyrics` | `{chat_id?, title?, artist?}` — lyrics for the now-playing track (or the given title) via keyless LRCLib; 404 when absent, never faked |
| POST | `/v1/queue` | `{action: add\|remove\|clear\|list, chat_id, position?, track?, query?}` |
| GET | `/v1/now-playing?chat_id=` | `{now_playing}` with `engine:"ts"`, `paused`, `position_sec`, `queue`, `volume`, `loop` |
| POST | `/v1/search` | `{query, source: youtube\|jiosaavn\|lemonic\|all, limit}` — per-source status in `data.sources` |
| POST | `/v1/lemonic/search` | `{query}` → `{results, via}` — `via` is `lemonic`, `lemonic-keyed`, or `spotify-search` |
| POST | `/v1/lemonic/download` | `{track_id}` → downloads audio to `data/lemonic/<id>.mp3` |
| POST | `/v1/inline` | `{query, user_id}` — YouTube + Lemonic (Lemonic always attempted now; skipped only if its API fails) |

All responses are `{ok:true, data}` or `{ok:false, error:{code, message}}`
(`bad_request`, `unauthorized`, `not_found`, `engine_unavailable`,
`download_failed`, `internal`).

## What is real (verified 2026-11-02, see PROGRESS.md)

- Per-chat queue/playback state in memory + JSON persistence to
  `data/state.json` (atomic tmp+rename write); rooms restore **paused**
  after a restart (verified).
- 1 s ticker: wall-clock position + auto-advance to the next queued
  track when a track's known duration elapses (verified: seek to 212 s
  of a 213 s track → next track playing 3 s later).
- YouTube resolve via **yt-dlp `-j`** (live path verified: real title /
  artist / 213 s duration for `dQw4w9WgXcQ`). `ytsearchN:` search works
  too — yt-dlp prints JSONL for searches, which the resolver parses
  line by line (this was a real bug found in testing: single-JSON
  parsing silently returned empty results).
- JioSaavn via TGVCBot's public proxy `jsvn-tgvc.vercel.app`
  (`/search`, `/song?id=`) — verified live with real results
  ("Kesariya", "Tum Hi Ho"). `saavn.dev` is unreachable from the
  sandbox, so it is only a fallback.
- **JioSaavn durations are real (Phase 4 fix).** Root cause, verified
  live: the search payload has no duration field, and the `/song`
  payload's `duration` is a `"m:ss"` string (`"4:28"`) that the old
  `Number(...)` cast turned into `NaN → 0`. Now `parseDuration`
  handles seconds, `"m:ss"` and `"h:mm:ss"`, search results are
  enriched with a parallel `/song?id=` lookup each (failures leave
  the field unset, never faked), and the single-song path returns the
  real value — `jsvn:rjkrTnma` resolves to `duration_sec: 268` with a
  `stream_url`. Auto-advance now fires for JioSaavn tracks (tested:
  seek to 267/268 s → next track within ~3 s). See
  `phase4/deepread-tsjs.md` §7.
- **Lyrics** (`POST /v1/lyrics`, remix `handlers/lyrics.ts`): LRCLib
  (keyless) for the now-playing title — first artist only, since
  JioSaavn artist strings are comma-joined lists and LRCLib answers
  far more reliably that way. Returns plain or synced lyrics;
  404 when absent. Caveat: LRCLib intermittently 503s
  (`ServerOverloaded`), sometimes per-query ("Raabta" 503s while
  "Kesariya" returns 200); the endpoint retries once.
- **Live control panel** (`GET/POST /v1/panel`, remix
  `handlers/panel.ts` + `helpers/volume.ts`): now/next text, loop
  flag, volume, queue count, and the same 10 buttons with the same
  `panel_*` callback_data (`shuffle/skip/pause/resume/mute/unmute/
  voldec/volinc/loop/update`). Volume is remix's 0–200 scale;
  volinc/voldec step 10 (= remix ±1000 on 0–20000). `loop` toggles
  per-room loop — the ticker restarts the current track instead of
  advancing. Shuffle reorders the queue in place (remix also
  re-streams the shuffled head; here the current track keeps playing).
- **Radio lane**: a raw `http(s)` audio URL — or `radio:<url>` — plays
  as a `radio` track (`duration_sec: 0`, never auto-advances), no
  yt-dlp involved. YouTube URLs are still detected first and resolve
  as YouTube, never as radio (regression-tested).
- **Lemonic, corrected to upstream (Phase 4).** Upstream
  `modules/lemonic.ts` does an *unauthenticated* fetch to
  `${LEMONIC_API}/api/raw/track/{id}` — no API key exists upstream,
  and upstream search is Spotify client-credentials search, not a
  Lemonic endpoint. So: raw-track and `/api/search` are tried
  unauthenticated first; `LEMONIC_API_KEY`, when set, adds
  `Authorization: Bearer <key>` (verified against a stub API: no
  header without a key, `Bearer testkey123` with one). Search falls
  back to Spotify client-credentials search when
  `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` are set (same flow as
  the upstream bot: Spotify search → Lemonic raw track by Spotify
  id); `via` in the response says which path answered. Only when
  every fallback fails does search return `engine_unavailable`, and
  the error names everything tried. `/healthz` reports
  `lemonic_mode: keyed|unauthenticated|spotify`. Sandbox reality:
  `api.lemonic.xyz` is unreachable from here (connect fails), so the
  live path is verified via the stub + the honest error path.
- Token auth: 401 without/wrong token on all routes except `/healthz`
  (verified); `X-Parlay-Token` header.
- Queue add (by track object or query) / remove / clear / list,
  pause / resume / seek / skip / stop, bad-request and not-found
  errors — all verified by the test suite (`npm test`, 24 tests).
- Voice transport is real (`src/voicecall.js`): ffmpeg Opus/RTP
  pipeline (251 frames / 251 RTP packets on the 5 s proof tone) +
  ntgcalls local offer + voice-bridge join client. The live join is
  blocked in the sandbox (no MTProto session, UDP sendto EPERM);
  now-playing reports `voice:"opus-rtp-ready"` once the pipeline is
  proven, or `voice:"unavailable:<exact reason>"` — see below.

## What is stubbed / degraded, and why

- **Live voice joining is blocked in the sandbox**
  (`src/voicecall.js`). Joining a real Telegram group call needs a
  live MTProto userbot session (`ASSISTANT_SESSION_FILE`, `API_ID`,
  `API_HASH` — all unset here) and a UDP route to the call (sendto is
  EPERM even to loopback). The transport itself is real, not a stub:
  the Opus/RTP pipeline is proven live (ffmpeg encode + RTP file
  packet count), the ntgcalls wheel builds a real local offer offline,
  and `join()` drives the same voice_bridge.py core-cpp spawns,
  reporting the exact failure. Playback state (queue, position,
  auto-advance) is real; audio reaches a voice chat only after a real
  join, which is a live-test-time check.
- **YouTube is flaky from the sandbox**: direct video resolve works,
  but YouTube intermittently 429s the sandbox egress IP (seen once
  during setup). When yt-dlp fails, resolve falls back to a **degraded
  placeholder track** (`yt:<id>` / `query:<hash>`, `duration_sec: 0`,
  `degraded: true`) so play/queue keep working instead of failing —
  the placeholder says so in `resolve_note`. Degraded tracks never
  auto-advance (unknown duration). yt-dlp resolve itself takes
  6.5–13.7 s per call (measured, network-bound).
- **Lemonic live API unreachable from the sandbox** — see above.
  Nothing is faked; the stub tests + honest error path are the
  verification that exists here.
- The service is plain Node, not TypeScript: no `tsc`, no grammy,
  no `tgcalls` dependency. The port is behavioural, not a line port.

## Files

- `src/main.js` — HTTP server, rooms, ticker, persistence, all routes
  (incl. panel, lyrics, volume, radio lane).
- `src/resolvers.js` — YouTube (yt-dlp) / JioSaavn / Lemonic / lyrics
  (LRCLib) / radio-track resolvers.
- `src/voicecall.js` — real voice transport: Opus/RTP pipeline +
  ntgcalls offer + voice-bridge join client (see above).
- `tests/run.js` — 24 tests (`npm test`): live JioSaavn/LRCLib/yt-dlp
  + stub-Lemonic keyed/unauthenticated + panel/loop/radio regressions
  + voice pipeline/offer/join proofs (offline).
- `Dockerfile` — node:24-slim + yt-dlp (compose `build:` target).
- `PROGRESS.md` — build + verification log.
