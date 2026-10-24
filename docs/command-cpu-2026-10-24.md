# 完整 parallel 命令的 CPU 记账（2026-10-24）

同图从 12 增至 24 workers，整个命令的 guest CPU 记账增加约 39%，增长主要
来自回收的 worker/tracee 子进程。协调器自身 CPU 几乎不变，平均不足一个核，
不支持“协调器一直被 CPU 跑满”的说法；也不能排除短时串行瓶颈或阻塞。

## 结果

当前冻结 typed-pool 基线、WSL、Raft-library depth16、pages/shared、BFS、
same-cpu、seed1、100 rounds，12/24 workers 各一次，未运行其他 CPU 实验。

| 完整命令窗口指标 | 12 workers | 24 workers |
| --- | ---: | ---: |
| 命令墙钟，秒 | 17.98197 | 16.36918 |
| 原生 search seconds，另列 | 17.72875 | 16.13908 |
| 协调器自身 CPU 秒（含其线程） | 11.29 | 11.48 |
| 协调器回收子进程的累计 CPU 秒 | 192.79 | 272.09 |
| 原有存活 tracees 的 CPU 增量 | 0 | 0 |
| 合计 guest CPU 秒 | 204.08 | 283.57 |
| 协调器平均核占用 | 0.6279 | 0.7013 |
| 合计平均核占用 | 11.3491 | 17.3234 |

回收子进程 CPU 增加 41.13%，合计增加 38.95%。这包括 worker 启动、执行、
被测进程及收尾，不能把它们全部叫作展开一条边的指令开销。每项一次，没有置信
区间；guest CPU 记账也不是宿主硬件有效执行时间。与前一轮宿主采样趋势一致，
但两个运行/窗口不同，不拼接为精确差分。

## 计数与校准

私有 Python wrapper 仅在 Session.send 的 parallel 命令前后读进程树的
`/proc/PID/stat`。按 SC_CLK_TCK（本机及 WSL 为 100）换算 utime/stime 和
cutime/cstime；后者用于退出并被回收的子树，原有仍存活的 tracees 单独计增量。
命令前后要求存活进程集合及 PID/starttime 一致，计数不能倒退。这个校验拒绝
尚有新存活子进程等不能闭合记账的窗口，而不是把缺失数据补零。

依据 [Linux proc 字段说明](https://www.kernel.org/doc/html/latest/filesystems/proc.html)，
并用实际子进程回收孙进程、父进程再回收子进程的 CPU 负载，对照独立
getrusage(RUSAGE_CHILDREN) 校准，允许少量 tick 量化差异。另覆盖带空格/右括号的
comm、进程身份变化、集合变化、计数倒退、只记录 parallel、失败命令的记录。
六项检查在本机和 WSL 均通过。

初次 wrapper 重算检查因 JSON 将 PID 整数键转换为字符串而失败；统一明细输出
为字符串键后通过，原失败日志保留。没有修改被测进程、tracer 或模型状态。

## 实际运行审计

两次均为 63418 节点、112305 边/尝试边、零重试；输入、配置、完整图和原始
journal 计数核对通过。命令前后恰为同一个协调器和原始三个 tracees，身份一致；
journal 记录的 worker PID 不在结束时的存活集合中。原始 ticks 可重算全部汇总，
读取边界包围命令窗口，error 为 null。两次 namespace 的三个挂载均正常卸载。

这依赖该受控场景的子进程归属及回收语义，不推广到任意逃逸/未被回收的进程树。
CPU 边界读数比 CLI 窗口略宽，可能含边界附近的后台线程工作；不是原生 search
计时器的精确 CPU 区间。不能从聚合 CPU 值倒推出每个 worker 的等待时间。

证据与实现位于 `build/shared-parallel/command-cpu/`，主审计为 `audit.json`。
归档验证后只清理可再生快照与重复 worker 二进制，保留原始统计、图及日志。
后续可针对协调器（例如逐条 journal flush）采样，但其 CPU 占用没有证明该路径
主导整体扩展损失，不据此删减状态恢复或线程校验。
