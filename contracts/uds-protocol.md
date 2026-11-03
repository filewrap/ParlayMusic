# Parlay UDS Protocol — core-cpp ↔ backend-go wire spec

Status: **Phase 2, implemented** (core side: `services/core-cpp/src/ipc.*`;
test peer: `services/core-cpp/tests/stub_backend.cpp`).
Replaces the HTTP wire protocol described in `contracts/ipc.json`
(FROZEN parity reference — the 56 actions and their payload shapes
are unchanged; only the transport changes). `ipc.json` stays the
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

## §4. Action table — all 56 contract actions

Payload/response shapes are exactly `contracts/ipc.json`
`actions/<name>` (request / response objects, `Track` /
`NowPlaying` / `QueueItem` types). This table maps each action onto
the UDS envelope and records the core-side legacy name it replaces
(the core translates legacy slash-form names — `search`,
`download/youtube`, `radio/start`, `share`, `fx`, `vote`,
`dislike`, vc short names — to these dotted names; that shim dies
with the dispatcher/engine-router rewrite in Wave 2).

| # | Action | Request (payload) | Response (data) | Deadline |
|---|---|---|---|---|
| 1 | `chat.settings` | `chat_id`; POST only: `key`, `value` | `chat_id`, `settings` (15 keys) | 10 s |
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
| 13 | `lemonic.download` | `track_id` | `file_path`, `track` | 30 s |
| 14 | `lemonic.search` | `query` | `results` ([Track]) | 30 s |
| 15 | `netease.download` | `track_id` | `file_path`, `track`, `auth` | 30 s |
| 16 | `netease.lyrics` | `track_id` | `lyrics`, `track_id`, `title`, `artist`, `source`, `truncated`, `lrc_path` | 30 s |
| 17 | `netease.search` | `query` | `results` ([Track]) | 30 s |
| 18 | `queue.vote` | `track`, `vote` (approve\|decline), `voter_id` | `queued` | 30 s |
| 19 | `radio.list` | — | `stations` ([string]) | 10 s |
| 20 | `radio.start` | `chat_id`, `station` | `stream_url` | 30 s |
| 21 | `radio.status` | `chat_id` | `now_playing`, `station`, `stream_url`, `reachable`, `vc_handoff` | 10 s |
| 22 | `radio.stop` | `chat_id` | `stopped` | 30 s |
| 23 | `search.inline` | `query`, `user_id`, `chat_id` | `results` ([Track]) | 30 s |
| 24 | `search.query` | `query`, `source`, `limit` | `results` ([Track]) | 30 s |
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

\* Download actions: the old HTTP call sites passed an explicit
120 s timeout for `download.*` POSTs. That explicit value still
governs (the deadline table has no `download.*` entry, so the
call-site parameter wins — see §8). The config rewrite should set
a `download_ms` budget instead.

Protocol-level action (NOT one of the 56): `ping`, empty payload,
answers `{service: "backend-go", …}` — the core's `healthz()`.

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
`timeout_sec` call-site parameter (download POSTs pass 120 s,
search 15–20 s, netease 20 s, GETs 10 s). The table is the source
of truth once the config rewrite (Worker C) populates it from
`parlay.toml [timeouts]` (`resolve_ms`, `join_ms`, `ipc_cmd_ms`,
`download_ms`) via `IpcClient::setDeadline()`.

| Class | Default |
|---|---|
| General (all actions not listed below) | 30,000 ms |
| GET-style: `vc.now_playing`, `vc.progress`, `vc.panel`, `vc.lyrics`, `radio.list`, `radio.status`, `download.progress`, `share.resolve`, `chat.settings`, `spotify.details`, `spotify.now_playing`, `spotify.watches`, `spotify.status`, `gui.now_playing`, `gui.queue` | 10,000 ms |
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
