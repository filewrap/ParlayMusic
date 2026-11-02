# netease-go — NetEase Cloud Music

From: XiaoMengXinX/Music163bot-Go
(`internal/bot/handlers_{music,search,lyric,recognize}.go`).
Isolated on purpose: cookie/login handling stays inside this service.
IPC: netease.search/download/lyrics + the generic `search.query` route
(`POST /v1/search`, Phase 5). Audio recognition is out of scope (B7).

## What is actually implemented (Phase 3, 2026-11-02)

Real Go IPC service (stdlib only), `cmd/parlay-netease` +
`internal/netease` (API client) + `internal/server` (HTTP front-end).
`go vet` + `go build` pass.

| Route | Behaviour |
|---|---|
| `GET /healthz` | `{ok:true, service:"netease-go"}` (unauthenticated, per contract) |
| `POST /v1/netease/search` `{query, limit?}` | Live NetEase cloudsearch (`POST https://music.163.com/api/cloudsearch/pc`, form, 10 s timeout; sends the `MUSIC_U` cookie when `NETEASE_MUSIC_U` is set) → `{results:[Track]}`. Verified live: "Jay Chou" → 5 results, "周杰伦" → 3. |
| `POST /v1/search` `{query, source?, limit?}` | Contract `search.query` (Phase 5): same NetEase search; `source` must be `netease`, `all`, or absent — any other source is refused 400, never answered with wrong-source results. |
| `POST /v1/netease/lyrics` `{track_id}` | Live lyric API (`/api/song/lyric`) → `{lyrics}`. Verified live for track 210049 (布拉格广场). |
| `POST /v1/netease/download` `{track_id}` | Resolves detail + audio URL, downloads to `PARLAY_DL_DIR` → `{file_path, track, oversize, size_bytes, level, auth}`. `auth` is `music_u` when `NETEASE_MUSIC_U` is set, else `anonymous`. 50 MB Bot API rule: declared or streamed size over 50 MB → no file kept, `oversize:true` (queued for a later MTProto pass, per contracts/ipc.json). |

Envelopes everywhere: `{ok:true,data}` / `{ok:false,error:{code,message}}`.
`X-Parlay-Token` auth when `PARLAY_TOKEN` is set (401 without/wrong
token, 200 with it; `/healthz` exempt). `track_id` accepts a JSON string
or number. Login is optional: set `NETEASE_MUSIC_U` to the `MUSIC_U`
cookie value and every NetEase-host request (search/detail/lyrics/
eapi/weapi/linuxapi/downloads) carries `Cookie: MUSIC_U=<value>`; the
cookie is never sent to the third-party mirror or any non-NetEase host.
Unset = anonymous. The cookie value is never logged. No QR login.

## Env

- `PORT` (default `8010`)
- `PARLAY_TOKEN` — `X-Parlay-Token` value; empty = auth disabled (warns)
- `PARLAY_DL_DIR` — download directory (default `./data`)
- `NETEASE_MUSIC_U` — optional MUSIC_U login cookie value (the value
  itself, not a `MUSIC_U=` pair). When set, every NetEase request
  carries `Cookie: MUSIC_U=<value>` — plain endpoints
  (`setAuthHeaders`), the eapi envelope (replaces the anonymous
  `MUSIC_A` constant), and the weapi/linuxapi envelopes — and direct
  downloads from NetEase CDN hosts carry it too. It is never sent to
  third parties (notably the gdstudio mirror) and never logged.
  Unset = anonymous-only; the mirror fallback covers downloads.

## MUSIC_U login (Phase 5, B3 — 2026-11-03)

`internal/netease/client.go` (`New()` reads the env once;
`NewWithMusicU(u)` for tests; `AuthMode()` reports `music_u` /
`anonymous`) + `eapi.go` (`eapiCookie`) + `weapi.go` / `linuxapi.go`
(`setAuthHeaders`). Verified offline with stubbed servers
(`internal/netease/client_test.go`, 12 cookie tests): logged-in eapi
calls carry `MUSIC_U` and no `MUSIC_A`; anonymous calls carry the
public `defaultMusicA` constant and no `MUSIC_U`; the cookie never
leaks to a non-NetEase host; `SongURLWithLevel` against a stubbed
eapi endpoint returns the URL at level `eapi:standard` with the
cookie attached. The download response exposes `auth` so callers can
tell which mode produced the URL. No real cookie has ever been used
here — supply it in the environment only, never in the repo.

## Known live limitation — download cannot complete from the sandbox

> **SUPERSEDED (Phase 5, 2026-11-03).** The section below describes the
> Phase 3 state (2026-11-02), when anonymous audio URLs were not
> obtainable from the sandbox egress IP. Phase 4 fixed it: the mirror
> fallback is in code (`internal/netease/client.go:336-360`,
> `SongURLWithLevel` step 6) and 3 live FLAC downloads were
> ffprobe-verified. `/v1/netease/download` no longer returns
> `engine_unavailable` for this reason. The old text is kept as history.

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
NetEase serves anonymous URLs, or with the `MUSIC_U` login now wired
(env `NETEASE_MUSIC_U`; the cookie is read once at startup, never
embedded). Direct (non-mirror) song URLs still return `url:null`
server-side without login; with a valid `MUSIC_U` the eapi song-url
call carries the login cookie, which is what unblocks them.

Also observed: NetEase (via the egress proxy) intermittently returns a
response body duplicated (`{...}{...}`); the client decodes only the
first JSON value, so this is tolerated.

## Still stubbed / not implemented

- Audio recognition (`handlers_recognize.go` in the ref) — NOT ported,
  dropped from scope in Phase 5 (B7): it needs an external fingerprint
  backend and the ref's flow needs login. No `netease.recognize` action
  exists in the contract.
- `MUSIC_U` login — DONE in code (Phase 5 scope update): env
  `NETEASE_MUSIC_U` is read once at startup and sent as a cookie only
  to NetEase hosts (never the mirror). QR login (`qrlogin.go` in the
  ref) is still not implemented — no QR flow, the cookie must come
  from the user.
- weapi/linuxapi-encrypted requests exist (`weapi.go`, `linuxapi.go`,
  `SongURLWithLevel` steps 2/3/5) but are only fallbacks: direct
  (non-mirror) song URLs still return `url:null` server-side without
  login; the mirror fallback is the supported anonymous download path.
- No automated live download test — the "works live" claim rests on
  PHASE4's manual run (3 ffprobe-verified FLACs), not a re-runnable test.
