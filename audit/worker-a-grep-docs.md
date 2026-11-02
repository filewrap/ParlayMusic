# Audit Worker A — grep + docs (Parlay)

- Date: 2026-11-03. Scope: `~/workspace/parlay/`, audit only — read + grep, no code modified, no push, no tokens.
- Method: read PHASE2.md, PHASE3.md, PHASE4.md, PHASE4-TDLIB.md, DEEP_READ.md, COMMANDS.md, BENCHMARKS.md, ARCHITECTURE.md, README.md; `grep -rniE` over `services/` excluding `refs/ .venv node_modules build target phase4 tdlib-src`. All 12 `services/*/README.md` read in full; 2–3 claims per README spot-checked against code.
- Headline: **zero TODO/FIXME/HACK/mock in the tree. Zero live stubs left unaddressed except voice joining.** The only real, user-facing gap is that **no audio reaches a Telegram voice chat from anywhere** (all three VC engines + core report it honestly). The bigger audit finding is **documentation drift**: the root README, ARCHITECTURE.md, core-cpp README/PROGRESS, vc-rust README/PROGRESS, and netease-go README all contradict Phase 4 code, and `docker-compose.yml` wires ports the services do not listen on.

## 1. Hit counts

- Total grep lines (all keywords, case-insensitive): **218**
- By keyword (a line can match >1 keyword): stub 129, engine_unavailable 64, placeholder 10, "not implemented" 10, passthrough 5, dummy 4, NotImplemented 1, "not supported" 1, XXX 1, TODO 0, FIXME 0, HACK 0, mock 0, unimplemented 0, pass-through 0.
- Split: doc-only (README.md + PROGRESS.md) **113**; test files **51** (vc-ts/tests/run.js 33, gui-js/tests/run.js 11, downloaders/tests/test_providers.py 2, plus 5 lines in README/PROGRESS that also match "test"); non-test, non-doc code **59**.
- The XXX hit is a false positive: `services/core-cpp/src/http.cpp:44` mkstemp template `"/tmp/parlay-core-body-XXXXXX"`.

## 2. Full hit list (all 218 lines, file:line + exact text)

services/core-cpp/src/http.cpp:44:        char tmpl[] = "/tmp/parlay-core-body-XXXXXX";
services/core-cpp/src/ipc.hpp:19:    std::string error_code;    // e.g. "engine_unavailable", "bad_request"
services/core-cpp/src/ipc.hpp:26:               error_code == "engine_unavailable";
services/core-cpp/src/ipc.cpp:16:        out.error_code = "engine_unavailable";
services/core-cpp/src/ipc.cpp:68:        out.error_code = "engine_unavailable";
services/core-cpp/src/engine_router.hpp:5:// is down or returns engine_unavailable, the core retries the same call on
services/core-cpp/src/engine_router.cpp:53:            last.error_code = "engine_unavailable";
services/core-cpp/src/voice_call.hpp:3:// The old logging stub is gone. Three real implementations behind one
services/core-cpp/src/voice_call.hpp:77:// Reports "unavailable" with the concrete reason. This is NOT a stub
services/core-cpp/src/voice_call.hpp:262:// UnavailableVoiceCall with the exact reason (never a silent stub).
services/core-cpp/src/dispatcher.hpp:11:// and formats the result. No handler replies with a stub.
services/core-cpp/src/store.cpp:709:            "mongodb URIs with credentials are not supported by the minimal "
services/core-cpp/src/tdlib_client.cpp:298:                " (registration / QR / email flows are not implemented; "
services/core-cpp/README.md:12:result. Engine fallback: on `engine_unavailable`/timeout, retry the next
services/core-cpp/README.md:76:  queue through real IPC calls (list → rebuild), never a stub reply.
services/core-cpp/README.md:113:  URIs are rejected honestly — SCRAM auth is not implemented; run
services/core-cpp/README.md:121:  not a stub. Who plays: the **assistant/userbot account**, never the bot
services/core-cpp/README.md:189:## Honest limits (not stubs — reported as limits)
services/core-cpp/README.md:197:  file_id hand-off is not implemented yet (link/search/reply-text
services/core-cpp/README.md:206:- **Mongo SCRAM auth** is not implemented; the wire client rejects
services/core-cpp/PROGRESS.md:25:- TDLib/ntgcalls not buildable here → voice isolated behind `VoiceCall` interface with a logging stub (`src/voice_call.hpp`).
services/core-cpp/PROGRESS.md:38:| `src/voice_call.hpp` | `VoiceCall` interface + `LoggingVoiceCall` stub (see below) |
services/core-cpp/PROGRESS.md:41:| `README.md` | Implemented vs stubbed, build/run instructions |
services/core-cpp/PROGRESS.md:47:- Fail-over: if the preferred engine is unreachable / 5xx / `engine_unavailable`, the next healthy engine is tried and the reply names the winning engine.
services/core-cpp/PROGRESS.md:52:## What is stubbed (and why)
services/core-cpp/PROGRESS.md:54:- **Voice-call binding** — `VoiceCall` interface + logging stub only. TDLib/ntgcalls cannot build in this sandbox; voice is owned by the vc-* services over IPC, so routing works without it.
services/core-cpp/PROGRESS.md:63:No real `BOT_TOKEN` exists in this task and the vc-*/downloader services are stubs, so no live Telegram or IPC round-trip was possible. The code paths are exercised only by the JSON harness + the missing-env smoke test. Live verification needs: a bot token, the vc-go service up, `/play` in a group, kill vc-go mid-play to prove fail-over to vc-ts.
services/core-cpp/voice_bridge.py:143:        except NotImplementedError:
services/vc-go/README.md:63:## What is still stubbed, and why
services/vc-go/PROGRESS.md:17:  server, graceful shutdown + final persist. (Replaced the stub.)
services/vc-go/PROGRESS.md:32:- `README.md` — rewritten: real vs stubbed, endpoints, run instructions.
services/vc-go/PROGRESS.md:80:## Real vs stubbed
services/vc-go/PROGRESS.md:86:- STUBBED / out of scope by design: ntgcalls voice joining and audio
services/vc-ts/src/main.js:396:      { code: "engine_unavailable" }
services/vc-ts/src/main.js:611:        return fail(res, 502, "engine_unavailable",
services/vc-ts/src/main.js:654:    if (e.code === "engine_unavailable") return fail(res, 502, "engine_unavailable", e.message);
services/vc-ts/src/voicecall.js:46:// stub. Nothing here throws: every failure is a returned reason.
services/vc-ts/src/resolvers.js:12:// engine_unavailable — results are never faked.
services/vc-ts/src/resolvers.js:61:// Error type that maps to the contract's engine_unavailable code.
services/vc-ts/src/resolvers.js:65:    this.code = "engine_unavailable";
services/vc-ts/src/resolvers.js:125:  // IP): keep a placeholder so play/queue still work, and say so.
services/vc-ts/src/resolvers.js:136:      resolve_note: "yt-dlp resolve failed (missing or YouTube 429); degraded placeholder",
services/vc-ts/src/resolvers.js:149:    resolve_note: "yt-dlp search failed (missing or YouTube 429); degraded placeholder",
services/vc-ts/src/resolvers.js:292:// unavailable or fails do we return engine_unavailable — and the error
services/vc-ts/PROGRESS.md:16:- `src/main.js` — replaced stub. HTTP server, per-chat rooms, 1 s
services/vc-ts/PROGRESS.md:23:- `src/voicecall.js` — voice transport stub (see README: why).
services/vc-ts/PROGRESS.md:26:- `README.md` — rewritten: real vs stubbed, endpoints, run instructions.
services/vc-ts/PROGRESS.md:37:`{"ok":true,"service":"vc-ts","engine":"ts","yt_dlp":true,"yt_dlp_path":".../downloaders/.venv/bin/yt-dlp","jiosaavn":"jsvn-tgvc.vercel.app","lemonic_configured":false,"voice":{"voice":"stubbed",...},"rooms":0}`
services/vc-ts/PROGRESS.md:77:  `sources: {youtube:"ok", jiosaavn:"ok", lemonic:"engine_unavailable",
services/vc-ts/PROGRESS.md:82:  `{"ok":false,"error":{"code":"engine_unavailable","message":"Lemonic
services/vc-ts/PROGRESS.md:108:## Still stubbed / untested
services/vc-ts/PROGRESS.md:110:- Voice joining (ntgcalls/tgcalls) — stubbed, see README.
services/vc-ts/PROGRESS.md:117:  placeholder resolve covers it but never auto-advances.
services/vc-ts/PROGRESS.md:143:  engine_unavailable naming what was tried; `via` in responses,
services/vc-ts/PROGRESS.md:145:  sandbox — verified via stub API (no-auth vs Bearer) + dead-API
services/vc-ts/PROGRESS.md:148:  JioSaavn/LRCLib/yt-dlp, stub Lemonic, panel/loop/radio regressions).
services/vc-ts/PROGRESS.md:156:- `src/voicecall.js` rewritten (was a stub): ffmpeg Opus/RTP pipeline
services/vc-ts/PROGRESS.md:170:  (`ASSISTANT_SESSION_FILE` / `API_ID` / `API_HASH` unset); with a dummy
services/vc-ts/tests/run.js:2:// a child-process server + a stub Lemonic API on localhost.
services/vc-ts/tests/run.js:6:// the Lemonic tests hit a local stub so keyed vs unauthenticated
services/vc-ts/tests/run.js:12:// 18109 stub Lemonic API. Data dirs under /tmp/p4-vcts/.
services/vc-ts/tests/run.js:82:// ------------------------------------------------------- stub Lemonic API --
services/vc-ts/tests/run.js:83:const stubHits = []; // { path, authHeader }
services/vc-ts/tests/run.js:84:let stubServer = null;
services/vc-ts/tests/run.js:86:function startStubLemonic(port) {
services/vc-ts/tests/run.js:88:    stubServer = http.createServer((req, res) => {
services/vc-ts/tests/run.js:89:      stubHits.push({ path: req.url, auth: req.headers.authorization || null });
services/vc-ts/tests/run.js:97:            { id: "stub1", title: "Stub Song One", artist: "Stub Artist", duration: 200, url: "https://example.invalid/stub1.mp3" },
services/vc-ts/tests/run.js:98:            { id: "stub2", title: "Stub Song Two", artist: "Stub Artist", duration_sec: 150, url: "https://example.invalid/stub2.mp3" },
services/vc-ts/tests/run.js:107:            title: "Stub Raw Track",
services/vc-ts/tests/run.js:108:            artist: "Stub Artist",
services/vc-ts/tests/run.js:118:    stubServer.listen(port, () => resolve());
services/vc-ts/tests/run.js:129:  await startStubLemonic(18109);
services/vc-ts/tests/run.js:144:  if (stubServer) stubServer.close();
services/vc-ts/tests/run.js:193:    const j = await voicecall.join(VOICE_CHAT, "dummy-offer");
services/vc-ts/tests/run.js:226:  assert.ok(v.voice.length > 0, "voice state is always a real string, never a stub label");
services/vc-ts/tests/run.js:420:test("lemonic search unauthenticated (stub): results, no Authorization header", async () => {
services/vc-ts/tests/run.js:421:  stubHits.length = 0;
services/vc-ts/tests/run.js:422:  const r = await req(18102, "POST", "/v1/lemonic/search", { query: "stub" });
services/vc-ts/tests/run.js:426:  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
services/vc-ts/tests/run.js:427:  assert.ok(hit, "stub saw /api/search");
services/vc-ts/tests/run.js:431:test("lemonic search keyed (stub): Bearer header sent, same results", async () => {
services/vc-ts/tests/run.js:432:  stubHits.length = 0;
services/vc-ts/tests/run.js:433:  const r = await req(18103, "POST", "/v1/lemonic/search", { query: "stub" });
services/vc-ts/tests/run.js:436:  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
services/vc-ts/tests/run.js:441:test("lemonic raw track via stub (unauthenticated) + download route", async () => {
services/vc-ts/tests/run.js:442:  const r = await req(18102, "POST", "/v1/lemonic/download", { track_id: "stub1" });
services/vc-ts/tests/run.js:443:  // The stub's stream URL is example.invalid, so the download fetch
services/vc-ts/tests/run.js:445:  // read fine), not engine_unavailable.
services/vc-ts/tests/run.js:450:test("lemonic search with dead API + no Spotify creds: honest engine_unavailable naming both fallbacks", async () => {
services/vc-ts/tests/run.js:453:  assert.equal(r.json.error.code, "engine_unavailable");
services/vc-ts/README.md:49:(`bad_request`, `unauthorized`, `not_found`, `engine_unavailable`,
services/vc-ts/README.md:106:  `Authorization: Bearer <key>` (verified against a stub API: no
services/vc-ts/README.md:112:  every fallback fails does search return `engine_unavailable`, and
services/vc-ts/README.md:116:  live path is verified via the stub + the honest error path.
services/vc-ts/README.md:129:## What is stubbed / degraded, and why
services/vc-ts/README.md:135:  EPERM even to loopback). The transport itself is real, not a stub:
services/vc-ts/README.md:145:  placeholder track** (`yt:<id>` / `query:<hash>`, `duration_sec: 0`,
services/vc-ts/README.md:147:  the placeholder says so in `resolve_note`. Degraded tracks never
services/vc-ts/README.md:151:  Nothing is faked; the stub tests + honest error path are the
services/vc-ts/README.md:165:  + stub-Lemonic keyed/unauthenticated + panel/loop/radio regressions
services/vc-rust/src/main.rs:49:/// Placeholder length for tracks whose real duration is unknown
services/vc-rust/src/main.rs:69:    /// state-wise (old stub behaviour) instead of stalling forever.
services/vc-rust/src/main.rs:564:/// placeholder duration, and no fake stream URL — the track still
services/vc-rust/src/voice.rs:855:    /// Short voice string for now-playing (never "stub-*" and never
services/vc-rust/src/voice.rs:1160:            Some("dummyhash".to_string()),
services/vc-rust/README.md:31:  deterministic `pattern-<fnv>` id, placeholder 180 s duration, no fake
services/vc-rust/README.md:87:## Stubbed (out of scope in the sandbox)
services/vc-rust/PROGRESS.md:8:- `src/main.rs` — rewritten from stub: full vc.* IPC state machine
services/vc-rust/PROGRESS.md:12:  resolve, `VoiceCall` trait + `LoggingVoiceCall` stub.
services/vc-rust/PROGRESS.md:17:- `README.md` — updated: implemented vs stubbed + creds (none needed).
services/vc-rust/PROGRESS.md:40:#   "duration_sec":180,"resolved_by":"pattern"},"queue":[],"voice":"stub-logging-only"},"queued":false,"resolved_by":"pattern"}}
services/vc-rust/PROGRESS.md:73:# Voice stub log lines observed: join / play / pause / resume / stop / leave per action.
services/vc-rust/PROGRESS.md:94:## Real vs stubbed
services/vc-rust/PROGRESS.md:97:- STUBBED: voice join/stream (LoggingVoiceCall, see README); no
services/vc-rust/PROGRESS.md:116:  with a dummy empty session file the bridge fails `ValueError`
services/vc-rust/PROGRESS.md:122:- STUBBED line above is superseded: voice join/stream is now the real
services/vc-rust/PROGRESS.md:123:  transport with a blocked live join, not a logging stub.
services/audio-fx-rust/src/main.rs:131:                    "engine_unavailable",
services/audio-fx-rust/README.md:37:## Stubbed / not done
services/audio-fx-rust/README.md:39:- Nothing stubbed in the endpoint itself. Not implemented: no
services/audio-fx-rust/PROGRESS.md:7:- `src/main.rs` — rewritten from stub: POST /v1/fx (real ffmpeg
services/audio-fx-rust/PROGRESS.md:14:- `README.md` — updated: implemented vs stubbed.
services/audio-fx-rust/PROGRESS.md:52:## Real vs stubbed
services/audio-fx-rust/PROGRESS.md:55:- STUBBED: none in the endpoint. Known gaps (see README): no ffmpeg
services/share-rust/README.md:37:## Stubbed / not done
services/share-rust/README.md:39:- Nothing stubbed in the endpoints. Not implemented: no token
services/share-rust/PROGRESS.md:7:- `src/main.rs` — rewritten from stub: POST /v1/share (16-char
services/share-rust/PROGRESS.md:13:- `README.md` — updated: implemented vs stubbed.
services/share-rust/PROGRESS.md:45:## Real vs stubbed
services/share-rust/PROGRESS.md:48:- STUBBED: none in the endpoints. Known gaps (see README): no token
services/queue-rust/README.md:39:## Stubbed / not done
services/queue-rust/README.md:41:- Nothing stubbed in the endpoint. Not implemented: no queue
services/queue-rust/PROGRESS.md:7:- `src/main.rs` — rewritten from stub: POST /v1/queue/vote
services/queue-rust/PROGRESS.md:13:- `README.md` — updated: implemented vs stubbed.
services/queue-rust/PROGRESS.md:47:## Real vs stubbed
services/queue-rust/PROGRESS.md:51:- STUBBED: none in the endpoint. Known gaps (see README): no queue
services/spotify-rust/src/main.rs:849:/// without them the honest answer is 503 `engine_unavailable`
services/spotify-rust/src/main.rs:882:                "engine_unavailable",
services/spotify-rust/README.md:51:  answers 503 `engine_unavailable` and points at the downloaders
services/spotify-rust/README.md:63:## NOT implemented — needs credentials / a classifier
services/spotify-rust/PROGRESS.md:8:- `src/main.rs` — rewritten from stub: watch registry, dislike store,
services/spotify-rust/PROGRESS.md:109:## Real vs stubbed
services/gui-js/app/main.js:21://   GET  /api/search?q=             proxy CORE_URL, else NETEASE_URL, else engine_unavailable
services/gui-js/app/main.js:29://                                   without CORS headers). Range passthrough.
services/gui-js/app/main.js:30://   POST /api/play  {chat_id,query} proxy VC engine, else engine_unavailable
services/gui-js/app/main.js:31://   POST /api/pause {chat_id}       proxy VC engine, else engine_unavailable
services/gui-js/app/main.js:32://   POST /api/skip  {chat_id}       proxy VC engine, else engine_unavailable
services/gui-js/app/main.js:179:    <input id="q" type="text" placeholder="Song, artist, or link&hellip;" autocomplete="off">
services/gui-js/app/main.js:413:        (env.error && env.error.code === "engine_unavailable" ? " (no VC engine connected)" : ""), true);
services/gui-js/app/main.js:466:      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
services/gui-js/app/main.js:482:      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
services/gui-js/app/main.js:583:      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
services/gui-js/app/main.js:590:      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "netease error", up.status || 502);
services/gui-js/app/main.js:593:  return fail(res, "engine_unavailable", "no search backend configured (set CORE_URL or NETEASE_URL)", 502);
services/gui-js/app/main.js:612:      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "engine error", up.status || 502);
services/gui-js/README.md:31:| `GET /api/search?q=` | Proxies `CORE_URL` (`POST /v1/search`), else `NETEASE_URL` (`POST /v1/netease/search`), else `engine_unavailable` |
services/gui-js/README.md:33:| `GET /api/audio?src=<url>` | **(Phase 4)** Same-origin proxy for that audio URL (Range passthrough, 206). This is what makes WebAudio gain work: media routed through a `MediaElementAudioSourceNode` is silenced when cross-origin without CORS headers, so the player loads the proxy instead of the CDN directly. Bad `src` → 400; dead upstream → 502 `download_failed`. |
services/gui-js/README.md:36:## In-browser playback (Phase 4 — the stub this phase killed)
services/gui-js/README.md:73:## Still stubbed / not implemented
services/gui-js/README.md:84:  (full + Range) against a local stub MP3, and `/api/stream-url` is
services/gui-js/PROGRESS.md:7:- `services/gui-js/app/main.js` — rewritten from the 1-line stub into the
services/gui-js/PROGRESS.md:9:- `services/gui-js/README.md` — rewritten: implemented vs stubbed.
services/gui-js/PROGRESS.md:21:| `GET /api/search?q=` | Proxy `CORE_URL`, else `NETEASE_URL`, else `engine_unavailable` |
services/gui-js/PROGRESS.md:58:   `engine_unavailable: "no search backend configured (set CORE_URL or
services/gui-js/PROGRESS.md:80:## Still stubbed
services/gui-js/PROGRESS.md:101:  proxy (Range passthrough) because cross-origin media through a
services/gui-js/PROGRESS.md:105:  Range 206 against a real ffmpeg MP3 stub; honest errors).
services/gui-js/PROGRESS.md:107:  through the egress proxy, so Saavn bytes are verified via the stub
services/gui-js/tests/run.js:7://     passthrough, byte-exact) so the GainNode actually changes gain
services/gui-js/tests/run.js:11:// proxy's byte path is tested against a local stub audio server
services/gui-js/tests/run.js:17:// Ports: 18122 vc-ts, 18130 gui-js, 18131 stub audio server.
services/gui-js/tests/run.js:76:// Stub audio server: serves a real MP3 file with Range support, the
services/gui-js/tests/run.js:78:let stubAudio = null;
services/gui-js/tests/run.js:79:function startStubAudio(port) {
services/gui-js/tests/run.js:82:    stubAudio = http.createServer((req, res) => {
services/gui-js/tests/run.js:100:    stubAudio.listen(port, () => resolve());
services/gui-js/tests/run.js:110:  await startStubAudio(18131);
services/gui-js/tests/run.js:119:  if (stubAudio) stubAudio.close();
services/gui-js/tests/run.js:167:test("audio proxy streams real bytes, Range passthrough, byte-exact", async () => {
services/radio-js/README.md:88:## What is stubbed / not done, and why
services/radio-js/README.md:94:  stubbed in the sandbox — see vc-ts README), so there is nothing to
services/radio-js/PROGRESS.md:16:- `src/main.js` — replaced stub. HTTP server, ffprobe stream probing
services/radio-js/PROGRESS.md:24:- `README.md` — rewritten: real vs stubbed, endpoints, stations.
services/radio-js/PROGRESS.md:67:## Still stubbed / untested
services/radio-js/PROGRESS.md:70:  engine's voice path is itself stubbed; nothing to pipe into yet).
services/downloaders/src/util.py:37:def engine_unavailable(msg):
services/downloaders/src/util.py:38:    return ApiError("engine_unavailable", msg, 503)
services/downloaders/src/ytdlp.py:17:from .util import (ApiError, download_failed, engine_unavailable,
services/downloaders/src/ytdlp.py:30:        raise engine_unavailable(
services/downloaders/src/ytdlp.py:48:        raise engine_unavailable("cannot run yt-dlp: %s" % exc)
services/downloaders/src/navidrome.py:12:``engine_unavailable`` naming the missing var.
services/downloaders/src/navidrome.py:32:from .util import (download_failed, engine_unavailable, sanitize_filename,
services/downloaders/src/navidrome.py:39:    """(base_url, user, password) or raise engine_unavailable."""
services/downloaders/src/navidrome.py:47:        raise engine_unavailable(
services/downloaders/README.md:81:  vars answer `engine_unavailable` naming them. Live-tested
services/downloaders/README.md:85:## Implemented vs stubbed
services/downloaders/README.md:119:Stubbed: nothing. All five providers are real (see the Phase 4
services/downloaders/PROGRESS.md:5:Real Python service in `services/downloaders/` (was a 3-line stub):
services/downloaders/PROGRESS.md:16:  (youtube, spotify real; deezer/tiktok/navidrome `engine_unavailable`).
services/downloaders/PROGRESS.md:19:- `README.md` — implemented vs stubbed.
services/downloaders/PROGRESS.md:65:7. `POST /v1/download/deezer` → `engine_unavailable` (stub, as designed) ✓
services/downloaders/PROGRESS.md:72:## Real vs stubbed
services/downloaders/PROGRESS.md:79:- STUBBED (route exists, honest `engine_unavailable`): deezer, tiktok,
services/downloaders/PROGRESS.md:116:  `engine_unavailable` naming them.
services/downloaders/tests/test_providers.py:141:    def test_missing_env_is_engine_unavailable(self):
services/downloaders/tests/test_providers.py:147:            self.assertEqual(ctx.exception.code, "engine_unavailable")
services/netease-go/README.md:36:IP**, so `/v1/netease/download` honestly returns `engine_unavailable`:
services/netease-go/README.md:46:  `engine_unavailable: "netease song url: track <id> has no playable URL
services/netease-go/README.md:60:## Still stubbed / not implemented
services/netease-go/internal/server/server.go:141:		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
services/netease-go/internal/server/server.go:175:		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
services/netease-go/internal/server/server.go:181:		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
services/netease-go/internal/server/server.go:270:		writeErr(w, http.StatusBadGateway, "engine_unavailable", err.Error())
services/netease-go/PROGRESS.md:8:  print-only stub into the real server entry (PORT/PARLAY_TOKEN/
services/netease-go/PROGRESS.md:15:- `services/netease-go/README.md` — rewritten: implemented vs stubbed,
services/netease-go/PROGRESS.md:65:# → {"ok":false,"error":{"code":"engine_unavailable",
services/netease-go/PROGRESS.md:101:## Still stubbed

## 3. The 59 non-test, non-doc code hits — verdict per hit

### 3a. `engine_unavailable` as the honest contract error code — 34 hits, NOT stubs

Legitimate: the code path that returns it is real; it names the missing piece. Hits:

- `services/core-cpp/src/ipc.{hpp,cpp}` (4): error-code enum + classification for fail-over. Real.
- `services/core-cpp/src/engine_router.{hpp,cpp}` (2): fail-over trigger. Real.
- `services/vc-ts/src/main.js:396,611,654` (3): Lemonic/search fallback exhaustion → 502. Real.
- `services/vc-ts/src/resolvers.js:12,61,65,292` (4): `EngineUnavailableError` type + comment. Real.
- `services/audio-fx-rust/src/main.rs:131` (1): ffmpeg missing → 503. Real.
- `services/spotify-rust/src/main.rs:849,882` (2): no-creds collection → 503 pointing at downloaders fallback. Real.
- `services/gui-js/app/main.js:21,30,31,32,413,466,482,583,590,593,612` (11): proxy fallback when no backend configured. Real (gui-js has no backend of its own by design).
- `services/downloaders/src/util.py:37,38; ytdlp.py:17,30,48; navidrome.py:12,32,39,47` (9): helper + yt-dlp-missing + Navidrome-missing-env paths. Real.
- `services/netease-go/internal/server/server.go:141,175,181,270` (4): search/detail/URL/lyrics upstream failure → 502. Real (these are error paths of a real handler, not stubs — see §7 netease note: the handler itself downloads live when a URL resolves).

### 3b. Real degraded-placeholder behaviour — 6 hits, PARTIAL (blocks full fidelity, not basic use)

- `services/vc-ts/src/resolvers.js:125,136,149` — yt-dlp failure → degraded placeholder track (`yt:<id>` / `query:<hash>`, `duration_sec: 0`, `degraded: true`, `resolve_note` says so). Play/queue keep working, but the track never auto-advances and has no real metadata. Blocks real use only when yt-dlp/YouTube is unreachable from the host.
- `services/vc-rust/src/main.rs:49,564` + `src/main.rs:69` — placeholder 180 s duration for unknown-duration tracks; ticker advances state-wise instead of stalling. Same class: state machine stays correct, timing is fake for those tracks. Non-blocking for queue/position correctness; blocks accurate now-playing duration.
- `services/vc-ts/src/voicecall.js:46` — comment only ("no silent stub"); the file is the real Opus/RTP + join client. Not a stub.
- `services/vc-rust/src/voice.rs:855` — comment only ("never stub-*"). Not a stub.

### 3c. Genuine unimplemented pieces named in code — 5 hits, 2 BLOCKING for real use

- `services/core-cpp/src/store.cpp:709` — "mongodb URIs with credentials are not supported by the minimal [client]" + `services/core-cpp/README.md:113,206` — **Mongo SCRAM auth not implemented; authenticated URIs rejected.** BLOCKS: any deployment whose MongoDB requires auth (incl. compose if auth is ever enabled; currently compose runs mongod without auth, so it works there). Non-blocking in the current sandbox/compose setup.
- `services/core-cpp/src/tdlib_client.cpp:298` — "(registration / QR / email flows are not implemented; …)" — TDLib first-time auth needs phone/code/password via env; no interactive registration. BLOCKS only first-time TDLib auth without a pre-authorized session dir. Non-blocking if a session dir is supplied.
- `services/core-cpp/src/voice_bridge.py:143` — `except NotImplementedError: pass` — signal-handler install on non-POSIX; benign, Windows-only path. Not blocking.
- `services/core-cpp/src/http.cpp:44` — mkstemp `XXXXXX` template. False positive. Not blocking.
- `services/vc-rust/src/voice.rs:1160` — `Some("dummyhash".to_string())` — **test-only**: inside `#[test] join_with_empty_session_reports_bridge_value_error`, a dummy API hash passed to the config gate. Not production code. Not blocking.
- `services/gui-js/app/main.js:179` — HTML `placeholder="Song, artist, or link…"` attribute. Not a stub.
- `services/gui-js/app/main.js:29` — "Range passthrough" comment on the real `/api/audio` proxy. Not a stub.
- `services/core-cpp/src/voice_call.hpp:3,77,262` + `src/dispatcher.hpp:11` — comments asserting the logging stub is gone / no handler replies with a stub. Verified true in code (see §5); the comments themselves are accurate.

### 3d. What the grep did NOT find (verified absences)

- No `TODO`, `FIXME`, `HACK`, `mock`, `unimplemented`, `pass-through` anywhere in `services/` (excluding build/target/refs).
- No `LoggingVoiceCall` in any non-doc code: it is deleted from vc-rust (`src/main.rs` uses `RealVoiceCall`) and never existed in core-cpp Phase 4 code (only `UnavailableVoiceCall`). All remaining `LoggingVoiceCall` mentions are stale PROGRESS.md lines (§7).
- No `TODO:`-style deferred work hidden in comments.

## 4. Test-only hits (51 lines) — all legitimate test doubles, no production impact

- `services/vc-ts/tests/run.js` (33): a local stub Lemonic API server (`startStubLemonic`, `stubHits`, stub tracks "Stub Song One/Two") used to verify keyed vs unauthenticated requests; plus assertions that `voice_joined` is never true without a real join. Legitimate.
- `services/gui-js/tests/run.js` (11): local stub audio server serving a real MP3 with Range support to verify the `/api/audio` proxy byte-exact. Legitimate.
- `services/downloaders/tests/test_providers.py` (2): `test_missing_env_is_engine_unavailable`. Legitimate.
- 5 further lines are README/PROGRESS sentences containing the word "test" (counted by the test-path filter, actually docs).

## 5. Voice-join stub state per service (second grep: LoggingVoiceCall | UnavailableVoiceCall | rtp_ok | opus-file-only | voice_joined — 21 hits)

| Service | State (code, not docs) |
|---|---|
| core-cpp | `VoiceCall` interface with 3 backends: `VoiceBridge` (Telethon+ntgcalls via `voice_bridge.py`) → `NtgCallsVoiceCall` (dlopen `libntgcalls.so`) → `UnavailableVoiceCall` (exact reason, never fakes success). `TdLibVoiceCall` added in TDLib wiring (authorize → offer → joinVideoChat → ntg.connect). `voice_call.cpp:488-489` sets `st.rtp_ok = (rtp_rc == 0)`; sandbox reports `rtp_ok=0` honestly (UDP sendto EPERM). Opus pipeline proven to file: 251 frames / 83,736 B / 5.0065 s. **No live join possible in sandbox** (no session, no direct TCP to Telegram DCs, UDP EPERM). `libntgcalls.so` absent, so the dlopen backend is unavailable here. |
| vc-ts | `src/voicecall.js` rewritten (was a stub): real ffmpeg Opus/RTP pipeline + ntgcalls local offer + voice-bridge join client. `src/main.js:63` now-playing reports `voice_joined: vs.joined` (false unless a real join happened); `voicecall.js:399` sets `opus-file-only:<reason>` when RTP cannot be delivered. Join blocked at the config gate (no `ASSISTANT_SESSION_FILE`/`API_ID`/`API_HASH`) + UDP EPERM. |
| vc-rust | `src/voice.rs` `RealVoiceCall` (std-only): same Opus/RTP pipeline, `opus-file-only:<reason>` at `:957`, local offer, join client driving `voice_bridge.py`. `LoggingVoiceCall` deleted from `src/main.rs`. Same sandbox blockers as vc-ts. |
| vc-go | By design a queue/playback-state engine only: no voice code at all, no voice grep hits. Voice joining is the core's job via `VoiceCall`. **Caveat (see §7):** `Track.StreamURL` is `json:"-"` (`internal/types/types.go:20`) — the resolved direct audio URL is never serialized over IPC, so the core cannot actually fetch it from vc-go's now-playing despite the README saying it can. |
| radio-js / gui-js | No voice code; radio hands the stream URL to a VC engine (`vc_handoff`); gui plays in-browser via `/api/stream-url` + `/api/audio` proxy. Both depend on the VC voice path above for actual voice-chat audio. |

Net: the Opus/RTP pipeline is proven in all three places (identical 251-frame / 251-RTP-packet numbers); the **live join has never happened anywhere** and cannot happen from this sandbox. Every service reports this honestly (`voice_joined:false`, `opus-file-only:<reason>`, `unavailable:<reason>`).

## 6. Docs read — claims vs reality

- PHASE2.md / PHASE3.md: accurate for their phase; their "still stubbed" sections are now partly stale (superseded by Phase 4 + TDLib wiring) — e.g. PHASE3 "vc-rust LoggingVoiceCall only" and "Netease download engine_unavailable" no longer describe the code.
- PHASE4.md: accurate in substance; its stub-grep claim ("remaining hits are (a) engine_unavailable, (b) voice-join stubs, (c) test-only stub servers") is confirmed by this audit, with the addition of the degraded-placeholder tracks (§3b) and the SCRAM/registration gaps (§3c).
- PHASE4-TDLIB.md: accurate except §5.5 "NetEase download still engine_unavailable (weapi encryption, Phase 3)" — **wrong**: the download handler + mirror fallback exist in code (`internal/server/server.go` handleDownload, `internal/netease/client.go` SongURLWithLevel/mirrorURL) and PHASE4.md's live FLAC downloads (34.9 MB etc.) are the current state. The TDLib doc also says vc-go tests "6/6 from Phase 2" — stale; the suite is 18 tests (14 queue + 2 resolver + 2 store).
- DEEP_READ.md: synthesis, no verifiable falsehoods found in the sampled claims (assistant-holds-VC-session headline matches all four ref families as cited; Lemonic "no API key upstream" matches the vc-ts rework; JioSaavn duration root cause matches the fix).
- COMMANDS.md: header claim "Every command dispatches to a real IPC call or a real Store action — no stub replies" holds for the dispatcher (no "not implemented" hits in `dispatcher.cpp`); its own "Not yet" section honestly lists the flag-only cases (`/mute` no `vc.mute` action, `/speed`/`/bass` seek-restart + stored value, replied-media `file_id` hand-off missing, broadcast no errors.txt). Spot-checked: `/eval` (dispatcher.cpp:267, owner, real `sh -c`) and `/restart` (dispatcher.cpp:273,3038-3044, exit 42) exist as claimed.
- BENCHMARKS.md: numbers are internally consistent with PHASE4.md (core-cpp 35.1 s / 866,656 B stripped; vc-go 92.4 s clean, 9.3 MB; vc-rust 17.7 s /play download; NetEase FLAC sizes). Not re-measured by this worker (audit scope is grep+docs).
- ARCHITECTURE.md: **stale scaffold-era description, several claims false against current code** — see §7.
- Root README.md: **stale** — see §7.

## 7. README spot-checks (2–3 claims each) — dishonesty / drift found

### Root README.md — STALE, contradicts Phases 2–4

- "Status: **Scaffold only.** The service directories contain original stubs … no upstream code was copied in." **False today**: 12 services are implemented and live-tested (PHASE2–4 + TDLib wiring). The "Quick start (scaffold)" + "docker compose up --build # builds the stubs" is likewise stale.
- Architecture table ("vc-go: VC streaming engine", "vc-ts: TS VC engine + Lemonic + inline", "gui-js: Next.js Mini App", "vc-rust: MTProto + ntgcalls", "downloaders: … Soulseek", "MongoDB + Redis shared state") repeats ARCHITECTURE.md's aspirational stack, not the built one (see ARCHITECTURE below). License section ("TBD — do not pick one… unlicensed scaffold") is also stale relative to the built tree.

### ARCHITECTURE.md — STALE in the same way

- "vc-go (Go, ntgcalls)", "vc-ts (TS, grammy)", "vc-rust (Rust, ntgcalls)", "gui-js (Next.js Mini App)": **no grammy, no tgcalls, no Next.js, no ntgcalls join exists in `services/` code** (grep: zero hits for grammy/tgcalls/Next.js outside README/PROGRESS prose). All three VC engines are stdlib-only with a proven Opus pipeline and a blocked live join; gui-js is plain Node stdlib, no Next.js build (its own README says so explicitly).
- "MongoDB + Redis (shared state: queues, auth, shares)" / "Services never share a database table directly": overstates — shares, votes, watches/dislikes are JSON files in each service; only core-cpp Store (hand-rolled Mongo wire + Redis RESP, real, tested vs mongod 8.0.4/redis) and vc-go (real Mongo driver) use Mongo/Redis. `docker-compose.yml` also wires `mongo:7`/`redis:7-alpine`, which matches core+vc-go only.
- "The C++ core never downloads, never streams" — now false in the narrow voice sense: the core owns the Opus pipeline + voice bridge (its own README says so); routing-only applies to playback state.
- "gui-js talks only to the core (/api/*)" — true for now-playing/queue/search, but `/api/play|pause|skip` proxy VC engines directly and `/api/stream-url` queries VC engines first (gui-js README documents this; ARCHITECTURE does not).

### services/core-cpp/README.md — mostly honest, two stale sections

- "Implemented (real)" claims spot-checked TRUE: dispatcher 3,199 lines with full inventory; Store hand-rolled Mongo/Redis (tested vs real servers); engine fail-over; voice backends incl. `UnavailableVoiceCall`. `src/tdlib_client.{hpp,cpp}` exist (verified on disk).
- STALE: "Tests" section — "TDLib: clone + configure pass … full build running, log phase4/tdlib-build.log" — the build **finished** 100% (PHASE4-TDLIB.md, 0 errors, libtdjson.so.1.8.69 50 MB). STALE: "Not yet implemented — **TDLib C++ client** — `src/tdlib_client` does not exist yet" — **false, it exists** (151-line header + ~330-line cpp + `tests/tdlib_test.cpp` 7/7). The "Core HTTP port / GUI /api/* — not served yet" claim is still TRUE (verified: `config.hpp:40` `port = 8080 // core HTTP port (GUI /api surface, future)`; no listen/serve in `main.cpp`).
- Honest-limits section verified accurate (mute flag-only, speed/bass stored-not-applied, file_id hand-off missing, SCRAM rejected, voice join needs session+network).

### services/core-cpp/PROGRESS.md — STALE (Phase 2 snapshot, never updated)

- L25, L38, L54: "voice isolated behind VoiceCall interface with a logging stub", "`LoggingVoiceCall` stub", "Voice-call binding — logging stub only". **False since Phase 4/TDLib wiring** (real pipeline + bridge + TdLib backends). L55-63 "MongoDB/Redis — env vars read, not connected", "MTProto unused", "Inline queries, callbacks, playlists, loop/seek, autoplay, broadcast, sudo lists — not in this command surface yet", "No live Telegram or IPC round-trip was possible … vc-*/downloader services are stubs" — all superseded by the Phase 4 dispatcher + Store. PROGRESS.md is a historical log, but nothing in it marks it as Phase-2-only, so it reads as current state.

### services/vc-go/README.md — honest except one real mismatch

- Loop/shuffle/autoplay/Mongo claims spot-checked TRUE in code (resolver `Related`, runner prefetch, store Mongo driver; 18 test funcs = 14 queue + 2 resolver + 2 store, matches "18 tests").
- MISMATCH: "The core asks this service … which stream URL to feed (`Track.stream_url` once yt-dlp resolves it)". **False**: `internal/types/types.go:18-20` — `StreamURL string \`json:"-"\`` with comment "Internal to vc-go; not part of the contract". No `stream_url` appears in `server.go` output. The core cannot obtain vc-go's resolved stream URL over IPC today.
- "yt-dlp is absent in this sandbox" is stale as a blanket claim (yt-dlp exists at `services/downloaders/.venv/bin/yt-dlp` and vc-ts/vc-rust resolve live through it); vc-go's resolver just doesn't point at it by default.

### services/vc-ts/README.md — honest

- Spot-checked TRUE: JioSaavn duration fix (`parseDuration` + `/song` enrichment), panel endpoints, Lemonic unauthenticated-first + Bearer <redacted>, voice transport real-but-blocked, degraded placeholder documented with `resolve_note`. Test count 24 matches `tests/run.js` (24 `test(` calls). No dishonesty found.

### services/vc-rust/README.md — internally contradictory (stale tail)

- Implemented/voice sections TRUE (loop/shuffle/autoplay live-tested, download path, `RealVoiceCall`).
- STALE tail "Notes / known gaps": "No loop/shuffle/autoplay yet (ParvatiMusic has them via room flags)" — **directly contradicted by the same file's Implemented section** (loop/shuffle/autoplay with live-test evidence). "`yt-dlp` is **not** installed in the sandbox, so live resolve is pattern-only there" — **false**: yt-dlp is used live (Rick Astley mp3 downloaded in the same README). This section is a Phase 3 leftover never updated.

### services/vc-rust/PROGRESS.md — internally contradictory

- L94-99 "Real vs stubbed" block says "STUBBED: voice join/stream (LoggingVoiceCall…); no download path; no loop/shuffle/autoplay" — superseded by its own later section (L102+) which says "STUBBED line above is superseded: voice join/stream is now the real transport…". The supersession note exists but the stale block is left standing above it; a reader stopping at L99 gets the wrong state. L12 also still lists "`LoggingVoiceCall` stub" in the file inventory.

### services/audio-fx-rust, share-rust, queue-rust, spotify-rust READMEs — honest

- Spot-checked: audio-fx filter chains match DEEP_READ §6 exactly; share token/link shape + anonymity (created_by not stored) matches code; queue 3-vote threshold + re-vote-replaces matches code; spotify 54-word list, LRCLib-only lyrics, `reason:"ai-generated"` never emitted, watch-registry-intent-only all match code. "Nothing stubbed in the endpoint" is accurate for all four.
- **Port drift (all four + vc-rust):** audio-fx README/code default PORT **8003**, share **8004**, queue **8005**, spotify **8007**, vc-rust **8003** — but `docker-compose.yml` maps audio-fx 8004, share 8005, queue 8006 and sets **no PORT env anywhere**, so under compose audio-fx would listen on 8003 (colliding with vc-rust's 8003) and be unreachable at its mapped 8004; share/queue likewise shifted by one. vc-rust and audio-fx both default to 8003. This is a compose/code mismatch, not a README lie (READMEs match code; compose matches the core README's port table instead).

### services/gui-js/README.md — honest

- Spot-checked TRUE: GainNode wiring (`app/main.js:300-317` `createMediaElementSource` → `GainNode`, `gain.value = v/100`, localStorage-persisted, `audio.volume` fallback); volume slider never overwritten by engine volume (`main.js:231-233` comment + code); `/api/audio` Range passthrough; Saavn 404 sandbox limit disclosed. Test count 7 matches file.

### services/radio-js/README.md — honest

- Spot-checked TRUE: 6 curated stations, ffprobe probing, VC handoff (`vc_handoff` on start, room cleared on stop only if still on the radio track), "no audio relayed by this service itself" disclosed. Test count 7 matches file.

### services/downloaders/README.md — honest (with a scope note)

- Spot-checked TRUE: all five providers have real handler modules (`providers.py` registers youtube/spotify/deezer/tiktok/navidrome; `deezer.py`/`tiktok.py`/`navidrome.py` implement search+download). "Stubbed: nothing. All five providers are real" is accurate **for preview-grade Deezer** (30 s previews; full tracks need an ARL cookie, disclosed in the same section) and Navidrome-via-Subsonic (Soulseek-via-slskd explicitly a separate opt-in job; zero soulseek/slskd hits in code — consistent, not a hidden stub).
- Spotify playlist embed fallback + 50-track cap + song-length guard match code.

### services/netease-go/README.md — internally contradictory (stale "Known live limitation")

- Route table at the top says download resolves + downloads to `PARLAY_DL_DIR` — TRUE in code (`handleDownload`, `SongURLWithLevel`, mirror fallback `client.go:315-319`).
- But the "Known live limitation" section says download "honestly returns `engine_unavailable`", "No weapi-encrypted requests", "No end-to-end download test with a real audio file — impossible while the URL endpoint returns `url:null`". **All three are false since Phase 4**: weapi/eapi/linuxapi crypto is ported (`client.go` AES-ECB key `e82ckenh8dichen8` etc.), the mirror fallback delivers, and PHASE4.md records live FLAC downloads (34.9 MB, 27.4 MB, 22.2 MB). The `engine_unavailable` in `server.go` is now only the error path when every URL source fails, not the normal outcome. This stale section is the direct source of PHASE4-TDLIB.md §5.5 repeating the same wrong claim.

## 8. Blocking items (condensed — full detail above)

| # | Item | File:line | Blocks real use? |
|---|---|---|---|
| 1 | No live voice join anywhere; audio never reaches a voice chat (sandbox: no assistant session, no direct TCP to Telegram DCs, UDP sendto EPERM even loopback) | vc-ts `src/voicecall.js:399` (`opus-file-only`); vc-rust `src/voice.rs:957`; core-cpp `src/voice_call.cpp:488-489` (`rtp_ok=0`) | **YES — the one project-level blocker.** Needs an open-network host + assistant session + `libntgcalls.so`. |
| 2 | Mongo SCRAM auth not implemented; authenticated URIs rejected | `services/core-cpp/src/store.cpp:709` | YES if Mongo requires auth; NO under current compose (no-auth mongod). |
| 3 | vc-go `StreamURL` never serialized (`json:"-"`) — core cannot fetch vc-go's resolved stream URL over IPC | `services/vc-go/internal/types/types.go:20` | YES for any core/gui flow that needs vc-go's direct audio URL (gui `/api/stream-url` falls back to vc-ts/core instead). |
| 4 | Compose port mismatch: audio-fx/share/queue code defaults 8003/8004/8005 vs compose mappings 8004/8005/8006, no PORT env set; audio-fx and vc-rust both default 8003 | `docker-compose.yml` (audio-fx/share/queue services); `services/audio-fx-rust/src/main.rs:160-163`, `services/share-rust/src/main.rs:58-61`, `services/queue-rust/src/main.rs:120-123` | YES under `docker compose up` (services unreachable/colliding at mapped ports). |
| 5 | Core serves no HTTP on :8080 though compose maps it and gui `CORE_URL=http://core-cpp:8080` | `services/core-cpp/src/config.hpp:40` (`port = 8080 // future`); `docker-compose.yml` core + gui-js services | YES for gui→core proxying under compose (every CORE_URL call fails; gui falls back to demo/NETEASE paths). |
| 6 | TDLib first-time auth flows (registration/QR/email) not implemented | `services/core-cpp/src/tdlib_client.cpp:298` | Only for first-time TDLib auth without a pre-authorized session dir. |
| 7 | Degraded placeholder tracks (vc-ts) / placeholder 180 s duration (vc-rust) when yt-dlp/YouTube unreachable | `services/vc-ts/src/resolvers.js:125,136,149`; `services/vc-rust/src/main.rs:49,564` | PARTIAL: queue/position keep working; durations + auto-advance wrong for those tracks. |
| 8 | Docs drift (root README "Scaffold only", ARCHITECTURE aspirational stack, core-cpp README "tdlib_client does not exist yet", core-cpp PROGRESS logging-stub, vc-rust README/PROGRESS "no loop/shuffle/autoplay", netease README + PHASE4-TDLIB §5.5 "download engine_unavailable") | See §7 | NO for runtime; YES for anyone trusting the docs to decide what works. |


## 9. Test-file hit list (for the record)

services/core-cpp/PROGRESS.md:63:No real `BOT_TOKEN` exists in this task and the vc-*/downloader services are stubs, so no live Telegram or IPC round-trip was possible. The code paths are exercised only by the JSON harness + the missing-env smoke test. Live verification needs: a bot token, the vc-go service up, `/play` in a group, kill vc-go mid-play to prove fail-over to vc-ts.
services/vc-ts/PROGRESS.md:108:## Still stubbed / untested
services/vc-ts/tests/run.js:2:// a child-process server + a stub Lemonic API on localhost.
services/vc-ts/tests/run.js:6:// the Lemonic tests hit a local stub so keyed vs unauthenticated
services/vc-ts/tests/run.js:12:// 18109 stub Lemonic API. Data dirs under /tmp/p4-vcts/.
services/vc-ts/tests/run.js:82:// ------------------------------------------------------- stub Lemonic API --
services/vc-ts/tests/run.js:83:const stubHits = []; // { path, authHeader }
services/vc-ts/tests/run.js:84:let stubServer = null;
services/vc-ts/tests/run.js:86:function startStubLemonic(port) {
services/vc-ts/tests/run.js:88:    stubServer = http.createServer((req, res) => {
services/vc-ts/tests/run.js:89:      stubHits.push({ path: req.url, auth: req.headers.authorization || null });
services/vc-ts/tests/run.js:97:            { id: "stub1", title: "Stub Song One", artist: "Stub Artist", duration: 200, url: "https://example.invalid/stub1.mp3" },
services/vc-ts/tests/run.js:98:            { id: "stub2", title: "Stub Song Two", artist: "Stub Artist", duration_sec: 150, url: "https://example.invalid/stub2.mp3" },
services/vc-ts/tests/run.js:107:            title: "Stub Raw Track",
services/vc-ts/tests/run.js:108:            artist: "Stub Artist",
services/vc-ts/tests/run.js:118:    stubServer.listen(port, () => resolve());
services/vc-ts/tests/run.js:129:  await startStubLemonic(18109);
services/vc-ts/tests/run.js:144:  if (stubServer) stubServer.close();
services/vc-ts/tests/run.js:193:    const j = await voicecall.join(VOICE_CHAT, "dummy-offer");
services/vc-ts/tests/run.js:226:  assert.ok(v.voice.length > 0, "voice state is always a real string, never a stub label");
services/vc-ts/tests/run.js:420:test("lemonic search unauthenticated (stub): results, no Authorization header", async () => {
services/vc-ts/tests/run.js:421:  stubHits.length = 0;
services/vc-ts/tests/run.js:422:  const r = await req(18102, "POST", "/v1/lemonic/search", { query: "stub" });
services/vc-ts/tests/run.js:426:  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
services/vc-ts/tests/run.js:427:  assert.ok(hit, "stub saw /api/search");
services/vc-ts/tests/run.js:431:test("lemonic search keyed (stub): Bearer header sent, same results", async () => {
services/vc-ts/tests/run.js:432:  stubHits.length = 0;
services/vc-ts/tests/run.js:433:  const r = await req(18103, "POST", "/v1/lemonic/search", { query: "stub" });
services/vc-ts/tests/run.js:436:  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
services/vc-ts/tests/run.js:441:test("lemonic raw track via stub (unauthenticated) + download route", async () => {
services/vc-ts/tests/run.js:442:  const r = await req(18102, "POST", "/v1/lemonic/download", { track_id: "stub1" });
services/vc-ts/tests/run.js:443:  // The stub's stream URL is example.invalid, so the download fetch
services/vc-ts/tests/run.js:445:  // read fine), not engine_unavailable.
services/vc-ts/tests/run.js:450:test("lemonic search with dead API + no Spotify creds: honest engine_unavailable naming both fallbacks", async () => {
services/vc-ts/tests/run.js:453:  assert.equal(r.json.error.code, "engine_unavailable");
services/vc-ts/README.md:151:  Nothing is faked; the stub tests + honest error path are the
services/gui-js/tests/run.js:7://     passthrough, byte-exact) so the GainNode actually changes gain
services/gui-js/tests/run.js:11:// proxy's byte path is tested against a local stub audio server
services/gui-js/tests/run.js:17:// Ports: 18122 vc-ts, 18130 gui-js, 18131 stub audio server.
services/gui-js/tests/run.js:76:// Stub audio server: serves a real MP3 file with Range support, the
services/gui-js/tests/run.js:78:let stubAudio = null;
services/gui-js/tests/run.js:79:function startStubAudio(port) {
services/gui-js/tests/run.js:82:    stubAudio = http.createServer((req, res) => {
services/gui-js/tests/run.js:100:    stubAudio.listen(port, () => resolve());
services/gui-js/tests/run.js:110:  await startStubAudio(18131);
services/gui-js/tests/run.js:119:  if (stubAudio) stubAudio.close();
services/gui-js/tests/run.js:167:test("audio proxy streams real bytes, Range passthrough, byte-exact", async () => {
services/radio-js/PROGRESS.md:67:## Still stubbed / untested
services/downloaders/README.md:81:  vars answer `engine_unavailable` naming them. Live-tested
services/downloaders/tests/test_providers.py:141:    def test_missing_env_is_engine_unavailable(self):
services/downloaders/tests/test_providers.py:147:            self.assertEqual(ctx.exception.code, "engine_unavailable")

## 10. Voice-grep hits (21 lines, for the record)

services/core-cpp/src/voice_call.hpp:80:class UnavailableVoiceCall final : public VoiceCall {
services/core-cpp/src/voice_call.hpp:82:    explicit UnavailableVoiceCall(std::string reason)
services/core-cpp/src/voice_call.hpp:158:    bool rtp_ok = false;
services/core-cpp/src/voice_call.hpp:262:// UnavailableVoiceCall with the exact reason (never a silent stub).
services/core-cpp/src/voice_call.cpp:488:        st.rtp_ok = (rtp_rc == 0);
services/core-cpp/src/voice_call.cpp:489:        if (!st.rtp_ok)
services/core-cpp/src/voice_call.cpp:919:    return std::make_unique<UnavailableVoiceCall>(reason);
services/core-cpp/README.md:125:  (dlopen'd `libntgcalls.so`) → `UnavailableVoiceCall` (reports the exact
services/core-cpp/README.md:169:  the RTP output reports `rtp_ok=0` honestly and the file encode still
services/core-cpp/PROGRESS.md:38:| `src/voice_call.hpp` | `VoiceCall` interface + `LoggingVoiceCall` stub (see below) |
services/vc-ts/src/main.js:63:    voice_joined: vs.joined,
services/vc-ts/src/voicecall.js:45:// (now-playing `voice` / `voice_joined` / `voice_reason`) — no silent
services/vc-ts/src/voicecall.js:399:    st.voice = `opus-file-only:${pipeline.rtp_error || "no RTP packets"}`;
services/vc-ts/src/voicecall.js:445:    else if (st.voice && st.voice.startsWith("opus-file-only") && voice.startsWith("unavailable")) voice = st.voice;
services/vc-ts/PROGRESS.md:162:  reports `voice` / `voice_joined` / `voice_reason`. `joined:true`
services/vc-ts/tests/run.js:227:  assert.equal(v.voice_joined, false, "never joined:true without a real join");
services/vc-rust/src/voice.rs:44://! the pipeline is proven, `"opus-file-only:<reason>"` when the Opus
services/vc-rust/src/voice.rs:957:                c.voice = format!("opus-file-only:{}", e);
services/vc-rust/PROGRESS.md:12:  resolve, `VoiceCall` trait + `LoggingVoiceCall` stub.
services/vc-rust/PROGRESS.md:97:- STUBBED: voice join/stream (LoggingVoiceCall, see README); no
services/vc-rust/PROGRESS.md:108:  `{"ok":false}` lines, 30 s bound). `main.rs`: `LoggingVoiceCall`

— End of worker A report. No service code modified. —
