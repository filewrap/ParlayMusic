# Wave 2 — Worker C report: Spotify + share + vote + fx + settings + gui + radio + netease

Date: 2026-11-03. Base: main at `3d84fb6` (G1/G2 fix). No commit, no push
(verification worker owns that). `~/workspace/parlay/.env` never read or
touched; all tests use dummy creds / httptest only.

## What was built

Eight packages under `services/backend-go/internal/`, each exposing
`func Register(s *uds.Server, d *app.Deps)` per the Wave 2 contract, 5,417
lines total (code + tests). Every handler returns honest contract errors
(`uds.NewHandlerError`); key-gated / missing-credential paths fail, never
fake data. No stubs, mocks, placeholders, TODO/FIXME/HACK/unimplemented
(grepped clean), no silent error drops.

| Package | Files | Tests |
|---|---|---|
| spotify/ | spotify.go, crypto.go, auth.go, watch.go, classify.go, lyrics.go, handlers.go | 22 |
| share/ | share.go | 5 |
| vote/ | vote.go | 5 |
| fx/ | fx.go | 5 |
| settings/ | settings.go | 6 |
| gui/ | gui.go | 5 |
| radio/ | radio.go | 6 |
| netease/ | netease.go | 8 |
| **Total** | **17 files** | **62 passed, 0 failed, 0 skipped** |

Verification: `go build` on all eight packages + `internal/app` OK;
`go vet` on all eight packages clean; `gofmt -l` clean.
`go vet ./...` at module root reports one pre-existing failure outside my
ownership: `internal/handlers/vc.go` imports `internal/room` (Worker A's
area; see §Seams). Pre-existing resolver/voice vet complaints from the
Wave 1 scaffold note were not re-verified — `go vet ./...` stops at the
handlers error, so they are neither confirmed nor cleared.

### spotify/ (11 actions)

- **OAuth token store**: AES-256-GCM (stdlib `crypto/aes` + `cipher.NewGCM`),
  96-bit random nonce per record, AAD `parlay:spotify-user-token:v1:<user_id>`
  (crypto.rs value wins over the inventory §8.2 wording), key from
  `PARLAY_SPOTIFY_OAUTH_KEY` (base64, 32 bytes) or `<data_dir>/spotify-user-key`
  (0600, hex-encoded, generated on first use). Tokens stored ciphertext-only
  via `store.PutSpotify/GetSpotify`. Round-trip + wrong-user AAD failure +
  key-file 0600 covered by tests.
- **App-token cache** with 401-clear-and-retry-once (`apiGet`).
- **Auth flow** (§8.2): login/callback/refresh/revoke. Missing
  `SPOTIFY_CLIENT_ID`/`SPOTIFY_CLIENT_SECRET`/`SPOTIFY_REDIRECT_URI` → 503
  `engine_unavailable` for all four sub-actions. Scopes
  `user-read-currently-playing user-read-playback-state`. State
  `<user_id>-<random16>`, single-use, 10-min TTL, deleted on first callback
  attempt. Refresh failure drops the token + 401. Revoke idempotent.
  Refresh window: expires_at ≤ now+120s.
- **Watch registry + poller**: start without token → 404; stop →
  `{watching:false, was_watching}`; poll interval env
  `SPOTIFY_POLL_INTERVAL_SECS`, default 30, 0 disables. Poller refreshes if
  needed, GETs `/v1/me/player/currently-playing`, 204/no item → not playing,
  401 → drop token, other failures keep previous state; on track change runs
  skip-check and records the verdict. Poller never skips on the user's player.
  `spotify.now_playing` returns LAST POLLED state only, never a live fetch;
  unwatched → 200 `{playing:false, track:null}`.
- **Skip-check classifier**: the 54-word profanity list verbatim from
  spotify-rust (word-boundary, case-insensitive; hell/damn/crap deliberately
  excluded). Order: disliked → skip `disliked`; Gemini (if key) replaces the
  wordlist verdict, failure → `wordlist-fallback`; no key → `wordlist`; no key
  + no lyrics → verdict `unknown`, provider `none`. `ai_generated` skips only
  at confidence ≥ 0.7. Lyrics fetch order Genius → Musixmatch → LRCLib
  (5s cap each); Genius via HTML scraper for `data-lyrics-container` divs.
- **resolve**: parse-only without creds (`api:"none"`, note naming missing
  creds, never fake); with creds, track → metadata + YouTube-ladder audio via
  `d.Registry.Resolve` (Spotify is metadata-only; never spotdl). API error
  with creds → 502 `engine_unavailable`.
- **collection**: kind track → 400 "use resolve"; no creds → 502; cap 50,
  `truncated` flag. Accepts `{url}` (ipc.json, authority) with `kind`+`id`
  fallback.
- **status**: never errors; `{api, credentials_configured, token_cached,
  user_tokens, watching, gemini_configured, poll_interval_secs, polling}`.
- **details**: creds required else 502; genres via `/v1/artists?ids=`,
  audio-features via deprecated endpoint (null when refused, never faked),
  lyrics + Genius link, first profanity word in lyrics. `disliked_by` /
  `ignored_by` are `[UserId]` arrays per ipc.json (old code returned counts);
  `language` always null; `ignored_by` always `[]` (`spotify.ignore` is an
  undecided proposal per §9-E8 — NOT added).
- **analyze**: no lyrics → 404; one-sentence `analysis` ("Verdict:
  \<verdict\> (\<provider\>) — \<detail\>"); `profane_words` = every matched
  word in lyrics, deduped, in appearance order.
- **watches**: plain `[UserId]` per ipc.json (inventory §6.6 objects claim
  rejected — store records no started_at).

### share/ (2 actions)

Anonymous link minting/resolution. 16 random alphanumeric tokens,
collision-checked, stored via `PutShare/GetShare`; link
`https://t.me/<bot>?start=share-<token>` with bot username from env
`TELEGRAM_BOT_USERNAME` (unset → bare `https://t.me/?start=share-<token>`,
never an invented username). No expiry. Resolve does NOT consume the token.
Unknown token → not_found. Fresh start — no migrated links.

### vote/ (1 action)

`queue.vote`: one vote per voter per track (re-vote replaces; decline cancels
approval); `queued = approvals >= threshold`; threshold from env
`PARLAY_VOTE_THRESHOLD`, default 3, min 1 (see §Gaps — no `[voting]` section
exists in config). Voter id accepts number or numeric string. Re-vote same
value = 200 no-op. A track that reached threshold stays queued even if a
later decline drops it below threshold. Response `{queued, approvals,
declines, threshold}` + `track_id`.

### fx/ (1 action)

`fx.apply`, RECORD-ONLY in Phase 3: effects slow-reverb, nightcore,
bass-boost, 8d, original. Persists preset/params in the room; answers
`{applied:false, pending_dsp:true}` plus stored values — never claims an
effect applied (except `original`: `{applied:true, pending_dsp:false}`).
Unknown effect / missing input / not-a-file / `..` traversal → 400.
No audio processing, no subprocess (parlay-dsp is Phase 5).

### settings/ (1 action)

`chat.settings`, all 15 keys (ipc.json authority): admin_mode int 0–2 (0),
play_mode string everyone|admins ("everyone"), cmd_delete bool (false),
clean_mode **bool** (false; ipc.json wins over inventory's int),
clean_delay_min int (0), thumbnails bool (true), autoplay bool (false),
autoplay_lang string ("" — inventory §8.1/U3; model default "en" overridden),
autoplay_mood string (""), silent_mode bool (false), auto_delete bool
(false), auto_delete_min int 2–7 (5), language string "en" (other codes →
bad_request), paginate_purge_old bool (true), chat_music_download bool
(true). clean_mode/clean_delay_min are one field through two names (kept in
sync; enabling clean_mode with delay 0 defaults delay to 15). GET no key →
all 15; GET with key → that key only under "settings"; POST validates
type/range. Bool keys accept JSON bool or "true/false/on/off/1/0/yes/no"
strings; int keys accept JSON number, numeric string, or float.

### gui/ (2 actions)

`gui.now_playing`, `gui.queue`: pure reads from room/store, no mutations.
now_playing 404 mirrors vc.now_playing (no room, or no track + empty queue);
queue-but-no-current → 200 track null. queue returns `[]` with 200 for an
unknown chat (Mini App needs empty state, not an error). Position reported
is the persisted frozen position (no live clock until the room seam lands).

### radio/ (4 actions)

Locked 6-station list: groovesalad, lush, radioparadise, fip, bbcws, kexp.
`FindStation` matches id, exact name, or name substring (case-insensitive).
Native stream verification via `resolver.ProbeStream` (never ffprobe);
unreachable still starts (`reachable:false`, not an error). Radio track id
`radio:<station_id>:<playlist_index>`, duration 0, `is_live:true`; fallback
playlist rotates every 180s. Stop: 404 when no radio; clears radio state;
stops VC only if the current track is still the radio track; response both
keys `{stopped:true, vc_cleared:bool}`. Status: 404 when no radio; superset
`listeners:0, total_listeners:0, loop_type:"none", autoplay_enabled,
queue_length`. `Start`/`Stop` exported on Service as the vc.radio seam
(Worker A hands off via the room/voice path); vc_handoff failure never fails
the start; handoff kept in an in-memory map (no store field). `radio.list`
probes all 6 stations concurrently.

### netease/ (3 actions)

Thin handlers over the absorbed client (`resolver/netease` +
`resolver.NewNetEaseResolver`). track_id accepts string OR number (URL and
`netease:<id>` forms too). Search via Registry (limit 10); zero results →
200 `tracks:[]`, not 404. Download: metadata resolve first, then
`NetEaseResolver.Download`; oversize → partial file deleted, 200
`{file_path:"", track, oversize:true, size_bytes, auth, note}` (no oversize
file written, per §5.2). Lyrics via `resolver.LyricsResolver` (NetEase leg
only) with synced LRC via `/lyric/new`, `source:"netease"`,
`truncated:false`, `.lrc` written to the download dir best-effort
(`lrc_path` superset, `""` on write failure); no lyrics → 404
(ErrNoLyrics). Lossless FLAC needs `NETEASE_MUSIC_U`: without it the
resolver degrades through lower levels/mirror and the response reports
`auth` honestly (`music_u`|`anonymous`) — "fail honestly" = `download_failed`
when no URL resolves at any level, not a hard fail on a missing cookie.

## Per-action status (25/25 implemented)

| Action | Status |
|---|---|
| spotify.resolve | done |
| spotify.collection | done |
| spotify.status | done |
| spotify.auth | done |
| spotify.watch | done |
| spotify.watches | done |
| spotify.dislike | done |
| spotify.skip_check | done |
| spotify.now_playing | done |
| spotify.details | done |
| spotify.analyze | done |
| share.create | done |
| share.resolve | done |
| queue.vote | done |
| fx.apply | done (record-only, per spec) |
| chat.settings | done (all 15 keys) |
| gui.now_playing | done |
| gui.queue | done |
| radio.list | done |
| radio.start | done |
| radio.stop | done |
| radio.status | done |
| netease.search | done |
| netease.download | done |
| netease.lyrics | done |

## Seams and dependencies (what changed while I worked)

- **`internal/app`**: my temporary contract-shaped stub (`app.go`, created
  solely so my packages could compile/test) was **deleted by Worker A** when
  the real `internal/app/deps.go` landed. Deps shape matches the contract
  exactly, plus a `New` constructor. All eight packages now build and test
  against the real package — no action needed.
- **`internal/room` and `internal/lyrics` landed after my packages were
  written** (room.go + lyrics.go present at finish). I deliberately did NOT
  rewire to them, for three concrete reasons:
  1. `app.Deps` has no Room or Lyrics field. `room.Manager` holds in-memory
     state; constructing a second Manager inside vote/fx/gui/radio would
     create a second owner of room state that diverges from Worker A's
     Manager. The correct wiring is a shared Manager exposed through Deps
     (or main) — a Worker A / verification-worker decision, not a silent
     second construction here. My store-based seams are marked in-code
     (vote.go notes `replace enqueueOnThreshold with room.Enqueue`;
     gui.go notes `with room.Snapshot`; fx persists via RoomState).
  2. `lyrics.Fetcher.Fetch` adds an LRCLib fallback behind the NetEase leg.
     For netease.lyrics the inventory requires NetEase synced LRC only,
     `source:"netease"`, no-lyrics → 404. A Fetcher swap would return LRCLib
     lyrics labelled `netease` for tracks NetEase has none for. My handler
     keeps the NetEase-only leg (`resolver.LyricsResolver` — the same
     resolver Fetcher wraps). One-line swap if the spec ever wants the
     fallback.
  3. spotify's lyrics order is Genius → Musixmatch → LRCLib; Fetcher has no
     Genius/Musixmatch legs, so swapping would regress
     spotify.details/analyze.
- **Radio vc seam**: `radio.Service.Start`/`Stop` exported for Worker A's
  `vc.radio` handler; handoff calls `d.Voice.Join`/`Play` directly, nil-safe
  (no voice manager → handoff reports `{joined:false, error:"voice manager
  not configured"}`, start still succeeds).
- **Vote enqueue seam**: counted votes drive the room queue via direct store
  writes today (GetQueue/PutQueue + RoomState.Queue, kept in sync); swap to
  `room.Enqueue` when a shared Manager is reachable through Deps.
- **GUI snapshot seam**: `gui.Snapshot(room, queue)` helper is exported so
  radio can reuse it; to be replaced by `room.Snapshot` when wired.

## Gaps / discrepancies (do not silently resolve)

- Config has no `[voting]` threshold, no poll interval, no bot username, no
  Genius/Musixmatch keys → vote threshold from env `PARLAY_VOTE_THRESHOLD`
  (default 3); poll interval from env (default 30); bot username from env;
  Genius/Musixmatch from env.
- Store has no ListDislikers, token-count, fx bucket, polled-now-playing
  record, or radio started_at/vc_handoff fields → workarounds: `disliked_by`
  = watched users + requester checked via IsDisliked (may under-report);
  `user_tokens` in status = watched users with a stored token; fx durable
  record is the room FXPreset/FXParams; radio `started_at_ms` derived from
  `RadioState.UpdatedAt` × 1000; vc_handoff in-memory only.
- ipc.json vs inventory, resolved in favour of ipc.json (contract
  authority): chat.settings `clean_mode` bool (not int); spotify.watches
  `[UserId]` (not objects); spotify.collection request `{url}` (kind+id
  accepted as fallback); skip_check `reason` uses `ai-generated` (hyphen)
  while `classifier_verdict` uses `ai_generated`.
- Inventory §8.1 autoplay_lang default "" (U3, undecided) vs model default
  "en" — implemented as "".
- NetEase MUSIC_U interpretation: no hard fail without it; degrade + report
  `auth` honestly.
- `spotify.resolve` API-error-with-creds returns HandlerError
  `engine_unavailable` (message carries the api error); it cannot also
  return a data object with `api_error` on a 502 in UDS terms.
- `go vet ./...` at module root fails in `internal/handlers/vc.go`
  (imports `internal/room`, not yet wired there) — outside my owned files,
  not fixed.

## Bug found and fixed during verification

`TestRegisterAnswersOverUDS` hung the whole spotify suite (10-min test
timeout hit; the first backgrounded run sat ~7 min with no output). Cause:
the test opened a second bbolt store on the same directory `testService`
had already opened — bbolt's file lock blocks the second open forever.
Fix: reuse the store from `testService`. Spotify suite now runs in 0.09s.
Lesson for other workers: never `store.Open` the same dir twice in one
process.

## Honestly unmeasured

Live-network paths were NOT tested in the sandbox (dummy creds / httptest
only, per the task): real Spotify Web API calls (resolve/collection/details/
analyze with creds, token refresh against accounts.spotify.com, OAuth
callback end-to-end), Genius/Musixmatch/LRCLib fetches, NetEase search/
download/lyrics against music.163.com (including lossless FLAC with a real
MUSIC_U), real radio stream probes (all six stations), live VC handoff
(Join/Play against ntgcalls), the Spotify poller end-to-end (interval ticks,
track-change detection), and the full UDS round-trip for actions other than
spotify.status. Key-gated paths (no Spotify creds, no Gemini key, no
MUSIC_U) are covered only by their honest-error branches.
