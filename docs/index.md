# DetSim 设计

## 概述

DetSim 是一个主要面向 x86-64\_Linux 上运行的原生可执行文件的代码级 Model Checker。主要适用于传统编译型语言（C、C++、Rust、Go）编译的程序。

### 特性

- 支持单机模拟分布式系统/并发程序运行
- Stateful：保存程序执行状态，与 Stateless 保存程序执行路径对应
- 执行调度交错粒度可调节：（跨）系统调用级
- 支持自动探索和手动调试模式

### 原理

想法起源于对分布式系统的代码级 Model Checking。分布式系统执行的不确定性主要来源于本地系统调用的不确定结果，以及网络系统调用（消息传递）发生的先后顺序。因而想到，只要将实际产生影响的关键系统调用进行模拟，使其产生各种各样的可能结果，并且控制好各节点的执行调度，即可模拟整个系统的所有可能执行序列。

通过这种方法，在系统发生状态违例的时候，我们就可以清晰地看到每一步执行了什么，以及所有的中间状态。这对于我们找到分布式系统中的 Bug 是有利的。

进一步地，如果将执行操控粒度改为指令级别，并实现对多线程程序的操控，就可以获得一个对并发程序进行模型检测的工具。不过这种方法有其局限性，因为单步执行并发程序隐含了 Strong Memory Model 的假设。

> 每个机器执行指令，默认地包含 acquire、release 语义
> 这意味着，当某个 cpu A 执行一连串的写动作时，对于其他CPU，它们看到的写入顺序应该与A执行的一致。

### 机制

我们将作跟踪的进程称为 Tracer，被跟踪的进程称为 Tracee。由 Tracer 启动 Tracee 并介入跟踪，将每个 Tracee 执行到一个初始状态后停下并对其状态进行保存（称为快照）并将快照入队。之后进入主循环，每次从队列中取出一个快照。对于每一个 Tracee，都恢复这个快照到进程中，并执行一步，再保存快照入队。整个探索过程相当于广度优先搜索，也可以使用其他的探索模式。

每探索到一个全局状态，都进行一次状态检查，以判断整个系统的状态是否违反了规约，并可以打印出到目前为止的执行路径。

### 目标

- 实现易用的调试模式
  - 变量分析，类型推导，表达式运算，更方便的操控方式，友好的信息展示
- 提高自动化程度，尤其是 Choose() 函数的生成
  - 手写是比较合理的。一些地方对于 choose() 的结果需要根据状态本身来判断
- 提高探索效率；方便地修改探索策略
  - 允许添加自定义的状态结构体，方便进行状态之间的比较
- 利用好 Stateful 可能带来的便利 ~看起来似乎没什么便利~
  - 恢复长 trace 较为迅速

## 关键技术

DetSim 所依赖的主要技术细节，有一些开发过程中需要特别注意的内容。

### 系统调用概述

这里的系统调用有两重含义，一重是位于 Linux Kernel 的 Native Syscall；另一重是 Libc Wrapper。

#### Native Syscall

Linux 发生系统调用是通过 `syscall`  指令（`int 0x80`），根据 `RAX` 寄存器存放的系统调用号以及 `RDI`, `RSI`, `RDX`,`R8`, `R10`, `R9` 六个寄存器存放的参数进行调用，最后将调用结果返回到 `RAX` 寄存器中。

所以，以 `write` 为例，一个系统调用最原本的样子是：

```assembly
.data
s: 
  .ascii "Hello World!\n"
  
.text
// write(1, s, 13)
mov $1, %rax
mov $1, %rdi
mov $s, %rsi
mov $13, %rdx
syscall
```

如果执行成功，返回值（`RAX`）是 13。如果失败的话，`RAX` 中将存放对应的错误号**的相反数**。**错误号均为正数，错误时的返回值均为负数**。

```plain
EPERM 1 Operation not permitted
ENOENT 2 No such file or directory
...
```



#### Libc Wrapper

使用 C 语言进行系统调用时，实际上调用的是 libc 的 wrapper 函数，而不是直接进行系统调用。包括 `read`, `write` 等等，甚至包括：

```c
long syscall(long number, ...);
```

有一些系统调用没有 C wrapper，于是可以使用这个通用的 wrapper 进行间接调用。

Libc wrapper 与 Native syscall 略有不同，主要在于部分函数参数语义变化以及返回值的处理。

##### 函数参数变化：一个典型例子

所举的例子就是 `ptrace` 本身。先来看看 `ptrace` 在 Libc Wrapper 中的函数原型：

```c
long ptrace(enum __ptrace_request op, pid_t pid, void *addr, void *data);
```

对于大多数 `ptrace` 的调用而言，这些参数都是传入的参数，而有一个例外就是 `PTRACE_PEEKDATA/PTRACE_PEEKTEXT`。它的功能是读取目标进程 `addr` 位置的一个*机器字*（64位机的8字节），作为函数的返回值。

这就出现了一个矛盾：`addr` 位置的值有可能是 -1；而 `ptrace` Wrapper 规定在系统调用失败时返回 -1。

对于 Native 的 `ptrace` 调用而言，对于地址内容的返回是通过 `data` 参数进行的，错误码的返回是通过返回值进行的。Wrapper 则希望用户忽略 `data` 参数，因为这个参数会被 Wrapper 使用。

```c
long ptrace(op, pid, addr, data) {
  switch (op) {
    case PTRACE_PEEKDATA: case PTRACE_PEEKTEXT:
      long real_data;
      rval = ptrace_native(op, pid, addr, &real_data);
      if (rval < 0) {
        errno = -rval;
        return -1;
      } else {
        return real_data;
      }
      break;
    case ...
  }
}
```



##### errno：错误情况下的返回值

Libc wrapper 除了将系统调用号和参数放进寄存器，并执行调用以外，还会在调用返回之后设置 `errno(3)`。

```plain
LIBRARY
    Standard C library
SYNOPSIS
	  #include <errno.h>
```

`errno` 是一个 Thread Local 的变量，用于标志系统调用发生的错误类型。对于成功的系统调用，`errno` 不会变化（不会清零）。因此如果需要使用 `errno` 判断是否发生错误，需要提前手动清零；或者就通过系统调用的返回值判断是否发生错误，再去看 `errno`。

Native Syscall 的错误号会反映在 `RAX` 寄存器中；而 Libc Wrapper 系统调用会进行进一步处理，将错误号放在 `errno` 中。

例如，一个 Native `write()` 系统调用实际返回了 `ENOENT 2`，也就是 -2，根据手册：

> On  success, the number of bytes written is returned.  On error, -1 is returned, and errno is set to indicate the error.

Libc Wrapper 的返回值是 -1，而 `errno` 的值将是 2。



### 进程与线程概述

整个系统的出发点是对于分布式系统的执行流进行操控，而这里面就要厘清进程和线程对于我们来说有什么不同。

第一重要的是，要认识到：**进程是一个资源单位，线程是一个调度单位**。一个进程首先必然有一个初始的线程，也可以有很多其他的线程，这些线程之间都是**平等的**。从操作系统的视角来说，同一个进程中的线程没有高低和主次之分。

#### 进程

我们要关心的进程，无非就是它有哪些执行流（线程），使用了哪些资源，即包括 socket 的文件系统对象，内存空间，等等。

进程的退出包含了对全部线程的退出，**使用的是 `SYS_exit_group` 系统调用，而不是 `SYS_exit`**。

#### Native Threads

这里特指调用 Linux 内核 `clone/clone3` 的 `CLONE_THREAD` 选项创建的执行流。

```c
pid_t child_tid = clone(
/* fn    = */ thread_entry,
/* stack = */ thread_stk + STK_SIZE,
/* flag  = */ CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS
            | CLONE_FILES | CLONE_PTRACE | CLONE_PARENT,
/* arg   = */ NULL,
/* ptid  = */ &parent_tid
  );
```

作为这样的原生线程，在创建完成之时，将会跳转到执行线程入口地址。如果运行完了它的全部逻辑，就会直接退出并消失（叫做 detach）并且对其他的资源并没有任何影响，包括线程使用的栈空间，因为栈空间实际上是调用者来管理的。

任意一个线程，甚至是主线程，调用

```c
syscall(SYS_exit, 0); // 没有直接的 libc wrapper！
```

都可以使得当前执行流停止，并且其他的线程继续运行。对于一个主线程和一个子线程的两线程程序，其树形结构应当是：

```plain
-process-{thread}
```

如果子线程退出，则会剩下一个进程及其线程：

```plain
-process
```

如果主线程退出，则留下一个僵尸进程，并下属一个子线程：

```plain
-process-{thread}
```

<img src="assets/image-20241113202635648.png" alt="defuncted process" style="zoom:50%;" />

**线程正常退出不会向任何地方发送 `SIGCHLD`，也不能够被任何 `wait` 系统调用等待到。**而线程 crash 会导致整个进程 crash。如果需要线程的返回值，只够通过其他线程与该线程之间的某种同步机制。

#### Posix Threads (pthread)

```c
int pthread_create(pthread_t *restrict thread,
                   const pthrad_attr_t *restrict attr,
                   void *(*start_routine)(void *),
                   void *arg);
int pthread_join(pthread_t thread, void **retval);
```

pthread 是携带了同步机制的。在执行入口函数前和退出后进行了额外的操作以收集线程退出时的信息。具体而言，是通过 `SYS_futex`。



### ptrace 系统调用

`ptrace()` 系统调用是  Linux 系统中对其他进程进行观察以及执行操控的系统调用。使用需要引用头文件：

```c
#include <sys/ptrace.h>

long ptrace(enum __ptrace_request op, pid_t pid,
                   void *addr, void *data);
```

在接下来的部分里，我们将作控制的进程成为 Tracer，被控制的进程称为 Tracee。

#### 基本原理

当 Tracer 使用 `PTRACE_SYSCALL` 时，Tracee 将从一个停止的状态开始运行，并停止在下一个系统调用的入口或出口，再次停下来，进入 `Syscall-stop state`。停止状态可以被 `waitpid` 观察到，status 满足 `WIFSTOPPED(status) == true && WSTOPSIG(status) == SIGTRAP​`。

#### 基础使用方法

以一个主进程和一个子进程的情况来说明，子进程为 `/bin/ls`。在子进程进行 `execve` 调用前，有一些准备工作。

1. `fork()` 一个新进程，并在子进程中执行 `ptrace(PTRACE_TRACEME, 0, 0, 0)`。这允许子进程被主进程跟踪。
2. 子进程调用 `raise(SIGSTOP)` 立即通过信号下来，等待主进程的控制。主进程可以使用 `waitpid(pid, NULL, 0)` 等待子进程状态变化。
3. 主进程调用 `ptrace(PTRACE_SEIZE, pid)`  介入控制，并通过 `PTRACE_SETOPTIONS` 设置控制参数。

之后进入一个主循环，主循环的过程大致如下：

1. 主进程调用 `ptrace(PTRACE_SYSCALL, pid)` 让子进程执行到下一个系统调用的入口/出口处。
2. 主进程调用 `ptrace(PTRACE_GET_SYSCALL_INFO)` 获取系统调用信息：在入口/出口，调用号，参数等等。
3. 主进程处理调用信息。

总体如下：

```c
int main() {
  int pid = fork();
  if (pid == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    execl("/bin/ls", "ls", NULL);
    perror("execl");
  } else { // tracer
    waitpid(pid, NULL, 0);
    /* ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD); */

    while (1) {
      ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
      waitpid(pid, NULL, 0);
      struct ptrace_syscall_info info;
      int result = ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), &info);
      if (result == -1)
        printf("ptrace error: %s\n", strerror(errno));

      switch (info.op) {
      case PTRACE_SYSCALL_INFO_ENTRY:
        printf("syscall entry: ID = %lld\n", info.entry.nr); break;
      case PTRACE_SYSCALL_INFO_EXIT:
        printf("syscall exit : ID = %lld\n", info.entry.nr); break;
      case PTRACE_SYSCALL_INFO_NONE: break;
      case PTRACE_SYSCALL_INFO_SECCOMP: break;
      default: printf("Unknown\n"); break;
      }
    }
  }
  return 0;
}
```

Line 11 的 `PTRACE_SETOPTIONS` 是必要的。如果没有设置 `PTRACE_O_TRACESYSGOOD` 的话，所有的 `info.op` 都会被设置为 `NONE`。其目的主要是通过将 `SIGTRAP` 改为 `SIGTRAP | 0x80` 将 `Syscall-stops` 与其他类型的 `ptrace-stops` 区分开来，包括 `signal-delivery-stop`，因为进程本身可能由于其他原因收到 `SIGTRAP`，不能混为一谈。

如果进程受控制了，那么它每一次成功执行 `execve` 都会收到一个正常的 `SIGTRAP`。

<img src="assets/image-20241113181716326.png" alt="SIGTRAP(5) and SIGTRAP | 0x80 (133)" style="zoom:50%;" />

#### 系统调用篡改

除了观测程序的执行和控制调度，我们还需要对于产生不确定性的系统调用，甚至是对不确定性的环境进行模拟；此外，部分系统资源需要由控制框架进行管理。因此我们还需要改变一些系统调用的行为，使得它们并不真实地在操作系统中申请资源。

为了生成不确定的结果，主要是需要修改系统调用的结果（返回值和输出参数）；为了使得系统调用不产生实际影响，则要直接禁止系统调用执行。

跳过系统调用的执行可以通过 `PTRACE_SYSEMU` 命令实现。但实际过程当中直到运行到下一个系统调用的入口才能知道下一个系统调用号，而此时再使用这条命令就无法跳过执行了。因而这里采用了一个更加直接的方法：修改系统调用号为一个不存在的调用。

```c
ptrace(PTRACE_GETREGS, pid, NULL, &regs);
regs.orig_rax = SYS_which;  // 新的系统调用号
ptrace(PTRACE_SETREGS, pid, NULL, &regs);
```

`ptrace` 提供了读取/设置寄存器值的命令。修改其中的 `orig_rax` 即可。

在此之上，我们不但可以改变系统调用，而且还可以凭空注入一些系统调用。其做法就是在一个系统调用结束之后，把 `RIP` 寄存器往回调 2 字节的位置，也就是回到 `int 0x80` 指令之前。这样就又得到了一次做系统调用的机会。

```c
regs.rip -= 2;
```

这对于我们作 Checkpoint 时管理系统资源有重要作用。

### ProcFS

`ptrace` 系统调用的 `PTRACE_PEEKDATA` 命令可以从 Tracee 中读到一个机器字的信息，理论上只需要这样就可以获取 Tracee 的全部内存空间中的状态。但我们每执行一步就要保存下所有 Tracee，也就是多个进程的全部内存状态，只用 `ptrace` 的效率较低；此外，还有文件描述符的状态需要获取。使用 `procfs` 是相对容易的。

#### /proc/$pid/maps

这里存放了进程做占用的全部虚拟地址空间项目以及类型。我们将根据这里给出的地址等信息做内存的快照。

![maps](assets/image-20250321153834937.png)

#### /proc/$pid/memory

procfs 实质上是一个虚拟文件系统，里面的文件并不是传统意义上的“文件”，而是对文件系统调用产生特定行为的一类对象。这个文件就代表了 `pid` 所指示的进程的全部地址空间的内容，并且根据内存映射方式（只读、读写），进程本身、进程所有者也可以对其进行相应的读写。当我们读写文件的时候，实际上就是在读写进程内存。

```c
int fd = open("/proc/pid/memory", O_RDWR);
lseek(fd, addr, SEEK_SET); /* 将文件偏移量设置到 addr 处 */
read(fd, buf, len); /* 读取 len 长度的内存内容 */
```

#### /proc/$pid/fdinfo

目录中存放了文件描述符的信息。

```plain
pos: 0                  // 偏移量
flags: 02100002         // 文件 flag
mnt_id: 27              // 挂载点 id
ino: 3                  // 文件系统 inode 号
```

#### /proc/$pid/fd

存放了打开文件的符号链接。这对于我们定位被打开的文件地址非常有用。

![zsh 的 /proc/$pid/fd](assets/image-20250321155418993.png)



### execinfo/libunwind/libelf

主要用于实现打印 Tracee 的调用栈。参见[栈回溯](backtrace.md)。

### Dwarf

主要用于分析 Tracee 中的符号信息。dwarf 是一种调试信息的格式，从中可以从变量名分析出其所在的内存位置。

### 其他



## 设计文档

### 概览

调试框架由 cpp 实现。

#### 参数

```plain
-a --auto         使用自动探索模式
-l --log=FILE     指定日志输出文件
-c --cfg=FILE     指定配置文件 
-b --batch=FILE   指定输入的命令脚本
```

#### 配置文件

使用 JSON 格式编写。

```json
{
  "Nodes": 2,    															// 节点数
  "Tracee": [
    ["./tracee", "0"], ["./tracee", "1"]      // tracee 程序和启动参数
  ],
  "Addr": [ "192.168.0.1", "192.168.0.2" ],   // tracee 所在节点的 IP 地址
  "SharedFiles": [ ],
  "UserCheck": "check_raft.cpp",
  "ChoosePoint": [
    { "syscall": 96, "choose": 2 }            // 在 SYS_gettimeofday 处 choose
  ],
  "ChooseFunc": "choose_raft.cpp"
}
```

#### 主函数流程

`init_monitor()` 解析 Tracer 的参数，设置跟踪的日志输出，读取配置文件。

`init_syscalls()` 初始化系统调用号到调用名的字符串数组。

`init_state()` 启动 Tracee，并将 tracee 运行至一个初始状态生成快照。

`init_dwarf()` 分析变量地址。

`ui_mainloop()` 进入调试器主循环，接受命令。

#### 命令列表

`help` 打印命令信息。

`c` 继续从当前状态以自动模式执行。

`q` 退出。

`si` 使当前焦点进程/线程执行一步。

`sw` 切换焦点进程/线程。

`load [HASH]` 根据所给的状态哈希加载快照。

`info` 打印当前状态的执行序列以及状态信息。

`x` 打印内存值。

`p` 计算表达式。支持简单的变量名解析。

`bt` 输出调用栈。



### 操控框架

Tracer 的全局状态记录为一个只有一个实例的结构体。

```c
struct ptmc_state {
  enum { PTMC_STOP, PTMC_RUNNING, PTMC_END, PTMC_ABORT, PTMC_QUIT } state;
  int cursor; // 指示焦点进程/线程
  sys_state *last;  // 上一次执行的最终状态
  sys_state *running;  // 这一次执行的状态
  hash_type ss;  // 全局状态哈希
  ...
} ptmc_state;
```

#### 基础的单步执行

整个框架的最核心功能就是单步执行。单步执行是从 `exec_once` 函数开始的，其目的是从一个已经加载的快照运行1个或多个系统调用，直到下一个被认为是关键的系统调用。

> 对分布式系统的测试中，并非所有系统调用都需要被交错执行。只有一部分关键的消息传递和时间相关的系统调用在影响最终结果。因而中间的系统调用是可以连续执行完毕的。

`exec_once` 的前置要求：执行焦点 `ptmc_state.cursor` 和全局状态 `ptmc_state.running` 已被设置。首先检查 `cursor` 所指示的线程是否已经退出。如果已经退出，则不执行。否则进入执行循环。执行循环基本上就是一次执行一个系统调用 `do_one_syscall`，并根据执行后的返回值来判断是否继续执行。

```c
int do_one_syscall(pid_t pid, syscall_info *si);
```

函数通过 `PTRACE_SYSCALL` 系统调用使 Tracee 运行两次，一次为 enter，另一次为 exit。在入口和出口处执行一些额外操作。最终收集系统调用信息放入 `si` 中，并返回是否作状态快照。

```plain
CKPT_NO      不进行快照，继续执行
CKPT_YES     进行快照
CKPT_DISCARD 丢弃状态
CKPT_EXIT    线程退出了，需要特判
```

入口和出口处的额外操作是通过一对函数进行的：

```c
static void on_syscall_enter(pid_t, int nr);
static int on_sycall_exit(pid_t pid, syscall_info *info);
```

enter 处根据系统调用号决定系统调用是否被执行。对于需要框架接管的系统调用，将会调用 `do_nosys` 调用一个不存在的系统调用使其失败，并在 exit 处模拟其结果；如果是 `SYS_exit` 或 `SYS_exit_group` 则需要对进程/线程状态做额外标记。

#### 系统调用模拟

对于需要模拟的系统调用都自定义了新函数。

由于需要在同一个进程上不断进行快照和恢复的过程，容易重复申请资源。因而这些资源应当全部由框架管理起来。因此框架也包含了存储 socket、网络buffer、文件描述符的数据结构。对系统调用的模拟就是通过与这些数据结构进行交互进行的。

以 `bind` 调用为例。

```c
int emu_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  Sock *sock = get_socket(sockfd);
  ...
  chat *tmp = (char *)malloc(addrlen);
  memcpy_guest2host(tmp, addr, addrlen);
  sock->addr = std::string(tmp, tmp + addrlen);
  free(tmp);
  return 0;
}
```

这里将会首先处理 bind 可能发生的部分错误类型。如果没有错误，则从 Tracee 的内存中拷贝 `addr` 到 Tracer 当中，并在 Tracer 管理的数据结构中完成 `bind` 应当达到的效果。

#### 引入不确定性

部分系统调用有着不确定的结果，如 `gettimeofday` 的结果是有赖于执行效率和调度的，可以产生不同的结果；或者是网络相关的系统调用受到网络环境的影响有可能不成功。为了进行完全的枚举，我们借鉴 CMC 的 `CMCChoose()` 的形式，也设置了这种分支选项的机制。

```c
struct {
  ...
  int choose;
  int n_choose;
  ...
} ptmc_state;
```

根据配置文件所指定的 `choose` 数组，我们可以得到特定的系统调用可能产生的结果类型数量。于是此时可以设置 `Choose` 变量以标志进入分支模式，并使用 `ChooseNO` 进行分支的计数。在系统调用的模拟函数中，可以根据分支的计数来产生不同类型的结果：

```c
int emu_gettimeofday(struct timeval *tv, struct timezone *tz)
{
  /* Ignore tz */
  int index = ptmc_state.cursor;
  struct timeval tracee_tv = ptmc_state.time[index];

  if (ptmc_state.n_choose == 0) /* 不进行choose，返回实时时间 */
  {
    memcpy_host2guest(tv, &tracee_tv, sizeof(struct timeval));
    return 0;
  }

  choose_in in(ptmc_state.time[index]);   /* 组织传入参数 */
  choose_func choose = choose_syswhat[SYS_gettimeofday];   /* 根据 syscall_nr 获取对应 choose 函数指针 */
  choose_out *out = choose(ptmc_state.pids[index], ptmc_state.choose, in); /* 调用对应的用户 choose 函数 */

  syscall_info info;
  info.args[0] = (uintptr_t)tv;
  info.args[1] = (uintptr_t)tz;
  apply_choose(info, out);

  ptmc_state.time[index] = *(struct timeval *)out->args[0];
  int rval = out->rval;
  return rval;
}
```

#### 自定义 choose 函数

为了对某个系统调用点进行 choose，事实上需要用户来根据选项来生成行为。

```c
typedef struct choose_in
{
  struct timeval now;
  ...
} choose_in;

typedef struct choose_out
{
  void *args[6];
  int len[6]; /* 0 represents for unmodified */
  int rval;
} choose_out;

typedef choose_out *(*choose_func)(int pid, int choice, const choose_in &in);

extern choose_func choose_syswhat[450];
```

框架的每个 `emu_syscall` 都是这样运作的：

- 判断是否在选择中。如果不在选择中，则执行默认行为
- 如果在选择中，则找到对应系统调用的 choose 函数并传入选项，并组织好 choose 函数做具体决定需要的信息 `choose_in`。选项为 0~N-1 之间的一个值
- choose 函数返回一个用于修改系统调用结果的数据结构体
- 调用 apply_choose() 将 choose() 产生的系统调用结果写回进程



#### 自动模式

实现于 `exec_cont()` 函数。

自动探索每次从状态队列中取一个状态并从它出发，进行多个执行，每一个执行都代表有可能的调度或不同的系统调用结果。具体而言：

1. 取得初始全局状态 `S`。
2. 对每一种执行的可能性：
   1. 加载 `S` 到所有 Tracee 上
   2. 根据选择的可能性，向前执行一步
   3. 将当前全局状态保存下来
3. 重复这个循环

### 快照

大致的构成。

#### 状态结构体

#### 快照生成

#### 快照存储

#### 快照恢复



### 多线程

#### 操控

#### 快照

#### 并发程序

#### 焦点线程



