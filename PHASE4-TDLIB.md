# Parlay — Phase 4: TDLib build + voice-join wiring

## 1. TDLib build outcome: SUCCEEDED

- Source: `~/workspace/parlay/phase4/tdlib-src` (TDLib **1.8.69**, `CMAKE_PROJECT_VERSION=1.8.69`)
- Build command (background, PID 18697, `-j2`): `cmake --build tdlib-src/build -j2`, cwd `~/workspace/parlay/phase4/tdlib-src`
- Started: ~2026-11-02 21:16 IST (PID elapsed 30:56 at the 21:47 IST first check)
- Finished: 2026-11-02 22:12 IST (log mtime 16:42:21 UTC)
- Duration: ~56 minutes
- Final progress: **[100%] Built target run_all_tests**
- Errors in log: **0** (`grep -ci "error:" tdlib-build.log` → 0)
- Progress observed: 64% (31 min) → 80% (43 min) → 95% (55 min) → 100% (~60 min). No restart, no kill, no second build was needed.

### Produced artifacts (`~/workspace/parlay/phase4/tdlib-src/build/`, total 518 MB)

| Artifact | Size | Notes |
|---|---|---|
| `libtdjson.so.1.8.69` (+ `libtdjson.so` symlink) | 50,166,944 B (~47.8 MB) | Shared JSON client library — the link target for core-cpp |
| `libtdcore.a` | 112,427,044 B (~107 MB) | Static core |
| `libtdapi.a` | 16,607,820 B | Generated TL API |
| `libtdmtproto.a` | 1,670,768 B | MTProto layer |
| `libtdclient.a` | 192,290 B | C++ client wrapper |
| `libtdjson_static.a` | 9,792 B | Static JSON shim |
| `libtdjson_private.a`, `libtdutils.a`, `libtdnet.a`, `libtdactor.a`, `libtddb.a`, `libtdsqlite.a`, `libtde2e.a`, `libtdtl.a` | — | Supporting static libs |
| `test/run_all_tests` | — | TDLib's own test binary, built 100% |

### Exact log tail

```
[ 97%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/port.cpp.o
[ 97%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/pq.cpp.o
[ 97%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/SharedObjectPool.cpp.o
[ 97%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/SharedSlice.cpp.o
[ 97%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/StealingQueue.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/variant.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/WaitFreeHashMap.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/WaitFreeHashSet.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdutils/test/WaitFreeVector.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdactor/test/actors_main.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdactor/test/actors_simple.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdactor/test/actors_workers.cpp.o
[ 98%] Building CXX object test/CMakeFiles/run_all_tests.dir/__/tdactor/test/actors_bugs.cpp.o
[100%] Linking CXX executable run_all_tests
[100%] Built target run_all_tests
```

Full log: `~/workspace/parlay/phase4/tdlib-build.log`

## 2. Wiring — core-cpp TDLib backend (DONE, independently verified)

Worker report: `phase4/tdlib-wiring-a.md`. Coordinator re-ran the build + smoke test and got identical results.

- **Choice: dlopen, not link.** `TdLibClient` dlopens `libtdjson.so` at runtime (`TDJSON_LIB` env, default `~/workspace/parlay/phase4/tdlib-src/build/libtdjson.so`); the binary still runs with TDLib absent. `CMakeLists.txt` needed no new link libs.
- **New files:** `services/core-cpp/src/tdlib_client.hpp` (151 lines) + `src/tdlib_client.cpp` (~330 lines): client create/destroy, send/receive with timeout, execute, `call()` matched on `@extra`, `setLogVerbosity`, auth state machine `authorize()` (WaitTdlibParameters → setTdlibParameters → WaitPhoneNumber → setAuthenticationPhoneNumber → WaitCode → checkAuthenticationCode → WaitPassword → checkAuthenticationPassword → Ready), missing credential at any step → `false` + honest "not authorized: no credentials" stop.
- **New class `TdLibVoiceCall`** (`src/voice_call.hpp/cpp`): TDLib is MTProto signaling only — TDLib does not do audio itself. Flow: authorize → `NtgCallsVoiceCall::createOffer()` (local SDP) → `getChat` → `video_chat.group_call_id` (or `createVideoChat` when 0) → `joinVideoChat(local SDP)` → `Text.text` = remote transport payload → `ntg.connect(remote)` → `ntg.setSources(ffmpeg cmd)`. `stop()` best-effort `leaveGroupCall`. Schema facts read from `td/generate/scheme/td_api.tl` (TDLib 1.8.69), not assumed.
- **Factory order** (`makeVoiceCall`): TdLib (when TD_API_ID + TD_API_HASH + TD_SESSION_DIR set and both `libtdjson.so` + `libntgcalls.so` load) → VoiceBridge → dlopen ntgcalls → UnavailableVoiceCall with the exact reason. New env names only (no values): `TD_API_ID` (falls back to `API_ID`), `TD_API_HASH`, `TD_SESSION_DIR`, `TDJSON_LIB`, `TD_PHONE`, `TD_CODE`, `TD_PASSWORD`; `summary()` gains a secret-free `tdlib=configured|unset` flag.
- **Build (clean, re-verified by coordinator):** `rm -rf build && cmake -S . -B build && cmake --build build` → exit 0, `[100%] Built target parlay-core`. 8 warnings total, all pre-existing GCC 13 `-Wmaybe-uninitialized` false positives in `store.cpp`; new code compiles with zero warnings.
- **Offline smoke test (no network, no credentials) — `tests/tdlib_test.cpp`, 7/7 PASS (coordinator re-ran: identical):** `getOption(version)` via synchronous `td_execute` returned the genuine `optionValueString "1.8.69"`; async `getChat` without auth returned a real TDLib error (code 400, "Initialization parameters are needed: call setTdlibParameters first"); `authorize()` without credentials fails honestly in ~0 s; `makeVoiceCall` does not pick tdlib without TD_* env; `TdLibVoiceCall::play` without auth returns false. No join faked. `ldd` on `libtdjson.so` beforehand: all deps resolvable.
- **Existing test:** `tests/store_test.cpp` — 30 PASS / 2 FAIL (pre-existing, environmental: no MongoDB on 127.0.0.1:27099; `store.cpp` untouched by this work).

## 3. Wiring — vc-ts / vc-rust voice transport (DONE, independently verified)

Worker report: `phase4/tdlib-wiring-b.md`. Coordinator re-ran both test suites and got identical results.

Both services now share the same real transport shape (TinaMusic `JoinGroupCall` flow), implemented independently in each language:
1. **Opus/RTP pipeline** — ffmpeg: source → libopus 128k, 48 kHz, stereo → Ogg Opus file (ffprobe-verified) AND a second ffmpeg run packetizing the same audio as RTP to a **file** + SDP (`-f rtp -sdp_file`). Same shape as core-cpp `OpusPipeline`. RTP count proven by parsing the file (SSRC-anchored scan).
2. **Local SDP offer** — ntgcalls 3.0.5 wheel in `phase4/voice-venv` (`NTgCalls().create_call(chat_id)` via `src/ntg_offer.py` in each service). Purely local WebRTC, no MTProto, no network, no credentials.
3. **Join client** — drives the same `services/core-cpp/voice_bridge.py` (Telethon + ntgcalls) in `join-call` mode, watching stdout JSON for `{"event":"joined"}` / `{"event":"error"}` / `{"ok":false,"error":...}`, bounded at 30 s. Reports the exact failure; `joined:true` is never returned without a real join.
4. **Wiring** — `play()` runs the pipeline then attempts the join; now-playing reports `voice` (`"opus-rtp-ready"` once the pipeline is proven, `"opus-file-only:<reason>"`, or `"unavailable:<exact reason>"`) and `voice_joined` (false unless a real join happened). Queue/IPC behaviour untouched.

### Files changed
- **vc-ts:** `src/voicecall.js` rewritten (was a stub; exports `opusPipeline`, `createLocalOffer`, `join`, `playStream`, `joinVoiceChat`/`leaveVoiceChat`, `voiceState`, `status`, `backendConfig`, `probeUdp`, `countRtpPackets`); `src/ntg_offer.py` new; `src/main.js` now-playing gains `voice`/`voice_joined`/`voice_reason`, `startTrack()` calls async `playStream()` fire-and-forget; `tests/run.js` +5 voice tests; README/PROGRESS updated.
- **vc-rust:** `src/voice.rs` new, std-only (`RealVoiceCall`, `opus_pipeline`, `count_rtp_packets`, `probe_udp`, `backend_config_from[_env]`, `create_local_offer`, `join`/`join_with_config`, per-chat `ChatVoice` state); `src/ntg_offer.py` new; `src/main.rs`: `LoggingVoiceCall` deleted, `static VOICE: LazyLock<RealVoiceCall>`, now-playing voice from `VOICE.voice_string()`; README/PROGRESS updated.
- **Not changed:** `contracts/ipc.json` (no `vc.join` action exists — coordinator decision whether to add one); `services/core-cpp/*` untouched by worker B.

### Opus/RTP proof numbers (5 s 440 Hz tone, offline, both services — identical to the core-cpp Phase 4 proof)

| measure | vc-ts | vc-rust |
|---|---|---|
| Opus frames (ffprobe) | 251 | 251 |
| Opus file bytes | 83,736 | 83,736 |
| Duration (ffprobe) | 5.0065 s | 5.0065 s |
| RTP packets (file parse, seq contiguous) | 251 | 251 |
| RTCP Sender Reports | 1 | 1 |
| RTP file bytes | 85,973 | 85,973 |
| SDP | `m=audio 0 RTP/AVP 97`, `a=rtpmap:97 opus/48000/2` | same |
| First RTP bytes | `80 c8` RTCP SR, then `80 e1` PT 97 | same |

Local ntgcalls offer (offline, voice-venv wheel): 456–476 chars, contains `ufrag` + `fingerprints`. End-to-end over HTTP (vc-rust): `POST /v1/play` → now-playing `voice:"opus-rtp-ready"`, `joined:false`.

### Exact failing-call evidence (live join path) — nothing worked around, no success faked
1. **UDP sendto EPERM — even to loopback.** Python `socket.sendto(b'x', ('127.0.0.1', 9999))` → `PermissionError: errno=1 Operation not permitted` (same for `8.8.8.8:53`). Node dgram: `EPERM (send EPERM 127.0.0.1:1)`. Rust `UdpSocket::send_to`: `EPERM (os error 1)`. ffmpeg `-f rtp rtp://127.0.0.1:9999` → `Error submitting a packet to the muxer: Operation not permitted`, exit 255. Consequence: RTP proven to a **file** only; no live UDP receiver reachable, `rtp_udp_ok` stays false.
2. **No MTProto session / credentials (the join gate).** `join()` with no env returns: `voice join unavailable: ASSISTANT_SESSION_FILE unset; API_ID unset; API_HASH unset`. No process spawned, no network attempted.
3. **Bridge with a dummy session.** Empty session file → bridge prints `{"ok": false, "error": "ValueError"}`, exit 1; nonexistent path → `{"ok": false, "error": "FileNotFoundError"}`, exit 1. No real session fabricated; no Telethon TCP connect attempted (direct TCP to Telegram DCs is proxy-blocked per Phase 4).

## 4. Coordinator verification — all prior tests re-run

| Suite | Result | Notes |
|---|---|---|
| TDLib build (PID 18697) | 100%, 0 errors, ~56 min | libtdjson.so.1.8.69 50,166,944 B |
| core-cpp clean rebuild | exit 0, `[100%] Built target parlay-core` | 8 pre-existing warnings in store.cpp only |
| core-cpp `tests/tdlib_test.cpp` | **7/7 PASS** | Real TDLib 1.8.69 responses, no credentials |
| core-cpp `tests/store_test.cpp` | 30 PASS / 2 FAIL | Both fails pre-existing: no MongoDB on :27099 |
| vc-ts `node --test tests/run.js` | **24/24 PASS** (was 19, +5 voice) | Coordinator re-ran: identical |
| vc-rust `cargo test --release --offline` | **17/17 PASS** (was 9, +7 voice) | 0 warnings; coordinator re-ran: identical |
| vc-go `go test ./...` | ok (cached) | 6/6 from Phase 2, no regression signal |

## 5. What remains blocked and why

1. **No live voice join has happened, and none can happen from this sandbox.** The `authorize()` → `getChat` → `createVideoChat` → `joinVideoChat` → `ntg.connect` chain is written and compiles, but the signaling half (real remote payload) and the `ntg.connect` half are untested against real Telegram. Needs: a host with `TD_API_ID`/`TD_API_HASH`/`TD_SESSION_DIR` (+ phone/code/password for first auth, or a pre-authorized session dir) and `libntgcalls.so` present, then one `/play` through the tdlib backend. (Same gate for the VoiceBridge path: `ASSISTANT_SESSION_FILE` + `API_ID` + `API_HASH`.)
2. **`libntgcalls.so` is absent in this sandbox** (`dlopen: No such file or directory`), so `TdLibVoiceCall::available()` is false here even when TD_* are set. Needs a `libntgcalls.so` build; the Python ntgcalls wheel (phase4/ntg-extract, voice-venv) works for the bridge path only.
3. **Live RTP delivery is impossible in this sandbox** — UDP sendto is EPERM even to loopback, proven in three languages + ffmpeg. Even with a join, the ntgcalls WebRTC media path cannot be proven here; it is proven only up to the local offer + the bridge's `connect(remote)` call, which needs the live join first.
4. **IPC gap (coordinator decision):** no `vc.join` action in `contracts/ipc.json` and no HTTP join endpoint in core-cpp; the vc services drive `voice_bridge.py` directly. The vc-side `join(chat_id, localSdp)` interface is already shaped for it if join-over-IPC is wanted.
5. **NetEase download still `engine_unavailable`** (weapi encryption, Phase 3) — untouched by this task. MongoDB/Redis still JSON-file stores. No live Telegram test (needs BOT_TOKEN + assistant session, not supplied). Nothing pushed to GitHub; no tokens/secrets in the tree.
