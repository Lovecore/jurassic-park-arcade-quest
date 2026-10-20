#!/bin/sh
# Runs the differential harness over every candidate, restarting after a
# function that kills the process outright (e.g. the original code loads
# esp from garbage, so Windows cannot even deliver the fault). Those are
# reported as CRASH lines.
#   tools/recomp/harness/run_all.sh <patched game> [trials] [seed] > results.txt
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
H="$ROOT/tools/recomp/out/harness${VARIANT:+-$VARIANT}/harness.exe"
C="$ROOT/tools/recomp/out/cands.txt"
ERR=$(mktemp)
start=0
while :; do
    "$H" "$1" "$C" "${2:-4}" "${3:-1}" 0 "$start" 2> "$ERR"
    rc=$?
    [ $rc -eq 0 ] && break
    last=$(grep -a '^[0-9a-f]\{8\}$' "$ERR" | tail -1)
    [ -n "$last" ] || { echo "harness died before any test (rc $rc)"; break; }
    echo "CRASH $last (process died, rc $rc)"
    next=$(awk -v a="$last" '($1 "") > (a "") { print $1; exit }' "$C")   # string compare: 08451e30 is a number to awk
    [ -n "$next" ] || break
    start=$next
done
rm -f "$ERR"
