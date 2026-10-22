# 共享池类型化缓存键（2026-10-23）

共享池命中内存缓存时，原先仍格式化十六进制文件名，并拼接类型前缀作为字符串
键。改为内部 `(Kind, hash_type)` 键，真实文件名只在需要访问文件时格式化。
这是通用共享池优化，没有工作负载特判、额外依赖或存储格式变化。

Blob/Ready 类型仍参与相等比较；缓存仍属于各自 pool，维持既有 LRU、容量和
immutable buffer 所有权。checksum、冲突比较、文件验证、锁、只读检查、失败
poison 与持久化 barrier 均保留。没有以“命中缓存”为由制造成功或跳过这些条件。

## 验证

先在原实现与私有候选上运行扩展组件测试，再推广到产品源码。新增覆盖同一个
数字 key 下 Blob/Ready 的不同内容、冷热读取、重复发布、命中不触及磁盘、
非法 Blob 与 Ready 冲突后仍可读、不同 pool 隔离以及 warmed read-only 拒绝发布。
原有并发发布、部分写、损坏、64 MiB 容量淘汰/保留 owner 和 barrier poison
测试继续通过。DeepSeek-V4.1-Flash / max 做了独立静态审查；最终验收由主 agent 完成。

完成的命令与层级：

```sh
make check CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4
make NP=3 CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4
make NP=2 check-shared-object-pool check-shared-snapshot CC=/usr/bin/gcc CXX=/usr/bin/g++ -j4
python3 build/shared-parallel/shared-dispatch-batch/native_gates.py \
  --tracer /home/kaguya/code/detsim/tracer \
  --output /home/kaguya/code/detsim/build/shared-parallel/typed-pool-cache/production-native-gates
```

这些分别提供组件/构建契约、生产编译、共享 StateStore/快照集成和七项原生场景
证据；构建不当作测试。原生门禁包含 full/pages、durable/shared 的完整图，
worker loss/retry、启动超时、中断和违规重放。没有据此关闭通用状态等价或所有
程序 interleaving 的未决问题。

## 完整探索测量

固定 Raft-library fixture、NP3、pages/shared、BFS depth 12、seed 1、same-cpu、
locality none。实验基线与候选使用同代完整 full-FP 对象，仅替换共享池实现；
两者都为 O3、调试信息、frame pointer、禁用 sibling-call 优化。
基线 SHA `8115a52dc572ceddfec31a5732593846e29cac33d8033a967eab91b1e7c60f0b`，
候选 SHA `ef808de53b2db36fcf644a2ac4e4ec9231a467702e8bc3f88f191cea95cc75c7`。
以下不是默认 release flags 的单独计时，也不是只测缓存的微基准。

每配置交错顺序运行，本机三轮、WSL 两轮。所有 20 个计时运行及新增的 3 份
native-reference 运行均通过 10457 节点、18266 边、18266 尝试边、零重试；
规范图摘要保持 `56ccf7cc9b866e62f6da715136eb596719b3f2b101915e93e4cf0c630217717b`。
native 串行参考验证节点与首次发现父边，完整带标签图另外比较，范围不扩大。

| 主机 / workers | 原实现中位数（秒） | 类型化键中位数（秒） | 耗时变化 |
| --- | ---: | ---: | ---: |
| 原生 6 核主机 / 1 | 31.8612 | 30.8927 | -3.04% |
| 原生 6 核主机 / 12 | 7.4434 | 7.1696 | -3.68% |
| gpunode WSL / 1 | 24.2485 | 24.0600 | -0.78% |
| gpunode WSL / 12 | 3.8429 | 3.7826 | -1.57% |

口径仍是搜索命令至提示符，加图验证之后的关闭；两段不连续，中间验证可以与
后台写回重叠，不等于连续端到端时间或立即关闭成本。样本少，本机 12-worker
逐轮存在明显波动，不能宣称普适精确百分比。WSL 两轮配对均同方向。
这是小幅绝对提速，12-worker 相对单-worker 的加速比基本未变，不能据此声称
已解决接近线性扩展的问题。

实现及验证证据在 `build/shared-parallel/typed-pool-cache/`；WSL 原始结果、
重算图摘要、输入核验和回收记录在 `build/shared-parallel/typed-pool-wsl/`。
远端三项 namespace 卸载成功，最终无本实验可识别的进程或宿主挂载残留。
代码已进入主干；独立搜索并发控制揭示的更大扩展性损失仍需继续调查。
