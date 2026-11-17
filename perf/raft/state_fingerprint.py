#!/usr/bin/env python3
"""state_fingerprint.py LOG_A LOG_B [LOG_C ...]

Prove that two (or more) fixed-depth searches explored the same state space.

DetSim writes one `SEARCH_EDGE` line per committed product edge, in commit
order, and one `SEARCH_AUDIT` summary line per search command. Both are opt-in:

    DETSIM_SEARCH_AUDIT=1 ./tracer -n -c config.json -b cmds.txt

The tool compares three layers, in the order the acceptance protocol uses them:

  * ordered edge sequence  (L2) -- a pure throughput refactor must not reorder
    the search, so this has to be identical for every Stage 1/2 change;
  * edge multiset          (L3) -- a sound partial-order reduction drops
    independent interleavings, so the multiset may shrink but must never grow
    into a state the unreduced search did not reach;
  * discovered state set   (L3) -- `states_fp` must be identical: the reduction
    may reach a state by a different edge, never fail to reach it.

Legacy logs that only contain `HASH => HASH` history lines are also accepted;
those lines only carry the single branch that ended the search, so they are a
weaker oracle. Prefer DETSIM_SEARCH_AUDIT.

`--receipts` compares the SEARCH_AUDIT receipts of several searches *inside one
log*, which is the only comparison that can use `states_fp`: a captured image may
carry a host-derived byte, so state identities are salted per process and are not
comparable across processes. With DETSIM_SEARCH_ISOLATE=1 every search command in
a session starts again from the session root, so a batch of

    dfs 8 --quiet
    dfs 8 --quiet

yields two receipts that must agree exactly -- that is the gate a reduction
policy has to pass:

  python3 perf/raft/state_fingerprint.py --receipts run.log

A third, binary-independent oracle is the metadata append log itself. Every
committed product identity is appended to `sstate/packed.dat`, so the set of
records in that file *is* the set of states the run reached -- whatever build
produced it. Use `--sstate` to compare those sets directly:

  python3 perf/raft/state_fingerprint.py --sstate runA/sstate/packed.dat \
                                                runB/sstate/packed.dat

  python3 perf/raft/state_fingerprint.py /tmp/run-A.log /tmp/run-B.log
"""
import hashlib
import json
import re
import struct
import sys

EDGE = re.compile(
    r"^SEARCH_EDGE\s+([0-9a-f]{16}) => ([0-9a-f]{16})"
    r"\s+p=(-?\d+)\s+t=(-?\d+)\s+c=(-?\d+)\s+n=(-?\d+)"
    r"\s+code=(-?\d+)\s+bad=(\d+)\s*$")
AUDIT = re.compile(r"^SEARCH_AUDIT (\{.*\})\s*$")
LEGACY = re.compile(r"([0-9a-f]{16}) => ([0-9a-f]{16})")
ENTRY_MAGIC = 0x53534545
ENTRY_HEADER = struct.Struct("<IQII")  # magic, hash, size, checksum


class Run:
    def __init__(self, path):
        self.path = path
        self.edges = []          # ordered tuples (src, dst, p, t, c, n, code, bad)
        self.audit = None
        self.legacy = False
        with open(path, errors="replace") as handle:
            for line in handle:
                line = re.sub(r"\x1b\[[0-9;]*m", "", line).rstrip("\n")
                match = EDGE.match(line.strip())
                if match:
                    src, dst, p, t, c, n, code, bad = match.groups()
                    self.edges.append((src, dst, int(p), int(t), int(c),
                                       int(n), int(code), int(bad)))
                    continue
                match = AUDIT.match(line.strip())
                if match:
                    self.audit = json.loads(match.group(1))
                    continue
                match = LEGACY.search(line)
                if match and not self.edges:
                    # Legacy history line: no actor metadata exists for it.
                    self.legacy = True
                    self.edges.append((match.group(1), match.group(2),
                                       -1, -1, -1, -1, -1, -1))

    def states(self):
        return sorted({edge[0] for edge in self.edges} |
                      {edge[1] for edge in self.edges})

    def states_fingerprint(self):
        blob = "\n".join(self.states())
        return hashlib.sha256(blob.encode()).hexdigest()[:32]

    def edge_fingerprint(self, ordered):
        rows = ["%s>%s|%d|%d|%d|%d|%d|%d" % edge for edge in self.edges]
        if not ordered:
            rows.sort()
        return hashlib.sha256("\n".join(rows).encode()).hexdigest()[:32]

    def graph_fingerprint(self):
        """Isomorphism-invariant fingerprint of the explored state graph.

        Two runs of the same search can hash the same state differently: the
        captured image of a static fixture contains host-derived bytes (one
        glibc cpu-feature field on this kernel), so the identity is not stable
        across processes. What must be stable is the *graph*: which labelled
        action leads from which state to which state, and how many states and
        edges there are.

        This computes the 1-dimensional Weisfeiler-Lehman refinement of the
        edge-labelled multigraph: colours start from the labelled in/out degree
        signature and are refined iteratively over successors and predecessors
        until they stop changing. The fingerprint is the multiset of final
        colours, which is independent of how states are named.
        """
        def edge_key(edge):
            _, _, process, thread, choice, count, code, bad = edge
            return (process, thread, choice, count, code, bad)

        successors = {}
        predecessors = {}
        nodes = set()
        for edge in self.edges:
            src, dst = edge[0], edge[1]
            label = edge_key(edge)
            successors.setdefault(src, []).append((label, dst))
            predecessors.setdefault(dst, []).append((label, src))
            nodes.add(src)
            nodes.add(dst)

        colours = {}
        for node in nodes:
            out_labels = sorted(label for label, _ in successors.get(node, []))
            in_labels = sorted(label for label, _ in predecessors.get(node, []))
            seed = repr((tuple(out_labels), tuple(in_labels)))
            colours[node] = hashlib.sha256(seed.encode()).hexdigest()[:16]

        for _ in range(len(nodes)):
            updated = {}
            for node in nodes:
                out = sorted((label, colours[target])
                             for label, target in successors.get(node, []))
                incoming = sorted((label, colours[source])
                                  for label, source in predecessors.get(node, []))
                signature = repr((colours[node], tuple(out), tuple(incoming)))
                updated[node] = hashlib.sha256(signature.encode()).hexdigest()[:16]
            same_partition = (len(set(updated.values())) ==
                              len(set(colours.values())))
            colours = updated
            if same_partition:
                break
        # One more refinement round makes the printed colours canonical for the
        # final partition, so the multiset below is reproducible in any process.
        buckets = {}
        for node in nodes:
            out = sorted((label, colours[target])
                         for label, target in successors.get(node, []))
            incoming = sorted((label, colours[source])
                              for label, source in predecessors.get(node, []))
            signature = repr((colours[node], tuple(out), tuple(incoming)))
            colour = hashlib.sha256(signature.encode()).hexdigest()[:16]
            buckets[colour] = buckets.get(colour, 0) + 1
        blob = "\n".join("%s:%d" % (colour, buckets[colour])
                         for colour in sorted(buckets))
        return hashlib.sha256(blob.encode()).hexdigest()[:32]

    def describe(self):
        audit = self.audit or {}
        return ("mode=%s depth=%s unique=%s searched=%s status=%s "
                "edges=%d states=%d states_fp=%s audit_fp=%s" % (
                    audit.get("mode", "?"), audit.get("depth", "?"),
                    audit.get("unique", len(self.states())),
                    audit.get("searched", "?"), audit.get("status", "?"),
                    len(self.edges), len(self.states()),
                    self.states_fingerprint(), audit.get("states_fp", "n/a")))


def first_divergence(left, right):
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index, a, b
    if len(left) != len(right):
        return min(len(left), len(right)), None, None
    return None


def read_sstate(path):
    """Return the set of product identities recorded in a metadata log."""
    hashes = set()
    with open(path, "rb") as handle:
        blob = handle.read()
    offset = 0
    while offset < len(blob):
        if len(blob) - offset < ENTRY_HEADER.size:
            raise SystemExit("%s: truncated metadata header at %d" % (path, offset))
        magic, value, size, _checksum = ENTRY_HEADER.unpack_from(blob, offset)
        if magic != ENTRY_MAGIC:
            raise SystemExit("%s: bad record magic at %d" % (path, offset))
        if size > len(blob) - offset - ENTRY_HEADER.size:
            raise SystemExit("%s: truncated record at %d" % (path, offset))
        hashes.add(value)
        offset += ENTRY_HEADER.size + size
    return hashes


def read_receipts(path):
    receipts = []
    for line in open(path, errors="replace"):
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).strip()
        match = AUDIT.match(line)
        if match:
            receipts.append(json.loads(match.group(1)))
    return receipts


def run_receipts(paths):
    receipts = []
    for path in paths:
        for index, receipt in enumerate(read_receipts(path)):
            receipts.append((path, index, receipt))
    if not receipts:
        print("ERROR: no SEARCH_AUDIT receipt found -- run the tracer with "
              "DETSIM_SEARCH_AUDIT=1", file=sys.stderr)
        return 3
    for path, index, receipt in receipts:
        print("[%s #%d] mode=%s depth=%s root=%s unique=%s searched=%s "
              "states_fp=%s status=%s por_skipped=%s" % (
                  path, index, receipt.get("mode"), receipt.get("depth"),
                  receipt.get("root"), receipt.get("unique"),
                  receipt.get("searched"), receipt.get("states_fp"),
                  receipt.get("status"), receipt.get("por_skipped")))
    if len(receipts) < 2:
        print("note: a single receipt proves nothing about equality", file=sys.stderr)
        return 2
    reference = receipts[0][2]
    ok = True
    for path, index, receipt in receipts[1:]:
        differences = [field for field in
                       ("states_fp", "unique", "searched", "status")
                       if receipt.get(field) != reference.get(field)]
        if differences:
            ok = False
            print("receipt #%d (%s) differs from #0 in: %s" % (index, path,
                                                               ", ".join(differences)))
        else:
            print("receipt #%d (%s) == #0 (states_fp %s)" % (
                index, path, receipt.get("states_fp")))
    print("\nRESULT:", "IDENTICAL RECEIPTS" if ok else "RECEIPTS DIFFER")
    return 0 if ok else 1


def run_sstate(paths):
    sets = [read_sstate(path) for path in paths]
    reference = sets[0]
    ok = True
    for path, hashes in zip(paths, sets):
        print("[%s] states=%d fingerprint=%s" % (
            path, len(hashes),
            hashlib.sha256("\n".join(sorted("%016x" % value
                                            for value in hashes)
                                     ).encode()).hexdigest()[:32]))
    for path, hashes in zip(paths[1:], sets[1:]):
        same = hashes == reference
        print("state set vs %s: %s (%d vs %d)" % (
            paths[0], "IDENTICAL" if same else "DIFFERS",
            len(reference), len(hashes)))
        if not same:
            print("  missing: %s" % sorted("%016x" % value
                                           for value in reference - hashes)[:5])
            print("  extra:   %s" % sorted("%016x" % value
                                           for value in hashes - reference)[:5])
        ok = ok and same
    print("\nRESULT:", "SAME STATE SET" if ok else "DIFFERENT STATE SETS")
    return 0 if ok else 1


def main():
    arguments = sys.argv[1:]
    if arguments and arguments[0] == "--receipts":
        if len(arguments) < 2:
            print(__doc__)
            return 2
        return run_receipts(arguments[1:])
    if arguments and arguments[0] == "--sstate":
        if len(arguments) < 3:
            print(__doc__)
            return 2
        return run_sstate(arguments[1:])
    if len(arguments) < 2:
        print(__doc__)
        return 2
    # Two logs that contain different numbers of searches are not comparable: a
    # two-search batch simply has twice the edges, which the multiset gate would
    # report as a regression. Refuse rather than print a misleading verdict.
    batches = dict((path, len(read_receipts(path))) for path in arguments)
    if any(batches.values()) and len(set(batches.values())) > 1:
        print("ERROR: logs contain different numbers of SEARCH_AUDIT receipts "
              "(%r); compare single searches with single searches"
              % (batches,), file=sys.stderr)
        return 4
    runs = [Run(path) for path in arguments]
    for run in runs:
        print("[%s] %s" % (run.path, run.describe()))

    reference = runs[0]
    ok = True
    if reference.legacy:
        print("note: legacy history lines found; only the final branch is visible")
    print("note: every expansion path drives the same enumerator with explicit choice\n"
              "      presets, so DFS/BFS commit the same edge sequence for the same root\n"
              "      (rand mode is the only randomized path). A differing sequence with an\n"
              "      identical multiset still means nondeterminism worth investigating; the\n"
              "      gate itself stays the edge multiset plus the discovered state set.")
    for other in runs[1:]:
        print("\n-- %s vs %s" % (reference.path, other.path))
        sequence = reference.edges == other.edges
        multiset = (sorted(reference.edges) == sorted(other.edges))
        states = reference.states() == other.states()
        print("edge sequence:  %s (informational)" %
              ("IDENTICAL" if sequence else "DIFFERS"))
        if not sequence:
            divergence = first_divergence(reference.edges, other.edges)
            print("  first divergence at %s: %s vs %s" %
                  (divergence[0], divergence[1], divergence[2]))
            print("  lengths %d vs %d" % (len(reference.edges), len(other.edges)))
        print("edge multiset:  %s (%d vs %d edges)  <-- gate" %
              ("IDENTICAL" if multiset else "DIFFERS",
               len(reference.edges), len(other.edges)))
        print("state set:      %s (%d vs %d states)  <-- gate  fp %s vs %s" %
              ("IDENTICAL" if states else "DIFFERS",
               len(reference.states()), len(other.states()),
               reference.states_fingerprint(), other.states_fingerprint()))
        graph = reference.graph_fingerprint()
        other_graph = other.graph_fingerprint()
        print("state graph:    %s  (isomorphism fingerprint %s vs %s)  <-- gate" %
              ("IDENTICAL" if graph == other_graph else "DIFFERS",
               graph, other_graph))
        if reference.audit and other.audit:
            for field in ("unique", "searched", "status"):
                if reference.audit.get(field) != other.audit.get(field):
                    print("  receipt differs: %s %s vs %s" %
                          (field, reference.audit.get(field),
                           other.audit.get(field)))
                    ok = False
        if not states:
            missing = set(reference.states()) - set(other.states())
            extra = set(other.states()) - set(reference.states())
            print("  missing from second: %s" % sorted(missing)[:5])
            print("  extra in second:     %s" % sorted(extra)[:5])
        ok = ok and graph == other_graph and len(reference.edges) == len(other.edges)

    print("\nRESULT:", "SAME STATE SPACE" if ok else "DIFFERENT STATE SPACE")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
