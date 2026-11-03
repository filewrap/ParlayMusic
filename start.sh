#!/usr/bin/env bash
# Parlay — native launcher (no Docker). Starts the 2-process engine in
# dependency order and prints a status table. Logs go to logs/<name>.log,
# PIDs to run/<name>.pid.
#
#   backend  Go unified backend (services/backend-go/cmd/backend):
#            owns playback/queue/vote/share/Spotify/settings/radio state
#            in the embedded bbolt store (data/parlay.db) and answers
#            the core over the persistent UDS socket
#            data/parlay-backend.sock. It spawns the persistent yt-dlp
#            daemon itself at boot (scripts/yt_dlp_worker.py on the
#            scripts/yt-dlp-venv python) — there is no separate yt-dlp
#            process to start. Readiness = socket file present + PID alive.
#   core     C++ core (services/core-cpp/build/parlay-core): dual TDLib
#            sessions (bot in data/tdlib-bot, assistant in data/tdlib),
#            HTTP API on :8080 (GET /healthz). Starts LAST, after the
#            backend socket is up — the core connects to it at startup.
#   slskd    optional Soulseek daemon on :5030 (opt-in, see below).
#
# There are no other services: no inter-service HTTP, no Mongo/Redis,
# no per-service ports 8001-8010/3000. The backend has no HTTP port.
#
# Layouts: works from an extracted release tarball (bin/*) AND from a
# repo checkout (services/core-cpp/build, services/backend-go built
# on demand with the Go toolchain when no binary is present yet).
#
# Usage: ./start.sh
#   Refuses to start if BOT_TOKEN is missing or if any process is
#   already running (run ./stop.sh first). Exits non-zero if a required
#   process fails its readiness check — the others keep running, and
#   the report names the failed ones.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [ -x "$ROOT/bin/parlay-core" ]; then LAYOUT="tarball"; else LAYOUT="repo"; fi

# name|kind — start order is dependency order. kind "socket" waits for
# the UDS socket file; kind "http" waits for GET /healthz on PORT.
SERVICES=(
    "backend|socket"
    "core|http"
    "slskd|http"
)
BACKEND_SOCK="$ROOT/data/parlay-backend.sock"
CORE_PORT="${PORT:-8080}"
SLSKD_PORT=5030

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
pid_alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

# ---------------------------------------------------------------- .env
[ -f "$ROOT/.env" ] || die "no .env at $ROOT/.env — run ./install.sh first."
chmod 600 "$ROOT/.env" 2>/dev/null || true
# shellcheck disable=SC1091
set -a; . "$ROOT/.env"; set +a
[ -n "${BOT_TOKEN:-}" ] || die "BOT_TOKEN is missing from .env — set it (./install.sh can create .env) and re-run ./start.sh."
[ -n "${PARLAY_TOKEN:-}" ] && export PARLAY_TOKEN || say "warning: PARLAY_TOKEN is empty — the core will warn at startup (engine calls go out unauthenticated)."

mkdir -p "$ROOT/run" "$ROOT/logs" "$ROOT/data" "$ROOT/data/downloads"
export PATH="$ROOT/bin:$PATH"

# ---------------------------------------------------------------- double-start
RUNNING=()
for entry in "${SERVICES[@]}"; do
    name="${entry%%|*}"
    pidf="$ROOT/run/$name.pid"
    if [ -f "$pidf" ]; then
        pid="$(cat "$pidf" 2>/dev/null || true)"
        if pid_alive "$pid"; then RUNNING+=("$name (pid $pid)"); else rm -f "$pidf"; fi
    fi
done
if [ "${#RUNNING[@]}" -gt 0 ]; then
    die "Parlay is already running: ${RUNNING[*]} — run ./stop.sh first."
fi

# ---------------------------------------------------------------- readiness
wait_socket() { # socket-path timeout_secs pid
    local sock="$1" timeout="${2:-60}" pid="${3:-}" i
    for ((i = 0; i < timeout; i++)); do
        [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null && return 1  # died: stop waiting
        [ -S "$sock" ] && return 0
        sleep 1
    done
    return 1
}
wait_health() { # name port path timeout_secs any-http-code(0/1) pid
    local name="$1" port="$2" path="$3" timeout="${4:-30}" any="${5:-0}" pid="${6:-}" i code
    for ((i = 0; i < timeout; i++)); do
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then return 1; fi  # died: stop waiting
        code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:${port}${path}" 2>/dev/null || true)"
        case "$code" in
            2*) return 0 ;;
            *)  [ "$any" = "1" ] && [ "$code" != "000" ] && return 0 ;;
        esac
        sleep 1
    done
    return 1
}

print_row() { printf '%-12s %-6s %-8s %s\n' "$1" "$2" "$3" "$4"; }

# ---------------------------------------------------------------- resolve
# Sets CMD (array), WORKDIR, ENVS (array of KEY=val) for a process.
GO_BIN=""
if [ -x "$ROOT/.tools/go/bin/go" ]; then GO_BIN="$ROOT/.tools/go/bin/go";
elif command -v go >/dev/null 2>&1; then GO_BIN="$(command -v go)"; fi

resolve_service() { # name
    local name="$1" bin=""
    CMD=(); ENVS=(); WORKDIR="$ROOT"
    case "$name" in
        backend)
            # The backend finds the yt-dlp venv + worker script by
            # walking up from its working directory
            # (scripts/yt-dlp-venv/bin/python, scripts/yt_dlp_worker.py),
            # so it MUST run with CWD=$ROOT in both layouts. Its UDS
            # socket + bbolt store live under PARLAY_DATA_DIR.
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-backend";
            elif [ -x "$ROOT/bin/parlay-backend" ]; then bin="$ROOT/bin/parlay-backend"; fi
            if [ -n "$bin" ]; then [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }; CMD=("$bin");
            elif [ -n "$GO_BIN" ]; then
                say "building backend (no binary yet — one-time, cgo + ntgcalls)…"
                ( cd "$ROOT/services/backend-go" && "$GO_BIN" build -trimpath -o "$ROOT/bin/parlay-backend" ./cmd/backend ) \
                    || { echo "MISSING:backend build failed (see above)"; return 1; }
                CMD=("$ROOT/bin/parlay-backend")
            else echo "MISSING:go (no parlay-backend binary and no Go toolchain)"; return 1; fi
            ENVS+=("PARLAY_DATA_DIR=$ROOT/data")
            # The backend binary's baked rpath points at the repo's
            # refs/ntgcalls dir; in the tarball layout libntgcalls.so
            # lives in lib/ instead, so put lib/ first on the search.
            if [ -d "$ROOT/lib" ]; then
                ENVS+=("LD_LIBRARY_PATH=$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
            fi
            ;;
        core)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-core";
            else bin="$ROOT/services/core-cpp/build/parlay-core"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin (repo checkout: build it first — cmake -S services/core-cpp -B services/core-cpp/build -DCMAKE_BUILD_TYPE=Release && cmake --build services/core-cpp/build)"; return 1; }
            CMD=("$bin")
            # libtdjson is linked at build time; the binary's RUNPATH
            # already covers lib/ next to bin/ in the tarball layout.
            # TDLib session dirs default to $PARLAY_DATA_DIR/tdlib-bot
            # (bot) and $PARLAY_DATA_DIR/tdlib (assistant); the core
            # creates them mode 0700 itself.
            ENVS+=("PARLAY_DATA_DIR=$ROOT/data")
            ;;
        slskd)
            if [ -x "$ROOT/bin/slskd" ]; then bin="$ROOT/bin/slskd";
            elif [ -x "$ROOT/phase5/slskd/slskd-bin/slskd" ]; then bin="$ROOT/phase5/slskd/slskd-bin/slskd"; fi
            [ -n "$bin" ] || { echo "MISSING:bin/slskd (run ./install.sh to fetch it)"; return 1; }
            CMD=("$bin" "--app-dir" "$ROOT/data/slskd" "--no-logo")
            ;;
    esac
    return 0
}

# ---------------------------------------------------------------- slskd gate
slskd_enabled() {
    case "${SLSKD_ENABLED:-0}" in 1|true|yes|on) : ;; *) return 1 ;; esac
    [ -n "${SLSKD_SLSK_USERNAME:-}" ] && [ -n "${SLSKD_SLSK_PASSWORD:-}" ]
}

# ---------------------------------------------------------------- start loop
say "Parlay ${LAYOUT} layout at $ROOT — starting the engine…"
echo ""
print_row "SERVICE" "PORT" "STATUS" "DETAIL"
FAILED=()
STARTED=()

for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name kind <<< "$entry"
    if [ "$name" = "slskd" ] && ! slskd_enabled; then
        print_row "slskd" "$SLSKD_PORT" "SKIPPED" "opt-in, needs Soulseek account (SLSKD_ENABLED=1 + SLSKD_SLSK_USERNAME/PASSWORD in .env)"
        continue
    fi
    [ "$name" = "slskd" ] && mkdir -p "$ROOT/data/slskd"
    if ! resolve_service "$name" >"$ROOT/run/.resolve.out" 2>&1; then
        missing="$(cat "$ROOT/run/.resolve.out" 2>/dev/null || true)"
        rm -f "$ROOT/run/.resolve.out"
        print_row "$name" "-" "FAIL" "missing: ${missing#MISSING:}"
        FAILED+=("$name (missing: ${missing#MISSING:})")
        continue
    fi
    rm -f "$ROOT/run/.resolve.out"
    log="$ROOT/logs/$name.log"
    pidf="$ROOT/run/$name.pid"
    ( cd "$WORKDIR" && exec env "${ENVS[@]}" "${CMD[@]}" ) >>"$log" 2>&1 &
    echo $! > "$pidf"
    pid="$(cat "$pidf")"
    if [ "$kind" = "socket" ]; then
        # The backend warms the yt-dlp daemon (up to 30 s) before it
        # listens, so allow 60 s for the socket to appear.
        if wait_socket "$BACKEND_SOCK" 60 "$pid"; then
            print_row "$name" "-" "OK" "pid $pid — socket $BACKEND_SOCK"
            STARTED+=("$name")
        else
            if pid_alive "$pid"; then detail="pid $pid alive but no UDS socket in 60s — see $log";
            else detail="process exited — see $log"; fi
            print_row "$name" "-" "FAIL" "$detail"
            FAILED+=("$name ($detail)")
        fi
    elif [ "$name" = "slskd" ]; then
        # slskd has no /healthz; any HTTP response counts.
        if wait_health "$name" "$SLSKD_PORT" "/" 30 1 "$pid"; then
            print_row "$name" "$SLSKD_PORT" "OK" "pid $pid — http://127.0.0.1:$SLSKD_PORT/"
            STARTED+=("$name")
        else
            if pid_alive "$pid"; then detail="pid $pid alive but no HTTP response in 30s — see $log";
            else detail="process exited — see $log"; fi
            print_row "$name" "$SLSKD_PORT" "FAIL" "$detail"
            FAILED+=("$name ($detail)")
        fi
    else
        if wait_health "$name" "$CORE_PORT" "/healthz" 30 0 "$pid"; then
            print_row "$name" "$CORE_PORT" "OK" "pid $pid — http://127.0.0.1:$CORE_PORT/healthz"
            STARTED+=("$name")
        else
            if pid_alive "$pid"; then detail="pid $pid alive but no healthy /healthz in 30s — see $log";
            else detail="process exited — see $log"; fi
            print_row "$name" "$CORE_PORT" "FAIL" "$detail"
            FAILED+=("$name ($detail)")
        fi
    fi
done

echo ""
if [ "${#FAILED[@]}" -gt 0 ]; then
    say "FAILED services (the rest keep running):"
    for f in "${FAILED[@]}"; do say "  - $f"; done
    say "Check the logs in $ROOT/logs/ and re-run ./start.sh after ./stop.sh, or fix .env and retry."
    exit 1
fi
say "Engine up: backend on $BACKEND_SOCK — core API: http://127.0.0.1:$CORE_PORT/healthz"
say "Stop everything with ./stop.sh; check status with ./status.sh."
