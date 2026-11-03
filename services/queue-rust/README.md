# queue-rust — crowd-curated voting queue (IMPLEMENTED)

From: daslui/telegram-music-bot (`src/spotify.rs`).
Spotify track link → voting chat → approve/decline → approved tracks join
the Spotify playback queue. `queue.vote` IPC.

## Status: implemented, smoke-tested 2026-11-02

- `POST /v1/queue/vote` `{track, vote: approve|decline, voter_id}` →
  `{ok:true, data:{queued, approvals, declines, threshold, track_id}}`.
- Rules (from the reference's voting-chat flow):
  - One vote per voter per track — a re-vote replaces the previous one.
  - A decline cancels that voter's approval.
  - A track is queued once approvals >= threshold
    (`$PARLAY_VOTE_THRESHOLD`, default 3).
- `GET /healthz` → `{ok:true, service, data:{service, status:"ok"}}`.
- JSON envelopes per `contracts/ipc.json`.
- Persistence: in-memory map + shared durable store (Phase 7, A11:
  `MONGO_URI` > `REDIS_URL` > `./data/votes.json`, key `queue:votes`)
  (`$PARLAY_DATA_DIR` override), atomic write (tmp + rename),
  reloaded on startup. Verified across a restart.
  Tracks are keyed by `track.id` (falls back to the rendered track
  JSON when `id` is absent).
- No Spotify/Telegram calls — the core owns playback; this service
  only decides when a track is crowd-approved.
- Auth: `X-Parlay-Token` is checked when `$PARLAY_TOKEN` is set.
- Std-only Rust (no crates); own `src/common.rs` tiny HTTP server +
  JSON parser shared with the other two Rust services.
- Env: `PORT` (default 8005), `PARLAY_VOTE_THRESHOLD`,
  `PARLAY_DATA_DIR`.

## Build / run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release          # target/release/parlay-queue
PORT=8005 ./target/release/parlay-queue
```

## Stubbed / not done

- Nothing stubbed in the endpoint. Not implemented: no queue
  listing endpoint (the core's `vc.queue` owns the playback queue;
  this service only answers "is it queued?"), no vote expiry,
  no Spotify playback-queue push (the reference's rspotify OAuth
  + live Spotify calls are intentionally not ported).
  Single-process store: concurrent writers on the same `votes.json`
  from two processes would race.
