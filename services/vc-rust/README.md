# vc-rust — Rust VC engine (fallback)

From: Iotatg ParvatiMusic (`bot/src/modules/play.rs`,
`bot/src/modules/{queuecmd,pause,skip,stop,vc}.rs`, `bot/src/room.rs`).
Native Ferogram MTProto + ntgcalls, live message editing, cancel download.
Same `vc.*` IPC as vc-go/vc-ts; the core falls back here when Go/TS fail.

## Implemented (real)

- Full queue/playback state machine over the IPC contract
  (`contracts/ipc.json` `vc.*`): `POST /v1/play|pause|resume|skip|stop|`
  `seek`, `POST /v1/queue` (`add|remove|clear|list`),
  `GET /v1/now-playing?chat_id=`, `GET /healthz`.
- Envelopes `{ok:true,data}` / `{ok:false,error:{code,message}}`;
  `X-Parlay-Token` auth when `PARLAY_TOKEN` is set (401 without/wrong,
  `/healthz` open).
- Per-chat rooms: current track, queue, paused flag, `position_sec`,
  volume. In-memory + JSON-file persistence (`data/vc-state.json`,
  atomic write, restored on restart; `PARLAY_DATA_DIR` overrides the
  data dir).
- 1-second auto-advance ticker: position advances while playing; at
  track end the next queued track starts, or the room goes idle.
- `/play` semantics follow ParvatiMusic: idle room starts the track at
  once; a busy room queues it (`queued:true` in the response).
- Resolve, in order: YouTube URL/ID pattern (`watch?v=`, `youtu.be/`,
  `/shorts/`, `/embed/`, `/live/`, `/v/`, bare 11-char id) → Spotify
  URL pattern (`open.spotify.com/track/<id>`, `spotify:track:<id>`) →
  **yt-dlp subprocess** (`timeout 20 yt-dlp --dump-single-json
  ytsearch1:<query>`, real title/uploader/duration when yt-dlp is
  installed) → degraded pattern resolve (`resolved_by:"pattern"`, a
  deterministic `pattern-<fnv>` id, placeholder 180 s duration, no fake
  stream URL). Every track reports `resolved_by` (`yt-dlp` |
  `url-pattern` | `pattern`) so callers know what they got.
- `now_playing` reports `engine:"rust"`, plus `loop_n`, `shuffle`,
  `autoplay`; each track reports `file_path`, `downloaded`,
  `download_failed`.
- **Download path (Phase 4)** — every track that starts is downloaded
  first to a local mp3 (`data/downloads/chat-<id>/<video_id>.mp3`,
  `yt-dlp -f bestaudio --extract-audio --audio-format mp3`, 300 s cap;
  android-player-client retry on YouTube's bot check, same as the
  downloaders service). A failed download never stalls the room: the
  track is marked `download_failed:true` and the ticker keeps advancing
  state-wise; the ticker also freezes `position_sec` while a download
  is still in flight. yt-dlp is found via `$YTDLP_BIN`, `yt-dlp` on
  PATH, or the downloaders venv
  (`services/downloaders/.venv/bin/yt-dlp`). Live-tested 2026-11-02:
  Rick Astley "Never Gonna Give You Up" → 7,045,581-byte mp3, 213 s,
  in ~18 s.
- **Loop (Phase 4)** — `POST /v1/loop {chat_id, count}` (`0..=10`,
  ParvatiMusic `room.rs::loop_n`): the current track repeats `count`
  times, then the queue resumes. `/skip` while looping replays the
  track and drains the count (live-tested).
- **Shuffle (Phase 4)** — `POST /v1/shuffle {chat_id, enabled}`:
  enabling shuffles the queue immediately (Fisher–Yates) and makes
  advance pick a random queue index (ParvatiMusic `next_track`).
- **Autoplay (Phase 4)** — `POST /v1/autoplay {chat_id, enabled}` and
  `POST /v1/autoplay-skip {chat_id}` (ParvatiMusic `/askip`): when the
  queue empties with autoplay on, the ticker picks from the YouTube mix
  playlist `RD<video_id>` (flat fetch, first entry not in play
  history), downloads it and starts it; `/autoplay-skip` forces a mix
  pick now and resets the loop count. Live-tested 2026-11-02: mix of
  "Never Gonna Give You Up" (360 entries) → Justin Timberlake "CAN'T
  STOP THE FEELING!" (286 s), mp3 downloaded and playing. The mix
  endpoint is occasionally gated from this egress IP (fast failure);
  the fetch retries once with the android player client, and the
  ticker falls back to stopping the room when no pick is available.
- Play history (last 200 track ids per room) is kept for autoplay
  exclusion and persisted with the room state.

## Voice transport (real pipeline, join blocked in sandbox)

- **Opus/RTP pipeline is real** (`src/voice.rs`): ffmpeg encodes the
  track to libopus 48 kHz stereo (Ogg Opus file, ffprobe-verified) and
  packetizes it as RTP (SDP `m=audio 0 RTP/AVP 97`, packet count proven
  by parsing the RTP file — 251 frames / 251 RTP packets on the 5 s
  proof tone). `play_stream` runs it, then attempts the join; now-playing
  reports `voice:"opus-rtp-ready"` once proven, or
  `voice:"unavailable:<exact reason>"` otherwise.
- **Join client is real** (`src/voice.rs` `join`): ntgcalls local offer
  (voice-venv wheel, offline) + voice_bridge.py (Telethon) join-call.
  Blocked in the sandbox: no MTProto session/credentials
  (`ASSISTANT_SESSION_FILE`, `API_ID`, `API_HASH` unset) and UDP sendto
  is EPERM even to loopback, so RTP is proven to a file, never to a
  live receiver. `joined:true` is never reported without a real join;
  the live join is a live-test-time check.

## Stubbed (out of scope in the sandbox)

- No cancel-download path (ParvatiMusic lets the user cancel an
  in-flight download; here a download runs to completion or its 300 s
  timeout inside the request that started the track).

## Run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release --offline   # std-only, zero crates
PORT=8003 ./target/release/parlay-vc-rust
```

Env: `PORT` (default 8003), `PARLAY_TOKEN` (unset = no auth, dev only),
`PARLAY_DATA_DIR` (default `./data`).

## Notes / known gaps

- Position is persisted only on mutations and track changes, not every
  ticker second; after a restart a playing track resumes from its last
  saved position.
- `yt-dlp` is **not** installed in the sandbox, so live resolve is
  pattern-only there; install yt-dlp to get real metadata.
- No loop/shuffle/autoplay yet (ParvatiMusic has them via room flags);
  no per-chat engine config — that lives in the core.
