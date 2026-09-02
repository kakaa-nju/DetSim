# DetSim User Guide

This guide covers the complete usage of DetSim, including environment setup, build options, execution model, interactive debugging, and advanced features.

## Requirements

DetSim currently targets the Linux platform.

### Required Development Libraries

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

### Installation

On Ubuntu, install the required packages with:

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

### Debug Information

The target program must include DWARF debug information to support expression evaluation and call stack analysis.
DetSim currently requires the target program to be compiled with DWARF-4:

```sh
gcc -gdwarf-4 -g -o test test.c
```

For projects using a Makefile:

```sh
CFLAGS="-gdwarf-4 -g" make
```

## Building DetSim

Use `NP` to specify the number of nodes in the target system:

```sh
make NP=1
```

For distributed systems with multiple nodes:

```sh
make NP=3
```

`NP` must match the `Nodes` field in the configuration file.

## Using DetSim

### 1. Build the Target Program

For example:

```sh
gcc -gdwarf-4 -g -o test test.c
```

For projects using a Makefile:

```sh
CFLAGS="-gdwarf-4 -g" make
```

### 2. Create a Configuration File

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

Where:

* `Nodes` specifies the number of nodes in the target system
* `Tracee` specifies the target program and its node ID

### 3. Launch

```sh
./tracer -c test.json
```

DetSim will launch the target program in a deterministic simulation environment and enter the interactive debugging interface.

> **Note:** To ensure consistent address space layout, it is recommended to disable Linux address space randomization:
>
> ```sh
> sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'
> ```

## Execution Model

DetSim represents the execution of a distributed system as a sequence of global states:

```text
        S0
        │
        │ syscall / scheduling decision
        ▼
        S1
        │
        │ syscall / scheduling decision
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

Each state captures the system state that affects subsequent behavior during distributed system execution. DetSim can save states during execution and restore them later to explore different execution paths.

## Interactive Command Line

DetSim provides an interactive CLI for controlling execution and inspecting system state.

| Command        | Description                          |
| -------------- | ------------------------------------ |
| `bfs/dfs/rand` | Automatic execution / exploration    |
| `si`           | Single-step the current process      |
| `sw N`         | Switch to process `N`                |
| `p EXPR`       | Evaluate a C expression              |
| `diff A B`     | Compare two execution states         |
| `load HASH`    | Restore a specific state             |
| `bt`           | Print the current call stack         |
| `info`         | Display current execution state info |
| `q`            | Quit                                 |

For a complete command reference, see the [CLI documentation](../zh/cli.md).

## Expression Evaluation

DetSim supports C expression evaluation based on DWARF debug information.

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

Struct fields:

```text
p g_struct.field
p g_ptr->field
```

Type casts:

```text
p ((hidden_struct*)g_container.hidden_ptr)->secret_value
```

You can also explicitly specify another tracee process:

```text
p tracee1(g_remote_data)
```

For detailed information, see the [Expression Evaluation documentation](../zh/expr.md).

## State Comparison

DetSim can compare two previously saved execution states.

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

State comparison can be used to locate the first observable state divergence between different execution paths.