# Wave 2 Worker D — Core-cpp fixes + AudD decision record

Worker D, Parlay Phase 3 Wave 2. Branch base: main at `3d84fb6` (G1/G2 fix).
No commit, no push (verification worker owns that). `.env` untouched; all
tests use dummy creds only; no service started with the user's token.

Owned files only: `services/core-cpp/src/dispatcher.cpp`,
`services/core-cpp/src/dispatcher.hpp`, `services/core-cpp/tests/dispatcher_test.cpp`,
`services/core-cpp/tests/routing_test.cpp`, `contracts/routing-table.md`
(affected rows only), `phase3/actions-inventory.md` (§9-E4 only),
`FINAL-ARCHITECTURE.md` (§11 OD-10 row + one decision note only).

## Fixes (all in `services/core-cpp/src/dispatcher.cpp`)

### D3 — `/loop` → `vc.loop` (inventory §10-D3)
`onLoop`, dispatcher.cpp:2805. No-args reports the stored field; `off` /
digits post `vc.loop` `{chat_id, count}` clamped 0–10, store the field,
reply `Turned loop on/off`. The old copy-queueing path is deleted — queueing
N copies AND setting the loop count would have looped the track 2N times.
No `queue` add is issued on this path.

### D4 — Queue display N → backend N−1 (inventory §10-D4)
The `vc.queue` list is waiting-only (§1.7): index 0 is the first *waiting*
track, the current track is never in the list. Display N now sends backend
position N−1 everywhere:
- `buildQueueCard` (dispatcher.cpp:1429) iterates the waiting list from
  index 0 and numbers up-next 1..N; `resolveQueueToken` (dispatcher.cpp:1494)
  returns the 0-based position.
- `refreshPanel` next track = `items[0]`; `onMenu` next track = `q[0]` (this
  also fixed a latent bug: it read `title`/`url` directly off the QueueItem
  instead of `item.find("track")`).
- `onPlay` queued card uses `queuedText(items->size())`; `onClear`
  (dispatcher.cpp:2905) counts all waiting tracks; `onRestore`
  (dispatcher.cpp:2954) restores all saved items from index 0.
- `onRemove` (dispatcher.cpp:2993): display p validates 1..size, sends
  `position = p-1`, names `items[p-1]`.
- `onMove` (dispatcher.cpp:3108): the `from == 1` "Cannot move the currently
  playing track" guard is deleted (display 1 is a waiting track now);
  rebuilds the whole waiting queue removing 0-based positions size−1..0,
  then re-adds in the new order.
- `onShuffle` (dispatcher.cpp:3179): needs `size >= 2` waiting tracks (was
  `< 3`), removes 0-based positions from the end, re-adds shuffled.

### D13 + §4.4 — `video`/`force` dropped; speed/bass ride `vc.play`
`vcPlayBody` (dispatcher.cpp:2512, declared in dispatcher.hpp:781) builds
`{chat_id, query, requested_by, speed: chatSpeedField(chat), bass:
chatIntField(chat, "bass", 0)}` — no `video`, no `force` (`-v` is accepted
and ignored). Used by all three direct `vc.play` sites: `onPlay`
(dispatcher.cpp:2485), `onRadio` (dispatcher.cpp:4208), `onPlayPlaylist`
(dispatcher.cpp:4616). This is the carrier §4.4 names and resolves inventory
§10 open question 6 (which call carries speed/bass). The user-facing
`/speed` / `/bass` replies stay honest as Phase 2 left them (saved,
backend cannot apply audio filters yet, live filtering lands with the
Phase 5 DSP); the values are record-only until Phase 5.

### §3.3 — `/play` replied media → `vc.play_file`
`playRepliedFile` (dispatcher.cpp:2632), called from `onPlay`
(dispatcher.cpp:2455): downloads via the TDLib bot session, then posts
`vc.play_file` `{chat_id, file_path, file_id, title, kind, requested_by}`
with its own `emitDispatchToIpc` telemetry line, then the shared
`finishPlay` (dispatcher.cpp:2526 — extracted onPlay tail: duration-limit
stop, `setCallActive`, last-track/logger bookkeeping, queued-vs-playing
card). Backend refusal answers the exact error, never a fake Now playing.
`duration_sec` is omitted (a `TgMessage` carries no duration for replied
media).

### D14 — `/download` routes Deezer/TikTok/Navidrome/Soulseek (§10-D14)
`onDownload` (dispatcher.cpp:3762): Spotify first (unchanged), then Deezer
(`deezer.com` substring or `deezer:` prefix, body `{query}`), TikTok
(`tiktok.com` substring or `tiktok:` prefix, body `{url}`), Navidrome
(`navidrome:` prefix only — a self-hosted URL has no fixed shape, mirrors
backend `ClassifyQuery`; body `{url}`), Soulseek (`soulseek:` prefix,
**prefix stripped** before sending body `{query}` — the old provider splits
on `" - "`), else YouTube `{query}`.

### D16 — `cbDl` FX honours `applied: false` (§10-D16)
`cbDl` (dispatcher.cpp:7333), FX branch ~dispatcher.cpp:7424: only an
explicit `applied: true` means the file was really processed. When false
(or absent), it answers the alert `<effect> recorded — live DSP filtering
lands in Phase 5. The file is unprocessed, so nothing was sent.` and sends
nothing — the returned path is the unprocessed input. `original`
(`applied:true`, a no-op by definition) still sends the file as before.

### E8 — `cbSpotify` routes details/analyze (§9-E8)
`cbSpotify` (dispatcher.cpp:7948) with file-local helpers
`spotifyDetailsText` (dispatcher.cpp:173) and `spotifyAnalyzeText`
(dispatcher.cpp:267), both defensive (accept `audio_features` or `features`,
`lyrics_url` or `lyrics_link`, `profanity` or `profane_words`;
`disliked_by`/`ignored_by` as arrays or integer counts; emoji-free HTML).
- `details`: `GET /v1/spotify/details?user_id=<id>&track_id=<id>`, silent ack
  + `editCbCard` with the details text + one `[Analyze text]` button
  (`pl:spotify:analyze:<id>`). The `[Links to other platforms]` / artist-page
  buttons were omitted: no backend action or URL source exists for them in
  the `spotify.details` response — no dead buttons.
- `analyze`: posts `spotify.analyze` directly, silent ack + edit with the
  analysis text.
- `dislike` keeps its exact current parsing (`parts[3]`,
  `spotify.dislike`); details/analyze rejoin `parts[3:]` (cbVote rule for
  opaque ids containing `:`). Any other verb → `This feature is not
  available yet.` alert.

### Item 9 — group-Music judgment call KEPT
Music in a group still opens the start card (G1/G2 fix). Not changed to the
search prompt — the user has not asked for that.

## Decisions recorded

### OD-10 — `/recognize` provider: AudD
- `phase3/actions-inventory.md` §9-E4 (line 2195): the OPEN "no provider"
  note replaced with: **DECIDED by the user 2026-11-03 ~16:57 IST — AudD
  (free tier, simple API)**; `audio.recognize` is built against AudD's HTTP
  API, env-gated on `AUDD_API_TOKEN`, honest 502 while unconfigured.
- `FINAL-ARCHITECTURE.md` §11, OD-10 row (line 950): same verdict. No other
  §11 row touched.

### Note under the §11 OD table (2026-11-03 ~17:23 IST, user decision)
FINAL-ARCHITECTURE.md:952: skip NetEase account creation, proceed with the
variables we have. No NetEase `MUSIC_U` is obtainable right now — mailbox
registration and Cloud Music phone login both need a mainland China (+86)
mobile for SMS, which the user has none of; WeChat login needs the phone app
(QR-only). The NetEase resolver stays env-gated and dormant for lossless;
anonymous search already works and is the only live NetEase path. Deezer is
also dropped for now (no ARL; sandbox geo-blocked signup — user: "Deezer
don't"); the Deezer resolver stays env-gated and dormant too. Neither blocks
verification — both fail honestly without keys. Variables we DO have: the
Spotify app "Parlay" exists in the user's dashboard (redirect URI
`http://127.0.0.1:8080/callback`) and the user is logged into AudD's
dashboard. The user copies the Spotify client ID/secret and the AudD token
into the VPS `.env` themselves — never through chat, NOT in the sandbox
`.env`. `/download` link routing (D14) is unchanged: Deezer links still
route to `download.deezer`, which fails honestly without an ARL.

## Routing table (`contracts/routing-table.md`, affected rows only)
- A64–A68 (lines 128–132): FX — `original` still sends the file
  (`applied:true`); slow/nightcore/bass/8d now answer the honest
  recorded/Phase 5 alert and send nothing (`applied:false`, record-only).
- A94 (line 158): details → `spotify.details`, analyze →
  `spotify.analyze`, Live; other verbs still "not available yet".
- C1 (line 205): replied media → `vc.play_file` (local path + `file_id`);
  backend refusal answers the exact error.
- C8 (line 212): `/loop` → `vc.loop` count 0–10, no queue copies.
- C13/C14/C15 (lines 217–219): display N → backend N−1; moving display #1
  allowed; shuffle needs 2 waiting tracks, removes 0-based from the end.
- C18/C19 (lines 222–223): speed/bass carried on `vc.play` into the room;
  reply text corrected from the stale "Speed set to Nx." / "Bass N." to the
  actual honest saved-not-applied reply.
- C25: already listed all `download.*` actions by link type — wording
  verified, no change.
- E8 (line 348): details/analyze now Live via the existing backend actions.
- 0 blank cells (awk check clean). No stale references remain ("loop
  copies", "Cannot move the currently playing track", "Speed set to",
  "no backend playback action" all gone).

## Tests

New section W in `services/core-cpp/tests/dispatcher_test.cpp:2698`
(`/loop 3` / `off` / no-args / clamp to 10 / not-streaming; `/remove 1, 2`;
`/move 1 3`, `/move 1 2`; `/shuffle` with 2 waiting tracks; `/clear` +
`/restore` all 3; `/queue` card lists all waiting tracks 1..3; `/download`
Deezer/TikTok/Navidrome/Soulseek/YouTube routing; `cbDl` FX
`applied:false` → toast + no audio, `applied:true` → audio sent; `cbSpotify`
details/analyze/dislike + unknown-verb toast). `FakeIpc` gained scripted
`loop`, `vc.play_file`, `fx`, `spotify.analyze`, `spotify/details` (GET),
queue `list` responses and a `post_bodies` vector; `has_post` /
`last_post_body` moved before §B so the rewritten §B replied-media tests
could use them. Updated in place: §B replied-media tests (now `vc.play_file`
behaviour, incl. backend-refusal case), §I stored speed/bass (now asserts
the `vc.play` body carries speed 1.5 / bass 40 and no `video`/`force`; the
old `vplay: video=true` assertion updated to assert the flags are dropped),
§J telemetry replied-file test (real IPC line now, `ok=1`, no `ipc=none`),
file header comment.

`services/core-cpp/tests/routing_test.cpp`: `FakeIpc` answers
`spotify.analyze` and `spotify/details` (GET) and `loop`; `pl:spotify:analyze:track1`
moved out of the §E unknown-callback list into a routed check (details card
edit + analyze card edit asserted). Its `fx` stub still returns no `applied`
field, so the harvested FX callbacks exercise the new recorded/honest
branch (toast asserted, still passes).

### Results (clean configure + build, 2026-11-03 ~17:25 IST)
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` from a wiped build dir,
  then `cmake --build build -j4`: **0 errors, 0 warnings**
  (`-Wall -Wextra -Wpedantic` on all targets).
- `dispatcher_test`: **278 PASS / 0 FAIL — ALL PASS** (baseline 227; +51
  new/updated checks).
- `routing_test` (absolute paths): **944 PASS / 0 failures** — 83 templates,
  116 harvested callbacks (same count as baseline; −2 removed unknown +2 new
  routed checks).
- Full `ctest`: **9/10 pass**. Only `store_test` fails — the pre-existing
  env-only failure (cannot connect to mongodb at 127.0.0.1:27099; redis also
  down). Same as the Phase 2 baseline. No new failures.

## Follow-ups for the verification / backend workers (not mine to do)
1. `services/core-cpp/src/ipc.cpp` (not owned): consider adding
   `"play_file"` → `"vc.play_file"` to `canonicalAction` — `vc.play_file`
   and `spotify.analyze` are currently posted with their dotted names
   directly because `canonicalAction` passes unknown names through unchanged.
2. `contracts/ipc.json` (not owned): should gain `speed`/`bass` in the
   `vc.play` request and the missing `vc.play_file` action entry.
3. `phase3/actions-inventory.md` §10-U4 (line 2412, outside my owned scope):
   still says the `/recognize` provider is undetermined — stale now that
   OD-10 records AudD. Flagged, not edited.
4. Test-harness limit (not a code gap): the `FakeTelegramSender`
   `editMessageText` drops the keyboard, so the `[Analyze text]` button on
   the details card cannot be asserted in `dispatcher_test` — the button's
   routing IS asserted by dispatching `pl:spotify:analyze:<id>`.
5. Unrelated working-tree change, not mine: `.env.example` is modified
   (another worker — Spotify redirect URI). I did not touch it.

## Live verification (honest limits)
Not possible from the sandbox: no real bot token / user session, no direct
TLS to Telegram DCs. All verification above is faked-backend unit tests
plus the clean build and routing-table harvest. Live MTProto verification
(`/loop` on a real call, replied-media play, `/download` Deezer link without
an ARL, details/analyze cards) is pending on the user's host.
