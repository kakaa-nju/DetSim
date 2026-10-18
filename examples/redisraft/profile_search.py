#!/usr/bin/env python3
"""Record perf stacks around each native command in parallel_benchmark.

Requires an NP=3 tracer and working perf; setup/prefix are never sampled.
The benchmark still validates equivalent graphs. Profiling changes timings.
"""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import runpy
import subprocess
import sys
import time

import parallel_benchmark as benchmark


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--perf', type=Path, required=True)
    parser.add_argument('--sudo', action='store_true')
    parser.add_argument('--frequency', type=int, default=499)
    parser.add_argument('--syscalls', action='store_true', help='record syscall enter/exit events instead of CPU stacks')
    parser.add_argument('--driver', type=Path, help='another case entrypoint using _support.session.Session')
    args, rest = parser.parse_known_args()
    if not 1 <= args.frequency <= 1000:
        parser.error('frequency must be 1..1000')
    prefix = ['sudo', '-n'] if args.sudo else []
    original = benchmark.common.Session

    class ProfileSession(original):
        profile_index = 0

        def send(self, text=None):
            if not text or not text.startswith(('dfs ', 'parallel ')):
                return super().send(text)
            self.profile_index += 1
            directory = self.directory / f'perf-{self.profile_index}'
            directory.mkdir()
            control, ack = directory / 'control', directory / 'ack'
            os.mkfifo(control, 0o600)
            os.mkfifo(ack, 0o600)
            ctl = os.open(control, os.O_RDWR | os.O_NONBLOCK)
            ackfd = os.open(ack, os.O_RDWR | os.O_NONBLOCK)
            children = Path(f'/proc/{self.proc.pid}/task/{self.proc.pid}/children').read_text().split()
            metadata = {'command': text, 'coordinator_pid': self.proc.pid,
                        'initial_tracees': list(map(int, children)), 'passed': False,
                        'scope': 'CPU samples, all tracer threads, current tracees and inherited future workers/tracees'}
            data = directory / 'perf.data'
            events = ['-e', 'cpu-clock', '-F', str(args.frequency), '--call-graph', 'dwarf,16384']
            if args.syscalls:
                names = ('sendto', 'recvfrom', 'fsync', 'fdatasync', 'poll', 'process_vm_readv', 'process_vm_writev')
                events = ['-e', ','.join(f'syscalls:sys_{phase}_{name}' for name in names for phase in ('enter', 'exit'))]
                metadata['scope'] = 'selected syscall counts and elapsed latency; concurrent task durations overlap'
            invocation = prefix + [str(args.perf.resolve()), 'record', *events,
                '--clockid', 'mono',
                '-D', '-1', '--control', f'fifo:{control},{ack}', '-p',
                ','.join([str(self.proc.pid), *children]), '-o', str(data)]
            metadata['argv'] = invocation
            profiler = None

            def toggle(command):
                os.write(ctl, (command + '\n').encode())
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if profiler.poll() is not None:
                        raise RuntimeError('perf exited: ' + (directory / 'perf.log').read_text())
                    if select.select([ackfd], [], [], .1)[0]:
                        if b'ack' in os.read(ackfd, 4096):
                            return
                raise TimeoutError('perf control acknowledgement: ' + command)

            try:
                with (directory / 'perf.log').open('wb') as log:
                    profiler = subprocess.Popen(invocation, stdout=log, stderr=subprocess.STDOUT,
                                                start_new_session=True)
                    toggle('enable')
                    metadata['begin'] = time.monotonic()
                    try:
                        reply = super().send(text)
                    finally:
                        metadata['end'] = time.monotonic()
                        toggle('disable')
                    (directory / 'command.log').write_text(reply)
                    metadata['passed'] = True
                    return reply
            finally:
                if profiler is not None and profiler.poll() is None:
                    if args.sudo:
                        subprocess.run(['sudo', '-n', 'kill', '-INT', str(profiler.pid)], check=True)
                    else:
                        profiler.send_signal(signal.SIGINT)
                    try:
                        code = profiler.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        subprocess.run(prefix + ['kill', '-KILL', str(profiler.pid)], check=True)
                        profiler.wait()
                        raise
                    if code not in (0, 130, -signal.SIGINT):
                        raise RuntimeError(f'perf failed: {code}')
                os.close(ctl)
                os.close(ackfd)
                control.unlink()
                ack.unlink()
                if args.sudo and data.exists():
                    subprocess.run(['sudo', '-n', 'chown', f'{os.getuid()}:{os.getgid()}', str(data)], check=True)
                (directory / 'profile.json').write_text(json.dumps(metadata, indent=2))

    benchmark.common.Session = ProfileSession
    sys.argv = [sys.argv[0], *rest]
    try:
        if args.driver:
            runpy.run_path(str(args.driver.resolve()), run_name='__main__')
            return 0
        return benchmark.main()
    finally:
        benchmark.common.Session = original


if __name__ == '__main__':
    raise SystemExit(main())
