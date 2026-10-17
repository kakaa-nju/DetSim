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
| `parallel N DEPTH` | 从当前状态枚举后继，起 N 个 fixed-layout tracer worker 各搜一个分支 |
| `load [--adopt] HASH` | 按 hash 前缀加载保存状态；`--adopt` 允许匹配 layout 的新进程接管 seed snapshot |
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

### 跨进程 seed worker（实验）

`load --adopt HASH` 用于并行探索的 seed-worker：一个 tracer 捕获 snapshot 后，
其他 tracer 启动同一 executable，跳过本地 capture provenance 的硬拒绝，把 seed
snapshot 绑定到本地 carrier。它要求：

- 所有 tracer 和 tracee 使用同一固定地址布局（例如 `setarch x86_64 -R ./tracer`）；
  `load --adopt` 会在恢复前拒绝非 fixed-layout tracee，而不是让 brk/mapping
  restore 走到一半才失败；
- 本地 logical thread layout 与 snapshot 完全一致（当前 fresh worker 只验证了
  single-thread；多线程需要 worker 先构造同样 layout）；
- model identity/executable 一致，store 已复制到 worker cwd；
- 这是显式 opt-in，普通 `load` 仍要求同进程 capture provenance。

已经有 full/pages 回归：同一 seed checkpoint 启动两个 worker，`load --adopt` 后执行
同一 `si`，得到相同 successor hash。

`parallel N DEPTH` 现在把这一步接成可执行的 coordinator：从当前状态用
`enumerate_successors` 枚举 actor/choice 后继，复制 store 到 worker 目录，用
`posix_spawn` 起 N 个 fixed-layout worker；每个 worker `load --adopt` 自己的分支后
跑内建 `dfs DEPTH-1`。worker 输出/artifacts 保留在 `.parallel-<pid>-<time>/`。
它仍是 **branch-partitioned seed-worker**：没有共享 frontier、跨 worker 去重/merge，
worker 失败只汇总不重试；NP=2 root 的 branch 分工已验证。跨启动 raw hash 仍受
loader RDTSC/CPUID 差异影响，不能用冷启动 hash 直接当同一状态。

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
