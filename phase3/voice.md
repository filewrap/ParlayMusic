# Phase 3 — Voice (cgo ntgcalls) — Worker 3 report

Date: 2026-11-03. Anchor: main at 3d84fb6. No commit, no push (Wave 2 owns that).
Module `parlay/backend-go` already existed when this work started (Worker 1
had landed `go.mod`, go 1.23.0); only `internal/voice` was added under it.
`~/workspace/parlay/.env` was never read, touched, or executed against; all
tests use dummy chat ids and locally rendered audio only. No service was
started with the user's token. No Phase 5 work was done (no DSP, no
symphonia, no Opus encode).

## Files

- `services/backend-go/internal/voice/voice.go` — cgo `Client` binding
  (New, CreateCall, Connect, SetStreamSources, SetExternalAudioSource,
  SendExternalFrame, Pause/Resume/Mute/Unmute, Stop, GetState, Time,
  Version, Close, OnStreamEnd, OnConnectionChange).
- `services/backend-go/internal/voice/callbacks.go` — exported C
  callbacks + handle→Client registry.
- `services/backend-go/internal/voice/manager.go` — per-chat lifecycle
  `Manager` + `JoinFunc`.
- `services/backend-go/internal/voice/feeder.go` — interim persistent
  ffmpeg PCM feed (see §Frame path).
- `services/backend-go/internal/voice/voice_test.go` — 13 tests, all
  against the real `libntgcalls.so` 3.0.5.

## Binding decisions

- **cgo flags follow TgMusicBot** (`refs/TgMusicBot/main.go:12`):
  `-L <lib dir> -lntgcalls -lm -lz -lresolv`, header from
  `refs/ntgcalls/shared-output/include/ntgcalls.h` (identical to
  TgMusicBot's bundled copy — `diff` clean). No header was copied into
  the backend; the include path points at the real header.
- **One correction to the TgMusicBot pattern.** Its `MediaSource`
  constants use `1<<iota`, which diverges from the C enum after Shell
  (C: Unknown=0 File=1 Shell=2 FFmpeg=3 Device=4 Desktop=5 External=6;
  TgMusicBot: FFmpeg=4 Device=8 Desktop=16 External=32). This package
  maps every enum value to its C constant explicitly, so a
  `MediaSourceExternal` actually reaches ntgcalls as 6.
- **Link fix, glibc 2.39.** The verbatim TgMusicBot flags do NOT link
  on this host: `ld` fails with `undefined reference to __dn_expand`
  and `__res_nquery`. The prebuilt library carries unversioned undefined
  refs to those symbols (they lived in libresolv on the glibc it was
  built against; on glibc 2.39 they live in libc). The runtime loader
  resolves them from libc without complaint (verified: a C probe linked
  with the flag runs and prints `3.0.5`). Fix, in `voice.go`:
  `-Wl,--allow-shlib-undefined`. It only permits undefined symbols
  inside shared libraries; Go objects are unaffected. `-lresolv` is kept.
- **rpath.** The link line also bakes an rpath to
  `refs/ntgcalls/shared-output/lib` so `go test` and local runs find
  the library without `LD_LIBRARY_PATH`. Release packaging must replace
  this with `$ORIGIN` (library shipped next to the backend binary) —
  the sandbox path must not ship.
- **Symbols re-verified with `nm -D`** at build time (all 18 bound
  symbols present as `T` in `libntgcalls.so`, addresses recorded in the
  work log; `TestRequiredSymbols` re-checks the whole set on every test
  run, failing by name if one is absent). The cgo link itself also
  fails loudly on a missing referenced symbol.
- **Callbacks follow TgMusicBot** (`ntgcalls.go`): a global
  handle→Client registry, `//export` Go functions cast to the C
  callback types, handlers invoked on ntgcalls threads. The Manager
  dispatches its own handler work to a goroutine so a slow handler
  never blocks a library thread.
- **Errors carry the library's own text.** `ntg_last_error()` is
  preferred; fallback is `ntgcalls error <code>`. Client errors carry
  no operation prefix — the caller names the operation/stage once, so a
  join failure reads `voice: join chat …: connect: ntgcalls error -102:
  Invalid JSON`, with each name exactly once.

## Correction: CreateCall does NOT return SDP text

The task brief and the UDS contract call the CreateCall output a
"local SDP offer" / `local_sdp`. Measured against the real library,
it is ntgcalls' **JSON params object**, not SDP:

```json
{"ufrag":"BZEz","pwd":"kJqKtwNCjgttLTc6xSrj7PQu",
 "fingerprints":[{"hash":"sha-256","setup":"passive","fingerprint":"27:09:…"}],
 "ssrc":2062178642,
 "ssrc-groups":[{"sources":[…],"semantics":"SIM"}, …]}
```

476 bytes, produced in 35–183 ms. This is the same value TgMusicBot
passes to `PhoneJoinGroupCall` as `DataJson` and the old core
`voice_call.cpp` passed to the join step. The code keeps the contract
names (`localSDP`, `local_sdp`) but documents the value as the params
JSON, which the core must pass **verbatim** to TDLib `joinVideoChat`
as params. Nothing in this package parses it as SDP or reformats it.
`TestCreateCallProducesParams` asserts the JSON shape (ufrag, pwd,
sha-256 fingerprint, non-zero ssrc).

## Frame-path verdict: (1) NO — ntg_set_stream_sources cannot take a URL or file

Investigated in order, against the ntgcalls source in
`refs/ntgcalls/ntgcalls/src/` and TgMusicBot's usage:

| Source | What the code actually does | Verdict |
|---|---|---|
| `FILE` | `io/file_reader.cpp`: opens a local path with `std::ifstream` and streams raw bytes straight to the sink. No decoder, no HTTP. | A URL fails to open as a file; an MP3/FLAC/Opus file would be fed to WebRTC as raw s16le garbage. Unusable for resolved streams. |
| `FFMPEG` | `media/media_source_factory.cpp`: `throw FFmpegError("FFmpeg encoder is not yet supported")`. | Explicitly unimplemented in 3.0.5. |
| `SHELL` | `io/shell_reader.cpp`: runs a shell command, reads raw PCM from stdout. TgMusicBot (`internal/calls/ffmpeg.go`) and the old `voice_call.cpp` (`buildFfmpegCmd`, `setSources`) both use this with an ffmpeg decode-to-s16le command. | Works, but it is one ffmpeg subprocess spawned by ntgcalls per track — the locked §1.2.3 prohibition on the primary path. Not used. |
| `EXTERNAL` | `media/stream_manager.cpp:250` `send_external_frame` → `AudioStreamer::send_data`, which treats bytes as 16-bit samples at the declared rate/channels (`bits_per_sample = 16`). No reader is created; the caller pushes frames. | **The only usable path.** |

**Frame geometry (measured from source, not assumed).**
`media/audio_sink.cpp`: `frame_size() = sample_rate * 16 / 8 / 100 *
channel_count`, `frame_time() = 10 ms`. At the declared 48 kHz stereo
that is **480 samples, 1920 bytes, 10 ms per frame** — `send_data`
hands exactly `frame_size() / (2 * channel_count)` samples to WebRTC
per call, so a mis-sized buffer would be silently truncated or
over-read. `SendExternalFrame` therefore enforces exactly 1920 bytes.
Note: the architecture's 960-sample / 20 ms figure is the DSP frame;
Phase 5 must split each 960-sample DSP frame into two 480-sample
external frames (or the DSP adapter must emit 480-sample frames).

## Interim frame path (Phase 5 replacement target)

`services/backend-go/internal/voice/feeder.go` — `startFFmpegFeed`,
`ffmpegFeed.pump`, `ffmpegFeed.Pause/Resume/Stop`,
`ffmpegFeed.terminalErr`.

- **Why it exists:** no in-library decode path exists for a resolved
  stream URL or compressed file (verdict above), and parlay-dsp does
  not exist yet. Until Phase 5, decoded PCM has to come from somewhere.
- **What it is:** ONE persistent ffmpeg process per room
  (`ffmpeg [-reconnect …] -i <source> -vn -f s16le -ac 2 -ar 48000
  pipe:1`), started when a track starts, restarted only on track
  change — never per call, never per frame. Source is passed as argv,
  never through a shell. The pump reads 1920-byte frames and pushes
  them via `SendExternalFrame` on a real-time schedule (frame N no
  earlier than start + N·10 ms), so a fast decoder cannot flood the
  WebRTC audio source.
- **What replaces it (Phase 5):** parlay-dsp decodes to f32, applies
  the effect chain, dithers once to s16le, and Go pushes the resulting
  480-sample frames through the same `SendExternalFrame`, with no
  ffmpeg anywhere. This file is replaced whole; the `Client` API it
  uses does not change. The interim nature is marked in the file's
  header comment.
- **Failure honesty:** a missing input file closes stdout with zero
  bytes, which reads as plain EOF — so end-of-stream is decided by
  the ffmpeg exit status, not the read error. A failed decode reports
  the exact ffmpeg reason including its stderr tail, e.g.
  `ffmpeg exited: exit status 254: Error opening input: No such file
  or directory / Error opening input file /nonexistent/no-such-file.flac.`
  An intentional stop suppresses the feed-end event (the caller knows
  it stopped the feed); a clean ffmpeg exit reports nil (track ended).
- **Pause finding.** `StreamManager::update_pause` iterates only
  `readers_`; external sources live in `external_readers_`, so
  `ntg_pause`/`ntg_resume` are no-ops for the external source
  (measured: `changed=false` on a created call). The feed therefore
  pauses itself: the pump stops consuming ffmpeg stdout, the pipe
  fills, ffmpeg blocks on write, and playback resumes from the exact
  paused sample. At most one in-flight frame can slip through after
  Pause lands (measured 21→22 frames in 600 ms paused; tolerated ≤1
  in the test). **Mute is different:** `update_mute` iterates
  `tracks_`, which exist for capture devices regardless of source, so
  the library-level mute works for the external source (measured
  `changed=true`); no feed gating is needed for mute. The pause/mute
  bool returned by ntgcalls is the "changed" flag, not the resulting
  state — the Manager tracks Paused/Muted itself.

## Call lifecycle manager

`manager.go` — `Manager`, `JoinFunc`, `Call`, `CallState`.

- `type JoinFunc func(ctx context.Context, chatID int64, localSDP string) (remoteSDP string, groupCallID int64, err error)` — exact
  signature as specified. The voice package never calls TDLib and
  never touches the socket; Wave 2 wires this to the UDS server's
  `JoinRequest` (contracts/uds-protocol.md §6).
- `Join(ctx, chatID)` runs create_call → JoinFunc → connect. A failure
  at any stage returns an error naming the exact stage and reason,
  records `StateFailed` with `FailedStage`/`FailedReason`, tears the
  half-created call down best-effort, and never reports success.
  Joining an already-connected chat returns the existing group call id.
  A connection-change callback (Failed/Timeout/Closed) marks a live
  call failed with stage `connection`.
- `Play(ctx, chatID, source)` requires a connected call, binds the
  external audio source, and starts the room's feed; a previous
  track's feed is stopped first. `Pause/Resume/Mute/Unmute/Stop`
  delegate to the client (plus feed gating for pause), update the
  record, and fail with `no active call` when there is none.
  `OnFeedEnd` fires when a track decodes fully (nil) or the feed dies
  (exact error) — the queue-advance hook for Wave 2.
- `Close()` stops every call and destroys the instance.

## Measured results

`go test -count=1 ./internal/voice/` — **13 passed, 0 failed**
(3.3 s); `go test -race` — **pass**; `go vet ./internal/voice/` —
**clean**; `gofmt` — clean; zero placeholder markers in the package. Full-module `go vet ./...` fails in another worker's
package (`internal/resolver/soulseek.go:147`, assignment mismatch) —
not this package, not touched.

| Test | Result |
|---|---|
| `TestRequiredSymbols` — all 18 bound symbols in `nm -D` | pass |
| `TestVersion` — `3.0.5` | pass |
| `TestCreateCallProducesParams` — 476-byte JSON params, 35–183 ms | pass |
| `TestConnectRejectsBadPayload` — exact error `ntgcalls error -102: Invalid JSON` | pass |
| `TestSendExternalFrameRequiresSource` — exact error `ntgcalls error -206: External source not initialized`; succeeds after binding the external source | pass |
| `TestPauseMuteUnmuteStopOnCreatedCall` — pause/resume `changed=false`, mute/unmute `changed=true`, `GetState` unmuted after unmute | pass |
| `TestManagerJoinWithoutJoinFunc` — `voice: join chat …: join: no JoinFunc configured: …` | pass |
| `TestManagerJoinStageErrors` — join stage: `…: join: joinVideoChat: GROUPCALL_FORBIDDEN (test double)`; connect stage: `…: connect: ntgcalls error -102: Invalid JSON` | pass |
| `TestManagerPauseWithoutCall` / `TestManagerPlayRequiresConnectedCall` — `no active call` | pass |
| `TestFeedEndToEnd` — 2 s tone decoded to exactly **200 frames** (10 ms each), clean EOF | pass |
| `TestFeedRejectsBadSource` — exact ffmpeg stderr (see §Frame path) | pass |
| `TestFeedPauseResume` — 0–1 frames while paused, resumes after | pass |

## Exact blockers / honest limits

1. **No real voice join is possible from the sandbox** — no session,
   no direct TLS to Telegram DCs. `Join` is proven only up to the
   handshake boundary: a real local params JSON is generated, a fake
   `JoinFunc` receives it, and both failure stages surface exactly.
   A live join (Wave 2 wiring + a host that reaches Telegram) is the
   only way to prove `Connect` with a real remote params JSON and a
   live `ConnectionConnected` callback. Nothing here fakes a join.
2. **`Manager.Play` is not proven end-to-end** — it requires a
   connected call, which requires (1). The feed itself is proven
   end-to-end against a created call (200 frames, clean EOF).
3. **Callbacks from a live call are unobserved** — `OnStreamEnd` /
   `OnConnectionChange` registration is proven (no error, registry
   wired), but no stream-end or connection-change event can fire
   without a live call; the first live join must confirm both fire.
4. **Release packaging must replace the rpath** (see §Binding
   decisions) and confirm the `-Wl,--allow-shlib-undefined` flag on
   the target host's glibc; on an older glibc the flag is harmless
   but the rpath is sandbox-specific.
5. `TestFeedPauseResume` tolerates one slipped frame after Pause
   (in-flight frame); this is inherent to the 10 ms pump schedule,
   not a bug.
