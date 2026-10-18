#!/usr/bin/env python3
"""Reproducible native Willemt Raft search timing plus a separate projection audit.

Python prepares the existing case, runs native DFS, and validates receipts. It
never searches. Audit projection equality does not assert full memory equality.
"""
import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import statistics
import tempfile

_case_spec = importlib.util.spec_from_file_location("willemt_benchmark_case", Path(__file__).with_name("run.py"))
case = importlib.util.module_from_spec(_case_spec)
_case_spec.loader.exec_module(case)

common = case.common
BASE_SESSION = common.Session


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


@contextmanager
def audit_session(observer, captured):
    class AuditSession(BASE_SESSION):
        def __init__(self, tracer, argv, plugins, *args, **kwargs):
            super().__init__(tracer, argv, list(plugins) + [observer], *args, **kwargs)

        def send(self, text=None):
            if text is None or not text.startswith("dfs "):
                return super().send(text)
            path = self.directory / "benchmark-transitions.jsonl"
            offset = path.stat().st_size if path.exists() else 0
            reply = super().send(text)
            with path.open("rb") as stream:
                stream.seek(offset)
                records = [json.loads(line) for line in stream]
            search_path = self.directory / "automatic-transitions.jsonl"
            search_path.write_text("".join(canonical(record) + "\n" for record in records))
            edges = Counter(canonical(record) for record in records)
            multiset = [[json.loads(edge), count] for edge, count in sorted(edges.items())]
            graph_path = self.directory / "projected-edge-multiset.json"
            graph_path.write_text(json.dumps(multiset, indent=2) + "\n")
            captured.update(actual_transitions=len(records), projected_edges=len(edges),
                            projected_graph_sha256=hashlib.sha256(canonical(multiset).encode()).hexdigest(),
                            edge_multiset=str(graph_path), events=str(search_path))
            return reply

    common.Session = AuditSession
    try:
        yield
    finally:
        common.Session = BASE_SESSION


def measure(tracer, fixture, checker, choose, directory, args, observer=None):
    audit = {}
    if observer:
        with audit_session(observer, audit):
            row = case.model_case(tracer, fixture, checker, choose, directory,
                                  args.backend, "healthy", args.timeout, args.depth)
    else:
        row = case.model_case(tracer, fixture, checker, choose, directory,
                              args.backend, "healthy", args.timeout, args.depth)
    common.require(row.get("status") == "pass" and row.get("tracer_exit") == 0, str(row))
    metrics = row["search_metrics"]
    common.require(metrics.get("searched", 0) > 1 and metrics.get("elapsed_s", 0) > 0,
                   "native search counters/timing unavailable")
    common.require(row.get("replay") == "same_process_hash_match_3x" and
                   min(node["commit_idx"] for node in row["nodes"]) >= 2,
                   "healthy business completion/replay evidence missing")
    work_reports = [json.loads(text) for text in re.findall(r"SEARCH_WORK (\{[^\n]+\})", (directory / "search.log").read_text())]
    common.require(len(work_reports) <= 1, "duplicate native search-work counter")
    transitions = work_reports[0].get("transitions") if work_reports else None
    common.require(transitions is None or (type(transitions) is int and transitions > 0), "invalid native transition count")
    if observer and transitions is not None:
        common.require(transitions == audit["actual_transitions"], "native transition counter differs from actual callbacks")
    row.update(kind="audit" if observer else "timing", audit=audit,
               visits=metrics["searched"], new_nodes=metrics.get("new"),
               visits_per_second=metrics["searched"] / metrics["elapsed_s"],
               actual_transitions=transitions if transitions is not None else audit.get("actual_transitions"),
               actual_transitions_source="native SEARCH_WORK" if transitions is not None else
                   "observer callbacks" if observer else "unavailable in this binary; see separate audit",
               timing_scope="native automatic DFS only; excludes build, setup, inspection and replay")
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, action="append", required=True,
                        help="immutable NP3 binary; repeat for alternating paired versions")
    parser.add_argument("--output", type=Path, default=case.ROOT / "build/raft-search-benchmark")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--depth", type=int, default=6)
    parser.add_argument("--backend", choices=["pages", "full"], default="pages")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--threshold", type=float, default=500)
    parser.add_argument("--seed", type=int, default=1, help="fixed host DFS srand seed via existing benchmark interposer")
    args = parser.parse_args()
    common.require(args.repeats >= 3 and args.depth > 0, "at least 3 repeats and positive depth required")
    tracers = [path.resolve() for path in args.tracer]
    common.require(len(set(tracers)) == len(tracers), "duplicate tracer path")
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    report = {"passed": False, "performance_target_met": False, "rows": [], "audits": [],
              "workdir": str(work), "backend": args.backend, "depth": args.depth,
              "repeats": args.repeats, "threshold_visits_per_second": args.threshold, "host_search_seed": args.seed,
              "profile_difference_from_2026_10_18_perf": "Explicit fixed srand interposer; historical 159 visits/s used time-based seed.",
              "comparison_scope": "same fixture/plugins/config inputs; fresh launches; Raft projection edge multiset + native counters",
              "limitations": ["Projection is not full native-memory graph equality.",
                              "Cross-binary model identities differ; no checkpoint compatibility bypass.",
                              "Timed visits are not actual transitions; old tracers lack an actual-transition counter.",
                              "Raw timing only includes each tracer's reported automatic search interval."]}
    try:
        # Build all immutable workload inputs before any timed run.
        fixture_dir, manifest = case.build_variant(work, "healthy", ["/usr/bin/gcc"])
        fixture = fixture_dir / "raft-case"
        checker, choose, observer = work / "checker.so", work / "choose.so", work / "observer.so"
        common.build_plugin(case.HERE / "plugins/check.cpp", checker, ["/usr/bin/g++"])
        common.build_plugin(case.HERE / "raft_time_choice.cpp", choose, ["/usr/bin/g++"])
        common.build_plugin(case.HERE / "benchmark_observer.cpp", observer, ["/usr/bin/g++"])
        seed_source = case.ROOT / "examples/redisraft/seed.c"
        seed_shim = work / "seed.so"
        result = common.command(["/usr/bin/gcc", "-shared", "-fPIC", seed_source, "-ldl", "-o", seed_shim], case.ROOT)
        common.require(result.returncode == 0, result.stdout)
        os.environ["LD_PRELOAD"] = str(seed_shim) + (":" + os.environ["LD_PRELOAD"] if os.environ.get("LD_PRELOAD") else "")
        os.environ["DETSIM_BENCH_SEED"] = str(args.seed)
        sources = [seed_source, Path(__file__).resolve(), case.HERE / "benchmark_observer.cpp", case.HERE / "run.py",
                   case.HERE / "raft_fixture.c", case.HERE / "plugins/check.cpp", case.HERE / "plugins/check_common.hpp",
                   case.HERE / "raft_time_choice.cpp", case.ROOT / "examples/_support/session.py"]
        archive = work / "source-archive"
        archive.mkdir()
        for source in sources:
            copy = archive / (common.digest(source) + "-" + source.name)
            copy.write_bytes(source.read_bytes())
            copy.chmod(0o400)
        report.update(tracers={str(path): common.digest(path) for path in tracers},
                      fixture_manifest=str(fixture_dir / "manifest.json"), fixture_sha256=manifest["binary_sha256"],
                      checker_sha256=common.digest(checker), choose_sha256=common.digest(choose),
                      observer_sha256=common.digest(observer), seed_shim_sha256=common.digest(seed_shim), sources={str(path): common.digest(path) for path in sources})
        for repeat in range(args.repeats):
            # Alternate order to avoid always granting one version the warmest host.
            order = tracers if repeat % 2 == 0 else list(reversed(tracers))
            for tracer in order:
                index = tracers.index(tracer)
                row = measure(tracer, fixture, checker, choose, work / f"timing-{repeat}-{index}", args)
                row.update(repeat=repeat, tracer=str(tracer))
                report["rows"].append(row)
                print("TIMING", index, repeat, row["visits"], row["new_nodes"], row["search_metrics"]["elapsed_s"], flush=True)
                (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        for index, tracer in enumerate(tracers):
            row = measure(tracer, fixture, checker, choose, work / f"audit-{index}", args, observer)
            row["tracer"] = str(tracer)
            report["audits"].append(row)
            print("AUDIT", index, row["audit"], flush=True)
        prefix_schedules = {canonical([(event["node"], event["choice"]) for event in
                            json.loads((Path(row["artifacts"]) / "events.json").read_text())])
                            for row in report["rows"] + report["audits"]}
        common.require(len(prefix_schedules) == 1, "manual business-prefix schedules differ")
        signatures = {(row["visits"], row["new_nodes"], row["search_metrics"]["completion"])
                      for row in report["rows"] + report["audits"]}
        common.require(len(signatures) == 1, "logical work/cutoff counts differ across repeats, versions or observer audit")
        graph_hashes = {row["audit"]["projected_graph_sha256"] for row in report["audits"]}
        common.require(len(graph_hashes) == 1, "normalized Raft transition multiset differs across tracers")
        config_hashes = {row["config_sha256"] for row in report["rows"]}
        common.require(len(config_hashes) == 1, "timed runs did not use byte-identical configuration")
        report["summary"] = []
        for tracer in tracers:
            rows = [row for row in report["rows"] if row["tracer"] == str(tracer)]
            rates = [row["visits_per_second"] for row in rows]
            audited = next(row["audit"]["actual_transitions"] for row in report["audits"] if row["tracer"] == str(tracer))
            native_counts = [row["actual_transitions"] for row in rows if row["actual_transitions"] is not None]
            common.require(not native_counts or (len(native_counts) == len(rows) and set(native_counts) == {audited}),
                           "timed actual-transition count differs from independent audit")
            report["summary"].append({"tracer": str(tracer), "median_visits_per_second": statistics.median(rates),
                "minimum_visits_per_second": min(rates), "all_repeats_above_threshold": min(rates) > args.threshold,
                "audit_actual_transitions": audited, "actual_transitions_source": "native timed counter verified against separate audit" if native_counts else
                    "separate matching-work audit, not timed-run counter"})
        report["passed"] = True
        report["performance_target_tracer"] = str(tracers[-1])
        report["performance_target_met"] = report["summary"][-1]["all_repeats_above_threshold"]
        return 0
    except Exception as error:
        report["diagnostic"] = str(error)
        return 1
    finally:
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print("RESULT", work / "results.json", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
