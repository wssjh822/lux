# Lux 文法定义

本文法描述 Lux 0.7 的完整语法。记号沿用扩展巴科斯范式（EBNF）：
`*` 表示重复零次或多次，`?` 表示可选，`|` 表示多选一，终结符用引号括起。

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
true    false   and     or      not     nan     inf
import  extern  as
int     float   bool    string  void
```

`elif` 是 `else if` 的别名；`repeat n {}` 是 `for` 区间的语法糖；
`struct` 声明具名类型（0.7）；`nan` / `inf` 是浮点字面量（0.7）；
`import` 与 `extern` 只能在顶层出现；`as` 用于给 import 起别名。

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

字符串是 UTF-8 编码，不允许跨行。

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
```

`[` `]` 从 0.5 起用于数组：类型写法（`[int]` / `int[]`）、数组字面量
（`[1, 2, 3]`）与下标访问（`a[i]`）。
`.` 自 0.7 起同时用于 struct 成员访问（`p.x`），模块限定访问（`mod.fn`）保持不变。
`++` / `--` 只能以语句形式出现（`x++;`）。复合赋值（`+=` 等）为独立左值语义，
左值子表达式只求值一次（0.7 起可用于下标 / 成员左值）。

---

## 2. 语法结构

### 2.1 程序与顶层声明

```ebnf
program     := topLevel*
topLevel    := importDecl | externDecl | funcDecl | globalConst | structDecl

importDecl  := 'import' stringLiteral ( 'as' IDENT )? ';'
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
type        := baseType ( '[]' )*          // int[]、int[][]（后缀写法）
             | ( '[]' )* baseType ( '[]' )*  // [int]、[[int]]（前缀写法，可混用）
baseType    := 'int' | 'float' | 'bool' | 'string' | 'void'
             | IDENT                       // struct 具名类型（0.7）
```

`void` 只能作为函数返回类型，不能出现在变量 / 参数 / 数组元素位置。
数组的前缀写法 `[T]` 与后缀写法 `T[]` 完全等价，`[]` 可以叠加以构造嵌套数组。

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

primary     := intLiteral
             | floatLiteral
             | stringLiteral
             | rawStringLiteral
             | 'true' | 'false' | 'nan' | 'inf'
             | '[' argList? ','? ']'          // 0.5 数组字面量（支持尾逗号）
             | IDENT '{' fieldInit ( ',' fieldInit )* ','? '}'  // 0.7 struct 字面量
             | 'if' expr '{' expr '}' 'else' '{' expr '}'      // 0.7 if 表达式
             | IDENT ( '.' IDENT )+           // 成员访问 / 模块限定访问
             | IDENT
             | IDENT ( '.' IDENT ) '(' argList? ')'  // 0.5 数组方法 a.push(x) 等
             | IDENT '(' argList? ')'         // 函数调用
             | ('int' | 'float' | 'string') '(' expr ')'   // 类型转换
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

数值混合规则：`int` 和 `float` 参与同一个运算时，`int` 自动提升为 `float`，结果是 `float`。

复合赋值（`+=` 等）与 `++` / `--` 不是表达式运算符，只能出现在语句位置：
`lhs op= e` 按上表取对应运算的优先级与类型规则，且 `lhs` 只求值一次；
`x++` / `x--` 等价于 `x = x ± 1`。

---

## 4. 类型规则

### 4.1 隐式转换

只有一条：`int → float`。其余转换必须显式写出。

### 4.2 各运算符的类型约束

| 运算符 | 左边 | 右边 | 结果 |
| --- | --- | --- | --- |
| `+` | int/float | int/float | 提升后的类型 |
| `+` | string | string | string（拼接） |
| `-` `*` `/` | int/float | int/float | 提升后的类型 |
| `%` | int | int | int |
| `<` `<=` `>` `>=` | 数值 或 string | 同左 | bool |
| `==` `!=` | 同类型，或数值混合 | 同左 | bool（**数组操作数被禁止**） |
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
