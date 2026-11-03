# parlay-dsp

In-process Rust C-ABI DSP engine for Parlay (Phase 5). Decode + resample +
f32 FX + gapless dual-slot playback, linked into the Go backend via cgo.
No subprocess, no network, no inter-service HTTP — the engine only ever
touches bytes Go pushes into it.

## Layout

| File | What it is |
| --- | --- |
| `Cargo.toml` | Crate `parlay-dsp` 0.1.0, `cdylib` + `rlib`, edition 2021 |
| `parlay_dsp.h` | The C-ABI contract Worker C consumes (authoritative) |
| `src/lib.rs` | C-ABI entry points + handle registry (panic-safe boundary) |
| `src/engine.rs` | Two slots, one resampler, one FX chain, one 960-frame pull |
| `src/slot.rs` | Per-track decode: symphonia probe/decode, delay/padding trim |
| `src/source.rs` | Growable `SharedBuf` + seekable blocking `BufReader` |
| `src/ogg_opus.rs` | Minimal Ogg demuxer + Opus decode (symphonia has no Opus) |
| `src/opus_ffi.rs` | Hand-rolled FFI to system `libopus.so.0` (5 functions) |
| `src/resample.rs` | rubato `SincFixedOut` to 48 kHz, ratio carries speed |
| `src/fx.rs` | Bass biquad, echo, 8D pan/Haas, Freeverb, volume, limiter, TPDF dither |
| `src/params.rs` | FX targets, per-frame smoothing, the five presets |
| `tests/engine_tests.rs` | 13 integration tests with measurements (see below) |

## Dependencies (and why each one is there)

| Crate | Why |
| --- | --- |
| `symphonia 0.5` | Decode: FLAC, MP3, AAC/M4A (incl. ALAC), Vorbis, WAV/PCM, ADTS. Features enabled: `aac`, `alac`, `mp3`, `isomp4` on top of the defaults. |
| `rubato 0.16` | Sinc resampling to 48 kHz (`SincFixedOut`, fixed 960-frame output, dynamic ratio for speed, built-in ratio ramping). Pulls `realfft`/`rustfft`. |

Nothing else. In particular:

- **Opus has no crate.** symphonia 0.5 cannot decode Opus, and the
  `opus`/`audiopus` crates need libopus *headers* + a `.so` stub at build
  time. The sandbox and VPS ship only the runtime `libopus.so.0` (via
  ffmpeg). So Opus is decoded by `src/ogg_opus.rs` (a ~400-line Ogg page
  parser — the framing is trivial) + `src/opus_ffi.rs` (five libopus
  functions declared by hand, linked by exact filename
  `#[link(name = ":libopus.so.0")]`). Runtime requirement: `libopus.so.0`
  must exist where the Go backend runs.
- **No RNG, biquad, or DSP crate.** The FX (biquad, echo, Freeverb, Haas,
  limiter, TPDF dither, xorshift PRNG) are ~700 lines of hand-rolled f32
  code — small, exact, and free of extra audit surface.

All dependencies were fetched through the sandbox egress proxy
(`cargo fetch` verified 2026-11-03; symphonia 0.5.5, rubato 0.16.2).
Rust 1.92+ required; built here with 1.99.1.

## Build

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release          # target/release/libparlay_dsp.so + .rlib
cargo test --release           # 13 integration tests (ffmpeg fixtures)
cargo clippy --release --all-targets
```

## Output format and sink (confirmed)

- Every pull returns **exactly 960 samples per channel, 48 kHz, stereo
  interleaved** — `pull_f32` as `float[1920]`, `pull_s16` as `int16[1920]`.
- Sink (confirmed 2026-11-03 by Worker A, `phase5/worker-a-ntgcalls.md`):
  `ntg_send_external_frame` takes **s16le, 48 kHz, stereo**; its frame
  size is **1920 bytes per 10 ms frame** (480 samples). One DSP pull
  (3840 bytes s16) is exactly **two** 10 ms sub-frames.
- **Sharp edge (Worker A):** `send_data` ignores the passed size and
  derives the frame count from the configured frame size — an oversize
  buffer is silently truncated, a short one is over-read. Go **must**
  chunk each pull into exact 1920-byte pushes and never send a short final
  chunk (pad or drop it). The DSP guarantees its pulls are exact
  multiples of the 10 ms frame; the 10 ms granularity is the contract.
- The f32 path never passes through 16-bit. The s16 path is the same f32
  frame plus TPDF dither + quantise at the very end.

## How playback works

Go pushes encoded bytes into a slot (`push` + `finish_input`) and pulls
960-frames (`pull_f32` / `pull_s16`). Two slots (0/1) form a 2-deep
queue: while slot 0 plays, Go prefetches into slot 1; when slot 0 drains
mid-frame the engine fills the rest of that same frame from slot 1 and
reports `*out_transition = 1`, so Go fires track-advance on the exact
sample boundary. `parlay_dsp_free_slot()` says which slot to push into
next. Loop mode 1 restarts the active track from byte 0 (`transition =
2`); queue loop is Go's job.

Push and pull **must run on different threads/goroutines**. A pull may
block inside the decoder until more bytes arrive or `finish_input` is
called. Push never takes the engine mutex, so a blocked pull can never
starve a push. `destroy` wakes blocked readers with EOF and is safe
while a pull is in flight (handles are `Arc`-backed ids).

## Decode and gapless trimming

- Format sniff order: Ogg-Opus (`OpusHead` in the first packet) first,
  then symphonia probe (FLAC, MP3, AAC/M4A/ALAC, Vorbis, WAV/PCM, ...).
- Gapless is exact, not approximate:
  - symphonia path: `codec_params.delay` leading samples are skipped,
    `codec_params.padding` trailing samples are held back and never fed
    to the resampler (FLAC/WAV have delay = padding = 0).
  - Opus path: `OpusHead.pre_skip` is dropped at the front; each packet
    is truncated to its completing page's granule position (RFC 7845),
    so the stream ends on the exact sample.
  - The resampler is **never reset at a track boundary** — when the next
    track has the same rate it just keeps consuming (the boundary is
    exactly what resampling the concatenation would produce). A rate
    change only updates the ratio (ramped); filter state carries over.
  - The FX chain state (echo/reverb tails, limiter envelope, LFO phase)
    also carries across boundaries.
- The media source is **seekable** over the buffered bytes (a seek past
  the buffered prefix blocks until the bytes arrive). This is what lets
  symphonia's isomp4 reader find a `moov` atom at the end of a
  non-faststart MP4. `SeekFrom::End` blocks until `finish_input`.

## FX chain (per 960-frame, f32 throughout)

```
resample (speed) -> bass shelf -> echo -> 8D pan/Haas -> Freeverb
  -> volume -> limiter -> [s16: TPDF dither + quantise]
```

- **Speed** is a resample-ratio change: `ratio = (48000/in_rate)/speed`.
  Pitch and tempo move together (vinyl-style). rubato ramps the ratio
  across the chunk; the engine additionally steps speed by at most 0.05
  per frame.
- **Bass** is an RBJ low-shelf biquad (`g` dB @ 110 Hz, slope `S = w =
  0.6`), coefficients interpolated per-sample when the gain moves, with
  the port's 0.9 post-gain blended in while boosted.
- **Echo** is ffmpeg `aecho` semantics (in 0.8 / out 0.88 / delay /
  decay). The delay line is never cleared; disabling ramps the wet level.
- **8D** is a phase-continuous 0.12 Hz quadrature LFO constant-power
  panner + Haas delay (up to 8 ms, quadrature to the pan). The LFO phase
  is never reset.
- **Reverb** is a stereo Freeverb (4 lowpass-feedback combs + 2
  allpasses per channel, right combs +23 samples), wet ramped.
- **Volume** 0–200 maps to `(v/100)^2`, smoothed per-sample.
- **Limiter** is always on: true-peak brickwall, 5 ms lookahead
  (240-sample delay — the output is delayed 5 ms by design), soft knee
  (3 dB), ceiling −0.1 dBFS, instant attack on the delayed signal,
  100 ms release.
- **TPDF dither** (2 LSB peak-to-peak) is applied **only** at the final
  f32 → s16 conversion. The f32 path is never dithered or quantised.

### Presets (ported from `services/audio-fx-rust`)

| Preset | Parameters |
| --- | --- |
| `original` | everything off, speed 1.0 |
| `nightcore` | speed 1.25 |
| `bass-boost` | bass +10 dB @ 110 Hz, post-gain 0.9 |
| `slow-reverb` | speed 0.8, echo 60 ms / decay 0.4, Freeverb wet 0.35 |
| `8d` | echo 40 ms / decay 0.3, LFO pan 0.12 Hz + Haas |

A preset never changes volume. **Documented deviation:** the reference
ffmpeg chain for nightcore was `asetrate=44100*1.25,atempo=1/1.25`
(pitch up, tempo restored); parlay-dsp implements nightcore as a pure
resample-ratio speed-up — pitch AND tempo up — per the locked doc's
"dynamic resampler ratio" design. Same for slow-reverb (0.8x = slower
and lower, vs ffmpeg `atempo=0.8` tempo-only).

### Zero-drop switching

No parameter ever steps discontinuously: speed steps ≤0.05/frame,
bass ≤1 dB/frame, echo/reverb/8D wet ≤0.2/frame (full on/off in 5 frames
= 100 ms), and the chain interpolates per-sample inside each frame
(biquad coefficients, wet levels, pan amount, volume gain). Delay lines,
LFO phase, and limiter envelope are continuous state — toggling an
effect never clears them.

## C-ABI

See `parlay_dsp.h` (authoritative) and `src/lib.rs`. In brief:

```
create / destroy / version
push / finish_input / reset_slot / free_slot
pull_f32 / pull_s16            -> 0 frame | 1 need input | 2 EOS | <0 error
set_preset / set_speed / set_bass_db / set_volume / set_loop
set_echo / set_reverb / set_8d
stats / last_error
```

Return codes: `0` ok, `1` need input, `2` EOS, `-1` generic,
`-2` bad handle, `-3` bad argument, `-4` decode error.
`*out_transition`: `0` none, `1` track boundary in this frame,
`2` loop restart in this frame. All entry points catch panics at the
boundary (translated to `-1`) and recover from poisoned mutexes.

## Tests

`tests/engine_tests.rs` — 13 tests, fixtures generated with ffmpeg
(+ one hand-written f32 WAV for the sub-LSB test). Measured results
(2026-11-03, sandbox, release build) — see `phase5/worker-b-dsp.md`
for the full table:

- Gapless two-track (FLAC and WAV): **max |two-track − one-track| = 0.0**
  (bit-identical); loop restart likewise **0.0**.
- Zero-drop speed switch: max step at the switch **0.00510** vs
  **0.00511** before (identical — no click); pitch ratio measured
  **1.250** exactly.
- Zero-drop bass switch: max step at the switch < 0.05; +8.5 dB at
  30 Hz, −0.8 dB at 1 kHz.
- f32 detail: 12 kHz @ 1e-6 amplitude (0.03 LSB) survives at
  **9.587e-7**; **99.9%** of samples carry sub-LSB information.
- Opus: sample count matches ffmpeg within one frame; waveform
  max diff **0.000026** after compensating the known 240-sample
  limiter delay.
- First-frame latency (cold): **3.7 ms**. Throughput: **10.3x
  realtime** (legacy ffmpeg pipeline: 46x — decode-only is faster, as
  expected; 10x is far above the 1x the product needs).

## Known limitations (honest)

- **Opus is mono/stereo only.** A multichannel OpusHead is a clean
  error, not a silent downmix.
- **Multichannel symphonia input (>2ch)** takes front-left/front-right
  (no proper downmix).
- **Resampler startup transient:** the first ~128 output samples ramp
  from silence (zero-padded sinc window) at stream start only — never
  at a track boundary.
- **Flush tail:** at queue end, the resampler tail is emitted as one
  extra 960-frame (up to ~832 zeros after the true end) and the FX
  tails (echo/reverb) are truncated there — mid-queue tails are
  preserved across boundaries.
- **Limiter latency:** the output is delayed a constant 5 ms
  (lookahead); the first 240 samples of a stream are silence.
- **Speed-up aliasing:** rubato's sinc filter is designed at the
  construction ratio; speeding up >~5% can alias near-Nyquist content
  (negligible for music — little energy above 19 kHz).
- **MP4 seeking:** `SeekFrom::End` blocks until `finish_input`; an
  MP4 pushed as a never-ending stream cannot be probed.
- **No ffmpeg fallback.** The locked doc allows ffmpeg for exotic
  formats (HLS/RTMP radio), but the engine is in-process only — no
  subprocess per call. An unsupported format is a clean decode error.
