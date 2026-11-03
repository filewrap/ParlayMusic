# Parlay Phase 3 — Resolvers + yt-dlp daemon (Worker 2)

Date: 2026-11-03. Anchor: main at 3d84fb6 (Phase 2 G1+G2 fix).
Module: `services/backend-go` (`parlay/backend-go`), package
`parlay/backend-go/internal/resolver`. Daemon: `scripts/yt_dlp_worker.py`.
No commit/push (Wave 2 owns that). No Phase 4 work (no TTL cache, no
prefetch — resolve on demand only). `.env` untouched; all tests used
dummy creds only; no service was started with the user's token.

## What was built

`internal/resolver` — one `Resolver` interface + one implementation per
source, all sharing the contract `Track` from Worker 1's
`internal/model` (JSON tags match `contracts/ipc.json` v0.3.1).
`Registry` dispatches a play query by shape (`ClassifyQuery`) and
owns the shared persistent yt-dlp daemon client. Every key-gated path
fails honestly with `ErrMissingCredential` (code `engine_unavailable`)
naming the missing var/cookie/server; no fake track is ever returned.
No TODO/FIXME/HACK/Unimplemented anywhere in the new code
(grep-verified). `go vet` clean; `gofmt` clean.

| File | Source / port origin |
|---|---|
| `resolver.go` | Interface, `Registry`, `ClassifyQuery`, `ParseDuration` (vc-ts `parseDuration`), `FormatDurationStr`, `SanitizeFilename`, `DataDir`/`DefaultDownloadDir` (`<PARLAY_DATA_DIR>/downloads/`), `Progress` (`download.progress` shape), telemetry `LogResolve` (`[telemetry] t_resolve_us=… source=… engine=go`) |
| `youtube.go` (+ `youtube_key.go`, generated) | Native InnerTube, ANDROID_VR `player` (fallback ANDROID) + WEB `search` (`params=CAASAhAB`). Stream = adaptiveFormats itag **251** (Opus) / **140** (AAC); signatureCipher-only formats skipped → daemon fallback. From YukkiMusic-Go `youtube.go` (`callInnerTube`, `fetchVideo`, `parseNodes`, ID regexes), ParvatiMusic `youtube.rs`, ArcMusic `youtube.go`. Public InnerTube key generated from the YukkiMusic-Go ref into `youtube_key.go` (public client key, not a user credential). |
| `jiosaavn.go` | jsvn-tgvc proxy, ONE `/song` call or search + parallel `/song` enrichment. Selects **320_KBPS → 160_KBPS → media_url** (the old 96 kbps selection is NOT copied). Duration parsed from the `"m:ss"` clock string. No DES-ECB. Stream URLs are ~2 min signed URLs — noted in code; must be used promptly, never cached long. From TGVCBot `jiosaavn.ts` + vc-ts `resolvers.js`. |
| `deezer.go` (+ `deezer_secret.go`, generated) | Native ARL gw API + in-process Blowfish-CBC (`golang.org/x/crypto/blowfish`, v0.58.0 via the shared module). Ladder **FLAC (9) → MP3_320 (3) → MP3_128 (1)** from `FILESIZE_*`. Modern `media.deezer.com/v1/get_url` + AES-ECB legacy stream URL; chunk decryption (first 2048 B of each 6144 B chunk, IV `00 01..07`). Public-API search + 30 s preview without an ARL. From `services/downloaders/src/deezer.py` (`_FORMAT_NUMBERS`, `_gw_call`, `_song_get_data`, `_blowfish_key`, `decryptChunk`); Blowfish secret generated from that file into `deezer_secret.go`. Key derivation cross-checked against the Python implementation (see Tests). |
| `netease/` (subpackage) | `services/netease-go/internal/netease` absorbed verbatim — `client.go`, `weapi.go`, `eapi.go`, `linuxapi.go` (crypto exact) + `crypto_test.go`. Adapter `netease.go`: search, detail, SongURL ladder (eapi levels → weapi → download br=999000 → linuxapi → mirror), synced lyrics, FLAC download with the 50 MB cap (`oversize=true`). Env-gated on `NETEASE_MUSIC_U`. |
| `spotify.go` | Client-credentials token (1 POST, cached to expiry − 60 s), track lookup + search. Metadata only from Spotify; audio via the YouTube ladder. From vc-rust `main.rs` (`spotify_client_token`, `resolve_spotify_track`) + vc-ts `resolvers.js`. No creds in sandbox — honest failure naming both vars. |
| `tiktok.go` | yt-dlp `-J` metadata via the daemon, then a native HTTP fetch of the CDN audio (yt-dlp's own downloader stalls on TikTok CDN reads). Daemon download as fallback. From `services/downloaders/src/tiktok.py`. |
| `navidrome.go` | Native Subsonic REST: `rest/search2` → `search3`, `rest/stream`, `rest/download` (original file). Token auth `md5(password+salt)` with sticky plain-password fallback on 401/403. From `services/downloaders/src/navidrome.py` + `refs/music-bot-navidrome/src/navidrome.py`. Env-gated on `NAVIDROME_URL/USER/PASSWORD`. |
| `soulseek.go` | Native slskd HTTP API v0: `POST /searches`, poll to file-count plateau (6 s) or completion, `PUT` stop to force the flush, `GET /responses`, `DELETE`. Scorer ported from `scorer.py` (match 0..55 = duration 40 + name 15, × version penalty; fetch −6..35 = reliability 25 + quality 10; floor 20; FLAC-only by default) + `selection.py` thresholds; peers ranked by free slot, speed, `.flac` first. Download: enqueue + poll to `Completed, Succeeded`, file located under `SLSKD_DOWNLOAD_DIR`. From `services/downloaders/src/soulseek/`. |
| `lyrics.go` | Combined LRCLib (vc-ts `fetchLyrics`: first-artist query, one retry on 503, synced preferred, exact-title match) + NetEase synced LRC. |
| `radio.go` | No resolution — the URL is the stream (`RadioTrackFromURL`: SHA-1 id, duration 0, `is_live`). Native HTTP/ICY probe (headers + first bytes, 10 s budget) replacing the 10 s ffprobe subprocess. Curated 6-station list ported verbatim from `services/radio-js/src/stations.js`. |
| `download.go` | Shared HTTP download with progress (`percentage`, `done_bytes`, `total_bytes`, `speed`, `eta`, `phase`), daemon download path, `readAllLimit`/`newUUID`/`copyFile` helpers. |
| `ytdlp.go` | Persistent daemon client: one process, stdin/stdout JSON-RPC lines, request/response demux by id, lazy start (never at construction), ping-verified. |
| `scripts/yt_dlp_worker.py` | The daemon: warm `yt_dlp.YoutubeDL` in RAM, methods `ping` / `version` / `extract` / `download` (bestaudio → mp3). Runs on `services/downloaders/.venv/bin/python` (yt-dlp 2026.10.26); system python3 does NOT have `yt_dlp` — verified. |

## Measured times (live, this sandbox, 2026-11-03)

`go test -v -run 'TestLive|TestYtDlpDaemon' ./internal/resolver/`
with `PARLAY_LIVE_TEST=1`. All live tests pass. Single runs unless
noted; network variance applies.

| Path | Measured | vs Phase 1 locked numbers |
|---|---|---|
| YouTube player (`dQw4w9WgXcQ`, ANDROID_VR) | **0.90 s** | Phase 1 probe 0.61 s (ANDROID_VR) / 0.81 s (ANDROID). Same video, same 213 s; title now reads "…(4K Remaster)". Stream URL is a `googlevideo.com` adaptive URL (itag 251). |
| YouTube search (`never gonna give you up…`, 3 hits) | **0.60 s** | Phase 1 probe 1.56 s. Top hit `dQw4w9WgXcQ`, correct. |
| JioSaavn `/song` (`rjkrTnma`, Kesariya) | **0.74 s** (rep2); rep1 **11.75 s** | Phase 1 median 0.32 s. Rep1 was a cold-start outlier (jsvn-tgvc Vercel cold start + egress); rep2 at 0.74 s is the warm shape. Duration 268 s correct; stream URL is `_320.mp4`. |
| JioSaavn search (`Kesariya`, 3 hits + enrichment) | **3.21 s** | Phase 1 median 0.51 s. Slower today (same cold-start effect on the parallel `/song` enrichments); all 3 hits got real durations after enrichment. |
| Deezer public search (`Daft Punk Get Lucky`) | **0.98 s** | Unmeasured in Phase 1. Top hit correct with 30 s preview URL. |
| NetEase search (`Jay Chou`, anonymous) | **13.55 s** | Unmeasured in Phase 1. Returns real tracks (top hit "屋顶" by 周杰伦…); the anonymous search path is slow from this egress IP. |
| LRCLib lyrics (`Never Gonna Give You Up`) | **1.33 s** | Phase 1 vc-ts range 1.0–1.9 s. 2,506 chars, synced LRC. |
| Radio probe (SomaFM Groove Salad) | **0.80 s** | Old ffprobe probe 0.9–1.8 s (radio start, incl. VC handoff). Reachable, `audio/mpeg`, ICY name/genre/br all read. |
| Daemon extract (YouTube `-J`, warm daemon) | **4.90 s** | Cold yt-dlp 5.7–6.7 s (Phase 1). The warm daemon saves the fork + import (~1 s); the rest is YouTube-side extraction. Fallback only — native InnerTube (0.90 s) stays the primary path. |
| Daemon download (YouTube bestaudio → mp3, 7,045,581 B) | **18.61 s** (Go→daemon end-to-end; manual daemon-only probe 11.9 s) | Phase 4: ~36 s end-to-end for the same track via subprocess. Same 7,045,581-byte file as the CLI run. |
| Daemon ping/version | **0.24 s** incl. process start | No prior number; ping proves one persistent process. |

## Per-source status

- **YouTube — DONE, measured.** Native InnerTube primary; daemon fallback wired (player failure → daemon extract). Playlist/mix (`browse` VL / `next` RD) implemented.
- **JioSaavn — DONE, measured.** 320 kbps selection verified by unit test + live `_320.mp4` URL. ~2 min URL expiry noted in code and docs.
- **Deezer — DONE, unmeasured live (blocked).** Code is real and env-gated: public search/preview measured (0.98 s); full-track ARL path fails honestly naming `DEEZER_ARL`. Blowfish key derivation cross-checked against the Python port source (vectors in tests). Blocker: no Deezer ARL cookie supplied.
- **NetEase — DONE, partially measured.** Search works anonymously (13.55 s); SongURL/download/lyrics paths are the absorbed, crypto-exact code. Blocker for lossless URLs: anonymous playback is gated server-side for this egress IP (url:null without login, per netease-go's own notes); needs `NETEASE_MUSIC_U` on the user's host.
- **Spotify — DONE, unmeasured live (blocked).** Token cache + lookup + search implemented exactly per vc-rust/vc-ts. Blocker: no `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` in the sandbox (and the token endpoint is refused by the egress proxy per vc-rust's note — live path needs the user's host + creds). Honest-failure path verified by test.
- **TikTok — DONE, unmeasured live (blocked).** Metadata-via-daemon + direct-CDN-fetch code is real; no representative TikTok URL was on hand and no engine path existed to drive one. Blocker: needs a real TikTok video URL to measure against.
- **Navidrome — DONE, unmeasured live (blocked).** Subsonic client + auth fallback implemented. Blocker: no Navidrome server URL/creds in the sandbox. Honest-failure path verified by test (names all three vars).
- **Soulseek — DONE, unmeasured live (blocked).** slskd client + scorer port implemented; scorer unit-tested (exact match wins, live-take penalised 0.30, 31 s-off excluded, reliability ordering). Blocker: slskd is not running (`curl 127.0.0.1:5030` → refused) and no slskd login exists. Honest-failure path verified by test.
- **Lyrics — DONE, measured.** LRCLib live (1.33 s, synced). NetEase lyrics via the absorbed client (unmeasured live — same NetEase blocker as above).
- **Radio — DONE, measured.** Track-from-URL is 108 µs-class (no I/O); native probe measured 0.80 s reachable with ICY metadata.
- **Downloads — DONE (code), download path live-measured via daemon (see table).** Per-source download functions return `file_path + track + run_id` with progress hooks (`download.progress` fields). Radio correctly refuses download (live stream, no file).

## Tests

`go test ./...` (module-wide, incl. Worker 1's packages): all pass.
Resolver package: unit tests (no network) + live tests gated on
`PARLAY_LIVE_TEST=1`. Unit coverage: `ParseDuration` (incl. the
`"4:28"` → 268 regression case), `FormatDurationStr`, YouTube ID /
playlist extraction (incl. the host-gate fix: the loose fallback
regex previously matched 11-char runs inside Spotify/TikTok URLs),
`ClassifyQuery` for all 12 shapes, JioSaavn 320/160/media_url
selection, Deezer Blowfish key vectors vs the Python implementation
(`123` → `6c32636d67367064637d716c3d3b6363`,
`123456789` → `6d34656061377f31322a7336393f626b`), Blowfish chunk
round-trip/determinism, legacy stream-URL shape, format ladder,
Spotify/Navidrome/Soulseek honest-failure errors, Soulseek scorer
math, radio stations (6, no dup ids) + track shape, registry wiring,
InnerTube key presence. NetEase absorbed `crypto_test.go` passes in
this module. Daemon tests: ping/version (0.24 s), garbage-URL extract
fails honestly.

## Daemon design (summary)

`scripts/yt_dlp_worker.py` — one process, started once by
`YtDlpClient.ensureStarted` on first use (lazy; a backend serving
only native resolvers never spawns it). Warm `yt_dlp.YoutubeDL`
(`extract_info(download=False)` for metadata; a second instance with
`bestaudio/best` + mp3 extraction for downloads). JSON-RPC lines
over stdin/stdout with `{"id","method","params"}` /
`{"id","result"|"error"}`; the Go client demultiplexes by id with a
reader goroutine and per-call timeouts (extract 120 s, download
600 s). The daemon never dies on a bad call (per-call try/except →
error frame). Python resolution: `$PARLAY_YTDLP_PYTHON`, else
`services/downloaders/.venv/bin/python` (walk-up from cwd), else
`python3`. Script resolution: `$PARLAY_YTDLP_WORKER`, else
`scripts/yt_dlp_worker.py` (walk-up). Verified: venv python has
yt_dlp 2026.10.26; system python3 does not.

## Notes for the coordinator / Wave 2

1. **Shared module moved under me:** Worker 1 created
   `services/backend-go` concurrently (module `parlay/backend-go`,
   `internal/model|config|store|uds|voice`, `cmd/backend`). Their
   `go.mod` now says `go 1.26.0` with `golang.org/x/crypto v0.58.0`
   + bbolt + BurntSushi/toml (the go1.26.10 toolchain auto-downloaded
   into the module cache). I built inside that module as-is and did
   not fight it; my resolver uses only `model` + `x/crypto/blowfish`.
   My early `go mod init` raced theirs — the file they produced is
   the one in the tree; I overwrote nothing of theirs (their
   `internal/model/model.go` etc. are untouched).
2. **No Phase 4 work done** — no cache, no prefetch; `Resolve` hits
   the network every call.
3. **JioSaavn search is slow when cold** (3.21 s vs 0.51 s Phase 1
   median) — Vercel cold starts dominate; a warm path or the Phase 4
   cache will help, but the single-`/song` path (0.74 s warm) is the
   fast lane and is what `/play` should prefer.
4. **NetEase anonymous search is slow** (13.55 s) from this egress;
   on the user's host with `MUSIC_U` it should be faster — not
   verified here.
5. **Daemon download test**: `TestLiveDaemonDownloadYouTube` passes —
   7,045,581-byte mp3 in 18.61 s end-to-end. Two real bugs were found
   and fixed while proving it (both are in the worker/client, not the
   network — the yt-dlp CLI downloaded the same track in 13 s throughout):
   (a) yt-dlp's `[download]` progress bar is written to **stdout**
   with `\r` separators, glued onto the same physical line as the
   JSON-RPC response; the Go reader dropped the unparseable line and
   the caller hung until its timeout. Fixed with `"noprogress": True`
   in the worker plus `\r`-segment-tolerant parsing in the Go
   `readLoop` (defence in depth). (b) The mp3 postprocessor never ran:
   `"extractaudio"`/`"audioformat"` params are only read by the
   yt-dlp **CLI** entrypoint, not by the `YoutubeDL` Python API — the
   daemon downloaded a raw `.webm` until the worker passed an explicit
   `"postprocessors": [{"key": "FFmpegExtractAudio", ...}]` entry.
   Both findings are documented in the worker source.
