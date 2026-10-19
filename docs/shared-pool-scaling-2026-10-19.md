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

## 大图 v6 在途记录与解析器候选

新大图的 parallel 命令已返回：depth20、8 workers、291,332 节点、521,196 条
实际边、零重试；原生搜索 207.0018 秒，完整命令 208.5001 秒（约 2,500 边/秒、
1,397 节点/秒，节点包含边界/预算叶子）。随后 native serial 参考仍在执行，
后续原生参考验证已完成：new_node_sets_equal 与 serial_projection_edges_subset
均为 true，tracer_exit=0，最终 passed=true；规范图 SHA 为
30653c6934051b642fcc4aa0af48b72094cd610f59de2dc3f3fae7a26f59ef91，与 v1 大图相同。
验证后收尾 0.4148 秒，整个会话 1495.1915 秒；该收尾包含 oracle 后的同步，
不作为 pure-parallel 全链路基线。native oracle 实际执行 957,059 次 transition，
不是拿耗时直接与 521,196 边的 parallel 算线性加速比。
来源：`build/shared-parallel/large-v6/run-nw7tdq95/results.json` 在途报告。
该阶段计时结束并记录后才启动下面的编译/组件测试，后续 serial oracle 仅用于
验证覆盖，其受并行编译影响的墙钟不用于效率对照。

下一候选仅替换 state.cpp 三处 maps 文本解析：一个私有 from_chars 行解析器，
并以 string_view 遍历保存文本，避免重复复制 istringstream/逐行 string。
不新增布局缓存或状态字段，不改变真实 /proc 读取和 resident 失效规则，
保留 unsigned 符号、十六进制前缀、匿名记录、7项、511字节 first-token 名称；
数值溢出明确拒绝。旧实现先通过新增词法回归。

初版候选在保存文本含 NUL 时改变旧 sscanf 行为，独立审查发现后用真实快照
副本复现 RED；修正入口 NUL 截断后测试 GREEN，要求在任何注入的映射 syscall
前明确拒绝。NP1 映射 full/pages/verify 与 tiny-cache、state-io、actors/adoption，
NP2 capture 等价、page/shared/v1 与15个 native parallel 场景均通过；
make check 与 NP3 生产构建通过。候选未提交；大图参考结束后首轮全链路
同图性能实验已启动，尚无最终判断。
日志位于 `build/shared-parallel/maps-parser/`。不把减少 sscanf 调用等同于吞吐收益。

## 计时口径补充：参考生成会话不是纯搜索收尾

源码复核发现 `parallel_benchmark.py` 的首次 `--parallel-first`、无 reference
模式，在 timed parallel 之后仍在同一个 tracer/store 执行 serial oracle，最后
才 graceful shutdown。因此该行的 finalization_seconds 包含验证新增写入，
search_and_finalization_seconds 不是隔离的 parallel-only 全链路耗时。此前
大图 v1 的 408.9988 秒只可称为“并行命令加验证后收尾”，不应拿它作为纯并行
全链路扩展性基线；parallel command_seconds 本身仍是已隔离记录的计时。

两轮正式交错小图对照全部使用每个二进制自身已验证的 reference-report，跳过
本地 oracle，故表中的 1/8 combined 中位数没有这项混合。早期首次 pilot 的
combined 单次比值不具备相同的收尾条件，应以这张正式交错表为准。

当前大图 v6 首次运行用于建立参考；其最终收尾同样不能充当 pure-parallel
耗时。参考通过后必须另跑 fresh-session reference-only 的 8-worker 行，
再与同条件 1/2/4/6-worker 行比较。保留真实持久化屏障，不通过跳过 flush
改善统计。不修改或重启仍在运行的参考生成进程。

## 解析器候选四轮结论：不合入优化

四轮分别以正/反版本顺序运行，每格4个全新 reference-only 会话；全部通过
完整参考图、10457/18266/零重试检查。原始16行保留在
`build/shared-parallel/repeat-v6-v7/summary.json`，摘要为 four-round-summary.json。

| 版本 | 1-worker 含收尾中位秒 | 8-worker 含收尾中位秒 | 同版加速比 |
| --- | ---: | ---: | ---: |
| v6 | 32.2624 | 7.1263 | 4.527 |
| v7 解析候选 | 30.5911 | 7.1039 | 4.306 |

单 worker 约快5.2%，8-worker 仅约快0.3%；各轮8-worker差异方向不一致，
不能宣称稳定并行收益，更未改善扩展性。故不合入该解析器实现，生产 state.cpp
已恢复 v6。候选原始源、patch、固定二进制、NUL RED/GREEN与性能记录归档，
保留原解析器也通过的词法/保存文本NUL回归；候选专属溢出拒绝测试没有移植到
仍使用 scanf 的生产代码，不冒充已实现的边界检查。

下一实验针对并行额外成本，而非继续均匀微优化：在 Hello admission 完成、
首次 Task 开始执行前，将每个 worker 与它控制的目标线程绑定到同一允许CPU，
测试迁移/跨缓存交接成本。只准备独立 artifact，未改生产或放宽 manifest。
受测程序仍必须符合当前虚拟CPU/rseq与CPU指令限制；不承诺任意程序的CPU位置
观测均已虚拟化。另保留独立 DWARF cache-first 候选的组件证据，暂不混入。

## 尚需完成

1. 根据当前 profile 验证映射解析等后续优化，而非继续假设通信/存储仍主导。
2. 补充更多重复样本，以及更大图的同工作量 1/2/4/6/8 对照。
3. 在结果达到目标前，不将传输量下降或高 CPU 利用率当作接近线性的证明。


## CPU 放置四轮对照与采样质量复核

独立 artifact 将每个 worker 及其目标物理线程绑定到同一 CPU；生产代码仍为
v6，未默认启用绑核。balanced artifact 的 CPU 顺序仅适用于本实验主机，不能
直接作为通用实现。四轮交替版本顺序，每格四次全新 reference-only 会话，
24 次全部通过 10457 节点、18266 唯一边、零重试的完整带标签图检查。
数据位于 `build/shared-parallel/repeat-placement/paired-summary.json`。

| 放置方式 | 1-worker 含收尾中位秒 | 8-worker 含收尾中位秒 | 同策略加速比 |
| --- | ---: | ---: | ---: |
| v6 不绑核 | 32.5225 | 7.3681 | 4.414 |
| 按 CPU 编号配对 | 32.1510 | 6.8223 | 4.713 |
| 跨 LLC 均衡配对 | 32.1069 | 6.5197 | 4.925 |

均衡配对相对不绑核的 8-worker 时间降低约 11.5%；使用不绑核单 worker
为分母得到 4.988 倍。单 worker 未被人为减慢。四轮不是置信区间；尚未达到
计划中 8-worker 至少 5.6 倍的目标，也没有新版大图全 worker 数对照。
通用化仍需 CPU 拓扑选择、线程增减/恢复、失败及冷重放验证。

采样复核：`profile-placement-corrected` 的 16 KiB DWARF 栈采样存在大量
无法归入阶段的 worker 样本。新建 `profile-placement-stack64`，在同一个
固定 balanced 二进制和同图参考上，用 199 Hz、65528 字节栈再次采样；
探索通过，9739 个样本，无丢样警告、解码错误。worker 样本 9115 个，
其中约 69.5% 仍无法用当前调用栈规则归入阶段；16 KiB 对照约 45.6%。
worker 栈帧数中位值由 7 变为 6，到达 main 的样本仅 22 个。
因此“加大用户栈就能修复归因”的假设未获支持。没有丢样并不意味着展开完整；
不能用这些阶段占比推导绑核使 pending 检查成本减半，也不能比较这两次
profile 的阶段占比来排名优化收益。下一步需定位展开失败或增加低开销阶段
计数/计时，再决定控制与恢复流水线的改造点。profile 耗时不纳入上表。


## 12-worker 压力首轮（尚非重复结论）

按用户建议扩展到12 worker，在相同 v6+balanced 独立 artifact 上，增加阶段
时钟采集但未输出逐任务日志。先独立建立同二进制 native 参考，然后新会话
分别跑1/8/12。三者完整图 SHA 均为
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`，
10457节点/18266边/零重试，均通过，摘要
`build/shared-parallel/stage-pressure12/summary.json`。

| worker | 含收尾秒 | 本轮单 worker 加速比 |
| --- | ---: | ---: |
| 1 | 35.2186 | 1.000 |
| 8 | 6.9020 | 5.103 |
| 12 | 5.9642 | 5.905 |

12 相对8耗时下降13.6%，不是worker数增加50%对应的吞吐增长。
本轮单worker较此前四轮中位值慢，故不能拿5.905与此前4.925直接当作版本
优化收益。这只是一次压力观察；6物理核/12逻辑CPU也不提供12独立物理核。
此 instrumentation 最初只在 Shutdown 输出阶段汇总，但正常 coordinator
结束使用进程回收，故没有得到阶段数据，不用缺失值计算比例。新v2 artifact
在每个 Done 前输出累计计时，计时包含日志扰动；以其结果分析成本，不宣称
这类instrumentation是优化。原首轮图和端到端测量仍保留。

另对64KiB采样用系统 libunwind 替代默认 /usr/local/lib 重新离线解码，
worker阶段计数完全相同，没有解决展开问题，未改系统库配置。NP=3实测
sizeof(syscall_info)=64、sizeof(tracee_state)=5240、sizeof(sys_state)=15856；
这些尺寸本身不足以证明展开失败来自栈采集长度。


## 12-worker 阶段计时（完整计数，非 CPU 归因）

v2 在每个 Done 前记录累计阶段墙钟时间；每次取各 worker 最后一条，核对
总计3824远程Task、18266边，三种worker数均通过同图验证。输出、时钟和
调度扰动包含在结果内，不与无instrumentation版本直接排名。
`build/shared-parallel/stage-pressure12-v2/phase-summary.json` 保留精确值。

| worker | 含收尾秒 | 导入累计秒 | adoption累计秒 | 首次恢复累计秒 | 枚举扣除发布/发送累计秒 | 发布累计秒 | 发送累计秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 30.9627 | 0.3691 | 0.8716 | 1.2474 | 23.3424 | 3.6292 | 0.1004 |
| 8 | 6.3208 | 0.7162 | 1.3381 | 2.1077 | 33.4765 | 5.7527 | 0.1847 |
| 12 | 6.1718 | 1.0667 | 1.9196 | 2.8246 | 44.3565 | 8.2189 | 0.5811 |

累计秒为各worker区间求和，包含睡眠/抢占，不能读作CPU秒，也不能相加
得到全链路时间。首次恢复不含 enumerate 内部为不同分支做的恢复；枚举
扣除项仍包括执行、分支恢复、捕获、性质检查。发送区间仅测Edge发送，
不是完整通信或等待总量。12相对8本轮仅快2.4%，与首轮13.6%不一致，
提示必须重复，不能据首轮宣布稳定12-worker收益。
同样工作量的多个阶段均出现累计墙钟放大，下一步增加线程CPU计时以区分
运行成本与等待，再细分枚举区间；当前证据不支持“只是通信瓶颈”。


## 可选放置产品接线验证

`--worker-placement none|same-cpu` 已接入原生 parallel，默认 none。
通用拓扑算法替代 artifact 的硬编码主机 CPU 顺序；尚未以此产品二进制
重跑大图吞吐，不能直接将 artifact 的11.5%收益当作产品测量。
独立静态审查未发现 P0/P1 阻断。以下命令新鲜通过（固定各NP二进制副本）：

- `make NP=2 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4 check-worker-placement`：拓扑、默认与私有参数。
- NP=2 production tracer 构建；`parallel_scenarios.py --worker-placement same-cpu`：15/15。
- 同一NP=2二进制 `--worker-placement none`：15/15。
- NP=1 production tracer 构建；`parallel_actor_scenarios.py --worker-placement same-cpu`：choices/mixed/pending × full/pages 六例通过。
- `make CC=/usr/bin/gcc CXX=/usr/bin/g++ check` 通过；`git diff --check` 通过。

原生15例包含full/pages同图、shared/durable对照、冷反例重放、worker丢失重试、
父进程死亡与晚建helper清理；新增审计核对原进程mask和worker逐TID绑定及
重启分配。actor六例覆盖实际线程、选择和pending/timeout的手工动作对照及重放。
尚无same-cpu专属2→3→2多次线程数量变化门禁、实际sched_setaffinity故障注入，
也没有产品二进制的新大图/重复吞吐矩阵；这些证据仍需补充。


## 主线程 CPU 对照：既有运行成本增加，也有等待

v3 对每个阶段增加 `CLOCK_THREAD_CPUTIME_ID`，每个区间各有独立起终点，
每次 Done 前记录累计值。1/8/12三组均通过同图，且累计3824Task/18266边。
数据：`build/shared-parallel/stage-pressure12-v3/phase-summary.json`。
以下“CPU”仅worker主线程，排除目标、IO/helper及coordinator，不是全系统CPU。

| worker | 含收尾秒 | 枚举扣发布发送 wall/CPU 累计秒 | 发布 wall/CPU 累计秒 | 初次恢复 wall/CPU 累计秒 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 32.3231 | 24.1253 / 23.8320 | 3.9828 / 3.9008 | 1.2807 / 1.2767 |
| 8 | 6.8236 | 35.3325 / 34.0952 | 6.5601 / 6.0876 | 2.2152 / 2.1695 |
| 12 | 6.5455 | 45.7892 / 38.4072 | 11.7754 / 6.7392 | 2.8019 / 2.5781 |

同工作量的枚举主线程CPU从23.83增至38.41秒，不能仅以worker等调度解释；
可能包含SMT资源竞争、缓存/内核路径成本等，尚未细分。12-worker发布阶段
wall明显超出CPU，也存在非运行时间，但差值不能直接命名为磁盘等待或某把锁。
读取时钟和逐Task输出有扰动，不据此更新headline版本收益。
下一隔离实验保持同二进制/图参考，把新建session目录放到本机tmpfs，以区分
文件系统/持久化路径与其余成本；不改原有存储位置，也不把tmpfs当持久存储。


## 同二进制 tmpfs 隔离实验

使用相同v3计时二进制、guest及已验证参考图，仅把新会话输出目录放入本机
`/dev/shm`新建私有目录。三组均3824远程Task/18266边且同图通过；不移动或
修改原存储。该目录不是持久存储。精确数据及目录位置保留于
`build/shared-parallel/stage-pressure12-tmpfs/{phase-summary.json,output-path.txt}`。

| worker | tmpfs含收尾秒 | 发布 wall/CPU 累计秒 | 枚举扣发布发送 CPU 累计秒 |
| --- | ---: | ---: | ---: |
| 1 | 29.9766 | 3.0126 / 2.9890 | 22.8340 |
| 8 | 5.8443 | 4.7477 / 4.7074 | 32.4494 |
| 12 | 5.3211 | 6.0925 / 5.7585 | 38.8761 |

磁盘目录同轮12-worker为6.5455秒，tmpfs下降约18.7%，发布wall−CPU
从约5.04累计秒缩到0.33秒；枚举CPU却从38.41到38.88，未改善。这支持
文件系统发布路径和执行CPU成本是不同问题，但未确定具体锁/IO原因。
实验也改变了metadata/store及worker目录所在文件系统，并非只替换CAS路径。
先磁盘后tmpfs且每格单样本，不能当作稳定收益或通用推荐；包含计时/输出扰动。
现有 state_store.packed_storage_path 配置可指定存储目录，无需为这项实验新增
后端；若使用tmpfs，重启/内存压力/容量和持久性边界需由调用方明确接受。


## 删除 expanded Task 首次重复恢复

worker 的prepare/adopt后原先无条件恢复source，第一条实际转移又在
`state_transition`无条件完整恢复source。现在仅classify为Expanded时删除
worker层前一次恢复；非Expanded仍恢复，正常枚举结尾的load不变，反例原有
提前返回语义不变。每条实际动作前的线程/信号/内核资源/内存恢复没有跳过。
静态独立审查追踪actor、timeout、saved IO readiness及adoption，确认首动作前
没有需要source已安装的live读取。没有引入恢复验证缓存或新身份字段。

新鲜门禁：NP=2 production构建，same-cpu与none各15项原生并行测试；
NP=1 production构建，same-cpu choices/mixed/pending × full/pages六例；
`make CC=/usr/bin/gcc CXX=/usr/bin/g++ check`，均通过。日志在
`build/shared-parallel/worker-restore-dedup/`。尚未测此删除的吞吐收益；
首次edge的restore_time可能变大，因为原来未归入edge的工作转移到了这里，
不能以该字段独立评判退化。专门的首次restore故障注入仍未新增，不能把已有
错误路径和同图门禁夸大成该故障点的独立覆盖。


## 产品版本两轮 1/2/4/6/8/12 对照

固定NP=3产品二进制（placement及首次恢复去重），使用通用same-cpu排列，
无阶段探针。先建立同二进制native参考，再以1/2/4/6/8/12及反向顺序运行
各格两个全新reference-only会话。12次均通过10457节点/18266边完整图，零重试。
原始行及中位摘要在`build/shared-parallel/product-placement/summary.json`、
`paired-summary.json`，固定二进制SHA记录于manifest.json。

| worker | 含收尾中位秒 | 相对同版1-worker | worker效率 |
| --- | ---: | ---: | ---: |
| 1 | 31.5293 | 1.000 | 100.0% |
| 2 | 15.9283 | 1.979 | 99.0% |
| 4 | 9.6683 | 3.261 | 81.5% |
| 6 | 7.5664 | 4.167 | 69.5% |
| 8 | 6.8074 | 4.632 | 57.9% |
| 12 | 6.4074 | 4.921 | 41.0% |

2-worker近线性；4-worker达到计划的至少2.8倍，但8-worker仍未达到5.6倍。
6个物理核的效率已约69.5%，因此不能把全部退化归于8/12使用SMT。12-worker
两次5.6011/7.2138秒，变异大，不能把最佳一次当headline。各格仅两次，不提供
置信区间；也没有与旧版交替测量，不能把历史数据差异归因于恢复去重。
产品journal验证选核为0,3,1,4,2,5,6,9，与artifact预期的均衡排列一致。
随后已启动depth16独立参考及1/2/4/6/8/12新会话矩阵，仍在运行，暂不发布
其吞吐结果。该更大图用于判断小图启动/收尾及宽度对扩展性的影响。


### depth16 独立参考验证完成，扩展矩阵仍在运行

固定产品二进制的depth16参考已完整通过，tracer退出0：63418节点、112305条
实际且唯一边、零重试。canonical SHA为
`7eba9a268e4fd2bc45bce49b59c02e85389b743c53a9a25214d59dd3569f2630`。
参考生成中的12-worker native搜索37.4390秒、命令37.7471秒；随后独立原生DFS
命令231.7836秒完成节点/首父边投影验证。该DFS不构成独立全边oracle，
其工作量也不能作为并行加速比分母。参考会话总计278.6153秒，包含验证，
不作为纯并行全链路吞吐。权威结果在
`build/shared-parallel/product-placement-large16/reference12/run-f_g2gsiz/results.json`。
后续fresh reference-only的1/2/4/6/8/12正在顺序运行，暂未形成完整加速比。


## depth16 产品扩展矩阵完成

六个全新reference-only会话均通过63418节点/112305边完整带标签图验证，
零重试、正常退出。全链路时间为command到prompt加store收尾，不含独立
参考生成。`product-placement-large16/scaling-summary.json` 保留各会话路径。

| worker | 含收尾秒 | 同版加速比 | worker效率 | 实际边/秒 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 185.5098 | 1.000 | 100.0% | 605.4 |
| 2 | 93.7690 | 1.978 | 98.9% | 1197.7 |
| 4 | 57.6095 | 3.220 | 80.5% | 1949.4 |
| 6 | 43.8786 | 4.228 | 70.5% | 2559.4 |
| 8 | 41.6209 | 4.457 | 55.7% | 2698.3 |
| 12 | 38.0117 | 4.880 | 40.7% | 2954.5 |

该图约为小图6倍边数，8/12-worker加速比仍约4.46/4.88，未达到8-worker
5.6倍目标。固定启动与收尾不能独自解释当前扩展损失；这不等于已经证明
某一个持续瓶颈是主因。每格仅一次，不能据此区分微小版本差异。

分桶64实验已完成独立artifact构建，三组适配组件门禁全部通过：atomic并发
发布/冲突/partial临时文件、类型与hash损坏、缓存、readonly、恒定flush及
跨设备/poison；StateStore pending/globalhint/coldfallback/barrier；full/pages
Ready完整闭包、actors、globalproof与冷只读重开。新布局拒绝flat/缺桶、
symlink/非目录/异设备及构造失败FD泄漏也纳入。精确命令/对象hash/测试
binaryhash在`pool-sharding64/{build-manifest.json,components-manifest.json}`，
原始输出在components.log。源码仍未进产品；独立native同图参考与两轮
baseline/candidate 1/8/12吞吐对照已启动，尚无收益结论。


## 分桶两轮结论：不合入

候选独立native参考通过；两轮baseline/candidate正反顺序，各1/8/12的12个
fresh reference-only会话全部通过10457节点/18266边/零重试和相同canonical图。
此实验两边都基于冻结v6+balanced worker，唯一受测差异为pool布局；没有把
新产品的恢复去重混入基线。精确行在`sharding-pilot/summary.json`，中位摘要
为paired-summary.json。

| worker | 原布局中位秒 | 分桶64中位秒 |
| --- | ---: | ---: |
| 1 | 32.2231 | 32.2176 |
| 8 | 6.8044 | 6.7719 |
| 12 | 6.1218 | 6.1647 |

8-worker约快0.5%，12-worker约慢0.7%，单worker近乎不变；方向和幅度均
不足以支持引入新布局及其兼容性成本。两次/格不是“完全无效果”的统计证明，
而是当前没有可用的收益证据。因此不合入，保留候选源、组件故障门禁与原始
计时。根目录仍使用原pool布局。原生同图通过也不扩大到任意程序等价保证。
冻结新产品对象的7.3MiB拷贝发生于本实验运行期间，未运行其他build/目标；
这也是不解读亚百分比差异的原因之一。
下一步测固定产品的cycles/instructions/task-clock，并评估bounded roots局部性
派发；后者只准备隔离候选，不授权跳过任何恢复校验。
