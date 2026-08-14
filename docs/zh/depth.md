# 深度-状态数统计与增长趋势分析

## 功能说明

Tracer 会自动记录搜索深度与累计搜索状态数之间的关系，并基于记录的数据进行指数拟合，用于分析和预测状态空间的增长趋势。

该功能包含两个部分：

1. **深度统计**：记录每次达到新的最大搜索深度时的累计搜索状态数。
2. **指数拟合**：根据已记录的数据实时拟合状态空间的增长趋势，并预测后续深度的状态数。

## 数据统计

### 输出文件

统计数据保存在：

```text
depth_stats.txt
```

文件格式：

```text
# depth total_states_searched
1 150
2 892
3 4250
4 18234
...
```

其中：

* 第一列 `depth`：当前搜索深度，即从初始状态到当前状态的路径长度。
* 第二列 `total_states_searched`：达到该深度时累计搜索的状态数量，**不进行去重**。

因此，该数据表示的是：

```text
搜索深度  →  累计搜索状态数
```

而不是每一层单独包含的状态数量。

### 数据记录

当状态监控线程检测到新的最大搜索深度时，Tracer 会记录一个新的数据点。

为避免重复记录：

* 使用原子变量 `g_max_depth_recorded` 保存已经记录的最大深度；
* 只有当当前深度超过 `g_max_depth_recorded` 时才记录；
* 每个深度最多记录一次。

相关代码：

* 全局变量：`scheduler.cpp` 中的 `g_max_depth_recorded`、`g_depth_stat_file`
* 记录逻辑：`status_monitor_thread()` 中的深度计算部分
* 文件管理：`start_status_monitor()` 打开文件，`stop_status_monitor()` 关闭文件

## 指数拟合

Tracer 会根据已收集的深度统计数据，对搜索状态数进行指数拟合。

### 拟合模型

采用指数模型：

```text
states = a * exp(b * depth)
```

对模型两边取自然对数：

```text
ln(states) = ln(a) + b * depth
```

因此可以将问题转换为对 `depth` 和 `ln(states)` 进行线性回归，并使用最小二乘法估计参数 `a` 和 `b`。

其中：

* `a`：拟合曲线的系数；
* `b`：状态数随搜索深度增长的指数增长率；
* `exp(b)`：可近似理解为平均分支因子。

### 数据点

每次发现新的最大搜索深度时，Tracer 会增加一个数据点。

拟合最多使用最近的 **50 个数据点**，以使拟合结果更多反映当前搜索区域的增长特征。

至少需要 **3 个数据点**后才会进行指数拟合。

相关代码：

* 拟合函数：`scheduler.cpp` 中的 `exponential_fit()`
* 更新函数：`scheduler.cpp` 中的 `update_depth_fit()`
* 显示逻辑：`status_monitor_thread()` 中的状态监控区域

数据使用以下结构保存：

```cpp
static std::vector<std::pair<double, double>> g_depth_data_points;
// (depth, states)

static double g_fit_a;  // 指数模型系数 a
static double g_fit_b;  // 指数增长率 b
```

## 实时监控

指数拟合结果会显示在状态监控区域，例如：

```text
DepthFit: N=15 | states=1.23*e^(0.4567*depth) | d+1: 12345 d+5: 678901
```

其中：

* `N`：参与当前拟合的数据点数量；
* `states=...`：当前拟合得到的指数模型；
* `d+1`：根据当前拟合结果预测下一搜索深度的累计状态数；
* `d+5`：根据当前拟合结果预测未来 5 个搜索深度的累计状态数。

当数据点不足 3 个时，不进行拟合，并显示：

```text
collecting data...
```

## 使用示例

### 运行 Tracer

```bash
cd examples/raft
./tracer -c raft.json
```

### 查看实时统计

可以在另一个终端中查看深度统计文件：

```bash
tail -f depth_stats.txt
```

状态监控区则会实时显示当前的指数拟合结果，例如：

```text
DepthFit: N=20 | states=150.5*e^(0.5802*depth) | d+1: 8912 d+5: 985641
```

根据该拟合结果：

* 状态数随深度近似按照 `150.5 * e^(0.5802 * depth)` 增长；
* 指数增长率约为 `0.5802`；
* 平均分支因子约为 `e^0.5802 ≈ 1.79`；
* 根据当前模型可以预测后续深度的累计状态数。

## 数据分析

`depth_stats.txt` 可以进一步用于离线分析。

例如，使用 Python 绘制搜索深度与累计状态数之间的关系：

```python
import matplotlib.pyplot as plt
import numpy as np

data = np.loadtxt('depth_stats.txt', comments='#')

depths = data[:, 0]
states = data[:, 1]

plt.figure(figsize=(10, 6))
plt.plot(depths, states, 'b-o', markersize=3)
plt.xlabel('Search Depth')
plt.ylabel('Total States Searched')
plt.title('State Space Growth vs Depth')
plt.grid(True)
plt.savefig('depth_analysis.png')
```

还可以计算每个深度新增的状态数量：

```python
layer_states = np.diff(states, prepend=0)

plt.figure(figsize=(10, 6))
plt.bar(depths, layer_states)
plt.xlabel('Depth Layer')
plt.ylabel('New States in Layer')
plt.title('States per Depth Layer')
plt.savefig('layer_analysis.png')
```

其中：

```text
layer_states[i] = states[i] - states[i-1]
```

表示从上一搜索深度到当前搜索深度期间新增的搜索状态数量。

## 应用场景

### 状态空间增长预测

通过指数拟合模型，可以根据当前搜索深度预测后续搜索所需处理的状态数量，从而初步评估状态空间规模。

### 分支因子分析

对于：

```text
states = a * exp(b * depth)
```

模型，其增长因子为：

```text
exp(b)
```

因此 `exp(b)` 可以作为平均分支因子的近似值。

例如：

```text
b ≈ ln(2) ≈ 0.693
```

意味着状态数量大致以 2 倍的比例随搜索深度增长。

### 搜索策略分析

可以利用深度-状态数数据分析不同搜索策略的状态空间增长特征，例如比较 BFS、DFS 和随机搜索在不同深度下的状态分布。

### 搜索规模与复杂度分析

通过实际搜索数据，可以观察状态空间的实际增长趋势，并与理论上的复杂度预期进行比较。

## 注意事项

1. **统计文件采用追加模式**

   `depth_stats.txt` 以 append 模式打开，多次运行 Tracer 会将数据追加到同一个文件中。

   建议每次实验前删除或重命名旧文件：

   ```bash
   rm -f depth_stats.txt
   ```

2. **拟合使用局部数据**

   指数拟合最多使用最近 50 个数据点，因此拟合结果反映的是当前搜索区域的增长趋势，而不一定代表整个状态空间。

3. **指数模型存在适用范围**

   拟合假设状态数随搜索深度近似指数增长。如果搜索过程中的分支因子发生明显变化，或者状态空间存在大量合并、终止状态等情况，指数模型可能无法准确描述实际增长趋势。

4. **深度计算存在一定开销**

   搜索深度基于 `state_tree` 的状态回溯关系计算，因此深度统计功能会引入一定的额外计算开销。
