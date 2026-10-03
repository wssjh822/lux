# Lux 语言教程（面向人与 AI）

> 版本：Lux 1.2.0。目标读者是**第一次接触 Lux 的程序员**，以及**需要生成
> Lux 代码的 AI**。读完本文你应当能独立写出正确的 Lux 程序。
>
> 配套文档：
> - `docs/grammar.md`：完整 EBNF 文法（精确定义，查阅用）
> - `docs/stability.md`：1.0 稳定承诺与明确不包含的内容
> - `docs/design.md`：编译器设计
> - `README.md`：构建、命令行、标准库细节
>
> 关键定位：**Lux 是一门编译到本机可执行文件的静态类型脚本语言**——语法像
> Python/Go 的混合体，内存模型像 Python（数组与 struct 是引用语义），但所有
> 类型在编译期确定，内存由引用计数（ARC）自动回收（0.9.4 起默认开启）。
> 它是 **POSIX-only** 的，目标是 Linux/macOS。

---

## 0. 快速上手

### 0.1 安装与运行

```bash
make -j4                 # 构建，产出 build/luxc
./build/luxc hello.lux --run      # 编译并运行
./build/luxc hello.lux -o hello   # 只编译，产出 ./hello
./build/luxc repl                 # 交互式 REPL
```

一个最小程序：

```lux
// hello.lux —— 每个程序必须有 main
fn main() {
    println("你好，Lux！");
}
```

### 0.2 程序结构

一个 Lux 源文件由若干**顶层声明**组成：`import` / `extern fn` / `const` /
`struct` / `fn`。函数与语句的顺序不重要（同文件内可互相引用、递归），
但 `const` 与 `struct` 用到的名字必须能被解析。

```lux
import "math";                 // 顶层：启用标准库模块
const LIMIT: int = 10;         // 顶层常量（编译期折叠）
struct Point { x: int; y: int; }

fn main() {
    let p = Point { x: 1, y: 2 };
    println(p.x + p.y + LIMIT);
}
```

### 0.3 入口 `main`

- `fn main()` 或 `fn main(argv: string[])` 二选一；参数不能是别的类型。
- 返回类型可以是 `void` 或 `int`（`int` 作为进程退出码）。
- 带 `argv` 时，`argv[0]` 是程序名（与 C 一致），其余是命令行参数。

```lux
fn main(argv: string[]) {
    if len(argv) < 2 {
        println("用法: " + argv[0] + " <名字>");
        return;
    }
    println("你好，" + argv[1]);
}
```

运行：`./build/luxc greet.lux --run -- 世界`（`--` 之后的参数传给程序）。

---

## 1. 词法

### 1.1 注释

```lux
// 行注释
/* 块注释，/* 可以嵌套 */ 仍然在块注释里 */
```

### 1.2 标识符与关键字

标识符只支持 **ASCII 字母、数字、下划线**，不能以数字开头，区分大小写
（`name` 与 `Name` 是两个变量）。源码是 UTF-8，字符串与注释里可以用中文。

关键字（不能作为变量名）：

```
fn  let  const  return  if  else  elif  while  for  loop  in  break  continue
repeat  struct  true  false  and  or  not  nan  inf  none  import  extern  as
int  float  bool  string  void
```

`elif` = `else if`；`and`/`or`/`not` 分别是 `&&`/`or`/`!` 的别名
（注意 `or` 有特殊的兜底语义，见第 5 节）。

### 1.3 字面量

```lux
let a = 42;              // 十进制
let b = 0xFF;            // 十六进制
let c = 0b1010;          // 二进制
let d = 0o17;            // 八进制
let e = 1_000_000;       // 下划线只是分隔，忽略
let pi = 3.14159;        // float
let big = 1.0e9;         // 科学计数法
let f = nan;             // 浮点 NaN（关键字）
let g = inf;             // 浮点 +∞（关键字；-inf 用一元负号）
let t = true;
let s = "hello\nworld";  // 字符串（UTF-8，不跨行）
let r = r"C:\path\no\escape";  // 原始字符串：r"..." 里不做任何转义
```

字符串转义：`\n \t \r \0 \a \b \f \v \e \\ \" \' \xNN \uNNNN \UNNNNNNNN`。
注意 `\xNN` 写的是**码点**：`"\xFF"` 编码成两字节 UTF-8 `C3 BF`。

---

## 2. 类型系统

### 2.1 类型一览

| 类型 | 写法 | 说明 |
| --- | --- | --- |
| 整数 | `int` | 64 位有符号；**没有无符号类型** |
| 浮点 | `float` | IEEE-754 双精度 |
| 布尔 | `bool` | `true` / `false` |
| 字符串 | `string` | 不可变的 UTF-8 字节序列；`len` 是**字节数** |
| 数组 | `T[]` 或 `[T]` | 元素同类型，引用语义，可增长 |
| 具名结构 | `Point` | struct，引用语义 |
| 可选 | `T?` | 可能是 `none`；错误通道用 |
| 空 | `void` | 只作函数返回类型 |

数组可嵌套：`int[][]`、`string[]`。可选也可嵌套，但 `T?` **不能作数组元素**：
写 `int[]?` 表示“可能为 none 的数组”，`int?[]` 是错误。

### 2.2 只有两条隐式转换

1. `int → float`（数值提升）
2. `T → T?`（自动装箱成 `some`）

其余都必须显式转换。特别地 **`T? → T` 必须解包**（`?` / `or` / `!`），
否则编译错误——这是 Lux 错误通道的核心设计。

### 2.3 引用语义 vs 复制语义（重要）

- **数组、struct 是引用语义**：赋值、传参、返回都只是复制“引用”，
  多个变量指向同一个对象；对其中一个 `push` / 改字段，其他都能看到。
- **切片 `a[lo..hi]` 是复制语义**：得到新数组，改它不影响原数组。
- **字符串不可变**：不能 `s[i] = ...`；拼接 `+` 总是产生新字符串。

```lux
fn main() {
    let a = [1, 2, 3];
    let b = a;          // b 和 a 是同一个数组
    b.push(4);
    println(a);         // [1, 2, 3, 4]

    let c = a[0..2];    // 切片：新数组
    c[0] = 99;
    println(a);         // [1, 2, 3, 4]（不受影响）
    println(c);         // [99, 2]
}
```

---

## 3. 变量与作用域

```lux
let x = 5;              // 类型推断为 int
let y: int = 5;         // 显式标注
let z: int;             // 未初始化 → 默认值 0（float 0.0 / bool false / string "" / 数组空）
let name = "lux";
```

- `let` 声明后**可重新赋值**（用 `=`）。
- 作用域从声明处到所在 `{ }` 结尾；内层可遮蔽外层同名变量；
  同一作用域内不允许重复声明。
- **没有顶层可变全局变量**：顶层只有 `const`（编译期常量）。
  需要跨函数共享可变状态，请用数组（引用语义）或作为参数传递。
- `const` 顶层声明要求初始值是常量表达式（字面量 / 算术 / 拼接 / 引用别的常量）。

```lux
const N: int = 4;
const NAME = "lux";        // 类型可推断

fn main() {
    let total = 0;
    for i in 0..N { total += i; }
    println(NAME + ":" + string(total));
}
```

赋值目标是“左值”：变量、数组元素 `a[i]`、struct 字段 `p.x`，以及组合
`arr[i].x`。复合赋值 `+= -= *= /= %= &= |= ^= <<= >>=` 可用，且左值只求值
一次；`x++` / `x--` 只能作为独立语句。

---

## 4. 控制流

### 4.1 if / else if / elif

```lux
if x > 0 {
    println("正");
} else if x < 0 {
    println("负");
} else {
    println("零");
}
```

条件必须是 `bool`（没有“真值”隐式转换，`if 1 {}` 会报错）。

`if` 还能作为**表达式**（三分支都有值、必须有 else）：

```lux
let label = if n > 0 { "pos" } else { "nonpos" };
```

### 4.2 while / loop / repeat

```lux
let i = 0;
while i < 10 { i += 1; }

loop {                 // 等价于 while true
    if i <= 0 { break; }
    i -= 1;
}

repeat 3 {             // 重复 3 次；次数只求值一次
    println("hi");
}
```

`break` / `continue` 只能出现在循环里。

### 4.3 for：区间与 for-in

```lux
for i in 0..10 {       // 左闭右开：0..9，共 10 次
    print(string(i));
}
for i in 0..=10 {      // 闭区间：0..10，共 11 次
    print(string(i));
}

let xs = [10, 20, 30];
for x in xs {          // for-in 数组：x 是元素
    println(x);
}
for c in "abc" {       // for-in 字符串：c 是单字节字符串
    println(c);
}
```

- 循环变量作用域只在循环体。
- 区间终点只求值一次；for-in 数组用进入循环时的长度快照，循环内 push/pop
  不改变本次遍历次数。
- for-in 只支持数组和字符串；需要下标请用区间形式。

---

## 5. 运算符

优先级从低到高（完整表见 `docs/grammar.md` §3）：

```
|| or
&& and
|   ^   &
==  !=
<  <=  >  >=
<<  >>
+  -
*  /  %
- ! not ~        （一元）
a[i]  p.x  expr?  f!(...)   （后缀）
```

要点：

- `+` 对两个 `string` 是拼接；对数值是加法。
- `/` 是整数除法（除零 panic）；`%` 只用于 `int`。
- `<< >>` 的位移量**按 64 取模**（`x << 64` 即 `x << 0`）。
- 比较 `< <= > >=` 对字符串按字节字典序。
- `== !=` 不能用于数组，也不能直接用于 `T?`（先解包）。
- `&&` / `||` 是逻辑（两侧必须 `bool`）。
- **`or` 会分派**：左侧是 `bool` 时是逻辑或；左侧是 `T?` 时是“兜底取值”。
  `||` 永远是逻辑或，不做分派。
- `or` 不能链式兜底：`a? or b? or c` 会报错；请写 `a? or (b? or c)`。

```lux
let v = int("42") or 0;    // 解析失败就用 0
let ok = a > 0 or b > 0;   // bool 逻辑或
```

---

## 6. 函数

```lux
fn add(a: int, b: int) -> int {
    return a + b;
}

fn greet(name: string) {        // 无返回类型 = void
    println("hi " + name);
}

fn fact(n: int) -> int {        // 递归
    if n <= 1 { return 1; }
    return n * fact(n - 1);
}
```

- 参数必须写类型；返回类型用 `-> T`，省略即 `void`。
- **没有默认参数、可变参数、重载、泛型、闭包**（1.1 起有 `fn` 函数类型，
  但只能引用**具名函数**，没有 lambda / 捕获；泛型留到 1.x）。
- 没有“表达式体”箭头函数，函数体总是 `{ }` 块。
- 非 `void` 函数的每条路径都必须 `return`（struct 返回类型漏掉路径是硬错误
  `E0005`）；编译器也会对疑似漏 return 给 `W1003` 警告。

### 6.1 extern：调用 C 函数

```lux
import "c:m";                       // 链接 libm
extern fn cos(x: float) -> float;   // 直接声明 C 符号

fn main() {
    println(cos(0.0));
}
```

`extern fn` 只声明、不生成函数体；返回/参数里的 `string` 按 `const char*`
**借用**传递，没有所有权转移（与 ARC 无关）。`extern fn` **不能作为值**。

### 6.2 函数作为值：`fn` 类型（1.1）

函数类型写作 `fn(T, ...) -> R`，可作局部变量、参数、返回值：

```lux
fn add(x: int, y: int) -> int { return x + y; }

fn apply(op: fn(int, int) -> int, a: int, b: int) -> int {
    return op(a, b);
}

fn main() {
    println(apply(add, 3, 4));            // 7
    let f: fn(int, int) -> int = add;      // 具名函数取地址
    println(f(10, 20));                    // 30
    println(apply(f, 1, 2));               // 3（函数值可再作参数）
}
```

函数值是 **8 字节地址槽**（C 函数指针 / 原生代码地址），**不产生堆对象**。
约束：

- 只能引用**具名函数**，**不能**内联定义（没有 lambda / 闭包 / 捕获）。
- `extern fn` 不能作为值；`main` 不能作为值。
- `get()(x)`（对返回函数的调用结果再调用）不支持，先 `let g = get(); g(x);`。

---

## 7. 数组

```lux
let a = [1, 2, 3];              // int[]
let b: float[] = [1, 2.5];      // int 元素自动提升为 float
let grid: int[][] = [[1, 2], [3]];
let empty: string[] = [];       // 空数组必须靠标注推断元素类型
```

- 下标从 0 开始；越界**立即 panic**（打印下标与长度）。
- 切片 `a[lo..hi]`（不含 hi）、`a[lo..=hi]`（含 hi）；端点可省略：
  `a[..2]` / `a[2..]` / `a[..]`。切片是**新数组**（复制语义）。
- 方法（只能在数组变量上调用）：

| 方法 | 说明 |
| --- | --- |
| `a.push(v)` | 尾部追加 |
| `a.pop()` | 弹出并返回末尾元素（空数组 panic） |
| `a.insert(i, v)` | 在下标 i 插入 |
| `a.remove(i)` | 删除并返回下标 i |
| `a.clear()` | 清空（长度归零） |

- `len(a)` 返回元素个数；`len(s)` 返回字符串**字节数**。
- 数组不能比较（`==`/`<` 等），不能作全局常量，不能对字符串元素做下标赋值。

```lux
fn main() {
    let a: string[] = [];
    for i in 0..3 { a.push("v" + string(i)); }
    println(a);              // ["v0", "v1", "v2"]
    println(len(a));         // 3
    let s = a[1..];          // ["v1", "v2"]
    a[0] = "z";
    println(a);
    println(s);
}
```

---

## 8. struct

```lux
struct Point { x: int; y: int; }

struct Person {
    name: string;
    age: int;
    tags: string[];
}

fn main() {
    let p = Point { x: 1, y: 2 };   // 字段必须全部给出，顺序任意
    p.x = 10;                        // 字段可写（非 const）
    println(p);                      // Point { x: 10, y: 2 }
}
```

规则：

- struct 名**全局唯一**；字段类型可以是 `int/float/bool/string/数组/struct`。
- **引用语义**：赋值 / 传参 / 返回共享同一个对象。
- 不能用 `==` 比较 struct；可以打印、嵌套、放进数组。
- 不能声明“未初始化的 struct 变量”（`let p: Point;` 报错；需要空值用 `Point?`）。
- struct 里不能有 `T?` 字段。
- 字段访问可链式：`a.b.c`、`arr[i].x`。

---

## 9. 错误通道（可选类型 `T?`）

Lux 用**可选类型 + 三个解包符**表达失败，而不是异常。

```lux
fn parse_int(s: string) -> int? {
    return int(s);           // int(string) 返回 int?
}

fn double(s: string) -> int? {
    let n = int(s)?;         // 失败 → 当前函数立即 return none
    return n * 2;
}

fn main() {
    // 1) ? 传播
    let a = double("21")?;          // 这里 main 返回 void，不能写 ?（见下）
}
```

- `expr?`：`expr` 是 `T?`；为 `none` 时当前函数**立即返回 `none`**。
  因此使用 `?` 的函数返回类型必须是 `U?`。`main` 返回 `void`/`int`，
  不能在 main 里直接用 `?`。
- `lhs or rhs`：`lhs` 是 `T?`，为 `none` 取 `rhs`（需能转成 `T`）。
- `name!(...)`：panic 变体，失败即打印错误并退出（脚本友好）。

```lux
fn main() {
    let n = int("abc") or -1;      // 兜底
    println(n);                    // -1
    let m = int!("42");            // 确定不会失败 → 解包，失败则 panic
    println(m);                    // 42
}
```

标准库中带错误通道的函数：

| 调用 | 返回 | 失败 |
| --- | --- | --- |
| `int(x)` | `int`（数值）/ `int?`（字符串） | 字符串解析失败 → `none` |
| `float(x)` | `float` / `float?` | 解析失败 → `none` |
| `read(path)` | `string?` | 打不开 → `none` |
| `find_opt(s, sub)` | `int?` | 找不到 → `none`（0.9.4） |
| `byte_at(s, i)` | `int` | 第 `i` 个字节的值 0..255（1.1） |
| `bytes(s)` | `int[]` | 每个字节的值（1.1） |
| `int!` / `float!` / `read!` | 对应 `T` | 失败 → panic |

`print`/`println`/`string`/`format` 可以直接打印 `T?`，输出 `some(x)` 或 `none`。

### 9.1 `last_error()`：知道“为什么失败”（0.9.4）

`T?` 只回答“成没成”，不回答“为什么”。需要区分时（例如“文件不存在”与
“没有读取权限”），失败后读一次全局的 `last_error()`：

```lux
import "file";

fn main() {
    let data = read("/etc/hosts") or "";
    if len(data) == 0 {
        println(last_error());     // 例如：read 失败：文件不存在
    }
}
```

- 消息在**最近一次标准库失败**时写入，成功不会覆盖（语义就是“最近一次失败”）。
- 当前填充失败路径：`read()`（不存在 / 没权限 / 打不开）、`int()`、`float()`、
  `find_opt()`。
- 无并发（Lux 不引入线程），单个全局量够用；**不改变 `T?` 的任何签名**。
- 无 import 即可用。

---

## 10. 模块与包

### 10.1 import 的四种形态

```lux
import "math";                 // 标准库模块（注入成员）
import "string";               // 字符串函数
import "time";                 // now / monotonic / sleep
import "file";                 // read / write / exists ...
import "system";               // system / env / setenv
import "c:m";                  // 链接 C 库（配合 extern fn）

import "./util.lux";           // 相对路径：合并该文件顶层声明
import "util.lux";
import "pkg";                  // 包目录（lux.json / main.lux / lib.lux）
import "jsonx";                // 已安装包：~/.lux/packages/jsonx

import "math" as m;            // 别名：只能 m.pow(...) 访问

from "./util.lux" import helper, TAG;   // 0.9.3：只导入列出的成员
from "math" import *;                   // 等价于默认导入
#import "math";                         // 0.9.3：预处理器写法（同 import）
```

不带 `as` 的 import 把成员注入当前命名空间；任何已 import 的模块都能用
`模块名.成员` 限定访问。同一文件只加载一次，循环 import 安全。

**选择性导入（0.9.3）**：`from "mod" import a, b;` 只把列出的成员注入当前
命名空间，模块里其他成员不会被裸名访问（仍可用 `mod.other` 限定访问）。
`from` 是上下文关键字，仍可当普通标识符用。

**预处理指令（0.9.3）**：以 `#` 开头的行在词法分析前被处理（行号不变）：
`#import` / `#include` 等价于 `import`；`#define NAME [值]` 定义对象式宏；
`#undef NAME`；`#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`
做条件编译；`#error 消息` 主动报错。宏在普通代码行里整词展开（跳过字符串与
`//` 注释）。示例：

```lux
#define DEBUG 1
#import "math";

fn main() {
#ifdef DEBUG
    println(sqrt(2.0));
#else
    println("release");
#endif
}
```

### 10.2 项目构建（LuxBuildFile，0.9.3）

大项目用 `luxc build` 按项目描述文件构建，类似 C 的 make：

```bash
luxc build .            # 读 ./LuxBuildFile 构建
luxc build . run        # 构建后运行
luxc build . test       # 构建后跑 tests/ 下用例并比对 .expected
luxc build . clean      # 清理产物
luxc build . rebuild    # 清理后重建
```

`LuxBuildFile` 是行式 `键 = 值` 配置（`#` 起注释，值可用引号包住空格）：

```ini
name    = myapp
main    = src/main.lux
out     = build/myapp
backend = c            # 或 native
arc     = true
opt     = 2
cflags  = -DFOO
run_args= a b
testdir = tests
```

### 10.3 包管理

```bash
luxc install jsonx            # 从注册表安装（自动装依赖）
luxc install mathx@0.1.0
luxc search json              # 搜索
luxc info jsonx                # 查看
luxc list / upgrade / remove
luxc add ./mypkg              # 安装本地包目录
luxc login                    # 登录后才能发布
luxc publish ./mypkg          # 发布（HTTP 账号 API）
luxc unpublish mypkg 1.0.0
```

包的元数据写在 `lux.json`：

```json
{
  "name": "mypkg", "version": "1.0.0",
  "summary": "一句话简介", "main": "lib.lux",
  "files": ["lib.lux"], "deps": { "mathx": "^0.1.0" },
  "tags": ["utils"], "authors": ["me"], "license": "MIT"
}
```

依赖会递归安装，版本约束只做 `^ ~ >= <= =` 的简单匹配（不做 semver 求解）。

---

## 11. 标准库速查

以下函数在 `import` 对应模块后可直接使用（`len` / `print` / 转换 / `assert`
等是所有程序都可用的核心内建）。

**核心 / 输出**

| 函数 | 说明 |
| --- | --- |
| `print(x...)` / `println(x...)` | 输出（可多个参数，自动转字符串） |
| `byte_at(s, i)` / `bytes(s)` | 字节值 / 字节数组（1.1） |
| `map` / `filter` / `map_opt` | 高阶映射 / 过滤 / 可选映射（1.1，见 §6.2） |
| `string(x)` | 转字符串 |
| `int(x)` / `float(x)` | 转数值；字符串版返回 `T?` |
| `find_opt(s, sub)` | 子串字节下标 `int?`；找不到返回 `none`（0.9.4） |
| `len(x)` | 字符串字节数 / 数组元素数 |
| `assert(cond)` / `assert(cond, msg)` | 不成立则 panic |
| `exit(code)` | 退出 |
| `input()` / `input(prompt)` | 读一行 |
| `format("x={} y={}", a, b)` | 占位符 `{}` 顺序替换 |
| `abs(x)` / `sqrt(x)` | 数学 |

**math**（`import "math";`）

`pow floor ceil round sin cos tan asin acos atan atan2 log log10 exp
fmod hypot trunc isnan isinf random seed min max`，以及常量
`math.pi` / `math.e`。

**time**：`now()`（墙钟秒）、`monotonic()`（单调秒）、`sleep(sec)`、
`sleep_ms(ms)`。

**system**：`system(cmd)`（返回退出码）、`env(name)`（缺失返回 `""`）、
`setenv(name, val)`。

**file**：`read(path)` → `string?`、`write(path, data)`、`append(path, data)`、
`exists(path)`、`remove(path)`、`rename(from, to)`。

**net**（`import "net";`，1.2）：`dial(host, port)` → `int?`（含 DNS-lite）、
`send(fd, data)` → `int?`、`recv(fd, max)` → `string?`、`close(fd)` → `bool`、
`listen(port)` → `int?`、`accept(lfd)` → `int?`、`set_timeout(fd, secs)` → `bool`。
`socket` fd 是裸 `int`，不参与 ARC；失败原因读 `last_error()`。
**没有 TLS/HTTPS**：官方 `httpx` 包（`luxc install httpx`）只支持 `http://`，
提供 `get(url)` / `post(url, body, content_type)` / `header(resp, name)`，
返回 `HttpResponse`（`status` / `reason` / `header_keys` / `header_vals` / `body`）。

**string**：`contains startswith endswith find replace trim upper lower
substr(s, start, len) split(s, sep) chars(s) join(arr, sep)`。
注意 `find` 找不到返回 `-1`；`substr` / `chars` / `split` 都按**字节**处理。

**数组方法**：`push / pop / insert / remove / clear`（见第 7 节）。

> 完整、随版本冻结的签名见 `README.md` 的标准库章节与
> `docs/stability.md`；`math` 等模块是否“默认可用”以 README 为准
> （推荐总是显式 `import`）。

---

## 12. 内存模型与 ARC

- 数组 / struct / 字符串在堆上分配。**0.9.4 起引用计数（ARC）默认开启，
  C 后端与原生后端都是**：临时对象在每条语句结束时归还，局部变量在作用域
  出口回收。典型收益：1e6 次字符串构造的峰值内存从 ~117 MiB 降到 ~11 MiB。
- 需要退回旧行为（只增不减、进程退出时统一回收）时用 `--no-arc`；
  它只为排查与对拍保留，不建议日常使用。
- 原生后端按 `docs/arc.md` §2.3 实现了**尺寸分级 free list**（2 的幂分级 +
  bss 桶 + 无锁），`--arc --native` 不再报错。
- 仍存的最小缺口（只泄漏、不误释放）：`main(argv)` 的 argv 数组是一次性
  泄漏。1.1 已修复原生后端 `break` / `continue` / `return` 提前离开块时
  不释放该块引用型局部的问题。详见 [arc.md](arc.md) §2.4。
- 无论 ARC 开还是关，**可观察行为完全一致**（输出、panic 文案逐字节相同）。

写字符串拼接密集的代码时（如序列化器），优先用一个字符串变量累加：

```lux
let out = "";
for c in chars(s) {
    out = out + c;      // --arc 下旧串会被回收
}
```

> 目前没有 `map<K,V>`、没有泛型、没有闭包 / lambda。`fn` 函数类型、
> `sort` / `map` / `filter` / `map_opt` 已在 1.1 落地（见 §6.2），可以放心使用。

---

## 13. 惯用法与小抄

```lux
// 遍历数组并带下标
let xs = [10, 20, 30];
for i in 0..len(xs) {
    println(string(i) + ": " + string(xs[i]));
}

// 读整个文件（失败给默认值）
let text = read("config.txt") or "";
if len(text) == 0 {
    println("空或读取失败");
}

// 按行处理
let lines = split(text, "\n");
for line in lines {
    let t = trim(line);
    if len(t) > 0 { println(t); }
}

// 取命令行参数
fn main(argv: string[]) {
    if len(argv) > 1 {
        println(argv[1]);
    }
}

// 注意：len() 是字节数，不是“字符数”
println(len("中文"));    // 6（UTF-8 每字 3 字节）

// 拼接用 +，比较用 ==（字符串）
let ok = lower(name) == "lux";

// 发一个 HTTP 请求（1.2；文件顶部 import "httpx"; 并先 luxc install httpx）
// 注意：无 TLS，URL 必须以 http:// 开头
let r = get("http://example.com/")!;
println(r.status);
println(header(r, "Content-Type") or "?");
```

### 13.1 常见陷阱

1. **没有隐式真假**：条件必须是 `bool`。
2. **`T?` 必须解包**：不能 `if maybe_int {}`，也不能用 `==` 比较 `T?`。
3. **数组/struct 是引用**：想复制请用切片或手动构造新对象。
4. **切片是复制**，与引用语义相反（这是刻意的）。
5. **字符串不可变**，字节为单位；中文一个字 3 字节。
6. **没有 map / 泛型 / 异常 / 类继承**（1.1 起数组有 `sort` / `map` / `filter` / `map_opt`）。用一个 `struct Foo { kind: int; ... }`
   加 `int` 标签模拟联合类型。
7. **`or` 不能链式兜底**，要加括号。
8. **`repeat n` 只求值一次**；`while` 每次重算条件。
9. **整数除法 `5/2 == 2`**；要小数请用 `float`（`5.0/2.0`）。
10. **`nan != nan`**，用 `isnan(x)` 判断；`inf` 用 `isinf(x)`。

---

## 14. 诊断与运行时错误

- 编译错误格式：`error[E0003]: 说明` + `--> 文件:行:列` + 源码片段与 `^`。
- 错误码：`E0001` 词法、`E0002` 语法、`E0003` 语义/类型、`E0004` 模块/IO、
  `E0005` struct 函数返回落穿；`W1001` 未使用、`W1002` 结果未使用、
  `W1003` 缺 return、`W1004` extern 与运行时符号冲突。
- 运行时错误（panic）统一输出：

```
lux: 运行时错误: <说明>
lux: （如果是递归函数，也可能是调用层数过深导致栈溢出）
```

  例如 `数组下标越界：下标 5，但长度只有 3（下标从 0 开始）`、
  `整数除法的除数为 0`、`对空数组调用 pop()`。
- 编译时可以 `-Werror` 把警告当错误；`--no-color` 关彩色。

---

## 15. 给 AI 的约束清单（生成 Lux 代码前必读）

1. 目标版本 **1.2.0**；只使用本文与 `docs/grammar.md` 里出现的语法。
2. **不要**使用：泛型、`map<K,V>`/`set`、闭包/lambda、`class`/继承、
   `interface`/trait、异常、`switch`/`match`、`for (;;)`、`do/while`、
   可变全局变量、无符号整数、`++x` 表达式、字符串插值、`null`（用 `none`）、
   命名参数、默认参数、可变参数、运算符重载。宏只支持 `#define NAME 值`
   这种对象式宏（不支持函数式宏）。
   `fn` 类型只能引用**具名函数**（`let f: fn(int) -> int = inc;`），
   不能内联定义函数值。
3. 每个程序都要有 `fn main()`（或 `fn main(argv: string[])`）。
4. 条件用 `bool`；`&&`/`||` 两侧必须 `bool`。
5. 失败用 `T?`：`int("x")` 返回 `int?`；用 `or` 兜底或 `!` panic；
   在返回 `T?` 的函数里才用 `?` 传播。
6. 数组与 struct 是引用；需要独立副本时用切片或重新构造。
7. 字符串按字节；中文一个字 3 字节；`len` 返回字节数。
8. `let` 必须有初始值或类型标注；`let x;` 是错的。
9. `const` 只能顶层、且是常量表达式。
10. 函数所有路径都要 `return`（非 void）。
11. 数组方法只在数组**变量**上调用（`a.push(x)`），不能 `f().push(x)` 或
    `obj.arr.push(x)`（先 `let t = obj.arr; t.push(x); obj.arr = t;`）。
12. 需要“可能缺失的结构体”用 `Point?`；不能声明未初始化的 struct 变量。
13. 包名 / 版本：`lux.json` 里 `main` 指向入口 `.lux` 文件，`files` 列出要打包的文件。
   网络只有 `http://`：**没有 HTTPS/TLS**，URL 必须以 `http://` 开头；
   `net` 只支持 IPv4，服务端顺序 accept，无并发。
14. 输出用 `print`/`println`（不要 `printf`）；格式化用 `format("{}", x)`。
15. 不确定行为时，优先选择更保守的写法，并保持类型完全一致，避免隐式转换
    （唯一允许的是 `int→float` 与 `T→T?`）。

### 15.1 一个可直接套用的骨架

```lux
import "math";
import "string";

struct Item { name: string; count: int; }

fn total(items: Item[]) -> int {
    let sum = 0;
    for it in items {
        sum += it.count;
    }
    return sum;
}

fn describe(n: int) -> string {
    if n > 0 {
        return "正数";
    } else if n < 0 {
        return "负数";
    } else {
        return "零";
    }
}

fn main(argv: string[]) {
    let items: Item[] = [
        Item { name: "a", count: 3 },
        Item { name: "b", count: 4 },
    ];
    println(format("total={}", total(items)));
    println(describe(total(items)));
    for it in items {
        println(upper(it.name) + " x" + string(it.count));
    }
}
```

---

## 16. 从哪里继续

- 精确语法：`docs/grammar.md`
- 1.0 会冻结什么、不会有什么：`docs/stability.md`
- 编译器内部与双后端：`docs/design.md`、`docs/arc.md`
- 命令行 / 标准库完整清单 / 包注册表：`README.md`、`server/README.md`
- 官方包源码（可当范例读）：`packages/mathx`、`packages/strx`、
  `packages/numx`、`packages/jsonx`
