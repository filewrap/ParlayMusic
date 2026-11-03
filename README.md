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

## Install (repo checkout, current architecture)

The v0.2.0 release tarball installs the **old** 12-service topology and
predates the locked 2-process architecture below — do not use it for a
fresh install. Build from the repo instead (main at `f1a7211`, Phase 3):

```bash
git clone https://github.com/filewrap/ParlayMusic.git && cd ParlayMusic
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
cp parlay.toml.example parlay.toml   # optional: defaults apply without it
python3 -m venv scripts/yt-dlp-venv && scripts/yt-dlp-venv/bin/pip install yt-dlp

# backend first (it creates the UDS socket the core connects to)
cd services/backend-go && go build -o parlay-backend ./cmd/backend && ./parlay-backend &
# core second
cd ../core-cpp && cmake -B build && cmake --build build && ./build/parlay-core
```

`install.sh` / `start.sh` / `stop.sh` / `status.sh` still describe the
old 12-service topology and are being rewritten for the new layout —
until then, start the two processes by hand as above. Full guide
(prereqs, systemd, troubleshooting): [INSTALL.md](INSTALL.md).

## Architecture (one line each)

Two processes plus a warm daemon — no inter-service HTTP, no Mongo/Redis,
no subprocess per call. See [FINAL-ARCHITECTURE.md](FINAL-ARCHITECTURE.md)
(the locked architecture) and [ARCHITECTURE.md](ARCHITECTURE.md).

| Piece | Language | From | Does |
|---|---|---|---|
| `services/core-cpp` | C++20 | anonx-cpp + NoxBeats + TinaMusic | Bot core / router: commands, permissions, routing; dual TDLib sessions (bot + assistant, build-time linked); owns the Opus pipeline + ntgcalls voice join (via the backend's UDS join handshake) |
| `services/backend-go` | Go | YukkiMusic (Go) + ArcMusic + TgMusicBot + the deleted TS/Rust/Python/JS services | Unified backend: all 65 IPC actions — rooms/queue, native resolvers (YouTube InnerTube, JioSaavn, NetEase, Deezer, Navidrome, Soulseek via slskd, Spotify, TikTok, radio), downloads, Spotify watch/classifier, share links, voting queue, audio FX (record-only until Phase 5 DSP), search, Mini App state views; bbolt store `data/parlay.db` |
| `scripts/yt_dlp_worker.py` | Python | — | Persistent yt-dlp daemon (warm fallback resolver/downloader), started once by the backend at boot; venv `scripts/yt-dlp-venv/` |
| `services/audio-fx-rust` | Rust | telegram-music-bot-rust | Kept as the Phase 5 DSP port source only — not a running service |
| slskd (optional) | — | — | Soulseek daemon for Soulseek search/download (opt-in) |

The core and the backend talk over one persistent Unix Domain Socket,
`data/parlay-backend.sock` (length-prefixed binary frames carrying JSON
envelopes, `contracts/uds-protocol.md`). The contract is
`contracts/ipc.json` v0.3.1 — **65 actions** (56 + 9 OD-3) — frozen as
the parity reference for request/response shapes; the transport is UDS,
not HTTP. See [ARCHITECTURE.md](ARCHITECTURE.md) and
[PORT_MAP.md](PORT_MAP.md).

## Status

**Implemented through Phase 3 (2026-11-03, main `f1a7211`).** The old
12-service HTTP topology is deleted: `services/` holds only
`core-cpp`, `backend-go`, and `audio-fx-rust` (Phase 5 port source).
Phase 3 verification: Go 25/25 packages, **245 passed / 0 failed /
12 skipped**; ctest 9/10 (**1,615 PASS**, the only failures the 2
pre-existing `store_test` env failures); routing table **944 pass**;
**64/65** IPC actions answered against the real core UDS client
(`vc.join` needs a real group call — Phase 6 live proof). What is
real, per piece:

- **core-cpp** — full command surface + permissions, dual TDLib
  sessions (bot token + assistant, build-time linked libtdjson
  1.8.69 — no dlopen, no `getUpdates` long-poll), token-bucket rate
  limits (`parlay.toml` `[rate_limit]`), UDS client to the backend.
  The legacy `:8080` HTTP server (`http_server.cpp` / `core_api.cpp`)
  still exists in the tree and starts at boot, but nothing uses it —
  FINAL-ARCHITECTURE.md §6-R1 schedules its deletion.
- **backend-go** — all 65 actions answered over UDS; single room
  manager + bbolt store (`data/parlay.db`, 0600 in a 0700 dir;
  kill −9 proof passed); native resolvers (YouTube InnerTube
  0.61–0.81 s vs yt-dlp 5.7–6.7 s, JioSaavn 0.32 s at 320 kbps);
  in-process ntgcalls via cgo (voice join handshake over UDS);
  `/loop` → `vc.loop`, queue display, `cbDl applied:false`, replied
  media → `vc.play_file` fixed in Phase 3.
- **yt-dlp daemon** — warm at backend boot; native resolvers first,
  daemon as fallback.
- **No live voice join has ever happened** — audio has never reached
  a Telegram voice chat. What is still missing is a host with open
  network + assistant session for the live call (Phase 6).
- **Key-gated paths fail honestly:** Deezer (no ARL — user-dropped
  for now), NetEase lossless (no `MUSIC_U` — +86-only signup),
  Spotify OAuth, AudD `/recognize`, Gemini classifier, Lemonic,
  Soulseek account.

Reference repos live under `refs/` (shallow clones, reference-only,
gitignored) so ports can be re-implemented behaviour-by-behaviour against
the IPC contract. See `PORT_MAP.md` for the per-feature source map.
Per the user's order, licensing is not re-raised as a task.

## Quick start (repo checkout)

```bash
cp .env.example .env   # fill in BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
cp parlay.toml.example parlay.toml   # optional: defaults apply without it
python3 -m venv scripts/yt-dlp-venv && scripts/yt-dlp-venv/bin/pip install yt-dlp
cd services/backend-go && go build -o parlay-backend ./cmd/backend && ./parlay-backend &
cd ../core-cpp && cmake -B build && cmake --build build && ./build/parlay-core
```

`docker-compose.yml` still describes the deleted 12-service topology
and has not been rewritten for the 2-process engine — there is no
Docker route until that rewrite lands. `docker compose up` has never
run here (no Docker in the sandbox).

## Configuration

No secrets are committed. Copy the template and fill it in locally:

```bash
cp .env.example .env   # BOT_TOKEN, API_ID, API_HASH, OWNER_ID, PARLAY_TOKEN
```

`.env.example` still lists variables from the old topology (e.g.
`MONGO_URI`, `REDIS_URL`, `VC_*_URL`) — those are legacy/unused and
are being cleaned up with the config rewrite. The live config is
`parlay.toml` (see `parlay.toml.example`: rate limits, timeouts,
quality, limits) plus `.env` for secrets. The Spotify per-user OAuth
redirect URI must be exactly `http://127.0.0.1:8080/callback`.
Never commit `.env`. See `.gitignore`.

## License

TBD — do not pick one until the GPL/AGPL port question in `PORT_MAP.md`
is settled. Until then this tree is unlicensed.
