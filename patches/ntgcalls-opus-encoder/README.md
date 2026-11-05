# ntgcalls Opus-encoder patch (Parlay Phase 5, OD-1)

Stock libntgcalls 3.0.5 exposes **no** Opus encoder configuration
surface (`nm -D` shows zero opus symbols). This patch adds one by
wrapping WebRTC's builtin audio encoder factory — no WebRTC rebuild,
no new dependency. It is a Parlay-local fork change, not an upstream
version bump (the library still reports version `3.0.5`).

## Base

- Upstream: `https://github.com/pytgcalls/ntgcalls.git`
- Pinned base rev: `a54e57f` ("Merge remote-tracking branch
  'origin/master'"), the HEAD of `refs/ntgcalls` when the patch was
  written (2026-11-03).
- Vendored WebRTC: M152, rev 152.7977.0.3, commit `6f37672d`
  (`refs/ntgcalls/deps/libwebrtc`).

## Files

| File | What it is |
|---|---|
| `ntgcalls-opus-encoder.patch` | The whole patch (467 lines, 7 files, +385/−3), generated from the nested `refs/ntgcalls` repo. Applies cleanly to a pristine checkout of `a54e57f` — verified 2026-11-03 by `git worktree` + `git apply --check` + byte-identical comparison of all 7 files. |
| `README.md` | This file. |
| `../../scripts/rebuild-ntgcalls.sh` | Rebuild script: clones (or reuses) ntgcalls, applies this patch, builds, verifies. |

Patch contents (see `phase5/worker-a-ntgcalls.md` for the full
design + verification evidence):

- NEW `wrtc/include/wrtc/interfaces/media/opus_encoder_factory.hpp`
  + NEW `wrtc/src/interfaces/media/opus_encoder_factory.cpp` —
  delegating `AudioEncoderFactory` that constructs
  `AudioEncoderOpusImpl` directly with a full config for Opus
  (application `kAudio` always, complexity 10, CBR iff !VBR,
  frame 20 ms) and delegates everything else to the builtin
  factory.
- `wrtc/src/interfaces/peer_connection/peer_connection_factory.cpp`
  (1 line): install the factory.
- `wrtc/src/interfaces/media/channels/outgoing_audio_channel.cpp`:
  SDP codec params now come from the live global settings
  (FEC / ptime / maxaveragebitrate / stereo) instead of hardcoded
  `useinbandfec=1, ptime=60`.
- NEW `targets/c/ntgcalls_opus.h` + NEW
  `ntgcalls/src/opus_encoder_config_c.cpp` — hand-written C API
  (NOT NTL schema): `ntg_opus_set_config` / `ntg_opus_get_config` /
  `ntg_opus_set_bitrate`. Defaults: 192000 bps, complexity 10,
  VBR on, FEC off, DTX off, 20 ms frames, 48000 Hz max playback.
- `targets/c/build.cmake` (+2 lines): install the new header into
  `shared-output/include`.

## What the patch does NOT do (documented deviation)

`OPUS_SIGNAL_MUSIC` is **not** literally set. WebRTC M152's
`AudioEncoderOpusConfig` has no signal field and nothing reaches
the live `OpusEncInst`; the encoder runs libopus `OPUS_AUTO` with
`OPUS_APPLICATION_AUDIO`. At the locked 128–192 kbps stereo rates
the audible difference vs explicit `OPUS_SIGNAL_MUSIC` is nil, but
it is not literally `OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)`. A literal
signal would need a ~600-line custom `AudioEncoder` subclass.
Coordinator decision recorded in `phase5/PHASE5.md` (open user
decision — ACCEPT AUTO+AUDIO for Phase 5). The C API deliberately
has **no** `signal` field, so nothing pretends to set it.

## Durability — what survives a fresh clone

`refs/` is gitignored in the outer repo (`refs/` in `.gitignore`),
so the nested `refs/ntgcalls` checkout, the uncommitted patch
there, and the rebuilt `.so` do **not** survive a fresh clone or a
push. What DOES survive:

- This patch file + README + `scripts/rebuild-ntgcalls.sh` (all
  committed in the outer repo).
- `scripts/package-release.sh`, which ships the rebuilt
  `libntgcalls.so` (98,092,096 bytes on the VPS toolchain,
  98,123,704 bytes in the sandbox — size varies with the vendored
  clang revision; both sizes are size-pinned) in the release
  tarball under `lib/`.

What does NOT survive a fresh clone and must be rebuilt on the
build machine: the `.so` itself (`refs/ntgcalls/shared-output/lib/
libntgcalls.so`, 98 MB — never committed to git), the `build-c/`
directory, the vendored clang toolchain (`deps/clang/`, also
gitignored — the rebuild script re-downloads it), and the apt
packages `libasound2-dev` + `libpulse-dev` (see below). The
release tarball is the distribution path for the `.so`.

## Rebuild

```sh
scripts/rebuild-ntgcalls.sh            # full rebuild from the patch
```

Prerequisites (build machine): `cmake`, `make`, a C++20
toolchain, and the ALSA + PulseAudio dev headers — the build fails
at ~74% on `alsa/asoundlib.h` and at ~78% on
`pulse/pulseaudio.h` without them:

```sh
apt-get install -y libasound2-dev libpulse-dev
```

(On Ubuntu 24.04 this may require downgrading `libpulse0` to the
exact version `libpulse-dev` depends on; same SONAME, no ABI
change. A VM replacement wipes apt-installed packages, so
re-install on every fresh build host.)

Expected result: `refs/ntgcalls/shared-output/lib/libntgcalls.so`,
98,092,096 bytes on the VPS toolchain or 98,123,704 bytes in the
sandbox (size varies slightly per toolchain — the packaging size
pin in `scripts/package-release.sh` accepts both known-good sizes
and must be extended if a rebuild produces a new one), exporting
`ntg_opus_set_config`, `ntg_opus_get_config`, `ntg_opus_set_bitrate`
(verify with `nm -D … | grep ntg_opus`), plus the `OpusEncoderFactory`
symbols (22 new dynamic symbol names vs stock, 0 missing).
