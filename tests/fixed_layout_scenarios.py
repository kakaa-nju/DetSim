"""Framework-owned tracee address layout; host ASLR/personality remain unchanged."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from _support.session import Session

TRACER = ROOT / "tracer"


class FixedLayoutScenarios(unittest.TestCase):
    def test_profile_controls_tracees_without_setarch(self):
        host_aslr = Path("/proc/sys/kernel/randomize_va_space").read_text()
        host_personality = Path("/proc/self/personality").read_text()
        with tempfile.TemporaryDirectory(prefix="detsim-fixed-layout-") as temporary:
            root = Path(temporary)
            source = root / "probe.c"
            source.write_text('#include <unistd.h>\nint main(void){return write(1,"x",1)!=1;}\n')
            binary = root / "probe"
            subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                           ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
                            str(source), "-o", str(binary)], check=True)
            layouts = []
            for attempt in range(3):
                session = Session(TRACER, [str(binary)], [], root / str(attempt), "full", 20,
                                  runtime_profile={"fixed_layout": True})
                owned = []
                try:
                    session.send()
                    children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()
                    self.assertEqual(len(children), 1)
                    owned = [session.proc.pid, int(children[0])]
                    self.assertTrue(int(Path(f"/proc/{children[0]}/personality").read_text(), 16) & 0x40000)
                    self.assertEqual(Path(f"/proc/{session.proc.pid}/personality").read_text(), host_personality)
                    layouts.append(Path(f"/proc/{children[0]}/maps").read_text())
                    session.send("sw 0")
                    for _ in range(40):
                        reply = session.send("si 0")
                        self.assertNotIn("DetSim state/execution error:", reply)
                        if "has exited with status 0" in reply:
                            break
                    else:
                        self.fail("fixed-layout guest did not complete")
                finally:
                    self.assertEqual(session.close(), 0)
                self.assertFalse([pid for pid in owned if Path(f"/proc/{pid}").exists()])
            self.assertEqual(layouts, [layouts[0]] * 3)
        self.assertEqual(Path("/proc/sys/kernel/randomize_va_space").read_text(), host_aslr)
        self.assertEqual(Path("/proc/self/personality").read_text(), host_personality)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=TRACER)
    args = parser.parse_args()
    TRACER = args.tracer.resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
