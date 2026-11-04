#!/usr/bin/env bash
# Parlay — build C++, Go, and Rust components required to run the engine.
# (Normal development build: compiles core-cpp, backend-go, parlay-dsp, and miniapp).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

say() { printf '==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

mkdir -p "$ROOT/bin" "$ROOT/lib"

# --- 1. Toolchain detection --------------------------------------------------
GO_BIN="${PARLAY_GO_BIN:-$ROOT/.tools/go/bin/go}"
[ -x "$GO_BIN" ] || GO_BIN="/usr/local/go/bin/go"
[ -x "$GO_BIN" ] || GO_BIN="$(command -v go || true)"
[ -n "$GO_BIN" ] || fail "no Go toolchain found"

CARGO_BIN="${PARLAY_CARGO_BIN:-$ROOT/.cargo/bin/cargo}"
[ -x "$CARGO_BIN" ] || CARGO_BIN="$HOME/.cargo/bin/cargo"
[ -x "$CARGO_BIN" ] || CARGO_BIN="$(command -v cargo || true)"
[ -n "$CARGO_BIN" ] || fail "no Cargo toolchain found"

command -v cmake >/dev/null 2>&1 || fail "cmake not found"

# --- 2. Rust DSP engine (services/parlay-dsp -> lib/libparlay_dsp.so) ---------
if [ -d "$ROOT/services/parlay-dsp" ]; then
    say "Building Rust DSP engine (services/parlay-dsp)…"
    ( cd "$ROOT/services/parlay-dsp" && "$CARGO_BIN" build --release )
    DSP_SO="$ROOT/services/parlay-dsp/target/release/libparlay_dsp.so"
    [ -f "$DSP_SO" ] || fail "Rust build failed: $DSP_SO missing"
    cp --remove-destination "$DSP_SO" "$ROOT/lib/libparlay_dsp.so"
fi

# --- 3. Go backend (services/backend-go -> bin/parlay-backend) ----------------
if [ -d "$ROOT/services/backend-go" ]; then
    say "Building Go backend (services/backend-go)…"
    # -tags parlaydsp links libparlay_dsp.so for high-res DSP pipeline
    ( cd "$ROOT/services/backend-go" && CGO_ENABLED=1 "$GO_BIN" build -tags parlaydsp -trimpath -o "$ROOT/bin/parlay-backend" ./cmd/backend )
    [ -x "$ROOT/bin/parlay-backend" ] || fail "Go build failed: bin/parlay-backend missing"
fi

# --- 4. C++ core (services/core-cpp -> bin/parlay-core) ----------------------
if [ -d "$ROOT/services/core-cpp" ]; then
    say "Building C++ core (services/core-cpp)…"
    TDJSON_LIB="$ROOT/lib/libtdjson.so.1.8.69"
    if [ ! -f "$ROOT/services/core-cpp/build/CMakeCache.txt" ]; then
        cmake -B "$ROOT/services/core-cpp/build" -DCMAKE_BUILD_TYPE=Release -S "$ROOT/services/core-cpp" \
              -DTDJSON_LIB="$TDJSON_LIB"
    fi
    cmake --build "$ROOT/services/core-cpp/build" -j"$(nproc)"
    CORE_BIN="$ROOT/services/core-cpp/build/parlay-core"
    [ -x "$CORE_BIN" ] || fail "C++ build failed: $CORE_BIN missing"
    cp --remove-destination "$CORE_BIN" "$ROOT/bin/parlay-core"
fi

# --- 5. TypeScript Mini App bundle (services/miniapp/dist) --------------------
if [ -d "$ROOT/services/miniapp" ] && command -v node >/dev/null 2>&1; then
    if [ ! -f "$ROOT/services/miniapp/dist/app.js" ]; then
        say "Building Mini App bundle (services/miniapp)…"
        ( cd "$ROOT/services/miniapp" && ./build.sh )
    fi
fi

say "Build complete: bin/parlay-backend, bin/parlay-core, lib/libparlay_dsp.so ready."
