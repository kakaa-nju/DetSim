#!/usr/bin/env python3
"""Same-checkpoint native DFS/parallel benchmark for the three-node Raft library.

Python prepares the prefix and checks native reports; it never explores states.
Serial `tree` stores first incoming edges only, so its projection is subset
coverage evidence, not a complete transition graph or an exhaustive oracle.
"""
import argparse
import hashlib
from collections import Counter, defaultdict, deque
import json
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.fixture_profile import startup_boundary
from redisraft import run as raft_case

ROOT = Path(__file__).resolve().parents[2]


class ResourceSample:
    """Read-only samples of this tracer's process tree, never system-wide totals.

    CPU and memory peaks are sampled lower bounds. RSS sums shared pages more
    than once; PSS is collected separately, at most once per second.
    """
    def __init__(self, session, enabled):
        self.session, self.enabled = session, enabled
        self.stop_event = threading.Event()
        self.thread = None
        self.initial, self.maximum = {}, {}
        self.peak_rss = self.peak_pss = self.peak_processes = 0
        self.samples = self.pss_samples = self.missed = 0
        self.first_lease = None
        self.old_directories = set(session.directory.glob('.parallel-*'))
        self.started = time.monotonic()
        self.last_pss = 0.0

    def sample(self, initial=False):
        pending, seen, processes = [self.session.proc.pid], set(), []
        while pending:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
            try:
                fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
                key = (pid, int(fields[19]))
                ticks = int(fields[11]) + int(fields[12])
                self.maximum[key] = max(ticks, self.maximum.get(key, 0))
                if initial:
                    self.initial[key] = ticks
                processes.append((pid, int(fields[21]) * os.sysconf('SC_PAGE_SIZE')))
                pending.extend(int(value) for value in Path(f'/proc/{pid}/task/{pid}/children').read_text().split())
            except (OSError, ValueError, IndexError):
                self.missed += 1
        self.peak_rss = max(self.peak_rss, sum(rss for _, rss in processes))
        self.peak_processes = max(self.peak_processes, len(processes))
        self.samples += 1
        now = time.monotonic()
        if now - self.last_pss >= 1:
            pss = 0
            for pid, _ in processes:
                try:
                    match = re.search(r'^Pss:\s+(\d+) kB$', Path(f'/proc/{pid}/smaps_rollup').read_text(), re.M)
                    if match:
                        pss += int(match[1]) * 1024
                except OSError:
                    self.missed += 1
            self.peak_pss = max(self.peak_pss, pss)
            self.pss_samples += 1
            self.last_pss = now
        if self.first_lease is None:
            for directory in set(self.session.directory.glob('.parallel-*')) - self.old_directories:
                try:
                    with (directory / 'journal.jsonl').open() as log:
                        records = log.read(65536).splitlines()
                    if any(json.loads(line).get('event') == 'lease' for line in records if line.endswith('}')):
                        self.first_lease = time.monotonic() - self.started
                        break
                except (OSError, ValueError):
                    pass

    def __enter__(self):
        if self.enabled:
            self.sample(initial=True)
            self.started = time.monotonic()
            def poll():
                while not self.stop_event.wait(.1):
                    self.sample()
            self.thread = threading.Thread(target=poll, daemon=True)
            self.thread.start()
        return self

    def __exit__(self, *_):
        if self.thread:
            self.stop_event.set()
            self.thread.join()
            self.sample()
        self.elapsed = time.monotonic() - self.started

    def result(self):
        if not self.enabled:
            return None
        cpu = sum(max(0, count - self.initial.get(key, 0)) for key, count in self.maximum.items()) / os.sysconf('SC_CLK_TCK')
        return dict(observed_cpu_seconds=cpu, observation_seconds=self.elapsed,
                    observed_average_cpu_cores=cpu / self.elapsed,
                    peak_summed_rss_bytes=self.peak_rss, peak_summed_pss_bytes=self.peak_pss,
                    peak_processes=self.peak_processes, samples=self.samples, pss_samples=self.pss_samples,
                    missed_process_reads=self.missed, first_lease_observed_seconds=self.first_lease,
                    scope='owned tracer/worker/tracee process tree; ~100ms samples, PSS <=1Hz; CPU and peaks lower bounds; summed RSS double-counts shared memory; first lease is sampled startup observation, not exact bootstrap timing')


def transport_summary(receipt):
    records = [json.loads(line) for line in (Path(receipt['artifacts']) / 'journal.jsonl').read_text().splitlines()]
    rows = [record for record in records if record.get('event') == 'transport']
    total_fields = ('stream_send_calls', 'stream_recv_calls', 'stream_poll_calls', 'stream_bytes_sent',
                    'stream_bytes_received', 'shared_frames', 'raw_fallback_frames', 'raw_fallback_bytes',
                    'object_send_calls', 'object_recv_calls', 'object_poll_calls', 'memfds_created',
                    'memfd_bytes_created', 'segments_received', 'reference_bytes', 'missing_bytes')
    peaks = ('peak_inflight_bytes', 'cache_bytes', 'cache_segments', 'peak_cache_bytes')
    work = {}
    for record in records:
        event = record.get('event')
        if event not in ('lease', 'edge', 'done'):
            continue
        worker = work.setdefault(str(record['worker']), dict(leases=0, edges=0, done=0, probe_leases=0,
                                                           zero_emission_done=0, done_kinds={}))
        worker[event + 's' if event != 'done' else 'done'] += 1
        if event == 'lease' and not record['may_expand']:
            worker['probe_leases'] += 1
        if event == 'done':
            worker['zero_emission_done'] += record['emitted'] == 0
            kind = str(record['kind'])
            worker['done_kinds'][kind] = worker['done_kinds'].get(kind, 0) + 1
    return dict(records=len(rows), work_by_worker=work,
                done_kind_schema={'0': 'expanded', '1': 'terminal', '2': 'depth_cut', '3': 'model_budget', '4': 'violation'},
                totals={key: sum(row.get(key, 0) for row in rows) for key in total_fields},
                peaks={key: max((row.get(key, 0) for row in rows), default=0) for key in peaks},
                scope='coordinator endpoints only; counters are not the sum of both IPC peers')


def canonical_graph(seed, receipt):
    """Canonical labels for a recorded graph; no target execution or search."""
    nodes, edges, details = parallel_graph(receipt)
    outgoing = defaultdict(list)
    for edge in edges:
        outgoing[edge[0]].append(edge)
    identifiers, queue, normalized = {seed: 0}, [seed], []
    for source in queue:
        labels = set()
        for edge in sorted(outgoing[source], key=lambda item: item[2:]):
            label = edge[2:]
            common.require(label not in labels, 'nondeterministic duplicate edge label in journal')
            labels.add(label)
            if edge[1] not in identifiers:
                identifiers[edge[1]] = len(identifiers)
                queue.append(edge[1])
            normalized.append((identifiers[source], identifiers[edge[1]], *label))
    common.require(set(identifiers) == nodes, 'journal has unreachable nodes')
    return dict(nodes=[(identifiers[node], *details[node]) for node in queue], edges=normalized)


def reference_inputs(report, path):
    if 'execution_inputs' in report:
        return report['execution_inputs']
    # Only an explicit launcher record can supply missing legacy inputs. Never
    # guess a host search seed from the saved checkpoint hash or a default.
    sidecar = path.parent / 'launch.json'
    common.require(sidecar.exists(), 'legacy reference needs recorded launch execution_inputs')
    return json.loads(sidecar.read_text())['execution_inputs']


def validate_reference(report, reference_path):
    reference = json.loads(reference_path.read_text())
    common.require(reference.get('passed') and reference.get('tracer_exit') == 0, 'reference did not pass')
    expected, actual = reference_inputs(reference, reference_path), report['execution_inputs']
    common.require(expected == actual,
                   'reference tracer/fixture/plugin/seed/backend/depth/prefix inputs differ')
    common.require(reference.get('seed_model_budget') == report['seed_model_budget'] and
                   reference['prefix']['nodes'] == report['prefix']['nodes'], 'reference seed model budget/business projection differs')
    common.require(reference.get('serial', {}).get('local') is not False,
                   'reference needs local serial coverage validation, not another reference')
    witnesses = [row for row in reference['parallel'] if
                 row.get('new_node_sets_equal') and row.get('serial_projection_edges_subset')]
    common.require(witnesses, 'reference lacks a graph with local serial coverage validation')
    expected_graph = canonical_graph(reference['seed'], witnesses[0])
    for row in report['parallel']:
        common.require(canonical_graph(report['seed'], row) == expected_graph, 'fresh graph differs from validated reference')
        row['shared_reference_graph_isomorphic'] = True
        row['serial_oracle_kind'] = 'shared_reference; not run locally'
    report['serial'] = dict(local=False, validation='shared_reference', source=str(reference_path),
        tracer_changed=expected['tracer_sha256'] != actual['tracer_sha256'],
        scope='prior local serial node/first-parent gate plus complete labeled-graph isomorphism; not arbitrary memory equivalence')


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
    report['execution_inputs'] = dict(tracer_sha256=common.digest(args.tracer), binary_sha256=common.digest(args.binary),
        plugins_sha256=[common.digest(path) for path in plugins], seed_shim_sha256=common.digest(shim),
        seed=args.seed, backend=args.backend, depth=args.depth, rounds=args.rounds, storage=args.storage)
    (work / 'driver-source.py').write_bytes(Path(__file__).read_bytes())
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
        report["seed_model_budget"] = raft_case.saved_budget(session.directory, seed)
        before = tree(session)
        (work / "prefix-tree.json").write_text(json.dumps(sorted(before)))
        serial_data = {}
        def run_serial():
            started = time.monotonic()
            reply = checked(session, f"dfs {args.depth} --quiet")
            elapsed = time.monotonic() - started
            (work / "serial-dfs.txt").write_text(reply)
            match = re.search(r"(?:Complete explore all|Searched for) (\d+) sys_states \(new: (\d+)\)", reply)
            common.require(match, "serial DFS did not report bounded search completion")
            native_metrics = common.search_metrics(reply)
            common.require(native_metrics.get('completion') in ('complete', 'depth_limit', 'model_budget'),
                           'serial oracle interrupted or has an unclassified frontier')
            common.require(native_metrics.get("elapsed_s", 0) > 0, "serial native timing unavailable")
            native_seconds = native_metrics["elapsed_s"]
            work_receipts = [json.loads(text) for text in re.findall(r"SEARCH_WORK (\{[^\n]+\})", reply)]
            common.require(len(work_receipts) <= 1, "duplicate native serial work receipt")
            actual_transitions = work_receipts[0]['transitions'] if work_receipts else None
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
                "actual_transitions": actual_transitions,
                "state_visits_per_second": int(match[1]) / native_seconds,
                "first_parent_nodes_lower_bound": len(serial_nodes), "first_parent_edges_lower_bound": len(serial_edges),
                "first_parent_edge_lower_bound_per_second": len(serial_edges) / native_seconds,
                "depth_cut": "Depth limit reached" in reply, "model_budget_cut": "Model budget exhausted" in reply,
                "scope": "canonical first-parent projection only; not full serial graph equivalence"}
            serial_data.update(nodes=serial_nodes, edges=serial_edges, prefix_nodes=prefix_nodes,
                               complete_nodes=serial_complete_nodes, generated_nodes=serial_generated_nodes)
        if not args.parallel_first:
            run_serial()
        report["parallel"] = []
        (work / 'results.json').write_text(json.dumps(report, indent=2))
        reference = None
        native_baselines = {}
        prefix_targets = {edge[1] for edge in before}
        def compare_serial(row, nodes, edges, node_details):
            serial_nodes, serial_edges = serial_data['nodes'], serial_data['edges']
            prefix_nodes, serial_complete_nodes = serial_data['prefix_nodes'], serial_data['complete_nodes']
            serial_generated_nodes = serial_data['generated_nodes']
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
        for strategy in args.strategies:
            for workers in args.workers:
                common.require("Loaded state " + seed in checked(session, "load " + seed), "seed load failed")
                common.require(session.current_hash() == seed, "loaded seed identity drift")
                remaining_ms = min(3_600_000, max(1, int((session.started + session.timeout - time.monotonic()) * 1000)))
                started = time.monotonic()
                with ResourceSample(session, args.sample_resources) as resources:
                    reply = checked(session, f"parallel {workers} {args.depth} --strategy {strategy} --storage {args.storage} --retries 0 --timeout-ms {remaining_ms}")
                elapsed = time.monotonic() - started
                matches = re.findall(r"(?m)^PARALLEL_RESULT (\{[^\n]+\})$", reply)
                common.require(len(matches) == 1, "missing native parallel receipt")
                receipt = json.loads(matches[0])
                common.require(receipt.get("seconds", 0) > 0, "parallel native timing unavailable")
                native_seconds = receipt["seconds"]
                row = dict(receipt, command_seconds=elapsed, resources=resources.result(),
                    transport=transport_summary(receipt),
                    nodes_per_second=receipt["nodes"] / native_seconds,
                    edges_per_second=receipt["edges"] / native_seconds,
                    attempted_edges_per_second=receipt["attempted_edges"] / native_seconds,
                    command_edges_per_second=receipt["edges"] / elapsed,
                    command_tail_and_driver_seconds=elapsed - native_seconds,
                    coordinator_store_condition='prefix-only before parallel' if args.parallel_first else
                        'prewarmed by serial oracle and possibly earlier parallel runs')
                report["parallel"].append(row)
                (work / "results.json").write_text(json.dumps(report, indent=2))
                common.require(receipt["status"] in ("complete", "bounded") and receipt["retries"] == 0,
                               "native parallel search failed: " + json.dumps(receipt))
                common.require(receipt["edges"] > 0, "checkpoint has no executed edges; not a throughput workload")
                nodes, edges, node_details = parallel_graph(receipt)
                row['phase_counts'] = dict(Counter(phase for depth, phase in node_details.values()))
                if serial_data:
                    compare_serial(row, nodes, edges, node_details)
                if reference is None:
                    reference = nodes, edges, node_details
                row["parallel_graph_equal"] = (nodes, edges) == reference[:2]
                row["node_depth_phase_equal"] = node_details == reference[2]
                common.require(row["parallel_graph_equal"] and row["node_depth_phase_equal"],
                               "bounded native parallel graph/depth/phase differs")
                common.require(session.current_hash() == seed, "parallel changed coordinator source cursor")
                if workers == 1:
                    native_baselines[strategy] = native_seconds
                row["speedup_vs_same_strategy_1_worker"] = native_baselines.get(strategy, 0) / native_seconds if strategy in native_baselines else None
                # Serial reports visits rather than full edges; this ratio is raw
                # latency, not an equal-work speedup or scalability conclusion.
                row["raw_latency_ratio_vs_serial"] = report["serial"]["seconds"] / native_seconds if serial_data else None
                (work / "results.json").write_text(json.dumps(report, indent=2))
        if args.reference_report:
            validate_reference(report, args.reference_report)
        elif args.parallel_first:
            run_serial()
            for row in report['parallel']:
                compare_serial(row, *parallel_graph(row))
                row['raw_latency_ratio_vs_serial'] = report['serial']['seconds'] / row['seconds']
            report['serial']['store_condition'] = 'oracle after measured parallel; not a cold serial timing comparison'
        for row in report["parallel"]:
            baseline = next((candidate for candidate in report["parallel"]
                             if candidate['strategy'] == row['strategy'] and candidate['workers'] == 1), None)
            row['speedup_vs_same_strategy_1_worker'] = baseline['seconds'] / row['seconds'] if baseline else None
            row['command_speedup_vs_same_strategy_1_worker'] = baseline['command_seconds'] / row['command_seconds'] if baseline else None
        for index, row in enumerate(report['parallel']):
            graph = canonical_graph(seed, row)
            encoded = json.dumps(graph, separators=(',', ':')).encode()
            graph_path = work / f'canonical-graph-{index}.json'
            graph_path.write_bytes(encoded)
            row['canonical_graph'] = str(graph_path)
            row['canonical_graph_sha256'] = hashlib.sha256(encoded).hexdigest()
        report["passed"] = True
        failed = False
    finally:
        if failed and session.proc.poll() is None:
            os.kill(session.proc.pid, signal.SIGINT)
        finalization_started = time.monotonic()
        report["tracer_exit"] = session.close(timeout=args.timeout)
        report["finalization_seconds"] = time.monotonic() - finalization_started
        report["search_and_finalization_seconds"] = sum(row['command_seconds'] for row in report.get('parallel', [])) + report["finalization_seconds"]
        report["finalization_scope"] = "graceful tracer shutdown after validation; includes store durability barriers, reported separately from search command"
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
    parser.add_argument("--workers", type=int, nargs='+', default=[1, 2, 4], help="explicit worker counts, including 8 for large cases")
    parser.add_argument("--strategies", choices=('dfs', 'bfs'), nargs='+', default=['dfs', 'bfs'])
    parser.add_argument("--storage", choices=('shared', 'durable'), default='shared')
    parser.add_argument("--reference-report", type=Path, help="locally verified same-workload graph; skips local serial and requires parallel-first")
    parser.add_argument("--parallel-first", action='store_true', help="one worker/strategy per fresh session; time parallel before serial oracle")
    parser.add_argument("--sample-resources", action='store_true', help="sample owned tree CPU/RSS and low-frequency PSS")
    args = parser.parse_args()
    if not 1 <= args.depth <= 64 or args.rounds < 1 or not 0 < args.timeout <= 7200:
        parser.error("depth1..64, positive rounds, total-session timeout0..7200 required")
    if not args.workers or any(not 1 <= count <= 32 for count in args.workers) or len(set(args.workers)) != len(args.workers):
        parser.error("workers must be distinct counts from 1..32")
    if len(set(args.strategies)) != len(args.strategies):
        parser.error("strategies must be distinct")
    if args.reference_report and not args.parallel_first:
        parser.error("reference-report requires parallel-first")
    if args.reference_report:
        args.reference_report = args.reference_report.resolve()
    if args.parallel_first and (len(args.workers) != 1 or len(args.strategies) != 1):
        parser.error("parallel-first requires one worker count and one strategy to avoid warm/cold mixing")
    for name in ("tracer", "binary", "plugin_dir", "output"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    report = {"passed": False, "artifacts": str(work), "backend": args.backend, "depth": args.depth,
              "workers": args.workers, "strategies": args.strategies, "storage": args.storage, "sample_resources": args.sample_resources, "parallel_first": args.parallel_first, "reference_report": str(args.reference_report) if args.reference_report else None,
              "scope": __doc__, "timing": "command_seconds is complete command to prompt (including tail journal and driver polling); native seconds separately excludes final node journal; primary large-scale throughput uses command_seconds"}
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
