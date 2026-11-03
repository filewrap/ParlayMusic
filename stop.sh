#!/usr/bin/env bash
# Parlay — stop every service started by ./start.sh.
# Kills PIDs from run/*.pid (TERM, then KILL after 10 s), falls back to
# killing exactly the process that owns the service's port (via ss),
# then verifies every service port is free. It deliberately never uses
# pkill by name: a substring pattern also matches build commands
# (e.g. `go build -o .../bin/parlay-vc-go`) and unrelated shells.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# name|port — same table as start.sh (reverse order stops the core first).
SERVICES=(
    "core|8080"
    "gui|3000"
    "radio|8008"
    "queue|8006"
    "share|8005"
    "audio-fx|8004"
    "vc-rust|8003"
    "vc-ts|8002"
    "vc-go|8001"
    "spotify|8007"
    "netease|8010"
    "downloaders|8009"
    "slskd|5030"
)

say() { printf '%s\n' "$*"; }
pid_alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
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
for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port <<< "$entry"
    pidf="$ROOT/run/$name.pid"
    pid="$(cat "$pidf" 2>/dev/null || true)"
    result="not running"
    stopped_this=0
    if pid_alive "$pid"; then
        if kill_pid_gracefully "$pid"; then result="stopped (pid $pid)";
        else result="STILL RUNNING (pid $pid, SIGKILL failed)"; fi
        stopped_this=1
    fi
    # Fallback: if the port is still bound, kill exactly the process that
    # owns it (ss). Never pkill by name substring: that also matches
    # build commands (e.g. `go build -o .../parlay-vc-go`) and other
    # shells whose command line merely mentions the service.
    if ! port_free "$port"; then
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
    printf '%-12s %-6s %s\n' "$name" "$port" "$result"
done

echo ""
STILL_BOUND=()
for entry in "${SERVICES[@]}"; do
    IFS='|' read -r name port <<< "$entry"
    if ! port_free "$port"; then STILL_BOUND+=("$name:$port"); fi
done
if [ "${#STILL_BOUND[@]}" -gt 0 ]; then
    say "warning: still bound after stop: ${STILL_BOUND[*]} — something else holds those ports."
    say "Check with: ss -ltnp | grep -E '$(printf '%s|' "${STILL_BOUND[@]#*:}" | sed 's/|$//')'"
    exit 1
fi
say "All Parlay services stopped ($STOPPED stopped this run); all ports free."
