# PROGRESS — share-rust

Worker C, 2026-11-02 ~19:3x IST. Toolchain: cargo 1.99.1 / rustc 1.99.1
(`~/.cargo/bin`).

## Files changed
- `src/main.rs` — rewritten from stub: POST /v1/share (16-char
  /dev/urandom token, `https://t.me/<bot>?start=share_<token>` link),
  GET /v1/share/:token, IPC JSON envelopes.
- `src/common.rs` — NEW: std-only JSON parser/serializer, tiny
  HTTP/1.1 server, X-Parlay-Token check, atomic-write + token helpers.
  Shared (copied) with audio-fx-rust and queue-rust.
- `README.md` — updated: implemented vs stubbed.
- `Cargo.toml` — unchanged (edition 2021, zero dependencies).
- `data/shares.json` — created by the smoke test (1 entry; test data).
- `target/` — release build artifacts (not committed).

## Build
```sh
export PATH="$HOME/.cargo/bin:$PATH"
cd ~/workspace/parlay/services/share-rust
cargo build --release --offline
# Finished `release` profile [optimized] target(s) in 7.75s
# (rebuilt clean, 0 warnings, after adding #![allow(dead_code)] to common.rs)
```
No crates downloaded — std-only, so no network was needed.

## Smoke tests (all run 2026-11-02, server on :8004)
```sh
./target/release/parlay-share &   # from the service dir
curl -s http://127.0.0.1:8004/healthz
# {"ok":true,"service":"share-rust","data":{"service":"share-rust","status":"ok"}}
curl -s -X POST http://127.0.0.1:8004/v1/share -d '{"created_by":777,"track":{"id":"yt-abc","title":"Test Song","artist":"Tester","source":"youtube","duration_sec":180}}'
# {"ok":true,"data":{"link":"https://t.me/ParlayBot?start=share_yJ0xE7L9wDPqFb1S","token":"yJ0xE7L9wDPqFb1S"}}
curl -s http://127.0.0.1:8004/v1/share/yJ0xE7L9wDPqFb1S
# {"ok":true,"data":{"track":{"id":"yt-abc","title":"Test Song","artist":"Tester","source":"youtube","duration_sec":180}}}
curl -s http://127.0.0.1:8004/v1/share/AAAAAAAAAAAAAAAA
# {"ok":false,"error":{"code":"not_found","message":"unknown share token"}}
cat data/shares.json
# {"yJ0xE7L9wDPqFb1S":{"id":"yt-abc","title":"Test Song","artist":"Tester","source":"youtube","duration_sec":180}}
# Persistence: server killed and restarted, same GET resolve returned the track again.
```
All test servers stopped afterwards (`pkill -x parlay-*`).

## Real vs stubbed
- REAL: token minting (urandom), link format, resolve, atomic JSON
  persistence + reload, healthz, token auth gate (via common.rs).
- STUBBED: none in the endpoints. Known gaps (see README): no token
  expiry/revocation, no per-user listing, no Postgres/teloxide
  backend (intentionally not ported — core owns Telegram).
  `created_by` is accepted but NOT stored (anonymous by design).
