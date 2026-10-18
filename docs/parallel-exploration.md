# 同一规范快照上的原生并行探索

DetSim 现在由一个原生协调器管理共享 frontier，持久 worker 负责枚举一个保存状态的
全部受支持转移。DFS、BFS 和未来的优先级策略复用同一套执行、状态传输和去重接口。
这替换了原来的静态分支分配、整库复制和结束后解析 stdout 做 union 的原型。

用户允许框架统一目标布局：`RuntimeProfile.fixed_layout=true` 只在 fork 出的目标
exec 前设置并读回 `ADDR_NO_RANDOMIZE`，exec 后再次验证；宿主 ASLR 和 tracer 的
personality 不变。不尝试推断任意不同布局中的无类型指针等价。

## 状态与执行环境

跨启动使用同一份已保存 checkpoint，不将独立冷启动的 root 当成同一状态。首次状态
发布前采集 bootstrap manifest，并把其摘要并入 fixed-profile 的 model identity：

- DetSim 可执行文件、所加载动态库及插件内容；tracer 自身映射地址不参与比较。
- 每个目标的文件映射地址、长度、权限、offset、dev/inode 及 backing 内容。
- 原始有序环境、原生目标 cwd、CPU feature/xstate、内核、用户/组与资源限制。
- 原有模型身份继续绑定目标 ELF/argv、虚拟 FS/网络配置、选择点、性质及插件配置。

worker 启动先校验 Hello 的 model 和 manifest，再接收任务。启动后的 UI 库可能修改
环境，因此 spawn 使用保存的原始环境，不能忽略环境差异。`--target-cwd` 将目标的
真实 exec cwd 与 worker 存储目录分开。manifest 不可建立时普通串行仍可启动，但
并行明确拒绝；不能拿当前环境的 Hello 替旧 checkpoint 的来源作证明。

多线程接管可反复处理线程增减、主线程退出、全部逻辑线程退出及恢复早期状态。
物理 leader 保留为 anchor，至少需要 `1 + 活非主线程数` 个 carrier；扩容前后核验
owned、停止状态、捕获边界及内核注册。失败后的 worker 整体退出，不复用半恢复映像。
这不是可回滚事务，也没有承诺任意 pthread/Go/JVM 状态已经覆盖。

## 一步任务与共享 frontier

`src/core/search/frontier` 管理唯一节点表、pending 索引、深度及租约。DFS 后进先出
分派；BFS 保持活动层屏障；C++ Priority callback 可以给 pending 节点排序。并行DFS
完成顺序可交错，不等于串行DFS访问顺序。更短路径会传播至已知后继，必要时重开节点。

Task 包含 source、lease、worker、深度及是否可展开。深度边界只作 probe，不执行
用户指令；终点、深度截断、模型预算不足、性质违反和执行失败分别处理。
`enumerate_state` 明确控制自动选择模式，按 saved actor/action 枚举实际发现的全部
choice，逐条发送 Edge，不把整个 fanout 的快照攒在内存。收到 Done 后才提交源展开：

1. 检查 lease/source、目标身份、saved actor 合法性、normal/timeout 与 choice。
2. 检查实际 choice cardinality；必须恰好覆盖整个 `0..N-1`，连续前缀不算完整。
3. 完整验证并导入目标闭包，核对保存的性质结果；重复同一动作必须得到相同后继。
4. Done 的 emitted 数量与已收边一致，全部 actor 完成，才将源标为 expanded。

完整验证并记录的性质反例 Edge 立即成立，不等待 Done；worker 随后失联不能抹掉
已经保存的反例。提前停止时源租约回到 pending，不假称整个源已经展开。

全局去重发生在搜索中，不从磁盘存在 metadata 推断“已经展开”。没有512分支截断。
传输/进程失联使租约回队，健康或替补 worker 可接手；按源状态限制重试次数。明确的
恢复/模型错误直接失败。失败或性质早停留下未完成节点，不能报告 complete。

## 传输、持久化与清理

SnapshotBundle 包含原始系统 metadata、所有节点 root、page 对象及外置 VFS 内容。
引用必须在包中声明，即使目的端缓存已有对应对象。先检查 hash/重复冲突，保存并验证
全部不可变依赖，完成数据持久化屏障后，最后发布系统 metadata。失败可以留下有效孤立
blob；I/O poison 后不能继续写，也不宣称 exactly-once 或协调器重启恢复。

IPC 为有版本和长度界限的二进制 Unix socket 帧。对象直接写入/读入 vector，没有第二份
完整编码包或 base64，但每条 Edge 仍需 **O(完整闭包)** raw 内存。默认 frame 上限8GiB、
单 blob 2GiB；超过界限明确失败。整帧使用绝对 deadline，缓慢分片不能无限延长超时。
一次发送 Task 后返回读取结果，避免连续上传所有大任务饿死已完成 worker。

worker 在创建 tracee 前设置并验证父死亡信号；control FD 对目标 exec 为 CLOEXEC。
tracee 另有 PDEATHSIG/ptrace EXITKILL。协调器临时作为 subreaper，使用精确 pidfd/子进程
集合清理和回收，保留原有目标；不使用 `waitpid(-1)` 消耗其他 ptrace 事件。
SIGINT 明确中断搜索并清理 worker，CLI 可继续使用。阻塞帧的取消延迟仍受 frame timeout
限制，尚未实现完全非阻塞的多连接 IO 状态机。

## 验证证据

所有真实搜索都在原生 CLI 内执行；Python 只编译 fixture、安排有限参考动作、启动命令、
注入进程故障与检查报告。证据保留在 `build/parallel-redesign/`，不提交生成的 store。

| 验证层 | 覆盖与结果 |
| --- | --- |
| Frontier 组件 | 72组有限图，对照独立串行 BFS oracle；3策略、1/2/4模拟worker、乱序返回；1025后继、失联重派、陈旧租约、深度改善；4097链的索引访问回归 |
| Bundle 组件 | NP2 full/pages、外置VFS、fresh-process读取、warm缓存缺引用仍拒绝、重复/损坏/冲突及fsync失败不发布新metadata |
| Protocol 组件 | 消息往返、1MiB对象、长度/枚举限制、截断、EOF、RAII、慢速header/body整帧超时 |
| 普通线程接管 | full/pages各3个fresh worker，42次导入、34次显式后继核对，含2→3→2、main-dead/all-dead；owned task清理 |
| pthread 接管 | TLS、信号屏蔽、altstack、join Waiting/Ready、工作/主线程robust owner-death，48+24次后继核对 |
| NP2 完整小图 | full/pages × 1/2/4worker × DFS/BFS：均16节点、24条完整边，边集合一致；depth0只probe |
| NP1 联合动作 | choices/mixed/pending × full/pages：12次并行DFS/BFS、48条实际worker边，与24条预定义动作参考精确一致；72次hash/callback重放 |
| 故障与反例 | worker在lease后/边持久化后被杀，重派后图一致；重试耗尽/启动超时明确失败；SIGINT后CLI可用；coordinator被杀后8个owned进程由死亡保护终止；性质反例incoming edge重复3次 |
| 环境负例 | 相同ELF/config/cwd改变环境后，旧快照因model不兼容拒绝 |
| 常规回归 | 36个原生ptrace场景通过；make check的组件、14个build contract和5个目录测试通过；NP1/2/3生产编译 |

联合动作测试先实际发现了worker错误进入交互choice输入的缺陷。RED与GREEN使用相同
fixture/plugin ELF、同脚本；修复枚举器的自动选择作用域后全部通过。冻结证据分别在
`actor-native/run-12aa250b/` 和 `actor-native-final/run-m1___yhs/`。
这些后继hash/callback证据不等于每次恢复后立即全字节recapture，更不证明所有Linux程序。

## 性能与尚未覆盖的边界

后续实际CPU/syscall采样见[全链路perf分析](search-perf-2026-10-18.md)：pages并行
约半数CPU样本落在细粒度IPC，串行主要是跨进程读取和比较。该报告区分采样与非采样计时。

16节点/24边的小图在初轮完整矩阵中每次约0.45–0.60秒，包含worker启动、通信与清理。
1/2/4worker没有稳定线性加速，不能用这个小图宣称并行收益。三节点真实Raft库另有同seed、
深度4的对照（项目自有 healthy 非阻塞驱动，不是完整RedisRaft模块）：

| 后端 | 串行DFS | 并行DFS：1 / 2 / 4 worker | 并行BFS：1 / 2 / 4 worker |
| --- | ---: | ---: | ---: |
| full | 1.692s | 3.650 / 2.822 / 2.640s | 3.811 / 3.032 / 3.016s |
| pages | 1.119s | 2.756 / 2.050 / 2.275s | 2.599 / 2.210 / 2.258s |

以上均使用原生命令内部搜索计时；手动前缀、构建、load、检查和命令墙钟时间另计。
所有12次并行运行均为71节点、131条实际边，完整边集、最短深度和phase一致：32个
expanded、28个depth-cut、11个model-budget，结论是bounded，不是穷尽安全证明。
串并行新生成节点集合相等（68个）；另有seed及2个返回prefix的旧节点。串行完整树
包含全部并行节点，但只有first-parent边，不能据此宣称串并行完整边集合已独立核对。

full并行DFS为35.89/46.41/49.63边每秒；pages为47.53/63.90/57.58边每秒。
与同策略的并行1worker基准相比，DFS2约为1.29倍/full、1.34倍/pages；4worker并不
持续更快，且目前都慢于原串行DFS。这是单次固定顺序测量，不是重复统计或线性扩展
结论；串行报告186次visit，与并行131条唯一边口径不同，不把二者混算成同工作量加速比。

最终固定输入、完整journal、实际命令、指标和冻结driver位于
`raft-parallel-native-full/run-zvkx_wla/`、`raft-parallel-native-pages/run-al93kdoh/`。
同driver对旧vendor示例的负测保留在`raft-parallel-native-legacy-red/run-_x3uanrp/`：
其阻塞recvfrom令三节点全部untimedWaiting，明确拒绝作为吞吐工作负载；未放松pending
模型或修改vendor。可用`make benchmark-redisraft-parallel`重现新比较。

目前明显的成本是每Task/Edge传完整闭包、导入重复验证与持久化屏障、协调器串行接收，
以及每次跨worker恢复的ptrace成本。可进一步研究内容寻址对象的按需传输、验证缓存与
独立IO泵，但必须保留完整闭包证明和metadata最后发布，不能靠省略未知引用提速。

未声称解决：

- 任意ASLR地址重定位、独立冷启动root字节归一化、跨主机迁移。
- RDTSC/RDTSCP、CPUID位置相关返回、RDRAND等绕过syscall的硬件输入。manifest相同
  不推出这些指令确定；它们属于尚未接管的执行输入。同一动作重复得到不同后继会报错，
  但一次性的未重复执行不能靠这个检查证明确定性。
- 任意信号处理器、PI futex、所有exec/线程生命周期/IO模型组合。
- 协调器崩溃后的持久frontier恢复、远程或不可信worker、完整闭包的常数内存传输。

可支持的claim是：在已覆盖的确定性执行模型、相同兼容环境和规范seed上，原生共享frontier
可以并行枚举、实时去重、处理有界深度和重派，并对上述场景保持转移与反例的一致性。

## 可重复命令

各NP生产构建应顺序运行，避免共同覆盖根目录tracer：

```sh
make check CC=/usr/bin/gcc CXX=/usr/bin/g++
make check-search-frontier check-parallel-protocol CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=1 check-adoption check-fixed-layout check-parallel-actors CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=1 check-scenarios CC=/usr/bin/gcc CXX=/usr/bin/g++
make NP=2 check-parallel check-snapshot-bundle CC=/usr/bin/gcc CXX=/usr/bin/g++
make benchmark-redisraft-parallel CC=/usr/bin/gcc CXX=/usr/bin/g++
make benchmark-redisraft-parallel REDISRAFT_PARALLEL_ARGS='--backend full --depth 4'
```
