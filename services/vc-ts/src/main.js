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
// pipeline + ntgcalls local offer + Telethon voice-bridge join client).
// Playback state (per-chat queue, position, auto-advance) is real and
// persisted to data/state.json (atomic write); restored rooms come back
// paused. In this sandbox the live join is blocked (no MTProto session,
// UDP sendto EPERM); voicecall reports the exact reason per chat.
// Phase 4 additions: real JioSaavn durations (resolvers.js), lyrics
// (POST /v1/lyrics, LRCLib), live control panel (GET/POST /v1/panel,
// remix panel.ts button ids), volume (POST /v1/volume, 0-200), loop,
// raw-stream/radio play (http(s) URL or radio:<url>).

"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");

const voicecall = require("./voicecall");
const R = require("./resolvers");

const PORT = Number(process.env.PORT) || 8002;
const TOKEN = process.env.PARLAY_TOKEN || "";
const DATA_DIR = process.env.PARLAY_DATA_DIR || path.join(__dirname, "..", "data");
const STATE_FILE = path.join(DATA_DIR, "state.json");

// ---------------------------------------------------------------- state ----
// Room: { chat_id, current: Track|null, queue: QueueItem[], paused,
//         position_sec (offset), started_at_ms, volume (0-200, remix
//         scale: 100 = 100%), loop, muted_volume (pre-mute volume) }
const rooms = new Map();

function positionSec(room) {
  if (!room.current) return 0;
  if (room.paused) return room.position_sec;
  return room.position_sec + Math.floor((Date.now() - room.started_at_ms) / 1000);
}

function nowPlaying(room) {
  const vs = voicecall.voiceState(room.chat_id);
  return {
    chat_id: room.chat_id,
    engine: "ts",
    track: room.current,
    paused: room.paused,
    position_sec: positionSec(room),
    queue: room.queue,
    volume: room.volume,
    loop: Boolean(room.loop),
    muted: room.volume === 0 && room.muted_volume !== null && room.muted_volume !== undefined,
    voice: vs.voice,
    voice_joined: vs.joined,
    voice_reason: vs.reason,
  };
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
      loop: false,
      muted_volume: null,
    };
    rooms.set(chatId, room);
  }
  return room;
}

function saveState() {
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
        loop: Boolean(r.loop),
      }));
    const tmp = STATE_FILE + ".tmp." + process.pid;
    fs.writeFileSync(tmp, JSON.stringify({ rooms: snapshot }, null, 2));
    fs.renameSync(tmp, STATE_FILE);
  } catch (e) {
    console.error("[vc-ts] state save failed:", e.message);
  }
}

function loadState() {
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
        loop: Boolean(s.loop),
        muted_volume: null,
      });
    }
    console.log(`[vc-ts] restored ${rooms.size} room(s) from ${STATE_FILE} (paused)`);
  } catch (e) {
    console.error("[vc-ts] state load failed:", e.message);
  }
}

// 1 s ticker: wall-clock position + auto-advance when a track ends.
// Loop (remix session.loop) re-starts the current track instead of
// advancing. Radio tracks have duration_sec 0 and never advance.
setInterval(() => {
  let changed = false;
  for (const room of rooms.values()) {
    if (!room.current || room.paused) continue;
    const dur = Number(room.current.duration_sec) || 0;
    if (dur > 0 && positionSec(room) >= dur) {
      if (room.loop) {
        room.position_sec = 0;
        room.started_at_ms = Date.now();
        changed = true;
        console.log(`[vc-ts] chat ${room.chat_id}: loop — restarted ${room.current.title}`);
        continue;
      }
      const next = room.queue.shift();
      room.current = next ? next.track : null;
      room.position_sec = 0;
      room.started_at_ms = Date.now();
      changed = true;
      console.log(
        `[vc-ts] chat ${room.chat_id}: auto-advanced to ` +
        (room.current ? room.current.title : "(end of queue)")
      );
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

async function resolvePlayQuery(query) {
  const q = String(query);
  if (/^radio:/i.test(q)) {
    const url = q.replace(/^radio:/i, "").trim();
    if (!RAW_URL_RE.test(url)) throw Object.assign(new Error(`radio: expects a stream URL, got ${q.slice(0, 60)}`), { code: "bad_request" });
    return { track: R.radioTrackFromUrl(url), degraded: false };
  }
  if (RAW_URL_RE.test(q.trim()) && !R.extractYouTubeId(q)) {
    // Non-YouTube http(s) URL: a direct audio/radio stream.
    return { track: R.radioTrackFromUrl(q.trim()), degraded: false };
  }
  if (/^jiosaavn:/i.test(q)) {
    const term = q.replace(/^jiosaavn:/i, "").trim();
    const { results } = await R.searchJioSaavn(term, 1);
    if (!results.length) throw new R.EngineUnavailable("jiosaavn", `JioSaavn found nothing for "${term}"`);
    return { track: results[0], degraded: false };
  }
  if (/^jsvn:/i.test(q)) {
    const id = q.replace(/^jsvn:/i, "").trim();
    return { track: await R.resolveJioSaavnSong(id), degraded: false };
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
      if (room.queue.length < 1) return { ok: false, message: "No enough items to shuffle." };
      for (let i = room.queue.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [room.queue[i], room.queue[j]] = [room.queue[j], room.queue[i]];
      }
      room.queue.forEach((it, i) => (it.position = i));
      saveState();
      return { ok: true, message: "Shuffling...", state: panelState(room) };
    }
    case "skip": {
      if (!room.current) return { ok: false, message: "Not streaming." };
      const next = room.queue.shift();
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
      room.loop = !room.loop;
      saveState();
      return { ok: true, message: room.loop ? "Loop on." : "Loop off.", state: panelState(room) };
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
      const { track, degraded, error } = await resolvePlayQuery(body.query);
      if (!room.current) startTrack(room, track);
      else enqueue(room, track, requestedBy);
      if (degraded) console.log(`[vc-ts] degraded resolve for chat ${chatId}: ${error || ""}`);
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
      const next = room.queue.shift();
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
      let title = String(body.title || "").trim();
      let artist = String(body.artist || "").trim();
      if (!title && body.chat_id !== undefined && body.chat_id !== null) {
        const room = rooms.get(Number(body.chat_id));
        if (room && room.current) {
          title = room.current.title || "";
          artist = room.current.artist || "";
        }
      }
      if (!title) return fail(res, 400, "bad_request", "title is required (or chat_id with a track playing)");
      const found = await R.fetchLyrics(title, artist);
      if (!found) return fail(res, 404, "not_found", `no lyrics found for "${title}"`);
      return ok(res, found);
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
      const query = String(body.query || "").trim();
      if (!query) return fail(res, 400, "bad_request", "query is required");
      // Inline path (lemonic handlers/inline_query.ts): YouTube first,
      // Lemonic appended; JioSaavn skipped (no inline use). Lemonic is
      // always attempted now — its unauthenticated path needs no key —
      // and skipped only if the API itself fails.
      const data = await handleSearch({ query, source: "youtube", limit: 5 });
      try {
        const lem = await R.searchLemonic(query, 5);
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

loadState();
server.listen(PORT, () => {
  console.log(
    `[vc-ts] listening on :${PORT} (yt-dlp: ${R.ytDlpAvailable() ? R.ytDlpPath() : "missing"}, ` +
    `lemonic: ${R.lemonicConfigured() ? "configured" : "not configured"}, ` +
    `auth: ${TOKEN ? "token required" : "open (no PARLAY_TOKEN)"}, ` +
    `voice: ${voicecall.status().voice})`
  );
});

process.on("SIGINT", () => { saveState(); process.exit(0); });
process.on("SIGTERM", () => { saveState(); process.exit(0); });
