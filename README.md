# DetSim

<p align="center">
  <a href="https://github.com/detsim-org/DetSim/blob/master/LICENSE">
    <img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License: Apache 2.0">
  </a>
</p>

English | [中文](docs/zh/README.md)

DetSim is a deterministic simulation platform for testing, model checking, and debugging distributed systems.

DetSim executes real system implementations in a controlled environment by intercepting system calls and virtualizing sources of nondeterminism. It provides deterministic execution, global state management, state-space exploration, and interactive debugging without requiring intrusive modifications to the target system.

## Overview

Distributed systems are inherently difficult to test and debug because their behavior depends on concurrency, asynchronous communication, scheduling, timing, randomness, and other environmental factors. DetSim provides a controlled execution environment that turns these sources of nondeterminism into explicitly controllable inputs.

At a high level, DetSim:

<div align="center">
  <img src="docs/assets/DetSim framework.png" alt="DetSim framework" style="width: 50%;" />
</div>

DetSim treats system calls as boundaries between deterministic execution segments. By controlling system-call inputs and the execution order of processes, DetSim can reproduce an execution, explore alternative executions, and restore previously reached states.

## Features

* **Deterministic Execution**
  Control scheduling and external inputs to make distributed-system executions reproducible.

* **State-Based Exploration and Debugging**
  Save, restore, compare, and explore global execution states, supporting systematic execution exploration and deterministic debugging.

* **System Call Interception and Resource Virtualization**
  Intercept system calls and virtualize nondeterministic resources such as networking, time, and randomness without requiring intrusive modifications to the target system.

* **Distributed-System Support**
  Coordinate multiple processes as a single globally controlled execution for deterministic testing, model checking, and debugging.


## Prerequisites

DetSim currently targets Linux.

Required development libraries:

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


## Quick Start

### 1. Write a Target Program

Create a simple C program:

```c
#include <stdio.h>

int g_value = 42;

int main(void) {
    printf("value = %d\n", g_value);
    return 0;
}
```

Compile it with debugging information:

```sh
gcc -gdwarf-4 -g -o test test.c
```

### 2. Create a Configuration

Create `test.json`:

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

* `Nodes` specifies the number of target processes. 
* `Tracee` specifies the executable and its node identifier.

### 3. Build DetSim

```sh
make NP=1
```

### 4. Run

```sh
./tracer -c test.json
```

DetSim starts the target program inside the deterministic execution environment and provides an interactive debugging interface.

> **Note:** For deterministic execution, it may be necessary to disable Linux address-space layout randomization:
>
> ```sh
> sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'
> ```

## Documentation

* [User Guide](docs/en/user_guide.md) — C expression evaluation and DWARF support
* [Expression Evaluation](docs/zh/expr.md) — C expression evaluation and DWARF support
* [Command-Line Interface](docs/zh/cli.md) — interactive CLI commands
* [Backtrace](docs/zh/backtrace.md) — stack and call-stack analysis
* [Design Manual](docs/zh/index.md) — Comprehensive design documentation and architecture reference

## Project Structure

```text
.
├── src/
│   ├── core/          # Core execution engine, scheduling, state management
│   ├── subsys/        # Virtualized subsystems and resource management
│   └── utils/         # Utilities, parsers, and expression evaluation
├── tests/             # Test cases 
├── examples/          # examples
├── docs/              # Documentation
└── Makefile
```

## Research

DetSim is designed as a research platform for deterministic execution of distributed systems. Its primary use cases include:

* deterministic testing of distributed systems;
* deterministic debugging and failure reproduction;

DetSim operates at the system-call boundary and controls the execution of the target program by intercepting and controlling system calls. It requires neither intrusive instrumentation at the application level nor a specific programming language or system design pattern.
