# Lux 变更日志

本文件记录各版本的改动。0.1 → 0.2 → 0.3 的内容从 git 历史整理而来，
0.4 起每个版本都会在这里登记。

## 0.7.0

**语言完整版**。新增 `struct` 结构体、`if` 表达式、原始字符串与 `nan`/`inf`
字面量、`main(argv)`；解锁嵌套 / 成员复合赋值；双后端（C × 原生）逐字节一致。
本版仍**未**包含 ARC 与错误通道——两者是 0.7.x / 0.8 的主题，设计草案已入库
（见 `docs/arc.md`、`docs/stability.md`）。

### 语言：struct（A2）

- 声明：`struct Point { let x: int; let y: int; }`（字段前缀 `let` / `const` 可选，
  字段类型均可为 `int` / `float` / `bool` / `string` / 数组 / 另一个 struct）。
- 构造：`Point { x: 1, y: 2 }`（字段顺序任意、必须全部给出，缺字段 / 未知字段 /
  重复字段均在编译期报错）。
- 成员读写：`p.x`、`p.x = v`、`p.x += v`；嵌套 `l.a.x`、数组 `ps[0].x = v`。
- 表示层：struct 是**堆上的字段对象（引用语义，与数组一致）**，字段布局为 N×8；
  C 后端映射到 `typedef struct` 指针，原生后端用 `__bump_alloc` 分配 N×8 字节。
- 双后端支持直接 `print` / `println` / `string()` / `format("{}", p)`：
  打印成 `Point { x: 1, y: 2 }`（字符串字段带引号转义）。struct 数组也可整体打印。
- `const p = ...` 禁止修改 `p` 的字段；struct 不支持 `==` / `!=`。
- 模块系统：`struct` 随 `import` 一起并入（Loader 新增合并路径）；REPL 支持
  逐行声明 struct 并在后续行使用。

### 语言：if 表达式（B1）

- `let x = if c { a } else { b };`——两个分支各是一个表达式，必须有 `else`；
  分支类型需相容（`int` / `float` 混合提升为 `float`）。
- 解析层用 `noStructLit_` 上下文标记区分 `if x {`（条件）与 `x { ... }`
  （struct 字面量），与 Rust 的 `no_struct_literal` 同款处理。

### 语言：原始字符串与 nan / inf（B2）

- `r"..."`：原始字符串，内容原样保留（反斜杠不转义），遇到下一个 `"` 结束。
- `nan` / `inf`（以及 `-inf`）：浮点字面量；C 后端生成 `0.0/0.0` / `1.0/0.0`
  等价表达式，原生后端写 IEEE 位模式。

### 语言：main(argv)（A4）

- 可选入口签名 `fn main(argv: string[])`；`argv[0]` 为程序名（C 约定）。
- C 后端生成 `int main(int argc, char** argv)` 并用 `lx_argv_new` 构造 `string[]`；
  原生后端新增运行时函数 `luxrt_argv()`，在 `_start` 快照的初始栈上重建参数数组。

### 语言：通用左值赋值（A2 的前置改造 + B4）

- `AssignStmt` 由「变量名 + 可选单下标」重构为**单一左值表达式**
  （`Ident` / `Index` / `Member` 可任意嵌套），解析、Sema、双后端、REPL 同步。
- 解锁 `grid[0][1] = 5`（旧版直接报错「暂不支持」）与 `a[i] += v`、`p.x *= 2`、
  `arr[i].x += 1` 等复合赋值；复合赋值在代码生成层读取旧值再写回，
  **左值子表达式只求值一次**（数组元素用临时变量，成员 / 变量用 C 自身的
  `+=` 单次求值语义；原生后端用临时槽）。

### 正确性欠账核销

- **C1**：原生后端特权内建（`__peek64` 等）新增**编译期参数个数校验**，
  `__peek64()` 这类空参数不再导致编译器自身崩溃，而是报 `E0003`。
- **C2**：`W1006`（用户误用特权内建）对 `luxrt_` 函数体豁免，避免假警告。
- **C5（部分）**：新增 `math_edges` 差分探查，发现 aarch64 原生数学函数在
  溢出 / 下溢 / 极端指数附近与 libm 存在末位差异（例如 `exp(-745)`、
  `pow(1e300,2)`、`asin(0.99999)`）。这是 0.6 声称「逐位一致」时未覆盖的区域，
  已在 README「已知限制」中如实记录，校准列入 0.7.x。
- `struct` 接入后，`cType` / `zeroOf` / 数组元素打印器 / 原生 `countSlots`
  等路径全覆盖 struct，避免栈帧预扫描漏计导致的内存越界。

### 测试

- 官方回归扩至 **143 项**：新增 `struct`、`if_expr`、`raw_nan_inf`、
  `compound_assign`、`nested_index_assign` 行为用例（全部跑 C / 原生双后端
  逐字节差分），以及 struct 未知 / 缺失字段、`main` 参数类型、
  特权内建参数个数、const struct 字段等诊断用例。
- REPL 冒烟覆盖 struct 声明 / 字面量 / 成员赋值 / 复合赋值。

### 文档与站点

- README 新增 struct / if 表达式 / argv / 原始字符串章节，路线图改写为 1.0 导向。
- 新增 [docs/stability.md](docs/stability.md)：1.0 冻结范围与不包含清单。
- 新增 [docs/arc.md](docs/arc.md)：ARC / 错误通道设计草案与落地计划。
- `index.html` 项目主页同步 0.7 特性与新版图标。
- 图标 `logo.svg` 重绘：更克制的几何构成、分层渐变与暗色适配。

## 0.6.0

双后端版本：新增**原生代码生成后端**（x86-64 Linux ELF 直出，摆脱 C 编译器
与 libc）与**数组/字符串切片**语法，并以 26 项原生冒烟测试并入官方回归。

### aarch64 原生后端（0.6 增补）

- 原生后端新增 **AArch64 (arm64)** 发射器（`native_emit_arm64.hpp`），
  按宿主架构在编译期经 `#if defined(__aarch64__)` 选择。`luxc --native`
  在 arm64 设备上同样能无 C 编译器 / 无 libc 直出可执行文件。
- `native_body.inc` 全面去架构化：原先散落的裸 x86 字节全部收敛为语义方法
  （`push`/`opMR`/`floatCompare`/`rawSyscall`/`frameEnter`…），x86-64 与
  aarch64 各自实现同一套接口；行为用例在 C / 原生后端下逐字节对拍。
- **值栈与硬件 sp 分离**：arm64 内核对 EL0 启用 SP 对齐检查，硬件 `sp` 必须
  恒为 16 字节对齐。启动时 mmap 64 MiB 作值栈（`x28` 向下增长），硬件 `sp`
  只用 `stp/ldp` 保存 LR/帧指针；槽布局与 x86 逐字节对应。
- **系统调用跨架构**：运行时直接调用号按 x86-64 书写，编译 arm64 运行时前
  改写为 aarch64 编号；`open`/`fork`/`unlink`/`rename` 用架构无关的
  `__sys_open`/`__sys_fork`/`__sys_unlink`/`__sys_rename` 内建吸收。
- **浮点数学**：arm64 无 x87，`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/
  `log`/`log10`/`exp`/`pow`/`atan2`/`fmod`/`hypot` 改由运行时 Lux 函数
  `luxrt_math_*` 实现，`log10(1000.0)`、`log(exp(1.0))` 等与 C 后端逐位一致。

### 代码审查修复（0.6.0）

- `countSlots` 漏计 `format()` 的 `__fmt` 临时槽：3 个以上 `format` 调用会
  让槽落到 `rsp` 之下、被下一次 `call` 的返回地址覆盖（栈帧溢出）。
- 版本号单源化补全：`main.cpp` 的 `LUX_VERSION` 兜底值由 0.5.1 统一为 0.6.0。
- 全局常量支持引用另一个全局常量（`const B = A;`），Sema 与原生后端补分支，
  与 grammar.md 的 `constExpr := IDENT` 产生式对齐。
- 用户程序直接调用 `__` 系特权内建时发出 **W1006** 警告（运行时库编译时抑制）。
- `--native` 下显式传入 `-O` 会提示“暂不支持优化，已忽略”。
- 新增 `tests/cases/slices.lux`（四种端点 + `..=` + 字符串切片 + 复制语义）、
  `slices_oob_hi` / `slices_oob_order` 越界 panic 用例，以及
  `tests/errors/priv_intrinsic.warn`。
- `make test` 新增 **C 后端 ↔ 原生后端逐字节差分对拍**，两套独立代码生成路径
  互相校验。

### 原生代码生成后端（--native，0.6 主线）

- `luxc --native` 直接从 Lux 源码输出可运行的 x86-64 ELF（单 LOAD RWE 段，
  静态、零依赖），不再经过 gcc/clang，也不链接 libc。
- 运行时系统全部以 Lux 自身编写（`native_rt.lux`，编译为生成的机器码）：
  整数/浮点/布尔/数组/字符串的打印与转换、panic 与越界诊断、文件与目录、
  环境变量、system()、时间与睡眠、随机数、字符串家族（比较/查找/替换/
  大小写/裁剪/分割/拼接）、bignum 浮点格式化（%.17g 级别的 round-trip）
  等约 90 个运行时函数。
- 特权内建（`__` 前缀，仅原生后端可用）：`__syscall` / `__bump_alloc` /
  `__peek64` / `__peek8u` / `__poke64` / `__poke8` / `__mem_copy` /
  `__f_to_bits` / `__f_from` / `__i_to_f` / `__f_to_i` / `__rand_next` /
  `__seed_set` / `__sptr` / `__sval` / `__sval_a`。bump 分配器（首次 mmap
  64 MiB 后线性前进）内联进调用点，数组与字符串全部落在私有堆上。
- 诊断与 `-O` 无关：panic 信息与 C 后端逐字对齐（越界/断言/除零等）。
- C 后端遇到 `__` 系内建时在生成的 C 流里插入一行
  `#error Lux: __ 系内建仅原生后端支持 (xxx)`，gcc/clang 预处理阶段即
  报出清晰错误，而不是留到链接期才失败。

### 语言：数组与字符串切片（0.6 新语法）

- `a[lo..hi]`（半开区间）、`a[lo..=hi]`（含端点）；端点均可省略：
  `a[..hi]`、`a[lo..]`、`a[..]`。
- **复制语义**：切片返回新数组/新字符串，修改切片不影响原对象。
- 越界（`lo<0`、`hi>len`、`lo>hi`）一律运行时 panic（"切片范围越界"），
  与下标访问同风格。
- 双后端一致实现：C 后端生成 `lx_arr_slice` / `lx_str_slice`（新增运行时
  函数），原生后端生成对 `luxrt_arr_slice` / `luxrt_str_slice` 的调用。

### 原生后端的编译器正确性修复（gdb 逐指令定位）

- `string(x)` 漏了参数求值，`string(123)` 用栈残留值算出 "0"
  （与 0.5 的 bump_alloc 同源 bug）；顺带全量审计了所有隐式传参发射路径。
- 字符串 `==` / `!=` 把 `str_eq` 返回值用 `pop` 丢弃，实际比较的是栈上
  垃圾值，且破坏栈平衡。
- `||` / `&&` 的短路发射在 `cmp` 与 `jcc` 之间插了 `add rsp`，
  加法覆盖标志位导致 `||` 恒真（Z 标志被无条件清零）。
- `isnan` 的 `setcc` 之前插了 `add rsp`，PF 被覆盖，结果随栈地址奇偶
  摆动。
- `__bump_alloc` / `__mem_copy`（memmove 语义）：dst > src 的重叠搬运
  必须反向复制，否则头部插入类操作自我覆盖（浮点 big 格式化损坏的
  根因）；`__bump_alloc` 漏求值 size 参数。
- x87 数学函数重写并逐指令实证（qemu 下 gdb watchpoint 不可信，全部
  改用定点断点读栈 + C 内联汇编对照实验）：
  - `pow`/`exp` 的 fscale 序列按实测语义重写（st0 = st0·2^st1，不弹栈，
    st1 需为整数指数），修复 `pow(2,10)=1024` 被算成 81.92/10/20 等；
  - `min`/`max` 的 `cmp` 操作数方向写反，min/max 输出互换；
  - `memmove` 反向复制时置 DF 后恢复（ABI 要求返回时 DF=0）。
- `luxrt_system` 修复：execve 的 path/argv/envp 需要的是 C 字符串指针
  （Lux 串对象 + 8 跳过 len 头）与 envp 的 data 区指针，此前全传了
  对象头导致 exec 必败（恒返回 127）。

### 测试与工具

- 收尾期 examples 全量原生冒烟又抓出三处并修复：
  - `min` / `max`（int 路径）在结果写回栈槽后多了一次 `push`，净 +2 破坏
    "净 +1 结果槽"约定——单层调用侥幸不崩，嵌套在 `string(max(3,7))` 里
    直接段错误；
  - 环境变量改从 `_start` 快照的初始栈读取（SysV 布局 argc/argv/envp），
    不再依赖 `/proc/self/environ`（qemu 等环境下不可用，`env("HOME")`
    恒为空）；/proc 路径保留作回退；
  - `luxrt_build_envp` 返回的是裸指针数组，`luxrt_system` 却按"带 data
    指针头的对象"再解引用一层，env blob 为空时恰好读到 0（空环境合法）
    掩盖了错误，blob 有效后 execve 必败（system 恒返 127）。
- bench 双后端数据：C 后端与手写 C 同速（1.01×），原生后端约 2.05×
  （无优化直出机器码），快于 Python 9.7×。

- `tests/run_tests.sh` 新增"原生后端冒烟"段：对全部行为用例以 `--native`
  编译运行并与同一 `.expected` 比对（extern fn 用例自动跳过）；另加
  切片双后端专项。0.6 起官方回归 = 90 项（C 后端 62 + 原生 26 + 切片 2）。
- 双后端输出对拍：算术/控制流/浮点格式化/字符串/数组/切片综合程序
  逐行 diff 一致（含 `0.1 + 0.2` 位级一致）。

## 0.5.1

还债版：针对 0.5.0 代码审查清单的集中修复——测试重建、编译器正确性、
诊断质量三个方向，无新语言特性。

### 编译器正确性

- **无条件兜底 return**（重要）：Codegen 不再依赖 `stmtAlwaysReturns`
  省略函数末尾的兜底 return——正确性不再押在静态分析的完备性上，
  任何分析盲区最多多生成一条不可达 return（`-O2` 下零开销），而不会
  产生 UB。`stmtAlwaysReturns` 保留给 Sema 做缺失 return 警告/报错。
- DepthGuard 修复构造顺序：先计入深度再检查上限，异常时 RAII 析构
  正确回退计数，不会因提前 return 泄漏深度。
- `lx_random` 的分母从 `RAND_MAX` 改为 `((double)0x7FFFFFFF + 1.0)`：
  `RAND_MAX` 类型随平台变化，原先在部分平台上 random() 可能返回 1.0。
- Sema 删除 `sawReturn` 死字段（3 处），返回路径判断已完全由
  `stmtAlwaysReturns` 承担。
- 全局常量预注册函数签名：`const X = f(1)` 现在得到定向报错
  "初始值必须在编译期确定下来"，而不是误导性的"未定义的函数 'f'"；
  字面量折叠（`const KB = 2 * 1024`）行为不变，重复定义检测不受影响。
- `lx_panic` 输出前先 `fflush(stdout)`：panic 前已打印的内容不因
  块缓冲丢失，且与 stderr 的交错顺序确定。

### 诊断改进

- extern fn 的参数与返回类型不能是数组（E0003 定向报错）：`lx_arr`
  是 Lux 运行时的私有结构，C 侧没有对应类型。
- 嵌套数组的直接下标赋值（`grid[i][j] = v`）给出定向报错，提示先取出
  子数组再修改。
- 本地 `.lux` 文件与标准库模块同名时产生 W1005 警告（此前被静默忽略）。
- Loader 以规范化路径与展示路径双键登记源文件，防御路径别名导致的
  重复导入。
- 数组与字符串越界的运行时报错分两条路径（`lx_arr_bounds_fail` /
  `lx_str_bounds_fail`），提示语各自准确。
- 清理：删除无引用的 `loadPackage` 单参声明；`cType` 数组分支补注
  "lx_arr 即堆头指针，赋值/传参共享"；design.md 决策表登记"数组字面量
  使用 GCC/Clang 语句表达式扩展"（含 0.6 去扩展化计划）；grammar.md
  版本号同步。

### 测试（重点）

- `run_tests.sh` 重构为编译/运行两阶段：编译阶段 stderr 必须干净，
  运行阶段单独执行并收集输出——运行期 panic 是被测行为而不再是
  "编译失败"，且二进制统一写入临时目录，源码树不再有编译产物残留。
- 恢复/新增 7 个行为用例：`runtime_panic_divzero`（除零 panic 前的
  输出顺序确定性）、`runtime_panics`（INT64_MIN / -1 溢出，运行时构造
  避免常量折叠发出超范围 C 字面量）、`strings`、`sugar`（+= / ++ /
  -- / repeat / elif）、`vars`、`system_setenv`、`time_mono`。
- 新增 17 个诊断用例：重复定义、break 位置、未闭合字符串、参数个数、
  format 占位符、缺 main、未导入模块、return 类型、全局常量非字面量、
  if 条件非布尔、缺分号、未知函数/成员、非 ASCII 标识符、extern 数组、
  嵌套下标赋值、语法错误恢复等。
- 全套 61 项测试（行为 + 诊断 + REPL + 包管理）全绿。

## 0.5.0

数组与迭代：这门语言补上了"数据结构"这块最基础的拼图。

### 语言

- **数组**（本次的核心特性）：
  - 类型写法两种等价：前缀 `[int]` 与后缀 `int[]`，可嵌套（`int[][]` / `[[int]]`）；
  - 数组字面量 `[1, 2, 3]`（支持尾逗号），元素类型统一推断，int/float 混合
    提升为 `float[]`；空数组 `[]` 从类型标注 / 赋值目标 / return 类型推断元素类型；
  - 下标访问 `a[i]` 与下标赋值 `a[i] = v`（越界立即 panic，错误信息带上下标与长度）；
    字符串下标 `s[i]` 返回单字节字符；
  - 方法调用：`push` / `pop` / `insert` / `remove` / `clear`（const 数组禁止
    修改方法与下标赋值）；
  - **引用语义**：数组头（len/cap/data）堆分配，`lx_arr` 本身是指针——赋值 /
    传参 / 返回共享同一个数组，函数内 `push` 对调用方可见（与 Python list 一致）；
  - 嵌套数组递归打印（元素打印器 `pelem` 函数指针）；`len()` / `print` /
    `string()` 均支持数组；数组之间的 `==` 比较被禁止（引用语义下有歧义）；
  - 数组不能作为全局常量（引用类型无法编译期确定）。
- **for-in 迭代**：`for x in arr`（数组逐元素）与 `for c in s`（字符串逐字节
  单字符）；迭代使用进入循环时的长度快照，循环内 push/pop 不会死循环。
- `_` 开头的变量（如 `for _ in 0..n`）不触发未使用警告。

### 标准库

- `string.split(s, sep)`：按分隔符切分成 `string[]`；
- `string.chars(s)`：拆成单字节字符数组；
- `string.join(arr, sep)`：用分隔符拼接字符串数组。

### 编译器

- **最短往返浮点打印**：新 `lx_f64_shortest` 依次尝试 `%.15g` → `%.17g`，
  取第一个能精确 round-trip 的表示。修复了 0.4 中 `3.141592653589793` 被
  打印成 `3.14159265358979`、`sqrt(2)` 打印成 `1.4142135623731` 的精度截断问题。
- **完整返回路径分析**（`stmtAlwaysReturns`，Sema 与 Codegen 共用）：
  `return` / 块末尾 return / if-else 双分支都返回 / `while (true)` 或 `loop`
  且不含 break，均视为"必然返回"；替代 0.4 只检查最后一条语句的简化版。
- Sema：`checkExpr` 支持类型提示（hint），供空数组字面量在 let 标注 /
  赋值目标 / return 类型处推断元素类型；数组方法不进内置函数表（避免与
  用户函数名冲突，走 `CallExpr::methodRecv` 专用通道）。
- REPL：识别下标赋值语句形态；补全词表加入 `split` / `chars` / `join` /
  `push` / `pop` / `insert` / `remove` / `clear`。

### 测试

- 重建测试套件（0.4 的 zip 中缺失）：`tests/run_tests.sh` 一键跑 36 项——
  行为测试（`cases/*.lux` + `.expected` 逐行比对）、诊断测试（`errors/*.lux` +
  `.err` 必须编译失败 / `.warn` 必须产生警告）、REPL 冒烟、包管理全流程
  （add / 重复 add 报错 / import / delete），包安装目录经 `LUX_HOME` 隔离。
- 新增行为用例：`arrays` / `for_in` / `string_ops` / `return_paths`；
  新增诊断用例 12 个（空数组推断失败、const 数组修改、下标类型错误、
  字符串下标赋值、for-in 非可迭代对象、数组比较等）。
- 更新受浮点打印修复影响的 expected（`3.141592653589793` /
  `1.4142135623730951` / `1.2599210498948732`）。

### 其他

- 全新 logo：深空光轨 λ（青紫渐变光束 + 双轨道光子 + 细网格），README 与
  官网同步更新；版本号升至 0.5.0。

## 0.4.0

语言与编译器的第一次架构升级：类型系统结构体化、诊断统一、模块命名空间。

### 语言

- **模块命名空间与导入别名**：`import "x" as y` 只登记别名（不污染全局）；
  任何已导入模块支持 `模块名.成员` 限定访问（`math.pow` / `y.pow` / `模块.常量`）。
- **常量表达式求值**：`const X = 2 * 1024 * 1024;` 编译期折叠（算术 / 位运算 /
  字符串拼接，可引用之前声明的全局常量；`__int128` 溢出检测，除零与
  nan/inf 保留运行时行为）。
- **编译期检查 format 占位符**：`format("{} {}", 1)` 直接报错。
- 未使用变量 / 参数警告（W1001，`__` 前缀的内部变量跳过）。
- extern fn 保留名单检查（W1004）：`lx_` 前缀与 C 标准库符号撞名会警告。

### 编译器

- **类型表结构体化**（`ty.cpp`）：`Ty = { kind, elem, members, name }`，
  结构等价类型 intern 化，指针相等即类型相等；预留 `Fn` / `Array` /
  `Tuple` / `Named`，是数组、struct、错误通道的共同地基。
- **诊断双轨统一 + 错误码**：词法（E0001）/ 语法（E0002）/ 语义（E0003）/
  模块（E0004）全部经 `Diags` 收集，渲染格式 `error[E0003]: ...`；
  删除了旧的 `CompileError` 异常通道。
- **错误恢复**：词法跳过坏字符、语法同步到语句边界后继续解析，
  一次编译报全所有错误（100 条预算截断）；表达式嵌套深度限制 256 层。
- 生成 C 改按 `-std=c17` 编译；运行时补 `_DEFAULT_SOURCE`。
- 中间 `.c` 默认写系统临时目录（mkstemp），`--keep-c` 才留在可执行文件旁；
  新增 `-C <选项>` 透传 C 编译器选项。
- 路径规范化支持 `../` 折叠；包拷贝跳过符号链接（防链接成环）；
  lux.json 极简读取器支持 `\uXXXX`（含代理对）。
- 版本号单源化：`Makefile` 经 `-DLUX_VERSION` 注入，README/文档同步更新。

### 标准库与运行时

- 新模块 **string**：`contains` `startswith` `endswith` `find` `replace`
  `trim` `upper` `lower` `substr` `format`（split/join 等数组落地后补齐）。
- math 扩充：`atan2` `fmod` `hypot` `trunc` `isnan` `isinf` `random` `seed`。
- time 新增 `monotonic()`（CLOCK_MONOTONIC，测耗时用）。
- system 新增 `setenv(name, value)`；file 新增 `remove` / `rename`。
- 执行器优化：整数转字符串手写 itoa（比 snprintf 快 3~5 倍）；
  `lx_read_line` 单行 1 MiB 上限；`input()` 前 `fflush(stdout)`；
  panic 信息提示"可能是递归过深"。

### 工具链

- **交互式 REPL**（`luxc repl`）：行编辑（左右移动 / 退格 / Home / End）、
  历史（上下方向键，跨会话保存 `~/.lux/repl_history`）、**TAB 补全**
  （关键字 / 内置函数 / 会话名字）、多行 fn 定义自动续行、
  let/const/赋值跨行存活；管道模式下退化为逐行求值。
- bench 脚本改用 `date +%s%N` 计时，不再依赖 GNU 专属的 `/usr/bin/time`。
- **Lux 语言图标**（`logo.svg`：深色圆角方块 + 金色太阳 + λ 徽记），
  已接入 index.html favicon。

### 测试与文档

- 回归测试 48 → 69 项：新增 string/math/time/system/file 扩展、
  常量折叠、命名空间导入、format、运行时 panic（INT64_MIN/-1、除零）、
  错误恢复、模块门禁、整数字面量溢出、Unicode 标识符（ASCII-only 文档漂移
  修正）等用例，以及 REPL 管道模式与跨行会话状态两节。
- 文档同步：README / grammar.md / design.md（新增"类型表示演化计划"一节）
  / index.html 全部更新到 0.4。

## 0.3.0

- **import 模块系统**：标准库模块、`c:库名` 链接、相对路径 `.lux` 文件、
  包目录（lux.json）；循环 import 安全，多文件诊断。
- **标准库**：math（pi/e、pow/floor/sin/log/min/max 等）、time（now/sleep）、
  system（system/env）、file（read/write/append/exists）。
- **语法糖**：复合赋值 `+= -= ...`、语句级 `++/--`、`repeat n {}`、`elif`。
- **包管理**：`luxc add / list / delete`（`~/.lux/packages/`，LUX_HOME 可重定位）。
- `extern fn` 直接调用 C 函数；按文件切换 `#line`。

## 0.2.0

- 编译器骨架：Lexer → Parser → Sema → Codegen（C 后端）。
- 基础类型 int/float/bool/string/void，类型推断，唯一隐式转换 int→float。
- 控制流 if/else/while/for 区间/loop，递归与相互递归。
- 内置函数：print/println/len/input/int/float/string/abs/sqrt/assert/exit。
- rustc 风格诊断（源码上下文 + 彩色输出），三层回归测试。

## 0.1.0

- 最初的实验版本：能用 `fn main() { println("hello"); }` 编译出可执行文件。
