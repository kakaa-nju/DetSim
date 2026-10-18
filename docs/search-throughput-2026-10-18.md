# 原生状态空间搜索吞吐改进与验收（2026-10-18）

调度等待的因果对照、串行导入成本和原生/并行同步粒度差异，见后续的
[并行低加速比专项调查](parallel-scaling-investigation-2026-10-19.md)。

## 验收口径与当前结论

本轮目标包含 `examples/raft` 单实例原生搜索超过 500 states/s，以及并行搜索流水线优化。
这里把单实例目标明确为 tracer 原生 DFS 报告的 **visits/s**；不把实际转换次数、唯一状态数、
页数或缓存命中次数改名为 states 来抬高结果。

当前代码对应的 immutable `tracer-coverage-np3`（SHA256 `4d908a2e…`）在 Willemt Raft
healthy/pages/depth6 三次达到 **632.21、629.15、620.20 visits/s**，中位 **629.15 visits/s**。
每次是 **387 visits、201 new nodes、497 actual transitions**；按同一中位耗时0.615120秒，
新增唯一状态吞吐为 **326.77 new nodes/s**，实际转换吞吐为 **807.97 transitions/s**。
因此验收中的500阈值是历史计划采用的visits计数，**尚未达到500个新增唯一状态/s**，不能把
“超过500 states/s”泛化为唯一状态吞吐。业务检查、恢复重放和独立Raft投影审计通过。

历史固定执行量版本 ffd52 的中位为584.85 visits/s（1084 visits、201 new、1463 transitions）；
当前DFS coverage修复消除了部分重复执行，79种投影边集合保持，而执行多重集合法变化。
这只支持明确负载与模型配置的结论，不代表任意程序都超过500，也不代表完整状态空间穷尽；
该案例仍以模型预算截断结束。全量读取oracle在前述protected版本单独验证。

最终并行版本的 correctness gates 已通过。三次交错重复的 on 路径性能矩阵也已完成，
但共享对象版本相对 buffered 基线出现了回退：pages 慢约 7.5%–35.9%，full 慢约 38.8%–90.5%。
目前不能宣称 memfd 路径提高吞吐。同一 binary 的 on/off 诊断同样显示 on 更慢，
因此最终默认使用 buffered raw transport，memfd 仅保留 `DETSIM_SHARED_OBJECTS=on` 实验开关。
历史pre-coverage默认binary（ffd52）的fresh验收已通过：对最初未缓冲版本，pages 并行中位搜索耗时降低约
18%–36%；full 约持平，部分配置略慢，不宣称 full 获得普遍提升。

## 可重复实验安排

`examples/raft/benchmark_search.py` 复用 `run.model_case`，保留原有健康案例的完整业务前缀、
三个节点提交两个命令的检查，以及最后业务转换的三次同源恢复重放。每次前缀均为相同的
131 个 `(node, choice)` 动作；Python 不实现搜索。

- 计时只取 tracer 的 `dfs 6 --quiet` 原生自动搜索报告。
- 编译、手动前缀、进程启动、检查及重放不进入单实例吞吐分母。
- 同一次比较只构建一份 fixture、checker、choose，供所有 immutable tracer 和重复使用。
- 不重复使用同一会话的 DFS visited/expanded 缓存冒充重复运行；每个计时样本新启动。
- 不同 tracer ELF 进入模型/兼容性身份，不能绕过校验直接导入另一个版本的 seed。
- 复用已有 benchmark seed interposer，固定 `DETSIM_BENCH_SEED=1`。这与先前 159 visits/s
  报告的 time-based host search seed 不同，不能将所有差异归为产品修改。
- 独立 audit run 加载 `benchmark_observer.cpp`，复用案例已有 Raft 投影实现，记录完整实际转换
  回调多重集，包括前后 Raft 投影、时钟、process/thread/choice/syscall/结果。audit 的额外开销
  不进入性能排名。
- 原始旧 binary 没有实际转换计数，单独报告 observer 计数；新 binary 的 `SEARCH_WORK.transitions`
  必须与各自版本的 audit 实际回调数一致：历史固定工作量版本1463，当前coverage版本497。

投影保留角色、任期、投票、提交/应用位置、日志内容、snapshot/base/count、复制进度和时钟。
它不是完整 native memory 图的等价证明；隐藏内存、完整内核状态及任意运行时的不确定性仍需
各自的恢复/控制回归。当前审计没有启发式清除未知字节，也没有放宽存储 hash。

## 历史pre-coverage固定工作量单实例数据

下表均为三次不采样自动搜索；各行工作量相同，为 1084 visits / 201 new。

| 版本 | 三次搜索秒数 | 三次 visits/s | 中位 visits/s |
| --- | --- | --- | ---: |
| 原 immutable 基线，固定 seed profile | 12.667266 / 11.489421 / 10.902745 | 85.57 / 94.35 / 99.42 | 94.35 |
| Resident MVP | 3.109803 / 3.160654 / 3.273525 | 348.58 / 342.97 / 331.14 | 342.97 |
| Protected + hint（中间版本） | 1.850446 / 1.881551 / 1.934934 | 585.80 / 576.12 / 560.23 | 576.12 |
| 历史pre-coverage默认 buffered（ffd52） | 1.851618 / 1.853473 / 1.880395 | 585.43 / 584.85 / 576.47 | 584.85 |

上表各历史版本独立审计均为 1463 次实际转换、79 条唯一 Raft 投影边；**投影边多重集文件逐字节相等**，
SHA256 为 `90ae3b21df812bbf3984711089a507e9f8438b280bee070a94e43ff5f69d0eff`。
上表具备计数器的版本每个计时run原生实际转换数同样为1463。不能将 protected 的 756–791
actual transitions/s 当成另一个“states/s”口径。

### 内存读取变化

以下取同配置的一次完整搜索，单位为字节，包含全部 NP3 进程捕获。

| 指标 | 固定 profile 基线 | MVP | Protected 默认 on |
| --- | ---: | ---: | ---: |
| capture 次数 | 4389 | 4389 | 4389 |
| capture 读量 | 11,595,386,880 | 3,865,128,960 | 545,312,768 |
| resident capture 次数 | 不适用 | 2926 | 2926 |
| protected 复用字节 | 不适用 | 不适用 | 3,319,816,192 |
| restore 读量 | 4,253,491,200 | 7,925,760 | 7,925,760 |
| restore 写量范围 | 8,781,824–11,124,736 | 9,273,344–9,334,784 | 8,302,592–10,969,088 |

没有减少搜索动作来获得这些结果。优化针对受控执行下可证明仍与保存镜像一致的 resident 内存，
以及允许复用的受保护映射；一般未跟踪写入不能据此假定不存在。

另以 `DETSIM_RESIDENT_MEMORY=verify` 执行相同真实 healthy/pages/depth6 案例：
全读 oracle 校验 **11,595,386,880 字节**，protected 路径实际复用 **3,319,816,192 字节**，
业务和三次重放通过。该次额外 oracle 开销的 8.634229 秒不纳入性能门槛。

### 为什么固定 profile 基线比旧报告更慢

先前不采样结果为 6.814516 秒、159.07 visits/s。只读核对保存产物发现：

- 同 tracer/checker hash、同库和 fixture 源码、同编译 flags、同 131 步业务前缀。
- ELF 文件 98,352 → 98,376 字节，text/data/bss 尺寸完全相同：34,826 / 712 / 640。
- seed.so 带来每节点 20 KiB：640 → 645 页；region 从 19 → 25，含额外 VMA 分拆。
- maps 文本 1809 → 2521 字节；capture 读量仅约增加 0.78%，restore 读量约增加 0.28%。
- 两者 StateStore 配置均为 32/64 MiB，报告 100% L1 命中、零磁盘 load。

这些事实排除了“大幅增加镜像字节、缓存 miss 或工作量膨胀”作为解释；更多 region 会增加
系统调用和解析工作，但当前证据不足以将全部 1.6–1.85 倍差异归给它。没有补做同 profile
配对采样之前，不下确定因果结论。本轮仍按新的固定 profile 验收，不降低 500 的门槛。

## MVP 剩余 CPU 热点

在相同 fixed-seed benchmark 中采集 CPU-clock 499 Hz、DWARF 16 KiB 栈。首个自动 DFS
窗口共 1608 个样本，未知叶符号 28（1.74%），未观察到 LOST 或解码错误。

互斥分类包含 capture 的系统状态构造器，避免把构造器里的 `prepare_thread_pool` 误记为 restore：

| 阶段 | CPU 样本占比 |
| --- | ---: |
| capture，含系统状态构造 | 52.43% |
| restore/adoption | 21.21% |
| snapshot load | 4.60% |
| execution engine | 1.87% |
| tracee/other inherited | 1.55% |
| background storage | 0.68% |
| other/unattributed | 17.66% |

capture 内部互斥拆分，占全部 CPU 样本：活动进程内存读 17.72%、memcmp 8.21%、线程池验证
6.22%、resident publish 3.86%、maps 解析 3.42%、输出序列化 1.74%、其他 capture 11.26%。
100 个 prepare_thread_pool 样本全部来自系统 capture 构造器，且无 capture_stopped frame；
单凭这些栈不能标定具体 inactive 节点，不能把它写成精确的 inactive 节点计数。

可重叠 inclusive 热点：proc stop checks 12.00%、page 访问 10.20%、resident publish 7.84%、
分配/释放 7.21%、get_thread_list 5.60%、load_shared 5.97%。这些比例不能相加，也不是墙钟时间
分解。该结果支持优先减少重复验证和页对象查找，序列化不是最大剩余热点。

## 共享对象协议与修复记录

并行原生协议仍由 coordinator 统一 frontier/lease/去重，worker 展开一个保存状态。
共享对象优化不改变 snapshot key、动作或性质定义：

1. 控制 stream 发送完整对象声明；V2 通过独立 Unix SOCK_SEQPACKET 传送 sealed memfd。
2. 对象段有固定尺寸和对象数边界，验证文件类型、seal、长度、连续范围和 raw key。
3. 命中提示只用于传输缓存；接收端缺失/eviction 时明确请求重传。未发布缓存不能当作可用对象。
4. SegmentAck 只允许释放在途段；完整 bundle 经现有 import 验证和持久化屏障后才发 CommitAck。
5. 大对象使用 V1 raw fallback；控制 stream read-ahead 由双方 buffered-input 检查处理。
6. worker import 后 ACK 再 adopt；coordinator import 并核对性质标志后 ACK，收到 Done 才提交展开。

传输资源预算为 memfd cache 64 MiB / 128 段、单在途段最多 8 MiB、confirmed hints 最多
16K keys。这不是整个 worker 的内存上限：owning Bundle、StateStore cache 和 resident page
owners 另外占用内存。Resident 保留每个节点有限的当前/先前镜像 owner，规模为
O(N × image)，不随搜索深度增长，但并非零开销，也不计入 StateStore 的 LRU 预算。
统计中的 `raw_fallback_frames` 同时包含默认有意选择的 V1，不等于异常退回或发生错误。

独立审查发现过真实阻断：新对象段代码误用 XXH64，而 StateStore 的 `XXHash64::hash` 实际为
XXH3_64bits；两个 transport fixture 也误用 XXH64，因此单组件绿灯掩盖了不兼容。
实现和 fixture 已改为调用生产 hash helper，另补真实 full/pages + 外部 VFS bundle 经 V2 冷/热
传输、import、fresh process 读取的验收。不能把传输层假对象测试当作状态闭包验证。

## 当前验证证据与未完成事项

根 agent 的 `build/search-throughput/final-np1-gates.log`：make check、37 项原生场景
（含同 hash 调试器内存修改 full/pages 回归），以及 adoption、parallel actors、page snapshot、
state IO、mapping、xstate、thread lifecycle、pending 门禁通过。
NP2 的 bundle、capture、protocol 门禁通过。

最终默认 off 的 immutable NP2 又分别以 unset 和显式 on 跑过全部两组 10 项测试，
每种模式 full/pages 的 12 行均为 complete 16 nodes / 24 edges，typed graph/depth 一致；
失联重派、取消、重放和整树清理均通过。最终 summary 为
`build/search-throughput/default-final-correctness-summary.json`：unset 610 个 V1 / 0 V2 / 0 memfd，
on 610 个 V2 / 0 V1。最新 `default-final-np3-gates.log`、`default-final-np2-gates.log`、
`default-final-np1-gates.log` 分别确认 protocol/object、真实 bundle、make check 门禁通过。

最终共享版本实际并行 correctness：`shared-final-parallel.log` 的 10 项测试通过；full/pages
各自 1/2/4-worker × DFS/BFS 得到相同 16 nodes / 24 edges，包含深度、终点和动作信息。
还覆盖 depth0、lease/edge 后杀 worker 重派、retry0 失败、启动 deadline、SIGINT 后 CLI 可用、
origin 拒绝、性质违反三次重放及父死整树回收。这些并发执行过 build，只作为正确性证据，不作性能比较。

该 correctness 批次统计 610 个 V2 frame、raw fallback 0；coordinator 可观察引用约 961.7 MB、
missing 约 275.8 MB，最大缓存 19,755,856 字节 / 12 段，最大在途段 4,952,034 字节。
所有记录的 worker PID 已消失。数据不是整棵树双向总量，也不是零拷贝或一般线性加速证明。

## 并行共享 on 路径：正确性通过，但吞吐回退

已完成 12 个独立会话，共 72 行：pages/full × buffered/protected-shared-on × 3 repeats ×
DFS/BFS × 1/2/4 workers。每个 backend 复用其历史 buffered fixture ELF，插件和 seed shim 也逐项
核对原 hash；两个版本按轮交错执行，没有生产构建或其他 benchmark 并行。

全部为相同深度 4 的 71 nodes / 131 edges，status=bounded。同一 seed 会话内，各策略/worker 数
的原 hash 图、depth/phase 完全一致；与内建串行保存树比较全部新节点，并保留 first-parent 边
子集证据，不把 first-parent tree 冒充串行全部边。

跨版本/启动的原始 state hash 包含不同 origin，未直接比较。另对已记录的完整 journal 图按
确定的 action/choice 标签从 root 规范编号，保留 choice_count/result_code/violation 及节点
深度/phase。72 行的有标签图结构均相同，signature 为
`91d0aba2ba4467ad5a80284633d00bc8bf7e5cedec387de42fcdddf7399fa10c`。
这证明已观测的确定性有标签图同构；不证明任意隐藏内存的语义等价。该分析只读产物，不执行目标搜索。

三次中位原生并行 command 秒数（含 worker 启动、传输、存储和清理）：

| 后端 | 策略 | workers | buffered | protected + shared on | 后者/前者 |
| --- | --- | ---: | ---: | ---: | ---: |
| pages | DFS | 1 | 2.1095 | 2.5163 | 1.193 |
| pages | DFS | 2 | 1.6416 | 2.0832 | 1.269 |
| pages | DFS | 4 | 1.4382 | 1.9542 | 1.359 |
| pages | BFS | 1 | 2.1117 | 2.7117 | 1.284 |
| pages | BFS | 2 | 1.7756 | 2.1833 | 1.230 |
| pages | BFS | 4 | 1.7451 | 1.8756 | 1.075 |
| full | DFS | 1 | 3.5180 | 5.0777 | 1.443 |
| full | DFS | 2 | 2.9055 | 4.0328 | 1.388 |
| full | DFS | 4 | 2.8624 | 5.4520 | 1.905 |
| full | BFS | 1 | 4.0239 | 5.7275 | 1.423 |
| full | BFS | 2 | 3.1374 | 5.1987 | 1.657 |
| full | BFS | 4 | 2.9919 | 5.3782 | 1.798 |

因此，减少 socket 小包不等于端到端提速。代码审查中的候选成本包括：仅请求旧 segment 的
少量 row 却重新 hash 整段、owning Bundle 重复复制、逐 frame CommitAck 同步等待。这些仍是
待隔离的解释，不能从上表直接推定每项占比。

后续 syscall 采样工具已补 sendmsg/recvmsg，避免漏掉 SCM_RIGHTS 路径；这两种 syscall 的
返回字节只表示控制包 payload，不包括通过 FD 映射的对象字节。当前未把新路径的缺失计数
与旧 sendto/recvfrom/poll 合计作误导比较。

同 binary `tracer-transport-switch-np3` 的 `DETSIM_SHARED_OBJECTS=on/off` 首轮 24 行均通过。
该轮可能含早段编译重叠，全部作为诊断，不纳入最终性能重复。pages 和 full 的每个
strategy/worker 配置都表现为 off 更快；例如 full DFS2 为 on 3.897 秒、off 2.865 秒。
结合前述三次重复回退，最终默认使用 buffered raw transport；共享对象路径保持 opt-in，
不以省 socket 调用数代替全链路收益。此决定保留完整 import/closure 校验，不删除验证换速度。

## 历史pre-coverage默认路径：与最初未缓冲版本的同期对照

另以最初 immutable `6380fd7f...` 与历史pre-coverage默认 `ffd52b11...` 交错执行三轮，两 backend 均复用
同一 fixture/plugins，合计另 72 行。整个窗口无生产构建和其他 benchmark 并行。
全部仍为 bounded 71 nodes / 131 edges；同 seed 原 hash 图相等，跨运行有标签图同构 signature
仍为 `91d0aba2...`。每个最终版本 channel 的统计均确认 V1、零 memfd、零共享 cache。

三次中位原生并行搜索秒数：

| 后端 | 策略 | workers | 最初未缓冲 | 最终默认 | 最终唯一边/s | 耗时比 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| pages | DFS | 1 | 2.7486 | 2.2470 | 58.30 | 0.818 |
| pages | DFS | 2 | 2.1880 | 1.6543 | 79.19 | 0.756 |
| pages | DFS | 4 | 2.2306 | 1.4508 | 90.29 | 0.650 |
| pages | BFS | 1 | 2.7752 | 1.9254 | 68.04 | 0.694 |
| pages | BFS | 2 | 2.3911 | 1.8358 | 71.36 | 0.768 |
| pages | BFS | 4 | 2.7465 | 1.7633 | 74.29 | 0.642 |
| full | DFS | 1 | 3.6711 | 3.7517 | 34.92 | 1.022 |
| full | DFS | 2 | 2.9661 | 2.9092 | 45.03 | 0.981 |
| full | DFS | 4 | 2.8981 | 2.8932 | 45.28 | 0.998 |
| full | BFS | 1 | 3.8341 | 3.8943 | 33.64 | 1.016 |
| full | BFS | 2 | 3.1465 | 3.1110 | 42.11 | 0.989 |
| full | BFS | 4 | 2.9894 | 3.1451 | 41.65 | 1.052 |

因此 pages 有稳定的“修改前后”收益；full 的 -1.9% 至 +5.2% 范围不支持普遍改善的 claim。
这与“增加 worker 的收益”是两个口径。以最终版本同一并行框架的 1-worker 中位时间为基准：

| 后端/策略 | 2 workers 加速比 | 4 workers 加速比 |
| --- | ---: | ---: |
| pages DFS | 1.36× | 1.55× |
| pages BFS | 1.05× | 1.09× |
| full DFS | 1.29× | 1.30× |
| full BFS | 1.25× | 1.24× |

不是四倍线性加速，也**没有证明并行模式比原生单实例 serial 更快**。本次小规模 pages 负载的
原生 serial DFS 报告约 0.278 秒，比带 worker 启动、闭包传输/import、恢复和清理的并行命令更短。
serial 回执只有 first-parent 边投影及节点覆盖证据，不能据此给出精确同图的并行/serial 加速比；
也不能将并行框架 1-worker 称为原生 serial。
这些唯一边/s 与前面的 Willemt visits/s 是不同工作负载、不同计数，不可直接排名。

历史pre-coverage默认binary（ffd52）单实例完成三次不采样和独立 audit：1084 visits / 201 new / 原生 1463
transitions 均不变，三次超过 500，投影多重集文件与最初基线逐字节一致。
默认路径 syscall 采样也通过全部图验收。pages / 2-worker DFS / depth4 的窗口计数为：

| 系统调用 | 原未缓冲 trace | 最终默认 trace |
| --- | ---: | ---: |
| sendto | 371,259 | 8,929 |
| recvfrom | 371,285 | 13,121 |
| poll | 743,938 | 7,120 |
| sendmsg / recvmsg | 旧 trace 未采集 | 0 / 0 |
| 所选 send/recv/poll 系统调用合计 | 1,486,482 | 29,170 |

所选同名系统调用集合的计数下降 98.04%，不是全部 IPC 或全部 syscall 的下降。poll 还包含
resident 存活检查等非 IPC 用途；只有 Channel 自身计数器直接统计 wire IO。确认默认没有 memfd；新 trace 已启用 sendmsg/recvmsg，因此零计数不是
遗漏事件。两次均为相同 71/131 的受限搜索，旧 trace 的 fixture 为独立构建，不以该计数表单独
推断全部性能变化原因。最终 fsync=1053、fdatasync=205，持久化屏障没有被删去；process_vm_readv
为6509次。最终窗口 LOST=0、未配对 entry=0。插桩秒数不进入吞吐排名；多进程累计 syscall
持续时间可能重叠，不能当墙钟占比。

## 更大规模：历史pilot与测量设计

71 nodes / 131 edges 足以验证小图正确性和发现协议回退，不足以证明生产规模的并行扩展性。
用户随后要求放大案例。保持同一业务前缀和真实 seed model budget=2，没有修改 snapshot
字节或预算语义，已取得以下 1-worker DFS pilot：

| depth | nodes | unique / attempted edges | 完整命令秒数 | 状态与口径 |
| --- | ---: | ---: | ---: | --- |
| 6 | 297 | 507 / 507 | 8.055 | serial先行预热；采样诊断；local oracle通过 |
| 8 | 1138 | 1934 / 1934 | 32.185 | serial先行预热；采样诊断；local oracle通过 |
| 12 | 10457 | 18266 / 18266 | 336.048 | parallel先行冷coordinator；采样+20秒perf；后置local serial oracle最终通过 |

depth8 最终节点分类 expanded413 / model_budget381 / depth_cut344；1194 leases 中有781次
zero-emission Done（65.4%）。这些叶子目前仍需要闭包传输、import、adopt/restore后才分类，
是实际额外负担。采样平均约1.036个CPU核，sumRSS峰110.3MB、sumPSS峰89.8MB。
RSS共享页会重复计入，PSS单列；这些采样值是观测下界，不是每个worker的独占内存承诺。

大规模主口径将使用 **command_seconds**：覆盖原生命令最终逐node journal和返回prompt，
也包含driver polling小开销。PARALLEL_RESULT.seconds在最终node journal之前取值，只并列
报告，不能充当含尾部输出的完整流水线耗时。正式三次计时不开资源sampler/perf；另做诊断
记录CPU/RSS/PSS与近似首lease启动时间。

正式矩阵将逐配置独立fresh Session，以 `--parallel-first` 在serial之前计时，避免把冷1worker
与后续已预热的2/4/8直接比较。每depth保留一次完整local serial witness，后续通过显式
`--reference-report` 校验同输入、同budget、同业务前缀投影及完整有标签图同构，报告
`shared_reference; not run locally`，不冒充本地运行了serial。

原生DFS自身有额外重复：任一successor耗尽model budget会令frame.complete=false，只有
complete frame才record_expanded，因此即使actor/choice已跑完，该source也可能沿其他路径
重新执行。depth8原生serial执行14698次转换，而parallel仅1934次，不能直接拿它们的秒数
称作同工作量扩展比。native UI的unique又不包括未push的budget叶；parallel会记录这些target。
因此不能用native UI unique除parallel节点总数估算完成比例。

depth12 后置local serial oracle最终完成，整会话1640.500秒、exit0。它耗时1301.790秒，
执行1,010,975次实际转换、663695 visits、6187 native unique；完整保存树新增10454个节点
与parallel新增集合一致，first-parent边包含验证通过。它比parallel的18266次执行多约55倍，
因此不能把1302/336称为多核加速；这是不同重复执行量、且serial在parallel之后预热的诊断。
该pilot的7258/11082个Done没有产生转换；coordinator端收发合计约77.9GB。

当前depth12诊断运行中20秒CPU采样有9905个样本，unknown leaf756（7.63%，其中707在
libxxhash）。将系统capture构造器的131个样本归回capture后，互斥占比为：bundle import31.60%、
wire IO24.25%、restore/adoption19.43%、bundle export12.87%、capture4.41%、其他3.81%。
coordinator main46.56%、worker main52.71%。它表明该规模的主要成本转向搬运、验证、恢复；
不是完整命令的墙钟分解，不丢弃未知符号。候选包括coordinator纯metadata cutoff、Task间
保留resident Scope、成功dispatch后poll0，均尚未实施或验证。

上述为旧版本诊断，不能当作正式重复计时。当前版本的24行正式矩阵见下一节；timeout或未完成
oracle的run仍明确保留incomplete状态，不纳入已验收同图性能结果。

## DFS coverage 修复后的大规模正式验证

上面的 ffd52 批次之后，原生 DFS 修复了按 `(state, remaining_depth)` 缓存覆盖结果及重复
cutoff 的行为。当前 immutable `tracer-coverage-np3` SHA256 为
`4d908a2ede8d996242b69a3b2a0646631c1630cd1c18973980e320c9b0005362`。
本节只比较这一binary内的数据，不混合旧版本计算加速比。

Willemt固定输入的三次原生搜索为0.612141、0.615120、0.623989秒，每次387 visits、
201 new、497 actual transitions。中位629.15 visits/s，最慢仍有620.20 visits/s。
独立audit为497 callbacks、79种投影边；79种边集合与旧baseline相等，重复次数下降，
旧1463次audit的多重集不再相等。健康业务及三次恢复重放通过，结果在
`build/raft-search-benchmark/coverage-final/run-urxdwn9u/`。

大图正式批次采用pages、depth12、seed budget2，各配置独立冷Session，关闭sampler/perf。
**24/24行全部通过**，每行10457节点、18266唯一边且恰18266实际边尝试、0 retries。
完整有标签图、节点depth/phase一致，图签名为
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
计时采用包含逐node journal和返回prompt的command_seconds。

| 策略 | workers | 三次command秒 | 中位秒 | 对同框架1-worker中位加速 |
| --- | ---: | --- | ---: | ---: |
| DFS | 1 | 326.693 / 628.252 / 613.756 | 613.756 | 1.000× |
| DFS | 2 | 240.833 / 515.617 / 509.259 | 509.259 | 1.205× |
| DFS | 4 | 205.687 / 487.388 / 478.141 | 478.141 | 1.284× |
| DFS | 8 | 205.255 / 489.166 / 562.235 | 489.166 | 1.255× |
| BFS | 1 | 368.845 / 596.095 / 583.575 | 583.575 | 1.000× |
| BFS | 2 | 486.826 / 499.995 / 514.592 | 499.995 | 1.167× |
| BFS | 4 | 471.115 / 479.355 / 465.731 | 471.115 | 1.239× |
| BFS | 8 | 479.368 / 474.244 / 480.574 | 479.368 | 1.217× |

首轮DFS各配置显著快于后两轮，时间漂移不可忽略。BFS首轮2/4/8-worker相对1-worker均变慢，
第二与第三轮则加速，方向并不稳定。DFS第三轮8-worker比4-worker慢17.6%；不能把4→8统一
概括为持平，更不能从中位数宣称线性扩展。所有原始样本保留；正式结果与逐轮配对比值位于
`build/search-throughput/large-formal/summary.json`，原始路径见同目录manifest。

参考行先冷parallel，再本地native oracle：后者35.282秒、27247 actual transitions、
20365 visits、6187 native unique，新增保存节点10454与parallel相等。其他行显式使用
`shared_reference`，校验同版本、同输入、同budget及完整有标签图同构，没有冒充各行都跑过serial。
另一个独立冷native会话已取得原生34.241422秒（command34.246329），594.75 visits/s；
其随后预热parallel已完成，整份结果passed=true、exit0，新增节点集合相等及first-parent边包含检查通过；
该预热parallel计时不进入冷矩阵。
**当前并行框架远慢于原生单实例**，上表加速仅相对并行框架自己的1-worker。原生执行27247次、
parallel执行18266次且原生只记录first-parent边投影，因此不是同逐边执行工作量的速度比。

01:34:27–31的低频主机检查见 `build/search-throughput/host-check-20261019/`：
12核83.53% idle、5.53% iowait；coordinator约32% CPU、两个worker约31%与28%；
可用内存约20.55GiB、swap0。NVMe写吞吐仅1.2–1.7MiB/s，但215–222次flush/s，
flush await2.09–2.40ms、util53–59%。这提示同步持久化延迟值得检查，不证明整批因果。
owned进程affinity允许0–11核，各层cgroup没有CPU quota或throttle；这不等于实际CPU利用率。

离线system-root局部性统计见 `large-formal/locality.json`：首轮DFS2/4/8首次在该worker观察到的
source比例31.21%/57.44%/71.93%，BFS为33.96%/61.48%/72.73%。单worker除首seed未知外
都已本地观察到。它不是blob缺失率、LRU命中率或实际写盘量，不能单独解释DFS/BFS差别。
BFS各行10457 leases中6633个Done没有转换，但仍传输、导入和恢复；coordinator收发约76.3GB。
所有worker先完整import/adopt/recover，随后才在enumerate_state中按metadata分类；这是可验证的
后续优化候选，不是在当前性能测量中已实施的优化。独立BFS8诊断已同图通过，记录在
`build/search-throughput/large-diagnostic-bfs8/run-02fm62h5/`。它带sampler/perf，command238.064秒
不进入正式排名；它比正式重复更快也进一步说明时间漂移。owned树观察到382.9 CPU秒，
平均1.608核；峰值PSS503,218,176B，RSS618,840,064B（包含共享重复），峰值36进程。
首lease约0.209秒是采样观察值，不是精确bootstrap时长，CPU与内存峰值均为采样下界。

独立20秒CPU窗口记录18190样本，record/decode无丢失警告，未知leaf1263（6.94%）保留。
coordinator main占29.85%、worker main合计69.04%；这不是coordinator利用率已满的证据。
修正capture构造器归类后的互斥CPU样本占比：bundle import25.50%、wire22.94%、
restore/adoption21.54%、export13.10%、capture8.42%、其他5.94%。inclusive memcpy12.70%、
memcmp6.39%、process_vm_readv5.96%、hash5.90%，相互重叠，不能相加作墙钟分解。

首个syscall窗口为尝试按调用栈区分屏障而启用DWARF，导致约3.4GB采样及明显丢失：
perf stderr报告3466 lost chunks、38.57% samples lost。虽然文本中没有字面LOST行，不能称无丢失。
原始TID配对有851 unmatched exits、527 overwritten enters、17 pending；该窗口明确标为失败，
不用于定量屏障延迟结论。另一次独立无栈轻量录制已完成且全图验收通过：
`build/search-throughput/large-diagnostic-bfs8-light/run-hbcyidac/`。ringbuffer为4096页，
只采selected syscall tracepoints；record/decode均无丢失警告。461489个enter中461472个完成配对，
按TID及syscall名称配对；17个开始时无enter的exit、17个结束pending均保留，各约0.0037%，
没有覆盖的enter。未采集调用栈，不声称精确区分各屏障callsite。配置采样20秒，首末事件
时间跨度约20.067秒；record命令总耗时22.107秒还包含启动和退出。已配对调用的累计时长如下：

| 角色 | fsync次数 | fsync累计秒 | fdatasync次数 | fdatasync累计秒 |
| --- | ---: | ---: | ---: | ---: |
| coordinator | 10335 | 6.1926 | 2067 | 2.0388 |
| 全部workers | 3475 | 2.7409 | 695 | 0.9936 |

两类角色fsync:fdatasync恰5:1，与源码的4个blob屏障fsync及metadata的fdatasync+目录fsync
相符；这是聚合与代码的对应，不能替代缺失的逐callsite观测。worker poll累计124.43秒、futex
151.54秒包含并发等待与后台线程空闲；这些时长重叠，不能相加后当成墙钟占比。poll也包含
非IPC用途。该轻量run的command235.646秒仍是诊断数据，不纳入正式三次重复；两次诊断都
明显快于正式后两轮，后续优化需交错对照，不能把当前一次较快run归功于代码改进。

证据支持优先研究减少重复闭包搬运/导入、metadata已知cutoff的提前分类、以及持久化等待。
不能直接删除clean flush：现有故障测试要求无新写时flush仍能报告fsync失败，任何省略都涉及
显式契约修改。当前报告没有实施这些候选，也没有通过减少durability保证来提升数字。

最终生产构建与检查 `make NP=3 -j4 all check CC=/usr/bin/gcc CXX=/usr/bin/g++` 通过，
见 `build/search-throughput/final-post-matrix-gates.log`：9 state、14 build、5 layout检查通过；
重链接tracer的SHA256仍与本节全部实验的4d908版本一致。

## 本机证据索引

- 基线：`build/raft-search-benchmark/baseline/run-fyiw_xbv/`。
- MVP：`build/raft-search-benchmark/resident-mvp/run-fc3ocamc/`。
- Protected 中间版本：`build/raft-search-benchmark/protected-final/run-f5th3rdr/`。
- 历史pre-coverage默认单实例：`build/raft-search-benchmark/default-final/run-pe1iqia7/`。
- 全读 oracle：`build/raft-search-benchmark/protected-verify/run-1sri19cf/`。
- CPU perf：`build/raft-search-benchmark/resident-profile/run-v8qvlphn/`；首窗口
  `timing-0-0/perf-1/`，含原始 perf、时间窗口、栈、普通 summary 和 capture-detail。
- 只读旧基线比较：baseline 目录的 `comparison-to-previous.json` 及上级 `compare_saved_inputs.py`。
- 最终并行 correctness：`build/search-throughput/shared-final-parallel/summary.json`。
- 同 binary on/off 诊断：`build/search-throughput/transport-isolation/manifest.json`。
- 当前coverage单实例：`build/raft-search-benchmark/coverage-final/run-urxdwn9u/`。
- 当前大图24行：`build/search-throughput/large-formal/{manifest,summary,locality}.json`。
- 冷native对照：`build/search-throughput/large-cold-native/run-9it3n8sy/results.json`。
- 大图CPU/资源诊断：`build/search-throughput/large-diagnostic-bfs8/run-02fm62h5/`；首个syscall窗口因丢失作废。
- 可靠无栈syscall诊断：`build/search-throughput/large-diagnostic-bfs8-light/run-hbcyidac/`，含质量及按角色配对摘要。
- 大图pilots：`build/search-throughput/large-pilots/`；depth12 `run-i_mk7cff/perf-large-live/`
  含原始栈和 `summary-corrected.json`，不能用该诊断run做正式速度排名。
- 历史pre-coverage默认syscall：`build/search-throughput/default-final-syscalls/run-d963bdv6/`，
  含 `perf-summary.json`、`dfs2-comparison.json` 和精确工具 hash。
- 历史pre-coverage默认对最初未缓冲基线：`build/search-throughput/final-default-parallel/manifest.json`、`summary.json`。
- 共享 on 对 buffered 的三次交错并行对照：`build/search-throughput/final-parallel/manifest.json`、`summary.json`，
  同目录 `summarize_matrix.py` 保存只读图同构验收算法。

精确 immutable tracer SHA256：

| 用途 | SHA256 |
| --- | --- |
| 原基线 final-np3 | `6380fd7f4b56aa4adb6814986388961702c7ba219bdd48b8b586388430238aec` |
| Resident MVP | `c068753683e8352ba965bfa1286a71a67cf8f196e1b9e44e53c2d2943409cd78` |
| Protected NP3 | `a50047ad0e36800be3b6e7ef74154374732a5ad53a93bd35da07b6729a6eb541` |
| 历史pre-coverage默认 NP3 | `ffd52b111edd900e4a9f148dc7c291b7d1d5cde2ea604e315cb607eb9b52177c` |
| 当前coverage NP3 | `4d908a2ede8d996242b69a3b2a0646631c1630cd1c18973980e320c9b0005362` |
| Buffered 并行对照 | `1933c1ae7dd9167e4250636890d56883f592208f892360568fa7dcec8801ce44` |

每个 benchmark 结果还记录 fixture、plugin、配置与脚本 hash，并保存源码归档和原始日志。
原始 store/perf 产物不入 Git。并行完整性与支持边界继续以 correctness backlog 为准。
