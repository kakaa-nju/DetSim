# 单实例优化对并行搜索的影响（2026-10-26）

结论：优化是通用热路径优化，并行也受益。本机同图两轮交错测量，1/6/12 workers
吞吐分别提高约20.2%/17.9%/22.2%；12-worker相对本版本1-worker的加速比从5.43×
变为5.51×，扩展性基本不变，不能据此宣称已解决线性扩展问题。

## 范围与口径

- 本机AMD Ryzen5 2600，6物理核/12逻辑CPU，原生Linux6.12；不是此前12核WSL机。
- 优化前：62ea4b3对应冻结普通O3 NP3程序；优化后：fc2a31b的普通O3 NP3程序。
- `examples/redisraft` 三节点Raft C库，非完整Redis模块；同一个目标二进制、
  choose/check/seed插件、seed1、100轮确定性前缀、初始model budget2。
- 原生parallel BFS，pages/shared，depth16；63418节点、112305唯一边及尝试边。
- 两版均用 `--worker-placement same-cpu`：每个worker与其tracees绑定到自己的CPU，
  不是把全部worker绑到CPU2。6-worker为0/3/1/4/2/5，12-worker再用6/9/7/10/8/11。
- 每次独立新会话，parallel-first。先为每版各建一份本地serial参考；正式计时用
  对应版本reference-report，避免每次重复运行serial或混入验证写入/收尾。
- 主指标为原生回执`seconds`。命令尾部journal、Python轮询、前缀、图验证、原生
  serial oracle、构建和关闭耗时分别记录，不混入搜索耗时。

## 同图两轮结果

两轮反转版本顺序和worker顺序，串行执行所有实验。表为两次原生搜索耗时中位数；
“吞吐提升”为旧耗时/新耗时−1，“耗时下降”为1−新耗时/旧耗时。

| workers | 优化前秒 | 优化后秒 | 耗时下降 | 吞吐提升 | 新版边/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 181.50 | 150.94 | 16.8% | **20.2%** | 744 |
| 6 | 39.80 | 33.77 | 15.1% | **17.9%** | 3325 |
| 12 | 33.46 | 27.38 | 18.2% | **22.2%** | 4102 |

这里的1-worker仍是并行框架的协调器加一个worker，不能与原生单实例BFS的
累计访问/s混算。本次固定完整图，所以节点/s和边/s的相对提升相同；绝对数量
分别包含边界节点和完整带标签转换。

| workers | 旧版对自身1-worker加速 | 新版对自身1-worker加速 |
|---:|---:|---:|
| 1 | 1.00× | 1.00× |
| 6 | 4.56× | 4.47× |
| 12 | 5.43× | 5.51× |

六组新旧配对均更快，但每格只有两次，不能当置信区间或硬件上限。不能把此机的
绝对秒数直接和旧WSL数据比较，也不能把12个逻辑CPU当12个物理核。

## 哪些是通用优化

| 改动 | 是否作用于并行 | 原因/范围 |
|---|---|---|
| 映射文本解析 | 是 | capture/restore公共路径，full/pages均使用 |
| 系统元数据一次编码 | 是 | 所有state_transition都会commit_metadata，保留旧哈希/归档字节及冲突检查 |
| StateView持有已验证根 | 是 | 通用插件/断言的保存态读取；不缓存live读或跨store generation结果 |
| status/task目录句柄复用 | 是 | 每个物理carrier池使用；仍逐次读取信号、重置并枚举目录及执行完整屏障 |
| BFS源状态少复制 | 主要单实例BFS | worker原本就以const源调用enumerate_state |
| BFS队列携带预取组件引用 | 主要单实例BFS | worker不走该队列；shared_writes路径也禁用该预取流程 |
| scanner声明修正 | 正确性修复 | 未单独声称吞吐收益 |

调用链依据：`src/core/search/worker.cpp`的enumerate_state，
`src/core/scheduler.cpp`的search_actor_choice，`src/core/state/state_transition.cpp`
的恢复/捕获/插件/commit_metadata。上述产品改动没有Raft名称或Raft专属状态补丁。
吞吐收益依赖具体工作量；大量内存复制占主导或不使用保存态读取的检查器不会
必然得到同样比例。代价仍是pool持有额外FD、StateView生命周期内持有快照根。

## 验证与留存

2次参考加12次计时全部正常退出、零重试。各版本的本地serial提供节点/首次父边
覆盖证据；完整并行带标签图另行比较。重新读取全部14份报告和canonical图文件，
核对图hash、输入、worker实际可执行文件副本、CPU放置、计时和退出状态。
图SHA256均为 `7eba9a268e4fd2bc45bce49b59c02e85389b743c53a9a25214d59dd3569f2630`。
这证明当前案例固定界限下图一致，不等于任意程序的内存/状态等价证明。

[全部数值、输入及报告哈希](parallel-after-serial-2026-10-26-results.json)。
原始材料位于 `build/parallel-after-serial-20261026/`：manifest记录准确命令/退出码，
各run保留配置、journal、canonical图及结果。reference会话的计时不参与上表。

复现框架（每个版本先各生成自己的参考，再执行同样参数的计时）：

```sh
python3 examples/redisraft/parallel_benchmark.py \
  --tracer PATH_TO_FROZEN_TRACER --binary PATH_TO_RAFT_CASE \
  --output PATH_TO_RUNS --backend pages --storage shared \
  --depth 16 --rounds 100 --seed 1 --timeout 1200 \
  --workers 6 --strategies bfs --worker-placement same-cpu --parallel-first
# 后续fresh会话附加 --reference-report PATH_TO_OWN_VERSION_RESULTS_JSON，
# 分别使用 --workers 1 / 6 / 12，并反转版本顺序重复。
```

两份冻结程序均保留：`base-tracer`和`next-tracer`。此次只评估既有改动对并行的影响，
没有恢复归档的“接近线性扩展”优化任务，也没有改变并行调度/通信/存储策略。

## 本轮收尾

修正了parallel_benchmark.py输出中沿用旧口径的timing说明文字，现在明确以原生
receipt.seconds为主指标；计时实现和探索逻辑未改。此次14次运行的冻结driver源码
哈希一致，且已与每份输入记录核对。脚本语法编译和--help通过。

完成审计后，仅删除本轮session的生成memory/sstate及重复worker程序，共218项；
目录由约6.1GiB降为654MiB。保留结果、配置、命令、原始journal、canonical图、
参考树、driver源码和两份顶层冻结程序，结果/图哈希复核未变。旧快照现场需要
重跑才能load，现有图参考仍可用于新的同输入比较。清单与结果在任务目录的
cleanup-inventory.json和cleanup-complete.json，未触碰其他实验树。
