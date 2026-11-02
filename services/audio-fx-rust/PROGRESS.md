# PROGRESS — audio-fx-rust

Worker C, 2026-11-02 ~19:3x IST. Toolchain: cargo 1.99.1 / rustc 1.99.1
(`~/.cargo/bin`), ffmpeg 8.1.2 verified via `ffmpeg -version`.

## Files changed
- `src/main.rs` — rewritten from stub: POST /v1/fx (real ffmpeg
  subprocess), path-traversal rejection, input-exists check, derived
  output path, IPC JSON envelopes.
- `src/common.rs` — NEW: std-only JSON parser/serializer, tiny
  HTTP/1.1 server (TcpListener, per-connection threads), X-Parlay-Token
  check, atomic-write + /dev/urandom token helpers. Shared (copied)
  with share-rust and queue-rust.
- `README.md` — updated: implemented vs stubbed.
- `Cargo.toml` — unchanged (edition 2021, zero dependencies).
- `target/` — release build artifacts (not committed).

## Build
```sh
export PATH="$HOME/.cargo/bin:$PATH"
cd ~/workspace/parlay/services/audio-fx-rust
cargo build --release --offline
# Finished `release` profile [optimized] target(s) in 8.97s
# (rebuilt clean, 0 warnings, after adding #![allow(dead_code)] to common.rs)
```
No crates downloaded — std-only, so no network was needed.

## Smoke tests (all run 2026-11-02, server on :8003)
```sh
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=2" /tmp/fx-in.wav   # 2.000000s
./target/release/parlay-audio-fx &   # from the service dir
curl -s http://127.0.0.1:8003/healthz
# {"ok":true,"service":"audio-fx-rust","data":{"service":"audio-fx-rust","status":"ok"}}
curl -s -X POST http://127.0.0.1:8003/v1/fx -d '{"effect":"nightcore","input_path":"/tmp/fx-in.wav","output_path":"/tmp/fx-nightcore.wav","requested_by":123}'
# {"ok":true,"data":{"output_path":"/tmp/fx-nightcore.wav"}}  (219,608 bytes, 1.991s)
curl -s -X POST ... -d '{"effect":"slow-reverb","input_path":"/tmp/fx-in.wav"}'
# {"ok":true,"data":{"output_path":"/tmp/fx-in_slow-reverb.wav"}}  (derived name, 225,436 bytes)
curl ... -d '{"effect":"bass-boost",...,"output_path":"/tmp/fx-bass.wav"}'   # ok:true
curl ... -d '{"effect":"8d",...,"output_path":"/tmp/fx-8d.wav"}'             # ok:true
curl ... -d '{"effect":"original",...,"output_path":"/tmp/fx-orig.wav"}'     # ok:true
# Errors:
{"effect":"chipmunk"}        -> {"ok":false,"error":{"code":"bad_request","message":"unknown effect (slow-reverb|nightcore|bass-boost|8d|original)"}}
{"input_path":"/tmp/nope.wav"} -> {"ok":false,"error":{"code":"bad_request","message":"input file not found: /tmp/nope.wav"}}
{"input_path":"/tmp/../etc/passwd"} -> {"ok":false,"error":{"code":"bad_request","message":"path traversal rejected: /tmp/../etc/passwd"}}
# Auth (separate instance, PORT=18003, PARLAY_TOKEN=test123):
no header / wrong header -> {"ok":false,"error":{"code":"unauthorized","message":"missing or invalid X-Parlay-Token"}}
X-Parlay-Token: test123  -> {"ok":true,"data":{"output_path":"/tmp/fx-auth.wav"}}
/healthz stays unauthenticated by design.
```
All test servers stopped afterwards (`pkill -x parlay-*`).

## Real vs stubbed
- REAL: all 5 effects via real ffmpeg subprocess; validation; error
  envelopes; healthz; token auth gate.
- STUBBED: none in the endpoint. Known gaps (see README): no ffmpeg
  subprocess timeout, no loudness normalisation, no format conversion
  beyond the output extension.
