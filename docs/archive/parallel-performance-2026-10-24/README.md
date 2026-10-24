# 并行性能归档附件

总览与恢复路线见[归档正文](../parallel-performance-2026-10-24.md)。这些是研究附件，
不是生产补丁发布；goal 处于暂停状态，不能因为附件存在就自动恢复实验。

| 文件 | 用途 |
| --- | --- |
| `mapped-pool.patch` | 私有共享字节原型与当前产品源码的差异，含 ImmutableBytes、新 owner 接口、池映射和复制适配。 |
| `mapped-large-policy.patch` | 在前一个补丁基础上，只映射至少4096字节对象。 |
| `verification-adaptations.patch` | 实际测试中的显式 mutable 拷贝、span linker wrapper、mapped cold oracle 等适配；需结合当前构建系统复查。 |
| `immutable-bytes-test.cpp.txt` | 当时的只读、move、fd/path 生命周期检查源码文本，不是自动测试入口。 |
| `journal-batching.patch` | 独立的 journal 原型，未合入；不要与映射补丁默认叠加测速。 |
| `journal-test.cpp.txt` | 当时的日志组件测试；依赖从实际源码提取的 `JOURNAL_HEADER`。 |
| `key-results.json` | 从既有已审计产物摘存的关键结果，不代替原始图、journal 或 perf。 |
| `manifest.json` | 归档基准 commit、附件 SHA256/长度、未纳入 Git 的待处理产物标识。 |

## 重新打开原型时

归档基准是 `9e24a1cd14132ee77c8389756bf02d93f3848887`。补丁只包含预期修改的
源文件，没有把旧冻结树中无关的实现覆盖回产品。未来请在隔离 worktree/分支中
复查差异和应用条件，不直接覆盖主干，不假定旧代码仍能无冲突应用。

映射补丁默认的私有宏是零，因此单独应用后不是“开启 mmap”：

- 新接口压缩堆：`DETSIM_EXPERIMENT_POOL_RAW=0`、`DETSIM_EXPERIMENT_POOL_MAPPED=0`。
- 新接口原始堆：`RAW=1`、`MAPPED=0`。
- 全部原始对象映射：`RAW=1`、`MAPPED=1`。
- 大对象映射：在前项上再应用 `mapped-large-policy.patch`。

上述缩写指相同的 `DETSIM_EXPERIMENT_POOL_` 前缀。宏应同时传给相关源码与测试；
原来的临时构建只对对象池翻译单元设置行为宏。完整 tracer 必须使用统一的新
owner 定义重新编译，不能混用旧 `vector` ABI 的对象文件。旧模型字节格式没有
改动，不等于内部 C++ API/ABI 没改动。

测试适配保留了原有负例。mapped tiny oracle 的额外 generation 变化会触发 VFS
注册缓存重绑定，所以先检查重新捕获的模型身份不变，再计原有保存次数；不能
把原断言简单改成更宽松的不等式。mapped 路径不能用 packed-only 的 disk_reads
来证明冷读取。poison 之后不能再向池调用正常读接口来检查 backing。

旧的完整构建/测试/运行脚本仍在 `build/shared-parallel/mapped-pool/`，以及
`mapped-pool-runs/`、`mapped-pool-timing/`、`mapped-pool-resources/`。它们依赖
当时的冻结源码、对象、机器、路径和固定输出目录，并非可携带的一键复现包。
恢复时应按正文验收清单重新建立命令，先检查目标是否已经完成，禁止盲目重跑。

## 尚未归档为结论的东西

资源采样 tar 已下载但未审计；只在 manifest 中标识原路径、大小和 SHA256，
没有提取其数值或宣称内存收益。新重编译的原始基线已有成功构建记录，但没有
运行对照。巨大的二进制、perf 原始采集和 runtime snapshots 不包含在本目录；
若需要长期迁移这些数据，应另行复制并核对，而不是认为 Git 已保存全部现场。
