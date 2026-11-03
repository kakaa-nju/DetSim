#!/usr/bin/env python3
"""Build deliberately mutated Willemt Raft cases for the examples/raft checker.

The real case (examples/raft/raft.json) is supposed to be *clean*, so running
it only proves the safety checker does not fire on healthy behaviour. This
script produces the complementary evidence: copies of the Raft library with a
known protocol bug injected, a static case binary built from those copies, and
a tracer config that points at it. Running such a config must make
plugins/check.cpp report the matching property violation.

The library sources and the checked-in tree are never modified: every variant
gets its own copy under examples/raft/mutants/<variant>/library, and only the
copy is patched.

Usage (paths are relative to examples/raft):

    ./mutants.py --list
    ./mutants.py --build                 # build every variant
    ./mutants.py --build vote-reply-ignored
    ./mutants.py --verify                # build, replay each variant's
                                         # drive.cmds batch and assert the
                                         # expected "[CHECK FAILED]" diagnostic
    ./mutants.py --verify --search       # bounded BFS instead of the batch
    ./mutants.py --verify --seconds 60 --tracer ../../tracer

The mutation is the only difference in the compiled case: the driver is a plain
three-node server, and the execution sequence lives entirely in the generated
`drive.cmds` batch. Replaying that batch must make the checker report the
injected bug on the mutant and stay silent on the unmutated control; the script
exits non-zero if either half fails.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIBRARY = HERE / "raft"
OUTPUT = HERE / "mutants"
SOURCES = (
    "src/raft_server.c",
    "src/raft_server_properties.c",
    "src/raft_node.c",
    "src/raft_log.c",
)
CC = os.environ.get("CC", "cc")
# Minimal driver: the realistic raft/main.c loop buries elections too deep.
DRIVER = HERE / "mutant_fixture.c"


@dataclass(frozen=True)
class Mutation:
    """One single-site source edit and the diagnostic it must provoke."""

    name: str
    summary: str
    path: str
    before: str
    after: str
    expected: str | tuple[str, ...] | None  # None marks the unmutated control case


# The first entry reproduces the injection used by upstream DetSim: the reply
# switch dispatches on the error enum instead of the reported vote, so a node
# counts a vote for every reply it receives.
MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        name="vote-reply-ignored",
        summary="treat every RequestVote reply as a granted vote",
        path="src/raft_server.c",
        before="switch (r->vote_granted)",
        after="switch (RAFT_REQUESTVOTE_ERR_GRANTED)",
        # Two nodes win the same term, so the checker may reach either face of
        # the "one leader per term" property first: the simultaneous one
        # ("multiple leaders in term 1") when the split brain is visible in a
        # single state, or the takeover one ("term 1 was led by node 1, now
        # node 0 claims it") when the second claimant arrives after the first
        # one moved on. Both are the injected bug, so accept either.
        expected=("Multiple leaders in term", "one-leader-per-term property"),
    ),
    Mutation(
        name="healthy",
        summary="unmutated control build (must not trip the checker)",
        path="src/raft_server.c",
        before="",
        after="",
        expected=None,
    ),
)

def raft_drive() -> tuple[tuple[int, int], ...]:
    """Syscall interleaving that exposes the vote path, as (node, choice) steps.

    Each node runs: socket, bind, then a loop of gettimeofday and recvfrom, with
    one sendto per outgoing message. The sequence below binds all sockets first,
    makes nodes 0 and 1 time out into term 1, lets node 2 vote for the first
    candidate and deny the second, and finally lets each candidate read its
    reply. A correct library keeps exactly one leader; the injected vote bug
    elects both in term 1. The last three steps give the tracer a following
    transition, because the check observes the state that is about to run.
    """
    steps: list[tuple[int, int]] = []
    for node in (0, 1, 2):
        steps += [(node, 0), (node, 0)]          # socket, bind
    for node in (0, 1):
        steps += [(node, 0), (node, 0)]          # gettimeofday, empty recvfrom
    for node in (0, 1):
        steps += [(node, 1), (node, 0), (node, 0)]  # time out, two requestvotes
    for _ in range(2):
        steps += [(2, 0), (2, 0), (2, 0)]        # grant, then deny
    steps += [(1, 0)] * 5                        # denied reply, then election
    steps += [(0, 0)] * 5                        # denial, then its own grant
    steps += [(1, 0)]                            # the check observes this state
    return tuple(steps)


BY_NAME = {mutation.name: mutation for mutation in MUTATIONS}

# State dump appended to every generated batch. `state` is 1 follower /
# 2 candidate / 3 leader, and `current_leader` is the leader node pointer a
# node believes in (0 when unknown), so the three blocks show whether the
# nodes agree on the leader and the term.
DRIVE_REPORT = (
    # Emitted per node, in this order:
    #   state        1 follower, 2 candidate, 3 leader
    #   current_term elected term of that node
    #   voted_for    node id this node voted for (-1 when none)
    #   commit_idx   highest committed log index
    #   <flag>       1 when the node knows a current leader, 0 otherwise
    #                (the CLI prints this comparison bare, without a label)
    "p ((raft_server_private_t*)raft)->state",
    "p ((raft_server_private_t*)raft)->current_term",
    "p ((raft_server_private_t*)raft)->voted_for",
    "p ((raft_server_private_t*)raft)->commit_idx",
    "p ((raft_server_private_t*)raft)->current_leader != 0",
)


def drive_commands() -> str:
    """The recorded syscall interleaving, then a per-node state dump."""
    text = "".join("sw {}\nsi {}\n".format(node, choice)
                   for node, choice in raft_drive())
    for node in range(3):
        text += "sw {}\n".format(node)
        text += "".join(line + "\n" for line in DRIVE_REPORT)
    return text


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_library(destination: Path) -> dict[str, str]:
    """Copy only the library inputs, never vendor objects or git metadata."""
    inputs: dict[str, str] = {}
    for subdir in ("include", "src"):
        target = destination / subdir
        target.mkdir(parents=True, exist_ok=True)
        for source in sorted((LIBRARY / subdir).iterdir()):
            if source.suffix not in (".c", ".h"):
                continue
            shutil.copy2(source, target / source.name)
            inputs[str(source.relative_to(LIBRARY))] = digest(source)
    return inputs


def compile_case(workdir: Path, library: Path) -> list[str]:
    argv = [
        CC,
        "-std=gnu2x",
        "-fno-stack-protector",
        "-gdwarf-4",
        "-g",
        "-O0",
        "-fno-omit-frame-pointer",
        "-fno-common",
        "-fsigned-char",
        "-D_FORTIFY_SOURCE=0",
        "-I" + str(library / "include"),
        str(DRIVER),
        *[str(library / source) for source in SOURCES],
        "-static",
        "-o",
        str(workdir / "raft"),
    ]
    result = subprocess.run(argv, cwd=workdir, capture_output=True, text=True)
    (workdir / "build.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        raise SystemExit(
            "build failed for {} (see {}):\n{}".format(
                workdir.name, workdir / "build.log", result.stderr[-2000:]
            )
        )
    return argv


def apply_mutation(mutation: Mutation, library: Path) -> str | None:
    if mutation.expected is None:
        return None
    target = library / mutation.path
    original = target.read_text()
    occurrences = original.count(mutation.before)
    if occurrences != 1:
        raise SystemExit(
            "{}: expected exactly one {!r} in {}, found {}".format(
                mutation.name, mutation.before, mutation.path, occurrences
            )
        )
    target.write_text(original.replace(mutation.before, mutation.after))
    return digest(target)


def case_config(mutation: Mutation) -> dict:
    """Config for the tracer, mirroring raft.json with the mutant tracee.

    Paths inside a DetSim config resolve against the config file's directory
    and plugin paths must not contain "..", so every variant is self-contained:
    it owns its binary, its config and copies of the two plugins.
    """
    base = json.loads((HERE / "raft.json").read_text())
    base["Tracee"] = [["./raft", str(node)] for node in range(3)]
    base["ChooseFunc"] = "./choose.so"
    base["UserCheck"] = "./check.so"
    base["UserState"] = "./check.so"
    return base


def build(mutation: Mutation) -> Path:
    workdir = OUTPUT / mutation.name
    library = workdir / "library"
    if workdir.exists():
        shutil.rmtree(workdir)
    library.mkdir(parents=True)

    inputs = copy_library(library)
    mutated_hash = apply_mutation(mutation, library)
    argv = compile_case(workdir, library)
    plugins = {}
    for name in ("check.so", "choose.so"):
        source = HERE / "plugins" / name
        if not source.exists():
            raise SystemExit(
                "{} is missing; run 'make -C {} plugins' first".format(source, HERE)
            )
        shutil.copy2(source, workdir / name)
        plugins[name] = digest(source)

    config_path = workdir / "config.json"
    config_path.write_text(json.dumps(case_config(mutation), indent=2) + "\n")

    commands = workdir / "drive.cmds"
    commands.write_text(drive_commands())

    manifest = {
        "variant": mutation.name,
        "summary": mutation.summary,
        "library": "willemt/raft",
        "source_root": str(LIBRARY),
        "inputs_sha256": inputs,
        "mutation": None
        if mutation.expected is None
        else {
            "file": mutation.path,
            "before": mutation.before,
            "after": mutation.after,
            "occurrences": 1,
            "mutated_source_sha256": mutated_hash,
        },
        "expected_check": mutation.expected,
        "plugins_sha256": plugins,
        "build_command": argv,
        "binary_sha256": digest(workdir / "raft"),
        "config": config_path.relative_to(HERE).as_posix(),
    }
    (workdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("built {} -> {}".format(mutation.name, workdir / "raft"))
    print("      config {}".format(config_path.relative_to(HERE)))
    return workdir


def write_index(built: list[str]) -> None:
    entries = []
    for name in built:
        manifest = json.loads((OUTPUT / name / "manifest.json").read_text())
        entries.append(
            {
                "variant": name,
                "summary": manifest["summary"],
                "expected_check": manifest["expected_check"],
                "binary_sha256": manifest["binary_sha256"],
                "config": manifest["config"],
            }
        )
    (OUTPUT / "index.json").write_text(json.dumps(entries, indent=2) + "\n")


def clean_runtime() -> None:
    for name in ("sstate", "memory", "tstate", "mappings", "filesystem", ".tracer.lock"):
        path = HERE / name
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        elif path.exists():
            path.unlink()


def verify(mutation: Mutation, tracer: Path, seconds: int, search: bool) -> bool:
    """Replay the recorded batch (or a bounded search) and judge the outcome."""
    if not (HERE / "plugins/check.so").exists():
        raise SystemExit(
            "plugins/check.so is missing; run 'make -C {} plugins' first".format(HERE)
        )
    workdir = OUTPUT / mutation.name
    config = workdir / "config.json"
    if search:
        commands = workdir / "search.cmds"
        commands.write_text("bfs --quiet\n")
        log = workdir / "search.log"
    else:
        commands = workdir / "drive.cmds"
        log = workdir / "drive.log"
    clean_runtime()
    argv = [
        "timeout", "-s", "INT", str(seconds), str(tracer),
        "-c", str(config.relative_to(HERE)), "-n",
        "-b", str(commands),
    ]
    result = subprocess.run(argv, cwd=HERE, capture_output=True, text=True)
    output = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout + result.stderr)
    log.write_text(output)

    failed = "[CHECK FAILED]" in output
    # A batch that keeps stepping after the violation hits the terminal guard.
    aborted = "state/execution error" in output
    wanted: tuple[str, ...] = ()
    if mutation.expected is not None:
        wanted = (
            (mutation.expected,)
            if isinstance(mutation.expected, str)
            else tuple(mutation.expected)
        )
    if mutation.expected is None:
        # The control build must run the same batch without a false positive.
        ok = not failed
        detail = "no violation on the same batch" if ok else "unexpected violation"
    else:
        ok = failed and any(w in output for w in wanted) and not aborted
        detail = "expected " + " or ".join(repr(w) for w in wanted)
        if failed and not ok:
            detail += ", got: " + "; ".join(
                line.strip() for line in output.splitlines() if "CHECK FAILED" in line
            )
        elif not failed:
            detail += ", no violation reported"
            if search:
                detail += " (bounded search; raise --seconds to explore further)"
        if aborted:
            detail += ", run aborted after the violation"
    print("[{}] {}  ({})".format("PASS" if ok else "FAIL", mutation.name, detail))
    if not ok:
        print("      log: {}".format(log.relative_to(HERE)))
    return ok


def selected(names: list[str]) -> list[Mutation]:
    if not names:
        return list(MUTATIONS)
    unknown = [name for name in names if name not in BY_NAME]
    if unknown:
        raise SystemExit(
            "unknown variant(s): {} (known: {})".format(
                ", ".join(unknown), ", ".join(BY_NAME)
            )
        )
    return [BY_NAME[name] for name in names]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build mutated Raft cases to test the examples/raft checker."
    )
    parser.add_argument("--list", action="store_true", help="list known variants")
    parser.add_argument(
        "--build", nargs="*", metavar="VARIANT", help="build these variants (default: all)"
    )
    parser.add_argument(
        "--verify",
        nargs="*",
        metavar="VARIANT",
        help="build and run these variants, asserting the checker diagnostic",
    )
    parser.add_argument(
        "--seconds", type=int, default=60, help="timeout per variant run (default 60)"
    )
    parser.add_argument(
        "--search",
        action="store_true",
        help="run a bounded BFS instead of replaying the recorded batch",
    )
    parser.add_argument(
        "--tracer",
        default="../../tracer",
        help="tracer executable, relative to examples/raft (default ../../tracer)",
    )
    args = parser.parse_args()

    if args.list:
        for mutation in MUTATIONS:
            if mutation.expected is None:
                mark = "control"
            elif isinstance(mutation.expected, str):
                mark = "expects " + mutation.expected
            else:
                mark = "expects " + " or ".join(mutation.expected)
            print("{:26} {:48} {}".format(mutation.name, mutation.summary, mark))
        return 0

    OUTPUT.mkdir(exist_ok=True)
    tracer = (HERE / args.tracer).resolve()

    if args.verify is not None:
        variants = selected(args.verify)
        built = [mutation.name for mutation in variants]
        for mutation in variants:
            build(mutation)
        write_index(built)
        if not tracer.exists():
            raise SystemExit(
                "tracer not found: {} (run 'make NP=3' in the repository root)".format(tracer)
            )
        results = [verify(mutation, tracer, args.seconds, args.search) for mutation in variants]
        print(
            "{} of {} variants behaved as expected".format(sum(results), len(results))
        )
        return 0 if all(results) else 1

    variants = selected(args.build or [])
    built = [mutation.name for mutation in variants]
    for mutation in variants:
        build(mutation)
    write_index(built)
    print("{} variant(s) built under {}".format(len(built), OUTPUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
