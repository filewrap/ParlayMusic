# Phase 6 — Worker C (honest-limit fixes + stub re-grep)

Date: 2026-11-03. No commit, no live Telegram test, no credentials read
(`.env` untouched). Mongo aside, English only.

## Context: a first Worker C already did items 1+2

When this run started, the tree already contained a complete
implementation of items 1 (broadcast `errors.txt` + `sendDocument`)
and 2 (speedtest upload probe) plus tests — credited in
`services/core-cpp/README.md` and `REMAINING.md` ("Phase 6
follow-up (Worker C, 2026-11-03) — full verification + B11
(broadcast + speedtest upload DONE, /speed+/bass recorded)").
That earlier Worker C (C1) also ran every suite (442 passed / 0
failed / 11 skipped across all services) and left notes in
`phase6/` (gitignored). This worker (C2, same brief) therefore
verified C1's items 1+2, closed their remaining gaps, did item 3
in full, and did item 4. The parent may want to reconcile the two
Worker C runs; nothing below re-does C1's work, and the code/tests
below are this worker's own additions on top of it.

## (1) /broadcast errors file — verified, one gap closed

C1's implementation is real and tested: `TelegramApi::sendDocument`
(`services/core-cpp/src/telegram_api.cpp`, multipart via
`HttpClient::postMultipart` in `src/http.cpp`), `onBroadcast` uses
it with an inline fallback when the upload itself fails, and
`tests/dispatcher_test.cpp` §F (4 blocks) + `tests/telegram_api_test.cpp`
§A–C prove the multipart body, the filename sanitisation and the
error paths against a local stub server.

Gap closed by this worker: the file listed bare chat ids, but the
task asks for "failed chats + error" — and no per-chat error text
exists, because the Bot API `forwardMessage`/`copyMessage` return
only success/failure (bool). So the file now starts with a header
line saying exactly that, then one id per line
(`services/core-cpp/src/dispatcher.cpp:2977-2981`). The dispatcher
broadcast test was updated to assert the header + the failed id
only (no other chat's id leaks in). Nothing else was missing: the
`sendDocument` multipart mechanism is proven offline by
telegram_api_test; a live Telegram send still needs the pending
live verification (see §Open).

## (2) /speedtest upload — verified, tightened, test gap closed

C1's implementation is real: `onSpeedtest` POSTs a generated
payload to Cloudflare `__up` via `HttpClient::postRaw` and reports
up+down, with an honest failure line when the upload fails.
`telegram_api_test.cpp` §D proves the `postRaw` mechanism
byte-exact (512 KiB incl. NULs) against a local stub.

Changes by this worker:

- Tightened the bound to match the brief ("small payload, short
  timeout"): upload 8 MB / 60 s → **4 MB / 30 s**
  (`dispatcher.cpp:3135-3140`). Download probe unchanged (25 MB /
  40 s, NoxBeats parity).
- Extracted the pure parts as testable seams so the handler's math
  and message text are proven without network:
  `Dispatcher::speedMbps` and `Dispatcher::formatSpeedtest`
  (`dispatcher.cpp:3093-3115`, declared in `dispatcher.hpp:69-72`),
  with 5 checks in `dispatcher_test.cpp` §H (20 Mbps math, 0-byte /
  0-second edges, up+down text, failed-upload text).
- Live endpoint check (bounded, 2 curls): `GET
  https://speed.cloudflare.com/__down?bytes=1000000` → HTTP 200 in
  1.4 s; `POST` of 256 KB and of 100 KB to `__up` → both time out
  (30 s / 20 s, HTTP 000), and a 100 KB POST to httpbin.org times
  out identically. Conclusion: **the sandbox egress stalls POST
  bodies while GETs succeed** — the probe cannot show a live upload
  number from this sandbox, through no fault of the code. The
  endpoint itself is Cloudflare's real speed-test upload endpoint.
  Live upload proof needs a host with open egress; the handler
  reports the failure honestly in the meantime (documented in
  `services/core-cpp/README.md` "Honest limits").

## (3) /speed + /bass — per-engine verdict + core-voice wiring

### Verdict: no vc engine can apply it; the core backend can

| Engine | Evidence (file:line) | Can it apply speed/bass? |
|---|---|---|
| vc-go | `internal/server/server.go:317` `handleSeek` only updates room position state; **0 `ffmpeg` hits in the whole service** — no audio path at all, queue/state only | No — needs a whole audio subsystem |
| vc-ts | `src/voicecall.js:254` `opusPipeline(source, opts)` — fixed ffmpeg args `-i … -map 0:a -c:a libopus -b:a 128k -ar 48000 -ac 2 -f opus` (:280) and the RTP variant (:299); `opts` carries only `workDir`; no `-af`, no `atempo`, no `-ss`. `src/main.js:609-618` `/v1/seek` only sets `room.position_sec` + `started_at_ms` | No — needs a filter parameter + re-encode on seek/play |
| vc-rust | `src/voice.rs:322` `opus_pipeline(source, work_root)` — same fixed ffmpeg args, no filter parameter. `src/main.rs:1288-1314` `handle_seek` only clamps `room.position_sec` | No — same as vc-ts |

No new IPC action was added: there is no field to send the value
through, because no engine has anywhere to receive it. Applying it
in the engines needs a new audio-filter subsystem (filter parameter
through the opus pipeline + re-encode on seek/play), not a core
change. That is now stated in `COMMANDS.md` ("Not yet") and
`services/core-cpp/README.md` ("Honest limits").

The core's own voice backend **can** apply it, and that is now
wired for real:

- `buildFfmpegCmd(src, seek, speed, bass)`
  (`services/core-cpp/src/voice_call.cpp:53-96`) builds
  `-af "atempo=X"` + a lowpass bass mix; `VoiceCall::play(…,
  seek_sec, speed, bass)` carries the values, and
  `TdLibVoiceCall::play` (`voice_call.cpp:649-700`) re-attaches the
  source via `setSources` when already joined — a real re-encode.
  Position comes from `VoiceCall::time(chat_id)`
  (`voice_call.cpp:384-390`, `:729`).
- `Dispatcher::applyCoreFilter` (`dispatcher.cpp:612-652`,
  declared `dispatcher.hpp:127-128`): for core-owned sessions
  (`voice_owner == "core"`) it re-attaches the recorded source at
  `voice_.time()` with the new speed/bass, and replies with the
  position. It also answers the nothing-playing and
  no-recorded-source cases honestly instead of claiming success.
- `playRepliedFile` (`dispatcher.cpp:1165-1186`) now passes the
  chat's stored speed/bass into `voice_.play` and records
  `core_source` / `core_title` / `core_video` chat fields, so a
  value set earlier applies to the next core play and later
  `/speed`/`/bass` can re-attach the same source. New helper
  `chatSpeedField` (`dispatcher.cpp:601-610`).
- `onSpeed` / `onBass` (`dispatcher.cpp:1783`, `:1849`) branch to
  the core path **before** the engine `nowPlaying` check (engines
  know nothing about a core session). The engine path is unchanged
  except the reply is now honest: "Speed 1.5x saved, but the \<engine\>
  engine cannot apply audio filters — the stream restarted at the
  current position at the old speed. It applies to core-voice
  playback (replied files)." Previously it claimed "Speed set to
  1.5x" while changing nothing.
- `COMMANDS.md` table rows for `/speed` / `/bass` and the "Not yet"
  bullet, plus `services/core-cpp/README.md` "Honest limits", now
  state the split exactly.

### Tests (13 new checks, `tests/dispatcher_test.cpp` §G)

FakeVoice now records `seek_sec`/`speed`/`bass` per play and has a
scriptable `time_value`; FakeIpc gained a scriptable now-playing +
seek success (`np_ok`). Checks: core `/speed` re-attaches at 42 s
with 1.5x and no engine seek; core `/bass` re-attaches with bass 40
+ stored speed 1.25x; core session with no recorded source → honest
reply, no re-attach; core session with no active call → "Nothing is
playing."; engine `/speed` → engine seek issued + honest
saved-not-applied reply + core voice untouched; replied-file `/play`
passes stored speed/bass to the core voice and records
`core_source`. All pass (see §Builds/tests).

## (4) Stub re-grep — classification, nothing left to fix

Command (same scope as the audit: `services/`, excluding `refs/`,
`.venv`, `node_modules`, `build/`, `target/`):

```
grep -rn -E "stub|TODO|FIXME|XXX|HACK|NotImplemented|unimplemented|engine_unavailable|placeholder|dummy|mock|pass-through|demo|fake|hardcoded" services/ --exclude-dir=refs --exclude-dir=.venv --exclude-dir=node_modules --exclude-dir=build --exclude-dir=target
```

Result: **540 lines** (Worker B's audit: 593 — see note below).
Split: **203 non-test code hits, 188 test-file hits, 149 doc
hits** (`.md`). TODO 0, FIXME 0, HACK 0, NotImplemented 0,
unimplemented 0, pass-through 0. Per term: stub 217,
`engine_unavailable` 118, demo 82, fake 85, placeholder 23, mock 22,
dummy 5, hardcoded 3, XXX 1. (Binary matches excluded from the
count: `vc-ts/src/ntg_addon.node`, one downloaded `.mp3` under
`vc-rust/data/downloads/`, and `downloaders` `__pycache__/*.pyc` —
build artifacts, not source.)

### Classification of all 203 non-test code hits: legitimate, 0 still-stubbed

- **`engine_unavailable` (~118)** — the real contract error code,
  used everywhere a backend is down or a resolve fails
  (`core-cpp/src/ipc.{hpp,cpp}`, `engine_router.cpp`, `vc-ts/src/
  resolvers.js` + `main.js`, `vc-go` server/resolver, `vc-rust`
  `main.rs`, `downloaders/src/{util,providers,ytdlp,navidrome}.py`,
  `netease-go` server, `audio-fx-rust`, `spotify-rust`). Not a stub.
- **Comments documenting honesty / deletions (~45)** — "never a
  fake …", "no placeholder fallback (A9)", "NOT a stub", "never
  fakes success" in `vc-rust/src/main.rs`, `voice_call.hpp`,
  `voicecall.js`, `spotify-rust/src/main.rs`, `dispatcher.cpp`,
  `core_api.hpp`, `http_server.hpp` (test-stub infrastructure
  comments). Not stubs; they describe removed stubs.
- **Test doubles inside `#[cfg(test)]` modules (~40)** — stub HTTP
  servers in `spotify-rust/src/auth.rs:542+` and
  `classifier.rs:310+`, fake-180 assertions in
  `vc-rust/src/main.rs:1732+`, `dummy-offer` in
  `vc-rust/src/voice.rs:1047` (inside `mod tests`). Test-only by
  construction.
- **Dev-only labelled demo state, gui-js (46)** —
  `services/gui-js/app/main.js`: local-demo queue/now-playing served
  ONLY when `CORE_URL` is unset, always labelled
  `source:"local-demo"`, with an honest note; configured-core
  failures are real errors, never demo state (Phase 6, A2). Intended
  dev behaviour, not a stub.
- **False positives (~10)** — `demote_measured_slow` in
  `downloaders/src/soulseek/{selection,downloader}.py` (contains
  "demo" as a substring of "demote"); the local variable name
  `fake` in `dispatcher.cpp:731+` (a synthetic `CommandContext`
  for engine queries, not fake data); `"demo"` as a Soulseek
  version-penalty keyword in `scorer.py:63` (a real metadata
  heuristic); the public Navidrome demo URL in doc comments
  (`navidrome.py:10-20`, the real public demo instance).
- **XXX 1** — the `mkstemp` template `"/tmp/parlay-core-body-XXXXXX"`
  (`core-cpp/src/http.cpp:109`). False positive, not a marker.
- **Doc hits (149)** — `engine_unavailable` mentions plus
  historical Phase 3/4 "Still stubbed" sections in
  `services/*/PROGRESS.md`, each with an explicit SUPERSEDED note.
  Their "still true" remainders are documented limits, not code
  stubs: gui-js has no Next.js build / album browse (Mongo aside
  per user order; GUI auth is a new feature); netease-go audio
  recognition is dropped (B7) and QR login is unimplemented (needs a
  user account flow); radio-js has no listener endpoint (no contract
  action) and no audio relay (needs the A1-blocked live join); vc-ts
  live voice join is A1-blocked (sandbox UDP EPERM) and the Lemonic
  keyed path is untested (key-gated).

**Nothing was fixed in this item because nothing fixable was
found**: zero still-stubbed non-test code hits, and zero
non-key-gated gaps. The key-gated items stay env-gated and honest
(Gemini, Spotify OAuth, Deezer ARL, NetEase MUSIC_U, Genius/
Musixmatch, Lemonic key, Soulseek account). The only live gaps are
the documented engine audio-filter subsystem (§3) and the A1 live
voice join — both need new subsystems / live network, not a grep
fix.

Count note vs Worker B (593 → 540): demo 114 → 82 and
`engine_unavailable` 120 → 118, stub 212 → 217, XXX 2 → 1. The
commands were not byte-identical between workers (B's exact pattern
is not quoted in REMAINING.md) and files changed since (Worker A
demo gating, C1's test additions, this worker's edits); the
classification above was done line-by-line from this run's output
(`/tmp/workerc-grep.txt`, code-only subset
`/tmp/workerc-grep-codent.txt`), so the counts stand on their own.

## Builds / tests (this worker, 2026-11-03)

`cmake --build build -j4` in `services/core-cpp`: clean, no
warnings. Test binaries built per each file's header command.

| Suite | Result |
|---|---|
| `dispatcher_test` | **81/81 PASS, 0 fail** (was 63/63: +13 §G speed/bass + 5 §H speedtest helpers) |
| `telegram_api_test` | **24/24 PASS, 0 fail** |
| `http_api_test` | **37/37 PASS, 0 fail** |
| `router_test` | **20/20 PASS, 0 fail** |
| `tdlib_test` | **ALL PASS, 0 failed, exit 0** (25 PASS lines executed: 24 stdout + 1 stderr; source has 26 `CHECK` invocations incl. the macro def — C1's "26/26" counts source invocations, this run counts executed lines; no failure either way) |
| `store_test` (live mongod :27017 + redis :6399) | **40/40 PASS, 0 fail** |
| **Core-cpp total** | **227 checks, 0 failed** |

Environment note: `store_test` needed its DBs — mongod 8.0.4 was
started from `~/workspace/parlay/.tools/mongodb/bin/mongod`
(dbpath `/tmp/parlay-mongo-test`, :27017) and `redis-server` was
installed via apt (it was absent from this sandbox) and started on
:6399. Both are left running, matching the Phase 4 state. No other
service's suite was re-run: this worker's changes touch only
core-cpp + docs.

## Files changed by this worker

- `services/core-cpp/src/dispatcher.cpp` — `chatSpeedField`
  (:601), `applyCoreFilter` (:612), `playRepliedFile` stores
  `core_source` + passes stored speed/bass (:1165-1186), core
  branch + honest engine reply in `onSpeed`/`onBass` (:1783,
  :1849), `speedMbps`/`formatSpeedtest` (:3093-3115), upload probe
  4 MB / 30 s (:3135-3140), broadcast `errors.txt` header (:2977).
- `services/core-cpp/src/dispatcher.hpp` — `chatSpeedField` /
  `applyCoreFilter` (:117-128), `speedMbps` / `formatSpeedtest`
  (:69-72).
- `services/core-cpp/tests/dispatcher_test.cpp` — FakeVoice
  records filter args + scriptable `time()`; FakeIpc scriptable
  now-playing/seek; §G (13 checks) + §H (5 checks); broadcast
  content assertion updated for the header line.
- `COMMANDS.md` — `/speed` / `/bass` rows + "Not yet" bullet
  (engine-only limit, core applies).
- `services/core-cpp/README.md` — "Honest limits" speed/bass +
  speedtest bullets, dispatcher test count 63/63 → 81/81.

Uncommitted, as instructed. The tree also still carries Workers
A+B + C1's uncommitted changes; this worker's edits are the ones
listed above (plus a concurrent worker's overlapping COMMANDS.md /
README.md wording on the broadcast + speedtest rows, which this
worker left in place where accurate).

## Open / for the parent

- **Live Telegram verification still pending for everything here**
  (as for all Phase 6 work): broadcast `errors.txt` to a real chat,
  `/speedtest` upload on a host with open egress, `/speed` + `/bass`
  on a real core-voice session (replied file) and on an engine
  session. None of it can be proven from this sandbox.
- **Sandbox POST stall**: `POST` bodies stall at the egress proxy
  (verified with 100 KB POSTs to both Cloudflare `__up` and
  httpbin.org; GETs fine). This also bounds any live `sendDocument`
  proof from the sandbox — the multipart mechanism is proven
  offline only.
- **Reconcile the two Worker C runs**: C1's `REMAINING.md` section
  ("Phase 6 follow-up (Worker C)") says "/speed+/bass recorded" and
  lists dispatcher 63/63, tdlib 26/26, total 442. After this run
  the core-cpp total is 227 checks (81 dispatcher) and the
  all-services total is 442 + 18 = 460 if nothing else changed.
  REMAINING.md was left untouched by this worker (Worker B owns the
  audit section) — suggested update: B11 `/speed`/`/bass` is now
  "applied on core-voice sessions, engine-only seek-restart";
  `/broadcast` errors file and `/speedtest` upload stay DONE.
