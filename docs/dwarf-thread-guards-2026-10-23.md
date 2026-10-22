# DWARF 调用栈与线程停止校验（2026-10-23）

更完整的调用栈表明，线程停止校验是值得继续优化的路径：它出现在约 19%–20%
的任务内 CPU 样本中，之前 frame-pointer 栈只显示约 6%–7%。这是归因能力改善，
不是代码突然变慢，也不是证明这部分成本可以全部消除。

## 采样链路验证

沿用已有 worker-cycle 诊断二进制
`6d771e15a7cce79dd830abb82dfe054f9303ba5403d0cd4316474fda3956d8af`，NP3、
Raft-library、pages/shared、BFS depth12、seed1、same-cpu、locality none。
它早于类型化 pool cache 优化，不把此次数据当作最新 release 的吞吐排名。

perf 5.10.223 支持 DWARF 展开。先用显式 `-fomit-frame-pointer` 的短程序验证，
297 个样本中 165 个以 `__vfscanf_internal` 为 leaf，全部能还原到自有 parse、work
和 main 调用者。首次解码请求了未记录的 CPU 字段而失败；保留原数据，去掉该字段
后解码通过。正式采集增加 `--sample-cpu`，不靠重新跑负载掩盖该配置错误。

正式采集采用 cpu-clock 99 Hz、`--call-graph dwarf,8192`、`--namespaces`、
`-B -N`，只记录命令及其继承子树，解码使用 `--no-inline` 保留实际栈帧的 DSO。
`-B` 避免此前宿主/chroot 同路径库的错误 Build ID 关联；它不是任意接受库版本：
从原始 perf MMAP2 逐 worker 核对可执行映射的设备号、inode 和完整文件摘要。
实验输入清单、未修改的初始 rootfs 清单以及每次 benchmark 的插件摘要共同覆盖
所有相关映射。首次核验拒绝遗漏的 seed 插件和 `/usr/local/lib` 库，补充已有的
输入证据来源后才通过，没有覆盖或追认原采集清单包含它们。

1/12 workers 分别核对了 24/288 个可执行映射。原始 perf 数据中 SAMPLE 记录数
3012/3821 与文本解析头数一致，未发现 LOST/LOST_SAMPLES 记录，解码日志为空。
这不证明任意未报告的内核丢失都不可能发生。沿用 journal 精确 worker main PID
和 TaskReceived→DoneSendBegin 的半开窗口，排除其他线程、启动和收尾。

两次搜索都通过 10457 节点、18266 边、18266 尝试边、零重试，以及完整规范图
摘要 `56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
参考仍是 native 节点/首次发现父边验证，完整图另外比较。五项解析/窗口测试通过。

## 任务内归因

以下 inclusive 比例相互重叠，不能相加；也不是墙钟时间或调用次数。

| 指标 | 1 worker | 12 workers |
| --- | ---: | ---: |
| 任务内样本数 | 2374 | 3171 |
| 已知用户 DSO 中未解析的 leaf | 81（3.41%） | 115（3.63%） |
| `verify_stopped_thread_set` | 19.76% | 19.11% |
| `require_no_pending_signals` | 11.67% | 11.07% |
| `capture_stopped` | 21.02% | 20.09% |
| `sys_state::recover_running_state` | 17.40% | 18.29% |

停止校验的主要已知上层路径是恢复绑定（218/293 样本）和捕获前准备（173/205），
另有 worker 准备与快照采用路径。尚未证明这些调用重复或不必要。8192 字节栈转储
仍有未知尾部，不能宣称所有调用链完整；不过 libc leaf 的上层归因已明显改善。

## 状态文件描述符复用的前提实验

下一候选先考虑减少 `/proc/<tid>/status` 的重复打开/关闭，保留每次实时读取，
不缓存“没有 pending signal”的结论。尚未实现产品缓存或测得性能收益。

本机 Linux 与 gpunode WSL 分别进行了 15 次原始 `lseek(fd,0)` / `read` 检查，
同一个 fd 与每次新打开的 fd 对照：进程级 SIGUSR1、线程级 SIGUSR2 的 pending
位均能更新，恢复执行并通过 sigwait 清除后也能更新。目标回收后，保留 fd 明确
返回 ESRCH，没有返回旧的合法零位图。fd 打开时设置并验证了 CLOEXEC。

两台机器另在独立 PID/mount namespace 中强制复用 PID 128：旧进程退出后新进程
使用同一数字，且有 pending signal；新 fd 能读到它，旧 fd 仍返回 ESRCH。
只调整该临时 PID namespace 的 ns_last_pid，没有改变宿主 PID 配置。

这些结果支持继续设计，但不证明 C++ 缓冲流 rewind 一定丢弃旧缓冲，也没有覆盖
真实多线程非 leader TID 的缓存绑定。缓存应由受控进程/线程池生命周期持有；旧
descriptor 失败后不能自动按同一数字 PID 重开，否则可能绕过 incarnation 检查。
生命周期关闭、非 leader 线程、失败/回收和实际读取次数都需要后续回归。

采样、映射核验、原始记录与分析在 `build/shared-parallel/dwarf-unwind-wsl/`；
内核前提实验在 `build/shared-parallel/proc-status-reuse/`。六项 namespace 卸载成功，
后验没有本轮 profiler/目标进程或宿主挂载残留。所有原生探针回收其所属子进程。
本轮没有产品代码改动。
