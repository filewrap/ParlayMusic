# Parlay 🎵

**Parlay isn't just a normal bot — it's a Mixture-of-Experts of languages:
Go, Rust, C++, JavaScript / TypeScript — in one Telegram music bot.**

- **Group management** — admin / sudo / auth users, chat settings, broadcast
- **Anonymous music sharing** — share a track via a unique link, no identity attached
- **Downloads** — YouTube, Spotify, NetEase, Deezer, TikTok, Soulseek/Navidrome
- **Streaming** — voice-chat streaming, live radio, inline search & play
- **GUI + webapp playing** — Next.js Mini App: now-playing, queue, search, controls
- **Audio FX** — Slow+Reverb, Nightcore, Bass Boost, 8D
- **Spotify intelligence** — profanity / AI-lyrics filter, auto-skip disliked tracks

## Architecture (one line each)

| Piece | Language | From | Does |
|---|---|---|---|
| `services/core-cpp` | C++20 | anonx-cpp + NoxBeats + TinaMusic | Bot core / router: commands, permissions, routing — owns no download/stream logic |
| `services/vc-go` | Go | YukkiMusic (Go) + ArcMusic + TgMusicBot | VC streaming engine, two profiles: `low-ram` / `high-perf` |
| `services/vc-ts` | TypeScript | TGVCBot + remix + Lemonic | TS VC engine + Lemonic API + inline search |
| `services/vc-rust` | Rust | ParvatiMusic | Rust VC engine (MTProto + ntgcalls), fallback VC |
| `services/audio-fx-rust` | Rust | telegram-music-bot-rust | FFmpeg audio effects |
| `services/share-rust` | Rust | feel-me | Anonymous sharing links |
| `services/queue-rust` | Rust | daslui telegram-music-bot | Crowd-curated voting queue → Spotify playback queue |
| `services/spotify-rust` | Rust | Rustify | Spotify playback watch, lyrics/profanity filter, auto-skip |
| `services/gui-js` | JS/TS (Next.js) | wavetune | Web GUI + Telegram Mini App player |
| `services/radio-js` | JS (Node) | openradiobot | Internet radio stations |
| `services/downloaders` | Python | sanila YT + Spotify-Loader + beetube + Navidrome companion | Download workers (YT/Spotify/Deezer/TikTok/Soulseek) |
| `services/netease-go` | Go | Music163bot-Go | NetEase Cloud Music search/download/lyrics |

All services talk through one contract: `contracts/ipc.json`
(HTTP + JSON, one endpoint per service, `X-Parlay-Token` header).
See [ARCHITECTURE.md](ARCHITECTURE.md) and [PORT_MAP.md](PORT_MAP.md).

## Status

**Scaffold only.** The service directories contain original stubs (a README
plus a `main` that prints its name and exits) — no upstream code was copied
in. Reference repos live under `refs/` (shallow clones, reference-only,
gitignored) so ports can be re-implemented behaviour-by-behaviour against
the IPC contract. See `PORT_MAP.md` for the per-feature source map and the
license warning: most refs are GPLv3/AGPLv3, three have no LICENSE file —
port ideas, not code, until the license question is settled.

## Quick start (scaffold)

```bash
docker compose up --build   # builds the stubs; real services land one by one
# or per service, e.g.:
#   cd services/core-cpp && cmake -B build && cmake --build build
#   cd services/vc-go && go run ./cmd/parlay-vc
#   cd services/vc-rust && cargo run
#   cd services/gui-js && npm install && npm run dev
```

## Configuration

No secrets are committed. Copy the template and fill it in locally:

```bash
cp .env.example .env   # BOT_TOKEN, API_ID, API_HASH, MONGO_URI, etc.
```

Never commit `.env`. See `.gitignore`.

## License

TBD — do not pick one until the GPL/AGPL port question in `PORT_MAP.md`
is settled. Until then this tree is unlicensed scaffold + reference clones.
