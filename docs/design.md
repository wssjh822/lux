# Lux 编译器设计与扩展指南

本文面向想继续扩展 Lux 的人，说明编译器各阶段的职责、关键数据结构，
以及"加一个新语法特性"具体要改哪些地方。完整文法见 [grammar.md](grammar.md)。

---

## 1. 整体流水线

```
 源码(.lux)
    │
    ▼  Lexer      lexer.cpp      字符流 → Token 流（错误恢复，跳过坏字符）
  Token 流
    │
    ▼  Parser     parser.cpp     Token 流 → AST（递归下降 + 错误恢复）
  AST
    │
    ▼  Loader     loader.cpp     解析 import，递归合并多文件，登记模块归属
   合并的 AST
    │
    ▼  Sema       sema.cpp       符号解析 + 类型检查/推断 + 常量折叠
   带类型的 AST
    │
    ▼  Codegen    codegen.cpp    AST → C 源码（内联运行时）      ┐ 两条后端
  C 源码（C17）                     │  二选一
    │                               │ （--native 走
    ▼  Driver     main.cpp       调用 cc/clang/gcc → 可执行文件   │  native_body）
   可执行文件                                        ┘
```

（另有 `ty.cpp` 提供类型表、`repl.cpp` 提供交互式执行环境，见对应章节。）

### 1.1 原生代码生成后端（0.6 新增，--native）

`--native` 分支不经过 C：Sema 输出的带类型 AST 直接交给
`native_body.inc`（由 `native.cpp` include），逐节点发射**本机架构**指令，
最终由对应发射器的 CodeWriter 回填标签并写出单个 `PT_LOAD`(RWE) 段的
静态 ELF。后端按宿主架构在编译期选择发射器：

| 宿主 | 发射器 / ELF | 指令集 | 入口约定 |
| --- | --- | --- | --- |
| x86-64 | `native_emit.hpp` 的 `X64` / `EM_X86_64` | SSE2 + x87 | rax / xmm0，`syscall` |
| aarch64 | `native_emit_arm64.hpp` 的 `Arm64` / `EM_AARCH64` | 标量 FP/SIMD | x0 / d0，`svc #0` |

`native_body.inc` 只调用一组语义化方法（`push`/`pop`/`opRR`/`movrm`/
`floatCompare`/`rawSyscall`…），不直接拼接机器码字节，因此同一份代码生成
逻辑可同时服务两种架构；`native_emit.hpp` 在 `#if defined(__aarch64__)`
处切换。要点：

- **运行时即 Lux**：`native_rt.lux`（约 100 个 `luxrt_*` 函数）以 Lux
  语言写成、随用户程序一起编译进可执行文件。打印/转换/文件/环境变量/
  system/时间/随机数/字符串家族/bignum 浮点格式化全部由它们承担；
  编译器只为它们生成调用。
- **特权内建**：`__syscall` / `__bump_alloc` / `__peek64` / `__poke64` /
  `__mem_copy` 等在 Sema 免检、由 `evalIntrinsic` 内联发射。bump 分配器
  在 bss 固定槽（`kSlotHeap`）维护 cur/end 两个指针，首次 mmap 64 MiB。
- **无 libc**：程序入口是手工 ELF 入口，`native_rt.lux` 启动时从原始
  `rsp` 读 argv/envp 快照到私有堆，`system()` 用 fork+execve+wait4。
- **栈约定**：参数自左向右压栈（首参最深），返回值：整数/指针在 rax(x0)、
  浮点在 xmm0(d0)（以位模式压回数据栈）；调用方清参。`callRet` 统一处理。
- **跨架构的系统调用**：`native_rt.lux` 统一按 x86-64 书写直接系统调用号，
  arm64 编译运行时前由 `patchArm64SyscallNumbers` 改写为 aarch64 编号
  （read 63 / write 64 / exit_group 94 …）；`open`/`fork`/`unlink`/`rename`
  这类调用形态不同的，用 `__sys_open`/`__sys_fork`/`__sys_unlink`/
  `__sys_rename` 四个架构无关内建吸收（x86 走 open/fork/…，arm64 走
  openat/clone/unlinkat/renameat）。
- **aarch64 的 SP 对齐约束**：本机内核对 EL0 启用栈指针对齐检查，硬件 `sp`
  必须始终 16 字节对齐，无法承载 8 字节步进的值栈。因此 arm64 后端把值栈
  从硬件 `sp` 中分离出来：启动时 mmap 64 MiB，`x28` 作为值栈指针向下增长，
  硬件 `sp` 只用于 `stp/ldp` 保存 LR 与帧指针（恒定 16 对齐）。帧指针 `x29`
  指向值栈中首个实参，局部槽在其下方分配。这样 `[sp+8k]` 的槽布局与 x86
  完全一致，代码生成逻辑无需分支。
- **aarch64 的浮点数学**：arm64 无 x87，`sin`/`cos`/`log`/`exp`/`pow` 等
  改由运行时 Lux 函数 `luxrt_math_*` 实现（分解 + Taylor/atanh 级数），
  `sqrt`/`floor`/`ceil`/`trunc`/`round` 走 FSQRT/FRINT 指令。精度足以让
  `log10(1000.0)`、`log(exp(1.0))` 等与 libc 逐位一致。
- **发射纪律**（0.6 调试期沉淀的教训）：
  1. 任何"约定栈顶已是参数"的发射路径必须先 `evalExpr` 求值参数——
     `string()` / `__bump_alloc` 都曾因此用栈残留值出错；
  2. `cmp`/`ucomisd` 与 `jcc`/`setcc` 之间不得插入 `add/sub rsp`
     等写标志位指令（`||`/`&&`、`isnan` 都栽过）；
  3. x87 序列以实测为准（qemu 下 FSCALE 实际是 `st0 = st0·2^st1`
     且不弹栈），跨平台文档描述与真机行为可能不同，勿凭记忆写序列；
  4. `cmp` 是有方向的（`lhs - rhs`），arm64 端 `opMR`/`opRM` 必须按
     内存是左/右操作数分别发射，否则有符号比较会整体翻车。
- **诊断对齐**：panic 文案与 C 后端逐字一致（同一批中文字符串常量），
  越界/断言/除零/切片越界全部有行号级报错。
- **差分对拍**：`make test` 把每个行为用例在 C 后端与原生后端各跑一遍，
  逐字节 diff 输出。两套独立的代码生成路径互相校验。

C 后端遇到 `__` 系内建则在生成的 C 流里插入 `#error`，让预处理阶段
直接报错。

设计原则：

- **单趟（one-pass）**：Sema 和 Codegen 各遍历一次 AST，不做多轮迭代。
- **AST 就地标注**：Sema 不生成新树，而是把解析结果写回节点字段
  （`Expr::ty`、`LetStmt::resolved`、`CallExpr::builtin` / `target`、
  `BinaryExpr::folded`），Codegen 直接读取。
- **AST 归 Parser 所有**：节点由 `Parser` 的对象池持有，节点之间用裸指针互引。
  多文件编译时每个文件一个 `Parser`，统一由 `ModuleLoader` 持有（`parsers`
  成员），`merged` 里的指针指向这些池中的节点 —— 因此 `ModuleLoader` 析构后
  AST 就失效了，Sema 和 Codegen 必须在 loader 的生命周期内完成。
- **错误策略**：词法、语法、语义、模块加载四阶段统一走 `Diags` 收集，
  每条诊断带错误码（`E0001` 词法 / `E0002` 语法 / `E0003` 语义 /
  `E0004` 模块，`W1001`~ 警告）。词法/语法阶段通过"记录 + 同步 token 流"
  实现错误恢复，一次编译尽量报全所有错误（错误数超预算才截断）。
- **多文件 #line**：`FuncDecl` / `GlobalConstDecl` 记录 `file` 字段，
  Codegen 在生成每个声明时切换 `curFile`，`#line` 指令总能指回正确的
  `.lux` 文件。

---

## 2. 各阶段详解

### 2.1 Lexer（lexer.cpp）

无状态的手写扫描器，核心是 `Lexer::run()`。几个容易踩坑的地方：

- **`0..10` 的切分**：扫到 `.` 时必须向前看一位，只有后面紧跟数字才算小数点。
  否则 `for i in 0..10` 会被切成 `0.` 和 `.10`。
- **块注释嵌套**：用 `depth` 计数器实现，`/* /* */ */` 是合法的。
- **数字后的非法字符**：`123abc` 直接报错，而不是切成两个 token。
- **`\uXXXX` 转义**：解码成码点后按 UTF-8 重新编码（`Lexer::appendUtf8`）。

### 2.2 Parser（parser.cpp）

递归下降。表达式用**优先级爬升**（precedence climbing）：每一层优先级一个函数，
从 `parseOr()` 一路降到 `parseUnary()` / `parsePrimary()`。

几个特殊处理：

- **赋值识别（0.7 重构）**：语句先解析一个左值表达式（`IDENT` +
  `[expr]` / `.member` 后缀链），若后面紧跟赋值运算符就构造
  `AssignStmt{target: Expr*, compound, compoundOp}`；否则当作表达式语句。
  左值可任意嵌套（`grid[i][j]`、`p.x`、`arr[i].x`），不再限制于"变量名
  + 单下标"。复合赋值保留独立的 `compound` 标志，让代码生成只在需要时
  读取旧值（左值子表达式只求值一次）。
- **常量折叠**：`-5` 在 `parseUnary` 里直接折叠成 `IntLitExpr(-5)`，
  更一般的折叠在 Sema 里做（见 2.5）。
- **类型转换**：`int(x)` 在 `parsePrimary` 里识别（类型关键字 + `(`），
  构造成 `callee == "int"` 的 `CallExpr`，交给 Sema 按内置函数处理。
- **`.` 语法（0.7）**：`IDENT` 后的 `.IDENT` 链有两种用途。后跟 `(` 时
  整体作为限定 callee（`math.pow` / `a.push`）；否则构建 `MemberExpr` 链，
  Sema 再决定它是模块常量（`math.pi`）还是 struct 字段（`p.x`）。
- **struct 字面量消歧**：`P { x: 1 }` 与 `while x { ... }` 的条件位置冲突。
  解析器用 `noStructLit_` 上下文标记：在 `if` / `while` / `for` 的条件表达式
  中禁止把 `IDENT {` 当 struct 字面量（Rust 的 `no_struct_literal` 同款）。
  即使不在条件位置，也只在 `{` 后是 `}` 或 `IDENT ':'` 时才按字面量解析。
- **错误恢复**：语法错误记录进 `Diags` 后抛内部异常 `ParseBail`，由
  `parseBlock`（语句级）与 `parseProgram`（顶层级）捕获，`syncStmt()` /
  `syncTopLevel()` 把 token 流同步到下一个安全边界后继续解析。
  同步逻辑保证每次至少前进一个 token，错误数超过 100 停止解析。
- **深度限制**：`parseExpr()` 维护嵌套深度计数，超过 256 层直接报错，
  防止恶意深嵌套把递归下降解析器压爆栈。

### 2.3 Loader（loader.cpp）

`ModuleLoader` 负责把 import 展开成一个合并的 `Program`：

- 每个源文件走一次 读取 → 词法 → 语法，得到一个 `Parser`（由 loader 持有）；
- `import "路径"` 的解析顺序：`c:库名`（记录 `-l` 参数）→ 标准库模块名
  （math / time / system / file / string，math 首次导入时合成 `pi` / `e`
  常量）→ 相对路径的 `.lux` 文件 → 包目录（`lux.json`，一个支持
  `\uXXXX` 转义的极简 JSON 读取器）→ `main.lux` / `lib.lux` → 最后回退到
  已安装包目录 `~/.lux/packages/`（见 pkgs.cpp 的 `luxc add / list / delete`）；
- 已加载文件按 realpath 规范化后去重（`loaded_`），循环 import 自然安全；
- 路径全部**相对 import 声明所在文件**解析，与进程工作目录无关。

**模块归属**：每个文件的全部声明打上模块名（文件名 / 包名 / 标准库名，
根文件为空串）。import 解析后登记进 `ModuleInfo`：

- 默认导入（`import "x"`）→ 放进 `flat` 集合，成员可裸名访问；
- 别名导入（`import "x" as y`）→ 只登记 `aliases[y] = x`，成员只能
  通过 `y.成员` 访问，不污染全局命名空间；
- 所有已导入模块都在 `imported` 集合里，Sema 据此做限定访问解析。

标准库模块并不存在于磁盘上 —— 它们的函数是 Sema/Codegen 里带"模块门禁"的
内置函数（`builtinModule()` 映射），import 只是登记模块名。

### 2.4 包管理（pkgs.cpp）

`luxc add / list / delete` 操作 `~/.lux/packages/`（可用 `LUX_HOME` 重定位）：

- `add` 目录包时读 `lux.json` 的 `"name"`（缺省用目录名），单文件包用
  文件名做包名；安装即递归拷贝（符号链接跳过，防链接成环），并给没有
  `lux.json` 的包补一份配置；
- 包名必须是合法路径分量；同名重复安装报错；
- `delete` 递归删除；`list` 读取各包 `lux.json` 汇总入口与文件数。

### 2.5 Sema（sema.cpp）

`Analyzer` 持有一个作用域栈（`std::vector<unordered_map<string, VarInfo>>`），
`lookup()` 从栈顶往下找，天然支持变量遮蔽。

`run()` 的顺序：

1. **全局常量**：类型检查 → 常量折叠 → 校验初始值是常量表达式
   （字面量 / 字面量间的运算 / 引用已声明的全局常量）→ 声明进最外层作用域
   （别名导入模块的常量只登记进限定名表，不注入裸名）。
2. **收集函数签名**：根文件 + 默认导入模块的函数进裸名表，全部函数进
   `模块名\x01函数名` 限定表；检查重名、与内置函数重名。这步先做，所以支持
   相互递归和"调用后面才定义的函数"（跨文件同理，因为 import 合并后
   就是一个程序）。`extern fn` 额外检查保留名单（`lx_` 前缀与 C 标准库
   符号），避免与运行时撞名。
3. **检查 `main`**：必须存在、无参数、返回 `void` 或 `int`、不能是 extern。
4. **逐个检查函数体**（extern fn 没有函数体，跳过）。

类型检查的核心是 `checkExpr()`（返回类型并写回 `e->ty`）和
`canCoerce()`（只允许 `int → float`）。

**常量折叠**：`checkBinary()` 算出结果类型后，若两个操作数是字面量
（或引用了已折叠的全局常量），就把算术 / 位运算 / 字符串拼接的结果折成
字面量写回 `BinaryExpr::folded`。用 `__int128` 检测整数溢出、跳过除零与
`nan/inf`（它们没有合法的 C 字面量），溢出时保留运行时行为。折叠的成果：
`const X = 2 * 1024 * 1024;` 直接变成 C 的 `static const int64_t`。

**模块解析**：`checkCall()` / `checkExpr()` 遇到含 `.` 的限定名时先查别名表
再当模块名处理 —— 内置函数按 `builtinModule()` 匹配模块，用户函数 /
常量按声明时的 `module` 归属匹配；裸名只见根文件 + `flat` 集合里的模块。

**未使用变量警告**：`lookup()` 顺手把 `VarInfo::used` 置位，作用域弹出时
对从未使用的变量/参数发 `W1001`（编译器内部生成的 `__` 前缀变量跳过）。

**抑制级联报错**：任何操作数类型为 `TyKind::Invalid` 时直接返回 invalid
而不再报错。否则一个拼写错误会连带产生十几条无意义的诊断。

### 2.6 Codegen（codegen.cpp）

生成的 C 文件是完全自包含的：头部内联一段运行时（`kRuntime`，约 200 行 C），
然后是 extern fn 原型、全局常量、函数前置声明、函数定义。

几个关键决策：

| 决策 | 原因 |
| --- | --- |
| 类型直接映射（`int` → `int64_t`） | 零成本抽象，没有装箱和运行时类型标签 |
| 用户标识符加 `lx_` 前缀，模块成员再加模块名前缀（`lx_模块_名字`） | 避免与 C 关键字、标准库符号冲突；不同模块的同名成员不打架（`extern fn` 除外，直接引用 C 符号） |
| 插入 `#line` 指令 | C 编译器的警告和运行时断言能指回 `.lux` 的行号 |
| `#line` 按声明切换 `curFile` | 多文件编译时每个函数都能指回自己所在的源文件 |
| 整数除法走 `lx_idiv()` | 除零给出可读的错误，而不是 SIGFPE 崩溃 |
| `for` 的终点存进临时变量 | 终点表达式只求值一次，语义符合直觉 |
| `print` 按参数静态类型分派 | 编译期就决定调 `lx_print_i64` 还是 `lx_print_str`，无运行期类型判断 |
| 生成 C 按 `-std=c17` 编译 | 较新的 C 标准，所有现代 cc/clang/gcc 都支持 |
| 运行时开头 `#define _POSIX_C_SOURCE` + `_DEFAULT_SOURCE` | 否则严格模式下 `clock_gettime` / `nanosleep` / `random` 不可见 |
| 整数转字符串用手写 itoa | 比 `snprintf` 快 3~5 倍，是打印热路径 |
| 浮点打印走 `lx_f64_shortest` | `%.15g` → `%.17g` 取第一个能精确 round-trip 的，保证 `println(0.1+0.2)` 与交互一致 |
| 数组运行时全部内联在 kRuntime | 生成的 C 自包含，无外部依赖（0.5） |
| 数组字面量使用 GCC/Clang **语句表达式扩展** `({...})` | 非 C17 标准：`-pedantic` 下有警告，MSVC 拒绝。`--emit-c` 的产物因此面向 gcc/clang；去扩展化（外围块预声明临时变量）排在切片改造时一起做（0.6） |
| `lx_read_line` 单行上限 1 MiB | 防止管道灌入超大行把内存吃光 |
| `input()` 前 `fflush(stdout)` | 交互提示语与 `print` 的输出顺序正确 |

**`print` 的特殊性**：它是唯一会展开成多条 C 语句的"表达式"
（每个参数一条 `lx_print_*` 调用）。Sema 保证它的类型是 `void`，
从而不可能出现在需要值的位置；Codegen 只在 `ExprStmt` 分支里特判展开。

**extern fn**：生成 `extern 类型 原名(参数类型);` 原型，不做 `lx_` 改写，
调用点直接引用原符号名。链接依赖由 `import "c:库名"` 提供（driver 加 `-l`），
额外的 cc 选项可用 `-C "..."` 透传。

**format**：`format("x={}", a)` 在 Codegen 里把每个参数按静态类型转成字符串
（复用 `lx_i64_to_str` 等），调 `lx_str_format_concat(模板, 参数..., NULL)`
拼接 —— 运行时不需要可变参数类型派发，占位符数量在 Sema 期就校验过。

**语法糖大部分在 Parser 里展开**：`++`/`--`、`repeat`、`elif` 在解析
阶段脱糖成普通 AST 节点（`AssignStmt` / `ForStmt` / `IfStmt`），Sema 和
Codegen 完全不知道这些糖的存在。复合赋值是例外：它保留 `compound` 标志，
因为 `a[i] += v` / `p.x += v` 必须保证左值只求值一次，不能展开成
`a[i] = a[i] + v`（下标 / 成员子表达式会被求值两次）。

### 2.7 数组运行时（0.5）

**表示**：数组头堆分配，`lx_arr` 是指针——这是引用语义的根基：

```c
struct lx_arr {
    int64_t len, cap;
    void* data;                          // 元素连续存储
    int64_t esz;                         // 单元素字节数（嵌套数组存指针，8）
    void (*pelem)(lx_sb*, const void*);  // 元素打印器（打印/转字符串用）
};
typedef struct lx_arr* lx_arr;
```

要点：

- **赋值 / 传参 / 返回拷贝的是指针**，多个变量共享同一个 len/cap/data。
  第一版曾把头按值传递，函数内 `push` 改的是调用方看不到的 len 副本，
  引用语义被破坏（`arrays` 测试抓了出来），随后改为堆头指针。
- 每种元素类型宏展开一套 `lx_arr_{push,get,set,pop,insert,remove}_<T>`
  （i64 / f64 / bool / str / arr 五种），get/set 带越界检查，越界走
  `lx_arr_bounds_fail` panic（信息带下标与长度）。
- **pelem 函数指针**解决"数组怎么打印"：`lx_arr_sb` 递归渲染 `[e1, e2, ...]`，
  嵌套数组靠元素的 `lx_pe_arr` 自然递归，不需要运行期类型信息。
- **数组字面量**编译为 GCC 语句表达式：`({ lx_arr v = lx_arr_new(...);
  lx_arr_push_i64(v, e1); ... v; })`，可以安全地出现在任何需要值的位置。
  空数组只剩 `lx_arr_new`，元素类型完全由 Sema 的 hint 通道确定。
- **for-in** 编译为外层块 + 迭代临时变量 + **长度快照**（进入循环时把
  `len` 读进临时变量），循环内 push/pop 不会把遍历变成死循环；
  字符串迭代用 `lx_str_char_at` 逐字节产出单字符 string。
- **Sema 侧**：数组方法（`push` 等）不走内置函数表（避免和用户函数名
  冲突），而是在 `checkCall` 解析限定名时优先尝试 `findVar` → 数组类型 →
  `checkArrayMethod` 专用通道，接收者记录在 `CallExpr::methodRecv`。
  下标赋值的元素类型记录在 `AssignStmt::elemTy` 供 Codegen 分派
  `lx_arr_set_<T>`。
- **空数组推断**：`checkExpr` 带可选 `hint` 参数，Let 标注 / 赋值目标 /
  return 类型会把期望类型传下去；`[]` 没有 hint 时报错并提示写显式标注。
  直接作为函数实参（`f([])`）暂不支持 hint（需要重构 checkCall 的
  两趟求值），已在 README 已知限制中说明。

### 2.8 struct（0.7）

**表示**：struct 是**堆上的字段对象（引用语义，与数组一致）**，槽布局 = N×8，
每个字段占一个 8 字节槽（int/float/bool/string/数组/嵌套 struct 都是 8 字节）：

- C 后端：生成 `typedef struct lx_st_Point { int64_t f_x; ... } lx_st_Point;`，
  表达式层是 `lx_st_Point*`；构造用语句表达式 `({ lx_st_Point* p =
  (lx_st_Point*)lx_struct_new(sizeof(lx_st_Point)); p->f_x = ...; p; })`。
- 原生后端：`luxrt_struct_new(n)` 用 `__bump_alloc` 拿 n 字节（零初始化），
  字段访问 = `[ptr + idx*8]`，`MemberExpr::fieldIndex` 由 Sema 填入。

关键路径：

- **Sema 先收集后解析**：`run()` 第一步把所有 `StructDecl` 登记进 `structs`
  表（全局唯一），再校验字段类型（允许互相引用 / 嵌套）；struct 字面量
  校验字段是否存在 / 重复 / 缺失，以及初始化值能否相容。
- **`.` 的双重语义**：`checkExpr` 的 `Member` 分支先尝试模块限定常量，
  再当 struct 字段解析（写回 `fieldIndex`）。
- **打印**：C 后端为每个 struct 生成 `lx_st_X_pelem` / `lx_st_X_to_str`
  （与数组的 `pelem` 机制一致），struct 数组直接复用 `lx_arr` 的
  `esz=8` + 元素打印器；原生后端无法调用运行时的类型信息，`structToStr`
  按字段偏移现场拼字符串（含嵌套 struct 与 `luxrt_quote` 引号转义）。
- **栈帧预扫描**：原生后端 `countSlots` / `countExprSlots` 必须把
  struct / struct 数组打印时 `declareVar` 的临时槽计入，否则槽会落到
  预留区之下（与 0.6 的 `__fmt` 同类错误）。

---

### 2.9 错误通道（0.8）

**类型**：`TyKind::Optional`，复用 `elem` 字段（`T?` 的 `elem` = `T`），
`TyStore::optionalOf` intern 化后指针相等依然成立。`tyName` 输出 `int?`。

**表示**：`T?` 在**两个后端**都统一为“指向堆槽的指针，NULL = none”：

- C 后端：`cType(T?) = cType(T) + "*"`；装箱用语句表达式
  `({ T* o = (T*)lx_opt_alloc(sizeof(T)); *o = v; o; })`，解包 `(*o)`。
- 原生后端：与所有值一样是 8 字节槽；装箱调 `luxrt_opt_box(v)`，解包 `[o]`。

选择指针而非双字 `{ok,val}` 的原因：原生后端所有表达式都是单 8 字节槽，
多字返回值会牵动调用约定 / `countSlots` / 所有值路径。

**三种解包**：

| 形式 | C 后端 | 原生后端 |
| --- | --- | --- |
| `expr?` | `({ T* o = expr; if (!o) return 0; (*o); })` | `test rax; jne ok; xor rax,rax; frameLeave; ok: mov rax,[rax]` |
| `lhs or rhs` | `({ T* o = lhs; o ? (*o) : (rhs); })` | 分支 + 解包 + 惰性求值 rhs |
| `f!(...)` | `({ T* o = f(...); if (!o) lx_panic_opt("f"); (*o); })` | `test rax; ... luxrt_panic_opt` |

`?` 的早返回用 `return 0;` / `frameLeave` 实现，语义上就是“从当前函数返回
`none`”；`frameLeave` 会重置栈指针，所以表达式里尚未清理的临时值会被一并丢弃。

**装箱发生在隐式转换点**：`coerceTo`（C）/ `evalCoerced`（原生）在
`let` 初始化、赋值、`return`、传参、struct 字段、数组元素、`if` 分支处统一处理
`int→float` 与 `T→T?`。

**打印**：`toStrOf`（C）/ `optToStrTop`（原生）把可选值渲染为 `some(x)` / `none`；
其中 `string` 元素不加引号（与普通 `string()` 一致，区别于 struct 字段）。

**为什么不用 `named("Result")`**：可选类型是语言原语而非库类型，放进 TyKind
后 intern 化 / 比较 / 诊断都自然延续，无需引入新的 struct 机制。

### 2.9.1 函数值 `fn` 类型（1.1，补记）

`TyKind::Fn` 的 `elem` 存返回类型、`members` 存参数类型，由
`TyStore::fnOf` intern。函数值在**两个后端**都只是一个 8 字节代码地址：

- **C 后端**：`cType(fn(T)->R)` 生成 `typedef R (*fn_i)(T);`（在
  `fnTdAnchor` 处集中插入），具名函数取值就是函数名，间接调用直接
  `f(a, b)`；不产生堆对象，不碰 ARC。
- **原生后端**：取值即函数代码地址（`mov rax, <symLabel>`），间接调用经
  特权内建 `__call1` / `__call2` 翻译为 `call r/m`（x86-64）/ `blr`（aarch64）。
  参数仍按栈约定求值后调用，故 `__callN` 必须按 N 调整栈。
- `extern fn` 不能作值（原生后端没有 C 符号）；无 lambda / 闭包。

`sort` / `map` / `filter` / `map_opt` 均在**调用点展开**（C 后端直接生成
循环，原生后端调 `luxrt_sort` / `luxrt_map` / `luxrt_filter` /
`luxrt_map_opt`，函数以代码地址传入）。详见 `docs/fn.md`。

### 2.10 net 模块（1.2）

网络是 1.2 唯一的主题，取「stdlib `net` + 纯 Lux `httpx` 包」双轨。

**编译期**：`Builtin` 加 7 个 `Net*`；`sema.cpp` 的 `builtinTable()` /
`builtinModule()`（返回 `"net"`）/ `checkBuiltinCall()` 登记参数与返回类型；
`loader.cpp` 的标准库名单加 `"net"`。**零新语法、零新 AST 节点**。

**C 后端**：`kRuntime` 里补 `lx_net_*`（libc socket 家族）+ C 版 DNS-lite，
`callExpr` 加 7 个发射分支。`T?` 返回值统一走 `lx_opt_alloc` 装箱。

**原生后端**：

- 特权内建 `__sys_socket` / `__sys_connect` / `__sys_sendto` /
  `__sys_recvfrom` / `__sys_bind` / `__sys_listen` / `__sys_accept` /
  `__sys_setsockopt`，以及 `__poke16` / `__poke32`（写 `sockaddr_in` 的
  2/4 字节字段）。参数计数 / void 判定在 `sema.cpp` 的特权内建表登记。
- 发射器新增 `sysSocket()` … `sysSetsockopt()` 与 `store16Reg` / `store32Reg`
  （x86-64 `mov word/dword ptr`，aarch64 `strh` / `str`）。**架构差异在发射器
  吸收**（与 `__sys_open → openat` 同款）：`accept` 在 aarch64 走
  `accept4(fd, addr, len, 0)`（`__NR_accept4 = 202`），其余 syscall 号按架构给出。
- `native_rt.lux` 补 `luxrt_sockaddr_in` / DNS-lite（`__peek8u` 解析 UDP 应答、
  CNAME 最多 8 跳）与 7 个 `luxrt_net_*`；`native_rt_embed.h` 构建期重生成。

**内存**：socket fd 是裸 int，不进 pending、不 retain / release。

**行为对齐**：靠 `tests/run_tests.sh` 的网络差分 —— C/Native × client/server
对角矩阵 + `set_timeout` 超时文案逐字节一致。详见 `docs/net.md`。

**性能（1.2 顺带）**：原生后端新增 `fnMayPend` 分析，对**函数体不会创建堆对象、
且只调用同类函数**的纯计算函数关闭 ARC 插桩（省掉每条语句的
`luxrt_pend_flush_to`）。ARC 只影响内存回收，不影响可观察行为；素数基准因此
从 4.10s 降到 1.09s（等同 `--no-arc`，约为手写 C 的 3.1×）。

---

## 3. 加一个新语法特性：清单

以"加入 `unless cond {}`（if 的反义）"为例，需要改动 4 处：

1. **`lexer.cpp`**：把 `unless` 加进关键字表（得到 `Tok::KwUnless`）。
2. **`lux.hpp`**：加 `Tok::KwUnless`；如果要新增语句类型，加 `StmtKind` 和节点结构体。
3. **`parser.cpp`**：在 `parseStmt()` 里解析 `unless expr block`。
4. **`sema.cpp`**：在 `checkStmt()` 里加分支做类型检查；**`codegen.cpp`** 在
   `stmt()` 里加分支输出 C 代码。

最后补测试：`tests/cases/` 加一个用例 + `.expected`，
如果涉及新错误再加 `tests/errors/`。

## 4. 加一个内置函数：清单

以 `max(a, b)` 为例：

1. **`lux.hpp`**：在 `enum class Builtin` 里加 `Max`。
2. **`sema.cpp`**：
   - 在 `builtinTable()` 里登记名字映射 `{"max", Builtin::Max}`；
   - 如果属于标准库模块，在 `builtinModule()` 里登记模块名
     （例如 `case Builtin::Max: return "math";`），否则返回 `nullptr`；
   - 在 `checkBuiltinCall()` 里加分支，校验参数个数与类型，并设置返回类型。
3. **`codegen.cpp`**：在 `callExpr()` 里加分支，生成对应的 C 代码。
   如果运行时尚缺这个函数，在 `kRuntime` 字符串里补上。

注意内置函数**优先于**用户函数解析，且不允许用户定义同名函数
（Sema 会报"与内置函数重名"），避免语义歧义。

## 5. 加一个语法糖：清单

语法糖尽量在 **Parser 里脱糖**，避免 Sema / Codegen 为此新增逻辑。
以"加入 `x += 1` 复合赋值"为例：

1. **`lexer.cpp`**：在运算符表里加 `+=` 的识别（注意要先于单独的 `+` 匹配）。
2. **`lux.hpp`**：加 `Tok::PlusEq`。
3. **`parser.cpp`**：在 `parseStmt()` 里识别 `IDENT '+=' expr ';'`，
   构造成 `AssignStmt(x, BinaryExpr(Add, Ident(x), expr))`。
4. 语义检查与代码生成自动复用赋值 / 二元的既有逻辑，无需改动。

最后补测试：`tests/cases/` 加一个用例 + `.expected`，
如果涉及新错误再加 `tests/errors/`。

---

## 6. 类型表示演化计划

类型是编译器各阶段共享的最核心数据结构，本版本已完成**结构体化**改造，
后续数组 / struct / 错误通道都挂在这个骨架上：

```cpp
enum class TyKind { Invalid, Int, Float, Bool, String, Void,
                    Fn, Array, Tuple, Named, Optional };

struct Ty {
    TyKind kind;
    const Ty* elem;               // Array 的元素类型；Fn 的返回类型
    std::vector<const Ty*> members;  // Tuple 成员；Fn 参数
    std::string name;             // Named 的名称
};
```

所有类型由 `TyStore`（ty.cpp）intern 化：结构等价的类型全局唯一，
**指针相等 == 类型相等**，比较逻辑零成本且无需再改。各阶段用
`TyStore::int64Ty()` 等工厂取基础类型，复合类型用
`arrayOf(elem)` / `fnOf(ret, params)` / `tupleOf(...)` / `named(...)`。

落地路线（照此顺序改，避免边写边改）：

1. **数组/切片**：✅ **0.5 / 0.6 已落地**——数组语法 `[1,2,3]` / `a[i]` /
   `a[i]=v` / `len(a)` / 方法调用，类型走 `arrayOf(elem)`，运行时 `lx_arr`
   堆头指针（0.9.2 起带引用计数头，**0.9.4 起 ARC 默认回收**）；切片
   `a[lo..hi]` / `a[lo..=hi]` 已双后端一致（复制语义）。**剩余部分**：
   `map` / `filter` / `sort` 等高阶方法（1.x）。
2. **struct + 成员访问**：✅ **0.7 已落地**（见 2.8）——类型 `named("Point")`，
   `.` 成员、构造、嵌套、struct 数组、打印、双后端差分均通过。
3. **错误通道**：✅ **0.8 已落地**（见 2.9）——类型走新增的 `TyKind::Optional`
   （复用 `elem` 字段）而非 `tupleOf` / `Result`，`T?` / `?` 传播 / `or` 兜底 /
   `name!` panic 均已双后端一致；`read()` / `int("abc")` 已迁移到统一失败语义。
4. **高阶函数 / 泛型**：类型走 `fnOf(ret, params)`；`sort` / `map` / `filter`
   都靠它。**1.1 已落地 `fn` 类型与三件套**（见 2.9.1）；泛型仍留 1.x。

## 7. 诊断信息

`Diags::flush()` 按 rustc 的风格渲染，每条带**错误码**（便于检索与 IDE 集成）：

```
error[E0003]: 变量 'x' 的初始值类型不匹配：这里是 'string' 类型，但需要 'int' 类型（...）
  --> examples/t.lux:2:18
   |
 2 |     let x: int = "hello";
   |                  ^
```

错误码约定：`E0001` 词法、`E0002` 语法、`E0003` 语义、`E0004` 模块/IO、
`E0005` struct 函数返回落穿（0.9.0 新增）；
`W1001` 未使用变量/参数、`W1002` 表达式结果未使用、`W1003` 函数末尾缺
return、`W1004` extern fn 与运行时符号冲突。新增诊断先在
`DiagCode`（lux.hpp）里登记。

指示符的缩进按**终端显示宽度**计算（`displayWidth()`，CJK 字符算 2 列），
所以中文代码里的 `^` 也能对准。

彩色输出只在 stderr 是终端时启用，可用 `--no-color` 强制关闭。

## 8. 测试

```bash
make test
```

五层：

| 层 | 位置 | 校验内容 |
| --- | --- | --- |
| 行为测试 | `tests/cases/*.lux` | 编译运行后与 `.expected` 逐行比对（含 `arrays` / `for_in` / `string_ops` / `return_paths` / `main_argv` / `math_edges` / `arc_stress`） |
| 诊断测试 | `tests/errors/*.lux` | `.err`：必须编译失败且诊断含关键字；`.warn`：必须编译成功且输出含警告关键字 |
| 原生后端差分 | `run_tests.sh` 第 2.5 节 | 每个行为用例再跑一遇 `--native`（0.9.4 起含 ARC），与 C 后端输出逐字节对比 |
| ARC | `run_tests.sh` 第 2.6 节 | 每个行为用例再跑一遇 `--arc`（MALLOC_CHECK_ 抽查）+ C 后端 1e6 次 / 原生后端 20 万次 churn 内存回归；另验证默认开启与 `--no-arc` |
| 冒烟测试 | `examples/*.lux` | 必须能顺利编译通过 |
| 包管理 | `run_tests.sh` 后续节 | add / 重复 add 报错 / import / delete / 在线注册表 / PHP 账号链路 |
| REPL | `run_tests.sh` 第 3 节 | 管道模式逐行求值与错误诊断 |

新增用例时，先写好 `.lux` 并**人工核对一遍输出**，再生成 `.expected`，
别直接用程序输出当基线 —— 否则 bug 会被固化进基线里。

## 9. REPL（repl.cpp）

`luxc repl` 在**当前目录**写 `.lux_repl.lux` 临时文件后走完整编译流水线
（所以相对 `import` 与工作目录一致），执行产物放系统临时目录：

- 每行输入先按词法分类：`let/const/if/while/...` 与赋值是**语句**
  （不打印、累积进会话）；其余按**表达式**处理（包一层 `println` 求值）；
- `import` 与 `fn` 定义进会话前导；`let`/赋值逐行累积、每次求值时重放，
  所以跨行状态天然存活；
- 行编辑在终端原始模式下逐字节读入：方向键（`ESC [ A/B/C/D/H/F`）、退格、
  `~` 删除、TAB 补全（关键字 + 内置函数 + 会话名字，公共前缀 / 候选列表）；
- 关键坑：pty 默认开 `ONLCR`，`\r` 会被翻译成换行 —— 重绘一律用
  `ESC [ n D` 回退列而不是 `\r`；`trim()` 必须剥掉 `\n`/`\r`，
  否则提交的行尾会混进换行符。

## 10. 已知的后续工作

按重要性排列，详见 README 的路线图：

1. **错误通道**：✅ **0.8 落地 `T?`，0.9.4 补齐余项**（见 2.9）——
   `find_opt(s, sub): int?` 可选变体 + `last_error(): string` 错误消息携带
   （`T?` 仍只携带 ok/值，但失败原因可从全局 `last_error()` 读到）。
2. **自动内存管理（ARC）**：✅ **已全部落地**（见 2.7 与
   `docs/stability.md` §6.1）——堆对象加 `{refs, on_zero}` 头、字符串
   字面量改为静态不可变对象、Codegen 在局部变量作用域 / 赋值 / 返回 /
   数组增删改 / struct 字段 / 嵌套拼接处插入 retain/release（0.9.2 C 后端）；
   0.9.3 修复折叠字符串常量的无头指针缺陷并回收打印临时串；
   **0.9.4 原生后端按 `docs/arc.md` §2.3 实现尺寸分级 free list + 延迟释放
   （pending）模型，`--arc --native` 解禁，ARC 默认开启并新增 `--no-arc`，
   `T?` 装箱槽参与回收**。剩余仅有 `docs/arc.md` §2.4 列出的「只泄漏、
   不误释放」缺口。
3. **高阶函数 / `fn` 类型**：`fn(T,...) -> R` 类型与 `sort` / `map` / `filter`；
   **1.1 已落地**（见 2.9.1），泛型留 1.x。
4. **原生后端**：`codegen.cpp` 目前是唯一依赖 C 编译器的环节。
   抽象出一个 `Backend` 接口后，可以并列实现 x86-64 汇编或 LLVM IR 后端。
5. **aarch64 数学末位对齐（C5）**：x86-64 侧 0.9.4 已把 `asin` / `acos`
   统一改为调用运行时库实现（旧 x87 序列的栈方向写反，`|x| < 1` 恒得 NaN）；
   aarch64 侧 `cos` / `tan` / `atan` / `atan2` 的末位对齐仍待有 aarch64
   机器的版本验证；
   大数浮点格式化（`bigE ≥ 0`）已在 0.8 修正。
6. **网络**：`net` + `httpx` 已在 1.2 落地（见 2.10）；TLS/HTTPS、IPv6、
   `connect` 阶段超时、chunked / keep-alive / gzip / 重定向均明确不做，
   留 1.3 候选。
