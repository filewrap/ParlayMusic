#!/usr/bin/env bash
# Rebuild the patched libntgcalls.so (Parlay Phase 5, OD-1) from the
# committed patch in patches/ntgcalls-opus-encoder/.
#
# Stock libntgcalls 3.0.5 has no Opus encoder configuration surface;
# the patch adds ntg_opus_set_config / ntg_opus_get_config /
# ntg_opus_set_bitrate (defaults 192000 bps, complexity 10, VBR on,
# FEC off, 20 ms frames) by wrapping WebRTC's builtin audio encoder
# factory. See patches/ntgcalls-opus-encoder/README.md.
#
# Layout: refs/ is gitignored in the outer repo, so a fresh clone
# has no ntgcalls checkout at all. This script clones it (or reuses
# an existing checkout), checks out the pinned base rev, applies the
# patch, and builds. The build writes straight to
# refs/ntgcalls/shared-output/lib/libntgcalls.so — the exact path
# the Go cgo link uses.
#
# Prerequisites: cmake, make, C++20 toolchain, libpulse-dev
# (the build fails at ~78% on pulse/pulseaudio.h without it:
#   apt-get install -y libpulse-dev
# On Ubuntu 24.04 that may require downgrading libpulse0 to the
# exact version libpulse-dev depends on; same SONAME, no ABI change.)
#
# Usage: scripts/rebuild-ntgcalls.sh [--clean]
#   --clean: remove the existing build dir first (full rebuild;
#            otherwise the build is incremental — ~15-30 s + link
#            for a patch-only change, ~4 min for the patch itself
#            after headers are installed).
#
# Expected result: refs/ntgcalls/shared-output/lib/libntgcalls.so
# (~98,123,704 bytes for this toolchain; the packaging size pin in
# scripts/package-release.sh must be updated if a rebuild changes
# it), exporting the 3 ntg_opus_* symbols. The script verifies that
# and fails loudly otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

NTG_DIR="$ROOT/refs/ntgcalls"
PATCH="$ROOT/patches/ntgcalls-opus-encoder/ntgcalls-opus-encoder.patch"
BASE_REV="a54e57f"
UPSTREAM="https://github.com/pytgcalls/ntgcalls.git"

[ -f "$PATCH" ] || { echo "ERROR: patch missing: $PATCH" >&2; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake not found" >&2; exit 1; }

if [ ! -d "$NTG_DIR/.git" ]; then
    echo "==> cloning ntgcalls $BASE_REV (refs/ is gitignored; fresh clone has no checkout)"
    mkdir -p "$ROOT/refs"
    git clone "$UPSTREAM" "$NTG_DIR"
fi

cd "$NTG_DIR"
git fetch --quiet origin || true
git checkout --quiet "$BASE_REV"
# The patch must apply cleanly to the pristine base. Detect "already
# applied" from the TREE (reverse-apply check), not from an existing
# .so: after `git checkout -- .` the tree is pristine but a stale
# patched .so may still sit in shared-output, and trusting the .so
# would skip the apply and build unpatched sources (caught only by
# the nm check at the end). The tree is the truth.
if git apply --check --reverse "$PATCH" >/dev/null 2>&1; then
    echo "NOTE: patch already applied in tree; skipping git apply"
elif git diff --quiet && [ -z "$(git status --porcelain -- wrtc targets ntgcalls | grep -v '^?? build-c')" ]; then
    echo "==> applying $PATCH"
    git apply --check "$PATCH"
    git apply "$PATCH"
else
    echo "==> working tree already carries changes; skipping git apply (verify below)"
fi

if [ "${1:-}" = "--clean" ]; then
    echo "==> --clean: removing build-c"
    rm -rf build-c
elif [ -f build-c/CMakeCache.txt ]; then
    # CMake loads the toolchain file ONLY on the first configure of a
    # build dir. A cache created by a configure WITHOUT the toolchain
    # file (the old script, or a manual cmake run) records the flag as
    # UNINITIALIZED and silently ignores it on every later run — the
    # configure keeps failing at FindWebRTC.cmake:1 with "Unknown
    # CMake command 'GetProperty'" no matter how many times you pass
    # -DCMAKE_TOOLCHAIN_FILE. Proven in the sandbox 2026-11-05: same
    # build dir, flag added, identical failure; cache line read
    # CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=cmake/Toolchain.cmake. The
    # only cure is a fresh build dir, so detect the stale cache and
    # wipe it automatically. A cache with the toolchain loaded
    # (CMAKE_TOOLCHAIN_FILE:FILEPATH=<abs path>) is kept for an
    # incremental build.
    if ! grep -q "^CMAKE_TOOLCHAIN_FILE:FILEPATH=$NTG_DIR/cmake/Toolchain.cmake\$" build-c/CMakeCache.txt; then
        echo "==> build-c cache has no loaded toolchain file (stale from a configure without it); removing build-c for a fresh configure"
        rm -rf build-c
    fi
fi

echo "==> configure"
# The toolchain file is REQUIRED, not optional: GetProperty (used by
# every cmake/Find*.cmake) is defined in cmake/PlatformUtils.cmake,
# which is loaded only via cmake/Toolchain.cmake. Without it a fresh
# configure fails at FindWebRTC.cmake:1 with "Unknown CMake command
# 'GetProperty'". It worked in the sandbox only because build-c/
# already had the toolchain file cached from the first configure.
cmake -S . -B build-c -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=OFF -DUSE_LIBCXX=ON -DBINDING=c -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain.cmake
echo "==> build (incremental; ~4 min first time after libpulse-dev install)"
cmake --build build-c --target ntgcalls -j2

SO="shared-output/lib/libntgcalls.so"
[ -f "$SO" ] || { echo "ERROR: expected $SO missing after build" >&2; exit 1; }
echo "==> verify: $SO ($(stat -c%s "$SO") bytes)"
# Do NOT use `nm | grep -q` here: the script runs under `set -o
# pipefail`, nm -D emits ~820 KB, and grep -q exits on the first
# match while nm still has hundreds of KB to write — nm dies with
# SIGPIPE (141), pipefail propagates it, and the check fails even
# though the symbol IS exported (proven 2026-11-05: 20/20 fails
# under pipefail, 0/20 without). grep -c reads the whole stream, so
# nm always finishes cleanly.
[ "$(nm -D "$SO" | grep -c " T ntg_opus_set_config")" -gt 0 ] || { echo "ERROR: ntg_opus_set_config not exported" >&2; exit 1; }
[ "$(nm -D "$SO" | grep -c " T ntg_opus_get_config")" -gt 0 ] || { echo "ERROR: ntg_opus_get_config not exported" >&2; exit 1; }
[ "$(nm -D "$SO" | grep -c " T ntg_opus_set_bitrate")" -gt 0 ] || { echo "ERROR: ntg_opus_set_bitrate not exported" >&2; exit 1; }
nm -D "$SO" | grep "ntg_opus"
echo "OK: patched libntgcalls.so rebuilt at $NTG_DIR/$SO"
echo "NOTE: if the size changed, update the NTG size pin in scripts/package-release.sh"
