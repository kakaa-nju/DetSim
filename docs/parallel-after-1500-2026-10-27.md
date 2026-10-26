# 单实例1500阶段对并行的影响（2026-10-27）

相对于单实例约1000/s阶段，本轮同图测量的并行吞吐在1/6/12workers下提高约
35%/24%/18%。绝对吞吐提高，但相对各版本自身1-worker的扩展加速比下降；
不能据此称并行调度已经更接近线性。

## 条件和结果

本机AMD Ryzen5 2600，6物理核/12逻辑CPU，Linux6.12.105+deb13-amd64。
旧版为fc2a31b对应冻结普通O3 NP3程序（SHA256以JSON为准），新版为368c9f9
的1500验收程序。目标、choose/check/seed插件不变：examples/redisraft三节点
RedisLabs Raft C库fixture，非完整Redis服务。seed1、100轮确定性前缀、budget2，
pages/shared、原生parallel BFS depth16、worker-placement=same-cpu。
两版均传入RESIDENT_MEMORY/OBSERVATION_CACHE/CAPTURE_LAYOUT_CACHE=on；旧版没有
后两项实现。具体环境名均带DETSIM_前缀，记录在JSON。

每格两次独立新会话，第二轮反转版本和worker数量顺序，所有实验顺序执行。
表为原生搜索回执seconds的中位数；前缀、参考验证、journal尾处理、Python轮询、
构建和关闭时间不混入搜索耗时。固定完整图63418节点、112305边/尝试边。

| workers | 1000阶段耗时/s | 当前耗时/s | 吞吐提升 | 当前边/s | 旧/新对自身1-worker加速 |
|---:|---:|---:|---:|---:|---:|
| 1 | 151.03 | 111.94 | 34.9% | 1003 | 1.00× / 1.00× |
| 6 | 33.76 | 27.15 | 24.4% | 4136 | 4.47× / 4.12× |
| 12 | 29.13 | 24.74 | 17.8% | 4540 | 5.18× / 4.53× |

1-worker是并行协调器加一个worker，不是单实例原生BFS；边/s也不能直接和原生
BFS累计访问/s混算。固定同图时耗时比等于吞吐比。12workers的新耗时两次分别为
23.31/26.16秒，旧版28.85/29.41秒，波动明显；两次样本不能给出可靠置信区间。
12workers使用SMT，不能当作12个物理核；这些秒数也不能直接与此前WSL机器比较。

## 一致性和审计

12次正式运行均passed、tracer退出0、零重试，无性质违规。每版使用自身经过本地
serial覆盖验证的reference；当前reference本轮新建，旧reference使用保留的已有
证据。正式运行先做parallel，再用reference检查完整图。重新读取全部报告、
canonical图、worker启动journal，核对复制的worker二进制和实际host_cpu列表；
相同数量workers对应相同CPU集合，非tracer输入及磁盘哈希一致。

所有图SHA256均为`7eba9a268e4fd2bc45bce49b59c02e85389b743c53a9a25214d59dd3569f2630`。
这是当前固定界限的完整带标签图一致性，不是任意程序的状态等价证明。
[原始样本、输入哈希、命令与审计](parallel-after-1500-2026-10-27-results.json)。
原始记录位于build/parallel-after-1500/，包括manifest、每次结果/journal/图和冻结新版。

## 结论的范围

新优化作用于worker也会使用的捕获、恢复和观测路径，因此并行绝对吞吐受益。
本轮没有改变调度、IPC或共享存储策略，也没有恢复已归档的线性扩展优化任务。
6workers加速比约4.47→4.12，12workers约5.18→4.53。收益随并发数增加而减小；
当前测量不能单独区分协调器、共享存储、同步与各worker缓存局部性的贡献，需要
新profile才能归因。更早750→1000阶段的18%–22%提升是另一组比较，不与本表相加。
