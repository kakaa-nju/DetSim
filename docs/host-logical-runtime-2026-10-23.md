# Hyper-V 宿主执行分布诊断（2026-10-23）

本轮观察到：12个guest worker虽占用接近12个逻辑处理器，但宿主逻辑处理器计数器上的
执行分布明显不均。它支持继续调查宿主调度/资源共享，不能单凭这次采样证明
全部搜索扩展损失都由它造成，也没有实施宿主优化。

## 方法与口径

同一算术worker，每进程固定200亿次迭代，分别1/12/24 workers，仅做诊断采样。
这是带观察器的运行，不混入此前未采样的性能排名。CPU分配仍为0、偶数0…22、
全部0…23。每次先启动持久PowerShell采样器，采集3个预备样本，再执行计算。

读取四组数据：宿主ProcessorInformation的格式化性能值，以及Hyper-V逻辑
处理器、root VP、VM VP的raw runtime。三类runtime与各自base的18项声明均
核对为precision timer542573824/base1073939712，按100*ΔN/ΔownBase换算。
使用QPC查询边界与guest请求边界关联时间，不拿有偏移的base绝对值对齐。
分析窗口为所有worker的执行交集：max(begin)到min(end)，不是整条命令时间。

逻辑处理器GuestRunTime包含宿主/root及guest执行；它不是WSL专属计数。
root VP与宿主LP有对应关系，guest VP则可迁移。因此保存三类原始数据，并把
LP Guest减Root Guest标为**非root估计**，不裁掉小的负值，不称作逐次调度轨迹。
三类provider异步取样，每个CPU的精确归属和瞬时SMT重叠仍需更强证据。
参考：[Hyper-V配置与计数器](https://learn.microsoft.com/en-us/windows-server/administration/performance-tuning/role/hyper-v-server/configuration)、[调度器及VP关系](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/manage/manage-hyper-v-scheduler-types)、[计数换算规则](https://learn.microsoft.com/en-us/windows/win32/perfctrs/calculating-counter-values)。

## 观察

以下为接受区间的中位数，busy以一个逻辑处理器满时运行=1计：

| workers | 接受区间 | 边界排除 | 无效区间 | 宿主LP busy | root busy | VM busy | VM Guest | 非root Guest估计 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 27 | 3 | 0 | 1.653 | 0.649 | 1.008 | 1.000 | 1.000 |
| 12 | 36 | 4 | 0 | 12.651 | 0.797 | 11.831 | 11.762 | 11.764 |
| 24 | 56 | 4 | 0 | 23.994 | 1.047 | 22.936 | 22.773 | 22.763 |

12-worker期间，非root Guest估计在若干相邻计数器索引上的合计均值如下。
每个数是区间百分比的算术均值；超过100表示两条LP的时间比例之和超过一条LP
容量，不是说单条LP超过100%，也不是精确到每个worker的硬件占用。

| 计数器索引对（未逐一校准） | 合计估计 |
| --- | ---: |
| 0,1 | 151.5% |
| 2,3 | 174.2% |
| 4,5 | 139.8% |
| 6,7 | 131.5% |
| 12,13 | 48.2% |
| 14,15 | 13.4% |
| 16,17 | 3.0% |
| 22,23 | 86.2% |

Windows物理核分组来自只读core关系API，但尚未逐一证明Hyper-V计数器索引与
Windows处理器编号一致，不能据此直接把上述索引对当作SMT核。其他索引的
数据完整保留于原始分析。单worker的非root估计也分布于多个宿主LP；不能把
固定guest CPU0直接等同于固定宿主CPU0。24-worker时则各核合计约182%–194%。
这些较大的分布差异值得继续核查，但仅一次诊断运行，未证明稳定的VP→LP映射。

按LP busy加权的宿主性能百分比中位数为109.34/104.96/103.96。这个上下文指标
不是每worker的真实频率，不据此单独解释或排除降频。

## 完整性与环境

六项分析测试通过，包括own-base换算、计数倒退/越界拒绝、QPC范围、实例集合、
真实预检和窗口排除。三次运行全部worker的配置、固定操作数、checksum、原始
CPU/墙钟时间及分析交集被独立核对；九项私有挂载卸载成功，输入摘要未变化，
没有本次进程/宿主rootfs挂载残留。证据在`build/shared-parallel/host-logical-runtime/`。

初次scheduler探针stdout编码解析失败，保留原始输出；显式UTF8后读取成功。
采样器stderr含两个GB18030 CLIXML模块准备progress记录，最初把stderr当UTF8空
文本的审计失败；后来按XML流类型核对为progress，未将其伪装成空stderr或忽略
未知错误。全部worker stderr为空，采样器退出0且没有记录采样错误。

Windows10Pro19045当前启动的Hyper-V事件2值为4，确认root scheduler。未修改
调度器、功耗、affinity或虚拟机设置。只读vmmem ProcessorAffinity显式getter
返回Access is denied，当前没有可用的宿主绑定实施结果。已询问是否有管理员
只读入口；同时准备绑定自建线程的Windows原生控制，可在不操作vmmem的前提下
继续诊断。CLR与Linux C++代码生成不同，后者只能比较各自内部scaling。

2026-10-24校准补充：已确认Windows偶数LP集合对应偶数计数器集合，但不是完整
逐索引/SMT配对证明。原生负载校准还发现root VP短区间大幅突增超过100%，因此
本报告的root差分继续只作估计，不作精确SMT时间证据；详见
[真实搜索分布与计数限制](search-host-placement-2026-10-24.md)。
