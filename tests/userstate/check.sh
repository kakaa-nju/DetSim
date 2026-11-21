#!/bin/bash
# UserState control/violation pair.
#
# One guest program, one checker plugin, two expectations:
#   userstate.json            the counter only increases  -> the checker must
#                             stay silent and the run must complete
#   userstate_violation.json  the counter is driven backwards -> the checker must
#                             report the regression it exists to catch
#
# Running only the first direction would not prove the oracle is live, and
# running only the second would not prove it is silent when it should be.
#
#   cd tests/userstate && ./check.sh
# Both cases must print PASS.

set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" NP=3 >/dev/null
make -C "$here" >/dev/null
tracer="$root/tracer"

rm -rf "$here"/memory "$here"/sstate "$here"/*.log
# One step per node: enough to walk the counter through its whole program.
printf 'sw 0\nsi\nsi\nsi\nsi\nsi\nsi\nsw 1\nsi\nsi\nsi\nsi\nsi\nsi\nsw 2\nsi\nsi\nsi\nsi\nsi\nsi\nq\n' \
    > "$here/cmds.txt"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run() {
    local cfg=$1 log=$2
    (cd "$here" && DETSIM_SNAPSHOT_BACKEND=pages timeout 180 \
        "$tracer" -n -b cmds.txt -c "$cfg") > "$log" 2>&1 || true
}

# Direction 1: a monotone counter must not be reported.
run userstate.json "$here/good.log"
grep -qF "[CHECK FAILED]" "$here/good.log" &&
    { tail -5 "$here/good.log" >&2; fail "monotone run raised a check failure"; }
grep -qF "DetSim state/execution error" "$here/good.log" &&
    { tail -5 "$here/good.log" >&2; fail "monotone run did not complete"; }
grep -qF "Batch executed" "$here/good.log" || fail "monotone run did not run the batch"
echo "PASS: monotone counter, checker silent"

# Direction 2: the same program breaking the invariant must be reported.
run userstate_violation.json "$here/violation.log"
grep -qF "user_counter decreased for node 0: 3 -> 2" "$here/violation.log" ||
    { tail -5 "$here/violation.log" >&2; fail "oracle breach was not reported"; }
echo "PASS: regressing counter, checker reports 3 -> 2"

rm -rf "$here"/memory "$here"/sstate
