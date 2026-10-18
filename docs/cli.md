# 命令行与 TUI

DetSim 有两种手动界面：默认的 ncurses TUI，以及 `-n` 的 readline/批处理界面。
两者现在使用同一张命令表和同一个解析器；TUI 不再是另一套旧命令集。

## 启动

```sh
./tracer -c config.json          # ncurses TUI
./tracer -n -c config.json       # readline
./tracer -n -c config.json -b commands   # 批处理，commands 每行一条
```

TUI 在手动模式下默认选中 process 0；多进程用 `sw N` 切换。自动探索中按
`Ctrl-C` 会发送 SIGINT，停止搜索并回到交互界面。

TUI 状态区显示搜索统计、存储队列、缓存、错误预算和线程/pending 摘要。
第7/8行是选中 process 的 live thread panel：`T0:run | T1:wait:futex@1` 等，
每次命令后以及自动搜索轮询时刷新。`make NP=1 check-tui` 通过 PTY 验证最小终端、
缩放以及 `help/pending/inspect/thread/q`；状态区在过小终端上会收缩，不再用
负高度建窗。

## 命令

| 命令 | 描述 |
|------|------|
| `help [COMMAND]` | 列出命令或显示单条说明 |
| `bfs [--quiet]` | 从当前状态做内建 BFS |
| `dfs <depth> [--quiet]` | 从当前状态做内建迭代 DFS，depth 是剩余深度 |
| `rand <depth> [--quiet]` | 随机执行 depth 步 |
| `si [CHOICE\|timeout]` | 执行当前 actor 的一个 syscall；timeout 完成有限 deadline |
| `sw N` | 切换进程焦点（0-indexed） |
| `thread [N]` | 列出线程，或切换到线程 N |
| `pending [PROCESS]` | 列出所有阻塞/可完成 syscall、原因、deadline 和 runnable actor |
| `inspect [PROCESS]` | 输出保存状态的 schema1 JSON（线程/寄存器/pending/futex/描述符/文件/socket） |
| `plugins` | 列出已加载插件、ABI/monitor schema、time-choice 和当前 monitor 字节 |
| `parallel N DEPTH [--strategy dfs|bfs] [--retries N] [--timeout-ms N]` | 使用统一 frontier 和持久 worker 搜索，完整提交一步的所有 actor/choice |
| `tree` | 打印已记录的搜索边（source target process choice thread） |
| `load [--adopt|--adopt-threads] HASH` | 按 hash 前缀加载保存状态；`--adopt` 允许匹配 layout 的新进程接管 seed snapshot，`--adopt-threads` 可创建缺失 carrier |
| `info` / `info current` | 系统调用历史 / 当前边的单行摘要 |
| `info sock` | 当前模拟 socket |
| `p EXPR` | 表达式求值 |
| `x/[COUNT][FORMAT][SIZE] ADDR` | 读内存；支持 x,d,u,o 和 b,h,w,g |
| `bt` / `frame N` / `locals` | DWARF 栈、栈帧和局部变量 |
| `ls` / `stat PATTERN` / `hexdump PATH` | 保存的文件系统视图 |
| `batch FILE` | 从文件执行命令 |
| `q` | 退出 |
| `diff [--brief] [--max-diff=N] H1 H2` | 比较两个保存状态的 StateView JSON；`-m` 明确不可用 |

`diff` 只比较保存状态的 `StateView` JSON（寄存器/线程/pending/描述符/文件/
socket 等），不是原始内存逐字节 diff。`--brief` 只列变化的顶层 section；
`--max-diff=N` 限制叶子差异数（默认10）。哈希参数是 `SysStateStore` 的
前缀，必须唯一。`-m/--memory-only` 会明确打印 memory-only 不可用，不会
静默降级。

## 状态观察

`pending` 是阻塞态的摘要；`inspect [PROCESS]` 输出完整的保存状态 JSON，
包括每个逻辑线程的寄存器、pending syscall/phase/deadline、futex 等待队列、
信号掩码、robust/clear-tid 注册、描述符、文件、socket、pipe 和虚拟映射。
它只读保存状态，不回退到 live memory；不可观察时打印 `inspect unavailable:`。
输出不受旧的 4 KiB `ui_printf` 限制，TUI 内容窗口可直接滚动查看。

`plugins` 显示已加载模块 ID、ABI、monitor schema、是否为 time-choice，以及当前
保存状态中每个 monitor key 的字节数；无插件时明确显示 `No plugins loaded`。

### 原生并行探索

配置 `RuntimeProfile.fixed_layout=true`，然后运行：

```text
parallel 4 100 --strategy bfs --retries 2 --timeout-ms 30000
```

框架只固定目标进程布局，宿主 ASLR 保持开启。`N` 为 1..32；深度可以为0，
此时仅判断终点/截断，不执行转移。DFS 使用后进先出分派，BFS 有层屏障；并行DFS
的完成顺序不等同于串行DFS。C++ `Frontier::Priority` 可提供启发式，CLI目前提供DFS/BFS。
旧 `--split-depth`、`--threads` 已移除：不再串行切分固定数量分支，线程接管自动进行。

协调器维护全局去重、深度和租约；worker逐状态枚举所有支持的actor/choice。
完整快照闭包经socket传输，导入验证后才接受边；只有完整Done才能标记源状态已展开。
失联/超时任务可重新分派，恢复异常、模型不匹配和协议错误明确失败，不继续损坏worker。
重试次数按源状态计，超过上限不能报告complete。

输出 `PARALLEL_RESULT` JSON，status为 `complete`、`bounded`、`violation` 或 `failed`。
`edges` 是唯一验证边，`attempted_edges` 包含重试/重展开；`expansions` 包含终点和边界probe
的完成提交。`seconds` 包括自动搜索中的worker启动、通信、导入及清理，不含此前手动前缀。
CLI/batch进程退出0不能代替此receipt。`.parallel-*/journal.jsonl` 保存实际边、租约、
worker PID、结束后的节点深度和阶段；日志与worker存储保留供复核。

启动兼容清单覆盖引擎/动态库、目标映像和映射、原始环境、CPU/xstate、身份、资源限制、
目标原生cwd，并纳入fixed-profile状态的model identity。worker使用缓存的原始环境，
不继承readline等后续修改。`--target-cwd /absolute/path` 单独固定目标exec目录，
不会改变tracer的存储目录；默认是tracer启动cwd。普通 `WorkingDir` 仍是虚拟FS目录。

`load --adopt HASH` 要求一致的逻辑线程布局；`load --adopt-threads HASH` 能创建停驻carrier，
支持反复导入及主线程/全部逻辑线程退出。普通 `load` 继续要求本地绑定。跨启动载入必须
保证完整store闭包、固定布局和模型兼容；旧fixed-profile快照不会自动迁移到新模型。

这是一套同机、同规范seed的并行执行机制，不会把独立冷启动的随机字节或不同地址中的
指针自动归一化。直接硬件输入（如RDTSC/CPUID）仍有边界，兼容清单不证明任意二进制
确定性。最新验收和性能限制见[并行探索报告](parallel-exploration.md)。

## 阻塞 syscall 的行为

阻塞调用不会在执行 `si` 时推进用户代码。actor 进入 pending：

- `Waiting`：没有可完成的返回值，自己的 `si` 会失败并保留源状态；
- `Retry`：I/O 条件可能已变化，下一次 `si` 重新检查；
- `Ready`：已经有结果，下一次 `si 0` 只发布结果，不执行后续用户指令。

`pending` 会显示 `futex(...) reason=futex phase=waiting`、deadline，并给出下一步：

```
(ptmc) pending
Pending syscalls:
Process 0 (pid=3540325) [selected]:
* [0] TID 1 state=waiting (main) futex(0x40405c, 0x80, 0x0, 0x0, 0x0, 0x0) reason=futex phase=waiting
       action: another actor must wake/complete this wait; use 'thread N' to switch
  [1] TID 2 state=runnable
Pending syscalls: 1
Runnable actors: 0:1
```

阻塞期间**可以执行其他 actor**：`thread N` 切到 runnable 线程后 `si`；waker、
peer read/write/close 或 `si timeout` 使等待者变 Ready，再切回并 `si 0`。
如果所有线程都在 Waiting 且没有 deadline/peer，`pending` 会明确显示没有
runnable actor，这就是死锁/等待外部输入状态。

有限 deadline 的完成只由显式 `si timeout` 触发：它把该进程的模型时钟推进到
deadline 并完成 pending，不自动推进时间，也不执行后续 guest 指令。无 deadline
的 futex 只能由另一个 actor 的 wake 完成。

原生 futex 验证示例（`tests/native_search_actors.c` 的 `w` 模式）：

```
si 0                      # main: futex WAIT <pending:waiting>
thread 1; si 0            # worker: nanosleep <pending:waiting deadline=...>
si timeout                # worker sleep 到期
si 0                      # worker write B
si 0                      # worker futex WAKE
thread 0; si 0            # main futex WAIT 返回，执行继续
```

`make NP=1 check-tui` 覆盖 TUI 命令/缩放；`tests/tracer_scenarios.py` 的
pending listing 测试覆盖 `thread`/`pending` 和 `si timeout`。
