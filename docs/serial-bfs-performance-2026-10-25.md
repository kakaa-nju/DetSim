# 单实例 BFS 吞吐优化（2026-10-25 至 26）

## 最终验收：目标已达到

2026-10-26核验完成。`examples/redisraft` 的三节点Raft C库案例，单个tracer运行
原生 `bfs --quiet`，普通O3编译、pages后端、tracer及其子进程固定到CPU2，
Python驱动保持原亲和性。**不需要PGO、LTO或多个探索worker。**
机器为AMD Ryzen5 2600、Linux6.12.105+deb13-amd64；hot/warm cache为32/64MiB，
seed1，确定性前缀100轮，初始model budget2，原有选择及性质插件保持开启。

| 原生搜索秒数 | 累计访问 | 累计访问/s | 新保存身份（含预算边界） | 新保存身份/s |
|---|---:|---:|---:|---:|
| 120.029240 | 121999 | **1016.41** | 68320 | 569.19 |
| 300.009176 | 303506 | **1011.66** | 169789 | 565.95 |
| 300.010851 | 307882 | **1026.24** | 172255 | 574.16 |

三次都正常中断并收尾，tracer exit0；两次300秒使用同一二进制，均超过1000。
计时包含原生搜索和其中的恢复/保存/性质检查；排除启动、确定性前缀、检查tree、
构建和退出后的清理。此处是累计访问口径，不是唯一状态吞吐或穷尽安全证明。
所测为Raft库，非完整Redis服务；速度也不是对其他机器或任意程序的保证。

二进制SHA256：`9ee454c6c99a046dc57f6021b3d436332532704e1dabd6dfb10f67c9944323f4`。
最终产品源码提交：`9240b1d`。测量时先冻结二进制，后将已验证源码
分批提交；原始JSON的checkout_commit仅记录启动时HEAD，不能代替二进制身份。
[验收数据及输入摘要](serial-bfs-performance-2026-10-25-results.json)保存精确数字、
原始报告哈希、编译配置与各次输入身份；大体积原始材料留在build目录。

复现（此机CPU2在允许集合内）：

```sh
make -j4 NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ tracer \
  build/release/np3/examples/redisraft/choose.so \
  build/release/np3/examples/redisraft/check.so \
  build/release/np3/examples/redisraft/seed.so
python3 examples/redisraft/serial_benchmark.py --cpu 2 --seconds 300
```

重复比较时用 `--binary PATH_TO_RAFT_CASE` 固定第一轮生成的目标。`--cpu` 仅限制
新启动的tracer及后代，恢复driver原亲和性；不改宿主调度/ASLR策略。
驱动会记录真实tracer退出码，仅在采集及收尾均成功时写passed=true。

## 最终实现与验证

采用的改动：队列携带已知组件引用，省去重复解码；BFS引用已取出的不可变源状态；
统一快速映射解析；从一次系统元数据编码计算身份并持久化；StateView复用已验证
快照；按物理carrier池保留status及task目录句柄。每次仍重新读取信号和枚举线程，
前后两次集合检查、GETREGS、rseq、robust检查均保留。另修复扫描器函数声明的
返回类型。没有修改搜索计数、动作枚举、model budget、检查插件或快照格式。

已验证：

- `make check CC=/usr/bin/gcc CXX=/usr/bin/g++`，含14项构建契约、5项目录/启动器检查；
  最终普通O3 NP3生产构建，哈希与两次长测完全一致。
- NP1：check-thread-control、check-pending-status-reader、check-thread-carrier、
  check-thread-identity、check-thread-signal-mask、check-thread-robust、
  check-thread-lifecycle、check-adoption、check-mappings、check-state-io。
- NP2：check-identity、check-state-view、check-capture、check-plugins。
- `tests/tracer_scenarios.py`：40个native场景；`tests/native_search_actors.py`：
  28行full/pages BFS/DFS及重放；`tests/parallel_actor_scenarios.py`通过。
- `examples/redisraft/run.py --backend all --variant all`：健康full/pages均pass，
  缺陷full/pages均violation，四组均三次同进程哈希一致重放、tracer exit0。
- 无rewind的reader变体确实失败；正常reader连续20次通过。独立复跑发现并修复了
  测试的退出通知竞态：先等待内核task集合移除worker，再断言旧描述符不可读。
- retained屏障第一/第二次枚举出错及worker运行时均拒绝、零resume；warm StateView
  在store poison和shutdown/reopen后均拒绝；身份及payload与独立旧编码完全一致。

精确日志：`build/serial-bfs-700/final-{np1,np2,scenarios,actors,parallel-actors,check}.log`、
`final-raft-cases.log`、`task-directory-{red,repeat}.log`及各case结果JSON。源码经独立
只读架构复核，指出的retained屏障及store-poison测试缺口已补上并通过。

资源取舍：每pool新增每carrier一个status fd及一个task目录句柄；每StateView最多
持有每节点一个完整快照根，大型full镜像的峰值内存未做专门量化。reader沿用串行
控制线程契约；未认证同一reader/View对象的并发读取。原有model/atom支持边界
仍见correctness-backlog与model-checking-contract，不能由性能结果宣称全部闭合。

以下为历史实验过程，其中“尚未达标”等表述属于当时阶段。

目标（用户更新）：提升单一实例在 `examples/redisraft` 上的 BFS 探索能力，
原生 CLI **累计访问速度达到1000 states/s以上**。此前700的目标已被替换。
这里的目标是 RedisLabs Raft C 库的三节点项目自有非阻塞 fixture，非完整 RedisRaft
Redis 模块。使用原生 `bfs --quiet`，Python只构造前缀、启动、定时中断及读取结果。
本轮没有启动并行 worker 搜索，也没有恢复此前暂停的并行优化路线。

## 验收与计数

必须同时报告三项，不能混用：

1. CLI `searched / elapsed`：根状态加已执行转换，包含重复后继。
2. CLI `new / elapsed`：新纳入搜索集合的身份；当前实现排除模型预算耗尽的后继。
3. 相对手动前缀新增的已保存身份/elapsed：从前后 `tree` 的节点集合差得到，包含
   预算边界。这是保存的唯一节点计数，first-parent tree 不提供所有转换边的证明。

最终应有多轮足够长的无采样测量、明确机器/构建/输入身份、不同缓存压力及正确性
回归。固定时长中断只能证明该窗口吞吐，不能证明搜索完成、性质成立或固定工作量
等价。当时仅建立基线；最终结果见文首验收。

## 基线

代码基线 `62ea4b3`，GNU g++/gcc，NP=3 release `-O3`，本机 tilnel，Linux
6.12.105+deb13-amd64。使用 pages，32 MiB hot /64 MiB warm cache，固定目标布局，
seed=1，三节点各100次确定性前缀，起始模型budget=2，性质插件保持启用。
完整输入SHA256、配置和主机信息在各次 `result.json` / `session/config.json`。

| 无采样搜索窗口 | searched | CLI new | 新保存身份（含预算边界） | 访问/s | CLI new/s | 新保存身份/s |
|---|---:|---:|---:|---:|---:|---:|
| 15.031352 s | 11656 | 4039 | 6633 | 775.45 | 268.71 | 441.28 |
| 60.010035 s | 45017 | 14598 | 25485 | 750.16 | 243.26 | 424.68 |

两次均被SIGINT正常中断。初版解析器未识别该中断格式，旧result中的completion为null；
原始bfs.txt明确记录中断，新驱动显式记录interrupted。两次前缀各301个保存身份。
旧result没有第三项字段，可由prefix-tree.json/tree.json的节点集合差复算。

路径：`build/serial-bfs-700/run-amp4exo8/`、`run-pu6o8oz0/`。
冻结基线程序：`build/serial-bfs-700/tracer-baseline`。

## 首轮 profile 与行动

原始采样：`build/serial-bfs-700/run-d8a9um6_/perf.data`，2911个 cycles:P 样本，
perf report报告丢样0。99Hz、DWARF栈8192字节，附加tracer；只反映tracer及其受采样
线程CPU，不代表全部墙钟时间。调用栈累计百分比可重叠，不能直接相加；栈深也有截断
可能。self/inclusive文本在 `perf-self.txt` / `perf-inclusive.txt`。

- 性质插件 check_transition 累计约20.14%，包含表达式解释/DWARF读取。
- verify_stopped_thread_set 累计约17.42%，包含线程枚举及信号检查。
- get_maps_item 累计约5.32%；全局scanf self约6.09%。
- StateStore::submit_prefetch_tasks 累计约6.03%。每次BFS弹出时重新解码队首最多100
  个元数据，即使它们上一轮已在窗口中。第一项优化针对这处可避免的重复解码。

预取优化不变量：FIFO、实际load的完整字节验证、错误传播和生命周期仍成立；预取
仅是提示，任何未命中的真实加载仍有效。新增跟踪必须在队列清空、初始化/关闭及
窗口变化时正确更新；并发队列操作不能使缓存引用失效。先做重复解码的红/绿回归及
现有存储测试，然后比较同版单实例BFS，最后扩大正确性及性能验证。

本机perf_event_paranoid=3，普通用户采样失败。仅对perf使用sudo -n，未改主机sysctl。
第一份有数据的采样结束时sudo返回130，而perf实际完成落盘；手动perf report成功
且丢样0。此行是诊断资料，不用于性能验收；驱动已兼容该信号结束码。

## 复现

```sh
make -j4 NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ tracer \
  build/release/np3/examples/redisraft/choose.so \
  build/release/np3/examples/redisraft/check.so \
  build/release/np3/examples/redisraft/seed.so
python3 examples/redisraft/serial_benchmark.py --seconds 60
# 重复时使用相同--binary，避免重新编译目标程序
python3 examples/redisraft/serial_benchmark.py --seconds 30 --binary PATH_TO_RAFT_CASE \
  --perf /usr/lib/linux-tools-6.8.0-138/perf --perf-sudo
```

每次独立目录保存数据。实验之间不同时运行编译/测试或另一个CPU实验；完成证据
核验后回收该次生成快照，不再积累整个实验树的重复worker和镜像。

## 长测：稳定基线与新目标

用户要求更长窗口检查树根重复状态的影响。预定600秒，按用户后续要求在约
552.5秒中断；之后用户将目标明确为原生BFS累计访问速度>=1000 states/s。
该次使用冻结原始基线（SHA256 `258d0dd9159c2128c7d0a6e9979f5b0275d50a275fe8c84ae27c786ef9d7cb0a`），
没有包含待验证的预取修改。搜索发生于单个tracer，受控目标为三个Raft进程。

- 原生搜索552.499738秒，418873次访问，**758.141536 visits/s**。
- CLI new121975；相对前缀新增保存身份234034，**423.591151/s**。
- 新保存身份/实际转换约55.87%；其余约44.13%未产生新保存身份，
  包含回到前缀的身份。不能用CLI new算出真实去重率。
- 30秒只读采样所得新增记录数与最终完整tree节点差完全一致。
- L1/L2最终均满；StateStore loads1776639，其中L1=1723670、L2=51779、disk=1190，命中率99.9%。

| 诊断时间段（约秒） | 新保存身份 | 新保存身份/s |
|---|---:|---:|
| 0–120 | 51315 | 427.34 |
| 120–240 | 50896 | 423.89 |
| 240–360 | 50799 | 423.07 |
| 360–480 | 50609 | 421.51 |
| 480–553 | 30415 | 420.82 |

分段分母来自宿主采样时钟，包含采样遍历的微小误差；总吞吐分母仍是原生搜索
报告。没有发现后续时间段比初期明显更快。该次first-parent图从搜索根可达
234035个身份，最大深度20；图宽很大，长时间BFS并不等于已经搜得很深。

产物：`build/serial-bfs-700/run-9bbfp0pv/`，含inputs/result、原始BFS输出、
30秒采样、prefix-tree/tree及long-summary。已核实驱动exit0、采样与状态树计数
一致，所有本次目标进程已由驱动关闭。此结果支持约758/s的基线，**不支持1000/s
目标已经达成**。

## 第一项优化：队列携带预取组件引用

BFS入队时已有 `ts_hash[NP]`，现在把这些值复制到队列项中。预取仍查看原来的
lookahead窗口，直接使用这些组件引用；不再为每个候选重新加载和解码sys_state。
保留hash-only接口原来的元数据解析路径。新增引用仅是预取提示，实际出队仍加载
并验证权威元数据，不改变状态身份、FIFO或调度动作。

NP=3每个队列项从8字节增长至40字节；在长测约3.5万项的frontier上，额外约1.1MiB。
这是避免重复解码的明确空间取舍，不是把frontier快照全部常驻。

- 新的 `queue_hints` 回归经实际 `state_queue_append` 路径验证：零额外元数据load，
  冷预取字节/FIFO、入队后源对象变化不污染队列、混合hash-only入口、零引用、
  清空及重开、错误key的元数据在真实extract时仍被拒绝。
- 把生产入队改回旧hash-only调用时，回归因 `known hints: metadata was loaded` 失败；
  恢复后 `make -j4 check-store-cache NP=1 CC=/usr/bin/gcc CXX=/usr/bin/g++` 的12项通过。
- NP=3生产构建通过。初次60.035851秒无perf采样BFS：48884次访问，
  **814.246807 visits/s**；全新保存身份约459.18/s。相对旧60秒基线约+8.54%，
  相对旧552.5秒基线约+7.40%。这是初测，不能把不同窗口当作最终配对性能结论。
- 结果：`build/serial-bfs-700/run-9mbcg7dr/`；冻结程序 `tracer-prefetch`。
  红/绿日志和边界说明在 `prefetch-notes.md`、`red.log`、`green.log`。

实际BFS/DFS actor/choice/pending/timeout回归28行通过（full/pages），
`python3 tests/native_search_actors.py --tracer build/serial-bfs-700/tracer-prefetch-np1`，
结果在 `build/native-search-actors/run-k03cqwst/`。`make check CC=/usr/bin/gcc
CXX=/usr/bin/g++` 也通过，包括14项构建契约和5项案例目录检查。
仍需更长/重复性能测量。新目标1000 visits/s尚未达到；下一步以profile为依据
削减不必要的状态复制和表达式类型处理。

## 后续候选（尚待最终组合验证）

- BFS源状态：队列弹出的optional在整次展开中拥有源状态；逐边改用const引用，
  消除同一源状态的重复深拷贝。初测60秒825.36 visits/s，较预取版增幅很小，
  单轮不足以量化该微优化的收益。
- 映射解析：复用 `build/shared-parallel/maps-parser/candidate.patch` 的生产部分，
  将3处scanf解析统一为from_chars/string_view；仍读取相同的/proc或已保存文本。
  该候选原先因没有改善并行扩展性而未合入，但当时单worker已约快5.2%。
  当前重新加入溢出回归，旧代码明确RED，新代码NP1映射full/pages/verify、tiny及
  state-I/O GREEN。当前两次无采样60秒为896.77、864.75 visits/s，仍须长测确认。
- DWARF：复用归档的exact-hit/const-reference候选及其测试。旧/新版本DWARF与
  StateView组件均通过；交错两轮分别为maps896.77/dwarf887.53、
  dwarf892.52/maps864.75。未见稳定优势，已撤回生产DWARF/AST改动，最终也移除该候选独有的新增
  类型/只读观察测试，原始补丁和测试日志仍留档。不能将候选的编译通过当作性能改善。
- StateView：只读保存视图首次read后持有该节点的已验证SnapshotView，避免每个
  字段重复访问全局LRU和store锁。metadata-only查询不提前加载完整根；stopped_live
  不缓存内存读取。每次saved read及read_into继续检查store generation。
  新测试在旧实现因重复open计数失败；新实现验证full/pages、分支/节点/copy、
  缺失根、关闭/重开及真实live原始读取均通过。每个视图最多持有NP个完整快照根，
  full模式的大镜像可能增加这段观察生命周期内的峰值内存，尚未测量其大规模峰值。

组件红/绿与独立复核日志：`maps-red.log`、`maps-green.log`、
`dwarf-baseline-tests.log`、`dwarf-candidate-tests.log`、`view-notes.md`、
`view-verify-build.log`。PGO只列入后续实验计划，尚未产生性能结果。

## 编译与调度放置候选

后续单轮60秒（均相同目标/插件、同前缀和budget，含StateView复用）：

| tracer构建 / 放置 | 原生累计访问/s |
|---|---:|
| 普通O3 / 默认亲和性 | 903.60 |
| PGO use / 默认亲和性 | 914.05 |
| PGO use / taskset -c2 | 965.53 |
| LTO / taskset -c2 | 927.62 |

均未达到1000。PGO先用原生BFS训练60秒，原子更新profile计数，再完整重编译use；
训练速度不计入优化结论。编译没有missing-profile/coverage-mismatch警告。PGO目录
显式指定为 `build/serial-bfs-700/pgo/{objects,profiles}`，编译保持普通O3/SSE4.2/
AVX2/BMI/BMI2，不使用fast-math或关闭检查。LTO使用单独build目录和-flto=4，
没有观察到稳定额外收益。taskset仅限制这次driver/tracer/tracees，不改变宿主策略。

LTO揭示了原有扫描器声明错误：expr_ast.cpp把yy_scan_string和yylex_destroy返回值
声明为void，而生成代码分别返回yy_buffer_state*及int。现已改成正确声明。
`-Werror=odr -Werror=lto-type-mismatch` 的旧实现链接RED（exit2），修复后GREEN；
记录在 `lto-odr-red.log` / `lto-odr-green.log`。未改生成的lexer/parser文件。

下一项为归档的PendingStatusReader：只复用绑定物理carrier的CLOEXEC fd，每次重新
rewind/read到EOF；不缓存pending结果。并尝试用string_view/from_chars替代逐行
字符串流解析。完整物理线程枚举、GETREGS、rseq、robust及信号校验必须保留。
实现及reader红/绿回归委托给DSH独立会话；父任务在其完成前不运行CPU实验。
尚未采信新实现或宣称其收益。之后需独立审查、线程生命周期/capture/实际BFS门禁，
再跑普通O3和进程级绑核的长窗口对照。**目标仍为1000累计访问/s，尚未达成。**


## 最后两项热路径调整与阶段补充

PendingStatusReader复用status FD后，from_chars解析替代字符串流；普通O3初测
966.28 visits/s。仅把tracer/后代绑CPU2、driver保持原亲和性，120秒985.18/s；
O2+mtune=native为981.36/s，未采用。元数据合并编码已通过旧格式oracle，但单轮
984.16/s没有显示可独立归因的吞吐改善，不将它包装成显著收益。

之后复用pool的 `/proc/<pid>/task` DIR：每次rewind并读到EOF，仍执行前后两次
完整枚举。[POSIX rewinddir语义](https://man7.org/linux/man-pages/man3/rewinddir.3p.html)
要求流反映当前目录状态。直接成员列表刷新、线程退出/增长、move/关闭、错误路径
及无rewind负例通过。其最终性能即文首三次数据。独立静态复核确认没有缓存线程
集合或pending结果。最终未采用PGO、LTO、O2/native调优及DWARF快路径。
