# audio-fx-rust — audio effects (IMPLEMENTED)

From: cakartamas2010/telegram-music-bot-rust (`main.rs`, single file).
Pure FFmpeg filter chains, no bot logic: `fx.apply` with
`slow-reverb | nightcore | bass-boost | 8d | original`.
Input file in, processed file out.

## Status: implemented, smoke-tested 2026-11-02

- `POST /v1/fx` — real `ffmpeg` subprocess (ffmpeg 8.1.2 verified).
  Filter chains (from the reference bot, re-implemented):
  - `slow-reverb`: `atempo=0.8,aecho=0.8:0.88:60:0.4`
  - `nightcore`: `asetrate=44100*1.25,atempo=1/1.25`
  - `bass-boost`: `bass=g=10:f=110:w=0.6,volume=0.9`
  - `8d`: `apulsator=mode=sine:hz=0.12,aecho=0.8:0.88:40:0.3`
  - `original`: stream copy (`-c:a copy`), no re-encode.
- `GET /healthz` → `{ok:true, service, data:{service, status:"ok"}}`.
- JSON envelopes per `contracts/ipc.json`
  (`{ok:true,data:{...}}` / `{ok:false,error:{code,message}}`).
- Path safety: any `..` component in `input_path`/`output_path` is
  rejected (`bad_request`); missing input file is rejected.
  `output_path` is optional — defaults to `<stem>_<effect>.<ext>`
  next to the input.
- Auth: `X-Parlay-Token` is checked when `$PARLAY_TOKEN` is set.
- Std-only Rust (no crates); own `src/common.rs` tiny HTTP server +
  JSON parser shared with the other two Rust services.
- Env: `PORT` (default 8003).

## Build / run

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release          # target/release/parlay-audio-fx
PORT=8003 ./target/release/parlay-audio-fx
```

## Stubbed / not done

- Nothing stubbed in the endpoint itself. Not implemented: no
  loudness normalisation, no output-format conversion beyond what
  ffmpeg infers from the output extension, no request timeout on
  the ffmpeg subprocess (a huge input blocks the worker thread),
  no per-user rate limiting. The core, not this service, owns
  the 50 MB Bot API oversize rule.
