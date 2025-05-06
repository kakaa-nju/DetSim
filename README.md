# Intro

DetSim is a deterministic simulation testing environment for distributed systems.

# 依赖

- libcereal-dev
- libdwarf-dev
- libelf-dev
- libreadline-dev
- libcjson-dev

# 编译

```
make NP=$(NODES)
```

需要通过定义指定被测系统的节点（进程）数量。

## 编译被测程序

```
CFLAGS += -static -gdwarf-4
```

# 启动

## 配置文件

tests/ 下给出了一些例子

```
./tracer -c tests/raft.json
```

注意：编译时设置的 NP 需要与配置文件中的 "Nodes" 数量一致。

# 使用

通过 cli 交互界面进行操作。

- `c`: 自动探索模式
- `sw N`: 切换到第 N 个进程
- `si`: 当前进程向前执行，直至遇到需要进行 checkpoint 的系统调用
- `info`: 打印当前状态，系统调用历史序列
- `load HASH`: 加载 HASH 前缀所指定的全局状态
- `q`: 退出
