#!/bin/bash
# ab.sh DEPTH REPS -- what do UserCheck + UserState cost?
#
# Builds the tracer with the phase profiler, then runs the two configurations
# (.bench-nohook.json, .bench-hook.json) alternately to a *fixed DFS depth*, so
# every run explores exactly the same state space, and prints a median table.
#
# The fixed depth is what makes this comparable: with a wall-clock budget a
# faster build explores further and spends more time in the deep, expensive
# states, which hides its own advantage.
#
#   bash perf/raft/ab.sh 11 3
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
D=${DETSIM_ROOT:-$(cd "$HERE/../.." && pwd)}
RAFT=$D/examples/raft
DEPTH=${1:-11}
REPS=${2:-3}
CMDS=/tmp/detsim-ab-cmds.txt
LOGDIR=/tmp/detsim-ab
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log
printf 'dfs %s --quiet\n' "$DEPTH" > "$CMDS"

if [ "$REPS" -lt 3 ]; then
  echo "warning: single-run noise is about +/-3%, which is the size of the" >&2
  echo "         effect being measured; use REPS>=3 and compare medians." >&2
fi

# The profiler is a compile-time switch (see Makefile), so build it in.
make -C "$D" NP=3 PROFILER=1 -j"$(nproc)" >/tmp/detsim-ab-build.log 2>&1 || {
  echo "build failed, see /tmp/detsim-ab-build.log" >&2
  tail -5 /tmp/detsim-ab-build.log >&2
  exit 1
}

bash "$HERE/bench_configs.sh" >/dev/null || exit 1

cd "$RAFT" || exit 1
for rep in $(seq 1 "$REPS"); do
  for case in nohook hook; do
    pkill -9 -x tracer 2>/dev/null
    pkill -9 -x raft 2>/dev/null
    sleep 0.4
    rm -rf memory sstate
    DETSIM_TRANSITION_PROFILE=1 "$D/tracer" -n -c ".bench-$case.json" -b "$CMDS" \
        > "$LOGDIR/$case-$rep.log" 2>&1
  done
done
pkill -9 -x tracer 2>/dev/null
pkill -9 -x raft 2>/dev/null

python3 - "$DEPTH" "$LOGDIR" <<'PY'
import glob, os, re, statistics, sys
depth, logdir = sys.argv[1], sys.argv[2]
rows = {}
for case in ("nohook", "hook"):
    speeds, check, trans = [], [], []
    for path in sorted(glob.glob(os.path.join(logdir, case + "-*.log"))):
        text = open(path, errors="replace").read()
        m = re.search(r"speed = ([0-9.]+) states/s", text)
        if m:
            speeds.append(float(m.group(1)))
        m = re.search(r"TRANSITION_PROFILE \{([^}]*)\}", text)
        if m:
            fields = dict(re.findall(r'"(\w+)":(\d+)', m.group(1)))
            check.append(int(fields["check_us"]))
            trans.append(int(fields["transitions"]))
    if speeds:
        rows[case] = (statistics.median(speeds), min(speeds), max(speeds),
                      statistics.median(check) / statistics.median(trans),
                      statistics.median(trans))

if not rows:
    print("no results: is the tracer built with PROFILER=1?")
    raise SystemExit(1)

base = rows.get("nohook", (None,))[0]
print("depth %s, median of %d runs per case" % (depth, len(glob.glob(os.path.join(logdir, "hook-*.log")))))
print("%-8s %10s %10s %10s %14s %8s %12s" %
      ("case", "states/s", "min", "max", "check us/tr", "trans", "vs nohook"))
for case in ("nohook", "hook"):
    if case not in rows:
        continue
    med, lo, hi, check_us, ntrans = rows[case]
    delta = "" if not base else "%+.1f%%" % (100.0 * (med - base) / base)
    print("%-8s %10.1f %10.1f %10.1f %14.1f %8d %12s" %
          (case, med, lo, hi, check_us, ntrans, delta))
PY
