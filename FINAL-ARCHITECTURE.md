# Parlay — FINAL ARCHITECTURE (Locked Execution Document)

Status: **LOCKED — finalisation complete, implementation NOT started.**
Finalised: 2026-11-03. Anchor commit: **d5065b9** (Phase 9 UI/UX port).
Supersedes the anchor in the uploaded locked document (1aa39cc) and every
number in `CURRENT-STATE.md` that Phase 9 changed. This is the single
document implementation executes against. If this document and the uploaded
locked document disagree, this document wins on facts (anchors, counts,
symbols); the uploaded document wins on decisions (nothing here re-litigates
a locked decision — contradictions are recorded as verdicts in §6 or open
decisions in §11, never silently redesigned).

Source of the locked decisions: `~/workspace/user/files/CURRENT-STATE-UPDATED_0_njyd.md`
(Master Execution Prompt + System Design spec), plus the user's locked
Telegram UI addition of 2026-11-03 ~12:59 IST (folded in as §7).

Order of work, per the user: **finalisation first, then implementation.**
Implementation does not start until the user confirms this document.

Superseded work (do not resume): the old Phase 8 linking conversion and the
old speed/async phase are absorbed into the locked architecture's Phases 1–4.

---

## §1. Locked decisions (verbatim in substance)

### §1.1 Mandate

Ground-up architectural consolidation and audio pipeline overhaul of Parlay
(github.com/filewrap/ParlayMusic). Transform the 12-service HTTP/JSON
polyglot mesh into an ultra-low-latency, lossless-capable engine: C++20 Core
+ Go Unified Backend with in-process Rust C-ABI DSP and a persistent yt-dlp
Python daemon, plus a TypeScript Lossless Mini App. Legacy test preservation
is NOT a constraint — tests validating obsolete HTTP microservices, engine
fallback routers, or curl subprocess wrappers are replaced, not preserved.

### §1.2 Hard prohibitions (violation = immediate failure)

1. **No assumptions or blind coding.** Before writing or modifying a single
   line in any phase, inspect the actual target files in `services/` and the
   relevant reference implementations in `~/workspace/parlay/refs/`. Verify
   every library symbol (`nm -D` / header inspection on libtdjson.so.1.8.69
   and libntgcalls.so 3.0.5) before binding to it. (Finalisation has done
   this verification once, §3 — implementation re-checks at link time.)
2. **No shortcuts, stubs, mocks, or placeholders.** No TODO,
   `unimplemented!()`, hardcoded dummy returns, fake "Now playing" messages,
   or silent error drops. Every feature across all 11 existing non-core
   services and all 785 Phase 9 UI elements must be ported with 100%
   functional parity (§10).
3. **No subprocess spawn per call.** Spawning curl, ffmpeg, ffprobe, or
   yt-dlp per command or per track is prohibited on the primary execution
   path.
4. **No inter-service HTTP or message bus.** Delete the 11 HTTP servers
   (ports 8001–8010 and 3000), delete the engine fallback router
   (`EngineRouter::callVc`), delete `/healthz` pre-probes, delete the
   5-second auto-advance polling watcher, delete external Mongo (27017) and
   Redis (6379) dependencies. (Correction §6-R1: the core's own :8080 HTTP
   server is also deleted; it was not named in the locked text.)
5. **No hardcoded limits or timeouts.** The 1 cmd/sec silent rate-limit
   drop (`dispatcher.cpp:911`), 30/10/5 s IPC timeouts (`ipc.hpp`,
   `http.hpp`), 128 kbit/s Opus ceiling, 60-minute duration limit, and
   20-item queue limit are replaced by a structured, hot-reloadable
   configuration schema. (Correction §6-R6: protocol/physical constants —
   loop 0–10, volume 0–200, 960-sample frames, 48 kHz — are NOT covered by
   this prohibition; they are wire-format and physics constants.)
6. **No compromise on audio quality.** Never transcode twice. Never run
   audio effects through 16-bit integer intermediaries. Never cap Telegram
   voice calls at 128 kbps when the SFU accepts higher bitrates. Never send
   lossy audio when a lossless FLAC source is available on the source
   ladder.

### §1.3 Locked four-part target architecture

**Part 1 — Core (`services/core-cpp`, C++20).** Build-time linking of
libtdjson.so.1.8.69 (follow NoxBeats/TinaMusic). Dual-session TDLib in one
process: bot session over MTProto via `checkAuthenticationBotToken` (inspect
anonx-cpp); completely remove the HTTPS Bot API `getUpdates` 30-second
long-poll loop in `main.cpp` (removes the 50 MB Bot API file cap, lifting it
to 2 GB, and eliminates the `oversize:true` deferred MTProto queue).
Assistant session (`data/tdlib`, mode 0700) in the same process for
`getChat`, `createVideoChat`, `joinVideoChat`. Responsibilities: command
parsing, alias and channel-command resolution, allow/block chat gates,
Owner/Sudo/Everyone hierarchy, configurable token-bucket rate limiting with
burst capacity, Phase 9 Telegram UI rendering (now under the §7 UI rules),
direct FLAC file uploads via TDLib. Persistent duplex IPC: delete
`services/core-cpp/src/http.cpp` (curl subprocess spawner); replace
`IpcClient` with ONE persistent bidirectional connection (Unix Domain
Socket or gRPC over UDS — open decision OD-4) to the Go backend, with
connection pooling/multiplexing and per-action deadlines from config.
Strict audio separation: core-cpp never touches libntgcalls.so, never spawns
ffmpeg, never touches audio frames or media URLs. For voice joins, core-cpp
receives `JoinRequest(chat_id, local_sdp)` from Go over the persistent
socket, executes TDLib `joinVideoChat(local_sdp)` on the assistant session,
and returns `JoinResponse(remote_sdp)` to Go.

**Part 2 — Backend (`services/backend-go`, single unified Go binary).**
Voice call ownership (ntgcalls Go bindings): link libntgcalls.so 3.0.5 into
Go via cgo (reference TgMusicBot). Go owns call lifecycle: CreateCall (local
SDP offer), Connect (remote SDP handshake via core-cpp), stream source
binding, pause, resume, mute, unmute, stop, real-time 20 ms frame delivery
from parlay-dsp. Single embedded state store: eliminate Mongo, Redis, and
multi-service JSON directories; one unified in-memory state engine backed by
a single embedded ACID key-value store (bbolt or pebble — open decision
OD-5) in `PARLAY_DATA_DIR`. It owns: per-room playback/queue state (loop
0–10, shuffle, autoplay, volume 0–200, speed, active FX preset/parameters,
is_paused, is_muted, position); crowd-voting state (configurable threshold,
from queue-rust); anonymous share links/tokens (from share-rust); Spotify
watch registry, dislike store, per-user AES-256-GCM OAuth tokens, skip-check
state (from spotify-rust); per-chat settings (all 15 keys from
`chat.settings`) and radio station state (from radio-js). In-process
event-driven track transitions: no 5-second watcher; when parlay-dsp crosses
a track boundary or hits EOF it invokes an in-process Go callback
immediately; Go updates queue state and pushes an asynchronous TrackChanged
event over the persistent socket to core-cpp. Native in-process Go resolvers
and downloaders, strictly ordered by quality ladder: Tier 1 true lossless
(FLAC) — Navidrome (native Go Subsonic client, from downloaders and
music-bot Navidrome), Soulseek (native Go client to slskd :5030, peers ranked
by free upload slots, speed, `.flac` format), Deezer (native Go ARL client,
in-process Blowfish-CBC decryption, FLAC format 9, fallback MP3_320),
NetEase (absorb `services/netease-go`: weapi/eapi/linuxapi crypto, search,
lossless FLAC download, synced lyrics); Tier 2 high-bitrate lossy (320 kbps)
— JioSaavn/Lemonic (native Go client, 320 kbps stream selection, exact
duration parsing, from vc-ts and lemonic-telegram-bot); Tier 3 adaptive
streams and metadata resolvers — YouTube (primary native Go InnerTube client,
ANDROID_VR / IOS / TVHTML5 contexts, adaptive Opus itag 251 / AAC itag 140
in memory; fallback to ONE persistent yt-dlp worker started once at Go boot
over persistent stdin/stdout JSON-RPC lines so `yt_dlp.YoutubeDL` stays warm
in RAM with zero per-call fork), Spotify and playlists (port spotify-rust and
downloaders Spotify logic: client-credentials and OAuth, metadata
resolution, Gemini/wordlist skip-check classifier, audio resolution through
the Tier 1 → 2 → 3 ladder), TikTok (native Go extractor), lyrics (combined
LRCLib + NetEase synced LRC), internet radio (native Go stream verifier
replacing the 10-second ffprobe subprocess, plus curated station fallback).
TTL stream cache and lookahead prefetch: resolved metadata/stream URLs cached
in the embedded store keyed by canonical `<source>:<track_id>`, expiration
from CDN URL signatures (`expire=` parameters); when track N starts (or N+1
is promoted to queue top) a background goroutine resolves N+1 and hands the
stream handle to parlay-dsp to pre-buffer the opening seconds. GUI and
lossless streaming server: serve the TypeScript Mini App static bundle,
replace gui-js (:3000) proxy endpoints with direct in-memory state
reads/mutations, expose `GET /api/v1/stream/:chat_id/flac` with HTTP 206
Partial Content byte-range streaming of raw FLAC.

**Part 3 — DSP engine (`services/parlay-dsp`, Rust C-ABI library).** Strict
boundary: compiled as a native library (`libparlay_dsp.so` /
`libparlay_dsp.a`) linked into the Go backend via cgo; Rust performs zero
network requests or downloads. Decode and demux (symphonia + ffmpeg
fallback): symphonia primary (FLAC, Opus, Vorbis, MP3, AAC, ALAC, WAV)
decoding directly into 32-bit float (f32) buffers; ffmpeg fallback only on
symphonia unsupported format/protocol errors (e.g. complex HLS/RTMP radio),
feeding the same f32 ringbuffer. High-precision resampling (rubato): any
non-48 kHz source (44.1/88.2/96/192 kHz) to 48,000 Hz stereo in f32, sinc
resamplers, mastering-grade stopband attenuation. Live zero-restart float
effect chain (f32): all DSP parameters stored in lock-free atomic state and
smoothed across 20 ms frame boundaries (960 samples per channel at 48 kHz):
nightcore and slow-reverb via dynamic resampler ratio + stereo Freeverb/FDN
reverb; bass-boost via cascaded 2nd-order Butterworth/biquad low-shelf with
smooth coefficient interpolation; 8D via phase-continuous quadrature LFO
panning + Haas delay; volume 0–200 mapped to linear/logarithmic gain;
true-peak brickwall limiter (soft-knee lookahead, −0.1 dBFS in f32); single
final TPDF dither f32 → s16le once at the final output stage.
Sample-accurate gapless playback: two decoder slots per room (active and
next, fed by Go prefetch); when active hits EOF partway through a 960-sample
frame, parlay-dsp fills the exact remaining samples from next, promotes
next to active, and sets the transition flag so Go fires the track-advance
callback on the exact sample boundary.

**Part 4 — Frontend (TypeScript Mini App and web player).** Consolidate
gui-js into a typed TypeScript application built and served directly by the
Go backend. WebSocket/SSE for instant state sync (now playing, queue,
synced lyrics, active effects, volume, loop, shuffle), zero polling delay.
HTML5 audio / Web Audio API lossless player streaming raw FLAC from
`/api/v1/stream/:chat_id/flac` while the voice chat runs simultaneously.

### §1.4 Quality and configuration lock

Voice chat Opus mastering: configure the Opus encoder for maximum music
fidelity (OPUS_APPLICATION_AUDIO, OPUS_SIGNAL_MUSIC,
OPUS_SET_COMPLEXITY(10), OPUS_SET_VBR(1), OPUS_SET_INBAND_FEC(0)). Expose
target bitrate in config (`opus_bitrate_kbps`), defaulting to the highest
stereo bitrate accepted by Telegram's WebRTC SFU without packet drop (test
320 kbps → 256 kbps → 192 kbps on the live VPS call path and lock the
verified ceiling). **Finalisation finding: this section is NOT implementable
against stock libntgcalls.so 3.0.5 — see §3.3 and open decision OD-1. The
requirement stands as locked; the binding surface does not exist and must be
resolved before Phase 5.**

Unified configuration file (`parlay.toml` / `parlay.yaml` + env overrides —
open decision OD-6): `rate_limit` (per-chat and per-user token bucket:
capacity burst, refill_rate_per_sec); `timeouts` (per-action budget table:
resolve_ms, join_ms, ipc_cmd_ms, download_ms); `quality` (voice_profile:
ultra_320k, high_256k, standard_192k; source_ladder priority array;
resampler_quality: sinc_256, sinc_128, fast); `limits`
(max_duration_minutes, max_queue_size, prefetch_lead_time_sec,
url_cache_ttl_sec).

### §1.5 Strict sequential order of execution

Phase 1 — Telemetry and engine benchmark. Phase 2 — Persistent IPC and
dual-TDLib C++ core. Phase 3 — Unified Go backend, embedded store, native
resolvers. Phase 4 — Cache and lookahead prefetch. Phase 5 — parlay-dsp Rust
engine and cgo integration. Phase 6 — TypeScript lossless frontend and live
VPS proof. Do not advance to Phase N+1 until Phase N builds cleanly and
passes its verification benchmark (§9).

---

## §2. Re-anchored baseline (d5065b9)

The locked document anchors at 1aa39cc (v0.2.0, "IPC 0.3.1", "51
capabilities"). That anchor is stale. Verified by `git log` / `git diff
--stat` 1aa39cc..d5065b9:

- **Anchor commit: d5065b9** — "Parlay Phase 9: Telegram UI/UX port from all
  21 refs", single commit, 53 files changed, +27,822/−604. Local HEAD ==
  origin/main at d5065b9 (verified via `git ls-remote`).
- **IPC contract: 51 actions at 1aa39cc (version 0.2.0) → 56 actions at
  d5065b9 (version 0.3.1).** The locked document's claim "IPC 0.3.1" at
  1aa39cc is wrong; 0.3.1 is the Phase 9 version. Five actions added, zero
  removed: `chat.settings`, `download.progress`, `spotify.analyze`,
  `spotify.details`, `vc.progress`. The parity target is **56 actions** (§10).
- **Phase 9 content the locked document does not know about:** new
  `services/core-cpp/src/ui_texts.hpp` (3,256 lines, the full UI string
  catalogue), `services/core-cpp/tests/ui_test.cpp`, `UI_INVENTORY.md` (785
  elements: 533 ported, 115 upgraded by a better ref, 137 not-applicable, 0
  unaccounted), `PHASE9.md`, `phase9/` inventory and port reports, the
  `pl:<domain>:<verb>[:args]` callback namespace (64-byte Bot API limit),
  inline mode built from scratch (`np`/`s`/`l`/`help`/search +
  `spotify----` deep link), new commands (`/menu`, `/id`, `/keyboard`,
  `/about`, `/status`, `/now`, `/repo`, `/download-settings`, named
  playlists), radio lifecycle commands, auth limit 25, IPC-1..7 (now-playing
  UI fields, progress, enriched Track fields, download progress, lyrics
  fields, chat settings, Spotify details/analyze), `download.spotify_playlist`
  `file_paths` (files mode), and the core interactive layer (sendPhoto,
  editMessage, answerCallbackQuery, inline/reply keyboards,
  `callback_query` + `my_chat_member` parsing, `pl:` callback router).
- **Verified test totals at d5065b9 (Phase 9 coordinator re-runs):** core-cpp
  635 passed / 0 code failures (dispatcher 228, ui 159, telegram_api 73,
  http_api 71, router 20, scram 40, tdlib 44); vc-ts 61/61; radio-js 22/22;
  downloaders 73 pass / 4 errors / 8 skipped. Caveats: store_test 2 failures
  (mongo/redis down in sandbox — environmental); downloaders 4 errors
  (pre-existing Cryptodome import gap); Go/Rust suites unrunnable in the
  sandbox (no toolchain — same as Phase 7). Legacy totals are superseded by
  §1.1 (legacy test preservation is NOT a constraint).
- **Phase 9 residuals (PHASE9.md §5–§6), carried into this document:**
  245 of 497 consts and 70 of 199 helpers in `ui_texts.hpp` are unreferenced
  by the core (deliberately kept as ported reference text); not implemented:
  `download.cancel` / `library.delete` IPC actions (buttons exist, taps
  answer honestly unavailable), `/mix`, `/recognize`, `/scan`, `/sharescan`,
  `/rmcache` (no IPC action), live download progress polling (needs async
  download POST), Spotify companion surfaces, download prefs contract fields,
  `vc.lyrics` GET-vs-POST mismatch (contract says GET, vc-ts is POST-only,
  core calls POST), inline `Song not cached` article, inline `np` oversize
  (Track carries no size), `/status` counts are 0. Disposition: §6-R5.
- **Live verification pending for everything** (pull + restart + real
  Telegram); the sandbox cannot exercise callbacks from a real client, the
  /play-via-Telegram path, or MTProto voice.
- **Untracked file in the tree:** `CURRENT-STATE.md` (written 2026-11-03
  ~12:08 IST for the colleagues' redesign discussion) is not a Phase 9 file
  and is not part of this architecture; leave it alone.

---

## §3. Symbol verification (run 2026-11-03 during finalisation)

Method: `nm -D` on the built libraries on disk, plus header inspection.
No code was written or modified to produce these results.

### §3.1 TDLib — libtdjson.so.1.8.69 — VERIFIED, all present

Library: `phase4/tdlib-src/build/libtdjson.so.1.8.69` (50,166,944 B).
All 14 `td_` dynamic exports present:

- `td_json_client_create`, `td_json_client_destroy`,
  `td_json_client_execute`, `td_json_client_receive`, `td_json_client_send`
- `td_create_client_id`, `td_execute`, `td_receive`, `td_send`
- `td_set_log_fatal_error_callback`, `td_set_log_file_path`,
  `td_set_log_max_file_size`, `td_set_log_message_callback`,
  `td_set_log_verbosity_level`

Every symbol the dual-session core needs exists. The bot-token path
(`checkAuthenticationBotToken`) is a TDLib API method invoked through
`td_json_client_send`, not a library symbol — its existence is proven by
the anonx-cpp reference (§5) and by TDLib 1.8.69's API, not by `nm -D`.

### §3.2 ntgcalls — libntgcalls.so 3.0.5 — control symbols VERIFIED, Opus config MISSING

Library: `refs/ntgcalls/shared-output/lib/libntgcalls.so` (98,121,040 B;
9,024 dynamic symbols). All control symbols the design needs are present:

- `ntg_instance_create`, `ntg_instance_destroy`
- `ntg_create_call`, `ntg_connect`, `ntg_connect_p2p`
- `ntg_set_stream_sources`
- `ntg_pause`, `ntg_resume`, `ntg_mute`, `ntg_unmute`, `ntg_stop`,
  `ntg_stop_presentation`, `ntg_time`
- `ntg_get_version`, `ntg_last_error`, `ntg_string_free`
- `ntg_on_stream_end_callback`, `ntg_on_connection_change_callback`
- `ntg_send_external_frame`, `ntg_send_broadcast_timestamp`,
  `ntg_send_broadcast_part`, `ntg_send_signaling_data`,
  `ntg_update_audio_ssrc_mappings`, `ntg_connection_info_free`

External PCM frame push exists: `ntg_send_external_frame(handle, chat_id,
device, data, data_len, frame_data)` is exported (C header
`refs/ntgcalls/shared-output/include/ntgcalls.h:521`) and implemented in
`ntgcalls/src/media/stream_manager.cpp:250` (forwards raw bytes to the
stream's `send_data`). **Caveat:** the C header does not document the
expected PCM format — the `ntg_frame_data` struct is video-oriented
(rotation/width/height). Phase 5 must confirm the expected format (expected:
s16le, 48 kHz, stereo) against the StreamManager external-reader init path
before binding the DSP output to it.

### §3.3 Opus encoder configuration — NOT FOUND (blocking gap)

Searches run: `nm -D` for any `opus` symbol (0 matches); full case-insensitive
grep for `opus|encoder|bitrate|complexity|vbr|fec` in the C header (0
matches); grep for `OPUS_SET_*|OpusEncoder|opus_encoder_create|
APPLICATION_AUDIO|bitrate` across the entire `refs/ntgcalls` source tree
excluding `deps/` (0 matches); "opus" appears in only 5 source files, all as
payload-type identification (WebRTC `kOpusCodecName`,
`FrameTransformer::PayloadType::Opus`) — never as encoder configuration.
The library dynamically links no libopus, no libssl, no libcrypto (all
statically embedded — consistent with the NoxBeats symbol-clash warning).

**Verdict: the locked §1.4 Opus mastering requirement
(OPUS_APPLICATION_AUDIO, OPUS_SIGNAL_MUSIC, OPUS_SET_COMPLEXITY(10),
OPUS_SET_VBR(1), OPUS_SET_INBAND_FEC(0), bitrate 320 → 256 → 192) has NO
binding surface in stock libntgcalls.so 3.0.5.** In this library, Opus
encoding is done by WebRTC's internal encoder with internal defaults that
the C API cannot change. Today's code encodes Opus outside ntgcalls (ffmpeg
libopus `-b:a 128k`, `voice_call.cpp:449,469`) — the 128 kbit/s ceiling the
locked document orders removed is a property of the current ffmpeg call, not
of ntgcalls. Open decision **OD-1** (§11) — the requirement stands, the
mechanism must be chosen before Phase 5: (a) patch and rebuild ntgcalls from
source to expose encoder configuration; (b) accept WebRTC's internal Opus
defaults and drop the configurable-bitrate requirement; or (c) have
parlay-dsp encode Opus itself (opus crate, full config control) and feed
Opus packets to ntgcalls via `ntg_set_stream_sources` (the path today's
ffmpeg pipeline already uses), keeping the f32 FX chain and adding an Opus
encode step in the DSP. Option (c) is the design-consistent option, but the
choice is the user's.

### §3.4 Other verification notes

- TDLib version string: `tdlib_client.cpp:1,36` records libtdjson.so.1.8.69,
  50,166,944 B, all deps resolvable.
- The Go backend's cgo binding pattern is proven by TgMusicBot (§5):
  `#cgo linux LDFLAGS: -L . -lntgcalls -lm -lz -lresolv` plus
  `#include "ntgcalls/ntgcalls.h"`.
- No yt-dlp daemon exists in the tree today (verified: no
  `yt_dlp_worker.py`); no bbolt/pebble dependency exists in the tree (new
  dependencies, Phase 3).

---

## §4. Code-location verification (all confirmed in-tree at d5065b9)

| Locked claim | Location | Verified |
|---|---|---|
| 1 cmd/sec silent rate-limit drop | `services/core-cpp/src/dispatcher.cpp:911`, `rateLimited()` — 1000 ms window, comment "anonx-cpp RateLimiter shape: 1 cmd/sec per chat" | Yes |
| 30/10/5 s IPC timeouts | `ipc.hpp:64,71` timeout_sec = 30; `ipc.hpp:75` healthz = 5; `http.hpp:26,34,47,50` timeout_sec = 30; `vcGet` uses 10 s | Yes |
| 30 s long-poll | `main.cpp:187` `getUpdates(offset, 30, err)`; backoff 1 s doubling, cap 30 s (`main.cpp:184,193`) | Yes |
| Engine fallback router | `engine_router.cpp` `callVc`; `engine_router.hpp:42` fallback order Go→{Go,Ts,Rust}, Ts→{Ts,Go,Rust}, Rust→{Rust,Go,Ts}; default engine Go | Yes |
| `/healthz` pre-probes | `engine_router.cpp:52` — non-preferred engines probed before the real call | Yes |
| curl-CLI HTTP client | `services/core-cpp/src/http.cpp` — curl subprocess spawner; one process fork per IPC call | Yes |
| 5-second watcher | `dispatcher.cpp:2033` — "every 5 s the watcher asks each…"; watcher only handles autoplay (dispatcher.cpp:1957) | Yes |
| Core HTTP server :8080 | `http_server.cpp:94` `listen()`; GUI API surface (Phase 6 A2) | Yes — must be deleted too (§6-R1) |
| 12 services, ports 8080/8001–8010/3000 | `services/` has 12 dirs; `config.hpp:35–46` URLs + port 8080; `docker-compose.yml` | Yes |
| mongo 27017, redis 6379, slskd 5030 | `docker-compose.yml` (`mongo:7`, `redis:7-alpine` AOF, slskd 5030/5031, Soulseek 50300) | Yes |
| 128 kbit/s Opus ceiling | `voice_call.cpp:449,469` ffmpeg `-c:a libopus -b:a 128k -ar 48000 -ac 2` | Yes |
| 60-min duration / 20-item queue limits | `config.hpp` `duration_limit_min = 60`, `queue_limit = 20` | Yes |
| `attachEngineAudio()` | `dispatcher.cpp` — core streams engine-resolved audio through its own ntgcalls session; attach failure stops engine playback + exact error | Yes |
| `vc.lyrics` GET-vs-POST mismatch | Contract says GET; vc-ts is POST-only; core calls POST | Yes (§6-R5) |
| Parlay's TDLib client has NO bot-token path | `grep checkAuthenticationBotToken services/core-cpp/src/tdlib_client.cpp` — zero matches. The path exists only in anonx-cpp (§5). Phase 2 must add it; it is not already in-tree despite the earlier claim that it was. | Corrected |

---

## §5. Reference-implementation verification

| Locked claim | Reference | Verified |
|---|---|---|
| Bot authenticates over MTProto via `checkAuthenticationBotToken` | anonx-cpp `src/telegram/tdlib_client.cpp:299`; dual client (`is_bot_` flag, `:82,296`) — same client handles bot (token) and user (phone) sessions | Yes |
| Build-time linking of libntgcalls | NoxBeats `CMakeLists.txt:35` `target_link_libraries(botcore PUBLIC ${NTGCALLS_DIR}/lib/libntgcalls.so)` behind `-DWITH_NTGCALLS=ON`; TinaMusic `CMakeLists.txt:29` `target_link_libraries(tina-music-bot PRIVATE CURL::libcurl ntgcalls …)` | Yes |
| cgo ntgcalls binding for Go | TgMusicBot `main.go:12` `#cgo linux LDFLAGS: -L . -lntgcalls -lm -lz -lresolv`, `:18` `#include "ntgcalls/ntgcalls.h"`, `:29` import `ashokshau/tgmusic/ntgcalls` | Yes |
| Static ntgcalls link (Rust) | ParvatiMusic `ntg/build.rs:21` `cargo:rustc-link-lib=static=ntgcalls_trimmed` (+ whole-archive BoringSSL shim `:19`) | Yes |
| JioSaavn/Lemonic DES-ECB stream URL decryption | lemonic-telegram-bot + vc-ts | **NO — NOT FOUND.** No `DES`/`decrypt`/`cipher` anywhere in `refs/lemonic-telegram-bot/modules/`, `handlers/`, or `services/vc-ts/src/resolvers.js`. vc-ts JioSaavn goes through TGVCBot's public proxy (`jsvn-tgvc.vercel.app`) and the saavn.dev API, which return clear URLs. The DES-ECB step does not exist in the cited refs. Phase 1 must re-derive the JioSaavn resolution strategy from the live proxy/API behaviour; do not assume DES-ECB exists in these sources. (DES-ECB with a known key is a public JioSaavn technique, but it is not in the cited code.) |
| Deezer ARL + Blowfish-CBC FLAC (format 9) | In-tree downloaders | Yes — `services/downloaders/src/deezer.py:37,45,60,212` (Blowfish-CBC chunk decryption, ARL cookie path, `Cryptodome`) |
| Navidrome Subsonic client | music-bot Navidrome | Yes — `refs/music-bot-navidrome/src/navidrome.py` references Subsonic |
| Spotify-Loader | Spotify-Loader | Yes — `refs/Spotify-Loader/bot.py` exists |
| NetEase weapi/eapi/linuxapi | netease-go (in-tree) | Yes — `services/netease-go/internal/netease/{weapi,eapi,linuxapi}.go` |
| Lemonic search/download | lemonic-telegram-bot | Yes — `refs/lemonic-telegram-bot/modules/lemonic.ts`, `songlink.ts` exist (no crypto in them) |

---

## §6. Reconciliation verdicts

Each contradiction or gap found during finalisation, with its verdict.
Verdicts resolve in favour of the locked text unless stated otherwise; a
verdict never silently redesigns.

**R1 — "12-service" vs "11 services" vs "11 HTTP servers".** `services/`
has 12 directories. 11 are non-core HTTP services (ports 8001–8010 and
3000); core-cpp is the 12th and also serves HTTP on :8080
(`http_server.cpp`, GUI API). Verdict: the mandate's "12-service" and the
prohibition's "11 existing services" are both correct about different sets.
The delete list must name **12 HTTP servers**, not 11: the 11 non-core
services plus the core's own :8080 server. `http_server.cpp` /
`http_server.hpp` are added to the DELETE list (§8); the locked text's
failure to name them is corrected here, not re-litigated.

**R2 — "51 IPC capabilities" vs 56 actions.** The locked document anchors
at 1aa39cc (51 actions, contract v0.2.0) while also claiming "IPC 0.3.1" at
that anchor. Both cannot be true: 0.3.1/56 is d5065b9. Verdict: parity target
is **56 actions** (§10), the five Phase 9 additions enumerated in §2. The
locked text's "51" is superseded by the re-anchor.

**R3 — "2-process" vs real process count.** The locked text says "two core
application binaries (plus optional slskd)" and in the same section "manages
one persistent background Python worker (yt_dlp_worker.py)". That is 2 core
binaries + 1 daemon + optional slskd = **4 OS processes (3 without
Soulseek)**, not 2. Verdict: the headline "2-process" is wrong; the correct
statement is "2 core binaries + 1 persistent yt-dlp daemon + optional
slskd". The topology itself (what runs) is unchanged; only the count is
corrected. Any diagram or doc that says "2 processes" must say "2 core
binaries".

**R4 — Existing Mongo/Redis/JSON state when the embedded store replaces
them.** The locked document orders the embedded store and the deletion of
Mongo/Redis but never says what happens to the state already in them
(per-chat engine choice, queue state, Spotify per-user OAuth tokens
AES-256-GCM, share links/tokens, votes, per-chat settings, radio state).
Verdict: open decision **OD-2**. Fresh start loses all of it (including
logging every Spotify user out); one-time migration preserves it. The
decision is the user's; Phase 3 cannot start its store work until it is
made.

**R5 — Phase 9 dead strings and not-implemented surfaces under the 100%
parity rule.** (a) 245 dead consts / 70 dead helpers in `ui_texts.hpp`:
these are unreferenced catalogue text, not live UI — verified in PHASE9.md
§5. Verdict: they are NOT parity items. **DELETE them** in Phase 2 (they
are not carried forward, and they are not counted in the 785). (b) Live
surfaces that currently answer "unavailable": `download.cancel` /
`library.delete` buttons, `/mix`, `/recognize`, `/scan`, `/sharescan`,
`/rmcache`, Spotify companion surfaces (`/details`, `/analyze`), download
prefs contract fields, inline `Song not cached`, inline `np` oversize.
Verdict: these ARE parity items because the buttons/commands exist in the
live UI. Each must either be implemented in the new architecture (new
backend actions) or explicitly cut by user decision — open decision **OD-3**.
The `vc.lyrics` GET-vs-POST mismatch is resolved by verdict: the new
contract says POST (the only form that works today); the old contract's GET
is corrected, not preserved. `/status` counts of 0 are honest zeros (the
core keeps no song cache); in the new architecture the backend owns the
counts, so `/status` reads them from the backend — no decision needed.

**R6 — "No hardcoded limits" vs the document's own fixed numbers.** The
locked text prohibits hardcoded limits, then fixes loop 0–10, volume 0–200,
960-sample frames, 48 kHz, and Opus bitrate profiles in the same document.
Verdict: **protocol and physical constants are not "hardcoded limits".**
Loop 0–10 and volume 0–200 are wire-format ranges in the IPC contract (the
`vc.loop` count range and `vc.volume` range); changing them changes the wire
format. 960 samples is 20 ms at 48 kHz — arithmetic, not a choice. 48 kHz is
the WebRTC/Opus native rate. The prohibition covers **operational values**:
rate limits, per-action timeouts, duration/queue caps, cache TTLs, prefetch
lead times, and the Opus bitrate ceiling — all of which go in the config
schema (§1.4). The Opus bitrate ceiling additionally has no binding surface
today (§3.3, OD-1).

**R7 — Latency table numbers are estimates, not measurements.** The locked
§4 table asserts curl IPC ~25 ms, UDS ~0.08 ms, InnerTube 120–450 ms, warm
yt-dlp ~900 ms, symphonia open 150–300 ms, cache hit ~0.05 ms. None of these
were measured in-tree; the current measured numbers are /play YouTube
6.5–13.7 s and JioSaavn 0.5 s (BENCHMARKS.md). Verdict: the table stands as
the design's **targets**, not as facts. Phase 1 telemetry must measure the
real numbers and replace the table; Phase 6 exit benchmarks are judged
against measured values, not against this table's estimates.

**R8 — "2 GB" upload claim.** The locked text says removing the Bot API
loop lifts the file cap to 2 GB. That is Telegram's documented server-side
MTProto limit, not something verifiable from the tree (no size constant
exists in the TDLib client). Verdict: accepted as a documented Telegram
limit; Phase 6 live test must verify an actual >50 MB upload end to end.

**R9 — The locked text's Phase 1 says "verify libntgcalls.so /
libtdjson.so symbols".** Done in finalisation (§3). Verdict: the Phase 1
symbol gate is satisfied by this document for the libraries on disk today;
implementation re-checks at link time in Phase 2 (C++) and Phase 3 (cgo)
against the same libraries, and fails the phase if a required symbol is
absent.

**R10 — Old work superseded.** Old Phase 8 linking conversion and old speed
phase are superseded by this architecture (absorbed into Phases 1–4). Do not
resume them as separate work.

---

## §7. Locked Telegram UI specification (user decision 2026-11-03 ~12:59 IST)

The uploaded locked document contains no Telegram UI spec. This section is
locked by the user's follow-up decision and has the same standing as §1.
It governs every message, card, panel, menu, and button the user sees.

### §7.1 Rendering layer: latest-layer rich messages

Message UI is built on the **latest-layer rich messages** — the rich-message
layer from the Hermes/richgram work (rich inline buttons, rich components),
not plain-text fallbacks. Rules:

- Every user-facing message is a rich message: structured components
  (headings, code/pre blocks, tables, collapsibles, copy pills, links) where
  the content calls for them, rendered through the rich-message layer.
- Plain-text fallback is not a rendering mode. If a component cannot render
  on a given surface, the message is redesigned for that surface — never
  silently downgraded to plain text.
- The rich-message component set used is the one established by the
  Hermes/richgram work. **DECIDED in Phase 2 (OD-8)** — see the binding
  note below. The renderer is the rich layer, not ad-hoc HTML strings.
- Phase 9's UI inventory (785 elements, `UI_INVENTORY.md`) remains the
  content source: what each message says, what each card shows, what each
  menu contains. §7 governs **how** it is rendered, not what it says.

**Binding (OD-8, decided Phase 2).** The concrete layer is the
**Hermes/richgram rich-message layer**, as it exists in the sandbox in two
Python implementations: (1) the **richgram** package —
`~/workspace/richgram/richgram/rich_ui.py` (component builders) and
`telethon.py` (Telethon transport); (2) **Hermes's own MTProto layer** —
`~/workspace/hermes-agent/plugins/platforms/telegram/mtproto_rich.py` and
`rich_blocks.py`, documented in
`RICH_MESSAGES_LAYER_232.md` (Telethon v1.45.0, MTProto layer 232).
**Both implementations are Python-only; the C++ core cannot link either.**
The binding is therefore a wire-model binding, not a link binding: the
C++ renderer (`services/core-cpp/src/ui_texts.hpp`) emits richgram's exact
component vocabulary as Rich HTML and sends it as a native rich message
over the core's own TDLib bot session (OD-7, `data/tdlib-bot`), with no
Python in the path and no Bot API traffic.

*Component set (richgram `rich_ui.py` line refs) and C++ mapping:*

| Component | Source | C++ core renders as |
|---|---|---|
| Heading h1–h6 | `rich_heading` (:226) | `<h1>`–`<h6>` |
| Note (blockquote, expandable) | `rich_note` (:231) | `<blockquote expandable>` |
| Inline code | `rich_code` (:236) | `<code>` |
| Pre / pre-with-language | `rich_pre` / `rich_pre_rich` (:240, :247) | `<pre><code class="language-…">` |
| List (ul/ol) | `rich_list` (:390) | `<ul>` / `<ol>` / `<li>` |
| To-do list (checkboxes) | `rich_todo_list` (:424) | checkbox `<li>` |
| Table / key–value table | `rich_table` / `rich_kv_table` (:757, :773) | `<table>` / `<th>` / `<td>` |
| Details / collapsible group / nested (max 16 levels) | `rich_details` / `rich_details_group` / `rich_details_nested` (:804, :852, :891) | `<details><summary>` |
| In-body button / button row | `rich_button` / `rich_button_row` (:723, :750) | in-body `<tg-button>` pills (`url` / `callback_data` / `copy_text` / `disabled`; styles primary / success / danger / link / default); ordinary `ReplyInlineMarkup` carries the `pl:` callbacks (§7.2) |
| Footer / page index (italic) | `rich_footer` / `rich_page_index` (:460, :464) | `<footer><i>` |
| Pull quote | `rich_pull_quote` (:497) | `<aside>` |
| Math | `rich_math` (:501) | `<tg-math-block>` |
| Anchor | `rich_anchor` (:505) | `<a name>` |
| Thinking | `rich_thinking` (:509) | `<tg-thinking>` |
| Image | `rich_img` (:222) | `<img>` / photo caption |
| Custom emoji (`<tg-emoji>`) | `rich_emoji` (:547) | **NOT USED** — forbidden by §7.3 |
| Escaping / plain-text conversion / captions | `rich_esc` (:187), `rich_to_plain` (:1011), `rich_caption` (:1032) | C++ equivalents in the renderer; no plain-text fallback mode (§7.1) |

*Wire model (verified against TDLib 1.8.69 in-tree,
`phase4/tdlib-src/td/generate/scheme/td_api.tl`):* TDLib natively supports
rich messages — `richMessage` (:128), `inputRichMessage` (:154),
`inputRichMessageMedia` (:133), `richMessageSourceHtml` (:144, "for bots
only"), `richMessageSourceMarkdown` (:141, "for bots only"),
`richMessageSourceBlocks` (:138), `inputMessageRichMessage` (:6683) —
accepted by `sendMessage` / `editMessageText`, with limits exposed as
options `rich_message_text_length_max` / `block_count_max` / `depth_max` /
`media_count_max` / `table_column_count_max` and the `InputPageBlock`
vocabulary at :6577–6670. The core sends each rendered message as
`inputMessageRichMessage { message: { "@type": "inputRichMessage",
source: { "@type": "richMessageSourceHtml", text: <rich HTML>, media: [] },
is_rtl: false, detect_automatic_blocks: false } }`. Components with no
TDLib/HTML equivalent are redesigned per §7.1, never silently downgraded
to plain text. (Send-side reference: richgram `telethon.py`
`telethon_send_request` / `telethon_edit_request` build
`messages.SendMessageRequest` with
`rich_message=InputRichMessageHTML/Markdown/blocks` (:180, :211, :153),
telethon ≥1.45; `telegram_api.tl:2257–2259` `InputRichMessage/HTML/Markdown`,
`:2628` `sendMessage` `rich_message` flag. No `tg-button` string exists
client-side in tdlib-src — the server parses it.)

### §7.2 Both button kinds

**Inline buttons AND regular buttons (reply keyboards) are both used, as
the UI requires.** Neither kind is the default to the exclusion of the
other:

- Inline buttons for actions on a specific message/card (play, pause, skip,
  queue, panel controls, settings toggles, pagination).
- Reply keyboards for persistent, mode-like controls the user reaches for
  repeatedly (the Phase 9 keyboard surface: player controls, menu entry).
- A surface that Phase 9 built with one kind keeps that kind unless the
  Phase 2 UI pass justifies a change in the routing table (§7.5).

### §7.3 NO emojis — Phase 9 override (reconciliation verdict)

**No emojis anywhere in the UI.** This is a locked override of Phase 9.

- Phase 9 ported emojis from the references (best-over-minimal) — verified
  in finalisation: **549 of 3,257 lines in `ui_texts.hpp` contain emoji**
  (status-line prefixes, card decorations, button labels, section markers).
- Those emoji'd templates are **superseded, not carried forward as-is**.
  Phase 2 rewrites every emoji-bearing template emoji-free. Stripping the
  characters is not enough: templates whose layout depends on an emoji
  prefix (e.g. status lines of the form "❌ | <b>…</b>") are re-laid-out
  with rich components (headings, bold labels, code spans) so the message
  reads correctly with no emoji at all.
- The override applies to every surface: cards, panels, menus, buttons,
  toasts, error messages, inline results, and the Mini App. There is no
  "emoji allowed here" exception list.
- The 785-element parity count is unchanged by the override — the count is
  about elements existing, not about their decoration. Each element is
  re-rendered under §7.1–§7.5.

### §7.4 Colon-based callbacks (kept from Phase 9)

The `pl:<domain>:<verb>[:args]` callback namespace is kept exactly as Phase
9 established it (`ui_texts.hpp:169,172–189`,
`dispatcher.cpp:6252,6257` — `Dispatcher::handleCallback`):

- Format: `pl:` + domain + `:` + verb + optional `:` + args
  (e.g. `pl:player:pause:<chat_id>`, `pl:queue:del:<chat_id>:<token>`,
  `pl:set:<key>`, `pl:help:<topic>`).
- **64-byte limit** — the Bot API `callback_data` limit, documented in
  `ui_texts.hpp:169`. Callers keep payloads short; track ids travel as
  tokens, never as full ids. Any callback that would exceed 64 bytes is a
  design error in Phase 2, caught by the routing-table test (§7.5).
- The namespace is the only callback scheme in the new architecture. No
  second scheme is introduced.

### §7.5 End-to-end routing (pixel-perfect, no dead ends)

Every message, card, panel, menu, and button has its route defined
end-to-end: user action → callback/router → handler → rendered update.
Requirements:

- **Routing table artifact (Phase 2 exit criterion):** a table listing
  every button and every callback with: the surface it appears on, the
  callback_data or command it emits, the router/handler that receives it,
  the backend action (if any) it triggers, and the exact rendered update
  the user sees in return (edited message, new message, toast, panel
  refresh). No row may be blank.
- **Zero unrouted buttons.** A button that exists but has no handler is a
  Phase 2 failure, not a cosmetic issue. (Phase 9's unimplemented buttons —
  `download.cancel`, `/mix`, `/recognize`, `/scan`, `/sharescan`, `/rmcache`
  — are the known cases; OD-3 decides implement vs cut, and the routing
  table records the verdict per button.)
- **Zero unhandled callbacks.** The router has an explicit default branch
  that answers every unknown callback with an honest "unavailable" toast —
  never silence, never a crash, never an un-answered callback query
  (Telegram shows a spinner until answered).
- **Zero dead ends.** Every flow ends in a rendered state the user can act
  on (a panel, a card, a menu, or an honest error with a way back). No
  message that leaves the user with nowhere to go.
- **Test:** the routing-table test enumerates every registered
  `callback_data` value and every reply-keyboard button and asserts a
  handler exists and the callback is ≤64 bytes. This test is part of the
  Phase 2 exit benchmark (§9).

  *Phase 2 completion (Worker E2, 2026-11-03): routing table done —
  `contracts/routing-table.md`, 223 rows (95 callbacks §A, 4 reply-keyboard
  buttons §B.1, 11 URL/switch-inline buttons §B.2, 101 command rows §C
  covering 112 names + 70 aliases + 40 channel commands, 12 inline-mode
  rows §D); 10 OD-3 rows marked "implemented in Phase 3"; 0 blank cells
  (script-checked). Routing test `services/core-cpp/tests/routing_test.cpp`
  green: 935 checks, 0 failures — every callback ≤64 bytes proven by the
  test, zero unrouted buttons, zero unhandled callbacks (2 documented
  §7.5 gaps pinned in the table: reply-keyboard buttons dropped in
  groups, vote callbacks unanswered). Default branch verified present in
  `handleCallback` (unknown callbacks answered with the "Unknown action."
  alert); no code change needed for it. Emoji parameter removed from
  `menuPanelText`/`stateCardText` in the same wave.*

### §7.6 UI parity counting (carries into §10)

The parity checklist counts UI elements under these rules: an element counts
as ported only when it renders as a rich message (§7.1), uses the correct
button kind (§7.2), contains zero emojis (§7.3), routes through the `pl:`
namespace (§7.4), and appears in the routing table with no dead end
(§7.5). An element that exists but violates any of these is counted as NOT
ported.

---

## §8. File-level inventory (DELETE / KEEP / MOVE / REWRITE)

Every service and every core-cpp file. "DELETE as a service" means the
running service goes away; its code remains in the tree as port source until
its features are ported, then the directory is removed in the phase that
ports the last of it. Nothing is deleted before its features are ported —
§1.2 prohibition 2 forbids deleting a feature by deleting its service.

### §8.1 Services

| Service (dir) | Verdict | Notes |
|---|---|---|
| `services/core-cpp` | **REWRITE** | Becomes the Part 1 core. Per-file verdicts in §8.2. |
| `services/vc-go` | **DELETE as service; port into backend-go** | Resolver + queue/room state are the port source for the Go backend (Phase 3). Default engine; its low-RAM profile is the baseline for the unified backend. |
| `services/vc-ts` | **DELETE as service; port into backend-go** | JioSaavn provider, lyrics (LRCLib), control panel, volume, Lemonic search/download, inline modes. JioSaavn resolution must be re-derived (§5 DES finding). |
| `services/vc-rust` | **DELETE as service; port into backend-go** | Queue/loop/shuffle/autoplay logic (ParvatiMusic port) is the reference for the Go room state machine. Its `voice.rs` Opus/RTP pipeline is superseded by parlay-dsp. |
| `services/audio-fx-rust` | **DELETE as service; port into parlay-dsp** | FFmpeg filter chains become the f32 effect chain (Phase 5). The `fx.apply` action survives as a backend action. |
| `services/share-rust` | **DELETE as service; port into backend-go** | Share links/tokens move to the embedded store (Phase 3). |
| `services/queue-rust` | **DELETE as service; port into backend-go** | Voting queue moves to the embedded store (Phase 3). Vote threshold becomes config. |
| `services/spotify-rust` | **DELETE as service; port into backend-go** | Watch registry, dislike store, OAuth tokens (AES-256-GCM), skip-check classifier move to the embedded store + Go (Phase 3). |
| `services/radio-js` | **DELETE as service; port into backend-go** | Station list, ffprobe reachability → native Go verifier (Phase 3). |
| `services/netease-go` | **DELETE as service; absorb into backend-go** | weapi/eapi/linuxapi, search, FLAC download, synced lyrics (Phase 3). |
| `services/downloaders` (Python) | **DELETE as service; port into backend-go + yt-dlp daemon** | Native Go resolvers replace it (Phase 3); the persistent yt-dlp worker (`scripts/yt_dlp_worker.py`, new) is the only Python that survives, as a warm fallback. Deezer Blowfish-CBC + ARL logic is the port source for the Tier 1 Deezer client. |
| `services/gui-js` | **DELETE as service; port into backend-go + Mini App** | Single-page player becomes the typed TypeScript Mini App served by the backend (Phase 6). |

New directories: `services/backend-go` (Part 2), `services/parlay-dsp`
(Part 3), `services/mini-app` (Part 4, TypeScript source),
`scripts/yt_dlp_worker.py` (persistent daemon). New files: `parlay.toml`
(or `.yaml` — OD-6), `parlay.db` location convention in `PARLAY_DATA_DIR`.

### §8.2 core-cpp files (services/core-cpp/src/)

| File | Verdict | Notes |
|---|---|---|
| `main.cpp` | **REWRITE** | Remove `getUpdates` long-poll loop; add TDLib bot-client loop + assistant-client loop + UDS client connect. Keep signal handling and startup logging. |
| `dispatcher.cpp` / `dispatcher.hpp` | **REWRITE** | Remove 5-second watcher; replace 1 cmd/s silent drop with token-bucket limiter; keep command table, aliases, channel-commands, gates, permission hierarchy. UI rendering moves to the §7 rich layer. Callback router (`handleCallback`, `pl:` namespace) kept and extended to the §7.5 routing table. |
| `http.cpp` / `http.hpp` | **DELETE** | curl-CLI spawner. No replacement file with the same role; the persistent transport replaces it (§8.2 ipc row). |
| `ipc.cpp` / `ipc.hpp` | **REWRITE** | Becomes the persistent duplex UDS/gRPC client (OD-4). Same logical role (core↔backend calls), new transport. Per-action deadlines from config. |
| `engine_router.cpp` / `engine_router.hpp` | **DELETE** | Fallback router. Single backend; no fallback order, no `/healthz` pre-probes. |
| `voice_call.cpp` / `voice_call.hpp` | **DELETE** | Core never touches ntgcalls. Join signalling moves into the dual-session TDLib module (`tdlib_client` rewrite); media/frame delivery is the backend's job. |
| `tdlib_client.cpp` / `tdlib_client.hpp` | **REWRITE** | Dual-session: bot session (`checkAuthenticationBotToken`) + assistant session (phone/code/2FA, `data/tdlib` 0700). Build-time link to libtdjson (no dlopen). Add `checkAuthenticationBotToken` — verified absent today (§4). |
| `telegram_api.cpp` / `telegram_api.hpp` | **DELETE** | Bot API HTTP client. Dies with the long-poll loop. |
| `core_api.cpp` / `core_api.hpp` | **DELETE** | Core :8080 GUI API. The backend serves the Mini App and the state API (Phase 6). |
| `http_server.cpp` / `http_server.hpp` | **DELETE** | Core :8080 HTTP server (§6-R1). |
| `store.cpp` / `store.hpp` | **DELETE** | Mongo/Redis/JSON store. Backend embedded store replaces it. Core keeps only in-memory ephemeral state (rate-limiter buckets, in-flight joins). |
| `scram.cpp` / `scram.hpp` | **DELETE** | Mongo SCRAM auth. Dies with Mongo. |
| `config.cpp` / `config.hpp` | **REWRITE** | Unified config schema (`parlay.toml`/`.yaml` + env overrides, §1.4). No scattered env vars. |
| `json.cpp` / `json.hpp` | **KEEP** | JSON utility. Still needed (TDLib JSON, config parsing). |
| `ntgcalls.h` | **DELETE** | Core never touches ntgcalls. (The backend links the real header via cgo.) |
| `ui_texts.hpp` | **REWRITE** | Prune the 245 dead consts / 70 dead helpers (§6-R5); rewrite all 549 emoji-bearing lines emoji-free (§7.3); re-render every template through the §7 rich layer; keep the `pl:` callback builders. |
| `tests/` (8 files) | **DELETE** | Legacy test preservation is NOT a constraint (§1.1). Replaced per phase by new tests/benchmarks (§9). |

### §8.3 Supporting files

| File | Verdict | Notes |
|---|---|---|
| `contracts/ipc.json` | **FREEZE as parity reference; DELETE as wire contract** | The 56 actions are the §10 parity checklist. The HTTP wire protocol it describes is deleted; the UDS/gRPC protocol spec (Phase 2) replaces it. Do not edit it after finalisation except to mark actions ported. |
| `docker-compose.yml` | **REWRITE** | 2 core binaries + slskd (optional). Mongo and Redis services deleted. |
| `install.sh` / `start.sh` / `stop.sh` / `status.sh` | **REWRITE** | New topology, new binaries, new config file, new session-dir layout. |
| `scripts/tdlib_login.py` | **KEEP** | Still the first-auth path for the assistant session (phone/code/2FA/QR → `data/tdlib` 0700). Phase 2 adds the bot-session first-auth path alongside it (bot token needs no login, only API_ID/API_HASH + token). |
| `scripts/package-release.sh` / `release/` | **REWRITE** | New binaries + libraries in the tarball (backend-go, parlay-dsp, libntgcalls.so, libtdjson.so). No v0.2.0-era service binaries. |
| `CURRENT-STATE.md` | **KEEP (untouched)** | Not part of this architecture. Left alone. |
| `PHASE9.md` / `UI_INVENTORY.md` / `phase9/` | **KEEP as reference** | UI content source (§7) and residual record (§6-R5). Not executed against after finalisation. |
| `refs/` (21 references) | **KEEP, read-only** | Port sources. Do not modify. (19 LICENSE files already deleted per user order — do not re-raise licensing.) |
| `BENCHMARKS.md` | **REWRITE in Phase 1** | Fastest-resolution-strategy-per-source record + measured stage timings. |

---

## §9. Per-phase execution plan

Do not advance to Phase N+1 until Phase N builds cleanly and passes its
exit benchmark. Each phase lists entry criteria (what must be true before
it starts), work items, and exit benchmarks (what must be true before the
next phase starts).

### Phase 1 — Telemetry and engine benchmark

- **Entry:** this document confirmed by the user; no implementation started.
- **Work:** add microsecond stage timers in core-cpp and the three engines
  (`t_dispatch_to_ipc`, `t_resolve` per source, `t_voice_join`,
  `t_first_frame`); benchmark vc-go, vc-ts, vc-rust head-to-head on every
  supported source (YouTube, JioSaavn, Deezer, NetEase, Spotify, Navidrome,
  Soulseek, TikTok, Radio); cross-reference the refs (TgMusicBot,
  YukkiMusic, ArcMusic, Music163bot-Go, TGVCBot, ParvatiMusic, lemonic,
  Spotify-Loader, music-bot Navidrome); re-derive the JioSaavn resolution
  strategy (§5 DES finding — do not assume DES-ECB); record the fastest
  resolution strategy per source in `BENCHMARKS.md` and lock that exact
  algorithm for the Go backend port; re-verify libntgcalls.so / libtdjson.so
  symbols at link time (§3 already verified them on disk).
- **Exit benchmarks:** stage timers emitting on every /play; `BENCHMARKS.md`
  has a per-source winner with measured numbers; the locked §4 latency table
  is replaced by measured values (§6-R7); JioSaavn strategy documented with
  its actual mechanism; no code deleted yet.

### Phase 2 — Persistent IPC and dual-TDLib C++ core

- **Entry:** Phase 1 exit benchmarks met; OD-4 (UDS vs gRPC-over-UDS)
  decided; OD-6 (config format) decided; OD-8 (rich layer binding) decided.
- **Work:** delete `http.cpp` (curl CLI); replace `IpcClient` with ONE
  persistent bidirectional connection (UDS or gRPC over UDS) to the Go
  backend, connection pooling/multiplexing, per-action deadlines from
  config; switch core-cpp to build-time linking of libtdjson; add
  `checkAuthenticationBotToken` bot session alongside the assistant session
  (dual TDLib clients in one process); implement token-bucket rate limiting
  with burst capacity (replacing `dispatcher.cpp:911`); remove the Bot API
  `getUpdates` long-poll loop and `telegram_api.*`; delete `engine_router.*`
  (the backend is not built yet — the core talks to a stub backend that
  answers the UDS protocol, or the phase lands with the transport + core
  only and the backend arrives in Phase 3; the phase does not advance until
  the transport is proven against a real peer); move voice signalling to the
  duplex handshake (`JoinRequest`/`JoinResponse`); rewrite `ui_texts.hpp`
  under §7 (prune dead strings, emoji-free, rich layer, `pl:` namespace);
  produce the §7.5 routing table.
- **Exit benchmarks:** core builds clean with build-time libtdjson link
  (missing `.so` fails at link/startup); bot session receives Telegram
  updates over MTProto (no Bot API traffic); token-bucket limiter proven
  (burst allowed, sustained rate enforced, no silent drops — denials answer
  honestly); UDS round-trip measured and recorded; routing-table test green
  (every registered callback has a handler, every callback ≤64 bytes, zero
  unrouted buttons); no `http.cpp`, no `getUpdates`, no `engine_router.*`
  remain in the tree.

### Phase 3 — Unified Go backend, embedded store, native resolvers

- **Entry:** Phase 2 exit benchmarks met; OD-2 (state migration) decided;
  OD-5 (bbolt vs pebble) decided; Phase 1's per-source winners locked.
- **Work:** merge vc-go, vc-ts, vc-rust, share-rust, queue-rust,
  spotify-rust, radio-js, netease-go, and Python downloaders into
  `backend-go` backed by the single embedded store and the persistent yt-dlp
  fallback worker (started once at Go boot, stdin/stdout JSON-RPC, warm
  `YoutubeDL` in RAM); link libntgcalls.so via cgo (TgMusicBot pattern);
  implement the quality ladder (Tier 1 FLAC → Tier 2 320k → Tier 3 adaptive);
  port all 11 services' features; delete Mongo/Redis dependencies and the
  11 HTTP servers; wire the VoiceJoin handshake to the core.
- **Exit benchmarks:** **100% feature parity across all 56 IPC actions**
  (§10 checklist, every box ticked with a test); all 11 services' features
  present in the backend (each with its §10 line); embedded store proven
  (kill −9 and restart preserves queue/state); yt-dlp daemon proven warm
  (second resolve shows no process fork); no Mongo/Redis process running; no
  HTTP server on ports 8001–8010/3000/8080.

### Phase 4 — Cache and lookahead prefetch

- **Entry:** Phase 3 exit benchmarks met.
- **Work:** TTL-aware URL/metadata cache in the embedded store (canonical
  `<source>:<track_id>`, expiry from CDN `expire=` signatures); automatic
  N+1 prefetch/pre-open (background goroutine resolves N+1 on track start or
  promotion, hands the stream handle to parlay-dsp to pre-buffer opening
  seconds).
- **Exit benchmarks:** cached `/play` resolves in ~0 ms (measured, not
  asserted); prefetch verified (N+1 stream handle pre-buffered before N
  ends); cache expiry proven (expired CDN URL is re-resolved, never served
  stale); prefetch never blocks the playing track.

### Phase 5 — parlay-dsp Rust engine and cgo integration

- **Entry:** Phase 4 exit benchmarks met; **OD-1 (Opus encoder config)
  decided** — this phase cannot start its audio path until OD-1 is resolved,
  because the choice determines whether the DSP encodes Opus (option c) or
  pushes PCM (options a/b).
- **Work:** build parlay-dsp (symphonia + rubato + float FX + limiter +
  TPDF dither + gapless dual-slot decoder); confirm the expected PCM format
  for `ntg_send_external_frame` against the StreamManager external-reader
  init path (§3.2 caveat) before binding the DSP output; link into
  backend-go via cgo to feed ntgcalls; benchmark CPU, RAM, and first-frame
  latency against the legacy ffmpeg subprocess pipeline.
- **Exit benchmarks:** gapless transitions sample-accurate (measured: zero
  silence/click at boundaries); zero-drop effect switching (parameter
  changes mid-track produce no click/pop, verified by waveform inspection);
  first-frame latency measured and recorded vs the legacy ffmpeg pipeline;
  CPU/RAM measured and recorded; Opus output verified at the locked bitrate
  (gated on OD-1).

### Phase 6 — TypeScript lossless frontend and live VPS proof

- **Entry:** Phase 5 exit benchmarks met.
- **Work:** integrate the TypeScript Mini App + FLAC streaming endpoint
  (`GET /api/v1/stream/:chat_id/flac`, HTTP 206 Partial Content); run full
  end-to-end live voice-chat verification on the VPS; verify cold `/play`
  (<1–3 s YouTube, <200 ms JioSaavn/NetEase, ~0 ms cached/prefetched);
  verify live gapless transitions and zero-drop effect switching; Opus
  bitrate test 320 → 256 → 192 kbps on the live VPS call path and lock the
  verified ceiling (gated on OD-1); verify a >50 MB upload end to end
  (§6-R8).
- **Exit benchmarks:** all latency targets measured on the live VPS (not
  the sandbox); gapless + zero-drop FX proven on a real call; Opus ceiling
  locked in config; Mini App streams bit-perfect FLAC while the voice chat
  runs; full parity checklist (§10) re-verified live; release cut.

---

## §10. Parity checklist (100% = every box ticked with a test)

### §10.1 IPC actions (56) — from `contracts/ipc.json` v0.3.1 at d5065b9

vc: `vc.play`, `vc.pause`, `vc.resume`, `vc.skip`, `vc.stop`, `vc.seek`,
`vc.queue`, `vc.loop`, `vc.shuffle`, `vc.autoplay`, `vc.autoplay_skip`,
`vc.volume`, `vc.now_playing`, `vc.progress`, `vc.panel`, `vc.lyrics`,
`vc.join`, `vc.mute`, `vc.radio`. Downloads: `download.youtube`,
`download.deezer`, `download.tiktok`, `download.navidrome`,
`download.spotify`, `download.spotify_playlist`, `download.soulseek`,
`download.progress`. NetEase: `netease.search`, `netease.download`,
`netease.lyrics`. Spotify: `spotify.resolve`, `spotify.collection`,
`spotify.status`, `spotify.auth`, `spotify.watch`, `spotify.watches`,
`spotify.dislike`, `spotify.skip_check`, `spotify.now_playing`,
`spotify.details`, `spotify.analyze`. Sharing/voting/FX/radio/search:
`share.create`, `share.resolve`, `queue.vote`, `fx.apply`, `radio.list`,
`radio.start`, `radio.stop`, `radio.status`, `search.query`,
`search.inline`, `lemonic.search`, `lemonic.download`. GUI/settings:
`gui.now_playing`, `gui.queue`, `chat.settings`. (Count: 19 + 8 + 3 + 11 + 12 + 3 = 56.)

### §10.2 Telegram UI elements (785) — under the §7 rules

Source: `UI_INVENTORY.md` (533 ported, 115 upgraded by a better ref, 137
not-applicable, 0 unaccounted). An element counts as ported only when it
meets §7.6 (rich message, correct button kind, zero emojis, `pl:` callback,
in the routing table with no dead end). The 137 not-applicable elements stay
not-applicable; the parity denominator is the 648 applicable elements
(533 + 115). The §7.5 routing table is the counting instrument.

### §10.3 Per-service features (11 services)

- **vc-go:** queue/room state machine, loop/shuffle/autoplay, resolver,
  low-RAM profile, Mongo/Redis/JSON persistence semantics (now embedded).
- **vc-ts:** JioSaavn provider + real durations, lyrics (LRCLib), control
  panel, volume 0–200, Lemonic search/download, inline modes, raw-stream /
  radio play.
- **vc-rust:** download + loop/shuffle/autoplay, Opus/RTP pipeline semantics
  (now parlay-dsp), ParvatiMusic room behaviour.
- **audio-fx-rust:** all five effects (slow-reverb, nightcore, bass-boost,
  8d, original) as the f32 chain.
- **share-rust:** anonymous link minting and resolution.
- **queue-rust:** voting queue with configurable threshold.
- **spotify-rust:** watch registry, dislike store, OAuth (AES-256-GCM),
  skip-check classifier, collection/resolve/status.
- **radio-js:** station list, reachability probing, playlist fallback, VC
  handoff.
- **netease-go:** weapi/eapi/linuxapi, search, FLAC download, synced lyrics.
- **downloaders:** YouTube, Deezer (ARL + Blowfish-CBC), TikTok, Navidrome,
  Spotify, Spotify playlists, Soulseek via slskd, progress reporting.
- **gui-js:** Mini App player, now-playing/queue views, play/pause/skip
  controls (now the Part 4 frontend).

### §10.4 Phase 9 residuals — disposition (from §6-R5)

- Dead strings (245 consts / 70 helpers): DELETE in Phase 2. Not counted.
- `download.cancel` / `library.delete`, `/mix`, `/recognize`, `/scan`,
  `/sharescan`, `/rmcache`, Spotify companion surfaces, download prefs
  contract fields, inline `Song not cached`, inline `np` oversize: decided
  by OD-3 (implement vs cut); each gets a routing-table row either way.
- `vc.lyrics` method: POST (verdict §6-R5).
- `/status` counts: read from the backend (Phase 3+).

---

## §11. Open decisions (each needs the user)

| # | Question | Locked text's position | What is needed from the user |
|---|---|---|---|
| OD-1 | Opus encoder configuration: libntgcalls.so 3.0.5 exposes NO Opus config API (§3.3). | §1.4 demands OPUS_APPLICATION_AUDIO, complexity 10, VBR on, FEC off, bitrate 320→256→192. The requirement stands; the mechanism does not exist. | **DECIDED 2026-11-03 ~13:20 IST by the user: option (a) — patch and rebuild ntgcalls from source to expose encoder config.** Recorded in Phase 1. Blocks Phase 5's audio path; the patch/rebuild itself is Phase 5 work (Phase 1 only records the verdict). |
| OD-2 | Existing Mongo/Redis/JSON state when the embedded store replaces them. | Locked text orders the embedded store, says nothing about existing state. | **DECIDED 2026-11-03 ~13:51 IST by the user ("Start fresh. You already have everything you need."): FRESH START.** Existing Mongo/Redis/JSON state is NOT migrated; the embedded store starts empty. Lost by this choice: per-chat queues, engine choices, Spotify OAuth tokens (every Spotify user logs in again), share links/tokens, votes, per-chat settings, radio state. Blocks Phase 3 store work (entry now satisfied). |
| OD-3 | Phase 9 unimplemented live surfaces: `download.cancel`, `library.delete`, `/mix`, `/recognize`, `/scan`, `/sharescan`, `/rmcache`, Spotify companion surfaces, download prefs contract fields, inline `Song not cached`, inline `np` oversize. | §1.2 demands 100% parity; the surfaces exist but have no backend action. | **DECIDED 2026-11-03 ~13:51 IST (delegated by the user: "Start fresh. You already have everything you need." — the locked parity rule decides it): IMPLEMENT all of them. No cuts.** Cutting any of these would violate the locked §1.2 100%-parity rule. Each needs a new backend action, which lands in Phase 3. Blocks the §7.5 zero-unrouted-buttons test until Phase 3 implements the actions; the routing table records each as "implemented in Phase 3" in Phase 2. |
| OD-4 | Persistent transport: Unix Domain Socket (length-prefixed binary frames) or gRPC over UDS. | Locked text allows either ("Unix Domain Socket or gRPC over UDS"). | **DECIDED 2026-11-03 ~13:51 IST (delegated by the user; spec recommendation adopted): plain UDS length-prefixed binary frames** (lowest latency, no extra dependency). Blocks Phase 2 — entry now satisfied. |
| OD-5 | Embedded store: bbolt or pebble. | Locked text says "e.g., bbolt or pebble". | **DECIDED 2026-11-03 ~13:51 IST (delegated by the user; spec recommendation adopted): bbolt** (single file, simplest). Recorded now; Phase 3's store entry is satisfied. |
| OD-6 | Config file format: `parlay.toml` or `parlay.yaml`. | Locked text says "parlay.toml / parlay.yaml + Env Overrides". | **DECIDED 2026-11-03 ~13:51 IST (delegated by the user): `parlay.toml`.** Rationale: Go and Rust both speak TOML natively, strict types, no YAML indentation ambiguity, and it is the locked text's first-listed option. Blocks Phase 2 (config schema) — entry now satisfied. |
| OD-7 | Bot-session TDLib dir: the assistant session lives in `data/tdlib` (0700); the locked text never says where the bot session lives. | Silent. | **DECIDED 2026-11-03 ~13:51 IST (delegated by the user; spec convention adopted): bot-session TDLib dir is `data/tdlib-bot` (0700), separate from the assistant session's `data/tdlib` (0700).** |
| OD-8 | Rich-message layer binding: which concrete component set from the Hermes/richgram work the core's renderer binds to. | §7.1 requires the latest rich layer, names no API. | **DECIDED 2026-11-03 in Phase 2 (delegated by the user to Phase 2): the Hermes/richgram rich-message layer** — richgram package (`richgram/rich_ui.py`, `telethon.py`) + Hermes's own MTProto layer (`plugins/platforms/telegram/mtproto_rich.py`, `rich_blocks.py`, Telethon 1.45.0 / layer 232). Both are Python-only, so the binding is a wire-model binding, not a link binding: the C++ renderer emits richgram's exact component vocabulary as Rich HTML and sends it as TDLib 1.8.69 `inputMessageRichMessage` (`richMessageSourceHtml`) via `sendMessage`/`editMessageText` on the bot session. Full component table + wire mapping in §7.1. Custom emoji (`rich_emoji`) NOT used (§7.3). Blocks Phase 2 UI work — entry now satisfied. |
| OD-9 | Live VPS proof prerequisites: the Opus bitrate test (OD-1), the >50 MB upload test (§6-R8), and a real-Telegram callback test all need the user's VPS + a live group call. | Phase 6 requires them. | **DEFERRED to Phase 6 entry (2026-11-03 ~13:51 IST, delegated by the user).** The live VPS proof needs the user's VPS, a test group, and a window; the user is already live-testing on their VPS, so confirm those with the user at Phase 6, not now. Not decided. |
| OD-10 | `/recognize` provider: which music-recognition service `audio.recognize` is built against (inventory §9-E4, §10-U4). | §9-E4 specified the action shape fully but left the provider open (AudD / ACRCloud / self-hosted fingerprinting); until configured the action answers 502 honestly. | **DECIDED 2026-11-03 ~16:57 IST by the user: AudD (free tier, simple API).** `audio.recognize` is built against AudD's HTTP API, env-gated on `AUDD_API_TOKEN`, honest 502 while unconfigured. Recorded in Phase 3 Wave 2. |

**Note (2026-11-03 ~17:23 IST, user decision — skip NetEase account creation, proceed with the variables we have).** No NetEase `MUSIC_U` is obtainable right now: mailbox registration and Cloud Music phone login both need a mainland China (+86) mobile for SMS, which the user has none of, and WeChat login needs the phone app (QR-only). The NetEase resolver stays env-gated and dormant for lossless; anonymous search already works and is the only live NetEase path. Deezer is also dropped for now (no ARL; sandbox geo-blocked signup — user: "Deezer don't"); the Deezer resolver stays env-gated and dormant too. Neither blocks verification — both fail honestly without keys. Variables we DO have: the Spotify app "Parlay" exists in the user's dashboard (redirect URI `http://127.0.0.1:8080/callback`) and the user is logged into AudD's dashboard. The user copies the Spotify client ID/secret and the AudD token into the VPS `.env` themselves — never through chat, NOT in the sandbox `.env`. `/download` link routing is unchanged: Deezer links still route to `download.deezer`, which fails honestly without an ARL.

---

## Appendix A. Latency targets (locked §4 table — estimates, §6-R7)

| Pipeline stage | Current (d5065b9) | New (cold, uncached) | New (cached / prefetched) |
|---|---|---|---|
| Telegram update receipt | HTTPS Bot API long-poll (30 s) | TDLib MTProto push | TDLib MTProto push |
| Core → backend IPC | curl CLI fork + TCP: ~25 ms (estimate) | Persistent UDS socket: ~0.08 ms (estimate) | Persistent UDS socket: ~0.08 ms (estimate) |
| Track and stream resolution | Python yt-dlp fork: 6.5–13.7 s (measured) | Go InnerTube / native resolver: 120–450 ms (estimate; warm yt-dlp daemon fallback ~900 ms) | Embedded URL cache hit: ~0.05 ms (estimate) |
| Decoder and resampler init | ffmpeg subprocess fork | In-process symphonia HTTP range open: 150–300 ms (estimate) | Pre-buffered in Slot B: 0 ms |
| Track advance delay | 5,000 ms polling watcher (measured) | Sample-accurate callback: 0 ms | Sample-accurate callback: 0 ms |

Phase 1 replaces every "estimate" above with a measured number.
Phase 6 exit benchmarks are judged against the measured numbers and the
locked cold-`/play` targets (<1–3 s YouTube, <200 ms JioSaavn/NetEase,
~0 ms cached/prefetched).

## Appendix B. What finalisation verified and did not do

Verified (evidence in §2–§5): re-anchor diff, IPC action count, symbol
exports on both libraries, every named code location, every named reference
implementation, Phase 9 residuals, emoji count in `ui_texts.hpp` (549 of
3,257 lines), `pl:` namespace and 64-byte limit, the absence of a yt-dlp
daemon and of bbolt/pebble in the tree.

Not done (per the finalisation brief): no implementation code was written or
modified, no service file was modified, no build or test was run that changes
the tree, nothing was pushed, `.env` was not touched, no service was
started with the user's token, no release or tag was created. The only file
written is this document.
