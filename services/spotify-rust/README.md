# spotify-rust — Spotify watch + profanity filter

From: vtvz/rustify (`src/{profanity,lyrics/{genius,lrclib,musixmatch},
spotify,queue}.rs`). Monitors Spotify playback, fetches lyrics, detects
profane / AI-generated lyrics, auto-skips tracks the user disliked.
`spotify.watch` + `spotify.skip-check` IPC, plus `spotify.dislike`,
`spotify.resolve`, `spotify.watches` (all three added to
`contracts/ipc.json`).

## Implemented (real)

- `POST /v1/spotify/watch` `{action:start|stop, user_id}` — in-memory
  watch registry, persisted to the shared durable store (Phase 7,
  A11: `MONGO_URI` > `REDIS_URL` > `data/spotify-state.json`, atomic
  write, restored on restart; `PARLAY_DATA_DIR` overrides the data
  dir). `GET /v1/spotify/watches` lists watched user ids.
- `POST /v1/spotify/dislike` `{action:add|remove, user_id, track_id}` —
  per-user disliked-track store, persisted with the watch registry.
- `POST /v1/spotify/skip-check` `{track:{id,title,artist}, user_id?}` →
  `{skip, reason, matched_word?, disliked_by?, lyrics_checked,
  lyrics_available, lyrics_source, classifier_provider,
  classifier_verdict, classifier_confidence, classifier_detail}`.
  Decision order (rustify `src/queue/track_check.rs`): disliked →
  profanity in title/artist → profanity in LRCLib lyrics →
  AI-generated lyrics (Gemini, confidence ≥ 0.7). `reason` is
  `profanity | disliked | ai-generated | null`.
- Gemini classifier (Phase 5): when `GEMINI_API_KEY` is set, lyrics
  (or title/artist when there are no lyrics) are classified by the
  Gemini API (`gemini-2.0-flash` by default; `GEMINI_MODEL` /
  `GEMINI_API_BASE` override) into `clean | profanity | ai_generated |
  unknown`, and the Gemini verdict replaces the wordlist verdict. The
  Gemini call failing falls back to the wordlist and says so
  (`classifier_provider:"wordlist-fallback"`). Without a key AND
  without lyrics the verdict is honestly `unknown`
  (`classifier_provider:"none"`) — a verdict is never fabricated.
  AI-generated skips only at confidence ≥ 0.7 (`AI_SKIP_CONFIDENCE`);
  below that it is reported but does not skip.
- Per-user OAuth + polling (Phase 5, port of rustify
  `src/spotify/auth.rs`): `POST /v1/spotify/auth`
  `{action:login|callback|refresh|revoke, user_id, code?}` — login
  returns the authorize URL (scopes `user-read-currently-playing
  user-read-playback-state` only; Parlay never controls the user's
  player), callback exchanges the code and stores the tokens,
  refresh rotates them, revoke drops them.
  `GET /v1/spotify/now-playing?user_id=` returns the last polled
  state. A poller thread (every `SPOTIFY_POLL_INTERVAL_SECS`,
  default 30; 0 disables) refreshes tokens within 120 s of expiry,
  fetches currently-playing for every watched user with a token, and
  runs the skip decision on track changes. `GET /v1/spotify/status`
  now also reports `{user_tokens, watching, gemini_configured,
  poll_interval_secs, polling}`.
- Per-user token store (Phase 5): `spotify-user-tokens.json` holds
  one record per user — `{user_id, nonce, ciphertext, tag,
  expires_at}`, all hex. Encryption is AES-256-GCM (96-bit random
  nonce per record, 128-bit tag, AAD
  `parlay:spotify-user-token:v1:<user_id>`), hand-rolled std-only
  (`src/crypto.rs`, verified against NIST AES-256-GCM vectors).
  The 32-byte key is generated once from `/dev/urandom` at first
  run and stored in `spotify-user-key` (mode 0600); there is no
  hardcoded default key. Plaintext tokens are never on disk;
  undecryptable records (key rotated) are skipped with a warning.
  This is the template for any other per-user credential: same
  scheme, same key-file discipline, per-record nonce + AAD.
- Profanity filter: curated 54-word English list in code (the wordlist
  concept from rustify `src/profanity.rs`, which uses the rustrict trie;
  re-implemented std-only), case-insensitive, word-boundary match over
  title + artist + lyrics, reports the matched word. Mild words (hell,
  damn, goddamn, crap, jesus, queer) are deliberately excluded — they
  appear in clean classics ("Imagine" — "no hell below us"; "Highway to
  Hell") and flagging those is worse than missing a mild word.
  Obfuscated spellings (l33t, spaced letters) are NOT caught.
- Lyrics check: **real** provider-chain fetch (Genius → Musixmatch →
  LRCLib, via a `curl` subprocess, 5 s cap) and profanity-checked.
  Genius uses the public search API (`api.genius.com/search`,
  `GENIUS_ACCESS_TOKEN`) + a song-page scrape of the
  `data-lyrics-container` divs; Musixmatch uses
  `matcher.lyrics.get` (`MUSIXMATCH_API_KEY`); LRCLib needs no key.
  When no provider is reachable or has lyrics, the response says
  `lyrics_available:false, lyrics_checked:false` and falls back to the
  title/artist check — a lyrics verdict is never faked. Without the
  keys the chain is LRCLib only.
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

## Wired, key-gated — only the credentials are missing

- **Genius / Musixmatch lyrics (Phase 5).** Both providers are wired
  against the public APIs, env-gated by `GENIUS_ACCESS_TOKEN` /
  `MUSIXMATCH_API_KEY`, falling back to LRCLib; only the keys are
  missing. (This line used to say only LRCLib was wired — superseded
  2026-11-03.)

## NOT implemented

- **Skip execution.** The poller decides skips and records them; no
  code issues a skip to the VC engine yet — issuing the skip is a
  core-cpp action outside this service.

## Run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release --offline   # std-only, zero crates
PORT=8007 ./target/release/parlay-spotify
```

Env: `PORT` (default 8007), `PARLAY_TOKEN` (unset = no auth, dev only),
`PARLAY_DATA_DIR` (default `./data`),
`GEMINI_API_KEY` (unset = wordlist-only classifier),
`GEMINI_MODEL` (default `gemini-2.0-flash`),
`GEMINI_API_BASE` (default `https://generativelanguage.googleapis.com`),
`SPOTIFY_POLL_INTERVAL_SECS` (default 30; 0 disables polling),
`SPOTIFY_REDIRECT_URI` (default `http://localhost:8080/callback`),
`SPOTIFY_ACCOUNTS_URL` / `SPOTIFY_API_BASE` (test overrides for the
stubbed Spotify endpoints; production defaults are
`accounts.spotify.com` / `api.spotify.com`).
