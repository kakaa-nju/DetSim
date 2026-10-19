#!/usr/bin/env python3
"""NP1 parallel actor/choice/pending/timeout oracle; Python does not search."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import tempfile

from native_search_actors import (
    ROOT, MODES, Session, actions, checked, command, current, digest,
    events, expected_event, fingerprint, prepare, require,
)


from parallel_scenarios import placement_command, original_affinities, assert_original_affinities, audit_placement, locality_command, audit_locality


def run(work, tracer, binary, plugin, mode, backend, worker_placement="none", worker_locality="none"):
    directory = work / f"{mode}-{backend}"
    session = Session(tracer, [str(binary), MODES[mode]], [str(plugin)], directory,
                      backend, 240, choose_points=[{"syscall": 96, "choose": 3}],
                      runtime_profile={"initial_time_us": 1000000, "fixed_layout": True})
    row = {"mode": mode, "backend": backend, "artifacts": str(directory), "passed": False}
    owned = set()
    worker_pids = set()
    try:
        source, inventory = prepare(session, mode)
        row.update(source=source, actors=inventory)
        children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
        require(len(children) == 1, "fixture must own one process leader")
        for child in children:
            owned.update(int(path.name) for path in Path(f"/proc/{child}/task").iterdir())
        require(len(owned) == 2, "fixture must own two live TIDs")
        row["owned_tids"] = sorted(owned)
        searches = []
        # Parallel searches precede manual references, so neither the state
        # history nor manually persisted successors can manufacture coverage.
        for strategy in ("dfs", "bfs"):
            before = original_affinities(session.proc.pid)
            selected_command, locality = locality_command(f"parallel 2 1 --strategy {strategy}", worker_locality)
            reply = checked(session, placement_command(selected_command, worker_placement))
            assert_original_affinities(before)
            (directory / f"parallel-{strategy}.log").write_text(reply)
            results = [json.loads(line) for line in re.findall(r"PARALLEL_RESULT (\{[^\n]+\})", reply)]
            require(len(results) == 1, "missing unique native parallel receipt: " + reply)
            result = results[0]
            audit_locality(result, locality)
            require(result["status"] == "bounded" and result["retries"] == 0, str(result))
            require(current(session) == source, "parallel search changed coordinator state")
            journal = Path(result["artifacts"]) / "journal.jsonl"
            records = [json.loads(line) for line in journal.read_text().splitlines()]
            placement = audit_placement(records, worker_placement, before[session.proc.pid])
            edge_rows = [record for record in records if record["event"] == "edge"]
            require(len(edge_rows) == result["edges"] == result["attempted_edges"] == len(actions(mode)),
                    "wrong native root action count")
            require(all(record["source"] == int(source, 16) and not record["violation"] for record in edge_rows),
                    "parallel depth one executed outside saved root")
            nodes = {record["state"]: record for record in records if record["event"] == "node"}
            targets = {record["target"] for record in edge_rows}
            require(set(nodes) == targets | {int(source, 16)} and len(nodes) == result["nodes"],
                    "coordinator node set differs from streamed root successors")
            require(nodes[int(source, 16)]["depth"] == 0 and nodes[int(source, 16)]["phase"] == "expanded" and
                    all(nodes[target]["depth"] == 1 for target in targets), "incorrect bounded graph depths")
            worker_pids.update(record["pid"] for record in records if record["event"] == "worker_spawn")
            require(all(not Path(f"/proc/{pid}").exists() for pid in worker_pids), "parallel worker remained alive")
            searches.append({"placement_audit": placement, "strategy": strategy, "result": result, "edges": edge_rows, "journal": str(journal)})
        row["searches"] = searches
        references = []
        for slot, choice in actions(mode):
            require("Loaded state" in checked(session, "load " + source), "manual source unavailable")
            checked(session, "sw 0")
            checked(session, "thread " + str(slot))
            reply = checked(session, "si " + choice)
            observed = events(reply)
            require(len(observed) == 1, "manual action lacks unique callback")
            event = observed[0]
            target = current(session)
            syscall, preset, result = expected_event(mode, slot, choice)
            require((event["process"], event["thread"], event["syscall"], event["choice"],
                     event["phase"], event["result_valid"], event["result"]) ==
                    (0, slot, syscall, preset, 2, 1, result), "manual oracle actor/outcome mismatch")
            if choice == "timeout":
                require(event["before"]["main_marker"] == event["after"]["main_marker"] == 0 and
                        event["before"]["worker_marker"] == event["after"]["worker_marker"] == 0,
                        "timeout resumed guest user code")
            replays = []
            for _ in range(3):
                checked(session, "load " + source)
                checked(session, "sw 0")
                checked(session, "thread " + str(slot))
                again = events(checked(session, "si " + choice))
                require(len(again) == 1 and fingerprint(again[0]) == fingerprint(event), "replay callback drift")
                replays.append(current(session))
                require(replays[-1] == target, "replay target hash drift")
            references.append({"thread": slot, "choice": preset, "action": "timeout" if choice == "timeout" else "normal",
                               "target": target, "event": event, "replays": replays})
        expected = Counter((0, item["thread"], item["choice"], item["action"], int(item["target"], 16)) for item in references)
        for search in searches:
            actual = Counter((edge["process"], edge["thread"], edge["choice"], edge["action"], edge["target"])
                             for edge in search["edges"])
            require(actual == expected, "parallel actor/choice/timeout hash set differs from independent manual oracle")
            for edge in search["edges"]:
                require(edge["choice_count"] == (3 if edge["choice"] >= 0 else 0), "choice cardinality leaked across actors")
        row.update(passed=True, reference=references, native_search_before_references=True,
                   config_sha256=digest(directory / "config.json"))
    except Exception as error:
        row["diagnostic"] = str(error)
    finally:
        row["tracer_exit"] = session.close(timeout=20)
        row["remaining_owned_tids"] = [pid for pid in owned if Path(f"/proc/{pid}").exists()]
        row["remaining_workers"] = [pid for pid in worker_pids if Path(f"/proc/{pid}").exists()]
        if row["tracer_exit"] != 0 or row["remaining_owned_tids"] or row["remaining_workers"]:
            row["passed"] = False
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, required=True)
    parser.add_argument("--worker-placement", choices=("none", "same-cpu"), default="none")
    parser.add_argument("--output", type=Path, default=ROOT / "build/parallel-actor-scenarios")
    parser.add_argument("--modes", nargs="+", choices=["choices", "mixed", "pending"], default=["choices", "mixed", "pending"])
    parser.add_argument("--worker-locality", choices=("none", "roots"), default="none")
    args = parser.parse_args()
    tracer = args.tracer.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.clear()
    os.environ.update({"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C", "PYTHONHASHSEED": "0"})
    work = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    binary, plugin = work / "actors", work / "observer.so"
    report = {"passed": False, "tracer": str(tracer), "tracer_sha256": digest(tracer), "rows": [],
              "scope": "NP1 two-thread fixed-layout seed; parallel2 native DFS/BFS depth1 vs finite explicit actor oracle; no Python search"}
    try:
        builds = [
            ["/usr/bin/gcc", "-std=gnu11", "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", "-Wall", "-Wextra", "-Werror",
             ROOT / "tests/native_search_actors.c", "-o", binary],
            ["/usr/bin/g++", "-std=gnu++20", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
             "-I" + str(ROOT / "third_party"), ROOT / "tests/native_search_plugin.cpp", "-o", plugin],
        ]
        for build in builds:
            result = command(build, ROOT)
            require(result.returncode == 0, result.stdout)
        report.update(binary_sha256=digest(binary), plugin_sha256=digest(plugin), sources={str(path): digest(path) for path in
                      [Path(__file__), ROOT / "tests/native_search_actors.py", ROOT / "tests/native_search_actors.c",
                       ROOT / "tests/native_search_plugin.cpp", ROOT / "examples/_support/session.py"]})
        for mode in args.modes:
            native = command([binary, MODES[mode]], work)
            require(native.returncode == 0 and native.stdout in ("AB", "BA"), "native fixture oracle failed")
            for backend in ("full", "pages"):
                row = run(work, tracer, binary, plugin, mode, backend, args.worker_placement, args.worker_locality)
                report["rows"].append(row)
                print(mode, backend, "PASS" if row["passed"] else row.get("diagnostic", "FAILED"), flush=True)
                (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        report["passed"] = all(row["passed"] for row in report["rows"])
        return 0 if report["passed"] else 1
    finally:
        (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print("RESULT", work / "results.json", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
