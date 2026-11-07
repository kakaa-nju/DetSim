#!/bin/bash
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" NP=1 >/dev/null
make -C "$here" >/dev/null
tracer="$root/tracer"

run_case() {
    local program=$1 marker=$2
    local out cmds
    out=$(mktemp)
    cmds=$(mktemp)
    trap 'rm -f "$out" "$cmds"' RETURN
    {
        echo 'sw 0'
        echo 'thread 0'
        for _ in $(seq 1 200); do echo 'si'; done
        echo 'pending'
        echo 'thread 1'
        for _ in $(seq 1 12); do echo 'si'; done
        echo 'si timeout'
        for _ in $(seq 1 4); do echo 'si'; done
        echo 'pending'
        echo 'q'
    } > "$cmds"
    (cd "$here" && timeout 120 "$tracer" -n -b "$cmds" -c "$program.json") > "$out" 2>&1 || {
        echo "FAIL: tracer exited for $program" >&2
        cat "$out" >&2
        return 1
    }
    if ! grep -q "$marker" "$out"; then
        echo "FAIL: suspended $program waiter was not woken by the peer write" >&2
        grep -E 'PROC |Runnable actors|pending:|error' "$out" >&2 || true
        return 1
    fi
    echo "PASS: $program"
}

run_case mt_async 'timer:ready pselect6 result=1'
run_case mt_poll  'timer:ready poll result=1'
