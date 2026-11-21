#!/bin/bash
# One-command regression for the whole fixture set.
#
#   cd tests && ./run_all.sh              full run
#   cd tests && ./run_all.sh --no-build   skip fixture/example rebuilds (the
#                                         three tracers are always built: NP is
#                                         compile-time, so reusing one binary for
#                                         a different Nodes count aborts)
#   CASE_TIMEOUT=300 ./run_all.sh         raise the per-case timeout
#   DETSIM_SNAPSHOT_BACKEND=full ./run_all.sh   use the flat snapshot backend
#
# What it runs:
#   1. three tracer builds, because NP is compile-time and the fixtures split
#      across NP=1/2/3;
#   2. the dedicated suites (snapshot_repro, symbol_relocation, userstate), each
#      of which encodes expectations a generic batch cannot express -- value
#      pairs that must pass, one that must fail, cross-process hash equality;
#   3. every remaining tests/**, examples/raft and examples/redisraft config
#      with one generated batch: switch to each node, step it a few times. A
#      state whose actor is blocked is reported by the tracer and does not stop
#      the run.
#
# A case fails on a non-zero exit or on any property/evaluation diagnostic.
# Expected failures (a fixture whose oracle is meant to be violated) belong to a
# dedicated suite, not to the generic batch.
#
# The script leaves ./tracer built for NP=3, the value the examples use.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

build=1
[ "${1:-}" = "--no-build" ] && build=0

timeout_s=${CASE_TIMEOUT:-240}
backend=${DETSIM_SNAPSHOT_BACKEND:-pages}
failed=0
ran=0
elapsed_start=$(date +%s)

say() { printf '%s\n' "$*"; }
fail() { say "FAIL: $*" >&2; failed=$((failed + 1)); }

# ---------------------------------------------------------------- dedicated --
DEDICATED="snapshot_repro symbol_relocation userstate"

# ------------------------------------------------------------------- helpers --
json_nodes() {
    python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('Nodes',1))" "$1" 2>/dev/null || echo 1
}

json_has_choice() {
    python3 -c "import json,sys;print(1 if json.load(open(sys.argv[1])).get('ChoosePoint') else 0)" "$1" 2>/dev/null || echo 0
}

# Markers that mean the tracer reported something the fixture does not expect.
diagnostics() {
    grep -cE "DetSim state/execution error|Snapshot read outside captured regions|Cannot evaluate observation expression|CHECK FAILED|Assertion \"|Build doesn't support" "$1"
}

make_batch() {
    local nodes=$1 choice=$2 out=$3
    local step=si
    [ "$choice" = 1 ] && step="si 0"
    : > "$out"
    for ((n = 0; n < nodes; n++)); do
        echo "sw $n" >> "$out"
        for ((k = 0; k < 6; k++)); do echo "$step" >> "$out"; done
    done
    echo "q" >> "$out"
}

run_case() {
    local json=$1 tracer=$2
    local dir base nodes choice
    dir=$(dirname "$json")
    base=$(basename "$json" .json)
    nodes=$(json_nodes "$json")
    choice=$(json_has_choice "$json")
    ran=$((ran + 1))

    if [ ! -x "$tracer" ]; then
        fail "${json#$root/}: no NP=$nodes tracer"
        return
    fi

    local batch="$work/batch.txt" log="$work/case.log"
    make_batch "$nodes" "$choice" "$batch"

    rm -rf "$dir/memory" "$dir/sstate"
    (cd "$dir" && DETSIM_SNAPSHOT_BACKEND="$backend" timeout "$timeout_s" \
        "$tracer" -n -b "$batch" -c "$base.json") > "$log" 2>&1
    local rc=$?

    local bad
    bad=$(diagnostics "$log")
    if [ "$rc" = 124 ]; then
        say "FAIL  ${json#$root/}  (timeout ${timeout_s}s)"
        failed=$((failed + 1))
    elif [ "$rc" != 0 ]; then
        say "FAIL  ${json#$root/}  (exit $rc)"
        tail -3 "$log" | sed 's/^/        /' >&2
        failed=$((failed + 1))
    elif [ "$bad" != 0 ]; then
        say "FAIL  ${json#$root/}  ($bad diagnostic lines)"
        grep -E "DetSim state/execution error|Cannot evaluate|CHECK FAILED|Assertion \"" "$log" |
            head -2 | sed 's/^/        /' >&2
        failed=$((failed + 1))
    else
        say "ok    ${json#$root/}  (NP=$nodes)"
    fi
    rm -rf "$dir/memory" "$dir/sstate"
}

# --------------------------------------------------------------------- build --
say "== building tracer (NP=1/2/3)"
for np in 1 2 3; do
    if ! make -C "$root" NP="$np" >"$work/build_np$np.log" 2>&1; then
        say "FAIL: tracer build NP=$np" >&2
        tail -5 "$work/build_np$np.log" >&2
        exit 1
    fi
    cp "$root/tracer" "$work/tracer_np$np"
done

# Every directory that owns a generic case needs its fixture binaries.
generic_json=()
for f in "$root"/tests/*/*.json; do
    [ -f "$f" ] || continue
    case "$(basename "$(dirname "$f")")" in
        snapshot_repro|symbol_relocation|userstate) continue ;;
    esac
    case "$f" in *_violation.json) continue ;; esac
    generic_json+=("$f")
done
for f in "$root"/examples/raft/*.json "$root"/examples/redisraft/*.json; do
    [ -f "$f" ] && generic_json+=("$f")
done

if [ "$build" = 1 ]; then
    say "== building fixtures and examples"
    declare -A built=()
    for f in "${generic_json[@]}"; do
        d=$(dirname "$f")
        [ -n "${built[$d]:-}" ] && continue
        built[$d]=1
        # Examples are built below with the targets that exist for them; their
        # default goal also builds a dynamic variant some checkouts lack.
        case "$d" in "$root"/examples/*) continue ;; esac
        [ -f "$d/Makefile" ] || continue
        make -C "$d" >"$work/build_fixture.log" 2>&1 ||
            { say "FAIL: fixture build in ${d#$root/}" >&2; tail -5 "$work/build_fixture.log" >&2; exit 1; }
    done
    for d in "$root/examples/raft" "$root/examples/redisraft"; do
        make -C "$d" raft plugins >"$work/build_example.log" 2>&1 ||
            { say "FAIL: build in ${d#$root/}" >&2; tail -5 "$work/build_example.log" >&2; exit 1; }
    done
fi

# ------------------------------------------------------------ dedicated suites --
say "== dedicated suites"
for suite in $DEDICATED; do
    log="$work/suite_$suite.log"
    if "$here/$suite/check.sh" >"$log" 2>&1; then
        n_pass=$(grep -c "^PASS" "$log")
        say "ok    $suite ($n_pass checks)"
    else
        say "FAIL  $suite"
        grep "^FAIL" "$log" | head -3 | sed 's/^/        /' >&2
        failed=$((failed + 1))
    fi
done

# ------------------------------------------------------------------ generic --
say "== cases"
for f in "${generic_json[@]}"; do
    nodes=$(json_nodes "$f")
    run_case "$f" "$work/tracer_np$nodes"
done

# Leave the tree with the tracer the examples expect.
cp "$work/tracer_np3" "$root/tracer" 2>/dev/null || true

# ------------------------------------------------------------------ summary --
elapsed=$(( $(date +%s) - elapsed_start ))
say "== summary"
say "   dedicated suites: $(( $(echo "$DEDICATED" | wc -w) )) attempted"
say "   generic cases:    $ran"
say "   failures:         $failed"
say "   elapsed:          $((elapsed / 60))m$((elapsed % 60))s"
[ "$failed" = 0 ] && say "   RESULT: PASS" || say "   RESULT: FAIL"
[ "$failed" = 0 ]
