# PROGRESS — queue-rust

Worker C, 2026-11-02 ~19:3x IST. Toolchain: cargo 1.99.1 / rustc 1.99.1
(`~/.cargo/bin`).

## Files changed
- `src/main.rs` — rewritten from stub: POST /v1/queue/vote
  (approve/decline, one vote per voter, decline cancels approval,
  queued at approvals >= threshold), IPC JSON envelopes.
- `src/common.rs` — NEW: std-only JSON parser/serializer, tiny
  HTTP/1.1 server, X-Parlay-Token check, atomic-write + token helpers.
  Shared (copied) with audio-fx-rust and share-rust.
- `README.md` — updated: implemented vs stubbed.
- `Cargo.toml` — unchanged (edition 2021, zero dependencies).
- `data/votes.json` — created by the smoke test (1 track; test data).
- `target/` — release build artifacts (not committed).

## Build
```sh
export PATH="$HOME/.cargo/bin:$PATH"
cd ~/workspace/parlay/services/queue-rust
cargo build --release --offline
# Finished `release` profile [optimized] target(s) in 6.06s
# (rebuilt clean, 0 warnings, after adding #![allow(dead_code)] to common.rs)
```
No crates downloaded — std-only, so no network was needed.

## Smoke tests (all run 2026-11-02, server on :8005, threshold 3)
Track used: `{"id":"sp-xyz","title":"Crowd Song","artist":"Crowd","source":"spotify","duration_sec":200}`
```sh
./target/release/parlay-queue &   # from the service dir
curl -s http://127.0.0.1:8005/healthz
# {"ok":true,"service":"queue-rust","data":{"service":"queue-rust","status":"ok"}}
# vote approve, voter 101 -> {"queued":false,"approvals":1,"declines":0,"threshold":3,"track_id":"sp-xyz"}
# vote approve, voter 102 -> {"queued":false,"approvals":2,...}
# vote approve, voter 103 -> {"queued":true,"approvals":3,...}
# vote approve, voter 103 again (duplicate) -> approvals still 3 (no double-count)
# vote decline, voter 103 -> {"queued":false,"approvals":2,"declines":1,...}  (approval cancelled)
# vote "maybe", voter 104 -> {"ok":false,"error":{"code":"bad_request","message":"vote must be approve|decline"}}
cat data/votes.json
# {"sp-xyz":{"track":{...},"approvals":["101","102"],"declines":["103"]}}
# Persistence: server killed and restarted, then voter 104 approve ->
# {"queued":true,"approvals":3,"declines":1,"threshold":3,"track_id":"sp-xyz"} (state reloaded)
```
All test servers stopped afterwards (`pkill -x parlay-*`).

## Real vs stubbed
- REAL: full voting rules, threshold (env `PARLAY_VOTE_THRESHOLD`,
  default 3), atomic JSON persistence + reload, healthz, token auth
  gate (via common.rs).
- STUBBED: none in the endpoint. Known gaps (see README): no queue
  listing endpoint, no vote expiry, no live Spotify playback-queue
  push (the reference's rspotify OAuth is intentionally not ported —
  the core owns playback).
