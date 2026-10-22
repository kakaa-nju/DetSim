# SnapshotView 缓存索引实验（2026-10-23）

私有候选为至多 256 项的 SnapshotView LRU 增加 hash→list iterator 索引，避免
`open` / `load_metadata` 线性扫描。保持既有锁、generation/poison 检查、完整与
metadata-only 条目区分、LRU 次序及容量限制；增加的索引内存有界，但未计入原有
4 MiB payload 预算。产品源码尚未修改。

原有 page snapshot 集成测试与新增缓存专项测试在基线、候选上均通过，专项覆盖
冷热命中、条目/字节容量淘汰、metadata/full 替换、generation 重开和并发填充。
候选另通过七项原生 full/pages 图、durable/shared、worker loss、retry、启动超时、
中断和违规重放场景，以及独立 Raft native reference。

本机 6 核 12 线程，固定 NP3 Raft-library、pages/shared、BFS depth12、seed1、
rounds100、same-cpu、locality none；完整 O3 full-FP 对象一致，仅替换
page_snapshot.o。基线包含已合入的类型化 shared-pool 缓存键，SHA 为
`ef808de53b2db36fcf644a2ac4e4ec9231a467702e8bc3f88f191cea95cc75c7`；
候选 SHA 为 `3c078e181a380efa2b84715520baa5235e8fb220b923b06507dac67ba9c59253`。

三轮交错顺序测量，单位秒：

| workers | 基线三次 | 候选三次 | 基线中位数 | 候选中位数 |
| --- | --- | --- | ---: | ---: |
| 1 | 32.4568 / 31.8180 / 33.3189 | 31.3255 / 32.0233 / 32.6385 | 32.4568 | 32.0233 |
| 12 | 7.4264 / 7.0742 / 7.3368 | 7.1023 / 7.0934 / 7.7356 | 7.3368 | 7.1023 |

口径为搜索命令加验证之后的延后关闭，两段不连续；不含中间图验证，不能当作
立即关闭或连续端到端时间。所有 12 次计时通过 10457 节点、18266 边、18266
尝试边、零重试；逐份重新计算规范图 SHA 均为
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
native reference 的节点/首次发现父边投影与完整图比较仍是不同证据层级。

中位数略好，但 12-worker 逐轮方向不一致，第三轮候选反而慢约 5.4%。这不是
稳定吞吐收益或扩展性改善的证据；暂不推广到产品代码。尚无此候选的 WSL 计时。
组件/原生门禁、独立参考、构建对象摘要与全部计时保存在
`build/shared-parallel/view-cache-index/`；辅助实现会话已归档。
