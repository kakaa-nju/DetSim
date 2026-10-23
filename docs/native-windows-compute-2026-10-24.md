# 宿主原生计算与恢复后的 WSL 对照（2026-10-24）

Windows/WSL恢复连接后，原生宿主控制与同一启动周期的WSL控制都完成三轮。
原生12线程聚合因子约9.95，WSL12进程约7.83；全部24逻辑处理器则两边都约
10.5–10.6。这个差距支持继续检查实际资源分配，但CLR/C++和线程/进程实现不同，
不是严格的虚拟化开销隔离实验，更不是DetSim的固定图加速比。

## 恢复与入口

Windows当前启动时间为2026-10-23T13:29:03.5Z，Hyper-V启动事件值仍为4。
此前机器不可达的调用没有启动控制程序，不能算运行失败的性能样本。恢复后的
WSL SSH可用，但该会话调用Windows exe返回Invalid argument；原生控制改走
宿主192.168.1.5直接SSH。该入口实测管理员身份，能够读取vmmem affinity，
当前mask为ffffff；此前WSL interop入口下的Access denied不代表所有入口都无权。

## 原生控制与验收

PowerShell5.1用已有.NET Framework的Add-Type /optimize+编译C#，不安装依赖。
仅新建本次worker线程，用SetThreadAffinityMask绑定并通过GetThreadGroupAffinity
回读；finally恢复并回读原mask。没有修改vmmem或其它进程的绑定。每个worker
固定50亿次八链uint64算术，开始/结束核号检查在计时外；CPU时间窗口在QPC内。
ready/go上限30秒、run上限240秒，异常/超时只结束本次控制进程。

第一次cross-check暴露了root审查中的版本混淆：root依据较早草稿给C#额外加了
mix，而最终已测Linux版本使用XOR。随后核对Linux源码、短测试和已测二进制摘要
一致，没有实验后漂移；C#改回已测算法。失败输出保留，未更改Linux测量二进制。
修正后1/12/24线程10000次短测试与Linux checksum一致，实际CPU及affinity恢复
检查通过；零迭代返回失败和非零退出码。修正前的native-mix短测试不算验收通过。

源与原始数据在`build/shared-parallel/windows-compute-control/`。九次长测量
逐项核对原始stdout、退出状态、源码摘要、操作数、核号、恢复标志、checksum和
QPC聚合跨度。全部checksum为0x34fb4dd3daf0781f。GetThreadTimes观察到粗粒度
增量，所以保留begin/end原值，不强行要求短区间CPU时间严格等于QPC墙钟时间。
所有归属PowerShell进程已退出，vmmem的PID、创建时间和原mask未变化。

## 同启动周期结果

WSL沿用冻结C++二进制90d6546d19f7b5b79c5743ea179c504ba3c978c5bcb43d068e43f357f5588026，
相同算术与操作数，1/12/24进程三轮交错，输入摘要和所有原始结果通过独立审计；
无swap计数增量，三项私有挂载卸载成功，没有本次进程或挂载残留。数据在
`build/shared-parallel/compute-placement-recovered/`。

| 控制 | 数量 | 组三轮中位数（秒） | 逐轮聚合因子的中位数 |
| --- | ---: | ---: | ---: |
| Windows原生，CPU0 | 1 | 9.41235 | 1.00× |
| Windows原生，宿主偶数LP | 12 | 11.35576 | 9.95× |
| Windows原生，全部LP | 24 | 21.48865 | 10.52× |
| WSL，guest CPU0 | 1 | 9.62537 | 1.00× |
| WSL，guest偶数CPU | 12 | 14.61031 | 7.83× |
| WSL，全部guest CPU | 24 | 21.49657 | 10.64× |

因子是N份独立固定工作任务的N*T1/TN，各轮计算后取中位数。两种runtime分别
成组测量而非跨runtime完全交错；JIT/codegen、进程/线程形态、系统波动仍是
限制。不与重启前的绝对耗时混用，不据此宣布实现了近线性模型检验。

下一步在任何虚拟机进程绑定试验前，必须先核实线程绑定现状并准备独立的定时
恢复机制和恢复验证；宿主设置修改尚未执行。
