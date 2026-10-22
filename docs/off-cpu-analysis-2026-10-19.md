# Worker 等待时间调查（2026-10-19）

此前on-CPU采样及整棵进程树CPU计数不能排除worker等待。此次在原6核12线程
Linux机器补采调度事件，确认等待是扩展性损失的一部分；WSL没有采这组数据，
不能把比例直接套到gpunode。

## 设置与结果

使用相同冻结NP3 tracer、Raft库fixture、插件，BFS/pages/shared、same-cpu、
locality none、depth12、seed1。1/8/12-worker各采一次，均通过10457节点、
18266边、零重试的完整同图校验。perf FIFO只在原生parallel命令窗口启用，
全CPU记录switch/wakeup/new/fork/exit/exec事件，再按所有权和journal worker PID
筛选；没有采CPU栈来代替off-CPU时间。计时诊断不纳入无探针吞吐排名。

仅统计journal worker_spawn对应的主线程，排除被控程序和后台I/O线程。
每个主线程从首次sched-in至最后sched-out，观测区间包含bootstrap/cleanup，
不含fork至首次运行的时间。按全部主线程的观测lifetime求和加权：

| workers | on-CPU | 阻塞至唤醒 | 可运行但未获CPU |
| ---: | ---: | ---: | ---: |
| 1 | 95.37% | 3.93% | 0.70% |
| 8 | 87.97% | 8.42% | 3.61% |
| 12 | 68.36% | 17.19% | 14.45% |

12-worker主线程共有76.7370线程秒：运行52.4601秒、阻塞13.1917秒、
可运行等待11.0853秒。约31.64%的主线程观测时间不在CPU上；其中17.19%
是确实未唤醒的阻塞，14.45%是仍可运行或已唤醒后的等待。

这些是并发线程的累计时间，不是搜索wall time，不能把累计13.19秒说成一次
6.6秒搜索中的13.19秒串行等待。on-CPU仍包含内核操作、可能归于当前task的
中断时间及执行资源停顿，不等于有效用户计算。R/R+合并的still-runnable类别
不能全部叫强制抢占，R也可包含主动yield等情况。

## 重建与验证

- CPU区间必须有同CPU的sched-in/out配对；exec不重置，exit不提前截断最后out。
- R/R+从out到下次in算可运行等待。其他状态用首次有效wake分成blocked与
  wake-to-in；重复wake不覆盖首次wake。
- 丢wake、缺边、CPU不符等进入unresolved/异常，不补成零时间或假定一直sleep。
- 21个worker-main的区间可完整分解，零unresolved/异常、零边界裁剪。这里的
  完整只针对观测lifetime，不宣称kernel绝无整对事件丢失。
- perf在退出任务部分事件header中显示TGID/TID=-1，但switch payload PID有效；
  按payload保留事件，不能丢整行。缺失header不提供额外TGID所有权证据。
- 10个合成回归通过：sleep/wake、runnable、重复wake、缺边/CPU不符、裁剪、
  exec/exit、负header及主线程/helper/tracee角色分离。另对一个真实worker独立
  扫描14136对事件，CPU/sleep/runnable逐纳秒吻合。源码和结果经独立审查。

perf6.8的summary-only输出未保留本次退出worker的完整历史；逐事件timehist
仍含全部记录，不使用不完整summary推断CPU很少。latency的runtime为0也不
作为事实：本次没有record sched_stat_runtime。工具行为没有在此定位为已证实
的版本bug。原始事件、工具输出和分析器保存在`build/shared-parallel/sched-wait/`。
事件解释参考本机perf-sched手册及[Linux perf sched源码](https://github.com/torvalds/linux/blob/v6.8/tools/perf/builtin-sched.c)。

## 单独 syscall 诊断：任务等待值得优先调查

另采一次12-worker sched＋raw_syscalls，仍通过同图。它输出约1.27GB perf数据，
追踪开销较高且记录了1条out-of-order警告，解码后所分析的scheduler流未出现
逆序或未分类sleep。仅用于识别等待位置，不与上表百分比拼接，也不用于速度排名。
对worker-main的switch-out至首次wake按当时系统调用分类：

| syscall | 已分类阻塞线程秒 | 占该次已分类阻塞 |
| --- | ---: | ---: |
| poll | 9.3427 | 64.8% |
| openat | 2.7001 | 18.7% |
| wait4 | 1.2137 | 8.4% |
| futex | 0.4941 | 3.4% |

其余主要是linkat/unlink/fsync等。poll阻塞全部为nfds=1、timeout=-1，
与worker.cpp的控制通道idle poll一致：此时worker不持有lease，等待Task或
Shutdown；源码与冻结版本的worker.cpp相同。常见pidfd存活检查是timeout=0，
不能与这部分无限等待混淆。此定位指向任务供应及阶段等待，并不证明通信带宽
不足或协调器CPU饱和。

下一步应在等待区间关联frontier是否有可派任务：有任务却未派发，调查coordinator
处理/IO/调度延迟；没有可派任务，调查BFS层屏障、尾部和任务粒度不均。
阻塞与runnable等待应分别处理，并与同图下单位CPU成本膨胀共同解释加速比。
之前仅据总CPU占用排除显著worker等待的推断应撤回。

后续的[任务供应与搜索顺序实验](dispatch-scaling-2026-10-23.md)比较了两台主机的
BFS/DFS，并记录协调器可见的可派任务与空闲槽位；该统计不等同于真实off-CPU时间。

## 系统背景与范围

按sysperf连续采样5秒系统背景，CPU/磁盘/内存/网络记录在system-baseline。
基础样本没有swap活动；这不是整个实验窗口没有背景竞争的证明。scheduler
trace是全系统采集，可进一步核对非本实验任务，不能把全部ready等待都归给
框架自身。生产源代码未改；本次没有恢复暂停的优化任务。
