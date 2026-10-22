# WSL 宿主运行与等待观测（2026-10-23）

本轮补充 Windows / Hyper-V 侧证据，检验“guest 调度器认为 on-CPU，但宿主实际
没有让 vCPU 运行”的可能性。结果不支持把本次扩展损失主要归因于宿主调度等待：
12-worker 搜索窗口内 VM 大部分时间确实在运行。仍不能据此排除 guest 内部等待、
缓存/内存停顿或重复工作，更不能称为已达到硬件极限。

## 固定输入与计时

沿用原 WSL 大规模对照的冻结产品二进制
`90df0e151499e360b9e988d97a1194c042e7f9be8e82f493ad99dd6bc79d183c`，不是最新
缓存优化版本。固定 Raft-library、NP3、pages/shared、BFS depth16、seed1、
rounds100、same-cpu、locality none；引用该二进制既有 native reference。

实际执行顺序：12-worker 无采样、12-worker 采样、1-worker 采样、1-worker 无采样。
所有运行串行，每个配置只有一次；没有更改电源计划、ASLR、内核或宿主服务。

| workers | 无采样搜索命令（秒） | 带采样搜索命令（秒） | 无采样搜索加延后关闭（秒） |
| --- | ---: | ---: | ---: |
| 1 | 165.7418 | 167.6848 | 165.9567 |
| 12 | 18.2258 | 20.6709 | 19.0923 |

无采样命令比约 **9.09×**，搜索加延后关闭比约 **8.69×**。这些是这一次对照，
不是优化后的稳定水平，也不能取代此前多轮约 7.6× 的结果。命令包含完整命令至
提示符，关闭发生在图验证之后；两段合计不等于立即关闭或连续端到端时间。

采样运行相对对照慢约 1.2% / 13.4%。单次、非随机重复不足以将全部差异归给
观测器；差异足以要求将诊断与吞吐排名分开。不能用带采样时间宣称性能提升。

四次都通过 63418 节点、112305 边、112305 尝试边、零重试检查。下载后逐份重算
完整规范图，SHA 均为
`7eba9a268e4fd2bc45bce49b59c02e85389b743c53a9a25214d59dd3569f2630`，输入摘要一致。
native reference 只独立验证节点与首次发现父边，完整带标签图另行比较。

## 采集与对齐

一个常驻 PowerShell 进程通过请求/响应采样，不反复启动解释器。物理 CPU 使用
`Win32_PerfFormattedData_Counters_ProcessorInformation`；虚拟处理器使用
`Win32_PerfRawData_HvStats_HyperVHypervisorVirtualProcessor`。每次保留 guest
单调时钟请求起止、host QPC 查询起止、原始累计值及其各自 base。

先进行了五次空闲采样与有时限的单核负载响应检查：guest CPU0 负载期间对应
VP0 的运行比例为 94%–99%。这证明采集器会响应负载，不证明任意 guest CPU、
Windows 逻辑 CPU 与 VP 编号之间的映射。本次汇总所有 24 个具名 VP，排除 `_Total`。

`Timestamp_Sys100NS` 与 host UTC 观察到约八小时差异，没有硬减时差使用它。
`Timestamp_PerfTime` 则逐行要求落在该次 host QPC 查询区间；由请求/响应边界
推导 provider 采样时刻在 guest 单调时钟上的保守区间。仅保留相邻两点都明确
位于 `Session.send("parallel …")` 起止之间的区间，排除启动、前缀、图验证和关闭。
不确定的边界不当作搜索时间；计数回退、时间逆序、身份变化和 runtime 分量不闭合
明确报告。原始 runtime base 与 QPC 的 epoch 不同，只使用自身差分计算比例。

四项包装器测试和十二项分析测试通过；另实际运行真实 benchmark `--help` 参数
转发及上述四个原生搜索。分析测试覆盖时钟界限、计数回退/零间隔、容量、边界裁剪
及 VP 身份变化。两份实际采样均没有无效区间。

## 宿主结果

以下均为 VM 范围，包含 worker、coordinator、tracees、guest 内核等；不是单独
worker 时间。CPU 占用不等于有效指令执行，仍包含硬件停顿和执行资源竞争。

| 指标 | 1 worker | 12 workers |
| --- | ---: | ---: |
| 完全落在搜索内的区间数 | 126 | 15 |
| 覆盖搜索时间（秒） | 167.2088 | 20.6057 |
| 搜索时间覆盖率 | 99.716% | 99.687% |
| VM 运行 VP 数中位数 | 1.1047 | 11.7434 |
| 按区间时长加权的 VM 运行 VP 数 | 1.1074 | 11.1249 |
| 覆盖区间累计 VM 运行量（VP·秒） | 185.1745 | 229.2366 |
| 累计 VP 调度等待（秒） | 2.1681 | 1.5727 |
| 按调度次数加权的等待（纳秒/次） | 1699.0 | 1889.2 |
| host 忙碌 CPU 加权性能指标中位数 | 108.772% | 105.735% |

中位数和按时长加权平均数不同，不能只报前者掩盖尾部利用率下降。两份采样覆盖
近乎整个命令，累计 VM 运行量增加约 23.8%；不能将其全部归到 worker，也不能与
以前不同规模、不同探针的累计 worker on-CPU 数字相加。

性能指标按 Windows 物理逻辑 CPU 的 `% Processor Time` 加权，仅作为宿主上下文，
不是按 worker 身份测得的精确频率。provider 帮助说明它表示相对标称性能，并提醒
某些处理器自主调频不一定被 Windows 准确反映。此处从约 108.77% 降至 105.73%，
约 2.8%，没有足够证据把全部额外运行成本归因于降频。

## 等待值的单位核验

本机 provider 将 `CPUWaitTimePerDispatch` 声明为 `PERF_PRECISION_100NS_TIMER`
（CounterType 542573824），其帮助将显示值定义为每次 VP 调度的平均等待纳秒数。
按 [Microsoft 的计数器换算规则](https://learn.microsoft.com/en-us/windows/win32/perfctrs/calculating-counter-values)，
使用 `100 × Δnumerator / Δown_base`，不是直接相除。累计等待由该显示值乘以
同一个 base 的增量推得，再换为秒；各 VP 的等待可重叠，不是可直接从墙钟耗时
扣除的时间。runtime 比例亦按其声明的 precision timer 类型计算。

`LogicalProcessorDispatchesPersec` 的原始累计值与等待 base 在少量捕获中相差
一次事件（汇总行可累加差异）；不能假定不同字段原子一致。保留这些差异，换算
始终使用各自声明的 base，不用另一个字段替换，也没有将缺数据填零。

这项等待仅描述 VP 等待被调度到宿主逻辑处理器，不能涵盖 guest futex/IO 阻塞、
锁自旋、缓存未命中或内存停顿。当前结果限制了“宿主长期不运行 vCPU”这一解释，
并未证明所有等待已经消除。接下来更值得验证减少缓存副本、复制和重复工作的
收益，同时保留对调度尾部与观测干扰的检查。

## 证据与回收

脚本、provider 帮助/元数据、原始采样、phase marker、图核验、四个运行的命令与
退出码均在 `build/shared-parallel/host-cpu-sampling/`。`analyze.py` 做保守裁剪，
`summarize.py` 核对 CounterType 后汇总，`dispatch-crosscheck.json` 保留字段差异。
`aggregate.json` 与 `complete-evidence/` 可复算表格。十二项 namespace 卸载全部
成功；后验检查没有本实验所属进程或宿主挂载，脚本与冻结二进制摘要已核对。
采集器每次成功结束，未进行强杀路径验证。本轮没有产品代码改动。
