// vc-ts — TypeScript VC engine + Lemonic + inline (Node, stdlib only).
//
// Ported behaviour (re-implemented, not copied) from:
//   TGVCBot  apps/tgvcbot/src/{bot,queue,tgcalls}.ts, handlers/{play,queue,
//            radio,youtube,jiosaavn}.ts, providers/{youtube,jiosaavn}.ts
//   remix    src/bot/{queues,session}.ts, handlers/{stream,controls,panel,
//            playlist,search,lyrics}.ts, streamer/{audio,youtube,stream}.ts
//   lemonic  modules/{lemonic,spotify,songlink}.ts, handlers/inline_query.ts
//
// IPC contract: contracts/ipc.json. Every response is
//   { ok:true, data:{...} }  or  { ok:false, error:{code, message} }
// Auth: X-Parlay-Token when PARLAY_TOKEN is set; GET /healthz is public.
// Voice transport is real — see src/voicecall.js (ffmpeg Opus/RTP
// pipeline + ntgcalls local offer + join delegated to the core-cpp
// TDLib backend over POST {CORE_URL}/v1/join).
// Playback state (per-chat queue, position, auto-advance) is real and
// persisted to data/state.json (atomic write); restored rooms come back
// paused. In this sandbox the live join is blocked (no MTProto session,
// UDP sendto EPERM); voicecall reports the exact reason per chat.
// Phase 4 additions: real JioSaavn durations (resolvers.js), lyrics
// (POST /v1/lyrics, LRCLib), live control panel (GET/POST /v1/panel,
// remix panel.ts button ids), volume (POST /v1/volume, 0-200), loop,
// raw-stream/radio play (http(s) URL or radio:<url>).
// Phase 5 additions: contract loop/shuffle/autoplay/autoplay-skip
// (POST /v1/loop|shuffle|autoplay|autoplay-skip, vc-go/vc-rust
// semantics — loop is a 0-10 replay count, not a boolean); YouTube
// resolve no longer degrades to a placeholder (resolvers.js: android
// retry, then engine_unavailable).

"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");

const voicecall = require("./voicecall");
const R = require("./resolvers");
const { SharedStore } = require("./store");
const { roomToCanonical, roomFromCanonical } = require("./canonical");

// Shared durable store (A11): Mongo > Redis > JSON file. Null while
// unconfigured (file mode) or before startup; SharedStore.open() throws
// when a configured backend is unreachable (fatal — see store.js).
let sharedStore = null;

const PORT = Number(process.env.PORT) || 8002;
const TOKEN = process.env.PARLAY_TOKEN || "";
const DATA_DIR = process.env.PARLAY_DATA_DIR || path.join(__dirname, "..", "data");
const STATE_FILE = path.join(DATA_DIR, "state.json");

// ---------------------------------------------------------------- state ----
// Room: { chat_id, current: Track|null, queue: QueueItem[], paused,
//         position_sec (offset), started_at_ms, volume (0-200, remix
//         scale: 100 = 100%), loop (remaining replays of the current
//         track, 0-10, 0 = off — contract vc.loop, vc-go/vc-rust
//         semantics), shuffle (queue pop is random while on),
//         autoplay (pick from the last track's YouTube mix on drain),
//         history (played ids, autoplay exclusion, capped at 50),
//         autoplay_pending (a mix pick is in flight), muted_volume
//         (pre-mute volume) }
const rooms = new Map();

function positionSec(room) {
  if (!room.current) return 0;
  if (room.paused) return room.position_sec;
  return room.position_sec + Math.floor((Date.now() - room.started_at_ms) / 1000);
}

function nowPlaying(room) {
  const vs = voicecall.voiceState(room.chat_id);
  const muted = room.volume === 0 && room.muted_volume !== null && room.muted_volume !== undefined;
  const np = {
    chat_id: room.chat_id,
    engine: "ts",
    track: room.current,
    paused: room.paused,
    position_sec: positionSec(room),
    queue: room.queue,
    volume: room.volume,
    loop: room.loop, // integer 0-10 (contract NowPlaying.loop)
    shuffle: Boolean(room.shuffle),
    autoplay: Boolean(room.autoplay),
    muted,
    voice: vs.voice,
    voice_joined: vs.joined,
    voice_reason: vs.reason,
    // IPC-1 (§29): the fields the core's merged 5-row now-playing
    // card reads. is_muted mirrors the legacy `muted` field above;
    // speed is always 1.0 — vc-ts has no speed control.
    is_paused: room.paused,
    is_muted: muted,
    loop_count: room.loop,
    speed: 1.0,
    autoplay_enabled: Boolean(room.autoplay),
  };
  // Top-level stream_url mirrors the current track's (contract
  // NowPlaying.stream_url, A8 residual): readers need not dig into
  // track.stream_url. Omitted when the engine has not resolved one.
  if (room.current && room.current.stream_url) np.stream_url = room.current.stream_url;
  return np;
}

function getRoom(chatId) {
  let room = rooms.get(chatId);
  if (!room) {
    room = {
      chat_id: chatId,
      current: null,
      queue: [],
      paused: false,
      position_sec: 0,
      started_at_ms: Date.now(),
      volume: 100,
      loop: 0,
      shuffle: false,
      autoplay: false,
      history: [],
      autoplay_pending: false,
      muted_volume: null,
    };
    rooms.set(chatId, room);
  }
  return room;
}

// Record a track id in play history (autoplay exclusion), capped at 50
// (vc-go / TgMusicBot cap).
function notePlayed(room, id) {
  if (!id || room.history.includes(id)) return;
  room.history.push(id);
  if (room.history.length > 50) room.history.shift();
}

// Ids the autoplay picker must avoid: the last track, everything
// already queued, and the play history.
function autoplayExclude(room, last) {
  const s = new Set(room.history);
  if (last) s.add(last.id);
  for (const it of room.queue) s.add(it.track.id);
  return s;
}

// Pop the next queued track: index 0 normally, a random index while
// shuffle is on (vc-go popWaitingLocked / vc-rust advance). Re-indexes
// the remaining queue positions.
function popNext(room) {
  if (!room.queue.length) return null;
  let idx = 0;
  if (room.shuffle && room.queue.length > 1) {
    idx = Math.floor(Math.random() * room.queue.length);
  }
  const [item] = room.queue.splice(idx, 1);
  room.queue.forEach((it, i) => (it.position = i));
  return item;
}

function saveState() {
  if (sharedStore) {
    // Async (Node has no sync sockets): ops serialise through the
    // store queue in call order; shutdown flushes (see SIGTERM below).
    saveStateShared().catch((e) => console.error("[vc-ts] shared state save failed:", e.message));
    return;
  }
  try {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    const snapshot = [...rooms.values()]
      .filter((r) => r.current || r.queue.length)
      .map((r) => ({
        chat_id: r.chat_id,
        current: r.current,
        queue: r.queue,
        paused: true, // restored rooms come back paused
        position_sec: positionSec(r),
        volume: r.volume,
        loop: r.loop,
        shuffle: Boolean(r.shuffle),
        autoplay: Boolean(r.autoplay),
        history: [...(r.history || [])],
      }));
    const tmp = STATE_FILE + ".tmp." + process.pid;
    fs.writeFileSync(tmp, JSON.stringify({ rooms: snapshot }, null, 2));
    fs.renameSync(tmp, STATE_FILE);
  } catch (e) {
    console.error("[vc-ts] state save failed:", e.message);
  }
}

// Canonical shared-store room document (A11): see src/canonical.js.
// roomToCanonical/roomFromCanonical live there so tests can import
// them without booting the server.
async function saveStateShared() {
  const live = new Set();
  for (const r of rooms.values()) {
    const key = `vc:room:${r.chat_id}`;
    if (r.current || r.queue.length) {
      live.add(r.chat_id);
      await sharedStore.set(key, JSON.stringify(roomToCanonical(r, positionSec(r))));
    } else {
      await sharedStore.del(key);
    }
  }
  for (const k of await sharedStore.keys("vc:room:")) {
    const id = Number(k.slice("vc:room:".length));
    if (!live.has(id)) await sharedStore.del(k);
  }
}

async function loadState() {
  if (sharedStore) {
    try {
      for (const key of await sharedStore.keys("vc:room:")) {
        const text = await sharedStore.get(key);
        if (!text) continue;
        const s = JSON.parse(text);
        if (s && s.chat_id) rooms.set(s.chat_id, roomFromCanonical(s));
      }
      console.log(`[vc-ts] restored ${rooms.size} room(s) from shared store (${sharedStore.describe()}) (paused)`);
    } catch (e) {
      console.error("[vc-ts] shared state load failed:", e.message);
    }
    return;
  }
  try {
    if (!fs.existsSync(STATE_FILE)) return;
    const data = JSON.parse(fs.readFileSync(STATE_FILE, "utf8"));
    for (const s of data.rooms || []) {
      rooms.set(s.chat_id, {
        chat_id: s.chat_id,
        current: s.current || null,
        queue: s.queue || [],
        paused: true,
        position_sec: s.position_sec || 0,
        started_at_ms: Date.now(),
        volume: s.volume || 100,
        // Old state files stored loop as a boolean; Number(true) = 1
        // replay, Number(false) = 0 — a safe migration.
        loop: Number(s.loop) || 0,
        shuffle: Boolean(s.shuffle),
        autoplay: Boolean(s.autoplay),
        history: Array.isArray(s.history) ? s.history : [],
        autoplay_pending: false,
        muted_volume: null,
      });
    }
    console.log(`[vc-ts] restored ${rooms.size} room(s) from ${STATE_FILE} (paused)`);
  } catch (e) {
    console.error("[vc-ts] state load failed:", e.message);
  }
}

// 1 s ticker: wall-clock position + auto-advance when a track ends.
// Loop (contract count 0-10, vc-go/vc-rust semantics) replays the
// current track, decrementing the count, instead of advancing. Radio
// tracks have duration_sec 0 and never advance; unknown-duration
// tracks never advance either. When the queue drains with autoplay on,
// the room goes idle at once and a YouTube mix pick is fetched
// asynchronously (pickAutoplay); the pick only starts if the room is
// still idle with autoplay on — a /play or /stop in the meantime wins.
async function pickAutoplay(room, last) {
  try {
    const pick = await R.autoplayPick(last, autoplayExclude(room, last));
    if (pick && room.autoplay && !room.current && room.queue.length === 0) {
      startTrack(room, pick);
      console.log(`[vc-ts] chat ${room.chat_id}: autoplay picked ${pick.title}`);
    } else if (!pick) {
      console.log(`[vc-ts] chat ${room.chat_id}: autoplay found no pick, room stays idle`);
    }
  } catch (e) {
    console.log(`[vc-ts] chat ${room.chat_id}: autoplay pick failed: ${e.message}`);
  } finally {
    room.autoplay_pending = false;
    saveState();
  }
}

setInterval(() => {
  let changed = false;
  for (const room of rooms.values()) {
    if (!room.current || room.paused) continue;
    const dur = Number(room.current.duration_sec) || 0;
    if (dur > 0 && positionSec(room) >= dur) {
      if (room.loop > 0) {
        room.loop -= 1;
        room.position_sec = 0;
        room.started_at_ms = Date.now();
        changed = true;
        console.log(`[vc-ts] chat ${room.chat_id}: loop (${room.loop} left) — restarted ${room.current.title}`);
        continue;
      }
      const next = popNext(room);
      if (next) {
        startTrack(room, next.track);
        changed = true;
        console.log(`[vc-ts] chat ${room.chat_id}: auto-advanced to ${room.current.title}`);
        continue;
      }
      if (room.autoplay && !room.autoplay_pending) {
        const last = room.current;
        room.current = null;
        room.position_sec = 0;
        room.autoplay_pending = true;
        changed = true;
        pickAutoplay(room, last); // async, fire-and-forget; clears pending
        continue;
      }
      room.current = null;
      room.position_sec = 0;
      changed = true;
      console.log(`[vc-ts] chat ${room.chat_id}: auto-advanced to (end of queue)`);
    }
  }
  if (changed) saveState();
}, 1000);

// ------------------------------------------------------------- resolving ---
// Play input: a YouTube URL, a free-text query (YouTube), a
// "jiosaavn:<query>" / "jsvn:<id>" prefixed query for the JioSaavn lane,
// or a raw http(s) audio URL / "radio:<url>" for the radio lane
// (TGVCBot /radio, openradiobot raw streams): the URL itself is the
// track, no yt-dlp, duration 0 so it plays until stopped/skipped.

const RAW_URL_RE = /^https?:\/\//i;

// Phase 1 telemetry: timed wrapper around resolvePlayQueryUntimed.
// One [telemetry] t_resolve_us=… line per resolve attempt (success
// or failure), same format as vc-go / vc-rust / the core worker.
// The source label comes from the returned track on success; on
// failure (no track) it comes from playQuerySource(), which mirrors
// resolvePlayQueryUntimed's dispatch order exactly.
async function resolvePlayQuery(query) {
  const startNs = R.telemetryNowNs();
  let source = "youtube";
  try {
    const out = await resolvePlayQueryUntimed(query);
    if (out && out.track && out.track.source) source = out.track.source;
    return out;
  } catch (e) {
    source = playQuerySource(query);
    throw e;
  } finally {
    R.telemetryLogResolve(source, startNs);
  }
}

// Mirror of resolvePlayQueryUntimed's dispatch, used only to label
// the source of a failed resolve. Keep in sync with it.
function playQuerySource(query) {
  const q = String(query);
  if (/^radio:/i.test(q)) return "radio";
  if (RAW_URL_RE.test(q.trim()) && !R.extractYouTubeId(q)) return "radio";
  if (/^jiosaavn:/i.test(q) || /^jsvn:/i.test(q)) return "jiosaavn";
  return "youtube";
}

async function resolvePlayQueryUntimed(query) {
  const q = String(query);
  if (/^radio:/i.test(q)) {
    const url = q.replace(/^radio:/i, "").trim();
    if (!RAW_URL_RE.test(url)) throw Object.assign(new Error(`radio: expects a stream URL, got ${q.slice(0, 60)}`), { code: "bad_request" });
    return { track: R.radioTrackFromUrl(url) };
  }
  if (RAW_URL_RE.test(q.trim()) && !R.extractYouTubeId(q)) {
    // Non-YouTube http(s) URL: a direct audio/radio stream.
    return { track: R.radioTrackFromUrl(q.trim()) };
  }
  if (/^jiosaavn:/i.test(q)) {
    const term = q.replace(/^jiosaavn:/i, "").trim();
    const { results } = await R.searchJioSaavn(term, 1);
    if (!results.length) throw new R.EngineUnavailable("jiosaavn", `JioSaavn found nothing for "${term}"`);
    return { track: results[0] };
  }
  if (/^jsvn:/i.test(q)) {
    const id = q.replace(/^jsvn:/i, "").trim();
    return { track: await R.resolveJioSaavnSong(id) };
  }
  return R.resolveYouTube(q);
}

function enqueue(room, track, requestedBy) {
  room.queue.push({ position: room.queue.length, requested_by: requestedBy, track });
}

function startTrack(room, track) {
  room.current = track;
  room.paused = false;
  room.position_sec = 0;
  room.started_at_ms = Date.now();
  notePlayed(room, track.id);
  // Real voice transport, fire-and-forget: playStream() runs the
  // Opus/RTP pipeline and then attempts the join; it never throws and
  // never blocks the HTTP response. The outcome lands in the per-chat
  // voice state surfaced by nowPlaying()/healthz.
  if (track.url || track.stream_url) {
    voicecall.playStream(room.chat_id, track.stream_url || track.url)
      .catch((e) => console.error(`[vc-ts] voice pipeline error chat ${room.chat_id}:`, e && e.message || e));
  }
}

// ------------------------------------------------------- control panel -----
// remix src/bot/handlers/panel.ts + helpers/volume.ts, as JSON instead of
// a Telegram inline keyboard. Button callback_data values are identical
// (panel_<action>) so the core can render the same keyboard.
// Volume scale is remix's 0-200 (%): volinc/voldec step 10, matching
// remix's ±1000 steps on its 0-20000 scale (10000 = 100%). First
// volinc/voldec from an unset volume behaves like remix (100%/50%);
// here volume always has a value (default 100), so plain ±10 applies.
const PANEL_BUTTONS = [
  { label: "🔀", action: "shuffle", callback_data: "panel_shuffle" },
  { label: "⏩", action: "skip", callback_data: "panel_skip" },
  { label: "⏸", action: "pause", callback_data: "panel_pause" },
  { label: "▶️", action: "resume", callback_data: "panel_resume" },
  { label: "🔇", action: "mute", callback_data: "panel_mute" },
  { label: "🔈", action: "unmute", callback_data: "panel_unmute" },
  { label: "🔉", action: "voldec", callback_data: "panel_voldec" },
  { label: "🔊", action: "volinc", callback_data: "panel_volinc" },
  { label: "🔁", action: "loop", callback_data: "panel_loop" },
  { label: "🔄", action: "update", callback_data: "panel_update" },
];

function panelState(room) {
  const now = room.current;
  const next = room.queue[0] ? room.queue[0].track : null;
  const nowEmoji = room.loop ? "🔁" : "🎵";
  return {
    chat_id: room.chat_id,
    text:
      `${nowEmoji} | ${now ? now.title : "Nothing streaming now"}\n` +
      `➡️ | ${next ? next.title : "Nothing streaming next"}`,
    now: now ? { title: now.title, url: now.url || now.stream_url || "", artist: now.artist } : null,
    next: next ? { title: next.title, url: next.url || next.stream_url || "", artist: next.artist } : null,
    paused: room.paused,
    is_muted: room.volume === 0 && room.muted_volume !== null && room.muted_volume !== undefined,
    position_sec: positionSec(room),
    duration_sec: now ? Number(now.duration_sec) || 0 : 0,
    volume: room.volume,
    loop: Boolean(room.loop),
    queue_count: room.queue.length,
    buttons: PANEL_BUTTONS,
  };
}

function panelAction(room, action) {
  // Returns { ok, message, state } — message mirrors remix's
  // answerCallbackQuery texts ("Paused.", "Skipped.", ...).
  const a = String(action || "").toLowerCase();
  switch (a) {
    case "update":
      return { ok: true, message: "Updated.", state: panelState(room) };
    case "shuffle": {
      // §27 typo correction (remix "No enough items" -> "Not enough").
      if (room.queue.length < 1) return { ok: false, message: "Not enough items to shuffle." };
      for (let i = room.queue.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [room.queue[i], room.queue[j]] = [room.queue[j], room.queue[i]];
      }
      room.queue.forEach((it, i) => (it.position = i));
      // Panel shuffle is also the contract shuffle mode: later advances
      // pop a random queue index while it is on (/v1/shuffle off clears it).
      room.shuffle = true;
      saveState();
      return { ok: true, message: "Shuffling...", state: panelState(room) };
    }
    case "skip": {
      if (!room.current) return { ok: false, message: "Not streaming." };
      room.loop = 0; // /skip resets the loop count (vc-go/vc-rust)
      const next = popNext(room);
      if (next) startTrack(room, next.track);
      else { room.current = null; room.position_sec = 0; }
      saveState();
      return { ok: true, message: "Skipped.", state: panelState(room) };
    }
    case "pause":
      if (!room.current) return { ok: false, message: "Not streaming." };
      if (!room.paused) { room.position_sec = positionSec(room); room.paused = true; }
      saveState();
      return { ok: true, message: "Paused.", state: panelState(room) };
    case "resume":
      if (!room.current) return { ok: false, message: "Not streaming." };
      if (room.paused) { room.paused = false; room.started_at_ms = Date.now(); }
      saveState();
      return { ok: true, message: "Resumed.", state: panelState(room) };
    case "mute":
      if (room.volume !== 0) { room.muted_volume = room.volume; room.volume = 0; }
      saveState();
      return { ok: true, message: "Muted.", state: panelState(room) };
    case "unmute":
      room.volume = room.muted_volume ?? 100;
      room.muted_volume = null;
      saveState();
      return { ok: true, message: "Unmuted.", state: panelState(room) };
    case "volinc":
      room.volume = Math.min(200, (room.volume || 0) + 10);
      room.muted_volume = null;
      saveState();
      return { ok: true, message: `Volume ${room.volume}%.`, state: panelState(room) };
    case "voldec":
      room.volume = Math.max(0, (room.volume || 0) - 10);
      saveState();
      return { ok: true, message: `Volume ${room.volume}%.`, state: panelState(room) };
    case "loop":
      // Panel loop is a boolean toggle (remix); the count model caps
      // at 10, so "on" means 10 replays — the closest the contract
      // (vc.loop, 0-10) allows to remix's infinite loop.
      room.loop = room.loop > 0 ? 0 : 10;
      saveState();
      return { ok: true, message: room.loop > 0 ? "Loop on." : "Loop off.", state: panelState(room) };
    default:
      return { ok: false, message: `unknown panel action "${action}" (shuffle|skip|pause|resume|mute|unmute|volinc|voldec|loop|update)` };
  }
}

// ----------------------------------------------------------------- http ----
function send(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(payload);
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

async function handleSearch(body) {
  const query = String(body.query || "").trim();
  if (!query) throw Object.assign(new Error("query is required"), { code: "bad_request" });
  const source = String(body.source || "all").toLowerCase();
  const limit = Math.max(1, Math.min(10, Number(body.limit) || 5));
  const valid = ["youtube", "jiosaavn", "lemonic", "all"];
  if (!valid.includes(source)) {
    throw Object.assign(new Error(`source must be one of ${valid.join("|")}`), { code: "bad_request" });
  }
  const results = [];
  const sources = {};
  const run = async (name, fn) => {
    try {
      const r = await fn();
      if (r.error && !r.results.length) {
        sources[name] = "error";
        sources[`${name}_error`] = r.error;
        return;
      }
      results.push(...r.results);
      sources[name] = "ok";
    } catch (e) {
      sources[name] = e.code || "error";
      sources[`${name}_error`] = e.message;
    }
  };
  if (source === "youtube" || source === "all") {
    await run("youtube", () => R.searchYouTube(query, limit));
  }
  if (source === "jiosaavn" || source === "all") {
    await run("jiosaavn", () => R.searchJioSaavn(query, limit));
  }
  if (source === "lemonic" || source === "all") {
    await run("lemonic", () => R.searchLemonic(query, limit));
  }
  if (!results.length && Object.values(sources).every((v) => v !== "ok")) {
    const detail = Object.entries(sources)
      .filter(([k]) => k.endsWith("_error"))
      .map(([, v]) => v)
      .join("; ");
    throw Object.assign(
      new Error(`all requested sources failed: ${detail || "no results"}`),
      { code: "engine_unavailable" }
    );
  }
  return { results: results.slice(0, limit * 3), sources };
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://localhost");
    const p = url.pathname;

    if (req.method === "GET" && p === "/healthz") {
      return send(res, 200, {
        ok: true,
        service: "vc-ts",
        engine: "ts",
        yt_dlp: R.ytDlpAvailable(),
        yt_dlp_path: R.ytDlpPath(),
        jiosaavn: "jsvn-tgvc.vercel.app",
        lemonic_configured: R.lemonicConfigured(),
        lemonic_mode: R.lemonicMode(),
        voice: voicecall.status(),
        rooms: rooms.size,
        store: sharedStore ? sharedStore.backend : "file",
      });
    }

    // Auth: everything except /healthz needs X-Parlay-Token when set.
    if (TOKEN && req.headers["x-parlay-token"] !== TOKEN) {
      return fail(res, 401, "unauthorized", "missing or wrong X-Parlay-Token");
    }

    if (req.method === "GET" && p === "/v1/now-playing") {
      const chatId = Number(url.searchParams.get("chat_id"));
      if (!Number.isInteger(chatId)) return fail(res, 400, "bad_request", "chat_id query param is required");
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (req.method === "GET" && p === "/v1/progress") {
      // IPC-2 (§29): progress snapshot for the core's no-op
      // progress-bar button refresh and the remix /menu panel.
      // Same 404 rule as /v1/now-playing: no room, or a room with
      // nothing playing. (A missing/non-integer chat_id is a 400 —
      // Number(null) is 0, so the raw param is checked first.)
      const raw = url.searchParams.get("chat_id");
      const chatId = raw === null ? NaN : Number(raw);
      if (!Number.isInteger(chatId)) return fail(res, 400, "bad_request", "chat_id query param is required");
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      const next = room.queue[0] ? room.queue[0].track : null;
      return ok(res, {
        position_sec: positionSec(room),
        duration_sec: Number(room.current.duration_sec) || 0,
        is_paused: room.paused,
        is_muted: room.volume === 0 && room.muted_volume !== null && room.muted_volume !== undefined,
        loop_enabled: room.loop > 0,
        now: room.current,
        next,
      });
    }

    if (req.method === "GET" && p === "/v1/panel") {
      const chatId = Number(url.searchParams.get("chat_id"));
      if (!Number.isInteger(chatId)) return fail(res, 400, "bad_request", "chat_id query param is required");
      const room = rooms.get(chatId);
      if (!room) return fail(res, 404, "not_found", "no playback state for this chat");
      return ok(res, { panel: panelState(room) });
    }

    if (req.method !== "POST") return fail(res, 404, "not_found", "unknown route");

    const body = await readBody(req);

    if (p === "/v1/play") {
      const chatId = requireChatId(body);
      if (!body.query || typeof body.query !== "string") {
        return fail(res, 400, "bad_request", "query is required");
      }
      const requestedBy = Number(body.requested_by) || 0;
      const room = getRoom(chatId);
      // Throws EngineUnavailable (HTTP 502) when the resolve fails —
      // no degraded placeholder track is ever queued (Phase 5, A9).
      const { track } = await resolvePlayQuery(body.query);
      if (!room.current) startTrack(room, track);
      else enqueue(room, track, requestedBy);
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/pause" || p === "/v1/resume") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      if (p === "/v1/pause" && !room.paused) {
        room.position_sec = positionSec(room);
        room.paused = true;
      }
      if (p === "/v1/resume" && room.paused) {
        room.paused = false;
        room.started_at_ms = Date.now();
      }
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/skip") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      room.loop = 0; // /skip resets the loop count (vc-go/vc-rust)
      const next = popNext(room);
      if (next) startTrack(room, next.track);
      else { room.current = null; room.position_sec = 0; }
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/stop") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (room) {
        room.current = null;
        room.queue = [];
        room.paused = false;
        room.position_sec = 0;
        // vc-go Stop: loop and autoplay reset; shuffle is kept.
        room.loop = 0;
        room.autoplay = false;
        room.history = [];
        room.autoplay_pending = false;
        voicecall.leaveVoiceChat(chatId);
      }
      saveState();
      return ok(res, {});
    }

    if (p === "/v1/seek") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      const pos = Number(body.position_sec);
      if (!Number.isFinite(pos) || pos < 0) return fail(res, 400, "bad_request", "position_sec must be >= 0");
      room.position_sec = Math.floor(pos);
      room.started_at_ms = Date.now();
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/loop") {
      // Contract vc.loop (ParvatiMusic room.rs loop_n, vc-go SetLoop):
      // count 0-10, remaining replays of the current track, 0 = off.
      // The ticker decrements it per replay; /skip resets it.
      const chatId = requireChatId(body);
      const count = Number(body.count);
      if (!Number.isInteger(count) || count < 0 || count > 10) {
        return fail(res, 400, "bad_request", "count must be an integer 0-10");
      }
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      room.loop = count;
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/shuffle") {
      // Contract vc.shuffle: enabling shuffles the queue in place
      // (vc-rust shuffle_queue) and makes advances pop a random queue
      // index (vc-go popWaitingLocked); disabling keeps the order.
      const chatId = requireChatId(body);
      if (typeof body.enabled !== "boolean") {
        return fail(res, 400, "bad_request", "enabled must be a boolean");
      }
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      if (body.enabled) {
        for (let i = room.queue.length - 1; i > 0; i--) {
          const j = Math.floor(Math.random() * (i + 1));
          [room.queue[i], room.queue[j]] = [room.queue[j], room.queue[i]];
        }
        room.queue.forEach((it, i) => (it.position = i));
        room.shuffle = true;
      } else {
        room.shuffle = false;
      }
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/autoplay") {
      // Contract vc.autoplay: when on, the ticker picks a related
      // track from the last track's YouTube mix once the queue drains
      // (vc-go/vc-rust autoplay). Disabling clears the play history.
      const chatId = requireChatId(body);
      if (typeof body.enabled !== "boolean") {
        return fail(res, 400, "bad_request", "enabled must be a boolean");
      }
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      room.autoplay = body.enabled;
      if (!body.enabled) room.history = [];
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/autoplay-skip") {
      // Contract vc.autoplay_skip (ParvatiMusic /askip, vc-rust
      // handle_autoplay_skip): only when autoplay is on — pick a mix
      // track from the current track now, reset the loop count, and
      // force-play it. 400 when autoplay is off, 502 when no mix pick
      // is available (mix empty or YouTube gated).
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room) return fail(res, 404, "not_found", "no playback state for this chat");
      if (!room.autoplay) return fail(res, 400, "bad_request", "autoplay is off in this chat");
      if (!room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      const last = room.current;
      let pick;
      try {
        pick = await R.autoplayPick(last, autoplayExclude(room, last));
      } catch (e) {
        if (e.code === "engine_unavailable") return fail(res, 502, "engine_unavailable", e.message);
        throw e;
      }
      if (!pick) {
        return fail(res, 502, "engine_unavailable", "no autoplay recommendation for the current track");
      }
      room.loop = 0;
      startTrack(room, pick);
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/queue") {
      const chatId = requireChatId(body);
      const room = getRoom(chatId);
      const action = String(body.action || "list").toLowerCase();
      if (action === "list") return ok(res, { queue: room.queue });
      if (action === "clear") {
        room.queue = [];
        saveState();
        return ok(res, { queue: [] });
      }
      if (action === "remove") {
        const pos = Number(body.position);
        if (!Number.isInteger(pos) || pos < 0 || pos >= room.queue.length) {
          return fail(res, 400, "bad_request", "position out of range");
        }
        room.queue.splice(pos, 1);
        room.queue.forEach((it, i) => (it.position = i));
        saveState();
        return ok(res, { queue: room.queue });
      }
      if (action === "add") {
        let track = body.track;
        if (!track && body.query) {
          const r = await resolvePlayQuery(body.query);
          track = r.track;
        }
        if (!track || !track.id || !track.title || !track.source) {
          return fail(res, 400, "bad_request", "track (with id, title, source) or query is required");
        }
        enqueue(room, track, Number(body.requested_by) || 0);
        saveState();
        return ok(res, { queue: room.queue });
      }
      return fail(res, 400, "bad_request", "action must be add|remove|clear|list");
    }

    if (p === "/v1/panel") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room) return fail(res, 404, "not_found", "no playback state for this chat");
      const result = panelAction(room, body.action);
      if (!result.ok) return fail(res, 400, "bad_request", result.message);
      return ok(res, { message: result.message, panel: result.state, now_playing: nowPlaying(room) });
    }

    if (p === "/v1/volume") {
      const chatId = requireChatId(body);
      const room = rooms.get(chatId);
      if (!room || !room.current) return fail(res, 404, "not_found", "no playback state for this chat");
      const vol = Number(body.volume);
      if (!Number.isFinite(vol) || vol < 0 || vol > 200) {
        return fail(res, 400, "bad_request", "volume must be 0-200 (remix scale: 100 = 100%)");
      }
      room.volume = Math.round(vol);
      room.muted_volume = null;
      saveState();
      return ok(res, { now_playing: nowPlaying(room) });
    }

    if (p === "/v1/lyrics") {
      // remix handlers/lyrics.ts: lyrics for the now-playing title.
      // Title/artist may be given directly; otherwise the room's
      // current track is used. Source: LRCLib (keyless).
      // Response is IPC-5 (§29): track_id/title/artist/source/
      // truncated + lyrics. track_id is the room's current track when
      // the lookup came from a chat_id, else null. truncated is false
      // — LRCLib lyrics are returned whole; the core chunks at 4096
      // chars for display (remix view) and 4000 for inline `l`.
      let title = String(body.title || "").trim();
      let artist = String(body.artist || "").trim();
      let trackId = null;
      if (!title && body.chat_id !== undefined && body.chat_id !== null) {
        const room = rooms.get(Number(body.chat_id));
        if (room && room.current) {
          title = room.current.title || "";
          artist = room.current.artist || "";
          trackId = room.current.id || null;
        }
      }
      if (!title) return fail(res, 400, "bad_request", "title is required (or chat_id with a track playing)");
      const found = await R.fetchLyrics(title, artist);
      if (!found) return fail(res, 404, "not_found", `no lyrics found for "${title}"`);
      return ok(res, {
        track_id: trackId,
        title: found.title,
        artist: found.artist,
        lyrics: found.lyrics,
        source: found.source,
        truncated: false,
      });
    }

    if (p === "/v1/search") {
      const data = await handleSearch(body);
      return ok(res, data);
    }

    if (p === "/v1/lemonic/search") {
      const query = String(body.query || "").trim();
      if (!query) return fail(res, 400, "bad_request", "query is required");
      const { results, via } = await R.searchLemonic(query, Number(body.limit) || 5);
      return ok(res, { results, via });
    }

    if (p === "/v1/lemonic/download") {
      const trackId = String(body.track_id || "").trim();
      if (!trackId) return fail(res, 400, "bad_request", "track_id is required");
      const { track, raw } = await R.lemonicRawTrack(trackId);
      const streamUrl = raw.stream_url || raw.download_url || raw.url || null;
      if (!streamUrl) {
        return fail(res, 502, "engine_unavailable",
          `Lemonic returned track ${trackId} but no stream/download URL; cannot download`);
      }
      // Fetch the audio to data/lemonic/<id>.mp3 so the core can send it.
      const dir = path.join(DATA_DIR, "lemonic");
      fs.mkdirSync(dir, { recursive: true });
      const filePath = path.join(dir, `${trackId.replace(/[^A-Za-z0-9_-]/g, "_")}.mp3`);
      let resp;
      try {
        resp = await fetch(streamUrl, { signal: AbortSignal.timeout(60000) });
      } catch (e) {
        return fail(res, 502, "download_failed", `Lemonic stream fetch failed: ${e.message.slice(0, 200)}`);
      }
      if (!resp.ok) {
        return fail(res, 502, "download_failed", `Lemonic stream fetch failed: HTTP ${resp.status}`);
      }
      const buf = Buffer.from(await resp.arrayBuffer());
      if (!buf.length) return fail(res, 502, "download_failed", "Lemonic stream was empty");
      const tmp = filePath + ".tmp";
      fs.writeFileSync(tmp, buf);
      fs.renameSync(tmp, filePath);
      return ok(res, { file_path: filePath, track, size_bytes: buf.length });
    }

    if (p === "/v1/inline") {
      // §13: navidrome modes (np/s/l/lib/del) + Music163bot help +
      // lemonic track results. lemonic's per-track documentPdf type is
      // NOT ported (a track is not a PDF) — the core renders lemonic
      // tracks as audio results from the Track below. The response
      // stays {results:[Track]} per the contract: the core knows the
      // query it sent, so it renders the mode-specific result kind
      // (cached audio for np, share message for s, lyrics article for
      // l — lyrics text via /v1/lyrics with the same chat_id) and the
      // static hint/help/no-results articles itself.
      const raw = String(body.query || "");
      const { mode, arg } = R.inlineMode(raw);
      const chatId = body.chat_id !== undefined && body.chat_id !== null
        ? Number(body.chat_id) : null;
      // np/s/l need a chat's now-playing. The core passes chat_id when
      // it knows which chat the inline user is playing in; without it
      // (or with nothing playing) the result is honestly empty — §13:
      // "Empty when nothing is playing".
      if (mode === "np" || mode === "s" || mode === "l") {
        const room = Number.isInteger(chatId) ? rooms.get(chatId) : null;
        if (!room || !room.current) return ok(res, { results: [] });
        return ok(res, { results: [room.current] });
      }
      if (mode === "lib" || mode === "del") {
        // No library backend in vc-ts: library search/delete need the
        // Navidrome library, which this service does not host (see
        // phase9/port-engines-gaps.md). Empty results, honestly.
        return ok(res, { results: [] });
      }
      if (mode === "hint" || mode === "help" || (mode === "search" && !arg)) {
        // Hint button / help articles / "Please enter a keyword" are
        // static inventory text the core renders from the query it
        // already has; the engine returns no tracks for them.
        return ok(res, { results: [] });
      }
      // Inline search (lemonic handlers/inline_query.ts): YouTube
      // first, Lemonic appended; JioSaavn skipped (no inline use).
      // Lemonic is always attempted — its unauthenticated path needs
      // no key — and skipped only if the API itself fails.
      const data = await handleSearch({ query: arg, source: "youtube", limit: 5 });
      try {
        const lem = await R.searchLemonic(arg, 5);
        data.results.push(...lem.results);
      } catch (e) {
        console.log("[vc-ts] inline: lemonic skipped:", e.message);
      }
      return ok(res, { results: data.results.slice(0, 10) });
    }

    return fail(res, 404, "not_found", "unknown route");
  } catch (e) {
    if (e.code === "engine_unavailable") return fail(res, 502, "engine_unavailable", e.message);
    if (e.code === "bad_request") return fail(res, 400, "bad_request", e.message);
    console.error("[vc-ts] error:", e);
    return fail(res, 500, "internal", e.message || "internal error");
  }
});

// Startup: open the shared store first (fatal when a configured
// backend is unreachable), restore state, then listen.
(async () => {
  try {
    sharedStore = await SharedStore.open();
  } catch (e) {
    console.error(`[vc-ts] shared store init failed: ${e.message}`);
    process.exit(1);
  }
  await loadState();
  server.listen(PORT, () => {
    console.log(
      `[vc-ts] listening on :${PORT} (yt-dlp: ${R.ytDlpAvailable() ? R.ytDlpPath() : "missing"}, ` +
        `lemonic: ${R.lemonicConfigured() ? "configured" : "not configured"}, ` +
        `auth: ${TOKEN ? "token required" : "open (no PARLAY_TOKEN)"}, ` +
        `voice: ${voicecall.status().voice})`
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
