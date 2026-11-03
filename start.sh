#!/usr/bin/env bash
# Parlay — native launcher (no Docker). Starts every service in
# dependency order, waits for each GET /healthz (30 s), and prints a
# status table. Logs go to logs/<service>.log, PIDs to run/<service>.pid.
#
# Layouts: works from an extracted release tarball (bin/*) AND from a
# repo checkout (services/*/build or target/release, go run fallback).
# The core starts LAST, after all engines are up.
#
# Usage: ./start.sh
#   Refuses to start if BOT_TOKEN is missing or if any service is
#   already running (run ./stop.sh first). Exits non-zero if a required
#   service fails its health check — the others keep running, and the
#   report names the failed ones.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [ -x "$ROOT/bin/parlay-core" ]; then LAYOUT="tarball"; else LAYOUT="repo"; fi

# name|port|required|health-path  — start order is dependency order.
SERVICES=(
    "slskd|5030|optional|/"
    "downloaders|8009|required|/healthz"
    "netease|8010|required|/healthz"
    "spotify|8007|required|/healthz"
    "vc-go|8001|required|/healthz"
    "vc-ts|8002|required|/healthz"
    "vc-rust|8003|required|/healthz"
    "audio-fx|8004|required|/healthz"
    "share|8005|required|/healthz"
    "queue|8006|required|/healthz"
    "radio|8008|required|/healthz"
    "gui|3000|required|/healthz"
    "core|8080|required|/healthz"
)

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
pid_alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

# ---------------------------------------------------------------- .env
[ -f "$ROOT/.env" ] || die "no .env at $ROOT/.env — run ./install.sh first."
chmod 600 "$ROOT/.env" 2>/dev/null || true
# shellcheck disable=SC1091
set -a; . "$ROOT/.env"; set +a
[ -n "${BOT_TOKEN:-}" ] || die "BOT_TOKEN is missing from .env — set it (./install.sh can create .env) and re-run ./start.sh."
[ -n "${PARLAY_TOKEN:-}" ] && export PARLAY_TOKEN || say "warning: PARLAY_TOKEN is empty — IPC auth is OFF (dev only)."

mkdir -p "$ROOT/run" "$ROOT/logs" "$ROOT/data"
export PATH="$ROOT/bin:$ROOT/.venv/bin:$PATH"

# Mongo/Redis are optional: JSON-file stores are the default while Mongo
# stays aside. A set-but-unreachable MONGO_URI is fatal for vc-go (its
# README documents this), so only pass these URIs through when the
# server actually answers; otherwise the services fall back to files.
probe_uri() { # uri default_port -> 0 = reachable
    local uri="$1" defport="$2" hostport host port
    hostport="${uri#*://}"; hostport="${hostport%%/*}"
    host="${hostport%%:*}"; port="${hostport##*:}"
    [ "$host" = "$hostport" ] && port="$defport"
    [ -n "$host" ] || return 1
    if command -v timeout >/dev/null 2>&1; then
        timeout 3 bash -c "( exec 3<>\"/dev/tcp/$host/$port\" )" 2>/dev/null
    else
        ( exec 3<>"/dev/tcp/$host/$port" ) 2>/dev/null
    fi
}
if [ -n "${MONGO_URI:-}" ]; then
    if probe_uri "$MONGO_URI" 27017; then say "mongo reachable — using MONGO_URI.";
    else say "warning: MONGO_URI is set but mongo is unreachable — starting with JSON-file stores (MONGO_URI ignored for this run)."; unset MONGO_URI; fi
fi
if [ -n "${REDIS_URL:-}" ]; then
    if probe_uri "$REDIS_URL" 6379; then say "redis reachable — using REDIS_URL.";
    else say "warning: REDIS_URL is set but redis is unreachable — starting without redis (REDIS_URL ignored for this run)."; unset REDIS_URL; fi
fi

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

# ---------------------------------------------------------------- health
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
# Sets CMD (array), WORKDIR, ENVS (array of KEY=val) for a service.
GO_BIN=""
if command -v go >/dev/null 2>&1; then GO_BIN="$(command -v go)"
elif [ -x "$ROOT/.tools/go/bin/go" ]; then GO_BIN="$ROOT/.tools/go/bin/go"; fi

resolve_service() { # name
    local name="$1" bin=""
    CMD=(); ENVS=(); WORKDIR="$ROOT"
    case "$name" in
        core)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-core";
            else bin="$ROOT/services/core-cpp/build/parlay-core"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ENVS+=("VC_GO_URL=http://127.0.0.1:8001" "VC_TS_URL=http://127.0.0.1:8002"
                   "VC_RUST_URL=http://127.0.0.1:8003" "FX_URL=http://127.0.0.1:8004"
                   "SHARE_URL=http://127.0.0.1:8005" "QUEUE_URL=http://127.0.0.1:8006"
                   "SPOTIFY_URL=http://127.0.0.1:8007" "RADIO_URL=http://127.0.0.1:8008"
                   "DL_URL=http://127.0.0.1:8009" "DOWNLOADERS_URL=http://127.0.0.1:8009"
                   "NETEASE_URL=http://127.0.0.1:8010")
            if [ -d "$ROOT/lib" ]; then
                ENVS+=("LD_LIBRARY_PATH=$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
            fi
            [ -n "${TDJSON_LIB:-}" ] || [ -f "$ROOT/lib/libtdjson.so.1.8.69" ] \
                && ENVS+=("TDJSON_LIB=${TDJSON_LIB:-$ROOT/lib/libtdjson.so.1.8.69}")
            [ -n "${NTGCALLS_LIB:-}" ] || [ -f "$ROOT/lib/libntgcalls.so" ] \
                && ENVS+=("NTGCALLS_LIB=${NTGCALLS_LIB:-$ROOT/lib/libntgcalls.so}")
            ;;
        vc-go)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-vc-go";
            elif [ -x "$ROOT/services/vc-go/parlay-vc" ]; then bin="$ROOT/services/vc-go/parlay-vc"; fi
            if [ -n "$bin" ]; then [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }; CMD=("$bin");
            elif [ -n "$GO_BIN" ]; then CMD=("$GO_BIN" "run" "./cmd/parlay-vc"); WORKDIR="$ROOT/services/vc-go";
            else echo "MISSING:go (no vc-go binary and no Go toolchain)"; return 1; fi
            ;;
        netease)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-netease";
            elif [ -x "$ROOT/services/netease-go/parlay-netease" ]; then bin="$ROOT/services/netease-go/parlay-netease"; fi
            if [ -n "$bin" ]; then [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }; CMD=("$bin");
            elif [ -n "$GO_BIN" ]; then CMD=("$GO_BIN" "run" "./cmd/parlay-netease"); WORKDIR="$ROOT/services/netease-go";
            else echo "MISSING:go (no netease binary and no Go toolchain)"; return 1; fi
            ENVS+=("PARLAY_DL_DIR=$ROOT/data/netease")
            ;;
        vc-rust)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-vc-rust";
            else bin="$ROOT/services/vc-rust/target/release/parlay-vc-rust"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ;;
        audio-fx)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-audio-fx";
            else bin="$ROOT/services/audio-fx-rust/target/release/parlay-audio-fx"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ;;
        share)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-share";
            else bin="$ROOT/services/share-rust/target/release/parlay-share"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ;;
        queue)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-queue";
            else bin="$ROOT/services/queue-rust/target/release/parlay-queue"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ;;
        spotify)
            if [ "$LAYOUT" = "tarball" ]; then bin="$ROOT/bin/parlay-spotify";
            else bin="$ROOT/services/spotify-rust/target/release/parlay-spotify"; fi
            [ -x "$bin" ] || { echo "MISSING:$bin"; return 1; }
            CMD=("$bin")
            ;;
        vc-ts)
            [ -f "$ROOT/services/vc-ts/src/main.js" ] || { echo "MISSING:$ROOT/services/vc-ts/src/main.js"; return 1; }
            CMD=("node" "$ROOT/services/vc-ts/src/main.js")
            if [ -x "$ROOT/bin/yt-dlp" ]; then ENVS+=("YT_DLP=$ROOT/bin/yt-dlp");
            elif [ -x "$ROOT/.venv/bin/yt-dlp" ]; then ENVS+=("YT_DLP=$ROOT/.venv/bin/yt-dlp"); fi
            ;;
        radio)
            [ -f "$ROOT/services/radio-js/src/main.js" ] || { echo "MISSING:$ROOT/services/radio-js/src/main.js"; return 1; }
            CMD=("node" "$ROOT/services/radio-js/src/main.js")
            ENVS+=("VC_GO_URL=http://127.0.0.1:8001" "VC_TS_URL=http://127.0.0.1:8002")
            ;;
        gui)
            [ -f "$ROOT/services/gui-js/app/main.js" ] || { echo "MISSING:$ROOT/services/gui-js/app/main.js"; return 1; }
            CMD=("node" "$ROOT/services/gui-js/app/main.js")
            ENVS+=("CORE_URL=http://127.0.0.1:8080" "VC_GO_URL=http://127.0.0.1:8001"
                   "VC_TS_URL=http://127.0.0.1:8002" "NETEASE_URL=http://127.0.0.1:8010")
            ;;
        downloaders)
            [ -f "$ROOT/services/downloaders/src/main.py" ] || { echo "MISSING:$ROOT/services/downloaders/src/main.py"; return 1; }
            if [ -x "$ROOT/.venv/bin/python" ]; then CMD=("$ROOT/.venv/bin/python" "-m" "src.main");
            else CMD=("python3" "-m" "src.main"); say "warning: no .venv — downloaders run on system python3."; fi
            WORKDIR="$ROOT/services/downloaders"
            ENVS+=("PARLAY_DL_DIR=$ROOT/data/downloads" "SLSKD_URL=http://127.0.0.1:5030")
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
say "Parlay ${LAYOUT} layout at $ROOT — starting services…"
echo ""
print_row "SERVICE" "PORT" "STATUS" "DETAIL"
FAILED=()
STARTED=()

for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port required health <<< "$entry"
    if [ "$name" = "slskd" ] && ! slskd_enabled; then
        print_row "slskd" "5030" "SKIPPED" "opt-in, needs Soulseek account (SLSKD_ENABLED=1 + SLSKD_SLSK_USERNAME/PASSWORD in .env)"
        continue
    fi
    mkdir -p "$ROOT/data/$name"
    if ! resolve_service "$name" >"$ROOT/run/.resolve.out" 2>&1; then
        missing="$(cat "$ROOT/run/.resolve.out" 2>/dev/null || true)"
        rm -f "$ROOT/run/.resolve.out"
        print_row "$name" "$port" "FAIL" "missing: ${missing#MISSING:}"
        FAILED+=("$name (missing: ${missing#MISSING:})")
        continue
    fi
    rm -f "$ROOT/run/.resolve.out"
    log="$ROOT/logs/$name.log"
    pidf="$ROOT/run/$name.pid"
    ( cd "$WORKDIR" && exec env "PORT=$port" "PARLAY_DATA_DIR=$ROOT/data/$name" "${ENVS[@]}" "${CMD[@]}" ) >>"$log" 2>&1 &
    echo $! > "$pidf"
    any=0; [ "$name" = "slskd" ] && any=1   # slskd has no /healthz; any HTTP response counts
    if wait_health "$name" "$port" "$health" 30 "$any" "$(cat "$pidf")"; then
        print_row "$name" "$port" "OK" "pid $(cat "$pidf") — http://127.0.0.1:$port$health"
        STARTED+=("$name")
    else
        pid="$(cat "$pidf" 2>/dev/null || true)"
        if pid_alive "$pid"; then detail="pid $pid alive but no healthy /healthz in 30s — see $log";
        else detail="process exited — see $log"; fi
        print_row "$name" "$port" "FAIL" "$detail"
        FAILED+=("$name ($detail)")
    fi
done

echo ""
if [ "${#FAILED[@]}" -gt 0 ]; then
    say "FAILED services (the rest keep running):"
    for f in "${FAILED[@]}"; do say "  - $f"; done
    say "Check the logs in $ROOT/logs/ and re-run ./start.sh after ./stop.sh, or fix .env and retry."
    exit 1
fi
say "All services up. GUI: http://127.0.0.1:3000 — core API: http://127.0.0.1:8080/healthz"
say "Stop everything with ./stop.sh; check status with ./status.sh."
