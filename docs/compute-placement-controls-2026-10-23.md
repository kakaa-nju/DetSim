# WSL 长计算与 CPU 分配控制（2026-10-23）

结论：把工作量扩大10倍后，12个worker的计算控制仍只有约8.2–8.3倍聚合吞吐，
偶数、奇数、连续12个guest CPU没有明显区别。全部24个逻辑CPU约10.8倍。
这说明不能只把此前结果归于短测量，但仍不是DetSim的固定图加速比或硬件上限。

## 方法与结果

沿用资源控制实验的同一冻结worker与runner，纯计算8条独立uint64算术链，每
worker固定50亿次迭代。三轮分别正序、反序、轮转case顺序；每轮有CPU0单worker
基线。没有文件、IPC或ptrace操作进入循环，没有新库/宿主配置/产品修改。

三轮中位数：

| guest分配 | workers | 组跨度秒 | 聚合吞吐因子 | 平均每job记账CPU成本比 |
| --- | ---: | ---: | ---: | ---: |
| CPU0 | 1 | 9.45286 | 1.00× | 1.00× |
| 0,2,…,22 | 12 | 13.83807 | 8.20× | 1.39× |
| 0,…,11 | 12 | 13.68588 | 8.29× | 1.39× |
| 1,3,…,23 | 12 | 13.66090 | 8.32× | 1.38× |
| 0,…,23 | 24 | 20.91680 | 10.81× | 2.20× |
| 0,4,…,20 | 6 | 11.31717 | 5.01× | 1.14× |

聚合因子按每轮 `N*T_single/T_group` 算后取中位数，不是简单拿表中两个
中位数相除。CPU成本也先按轮计算；guest CPU时间不等于宿主实际执行时间。
没有同时采集宿主运行/频率数据，不能据此断言SMT配对、降频或Hyper-V调度就是
原因。尚无根据为了这个结果改变产品默认CPU分配。

## 核验与拓扑查询

18个结果均逐worker核对参数、操作数、CPU、checksum、原始计时、汇总窗口和
退出状态；checksum全部为`0x34fb4dd3daf0781f`。计时外vmstat没有swap计数增量。
四项输入摘要未变，三项私有挂载卸载成功，未发现本次进程或宿主挂载残留。
证据在`build/shared-parallel/compute-placement-control/`，包括逐轮原始结果、
命令、独立audit与清理记录。原有worker的八项边界测试不因调整操作数而重做。

实验全部结束后，使用只读Windows API查询宿主core关系。返回12个core、每组
两个逻辑处理器，分组为[0,1]、[2,3]直到[22,23]，均在processor group0。
查询脚本限制为64位进程，验证变长记录边界、关系类型和每核GroupCount。
这只描述Windows编号；不能因为guest编号相同就推断它们固定对应这些宿主CPU。
API与结构依据：[GetLogicalProcessorInformationEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex)、[PROCESSOR_RELATIONSHIP](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_relationship)。

下一步应分清虚拟CPU实际运行位置、宿主资源共享和工作集成本；先做诊断，再把
有证据支持的调整放回同一张真实搜索图验证。不能通过换分母或挑某一轮较慢的
单worker基线宣称接近线性。
