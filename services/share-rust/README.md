# share-rust — anonymous music sharing (IMPLEMENTED)

From: 0xNima/feel-me (`src/{main,lib,models}.rs`, teloxide + Diesel).
`share.create` → unique link + token; `share.resolve` → track.
No identity is stored with a link. Tokens are random, single-purpose.

## Status: implemented, smoke-tested 2026-11-02

- `POST /v1/share` `{created_by, track}` →
  `{ok:true, data:{link, token}}`.
  Token: 16 random alphanumeric chars from `/dev/urandom`.
  Link: `https://t.me/<bot>?start=share_<token>`, bot username from
  `$PARLAY_BOT_USERNAME` (default `ParlayBot`).
- `GET /v1/share/:token` → `{ok:true, data:{track}}`;
  unknown token → `404 {ok:false, error:{code:"not_found", ...}}`.
- `GET /healthz` → `{ok:true, service, data:{service, status:"ok"}}`.
- JSON envelopes per `contracts/ipc.json`.
- Anonymity: `created_by` is accepted (contract field) but
  deliberately NOT stored — the record is token → track only.
- Persistence: in-memory map + shared durable store (Phase 7, A11:
  `MONGO_URI` > `REDIS_URL` > `./data/shares.json`, key `share:links`)
  (`$PARLAY_DATA_DIR` override), atomic write (tmp + rename),
  reloaded on startup. Verified across a restart.
- No Telegram call is made — this service only mints the token/link.
- Auth: `X-Parlay-Token` is checked when `$PARLAY_TOKEN` is set.
- Std-only Rust (no crates); own `src/common.rs` tiny HTTP server +
  JSON parser shared with the other two Rust services.
- Env: `PORT` (default 8004), `PARLAY_BOT_USERNAME`, `PARLAY_DATA_DIR`.

## Build / run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release          # target/release/parlay-share
PORT=8004 ./target/release/parlay-share
```

## Stubbed / not done

- Nothing stubbed in the endpoints. Not implemented: no token
  expiry/revocation, no per-user share listing, no Diesel/Postgres
  backend (the reference's teloxide bot + DB are intentionally not
  ported — the core owns Telegram; this service is link minting
  only). Single-process store: concurrent writers on the same
  `shares.json` from two processes would race.
