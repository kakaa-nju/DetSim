#!/usr/bin/env python3
"""verify_checker.py DEPTHS PLUGIN_A PLUGIN_B

Prove that two checker builds are semantically equivalent.

Each plugin is run as UserCheck + UserState over an exhaustive DFS to a fixed
depth, with a fresh state store every time, and the two runs are compared on

  * every explored transition line ("HASH => HASH ..."), in order,
  * the search summary ("Searched for N sys_states ..."),
  * every "[CHECK FAILED]" / assertion line.

A fixed depth is required: DetSim's DFS is deterministic at a fixed depth, so
the state space (and therefore every hash it prints) is reproducible, while a
wall-clock budget is not.

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


def run(tag, config, depth):
    for store in ("memory", "sstate"):
        shutil.rmtree(os.path.join(RAFT, store), ignore_errors=True)
    cmds = "/tmp/detsim-verify-%s-cmds.txt" % tag
    open(cmds, "w").write("dfs %s --quiet\n" % depth)
    log = os.path.join(LOGDIR, "%s-d%s.log" % (tag, depth))
    with open(log, "w") as out:
        subprocess.run([TRACER, "-n", "-c", config, "-b", cmds], cwd=RAFT,
                       stdout=out, stderr=subprocess.STDOUT, timeout=3600)
    seq, summary, fails = [], None, []
    for line in open(log, errors="replace"):
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).rstrip()
        if "CHECK FAILED" in line or "Assertion \"" in line:
            fails.append(line)
        m = TRANS.search(line)
        if m:
            seq.append(m.group(0))
        if "Searched for" in line or "Complete explore" in line:
            summary = line
    return seq, summary, fails


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

    ok = True
    for depth in depths:
        a, b = results[("A", depth)], results[("B", depth)]
        same = (a[0] == b[0], a[1] == b[1], a[2] == b[2])
        print("depth %s: sequence %s, summary %s, failures %s" %
              (depth,
               "IDENTICAL" if same[0] else "DIFFERS",
               "IDENTICAL" if same[1] else "DIFFERS",
               "IDENTICAL" if same[2] else "DIFFERS"))
        if not same[0]:
            for i, (x, y) in enumerate(zip(a[0], b[0])):
                if x != y:
                    print("  first divergence at %d: %r vs %r" % (i, x, y))
                    break
            print("  lengths %d vs %d" % (len(a[0]), len(b[0])))
        ok = ok and all(same)

    for path in configs.values():
        os.unlink(path)
    print("RESULT:", "EQUIVALENT" if ok else "NOT EQUIVALENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
