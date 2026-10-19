# 共享快照池：并行搜索优化与规模测试（2026-10-19）

本轮目标仍是接近线性的并行加速；以下结果尚未达到这个目标。
优化消除了协调器反复传输、校验、导入完整快照闭包的主要开销，
但不能据此声称一般程序的恢复正确性或无界模型检查已经成立。

## 测试对象与版本

对象是三进程、每进程单线程的 Raft **库驱动**，不是完整 RedisRaft Redis 模块。
先按固定调度执行 300 步，建立一个 leader、三个节点均提交到 index 3，再搜索后缀。
时间选择插件提供两种选择；搜索根的模型预算为 2。
该预算会截断部分选举超时路径；深度界限也不等于协议轮数。

- tracer SHA256：`1fb6b34cad3a0ef6d55fff4dc55223b56f0a4fd467ebc960b33d5c7a3b550783`
- target SHA256：`2868f3d6e34daf745bd786af31e0d46b7a22994411d1e6162f9c15b1fe8c71d7`
- 设置：BFS、pages、shared、seed 1、resident-memory 默认开启，宿主 ASLR 不变。
- 冻结 binary：`build/shared-parallel/tracer-shared-v1-np3`。
- 驱动：`examples/redisraft/parallel_benchmark.py`；探索由 C++ 原生框架执行。
- 宿主：AMD Ryzen 5 2600，6 物理核／12 SMT 逻辑 CPU，约 31 GiB 内存、无 swap，
  Linux `6.12.105+deb13-amd64`，CPU governor 为 `schedutil`。
  8 worker 加协调器超过物理核数；没有固定频率或逐 worker 绑核，
  因而不能将 8-worker 效率差距全部解释为软件串行比例。

共享池中的 blob/Ready 原子发布后供其它进程读取；每个 Ready 发布前验证完整依赖。
协调器通过小型引用和已验证的 metadata/actor 集合管理 frontier，
不在每条边上重新传输完整镜像。普通显式持久化屏障与正常关闭仍同步共享池。
当前契约不包含断电恢复或协调器崩溃后的搜索事务恢复。

## 深度 12：初步扩展性对照

每个 worker 数分别使用全新 Session，先计时并行搜索，再做本地串行核对。
四轮均为 10,457 状态、18,266 唯一边、18,266 次实际边执行、零重试，
完整带标签规范化图一致：
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
无 perf 或资源采样并发。每个配置只有一次，不能当作重复测量的稳定中位数。

| workers | 搜索命令秒 | 正常关闭秒 | 搜索＋关闭秒 | 搜索加速 | 含关闭加速 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 45.257 | 7.127 | 52.384 | 1.000× | 1.000× |
| 2 | 24.260 | 4.321 | 28.581 | 1.866× | 1.833× |
| 4 | 15.085 | 4.321 | 19.406 | 3.000× | 2.699× |
| 8 | 10.012 | 4.322 | 14.334 | 4.520× | 3.654× |

“搜索命令”包含尾部 journal 与提示符返回；原生搜索计时略短。
“关闭”发生在后置覆盖核对之后，因此不能当作没有验证插入时的精确关闭成本。
表中相加明确包含这项实际费用；完整 Session 另包含前缀、验证与结果处理。

8 worker 的搜索效率为 56.5%，含关闭为 45.7%。实际边分配每 worker
2,264–2,310 条，分工不均不足以解释剩余差距。
协调器控制流传输合计约 11.49 MB；此前完整闭包路径约 76 GB。
旧版本同图 8-worker BFS 中位约 479 秒，本版单次约 10 秒；约 48 倍的
跨版本初步改善还需要新版本重复测试，且不能与 4.52 倍的并行加速混淆。

原始清单与结果：`build/shared-parallel/pilot-v1/{manifest,summary}.json`。

## 深度 20：规模测试（整场通过）

全新 Session、相同冻结版本，8 worker；开启进程树 CPU/RSS/PSS 采样，
与上面的无采样 pilot 不构成严格的性能 A/B。

| 项目 | 并行搜索已完成的结果 |
| --- | ---: |
| 唯一状态 | 291,332 |
| 唯一边／实际边执行 | 521,196／521,196 |
| 真正展开状态 | 106,236（36.47%） |
| 模型预算截断状态 | 140,699（48.29%） |
| 深度截断状态 | 44,397（15.24%） |
| 重试／发现性质违反 | 0／0 |
| 搜索命令秒／原生秒 | 299.035／297.891 |
| 正常关闭秒／搜索＋关闭秒 | 109.964／408.999 |
| 唯一状态／搜索命令秒 | 974.2 |
| 实际边／搜索命令秒 | 1,742.9 |
| 搜索采样 CPU 秒 | 2,324.77（平均约 7.77 个逻辑 CPU 的运行时间） |
| 搜索采样峰值 PSS／RSS | 约 1.050／1.167 GB |

峰值是采样下界；RSS 对共享页重复计数，PSS 更接近实际物理内存占用。
资源数字覆盖 tracer、workers 与 tracees，不包含 Python 驱动。
291,332 状态约为 pilot 的 27.9 倍；只对 106,236 状态执行了后继枚举，
其余节点不能作为已展开状态宣传。

并行搜索返回 bounded；后置原生串行覆盖核对和正常关闭均完成，整场 `passed=true`，
tracer exit 0。串行核对执行 957,059 次转换，报告 670,414 次访问、150,633 个
搜索记录中的唯一状态，耗时 1,226.520 秒。新增保存节点集合相等，first-parent
边投影包含检查通过。预算叶不进入原生 DFS 的访问计数，所以该 unique 数不等于
并行图的节点数；原生 DFS 也会以不同剩余深度重复查询状态。
串行保存的是 first-parent 边，只能核对新增节点集合与该边投影包含关系，
不是独立的完整逐边 oracle。没有同规模 1-worker 对照，不能报告大图加速比。
同样没有百万状态完成证据，不能将其外推为百万规模保证。

完整 Session 为 1,668.020 秒（约 27.8 分钟），包含前缀、并行搜索、
后置串行验证、图处理和正常关闭；不能把它作为原生探索吞吐的分母。
正常关闭占搜索＋关闭的 26.9%，原版本在此逐对象 fsync；独立后续改动用
filesystem 级 syncfs 替代用户态循环，尚待测试和测量，不能预先扣除这 110 秒。
本轮完整规范化图 SHA256 为
`30653c6934051b642fcc4aa0af48b72094cd610f59de2dc3f3fae7a26f59ef91`。

后置串行核对还给出成本线索（不是并行 worker 的性能分解）：保存 839.438 秒、
恢复 230.882 秒，合计占其搜索的 87.3%；StateStore load 请求 148,927,530 次，
绝大多数 L1 命中；页捕获遍历约 18.63 亿页、跨进程读约 356.73 GB。
因此缓存命中率高并不代表捕获或调用流水线便宜。后续仍需用优化版本的 perf
分别核对协调器和 worker，不能把这组串行比例直接赋给并行版本。

正常关闭后测得以下占用，均含完整案例的验证产物，不能误作单状态镜像大小：

| 范围 | 文件逻辑字节 | 文件系统实际分配字节 |
| --- | ---: | ---: |
| 共享对象池 | 254,607,000 | 1,316,499,456 |
| memory（含共享池） | 352,709,841 | 1,414,615,040 |
| 系统 metadata | 128,302,906 | 128,311,296 |
| 完整 case（含图、日志、验证记录） | 1,011,975,179 | 2,074,206,208 |

小对象单独建文件带来明显块分配开销，实际分配约为 CAS 文件字节的 5.17 倍；
这些对象包含共享的快照 root、页、VFS 数据及 Ready，不是一状态一份完整内存。
协调器 socket 共发 52,799,292 字节、收 268,770,400 字节，合计约 321.57 MB。
这些只计协调器端，不把 IPC 两端重复相加，也不包含共享文件系统读取字节。

原始结果：`build/shared-parallel/large-v1/run-j1a14_to/results.json`；
增量监控：`build/shared-parallel/large-v1/growth.jsonl`。
监控是在搜索启动后才开始，监控 elapsed 不能代替完整搜索耗时。

## 优化版本的 CPU profile

同一冻结 binary，独立 depth16/BFS/8-worker/pages/shared 会话，199 Hz CPU-clock、
DWARF 调用栈，继承新 worker/tracee；前缀不采样。并行窗口 95,743 个样本完整解码，
record/decode 无 loss 警告。整场 63,418 状态、112,305 条边、零重试，
后置本地串行核对及正常关闭通过。profile 会增加成本，其 64.61 秒命令耗时
不能与无采样配置直接排名。

- 协调器占全体 CPU 样本约 9.92%；其中 68.46% 属于 `observe_descendants`
  的 `/proc` 进程树扫描，即全体样本约 6.79%。这是宿主清理的簿记，非模型控制边界。
- worker 占约 89.22%；inclusive 栈中 Ready 闭包验证/发布占 worker 的 36.71%，
  页相关 24.75%，CAS 21.21%，`load_shared` 7.58%，hash 6.46%，serde 5.45%。
  **这些组重叠，不能相加**；CPU 样本比例也不是墙钟时间比例。
- 发布栈内部主要是 CAS（54.13%）、强制页读取（13.92%）和 seen-set（10.25%）；
  VFS/FileContent 在整个 worker 栈中的 inclusive 比例仅约 0.55%。
- CAS 目录/name 栈约为 worker 的 2.00%，CAS 内核锁约 0.263%，flock 约 0.080%，
  zstd 约 0.515%。当前 CPU 证据不支持优先给目录分片；off-CPU 等待未覆盖，不能
  用这些数字排除所有锁等待。

worker 未解析叶符号约 7.75%（包括已知 xxhash DSO 中无符号的采样），
协调器约 0.66%；调用栈也可能截断。原始数据与分角色 folded 栈位于
`build/shared-parallel/profile-v1/run-4t0nikue/session/perf-1/`；
摘要 `summary-role.json`，细分 `closure-detail.json`。

据此后续改动移除了调度循环中的进程树扫描，只在 Hello/terminate 扫描；
最终 subreaper 回收仍覆盖未观察到的孤儿。未配置父死保护的宿主辅助进程若先孤儿化，
可能延迟到本次 parallel 结束才清理；coordinator 被杀时受支持 tracee 的
PDEATHSIG/EXITKILL 保证未改变。这里没有承诺任意无保护宿主进程随 coordinator 死亡。
该改动和批量同步的新门禁已通过：15 个 NP2 原生并行场景（含 post-Hello setsid
孤儿回收）、40 个 NP1 原生场景、实际 actor/线程接管、CAS/Ready/严格 Bundle/协议
组件、`make check`、纯池 ASan/UBSan 和 NP1/2/3 生产构建。跨设备屏障是模拟 st_dev
覆盖，不宣称真实挂载实验。日志在 `build/shared-parallel/*syncfs*.log` 与
`np2-phase-inventory-gates.log`，保留的辅助进程证据在 `late-helper-evidence/`。
独立性能对照仍在进行。

## v2：批量同步＋移除热路径进程扫描

提交 `8db95db`；冻结 tracer SHA256
`2f4cc9fe7fe94f02d43627cfc2d46beeb89cc064e86fc53eaae3b5cb8dffc8ec`。
depth12、BFS、全新 Session、无 profiler/资源采样；每配置单次。
首个 1-worker 会话做本地串行核对，其余会话复用该已验证参考并比较**完整带标签图**，
避免每次重复独立串行验证。所有行保留 10,457 状态／18,266 边、零重试，
规范图摘要与 v1 相同。来源：`build/shared-parallel/pilot-v2/summary.json`。

| workers | 搜索命令秒 | 正常关闭秒 | 搜索＋关闭秒 | 含关闭加速 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 43.926 | 0.114 | 44.040 | 1.000× |
| 2 | 22.700 | 0.164 | 22.864 | 1.926× |
| 4 | 13.631 | 0.214 | 13.845 | 3.181× |
| 6 | 11.028 | 0.214 | 11.242 | 3.918× |
| 8 | 9.376 | 0.214 | 9.590 | 4.592× |

持久化屏障没有删除或变成异步退出；关闭耗时下降来自去除逐文件同步循环。
但搜索本身的 8-worker 单次仅从 10.012 到 9.376 秒，说明删除扫描虽有效，
也没有解决所有扩展性损失。全链路 4-worker 效率约 79.5%、8-worker 约 57.4%，
尚不支持接近线性扩展到 8 worker 的结论；6 物理核及宿主其它负载也需要控制。
未重复测量，且参考核对方式不同，暂不把完整 Session 总耗时做前后速度比。

## v3：有界 tracee-root 闭包证明

`SharedPublisher` 绑定具体共享 Pool 实例，worker loop 保活 Pool，并且不可复制/移动。
最多缓存 1024 个已经完整验证、发布的 tracee-root key，不缓存可变 child/actor 图。
新系统状态的命中 root 仍重新解码 metadata、计算 actor；只省去完整页/依赖遍历。
证明与 LRU 只在 Ready 成功发布后更新；cap0/stateless 和 miss/eviction 是完整路径。
该优化不改进程内存捕获、不增加脏页追踪，也不弱化 v1 Bundle 成员检查或持久化屏障。

实际 linker wrapper 测试确认命中新 system key 时页访问/blob 发布均为零，避免误测
已有 whole-system Ready 的捷径；冷 miss、cap1 淘汰、发布失败不 seed/promote、新上下文/
同路径新 Pool、性质/退出状态重新推导 actors 全通过。另验证混合 `[A,B]→[A,C]`
比 stateless 少实际调用且 Ready bytes 相同，暖 proof 遇到 pool poison 必须拒绝。
NP2 15 原生并行场景、NP1 actual actors/adoption、严格 v1 Bundle、`make check` 和
NP1/2/3 生产构建通过；独立源码审查未发现 P0/P1。
日志：`publisher-components.log`、`publisher-review-gates.log`、
`publisher-native-np{1,2}.log`、`publisher-make-check.log`，均在 `build/shared-parallel/`。
独立五行 pilot 已完成，冻结 tracer SHA256：
`8ff9c82eb9c34f9b5f7886c43169be629df75a95cc96d124add9a2d155834b18`。
与 v2 相同冷启动/参考方式，均为同一规范图、10,457 状态／18,266 条边、零重试。
来源：`build/shared-parallel/pilot-v3/summary.json`。

| workers | 搜索命令秒 | 正常关闭秒 | 搜索＋关闭秒 | 含关闭加速 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 31.085 | 0.064 | 31.149 | 1.000× |
| 2 | 17.338 | 0.214 | 17.553 | 1.775× |
| 4 | 10.756 | 0.264 | 11.020 | 2.827× |
| 6 | 8.874 | 0.214 | 9.089 | 3.427× |
| 8 | 7.555 | 0.264 | 7.820 | 3.983× |

各 worker 数的绝对速度都提高，但 1-worker 获益更多，因而相对并行加速比反而下降。
不能通过改用 v2 的较慢 1-worker 分母把这个结果包装成接近线性。
这仍是单次 pilot；后续应交错重复测量，且要在大图上复核。

离线读取该 case 的系统 metadata（含前缀/后置核对，共 10,755 个唯一记录），
三个进程分别只有 775／396／368 个不同 tracee root，共 1,539 个。
此处按 `serialize.cpp` 的 `ar(ts_hash, status, ...)` 顺序，从 schema header 后的
offset12 读取三个 u64；先前草稿误从 offset24 读取，已纠正。
这说明不同系统状态确实大量复用 root，也说明每 worker 的 1,024 项缓存仍可能
淘汰和重复验证。尚不能据此量化跨 worker 重复验证的因果比例。
下一步研究导入 Task 的 Ready 是否可以直接为本 worker 建立相同根的闭包证明，
以复用已经完成的全局验证，而非盲目扩大缓存或减慢单 worker 基线。

## v4：复用导入 Task 的 Ready

`SharedPublisher::import_bundle` 先完成原有共享引用验证、可选 child materialization
和本地系统 metadata 登记，全部成功后才记入 root proof。Ready 是同一私有池中
已经完成闭包发布的证明，因此不新增磁盘证书格式或全局 registry。
这仍是保存字节的证明，不证明当前 live memory，也不省略物理恢复。

full/pages 组件覆盖成功种根、cap0/eviction、模型/引用/未 Ready/不同 Pool 拒绝、
materialization 缺少绑定、真实 metadata save 调用的 false 注入及暖 pool poison；
确认失败不提前种根/改变 LRU，actor 仍按当前系统状态计算。
组件、严格 v1、NP2 15 原生场景、NP1 actual actors/adoption、`make check`、
NP1/2/3 构建及独立实现审查通过。日志前缀为 `publisher-import-`。

冻结 tracer SHA256：
`264e357ce61d6ee2fe6d82bce4c06561bc17f65c3ab1f4b72167e6174596d216`。
同配置五行单次 pilot 全部保持同图、状态/边数和零重试；
来源：`build/shared-parallel/pilot-v4/summary.json`。

| workers | 搜索命令秒 | 搜索＋关闭秒 | 含关闭加速 |
| ---: | ---: | ---: | ---: |
| 1 | 32.090 | 32.154 | 1.000× |
| 2 | 17.044 | 17.258 | 1.863× |
| 4 | 10.741 | 10.956 | 2.935× |
| 6 | 8.433 | 8.648 | 3.718× |
| 8 | 7.634 | 7.849 | 4.096× |

与 v3 的差别很小，且没有重复样本；目前不声称该改动带来显著性能收益。
它也没有让 8-worker 接近线性。正在用 CPU/指令/周期与迁移计数进一步区分
额外工作、SMT/缓存和调度成本；测量窗口仅原生 parallel 命令，不混入前缀或后置串行核对。

## v4 进一步诊断：CPU 成本与重复验证

`perf stat` 的四个 fresh BFS 会话均通过完整参考图核对，五个事件均为单 PMU 且
报告 100% running。每条边是相同的实际 18,266 次转换，包含 kernel/user 指令。
每个配置只测一次；这些是计数诊断，不把带仪器的耗时用于无采样性能排名。

| workers | CPU ms/边 | 指令/边（百万） | IPC | 迁移/边 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.936 | 6.896 | 1.021 | 0.401 |
| 4 | 2.500 | 7.471 | 0.851 | 0.685 |
| 6 | 2.884 | 7.762 | 0.777 | 1.149 |
| 8 | 3.182 | 8.013 | 0.729 | 1.164 |

8-worker 每边多约 16.2% 指令、CPU 时间多约 64.4%，并伴随 IPC 下降约 28.6%。
这些观测不能单独确定缓存或 SMT 的因果份额。
一次额外对照只在启动 tracer 前限制其继承 CPU mask 为 0..5，即每物理核一个
硬件线程，控制器立即恢复原 mask；不更改全机 SMT/调频。实际 mask 逐行验证，
不同 bootstrap profile 的新图与原参考完整图同构，不声称任意内存等价。

| workers | 限定 mask 后 CPU ms/边 | IPC | 指令/边（百万） |
| ---: | ---: | ---: | ---: |
| 1 | 1.953 | 1.037 | 6.915 |
| 4 | 2.162 | 0.997 | 7.513 |
| 6 | 2.364 | 0.943 | 7.780 |
| 8 | 2.484 | 0.924 | 8.005 |

指令量变化小于 0.6%，而高并发时单位边 CPU 成本下降。可用逻辑 CPU 同时减少，
因此不能把 CPU 成本下降约 22% 说成墙钟也改善 22%。原始事件 runtime、报告的运行
比例及失败证据（首轮 verbose 输出混入 JSON，修脚本后重跑）均保留在
`build/shared-parallel/stat-v4/`，摘要为 `core-summary.json` 与 `physical-core-summary.json`。

为验证软件重复工作，另建仅增加累计计数的诊断 binary，不改产品或协议。
每个 worker 在 Done 前记录 root 验证/page 访问，全部 3,824 个 remote Done 的
lease/source/emitted 与 journal 逐条相符，6,633 个协调器本地 Done 排除。
1/8 worker 都通过同图、节点/边数和零重试检查。

| 指标 | 1 worker | 8 workers |
| --- | ---: | ---: |
| root 完整验证次数 | 1,439 | 4,284 |
| snapshot page 访问次数 | 933,911 | 2,780,316 |
| 每边完整验证次数 | 0.07878 | 0.23453 |

每次验证恰访问 649 页，总量增加约 2.977×；最后四分之一仍约 2×，
因此不能只按一次性启动开销解释。page 访问不是物理读或唯一页计数。
没有逐 root 身份/LRU 原因追踪，不能将全部额外指令都归因于这些验证。
来源：`build/shared-parallel/root-count-v4/{report.md,summary.json,build-manifest.json}`。

## 跨 worker 闭包证明候选（v5，性能复核中）

上述重复验证计数促成了同池共享 root 证书的候选实现。证书使用已有 Ready 类型，
与 guest 可写内容的 Blob 类型隔离；记录包含独立 domain、版本、NP、root 和完整
model identity，读取时要求逐字节一致。仅框架认可的 bootstrap-v1 模型启用。
完整闭包验证、依赖发布、SystemReady 发布成功后才能建立 root 证书；导入则必须先
完成已有 Ready 校验与元数据登记。局部失败留下的有效不可变对象不会删除回滚。
证书命中仍重新读取元数据并推导当前系统的 actors/property/status。

cap0、无上下文接口与传统完整 bundle 保留完整校验路径。旧池没有可选证书时，
只读导入仍应成功；这条回归实际先失败（误尝试写只读池）再修复通过，未吞掉
I/O 或 poison 错误。全量/pages 组件、传统 bundle 组件、NP2 的 15 个 native parallel 场景、
NP1 actors/adoption、make check 与 NP3 生产构建已通过；独立静态审查通过。
首次未采样同图性能对照已完成，尚需交错重复测量，当前不宣称显著收益。
每个不同 root/model 多一个小文件，首次并发 miss 仍可重复验证，需量化其代价。

证据：`build/shared-parallel/global-root-proof-{components,native-np2,readonly-red}.log`。
这是私有不可变池内的框架校验复用，不构成不可信生产者的认证方案。

v5 首轮每并发数一个冷会话，均为 10,457 节点、18,266 条边、零重试，完整图
SHA 与前版相同；第一行还通过 native serial 节点集/首父边投影检查。

| workers | 搜索秒 | 含收尾秒 | 同版含收尾加速比 |
| ---: | ---: | ---: | ---: |
| 1 | 32.0395 | 32.1033 | 1.000 |
| 2 | 17.5372 | 17.6513 | 1.819 |
| 4 | 10.9721 | 11.2364 | 2.857 |
| 6 | 8.3907 | 8.6049 | 3.731 |
| 8 | 7.4279 | 7.7422 | 4.147 |

8-worker 含收尾比 v4 单次快约 1.4%，4-worker 反而慢；单次数据不足以认定
小幅差异为收益。候选尚未提交，是否保留取决于重复对照，而非组件通过。
来源：`build/shared-parallel/pilot-v5/summary.json`。

## 四页读取缓存（产品回归通过）

`PagedMemory` 的单页缓存改为受同一 mutex 保护的四槽 FIFO。加载成功并校验长度后
才替换，命中仍检查引用长度，返回的 OwnedSpan 保留独立 immutable owner。
page 格式的全局 view 缓存预算按四页计费，flat 保留原预算；不是无限增加驻留内存。
已有 generation/poison 检查保留，未修改状态格式、capture 或 restore 算法。

隔离编译的原版/候选组件均通过完整 page-snapshot 测试，交替 A/B 两页的 40 次
读取使 StateStore load_requests 从 40 降为 2；超过四页的驱逐、owner 存活、
同 key 不同长度、cold reopen 与四槽预热后 poison/generation 均有检查。
独立静态审查通过。隔离候选保留在 `build/shared-parallel/page-cache4/`；重复测量后同一实现已进入产品路径，
正式 NP2 page/shared/v1 组件与 15 个 native parallel 场景、NP1 actors/adoption、
make check 和 NP3 生产构建均通过。生产 page_snapshot.o 与隔离候选对象逐字节 SHA
一致（72e387146469461a360ff0103a30f8092bf6782d3d47f0be543a98651f99e284）。

首轮 1/2/4/6/8 worker 的搜索加收尾分别为
31.6061 / 16.9516 / 11.0901 / 8.2652 / 7.0331 秒，各行均通过完整参考检查，
节点/边及规范图 SHA 与前版一致。该候选基于 v5，其独立原生参考也完整运行；
跨二进制的旧报告不能直接用作测试工具输入，重复测试按版本使用各自已验证参考。
单次数据由下述交错重复对照补充；样本仍小，不作统计显著性结论。

来源：`build/shared-parallel/page-cache4/build-and-components.log`、
`build/shared-parallel/pilot-page4/summary.json` 与
`build/shared-parallel/repeat-v4-v5-page4/`。

## 两轮交错复核

两轮各跑 1/8 worker，第二轮反转版本顺序，每个会话使用新目录。所有结果通过各自
相同二进制已验证参考的完整带标签图检查。工具拒绝了最初跨二进制参考请求，失败行
保留，改用每版本独立参考后继续；未把拒绝当作通过。第一次 v4-w1 期间误跑一次
旧池离线文件统计，故该行排除，矩阵结束后另补一个无干扰冷会话。未中断/重启活跃搜索。

| 版本 | 1-worker 含收尾中位秒 | 8-worker 含收尾中位秒 | 同版加速比 |
| --- | ---: | ---: | ---: |
| v4 | 32.4007 | 8.2549 | 3.925 |
| v5 共享闭包证书 | 32.9381 | 7.6473 | 4.307 |
| v5 + 四页缓存 | 31.6570 | 7.0050 | 4.519 |

8-worker 两轮分别是 v4 8.2525/8.2574、v5 7.6215/7.6731、四页缓存
6.9768/7.0332 秒。共享证书减少约 7.4% 的 8-worker 时间；四页缓存在其基础上
减少约 8.4%，合计约 15.1%。单 worker 共享证书略慢，额外证明有成本，不能只报
并行收益。每格仅两次，不提供置信区间，不将 4.52× 称为接近线性。

v5 各并发数产生 1,245 个 RootProof 文件，总文件逻辑字节 196,710，实际分配
5,099,520 字节（含对象头与文件块，不含额外目录开销）；不能只按小 payload 算空间。
原始行、替补及排除原因：`build/shared-parallel/repeat-v4-v5-page4/`；空间统计：
`build/shared-parallel/pilot-v5/root-proof-cost.json`。

## 当前产品 v6 的 CPU profile

冻结的 `tracer-shared-v6-np3` 完成 depth12/8-worker 同图搜索及随后的 native
serial 节点集/首父边投影验证。499Hz cpu-clock/DWARF16KiB 共 26,028 个样本，
全部被显式字段解码后的分析器接收，无丢样警告。最初默认 perf script 字段无法
匹配旧分析器，零样本结果废弃；重解码并加入零样本拒绝后才生成下列数字。

协调器占全部 CPU 样本 4.98%，worker 93.73%，tracee/其它 1.29%。worker 的
互斥优先归类为：CAS 外 capture 32.72%、restore/adoption 24.73%、CAS 10.69%、
CAS 外 Ready 校验/发布 4.87%、wire 1.91%、CAS 外 Ready metadata 1.77%、
其它/未归类 21.75%；不是墙钟等待份额。worker 未知叶子 4.21%。

可重叠的调用栈份额中，Ready 校验/发布 12.63%、页访问/迭代 27.94%、
StateView::read 3.39%。单独 vfscanf 叶子占 worker CPU 3.49%，调用链显示它
来自 capture_stopped/get_maps_item 与 restore_memory_mappings 的 sscanf。
尤其 capture_stopped 已有 resident 快路径，主要是在反复解析保存的 maps_text，
不能将这些样本全部解释成重新读取 /proc。这里是目标内存布局解析，与先前移除的
协调器后代扫描不同；不能因为它耗时就不观察可能变化的映射。后续可先验证严格等价的解析器优化，避免贸然缓存布局。

该 profile 深度比早期 v1 depth16 小，频率也不同，不能把两份百分比当作严格
同工作量 A/B 时间差。当前样本用于定位剩余工作，不作已经达到硬件上限的证明。
原始样本、显式解码及摘要位于 `build/shared-parallel/profile-v6/`。
新的 depth20 大图运行已启动于 `build/shared-parallel/large-v6/`，仍未完成，
不将旧版的大图通过结果当作本版已验证。

## 尚需完成

1. 根据当前 profile 验证映射解析等后续优化，而非继续假设通信/存储仍主导。
2. 补充更多重复样本，以及更大图的同工作量 1/2/4/6/8 对照。
3. 在结果达到目标前，不将传输量下降或高 CPU 利用率当作接近线性的证明。
