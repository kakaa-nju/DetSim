#!/usr/bin/env python3
"""Summarize window-filtered perf stacks; inclusive hotspots overlap, phases do not."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess


def summarize_syscalls(metadata, stacks):
    pattern = re.compile(r'^\s*.*?\s+(\d+)\s+\[\d+\]\s+([\d.]+):\s+syscalls:sys_(enter|exit)_(\w+):\s*(.*)')
    pending, counts, lost = {}, {}, 0
    with stacks.open() as stream:
        for line in stream:
            if 'LOST' in line:
                lost += 1
            match = pattern.match(line)
            if not match or not metadata['begin'] <= float(match[2]) <= metadata['end']:
                continue
            tid, now, phase, name, detail = int(match[1]), float(match[2]), match[3], match[4], match[5]
            row = counts.setdefault(name, dict(entered=0, completed=0, seconds=0.0, max_seconds=0.0,
                over_1ms=0, errors=0, requested_bytes=0, returned_bytes=0, requests_at_most_16_bytes=0))
            key = tid, name
            if phase == 'enter':
                row['entered'] += 1
                pending[key] = now
                size = re.search(r'\blen: (0x[0-9a-f]+)', detail)
                if size:
                    value = int(size[1], 16)
                    row['requested_bytes'] += value
                    row['requests_at_most_16_bytes'] += value <= 16
            else:
                begin = pending.pop(key, None)
                if begin is None:
                    continue
                duration = now - begin
                row['completed'] += 1
                row['seconds'] += duration
                row['max_seconds'] = max(row['max_seconds'], duration)
                row['over_1ms'] += duration >= .001
                value = int(detail.split()[0], 0)
                if value >= 2**63:
                    value -= 2**64
                row['errors'] += value < 0
                if name in ('sendto', 'recvfrom', 'sendmsg', 'recvmsg', 'process_vm_readv', 'process_vm_writev') and value > 0:
                    row['returned_bytes'] += value
    return {'command': metadata['command'], 'window_seconds': metadata['end']-metadata['begin'],
            'syscalls': counts, 'unmatched_entries': len(pending), 'lost_record_lines': lost,
            'limits': 'Instrumented syscall latency, concurrent durations overlap; not CPU time or uninstrumented throughput. sendmsg/recvmsg return bytes count only their packet payload; SCM_RIGHTS mapped object bytes and requested iov lengths are not present in these tracepoints.'}


def summarize(metadata, stacks, worker_pids):
    phases, roles, inclusive, leaves = Counter(), Counter(), Counter(), Counter()
    total = unknown_leaf = 0
    header = re.compile(r'^.*?\s+(\d+)/(\d+)\s+([\d.]+):\s+cpu-clock:')
    frame = re.compile(r'^\s+[0-9a-f]+\s+(.+?)\s+\(([^()]*)\)\s*$')
    patterns = {
        'process_vm_readv': ('process_vm_readv',),
        'memcmp': ('memcmp',),
        'memcpy/memmove': ('memcpy', 'memmove', 'rep_movs'),
        'page pin/walk': ('get_user_pages', 'pin_user_pages', 'follow_page'),
        'maps parsing': ('get_maps_item',),
        'fsync/fdatasync': ('fsync', 'fdatasync',),
        'hash': ('XXH', 'XXHash', 'libxxhash'),
        'compression': ('ZSTD_', 'libzstd'),
        'bundle import': ('snapshot::import_bundle',),
        'bundle export': ('snapshot::export_bundle',),
        'proc inventory': ('observe_descendants', 'direct_children'),
    }

    def consume(lines):
        nonlocal total, unknown_leaf
        match = header.match(lines[0]) if lines else None
        if not match or not metadata['begin'] <= float(match[3]) <= metadata['end']:
            return
        pid, tid = int(match[1]), int(match[2])
        block = '\n'.join(lines)
        symbols = [m[1] for line in lines[1:] if (m := frame.match(line))]
        leaf = symbols[0] if symbols else '[unparsed]'
        total += 1
        unknown_leaf += leaf in ('[unknown]', '[unparsed]')
        if leaf == '[unknown]':
            parsed = frame.match(lines[1]) if len(lines) > 1 else None
            leaf += '@' + (parsed[2] if parsed else 'unparsed')
        leaves[leaf] += 1
        if pid == metadata['coordinator_pid']:
            role = 'coordinator main' if pid == tid else 'coordinator threads'
        elif pid in worker_pids:
            role = 'worker main' if pid == tid else 'worker threads'
        else:
            role = 'tracee/other inherited'
        roles[role] += 1
        for name, needles in patterns.items():
            if any(needle in block for needle in needles):
                inclusive[name] += 1
        if role == 'tracee/other inherited': phase = 'tracee/other inherited'
        elif 'StateStore::io_worker' in block: phase = 'background storage'
        elif any(x in block for x in ('state_initialization', 'read_config(', 'init_dwarf', 'build_manifest(')):
            phase = 'worker bootstrap/compatibility'
        elif 'snapshot::import_bundle' in block: phase = 'bundle import'
        elif 'snapshot::export_bundle' in block: phase = 'bundle export'
        elif any(x in block for x in ('capture_stopped', 'save_paged_snapshot', 'save_full_state_to_state_store', 'prepare_capture')):
            phase = 'capture'
        elif any(x in block for x in ('recover_running_state', 'recover_mem_reg', 'restore_memory', 'adopt_snapshot', 'prepare_thread_pool')):
            phase = 'restore/adoption'
        elif 'wire::' in block: phase = 'wire IO/encoding'
        elif 'observe_descendants' in block or 'direct_children' in block: phase = 'process inventory'
        elif 'sys_state::sys_state(unsigned long)' in block or 'tracee_state::tracee_state(unsigned long)' in block:
            phase = 'snapshot load'
        elif 'ExecutionEngine::' in block or 'exec_once' in block: phase = 'execute'
        else: phase = 'other/unattributed'
        phases[phase] += 1

    block = []
    with stacks.open() as stream:
        for line in stream:
            if line.strip(): block.append(line.rstrip())
            elif block: consume(block); block = []
        if block: consume(block)
    if not total:
        raise RuntimeError('no samples in command window: ' + str(stacks))
    def rows(counter):
        return [{'name': name, 'samples': count, 'cpu_sample_percent': round(100*count/total, 2)}
                for name, count in counter.most_common()]
    return {'command': metadata['command'], 'samples': total, 'unknown_leaf_samples': unknown_leaf,
            'window_seconds': metadata['end'] - metadata['begin'], 'roles': rows(roles),
            'exclusive_phases': rows(phases), 'inclusive_hotspots_overlap': rows(inclusive),
            'top_self_symbols': rows(Counter(dict(leaves.most_common(20)))),
            'limits': 'CPU samples, not wall-time fractions; stack truncation may undercount callers. Kernel sleep is not sampled.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--perf', type=Path, required=True)
    parser.add_argument('--sudo', action='store_true')
    parser.add_argument('--reuse-stacks', action='store_true')
    args = parser.parse_args()
    report_path = args.run / 'results.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    workers = set()
    for row in report.get('parallel', []):
        for line in (Path(row['artifacts']) / 'journal.jsonl').read_text().splitlines():
            record = json.loads(line)
            if record['event'] == 'worker_spawn': workers.add(record['pid'])
    summary = []
    profile_root = args.run / 'session' if (args.run / 'session').is_dir() else args.run
    directories = sorted(profile_root.glob('perf-*'))
    if not directories:
        parser.error('no perf-* profiles found')
    for directory in directories:
        metadata = json.loads((directory / 'profile.json').read_text())
        syscall_mode = metadata['scope'].startswith('selected syscall')
        stacks = directory / 'stacks.txt'
        if not args.reuse_stacks or not stacks.exists():
            with stacks.open('w') as output, (directory / 'decode.log').open('w') as errors:
                subprocess.run((['sudo', '-n'] if args.sudo else []) + [str(args.perf), 'script', '-f',
                    '-i', str(directory / 'perf.data')] + ([] if syscall_mode else
                    ['-F', 'comm,pid,tid,time,event,ip,sym,dso']),
                    stdout=output, stderr=errors, check=True)
        row = summarize_syscalls(metadata, stacks) if syscall_mode else summarize(metadata, stacks, workers)
        (directory / 'summary.json').write_text(json.dumps(row, indent=2))
        summary.append(row)
    (args.run / 'perf-summary.json').write_text(json.dumps(summary, indent=2))
    for row in summary:
        if 'syscalls' in row:
            print(row['command'], row['syscalls'])
            continue
        print(row['command'], 'samples', row['samples'], 'unknown', row['unknown_leaf_samples'])
        print('phases', [(x['name'], x['cpu_sample_percent']) for x in row['exclusive_phases']])
        print('hotspots', [(x['name'], x['cpu_sample_percent']) for x in row['inclusive_hotspots_overlap'][:6]])


if __name__ == '__main__':
    main()
