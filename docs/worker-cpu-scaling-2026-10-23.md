# 任务区间与调度事件对齐（2026-10-23）

本轮继续[派发与任务周期实验](dispatch-scaling-2026-10-23.md)，将同一次运行的
worker 时间戳与 perf sched switch/wakeup/exec/fork/exit 对齐。目标是区分任务
内部的阻塞、可运行等待和 on-CPU，而不是把整个 Task 持有区间叫作计算。

## 实验与校验

两台机器分别运行 1 / 12 workers：原生 Linux 6 核 12 线程主机，以及 gpunode
的 Ryzen 9 3900X、12 核 24 线程 WSL2。固定同一个私有 NP3 full-FP 诊断二进制
`6d771e15a7cce79dd830abb82dfe054f9303ba5403d0cd4316474fda3956d8af`，Raft-library
fixture、pages/shared、BFS、same-cpu、locality none、depth 12、seed 1。

四次运行均通过 10457 节点、18266 边、18266 尝试边和零重试检查；每次匹配
3824 个远程任务，完整规范图摘要保持
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
引用各平台已有的同二进制 native reference；原生串行检查是节点与首次发现父边
投影，完整带标签图另外比较，不声称独立串行 oracle 验证所有边。

分类复用此前已验证的调度状态机，仅观察它输出的区间，与 TaskReceived→DoneSendBegin、
DoneSendBegin→DoneSent、DoneSent→NextTaskReceived 三种区间求交。
精确选择 journal worker main PID；不加入辅助线程或被控程序。
启动及最后任务后收尾排除。缺唤醒/缺边保留 unresolved，绝不填成 sleep 或零 CPU。
每阶段及总覆盖逐纳秒闭合。四次最终分析均零逆序、未解析行、异常及 unresolved；
这是观测范围内完整性，不是证明内核从未丢失整对事件。

## 结果

单位为累计 worker 秒。on-CPU 是调度事件显示该线程正在运行的区间，包含内核、
执行资源停顿及可能计入当前任务的中断，不等于有效用户计算。

| 主机 / workers | 任务内 on-CPU | 任务内阻塞 | 任务内 runnable 等待 | 两任务间总空档 |
| --- | ---: | ---: | ---: | ---: |
| 原生 Linux / 1 | 29.4141 | 0.3545 | 0.2174 | 0.9147 |
| 原生 Linux / 12 | 50.2947 | 3.0645 | 6.6421 | 4.4761 |
| WSL / 1 | 22.9371 | 0.2855 | 0.1616 | 0.5464 |
| WSL / 12 | 32.2657 | 0.6785 | 0.2194 | 1.9345 |

原生 Linux 上任务内 on-CPU 占比从 98.09% 降至 83.82%；12 workers 的任务内
off-CPU 达到 9.7067 秒，超过任务之间的 off-CPU 4.2649 秒。仅看等下一份 Task
会漏掉大量等待，先前只靠总 CPU 排除等待的结论不成立。

WSL 上任务内 on-CPU 占比从 98.09% 降至 97.29%，但完成相同图和尝试边的
累计 on-CPU 增加约 40.67%。任务间空档确实增加，却不足以解释全部成本增长。
下一步应优先比较并发下的 on-CPU 热点、重复恢复/读取和共享资源开销，而不是
只继续增加派发批量。仍不能据此宣称等待不重要，或已经证明某个共享锁是瓶颈。

特别是 WSL 的 on-CPU 是 **guest 调度器视角**：宿主暂停 vCPU 的时间可能不表现
为 guest sched_switch。这里没有 Windows 宿主调度或硬件指令计数证据，不能把
40.67% 全解释为更多指令，也不能排除频率、缓存、内存带宽或虚拟化影响。
这些诊断带探针、每配置一次，不用于无探针吞吐排名或精确加速比承诺。

## WSL perf 与 PID 域

WSL 内核支持软件 task-clock 和调度 tracepoint，但原先没有 perf 可执行文件。
已在实验目录解包 Debian bullseye 的 perf 5.10.223 及缺失库；没有安装系统软件包、
修改宿主 ASLR 或 sysctl。软件计数和实际调度采集均通过。硬件 cycles 的既有探测
仍不可用，本轮没有宣称修复硬件 PMU。

WSL trace 的 header/journal PID 与 payload PID 不同，例如 4501→5100。
直接按 journal PID 筛选时分析器报告 unresolved；原失败产物保留，未解释为零 CPU。
随后对每个 worker 单独建立双向唯一映射：

- 所有 switch-out 的 header TID 与 raw prev_pid 必须始终一致。
- 独立 exec 事件的 pid/old_pid 必须与候选映射一致。
- 必须存在唯一 fork birth，共同父进程也须有独立 switch 映射；出生顺序必须合法。
- 缺证据、冲突、负 header 建候选、时间逆序全部拒绝，没有使用固定偏移量。

13 个 worker 全部通过。只转换分析副本中的 journal/cycle/coordinator 身份，
不改变原始 trace 或 DetSim 模型 identity。Linux v6.18 的
[调度 tracepoint 源码](https://github.com/torvalds/linux/blob/v6.18/include/trace/events/sched.h)
显示 switch 和 exec payload 分别取 task 的 pid；本次映射另外依赖上述逐事件证据，
不据此推广到任意 PID namespace 或非主线程 exec。

区间与映射相关合计 14 项合成测试通过。两台机器的导入状态机及 cycle 分析器
源摘要相同。WSL 五次目标运行（三次无 sched、两次有 sched）全部成功退出，各
namespace 的 proc/dev/sys 卸载均成功；最后复查无本实验进程、无宿主挂载残留，
原冻结 tracer/fixture 与新增工具、脚本摘要已核对。

## 证据与下一步

- 本机原始采集：`build/shared-parallel/worker-cycle-sched-runs/`。
- 阶段分析、原未映射失败结果、映射证据及最终结果：
  `build/shared-parallel/worker-cycle-sched-analysis/`。
- WSL 输入、私有 perf 包清单、探测、原始采集和回收核验：
  `build/shared-parallel/worker-cycle-wsl/`。
- 远端私有工具和数据：
  `/home/asaaoi/exp/detsim-scaling-20261019-3fb4274f/`。

下一项是 WSL 上 1 / 12 workers 的 on-CPU 栈对比，以及用简单路径为 oracle 的
恢复局部性优化。当前没有新的生产优化合入，不宣称已经达到接近线性加速。

## 后续 CPU 栈对比

已用同一诊断二进制补采 WSL cpu-clock 199 Hz、frame-pointer 栈，各一次 1 / 12
workers。两个运行仍通过相同图及零重试；只取 journal 中精确 worker main 的
header PID，并裁剪到 TaskReceived→DoneSendBegin。这里不使用 sched payload
的 PID 映射。原始样本总数与 perf 录制日志一致，日志未见丢失警告。

任务内样本分别 5098 / 7219，kernel leaf 占比 39.45% / 40.68%。完整函数身份
去重后的部分 inclusive 比例如下；各行重叠，不能相加：

| 栈祖先 | 1 worker | 12 workers |
| --- | ---: | ---: |
| `tracee_state::capture_stopped` | 14.50% | 17.23% |
| `save_paged_snapshot` | 8.91% | 10.90% |
| `sys_state::recover_running_state` | 11.22% | 12.45% |
| `verify_stopped_thread_set` | 6.77% | 5.93% |
| `proc_pid_status` | 5.10% | 4.45% |
| `dwarf_get_type_info` | 3.86% | 3.01% |
| `detsim::search::wire::Channel::send` | 1.63% | 0.79% |

内核 exclusive leaf 中，`rep_movs_alternative` 从 3.35% 增至 4.81%，
`clear_page_rep` 从 0.04% 增至 1.12%；出现少量 `rwsem_spin_on_owner` 样本，
但未见调度锁自旋主导的证据。unlock 函数不因名字含 spin 就算作自旋等待。
这约束了下一步方向，但份额变化本身不是绝对耗时、指令量或因果证明。

符号仍有重要限制：26.32% / 26.74% 的 leaf 在已知用户库中但函数未知，其中
libc 占总 active 样本约 23% / 22%。保留原始栈、最近已知用户祖先和无祖先样本，
没有把未知 libc 命名为 memcpy/malloc。FP 栈和缺少库内符号限制精细归因。
五项分析器合成测试通过；原始记录位于 `build/shared-parallel/worker-cpu-wsl/`，
分类、测试和限制位于 `build/shared-parallel/worker-cpu-wsl-analysis/`。
远端两次采集均退出，proc/dev/sys 卸载成功；后续进程、挂载及输入摘要复查通过。

## 局部性查找对照：局部快了，完整搜索没有稳定变快

私有候选只将协调器 roots 查找表从 `std::map` 换为 `std::unordered_map`。
此表只 emplace/at，没有有序遍历；候选窗口与 tie-break 不变，不混入其他改动。
另以显式参数建立 256 窗口的 map/hash 对照，保留生产默认窗口 32 不变。
三份候选均通过实际启用 BFS roots 的 full/pages、durable/shared 同图和 worker loss
重派门禁。随后每个二进制独立建立 Raft native reference，再做正反顺序两轮计时。
12 次运行全部保持 10457 / 18266、18266 尝试边、零重试及相同规范图。

本机 12-worker、full-FP、roots 模式，两轮中位数：

| 配置 | 搜索加延后关闭（秒） | locality selection（秒） |
| --- | ---: | ---: |
| map / 32 | 7.2034 | 0.1414 |
| hash / 32 | 7.1930 | 0.0631 |
| map / 256 | 7.2317 | 0.6543 |
| hash / 256 | 7.2364 | 0.2748 |

选取时间分别下降约 55.4% / 58.0%，完整计时变化仅 -0.14% / +0.07%，逐轮方向
也不一致。减少树查找确实降低局部成本，但本组实验未证明完整探索收益，因此不合入。
选取时间包含 Frontier lease 最终更新，不是仅查找函数时间。搜索加关闭是两个不连续
区间，中间验证不计时，仍沿用之前报告的限制；两轮不足以建立精确稳定收益。

构建与门禁在 `build/shared-parallel/roots-hash-lookup/`，原始计时在
`build/shared-parallel/roots-hash-timing-20261023/`。另通过现有 DSH Web 实例的
DeepSeek-V4.1-Flash 辅助会话编写独立 artifact audit，重新计算全部规范图及 ELF
摘要、核对原始计数并重算中位数；主 agent 审查并重新运行。辅助脚本与核验结果在
`build/shared-parallel/deepseek-helpers/roots-audit/`。它核验已有产物，不替代原生测试。
