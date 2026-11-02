# Parlay — Benchmarks (Phase 4)

Measured 2026-11-02 in the sandbox (4 cores, 7 GB RAM, g++ 13,
cmake 3.28.3, Go 1.23.0, rustc 1.99.1, Node v24.22.1, ffmpeg 8.1.2,
yt-dlp 2026.10.26). Sources: `phase4/bench-core-cpp.md`,
`phase4/bench-go.md`, `phase4/bench-tsjs.md`, `phase4/bench-rustpy.md`.
All sizes are real files; all times wall-clock.

## Build time + binary size

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

## Startup to healthz

| Service | Startup |
|---|---|
| core-cpp | exit 1 in 0.00 s with missing env (validation before any network/thread work) |
| vc-go | 0.03 s (Mongo backend, low-ram) |
| vc-ts | 0.4–1.4 s |
| radio-js | 2.9–3.6 s (6 station probes in parallel at boot) |
| gui-js | 1.1–3.0 s (first run inflated by sequential polling) |
| vc-rust / spotify-rust / audio-fx / share / queue | < 1 s (process spawn) |
| netease-go / downloaders | < 2 s |

## Latency (live)

| Operation | Latency | Notes |
|---|---|---|
| `/play` YouTube URL (vc-ts, live yt-dlp) | 6.5–13.7 s | network variance, same input; resolution dominates, not the server |
| `/play` YouTube URL (vc-go, degraded, no yt-dlp) | < 100 ms | pattern resolve; live resolve needs yt-dlp installed |
| `/play` JioSaavn (vc-ts, queued) | 0.5–0.6 s | search + 1 duration enrichment |
| `/v1/search` JioSaavn, limit 3 (vc-ts) | ~2.0 s | incl. 3 parallel `/song` duration enrichments (~1.5 s per search — the price of real durations, paid once) |
| `/v1/panel`, `/v1/now-playing` (vc-ts) | < 25 ms | local endpoints |
| `/v1/lyrics` (LRCLib) | 1.0–1.9 s | retries once on 503 |
| radio start (ffprobe + VC handoff) | 0.9–1.8 s | ffprobe probe dominates |
| radio stop (with VC clear) | 181 ms | |
| gui page load | 2–11 ms (15 KB) | stream-url ~0.5–0.8 s (one VC round-trip); audio proxy 15–176 ms (5 KB tone, byte-exact) |
| vc-rust `/v1/play` YouTube (mp3 download) | 17.7 s | 7,045,581 B mp3; ~400 KB/s incl. extract + transcode |
| vc-rust `/autoplay-skip` end-to-end | 57.8 s | mix fetch 32–44 s (YouTube-side) + download ~15 s |

## Download throughput (live)

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

## What the numbers say

- **/play latency is resolution, not server.** YouTube via yt-dlp
  6.5–13.7 s; JioSaavn ~0.5 s queued; local endpoints < 25 ms.
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
