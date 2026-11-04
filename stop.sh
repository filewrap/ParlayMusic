#!/usr/bin/env bash
# Parlay — stop the engine started by ./start.sh.
# Kills PIDs from run/*.pid (TERM, then KILL after 10 s); for the core
# it falls back to killing exactly the process that owns its HTTP port
# (via ss). The backend has no TCP port, so it is stopped by PID only.
# It deliberately never uses pkill by name: a substring pattern also
# matches build commands (e.g. `go build -o .../bin/parlay-backend`)
# and unrelated shells.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# name|port — same processes as start.sh (reverse order stops the core
# first, then the backend, then slskd). Empty port = PID only, no
# port fallback or port verification.
SERVICES=(
    "core|8080"
    "backend|"
    "slskd|5030"
)

say() { printf '%s\n' "$*"; }
pid_alive() {
    [ -n "${1:-}" ] || return 1
    local st
    st="$(ps -o state= -p "$1" 2>/dev/null | tr -d ' ')" || return 1
    [ -n "$st" ] && [ "$st" != "Z" ]
}
port_free() { # 0 = free, 1 = still bound
    ( exec 3<>"/dev/tcp/127.0.0.1/$1" ) 2>/dev/null && return 1 || return 0
}
port_owner_pids() { # port -> owning PIDs, one per line (ss; empty if unknown)
    ss -ltnp "sport = :$1" 2>/dev/null | grep -o 'pid=[0-9]*' | sed 's/pid=//' | sort -u
}
kill_pid_gracefully() { # pid -> 0 if dead afterwards
    local pid="$1" i
    kill -TERM "$pid" 2>/dev/null || return 0
    for i in $(seq 1 10); do pid_alive "$pid" || return 0; sleep 1; done
    kill -KILL "$pid" 2>/dev/null || true
    sleep 1
    pid_alive "$pid" && return 1 || return 0
}

printf '%-12s %-6s %s\n' "SERVICE" "PORT" "RESULT"
STOPPED=0
STILL_RUNNING=()
for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port <<< "$entry"
    pidf="$ROOT/run/$name.pid"
    pid="$(cat "$pidf" 2>/dev/null || true)"
    result="not running"
    stopped_this=0
    if pid_alive "$pid"; then
        if kill_pid_gracefully "$pid"; then result="stopped (pid $pid)";
        else result="STILL RUNNING (pid $pid, SIGKILL failed)"; STILL_RUNNING+=("$name (pid $pid)"); fi
        stopped_this=1
    fi
    # Fallback (TCP services only): if the port is still bound, kill
    # exactly the process that owns it (ss). Never pkill by name
    # substring: that also matches build commands (e.g.
    # `go build -o .../parlay-backend`) and other shells whose
    # command line merely mentions the service.
    if [ -n "$port" ] && ! port_free "$port"; then
        owners="$(port_owner_pids "$port")"
        if [ -n "$owners" ]; then
            for owner in $owners; do
                [ "$owner" = "$$" ] && continue
                kill_pid_gracefully "$owner" || true
            done
            if [ "$result" = "not running" ]; then result="stopped (port owner)"; fi
            stopped_this=1
        fi
    fi
    [ "$stopped_this" = "1" ] && STOPPED=$((STOPPED + 1))
    rm -f "$pidf"
    printf '%-12s %-6s %s\n' "$name" "${port:--}" "$result"
done

echo ""
STILL_BOUND=()
for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port <<< "$entry"
    if [ -n "$port" ] && ! port_free "$port"; then STILL_BOUND+=("$name:$port"); fi
done
if [ "${#STILL_RUNNING[@]}" -gt 0 ]; then
    say "warning: still running after stop: ${STILL_RUNNING[*]}"
    exit 1
fi
if [ "${#STILL_BOUND[@]}" -gt 0 ]; then
    say "warning: still bound after stop: ${STILL_BOUND[*]} — something else holds those ports."
    say "Check with: ss -ltnp | grep -E '$(printf '%s|' "${STILL_BOUND[@]#*:}" | sed 's/|$//')'"
    exit 1
fi
say "All Parlay processes stopped ($STOPPED stopped this run); core/slskd ports free."
say "Note: a stale data/parlay-backend.sock file (if any) is removed automatically on the next start."
