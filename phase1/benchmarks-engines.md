# Phase 1 — Engine timers + head-to-head benchmark (Worker B)

Date: 2026-11-03 (runs 07:56–08:05 IST). Anchor: `d5065b9` (Phase 9).
Scope: FINAL-ARCHITECTURE.md §9 Phase 1, §5, §10.1. No behaviour change,
no IPC contract change, no code deleted, no Phase 2/3 work.
`.env` untouched; no user token used anywhere; all local runs on
localhost with dummy creds only (no `PARLAY_TOKEN`, no Spotify creds,
no Deezer ARL, no NetEase `MUSIC_U`, no slskd login).

Method: the three engines were run locally on distinct ports
(vc-go :18001, vc-ts :18002, vc-rust :18003) with JSON-file state in
`/tmp/phase1-bench/{go,ts,rust}` (nothing written into the repo tree;
one stray vc-ts download dir from the first harness run was removed
before the re-run — see §7). Each engine's real `POST /v1/play`
resolve path was driven with `curl` (fresh `chat_id` per call, 120 s
cap). `t_resolve` is read off the `[telemetry]` line each engine
emits; the HTTP wall-clock (`time_total`) is recorded alongside it
because for vc-rust the two differ sharply (§7).

Toolchains (all present; none installed by this task):
Go 1.23.0 (`~/workspace/parlay/.tools/go/bin/go`), cargo/rustc 1.99.1
(`~/.cargo/bin`), Node v24.22.1, yt-dlp 2026.10.26
(`services/downloaders/.venv/bin/yt-dlp`).

Representative queries: YouTube URL
`https://www.youtube.com/watch?v=dQw4w9WgXcQ`; YouTube search
`never gonna give you up rick astley`; JioSaavn `jiosaavn:Kesariya`
and `jsvn:rjkrTnma` (id taken from the search result); Radio
`radio:https://ice1.somafm.com/groovesalad-128-mp3`; Spotify
`https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC`.

---

## 1. Timers added (t_resolve)

One structured line per resolve attempt (success **and** failure),
same format in all three engines — and the same shape as the core
worker's `services/core-cpp/src/telemetry.hpp`
(`[telemetry] <name>_us=<integer> [key=value ...]`):

```
[telemetry] t_resolve_us=<integer> source=<source> engine=<go|ts|rust>
```

`t_resolve` covers query/URL input → playable media URL + metadata,
measured on a monotonic clock (Go `time.Since` / `Instant`-equivalent,
Node `process.hrtime.bigint()`, Rust `std::time::Instant`), in
microseconds. The source label comes from the returned track on
success, and from the query shape (same dispatch order as the
resolver) on failure. Pure wrappers — the resolve bodies are
byte-identical, only renamed (`resolve` / `resolvePlayQueryUntimed` /
`resolve_query_inner`); no behaviour change.

| Engine | File(s) | Entry point timed |
|---|---|---|
| vc-go | `services/vc-go/internal/resolver/resolver.go` | `Resolver.Resolve` (YouTube only; `source` is always `youtube` here) |
| vc-ts | `services/vc-ts/src/resolvers.js` (helpers `telemetryNowNs` / `telemetryLogResolve`) + `services/vc-ts/src/main.js` (wrapper) | `resolvePlayQuery` (the `/v1/play` dispatcher: radio / jiosaavn / youtube). Search endpoints (`/v1/search`, Lemonic) are **not** timed — a search returns metadata lists, not a playable stream URL |
| vc-rust | `services/vc-rust/src/main.rs` | `resolve_query` (YouTube URL/ID → yt-dlp; Spotify URL → Web API; else yt-dlp search). Note: a Spotify resolve returns metadata only, `stream_url: None` |

Diff: 4 files, +112/−1 (`git diff --stat`). Verified live: every
benchmark row below produced exactly one `[telemetry]` line in the
corresponding engine log (raw logs: `/tmp/phase1-bench/vc-{go,ts,rust}.log`;
raw results: `/tmp/phase1-bench/results.txt`).

---

## 2. Measured table — per-source × per-engine t_resolve (microseconds)

Medians of the repeated runs; all reps listed. "n/a" means the engine
has no resolver for that source (not a failed measurement).

### YouTube — URL (`https://www.youtube.com/watch?v=dQw4w9WgXcQ`)

| Engine | rep1 | rep2 | rep3 | **Median** | HTTP wall-clock (median) |
|---|---|---|---|---|---|
| vc-go | 5,632,422 | 5,960,353 | 5,875,831 | **5,875,831 µs (5.88 s)** | 5.88 s |
| vc-ts | 6,483,570 | 5,693,403 | 5,354,212 | **5,693,403 µs (5.69 s)** | 5.73 s |
| vc-rust | 5,316,676 | 6,247,918 | 5,857,190 | **5,857,190 µs (5.86 s)** | 29.38 s (§7) |

All three return the same real track (Rick Astley, 213 s) with a
signed `googlevideo.com/videoplayback?expire=…` stream URL. The three
engines are statistically identical here: every one shells out to the
same yt-dlp (`-j -f bestaudio/best --no-playlist`, android retry on
bot-check), so the ~5.3–6.5 s is yt-dlp + YouTube, not engine code.
No engine wins; the subprocess is the bottleneck.

### YouTube — free-text search (`never gonna give you up rick astley`)

| Engine | rep1 | rep2 | rep3 | **Median** | HTTP wall-clock (median) |
|---|---|---|---|---|---|
| vc-go | 6,660,718 | 5,668,585 | 7,374,034 | **6,660,718 µs (6.66 s)** | 6.66 s |
| vc-ts | 7,521,034 | 6,511,333 | 6,092,429 | **6,511,333 µs (6.51 s)** | 6.52 s |
| vc-rust | 6,341,906 | 6,683,776 | 6,386,910 | **6,386,910 µs (6.39 s)** | 38.09 s (§7) |

Same conclusion: `ytsearch1:` via yt-dlp, ~6.4–6.7 s on all three.
Footnote: vc-go rep1 resolved a *different* video than the other runs
(`yt:3BFTio5296w`, "Never Gonna Give You Up (2022 Remaster)") while
ts/rust reps resolved `dQw4w9WgXcQ` — `ytsearch1:` top-hit
nondeterminism across runs, not an engine difference.

### JioSaavn

| Engine | Input | rep1 | rep2 | rep3 | **Median** |
|---|---|---|---|---|---|
| vc-ts | `jiosaavn:Kesariya` (search + parallel `/song` enrichment, 1 result) | 991,686 | 478,444 | 512,810 | **512,810 µs (0.51 s)** |
| vc-ts | `jsvn:rjkrTnma` (single `/song` lookup) | 316,407 | 239,237 | 338,619 | **316,407 µs (0.32 s)** |
| vc-go | — | n/a — no JioSaavn lane | | | |
| vc-rust | — | n/a — no JioSaavn lane | | | |

Both resolve to the real track (Kesariya, 268 s, `duration_str`
`4:28`, stream URL `https://aac.saavncdn.com/871/…_96.mp4` — see §5
for why it is `_96`). Rep1 of the search (991,686 µs) is a cold
first-call outlier (proxy/connection warm-up); reps 2–3 are ~0.5 s.
Meets the locked Phase 6 cold-`/play` target (<200 ms is **not** met —
0.51 s for search+enrich, 0.32 s for a bare song lookup; the
`<200 ms` target needs the Phase 4 cache or a faster single-call
path).

### Radio

| Engine | Input | rep1 | rep2 | rep3 | rep4 | rep5 | **Median** |
|---|---|---|---|---|---|---|---|
| vc-ts | `radio:https://ice1.somafm.com/groovesalad-128-mp3` | 8,217 | 116 | 96 | 108 | 95 | **108 µs** |
| vc-go | — | n/a — no radio lane | | | | | |
| vc-rust | — | n/a — no radio lane | | | | | |

No network I/O at all: the URL *is* the stream
(`radioTrackFromUrl` builds the track from the URL + a SHA-1 id,
duration 0, `is_live: true`). Rep1 (8,217 µs) is first-call warm-up.
Caveat: the SomaFM stream itself was **not** fetched or verified —
the engine never checks reachability on this path (that is the
radio-js service's ffprobe job, not the resolve).

### Spotify

| Engine | Input | t_resolve | Outcome |
|---|---|---|---|
| vc-rust | `https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC` | **8,321 µs** | HTTP 502 `engine_unavailable`: `Spotify track cannot be resolved: SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET are not set (client-credentials flow)` |
| vc-go | — | n/a — no Spotify lane | |
| vc-ts | — | n/a on the `/v1/play` path (Spotify exists only as a *search* fallback via Lemonic/`searchLemonic`, not as a play lane) | |

Blocked, exactly: no Spotify creds in the sandbox (by design — dummy
creds only) **and** the egress proxy refuses the token endpoint
(`accounts.spotify.com/api/token` → 502, per the `main.rs` comment;
`api.spotify.com` itself answers). The 8,321 µs is time-to-honest-
failure, not a resolve time. Note vc-rust's Spotify resolve would
return metadata only anyway (`stream_url: None`) — audio comes from
the YouTube ladder.

### Deezer / NetEase / Navidrome / Soulseek / TikTok

| Source | vc-go | vc-ts | vc-rust | Blocker / owner |
|---|---|---|---|---|
| Deezer | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/deezer.py` (ARL + gw API + Blowfish-CBC, §4). Blocked: **no Deezer ARL cookie supplied** (user has not provided one). |
| NetEase | n/a | n/a | n/a | No engine implements it. Owner: `services/netease-go` (weapi/eapi/linuxapi, §4). Blocked: **no `MUSIC_U` cookie**; song-URL path needs a logged-in session. |
| Navidrome | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/navidrome.py` (Subsonic, §4). Blocked: **no Navidrome server URL/creds in the sandbox**. |
| Soulseek | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/soulseek/` (slskd HTTP API, §4). Blocked: **slskd is not running** (`curl 127.0.0.1:5030` → HTTP 000) and no slskd login exists in the sandbox. |
| TikTok | n/a | n/a | n/a | No engine implements it. Owner: `services/downloaders/src/tiktok.py` (yt-dlp `-J` + direct CDN fetch, §4). Not measured live: no representative TikTok URL on hand and no engine path to drive; strategy documented from code only. |

No number in this table is fabricated: every unmeasured cell names
its exact blocker above.

### Reference probes (not engine resolves — fastest-strategy evidence, §4)

Direct InnerTube calls from the sandbox, same video
(`dQw4w9WgXcQ`), public WEB key taken from the refs:

| Call | Result |
|---|---|
| `POST m.youtube.com/youtubei/v1/player` WEB 2.20250101.01.00 | HTTP 200, **0.76 s**, `playability: UNPLAYABLE`, 0 adaptiveFormats (metadata only) |
| `POST …/player` ANDROID 20.10.38 | HTTP 200, **0.81 s**, `playability: OK`, 26 adaptiveFormats — audio itags 139/140/249/251, all with direct URLs |
| `POST …/player` ANDROID_VR 1.62.27 | HTTP 200, **0.61 s**, `playability: OK`, same 26 formats |
| `POST …/player` IOS 19.29.1 | HTTP 400 (stale client version — not retried; version must come from the refs at port time) |
| `POST …/search` WEB, `params=CAASAhAB` | HTTP 200, **1.56 s**, 20 video hits, first hit `dQw4w9WgXcQ` correct |

So a native InnerTube resolve is **~0.6–0.8 s (player) / ~1.6 s
(search)** vs yt-dlp **~5.7–6.7 s** — roughly 7–10× faster, measured
live from this sandbox on the same video.

---

## 3. Fastest resolution strategy per source (verdicts)

Locked for the Go backend port (§9: "lock that exact algorithm").
"Fastest" = fewest round trips / no subprocess, judged on the
mechanism plus the measurements in §2 where they exist.

| Source | Fastest strategy | Exact algorithm / file |
|---|---|---|
| **YouTube** | **Native InnerTube, ANDROID_VR (or ANDROID) `player` + WEB `search`** — one in-process HTTPS POST, no subprocess. Measured 0.61–0.81 s (player) / 1.56 s (search) vs 5.7–6.7 s for yt-dlp. | YukkiMusic-Go `internal/platforms/youtube.go:74` `Get` / `:362` `fetchVideo` / `:479` `callInnerTube` (`POST https://m.youtube.com/youtubei/v1/<search\|player\|browse\|next>?key=<key>`, WEB client 2.20250101.01.00); ParvatiMusic `bot/src/platforms/youtube.rs:237` `call_innertube` (`POST {base}/youtubei/v1/{ep}?key={INNER_TUBE_KEY}`) + `:360` player / `:279` search; ArcMusic `internal/platforms/youtube.go:272` `callInnerTube("search", …)`. Stream URL = adaptiveFormats itag **251** (Opus) / **140** (AAC) from the ANDROID/ANDROID_VR `player` response (WEB is UNPLAYABLE — metadata only). yt-dlp stays only as the warm-daemon fallback. |
| **JioSaavn** | **jsvn-tgvc proxy, one `/song` call** (0.32 s measured) — or search + parallel `/song` enrichment (0.51 s). Clear URLs, no crypto. | TGVCBot `apps/tgvcbot/src/providers/jiosaavn.ts:80` `GET https://jsvn-tgvc.vercel.app/search?query=` / `:92` `GET …/song?id=`; stream = `media_urls['96_KBPS'] ?? media_url` there — **but see §5: the port must select `320_KBPS`, not 96**. vc-ts `services/vc-ts/src/resolvers.js` (`searchJioSaavn` / `resolveJioSaavnSong` / `jiosaavnTrack`) is the in-tree implementation of exactly this. |
| **Deezer** | **Native ARL gw API + Blowfish-CBC** (in-process; FLAC format 9 first, then MP3_320, then MP3_128). No faster ref exists — every ref goes through the same gw API; the win is in-process vs subprocess. | `services/downloaders/src/deezer.py:219` `_FORMAT_NUMBERS = {MP3_128: 1, MP3_320: 3, FLAC: 9}`; `:268` `_gw_call`; `:294` `_song_get_data`; `:357` `_blowfish_key` (deemix MD5(track_id) XOR-fold) / `:368` decryptChunk (Blowfish-CBC, IV `00 01..07`); `:40-42` format ladder `FLAC → MP3_320 → MP3_128`. Requires ARL cookie + Cryptodome. |
| **NetEase** | **Native weapi client with MUSIC_U cookie** (in-process AES-CBC+RSA); search + `SongDetailAndURL` in one client, lossless when the level allows. | Music163bot-Go `internal/netease/netease.go:22` `New(musicU, level, …)` / `:38` `SongDetailAndURL` / `:90` `SearchSong` (via `github.com/XiaoMengXinX/Music163Api-Go/api`); in-tree port source `services/netease-go/internal/netease/weapi.go:114` `weapiEncryptParams` / `:129` `weapiPost` (+ `eapi.go`, `linuxapi.go`). CDN host rewrite `m8→m7` etc. in `internal/fetch/fetch.go` (`hostReplacer`). |
| **Spotify** | **Client-credentials metadata (1 POST) + audio via the YouTube ladder** — never shell out to spotdl for a resolve. | vc-rust `services/vc-rust/src/main.rs` `spotify_client_token` (token cached to expiry) + `resolve_spotify_track` (`GET https://api.spotify.com/v1/tracks/<id>`); vc-ts `services/vc-ts/src/resolvers.js` `spotifyClientToken` / `spotifySearchTracks`. Ref: lemonic-telegram-bot `modules/spotify.ts` (same client-credentials flow); Spotify-Loader `bot.py:146,358` shells out to `spotdl '<query|url>'` — that is the *slow* path (subprocess + YouTube search inside spotdl), kept only as a download fallback, not a resolve strategy. Downloaders `services/downloaders/src/spotify.py` documents the no-creds fallbacks (public embed page, oEmbed; yt-dlp flat Spotify extractor now answers `[DRM]`, proven 2026-11-02). |
| **Navidrome** | **Native Subsonic REST** — `rest/search3` then `rest/stream` (or `rest/download` for the original file, no transcode). One authenticated HTTP call each; no crypto beyond Subsonic auth. | music-bot Navidrome `refs/music-bot-navidrome/src/navidrome.py:177` `search` (`rest/search3`) / `:104` `stream_song` (`rest/stream`) / `:126` `download_song` (`rest/download`) / `:41` `_auth_params`; in-tree `services/downloaders/src/navidrome.py:62` `_call` (`/rest/<path>?…`), `rest/search2`/`search3` for lookup. |
| **Soulseek** | **slskd HTTP API v0** — `POST /api/v0/searches`, poll `GET …/responses`, `POST /api/v0/transfers/downloads/<user>`; peers ranked by free upload slots, speed, `.flac` first. No faster path exists (the network *is* the search). | `services/downloaders/src/soulseek/client.py:8-15` (endpoints verified against slskd-api 0.2.4) + `scorer.py`/`selection.py` (peer ranking); ref `refs/music-bot-navidrome/src/soulseek/` (same slskd shape). |
| **TikTok** | **yt-dlp `-J --no-playlist` metadata, then a native HTTP fetch of the CDN audio** (yt-dlp's own downloader stalls on TikTok CDN reads, so bytes come from a direct fetch; yt-dlp download is the fallback). | `services/downloaders/src/tiktok.py:8-16` (from telegram-music-bot-rust `main.rs::download_media` / `get_search_results`; TikTok URLs skip the `youtube:player_client=android` extractor arg). No ref beats this — TikTok has no public audio API. |
| **Radio** | **No resolution at all — the URL is the stream.** Fastest possible: 108 µs median measured (§2). | vc-ts `services/vc-ts/src/resolvers.js` `radioTrackFromUrl` (SHA-1 id, duration 0, `is_live: true`); ref openradiobot `refs/openradiobot/index.js` (`openradio` player takes the raw stream URL). Reachability is a separate concern: radio-js `services/radio-js` (10 s ffprobe probe → native Go verifier in Phase 3), not part of the resolve. |

---

## 4. JioSaavn re-derivation (from live behaviour)

**DES-ECB does not exist in this path.** Re-confirmed three ways:
(a) precise grep for `DES` / `createDecipheriv` / `createCipheriv` /
`CryptoJS` / `decrypt(` / `blowfish` over `refs/lemonic-telegram-bot/modules|handlers|services`
and `refs/TGVCBot/apps/tgvcbot/src` — **zero matches**; (b) the live
`/song` response contains clear-text CDN URLs (below); (c) vc-ts
`resolvers.js` contains no crypto for JioSaavn at all. (DES-ECB with
a known key is a public JioSaavn technique in the abstract; it is
simply not what these refs — or this codebase — do.)

**Actual mechanism** (proven live from the sandbox, 2026-11-03):

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
   returns a fresh token each time
   (`…_96.mp4` token differed between the 07:57 probe and the 08:01
   bench run), and a URL fetched ~2 min earlier 404s on
   `aac.saavncdn.com` (HEAD/GET, with and without browser UA +
   Referer; the JioTune preview host `jiotunepreview.jio.com` from
   the same payload answers 200, so this is URL expiry, not egress
   blocking). Consequence for Phase 4: JioSaavn URLs must be used
   promptly after resolve (or re-resolved); a long TTL cache will
   serve dead URLs.
6. **Quality finding — the current code takes the lowest quality.**
   vc-ts `jiosaavnTrack` / `resolveJioSaavnSong` select
   `media_urls["96_KBPS"] || media_url` (TGVCBot's exact shape:
   `media_urls['96_KBPS'] ?? media_url`), i.e. 96 kbps, while the
   same payload carries a `320_KBPS` URL. The locked §1.3 Tier 2
   requirement is "320 kbps stream selection" — the Go backend port
   must select `320_KBPS` (fallback `160_KBPS`, then `media_url`),
   not copy the 96 kbps selection.

---

## 5. Test suites (regressions)

| Suite | Command | Result |
|---|---|---|
| vc-go | `go test ./...` (Go 1.23.0) | **All packages ok** — resolver, queue, server, store, types (live tests skipped without `PARLAY_LIVE_TEST=1`, as designed) |
| vc-ts | `node --test tests/run.js tests/store.js` | **61 passed / 0 failed** (matches Phase 9 total 61/61) |
| vc-rust | `cargo test` (cargo 1.99.1) | **59 passed / 0 failed** (incl. `youtube_url_resolve_is_real_or_honest_error`, `bare_youtube_id_resolve_is_real_or_honest_error`, `free_text_query_never_returns_a_fake_track`, Opus/RTP pipeline tests) |
| radio-js (extra, not an engine) | `node --test tests/run.js tests/store.js` | **22 passed / 0 failed** (matches Phase 9 total 22/22) |

**No regressions.** The timer changes are wrappers only; the
pre-existing suite totals are reproduced exactly. Go/Rust suites are
runnable in the sandbox (contrary to the Phase 7 note — toolchains
are present at `~/workspace/parlay/.tools/go/bin` and `~/.cargo/bin`).

---

## 6. What this means for the locked targets

- The locked Appendix A estimate "Go InnerTube 120–450 ms" is
  **plausible but not yet met by any engine**: measured native
  InnerTube is 0.61–0.81 s (player) / 1.56 s (search) from this
  sandbox — slower than the estimate, still 7–10× faster than
  yt-dlp's 5.7–6.7 s. The "warm yt-dlp ~900 ms" estimate is **wrong**
  for the current code: cold yt-dlp is ~5.9 s and there is no warm
  daemon yet (that is Phase 3 work).
- The locked Phase 6 cold-`/play` targets: YouTube <1–3 s is
  **unreachable with yt-dlp** (5.7–6.7 s measured) and **reachable
  only with native InnerTube** (0.61–1.56 s measured). JioSaavn
  <200 ms is **not met** (0.32 s single lookup / 0.51 s search,
  measured) — needs the Phase 4 cache or a single-call path.
- `BENCHMARKS.md` was **not** edited (another worker owns it); this
  file is the engine-benchmark record for the coordinator to merge.

---

## 7. Side findings (not regressions — record for Phase 2/3)

1. **vc-rust `/v1/play` is resolve + synchronous download.**
   HTTP wall-clock 27.1 / 29.4 / 49.3 s (URL reps) and 34.5 / 38.1 /
   42.6 s (search reps) vs `t_resolve` 5.3–6.7 s. The gap is a
   synchronous yt-dlp audio download inside the play handler
   (`downloaded: true`, `file_path:
   data/downloads/chat-<id>/<video_id>.mp3`, ~93 MB downloaded
   across the bench into `/tmp/phase1-bench/rust`). `t_resolve`
   correctly excludes it. Phase 3 must not copy this shape: the
   play response must return after resolve, with download/playback
   async.
2. **vc-go and vc-ts `/v1/play` return right after resolve**
   (HTTP ≈ `t_resolve` + ~0.1 s). vc-ts then downloads audio in the
   background as part of its voice pipeline (fire-and-forget
   `playStream`); the first harness run briefly wrote into
   `~/workspace/parlay/data/downloads` before the engine was
   restarted with `cwd=/tmp/phase1-bench/ts` — the repo dir was
   removed and `git status` is clean of it.
3. **vc-ts JioSaavn takes 96 kbps** (§4 item 6) — port must take
   320 kbps.
4. **JioSaavn stream URLs expire within minutes** (§4 item 5) —
   cache TTLs must be short for this source.
5. Worker A (core timers) is working in the same tree in parallel
   (`services/core-cpp/src/dispatcher.{cpp,hpp}`,
   `services/core-cpp/src/telemetry.hpp` modified) — untouched by
   this worker; no conflicts (disjoint files).

---

## Appendix — raw evidence

- Engine logs with every `[telemetry]` line:
  `/tmp/phase1-bench/vc-go.log`, `/tmp/phase1-bench/vc-ts.log`,
  `/tmp/phase1-bench/vc-rust.log`
- Per-call results: `/tmp/phase1-bench/results.txt` (30 rows)
- Benchmark harness: `/tmp/phase1-bench/bench.sh`
- JioSaavn live payloads: `/tmp/jsvn-search.json`,
  `/tmp/jsvn-song.json` (Kesariya, `id=rjkrTnma`)
- InnerTube live payloads: `/tmp/innertube-player.json`,
  `/tmp/it-ANDROID.json`, `/tmp/it-ANDROID_VR.json`,
  `/tmp/it-search.json`
- Engines were stopped after the benchmark; no processes left
  listening on :18001–18003. No commits made (coordinator owns
  that).
