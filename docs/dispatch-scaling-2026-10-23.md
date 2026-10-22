# 任务供应与搜索顺序实验（2026-10-23）

此前的[调度事件分析](off-cpu-analysis-2026-10-19.md)已确认 worker 等待显著。
本轮检验两个方向：现有 DFS 能否减少阶段等待，以及协调器是否有任务却没有及时派发。
以下是实验结果，不是新的生产优化或近线性加速声明。

## 已有 BFS / DFS 策略对比

固定 NP3 tracer SHA256 `90df0e151499e360b9e988d97a1194c042e7f9be8e82f493ad99dd6bc79d183c`，
Raft-library fixture、pages/shared、same-cpu、locality none、seed 1、depth 12。
每台机器各两轮，策略顺序分别 BFS→DFS、DFS→BFS；每种策略运行 1 / 12 workers。
16 个 session 均通过完整规范图比较：10457 节点、18266 条边、18266 次尝试边、零重试。
规范图摘要均为 `56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
引用已有同二进制 native reference；原生串行 oracle 的边部分是首次发现父边投影，
完整带标签图一致性来自各并行运行间比较，不能说 native oracle 独立验证了所有边。

下表为两次计时的中位数，单位秒：

| 主机 | BFS / 1 worker | BFS / 12 workers | DFS / 1 worker | DFS / 12 workers |
| --- | ---: | ---: | ---: | ---: |
| 本机，6 核 12 线程 | 30.4496 | 7.0376 | 30.9760 | 6.4501 |
| gpunode WSL，12 核 24 线程 | 24.5727 | 3.9354 | 23.6278 | 3.6290 |

12-worker DFS 相对 BFS 分别减少约 8.35% / 7.79% 的时间，两轮方向一致。
只有两轮、小图，尚不足以证明稳定通用收益。DFS 改变访问顺序和局部性，不能把差值
全算作消除 BFS 层屏障的收益。公平串行基线应使用同机较快策略；据此这组最佳
12-worker 加速比分别约 4.72 / 6.51，仍未达到接近线性。

DFS 没有增加实际尝试边，但增加了本地深度截断与重新打开的记账：
BFS 固定 10457 leases / 6633 local completions；DFS 单 worker 为 11082 / 7258，
12 workers 的 leases 为本机 10745、10740，WSL 10715、10612。
因此“图和执行边相同”不意味着全部计算量相同。这里使用的是已有策略，不是新增功能。

计时沿用 `search_and_finalization_seconds`：搜索命令至提示符，加上图验证之后的
graceful close。两段不连续，中间验证可能与内核写回重叠；不是完整连续端到端耗时，
也不是立即关闭的耗时。所有输入、命令、逐次结果保存在
`build/shared-parallel/dispatch-policy-20261023/`，WSL 镜像在其 `wsl/results/`。
WSL 私有 namespace 退出时三项卸载均成功，实验后重新检查无归属进程、无宿主残留挂载；
tracer 和目标 ELF 摘要保持不变。宿主 ASLR 和系统配置未修改。

## 协调器空闲归因

私有诊断基于完整同代 full-FP 对象构建，仅增加观察，不改变调度策略。
二进制摘要 `6762466bb114599a53b6961024f47359ff34e2c1e3262e6673604cb158933a95`。
本机 BFS / 12 workers 的一次 native-reference 运行通过上述同图和零重试检查。
每次 frontier / ticket / worker 生命周期变更前后结算单调时钟，按阶段和 frontier
可派性累计；没有每轮扫描整个 frontier。

测量区间 6.5971 秒，另有 0.1273 秒初始化；累计空闲按 coordinator 中
`hello && !ticket` 定义，单位为 worker·秒：

| Frontier 状态 | 累计逻辑空闲 |
| --- | ---: |
| 有结构上可派的状态 | 2.0500 |
| BFS 层屏障阻止派发 | 1.5557 |
| pending 为空，包含启动与退出阶段 | 1.2218 |

有可派状态的空闲中，约 1.2216 worker·秒发生于 dispatch，0.3551 于本地完成，
0.3535 于边验证。层屏障空闲中约 1.3566 worker·秒发生于协调器 poll 阶段。
逐 cell 的 idle + busy-ticket + startup + unstarted + retired 均严格等于
12 × wall time；总积分加初始化等于 scope finish，事件缓冲零丢弃。

这些数值不能替代 perf 的真实 off-CPU 时间。已发送 Done 但尚未被协调器接收的
worker 仍算 busy；准备 Task 元数据时 ticket 也可能已经分配。eligible 还可能仅是
能在协调器本地完成的状态。该探针有开销，不能与无探针速度比较，也不能把前次
syscall 跟踪的 poll 时间减去本表以推导遗漏时间。
原始 JSON、构建清单和完整性检查在 `build/shared-parallel/dispatch-idle-diagnostic/`。

## 当前优化实验

协调器原先每成功发送一个远程 Task 就结束派发扫描，这对传输完整闭包的 durable
路径有避免长时间阻塞结果排空的用途。shared 路径只传元数据引用，也沿用了该限制。
已准备私有 cap4 / fullscan 两个变体，仅 shared 路径每轮允许四次或最多 worker 数次
成功发送；本地完成和启动不计数，durable 仍在首次成功发送后退出扫描。

两个变体均通过 pure Frontier 和各七项 NP3 原生测试：64 状态 / 144 边的
full/pages、BFS/DFS、1/4/8 workers 完整带标签图，durable/shared 对照，以及
worker loss、重试耗尽、启动超时、中断和违规重放。未单独观测实际每一轮成功
发送次数；不能仅凭 journal 中连续 lease 宣称已经证明批次执行频率。

随后在本机完成相同 full-FP 构建参数的基线与两个变体对比，12 workers，其他
Raft 输入同上。每个二进制先建立自己的 native reference，再分别按
baseline→cap4→fullscan 和相反顺序计时。九次运行全部通过 10457 / 18266 同图、
18266 尝试边、零重试；参考建立运行不计入下表两轮中位数。

| 派发策略 | 第一轮秒 | 第二轮秒 | 中位数秒 |
| --- | ---: | ---: | ---: |
| 基线，每次一个 | 6.2084 | 7.2181 | 6.7132 |
| cap4 | 7.4730 | 6.2415 | 6.8572 |
| fullscan | 7.2536 | 7.3236 | 7.2886 |

cap4 中位数增加约 2.15%，fullscan 增加约 8.57%；cap4 两轮方向不一致，样本少且
基线波动明显，不能据此断言精确回退比例。但没有足够收益证据，故两者均不进入生产，
也没有必要以这组结果宣称更好的加速比。完整输入、同二进制参考、原始结果在
`build/shared-parallel/dispatch-batch-timing-20261023/`；构建与故障测试证据在
`build/shared-parallel/shared-dispatch-batch/`。生产源、默认策略均未改动。

更多同步发送可能推迟读取结果、发现 worker 错误和检查 deadline；通过当前门禁
并不证明响应延迟不变。下一项应量化 worker Done 发出至协调器接收、再至下次
Task 收到的间隔，与 frontier 可派性关联。这能补足当前 ticket 统计漏掉的实际
空闲；该轮尚无这组测量，不能预先归因于结果排空或通信带宽。

## Worker 实际任务间隔：后续测量

补充了私有 worker 时间线：Task receive 返回、Done send 开始/返回使用同一个
steady-clock epoch；协调器日志增加 epoch 起点，将其相对时间换算到同一时间轴。
没有修改协议或生命周期。成功搜索也会直接终止 worker，故不能依赖退出时写日志：
每个 worker 在私有 cwd 创建固定容量 MAP_SHARED 文件，每事件只写内存，完整
record 后 release 发布 committed 数；主进程回收 worker 后解析，不逐任务 write/fsync。
这仍会增加文件映射、页错误和时间戳开销，不用于生产速度排名。

二进制 SHA256 为 `6d771e15a7cce79dd830abb82dfe054f9303ba5403d0cd4316474fda3956d8af`。
Frontier、假时钟积分和映射日志测试通过；映射测试包含真实 fork/SIGKILL/wait 后
读取已提交前缀、容量溢出、创建失败及绝对时间转换。分析器 11 项合成测试通过，
独立审查指出的协调器自身时间因果校验缺口已加入回归。

本机各一次 BFS/12、DFS/12、BFS/1 均通过相同 10457 / 18266 图、18266 尝试边、
零重试。每次 3824 个远程任务全部一一匹配，worker/coordinator 均零丢失事件，
读取前确认对应 worker PID 已消失。只计算每个 worker 首次 Task 接收至最后 Done
发送返回的范围；排除启动、最后一个任务后等待、收尾及失败重试。

下列为累计 worker 墙钟秒，不是 CPU 时间或搜索 wall time：

| 区间 | BFS / 1 | BFS / 12 | DFS / 12 |
| --- | ---: | ---: | ---: |
| Task 收到至开始发送 Done | 30.8507 | 73.8487 | 69.5507 |
| Done send 调用 | 0.0184 | 0.1341 | 0.1425 |
| 两任务之间空档 | 0.7496 | 6.6292 | 5.2965 |
| 覆盖的 worker 时间合计 | 31.6187 | 80.6119 | 74.9897 |
| 空档占覆盖时间 | 2.37% | 8.22% | 7.06% |

三项时间之和逐纳秒等于覆盖时间。12 workers 各有 3812 个任务间区间，单 worker
3823 个；任务区间重叠，不能把累计秒直接从搜索耗时中扣除。

| 任务间空档的细分 | BFS / 1 | BFS / 12 | DFS / 12 |
| --- | ---: | ---: | ---: |
| Done 返回后至协调器接收 Done | 0.0601 | 2.2529 | 2.2353 |
| 协调器接收到完成验证 | 0.1432 | 0.1630 | 0.1238 |
| 验证后至下一 Task 发送返回 | 0.4983 | 3.0631 | 1.9899 |
| Task 发送返回至 worker 接收 | 0.0480 | 1.1502 | 0.9475 |

四段严格分割同一 worker 的任务间空档。跨 CPU 的接收可以早于发送函数返回，
所以按 worker 空档范围裁剪有序的协调器边界，并单列此类并发重叠计数；没有将其
当作时钟错误。BFS/12 实际出现 309 次 Done 接收早于 send 返回、199 次 Task
接收早于 send 返回。最后一项不能叫单向网络延迟，前几项也包含 CPU 调度等待；
“验证后至发送”还包含选择、加载、构包和发送，不能全部归为 BFS 层屏障。

这组数据说明：任务供应间隔确实扩大，但任务持有期间的累计墙钟也从约 30.85 秒
增至 73.85 秒。仅消除任务之间的空档不足以解释或消除全部扩展性损失。持有任务
期间仍可能阻塞于文件、ptrace/tracee 等待或处于 runnable 排队，不能把该区间误称
为纯计算。下一步需要在同一次运行中关联 scheduler trace 与这些任务区间，并在
gpunode 的 12 个物理核上复核；这里仍是 6 核 12 线程机器，不能代替该结论。

源码与构建证据：`build/shared-parallel/worker-cycle-diagnostic/`；分析器与测试：
`build/shared-parallel/worker-cycle-analysis/`；原始运行和汇总：
`build/shared-parallel/worker-cycle-runs/`。这些是私有诊断，未改变生产执行代码。

后续已完成[任务区间与调度事件对齐](worker-cpu-scaling-2026-10-23.md)，包括
gpunode WSL 的独立 PID 域映射验证；不能混用两台机器的等待比例。
