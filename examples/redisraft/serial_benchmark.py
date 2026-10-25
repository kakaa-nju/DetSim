#!/usr/bin/env python3
"""Time native single-instance BFS; Python only prepares and observes the run.

A time-limited run is interrupted, never described as an exhaustive proof.
The CLI's visits/s and newly discovered identities/s are reported separately.
"""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from _support.fixture_profile import startup_boundary
from redisraft import parallel_benchmark as setup
from redisraft import run as raft_case


class SavedStateSamples:
    """Read only newly appended metadata; interval timing is host-side diagnostic."""
    def __init__(self, work, interval):
        self.path = work / 'session/sstate/packed.dat'
        self.output = work / 'saved-state-samples.jsonl'
        self.interval = interval
        self.offset = self.count = 0
        self.stop = threading.Event()
        self.error = None
        self.started = time.monotonic()
        self.sample()
        self.initial_count = self.count
        self.thread = threading.Thread(target=self.run)

    def sample(self):
        with self.path.open('rb') as source:
            source.seek(self.offset)
            data = source.read()
        cursor = 0
        while len(data) - cursor >= 20:
            magic, key, size, _ = struct.unpack_from('<IQII', data, cursor)
            if magic != 0x53534545 or not key:
                raise RuntimeError('invalid sampled metadata header')
            if len(data) - cursor < 20 + size:
                break  # append may still be in progress; retry from this offset
            cursor += 20 + size
            self.count += 1
        self.offset += cursor
        with self.output.open('a') as destination:
            destination.write(json.dumps(dict(elapsed_s=time.monotonic() - self.started,
                saved_states=self.count, parsed_bytes=self.offset)) + '\n')

    def run(self):
        try:
            while not self.stop.wait(self.interval):
                self.sample()
        except Exception as error:
            self.error = error

    def close(self):
        self.stop.set()
        self.thread.join()
        if self.error:
            raise self.error
        self.sample()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tracer', type=Path, default=common.ROOT / 'tracer')
    parser.add_argument('--plugin-dir', type=Path,
                        default=common.ROOT / 'build/release/np3/examples/redisraft')
    parser.add_argument('--output', type=Path, default=common.ROOT / 'build/serial-bfs-700')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--backend', choices=('full', 'pages'), default='pages')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--rounds', type=int, default=100)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--cpu', type=int,
                        help='pin tracer and its children to this allowed CPU; leave driver unpinned')
    parser.add_argument('--perf', type=Path, help='optional perf executable for CPU sampling')
    parser.add_argument('--perf-stack-size', type=int, default=8192,
                        help='bytes of user stack per perf DWARF sample (8..65528, multiple of 8)')
    parser.add_argument('--perf-sudo', action='store_true', help='run only perf through sudo -n')
    parser.add_argument('--sample-interval', type=float, default=0,
                        help='optional seconds between read-only saved-state count samples')
    args = parser.parse_args()
    if args.seconds <= 0 or args.rounds < 0 or args.sample_interval < 0:
        parser.error('seconds must be positive and rounds nonnegative')
    if not 8 <= args.perf_stack_size <= 65528 or args.perf_stack_size % 8:
        parser.error('perf-stack-size must be a multiple of 8 in 8..65528')
    driver_affinity = os.sched_getaffinity(0)
    if args.cpu is not None and args.cpu not in driver_affinity:
        parser.error('cpu must belong to the current allowed CPU set')
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=args.output)).resolve()
    print(work, flush=True)
    (work / 'driver-source.py').write_bytes(Path(__file__).read_bytes())
    if args.binary is None:
        owned, _ = raft_case.build_variant(work / 'target', 'healthy', ['/usr/bin/gcc'])
        args.binary = owned / 'raft-case'
    binary, tracer = args.binary.resolve(), args.tracer.resolve()
    plugins = [(args.plugin_dir / name).resolve() for name in ('choose.so', 'check.so')]
    shim = (args.plugin_dir / 'seed.so').resolve()
    report = dict(host=platform.uname()._asdict(), backend=args.backend,
                  seed=args.seed, rounds=args.rounds, requested_seconds=args.seconds,
                  inputs_sha256={str(p): common.digest(p) for p in
                                 [binary, tracer, *plugins, shim, Path(__file__)]})
    report['cpu_affinity'] = sorted(os.sched_getaffinity(0))
    report['tracer_cpu_affinity'] = [args.cpu] if args.cpu is not None else sorted(driver_affinity)
    report['cpu_model'] = next((line.split(':', 1)[1].strip()
        for line in Path('/proc/cpuinfo').read_text().splitlines()
        if line.startswith('model name')), 'unknown')
    report['checkout_commit'] = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=common.ROOT, text=True).strip()
    report['execution_options'] = {name: os.environ.get(name) for name in (
        'DETSIM_RESIDENT_MEMORY', 'DETSIM_CAPTURE_MODE', 'DETSIM_CAPTURE_VERIFY',
        'DETSIM_SOFT_DIRTY', 'DETSIM_OBSERVATION_CACHE', 'DETSIM_OBSERVATION_CACHE_STATS',
        'DETSIM_CAPTURE_LAYOUT_CACHE')}
    (work / 'inputs.json').write_text(json.dumps(report, indent=2))
    old_preload, old_seed = os.environ.get('LD_PRELOAD'), os.environ.get('DETSIM_BENCH_SEED')
    old_limit = common.MODEL_ADDRESS_SPACE_LIMIT
    try:
        os.environ['LD_PRELOAD'] = str(shim) + (':' + old_preload if old_preload else '')
        os.environ['DETSIM_BENCH_SEED'] = str(args.seed)
        common.MODEL_ADDRESS_SPACE_LIMIT = 3 * 1024**3
        if args.cpu is not None:
            os.sched_setaffinity(0, {args.cpu})
        session = common.Session(tracer, [str(binary), '0'], plugins, work / 'session',
            args.backend, args.seconds + 180, choices=True,
            tracees=[[str(binary), str(node)] for node in range(3)],
            addresses=[f'192.168.0.{node + 1}' for node in range(3)],
            runtime_profile={'fixed_layout': True, 'startup_syscall': startup_boundary(binary)},
            target_cwd=work)
    finally:
        if args.cpu is not None:
            os.sched_setaffinity(0, driver_affinity)
        common.MODEL_ADDRESS_SPACE_LIMIT = old_limit
        for name, value in [('LD_PRELOAD', old_preload), ('DETSIM_BENCH_SEED', old_seed)]:
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
    timer = None
    profiler = None
    perf_log = None
    samples = None
    completed = False
    def stop_profiler():
        if profiler and profiler.poll() is None:
            profiler.send_signal(signal.SIGINT)
            profiler.wait(timeout=15)
    try:
        setup.checked(session)
        last = [''] * 3
        for _ in range(args.rounds):
            for node in range(3):
                setup.checked(session, f'sw {node}')
                advance = node == 0 and ('bind(' in last[node] or
                    ('recvfrom(' in last[node] and '= -11' in last[node]))
                last[node] = setup.checked(session, 'si ' + ('1' if advance else '0'))
                common.require('has exited' not in last[node], 'prefix guest exited')
        report['prefix_nodes'] = setup.observe(session)
        if args.rounds >= 100:
            common.require(sum(n['state'] == 4 for n in report['prefix_nodes']) == 1,
                           'prefix did not establish one leader')
        report['root'] = session.current_hash()
        report['root_budget'] = raft_case.saved_budget(session.directory, report['root'])
        before = setup.tree(session)
        if args.sample_interval:
            samples = SavedStateSamples(work, args.sample_interval)
            samples.thread.start()
        if args.perf:
            perf_log = (work / 'perf.log').open('w')
            profiler = subprocess.Popen((['sudo', '-n'] if args.perf_sudo else []) +
                [str(args.perf.resolve()), 'record', '-F', '99', '-m', '4096',
                '--call-graph', f'dwarf,{args.perf_stack_size}', '-p', str(session.proc.pid),
                '-o', str(work / 'perf.data')], stdout=perf_log, stderr=perf_log)
            time.sleep(.2)
            common.require(profiler.poll() is None, 'perf failed; see perf.log')
        # A host timer only terminates the command. All exploration is native BFS.
        timer = threading.Timer(args.seconds, lambda: session.proc.send_signal(signal.SIGINT)
                                if session.proc.poll() is None else None)
        start = time.monotonic()
        timer.start()
        reply = setup.checked(session, 'bfs --quiet')
        timer.cancel()
        timer.join()
        report['command_seconds'] = time.monotonic() - start
        (work / 'bfs.txt').write_text(reply)
        if samples:
            samples.close()
            report['sampled_new_saved_states'] = samples.count - samples.initial_count
            report['sample_interval_s'] = args.sample_interval
            samples = None
        if profiler:
            profiler.send_signal(signal.SIGINT)
            # The supervisor may report either form of SIGINT after perf flushes.
            report['perf_exit'] = profiler.wait(timeout=15)
            common.require(report['perf_exit'] in (0, -signal.SIGINT, 128 + signal.SIGINT),
                           'perf record failed')
            perf_log.flush()
            recorded = (work / 'perf.log').read_text()
            common.require('Captured and wrote' in recorded and ' lost ' not in recorded,
                           'perf recording incomplete or lost samples; see perf.log')
            report['profile_scope'] = 'tracer CPU samples around BFS; diagnostic timing only'
            report['perf_stack_size'] = args.perf_stack_size
            report['perf_buffer_pages'] = 4096
        metrics = common.search_metrics(reply)
        if 'Program received signal SIGINT, Interrupt.' in reply:
            metrics['completion'] = 'interrupted'
        common.require(metrics.get('elapsed_s', 0) > 0 and 'new' in metrics,
                       'missing native BFS metrics')
        metrics['new_states_per_s'] = metrics['new'] / metrics['elapsed_s']
        report['search_metrics'] = metrics
        work_receipts = [json.loads(line.removeprefix('SEARCH_WORK '))
                         for line in reply.splitlines() if line.startswith('SEARCH_WORK ')]
        common.require(len(work_receipts) <= 1, 'duplicate native BFS work receipt')
        if work_receipts:
            report['search_work'] = work_receipts[0]
            common.require(work_receipts[0]['transitions'] == metrics['searched'] - 1,
                           'native BFS transition and visit counts disagree')
        after = setup.tree(session)
        (work / 'prefix-tree.json').write_text(json.dumps(sorted(before)))
        (work / 'tree.json').write_text(json.dumps(sorted(after)))
        report['first_parent_edges'] = len(after)
        prefix_nodes = {report['root']} | {node for edge in before for node in edge[:2]}
        all_nodes = {report['root']} | {node for edge in after for node in edge[:2]}
        report['generated_new_nodes_including_budget_cuts'] = len(all_nodes - prefix_nodes)
        report['generated_new_nodes_per_s'] = len(all_nodes - prefix_nodes) / metrics['elapsed_s']
        if 'sampled_new_saved_states' in report:
            common.require(report['sampled_new_saved_states'] == len(all_nodes - prefix_nodes),
                           'saved-state sampling and final tree counts disagree')
        report['counter_scope'] = ('CLI searched counts root plus transitions; CLI new excludes '
            'model-budget-cut successors; generated_new_nodes counts all saved new identities '
            'relative to the deterministic prefix, including budget cuts')
        report['evidence_scope'] = 'native BFS; tree is first-parent witnesses, not every edge'
        completed = True
    finally:
        try:
            # Every cleanup runs even if a sampler or profiler fails. Cancel the
            # timer before joining it; close the owned tracer after observers.
            with ExitStack() as cleanup:
                cleanup.callback(session.close, timeout=30)
                if perf_log:
                    cleanup.callback(perf_log.close)
                cleanup.callback(stop_profiler)
                if samples:
                    cleanup.callback(samples.close)
                if timer:
                    cleanup.callback(timer.join)
                    cleanup.callback(timer.cancel)
        finally:
            report['tracer_exit'] = session.proc.returncode
            report['passed'] = completed and report['tracer_exit'] == 0 and sys.exc_info()[0] is None
            with session.log.open('rb') as transcript:
                transcript.seek(max(0, session.log.stat().st_size - 65536))
                tail = transcript.read().decode(errors='replace')
            summaries = [json.loads(line.removeprefix('OBSERVATION_CACHE_PROCESS '))
                         for line in tail.splitlines() if line.startswith('OBSERVATION_CACHE_PROCESS ')]
            if summaries:
                report['observation_cache_process'] = summaries
            (work / 'result.json').write_text(json.dumps(report, indent=2))
    common.require(report['passed'], 'benchmark tracer did not finish successfully')
    print(json.dumps(metrics, indent=2), flush=True)
    print('all saved new identities/s:', report['generated_new_nodes_per_s'], flush=True)


if __name__ == '__main__':
    main()
