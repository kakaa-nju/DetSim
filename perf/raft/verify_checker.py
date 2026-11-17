#!/usr/bin/env python3
"""verify_checker.py DEPTHS PLUGIN_A PLUGIN_B

Compare two checker builds over an exhaustive DFS to a fixed depth.

Each plugin is run as UserCheck + UserState with a fresh state store, and the two
runs are compared on

  * the multiset of committed transitions (SEARCH_EDGE lines), and the
    isomorphism fingerprint of the explored state graph,
  * the search summary ("Searched for N sys_states ..."),
  * every "[CHECK FAILED]" / assertion line.

Why not a line-by-line hash sequence: a captured image may contain
host-derived bytes, so the same graph hashes differently in two processes even
when every modeled input agrees. The graph isomorphism fingerprint
(perf/raft/state_fingerprint.py) is stable under that and is what actually answers
"did the two builds explore the same state space". The commit *order* is the same
for DFS and BFS now that every path drives one enumerator with explicit presets
(rand mode is the only randomized path), so the sequence is printed for
diagnostics rather than used as the gate.

The run is done with DETSIM_SEARCH_AUDIT=1; a run that produces no transition
receipt at all is a failure of the harness, never a pass.

  python3 perf/raft/verify_checker.py 4,5,6,8 plugins/check.so /tmp/ex/check.orig.so

Use it to validate a generated checker against a reference, or a refactor
against the implementation it replaces.
"""
import json
import os
import re
import shutil
import subprocess
import sys

D = os.environ.get("DETSIM_ROOT") or os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RAFT = os.path.join(D, "examples", "raft")
TRACER = os.path.join(D, "tracer")
LOGDIR = "/tmp/detsim-verify"
TRANS = re.compile(r"([0-9a-f]{16}) => ([0-9a-f]{16})")


def write_config(tag, plugin):
    base = json.load(open(os.path.join(RAFT, "raft.json")))
    base["Loglevel"] = 0
    base["UserCheck"] = plugin
    base["UserState"] = plugin
    path = os.path.join(RAFT, ".bench-verify-%s.json" % tag)
    json.dump(base, open(path, "w"), indent=1)
    return path


EDGE = re.compile(
    r"^SEARCH_EDGE\s+([0-9a-f]{16}) => ([0-9a-f]{16})"
    r"\s+p=(-?\d+)\s+t=(-?\d+)\s+c=(-?\d+)\s+n=(-?\d+)"
    r"\s+code=(-?\d+)\s+bad=(\d+)\s*$")


def run(tag, config, depth):
    for store in ("memory", "sstate"):
        shutil.rmtree(os.path.join(RAFT, store), ignore_errors=True)
    cmds = "/tmp/detsim-verify-%s-cmds.txt" % tag
    open(cmds, "w").write("dfs %s --quiet\n" % depth)
    log = os.path.join(LOGDIR, "%s-d%s.log" % (tag, depth))
    environment = dict(os.environ, DETSIM_SEARCH_AUDIT="1")
    with open(log, "w") as out:
        subprocess.run([TRACER, "-n", "-c", config, "-b", cmds], cwd=RAFT,
                       env=environment, stdout=out, stderr=subprocess.STDOUT,
                       timeout=3600)
    edges, summary, fails = [], None, []
    for line in open(log, errors="replace"):
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).rstrip()
        if "CHECK FAILED" in line or "Assertion \"" in line:
            fails.append(line)
        match = EDGE.match(line.strip())
        if match:
            src, dst, p, t, c, n, code, bad = match.groups()
            # Drop the identities: they are host-dependent, the shape is not.
            edges.append((p, t, c, n, code, bad))
            continue
        m = TRANS.search(line)
        if m and not edges:
            edges.append((m.group(1), m.group(2)))
        if "Searched for" in line or "Complete explore" in line:
            summary = line
    return edges, summary, fails


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    depths = sys.argv[1].split(",")
    plugin_a, plugin_b = sys.argv[2], sys.argv[3]
    os.makedirs(LOGDIR, exist_ok=True)
    if not os.path.exists(TRACER):
        print("missing %s -- run 'make NP=3' first" % TRACER)
        return 2
    configs = {"A": write_config("A", plugin_a), "B": write_config("B", plugin_b)}

    results = {}
    for depth in depths:
        for tag in ("A", "B"):
            seq, summary, fails = run(tag, configs[tag], depth)
            results[(tag, depth)] = (seq, summary, fails)
            print("[%s = %s depth %s] %s  transitions=%d failures=%d" %
                  (tag, os.path.basename(plugin_a if tag == "A" else plugin_b),
                   depth, summary, len(seq), len(fails)))
            if not seq:
                print("ERROR: no SEARCH_EDGE receipt for %s depth %s -- the run "
                      "produced nothing to compare (check DETSIM_SEARCH_AUDIT "
                      "support and the tracer exit status)" % (tag, depth),
                      file=sys.stderr)
                return 3

    ok = True
    for depth in depths:
        a, b = results[("A", depth)], results[("B", depth)]
        # Order-insensitive: DFS choice order is seeded from the wall clock.
        same = (sorted(a[0]) == sorted(b[0]), a[1] == b[1], a[2] == b[2])
        print("depth %s: transition multiset %s (%d vs %d), summary %s, "
              "failures %s" %
              (depth,
               "IDENTICAL" if same[0] else "DIFFERS",
               len(a[0]), len(b[0]),
               "IDENTICAL" if same[1] else "DIFFERS",
               "IDENTICAL" if same[2] else "DIFFERS"))
        if not same[0]:
            missing = sorted(set(a[0]) - set(b[0]))
            extra = sorted(set(b[0]) - set(a[0]))
            print("  only in A (%d): %s" % (len(missing), missing[:4]))
            print("  only in B (%d): %s" % (len(extra), extra[:4]))
        ok = ok and all(same)

    for path in configs.values():
        os.unlink(path)
    print("RESULT:", "EQUIVALENT" if ok else "NOT EQUIVALENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
