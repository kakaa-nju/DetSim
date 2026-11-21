#!/bin/bash
# Symbol-relocation regression.
#
# A tracee's globals must be resolved at the addresses it actually runs at.
# dwarf_init() records link-time vaddrs before the tracee exists, so the symbol
# service has to be rebased onto the tracee's load bias once the image is
# mapped; a miss shows up as "Snapshot read outside captured regions" or as a
# value read from the wrong address.
#
# Three inputs cover the paths that a single fixture cannot:
#   sym_pie           PIE + DWARF   -> resolution through global_vars
#   sym_pie_stripped  PIE, no debug -> resolution through the ELF symbol table,
#                                     which stayed empty for every PIE before
#                                     the main image was loaded at link-time
#                                     vaddrs and rebased afterwards
#   sym_exec          non-PIE       -> the layout must not be moved at all
#
# Each case asserts `*(long*)&g_marker == 4660`, which the tracer evaluates
# against a captured state, so a wrong bias fails the run instead of silently
# reading plausible-looking data.
#
#   cd tests/symbol_relocation && ./check.sh
# All three cases must print PASS.

set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

make -C "$root" NP=1 >/dev/null
make -C "$here" >/dev/null
tracer="$root/tracer"

rm -rf "$here"/memory "$here"/sstate "$here"/store_* "$here"/*.log
printf 'sw 0\nsi\nsi\nsi\np g_marker\nq\n' > "$here/cmds.txt"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run_case() {
    local name=$1 kind=$2
    local log="$here/$name.log"

    (cd "$here" && DETSIM_SNAPSHOT_BACKEND=pages timeout 120 \
        "$tracer" -n -b cmds.txt -c "$name.json") > "$log" 2>&1 ||
        { tail -5 "$log" >&2; fail "$name: tracer exited non-zero"; }

    # Every failure shape this bug has ever produced, plus a rejected assertion.
    for bad in "Snapshot read outside captured regions" \
               "Error evaluating expression" \
               "Cannot evaluate observation expression" \
               "Assertion \""; do
        grep -qF "$bad" "$log" && { tail -8 "$log" >&2; fail "$name: found '$bad'"; }
    done

    # The relocation must actually have run, with the bias the link type implies.
    grep -qE "ELF: Relocated \./$name symbols by 0x[0-9a-f]+" "$log" ||
        { grep -F "Relocated" "$log" >&2; fail "$name: no relocation recorded"; }
    if [ "$kind" = exec ]; then
        grep -qF "ELF: Relocated ./$name symbols by 0x0 " "$log" ||
            { grep -F "Relocated" "$log" >&2; fail "$name: non-PIE image was moved"; }
    else
        grep -qE "ELF: Relocated \./$name symbols by 0x([1-9a-f][0-9a-f]*) " "$log" ||
            { grep -F "Relocated" "$log" >&2; fail "$name: PIE image was not rebased"; }
    fi

    # `p g_marker` reads through the same symbol service the assertion used.
    grep -qF "Batch executed" "$log" || fail "$name: batch did not run"
    if [ "$kind" = stripped ]; then
        # No DWARF: the ELF fallback reports the address, so also prove that the
        # main image really was loaded through the link-time path.
        grep -qF "link-time vaddrs" "$log" ||
            fail "$name: ELF fallback did not load the main image"
        grep -qE "g_marker = \(void\*\) 0x[1-9a-f][0-9a-f]+" "$log" ||
            { grep -F "g_marker" "$log" >&2; fail "$name: global did not resolve"; }
    else
        grep -qF "g_marker = 4660" "$log" ||
            { grep -F "g_marker" "$log" >&2; fail "$name: global read the wrong value"; }
    fi

    echo "PASS: $name ($kind)"
}

run_case sym_pie pie
run_case sym_pie_stripped stripped
run_case sym_exec exec

rm -rf "$here"/memory "$here"/sstate "$here"/store_*
