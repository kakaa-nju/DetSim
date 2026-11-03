#!/bin/bash
# measure.sh TAG CONFIG SECONDS -- one exploration, then the phase breakdown.
#
# Runs a single configuration with the phase profiler on, interrupts it after
# SECONDS, and prints states/s plus the TRANSITION_PROFILE / SEARCH_WORK lines.
#
#   bash perf/raft/measure.sh myrun .bench-hook.json 30
#
# CONFIG is resolved against examples/raft/.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
D=${DETSIM_ROOT:-$(cd "$HERE/../.." && pwd)}
RAFT=$D/examples/raft
TAG=${1:?usage: measure.sh TAG CONFIG SECONDS}
CFG=$2
SECS=${3:-30}
OUT=/tmp/detsim-$TAG.log
CMDS=/tmp/detsim-cmds.txt
printf 'bfs --quiet\n' > "$CMDS"

cd "$RAFT" || exit 1
rm -rf memory sstate
mk=""
case "$CFG" in /*) CFG_ABS=$CFG ;; *) CFG_ABS=$RAFT/$CFG ;; esac

if ! grep -q '"Loglevel": 0' "$CFG_ABS" 2>/dev/null; then
  mk="(note: Loglevel is not 0; per-state logging will dominate the measurement)"
fi

pkill -9 -x tracer 2>/dev/null
sleep 0.3
DETSIM_TRANSITION_PROFILE=1 "$D/tracer" -n -c "$CFG_ABS" -b "$CMDS" > "$OUT" 2>&1 &
P=$!
sleep "$SECS"
kill -INT $P 2>/dev/null
for _ in $(seq 1 600); do kill -0 $P 2>/dev/null || break; sleep 0.1; done
kill -9 $P 2>/dev/null
pkill -9 -x raft 2>/dev/null

echo "===== $TAG ($OUT) $mk"
grep -a "speed =" "$OUT" | tail -1
grep -a "TRANSITION_PROFILE" "$OUT" | tail -1
grep -a "SEARCH_WORK" "$OUT" | tail -1
