# DetSim

English | [中文](docs/zh/README.md)

DetSim is a deterministic simulation platform for testing, model checking, and debugging distributed systems.

DetSim executes real system implementations in a controlled environment by intercepting system calls and virtualizing sources of nondeterminism. It provides deterministic execution, global state management, state-space exploration, and interactive debugging without requiring intrusive modifications to the target system.

## Overview

Distributed systems are inherently difficult to test and debug because their behavior depends on concurrency, asynchronous communication, scheduling, timing, randomness, and other environmental factors. DetSim provides a controlled execution environment that turns these sources of nondeterminism into explicitly controllable inputs.

At a high level, DetSim:

<img src="docs/assets/DetSim framework.png" alt="DetSim framework" style="zoom: 50%;" />

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

For Ubuntu, the required packages can be installed with:

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

The target program must provide DWARF debugging information for expression evaluation and stack inspection.

DetSim currently expects the target program to be compiled with DWARF-4:

```sh
CFLAGS="-gdwarf-4 -g" make
```

## Build

Build DetSim with the number of target nodes specified by `NP`:

```sh
make NP=1
```

For a distributed system with multiple processes:

```sh
make NP=3
```

`NP` must match the `Nodes` value in the DetSim configuration file.

### Build the Target Program

For example:

```sh
gcc -gdwarf-4 -g -o test test.c
```

For a project using Make:

```sh
CFLAGS="-gdwarf-4 -g" make
```

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

## Execution Model

DetSim models a distributed execution as a sequence of global states:

```text
        S0
        │
        │ system call / scheduling decision
        ▼
        S1
        │
        │ system call / scheduling decision
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

Each state captures the relevant execution state of the distributed system. DetSim can save states during execution and later restore them to explore alternative execution paths.

## Interactive CLI

DetSim provides an interactive CLI for execution control and state inspection.

| Command     | Description                                 |
| ----------- | ------------------------------------------- |
| `bfs/dfs/rand`         | Continue automatic execution/exploration    |
| `si`        | Execute one step of the current process     |
| `sw N`      | Switch to process `N`                       |
| `p EXPR`    | Evaluate a C expression                     |
| `diff A B`  | Compare two execution states                |
| `load HASH` | Restore a specified state                   |
| `bt`        | Print the current call stack                |
| `info`      | Display current execution-state information |
| `q`         | Exit                                        |

See [CLI Documentation](docs/cli.md) for the complete command reference.

## Expression Evaluation

DetSim can evaluate C expressions using DWARF debugging information.

Basic variables:

```text
p g_value
```

Arrays and pointers:

```text
p g_arr[2]
p *g_ptr
p **g_double_ptr
```

Structure fields:

```text
p g_struct.field
p g_ptr->field
```

Type casts:

```text
p ((hidden_struct*)g_container.hidden_ptr)->secret_value
```

Expressions can also explicitly refer to another target process:

```text
p tracee1(g_remote_data)
```

See [Expression Evaluation](docs/expr.md) for details.

## State Comparison

DetSim can compare two saved execution states.

For example:

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

This is useful for identifying the first observable differences between alternative executions.

## Documentation

* [Expression Evaluation](docs/expr.md) — C expression evaluation and DWARF support
* [Command-Line Interface](docs/cli.md) — interactive CLI commands
* [Backtrace](docs/backtrace.md) — stack and call-stack analysis
* [Memory Management](docs/brk_and_mmap.md) — `brk`/`mmap` handling

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



## License

[To be determined]
