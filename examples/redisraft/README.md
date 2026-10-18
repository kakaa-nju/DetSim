# RedisLabs Raft library

`raft/` is the pinned Raft C library, not the complete RedisRaft Redis module.
`redisraft_fixture.c` drives three nodes; `redisraft_repair_fixture.c` adds the
log-repair workload. Existing check/clock plugins and JSON configurations remain
beside their library. `run.py` builds owned healthy/mutant copies and leaves the
submodule untouched.

```sh
python3 examples/redisraft/run.py --tracer ./tracer --backend all
python3 examples/redisraft/quorum.py --tracer ./tracer --backend all
python3 examples/redisraft/repair.py --tracer ./tracer --backend all
```

Each controlled entrypoint requires NP=3 and also supports `--native-only`.
`run.py`, `quorum.py` and `repair.py` own their election, replication-quorum and
log-repair configuration/acceptance policy. `search.py` holds this case's fixed
three-node sibling search, Raft log observation, ledger and replay validation;
its bounded beam policy is not native exhaustive BFS/DFS. It is case-side debt:
do not extend it; new exploration should use the built-in CLI DFS, and the
sibling search should be replaced when the built-in actor/outcome/property path
can produce the same exact witness and replay evidence. For the built-in entries,
performance records use `search_metrics` from the automatic DFS report only;
manual-prefix, build, replay and whole-case timings are operational context.
Beam-1 rows remain case-side debt and cannot be compared as built-in throughput.

Each session writes its exact `<session>/config.json`; source and tool hashes,
native oracles, observations, transcripts and three-replay evidence go beneath
the selected `--output` directory. The actual Redis module/client workload lives
separately in `../redisraft_module/`.

## Profiling and snapshot comparisons

`profile_search.py` wraps the native comparison with perf control-FIFO windows;
`--syscalls` records syscall enter/exit events instead of CPU call stacks.
`profile_search_summary.py` separates exclusive phases from overlapping inclusive
hotspots. Setup and manual prefixes are excluded. See the
[full-pipeline perf report](../../docs/search-perf-2026-10-18.md) for commands,
measured IPC/capture costs, and the additional Willemt Raft comparison.

`parallel_benchmark.py` builds the project-owned healthy nonblocking fixture,
prepares one three-node checkpoint, then runs native DFS and 1/2/4-worker DFS/BFS
from that checkpoint. It compares full parallel transition graphs, node depths
and phases, plus the serial search's newly discovered nodes and available
first-parent witnesses. A first-parent tree is not a complete serial edge oracle.

```sh
make benchmark-redisraft-parallel CC=/usr/bin/gcc CXX=/usr/bin/g++
make benchmark-redisraft-parallel REDISRAFT_PARALLEL_ARGS='--backend full --depth 4'
```

Search throughput uses the native command's own timer. Build, prefix, loading,
inspection and command wall time are recorded separately. The target remains
the Raft C library, not a Redis consensus module. See the
[parallel execution report](../../docs/parallel-exploration.md) for results and
limits. The legacy library driver used below can reach an untimed all-waiting
state because its `recvfrom` assumes nonblocking behavior; the new comparison
rejects that as a throughput workload instead of weakening pending syscall rules.

`benchmark.py` controls the existing three-node library driver and measures its
manual prefix followed by native DFS. `seed.c` supplies the optional benchmark
seed interposer; it is not part of ordinary case semantics. The
`benchmark-redisraft` make target builds case plugins under
`build/<mode>/np3/examples/redisraft/` and runs this benchmark.

```sh
make benchmark-redisraft CC=/usr/bin/gcc CXX=/usr/bin/g++
python3 examples/redisraft/page_snapshot_benchmark.py --tracer ./tracer --repeats 3
python3 examples/redisraft/profile_summary.py PATH_TO_RESULT_JSON PATH_TO_PERF_STACKS
```

The default comparison plugin directory is
`build/release/np3/examples/redisraft`; pass `--plugin-dir` for another build mode.
Its files are `choose.so`, `check.so` and `seed.so`. Comparison runners invoke
this directory's `benchmark.py` and leave exact per-run configuration, outputs
and measurements under the selected `--output`. `profile_summary.py` attributes
recorded DFS-window CPU samples, not wall-clock shares.

`soft_dirty_benchmark.py` and the page comparison's optional `--soft-dirty` flag
retain the historical explicit unsafe delta experiment. They do not establish
sound incremental capture and are not enabled by the normal case runner.
