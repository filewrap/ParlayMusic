# Wave 2 — Worker B report: Downloads + Library + Search + AudD Recognize

**Date:** 2026-11-03 ~17:30 IST. **Worker:** B (Downloads + library + search + AudD recognize).
**Repo:** `~/workspace/parlay/` (main at `3d84fb6`; all Wave 2 backend-go work is still untracked — no commit/push, per brief).
**Status: DONE, all owned packages green.** `go vet` clean, `gofmt` clean, **75 tests passing / 0 failing** across the four owned packages (download 37, library 10, search 15, recognize 13). Pre-existing packages (model, config, resolver, store, uds, voice) still pass. `.env` untouched; all tests use dummy creds only; no service started with the user's token.

## What was built

| Package | Files | Actions registered |
|---|---|---|
| `internal/download/` | `download.go`, `handlers.go`, `registry.go`, `prefs.go`, `spotify_playlist.go`, `soulseek.go`, `mix.go`, `playfile.go` | `download.youtube/deezer/tiktok/navidrome/spotify/spotify_playlist/soulseek`, `download.progress`, `download.cancel` (E1), `download.prefs` (E9), `mix.generate` (E3), `vc.play_file` (E12) |
| `internal/library/` | `library.go`, `delete.go`, `scan.go`, `sharescan.go`, `cache.go` | `library.delete` (E2), `library.scan` (E5), `library.sharescan` (E6), `cache.clear` (E7) |
| `internal/search/` | `search.go`, `query.go`, `inline.go`, `lemonic.go` | `search.query`, `search.inline`, `lemonic.search`, `lemonic.download` |
| `internal/recognize/` | `recognize.go` | `audio.recognize` (E4, AudD) |
| `internal/model/model.go` | `Track` only | added `cached_file_id` / `size_bytes` (E10/E11), no existing field changed |
| `internal/config/config.go` | `BackendConfig` only | appended `AuddAPIToken` + `AUDD_API_TOKEN` env mapping |
| `.env.example` | — | appended `AUDD_API_TOKEN=` (empty, documented) + `AUDD_API_BASE` (test override) |
| `contracts/uds-protocol.md` | — | §4 now 65 actions (rows 57–65) + new §4.1 documenting all 9 OD-3 actions with shapes matching the handlers |

Every handler is `func(ctx, req uds.Request) (any, error)` via `func Register(s *uds.Server, d *app.Deps)` per the Wave 2 contract. Missing-credential / key-gated failures are honest errors, never fake data. No stubs, no mocks in production code, no TODO/FIXME/HACK, no silent error drops.

## Per-action status

**Downloads** — all 7 providers route through `d.Registry.Download` with progress callbacks; every `download.*` POST opens a run before provider work (run_id rides back); partial files deleted on failure; files over the 50 MB Bot API cap report `oversize:true`, NOT failed. `download.progress` snapshots a run (unknown → `not_found`, missing id → `bad_request`); `download.cancel` cancels live runs via context (`{cancelled:true, phase:"failed"}`) and answers honestly for finished runs (`{cancelled:false, phase:"done"|"failed"}`, 200). Async runs return `{run_id, phase:"searching"}` immediately and fire `DownloadReady` on success (sync POSTs never fire it). `download.spotify_playlist`: 50-track cap, zip/files modes, per-track checklist. `download.soulseek`: single + album, per-track `[title, reason]` failures.

**`download.prefs` (E9)** — GET returns defaults (mp3, `use_youtube:true`, `"{artist} - {title}.{ext}"`), never 404; POST validates (6 formats; template must contain `{ext}`, no separator/`..`, ≤150 chars; empty coerces to default). Enforcement is honest: non-mp3 format → `engine_unavailable` on daemon-backed downloads (daemon extracts mp3 only); `use_youtube:false` → Spotify downloads fail honestly.

**`mix.generate` (E3)** — explicit query → resolve; else the room's current track as seed (nothing playing → `bad_request` "Nothing is playing…"); non-YouTube seed → 502. Mix = YouTube `RD<id>` via the yt-dlp daemon, ≤10 tracks, excluding seed + room queue + **autoplay history** (`RoomState.AutoplayHistory` — available now that the real room state is in-tree; the earlier "unavailable" note is superseded).

**`vc.play_file` (E12)** — path (not bytes), validated under the data dir, `file:<file_id or basename-hash>`, `source:"file"`, play-now-if-idle else enqueue. **Now delegates to the real `room.Manager`** (`Play`/`Snapshot`/`ClearCurrent`) + voice Manager (join first, then room transition, then the interim ffmpeg feed on the local path; a failed feed rolls the room back to idle). The earlier store-direct adapter was replaced when Worker A landed the real room Manager — writing store buckets directly would have left the Manager's in-memory cache stale. **⚠️ DUPLICATION — see §"Needs a ruling" below.**

**Library** — in-memory shared index (`SharedIndex()`; download + lemonic.download record every finished file with disk-verified size). `library.delete`: record AND file, `..`/top-level-dir refused, stale-record and orphan-file cases both `{deleted:true}`. `library.scan`: Subsonic `startScan` (token auth, password fallback, mirroring the resolver), env-unset → `200 {triggered:false, error:"not configured"}`, wrong creds → `200 {triggered:false, error:"wrong credentials"}`, unreachable → 502; then a bounded library recount (`tracks_found`). `library.sharescan`: `POST /api/v0/options/shares/scan`, slskd down → 502 naming `SLSKD_URL`, 409 → `200 {triggered:false}` with `scheduled_in_min` omitted (the API exposes no minute estimate — none invented). `cache.clear`: scope track/all, unknown name → `200 {cleared:false}` ("Song not cached"), bad scope → `bad_request`; clears the record AND its file; never touches the Phase 4 URL cache.

**Search** — `search.query` fans out across youtube, jiosaavn, lemonic, spotify, deezer, netease (20 s cap; per-source `source_status` ok/empty/failed; all-fail → 502 naming each source; a credential-less source fails alone, never fakes another source's results). `search.inline` mirrors vc-ts `inlineMode` exactly (np/s/l/lib/del/help/search/hint); lib/del now return real library entries with `file_path` + `size_bytes` + `cached:true` (vc-ts returned `[]` here); search mode = YouTube first + Lemonic appended, sliced to 10, never errors. `lemonic.search/download` port vc-ts exactly (unauthenticated first, Bearer key when set, Spotify client-credentials fallback mapped to `lemonic:<spotify-id>`; download fetches to `data/lemonic/<id>.mp3`, 60 s timeout, tracked in the shared run registry). Every emitted Track carries `size_bytes` from the library index when cached (E10).

## AudD integration status (E4)

**Built, env-gated, honest 502 while unconfigured.** `audio.recognize` POSTs multipart to `https://api.audd.io/` (`api_token` + `file`, `return=apple_music,spotify,deezer,musicbrainz`; 10 MB cap enforced client-side; AudD error codes surfaced, e.g. #900 invalid token). **No token has been supplied** — the user is logged into the AudD dashboard and will copy the token into the VPS `.env` themselves; it never passes through chat and is not in the sandbox `.env`. Until then the action answers `502 engine_unavailable` "Music recognition is not configured" (verified by test). Match → `{recognized:true, track, name, artists, album, source}` with the track id `spotify:<id>` / `deezer:<id>` when AudD returned one, else the label-only `audd:<artist> - <title>` (never a fake resolvable id). No match → `200 {recognized:false}`. No second provider. Verified against a fake AudD server only (13 tests) — **no live AudD call has been made**.

## User decision applied (2026-11-03 ~17:23 IST)

NetEase MUSIC_U is unobtainable (needs a +86 mobile / WeChat app); Deezer is dropped (no ARL, geo-blocked signup). Both resolvers stay env-gated and dormant; nothing in this work blocks on either — `search.query` marks those sources `failed` honestly without keys, and no code path requires them. Verified: `.env.example:101` still reads `SPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback` (it was already updated before this session; my AudD append did not touch it). Spotify app "Parlay" exists; the user copies its client ID/secret and the AudD token into the VPS `.env` themselves.

## Bugs found and fixed by the tests

1. `spotifyURLKind` accepted any `https://<host>/…` URL as Spotify (the host check compared against the unstripped input). Fixed: requires the `open.spotify.com/` prefix.
2. `library.delete` returned 404 for a record whose file was already gone (`Get` drops stale entries lazily). Fixed: delete-by-id removes the record directly, file or no file.
3. `download.finishFile` parameter `applyTemplate bool` shadowed the `applyTemplate` function (compile error). Fixed: parameter renamed to `rename`.
4. `download/spotify_playlist.go` used `library.SharedIndex()` without importing the library package (compile error). Fixed: import added.
5. `roomBackedPlayer` checked voice-nil before the idle check, so busy rooms failed too. Fixed: voice is only required when the room is idle (or when a track actually starts).
6. `lemonic.download` recorded into the process-wide index instead of the service's index (untestable, inconsistent with the other search handlers). Fixed: records into `s.index`.

## Honestly unmeasured / not live-verified

- **No live download** against any real provider (YouTube/Deezer/TikTok/Navidrome/Spotify/Soulseek): sandbox has no route to them and no creds. Provider handlers are covered only on validation/error paths + the `downloadOne` seam + `finishFile` with real temp files.
- **No live AudD, Lemonic, Navidrome, or slskd call** — all verified against httptest fakes only.
- **No live voice join / `vc.play_file` feed** — sandbox UDP EPERM + no session; the join-then-Play-then-rollback path is covered only up to the voice-nil / busy-room branches.
- **`mix.generate` fetch path** (daemon `Extract` on `RD<id>`) unmeasured — needs the daemon + network; only validation, seed selection, and `mixTrackFromEntry` mapping are tested.
- **Fan-out `all` timing** unmeasured — bounded by the 20 s cap, but no live timing.
- **Async `DownloadReady` over a live UDS connection** unmeasured — `Server.Emit` with no connection is a no-op; the async return + run-finish path is tested.
- **Spotify playlist zip + Soulseek album flows**: code only, no live collections.
- **Restart behaviour**: runs + prefs are in-memory (see gaps) — a restart loses finished-run snapshots (later poll → honest `not_found`) and resets prefs to defaults.

## Flagged gaps (not mine to fix here)

1. **Store buckets missing** (`internal/store` has no `downloads`, `library`, or `dlprefs` buckets; store is outside my owned list). Runs + prefs + library index are in-memory singletons; the `Registry` / `PrefsStore` / `Index` types are the seam for store-backed implementations. Nothing opens a second bbolt file.
2. **Soulseek `accept_lossy`**: `resolver.SoulseekResolver.Download` hardcodes lossless-only with no accept-lossy parameter or lossy-only detection, so `accept_lossy=true` cannot change the outcome today. The handler runs the lossless attempt and on "found nothing usable" fails `download_failed` with `lossy_available` left false (unknown, not claimed). Fix needs a resolver API addition.
3. **Download format**: enforcing non-mp3 `download.prefs` formats needs a format parameter on the daemon download method (resolver + `scripts/yt_dlp_worker.py` change).
4. **`cached_file_id` stays empty**: the backend never learns real Telegram file_ids — no core→backend file_id reporting action exists. Inventing one would break the core if passed to `sendAudio`. `size_bytes` is filled from the disk-verified library index.
5. **yt-dlp daemon fixes re-verified, not regressed**: `"noprogress": True` on both `YoutubeDL` instances and the `FFmpegExtractAudio` mp3 postprocessor are still in `scripts/yt_dlp_worker.py`; the Go client still splits JSON-RPC lines on `\r`. I touched no ytdlp-adjacent code.
6. **Real `internal/app` landed mid-task** (Worker A): `app.Deps` is now `{Store, Config, Registry, Voice, Server, Room, Lyrics}` — my five contract fields are intact, plus `Room` and `Lyrics`. My packages compile against it; `vc.play_file` and `mix.generate` now use `d.Room` where it matters.

## ⚠️ Needs a ruling (parent): `vc.play_file` is implemented twice

- **Mine** (`internal/download/playfile.go`, registered by `download.Register`): delegates to `room.Manager` + voice; **binds the interim ffmpeg feed** on the local path, so the replied file actually plays in Phase 3. Assigned to me by the brief (E12).
- **Worker A's** (`internal/handlers/vc.go:1026`, registered by `handlers.Register`): delegates to `room.Manager` + `ensureCall`; **record-only per inventory §3.3** — no feed bound in Phase 3, the Phase 5 DSP decodes the local path; `TrackStarted` still fires.
- `uds.Server.Register` **silently overwrites on a duplicate action** (`s.handlers[action] = h`) — whichever `Register` runs last in `cmd/backend` wins, with no error. Both cannot stay registered.
- **Recommendation:** register the handlers' record-only implementation (single owner of room+voice+events; follows §3.3; the DSP owns local-file decoding in Phase 5) and drop mine at wiring time — unless the parent wants Phase 3 playback of replied files, in which case `handlers.vcPlayFile` should gain the feed bind and mine should still be dropped (one owner, one behaviour). Either way the decision is a wiring choice in `cmd/backend`, not a code change in either package; my implementation is kept registered here until ruled, and the divergence is documented in `playfile.go` and in `contracts/uds-protocol.md` §4.1.

## Tree state (not mine)

`go test ./...` fails **only** in `internal/handlers` (another worker's in-flight package: `h.vcPlay undefined` vet error; `handlers_test.go:334` — `Enqueue` arity mismatch). I did not touch it. A transient `internal/lyrics` vet error (`strconv` unused) appeared once mid-run while another worker was editing that file and did not reproduce. `go vet` + `go test` are green for every package I own and every pre-existing package I depend on.
