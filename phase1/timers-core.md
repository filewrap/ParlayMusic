# Phase 1 — core-cpp stage timers (`t_dispatch_to_ipc`, `t_voice_join`, `t_first_frame`)

Worker A — core-cpp stage. Anchor: `d5065b9`. No behaviour changed, no IPC
contract change, no code deleted, no Phase 2 work. Nothing committed or
pushed (coordinator owns that). `~/workspace/parlay/.env` untouched; the
only live-binary run used dummy creds (`BOT_TOKEN=dummy-token-for-smoke`).

## What was added

### New file: `services/core-cpp/src/telemetry.hpp` (header-only)

- `telemetry::nowUs()` — microseconds from `std::chrono::steady_clock`
  (monotonic; immune to NTP/TZ jumps).
- `telemetry::emit(name, total_us, fields)` — one line to stdout (the same
  channel as the existing `[cmd]` / `[voice]` lines), serialized by a
  mutex so the HTTP-API thread and the dispatcher thread never interleave
  mid-line.
- `telemetry::sanitize(value)` — whitespace runs become `_`, `=` becomes
  `:`, truncated to 200 chars, so a long error cannot flood the log or
  break `key=value` parsing.
- `telemetry::Timer` — accumulating stage timer (`stage(name, us)` +
  `emit(name, extra)`).

### Exact log format

```
[telemetry] <name>_us=<integer> [key=value ...]
```

- `t_dispatch_to_ipc` — one line per `/play` (see §1).
- `t_voice_join` — one line per voice-join attempt (see §2).
- `t_first_frame` — one line per `OpusPipeline::run` (see §3).

Honesty rule (enforced by the code and the tests): a stage that never ran
is **omitted** from the line, never zero-filled; a failed attempt emits
with `ok=0`, the stage it failed at (`failed_stage=`), and the exact
failure reason (`error=`, sanitized). A `*_us` value of `-1` means "no such
packet/stage was produced". Totals on a failed attempt are
**time-to-failure**, not a successful-stage time. Nothing is ever
fabricated.

## 1. `t_dispatch_to_ipc` — command receipt → engine IPC

Files: `services/core-cpp/src/dispatcher.hpp:41-45`
(`CommandContext::receipt_us`), `services/core-cpp/src/dispatcher.cpp`
(`telemetry.hpp` include :23; helpers :495-529; receipt capture :531-534
and :776; emission :2335, :2340, :2378-2391).

- Receipt is stamped at the top of `Dispatcher::handleMessage` and carried
  on the routing `CommandContext`.
- Engine-play path (`onPlay`, the `vcCall(ctx, "play", ...)` call):
  `dispatch_to_post_us` = receipt → `vcCall` entry, `post_to_response_us`
  = `vcCall` entry → response. "POST issued" is measured at the `vcCall`
  entry (the core handing the play to the IPC layer); the router's engine
  selection and its healthz pre-probes for a fallback engine run *inside*
  `vcCall`, so they fall in `post_to_response_us`. The total is exact
  either way, and `dispatch_to_post_us + post_to_response_us == total`
  (asserted by test).
- The line fires whether the engine answers or not (`ok=0` + `error=` on
  failure, e.g. `error=not_found` on the stub 404).
- Replied-file `/play` and usage-error `/play` have no engine IPC post,
  so their lines carry `ipc=none` instead of the two sub-fields, and
  `ok=` reflects the outcome (`ok=1` on a successful replied-file play,
  `ok=0 error=replied_file_failed` on download/voice failure,
  `ok=0 error=no_query` on the usage error). The voice-join stages of
  those plays are still timed by `t_voice_join`.
- Denials before any play is attempted (not a group, play not allowed, no
  channel linked) emit nothing — no play was attempted.
- Only signature change in this work: `Dispatcher::playRepliedFile` now
  returns `bool` (was `void`) so the timer line can report its outcome
  honestly. Its only caller is `onPlay`; behaviour is otherwise identical.

Verbatim lines from the stubbed test run:

```
[telemetry] t_dispatch_to_ipc_us=784 dispatch_to_post_us=712 post_to_response_us=72 action=play chat_id=-1051 engine=go fell_back=0 ok=1
[telemetry] t_dispatch_to_ipc_us=279 ipc=none action=play chat_id=-1003 ok=0 error=no_query
[telemetry] t_dispatch_to_ipc_us=4617 ipc=none action=play chat_id=-1001 ok=1
```

(The stubbed engine answers instantly, so these magnitudes are tiny —
712 µs of dispatch work before the post, 72 µs for the stub round-trip.
Real magnitudes come from the live path; the engine worker's
`BENCHMARKS.md` is the record for those.)

## 2. `t_voice_join` — join stages

Files: `services/core-cpp/src/voice_call.cpp` (`emitVoiceJoin` :280-300,
`stageUs` helper; `TdLibVoiceCall::joinCall` :875-949;
`TdLibVoiceCall::join` :951-957; `TdLibVoiceCall::play` :959-990;
`NtgCallsVoiceCall::join` :455-468; `NtgCallsVoiceCall::play` :470-511),
`services/core-cpp/src/voice_call.hpp` (`joinCall` declaration :243;
`UnavailableVoiceCall` emission :95-112).

- The join sequence in `TdLibVoiceCall::join` and `::play` was factored
  into one private method, `joinCall(chat_id, title, video)`, so both
  paths are timed once. Step order and error strings are unchanged:
  authorize → `create_call` (local SDP offer) → `get_chat` →
  `create_video_chat` (only when `group_call_id == 0`) →
  `join_video_chat` → `connect` (remote SDP); `joined_` is recorded only
  on success, exactly as before.
- Stage fields are emitted in execution order:
  `authorize_us create_call_us get_chat_us [create_video_chat_us]
  join_video_chat_us connect_us`, plus `chat_id backend=tdlib ok=1|0`
  and, on failure, `failed_stage=<stage> error=<exact reason>`.
  `authorize_us=0` means "already authorized, no authorize call needed".
- `NtgCallsVoiceCall::join` / `::play` (no MTProto session) also emit:
  `backend=ntgcalls-dlopen`, `failed_stage=join` (or `create_call` /
  `set_sources` / `available` when the failure is earlier), with the
  exact reason. `UnavailableVoiceCall::play` / `::join` emit with
  `failed_stage=available` and the backend's reason.
- Already-joined chats emit nothing — no join is attempted.

Verbatim line from a mock join (scripted TDLib, **real** libntgcalls.so
offer + **real** `ntg.connect` rejecting the mock payload):

```
[telemetry] t_voice_join_us=41288 authorize_us=0 create_call_us=37763 get_chat_us=75 join_video_chat_us=82 connect_us=3343 chat_id=-1001234567890 backend=tdlib ok=0 failed_stage=connect error=ntgcalls_connect_failed:_ntgcalls_connect_failed_(-102):_Invalid_JSON
```

What that one line already says about the live path: the local SDP offer
(`create_call`) dominates at ~37.8 ms; the scripted TDLib round-trips are
~75–82 µs each; the real `connect` fails in ~3.3 ms on a bad payload.
Total (41,288 µs) = stage sum (41,263 µs) + 25 µs of inter-stage time.

## 3. `t_first_frame` — ffmpeg Opus/RTP pipeline

Files: `services/core-cpp/src/voice_call.cpp` (`scanOggAudioPage` :80-101,
`runFfmpegPipe` :103-186, `OpusPipeline::run` rewrite :609-758).

- Encode leg: the same ffmpeg invocation as before, but the Ogg output
  now goes to a pipe (teed byte-for-byte into `opus_out`, so the file
  and the ffprobe verification are unchanged) and Ogg pages are parsed
  incrementally as bytes arrive. The first page with
  `granule_position > 0` is the first 20 ms Opus packet leaving the
  encoder (granule = end sample of its last packet, 960 per 20 ms frame
  at 48 kHz); header (BOS) pages carry granule 0.
- Fields: `resolve_us` (yt-dlp `-g`, URL inputs only),
  `stream_open_us` (run start → first output byte = headers written),
  `first_packet_us` (run start → first Ogg audio page — the headline
  `t_first_frame_us` value), `encode_total_us`, `rtp_us` + `rtp_ok`,
  `frames`, `bytes`, `ok`, `error` / `rtp_error`.
- The RTP leg is unchanged (separate ffmpeg `-f rtp` run, same command)
  and reports its wall time + ok flag. **The sender cannot observe a UDP
  packet on the wire, so no RTP first-packet time is fabricated.** In
  production with working UDP the first RTP packet leaves
  ~`first_packet_us` after the RTP leg spawns (same encoder, same input,
  same settings as the encode leg — the RTP muxer transmits each packet
  as soon as it is encoded).
- Dead ends investigated and rejected (documented so nobody re-tries
  them): ffmpeg `-progress` writes only a final summary for sub-second
  encodes (no per-frame updates); polling the Ogg file's size measures
  ~encode completion, not the first packet (first byte appeared at
  174/179 ms of a 5 s encode — the muxer flushes at the end).

Verbatim line from the sandbox run (5 s 440 Hz tone, loopback RTP sink):

```
[telemetry] t_first_frame_us=121528 stream_open_us=98714 first_packet_us=121528 encode_total_us=159593 rtp_us=100438 rtp_ok=0 frames=251 bytes=83736 ok=1 rtp_error=ffmpeg_rtp_output_failed_(exit_255)_—_UDP_may_be_blocked_in_this_sandbox
```

251 frames / 83,736 bytes matches the README's standalone opus proof
exactly. First packet at ~122 ms, encode done at ~160 ms (startup
dominates; the encode itself runs at ~30× realtime).

## What could and could not be measured live in the sandbox

Measured live (real, not stubbed):
- `t_dispatch_to_ipc` on the stubbed `/play` path (engine answers
  instantly; magnitudes are stub magnitudes, structure is real).
- `t_voice_join` stages up to `connect` with the **real**
  libntgcalls.so: `create_call` produces a real local SDP offer
  (>100 bytes, ufrag), `connect` is really invoked and really rejects
  the mock payload. Only the TDLib side is scripted (FakeTdLib).
- `t_first_frame` encode leg end-to-end with real ffmpeg 8.1.2:
  first-packet time, stream-open time, encode total, 251 real Opus
  frames verified by ffprobe.

Could NOT be measured live, and why (all honest, none faked):
- A successful real voice join: no session dir, no phone/QR first-auth,
  no direct TLS to Telegram DCs from the sandbox. The join code exists
  and is correct; every stage up to `connect` is proven against the real
  library, and a failure at any stage emits the stage + exact reason.
- A successful RTP transmission / first RTP packet on the wire: UDP
  `sendto` is EPERM in this sandbox, even to loopback (README's opus
  proof records the same). The RTP leg reports `rtp_ok=0`, exit 255,
  `Operation not permitted`, and the file encode is unaffected. The
  first-packet time that *is* reported is the encode-side one.
- URL resolution (`resolve_us`): yt-dlp is not installed in the sandbox,
  so `resolveUrl` fails fast for http(s) inputs; local files skip
  resolution (field omitted). The timing hook is in place for hosts
  with yt-dlp.
- Real `/play` magnitudes (engine resolve times, real dispatch→post):
  need the live VPS + real engines. The timer plumbing is in place;
  `BENCHMARKS.md` (other worker) is where those numbers land.

## Tests

New test file `tests/telemetry_test.cpp` (19 checks) — build line is in
its header. Extended `tests/dispatcher_test.cpp` §J (6 checks) and
`tests/tdlib_test.cpp` §7 (7 checks). No stubs, no dead code; the only
new test-only helpers are stdout-capture + line/field parsers.

Full core-cpp results (run from `services/core-cpp`, which matters for
the tdlib relative lib path):

| Suite | Result |
|---|---|
| dispatcher_test | 234 passed / 0 failed (228 baseline + 6 new) |
| tdlib_test | 51 passed / 0 failed (44 baseline + 7 new) |
| ui_test | 159 passed / 0 failed |
| router_test | 20 passed / 0 failed |
| telegram_api_test | 73 passed / 0 failed |
| http_api_test | 71 passed / 0 failed |
| scram_test | 40 passed / 0 failed |
| telemetry_test (new) | 19 passed / 0 failed |
| store_test | 19 passed / 2 failed — **pre-existing environmental** (mongo/redis down in sandbox; same 2 as the Phase 9 baseline, untouched code) |
| **Total** | **667 passed / 0 code failures** |

`parlay-core` builds clean via CMake (`-Wall -Wextra -Wpedantic`, no
warnings). Smoke run of the built binary with dummy creds only
(`BOT_TOKEN=dummy-token-for-smoke`, `PARLAY_DATA_DIR=/tmp/parlay-smoke`,
10 s timeout): starts cleanly, HTTP API on :8080, voice backend honestly
unavailable (no TD_API_ID/TD_API_HASH; libntgcalls.so not found from
that cwd), `getMe` fails 404 on the dummy token and the binary exits 1
with an honest message. No `/play` was issued (no Telegram connection
possible with dummy creds), so no `[telemetry]` lines in that run —
expected.

## Files changed (this worker only)

- `services/core-cpp/src/telemetry.hpp` — new (header-only timer + emit).
- `services/core-cpp/src/dispatcher.hpp` — `CommandContext::receipt_us`.
- `services/core-cpp/src/dispatcher.cpp` — receipt capture, `onPlay`
  emission (3 paths), `playRepliedFile` now returns `bool`.
- `services/core-cpp/src/voice_call.hpp` — `joinCall` declaration,
  `UnavailableVoiceCall` emission.
- `services/core-cpp/src/voice_call.cpp` — `runFfmpegPipe` + Ogg page
  parser, `OpusPipeline::run` timing, `joinCall` refactor,
  ntgcalls-dlopen emission.
- `services/core-cpp/tests/telemetry_test.cpp` — new (19 checks).
- `services/core-cpp/tests/dispatcher_test.cpp` — §J (6 checks).
- `services/core-cpp/tests/tdlib_test.cpp` — §7 telemetry (7 checks).
- `phase1/timers-core.md` — this report.

Not touched: `BENCHMARKS.md` (engine worker), `contracts/ipc.json`,
`.env`, any engine service. (Note: the working tree also contains
uncommitted changes in `services/vc-go`, `services/vc-rust`,
`services/vc-ts` — those are the parallel engine worker's, not mine.)

## Open points for the coordinator / live VPS run

- The first real `t_voice_join ok=1` line and the real `create_call` /
  `connect` times can only come from the user's VPS with a real
  assistant session.
- The first real `t_first_frame` with `rtp_ok=1` likewise needs a host
  where UDP works; the sandbox number to beat is `first_packet_us`
  ≈ 90–470 ms (startup-dominated, varies with load — the probe runs
  ranged 89–472 ms for first audio page).
- `t_dispatch_to_ipc` on the live path will show the real engine
  resolve time inside `post_to_response_us` (YouTube 6.5–13.7 s per
  BENCHMARKS.md is the current expectation for that field).
