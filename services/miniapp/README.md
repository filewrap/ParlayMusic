# Parlay Mini App — TypeScript lossless player (Phase 6,
# FINAL-ARCHITECTURE.md Part 4)

Typed single-page player, no framework, no runtime
dependencies. Built by `build.sh` into `dist/`; the Go backend
serves that directory (see `services/backend-go/internal/httpapi`).

## Endpoints used (all real backend state — no second API)

The Mini App talks only to the backend's `/api/v1` HTTP
surface, which is a transport into the exact `gui.*` / `vc.*`
UDS actions the core uses over the socket:

| UI | Endpoint | UDS action behind it |
|---|---|---|
| Now playing + position | `GET /api/v1/now-playing?chat_id=` | `gui.now_playing` |
| Queue list | `GET /api/v1/queue?chat_id=` | `gui.queue` |
| Pause / Resume button | `POST /api/v1/pause`, `POST /api/v1/resume` | `vc.pause`, `vc.resume` |
| Skip button | `POST /api/v1/skip` | `vc.skip` |
| Volume slider | `POST /api/v1/volume` | `vc.volume` |
| Stream FLAC button / `<audio>` src | `GET /api/v1/stream/:chat_id/flac` | FLAC cache lookup (library index) |

State refreshes by polling `now-playing` every 2 s.

## Known gaps (honest, not hidden)

- **No WebSocket/SSE instant sync yet.** The locked spec
  wants instant state sync; the app polls every 2 s instead.
  Polling hits the real state, so nothing is faked — SSE is a
  follow-up, not a stub.
- **No Telegram WebApp initData validation in the backend.**
  The bot token lives in the core, not the backend, so the
  HTTP server has no initData helper in-tree. Default bind is
  loopback-only (`127.0.0.1:8081`); a public deployment must
  sit behind a trusted reverse proxy. Live-test item.
- **chat_id source:** `Telegram.WebApp.initDataUnsafe.chat.id`
  inside Telegram, `?chat_id=` query param outside it (dev).

## Build

```sh
services/miniapp/build.sh   # typecheck + build to dist/
npm run typecheck           # tsc --noEmit only
```

`node_modules/` is gitignored; `npm install` runs once after
checkout (or automatically from `build.sh`).
