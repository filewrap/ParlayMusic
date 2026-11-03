# Wave 2 — Worker A: Room state machine + vc.* handlers + main wiring

**Status: done, all green.** `go build ./...` clean, `go vet ./...` clean,
`gofmt` clean. Owned-package tests: **51 pass, 0 fail** (plus 1 live-gated
lyrics test that passes with `PARLAY_LIVE_TEST=1` and skips otherwise).
Full-repo suite: everything passes **except** `TestEventsReachCore` in
`internal/uds` — a pre-existing Wave 1 test race, not this worker's code
(see §8). Nothing is committed or pushed (verification worker owns that);
the whole `services/backend-go/` tree is currently untracked in git.

## 1. What was built

| File | What it is |
|---|---|
| `internal/app/deps.go` | `Deps` bundle + `New` constructor (see §2 for the final shape) |
| `internal/app/app_test.go` | 3 tests: assembly, nil-Lyrics allowed, nil panics |
| `internal/room/room.go` (+ `room_test.go`, 21 tests) | Per-chat Room + Manager: §4 state machine |
| `internal/events/events.go` (+ `events_test.go`, 2 tests) | The one emitter, §4.5 firing points only |
| `internal/lyrics/lyrics.go` (+ `lyrics_test.go`, 5 offline + 1 live) | Shared lyrics Fetcher: LRCLib + NetEase, plus NetEase-only path (§3) |
| `internal/handlers/handlers.go` | Orchestration: per-chat locks, join/bind, advance, feed-end |
| `internal/handlers/vc.go` | The 19 vc.* actions + `vc.play_file` |
| `internal/handlers/mix.go` | YouTube RD-mix autoplay picker (InnerTube `next`) |
| `internal/handlers/handlers_test.go` (20 tests) | Handler happy + error paths, feed-end advance, wiring proof |
| `cmd/backend/main.go` | Full wiring: config → store → registry → yt-dlp warm → voice → UDS → all 13 Registers → Listen → signals |
| `internal/model/model.go` | **Additive only** (the one shared-file touch outside the owned list): `RoomState` gained `AutoplayHistory []string`, `MutedVolume *int`, `Bass int`, `Radio *RadioState`, `LastTrackID string` (all `omitempty`). No existing field/tag changed; Wave 1 records read back as zero values. Required by inventory §4.6's PersistedRoom field list. |

## 2. Final `app.Deps` shape (for the verification worker's rewire)

```go
// internal/app/deps.go
package app
type Deps struct {
    Store    *store.Store
    Config   *config.Config
    Registry *resolver.Registry
    Voice    *voice.Manager
    Server   *uds.Server
    Room     *room.Manager      // NEW — the single room state machine
    Lyrics   *lyrics.Fetcher    // NEW — the single shared lyrics fetcher
}
```

Constructor (exact signature):

```go
func New(st *store.Store, cfg *config.Config, reg *resolver.Registry,
    voiceMgr *voice.Manager, srv *uds.Server,
    rooms *room.Manager, lyr *lyrics.Fetcher) *Deps
```

- `main.go` constructs the Manager (`room.NewManager(st,
  cfg.Limits.MaxQueueSize)`) and the Fetcher (`lyrics.New(reg)`) exactly
  once and passes them in; `New` only assembles, never constructs a
  second Manager/Fetcher/store/client. `New` panics on any nil argument
  **except `Lyrics`**.
- `Lyrics` may be nil: it means lyrics are not configured (the Fetcher
  could not be built from the registry — impossible with the real
  registry, but the degraded mode is designed, not accidental).
  `vc.lyrics` answers `engine_unavailable` when it is nil. `Room` must
  never be nil.
- `handlers.New(d)` takes `d.Room` / `d.Lyrics` and installs the autoplay
  picker on the shared Manager (`d.Room.SetPicker(...)`) plus
  `Restore()`. It never builds its own Manager or Fetcher.

**Rewire note for the verification worker** (do not guess — these are the
exact seams Worker C left):

- `internal/vote/vote.go:17,163` — replace `enqueueOnThreshold`'s
  store-direct queue write with `d.Room.Enqueue(chatID, track, by)`.
  (Vote currently writes the queue bucket + RoomState directly; that
  leaves the Manager's in-memory cache stale.)
- `internal/gui/gui.go:8` — replace the stored-room reads with
  `d.Room.Snapshot(chatID)`; the room owns the live position clock.
- `internal/netease/netease.go:17,38-41` — swap the `lyricsFetcher`
  (currently `resolver.NewLyricsResolver(ne)`) for the shared Fetcher's
  **NetEase-only** path: `d.Lyrics.FetchNetEase(ctx,
  strconv.FormatInt(id, 10))` — see §3. Do NOT route it through
  `d.Lyrics.Fetch`.
- `internal/spotify/` keeps its own Genius→Musixmatch→LRCLib chain.
  Do NOT force it through the Fetcher.

## 3. `internal/lyrics`: two fetch paths (do not confuse them)

- `Fetch(ctx, title, artist, neteaseID)` / `FetchForTrack(ctx, track)` —
  the **vc.lyrics** path: NetEase first when a numeric id is known, else
  LRCLib by title + artist (LRCLib is also the fallback when NetEase
  errors without saying "no lyrics"). Miss → `lyrics.ErrNoLyrics`
  (→ `not_found`); source failure → the underlying error
  (→ `engine_unavailable`). Lyrics are never invented.
- `FetchNetEase(ctx, neteaseID string) (*Result, error)` — the
  **netease.lyrics** path: NetEase ONLY, never LRCLib. It calls
  `NetEaseResolver.Lyrics(id)` directly and builds the `Result`
  (`Source: "netease"`, `TrackID`, `Synced`). An invalid (non-numeric,
  zero, negative) id is a plain error — callers validate the id shape
  first. Title/artist are left empty: the NetEase lyric endpoint
  returns lyric text only, and the caller fills them from its own
  track lookup (as `netease.lyrics` already does via
  `Registry.Resolve`). The ctx is accepted for signature symmetry; the
  NetEase client carries its own 10 s timeout.
- Why the split exists: `netease.lyrics` hardcodes `"source":
  "netease"` in its response. Routing it through `Fetch` could return
  LRCLib lyrics labelled `netease` whenever NetEase errored and a
  title was known. `FetchNetEase` closes that structurally.
- Miss-error contract: a NetEase miss returns an error wrapping **both**
  `lyrics.ErrNoLyrics` and `netease.ErrNoLyrics` (via
  `mapNetEaseLyricsErr`), so the netease handler's existing
  `errors.Is(err, netease.ErrNoLyrics)` check keeps working after the
  rewire, and the shared `lyrics.ErrNoLyrics` check works too.
  Pinned by `TestMapNetEaseLyricsErr`.
- Live verification (anonymous — no MUSIC_U, which is unobtainable;
  see §9): `TestFetchNetEaseLive` (`PARLAY_LIVE_TEST=1`) passes —
  id `123456` returns 1680 bytes, `source: "netease"`, synced.
  The miss branch could NOT be produced live: the endpoint returns a
  22-byte timestamp line even for nonexistent ids (probed 2026-11-03:
  ids 1, 2, 3, 99999999999 all returned lyrics), so that branch is
  verified by code inspection + the wrapping test, not by a live test.
  Flagged here, not hidden.

## 4. Room state machine (`internal/room`)

Idle/Playing/Paused, queue ops (0-based, waiting-only), loop 0–10
consumed at advance, shuffle-in-place, autoplay RD-mix on drain with
history cap 50, position clock, persisted snapshots via
`store.PutRoom/GetRoom/PutQueue/GetQueue`. The room is the single
source of truth for now_playing/queue/progress.

- `Advance` kinds: NoOp, Replayed (loop; fires TrackStarted only),
  Next (queue pop; TrackAdvanced + TrackStarted), Autoplay (pick;
  TrackStarted), Idle (TrackEnded + QueueEmpty, then leave the call).
- No advance ticker, by task order (overrides inventory §4.3's interim
  1 s ticker note): advance is event-driven only. Consequence: a track
  with no feed never auto-advances, and a kill mid-track restores the
  last persisted position, not the kill-time position.
- `Restore()` forces `Paused=true` with the position frozen.
- `Stop` always succeeds (even with no room), keeps shuffle + volume /
  mute, clears loop + autoplay + history, prunes default rooms.
- Volume clamps 0–200 (vc-go rule); `SetVolume` while muted also moves
  `MutedVolume`, so unmuting restores the latest intent. Mute is a
  real flag, orthogonal to volume. Speed/bass are stored for Phase 5
  DSP. Radio start/stop keeps the radio bucket in sync.
- `SetVolume`-while-muted → `MutedVolume` update was added during
  handler work (small room change, covered by room tests).

## 5. Per-action status (19 vc.* + vc.play_file)

All registered in `handlers.Register` (20 actions). Error codes follow
the contract: `bad_request` / `not_found` / `engine_unavailable`
(`uds.NewHandlerError`); nothing-playing → `not_found`, except
`vc.stop` (always 200) and `vc.queue list` (200 with `[]`).

| Action | Status | Notes |
|---|---|---|
| vc.play | done | Resolves via `d.Registry.Resolve` (JioSaavn `/song` fast lane verified preferred in `resolver.go` — no resolver change needed). Empty `StreamURL` (Spotify metadata-only) → `engine_unavailable` before touching the room. Join first (failed join → room untouched); bind before `TrackStarted` so a failed bind rolls back to idle with no event. Optional play-body volume/speed/bass applied to the room after Play. Returns `{now_playing, queued, resolved_by}`. |
| vc.pause | done | Room-only when no call record; voice pause failure undoes the room pause. Idempotent. |
| vc.resume | done | Feed paused in place when the feed is recorded; rebinds when no feed runs (needs a live call, else honest `engine_unavailable` + room stays paused — resume-after-restart restarts audio from track start, not the frozen position). Shared `resumeFeed` helper with panel resume. |
| vc.skip | done | Loop reset + advance; empty room → `not_found` without joining; join first. |
| vc.stop | done | Always 200; leaves the call best-effort. |
| vc.seek | done | `position_sec` required; clamps to `[0, duration]`; negative → `bad_request`. Seek to 0 rebinds the feed from the start; seek to >0 moves the room clock only — **audio-position seek needs a `voice.Manager.Seek` (ffmpeg `-ss`), which does not exist; see §7(a)**. |
| vc.queue | done | add (literal track, source defaults `youtube`, or query via registry) / remove (0-based) / clear / list (never null). Add never starts playback even when idle. Max-queue enforced (`ErrQueueFull` → `bad_request`). |
| vc.loop | done | `count` required, 0–10; idle → `not_found`. |
| vc.shuffle | done | `enabled` required; enabling shuffles the waiting queue in place. |
| vc.autoplay | done | `enabled` required; disabling clears history. |
| vc.autoplay_skip | done | Gated pick (autoplay off → `bad_request`, nothing playing → `not_found`); loop reset; pick stands as current on bind failure (previous track already replaced) with `VoiceError` + `engine_unavailable`. |
| vc.volume | done | `volume` required; clamps 0–200; stored only in Phase 3 (DSP gain is Phase 5). |
| vc.now_playing | done | 404 iff no room or (no current AND empty queue) — vc-go rule; queue-only room → 200 with `track: null`. |
| vc.progress | done | Same 404 rule; queue-only → 200 with `now: null`, `next` = first waiting. `{position_sec, duration_sec, is_paused, is_muted, loop_enabled, now, next}`. |
| vc.panel | done | GET (no `action`) → `{now_playing, panel}`; POST → `{message, panel, now_playing}`. 10 buttons, plain status text, no emojis. Control actions need a current track (`bad_request "Not streaming."`, vc-ts 400, not 404); shuffle needs ≥1 queued; volinc/voldec ±10; loop toggles 10/0. |
| vc.lyrics | done | POST (FINAL §6-R5; the old GET is dead). Title/artist from body else current track; no title → `bad_request`; `ErrNoLyrics` → `not_found`; nil Fetcher → `engine_unavailable` "lyrics not configured". |
| vc.join | done | `Voice.Join` (idempotent); failure → `VoiceError("join_failed")` + `engine_unavailable` with the exact reason, never a faked join. Success → `{joined: true, now_playing}` (honest empty snapshot when no room exists). Join only — does not start a feed. |
| vc.mute | done | `muted` required; real flag; repeated mute/unmute → 200; voice failure undoes the room flag. |
| vc.radio | done | start: station required, unknown → `bad_request` naming valid stations; `ProbeStream` (10 s) best effort; room start succeeds even when the handoff fails (`vc_handoff: {ok, joined, reachable, error?}`); an unreachable stream starts the room but binds no feed (binding a dead stream would die at once and the feed-end advance would kill the radio). stop: `not_found` when no radio; leaves the call only when the radio was the only thing playing. |
| vc.play_file | done — record-only per §3.3 (ruling, see §6) | Path under the data dir (`..` rejected; relative paths resolve against the data dir), missing → `not_found`, directory → `bad_request`, unknown kind → `bad_request`, negative duration → `bad_request`. Track `{id: "file:<file_id or sha1hex(path)[:12]>", source: "file", duration_sec, url: "", stream_url: ""}`. Play-now-if-idle else enqueue. **No feed is bound in Phase 3**; response carries an honest `voice_state` (§6). Duration 0 never auto-advances (no feed → no EOF → no advance, ever). The file is not moved/deleted (move is optional in §3.3). |

## 6. vc.play_file duplicate — ruling applied (2026-11-03 ~17:30 IST)

Worker B also implemented `vc.play_file` in
`internal/download/playfile.go`, binding the interim ffmpeg feed so the
file would actually play in Phase 3. The parent ruled: §3.3 explicitly
prescribes **record-only** for Phase 3, so the handlers version is the
per-spec one.

Applied in the wiring (`cmd/backend/main.go`):

- `download.Register(server, deps)` runs **first**, `handlers.Register`
  runs **immediately after it**. Both register `vc.play_file`, and
  `uds.Server.Register` silently overwrites on a duplicate, so the
  LAST registration wins — the handlers record-only version is the
  effective one. `internal/download/playfile.go` is left in place,
  registered-then-overwritten; it was **not deleted** (the verification
  worker deletes or neutralizes it). No other action overlaps:
  `download.Register`'s other actions are all `download.*` /
  `mix.generate`.
- Proof: `TestWiringPlayFileIsHandlersVersion` dispatches a real
  `vc.play_file` frame over the socket after registering in main.go
  order and asserts the handlers fingerprint — a `VoiceError`
  (`join_failed`) event before the `engine_unavailable` response.
  Download's version emits no event on join failure, so a reversed
  order would fail this test. (Both versions fail identically at the
  join with no core, so the event is the only offline distinguisher;
  the success path, where they differ by feed vs record-only, is
  live-only.)
- The handlers version was aligned to §3.3 + B's validation while
  keeping it record-only:
  - Response carries `voice_state`: `{joined, streaming: false,
    detail: "local file — audio path lands with the DSP"}` — the §3.3
    wording verbatim. `joined` reads the live call record (never
    claimed); `streaming` is always false for a local file in Phase 3.
    Pinned by `TestVCPlayFileVoiceState`. (`voiceState` has no field in
    `model.NowPlaying` or `contracts/ipc.json` — it is a top-level key
    on the play_file response, defined here and in code, so the core
    reads exactly these three keys.)
  - Kind validation (`audio|video|voice|video_note|document`) and
    negative-`duration_sec` rejection, matching B's validation and the
    §3.3 request shape.
  - Relative paths resolve against the data dir (same rule as B's
    `validateRepliedPath`); absolute paths must already be under it.
    Pinned by a relative-path test case.

## 7. Frame path — interim (Phase 5 replacement target)

Audio reaches ntgcalls through `voice.Manager.Play`'s existing
ffmpeg-decode-to-PCM path (`internal/voice/feeder.go`): one persistent
ffmpeg process per room, 10 ms s16le frames (480 samples × 2ch × 2B at
48 kHz) over the external source, started on track start and restarted
only on track change — never per call or per frame. No other frame path
was invented. Phase 5 replaces it with parlay-dsp: DSP decodes to f32,
applies the FX chain, dithers once to s16le, Go pushes 480-sample
frames via `SendExternalFrame`. The 20 ms frame delivery from
parlay-dsp is Phase 5 work, not this wave.

## 8. Honestly unmeasured / not verified live

- **All live voice paths**: real Join/Connect/playback, `vc.join`
  success, `vc.play` success, feed-end advance on a real call, radio
  handoff success, `vc.play_file` success + `voice_state.joined: true`.
  The sandbox has no core, no MTProto session, and UDP EPERM — Join
  cannot succeed here (proven by the voice tests: a garbage remote SDP
  fails at the `connect` stage). Handler tests use a joinFunc that
  always errors, so every join-dependent success path is unmeasured;
  the room transitions underneath are covered by room tests and the
  voice Manager's own Join/Play paths by voice tests.
- **Autoplay picker live**: `parseMixJSON` + `mixVideoID` are unit-tested
  on canned JSON; the live InnerTube `next` fetch + candidate resolve
  is unmeasured (network + YouTube). The picker runs outside the room
  lock and a stale pick is discarded if a `/play` wins mid-pick.
- **Lyrics miss branch live**: see §3 (endpoint never returns empty
  lyric for any probed id).
- **yt-dlp daemon warm at boot**: `Ping`/`Version` are wired in main.go
  but the binary has not been run end-to-end in the sandbox (no run was
  started with the user's token; all tests use dummy creds only). The
  daemon client's sticky-failure behaviour (once `ensureStarted` fails
  it never retries in-process) is noted in main.go.
- **Pre-existing failure (NOT this worker's)**: `TestEventsReachCore`
  in `internal/uds` fails 3/3 in isolation and in the full suite
  (`read event: i/o timeout`). Cause: `dialPeer` in `uds_test.go` does
  not wait for the server's `acceptLoop` to register the connection
  (`s.conn` is set asynchronously), and `Emit` drops silently when
  `s.conn == nil` — so the emitted event never arrives. My own
  events/handlers tests work around the same race with a 50 ms sleep
  after dial. `internal/uds` is outside my owned files and untouched
  by this work (the test binary doesn't even compile my packages), so
  I did not fix it. The verification worker should: either wait for
  accept in the test, or make `Emit` not drop the first event. (The
  same drop-on-no-core race exists in production in miniature: an
  event emitted in the microseconds between core connect and accept
  is lost. Fail-open by design, but worth knowing.)
- **Room position clock, duration-0 tracks**: `positionLocked` does
  not clamp to duration, so a duration-0 file track's `position_sec`
  ticks up unboundedly while it is "playing" (no feed, no EOF). It
  never auto-advances — that requirement holds — but the displayed
  position is a clock, not a playback position. Not changed (room
  behaviour, out of the play_file ruling's scope); flagged here.
- **JioSaavn `/song` fast lane**: verified preferred in `resolver.go`
  (ids → one `/song?id=` call, 0.32 s measured in Phase 1; free text →
  search + parallel `/song` enrichment, 320_KBPS never 96). No resolver
  change was needed; no second resolver was added.

## 9. Standing decisions applied (no changes made outside scope)

- **NetEase / Deezer (user ruling 2026-11-03 ~17:23 IST)**: NetEase
  MUSIC_U is unobtainable (no +86 mobile; WeChat login is QR-only), so
  no account creation was attempted. The NetEase resolver stays
  env-gated on `NETEASE_MUSIC_U` and dormant for lossless; anonymous
  search — and the anonymous lyric endpoint used by `FetchNetEase` —
  already work and are the only live NetEase paths. Deezer is dropped
  for now (no ARL; sandbox geo-blocked signup) and its resolver stays
  env-gated/dormant. Verification was not blocked on either; both fail
  honestly without keys (proven in tests by the Spotify no-creds
  resolve-failure path, same pattern).
- **Spotify redirect URI**: `.env.example:101` already has
  `SPOTIFY_REDIRECT_URI=http://127.0.0.1:8080/callback` with the
  "Spotify rejects localhost" note — the exact value the user's app
  registered. Config reads it via env into
  `cfg.Backend.SpotifyRedirectURI`. Nothing to fix; config is not in
  my owned files and was not edited. Spotify client ID/secret and the
  AudD token are copied by the user into the VPS `.env` themselves;
  those values never passed through chat and are not in the sandbox.
- **AudD** (`/recognize` provider, user-ruled 2026-11-03 ~16:57 IST):
  token not supplied yet; that package is another worker's and stays
  env-gated until configured.
- `.env` was never touched; no service was started with the user's
  token; nothing was committed or pushed.

## 10. Gaps and open items (for the parent / verification worker)

(a) Audio-position seek for `position > 0` needs a
`voice.Manager.Seek` (ffmpeg `-ss` rebind) — outside the owned files;
today the room clock moves and the audio continues from its decode
point. (b) No ticker by task order — a track with no feed never
auto-advances; position restores to the last persisted value.
(c) Resume-after-restart requires re-join and restarts audio from
track start, not the frozen position. (d) Radio fallback-playlist
rotation on dead streams is not implemented (radio package / Phase 4).
(e) Live voice paths unmeasured — see §8. (f) `model.RoomState`
additive extension — see §1. (g) `main.go` calls all 13 feature
Registers; all 13 now exist (download, library, search, recognize,
spotify, share, vote, fx, settings, gui, radio, netease + handlers),
so no missing-package note remains. (h) Worker C's spotify package
was broken mid-write at Wave 2 start; it now builds (full `go build
./...` clean). (i) Event ordering: vc.play binds before TrackStarted
so rollback is clean; advances fire events before binding because the
transition is irreversible. (j) `SetVolume`-while-muted updates
`MutedVolume` — done. (k) `vc.play_file` added although not in the
parent's original 19-action list (inventory §11.7 says Wave 2 adds
it; §3.3 designs it). (l) The mix picker duplicates the InnerTube
`next` call + public key from the resolver (unexported there; the
resolver is outside the owned files). (m) `voiceStateForFile`'s
`joined: true` case is unmeasured (needs a connected call).
(n) `TestEventsReachCore` race — see §8; needs a fix outside this
worker's files. (o) The `vc.play_file` duplicate file
`internal/download/playfile.go` is still in the tree, neutralized only
by registration order — the verification worker still has to delete
or neutralize it, and re-run `TestWiringPlayFileIsHandlersVersion`
after doing so (it will fail if download's version ever wins the slot
again).

## 11. Test counts (exact, all run 2026-11-03)

- `internal/app`: 3 pass (assembly, nil-Lyrics allowed, 6 nil panics).
- `internal/room`: 21 pass (~3.7 s; includes ~1.1 s pause/resume clock sleeps).
- `internal/events`: 2 pass (live UDS round-trip for all 5 events; no-core silent no-op).
- `internal/lyrics`: 5 pass offline + 1 live (`TestFetchNetEaseLive`,
  `PARLAY_LIVE_TEST=1`, 1.46 s, anonymous).
- `internal/handlers`: 20 pass (~2.5 s; includes feed-end advance ×2,
  wiring proof, mix parsing).
- Full repo `go test ./...`: all packages pass except `internal/uds`
  (`TestEventsReachCore`, §8).
- `go build ./...`, `go vet ./...`, `gofmt -l cmd internal`: all clean.
