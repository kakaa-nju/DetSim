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

# 比较两个 checker 构建是否等价（转换多重集 + 摘要 + 违规行；不再逐行比哈希）
python3 perf/raft/verify_checker.py 4,5,6,8 plugins/check.so /path/to/other.so
```

## 分阶段细目与搜索收据

`PROFILER=1` + `DETSIM_TRANSITION_PROFILE=1` 现在额外输出一行 16 段的细目
（capture 的 layout/regs/serialize/snapshot、ctor、check、commit 的 encode/persist、
state 重开、running 拷贝等）：

```
TRANSITION_PROFILE_DETAIL {"transitions":N,"capture_total_us":…,"capture_snapshot_us":…, …}
```

另有一个独立的、默认关闭的搜索收据，用于比对两次运行的**状态空间**（而不是时间）：

```sh
DETSIM_SEARCH_AUDIT=1 ./tracer -n -c config.json -b cmds.txt 2> run.log

# 边集 + 状态集 + 与状态命名无关的图同构指纹
python3 perf/raft/state_fingerprint.py run-A.log run-B.log

# 二进制无关：直接读 sstate 元数据 append log 里的状态集合
python3 perf/raft/state_fingerprint.py --sstate A/sstate/packed.dat B/sstate/packed.dat
```

`SEARCH_EDGE` 每提交一条边输出一行，`SEARCH_AUDIT` 每次搜索输出一行收据。
**不要**在计时时打开：它写 stderr，未缓冲写到 virtiofs 上会严重拖慢。

状态标识目前不能跨进程复现（静态目标镜像里有一处宿主派生字节），所以
`states_fp` 只能用于**同一进程内**的比较；跨运行/跨构建请用图同构指纹。
比较前工具会核对两边日志里的 `SEARCH_AUDIT` 收据条数，条数不同直接拒绝（退出 4）——
双搜索日志本来就比单搜索多一倍边，这样的比较没有意义。
同进程比较的做法：`DETSIM_SEARCH_ISOLATE=1` 让每条搜索命令重入会话首个根状态并清空
覆盖记忆，于是同一日志里的多条收据可以直接比对：

```sh
printf 'dfs 8 --quiet\ndfs 8 --quiet\n' > cmds.txt
DETSIM_SEARCH_ISOLATE=1 DETSIM_SEARCH_AUDIT=1 ./tracer -n -c cfg -b cmds.txt 2> run.log
python3 perf/raft/state_fingerprint.py --receipts run.log     # 规约/完备性闸门
```

这是 Stage 4 规约策略必须通过的闸门：同一根、未加规约 → 收据逐字段相同；
换成不同深度立刻报错（已验证灵敏性）。
细节见 [`docs/search-efficiency-report-2026-11-17.md`](../docs/search-efficiency-report-2026-11-17.md)。

## 覆盖收据（判断"重复展开"还剩多少）

```
SEARCH_COVERAGE {"entries":12726,"skipped_fully":0,"skipped_depth":6522,"expanded":3551,"re_expanded":105}
```

`skipped_depth`/`skipped_fully` 是被覆盖记忆跳过的进入；`re_expanded` 是"同一状态被再次
真正展开"的次数——后者才是"重放已记录后继"这类优化能触达的空间（实测 d12 仅 3%）。

## 身份纯度诊断（Stage 3 取舍用）

判断"某个分量值不值得从状态身份里净化"，靠测量而不是判断：

```sh
DETSIM_IDENTITY_DIAG=1 DETSIM_SEARCH_AUDIT=1 ./tracer -n -c cfg -b cmds.txt 2> run.log
grep IDENTITY_DIAG run.log
# IDENTITY_DIAG {"base":702,"no_user_state":459,"no_metadata":636,"no_maps":702,
#                "no_regs":702,"no_pending_phase":702}
```

每个 variant 是同一次捕获的**只读投影**（把该切片从身份编码里去掉后的身份），
报告整场搜索看到的 distinct 数；数字不降即该项对本负载是 no-op。诊断不回灌搜索：
开/关它时 `searched/unique/transitions` 与图同构指纹完全一致。仅 `pages` 后端提供切片。

## 线程池停止屏障证书

`prepare_thread_pool()` 的完整停止校验（枚举宿主任务集合 + 逐 carrier 探针）在
一个转换里会被 restore 与 capture 各做一次，占搜索时间约 19%。默认按证书复用一次：

```sh
DETSIM_POOL_BARRIER=on      # 默认：证书有效时跳过 capture 侧那一次
DETSIM_POOL_BARRIER=off     # 旧行为，每次都校验（bisect / oracle）
DETSIM_POOL_BARRIER=verify  # 每次都校验，并审计"证书有效"这个判断是否成立
```

证据（d8 −7.8% 搜索时间、图同构指纹三档一致、全字节 oracle、多线程回归）见
[`docs/search-efficiency-report-2026-11-17.md`](../docs/search-efficiency-report-2026-11-17.md)。

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
* **`Loglevel` 是"最低严重级别"**。`should_log()` 在 `level >= loglevel` 时打印，
  所以 `Loglevel=0` 会打开**全部** TRACE —— 逐状态日志的开销（d8 实测约 2.5×）
  会淹没被测量的钩子。杀日志要用 `6`（CRIT=5 之上），`bench_configs.sh` 已按此生成。
* 同一目录同时只允许一个 tracer（文件锁）；脚本会先清理残留进程。

## 本机限制

`kernel.perf_event_paranoid >= 3` 且无 `CAP_PERFMON` 时，`perf`、`gdb -p`、
`bpftrace`（kernel probe）以及读 `/proc/<pid>/syscall` 都会被拒绝，无法采样剖析或
统计系统调用。替代方案：

* 上述编译期阶段分析器（无需特权）；
* `make NP=3 CXXFLAGS="… -pg"` + `gprof`：可做函数级归因，但采样率低，
  40 秒的运行只有几十个采样点，需数分钟才有统计意义。
