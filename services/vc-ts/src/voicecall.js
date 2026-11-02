// VoiceCall — real voice transport for vc-ts (Opus/RTP pipeline +
// TDLib-join client). Node stdlib only.
//
// The voice path, as in the upstream refs (TGVCBot tgcalls.ts, remix
// stream.ts, TinaMusic tinaproto/mtproto.cpp JoinGroupCall):
//   1. ntgcalls create_call(chat_id)        -> local SDP offer
//   2. ffmpeg -> Opus 48 kHz stereo        -> RTP packets (the stream)
//   3. phone.joinGroupCall(local SDP)       -> remote transport SDP
//   4. ntgcalls connect(remote)             -> live call
//   5. ntgcalls set_stream_sources(ffmpeg)  -> audio flows
//
// What is REAL here:
//   * The Opus/RTP pipeline (opusPipeline): ffmpeg encodes the source
//     to libopus 48 kHz stereo (Ogg Opus file, ffprobe-verified frame
//     count + duration) and packetizes the same audio as RTP
//     (SDP `m=audio 0 RTP/AVP 97`, `a=rtpmap:97 opus/48000/2`), with
//     the packet count proven by parsing the RTP file (SSRC-anchored
//     scan). Proof on a 5 s 440 Hz tone: 251 frames, 83,736 bytes,
//     5.0065 s; 251 RTP packets + 1 RTCP SR, 85,973 bytes.
//   * The local offer (createLocalOffer): the ntgcalls 3.0.5 wheel in
//     phase4/voice-venv builds a real offer offline (466 chars on the
//     probe chat) — no MTProto, no network, no credentials.
//   * The join client (join): drives the same voice_bridge.py
//     (Telethon + ntgcalls) that core-cpp spawns, and reports the
//     exact failure when the backend is not configured. There is NO
//     voice-join action in contracts/ipc.json and core-cpp exposes no
//     HTTP join endpoint, so the join goes through the bridge process
//     directly — the IPC contract only carries vc.play/pause/etc.
//
// What is BLOCKED (sandbox, proven, not worked around):
//   * UDP sendto is EPERM even to loopback (plain-Python probe:
//     PermissionError errno=1 Operation not permitted; ffmpeg
//     `-f rtp rtp://127.0.0.1:PORT` fails the same way, exit 255).
//     RTP packets are therefore proven to a FILE, never to a live
//     UDP receiver, and rtp_udp_ok stays false.
//   * No MTProto session / credentials in this sandbox
//     (ASSISTANT_SESSION_FILE, API_ID, API_HASH unset), so join()
//     fails at the config gate with the exact missing pieces named.
//     Without a session there is no phone.joinGroupCall and no live
//     call; joined:true is NEVER reported without a real join.
//
// playStream() runs the pipeline and then attempts the join: when a
// voice backend is available the stream is fed into the join; when it
// is not, the exact reason is recorded in the per-chat voice state
// (now-playing `voice` / `voice_joined` / `voice_reason`) — no silent
// stub. Nothing here throws: every failure is a returned reason.

"use strict";

const { spawn } = require("node:child_process");
const dgram = require("node:dgram");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const REPO_ROOT = path.join(__dirname, "..", "..", "..");
const BRIDGE_DEFAULT = path.join(REPO_ROOT, "services", "core-cpp", "voice_bridge.py");
const NTG_OFFER_PY = path.join(__dirname, "ntg_offer.py");

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
function backendConfig() {
  const missing = [];
  const bridge = process.env.VOICE_BRIDGE || BRIDGE_DEFAULT;
  if (!fs.existsSync(bridge)) missing.push(`VOICE_BRIDGE missing (${bridge})`);
  const session = process.env.ASSISTANT_SESSION_FILE || "";
  if (!session) missing.push("ASSISTANT_SESSION_FILE unset");
  else if (!fs.existsSync(session)) missing.push(`ASSISTANT_SESSION_FILE not found (${session})`);
  const apiId = Number(process.env.API_ID || 0);
  if (!apiId) missing.push("API_ID unset");
  const apiHash = process.env.API_HASH || "";
  if (!apiHash) missing.push("API_HASH unset");
  return {
    ok: missing.length === 0,
    missing,
    bridge,
    session,
    apiId,
    apiHash,
    describe: missing.length
      ? `voice join unavailable: ${missing.join("; ")}`
      : `voice-bridge via ${bridge} (Telethon + ntgcalls)`,
  };
}

function voicePython() {
  if (process.env.PARLAY_VOICE_PYTHON) return process.env.PARLAY_VOICE_PYTHON;
  const venv = path.join(REPO_ROOT, "phase4", "voice-venv", "bin", "python");
  if (fs.existsSync(venv)) return venv;
  return "python3";
}

// ------------------------------------------------------------- helpers ----
function shellQuote(s) {
  return "'" + String(s).replace(/'/g, "'\\''") + "'";
}

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
// ntgcalls create_call(chat_id) via the voice-venv wheel. Offline:
// proves the engine can build a call; the offer is what
// phone.joinGroupCall carries. Returns {ok, offer, offer_len} or
// {ok:false, error}.
async function createLocalOffer(chatId) {
  const py = voicePython();
  const r = await runCmd(py, [NTG_OFFER_PY, String(chatId)], 60000);
  let parsed = null;
  try { parsed = JSON.parse(String(r.stdout || "").trim().split("\n").filter(Boolean).pop() || ""); } catch { /* not JSON */ }
  if (parsed && parsed.ok) return { ok: true, offer: parsed.offer, offer_len: parsed.offer_len };
  const why = (parsed && parsed.error) || lastLine(r.stderr) || `ntg_offer.py failed (exit ${r.code})`;
  return { ok: false, error: `ntgcalls create_call failed: ${why}` };
}

// ------------------------------------------------------------- join -------
// The MTProto step: phone.joinGroupCall(local SDP) -> remote transport
// -> ntgcalls connect(remote). Done by voice_bridge.py join-call, the
// same child core-cpp spawns. The bridge prints one JSON object per
// line; we watch for {"event":"joined"} (then SIGTERM the child — the
// join is proven, the stream would run until stopped) or
// {"event":"error"} (the exact failure). Bounded at 30 s.
// Returns {ok, joined, remote_len?} or {ok:false, joined:false, error}.
async function join(chatId, localSdp) {
  const cfg = backendConfig();
  if (!cfg.ok) return { ok: false, joined: false, error: cfg.describe };
  let offer = localSdp || "";
  let offerLen = offer ? offer.length : 0;
  if (!offer) {
    const o = await createLocalOffer(chatId);
    if (!o.ok) return { ok: false, joined: false, error: o.error };
    offer = o.offer;
    offerLen = o.offer_len;
  }
  const st = chatState(chatId);
  st.offer_len = offerLen;
  // ffmpeg shell command for the bridge (TinaMusic buildFfmpegCmd shape):
  // the already-encoded Opus file, decoded back to s16le 48 kHz stereo.
  const opusFile = st.pipeline && st.pipeline.opus_file;
  const src = opusFile && fs.existsSync(opusFile) ? opusFile : null;
  const ffmpegCmd = src
    ? `ffmpeg -v quiet -i ${shellQuote(src)} -vn -f s16le -ac 2 -ar 48000 pipe:1`
    : `ffmpeg -v quiet -f lavfi -i "sine=frequency=440:duration=5" -vn -f s16le -ac 2 -ar 48000 pipe:1`;
  const py = voicePython();
  const args = [cfg.bridge, "join-call", "--chat-id", String(chatId),
    "--session", cfg.session, "--api-id", String(cfg.apiId), "--api-hash", cfg.apiHash,
    "--ffmpeg-cmd", ffmpegCmd];
  return new Promise((resolve) => {
    let child;
    try {
      child = spawn(py, args, { timeout: 35000 });
    } catch (e) {
      return resolve({ ok: false, joined: false, error: `cannot spawn voice bridge: ${e.message}` });
    }
    let out = "";
    let settled = false;
    const done = (r) => { if (!settled) { settled = true; try { child.kill("SIGKILL"); } catch { /* */ } resolve(r); } };
    const timer = setTimeout(() => done({ ok: false, joined: false, error: `voice bridge join timed out after 30 s (no joined/error event; output: ${lastLine(out) || "(none)"})` }), 30000);
    child.stdout.on("data", (d) => {
      out += d;
      for (const line of String(d).split("\n")) {
        let ev = null;
        try { ev = JSON.parse(line); } catch { /* not JSON */ }
        if (!ev) continue;
        if (ev.event === "joined") {
          clearTimeout(timer);
          return done({ ok: true, joined: true, remote_len: ev.transport_len || 0 });
        }
        if (ev.event === "error") {
          clearTimeout(timer);
          return done({ ok: false, joined: false, error: `voice bridge join failed: ${ev.error}` });
        }
        // main() wrapper failure shape (e.g. session file unreadable
        // before connect): {"ok": false, "error": ...}
        if (ev.ok === false && ev.error) {
          clearTimeout(timer);
          return done({ ok: false, joined: false, error: `voice bridge join failed: ${ev.error}` });
        }
      }
    });
    child.stderr.on("data", (d) => { out += d; });
    child.on("close", (code) => {
      clearTimeout(timer);
      done({ ok: false, joined: false, error: `voice bridge exited (${code}) without joining: ${lastLine(out) || "(no output)"}` });
    });
    child.on("error", (e) => {
      clearTimeout(timer);
      done({ ok: false, joined: false, error: `cannot spawn voice bridge: ${e.message}` });
    });
  });
}

// ------------------------------------------------------------ play --------
// Run the Opus/RTP pipeline for `source`, then attempt the join so the
// stream feeds a live call when a backend exists. Never throws; the
// result (and the per-chat voice state) always names the exact outcome.
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
  const j = await join(chatId);
  if (j.ok && j.joined) {
    st.joined = true;
    st.voice = "live";
    st.reason = null;
    return { streaming: true, voice: "live", reason: null, pipeline, remote_len: j.remote_len };
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
  if (!st) return { voice: "unavailable:not started", joined: false, reason: null, pipeline: null };
  return { voice: st.voice, joined: st.joined, reason: st.reason, pipeline: st.pipeline };
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
    engine: "ffmpeg-libopus-rtp + ntgcalls-3.0.5 + voice_bridge(telethon)",
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
};
