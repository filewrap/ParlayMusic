# Parlay 🎵

**Parlay isn't just a normal bot — it's a Mixture-of-Experts of languages:
Go, Rust, C++, JavaScript / TypeScript — in one Telegram music bot.**

- **Group management** — admin / sudo / auth users, chat settings, broadcast
- **Anonymous music sharing** — share a track via a unique link, no identity attached
- **Downloads** — YouTube, Spotify, NetEase, Deezer, TikTok, Navidrome/Soulseek
- **Streaming** — voice-chat streaming, live radio, inline search & play
- **GUI + webapp playing** — web player: now-playing, queue, search, controls
- **Audio FX** — Slow+Reverb, Nightcore, Bass Boost, 8D
- **Spotify intelligence** — profanity / AI-lyrics filter, auto-skip disliked tracks

## Architecture (one line each)

| Piece | Language | From | Does |
|---|---|---|---|
| `services/core-cpp` | C++20 | anonx-cpp + NoxBeats + TinaMusic | Bot core / router: commands, permissions, routing; owns the Opus pipeline + TDLib/ntgcalls voice backends (in-process) |
| `services/vc-go` | Go | YukkiMusic (Go) + ArcMusic + TgMusicBot | VC streaming engine, two profiles: `low-ram` / `high-perf` |
| `services/vc-ts` | TypeScript | TGVCBot + remix + Lemonic | TS VC engine + Lemonic API + inline search |
| `services/vc-rust` | Rust | ParvatiMusic | Rust VC engine (Opus/RTP pipeline, ntgcalls offer), fallback VC |
| `services/audio-fx-rust` | Rust | telegram-music-bot-rust | FFmpeg audio effects |
| `services/share-rust` | Rust | feel-me | Anonymous sharing links |
| `services/queue-rust` | Rust | daslui telegram-music-bot | Crowd-curated voting queue → Spotify playback queue |
| `services/spotify-rust` | Rust | Rustify | Spotify playback watch, lyrics/profanity filter, auto-skip |
| `services/gui-js` | JS (plain Node, stdlib — no Next.js) | wavetune | Web GUI + Telegram Mini App player |
| `services/radio-js` | JS (Node) | openradiobot | Internet radio stations |
| `services/downloaders` | Python | sanila YT + Spotify-Loader + beetube + Navidrome companion | Download workers (YT/Spotify/Deezer/TikTok/Navidrome; Soulseek via slskd, opt-in) |
| `services/netease-go` | Go | Music163bot-Go | NetEase Cloud Music search/download/lyrics |

All services talk through one contract: `contracts/ipc.json`
(HTTP + JSON, one endpoint per service, `X-Parlay-Token` header).
See [ARCHITECTURE.md](ARCHITECTURE.md) and [PORT_MAP.md](PORT_MAP.md).

## Status

**Implemented through Phases 2–5 + TDLib wiring (2026-11-03).** All 12
services build clean and 361 tests pass on re-run (Phase 5
verification; see `PHASE5.md` §6). Phase 6 closes the remaining
audit gaps (see `REMAINING.md`). What is real, per service:

- **core-cpp** — full command surface + permissions, engine fail-over
  (go→ts→rust), Mongo/Redis store, TDLib 1.8.69 client (26/26 tests) +
  in-process ntgcalls voice backend. Serves its own HTTP API on
  `:8080` (`GET /healthz`, `/api/now-playing`, `/api/queue` — Phase 6,
  A2 closed), so the GUI's now-playing/queue endpoints have a real
  backend now; with no `BOT_TOKEN` the core runs HTTP-only.
- **vc-go / vc-ts / vc-rust** — queue + playback state machines, real
  resolvers (yt-dlp), loop/shuffle/autoplay in all three engines (A3
  closed in Phase 5), Opus/RTP pipeline proven to a file in all
  three (251 frames). **No live voice join has ever happened** — audio
  has never reached a Telegram voice chat (A1, needs a host with open
  network + assistant session + libntgcalls.so).
- **downloaders** — YouTube/Spotify/Deezer/TikTok/Navidrome all real;
  Spotify collections served over HTTP (`POST /v1/spotify/collection`,
  Phase 5). Deezer is 30 s previews without an ARL cookie.
- **netease-go** — search/lyrics/download live-verified in Phase 4
  (mirror fallback, 3 FLACs); `POST /v1/search` added in Phase 5.
- **audio-fx / share / queue / spotify / gui / radio** — all real
  services with smoke tests (Phase 5 added the missing Rust tests).

Reference repos live under `refs/` (shallow clones, reference-only,
gitignored) so ports can be re-implemented behaviour-by-behaviour against
the IPC contract. See `PORT_MAP.md` for the per-feature source map.
Per the user's order, licensing is not re-raised as a task.

## Quick start

```bash
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
docker compose up --build            # core + vc-go + downloaders + mongo + redis
docker compose --profile full up --build   # + every other service + slskd
```

Docker is not installed in the sandbox, so `docker compose up` has never
run here; the compose file is validated statically (YAML parse +
port/URL cross-check, `phase5/worker-e-compose-check.py`). Run
`docker compose config` on a host with Docker first.

Per-service runs (no Docker; toolchains live under `.tools/` and
`~/.cargo/bin` — see each service README for the exact command):

```bash
# core (needs BOT_TOKEN + PARLAY_TOKEN)
cd services/core-cpp && cmake -B build && cmake --build build && ./build/parlay-core
# Go VC engine (default :8001) / NetEase (:8010)
cd services/vc-go && ../../.tools/go/bin/go run ./cmd/parlay-vc
cd services/netease-go && ../../.tools/go/bin/go run ./cmd/parlay-netease
# TS VC engine (:8002) / radio (:8008) / GUI (:3000) — stdlib Node, no npm install
cd services/vc-ts && node src/main.js
cd services/radio-js && node src/main.js
cd services/gui-js && node app/main.js
# Rust services (PORT env or the default in each README)
cd services/vc-rust && cargo run --release          # :8003
cd services/audio-fx-rust && cargo run --release    # :8004 under compose (code default 8003)
cd services/share-rust && cargo run --release       # :8005 under compose (code default 8004)
cd services/queue-rust && cargo run --release       # :8006 under compose (code default 8005)
cd services/spotify-rust && cargo run --release     # :8007
# downloaders (:8009 under compose, code default 8010)
cd services/downloaders && .venv/bin/python -m src.main
```

Port map (compose is the source of truth — it sets `PORT` explicitly;
see the header of `docker-compose.yml` for the four code-default
overrides): core 8080, vc-go 8001, vc-ts 8002, vc-rust 8003, audio-fx
8004, share 8005, queue 8006, spotify 8007, radio 8008, downloaders
8009, netease 8010, gui 3000, slskd 5030/5031. Health: `GET /healthz`
on every service, including core-cpp (:8080).

## Configuration

No secrets are committed. Copy the template and fill it in locally:

```bash
cp .env.example .env   # BOT_TOKEN, API_ID, API_HASH, MONGO_URI, etc.
```

`.env.example` lists every variable the code reads (84, one line each
with a comment). Never commit `.env`. See `.gitignore`.

## License

TBD — do not pick one until the GPL/AGPL port question in `PORT_MAP.md`
is settled. Until then this tree is unlicensed.
