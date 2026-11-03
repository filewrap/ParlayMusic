# Parlay Phase 3 — Action Semantics Inventory (Worker 4)

Status: **inventory + design only — no code.** Read-only job: this document
is the specification Wave 2 implements from. Anchor commit: `3d84fb6`
(Phase 2 complete; main). Contract authority: `contracts/ipc.json` v0.3.1
(56 actions — request/response field shapes). Transport authority:
`contracts/uds-protocol.md` (UDS frames, deadlines, events, JoinRequest
handshake). Routing authority: `contracts/routing-table.md` (223 rows;
OD-3 rows E1–E11). Architecture authority: `FINAL-ARCHITECTURE.md`
(§1.2 prohibitions, §6-R5, §10 parity, §11 OD-3 decided: implement all).

Sources read (not guessed): every handler file listed per action below —
`services/vc-go` (`server.go`, `queue/room.go`, `resolver/resolver.go`,
`runner/runner.go`, `store/store.go`, `types/types.go`),
`services/vc-ts/src` (`main.js`, `resolvers.js`, `voicecall.js`,
`store.js`), `services/vc-rust/src` (`main.rs`, `voice.rs`),
`services/share-rust`, `services/queue-rust`, `services/spotify-rust`
(`main.rs`, `auth.rs`, `crypto.rs`, `classifier.rs`),
`services/radio-js` (`main.js`, `stations.js`),
`services/netease-go` (`server.go`, `client.go`, `weapi/eapi/linuxapi.go`),
`services/downloaders/src` (`server.py`, `providers.py`, `progress.py`,
`ytdlp.py`, `deezer.py`, `tiktok.py`, `navidrome.py`, `spotify.py`,
`soulseek/*`), `services/gui-js/app/main.js`,
`services/audio-fx-rust/src/main.rs`, and the core side
(`services/core-cpp/src/dispatcher.cpp`, `ipc.cpp`, `ui_texts.hpp`) for
call sites, `chat.settings`, the replied-media path, and the OD-3 gaps.
`BENCHMARKS.md` for the locked per-source resolution winners (Phase 1).

## How to read this document

- **56 actions inventoried, 0 unaccounted.** Each §2 entry has the four
  required parts: (1) contract request/response shape (from `ipc.json`;
  extra fields the old code *actually* returned are listed as
  "superset" — Wave 2 keeps them, see §9-D1); (2) old implementation
  location (file + function) with a behaviour summary; (3) new handler
  plan naming the Phase 3 packages it touches; (4) edge cases (timeouts,
  honest errors, key-gating).
- **Deadlines** (uds-protocol §4/§8): GET-style 10 s (`vc.now_playing`,
  `vc.progress`, `vc.panel`, `vc.lyrics`, `radio.list`, `radio.status`,
  `download.progress`, `share.resolve`, `chat.settings`,
  `spotify.details`, `spotify.now_playing`, `spotify.watches`,
  `spotify.status`, `gui.now_playing`, `gui.queue`, `ping` 5 s);
  everything else 30 s; `download.*` POSTs keep the old explicit
  **120 s** call-site budget (uds-protocol §4 note; `parlay.toml
  [timeouts] download_ms` replaces it in the config rewrite).
- **Envelope**: UDS `{type:"request", request_id, action, chat_id?,
  user_id?, deadline_ms, payload}` → `{type:"response", request_id,
  ok, data | error:{code, message}}`. Error codes (only these):
  `bad_request` (400), `unauthorized` (401), `not_found` (404),
  `engine_unavailable` (502/503), `download_failed` (502),
  `rate_limited` (429), `internal` (500). Unknown action →
  `not_found`. There is no `X-Parlay-Token` on UDS; the socket file
  (0600, in a 0700 data dir) is the access control.
- **Key-gating summary** (full detail per action): Spotify client
  credentials (`SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET`) gate
  `spotify.resolve` enrichment, `spotify.collection` (spotify-rust),
  `spotify.details`, `spotify.analyze`, `spotify.auth`, and the
  Lemonic→Spotify search fallback; `SPOTIFY_REDIRECT_URI` additionally
  gates `spotify.auth` login/callback. `GEMINI_API_KEY` (model
  `gemini-2.0-flash` default) gates the Gemini classifier leg of
  `spotify.skip_check`/`spotify.analyze` (wordlist fallback when absent).
  `GENIUS_ACCESS_TOKEN` / `MUSIXMATCH_API_KEY` gate the credentialed
  lyrics legs (LRCLib is keyless). `DEEZER_ARL` gates full Deezer
  tracks (format 9 FLAC / 3 MP3_320 / 1 MP3_128, `DEEZER_FORMAT`
  default `MP3_320`); without it only 30 s previews. `LEMONIC_API_KEY`
  is optional (unauthenticated Lemonic works; key adds a Bearer).
  `MUSIC_U` (NetEase `music_u` cookie) gates VIP/higher NetEase
  levels (anonymous otherwise). slskd (`SLSKD_URL`/`SLSKD_API_KEY`)
  gates `download.soulseek`. `NAVIDROME_URL`/`NAVIDROME_USER`/
  `NAVIDROME_PASS` gate `download.navidrome` (and `/scan`, §8).
  No key is ever logged; missing keys always answer honest
  `engine_unavailable`, never a faked result.

## §0. Proposed Phase 3 package map (`services/backend-go/`)

`go.mod` already exists (`module parlay/backend-go`, go 1.23.0 — Wave 1
scaffold, no code yet). Packages below are the ones each §2 entry names;
Wave 2 may merge siblings but must not invent a second room store, a
second resolver, or a second download registry.

| Package | Owns |
|---|---|
| `internal/uds` | Frame codec, request dispatch, event emission, JoinRequest handshake (§3.1–§3.4, §5–§6 of uds-protocol). |
| `internal/room` | Per-chat Room + Manager (§4 state machine), position clock, loop/shuffle/autoplay, queue ops, persisted snapshots. |
| `internal/resolver` | Source routing for `vc.play`/`search.*` (§3); native resolvers: YouTube InnerTube (Phase 1 winner), JioSaavn proxy, Lemonic, Spotify-metadata, Deezer, NetEase, Navidrome, Soulseek-via-slskd, TikTok, radio/raw URL; yt-dlp daemon client (warm fallback). |
| `internal/voice` | ntgcalls cgo binding (create/connect/pause/resume/mute/unmute/stop/set_stream_sources/send_external_frame), per-chat call state, JoinRequest → core → remote SDP completion. |
| `internal/store` | bbolt store (OD-5), buckets per domain (§4.6). Fresh start (OD-2): no migration. |
| `internal/download` | Download providers + run registry + IPC-4 progress (§8.3). Owns `download.*`, `download.progress`, `download.cancel` (§9-E1). |
| `internal/spotify` | OAuth token store (AES-256-GCM), app-token cache, watch registry + poller, dislike store, skip-check classifier, details/analyze. |
| `internal/radio` | Station list, native stream verifier, per-chat radio state, VC handoff (§7.2–§7.3). |
| `internal/share` | Share link mint/resolve (§2.25–2.26). |
| `internal/vote` | Voting queue (§2.18). |
| `internal/fx` | FX preset/parameter store (§2.10). No audio processing in Phase 3. |
| `internal/settings` | Per-chat settings, 15 keys (§5). |
| `internal/lyrics` | LRCLib + NetEase synced lyrics, shared by `vc.lyrics` and `netease.lyrics`. |
| `internal/library` | NEW (§8): library index + cache accounting for `library.delete`, `/scan`, `/rmcache`, `/status` counts, inline `Song not cached` / `np` oversize. |
| `internal/search` | `search.query` fan-out + `search.inline` modes (np/s/l/lib/del/help/search/hint). |
| `internal/gui` | Read-only state views for `gui.*` (Mini App serving itself is Phase 6). |
| `internal/events` | Event construction + emission (TrackStarted/TrackAdvanced/TrackEnded/QueueEmpty/DownloadReady/VoiceError) — one emitter, called only from the §4.5 firing points. |
| `internal/netease` | Absorbed netease-go client (weapi/eapi/linuxapi, search, FLAC download, lyrics). |
| `cmd/parlay-backend` | main: config (`parlay.toml`), UDS listener, yt-dlp daemon boot, graceful shutdown. |

Shared rules for every handler: validate `chat_id != 0` first
(`bad_request`); unknown/invalid enum → `bad_request`; nothing playing
→ `not_found` "nothing playing in this chat" (see §9-D5 for the one
exception); resolve failure → `engine_unavailable` (502) with the
resolver's message, never a placeholder track (Phase 5 A9 rule, all
three engines); never invent durations, URLs, lyrics, or verdicts.

---

## §1. Playback core — `vc.*` (19 actions)

### 1.1 `vc.play` — POST, 30 s

- **Contract.** Req: `{chat_id: ChatId, query: string (title, URL or
  track id), requested_by: UserId}`. Resp: `{now_playing: NowPlaying}`.
  (vc-rust additionally returns `queued: boolean` and
  `resolved_by: string`; the core reads `queued` — keep both, §9-D1.)
- **Old impl.** vc-go `server.go:handlePlay` → `resolver.Resolve` →
  `room.Play`; vc-ts `main.js:/v1/play` → `resolvePlayQuery` →
  `startTrack`/`enqueue`; vc-rust `main.rs:handle_play` (+
  `resolve_query` pre-resolve outside the state lock) → `VOICE.join`
  on idle start + `start_track`. All three: resolve first, then
  play-now-if-idle else enqueue (YukkiMusic behaviour). vc-go also
  accepts an optional `volume` (0–200) in the play body.
- **Behaviour.** Full chain is §3. Query routing by source prefix;
  YouTube via yt-dlp (all three old engines) → native InnerTube in
  Phase 3 (Phase 1 winner: 0.61–0.81 s player / 1.56 s search vs
  5.7–6.7 s yt-dlp; yt-dlp stays only as the warm-daemon fallback).
  JioSaavn via `jiosaavn:`/`jsvn:` prefixes (vc-ts only). Raw
  `http(s)` non-YouTube URL or `radio:<url>` → radio track
  (duration 0, never auto-advances). Resolve failure → 502
  `engine_unavailable`, nothing queued. On idle room: current =
  track, clock restarts, `TrackStarted` fires, voice join is
  triggered (§3.4); on busy room: appended to waiting, `queued`
  semantics. Duration limit (60 min, `config.hpp
  duration_limit_min`) is enforced by the **core** after the call
  (it stops the track when `duration_sec` exceeds it) — the backend
  does not enforce it.
- **New plan.** `uds` dispatch → `resolver.Resolve` (source routing,
  §3) → `room.Play` → `voice.EnsureCall` (JoinRequest handshake on
  first play in a chat) → `events.TrackStarted`. Room state in
  `room`; prefetch hook: Phase 4 will resolve N+1 here — Phase 3
  only records `prefetched` slot, no worker yet (vc-go's prefetch
  workers are NOT ported; the 1 s advance ticker is replaced by the
  §4.5 event model).
- **Edge cases.** Empty query → `bad_request`; `chat_id` 0 →
  `bad_request`; yt-dlp bot-check → one retry with
  `youtube:player_client=android`, no cookies; no placeholder track
  ever; `requested_by` 0 is allowed (radio handoff passes 0);
  `playforce`/`vplay` are core-side (stop-then-play, video flag in
  the body — the backend ignores `video`/`force` fields it does not
  know; Wave 2 core drops them from the payload rather than the
  backend accepting unknown fields).

### 1.2 `vc.pause` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{now_playing}`.
- **Old impl.** vc-go `server.go:handlePause` (`room.Pause`, 404 when
  no room/current); vc-ts `main.js:/v1/pause` (same 404 rule, no-op
  when already paused); vc-rust `main.rs:handle_pause` (400 when
  nothing playing — note the code difference, §9-D5) + `VOICE.pause`.
- **Behaviour.** Freezes the position clock (`position_sec` frozen,
  `startedAt` cleared); idempotent when already paused; no-op when
  idle except for the error response. Voice side: ntgcalls pause
  (vc-rust) / pipeline pause (vc-ts voicecall); vc-go has no voice
  side (core owned the call).
- **New plan.** `room.Pause` → `voice.Pause` → snapshot. Persist.
- **Edge cases.** Idle → `not_found` (unify on the vc-go/vc-ts 404,
  §9-D5); already-paused → 200 with unchanged snapshot, no error.

### 1.3 `vc.resume` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{now_playing}`.
- **Old impl.** vc-go `handleResume`; vc-ts `/v1/resume`; vc-rust
  `handle_resume` + `VOICE.resume`. Mirror of pause: restarts the
  clock from the frozen offset; no-op when not paused.
- **New plan.** `room.Resume` → `voice.Resume` → snapshot.
- **Edge cases.** Idle → `not_found`; not-paused → 200 unchanged.

### 1.4 `vc.skip` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{now_playing}`.
- **Old impl.** vc-go `handleSkip` → `room.Skip` (loop reset, then
  `Advance`); vc-ts `/v1/skip` (loop = 0, `popNext`, else idle);
  vc-rust `handle_skip` → `State::advance` + `VOICE.play_stream` on
  Next / `VOICE.stop` on Idle (downloads the next track's audio
  first; a failed download leaves the room playing state-wise, it
  never blocks the skip).
- **Behaviour.** Loop counter resets to 0 (all three). Advance
  order: queue pop (shuffle-aware) → autoplay pick → idle (§4.3).
  Fires `TrackAdvanced` (on Next) or `TrackEnded` + `QueueEmpty`
  (on Idle), §4.5.
- **New plan.** `room.Skip` → `voice` rebind to the new current
  stream (or stop) → events.
- **Edge cases.** Nothing playing but a non-empty queue: vc-rust
  advances (has_something = current OR queue non-empty); vc-go
  returns `ok:false` when `current == nil`. Unify on vc-rust:
  skip with an empty current but a waiting queue starts the next
  track. Fully empty → `not_found`.

### 1.5 `vc.stop` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{}` (empty data).
- **Old impl.** vc-go `handleStop` (`room.Stop`, always 200 even
  with no room); vc-ts `/v1/stop` (same + `voicecall.leaveVoiceChat`);
  vc-rust `handle_stop` (`room` cleared + `VOICE.stop` +
  `VOICE.leave`).
- **Behaviour.** Clears current + whole queue; loop and autoplay
  reset to off; autoplay history cleared; **shuffle is kept**
  (persistent setting, all three); paused cleared. Voice side
  leaves the call. `callActive` is cleared by the core on
  TrackEnded/QueueEmpty/`vc.stop`.
- **New plan.** `room.Stop` → `voice.Leave` → persist (room
  deleted or kept idle? Keep the room object with idle state so
  settings survive; the store drops rooms only when they have no
  current, no queue, and no non-default settings — §4.6).
- **Edge cases.** Always 200, even with no room (idempotent stop);
  never 404.

### 1.6 `vc.seek` — POST, 30 s

- **Contract.** Req `{chat_id, position_sec: integer}`. Resp
  `{now_playing}`.
- **Old impl.** vc-go `handleSeek` → `room.SeekTo` (clamp
  `[0, duration]` when known, 404 when idle); vc-ts `/v1/seek`
  (400 when `position_sec < 0` or non-finite; **no upper clamp** —
  sets the raw value); vc-rust `handle_seek` (clamp `[0, max]`,
  or `max(0)` when duration unknown).
- **Behaviour.** Moves the position clock; if playing, `startedAt`
  resets to now at the new offset; if paused, only the frozen
  offset changes. Core gates: `/seek` requires ≥10 s steps, refuses
  while paused, refuses unknown-duration tracks, clamps forward
  seeks to `duration - 5`; `/jump <mm:ss>` is the same action;
  `/replay` and the Replay button are seek-to-0; `+15s`/`-15s`
  buttons are ±15 s seeks. `/speed` and `/bass` are **not** seek
  actions in the backend — the core stores `speed` (0.5–4.0) /
  `bass` (0–100) as chat fields and re-issues a seek to restart the
  stream at the current position (§4.4); the backend stores both
  per room for the Phase 5 DSP and otherwise ignores them.
- **New plan.** `room.SeekTo` (vc-go/vc-rust clamp) → `voice.Seek`
  (frame-source reposition; Phase 3: rebind stream at offset) →
  snapshot.
- **Edge cases.** Idle → `not_found`; negative → `bad_request`
  (vc-ts rule); beyond duration → clamp to duration (vc-go/vc-rust
  rule, not vc-ts raw); unknown duration → accept any ≥0.

### 1.7 `vc.queue` — POST, 30 s (add/remove/clear/list)

- **Contract.** Req `{action: add|remove|clear|list, chat_id,
  position?: integer (remove), track?: Track (add)}`. Resp
  `{queue: [QueueItem]}`. `QueueItem = {position, requested_by,
  track}`; `position` is **0-based within the waiting queue**
  (the current track is never in the list) — locked semantics, §4.2.
- **Old impl.** vc-go `server.go:handleQueue` (list: empty array
  when no room; add: requires track id+title, defaults empty source
  to `youtube`, creates the room; remove: requires position,
  out-of-range → `bad_request`; clear: empties waiting, returns
  `[]`); vc-ts `main.js:/v1/queue` (same, plus add accepts `query`
  instead of a track and resolves it; list returns `room.queue`
  even for a fresh room — `getRoom` creates it); vc-rust
  `main.rs:handle_queue` (same 0-based remove; add requires track
  with id+title).
- **Behaviour.** `list`: waiting items in play order with
  re-indexed positions. `add`: appends to the tail; does **not**
  start playback even when idle (contrast `vc.play`, which starts
  when idle) — the core's `/restore` and `/loop` copy-queueing rely
  on this. `remove`: deletes exactly one waiting item by 0-based
  index; removing the current track is impossible through this
  action. `clear`: empties waiting only; current keeps playing.
  Core callers: `/clear` (saves the list first when >1 item, under
  `cleared:<chat>` for `/restore`), `/remove` (single/range/list),
  `/move` (list → remove → re-add), `/loop off` (remove copies),
  `/shuffle` (list → shuffle → remove-all → re-add in new order).
- **New plan.** `room` queue ops (mutex-guarded, §4.2) → persist
  on every mutation (bbolt, §4.6).
- **Edge cases.** add with missing id/title → `bad_request`; add
  with missing source → default `youtube` (vc-go rule); remove
  without position → `bad_request`; remove out of range →
  `bad_request` "position out of range"; unknown action →
  `bad_request`; list on unknown chat → `[]` (200, not 404).
  **Known old-core divergence (do NOT port):** the core's
  onRemove/onMove/onLoop-off treat the waiting list as if index 0
  were the current track and pass 1-based display numbers straight
  through as `position` (dispatcher.cpp onMove comment: "index 0 is
  now-playing" — false for all three engines). Under the locked
  0-based waiting semantics, the old core's `/remove 1` removes
  waiting[1] (the *second* queued track) while reporting its title.
  Wave 2 core must send display N as backend position N−1 and must
  not skip items[0] in `/restore`/`/move`/`/shuffle` (see §9-D4).

### 1.8 `vc.loop` — POST, 30 s

- **Contract.** Req `{chat_id, count: integer 0..=10}`. Resp
  `{now_playing}`. `loop`/`loop_count` = remaining replays of the
  current track, 0 = off (wire-format range, FINAL §6-R6).
- **Old impl.** vc-go `handleLoop` → `room.SetLoop` (clamp
  [0, MaxLoop=10], 404 when idle); vc-ts `/v1/loop` (same range
  check, 400 outside); vc-rust `handle_loop` (same). Ticker/
  advance: while `loop > 0`, replay the current track and
  decrement; `/skip` resets it to 0. vc-ts panel loop is a boolean
  toggle that sets 10 (remix "infinite" loop approximated by the
  max count).
- **Behaviour.** Setting the count does not restart the track; the
  count is consumed one per replay at the natural track end.
  **Divergence (do NOT port):** the core's `/loop` handler does
  NOT call `vc.loop` — it queues N copies of the current track
  (dispatcher.cpp `onLoop`, comment: "The vc services have no
  native loop action") and stores the count in the core chat field
  `loop`. That comment is stale: all three engines implement
  `vc.loop` natively. Wave 2 core must switch `/loop` to `vc.loop`;
  if both mechanisms run, a track loops 2N times (§9-D3).
- **New plan.** `room.SetLoop` (clamp 0–10) → snapshot. No voice
  call (the loop is consumed at advance time, §4.3).
- **Edge cases.** count outside 0–10 → `bad_request`; idle →
  `not_found`; `count` missing/non-integer → `bad_request`.

### 1.9 `vc.shuffle` — POST, 30 s

- **Contract.** Req `{chat_id, enabled: boolean}`. Resp
  `{now_playing}`.
- **Old impl.** vc-go `handleShuffle` → `room.SetShuffle` (flag
  only; the stored queue order is **never** reordered — later pops
  pick a random index); vc-ts `/v1/shuffle` (enabling ALSO shuffles
  the queue in place, Fisher–Yates, and sets the flag; disabling
  keeps the order); vc-rust `handle_shuffle` (enabling shuffles in
  place via `shuffle_queue` AND sets the flag; disabling clears
  the flag only).
- **Behaviour.** Two semantics exist in-tree: flag-only (vc-go) vs
  shuffle-in-place + flag (vc-ts/vc-rust). Locked Phase 3
  semantics: **vc-ts/vc-rust (shuffle-in-place + flag)** — it is
  what the core's `/shuffle` (list → shuffle → remove-all →
  re-add) and the panel shuffle button assume, and it is what the
  user sees. Enabling with <2 waiting tracks: the core answers
  "Need at least two waiting tracks to shuffle" before calling;
  the backend itself shuffles whatever is there (0–1 items is a
  no-op, still 200). Disabling never reorders.
- **New plan.** `room.SetShuffle`: enabled → Fisher–Yates over
  waiting + `shuffle=true`, re-index positions; disabled →
  `shuffle=false`. Persist.
- **Edge cases.** `enabled` not a boolean → `bad_request`; idle →
  `not_found` (vc-go/vc-ts rule; vc-rust creates the room — do not
  port that: `vc.loop`/`shuffle`/`autoplay` must not create rooms).

### 1.10 `vc.autoplay` — POST, 30 s

- **Contract.** Req `{chat_id, enabled: boolean}`. Resp
  `{now_playing}`.
- **Old impl.** vc-go `handleAutoplay` → `room.SetAutoplay`
  (disabling clears history); vc-ts `/v1/autoplay` (same);
  vc-rust `handle_autoplay` (sets the flag; does NOT clear
  history on disable — minor divergence, unify on vc-go/vc-ts).
- **Behaviour.** When on, a drained queue triggers a related-track
  pick from the last track's YouTube mix (§4.3). Only YouTube
  tracks autoplay (all three pickers return nil for non-YouTube).
  History (played ids) excludes repeats; cap 50 (vc-go/vc-ts;
  vc-rust uses 200 with shift — unify on 50, §4.3). Disabling
  clears history. The `chat.settings` `autoplay` key and the
  `autoplay:<chat>` store flag are the same setting viewed from
  the core; the backend is authoritative in Phase 3 (§5).
- **New plan.** `room.SetAutoplay` → persist.
- **Edge cases.** `enabled` not boolean → `bad_request`; idle →
  `not_found`.

### 1.11 `vc.autoplay_skip` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{now_playing}`.
- **Old impl.** vc-go `handleAutoplaySkip` → `room.AutoplaySkip`
  (= `Skip`); vc-ts `/v1/autoplay-skip` (**only when autoplay is
  on**: 400 "autoplay is off in this chat" otherwise; picks a mix
  track from the current track now, loop reset, force-play; 502
  when no pick); vc-rust `handle_autoplay_skip` (same gate: 400
  autoplay-off, 400 nothing-playing, 502 `download_failed` "no
  autoplay recommendation").
- **Behaviour.** Locked semantics: **vc-ts/vc-rust** (gated pick),
  not vc-go's plain skip. The pick excludes last + queue +
  history; the picked track starts immediately (current is
  replaced, not queued); loop resets to 0. Fires `TrackStarted`
  for the pick.
- **New plan.** `room` gate → `resolver.Related` (mix fetch,
  §3.2) → `room.StartTrack` → `voice` rebind → events.
- **Edge cases.** Autoplay off → `bad_request` 400; nothing
  playing → `not_found`; mix fetch fails → 502
  `engine_unavailable` (vc-ts) — unify on `engine_unavailable`,
  not vc-rust's `download_failed`; mix empty (every candidate
  excluded) → 502 `engine_unavailable` "no autoplay recommendation".

### 1.12 `vc.volume` — POST, 30 s

- **Contract.** Req `{chat_id, volume: integer 0..200}`. Resp
  `{now_playing}`. Remix scale: 100 = 100%.
- **Old impl.** vc-ts `main.js:/v1/volume` ONLY (clamp-check
  0–200, 400 outside, 404 idle; sets `volume`, clears
  `muted_volume`). vc-go has no `/v1/volume` endpoint (accepts an
  optional `volume` in the play body; `SetVolume` clamps 0–200).
  vc-rust has no volume endpoint (volume field exists, never
  changed over IPC). Panel volinc/voldec are ±10 steps on the same
  0–200 scale (vc-ts `panelAction`).
- **Behaviour.** Volume is a room property, not a voice-call
  property in the old code (the core muted by setting volume 0).
  Phase 3 keeps volume 0–200 on the room and maps it to DSP gain
  in Phase 5 (volume 0–200 → linear/log gain; the DSP owns the
  mapping). **Mute is separate** (§1.18): `is_muted` is its own
  flag; setting volume 0 does NOT set `is_muted`, and unmuting
  restores the stored pre-mute volume.
- **New plan.** `room.SetVolume` (clamp, do not 400 — vc-go
  clamp rule wins over vc-ts 400 rule for out-of-range values;
  non-numeric → `bad_request`) → `voice.SetVolume` (Phase 5 gain;
  Phase 3: stored only) → snapshot.
- **Edge cases.** Idle → `not_found`; volume is per-chat, not
  per-user; the core's Vol−/Vol+ buttons send current ±10.

### 1.13 `vc.now_playing` — GET, 10 s

- **Contract.** Req `{chat_id}` (query). Resp
  `{now_playing: NowPlaying}`.
- **Old impl.** vc-go `handleNowPlaying` (GET, 404 when no room or
  room with no track and empty queue); vc-ts `GET /v1/now-playing`
  (404 when no room or no current — stricter: an idle room with a
  queued list still 404s); vc-rust `GET /v1/now-playing` (200 with
  an **empty room** for an unknown chat — divergence, §9-D5).
- **Behaviour.** Returns the full `NowPlaying` snapshot: track,
  queue (waiting, 0-based positions), paused/is_paused, volume,
  is_muted, loop/loop_count, shuffle, autoplay/autoplay_enabled,
  position_sec (wall-clock), speed (always 1.0 in the old engines),
  stream_url (mirrors current track's stream_url when known),
  engine tag (`"go"`/`"ts"`/`"rust"` → `"backend-go"` in Phase 3).
  `NowPlaying` legacy vs IPC-1 mirror fields: `paused` =
  `is_paused`, `loop` = `loop_count`, `autoplay` =
  `autoplay_enabled`; `is_muted` was volume==0 in vc-go/vc-rust
  and `muted_volume != null` in vc-ts — Phase 3 uses the real
  mute flag (§1.18). `speed` is 1.0 until the Phase 5 DSP applies
  the room's stored speed.
- **New plan.** `room.Snapshot` (read-only, no room creation).
- **Edge cases.** Missing/non-integer `chat_id` → `bad_request`;
  no room, or room with no track and empty queue → `not_found`
  (vc-go rule — the strictest consistent rule; vc-rust's 200-empty
  is not ported). A room with no current but a non-empty queue
  returns 200 with `track: null` (vc-go rule).

### 1.14 `vc.progress` — GET, 10 s (IPC-2)

- **Contract.** Req `{chat_id}`. Resp `{position_sec,
  duration_sec, is_paused, is_muted, loop_enabled, now: Track|null,
  next: Track|null}`.
- **Old impl.** vc-go `handleProgress`; vc-ts `GET /v1/progress`;
  vc-rust `GET /v1/progress` (`progress_json`). All three agree:
  same 404 rule as `vc.now_playing` (no room, or nothing playing,
  is 404); `next` is the first waiting track or null;
  `loop_enabled` is `loop > 0`; `duration_sec` is the current
  track's duration (0 when unknown/live).
- **Behaviour.** Read-only view over the room snapshot; drives the
  now-playing card's progress-bar button refresh and the `/menu`
  panel. `position_sec` is wall-clock derived (§4.4).
- **New plan.** `room.Snapshot` → project to IPC-2 shape.
- **Edge cases.** Same 400/404 rules as `vc.now_playing`; a room
  with a queue but no current track 404s (all three engines agree
  here — vc-ts requires `room.current`; vc-go requires track or
  non-empty queue for now-playing but progress in vc-go uses the
  same check as now-playing… note: vc-go `handleProgress` 404s
  when `Track == nil && len(Queue) == 0`, so a queued-only room
  returns 200 with `now: null`. vc-ts/vc-rust 404 on queued-only.
  Unify on vc-go (200 with `now: null`, `next` = first waiting) —
  it is the only rule under which a queued-only room is observable.

### 1.15 `vc.panel` — GET/POST, 10 s

- **Contract.** Req `{chat_id}` (+ POST `action`). Resp
  `{now_playing: NowPlaying}` per `ipc.json`.
- **Old impl.** vc-ts `main.js` ONLY: `GET /v1/panel` returns
  **`{panel: panelState}`** (not `{now_playing}` — contract
  divergence, §9-D2); `POST /v1/panel` with `action` returns
  `{message, panel, now_playing}`. `panelState(room)`:
  `{chat_id, text, now {title,url,artist}|null, next {...}|null,
  paused, is_muted, position_sec, duration_sec, volume, loop
  (boolean), queue_count, buttons: [PANEL_BUTTONS]}` with the 10
  remix button ids (`panel_shuffle/skip/pause/resume/mute/unmute/
  voldec/volinc/loop/update`). `panelAction(room, action)`:
  update (no-op refresh), shuffle (requires ≥1 queued, Fisher–
  Yates + sets shuffle mode), skip (loop reset, pop next),
  pause/resume, mute (stores `muted_volume`, sets volume 0),
  unmute (restores `muted_volume ?? 100`), volinc/voldec (±10,
  0–200), loop (toggle → 10 or 0). Failures: "Not streaming.",
  "Not enough items to shuffle.", unknown action → 400 with the
  action list.
- **Behaviour.** The panel is the remix control-panel surface the
  core's `/menu` renders. The core calls GET to render and POST to
  act; the GET response shape the core actually reads is `panel`,
  not `now_playing`.
- **New plan.** `room` + `voice`: implement the contract shape
  (`{now_playing}`) AND the `panel` object the core reads (return
  both keys; §9-D2). Panel actions map 1:1 onto the room ops
  (§1.2/1.4/1.9/1.12/1.18 + §1.8 with the toggle → 10/0 rule).
- **Edge cases.** Unknown chat → `not_found`; unknown action →
  `bad_request` naming the valid actions; skip/pause/resume with
  nothing streaming → `bad_request` "Not streaming." (vc-ts 400,
  not 404 — keep it: the panel is a control surface, its errors
  are action errors); shuffle with an empty queue → `bad_request`
  "Not enough items to shuffle."

### 1.16 `vc.lyrics` — POST in the new contract, 10 s

- **Contract.** `ipc.json` says `GET /v1/lyrics`, req `{chat_id}`,
  resp `{lyrics, source: lrclib|netease|file-tag, track_id, title,
  artist, truncated}`. **The GET form is dead**: vc-ts is
  POST-only and the core calls POST with `{chat_id}` (and
  optionally `{title, artist}`). FINAL §6-R5 resolves this: the
  new contract says **POST**; the old GET is corrected, not
  preserved (uds-protocol §4 lists it as a POST-style action with a
  10 s deadline).
- **Old impl.** vc-ts `main.js:/v1/lyrics` → `resolvers.fetchLyrics`
  (LRCLib `GET https://lrclib.net/api/search?q=<title artist>`,
  first-artist-only for comma-joined JioSaavn artists, exact-title
  match preferred else first hit, synced lyrics preferred over
  plain, one retry on transient failure; 404 "no lyrics found"
  when the title is missing or LRCLib has nothing; `truncated`
  always false — LRCLib lyrics are returned whole, the core chunks
  at 4096 chars for display and 4000 for inline `l`). `source` is
  always `lrclib` from this endpoint (NetEase synced lyrics come
  from `netease.lyrics`, §5.3; `file-tag` is a source label the
  old code never produced — keep it as a valid enum value for
  Phase 4+ cache reads).
- **Behaviour.** Title/artist come from the body when given, else
  from the room's current track (track_id is then the current
  track's id, else null). JioSaavn cannot supply lyric text (its
  `/song` payload has `has_lyrics: true` but `lyrics: null`).
- **New plan.** `lyrics.FetchLyrics(title, artist)` (LRCLib
  primary; NetEase synced as a second source when the track is a
  NetEase track — see `netease.lyrics`) → response.
- **Edge cases.** No title and no current track → `bad_request`;
  no lyrics → `not_found` (not an empty string); LRCLib 503 →
  one retry, then `engine_unavailable`; lyrics are never invented.

### 1.17 `vc.join` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{joined: boolean,
  now_playing: NowPlaying}`.
- **Old impl.** **No engine implements it.** `ipc.json` attributes
  it to "core-cpp TdLibVoiceCall::join (TDLib + dlopen'd ntgcalls,
  in-process)" — that file (`voice_call.cpp`) was deleted in
  Phase 2, so the old behaviour is reconstructed from the contract
  + callers, not from surviving code (§9-U1). vc-ts `voicecall.js:
  join()` and vc-rust `voice.rs:join()` both DELEGATED to the core
  (`POST {CORE_URL}/v1/join`, reporting the core's answer verbatim:
  `joined:true` only when the core's TDLib backend really joined).
  Core side today: `Dispatcher::joinVoice` (dispatcher.cpp:1668)
  calls `vcCall "join"` and sets `callActive` on success; it
  short-circuits when already in a call.
- **Behaviour (new).** `vc.join` is a backend action that OWNS the
  call: the backend creates the ntgcalls call (local SDP offer),
  sends a `JoinRequest` frame over UDS (§3.4), the core runs
  `joinVideoChat(local_sdp)` on the assistant session and answers
  `{remote_sdp, group_call_id}`, the backend completes the
  handshake (`ntg_connect`) and owns the call from then on.
  `vc.join`'s own response (`{joined, now_playing}`) is a separate
  request/response pair and may precede or follow the handshake
  (uds-protocol §6). A failed handshake → `joined: false` with the
  core's exact reason, never a faked join; the backend must treat
  a missing/throwing JoinRequest handler as failed, never as
  silently joined.
- **New plan.** `voice.Join(chat_id)` → `uds.JoinRequest` →
  `ntg_connect(remote_sdp)` → `room` snapshot. Idempotent:
  already-joined → 200 `{joined: true}` without a second handshake.
- **Edge cases.** Core unreachable / no handler → `ok:false,
  error:{code:"internal"}`; join failure → `engine_unavailable`
  with the exact reason; `chat_id` 0 → `bad_request`. Live join
  is still blocked in the sandbox (no MTProto session, UDP EPERM)
  — the backend must surface the exact reason per chat in
  `voiceState`, never claim joined without the core's answer.

### 1.18 `vc.mute` — POST, 30 s

- **Contract.** Req `{chat_id, muted: boolean}`. Resp `{muted:
  boolean, now_playing: NowPlaying}`.
- **Old impl.** **No engine implements `/v1/mute`.** The old
  behaviour is reconstructed (§9-U2): all three engines model
  mute as volume 0 (vc-go `IsMuted = volume == 0`; vc-ts
  `muted_volume` preserved across mute/unmute; vc-rust `IsMuted =
  volume == 0`), and the engine comments say "the core mutes via
  /v1/volume 0". The old HTTP mapping (`engine_router.cpp`) was
  deleted in Phase 2, so the exact old route of `vc.mute` is
  UNDETERMINED. Core side today: `Dispatcher::muteVoice`
  (dispatcher.cpp:1688) calls `vcCall "mute"` and stores the
  `muted` chat field; unlike `/mute` it has no already-muted
  short-circuit — a repeated mute is a success.
- **Behaviour (new).** Real mute flag on the room, orthogonal to
  volume: mute stores the current volume, sets the ntgcalls/DSP
  output to silent, keeps `volume` unchanged; unmute restores.
  `is_muted` in `NowPlaying`/`vc.progress` reads this flag (not
  volume==0).
- **New plan.** `room.SetMuted` → `voice.Mute/Unmute` → snapshot.
- **Edge cases.** Idle → `not_found` (muteVoice's own gate:
  "Nothing is playing." when neither now-playing nor callActive);
  repeated mute/unmute → 200 success with the requested state;
  `muted` missing/non-boolean → `bad_request`.

### 1.19 `vc.radio` — POST, 30 s

- **Contract.** Req `{action: start|stop, chat_id, station:
  string}`. Resp `{now_playing: NowPlaying}`.
- **Old impl.** radio-js `main.js:/v1/radio` (start → `startRadio`,
  stop → `stopRadio`; thin wrapper over `radio.start`/`radio.stop`,
  §7.2/§7.3) — **but it does not return `{now_playing}`**: it
  returns the `startRadio`/`stopRadio` objects (§7.2). The
  `vc.radio` → `NowPlaying` shape in `ipc.json` was never what
  radio-js returned. The core's `/radio` handler calls
  `radio.start` + `vc.play` (VC handoff), not `vc.radio`
  (routing-table C26).
- **Behaviour.** Start: resolve the station, probe, hand the live
  stream to the room as a radio track (duration 0, `is_live`,
  `source: "radio"`), fire `TrackStarted`. Stop: clear the radio
  state and stop the room's radio track (only if the room is
  playing that radio track — stopping must not kill a normal music
  track that replaced it; radio-js `clearVcIfRadio` checks the
  current track before stopping).
- **New plan.** `radio.Start/Stop` → `room` (radio track as
  current) → `voice` rebind → snapshot. Return `{now_playing}`
  per the contract (and keep the `vc_handoff` object in the
  response as a superset field for the core's diagnostics — the
  old radio-js shape is preserved under `radio.start`, §7.2).
- **Edge cases.** Unknown station → `bad_request` naming
  `radio.list`; stop with no radio → `not_found`; action other
  than start|stop → `bad_request`.

---

## §2. Downloads — `download.*` (8 actions)

Shared: every `download.*` POST opens a progress run **before** the
provider runs and finishes it after (`downloaders/src/server.py:
do_POST`); the run_id rides back in the response (`data.run_id`).
The core passes an explicit **120 s** timeout for these calls. Files
over the 50 MB Bot API cap (`MAX_BOT_API_BYTES = 50*1024*1024`,
`downloaders/src/util.py`) are reported with `oversize: true` and
**not** failed — the core queues them for a later MTProto pass
("That file is over the 50 MB send cap — queued for a later send
instead of failing"). Partial files are deleted on failure. All
download file results are `{file_path, track, size_bytes, oversize}`
via `providers._file_result` unless noted.

### 2.1 `download.youtube` — POST, 120 s

- **Contract.** Req `{query, requested_by}`. Resp `{file_path,
  oversize, size_bytes, track, run_id}`.
- **Old impl.** `downloaders/src/providers.py:youtube_download` →
  `ytdlp.download_youtube_audio` (`ytdlp.py:220`). Flow: bare
  query → `search_youtube(limit=1)` first (flat `ytsearch1:`);
  URL → direct. yt-dlp args: `--format bestaudio/best --output
  <job-dir>/%(title).150B [%(id)s].%(ext)s --no-playlist
  --restrict-filenames --no-embed-metadata --extract-audio
  --audio-format mp3 --audio-quality 0` (want_mp3 default true).
  Progress via `--progress --newline` lines parsed by
  `util.parse_ytdlp_progress_line` into the run
  (percentage/done/total/speed/eta, phase `downloading`).
  Bot-check ("not a bot" / "Sign in to confirm") → one retry with
  `--extractor-args youtube:player_client=android`. Track metadata
  from a cheap flat extract afterwards; falls back to a sanitised
  filename track when that fails.
- **Behaviour.** Returns an **MP3 file** (not the stream URL).
  `track.source` is `youtube`; id is the yt-dlp id (no `yt:`
  prefix in the downloaders track shape — contrast the vc
  resolvers' `yt:<id>`; Wave 2 unifies on `yt:<id>` for tracks
  that enter a room, and keeps the downloader id as-is for files).
- **New plan.** `download.YouTube` (yt-dlp daemon client — the
  warm `YoutubeDL` in RAM, not a per-call fork; §1.2 prohibition
  3) → `store` run record → response.
- **Edge cases.** Empty query → `bad_request`; no match →
  `download_failed` "no YouTube match"; yt-dlp failure →
  `download_failed` with stderr (trimmed); oversize → `oversize:
  true` + file path still returned (the file IS written — the
  oversize rule is about *sending*, not downloading; contrast
  NetEase §2.31, which does not write oversize files).

### 2.2 `download.deezer` — POST, 120 s

- **Contract.** Req `{query, requested_by}`. Resp `{file_path,
  track, run_id}` (plus `size_bytes`/`oversize` in practice).
- **Old impl.** `providers.py:deezer_download` →
  `deezer.download_deezer` (`deezer.py:504`). Input: Deezer URL
  (track/album, `parse_deezer_url`) or free-text query (searched
  first via the public API `https://api.deezer.com`). Without
  `DEEZER_ARL`: 30 s **preview** downloads only (public API
  `preview` URL; track carries the preview note "30-second preview
  … the full track needs a Deezer account"). With `DEEZER_ARL`:
  gateway `api_token` fetch → `track_tokens` → CDN URL → chunk
  **Blowfish-CBC decryption** (deemix key derivation; AES-ECB
  legacy stream-URL step) → format by `DEEZER_FORMAT`
  (MP3_128=1, MP3_320=3, FLAC=9; default MP3_320; FLAC falls back
  MP3_320 → MP3_128 by FILESIZE_* availability). Album input →
  ZIP of previews (or full tracks with ARL); response gains
  `count`. Requires pycryptodomex for the ARL path; without it
  the ARL attempt fails honestly and previews are used instead.
  No ARL value is ever logged or stored.
- **Behaviour.** `track.source` is `deezer`. The run reports phase
  transitions only (no streaming progress — single-shot API
  calls); percentage stays 0 until the file lands.
- **New plan.** `download.Deezer` (native Go ARL client, in-process
  Blowfish-CBC, Tier 1 ladder) → run → response.
- **Edge cases.** No ARL + album → ZIP of previews (honest, not
  an error); ARL path failure → preview fallback, never a fake
  full track; no previews downloadable → `download_failed`;
  unknown query → `download_failed`.

### 2.3 `download.tiktok` — POST, 120 s

- **Contract.** Req `{requested_by, url}`. Resp `{file_path,
  track, run_id}` (plus `size_bytes`/`oversize`).
- **Old impl.** `providers.py:tiktok_download` →
  `tiktok.download_tiktok` (`tiktok.py:142`). Flow: validate the
  URL (`parse_tiktok_url`, short links resolved); `extract_info`
  via yt-dlp (metadata only); `_best_audio_url` picks the audio
  stream; **direct CDN download** first (yt-dlp's own downloader
  stalls on reads from this egress; the same signed URL downloads
  the whole file in ~2 s); on direct failure, fallback to a native
  yt-dlp download (`_download_via_ytdlp`).
- **Behaviour.** Audio-only extraction from a TikTok video; the
  track note records the direct-CDN path.
- **New plan.** `download.TikTok` (native Go extractor, Tier 3) →
  run → response.
- **Edge cases.** Not a TikTok URL → `download_failed`; metadata
  unparsable / yt-dlp timeout → `download_failed`; empty CDN
  download → `download_failed`; direct-download HTTP error →
  `download_failed` with the status.

### 2.4 `download.navidrome` — POST, 120 s

- **Contract.** Req `{requested_by, url}`. Resp `{imported,
  track, run_id}` (plus `file_path`, `size_bytes`, `oversize`).
- **Old impl.** `providers.py:navidrome_download` →
  `navidrome.download_navidrome` (`navidrome.py:157`). Input: a
  Navidrome URL/query — `?id=` is a direct download by id, else
  `search_songs(query, limit=1)` then `download_song`. Subsonic
  API: search → album → song → `rest/download` returns the
  original file (no transcode); `imported` is always `True` in
  practice (the file is imported into the download dir). Auth via
  `NAVIDROME_URL`/`NAVIDROME_USER`/`NAVIDROME_PASS` (+ token/salt
  auth params); transport errors → `download_failed`; HTTP errors
  name the URL.
- **Behaviour.** `track.source` is the Navidrome source; the file
  keeps its original container/quality (lossless when the library
  holds FLAC — Tier 1).
- **New plan.** `download.Navidrome` (native Go Subsonic client,
  Tier 1) → run → response. The same client backs `/scan` and
  `library.*` (§8).
- **Edge cases.** No match → `download_failed` "no Navidrome
  match"; server unreachable → `download_failed` (not
  `engine_unavailable` — the old code raises `download_failed`
  on transport errors; keep it); missing env → `download_failed`
  naming the missing variable.

### 2.5 `download.spotify` — POST, 120 s

- **Contract.** Req `{requested_by, url}`. Resp `{file_path,
  track, run_id}` (plus `size_bytes`/`oversize`).
- **Old impl.** `providers.py:spotify_download` →
  `spotify.resolve_track` + `spotify.download_spotify_track`
  (`spotify.py:305`). Non-track URLs are refused (`bad_request`,
  "use /v1/download/spotify-playlist for … URLs"). Resolution:
  Spotify Web API when credentials exist, else yt-dlp/oEmbed
  fallback. Download: if `spotdl` is installed, `_download_via_spotdl`
  (`spotdl "<artist> <title>" --output-format mp3 --use-youtube
  --path-template <job-dir>/{artist} - {title}.{ext}`, 600 s cap);
  else `_pick_match` (YouTube search for "artist title") →
  `download_youtube_audio` (MP3) and the Spotify metadata is
  merged over the YouTube track (duration/thumbnail/url kept from
  YouTube when present).
- **Behaviour.** The file is a YouTube-sourced MP3 (or a spotdl
  MP3) carrying Spotify metadata; the track is honest about that
  (no claim the bytes came from Spotify).
- **New plan.** `download.SpotifyTrack`: resolve via `spotify`
  (Web API / fallback) → audio through the Tier 1→2→3 ladder
  (never a second download loop: the ladder resolves audio, the
  download writes the file once).
- **Edge cases.** Non-track URL → `bad_request`; empty title →
  `download_failed`; spotdl timeout (600 s) → `download_failed`;
  no YouTube match → `download_failed`.

### 2.6 `download.spotify_playlist` — POST, 120 s

- **Contract.** Req `{mode: zip|files, requested_by, url}`. Resp
  `{count, file_path, file_paths, run_id}`.
- **Old impl.** `providers.py:spotify_playlist_download` →
  `spotify.download_spotify_collection` (`spotify.py:352`).
  Track URLs are refused (`bad_request`, "use /v1/download/
  spotify for single track URLs"). Collections resolve via
  `resolve_collection`; lists are capped at `MAX_PLAYLIST_TRACKS
  = 50` (`truncated: true` beyond). Each track downloads via
  §2.5 with per-track progress callbacks into the run's checklist.
  `mode=zip`: one ZIP (`<artist>-playlist-<8hex>.zip`),
  `file_path` set, `file_paths` absent. `mode=files`: per-file
  list, `file_path` absent (null in the old response — the
  contract lists both keys; the backend returns the applicable
  one and `null` for the other). `count` = number of files
  actually downloaded (not the playlist length). The core's
  Spotify option picker (`pl:dl:zip` / `pl:dl:1by1`, routing
  A69–A70) is the only caller that chooses the mode.
- **New plan.** `download.SpotifyCollection` (resolve → per-track
  ladder downloads → zip-or-files assembly) → run → response.
- **Edge cases.** `mode` other than zip|files → `bad_request`
  (default zip when absent); empty collection / nothing
  downloadable → `download_failed`; `count` may be < playlist
  length — the response also carries `tracks` and `truncated`
  as superset fields (§9-D1).

### 2.7 `download.soulseek` — POST, 120 s (call-site; transfers run longer)

- **Contract.** Req `{query, artist, title, duration_sec,
  accept_lossy, album: {artist, title, tracks: [{title, artist,
  duration_sec}]}, requested_by}`. Resp `{file_path, track,
  oversize, size_bytes, peer, format, count, total, failed:
  [[title, reason]], album_dir, source: "soulseek", run_id}`.
  Errors: `engine_unavailable` = slskd unreachable or search
  could not run (e.g. not logged in); `download_failed` = search
  ran but found nothing usable (honest reason in the message).
- **Old impl.** `providers.py:soulseek_download` →
  `soulseek/client.py` (slskd REST: `_start_search`, 20 s search
  timeout, 200-response limit) + `soulseek/downloader.py` +
  `soulseek/{matcher,scorer,selection}.py`. Single: `query` split
  on `" - "` into artist/title, or artist+title fields; search →
  flatten → score (peers ranked by free upload slots, speed,
  `.flac` format; `is_lossless()` on extension/bit-depth) →
  enqueue → `wait_for_files` (600 s cap, `progress_cb`/`state_cb`
  into the run) → move from the slskd download dir. Album: per-
  track checklist in the run (`set_titles` first), each track
  downloaded independently; `failed` lists `[title, reason]` per
  miss. `accept_lossy: true` allows mp3 ≥256 kbps / m4a when no
  FLAC exists; otherwise lossless-only. Lossy-only result sets
  `run.lossy_available = true` + `lossy_summary` ("only lossy
  copies found (mp3 320 kbps)" etc.) and the core shows the
  lossy prompt (`pl:lossy:accept/skip`, routing A83–A84).
  Failure strings are humanised (`progress.humanize_failure` —
  "peer rejected the transfer", "transfer timed out", …).
- **Behaviour.** `format` is the file format (`flac`/`mp3`/…);
  `peer` is the Soulseek username; `source` is always
  `"soulseek"`. Single-track responses carry `file_path`/`track`/
  `peer`/`format`; album responses carry `count`/`total`/
  `failed`/`album_dir`/`format`/`source_counts` and NO single
  `file_path`/`track` (the contract's single shape is the union
  of both modes — Wave 2 returns the applicable subset, §9-D1).
- **New plan.** `download.Soulseek` (native Go slskd client;
  slskd itself stays an optional sidecar process — FINAL §6-R3:
  "2 core binaries + 1 persistent yt-dlp daemon + optional
  slskd") → run → response.
- **Edge cases.** slskd down / not logged in →
  `engine_unavailable` naming `SLSKD_URL`; search ran, nothing
  usable → `download_failed` (distinguish the two — the core
  renders them differently); `accept_lossy` false + only lossy
  copies → `download_failed` with the lossy reason AND
  `lossy_available: true` on the run (so the core can offer the
  lossy prompt); transfer timeout/cancel/reject → humanised
  `download_failed`; album with zero downloads →
  `download_failed` naming the first failure.

### 2.8 `download.progress` — GET, 10 s (IPC-4)

- **Contract.** Req `{run_id}`. Resp `{run_id, percentage,
  done_bytes, total_bytes|null, speed_bytes_per_sec, eta_sec|null,
  elapsed_sec, phase: searching|downloading|uploading|done|failed,
  per_track: [{title, state, pct, peer|null, reason|null}],
  lossy_available, lossy_summary|null}`.
- **Old impl.** `downloaders/src/progress.py` (`ProgressRegistry`,
  `DownloadRun`) + `server.py:do_GET /v1/download/progress`.
  Run ids are 12-hex (`uuid4().hex[:12]`, Navidrome style). One
  run per download POST (§2 intro). Numbers come from the real
  downloader: yt-dlp `--progress --newline` lines (percentage /
  bytes / speed / ETA straight off yt-dlp); Soulseek slskd
  `progress_cb`/`state_cb`; other providers report phase
  transitions only (percentage stays 0 until the file lands —
  honest about granularity). Per-track checklist states:
  pending | searching | queued | downloading | done | failed;
  the checklist collapses above 28 tracks in the core, the
  engine still records all. Finished runs are kept for late
  polls; the registry caps at 200 finished runs, oldest first;
  live runs are never evicted. `finish("done")` forces 100%;
  `finish` with an error forces `failed` (humanised).
- **Behaviour.** A snapshot is a point-in-time read; polling is
  cheap and side-effect-free. The core polls it only AFTER the
  synchronous POST returns (live polling during the transfer is
  impossible in the old design — the run_id is only known from
  the POST response; §9-E1 `download.cancel` + an async mode
  change this, see §8.3).
- **New plan.** `download.Registry` (in-memory + bbolt-backed run
  records so a restart does not lose finished runs; live runs
  resume as `failed` with reason "backend restarted" on boot —
  never stuck at `searching`) → snapshot.
- **Edge cases.** Unknown `run_id` → `not_found` "no download
  run"; `run_id` missing → `bad_request`; `total_bytes` null
  until the provider knows it; `eta_sec` null until a speed is
  measurable; `phase: uploading` is the Telegram-upload phase —
  the old code never set it (no upload step in the service); the
  backend sets it only when it actually uploads (OD-3 async
  delivery, §9-E1).
- **Run lifecycle (§8.3, full design).** `new_run(kind, title)` at
  POST entry → `searching` → provider sets `downloading` on
  first bytes → per-track `set_titles`/`set_track` for albums →
  `finish(done|failed)` exactly once → retained (cap 200
  finished). Cancel (§9-E1) transitions a live run to `failed`
  with reason "cancelled by user" and deletes the partial file.
  A run's `error` is humanised and stored but NOT in the snapshot
  shape (the core reads the failure from the POST error, not the
  snapshot — keep that).

---

## §3. `vc.play` chain — deep design (the action everything hangs on)

### 3.1 Query routing (source prefixes, in order)

| # | Input shape | Route | Resolver (Phase 3) | Old source |
|---|---|---|---|---|
| 1 | `radio:<url>` | Radio track (duration 0, `is_live`) | `resolver.RadioURL` | vc-ts `resolvePlayQueryUntimed` (`radio:` branch) |
| 2 | raw `http(s)` URL, non-YouTube | Radio/raw-stream track (duration 0) | `resolver.RadioURL` | vc-ts (same); `radioTrackFromUrl` (`radio:<sha1[:12]>` id) |
| 3 | `jiosaavn:<query>` | JioSaavn search, first result | `resolver.JioSaavnSearch` | vc-ts `searchJioSaavn(term, 1)` |
| 4 | `jsvn:<id>` | JioSaavn single-song lookup | `resolver.JioSaavnSong` | vc-ts `resolveJioSaavnSong` |
| 5 | YouTube URL / 11-char video id | YouTube resolve | `resolver.YouTube` (InnerTube, §3.2) | vc-go `IsYouTubeURL`, vc-ts `extractYouTubeId`, vc-rust `yt_id_from_url` |
| 6 | Spotify URL / `spotify:<kind>:<id>` | Spotify metadata → audio via ladder | `resolver.Spotify` (§3.2) | downloaders `spotify.resolve_track` (download path); vc resolvers never handled Spotify |
| 7 | Deezer / NetEase / TikTok / Navidrome URL | Source metadata → audio via ladder | `resolver.{Deezer,NetEase,TikTok,Navidrome}` | downloaders providers (download path only) |
| 8 | Anything else (free text) | YouTube search, first result | `resolver.YouTubeSearch` | all three (`ytsearch1:`) |

Routing is by link type only (MEMORY: t.me links invoke the src
lane — not applicable here; this is the music-play lane). A query
that matches no route is a YouTube search (row 8), never an error.
The `video`/`force` flags in the core's play body are core-side
only (force = stop-then-play; video = prefer a video stream —
no backend effect in Phase 3; the backend plays audio).

### 3.2 Per-source resolution (Phase 1 winners, locked)

- **YouTube — native InnerTube** (winner: 0.61–0.81 s player /
  1.56 s search, BENCHMARKS.md §2). `POST
  https://m.youtube.com/youtubei/v1/<search|player|browse|next>
  ?key=<key>`, ANDROID_VR (or ANDROID) `player` + WEB `search`;
  stream URL = adaptiveFormats itag **251** (Opus) / **140**
  (AAC) from the ANDROID/ANDROID_VR `player` response (WEB is
  UNPLAYABLE — metadata only). Fallback: warm yt-dlp daemon
  (one persistent process, stdin/stdout JSON-RPC, `YoutubeDL`
  warm in RAM — started once at Go boot, §1.2 prohibition 3).
  Bot-check retry: android player client once. Track id
  `yt:<video-id>`; IPC-3 fields (duration_str, views, uploader,
  upload_time, is_live) from the player/search response.
- **JioSaavn — jsvn-tgvc proxy, one `/song` call** (winner:
  0.32 s single lookup; search + parallel `/song` enrichment
  0.51 s). `GET https://jsvn-tgvc.vercel.app/search?query=` /
  `/song?id=` (fallback `https://saavn.dev/api`); stream =
  `media_urls['320_KBPS']` (**not** 96 — the old vc-ts code took
  96_KBPS; Phase 1 verdict: select 320_KBPS), else `media_url`.
  **No crypto**: DES-ECB does not exist in these sources
  (FINAL §5). Duration is `"m:ss"` — parse it (the old
  `Number("4:28")` bug zeroed every duration). URLs live ~2 min:
  use promptly or re-resolve. Track id `jsvn:<id>`.
- **Lemonic** — `GET ${LEMONIC_API}/api/search?query=&limit=`
  (unauthenticated, or Bearer when `LEMONIC_API_KEY` is set;
  default base `https://api.lemonic.xyz`); fallback: Spotify
  client-credentials search (ids come back as `lemonic:<spotify-
  id>`, resolvable via `/api/raw/track/<id>`); raw track:
  `GET /api/raw/track/<id>` → `stream_url`/`download_url`/
  `url`. `lemonic.download` fetches the stream to
  `data/lemonic/<sanitised-id>.mp3` (60 s fetch timeout).
- **Spotify** — metadata via `spotify.resolve`/`spotify.collection`
  (Web API when configured, yt-dlp/oEmbed fallback otherwise);
  audio through the Tier 1→2→3 ladder (never a Spotify audio
  URL — none exists). Gemini/wordlist skip-check applies to
  autoplay and watch flows, not to an explicit `/play`.
- **Deezer** — Tier 1: ARL client (§2.2); without ARL only the
  30 s preview resolves (and the track says so).
- **NetEase** — Tier 1: eapi levels `standard → higher →
  lossless → hires`, then weapi v1 standard, then linuxapi
  (`SongURLWithLevel`); `music_u` cookie upgrades the level;
  `auth` in the response is `music_u|anonymous`. FLAC when the
  level yields it; mirror fallback serves FLAC, direct endpoints
  MP3.
- **Navidrome** — Tier 1: Subsonic search → `rest/download`
  (original file, no transcode).
- **Soulseek** — Tier 1: slskd search → peer ranking (free
  slots, speed, `.flac`) → transfer. Only for explicit Soulseek
  queries/downloads in Phase 3 (not in the `/play` fast path
  unless the query names it — `/play` does not search Soulseek
  by default; the ladder order is FLAC-first per source, not
  cross-source Soulseek fallback).
- **TikTok** — Tier 3: yt-dlp metadata → best audio URL →
  direct CDN download, yt-dlp fallback.
- **Radio / raw URL** — the URL itself is the track (`radio:
  <sha1[:12]>`); no resolve, duration 0.
- **Autoplay pick** — YouTube mix only: `watch?v=<id>&list=RD<id>`,
  flat playlist, first entry not in exclude (last + queue +
  history); 30 s (vc-go) / 60 s (vc-ts/vc-rust) yt-dlp timeout;
  bot-check retry once. Returns nil (not an error) when the mix
  is empty or every candidate is excluded — the caller decides
  idle (ticker) vs 502 (autoplay-skip).

### 3.3 Replied-media path (the Phase 3 backend action)

**What the core sends today.** `Dispatcher::playRepliedFile`
(dispatcher.cpp:2480, called from `onPlay` when the query is
empty and the message replies to / carries audio, video, voice,
video_note, or document media): the core downloads the file via
the TDLib bot session (`tg_.downloadFile(file_id, <data_dir>/
replied, saved, err)` — `getRemoteFile` + `downloadFile`, with
timeouts) and then answers with an **honest gap reply** — the
file is on disk under `<data_dir>/replied`, the user is told
"playing a replied file is not available yet — local-file
playback lands with the Phase 3 backend action. Nothing is
playing." No IPC is issued on this path (`emitDispatchToIpc(…
ipc=none)`); `playRepliedFile` returns false. The core also
records the title in the `last_track` chat field. Media kinds:
`reply_media_file_id` / `reply_media_title` /
`reply_media_kind` (reply) or `media_file_id` / `media_title` /
`media_kind` (caption media). `/share` on replied media is a
separate path (share send prompt, not playback).

**What the backend action must be (new action `vc.play_file`,
§9-E12 — proposed here, Wave 2 adds it to the UDS dispatch and
the routing table).** The core already has the file on disk; it
must not re-upload bytes over UDS (16 MiB frame cap). The action
carries a **path**, not bytes:

- Request: `{chat_id, file_path: string (absolute path under
  <PARLAY_DATA_DIR>/replied, as saved by the core),
  file_id?: string (Telegram file_id, for the Track id),
  title?: string, kind?: string (audio|video|voice|
  video_note|document), duration_sec?: integer (0 when unknown),
  requested_by: UserId}`.
- Response: `{now_playing: NowPlaying}` (same as `vc.play`).
- Behaviour: validate the path (must be under the data dir;
  reject `..` — same rule as `fx.apply`'s `sanitize_path`;
  missing file → `not_found` "replied file not found on disk").
  Build a Track `{id: "file:<file_id or basename-hash>",
  title, source: "file", duration_sec, url: "", stream_url: ""}`
  (`Track.source` already admits `"file"` in the contract enum).
  Play-now-if-idle else enqueue, exactly like `vc.play` (§1.1)
  — the file is decoded by parlay-dsp from the local path in
  Phase 5; in Phase 3 the room records it as current with
  `stream_url` empty and the voice side reports "local file —
  audio path lands with the DSP" in `voiceState` (never a fake
  join/play claim). Position clock runs from the file's known
  duration when present; duration 0 → never auto-advances (§4.4).
  The backend takes ownership of the file (it may move it from
  `replied/` into its own media dir; it must not delete the
  core's copy until the move succeeds).
- Edge cases: download failed in the core → no action is ever
  sent (the core already replied); file over the old 50 MB cap
  is fine (MTProto, 2 GB limit — FINAL §6-R8); voice/voice_note
  (opus/ogg) and video (audio track) are accepted as audio
  sources; `requested_by` is the replier.
- Routing-table impact: C1 (`/play` replied media) flips from
  "honest gap reply" to `vc.play_file`; the core deletes the
  gap branch in Wave 2.

### 3.4 Voice join handshake (backend owns the call)

1. `vc.play` (or `vc.join`, or a radio handoff) needs a call →
   `voice.CreateCall(chat_id)` → ntgcalls `ntg_create_call` →
   local SDP offer. (One call per chat; a second `create_call`
   for the same chat fails `-202` — stop the stale call and
   retry once, vc-ts `createLocalOffer` rule.)
2. Backend sends `JoinRequest {request_id (1,000,000+ range),
   chat_id, local_sdp}` over UDS.
3. Core runs `joinVideoChat(local_sdp)` on the assistant session
   → answers `{remote_sdp, group_call_id}` (or `ok:false,
   error:{code:"internal"}`).
4. Backend `ntg_connect(remote_sdp)` → owns the call; `voice`
   state becomes live; `TrackStarted` fires for the current track.
5. Stream binding: `ntg_set_stream_sources` (Phase 3: the
   resolved `stream_url`/local file; Phase 5: the DSP frame pump
   via `ntg_send_external_frame`).
Pause/resume/mute/unmute/stop map to `ntg_pause`/`ntg_resume`/
`ntg_mute`/`ntg_unmute`/`ntg_stop`. `ntg_time` is the call-clock
source if the wall-clock position proves unreliable (record as
a Wave 2 verification item, not a design decision).
The old engines' voice code is the port source for the failure
shapes only: ffmpeg Opus/RTP pipeline (`voicecall.js:
opusPipeline`, `voice.rs:opus_pipeline` — libopus 128k, 48 kHz
stereo, RTP PT 97) is **superseded by parlay-dsp** (Phase 5)
and is NOT ported; the sandbox UDP EPERM block and the
"opus-rtp-ready, not live" reporting discipline ARE ported
(§1.17).

---

## §4. Room state machine design

### 4.1 States and fields

One `Room` per chat (`room.Manager`, map keyed by `chat_id`,
mutex-guarded; all mutations through methods, never direct
field access — vc-go `room.go` pattern). States:

```
Idle ──vc.play (idle)──▶ Playing ──vc.pause──▶ Paused ──vc.resume──▶ Playing
  ▲                        │  │                       │
  │                        │  └─vc.skip/advance───────┘
  └──── vc.stop / advance-to-empty / autoplay-exhausted ──┘
```

Fields (persisted, §4.6): `current: Track|null`,
`waiting: [QueueItem]` (play order, 0-based positions),
`paused: bool`, `volume: int 0–200 (default 100)`,
`is_muted: bool (default false)`, `muted_volume: int|null`
(pre-mute volume, vc-ts rule),
`position_sec: int64` (frozen offset), `started_at:
time|null` (zero when paused/idle),
`loop: int 0–10 (remaining replays)`,
`shuffle: bool`, `autoplay: bool`,
`autoplay_history: [track_id]` (cap 50),
`speed: float 0.5–4.0 (default 1.0, core field — stored, DSP
applies in Phase 5)`, `bass: int 0–100 (default 0, core field —
stored, DSP applies in Phase 5)`,
`radio: {station_id, stream_url, started_at_ms,
playlist_index} | null` (radio state, §7.2),
`fx: {effect, params} | null` (active FX preset, §2.10),
`last_track_id: string` (for the core's `last_track` display).

Position (§4.4): `position()` = `position_sec` when paused/idle,
else `position_sec + now - started_at` (seconds, floor).
`SeekTo` clamps `[0, duration]` when duration is known.
Unknown-duration (0) and live/radio tracks never auto-advance.

### 4.2 Queue ops (locked semantics)

- `Play(track, by)`: current nil → `StartTrack`; else append
  to waiting (YukkiMusic rule, all three engines).
- `Enqueue(track, by)`: always append, never starts.
- `Remove(pos)`: 0-based waiting index; out-of-range → error
  (no mutation). Re-index after removal.
- `Clear()`: waiting = []; current untouched.
- `PopNext()`: index 0, or a random index when shuffle is on
  (uniform over waiting; vc-go `rand.Intn`, vc-ts
  `Math.random`, vc-rust xorshift64* — any uniform RNG in Go).
  The current track is never touched by shuffle.
- `ShuffleInPlace()`: Fisher–Yates over waiting + `shuffle =
  true` (vc-ts/vc-rust rule, §1.9).
- `Move` is NOT a backend action (no `move` in the 56): the
  core implements it as list → remove → re-add (§1.7). Wave 2
  may add a `vc.queue` `move` verb as a contract extension, but
  it is not required for parity.

### 4.3 Advance (loop → queue → autoplay → idle)

```
Advance():
  1. if current != nil and loop > 0:
       loop--; StartTrack(current)          # Replay; fire TrackStarted
       return
  2. if next = PopNext(): StartTrack(next)   # fire TrackAdvanced + TrackStarted
       return
  3. if autoplay and picker != nil:
       if len(history) >= 50: history = []; GoIdle(); return
       pick = Related(last, exclude={last, queue, history})
       if pick != nil: history += pick.id; StartTrack(pick)  # fire TrackStarted
       else: GoIdle()
       return
  4. GoIdle()
```

- `StartTrack(t)`: current = t; position = 0; started_at =
  now; paused = false; note history (append id if new, cap 50
  — vc-go/vc-ts cap; vc-rust's 200-with-shift is NOT ported).
- `GoIdle()`: current = nil; position = 0; started_at = zero;
  paused = false; keep waiting (empty by construction here),
  keep shuffle, keep volume/mute; loop is already 0; autoplay
  flag is kept (it is a setting), history is kept.
- `Skip()`: loop = 0, then `Advance()`. `AutoplaySkip()` is
  NOT a skip — it is the gated pick (§1.11).
- `Stop()`: current = nil; waiting = []; loop = 0; autoplay =
  false; history = []; paused = false; position = 0; shuffle
  KEPT; volume/mute KEPT (vc-go/vc-ts/vc-rust agree on loop+
  autoplay reset and shuffle kept; volume/mute are not reset
  by any engine — record, don't invent).
- Autoplay runs ONLY for YouTube last-tracks; the picker runs
  OUTSIDE the room lock (network I/O) and the pick is applied
  only if the room is still in the same drained state (same
  current id, empty queue) — vc-go `Advance` rule. A `/play` or
  `/stop` in the meantime wins.
- Due-for-advance: `current != nil && duration > 0 && !paused
  && position >= duration`. In Phase 3 the trigger is the DSP
  track-boundary callback (in-process, no polling); until the
  DSP lands (Phase 5), a 1 s ticker in the backend (vc-go
  `runner.tick` / vc-rust `ticker` shape) drives it — the 5 s
  core watcher is deleted and never comes back. The ticker is
  an interim mechanism, flagged for removal in Phase 5.

### 4.4 Position, speed, bass

- Position is wall-clock derived (§4.1); the DSP's
  sample-accurate position replaces it in Phase 5 (the room
  keeps the wall-clock as fallback).
- `speed` (0.5–4.0, default 1.0) and `bass` (0–100, default 0)
  are core chat fields today (`speed`, `bass` keys in the core
  store; `/speed` and `/bass` re-issue a seek to restart the
  stream at the current position because no live filtering
  exists). The backend stores both per room and reports `speed`
  in `NowPlaying` (currently always 1.0 in the old engines);
  live application is Phase 5 DSP work. No new IPC action is
  needed for them in Phase 3 — but Wave 2 must route the core's
  `speed`/`bass` chat fields into the room on every `vc.play`
  (read them from `chat.settings` or a room-fields call; do not
  leave them core-only or the DSP will never see them).

### 4.5 Event firing points (exact)

Events go over the same UDS socket, unsolicited, with top-level
`chat_id` and `data` (uds-protocol §5). One emitter
(`internal/events`); only these call sites fire.

| Event | Fires when (exact point) | `data` |
|---|---|---|
| `TrackStarted` | `StartTrack` completes: `vc.play` on idle; `Advance` → Replay (loop) or Next; autoplay pick starts; `autoplay_skip` pick starts; `vc.play_file` starts; radio track starts (`vc.radio` start / `radio.start` handoff) | `{track, chat_id}` |
| `TrackAdvanced` | `Advance` pops a Next track (queue advanced, loop replay does NOT fire this) | `{track, previous_track_id, chat_id}` |
| `TrackEnded` | A track ends with no successor: `Advance` → Idle after a track was playing; `vc.skip` to empty; autoplay pick fails/empty | `{track, chat_id, reason}` — reason: `"queue_empty"` \| `"autoplay_failed"` \| `"skipped"` |
| `QueueEmpty` | The queue drains AND the room goes idle (same moment as `TrackEnded` when the queue is the cause; fires alongside it, not instead). Autoplay decision point for the core. | `{chat_id}` |
| `DownloadReady` | An **async** download finishes (OD-3 async paths only, §9-E1). Phase 3 synchronous `download.*` POSTs MUST NOT fire it (the response already carries the result). | `{run_id, file_path, track, chat_id}` |
| `VoiceError` | Voice-call failure the core should surface: JoinRequest handshake failure, `ntg_connect` failure, stream/DSP error mid-track, radio handoff failure when a radio was requested. NOT for resolve failures (those are synchronous action errors) and NOT for autoplay pick failures (those are `TrackEnded`/`QueueEmpty`). | `{chat_id, code, message}` |

The core's `onBackendEvent` (Phase 2) consumes these: it
refreshes/clears the now-playing card, clears `callActive` on
`TrackEnded`/`QueueEmpty`/`vc.stop`, and surfaces `VoiceError`.
Events must be fire-and-forget from the backend's side (never
block playback on the core's reader).

### 4.6 Persistence (bbolt, OD-5; fresh start, OD-2)

- Buckets: `rooms` (key `room:<chat_id>` → PersistedRoom JSON:
  current, waiting, paused, volume, is_muted, muted_volume,
  position_sec, loop, shuffle, autoplay, autoplay_history,
  speed, bass, radio, fx); `settings` (key `chat:<chat_id>` →
  15-key object, §5); `spotify_tokens` (key `user:<id>` →
  AES-256-GCM blob, §6); `spotify_state` (watches, dislikes);
  `shares` (key `share:<token>` → track); `votes` (key
  `vote:<track_id>` → entry); `downloads` (key `run:<run_id>` →
  run snapshot); `library` (§9-E2/E5/E7); `dlprefs` (key
  `dlpref:<user_id>` → §9-E9).
- Restored rooms come back **paused** (all three engines agree;
  never auto-resume audio after a restart). Position is frozen
  at capture time. The autoplay picker is re-installed by the
  manager on load (it is a function, not data).
- Persistence is on every mutation (vc-go `Manager.Changed`
  hook), atomic per write (bbolt single transaction; the old
  JSON-file temp+rename discipline is replaced, not ported).
  Kill −9 + restart preserves queue/state (Phase 3 exit
  benchmark).
- OD-2 fresh start: the store starts empty. Lost by decision:
  per-chat queues, engine choices, Spotify OAuth tokens (every
  Spotify user logs in again), share links/tokens, votes,
  per-chat settings, radio state. Nothing is migrated.
- The old per-service stores are the port source for shapes
  only: vc-go `store.go` (JSON `queues.json`, Mongo/Redis
  variants — deleted), vc-ts `store.js` (SharedStore: Mongo >
  Redis > JSON `state.json`, incl. a hand-rolled Mongo SCRAM +
  RESP client — deleted, do not port), vc-rust `store.rs`,
  share-rust `store.rs` (`share:links`), queue-rust `store.rs`
  (`queue:votes`), spotify-rust `store.rs` + `spotify-state.json`,
  radio-js `store.js` (`radio:state`). Canonical room docs
  (`vc-ts/src/canonical.js`, `vc-go/internal/store/canonical.go`)
  define the field set the bbolt `rooms` bucket must carry.

---

## §5. NetEase — `netease.*` (3 actions)

Shared: `services/netease-go/internal/netease/client.go` — `AuthMode()
= "music_u"` when the `music_u` cookie env is set, else
`"anonymous"`; `SongURLWithLevel` tries eapi v1 levels `standard →
higher → lossless → hires`, then weapi v1 standard, then weapi,
download, linuxapi, mirror (FLAC); `MaxBotAPIBYTES =
50*1024*1024` is enforced by the client before writing; crypto in
`weapi.go` (AES-CBC + RSA), `eapi.go` (AES-ECB encrypt/decrypt),
`linuxapi.go`. The core sends `track_id` as a string **or** a
number — the backend must accept both.

### 5.1 `netease.search` — POST, 30 s

- **Contract.** Req `{query}`. Resp `{tracks: [Track]}`.
- **Old impl.** netease-go `server.go:handleSearch` → `client.Search`
  (`netease.go:SearchSongs`). `POST /v1/netease/search` (and the
  generic `POST /v1/search` when source is netease/all/absent).
  Results are real NetEase tracks (id, title, artist, duration,
  album, url when resolvable).
- **Behaviour.** `Track.source` is `netease`; ids are the numeric
  NetEase song ids (as strings in the Track id). No stream URL is
  guaranteed at search time — the URL is resolved at play/download
  time (`SongURLWithLevel`).
- **New plan.** `netease.Search` → `resolver` (NetEase lane) →
  response.
- **Edge cases.** Empty query → `bad_request`; API error →
  `engine_unavailable` with the NetEase error; zero results → 200
  with `tracks: []` (not 404).

### 5.2 `netease.download` — POST, 120 s

- **Contract.** Req `{track_id, requested_by}`. Resp `{file_path,
  track, run_id}` (plus `size_bytes`/`oversize`).
- **Old impl.** netease-go `server.go:handleDownload` →
  `client.SongDetail` → `SongURLWithLevel` → `client.Download`.
  Filename: `netease-<id>-<artist>-<title>.<ext>` (sanitised,
  ≤120 chars, ext from the URL, default `.mp3`); destination is
  the shared download dir. **Oversize rule (NetEase-specific,
  stricter than §2):** the 50 MB cap is checked BEFORE writing
  (from the `Content-Length` / URL size) and mid-download (abort
  + partial-file deletion); an oversize track returns 200 with
  `{file_path: "", track, oversize: true, size_bytes, auth,
  note}` — "queued for a later MTProto pass, not downloaded".
  `auth` is `"music_u"` or `"anonymous"` (which credential level
  served the URL).
- **Behaviour.** Unlike `download.youtube` (§2.1), no oversize file
  is written. The run is opened the same way (12-hex run_id in
  the response). FLAC when the level yields it.
- **New plan.** `download.Netease` (native Go eapi/weapi/linuxapi
  client — the Go port of `client.go`, not a subprocess) → run →
  response.
- **Edge cases.** `track_id` missing → `bad_request`; detail
  lookup fails → `download_failed`; URL resolution fails at all
  levels → `download_failed` naming the last level tried;
  oversize → the 200-with-empty-path shape above (NOT an error);
  partial download on abort → deleted.

### 5.3 `netease.lyrics` — POST, 30 s

- **Contract.** Req `{track_id}`. Resp `{lyrics, source:
  "netease", track_id, title, artist, truncated}`.
- **Old impl.** netease-go `server.go:handleLyrics` →
  `client.Lyrics(id)` (`lyrics.go`, `GET /lyric/new` — the new
  endpoint, not the legacy `/lyric`). Synced lyrics (LRC) are
  returned with `source: "netease"` and `truncated: false`;
  title/artist come from a `SongDetail` call (empty on failure —
  lyrics are still returned). Writes the `.lrc` to the download
  dir (`netease-<id>.lrc`, `lrc_path` in the superset response,
  `""` when the write fails). No lyrics → 404 (`ErrNoLyrics`);
  other errors → 502 `engine_unavailable`.
- **Behaviour.** This is the synced-lyrics source for `vc.lyrics`
  when the current track is a NetEase track (§1.16) — the
  backend's `lyrics` package tries LRCLib first for generic
  tracks and NetEase here for `netease:` tracks.
- **New plan.** `lyrics.NeteaseLyrics` → response (+ optional
  `lrc_path` superset field).
- **Edge cases.** `track_id` missing → `bad_request`; no lyrics
  → `not_found` (not an empty string); detail failure does not
  fail the lyrics call.

---

## §6. Spotify — `spotify.*` (11 actions)

Shared: `services/spotify-rust/src/main.rs` + `auth.rs` +
`crypto.rs` + `classifier.rs`. State file `data/spotify-state.json`
(watches: user_id → started_at; dislikes: user_id → set of track
ids); the bbolt `spotify_state` bucket replaces it (§4.6). App
token: client-credentials, cached in memory, refreshed 60 s
before expiry. All actions that need Web API creds return 502
`engine_unavailable` "Spotify credentials not configured" when
`SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` are absent — except
`spotify.resolve`, which degrades to parse-only (§6.1). The poller
thread runs when `SPOTIFY_POLL_INTERVAL_SECS > 0` (0 = disabled).

### 6.1 `spotify.resolve` — POST, 30 s

- **Contract.** Req `{url}`. Resp `{kind: track|album|playlist|
  artist, id, title?, name?, total?, api: "spotify-web-api"|"none",
  api_error?, note?}`.
- **Old impl.** spotify-rust `main.rs:handle_spotify_resolve` →
  `parse_spotify_url` (accepts `open.spotify.com/<kind>/<id>` and
  `spotify:<kind>:<id>`; kind track|album|playlist|artist).
  With creds: real Web API fetch — track → full track object
  (`track` superset field), collection → name + total + `api:
  "spotify-web-api"`. Without creds: parse-only, `api: "none"`,
  `note` naming what is missing (honest degradation, never a
  fake). The downloaders' `spotify.resolve_track` /
  `resolve_collection` are the download-side twins (same parse,
  yt-dlp/oEmbed fallback for metadata).
- **New plan.** `spotify.Resolve` → response. Used by `vc.play`
  (§3.1 row 6) and `download.spotify*`.
- **Edge cases.** Not a Spotify URL/URI → `bad_request`; unknown
  kind → `bad_request`; API error with creds → 502
  `engine_unavailable` with `api_error` set (the response is
  still returned with `api_error`, not a bare error — the core
  renders the note).

### 6.2 `spotify.collection` — POST, 30 s

- **Contract.** Req `{kind: album|playlist|artist, id}`. Resp
  `{api, kind, name, tracks: [Track], truncated: boolean}`.
- **Old impl.** spotify-rust `handle_spotify_collection` (track
  kind → 400 "use resolve"; no creds → 503 `engine_unavailable`
  pointing at the downloaders fallback; with creds real Web API
  list; response adds `total` as a superset field).
  Downloaders' `spotify_collection`: with creds real Web API
  (album/playlist/artist-top), without creds yt-dlp/oEmbed
  fallback with `api` saying so honestly; capped at 50
  (`truncated` set). Track fields come from the Web API
  (id, name, artists, duration_ms → duration_sec, isrc when
  present).
- **New plan.** `spotify.Collection` → `resolver` (Spotify lane)
  → response.
- **Edge cases.** kind = track → `bad_request`; no creds →
  `engine_unavailable` (spotify-rust) — the downloaders'
  fallback lives in `download.spotify_playlist` (§2.6), not here;
  empty collection → 200 `tracks: []`.

### 6.3 `spotify.status` — GET, 10 s

- **Contract.** Req `{}`. Resp `{api: "spotify-web-api"|"none",
  credentials_configured: boolean, token_cached: boolean}`.
- **Old impl.** spotify-rust `handle_spotify_status` — returns the
  contract fields PLUS superset: `user_tokens: int`,
  `watching: int`, `gemini_configured: boolean`,
  `poll_interval_secs: int`, `polling: boolean`. This is the
  action the core's `/spotify` status card reads.
- **New plan.** `spotify.Status` (read-only over `store` +
  config) → response. Keep the superset fields.
- **Edge cases.** Never errors (200 always); `api` is `"none"`
  when creds are absent.

### 6.4 `spotify.auth` — POST, 30 s (login / callback / refresh / revoke)

- **Contract.** Req `{action: login|callback|refresh|revoke,
  user_id, code?, state?, redirect_uri?}`. Resp `{authorized:
  boolean, authorize_url?, expires_at?, was_authorized?}`.
- **Old impl.** `auth.rs` (`AuthManager`) + `main.rs:handle_auth`.
  Missing `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET`/
  `SPOTIFY_REDIRECT_URI` → 503 `engine_unavailable` for ALL
  four sub-actions. Full flow is §8.2.
- **New plan.** `spotify.Auth` → `store` (bbolt `spotify_tokens`
  bucket, AES-256-GCM blobs, §8.2) → response.
- **Edge cases.** See §8.2 (state mismatch, exchange failure,
  token drop on refresh failure, revoke idempotence).

### 6.5 `spotify.watch` — POST, 30 s

- **Contract.** Req `{action: start|stop, user_id}`. Resp `{}`.
- **Old impl.** spotify-rust `handle_spotify_watch` — actual
  responses are supersets: start → `{watching: true, user_id}`;
  stop → `{watching: false, was_watching: bool, user_id}`.
  Watches are `user_id → started_at` (in-memory + state file).
  The **poller** (when enabled) runs per watched user with a
  stored token: refresh when `needs_refresh` (expires_at ≤
  now + 120 s), fetch `/v1/me/player/currently-playing`, on
  track change run the skip-check decision and record the
  verdict; 401 → drop the token (user must re-login); other
  failures keep the previous state. The poller does NOT skip on
  the user's player (scopes are read-only).
- **New plan.** `spotify.Watch` → `store` watch registry; the
  poller is a `spotify` goroutine (same cadence, same 120 s
  refresh window).
- **Edge cases.** Missing `user_id` → `bad_request`; action
  other than start|stop → `bad_request`; start without a token
  → 404 `not_found` "no Spotify token — log in first" (the old
  code stored the watch anyway and the poller skipped it; the
  backend must answer honestly instead); stop of a non-watch →
  200 `{watching: false, was_watching: false}`.

### 6.6 `spotify.watches` — GET, 10 s

- **Contract.** Req `{}`. Resp `{watches: [{user_id,
  started_at}]}`.
- **Old impl.** spotify-rust `handle_spotify_watches` — reads the
  in-memory watch map (persisted in `spotify-state.json`).
- **New plan.** `spotify.Watches` (read-only over `store`) →
  response.
- **Edge cases.** Never errors; empty → `watches: []`.

### 6.7 `spotify.dislike` — POST, 30 s

- **Contract.** Req `{action: add|remove, user_id, track_id}`.
  Resp `{disliked: boolean}`.
- **Old impl.** spotify-rust `handle_spotify_dislike` (action
  defaults to `add`; actual responses are supersets: add →
  `{disliked: true, user_id, track_id}`; remove →
  `{disliked: false, removed: true, user_id, track_id}`).
  Dislikes are per-user sets of track ids (state file; bbolt
  `spotify_state` bucket in Phase 3). The core's
  `pl:spotify:dislike:<id>` button (routing A93) is the only
  live caller; `cbSpotify` answers "not available" for every
  other verb (§9-D7).
- **New plan.** `spotify.Dislike` → `store` → response. This is
  the store the skip-check reads (§6.8).
- **Edge cases.** Missing `user_id`/`track_id` → `bad_request`;
  unknown action → `bad_request`; remove of a non-dislike →
  200 `{disliked: false, removed: false}`.

### 6.8 `spotify.skip_check` — POST, 30 s

- **Contract.** Req `{track: Track, user_id?}`. Resp `{skip:
  boolean, reason: "disliked"|"profanity"|"ai_generated"|
  "clean"|"unknown", matched_word?: string, confidence?:
  float}`.
- **Old impl.** spotify-rust `handle_spotify_skip_check` →
  `decide_skip` (`main.rs`) + `classifier.rs`. Actual response
  adds superset fields: `matched_word, disliked_by, lyrics_checked,
  lyrics_available, lyrics_source`. Decision order (exact):
  1. **Disliked** (track id in any watched user's dislike set,
     or the requesting user's set when `user_id` is given) →
     skip, reason `disliked`, `disliked_by` = the disliking
     user ids (or count — see §9-D6).
  2. **No Gemini key**: 54-word clearly-profane wordlist on
     title + artist (word-boundary, case-insensitive; mild
     words deliberately excluded) → skip `profanity` with the
     first matched word.
  3. **Lyrics** via Genius → Musixmatch → LRCLib (curl
     subprocess, 5 s cap each), then the classifier: Gemini
     (key set) replaces the wordlist verdict; Gemini failure →
     `wordlist-fallback`; no key → `wordlist`; no key + no
     lyrics → verdict `unknown`.
  4. **`ai_generated` skips ONLY at confidence ≥ 0.7**
     (`AI_SKIP_CONFIDENCE`); below that the track is `clean`
     with the verdict recorded.
- **New plan.** `spotify.SkipCheck` → `store` (dislikes) →
  `classifier` (wordlist, then Gemini when configured) →
  response. The wordlist is a data file in the repo, not a
  hardcoded list in the handler (the 54-word list must be
  ported verbatim — it is the user's content policy).
- **Edge cases.** Missing track → `bad_request`; lyrics fetch
  failure → `unknown` (not an error — the check degrades);
  Gemini unparsable JSON → wordlist fallback (never an error);
  `user_id` absent → dislikes checked across all users.

### 6.9 `spotify.now_playing` — GET, 10 s

- **Contract.** Req `{user_id}`. Resp `{playing: boolean,
  user_id, track: Track|null, checked_at, last_skip,
  last_reason}`.
- **Old impl.** spotify-rust `handle_spotify_now_playing` —
  returns the **last polled** state (not a live fetch): `{playing,
  user_id, track {id,title,artist,duration_sec}, checked_at,
  last_skip, last_reason, classifier_verdict}`. No track →
  200 `{playing: false, track: null, note}` (NOT 404).
- **New plan.** `spotify.NowPlaying` (read-only over the poller
  state in `store`) → response. The poller is the only writer.
- **Edge cases.** Missing `user_id` → `bad_request`; unwatched
  user → 200 `{playing: false, track: null}`; a live fetch is
  NOT done here (the poller cadence is the freshness contract).

### 6.10 `spotify.details` — GET, 10 s

- **Contract.** Req `{user_id, track_id}`. Resp `{track: Track,
  disliked_by: [UserId], ignored_by: [UserId], lyrics?, lyrics_url?,
  language?, audio_features?, profane_words: [string]}`.
- **Old impl.** spotify-rust `handle_spotify_details` — requires
  creds else 502. Fetches `/tracks/<id>` (Web API app token),
  genres via `/artists?ids=` (failure → empty list), audio
  features via deprecated `/audio-features/<id>` (null when
  refused, **never faked**), lyrics + Genius link via
  `fetch_lyrics_with_url`, first profanity word in lyrics
  (`profane_words` = the matched words found). **Divergence
  (§9-D6): the old code returns `disliked_by` as an INTEGER
  COUNT and `ignored_by: 0` (integer); the contract says both
  are `[UserId]` arrays. `language` is always null (no
  detection exists); `ignored_by` is always 0 (no ignore store
  exists).**
- **Behaviour.** This action (with `spotify.analyze`, §6.11)
  already covers the `/details` companion surface — §9-E8.
- **New plan.** `spotify.Details` → Web API + `lyrics` +
  `store` (dislikes) → response **in the contract shape**
  (`disliked_by`/`ignored_by` as arrays; `ignored_by` from the
  new ignore store if §9-E8's `spotify.ignore` lands, else `[]`).
  Wave 2 must not copy the count-vs-array bug.
- **Edge cases.** No creds → 502; track lookup failure → 502;
  lyrics absent → `lyrics: null` (not 404 — the track details
  still return); audio-features refusal → `audio_features: null`.

### 6.11 `spotify.analyze` — POST, 30 s

- **Contract.** Req `{user_id, track_id}`. Resp `{analysis:
  string, profane_words: [string]}`.
- **Old impl.** spotify-rust `handle_spotify_analyze` — requires
  creds else 502; track lookup failure → 502; no lyrics → 404
  `not_found`. `analysis` is one sentence from the classifier
  (Gemini when configured, wordlist otherwise);
  `profane_words` = EVERY matched word in the lyrics, deduped,
  in order of appearance. This action already covers the
  `/analyze` companion surface — §9-E8.
- **New plan.** `spotify.Analyze` → Web API + `lyrics` +
  `classifier` → response.
- **Edge cases.** No creds → 502; no lyrics → 404; no profanity
  → 200 `{analysis: "<clean sentence>", profane_words: []}`.

---

## §7. Radio, sharing, voting, FX, search, GUI (14 actions)

### 7.1 `radio.list` — GET, 10 s

- **Contract.** Req `{}`. Resp `{stations: [string]}`.
- **Old impl.** radio-js `main.js:GET /v1/radio/stations` →
  `stations.js` (6 curated stations, Parlay-original — upstream
  openradiobot has no station list). Actual response is a
  **superset**: stations are objects `{id, name, genre,
  stream_url, reachable, format}` (the contract says
  `[string]`). Reachability comes from ffprobe probes at startup
  + per start (10 s timeout; ffprobe missing →
  `reachable: false, probe: "ffprobe not installed"`).
- **Stations (locked list):** groovesalad (SomaFM Groove Salad,
  `https://ice1.somafm.com/groovesalad-128-mp3`), lush, radioparadise
  (`https://stream.radioparadise.com/aac-320`), fip, bbcws, kexp.
  `findStation` matches id, exact name, or name substring
  (case-insensitive).
- **New plan.** `radio.List` → response with the object shape
  (keep the superset; the contract's `[string]` is corrected in
  Wave 2 — the core's `/radio` station picker reads `id`/`name`).
- **Edge cases.** Never errors; `reachable` is best-effort (a
  station that fails its probe is still listed, flagged).

### 7.2 `radio.start` — POST, 30 s

- **Contract.** Req `{chat_id, station: string}`. Resp
  `{stream_url: string}`.
- **Old impl.** radio-js `main.js:POST /v1/radio/start` →
  `startRadio(chat_id, station_id)`. Actual response is a
  **superset**: `{station, station_name, stream_url, reachable,
  probe, format, fallback_playlist, vc_handoff, now_playing}`
  (the contract's `{stream_url}` is the minimum). Unknown
  station → 400. Restarting replaces the existing radio for the
  chat. The radio track: id `radio:<station_id>:<playlist_index>`,
  title `<station> — <playlist entry>`, duration 0, `is_live:
  true`, position = elapsed % 180 s. Fallback playlist rotates
  every 180 s (15 s ticker). State persisted (`radio:state`
  shared store or `state.json`); restored radios keep
  `vc_handoff: null`.
- **`vc_handoff`.** After starting, radio-js tries vc-go then
  vc-ts `POST /v1/play {chat_id, query: stream_url,
  requested_by: 0}` (15 s timeout each); failure is reported in
  `vc_handoff` and NEVER fails the start. In Phase 3 the handoff
  is internal (`room` + `voice`, §1.19) — no HTTP, no second
  engine — but the `vc_handoff` field stays in the response so
  the core can diagnose a failed handoff.
- **New plan.** `radio.Start` → `room` (radio track as current)
  → `voice` (join + bind stream) → response (contract +
  `vc_handoff` + `now_playing` superset).
- **Edge cases.** Unknown station → `bad_request` (name the
  valid stations — the core's `/radio` picker lists them);
  stream unreachable → still starts (the radio is the fallback
  playlist until the stream recovers — `reachable: false` in
  the response, not an error); `chat_id` 0 → `bad_request`.

### 7.3 `radio.stop` — POST, 30 s

- **Contract.** Req `{chat_id}`. Resp `{stopped: boolean}`.
- **Old impl.** radio-js `main.js:POST /v1/radio/stop` →
  `stopRadio(chat_id)`. Actual response is `{vc_cleared:
  boolean}` (the contract's `{stopped}` was never what
  radio-js returned — §9-D8). 404 when no radio for the chat.
  `clearVcIfRadio`: reads the engine now-playing and stops the
  VC only if the current track is the radio stream.
- **New plan.** `radio.Stop` → `room` (clear radio state; stop
  the room's radio track only if it is still the current track)
  → `voice` (leave if the call was radio-only) → response
  `{stopped: true, vc_cleared: <bool>}` (both keys).
- **Edge cases.** No radio → `not_found`; a normal music track
  that replaced the radio is NOT stopped (the radio state is
  cleared, the music keeps playing).

### 7.4 `radio.status` — GET, 10 s

- **Contract.** Req `{chat_id}`. Resp `{station, stream_url,
  now_playing: Track|null, started_at_ms}`.
- **Old impl.** radio-js `main.js:GET /v1/radio/status?chat_id=`
  → 404 when no radio. Actual response adds superset fields:
  `station_name, started_at_ms, listeners: 0, total_listeners: 0,
  loop_type: "none", autoplay_enabled: false, queue_length: 0`
  (listeners are always 0 — no listener accounting exists).
- **New plan.** `radio.Status` (read-only over `room.radio`) →
  response (contract + superset, `listeners: 0` kept honestly).
- **Edge cases.** No radio → `not_found`; `chat_id` missing →
  `bad_request`.

### 7.5 `share.create` — POST, 30 s

- **Contract.** Req `{chat_id, track: Track, created_by:
  UserId}`. Resp `{link: string, token: string}`.
- **Old impl.** share-rust `main.rs:handle_share` — POST
  `/v1/share {created_by, track}` → `{link, token}`. Token: 16
  random alphanumeric chars, collision-checked against the
  store. Link: `https://t.me/<bot>?start=share-<token>` (bot
  username from env `TELEGRAM_BOT_USERNAME`). The stored record
  is token → track only (anonymous — `created_by` is accepted
  but NOT stored). Persistence `./data/shares.json` or shared
  store key `share:links` (whole map, one value).
- **Behaviour.** The core's `/share` handler builds the share
  card from the link; the deep link `start=share-<token>` is
  resolved by `share.resolve` (§7.6) when the recipient opens it.
  No expiry (the old code has none — record, don't invent one).
- **New plan.** `share.Create` → `store` (`shares` bucket, key
  `share:<token>`) → response. Token generation: `crypto/rand`,
  16 chars, collision-checked in the bucket.
- **Edge cases.** Missing track → `bad_request`; missing
  `created_by` → `bad_request` (the old code accepted it but
  the core always sends it); bot username unset → link uses the
  bare `t.me/?start=share-<token>` form — Wave 2 must set the
  username in config, the backend must not invent one.

### 7.6 `share.resolve` — GET, 10 s

- **Contract.** Req `{token}`. Resp `{track: Track}`.
- **Old impl.** share-rust `main.rs:handle_get_share` — GET
  `/v1/share/:token` → `{track}` or 404 `not_found` "unknown
  share token". No auth (the token IS the auth).
- **New plan.** `share.Resolve` (read-only over `store`) →
  response.
- **Edge cases.** Missing token → `bad_request`; unknown token
  → `not_found`; a resolved token is NOT consumed (the same
  link works repeatedly — the old code never deletes on resolve).

### 7.7 `queue.vote` — POST, 30 s

- **Contract.** Req `{track: Track, vote: approve|decline,
  voter_id: UserId}`. Resp `{queued: boolean, approvals: int,
  declines: int, threshold: int}`.
- **Old impl.** queue-rust `main.rs:handle_vote` — POST
  `/v1/queue/vote {track, vote, voter_id}` → actual response
  adds `track_id` (superset). One vote per voter per track
  (re-vote replaces; a decline cancels that voter's approval).
  Track key = `track.id` when present, else the full rendered
  track JSON. `queued = approvals >= threshold`. Threshold from
  env `PARLAY_VOTE_THRESHOLD`, default **3** (in Phase 3:
  `parlay.toml [voting] threshold`, default 3 — §4, config).
  Persistence `./data/votes.json` or shared store key
  `queue:votes`. Save failure → 500 `internal`. Validation:
  missing track → 400; bad vote → 400; missing voter_id → 400
  (`voter_id` accepts string or number).
- **Behaviour.** `queued` is a threshold signal, NOT a queue
  insertion: the core enqueues the track (via `vc.queue add`)
  when `queued` becomes true. A track that reaches threshold
  stays queued (the vote record is not consumed — a later
  decline can drop it back below threshold, and the core must
  not remove an already-enqueued track on that signal; record
  as a Wave 2 core rule).
- **New plan.** `vote.Cast` → `store` (`votes` bucket) →
  response. Threshold read from config at call time (a config
  reload changes the threshold for new votes; existing tallies
  are recomputed against the new threshold — record).
- **Edge cases.** See validation above; `vote` other than
  approve|decline → `bad_request`; a re-vote with the same value
  is a 200 no-op (same tallies).

### 7.8 `fx.apply` — POST, 120 s

- **Contract.** Req `{effect, input_path, output_path?,
  requested_by?}`. Resp `{output_path: string}`.
- **Old impl.** audio-fx-rust `main.rs:handle_fx` — POST
  `/v1/fx`. Effects (ffmpeg `-af` chains): slow-reverb
  `atempo=0.8,aecho=0.8:0.88:60:0.4`; nightcore
  `asetrate=44100*1.25,atempo=1/1.25`; bass-boost
  `bass=g=10:f=110:w=0.6,volume=0.9`; 8d
  `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`;
  `original` = stream copy (`-c:a copy`, no re-encode).
  Default output `<stem>_<effect>.<ext>` in the input dir.
  Validation: unknown effect → 400; missing input_path → 400;
  input not a file → 400; `..` traversal rejected (400);
  ffmpeg missing → 500 `engine_unavailable`; ffmpeg failure →
  500 `internal` (last 500 chars of stderr). A real ffmpeg
  subprocess ran per call.
- **Phase 3 behaviour (locked by the task + FINAL §1.2
  prohibition 3 — no subprocess per call on the primary path,
  and the DSP is Phase 5).** `fx.apply` in Phase 3 is
  **record-only**: it stores the FX preset + parameters on the
  room (and a per-file record keyed by `input_path`) and returns
  `output_path` honestly = the input path unchanged (no
  re-encode, no ffmpeg subprocess). The response gains superset
  fields: `{output_path, applied: false, effect, pending_dsp:
  true}` — EXCEPT `original`, which is a no-op by definition:
  `{output_path: <input>, applied: true, effect: "original",
  pending_dsp: false}`. The core's `cbDl` FX branch
  (dispatcher.cpp:7151) today calls `fx.apply` (120 s timeout)
  and then `sendAudioFile(output_path)` after the toast
  "Downloading and applying effect..." — **Wave 2 core MUST
  change that flow**: when `applied: false` it must tell the
  user the effect is recorded and will be applied live by the
  DSP in Phase 5, and must NOT present the unprocessed file as
  processed (§10, open item). The alternative (keep ffmpeg as
  an interim) is REJECTED by the locked prohibition.
- **New plan.** `fx.Apply` → `room` (store preset on the room)
  + `store` (per-file FX record) → response. The DSP (Phase 5)
  reads the room's `fx` field at stream time.
- **Edge cases.** Unknown effect → `bad_request` (same 400 as
  the old code); missing input_path → `bad_request`; input not
  a file → `bad_request`; `..` in the path → `bad_request`
  (same `sanitize_path` rule as §3.3); `output_path` given →
  accepted and stored as the intended output, but the returned
  `output_path` is still the input until the DSP writes it.

### 7.9 `search.query` — POST, 30 s

- **Contract.** Req `{query, source: youtube|spotify|netease|
  deezer|lemonic|all, limit?: int}`. Resp `{results: [Track],
  source_status: {source: "ok"|"failed"}}`.
- **Old impl.** Three engines, same shape: vc-ts `main.js:
  /v1/search` (sources youtube|jiosaavn|lemonic|all, limit clamp
  1–10 default 5, per-source status, all-fail → 502; results
  sliced to limit*3); downloaders `server.py:/v1/search`
  (sources youtube|spotify|deezer|all, limit clamp 1–20 default
  5; Spotify without creds → YouTube fallback marked honestly;
  Deezer best-effort, swallowed on failure); vc-rust has no
  search endpoint (its `resolve_query` is the play path).
  **Contract gap (§10-D9): `jiosaavn` is missing from the
  `source` enum** though vc-ts serves it and the core's search
  UI lists it — Wave 2 adds `jiosaavn` to the enum.
- **Behaviour.** Fan-out per source (parallel, each with its own
  timeout); a source that fails reports `"failed"` in
  `source_status` and contributes nothing — it never fails the
  whole call. `all` = every configured source; results are
  interleaved per source (not globally ranked — the old code
  concatenates per-source lists, sliced to `limit*3` in vc-ts).
  Spotify results come from the Web API search (creds) or the
  YouTube fallback (no creds, marked in `source_status` note).
- **New plan.** `search.Query` → `resolver` fan-out (YouTube
  InnerTube, JioSaavn proxy, Lemonic, Spotify Web API, Deezer,
  NetEase) → response.
- **Edge cases.** Empty query → `bad_request`; unknown source →
  `bad_request`; limit outside 1–20 → clamped (vc-ts 1–10,
  downloaders 1–20 — unify on **1–20, default 5**); every source
  fails → 502 `engine_unavailable` (not an empty list — the
  user must know the search itself failed).

### 7.10 `search.inline` — POST, 30 s

- **Contract.** Req `{query, user_id, chat_id?}`. Resp
  `{results: [Track], mode: "np"|"s"|"l"|"lib"|"del"|"search"|
  "help"|"hint"}`.
- **Old impl.** vc-ts `resolvers.js:inlineMode` + core
  `handleInlineQuery` / `inlineTrackResult`
  (dispatcher.cpp ~6185). Modes, by query shape:
  - `np` / `s` / `l` (whole query): the user's most recent
    group chat (`last_chat:<user>` in the core store) → `np` =
    now-playing track as a result, `s` = search in that chat,
    `l` = lyrics of the current track (truncated to 4000 chars
    for inline). No known chat → empty results.
  - `lib <arg>` / `del <arg>` / `search <arg>`: library search /
    library delete / free search.
  - `< 3 chars` or empty → `hint` (usage text).
  - anything else → `search` (YouTube search, first results).
  - `help` → help text.
  Audio result only when `stream_url` is known; else an article
  with an `[Open in Parlay Bot]` button
  (`t.me/<bot>?start=spotify----<track_id>` → `/start` deep link
  → `download.spotify` / `download.youtube`). A track carries NO
  `file_id` → no cached-audio result (§9-E10); NO size on Track
  → no oversize branch (§9-E11). Blocked/maintenance users →
  empty results silently.
- **New plan.** `search.Inline` → `room` (np: current track of
  `chat_id` or the user's last chat) + `library` (lib/del) +
  `resolver` (search) + `lyrics` (l) → response. `chat_id` is
  optional in the request and is REQUIRED for np/s/l — the core
  must send the user's last chat id when it knows it.
- **Edge cases.** Unknown user → empty results (200, not an
  error — inline answers must never error visibly); `np` with
  nothing playing → empty results; `l` with no lyrics →
  `not_found` lyrics text as the result (not an error).

### 7.11 `lemonic.search` — POST, 30 s

- **Contract.** Req `{query, limit?: int}`. Resp `{results:
  [Track]}`.
- **Old impl.** vc-ts `resolvers.js:searchLemonic` —
  `GET ${LEMONIC_API}/api/search?query=&limit=` (default base
  `https://api.lemonic.xyz`; Bearer when `LEMONIC_API_KEY` is
  set); on failure, Spotify client-credentials search as
  fallback (token cached in memory, 60 s early refresh, market
  IN) with results mapped to `lemonic:<spotify-id>`. Results
  carry `stream_url` when the raw endpoint has it.
- **New plan.** `resolver.LemonicSearch` → response.
- **Edge cases.** Empty query → `bad_request`; limit clamp 1–20
  default 5; Lemonic down + Spotify fallback also down → 502
  `engine_unavailable`.

### 7.12 `lemonic.download` — POST, 120 s

- **Contract.** Req `{track_id, requested_by}`. Resp
  `{file_path, track, run_id}` (plus `size_bytes`/`oversize`).
- **Old impl.** vc-ts `resolvers.js:downloadLemonicTrack` —
  `GET /api/raw/track/<id>` → `stream_url`/`download_url`/`url`
  → fetch to `data/lemonic/<sanitised-id>.mp3` (60 s fetch
  timeout). No run registry in vc-ts (the action predates
  IPC-4); the Phase 3 backend opens a run like every other
  download (§2 intro).
- **New plan.** `download.Lemonic` → run → response.
- **Edge cases.** Unknown id → `download_failed`; fetch timeout
  (60 s) → `download_failed`; `track_id` missing →
  `bad_request`.

### 7.13 `gui.now_playing` — GET, 10 s

- **Contract.** Req `{chat_id}`. Resp `{now_playing:
  NowPlaying}`.
- **Old impl.** gui-js `app/main.js:/api/now-playing?chat_id=` —
  PROXIES `CORE_URL` (flat core shape). `CORE_URL` unset →
  local-demo state labelled `source: "local-demo"` (DEV ONLY).
- **Behaviour (Phase 3).** Direct in-memory read of the room
  snapshot (same as `vc.now_playing`, §1.13) — no proxy, no
  demo state. The Mini App itself (the HTML/JS player) is
  Phase 6; Phase 3 serves only the two data actions.
- **New plan.** `gui.NowPlaying` → `room.Snapshot` → response.
- **Edge cases.** Same 400/404 rules as `vc.now_playing`; the
  `local-demo` fallback MUST NOT be carried forward (it would
  serve fake state to a real user).

### 7.14 `gui.queue` — GET, 10 s

- **Contract.** Req `{chat_id}`. Resp `{queue: [QueueItem]}`.
- **Old impl.** gui-js `app/main.js:/api/queue?chat_id=` — same
  proxy/demo pattern as §7.13.
- **Behaviour (Phase 3).** Direct read of the room's waiting
  queue (§1.7 list semantics).
- **New plan.** `gui.Queue` → `room` → response.
- **Edge cases.** Unknown chat → `[]` (200, same as `vc.queue`
  list); never the demo state.

---

## §8. Deep dives — `chat.settings`, `spotify.auth`, `download.progress` lifecycle

### 8.1 `chat.settings` — GET/POST, 10 s (all 15 keys)

- **Contract.** Req `{chat_id, key?, value?}` (GET reads one or
  all; POST sets one). Resp `{settings: {<15 keys>}}`.
- **Old impl.** **No engine ever implemented it.** The old
  implementation is core-local: `Dispatcher::chatSettings` /
  `saveChatSettings` (dispatcher.cpp:1199–1235) over the core
  store hash `chat:<id>` with string fields; `ui_texts::
  ChatSettings` (ui_texts.hpp:1454) defines 12 fields; `ipc.json`
  lists 15 keys. The action exists in `ipc.cpp`'s GET map and
  the 10 s deadline table, but no engine serves it — FINAL §1.3
  Part 2: the backend owns per-chat settings, all 15 keys, in
  Phase 3. The core's settings panel (`/settings`, routing
  A1–A15) reads/writes through this action in Phase 3.
- **The 15 keys (verified mapping + defaults).** `clean_delay_min`
  is an alias of `clean_mode`, not a separate field.

| # | Key (ipc.json) | Type | Core store field | Default | Meaning / values |
|---|---|---|---|---|---|
| 1 | `admin_mode` | int | `admin_mode` | `0` | 0 = admins + auth users, 1 = admins only, 2 = everyone |
| 2 | `play_mode` | string | `play_mode` | `"everyone"` | who may play (`"everyone"` / `"admins"`) |
| 3 | `cmd_delete` | bool | `cmd_delete` ("on"/"off") | `false` | delete command messages after handling |
| 4 | `clean_mode` | int | `clean_mode` | `0` | auto-clean delay in minutes; 0 = off. Panel cycles 15→30→60→5 |
| 5 | `clean_delay_min` | int | **same field as `clean_mode`** (alias) | `0` | alias — GET returns the same value as `clean_mode`; POST to either key writes the same field |
| 6 | `thumbnails` | bool | `thumb` | `true` | show thumbnails on cards |
| 7 | `autoplay` | bool | `autoplay` (also mirrored to store key `autoplay:<chat_id>` "1"/"0") | `false` | autoplay on queue drain (§1.10) |
| 8 | `autoplay_lang` | string | **none exists** | `""` (proposed — §10-U3) | autoplay language preference; no value anywhere in-tree |
| 9 | `autoplay_mood` | string | **none exists** | `""` (proposed — §10-U3) | autoplay mood preference; no value anywhere in-tree |
| 10 | `silent_mode` | bool | `silent` | `false` | silent mode (no join/leave notices) — note the name differs: core field is `silent`, ipc key is `silent_mode` |
| 11 | `auto_delete` | bool | `autodelete` | `false` | auto-delete bot messages |
| 12 | `auto_delete_min` | int | `autodelete_min` | `5` | auto-delete delay in minutes; valid 2–7 (panel timer buttons 2–7) |
| 13 | `language` | string | `lang` | `"en"` | English-only; other codes are refused by the core |
| 14 | `paginate_purge_old` | bool | `page_purge` | `true` | purge old paginated messages |
| 15 | `chat_music_download` | bool | `musicdl` | `true` | allow music downloads in this chat |

- **New plan.** `settings.Get/Set` → `store` (`settings` bucket,
  key `chat:<chat_id>`, one JSON object with all 15 keys) →
  response. GET with no `key` returns all 15; GET with `key`
  returns that key only (still under `settings`). POST validates
  the value's type and range (int ranges above; `auto_delete_min`
  2–7; `admin_mode` 0–2; `language` must be `"en"` until the
  core's i18n lands — other codes → `bad_request`).
  `autoplay` writes through to the room (§1.10) so the two
  views never disagree.
- **Edge cases.** Unknown key → `bad_request` naming the 15
  keys; wrong type → `bad_request`; `chat_id` 0 →
  `bad_request`; OD-2 fresh start: every chat starts at the
  defaults above (the core's old store values are NOT migrated).
  `clean_mode` and `clean_delay_min` must stay in sync — they
  are one field read through two names.

### 8.2 `spotify.auth` — login / callback / refresh / revoke (full flow)

- **Old impl.** `spotify-rust/src/auth.rs` (`AuthManager`) +
  `main.rs:handle_auth` (main.rs:1719). Missing
  `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` /
  `SPOTIFY_REDIRECT_URI` → 503 `engine_unavailable` for ALL
  four sub-actions (checked before the sub-action dispatch).
- **login.** Request `{action: "login", user_id}`. Response
  `{authorized: false, authorize_url}`. `state` =
  `<user_id>-<random16>` (stored server-side, single-use, bound
  to the user). `authorize_url` =
  `https://accounts.spotify.com/authorize?client_id=…&
  response_type=code&redirect_uri=<encoded>&scope=
  user-read-currently-playing%20user-read-playback-state&
  state=<state>`. Scopes are read-only by design (the poller
  never skips on the user's player, §6.5). The core sends the
  user this URL (or an inline button to it).
- **callback.** Request `{action: "callback", user_id, code,
  state}`. Validates `state` (unknown/mismatched/expired →
  401 `unauthorized` "invalid state — start login again").
  Exchanges via `POST https://accounts.spotify.com/api/token`
  (`grant_type=authorization_code`, code, redirect_uri, Basic
  client auth). Exchange failure → 502 `download_failed`
  (spotify-rust's code) with Spotify's error in the message.
  Success → stores the encrypted token, response
  `{authorized: true, expires_at}`.
- **refresh.** Request `{action: "refresh", user_id}`. No stored
  token → 404 `not_found` "no Spotify token — log in first".
  Success → stores the new token (the refresh token is kept
  unless Spotify returns a new one), response
  `{authorized: true, expires_at}`. Failure → the token is
  **DROPPED** and the response is 401 `unauthorized`
  (`invalid_grant` — the user must log in again). This is the
  one place a token is deleted on failure; it is deliberate
  (a dead refresh token can never recover).
- **revoke.** Request `{action: "revoke", user_id}`. Deletes the
  stored token; response `{authorized: false, was_authorized:
  bool}`. Revoking a non-existent token → 200
  `{authorized: false, was_authorized: false}` (idempotent).
- **Token storage (AES-256-GCM, `crypto.rs` — port verbatim).**
  Hand-rolled AES-256-GCM: 96-bit random nonce per record,
  128-bit tag, AAD = `"spotify-user:<user_id>"` prefix. The
  service key is generated once per run and stored in a file
  `spotify-user-key` (0600, in the data dir) — never hardcoded,
  never in the repo, never logged. Plaintext is the JSON
  `{"access_token", "refresh_token"}` (plus `expires_at` in the
  record). Wrong key / wrong AAD / tampering → the record is
  skipped on load (the user is treated as not logged in, no
  error is raised). A test asserts the raw token never appears
  on disk — Wave 2 keeps that test. In Phase 3 the encrypted
  blob lives in the bbolt `spotify_tokens` bucket (key
  `user:<id>`); the service key file stays a file (0600), not a
  bbolt value.
- **New plan.** `spotify.Auth` → `store` (bbolt) + `crypto`
  (AES-256-GCM, same parameters) → response. The `state` store
  is in-memory with a 10-minute TTL (single-use; a used state
  is deleted on first callback attempt, success or failure).
- **Edge cases.** `user_id` missing → `bad_request`; `callback`
  without `code` → `bad_request`; `refresh`/`revoke` without a
  token → see above (404 / 200-idempotent); the redirect URI
  must match the registered one exactly (Spotify enforces it —
  a mismatch surfaces as a 502 `download_failed` from the
  exchange, with Spotify's message).

### 8.3 `download.progress` — run lifecycle (detail)

- **Lifecycle.** `new_run(kind, title)` at POST entry (before
  any provider work) → `searching` → provider sets
  `downloading` on first bytes (or on first phase transition
  for phase-only providers) → per-track `set_titles` /
  `set_track` for albums/collections (checklist states
  pending → searching → queued → downloading → done/failed)
  → `finish("done")` (forces percentage 100) or
  `finish("failed")` (humanised error, partial file deleted)
  exactly once → retained for late polls. Registry caps at 200
  finished runs, oldest first; live runs are never evicted.
  A backend restart marks any live run `failed` with reason
  "backend restarted" (never stuck at `searching`).
- **Numbers.** yt-dlp progress lines are the only source of
  real percentages/speed/ETA (`util.parse_ytdlp_progress_line`);
  Soulseek reports slskd `progress_cb`/`state_cb`; every other
  provider reports phase transitions only (percentage stays 0
  until the file lands — honest about granularity). `total_bytes`
  is null until the provider knows it; `eta_sec` is null until
  a speed is measurable; `speed_bytes_per_sec` is 0 until bytes
  move. `lossy_available` / `lossy_summary` are set only by the
  Soulseek path (§2.7). `phase: uploading` is never set by the
  old code and is set by the backend only when it actually
  uploads (OD-3 async delivery, §9-E1).
- **Core use.** The core polls `download.progress` only AFTER
  the synchronous POST returns (the run_id is only known from
  the POST response — live polling during the transfer is
  impossible in the old design). After the POST returns, the
  core fetches the final snapshot for the album checklist
  (collapses above 28 tracks) and the lossy prompt. `oversize:
  true` on the response → "over the 50 MB send cap — queued
  for a later send" (not a failure). `dlrun:<run_id>` =
  `<file_path>\n<chat_id>` in the core store; `dlsrv:<run_id>`
  is the server-side run id.
- **New plan.** `download.Registry` (in-memory + bbolt-backed
  run records, §4.6) → snapshot. In Phase 3 the POST stays
  synchronous (the run_id is still only known after the POST
  returns) UNLESS §9-E1's async mode is implemented — that is
  the one OD-3 change that makes live polling possible.

---

## §9. OD-3 designs — contract extensions (decided: IMPLEMENT all, no cuts)

OD-3 index: routing-table §E (E1–E11) + the replied-media action
(E12, designed in §3.3). Each entry gives the proposed action
name, request/response fields, behaviour, and the existing action
it relates to. Wave 2 adds these to the UDS dispatch and updates
the routing table (new rows E1–E12). No new action is invented
beyond what is listed here.

### E1 — `download.cancel` (from `/download` cancel button; routing A78)

- **Request:** `{run_id: string}`.
- **Response:** `{cancelled: boolean, run_id: string, phase:
  "done"|"failed"|"cancelled"}` (200 always when the run_id is
  known).
- **Behaviour.** Cancels an in-flight run via context cancel
  (the provider's download loop checks the context between
  chunks / between tracks); deletes the partial file; the run
  finishes `failed` with reason "cancelled by user" (so
  `download.progress` shows it consistently). Album/collection
  runs cancel between tracks (a track mid-transfer finishes
  its chunk, then stops — never a torn file). **Async
  precondition:** live cancellation is only meaningful once a
  run can be polled while it is running — today the run_id is
  only known after the POST returns (§8.3). So `download.cancel`
  ships with the async download mode: `download.*` POSTs gain
  an optional `async: true` flag → the backend returns 202
  `{run_id, phase: "searching"}` immediately and the transfer
  runs in the background; the core polls `download.progress`
  and offers the cancel button (`pl:dl:cancel:<run_id>`,
  routing A78) while `phase` is `searching|downloading`.
  Without `async`, the old synchronous behaviour is unchanged
  and `download.cancel` on a finished run answers
  `{cancelled: false, phase: "done"|"failed"}` (200, not an
  error — this is the core's "Nothing to cancel — already
  finished." toast path, dispatcher.cpp `cbDl`).
- **Relates to:** `download.progress` (§2.8), all `download.*`
  POSTs (§2.1–§2.7).
- **Edge cases.** Unknown run_id → 404 `not_found`; a run that
  already finished → `{cancelled: false, ...}` (200); a run in
  `uploading` (OD-3 delivery) → cancel aborts the upload too.

### E2 — `library.delete` (from `pl:del:yes:<id>` and the
`delete:<path>` text trigger; routing A75–A76, C96)

- **Request:** `{id: string, path?: string}` — `id` is the
  library track id; `path` is the filesystem path when the core
  is deleting by path (the `delete:<path>` trigger).
- **Response:** `{deleted: boolean, id: string}`.
- **Behaviour.** Deletes the library track record (bbolt
  `library` bucket) AND the file on disk (when the record has
  one and the path is under the data dir — same `sanitize_path`
  rule as §3.3; `..` → `bad_request`). The core's confirm flow
  is unchanged: `delete:<path>` → confirm card → `pl:del:yes`
  → this action → "Deleted." / `pl:del:no` → "Cancelled." +
  message deleted. The old code answered the yes-button with
  an unavailable alert (dispatcher.cpp `cbDel`, 7528) — this
  action is what makes it real.
- **Relates to:** `search.inline` `del` mode (§7.10),
  `library.scan` (E5), `cache.clear` (E7).
- **Edge cases.** Unknown id → 404 `not_found`; top-level
  directory as `path` → `bad_request` "Cannot delete top-level
  directories." (the core already guards this, the backend
  re-checks); file already gone but record exists → delete the
  record, return `{deleted: true}` (the record is the source of
  truth); record gone but file exists → delete the file,
  `{deleted: true}`.

### E3 — `mix.generate` (from `/mix`; TgMusicBot §5.5, UI_INVENTORY.md:382)

- **Request:** `{chat_id: ChatId, query?: string,
  requested_by: UserId}`.
- **Response:** `{tracks: [Track], source_track?: Track}`.
- **Behaviour.** Usage: `/mix [song or URL]`, or bare `/mix`
  while a track is playing (mix based on the current track).
  Resolution: explicit query → resolve it (same router as
  `vc.play`, §3.1) → that track is the seed; no query → the
  room's current track is the seed; no query and nothing
  playing → `bad_request` "Nothing is playing — give me a song
  to mix from." Expansion: the YouTube RD-mix picker (§3.2),
  run repeatedly to collect up to 10 distinct tracks (exclude
  = seed + room queue + history + already-picked); each pick is
  a mix fetch, not a single call (the mix is a flat playlist,
  so one fetch usually yields the 10). The response is a track
  LIST — the core enqueues them (`vc.queue add` per track) and
  reports "All mix tracks are already in the queue or playing."
  when the filtered list is empty. Strings (locked, from the
  inventory): "Fetching mix tracks...", "Failed to get mix:
  Please try again later.", "No tracks found for mix."
- **Relates to:** `vc.autoplay_skip` (§1.11 — same RD-mix
  picker, one track), `vc.queue` add (§1.7).
- **Edge cases.** Mix fetch fails → 502 `engine_unavailable`
  "Failed to get mix: Please try again later."; seed is not a
  YouTube track → 502 (only YouTube has mixes — same limit as
  autoplay); every candidate excluded → 200 `tracks: []` (the
  core renders the "already in the queue" string).

### E4 — `audio.recognize` (from `/recognize`; Music163bot voice recognition, UI_INVENTORY.md:461)

- **Request:** `{chat_id: ChatId, file_path?: string,
  file_id?: string, message_id?: integer, requested_by: UserId}`.
- **Response:** `{recognized: boolean, track?: Track, name?:
  string, artists?: [string], album?: string, source?:
  string}`.
- **Behaviour.** The core sends the voice-message file it has
  already downloaded (same pattern as §3.3 — a path, not bytes;
  the core's `/recognize` handler requires the user to reply to
  a voice message first). The backend fingerprints the audio
  and matches it. Strings (locked, from the inventory):
  no-voice "Please send a voice message containing a music clip
  first, then reply to it with /recognize." (core-side, before
  this action is called); status "Recognizing music, please
  wait..."; failures "Can't read this voice message right
  now…", "No song recognized yet…", "Music recognition failed
  temporarily…"; success "Recognized 「{name}」- {artists},
  Album: {album}" + an Open-in-NetEase button (when the match
  has a NetEase id — `netease.search` for the name, best effort).
  A recognised track is returned as a `Track` so the core can
  offer Play/Download on it.
- **Relates to:** `netease.search` (§5.1), `vc.play_file`
  (§3.3 — same file-path pattern).
- **Edge cases.** No file → `bad_request`; unreadable file →
  502 `engine_unavailable` "Can't read this voice message right
  now…"; no match → 200 `{recognized: false}` (NOT an error —
  the core renders "No song recognized yet…"); provider error
  → 502 `engine_unavailable` "Music recognition failed
  temporarily…".
- **DECIDED by the user 2026-11-03 ~16:57 IST — AudD (free tier,
  simple API).** `audio.recognize` is built against AudD's HTTP
  API, env-gated on `AUDD_API_TOKEN`, and answers an honest 502
  while unconfigured (`engine_unavailable` "Music recognition is
  not configured", rendered by the core as the temporary-failure
  string). No fake matches, no invented provider: until the token
  is set, the action stays 502. (Was OPEN — provider not
  determined, §10-U4; the user ruled AudD over ACRCloud and
  self-hosted fingerprinting.)

### E5 — `library.scan` (from `/scan`; navidrome §7.5, UI_INVENTORY.md:459)

- **Request:** `{requested_by: UserId}`.
- **Response:** `{triggered: boolean, tracks_found?: integer,
  error?: string}`.
- **Behaviour.** Triggers a Navidrome library rescan (Subsonic
  `startScan`) and returns immediately (`triggered: true` —
  the scan itself is async on the Navidrome side; the backend
  does not wait for it). After the scan, the backend refreshes
  its `library` index from the Subsonic library (the same
  client as `download.navidrome`, §2.4). Strings (locked):
  "Library scan triggered." / "Library scan not configured
  (NAVIDROME_USER/NAVIDROME_PASS not set)." / "Library scan
  failed: wrong credentials." / "Library scan failed."
- **Relates to:** `download.navidrome` (§2.4), `library.delete`
  (E2).
- **Edge cases.** Navidrome env unset → 200 `{triggered: false,
  error: "not configured"}` (not an error — the core renders
  the not-configured string); wrong credentials → 200
  `{triggered: false, error: "wrong credentials"}`; server
  unreachable → 502 `engine_unavailable` "Library scan failed."

### E6 — `library.sharescan` (from `/sharescan`; slskd, UI_INVENTORY.md:459)

- **Request:** `{requested_by: UserId}`.
- **Response:** `{triggered: boolean, scheduled_in_min?:
  integer, error?: string}`.
- **Behaviour.** Triggers (or schedules) an slskd share scan
  (`POST /api/v0/options/shares/scan` — the slskd shares-scan
  endpoint; if slskd reports the scan is already queued, the
  response carries `scheduled_in_min`). Strings (locked):
  "slskd share scan triggered." / "slskd share scan scheduled
  in ~{mins}m."
- **Relates to:** `download.soulseek` (§2.7 — same slskd
  client).
- **Edge cases.** slskd down → 502 `engine_unavailable`
  naming `SLSKD_URL`; scan already running → 200
  `{triggered: false, scheduled_in_min: <n>}`.

### E7 — `cache.clear` (from `/rmcache`; Music163bot §6.9, UI_INVENTORY.md:462)

- **Request:** `{scope?: "track"|"all", name?: string,
  requested_by: UserId}`.
- **Response:** `{cleared: boolean, names: [string]}`.
- **Behaviour.** Clears per-track cache entries in the embedded
  store (`library` bucket — the cached-file records that back
  the inline `Song not cached` marker, §9-E10): `scope: "track"`
  + `name` → delete that entry; `scope: "all"` (or absent) →
  delete all cache entries. Strings (locked): "Song not
  cached" (when the named entry does not exist — 200,
  `{cleared: false, names: []}`, not an error) / "Cleared
  [{name}] cache successfully". **Boundary:** this is the
  embedded-store cache only — the URL cache is Phase 4's, and
  this action must not touch it (record the boundary; Phase 4
  owns URL-cache invalidation).
- **Relates to:** `library.delete` (E2), `search.inline`
  (§7.10).
- **Edge cases.** Unknown name → 200 `{cleared: false}` (the
  "Song not cached" string); `scope` other than track|all →
  `bad_request`.

### E8 — Spotify companion surfaces (`/details`, `/analyze`) — NO new actions needed

- **Finding.** `spotify.details` (§6.10) and `spotify.analyze`
  (§6.11) already exist and fully cover the `/details` and
  `/analyze` companion surfaces. The gap is **core routing
  only**: `cbSpotify` (dispatcher.cpp:7748) answers "not
  available" for every `pl:spotify:<verb>` except `dislike`
  (routing A94). Wave 2 core routes `pl:spotify:details:<id>`
  → `spotify.details` and `pl:spotify:analyze:<id>` →
  `spotify.analyze` (and the Hide-alerts button, if it lands,
  to the proposed `spotify.ignore` below — not a decided action).
- **Known honest nulls in `spotify.details` (§9-D6):**
  `language` is always null (no detection exists);
  `ignored_by` is always 0 in the old code (no ignore store).
  Wave 2 returns `ignored_by: []` (contract shape) and
  `language: null` until a detector exists — never a faked value.
- **Proposed (not decided) — `spotify.ignore`.** If the
  Hide-alerts button needs a backend action: request
  `{action: add|remove, user_id, track_id}` → `{ignored:
  boolean}`. It would write a per-user ignore set in the
  `spotify_state` bucket, read by `spotify.details`
  (`ignored_by`) and by the watch poller (an ignored track is
  never skip-checked). This is a proposal for the user/Wave 2
  to confirm, not a locked action — do not add it to the
  dispatch until confirmed.

### E9 — Download prefs contract (`download.prefs`; from `cbDlset`, dispatcher.cpp:7647)

- **Request (GET):** `{user_id}`. **Request (POST):**
  `{user_id, format?: "mp3"|"m4a"|"flac"|"opus"|"ogg"|"wav",
  use_youtube?: boolean, path_template?: string}`.
- **Response:** `{user_id, format, use_youtube,
  path_template}`.
- **Behaviour.** Per-user download preferences, today core-local
  in store key `dlset:<user>` = `"<format>\n<youtube 0|1>\n
  <template>"` (format default `mp3`; use-YouTube default true;
  template via pending state `dlset_tpl_pending:<user>` + the
  user's next DM text). In Phase 3 the backend owns them
  (bbolt `dlprefs` bucket, key `dlpref:<user_id>`); the core's
  `/dlset` panel reads/writes through this action. The
  `download.*` handlers apply them: `format` = the output
  container (yt-dlp `--audio-format`, spotdl `--output-format`);
  `use_youtube` = whether the Spotify resolution path may fall
  back to YouTube audio (false = Spotify metadata only, no
  YouTube match — the download then fails honestly when no
  spotdl/Spotify audio exists); `path_template` = the output
  filename template (spotdl already uses
  `{artist} - {title}.{ext}`; the template is applied to
  yt-dlp's `--output` template too, sanitised, ≤150 chars).
- **Relates to:** all `download.*` POSTs (§2.1–§2.7).
- **Edge cases.** Unknown format → `bad_request` naming the six
  formats; GET for a fresh user → defaults (200, never 404);
  OD-2 fresh start: prefs start at defaults (the core's old
  `dlset:<user>` values are NOT migrated).

### E10 — Inline `Song not cached` (Track extension, no new action)

- **Finding.** The core's inline handler needs to know whether
  a track has a cached file to answer the `Song not cached`
  article. Today a `Track` carries NO `file_id` and NO size, so
  the core cannot know (§7.10). The fix is a **Track type
  extension in `ipc.json`** (Wave 2 contract change), not a new
  action: add optional fields `cached_file_id?: string` and
  `size_bytes?: integer` to `Track`. The backend fills them from
  the `library` bucket whenever it returns a track that has a
  cached file (search results, now-playing, queue items,
  share-resolved tracks). The core answers the `Song not
  cached` article when a result has no `cached_file_id`.
- **Relates to:** `search.inline` (§7.10), `library.delete`
  (E2), `cache.clear` (E7).
- **Edge cases.** A cached file deleted on disk but still in the
  index → the backend must verify the file exists before
  setting `cached_file_id` (stale entries are dropped lazily on
  read); `size_bytes` is the file size, not the stream size.

### E11 — Inline `np` oversize (Track extension, no new action)

- **Finding.** Same Track extension as E10 (`size_bytes`): the
  core's `np` inline mode needs the track size to answer the
  oversize article/warning for a now-playing track over the
  sendable limit. The threshold question is open for Wave 2:
  the old limit is the 50 MB Bot API cap (`MAX_BOT_API_BYTES`);
  the new architecture's MTProto limit is 2 GB (FINAL §6-R8).
  Record the decision as **Wave 2 / user to confirm** — the
  backend reports the honest `size_bytes` either way; the core
  applies whichever threshold is decided. Do not hardcode a
  threshold in the backend.
- **Relates to:** `search.inline` `np` mode (§7.10),
  `vc.now_playing` (§1.13).

### E12 — `vc.play_file` (replied-media playback; designed in §3.3)

- **Request:** `{chat_id, file_path, file_id?, title?, kind?,
  duration_sec?, requested_by}`.
- **Response:** `{now_playing: NowPlaying}`.
- **Behaviour:** §3.3 (path validation under the data dir,
  Track `{id: "file:<…>", source: "file"}`, play-now-if-idle
  else enqueue, DSP decodes the local file in Phase 5, Phase 3
  records it as current with an empty `stream_url` and an
  honest `voiceState`).
- **Relates to:** `vc.play` (§1.1), `audio.recognize` (E4 —
  same file-path pattern).
- **Edge cases.** §3.3 (missing file → `not_found`; `..` →
  `bad_request`; duration 0 → never auto-advances).

**OD-3 count (honest):** 12 surfaces designed. New actions
proposed: **8** — `download.cancel`, `library.delete`,
`mix.generate`, `audio.recognize`, `library.scan`,
`library.sharescan`, `cache.clear`, `download.prefs`, plus
`vc.play_file` (E12) = **9 new actions**. Covered by existing
actions (no new action): E8 (`spotify.details`/`spotify.analyze`
+ core routing only), E10/E11 (Track extension, no new action).
E4's recognition provider was decided by the user 2026-11-03
~16:57 IST — AudD (OD-10, §10-U4); no longer blocked.

---

## §10. Divergences, undetermined items, and open questions

### Divergences (old code disagrees — the locked Phase 3 rule is stated)

| # | Divergence | Locked rule |
|---|---|---|
| D1 | `vc.play` / `vc.queue` / `radio.*` / `spotify.*` responses are supersets of `ipc.json` (extra fields: `queued`, `resolved_by`, `vc_handoff`, `now_playing`, `track_id`, `total`, `user_tokens`, `watching`, `gemini_configured`, `poll_interval_secs`, `polling`, `station_name`, `listeners`, `loop_type`, `autoplay_enabled`, `queue_length`, `api_error`, `note`, `lrc_path`, `auth`) | Keep the superset fields (the core reads several of them). Wave 2 updates `ipc.json` to document them; never silently drop a field the core reads. |
| D2 | `vc.panel` GET returns `{panel}` (vc-ts), contract says `{now_playing}` | Return both keys (§1.15). |
| D3 | `/loop`: engines implement native `vc.loop`; the core queues N copies and never calls it | Wave 2 core switches `/loop` to `vc.loop`; the copy-queueing path is deleted (§1.8). |
| D4 | Queue positions: engines are 0-based waiting-only; the core treats index 0 as the current track and passes 1-based display numbers straight through (`onRemove`, `onMove`, `onLoop`-off, `onRestore`) | Backend contract locked: 0-based waiting-only. Wave 2 core subtracts 1 and stops skipping items[0] (§1.7). |
| D5 | `vc.now_playing` on unknown chat: vc-go/vc-ts 404, vc-rust 200-empty. Pause/resume/skip: vc-go/vc-ts 404, vc-rust 400. `vc.progress` queued-only room: vc-go 200 `now: null`, vc-ts/vc-rust 404. | Unify on vc-go/vc-ts 404 for now-playing/pause/resume/skip; vc-go 200-with-`now: null` for queued-only `vc.progress` (§1.13–§1.14). |
| D6 | `spotify.details`: `disliked_by` is an integer count and `ignored_by: 0` in the old code; contract says both are `[UserId]` arrays. `language` always null. | Contract shape wins: arrays; `ignored_by: []` until `spotify.ignore` exists; `language: null` (§6.10, §9-E8). |
| D7 | `cbSpotify` answers "not available" for every `pl:spotify:<verb>` except `dislike` | Core routing fix only (§9-E8); no new backend action. |
| D8 | `radio.stop` returns `{vc_cleared}` (radio-js), contract says `{stopped}` | Return both keys (§7.3). |
| D9 | `search.query` `source` enum lacks `jiosaavn`, though vc-ts serves it | Add `jiosaavn` to the enum in Wave 2 (§7.9). |
| D10 | `vc.lyrics` is GET in `ipc.json`, POST-only in vc-ts and in the core's call | POST (FINAL §6-R5, §1.16). |
| D11 | Autoplay history cap: 50 (vc-go/vc-ts) vs 200 with shift (vc-rust) | 50, clear + stop autoplay at the cap (§4.3). |
| D12 | `vc.stop` resets autoplay in vc-go/vc-ts but not in vc-rust | Reset (vc-go rule, §1.5). |
| D13 | `vc.play` body carries extra `video`/`force` fields the contract does not list | Backend ignores them; Wave 2 core drops them from the payload (§1.1). |
| D14 | `/download` routes only Spotify links to `download.spotify*` and everything else to `download.youtube`; Deezer/TikTok/Navidrome/Soulseek links never reach their actions from `/download` | Wave 2 core routes by link type in `onDownload` (§3.1 row 7); the backend actions already exist. |
| D15 | `vc.volume`: vc-ts 400s out-of-range, vc-go clamps | Clamp (§1.12). |
| D16 | `fx.apply` Phase 3 is record-only (no ffmpeg) while the core's `cbDl` sends the returned path as processed audio | Wave 2 core must honour `applied: false` and not present the file as processed (§7.8). |

### Undetermined items (could not be determined from the old code)

- **U1 — `vc.mute` old HTTP mapping.** `engine_router.cpp` was deleted in Phase 2 and no engine implements `/v1/mute`. The old route cannot be reconstructed; the new action is designed from the contract + the volume-0 mute model (§1.18).
- **U2 — `vc.join` old implementation.** `voice_call.cpp` (core-cpp `TdLibVoiceCall`) was deleted in Phase 2. The old behaviour is reconstructed from `ipc.json` + the delegation in vc-ts/vc-rust `join()` + `Dispatcher::joinVoice` (§1.17).
- **U3 — `chat.settings` `autoplay_lang` / `autoplay_mood` defaults.** No value exists anywhere in-tree (only the inventory strings `autoplay_lang_set` / `autoplay_mood_set` in phase9/inventory-group-a.md:1309–1310). Proposed default `""` (any language / any mood) — Wave 2 / user to confirm (§8.1).
- **U4 — `/recognize` provider.** DECIDED by the user 2026-11-03 ~16:57 IST — **AudD (free tier, simple API)**; `audio.recognize` is built against AudD's HTTP API, env-gated on `AUDD_API_TOKEN`, honest 502 while unconfigured (OD-10). No recognition code existed in any service before Wave 2; the action shape is fully specified (§9-E4).

### Open questions for Wave 2 / the user (not blocking Phase 3)

1. `np` oversize threshold: 50 MB (old Bot API cap) or 2 GB (MTProto)? (§9-E11.)
2. `spotify.ignore` — confirm or drop (§9-E8).
3. `autoplay_lang` / `autoplay_mood` defaults (§10-U3).
4. ~~`/recognize` provider (§10-U4)~~ — DECIDED 2026-11-03 ~16:57 IST: AudD (OD-10).
5. `vc.queue` `move` verb — add as a contract extension, or keep the core's list→remove→re-add (§4.2)?
6. `speed`/`bass` room fields: which call carries the core's chat fields into the room on `vc.play` — `chat.settings` read, or a room-fields extension (§4.4)?
7. `ntg_time` as the position-clock source vs wall-clock (§3.4).

---

## §11. Wave 2 checklist (implementation order)

1. **UDS + dispatch** (`internal/uds`): frame codec, action table with deadlines (§0), error envelope, event emission. Unknown action → `not_found`.
2. **Store** (`internal/store`): bbolt buckets (§4.6); fresh start; service-key file (0600) for Spotify tokens.
3. **Room** (`internal/room`): state machine §4; 1 s advance ticker (interim, flagged for Phase 5 removal); restored rooms paused.
4. **Resolver** (`internal/resolver`): source router §3.1; InnerTube (primary) + warm yt-dlp daemon fallback; JioSaavn proxy (320_KBPS); Lemonic; Spotify/Deezer/NetEase/Navidrome/Soulseek/TikTok lanes; RD-mix picker.
5. **Voice** (`internal/voice`): ntgcalls cgo binding; JoinRequest handshake (§3.4); `vc.join`/`vc.mute` as designed (§1.17–§1.18).
6. **The 56 actions** (§1, §2, §5, §6, §7): implement in the order vc playback → downloads → NetEase → Spotify → radio/share/vote/FX/search/gui. Keep every superset field (§10-D1). Fix the contract where it is wrong: `vc.lyrics` POST, `search.query` + `jiosaavn`, `vc.panel` both keys, `radio.stop` both keys, `spotify.details` arrays, Track `cached_file_id`/`size_bytes` (§9-E10/E11).
7. **OD-3 actions** (§9): the 9 new actions + the async download mode (E1) + the Track extension (E10/E11). E4 answers 502-not-configured until a provider is chosen.
8. **Core-side fixes this inventory requires (Wave 2 core work, not backend):** `/loop` → `vc.loop` (§10-D3); queue display N → backend N−1 (§10-D4); `cbDl` FX honours `applied: false` (§10-D16); `cbSpotify` routes details/analyze (§9-E8); `/download` routes Deezer/TikTok/Navidrome/Soulseek links (§10-D14); `/play` replied media → `vc.play_file` (§3.3); `speed`/`bass` fields into the room (§4.4); drop `video`/`force` from the `vc.play` payload (§10-D13).
9. **Tests (Wave 2 must add, mirroring the old suites):** room state machine (loop/shuffle/autoplay/advance, §4.3); queue 0-based semantics (§1.7); resolver routing table (§3.1); Spotify auth round-trip + token-never-on-disk (§8.2); download run lifecycle + 200-run cap (§8.3); vote threshold (§7.7); radio handoff never fails the start (§7.2); `fx.apply` record-only (§7.8); `chat.settings` 15 keys + alias (§8.1); OD-3 actions (§9).
10. **Benchmarks (Phase 3 exit, FINAL §8):** UDS round-trip ≤500 µs p50; resolve ≤1 s p50 per source (InnerTube/JioSaavn targets from Phase 1); kill −9 + restart preserves queue/state; no subprocess per call on the primary path (yt-dlp daemon is the single exception, warm in RAM).

---

*End of inventory. 56 actions inventoried (§1, §2, §5, §6, §7, §8.1), 9 new OD-3 actions proposed (§9, E1–E7 + E9 + E12), 3 surfaces covered by existing actions/extensions (E8, E10, E11). Undetermined: U1 (`vc.mute` old mapping), U2 (`vc.join` old implementation), U3 (`autoplay_lang`/`autoplay_mood` defaults). U4 (`/recognize` provider) is DECIDED — AudD (OD-10, 2026-11-03 ~16:57 IST).*
