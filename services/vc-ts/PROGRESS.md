# vc-ts — progress

Worker A — Node services, Phase 3. 2026-11-02 ~19:20–20:00 IST.

## Toolchain

- Node v24.22.1 at /usr/bin/node, ffmpeg 8.1.2 at /usr/bin/ffmpeg.
- Stdlib only: **no npm install, no new dependencies**. `package.json`
  unchanged (`"start": "node src/main.js"`).
- yt-dlp: present at `services/downloaders/.venv/bin/yt-dlp` (2026.10.26).
  First direct check 429'd on YouTube; later runs resolved fine —
  YouTube rate-limits the sandbox egress IP intermittently.

## Files written (all under services/vc-ts/)

- `src/main.js` — replaced stub. HTTP server, per-chat rooms, 1 s
  advance ticker, JSON-file persistence (atomic tmp+rename,
  `data/state.json`, restored rooms come back paused), all 12 routes,
  token auth, contract envelopes.
- `src/resolvers.js` — YouTube (yt-dlp `-j`, degraded fallback),
  JioSaavn (`jsvn-tgvc.vercel.app`, `saavn.dev` fallback), Lemonic
  (`LEMONIC_API_KEY` gate, raw-track endpoint from `modules/lemonic.ts`).
- `src/voicecall.js` — voice transport stub (see README: why).
- `Dockerfile` — node:24-slim + yt-dlp (compose `build:` target; no
  Dockerfile existed anywhere in the tree before this).
- `README.md` — rewritten: real vs stubbed, endpoints, run instructions.
- `PROGRESS.md` — this file.

## Verification (exact commands, all run 2026-11-02)

```sh
node --check src/main.js src/resolvers.js src/voicecall.js   # OK
PARLAY_DATA_DIR=/tmp/vcts-test2/data PARLAY_TOKEN=testtoken123 PORT=18002 node src/main.js
```

`GET /healthz` (no token) →
`{"ok":true,"service":"vc-ts","engine":"ts","yt_dlp":true,"yt_dlp_path":".../downloaders/.venv/bin/yt-dlp","jiosaavn":"jsvn-tgvc.vercel.app","lemonic_configured":false,"voice":{"voice":"stubbed",...},"rooms":0}`

Auth: `POST /v1/play` with no token and with a wrong token both →
`{"ok":false,"error":{"code":"unauthorized","message":"missing or wrong X-Parlay-Token"}}` (2/2 pass).

Play/queue/now-playing round-trip (chat -100123, token testtoken123):

- `POST /v1/play {"chat_id":-100123,"query":"https://www.youtube.com/watch?v=dQw4w9WgXcQ","requested_by":42}` →
  now_playing, track `yt:dQw4w9WgXcQ`, title "Rick Astley - Never Gonna
  Give You Up (Official Video) (4K Remaster)", artist "Rick Astley",
  `duration_sec:213`, `paused:false`, `position_sec:0` — **live yt-dlp
  resolve, not degraded**.
- `POST /v1/queue {"action":"add","chat_id":-100123,"query":"jiosaavn:kesariya"}` →
  queue `[{position:0, track:{id:"jsvn:rjkrTnma", title:"Kesariya",
  artist:"Pritam, Arijit Singh, Amitabh Bhattacharya", source:"jiosaavn"}}]`
  — **live JioSaavn resolve** via jsvn-tgvc.vercel.app.
- `GET /v1/now-playing?chat_id=-100123` → current + 1 queued item,
  `position_sec:6` (wall clock advancing).
- pause → `paused:true, position_sec:6`; seek 30 → `position_sec:30`
  (paused); resume → `paused:false, position_sec:30`; skip → now
  playing "Kesariya" (jiosaavn), queue empty.
- Queue remove/clear: add two literal tracks, remove position 0 →
  `["Two"]`; clear → `[]`.
- `POST /v1/play` without chat_id → `bad_request "chat_id is required"`.
- stop → `{"ok":true,"data":{}}`; now-playing after stop →
  `not_found "no playback state for this chat"`.

Search:

- `POST /v1/search {source:"jiosaavn", query:"arijit singh", limit:2}` →
  2 real results ("Gehra Hua (From Dhurandhar)", "Gehra Hua").
- `POST /v1/search {source:"youtube", query:"lofi beats", limit:2}` →
  initially `results:[]` with a swallowed error — **real bug found in
  testing**: yt-dlp prints JSONL (one JSON object per line) for
  `ytsearchN:`, and the resolver did single-JSON parsing, so every
  search silently returned nothing. Fixed in `resolvers.js` (line-by-line
  JSONL parse) + `main.js` (a search error with zero results now marks
  the source `"error"` instead of `"ok"`). After fix: 2 results
  ("lofi hip hop radio …", "cozy autumn music …", Lofi Girl).
- `POST /v1/search {source:"all", query:"kesariya"}` → 2 results,
  `sources: {youtube:"ok", jiosaavn:"ok", lemonic:"engine_unavailable",
  lemonic_error:"Lemonic is not configured: set LEMONIC_API_KEY ..."}`.
- `POST /v1/inline {query:"lofi", user_id:42}` → 5 results after fix
  (0 before fix, same JSONL bug).
- `POST /v1/lemonic/search` and `/v1/lemonic/download` (no key) →
  `{"ok":false,"error":{"code":"engine_unavailable","message":"Lemonic
  is not configured: set LEMONIC_API_KEY (and optionally LEMONIC_API
  base URL) to enable Lemonic search/download"}}` — honest, no fake
  results (2/2 pass).

Auto-advance: play Rick Astley (213 s) in chat -100999, queue
`jiosaavn:tum hi ho`, seek to 212 → 3 s later now-playing is
"Tum Hi Ho" (jiosaavn), `position_sec:1`. Pass.

Persistence: state file held 1 room (current Rick Astley, 1 queued,
`paused:true`); killed the server, restarted with the same
`PARLAY_DATA_DIR` → log `restored 1 room(s) ... (paused)`, now-playing
returns the track, `paused:true`, queue 1, `position_sec:15`. Pass.
(Note: an earlier restart attempt killed the wrong PID — the bash
wrapper, not the node process — and hit EADDRINUSE; the real restart
after killing the node PID worked.)

## Test tally

- vc-ts: 20 checks, 20 pass (healthz, auth ×2, play, queue add/list/
  remove/clear, now-playing, pause, seek, resume, skip, stop, 404
  after stop, bad play, jiosaavn search, youtube search, all-source
  search, lemonic search, lemonic download, inline, auto-advance,
  persistence restore). YouTube search + inline initially FAILED
  (JSONL bug), fixed and re-verified — counted as pass after fix.

## Still stubbed / untested

> **SUPERSEDED in part (Phase 4/5, 2026-11-03).** The bullets below are
> the Phase 3 state. Since then: voice transport is the real Opus/RTP
> pipeline + in-process ntgcalls offer (live join blocked in the
> sandbox, not a stub — see the "Voice transport" section below);
> JioSaavn durations are real (268 s, Phase 4 fix); lyrics
> (`POST /v1/lyrics`) and the live control panel (`/v1/panel`,
> `/v1/volume`) are built and in the contract (Phase 4/5); the
> degraded placeholder resolve was deleted (Phase 5, A9) — a failed
> YouTube resolve is `engine_unavailable`, never a fake track.
> Still true: the Lemonic keyed path is untested (no key in sandbox).

- Voice joining (ntgcalls/tgcalls) — stubbed, see README.
- Lemonic keyed path (search/download with a real key) — untested, no
  key in sandbox.
- JioSaavn `duration_sec` is 0 on search results (upstream search
  payload has no duration) → no auto-advance for those tracks.
- Lyrics + live control panel (remix) — no contract action, not built.
- YouTube 429s the sandbox egress IP intermittently; degraded
  placeholder resolve covers it but never auto-advances.

## Phase 4 — Worker C (2026-11-02)

Deep read: `phase4/deepread-tsjs.md` (TGVCBot/remix VC join via userbot
+ tgcalls-next, panel.ts, Lemonic shape, wavetune player, openradiobot
flow, beetube registry).

- **JioSaavn duration fixed**: root cause was twofold — search payload
  has no duration, and `/song` returns `"4:28"` which `Number()`
  turned into NaN→0. `parseDuration` (seconds / "m:ss" / "h:mm:ss")
  + parallel `/song?id=` enrichment per search result. Verified live:
  `jsvn:rjkrTnma` → 268 s + `stream_url`; auto-advance fires
  (seek 267/268 → next track in ~3 s).
- **Lyrics** `POST /v1/lyrics` via keyless LRCLib (first artist only);
  404 when absent. LRCLib intermittently 503s, sometimes per-query
  ("Raabta" 503s, "Kesariya" 200) — endpoint retries once.
- **Live control panel** `GET/POST /v1/panel` (remix button ids
  `panel_*`, volume 0–200 step 10) + `POST /v1/volume` + per-room
  `loop` (ticker restarts current track). Shuffle reorders queue
  in place.
- **Radio lane**: raw `http(s)` URL or `radio:<url>` → `radio` track
  (duration 0); YouTube URLs still resolve as YouTube first.
- **Lemonic corrected to upstream**: unauthenticated first (upstream
  has no key), Bearer iff `LEMONIC_API_KEY` set; search chain
  Lemonic `/api/search` → Spotify client-creds → honest
  engine_unavailable naming what was tried; `via` in responses,
  `lemonic_mode` in healthz. `api.lemonic.xyz` unreachable from the
  sandbox — verified via stub API (no-auth vs Bearer) + dead-API
  error path.
- **Tests**: `tests/run.js`, `npm test` → **19/19 pass** (live
  JioSaavn/LRCLib/yt-dlp, stub Lemonic, panel/loop/radio regressions).
  Also fixed en route: lemonic download fetch throw → 502
  `download_failed` (was 500).
- Benchmarks: `phase4/bench-tsjs.md` (yt-dlp play 6.5–13.7 s,
  jiosaavn search ~2 s, panel/now-playing <25 ms).

## Voice transport — real pipeline + join client (Phase 4, TDLib wiring B, 2026-11-02)

> SUPERSEDED (Phase 7, A1): the `voice_bridge.py` join client described
> below was deleted in Phase 5 (user order). The join is now delegated
> to the core: `join()` POSTs vc.join to `{CORE_URL}/v1/join` and
> reports the core's answer verbatim. The rest of this section (Opus/RTP
> pipeline, local offer, proof numbers) is still accurate.

- `src/voicecall.js` rewritten (was a stub): ffmpeg Opus/RTP pipeline
  (libopus 128k 48 kHz stereo; Ogg Opus file ffprobe-verified; RTP to a
  file + SDP, packet count proven by SSRC-anchored parse), ntgcalls
  local offer via phase4/voice-venv (`src/ntg_offer.py`), and a join
  client driving the same `voice_bridge.py` core-cpp spawns.
  `playStream()` runs the pipeline then attempts the join; now-playing
  reports `voice` / `voice_joined` / `voice_reason`. `joined:true`
  never without a real join.
- Proof (5 s 440 Hz tone, offline): 251 frames, 83,736 bytes, 5.0065 s;
  251 RTP packets + 1 RTCP SR, 85,973 bytes; SDP `m=audio 0 RTP/AVP 97`,
  `a=rtpmap:97 opus/48000/2`. Local offer 456–476 chars (random per run).
- Exact failing calls: UDP sendto EPERM even to loopback (Node dgram
  `EPERM (send EPERM 127.0.0.1:1)`; ffmpeg `rtp://127.0.0.1` exit 255
  "Operation not permitted"); join fails at the config gate
  (`ASSISTANT_SESSION_FILE` / `API_ID` / `API_HASH` unset); with a dummy
  empty session file the bridge itself fails `ValueError` before any
  network. No live call possible in the sandbox.
- Tests: 24 pass / 0 fail (`node --test tests/run.js`), incl. 5 new
  voice proofs (pipeline, offer, join gate, playStream state,
  now-playing voice fields). No queue/IPC regression.
