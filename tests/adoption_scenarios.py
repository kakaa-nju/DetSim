#!/usr/bin/env python3
"""Real cross-startup thread adoption lifecycle, isolated stores and explicit edges."""
import argparse
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
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from _support.session import Session, ENV, limits

TRACER = ROOT / "tracer"
OUTPUT = None
MANAGED_LAYOUT = False
FIXTURE = r'''
#include <sys/syscall.h>
#include <unistd.h>
_Alignas(16) static unsigned char stacks[2][65536];
extern long spawn(void *);
asm(".text\n.globl spawn\n.type spawn,@function\nspawn:\n"
    "mov %rdi,%rsi\nmov $0x50f00,%edi\nxor %edx,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\n"
    "mov $56,%eax\nsyscall\ntest %rax,%rax\njz 1f\nret\n"
    "1: mov $1,%eax\nmov $1,%edi\nlea letter(%rip),%rsi\nmov $1,%edx\nsyscall\n"
    "mov $60,%eax\nxor %edi,%edi\nsyscall\nud2\n.size spawn,.-spawn\n"
    ".section .rodata\nletter: .ascii \"W\"\n.text\n");
int main(void) {
    if (spawn(stacks[0] + sizeof stacks[0]) < 0) return 1;
    syscall(SYS_write, 1, "A", 1);
    if (spawn(stacks[1] + sizeof stacks[1]) < 0) return 2;
    syscall(SYS_write, 1, "B", 1);
    syscall(SYS_exit, 0);
    __builtin_unreachable();
}
'''


class AdoptionScenarios(unittest.TestCase):
    def scenario(self, backend):
        work = Path(tempfile.mkdtemp(prefix=backend + "-", dir=OUTPUT))
        source = work / "probe.c"
        source.write_text(FIXTURE)
        binary = work / "probe"
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", source, "-o", binary], check=True)
        wrapper = TRACER
        if not MANAGED_LAYOUT:
            wrapper = work / "fixed-tracer"
            wrapper.write_text("#!/bin/sh\nexec /usr/bin/setarch x86_64 -R " + shlex.quote(str(TRACER)) + ' "$@"\n')
            wrapper.chmod(0o755)
        # A shell launcher rewrites PWD to its storage cwd. Managed runs exec the
        # tracer directly, preserving the identical ordered bootstrap environment.
        checkpoints = {}
        caller_personality = int(Path("/proc/self/personality").read_text().strip(), 16)
        session = Session(wrapper, [str(binary)], [], work / "seed", backend, 60,
                          runtime_profile={"fixed_layout": True} if MANAGED_LAYOUT else None,
                          target_cwd=work)

        def step(slot):
            session.send("thread " + str(slot))
            reply = session.send("si 0")
            self.assertNotIn("DetSim state/execution error:", reply)
            return reply

        def capture(name, slot=None):
            identity = session.current_hash()
            self.assertIsNotNone(identity)
            row = {"hash": identity, "threads": session.send("thread"), "slot": slot}
            if slot is not None:
                step(slot)
                row["successor"] = session.current_hash()
                self.assertNotEqual(identity, row["successor"])
                self.assertIn("Loaded state " + identity, session.send("load " + identity))
            checkpoints[name] = row

        donor_pids = []
        try:
            session.send(); session.send("sw 0")
            if MANAGED_LAYOUT:
                tracer_personality = int(Path(f"/proc/{session.proc.pid}/personality").read_text().strip(), 16)
                self.assertEqual(tracer_personality, caller_personality)
                children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
                self.assertEqual(len(children), 1)
                target_personality = int(Path(f"/proc/{children[0]}/personality").read_text().strip(), 16)
                self.assertTrue(target_personality & 0x40000)
                (work / "seed/layout.json").write_text(json.dumps({
                    "managed_layout": True, "caller_personality": caller_personality,
                    "tracer_personality": tracer_personality,
                    "target_personality": target_personality}, indent=2))
            for _ in range(80):
                if re.search(r"\[1\].*state=runnable", session.send("thread")):
                    break
                step(0)
            else:
                self.fail("first child not created")
            capture("two", 0)
            for _ in range(20):
                if re.search(r"\[2\].*state=runnable", session.send("thread")):
                    break
                step(0)
            else:
                self.fail("second child not created")
            capture("three", 2)
            for _ in range(8):
                if "exit(" in step(2):
                    break
            else:
                self.fail("second child did not exit")
            capture("two_after_exit", 0)
            for _ in range(8):
                if "exit(" in step(0):
                    break
            else:
                self.fail("main thread did not exit")
            capture("main_dead", 1)
            for _ in range(8):
                if "exit(" in step(1):
                    break
            else:
                self.fail("last child did not exit")
            capture("all_dead")
            children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
            donor_pids = [session.proc.pid] + [int(pid) for pid in children]
            for pid in children:
                donor_pids += [int(path.name) for path in Path(f"/proc/{pid}/task").iterdir()]
        finally:
            self.assertEqual(session.close(), 0)
        self.assertTrue(donor_pids)
        self.assertFalse([pid for pid in donor_pids if Path(f"/proc/{pid}").exists()])
        (work / "checkpoints.json").write_text(json.dumps(checkpoints, indent=2))
        orders = (
            ("two", "three", "two", "two_after_exit", "main_dead", "all_dead", "two", "three"),
            ("main_dead", "all_dead", "two", "three", "two", "main_dead"),
            ("all_dead", "main_dead", "two", "three", "two_after_exit", "all_dead", "two"),
        )
        for worker, order in enumerate(orders):
            directory = work / ("worker-" + str(worker))
            directory.mkdir()
            for name in ("memory", "sstate"):
                shutil.copytree(work / "seed" / name, directory / name)
            (directory / "memory/.packed.lock").unlink(missing_ok=True)
            shutil.copy2(work / "seed/config.json", directory / "config.json")
            commands = ["sw 0"]
            for name in order:
                row = checkpoints[name]
                commands += ["load --adopt-threads " + row["hash"], "info current"]
                if row["slot"] is not None:
                    commands += ["thread " + str(row["slot"]), "si 0", "info current"]
            (directory / "commands").write_text("\n".join(commands + ["q"]) + "\n")
            transcript = directory / "transcript.log"
            owned = []
            with transcript.open("wb") as log:
                proc = subprocess.Popen([str(wrapper), "-n", "-c", str(directory / "config.json"),
                                         "--target-cwd", str(work)],
                                        cwd=directory, env=os.environ | ENV | {"DETSIM_SNAPSHOT_BACKEND": backend},
                                        stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True, preexec_fn=limits)
                try:
                    proc.stdin.write(("\n".join(commands) + "\n").encode())
                    proc.stdin.flush()
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        content = transcript.read_bytes()
                        if content.endswith(b"(ptmc) ") and content.count(b"(ptmc) ") == len(commands) + 1:
                            break
                        self.assertIsNone(proc.poll(), content.decode(errors="replace")[-5000:])
                        time.sleep(.005)
                    else:
                        self.fail("worker did not complete all commands before cleanup inventory")
                    children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
                    self.assertEqual(len(children), 1)
                    personalities = {}
                    if MANAGED_LAYOUT:
                        personalities = {"caller_personality": caller_personality,
                            "tracer_personality": int(Path(f"/proc/{proc.pid}/personality").read_text().strip(), 16),
                            "target_personality": int(Path(f"/proc/{children[0]}/personality").read_text().strip(), 16)}
                        self.assertEqual(personalities["tracer_personality"], caller_personality)
                        self.assertTrue(personalities["target_personality"] & 0x40000)
                    for pid in [str(proc.pid), *children]:
                        owned += [int(path.name) for path in Path(f"/proc/{pid}/task").iterdir()]
                    carriers = [int(path.name) for path in Path(f"/proc/{children[0]}/task").iterdir()]
                    self.assertEqual(len(carriers), 3)
                    (directory / "owned-before-close.json").write_text(json.dumps(
                        {"tracer": proc.pid, "tracees": list(map(int, children)),
                         "physical_carriers": sorted(carriers), "all_tasks": sorted(owned),
                         "managed_layout": MANAGED_LAYOUT, **personalities}, indent=2))
                finally:
                    try:
                        proc.communicate(b"q\n", timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.communicate()
            result = subprocess.CompletedProcess(proc.args, proc.returncode,
                                                  transcript.read_text(errors="replace"))
            self.assertTrue(owned)
            self.assertFalse([pid for pid in owned if Path(f"/proc/{pid}").exists()])
            self.assertEqual(result.returncode, 0, result.stdout[-5000:])
            self.assertNotIn("DetSim state/execution error:", result.stdout, result.stdout[-5000:])
            loads = re.findall(r"Loaded state ([0-9a-f]{16}) \(adopted multi-thread", result.stdout)
            self.assertEqual(loads, [checkpoints[name]["hash"] for name in order], result.stdout[-5000:])
            chunks = re.split(r"Loaded state [0-9a-f]{16} \(adopted multi-thread[^\n]*\n", result.stdout)[1:]
            self.assertEqual(len(chunks), len(order))
            for name, chunk in zip(order, chunks):
                row = checkpoints[name]
                if row["slot"] is not None:
                    self.assertIn("Switched to thread " + str(row["slot"]) + " ", chunk)
                    edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", chunk)
                    self.assertTrue(edges, chunk)
                    self.assertEqual(edges[-1][1], row["successor"], chunk)
                    self.assertIn((row["hash"], row["successor"]), edges, chunk)

    def test_full(self):
        self.scenario("full")

    def test_pages(self):
        self.scenario("pages")


def main():
    global TRACER, OUTPUT, MANAGED_LAYOUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=TRACER)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--managed-layout", action="store_true",
                        help="use RuntimeProfile.fixed_layout; keep tracer personality unchanged")
    args = parser.parse_args()
    TRACER = args.tracer.resolve()
    MANAGED_LAYOUT = args.managed_layout
    OUTPUT = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="ds-adoption-"))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    print("Adoption evidence: " + str(OUTPUT), flush=True)
    unittest.main(argv=[sys.argv[0]], verbosity=2)


if __name__ == "__main__":
    main()
