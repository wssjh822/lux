# Lux 变更日志

本文件记录各版本的改动。0.1 → 0.2 → 0.3 的内容从 git 历史整理而来，
0.4 起每个版本都会在这里登记。

## 0.9.4

**ARC 收官 + 还账版：不加任何新语法**。0.9.4 把 0.9.2~0.9.3 承诺过的内存管理
闭环补完 —— **原生后端 free list 落地、ARC 双后端完整、ARC 默认开启、
`T?` 装箱回收、错误消息携带**。这三件事（ARC 双后端完整 + 默认开启 +
错误消息携带）落地后，Lux 才有资格叫 1.0。同时清掉欠账清单里的
C5/C6 相邻项与 D6。**全部 222 项回归测试通过**（含双后端逐字节差分）。

### A. ARC 收官（P0）

- **原生后端尺寸分级 free list（照 `docs/arc.md` §2.3 落地）**：新增
  `luxrt_alloc` / `luxrt_free`，把请求大小向上取整到 2 的幂（32B…16MiB，
  共 20 级），桶头放 bss 固定槽（`0x700000`），桶空才向 bump 区要内存，
  单线程无锁。对象头 32 字节 = `{refs, desc, class, 保留}`，**位于对象
  指针之前**，因此所有既有偏移运算（`[len][cap][data][ek]`）保持不变。
  数组元素区改为经 `luxrt_data_alloc` 申请，`luxrt_arr_reserve` 扩容时
  **归还旧块**（以前是 bump 泄漏）。
- **原生后端引用计数**：新增 `luxrt_retain` / `luxrt_release`，
  按对象头 `desc` 递归释放内层引用 —— `kDescOpt`（可选槽内层是引用类型）、
  `kDescArr`（数组按 `ek` 逐个释放引用元素 + 归还元素区）、
  `>= 0x10000`（struct 字段描述符，数据段地址）。
- **延迟释放（pending）模型**：`luxrt_alloc` 把对象登记进本帧的 pending 区，
  每条语句结束时 `luxrt_pend_flush_to(本帧基准)` 归还本次语句产生的全部
  临时对象；「存进变量 / 数组元素 / struct 字段 / 返回值」的位置先 `retain`，
  引用计数天然配平。这正是 `docs/arc.md` §2.2 写的「先做对，再做省」：
  多 retain 只会短暂多占内存，不会误释放。函数返回时先 retain 返回值、
  再 flush 本帧、最后把返回值登记给调用方。
- **解禁 `--arc --native`**：原生后端不再是 bump 分配器，`--arc --native`
  不再报错；ARC 场景进入双后端差分矩阵（`tests/run_tests.sh` 的
  「原生后端冒烟」一节对全部行为用例做 C ↔ native 逐字节对拍）。
- **ARC 默认开启（C 后端与原生后端）**：`--arc` 成为冗余开关，
  新增 `--no-arc` 退回 0.9.1 的「只增不减」语义（仅用于排查 / 对拍）。
  `LuxBuildFile` 的 `arc` 缺省值同样改为 `true`。
- **`T?` 装箱回收（P0-3）**：可选槽不再是裸 `malloc`，改为与堆对象同构的
  16 字节头 + `on_zero`；载荷是引用类型时槽归零顺带释放内层。局部 `T?`
  变量挂 `cleanup(lx_opt_releasep)` 在作用域出口回收；`or` 兜底 / `?` 传播 /
  `name!` 解包三处「消费掉一个可选值」的位置统一走新语义
  （`lx_opt_drop` 取走载荷、借用则先 `retain`），槽不再只增不减。
- **新增回归用例** `tests/cases/last_error.lux`；`arc_stress` / `arc_rawopt` /
  `arc_folded` 在原生后端也全部通过并对拍一致。
- **内存回归**：新增原生后端 churn 断言（20 万次 × 3 个字符串构造，
  峰值 RSS 必须 < 32 MiB），C 后端保留原有 1e6 次 / < 64 MiB 断言。

### B. 错误通道还账（P1）

- **`last_error(): string`（P1-5）**：全局错误消息通道，无 import 即可用。
  `T?` 只回答「成没成」，`last_error()` 补上「为什么」，**不改动 `T?` 签名**。
  两个后端的标准库失败路径填充同一套文案：
  `read 失败：文件不存在` / `read 失败：没有读取权限`（C 后端按 `errno`
  的 `ENOENT` / `EACCES`，原生后端按 `__sys_open` 的 `-2` / `-13`）、
  `int 解析失败：不是合法的 64 位整数`、`float 解析失败：不是合法的浮点数`、
  `find_opt 失败：未找到子串`。消息作为进程级根永久持有（无并发，
  单全局量够用）。
- **`find_opt(s, sub): int?`（P1-8）**：`find` 的可选变体，找不到返回 `none`
  并写 `last_error()`，调用方不必再跟 `-1` 打交道。`string` 模块，
  双后端行为逐字节一致。

### C. 欠账清单收尾（P1）

- **D6：bench 数字进 README**。`bench/run_bench.sh` 跑出的
  Lux(C) / Lux(原生, ARC 开) / Lux(原生, ARC 关) / C / Python 五方对照表
  写进 README「性能」一节，含 ARC 开关的性能代价。
- **原生后端 `asin` / `acos` 在 x86-64 上恒为 `nan`（既有缺陷，已修）**：
  旧的 x86-64 路径用 x87 手写，`FSUBP` 的栈方向写反，实际算的是
  `√(x²−1)`，对 `|x| < 1` 一律得 NaN。现在两个架构统一调用运行时库的
  Lux 实现（`luxrt_math_asin` / `luxrt_math_acos`），一套代码两处复用。
- **NaN 打印跨架构统一**：glibc 会按 NaN 的符号位印出 `-nan`，aarch64 的
  默认 NaN 却是正号 —— 同一份源码在不同架构上输出不同，也让 `raw_nan_inf`
  的双后端差分失败。现在打印层一律输出 `nan`（`inf` 仍保留 `+-`）。
- **自引用 struct 把编译器打崩（既有缺陷，已修）**：`strSlotsForType` 与
  `structToStr` ↔ `arrayToStrOnStack` 对递归类型（如 jsonx 的 `Json` 含
  `Json[]`）无限展开，栈溢出 SIGSEGV。前者加 visited 集合，后者加打印
  深度上限（超过输出 `<...>` 占位；正常嵌套深度 ≤ 3 不受影响）。
- **数组元素写入的引用计数语义**：`push` / `insert` 走 `luxrt_arr_put`
  （只 retain，不 release —— free list 回收来的块里是上一任的残渣，
  当垃圾指针释放会直接崩），`set` 走 `luxrt_arr_store`（retain 新 + release 旧）；
  `clear` 从「只把 len 置 0」改为真正释放引用元素。
- **迭代器与 bss 根的引用保持**：`for … in` 的被迭代对象跨语句存活，
  现在自持一份引用、循环出口释放；`setenv` 写进 bss 槽的 env 表永久 retain
  —— 否则调用方的语句级 flush 会把它们回收掉，槽里留悬空指针。

### D. 文档对齐（P0-4）

- `docs/stability.md`：§4「1.0 不包含」删掉已兑现的 ARC 默认启用 / 原生
  free list / `T?` 装箱回收三条；§5 破坏性窗口补 0.9.4 行；§6.1 状态更新为
  「已转正」；§1 补 `find_opt` / `last_error`。
- `docs/arc.md`：新增 0.9.4 状态块，§5 欠账清单逐条勾掉 A1 的收尾项。
- `README.md`：版本横幅、`--arc` / `--no-arc` 选项表、性能表、已知限制、
  路线图勾选同步。
- `Makefile`：`LUX_VERSION` → `0.9.4`。

### 0.9.4 明确不做（与拍板一致）

- `fn` 类型 / `map<K,V>` / 泛型 / 并发：已拍板 1.x，塞进来会重演
  「A1/A3 不同版本」的教训。
- `--emit-c` 去 GCC 扩展（C4）：已写进「不包含」，不做 scope creep。
- **C6（Arm64 未识别操作码改编译期报错）**：本机是 x86-64，无法验证
  aarch64 路径；不做未经测试的机器码改动，继续挂在欠账清单里（见
  `docs/arc.md` §5），留到能在 aarch64 上跑回归的版本。
- **字节视角 API（`byte_at` / `bytes(s)`）**：jsonx 已有 `BYTE_MASTER`
  绕行方案可用，按拍板留到 1.x。

## 0.9.3

**转正版：修 ARC 正确性 + 项目构建 + 导入系统 + 预处理**。0.9.3 按
`建议.txt` 的 A 块把 ARC 的正确性裂缝焊死，并按用户需求新增 `luxc build`、
`from … import …`、`#import` 与条件编译。ARC 的默认开启、原生后端 free list
与 `T?` 装箱回收仍挂账（见文末「已知缺口」）。

### A. ARC 正确性（A 块）

- **A1 折叠串 bug（最致命）**：`expr(Binary)` 的 `foldedIsStr` 分支在 ARC 下
  以前发裸 C 字面量，导致 `let s = "a" + "b";`、`const F = "x" + "y";`
  得到的指针没有 16 字节头，retain/release 读到非法头而崩溃。现在 ARC 下统一
  走 `internLit`（`refs = -1` 的静态不可变对象）。新增回归用例
  `tests/cases/arc_folded.lux`（局部折叠 / 全局常量 / 未初始化 string /
  循环内拼接），并在 `--arc` 维度下与 C / 原生后端差分。
- **A2-8 打印临时串回收**：`println(int/float/数组/struct/可选值)` 经 `toStrOf`
  产生的堆串在打印后 `lx_gc_release`；`bool`（返回静态字面量）与 `string`
  借用不释放，避免误 free .rodata。
- **A2 其余不变量核销**：return 走 `coerceStore`、赋值先存新值再释旧值、
  cleanup 仅挂 `arcMode && isRefTy`、`for c in s` 循环变量每轮释放、
  struct `cn_on_zero` 逐个发射、字面量包装区先于全局常量区。
- **原生后端未初始化 string**：`let s: string;` 以前压入 NULL，`println` /
  `len` 会段错误；现在指向合法的空串对象（`pushStrRef("")`）。
- 删除死代码 `lx_arr_print`。
- **未闭合块注释导致编译器 abort**：`skipWhitespaceAndComments` 对未闭合
  `/*` 抛出的 `LexBail` 以前在 `try` 之外，异常逃出 `run()` 触发
  `terminate`（fuzzing 命中）。现在也包进恢复逻辑，报错后继续。

### B. 项目构建：`luxc build`

- `luxc build [路径] [build|run|test|clean|rebuild]`：在路径下查找
  `LuxBuildFile`（兼容 `LuxBuildFile.lux` / `LuxBulidFile` / `lux.build` /
  `LuxBuild`），按配置构建 / 运行 / 测试 / 清理，面向更大项目。
- `LuxBuildFile` 为行式 `键 = 值` 配置：`name` / `main`(=`src`/`source`) /
  `out` / `outdir` / `backend`(c|native) / `arc` / `opt` / `cc` / `cflags` /
  `run_args` / `testdir` / `test` / `clean`，`#` 起注释，值可用引号。
- `build test` 会逐个编译测试文件、运行并与同名 `.expected` 比对，
  支持 `.args` 传参；`clean` 删除输出目录与额外清理项。
- 编译流水线从 `main()` 抽出为 `compilePipeline()`，供主命令与构建共用。

### C. 导入系统与预处理

- **`from "mod" import a, b;`（0.9.3）**：选择性导入，只把列出的成员注入
  当前命名空间；`from "mod" import *;` 等价默认导入。`from` 是**上下文
  关键字**（只在顶层 `from "路径" import` 生效），仍可作普通标识符。
- **`#import "mod";` / `#include "mod";`**：预处理写法，等价于 `import`。
- **预处理指令**：`#define NAME [值]`（对象式宏，整词展开、跳过字符串与
  行注释）、`#undef`、`#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` /
  `#endif`（`#if` 支持整数、`defined(NAME)`、`!` / `&&` / `||` 与括号）、
  `#error`。预处理逐行进行，**行号严格保持不变**，诊断定位不漂。
  这套指令即「增 / 删 / 改语言结构」的入口：`#define` 增、`#undef` 删、
  `#if*` 条件改。

### D. 自带库

- `packages/mathx` 增补 `sign` / `is_even` / `is_odd` / `ipow` /
  `digit_sum`；`packages/strx` 增补 `is_empty` / `index_of` / `trim_left` /
  `trim_right` / `replace_all`；新增官方包 `packages/arrx`（数组小工具：
  `reverse` / `sum` / `min` / `max` / `contains` / `index_of`，含 int /
  float / string 重载）。

### E. 文档

- `docs/grammar.md`：`importDecl` 增补 `from … import …` 与 `#import`，
  新增预处理指令说明；`docs/language.md` §10 增补选择性导入、预处理与
  `luxc build`；`docs/stability.md` §1/§4/§5 同步 0.9.3；`docs/arc.md`
  增补 0.9.3 状态与已知缺口（同时删掉顶部重复 H1）。

### 已知缺口（1.0 前）

- ARC 仍非默认（需 `--arc`）；原生后端仍是 bump 分配器（`--arc --native`
  明确报错）；`T?` 装箱仍只增不减；`main(argv)` 的 argv 数组是一次性泄漏。

## 0.9.2

**只修不扩 + ARC 落地 + 注册表加固 + 语法手册**。0.9.2 把 `建议.txt`
（A/B/C/D 四块）清干净，并按拍板结果把 **ARC（引用计数）** 的表示层与
C 后端实现落地（实验性 `--arc`，0.9.3 转正）；同时新增面向人与 AI 的
`docs/language.md` 语言教程。

### ARC（0.9.2 的重头戏，拍板：进 1.0）

- **表示层**：每个堆对象（string / 数组 / struct）前面加 16 字节头
  `{ refs, on_zero }`；字符串字面量是 `refs = -1` 的静态不可变对象，
  `retain` / `release` 对其跳过。数组头新增元素释放器 `relem`。
- **C 后端引用计数**（`--arc`，默认关闭）：在 `let` / 赋值（含 `x = x + …`、
  `x += …`）/ `return` / 数组 `push·pop·insert·remove·set·clear·slice` /
  struct 字段与嵌套 / **嵌套字符串拼接** 处插入 retain / release；
  局部变量用 `__attribute__((cleanup))` 在作用域出口释放。
- **extern 借用视图**：extern fn 的 string 参数与返回值仍是 `const char*`，
  不参与回收（遵守 0.9.2 拍板）。
- **实测**：1e6 次字符串构造的峰值 RSS 从 ~117 MiB 降到 ~11 MiB；
  全部行为用例在 `--arc` 下输出与非 ARC 逐字节一致（含 MALLOC_CHECK_ 抽查）。
- **已知缺口（0.9.3 补）**：`T?` 装箱不回收；控制流条件里的临时串会漏；
  原生后端仍是 bump 分配器（`--arc --native` 明确报错）。

### 建议.txt 审阅修复（A 块）

- `docs/stability.md` 开头改为“0.9.2 是最后一个破坏窗口”，与 §5 一致。
- `docs/grammar.md` 头部版本 0.8 → 0.9；`docs/design.md` §7 补登 `E0005`。
- struct 返回落穿（`E0005`）的 `stmtAlwaysReturns` 假阳性审计；新增
  `return_paths_deep` / `struct_fallthrough_break` 防御用例。
- `strx` 的 `pad_left` / `pad_right` 在 `fill == ""` 时直接返回，不再死循环。
- `jsonx` 支持 `INT64_MIN`（`-9223372036854775808` 不再退化成 float）。
- `src/json.cpp`：孤立代理→U+FFFD（不再产非法 UTF-8）；`dumpTo` 对
  ≥2^63 的 double 先做范围检查再转整数（修 UB）；注释写明前导 `+` 的宽松处理。
- 删除孤儿文件 `tests/cases/qualified_stdlib.expected`；双架构注释对齐
  （本机架构：x86-64 / aarch64）。
- 原生后端 `luxrt_math_pow2i` 修次正规 / 上下溢（`exp(-745)` 不再输出垃圾）。

### 测试缺口（B 块）

- `main_argv`：argv[0] 为程序名 + 参数逐个（支持 `tests/cases/*.args`）；
  测试脚本两后端各跑一遍。
- `math_edges`：`asin(±0.99999)` / `log(1e±300)` / `pow(1e300,2)` /
  `exp(±745/710)`，双后端差分（收敛到 6 位小数以屏蔽自研级数与 libm 的差异）。
- `optional_double_box`：`int??` 被 Sema 拒绝（`.err` 锁定）。
- `optional_eq`：`int("1") == int("2")` 被 Sema 拒绝（`.err` 锁定）。
- panic 文案（`assert_fail` / `read_fail` / `slices_oob_*`）继续由
  原生后端差分矩阵逐字校验。

### 拍板（C 块，详见 `docs/stability.md` §6）

- **ARC 进 1.0**：0.9.2 落地表示层 + C 后端，0.9.3 默认开启并补原生后端。
- **`fn` 类型不进 1.0**：`sort` / `map` / `filter` 留到 1.x。
- **deps 语义**：`install` / `upgrade` 确实递归安装 `deps`，仅做
  `^ ~ >= <= =` 的简单匹配，不做 semver 求解（与 pkgs.cpp 一致）。

### 注册表加固（D 块）

- `data/tokens.json` 只存令牌的 **SHA-256**，不再存明文。
- 注册（10 次/小时/IP）与上传（60 次/小时/IP）加速率限制
  （`data/ratelimit/`）。
- `server/README.md` 写明部署顺序：**先自己注册管理员账号，再开放公网**。
- 移除 `src/pkgs.cpp` 里内置的 FTP 账号密码：FTP 发布现在必须显式设置
  `LUX_FTP_USER` / `LUX_FTP_PASS`（或在 `~/.lux/config.json` 配置）；
  正式发布推荐账号 API（`luxc login` + `luxc publish`）。

### 新增文档

- **`docs/language.md`**：面向人与 AI 的 Lux 语言教程（语法 → 类型 →
  错误通道 → 标准库 → 惯用法 → 常见错误），配可运行的片段。

## 0.9.1

**注册表账号系统 + 网页界面 + 源码下载**。在 0.9.0 的在线包管理之上，把服务端
从「静态文件 + FTP」升级为带账号体系的完整注册表：上传 / 修改 / 删除自己发布的
包都要求登录，网页端提供注册 / 登录 / 浏览 / 搜索 / 上传 / 编辑 / 删除，并提供
编译器与包管理系统的源码下载。

### 服务端（server/）

- `index.html` 改为 `index.php`：暗色主题的网页界面，支持在线注册 / 登录 /
  退出、浏览与搜索包、查看包详情与版本、上传新包、编辑元数据、删除自己的包，
  也能查看与下载别人的包。
- 新增 `lib.php`：账号（`password_hash`）、API 令牌、包存储与所有权校验，
  纯文件存储（`data/users.json` / `data/tokens.json`），不依赖数据库扩展。
- `lux.php` 扩展为完整 API：`register` / `login` / `logout` / `whoami` / `mine` /
  `publish` / `edit` / `delete`，公开接口 `index` / `search` / `info` /
  `download` / `health` 保持兼容。
- 上传 / 修改 / 删除均校验所有权：只能操作自己发布的包（管理员除外），
  已存在的他人版本禁止覆盖。
- 新增「源码下载」页与 `downloads/downloads.json` 清单；
  `server/make_downloads.sh` 一键打包包管理系统源码、当前 Lux 源码，
  并收集 `/sdcard/code/lux/` 里的历史版本。
- 官方包新增 **`jsonx` 1.0.0**（专业 JSON 处理：解析 / 生成 / 美化 /
  文件读写，完整转义与 `\u` 代理对、错误行列、64 位整数溢出退化），
  已发布到注册表；`packages/jsonx/` 附带源码与 README。

### 命令行（luxc）

- 新增 `luxc login [用户名]`（密码无回显）/ `logout` / `whoami`，令牌存到
  `~/.lux/config.json`，也可用 `$LUX_TOKEN`。
- `luxc publish` 默认改走账号 API（HTTP POST + 令牌）；未登录会提示先
  `luxc login`。`ftp://` 注册表仍回退到 FTP 上传。
- 新增 `luxc unpublish <包名> [版本]` 删除自己发布的包；发布 / 删除后自动
  作废本地索引缓存。

### 0.9.0 内容（保留）

**在线包管理 + 建议.txt 审阅修复**。把包管理从「本地 `add`」升级成完整的
注册表客户端（下载 / 校验 / 依赖 / 版本 / 搜索 / 升级 / 发布），并落地
`建议.txt` 里一批已确认的 bug 修复。

### 包管理（在线注册表）

- 新增 `luxc install <名字[@版本] | URL | 本地路径>`：从注册表下载安装，
  自动按 `lux.json` 的 `deps` 递归安装依赖，支持 `^` / `~` / `>=` 等语义化
  版本约束；下载后校验元数据里的 SHA-256，安装走「临时目录 → 原子 rename」。
- 新增 `luxc search` / `info` / `update` / `upgrade` / `remove` / `registry`，
  `list` 显示版本、来源与入口。
- 新增 `luxc publish <包目录>`：打包 + 算哈希 + 生成元数据，通过 FTP 上传到
  注册表的 `packages/<名字>/<版本>.{tar.gz,json}`。
- 注册表默认 `https://lux.xfes.top/lux/lux.php`，可用 `luxc registry` 或
  `$LUX_REGISTRY` 换成任何返回相同 JSON 结构的外部地址；索引缓存在
  `$LUX_HOME/cache/index.json`，离线可读。
- 新增 `server/lux.php`（注册表服务端，动态扫描索引）与示例包
  `packages/mathx`、`packages/strx`、`packages/numx`（`numx` 演示依赖）。
- 新增完整 JSON 解析器（`src/json.cpp`）与 SHA-256（`src/sha256.cpp`）。

### 建议.txt 修复（代码级）

- **struct 返回落穿**：返回类型是 struct 的函数存在不经过 `return` 的路径时，
  从警告升级为硬错误 `E0005`（原来会返回空指针，访问字段直接崩溃）。
- **`Point[][]` 打印跨后端分歧**：原生后端把 struct 字段裸 qword 当整数打印，
  现在 `arrayToStrOnStack` 按类型结构递归展开到任意嵌套。
- **extern fn × Optional**：Sema 拒绝 extern 收发 `T?`（运行时私有指针表示，
  C 侧没有对应 ABI）。
- **panic 文案对齐 stability.md §3**：切片越界补上「起点 / 终点 / 长度」数字；
  `assert` 并进统一的 `lx_panic` 通道；`read!` 失败带上具体路径。
- **可选类型边界**：`T?` 不能作为数组元素类型（parser 报错）或 struct 字段
  （Sema 报错），与 grammar §8.1 一致。
- **`patchArm64SyscallNumbers`**：常量整个缺失时也报内部错误，不再静默用错
  系统调用号。
- 防御 `NoneLit` 类型缺失时的 codegen 崩溃；删除死代码 `lx_file_read` /
  `lx_file_err` / `lx_str_to_i64` / `lx_str_to_f64`。

### 文档 / 卫生

- grammar：修正前后缀类型 EBNF（`'[]'` 不是 token），删掉 "`int?[]` 合法"
  与 `T?` 不能作数组元素的自相矛盾表述。
- stability.md：`read!` 文案单列一行。
- 测试：新增 `struct_nested_arr`、`assert_fail`、`read_fail`、
  `struct_fallthrough`、`extern_optional`、`optional_arr_elem`、
  `optional_struct_field`，以及一个 `file://` 本地注册表的在线安装端到端用例。
- 版本号提升到 0.9.0。

## 0.8.0

**错误通道 + 发布工程版**。实装语言层最后一个破坏性特性：可选类型 `T?`、
错误传播后缀 `expr?`、兜底表达式 `lhs or rhs`、panic 变体 `name!(...)`，并把
`int` / `float` / `read` 三个最常失败的标准库函数迁移到错误通道。同时清掉一批
欠了多个版本的 C 层硬伤（复合 `/=` `%=` 的除零、`>>` 语义分裂、struct 零值、
`f([])` 空数组实参、C 后端标识符撞名），并发现 / 修复了三个长期潜伏的原生
后端 bug（aarch64 浮点数组元素读取、文件路径少了 8 字节偏移、大数浮点格式化）。

> **关于 ARC（A1）与 `fn` 类型（B6）**：两者按 `docs/arc.md` 的计划顺延。
> `建议.txt` 的决策树明确指出**不要把 A1 与 A3 塞进同一个版本**（两者都触碰
> 所有值传递路径，差分体系无法归因）。本版选择先把错误通道做扎实；ARC 与
> 高阶函数明确列为 1.0 的“顺延 / 不包含”项（见 `docs/stability.md`）。

### 语言：错误通道（A3）

- 新增可选类型 `T?`（`int?` / `float?` / `string?` / struct? / 数组?，可嵌套
  `int??`）。表示层统一为**指向堆槽的指针，NULL = none**，两后端都是单个 8 字节槽。
- 新增 `none` 字面量：类型完全由上下文决定（标注 / 返回类型 / `or` 右侧）；
  没有可选上下文时报定向错误。
- 新增传播后缀 `expr?`：`operand` 必须是 `T?`，所在函数必须返回 `U?`；失败时
  整个函数直接返回 `none`，成功时解包出 `T`。
- 新增兜底表达式 `lhs or rhs`（**A3.1 方案 a**）：`or` 关键字按**左操作数类型**
  分派——左边是 `bool` 时仍是逻辑或，是 `T?` 时是兜底（失败取右侧）；`||`
  永远是逻辑或。规则已写进 `docs/grammar.md`。
- 新增 panic 变体 `name!(...)`：`int!` / `float!` / `read!` 失败即 panic；
  任意返回 `T?` 的用户函数也支持 `f!(...)`。
- **标准库语义迁移（破坏性，A3.4）**：
  - `int(x)`：数值 / bool → `int`（不变）；字符串 → `int?`（解析失败返回 none，
    不再静默返回 0）。
  - `float(x)`：数值 / bool → `float`；字符串 → `float?`。
  - `read(path)` → `string?`（文件不存在返回 none，不再直接 panic）。
  - 其余保持原值语义：`write` / `append` / `remove` / `rename` / `exists` 仍返回
    `bool`，`system` 仍返回退出码，`env` 仍返回空串（这些函数本来就把失败
    编码在返回值里）。
- **迁移指南 + 定向诊断（A3.6）**：`?` 用在非 `T?` 返回的函数里、`or` 左侧不是
  `T?`、`!` 用在不会失败的内建上、对 `T?` 直接做算术——各给专属报错，错误信息
  里写明如何改用 `?` 传播 / `or` 兜底 / `!` panic，而不是泛化的“类型不匹配”。
- **测试迁移（A3.7）**：所有用到 `int("...")` / `read(...)` 的存量用例已重录
  （改用 `int!` / `read!`）；新增 `tests/cases/error_channel.lux` 与 7 个诊断用例，
  覆盖传播链、`or` 兜底、`!` panic、嵌套 `int??`、`none`，并全程双后端差分。
- **REPL（A3.8）**：`T?` 结果直接显示为 `some(x)` / `none`；补全词表加入 `none`。

### 语言：其他

- **B3 —— `f([])` 空数组实参**：`checkCall` 重构为先解析被调签名、再用形参类型
  逐个检查实参，空数组字面量终于能从形参推断元素类型（欠了四个版本的缺陷）。
- **C2 —— struct 零值**：Sema 拒绝 `let p: Point;` 这种无初始化的 struct 声明
  （默认空指针访问字段会崩溃）；确实要空值请声明为 `Point?`。
- **C1 —— 复合赋值与移位**：`x /= v` / `x %= v`（含 `a[i]` / `p.x`）改走带零检查
  的 `lx_idiv` / `lx_imod`，不再因为除以 0 触发 SIGFPE；`>>` 两后端统一为**算术
  右移**（旧版 C 后端是逻辑右移，`-8 >> 1` 与原生结果不同）。
- **C7 —— 原生 syscall 编号改写防脆弱**：常量存在但不是整数字面量时报内部错误，
  不再静默留下错误编号。
- **C4**：生成 C 的数组字面量仍用 GCC 语句表达式（`--emit-c` 可移植性列为 1.0
  的不包含项）。

### 修复：三个长期潜伏的原生后端 bug

- **aarch64 浮点数组元素读取（C11）**：`ldr <Dt>, [Xn, Xm, lsl #3]` 的机器码多置了
  bit24（`0xFD600800` → 正确 `0xFC600800`），导致 `xs[0]` 对 `float[]` 恒为 0。
  旧测试只打印整个 float 数组（走另一条路径）所以一直未暴露。
- **原生文件 I/O 路径（C12）**：`__sys_open` / `unlink` / `rename` 传的是
  Lux 字符串头（head 8 字节是长度），实际读写的是名字里带长度字节的畸形文件。
  因为测试里“写 / 读 / 存在 / 删”用的是同一个变异名而自洽，所以一直“通过”。
  修正为 `__sptr(path) + 8`，原生与 C 后端现在操作的是真正的文件。
- **大数浮点格式化（C13）**：`luxrt_f64_to_str` 在 `bigE ≥ 0` 路径把指数重复
  计入（`xp = nd + bigE`），导致 `≥ ~4.5e15` 的整数打印出错误指数
  （`1e150` → `1e595`）；修正为 `xp = nd`。最后一位精度仍可能与 libm 有差异。
- **C 后端标识符撞名**：用户标识符从 `lx_` 改为 `lxv_` / `lxm_` 前缀，
  与运行时 `lx_*` 命名空间彻底分开（旧版 `let arr = ...` 会生成
  `lx_arr lx_arr = ...`，直接编译错误）。

### 发布工程与文档

- 新增 `.github/workflows/ci.yml`：构建 + 全量回归 + 双后端差分矩阵
  （x86-64 与 aarch64），另加 ASan/UBSan 构建跑测试、ASan 编译生成的 C。
- 新增黑盒 fuzzing 冒烟（`tests/fuzz_lexparse.sh`）：随机字节 + 合法程序变异，
  断言编译器不崩溃 / 不挂死。
- 新增回归用例：`error_channel`、`b3_empty_array`、`compound_divmod`、
  `struct_stress`、`float_index` + 7 个诊断用例。
- `docs/grammar.md` 升格为 0.8 文法（`?` / `none` / `or` 分派 / `name!`）。
- `docs/stability.md` 更新：错误通道进入稳定承诺；ARC、`fn` 类型、`static let`、
  `--emit-c` 可移植性明确列入 1.0 不包含项。
- `README.md` / `index.html` / `docs/design.md` 全面对齐。

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
