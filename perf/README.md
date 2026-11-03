# perf — DetSim 性能测量

每个 benchmark 一个目录，工具与它测量的对象放在一起。报告见
[`report/perf-usercheck-userstate.md`](../report/perf-usercheck-userstate.md)。

| 目录 | 测量内容 |
| --- | --- |
| [`raft/`](raft/) | `examples/raft` 上的端到端吞吐：**启用 `UserCheck`+`UserState` 的代价**，以及每状态阶段分解 |
| [`encoding/`](encoding/) | 状态元数据编码与快照哈希的微基准（`tracee_state` cereal 编码、xxh3 吞吐） |
| [`state_store/`](state_store/) | `StateStore` 分级缓存/压缩/去重基准 |
| [`sysstate_store/`](sysstate_store/) | 系统状态元数据存储基准 |
| [`xxhash/`](xxhash/) | xxh3 原始吞吐基准 |

## 阶段分析器的构建开关

每状态阶段计时是**编译期开关，默认关闭**：

```sh
make NP=3              # 默认：插桩完全不参与编译
make NP=3 PROFILER=1   # 启用阶段计时
```

关闭时 `src/core/state/transition_profile.h` 的所有入口都退化为空内联函数，
不引入时钟读取、计数器或输出代码。`PROFILER` 参与编译配置指纹，切换会触发重编。
即使启用，也只有进程设置了 `DETSIM_TRANSITION_PROFILE=1` 才会真正计时，
因此同一个二进制既能测量也能正常跑探索。

启用后每次搜索结束向 stderr 输出：

```
TRANSITION_PROFILE {"transitions":N,"restore_us":…,"syscall_us":…,
                    "checkpoint_us":…,"check_us":…}
```

`capture` 没有独立时钟：用总时间减去上述四项（BFS 也可用
`SEARCH_WORK.capture_check_commit_us − checkpoint_us − check_us` 得到）。

## raft/ 快速开始

```sh
make NP=3 PROFILER=1

# 生成配置（写入 examples/raft/.bench-*.json，已被 .gitignore 忽略）
bash perf/raft/bench_configs.sh

# 固定深度、交错 3 轮：UserCheck+UserState 的代价
bash perf/raft/ab.sh 11 3

# 单次分阶段测量
bash perf/raft/measure.sh myrun .bench-hook.json 30

# 比较两个 checker 构建是否等价（逐行比对探索序列）
python3 perf/raft/verify_checker.py 4,5,6,8 plugins/check.so /path/to/other.so
```

## 既有存储基准

```sh
make NP=3 benchmark
build/release/np3/bin/ss_bench
build/release/np3/bin/sys_bench
build/release/np3/bin/xxh_bench
build/release/np3/bin/probe_enc     # encoding/ 的探针
```

## 注意事项

* **固定深度比较**。用墙钟预算比较不同构建，会让更快的版本探索更深、更昂贵的
  状态，从而掩盖自身优势。
* **交错 + 中位数**。单次结果噪声约 ±3%；`ab.sh` 以轮转顺序消除漂移，
  并在 `REPS<3` 时提示。
* **`Loglevel=0`**。逐状态日志的开销可能超过被测量的钩子。
* 同一目录同时只允许一个 tracer（文件锁）；脚本会先清理残留进程。

## 本机限制

`kernel.perf_event_paranoid >= 3` 且无 `CAP_PERFMON` 时，`perf`、`gdb -p`、
`bpftrace`（kernel probe）以及读 `/proc/<pid>/syscall` 都会被拒绝，无法采样剖析或
统计系统调用。替代方案：

* 上述编译期阶段分析器（无需特权）；
* `make NP=3 CXXFLAGS="… -pg"` + `gprof`：可做函数级归因，但采样率低，
  40 秒的运行只有几十个采样点，需数分钟才有统计意义。
