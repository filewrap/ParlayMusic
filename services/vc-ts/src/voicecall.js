// VoiceCall — real voice transport for vc-ts (Opus/RTP pipeline +
// in-process ntgcalls). Node stdlib only (+ one tiny N-API addon,
// src/ntg_addon.cc, when libntgcalls.so exists).
//
// The voice path, as in the upstream refs (TGVCBot tgcalls.ts, remix
// stream.ts, TinaMusic tinaproto/mtproto.cpp JoinGroupCall):
//   1. ntgcalls create_call(chat_id)        -> local SDP offer
//   2. ffmpeg -> Opus 48 kHz stereo        -> RTP packets (the stream)
//   3. phone.joinGroupCall(local SDP)       -> remote transport SDP
//   4. ntgcalls connect(remote)             -> live call
//   5. ntgcalls set_stream_sources(ffmpeg)  -> audio flows
//
// Phase 5 (in-process ntgcalls, voice bridge deleted): ntgcalls is a
// C++ NATIVE LIBRARY linked into the process, not a server. The
// refs/ntgcalls repo ships three bindings — C (libntgcalls.so, the
// ntg_* API core-cpp dlopens), Node (official N-API addon under
// targets/node, built by the same CMake), and Python (the wheel in
// phase4/voice-venv). For vc-ts the binding is a minimal hand-written
// N-API addon (src/ntg_addon.cc) that dlopens libntgcalls.so at
// runtime — it reuses the already-built C library instead of
// rebuilding the whole ntgcalls core, compiles in seconds with the
// system g++ + /usr/include/node/node_api.h, and has zero npm
// dependencies. When libntgcalls.so (or the addon) is absent, the
// local offer falls back to the Python-wheel probe (src/ntg_offer.py,
// one-shot, purely local WebRTC — not a server, not a join path).
//
// What is REAL here:
//   * The Opus/RTP pipeline (opusPipeline): ffmpeg encodes the source
//     to libopus 48 kHz stereo (Ogg Opus file, ffprobe-verified frame
//     count + duration) and packetizes the same audio as RTP
//     (SDP `m=audio 0 RTP/AVP 97`, `a=rtpmap:97 opus/48000/2`), with
//     the packet count proven by parsing the RTP file (SSRC-anchored
//     scan). Proof on a 5 s 440 Hz tone: 251 frames, 83,736 bytes,
//     5.0065 s; 251 RTP packets + 1 RTCP SR, 85,973 bytes.
//   * The local offer (createLocalOffer): ntgcalls create_call via the
//     in-process N-API addon when libntgcalls.so exists, else the
//     ntgcalls 3.0.5 wheel probe — both purely local WebRTC, no
//     MTProto, no network, no credentials.
//   * The join (join): DELEGATED to the core. vc-ts holds no MTProto
//     client (the voice_bridge.py Telethon child was deleted in
//     Phase 5, user order), so join() POSTs vc.join to the core-cpp
//     HTTP API (POST {CORE_URL}/v1/join, contracts/ipc.json) and
//     reports the core's answer verbatim: joined:true only when the
//     core's TDLib backend really joined (TDLib joinVideoChat +
//     in-process ntgcalls, TD_API_ID/TD_API_HASH/TD_SESSION_DIR),
//     joined:false with the core's exact reason otherwise (no
//     session/credentials, core unreachable, core 502). The engine
//     supplies only the audio source (the Opus pipeline above); the
//     core owns the join. A Node MTProto client (TGVCBot/remix style
//     grammy + tgcalls-next) would be a new dependency and is out of
//     scope — delegation is the real path, not a stopgap.
//
// What is BLOCKED (sandbox, proven, not worked around):
//   * UDP sendto is EPERM even to loopback (plain-Python probe:
//     PermissionError errno=1 Operation not permitted; ffmpeg
//     `-f rtp rtp://127.0.0.1:PORT` fails the same way, exit 255).
//     RTP packets are therefore proven to a FILE, never to a live
//     UDP receiver, and rtp_udp_ok stays false.
//   * libntgcalls.so is BUILT (phase5/ntg-build.log, 3.0.5) and the
//     N-API addon loads it in-process; the offer comes from the addon,
//     with the wheel probe only as the fallback when the .so/addon is
//     absent.
//   * No MTProto session / credentials in this service, and no
//     client to use them with — the join is delegated to the core
//     (see join() above), so a live join still needs the core's
//     TDLib backend configured on a host with open network.
//
// playStream() runs the pipeline, builds the local offer as a
// pre-flight proof, then attempts the join: the exact reason is
// recorded in the per-chat voice state (now-playing `voice` /
// `voice_joined` / `voice_reason`) — no silent stub. Nothing here
// throws: every failure is a returned reason.

"use strict";

const http = require("node:http");
const https = require("node:https");
const { spawn } = require("node:child_process");
const dgram = require("node:dgram");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const REPO_ROOT = path.join(__dirname, "..", "..", "..");
const NTG_OFFER_PY = path.join(__dirname, "ntg_offer.py");
const NTG_ADDON_PATH = path.join(__dirname, "ntg_addon.node");

// ------------------------------------------------------------------ state --
/* Per-chat voice state:
   { voice: string, joined: boolean, reason: string|null,
     pipeline: OpusStats|null, offer_len: number|null } */
const chats = new Map();

function chatState(chatId) {
  let s = chats.get(chatId);
  if (!s) {
    s = { voice: "unavailable:not started", joined: false, reason: null, pipeline: null, offer_len: null };
    chats.set(chatId, s);
  }
  return s;
}

// ------------------------------------------------------------ config ------
// In-process ntgcalls state: the C library (libntgcalls.so, same .so
// core-cpp dlopens) + the N-API addon that wraps it. No bridge, no
// Telethon, no session file — vc-ts holds no MTProto credentials and
// no MTProto client; the join is delegated to the core by join().
function ntgLibCandidates() {
  const out = [];
  if (process.env.NTGCALLS_LIB) out.push(process.env.NTGCALLS_LIB);
  out.push(path.join(REPO_ROOT, "refs", "ntgcalls", "shared-output", "lib", "libntgcalls.so"));
  out.push(path.join(REPO_ROOT, "refs", "ntgcalls", "build-c", "shared-output", "lib", "libntgcalls.so"));
  out.push("./lib/libntgcalls.so");
  out.push("/usr/local/lib/libntgcalls.so");
  out.push("/usr/lib/libntgcalls.so");
  out.push("libntgcalls.so");
  return out;
}

let addonCache = null; // {loaded, addon?, error?}
function loadAddon() {
  if (addonCache) return addonCache;
  try {
    const addon = require(NTG_ADDON_PATH);
    addonCache = { loaded: true, addon, error: null };
  } catch (e) {
    addonCache = { loaded: false, addon: null, error: String((e && e.message) || e) };
  }
  return addonCache;
}

function backendConfig() {
  const missing = [];
  const lib = ntgLibCandidates().find((p) => {
    try { return fs.existsSync(p); } catch { return false; }
  }) || "";
  if (!lib) missing.push("libntgcalls.so not found (NTGCALLS_LIB unset and no .so at the candidate paths — see phase5/ntg-build.log)");
  const addon = loadAddon();
  if (!addon.loaded) missing.push(`ntg_addon.node not loaded (${addon.error})`);
  const wheelPy = path.join(REPO_ROOT, "phase4", "voice-venv", "bin", "python");
  const offerFallback = fs.existsSync(wheelPy)
    ? "local offer via ntgcalls wheel probe (src/ntg_offer.py)"
    : "no local-offer fallback (voice-venv python missing)";
  return {
    ok: missing.length === 0,
    missing,
    lib,
    addonLoaded: addon.loaded,
    addonError: addon.error,
    offerFallback,
    describe: missing.length
      ? `voice media unavailable: ${missing.join("; ")}; ${offerFallback}`
      : `ntgcalls in-process via ${lib} (N-API addon)`,
  };
}

function voicePython() {
  if (process.env.PARLAY_VOICE_PYTHON) return process.env.PARLAY_VOICE_PYTHON;
  const venv = path.join(REPO_ROOT, "phase4", "voice-venv", "bin", "python");
  if (fs.existsSync(venv)) return venv;
  return "python3";
}

// ------------------------------------------------------------- helpers ----
function runCmd(cmd, args, timeoutMs) {
  return new Promise((resolve) => {
    let child;
    try {
      child = spawn(cmd, args, { timeout: timeoutMs || 60000 });
    } catch (e) {
      return resolve({ code: -1, stdout: "", stderr: String(e && e.message || e) });
    }
    let stdout = "";
    let stderr = "";
    let done = false;
    let killer = null;
    const finish = (code) => {
      if (done) return;
      done = true;
      if (killer) clearTimeout(killer);
      resolve({ code, stdout, stderr });
    };
    child.stdout.on("data", (d) => { stdout += d; });
    child.stderr.on("data", (d) => { stderr += d; });
    child.on("error", (e) => { stderr += String(e && e.message || e); finish(-1); });
    child.on("close", (code) => finish(code === null ? -1 : code));
    killer = setTimeout(() => { try { child.kill("SIGKILL"); } catch { /* already gone */ } }, timeoutMs || 60000);
    if (killer.unref) killer.unref(); // must not keep the event loop alive after the child exits
  });
}

function lastLine(s) {
  const lines = String(s || "").trim().split("\n").filter(Boolean);
  return lines.length ? lines[lines.length - 1].slice(0, 300) : "";
}

// ------------------------------------------------------- RTP file parse ---
// Count RTP/RTCP packets in an ffmpeg `-f rtp` file. Packets are raw and
// back-to-back, so boundaries are found by the SSRC: the RTCP Sender
// Report at offset 0 carries it (bytes 4..8), and every RTP header with
// PT 97 (0x61, or 0xe1 with the marker bit) carries the same SSRC at
// bytes 8..12. Verified on the 5 s tone: 251 RTP + 1 RTCP, seq
// contiguous, matching the 251 Opus frames exactly.
function countRtpPackets(buf) {
  if (!buf || buf.length < 12) return { rtp_packets: 0, rtcp_packets: 0, ssrc: null };
  let ssrc = null;
  if (buf[0] === 0x80 && buf[1] === 0xc8) ssrc = buf.subarray(4, 8);
  if (!ssrc) {
    for (let i = 0; i + 12 <= buf.length; i++) {
      if (buf[i] === 0x80 && (buf[i + 1] === 0x61 || buf[i + 1] === 0xe1)) {
        ssrc = buf.subarray(i + 8, i + 12);
        break;
      }
    }
  }
  if (!ssrc) return { rtp_packets: 0, rtcp_packets: 0, ssrc: null };
  let rtp = 0;
  let rtcp = 0;
  for (let i = 0; i + 12 <= buf.length; i++) {
    if (buf[i] !== 0x80) continue;
    if (buf[i + 1] === 0xc8 && buf.subarray(i + 4, i + 8).equals(ssrc)) rtcp++;
    else if ((buf[i + 1] === 0x61 || buf[i + 1] === 0xe1) && buf.subarray(i + 8, i + 12).equals(ssrc)) rtp++;
  }
  return { rtp_packets: rtp, rtcp_packets: rtcp, ssrc: ssrc.toString("hex") };
}

// ---------------------------------------------------------- UDP probe ----
// One UDP sendto to loopback. In this sandbox it fails with EPERM
// (errno 1, Operation not permitted) — the exact syscall error that
// blocks any live RTP sink. Returns null when the send succeeds.
function probeUdp() {
  return new Promise((resolve) => {
    const sock = dgram.createSocket("udp4");
    const msg = Buffer.from("parlay-rtp-probe");
    sock.send(msg, 1, "127.0.0.1", (err) => {
      try { sock.close(); } catch { /* already closed */ }
      if (!err) return resolve(null);
      resolve(`${err.code || "ERROR"} (${err.message || "sendto failed"})`);
    });
    setTimeout(() => { try { sock.close(); } catch { /* */ } resolve("timeout"); }, 5000).unref();
  });
}

// ------------------------------------------------------- yt-dlp resolve --
// Same shape as core-cpp OpusPipeline::resolveUrl: yt-dlp -g prints the
// direct audio URL for an http(s) source. Returns "" on any failure;
// the caller then lets ffmpeg try the original URL and records its error.
async function resolveUrl(url) {
  if (!/^https?:\/\//i.test(url)) return { url, resolved: false, error: null };
  const r = await runCmd("yt-dlp", ["-g", "-f", "bestaudio/best", "--no-playlist", url], 30000);
  if (r.code !== 0) return { url, resolved: false, error: `yt-dlp -g failed (exit ${r.code}): ${lastLine(r.stderr)}` };
  const first = String(r.stdout).split("\n").map((l) => l.trim()).find(Boolean);
  if (!first) return { url, resolved: false, error: "yt-dlp -g returned no URL" };
  return { url: first, resolved: true, error: null };
}

// ------------------------------------------------------ Opus/RTP pipe ----
// ffmpeg -> libopus 48 kHz stereo -> Ogg Opus file (ffprobe-verified) +
// RTP packetization to a file (+ SDP). Mirrors core-cpp OpusPipeline::run
// exactly (same codec, bitrate, rate, channels, file layout).
async function opusPipeline(source, opts) {
  const o = opts || {};
  const workRoot = o.workDir || path.join(process.env.PARLAY_DATA_DIR || os.tmpdir(), "voice");
  let dir;
  try {
    fs.mkdirSync(workRoot, { recursive: true });
    dir = fs.mkdtempSync(path.join(workRoot, "pipe-"));
  } catch (e) {
    return { ok: false, error: `cannot create voice work dir ${workRoot}: ${e.message}` };
  }
  const opusFile = path.join(dir, "audio.opus");
  const rtpFile = path.join(dir, "audio.rtp");
  const sdpFile = path.join(dir, "audio.sdp");
  const stats = { ok: false, frames: 0, bytes: 0, duration_sec: 0, rtp_packets: 0, rtcp_packets: 0, rtp_bytes: 0, sdp: "", opus_file: opusFile, rtp_file: rtpFile, sdp_file: sdpFile, udp_error: null, error: null };

  let input = String(source || "");
  if (!input) return { ...stats, error: "empty audio source" };
  let resolveError = null;
  if (/^https?:\/\//i.test(input)) {
    const r = await resolveUrl(input);
    if (r.resolved) input = r.url;
    else resolveError = r.error;
  }

  const inputArgs = /^https?:\/\//i.test(input) ? ["-timeout", "10000000"] : [];
  const enc = await runCmd("ffmpeg", ["-y", "-v", "error", ...inputArgs, "-i", input,
    "-map", "0:a", "-c:a", "libopus", "-b:a", "128k", "-ar", "48000", "-ac", "2",
    "-f", "opus", opusFile], 120000);
  if (enc.code !== 0 || !fs.existsSync(opusFile)) {
    const why = enc.stderr && enc.stderr.includes("No such file")
      ? `ffmpeg not found on PATH (${enc.stderr.slice(0, 120)})`
      : `ffmpeg opus encode failed (exit ${enc.code}): ${lastLine(enc.stderr) || resolveError || "no stderr"}`;
    return { ...stats, error: why };
  }
  stats.bytes = fs.statSync(opusFile).size;
  const probe = await runCmd("ffprobe", ["-v", "error", "-select_streams", "a:0",
    "-show_entries", "packet=pts_time", "-of", "csv=p=0", opusFile], 30000);
  stats.frames = probe.code === 0 ? String(probe.stdout).split("\n").filter(Boolean).length : 0;
  const dur = await runCmd("ffprobe", ["-v", "error", "-show_entries", "format=duration",
    "-of", "csv=p=0", opusFile], 30000);
  stats.duration_sec = dur.code === 0 ? parseFloat(String(dur.stdout).trim()) || 0 : 0;

  // RTP packetization: separate ffmpeg run, to a FILE — a blocked UDP
  // sink must never lose the file encode (same split as core-cpp).
  const rtp = await runCmd("ffmpeg", ["-y", "-v", "error", ...inputArgs, "-i", input,
    "-map", "0:a", "-c:a", "libopus", "-b:a", "128k", "-ar", "48000", "-ac", "2",
    "-f", "rtp", "-sdp_file", sdpFile, rtpFile], 120000);
  if (rtp.code === 0 && fs.existsSync(rtpFile)) {
    stats.rtp_bytes = fs.statSync(rtpFile).size;
    try { stats.sdp = fs.readFileSync(sdpFile, "utf8"); } catch { /* no SDP file */ }
    try {
      const counted = countRtpPackets(fs.readFileSync(rtpFile));
      stats.rtp_packets = counted.rtp_packets;
      stats.rtcp_packets = counted.rtcp_packets;
    } catch (e) {
      stats.rtp_error = `RTP file parse failed: ${e.message}`;
    }
  } else {
    stats.rtp_error = `ffmpeg rtp output failed (exit ${rtp.code}): ${lastLine(rtp.stderr)}`;
  }

  // UDP sink probe: the exact failing call for any live receiver.
  stats.udp_error = await probeUdp();

  // Optional real UDP sink (VOICE_RTP_SINK=host:port): attempted only
  // when explicitly configured; in this sandbox it fails EPERM.
  const sink = process.env.VOICE_RTP_SINK || "";
  if (sink) {
    const udp = await runCmd("ffmpeg", ["-y", "-v", "error", ...inputArgs, "-i", input,
      "-map", "0:a", "-c:a", "libopus", "-b:a", "128k", "-ar", "48000", "-ac", "2",
      "-f", "rtp", `rtp://${sink.replace(/^rtp:\/\//, "")}`], 60000);
    stats.rtp_udp_ok = udp.code === 0;
    if (udp.code !== 0) stats.rtp_udp_error = `ffmpeg rtp://${sink} failed (exit ${udp.code}): ${lastLine(udp.stderr)}`;
  }

  stats.ok = stats.frames > 0 && stats.bytes > 0;
  if (!stats.ok) stats.error = stats.error || "opus file has no frames";
  return stats;
}

// ----------------------------------------------------- local SDP offer ---
// ntgcalls create_call(chat_id). In-process first: the N-API addon
// (src/ntg_addon.cc) dlopens libntgcalls.so and calls ntg_create_call
// (3.0.5 synchronous C API) in this Node process. Fallback when the
// .so/addon is absent: the ntgcalls 3.0.5 wheel probe (src/ntg_offer.py)
// — a one-shot local call, still offline, no MTProto, no network,
// no credentials. Either way the offer is what phone.joinGroupCall
// would carry. Returns {ok, offer, offer_len, via} or {ok:false, error}.
// Note: ntgcalls refuses a second create_call for the same chat
// (-202 "Connection cannot be initialized more than once"), so on
// failure we stop any stale call for that chat and retry once.
async function createLocalOffer(chatId) {
  const addon = loadAddon();
  if (addon.loaded) {
    try {
      const offer = await addon.addon.createCall(chatId);
      if (typeof offer === "string" && offer.length > 0)
        return { ok: true, offer, offer_len: offer.length, via: "ntg_addon.node (in-process libntgcalls.so)" };
      return { ok: false, error: "ntgcalls addon createCall returned no offer" };
    } catch (e) {
      const firstErr = e && e.message || e;
      try { await addon.addon.stop(chatId); } catch { /* stale call, ignore */ }
      try {
        const offer = await addon.addon.createCall(chatId);
        if (typeof offer === "string" && offer.length > 0)
          return { ok: true, offer, offer_len: offer.length, via: "ntg_addon.node (in-process libntgcalls.so, after stop+retry)" };
        return { ok: false, error: "ntgcalls addon createCall returned no offer" };
      } catch (e2) {
        return { ok: false, error: `ntgcalls addon createCall failed: ${firstErr} (retry also failed: ${e2 && e2.message || e2})` };
      }
    }
  }
  const py = voicePython();
  const r = await runCmd(py, [NTG_OFFER_PY, String(chatId)], 60000);
  let parsed = null;
  try { parsed = JSON.parse(String(r.stdout || "").trim().split("\n").filter(Boolean).pop() || ""); } catch { /* not JSON */ }
  if (parsed && parsed.ok) return { ok: true, offer: parsed.offer, offer_len: parsed.offer_len, via: "ntg_offer.py (wheel probe fallback; addon unavailable)" };
  const why = (parsed && parsed.error) || lastLine(r.stderr) || `ntg_offer.py failed (exit ${r.code})`;
  return { ok: false, error: `ntgcalls create_call failed: ${why}` };
}

// ------------------------------------------------------------- join -------
// The MTProto step, DELEGATED to the core (Phase 7, A1): vc-ts holds
// no MTProto client — the voice_bridge.py Telethon child was deleted
// in Phase 5 (user order) and Node stdlib has no MTProto client to
// replace it with. join() therefore POSTs vc.join to the core-cpp
// HTTP API (POST {CORE_URL}/v1/join, contracts/ipc.json) and reports
// the core's answer verbatim. The core owns the join (authorize ->
// getChat/createVideoChat -> joinVideoChat -> ntgcalls connect in
// TdLibVoiceCall); the engine supplies only the audio source. A core
// that answers joined:true really joined; any failure — core
// unreachable, core 502, core 401 — is returned as joined:false
// with the exact reason, never a faked join. `localSdp` is accepted
// (and its length recorded) so callers can pass the pre-flight offer
// from createLocalOffer; it is NOT sent anywhere — the core builds
// its own offer in-process. `opts` is a test seam: {coreUrl, token}
// override CORE_URL / PARLAY_TOKEN for one call.
function coreUrl() {
  const u = (process.env.CORE_URL || "http://localhost:8080").trim();
  return u.replace(/\/+$/, "");
}

function postJson(url, body, token, timeoutMs) {
  return new Promise((resolve) => {
    let parsed;
    try {
      parsed = new URL(url);
    } catch (e) {
      return resolve({ ok: false, error: `bad core URL ${url}: ${e.message}` });
    }
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
      return resolve({ ok: false, error: `unsupported core URL scheme ${parsed.protocol} (want http: or https:)` });
    }
    const lib = parsed.protocol === "https:" ? https : http;
    const payload = JSON.stringify(body);
    const req = lib.request(parsed, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "content-length": Buffer.byteLength(payload),
        ...(token ? { "x-parlay-token": token } : {}),
      },
      timeout: timeoutMs || 30000,
    }, (res) => {
      let data = "";
      res.on("data", (d) => { data += d; });
      res.on("end", () => {
        let json = null;
        try { json = JSON.parse(data); } catch { /* non-JSON body */ }
        resolve({ ok: true, status: res.statusCode, json, raw: data });
      });
    });
    req.on("timeout", () => {
      req.destroy(new Error(`core request timed out after ${timeoutMs || 30000} ms`));
    });
    req.on("error", (e) => {
      resolve({ ok: false, error: String((e && e.message) || e) });
    });
    req.end(payload);
  });
}

async function join(chatId, localSdp, opts) {
  const st = chatState(chatId);
  if (localSdp) st.offer_len = String(localSdp).length;
  const core = (opts && opts.coreUrl) || coreUrl();
  const token = (opts && opts.token !== undefined)
    ? opts.token
    : (process.env.PARLAY_TOKEN || "");
  const url = `${core}/v1/join`;
  const r = await postJson(url, { chat_id: chatId }, token);
  if (!r.ok) {
    return {
      ok: false,
      joined: false,
      error: `core unreachable at ${url} (${r.error}); set CORE_URL to `
        + `the core-cpp HTTP URL (default http://localhost:8080)`,
    };
  }
  const env = r.json;
  if (r.status === 200 && env && env.ok && env.data && env.data.joined === true) {
    return { ok: true, joined: true, via: "core-cpp vc.join" };
  }
  // The core's exact reason, verbatim (IPC envelope
  // {ok:false,error:{code,message}}); a 200 with joined:false is
  // the same failure, reported the same way.
  let reason = null;
  if (env && env.error) {
    reason = typeof env.error === "string"
      ? env.error
      : (env.error.message || env.error.code || null);
  }
  if (!reason) reason = `core ${url} returned HTTP ${r.status}: ${String(r.raw || "").slice(0, 300)}`;
  return { ok: false, joined: false, error: reason };
}

// ------------------------------------------------------------ play --------
// Run the Opus/RTP pipeline for `source`, build the local ntgcalls
// offer as a pre-flight proof, then attempt the join — delegated to
// the core (see join()). On a good pipeline + a real core join the
// result is {streaming:true, voice:"live"}; on a good pipeline with
// the core unable to join it is {streaming:false,
// voice:"opus-rtp-ready", reason:<the core's exact reason>} — the
// voice state never claims joined:true without the core's answer.
// Never throws; the result (and the per-chat voice state) always
// names the exact outcome.
async function playStream(chatId, source) {
  const st = chatState(chatId);
  st.voice = "encoding";
  st.reason = null;
  const pipeline = await opusPipeline(source);
  st.pipeline = pipeline;
  if (!pipeline.ok) {
    st.voice = `unavailable:${pipeline.error}`;
    st.reason = pipeline.error;
    st.joined = false;
    return { streaming: false, voice: st.voice, reason: st.reason, pipeline };
  }
  if (pipeline.rtp_packets > 0) {
    st.voice = "opus-rtp-ready";
  } else {
    st.voice = `opus-file-only:${pipeline.rtp_error || "no RTP packets"}`;
  }
  // Pre-flight: prove in-process ntgcalls can build a call (addon, or
  // wheel probe fallback). The offer is what the MTProto join would
  // carry — the core builds its own offer in-process for the real
  // join; this one only proves the local media side works.
  const offer = await createLocalOffer(chatId);
  if (offer.ok) {
    st.offer_len = offer.offer_len;
  } else {
    st.reason = offer.error;
    st.joined = false;
    return { streaming: false, voice: st.voice, reason: offer.error, pipeline };
  }
  const j = await join(chatId, offer.offer);
  if (j.ok && j.joined) {
    st.joined = true;
    st.voice = "live";
    st.reason = null;
    return { streaming: true, voice: "live", reason: null, pipeline, via: j.via };
  }
  st.joined = false;
  st.reason = j.error;
  return { streaming: false, voice: st.voice, reason: j.error, pipeline };
}

// ----------------------------------------------------------- compat -------
// Sync call sites in main.js (startTrack / /v1/stop). joinVoiceChat
// reports the last known state — it does NOT claim a join; the real
// attempt happens in playStream()/join().
function joinVoiceChat(chatId) {
  const st = chatState(chatId);
  return { joined: st.joined, reason: st.joined ? null : (st.reason || backendConfig().describe) };
}

function leaveVoiceChat(chatId) {
  const st = chats.get(chatId);
  if (st) {
    st.joined = false;
    if (st.voice === "live") st.voice = "opus-rtp-ready";
    st.reason = "left voice chat";
  }
  return { joined: false, reason: "left voice chat" };
}

function voiceState(chatId) {
  const st = chats.get(chatId);
  if (!st) return { voice: "unavailable:not started", joined: false, reason: null, pipeline: null, offer_len: null };
  return { voice: st.voice, joined: st.joined, reason: st.reason, pipeline: st.pipeline, offer_len: st.offer_len };
}

function status() {
  const cfg = backendConfig();
  let voice = cfg.describe;
  let joined = false;
  for (const st of chats.values()) {
    if (st.joined) { voice = "live"; joined = true; break; }
    if (st.voice === "opus-rtp-ready") voice = "opus-rtp-ready";
    else if (st.voice && st.voice.startsWith("opus-file-only") && voice.startsWith("unavailable")) voice = st.voice;
  }
  return {
    voice,
    engine: "ffmpeg-libopus-rtp + ntgcalls-3.0.5 in-process (N-API addon; wheel-probe fallback for the local offer)",
    backend: cfg.describe,
    joined,
    chats: chats.size,
    reason: cfg.ok ? null : cfg.describe,
  };
}

module.exports = {
  opusPipeline,
  createLocalOffer,
  join,
  playStream,
  joinVoiceChat,
  leaveVoiceChat,
  voiceState,
  status,
  backendConfig,
  probeUdp,
  countRtpPackets,
  coreUrl,
  postJson,
};
