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

为了确保分析准确，需要关闭 Linux 系统的地址空间随机化机制。

```sh
root# echo 0 > /proc/sys/kernel/randomize_va_space
```

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

在自动探索模式中键入 Ctrl-C 将会发送一个 SIGINT，并最终停止探索，回到交互界面。此时除了最后一个到达的状态以外，状态队列全部清空。
