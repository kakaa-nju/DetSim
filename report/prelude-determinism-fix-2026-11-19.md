# 内存快照跨进程不可复现(1 字节漂移)修复报告

日期:2026-11-19
涉及代码:`src/core/state/state_transition.cpp`(+174 行)、`src/core/guest.h`(+3 行)
状态:已实现、已验证,尚未提交 git(建议提交消息:`fix(prestate): virtualize prelude CPUID/RDTSC/fstat and scrub inherited stack for cross-process snapshot reproducibility`)

---

## 1. 问题现象

同一 tracee、同一配置,在两个**独立的 tracer 进程**中各跑一遍,捕获的内存快照(StateStore blob)字节不一致——跨进程不可复现。用户报告的可见症状是 **1 字节漂移**。

复现方法:两个运行必须环境逐字节一致(见 §6.9 的 `OLDPWD` 陷阱),`StateStore.packed_storage_path` 指向不同目录即可分离两份 store;然后解码 packed store(`memory/global.idx|global.inc` + `segments/*.dat`,PackedDataHeader 24 字节、索引进制 32 字节)逐 blob 对比。

### 实验环境

| 项 | 值 |
|---|---|
| CPU | AMD EPYC 7642(Zen 2,32 vCPU,同构) |
| 内核 | 6.8.0-147-generic |
| glibc | 2.39(Ubuntu 24.04) |
| ASLR | `/proc/sys/kernel/randomize_va_space = 0` |

### 修复前实测漂移

| tracee | 漂移 |
|---|---|
| 动态 hello | 19 字节 / 5 处 |
| 静态 hello | 5 字节 / 2 处(其中 1 字节正是用户所见) |

---

## 2. 根因分析

全部漂移都在 **loader prelude(execve 完成 → 配置的边界 syscall)真实执行的代码**写入,来源是三个宿主接口 + 一处进程间继承:

### 2.1 CPUID 的 APIC ID 字节("1 字节漂移"本体)

glibc 启动时 `init_cpu_features` 执行 `CPUID(1)` 并把 **原始 EBX** 缓存进 `_rtld_global_ro + 0x88`(ld.so 的 `.data.rel.ro`;静态二进制在静态 libc 的同一结构)。`CPUID(1):EBX` 的 bits 24-31 是 **initial APIC ID = 进程启动时被 OS 调度到的物理 CPU 编号**,每次 exec 都可能不同。

证据链:
- 快照中该字节跨运行取值 48/49/50/51,与主机 CPU 编号区间一致;
- `objdump -d ld-linux` 定位到写入指令:`18400: mov $0x1,%eax; 18405: cpuid; 18412: mov %ebx,0x1f710(%rip)  # 37b28 <_rtld_global_ro+0x88>`;
- 用 `objdump` + glibc 2.39 源码确认周边布局(`0x37a60 = __libc_enable_secure+0x18` 等);
- 绑定 CPU 0 后该字节跨运行恒定(实验确认)。

这就是"疑似来自执行时的操作系统注入"的确切含义:OS 调度决定了该字节的值,而 DetSim 的 ptrace 粒度模型看不到 CPUID。

### 2.2 RDTSC 启动计时残留(动态 tracee 另有 ~18 字节)

glibc 2.39 的 rtld 用 HP_TIMING 记录自身启动耗时(`elf/rtld.c` 的 `RLTD_TIMING_DECLARE(start_time/load_time/relocate_time)`):

- `start_time` = **裸 RDTSC 快照**,落在 ld.so RELRO(`__libc_enable_secure+0x18`,即 vaddr 0x37a60)——快照里呈 `0x000e0???_xxxxxxxx` 形态,数值随主机运行时间单调增长(实测 4 个连续运行逐一递增);
- `load_time`(RELRO 0x37a68)、`relocate_time`(.data/.bss)为 RDTSC 差值,ptrace 拖慢 prelude 后数值更大且逐次不同;
- `dl_main` 活栈帧里还留有 `rtld_timer_start(&start)` 的快照局部。

汇编证据:`20233: mov %rax,…# 37a60`、`20803: sub …# 37a60`、`22425: add %rax,…# 37a68` 等,与 glibc 源码的 `rtld_timer_start/stop/accum` 一一对应。

### 2.3 fstat(1) 的管道标识(静态浅栈 tracee 的 4+1 字节)

静态 glibc 在边界前 `fstat(1, statbuf)` 初始化 stdout。内核把**真实管道的 `st_ino`(每次 `pipe()` 都不同)** 和 **`st_atim/st_mtim/st_ctim`(当前墙钟时间)** 写进栈上 statbuf;帧复用后这些值残留在 write 边界的活帧里:
- 4 字节槽 = 管道 inode,跨运行完全随机;
- 相邻槽 = `st_mtime` 的 epoch 秒低字节——两次运行恰好相隔 1 秒,观测到 +1,曾伪装成"慢速计数器"。

### 2.4 fork+exec 遗留的 tracer 栈垃圾

fork 把 tracer 的栈映像交给子进程,exec 只重写栈顶(argv/env/auxv)。死栈区带着 tracer 的**每次运行都不同的数据**(带时间戳的日志缓冲、指针);prelude 栈较浅的 tracee(静态 hello)未将其覆盖,残留进快照——包括活帧里**未被初始化的槽位**。

---

## 3. 修复设计

原则:与既有机制同一风格——**prelude 内的宿主状态归一化**(getrandom 归零、getcpu 归零、AT_RANDOM 打补丁、rseq ENOSYS、robust list 仿真),全部 fail-closed,不硬编码任何 glibc 版本/二进制布局偏移。

### 3.1 逐项改动

| 位置 | 改动 | 作用 |
|---|---|---|
| `start_tracee()`(fork 子进程,exec 前) | `prctl(PR_SET_TSC, PR_TSC_SIGSEGV)` + `sched_setaffinity(0, {CPU0})` | 两个策略都跨 execve(已实验确认):prelude 内每次 CPUID(1) 都观察 CPU 0 的同一 APIC ID;每次 RDTSC 变为可拦截的 SIGSEGV。失败则 `_exit(125)` 带明确 stderr 消息(与 fixed_layout 同风格) |
| `emulate_startup_tsc_fault()`(新,`resume_startup_syscall` 分支) | SIGSEGV 停止时解码 rip 处 `0F 31`(rdtsc)/`0F 01 F9`(rdtscp),写建模值 RAX=RDX=0(rdtscp 另写 RCX=0),跳过指令 | 建模机器不暴露宿主时钟:prelude 内一切 TSC 读数恒为 0,与 getrandom 归零语义一致。非 TSC 指令的 SIGSEGV 不消费,继续走原有 fail-closed 路径 |
| `state_initialization()` fork 前 | `sched_getaffinity` 保存每个 tracee 继承的掩码;解析 tracer 自身 `[stack]` vma 底(一次,在父进程做,fork 子进程不再做文件 IO) | 供边界恢复与子进程栈清零使用 |
| `start_tracee()` 栈源头清零 | 子进程以真实 RSP(内联汇编读取)为锚,把 `[max(vma底, rsp-256KB), rsp-128)` 清零(保留红区) | exec 保留的旧栈不再携带 tracer 垃圾;exec 后残留只剩子进程到 exec 的固定代码路径字节(确定性)。注意必须用真实 RSP 而非局部变量地址——大帧函数中局部变量距帧底可达 0x1C0,否则 memset 会清掉自己的返回地址(调试中实际发生过,rip=0) |
| `extract_one_syscall()` fstat 归一化 | `nr==SYS_fstat && rval==0` 时把 statbuf 的 `st_ino`(+0x08)与 `st_atim/st_mtim/st_ctim`(+0x48..0x78)归零 | stdio 只读 type/blksize;管道 inode 与墙钟不再进入帧残留。偏移取自 x86-64 内核 UAPI `struct stat`(glibc 位兼容,ABI 稳定) |
| `restore_startup_host_policies()`(新,边界处调用) | 注入 `sched_setaffinity(0, 保存的掩码)`(掩码写在保留 scratch 页 +0x800,避开 +0xff0 的 syscall 桩)与 `prctl(PR_SET_TSC, PR_TSC_ENABLE)`,复用 tracee 自身边界 syscall 指令 in-place 注入 | 两个策略只覆盖 prelude:之后恢复原掩码(并行 worker 互不干扰、恢复真实 TSC),且两处返回值非 0 即抛异常 |
| `scrub_inherited_dead_stack()`(新,边界处调用) | tracer 侧把 tracee `[stack]` vma 底到 `rsp-128` 清零 | 纵深防御:即使子进程源头清零之后,也把 prelude 自身死帧从快照身份中抹掉,防未来任何宿主相关的 prelude 死帧写入进入身份 |
| `guest.h` | 补 `tracee_do_syscall_in_place` 声明(原先只在 guest.cpp 定义) | 供边界恢复注入使用 |

(注:第一版曾通过 `runtime_profile_identity()` 新增 `startup_tsc`/`startup_cpu_pin` 字段声明模型变化;按"无需考虑快照兼容性"的决定,该改动已移除,既有快照已全部清除,新快照自洽。) |

### 3.2 修复后的确定 prelude 归一化清单

getrandom(归零)、getcpu(归零)、AT_RANDOM(打补丁)、rseq(ENOSYS)、robust list(仿真)、rt_sigprocmask(仿真)、set_tid_address(逻辑化)、getpid/gettid(逻辑化)、**CPUID(经 pin 恒定)**、**RDTSC(仿真归零)**、**fstat(标识字段归零)**、**继承栈(清零)**。

---

## 4. 验证结果

全部在修复后的 `NP=1` tracer 上执行,两次运行环境逐字节一致、store 路径分离:

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 动态 hello(prelude 边界快照) | 19 字节/5 处 | **2/2 blob 哈希完全一致** |
| 静态 hello | 5 字节/2 处 | **2/2 一致** |
| hello + 5 步 `si`(含 brk 扩展堆,18 regions 状态) | — | **逐个保存的 hash 序列完全一致** |
| mt_async 多线程(pthread + nanosleep + pselect + pipe),20 步 `si` | 大量差异 | **14/14 快照哈希一致** |
| 跨进程恢复 | — | 第二个 tracer 进程 `load <hash>` 恢复第一个进程保存的状态,继续执行 3 步后到达的状态哈希与第一个进程的历史状态精确重合 |

回归:

- `tests/mt_async/check.sh`:**PASS ×2**(mt_async、mt_poll);
- `tests/select_poll_semantics/check.sh`:**14 项全 PASS、无 FAIL**;
- NP=1 / NP=2 / NP=3 三种构建编译通过;
- `tests/userstate/rewind.json`(NP=3 多节点)启动、多节点快照正常,无 `[CHECK FAILED]`(batch 中 `si` 停在 pending 决策属交互模式预期)。

---

## 5. 可能带来的副作用

按影响面从大到小列出,均为分析结论;标注"实测"的项已有数据。

### 5.1 快照字节内容变化

同一逻辑状态的快照字节与修复前不同:死栈区清零、TSC/stat 归一化字段为 0。按决定**不保留新旧 store 的兼容性逻辑**(`runtime_profile_identity()` 未变更):仓库内全部既有快照已清除(根目录与 tests/、examples/、bugs/ 下的 memory/、sstate/),新代码产生的快照彼此自洽。若用旧 store 与新二进制混跑,新旧快照不会按哈希互相命中——与"清空重来"的使用方式一致,如后续需要混用再做模型身份隔离。

### 5.2 prelude 内 TSC 语义变化

prelude 内任何代码读 TSC 得到 0(建模机器没有宿主时钟)。影响面:
- glibc HP_TIMING 仅用于 `LD_DEBUG=statistics` 输出,统计将显示 0——无功能影响;
- 若某个 libc 版本/应用在 prelude 内用 RDTSC 做**功能决策**,行为会变为"读到固定值"。与 getrandom 归零、rseq ENOSYS 属同类建模决策,DetSim 的既定语义就是"prelude 只见到被建模的宿主";
- 边界之后 TSC 已恢复可读,应用自己执行期间的 rdtsc 不受影响。

### 5.3 prelude 绑定 CPU 0 的次生效应

- **确定性**:同构主机上每次 exec 的 CPUID(1) 观察同一 APIC ID——正是修复目标;
- **并行度**:所有 tracee 的 prelude(约 1–3 ms)在 CPU 0 上短时串行。prelude 是每 tracee 一次性的启动路径,不进入搜索循环;并行 worker 同时冷启动时也只是毫秒级排队。worker 自身的 `worker_host_cpu` 不受影响(恢复的是各自 fork 时继承的掩码);
- **CPU 0 偏忙**:IRQ 常驻 CPU 0,prelude 可能略慢——无正确性影响;
- **失败模式(fail-closed)**:若部署环境的 cpuset 不含 CPU 0(容器 cgroup、isolcpus、CPU 热插拔),子进程 `sched_setaffinity` 失败,启动即终止并在 stderr 打印 `Cannot establish deterministic startup CPU/TSC policy before exec`,退出码 125(与 fixed_layout 的拒绝路径同风格)。**不会静默退化为不可复现模式**。

### 5.4 性能开销(全部只在启动路径,一次性)

- 每个 prelude RDTSC:一次 SIGSEGV + 两次额外 ptrace 停止 ≈ 每次数十 µs;glibc 2.39 全程约 4–8 次;
- 子进程栈清零 ≤256 KiB 一次 memset(COW 仅触写的页);tracer 侧边界死栈清零 ≈130 KiB 一次 `process_vm_writev`;
- 边界处两次 in-place 注入 syscall(affinity + prctl);
- 合计 **每 tracee 启动 <1 ms**,搜索 transition 循环完全不受影响。实测 mt_async 的 20 步 batch 总时长无可感知变化。

### 5.5 fstat 归一化的语义影响

stdio 初始化只消费 `st_mode`(类型)与 `st_blksize`(缓冲大小),均保留;归零的是 inode 与三个时间戳。任何依赖 stdout inode/时间戳的启动逻辑(现实中不存在)会看到 0。

### 5.6 栈清零的语义安全性

- 清零范围严格在 `rsp-128` 之下(红区不动,活帧不动);边界停止处红区内的数据是 syscall 语义保留的,实验确认不触碰;
- 死栈在 C 语义下不可读,tracee 后续只会先写后读;restore 时写回的是同样的 0,无副作用;
- 多线程:清零只发生在 prelude 边界(仅主线程存在);carrier 线程创建于恢复之后,其栈本就是新映射零页,不受影响(mt_async 回归通过)。

### 5.7 TSC 仿真中的寄存器规约(实现细节,已踩坑)

RDTSC 只写 EDX:EAX;RDTSCP 另写 ECX。若对 RDTSC 也清 ECX,会破坏调用方活寄存器(调试中曾因此导致重定位循环读 NULL 崩溃)。当前实现按 ISA 规约区分,已由动态/静态/多线程全量回归背书。

### 5.8 已知边界(如实声明)

- **prelude 内其它"时间类" syscall**(clock_gettime、time、times、sysinfo 等)当前未归一化:本机 glibc 的启动序列不出现它们(strace 全序列确认),故未加;若未来 libc 在 prelude 调用并让输出残留在活帧未初始化槽,可能需要类似 §3.1 的归一化项。这是与 fstat 同类的可扩展点,不是机制缺口;
- **边界之后**应用自身的 rdtsc/cpuid/时钟读取仍引入宿主值——DetSim 是 syscall 粒度模型,应用层消费原始时钟本就在建模范围之外(rr 靠全程虚拟化解决,DetSim 无此层);
- **PR_SET_TSC 的适用性**:Linux ≥2.6.12 的 x86 ABI,AMD/Intel 均支持(本机实验确认);非 x86 无此机制——见 §6。

---

## 6. 通用性结论:机制通用,边界显式

### 6.1 通用的部分(不依赖任何二进制布局/版本偏移)

- **TSC 虚拟化**:`PR_TSC_SIGSEGV` + 按 ISA 指令编码仿真,与 libc 版本、tracee 类型(动态/静态)、编译选项全部无关。任何 libc 或启动代码在 prelude 内读 TSC 都被拦截;
- **CPU pin 消除 APIC 漂移**:任何 x86 启动代码执行 CPUID(1) 都被同一机制覆盖,不依赖 glibc `init_cpu_features` 的存在或其缓存位置;
- **栈卫生**:锚点用真实 RSP 与 `[stack]` vma 边界,与栈帧布局、栈深、env 长度无关;
- **恢复**:边界恢复(affinity/TSC 策略交还)是平台无关的工程结构。

换言之,这次不是"往 0x37a60 打补丁"式修复——那类修复每个 glibc 版本都要重做;所有写入位置都是**由 tracee 自己的代码确定的**,我们消除的是这些位置的**取值随机性**。

### 6.2 显式的环境边界(每一项都有对应的行为约定)

| 边界 | 说明 | 当前行为 |
|---|---|---|
| **x86-64 专属** | rdtsc/rdtscp 编码、`struct stat` x86-64 布局、CPUID(1):EBX 的 APIC 语义 | 与 DetSim 现有模型一致(fixed_layout、scratch_page 同为 x86-64 假设),未缩小适用面。stat 偏移来自内核 UAPI,跨 glibc 版本稳定 |
| **同构主机假设** | pin 消除 APIC 漂移的前提是"各 CPU 的 CPUID 输出一致" | 单机同构(本机 EPYC ✓)。同机异构(大小核)上 pin 到 CPU 0 反而**增强**确定性(否则 glibc 可能按不同核的功能位选不同 IFUNC);跨主机复用 store 本就由 `compatibility.cpp` 的 `cpu_profile()` 门控 |
| **CPU 0 可用** | cpuset 不含 CPU 0 的部署 | fail-closed、错误信息明确(见 §5.3)。若未来遇到,建议扩展 `RuntimeProfile` 增加 `prelude_cpu` 配置项(本次未加,遵循 fail-closed 先行) |
| **Intel 的另一条路** | Intel 支持 `ARCH_SET_CPUID`(CPUID 故障+仿真,rr 采用);AMD(本机)不支持 | 本修复用 pin 达成同等确定性,不依赖该特性;若未来要在 Intel 上跨核记录/重放可再加 cpuid 故障路径 |
| **prelude 范围** | 只保证 exec→边界 syscall 之间写入的确定性 | 这是 DetSim 模型的自然边界,不是本修复引入的 |

### 6.3 一句话回答

**机制是通用的**:它对任何 x86-64 Linux 上的任何 libc(本机 glibc 2.39 已验证动态/静态/多线程)在同一台同构主机上都成立,且不硬编码任何版本相关偏移。**显式边界有两条**:CPU 0 必须可用(fail-closed,报错可见),以及确定性保证范围止于 prelude(边界后应用自身的宿主时钟消费属模型外)。相比之下,如果当初选择"按 glibc 布局打补丁",那就是特定于本实验环境的修复——那正是本次刻意避开的方案。

---

## 7. 后续建议(非必须)

1. 若部署环境出现 cpuset 无 CPU 0 的场景,把 prelude CPU 做成 `RuntimeProfile` 配置(`prelude_cpu`,默认 0),沿用 fail-closed;
2. 把本报告的复现脚本(cmp_stores/两运行对比)固化成 `tests/` 下的回归用例,防未来 libc 升级引入新的 prelude 漂移源;
3. 如需覆盖"prelude 内其它时间类 syscall"的极端情况,可按 §5.8 第一条的清单逐项增加归一化;
4. 提交前建议按仓库惯例跑一遍 `make format`(clang-format 对本 diff 的代码风格)。

## 8. 第二轮变更与全量回归(2026-11-19 追记)

按决定执行了三项后续工作:

1. **移除快照兼容性逻辑**:删除了 `runtime_profile_identity()` 中的两处新增字段(它们唯一的作用是隔离新旧模型的 store);
2. **清除既有快照**:根目录与 `tests/`、`examples/`、`bugs/` 下全部 `memory/`、`sstate/` 已清空;
3. **固化跨执行一致性回归**:`tests/snapshot_repro/`(动态 + 静态两个 fixture,`check.sh` 各跑两个独立 tracer 进程,要求全部快照哈希与系统状态哈希序列逐字节一致),当前双 PASS。

### 8.1 tests/ 全量跨执行一致性

方法:每个场景以相同 batch(`sw 0` + 8×`si` + `q`)在两个独立 tracer 进程运行,对比日志中全部 `StateStore saved hash` 与 `Create sstate` 序列;对 FAIL 项用**修复前基线**(git stash)对照鉴别。

| 组 | 结果 |
|---|---|
| NP=1(19 个 json) | **12 PASS**(brk、fs、hello、interrupt、mt、mt_async、mt_poll、pipe、select_poll_semantics、simple_complex、snapshot_repro×2);7 个 FAIL/SKIP 与基线完全一致(见下) |
| NP=2(poll、write) | **2 PASS** |
| NP=3(8 个 json) | 场景性 FAIL 与基线完全一致,且修复版在相同失败下 hash 序列仍一致(基线则漂移) |

**基线对照结论(非本次改动引入,预存)**:`bt`(无快照,backtrace 工具用例)、`cast_test`/`complex`/`expr_test`/`member`/`parentheses_test`×2 与 `userstate`(UserCheck 断言读超出快照区、`userstate.cmds` 在仓库中缺失,统一 batch 无法驱动)——基线即报 `Snapshot read outside captured regions`/`CHECK FAILED`,且基线的两次运行 hash 漂移、修复后一致;`devices`(openat `/dev/urandom` 未建模,基线同报);`loop`/`loop3`/`malloc`(统一 batch 下 tracee 未达边界,无快照,基线相同);`loop_inf`(无限循环 fixture,边界不可达,预期);`exception`×3(fixture 故意 exit/abort/segv,验证异常路径,预期报错)。

### 8.2 raft / redisraft 跨执行一致性

| 场景 | 修复后 | 基线(修复前) |
|---|---|---|
| examples/raft(raft.json,NP=3,静态 tracee) | **9/9 快照哈希一致、sstate 一致** | — |
| examples/redisraft(raft.json,NP=3,动态 tracee) | **6/6 一致** | **6/6 全部漂移**(基线两次运行哈希无一相同) |
| bugs/redisraft-commit-bound(raft_commit_bound.json + bfs.cmds,450 个快照) | **450/450 一致、sstate 一致,exit=0**;`[CHECK FAILED] Committed log disagreement at index 2!` 在两次运行同样触发——该用例要复现的缺陷按预期出现且**路径完全可复现** | — |

redisraft 的 `Snapshot read outside captured regions`(UserCheck 插件)为预存问题:基线同样报错。

---

## 附:复现/验证所用工具(位于 /tmp/snaprepro,未入库)

- `cmp_stores.py`:解码两份 packed store 并逐 blob/逐字节对比;
- `analyze_blob.py` / `dump_region.py`:把 blob 偏移映射回 tracee 地址并打印上下文;
- `env_list.py`:从快照栈区提取环境变量列表(用于排查实验自身污染)。
