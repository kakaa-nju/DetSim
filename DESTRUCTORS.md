# 完整析构函数和内存清理实现

## 1. StateStore 清理

### 析构函数
- `~StateStore()` 调用 `shutdown()`

### shutdown() 方法
- 停止 io_worker 和 prefetch_worker 线程
- 清空 L1 (hot_cache_) 和 L2 (warm_cache_) 缓存
- 清空 state_queue_
- 清空 prefetch_inflight_ 和 prefetch_queue_
- 清空 io_queue_
- 重置所有内存使用计数器

### 新增清理方法
- `trim_l2_cache(target_bytes)` - 修剪 L2 缓存到目标大小
- `clear_l1_cache()` - 完全清空 L1 缓存
- `clear_l2_cache()` - 完全清空 L2 缓存

## 2. SysStateStore 清理

### 析构函数
- `~SysStateStore()` 调用 `merge_incremental_index()` 和 `flush_index()`

### shutdown() 方法 (新增)
- 合并增量索引到主索引
- 刷新索引到磁盘
- 清空 index_ 和 incremental_index_
- 重置所有统计计数器

## 3. tracee_state / sys_state 清理

### clear() 方法 (新增)
- `tracee_state::clear()` - 重置对象到默认状态，释放所有成员内存
- `sys_state::clear()` - 重置对象到默认状态，释放所有成员内存

### 析构函数
- `~tracee_state() = default` (成员自动析构)
- `~sys_state() {}` (成员自动析构)

## 4. DWARF 资源清理

### cleanup_dwarf() 函数 (新增)
- 调用 `dwarf_finish()` 关闭 DWARF 调试句柄
- 关闭 dwarf_fd 文件描述符
- 清空 global_vars 和 type_cache

## 5. 配置内存清理

### cleanup_config() 函数 (新增)
- 释放所有通过 malloc 分配的 argv 字符串
- 在 config.cpp 中实现

## 6. 全局清理函数

### cleanup_all() 函数 (新增)
在 state.cpp 中实现，按顺序执行：
1. `cleanup_dwarf()` - 清理 DWARF 资源
2. `StateStore::shutdown()` - 停止线程，清空缓存
3. `SysStateStore::shutdown()` - 刷新并清空索引
4. 清空 ptmc_state (source_state, dest_state, sock_states, fs_states 等)
5. 清空全局容器 (state_tree, state_queue, state_set)
6. 释放 rseq_struct 内存
7. `cleanup_config()` - 释放配置内存
8. 释放 membuf

### 调用位置
- `main.cpp` 在 `ui_mainloop()` 返回后调用 `cleanup_all()`

## 7. BFS 循环中的定期清理

### scheduler.cpp
- 每处理 1000 个状态，自动清理：
  - `clear_l1_cache()` - 清空 L1 缓存
  - `trim_l2_cache(l2_capacity / 2)` - 限制 L2 为 50% 容量

### 状态对象清理
- 在 `delete state_fetched` 之前调用 `state_fetched->clear()`

## 8. 已修复的内存问题

### 修复 1: Double-free (choose.cpp + emu.cpp)
- 移除了 emu.cpp 中冗余的 `free(out->args[0])`

### 修复 2: AST 节点泄漏 (expr_parser.y)
- 添加了 `%destructor` 处理 `node_val` 和 `str_val`

### 修复 3: utils.cpp 错误路径泄漏
- 在错误返回前添加 `free(in_buf)` / `free(out_buf)` / `fclose()`

### 修复 4: warm_lru 无限增长 (state_store.cpp)
- 只在 `in_l2(hash)` 为真时才调用 `update_warm_lru(hash)`

### 修复 5: clear() 位置错误 (scheduler.cpp)
- 将 `clear()` 从循环内部移到 `delete` 之前，避免 goto again 时访问已清理对象

## 9. 静态/全局对象列表

### 已添加清理
- [x] `PTMC_STATE ptmc_state` - 在 cleanup_all() 中清理
- [x] `TSS state_tree` - 在 cleanup_all() 中 clear()
- [x] `LSS state_queue` - 在 cleanup_all() 中 clear()
- [x] `SSS state_set` - 在 cleanup_all() 中 clear()
- [x] `rseq_struct/rseq_len` - 在 cleanup_all() 中 free 和 clear()
- [x] `membuf` - 在 cleanup_all() 中 free
- [x] `global_vars/type_cache` - 在 cleanup_dwarf() 中 clear()
- [x] `argv strings` - 在 cleanup_config() 中 free

### 单例对象 (自动析构)
- [x] `StateStore` - static instance，程序退出时析构
- [x] `SysStateStore` - static instance，程序退出时析构

## 10. 工作线程清理

### io_worker
- 在 `shutdown()` 中被通知停止
- 处理完队列中的剩余任务后退出
- 所有 entry 引用通过 shared_ptr 管理，自动释放

### prefetch_worker
- 在 `shutdown()` 中被通知停止
- 清空 prefetch_queue_ 后退出

## 使用说明

正常退出流程：
1. 用户输入 `q` 或 `quit`
2. `cmd_q()` 设置 `ptmc_state.state = PTMC_QUIT` 并返回 -1
3. `ui_mainloop()` 返回
4. `main()` 调用 `cleanup_all()`
5. 所有内存被释放，程序正常退出

预期 Valgrind 结果：
- 无 "definitely lost" 内存泄漏
- 无 "indirectly lost" 内存泄漏
- "still reachable" 应该最小化（主要是单例和全局状态）
