# WSL CPU 样本的 libc 符号补全（2026-10-23）

对[既有任务内 CPU 样本](worker-cpu-scaling-2026-10-23.md)离线补符号，没有
重新执行工作负载。补全结果未发现一个足以单独解释扩展损失的 libc 热点。

## 同名 DSO 的版本冲突

perf 5.10 对系统范围采样中的 `/lib/x86_64-linux-gnu/libc.so.6` 记录了宿主
glibc 2.31 的 Build ID `a727537547829074887adbdd56624e44bb0011bb`。
chroot 内 worker 使用另一份 libc，Build ID 为
`5e5b51f4c39af9ac589408c1dc432ba390a273cc`。verbose 解码明确报告 build-id
mismatch，故仅为目标 libc 提供正确调试文件，并不能让该次 perf 解码自动恢复符号。
没有将宿主 libc 的符号强套到 worker 地址。

本机恰有与目标 Build ID 一致的独立 debug 文件，其 SHA256 为
`eec1f26d1accfe8f3ff4a4a11fe381fa0184f242e62ef7c6f44dab59a747a67f`。
只将它复制到远端私有实验目录及该目录下的 rootfs，不安装系统软件包。

替代解析严格限定为原 journal 中的 worker main PID 和原 Task active 时间窗：

- 逐 worker 从原 perf MMAP2 记录提取唯一 libc 可执行映射，要求 PID/TID 一致、
  映射早于任务窗口、inode 与实验 rootfs 中 libc 的 79323 一致。
- 每个 libc leaf 的原 IP 必须落入此映射，换算为 `IP - map_start + file_offset`。
  该 ELF 的可执行 LOAD 段文件偏移与虚拟地址同为 0x28000，已核对，不将此等式
  推广到任意 ELF 段。
- 用匹配的 debug 文件进行 addr2line；未知地址保留未知。不是修改 perf 原数据或
  清除 build-id 检查，也不推断未捕获的栈帧。
- 旧/新 perf 文本逐行核对 header、地址、DSO、非帧行；直接补 debug 的那次解码
  未改变样本。独立解析复用原窗口，任务内样本数严格保持 5098 / 7219，libc leaf
  计数保持 1172 / 1614，与原分类逐项闭合。

## 新增可见信息

下表为 exclusive leaf，分母是全部任务内 CPU-clock 样本，不是 libc 子集。

| 函数 | 1 worker 样本 / 占比 | 12 workers 样本 / 占比 |
| --- | ---: | ---: |
| `__memcmp_avx2_movbe` | 180 / 3.53% | 259 / 3.59% |
| `__vfscanf_internal` | 163 / 3.20% | 188 / 2.60% |
| `__memcpy_avx_unaligned_erms` | 92 / 1.80% | 137 / 1.90% |
| `_int_malloc` | 58 / 1.14% | 93 / 1.29% |
| `tcache_get_n` | 51 / 1.00% | 71 / 0.98% |

1-worker 的 libc leaf 全部得到函数名；12-worker 剩 1 个未知地址 0x282c0，
未猜测其函数。其他 DSO 的未知仍然存在。

利用原栈中最近的已知用户祖先，memcmp 样本里可归到 `SharedObjectPool::publish_blob`
的有 75 / 87，`capture_stopped` 为 27 / 61，`save_paged_snapshot` 为 18 / 31。
这些说明重复内容比对值得继续审视，但不能因此删除 hash 冲突检验。
vfscanf 样本没有已知用户祖先，不能仅凭函数名认定全部来自 `/proc/maps`。

样本份额不是墙钟时间，也不证明并发多执行了多少指令。库内部缺 frame pointer
仍会截断栈，补符号不会修复展开。当前结果不支持“单个 memcpy 或 malloc 热点
主导全部扩展损失”的结论；下一步应按具体调用路径验证减少重复工作是否有效。

分析脚本、逐偏移结果、父调用分类、verbose 错误和 ELF 核验在
`build/shared-parallel/libc-symbols/`。远端原数据及解码在实验目录下的
`worker-cpu-20261023/` 和 `libc-symbols/`；原 perf 数据未改写。
本次是离线证据补全，没有产品代码变更，也没有新增运行时性能测量。
