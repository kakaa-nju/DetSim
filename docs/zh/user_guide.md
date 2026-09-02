# DetSim 用户手册

本手册涵盖 DetSim 的完整使用指南，包括环境配置、编译选项、执行模型、交互式调试和高级功能。

## 环境要求

DetSim 目前针对 Linux 平台。

### 所需开发库

* `libdw`
* `libcereal-dev`
* `libdwarf-dev`
* `libelf-dev`
* `libreadline-dev`
* `libcjson-dev`
* `libunwind-dev`
* `libfmt-dev`
* `flex`
* `bison`

### 安装命令

在 Ubuntu 上，可以通过以下命令安装所需软件包：

```sh
sudo apt install \
    libdw-dev \
    libcereal-dev \
    libdwarf-dev \
    libelf-dev \
    libreadline-dev \
    libcjson-dev \
    libunwind-dev \
    libfmt-dev \
    flex \
    bison
```

### 调试信息要求

被测程序需要包含 DWARF 调试信息，以支持表达式求值和调用栈分析。
DetSim 当前要求被测程序使用 DWARF-4 编译：

```sh
gcc -gdwarf-4 -g -o test test.c
```
对于使用 Makefile 的项目：
```sh
CFLAGS="-gdwarf-4 -g" make
```

## 编译 DetSim

使用 `NP` 指定被测系统的节点数量：

```sh
make NP=1
```

对于包含多个节点的分布式系统：

```sh
make NP=3
```

`NP` 必须与配置文件中的 `Nodes` 保持一致。

## 使用 DetSim

### 1. 编译被测程序

例如：

```sh
gcc -gdwarf-4 -g -o test test.c
```

对于使用 Makefile 的项目：

```sh
CFLAGS="-gdwarf-4 -g" make
```

### 2. 创建配置文件

创建 `test.json`：

```json
{
  "Loglevel": 0,
  "Nodes": 1,
  "Tracee": [["./test", "0"]],
  "Addr": ["192.168.0.1"],
  "Assertions": ["tracee0(g_value == 42)"],
  "Checkpoint": ["write"]
}
```

其中：

* `Nodes` 指定被测系统的节点数量
* `Tracee` 指定被测程序及其节点编号

### 3. 启动

```sh
./tracer -c test.json
```

DetSim 将在确定性模拟执行环境中启动被测程序，并进入交互式调试界面。

> **注意：** 为保证地址空间布局的一致性，建议关闭 Linux 的地址空间随机化：
>
> ```sh
> sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'
> ```

## 执行模型

DetSim 将分布式系统的执行过程表示为一系列全局状态：

```text
        S0
        │
        │ 系统调用 / 调度决策
        ▼
        S1
        │
        │ 系统调用 / 调度决策
        ▼
        S2
       /  \
      /    \
     ▼      ▼
    S3      S4
    │       │
    ▼       ▼
   ...     ...
```

每个状态保存分布式系统执行过程中影响后续行为的系统状态。DetSim 可以在执行过程中保存状态，并在之后恢复这些状态，从而探索不同的执行路径。


## 交互式命令行

DetSim 提供交互式 CLI，用于控制执行过程和检查系统状态。

| 命令          | 描述           |
| ----------- | ------------ |
| `bfs/dfs/rand`| 自动继续执行/探索    |
| `si`        | 单步执行当前进程     |
| `sw N`      | 切换到第 `N` 个进程 |
| `p EXPR`    | 求值 C 表达式     |
| `diff A B`  | 比较两个执行状态     |
| `load HASH` | 恢复指定状态       |
| `bt`        | 打印当前调用栈      |
| `info`      | 显示当前执行状态信息   |
| `q`         | 退出           |

完整命令参考请参阅 [命令行接口文档](cli.md)。

## 表达式求值

DetSim 基于 DWARF 调试信息支持 C 表达式求值。

基本变量：

```text
p g_value
```

数组和指针：

```text
p g_arr[2]
p *g_ptr
p **g_double_ptr
```

结构体字段：

```text
p g_struct.field
p g_ptr->field
```

类型转换：

```text
p ((hidden_struct*)g_container.hidden_ptr)->secret_value
```

还可以显式指定其他被测进程：

```text
p tracee1(g_remote_data)
```

详细说明请参阅 [表达式求值文档](expr.md)。

## 状态比较

DetSim 可以比较两个已经保存的执行状态。

例如：

```text
(detsim) diff 80a3 a87f

=== State Diff: 80a3157e vs a87f8fb6 ===

[Overview]
  System state hash: 80a3157e != a87f8fb6
  Process count: 2

  [Process 0 - Registers]
    rax    0x0000000000000009   0x0000000000000001
    ...

  [Process 0 - Memory]
    0x404040: A=00 00 00 00  B=2a 00 00 00
```

状态比较可以用于定位不同执行路径之间首次出现的可观测状态差异。