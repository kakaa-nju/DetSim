#!/usr/bin/env python3
"""NP=2 parallel seed-worker scenario: partition root branches across tracers."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TRACER = Path(os.environ.get("DETSIM_TRACER", ROOT / "tracer")).resolve()


class ParallelScenarios(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ds-parallel-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = self.root / "probe.c"
        source.write_text(
            '#include <unistd.h>\n'
            'int main(void) { for (int i=0;i<2;++i)\n'
            '  if (write(1, "p\\n", 2) != 2) return 1;\n'
            'return 0; }\n')
        subprocess.run(
            shlex.split(os.environ.get("CC", "/usr/bin/gcc"))
            + ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
               str(source), "-o", str(self.root / "probe")], check=True)
        self.config = self.root / "config.json"
        self.config.write_text(json.dumps({
            "Nodes": 2, "Loglevel": 4,
            "Tracee": [[str(self.root / "probe")], [str(self.root / "probe")]],
            "Addr": ["192.168.0.1", "192.168.0.2"],
            "Assertions": [], "Plugins": [], "RuntimeProfile": {}, "FSMap": [],
        }))

    def run_parallel(self, fixed):
        commands = self.root / "commands"
        commands.write_text("sw 0\nparallel 2 2\nq\n")
        argv = [str(TRACER), "-n", "-c", str(self.config), "-b", str(commands)]
        if fixed:
            argv = ["/usr/bin/setarch", "x86_64", "-R"] + argv
        return subprocess.run(argv, cwd=self.root, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=60)

    def test_two_workers_partition_and_run_distinct_branches(self):
        result = self.run_parallel(fixed=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Parallel exploration: 2 workers, 2 branches, depth 2",
                      result.stdout)
        self.assertEqual(result.stdout.count("worker ") >= 2, True, result.stdout)
        artifacts = re.search(r"Artifacts: (.*)", result.stdout)
        self.assertIsNotNone(artifacts, result.stdout)
        base = Path(artifacts.group(1).strip())
        self.assertTrue(base.is_dir(), base)
        loaded = []
        for index in range(2):
            output = base / f"worker-{index}" / "worker.out"
            self.assertTrue(output.is_file(), output)
            text = output.read_text(errors="replace")
            self.assertNotIn("DetSim state/execution error:", text)
            self.assertIn("adopted cross-process bindings", text)
            match = re.search(r"Loaded state ([0-9a-f]{16})", text)
            self.assertIsNotNone(match, text[-2000:])
            loaded.append(match.group(1))
        self.assertEqual(len(set(loaded)), 2, loaded)

    def test_normal_aslr_is_rejected_before_spawning(self):
        aslr = Path("/proc/sys/kernel/randomize_va_space")
        if aslr.exists() and aslr.read_text().strip() == "0":
            self.skipTest("host ASLR is disabled")
        result = self.run_parallel(fixed=False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("parallel requires fixed layout", result.stdout)
        self.assertNotIn("Parallel exploration:", result.stdout)


def main():
    global TRACER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=TRACER)
    args = parser.parse_args()
    TRACER = args.tracer.resolve()
    if not TRACER.is_file():
        print(f"missing tracer: {TRACER}", file=sys.stderr)
        return 2
    unittest.main(argv=[sys.argv[0]], verbosity=2)


if __name__ == "__main__":
    raise SystemExit(main())
