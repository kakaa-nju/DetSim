# 原生主机的周期、指令与缓存事件（2026-10-24）

新一轮硬件计数再次观察到：同图的指令数增加远小于周期数增加。通用缓存事件
比值未随并发显著增加，因此不能把 IPC 下降简单解释成该比值暴涨；它也不能
排除缓存未命中延迟、其他缓存层级、SMT 资源竞争或内核开销。

## 范围与验证

本机 Ryzen 5 2600、6 核 12 线程、Linux 6.12.105，非 gpunode WSL。
沿用旧 product-placement 冻结版本及其已验证参考，tracer SHA256 为
`90df0e151499e360b9e988d97a1194c042e7f9be8e82f493ad99dd6bc79d183c`。
这与近期 WSL typed-pool 基线不是同一版本，不把两台机器的数字拼成比例。

NP3 Raft-library、depth12、pages/shared、BFS、same-cpu、seed1、100 rounds。
按 1/core、1/cache、6/cache、6/core、12/core、12/cache 顺序各执行一次。
六图均为 10457 节点、18266 边/尝试边、零重试，完整图摘要与既有参考一致。
使用 FIFO 在原生 parallel 命令前启用 perf、命令返回后停用，包含该命令的
收尾及最终日志，排除 prefix 和串行参考；继承计数覆盖协调器、初始 tracees、
workers 及其后代，不仅是 worker main。

两个硬件事件组分别为 cycles/instructions 和 cache-references/cache-misses，
均另带 task-clock、上下文切换和迁移计数。六组原始 stat.jsonl 重解析与报告一致，
计数完整、事件 running fraction 均为 1，启停 ACK 时序及原生 receipt 核对通过。
它们是诊断数据，不能拿包含收尾的计数窗口当自动搜索耗时或无插桩吞吐排名。

## 结果

| 指标 | 1 worker | 6 workers | 12 workers |
| --- | ---: | ---: | ---: |
| 指令/尝试边，百万 | 6.7540 | 7.1228 | 7.3760 |
| 周期/尝试边，百万 | 6.5793 | 7.9719 | 10.8213 |
| IPC | 1.02655 | 0.89349 | 0.68162 |
| core 组 CPU 毫秒/尝试边 | 1.8523 | 2.2918 | 3.1258 |
| cache references/尝试边 | 505710 | 529670 | 606245 |
| cache misses/尝试边 | 125389 | 129711 | 148146 |
| misses/references | 24.795% | 24.489% | 24.437% |

1→12 的指令/边约 +9.2%，周期/边约 +64.5%，IPC 约 -33.6%；缓存引用/边
约 +19.9%，未命中/边约 +18.1%，两者比值基本持平。core 和 cache 是不同运行，
没有逐事件同时测量；每项一次，不是置信区间或因果归因。

这些是 perf 通用事件，不应写成普适的 LLC miss rate。Linux v6.12 的
[AMD Zen1 事件映射](https://github.com/torvalds/linux/blob/v6.12/arch/x86/events/amd/core.c)
将通用 references/misses 分别映射到 0xff60/0x0964。没有进一步核验本机内核
所有发行版补丁或测量 L3/DRAM 延迟，不能凭比值不变排除整个内存层级。

## 其他取证与下一步

另一个私有 carrier-accounting 版本只增加调用计数，八项解析器检查、七项原生
检查及 depth12 同图验证通过。在 3824 个远端任务/18266 边中，载体 ensure 为
1793 次，全部无需扩容；clear_child_tid 查询也是 1793 次；prepare 为 66270 次，
verify 为 147598 次。这约束了“无需扩容却重复准备”的频率，未测得这些调用
各自耗时，也不足以支持删除线程停止、pending signal 或继承注册检查。

DeepSeek 辅助会话只整理静态证据，交付后已停止并归档。其跨版本/跨运行精确
分解没有被采信；本报告以本轮原始测量为准。当前没有足够证据支持新的产品改动。
下一步应核实具体缓存层级/延迟事件或同窗口资源竞争，不能把本机的 IPC 变化
直接移植为 WSL 的结论，也不能仅凭这些计数断言已经达到硬件上限。

证据在 `build/shared-parallel/native-cache-counters/`：六份命令、原始 perf 输出、
报告、`audit.json` 与清理清单。可再生 snapshot stores 和重复 worker 二进制
在审计后删除，原始证据保留。载体计数位于 `build/shared-parallel/carrier-accounting/`。
可用 perf 路径为 `/usr/lib/linux-tools-6.8.0-138/perf`；最初尝试的另一旧路径缺少
libperl，未安装额外依赖或修改系统设置。
