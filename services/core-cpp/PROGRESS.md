# core-cpp — Worker A progress

> **SUPERSEDED as a status snapshot (Phase 5, 2026-11-03).** This file is
> the Phase 2 (2026-11-02) build log, kept as history — do not read its
> "What is stubbed" section as current status. Superseded since:
> `LoggingVoiceCall` is deleted (real `TdLibVoiceCall` +
> `NtgCallsVoiceCall` + `OpusPipeline` backends, 7/7 TDLib tests);
> MongoDB/Redis are connected (store_test 40/40 vs real mongod/redis);
> the voice bridge (`voice_bridge.py`) was deleted in Phase 5; the vc-*
> and downloader services are real, tested services (see root
> `REMAINING.md` + each service README's Implemented section).

Date: 2026-11-02. Scope: `services/core-cpp/` only (read `refs/`, `contracts/ipc.json`, `ARCHITECTURE.md`, `PORT_MAP.md`). No push, no tokens, no secrets in files.

## Commands run

```sh
cmake -S . -B build && cmake --build build   # from services/core-cpp
./build/parlay-core                           # smoke test, no env
g++ -std=c++20 -I src /tmp/json_test.cpp src/json.cpp -o /tmp/json_test && /tmp/json_test
```

## Build result — PASS

- `cmake -S . -B build && cmake --build build` → exit 0, `[100%] Built target parlay-core`, **zero warnings** (`-Wall -Wextra -Wpedantic`).
- Toolchain: g++ 13.3.0, cmake 3.28.3. Binary: `build/parlay-core` (335,600 bytes).
- Note: first build took ~50 s wall clock; each TU compiles slowly in the sandbox (~5–8 s each). No errors.
- Smoke test: with no env, binary exits 1 with `parlay-core: missing required env vars: BOT_TOKEN PARLAY_TOKEN` — correct, no network attempted before config check.
- JSON test harness (`/tmp/json_test.cpp`, not committed): Telegram `getUpdates` payload, IPC success/error envelopes, `\u` escapes + surrogate pair (`café 🎵`), dump/parse round-trip with quotes/backslashes/newlines — all assertions pass.

## Environment findings (drove design decisions)

- No libcurl dev headers installed (only runtime `libcurl4`/`libcurl3t64-gnutls`) → HTTP via system `curl` CLI through `popen()` (`src/http.*`), body via temp file, args shell-quoted. No invented network library.
- No nlohmann/json installed, no dep download attempted → hand-rolled JSON parser/serializer (`src/json.*`).
- TDLib/ntgcalls not buildable here → voice isolated behind `VoiceCall` interface with a logging stub (`src/voice_call.hpp`).

## Files changed (all new except CMakeLists.txt / README.md / main.cpp replaced)

| File | What |
|---|---|
| `src/main.cpp` | Long-poll loop, config check, `getMe` login check, exponential backoff on poll errors, SIGINT/SIGTERM shutdown |
| `src/json.hpp`, `src/json.cpp` | Minimal JSON value, recursive-descent parser (incl. `\u`/surrogates), serializer |
| `src/http.hpp`, `src/http.cpp` | curl-CLI HTTP client, `POST JSON` + `GET`, status trailer parsing |
| `src/config.hpp`, `src/config.cpp` | Env-only config: BOT_TOKEN, API_ID, API_HASH, MONGO_URI, REDIS_URL, PARLAY_TOKEN, VC_GO_URL, VC_TS_URL, VC_RUST_URL, FX_URL, SHARE_URL, QUEUE_URL, DL_URL, RADIO_URL, PORT, OWNER_ID, PARLAY_ALLOW_CHATS, PARLAY_BLOCK_CHATS |
| `src/telegram_api.hpp`, `src/telegram_api.cpp` | Bot API client: `getUpdates` (30 s long poll), `sendMessage`, `getChatMember`, `getMe` |
| `src/ipc.hpp`, `src/ipc.cpp` | IPC client: `POST /v1/<action>` + `X-Parlay-Token`, `{ok:true,data}` / `{ok:false,error}` envelopes, `GET /healthz`, `unavailable()` classification |
| `src/engine_router.hpp`, `src/engine_router.cpp` | Per-chat engine map (default `go`), fallback order go→ts→rust rotated per preference, healthz probe before fallback engines, reports which engine won |
| `src/voice_call.hpp` | `VoiceCall` interface + `LoggingVoiceCall` stub (see below) |
| `src/dispatcher.hpp`, `src/dispatcher.cpp` | Command table + permissions + all handlers |
| `CMakeLists.txt` | C++20, `-Wall -Wextra -Wpedantic`, no external deps |
| `README.md` | Implemented vs stubbed, build/run instructions |

## What works (real)

- Commands: `/start /help /play /pause /resume /skip /queue /stop /search /download /radio /share /engine` — parsing strips `/`, `@bot` suffix, trims args; unknown command → "Try /help".
- IPC actions wired per `contracts/ipc.json`: `vc.play/pause/resume/skip/queue/stop` (with fail-over), `search.query` (vc-ts), `download.youtube`, `radio.start` → stream URL fed into `vc.play`, `share.create`.
- Fail-over: if the preferred engine is unreachable / 5xx / `engine_unavailable`, the next healthy engine is tried and the reply names the winning engine.
- Group management: `getChatMember` admin check gates playback + `/engine` in groups; private chats open; owner bypasses; `PARLAY_ALLOW_CHATS` / `PARLAY_BLOCK_CHATS` allow/block lists; per-chat rate limit (1 cmd/s).
- Oversize rule: `download.youtube` `oversize=true` → "queued for a later MTProto send", not a failure.
- Config: env names only, no values anywhere; required-var check at startup; secret-free startup summary.

## What is stubbed (and why)

- **Voice-call binding** — `VoiceCall` interface + logging stub only. TDLib/ntgcalls cannot build in this sandbox; voice is owned by the vc-* services over IPC, so routing works without it.
- **MongoDB / Redis** — env vars read and reported, not connected. Engine choice is in-memory; runtime queue state lives in the VC services.
- **MTProto (`API_ID`/`API_HASH`)** — read for the future voice path, unused today.
- **Core HTTP port / GUI `/api/*`** — not served yet; core only long-polls Telegram.
- **`/download` file delivery** — reports the downloader's `file_path`; `sendAudio`/MTProto send is a follow-up.
- **Inline queries, callbacks, playlists, loop/seek, autoplay, broadcast, sudo lists** — not in this command surface yet.

## Not tested live

No real `BOT_TOKEN` exists in this task and the vc-*/downloader services are stubs, so no live Telegram or IPC round-trip was possible. The code paths are exercised only by the JSON harness + the missing-env smoke test. Live verification needs: a bot token, the vc-go service up, `/play` in a group, kill vc-go mid-play to prove fail-over to vc-ts.
