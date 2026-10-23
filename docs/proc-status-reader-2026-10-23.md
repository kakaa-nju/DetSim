# 线程状态描述符复用实验（2026-10-23）

状态：本机验证、WSL depth16 两轮成对计时及结果审计已完成。候选有小幅绝对
耗时下降，但没有改善并行加速比；暂不合入产品代码，也不宣称接近线性。

## 改动与不变量

每个 CarrierPool 持有物理线程的 `/proc/<tid>/status` 描述符。线程加入池时
立即打开并验证 CLOEXEC；逻辑线程退出但物理 carrier 停驻时继续保留，清空池时
通过 RAII 关闭。每次检查仍执行 lseek 到零、read 到 EOF 和原来的信号位图解析。
不缓存 pending signal 结果，不因读取失败按相同数字 TID 重新打开。

校验保留前后物理线程枚举、GETREGS、rseq 和 robust-list 检查，并要求 reader
持有的 TID 集合与 carrier 集合一致。独立 sigprocmask 和创建 carrier 时的 anchor
检查仍使用原来的 fresh-open 路径。本优化没有覆盖全部 pending-signal 检查。

reader 只供 tracer 控制线程串行使用。初始 vector 预留 64 个槽，使成功 retain
后的合法增长不会再因 vector 分配而失败。但 native clone 已成功、retain 失败时
仍是显式失败而非完整回滚；不能把它描述成事务式线程创建。

## 已执行验证

实验文件位于 `build/shared-parallel/proc-status-reuse/`，均为私有产物。

| 命令/证据 | 实际覆盖及结果 |
| --- | --- |
| `python3 build/shared-parallel/proc-status-reuse/build.py` | 同一冻结源码编译 NP3 的 5 个受影响 TU；原有 thread-control 测试基线/候选通过 |
| `python3 build/shared-parallel/proc-status-reuse/build_np1.py` | 全部 56 个 TU 以 NP1 重编译；线程身份、carrier、生命周期 full/pages 与 tiny-cache 通过 |
| `python3 build/shared-parallel/proc-status-reuse/recheck_np1.py` | 后续错误处理修订后重编译两个变化 TU，上述 NP1 测试再次通过 |
| `python3 build/shared-parallel/proc-status-reuse/run_reader_test.py` | 冻结专项测试源码，链接真实候选对象，包装 open/lseek/read/close；退出 0 |
| `python3 build/shared-parallel/proc-status-reuse/reader_mutation.py` | 独立对象故意删除 rewind，测试因未执行 rewind 退出 1；正常实现退出 0 |
| `native-gates-reviewed.log` | 当前 NP3 候选的 7 项原生场景全部通过，20.932 秒 |
| `native-reference12-command.json` | 当前二进制的独立 depth12 参考：10457 状态、18266 边及尝试边、零重试，完整图摘要与基线一致 |

专项测试覆盖真实非 leader 线程、信号置位与清除、每次读取次数、未知/重复 TID、
描述符移动/关闭、缺字段及非法文本、部分读取后失败、seek/read 故障、EINTR、
非零 seek 返回拒绝，以及 reader 集合不一致。它没有独立构造完整合法的三参数
ptrace barrier；相应整合行为由上述线程/原生场景覆盖，不能把 reader 单测当作
完整恢复正确性的证明。内核 FD 新鲜性、PID namespace 内强制复用和非 leader
退出另有本机/WSL 探针，见实验目录中的 JSON；这些探针也不是产品集成测试。

当前候选 SHA256：
`90519c636e6844f4a07174521184f4cd1f53d8e12142a5da172b925ac6efa1e9`。
本机 depth12 完整图 SHA256：
`56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。

## 性能测量口径及当前限制

WSL 测量目录为 `build/shared-parallel/proc-status-reuse-wsl/`。基线使用此前的
类型化 shared-pool cache 二进制，候选只增加 reader 改动；传输后核对二进制及
脚本 SHA256。每种二进制各生成自己的 depth16 参考，随后 1/12 workers 两轮
交错顺序运行。固定 Raft-library 输入、pages/shared、BFS、seed1、same-cpu。
每次要求 63418 状态、112305 边及尝试边、零重试以及相同完整规范图。

分别报告 native 搜索命令耗时，以及搜索命令加验证后延迟执行的关闭耗时；后者
是两个不连续时间段之和，不是启动到退出的总墙钟时间。参考生成额外执行的串行
搜索不计入这两个指标。两轮测量只能作为候选筛选，不能宣称统计显著或一般上限。

既有 DWARF 样本重新按任务内窗口归因后，1/12 workers 的内核 leaf 样本中，
pending-signal 检查分别有 217/288 个，线程枚举 164/229 个，read_regions
130/170 个。rep_movs_alternative 中有 75/116 个归属于 read_regions。
这些是最近可识别 tracer 祖先归因的样本数，不是调用次数；栈仍存在未知尾部。
其中 status 文本生成成本不会被 FD 复用消除，因此不能把约 11% 的 pending 检查
CPU 占比全部当作本优化的潜在收益。

## 完成的 WSL 结果与决定

10 次搜索（两份独立参考加八次计时）均通过。`audit.py` 独立读取结果并重新哈希
每个完整图文件，核对候选/基线二进制、共同输入、深度、worker 数、节点/边数、
零重试及关闭时间的加法关系；没有以结果中的 passed 字段代替这些检查。

两轮中位数（秒）：

| 范围 | workers | 基线 | FD reader | 耗时变化 |
| --- | ---: | ---: | ---: | ---: |
| native 搜索命令 | 1 | 143.93277 | 139.73912 | -2.91% |
| native 搜索命令 | 12 | 18.46398 | 18.10119 | -1.97% |
| 搜索加延后关闭 | 1 | 144.14735 | 140.00381 | -2.87% |
| 搜索加延后关闭 | 12 | 19.33019 | 18.96747 | -1.88% |

12-worker 搜索命令两轮分别下降 1.63% / 2.30%；加入关闭后分别下降 0.25% /
3.46%，说明关闭时段会影响小收益的判断。只有两轮，不声称统计显著性。
搜索命令加速比约从 7.80× 变为 7.72×；搜索加延后关闭从 7.46× 变为 7.38×。
因此这是一个小幅吞吐候选，未解决并发效率下降。先保留私有候选和证据，暂不引入
产品的描述符生命周期及新接口；后续可与其他候选统一评估是否值得维护。

`post-run-verification.json` 核对传输前后的四项二进制/脚本摘要一致、三项私有
挂载卸载成功、没有本次 owned 进程及宿主 rootfs 子挂载残留。原始结果、完整图、
每次命令、汇总与成对变化保存在 `build/shared-parallel/proc-status-reuse-wsl/`。

单独发现的线程枚举 I/O 错误已记录到 correctness backlog：故障注入证明当前
get_thread_list 忽略 readdir EIO。它未混入本轮候选，也尚未修复。
