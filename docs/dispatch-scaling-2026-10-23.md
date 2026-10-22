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

变体尚未完成正确性与性能验收，不进入生产。更多同步发送也可能推迟读取结果、
发现 worker 错误和检查 deadline；必须对比中断、worker loss、重试及全图结果，
不能单凭提高派发量宣称吞吐改进。
