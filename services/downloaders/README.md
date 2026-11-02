# downloaders — Python download workers

From: sanila2007 Telegram-Song-Downloader-Bot (`bot.py`, free YouTube) +
xgorn Spotify-Loader (`bot.py`, Spotify playlist/album/track → ZIP or
one-by-one) + kodjunkie beetube-bot (`providers/*.js` registry: music,
movies, anime, torrents) + eeegoloauq music-bot (`src/navidrome.py`,
`src/soulseek/*`, `src/metadata/*` — Navidrome companion).

Behaviour is re-implemented (not copied) behind Parlay's IPC contract
(`contracts/ipc.json`). One HTTP surface: `download.youtube/spotify/
spotify_playlist/deezer/tiktok/navidrome` + `search`. Provider registry
pattern from beetube: each provider is one handler in `src/providers.py`;
music is default, others behind a flag. Files over the 50 MB Bot API cap
return `oversize: true` and are queued for a later MTProto pass — never
failed.

## Run

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # yt-dlp (spotdl optional)
PORT=8010 PARLAY_TOKEN=... PARLAY_DL_DIR=./downloads \
  .venv/bin/python -m src.main
```

Stdlib-only HTTP server (`http.server.ThreadingHTTPServer`); the only
non-stdlib piece is the `yt-dlp` binary invoked as a subprocess.
`ffmpeg` must be on PATH for mp3 extraction.

## Endpoints

| Route | Body | Returns |
|---|---|---|
| `GET /healthz` | — | `{ok, service}` (no auth) |
| `POST /v1/download/youtube` | `{query, requested_by}` | `{file_path, track, size_bytes, oversize}` |
| `POST /v1/download/spotify` | `{url, requested_by}` | same as above (track `source=spotify`) |
| `POST /v1/download/spotify-playlist` | `{url, mode: zip\|files, requested_by}` | `{count, file_path\|file_paths, size_bytes, oversize, truncated}` |
| `POST /v1/search` | `{query, source: youtube\|spotify\|all, limit}` | `{results: [Track]}` |
| `POST /v1/download/deezer` | `{query\|url, requested_by}` | `{file_path, track}` (single: 30 s preview mp3; album: ZIP of previews, `count`) |
| `POST /v1/download/tiktok` | `{url, requested_by}` | `{file_path, track, size_bytes, oversize}` (audio mp3) |
| `POST /v1/download/navidrome` | `{query\|url, requested_by}` | `{file_path, track, size_bytes, oversize, imported:true}` (original file) |

Auth: `X-Parlay-Token` on every POST when `$PARLAY_TOKEN` is set.
Envelopes per `contracts/ipc.json`: `{ok:true, data:{...}}` /
`{ok:false, error:{code, message}}`.

## Spotify Web API (Phase 4)

`src/spotify_api.py` — real client-credentials flow (same as
`spotify-rust`): `POST accounts.spotify.com/api/token` with Basic auth,
then `GET api.spotify.com/v1/...` with a Bearer token, token cached in
memory. Env: `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET`. When both
are set, `spotify.py` resolves tracks/playlists/albums/artists from
the API (real track lists, real titles/durations) and `/v1/search`
`source=spotify` uses real Spotify search. When they are absent the
old yt-dlp/oEmbed fallback runs and every track carries a `note`
saying so — a track list is never fabricated.

## Deezer / TikTok / Navidrome (Phase 4)

- **Deezer** (`src/deezer.py`) — public API `api.deezer.com`, no auth
  (same shape as music-bot-navidrome `src/metadata/deezer.py`).
  yt-dlp refuses Deezer outright ("[DRM] The requested site is known
  to use DRM protection") — proven 2026-11-02 — so the download is the
  30-second preview MP3 from the API; the track `note` says so, and a
  full track needs a Deezer account (ARL cookie, not held here). Album
  input → ZIP of all previews. Live-tested 2026-11-02: search +
  preview download, Daft Punk *Discovery* album → 14-preview ZIP.
- **TikTok** (`src/tiktok.py`) — yt-dlp `-J` metadata + best-audio
  URL (works), then the bytes via plain urllib GET of the signed CDN
  URL: yt-dlp's own downloader stalls on reads from this egress proxy
  ("The read operation timed out", retries exhausted — proven
  2026-11-02), while the same URL downloads whole in ~2 s via curl
  (711,792-byte 128 kbps mp3). Falls back to native yt-dlp download
  if the direct fetch fails. Live-tested 2026-11-02.
- **Navidrome** (`src/navidrome.py`) — Subsonic API (`rest/ping`,
  `rest/search2`/`search3`, `rest/download` original file), token
  auth with plain-password fallback (the public demo rejects token
  auth with 403 but accepts `p=` — proven 2026-11-02). Env:
  `NAVIDROME_URL` / `NAVIDROME_USER` / `NAVIDROME_PASSWORD`; missing
  vars answer `engine_unavailable` naming them. Live-tested
  2026-11-02 against demo.navidrome.org: search → download,
  5,365,035-byte mp3.

## Implemented vs stubbed

Implemented (live-tested 2026-11-02, see PROGRESS.md):

- **YouTube download** — query or URL → `yt-dlp` bestaudio → mp3 via
  ffmpeg. Search uses `ytsearch`; downloads retry with the android
  player client when YouTube's web client answers "Sign in to confirm
  you're not a bot" (happens from this sandbox egress IP; no cookies
  or credentials involved).
- **YouTube search** — `yt-dlp --flat-playlist ytsearchN:` → `Track`
  list (`id, title, source, artist, duration_sec, url, thumbnail`).
- **Spotify track** — URL parsed (track/album/playlist/artist, web or
  `spotify:` URI); metadata from the real Web API when
  `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET` are set, else the public
  embed page (`__NEXT_DATA__`), else oEmbed; audio matched by
  searching YouTube for "<artist> <title>" (the same fallback
  Spotify-Loader gets from `spotdl --use-youtube`). If `spotdl` is
  installed it is used instead — it is optional, the service never
  fails for its absence.
- **Spotify playlist/album** — real track list from the Web API when
  creds exist, else the public embed page (50 tracks for "Today's Top
  Hits", live-tested); each track downloaded, then ZIP (`mode=zip`,
  Spotify-Loader's "Send as ZIP") or file list (`mode=files`, "Send
  one by one"). One bad track never kills the whole playlist. Capped
  at 50 tracks (`truncated`).
- **Search `source=spotify`** — real Spotify search when creds exist,
  else YouTube matches marked `source=spotify` with an honest `note`.
- **Deezer** — public API search/track/album; 30 s preview mp3
  download (yt-dlp refuses Deezer: DRM); album → ZIP of previews.
- **TikTok** — yt-dlp metadata + direct CDN download of the audio mp3
  (yt-dlp's own downloader stalls on reads from this egress).
- **Navidrome** — Subsonic search + `rest/download` of the original
  file; `NAVIDROME_URL`/`NAVIDROME_USER`/`NAVIDROME_PASSWORD` env.

Stubbed: nothing. All five providers are real (see the Phase 4
sections above). Soulseek-via-slskd (Navidrome's lossy-download path)
is a separate opt-in job — it needs its own slskd daemon.

## Notes / limits

- YouTube search is fast (~3 s) but video extraction is slow from this
  sandbox (~15–25 s per track) and the web player client is bot-gated;
  the android-client retry is what makes downloads work here.
- Spotify playlist/album track lists resolve without credentials too
  (public embed page, `__NEXT_DATA__`); with credentials they come
  from the Web API. yt-dlp's Spotify extractor returns nothing for
  these URLs here (DRM refusal), and oEmbed only gives the page title
  — both are now last-resort fallbacks, not the path.
- Spotify downloads match each track to a song-length YouTube result
  and refuse to download a multi-hour compilation or livestream
  (song-length guard, 20 min default, `PARLAY_SPOTIFY_MAX_MATCH_SEC`),
  failing fast with a clear error instead of downloading hundreds of
  MB.
- Downloads land in `$PARLAY_DL_DIR` (default `./downloads`), one
  `job-<id>/` subdir per download; filenames are sanitized (no `..`,
  no absolute paths, no separators).
