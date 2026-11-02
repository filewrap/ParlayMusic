# netease-go — PROGRESS (Phase 3, 2026-11-02)

Worker C. Real Go IPC service, stdlib only.

## Files changed

- `services/netease-go/cmd/parlay-netease/main.go` — rewritten from the
  print-only stub into the real server entry (PORT/PARLAY_TOKEN/
  PARLAY_DL_DIR, graceful shutdown).
- `services/netease-go/internal/netease/client.go` — NEW: anonymous
  NetEase client (cloudsearch, song detail, song URL, lyrics, download
  with 50 MB cap). Plain endpoints only, no weapi encryption, no login.
- `services/netease-go/internal/server/server.go` — NEW: HTTP front-end,
  envelope, `X-Parlay-Token` auth, handlers for search/download/lyrics.
- `services/netease-go/README.md` — rewritten: implemented vs stubbed,
  incl. the live download limitation.
- `services/netease-go/PROGRESS.md` — this file.
- `services/netease-go/go.mod` — unchanged (`module parlay/netease-go`,
  `go 1.23`, no dependencies).

## Endpoints

| Route | Status |
|---|---|
| `GET /healthz` | `{ok:true,service:"netease-go"}`, unauthenticated |
| `POST /v1/netease/search` | Live cloudsearch, real results |
| `POST /v1/netease/lyrics` | Live lyric API, real lyrics |
| `POST /v1/netease/download` | Implemented end-to-end (detail → URL → stream to `PARLAY_DL_DIR`, 50 MB oversize rule) but **cannot complete live from the sandbox** — see below |

## Exact verification (2026-11-02 ~19:59–20:02 IST)

Build (Go 1.23.0 at `~/workspace/parlay/.tools/go/bin/go`):

```
cd ~/workspace/parlay/services/netease-go
go vet ./... && go build -o /tmp/parlay-netease ./cmd/parlay-netease
# → BUILD_OK (vet clean, no warnings)
```

Live run: `PORT=18010 PARLAY_DL_DIR=/tmp/parlay-netease-dl /tmp/parlay-netease`

```
curl http://127.0.0.1:18010/healthz
# → {"ok":true,"service":"netease-go"}                                  PASS

curl -X POST http://127.0.0.1:18010/v1/netease/search -d '{"query":"Jay Chou","limit":5}'
# → ok:true, 5 results:
#   210049 布拉格广场 - 蔡依林, 周杰伦 294s
#   509781655 想你就写信 (Live) - 周杰伦, 李硕, 张鑫 238s
#   5257138 屋顶 - 周杰伦, 温岚, 吴宗宪 319s
#   490595315 因为爱情 (Live) - 周杰伦, 那英 202s
#   1317494434 屋顶 (Live) - 周杰伦, 宿涵, 张神儿 312s               PASS

curl -X POST .../search -d '{"query":"周杰伦","limit":3}'
# → ok:true, 3 results (509781655, 5257138, 210049)                    PASS

curl -X POST .../search -d '{"query":""}'
# → {"ok":false,"error":{"code":"bad_request","message":"query is required"}}  PASS

curl -X POST http://127.0.0.1:18010/v1/netease/lyrics -d '{"track_id":"210049"}'
# → ok:true, lyrics start "[00:00.00] 作词 : 方文山 / [00:01.00] 作曲 : 周杰伦 …"  PASS
# (lyrics also verified directly via curl to music.163.com beforehand)

curl -X POST http://127.0.0.1:18010/v1/netease/download -d '{"track_id":210049}'
# → {"ok":false,"error":{"code":"engine_unavailable",
#      "message":"netease song url: track 210049 has no playable URL
#      without login (netease code 404)"}}                              PASS (honest)
#   /tmp/parlay-netease-dl stayed empty — no file, nothing fake.

curl -X POST .../download -d '{"track_id":"notanumber"}'
# → {"ok":false,"error":{"code":"bad_request","message":"strconv.ParseInt: parsing \"notanumber\": invalid syntax"}}  PASS
```

Auth (restarted with `PARLAY_TOKEN=test-token-123`):

```
no token    → HTTP 401   PASS
wrong token → HTTP 401   PASS
right token → HTTP 200   PASS
GET /healthz without token → HTTP 200   PASS
```

## Why download cannot complete live (verified by direct curl)

1. `GET /api/song/enhance/player/url/v1?ids=[210049]&level=standard`
   (and POST form, and `ids=210049` without brackets) →
   `{"msg":"参数错误","code":400}` every time. The v1 endpoint needs the
   encrypted weapi payload; plain use is dead. The service therefore
   uses the non-v1 endpoint instead.
2. `GET /api/song/enhance/player/url?ids=[210049]&br=320000` →
   `{"data":[{"id":210049,"url":null,"br":0,"code":404,...}]}`.
   Same `url:null, code:404` for 5257138 and 509781655. One POST attempt
   returned code `-462` with a verification challenge
   (`st.music.163.com/encrypt-pages`, `verifyType:40`).
   Conclusion: no anonymous playable URL from this egress IP.
3. Side observation: NetEase (via the egress proxy) intermittently
   returns a response body duplicated (`{...}{...}`). The client now
   decodes only the first JSON value (`decodeFirst`), so this is
   tolerated — search/lyrics/detail all work despite it.

## Still stubbed

- Audio recognition (`handlers_recognize.go` in the ref) — not ported.
- No `MUSIC_U` login / QR login — anonymous only, by design.
- No weapi-encrypted requests — the route to unlock v1 song URLs.
- No end-to-end download test with a real audio file — impossible while
  the URL endpoint returns `url:null` (see above). The download code
  path (detail → URL → streamed write, 50 MB cap, partial cleanup) is
  implemented and will work unchanged if a playable URL ever resolves.

## Cleanup

All test servers killed (port 18010 confirmed free); test binaries and
`/tmp/parlay-netease-dl` removed. No secrets in the tree (test token
`test-token-123` was only a process env var, never written to a file).
No push to GitHub.
