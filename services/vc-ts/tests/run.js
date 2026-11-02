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
let main, keyed, down;

test.before(async () => {
  // 5 s 440 Hz tone for the offline voice-pipeline proofs.
  fs.mkdirSync(BASE, { recursive: true });
  execFileSync("ffmpeg", ["-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=5", "-ar", "48000", "-ac", "2", path.join(BASE, "tone5.wav")]);
  await startStubLemonic(18109);
  main = startServer(18102, { LEMONIC_API: "http://127.0.0.1:18109" });
  keyed = startServer(18103, {
    LEMONIC_API: "http://127.0.0.1:18109",
    LEMONIC_API_KEY: "testkey123",
  });
  // LEMONIC_API points at a closed port: every Lemonic fallback fails.
  down = startServer(18104, { LEMONIC_API: "http://127.0.0.1:9" });
  await waitHealth(18102);
  await waitHealth(18103);
  await waitHealth(18104);
});

test.after(() => {
  for (const c of [main, keyed, down]) { try { c.kill("SIGTERM"); } catch { /* already dead */ } }
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
// Real Opus/RTP pipeline + ntgcalls offer + join client (src/voicecall.js).
// All offline: the tone is generated locally, the offer comes from the
// ntgcalls wheel, and the join fails at the (missing) MTProto config.
const TONE = path.join(BASE, "tone5.wav");
const VOICE_CHAT = -1004243; // throwaway chat id for the voice proofs

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
});

test("voice: join without credentials names the exact missing pieces", async () => {
  const saved = {};
  for (const k of ["ASSISTANT_SESSION_FILE", "API_ID", "API_HASH"]) {
    if (k in process.env) { saved[k] = process.env[k]; delete process.env[k]; }
  }
  try {
    const j = await voicecall.join(VOICE_CHAT, "dummy-offer");
    assert.equal(j.ok, false);
    assert.equal(j.joined, false);
    assert.ok(j.error.includes("ASSISTANT_SESSION_FILE"), j.error);
    assert.ok(j.error.includes("API_ID"), j.error);
    assert.ok(j.error.includes("API_HASH"), j.error);
  } finally {
    Object.assign(process.env, saved);
  }
});

test("voice: playStream on a local tone reports opus-rtp-ready + exact join reason, never joined:true", async () => {
  const r = await voicecall.playStream(VOICE_CHAT, TONE);
  assert.equal(r.streaming, false, "no live call, so nothing streams");
  assert.equal(r.voice, "opus-rtp-ready");
  assert.equal(r.pipeline.frames, 251);
  assert.equal(r.pipeline.rtp_packets, 251);
  assert.ok(r.reason.includes("ASSISTANT_SESSION_FILE"), r.reason);
  const st = voicecall.voiceState(VOICE_CHAT);
  assert.equal(st.voice, "opus-rtp-ready");
  assert.equal(st.joined, false);
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

test("jsvn: id resolve returns real duration + stream_url", async () => {
  const r = await req(18102, "POST", "/v1/play", { chat_id: CHAT, query: "jsvn:rjkrTnma" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  const t = r.json.data.now_playing.track;
  assert.equal(t.id, "jsvn:rjkrTnma");
  assert.equal(t.duration_sec, 268);
  assert.ok(t.stream_url);
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
  assert.equal(r.status, 200, JSON.stringify(r.json));
  const q = await req(18102, "POST", "/v1/queue", { action: "list", chat_id: CHAT });
  const yt = q.json.data.queue.find((it) => it.track.id === "yt:dQw4w9WgXcQ");
  assert.ok(yt, "youtube track queued with yt: id");
  assert.equal(yt.track.source, "youtube");
  if (!yt.track.degraded) assert.equal(yt.track.duration_sec, 213);
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
