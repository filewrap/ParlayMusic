// vc-ts tests — Phase 4 (Worker C). Node stdlib only: node:test +
// a child-process server + a stub Lemonic API on localhost.
//
// Run:  node --test tests/run.js
// Live-network tests (JioSaavn, LRCLib, yt-dlp) hit the real APIs;
// the Lemonic tests hit a local stub so keyed vs unauthenticated
// behaviour is deterministic. api.lemonic.xyz itself is unreachable
// from the sandbox, so the "real API down" case is tested against a
// closed port instead.
//
// Ports: 18102 main (no key), 18103 keyed, 18104 lemonic-down,
// 18109 stub Lemonic API. Data dirs under /tmp/p4-vcts/.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const http = require("node:http");
const { spawn } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const TOKEN = "p4-test-token";
const BASE = "/tmp/p4-vcts";
const CHAT = -1004242; // throwaway chat id for the suite

const R = require("../src/resolvers");
const voicecall = require("../src/voicecall");
const { execFileSync } = require("node:child_process");

// ---------------------------------------------------------------- helpers --
function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function req(port, method, p, body, token = TOKEN) {
  const res = await fetch(`http://127.0.0.1:${port}${p}`, {
    method,
    headers: {
      "content-type": "application/json",
      ...(token ? { "x-parlay-token": token } : {}),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let json = null;
  try { json = await res.json(); } catch { /* non-JSON */ }
  return { status: res.status, json };
}

function startServer(port, extraEnv) {
  const dataDir = path.join(BASE, `s${port}`, "data");
  fs.rmSync(path.join(BASE, `s${port}`), { recursive: true, force: true });
  fs.mkdirSync(dataDir, { recursive: true });
  const child = spawn("node", ["src/main.js"], {
    cwd: path.join(__dirname, ".."),
    env: {
      ...process.env,
      PORT: String(port),
      PARLAY_TOKEN: TOKEN,
      PARLAY_DATA_DIR: dataDir,
      ...extraEnv,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (d) => process.stdout.write(`[vc-ts:${port}] ${d}`));
  child.stderr.on("data", (d) => process.stderr.write(`[vc-ts:${port} ERR] ${d}`));
  return child;
}

async function waitHealth(port, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    try {
      const r = await req(port, "GET", "/healthz", undefined, null);
      if (r.status === 200 && r.json && r.json.ok) return r.json;
    } catch { /* not up yet */ }
    if (Date.now() > deadline) throw new Error(`server :${port} did not come up`);
    await sleep(250);
  }
}

// ------------------------------------------------------- stub Lemonic API --
const stubHits = []; // { path, authHeader }
let stubServer = null;

function startStubLemonic(port) {
  return new Promise((resolve) => {
    stubServer = http.createServer((req, res) => {
      stubHits.push({ path: req.url, auth: req.headers.authorization || null });
      const send = (obj) => {
        res.writeHead(200, { "content-type": "application/json" });
        res.end(JSON.stringify(obj));
      };
      if (req.url.startsWith("/api/search")) {
        return send({
          results: [
            { id: "stub1", title: "Stub Song One", artist: "Stub Artist", duration: 200, url: "https://example.invalid/stub1.mp3" },
            { id: "stub2", title: "Stub Song Two", artist: "Stub Artist", duration_sec: 150, url: "https://example.invalid/stub2.mp3" },
          ],
        });
      }
      const m = req.url.match(/^\/api\/raw\/track\/(.+)$/);
      if (m) {
        return send({
          data: {
            id: decodeURIComponent(m[1]),
            title: "Stub Raw Track",
            artist: "Stub Artist",
            duration: "3:20",
            url: "https://example.invalid/raw.mp3",
            stream_url: "https://example.invalid/raw.mp3",
          },
        });
      }
      res.writeHead(404);
      res.end("{}");
    });
    stubServer.listen(port, () => resolve());
  });
}

// ------------------------------------------------------------------ setup --
let main, keyed, down, broken;

test.before(async () => {
  // 5 s 440 Hz tone for the offline voice-pipeline proofs.
  fs.mkdirSync(BASE, { recursive: true });
  execFileSync("ffmpeg", ["-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=5", "-ar", "48000", "-ac", "2", path.join(BASE, "tone5.wav")]);
  // Fake yt-dlp for the android-retry unit test: it fails with
  // YouTube's bot-check text unless the android player client is
  // requested, then prints one video's JSON.
  const stubYtDlp = path.join(BASE, "stub-ytdlp.sh");
  fs.writeFileSync(stubYtDlp, [
    "#!/bin/sh",
    "case \"$*\" in",
    "  *player_client=android*)",
    "    printf '%s' '{\"id\":\"dQw4w9WgXcQ\",\"title\":\"Stub Video\",\"duration\":213,\"uploader\":\"Stub Channel\",\"webpage_url\":\"https://www.youtube.com/watch?v=dQw4w9WgXcQ\",\"thumbnail\":null}';;",
    "  *)",
    "    echo \"ERROR: Sign in to confirm you're not a bot\" >&2; exit 1;;",
    "esac",
    "",
  ].join("\n"));
  fs.chmodSync(stubYtDlp, 0o755);
  await startStubLemonic(18109);
  main = startServer(18102, { LEMONIC_API: "http://127.0.0.1:18109" });
  keyed = startServer(18103, {
    LEMONIC_API: "http://127.0.0.1:18109",
    LEMONIC_API_KEY: "testkey123",
  });
  // LEMONIC_API points at a closed port: every Lemonic fallback fails.
  down = startServer(18104, { LEMONIC_API: "http://127.0.0.1:9" });
  // YT_DLP points at a nonexistent binary: every yt-dlp resolve fails,
  // so the A9 behaviour (engine_unavailable, no placeholder) is testable.
  broken = startServer(18105, { YT_DLP: "/nonexistent/yt-dlp-xyz" });
  await waitHealth(18102);
  await waitHealth(18103);
  await waitHealth(18104);
  await waitHealth(18105);
});

test.after(() => {
  for (const c of [main, keyed, down, broken]) { try { c.kill("SIGTERM"); } catch { /* already dead */ } }
  if (stubServer) stubServer.close();
});

// ------------------------------------------------------------------ units --
test("parseDuration handles seconds, m:ss, h:mm:ss (regression: '4:28' was 0)", () => {
  assert.equal(R.parseDuration("4:28"), 268);
  assert.equal(R.parseDuration("1:02:03"), 3723);
  assert.equal(R.parseDuration(213), 213);
  assert.equal(R.parseDuration("268"), 268);
  assert.equal(R.parseDuration("0:45"), 45);
  assert.equal(R.parseDuration(undefined), 0);
  assert.equal(R.parseDuration("nonsense"), 0);
});

// ------------------------------------------------------- voice transport --
// Real Opus/RTP pipeline + in-process ntgcalls offer + join DELEGATED
// to the core (src/voicecall.js, Phase 7 A1). All offline: the tone
// is generated locally, the offer comes from the ntgcalls addon (or
// the wheel probe fallback until libntgcalls.so is built), and the
// join is proven against a stub core on 127.0.0.1:18110 that answers
// POST /v1/join — the engine calls the core, the core returns the
// join result. No test here fakes joined:true: the stub is the core
// in the test, and the core-down case proves the honest failure.
const TONE = path.join(BASE, "tone5.wav");
const VOICE_CHAT = -1004243; // throwaway chat id for the voice proofs
const STUB_CORE_PORT = 18110;
const STUB_CORE_URL = `http://127.0.0.1:${STUB_CORE_PORT}`;

// Stub core for the vc.join delegation proofs: records every request
// (method, path, token header, parsed body) and answers POST /v1/join
// from a scripted behaviour ({status, json}).
function startStubCore(behaviour) {
  const seen = [];
  const server = http.createServer((req, res) => {
    let body = "";
    req.on("data", (d) => { body += d; });
    req.on("end", () => {
      let parsed = null;
      try { parsed = JSON.parse(body); } catch { /* keep null */ }
      seen.push({ method: req.method, path: req.url, token: req.headers["x-parlay-token"] || null, body: parsed, raw: body });
      const answer = behaviour(req, parsed);
      res.writeHead(answer.status, { "content-type": "application/json" });
      res.end(JSON.stringify(answer.json));
    });
  });
  return new Promise((resolve) => {
    server.listen(STUB_CORE_PORT, "127.0.0.1", () => resolve({ server, seen }));
  });
}

function stopStubCore(stub) {
  return new Promise((resolve) => stub.server.close(() => resolve()));
}

test("voice: opus pipeline proves frames + RTP packets on a local tone", async () => {
  const p = await voicecall.opusPipeline(TONE, { workDir: path.join(BASE, "voice") });
  assert.equal(p.ok, true, JSON.stringify(p));
  assert.equal(p.frames, 251, `expected 251 Opus frames, got ${p.frames}`);
  assert.equal(p.bytes, 83736, `expected 83736 bytes, got ${p.bytes}`);
  assert.ok(Math.abs(p.duration_sec - 5.0065) < 0.01, `duration ${p.duration_sec}`);
  assert.equal(p.rtp_packets, 251, `expected 251 RTP packets, got ${p.rtp_packets}`);
  assert.equal(p.rtcp_packets, 1);
  assert.equal(p.rtp_bytes, 85973, `expected 85973 RTP bytes, got ${p.rtp_bytes}`);
  assert.ok(p.sdp.includes("m=audio 0 RTP/AVP 97"), "SDP media line");
  assert.ok(p.sdp.includes("a=rtpmap:97 opus/48000/2"), "SDP opus map");
  // The exact sandbox block on any live UDP receiver, even loopback.
  assert.ok(p.udp_error && p.udp_error.startsWith("EPERM"), `udp_error: ${p.udp_error}`);
});

test("voice: ntgcalls create_call produces a real local offer offline", async () => {
  const o = await voicecall.createLocalOffer(VOICE_CHAT);
  assert.equal(o.ok, true, JSON.stringify(o));
  assert.ok(o.offer_len > 100, `offer_len ${o.offer_len}`);
  assert.ok(o.offer.includes("ufrag"), "offer carries an ICE ufrag");
  // In-process addon when libntgcalls.so exists, wheel probe fallback
  // until the C build lands (phase5/ntg-build.log) — either is a real
  // ntgcalls offer; the path is named, never hidden.
  assert.ok(typeof o.via === "string" && o.via.length > 0, JSON.stringify(o));
});

test("voice: join delegates to the core — stub core answers joined:true", async () => {
  // The engine calls POST {CORE_URL}/v1/join and reports the core's
  // answer. The stub is the core in this test: it records the exact
  // request (method, path, token, chat_id) and answers joined:true.
  const stub = await startStubCore(() => ({
    status: 200,
    json: { ok: true, data: { joined: true, chat_id: VOICE_CHAT, now_playing: null } },
  }));
  try {
    const j = await voicecall.join(VOICE_CHAT, "dummy-offer", { coreUrl: STUB_CORE_URL, token: "test-token" });
    assert.equal(j.ok, true, JSON.stringify(j));
    assert.equal(j.joined, true, "joined:true comes from the core's answer");
    assert.equal(j.via, "core-cpp vc.join");
    assert.equal(stub.seen.length, 1, "exactly one core call");
    assert.equal(stub.seen[0].method, "POST");
    assert.equal(stub.seen[0].path, "/v1/join");
    assert.equal(stub.seen[0].token, "test-token", "IPC token forwarded");
    assert.deepEqual(stub.seen[0].body, { chat_id: VOICE_CHAT });
  } finally {
    await stopStubCore(stub);
  }
  // backendConfig now describes the in-process ntgcalls media state,
  // not bridge credentials. The addon loads exactly when the built
  // libntgcalls.so exists (phase5/ntg-build.log) — it is built now,
  // so addonLoaded is true in this tree; it would be false before
  // the C build lands.
  const cfg = voicecall.backendConfig();
  assert.equal(typeof cfg.describe, "string");
  assert.ok(cfg.describe.length > 0);
  const soBuilt = fs.existsSync("../../refs/ntgcalls/shared-output/lib/libntgcalls.so");
  assert.equal(cfg.addonLoaded, soBuilt, `addonLoaded ${cfg.addonLoaded} vs .so built ${soBuilt}`);
});

test("voice: join reports the core's 502 reason verbatim, never joined:true", async () => {
  const stub = await startStubCore(() => ({
    status: 502,
    json: { ok: false, error: { code: "engine_unavailable", message: "tdlib unavailable: no TD_API_ID/TD_API_HASH" } },
  }));
  try {
    const j = await voicecall.join(VOICE_CHAT, "dummy-offer", { coreUrl: STUB_CORE_URL, token: "test-token" });
    assert.equal(j.ok, false);
    assert.equal(j.joined, false);
    assert.equal(j.error, "tdlib unavailable: no TD_API_ID/TD_API_HASH", j.error);
  } finally {
    await stopStubCore(stub);
  }
});

test("voice: join with the core down fails honestly naming CORE_URL", async () => {
  // Port 9 is the discard port: nothing listens, connect is refused.
  const j = await voicecall.join(VOICE_CHAT, "dummy-offer", { coreUrl: "http://127.0.0.1:9", token: "test-token" });
  assert.equal(j.ok, false);
  assert.equal(j.joined, false);
  assert.ok(j.error.includes("core unreachable"), j.error);
  assert.ok(j.error.includes("CORE_URL"), j.error);
});

test("voice: playStream with a stub core joins for real (delegation)", async () => {
  // playStream reads CORE_URL / PARLAY_TOKEN from the env (the child
  // servers are already spawned, so this only affects this in-process
  // call). The stub core answers joined:true; the voice state must
  // then be live.
  const stub = await startStubCore(() => ({
    status: 200,
    json: { ok: true, data: { joined: true, chat_id: VOICE_CHAT, now_playing: null } },
  }));
  const savedCore = process.env.CORE_URL;
  const savedToken = process.env.PARLAY_TOKEN;
  process.env.CORE_URL = STUB_CORE_URL;
  process.env.PARLAY_TOKEN = "test-token";
  try {
    const r = await voicecall.playStream(VOICE_CHAT, TONE);
    assert.equal(r.streaming, true, JSON.stringify(r));
    assert.equal(r.voice, "live");
    assert.equal(r.pipeline.frames, 251);
    assert.equal(r.pipeline.rtp_packets, 251);
    assert.equal(r.via, "core-cpp vc.join");
    const st = voicecall.voiceState(VOICE_CHAT);
    assert.equal(st.voice, "live");
    assert.equal(st.joined, true, "joined:true only because the core answered it");
    assert.ok(st.offer_len > 100, `pre-flight offer recorded, len ${st.offer_len}`);
    assert.equal(stub.seen.length, 1, "one core join call for the play");
    assert.deepEqual(stub.seen[0].body, { chat_id: VOICE_CHAT });
  } finally {
    if (savedCore === undefined) delete process.env.CORE_URL;
    else process.env.CORE_URL = savedCore;
    if (savedToken === undefined) delete process.env.PARLAY_TOKEN;
    else process.env.PARLAY_TOKEN = savedToken;
    await stopStubCore(stub);
    // Leave the chat un-joined for the next voice test.
    voicecall.leaveVoiceChat(VOICE_CHAT);
  }
});

test("voice: playStream with the core down reports opus-rtp-ready + the core reason, never joined:true", async () => {
  const savedCore = process.env.CORE_URL;
  const savedToken = process.env.PARLAY_TOKEN;
  process.env.CORE_URL = "http://127.0.0.1:9";
  process.env.PARLAY_TOKEN = "test-token";
  try {
    const r = await voicecall.playStream(VOICE_CHAT, TONE);
    assert.equal(r.streaming, false, "no live call, so nothing streams");
    assert.equal(r.voice, "opus-rtp-ready");
    assert.equal(r.pipeline.frames, 251);
    assert.equal(r.pipeline.rtp_packets, 251);
    assert.ok(r.reason.includes("core unreachable"), r.reason);
    const st = voicecall.voiceState(VOICE_CHAT);
    assert.equal(st.voice, "opus-rtp-ready");
    assert.equal(st.joined, false);
    assert.ok(st.offer_len > 100, `pre-flight offer recorded, len ${st.offer_len}`);
  } finally {
    if (savedCore === undefined) delete process.env.CORE_URL;
    else process.env.CORE_URL = savedCore;
    if (savedToken === undefined) delete process.env.PARLAY_TOKEN;
    else process.env.PARLAY_TOKEN = savedToken;
  }
});

test("voice: now-playing surfaces the real voice state (string, joined false)", async () => {
  // Own chat id: must not disturb the shared CHAT room the jiosaavn
  // auto-advance / loop tests depend on.
  const vcChat = -1004244;
  const play = await req(18102, "POST", "/v1/play", { chat_id: vcChat, query: "jiosaavn:kesariya" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  const np = await req(18102, "GET", `/v1/now-playing?chat_id=${vcChat}`);
  assert.equal(np.status, 200, JSON.stringify(np.json));
  const v = np.json.data.now_playing;
  assert.equal(typeof v.voice, "string");
  assert.ok(v.voice.length > 0, "voice state is always a real string, never a stub label");
  assert.equal(v.voice_joined, false, "never joined:true without a real join");
  assert.equal(typeof v.voice_reason === "string" || v.voice_reason === null, true);
  await req(18102, "POST", "/v1/stop", { chat_id: vcChat });
});

// ------------------------------------------------------------- health/auth --
test("healthz reports lemonic mode; unauthenticated play is 401", async () => {
  const h = await req(18102, "GET", "/healthz", undefined, null);
  assert.equal(h.status, 200);
  assert.equal(h.json.service, "vc-ts");
  assert.equal(h.json.lemonic_mode, "unauthenticated");
  const hk = await req(18103, "GET", "/healthz", undefined, null);
  assert.equal(hk.json.lemonic_mode, "keyed");
  const bad = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: "x" }, null);
  assert.equal(bad.status, 401);
  assert.equal(bad.json.error.code, "unauthorized");
});

// --------------------------------------------------------------- jiosaavn --
test("jiosaavn search returns REAL durations (live jsvn-tgvc)", async () => {
  const r = await req(18102, "POST", "/v1/search", { query: "kesariya", source: "jiosaavn", limit: 3 });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.ok(r.json.data.results.length >= 1, "expected >=1 jiosaavn result");
  const first = r.json.data.results[0];
  assert.equal(first.id, "jsvn:rjkrTnma");
  assert.equal(first.title, "Kesariya");
  assert.equal(first.duration_sec, 268, `expected 268s from "4:28", got ${first.duration_sec}`);
  assert.ok(first.stream_url && first.stream_url.startsWith("https://"), "stream_url present");
  assert.equal(first.duration_source, "song-lookup");
});

test("jsvn: id resolve returns real duration + stream_url (both levels)", async () => {
  const r = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: "jsvn:rjkrTnma" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  const np = r.json.data.now_playing;
  const t = np.track;
  assert.equal(t.id, "jsvn:rjkrTnma");
  assert.equal(t.duration_sec, 268);
  assert.ok(t.stream_url);
  // A8 residual: the contract's top-level NowPlaying.stream_url must
  // mirror the nested track.stream_url.
  assert.equal(np.stream_url, t.stream_url, "top-level stream_url must mirror track.stream_url");
});

test("auto-advance fires for a jiosaavn track (seek to end -> next)", async () => {
  // Current: Kesariya (268s). Queue a second jiosaavn track, then seek
  // to 1s before the end; the 1s ticker must advance within ~3s.
  const q = await req(18102, "POST", "/v1/queue", { action: "add", chat_id: CHAT, query: "jiosaavn:tum hi ho" });
  assert.equal(q.status, 200, JSON.stringify(q.json));
  assert.equal(q.json.data.queue.length, 1);
  const s = await req(18102, "POST", "/v1/seek", { chat_id: CHAT, position_sec: 267 });
  assert.equal(s.status, 200);
  await sleep(3500);
  const np = await req(18102, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 200, JSON.stringify(np.json));
  assert.notEqual(np.json.data.now_playing.track.id, "jsvn:rjkrTnma", "should have auto-advanced off Kesariya");
  assert.ok(np.json.data.now_playing.track.duration_sec > 0, "next track also has a real duration");
  assert.equal(np.json.data.now_playing.queue.length, 0);
});

test("loop restarts the current track instead of advancing", async () => {
  const before = await req(18102, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  const track = before.json.data.now_playing.track;
  const dur = track.duration_sec;
  assert.ok(dur > 0);
  const p = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "loop" });
  assert.equal(p.status, 200);
  assert.equal(p.json.data.panel.loop, true);
  assert.equal(p.json.data.message, "Loop on.");
  await req(18102, "POST", "/v1/seek", { chat_id: CHAT, position_sec: dur - 1 });
  await sleep(3500);
  const np = await req(18102, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.json.data.now_playing.track.id, track.id, "loop must keep the same track");
  assert.ok(np.json.data.now_playing.position_sec < dur - 1, "position restarted");
  // Loop off again for later tests.
  await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "loop" });
});

// ------------------------------------------------------------------ panel --
test("panel GET returns remix-shaped state with panel_* buttons", async () => {
  const r = await req(18102, "GET", `/v1/panel?chat_id=${CHAT}`);
  assert.equal(r.status, 200, JSON.stringify(r.json));
  const panel = r.json.data.panel;
  assert.ok(panel.text.includes("🎵") || panel.text.includes("🔁"));
  assert.ok(panel.now && panel.now.title);
  assert.equal(panel.loop, false);
  const cbs = panel.buttons.map((b) => b.callback_data);
  for (const want of ["panel_shuffle", "panel_skip", "panel_pause", "panel_resume", "panel_mute", "panel_unmute", "panel_voldec", "panel_volinc", "panel_update"]) {
    assert.ok(cbs.includes(want), `missing button ${want}`);
  }
});

test("panel volume actions step by 10; mute/unmute round-trips", async () => {
  const v0 = await req(18102, "POST", "/v1/volume", { chat_id: CHAT, volume: 100 });
  assert.equal(v0.json.data.now_playing.volume, 100);
  const inc = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "volinc" });
  assert.equal(inc.json.data.panel.volume, 110);
  assert.equal(inc.json.data.message, "Volume 110%.");
  const dec = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "voldec" });
  assert.equal(dec.json.data.panel.volume, 100);
  const mute = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "mute" });
  assert.equal(mute.json.data.panel.volume, 0);
  const unmute = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "unmute" });
  assert.equal(unmute.json.data.panel.volume, 100);
  const bad = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "explode" });
  assert.equal(bad.status, 400);
});

test("volume endpoint validates 0-200", async () => {
  const r = await req(18102, "POST", "/v1/volume", { chat_id: CHAT, volume: 201 });
  assert.equal(r.status, 400);
  const okR = await req(18102, "POST", "/v1/volume", { chat_id: CHAT, volume: 150 });
  assert.equal(okR.status, 200);
  assert.equal(okR.json.data.now_playing.volume, 150);
  await req(18102, "POST", "/v1/volume", { chat_id: CHAT, volume: 100 });
});

test("panel shuffle reorders the queue; skip advances", async () => {
  await req(18102, "POST", "/v1/queue", { action: "clear", chat_id: CHAT });
  // Three songs distinct from the current track (which is "tum hi ho"
  // from the loop test), so a skip provably changes the current id.
  for (const q of ["jiosaavn:kesariya", "jiosaavn:raabta", "jiosaavn:kal ho naa ho"]) {
    await req(18102, "POST", "/v1/queue", { action: "add", chat_id: CHAT, query: q });
  }
  const before = await req(18102, "GET", `/v1/panel?chat_id=${CHAT}`);
  assert.equal(before.json.data.panel.queue_count, 3);
  const sh = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "shuffle" });
  assert.equal(sh.status, 200);
  assert.equal(sh.json.data.message, "Shuffling...");
  assert.equal(sh.json.data.panel.queue_count, 3);
  const cur = await req(18102, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  const curId = cur.json.data.now_playing.track.id;
  const sk = await req(18102, "POST", "/v1/panel", { chat_id: CHAT, action: "skip" });
  assert.equal(sk.json.data.message, "Skipped.");
  assert.notEqual(sk.json.data.now_playing.track.id, curId);
  assert.equal(sk.json.data.now_playing.queue.length, 2);
});

// ----------------------------------------------------------------- lyrics --
test("lyrics for the now-playing track come from LRCLib (live)", async () => {
  // Play a track LRCLib reliably answers for (Kesariya). NOTE: some
  // queries 503 on LRCLib itself (e.g. "Raabta" 503s consistently
  // while "Kesariya"/"Tum Hi Ho"/"Imagine" return 200) — that is an
  // upstream quirk, so the test pins a known-good title.
  await req(18102, "POST", "/v1/stop", { chat_id: CHAT });
  const play = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: "jiosaavn:kesariya" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  assert.equal(play.json.data.now_playing.track.id, "jsvn:rjkrTnma");
  let r = null;
  for (let i = 0; i < 3; i++) {
    r = await req(18102, "POST", "/v1/lyrics", { chat_id: CHAT });
    if (r.status === 200) break;
    await sleep(1500);
  }
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.source, "lrclib");
  assert.ok(r.json.data.lyrics.length > 50, "lyrics text is real content");
});

test("lyrics by explicit title; unknown title is an honest 404", async () => {
  const r = await req(18102, "POST", "/v1/lyrics", { title: "Kesariya", artist: "Arijit Singh" });
  assert.equal(r.status, 200);
  assert.ok(r.json.data.lyrics.includes("मुझको") || r.json.data.lyrics.toLowerCase().includes("kesariya") || r.json.data.lyrics.length > 100);
  const miss = await req(18102, "POST", "/v1/lyrics", { title: "xqzzy-no-such-song-12345", artist: "nobody" });
  assert.equal(miss.status, 404);
  assert.equal(miss.json.error.code, "not_found");
});

// ------------------------------------------------------------------ radio --
test("raw http(s) URL plays as a radio track (duration 0, never advances)", async () => {
  const url = "https://ice1.somafm.com/groovesalad-128-mp3";
  // Stop first so the radio track becomes current immediately.
  await req(18102, "POST", "/v1/stop", { chat_id: CHAT });
  const r = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: `radio:${url}` });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  const t = r.json.data.now_playing.track;
  assert.equal(t.source, "radio");
  assert.equal(t.duration_sec, 0);
  assert.equal(t.stream_url, url);
  // Wait past two ticker ticks: the track must still be current.
  await sleep(2500);
  const np = await req(18102, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.json.data.now_playing.track.source, "radio");
  assert.equal(np.json.data.now_playing.track.id, t.id);
});

test("YouTube URL still resolves as youtube, not radio (regression)", async () => {
  const r = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
  const q = await req(18102, "POST", "/v1/queue", { action: "list", chat_id: CHAT });
  const yt = q.json.data.queue.find((it) => it.track.id === "yt:dQw4w9WgXcQ");
  if (r.status === 502) {
    // YouTube gated right now: the honest failure — and, the A9 point,
    // no fake placeholder track was queued in its place.
    assert.equal(r.json.error.code, "engine_unavailable");
    assert.equal(yt, undefined, "a failed resolve must not queue a placeholder");
    return;
  }
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.ok(yt, "youtube track queued with yt: id");
  assert.equal(yt.track.source, "youtube");
  assert.equal(yt.track.duration_sec, 213);
});

// ---------------------------------------------------------------- lemonics --
test("lemonic search unauthenticated (stub): results, no Authorization header", async () => {
  stubHits.length = 0;
  const r = await req(18102, "POST", "/v1/lemonic/search", { query: "stub" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.results.length, 2);
  assert.equal(r.json.data.via, "lemonic");
  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
  assert.ok(hit, "stub saw /api/search");
  assert.equal(hit.auth, null, "unauthenticated path must not send Authorization");
});

test("lemonic search keyed (stub): Bearer header sent, same results", async () => {
  stubHits.length = 0;
  const r = await req(18103, "POST", "/v1/lemonic/search", { query: "stub" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.via, "lemonic-keyed");
  const hit = stubHits.find((h) => h.path.startsWith("/api/search"));
  assert.ok(hit);
  assert.equal(hit.auth, "Bearer testkey123");
});

test("lemonic raw track via stub (unauthenticated) + download route", async () => {
  const r = await req(18102, "POST", "/v1/lemonic/download", { track_id: "stub1" });
  // The stub's stream URL is example.invalid, so the download fetch
  // fails — but the failure must be download_failed (raw track was
  // read fine), not engine_unavailable.
  assert.equal(r.status, 502, JSON.stringify(r.json));
  assert.equal(r.json.error.code, "download_failed");
});

test("lemonic search with dead API + no Spotify creds: honest engine_unavailable naming both fallbacks", async () => {
  const r = await req(18104, "POST", "/v1/lemonic/search", { query: "kesariya" });
  assert.equal(r.status, 502, JSON.stringify(r.json));
  assert.equal(r.json.error.code, "engine_unavailable");
  assert.ok(r.json.error.message.includes("lemonic /api/search"), r.json.error.message);
  assert.ok(r.json.error.message.includes("spotify search"), r.json.error.message);
});

// -------------------------------------------------------------- regression --
test("queue add/remove/clear + stop still work", async () => {
  await req(18102, "POST", "/v1/stop", { chat_id: CHAT });
  const add = await req(18102, "POST", "/v1/queue", { action: "add", chat_id: CHAT, track: { id: "t1", title: "One", source: "file" } });
  assert.equal(add.json.data.queue.length, 1);
  const add2 = await req(18102, "POST", "/v1/queue", { action: "add", chat_id: CHAT, track: { id: "t2", title: "Two", source: "file" } });
  assert.equal(add2.json.data.queue.length, 2);
  const rem = await req(18102, "POST", "/v1/queue", { action: "remove", chat_id: CHAT, position: 0 });
  assert.equal(rem.json.data.queue[0].track.title, "Two");
  const clr = await req(18102, "POST", "/v1/queue", { action: "clear", chat_id: CHAT });
  assert.deepEqual(clr.json.data.queue, []);
});

// ------------------------------------------------- Phase 5: A9 placeholders --
// A failed YouTube resolve must surface engine_unavailable, never a
// fake track. The broken server (:18105) has YT_DLP pointed at a
// nonexistent binary, so every yt-dlp call fails there.
const BROKEN_CHAT = -1004247;

test("A9 unit: isBotCheck spots YouTube's bot-check text", () => {
  assert.equal(R.isBotCheck("ERROR: Sign in to confirm you're not a bot"), true);
  assert.equal(R.isBotCheck("... not a bot ..."), true);
  assert.equal(R.isBotCheck("ERROR: Video unavailable"), false);
  assert.equal(R.isBotCheck(""), false);
});

test("A9 unit: resolveYouTube rejects engine_unavailable when yt-dlp is missing", async () => {
  const saved = process.env.YT_DLP;
  process.env.YT_DLP = "/nonexistent/yt-dlp-xyz";
  try {
    await assert.rejects(
      R.resolveYouTube("https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
      (e) => e.code === "engine_unavailable" && e.message.includes("yt-dlp")
    );
    await assert.rejects(
      R.resolveYouTube("some free text query"),
      (e) => e.code === "engine_unavailable" && e.message.includes("yt-dlp")
    );
  } finally {
    if (saved === undefined) delete process.env.YT_DLP;
    else process.env.YT_DLP = saved;
  }
});

test("A9 unit: bot-check failure retries once with the android client (stub yt-dlp)", async () => {
  const saved = process.env.YT_DLP;
  process.env.YT_DLP = path.join(BASE, "stub-ytdlp.sh");
  try {
    // The stub fails the first call with bot-check text and answers
    // only when the android player client is requested.
    const { track } = await R.resolveYouTube("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    assert.equal(track.id, "yt:dQw4w9WgXcQ");
    assert.equal(track.title, "Stub Video");
    assert.equal(track.duration_sec, 213);
    const { track: searched } = await R.resolveYouTube("a free text query");
    assert.equal(searched.id, "yt:dQw4w9WgXcQ");
  } finally {
    if (saved === undefined) delete process.env.YT_DLP;
    else process.env.YT_DLP = saved;
  }
});

test("A9 http: failed YouTube resolve is 502 engine_unavailable, nothing queued", async () => {
  const playUrl = await req(18105, "POST", "/v1/play", { chat_id: BROKEN_CHAT, query: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
  assert.equal(playUrl.status, 502, JSON.stringify(playUrl.json));
  assert.equal(playUrl.json.error.code, "engine_unavailable");
  const playQuery = await req(18105, "POST", "/v1/play", { chat_id: BROKEN_CHAT, query: "never gonna give you up" });
  assert.equal(playQuery.status, 502);
  assert.equal(playQuery.json.error.code, "engine_unavailable");
  // No placeholder was queued: the room has no playback state at all.
  const np = await req(18105, "GET", `/v1/now-playing?chat_id=${BROKEN_CHAT}`);
  assert.equal(np.status, 404);
  assert.equal(np.json.error.code, "not_found");
  const q = await req(18105, "POST", "/v1/queue", { action: "list", chat_id: BROKEN_CHAT });
  assert.deepEqual(q.json.data.queue, []);
  const add = await req(18105, "POST", "/v1/queue", { action: "add", chat_id: BROKEN_CHAT, query: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
  assert.equal(add.status, 502);
  assert.equal(add.json.error.code, "engine_unavailable");
  const search = await req(18105, "POST", "/v1/search", { query: "kesariya", source: "youtube" });
  assert.equal(search.status, 502);
  assert.equal(search.json.error.code, "engine_unavailable");
});

// ------------------------------------------------- Phase 5: loop/shuffle/autoplay --
// Dedicated chats so these tests never disturb the shared CHAT room.
const LOOP_CHAT = -1004245;
const AUTO_CHAT = -1004246;

test("loop route sets a 0-10 count, validates, reports an integer", async () => {
  const play = await req(18102, "POST", "/v1/play", { chat_id: LOOP_CHAT, query: "jiosaavn:kesariya" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  assert.equal(play.json.data.now_playing.track.id, "jsvn:rjkrTnma");
  await req(18102, "POST", "/v1/queue", { action: "add", chat_id: LOOP_CHAT, query: "jiosaavn:tum hi ho" });
  const set = await req(18102, "POST", "/v1/loop", { chat_id: LOOP_CHAT, count: 2 });
  assert.equal(set.status, 200, JSON.stringify(set.json));
  assert.equal(set.json.data.now_playing.loop, 2);
  assert.equal(typeof set.json.data.now_playing.loop, "number");
  for (const bad of [{ count: 11 }, { count: -1 }, { count: 2.5 }, {}]) {
    const r = await req(18102, "POST", "/v1/loop", { chat_id: LOOP_CHAT, ...bad });
    assert.equal(r.status, 400, JSON.stringify(r.json));
    assert.equal(r.json.error.code, "bad_request");
  }
  const missing = await req(18102, "POST", "/v1/loop", { chat_id: -999999, count: 1 });
  assert.equal(missing.status, 404);
  assert.equal(missing.json.error.code, "not_found");
});

test("loop count replays the track, drains, then advances", async () => {
  // Current: Kesariya (268 s), loop 2, Tum Hi Ho queued.
  const seek = async () => {
    const s = await req(18102, "POST", "/v1/seek", { chat_id: LOOP_CHAT, position_sec: 267 });
    assert.equal(s.status, 200);
    await sleep(3500);
    const np = await req(18102, "GET", `/v1/now-playing?chat_id=${LOOP_CHAT}`);
    assert.equal(np.status, 200, JSON.stringify(np.json));
    return np.json.data.now_playing;
  };
  let np = await seek();
  assert.equal(np.track.id, "jsvn:rjkrTnma", "loop 2 -> replay 1");
  assert.equal(np.loop, 1);
  np = await seek();
  assert.equal(np.track.id, "jsvn:rjkrTnma", "loop 1 -> replay 2");
  assert.equal(np.loop, 0);
  np = await seek();
  assert.notEqual(np.track.id, "jsvn:rjkrTnma", "loop drained -> advance to queue");
  assert.equal(np.queue.length, 0);
  assert.ok(np.track.duration_sec > 0, "next track has a real duration");
});

test("shuffle route toggles mode; skip pops from the queue", async () => {
  // Current: Tum Hi Ho (from the loop test). Queue 3 fake tracks.
  for (const [id, title] of [["sh1", "Shuffle One"], ["sh2", "Shuffle Two"], ["sh3", "Shuffle Three"]]) {
    const a = await req(18102, "POST", "/v1/queue", { action: "add", chat_id: LOOP_CHAT, track: { id, title, source: "file" } });
    assert.equal(a.status, 200, JSON.stringify(a.json));
  }
  const on = await req(18102, "POST", "/v1/shuffle", { chat_id: LOOP_CHAT, enabled: true });
  assert.equal(on.status, 200, JSON.stringify(on.json));
  assert.equal(on.json.data.now_playing.shuffle, true);
  const list = await req(18102, "POST", "/v1/queue", { action: "list", chat_id: LOOP_CHAT });
  assert.deepEqual(
    list.json.data.queue.map((it) => it.track.id).sort(),
    ["sh1", "sh2", "sh3"],
    "shuffle keeps every queued track"
  );
  const sk = await req(18102, "POST", "/v1/skip", { chat_id: LOOP_CHAT });
  assert.equal(sk.status, 200, JSON.stringify(sk.json));
  assert.ok(["sh1", "sh2", "sh3"].includes(sk.json.data.now_playing.track.id), "skip popped a queued track");
  assert.equal(sk.json.data.now_playing.queue.length, 2);
  assert.equal(sk.json.data.now_playing.loop, 0, "skip resets the loop count");
  const off = await req(18102, "POST", "/v1/shuffle", { chat_id: LOOP_CHAT, enabled: false });
  assert.equal(off.json.data.now_playing.shuffle, false);
  const bad = await req(18102, "POST", "/v1/shuffle", { chat_id: LOOP_CHAT, enabled: "yes" });
  assert.equal(bad.status, 400);
  const missing = await req(18102, "POST", "/v1/shuffle", { chat_id: -999999, enabled: true });
  assert.equal(missing.status, 404);
  await req(18102, "POST", "/v1/stop", { chat_id: LOOP_CHAT });
});

test("autoplay route toggles mode; autoplay-skip needs it on", async () => {
  const none = await req(18102, "POST", "/v1/autoplay-skip", { chat_id: -999999 });
  assert.equal(none.status, 404);
  // Play a real YouTube track; if YouTube is gated right now the
  // honest 502 is the whole test — autoplay cannot pick without it.
  const play = await req(18102, "POST", "/v1/play", { chat_id: AUTO_CHAT, query: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
  if (play.status === 502) {
    assert.equal(play.json.error.code, "engine_unavailable");
    return;
  }
  assert.equal(play.status, 200, JSON.stringify(play.json));
  assert.equal(play.json.data.now_playing.track.id, "yt:dQw4w9WgXcQ");
  assert.ok(play.json.data.now_playing.track.duration_sec > 0);
  const skipOff = await req(18102, "POST", "/v1/autoplay-skip", { chat_id: AUTO_CHAT });
  assert.equal(skipOff.status, 400, JSON.stringify(skipOff.json));
  assert.equal(skipOff.json.error.code, "bad_request");
  const on = await req(18102, "POST", "/v1/autoplay", { chat_id: AUTO_CHAT, enabled: true });
  assert.equal(on.status, 200, JSON.stringify(on.json));
  assert.equal(on.json.data.now_playing.autoplay, true);
  const bad = await req(18102, "POST", "/v1/autoplay", { chat_id: AUTO_CHAT, enabled: "yes" });
  assert.equal(bad.status, 400);
  const skip = await req(18102, "POST", "/v1/autoplay-skip", { chat_id: AUTO_CHAT });
  assert.equal(skip.status, 200, JSON.stringify(skip.json));
  const picked = skip.json.data.now_playing.track;
  assert.ok(picked.id.startsWith("yt:"), `expected a YouTube mix pick, got ${picked.id}`);
  assert.notEqual(picked.id, "yt:dQw4w9WgXcQ", "the pick is not the track it replaces");
  assert.ok(picked.duration_sec > 0, "the mix pick has a real duration");
  assert.equal(skip.json.data.now_playing.loop, 0, "autoplay-skip resets the loop count");
  // Drain: seek to the end of the mix pick; the ticker must fetch a
  // second mix pick (room goes idle while it is in flight, so
  // now-playing 404s meanwhile — keep polling).
  const dur = picked.duration_sec;
  const s = await req(18102, "POST", "/v1/seek", { chat_id: AUTO_CHAT, position_sec: dur - 1 });
  assert.equal(s.status, 200);
  let drained = null;
  for (let i = 0; i < 15; i++) {
    await sleep(2000);
    const np = await req(18102, "GET", `/v1/now-playing?chat_id=${AUTO_CHAT}`);
    if (np.status === 200 && np.json.data.now_playing.track) {
      if (np.json.data.now_playing.track.id !== picked.id) { drained = np.json.data.now_playing.track; break; }
    }
    // 404 = room idle while the mix pick is in flight; keep waiting.
  }
  assert.ok(drained, "autoplay on drain must start a second mix pick");
  assert.ok(drained.id.startsWith("yt:"), `expected a YouTube mix pick, got ${drained.id}`);
  assert.notEqual(drained.id, picked.id);
  const off = await req(18102, "POST", "/v1/autoplay", { chat_id: AUTO_CHAT, enabled: false });
  assert.equal(off.json.data.now_playing.autoplay, false);
  await req(18102, "POST", "/v1/stop", { chat_id: AUTO_CHAT });
});
