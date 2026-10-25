# 单实例 BFS 吞吐优化（2026-10-25，进行中）

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
等价。当前仅建立基线，尚不声明本轮完成或达成性能改进。

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
