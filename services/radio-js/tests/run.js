// radio-js tests — Phase 4 (Worker C). Node stdlib only: node:test.
// Spawns a real vc-ts engine and checks the VC handoff end to end:
// radio start must land the live stream URL in the VC engine as a
// `radio` track, and radio stop must clear it only while the engine
// is still on that track.
//
// Run:  node --test tests/run.js
// Ports: 18112 vc-ts, 18118 radio (VC wired), 18119 radio (no VC).
// Data dirs under /tmp/p4-radio/.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const TOKEN = "p4-test-token";
const BASE = "/tmp/p4-radio";
const CHAT = -1004343;

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

function startService(dir, port, extraEnv) {
  const dataDir = path.join(BASE, `s${port}`, "data");
  fs.rmSync(path.join(BASE, `s${port}`), { recursive: true, force: true });
  fs.mkdirSync(dataDir, { recursive: true });
  const child = spawn("node", ["src/main.js"], {
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

let vcts, radio, radioNoVc;

test.before(async () => {
  vcts = startService("vc-ts", 18112, {});
  radio = startService("radio-js", 18118, { VC_TS_URL: "http://127.0.0.1:18112" });
  radioNoVc = startService("radio-js", 18119, {});
  await waitHealth(18112);
  await waitHealth(18118);
  await waitHealth(18119);
});

test.after(() => {
  for (const c of [vcts, radio, radioNoVc]) { try { c.kill("SIGTERM"); } catch { /* dead */ } }
});

test("healthz reports the wired VC engine; auth enforced", async () => {
  const h = await req(18118, "GET", "/healthz", undefined, null);
  assert.equal(h.status, 200);
  assert.equal(h.json.vc_ts, "http://127.0.0.1:18112");
  assert.equal(h.json.vc_go, null);
  const hn = await req(18119, "GET", "/healthz", undefined, null);
  assert.equal(hn.json.vc_ts, null);
  const bad = await req(18118, "GET", "/v1/radio/stations", undefined, null);
  assert.equal(bad.status, 401);
});

test("stations list still works (6 curated stations)", async () => {
  const r = await req(18118, "GET", "/v1/radio/stations");
  assert.equal(r.status, 200);
  assert.equal(r.json.data.stations.length, 6);
  const rp = r.json.data.stations.find((s) => s.id === "radioparadise");
  assert.ok(rp);
  assert.ok(rp.stream_url.startsWith("https://"));
});

test("start hands the live stream to vc-ts as a radio track", async () => {
  const r = await req(18118, "POST", "/v1/radio/start", { chat_id: CHAT, station: "radioparadise" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.station, "radioparadise");
  assert.ok(r.json.data.vc_handoff, "vc_handoff present");
  assert.equal(r.json.data.vc_handoff.ok, true, JSON.stringify(r.json.data.vc_handoff));
  assert.equal(r.json.data.vc_handoff.engine, "ts");
  // The VC engine itself must now be playing the stream.
  const np = await req(18112, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 200, JSON.stringify(np.json));
  const t = np.json.data.now_playing.track;
  assert.equal(t.source, "radio");
  assert.equal(t.stream_url, "https://stream.radioparadise.com/aac-320");
  assert.equal(t.duration_sec, 0);
  // Radio's own status agrees.
  const st = await req(18118, "GET", `/v1/radio/status?chat_id=${CHAT}`);
  assert.equal(st.json.data.vc_handoff.ok, true);
  assert.equal(st.json.data.stream_url, "https://stream.radioparadise.com/aac-320");
});

test("stop clears the VC engine room while it is on the radio track", async () => {
  const r = await req(18118, "POST", "/v1/radio/stop", { chat_id: CHAT });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.vc_cleared.cleared, true);
  assert.equal(r.json.data.vc_cleared.engine, "ts");
  const np = await req(18112, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 404, "vc-ts room must be stopped");
});

test("stop does NOT clear the VC engine once it moved on to real music", async () => {
  await req(18118, "POST", "/v1/radio/start", { chat_id: CHAT, station: "groovesalad" });
  // User plays a song on the same engine, then skips to it: the radio
  // track is no longer current (play alone would only queue it).
  const play = await req(18112, "POST", "/v1/play", { chat_id: CHAT, query: "jiosaavn:kesariya" });
  assert.equal(play.status, 200, JSON.stringify(play.json));
  const skip = await req(18112, "POST", "/v1/skip", { chat_id: CHAT });
  assert.equal(skip.json.data.now_playing.track.id, "jsvn:rjkrTnma");
  const r = await req(18118, "POST", "/v1/radio/stop", { chat_id: CHAT });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.vc_cleared.cleared, false);
  assert.ok(r.json.data.vc_cleared.reason.includes("no longer on the radio track"));
  // …and the song is still playing.
  const np = await req(18112, "GET", `/v1/now-playing?chat_id=${CHAT}`);
  assert.equal(np.status, 200);
  assert.equal(np.json.data.now_playing.track.id, "jsvn:rjkrTnma");
  await req(18112, "POST", "/v1/stop", { chat_id: CHAT });
});

test("no VC configured: radio still starts, handoff honestly reported", async () => {
  const r = await req(18119, "POST", "/v1/radio/start", { chat_id: CHAT, station: "fip" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  assert.equal(r.json.data.vc_handoff.ok, false);
  assert.ok(r.json.data.vc_handoff.error.includes("no VC engine configured"));
  const st = await req(18119, "GET", `/v1/radio/status?chat_id=${CHAT}`);
  assert.equal(st.json.data.vc_handoff.ok, false);
  const stop = await req(18119, "POST", "/v1/radio/stop", { chat_id: CHAT });
  assert.equal(stop.status, 200);
  assert.equal(stop.json.data.vc_cleared.cleared, false);
});

test("errors: unknown station 400, stop with no radio 404, /v1/radio action route", async () => {
  const bad = await req(18118, "POST", "/v1/radio/start", { chat_id: CHAT, station: "nope" });
  assert.equal(bad.status, 400);
  const stop = await req(18118, "POST", "/v1/radio/stop", { chat_id: CHAT });
  assert.equal(stop.status, 404);
  const viaAction = await req(18118, "POST", "/v1/radio", { action: "start", chat_id: CHAT, station: "lush" });
  assert.equal(viaAction.status, 200, JSON.stringify(viaAction.json));
  assert.equal(viaAction.json.data.vc_handoff.ok, true);
  const stop2 = await req(18118, "POST", "/v1/radio", { action: "stop", chat_id: CHAT });
  assert.equal(stop2.status, 200);
  assert.equal(stop2.json.data.vc_cleared.cleared, true);
});

// ------------------------------------------------ Phase 9: IPC-1/3, §21 --
test("status carries the /manage card fields; now-playing carries IPC-1 + IPC-3", async () => {
  const before = Date.now();
  const r = await req(18119, "POST", "/v1/radio/start", { chat_id: CHAT, station: "fip" });
  assert.equal(r.status, 200, JSON.stringify(r.json));
  // The start response's now_playing carries the card fields too.
  const np0 = r.json.data.now_playing;
  assert.equal(np0.is_paused, false);
  assert.equal(np0.is_muted, false);
  assert.equal(np0.loop_count, 0);
  assert.equal(np0.speed, 1);
  assert.equal(np0.autoplay_enabled, false);
  assert.equal(np0.track.duration_str, "∞");
  assert.equal(np0.track.is_live, true);

  const st = await req(18119, "GET", `/v1/radio/status?chat_id=${CHAT}`);
  assert.equal(st.status, 200, JSON.stringify(st.json));
  const d = st.json.data;
  assert.equal(d.station, "fip");
  assert.equal(d.station_name, "FIP Paris");
  assert.ok(typeof d.started_at_ms === "number" && d.started_at_ms >= before);
  // No listener tracking exists (no web player) — always 0, honestly.
  assert.equal(d.listeners, 0);
  assert.equal(d.total_listeners, 0);
  assert.equal(d.loop_type, "none");
  assert.equal(d.autoplay_enabled, false);
  assert.equal(d.queue_length, 0);
  const np = d.now_playing;
  assert.equal(np.is_paused, false);
  assert.equal(np.is_muted, false);
  assert.equal(np.loop_count, 0);
  assert.equal(np.speed, 1);
  assert.equal(np.autoplay_enabled, false);
  assert.equal(np.track.duration_str, "∞");
  assert.equal(np.track.is_live, true);
  assert.equal(np.track.source, "radio");
  const stop = await req(18119, "POST", "/v1/radio/stop", { chat_id: CHAT });
  assert.equal(stop.status, 200);
});
