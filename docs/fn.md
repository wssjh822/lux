# Lux 1.1 设计草案：函数成为值（`fn` 类型）

> 状态：**1.1.0 已落地**。本文拍死三条边界（无闭包、`map_opt` 签名、
> `sort` 方法化），实现按本文执行。格式沿用 `arc.md`。

## 1. 范围

1.1 只做**具名函数的引用**，不做闭包 / lambda / 捕获：

```lux
fn apply(op: fn(int, int) -> int, a: int, b: int) -> int {
    return op(a, b);
}
fn add(x: int, y: int) -> int { return x + y; }

fn main() {
    println(apply(add, 3, 4));   // 7
    let f: fn(int, int) -> int = add;
    println(f(10, 20));          // 30
}
```

### 表示层：8 字节地址槽

- **C 后端** = C 函数指针（`RET (*)(ARGS)` 的 typedef）。
- **原生后端** = 函数代码地址 + 间接调用（`call r/m` / `blr`）。
- **不产生堆对象**，因此**完全不碰 ARC**（闭包对象才需要计数，本版不做）。

### 约束

- **`extern fn` 不能作为值**：原生后端没有 C 符号，取地址无从谈起。
- 函数值只能引用**具名函数**，不能内联定义（没有 lambda）。
- `get()(x)`（对"返回函数的调用结果"再调用）**不支持**；先 `let g = get(); g(x);`。
- `fn` 类型可作：局部变量、参数、返回值。**不保证**作数组元素（1.1 未列入）。

## 2. 类型与语法

新增产生式（见 `docs/grammar.md`）：

```
fnType := 'fn' '(' [ type (',' type)* ] ')' '->' type
```

类型表里 `TyKind::Fn`（`elem` = 返回类型，`members` = 参数类型）与
`TyStore::fnOf` 早已就位，1.1 只是把它接到语法、语义与两个后端。

## 3. 无泛型下的三件套

| 名字 | 形态 | 说明 |
| --- | --- | --- |
| `sort` | **方法** `a.sort(cmp)` | 原地排序；按 D1「改接收者就用方法」。运行时按元素类型泛化（memcpy 交换 + 比较器） |
| `map` | 裸函数 `map(a, f)` | 调用点展开循环，输出数组的元素类型由 `f` 的返回类型在编译期确定 |
| `filter` | 裸函数 `filter(a, f)` | 条件 `f` 返回 `bool`，保留为真的元素 |
| `map_opt` | `map_opt(arr: T[], f: fn(T) -> R?) -> R[]?` | **任一元素失败整体返回 `none`**（Rust collect 风格）；`R[]?` 是合法类型，与 `?` 传播自然组合，绕开「`T?` 不能作数组元素」 |

命名遵循 D1：`map` / `filter` 不改接收者 → 裸函数；`sort` 改接收者 → 方法。

## 4. 顺路捎上

- **字节原语**：`byte_at(s, i) -> int`、`bytes(s) -> int[]`。jsonx 的
  `BYTE_MASTER` 巨表就是动机证明。
- **原生 `break` / `continue` / `return` 泄漏修复**：提前离开块时释放该块
  已声明的引用型局部（`arc.md` §2.4 第二条）。`main(argv)` 的一次性泄漏继续挂账。
- **`list_dir(path) -> string[]?`**：`file` 模块的目录列举；原生后端包
  `getdents64`。

## 5. 明确不进 1.1

闭包 / lambda（1.2+）、`map<K,V>` / 泛型、解禁 `T?[]`、switch/match、并发。

## 6. 落地顺序

1. parser 加 `fnType`；sema 用 hint 通道识别具名函数 + 间接调用分支。
2. C 后端：`fn` 类型 → typedef；函数名取值；间接调用。
3. 原生后端：函数地址 + `call r/m` / `blr`。
4. `sort` / `map` / `filter` / `map_opt` + 字节原语 + `list_dir`。
5. 原生提前离开块的引用释放。
6. 测试：错误用例（对非函数取值 / `fn` 当 `int` 用 / 参数个数不匹配）+
   三件套行为用例 + **双后端逐字节差分覆盖间接调用**。
