# Lux

<p align="center">
  <img src="logo.svg" alt="Lux 图标" width="160">
</p>

一门用 C++ 写的、**编译到原生机器码**的静态类型编程语言。

Lux 的语法是现代静态风格（`fn` / `let` / 类型后置），有两种编译路径：
**C 后端**把 Lux 源码翻译成 C，再交给 gcc/clang 优化成可执行文件；
**原生后端**（0.6 新增，`--native`）直接输出**本机架构**的 Linux ELF，
不依赖 C 编译器、不链接 libc —— 运行时系统全部由 Lux 自身实现。
当前原生后端覆盖 **x86-64** 与 **aarch64(arm64)**：编译时按宿主架构选择
发射器（`native_emit.hpp` / `native_emit_arm64.hpp`），在 arm64 上也能做到
无 C 依赖直出可执行文件。

0.5 带来了**数组**（引用语义 + 越界检查 + `push` / `pop` / `insert` / `remove` 方法）、
**for-in 迭代**（数组与字符串）和 `split` / `chars` / `join` 三个字符串利器；
0.6 带来了**原生后端**与**切片**语法 `a[lo..hi]` / `a[lo..=hi]`；
0.7 是**语言完整版**：新增 **`struct` 结构体**（声明 / 构造 / 成员读写 / 嵌套 /
struct 数组）、**`if` 表达式**、**原始字符串 `r"..."`** 与 **`nan` / `inf` 字面量**、
**命令符参数 `main(argv: string[])`**，并解锁**嵌套 / 成员复合赋值**
（`grid[i][j] = v`、`p.x += 1`、`a[i] *= 2`）。
0.8 是**错误通道 + 发布工程版**：新增**可选类型 `T?`**、**传播后缀 `expr?`**、
**兜底表达式 `lhs or rhs`**、**panic 变体 `int!` / `read!`**，并把 `int` / `float` /
`read` 迁移到错误通道；同时清掉一批 C 层欠账、修了三个原生后端潜伏 bug、
上线 CI + 双后端差分矩阵：

```lux
// config.lux —— 0.8 的错误通道
import "file";
import "string";

// 返回 int?：读取或解析失败就返回 none
fn load_port(path: string) -> int? {
    let text = read(path)?;        // 失败向上传播
    return int(text)?;
}

fn main(argv: string[]) {
    let path = if len(argv) > 1 { argv[1] } else { "port.txt" };
    println(load_port(path));              // some(8080) 或 none
    println(load_port(path) or 3000);      // 兜底默认值
    let port = load_port(path) or 3000;
    println("listening on " + string(port));
    println(read!("README.md")[0..5]);     // read!：失败即 panic
}
```

0.9.3 是**转正版**：修复 ARC 下折叠字符串常量的无头指针缺陷并回收打印临时串；
新增 **`luxc build` + `LuxBuildFile`** 项目构建（构建 / 运行 / 测试 / 清理，
类似 `make`）、**`from … import …` 选择性导入**、**`#import` 与
`#define`/`#ifdef` 预处理指令**；自带库增补 mathx / strx 并新增 arrx。
0.9.4 是 **ARC 收官版**：原生后端尺寸分级 free list 与引用计数落地，
**两个后端 ARC 默认开启**（`--no-arc` 退回旧行为），`T?` 装箱回收闭环，
并新增错误消息通道 `last_error()` 与 `find_opt()`。
**1.0.0 是正式版**：语言、标准库、错误通道、项目构建、包管理与 ARC
内存管理全部冻结；相对 0.9.4 **不加新语法、不改可观察行为**，只把版本
转正、自带包对齐到 1.0、补齐文档 —— 稳定承诺见
[docs/stability.md](docs/stability.md)。

```lux
// scores.lux —— 0.5 的数组与 for-in
fn average(xs: float[]) -> float {
    let total = 0.0;
    for x in xs {
        total += x;
    }
    return total / float(len(xs));
}

fn main() {
    let scores: float[] = [92.5, 88.0, 76.5, 95.0];
    println(scores);                        // [92.5, 88, 76.5, 95]
    println(average(scores));               // 88

    let parts = split("lux-0.8-release", "-");
    println(parts[1]);                      // 0.8
    println(join(parts, " / "));            // lux / 0.5 / release

    for c in "lux-lang" {                   // 逐字节迭代字符串
        print("[" + c + "]");
    }
    println("");
}
```

```lux
// hello.lux
fn fib(n: int) -> int {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

fn main() {
    let name = "Lux";
    println("你好, " + name + "!");
    for i in 0..10 {
        print(fib(i));
        print(" ");
    }
    println("");
}
```

```bash
$ luxc hello.lux --run
你好, Lux!
0 1 1 2 3 5 8 13 21 34
```

---

## 目录

- [快速开始](#快速开始)
- [语法速查](#语法速查)
- [类型系统](#类型系统)
- [模块与 import](#模块与-import)
- [项目构建](#项目构建luxbuildfile093-新增)
- [标准库](#标准库)
- [内置函数](#内置函数)
- [性能](#性能)
- [项目结构](#项目结构)
- [已知限制](#已知限制)
- [路线图](#路线图)

---

## 快速开始

### 构建编译器

需要 C++17 编译器，以及一台装有 `cc` / `gcc` / `clang` 的机器（用来编译生成的 C 代码）。

```bash
make                 # 产出 build/luxc
sudo make install    # 可选：安装到 /usr/local/bin
make test            # 跑回归测试
```

### 编译 Lux 程序

```bash
luxc hello.lux              # 生成可执行文件 ./hello
luxc hello.lux --run        # 编译后立即运行
luxc hello.lux -o app       # 指定输出文件名
luxc hello.lux -O3          # 指定优化级别（默认 -O2）
luxc hello.lux --emit-c     # 只输出生成的 C 代码，不调用 C 编译器
luxc hello.lux --keep-c     # 把生成的 .c 文件保留在可执行文件旁
```

### 交互式执行（REPL）

```bash
luxc repl                   # 输入表达式立即求值并打印结果
```

支持方向键行编辑（左右移动 / 退格 / Home / End）、上下方向键浏览历史
（跨会话保存在 `~/.lux/repl_history`）、**TAB 补全**关键字 / 内置函数 /
会话中定义的名字、多行 `fn` 定义（花括号不平衡时自动续行）。
`let` 变量跨行存活，`.clear` 清空会话，Ctrl-D 或 `.exit` 退出。
管道模式（`echo '1+2' | luxc repl`）下退化为逐行求值，适合脚本化。

### 项目构建（LuxBuildFile，0.9.3 新增）

大项目用 `luxc build` 按项目描述文件构建，用法类似 C 的 `make`：

```bash
luxc build .             # 读 ./LuxBuildFile 并构建
luxc build . run         # 构建后立即运行
luxc build . test        # 构建后跑测试（比对同名 .expected）
luxc build . clean       # 清理产物
luxc build . rebuild     # 清理后重建
luxc build --help        # 构建帮助
```

`luxc build` 会在给定路径下依次找 `LuxBuildFile` / `LuxBuildFile.lux` /
`LuxBulidFile` / `lux.build` / `LuxBuild`。`LuxBuildFile` 是行式
`键 = 值` 配置（`#` 起注释，值可用引号包住空格，相对路径相对该文件解析）：

```ini
name     = myapp            # 项目名（缺省取目录名）
main     = src/main.lux     # 入口源文件（同义：src / source / entry，可多个）
out      = build/myapp      # 输出可执行文件（缺省 build/<name>）
outdir   = build            # 输出目录
backend  = c                # c（缺省）或 native
arc      = true             # 开启引用计数内存回收
opt      = 2                # C 后端优化级别 0~3
cc       = gcc              # 指定 C 编译器
cflags   = -DFOO -O3        # 额外传给 C 编译器的选项
run_args = a b              # 运行时参数
testdir  = tests            # 测试目录（缺省 tests）
test     = tests/a.lux      # 测试文件 / 目录 / 通配符（可重复）
clean    = extra.tmp        # 额外清理项（可重复）
```

`build test` 会逐个编译并运行测试文件，与同名 `.expected` 逐行比对；
若存在同名 `.args` 则作为运行参数。

### 包管理

```bash
luxc install mathx          # 从官方注册表下载安装（自动装依赖）
luxc install mathx@1.0.0    # 指定版本（也支持 ^1.2 / >=1.2 等约束）
luxc add ./mypkg            # 安装本地包（目录或单个 .lux 文件，离线）
luxc search math            # 搜索注册表
luxc info numx              # 查看包信息
luxc list                   # 列出已安装的包
luxc upgrade                # 把已安装的包升级到最新
luxc remove mypkg           # 卸载（delete 同义）
luxc update                 # 刷新注册表索引缓存
luxc login                  # 登录注册表账号（发布 / 删除前需要）
luxc publish ./mypkg        # 发布自己的包（需登录）
luxc unpublish mypkg        # 从注册表删除自己发布的包
luxc registry <URL>         # 换用第三方注册表
```

安装后的包可以直接 `import "包名"` 使用，见[包管理](#包管理)。
默认注册表是 `https://lux.xfes.top/lux/lux.php`，可用 `LUX_REGISTRY`
环境变量或 `luxc registry` 指向任何返回相同 JSON 结构的地址（HTTP/HTTPS/FTP
都行）。

完整选项：

| 选项 | 说明 |
| --- | --- |
| `-o <文件>` | 输出的可执行文件名（默认取源文件名去掉扩展名） |
| `-c <文件>` | 把生成的 C 代码写到指定文件 |
| `--emit-c` | 只把 C 代码输出到标准输出 |
| `--keep-c` | 编译后把中间 `.c` 文件保留在可执行文件旁 |
| `-O <0~3>` | 优化级别，直接传给 C 编译器（默认 2） |
| `--run` | 编译成功后立即运行 |
| `--native` | **0.6 新增**：原生代码生成后端，直接输出本机架构（x86-64 / aarch64）ELF，不调用 C 编译器 |
| `--arc` | **0.9.2 新增，0.9.4 起默认开启**：引用计数内存回收，两个后端都支持；显式写出是冗余开关 |
| `--no-arc` | **0.9.4 新增**：退回 0.9.1 的「只增不减」语义，仅用于排查 / 对拍 |
| `build [路径] [动作]` | **0.9.3 新增**：按路径下的 `LuxBuildFile` 构建 / 运行 / 测试 / 清理（见[项目构建](#项目构建luxbuildfile093-新增)） |
| `--cc <编译器>` | 指定 C 编译器（默认自动探测 `cc` / `clang` / `gcc`） |
| `-C <选项>` | 把额外选项原样传给 C 编译器（可多次使用，如 `-C "-I/usr/local/include"`） |
| `-Werror` | 把警告当成错误 |
| `--no-color` | 关闭彩色诊断输出 |
| `-v` / `--version` | 版本信息 |
| `-h` / `--help` | 帮助 |

---

## 语法速查

### 变量与常量

```lux
let x: int = 10;        // 显式类型
let y = 3.14;           // 类型推断 → float
let z: int;             // 无初始值，默认 0
const MAX: int = 100;   // 局部常量，不可重新赋值

const VERSION = "0.7";  // 顶层全局常量
const KB: int = 2 * 1024;         // 常量表达式：编译期折叠
const GREETING = "hello" + "!";   // 字符串常量也能折叠
```

### 函数

```lux
fn add(a: int, b: int) -> int {
    return a + b;
}

fn greet() {            // 不带 -> 即返回 void
    println("hi");
}

fn fact(n: int) -> int {
    if n <= 1 { return 1; }
    return n * fact(n - 1);   // 支持递归、相互递归
}
```

程序入口是 `fn main()`，返回类型只能是 `void` 或 `int`；
允许可选签名 `fn main(argv: string[])` 接收命令行参数（0.7，`argv[0]` 为程序名）。

### 基础类型

| 类型 | 说明 | 对应的 C 类型 |
| --- | --- | --- |
| `int` | 64 位有符号整数 | `int64_t` |
| `float` | 64 位双精度浮点 | `double` |
| `bool` | `true` / `false` | `bool` |
| `string` | UTF-8 字符串 | `const char*` |
| `int[]` / `[int]` | **0.5 新增**：动态数组，两种写法等价（可嵌套 `int[][]`） | `lx_arr`（堆分配头指针） |
| `Point` 等 | **0.7 新增**：`struct` 具名类型（引用语义，堆上字段对象） | `lx_st_Point*` |
| `T?` | **0.8 新增**：可选类型（`int?` / `string?` / `Point?`，可嵌套 `int??`） | `T*`（NULL = none） |
| `void` | 无返回值（只能做函数返回类型） | `void` |

### 字面量

```lux
42                      // 十进制
1_000_000               // 下划线分隔，纯为可读性
0xFF                    // 十六进制
0b1010_1010             // 二进制
0o777                   // 八进制
3.14                    // 浮点
1.5e3                   // 科学计数法 = 1500
"hello\n\"world\""      // 字符串，支持 \n \t \" \\ \x41 \u4f60
true / false            // 布尔
nan / inf               // 0.7：非数 / 正无穷（-inf 也支持）
none                    // 0.8：可选类型的空值（类型由上下文决定）
r"no\escape"           // 0.7：原始字符串，不做任何转义处理
[1, 2, 3]               // 0.5 新增：数组字面量（支持尾逗号）
[]                      // 空数组：需要类型上下文（如 let a: int[] = [];）
[[1, 2], [3, 4]]        // 嵌套数组
```

### 运算符（优先级从高到低）

| 优先级 | 运算符 | 说明 |
| --- | --- | --- |
| 0 | `a[i]`（后缀） | **0.5 新增**：数组 / 字符串下标访问，可连续下标（`m[i][j]`） |
| 0 | `p.x`（后缀） | **0.7 新增**：struct 成员访问；也可与下标组合（`a[i].x`） |
| 0 | `a[lo..hi]`（后缀） | **0.6 新增**：切片（复制语义），`..=` 含端点；端点可省略（`a[..hi]` / `a[lo..]` / `a[..]`） |
| 0 | `expr?`（后缀） | **0.8 新增**：错误传播，失败时当前函数返回 `none`（需返回 `T?`） |
| 0 | `name!(...)`（后缀） | **0.8 新增**：panic 变体（`int!` / `read!` / 返回 `T?` 的用户函数），失败即退出 |
| 1 | `-x` `!x` `~x` `not x` | 取负、逻辑非、按位取反 |
| 2 | `*` `/` `%` | 乘、除、取模（`%` 只用于 int） |
| 3 | `+` `-` | 加、减（`+` 也用于字符串拼接） |
| 4 | `<<` `>>` | 左移、右移（只用于 int） |
| 5 | `<` `<=` `>` `>=` | 比较（数值，或字符串的字典序） |
| 6 | `==` `!=` | 相等性比较 |
| 7 | `&` | 按位与 |
| 8 | `^` | 按位异或 |
| 9 | `\|` | 按位或 |
| 10 | `&&` `and` | 逻辑与（短路） |
| 11 | `\|\|` `or` | 逻辑或（短路）；**0.8**：`or` 左侧为 `T?` 时改为兜底 |
| 12 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | 赋值与复合赋值 |

整数除法是整除（`7 / 2 == 3`），并且**带除零检查**，除零会给出明确的运行时错误而不是直接崩。

下标访问越界同样会立即 panic，错误信息带上下标与长度：
`数组下标越界：下标 5，但长度只有 3（下标从 0 开始）`。

### 控制流

```lux
// if / else if / else
if score >= 90 {
    println("优秀");
} else if score >= 60 {
    println("及格");
} else {
    println("不及格");
}

// while
let i = 0;
while i < 10 {
    i = i + 1;
}

// 区间循环：0..10 左闭右开，1..=10 闭区间
for i in 0..10 {
    print(i);
}
for i in 1..=10 {
    print(i);
}

// for-in 迭代（0.5 新增）：数组逐元素、字符串逐字节单字符
let total = 0;
for x in [10, 20, 30] {
    total += x;
}
for c in "lux" {
    print("[" + c + "]");
}
// 不需要循环变量时用下划线开头（_ 开头的变量不触发未使用警告）
for _ in 0..3 {
    print(".");
}

// 无限循环
loop {
    if done { break; }
    if skip { continue; }
}
```

`if` / `while` 的条件必须是 `bool`，不会做隐式的真值转换 —— 这是刻意的，
能挡掉一整类 bug。

### 数组（0.5 新增）

```lux
let a: int[] = [10, 20, 30];     // 前缀写法 [int] 与后缀写法 int[] 等价
a[1] = 99;                       // 下标赋值（越界立即 panic）
println(a[1]);                   // 下标访问，可连续：m[i][j]
println(a);                      // [10, 99, 30]，print / to_str 直接可用
println(len(a));                 // 3，len() 同样适用于数组

let s1 = a[0..2];                // 0.6 切片：[10, 20]（复制语义，半开区间）
let s2 = a[0..=1];               // ..= 含端点：[10, 20]
let s3 = a[1..];                 // 端点省略：从 1 到末尾
s1[0] = 999;                     // 修改切片不影响原数组

let names: string[] = ["ann", "bo"];
let nested = [[1, 2], [3, 4]];   // 嵌套数组，自动推断 int[][]
let empty: float[] = [];         // 空数组需要类型标注来推断元素类型

a.push(40);                      // 尾部追加
a.pop();                         // 弹出并返回尾元素（空数组 panic）
a.insert(1, 7);                  // 在下标 1 处插入
a.remove(0);                     // 删除下标 0 处的元素并返回它
a.clear();                       // 清空
```

**数组是引用语义**（与 Python 的 list 一致）：赋值、传参、返回、放进另一个数组，
传递的都是同一个数组——在函数里 `push`，调用方看得见：

```lux
fn grow(xs: int[]) {
    xs.push(4);                  // 修改对调用方可见
}
fn main() {
    let a: int[] = [1, 2, 3];
    let b = a;                   // b 和 a 是同一个数组
    b.push(4);
    println(len(a));             // 4
    grow(a);
    println(a);                  // [1, 2, 3, 4, 4]
}
```

数组方法一览（`push` / `insert` 会按需自动扩容）：

| 方法 | 说明 |
| --- | --- |
| `a.push(v)` | 尾部追加一个元素 |
| `a.pop()` | 弹出并返回尾元素（空数组会 panic） |
| `a.insert(i, v)` | 在下标 `i` 处插入（`i` 允许等于 `len(a)`，即尾部追加） |
| `a.remove(i)` | 删除下标 `i` 处的元素并返回它 |
| `a.clear()` | 清空（长度归零，容量保留） |

`const` 数组禁止调用会修改内容的方法，也禁止下标赋值。数组不支持 `==` 比较
（引用语义下"相等"有歧义），请逐个比较元素或比较 `len()`。

### struct 结构体（0.7 新增）

```lux
struct Point {
    let x: int;
    let y: int;
}

struct Line {
    let a: Point;   // 字段可以是另一个 struct
    let b: Point;
    let tag: string;
}

fn mid_x(l: Line) -> float {
    return float(l.a.x + l.b.x) / 2.0;
}

fn main() {
    let p = Point { x: 3, y: 4 };     // 构造：字段名必须全部给出
    println(p);                        // Point { x: 3, y: 4 }
    p.x = 10;                          // 成员赋值
    p.x += 5;                          // 成员复合赋值

    let q = Point { y: 7, x: -1 };     // 字段顺序任意
    let l = Line { a: p, b: q, tag: "seg" };
    println(mid_x(l));

    let ps = [Point { x: 1, y: 2 }, Point { x: 3, y: 4 }];
    ps[0].x = 100;                     // 数组元素也是 struct：直接改字段
}
```

- **引用语义**：struct 与数组一致，变量 / 传参 / 返回共享同一个字段对象，
  修改字段对调用方可见。
- 字段类型可为 `int` / `float` / `bool` / `string` / 数组 / 另一个 struct。
- 字段名在同一个 struct 内必须唯一；构造时必须给出全部字段（未给出的会报错）。
  `const p = ...` 声明的 struct 不能修改它的字段。
- struct 可以直接 `print` / `println` / `string()`，打印成 `Name { 字段: 值, ... }`。

### if 表达式（0.7 新增）

`if` 也可以出现在表达式位置（两个分支各是一个表达式，必须有 `else`）：

```lux
let label = if score >= 60 { "及格" } else { "不及格" };
let abs = if n > 0 { n } else { -n };
println(if a > b { a } else { b });
```

分支类型需相容（`int` 与 `float` 混合时提升为 `float`）。
控制流语句形式的 `if c { ... }` 仍然是默认形态，不要和表达式形态混淆。

### 命令行参数（0.7 新增）

```lux
fn main(argv: string[]) {
    println(len(argv));
    for a in argv {
        println(a);
    }
}
```

`argv[0]` 是程序名（与 C 约定一致），后续为传入的参数；
两个后端行为一致。用 `luxc app.lux --run -- args...` 可向程序传参。

### 错误通道：`T?` / `?` / `or` / `!`（0.8 新增）

可选类型 `T?` 表示“可能缺失 / 失败的值”（`none` = 空）。三种解包方式：

```lux
import "file";
import "string";

// 1) ? 传播：失败时整个函数立即返回 none（所在函数必须返回 U?）
fn load(path: string) -> int? {
    let text = read(path)?;
    return int(text)?;        // 解析失败也向上传播
}

fn main() {
    // 2) or 兜底：左边是 T? 时取默认值（左边是 bool 时仍是逻辑或）
    let n = load("port.txt") or 8080;
    println(n);

    // 3) ! panic 变体：失败即退出（脚本友好）
    println(int!("42") + 1);   // 43
    println(read!("README.md")[0..4]);

    // none 字面量与打印
    let maybe: string? = none;
    println(maybe);              // none
    println(load("port.txt"));   // some(8080) / none
}
```

规则（详见 [docs/grammar.md](docs/grammar.md) §8、[docs/stability.md](docs/stability.md)）：

- `T` 自动变成 `T?`（装箱）；`T?` **不能**自动变回 `T`。
- `or` 关键字按**左操作数类型**分派：`bool or bool` 是逻辑或，`lhs or default`
  是兜底；`||` 永远是逻辑或。
- **标准库迁移**：`int("abc")` 现在返回 `int?`（不再静默返回 `0`）；
  `float` 同理；`read` 返回 `string?`。带 `!` 的版本（`int!` / `float!` / `read!`）
  失败即 panic。其它函数（`write` / `exists` / `system` / `env` …）保持原值语义。

### 语法糖

```lux
// 复合赋值
x += 5;          // 等价于 x = x + 5
s += "!";        // 字符串同样适用
a[i] += 2;       // 0.7：数组元素复合赋值（左值只求值一次）
grid[i][j] *= 3; // 0.7：嵌套下标
p.x -= 1;        // 0.7：struct 成员
x <<= 2;         // 位运算复合赋值也支持（<<= >>= &= |= ^=）

// 自增 / 自减（语句形式）
i++;
j--;

// repeat n：把代码块重复执行 n 次（等价于 for _ in 0..n）
repeat 5 {
    println("hi");
}

// elif：else if 的别名
if score >= 90 {
    println("优秀");
} elif score >= 60 {
    println("及格");
} else {
    println("不及格");
}
```

### 注释

```lux
// 行注释
/* 块注释，可以 /* 嵌套 */ */
```

---

## 类型系统

- **静态类型**：每个表达式的类型在编译期就已确定。
- **类型推断**：`let x = 1;` 会根据初始值推断出 `int`。
- **唯一的隐式转换**是 `int → float`（ widening，不会丢信息）：

  ```lux
  let f: float = 3;      // OK，3 提升为 3.0
  println(7 / 2.0);      // 3.5，7 先提升为 float
  let i: int = 3.9;      // 错误！float 转 int 会丢信息
  let i2 = int(3.9);     // 正确，显式转换 → 3
  println("答案是 " + 42);   // 错误！字符串和数值不能相加
  println("答案是 " + string(42));  // 正确
  ```

  这条规则换来的是：**编译期就能抓住绝大多数类型错误，而且没有隐式转换带来的性能意外**。

---

## 模块与 import

`import "路径";` 把别的模块的函数和常量引入当前程序（顶层声明）：

```lux
import "math";          // 内置标准库模块
import "./util.lux";    // 相对当前文件所在目录的 .lux 文件
import "mypkg";         // 包目录 / 已安装的包
import "c:m";           // 链接 C 库（等价于 -lm）

// 0.9.3 新增：选择性导入 / 预处理器写法
from "./util.lux" import helper, TAG;   // 只导入列出的成员
from "math" import *;                   // 等价于默认导入
#import "math";                         // 等价于 import "math";
```

import 的解析顺序：

1. `c:库名` → 记录链接参数（等价于 `-l库名`）；
2. `math` / `time` / `system` / `file` / `string` → 启用标准库模块；
3. 相对 **import 声明所在文件的目录**：`路径.lux` 文件，或含
   `lux.json` / `main.lux` / `lib.lux` 的包目录；
4. 已安装的包目录 `~/.lux/packages/<路径>`（见[包管理](#包管理)）。

同一个文件被多个模块 import 只会加载一次，循环 import 也安全。

**选择性导入（0.9.3）**：`from "mod" import a, b;` 只把列出的成员注入当前
命名空间，模块里其他成员不会被裸名访问（仍可用 `mod.other` 限定访问）。
`from` 是上下文关键字，仍可当普通标识符用。

**预处理指令（0.9.3）**：以 `#` 开头的行在词法分析前处理（行号不变）：
`#import` / `#include` 等价于 `import`；`#define NAME [值]` 定义对象式宏；
`#undef`；`#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif` 条件编译；
`#error 消息` 主动报错。宏在普通代码行里整词展开（跳过字符串与 `//` 注释）：

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

### 命名空间与别名

每个模块都有自己的命名空间（模块名 = 文件名 / 包名 / 标准库名）。
默认导入会把成员注入当前命名空间；带 `as` 的导入只登记别名，
之后必须用 `别名.成员` 访问，**不会污染全局命名空间**：

```lux
import "math" as m;
import "./lib.lux" as lib;

fn main() {
    println(m.pow(2.0, 10.0));   // 别名限定访问
    println(m.pi);               // 模块常量同样支持
    println(lib.double(21));     // 用户模块的函数
}
```

默认导入的模块同样支持限定访问（`math.pow(...)` 和 `pow(...)` 都行）：

```lux
import "math";
println(math.pow(2.0, 8.0));   // 限定访问
println(pow(2.0, 8.0));        // 默认导入，裸名也行
```

### 包（项目目录）

`import "mypkg"` 会按顺序查找：`mypkg/lux.json`（包配置文件）→ `mypkg/main.lux`
→ `mypkg/lib.lux`。包配置文件是一个小 JSON：

```json
{
  "name": "mypkg",
  "main": "src/lib.lux"
}
```

`"main"` 指定入口文件；也可以用 `"files": ["a.lux", "b.lux"]` 列出多个源文件。
包内部的 import 同样递归生效。

### 包管理

写好的包可以用 `luxc add` 安装到 `~/.lux/packages/`，之后任何项目都能
直接 `import "包名"`：

```bash
luxc add ./mypkg            # 目录包：读 lux.json 的 "name"（没有就用目录名）
luxc add ./util.lux         # 单文件包：包名取文件名
luxc list                   # 查看已安装的包
luxc delete mypkg           # 卸载
```

- 安装是**拷贝**，之后修改原目录不影响已安装的包；
- 同名包重复安装会报错，先 `delete` 再 `add`；
- 包目录默认在 `$HOME/.lux/packages/`，可用环境变量 `LUX_HOME` 重定位。

#### 在线安装（注册表 / 下载库）

0.9 起 `luxc install` 可以直接从注册表下载安装，并自动解析依赖：

```bash
luxc install numx           # 安装 numx，会自动先装它依赖的 mathx
luxc install jsonx          # 专业 JSON 库：解析 / 生成 / 文件读写
luxc install mathx@0.1.0    # 精确版本；也支持 ^0.1 / ~0.1 / >=0.1 等约束
luxc install https://example.com/foo-1.0.0.tar.gz   # 直接从 URL 装
```

官方注册表现有：`mathx`（数论 / 数值）、`strx`（字符串）、`numx`（整数分解，依赖
mathx）、`jsonx`（JSON）。`jsonx` 用法：

```lux
import "jsonx";

fn main() {
    let r = parse("{\"port\":8080,\"hosts\":[\"a\",\"b\"]}");
    if r.ok {
        println(getInt(r.value, "port"));                // 8080
        println(asString(getPath(r.value, "hosts[1]"))); // b
        println(pretty(r.value));
    }
}
```

`install` 的 `spec` 可以是注册表里的包名、任意 `http(s)://` / `ftp://` URL，
或本地路径（等价于 `add`）。归档支持 `.tar.gz` / `.tgz` / 单文件 `.lux`。
安装过程是**先下载到临时目录 → 校验 SHA-256 → 原子 rename 到包目录**，
失败不会留下半成品。

注册表协议很小：

```
GET <registry>?action=index      -> {"packages":[{name,version,url,sha256,...}]}
GET <registry>?action=search&q=  -> 过滤后的索引
GET <registry>?action=info&name= -> 单包全部版本
GET <registry>?action=download&name=&version= -> 归档
```

`<registry>` 默认是 `https://lux.xfes.top/lux/lux.php`。只要外部地址返回
相同结构（服务端示例见仓库 `server/lux.php`），就能当注册表：

```bash
luxc registry https://my-mirror.example/lux.php
# 或临时： LUX_REGISTRY=https://my-mirror.example/lux.php luxc install foo
```

索引会缓存在 `$LUX_HOME/cache/index.json`，断网时仍可读缓存；`luxc update`
强制刷新。每个包目录里会写入 `.lux-meta.json` 记录版本 / 来源 / 哈希，
`luxc list` 与 `luxc upgrade` 据此工作。

#### 发布自己的包（需注册账号）

上传 / 修改 / 删除包都要求账号，且只能操作自己发布的包。先在网页端注册，
或者直接命令行登录：

```bash
luxc login alice            # 提示输入密码，令牌存到 ~/.lux/config.json
luxc whoami                 # 确认当前账号
```

包目录里放一个带 `name` / `version` / `main` 的 `lux.json`，然后：

```bash
luxc publish ./mypkg        # 未登录会提示先 luxc login
luxc unpublish mypkg 0.1.0  # 删除某个版本；省略版本号则删除整包
luxc logout                 # 退出登录
```

`publish` 会打包成 `<名字>-<版本>.tar.gz`、计算 SHA-256、生成元数据，再通过
HTTP 账号 API 上传到注册表（`lux.php?action=publish`）。发布 / 删除后本地索引
缓存会自动作废，`search` / `install` 立即能看到变化。若注册表地址是 `ftp://`，
则回退到 FTP 上传，用 `$LUX_FTP_HOST` / `$LUX_FTP_USER` / `$LUX_FTP_PASS` /
`$LUX_FTP_ROOT` 配置。

#### 网页界面与源码下载

注册表自带网页界面（`index.php`）：在线注册 / 登录 / 浏览 / 搜索 / 上传 /
编辑 / 删除自己的包，也能查看与下载别人的包；「下载」页还提供**包管理系统
源码**与**Lux 各版本编译器源码**（历史版本来自 `/sdcard/code/lux/`）。
本地生成下载包的脚本：

```bash
server/make_downloads.sh        # 产出 dist/downloads/*.tar.gz|zip + downloads.json
```

### 调用 C 库

```lux
import "c:z";   // 链接 -lz

// extern fn：声明一个 C 函数（不生成函数体，直接链接外部符号）
extern fn zlibVersion() -> string;

fn main() {
    println(zlibVersion());
}
```

`extern fn` 的类型映射与普通函数一致：`int → int64_t`、`float → double`、
`bool → bool`、`string → const char*`。注意声明的类型必须与 C 头文件一致。

---

## 标准库

Lux 自带五个标准库模块，用 `import` 引入后才能使用其中的函数：

```lux
import "math";
import "time";
import "system";
import "file";
import "string";
```

### math

| 函数 / 常量 | 签名 | 说明 |
| --- | --- | --- |
| `pi` `e` | 常量 | 圆周率、自然常数（float） |
| `pow(x, y)` | `(float, float) → float` | 幂运算 x^y |
| `sqrt(x)` | `→ float` | 平方根（不 import 也能用） |
| `floor(x)` / `ceil(x)` / `round(x)` / `trunc(x)` | `→ float` | 取整（向下 / 向上 / 四舍五入 / 向零） |
| `sin` `cos` `tan` | `→ float` | 三角函数（弧度） |
| `asin` `acos` `atan` | `→ float` | 反三角函数 |
| `atan2(y, x)` | `(float, float) → float` | 四象限反正切 |
| `fmod(a, b)` | `(float, float) → float` | 浮点取模 |
| `hypot(a, b)` | `(float, float) → float` | sqrt(a²+b²) |
| `log(x)` / `log10(x)` / `exp(x)` | `→ float` | 自然对数 / 常用对数 / e^x |
| `isnan(x)` / `isinf(x)` | `→ bool` | 是否 NaN / 无穷 |
| `min(a, b)` / `max(a, b)` | `int/float → 同类型` | 最小值 / 最大值 |
| `abs(x)` | `int/float → 同类型` | 绝对值（不 import 也能用） |
| `random()` | `→ float` | [0,1) 均匀分布随机数 |
| `seed(n)` | `int → void` | 设置随机数种子 |

### time

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `now()` | `→ float` | 当前 Unix 时间戳（秒，带小数） |
| `monotonic()` | `→ float` | 单调时钟（不受系统校时影响，测耗时用） |
| `sleep(sec)` | `float → void` | 休眠指定秒数 |
| `sleep_ms(ms)` | `int → void` | 休眠指定毫秒数 |

### system

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `system(cmd)` | `string → int` | 执行 shell 命令，返回退出码 |
| `env(name)` | `string → string` | 读环境变量，不存在返回 `""` |
| `setenv(name, value)` | `(string, string) → bool` | 设置环境变量 |

### file

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `read(path)` | `string → string?` | 读入整个文件（**0.8**：失败返回 `none`；`read!` 为 panic 版） |
| `write(path, data)` | `(string, string) → bool` | 覆盖写文件，返回是否成功 |
| `append(path, data)` | `(string, string) → bool` | 追加写文件，返回是否成功 |
| `exists(path)` | `string → bool` | 文件是否存在 |
| `remove(path)` | `string → bool` | 删除文件 |
| `rename(from, to)` | `(string, string) → bool` | 重命名 / 移动文件 |

### string

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `contains(s, sub)` | `(string, string) → bool` | 是否包含子串 |
| `startswith(s, pre)` / `endswith(s, suf)` | `(string, string) → bool` | 前缀 / 后缀判断 |
| `find(s, sub)` | `(string, string) → int` | 子串首次出现位置（字节偏移，找不到 -1） |
| `replace(s, old, new)` | `(string, string, string) → string` | 替换所有出现 |
| `trim(s)` | `string → string` | 去掉首尾空白 |
| `upper(s)` / `lower(s)` | `string → string` | ASCII 大小写转换 |
| `substr(s, start, len)` | `(string, int, int) → string` | 按字节截取（越界自动收拢） |
| `format(fmt, ...)` | `(string, ...) → string` | `{}` 占位符格式化（编译期校验占位符数量） |
| `split(s, sep)` | `(string, string) → string[]` | **0.5 新增**：按分隔符切分成数组（空分隔符会 panic） |
| `chars(s)` | `string → string[]` | **0.5 新增**：拆成单字节字符数组 |
| `join(arr, sep)` | `(string[], string) → string` | **0.5 新增**：用分隔符把字符串数组拼接成一个字符串 |

---

## 内置函数

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `print(...)` | 任意个参数 | 输出，不换行 |
| `println(...)` | 任意个参数 | 输出并换行 |
| `len(s)` | `string/int[]/... → int` | 字符串长度（**返回 UTF-8 字节数**）；0.5 起也接受数组（元素个数） |
| `input()` | `→ string` | 读取一行 |
| `input(prompt)` | `string → string` | 先打印提示语再读取一行 |
| `int(x)` | **0.8**：数值/bool → `int`；string → `int?` | 字符串解析失败返回 `none`（`int!` 为 panic 版） |
| `float(x)` | **0.8**：数值/bool → `float`；string → `float?` | 字符串解析失败返回 `none`（`float!` 为 panic 版） |
| `string(x)` | `→ string` | 从 int / float / bool 转字符串；0.5 起也接受数组（如 `[1, 2]`） |
| `abs(x)` | `int/float → 同类型` | 绝对值 |
| `sqrt(x)` | `→ float` | 平方根 |
| `assert(cond)` | `bool → void` | 断言，失败时打印行号并退出 |
| `assert(cond, msg)` | `(bool, string)` | 带提示语的断言 |
| `exit(code)` | `int → void` | 以指定退出码结束程序 |

---

## 性能

Lux 没有虚拟机、没有 GC、没有装箱 —— 生成的 C 代码里 `int` 就是 `int64_t`，
`string` 就是 `const char*`，所以优化后的机器码和手写 C 没有区别。

用同一套试除法素数算法（统计 2..2,000,000 之间的素数，正确结果 148,933）对比：

| 实现 | 耗时 | 相对速度 |
| --- | --- | --- |
| 手写 C（gcc -O2） | 0.17 s | 1.0× |
| **Lux**（luxc -O2 → gcc -O2） | **0.17 s** | **1.00×（与 C 同速）** |
| Python 3.11 | 3.53 s | 慢 20.8 倍 |

复现方式（脚本会自动构建、校验三方结果一致、各跑 3 次取最快）：

```bash
./bench/run_bench.sh
```

---

## 项目结构

```
lux/
├── Makefile                 构建脚本（版本号经 -DLUX_VERSION 单源注入）
├── logo.svg                 Lux 语言图标（0.7 重绘：一束穿过轨道的 λ）
├── src/
│   ├── lux.hpp              公共定义（Token / AST / 类型表 / 各阶段接口）
│   ├── lexer.cpp            词法分析：源码 → Token 流（错误恢复）
│   ├── parser.cpp           语法分析：Token 流 → AST（错误恢复 + 嵌套深度限制）
│   ├── loader.cpp           模块加载：递归解析 import，模块归属与别名注册
│   ├── pkgs.cpp             包管理：本地 add + 在线 install/search/publish
│   ├── json.cpp             JSON 解析 / 序列化（注册表与 lux.json）
│   ├── sha256.cpp           SHA-256（下载校验 / 发布元数据）
│   ├── sema.cpp             语义分析：作用域、类型检查、struct 字段、常量折叠
│   ├── ty.cpp               类型表：结构等价的类型 intern 化
│   ├── codegen.cpp          C 后端：AST → C 源码（含内联运行时与 struct typedef）
│   ├── native.cpp           原生后端入口（编译运行时库 + 用户程序）
│   ├── native_body.inc      原生后端发射逻辑（与架构无关）
│   ├── native_emit.hpp      x86-64 机器码发射 + ELF 布局
│   ├── native_emit_arm64.hpp aarch64 机器码发射 + ELF 布局
│   ├── native_rt.lux        原生运行时库（Lux 自身编写，构建期嵌入）
│   ├── repl.cpp             交互式 REPL：行编辑 / 历史 / TAB 补全 / struct
│   ├── util.cpp             诊断渲染、文件读写、字符串转义
│   └── main.cpp             luxc 驱动：编译流水线 + luxc build + 包管理子命令
├── examples/                示例程序
├── packages/                官方包源码（mathx / strx / numx / arrx / jsonx）
├── server/                  注册表服务端（index.php 网页 + lux.php API + lib.php + 部署/打包脚本）
├── site/                    lux.xfes.top 站点根首页源码（index.php，动态读注册表）
├── bench/                   性能对比（C / Python 对照实现）
├── tests/
│   ├── cases/               行为测试（.lux + .expected，含切片 / struct / 双后端差分）
│   ├── errors/              诊断测试（.lux + .err 必须编译失败 / .warn 必须警告）
│   └── run_tests.sh         回归测试脚本（行为 + 诊断 + 双后端差分 + REPL + 包管理）
└── docs/
    ├── language.md          语言教程（面向人与 AI，推荐从这里入门）
    ├── grammar.md           完整文法定义（1.0）
    ├── design.md            编译器架构与扩展指南
    ├── stability.md         1.0 稳定性承诺与不包含清单
    └── arc.md               ARC 设计草案与 1.0 状态
```

编译流水线（双后端）：

```
源码(.lux) → Lexer → Parser → Loader(import 展开) → Sema ─┬→ Codegen(C) → C17 → gcc/clang → 可执行文件
                                                           └→ NativeGen → x86-64/aarch64 机器码 + Lux 运行时 → ELF
```

想加新语法需要动哪些地方，见 [docs/design.md](docs/design.md)。

---

## 已知限制

以下几条是**已知且刻意接受的**取舍：

1. **内存由引用计数（ARC）自动回收，0.9.4 起两个后端都默认开启**。字符串
   拼接、数组扩容、**可选值装箱** 与 struct 构造都会分配新内存，ARC 会在
   局部变量作用域出口、赋值、返回、数组增删改、struct 字段与嵌套拼接处
   自动 `retain`/`release`；临时对象在**每条语句结束时**归还。实测 1e6 次
   字符串构造的峰值内存从 ~117 MiB 降到 ~11 MiB。需要退回 0.9.1 的
   「只增不减」语义时用 `--no-arc`（仅用于排查 / 对拍）。仍存的最小缺口
   只泄漏、不误释放（`main(argv)` 的 argv 数组、原生后端提前 `break` /
   `continue` / `return`），见 [docs/arc.md](docs/arc.md) §2.4 与
   [docs/stability.md](docs/stability.md) §6.1。
2. 数组与 struct 都是**引用语义**，两者都不支持 `==` / `!=` 比较
   （引用语义下"相等"有歧义），请逐个比较元素 / 字段。
3. `len()` 返回的是 UTF-8 **字节数**，不是字符数（`"你好"` 的长度是 6）；
   `string` 模块的 `find` / `substr`、for-in 对字符串的迭代、`chars()` 同样按字节计。
4. 空数组字面量 `[]` 需要类型上下文。**0.8 起**作为函数实参可以从形参
   推断元素类型（`f([])` 可用）；但 `let a = [];` 这种无标注写法仍会报错。
5. for-in 迭代数组时使用**进入循环时的长度快照**：循环体内 `push` / `pop`
   不会影响本次遍历（也不会死循环），但请勿在迭代中做结构性修改后依赖遍历结果。
6. 标识符只支持 ASCII 字母 / 数字 / 下划线（源码文件本身是 UTF-8，字符串
   和注释里可以随意用中文）。
7. C 后端依赖系统装有 C 编译器；`--native` 原生后端（0.6）无此依赖，
   按宿主架构直出 x86-64 或 aarch64 机器码（未优化，bench 里约为 C 后端
   2 倍耗时，仍远快于解释执行）。aarch64 下浮点数学函数（`sin`/`log`/
   `exp` 等）由 Lux 运行时实现：`sqrt` / `sin` / `log` / `exp` / `pow` 等
   常规范围与 libm 一致，但 `cos` / `tan` / `asin` / `atan` / `atan2` 在
   **末位** 仍可能不同（C5，待校准）；`format()` 在原生后端要求第一个参数
   是字符串字面量，`-O` 对 `--native` 无效。
8. 只支持 POSIX 平台（Linux / macOS），Windows 请用 WSL / MSYS2。

---

## 路线图

按计划推进，优先级从高到低：

- [x] 多文件与模块系统（`import`、包目录、`extern fn` 调用 C 库）
- [x] 标准库：math / time / system / file / string（`format` 占位符等）
- [x] 模块命名空间与导入别名（`import "x" as y` / `x.member` 限定访问）
- [x] 常量表达式求值（`const X = 2 * 1024 * 1024` 编译期折叠）
- [x] 词法 / 语法错误恢复（一次编译报全所有错误，诊断带错误码）
- [x] 交互式 REPL（行编辑、历史、TAB 补全）
- [x] **数组**：类型（`int[]` / `[int]`）、字面量、下标读写、`push` / `pop` /
  `insert` / `remove` / `clear` 方法、嵌套数组、引用语义（0.5）
- [x] for-in 迭代数组与字符串（0.5）
- [x] `string.split` / `chars` / `join`（0.5）
- [x] 完整的返回路径分析（if-else 双分支 / while(true) / loop 均视为覆盖）（0.5）
- [x] 交互式 REPL 支持下标赋值等新语法（0.5）
- [x] 数组切片 `a[1..3]` / `a[1..=3]`（含端点省略，复制语义，双后端一致）（0.6）
- [x] **struct 结构体**：声明 / 构造（`Point { x: 1 }`）/ 成员读写 / 嵌套 /
  struct 数组 / 打印 / `const` 限制，双后端一致（0.7）
- [x] **if 表达式** `let x = if c { a } else { b };`（0.7）
- [x] **原始字符串** `r"..."` 与 `nan` / `inf` 字面量（0.7）
- [x] **命令符参数** `fn main(argv: string[])`（0.7）
- [x] **复合赋值扩展到左值**：`a[i] += v` / `grid[i][j] = v` / `p.x += v`
  （左值子表达式只求值一次）（0.7）
- [x] 原生后端特权内建参数个数编译期校验（E，0.7）；`format` / `println`
  支持 struct 与 struct 数组（0.7）
- [x] **错误通道**：`T?` 可选类型 + `?` 传播 + `or` 兜底 + `name!` panic 变体；
  `int` / `float` / `read` 语义迁移（破坏性，0.8）
- [x] **C 层欠账**：复合 `/=` `%=` 除零检查、`>>` 统一算术右移、struct 零值、
  `f([])` 空数组实参、C 后端标识符撞名（0.8）
- [x] **原生后端 bug 修复**：aarch64 浮点数组读取、文件路径偏移、大数浮点格式化（0.8）
- [x] **发布工程**：GitHub Actions CI + 双后端差分矩阵 + ASan/UBSan + fuzzing
  冒烟（0.8）
- [x] **ARC 表示层 + C 后端引用计数**（0.9.2，实验性 `--arc`；原生后端与
  默认开启在 0.9.4，见 [docs/arc.md](docs/arc.md)）
- [x] **语言教程 `docs/language.md`**（面向人与 AI，0.9.2）
- [x] **注册表加固**：令牌散列存储 + 注册/上传速率限制（0.9.2）
- [x] **ARC 正确性**：折叠字符串常量无头指针修复、打印临时串回收、原生后端
  未初始化 string 修复（0.9.3）
- [x] **项目构建 `luxc build` + LuxBuildFile**（0.9.3）
- [x] **导入系统**：`from … import …`、`#import` 与 `#define`/`#ifdef` 预处理
  （0.9.3）
- [x] **自带库扩展**：mathx / strx 增补，新增 arrx（0.9.3）
- [x] **原生后端 ARC（free list）+ `T?` 装箱回收 + ARC 双后端默认开启**
  （0.9.4，新增 `--no-arc` 逃生阀）
- [x] **错误消息携带**：`last_error()` 与 `find_opt()` 可选变体（0.9.4）
- [x] **Lux 1.0.0 正式版**：语言 / 标准库 / 错误通道 / 构建 / 包管理 / ARC
  全部冻结（1.0）
- [ ] 高阶函数 / `fn` 类型与 `sort` / `map` / `filter`（不进 1.0，留 1.x）
- [ ] aarch64 原生数学末位对齐（C5）
- [ ] 标准库扩展：`net` 模块、文件 IO 增强
- [x] 原生代码生成后端：`--native` 直出 Linux ELF（x86-64 与 aarch64），运行时
  由 Lux 自身实现（约 100 个 `luxrt_*` 函数），不依赖 C 编译器与 libc（0.6）
- [ ] 枚举与模式匹配
- [ ] 泛型

### 1.0 不包含（提前声明，管理预期）

泛型、`map<K,V>`、无符号整数、并发、Windows、LLVM 后端。此外，`fn` 类型 /
高阶函数确定不进 1.0（1.x 首项）；ARC 双后端默认开启已在 0.9.4 兑现，
1.0 不再改动（见 [docs/stability.md](docs/stability.md) §6）。POSIX-only
是刻意选择。

---

## 许可

MIT
