# ARC 与错误通道设计草案（0.7.x / 0.8）

> **0.9.3 状态更新**
>
> 0.9.3 继续把 C 后端 ARC 打磨到「可长期使用」：
>
> * **A1 折叠串 bug 已修**：`let s = "a" + "b";`、`const F = "x" + "y";`
>   这类编译期折叠出的字符串常量在 `--arc` 下走 `internLit`（`refs = -1`
>   的静态对象），不再发裸 C 字面量导致 retain/release 读到非法头。
> * **打印临时串已回收**：`println(int/float/数组/struct/可选值)` 生成的
>   临时字符串在打印后 `lx_gc_release`（bool 与 string 借用不释放）。
> * **A2 不变量核销**：return 走 coerceStore、赋值先存后释、cleanup 仅挂
>   ARC 引用类型、`for c in s` 循环变量每轮释放、struct `cn_on_zero` 逐个
>   发射、字面量包装区先于全局常量。
> * **原生后端未初始化 string 修复**：`let s: string;` 在原生后端指向合法
>   空串对象，不再是 NULL。
>
> **已知缺口（留待 1.0 前）**：`T?` 装箱仍只增不减（内存安全但会泄漏）；
> 原生后端仍是 bump 分配器（`--arc --native` 明确报错）；`main(argv)` 的
> argv 数组是借用参数方案下的一次性泄漏。

> **0.9.2 状态更新（拍板：ARC 进 1.0）**
>
> 本篇是 0.7.x 的设计草案。0.9.2 已把 ARC 的**表示层 + C 后端**落地
> （`--arc`，默认关闭），设计与本文 §2.1 / §2.2 一致：
>
> * 每个堆对象（string / 数组 / struct）前面加 16 字节头：`refs` + `on_zero`；
>   字符串字面量是 `refs = -1` 的静态不可变对象。
> * C 后端在 `let` / 赋值 / 返回 / 数组元素写入 / struct 字段 / 嵌套拼接处
>   插入 retain / release；局部变量用 `__attribute__((cleanup))` 在作用域出口释放。
> * extern fn 的 string 参数与返回值保持 `const char*` **借用视图**，不参与回收。
> * 原生后端仍未实现（free list 顺延到 0.9.3）。
>
> 详细的拍板与覆盖范围见 `docs/stability.md` §6.1；实测内存回归：1e6 次
> 字符串构造的峰值 RSS 从 ~117 MiB 降到 ~11 MiB。
>
> 错误通道（A3）已在 0.8.0 完成，本文 §3 即为最终设计。

0.7.0 完成了语言层最显眼的部分（struct / if 表达式 / argv / 复合赋值），
但清单里两件最难、也最重要的地基工程 **ARC（A1）** 与 **错误通道（A3）**
按计划留到 0.7.x–0.8。本文记录设计取舍与落地顺序，避免边写边改。

## 1. 现状（0.7.0）

| 对象 | C 后端 | 原生后端 |
| --- | --- | --- |
| `string` | `const char*`（`malloc`，从不释放） | `[i64 len][bytes][NUL]`（bump，从不释放） |
| 数组 `T[]` | `struct lx_arr*`（`calloc` 头 + `realloc` 数据） | `[len][cap][data][ek]`（bump，扩容后旧块泄漏） |
| `struct` | `lx_st_X*`（`calloc` N×8） | N×8（bump） |

结论：**内存只增不减**。对短命脚本无害，对长驻 / 大循环是硬伤，也是 1.0
不能带着发布的问题。

## 2. ARC 设计（A1）

### 2.1 表示层

字符串是最大障碍：现在字面量（`.rodata` 的 `const char*`）与堆分配串
无法区分，无法统一 release。因此字符串必须换底：

```c
typedef struct lx_str {
    int64_t refs;
    int64_t len;      // 字节数（保留 len 字段可避免每次 strlen）
    char*   data;     // 堆数据；字面量用静态 lx_str 持有
} lx_str;
```

- 字面量：编译期生成静态 `lx_str`（`refs = -1` 作哨兵，retain/release 跳过）。
- 数组 / struct：头部加 `refs` 字段（数组头已经是堆指针，天然可共享；
  struct 同理）。
- 元素级引用：`lx_arr` 的元素若为 string / array / struct，需要在
  `push` / `set` / `pop` / `remove` / `clear` / 切片复制时对元素做
  retain/release。这是 ARC 里最容易漏掉的部分，测试要专门覆盖。

### 2.2 Codegen 插入点

在 **C 后端**，对每个「拥有引用」的表达式值插入 retain/release：

| 位置 | 动作 |
| --- | --- |
| `let x = e` | 生成 `lx_retain(e)` 后赋值；变量离开作用域时 `lx_release(x)` |
| `x = e` | 先 `lx_retain(e)`，再 `lx_release(旧 x)`，最后赋值 |
| 传参（非 extern） | 调用前 retain（callee 不拥有，或按借用约定） |
| `return e` | 返回值转移所有权（不做额外 retain），调用点负责 release 临时 |
| 数组 / struct 元素写入 | 元素 retain，旧元素 release |
| 语句结束的临时值 | release |

难点是**控制流与语句流的精确释放点**（break / continue / return / panic
都要释放已持有的局部变量）。最简可行方案：每个函数维护一个「活引用表」，
在函数出口统一 release（牺牲一点峰值内存，换正确性）。先做对，再做省。

### 2.3 原生后端的 free list

bump 分配器不能 free。ARC 减到 0 后内存需要回收，0.7 提案采用
**尺寸分级的 free list**：

- 分配：把请求大小向上取整到 2 的幂（16 / 32 / 64 / … / 1 MiB），
  先查对应桶；桶空才 bump。
- 释放：refs 归零后把块头（或数据块，取决于布局）链入对应桶。
- 桶本身用 bss 固定数组（不额外分配），实现保持无锁（单线程）。
- 与现有「字符串 / 数组 / 结构体都是裸指针 + 对象头」的表示兼容：
  对象头统一加 `refs`，free list 只认块大小。

这是 0.7 里唯一的硬设计题——机制本身不复杂，难的是它牵动**所有后端的
所有值传递路径**，必须靠「C 后端 ↔ 原生后端逐字节差分」兜底。

## 3. 错误通道设计（A3）

### 3.1 类型表示

用 `named("Result")` 承载 `T?`，成员为 `(ok: bool, value: T, err: string)`；
`?` 是传播运算符，`or` 是兜底表达式：

```lux
fn read_config(p: string) -> string? {
    let data = read(p)?;          // 失败直接向上传播
    return parse(data)?;
}

fn main() {
    let cfg = read_config("a.toml") or "default";
    println(cfg);
}
```

### 3.2 语义迁移（破坏性，必须赶在 1.0 前）

| 现在（0.7） | 迁移后（0.8） |
| --- | --- |
| `int("abc")` 静默返回 0 | 返回 `int?`；`int!` 保留 panic 版本 |
| `read(path)` 失败即 panic | 返回 `string?`；`read!` 保留 panic 版本 |
| `find(s, sub)` 找不到返回 -1 | 返回 `int?`（或保留 `find` 裸 int、新增 `find_opt`） |

标准库函数成对提供：`name`（返回 `T?`）与 `name!`（panic 版本），
脚本场景仍可用 panic 获得简单体验。

### 3.3 编译期校验

- `?` 只能用在返回类型为 `T?` 的函数里，且被传播表达式的类型必须是 `R?`。
- `or` 左侧必须是 `T?`，右侧类型必须能相容到 `T`。
- 延续 `format_arity` 风格：误用 `?` / `or` 给出**定向**错误码，而不是
  泛化的「类型不匹配」。

## 4. 落地顺序（0.7.x → 0.8）

1. **0.8.0（已完成）**：错误通道 `T?` / `?` / `or` / `name!` 实装 +
   标准库语义迁移；并清掉一批 C 层欠账（C1 / C2 / C7）与三个原生后端 bug。
   详细内容见 `CHANGELOG.md` 的 0.8.0 章节。
2. **0.9.2（已完成 C 后端）**：ARC（A1）——堆对象引用计数头（`refs` +
   `on_zero`）、字符串字面量改为静态不可变对象、C 后端在局部变量 / 赋值 /
   返回 / 数组增删改 / struct 字段 / 嵌套拼接处插入 retain/release，
   以 `--arc` 实验开关提供。原生后端 free list 与默认开启留到 0.9.3；
   高阶函数 / `fn` 类型（B6）拍板不进 1.0。
3. **1.0.0**：冻结。内容即 `docs/stability.md`；若 A1 / B6 仍未落地，
   则写入“不包含”清单。

> **决策记录**：`建议.txt` 的决策树指出**不要把 A1（ARC）与 A3（错误通道）
> 放进同一个版本**（两者都触碰所有值传递路径，一次洗两次大澡，出问题时无法
> 归因）。0.8 选择先做 A3；A1 与 B6 顺延。

## 5. 0.7.x 欠账清单（未关闭）

以下条目在 0.7.0 中**未完成**，按优先级排列，全部应在 1.0 前关闭：

| 编号 | 内容 | 备注 |
| --- | --- | --- |
| A1 | ARC / 引用计数 | **0.9.2 已完成 C 后端（`--arc`）；原生后端 0.9.3** |
| A3 | 错误通道 `T?` / `?` / `or` | **已完成**（0.8.0） |
| B3 | `f([])` 空数组实参 hint 修复 | **已完成**（0.8.0，checkCall 重构） |
| B5 | 全局可变变量 `static let` | **不做**，已写入 stability.md “不包含” |
| C4 | 数组字面量去 GCC 语句表达式 | **未做**，已写入 stability.md “不包含” |
| C5 | aarch64 原生数学函数末位对齐 | **未做**（差分测试已记录差异；大数格式化 bug 已修） |
| C6 | Arm64 未识别操作码映射改编译期报错 | 未做 |
| C7 | `patchArm64SyscallNumbers` 防脆弱 | **已完成**（0.8.0） |
| B6 | 高阶函数 / `fn` 类型 | **不进 1.0**，留到 1.x |
| D4 | CI + 双后端差分矩阵 | **已完成**（`.github/workflows/ci.yml`） |
| D5 | Fuzzing + ASan | **已完成**基础版（`tests/fuzz_lexparse.sh` + CI ASan job） |
| D6 | bench 双后端数字写进 README | 未做 |

## 6. 明确不做

- `map<K,V>`：需要新的运行时表示 + 打印器 + 方法族，放到 1.x。
- 泛型：`sort` 先用 `fn` 类型 + 手工实例化。
- 并发：ARC 与并发不同时上，避免两个雷一起踩。
