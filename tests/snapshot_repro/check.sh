#!/bin/bash
# Cross-process snapshot reproducibility regression.
#
# Each fixture runs twice in two independent tracer processes from the same
# working directory (identical environment) with separated snapshot stores.
# Every snapshot the tracer saves logs a content hash ("StateStore saved
# hash ...") and every system state logs a metadata hash ("Create sstate
# ..."); both sequences must be byte-identical across the two runs, which is
# exactly the cross-process reproducibility property. A diverging host
# nondeterminism source (CPUID APIC id, RDTSC startup timers, fstat
# timestamps, inherited stack garbage) breaks this test.
#
#   cd tests/snapshot_repro && ./check.sh
# Both cases must print PASS.

set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" NP=1 >/dev/null
make -C "$here" >/dev/null
tracer="$root/tracer"

mkdir -p "$here/store1" "$here/store2"
for name in dyn stat; do
    for run in 1 2; do
        cat > "$here/$name$run.json" <<EOF
{
  "Loglevel": 0,
  "Nodes": 1,
  "Tracee": [["./${name}_hello", "0"]],
  "Addr": ["192.168.0.1"],
  "RuntimeProfile": {
    "fixed_layout": true,
    "startup_syscall": $([ "$name" = stat ] && echo 10 || echo 11)
  },
  "StateStore": {"packed_storage_path": "store$run"}
}
EOF
    done
done
printf 'sw 0\nsi\nsi\nsi\nsi\nsi\nsi\nq\n' > "$here/cmds.txt"

run_case() {
    local name=$1
    (cd "$here" && timeout 120 "$tracer" -n -b cmds.txt -c "$name"1.json) > "$here/run1.log" 2>&1 || {
        echo "FAIL: tracer exited for $name run 1" >&2; tail -3 "$here/run1.log" >&2; return 1; }
    (cd "$here" && timeout 120 "$tracer" -n -b cmds.txt -c "$name"2.json) > "$here/run2.log" 2>&1 || {
        echo "FAIL: tracer exited for $name run 2" >&2; tail -3 "$here/run2.log" >&2; return 1; }

    grep -aoE "StateStore saved hash [0-9a-f]{16}" "$here/run1.log" | awk '{print $4}' > "$here/seq1"
    grep -aoE "StateStore saved hash [0-9a-f]{16}" "$here/run2.log" | awk '{print $4}' > "$here/seq2"
    local n
    n=$(wc -l < "$here/seq1")
    if [ "$n" -lt 1 ]; then
        echo "FAIL: $name captured no snapshots" >&2; return 1
    fi
    if ! cmp -s "$here/seq1" "$here/seq2"; then
        echo "FAIL: $name snapshot hashes differ across processes ($n snapshots)" >&2
        diff "$here/seq1" "$here/seq2" | head -6 >&2
        return 1
    fi
    grep -aoE "Create sstate [0-9a-f]{16}" "$here/run1.log" | awk '{print $3}' > "$here/sstate1"
    grep -aoE "Create sstate [0-9a-f]{16}" "$here/run2.log" | awk '{print $3}' > "$here/sstate2"
    if ! cmp -s "$here/sstate1" "$here/sstate2"; then
        echo "FAIL: $name system-state hashes differ across processes" >&2
        diff "$here/sstate1" "$here/sstate2" | head -6 >&2
        return 1
    fi
    echo "PASS: $name ($n snapshot hashes identical across processes)"
}

run_case dyn
run_case stat
