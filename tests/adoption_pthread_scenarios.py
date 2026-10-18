#!/usr/bin/env python3
"""Managed-layout imports of real pthread TLS, join and robust-owner checkpoints.

This probes same-checkpoint successor identity. It does not canonicalize physical
IDs already present in guest memory or claim arbitrary pthread scheduling.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from _support.session import Session, require, ENV, limits

FIXTURE = r'''
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
static _Thread_local int tls = 11;
static pthread_mutex_t robust;
static unsigned char alternate[16384];
volatile int recoveries;
static void *owner(void *arg) {
    (void)arg;
    if (tls != 11 || pthread_mutex_lock(&robust)) return (void *)-1;
    tls = 77;
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &mask, 0)) return (void *)-2;
    stack_t stack = {.ss_sp=alternate, .ss_size=sizeof alternate, .ss_flags=0};
    if (sigaltstack(&stack, 0)) return (void *)-3;
    if (write(1, "WORKER_READY\n", 13) != 13) return (void *)-4;
    stack_t observed;
    sigset_t observed_mask;
    if (sigaltstack(0, &observed) || observed.ss_sp != alternate ||
        observed.ss_size != sizeof alternate || observed.ss_flags != 0) return (void *)-5;
    if (pthread_sigmask(SIG_SETMASK, 0, &observed_mask) ||
        sigismember(&observed_mask, SIGUSR1) != 1 || tls != 77) return (void *)-6;
    if (write(1, "WORKER_CHECKED\n", 15) != 15) return (void *)-7;
    return (void *)(intptr_t)tls; /* Deliberate robust owner death. */
}
int main(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
        pthread_mutex_init(&robust, &attr) || pthread_mutexattr_destroy(&attr)) return 2;
    pthread_t thread; void *result = 0;
    if (pthread_create(&thread, 0, owner, 0) || pthread_join(thread, &result)) return 3;
    if ((intptr_t)result != 77 || tls != 11) return 4;
    if (pthread_mutex_lock(&robust) != EOWNERDEAD) return 5;
    ++recoveries;
    if (pthread_mutex_consistent(&robust) || pthread_mutex_unlock(&robust)) return 6;
    return write(1, "PTHREAD_OK\n", 11) != 11;
}
'''


MAIN_OWNER_FIXTURE = r'''
#include <pthread.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
static pthread_mutex_t robust;
static void *waiter(void *arg) {
    (void)arg;
    if (pthread_mutex_lock(&robust) != EOWNERDEAD) return (void *)1;
    if (pthread_mutex_consistent(&robust) || pthread_mutex_unlock(&robust)) return (void *)2;
    if (write(1, "MAIN_OWNER_OK\n", 14) != 14) return (void *)3;
    return 0;
}
int main(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
        pthread_mutex_init(&robust, &attr) || pthread_mutexattr_destroy(&attr) ||
        pthread_mutex_lock(&robust)) return 2;
    pthread_t thread;
    if (pthread_create(&thread, 0, waiter, 0)) return 3;
    write(1, "MAIN_OWNED\n", 11);
    syscall(SYS_exit, 0); /* Kernel robust owner death, while pthread peer remains. */
    __builtin_unreachable();
}
'''


def probe_main_owner(tracer, work, backend):
    source = work / "probe.c"; source.write_text(MAIN_OWNER_FIXTURE)
    binary = work / "probe"
    subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                   ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", "-pthread", source, "-o", binary], check=True)
    native = subprocess.run([binary], capture_output=True, text=True, timeout=10)
    (work / "native.log").write_text(native.stdout + native.stderr)
    require(native.returncode == 0 and native.stdout == "MAIN_OWNED\nMAIN_OWNER_OK\n", "native main-owner oracle failed")
    session = Session(tracer, [str(binary)], [], work / "seed", backend, 90,
                      runtime_profile={"fixed_layout": True}, target_cwd=work)
    record = {"native_exit": native.returncode, "native_output": native.stdout}
    owned = []
    try:
        session.send(); session.send("sw 0")
        for _ in range(100):
            if re.search(r"\[1\].*state=runnable", session.send("thread")):
                break
            session.send("si 0")
        else:
            raise RuntimeError("main-owner worker not created")
        session.send("thread 1")
        for _ in range(40):
            reply = session.send("si 0")
            require("DetSim state/execution error:" not in reply, reply)
            if re.search(r"\[1\].*state=waiting.*futex", session.send("thread")):
                break
        else:
            raise RuntimeError("main-owner peer did not wait")
        record.update(source=session.current_hash(), before=inspect(session))
        session.send("thread 0")
        for _ in range(20):
            record["exit_predecessor"] = session.current_hash()
            reply = session.send("si 0")
            require("DetSim state/execution error:" not in reply, reply)
            if "exit(" in reply:
                record.update(exit_reply=reply, successor=session.current_hash(),
                              after=inspect(session),
                              threads_after=session.send("thread"))
                break
        else:
            raise RuntimeError("main owner did not exit")
        (work / "main-owner.json").write_text(json.dumps(record, indent=2))
        require(re.search(r"\[1\].*state=runnable.*phase=ready", record["threads_after"]),
                "native main-owner mutex recovered, modeled owner exit left waiter Waiting")
        record["checkpoints"] = [{"name": "main_robust_owner_exit", "slot": 0,
            "source": record["exit_predecessor"], "action": "si 0",
            "expected_successor": record["successor"]}]
        session.send("thread 1")
        completion_source = session.current_hash()
        reply = session.send("si 0")
        require("DetSim state/execution error:" not in reply, reply)
        record["checkpoints"].append({"name": "main_owner_ready_wait_completion", "slot": 1,
            "source": completion_source, "action": "si 0",
            "expected_successor": session.current_hash()})
        (work / "main-owner.json").write_text(json.dumps(record, indent=2))
        return record
    finally:
        children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
        for pid in [str(session.proc.pid), *children]:
            owned += [int(path.name) for path in Path(f"/proc/{pid}/task").iterdir()]
        (work / "owned-before-close.json").write_text(json.dumps(owned))
        require(session.close() == 0, "main-owner donor cleanup failed")
        require(not any(Path(f"/proc/{tid}").exists() for tid in owned), "main-owner task leak")


def inspect(session):
    text = session.send("inspect 0")
    return json.JSONDecoder().raw_decode(text[text.index("{"):])[0]


def make_checkpoints(tracer, work, backend):
    binary = work / "probe"
    source = work / "probe.c"
    source.write_text(FIXTURE)
    command = shlex.split(os.environ.get("CC", "/usr/bin/gcc")) + [
        "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", "-pthread", source, "-o", binary]
    subprocess.run(command, check=True)
    native = subprocess.run([binary], capture_output=True, text=True, timeout=10)
    (work / "native.log").write_text(native.stdout + native.stderr)
    require(native.returncode == 0 and native.stdout == "WORKER_READY\nWORKER_CHECKED\nPTHREAD_OK\n", "native pthread oracle failed")
    session = Session(tracer, [str(binary)], [], work / "seed", backend, 90,
                      runtime_profile={"fixed_layout": True, "initial_time_us": 1000000}, target_cwd=work)
    rows = []
    owned = []

    def step(slot):
        session.send("thread " + str(slot))
        reply = session.send("si 0")
        require("DetSim state/execution error:" not in reply, reply)
        require("Reach illegal state" not in reply, reply)
        return reply

    def capture(name, slot):
        identity = session.current_hash()
        row = {"name": name, "source": identity, "slot": slot, "action": "si 0",
               "thread_listing": session.send("thread"), "inspection": inspect(session)}
        step(slot)
        row["expected_successor"] = session.current_hash()
        require(row["expected_successor"] != identity, "checkpoint action made no transition")
        row["reply"] = session.send("info current")
        rows.append(row)
        (work / "checkpoints.json").write_text(json.dumps(rows, indent=2))
        require("Loaded state " + identity in session.send("load " + identity), "donor source unavailable")

    try:
        session.send(); session.send("sw 0")
        for _ in range(100):
            listing = session.send("thread")
            if re.search(r"\[0\].*state=waiting.*futex", listing):
                break
            step(0)
        else:
            raise RuntimeError("main did not reach pending pthread join")
        capture("join_waiting_before_worker", 1)
        for _ in range(40):
            reply = step(1)
            if "WORKER_READY" in reply:
                break
        else:
            raise RuntimeError("worker did not register TLS/mask/altstack/robust state")
        saved_threads = inspect(session)["threads"]
        live = saved_threads[1]
        require(live["registers"]["fs_base"] != saved_threads[0]["registers"]["fs_base"],
                "pthread worker TLS base is not distinct from main")
        require(live["signal_mask"] & (1 << 9) and live["robust_list_head"] and live["clear_child_tid"],
                "worker registration evidence missing")
        capture("worker_registered_owner", 1)
        for _ in range(12):
            if "WORKER_CHECKED" in step(1):
                break
        else:
            raise RuntimeError("worker did not verify saved TLS/mask/altstack values")
        capture("worker_queries_verified", 1)
        for _ in range(20):
            if "exit(" in step(1):
                break
        else:
            raise RuntimeError("worker did not exit")
        listing = session.send("thread")
        require(re.search(r"\[0\].*phase=ready", listing), "join waiter did not become Ready: " + listing)
        capture("join_ready_after_owner_exit", 0)
        for _ in range(40):
            if "exit_group(" in step(0):
                break
        else:
            raise RuntimeError("main did not finish owner-death recovery")
        require("recoveries = 1" in session.send("p recoveries"), "missing owner-death recovery")
        require("has exited with status 0" in session.send("si 0"), "pthread fixture did not exit zero")
        children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
        for pid in [str(session.proc.pid), *children]:
            owned += [int(path.name) for path in Path(f"/proc/{pid}/task").iterdir()]
        (work / "seed/owned-before-close.json").write_text(json.dumps(owned))
    finally:
        require(session.close() == 0, "donor tracer cleanup failed")
    require(owned and not any(Path(f"/proc/{tid}").exists() for tid in owned), "donor task leak")
    return rows


def import_worker(tracer, work, backend, index, checkpoints):
    directory = work / ("worker-" + str(index))
    directory.mkdir()
    for name in ("memory", "sstate"):
        shutil.copytree(work / "seed" / name, directory / name)
    (directory / "memory/.packed.lock").unlink(missing_ok=True)
    shutil.copy2(work / "seed/config.json", directory / "config.json")
    order = [*checkpoints, *reversed(checkpoints)]
    commands = ["sw 0"]
    for row in order:
        commands += ["load --adopt-threads " + row["source"], "thread " + str(row["slot"]),
                     row["action"], "info current"]
    (directory / "commands").write_text("\n".join(commands + ["q"]) + "\n")
    transcript = directory / "transcript.log"
    owned = []
    with transcript.open("wb") as log:
        proc = subprocess.Popen([str(tracer), "-n", "-c", str(directory / "config.json"),
                                 "--target-cwd", str(work)],
                                cwd=directory, stdin=subprocess.PIPE, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True,
                                env=os.environ | ENV | {"DETSIM_SNAPSHOT_BACKEND": backend}, preexec_fn=limits)
        try:
            proc.stdin.write(("\n".join(commands) + "\n").encode()); proc.stdin.flush()
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                content = transcript.read_bytes()
                require(proc.poll() is None, content.decode(errors="replace")[-6000:])
                if content.endswith(b"(ptmc) ") and content.count(b"(ptmc) ") == len(commands) + 1:
                    break
                time.sleep(.005)
            else:
                raise TimeoutError("worker command deadline")
            children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
            for pid in [str(proc.pid), *children]:
                owned += [int(path.name) for path in Path(f"/proc/{pid}/task").iterdir()]
            (directory / "owned-before-close.json").write_text(json.dumps(owned))
        finally:
            try:
                proc.communicate(b"q\n", timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL); proc.communicate()
    require(proc.returncode == 0, "worker cleanup failed")
    require(owned and not any(Path(f"/proc/{tid}").exists() for tid in owned), "worker task leak")
    chunks = re.split(r"Loaded state [0-9a-f]{16} \(adopted multi-thread[^\n]*\n", transcript.read_text())[1:]
    require(len(chunks) == len(order), "not all checkpoints imported")
    results = []
    for row, chunk in zip(order, chunks):
        edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", chunk)
        observed = edges[-1][1] if edges else None
        receipt = {"checkpoint": row["name"], "source": row["source"], "slot": row["slot"],
                   "action": row["action"], "expected": row["expected_successor"], "observed": observed,
                   "passed": bool(edges) and edges[-1] == (row["source"], row["expected_successor"])}
        results.append(receipt)
        (directory / "results.json").write_text(json.dumps(results, indent=2))
        require(receipt["passed"], "imported pthread successor differs: " + json.dumps(receipt))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/parallel-redesign/adoption-pthread")
    parser.add_argument("--backend", choices=("full", "pages", "both"), default="both")
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--main-owner", action="store_true", help="probe cached-main-TID robust owner death separately")
    args = parser.parse_args()
    require(args.workers > 0, "positive worker count required")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"tracer_sha256": hashlib.sha256(args.tracer.read_bytes()).hexdigest(),
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "fixture_sha256": hashlib.sha256((MAIN_OWNER_FIXTURE if args.main_owner else FIXTURE).encode()).hexdigest(),
              "rows": [], "passed": False}
    try:
        for backend in (("full", "pages") if args.backend == "both" else (args.backend,)):
            work = Path(tempfile.mkdtemp(prefix=backend + "-", dir=args.output.resolve()))
            row = {"backend": backend, "artifacts": str(work), "workers": []}
            report["rows"].append(row)
            (work / "runner-source.py").write_bytes(Path(__file__).read_bytes())
            if args.main_owner:
                row["main_owner"] = probe_main_owner(args.tracer.resolve(), work, backend)
                checkpoints = row["main_owner"]["checkpoints"]
            else:
                checkpoints = make_checkpoints(args.tracer.resolve(), work, backend)
            for index in range(args.workers):
                row["workers"].append(import_worker(args.tracer.resolve(), work, backend, index, checkpoints))
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    (args.output / "results.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
