#!/usr/bin/env bash
# Parlay — status table without starting anything.
# For each service: PID alive? + GET /healthz. Exits 0 only when every
# required service is UP (slskd shows SKIPPED when it is not enabled).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# name|port|required|health-path — same table as start.sh.
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
for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port required health <<< "$entry"
    if [ "$name" = "slskd" ] && ! slskd_enabled; then
        printf '%-12s %-6s %-8s %s\n' "slskd" "5030" "SKIPPED" "opt-in, needs Soulseek account"
        continue
    fi
    pid="$(cat "$ROOT/run/$name.pid" 2>/dev/null || true)"
    any=0; [ "$name" = "slskd" ] && any=1
    if pid_alive "$pid"; then
        hc="$(health_code "$port" "$health" "$any")"
        case "$hc" in
            UP*) status="UP" ;;
            *)   status="DOWN"; [ "$required" = "required" ] && ALL_UP=0 ;;
        esac
        printf '%-12s %-6s %-8s %s\n' "$name" "$port" "$status" "pid $pid, $hc"
    else
        [ "$required" = "required" ] && ALL_UP=0
        printf '%-12s %-6s %-8s %s\n' "$name" "$port" "DOWN" "not running (no live PID)"
    fi
done
exit $((1 - ALL_UP))
