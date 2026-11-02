#!/bin/bash
# usage: run-one.sh <logfile> <workdir> <cmd...>
LOG="$1"; DIR="$2"; shift 2
{
  echo "CMD: $*"
  echo "CWD: $DIR"
  echo "START: $(date -Iseconds)"
  S=$(date +%s)
  ( cd "$DIR" && "$@" )
  EC=$?
  E=$(date +%s)
  echo "END: $(date -Iseconds)"
  echo "DURATION_SEC: $((E-S))"
  echo "EXIT: $EC"
} > "$LOG" 2>&1
