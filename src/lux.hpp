// =============================================================================
//  Lux Programming Language -- Compiler
//  lux.hpp : 公共定义（诊断、类型、Token、AST、各阶段接口）
//
//  Lux 是一门静态类型的编译型语言，当前版本通过 C 后端生成原生可执行文件。
//  编译流水线： 源码 -> Lexer -> Parser -> Loader(import 展开) -> Sema -> Codegen(C) -> cc
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux {

// =============================================================================
//  诊断信息
// =============================================================================

struct SourceLoc {
    int line = 1;
    int col = 1;
};

// 诊断错误码（E=错误，W=警告）。新增代码时在此登记，方便文档检索与 IDE 集成。
namespace DiagCode {
inline constexpr const char* kLex            = "E0001";  // 词法错误
inline constexpr const char* kParse          = "E0002";  // 语法错误
inline constexpr const char* kSema           = "E0003";  // 语义 / 类型错误
inline constexpr const char* kModule         = "E0004";  // 模块 / import / 文件错误
inline constexpr const char* kEStructReturn  = "E0005";  // struct 函数存在不 return 的路径
inline constexpr const char* kWUnusedVar     = "W1001";  // 未使用的变量 / 参数
inline constexpr const char* kWNoEffect      = "W1002";  // 表达式结果未被使用
inline constexpr const char* kWMissingReturn = "W1003";  // 函数末尾缺少 return
inline constexpr const char* kWExternReserved = "W1004"; // extern fn 与运行时符号冲突
inline constexpr const char* kWShadowStdlib  = "W1005";  // 本地文件与标准库模块同名
inline constexpr const char* kWPrivIntrinsic = "W1006";  // 用户代码调用 __ 系特权内建
}  // namespace DiagCode

struct Diagnostic {
    bool isError = true;
    std::string code;  // 错误码（E0001 ... / W1001 ...）
    SourceLoc loc;
    std::string msg;
    std::string file;  // 诊断所属文件（空 = 未知 / 单文件模式）
};

// 文件路径 -> 源码内容（多文件编译时用于渲染诊断上下文）
using SourceMap = std::unordered_map<std::string, std::string>;

// 统一的诊断收集器：词法、语法、语义、模块加载四阶段共用同一套机制，
// 一次编译可以收集并报告全部错误（不再"第一个错误就停止"）。
class Diags {
public:
    std::vector<Diagnostic> items;
    int errors = 0;
    int warnings = 0;

    void error(std::string code, SourceLoc loc, std::string msg) {
        error(std::move(code), {}, loc, std::move(msg));
    }
    void error(std::string code, std::string file, SourceLoc loc, std::string msg) {
        items.push_back({true, std::move(code), loc, std::move(msg), std::move(file)});
        errors++;
    }
    void warn(std::string code, SourceLoc loc, std::string msg) {
        warn(std::move(code), {}, loc, std::move(msg));
    }
    void warn(std::string code, std::string file, SourceLoc loc, std::string msg) {
        items.push_back({false, std::move(code), loc, std::move(msg), std::move(file)});
        warnings++;
    }
    bool ok() const { return errors == 0; }
    // 把收集到的诊断信息按 GCC 风格打印到 stderr。
    // sources 按诊断中的 file 字段查找源码上下文，找不到就不显示。
    void flush(const SourceMap& sources = {});
};

// 是否启用彩色输出（由 --no-color / 终端类型决定）
void setColorEnabled(bool on);

// =============================================================================
//  类型系统
//
//  Ty 采用「两层结构」：基础类型是全局唯一的 intern 对象（指针相等 == 类型
//  相等），复合类型挂在 elem / members 上。未来数组、map、tuple、struct、
//  函数类型都只需在 TyKind / Ty 上新增一层，不需要改动比较逻辑。
// =============================================================================

enum class TyKind {
    Invalid,  // 尚未推断 / 出错后的占位
    Int,      // int    : 64 位有符号整数
    Float,    // float  : 64 位双精度浮点
    Bool,     // bool   : true / false
    String,   // string : UTF-8 字符串（C 层为 const char*）
    Void,     // void   : 无返回值
    Fn,       // fn(参数...) -> 返回类型（预留给高阶函数）
    Array,    // elem[]（预留给数组 / 切片）
    Tuple,    // (a, b, c)（预留给多返回值 / struct 展开）
    Named,    // 具名类型（预留给 struct / enum）
    Optional, // T?（0.8 错误通道：可能缺失 / 失败的值）
};

struct Ty {
    TyKind kind = TyKind::Invalid;
    const Ty* elem = nullptr;        // Array 的元素类型；Fn 的返回类型
    std::vector<const Ty*> members;  // Tuple 的成员类型；Fn 的参数类型
    std::string name;                // Named 的类型名
};

// 类型表：结构等价的类型全局唯一，比较用指针相等即可
class TyStore {
public:
    static const Ty* invalid();
    static const Ty* int64Ty();
    static const Ty* float64Ty();
    static const Ty* boolTy();
    static const Ty* stringTy();
    static const Ty* voidTy();
    static const Ty* fnOf(const Ty* ret, std::vector<const Ty*> params);
    static const Ty* arrayOf(const Ty* elem);
    static const Ty* tupleOf(std::vector<const Ty*> members);
    static const Ty* named(const std::string& name);
    // 可选类型 T?（0.8）：elem 为 T，intern 化后指针相等依然成立
    static const Ty* optionalOf(const Ty* elem);
};

std::string tyName(const Ty* t);
bool isNumeric(const Ty* t);

// 类型注解在源码中是否显式写出
struct TypeAnn {
    const Ty* ty = nullptr;
    SourceLoc loc;
    bool present = false;
};

// =============================================================================
//  Token
// =============================================================================

enum class Tok {
    Eof,
    Ident,

    // 字面量
    IntLit,
    FloatLit,
    StrLit,

    // 关键字
    KwFn,
    KwLet,
    KwConst,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwLoop,
    KwIn,
    KwBreak,
    KwContinue,
    KwTrue,
    KwFalse,
    KwAnd,   // and
    KwOr,    // or
    KwNot,   // not
    KwImport,   // import
    KwFrom,     // from（0.9.3：from "mod" import a, b）
    KwExtern,   // extern
    KwAs,       // as（import 别名）
    KwElif,     // elif（else if 的别名）
    KwRepeat,   // repeat n { ... }
    KwStruct,   // struct（0.7）
    KwNan,      // nan 字面量（0.7）
    KwInf,      // inf 字面量（0.7）
    KwNone,     // none 字面量（0.8）

    // 类型关键字
    KwInt,
    KwFloat,
    KwBool,
    KwString,
    KwVoid,

    // 标点符号
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    Arrow,      // ->
    Dot,
    DotDot,     // ..
    DotDotEq,   // ..=
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Assign,     // =
    Eq,         // ==
    Ne,         // !=
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,     // &&
    OrOr,       // ||
    Bang,       // !
    Amp,        // &
    Pipe,       // |
    Caret,      // ^
    Tilde,      // ~
    Shl,        // <<
    Shr,        // >>
    PlusPlus,   // ++
    MinusMinus, // --
    PlusEq,     // +=
    MinusEq,    // -=
    StarEq,     // *=
    SlashEq,    // /=
    PercentEq,  // %=
    AmpEq,      // &=
    PipeEq,     // |=
    CaretEq,    // ^=
    ShlEq,      // <<=
    ShrEq,      // >>=
    Question,   // ?（0.8：类型位 T? / 表达式后缀 expr?）
};

const char* tokName(Tok t);

struct Token;  // 前向声明，供 tokDesc 使用
// 把 token 渲染成人类可读形式，用于错误信息
std::string tokDesc(const Token& tk);

struct Token {
    Tok kind = Tok::Eof;
    std::string text;  // 原始文本
    long long ival = 0;
    double fval = 0.0;
    SourceLoc loc;
};

// =============================================================================
//  词法分析（错误经 Diags 报告；出错后跳过坏字符继续，一次报完所有词法错误）
// =============================================================================

std::vector<Token> tokenize(const std::string& src, const std::string& filename,
                            Diags& diags);

// =============================================================================
//  AST
// =============================================================================

// 所有 AST 节点由 Parser 内的对象池持有，节点之间用裸指针互相引用。

enum class ExprKind {
    IntLit,
    FloatLit,
    BoolLit,
    StrLit,
    Ident,
    Binary,
    Unary,
    Call,
    Index,      // a[i]（0.5 新增）
    Slice,      // a[lo..hi] / a[lo..=hi]（0.6 新增，复制语义）
    ArrayLit,   // [1, 2, 3]（0.5 新增）
    StructLit,  // Point { x: 1, y: 2 }（0.7 新增）
    Member,     // p.x（0.7 新增）
    If,         // if c { a } else { b }（0.7 新增，表达式形态）
    NoneLit,    // none 字面量（0.8 新增）
    Try,        // expr?（0.8 新增：错误传播后缀）
};

enum class BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    LogicAnd,
    LogicOr,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
};

enum class UnOp {
    Neg,       // -
    LogicNot,  // ! / not
    BitNot,    // ~
};

const char* binOpText(BinOp op);
const char* unOpText(UnOp op);

// 内置函数（由编译器直接展开为运行时调用，不参与名字改写）
// 其中一部分属于标准库模块（math / time / system / file / string），
// 必须先 import 对应模块才能使用（见 builtinModule 映射）。
enum class Builtin {
    None,
    Print,     // print(...)     : 输出，不换行
    PrintLn,   // println(...)   : 输出并换行
    Len,       // len(s)         : 字符串长度
    Input,     // input()        : 读取一行
    ToInt,     // int(x)
    ToFloat,   // float(x)
    ToString,  // string(x)
    Assert,    // assert(cond)
    Exit,      // exit(code)
    Abs,       // abs(x)
    Sqrt,      // sqrt(x)
    // ---- math 模块 ----
    Pow,       // pow(x, y)   : x^y
    Floor,     // floor(x)
    Ceil,      // ceil(x)
    Round,     // round(x)
    Sin,       // sin(x)
    Cos,       // cos(x)
    Tan,       // tan(x)
    Asin,      // asin(x)
    Acos,      // acos(x)
    Atan,      // atan(x)
    Log,       // log(x)      : 自然对数
    Log10,     // log10(x)
    Exp,       // exp(x)
    Min,       // min(a, b)   : int/float 同型
    Max,       // max(a, b)   : int/float 同型
    Atan2,     // atan2(y, x)
    Fmod,      // fmod(a, b)
    Hypot,     // hypot(a, b)
    Trunc,     // trunc(x)
    IsNan,     // isnan(x)    : bool
    IsInf,     // isinf(x)    : bool
    Random,    // random()    : [0,1) 之间的 float
    Seed,      // seed(n)     : 随机数种子
    // ---- time 模块 ----
    TimeNow,   // now()          : 当前 Unix 时间戳（秒，float）
    TimeMono,  // monotonic()    : 单调时钟（秒，float，测耗时用）
    Sleep,     // sleep(sec)     : 休眠秒数（float）
    SleepMs,   // sleep_ms(ms)   : 休眠毫秒数（int）
    // ---- system 模块 ----
    SysCall,   // system(cmd)    : 执行 shell 命令，返回退出码
    Env,       // env(name)      : 读环境变量，不存在返回 ""
    Setenv,    // setenv(name, val) : 设置环境变量，成功返回 true
    // ---- file 模块 ----
    FileRead,    // read(path)          : 读整个文件（失败即运行时错误）
    FileWrite,   // write(path, data)   : 写文件（覆盖）
    FileAppend,  // append(path, data)  : 追加写文件
    FileExists,  // exists(path)        : 文件是否存在
    FileRemove,  // remove(path)        : 删除文件，成功返回 true
    FileRename,  // rename(from, to)    : 重命名，成功返回 true
    // ---- string 模块 ----
    StrContains,   // contains(s, sub)     : bool
    StrStartsWith, // startswith(s, pre)   : bool
    StrEndsWith,   // endswith(s, suf)     : bool
    StrFind,       // find(s, sub)         : int（字节偏移，找不到返回 -1）
    StrFindOpt,    // find_opt(s, sub)     : int?（0.9.4：找不到返回 none）
    StrReplace,    // replace(s, old, new) : string
    StrTrim,       // trim(s)              : string
    StrUpper,      // upper(s)             : string（ASCII 大小写）
    StrLower,      // lower(s)             : string
    StrSubstr,     // substr(s, start, len): string（按字节）
    StrFormat,     // format("...{}...", args...) : string
    StrSplit,      // split(s, sep)       : string[]（0.5 新增）
    StrChars,      // chars(s)            : string[]（逐字节单字符，0.5 新增）
    StrJoin,       // join(arr, sep)      : string（0.5 新增）
    LastError,     // last_error()        : string（0.9.4：最近一次标准库失败的说明）
    // ---- 数组方法（a.push(x) 形式；Sema 校验接收者，0.5 新增） ----
    ArrPush,       // a.push(v)           : void
    ArrPop,        // a.pop()             : 元素类型
    ArrInsert,     // a.insert(i, v)      : void
    ArrRemove,     // a.remove(i)         : 元素类型
    ArrClear,      // a.clear()           : void
    // ---- 原生后端特权内建（__ 前缀，0.6；仅 native 后端支持，运行时库专用，
    //      Sema 不检查参数个数与类型，返回类型除注明外均为 int） ----
    IntrSyscall,   // __syscall(nr, a1..a6)          : 系统调用
    // 结构性系统调用：x86-64 与 aarch64 的调用形态不同（openat/unlinkat/
    // renameat/clone），由后端翻译，运行时库用这组内建保持架构无关
    IntrSysOpen,   // __sys_open(path, flags)        : 打开文件（失败 <0）
    IntrSysUnlink, // __sys_unlink(path)             : 删除文件
    IntrSysRename, // __sys_rename(from, to)         : 重命名
    IntrSysFork,   // __sys_fork()                   : 创建子进程
    IntrBumpAlloc, // __bump_alloc(n)                : 16 对齐的堆分配
    IntrPeek64,    // __peek64(addr, off)            : 读 8 字节
    IntrPeek8u,    // __peek8u(addr, off)            : 读 1 字节（零扩展）
    IntrPoke64,    // __poke64(addr, off, v)         : 写 8 字节
    IntrPoke8,     // __poke8(addr, off, v)          : 写 1 字节
    IntrMemCopy,   // __mem_copy(dst,do,src,so,n)    : 复制内存
    IntrFBits,     // __f_to_bits(f) -> int          : float 位模式
    IntrFFrom,     // __f_from(bits) -> float        : 位模式转 float
    IntrIToF,      // __i_to_f(i) -> float           : int 转 float
    IntrFToI,      // __f_to_i(f) -> int             : float 截断转 int
    IntrRandNext,  // __rand_next()                  : xorshift64
    IntrSeedSet,   // __seed_set(v)                  : 设置随机种子
    IntrSPtr,      // __sptr(s) -> int               : 任意指针值透传为 int
    IntrSVal,      // __sval(p) -> string            : int 指针透传为 string
    IntrSValA,     // __sval_a(p) -> int[]           : int 指针透传为数组
};

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    const Ty* ty = nullptr;  // 由 Sema 填充

    explicit Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
    long long value = 0;
    IntLitExpr(SourceLoc l, long long v) : Expr(ExprKind::IntLit, l), value(v) {
        ty = TyStore::int64Ty();
    }
};

struct FloatLitExpr : Expr {
    double value = 0.0;
    FloatLitExpr(SourceLoc l, double v) : Expr(ExprKind::FloatLit, l), value(v) {
        ty = TyStore::float64Ty();
    }
};

struct BoolLitExpr : Expr {
    bool value = false;
    BoolLitExpr(SourceLoc l, bool v) : Expr(ExprKind::BoolLit, l), value(v) {
        ty = TyStore::boolTy();
    }
};

struct StrLitExpr : Expr {
    std::string value;
    StrLitExpr(SourceLoc l, std::string v)
        : Expr(ExprKind::StrLit, l), value(std::move(v)) {
        ty = TyStore::stringTy();
    }
};

struct GlobalConstDecl;  // 前向声明（IdentExpr 引用它）
struct FuncDecl;         // 前向声明（IdentExpr 引用具名函数，1.1）

struct IdentExpr : Expr {
    std::string name;  // 普通标识符，或模块限定访问 "mod.name"
    GlobalConstDecl* constRef = nullptr;  // 引用全局常量时由 Sema 填充
    FuncDecl* funcRef = nullptr;  // 1.1：引用具名函数（函数作为值）
    IdentExpr(SourceLoc l, std::string n)
        : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
    BinOp op;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
    // 0.8：`lhs or rhs` 的 or 关键字形式。当 lhs 是 T? 时按兜底（fallback）
    // 语义解释；否则仍按逻辑或解释（A3.1 按左操作数类型分派）。
    bool orKeyword = false;
    bool orFallback = false;  // Sema 填充：确实走了兜底语义
    // 常量折叠结果（Sema 填充）：folded 为 true 时代码生成直接使用字面量
    bool folded = false;
    bool foldedIsStr = false;
    long long foldedI = 0;
    double foldedF = 0.0;
    std::string foldedS;
    BinaryExpr(SourceLoc l, BinOp o, Expr* a, Expr* b)
        : Expr(ExprKind::Binary, l), op(o), lhs(a), rhs(b) {}
};

struct UnaryExpr : Expr {
    UnOp op;
    Expr* operand = nullptr;
    UnaryExpr(SourceLoc l, UnOp o, Expr* e)
        : Expr(ExprKind::Unary, l), op(o), operand(e) {}
};

struct CallExpr : Expr {
    std::string callee;  // 函数名，或模块限定名 "mod.name"，或数组方法 "a.push"
    SourceLoc calleeLoc;
    std::vector<Expr*> args;
    Builtin builtin = Builtin::None;  // Sema 填充
    struct FuncDecl* target = nullptr;  // Sema 填充（用户函数）
    // 1.1：被调者是 fn 类型的局部变量（间接调用），callee 即变量名
    bool viaValue = false;
    const Ty* calleeFnTy = nullptr;  // 1.1：viaValue 时被调函数值的类型
    // 0.8：panic 变体调用（如 int!(s) / read!(p)），失败时退出而不是返回 T?
    bool panicVariant = false;
    // 数组方法调用（a.push(x)）：Sema 填充接收者变量名，代码生成用它
    // 定位 C 变量；methodRecv 为空 = 不是方法调用。
    std::string methodRecv;
    const Ty* methodElem = nullptr;  // Sema 填充：接收数组的元素类型

    CallExpr(SourceLoc l, std::string c, SourceLoc cl, std::vector<Expr*> a)
        : Expr(ExprKind::Call, l),
          callee(std::move(c)),
          calleeLoc(cl),
          args(std::move(a)) {}
};

// 下标表达式 a[i]（0.5 新增）：数组取元素 / 字符串取单字节字符
struct IndexExpr : Expr {
    Expr* base = nullptr;
    Expr* index = nullptr;
    IndexExpr(SourceLoc l, Expr* b, Expr* i)
        : Expr(ExprKind::Index, l), base(b), index(i) {}
};

// 切片表达式 a[lo..hi] / a[lo..=hi]（0.6 新增）：复制语义，
// 返回新数组 / 新字符串。start/end 为 nullptr 表示端点省略
// （a[..j] 从 0 开始、a[i..] 到末尾）。
struct SliceExpr : Expr {
    Expr* base = nullptr;
    Expr* start = nullptr;
    Expr* end = nullptr;
    bool inclusive = false;  // ..= 为 true
    SliceExpr(SourceLoc l, Expr* b, Expr* s, Expr* e, bool inc)
        : Expr(ExprKind::Slice, l), base(b), start(s), end(e), inclusive(inc) {}
};

// 数组字面量 [e1, e2, ...]（0.5 新增）；元素类型由 Sema 统一推断
struct ArrayLitExpr : Expr {
    std::vector<Expr*> elems;
    const Ty* elemTy = nullptr;  // Sema 填充：最终统一的元素类型
    ArrayLitExpr(SourceLoc l, std::vector<Expr*> es)
        : Expr(ExprKind::ArrayLit, l), elems(std::move(es)) {}
};

struct StructDecl;  // 前向声明

// struct 字面量 Point { x: 1, y: 2 }（0.7 新增）
struct StructLitExpr : Expr {
    std::string typeName;  // 可能带模块限定，如 mod.Point
    SourceLoc typeLoc;
    std::vector<std::pair<std::string, Expr*>> inits;  // 字段名 -> 初始值
    StructDecl* decl = nullptr;                        // Sema 填充
    StructLitExpr(SourceLoc l, std::string n, SourceLoc nl,
                  std::vector<std::pair<std::string, Expr*>> is)
        : Expr(ExprKind::StructLit, l),
          typeName(std::move(n)),
          typeLoc(nl),
          inits(std::move(is)) {}
};

// 成员访问 p.x（0.7 新增）；fieldIndex 由 Sema 填充
struct MemberExpr : Expr {
    Expr* base = nullptr;
    std::string member;
    SourceLoc memberLoc;
    int fieldIndex = -1;
    // 模块限定常量 math.pi：Sema 解析后填入，代码生成按它改写名字
    GlobalConstDecl* constRef = nullptr;
    MemberExpr(SourceLoc l, Expr* b, std::string m, SourceLoc ml)
        : Expr(ExprKind::Member, l),
          base(b),
          member(std::move(m)),
          memberLoc(ml) {}
};

// if 表达式 if c { a } else { b }（0.7 新增）；两个分支都必须是表达式
struct IfExpr : Expr {
    Expr* cond = nullptr;
    Expr* thenVal = nullptr;
    Expr* elseVal = nullptr;
    IfExpr(SourceLoc l, Expr* c, Expr* t, Expr* e)
        : Expr(ExprKind::If, l), cond(c), thenVal(t), elseVal(e) {}
};

// none 字面量（0.8）：类型由上下文（hint）确定为某个 T?，无 hint 时报错
struct NoneLitExpr : Expr {
    explicit NoneLitExpr(SourceLoc l) : Expr(ExprKind::NoneLit, l) {}
};

// 错误传播后缀 expr?（0.8）：operand 必须是 T?，在返回类型为 U? 的函数里
// 失败时直接向上返回 none，成功时解包出 T
struct TryExpr : Expr {
    Expr* operand = nullptr;
    TryExpr(SourceLoc l, Expr* e) : Expr(ExprKind::Try, l), operand(e) {}
};

enum class StmtKind {
    Let,
    Expr,
    Assign,
    If,
    While,
    For,
    Loop,
    Break,
    Continue,
    Return,
    Block,
};

struct Stmt {
    StmtKind kind;
    SourceLoc loc;
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};

struct BlockStmt : Stmt {
    std::vector<Stmt*> stmts;
    BlockStmt(SourceLoc l, std::vector<Stmt*> s)
        : Stmt(StmtKind::Block, l), stmts(std::move(s)) {}
};

struct LetStmt : Stmt {
    std::string name;
    SourceLoc nameLoc;
    TypeAnn ann;
    bool isConst = false;
    Expr* init = nullptr;
    const Ty* resolved = nullptr;  // Sema 填充的最终类型
    LetStmt(SourceLoc l, std::string n, SourceLoc nl, TypeAnn a, bool c, Expr* i)
        : Stmt(StmtKind::Let, l),
          name(std::move(n)),
          nameLoc(nl),
          ann(a),
          isConst(c),
          init(i) {}
};

struct ExprStmt : Stmt {
    Expr* expr = nullptr;
    ExprStmt(SourceLoc l, Expr* e) : Stmt(StmtKind::Expr, l), expr(e) {}
};

struct AssignStmt : Stmt {
    // 赋值目标是一个左值表达式（0.7 起统一为 Expr*）：
    //   x = v          -> IdentExpr
    //   a[i] = v       -> IndexExpr（可嵌套 a[i][j] = v）
    //   p.x = v        -> MemberExpr
    //   a[i].x = v     -> MemberExpr(IndexExpr(...))
    // 旧版的 target/index 字段在 0.7 合并为单一 target 表达式。
    Expr* target = nullptr;
    SourceLoc targetLoc;
    Expr* value = nullptr;
    // 复合赋值（a[i] += v 等，0.7）：在代码生成阶段读取旧值再写回，
    // target 的子表达式只会求值一次。
    bool compound = false;
    BinOp compoundOp = BinOp::Add;
    const Ty* elemTy = nullptr;  // Sema 填充：目标元素 / 字段类型
    AssignStmt(SourceLoc l, Expr* t, SourceLoc tl, Expr* v)
        : Stmt(StmtKind::Assign, l),
          target(t),
          targetLoc(tl),
          value(v) {}
};

struct IfStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* thenBranch = nullptr;  // 恒为 BlockStmt
    Stmt* elseBranch = nullptr;  // BlockStmt 或 IfStmt（else if 链）
    IfStmt(SourceLoc l, Expr* c, Stmt* t, Stmt* e)
        : Stmt(StmtKind::If, l), cond(c), thenBranch(t), elseBranch(e) {}
};

struct WhileStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* body = nullptr;
    WhileStmt(SourceLoc l, Expr* c, Stmt* b)
        : Stmt(StmtKind::While, l), cond(c), body(b) {}
};

// for 循环的两种形态（0.5 起支持 for-in 迭代）
enum class ForMode {
    Range,  // for i in a..b（闭开 / 闭闭区间）
    In,     // for x in iterable（迭代数组或字符串，具体类型见 varTy）
};

struct ForStmt : Stmt {
    std::string var;
    SourceLoc varLoc;
    Expr* start = nullptr;
    Expr* end = nullptr;
    bool inclusive = false;  // 0..n 为 false，0..=n 为 true
    // for-in（0.5 新增）：iterable 是被迭代的数组 / 字符串表达式
    ForMode mode = ForMode::Range;
    Expr* iterable = nullptr;
    const Ty* varTy = nullptr;  // Sema 填充：循环变量的类型（元素类型 / string）
    const Ty* iterTy =
        nullptr;  // Sema 填充：被迭代对象本身的类型（区分迭代 string 与 string[]）
    Stmt* body = nullptr;
    ForStmt(SourceLoc l, std::string v, SourceLoc vl, Expr* s, Expr* e, bool inc,
            Stmt* b)
        : Stmt(StmtKind::For, l),
          var(std::move(v)),
          varLoc(vl),
          start(s),
          end(e),
          inclusive(inc),
          body(b) {}
};

struct LoopStmt : Stmt {
    Stmt* body = nullptr;
    LoopStmt(SourceLoc l, Stmt* b) : Stmt(StmtKind::Loop, l), body(b) {}
};

struct BreakStmt : Stmt {
    explicit BreakStmt(SourceLoc l) : Stmt(StmtKind::Break, l) {}
};

struct ContinueStmt : Stmt {
    explicit ContinueStmt(SourceLoc l) : Stmt(StmtKind::Continue, l) {}
};

struct ReturnStmt : Stmt {
    Expr* value = nullptr;  // 可为 nullptr（void 返回）
    ReturnStmt(SourceLoc l, Expr* v) : Stmt(StmtKind::Return, l), value(v) {}
};

struct Param {
    std::string name;
    SourceLoc loc;
    TypeAnn ann;
    const Ty* ty = nullptr;
};

// struct 字段声明（0.7）
struct FieldDecl {
    std::string name;
    SourceLoc loc;
    TypeAnn ann;
    const Ty* ty = nullptr;  // Sema 填充
};

// struct 声明（0.7）： struct Point { let x: int; let y: int; }
// 表示层是堆上的字段对象（引用语义，与数组一致），字段布局为 N*8。
struct StructDecl {
    std::string name;
    SourceLoc nameLoc;
    SourceLoc loc;
    std::vector<FieldDecl> fields;
    std::string file;    // 所在源文件
    std::string module;  // 所属模块（空 = 根文件）
};

struct FuncDecl {
    std::string name;
    SourceLoc nameLoc;
    SourceLoc loc;
    std::vector<Param> params;
    TypeAnn retAnn;
    const Ty* retTy = TyStore::voidTy();
    BlockStmt* body = nullptr;
    bool isExtern = false;   // extern fn：只声明 C 函数，不生成函数体
    std::string file;        // 所在源文件（用于诊断与 #line 定位）
    std::string module;      // 所属模块（空 = 根文件 / 主程序）
};

// 顶层全局常量： const NAME: 类型 = 常量表达式;
struct GlobalConstDecl {
    std::string name;
    SourceLoc nameLoc;
    SourceLoc loc;
    TypeAnn ann;
    Expr* init = nullptr;
    const Ty* resolved = nullptr;
    std::string file;    // 所在源文件
    std::string module;  // 所属模块（空 = 根文件）
    bool synth = false;  // 标准库模块合成的声明（如 math 的 pi / e）
};

// 顶层 import 声明： import "path"; 或 import "path" as 别名;
// path 的几种形态：
//   "math"/"time"/"system"/"file"/"string"  标准库模块
//   "c:库名"                       链接 C 库（等价于 -l库名）
//   "./x.lux" / "x"                相对路径的 .lux 文件
//   "pkg"                          包目录（含 lux.json，或 main.lux/lib.lux）
// 不带 as 时模块成员注入当前命名空间（保持向后兼容）；
// 带 as 时只能通过 别名.成员 访问，避免命名空间污染。
struct ImportDecl {
    std::string path;
    std::string alias;  // 空 = 默认导入（成员可直接使用）
    SourceLoc loc;
    std::string file;  // 声明所在的源文件
    // 0.9.3：from "mod" import a, b; / from "mod" import *;
    bool selective = false;          // 是否选择性导入
    bool star = false;               // import *（等价于默认导入）
    std::vector<std::string> names;  // 选择性导入的成员名
};

struct Program {
    std::vector<FuncDecl*> funcs;
    std::vector<GlobalConstDecl*> consts;
    std::vector<ImportDecl*> imports;
    std::vector<StructDecl*> structs;  // struct 声明（0.7）
};

// =============================================================================
//  语法分析
// =============================================================================

// 语法错误经 Diags 记录后抛出的内部异常：被语句 / 顶层声明的恢复点捕获，
// 同步 token 流后继续解析，一次编译尽量报全所有语法错误。
struct ParseBail {};

class Parser {
public:
    Parser(std::vector<Token> toks, std::string filename, Diags& diags);
    // 解析整个文件；AST 节点由本对象的对象池持有，Parser 析构后失效
    Program* parseProgram();

private:
    std::vector<Token> toks_;
    std::string filename_;
    Diags& diags_;
    size_t pos_ = 0;
    int depth_ = 0;  // 表达式嵌套深度（防恶意深嵌套栈溢出）
    // 条件位置（if / while / for）禁止把 `ident {` 解析成 struct 字面量，
    // 否则 `while x { ... }` 会被误判（Rust 的 no_struct_literal 同款处理）。
    bool noStructLit_ = false;
    std::vector<std::unique_ptr<Expr>> exprPool_;
    std::vector<std::unique_ptr<Stmt>> stmtPool_;
    std::vector<std::unique_ptr<FuncDecl>> funcPool_;
    std::vector<std::unique_ptr<GlobalConstDecl>> constPool_;
    std::vector<std::unique_ptr<ImportDecl>> importPool_;
    std::vector<std::unique_ptr<StructDecl>> structPool_;
    Program program_;

    // token 流访问
    const Token& peek(size_t off = 0) const;
    const Token& cur() const { return peek(0); }
    Token advance();
    bool at(Tok k) const { return cur().kind == k; }
    // 0.9.3：'from' 是上下文关键字（只在顶层 "from \"路径\" import" 里
    // 生效），这样用户仍可把 from 当普通标识符（native_rt.lux 就有）。
    bool isFromKeyword() const {
        return cur().kind == Tok::Ident && cur().text == "from";
    }
    bool match(Tok k);
    Token expect(Tok k, const char* what);
    [[noreturn]] void errorHere(const std::string& msg);
    [[noreturn]] void errorAt(const Token& tk, const std::string& msg);

    // 错误恢复：把 token 流同步到下一个安全边界（语句 / 顶层声明）
    void syncStmt();
    void syncTopLevel();

    // 语法单元
    FuncDecl* parseFunc(bool isExtern = false);
    FuncDecl* parseExternFn();
    GlobalConstDecl* parseTopLevelConst();
    ImportDecl* parseImport();
    StructDecl* parseStruct();  // struct 声明（0.7）
    Stmt* parseStmt();
    BlockStmt* parseBlock();
    Stmt* parseLetOrConst();
    Stmt* parseIf();
    Stmt* parseIfRest(SourceLoc l);  // if 头之后的共同部分（供 elif 复用）
    Stmt* parseWhile();
    Stmt* parseFor();
    Stmt* parseReturn();

    Expr* parseExpr();
    Expr* parseOr();
    Expr* parseAnd();
    Expr* parseBitOr();
    Expr* parseBitXor();
    Expr* parseBitAnd();
    Expr* parseEquality();
    Expr* parseComparison();
    Expr* parseShift();
    Expr* parseAdditive();
    Expr* parseMultiplicative();
    Expr* parseUnary();
    Expr* parsePrimary();
    Expr* parsePrimaryBase();  // 不含后缀下标的原子表达式
    // if 表达式（0.7）：条件位置禁止 struct 字面量
    Expr* parseIfExpr();
    // 后缀：连续的 '[expr]' 与 '.member'（0.5 / 0.7）
    Expr* parsePostfix(Expr* e);
    // 解析 '(' 之后的参数列表并构造 CallExpr
    Expr* finishCall(SourceLoc l, const std::string& name, SourceLoc nameLoc);

    TypeAnn parseTypeAnn();    // 解析 ": 类型"
    const Ty* parseTypeTok();  // 只解析类型名部分

    template <typename T, typename... Args>
    T* newExpr(Args&&... args) {
        exprPool_.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return static_cast<T*>(exprPool_.back().get());
    }
    template <typename T, typename... Args>
    T* newStmt(Args&&... args) {
        stmtPool_.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return static_cast<T*>(stmtPool_.back().get());
    }

    int repeatId_ = 0;  // repeat 语法糖生成的隐藏循环变量编号
};

// =============================================================================
//  语义分析
// =============================================================================

// 模块系统信息：Loader 填充（哪些模块被 import、哪些是默认导入、别名映射），
// Sema 消费（裸名可见性 + 限定访问解析）。
struct ModuleInfo {
    std::set<std::string> imported;  // 已 import 的全部模块名（别名导入也算）
    std::set<std::string> flat;      // 默认导入（成员注入全局命名空间）
    std::unordered_map<std::string, std::string> aliases;  // 别名 -> 模块名
    // 0.9.3：from "mod" import a, b; —— 只把列出的成员注入全局命名空间
    std::unordered_map<std::string, std::set<std::string>> selected;

    // 成员 name 是否通过某个 import 注入全局命名空间
    bool memberVisible(const std::string& mod, const std::string& name) const {
        if (flat.count(mod)) return true;
        auto it = selected.find(mod);
        if (it == selected.end()) return false;
        return it->second.count("*") || it->second.count(name);
    }
};

// 对整个程序做符号解析与类型检查；发现错误时返回 false。
// needMain=false 用于编译原生后端的运行时库（无 main 入口）。
bool analyze(Program* prog, Diags& diags, const std::string& filename,
             const ModuleInfo& modules, bool needMain = true);

// =============================================================================
//  模块加载
// =============================================================================

// 递归加载根文件及其全部 import 得到的模块。
// 所有 Parser（AST 的所有者）保存在对象内部，析构后 AST 失效。
struct ModuleLoader {
    Diags& diags;
    SourceMap sources;                    // 已读取的文件 -> 源码（供诊断渲染）
    std::vector<std::unique_ptr<Parser>> parsers;
    Program merged;                       // 合并后的函数/常量
    ModuleInfo modules;                   // 模块注册信息（供 Sema 消费）
    std::vector<std::string> linkLibs;    // 需要 -l 链接的 C 库

    explicit ModuleLoader(Diags& d) : diags(d) {}

    // 加载根文件及其全部依赖；返回合并后的 Program*（永远非空，
    // 加载是否成功以 diags.ok() 为准）。
    Program* load(const std::string& rootPath);
    // 加载单个文件（内部使用，可递归）；module 为该文件的模块归属（根文件传空串）
    void loadFile(const std::string& path, const std::string& module);
    // 处理一条 import 声明（内部使用）
    void processImport(const ImportDecl* imp, const std::string& fromDir);
    // 把模块注册进 ModuleInfo（默认导入注入全局命名空间，别名导入只登记映射）
    void registerModule(const std::string& mod, const ImportDecl* imp);
    // 加载一个包目录（内部使用）
    void loadPackage(const std::string& dir, const std::string& module);

private:
    std::set<std::string> loaded_;  // 已加载文件（规范路径，去重并防循环 import）
    // 标准库模块合成的声明（如 math 的 pi / e 常量）
    std::vector<std::unique_ptr<GlobalConstDecl>> synthConsts_;
    std::vector<std::unique_ptr<Expr>> synthExprs_;
};

// =============================================================================
//  代码生成（C 后端）
// =============================================================================

struct CodegenOptions {
    bool emitLineMarks = false;  // 是否在生成的 C 里插入 Lux 源码行号注释
    bool arc = true;             // 0.9.4：ARC 默认开启（--no-arc 关闭）
    std::string sourceName;
};

// 返回生成的 C 源码；失败返回空串
std::string generateC(Program* prog, const CodegenOptions& opt);

// =============================================================================
//  代码生成（原生后端，0.6）
// =============================================================================

// 直接生成本机架构（x86-64 / aarch64）的 Linux ELF 可执行文件
//（不依赖 C 编译器 / libc）。失败返回 false（诊断经 diags 报告）。
bool generateNative(Program* prog, Diags& diags, const std::string& sourceName,
                    std::vector<uint8_t>& outElf);

// 0.9.4：原生后端引用计数开关（默认开，--no-arc 关闭）
void setNativeArc(bool on);

// =============================================================================
//  工具
// =============================================================================

// 读取整个文件；失败返回 std::nullopt
std::optional<std::string> readFile(const std::string& path);
bool writeFile(const std::string& path, const std::string& content);
// 把任意字符串转成合法的 C 字符串字面量（含转义）
std::string escapeCString(const std::string& s);
// 取路径的主文件名（去掉目录与最后一个扩展名）
std::string stemOf(const std::string& path);
// 取路径的最后一个分量（去掉目录）
std::string basenameOf(const std::string& path);

// 从极简 JSON 文本中读取 "key": "字符串值"（够 lux.json 用，失败返回 false）
bool jsonStringField(const std::string& text, const std::string& key,
                     std::string& out);
// 从极简 JSON 文本中读取 "key": ["s1", "s2", ...]（数组元素只能是字符串）
bool jsonStringArray(const std::string& text, const std::string& key,
                     std::vector<std::string>& out);

// -----------------------------------------------------------------------------
//  JSON（json.cpp）：包注册表 / lux.json 都是嵌套 JSON，需要完整解析
// -----------------------------------------------------------------------------
struct JsonValue {
    enum class Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    // 对象取字段（不存在 / 类型不是对象时返回 nullptr）
    const JsonValue* find(const std::string& key) const;
    // 数组按下标取元素
    const JsonValue* at(size_t idx) const;
    std::string asString(const std::string& def = "") const;
    double asNumber(double def = 0) const;
    bool asBool(bool def = false) const;
};

// 解析 JSON；失败时把原因（含行列）写入 err
bool jsonParse(const std::string& text, JsonValue& out, std::string& err);
// 转义成 JSON 字符串内容（不含两端的引号）
std::string jsonEscape(const std::string& s);
// 序列化（缩进两空格，末尾带换行）
std::string jsonDump(const JsonValue& v);

// SHA-256（十六进制小写），用于校验下载的包与生成注册表元数据
std::string sha256Hex(const std::string& data);
bool sha256File(const std::string& path, std::string& hex);

// 包管理根目录：优先 $LUX_HOME，否则 $HOME/.lux（都没有则 "./.lux"）
std::string luxHome();
// 已安装包的目录：<luxHome>/packages
std::string packagesDir();

// 返回路径分析（0.5 起由 Sema 与 Codegen 共用）：
// 判断一条语句执行后是否"必然"离开函数（return / if-else 双分支都返回 /
// 不含 break 的 while(true) 或 loop）。用于精确的"缺少 return"诊断；
// 注意：Codegen 不再依赖它省略兜底 return（0.5.1 起兜底 return 无条件生成，
// 正确性不押在分析的完备性上），本函数只喂给 Sema 的 W1003 警告。
bool stmtAlwaysReturns(const Stmt* s);

// =============================================================================
//  包管理（luxc add / list / delete）
// =============================================================================

// =============================================================================
//  交互式 REPL（luxc repl）
// =============================================================================

// 运行读-求值-打印循环；返回进程退出码
int runRepl();

struct PkgInfo {
    std::string name;
    std::string version;  // 已安装版本（未知 / 本地包为空）
    std::string main;     // 入口文件（相对包目录），未知时为空
    std::string source;   // 安装来源：registry / url / local
    std::string origin;   // 具体来源（URL 或本地路径）
    int files = 0;        // 包目录顶层 .lux 文件数量
    std::vector<std::string> deps;  // 依赖（"名字" 或 "名字@约束"）
};

// 安装本地包：path 是 .lux 文件或包目录；成功时把包名写入 name
bool pkgAdd(const std::string& path, std::string& name, std::string& err);
// 列出已安装的包（按名字排序）；还没安装过任何包时返回空
std::vector<PkgInfo> pkgList();
// 删除包
bool pkgDelete(const std::string& name, std::string& err);

// -----------------------------------------------------------------------------
//  在线包管理（0.9）：从注册表 / URL 下载安装
//
//  spec 的几种形态：
//    "mathx"              注册表能力范围内的最新版本
//    "mathx@1.2.0"        指定版本
//    "mathx@^1.2"         满足语义化版本约束的最高版本
//    "https://.../x.tar.gz" / "ftp://.../x.lux"   直接下载
//    "./mypkg" / "util.lux"                      本地路径（等价 add）
// -----------------------------------------------------------------------------

// 注册表地址：$LUX_REGISTRY 优先，否则 ~/.lux/config 的 "registry"，
// 最后回落到官方默认（https://lux.xfes.top/lux/lux.php）
std::string registryUrl();
// 把注册表地址写进 ~/.lux/config.json（空串 = 清除，回到默认）
bool setRegistryUrl(const std::string& url);

// 安装 spec；force=true 时覆盖已安装版本。成功时写回 installedName
bool pkgInstall(const std::string& spec, bool force, std::string& err,
                std::string& installedName);
// 刷新本地注册表缓存（~/.lux/cache/index.json）
bool pkgUpdateRegistry(std::string& err);
// 搜索注册表（query 为空 = 列出全部）；结果按名字排序
bool pkgSearch(const std::string& query, std::vector<PkgInfo>& out,
               std::string& err);
// 生成某个包的详细信息文本（来自注册表）
bool pkgInfo(const std::string& name, std::string& out, std::string& err);
// 把已安装包升级到注册表最新版本（name 为空 = 全部）
bool pkgUpgrade(const std::string& name, std::string& err);
// 发布本地包目录到注册表（HTTP 账号登录后走 API；ftp 注册表走 FTP）
bool pkgPublish(const std::string& dir, std::string& err);
// 删除自己发布在注册表上的包（HTTP API，需登录）
bool pkgUnpublish(const std::string& name, const std::string& version,
                  std::string& err);

// -----------------------------------------------------------------------------
//  账号（luxc login / logout / whoami）
// -----------------------------------------------------------------------------
// 当前 API 令牌：$LUX_TOKEN 优先，否则 ~/.lux/config.json 的 "token"
std::string authToken();
std::string authUser();
// 登录并把令牌写入配置；成功时把用户名写入 userOut
bool luxLogin(const std::string& user, const std::string& pass,
              std::string& userOut, std::string& err);
bool luxLogout(std::string& err);
bool luxWhoami(std::string& userOut, std::string& err);

}  // namespace lux
