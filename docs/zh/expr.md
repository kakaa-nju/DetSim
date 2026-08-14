# 表达式求值

DetSim 提供了类似 gdb 的表达式求值功能，支持变量访问、指针操作、结构体成员访问、类型转换等。

## 支持的表达式类型

### 基本表达式

- **变量访问**: `g_int`, `g_long` - 访问全局变量
- **数组索引**: `g_int_arr[2]`, `g_str[0]` - 数组元素访问
- **指针解引用**: `*g_int_ptr`, `**g_double_ptr` - 多级指针解引用
- **取地址**: `&g_int` - 获取变量地址

### 结构体和联合体

- **成员访问**: `g_basic.x`, `g_basic.y` - 结构体成员访问
- **指针成员访问**: `g_basic_ptr->x`, `g_rect_ptr->br.x` - 通过指针访问成员

### 类型转换

支持 C 语言风格的类型转换，包括结构体指针转换：

```c
// 将 void* 转换为具体结构体指针
((struct hidden_struct*)g_container.hidden_data)->secret_value

// 基础类型指针转换
(int*)g_generic_ptr

// 结构体指针转换
(point*)g_generic_ptr
```

这在被测系统将结构体隐藏在 `void*` 后面时特别有用，可以通过类型转换重新访问结构体细节。

### 进程限定

在多进程模式下，可以使用 `traceeN()` 语法指定表达式在哪个进程的上下文中求值：

```
tracee0(g_int == 42)          # 在进程 0 中求值
tracee1(g_container.num_rects)  # 在进程 1 中求值
```

### 运算符

- **算术**: `+`, `-`, `*`, `/`, `%`
- **比较**: `==`, `!=`, `<`, `>`, `<=`, `>=`
- **逻辑**: `&&`, `||`, `!`
- **位运算**: `&`, `|`, `^`, `~`, `<<`, `>>`
- **自增/自减**: `++`, `--`（前缀和后缀）
- **三元条件**: `cond ? then : else`

### 特殊操作符

- **sizeof**: `sizeof(g_int)`, `sizeof(int)` - 获取大小
- **offsetof**: `offsetof(struct point, x)` - 获取成员偏移
- **typeof**: `typeof(g_int)` - 获取类型名

## 使用示例

### 基本类型检查

```
(pmc) p g_int
g_int = 42

(pmc) p g_int == 42
true

(pmc) p g_char == 'A'
true
```

### 数组和指针

```
(pmc) p g_int_arr[2]
g_int_arr[2] = 30

(pmc) p *g_int_ptr
*g_int_ptr = 42

(pmc) p **g_double_ptr
**g_double_ptr = 42
```

### 结构体访问

```
(pmc) p g_basic
{
  x = 100,
  y = 200,
}

(pmc) p g_basic_ptr->x
g_basic_ptr->x = 100

(pmc) p g_rect_ptr->br.x
g_rect_ptr->br.x = 100
```

### 类型转换

```
(pmc) p (int*)g_generic_ptr
0x404080

(pmc) p ((point*)g_generic_ptr)->x
((point*)g_generic_ptr)->x = 10

(pmc) p ((hidden_struct*)g_container.hidden_data)->secret_value
((hidden_struct*)g_container.hidden_data)->secret_value = 42
```

## 实现机制

DetSim 采用第一种实现方式：在 tracer 中解析表达式，通过 DWARF 调试信息获取变量地址、类型和成员偏移量，使用 ptrace 读写 tracee 内存来进行求值。

表达式解析使用 flex/bison 实现完整的 C 表达式语法，AST（抽象语法树）节点负责实际的求值逻辑：

- `VariableNode`: 变量查找，通过 DWARF 获取地址
- `MemberAccessNode`: 成员访问，支持 `.` 和 `->`
- `ArrayIndexNode`: 数组索引
- `DereferenceNode`: 指针解引用
- `CastNode`: 类型转换
- `BinaryOpNode`: 二元运算

类型系统通过 `type_info` 结构维护，支持基础类型、结构体、数组和指针的完整类型信息。
