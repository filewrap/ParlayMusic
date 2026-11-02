# spotify-rust — Spotify watch + profanity filter

From: vtvz/rustify (`src/{profanity,lyrics/{genius,lrclib,musixmatch},
spotify,queue}.rs`). Monitors Spotify playback, fetches lyrics, detects
profane / AI-generated lyrics, auto-skips tracks the user disliked.
`spotify.watch` + `spotify.skip-check` IPC, plus `spotify.dislike`,
`spotify.resolve`, `spotify.watches` (all three added to
`contracts/ipc.json`).

## Implemented (real)

- `POST /v1/spotify/watch` `{action:start|stop, user_id}` — in-memory
  watch registry, persisted to `data/spotify-state.json` (atomic write,
  restored on restart; `PARLAY_DATA_DIR` overrides the data dir).
  `GET /v1/spotify/watches` lists watched user ids.
- `POST /v1/spotify/dislike` `{action:add|remove, user_id, track_id}` —
  per-user disliked-track store, persisted with the watch registry.
- `POST /v1/spotify/skip-check` `{track:{id,title,artist}, user_id?}` →
  `{skip, reason, matched_word?, disliked_by?, lyrics_checked,
  lyrics_available, lyrics_source}`. Decision order (rustify
  `src/queue/track_check.rs`): disliked → profanity in title/artist →
  profanity in LRCLib lyrics. `reason` is `profanity | disliked | null`.
- Profanity filter: curated 54-word English list in code (the wordlist
  concept from rustify `src/profanity.rs`, which uses the rustrict trie;
  re-implemented std-only), case-insensitive, word-boundary match over
  title + artist + lyrics, reports the matched word. Mild words (hell,
  damn, goddamn, crap, jesus, queer) are deliberately excluded — they
  appear in clean classics ("Imagine" — "no hell below us"; "Highway to
  Hell") and flagging those is worse than missing a mild word.
  Obfuscated spellings (l33t, spaced letters) are NOT caught.
- Lyrics check: **real** LRCLib fetch (`https://lrclib.net/api/search`,
  via a `curl` subprocess, 5 s cap) and profanity-checked. When LRCLib
  is unreachable or has no lyrics, the response says
  `lyrics_available:false, lyrics_checked:false` and falls back to the
  title/artist check — a lyrics verdict is never faked.
- `POST /v1/spotify/resolve` `{url}` — parses
  `open.spotify.com/<track|album|playlist|artist>/<id>` (query/fragment
  tolerated) and `spotify:<kind>:<id>` into `{kind, track_id, url,
  source:"spotify"}`. With `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET`
  set it also enriches the response from the real Web API: a `track`
  object (title/artist/duration/album/cover) for tracks, or `name` +
  `total` for albums/playlists/artists (`api:"spotify-web-api"`).
  Without credentials it stays parse-only and says so
  (`api:"none"`, `note`); ids are case-sensitive and preserved
  exactly.
- `POST /v1/spotify/collection` `{url}` (Phase 4) — real track list
  for an album/playlist/artist URL from the Web API
  (`{kind, id, name, total, truncated, tracks, api:"spotify-web-api"}`).
  Playlists paginate (100/page, capped at 100); albums come back
  whole; artists return top tracks (max 10). Without credentials it
  answers 503 `engine_unavailable` and points at the downloaders
  service fallback — a track list is never fabricated.
- `GET /v1/spotify/status` (Phase 4) — `{credentials_configured,
  token_cached, api}` (no secrets in the response).
- Spotify Web API client (Phase 4): client-credentials flow
  (`POST accounts.spotify.com/api/token`, Basic auth, token cached in
  memory until ~60 s before expiry, never written to disk) over a
  `curl` subprocess, std-only like the LRCLib fetch.
- Envelopes `{ok:true,data}` / `{ok:false,error:{code,message}}`;
  `X-Parlay-Token` auth when `PARLAY_TOKEN` is set (401 without/wrong,
  `/healthz` open).

## NOT implemented — needs credentials / a classifier

- **Real Spotify playback monitoring.** The watch registry records
  intent only; nothing polls the Spotify Web API for a user's
  currently-playing track. Live monitoring needs a per-user OAuth
  token (rustify `src/spotify/auth.rs`), then a poller that feeds the
  currently-playing track into skip-check and issues a skip to the VC
  engine. Without them: watch start/stop, dislikes, and skip-check all
  still work standalone.
- **Genius / Musixmatch lyrics** — need `GENIUS_ACCESS_TOKEN` /
  `MUSIXMATCH_API_KEY`. Only LRCLib (no key) is wired.
- **AI-generated-lyrics detection** — rustify flags tracks whose lyrics
  look AI-written; there is no classifier in this service, so
  `reason:"ai-generated"` is **never emitted**. The value stays in the
  contract for a future detector.

## Run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release --offline   # std-only, zero crates
PORT=8007 ./target/release/parlay-spotify
```

Env: `PORT` (default 8007), `PARLAY_TOKEN` (unset = no auth, dev only),
`PARLAY_DATA_DIR` (default `./data`).
