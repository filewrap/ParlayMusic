// radio-js — internet radio as a live source (Node, stdlib only).
//
// Ported behaviour (re-implemented, not copied) from Yonle/openradiobot
// (index.js, single file): a chat owns a radio; the radio plays a queue
// of streams; listeners attach to the live output. Differences: Parlay
// has no Telegram bot inside this service (the core owns Telegram), so
// this service is pure IPC, and it ships a curated station list —
// openradiobot itself has none, its queue is user-supplied.
//
// IPC contract: contracts/ipc.json. Envelopes {ok:true,data} /
// {ok:false,error:{code,message}}; X-Parlay-Token when PARLAY_TOKEN is
// set; GET /healthz is public.
//
// Stream reachability: when ffmpeg/ffprobe is present the stream URL is
// probed (ffprobe, 10 s timeout) at startup and on each start; the
// result is reported as reachable:true/false and never faked. If the
// stream is unreachable the station still starts in "playlist fallback"
// mode: a rotating playlist from src/stations.js keeps now-playing state
// alive, exactly like openradiobot's queue loop keeps playing.
//
// VC handoff (Phase 4): on start, the live stream URL is handed to a
// VC engine (VC_GO_URL then VC_TS_URL, same order as gui-js) via
// POST /v1/play, so the radio actually sounds in the group's voice
// chat — contract action vc.radio ("radio-js -> vc-*"). Before Phase 4
// the handoff did not exist: radio-js only kept its own state. A
// failed handoff is reported in vc_handoff and never fails the start.
// On stop, the engine's room is cleared only if it is still on the
// radio track, so a user's later music is never killed.

"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");
const { execFile } = require("child_process");

const { STATIONS, findStation } = require("./stations");
const { SharedStore } = require("./store");

// Shared durable store (A11): Mongo > Redis > JSON file. Null while
// unconfigured (file mode) or before startup; SharedStore.open() throws
// when a configured backend is unreachable (fatal — see store.js).
let sharedStore = null;

const PORT = Number(process.env.PORT) || 8008;
const TOKEN = process.env.PARLAY_TOKEN || "";
const DATA_DIR = process.env.PARLAY_DATA_DIR || path.join(__dirname, "..", "data");
const STATE_FILE = path.join(DATA_DIR, "state.json");
// VC engines the live stream is handed to on start (contract action
// vc.radio: "radio-js -> vc-*"). Unset = no handoff; the radio still
// starts and reports vc_handoff: null honestly.
const VC_GO_URL = (process.env.VC_GO_URL || "").replace(/\/$/, "");
const VC_TS_URL = (process.env.VC_TS_URL || "").replace(/\/$/, "");
const PROBE_TIMEOUT_MS = 10000;
const PLAYLIST_ROTATE_SEC = 180; // rotate fallback playlist entry

// ------------------------------------------------------- VC engine handoff --
// Hand the live stream URL to a VC engine as a live source, so the
// radio actually sounds in the group's voice chat (contract action
// vc.radio). vc-ts takes a raw http(s) URL as a `radio` track directly
// (duration 0, plays until stopped); vc-go takes it through yt-dlp's
// generic extractor (verified: direct stream, no duration, ~2s).
// Order matches the gui-js fallback order: vc-go first, vc-ts second.
// A handoff failure never fails the radio start — it is reported in
// vc_handoff and the radio keeps its own now-playing state.
async function vcRequest(base, method, path, body, timeoutMs = 15000) {
  if (!base) return null;
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(base + path, {
      method,
      headers: {
        "content-type": "application/json",
        ...(TOKEN ? { "x-parlay-token": TOKEN } : {}),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: ctrl.signal,
    });
    const text = await res.text();
    let json = null;
    try { json = JSON.parse(text); } catch { /* non-JSON */ }
    return { status: res.status, json };
  } catch (e) {
    return { status: 0, json: null, error: e.message };
  } finally {
    clearTimeout(t);
  }
}

async function handOffToVc(chatId, streamUrl) {
  const engines = [
    ["go", VC_GO_URL],
    ["ts", VC_TS_URL],
  ].filter(([, base]) => base);
  if (!engines.length) return { engine: null, ok: false, error: "no VC engine configured (set VC_GO_URL or VC_TS_URL)" };
  const errors = [];
  for (const [engine, base] of engines) {
    const r = await vcRequest(base, "POST", "/v1/play", { chat_id: chatId, query: streamUrl, requested_by: 0 });
    if (r && r.json && r.json.ok) {
      return { engine, ok: true, now_playing: r.json.data.now_playing || null };
    }
    errors.push(`${engine}: ${r && r.json && r.json.error ? r.json.error.message : r.error || `HTTP ${r.status}`}`);
  }
  return { engine: null, ok: false, error: `VC handoff failed on all engines: ${errors.join("; ")}` };
}

// On radio stop, clear the VC engine's room ONLY if it is still
// playing our radio track — never kill a user's later music.
async function clearVcIfRadio(chatId, handoff) {
  if (!handoff || !handoff.ok || !handoff.engine) return { cleared: false, reason: "no VC handoff to clear" };
  const base = handoff.engine === "go" ? VC_GO_URL : VC_TS_URL;
  const np = await vcRequest(base, "GET", `/v1/now-playing?chat_id=${chatId}`);
  const track = np && np.json && np.json.ok && np.json.data.now_playing && np.json.data.now_playing.track;
  if (!track || track.source !== "radio") {
    return { cleared: false, reason: "VC engine is no longer on the radio track" };
  }
  const stopped = await vcRequest(base, "POST", "/v1/stop", { chat_id: chatId });
  if (stopped && stopped.json && stopped.json.ok) return { cleared: true, engine: handoff.engine };
  return { cleared: false, reason: `VC stop failed: ${stopped && stopped.json && stopped.json.error ? stopped.json.error.message : "unreachable"}` };
}

// ------------------------------------------------------------------ probe --
let ffprobePath = null;
for (const p of ["/usr/bin/ffprobe", "/usr/local/bin/ffprobe"]) {
  try { fs.accessSync(p, fs.constants.X_OK); ffprobePath = p; break; } catch { /* next */ }
}

function probeStream(url) {
  return new Promise((resolve) => {
    if (!ffprobePath) return resolve({ reachable: false, probe: "ffprobe not installed" });
    execFile(
      ffprobePath,
      ["-v", "error", "-show_entries", "format=format_name", "-of", "json", url],
      { timeout: PROBE_TIMEOUT_MS },
      (err, stdout) => {
        if (err) return resolve({ reachable: false, probe: `ffprobe failed: ${err.message.slice(0, 200)}` });
        try {
          const fmt = JSON.parse(stdout).format || {};
          resolve({ reachable: true, format: fmt.format_name || null, probe: "ok" });
        } catch {
          resolve({ reachable: false, probe: "ffprobe output unparsable" });
        }
      }
    );
  });
}

// Probe all stations once at startup (in parallel) so /stations can
// report real reachability instead of guessing.
const stationProbe = new Map(); // id -> {reachable, format, probe}
async function probeAllStations() {
  await Promise.all(
    STATIONS.map(async (s) => {
      stationProbe.set(s.id, await probeStream(s.stream_url));
    })
  );
  console.log("[radio-js] startup probe:",
    STATIONS.map((s) => `${s.id}=${stationProbe.get(s.id).reachable ? "ok" : "unreachable"}`).join(" "));
}

// ------------------------------------------------------------------ state --
// Active radio per chat: { chat_id, station_id, stream_url, started_at_ms,
//   reachable, playlist_index, playlist_started_at_ms, listeners }
const radios = new Map();

function stateSnapshot() {
  return {
    radios: [...radios.values()].map((r) => ({
      chat_id: r.chat_id,
      station_id: r.station_id,
      started_at_ms: r.started_at_ms,
      playlist_index: r.playlist_index,
    })),
  };
}

function saveState() {
  if (sharedStore) {
    // Async (Node has no sync sockets): op serialises through the
    // store queue; shutdown flushes (see SIGTERM below).
    sharedStore
      .set("radio:state", JSON.stringify(stateSnapshot()))
      .catch((e) => console.error("[radio-js] shared state save failed:", e.message));
    return;
  }
  try {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    const tmp = STATE_FILE + ".tmp." + process.pid;
    fs.writeFileSync(tmp, JSON.stringify(stateSnapshot(), null, 2));
    fs.renameSync(tmp, STATE_FILE);
  } catch (e) {
    console.error("[radio-js] state save failed:", e.message);
  }
}

function restoreRadios(data) {
  for (const s of data.radios || []) {
    const station = findStation(s.station_id);
    if (!station) continue;
    radios.set(s.chat_id, {
      chat_id: s.chat_id,
      station_id: station.id,
      stream_url: station.stream_url,
      started_at_ms: s.started_at_ms || Date.now(),
      reachable: (stationProbe.get(station.id) || {}).reachable ?? null,
      playlist_index: s.playlist_index || 0,
      playlist_started_at_ms: Date.now(),
      listeners: 0,
      // Restored radios are NOT re-handed-off: the VC engine keeps
      // its own persisted room, and re-playing here could double
      // queue a stream. vc_handoff stays null until a fresh start.
      vc_handoff: null,
    });
  }
}

async function loadState() {
  if (sharedStore) {
    try {
      const text = await sharedStore.get("radio:state");
      if (text) {
        restoreRadios(JSON.parse(text));
        console.log(`[radio-js] restored ${radios.size} active radio(s) from shared store (${sharedStore.describe()})`);
      }
    } catch (e) {
      console.error("[radio-js] shared state load failed:", e.message);
    }
    return;
  }
  try {
    if (!fs.existsSync(STATE_FILE)) return;
    const data = JSON.parse(fs.readFileSync(STATE_FILE, "utf8"));
    restoreRadios(data);
    console.log(`[radio-js] restored ${radios.size} active radio(s) from ${STATE_FILE}`);
  } catch (e) {
    console.error("[radio-js] state load failed:", e.message);
  }
}

function currentPlaylistEntry(radio) {
  const station = findStation(radio.station_id);
  const list = station ? station.playlist : [];
  if (!list.length) return { title: "Live stream", artist: station ? station.name : "radio" };
  const idx = radio.playlist_index % list.length;
  return { ...list[idx], fallback_playlist: true };
}

function nowPlaying(radio) {
  const station = findStation(radio.station_id);
  const entry = currentPlaylistEntry(radio);
  return {
    chat_id: radio.chat_id,
    engine: "ts", // radio is fed into the active VC engine as a live source
    track: {
      id: `radio:${radio.station_id}:${radio.playlist_index}`,
      title: `${station ? station.name : radio.station_id} — ${entry.title}`,
      artist: entry.artist,
      duration_sec: 0,
      // IPC-3 (§29): a station stream is live and endless.
      duration_str: "∞",
      is_live: true,
      url: radio.stream_url,
      source: "radio",
    },
    paused: false,
    position_sec: Math.floor((Date.now() - radio.playlist_started_at_ms) / 1000) % PLAYLIST_ROTATE_SEC,
    queue: [],
    volume: 100,
    // IPC-1 (§29): a radio stream is never paused/muted/looped, has
    // no speed control and no autoplay — the constants are the honest
    // values for this engine, not placeholders for missing state.
    is_paused: false,
    is_muted: false,
    loop_count: 0,
    speed: 1.0,
    autoplay_enabled: false,
  };
}

// Rotate the fallback playlist entry every PLAYLIST_ROTATE_SEC.
setInterval(() => {
  let changed = false;
  for (const radio of radios.values()) {
    const station = findStation(radio.station_id);
    const n = station ? station.playlist.length : 0;
    if (n > 1 && Date.now() - radio.playlist_started_at_ms >= PLAYLIST_ROTATE_SEC * 1000) {
      radio.playlist_index = (radio.playlist_index + 1) % n;
      radio.playlist_started_at_ms = Date.now();
      changed = true;
    }
  }
  if (changed) saveState();
}, 15000);

// ------------------------------------------------------------------- http --
function send(res, status, body) {
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(JSON.stringify(body));
}

const ok = (res, data) => send(res, 200, { ok: true, data });
const fail = (res, status, code, message) =>
  send(res, status, { ok: false, error: { code, message } });

function readBody(req) {
  return new Promise((resolve, reject) => {
    let buf = "";
    req.on("data", (c) => {
      buf += c;
      if (buf.length > 1024 * 1024) reject(new Error("body too large"));
    });
    req.on("end", () => {
      if (!buf) return resolve({});
      try { resolve(JSON.parse(buf)); } catch { reject(new Error("invalid JSON body")); }
    });
    req.on("error", reject);
  });
}

function requireChatId(body) {
  const id = body.chat_id;
  if (id === undefined || id === null || !Number.isInteger(Number(id))) {
    const e = new Error("chat_id is required");
    e.code = "bad_request";
    throw e;
  }
  return Number(id);
}

async function startRadio(chatId, stationRef) {
  const station = findStation(stationRef);
  if (!station) {
    const e = new Error(`unknown station "${stationRef}"; see GET /v1/radio/stations`);
    e.code = "bad_request";
    throw e;
  }
  const probe = await probeStream(station.stream_url);
  stationProbe.set(station.id, probe);
  const radio = {
    chat_id: chatId,
    station_id: station.id,
    stream_url: station.stream_url,
    started_at_ms: Date.now(),
    reachable: probe.reachable,
    playlist_index: 0,
    playlist_started_at_ms: Date.now(),
    listeners: 0,
    vc_handoff: null,
  };
  radios.set(chatId, radio);
  // Hand the live stream to a VC engine so it sounds in the group call.
  // A failed handoff is reported, never fatal (see handOffToVc).
  radio.vc_handoff = await handOffToVc(chatId, station.stream_url);
  if (radio.vc_handoff.ok) {
    console.log(`[radio-js] chat ${chatId}: ${station.id} handed to vc-${radio.vc_handoff.engine}`);
  } else {
    console.log(`[radio-js] chat ${chatId}: ${station.id} VC handoff failed: ${radio.vc_handoff.error}`);
  }
  saveState();
  return {
    station: station.id,
    station_name: station.name,
    stream_url: station.stream_url,
    reachable: probe.reachable,
    probe: probe.probe,
    format: probe.format || null,
    fallback_playlist: !probe.reachable,
    vc_handoff: radio.vc_handoff,
    now_playing: nowPlaying(radio),
  };
}

async function stopRadio(chatId) {
  const radio = radios.get(chatId);
  if (!radio) return { existed: false, vc_cleared: null };
  radios.delete(chatId);
  saveState();
  const vcCleared = await clearVcIfRadio(chatId, radio.vc_handoff);
  return { existed: true, vc_cleared: vcCleared };
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://localhost");
    const p = url.pathname;

    if (req.method === "GET" && p === "/healthz") {
      return send(res, 200, {
        ok: true,
        service: "radio-js",
        ffprobe: ffprobePath || null,
        stations: STATIONS.length,
        active_radios: radios.size,
        store: sharedStore ? sharedStore.backend : "file",
        vc_go: VC_GO_URL || null,
        vc_ts: VC_TS_URL || null,
      });
    }

    if (TOKEN && req.headers["x-parlay-token"] !== TOKEN) {
      return fail(res, 401, "unauthorized", "missing or wrong X-Parlay-Token");
    }

    if (req.method === "GET" && p === "/v1/radio/stations") {
      return ok(res, {
        stations: STATIONS.map((s) => {
          const probe = stationProbe.get(s.id) || {};
          return {
            id: s.id,
            name: s.name,
            genre: s.genre,
            stream_url: s.stream_url,
            reachable: probe.reachable ?? null,
            format: probe.format || null,
          };
        }),
      });
    }

    if (req.method === "GET" && p === "/v1/radio/status") {
      const chatId = Number(url.searchParams.get("chat_id"));
      if (!Number.isInteger(chatId)) return fail(res, 400, "bad_request", "chat_id query param is required");
      const radio = radios.get(chatId);
      if (!radio) return fail(res, 404, "not_found", "no active radio for this chat");
      const station = findStation(radio.station_id);
      return ok(res, {
        now_playing: nowPlaying(radio),
        station: radio.station_id,
        // §21 /manage card fields, as far as this engine honestly
        // knows them: started_at_ms is real; listeners/total_listeners
        // are always 0 (no listener tracking exists — there is no web
        // player, the stream goes to the VC engine); the station
        // stream is endless, so loop_type is "none", autoplay is off
        // and the queue is empty.
        station_name: station ? station.name : radio.station_id,
        started_at_ms: radio.started_at_ms,
        listeners: 0,
        total_listeners: 0,
        loop_type: "none",
        autoplay_enabled: false,
        queue_length: 0,
        stream_url: radio.stream_url,
        reachable: radio.reachable,
        vc_handoff: radio.vc_handoff,
      });
    }

    if (req.method !== "POST") return fail(res, 404, "not_found", "unknown route");
    const body = await readBody(req);

    if (p === "/v1/radio/start") {
      const chatId = requireChatId(body);
      if (!body.station) return fail(res, 400, "bad_request", "station is required");
      return ok(res, await startRadio(chatId, body.station));
    }

    if (p === "/v1/radio/stop") {
      const chatId = requireChatId(body);
      const { existed, vc_cleared } = await stopRadio(chatId);
      if (!existed) return fail(res, 404, "not_found", "no active radio for this chat");
      return ok(res, { vc_cleared });
    }

    if (p === "/v1/radio") {
      const action = String(body.action || "").toLowerCase();
      const chatId = requireChatId(body);
      if (action === "start") {
        if (!body.station) return fail(res, 400, "bad_request", "station is required for action=start");
        return ok(res, await startRadio(chatId, body.station));
      }
      if (action === "stop") {
        const { existed, vc_cleared } = await stopRadio(chatId);
        if (!existed) return fail(res, 404, "not_found", "no active radio for this chat");
        return ok(res, { vc_cleared });
      }
      return fail(res, 400, "bad_request", "action must be start|stop");
    }

    return fail(res, 404, "not_found", "unknown route");
  } catch (e) {
    if (e.code === "bad_request") return fail(res, 400, "bad_request", e.message);
    console.error("[radio-js] error:", e);
    return fail(res, 500, "internal", e.message || "internal error");
  }
});

(async () => {
  await probeAllStations();
  try {
    sharedStore = await SharedStore.open();
  } catch (e) {
    console.error(`[radio-js] shared store init failed: ${e.message}`);
    process.exit(1);
  }
  await loadState();
  server.listen(PORT, () => {
    console.log(
      `[radio-js] listening on :${PORT} (ffprobe: ${ffprobePath || "missing"}, ` +
      `stations: ${STATIONS.length}, auth: ${TOKEN ? "token required" : "open (no PARLAY_TOKEN)"}, ` +
      `vc-go: ${VC_GO_URL || "unset"}, vc-ts: ${VC_TS_URL || "unset"})`
    );
  });
})();

async function shutdown() {
  saveState();
  if (sharedStore) await sharedStore.flush();
  process.exit(0);
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
