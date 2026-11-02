// Command parlay-gui is the Parlay web GUI / Telegram Mini App player.
//
// Ported (re-implemented, not copied) from refs/wavetune (Next.js Mini App:
// app/(player)/, app/(root)/page.js, components/music-provider.jsx).
// Per the task brief this service does NOT build Next.js: it is a plain
// Node.js stdlib HTTP server serving one single-page player. No npm deps.
//
// Environment:
//   PORT        HTTP listen port (default 3000)
//   CORE_URL    core-cpp base URL for now-playing/queue/search proxy
//               (e.g. http://localhost:8000); unset = local demo state
//   VC_GO_URL   vc-go base URL for play/pause/skip proxy (e.g. http://localhost:8001)
//   VC_TS_URL   vc-ts base URL; used only if VC_GO_URL is unset/unreachable
//   PARLAY_TOKEN forwarded as X-Parlay-Token when proxying to core/VC
//
// Routes:
//   GET  /                          player page (Telegram Mini App entry)
//   GET  /healthz                   {ok:true,service:"gui-js"}
//   GET  /api/now-playing?chat_id=  proxy CORE_URL, else local demo state
//   GET  /api/queue?chat_id=        proxy CORE_URL, else local demo state
//   GET  /api/search?q=             proxy CORE_URL, else NETEASE_URL, else engine_unavailable
//   GET  /api/stream-url?chat_id=   direct audio URL for the current track
//                                   (jiosaavn stream_url, radio stream, file
//                                   URL) — {playable, url?, reason?}
//   GET  /api/audio?src=<url>       same-origin proxy for that audio URL, so
//                                   the page's WebAudio GainNode can change
//                                   gain (cross-origin media through a
//                                   MediaElementAudioSourceNode is silenced
//                                   without CORS headers). Range passthrough.
//   POST /api/play  {chat_id,query} proxy VC engine, else engine_unavailable
//   POST /api/pause {chat_id}       proxy VC engine, else engine_unavailable
//   POST /api/skip  {chat_id}       proxy VC engine, else engine_unavailable
//
// Every /api response uses the Parlay envelope:
//   {ok:true,data:{...}} or {ok:false,error:{code,message}}
// Demo (non-proxied) data is always labelled source:"local-demo" so the
// page never pretends a live engine is behind it.
"use strict";

const http = require("http");
const { URL } = require("url");

const PORT = process.env.PORT || "3000";
const CORE_URL = (process.env.CORE_URL || "").replace(/\/$/, "");
const VC_GO_URL = (process.env.VC_GO_URL || "").replace(/\/$/, "");
const VC_TS_URL = (process.env.VC_TS_URL || "").replace(/\/$/, "");
const NETEASE_URL = (process.env.NETEASE_URL || "").replace(/\/$/, "");
const TOKEN = process.env.PARLAY_TOKEN || "";

// ---------------------------------------------------------------------------
// Local demo state (used ONLY when no core is reachable). Honestly labelled.
// ---------------------------------------------------------------------------
const demoQueue = [
  { position: 0, requested_by: 0, track: { id: "demo-1", title: "Demo Track — Raag Bhairavi (local demo)", artist: "Parlay demo", duration_sec: 214, source: "file", url: "" } },
  { position: 1, requested_by: 0, track: { id: "demo-2", title: "Demo Track — Madhumati (local demo)", artist: "Parlay demo", duration_sec: 187, source: "file", url: "" } },
];
let demoNowPlaying = {
  chat_id: 0,
  engine: "none",
  paused: true,
  position_sec: 0,
  volume: 70,
  source: "local-demo",
  track: demoQueue[0].track,
  queue: demoQueue,
};

function ok(res, data, status = 200) {
  const body = JSON.stringify({ ok: true, data });
  res.writeHead(status, { "content-type": "application/json" });
  res.end(body);
}
function fail(res, code, message, status = 502) {
  const body = JSON.stringify({ ok: false, error: { code, message } });
  res.writeHead(status, { "content-type": "application/json" });
  res.end(body);
}

async function readJson(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const raw = Buffer.concat(chunks).toString("utf8");
  if (!raw) return {};
  try { return JSON.parse(raw); } catch { return null; }
}

// Proxy a request to an upstream service. Returns {status, json} or null
// (null = upstream unreachable / not configured).
async function proxyJson(base, method, path, body, timeoutMs = 8000) {
  if (!base) return null;
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const r = await fetch(base + path, {
      method,
      headers: {
        "content-type": "application/json",
        ...(TOKEN ? { "x-parlay-token": TOKEN } : {}),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: ctrl.signal,
    });
    const text = await r.text();
    let json = null;
    try { json = JSON.parse(text); } catch { /* non-JSON upstream */ }
    return { status: r.status, json, text };
  } catch {
    return null;
  } finally {
    clearTimeout(t);
  }
}

// ---------------------------------------------------------------------------
// Player page. Single file, no external assets except the optional
// Telegram WebApp script (loaded lazily, guarded, plain-browser safe).
// ---------------------------------------------------------------------------
const PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Parlay — Player</title>
<style>
  body{font-family:system-ui,-apple-system,sans-serif;background:#111827;color:#f9fafb;margin:0;padding:16px;max-width:640px}
  h1{font-size:20px;margin:0 0 4px} .sub{color:#9ca3af;font-size:13px;margin:0 0 16px}
  .card{background:#1f2937;border-radius:12px;padding:16px;margin-bottom:12px}
  .np-title{font-size:18px;font-weight:600} .np-artist{color:#9ca3af}
  .badge{display:inline-block;font-size:11px;padding:2px 8px;border-radius:999px;background:#374151;color:#d1d5db;margin-left:6px}
  .badge.live{background:#065f46;color:#a7f3d0} .badge.demo{background:#78350f;color:#fde68a}
  .controls{display:flex;gap:8px;margin-top:12px;align-items:center;flex-wrap:wrap}
  button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:10px 16px;font-size:15px;cursor:pointer}
  button:disabled{opacity:.5;cursor:default}
  input[type=range]{flex:1;min-width:120px}
  input[type=text]{flex:1;min-width:160px;padding:10px;border-radius:8px;border:1px solid #374151;background:#111827;color:#fff}
  .row{display:flex;gap:8px}
  #queue{list-style:none;padding:0;margin:0} #queue li{padding:8px 0;border-bottom:1px solid #374151;font-size:14px}
  #queue li .pos{color:#9ca3af;margin-right:8px}
  #status{font-size:13px;color:#9ca3af;min-height:18px}
  #results{list-style:none;padding:0;margin:8px 0 0} #results li{padding:8px 0;border-bottom:1px solid #374151;font-size:14px;cursor:pointer}
  .err{color:#fca5a5}
</style>
</head>
<body>
<h1>Parlay Player <span id="srcBadge" class="badge demo">local demo</span></h1>
<p class="sub">Telegram Mini App player &middot; talks to the core over /api &middot; <span id="chatLabel">no chat selected</span></p>

<div class="card">
  <div class="np-title" id="npTitle">Nothing playing</div>
  <div class="np-artist" id="npArtist">&mdash;</div>
  <div class="controls">
    <button id="btnPlay" type="button">&#9654; Play</button>
    <button id="btnPause" type="button">&#10074;&#10074; Pause</button>
    <button id="btnSkip" type="button">Skip &raquo;</button>
    <label for="vol">Vol</label>
    <input id="vol" type="range" min="0" max="100" value="70">
    <span id="volVal">70</span>
  </div>
  <div id="status"></div>
  <div class="listen">
    <audio id="player" preload="none"></audio>
    <div class="row">
      <button id="btnListen" type="button">&#9654; Listen in browser</button>
      <button id="btnPreview" type="button" disabled>&#10074;&#10074; Preview</button>
      <span id="listenStatus" class="sub">browser audio off</span>
    </div>
    <div class="row">
      <span id="tCur">0:00</span>
      <input id="seek" type="range" min="0" max="0" value="0" step="1">
      <span id="tDur">0:00</span>
    </div>
    <div id="volMode" class="sub">volume: not initialised (press Listen — WebAudio gain when available)</div>
  </div>
</div>

<div class="card">
  <h2 style="font-size:15px;margin:0 0 8px">Search</h2>
  <div class="row">
    <input id="q" type="text" placeholder="Song, artist, or link&hellip;" autocomplete="off">
    <button id="btnSearch" type="button">Search</button>
  </div>
  <ul id="results"></ul>
</div>

<div class="card">
  <h2 style="font-size:15px;margin:0 0 8px">Queue</h2>
  <ul id="queue"></ul>
</div>

<script src="https://telegram.org/js/telegram-web-app.js" async></script>
<script>
(function () {
  "use strict";
  // Telegram WebApp hook: guarded so the page also works in a plain browser.
  try {
    if (window.Telegram && window.Telegram.WebApp) {
      window.Telegram.WebApp.ready();
      window.Telegram.WebApp.expand();
    }
  } catch (e) { /* plain browser: no WebApp */ }

  var params = new URLSearchParams(location.search);
  var chatId = params.get("chat_id") || "";
  document.getElementById("chatLabel").textContent =
    chatId ? ("chat " + chatId) : "no chat selected (demo state)";

  function api(path, opts) {
    return fetch(path, opts).then(function (r) { return r.json(); });
  }
  function setStatus(msg, isErr) {
    var el = document.getElementById("status");
    el.textContent = msg || "";
    el.className = isErr ? "err" : "";
  }
  function setBadge(source) {
    var b = document.getElementById("srcBadge");
    if (source === "local-demo") { b.textContent = "local demo"; b.className = "badge demo"; }
    else { b.textContent = "live: " + source; b.className = "badge live"; }
  }
  function renderNowPlaying(np) {
    if (!np || !np.track) {
      document.getElementById("npTitle").textContent = "Nothing playing";
      document.getElementById("npArtist").textContent = "—";
      return;
    }
    document.getElementById("npTitle").textContent = np.track.title || "Untitled";
    document.getElementById("npArtist").textContent =
      (np.track.artist || "Unknown artist") +
      (np.track.duration_sec ? (" · " + Math.floor(np.track.duration_sec / 60) + ":" + String(np.track.duration_sec % 60).padStart(2, "0")) : "") +
      (np.paused ? " · paused" : " · playing");
    // NOTE: the volume slider is the *browser* volume (WebAudio gain
    // below), not the VC engine volume, so it is never overwritten by
    // the engine's reported volume.
    setBadge(np.source || (np.engine && np.engine !== "none" ? np.engine : "local-demo"));
  }
  function renderQueue(items) {
    var ul = document.getElementById("queue");
    ul.innerHTML = "";
    (items || []).forEach(function (item, i) {
      var li = document.createElement("li");
      var t = item.track || item;
      li.innerHTML = '<span class="pos">' + (item.position != null ? item.position + 1 : i + 1) + ".</span>" +
        escapeHtml(t.title || "Untitled") + " — " + escapeHtml(t.artist || "");
      ul.appendChild(li);
    });
    if (!items || !items.length) ul.innerHTML = "<li>Queue is empty.</li>";
  }
  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }
  function refresh() {
    var qs = chatId ? ("?chat_id=" + encodeURIComponent(chatId)) : "";
    api("/api/now-playing" + qs).then(function (env) {
      if (env.ok) renderNowPlaying(env.data.now_playing || env.data);
      else setStatus("now-playing: " + (env.error && env.error.message), true);
    }).catch(function (e) { setStatus("now-playing failed: " + e.message, true); });
    api("/api/queue" + qs).then(function (env) {
      if (env.ok) renderQueue(env.data.queue || []);
      else setStatus("queue: " + (env.error && env.error.message), true);
    }).catch(function (e) { setStatus("queue failed: " + e.message, true); });
    if (listening) refreshStream();
  }

  // ------------------------------------------------- in-browser playback --
  // wavetune's player is a plain <audio src={downloadUrl}> element
  // (components/cards/player.jsx). Same here, with two additions:
  // the URL comes from /api/stream-url (a real stream/download URL
  // from the IPC layer, via the same-origin /api/audio proxy), and
  // volume goes through a WebAudio GainNode so the slider actually
  // changes gain. The proxy matters: media routed through a
  // MediaElementAudioSourceNode is silenced when it is cross-origin
  // without CORS headers, so same-origin is what makes gain work.
  var audioEl = document.getElementById("player");
  var audioCtx = null, gainNode = null;
  var listening = false, currentTrackId = null;
  var vol = 70;
  try {
    var savedVol = Number(localStorage.getItem("parlay-vol"));
    if (savedVol >= 0 && savedVol <= 100) vol = savedVol;
  } catch (e) { /* no localStorage */ }
  document.getElementById("vol").value = vol;
  document.getElementById("volVal").textContent = vol;
  audioEl.volume = vol / 100; // fallback path if WebAudio is unavailable

  function fmtTime(s) {
    s = Math.floor(s || 0);
    return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
  }
  function setVolMode(text) {
    document.getElementById("volMode").textContent = "volume: " + text;
  }
  function ensureAudioGraph() {
    if (audioCtx) return true;
    try {
      var AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) { setVolMode("element volume (no WebAudio in this browser)"); return false; }
      audioCtx = new AC();
      var srcNode = audioCtx.createMediaElementSource(audioEl);
      gainNode = audioCtx.createGain();
      gainNode.gain.value = vol / 100;
      srcNode.connect(gainNode);
      gainNode.connect(audioCtx.destination);
      setVolMode("WebAudio gain (gainNode.gain = " + (vol / 100) + ")");
      return true;
    } catch (e) {
      setVolMode("element volume (WebAudio failed: " + e.message + ")");
      return false;
    }
  }
  function applyVolume(v) {
    vol = v;
    document.getElementById("volVal").textContent = v;
    try { localStorage.setItem("parlay-vol", String(v)); } catch (e) { /* ignore */ }
    if (gainNode) {
      gainNode.gain.value = v / 100;
      setVolMode("WebAudio gain (gainNode.gain = " + (v / 100) + ")");
    } else {
      audioEl.volume = v / 100;
    }
  }
  function refreshStream() {
    if (!listening) return;
    var qs = chatId ? ("?chat_id=" + encodeURIComponent(chatId)) : "";
    var st = document.getElementById("listenStatus");
    api("/api/stream-url" + qs).then(function (env) {
      if (!env.ok || !env.data.playable) {
        st.textContent = "browser audio: " + ((env.data && env.data.reason) || (env.error && env.error.message) || "unavailable");
        if (currentTrackId !== null) {
          currentTrackId = null;
          audioEl.removeAttribute("src");
          audioEl.load();
        }
        return;
      }
      var t = env.data.track;
      if (currentTrackId === t.id && audioEl.getAttribute("src")) return; // same track: keep playing
      currentTrackId = t.id;
      ensureAudioGraph();
      if (audioCtx && audioCtx.state === "suspended") audioCtx.resume();
      audioEl.src = env.data.url;
      var playP = audioEl.play();
      if (playP && playP.then) {
        playP.then(function () {
          st.textContent = "browser audio: " + t.title + " — " + (t.artist || "");
        }).catch(function (e) {
          st.textContent = "browser audio: play blocked (" + e.message + ")";
        });
      } else {
        st.textContent = "browser audio: " + t.title + " — " + (t.artist || "");
      }
      document.getElementById("btnPreview").disabled = false;
    }).catch(function (e) {
      st.textContent = "browser audio: stream-url failed (" + e.message + ")";
    });
  }
  audioEl.addEventListener("ended", function () {
    // The VC engine may have advanced to the next track; reload the
    // stream URL for whatever is now playing (never skips the VC).
    currentTrackId = null;
    refreshStream();
  });
  audioEl.addEventListener("timeupdate", function () {
    document.getElementById("tCur").textContent = fmtTime(audioEl.currentTime);
    document.getElementById("tDur").textContent = fmtTime(audioEl.duration);
    var seek = document.getElementById("seek");
    if (isFinite(audioEl.duration) && audioEl.duration > 0) {
      seek.max = Math.floor(audioEl.duration);
      if (document.activeElement !== seek) seek.value = Math.floor(audioEl.currentTime);
    }
  });
  audioEl.addEventListener("play", function () {
    document.getElementById("btnPreview").innerHTML = "&#10074;&#10074; Preview";
  });
  audioEl.addEventListener("pause", function () {
    document.getElementById("btnPreview").innerHTML = "&#9654; Preview";
  });
  document.getElementById("btnListen").addEventListener("click", function () {
    listening = !listening;
    var btn = document.getElementById("btnListen");
    if (listening) {
      btn.innerHTML = "&#9632; Stop browser audio";
      applyVolume(vol);
      refreshStream();
    } else {
      btn.innerHTML = "&#9654; Listen in browser";
      audioEl.pause();
      audioEl.removeAttribute("src");
      audioEl.load();
      currentTrackId = null;
      document.getElementById("btnPreview").disabled = true;
      document.getElementById("listenStatus").textContent = "browser audio off";
    }
  });
  document.getElementById("btnPreview").addEventListener("click", function () {
    if (audioEl.paused) { if (audioCtx && audioCtx.state === "suspended") audioCtx.resume(); audioEl.play(); }
    else audioEl.pause();
  });
  document.getElementById("seek").addEventListener("input", function (e) {
    audioEl.currentTime = Number(e.target.value) || 0;
  });
  function control(action, extra) {
    var body = Object.assign({ chat_id: chatId ? Number(chatId) : 0 }, extra || {});
    setStatus(action + "…");
    api("/api/" + action, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    }).then(function (env) {
      if (env.ok) { setStatus(""); refresh(); }
      else setStatus(action + ": " + (env.error && env.error.message) +
        (env.error && env.error.code === "engine_unavailable" ? " (no VC engine connected)" : ""), true);
    }).catch(function (e) { setStatus(action + " failed: " + e.message, true); });
  }
  document.getElementById("btnPlay").addEventListener("click", function () { control("play", { query: "" }); });
  document.getElementById("btnPause").addEventListener("click", function () { control("pause"); });
  document.getElementById("btnSkip").addEventListener("click", function () { control("skip"); });
  document.getElementById("vol").addEventListener("input", function (e) {
    applyVolume(Number(e.target.value) || 0);
  });
  document.getElementById("btnSearch").addEventListener("click", doSearch);
  document.getElementById("q").addEventListener("keydown", function (e) { if (e.key === "Enter") doSearch(); });
  function doSearch() {
    var q = document.getElementById("q").value.trim();
    if (!q) return;
    var ul = document.getElementById("results");
    ul.innerHTML = "<li>Searching…</li>";
    api("/api/search?q=" + encodeURIComponent(q)).then(function (env) {
      ul.innerHTML = "";
      if (!env.ok) { ul.innerHTML = "<li class='err'>" + escapeHtml(env.error.message) + "</li>"; return; }
      var results = env.data.results || [];
      if (!results.length) { ul.innerHTML = "<li>No results.</li>"; return; }
      results.forEach(function (t) {
        var li = document.createElement("li");
        li.textContent = (t.title || "Untitled") + " — " + (t.artist || "") + (t.source ? " [" + t.source + "]" : "");
        li.addEventListener("click", function () {
          document.getElementById("q").value = t.title;
          control("play", { query: t.url || t.title, track_id: t.id });
        });
        ul.appendChild(li);
      });
    }).catch(function (e) { ul.innerHTML = "<li class='err'>search failed: " + escapeHtml(e.message) + "</li>"; });
  }
  refresh();
  setInterval(refresh, 15000);
})();
</script>
</body>
</html>
`;

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
async function handleNowPlaying(req, res, url) {
  const chatId = url.searchParams.get("chat_id") || "";
  if (CORE_URL) {
    const up = await proxyJson(CORE_URL, "GET", "/api/now-playing?chat_id=" + encodeURIComponent(chatId));
    if (up && up.json && up.json.ok) {
      const np = up.json.data.now_playing || up.json.data;
      if (np && !np.source) np.source = "core";
      return ok(res, { now_playing: np });
    }
    if (up && up.json && !up.json.ok) {
      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
    }
    // CORE_URL set but unreachable: fall through to honest demo state.
  }
  const np = { ...demoNowPlaying, chat_id: chatId ? Number(chatId) : 0 };
  return ok(res, { now_playing: np, note: "core unreachable or CORE_URL unset — serving local demo state" });
}

async function handleQueue(req, res, url) {
  const chatId = url.searchParams.get("chat_id") || "";
  if (CORE_URL) {
    const up = await proxyJson(CORE_URL, "GET", "/api/queue?chat_id=" + encodeURIComponent(chatId));
    if (up && up.json && up.json.ok) {
      return ok(res, { queue: up.json.data.queue || [], source: "core" });
    }
    if (up && up.json && !up.json.ok) {
      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
    }
  }
  return ok(res, { queue: demoQueue, source: "local-demo", note: "core unreachable or CORE_URL unset — serving local demo queue" });
}

async function handleStreamUrl(req, res, url) {
  const chatId = url.searchParams.get("chat_id") || "";
  const np = await fetchNowPlaying(chatId);
  const track = np && np.track;
  if (!track) {
    return ok(res, { playable: false, reason: "nothing playing", source: np ? np.source : "none" });
  }
  const direct = playableUrl(track);
  if (!direct) {
    return ok(res, {
      playable: false,
      reason: `no direct audio URL for source "${track.source}" (YouTube watch pages are not audio streams; play it in the Telegram voice chat)`,
      source: track.source,
      track: { id: track.id, title: track.title, artist: track.artist },
    });
  }
  return ok(res, {
    playable: true,
    // Same-origin proxy URL: the <audio> element loads this, so the
    // WebAudio gain stage sees CORS-clean media.
    url: `/api/audio?src=${encodeURIComponent(direct)}`,
    direct_url: direct,
    source: track.source,
    track: { id: track.id, title: track.title, artist: track.artist, duration_sec: track.duration_sec },
  });
}

// Fetch now-playing from the VC engines (then CORE_URL) for stream-url
// resolution. Returns the now-playing object or null.
async function fetchNowPlaying(chatId) {
  for (const base of [VC_GO_URL, VC_TS_URL]) {
    if (!base) continue;
    const up = await proxyJson(base, "GET", "/v1/now-playing?chat_id=" + encodeURIComponent(chatId));
    if (up && up.json && up.json.ok) {
      const np = up.json.data.now_playing || up.json.data;
      if (np && np.track) return { ...np, source: base === VC_GO_URL ? "go" : "ts" };
    }
  }
  if (CORE_URL) {
    const up = await proxyJson(CORE_URL, "GET", "/api/now-playing?chat_id=" + encodeURIComponent(chatId));
    if (up && up.json && up.json.ok) {
      const np = up.json.data.now_playing || up.json.data;
      if (np && np.track) return { ...np, source: "core" };
    }
  }
  return null;
}

// The only URLs a browser <audio> element can play directly: a real
// media URL, not a watch/listing page. JioSaavn perma_url and YouTube
// watch URLs are pages — only stream_url (or a radio/file URL) counts.
function playableUrl(track) {
  if (!track) return null;
  if (track.stream_url && /^https?:\/\//i.test(track.stream_url)) return track.stream_url;
  if ((track.source === "radio" || track.source === "file") && track.url && /^https?:\/\//i.test(track.url)) {
    return track.url;
  }
  return null;
}

async function handleAudioProxy(req, res, url) {
  const src = url.searchParams.get("src") || "";
  if (!/^https?:\/\//i.test(src)) {
    return fail(res, "bad_request", "src must be an http(s) audio URL", 400);
  }
  let upstream;
  try {
    const headers = {};
    if (req.headers.range) headers.range = req.headers.range;
    upstream = await fetch(src, { headers, redirect: "follow" });
  } catch (e) {
    return fail(res, "download_failed", `audio proxy fetch failed: ${e.message.slice(0, 200)}`, 502);
  }
  if (!upstream.ok && upstream.status !== 206) {
    return fail(res, "download_failed", `audio source returned HTTP ${upstream.status}`, 502);
  }
  const outHeaders = { "content-type": upstream.headers.get("content-type") || "audio/mpeg", "accept-ranges": "bytes" };
  if (upstream.headers.get("content-length")) outHeaders["content-length"] = upstream.headers.get("content-length");
  if (upstream.headers.get("content-range")) outHeaders["content-range"] = upstream.headers.get("content-range");
  res.writeHead(upstream.status, outHeaders);
  try {
    for await (const chunk of upstream.body) {
      if (!res.write(chunk)) await new Promise((r) => res.once("drain", r));
    }
  } catch { /* client went away */ }
  res.end();
}

async function handleSearch(req, res, url) {
  const q = (url.searchParams.get("q") || "").trim();
  if (!q) return fail(res, "bad_request", "missing q", 400);
  if (CORE_URL) {
    const up = await proxyJson(CORE_URL, "POST", "/v1/search", { query: q, source: "all", limit: 10 });
    if (up && up.json && up.json.ok) return ok(res, { results: up.json.data.results || [], source: "core" });
    if (up && up.json && !up.json.ok) {
      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "core error", up.status || 502);
    }
  }
  if (NETEASE_URL) {
    const up = await proxyJson(NETEASE_URL, "POST", "/v1/netease/search", { query: q });
    if (up && up.json && up.json.ok) return ok(res, { results: up.json.data.results || [], source: "netease" });
    if (up && up.json && !up.json.ok) {
      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "netease error", up.status || 502);
    }
  }
  return fail(res, "engine_unavailable", "no search backend configured (set CORE_URL or NETEASE_URL)", 502);
}

async function handleControl(req, res, action) {
  const body = await readJson(req);
  if (body === null) return fail(res, "bad_request", "invalid JSON body", 400);
  const chatId = body.chat_id;
  if (chatId === undefined || chatId === null || chatId === "") {
    return fail(res, "bad_request", "chat_id is required", 400);
  }
  // Try vc-go first, then vc-ts (per ARCHITECTURE.md engine fallback order).
  for (const base of [VC_GO_URL, VC_TS_URL]) {
    if (!base) continue;
    const up = await proxyJson(base, "POST", "/v1/" + action, body);
    if (up && up.json && up.json.ok) {
      const np = up.json.data.now_playing || up.json.data;
      return ok(res, { now_playing: np, engine: base === VC_GO_URL ? "go" : "ts" });
    }
    if (up && up.json && !up.json.ok) {
      return fail(res, up.json.error.code || "engine_unavailable", up.json.error.message || "engine error", up.status || 502);
    }
    // Unreachable: try the next engine.
  }
  // No engine reachable: mutate the local demo state honestly, and say so.
  if (action === "play") {
    if (body.query) {
      const t = { id: "demo-play-" + Date.now(), title: String(body.query), artist: "local demo", duration_sec: 0, source: "file", url: "" };
      demoQueue.unshift({ position: 0, requested_by: body.requested_by || 0, track: t });
      demoNowPlaying = { ...demoNowPlaying, track: t, paused: false, position_sec: 0, queue: demoQueue };
    } else {
      demoNowPlaying = { ...demoNowPlaying, paused: false };
    }
  } else if (action === "pause") {
    demoNowPlaying = { ...demoNowPlaying, paused: true };
  } else if (action === "skip") {
    const next = demoQueue[1];
    if (next) {
      demoQueue.shift();
      demoNowPlaying = { ...demoNowPlaying, track: next.track, paused: false, position_sec: 0, queue: demoQueue };
    } else {
      demoNowPlaying = { ...demoNowPlaying, paused: true, position_sec: 0 };
    }
  }
  return ok(res, {
    now_playing: demoNowPlaying,
    note: "no VC engine configured/reachable (set VC_GO_URL or VC_TS_URL) — applied to local demo state only",
  });
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://localhost");
    if (req.method === "GET" && url.pathname === "/") {
      res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      res.end(PAGE);
      return;
    }
    if (req.method === "GET" && url.pathname === "/healthz") {
      // Contract shape: top-level {ok, service}, not wrapped in data.
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, service: "gui-js" }));
      return;
    }
    if (req.method === "GET" && url.pathname === "/api/now-playing") {
      return handleNowPlaying(req, res, url);
    }
    if (req.method === "GET" && url.pathname === "/api/queue") {
      return handleQueue(req, res, url);
    }
    if (req.method === "GET" && url.pathname === "/api/search") {
      return handleSearch(req, res, url);
    }
    if (req.method === "GET" && url.pathname === "/api/stream-url") {
      return handleStreamUrl(req, res, url);
    }
    if (req.method === "GET" && url.pathname === "/api/audio") {
      return handleAudioProxy(req, res, url);
    }
    if (req.method === "POST" && ["/api/play", "/api/pause", "/api/skip"].includes(url.pathname)) {
      return handleControl(req, res, url.pathname.slice(5));
    }
    return fail(res, "not_found", "no such route: " + req.method + " " + url.pathname, 404);
  } catch (e) {
    return fail(res, "internal", String(e && e.message || e), 500);
  }
});

server.listen(Number(PORT), () => {
  console.log("parlay gui-js listening on :" + PORT +
    (CORE_URL ? " core=" + CORE_URL : " (no CORE_URL — demo state)") +
    (VC_GO_URL ? " vc-go=" + VC_GO_URL : "") +
    (VC_TS_URL ? " vc-ts=" + VC_TS_URL : "") +
    (NETEASE_URL ? " netease=" + NETEASE_URL : ""));
});
