# 原生搜索全链路 perf 分析（2026-10-18）

## 结论

串行主要耗在跨进程内存读取、页比较与状态捕获/恢复；pages 并行又新增了明显的
IPC 小包开销。当前协议逐字段发送，且每次读写前无条件 poll，导致每页约12次
收发/就绪检查系统调用。这个开销可以通过缓冲和批量IO解决，不需要先实现脏页追踪。
full 后端另有显著的 Zstd 压缩/解压成本。不能把当前速度解释成模型检验不可避免的上限。

本轮只增加采样/分析工具，没有修改模型检查器的执行、快照或通信实现。

## 实验范围与计数

固定 NP3 二进制 `build/parallel-redesign/tracer-final-np3`，SHA256：
`6380fd7f4b56aa4adb6814986388961702c7ba219bdd48b8b586388430238aec`。
这是一份已归档的并行实现测量基线，不是本轮临时优化构建。

- RedisLabs Raft 库：项目自有 healthy fixture，同一进程内同一 checkpoint，深度4。
  full/pages 各采集原生串行DFS及1/2/4-worker DFS/BFS，共14个CPU采样窗口。
  所有并行运行仍验证71节点/131条边及相同图、深度、phase；结论是 bounded。
- Willemt `examples/raft`：healthy fixture、原固定业务前缀后原生DFS深度6，pages；
  增加一个perf窗口和一次不采样对照。它不是旧版本/旧配置的BFS复测。
- 另对pages的7个搜索窗口记录syscall tracepoint，核对收发次数及同步延迟。
- Python仅准备前缀、启动原生命令和验收结果，不实现搜索。
- `visits/s`、唯一边/s、唯一状态/s不可混用；旧500状态/s未在相同旧ELF、前缀和BFS
  配置上重测，因此本报告不声称已量化从500到当前速度的精确回归比例。

CPU使用 `cpu-clock`、499Hz、DWARF 16KiB调用栈。通过control FIFO启停并等待ACK，
随后按单调时间窗口再过滤；初始化、手动前缀、load和检查不进入窗口。
挂载协调器全部线程及已有目标，future worker/tracee通过事件继承覆盖。
并行命令内部的worker启动、快照传输、存储与清理仍属于计时和采样范围。

默认 `/usr/bin/perf` 是失效的内核版本分发器；本轮使用已安装且实测可用的
`/usr/lib/linux-tools-6.8.0-138/perf`（6.8.12），配合 `sudo -n`。
没有改 ASLR、perf sysctl 或安装依赖。解析kernel符号也用sudo；full部分库内部函数
缺少符号，仍可按libzstd/libxxhash模块归因，不能强行给未知函数命名。

## 串行：瓶颈在读取与比较，不是被测Raft的计算

### RedisLabs Raft，pages

546个窗口内CPU样本，互斥阶段分类：

| 阶段 | CPU样本 |
| --- | ---: |
| 捕获 | 52.93% |
| 恢复/接管 | 31.32% |
| 快照加载 | 1.83% |
| 被测进程/其他继承任务 | 1.47% |
| 后台存储 | 0.92% |
| 执行引擎 | 0.37% |
| 其他/未归因 | 11.17% |

以下是**包含子调用的热点，彼此重叠，不能相加**：process_vm_readv 41.76%、
memcmp 14.65%、页固定/页表遍历13.55%、maps读取解析7.51%。
内核实际复制函数 `rep_movs_alternative` 自身占23.08%，包含在跨进程读取等路径内。

### Willemt `examples/raft`，pages

不采样对照：1084 visits、201 new，6.814516秒，**159.07 visits/s**。
采样运行：同为1084 visits、201 new，7.008643秒，154.67 visits/s。
两次均为model-budget截断，不能称为全部状态空间已经穷尽。它们是fresh launch，
相同fixture/输入与visit计数，不是旧BFS性能对照，也不把全部时间差归因于perf。

不采样运行保存4.461秒、恢复2.030秒，两者合计95.3%的搜索计时。
捕获4389个进程镜像，读取11.506GB；恢复又读取4.241GB，实际写回仅9.31MB。
所有报告的StateStore读取均命中L1，磁盘load为0。因此这里的瓶颈不是磁盘读不到数据。

3436个CPU样本给出的互斥阶段：捕获51.80%、恢复31.90%。包含子调用的热点：
process_vm_readv 40.77%、memcmp 13.71%、页固定/页表遍历13.47%、maps解析5.94%。
与RedisLabs案例一致：page去重/稀疏写回并未消除反复读取和比较整个镜像。

代码位置：`sys_state`构造对全部NP节点调用capture；`page_capture.cpp`先跨进程读取，
再逐页memcmp决定复用；恢复路径也读取现场内存以避免不必要写回。

历史检查发现 `8d58059` 删除过“inactive进程只凭metadata_equal便复用旧hash”的
捷径，改成全部读取后比较。这是明确的实现差异，但旧测试配置和镜像大小也不同，
不能直接把旧500/s到当前159/s的全部差距归因于该提交。恢复旧捷径需要重新证明
其他进程内存及框架写入没有变化，不能仅凭元数据相等。

### full 的额外成本

RedisLabs串行full窗口1122个CPU样本中，**44.56%调用栈落在Zstd模块**，包括压缩与
解压。该包含关系不能与捕获/后台存储阶段百分比相加。full优化除了内存扫描，还应
针对重复压解缩、缓存和对象复用；pages串行该比例仅0.73%。

## 并行：页协议形成了系统调用风暴

RedisLabs pages、2-worker DFS，1976个CPU样本：

| 互斥阶段 | CPU样本 |
| --- | ---: |
| wire IO/编码 | **51.01%** |
| 恢复/接管 | 13.26% |
| bundle导入 | 11.59% |
| 捕获 | 10.43% |
| bundle导出 | 5.72% |

协调器主线程占所有样本34.92%，worker主线程合计62.50%；被测进程/其他继承任务
仅0.46%。1-worker DFS的wire占54.51%，4-worker为46.00%，增加worker并未消除它。
这些比例是跨所有采样任务的CPU占比，不是墙钟时间占比。

另一次syscall事件运行仍产生同样131条边，窗口3.477秒：

| 系统调用 | 进入次数 |
| --- | ---: |
| sendto（send的底层调用） | 371,259 |
| recvfrom（recv的底层调用） | 371,285 |
| poll | 743,938 |
| process_vm_readv | 9,323 |
| fsync | 1,053 |
| fdatasync | 205 |

三项IPC相关合计 **1,486,482次，约11,347次/边**。
其中248,393次send请求不超过16字节，占66.91%；发送数据合计544.30MB（包括Task和Edge）。
没有观察到LOST记录；窗口边界/退出附近少量进入与返回未配对，延迟只统计配对事件。

源码原因在 `src/core/search/protocol.cpp`：

```text
一页发送：poll + send(key 8B)，poll + send(length 8B)，poll + send(page 4KiB)
接收侧：对应的三次 poll + recv
```

标量编码立即调用write_exact，而read_exact/write_exact在每次尝试前都poll。
即使socket已经可读/可写，也会付出这次系统调用；每页至少约12次两端系统调用。
这不是网络带宽不足，而是本地Unix socket协议操作过细。

fsync+fdatasync配对事件累计耗时0.551秒，确实存在同步成本；但这是**多个任务
重叠的累计调用时间**，不能除以墙钟时间当占比。poll累计4.496秒同理包含等待和重叠，
不能说poll占了129%的墙钟时间。syscall trace本身也有明显开销，该运行不用于速度排名。

## 优化顺序

1. **先改IPC粒度。** 有界缓冲（如64KiB）合并标量和多个页；接收侧保留跨字段缓冲。
   先尝试非阻塞收发，仅EAGAIN时poll，同时保留整帧绝对deadline、长度检查及失败poison。
   这直接针对已测出的约半数CPU热点，不需要修改状态语义或引入脏页追踪。
2. **避免重复搬运完整闭包。** 对已验证不可变对象按hash协商缺失集合，保留完整引用
   声明与缺失检查；当前每次发全部对象，即使对端已有它们。
3. **减少捕获/恢复的重复全读。** 先证明受控执行中哪些进程内存未变化，区分框架写入、
   共享映射、线程内核写入及元数据变化，再复用内存页。不能简单恢复旧metadata_equal捷径。
4. **明确持久化等级。** 临时worker崩溃可由协调器重派，不必让它承担与协调器提交相同
   的持久化成本；任何调整都必须保证协调器在发布展开完成前已有完整可用状态。
5. full再优化压解缩/缓存；其CPU样本与pages不同，不能用同一个“存储慢”解释全部问题。

以上是基于采样的优化优先级，不是已经实现或实测的加速比。

## 系统性能报告

### 1. 总览

主机tilnel，12逻辑CPU，裸机，内核6.12；pages采样期间并行采集5个1秒周期。
基线load0.36/0.25/0.21。系统并未整体饱和，不意味着单条串行路径没有CPU瓶颈。

### 2. CPU

同期全机约77–81% idle，iowait约0–2%，无steal。上下文切换最高约10.4万次/秒，
与大量细粒度IPC相吻合；不能单用整机低CPU利用率排除程序本身的瓶颈。

### 3. 内存

31GiB内存，约20–21GiB available，无swap；未见换页或容量压力。

### 4. 磁盘IO

工作盘nvme2n1容量约59%使用；示例区间write await约0.39ms，flush await约0.47–0.48ms，
util约10–22%。没有持续队列饱和；同步小写延迟仍会影响单个任务。

### 5. 网络

物理网卡平均约45.7KiB/s收、204.2KiB/s发，利用率约0.17%。DetSim worker通信是本地
Unix socket，虚拟被测网络不依赖物理网卡吞吐。没有证据指向外部网络瓶颈。

### 6. GPU

没有nvidia-smi；该测试不使用GPU。

### 7. 瓶颈判断

串行是内存捕获/恢复相关的CPU与内核工作，并行pages首先是细粒度IPC。
未发现整机内存、磁盘带宽或网卡饱和导致这次低速。

### 8. 建议

按前述IPC缓冲→对象复用→减少全读→持久化等级顺序做等价性验证和性能对照。

## 证据与复现

本机产物根：`build/perf-search-20261018/`。

- CPU：`pages/run-40ofyx01/`、`full/run-ok5m8w_l/`，各有7个perf.data、窗口、调用栈和摘要。
- syscall：`syscalls-pages/run-ly2mze2u/`；两worker DFS汇总为根目录`syscalls-2dfs-summary.json`。
- Willemt：`willemt/run-an8bmt3v/`，含profile和同ELF不采样对照。
- 系统：vmstat/mpstat/iostat/sar/pidstat/iotop原始文件。
- 原始perf文件和生成store不入Git；每个profile.json保存perf参数与单调时钟边界。

```sh
python3 examples/redisraft/profile_search.py --perf /usr/lib/linux-tools-6.8.0-138/perf --sudo --tracer build/parallel-redesign/tracer-final-np3 --backend pages --output build/perf-search/pages
python3 examples/redisraft/profile_search.py --perf /usr/lib/linux-tools-6.8.0-138/perf --sudo --syscalls --tracer build/parallel-redesign/tracer-final-np3 --backend pages --output build/perf-search/syscalls
python3 examples/redisraft/profile_search_summary.py RUN_DIRECTORY --perf /usr/lib/linux-tools-6.8.0-138/perf --sudo
python3 examples/redisraft/profile_search.py --perf /usr/lib/linux-tools-6.8.0-138/perf --sudo --driver examples/raft/run.py --tracer build/parallel-redesign/tracer-final-np3 --variant healthy --backend pages --depth 6 --output build/perf-search/willemt
```

Willemt汇总传入`healthy-pages`目录；RedisLabs比较传入有results.json的run目录。
CPU摘要的阶段互斥、热点包含关系可重叠；未知/截短调用栈保留为未归因，不作为零成本。
