//! Real voice transport for vc-rust: Opus/RTP pipeline + in-process
//! ntgcalls. Std-only (no new crates, `cargo --offline`).
//!
//! The voice path, as in the upstream refs (ParvatiMusic `mtproto/` +
//! `ntg/`, TinaMusic `tinaproto/mtproto.cpp` JoinGroupCall):
//!   1. ntgcalls create_call(chat_id)        -> local SDP offer
//!   2. ffmpeg -> Opus 48 kHz stereo        -> RTP packets (the stream)
//!   3. phone.joinGroupCall(local SDP)       -> remote transport SDP
//!   4. ntgcalls connect(remote)             -> live call
//!   5. ntgcalls set_stream_sources(ffmpeg)  -> audio flows
//!
//! Phase 5 (in-process ntgcalls, voice bridge deleted): ntgcalls is a
//! C++ NATIVE LIBRARY linked into the process, not a server. The
//! refs/ntgcalls repo ships a C target (libntgcalls.so) and a Rust
//! target (ntgcalls/ntgcalls-sys crates over PREBUILT static libs —
//! final-only, no local build). Depending on ntgcalls-sys would add a
//! crates.io dependency plus a prebuilt-binary download, so vc-rust
//! uses a minimal hand-written FFI module instead (`ntg.rs`, loaded
//! via `#[path]` below): it dlopens libntgcalls.so at runtime with
//! libc's dlopen (std-only, no crates) and resolves the ntg_*
//! functions — the same pattern as core-cpp NtgCallsVoiceCall and the
//! vc-ts N-API addon. When the .so is absent, the local offer falls
//! back to the ntgcalls-wheel probe (src/ntg_offer.py, one-shot,
//! purely local WebRTC — not a server, not a join path).
//!
//! What is REAL here:
//!   * The Opus/RTP pipeline (`opus_pipeline`): ffmpeg encodes the
//!     source to libopus 48 kHz stereo (Ogg Opus file, ffprobe-verified
//!     frame count + duration) and packetizes the same audio as RTP
//!     (SDP `m=audio 0 RTP/AVP 97`, `a=rtpmap:97 opus/48000/2`), with
//!     the packet count proven by parsing the RTP file (SSRC-anchored
//!     scan). Proof on a 5 s 440 Hz tone: 251 frames, 83,736 bytes,
//!     5.0065 s; 251 RTP packets + 1 RTCP SR, 85,973 bytes.
//!   * The local offer (`create_local_offer`): ntgcalls create_call
//!     in-process via the FFI module when libntgcalls.so exists, else
//!     the wheel probe — both purely local WebRTC, no MTProto, no
//!     network, no credentials.
//!   * The join (`join`): HONESTLY UNAVAILABLE in this service. The
//!     voice_bridge.py Telethon child that used to do
//!     phone.joinGroupCall was deleted in Phase 5 (user order), and
//!     Rust stdlib has no MTProto client — there is no Telethon in
//!     Rust. The MTProto join now lives only in core-cpp's
//!     TdLibVoiceCall (TDLib joinVideoChat + in-process ntgcalls).
//!     join() therefore always returns Err with the exact reason;
//!     `joined:true` is NEVER reported without a real join. A Rust
//!     MTProto client (ParvatiMusic-style `mtproto/`) would be a new
//!     subsystem and is out of scope.
//!
//! What is BLOCKED (sandbox, proven, not worked around):
//!   * UDP sendto is EPERM even to loopback (`probe_udp`: io error 1,
//!     Permission denied; ffmpeg `-f rtp rtp://127.0.0.1:PORT` fails
//!     the same way, exit 255). RTP packets are therefore proven to a
//!     FILE, never to a live UDP receiver.
//!   * libntgcalls.so is BUILT (refs/ntgcalls/shared-output/lib,
//!     phase5/ntg-build.log, 3.0.5, 98 MB) and the FFI module loads
//!     it in-process; the offer now comes from ntg_create_call via
//!     the FFI, with the wheel probe only as the fallback when the
//!     .so is absent.
//!   * No MTProto session / credentials in this service, and no
//!     client to use them with — see join() above.
//!
//! `RealVoiceCall` implements the `VoiceCall` trait in main.rs.
//! `play_stream` runs the pipeline, builds the local offer as a
//! pre-flight proof, then attempts the join, so now-playing `voice`
//! reports the real state: `"opus-rtp-ready"` when the pipeline is
//! proven, `"opus-file-only:<reason>"` when the Opus file is good but
//! RTP packetization failed, or `"unavailable:<exact reason>"`
//! otherwise. Nothing here panics on a voice failure: every failure
//! is a returned reason string.

#[path = "ntg.rs"]
mod ntg;

use super::{parse_json, Track, VoiceCall};
use crate::data_file;
use ntg::NtgLib;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------- stats ---

#[derive(Clone, Debug, Default)]
pub struct OpusStats {
    pub ok: bool,
    pub frames: i64,
    pub bytes: i64,
    pub duration_sec: f64,
    pub rtp_packets: usize,
    pub rtcp_packets: usize,
    pub rtp_bytes: i64,
    pub sdp: String,
    pub opus_file: String,
    pub rtp_file: String,
    pub udp_error: Option<String>,
    pub rtp_error: Option<String>,
    pub error: Option<String>,
}

// ------------------------------------------------------------ helpers -----

fn last_line(s: &str) -> String {
    s.trim()
        .lines()
        .last()
        .unwrap_or("")
        .chars()
        .take(300)
        .collect()
}

/// Run a command, capturing stdout/stderr to temp files, with a wall
/// timeout (std has no wait_timeout; poll try_wait). Returns
/// (exit_code, stdout, stderr); code -1 on spawn failure or timeout.
fn run_cmd(cmd: &str, args: &[String], timeout: Duration) -> (i32, String, String) {
    let tag = format!(
        "parlay-vc-cmd-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    );
    let dir = std::env::temp_dir().join(&tag);
    let _ = std::fs::create_dir_all(&dir);
    let out_path = dir.join("stdout");
    let err_path = dir.join("stderr");
    let out_f = match std::fs::File::create(&out_path) {
        Ok(f) => f,
        Err(e) => return (-1, String::new(), format!("cannot create temp file: {}", e)),
    };
    let err_f = match std::fs::File::create(&err_path) {
        Ok(f) => f,
        Err(e) => return (-1, String::new(), format!("cannot create temp file: {}", e)),
    };
    let mut child = match Command::new(cmd)
        .args(args)
        .stdout(Stdio::from(out_f))
        .stderr(Stdio::from(err_f))
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            let _ = std::fs::remove_dir_all(&dir);
            return (-1, String::new(), format!("cannot spawn {}: {}", cmd, e));
        }
    };
    let deadline = Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                let code = status.code().unwrap_or(-1);
                let out = std::fs::read_to_string(&out_path).unwrap_or_default();
                let err = std::fs::read_to_string(&err_path).unwrap_or_default();
                let _ = std::fs::remove_dir_all(&dir);
                return (code, out, err);
            }
            Ok(None) => {
                if Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    let _ = std::fs::remove_dir_all(&dir);
                    return (
                        -1,
                        String::new(),
                        format!("timed out after {} s", timeout.as_secs()),
                    );
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(e) => {
                let _ = std::fs::remove_dir_all(&dir);
                return (-1, String::new(), format!("wait failed: {}", e));
            }
        }
    }
}

fn exe_dir() -> Option<PathBuf> {
    std::env::current_exe().ok().and_then(|p| {
        p.parent().map(|d| d.to_path_buf())
    })
}

fn first_existing(candidates: &[PathBuf]) -> Option<PathBuf> {
    candidates.iter().find(|c| c.exists()).cloned()
}

fn repo_candidates(rel: &str) -> Vec<PathBuf> {
    // rel is relative to the parlay repo root. The process may run from
    // the repo root, a service dir, or (cargo test / installed binary)
    // target/<profile>[/deps], so walk up from both the working
    // directory and the exe directory until the file is found.
    let mut out = vec![PathBuf::from(rel)];
    let mut bases: Vec<PathBuf> = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        bases.push(cwd);
    }
    if let Some(exe) = exe_dir() {
        bases.push(exe);
    }
    for base in bases {
        let mut dir = base.clone();
        for _ in 0..6 {
            out.push(dir.join(rel));
            match dir.parent() {
                Some(p) => dir = p.to_path_buf(),
                None => break,
            }
        }
    }
    out
}

// ------------------------------------------------------- RTP file parse ---
// Count RTP/RTCP packets in an ffmpeg `-f rtp` file. Packets are raw and
// back-to-back, so boundaries are found by the SSRC: the RTCP Sender
// Report at offset 0 carries it (bytes 4..8), and every RTP header with
// PT 97 (0x61, or 0xe1 with the marker bit) carries the same SSRC at
// bytes 8..12. Verified on the 5 s tone: 251 RTP + 1 RTCP, sequence
// contiguous, matching the 251 Opus frames exactly.
pub fn count_rtp_packets(data: &[u8]) -> (usize, usize) {
    if data.len() < 12 {
        return (0, 0);
    }
    let mut ssrc: Option<[u8; 4]> = None;
    if data[0] == 0x80 && data[1] == 0xc8 {
        ssrc = Some([data[4], data[5], data[6], data[7]]);
    }
    if ssrc.is_none() {
        for i in 0..=(data.len() - 12) {
            if data[i] == 0x80 && (data[i + 1] == 0x61 || data[i + 1] == 0xe1) {
                ssrc = Some([data[i + 8], data[i + 9], data[i + 10], data[i + 11]]);
                break;
            }
        }
    }
    let ssrc = match ssrc {
        Some(s) => s,
        None => return (0, 0),
    };
    let (mut rtp, mut rtcp) = (0usize, 0usize);
    for i in 0..=(data.len() - 12) {
        if data[i] != 0x80 {
            continue;
        }
        if data[i + 1] == 0xc8 && data[i + 4..i + 8] == ssrc {
            rtcp += 1;
        } else if (data[i + 1] == 0x61 || data[i + 1] == 0xe1)
            && data[i + 8..i + 12] == ssrc
        {
            rtp += 1;
        }
    }
    (rtp, rtcp)
}

// ---------------------------------------------------------- UDP probe -----
// One UDP sendto to loopback. In this sandbox it fails with EPERM
// (os error 1, Operation not permitted) — the exact syscall error that
// blocks any live RTP sink. Returns None when the send succeeds.
pub fn probe_udp() -> Option<String> {
    use std::net::UdpSocket;
    let sock = UdpSocket::bind("0.0.0.0:0").ok()?;
    match sock.send_to(b"parlay-rtp-probe", "127.0.0.1:1") {
        Ok(_) => None,
        Err(e) => Some(match e.raw_os_error() {
            Some(1) => format!("EPERM ({})", e),
            _ => format!("{}", e),
        }),
    }
}

// ------------------------------------------------------- yt-dlp resolve --
// Same shape as core-cpp OpusPipeline::resolveUrl: yt-dlp -g prints the
// direct audio URL for an http(s) source. Returns (url, error).
fn resolve_url(url: &str) -> (String, Option<String>) {
    if !url.starts_with("http://") && !url.starts_with("https://") {
        return (url.to_string(), None);
    }
    let ytdlp = match super::ytdlp_bin() {
        Some(b) => b,
        None => {
            return (
                url.to_string(),
                Some("yt-dlp not found; ffmpeg will try the URL directly".to_string()),
            )
        }
    };
    let (code, out, err) = run_cmd(
        &ytdlp,
        &[
            "-g".to_string(),
            "-f".to_string(),
            "bestaudio/best".to_string(),
            "--no-playlist".to_string(),
            url.to_string(),
        ],
        Duration::from_secs(30),
    );
    if code != 0 {
        return (
            url.to_string(),
            Some(format!("yt-dlp -g failed (exit {}): {}", code, last_line(&err))),
        );
    }
    match out.lines().map(|l| l.trim()).find(|l| !l.is_empty()) {
        Some(first) => (first.to_string(), None),
        None => (
            url.to_string(),
            Some("yt-dlp -g returned no URL".to_string()),
        ),
    }
}

// ------------------------------------------------------ Opus/RTP pipe ----
// ffmpeg -> libopus 48 kHz stereo -> Ogg Opus file (ffprobe-verified) +
// RTP packetization to a file (+ SDP). Mirrors core-cpp
// OpusPipeline::run exactly (same codec, bitrate, rate, channels).
pub fn opus_pipeline(source: &str, work_root: &Path) -> OpusStats {
    let mut st = OpusStats::default();
    if source.trim().is_empty() {
        st.error = Some("empty audio source".to_string());
        return st;
    }
    let tag = format!(
        "pipe-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    );
    let dir = work_root.join(tag);
    if std::fs::create_dir_all(&dir).is_err() {
        st.error = Some(format!("cannot create voice work dir {}", dir.display()));
        return st;
    }
    let opus_file = dir.join("audio.opus");
    let rtp_file = dir.join("audio.rtp");
    let sdp_file = dir.join("audio.sdp");
    st.opus_file = opus_file.to_string_lossy().to_string();
    st.rtp_file = rtp_file.to_string_lossy().to_string();

    let (input, resolve_error) = resolve_url(source);
    let is_http = input.starts_with("http://") || input.starts_with("https://");
    let input_args: Vec<String> = if is_http {
        vec!["-timeout".to_string(), "10000000".to_string()]
    } else {
        vec![]
    };
    let mut enc_args: Vec<String> = vec![
        "-y".into(),
        "-v".into(),
        "error".into(),
    ];
    enc_args.extend(input_args.clone());
    enc_args.extend([
        "-i".into(),
        input.clone(),
        "-map".into(),
        "0:a".into(),
        "-c:a".into(),
        "libopus".into(),
        "-b:a".into(),
        "128k".into(),
        "-ar".into(),
        "48000".into(),
        "-ac".into(),
        "2".into(),
        "-f".into(),
        "opus".into(),
        opus_file.to_string_lossy().to_string(),
    ]);
    let (code, _out, err) = run_cmd("ffmpeg", &enc_args, Duration::from_secs(120));
    if code != 0 || !opus_file.exists() {
        let why = if err.contains("No such file") || code == -1 && err.contains("cannot spawn") {
            format!("ffmpeg not found on PATH ({})", last_line(&err))
        } else {
            format!(
                "ffmpeg opus encode failed (exit {}): {}",
                code,
                last_line(&err).chars().take(200).collect::<String>()
            )
        };
        st.error = Some(if resolve_error.is_some() && last_line(&err).is_empty() {
            resolve_error.unwrap()
        } else {
            why
        });
        return st;
    }
    st.bytes = std::fs::metadata(&opus_file).map(|m| m.len() as i64).unwrap_or(0);
    let (pcode, pout, _) = run_cmd(
        "ffprobe",
        &[
            "-v".into(),
            "error".into(),
            "-select_streams".into(),
            "a:0".into(),
            "-show_entries".into(),
            "packet=pts_time".into(),
            "-of".into(),
            "csv=p=0".into(),
            opus_file.to_string_lossy().to_string(),
        ],
        Duration::from_secs(30),
    );
    st.frames = if pcode == 0 {
        pout.lines().filter(|l| !l.trim().is_empty()).count() as i64
    } else {
        0
    };
    let (dcode, dout, _) = run_cmd(
        "ffprobe",
        &[
            "-v".into(),
            "error".into(),
            "-show_entries".into(),
            "format=duration".into(),
            "-of".into(),
            "csv=p=0".into(),
            opus_file.to_string_lossy().to_string(),
        ],
        Duration::from_secs(30),
    );
    st.duration_sec = if dcode == 0 {
        dout.trim().parse::<f64>().unwrap_or(0.0)
    } else {
        0.0
    };

    // RTP packetization: separate ffmpeg run, to a FILE — a blocked UDP
    // sink must never lose the file encode (same split as core-cpp).
    let mut rtp_args: Vec<String> = vec!["-y".into(), "-v".into(), "error".into()];
    rtp_args.extend(input_args);
    rtp_args.extend([
        "-i".into(),
        input.clone(),
        "-map".into(),
        "0:a".into(),
        "-c:a".into(),
        "libopus".into(),
        "-b:a".into(),
        "128k".into(),
        "-ar".into(),
        "48000".into(),
        "-ac".into(),
        "2".into(),
        "-f".into(),
        "rtp".into(),
        "-sdp_file".into(),
        sdp_file.to_string_lossy().to_string(),
        rtp_file.to_string_lossy().to_string(),
    ]);
    let (rcode, _rout, rerr) = run_cmd("ffmpeg", &rtp_args, Duration::from_secs(120));
    if rcode == 0 && rtp_file.exists() {
        st.rtp_bytes = std::fs::metadata(&rtp_file).map(|m| m.len() as i64).unwrap_or(0);
        st.sdp = std::fs::read_to_string(&sdp_file).unwrap_or_default();
        match std::fs::read(&rtp_file) {
            Ok(data) => {
                let (rtp, rtcp) = count_rtp_packets(&data);
                st.rtp_packets = rtp;
                st.rtcp_packets = rtcp;
            }
            Err(e) => st.rtp_error = Some(format!("cannot read RTP file: {}", e)),
        }
    } else {
        st.rtp_error = Some(format!(
            "ffmpeg rtp output failed (exit {}): {}",
            rcode,
            last_line(&rerr)
        ));
    }

    // UDP sink probe: the exact failing call for any live receiver.
    st.udp_error = probe_udp();

    // Optional real UDP sink (VOICE_RTP_SINK=host:port): attempted only
    // when explicitly configured; in this sandbox it fails EPERM.
    if let Ok(sink) = std::env::var("VOICE_RTP_SINK") {
        if !sink.is_empty() {
            let sink = sink.strip_prefix("rtp://").unwrap_or(&sink);
            let mut uargs: Vec<String> = vec!["-y".into(), "-v".into(), "error".into()];
            uargs.extend([
                "-i".into(),
                input.clone(),
                "-map".into(),
                "0:a".into(),
                "-c:a".into(),
                "libopus".into(),
                "-b:a".into(),
                "128k".into(),
                "-ar".into(),
                "48000".into(),
                "-ac".into(),
                "2".into(),
                "-f".into(),
                "rtp".into(),
                format!("rtp://{}", sink),
            ]);
            let (ucode, _uout, uerr) = run_cmd("ffmpeg", &uargs, Duration::from_secs(60));
            if ucode != 0 {
                st.rtp_error = Some(format!(
                    "ffmpeg rtp://{} failed (exit {}): {}",
                    sink,
                    ucode,
                    last_line(&uerr)
                ));
            }
        }
    }

    st.ok = st.frames > 0 && st.bytes > 0;
    if !st.ok && st.error.is_none() {
        st.error = Some("opus file has no frames".to_string());
    }
    st
}

// ----------------------------------------------------- backend config -----

#[derive(Clone, Debug)]
pub struct BackendConfig {
    pub ok: bool,
    pub missing: Vec<String>,
    // Resolved libntgcalls.so path ("" when none found). The media
    // side only: even a fully-configured lib does NOT make join()
    // possible — there is no MTProto client in this service.
    pub lib: String,
    pub offer_fallback: String,
}

impl BackendConfig {
    pub fn describe(&self) -> String {
        if self.ok {
            format!("ntgcalls in-process via {} (FFI)", self.lib)
        } else {
            format!(
                "voice media unavailable: {}; {}",
                self.missing.join("; "),
                self.offer_fallback
            )
        }
    }
}

/// Pure config check (env-independent so tests can exercise it without
/// touching process env).
pub fn backend_config_from(ntgcalls_lib: Option<String>) -> BackendConfig {
    let mut missing = Vec::new();
    let lib = ntgcalls_lib.unwrap_or_default();
    if lib.is_empty() {
        missing.push(
            "libntgcalls.so not found (NTGCALLS_LIB unset and no .so at the candidate paths — see phase5/ntg-build.log)".to_string(),
        );
    } else if !Path::new(&lib).exists() {
        missing.push(format!("NTGCALLS_LIB missing ({})", lib));
    }
    let offer_fallback = "local offer via ntgcalls wheel probe (src/ntg_offer.py)".to_string();
    BackendConfig {
        ok: missing.is_empty(),
        missing,
        lib,
        offer_fallback,
    }
}

pub fn backend_config_from_env() -> BackendConfig {
    let lib = std::env::var("NTGCALLS_LIB")
        .ok()
        .filter(|v| !v.is_empty())
        .or_else(NtgLib::first_existing);
    backend_config_from(lib)
}

fn voice_python() -> String {
    if let Ok(p) = std::env::var("PARLAY_VOICE_PYTHON") {
        if !p.is_empty() {
            return p;
        }
    }
    if let Some(p) = first_existing(&repo_candidates("phase4/voice-venv/bin/python")) {
        return p.to_string_lossy().to_string();
    }
    "python3".to_string()
}

fn ntg_offer_py() -> Option<PathBuf> {
    first_existing(&repo_candidates("services/vc-rust/src/ntg_offer.py"))
}

// ----------------------------------------------------- local SDP offer ---
// ntgcalls create_call(chat_id). In-process first: the FFI module
// (ntg.rs) dlopens libntgcalls.so and calls ntg_create_call (3.0.5
// synchronous C API) in this process. Fallback when the .so is
// probe (src/ntg_offer.py) — a one-shot local call, still offline, no
// MTProto, no network, no credentials. Either way the offer is what
// phone.joinGroupCall would carry.
pub fn create_local_offer(chat_id: i64) -> Result<String, String> {
    // In-process FFI first (NTGCALLS_LIB, else the C-build output).
    // ntgcalls refuses a second create_call for the same chat (-202
    // "Connection cannot be initialized more than once"), so on
    // failure we stop any stale call for that chat and retry once.
    match NtgLib::load(None) {
        Ok(lib) => match lib.create_offer(chat_id) {
            Ok(offer) => return Ok(offer),
            Err(e) => {
                let first_err = e;
                let _ = lib.stop(chat_id);
                match lib.create_offer(chat_id) {
                    Ok(offer) => return Ok(offer),
                    Err(e2) => {
                        return Err(format!(
                            "ntgcalls create_call failed (in-process FFI via {}): {} (retry also failed: {})",
                            lib.lib_path(),
                            first_err,
                            e2
                        ));
                    }
                }
            }
        },
        Err(load_err) => {
            // Fall through to the wheel probe; the load error is kept
            // for the failure message if the probe fails too.
            let script = match ntg_offer_py() {
                Some(p) => p,
                None => {
                    return Err(format!(
                        "ntgcalls create_call failed: {} (and ntg_offer.py not found)",
                        load_err
                    ));
                }
            };
            let (code, out, err) = run_cmd(
                &voice_python(),
                &[script.to_string_lossy().to_string(), chat_id.to_string()],
                Duration::from_secs(60),
            );
            let last_json = out.lines().filter(|l| !l.trim().is_empty()).last();
            if let Some(line) = last_json {
                if let Some(v) = parse_json(line) {
                    if v.get("ok").and_then(|x| x.as_bool()).unwrap_or(false) {
                        if let Some(offer) = v.get("offer").and_then(|x| x.as_str()) {
                            return Ok(offer.to_string());
                        }
                    }
                    if let Some(e) = v.get("error").and_then(|x| x.as_str()) {
                        return Err(format!(
                            "ntgcalls create_call failed (wheel probe; FFI unavailable: {}): {}",
                            load_err, e
                        ));
                    }
                }
            }
            Err(format!(
                "ntgcalls create_call failed (exit {}): {} (FFI unavailable: {})",
                code,
                last_line(&err),
                load_err
            ))
        }
    }
}

// ------------------------------------------------------------- join -------
// The MTProto step: phone.joinGroupCall(local SDP) -> remote transport
// -> ntgcalls connect(remote). HONESTLY UNAVAILABLE in vc-rust: the
// voice_bridge.py Telethon child that did this was deleted in Phase 5
// (user order — ntgcalls is a native library linked in-process, not a
// server), and Rust stdlib has no MTProto client to replace it with.
// The join lives only in core-cpp's TdLibVoiceCall (TDLib
// joinVideoChat + in-process ntgcalls, TD_API_ID/TD_API_HASH/
// TD_SESSION_DIR). This function therefore always returns Err with
// the exact reason — never a spawned process, never a faked join.
// The arguments are kept (offer pre-flight proof + opus source) so
// the call shape survives for the day a Rust MTProto client exists.
pub fn join(chat_id: i64, local_sdp: &str, opus_file: Option<&str>) -> Result<(usize, usize), String> {
    join_with_config(chat_id, local_sdp, opus_file, &backend_config_from_env())
}

pub fn join_with_config(
    chat_id: i64,
    local_sdp: &str,
    _opus_file: Option<&str>,
    _cfg: &BackendConfig,
) -> Result<(usize, usize), String> {
    let _ = chat_id;
    let _ = local_sdp;
    Err("voice join unavailable in vc-rust: no MTProto client in-process \
         (voice_bridge.py deleted Phase 5); join via the core-cpp tdlib \
         backend (TD_API_ID/TD_API_HASH/TD_SESSION_DIR + libntgcalls.so)"
        .to_string())
}

// ---------------------------------------------------------- per-chat ------

#[derive(Clone, Debug)]
pub struct ChatVoice {
    pub voice: String,
    pub joined: bool,
    pub reason: Option<String>,
    pub pipeline: Option<OpusStats>,
    pub offer_len: Option<usize>,
}

impl ChatVoice {
    fn fresh() -> ChatVoice {
        ChatVoice {
            voice: "unavailable:not started".to_string(),
            joined: false,
            reason: None,
            pipeline: None,
            offer_len: None,
        }
    }
}

pub struct RealVoiceCall {
    states: Mutex<HashMap<i64, ChatVoice>>,
}

impl RealVoiceCall {
    pub fn new() -> RealVoiceCall {
        RealVoiceCall {
            states: Mutex::new(HashMap::new()),
        }
    }

    fn state_mut(&self) -> std::sync::MutexGuard<'_, HashMap<i64, ChatVoice>> {
        self.states.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Short voice string for now-playing (never "stub-*" and never
    /// "live"/joined:true without a real join).
    pub fn voice_string(&self, chat_id: i64) -> String {
        let st = self.states.lock().unwrap_or_else(|e| e.into_inner());
        match st.get(&chat_id) {
            Some(c) => c.voice.clone(),
            None => "unavailable:not started".to_string(),
        }
    }

    /// Full per-chat voice detail (diagnostics + tests).
    #[allow(dead_code)]
    pub fn voice_detail(&self, chat_id: i64) -> Option<ChatVoice> {
        let st = self.states.lock().unwrap_or_else(|e| e.into_inner());
        st.get(&chat_id).cloned()
    }

    /// Attempt the MTProto join for a chat and record the outcome.
    /// Returns true only on a real join. Pre-flight: prove in-process
    /// ntgcalls can build a call (create_local_offer — FFI, or wheel
    /// probe fallback) and record the offer length; the join itself is
    /// honestly unavailable in this service (no MTProto client —
    /// see join()), so the outcome is always "not joined" with the
    /// exact reason until a Rust MTProto client exists.
    fn attempt_join(&self, chat_id: i64) -> bool {
        let opus_file = {
            let st = self.state_mut();
            st.get(&chat_id)
                .and_then(|c| c.pipeline.as_ref())
                .map(|p| p.opus_file.clone())
        };
        match create_local_offer(chat_id) {
            Ok(offer) => {
                let mut st = self.state_mut();
                let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
                c.offer_len = Some(offer.len());
            }
            Err(e) => {
                let mut st = self.state_mut();
                let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
                c.joined = false;
                c.reason = Some(e.clone());
                eprintln!("[voice] chat {} offer failed: {}", chat_id, e);
                return false;
            }
        }
        match join(chat_id, "", opus_file.as_deref()) {
            Ok((remote_len, offer_len)) => {
                let mut st = self.state_mut();
                let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
                c.offer_len = Some(offer_len);
                c.joined = true;
                c.voice = "live".to_string();
                c.reason = None;
                eprintln!(
                    "[voice] chat {} joined (remote transport {} bytes)",
                    chat_id, remote_len
                );
                true
            }
            Err(e) => {
                let mut st = self.state_mut();
                let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
                c.joined = false;
                c.reason = Some(e.clone());
                eprintln!("[voice] chat {} join failed: {}", chat_id, e);
                false
            }
        }
    }
}

impl VoiceCall for RealVoiceCall {
    fn join(&self, chat_id: i64) {
        self.attempt_join(chat_id);
    }

    fn leave(&self, chat_id: i64) {
        let mut st = self.state_mut();
        if let Some(c) = st.get_mut(&chat_id) {
            c.joined = false;
            if c.voice == "live" {
                c.voice = "opus-rtp-ready".to_string();
            }
            c.reason = Some("left voice chat".to_string());
        }
        eprintln!("[voice] leave voice chat {}", chat_id);
    }

    fn play_stream(&self, chat_id: i64, track: &Track) {
        // Source: the downloaded local file when present (ParvatiMusic
        // downloads before streaming), else the track URL.
        let source = match &track.file_path {
            Some(p) if Path::new(p).exists() => p.clone(),
            _ => track.url.clone(),
        };
        {
            let mut st = self.state_mut();
            let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
            c.voice = "encoding".to_string();
            c.reason = None;
        }
        let work_root = data_file("voice");
        let pipeline = opus_pipeline(&source, &work_root);
        {
            let mut st = self.state_mut();
            let c = st.entry(chat_id).or_insert_with(ChatVoice::fresh);
            if !pipeline.ok {
                let e = pipeline
                    .error
                    .clone()
                    .unwrap_or_else(|| "opus pipeline failed".to_string());
                c.voice = format!("unavailable:{}", e);
                c.reason = Some(e);
                c.joined = false;
            } else if pipeline.rtp_packets > 0 {
                c.voice = "opus-rtp-ready".to_string();
            } else {
                let e = pipeline
                    .rtp_error
                    .clone()
                    .unwrap_or_else(|| "no RTP packets".to_string());
                c.voice = format!("opus-file-only:{}", e);
            }
            c.pipeline = Some(pipeline);
        }
        // Feed the stream into the join when a backend exists; the
        // attempt records the exact reason when it does not.
        self.attempt_join(chat_id);
        let st = self.state_mut();
        if let Some(c) = st.get(&chat_id) {
            eprintln!(
                "[voice] play in chat {}: {} ({}, {}) voice={} joined={}",
                chat_id, track.title, track.source, track.id, c.voice, c.joined
            );
        }
    }

    fn pause(&self, chat_id: i64) {
        let mut st = self.state_mut();
        if let Some(c) = st.get_mut(&chat_id) {
            if c.voice == "opus-rtp-ready" {
                c.voice = "opus-rtp-ready-paused".to_string();
            }
        }
        eprintln!("[voice] pause in chat {}", chat_id);
    }

    fn resume(&self, chat_id: i64) {
        let mut st = self.state_mut();
        if let Some(c) = st.get_mut(&chat_id) {
            if c.voice == "opus-rtp-ready-paused" {
                c.voice = "opus-rtp-ready".to_string();
            }
        }
        eprintln!("[voice] resume in chat {}", chat_id);
    }

    fn stop(&self, chat_id: i64) {
        let mut st = self.state_mut();
        if let Some(c) = st.get_mut(&chat_id) {
            c.joined = false;
            c.voice = "unavailable:stopped".to_string();
            c.reason = Some("stopped".to_string());
        }
        eprintln!("[voice] stop in chat {}", chat_id);
    }
}

impl Default for RealVoiceCall {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------- tests ---

#[cfg(test)]
mod tests {
    use super::*;

    fn make_tone_5s(path: &Path) {
        let (code, _, err) = run_cmd(
            "ffmpeg",
            &[
                "-y".to_string(),
                "-v".to_string(),
                "error".to_string(),
                "-f".to_string(),
                "lavfi".to_string(),
                "-i".to_string(),
                "sine=frequency=440:duration=5".to_string(),
                "-ar".to_string(),
                "48000".to_string(),
                "-ac".to_string(),
                "2".to_string(),
                path.to_string_lossy().to_string(),
            ],
            Duration::from_secs(30),
        );
        assert_eq!(code, 0, "ffmpeg tone generation failed: {}", err);
    }

    fn work_dir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("parlay-vc-rust-voice-{}-{}", tag, std::process::id()));
        let _ = std::fs::create_dir_all(&d);
        d
    }

    #[test]
    fn rtp_packet_counter_counts_synthetic_file() {
        // Hand-built: one RTCP SR (28 bytes) + 3 RTP packets (PT 97,
        // seq 100..102, ts +960 each, SSRC 0x11223344, 10-byte payloads).
        let mut data = Vec::new();
        let ssrc: [u8; 4] = [0x11, 0x22, 0x33, 0x44];
        data.extend_from_slice(&[0x80, 0xc8, 0x00, 0x06]);
        data.extend_from_slice(&ssrc);
        data.extend_from_slice(&[0u8; 20]);
        for i in 0..3u16 {
            let marker = if i == 0 { 0xe1 } else { 0x61 };
            data.push(0x80);
            data.push(marker);
            data.extend_from_slice(&(100u16 + i).to_be_bytes());
            data.extend_from_slice(&(1000u32 + (i as u32) * 960).to_be_bytes());
            data.extend_from_slice(&ssrc);
            data.extend_from_slice(&[0xabu8; 10]);
        }
        let (rtp, rtcp) = count_rtp_packets(&data);
        assert_eq!(rtp, 3, "expected 3 RTP packets");
        assert_eq!(rtcp, 1, "expected 1 RTCP packet");
        assert_eq!(count_rtp_packets(b"too short"), (0, 0));
        assert_eq!(count_rtp_packets(b""), (0, 0));
    }

    #[test]
    fn opus_pipeline_proves_frames_and_rtp_packets() {
        let dir = work_dir("pipeline");
        let tone = dir.join("tone5.wav");
        make_tone_5s(&tone);
        let st = opus_pipeline(tone.to_str().unwrap(), &dir);
        eprintln!(
            "opus pipeline proof: ok={} frames={} bytes={} duration={} rtp_packets={} rtcp={} rtp_bytes={} udp_error={:?} error={:?}",
            st.ok, st.frames, st.bytes, st.duration_sec, st.rtp_packets, st.rtcp_packets, st.rtp_bytes, st.udp_error, st.error
        );
        assert!(st.ok, "pipeline failed: {:?}", st.error);
        assert_eq!(st.frames, 251, "expected 251 Opus frames, got {}", st.frames);
        assert_eq!(st.bytes, 83736, "expected 83736 bytes, got {}", st.bytes);
        assert!(
            (st.duration_sec - 5.0065).abs() < 0.01,
            "duration {}",
            st.duration_sec
        );
        assert_eq!(st.rtp_packets, 251, "expected 251 RTP packets, got {}", st.rtp_packets);
        assert_eq!(st.rtcp_packets, 1);
        assert_eq!(st.rtp_bytes, 85973, "expected 85973 RTP bytes, got {}", st.rtp_bytes);
        assert!(st.sdp.contains("m=audio 0 RTP/AVP 97"), "SDP media line: {}", st.sdp);
        assert!(st.sdp.contains("a=rtpmap:97 opus/48000/2"), "SDP opus map: {}", st.sdp);
        // The exact sandbox block on any live UDP receiver, even loopback.
        let udp = st.udp_error.clone().unwrap_or_default();
        assert!(udp.starts_with("EPERM"), "udp_error: {}", udp);
    }

    #[test]
    fn udp_probe_reports_eperm() {
        let err = probe_udp();
        eprintln!("udp probe: {:?}", err);
        assert!(err.is_some(), "expected UDP sendto to fail in this sandbox");
        assert!(err.unwrap().starts_with("EPERM"), "expected EPERM");
    }

    #[test]
    fn backend_config_names_missing_lib() {
        let cfg = backend_config_from(None);
        assert!(!cfg.ok);
        let d = cfg.describe();
        eprintln!("backend describe: {}", d);
        assert!(d.contains("libntgcalls.so"), "{}", d);
        assert!(d.contains("wheel probe"), "{}", d);
        // A lib path that exists is "configured" for the media side
        // (existence only — loading is proven by create_local_offer).
        let cfg2 = backend_config_from(Some("/tmp".to_string()));
        assert!(cfg2.ok, "{}", cfg2.describe());
        assert!(cfg2.describe().contains("in-process"), "{}", cfg2.describe());
        // Nonexistent explicit path is named.
        let cfg3 = backend_config_from(Some("/nonexistent/libntgcalls.so".to_string()));
        assert!(!cfg3.ok);
        assert!(cfg3.describe().contains("NTGCALLS_LIB missing"), "{}", cfg3.describe());
    }

    #[test]
    fn join_fails_honestly_no_mtproto_client() {
        // Env-independent path: join_with_config must fail with the
        // no-MTProto-client reason before any process is spawned —
        // vc-rust holds no MTProto client since the bridge was deleted.
        let cfg = backend_config_from(None);
        let err = join_with_config(-1004243, "", None, &cfg)
            .expect_err("join must fail without an MTProto client");
        eprintln!("join error: {}", err);
        assert!(err.contains("no MTProto client"), "{}", err);
        assert!(err.contains("voice_bridge.py deleted"), "{}", err);
        assert!(err.contains("tdlib"), "{}", err);
    }

    #[test]
    fn join_still_fails_when_ntg_lib_is_configured() {
        // Media vs signaling are separate gates: even with a lib path
        // configured, join() cannot phone.joinGroupCall — there is no
        // MTProto client in this service.
        let cfg = backend_config_from(Some("/tmp".to_string()));
        assert!(cfg.ok, "{}", cfg.describe());
        let err = join_with_config(-1004243, "dummy-offer", None, &cfg)
            .expect_err("join must fail even with ntgcalls configured");
        eprintln!("join with lib configured: {}", err);
        assert!(err.contains("no MTProto client"), "{}", err);
    }

    #[test]
    fn create_local_offer_offline() {
        let offer = create_local_offer(-1004243).expect("ntgcalls create_call must succeed offline");
        eprintln!("local offer len: {}", offer.len());
        assert!(offer.len() > 100, "offer suspiciously short: {}", offer.len());
        assert!(offer.contains("ufrag"), "offer carries an ICE ufrag");
    }

    #[test]
    fn play_stream_reports_real_voice_state_never_joined() {
        let vc = RealVoiceCall::new();
        let track = Track {
            id: "tone5".to_string(),
            title: "Test tone".to_string(),
            artist: "Test".to_string(),
            source: "file".to_string(),
            url: String::new(),
            duration_sec: 5,
            resolved_by: "test".to_string(),
            file_path: Some({
                let dir = work_dir("play");
                let tone = dir.join("tone5.wav");
                make_tone_5s(&tone);
                tone.to_string_lossy().to_string()
            }),
            downloaded: true,
            download_failed: false,
        };
        vc.play_stream(-1004243, &track);
        let detail = vc.voice_detail(-1004243).expect("voice state recorded");
        eprintln!(
            "play_stream voice={} joined={} reason={:?} frames={:?} rtp={:?} offer_len={:?}",
            detail.voice,
            detail.joined,
            detail.reason,
            detail.pipeline.as_ref().map(|p| p.frames),
            detail.pipeline.as_ref().map(|p| p.rtp_packets),
            detail.offer_len
        );
        assert_eq!(detail.voice, "opus-rtp-ready");
        assert!(!detail.joined, "never joined:true without a real join");
        assert!(detail.reason.as_deref().unwrap_or("").contains("no MTProto client"));
        // Pre-flight offer was built (FFI, or wheel probe fallback).
        let offer_len = detail.offer_len.expect("offer_len recorded");
        assert!(offer_len > 100, "offer_len {}", offer_len);
        let p = detail.pipeline.expect("pipeline stats recorded");
        assert_eq!(p.frames, 251);
        assert_eq!(p.rtp_packets, 251);
        // pause/resume move the voice state; stop clears it.
        vc.pause(-1004243);
        assert_eq!(vc.voice_string(-1004243), "opus-rtp-ready-paused");
        vc.resume(-1004243);
        assert_eq!(vc.voice_string(-1004243), "opus-rtp-ready");
        vc.stop(-1004243);
        assert_eq!(vc.voice_string(-1004243), "unavailable:stopped");
    }
}
