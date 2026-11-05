# Parlay UDS Protocol — core-cpp ↔ backend-go wire spec

Status: **Phase 2, implemented** (core side: `services/core-cpp/src/ipc.*`;
test peer: `services/core-cpp/tests/stub_backend.cpp`).
Replaces the HTTP wire protocol described in `contracts/ipc.json`
(live contract since Phase 4 — the 66 actions and their payload shapes,
retargeted to the unified backend; only the transport changes). `ipc.json` stays the
authority for request/response field shapes; this document is the
authority for how those shapes move over the wire.

Transport decision: **plain Unix Domain Socket frames (OD-4: UDS,
not gRPC-over-UDS)** — one persistent duplex socket, length-prefixed
binary frames carrying JSON envelopes. No HTTP, no per-call subprocess,
no `/healthz` polling, no `X-Parlay-Token` header (see §7).

## §1. Transport

- Socket: `AF_UNIX`, `SOCK_STREAM`, one connection per core process.
  Path: `<PARLAY_DATA_DIR>/parlay-backend.sock` by default
  (`data/parlay-backend.sock` when unset), overridable with the
  `PARLAY_UDS_PATH` env var until the config rewrite adds an
  `ipc_socket_path` key (Worker C, Phase 2). The Go backend creates
  the socket and listens; the core connects.
- Exactly one connection. If it drops, the core reconnects with
  exponential backoff (100, 200, 400, 800, 1600, 3200, 5000 ms,
  bounded by the in-flight call's deadline). A call whose deadline
  expires, or whose write fails, tears the connection down — a peer
  that stops answering is not trusted again — and the next call
  reconnects.
- Concurrency: request/response multiplexing by `request_id`.
  Many calls may be in flight on the one socket at once; responses
  may arrive in any order. The backend may also initiate traffic on
  the same socket at any time (§5, §6).

## §2. Frame format

```
+------------------+---------------------------+
| length (4 bytes) | payload (length bytes)    |
+------------------+---------------------------+
```

- Length: **uint32, little-endian**, the payload byte count.
- Payload: UTF-8 JSON (one envelope object, §3–§6).
- Max frame: **16 MiB** (16,777,216 bytes). A length prefix of 0 or
  > 16 MiB is a protocol error: the reader drops the frame (and, on
  the core side, drops the connection). 16 MiB is far above any
  envelope (the largest is a `download.progress` snapshot) and
  bounds a corrupt or hostile length prefix.
- Frames are written atomically per `send()` loop with `MSG_NOSIGNAL`
  (no partial frames on the wire; a short write is retried, an error
  fails the call). Readers read exactly 4 bytes, then exactly
  `length` bytes.

Reference implementation: `parlay::uds::encodeFrame` /
`writeFrame` / `readFrame` / `readFrameUntil` in
`services/core-cpp/src/ipc.hpp` — the stub backend uses the same
functions, so the two sides cannot drift.

## §3. Message envelope

Every frame's JSON payload has a `"type"` field. Four types:

### §3.1 Request (core → backend)

```json
{
  "type": "request",
  "request_id": 42,
  "action": "vc.play",
  "chat_id": -1001234567890,
  "user_id": 111222333,
  "deadline_ms": 30000,
  "payload": { "chat_id": -1001234567890, "query": "…", "requested_by": 111222333 }
}
```

| Field | Type | Notes |
|---|---|---|
| `type` | string | Always `"request"`. |
| `request_id` | int64 | Monotonic per core process, starting at 1. Unique per in-flight call; the response echoes it. |
| `action` | string | Dotted contract action (§4). |
| `chat_id` | int64, optional | Promoted from `payload.chat_id` when present, so the backend can route without parsing the payload. Absent for chat-less actions (`spotify.status`, `spotify.watches`). |
| `user_id` | int64, optional | Promoted from `payload.user_id`, else `requested_by`, else `voter_id`, else `created_by`, when present. |
| `deadline_ms` | int | The core's per-action deadline (§8). The backend SHOULD abandon work past it; the core will have already failed the call and ignored a late response. |
| `payload` | object | The action's request body, exactly as `contracts/ipc.json` defines it. |

### §3.2 Response (backend → core)

```json
{ "type": "response", "request_id": 42, "ok": true, "data": { "now_playing": { … } } }
{ "type": "response", "request_id": 42, "ok": false,
  "error": { "code": "not_found", "message": "unknown action vc.bogus" } }
```

- `ok: true` → `data` holds the action's response object from
  `contracts/ipc.json`. `data` may be `{}` (e.g. `vc.stop`,
  `spotify.watch`).
- `ok: false` → `error: {code, message}` with a code from §8.
- A response whose `request_id` matches no in-flight call (late,
  duplicate, or unsolicited) is ignored by the core.
- The core maps error codes to the legacy `http_status` for the
  dispatcher/router (`not_found`→404, `unauthorized`→401,
  `rate_limited`→429, `engine_unavailable`→503, `bad_request`→400,
  anything else→500; transport failure→0).

### §3.3 Event (backend → core)

```json
{ "type": "event", "event": "TrackStarted",
  "chat_id": -1001234567890,
  "data": { "track": { … }, "chat_id": -1001234567890 } }
```

No `request_id` — events are unsolicited. The core dispatches them
to the registered event handler (§5). Unknown event names are
ignored (forward compatibility).

### §3.4 JoinRequest (backend → core)

```json
{ "type": "join_request", "request_id": 900001,
  "chat_id": -1001234567890,
  "local_sdp": "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=…\r\n…" }
```

The backend initiates a voice join with its local SDP offer; the
core answers with a **response** frame (§3.2) carrying the same
`request_id` — `ok: true, data: {remote_sdp, group_call_id}` on
success, `ok: false, error: {code: "internal", …}` when no handler
is registered or the join fails. The backend's `request_id`
sequence for join requests is independent of the core's (the stub
uses 1,000,000+). The core runs the handler on a worker thread, so
a slow TDLib `joinVideoChat` never stalls the reader or other
in-flight responses. See §6.

## §4. Action table — all 68 contract actions

Payload/response shapes are exactly `contracts/ipc.json`
`actions/<name>` (request / response objects, `Track` /
`NowPlaying` / `QueueItem` types). This table maps each action onto
the UDS envelope and records the core-side legacy name it replaces
(the core translates legacy slash-form names — `search`,
`download/youtube`, `radio/start`, `share`, `fx`, `vote`,
`dislike`, vc short names — to these dotted names via
`IpcClient::canonicalAction`; the shim still exists, because the
dispatcher still posts legacy names).

Rows 57–65 are the nine OD-3 actions (Phase 3, Wave 2): they had
no IPC action before (their buttons answered "unavailable"). Rows
13–14 and 23–24 carry backend superset fields added in Wave 2
(`run_id` / `size_bytes` on `lemonic.download`, `via` on
`lemonic.search`, `mode` on `search.inline`, `source_status` on
`search.query`) — the core ignores unknown fields, so old readers
keep working.

| # | Action | Request (payload) | Response (data) | Deadline |
|---|---|---|---|---|
| 1 | `chat.settings` | `chat_id`; POST only: `key`, `value` | `chat_id`, `settings` (21 keys) | 10 s |
| 2 | `download.deezer` | `query`, `requested_by` | `file_path`, `track`, `run_id` | 30 s* |
| 3 | `download.navidrome` | `url`, `requested_by` | `imported`, `track`, `run_id` | 30 s* |
| 4 | `download.progress` | `run_id` | `run_id`, `percentage`, `done_bytes`, `total_bytes`, `speed_bytes_per_sec`, `eta_sec`, `elapsed_sec`, `phase`, `per_track`, `lossy_available`, `lossy_summary` | 10 s |
| 5 | `download.soulseek` | `query`, `artist`, `title`, `duration_sec`, `accept_lossy`, `album`, `requested_by` | `file_path`, `track`, `oversize`, `size_bytes`, `peer`, `format`, `count`, `total`, `failed`, `album_dir`, `source`, `run_id` | 30 s* |
| 6 | `download.spotify` | `url`, `requested_by` | `file_path`, `track`, `run_id` | 30 s* |
| 7 | `download.spotify_playlist` | `mode` (zip\|files), `url`, `requested_by` | `count`, `file_path`, `file_paths`, `run_id` | 30 s* |
| 8 | `download.tiktok` | `url`, `requested_by` | `file_path`, `track`, `run_id` | 30 s* |
| 9 | `download.youtube` | `query`, `requested_by` | `file_path`, `oversize`, `size_bytes`, `track`, `run_id` | 30 s* |
| 10 | `fx.apply` | `effect`, `input_path`, `requested_by` | `output_path` | 30 s |
| 11 | `gui.now_playing` | `chat_id` | `now_playing` | 10 s |
| 12 | `gui.queue` | `chat_id` | `queue` ([QueueItem]) | 10 s |
| 13 | `lemonic.download` | `track_id` | `file_path`, `track`, `size_bytes`, `run_id` | 30 s |
| 14 | `lemonic.search` | `query` | `results` ([Track]), `via` | 30 s |
| 15 | `netease.download` | `track_id` | `file_path`, `track`, `auth` | 30 s |
| 16 | `netease.lyrics` | `track_id` | `lyrics`, `track_id`, `title`, `artist`, `source`, `truncated`, `lrc_path` | 30 s |
| 17 | `netease.search` | `query` | `results` ([Track]) | 30 s |
| 18 | `queue.vote` | `track`, `vote` (approve\|decline), `voter_id` | `queued` | 30 s |
| 19 | `radio.list` | — | `stations` ([string]) | 10 s |
| 20 | `radio.start` | `chat_id`, `station` | `stream_url` | 30 s |
| 21 | `radio.status` | `chat_id` | `now_playing`, `station`, `stream_url`, `reachable`, `vc_handoff` | 10 s |
| 22 | `radio.stop` | `chat_id` | `stopped` | 30 s |
| 23 | `search.inline` | `query`, `user_id`, `chat_id` | `results` ([Track] or lib entries with `file_path`, `cached`, `size_bytes`), `mode` | 30 s |
| 24 | `search.query` | `query`, `source`, `limit` | `results` ([Track]), `source_status` | 30 s |
| 25 | `share.create` | `track`, `created_by` | `link`, `token` | 30 s |
| 26 | `share.resolve` | `token` | `track` | 10 s |
| 27 | `spotify.analyze` | `user_id`, `track_id` | `analysis`, `profane_words` | 30 s |
| 28 | `spotify.details` | `user_id`, `track_id` | `track`, `features`, `genres`, `disliked_by`, `ignored_by`, `profanity`, `language`, `lyrics`, `lyrics_link` | 10 s |
| 29 | `spotify.auth` | `action` (login\|callback\|refresh\|revoke), `user_id`, `code` | `login`\|`callback`\|`refresh`\|`revoke` object per `action` | 30 s |
| 30 | `spotify.dislike` | `action` (add\|remove), `track_id`, `user_id` | `disliked` | 30 s |
| 31 | `spotify.now_playing` | `user_id` | `playing`, `user_id`, `track`, `checked_at`, `last_skip`, `last_reason`, `classifier_verdict` | 10 s |
| 32 | `spotify.resolve` | `url` | `kind`, `source`, `track_id`, `url` | 30 s |
| 33 | `spotify.skip_check` | `track` | `reason`, `skip`, `classifier_provider`, `classifier_verdict`, `classifier_confidence`, `classifier_detail` | 30 s |
| 34 | `spotify.watch` | `action` (start\|stop), `user_id` | `{}` | 30 s |
| 35 | `spotify.watches` | — | `watches` ([UserId]) | 10 s |
| 36 | `vc.join` | `chat_id` | `joined`, `now_playing` | 30 s |
| 37 | `vc.lyrics` | `chat_id` | `lyrics`, `source`, `track_id`, `title`, `artist`, `truncated` | 10 s |
| 38 | `vc.mute` | `chat_id`, `muted` | `muted`, `now_playing` | 30 s |
| 39 | `vc.now_playing` | `chat_id` | `now_playing` | 10 s |
| 40 | `vc.progress` | `chat_id` | `position_sec`, `duration_sec`, `is_paused`, `is_muted`, `loop_enabled`, `now`, `next` | 10 s |
| 41 | `vc.panel` | `chat_id` | `now_playing` | 10 s |
| 42 | `vc.pause` | `chat_id` | `now_playing` | 30 s |
| 43 | `vc.play` | `chat_id`, `query`, `requested_by` | `now_playing` | 30 s |
| 44 | `vc.queue` | `action` (add\|remove\|clear\|list), `chat_id`, `position`, `track` | `queue` ([QueueItem]) | 30 s |
| 45 | `vc.radio` | `action` (start\|stop), `chat_id`, `station` | `now_playing` | 30 s |
| 46 | `vc.resume` | `chat_id` | `now_playing` | 30 s |
| 47 | `vc.seek` | `chat_id`, `position_sec` | `now_playing` | 30 s |
| 48 | `vc.skip` | `chat_id` | `now_playing` | 30 s |
| 49 | `vc.stop` | `chat_id` | `{}` | 30 s |
| 50 | `vc.loop` | `chat_id`, `count` (0–10) | `now_playing` | 30 s |
| 51 | `vc.shuffle` | `chat_id`, `enabled` | `now_playing` | 30 s |
| 52 | `vc.autoplay` | `chat_id`, `enabled` | `now_playing` | 30 s |
| 53 | `vc.autoplay_skip` | `chat_id` | `now_playing` | 30 s |
| 54 | `spotify.collection` | `url` | `api`, `kind`, `name`, `tracks`, `truncated` | 30 s |
| 55 | `spotify.status` | — | `api`, `credentials_configured`, `token_cached` | 10 s |
| 56 | `vc.volume` | `chat_id`, `volume` (0–200) | `now_playing` | 30 s |
| 57 | `download.cancel` | `run_id` | `cancelled`, `run_id`, `phase` | 10 s |
| 58 | `library.delete` | `id`, `path` (one required) | `deleted`, `id` | 10 s |
| 59 | `mix.generate` | `chat_id`, `query`, `requested_by` | `tracks` ([Track]), `source_track` | 30 s* |
| 60 | `audio.recognize` | `file_path`, `file_id` | `recognized`, `track`, `name`, `artists`, `album`, `source` | 30 s |
| 61 | `library.scan` | — | `triggered`, `error`, `tracks_found` | 30 s |
| 62 | `library.sharescan` | — | `triggered`, `scheduled_in_min` | 30 s |
| 63 | `cache.clear` | `scope` (track\|all), `name` | `cleared`, `names` ([string]) | 10 s |
| 64 | `download.prefs` | `user_id`; POST only: `format`, `use_youtube`, `path_template` | `user_id`, `format`, `use_youtube`, `path_template` | 10 s |
| 65 | `vc.play_file` | `chat_id`, `file_path`, `file_id`, `title`, `kind`, `duration_sec`, `requested_by` | `now_playing` | 30 s |
| 66 | `quality.get` | `chat_id` | `chat_id`, `effective`, `sources`, `library`, `active_call`, `applies_on` | 10 s |
| 67 | `vc.play_next` | `chat_id`, `track_id` | `now_playing`, `moved`, `track_id` | 30 s |
| 68 | `download.cancel_chat` | `chat_id` | `cancelled`, `chat_id`, `run_ids` | 10 s |
| 69 | `vc.card` | `chat_id`, `track_id`, `message_id` | `now_playing` | 30 s |

\* Download actions: the old HTTP call sites passed an explicit
120 s timeout for `download.*` POSTs. That explicit value still
governs (the deadline table has no `download.*` entry, so the
call-site parameter wins — see §8). The config rewrite should set
a `download_ms` budget instead.

Protocol-level action (NOT one of the 68): `ping`, empty payload,
answers `{service: "backend-go", …}` — the core's `healthz()`.

### §4.1 OD-3 actions (Phase 3, Wave 2)

The nine actions below had no IPC action before Wave 2 (their
buttons answered "unavailable"). Shapes match the handlers in
`services/backend-go/internal/{download,library,search,recognize}`.
All nine fail honestly — a missing credential, a missing file, or
an unconfigured provider is an error code, never fake data.

**`download.cancel` (E1)** — cancels a live run. Request
`{run_id}`. A live run is cancelled via its context: the provider
stops between chunks/tracks, the partial file is deleted by the
job's failure path, and the run finishes `failed` with reason
"cancelled by user". Response `{cancelled: true, run_id, phase:
"failed"}`. A finished run answers `{cancelled: false, run_id,
phase: "done"|"failed"}` (200, not an error — the core's "Nothing
to cancel — already finished." path). Unknown `run_id` →
`not_found`. Note the phase value is `"failed"`, not `"cancelled"`:
the run transitions to failed per the §8.3 run lifecycle.

**`library.delete` (E2)** — deletes a library track record AND the
file on disk (when the record has one and the path is under the
data dir — the `sanitize_path` rule; `..` → `bad_request`).
Request `{id}` or `{path}` (one required; an id that looks like a
path is treated as a path). Unknown id → `not_found`. A top-level
directory as path → `bad_request` ("Cannot delete top-level
directories."). File already gone but record exists → the record
is deleted, `{deleted: true}`. Record gone but file exists → the
file is deleted, `{deleted: true}`. Response `{deleted, id}`.

**`mix.generate` (E3)** — `/mix [song or URL]`, or bare `/mix`
while a track is playing. Request `{chat_id, query?,
requested_by?}`. Resolution: an explicit query is resolved first
(same router as `vc.play`); otherwise the seed is the room's
current track — no query and nothing playing → `bad_request`
("Nothing is playing — give me a song to mix from."). Only
YouTube seeds have mixes: a non-YouTube seed → `502
engine_unavailable` ("Failed to get mix: Please try again later.").
The mix is the YouTube `RD<id>` flat playlist via the warm yt-dlp
daemon, up to 10 tracks, excluding the seed, the room queue, and
the autoplay history (`RoomState.AutoplayHistory`, capped at 100).
Response `{tracks: [Track], source_track: Track}`; zero tracks
after filtering → `200 {tracks: []}`.

**`audio.recognize` (E4)** — music recognition against AudD
(`https://api.audd.io`, POST multipart `api_token` + `file`,
`return=apple_music,spotify,deezer,musicbrainz`). Request
`{file_path, file_id?}`: a local audio path under the data dir
(the core downloaded the replied voice message / audio file via
the TDLib bot session — the same flow as `vc.play_file`). No
token configured (`AUDD_API_TOKEN` empty) → `502
engine_unavailable` "Music recognition is not configured" — the
user has not supplied a token yet; the action stays env-gated and
answers honestly until configured. No file → `bad_request`;
missing file → `not_found`; unreadable file or AudD provider
error → `502` ("Can't read this voice message right now…" /
"Music recognition failed temporarily…"); file over the AudD
10 MB cap → `502`. A match → `{recognized: true, track, name,
artists, album, source}` where the track id is `spotify:<id>` when
AudD returned one (resolvable through the normal Spotify
resolver), else `deezer:<id>`, else the label-only
`audd:<artist> - <title>` (never a fake resolvable id). No match
→ `200 {recognized: false}`. There is no second provider.

**`library.scan` (E5)** — triggers a Navidrome library rescan
(Subsonic `startScan`) and returns immediately (`triggered: true`
— the scan itself is async on the server). Request: none (the
payload is ignored). Navidrome env unset → `200 {triggered:
false, error: "not configured"}` (not an error); wrong
credentials → `200 {triggered: false, error: "wrong
credentials"}`; server unreachable → `502 engine_unavailable`.
On success the backend refreshes its library count from the
server (bounded: one `getAlbumList2` page + one `getAlbum` per
album) and reports it as `tracks_found` when the count succeeds;
a failed count never fails the trigger. Response `{triggered,
error?, tracks_found?}`.

**`library.sharescan` (E6)** — triggers (or schedules) an slskd
share scan (`POST /api/v0/options/shares/scan`, `X-API-Key` when
`SLSKD_API_KEY` is set). Request: none. slskd down → `502
engine_unavailable` naming `SLSKD_URL`. Scan already running
(slskd `409`) → `200 {triggered: false}`; `scheduled_in_min` is
omitted — the slskd API exposes no minute estimate, so none is
invented. Response `{triggered, scheduled_in_min?}`.

**`cache.clear` (E7)** — clears per-track cache entries in the
embedded store (the library bucket — the cached-file records that
back the inline "Song not cached" marker). Request `{scope:
"track"|"all", name?}`. Scope `track` + `name` → delete that
entry (exact id, else exact title, case-insensitive); scope `all`
(or absent) → delete all cache entries. Unknown name → `200
{cleared: false, names: []}` (the "Song not cached" string), not
an error. Scope other than `track|all` → `bad_request`. A
cleared entry's file is deleted with it. Boundary: this is the
embedded-store cache only — the URL cache is Phase 4's, and this
action never touches it. Response `{cleared, names: [string]}`.

**`download.prefs` (E9)** — per-user download preferences.
Request `{user_id}` (GET) or `{user_id, format?,
use_youtube?, path_template?}` (POST — any settable field makes
it a POST). GET returns the stored prefs or the defaults (`200`,
never `404`): format `mp3`, `use_youtube` true, `path_template`
`"{artist} - {title}.{ext}"`. POST validates and stores: unknown
format → `bad_request` naming the six formats
(`mp3|m4a|flac|opus|ogg|wav`); a template must contain `{ext}`,
no separator or `..`, ≤ 150 chars. An empty format/template
coerces to the default. Enforcement: a non-`mp3` format is
refused honestly on daemon-backed downloads (the yt-dlp daemon
extracts mp3 only — a format parameter on the daemon download
method is the fix); `use_youtube: false` refuses Spotify
downloads honestly. The `path_template` rename applies to single
files after download (collisions disambiguated with the track
id). Response `{user_id, format, use_youtube, path_template}`.

**`vc.play_file` (E12)** — replied-media playback. Request
`{chat_id, file_path, file_id?, title?, kind?,
duration_sec?, requested_by?}`. The core already has the file on
disk (downloaded via the TDLib bot session); the action carries a
path, not bytes (16 MiB frame cap). The path must be under the
data dir (`..` rejected — the `sanitize_path` rule); a missing
file → `not_found`; a directory → `bad_request`. `kind` is one
of `audio|video|voice|video_note|document` (voice/video are
accepted as audio sources). The track is `{id:
"file:<file_id or basename-hash>", source: "file"}` and plays
now when the room is idle, else enqueues, exactly like `vc.play`.
Duration 0 never auto-advances. A file over the old 50 MB cap is
fine (MTProto, 2 GB limit). Response `{now_playing}`.
Implementation note (Wave 2 verification): the download package's
feed-binding `vc.play_file` implementation was deleted; the ONLY
implementation is `internal/handlers` (record-only per inventory
§3.3 — no feed bound in Phase 3, the Phase 5 DSP decodes the local
path). The action is registered exactly once, by
`handlers.Register`.

### §4.2 Queue-port Tier 3 actions (2026-11-05)

**`vc.play_next`** — the "Play Now" button on a queued card.
Request `{chat_id, track_id}`. Moves an already-queued track to
the front of the waiting list (list index 1 while a track is
playing, index 0 while idle), so the normal advance path picks it
up next — "play next", not an interrupt. No track is resolved or
added and nothing is interrupted. Response `{now_playing, moved,
track_id}`: `moved` is false when the track was already next or
already playing (a no-op, not an error). Unknown track id in the
queue → `bad_request`; empty room → `not_found` ("nothing playing
in this chat").

**`download.cancel_chat`** — the per-chat download-cancel flag.
Request `{chat_id}`. Cancels every live download run for the chat
via the runs' contexts (the provider loop checks the context
between chunks/tracks; the partial files are deleted by the jobs'
failure paths). Response `{cancelled, chat_id, run_ids}`:
`cancelled` is true when at least one run was live. No live run
is 200 with `cancelled: false` (the core's "Nothing to cancel —
already finished." path). The per-chat form exists because the
core cannot know a run's backend id mid-download — the id is only
revealed in the POST response, after the download — so the
per-run `download.cancel` cannot be the button's mechanism.
Runs record their requesting chat from the POST body's `chat_id`
(the core sends it on `download.*` POSTs).

### §4.3 Queue-port Tier 6 action (2026-11-05)

**`vc.card`** — the core reports a now-playing card's Telegram
message id back to the backend (queue-port Tier 6, item 32 —
NoxBeats `Track::message_id`). Request `{chat_id, track_id,
message_id}`. The room stores the id on the matching track and
persists it with the queue in bbolt, so the id survives a core
restart and `vc.now_playing` surfaces it as
`track.card_message_id`; the core's card refresh prefers that
field, with its local `recordCard` entry as fallback. The core
posts it after sending a fresh card (`sendNowPlayingCard`) and
after re-rendering the recorded card onto a new front track
(`editNowPlayingCard`, `refreshCard`), so the stored id always
belongs to the track the card currently shows. Response
`{now_playing}`. Unknown track id in the queue → `bad_request`;
empty room → `not_found`. The call is fire-and-forget from the
core: a failure is a stderr line, never a card failure and never
an owner error DM.

## §5. Backend-initiated events

The backend pushes these over the same socket whenever they happen
(no polling, no 5-second watcher — the watcher is deleted):

| Event | `data` | When |
|---|---|---|
| `TrackStarted` | `track`, `chat_id` | A track starts playing in a chat. |
| `TrackAdvanced` | `track`, `previous_track_id`, `chat_id` | parlay-dsp crossed a track boundary / EOF; the queue advanced. |
| `TrackEnded` | `track`, `chat_id`, `reason` | A track ended without a successor. |
| `QueueEmpty` | `chat_id` | The queue drained (autoplay decision point). |
| `DownloadReady` | `run_id`, `file_path`, `track`, `chat_id` | An async download finished (Phase 3+; `download.*` POSTs are synchronous until then). |
| `VoiceError` | `chat_id`, `code`, `message` | Voice-call failure the core should surface (e.g. join failed, stream error). |
| `PlayProgress` | `chat_id`, `play_status_id`, `stage`, `title`, `artist`, `duration_str`, `source`, `has_thumbnail` | One vc.play's staged progress (`resolved` / `thumbnail` / `joining`), emitted mid-request so the core can edit that play's status message live. Only when the play body carried `play_status_id`; the core ignores it for a status it no longer tracks. |

Event frames carry `chat_id` at top level as well as in `data`.
The core's event handler runs on the reader thread and must be
fast and non-blocking; unknown event names are ignored.

## §6. Voice join handshake

`vc.join` (core → backend, §4 row 36) is NOT the join itself. The
join is a backend-initiated duplex handshake, because the backend
owns the call (ntgcalls) and the core owns the TDLib assistant
session:

1. Backend decides a call is needed and sends `JoinRequest`
   (§3.4) with its local SDP offer.
2. Core runs `joinVideoChat(local_sdp)` on the assistant session
   (Wave 2 wiring; Phase 2 provides only the handler hook) and
   answers with a response frame: `ok: true, data: {remote_sdp,
   group_call_id}`.
3. Backend completes the handshake with the remote SDP and owns
   the call from then on. `vc.join`'s own response
   (`{joined, now_playing}`) is a separate, ordinary request/response
   pair and may precede or follow the handshake.

If the core has no JoinRequest handler registered (or the handler
throws), it answers `ok: false, error: {code: "internal", message:
"…"}`, and the backend must treat the join as failed — never as
silently joined.

## §7. Authentication

There is no token header on UDS. The old `X-Parlay-Token`
(`PARLAY_TOKEN`) concept does not exist on this transport: the
socket is a local file, and **filesystem permissions are the access
control**. The backend must create the socket with restrictive
permissions (owner-only, `0600`, in a `0700` data directory — the
same convention as the TDLib session dir). Any local process that
can open the socket can issue any action; that is by design, and
the deployment must not expose the socket path outside the host.

## §8. Deadlines and error codes

### Deadlines

Per-action budgets, in milliseconds, from the core's deadline
table (defaults = the old hardcoded 30/10/5 s values, so the
transport swap changes no timing). Resolution order for a call:
(1) the table entry for the action, when set; (2) the legacy
`timeout_sec` call-site parameter (download calls pass 120 s,
search 15–20 s, netease 20 s, reads 10 s). The table is the source
of truth once the config rewrite (Worker C) populates it from
`parlay.toml [timeouts]` (`resolve_ms`, `join_ms`, `ipc_cmd_ms`,
`download_ms`) via `IpcClient::setDeadline()`.

| Class | Default |
|---|---|
| General (all actions not listed below) | 30,000 ms |
| Read-style (fast reads): `vc.now_playing`, `vc.progress`, `vc.panel`, `vc.lyrics`, `radio.list`, `radio.status`, `download.progress`, `share.resolve`, `chat.settings`, `quality.get`, `spotify.details`, `spotify.now_playing`, `spotify.watches`, `spotify.status`, `gui.now_playing`, `gui.queue` | 10,000 ms |
| `ping` (healthz) | 5,000 ms |

On deadline expiry the core fails the call with
`engine_unavailable` / `http_status` 0, drops the connection, and
ignores any late response.

### Error codes

Same set as `contracts/ipc.json` (`bad_request`, `unauthorized`,
`not_found`, `engine_unavailable`, `download_failed`,
`rate_limited`, `internal`), plus `internal` for a missing or
throwing JoinRequest handler. The envelope is unchanged:
`{ok: true, data}` / `{ok: false, error: {code, message}}`.

## §9. Measured performance

Measured 2026-11-03 in the sandbox with `tests/uds_test.cpp`
against `tests/stub_backend.cpp` (200 `ping` round-trips over the
live socket, `IpcClient::healthz`, timed with
`telemetry::Timer` / `steady_clock`):

| Run | mean | median | p95 | min | max |
|---|---|---|---|---|---|
| 1 | 1,038 µs | 500 µs | 1,707 µs | 122 µs | 55,286 µs |
| 2 | 316 µs | 123 µs | 209 µs | 117 µs | 34,861 µs |
| 3 | 145 µs | 142 µs | 169 µs | 135 µs | 331 µs |

Steady-state round-trip is **~120–500 µs (median 123–500 µs)**;
the single-sample max in runs 1–2 is a scheduling spike (first
call / context switch), not transport cost — p95 stays under
~2 ms, and the cleanest run (run 3, no spike) measured
mean 145 µs / median 142 µs / p95 169 µs / max 331 µs. For
comparison, the deleted transport forked one subprocess per call
(tens of ms per call before any server work).
The locked doc's §4 estimate of "UDS ~0.08 ms" was an estimate;
the measured steady-state median of ~0.12 ms is in the same
range, with the JSON parse/serialize and thread hop accounting
for the remainder.

## §10. Notes for the Go backend (Phase 3)

- Read frames exactly as §2 defines; write responses with the
  `request_id` echoed from the request. Never send a response
  without a matching request, except events (§5) and
  `JoinRequest` (§6).
- `request_id` values from the core start at 1 and increase
  monotonically per process; backend-initiated `request_id`s
  (join requests) must not collide with the core's in-flight ids
  — use a disjoint range (the stub uses 1,000,000+).
- The `deadline_ms` on each request is a real budget: abandon
  work past it (notably `download.*` and `vc.join`).
- `test.sleep_ms` is a stub-only action for deadline tests. The
  real backend must answer it `not_found`.
- Unknown actions answer `not_found` (the core treats a 404 for
  a known contract action as "engine does not implement it").
- The core's `get()` path translation (§4 legacy shim) means the
  backend only ever sees dotted actions — it never needs to parse
  `/v1/…` paths.
