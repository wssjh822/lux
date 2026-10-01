# jsonx — Lux JSON 库

纯 Lux 实现的 JSON 解析 / 生成 / 文件读写，无外部依赖，C 后端与原生后端
（`--native`）行为一致。

```bash
luxc install jsonx
```

```lux
import "jsonx";

fn main() {
    let r = parse("{\"name\":\"lux\",\"stars\":[1,2,3],\"ok\":true}");
    if !r.ok {
        println("JSON 错误：" + r.error + " @" + string(r.line) + ":" + string(r.col));
        return;
    }
    let j = r.value;
    println(getString(j, "name"));            // lux
    println(asInt(getPath(j, "stars[0]")));   // 1
    println(pretty(j));
}
```

## API

### 解析
| 函数 | 说明 |
| --- | --- |
| `parse(text) -> JResult` | 解析文本；`ok` / `value` / `error` / `line` / `col` |
| `decode(text) -> Json?` | 成功返回 `some`，失败 `none` |
| `decodeOr(text, fallback) -> Json` | 失败用 fallback |
| `parseOr(text, fallback) -> Json` | 同 `decodeOr` |
| `readJson(path) -> JResult` | 读文件并解析 |
| `readJsonOr(path, fallback) -> Json` | 失败用 fallback |

### 生成
| 函数 | 说明 |
| --- | --- |
| `stringify(j) -> string` | 紧凑 JSON |
| `pretty(j) -> string` | 两空格缩进 |
| `quote(s) -> string` | 字符串转义成 JSON 字面量 |
| `writeJson(path, j) -> bool` | 写紧凑 JSON 到文件 |
| `writeJsonPretty(path, j) -> bool` | 写美化 JSON 到文件 |

### 构造
`jNull()` / `jBool(b)` / `jInt(i)` / `jFloat(f)` / `jStr(s)` / `jArray(items)` /
`jObject(keys, vals)`，以及 `jArrayOfInts` / `jArrayOfFloats` /
`jArrayOfBools` / `jArrayOfStrings`。

### 访问
| 函数 | 说明 |
| --- | --- |
| `kindName(j) / isNull / isBool / isInt / isFloat / isNumber / isString / isArray / isObject` | 类型 |
| `asBool / asInt / asFloat / asString / asArray` | 取值（类型不符给默认值） |
| `get(j, key) -> Json?` / `getOr` | 对象取字段 |
| `at(j, i) -> Json?` / `atOr` | 数组取下标 |
| `getString / getInt / getFloat / getBool / getArray / getObject` | 便捷取值 |
| `getPath(j, "a.b[0].c") -> Json` | 路径访问，缺失返回 null 值 |
| `has(j, key) / keys(j) / size(j)` | 查询 |
| `toIntArray(j) / toStringArray(j)` | 转成 Lux 数组 |

## 覆盖的 JSON 细节

- 字符串转义：`\" \\ \/ \b \f \n \r \t` 与 `\uXXXX`，含 UTF-8 代理对
  （`\uD83D\uDE00` → 😀），孤立/错误代理对报错；
- 数字：整数 / 小数 / 指数、负数；64 位整数精确解析，溢出自动退化为浮点；
- 深度上限 256，防恶意深嵌套；
- 错误带 `line:col`；
- 生成时保留 UTF-8 原字节，只转义 JSON 必需的字符；`NaN` / `Inf` 输出 `null`；
- `\u0000` 无法保存在 Lux 字符串里（C 字符串以 NUL 结束），会被丢弃。

## 表示

```lux
struct Json {
    kind: int;      // J_NULL/J_BOOL/J_INT/J_FLOAT/J_STR/J_ARR/J_OBJ
    b: bool;
    i: int;
    f: float;
    s: string;
    arr: Json[];    // 数组元素
    keys: string[]; // 对象的键
    vals: Json[];   // 对象的值（与 keys 等长）
}
```

对象用并行的 `keys` / `vals` 表示（Lux 0.9 尚无 map / union / 模式匹配），
按插入顺序保存，查找是线性扫描。
