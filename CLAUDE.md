# DetSim - Deterministic Simulation Testing Framework

DetSim is a deterministic simulation testing (DST) framework for distributed systems. It uses ptrace-based syscall interception to control and observe process execution, enabling reproducible testing of concurrent and distributed behaviors.

## Project Overview

**Purpose**: Provide deterministic execution of multi-process distributed systems for testing and verification.

**Key Capabilities**:
- Ptrace-based syscall interception and emulation
- Deterministic scheduling of threads and processes
- State checkpoint/restore for state space exploration (BFS/DFS/random)
- Network emulation for distributed systems testing
- Futex emulation for deterministic synchronization

## Directory Structure

```
src/
├── core/                    # Core simulation engine
│   ├── engine/              # Execution engine
│   │   ├── exec_engine.{h,cpp}       - Integrated execution coordination
│   │   ├── thread_manager.{h,cpp}    - Thread lifecycle & scheduling
│   │   └── signal_handler.{h,cpp}    - Signal classification & handling
│   ├── scheduler/           # Exploration strategies
│   │   ├── scheduler.h               - Scheduler interface
│   │   └── exploration.cpp           - BFS/DFS/RAND implementations
│   ├── state/               # State management
│   │   ├── state.{h,cpp}             - Core state structures
│   │   ├── state_store.{h,cpp}       - State storage & retrieval
│   │   ├── state_store_packed.{h,cpp}- Packed state storage
│   │   ├── sysstate_store.{h,cpp}    - System-wide state store
│   │   ├── state_transition.{h,cpp}  - State transition logic
│   │   └── serialize.cpp             - State serialization
│   ├── syscall/             # Syscall handling
│   │   ├── dispatcher.{h,cpp}        - Syscall routing
│   │   └── handlers.{h,cpp}          - Category handlers
│   ├── config.{h,cpp}       - Configuration parsing
│   ├── dwarf.{h,cpp}        - Debug info (DWARF) parsing
│   ├── guest.{h,cpp}        - Low-level ptrace operations
│   ├── monitor.{h,cpp}      - CLI/TUI interface
│   ├── proc_status.h        - Process status definitions
│   ├── scheduler.{h,cpp}    - Exploration scheduler (BFS/DFS)
│   ├── syscall_fmt.{h,cpp}  - Syscall formatting/display
│   ├── types.h              - Core type definitions
│   └── main.cpp             - Entry point
│
├── subsys/                  # Subsystem implementations
│   ├── fs/                  # Filesystem emulation
│   │   ├── fd_manager.{h,cpp}        - File descriptor management
│   │   └── fsstate.{h,cpp}           - Filesystem state
│   ├── net/                 # Network emulation
│   │   ├── emu.{h,cpp}               - Network emulation
│   │   └── sockstate.{h,cpp}         - Socket state management
│   └── sync/                # Synchronization emulation
│       └── futexstate.{h,cpp}        - Futex emulation
│
├── ui/                      # User interface
│   ├── cli/                 # CLI command implementations
│   │   ├── commands.{h,cpp}          - Command handlers
│   ├── log_wrapper.{h,cpp}  - Logging abstraction
│   └── ncurses_ui.{h,cpp}   - TUI implementation
│
└── utils/                   # Utilities
    ├── common.h             - Common macros/definitions
    ├── debug.h              - Debug utilities
    ├── expr*.{h,cpp,l,y}    - Expression evaluator (lex/yacc)
    ├── file_lock.{h,cpp}    - Process singleton lock
    └── utils.{h,cpp}        - General utilities
```

## Architecture

### Core Concepts

1. **Tracee**: A process being traced/simulated (the "guest")
2. **State**: A snapshot of system state including memory, registers, and subsystem states
3. **Transition**: Moving from one state to another by executing a syscall
4. **Exploration**: Systematic state space search (BFS/DFS/random)

### Execution Flow

```
main.cpp
  └── init_monitor()              - Initialize tracees
      └── state_initialization()
          └── exec_once()         - Execute single step
              └── ExecutionEngine::execute_step()
                  ├── ThreadManager::next_runnable_thread()
                  ├── ptrace(PTRACE_SYSCALL)
                  ├── SignalHandler::handle()
                  └── SyscallDispatcher::enter/exit()
```

### Module Responsibilities

#### core/engine/ - Execution Control

**ExecutionEngine** (`exec_engine.h/cpp`):
- Coordinates thread scheduling, signal handling, and syscall dispatch
- Main entry point: `execute_step()`
- Manages thread lifecycle events (create/exit)

**ThreadManager** (`thread_manager.h/cpp`):
- Tracks thread states (RUNNING, BLOCKED, EXITED, ZOMBIE)
- Round-robin scheduling of runnable threads
- Maps virtual TIDs (stable) to physical TIDs (may change on restore)

**SignalHandler** (`signal_handler.h/cpp`):
- Classifies signals (FATAL, NON_FATAL, GO_RUNTIME, SYSCALL_TRAP)
- Determines appropriate response (CONTINUE, CRASH, SPECIAL)
- Handles Go runtime SIGURG specially

#### core/syscall/ - Syscall Handling

**Dispatcher** (`dispatcher.h/cpp`):
- Routes syscalls to appropriate handlers
- Maintains per-syscall handler mappings
- Provides default pass-through behavior

**Handlers** (`handlers.h/cpp`):
- Category-based handlers (network, filesystem, time, thread, futex)
- Emulation vs pass-through decisions
- Checkpoint decision (CKPT_YES/NO/EXIT/DISCARD)

#### core/state/ - State Management

**sys_state** (`state.h`):
- Global system state with NP tracees
- Hash-based state identification
- Serialization support

**tracee_state** (`state.h`):
- Per-tracee state including:
  - syscall_info (current syscall)
  - Memory mappings and content
  - Register contexts for all threads
  - Subsystem states (fs, net, futex)

**StateStore** (`state_store.h/cpp`):
- Persistent storage of states
- Deduplication via hashing
- Memory-mapped I/O for efficiency

#### subsys/ - Subsystem Emulation

**FutexState** (`sync/futexstate.h/cpp`):
- Emulates futex operations for deterministic scheduling
- Wait queues per futex address
- Wake operations trigger thread state changes

**SockState** (`net/sockstate.h/cpp`):
- Socket state tracking
- Message buffering for deterministic delivery
- Network emulation integration

**FdManager** (`fs/fd_manager.h/cpp`):
- File descriptor allocation tracking
- Per-process fd tables
- Serialization support

## Key Data Structures

### Thread State

```cpp
struct thread_state {
    pid_t tid;                    // Physical thread ID
    struct user_regs_struct regs; // Register context
    uint64_t clone_flags;         // Thread creation flags
    bool is_main;                 // Is main thread
};
```

### Syscall Info

```cpp
struct syscall_info {
    int nr;                       // Syscall number
    uint64_t args[6];            // Arguments
    uint64_t rval;               // Return value
    bool is_enter;               // Entry vs exit
    // ... extended fields
};
```

### PTMC_STATE (Global Runtime State)

```cpp
struct PTMC_STATE {
    int mode;                     // MODE_BFS/MODE_DFS/MODE_RAND
    int state;                    // PTMC_PRELOAD/LOADED/STOP/RUNNING
    int cursor;                   // Current tracee index
    int current_thread_idx[NP];   // Per-tracee thread selection
    sys_state running_state;      // Current system state
    // ... per-tracee subsystems
};
```

### Checkpoint Decisions

```cpp
#define CKPT_YES     1    // Save state, explore children
#define CKPT_NO      0    // Continue without checkpoint
#define CKPT_EXIT    2    // Process exited
#define CKPT_STOP    3    // Crash or signal - stop exploration
#define CKPT_DISCARD 4    // Invalid path (e.g., blocked thread)
```

## Build System

```bash
# Build release version
NP=3 make release

# Build debug version
NP=3 make debug

# Clean build
make clean

# Run benchmarks
NP=3 make benchmark
```

**NP**: Number of processes (compile-time constant for array sizing)

## Development Guidelines

### Adding a New Syscall Handler

1. Determine handler category in `core/syscall/handlers.cpp`
2. Add handler function with signature:
   ```cpp
   int handle_<name>(pid_t pid, syscall_info& info, uint64_t* args);
   ```
3. Register in dispatcher initialization
4. Return checkpoint decision (CKPT_YES/NO/EXIT/DISCARD)

### Adding Thread Support to a Subsystem

1. Store per-thread state in `tracee_state`
2. Implement `serialize()` for checkpoint/restore
3. Update on thread creation (clone) and exit
4. Use virtual TID (index) for stable identification

### State Save/Restore

**Save** (`tracee_state::save()`):
1. Read `/proc/pid/maps` for memory regions
2. Save writable memory pages
3. Capture all thread registers via ptrace
4. Serialize subsystem states

**Restore** (`tracee_state::recover_running_state()`):
1. Restore memory mappings (mmap/munmap)
2. Restore memory content
3. Recreate threads if needed (clone)
4. Restore registers for all threads
5. Restore subsystem states

## CLI Commands

| Command | Description |
|---------|-------------|
| `c` | Continue (auto exploration) |
| `si [N]` | Single step, optionally with choice |
| `sw N` | Switch to process N |
| `thread [N]` | List threads or switch to thread N |
| `p EXPR` | Evaluate expression |
| `x/[n][f][s] ADDR` | Examine memory (GDB-style) |
| `bt` | Backtrace |
| `frame N` | Select stack frame |
| `locals` | Show local variables |
| `load HASH` | Load state by hash prefix |
| `diff A B` | Compare two states |
| `info [sock]` | Show process or network state |
| `ls` | List files in VFS |
| `bfs` | Breadth-first search |
| `dfs N` | Depth-first search to depth N |
| `rand N` | Random exploration to depth N |
| `batch FILE` | Execute commands from file |
| `q` | Quit |

## Expression Evaluation

Supports C-like expressions:

```
p g_value              # Global variable
p g_arr[2]             # Array access
p *g_ptr               # Pointer dereference
p g_struct.field       # Struct access
p ((Type*)ptr)->field  # Cast and access
p tracee1(var)         # Access other process's variable
```

## Debugging Tips

1. **Enable debug logging**: Set `loglevel = LOG_LEVEL_DEBUG` in config
2. **Trace syscall flow**: Use `si` command to single-step
3. **Check thread states**: Use `thread` command to list threads
4. **Memory inspection**: Use `x` command (GDB-style)
5. **Stack traces**: Use `bt` command for backtrace

## Common Issues

1. **Thread TID mismatch**: Physical TIDs change on restore; use virtual TIDs
2. **VDSO non-determinism**: Removed via `remove_vdso()` at startup
3. **AT_RANDOM entropy**: Patched to deterministic values
4. **Signal storms**: SIGURG from Go runtime filtered

## Configuration

JSON configuration format:

```json
{
  "Loglevel": 0,
  "Nodes": 2,
  "Tracee": [["./raft_node", "0"], ["./raft_node", "1"]],
  "Addr": ["192.168.0.1", "192.168.0.2"],
  "Assertions": ["tracee0(leader == 1)"],
  "VFS": ["/host/path:/guest/path"]
}
```

## Known Limitations

1. **Max 64 threads per process** (compile-time constant)
2. **x86_64 only** (relies on x86 ptrace features)
3. **DWARF-4 required** for debug info
4. **ASLR must be disabled**: `echo 0 > /proc/sys/kernel/randomize_va_space`

## Refactoring History

### Phase 1: Core Engine Abstractions

Created `core/engine/` with:
- **ThreadManager**: Centralized thread lifecycle management
- **SignalHandler**: Unified signal classification and handling
- **ExecutionEngine**: Coordinated execution integrating all components

### Phase 2: Syscall Abstractions

Created `core/syscall/` with:
- **Dispatcher**: Clean syscall routing
- **Handlers**: Category-based handler organization

### Phase 3: Directory Reorganization

Reorganized subsystems into `subsys/`:
- `fs/`: Filesystem-related code
- `net/`: Network emulation code
- `sync/`: Synchronization (futex) code

Moved state management to `core/state/`:
- All state-related files centralized

Organized UI code into `ui/`:
- `cli/`: Command implementations
- TUI and logging wrappers

### Phase 4: Scheduler Module and Utils Cleanup

Populated `core/scheduler/` with:
- **scheduler.h**: Clean scheduler interface with callbacks
- **exploration.cpp**: Core BFS/DFS/RAND implementations separated from UI

Utils cleanup:
- Moved `serialize.cpp` to `core/state/`
- Moved `resolve.cpp` to `core/`
- Removed unused `crc32.cpp`
- Integrated `commands.cpp` into the build
