#!/usr/bin/env bash
# Parlay — build the 2-process engine in release mode and pack the
# release tarball (FINAL-ARCHITECTURE.md).
#
# Output:  release/parlay-<VERSION>-linux-x86_64.tar.gz
#          release/parlay-<VERSION>-linux-x86_64.tar.gz.sha256
#          (SHA256SUMS also lives inside the tarball, at its root.)
#
# Staged: bin/parlay-core, bin/parlay-backend, lib/libtdjson.so.1.8.69,
# lib/libntgcalls.so (PATCHED Phase 5 build — see
# patches/ntgcalls-opus-encoder/README.md; size-pinned below),
# lib/libparlay_dsp.so (Phase 5 Rust DSP engine, rebuilt from
# services/parlay-dsp sources in step 2b — never ship a stale .so:
# a stale artifact fails exactly at EOS, after all audio played),
# services/miniapp/dist (Phase 6 TypeScript Mini App bundle, built
# in step 2c — the backend auto-searches services/miniapp/dist
# under its working directory, which start.sh sets to the install
# root), scripts/yt_dlp_worker.py (the yt-dlp daemon the
# backend spawns at boot; its venv is created by install.sh, never
# bundled), launchers, parlay.toml.example, .env.example, README,
# INSTALL.md, VERSION.
#
# Re-runnable: the staging dir is wiped at the start of every run.
# Never bundles: .env, tokens, session files, refs/ (except the
# prebuilt libntgcalls.so copied to lib/), build dirs, venvs,
# __pycache__, .git, slskd (install.sh fetches it from upstream),
# audio-fx-rust (Phase 5 port source, not a shipped service).
#
# Launcher scripts (install.sh / start.sh / stop.sh / status.sh) are owned
# by a parallel workstream. If any is missing this script FAILS, unless
# PARLAY_PACKAGE_ALLOW_MISSING_LAUNCHERS=1 is set — that override exists
# only so the build+pack pipeline can be tested before the launchers land,
# and it prints a loud warning when used.
#
# Usage: scripts/package-release.sh
#        (run from anywhere; paths resolve relative to the repo root)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(tr -d '[:space:]' < VERSION)"
DIST="parlay-${VERSION}-linux-x86_64"
RELEASE_DIR="$ROOT/release"
STAGE_PARENT="$RELEASE_DIR/.stage"
STAGE="$STAGE_PARENT/$DIST"
ARCHIVE="$RELEASE_DIR/$DIST.tar.gz"
ARCHIVE_SHA="$ARCHIVE.sha256"

GO_BIN="${PARLAY_GO_BIN:-$ROOT/.tools/go/bin/go}"
[ -x "$GO_BIN" ] || GO_BIN="$(command -v go || true)"
[ -n "$GO_BIN" ] || { echo "ERROR: no Go toolchain (tried $ROOT/.tools/go/bin/go and PATH)" >&2; exit 1; }
# NOTE: go.mod requires Go >= 1.26; a 1.23 toolchain resolves it via
# GOTOOLCHAIN=auto (downloads the newer toolchain on first use).

# --- launcher scripts -------------------------------------------------------
LAUNCHERS=(install.sh start.sh stop.sh status.sh)
MISSING=()
for f in "${LAUNCHERS[@]}"; do
  [ -f "$ROOT/$f" ] || MISSING+=("$f")
done
if [ "${#MISSING[@]}" -gt 0 ]; then
  if [ "${PARLAY_PACKAGE_ALLOW_MISSING_LAUNCHERS:-0}" = "1" ]; then
    echo "WARNING: missing launcher scripts (test override active): ${MISSING[*]}"
    echo "WARNING: the tarball built by this run is INCOMPLETE — re-run once they exist."
  else
    echo "ERROR: missing launcher scripts: ${MISSING[*]}" >&2
    echo "ERROR: they are owned by the launcher workstream; re-run once they exist." >&2
    echo "ERROR: (test override: PARLAY_PACKAGE_ALLOW_MISSING_LAUNCHERS=1)" >&2
    exit 1
  fi
fi

# --- clean staging ----------------------------------------------------------
rm -rf "$STAGE_PARENT"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/scripts"

fail() { echo "ERROR: $*" >&2; exit 1; }
need_file() { [ -f "$1" ] || fail "expected file missing after build: $1"; }
need_exec() { [ -x "$1" ] || fail "expected executable missing after build: $1"; }

# --- 1. core-cpp (C++20, cmake Release) -------------------------------------
echo "==> core-cpp"
if [ -f services/core-cpp/build/CMakeCache.txt ]; then
  cmake --build services/core-cpp/build
else
  cmake -B services/core-cpp/build -DCMAKE_BUILD_TYPE=Release -S services/core-cpp
  cmake --build services/core-cpp/build
fi
need_exec services/core-cpp/build/parlay-core
cp services/core-cpp/build/parlay-core "$STAGE/bin/parlay-core"

# --- 2. backend-go (unified Go backend) ----------------------------------
# Phase 5: the backend MUST be built with -tags parlaydsp, or the
# shipped binary logs "parlay-dsp NOT linked" and every Play fails
# with dsp.ErrUnavailable. The DSP library is rebuilt from current
# sources first (step 2b) so the Go link can never pick up a stale
# .so (a stale .so fails exactly at EOS — see phase5/PHASE5.md).
echo "==> backend-go"
DSP_SO="services/parlay-dsp/target/release/libparlay_dsp.so"
CARGO_BIN="${PARLAY_CARGO_BIN:-$ROOT/.cargo/bin/cargo}"
# Cargo lives at ~/.cargo/bin in the sandbox, which is $HOME/.cargo
# — but the repo root is not necessarily $HOME, so try both.
[ -x "$CARGO_BIN" ] || CARGO_BIN="$HOME/.cargo/bin/cargo"
[ -x "$CARGO_BIN" ] || CARGO_BIN="$(command -v cargo || true)"
[ -n "$CARGO_BIN" ] || { echo "ERROR: no cargo toolchain (tried $ROOT/.cargo/bin/cargo, \$HOME/.cargo/bin/cargo and PATH)" >&2; exit 1; }
(cd services/parlay-dsp && "$CARGO_BIN" build --release)
need_file "$DSP_SO"
(cd services/backend-go && CGO_ENABLED=1 "$GO_BIN" build -tags parlaydsp -trimpath -o "$STAGE/bin/parlay-backend" ./cmd/backend)
need_exec "$STAGE/bin/parlay-backend"

# --- 2c. Mini App bundle (Phase 6: TypeScript lossless player) -----------
# The backend serves the built bundle from services/miniapp/dist
# (auto-search: services/miniapp/dist under the working directory —
# start.sh runs the backend with CWD=$ROOT, so the staged path
# below is found with no config). Without it the backend answers
# 503 miniapp_not_built. The build needs node (typescript is a
# dev-only tool; node_modules is gitignored and installed once by
# build.sh itself). Override PARLAY_PACKAGE_SKIP_MINIAPP=1 exists
# only so the build+pack pipeline can be tested without node, and
# prints a loud warning when used (same pattern as the launcher
# override above).
echo "==> miniapp"
if [ "${PARLAY_PACKAGE_SKIP_MINIAPP:-0}" = "1" ]; then
  echo "WARNING: miniapp build skipped (test override active): the tarball will 503 the Mini App."
else
  command -v node >/dev/null 2>&1 || fail "node is required to build the Mini App (services/miniapp/build.sh)"
  (cd services/miniapp && ./build.sh)
  need_file services/miniapp/dist/app.js
  mkdir -p "$STAGE/services/miniapp"
  cp -r services/miniapp/dist "$STAGE/services/miniapp/dist"
fi

# --- 3. Native voice libs (copy only — NEVER rebuild here) ------------------
echo "==> native libs (copy only)"
TDJSON_SRC="phase4/tdlib-src/build/libtdjson.so.1.8.69"
NTG_SRC="refs/ntgcalls/shared-output/lib/libntgcalls.so"
DSP_SRC="$DSP_SO"
need_file "$TDJSON_SRC"; need_file "$NTG_SRC"; need_file "$DSP_SRC"
# Guard against accidental rebuilds / wrong files: sizes are pinned.
[ "$(stat -c%s "$TDJSON_SRC")" = "50166944" ] || fail "$TDJSON_SRC has an unexpected size (expected 50166944 B)"
# NTG size is the PATCHED Phase 5 build. The size varies with the
# vendored clang revision, so TWO legitimate sizes are known:
# 98,092,096 B (VPS rebuild 2026-11-05) and 98,123,704 B (sandbox
# rebuild 2026-11-05). Both probe-verify: 3 ntg_opus_* symbols,
# defaults 192000 bps / 20 ms, 256k/10ms round-trips. The stock 3.0.5
# .so is 98,121,040 B — if this pin trips on the stock size, the
# patch was never applied; rebuild with scripts/rebuild-ntgcalls.sh.
# If a legitimate rebuild produces a NEW size, add it here (and in
# patches/ntgcalls-opus-encoder/README.md) — never silently widen
# the pin to accept any size.
NTG_SIZE="$(stat -c%s "$NTG_SRC")"
[ "$NTG_SIZE" = "98092096" ] || [ "$NTG_SIZE" = "98123704" ] || fail "$NTG_SRC has an unexpected size ($NTG_SIZE B; expected 98092096 or 98123704 B)"
cp "$TDJSON_SRC" "$STAGE/lib/libtdjson.so.1.8.69"
cp "$NTG_SRC" "$STAGE/lib/libntgcalls.so"
cp "$DSP_SRC" "$STAGE/lib/libparlay_dsp.so"

# --- 4. yt-dlp worker (sources only — the venv is created by install.sh) --
echo "==> yt-dlp worker"
cp scripts/yt_dlp_worker.py "$STAGE/scripts/yt_dlp_worker.py"
need_file "$STAGE/scripts/yt_dlp_worker.py"

# --- 5. Root files -----------------------------------------------------------
echo "==> root files"
for f in "${LAUNCHERS[@]}"; do
  [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$STAGE/$f"
done
cp "$ROOT/.env.example" "$STAGE/.env.example"
cp "$ROOT/parlay.toml.example" "$STAGE/parlay.toml.example"
cp "$ROOT/README.md" "$STAGE/README.md"
cp "$ROOT/INSTALL.md" "$STAGE/INSTALL.md"
cp "$ROOT/VERSION" "$STAGE/VERSION"

# --- 6. Runtime-link check (tarball layout: lib/ next to bin/) -----------
echo "==> runtime-link check"
# Both binaries must resolve every shared lib from the staged lib/
# alone: parlay-core via its $ORIGIN RUNPATH, parlay-backend via
# LD_LIBRARY_PATH (its baked rpath points at the repo's refs/ dir,
# which does not exist in the tarball — start.sh exports
# LD_LIBRARY_PATH=$ROOT/lib for it at runtime).
for bin in parlay-core parlay-backend; do
  if LD_LIBRARY_PATH="$STAGE/lib" ldd "$STAGE/bin/$bin" 2>&1 | grep -q "not found"; then
    LD_LIBRARY_PATH="$STAGE/lib" ldd "$STAGE/bin/$bin" >&2 || true
    fail "$bin has unresolved shared libraries against the staged lib/"
  fi
done

# --- 7. Secret sweep (before the tarball exists) -----------------------------
echo "==> secret sweep"
# (a) No live env / session files anywhere in the staging tree.
if find "$STAGE" \( -name '.env' -o -name '*.session' -o -name '*.session-journal' \
     -o -name 'cookies.txt' -o -name 'config.json' \) -print -quit | grep -q .; then
  fail "staging tree contains a live env/session file"
fi
# (b) The sandbox's real token / api hash must not appear anywhere staged.
#     Values are read into shell variables and never printed.
if [ -f "$ROOT/.env" ]; then
  for var in BOT_TOKEN API_HASH; do
    val="$(grep -E "^${var}=" "$ROOT/.env" | head -n1 | cut -d= -f2- | tr -d '[:space:]')"
    if [ -n "$val" ]; then
      if grep -rFq -- "$val" "$STAGE"; then
        fail "staging tree contains the live value of $var"
      fi
    fi
  done
fi
# (c) .env.example must carry placeholders only: every non-empty assigned
#     value must be a known-safe default (URL, localhost path, number,
#     or one of the documented literal defaults).
if ! awk -F= '/^[A-Z_]+=/ {
       v=$2; sub(/ *#.*/, "", v); gsub(/ /, "", v)
       # Allowed placeholder shapes: URLs, integers AND decimals
       # (rate limits are 1.0), the documented literal defaults
       # (voice profiles, resampler qualities, comma-separated
       # source ladders). A real secret (token/hash) matches none
       # of these shapes. (Phase 5 fix: the sweep rejected the
       # pre-Phase 4 .env.example literals, so packaging failed at
       # step 7 before any tarball existed.)
       if (v != "" && v !~ /^(https?:\/\/.*|mongodb:\/\/.*|redis:\/\/.*|[0-9]+(\.[0-9]+)?|low-ram|data|\.\/downloads|MP3_320|gemini-2\.0-flash|ParlayBot|python3|standard_192k|high_256k|ultra_320k|sinc_256|sinc_128|fast|[a-z]+(,[a-z]+)*)$/) {
         print "  suspicious value: " $0; bad=1
       }
     } END { exit bad }' "$STAGE/.env.example"; then
  fail ".env.example carries a non-placeholder value (see above)"
fi
# (d) No forbidden dirs/files slipped in via rsync.
if find "$STAGE" \( -name '__pycache__' -o -name '.venv' -o -name 'node_modules' \
     -o -name '.git' -o -name 'refs' -o -name 'build' -o -name 'target' \) \
     -print -quit | grep -q .; then
  fail "staging tree contains a forbidden dir (venv/node_modules/build/target/refs/.git/__pycache__)"
fi

# --- 8. SHA256SUMS inside the tarball + pack --------------------------------
echo "==> checksums + pack"
(cd "$STAGE" && find . -type f -not -name SHA256SUMS -print0 | sort -z \
  | xargs -0 sha256sum > SHA256SUMS)
# --owner/--group: the sandbox files are root:nogroup; normalize to root:root
# so extraction on a VPS (as root, or as a user who then chowns) is sane.
tar --owner=0 --group=0 --numeric-owner -czf "$ARCHIVE" -C "$STAGE_PARENT" "$DIST"
sha256sum "$ARCHIVE" > "$ARCHIVE_SHA"
rm -rf "$STAGE_PARENT"

# --- 9. Report ---------------------------------------------------------------
echo
echo "Built: $ARCHIVE"
echo "Size:  $(du -h "$ARCHIVE" | cut -f1) ($(stat -c%s "$ARCHIVE") bytes)"
echo "Files: $(tar tzf "$ARCHIVE" | wc -l)"
echo "SHA256: $(cut -d' ' -f1 "$ARCHIVE_SHA")"
echo "Top 10 largest files in the tarball:"
tar -tzvf "$ARCHIVE" | awk '{print $3, $6}' | sort -rn | head -10
