# DetSim

DetSim 是一个确定性模拟测试环境，用于分布式系统的确定性测试和调试。

## 特性

- **确定性执行**: 通过控制调度消除非确定性，确保测试可重复
- **全局状态管理**: 保存/恢复任意执行点的完整系统状态
- **表达式求值**: 类似 GDB 的表达式求值，支持结构体、指针、类型转换
- **状态比较**: 对比不同执行路径的系统状态差异
- **分布式支持**: 支持多进程分布式系统的统一调试

## 依赖

- libdw
- libcereal-dev
- libdwarf-dev
- libelf-dev
- libreadline-dev
- libcjson-dev
- libunwind-dev
- libfmt-dev
- flex, bison

## 编译

```sh
make NP=$(NODES)
```

`NP` 指定被测系统的节点（进程）数量，必须与配置文件中的 `Nodes` 一致。

### 编译被测程序

被测程序需要使用 `-gdwarf-4` 编译以保留调试信息：

```sh
CFLAGS="-gdwarf-4 -g" make
```

## 启动

为确保分析准确，需要关闭 Linux 系统的地址空间随机化机制：

```sh
sudo echo 0 > /proc/sys/kernel/randomize_va_space
```

### 运行测试

```sh
./tracer -c tests/expr_test.json
```

`tests/` 目录下提供了多个测试示例。

## 快速开始

### 1. 编写被测程序

```c
// test.c
#include <stdio.h>

int g_value = 42;

int main() {
    printf("value = %d\n", g_value);
    return 0;
}
```

编译：
```sh
gcc -gdwarf-4 -g -o test test.c
```

### 2. 编写配置文件

```json
{
  "Loglevel": 0,
  "Nodes": 1,
  "Tracee": [["./test", "0"]],
  "Addr": ["192.168.0.1"],
  "Assertions": ["tracee0(g_value == 42)"]
}
```

### 3. 运行测试

```sh
make NP=1
./tracer -c test.json -a
```

## 命令行界面

DetSim 提供交互式 CLI 用于调试：

| 命令 | 描述 |
|------|------|
| `c` | 自动探索模式 |
| `si` | 单步执行当前进程 |
| `sw N` | 切换到第 N 个进程 |
| `p EXPR` | 求值表达式 |
| `diff A B` | 比较两个状态 |
| `load HASH` | 加载指定状态 |
| `bt` | 打印调用栈 |
| `info` | 显示当前状态信息 |
| `q` | 退出 |

详细文档请参考 [CLI 文档](docs/cli.md)。

## 表达式求值

支持完整的 C 表达式语法：

```
# 基本变量
p g_value

# 数组和指针
p g_arr[2]
p *g_ptr
p **g_double_ptr

# 结构体访问
p g_struct.field
p g_ptr->field

# 类型转换（将 void* 转回结构体指针）
p ((hidden_struct*)g_container.hidden_ptr)->secret_value

# 进程限定
p tracee1(g_remote_data)
```

详见 [表达式求值文档](docs/expr.md)。

## 状态比较

使用 `diff` 命令比较两个系统状态：

```
(ptmc) diff 80a3 a87f

=== State Diff: 80a3157e vs a87f8fb6 ===

[Overview]
  System state hash: 80a3157e != a87f8fb6 ✗
  Process count: 2

  [Process 0 - Registers]
    rax    0x0000000000000009   0x0000000000000001
    ...

  [Process 0 - Memory]
    0x404040: A=00 00 00 00  B=2a 00 00 00
```

## 项目结构

```
.
├── src/
│   ├── core/       # 核心功能（调度、状态管理、DWARF解析）
│   ├── subsys/     # 子系统（文件系统、网络状态序列化）
│   └── utils/      # 工具（表达式解析器）
├── tests/          # 测试用例
├── docs/           # 文档
└── Makefile
```

## 文档

- [表达式求值](docs/expr.md) - 详细表达式语法
- [命令行接口](docs/cli.md) - CLI 命令参考
- [回溯](docs/backtrace.md) - 调用栈分析
- [内存管理](docs/brk_and_mmap.md) - brk/mmap 处理

## 许可证

[待补充]
