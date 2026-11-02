# Parlay — IPC contract

`ipc.json` is the source of truth for every inter-service call.

- Transport: HTTP/1.1 + JSON, `POST /v1/<action>`, header
  `X-Parlay-Token: $PARLAY_TOKEN`.
- Health: `GET /healthz` → `{ "ok": true, "service": "<name>" }`.
- Success: `{ "ok": true, "data": { … } }`.
  Error: `{ "ok": false, "error": { "code": "…", "message": "…" } }`.
- Error codes: `bad_request`, `unauthorized`, `not_found`,
  `engine_unavailable`, `download_failed`, `rate_limited`, `internal`.
- Core rule: if the selected VC engine returns `engine_unavailable` or
  times out (30 s), the core retries the same call on the next engine in
  the chat's fallback list.
- Oversize rule: download actions return `oversize: true` for files over
  the 50 MB Bot API cap; the core queues them for a later MTProto pass
  instead of failing the request.

Add a new action here first, then in the service, then in the core handler.
