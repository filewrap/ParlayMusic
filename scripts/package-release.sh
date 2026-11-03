#!/usr/bin/env bash
# Parlay — build every service in release mode and pack the release tarball.
#
# Output:  release/parlay-<VERSION>-linux-x86_64.tar.gz
#          release/parlay-<VERSION>-linux-x86_64.tar.gz.sha256
#          (SHA256SUMS also lives inside the tarball, at its root.)
#
# Re-runnable: the staging dir is wiped at the start of every run.
# Never bundles: .env, tokens, session files, refs/, build dirs, venvs,
# __pycache__, .git, slskd (install.sh fetches it from upstream).
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
export PATH="$HOME/.cargo/bin:$PATH"
command -v cargo >/dev/null || { echo "ERROR: cargo not found (tried ~/.cargo/bin and PATH)" >&2; exit 1; }

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
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/services"

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

# --- 2. vc-go + netease-go ---------------------------------------------------
echo "==> vc-go"
(cd services/vc-go && "$GO_BIN" build -trimpath -o "$STAGE/bin/parlay-vc-go" ./cmd/parlay-vc)
need_exec "$STAGE/bin/parlay-vc-go"
echo "==> netease-go"
(cd services/netease-go && "$GO_BIN" build -trimpath -o "$STAGE/bin/parlay-netease" ./cmd/parlay-netease)
need_exec "$STAGE/bin/parlay-netease"

# --- 3. Rust services (offline release builds) ------------------------------
echo "==> rust services"
declare -A RUST_BINS=(
  [vc-rust]=parlay-vc-rust
  [audio-fx-rust]=parlay-audio-fx
  [share-rust]=parlay-share
  [queue-rust]=parlay-queue
  [spotify-rust]=parlay-spotify
)
for svc in vc-rust audio-fx-rust share-rust queue-rust spotify-rust; do
  bin="${RUST_BINS[$svc]}"
  (cd "services/$svc" && cargo build --release --offline)
  need_exec "services/$svc/target/release/$bin"
  cp "services/$svc/target/release/$bin" "$STAGE/bin/$bin"
done

# --- 4. Native voice libs (copy only — NEVER rebuild here) ------------------
echo "==> native libs (copy only)"
TDJSON_SRC="phase4/tdlib-src/build/libtdjson.so.1.8.69"
NTG_SRC="refs/ntgcalls/shared-output/lib/libntgcalls.so"
need_file "$TDJSON_SRC"; need_file "$NTG_SRC"
# Guard against accidental rebuilds / wrong files: sizes are pinned.
[ "$(stat -c%s "$TDJSON_SRC")" = "50166944" ] || fail "$TDJSON_SRC has an unexpected size (expected 50166944 B)"
[ "$(stat -c%s "$NTG_SRC")" = "98121040" ] || fail "$NTG_SRC has an unexpected size (expected 98121040 B)"
cp "$TDJSON_SRC" "$STAGE/lib/libtdjson.so.1.8.69"
cp "$NTG_SRC" "$STAGE/lib/libntgcalls.so"

# --- 5. JS services (sources only — no node_modules, no data, no tests) -----
echo "==> js services"
rsync -a --exclude node_modules --exclude data --exclude tests \
  --exclude Dockerfile --exclude PROGRESS.md --exclude '*.node' \
  services/vc-ts/ "$STAGE/services/vc-ts/"
rsync -a --exclude node_modules --exclude data --exclude tests \
  --exclude Dockerfile --exclude PROGRESS.md \
  services/gui-js/ "$STAGE/services/gui-js/"
rsync -a --exclude node_modules --exclude data --exclude tests \
  --exclude Dockerfile --exclude PROGRESS.md \
  services/radio-js/ "$STAGE/services/radio-js/"
need_file "$STAGE/services/vc-ts/src/main.js"
need_file "$STAGE/services/gui-js/app/main.js"
need_file "$STAGE/services/radio-js/src/main.js"

# --- 6. downloaders (Python sources + requirements.txt only) ----------------
echo "==> downloaders"
rsync -a --exclude .venv --exclude venv --exclude __pycache__ --exclude '*.pyc' \
  --exclude tests --exclude Dockerfile --exclude PROGRESS.md \
  services/downloaders/ "$STAGE/services/downloaders/"
need_file "$STAGE/services/downloaders/requirements.txt"
need_file "$STAGE/services/downloaders/src/main.py"

# --- 7. Root files -----------------------------------------------------------
echo "==> root files"
for f in "${LAUNCHERS[@]}"; do
  [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$STAGE/$f"
done
cp "$ROOT/.env.example" "$STAGE/.env.example"
cp "$ROOT/README.md" "$STAGE/README.md"
cp "$ROOT/INSTALL.md" "$STAGE/INSTALL.md"
cp "$ROOT/VERSION" "$STAGE/VERSION"

# --- 8. Secret sweep (before the tarball exists) -----------------------------
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
       if (v != "" && v !~ /^(https?:\/\/.*|mongodb:\/\/.*|redis:\/\/.*|[0-9]+|low-ram|data|\.\/downloads|MP3_320|gemini-2\.0-flash|ParlayBot|python3)$/) {
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

# --- 9. SHA256SUMS inside the tarball + pack --------------------------------
echo "==> checksums + pack"
(cd "$STAGE" && find . -type f -not -name SHA256SUMS -print0 | sort -z \
  | xargs -0 sha256sum > SHA256SUMS)
# --owner/--group: the sandbox files are root:nogroup; normalize to root:root
# so extraction on a VPS (as root, or as a user who then chowns) is sane.
tar --owner=0 --group=0 --numeric-owner -czf "$ARCHIVE" -C "$STAGE_PARENT" "$DIST"
sha256sum "$ARCHIVE" > "$ARCHIVE_SHA"
rm -rf "$STAGE_PARENT"

# --- 10. Report ---------------------------------------------------------------
echo
echo "Built: $ARCHIVE"
echo "Size:  $(du -h "$ARCHIVE" | cut -f1) ($(stat -c%s "$ARCHIVE") bytes)"
echo "Files: $(tar tzf "$ARCHIVE" | wc -l)"
echo "SHA256: $(cut -d' ' -f1 "$ARCHIVE_SHA")"
echo "Top 10 largest files in the tarball:"
tar -tzvf "$ARCHIVE" | awk '{print $3, $6}' | sort -rn | head -10
