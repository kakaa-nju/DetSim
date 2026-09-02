# DetSim

<p align="center">
  <a href="https://github.com/detsim-org/DetSim/blob/master/LICENSE">
    <img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License: Apache 2.0">
  </a>
</p>

[English](../../README.md) | 中文

DetSim 是一个确定性模拟平台，用于分布式系统的测试、模型检测和调试。

DetSim 通过拦截系统调用并虚拟化非确定性来源，在受控环境中执行真实的系统实现。它无需对目标系统进行侵入式修改，提供确定性执行控制、全局状态管理、状态空间探索和交互式调试功能。

## 概述

分布式系统的执行行为受到并发、异步通信、线程调度、时间、随机数以及其他环境因素的影响，因此系统缺陷往往依赖特定的执行时序，难以稳定触发和复现。
DetSim 提供了一个受控的执行环境，将这些非确定性因素转化为可控制的执行输入。

从整体上看，DetSim 的执行架构如下：


<img src="../assets/DetSim framework.png" alt="DetSim 框架图" style="zoom: 50%;" />

DetSim 将系统调用作为确定性执行片段之间的边界。通过控制系统调用输入以及多进程之间的执行顺序，DetSim 可以复现已有执行、探索替代执行路径，并恢复到之前保存的执行状态。

## 特性

* **确定性执行**
  控制调度和外部输入，使分布式系统的执行具有可重现性。

* **基于状态的探索与调试**
  保存、恢复、比较和探索全局执行状态，支持系统化的执行探索和确定性调试。

* **系统调用拦截与资源虚拟化**
  拦截系统调用并虚拟化非确定性资源（如网络、时间和随机性），无需对目标系统进行侵入式修改。

* **分布式系统支持**
  将多个进程协调为单个全局受控的执行单元，用于确定性测试、模型检测和调试。

## 环境要求

DetSim 目前针对 Linux 平台。

所需开发库：

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

## 快速开始

### 1. 编写被测程序

创建一个简单的 C 程序：

```c
#include <stdio.h>

int g_value = 42;

int main(void) {
    printf("value = %d\n", g_value);
    return 0;
}
```

使用调试信息编译：

```sh
gcc -gdwarf-4 -g -o test test.c
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

### 3. 编译 DetSim

```sh
make NP=1
```

### 4. 启动

```sh
./tracer -c test.json
```

DetSim 将在确定性模拟执行环境中启动被测程序，并进入交互式调试界面。

> **注意：** 为保证地址空间布局的一致性，建议关闭 Linux 的地址空间随机化：
>
> ```sh
> sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'
> ```


## 文档

* [用户手册](user_guide.md) — 完整的使用指南
* [表达式求值](expr.md) — C 表达式求值和 DWARF 支持
* [命令行接口](cli.md) — 交互式 CLI 命令参考
* [回溯](backtrace.md) — 调用栈分析
* [设计手册](index.md) — 完整的设计手册

## 项目结构

```text
.
├── src/
│   ├── core/          # 核心执行引擎、调度和状态管理
│   ├── subsys/        # 虚拟化子系统和资源管理
│   └── utils/         # 工具、解析器和表达式求值
├── tests/             # 测试用例
├── examples/          # 示例
├── docs/              # 项目文档
└── Makefile
```

## 研究用途

DetSim 面向分布式系统的确定性执行研究，主要应用场景包括：

* 分布式系统的确定性测试
* 分布式系统缺陷的确定性调试和复现

DetSim 工作于系统调用边界，通过拦截和控制系统调用对被测程序的执行过程进行确定性控制，无需对被测系统进行应用层面的侵入式插桩，也不依赖特定的编程语言或系统设计模式。
