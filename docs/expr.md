# 表达式求值

想要实现类似 gdb 的表达式求值功能。目前没实现结构体成员访问、指针成员访问这些功能。

gdb 的表达式求值还有一个很强的功能：解析函数符号并执行函数。

```c
// foo.c

#include <stdio.h>
int a;
int foo() {
  printf("hahaha\n");
  a = 1;
  return 0;
}
int main() {}
```

对于这样的一段 C 程序，使用 `-g` 编译的话，用 gdb 调试将会产生如下的效果：

```plain
(gdb) start
(gdb) p main
$0 = {int ()} 0x4017f4 <main>
(gdb) p main()
$1 = 0
(gdb) p foo()
hahaha
$2 = 0
```

进一步地，如果程序是动态链接的话，还可以直接调用 printf：

```plain
(gdb) printf("%x\n", 114514)
1bf52
$3 = 6
```

注意：如果不输出换行的话，在控制台模式下输出有可能会被放在 `stdout` 的 buffer 中，从而一时看不到。

如果是静态链接的话，foo.c 就不会包含 `printf` 符号，从而导致：

```plain
(gdb) p printf("%x\n", 114514)
No symbol "printf" in current context.
```

因为调用的 `printf("hahaha\n")` 会被优化为 `puts("hahaha")`。关于这个优化，请参见 [clu2's notes: How GCC generates optimized code for printf (and GCC built-in functions)](https://publicclu2.blogspot.com/2013/05/how-gcc-generates-optimized-code-for.html)

> 使用 `-fno-builtin` 可以取消此类优化。

```asm
0000000000401126 <foo>:
  401126:       55                      push   %rbp
  401127:       48 89 e5                mov    %rsp,%rbp
  40112a:       bf 04 20 40 00          mov    $0x402004,%edi
  40112f:       b8 00 00 00 00          mov    $0x0,%eax
  401134:       e8 f7 fe ff ff          call   401030 <printf@plt>
  401139:       c7 05 d9 2e 00 00 01    movl   $0x1,0x2ed9(%rip)        # 40401c <a>
  401140:       00 00 00
  401143:       b8 00 00 00 00          mov    $0x0,%eax
  401148:       5d                      pop    %rbp
  401149:       c3                      ret
```

still，这样的调用是可以设置断点的。只不过这样的话，求值会在断点处失败。

```plain
(gdb) b write
Breakpoint 3 at 0x4192d0
(gdb) p foo()
	
Breakpoint 3, 0x00000000004192d0 in write ()
The program being debugged stopped while in a function called from GDB.
Evaluation of the expression containing the function
(foo) will be abandoned.
When the function is done executing, GDB will silently stop.
(gdb) bt
#0  0x00000000004192d0 in write ()
#1  0x0000000000407205 in _IO_new_file_write ()
#2  0x0000000000405491 in _IO_new_do_write ()
#3  0x0000000000406783 in _IO_new_file_overflow ()
#4  0x0000000000404e4a in puts ()
#5  0x00000000004017e3 in foo () at foo.c:6
#6  <function called from gdb>
#7  main () at foo.c:12
```

`p` 还有一个重要的方面就是能够保留表达式计算产生的所有副作用。从上面 `printf` 将输出写入到 `stdout` 的 buffer 这点即可略窥一二。事实上，`p` 可以对所有的合法表达式进行求值，比如变量赋值表达式 `a = 1`。作为结果，会输出 1，并将这个值直接赋给进程中的那个变量。



**机制：猜测**

实现一般的表达式求值，总的来说可能有两类实现方式：

1、对于纯表达式的无函数调用求值，可以在 gdb 中解析表达式，做求值。通过解析二进制调试信息获得变量地址、字段偏移量、变量类型。通过 ptrace/procfs 获取进程实时信息，读写内存/寄存器实际值。

2、对于所有类型的表达式，都可以分析表达式类型，编译 wrapper 求值函数，注入到 tracee 中，并控制 tracee 执行注入代码求值。

主观认为 2 似乎是更方便的（写起来一点也不比 1 方便）。



**机制：分析**

有 `strace` 这样成熟的工具。

首先运行 gdb ./foo，start，然后输入好函数求值的命令 `p main()`。这样在输入回车之前，gdb 不会产生任何其他的系统调用而是卡在 read 上。

接着：

```sh
sudo strace -p $(pidof gdb)
```

`sudo` 似乎是必要的。然后在 gdb 那里点回车。这让我们能够观察到 `gdb` 运行这一条命令的时候进行的全部系统调用，一共有 200+ 条。

当一个进程卡在 gdb 给的 TRAP 之后，如果想要对函数求值，那就只能通过执行 tracee 自身的那个函数了。因而 gdb 一定需要某种办法让进程恢复执行，那么最明显的就是 `ptrace(PTRACE_CONT)`。在输出的系统调用中，果不其然找到了它的影子：

```plain
ptrace(PTRACE_CONT, 3926087, 0x1, 0)    = 0
```

合理推测，它的前面一定是将 tracee 的状态设置到适合进行函数调用的状态的过程；而后面则是收集函数调用结果，以及将状态恢复到调用前的过程。由于它会保留调用所有的副作用，因而这个状态的恢复，仅仅是恢复寄存器上下文。

去除掉一些无关的调用之后，来看核心部分：

```plain
ptrace(PTRACE_GETSIGINFO, 3926087, NULL, {si_signo=SIGTRAP, si_code=SI_KERNEL, si_addr=NULL}) = 0
pwrite64(14, "\314", 1, 140737488348431) = 1
pwrite64(14, "\17\345\377\377\377\177\0\0", 8, 140737488348408) = 8
ptrace(PTRACE_GETREGS, 3926087, {r15=0x1, r14=0x4a5f68, r13=0x7fffffffe6b8, r12=0x7fffffffe6a8, rbp=0x7fffffffe590,   rbx=0x1, r11=0, r10=0x1, r9=0x110, r8=0x4aa820, rax=0, rcx=0x4b37e0, rdx=0x7fffffffe6b8, rsi=0x7fffffffe6a8, rdi=0x1, orig_rax=0xffffffffffffffff, rip=0x4017fd, cs=0x33, eflags=0x246, rsp=0x7fffffffe590, ss=0x2b, fs_base=0x4b2380,      gs_base=0, ds=0, es=0, fs=0, gs=0}) = 0
...
ptrace(PTRACE_SETREGS, 3926087, {r15=0x1, r14=0x4a5f68, r13=0x7fffffffe6b8, r12=0x7fffffffe6a8, rbp=0x7fffffffe4f8,   rbx=0x1, r11=0, r10=0x1, r9=0x110, r8=0x4aa820, rax=0, rcx=0x4b37e0, rdx=0x7fffffffe6b8, rsi=0x7fffffffe6a8, rdi=0x1, orig_rax=0xffffffffffffffff, rip=0x4017f4, cs=0x33, eflags=0x246, rsp=0x7fffffffe4f8, ss=0x2b, fs_base=0x4b2380,      gs_base=0, ds=0, es=0, fs=0, gs=0}) = 0
pread64(14, "\363", 1, 4302224)         = 1
pwrite64(14, "\314", 1, 4302224)        = 1
pread64(14, "\220", 1, 4594263)         = 1
pwrite64(14, "\314", 1, 4594263)        = 1
pread64(14, "\314", 1, 140737488348431) = 1
pwrite64(14, "\314", 1, 140737488348431) = 1
```

通过查看 `/proc/$(pidof gdb)/fd/14` 的符号链接，可以看到这个文件指向的是 `/proc/$(pidof foo)/task/$(tidof foo)/mem`。因而，`pwrite/pread` 就都是直接读写 tracee 的内存了。推测到，由于要恢复一部分状态，因而 `pread` 是对当前状态的保存，而 `pwrite` 是改写。未来在执行完成后，这些读取的状态一定还是会被写回的。

首先第一个 `ptrace` 就 get 到 tracee 进入了 SIGTRAP。在此基础上，进行了两个写入：

```
pwrite64(14, "\314", 1, 140737488348431) = 1
pwrite64(14, "\17\345\377\377\377\177\0\0", 8, 140737488348408) = 8
```

`\314` 其实就是我们熟悉的 `int 0x3 (0xcc)`。`140737488348431 = 0x7fffffffe50f​`，可以看出这是写在了栈上。

第二个则是向 `0x7fffffffe4f8` ，也是栈上的位置写入了一个地址 `0x7fffffffe50f`。

接下来通过一系列的 `ptrace(PTRACE_GETREGS/PTRACE_SETREGS)`，可以看到最终的寄存器状态是这样的：

```
...
rbp=0x7fffffffe4f8,   
...
rax=0, 
...
rip=0x4017f4,
...
rsp=0x7fffffffe4f8,
...
```

此时，`RIP` 刚好落在 `main` 的入口，而 `RSP` 和 `RBP` 都落在了刚才写入地址的位置上。可以想见，如果接下来开始执行的话，等到函数退出时，`RAX` 将会代表函数返回值，并且函数返回地址正好在 `RBP` 的位置写着，跳转之后，等待程序的会是一个 0xcc。

只不过，这个 code 真的被执行了吗？并没有。众所都周知，栈区一般都是 `prot = PROT_READ | PROT_WRITE`，而通常不会是可执行的。事实上，我是在看了 `PTRACE_CONT` 之后的输出才明白了这一点的。

```plain
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_TRAPPED, si_pid=3926087, si_uid=1000, si_status=SIGSEGV, si_utime=0,       si_stime=0} ---
...
wait4(-1, [{WIFSTOPPED(s) && WSTOPSIG(s) == SIGSEGV}], WNOHANG|__WALL, NULL) = 3926087
```

这等于说，如果栈区可执行，那么迎接进程的是 0xcc (SIGTRAP)；如果不可执行，就会直接产生一个 SIGSEGV。

不过这样的话，进程不会直接崩溃吗？在 ptrace 下，是 gdb 先收到 SIGCHLD，标志着子进程状态变化。gdb wait 获得了子进程的 STOPSIG 后，才决定是否发送给进程。进程本身没有收到这个信号，因而不会执行 SIGSEGV 的 SIG_DFL 默认信号处理函数。

实际上，也许我们也能强行通过注册信号处理函数，给触发了 SIGSEGV 的进程续命。不过这就需要使用 `rt_sigprocmask` 或是 `siglongjmp` 这类办法了。

往后就是恢复进程上下文了。



当然，对于纯表达式的求值，gdb 的确采用了第一种办法，即当场解析表达式并且通过读进程内存来进行计算。

