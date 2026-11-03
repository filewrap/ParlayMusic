# Parlay — IPC contract

`ipc.json` is the source of truth for every core → backend call.

- Transport: UDS (Unix domain socket) + JSON — one persistent socket
  (`data/parlay-backend.sock`), length-prefixed frames carrying JSON
  envelopes, no token header (socket permissions are the access
  control). See `uds-protocol.md`.
- Health: the `ping` UDS action (not one of the 65) →
  `{ "ok": true, "service": "backend-go", … }` — the core's
  `healthz()` check.
- Success: `{ "ok": true, "data": { … } }`.
  Error: `{ "ok": false, "error": { "code": "…", "message": "…" } }`.
- Error codes: `bad_request`, `unauthorized`, `not_found`,
  `engine_unavailable`, `download_failed`, `rate_limited`, `internal`.
- Core rule: if the backend returns `engine_unavailable` for a known
  contract action, or times out (30 s), the core fails the call
  honestly — there is no second engine to fall back to; the unified
  backend is the only engine. A `not_found` for an unknown action is a
  real not-found and stays a 404.
- Oversize rule: download actions return `oversize: true` for files over
  the 50 MB Bot API cap; the core queues them for a later MTProto pass
  instead of failing the request.

Add a new action here first, then in the backend package, then in the core handler.

## Core HTTP API (GUI-facing, Phase 6 A2)

The core also serves the GUI directly on `PORT` (default 8080) —
these are **not** IPC actions and have no entry in `ipc.json`:

- `GET /healthz` → `{"ok":true}`.
- `GET /api/now-playing?chat_id=<id>` → `{"ok":true,"now_playing":{...}}`
  (the backend room's live state, same data as the Telegram
  commands) or `404 {"ok":false,"error":"no playback state"}`.
- `GET /api/queue?chat_id=<id>` → `{"ok":true,"queue":[...],"chat_id":<id>}`
  or the same honest 404.

Note the shape difference: the core HTTP API is **flat**
(`{"ok":false,"error":"<message>"}`), while the IPC envelope above
wraps errors as `{"error":{"code","message"}}`. The GUI passes the
core's flat errors through verbatim. Missing/invalid `chat_id` is a
400; unknown routes 404; non-GET 405.
