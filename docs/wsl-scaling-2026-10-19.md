# gpunode WSL 上的并行探索对照（2026-10-19）

本次是用户明确授权的独立机器实验。此前通用优化目标仍暂停；未继续推广
capture-layout、rseq 或 GPR 私有候选。被测对象是三节点 RedisLabs Raft
**库驱动**，不运行完整 RedisRaft Redis 模块。

## 环境与运行边界

- gpunode：AMD Ryzen 9 3900X，Windows CIM 与 WSL 均报告12核24线程。
- WSL2 内核：`6.18.54.1-microsoft-standard-WSL2`，Debian 11原用户空间。
- WSL可用内存约9 GiB、总内存约9.7 GiB，swap 3 GiB。
- 数据保存在WSL Linux根文件系统 `/dev/sdd`，不使用 `/mnt/c`。
- 原系统glibc2.31/gcc10，不升级系统、不安装项目依赖。复制现有二进制、
  原动态链接器/共享库、Python3.13和标准库到独立rootfs，796个文件约116.7MB。
  本地及传输后逐文件SHA核对通过。原ELF/插件字节均未修改。
- rootfs在私有mount namespace内绑定proc/dev/sys，sys只读，chroot以
  uid/gid1000执行。每个阶段结束显式卸载并记录返回码；宿主ASLR未改。
- 实测ptrace GET/SETREGS、XSTATE（832字节）、GET_RSEQ_CONFIGURATION、
  process_vm_readv/writev、pidfd_open通过；探针只操作自己fork的子进程。
- root实测software task-clock perf_event_open可用，hardware cycles返回
  ENOENT，sysfs没有CPU/AMD硬件PMU。此次不提供IPC、硬件cache miss结论。
- Windows侧测试前单点CPU负载14%，不是整个窗口的背景负载监控。
  WSL的CPU绑定约束客体vCPU；宿主Windows调度和实际缓存拓扑没有独立控制。

## 固定输入与计时

冻结tracer为NP3、O3、同CPU放置支持版本，SHA256：
`90df0e151499e360b9e988d97a1194c042e7f9be8e82f493ad99dd6bc79d183c`。
fixture SHA256：
`2868f3d6e34daf745bd786af31e0d46b7a22994411d1e6162f9c15b1fe8c71d7`。
choose/check/seed三个插件也使用原机器相同文件；完整闭包SHA与命令保存在
`build/shared-parallel/gpunode-wsl/{manifest,environment,deployment}.json`。
实际探索由C++原生框架执行，Python仅准备前缀、发送命令和校验报告。

共同参数为BFS、pages/shared、seed1、rounds100、same-cpu、locality none。
先固定执行300步，三个节点commit index均为3，再搜索模型预算2的后缀。
每个depth先建立本机独立native DFS参考，再用reference-only全新session测速。
参考包含完整节点及首父边投影核对，不冒充独立的完整边oracle；不同worker
之间额外验证完整带标签规范化图一致。统计为搜索命令加正常关闭，包含尾部
journal及持久化屏障，不含前缀、串行oracle及Python图后处理。这是两个不连续
区间之和：图核对发生在搜索返回与关闭之间，可与内核writeback重叠，因此
不能称为立即关闭或连续端到端wall time。各并发档位使用同一统计口径。
首次参考在parallel之后执行串行oracle，其后关闭不是纯并行关闭成本，不纳入
下面的性能中位数。

## 深度12：两轮、16个全新探索

全部10457节点、18266唯一边、零重试，完整规范化图与原机器相同：
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
第一轮worker数升序，第二轮降序。每档仅两次，不提供统计置信区间。

| workers | 搜索＋关闭中位秒 | 加速比 | 按worker数计算效率 |
| ---: | ---: | ---: | ---: |
| 1 | 24.0486 | 1.000× | 100.0% |
| 2 | 12.9268 | 1.860× | 93.0% |
| 4 | 7.1809 | 3.349× | 83.7% |
| 6 | 5.2452 | 4.585× | 76.4% |
| 8 | 4.3546 | 5.523× | 69.0% |
| 12 | 3.6679 | 6.557× | 54.6% |
| 16 | 3.3739 | 7.128× | 44.5% |
| 24 | 3.2998 | 7.288× | 30.4% |

按客体暴露的core/sibling信息，前12个worker使用不同core标签，之后使用
SMT兄弟。12→24只快约10%，不可把24个逻辑CPU视为24个完整物理核。
同一冻结版本在原6核12线程机器的小图中，8/12-worker加速约4.63/4.92倍。
新机器改善了扩展性，但仍有物理核范围内的损耗；CPU代际、内核、虚拟化和
宿主背景负载同时不同，不能将全部差异归因于核心数量。

原始报告位于该artifact的`results/`；远端保留全部session及快照。
`whole_session_child_cpu_seconds`来自外层wait/rusage，包含前缀、Python后处理
和关闭，只作整个session的诊断，不当作搜索窗口CPU时间。

namespace runner只打印umount返回码，退出码仍取被测脚本，因此单独exit0
不作为卸载成功证据。需要分别核对每个阶段日志的proc/dev/sys三个返回码，
并在全部结束后检查实验所有的进程是否仍存活。原796文件manifest是不可变的
初始staging记录；新增orchestration helper及执行状态另行记录provenance，
不覆盖或追认初始manifest已包含它们。

## 深度16

独立native参考通过；两轮10个fresh reference-only会话全部为63418节点、
112305唯一边、零重试，完整规范化图与原机器相同：
`7eba9a268e4fd2bc45bce49b59c02e85389b743c53a9a25214d59dd3569f2630`。
第一轮1/4/8/12/24，第二轮反序。每档仍仅两次。

| workers | 搜索＋关闭中位秒 | 加速比 | 效率 | 唯一边/秒 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 147.5737 | 1.000× | 100.0% | 761.0 |
| 4 | 41.9418 | 3.519× | 88.0% | 2677.6 |
| 8 | 24.0089 | 6.147× | 76.8% | 4677.6 |
| 12 | 19.4398 | 7.591× | 63.3% | 5777.1 |
| 24 | 17.4418 | 8.461× | 35.3% | 6438.8 |

原6核机器同一冻结版本的depth16结果为：1-worker185.51秒、
8-worker41.62秒（4.46×）、12-worker38.01秒（4.88×）。本机8-worker
为6.15×，12-worker为7.59×，较小图的扩展性有所改善；24-worker仅比
12-worker多约11.5%吞吐。更多物理核有助于当前流水线，但没有消除核数
范围内的损耗，也不能仅凭跨机器比较证明CPU缓存、内核锁或Windows调度
各自的因果贡献。

大图native DFS独立核验实际执行186743次transition，约218.33秒；这是
正确性检查而非冷串行吞吐基线，不纳入矩阵分母。矩阵分母是同一BFS配置、
同一close统计口径的独立1-worker探索。

## 最终核验与清理

26个fresh计时session（小图16、大图10）及两个本机独立native参考全部通过。
本地最终同步的结果JSON及canonical图文件逐一复核，避免使用运行中的中间副本。
远端原796个文件在结束后再次逐一SHA核对；两个新增执行helper也分别核对
本地/远端SHA，记录于`execution-provenance.json`。初始manifest保持原样。

五个阶段共15次显式umount均返回0；结束后root检查未发现任何仍运行的
rootfs内可执行程序，也没有宿主mount namespace中指向该rootfs的挂载。
结束时swap仍为0使用。保留独立rootfs、日志、结果和快照供复查，不删除
既有远端应用或修改系统软件。生产源代码未改；本轮只提交报告及采样归类
更正，之前暂停的通用优化目标保持暂停。
