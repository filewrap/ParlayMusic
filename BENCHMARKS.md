# Parlay — Benchmarks (Phase 1 rewrite)

Rewritten 2026-11-03 in Phase 1 (FINAL-ARCHITECTURE.md §9) from measured
evidence only. Anchor: `d5065b9` (Phase 9). Supersedes the Phase 4 version
of this file (2026-11-02) wherever Phase 1 measured a number; Phase 4
numbers that Phase 1 did not re-measure are kept in §7 and labelled with
their provenance — they are still valid, not re-verified.

## Measurement provenance

- **Engine resolve benchmarks** (`t_resolve`): `phase1/benchmarks-engines.md`
  (Worker B), runs 2026-11-03 07:56–08:05 IST. The three engines ran locally
  on distinct ports (vc-go :18001, vc-ts :18002, vc-rust :18003) with
  JSON-file state in `/tmp/phase1-bench/`; each engine's real `POST /v1/play`
  resolve path was driven with `curl` (fresh `chat_id` per call, 120 s cap).
  `t_resolve` is one `[telemetry] t_resolve_us=<integer> source=<source>
  engine=<go|ts|rust>` line per resolve attempt (success and failure),
  monotonic clock, covering query/URL input → playable media URL +
  metadata. Raw logs: `/tmp/phase1-bench/vc-{go,ts,rust}.log`; raw results:
  `/tmp/phase1-bench/results.txt` (30 rows); harness:
  `/tmp/phase1-bench/bench.sh`.
- **Core stage timers** (`t_dispatch_to_ipc`, `t_voice_join`,
  `t_first_frame`): `phase1/timers-core.md` (Worker A), 2026-11-03.
  One `[telemetry] <name>_us=<integer> [key=value ...]` line per
  attempt, monotonic (`std::chrono::steady_clock`). A stage that never ran
  is omitted, never zero-filled; a failed attempt emits `ok=0` with
  `failed_stage=` and the exact `error=`; `*_us = -1` means "no such
  packet/stage was produced". Totals on failed attempts are time-to-failure.
- **Environment (both workers):** sandbox, 4 cores, 7 GB RAM, g++ 13,
  cmake 3.28.3, Go 1.23.0 (`~/workspace/parlay/.tools/go/bin/go`),
  cargo/rustc 1.99.1 (`~/.cargo/bin`), Node v24.22.1, ffmpeg 8.1.2,
  yt-dlp 2026.10.26 (`services/downloaders/.venv/bin/yt-dlp`).
- **No user token was used anywhere; all local runs used dummy creds
  only.** No `/play` was issued against real Telegram; no service was
  started with the user's token (see §8 for what could not be measured).
- **Rule for this file:** a number appears here only if it was measured
  in one of the two Phase 1 reports or in the Phase 4 record (§7). A cell
  with no measurement names its exact blocker — never a fabricated number.
  Anything still an estimate is labelled **ESTIMATE** with its source.

---

## §1. Measured table — per-source × per-engine `t_resolve` (microseconds)

Medians of the repeated runs; all reps listed. "n/a" means the engine has
no resolver for that source (not a failed measurement).

### YouTube — URL (`https://www.youtube.com/watch?v=dQw4w9WgXcQ`)

| Engine | rep1 | rep2 | rep3 | **Median** | HTTP wall-clock (median) |
|---|---|---|---|---|---|
| vc-go | 5,632,422 | 5,960,353 | 5,875,831 | **5,875,831 µs (5.88 s)** | 5.88 s |
| vc-ts | 6,483,570 | 5,693,403 | 5,354,212 | **5,693,403 µs (5.69 s)** | 5.73 s |
| vc-rust | 5,316,676 | 6,247,918 | 5,857,190 | **5,857,190 µs (5.86 s)** | 29.38 s (see §1 note) |

### YouTube — free-text search (`never gonna give you up rick astley`)

| Engine | rep1 | rep2 | rep3 | **Median** | HTTP wall-clock (median) |
|---|---|---|---|---|---|
| vc-go | 6,660,718 | 5,668,585 | 7,374,034 | **6,660,718 µs (6.66 s)** | 6.66 s |
| vc-ts | 7,521,034 | 6,511,333 | 6,092,429 | **6,511,333 µs (6.51 s)** | 6.52 s |
| vc-rust | 6,341,906 | 6,683,776 | 6,386,910 | **6,386,910 µs (6.39 s)** | 38.09 s (see §1 note) |

**YouTube conclusion (measured):** all three engines return the same real
track (Rick Astley, 213 s) with a signed
`googlevideo.com/videoplayback?expire=…` stream URL, and all three are
statistically identical — every one shells out to the same yt-dlp
(`-j -f bestaudio/best --no-playlist`, android retry on bot-check), so the
~5.3–6.5 s is yt-dlp + YouTube, not engine code. **No engine wins; the
subprocess is the bottleneck.** Footnote: vc-go search rep1 resolved a
different video than the other runs (`yt:3BFTio5296w`, "Never Gonna Give
You Up (2022 Remaster)") while ts/rust reps resolved `dQw4w9WgXcQ` —
`ytsearch1:` top-hit nondeterminism across runs, not an engine difference.

**vc-rust wall-clock note:** vc-rust `/v1/play` is resolve + synchronous
download. HTTP wall-clock 27.1 / 29.4 / 49.3 s (URL reps) and 34.5 / 38.1 /
42.6 s (search reps) vs `t_resolve` 5.3–6.7 s. The gap is a synchronous
yt-dlp audio download inside the play handler (`downloaded: true`,
`file_path: data/downloads/chat-<id>/<video_id>.mp3`, ~93 MB downloaded
across the bench into `/tmp/phase1-bench/rust`). `t_resolve` correctly
excludes it. Phase 3 must not copy this shape: the play response must
return after resolve, with download/playback async. vc-go and vc-ts
`/v1/play` return right after resolve (HTTP ≈ `t_resolve` + ~0.1 s).

**Reconciliation with the old number:** the Phase 4 record (§7) said
`/play` YouTube URL (vc-ts, live yt-dlp) **6.5–13.7 s**. The new medians
(5.69–5.88 s URL, 6.39–6.66 s search) sit at the fast end of that old
range. The old range is not contradicted — it is a wider historical range
over more runs with more network variance; the new numbers are the
tighter Phase 1 medians (3 reps per cell, same video, same sandbox).

### JioSaavn

| Engine | Input | rep1 | rep2 | rep3 | **Median** |
|---|---|---|---|---|---|
| vc-ts | `jiosaavn:Kesariya` (search + parallel `/song` enrichment, 1 result) | 991,686 | 478,444 | 512,810 | **512,810 µs (0.51 s)** |
| vc-ts | `jsvn:rjkrTnma` (single `/song` lookup) | 316,407 | 239,237 | 338,619 | **316,407 µs (0.32 s)** |
| vc-go | — | n/a — no JioSaavn lane | | | |
| vc-rust | — | n/a — no JioSaavn lane | | | |

Both resolve to the real track (Kesariya, 268 s, `duration_str` `4:28`,
stream URL `https://aac.saavncdn.com/871/…_96.mp4` — see §3 for why it is
`_96`). Rep1 of the search (991,686 µs) is a cold first-call outlier
(proxy/connection warm-up); reps 2–3 are ~0.5 s.

**Reconciliation with the old number:** the Phase 4 record (§7) said
`/play` JioSaavn (vc-ts, queued) **0.5–0.6 s**. The new median
(0.51 s search+enrich) reproduces it exactly; the new single-lookup
median (0.32 s) is the faster path the old record did not separate out.

### Radio

| Engine | Input | rep1 | rep2 | rep3 | rep4 | rep5 | **Median** |
|---|---|---|---|---|---|---|---|
| vc-ts | `radio:https://ice1.somafm.com/groovesalad-128-mp3` | 8,217 | 116 | 96 | 108 | 95 | **108 µs** |
| vc-go | — | n/a — no radio lane | | | | | |
| vc-rust | — | n/a — no radio lane | | | | | |

No network I/O at all: the URL *is* the stream (`radioTrackFromUrl`
builds the track from the URL + a SHA-1 id, duration 0, `is_live: true`).
Rep1 (8,217 µs) is first-call warm-up. Caveat: the SomaFM stream itself
was **not** fetched or verified — the engine never checks reachability on
this path (that is the radio-js service's ffprobe job, not the resolve).

### Spotify — blocked (time-to-honest-failure, not a resolve time)

| Engine | Input | t_resolve | Outcome |
|---|---|---|---|
| vc-rust | `https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC` | **8,321 µs** | HTTP 502 `engine_unavailable`: `Spotify track cannot be resolved: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)` |
| vc-go | — | n/a — no Spotify lane | |
| vc-ts | — | n/a on the `/v1/play` path (Spotify exists only as a *search* fallback via Lemonic/`searchLemonic`, not as a play lane) | |

Blocked, exactly: no Spotify creds in the sandbox (by design — dummy
creds only) **and** the egress proxy refuses the token endpoint
(`accounts.spotify.com/api/token` → 502, per the `main.rs` comment;
`api.spotify.com` itself answers). Note vc-rust's Spotify resolve would
return metadata only anyway (`stream_url: None`) — audio comes from the
YouTube ladder.

### Deezer / NetEase / Navidrome / Soulseek / TikTok — unmeasured, blockers named

| Source | vc-go | vc-ts | vc-rust | Blocker / owner |
|---|---|---|---|---|
| Deezer | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/deezer.py` (ARL + gw API + Blowfish-CBC, §2). Blocked: **no Deezer ARL cookie supplied** (user has not provided one). |
| NetEase | n/a | n/a | n/a | No engine implements it. Owner: `services/netease-go` (weapi/eapi/linuxapi, §2). Blocked: **no `MUSIC_U` cookie**; song-URL path needs a logged-in session. |
| Navidrome | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/navidrome.py` (Subsonic, §2). Blocked: **no Navidrome server URL/creds in the sandbox**. |
| Soulseek | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/soulseek/` (slskd HTTP API, §2). Blocked: **slskd is not running** (`curl 127.0.0.1:5030` → HTTP 000) and no slskd login exists in the sandbox. |
| TikTok | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/tiktok.py` (yt-dlp `-J` + direct CDN fetch, §2). Not measured live: no representative TikTok URL on hand and no engine path to drive; strategy documented from code only. |

### Reference probes — native InnerTube (not engine resolves; fastest-strategy evidence, §2)

Direct InnerTube calls from the sandbox, same video (`dQw4w9WgXcQ`),
public WEB key taken from the refs:

| Call | Result |
|---|---|
| `POST m.youtube.com/youtubei/v1/player` WEB 2.20250101.01.00 | HTTP 200, **0.76 s**, `playability: UNPLAYABLE`, 0 adaptiveFormats (metadata only) |
| `POST …/player` ANDROID 20.10.38 | HTTP 200, **0.81 s**, `playability: OK`, 26 adaptiveFormats — audio itags 139/140/249/251, all with direct URLs |
| `POST …/player` ANDROID_VR 1.62.27 | HTTP 200, **0.61 s**, `playability: OK`, same 26 formats |
| `POST …/player` IOS 19.29.1 | HTTP 400 (stale client version — not retried; version must come from the refs at port time) |
| `POST …/search` WEB, `params=CAASAhAB` | HTTP 200, **1.56 s**, 20 video hits, first hit `dQw4w9WgXcQ` correct |

So a native InnerTube resolve is **~0.6–0.8 s (player) / ~1.6 s
(search)** vs yt-dlp **~5.7–6.7 s** — roughly 7–10× faster, measured live
from this sandbox on the same video. Raw payloads: `/tmp/innertube-player.json`,
`/tmp/it-ANDROID.json`, `/tmp/it-ANDROID_VR.json`, `/tmp/it-search.json`.

---

## §2. LOCKED fastest resolution strategy per source

Locked for the Go backend port (FINAL-ARCHITECTURE.md §9 Phase 1: "lock
that exact algorithm"). "Fastest" = fewest round trips / no subprocess,
judged on the mechanism plus the measurements in §1 where they exist.

| Source | Fastest strategy | Exact algorithm / file |
|---|---|---|
| **YouTube** | **Native InnerTube, ANDROID_VR (or ANDROID) `player` + WEB `search`** — one in-process HTTPS POST, no subprocess. Measured 0.61–0.81 s (player) / 1.56 s (search) vs 5.7–6.7 s for yt-dlp. | YukkiMusic-Go `internal/platforms/youtube.go:74` `Get` / `:362` `fetchVideo` / `:479` `callInnerTube` (`POST https://m.youtube.com/youtubei/v1/<search\|player\|browse\|next>?key=<key>`, WEB client 2.20250101.01.00); ParvatiMusic `bot/src/platforms/youtube.rs:237` `call_innertube` (`POST {base}/youtubei/v1/{ep}?key={INNER_TUBE_KEY}`) + `:360` player / `:279` search; ArcMusic `internal/platforms/youtube.go:272` `callInnerTube("search", …)`. Stream URL = adaptiveFormats itag **251** (Opus) / **140** (AAC) from the ANDROID/ANDROID_VR `player` response (WEB is UNPLAYABLE — metadata only). yt-dlp stays only as the warm-daemon fallback. |
| **JioSaavn** | **jsvn-tgvc proxy, one `/song` call** (0.32 s measured) — or search + parallel `/song` enrichment (0.51 s). Clear URLs, no crypto. | TGVCBot `apps/tgvcbot/src/providers/jiosaavn.ts:80` `GET https://jsvn-tgvc.vercel.app/search?query=` / `:92` `GET …/song?id=`; stream = `media_urls['96_KBPS'] ?? media_url` there — **but see §3: the port must select `320_KBPS`, not 96**. vc-ts `services/vc-ts/src/resolvers.js` (`searchJioSaavn` / `resolveJioSaavnSong` / `jiosaavnTrack`) is the in-tree implementation of exactly this. |
| **Deezer** | **Native ARL gw API + Blowfish-CBC** (in-process; FLAC format 9 first, then MP3_320, then MP3_128). No faster ref exists — every ref goes through the same gw API; the win is in-process vs subprocess. | `services/downloaders/src/deezer.py:219` `_FORMAT_NUMBERS = {MP3_128: 1, MP3_320: 3, FLAC: 9}`; `:268` `_gw_call`; `:294` `_song_get_data`; `:357` `_blowfish_key` (deemix MD5(track_id) XOR-fold) / `:368` decryptChunk (Blowfish-CBC, IV `00 01..07`); `:40-42` format ladder `FLAC → MP3_320 → MP3_128`. Requires ARL cookie + Cryptodome. |
| **NetEase** | **Native weapi client with MUSIC_U cookie** (in-process AES-CBC+RSA); search + `SongDetailAndURL` in one client, lossless when the level allows. | Music163bot-Go `internal/netease/netease.go:22` `New(musicU, level, …)` / `:38` `SongDetailAndURL` / `:90` `SearchSong` (via `github.com/XiaoMengXinX/Music163Api-Go/api`); in-tree port source `services/netease-go/internal/netease/weapi.go:114` `weapiEncryptParams` / `:129` `weapiPost` (+ `eapi.go`, `linuxapi.go`). CDN host rewrite `m8→m7` etc. in `internal/fetch/fetch.go` (`hostReplacer`). |
| **Spotify** | **Client-credentials metadata (1 POST) + audio via the YouTube ladder** — never shell out to spotdl for a resolve. | vc-rust `services/vc-rust/src/main.rs` `spotify_client_token` (token cached to expiry) + `resolve_spotify_track` (`GET https://api.spotify.com/v1/tracks/<id>`); vc-ts `services/vc-ts/src/resolvers.js` `spotifyClientToken` / `spotifySearchTracks`. Ref: lemonic-telegram-bot `modules/spotify.ts` (same client-credentials flow); Spotify-Loader `bot.py:146,358` shells out to `spotdl '<query|url>'` — that is the *slow* path (subprocess + YouTube search inside spotdl), kept only as a download fallback, not a resolve strategy. Downloaders `services/downloaders/src/spotify.py` documents the no-creds fallbacks (public embed page, oEmbed; yt-dlp flat Spotify extractor now answers `[DRM]`, proven 2026-11-02). |
| **Navidrome** | **Native Subsonic REST** — `rest/search3` then `rest/stream` (or `rest/download` for the original file, no transcode). One authenticated HTTP call each; no crypto beyond Subsonic auth. | music-bot Navidrome `refs/music-bot-navidrome/src/navidrome.py:177` `search` (`rest/search3`) / `:104` `stream_song` (`rest/stream`) / `:126` `download_song` (`rest/download`) / `:41` `_auth_params`; in-tree `services/downloaders/src/navidrome.py:62` `_call` (`/rest/<path>?…`), `rest/search2`/`search3` for lookup. |
| **Soulseek** | **slskd HTTP API v0** — `POST /api/v0/searches`, poll `GET …/responses`, `POST /api/v0/transfers/downloads/<user>`; peers ranked by free upload slots, speed, `.flac` first. No faster path exists (the network *is* the search). | `services/downloaders/src/soulseek/client.py:8-15` (endpoints verified against slskd-api 0.2.4) + `scorer.py`/`selection.py` (peer ranking); ref `refs/music-bot-navidrome/src/soulseek/` (same slskd shape). |
| **TikTok** | **yt-dlp `-J --no-playlist` metadata, then a native HTTP fetch of the CDN audio** (yt-dlp's own downloader stalls on TikTok CDN reads, so bytes come from a direct fetch; yt-dlp download is the fallback). | `services/downloaders/src/tiktok.py:8-16` (from telegram-music-bot-rust `main.rs::download_media` / `get_search_results`; TikTok URLs skip the `youtube:player_client=android` extractor arg). No ref beats this — TikTok has no public audio API. |
| **Radio** | **No resolution at all — the URL is the stream.** Fastest possible: 108 µs median measured (§1). | vc-ts `services/vc-ts/src/resolvers.js` `radioTrackFromUrl` (SHA-1 id, duration 0, `is_live: true`); ref openradiobot `refs/openradiobot/index.js` (`openradio` player takes the raw stream URL). Reachability is a separate concern: radio-js `services/radio-js` (10 s ffprobe probe → native Go verifier in Phase 3), not part of the resolve. |

---

## §3. JioSaavn strategy, re-derived from live behaviour

**DES-ECB does not exist in this path.** Re-confirmed three ways:
(a) precise grep for `DES` / `createDecipheriv` / `createCipheriv` /
`CryptoJS` / `decrypt(` / `blowfish` over
`refs/lemonic-telegram-bot/modules|handlers|services` and
`refs/TGVCBot/apps/tgvcbot/src` — **zero matches**; (b) the live `/song`
response contains clear-text CDN URLs (below); (c) vc-ts `resolvers.js`
contains no crypto for JioSaavn at all. (DES-ECB with a known key is a
public JioSaavn technique in the abstract; it is simply not what these
refs — or this codebase — do. Do not write it into the port.)

**Actual mechanism** (proven live from the sandbox, 2026-11-03;
raw payloads `/tmp/jsvn-search.json`, `/tmp/jsvn-song.json` — Kesariya,
`id=rjkrTnma`):

1. Search: `GET https://jsvn-tgvc.vercel.app/search?query=<url-encoded query>`
   (TGVCBot `jiosaavn.ts:80`; spaces become `+` there, `%20` via
   `encodeURIComponent` in vc-ts — both accepted). Live: HTTP 200 in
   1.58 s. Response: `{status: true, serverTime, searchQuery,
   results: [{id, title, image, images: {50x50, 150x150, 500x500},
   album, description, more_info: {vlink, singers, language,
   album_id}, perma_url, api_url: {song, album}}]}`. **No duration
   and no media URL in search results** — that is why vc-ts enriches
   each result with a `/song` lookup.
2. Song: `GET https://jsvn-tgvc.vercel.app/song?id=<id>` (TGVCBot
   `jiosaavn.ts:92`). Live: HTTP 200 in 0.72 s for `id=rjkrTnma`
   (Kesariya). Response fields: `song`, `singers` (empty in this
   response; `primary_artists` is the fallback), `duration: "4:28"`
   — **a `"m:ss"` clock string, not seconds** (vc-ts `parseDuration`
   handles `m:ss` / `h:mm:ss` / seconds; the old `Number("4:28")`
   bug returned `NaN → 0`), `has_lyrics: true, lyrics: null`
   (JioSaavn cannot supply lyric text), `perma_url`,
   `media_url: https://aac.saavncdn.com/871/…_160.mp4`,
   `media_urls: {96_KBPS, 160_KBPS, 320_KBPS}` — all clear
   `https://aac.saavncdn.com/871/…_<bitrate>.mp4` URLs.
3. Stream URL = `media_urls[<quality>]` — a plain AAC CDN URL.
   **No decryption step exists anywhere in the chain.**
4. Fallback base `https://saavn.dev/api` (`/search/songs?query=`,
   `/songs/<id>`) is **unreachable from the sandbox** (HTTP 000,
   connection failure, 0.73 s) — vc-ts tries it second and it fails
   here every time; on the user's host it may answer, but nothing in
   this benchmark depended on it.
5. **The stream URLs are short-lived signed URLs.** A `/song` call
   returns a fresh token each time (`…_96.mp4` token differed between
   the 07:57 probe and the 08:01 bench run), and a URL fetched ~2 min
   earlier 404s on `aac.saavncdn.com` (HEAD/GET, with and without
   browser UA + Referer; the JioTune preview host
   `jiotunepreview.jio.com` from the same payload answers 200, so
   this is URL expiry, not egress blocking). Consequence for Phase 4:
   JioSaavn URLs must be used promptly after resolve (or re-resolved);
   a long TTL cache will serve dead URLs.
6. **Quality finding — the current code takes the lowest quality.**
   vc-ts `jiosaavnTrack` / `resolveJioSaavnSong` select
   `media_urls["96_KBPS"] || media_url` (TGVCBot's exact shape:
   `media_urls['96_KBPS'] ?? media_url`), i.e. 96 kbps, while the
   same payload carries a `320_KBPS` URL. The locked §1.3 Tier 2
   requirement is "320 kbps stream selection" — the Go backend port
   **must select `320_KBPS` (fallback `160_KBPS`, then `media_url`)**,
   not copy the 96 kbps selection.

---

## §4. Measured core stage timings (Worker A)

### `t_dispatch_to_ipc` — command receipt → engine IPC

Receipt is stamped at the top of `Dispatcher::handleMessage` and carried
on the routing `CommandContext`. Engine-play path: `dispatch_to_post_us`
= receipt → `vcCall` entry, `post_to_response_us` = `vcCall` entry →
response; `dispatch_to_post_us + post_to_response_us == total` (asserted
by test). Replied-file `/play` and usage-error `/play` have no engine IPC
post, so their lines carry `ipc=none`.

Verbatim lines from the stubbed test run (stubbed engine answers
instantly, so these magnitudes are **stub magnitudes, structure is
real** — real magnitudes come from the live path):

```
[telemetry] t_dispatch_to_ipc_us=784 dispatch_to_post_us=712 post_to_response_us=72 action=play chat_id=-1051 engine=go fell_back=0 ok=1
[telemetry] t_dispatch_to_ipc_us=279 ipc=none action=play chat_id=-1003 ok=0 error=no_query
[telemetry] t_dispatch_to_ipc_us=4617 ipc=none action=play chat_id=-1001 ok=1
```

712 µs of dispatch work before the post, 72 µs for the stub round-trip.
On the live path, `post_to_response_us` will carry the real engine
resolve time — YouTube 5.7–6.7 s (§1) is the current expectation for
that field.

### `t_voice_join` — join stages

Stages in execution order: `authorize_us create_call_us get_chat_us
[create_video_chat_us] join_video_chat_us connect_us`, plus `chat_id
backend=tdlib ok=1|0` and, on failure, `failed_stage=` + exact `error=`.
`authorize_us=0` means "already authorized, no authorize call needed".
Already-joined chats emit nothing — no join is attempted.

Verbatim line from a mock join (scripted TDLib, **real** libntgcalls.so
offer + **real** `ntg.connect` rejecting the mock payload):

```
[telemetry] t_voice_join_us=41288 authorize_us=0 create_call_us=37763 get_chat_us=75 join_video_chat_us=82 connect_us=3343 chat_id=-1001234567890 backend=tdlib ok=0 failed_stage=connect error=ntgcalls_connect_failed:_ntgcalls_connect_failed_(-102):_Invalid_JSON
```

What that one line already says about the live path: the local SDP offer
(`create_call`) dominates at ~37.8 ms; the scripted TDLib round-trips are
~75–82 µs each; the real `connect` fails in ~3.3 ms on a bad payload.
Total (41,288 µs) = stage sum (41,263 µs) + 25 µs of inter-stage time.
**No successful real voice join has been measured** — see §8.

### `t_first_frame` — ffmpeg Opus/RTP pipeline (legacy pipeline)

Encode leg: the same ffmpeg invocation as before, but the Ogg output now
goes to a pipe (teed byte-for-byte into `opus_out`, so the file and the
ffprobe verification are unchanged) and Ogg pages are parsed incrementally
as bytes arrive. The first page with `granule_position > 0` is the first
20 ms Opus packet leaving the encoder. Fields: `resolve_us` (yt-dlp `-g`,
URL inputs only), `stream_open_us` (run start → first output byte =
headers written), `first_packet_us` (run start → first Ogg audio page —
the headline `t_first_frame_us` value), `encode_total_us`, `rtp_us` +
`rtp_ok`, `frames`, `bytes`, `ok`, `error` / `rtp_error`. The RTP leg is
unchanged (separate ffmpeg `-f rtp` run, same command) and reports its
wall time + ok flag. **The sender cannot observe a UDP packet on the
wire, so no RTP first-packet time is fabricated.**

Verbatim line from the sandbox run (5 s 440 Hz tone, loopback RTP sink):

```
[telemetry] t_first_frame_us=121528 stream_open_us=98714 first_packet_us=121528 encode_total_us=159593 rtp_us=100438 rtp_ok=0 frames=251 bytes=83736 ok=1 rtp_error=ffmpeg_rtp_output_failed_(exit_255)_—_UDP_may_be_blocked_in_this_sandbox
```

251 frames / 83,736 bytes matches the README's standalone opus proof
exactly. First packet at ~122 ms, encode done at ~160 ms (startup
dominates; the encode itself runs at ~30× realtime). Probe runs ranged
**89–472 ms** for first audio page (startup-dominated, varies with load).
Re-run during Phase 1 verification reproduced the same shape:
`t_first_frame_us=114654 stream_open_us=88133 first_packet_us=114654
encode_total_us=156513 rtp_us=109214 rtp_ok=0 frames=251 bytes=83736 ok=1`
— identical frame/byte counts, first packet 115 ms.

---

## §5. Appendix A latency table — estimates replaced by measured values

FINAL-ARCHITECTURE.md Appendix A (locked §4 table) as corrected by
Phase 1 (§6-R7: the table stood as design targets, not facts). Every
"estimate" below is replaced by a measured value wherever Phase 1
measured one; what remains an estimate is labelled **ESTIMATE** with its
source.

| Pipeline stage | Current (d5065b9) | New (cold, uncached) | New (cached / prefetched) |
|---|---|---|---|
| Telegram update receipt | HTTPS Bot API long-poll (30 s) — code fact (`main.cpp:187`) | TDLib MTProto push — **ESTIMATE** (no live join measured yet; §8) | TDLib MTProto push — **ESTIMATE** |
| Core → backend IPC | curl CLI fork + TCP: ~25 ms — **ESTIMATE, unmeasured** (locked-table figure; Phase 1 measured only a stub round-trip: `post_to_response_us=72 µs` against an instant stub, §4 — that is not a real engine call and not a curl measurement) | Persistent UDS socket: ~0.08 ms — **ESTIMATE** (locked-table figure; no UDS transport exists yet — Phase 2 work) | Persistent UDS socket: ~0.08 ms — **ESTIMATE** |
| Track and stream resolution | yt-dlp fork: **5.69–5.88 s median (YouTube URL), 6.39–6.66 s (YouTube search), 0.51 s (JioSaavn search+enrich), 0.32 s (JioSaavn single lookup), 108 µs (radio)** — measured §1. Old Phase 4 range 6.5–13.7 s (YouTube) / 0.5–0.6 s (JioSaavn) still stands as the wider historical range. | Go native InnerTube: **0.61–0.81 s (player) / 1.56 s (search)** — measured §1 (direct probes, same video). Native resolvers for other sources: **unmeasured** (§1 blockers). Warm yt-dlp daemon fallback ~900 ms — **ESTIMATE, and WRONG for the current code**: cold yt-dlp is ~5.9 s and no warm daemon exists yet (that is Phase 3 work); 900 ms is a target, not a measurement. | Embedded URL cache hit: ~0.05 ms — **ESTIMATE** (locked-table figure; no cache exists yet — Phase 4 work) |
| Decoder and resampler init | ffmpeg subprocess fork — code fact | In-process symphonia HTTP range open: 150–300 ms — **ESTIMATE** (locked-table figure; no symphonia in the tree yet — Phase 5 work). Adjacent measured number, legacy pipeline: ffmpeg Opus **first packet 89–472 ms** (probe range), 122 ms in the recorded run (§4) — startup-dominated, not a symphonia measurement. | Pre-buffered in Slot B: 0 ms — **target, unmeasured** (Phase 5) |
| Track advance delay | 5,000 ms polling watcher — code fact (`dispatcher.cpp:2033`) | Sample-accurate callback: 0 ms — **target, unmeasured** (Phase 5) | Sample-accurate callback: 0 ms — **target, unmeasured** (Phase 5) |

**Verdicts on the locked estimates (from Worker B, §6):**

- "Go InnerTube 120–450 ms" is **plausible but not yet met by any
  engine**: measured native InnerTube is 0.61–0.81 s (player) / 1.56 s
  (search) from this sandbox — slower than the estimate, still 7–10×
  faster than yt-dlp's 5.7–6.7 s.
- "Warm yt-dlp ~900 ms" is **wrong for the current code**: cold yt-dlp
  is ~5.9 s and there is no warm daemon yet (Phase 3).
- The locked Phase 6 cold-`/play` targets, judged against measured
  values: YouTube <1–3 s is **unreachable with yt-dlp** (5.7–6.7 s
  measured) and **reachable only with native InnerTube** (0.61–1.56 s
  measured). JioSaavn <200 ms is **not met** (0.32 s single lookup /
  0.51 s search, measured) — needs the Phase 4 cache or a single-call
  path. Cached/prefetched ~0 ms is **unmeasured** (no cache exists yet).

---

## §6. Phase 1 verification (2026-11-03, this rewrite)

Rebuilt and re-run by Worker C; nothing committed or pushed (coordinator
owns that). `.env` untouched; no user token used; all runs on dummy creds
only.

- **core-cpp:** clean CMake rebuild (`-Wall -Wextra -Wpedantic`), **0
  warnings** in the `parlay-core` build. Test suites (built per the
  documented `g++ -std=c++20 -I src` lines, also with `-Wall -Wextra
  -Wpedantic`): dispatcher_test **234 passed / 0 failed** (228 baseline
  + 6 new); tdlib_test **51 / 0** (44 + 7); telemetry_test (new)
  **19 / 0**; ui_test **159 / 0**; router_test **20 / 0**;
  telegram_api_test **73 / 0**; http_api_test **71 / 0**; scram_test
  **40 / 0** (2 live sections skipped — no mongod running); store_test
  **19 passed / 2 failed** — the same 2 pre-existing environmental
  failures as the Phase 9 baseline (`mongo connect: cannot connect to
  mongodb at 127.0.0.1:27099`; `store backend=mongo (got file)`), redis
  tests skipped (not up); `store.cpp`/`scram.cpp` untouched by the
  Phase 1 diff. Total: **686 passed / 0 code failures** (Worker A's
  "667" counts the same suites excluding store_test's 19 passes).
  One pre-existing test-only nit, **not** introduced by Phase 1:
  `tests/tdlib_test.cpp:682` unused variable `fp` (`-Wunused-variable`)
  — present at `d5065b9` in the same block, left untouched.
- **vc-ts:** `node --test tests/run.js tests/store.js` — **61 passed /
  0 failed** (matches Phase 9 61/61).
- **radio-js:** same command — **22 passed / 0 failed** (matches Phase 9
  22/22).
- **vc-go:** `go test ./...` (Go 1.23.0,
  `~/workspace/parlay/.tools/go/bin/go`) — **all packages ok: 37 passed /
  0 failed / 5 skipped** (live tests skipped without
  `PARLAY_LIVE_TEST=1`, as designed; Redis store test skipped).
- **vc-rust:** `cargo test` (cargo 1.99.1, `~/.cargo/bin`) — **59 passed /
  0 failed** (incl. `youtube_url_resolve_is_real_or_honest_error`,
  `bare_youtube_id_resolve_is_real_or_honest_error`,
  `free_text_query_never_returns_a_fake_track`, Opus/RTP pipeline
  tests). Go/Rust suites **do run** in the sandbox (contrary to the
  Phase 7 note — toolchains are present at
  `~/workspace/parlay/.tools/go/bin` and `~/.cargo/bin`).
- **Phase 1 diff check:** `git diff` = 10 files, +803/−78, plus 2 new
  files (`services/core-cpp/src/telemetry.hpp`,
  `services/core-cpp/tests/telemetry_test.cpp`) and `phase1/` docs.
  **No TODO / FIXME / HACK / NotImplemented in any added or changed
  line or in the new files.** The diff touches instrumentation + tests +
  docs only. The deletions are refactors, not feature removals:
  `TdLibVoiceCall::join`/`::play` join sequences factored into one
  `joinCall()` (same steps, same error strings); `OpusPipeline::run`
  rewritten with pipe-based Ogg parsing (same ffmpeg invocation, same
  file output + ffprobe verification); `playRepliedFile` void → bool
  (only caller `onPlay`); engine diffs are timed wrappers only
  (`Resolve` → `resolve`, `resolve_query` → `resolve_query_inner`,
  `resolvePlayQuery` → `resolvePlayQueryUntimed`). No IPC contract
  change (`contracts/ipc.json` untouched), no behaviour change, no
  live code deleted. **No regressions vs the Phase 9 / Phase 7
  baselines.**

---

## §7. Older measured numbers still valid (Phase 4, 2026-11-02)

Not re-measured in Phase 1; kept because nothing in Phase 1 invalidates
them. Sources: `phase4/bench-core-cpp.md`, `phase4/bench-go.md`,
`phase4/bench-tsjs.md`, `phase4/bench-rustpy.md`. All sizes are real
files; all times wall-clock. Environment: sandbox, 4 cores, 7 GB RAM,
g++ 13, cmake 3.28.3, Go 1.23.0, rustc 1.99.1, Node v24.22.1,
ffmpeg 8.1.2, yt-dlp 2026.10.26.

### Build time + binary size

| Service | Clean build | Incremental | Binary (stripped) | Notes |
|---|---|---|---|---|
| core-cpp | 35.1 s (`-j4`, 10 TUs) | 0.13 s no-op; 14.4 s `dispatcher.cpp` (3,199 lines) | 1,002,808 B (866,656 stripped) | 0 errors, 8 warnings — all `-Wmaybe-uninitialized` false positives inside libstdc++ `variant`/`map`; zero warnings in own code. Configure 0.3 s. |
| vc-go | 92.4 s clean (Mongo driver compile dominates; warm 8.1 s) | ~8 s | 9,257,220 B stripped (13,476,313 unstripped) | stdlib-only build was a few seconds before the driver; driver deps fetched once into module cache. |
| netease-go | seconds (stdlib only, `go vet`/`build` clean) | seconds | — | Crypto port adds no deps. |
| vc-rust | 9.2 s | 9.3 s | 821,336 B | std-only, zero crates; `target/` 2.1 MB. |
| spotify-rust | 8.1 s | 6.2 s | 783,232 B | std-only, zero crates; `target/` 2.0 MB. |
| audio-fx-rust / share-rust / queue-rust | single-digit s (release, offline) | — | < 1 MB each | std-only, zero crates. |
| vc-ts / radio-js / gui-js | **no build step** (plain Node stdlib, `node --check` clean, zero npm installs) | — | — | Startup is process-spawn time only. |

Optimisations applied in Phase 4: release builds everywhere
(`-O2` C++, `cargo --release`, `-trimpath -ldflags='-s -w'` Go),
stripped binaries, parallel builds (`-j4` cmake, `-j2` TDLib on
7 GB RAM). Before/after: vc-go clean build went from seconds
(stdlib-only) to 92.4 s with the Mongo driver — the cost of a real
driver; warm rebuilds stay at 8.1 s. core-cpp clean build 35.1 s is
dominated by the 3,199-line dispatcher (14.4 s alone); splitting it
per command group would cut rebuild time but was left alone — one
file keeps the NoxBeats route table readable.

### Startup to healthz

| Service | Startup |
|---|---|
| core-cpp | exit 1 in 0.00 s with missing env (validation before any network/thread work) |
| vc-go | 0.03 s (Mongo backend, low-ram) |
| vc-ts | 0.4–1.4 s |
| radio-js | 2.9–3.6 s (6 station probes in parallel at boot) |
| gui-js | 1.1–3.0 s (first run inflated by sequential polling) |
| vc-rust / spotify-rust / audio-fx / share / queue | < 1 s (process spawn) |
| netease-go / downloaders | < 2 s |

### Latency (live) — not re-measured in Phase 1

| Operation | Latency | Notes |
|---|---|---|
| `/play` YouTube URL (vc-ts, live yt-dlp) | 6.5–13.7 s | network variance, same input; resolution dominates, not the server. Reconciled with §1: new medians 5.69–5.88 s sit at the fast end of this range. |
| `/play` YouTube URL (vc-go, degraded, no yt-dlp) | < 100 ms | pattern resolve; live resolve needs yt-dlp installed |
| `/play` JioSaavn (vc-ts, queued) | 0.5–0.6 s | search + 1 duration enrichment. Reproduced by §1: 0.51 s median. |
| `/v1/search` JioSaavn, limit 3 (vc-ts) | ~2.0 s | incl. 3 parallel `/song` duration enrichments (~1.5 s per search — the price of real durations, paid once) |
| `/v1/panel`, `/v1/now-playing` (vc-ts) | < 25 ms | local endpoints |
| `/v1/lyrics` (LRCLib) | 1.0–1.9 s | retries once on 503 |
| radio start (ffprobe + VC handoff) | 0.9–1.8 s | ffprobe probe dominates |
| radio stop (with VC clear) | 181 ms | |
| gui page load | 2–11 ms (15 KB) | stream-url ~0.5–0.8 s (one VC round-trip); audio proxy 15–176 ms (5 KB tone, byte-exact) |
| vc-rust `/v1/play` YouTube (mp3 download) | 17.7 s | 7,045,581 B mp3; ~400 KB/s incl. extract + transcode. Superseded as a wall-clock range by §1 (27.1–49.3 s across 3 URL reps) — same shape, wider variance; the 17.7 s figure was a single earlier run. |
| vc-rust `/autoplay-skip` end-to-end | 57.8 s | mix fetch 32–44 s (YouTube-side) + download ~15 s |

### Download throughput (live)

| Path | File | Time |
|---|---|---|
| downloaders YouTube (Rick Astley) | 7,045,581 B mp3 | ~36 s end-to-end over HTTP (incl. ytsearch resolve) |
| downloaders Spotify (same track, fallback) | 7,045,581 B mp3 | 35.9 s (embed resolve + YouTube match + download) |
| TikTok direct CDN | 711,792 B mp3 | 2.2 s for the bytes; 6.5 s over HTTP incl. metadata (yt-dlp's own downloader stalls on reads from this egress — plain GET works) |
| Navidrome (demo server) | 5,365,035 B mp3 | 4.3 s (~1.2 MB/s) |
| Deezer preview | 479,827 B mp3 (30 s) | 5.8 s |
| Deezer album ZIP (14 previews) | 6,696,088 B | part of 110 s live suite |
| NetEase download (mirror) | 34,918,981 B FLAC (294 s) | live; also 27.4 MB + 22.2 MB FLACs; 64.6 MB file correctly refused by the 50 MB cap |
| Spotify playlist resolve (no creds) | 50-track list via `__NEXT_DATA__` embed | ~2 s |

### What the numbers say

- **/play latency is resolution, not server.** YouTube via yt-dlp
  6.5–13.7 s (Phase 4 range; Phase 1 medians 5.7–6.7 s); JioSaavn ~0.5 s
  queued; local endpoints < 25 ms.
- **YouTube is the slow path** (~18–36 s/track) — extraction +
  transcode, not bandwidth. TikTok/Navidrome/Deezer previews are
  2–6 s (finished file, no transcode).
- **Rust services are tiny and fast** (single-digit-second builds,
  < 1 MB binaries) because they are std-only; cost is all in
  subprocess calls (yt-dlp, curl, ffmpeg) and network.
- **The Mongo driver is vc-go's only build cliff** (92.4 s clean
  vs seconds stdlib-only); warm rebuilds are 8.1 s.
- **Autoplay mix fetch (32–44 s) dominates `/autoplay-skip`** —
  a YouTube-side cost, not a service cost.

---

## §8. What could not be measured live in the sandbox (honest gaps)

From Worker A (core) and Worker B (engines); none of these was faked:

- **A successful real voice join:** no session dir, no phone/QR
  first-auth, no direct TLS to Telegram DCs from the sandbox. The join
  code exists and is correct; every stage up to `connect` is proven
  against the real library (§4), and a failure at any stage emits the
  stage + exact reason. The first real `t_voice_join ok=1` line can only
  come from the user's VPS with a real assistant session.
- **A successful RTP transmission / first RTP packet on the wire:** UDP
  `sendto` is EPERM in this sandbox, even to loopback (README's opus
  proof records the same). The RTP leg reports `rtp_ok=0`, exit 255,
  `Operation not permitted`, and the file encode is unaffected. The
  first-packet time that *is* reported is the encode-side one (§4).
- **URL resolution (`resolve_us` in `t_first_frame`):** yt-dlp is not
  installed in the sandbox (Worker A ran without it), so `resolveUrl`
  fails fast for http(s) inputs; local files skip resolution (field
  omitted). The timing hook is in place for hosts with yt-dlp.
- **Real `/play` magnitudes (engine resolve times, real
  dispatch→post):** need the live VPS + real engines. The timer
  plumbing is in place; §1 is where those numbers land.
- **Deezer / NetEase / Navidrome / Soulseek / TikTok resolves:** blocked
  per §1 (no ARL cookie, no `MUSIC_U`, no Navidrome server/creds, slskd
  not running, no TikTok URL/engine path). Strategies are locked from
  code + refs only (§2), with no measured time attached.
- **Spotify resolve:** blocked (no creds; token endpoint refused by the
  egress proxy). Time-to-honest-failure 8,321 µs only (§1).
- **saavn.dev fallback:** unreachable from the sandbox (HTTP 000); the
  jsvn-tgvc proxy is the measured path (§3).
- **UDS round-trip, cache-hit time, symphonia open time, sample-accurate
  track advance:** do not exist yet (Phases 2/4/5 work); their Appendix A
  cells stay labelled **ESTIMATE** until measured (§5).
