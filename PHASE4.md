# Parlay — Phase 4 Report (deep port, kill every stub)

Date: 2026-11-02. Work in `~/workspace/parlay/`. Nothing pushed to
GitHub, no tokens/secrets in the tree. One worker (B, Go) errored
mid-run on the output token cap after finishing its deep read and
the NetEase crypto port; its remainder was re-dispatched as two
narrow workers (B1: vc-go loop/shuffle/autoplay + Mongo; B2:
NetEase live download verification) — both completed.

## Step 1 — Deep read (before any porting)

Four per-language deep reads, all from actual code (not READMEs):
`phase4/deepread-cpp.md` (387 lines), `phase4/deepread-go.md` (359),
`phase4/deepread-tsjs.md` (451), `phase4/deepread-rustpy.md` (510).
Synthesis: `DEEP_READ.md`. Unified command inventory: `COMMANDS.md`.

The headline finding, confirmed in all four families: **the bot
account never plays audio — the assistant/userbot account holds the
MTProto voice session and streams it.** NoxBeats
(`Bad/cmd/play.cpp::ensure_assistant()`, round-robin `SESSION`…
`SESSION10`), TinaMusic (`tinaproto/client.cpp::Play()`), TGVCBot /
remix (grammy + `TelegramClient` + `tgcalls-next`), ParvatiMusic
(`assistant.rs`, `context.rs::join_locked`), Yukki-Go / ArcMusic /
TgMusicBot — all the same shape. The core therefore owns the Bot API
surface and routes; voice joining lives behind the core's `VoiceCall`
interface with the assistant session.

## Step 2 — Stubs killed (each with proof)

### core-cpp (Worker A)
- **Full dispatcher**: `src/dispatcher.cpp` rewritten (3,199 lines),
  full NoxBeats inventory — playback, queue-ops, playlists, settings,
  autoplay, channel-play, admin/sudo/broadcast/auth/blacklist, RTMP,
  eval, search/download/lyrics/radio/share/engine, 34 `c*`
  channel commands. Every command dispatches to a real IPC call or
  Store action; permission model ported (owner > sudo > admin/auth,
  admin_mode 0/1/2, play_mode, blacklists, maintenance, 1 cmd/s).
  Bugs fixed: `/clear` lists before clearing (so `/restore` works),
  `/restore` skips the still-playing track, `fn_ntg_time` enum,
  `EngineRouter` const-correctness, `Json(long long)` ambiguity.
- **Store**: hand-rolled Redis RESP + MongoDB OP_MSG/BSON clients.
  `tests/store_test.cpp` ALL PASS vs real mongod 8.0.4 (:27017) and
  real redis-server built from source (:6399). Mongo SCRAM auth
  honestly rejected (not implemented).
- **Voice pipeline proven, no logging stub**: `voice_call.*` +
  `voice_bridge.py` (Telethon + ntgcalls, TinaMusic `Play` steps).
  Opus proof: 5 s tone → **251 Opus frames, 83,736 bytes, 5.0065 s**
  (ffprobe); RTP packetization to a file (85,973 B, SDP
  `opus/48000/2`, RTCP SR v2). UDP to a live receiver is blocked by
  the sandbox (EPERM on `sendto`, even loopback — proven with plain
  Python); RTP honestly reports `rtp_ok=0`, file encode still
  succeeds. Two bugs fixed by the proof (double `rtp://` prefix;
  RTP sharing one ffmpeg run with the file encode).
- **Build**: clean `-j4` 35.1 s, binary 1,002,808 B (866,656 stripped),
  0 errors, 8 warnings (all libstdc++ `variant` false positives;
  zero in own code).

### vc-go (Worker B1)
- Loop (0–10, `/skip` resets), shuffle (pick-only, stored order
  untouched), autoplay (YouTube mix `RD<video-id>`, history cap 50,
  mutex released during the network call). `NowPlaying` reports
  loop/shuffle/autoplay. Contract actions `vc.loop/shuffle/autoplay/
  autoplay_skip` + `NowPlaying` fields.
- **MongoDB**: real official driver v2.3.0, DB `parlay`, collection
  `vc_rooms`, upsert + stale prune; unreachable `MONGO_URI` is fatal
  at startup (never silently falls back). JSON file only when
  `MONGO_URI` is unset. Live test vs local mongod: play + queue +
  loop 2 + shuffle/autoplay → kill → restart → room restored from
  Mongo (paused, loop 2, queue intact); `/autoplay-skip` advanced
  with loop reset. JSON fallback verified the same way.
- Tests: `go vet` clean, `go test` **18/18** (was 6/6). Bench: clean
  build 92.4 s (driver compile; warm 8.1 s), stripped binary 9.3 MB,
  startup-to-healthz 0.03 s.
- Gap: live autoplay picks unverified here — no yt-dlp in the
  sandbox, so `resolver.Related` returns nil and a drained room
  idles; the pick path is unit-tested with an injected picker.

### netease-go (Worker B, finished by B2)
- weapi/eapi/linuxapi crypto ported from Music163bot-Go (AES-ECB
  key `e82ckenh8dichen8`, MD5 splice, `params` HEX-uppercase;
  classic weapi double AES-CBC + RSA `encSecKey`).
- **Download works live**: 210049 布拉格广场 34,918,981 B FLAC
  (294.6 s), 509781655 27.4 MB, 490595315 22.2 MB — all ffprobe-verified.
  64.6 MB file correctly refused by the 50 MB cap. Direct encrypted
  routes all accepted (HTTP 200, API 200) but returned `url:null`
  per-track (`code:404`; eapi download URL `code:-110`) — crypto
  proven correct, anonymous playback gated server-side for this
  egress IP; the public mirror fallback is what delivers. Only a
  `MUSIC_U` login would unlock direct URLs.

### vc-ts (Worker C)
- JioSaavn duration fixed at the root: search results carry no
  duration; `/song?id=` returns `"duration":"4:28"` — the old
  `Number()` cast turned it into `NaN → 0`. Now `parseDuration`
  + parallel `/song` enrichment: `jsvn:rjkrTnma` → 268 s + stream
  URL, auto-advance fires. Lyrics endpoint (LRCLib, keyless).
  Live control panel (`GET/POST /v1/panel`, remix button ids,
  volume 0–200 step 10) + `/v1/volume` + per-room loop.
  Radio lane (raw URL / `radio:<url>`). Lemonic reworked:
  unauthenticated first (upstream has no API key — the Phase 3
  gate was invented), Bearer iff key set, search chain Lemonic →
  Spotify client-creds → honest `engine_unavailable`.
- Tests: `npm test` **19/19** (live JioSaavn/LRCLib/yt-dlp).

### gui-js (Worker C)
- Real in-browser audio: `<audio id="player">` + "Listen in
  browser" toggle, fed by `GET /api/stream-url`; WebAudio gain
  (`AudioContext` + `GainNode`, slider sets `gain.value = v/100`,
  localStorage-persisted, `audio.volume` fallback); seek bar +
  time display; `ended` reloads the stream URL for whatever is now
  playing. `GET /api/audio?src=` same-origin proxy (Range
  passthrough, 206) so the GainNode sees CORS-clean media.
  Telegram WebApp hook kept.
- Tests: `npm test` **7/7** (page wiring, live stream-url for a
  JioSaavn track, proxy byte-exact full + Range 206).

### radio-js (Worker C)
- VC handoff implemented (contract `vc.radio` existed, code did
  not): on start, `POST /v1/play` with the stream URL to vc-go
  then vc-ts, reported as `vc_handoff`; on stop, the engine room
  is cleared only if still on the radio track.
- Tests: `npm test` **7/7** (live vc-ts handoff verified).

### vc-rust (Worker D)
- Download path: every track that starts is downloaded first to
  local mp3 (yt-dlp bestaudio, 300 s cap, android-client retry);
  failed download never stalls the room. Loop (count 0–10),
  shuffle (Fisher–Yates on enable + random pick), autoplay
  (YouTube mix `RD<video_id>`, first entry not in history).
- Live evidence: `/play` Rick Astley → 7,045,581 B mp3,
  `downloaded:true`, 17.7 s; `/autoplay-skip` → Justin Timberlake
  "CAN'T STOP THE FEELING!" (286 s) from the 360-entry mix,
  57.8 s end-to-end. State persists across restart.
- Tests: `cargo test --release` **9/9** (7 unit + 2 live gated).

### spotify-rust (Worker D)
- Real Spotify API client: client-credentials flow (std-only,
  `curl` subprocess, token cached in memory, never on disk).
  `resolve` enriches with real metadata when creds exist;
  `spotify.collection` (albums whole, playlists paginated 100/page
  capped 100, artists top 10); `spotify.status` (no secrets in
  response). Without creds: parse-only + honest note, or 503
  pointing at the downloaders fallback — never a fabricated list.
- Tests: `cargo test --release` **8/8** (live tests skipped: no
  creds in sandbox).

### downloaders (Worker D)
- **Deezer**: real — preview 479,827 B; album ZIP 14 previews /
  6,696,088 B. Full tracks need a Deezer ARL cookie (named in
  every track `note`).
- **TikTok**: real — 711,792 B mp3 (yt-dlp's own downloader stalls
  on reads from this egress; plain GET works).
- **Navidrome**: real — 5,365,035 B via Subsonic on
  demo.navidrome.org (server 0.64.2). Soulseek-via-slskd path
  still needs its own daemon.
- **Spotify playlist**: real 50-track list via `__NEXT_DATA__`
  embed fallback (yt-dlp refuses Spotify with DRM); track artist
  now resolved. One live failure found and fixed (1-track result
  → embed fallback, re-run green).
- Tests: `unittest discover` **21 tests: 13 unit pass, 8 live
  pass, 1 skipped in D's run** (Spotify API live, no creds);
  coordinator re-run: 21 tests OK (13 pass, 8 live skipped —
  live tests are gated, unit suite green).
- Coordinator fix: `src/server.py` had no `__main__` block and
  crashed on relative imports when run directly — added a
  sys.path bootstrap + `__main__` guard (my first edit landed
  mid-function and broke `serve()`; caught by the re-test and
  fixed). Both entry points now verified: `python -m src.main`
  and `python src/server.py` serve correctly.

## Step 3 — Speed + compilation

Full numbers: `BENCHMARKS.md`. Summary:

| Service | Clean build | Binary (stripped) | Startup |
|---|---|---|---|
| core-cpp | 35.1 s | 866,656 B | 0.00 s (missing-env exit) |
| vc-go | 92.4 s (warm 8.1 s) | 9.3 MB | 0.03 s |
| vc-rust / spotify-rust | 9.2 s / 8.1 s | 821,336 B / 783,232 B | < 1 s |
| vc-ts / radio-js / gui-js | no build step | — | 0.4–3.6 s |
| netease-go | seconds | — | < 2 s |

Latency: `/play` YouTube 6.5–13.7 s (yt-dlp resolution, network
variance); JioSaavn play 0.5–0.6 s queued; local endpoints < 25 ms;
lyrics 1.0–1.9 s; radio start 0.9–1.8 s. Download: YouTube mp3
17.7–36 s/track (extract + transcode); TikTok/Navidrome/Deezer
2–6 s (finished file, no transcode).

## Coordinator verification (independent re-run)

- **Rebuilds, all PASS**: core-cpp `[100%] Built target
  parlay-core`; vc-go `go vet` + build + test; netease-go vet/build/
  test; all five Rust services `cargo build --release --offline`
  (0 warnings); `node --check` on all Node files; downloaders
  `py_compile` OK; `contracts/ipc.json` valid, **41 actions**.
- **Tests re-run**: vc-go 18/18 (queue/resolver/store ok);
  netease-go ok; vc-rust 9/9; spotify-rust 8/8; vc-ts **19/19**;
  radio-js **7/7**; gui-js **7/7**; downloaders 21 OK (13 unit
  pass, 8 live skipped/gated).
- **Combined live smoke** (11 services, test ports, token
  `p4-test`): healthz **11/11** after fixing my own harness
  mistakes (first run: `go run` startup race + `python
  src/server.py` wrong entry point — both mine, not the services;
  re-ran correctly). Live results: vc-go play + loop OK; vc-ts
  JioSaavn "Kesariya" 268 s + panel OK; vc-rust play (download
  path engaged, yt-dlp failed on the pattern test id as
  expected); FX nightcore → real file; share create + token;
  queue vote 1/3; spotify skip-check "Imagine" `skip:false` with
  real LRCLib lyrics; radio stations 6 (5 reachable); NetEase
  search 10 real results; GUI page serves; downloaders live
  YouTube search 2 real results. core-cpp with no env exits
  `missing required env vars: BOT_TOKEN`. No stray processes left.
- **Stub grep** (`TODO|FIXME|XXX|engine_unavailable|stub` across
  services): remaining hits are (a) `engine_unavailable` as the
  contract's honest error code (legitimate), (b) the voice-join
  stubs listed below, (c) test-only stub servers. Zero killed-list
  stubs remain unaddressed.

## Remaining gaps (with evidence — not stubs by omission)

1. **Live voice joining / audio into a voice chat** — the one
   remaining transport gap, in vc-ts (`voicecall.js` stub) and
   vc-rust (`LoggingVoiceCall`). What Phase 4 proved instead:
   the full Opus pipeline (251 frames, 83,736 B, 5.0065 s), RTP
   packetization to a file, and the Telethon + ntgcalls bridge
   steps up to the network call (476-char offer from
   `create_call`/`set_stream_sources` offline). What blocks a
   live join here: sandbox UDP `sendto` → EPERM even on loopback,
   and no direct TCP to Telegram DCs (proxy-only egress). Needs a
   host with a real assistant session + open network.
2. **TDLib build — 62% and still running** in the background
   (`phase4/tdlib-build.log`, `-j2`, 7 GB RAM). Configure passed
   (after gperf 3.1 + libopus-dev 1.4; apt mirror fix logged in
   `phase4/apt-install.log`). ntgcalls wheel 3.0.5 has no C
   `ntg_*` symbols (only `PyInit_ntgcalls`) — the C++ dlopen path
   needs a real `libntgcalls.so` build; the Python bridge is the
   working voice route until then.
3. **Mongo SCRAM auth** (core-cpp Store): authenticated URIs
   rejected honestly — not implemented.
4. **Spotify per-user playback monitoring**: needs per-user OAuth
   (rustify `auth.rs`); app-only flow covers metadata/track lists.
   Genius/Musixmatch lyrics + AI-slop classifier need keys /
   a classifier. Lemonic live API unreachable from the sandbox
   (connect fails); verified via stub API + dead-API error path.
5. **Deezer full tracks**: need an ARL cookie (previews only
   without one). **Navidrome Soulseek path**: needs its own slskd
   daemon. **NetEase direct URLs**: need `MUSIC_U` login (mirror
   fallback delivers today). **Autoplay live picks in vc-go**:
   need yt-dlp installed (unit-tested with an injected picker).
6. **Core honest limits** (documented in worker-a.md): mute is
   flag-only (no `vc.mute` contract action); speed/bass are
   seek-restart + stored value; `/play` from replied-to media
   needs the `file_id` hand-off; `/lang` English-only;
   `/broadcast` failures inline (no errors.txt); `/speedtest`
   download-only.
7. **No live Telegram test yet** — needs BOT_TOKEN + assistant
   session. No push to GitHub — awaiting the user's repo URL.

## Files

- `DEEP_READ.md`, `COMMANDS.md`, `BENCHMARKS.md`, `PHASE4.md`
  (this file)
- `phase4/deepread-{cpp,go,tsjs,rustpy}.md`,
  `phase4/bench-{core-cpp,go,tsjs,rustpy}.md`,
  `phase4/worker-{a,b1,b2,c,d}.md`, `phase4/tdlib-build.log`,
  `phase4/apt-install.log`, `phase4/db-install.log`
- `contracts/ipc.json` — 41 actions (was 35)
- All 12 service READMEs updated.
