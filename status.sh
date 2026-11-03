#!/usr/bin/env bash
# Parlay — status table without starting anything.
#   backend: PID alive? + UDS socket file present?
#   core:    PID alive? + GET /healthz on :8080
#   slskd:   PID alive? + any HTTP response on :5030 (optional; shows
#            SKIPPED when it is not enabled).
# Exits 0 only when every required process (backend, core) is UP.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BACKEND_SOCK="$ROOT/data/parlay-backend.sock"
CORE_PORT="${PORT:-8080}"
SLSKD_PORT=5030

pid_alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
health_code() { # port path any(0/1) -> prints UP/DOWN (+code)
    local code
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$1$2" 2>/dev/null || true)"
    case "$code" in
        2*) echo "UP (HTTP $code)" ;;
        *)  [ "${3:-0}" = "1" ] && [ "$code" != "000" ] && [ -n "$code" ] && echo "UP (HTTP $code)" || echo "DOWN (HTTP ${code:-000})" ;;
    esac
}

slskd_enabled() {
    [ -f "$ROOT/.env" ] || return 1
    # shellcheck disable=SC1091
    set -a; . "$ROOT/.env"; set +a
    case "${SLSKD_ENABLED:-0}" in 1|true|yes|on) : ;; *) return 1 ;; esac
    [ -n "${SLSKD_SLSK_USERNAME:-}" ] && [ -n "${SLSKD_SLSK_PASSWORD:-}" ]
}

printf '%-12s %-6s %-8s %s\n' "SERVICE" "PORT" "STATUS" "DETAIL"
ALL_UP=1

# --- backend (UDS socket, no HTTP port) ---
pid="$(cat "$ROOT/run/backend.pid" 2>/dev/null || true)"
if pid_alive "$pid"; then
    if [ -S "$BACKEND_SOCK" ]; then status="UP"; detail="pid $pid, socket $BACKEND_SOCK";
    else status="DOWN"; ALL_UP=0; detail="pid $pid alive but no UDS socket at $BACKEND_SOCK"; fi
else
    status="DOWN"; ALL_UP=0; detail="not running (no live PID)"
fi
printf '%-12s %-6s %-8s %s\n' "backend" "-" "$status" "$detail"

# --- core (HTTP :8080 /healthz) ---
pid="$(cat "$ROOT/run/core.pid" 2>/dev/null || true)"
if pid_alive "$pid"; then
    hc="$(health_code "$CORE_PORT" "/healthz" 0)"
    case "$hc" in
        UP*) status="UP" ;;
        *)   status="DOWN"; ALL_UP=0 ;;
    esac
    printf '%-12s %-6s %-8s %s\n' "core" "$CORE_PORT" "$status" "pid $pid, $hc"
else
    ALL_UP=0
    printf '%-12s %-6s %-8s %s\n' "core" "$CORE_PORT" "DOWN" "not running (no live PID)"
fi

# --- slskd (optional) ---
if ! slskd_enabled; then
    printf '%-12s %-6s %-8s %s\n' "slskd" "$SLSKD_PORT" "SKIPPED" "opt-in, needs Soulseek account"
else
    pid="$(cat "$ROOT/run/slskd.pid" 2>/dev/null || true)"
    if pid_alive "$pid"; then
        hc="$(health_code "$SLSKD_PORT" "/" 1)"
        case "$hc" in
            UP*) status="UP" ;;
            *)   status="DOWN" ;;
        esac
        printf '%-12s %-6s %-8s %s\n' "slskd" "$SLSKD_PORT" "$status" "pid $pid, $hc"
    else
        printf '%-12s %-6s %-8s %s\n' "slskd" "$SLSKD_PORT" "DOWN" "not running (no live PID)"
    fi
fi
exit $((1 - ALL_UP))
