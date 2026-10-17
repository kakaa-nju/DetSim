#!/usr/bin/env python3
"""Headless PTY smoke for the ncurses UI: startup, commands, resize, small terms."""
import argparse
import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRACER = ROOT / "tracer"


def drain(master, output, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        readable, _, _ = select.select([master], [], [], 0.05)
        if not readable:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            return
        if not chunk:
            return
        output.extend(chunk)


class TuiSmokeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="detsim-tui-")
        root = Path(cls.temp.name)
        source = root / "probe.c"
        source.write_text(
            "#include <unistd.h>\n"
            "int main(void) { return write(1, \"TUI_OK\\n\", 7) != 7; }\n")
        subprocess.run(
            ["/usr/bin/gcc", "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
             str(source), "-o", str(root / "probe")], check=True)
        (root / "config.json").write_text(json.dumps({
            "Nodes": 1, "Loglevel": 4,
            "Tracee": [[str(root / "probe")]],
            "Addr": ["192.168.0.1"], "Assertions": [], "Plugins": [],
            "RuntimeProfile": {}, "FSMap": []}))
        cls.root = root

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_tui(self, rows, cols, keys, resize=None):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        env = os.environ.copy()
        env.update({"TERM": "xterm-256color", "LC_ALL": "C", "LANG": "C"})
        process = subprocess.Popen(
            [str(TRACER), "-c", str(self.root / "config.json")],
            cwd=self.root, stdin=slave, stdout=slave, stderr=slave,
            env=env, start_new_session=True, close_fds=True)
        os.close(slave)
        output = bytearray()
        try:
            drain(master, output, 0.8)
            if resize:
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", resize[0], resize[1], 0, 0))
                try:
                    os.kill(process.pid, signal.SIGWINCH)
                except ProcessLookupError:
                    pass
                drain(master, output, 0.5)
            for key in keys:
                try:
                    os.write(master, key.encode() + b"\n")
                except OSError:
                    break
                drain(master, output, 0.3)
            drain(master, output, 0.8)
            try:
                code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                self.fail("TUI did not exit\n" + output.decode("utf-8", "replace")[-2000:])
            self.assertEqual(code, 0, output.decode("utf-8", "replace")[-2000:])
            return bytes(output)
        finally:
            try:
                os.close(master)
            except OSError:
                pass
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()

    def test_help_pending_thread_and_quit(self):
        output = self.run_tui(24, 80, ("help", "pending", "inspect", "thread", "q"))
        text = output.decode("utf-8", "replace")
        self.assertIn("pending - List blocked/ready syscalls", text)
        self.assertIn("Runnable actors:", text)
        self.assertNotIn("Unknown command 'pending'", text)
        self.assertNotIn("Unknown command 'inspect'", text)

    def test_small_terminal_starts_and_quits(self):
        output = self.run_tui(10, 40, ("help", "q"))
        text = output.decode("utf-8", "replace")
        self.assertNotIn("Segmentation fault", text)
        self.assertNotIn("Aborted", text)
        self.assertIn("pending - List blocked/ready syscalls", text)

    def test_resize_to_small_terminal_then_command(self):
        output = self.run_tui(24, 80, ("pending", "q"), resize=(8, 20))
        text = output.decode("utf-8", "replace")
        self.assertIn("Pending syscalls:", text)
        self.assertNotIn("Segmentation fault", text)


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
