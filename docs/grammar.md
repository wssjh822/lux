# Lux 文法定义

本文法描述 Lux 0.9 的完整语法（涵盖 0.9.x；1.0 只冻结、不再新增语法）。记号沿用扩展
巴科斯范式（EBNF）：`*` 表示重复零次或多次，`?` 表示可选，`|` 表示多选一，
终结符用引号括起。

---

## 1. 词法结构

### 1.1 空白与注释

空白（空格、`\t`、`\r`、`\n`）在词法层面被跳过，不作为语法的一部分。

```ebnf
lineComment  := '//' 任意字符* 换行
blockComment := '/*' (块注释 | 任意字符)* '*/'      // 支持嵌套
```

### 1.2 标识符

```ebnf
ident := ('a'..'z' | 'A'..'Z' | '_') ('a'..'z' | 'A'..'Z' | '0'..'9' | '_')*
```

标识符区分大小写，**只支持 ASCII 字母、数字与下划线**（源码文件是 UTF-8，
字符串与注释里可以随意使用非 ASCII 字符）。

### 1.3 关键字

```
fn      let     const   return  if      else
elif    while   for     loop    in      break
continue        repeat  struct
true    false   and     or      not     nan     inf     none
import  extern  as
int     float   bool    string  void
```

（`from` 是上下文关键字，不作为保留字；见 §2.1 选择性导入。）

`elif` 是 `else if` 的别名；`repeat n {}` 是 `for` 区间的语法糖；
`struct` 声明具名类型（0.7）；`nan` / `inf` 是浮点字面量（0.7）；
`none` 是可选类型的空值字面量（0.8）；`import` 与 `extern` 只能在顶层出现；
`as` 用于给 import 起别名。

### 1.4 整数字面量

```ebnf
decLiteral  := 数字 (数字 | '_')*
hexLiteral  := '0' [xX] 十六进制数字 (十六进制数字 | '_')*
binLiteral  := '0' [bB] 二进制数字   (二进制数字   | '_')*
octLiteral  := '0' [oO] 八进制数字   (八进制数字   | '_')*
intLiteral  := decLiteral | hexLiteral | binLiteral | octLiteral
```

下划线只起分隔作用，解析时被忽略。所有整数字面量都按 64 位有符号整数处理，
超出范围会在编译期报错。

### 1.5 浮点字面量

```ebnf
floatLiteral := 数字 (数字|'_')* '.' 数字 (数字|'_')* 指数部分?
              | 数字 (数字|'_')* 指数部分
指数部分      := [eE] ['+'|'-'] 数字 (数字|'_')*
```

注意 `0..10` 中的 `0` 不会被解析成浮点数：小数点后必须紧跟数字才会被当作小数部分。
`1.` 会被解析成整数 `1` 后跟一个 `.`。
`nan` / `inf` 是关键字形式的浮点字面量（`-inf` 由一元负号构造，0.7）。

### 1.6 字符串字面量

```ebnf
stringLiteral := '"' (普通字符 | 转义序列)* '"'
转义序列       := '\' ( 'n' | 't' | 'r' | '0' | 'a' | 'b' | 'f' | 'v' | 'e'
                      | '\\' | '"' | '\''
                      | 'x' 十六进制数字×2
                      | 'u' 十六进制数字×4
                      | 'U' 十六进制数字×8 )
```

字符串是 UTF-8 编码，不允许跨行。`\xNN` 写出的是**码点**而非原始字节：
`"\xFF"` 编码成两字节 `C3 BF`，不是单字节 `0xFF`。

原始字符串（0.7）不做任何转义处理，内容原样保留：

```ebnf
rawStringLiteral := 'r' '"' 任意字符* '"'
```

### 1.7 运算符与分隔符

```
(  )  {  }  [  ]  ,  ;  :  .
-> .. ..= +  -  *  /  %  =
== != <  <= >  >=
&& || !  &  |  ^  ~  << >>
++ -- += -= *= /= %= &= |= ^= <<= >>=
?  !(后缀)
```

`[` `]` 从 0.5 起用于数组：类型写法（`[int]` / `int[]`）、数组字面量
（`[1, 2, 3]`）与下标访问（`a[i]`）。
`.` 自 0.7 起同时用于 struct 成员访问（`p.x`），模块限定访问（`mod.fn`）保持不变。
`++` / `--` 只能以语句形式出现（`x++;`）。复合赋值（`+=` 等）为独立左值语义，
左值子表达式只求值一次（0.7 起可用于下标 / 成员左值）。

`?`（0.8）有且只有两个固定位置：类型后缀 `T?`（可选类型）与表达式后缀
`expr?`（错误传播）。`!`（0.8）额外用作函数调用后缀：`name!(...)` 是
panic 变体（见 §8）；前缀 `!` 仍然是一元逻辑非，二者不冲突。

---

## 2. 语法结构

### 2.1 程序与顶层声明

```ebnf
program     := topLevel*
topLevel    := importDecl | externDecl | funcDecl | globalConst | structDecl

importDecl  := 'import' stringLiteral ( 'as' IDENT )? ';'
             | 'from' stringLiteral 'import' ( '*' | IDENT ( ',' IDENT )* ) ';'   // 0.9.3
             | '#import' stringLiteral ';'?                                       // 0.9.3（预处理）
externDecl  := 'extern' 'fn' IDENT '(' paramList? ')' ( '->' type )? ';'

structDecl  := 'struct' IDENT '{' fieldDecl* '}'          // 0.7
fieldDecl   := ( 'let' | 'const' )? IDENT ':' type ';'

globalConst := 'const' IDENT ( ':' type )? '=' constExpr ';'
constExpr   := literal
             | constExpr 二元运算符 constExpr       // 编译期常量折叠
             | IDENT                               // 引用已声明的全局常量
literal     := intLiteral | floatLiteral | stringLiteral | 'true' | 'false'
             | 'nan' | 'inf'

funcDecl    := 'fn' IDENT '(' paramList? ')' ( '->' type )? block
paramList   := param ( ',' param )*
param       := IDENT ':' type
type        := '[' type ']'                 // 前缀写法 [T]（单层）
             | baseType suffix*
suffix      := '[' ']' | '?'                // 数组 / 可选，从左到右依次绑定
baseType    := 'int' | 'float' | 'bool' | 'string' | 'void'
             | IDENT                       // struct 具名类型（0.7）
```

`void` 只能作为函数返回类型，不能出现在变量 / 参数 / 数组元素位置。
数组的前缀写法 `[T]` 与后缀写法 `T[]` 等价，`[]` 可以叠加以构造嵌套数组。
`?`（0.8）把类型变成可选类型 `T?`，可嵌套（`int??`）；`T?` 不能作为数组元素
类型：`int?[]` 会被拒绝，想要“可能为 none 的数组”请写 `int[]?`（后缀从左到右
依次绑定：`int[]?` 是“可选数组”，而 `int?[]` 是非法数组元素类型）。

全局常量的初始值必须是**常量表达式**（字面量、字面量之间的算术 / 位运算 /
字符串拼接，或引用之前声明的全局常量）—— 它需要编译成 C 的 `static const`，
编译期就折叠成字面量。函数体内则没有这个限制。

`import` 的路径解析（由 loader 负责，见 README）：

| 形态 | 示例 | 含义 |
| --- | --- | --- |
| 标准库模块 | `"math"` `"time"` `"system"` `"file"` `"string"` | 启用对应模块的内置函数 |
| C 库 | `"c:m"` | 链接 `-lm`，配合 `extern fn` 调用 |
| 相对路径文件 | `"./util.lux"` `"util.lux"` | 合并该文件的顶层声明 |
| 包目录 | `"pkg"` | 读 `pkg/lux.json`（或 `main.lux` / `lib.lux`） |
| 已安装包 | `"pkg"` | 以上都找不到时，回退到 `~/.lux/packages/` |

不带 `as` 的 import 把成员注入当前命名空间；带 `as 别名` 时只能通过
`别名.成员` 访问。任何已 import 的模块都支持 `模块名.成员` 限定访问。
同一文件只加载一次，循环 import 安全。`extern fn` 只声明 C 函数
（不生成函数体），生成的 C 里直接引用原符号名。

**选择性导入（0.9.3）**：`from "mod" import a, b;` 只把 `a`、`b` 注入当前
命名空间，模块里的其他成员不会被裸名访问（但仍可用 `mod.other` 限定访问）。
`from "mod" import *;` 等价于默认导入。`from` 是上下文关键字：它只在顶层
`from "路径" import` 形态里生效，仍可作为普通标识符使用。

**预处理指令（0.9.3）**：在词法分析前逐行处理 `#` 开头的指令，行号保持不变。
支持 `#import` / `#include`（等价于 `import`）、`#define NAME [值]`（对象式宏）、
`#undef`、`#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`、`#error`。
宏在普通代码行里做整词展开（跳过字符串字面量与 `//` 注释）；`#if` 支持整数、
`defined(NAME)`、`!` / `&&` / `||` 与括号。

### 2.2 语句

```ebnf
block       := '{' stmt* '}'

stmt        := letStmt
             | ifStmt
             | whileStmt
             | forStmt
             | repeatStmt
             | loopStmt
             | breakStmt
             | continueStmt
             | returnStmt
             | block              // 裸代码块，用于限制作用域
             | assignStmt
             | exprStmt
             | ';'                // 空语句

letStmt     := ('let' | 'const') IDENT ( ':' type )? ( '=' expr )? ';'
assignStmt  := lvalue ('=' | '+=' | '-=' | '*=' | '/=' | '%='
                    | '&=' | '|=' | '^=' | '<<=' | '>>=') expr ';'   // 0.7 通用左值
             | IDENT ('++' | '--') ';'
lvalue      := IDENT
             | lvalue '[' expr ']'      // a[i] / grid[i][j]
             | lvalue '.' IDENT         // p.x / l.a.x
exprStmt    := expr ';'

ifStmt      := 'if' expr block ( ('else' | 'elif') ( block | ifStmt ) )?
whileStmt   := 'while' expr block
forStmt     := 'for' IDENT 'in' expr ( '..' | '..=' ) expr block   // 区间
             | 'for' IDENT 'in' expr block                          // 0.5 for-in
repeatStmt  := 'repeat' expr block        // 语法糖，等价于 for _ in 0..expr
loopStmt    := 'loop' block
breakStmt   := 'break' ';'
continueStmt:= 'continue' ';'
returnStmt  := 'return' expr? ';'
```

说明：

- `for` 的循环变量作用域**仅限循环体**。区间形式下类型是 `int`；for-in
  形式下类型是被迭代数组的**元素类型**（或迭代字符串时的 `string`，逐字节单字符）。
- for-in 只接受数组或字符串；需要计数时用区间形式或自行维护下标。
- for-in 对数组使用进入循环时的长度快照，循环体内 push/pop 不会改变本次遍历次数。
- `0..10` 是左闭右开（10 次），`0..=10` 是闭区间（11 次）。
- 区间终点在循环开始时求值一次，之后不再重复计算。
- `else` 后面可以直接跟 `if`，形成 `else if` 链；`elif` 与之等价。
- 复合赋值 `lhs op= e` 在代码生成时读取旧值再写回，**左值子表达式只求值一次**
  （`a[f()] += 1` 中 `f()` 只调用一次）；`x++` / `x--` 等价于 `x = x + 1` /
  `x = x - 1`，只能作为独立语句出现，没有"后缀表达式的值"这一语义。
- 赋值目标是任意左值：变量 `x`、数组元素 `a[i]` / `grid[i][j]`、
  struct 成员 `p.x` / `l.a.x`，以及它们的组合 `arr[i].x`（0.7）。
  `const` 变量 / 数组 / struct 不可写；字符串不可变（不能用 `s[i] = ...`）。
- `repeat n { ... }` 的 `n` 只求值一次，必须是 `int`。

### 2.3 表达式

```ebnf
expr        := orExpr

orExpr      := andExpr  ( ('||' | 'or')  andExpr  )*
andExpr     := bitOr    ( ('&&' | 'and') bitOr    )*
bitOr       := bitXor   ( '|'  bitXor   )*
bitXor      := bitAnd   ( '^'  bitAnd   )*
bitAnd      := equality ( '&'  equality )*
equality    := relational ( ('==' | '!=') relational )*
relational  := shift    ( ('<' | '<=' | '>' | '>=') shift )*
shift       := additive ( ('<<' | '>>') additive )*
additive    := multiplicative ( ('+' | '-') multiplicative )*
multiplicative := unary ( ('*' | '/' | '%') unary )*
unary       := ('-' | '!' | 'not' | '~') unary | postfix

postfix     := primary ( postfixSuffix )*
postfixSuffix := '[' expr ']'                       // 0.5 下标：a[i]、m[i][j]、s[i]
               | '[' expr? ('..' | '..=') expr? ']' // 0.6 切片：a[lo..hi]、a[lo..=hi]，
                                                    // 端点可省略（a[..hi] / a[lo..] / a[..]）
               | '.' IDENT                          // 0.7 struct 成员：p.x、a[i].x
               | '?'                                // 0.8 错误传播：expr?

primary     := intLiteral
             | floatLiteral
             | stringLiteral
             | rawStringLiteral
             | 'true' | 'false' | 'nan' | 'inf' | 'none'   // none：0.8 可选空值
             | '[' argList? ','? ']'          // 0.5 数组字面量（支持尾逗号）
             | IDENT '{' fieldInit ( ',' fieldInit )* ','? '}'  // 0.7 struct 字面量
             | 'if' expr '{' expr '}' 'else' '{' expr '}'      // 0.7 if 表达式
             | IDENT ( '.' IDENT )+           // 成员访问 / 模块限定访问
             | IDENT
             | IDENT ( '.' IDENT ) '(' argList? ')'  // 0.5 数组方法 a.push(x) 等
             | IDENT ( '.' IDENT )? '!' '(' argList? ')'  // 0.8 panic 变体 f!(...) / read!(...)
             | ('int' | 'float' | 'string') ( '!' )? '(' expr ')'   // 类型转换（int! 为 panic 版）
             | '(' expr ')'

fieldInit   := IDENT ':' expr
argList     := expr ( ',' expr )*
```

`a.b` 形式根据上下文解析：若 `a` 是已导入模块名且 `b` 是模块成员，则为模块
限定访问（`math.pow` / `m.pi` / `模块.常量`）；否则为 struct 成员访问（0.7，
`p.x`）。作为赋值目标时，成员链必须是可写左值（非 `const`）。

`P { x: 1, y: 2 }` 是 struct 字面量（0.7）：字段名必须全部给出且不重复，
顺序任意。为避免与语句块冲突，在 `if` / `while` / `for` 的条件位置不解析
struct 字面量（`while x { ... }` 中的 `x` 是普通标识符）。

`if` 表达式（0.7）两个分支各是一个表达式且必须有 `else`；分支类型需相容
（`int` / `float` 混合时提升为 `float`）。它不是语句位置 `if` 的替代，
只是多了一种可赋值的形态。

数组字面量在元素类型无法统一推断时报错；空数组 `[]` 需要从上下文
（let 标注 / 赋值目标 / return 类型）获得元素类型，否则报错。
数组方法（`push` / `pop` / `insert` / `remove` / `clear`）只能通过
`变量.方法(参数)` 形式调用，接收者必须是可见的数组变量。

---

## 3. 运算符优先级表

从**低到高**（同一行优先级相同）：

| 级别 | 运算符 | 结合性 | 操作数类型 | 结果类型 |
| --- | --- | --- | --- | --- |
| 1 | `\|\|` `or` | 左 | bool, bool | bool |
| 2 | `&&` `and` | 左 | bool, bool | bool |
| 3 | `\|` | 左 | int, int | int |
| 4 | `^` | 左 | int, int | int |
| 5 | `&` | 左 | int, int | int |
| 6 | `==` `!=` | 左 | 同类型（或 int/float 混合） | bool |
| 7 | `<` `<=` `>` `>=` | 左 | 数值，或 string | bool |
| 8 | `<<` `>>` | 左 | int, int | int |
| 9 | `+` `-` | 左 | 数值，或 string+string | 数值 / string |
| 10 | `*` `/` `%` | 左 | 数值（`%` 仅 int） | 数值 |
| 11 | `-` `!` `~` `not`（一元） | 右 | int/float、bool、int | 同类型 / bool |
| 12 | `(` `)` 分组、函数调用 | — | — | — |
| 13 | `a[i]`（后缀下标） | 左 | 数组×int → 元素类型；string×int → string | 元素类型 / string |
| 13 | `p.x`（后缀成员，0.7） | 左 | struct → 字段类型 | 字段类型 |
| 13 | `expr?`（后缀传播，0.8） | 左 | `T?` → `T`（所在函数须返回 `U?`） | `T` |
| 13 | `f!(...)`（panic 变体，0.8） | 左 | `T?` 函数 → `T` | `T` |

数值混合规则：`int` 和 `float` 参与同一个运算时，`int` 自动提升为 `float`，结果是 `float`。
移位 `<<` / `>>` 的位移量**按 64 取模**（`x << 64` 等价于 `x << 0`，负数位移按补码取模），
两个后端一致；不要依赖“位移超宽就归零”的直觉。

**`or` 的分派规则（0.8，A3.1 方案 a）**：`or` 关键字按**左操作数类型**分派——
左操作数为 `bool` 时按级别 1 的逻辑或解释；为 `T?` 时按兜底解释（失败取右侧，
右侧需可转换为 `T`，结果类型为 `T`）。`||` 永远是逻辑或，不参与分派。

`or` 是左结合的，而 `a? or b?` 的结果类型是 `T`，所以 **`or` 不能链式兜底**：
`a? or b? or c` 里第二个 `or` 的左侧已是 `T` 而不是 `T?`，会被当成逻辑或而报错。
需要多层兜底时请嵌套：`a? or (b? or c)`，或显示解包后再判断。

复合赋值（`+=` 等）与 `++` / `--` 不是表达式运算符，只能出现在语句位置：
`lhs op= e` 按上表取对应运算的优先级与类型规则，且 `lhs` 只求值一次；
`x++` / `x--` 等价于 `x = x ± 1`。

---

## 4. 类型规则

### 4.1 隐式转换

只有两条：`int → float`；`T → T?`（0.8 自动装箱）。其余转换必须显式写出。
特别地 **`T? → T` 不是隐式转换**：必须用 `?` 传播、`or` 兜底或 `!` panic 解包，
否则错误通道会泄漏。

### 4.2 各运算符的类型约束

| 运算符 | 左边 | 右边 | 结果 |
| --- | --- | --- | --- |
| `+` | int/float | int/float | 提升后的类型 |
| `+` | string | string | string（拼接） |
| `-` `*` `/` | int/float | int/float | 提升后的类型 |
| `%` | int | int | int |
| `<` `<=` `>` `>=` | 数值 或 string | 同左 | bool |
| `==` `!=` | 同类型，或数值混合 | 同左 | bool（**数组操作数被禁止；`T?` 也被禁止**，需先用 `?` / `or` / `!` 解包） |
| `&&` `\|\|` | bool | bool | bool |
| `&` `\|` `^` `<<` `>>` | int | int | int |
| 一元 `-` | int/float | — | 同类型 |
| 一元 `!` / `not` | bool | — | bool |
| 一元 `~` | int | — | int |

### 4.3 变量声明的类型确定

| 写法 | 结果 |
| --- | --- |
| `let x: int = 5;` | `int`，检查初始值能否转换 |
| `let x = 5;` | 推断为 `int` |
| `let x: int;` | `int`，初始化为 `0` |
| `let x;` | **错误**：无法推断类型 |
| `let a: int[] = [];` | `int[]`，空数组从标注推断元素类型 |
| `let a = [1, 2.5];` | `float[]`（int/float 混合提升） |
| `let a = [];` | **错误**：空数组无类型上下文，无法推断元素类型 |
| `let a = [1, "x"];` | **错误**：元素类型不一致 |

### 4.4 数组（0.5 新增）

- **元素类型一致**：数组字面量所有元素必须同类型；`int` 元素在 `float[]`
  目标下自动提升。
- **引用语义**：数组赋值 / 传参 / 返回不拷贝内容，全部指向同一个数组；
  下标读写与 `push` 等方法对所有引用可见。
- **协变不允许**：`int[]` 与 `float[]` 是不同类型，互不赋值（字面量除外，
  见 4.3 的提升规则）。
- **禁止的操作**：`==` / `!=` 与关系比较；作为全局常量；字符串元素下标赋值。
- **越界即 panic**：下标访问 / 赋值 / `insert` / `remove` 越界立即终止并
  打印下标与长度。

---

## 5. 作用域规则

- 参数、局部变量的作用域从声明处延伸到所在代码块的结尾。
- 内层代码块可以声明与外层同名的变量（遮蔽 / shadowing）。
- 同一作用域内不允许重复声明同名变量。
- `for` 的循环变量作用域只有循环体。
- 顶层全局常量对所有函数可见。
- `break` / `continue` 必须出现在 `while` / `for` / `loop` 内部。

---

## 6. 程序入口

程序必须包含一个 `fn main()`：

- 参数个数为 0，或恰好一个 `argv: string[]`（0.7）；
- 返回类型只能是 `void` 或 `int`（`int` 会作为进程的退出码）。

带 `argv` 时，`argv[0]` 是程序名（与 C 约定一致），后续为命令行参数。

---

## 7. struct 类型规则（0.7）

- 声明必须在使用之前（或同一程序内前向声明均可，Sema 先收集后解析）。
- struct 名**全局唯一**（跨模块不允许重名）。
- 字段类型可为 `int` / `float` / `bool` / `string` / 数组 / 另一个 struct。
- **引用语义**：与数组一致，赋值 / 传参 / 返回共享同一字段对象。
- 构造时字段必须全部给出且不重复；未知字段报错。
- 字段访问链必须是左值才能赋值；`const` struct 不可修改字段。
- struct 不支持 `==` / `!=`；可作为数组元素、嵌套、打印。
- 不支持无初始化的 struct 变量声明（`let p: Point;` 报错；需要空值请写 `Point?`）。

---

## 8. 错误通道（0.8）

### 8.1 类型

```ebnf
optionalType := type '?'        // int? / string? / Point? / int??（可嵌套）
```

- `T?` 表示“可能缺失 / 失败的 `T`”。表示层统一为**指向堆槽的指针，`none` = 空指针**；
  两个后端都用单个 8 字节槽传递。`T?` 不能作为数组元素类型。
- `T` 可以隐式转成 `T?`（自动装箱）；`T?` **不能**隐式转回 `T`。
- `none` 是 `T?` 的空值字面量，类型完全由上下文决定；没有可选上下文时报错。
- `return;`（不带值）在返回类型为 `T?` 的函数里等价于 `return none;`。

### 8.2 三种解包方式

| 形式 | 语义 | 适用场景 |
| --- | --- | --- |
| `expr?` | 传播：`expr` 为 `none` 时当前函数立即返回 `none`；成功时得到 `T` | 库代码 / 调用链上抛 |
| `lhs or rhs` | 兜底：`lhs` 为 `none` 时取 `rhs`（`rhs` 需能转成 `T`） | 需要一个默认值 |
| `name!(...)` / `f!(...)` | panic：为 `none` 时打印错误并退出（脚本友好） | 脚本 / 确定不会失败 |

约束：
- `expr?` 的 `expr` 必须是 `T?`，且**所在函数的返回类型必须是 `U?`**（否则报定向错误）。
- `or` 的左侧必须是 `T?`（否则按逻辑或解释，要求两侧 `bool`）。
- `!` 后缀只能用于可能失败的内建（`int` / `float` / `read`）或返回 `T?` 的用户函数。

### 8.3 标准库中的错误通道

| 函数 | 签名 | 失败行为 |
| --- | --- | --- |
| `int(x)` | 数值/bool → `int`；string → `int?` | 解析失败返回 `none` |
| `float(x)` | 数值/bool → `float`；string → `float?` | 解析失败返回 `none` |
| `read(path)` | → `string?` | 打开失败返回 `none` |
| `int!` / `float!` / `read!` | 对应 panic 变体 | 失败即 panic（退出码 1） |

其余标准库函数保持原值语义（`write` / `append` / `remove` / `rename` / `exists`
返回 `bool`，`system` 返回退出码，`env` 缺失返回 `""`）。

### 8.4 打印

`print` / `println` / `string` / `format` 直接接受 `T?`，输出 `some(x)` 或 `none`。
