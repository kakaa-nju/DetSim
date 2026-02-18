# 命令行接口 (CLI)

DetSim 提供交互式命令行界面用于调试和状态探索。

## 启动

```sh
./tracer -c tests/config.json
```

在自动探索模式中按 `Ctrl-C` 会发送 SIGINT，停止探索并回到交互界面。

## 基础命令

### 执行控制

| 命令 | 描述 |
|------|------|
| `c` | 自动探索模式，从当前状态开始自动执行 |
| `si` | 单步执行，当前进程向前执行一个系统调用 |
| `sw N` | 切换到第 N 个进程 (0-indexed) |
| `q` | 退出程序 |

### 状态管理

| 命令 | 描述 |
|------|------|
| `info` | 显示当前状态信息，包括系统调用历史 |
| `load HASH` | 加载指定 HASH 前缀的全局状态 |
| `diff [options] HASH1 HASH2` | 比较两个全局状态的差异 |

**diff 命令选项**:
- `-b, --brief`: 仅显示概览，跳过详细差异
- `-m, --memory-only`: 仅比较内存内容
- `--max-diff=N`: 每部分最多显示 N 个差异 (默认 10)

**diff 输出示例**:
```
(ptmc) diff 1f3f 3238

=== State Diff: 1f3f3246 vs 323803d5 ===

[Overview]
  System state hash: 1f3f3246 != 323803d5 ✗
  Process count: 2
  Process 0: ts_hash f60152ec a15d5492, exited=✗/running running
  Process 1: ts_hash e93e60aa 93655747, exited=✗/running running

  [Process 0]
    ts_hash: 8c38ae4b != de260c5c
    pid: 1308513 != 1308577
    brk: 0x27c0e000 != 0x26231000
  [Process 0 - Registers]
    rax    0x0000000000000009   0x0000000000000001
    rbx    0x00007ab79427e128   0x00007ab79427e000
    ...
```

## 调试命令

### 表达式求值

| 命令 | 描述 |
|------|------|
| `p EXPRESSION` | 计算并打印表达式值 |

表达式支持完整的 C 语法，详见 [表达式求值文档](expr.md)。

**示例**:
```
(ptmc) p g_int
g_int = 42

(ptmc) p g_basic_ptr->x
g_basic_ptr->x = 100

(ptmc) p ((point*)g_generic_ptr)->x
((point*)g_generic_ptr)->x = 10

(ptmc) p tracee1(g_container.num_rects)
tracee1(g_container.num_rects) = 2
```

### 内存检查

| 命令 | 描述 |
|------|------|
| `x ADDR` | 打印指定内存地址的内容 (16字节) |
| `hexdump PATH` | 以十六进制格式打印文件内容 |

### 调用栈

| 命令 | 描述 |
|------|------|
| `bt` | 打印当前进程的调用栈 (backtrace) |
| `frame N` | 切换到第 N 个栈帧 |
| `locals` | 显示当前帧的局部变量 |

**bt 输出示例**:
```
(ptmc) bt
#0  0x0000000000401176 in main () at test.c:10
#1  0x00007ffff7a29d90 in __libc_start_main () from /lib/x86_64-linux-gnu/libc.so.6
#2  0x000000000040105e in _start ()
```

## 文件系统

| 命令 | 描述 |
|------|------|
| `ls` | 列出当前进程文件系统中的文件 |
| `stat PATTERN` | 显示匹配模式的文件状态信息 |

## 批处理

| 命令 | 描述 |
|------|------|
| `batch FILE` | 从文件批量读取命令 |

**示例**:
```
(ptmc) batch commands.txt
```

其中 `commands.txt` 包含每行一个命令。
