# netease-go — NetEase Cloud Music

From: XiaoMengXinX/Music163bot-Go
(`internal/bot/handlers_{music,search,lyric,recognize}.go`).
Isolated on purpose: cookie/login handling stays inside this service.
IPC: netease.search/download/lyrics (+ recognize later).

## What is actually implemented (Phase 3, 2026-11-02)

Real Go IPC service (stdlib only), `cmd/parlay-netease` +
`internal/netease` (API client) + `internal/server` (HTTP front-end).
`go vet` + `go build` pass.

| Route | Behaviour |
|---|---|
| `GET /healthz` | `{ok:true, service:"netease-go"}` (unauthenticated, per contract) |
| `POST /v1/netease/search` `{query, limit?}` | Live NetEase cloudsearch (`POST https://music.163.com/api/cloudsearch/pc`, form, 10 s timeout, no login/cookie) → `{results:[Track]}`. Verified live: "Jay Chou" → 5 results, "周杰伦" → 3. |
| `POST /v1/netease/lyrics` `{track_id}` | Live lyric API (`/api/song/lyric`) → `{lyrics}`. Verified live for track 210049 (布拉格广场). |
| `POST /v1/netease/download` `{track_id}` | Resolves detail + audio URL, downloads to `PARLAY_DL_DIR` → `{file_path, track, oversize, size_bytes}`. 50 MB Bot API rule: declared or streamed size over 50 MB → no file kept, `oversize:true` (queued for a later MTProto pass, per contracts/ipc.json). |

Envelopes everywhere: `{ok:true,data}` / `{ok:false,error:{code,message}}`.
`X-Parlay-Token` auth when `PARLAY_TOKEN` is set (401 without/wrong
token, 200 with it; `/healthz` exempt). `track_id` accepts a JSON string
or number. No login, no `MUSIC_U` cookie, no credentials anywhere.

## Env

- `PORT` (default `8010`)
- `PARLAY_TOKEN` — `X-Parlay-Token` value; empty = auth disabled (warns)
- `PARLAY_DL_DIR` — download directory (default `./data`)

## Known live limitation — download cannot complete from the sandbox

Verified 2026-11-02 (see PROGRESS.md): search, detail and lyrics work
live, but **anonymous audio URLs are not obtainable from this egress
IP**, so `/v1/netease/download` honestly returns `engine_unavailable`:

- The v1 song-url endpoint (`/api/song/enhance/player/url/v1`) rejects
  plain requests with `{"msg":"参数错误","code":400}` — it requires the
  encrypted weapi payload the upstream Music163Api-Go library builds.
- The non-v1 endpoint (`/api/song/enhance/player/url`) answers plainly
  but returns `url:null, code:404` for every track tried (210049,
  5257138, 509781655), and once returned a `-462` verification challenge
  (`st.music.163.com/encrypt-pages`). No playable URL without login.
- The service therefore returns
  `engine_unavailable: "netease song url: track <id> has no playable URL
  without login (netease code 404)"` and writes **no file, nothing fake**.

The download code path itself (detail → URL → streamed download with
the 50 MB cap, partial-file cleanup) is implemented and will work
unchanged if a playable URL ever resolves — e.g. on a host where
NetEase serves anonymous URLs, or after adding the weapi-encrypted
song-url call / an optional `MUSIC_U` login (not done here, and would
need the user's cookie — never embed one).

Also observed: NetEase (via the egress proxy) intermittently returns a
response body duplicated (`{...}{...}`); the client decodes only the
first JSON value, so this is tolerated.

## Still stubbed / not implemented

- Audio recognition (`handlers_recognize.go` in the ref) — not ported.
- No `MUSIC_U` login / QR login (`qrlogin.go` in the ref) — anonymous
  only, by design for this phase.
- No weapi-encrypted requests (would be needed to unlock v1 song URLs).
- No end-to-end download test with a real audio file — impossible while
  the URL endpoint returns `url:null` (see above).
