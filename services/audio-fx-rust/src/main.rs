//! parlay-audio-fx — POST /v1/fx.
//!
//! Audio effects ported (behaviour only, re-implemented) from
//! refs/telegram-music-bot-rust `main.rs`, which applies FFmpeg filter
//! chains to a downloaded track:
//!   slow      -> atempo=0.85, aecho=0.8:0.88:60:0.4
//!   nightcore -> asetrate=44100*1.2, atempo=1/1.2
//!   bass      -> bass=g=12:f=110:w=0.6, volume=0.9
//!   8d        -> apulsator=mode=sine:hz=0.12, aecho=0.8:0.88:40:0.3
//!
//! Parlay exposes the same effects through the IPC contract
//! (contracts/ipc.json `fx.apply`): POST /v1/fx
//!   {effect, input_path, output_path?, requested_by?}
//! -> {ok:true, data:{output_path}}  (or the {ok:false, error:{...}} envelope)
//!
//! Real work: a real `ffmpeg` subprocess. No bot logic in this service.

mod common;

use common::*;
use std::path::{Component, Path};
use std::process::Command;

const SERVICE: &str = "audio-fx-rust";

/// Reject path traversal: any `..` component is refused. Absolute paths
/// are allowed (callers pass real file locations, e.g. /tmp/...).
fn sanitize_path(p: &str) -> Result<std::path::PathBuf, String> {
    if p.is_empty() {
        return Err("empty path".to_string());
    }
    if p.contains('\0') {
        return Err("NUL byte in path".to_string());
    }
    let path = Path::new(p);
    for comp in path.components() {
        if matches!(comp, Component::ParentDir) {
            return Err(format!("path traversal rejected: {}", p));
        }
    }
    Ok(path.to_path_buf())
}

/// FFmpeg -af filter per effect. Values follow the reference bot's chains
/// (see module docs); task spec pins nightcore at 1.25x and bass g=10.
fn filter_for(effect: &str) -> Option<&'static str> {
    match effect {
        "slow-reverb" => Some("atempo=0.8,aecho=0.8:0.88:60:0.4"),
        "nightcore" => Some("asetrate=44100*1.25,atempo=1/1.25"),
        "bass-boost" => Some("bass=g=10:f=110:w=0.6,volume=0.9"),
        "8d" => Some("apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3"),
        "original" => Some(""),
        _ => None,
    }
}

/// Default output: same dir as input, "<stem>_<effect>.<ext>".
fn derive_output(input: &Path, effect: &str) -> std::path::PathBuf {
    let stem = input
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "out".to_string());
    let ext = input
        .extension()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "wav".to_string());
    input.with_file_name(format!("{}_{}.{}", stem, effect, ext))
}

fn handle(req: &Request) -> (u16, String) {
    if req.method != "POST" || req.path != "/v1/fx" {
        return (404, err("not_found", "route not found (POST /v1/fx)"));
    }
    let body = match parse_json(&req.body) {
        Some(b @ Json::Obj(_)) => b,
        _ => return (400, err("bad_request", "invalid JSON body")),
    };
    let effect = match body.get("effect").and_then(|v| v.as_str()) {
        Some(e) => e.to_string(),
        None => return (400, err("bad_request", "missing string field: effect")),
    };
    let filter = match filter_for(&effect) {
        Some(f) => f,
        None => {
            return (
                400,
                err(
                    "bad_request",
                    "unknown effect (slow-reverb|nightcore|bass-boost|8d|original)",
                ),
            )
        }
    };
    let input_raw = match body.get("input_path").and_then(|v| v.as_str()) {
        Some(p) => p.to_string(),
        None => return (400, err("bad_request", "missing string field: input_path")),
    };
    let input = match sanitize_path(&input_raw) {
        Ok(p) => p,
        Err(e) => return (400, err("bad_request", &e)),
    };
    if !input.is_file() {
        return (
            400,
            err("bad_request", &format!("input file not found: {}", input_raw)),
        );
    }
    let output = match body.get("output_path").and_then(|v| v.as_str()) {
        Some(o) => match sanitize_path(o) {
            Ok(p) => p,
            Err(e) => return (400, err("bad_request", &e)),
        },
        None => derive_output(&input, &effect),
    };

    let mut cmd = Command::new("ffmpeg");
    cmd.arg("-y").arg("-i").arg(&input);
    if filter.is_empty() {
        // "original": stream copy, no re-encode.
        cmd.arg("-c:a").arg("copy");
    } else {
        cmd.arg("-af").arg(filter);
    }
    cmd.arg(&output);
    let out = match cmd.output() {
        Ok(o) => o,
        Err(e) => {
            return (
                500,
                err(
                    "engine_unavailable",
                    &format!("failed to run ffmpeg: {}", e),
                ),
            )
        }
    };
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let tail: String = stderr
            .chars()
            .rev()
            .take(500)
            .collect::<String>()
            .chars()
            .rev()
            .collect();
        return (
            500,
            err("internal", &format!("ffmpeg failed: {}", tail.trim())),
        );
    }
    let out_str = output.to_string_lossy().into_owned();
    (
        200,
        ok(&format!("{{\"output_path\":{}}}", quote(&out_str))),
    )
}

fn main() {
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(8003);
    if let Err(e) = run_server(port, SERVICE, handle) {
        eprintln!("parlay {} failed to listen on {}: {}", SERVICE, port, e);
        std::process::exit(1);
    }
}
