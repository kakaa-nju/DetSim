#!/usr/bin/env python3
"""Same-checkpoint native DFS/parallel benchmark for the three-node Raft library.

Python prepares the prefix and checks native reports; it never explores states.
Serial `tree` stores first incoming edges only, so its projection is subset
coverage evidence, not a complete transition graph or an exhaustive oracle.
"""
import argparse
from collections import Counter, defaultdict, deque
import json
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.fixture_profile import startup_boundary
from redisraft import run as raft_case

ROOT = Path(__file__).resolve().parents[2]


def checked(session, command=None):
    reply = session.send(command)
    common.require(not any(marker in reply for marker in (
        "DetSim state/execution error:", "Reach illegal state", "Stopped for illegal state",
        "Fatal signal", "stopped for signal", "Load rejected:", "No state found")), reply[-4000:])
    return reply


def tree(session):
    return {(source, target, int(process), int(thread), int(choice),
             "timeout" if int(choice) == -2 else "normal")
            for source, target, process, choice, thread in re.findall(
                r"(?m)^EDGE ([0-9a-f]{16}) ([0-9a-f]{16}) (\d+) (-?\d+) (\d+)$", checked(session, "tree"))}


def tree_projection(edges, seed, depth):
    """Reachable canonical first-parent witnesses, including any recorded loops."""
    outgoing = defaultdict(list)
    for edge in edges:
        outgoing[edge[0]].append(edge)
    nodes, selected = {seed: 0}, set()
    queue = deque([seed])
    while queue:
        source = queue.popleft()
        if nodes[source] >= depth:
            continue
        for edge in outgoing[source]:
            selected.add(edge)
            if edge[1] not in nodes:
                nodes[edge[1]] = nodes[source] + 1
                queue.append(edge[1])
    return set(nodes), selected


def observe(session):
    nodes = []
    for node in range(3):
        checked(session, f"sw {node}")
        values = {"node": node}
        for field in ("state", "current_term", "commit_idx", "last_applied_idx", "snapshot_last_idx"):
            reply = checked(session, "p ((raft_server*)raft)->" + field)
            match = re.search(r" = (-?\d+)\r?\n", reply)
            common.require(match, "missing saved Raft field " + field + ": " + reply)
            values[field] = int(match[1])
        nodes.append(values)
    return nodes


def parallel_graph(receipt):
    directory = Path(receipt["artifacts"])
    journal = directory / "journal.jsonl"
    rows = [json.loads(line) for line in journal.read_text().splitlines()]
    edges = {(f'{row["source"]:016x}', f'{row["target"]:016x}', row["process"], row["thread"],
              row["choice"], row["action"], row["choice_count"], row["result_code"], row["violation"])
             for row in rows if row.get("event") == "edge"}
    node_details = {f'{row["state"]:016x}': (row["depth"], row["phase"])
                    for row in rows if row.get("event") == "node"}
    nodes = set(node_details)
    common.require(len(edges) == receipt["edges"] and len(nodes) == receipt["nodes"],
                   "native journal/report graph counts differ")
    return nodes, edges, node_details


def run(args, work, report):
    # benchmark.py is intentionally not imported: its existing top-level script
    # parses argv and runs a benchmark. Reuse only its small documented prefix.
    if args.binary is None:
        owned, manifest = raft_case.build_variant(work / "target", "healthy", ["/usr/bin/gcc"])
        args.binary = (owned / "raft-case").resolve()
        report["workload"] = {"family": "RedisLabs Raft library, project-owned nonblocking fixture",
            "scope": "three-node Raft C library, not the complete RedisRaft Redis module",
            "build_manifest": str(owned / "manifest.json"), "native_oracle": manifest["native"]}
    else:
        report["workload"] = {"family": "explicit binary", "binary": str(args.binary)}
    plugins = [args.plugin_dir / "choose.so", args.plugin_dir / "check.so"]
    shim = args.plugin_dir / "seed.so"
    for path in (args.tracer, args.binary, *plugins, shim):
        common.require(path.is_file(), "missing benchmark input: " + str(path))
    report["inputs_sha256"] = {str(path): common.digest(path) for path in
                               (args.tracer, args.binary, *plugins, shim, Path(__file__))}
    old_preload, old_seed = os.environ.get("LD_PRELOAD"), os.environ.get("DETSIM_BENCH_SEED")
    old_limit = common.MODEL_ADDRESS_SPACE_LIMIT
    common.MODEL_ADDRESS_SPACE_LIMIT = 3 * 1024**3
    os.environ["LD_PRELOAD"] = str(shim) + (":" + old_preload if old_preload else "")
    os.environ["DETSIM_BENCH_SEED"] = str(args.seed)
    try:
        session = common.Session(args.tracer, [str(args.binary), "0"], plugins, work / "session",
            args.backend, args.timeout, choices=True,
            tracees=[[str(args.binary), str(node)] for node in range(3)],
            addresses=[f"192.168.0.{node + 1}" for node in range(3)],
            runtime_profile={"fixed_layout": True, "startup_syscall": startup_boundary(args.binary)},
            target_cwd=work)
    finally:
        common.MODEL_ADDRESS_SPACE_LIMIT = old_limit
        for name, previous in (("LD_PRELOAD", old_preload), ("DETSIM_BENCH_SEED", old_seed)):
            if previous is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = previous
    failed = True
    try:
        checked(session)
        last = [""] * 3
        started = time.monotonic()
        for _ in range(args.rounds):
            for node in range(3):
                checked(session, f"sw {node}")
                advance = node == 0 and ("bind(" in last[node] or
                    ("recvfrom(" in last[node] and "= -11" in last[node]))
                last[node] = checked(session, "si " + ("1" if advance else "0"))
                common.require("has exited" not in last[node], "prefix guest exited")
        report["prefix"] = {"seconds": time.monotonic() - started, "steps": args.rounds * 3,
                            "rounds": args.rounds, "nodes": observe(session)}
        if args.rounds >= 100:
            common.require(sum(node["state"] == 4 for node in report["prefix"]["nodes"]) == 1,
                           "prefix did not establish one leader")
        listings = []
        for node in range(3):
            checked(session, f"sw {node}"); listings.append(checked(session, "thread"))
        report["prefix"]["thread_listings"] = listings
        common.require(any("state=runnable" in text or "deadline=" in text for text in listings),
                       "prefix is an untimed all-waiting state; not a throughput workload")
        seed = session.current_hash()
        common.require(seed, "missing common seed identity")
        report["seed"] = seed
        before = tree(session)
        (work / "prefix-tree.json").write_text(json.dumps(sorted(before)))
        started = time.monotonic()
        reply = checked(session, f"dfs {args.depth} --quiet")
        elapsed = time.monotonic() - started
        (work / "serial-dfs.txt").write_text(reply)
        match = re.search(r"(?:Complete explore all|Searched for) (\d+) sys_states \(new: (\d+)\)", reply)
        common.require(match, "serial DFS did not report bounded search completion")
        native_metrics = common.search_metrics(reply)
        common.require(native_metrics.get("elapsed_s", 0) > 0, "serial native timing unavailable")
        native_seconds = native_metrics["elapsed_s"]
        after = tree(session)
        serial_nodes, serial_edges = tree_projection(after, seed, args.depth)
        (work / "serial-tree.json").write_text(json.dumps(sorted(after)))
        (work / "serial-projection.json").write_text(json.dumps(
            {"nodes": sorted(serial_nodes), "first_parent_edges": sorted(serial_edges)}, indent=2))
        prefix_nodes = {seed} | {key for edge in before for key in edge[:2]}
        serial_complete_nodes = {seed} | {key for edge in after for key in edge[:2]}
        serial_generated_nodes = serial_complete_nodes - prefix_nodes
        report["serial"] = {"strategy": "dfs", "seconds": native_seconds,
            "command_seconds": elapsed, "native_metrics": native_metrics,
            "complete_tree_nodes_including_prefix": len(serial_complete_nodes),
            "prefix_nodes": len(prefix_nodes), "generated_new_nodes": len(serial_generated_nodes),
            "state_visits_reported": int(match[1]), "new_states_reported": int(match[2]),
            "state_visits_per_second": int(match[1]) / native_seconds,
            "first_parent_nodes_lower_bound": len(serial_nodes), "first_parent_edges_lower_bound": len(serial_edges),
            "first_parent_edge_lower_bound_per_second": len(serial_edges) / native_seconds,
            "depth_cut": "Depth limit reached" in reply, "model_budget_cut": "Model budget exhausted" in reply,
            "scope": "canonical first-parent projection only; not full serial graph equivalence"}
        report["parallel"] = []
        reference = None
        native_baselines = {}
        prefix_targets = {edge[1] for edge in before}
        for strategy in ("dfs", "bfs"):
            for workers in (1, 2, 4):
                common.require("Loaded state " + seed in checked(session, "load " + seed), "seed load failed")
                common.require(session.current_hash() == seed, "loaded seed identity drift")
                remaining_ms = max(1, int((session.started + session.timeout - time.monotonic()) * 1000))
                started = time.monotonic()
                reply = checked(session, f"parallel {workers} {args.depth} --strategy {strategy} --retries 0 --timeout-ms {remaining_ms}")
                elapsed = time.monotonic() - started
                matches = re.findall(r"(?m)^PARALLEL_RESULT (\{[^\n]+\})$", reply)
                common.require(len(matches) == 1, "missing native parallel receipt")
                receipt = json.loads(matches[0])
                common.require(receipt.get("seconds", 0) > 0, "parallel native timing unavailable")
                native_seconds = receipt["seconds"]
                row = dict(receipt, command_seconds=elapsed,
                    nodes_per_second=receipt["nodes"] / native_seconds,
                    edges_per_second=receipt["edges"] / native_seconds,
                    attempted_edges_per_second=receipt["attempted_edges"] / native_seconds)
                report["parallel"].append(row)
                (work / "results.json").write_text(json.dumps(report, indent=2))
                common.require(receipt["status"] in ("complete", "bounded") and receipt["retries"] == 0,
                               "native parallel search failed: " + json.dumps(receipt))
                common.require(receipt["edges"] > 0, "checkpoint has no executed edges; not a throughput workload")
                nodes, edges, node_details = parallel_graph(receipt)
                projected = {edge[:6] for edge in edges}
                row.update(serial_complete_tree_contains_all_parallel_nodes=nodes <= serial_complete_nodes,
                           serial_complete_tree_missing_nodes=sorted(nodes - serial_complete_nodes),
                           new_node_sets_equal=(nodes - prefix_nodes) == serial_generated_nodes,
                           parallel_generated_new_nodes=len(nodes - prefix_nodes),
                           phase_counts=dict(Counter(phase for depth, phase in node_details.values())),
                           serial_projection_nodes_subset=serial_nodes <= nodes,
                           serial_projection_edges_subset=serial_edges <= projected,
                           serial_missing_nodes=sorted(serial_nodes - nodes),
                           serial_missing_edges=sorted(serial_edges - projected),
                           returned_prefix_nodes=sorted((nodes & prefix_targets) - {seed}))
                common.require(serial_nodes <= nodes and serial_edges <= projected,
                               "parallel omitted a bounded serial first-parent witness")
                common.require(row["serial_complete_tree_contains_all_parallel_nodes"] and row["new_node_sets_equal"],
                               "native serial/parallel generated-node coverage differs")
                if reference is None:
                    reference = nodes, edges, node_details
                row["parallel_graph_equal"] = (nodes, edges) == reference[:2]
                row["node_depth_phase_equal"] = node_details == reference[2]
                common.require(row["parallel_graph_equal"] and row["node_depth_phase_equal"],
                               "bounded native parallel graph/depth/phase differs")
                common.require(session.current_hash() == seed, "parallel changed coordinator source cursor")
                native_baselines.setdefault(strategy, native_seconds)
                row["speedup_vs_same_strategy_1_worker"] = native_baselines[strategy] / native_seconds
                # Serial reports visits rather than full edges; this ratio is raw
                # latency, not an equal-work speedup or scalability conclusion.
                row["raw_latency_ratio_vs_serial"] = report["serial"]["seconds"] / native_seconds
                (work / "results.json").write_text(json.dumps(report, indent=2))
        report["passed"] = True
        failed = False
    finally:
        if failed and session.proc.poll() is None:
            os.kill(session.proc.pid, signal.SIGINT)
        report["tracer_exit"] = session.close()
        common.require(report["tracer_exit"] == 0, "benchmark tracer cleanup failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=ROOT / "tracer")
    parser.add_argument("--binary", type=Path, help="override the default owned healthy fixture build")
    parser.add_argument("--plugin-dir", type=Path, default=ROOT / "build/release/np3/examples/redisraft")
    parser.add_argument("--output", type=Path, default=ROOT / "build/redisraft-parallel-benchmark")
    parser.add_argument("--backend", choices=("full", "pages"), default="pages")
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.depth <= 8 or args.rounds < 1 or not 0 < args.timeout <= 120:
        parser.error("depth1..8, positive rounds, timeout0..120 required")
    for name in ("tracer", "binary", "plugin_dir", "output"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    report = {"passed": False, "artifacts": str(work), "backend": args.backend, "depth": args.depth,
              "scope": __doc__, "timing": "native CLI search elapsed_s / PARALLEL_RESULT seconds; command_seconds includes driver polling separately"}
    started = time.monotonic()
    try:
        run(args, work, report)
    except Exception as error:
        report["passed"] = False; report["error"] = str(error)
    report["total_seconds"] = time.monotonic() - started
    (work / "results.json").write_text(json.dumps(report, indent=2))
    print("RESULT " + str(work / "results.json"), flush=True)
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
