#!/bin/bash
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" NP=1 >/dev/null
make -C "$here" >/dev/null
out=$(mktemp)
trap 'rm -f "$out"' EXIT
(cd "$here" && timeout 180 "$root/tracer" -n -a -c select_poll_semantics.json) > "$out" 2>&1 || {
    echo "FAIL: tracer exited for select_poll_semantics" >&2
    tail -40 "$out" >&2
    exit 1
}
if ! grep -q "select_poll_semantics" "$out" && ! grep -q "PASS" "$out"; then
    echo "FAIL: no PASS lines found" >&2
    tail -40 "$out" >&2
    exit 1
fi
if grep -q "FAIL" "$out"; then
    echo "FAIL: a semantic check failed" >&2
    grep -E "PASS|FAIL" "$out" >&2
    exit 1
fi
grep -E "PASS" "$out"
