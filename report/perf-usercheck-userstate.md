# UserCheck / UserState 性能分析报告

对 `examples/raft` 的实测性能分解，定位启用 `UserCheck` + `UserState` 后的真实开销，
并给出各瓶颈的量化证据与优化方向。

工具在 [`perf/`](../perf/)，全部数字可用第 7 节命令复现。

---

## 1. 结论摘要

1. **启用 `UserCheck` + `UserState` 的代价约为每状态 92–125 µs**，占单状态预算
   （约 2.4 ms）的 **4–5%**；端到端实测为 **−1% 至 −8%**（跨 session 波动，
   噪声 ±3%，见 3.1）。
2. **因此 400 → 200 states/s 不能由框架侧的钩子开销解释。** 观测到的减半必然来自
   **checker 本身比参考实现重约 18 倍**，或 `UserState` blob 很大。第 5 节给出
   定量成本模型与判定方法。
3. **真正的全局瓶颈是状态捕获（capture）**：约 **1.7 ms/状态，占 70–75%**，
   与是否启用钩子无关。要整体提速必须从这里下手（第 4 节）。
4. 参考 checker 内部存在约 **2 倍** 的重复求值空间（每状态重复读取同一字段、
   同一日志项最多询问 3 次）。已做原型验证（125 → 62 µs），但为保持插件可读性
   **未合入**，改为第 5 节的建模规则与护栏建议。

---

## 2. 每状态成本分解

固定深度 11（每次 4722 次转换），`Loglevel=0`，阶段分析器数据：

| 阶段 | 无钩子 | 启用钩子 | 说明 |
| --- | --- | --- | --- |
| capture | **≈1.74 ms (75%)** | **≈1.73 ms (71%)** | NP 个 tracee 的完整页级快照 |
| restore | 236 µs | 244 µs | 恢复 tracee 镜像 |
| syscall | 208 µs | 206 µs | 一次系统调用（含 ptrace 往返） |
| checkpoint | 141 µs | 145 µs | identity hash + 元数据提交 |
| checker | 0 | **125 µs** | `UserCheck::check()` |
| **合计** | **≈2.33 ms** | **≈2.45 ms** | 1/throughput |

`capture = 总时间 − (restore + syscall + checkpoint + checker)`；与 `SEARCH_WORK` 的
`capture_check_commit_us` 交叉验证一致。深度 8 时 checker 为 92 µs（状态更浅、
日志更短，扫描的日志项更少）。

---

## 3. 测量结果与可信度

### 3.1 启用钩子的端到端代价

固定深度、交错 3 轮取中位数（`bash perf/raft/ab.sh DEPTH 3`）：

| 深度 | 转换数 | 无钩子 | 启用钩子 | 差值 | checker |
| --- | --- | --- | --- | --- | --- |
| 8 | 936 | 432.4 | 426.8 | −1.3% | 92.0 µs |
| 11 | 4722 | 424.9 | 390.1 | −8.2% | 125.1 µs |
| 11（另一次 session） | 4722 | 422.8 | 408.3 | −3.5% | 125.0 µs |

**如何解读**：`check_us` 在多次 session 间非常稳定（92.0 / 92.6 / 125.0 / 125.1 µs），
而**端到端差值散布在 −1% 到 −8%**。原因是这个效应（约 5%）与主机噪声（±3%）同量级。
因此本报告以**阶段计时为准**（checker = 4–5%），端到端差值只作旁证。

这正是"钩子不可能造成 2 倍"的核心论据：若钩子真占一半时间，阶段计时会直接显示
约 1.2 ms/状态的 `check_us`，而实测是 0.125 ms。

### 3.2 微基准（`perf/encoding`）

```
tracee_state cereal 编码: 11.0 µs / 6.5 KB
xxh3_64 over 1.19 MB:    84 µs（14 GB/s）
```

---

## 4. 瓶颈清单（按影响排序）

### 4.1 状态捕获 capture — 1.7 ms/状态（70–75%）

每次状态转换要为**每个** tracee 做完整快照。raft 配置下每 tracee 捕获 7 个区域
共 1.19 MB，三个节点即 **3.56 MB/状态**：

* `process_vm_readv` 读取全部区域（约 0.3–0.4 ms）；
* **xxh3_64 内容寻址整份快照**：84 µs/1.19 MB × 3 ≈ **252 µs/状态**；
* `tracee_state` 的 cereal 序列化（11 µs/6.5 KB）；
* identity hash 又把整个 `sys_state` **再序列化一次**（与 4.4 同源）；
* 页清单编码、`resident` 发布、去重比较。

**优化方向**（未实施，需设计决策）：

* **去掉重复序列化**：`commit_metadata()` 重新序列化整个 `sys_state` 只为算
  identity hash，而同一份元数据刚在 `capture_stopped()` 里序列化过。缓存每节点
  元数据字节、只重编码变化部分。
* **增量捕获**：`soft_dirty_experiment.h` / `page_capture` 已有按页存储基础设施，
  配合固定布局（`RuntimeProfile.fixed_layout`）只捕获脏页。
* **合并哈希**：复用上一状态哈希 + 只对差异页重哈希，可省掉大部分 252 µs。

### 4.2 状态恢复 restore — 240 µs（10%）

* 每次恢复都**重新解析 `/proc/<pid>/maps` 文本**：gprof 中 `parse_mapping_text`
  是第二热点（self 4.7%，被调用 5.9 万次），而 `maps` 文本已经是快照的一部分。
* `SnapshotView::page()` 被调用 **474 万次**（gprof），每次区间二分 + 页查找；
  恢复路径可按页批量处理。

**建议**：恢复优先使用快照内 `maps_text()`，
`restore_memory_mappings_unchecked` 已有比较逻辑，仅在发现差异时读 live maps。

### 4.3 系统调用 syscall — 205 µs（8%）

单步一次系统调用的 ptrace 往返：`PTRACE_SYSCALL` ×2、`PTRACE_GETREGS` ×3、
`PTRACE_GET_SYSCALL_INFO` ×3，加 futex/信号检查。平台固有，需改架构才能显著压缩。

### 4.4 checkpoint — 143 µs（6%）

`commit_metadata()` 将整个 `sys_state` cereal 序列化后 xxhash 求 identity，
与 4.1 的重复序列化同源。

### 4.5 UserCheck — 92–125 µs（4–5%）

见第 5 节。

### 4.6 UserState — 可忽略

参考实现每节点 88 字节：每状态每节点一次 `memcpy`，并在 identity 编码里增加
264 字节。只有当 blob 达到数百 KB 时才显现。

### 4.7 配置项实测（均无可观收益）

| 调整 | 结果 | 结论 |
| --- | --- | --- |
| `compression_level: 1 → 0` | 432.1 vs 422.8 states/s | 噪声内；异步压缩线程不是瓶颈 |
| `Loglevel: 0 → 3` | 413 vs 423 states/s | 噪声内；**日志不是 400→200 的原因** |
| `hot_cache_mb: 512 → 2048` | 383.6 vs 408 states/s | 更大缓存反而更慢（内存压力） |

### 4.8 两个已排除的"优化"（重要，避免重复走弯路）

1. **把 checker 改成直接读 live tracee 内存 —— 慢 2.2 倍。**
   `eval<>()` 读的是**捕获快照**（`expr_ast.cpp: read_memory_long` →
   `SnapshotView::read_into`，约 0.2–0.4 µs/字），不是 live 进程。
   `ptrace(PTRACE_PEEKDATA)` / `process_vm_readv` 读 live 要 ~1.4 µs/字，
   且恢复未完成时会读到**恢复前的旧页**（实测读到 `raft` 为 0）。
   原型实测：122 µs/call → 271 µs/call。

2. **在表达式求值器里缓存 `SnapshotView` —— 仅 0.3%。**
   每次 `open()` 确实取全局锁并扫 LRU，但命中项被 splice 到表头，扫描是 O(1)。
   收益在噪声内，已回退以免引入无谓复杂度。

---

## 5. UserCheck 内部结构与成本模型

### 5.1 参考实现的重复求值

一次 `check()` 要发**数百次** `eval<>()`，不同取值只有几十个：

* 11 个属性函数各自重新求值同一批字段（`current_term`、`state`、`voted_for`、
  `commit_idx`、`num_nodes` …）；
* `get_log_entry_term(node, idx)` 对同一 `(node, idx)` 最多被询问 **3 次**
  （`check_leader_completeness` 两次、`check_committed_entries_replicated_to_majority`
  一次）。

### 5.2 成本模型

`eval<>()` 单次实测约 **0.19 µs**（含 1–3 次快照读）。于是

```
checker 成本 ≈ 0.19 µs × 节点数 × 采样日志项数 × 属性数
```

校验：参考实现 `3 节点 × ~20 日志项 × 11 属性 ≈ 660` 次求值 ≈ 125 µs，
与实测 125 µs 吻合（已含短路）。

**要把吞吐从 400 压到 200，checker 需要约 2.3 ms/状态 ≈ 参考实现的 18 倍。**
典型放大因子：

| 因子 | 示例 | 倍数 |
| --- | --- | --- |
| 节点数 | 3 → 5 | ×1.7 |
| 采样日志项 | 20 → 200 | ×10 |
| 属性数 | 11 → 20 | ×1.8 |
| 每属性重复读同字段 | 无去重 | ×2–3 |
| `UserState` blob | 88 B → 64 KB/节点 | identity 编码与 memcpy 上升 |

`NP=5`、`200` 个日志项、`20` 个属性即约 `5×200×20×0.19 µs ≈ 3.8 ms/状态` ——
足以把吞吐压到 200 states/s 以下。

### 5.3 已原型的优化（未合入）

在一次 `check()` 内，tracee 始终停止且不执行系统调用，因此同一字段读一次即可复用；
同理同一 `(node, idx)` 日志项只需读一次。用「按 (node, field) 的调用期缓存 +
日志项去重」实现后实测：

| 深度 | 参考 | 原型优化 | 加速 |
| --- | --- | --- | --- |
| 6 | 67.0 µs | 30.9 µs | 2.2× |
| 8 | 92.6 µs | 47.4 µs | 2.0× |
| 11 | 125.0 µs | 62.2 µs | 2.0× |

等价性用固定深度穷举逐行比对（深度 4/5/6/8 的探索序列、汇总行、
`[CHECK FAILED]` 完全一致）。**结论：这段复杂度只换来约 2 个百分点的端到端提升，
不值得牺牲插件的可读性，因此没有合入 `plugins/check.cpp`。** 如果将来某个 checker
的 `check_us` 超过了预算，可以按 §5.4 的规则在生成侧解决，而不是改参考实现。

### 5.4 生成 checker 的护栏（建议写入 `skills/model-construction.md`）

1. 一次 `check()` 内每个字段只读一次，跨属性复用；
2. 日志扫描设硬上限（如 ≤20 项），并对 `(node, idx)` 去重；
3. 避免每次调用分配 `std::map` / `std::vector`；
4. `user_state_size` 保持字节级，不要把日志或大数组塞进 `UserState`；
5. 用 `DETSIM_TRANSITION_PROFILE=1` 量 `check_us`，把它控制在状态预算的 5% 以内
   （参考：≤100 µs/状态）。

---

## 6. 本次改动

| 文件 | 内容 |
| --- | --- |
| `src/core/state/transition_profile.h` | 新增。每状态阶段计时，**编译期开关，默认关闭**（`make PROFILER=1`）；关闭时全部入口为空内联函数 |
| `src/core/state/state_transition.cpp` | 接入 restore / syscall / checkpoint / check 计时；热路径改用内联查询 |
| `src/core/scheduler.cpp` | 搜索结束时输出 `TRANSITION_PROFILE` 并重置 |
| `src/core/user_plugin.{h,cpp}` | `has_check_inline()` / `user_state_enabled_inline()`，避免每状态每节点跨编译单元调用 |
| `src/core/state/state.cpp` | 使用内联查询 |
| `Makefile` | 新增 `PROFILER=1` 开关；benchmark 源文件路径按目录调整 |
| `perf/` | 按 benchmark 分目录重组；新增 raft 基准与 encoding 探针 |
| `.gitignore` | 忽略基准生成的 `examples/raft/.bench-*.json` |

**`examples/raft/plugins/check.cpp` 未改动**（保持原始实现）。

---

## 7. 复现

```sh
cd DetSim

# 1) 阶段分析器（编译期开关）
make NP=3 PROFILER=1

# 2) 生成基准配置（写入 examples/raft/.bench-*.json，已 gitignore）
bash perf/raft/bench_configs.sh

# 3) 固定深度、交错 3 轮：启用 UserCheck+UserState 的代价
bash perf/raft/ab.sh 11 3
#   case       states/s        min        max    check us/tr    trans    vs nohook
#   nohook        424.9      418.8      435.0            0.0     4722        +0.0%
#   hook          390.1      389.1      400.2          125.1     4722        -8.2%

# 4) 单次分阶段测量
bash perf/raft/measure.sh myrun .bench-hook.json 30

# 5) 比较两个 checker 构建是否等价
python3 perf/raft/verify_checker.py 4,5,6,8 plugins/check.so /path/to/other.so

# 6) 编码/哈希微基准
make NP=3 benchmark && build/release/np3/bin/probe_enc
```

默认构建（不带 `PROFILER=1`）不含任何插桩代码，可用于正式性能测试。

---

## 8. 测量注意事项

* **必须固定深度**。用墙钟预算比较不同构建，会让更快的版本探索更深、更昂贵的
  状态，从而掩盖自身优势（早期用 30 s 预算得到的数字波动达 ±10%）。
* **交错 + 中位数**。单次噪声约 ±3%，与被测效应同量级；`ab.sh` 以轮转顺序消除漂移。
* **`Loglevel=0`**。逐状态日志开销可能超过被测量的钩子。
* 同一目录同时只允许一个 tracer（文件锁）；脚本会先清理残留进程。
