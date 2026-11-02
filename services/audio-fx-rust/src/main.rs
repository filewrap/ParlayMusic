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

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    fn req(method: &str, path: &str, body: &str) -> Request {
        Request {
            method: method.to_string(),
            path: path.to_string(),
            headers: Vec::new(),
            body: body.to_string(),
        }
    }

    /// Unique temp dir per test (tests run in parallel in one process).
    fn tmpdir(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir()
            .join(format!("parlay-fx-test-{}-{}", std::process::id(), name));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("create tmpdir");
        dir
    }

    /// 1 s 440 Hz sine WAV fixture, via real ffmpeg.
    fn sine_wav(dir: &std::path::Path) -> std::path::PathBuf {
        let path = dir.join("tone.wav");
        let out = Command::new("ffmpeg")
            .args(["-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
                   "-ac", "1", "-ar", "44100"])
            .arg(&path)
            .output()
            .expect("run ffmpeg");
        assert!(out.status.success(), "ffmpeg fixture failed: {}",
                String::from_utf8_lossy(&out.stderr));
        assert!(path.is_file(), "fixture wav missing");
        path
    }

    fn data_field(body: &str, key: &str) -> Json {
        let v = parse_json(body).expect("response parses");
        assert_eq!(v.get("ok").and_then(|x| x.as_bool()), Some(true), "ok:true in {body}");
        v.get("data").and_then(|d| d.get(key)).cloned().expect("data field present")
    }

    #[test]
    fn filter_for_known_effects() {
        for effect in ["slow-reverb", "nightcore", "bass-boost", "8d", "original"] {
            assert!(filter_for(effect).is_some(), "{effect} has a filter");
        }
        assert!(filter_for("chipmunk").is_none());
        assert!(filter_for("").is_none());
    }

    #[test]
    fn derive_output_naming() {
        let out = derive_output(Path::new("/tmp/track.mp3"), "nightcore");
        assert_eq!(out.to_string_lossy(), "/tmp/track_nightcore.mp3");
        let out = derive_output(Path::new("song.wav"), "8d");
        assert_eq!(out.to_string_lossy(), "song_8d.wav");
    }

    #[test]
    fn sanitize_rejects_traversal() {
        assert!(sanitize_path("../secret.wav").is_err());
        assert!(sanitize_path("/tmp/../etc/passwd").is_err());
        assert!(sanitize_path("").is_err());
        assert!(sanitize_path("/tmp/tone.wav").is_ok());
    }

    #[test]
    fn handle_rejects_bad_requests() {
        // Wrong route / method.
        let (st, _) = handle(&req("GET", "/v1/fx", ""));
        assert_eq!(st, 404);
        let (st, _) = handle(&req("POST", "/v1/other", "{}"));
        assert_eq!(st, 404);
        // Invalid JSON, unknown effect, missing input file.
        let (st, _) = handle(&req("POST", "/v1/fx", "not json"));
        assert_eq!(st, 400);
        let (st, _) = handle(&req(
            "POST", "/v1/fx",
            r#"{"effect":"chipmunk","input_path":"/tmp/tone.wav"}"#,
        ));
        assert_eq!(st, 400);
        let (st, _) = handle(&req(
            "POST", "/v1/fx",
            r#"{"effect":"nightcore","input_path":"/tmp/no-such-file.wav"}"#,
        ));
        assert_eq!(st, 400);
        let (st, _) = handle(&req("POST", "/v1/fx", r#"{"effect":"nightcore"}"#));
        assert_eq!(st, 400);
    }

    #[test]
    fn fx_nightcore_produces_nonempty_output() {
        let dir = tmpdir("nightcore");
        let input = sine_wav(&dir);
        let body = format!(
            r#"{{"effect":"nightcore","input_path":{}}}"#,
            quote(&input.to_string_lossy())
        );
        let (st, resp) = handle(&req("POST", "/v1/fx", &body));
        assert_eq!(st, 200, "nightcore: {resp}");
        let out = data_field(&resp, "output_path");
        let out_path = std::path::PathBuf::from(out.as_str().unwrap());
        assert!(out_path.is_file(), "output file exists: {}", out_path.display());
        assert!(std::fs::metadata(&out_path).unwrap().len() > 0, "output non-empty");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn fx_original_stream_copy_produces_nonempty_output() {
        let dir = tmpdir("original");
        let input = sine_wav(&dir);
        let body = format!(
            r#"{{"effect":"original","input_path":{}}}"#,
            quote(&input.to_string_lossy())
        );
        let (st, resp) = handle(&req("POST", "/v1/fx", &body));
        assert_eq!(st, 200, "original: {resp}");
        let out = data_field(&resp, "output_path");
        let out_path = std::path::PathBuf::from(out.as_str().unwrap());
        assert!(out_path.is_file(), "output file exists: {}", out_path.display());
        assert!(std::fs::metadata(&out_path).unwrap().len() > 0, "output non-empty");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
