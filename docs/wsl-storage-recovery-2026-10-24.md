# WSL 满盘恢复与空间回收（2026-10-24）

用户授权清理 WSL 并压缩虚拟磁盘后，删除了已完成 DetSim 实验的生成文件，
并完成 TRIM、离线压缩和重启验证。源码、配置、性能剖析和搜索证据保留。

## 实际结果

| 项目 | 处理前 | 处理后 |
| --- | ---: | ---: |
| Debian VHDX 文件 | 141387890688 字节 | 82579554304 字节（重启后） |
| Windows D: 空闲 | 约 17 MB，维护期间最低不足 1 MB | 58781147136 字节 |
| Linux 根分区已用 | 约 128 GiB | 77921366016 字节 |

清单包含 3153 个条目：生成的 memory/sstate 存储占 47358803968 字节，重复
worker tracer 副本占 11436064768 字节，总计约 58.8 GB。副本无硬链接；删除日志
与清单集合逐项一致，复查这些路径均已不存在。

清理范围仅为 `/home/asaaoi/exp/detsim-scaling-20261019-3fb4274f/rootfs/work/results`
下的上述生成内容。保留报告、规范图、配置、journal、worker 日志、计数文件及
源码/构建输入。旧快照不能再直接 load；需要时可用保留的输入重新生成。

## 恢复流程与回退保护

1. Windows 可响应，但 D: 几乎耗尽；Debian VHDX 位于 `D:\wsl\Debian`。
   Linux 命令报 I/O error，后来启动明确报 ERROR_DISK_FULL。guest 内仍有大量
   虚拟空闲空间，因此仅检查 guest 的 df 不足以防止这种故障。
2. 停止 WSL、确认 VHD 未挂载，将完整 141.4 GB 离线副本复制至 C:。复制任务
   退出码 0、长度一致。最初通过短 SSH 会话启动的后台进程未存活，改为有独立
   状态文件的临时计划任务；未重复运行已存活的复制进程。
3. 第一次只读挂载后的 Full 压缩成功但未缩小。为提供启动空间，临时将
   `D:\SquirrelTemp` 的两个旧安装缓存共 540718519 字节复制到 C:，SHA256 校验
   后移除 D: 原件。WSL 随即恢复可启动；最后这两份文件均已按原哈希放回原位。
4. ext4 日志恢复完成，根分区 rw。此前未取回的十次测速证据直接流式传到本机，
   避免在满盘上生成另一个大 tar。本机图、配置、日志和报告审计通过。
5. 扫描并删除已审阅的生成文件清单，sync 后执行 fstrim。其报告的 183765225472
   字节是虚拟空闲范围，不能当作 Windows 实际回收量。
6. 停止 WSL，确认 VHD 已卸载；只读挂载后 Optimize-VHD Full，再卸载。
   第二次压缩由 141395230720 降至 82545999872 字节；重启产生少量增长。
7. 重启验证根分区可写、SSH 可用，无新 ext4/I/O 错误；基线 tracer 和 raft-case
   哈希未变，十组图哈希及配置/journal 仍可读取。随后移除完整临时备份，避免
   将空间压力转移到 C:；三个临时计划任务和本次保活进程均已结束/移除。

没有永久修改发行版注册、磁盘路径、ASLR 或 Windows 电源设置。WSL 原有空闲
退出行为保留，没有新增永久保活服务。恢复后的启动检查不等于完整离线 fsck。

## 证据与后续约束

本机 `build/shared-parallel/wsl-reclaim/` 保存计划、路径清单、逐项删除日志、
TRIM、备份/压缩任务记录和启动校验。最新测速数据保存在
`build/shared-parallel/restored-page-hint/timing-evidence/`，详见
[页面提示实验](restored-page-hint-2026-10-24.md)。原始传输失败时的未审计记录仍保留。

后续大实验应同时检查宿主承载盘和 guest 空间，并在结果归档后及时清理可再生
状态存储；不能把 guest 的虚拟空闲量当成宿主物理余量。当前记录只建立这条操作
约束，没有宣称已实现自动空间监控。

操作依据：[Microsoft 的 WSL 磁盘管理说明](https://learn.microsoft.com/en-us/windows/wsl/disk-space)
及 [Optimize-VHD 的只读挂载/离线要求](https://learn.microsoft.com/en-us/powershell/module/hyper-v/optimize-vhd?view=windowsserver2025-ps)。
