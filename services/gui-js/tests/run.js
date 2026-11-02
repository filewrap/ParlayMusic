// gui-js tests — Phase 4 (Worker C). Node stdlib only: node:test.
// Verifies the real in-browser playback path:
//   * the page ships a real <audio> element + WebAudio gain wiring
//   * /api/stream-url resolves the current track to a direct audio URL
//     from the IPC layer (vc-ts), or honestly says it cannot
//   * /api/audio proxies real audio bytes same-origin (Range
//     passthrough, byte-exact) so the GainNode actually changes gain
//
// Sandbox limit (recorded, not hidden): aac.saavncdn.com returns 404
// for every fresh media URL through the sandbox egress proxy, so the
// proxy's byte path is tested against a local stub audio server
// serving a real ffmpeg-generated MP3; the stream-url path is tested
// against live vc-ts/JioSaavn metadata. On a normal network the same
// proxy code serves the Saavn bytes.
//
// Run:  node --test tests/run.js
// Ports: 18122 vc-ts, 18130 gui-js, 18131 stub audio server,
//   18132 gui-js with CORE_URL -> stub core, 18133 stub core,
//   18134 gui-js with CORE_URL -> dead port (core unreachable).
// Data dirs under /tmp/p4-gui/.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const http = require("node:http");
const { spawn, execFileSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const TOKEN = "p4-test-token";
const BASE = "/tmp/p4-gui";
const CHAT = -1004444;
const TONE = path.join(BASE, "tone.mp3");

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
  return { status: res.status, json: await res.json().catch(() => null), res };
}

function startService(dir, port, extraEnv, entry = "src/main.js") {
  const dataDir = path.join(BASE, `s${port}`, "data");
  fs.rmSync(path.join(BASE, `s${port}`), { recursive: true, force: true });
  fs.mkdirSync(dataDir, { recursive: true });
  const child = spawn("node", [entry], {
    cwd: path.join(__dirname, "..", "..", dir),
    env: { ...process.env, PORT: String(port), PARLAY_TOKEN: TOKEN, PARLAY_DATA_DIR: dataDir, ...extraEnv },
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (d) => process.stdout.write(`[${dir}:${port}] ${d}`));
  child.stderr.on("data", (d) => process.stderr.write(`[${dir}:${port} ERR] ${d}`));
  return child;
}

async function waitHealth(port, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    try {
      const r = await req(port, "GET", "/healthz", undefined, null);
      if (r.status === 200 && r.json && r.json.ok) return r.json;
    } catch { /* not up yet */ }
    if (Date.now() > deadline) throw new Error(`service :${port} did not come up`);
    await sleep(250);
  }
}

// Stub audio server: serves a real MP3 file with Range support, the
// way a CDN would. Used as the /api/audio upstream.
let stubAudio = null;
function startStubAudio(port) {
  return new Promise((resolve) => {
    const buf = fs.readFileSync(TONE);
    stubAudio = http.createServer((req, res) => {
      if (req.url !== "/tone.mp3") { res.writeHead(404); return res.end(); }
      const range = req.headers.range;
      if (range) {
        const m = range.match(/bytes=(\d+)-(\d*)/);
        const start = Number(m[1]);
        const end = m[2] === "" ? buf.length - 1 : Math.min(Number(m[2]), buf.length - 1);
        res.writeHead(206, {
          "content-type": "audio/mpeg",
          "content-range": `bytes ${start}-${end}/${buf.length}`,
          "content-length": end - start + 1,
          "accept-ranges": "bytes",
        });
        return res.end(buf.subarray(start, end + 1));
      }
      res.writeHead(200, { "content-type": "audio/mpeg", "content-length": buf.length, "accept-ranges": "bytes" });
      res.end(buf);
    });
    stubAudio.listen(port, () => resolve());
  });
}

let vcts, gui, guiCore, guiDead, stubCore;

// Stub core (Phase 6, A2): answers the core's flat HTTP shape
// ({"ok":true,"now_playing":{...}} / {"ok":false,"error":"..."}).
// -100999 has no playback state; every other chat plays CORE_NP.
const CORE_NP = {
  chat_id: CHAT, engine: "go",
  track: { id: "core-test-track", title: "Core Test Track", artist: "Core Artist", duration_sec: 200, source: "youtube", url: "" },
  paused: false, position_sec: 42, volume: 80,
  queue: [{ position: 0, requested_by: 1, track: { id: "core-test-next", title: "Core Next", artist: "Core Artist", duration_sec: 180, source: "youtube", url: "" } }],
};
function startStubCore(port) {
  return new Promise((resolve) => {
    stubCore = http.createServer((req, res) => {
      const u = new URL(req.url, "http://127.0.0.1");
      const send = (status, obj) => {
        res.writeHead(status, { "content-type": "application/json" });
        res.end(JSON.stringify(obj));
      };
      if (u.pathname === "/healthz") return send(200, { ok: true });
      if (u.pathname === "/api/now-playing") {
        if (u.searchParams.get("chat_id") === "-100999") {
          return send(404, { ok: false, error: "no playback state" });
        }
        return send(200, { ok: true, now_playing: CORE_NP });
      }
      if (u.pathname === "/api/queue") {
        return send(200, { ok: true, queue: CORE_NP.queue, chat_id: Number(u.searchParams.get("chat_id")) || 0 });
      }
      return send(404, { ok: false, error: "not found" });
    });
    stubCore.listen(port, () => resolve());
  });
}

test.before(async () => {
  fs.mkdirSync(BASE, { recursive: true });
  execFileSync("ffmpeg", ["-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-codec:a", "libmp3lame", "-q:a", "4", TONE]);
  assert.ok(fs.statSync(TONE).size > 1000, "tone mp3 generated");
  await startStubAudio(18131);
  vcts = startService("vc-ts", 18122, {});
  gui = startService("gui-js", 18130, { VC_TS_URL: "http://127.0.0.1:18122" }, "app/main.js");
  await waitHealth(18122);
  await waitHealth(18130);
  // Phase 6 (A2): one gui with a live (stub) core, one with a dead one.
  await startStubCore(18133);
  guiCore = startService("gui-js", 18132, { CORE_URL: "http://127.0.0.1:18133" }, "app/main.js");
  guiDead = startService("gui-js", 18134, { CORE_URL: "http://127.0.0.1:9" }, "app/main.js");
  await waitHealth(18132);
  await waitHealth(18134);
});

test.after(() => {
  for (const c of [vcts, gui, guiCore, guiDead]) { try { c.kill("SIGTERM"); } catch { /* dead */ } }
  if (stubAudio) stubAudio.close();
  if (stubCore) stubCore.close();
});

test("player page ships a real <audio> element + WebAudio gain wiring", async () => {
  const r = await fetch("http://127.0.0.1:18130/");
  assert.equal(r.status, 200);
  const html = await r.text();
  assert.ok(html.includes('<audio id="player"'), "audio element present");
  assert.ok(html.includes("createMediaElementSource"), "MediaElementAudioSourceNode wiring present");
  assert.ok(html.includes("createGain"), "GainNode present");
  assert.ok(html.includes("gainNode.gain.value = v / 100"), "slider drives gainNode.gain (not display-only)");
  assert.ok(html.includes("audioEl.volume = v / 100"), "element-volume fallback present");
  assert.ok(html.includes('id="btnListen"'), "listen toggle present");
  assert.ok(html.includes('id="seek"'), "seek slider present");
  assert.ok(html.includes("/api/stream-url"), "stream-url lookup present");
  assert.ok(html.includes("refreshStream"), "stream refresh wiring present");
  assert.ok(html.includes("Telegram.WebApp"), "Telegram WebApp hook kept");
});

test("stream-url resolves a live jiosaavn track to a real direct URL", async () => {
  const play = await req(18122, "POST", "/v1/play", { chat_id: CHAT, query: "jiosaavn:kesariya" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  assert.equal(play.json.data.now_playing.track.duration_sec, 268);
  const r = await req(18130, "GET", `/api/stream-url?chat_id=${CHAT}`);
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.playable, true);
  assert.equal(r.json.data.source, "jiosaavn");
  assert.ok(r.json.data.direct_url.startsWith("https://aac.saavncdn.com/"), r.json.data.direct_url);
  assert.ok(r.json.data.url.startsWith("/api/audio?src="), "src is the same-origin proxy");
  assert.equal(r.json.data.track.id, "jsvn:rjkrTnma");
});

test("stream-url is honest for a YouTube track (no direct audio)", async () => {
  await req(18122, "POST", "/v1/stop", { chat_id: CHAT });
  const play = await req(18122, "POST", "/v1/play", { chat_id: CHAT, query: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  const r = await req(18130, "GET", `/api/stream-url?chat_id=${CHAT}`);
  assert.equal(r.json.data.playable, false);
  assert.ok(r.json.data.reason.includes("YouTube"), r.json.data.reason);
});

test("stream-url with nothing playing is honest", async () => {
  await req(18122, "POST", "/v1/stop", { chat_id: CHAT });
  const r = await req(18130, "GET", `/api/stream-url?chat_id=${CHAT}`);
  assert.equal(r.json.data.playable, false);
  assert.equal(r.json.data.reason, "nothing playing");
});

test("audio proxy streams real bytes, Range passthrough, byte-exact", async () => {
  const tone = fs.readFileSync(TONE);
  const src = encodeURIComponent("http://127.0.0.1:18131/tone.mp3");
  // Full fetch: byte-identical body.
  const full = await fetch(`http://127.0.0.1:18130/api/audio?src=${src}`);
  assert.equal(full.status, 200);
  assert.ok((full.headers.get("content-type") || "").startsWith("audio/"), full.headers.get("content-type"));
  const fullBuf = Buffer.from(await full.arrayBuffer());
  assert.ok(fullBuf.equals(tone), "proxied body is byte-identical to the source MP3");
  // Range fetch: 206, content-range, exact slice.
  const part = await fetch(`http://127.0.0.1:18130/api/audio?src=${src}`, { headers: { range: "bytes=100-199" } });
  assert.equal(part.status, 206);
  assert.equal(part.headers.get("content-range"), `bytes 100-199/${tone.length}`);
  const partBuf = Buffer.from(await part.arrayBuffer());
  assert.equal(partBuf.length, 100);
  assert.ok(partBuf.equals(tone.subarray(100, 200)), "range slice is byte-identical");
});

test("audio proxy rejects bad input and dead upstreams honestly", async () => {
  const bad = await req(18130, "GET", "/api/audio?src=not-a-url");
  assert.equal(bad.status, 400);
  const dead = await req(18130, "GET", "/api/audio?src=" + encodeURIComponent("http://127.0.0.1:9/nope.mp3"));
  assert.equal(dead.status, 502);
  assert.equal(dead.json.error.code, "download_failed");
});

test("/api/play still proxies to vc-ts (engine ts)", async () => {
  const r = await req(18130, "POST", "/api/play", { chat_id: CHAT, query: "jiosaavn:kesariya" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.engine, "ts");
  assert.equal(r.json.data.now_playing.track.id, "jsvn:rjkrTnma");
  await req(18122, "POST", "/v1/stop", { chat_id: CHAT });
});

// --- Phase 6 (A2): CORE_URL configured -> core is the only source ---
// --- of truth; demo state is dev-only (CORE_URL unset). ---

test("CORE_URL set: now-playing/queue proxy the core (flat shape), never demo", async () => {
  const np = await req(18132, "GET", `/api/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 200, JSON.stringify(np.json));
  assert.equal(np.json.ok, true);
  assert.equal(np.json.data.now_playing.track.id, "core-test-track");
  assert.equal(np.json.data.now_playing.source, "core");
  assert.ok(!JSON.stringify(np.json).includes("local-demo"), "no demo leakage");
  const q = await req(18132, "GET", `/api/queue?chat_id=${CHAT}`);
  assert.equal(q.status, 200, JSON.stringify(q.json));
  assert.equal(q.json.data.source, "core");
  assert.equal(q.json.data.queue[0].track.id, "core-test-next");
});

test("CORE_URL set: core 404 passes through as an honest error, not demo", async () => {
  const np = await req(18132, "GET", "/api/now-playing?chat_id=-100999");
  assert.equal(np.status, 404, JSON.stringify(np.json));
  assert.equal(np.json.ok, false);
  assert.equal(np.json.error, "no playback state");
  assert.ok(!JSON.stringify(np.json).includes("local-demo"), "no demo leakage");
});

test("CORE_URL set + core down: honest core-unreachable error, never demo", async () => {
  const np = await req(18134, "GET", `/api/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 502, JSON.stringify(np.json));
  assert.equal(np.json.ok, false);
  assert.equal(np.json.error, "core unreachable");
  assert.ok(!JSON.stringify(np.json).includes("Demo Track"), "no demo track served");
  assert.ok(!JSON.stringify(np.json).includes("local-demo"), "no demo label served");
  const q = await req(18134, "GET", `/api/queue?chat_id=${CHAT}`);
  assert.equal(q.status, 502, JSON.stringify(q.json));
  assert.equal(q.json.ok, false);
  assert.equal(q.json.error, "core unreachable");
  assert.ok(!JSON.stringify(q.json).includes("local-demo"), "no demo queue served");
});

test("CORE_URL unset: demo state is still served, labelled local-demo (dev only)", async () => {
  const np = await req(18130, "GET", `/api/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 200, JSON.stringify(np.json));
  assert.equal(np.json.data.now_playing.source, "local-demo");
  const q = await req(18130, "GET", `/api/queue?chat_id=${CHAT}`);
  assert.equal(q.json.data.source, "local-demo");
});
