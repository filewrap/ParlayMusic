# Parlay Phase 9 — engines port report

Engines half of UI_INVENTORY.md, implemented against the §29 IPC spec
exactly (field names/types as specified). No stubs, no dead strings, no
playback/voice/audio/network-behaviour changes — UI payloads only.
Queue/playback state logic is untouched in every service.

Boundaries kept: `.env` untouched, core never started with the user's
token, nothing pushed, `services/core-cpp/**`, `contracts/ipc.json`
and `COMMANDS.md` untouched (core-cpp worker's parallel changes are
theirs — they show in `git status` but are not mine).

## Test totals

| Service | Before | After | New tests |
|---|---|---|---|
| vc-go | 30 passed / 5 skipped | **37 passed / 5 skipped** | 7 |
| vc-ts | 52 passed | **61 passed** | 9 |
| vc-rust | 50 passed | **59 passed** | 9 |
| downloaders | 42 passed / 8 skipped | **69 passed / 8 skipped** (77 ran) | 27 |
| netease-go | 23 passed | **30 passed** | 7 |
| share-rust | 17 passed | **17 passed** | 0 (format covered by updated assertion) |
| queue-rust | 20 passed | **21 passed** | 1 |
| spotify-rust | 51 passed | **61 passed** | 10 |
| radio-js | 21 passed | **22 passed** | 1 |
| **Total (9 services)** | **306 passed / 13 skipped** | **377 passed / 13 skipped, 0 failed** | **71** |

Phase 7 totals (717 passed / 13 skipped across 12 services) are not
dropped — the 9 engines services went from 306 to 377 passed with the
same 13 skips.

**Flake note (vc-ts, pre-existing, not a regression):** the live-LRCLib
lyrics tests flaked in two mid-work runs (first: 51 pass / 1 fail on
"lyrics by explicit title"; later: 58 pass / 3 fail across the lyrics
tests). LRCLib itself is intermittently timing out from the sandbox
right now — a direct curl timed out at 15 s, then returned 200 on
retry. The pre-existing test now retries 3× like its sibling (test-only
change, same pattern already in the file). Final vc-ts run:
**61/61 pass, 0 fail**. A sandbox restart also killed one vc-ts run
mid-flight; the re-run after it was the clean 61/61.

## Shared formatting decisions (identical in all 5 engines that render them)

- `duration_str`: sec ≤ 0 or live → `"∞"`; < 3600 → `"M:SS"`
  (268 → `"4:28"`); ≥ 3600 → `"Xh Ym Zs"` with zero parts dropped
  (3600 → `"1h"`, 3661 → `"1h 1m 1s"`, 3720 → `"1h 2m"`).
  Asserted identically in vc-go, vc-ts, vc-rust, downloaders, netease-go,
  spotify-rust.
- `upload_time`: yt-dlp `upload_date` `"YYYYMMDD"` → `"YYYY-MM-DD"`;
  anything else passes through unchanged.
- `is_muted`: no engine has a separate mute flag — mute IS volume 0,
  so `is_muted = (volume == 0)` (vc-ts additionally requires
  `muted_volume` set, matching its existing `muted` logic).
- `speed`: always `1.0` (no engine has speed control).
- `is_paused` / `loop_count` / `autoplay_enabled` mirror the legacy
  `paused` / `loop` / `autoplay` fields; legacy fields are kept.

## Per-service changes

### vc-go (37/5)
- `internal/types/types.go`: `Track` gained IPC-3 fields
  (`duration_str`, `views *int64`, `uploader`, `upload_time`,
  `is_live`); `NowPlaying` gained IPC-1 fields (`is_paused`,
  `is_muted`, `loop_count`, `speed`, `autoplay_enabled`); new
  `FormatDurationStr` / `FormatUploadDate` helpers.
- `internal/queue/room.go`: `snapshotLocked()` populates IPC-1 from
  room state.
- `internal/resolver/resolver.go`: `ytDlpJSON` gained `view_count`,
  `upload_date`, `is_live`, `live_status`; new `enrichIPC3()` called
  in `resolveViaYtDlp` and `Related`.
- `internal/server/server.go`: new `GET /v1/progress` (IPC-2; same 404
  rule as now-playing: no room, or room with no track and empty queue).
- `go vet` clean.

### vc-ts (61/0)
- `src/resolvers.js`: `formatDurationStr`, `formatUploadDate`,
  `enrichYtIPC3` (exported); YouTube/autoplay/JioSaavn/Lemonic tracks
  enriched; radio tracks get `duration_str: "∞"`, `is_live: true`;
  new pure `inlineMode(query)` dispatcher (§13).
- `src/main.js`: `nowPlaying()` + `panelState()` gained IPC-1 fields
  (legacy `muted` kept); new `GET /v1/progress` (IPC-2; missing /
  non-integer `chat_id` is 400, no room is 404); `/v1/lyrics` returns
  IPC-5 shape (`track_id` from the room's current track when looked
  up via `chat_id`, else null; `source: "lrclib"`, `truncated: false`);
  `/v1/inline` rewritten with mode dispatch — response stays
  `{results: [Track]}` (no new field invented); panel shuffle typo
  fixed per §27 ("No enough items" → "Not enough items").
- Inline modes: `np`/`s`/`l` return the current track when an optional
  `chat_id` is supplied, else empty; `lib`/`del` empty (no library
  backend — gap 1); `help`/`hint`/bare `search` empty (core renders
  the static inventory text); search path unchanged (YouTube first,
  Lemonic appended, capped at 10), Lemonic results are audio type.

### vc-rust (59/0)
- `Track` gained `thumbnail` + IPC-3 fields; `track_json`,
  `track_canonical_json`, `track_from_json` emit/round-trip them
  (stored tracks without `duration_str` re-derive it).
- `resolve_youtube_url`, `resolve_via_ytdlp`,
  `spotify_track_from_json` (album art), `first_unplayed_entry`
  (autoplay mix) all populate thumbnail + IPC-3 via shared
  `ipc3_from_ytdlp` / `thumbnail_from_ytdlp` helpers.
- `now_playing_json` gained IPC-1 fields; new `progress_json` +
  `GET /v1/progress` (IPC-2 fields at the top level of `data`, same
  shape as vc-go/vc-ts; 404 rule matches them, unlike vc-rust's
  lenient now-playing which returns an empty room with 200).
- `src/voice.rs` test helper `tone_track` updated for the new fields.

### downloaders (69 passed / 8 skipped, 77 ran)
- `src/util.py`: `format_duration_str` / `format_upload_date`;
  `track_dict` now always derives `duration_str` and accepts IPC-3
  params (views > 0 only, uploader, upload_time formatted, is_live).
- `src/ytdlp.py`: `_entry_to_track` maps IPC-3 (keeps legacy `live`
  field); new `parse_ytdlp_progress_line` (pure, tested) and
  `_run_streaming` (`--progress --newline` via Popen + reader thread,
  stderr to a temp file, hard timeout preserved); `download_youtube_audio`
  takes an optional real progress callback.
  **Live smoke verified:** real yt-dlp download of Rick Astley
  streamed 13 progress events (0.1% → 100%, real bytes/speed/ETA);
  the returned track carried `duration_str "3:33"`, views 1833238663,
  uploader "Rick Astley", `upload_time "2009-10-25"`, thumbnail.
- `src/progress.py` (new): thread-safe `ProgressRegistry` /
  `DownloadRun` — IPC-4 snapshot shape, per-track checklist,
  `lossy_available`/`lossy_summary`, `humanize_failure` (ported
  mapping from refs/music-bot-navidrome `reporting.py`), finished-run
  cap 200 (live runs never evicted).
- `src/soulseek/downloader.py`: `wait_for_files` progress/state
  callbacks wired through `_await_one_file` → `_download_chosen` →
  `_try_candidates` into the run (per-track state/pct/peer, overall
  %/bytes folded live); single-track lossy detection calls the real
  `find_lossy_candidates` preview when no FLAC exists; album
  `_attempt` + no-candidate branch update the checklist (no entry
  ever sticks at "searching").
- `src/spotify.py`: `download_spotify_track` passes the yt-dlp
  callback through (+ merges `duration_str`); `download_spotify_collection`
  takes a run and builds/folds the per-track checklist.
- `src/spotify_api.py`: `_track_dict` gained `duration_str`.
- `src/providers.py`: `_run_for` / `_ytdlp_progress_cb` helpers; every
  download handler wires its run (Soulseek sets checklist titles from
  the album/track list; Deezer/TikTok/Navidrome flip phase to
  "downloading" — phase transitions only, see gap 11).
- `src/server.py`: every download POST opens a run (kind from the
  path) and returns `run_id` in the response data; runs finish
  done/failed on every exit path (including raw exceptions, so no run
  ever sticks mid-flight); new `GET /v1/download/progress?run_id=`
  (unknown run → 404).
- New `tests/test_progress.py` (27 tests): formatters, `track_dict`
  IPC-3, entry mapping, progress-line parsing, all 10 failure
  mappings + passthrough, registry shape/cap/concurrency, mocked
  slskd progress folding + lossy fields, HTTP progress route +
  `run_id` in responses + failed-run marking.

### netease-go (30/0)
- `internal/netease/client.go`: `Track` gained `duration_str` (always
  derived in `toTrack`) and `is_live` (never set — NetEase songs are
  not live; views/uploader/upload_time are not reported by the API
  and stay absent); `formatDurationStr` helper; `ErrNoLyrics`
  sentinel (empty lyric → 404, not an engine error); new
  `NewWithBase` constructor (same pattern as `NewWithMusicU`) for
  stub-based tests.
- `internal/server/server.go`: `/v1/netease/lyrics` returns IPC-5
  (`track_id`/`title`/`artist` via `SongDetail`, `source: "netease"`,
  `truncated: false`) and writes the raw LRC to the download dir as
  `netease-<id>.lrc`, returning `lrc_path` (flagged in gaps — not in
  §29). No-lyrics is now 404 `not_found` (was 502).

### share-rust (17/0)
- Link format changed per inventory §22: `?start=share_<token>` →
  `?start=share-<token>` (token is 16 alphanumerics, so the hyphen is
  unambiguous). Doc comment + round-trip test updated. No other
  change; feedback/report dedup is a gap (8), not implemented.

### queue-rust (21/0)
- No code change needed: `queue.vote` already returns the full
  contract `{queued, approvals, declines, threshold, track_id}`.
  Added `vote_response_carries_exact_contract_fields` — asserts the
  exact 5-key field set plus the values across approve/decline/
  re-vote sequences.

### spotify-rust (61/0)
- `src/common.rs`: added `Json::as_f64`.
- `src/main.rs`: `profanity_words` (all matches, deduped, in order;
  `profanity_check` now delegates, still first-match);
  `format_duration_str` + `duration_str` on `spotify_track_to_json`;
  `audio_features_to_json` (key int → note name, mode → Major/Minor,
  key −1 → "Unknown", non-object → None so `features` is null when
  the deprecated endpoint 403s); `artist_genres` (dedup, first-seen
  order); `fetch_genius_lyrics_with_url` + `fetch_lyrics_with_url`
  (Genius page URL threaded through as `lyrics_link`; skip path
  unchanged); `analysis_text` (verdict + provider + detail).
- New `GET /v1/spotify/details?user_id=&track_id=` (IPC-7):
  `{track, features, genres, disliked_by, ignored_by: 0, profanity,
  language: null, lyrics, lyrics_link}`. `disliked_by` counts users
  whose dislike set contains the track. `user_id` accepted, not
  needed. 502 naming the credentials when they are absent.
- New `POST /v1/spotify/analyze` (IPC-7): `{analysis,
  profane_words}` — 404 `not_found` when no lyrics exist ("🚫 Lyrics
  not found"), classifier run for the analysis sentence.
- Handler tests without creds are deterministic (fail before any
  network call); pure-mapper tests cover features/genres/profanity/
  duration/analysis-text.

### radio-js (22/0)
- `src/main.js`: `nowPlaying()` gained IPC-1 fields (constants —
  a station stream is never paused/muted/looped, no speed control,
  no autoplay) and the track gained IPC-3 `duration_str: "∞"` /
  `is_live: true`; `/v1/radio/status` gained the §21 `/manage` card
  fields radio-js honestly knows (`started_at_ms`, `station_name`,
  `listeners: 0`, `total_listeners: 0`, `loop_type: "none"`,
  `autoplay_enabled: false`, `queue_length: 0`). Start responses
  carry the same now-playing shape automatically.

## Gaps

All gaps (including fields the spec does not define) are in
[port-engines-gaps.md](sandbox://workspace/parlay/phase9/port-engines-gaps.md):
lib/del inline modes (no library backend); `chat_id` needed for
np/s/l inline; `l` lyrics article fetch; NetEase `lrc_path` (not in
IPC-5); Spotify `ignored_by` / `language` / `features` (deprecated);
share feedback/report dedup; radio listener counts; `run_id` in
download responses (not in §29); granular progress is yt-dlp /
Soulseek only.
