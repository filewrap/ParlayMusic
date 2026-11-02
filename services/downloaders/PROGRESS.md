# downloaders — PROGRESS (Worker D, 2026-11-02)

## What was built

Real Python service in `services/downloaders/` (was a 3-line stub):

- `src/main.py` — entry point (`PORT`, `PARLAY_TOKEN`, `PARLAY_DL_DIR`).
- `src/server.py` — stdlib-only `ThreadingHTTPServer`; routes, envelopes
  per `contracts/ipc.json`, `X-Parlay-Token` auth when `PARLAY_TOKEN` set.
- `src/ytdlp.py` — yt-dlp subprocess wrapper (search via `ytsearchN:`,
  bestaudio → mp3 via ffmpeg, flat info extract).
- `src/spotify.py` — Spotify URL parsing, metadata via yt-dlp flat
  extract + public oEmbed fallback, YouTube-match download, ZIP/files
  playlist flow (Spotify-Loader behaviour), song-length match guard.
- `src/providers.py` — beetube-style provider registry
  (youtube, spotify real; deezer/tiktok/navidrome `engine_unavailable`).
- `src/util.py` — envelopes, error codes, filename sanitizing, Track map.
- `requirements.txt` — `yt-dlp>=2024.1.7`; spotdl commented out (optional).
- `README.md` — implemented vs stubbed.
- `.venv/` — venv with yt-dlp installed (not committed).

## Environment

- Python 3.12.3, ffmpeg present (`/usr/bin/ffmpeg`).
- venv: `python3 -m venv services/downloaders/.venv`
- install: `.venv/bin/pip install --upgrade pip yt-dlp`
  → **yt-dlp 2026.10.26 installed OK** (PyPI reachable via proxy).
- spotdl NOT installed (optional by design; fallback path is what runs).

## Smoke tests (all run 2026-11-02 ~19:15–19:50 IST)

Server started with:
`PORT=8010 PARLAY_DL_DIR=/tmp/parlay-dl-test .venv/bin/python -m src.main`

1. `GET /healthz` → `{"ok": true, "service": "downloaders"}` ✓
2. `POST /v1/search {"query":"test","source":"youtube","limit":2}`
   → 2 real YouTube tracks (e.g. `7-boeOYXsUU`, "100 Funniest Kids Test
   Answers", 1069s) in ~3s ✓ (first attempt once took >60s — transient
   slow YouTube read; timeout raised to 150s afterwards)
3. `POST /v1/search {"query":"rick astley never gonna give you up",
   "source":"spotify","limit":2}` → 2 tracks with `source=spotify` and
   honest `note` (matched via YouTube, no Spotify API creds) ✓
4. `POST /v1/download/youtube {"query":"https://www.youtube.com/watch?v=jNQXAC9IVRw"}`
   → `Me_at_the_zoo [jNQXAC9IVRw].mp3`, **230,602 bytes, oversize=false**,
   track `Me at the zoo / jawed / 19s`, ~15–23s ✓
   - First attempt FAILED: YouTube web client answered "Sign in to
     confirm you're not a bot" from this egress IP. Fixed by retrying
     with the android player client (`--extractor-args
     youtube:player_client=android`); no cookies/credentials used.
5. `POST /v1/download/spotify {"url":"https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"}`
   → `Rick_Astley_-_Never_Gonna_Give_You_Up... [dQw4w9WgXcQ].mp3`,
   **7,045,581 bytes, oversize=false**, track id kept as the Spotify id,
   resolved via oEmbed, ~24s ✓
6. `POST /v1/download/spotify-playlist {"url":"https://open.spotify.com/playlist/37i9dQZF1DX4WYpdgoIcn6","mode":"zip"}`
   → first run: curl timed out at 240s; server-side the playlist-title
   fallback had matched a multi-hour compilation and was downloading a
   285 MB mp4. Fixed with a song-length match guard (20 min default):
   now fails fast in ~4s with
   `download_failed: no song-length YouTube match for 'Chill Hits': all
   5 matches are long compilations/livestreams (shortest 3663s)`.
   Playlist ZIP path itself (zip creation, mode=files) is therefore
   **NOT end-to-end verified** — no resolvable multi-track playlist
   exists without Spotify credentials/spotdl. Code path is exercised
   only up to the guard.
7. `POST /v1/download/deezer` → `engine_unavailable` (stub, as designed) ✓
8. Bad input: `{}` to youtube → `bad_request: query is required` ✓;
   `GET /nope` → `not_found` ✓
9. Auth (separate instance on :8011 with `PARLAY_TOKEN=test-token-123`):
   no token → 401 `unauthorized`; wrong token → 401; right token → 200;
   `/healthz` without token → 200 ✓

## Real vs stubbed

> **SUPERSEDED (Phase 4/5, 2026-11-03).** The "STUBBED" line below is
> the Phase 3 state — deezer, tiktok and navidrome are real since the
> "Phase 4" section below (public-API previews / direct CDN audio /
> Subsonic download), Deezer full tracks are env-gated on `DEEZER_ARL`
> and Soulseek is real via slskd (Phase 5). See `README.md`
> "Implemented vs stubbed" for current status.

- REAL: YouTube search + download (mp3), Spotify track resolve +
  download, Spotify search marking, playlist endpoint mechanics
  (resolve → per-track download → zip/files, 50-track cap, per-track
  failure tolerance) with the track-list limitation below, auth,
  envelopes, filename sanitizing, oversize flag.
- STUBBED (route exists, honest `engine_unavailable`): deezer, tiktok,
  navidrome downloads.
- KNOWN LIMIT: Spotify playlist/album/artist track lists cannot be
  resolved here — yt-dlp's Spotify extractor returns nothing for those
  URLs and oEmbed gives only the page title. Full playlist support
  needs Spotify API credentials or spotdl installed. The ZIP/files
  assembly code is written but not end-to-end verified for that reason.
- No push made. No tokens/secrets in any file (test token
  `test-token-123` was passed via env only, never written to disk).
- Test servers on :8010/:8011 stopped; `/tmp/parlay-dl-test` removed.

## Phase 4 (Worker D, 2026-11-02) — deezer/tiktok/navidrome killed, real Spotify API

- `src/spotify_api.py` (new) — real client-credentials flow (urllib):
  token endpoint + `api.spotify.com/v1/` Bearer calls, in-memory token
  cache. `spotify.py` uses it for track/album/playlist/artist resolve
  when `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` are set; otherwise
  the fallback runs and every track carries a `note` saying so.
  `providers.search` uses real Spotify search when creds exist.
- Embed fallback (no creds): Spotify's public embed page
  (`__NEXT_DATA__` → `entity.trackList`) now resolves playlist/album
  track lists for real — yt-dlp refuses Spotify URLs outright
  ("[DRM] ... will NOT be supported", proven 2026-11-02), so the old
  flat-extract path is dead and oEmbed alone gave 1 track. Live:
  "Today's Top Hits" → 50 real tracks.
- `src/deezer.py` (new) — public API `api.deezer.com` (search/track/
  album + paginated album tracks); yt-dlp refuses Deezer (DRM) so the
  download is the 30 s preview MP3 (urllib); album → ZIP of previews.
- `src/tiktok.py` (new) — yt-dlp `-J` metadata + best-audio URL, then
  the bytes via plain urllib GET: yt-dlp's own downloader stalls on
  reads from this egress ("The read operation timed out", retries
  exhausted), while the same signed URL downloads whole in ~2 s
  (711,792-byte mp3). Native yt-dlp download kept as fallback.
- `src/navidrome.py` (new) — Subsonic API (ping/search2/search3/
  download original file), token auth with plain-password fallback
  (the public demo rejects token auth with 403 but accepts `p=`).
  Env `NAVIDROME_URL/USER/PASSWORD`; missing vars →
  `engine_unavailable` naming them.
- `src/providers.py` — all five providers real; `search` gains
  `source=deezer` (and real Spotify search when creds exist).
- `tests/test_providers.py` (new) — 21 tests: 13 unit (parsing,
  API-shape mapping, embed parsing, Navidrome auth shape, audio pick)
  + 8 live (gated `PARLAY_LIVE_TEST=1`; Navidrome live needs
  `NAVIDROME_*` env, Spotify API live needs `SPOTIFY_*` env).
  Live run 2026-11-02 ~16:00 IST: 21 tests, OK (1 skipped — Spotify
  API, no creds in sandbox). YouTube 7,045,581 B; Spotify track
  7,045,581 B; Spotify playlist 50 tracks; Deezer preview 479,827 B;
  Deezer album ZIP 14 previews / 6,696,088 B; TikTok 711,792 B;
  Navidrome 5,365,035 B (demo.navidrome.org, server 0.64.2).
- HTTP smoke 2026-11-02 (PORT 18010): /healthz, /v1/search
  (deezer + spotify sources), /v1/download/{deezer,tiktok,navidrome,
  spotify} all `{ok:true}` with real files on disk. No regressions:
  YouTube + Spotify single-track flows unchanged.
