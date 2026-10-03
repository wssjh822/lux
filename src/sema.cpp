// =============================================================================
//  sema.cpp : 语义分析 —— 符号解析、作用域管理、类型检查与推断、常量折叠
//
//  Sema 会在 AST 节点上就地写入解析结果：
//    * Expr::ty        每个表达式的静态类型
//    * LetStmt::resolved  变量最终确定的类型
//    * CallExpr::builtin / target  调用目标是内置函数还是用户函数
//    * BinaryExpr::folded   常量表达式的折叠结果
//
//  命名空间解析：
//    * 裸名（print / pow / greeting）       仅根文件 + 默认导入模块可见
//    * 限定名（math.pow / 别名.pow / lib.foo）任何已 import 的模块可见
// =============================================================================
#include "lux.hpp"

#include <cmath>
#include <set>
#include <unordered_map>

namespace lux {

namespace {

struct VarInfo {
    std::string name;
    const Ty* ty = nullptr;
    bool isConst = false;
    SourceLoc declLoc;
    GlobalConstDecl* constRef = nullptr;  // 非空 = 引用全局常量
    bool used = false;
};

// 内置函数签名表
const std::unordered_map<std::string, Builtin>& builtinTable() {
    static const std::unordered_map<std::string, Builtin> table = {
        {"print", Builtin::Print},   {"println", Builtin::PrintLn},
        {"len", Builtin::Len},       {"input", Builtin::Input},
        {"int", Builtin::ToInt},     {"float", Builtin::ToFloat},
        {"string", Builtin::ToString},{"assert", Builtin::Assert},
        {"exit", Builtin::Exit},     {"abs", Builtin::Abs},
        {"sqrt", Builtin::Sqrt},
        // math 模块
        {"pow", Builtin::Pow},       {"floor", Builtin::Floor},
        {"ceil", Builtin::Ceil},     {"round", Builtin::Round},
        {"sin", Builtin::Sin},       {"cos", Builtin::Cos},
        {"tan", Builtin::Tan},       {"asin", Builtin::Asin},
        {"acos", Builtin::Acos},     {"atan", Builtin::Atan},
        {"log", Builtin::Log},       {"log10", Builtin::Log10},
        {"exp", Builtin::Exp},       {"min", Builtin::Min},
        {"max", Builtin::Max},       {"atan2", Builtin::Atan2},
        {"fmod", Builtin::Fmod},     {"hypot", Builtin::Hypot},
        {"trunc", Builtin::Trunc},   {"isnan", Builtin::IsNan},
        {"isinf", Builtin::IsInf},   {"random", Builtin::Random},
        {"seed", Builtin::Seed},
        // time 模块
        {"now", Builtin::TimeNow},   {"monotonic", Builtin::TimeMono},
        {"sleep", Builtin::Sleep},   {"sleep_ms", Builtin::SleepMs},
        // system 模块
        {"system", Builtin::SysCall},{"env", Builtin::Env},
        {"setenv", Builtin::Setenv},
        // file 模块
        {"read", Builtin::FileRead}, {"write", Builtin::FileWrite},
        {"append", Builtin::FileAppend}, {"exists", Builtin::FileExists},
        {"remove", Builtin::FileRemove}, {"rename", Builtin::FileRename},
        // string 模块
        {"contains", Builtin::StrContains},
        {"startswith", Builtin::StrStartsWith},
        {"endswith", Builtin::StrEndsWith},
        {"find", Builtin::StrFind},
        {"find_opt", Builtin::StrFindOpt},
        {"replace", Builtin::StrReplace},
        {"trim", Builtin::StrTrim},
        {"upper", Builtin::StrUpper},
        {"lower", Builtin::StrLower},
        {"substr", Builtin::StrSubstr},
        {"format", Builtin::StrFormat},
        {"split", Builtin::StrSplit},
        {"chars", Builtin::StrChars},
        {"join", Builtin::StrJoin},
        // 全局错误通道（0.9.4）：无 import 即可用
        {"last_error", Builtin::LastError},
        {"byte_at", Builtin::ByteAt},
        {"bytes", Builtin::Bytes},
        {"list_dir", Builtin::ListDir},
        // net 模块（1.2）
        {"dial", Builtin::NetDial},       {"send", Builtin::NetSend},
        {"recv", Builtin::NetRecv},       {"close", Builtin::NetClose},
        {"listen", Builtin::NetListen},   {"accept", Builtin::NetAccept},
        {"set_timeout", Builtin::NetSetTimeout},
        {"map", Builtin::Map},
        {"filter", Builtin::Filter},
        {"map_opt", Builtin::MapOpt},
        // 数组方法（a.push 等）不在此表：它们只经"变量.方法"路径解析，
        // 避免与用户自定义的 push / pop 等函数名冲突。
    };
    return table;
}

// 内置函数所属的标准库模块；返回 nullptr 表示随时可用、无需 import
const char* builtinModule(Builtin b) {
    switch (b) {
        case Builtin::Pow:
        case Builtin::Floor:
        case Builtin::Ceil:
        case Builtin::Round:
        case Builtin::Sin:
        case Builtin::Cos:
        case Builtin::Tan:
        case Builtin::Asin:
        case Builtin::Acos:
        case Builtin::Atan:
        case Builtin::Log:
        case Builtin::Log10:
        case Builtin::Exp:
        case Builtin::Min:
        case Builtin::Max:
        case Builtin::Atan2:
        case Builtin::Fmod:
        case Builtin::Hypot:
        case Builtin::Trunc:
        case Builtin::IsNan:
        case Builtin::IsInf:
        case Builtin::Random:
        case Builtin::Seed:
            return "math";
        case Builtin::TimeNow:
        case Builtin::TimeMono:
        case Builtin::Sleep:
        case Builtin::SleepMs:
            return "time";
        case Builtin::SysCall:
        case Builtin::Env:
        case Builtin::Setenv:
            return "system";
        case Builtin::FileRead:
        case Builtin::FileWrite:
        case Builtin::FileAppend:
        case Builtin::FileExists:
        case Builtin::FileRemove:
        case Builtin::FileRename:
        case Builtin::ListDir:
            return "file";
        case Builtin::StrContains:
        case Builtin::StrStartsWith:
        case Builtin::StrEndsWith:
        case Builtin::StrFind:
        case Builtin::StrFindOpt:
        case Builtin::StrReplace:
        case Builtin::StrTrim:
        case Builtin::StrUpper:
        case Builtin::StrLower:
        case Builtin::StrSubstr:
        case Builtin::StrFormat:
        case Builtin::StrSplit:
        case Builtin::StrChars:
        case Builtin::StrJoin:
            return "string";
        case Builtin::NetDial:
        case Builtin::NetSend:
        case Builtin::NetRecv:
        case Builtin::NetClose:
        case Builtin::NetListen:
        case Builtin::NetAccept:
        case Builtin::NetSetTimeout:
            return "net";
        default:
            return nullptr;
    }
}

// extern fn 直接引用 C 符号，与运行时 / 标准库撞名会产生难以排查的问题
const std::set<std::string>& reservedExternNames() {
    static const std::set<std::string> names = {
        "malloc",  "calloc",  "realloc", "free",   "exit",   "abort",
        "system",  "getenv",  "printf",  "puts",   "fopen",  "fclose",
        "fread",   "fwrite",  "strlen",  "strcmp", "strcpy", "memcpy",
        "memset",  "strstr",  "strtol",  "strtod", "snprintf",
    };
    return names;
}

// 能否把 from 类型的值隐式转换为 to 类型
bool canCoerce(const Ty* from, const Ty* to) {
    if (!from || !to) return true;
    if (from->kind == TyKind::Invalid || to->kind == TyKind::Invalid)
        return true;  // 抑制级联报错
    if (from == to) return true;
    if (from->kind == TyKind::Int && to->kind == TyKind::Float)
        return true;  // int 自动提升为 float
    // 0.8 错误通道：T -> T?（自动装箱）；int -> float? 先提升再装箱。
    // 注意：T? -> T 不自动转换（必须显式 ? / or / !），否则错误通道会泄漏。
    if (to->kind == TyKind::Optional) {
        if (from == to->elem) return true;
        if (from->kind == TyKind::Int && to->elem &&
            to->elem->kind == TyKind::Float)
            return true;
    }
    return false;
}

// 是否为可选类型 T?
bool isOptional(const Ty* t) {
    return t && t->kind == TyKind::Optional;
}

// 拆分限定名 "prefix.member"（只支持一级限定）
bool splitQualified(const std::string& name, std::string& prefix,
                    std::string& member) {
    size_t dot = name.find('.');
    if (dot == std::string::npos) return false;
    prefix = name.substr(0, dot);
    member = name.substr(dot + 1);
    return true;
}

// 别名优先，其次把前缀本身当模块名
std::string resolveModule(const ModuleInfo& modules, const std::string& prefix) {
    auto it = modules.aliases.find(prefix);
    if (it != modules.aliases.end()) return it->second;
    return prefix;
}

struct Analyzer {
    Program* prog;
    Diags& diags;
    std::string filename;
    const ModuleInfo& modules;

    const Ty* tInvalid = TyStore::invalid();
    const Ty* tInt = TyStore::int64Ty();
    const Ty* tFloat = TyStore::float64Ty();
    const Ty* tBool = TyStore::boolTy();
    const Ty* tString = TyStore::stringTy();
    const Ty* tVoid = TyStore::voidTy();
    const Ty* tIntArr = TyStore::arrayOf(tInt);

    std::unordered_map<std::string, FuncDecl*> funcs;     // 裸名可见的函数
    std::unordered_map<std::string, FuncDecl*> modFuncs;  // "mod\x01name"
    std::unordered_map<std::string, StructDecl*> structs; // struct 名 -> 声明（0.7）
    std::vector<std::unordered_map<std::string, VarInfo>> scopes;
    std::unordered_map<std::string, VarInfo> qConsts;     // "mod\x01name" 全局常量
    FuncDecl* curFunc = nullptr;
    std::string curFile;  // 当前正在检查的声明所在文件（用于诊断定位）
    int loopDepth = 0;
    bool needMain_ = true;  // false = 运行时库编译（跳过 main 入口检查）

    Analyzer(Program* p, Diags& d, std::string f, const ModuleInfo& mods,
             bool needMainFlag = true)
        : prog(p),
          diags(d),
          filename(std::move(f)),
          modules(mods),
          needMain_(needMainFlag) {}

    // 把诊断信息关联到当前文件
    void err(SourceLoc loc, const std::string& msg) {
        diags.error(DiagCode::kSema, curFile, loc, msg);
    }
    void warn(const char* code, SourceLoc loc, const std::string& msg) {
        diags.warn(code, curFile, loc, msg);
    }

    // ---------------- 作用域 ----------------
    void pushScope() { scopes.emplace_back(); }
    // forceWarn: 函数参数作用域弹出时也要检查未使用
    void popScope(bool forceWarn = false) {
        if (forceWarn || scopes.size() > 1) {
            for (const auto& kv : scopes.back()) {
                const VarInfo& v = kv.second;
                // 编译器内部生成的隐藏变量（如 repeat 的循环变量）跳过；
                // 下划线开头的变量按惯例表示"故意不用"，也不警告（0.5 新增）
                if (v.used || kv.first.compare(0, 2, "__") == 0 ||
                    kv.first.compare(0, 1, "_") == 0)
                    continue;
                warn(DiagCode::kWUnusedVar, v.declLoc,
                     "变量 '" + kv.first + "' 声明了但从未使用");
            }
        }
        scopes.pop_back();
    }

    VarInfo* findVar(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    VarInfo* lookup(const std::string& name) {
        VarInfo* v = findVar(name);
        if (v) v->used = true;
        return v;
    }

    bool declare(const VarInfo& v) {
        if (scopes.empty()) return true;
        auto& top = scopes.back();
        if (top.count(v.name)) {
            err(v.declLoc, "变量 '" + v.name + "' 在当前作用域中已经声明过了");
            return false;
        }
        top[v.name] = v;
        return true;
    }

    // ---------------- struct 查找（0.7） ----------------
    StructDecl* findStruct(const std::string& name) {
        auto it = structs.find(name);
        return it == structs.end() ? nullptr : it->second;
    }
    // 字段下标；找不到返回 -1
    static int fieldIndex(const StructDecl* sd, const std::string& name) {
        for (size_t i = 0; i < sd->fields.size(); i++) {
            if (sd->fields[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    // ---------------- fn 类型（1.1：函数成为值） ----------------
    // 具名函数能否作为某个 fn 类型的值：参数与返回类型必须逐项完全相同。
    // 类型已 intern 化，指针比较即结构相等。
    static bool fnMatchesType(const FuncDecl* fn, const Ty* ft) {
        if (!fn || !ft || ft->kind != TyKind::Fn) return false;
        if (fn->isExtern) return false;
        if (fn->params.size() != ft->members.size()) return false;
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (fn->params[i].ty != ft->members[i]) return false;
        }
        return fn->retTy == ft->elem;
    }

    static std::string fnSignName(const FuncDecl* fn) {
        std::vector<const Ty*> ps;
        for (const Param& p : fn->params) ps.push_back(p.ty);
        return tyName(TyStore::fnOf(fn->retTy, std::move(ps)));
    }

    // 取具名函数的真实 fn 类型（供 map/filter/map_opt 给函数名实参作 hint）
    const Ty* fnTypeOfName(const std::string& name) {
        auto it = funcs.find(name);
        if (it == funcs.end() || it->second->isExtern) return nullptr;
        std::vector<const Ty*> ps;
        for (const Param& p : it->second->params) ps.push_back(p.ty);
        return TyStore::fnOf(it->second->retTy, std::move(ps));
    }

    // ---------------- 赋值左值检查（0.7） ----------------
    // 返回左值根变量（用于 const 可写性检查）；非变量根返回 nullptr
    static IdentExpr* lvalueRoot(Expr* t) {
        while (t && (t->kind == ExprKind::Index || t->kind == ExprKind::Member)) {
            t = (t->kind == ExprKind::Index)
                    ? static_cast<IndexExpr*>(t)->base
                    : static_cast<MemberExpr*>(t)->base;
        }
        return (t && t->kind == ExprKind::Ident)
                   ? static_cast<IdentExpr*>(t)
                   : nullptr;
    }

    // 检查赋值目标是否可写并返回它的类型（元素 / 字段 / 变量类型）。
    // 结构：Ident | Index(数组) | Member(struct)，可任意嵌套（a[i].x）。
    const Ty* checkAssignTarget(Expr* t) {
        if (!t) return tInvalid;
        switch (t->kind) {
            case ExprKind::Ident: {
                auto* id = static_cast<IdentExpr*>(t);
                VarInfo* v = lookup(id->name);
                if (!v) {
                    err(id->loc, "未定义的变量 '" + id->name +
                                         "'，无法对它赋值");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                id->constRef = v->constRef;
                if (v->isConst) {
                    err(id->loc, "常量 '" + id->name +
                                         "' 用 const 声明，不能被重新赋值");
                }
                t->ty = v->ty;
                return v->ty;
            }
            case ExprKind::Index: {
                auto* ix = static_cast<IndexExpr*>(t);
                const Ty* bt = checkExpr(ix->base);
                const Ty* it = checkExpr(ix->index);
                if (it != tInvalid && it != tInt) {
                    typeError(ix->index->loc, it, tInt, "下标必须是 int");
                }
                if (bt == tInvalid) {
                    t->ty = tInvalid;
                    return tInvalid;
                }
                if (bt == tString) {
                    err(ix->loc,
                        "字符串不可变，不能用 s[i] = ... 修改（请用拼接构造新串）");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                if (bt->kind != TyKind::Array) {
                    err(ix->loc, "'" + tyName(bt) +
                                         "' 类型不支持下标赋值（只有数组可用）");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                checkConstRoot(ix, "数组", "元素");
                t->ty = bt->elem;
                return bt->elem;
            }
            case ExprKind::Member: {
                auto* m = static_cast<MemberExpr*>(t);
                const Ty* bt = checkExpr(m->base);
                if (bt == tInvalid) {
                    t->ty = tInvalid;
                    return tInvalid;
                }
                if (bt->kind != TyKind::Named) {
                    err(m->memberLoc, "'" + tyName(bt) + "' 类型没有成员 '" +
                                              m->member + "'");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                StructDecl* sd = findStruct(bt->name);
                if (!sd) {
                    err(m->memberLoc, "未知的 struct 类型 '" + bt->name + "'");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                int idx = fieldIndex(sd, m->member);
                if (idx < 0) {
                    err(m->memberLoc, "struct '" + sd->name + "' 没有字段 '" +
                                              m->member + "'");
                    t->ty = tInvalid;
                    return tInvalid;
                }
                checkConstRoot(m, "struct", "字段");
                m->fieldIndex = idx;
                t->ty = sd->fields[idx].ty;
                return t->ty;
            }
            default:
                err(t->loc, "赋值号左边不是可写的位置");
                return tInvalid;
        }
    }

    // 左值根变量是 const 时禁止修改其内容（与数组 const 规则一致）
    void checkConstRoot(Expr* t, const char* kind, const char* part) {
        IdentExpr* r = lvalueRoot(t);
        if (!r) return;
        VarInfo* rv = findVar(r->name);
        if (rv && rv->isConst) {
            err(t->loc, std::string("用 const 声明的 ") + kind + " '" + r->name +
                            "' 不能修改它的" + part);
        }
    }

    // ---------------- 类型不匹配的通用报错 ----------------
    void typeError(SourceLoc loc, const Ty* from, const Ty* to,
                   const std::string& ctx) {
        std::string hint;
        if (from == tFloat && to == tInt) {
            hint = "（float 转 int 会丢失小数部分，请显式写成 int(x)）";
        } else if (from == tString || to == tString) {
            hint = "（字符串与数值之间需要显式转换，例如 int(s) / string(x)）";
        } else if (from == tVoid) {
            hint = "（void 表达式没有值，不能参与运算或赋值）";
        } else if (from && from->kind == TyKind::Array && to &&
                   to->kind == TyKind::Array) {
            hint = "（数组元素类型必须一致，int[] 不能隐式转成 " + tyName(to) +
                   "，但字面量里的 int 元素可以自动提升为 float）";
        }
        err(loc, ctx + "：这里是 '" + tyName(from) + "' 类型，但需要 '" +
                     tyName(to) + "' 类型" + hint);
    }

    // ---------------- 数组字面量的元素类型适配（0.5） ----------------
    // 当目标是 T[] 且字面量元素都是可提升的数值时，把字面量的元素类型
    // 调整为目标的元素类型（如 [1, 2, 3] 适配 float[]）。成功返回 true。
    bool tryCoerceArrayLit(Expr* e, const Ty* target) {
        if (!e || e->kind != ExprKind::ArrayLit) return false;
        if (!target || target->kind != TyKind::Array) return false;
        auto* al = static_cast<ArrayLitExpr*>(e);
        if (target->elem != tFloat) return false;
        for (const Expr* el : al->elems) {
            if (!el || (el->ty != tInt && el->ty != tFloat)) return false;
        }
        al->ty = target;
        al->elemTy = tFloat;
        return true;
    }

    // 便捷包装：类型不匹配时先尝试数组字面量适配，再报错
    void requireCoerce(Expr* e, const Ty* from, const Ty* to,
                       const std::string& ctx, SourceLoc loc) {
        if (from == tInvalid || to == tInvalid) return;
        if (canCoerce(from, to)) return;
        if (tryCoerceArrayLit(e, to)) return;
        typeError(loc, from, to, ctx);
    }

    // 全局常量初始值是否可在编译期确定：字面量 / 已折叠的运算 /
    // 引用另一个同样可折叠的全局常量（const B = A;）。
    bool constFoldable(const Expr* e) {
        if (!e) return false;
        switch (e->kind) {
            case ExprKind::IntLit:
            case ExprKind::FloatLit:
            case ExprKind::BoolLit:
            case ExprKind::StrLit:
                return true;
            case ExprKind::Ident: {
                auto* id = static_cast<const IdentExpr*>(e);
                return id->constRef && constFoldable(id->constRef->init);
            }
            case ExprKind::Binary:
                return static_cast<const BinaryExpr*>(e)->folded;
            default:
                return false;
        }
    }

    // ---------------- 常量折叠 ----------------
    // 取表达式可折叠的字面量形态：0=不可折叠 1=int 2=float 3=string。
    // 引用已声明全局常量的标识符会递归取它的初始值（无环：前向引用报错）。
    int litKind(const Expr* e) {        switch (e->kind) {
            case ExprKind::IntLit: return 1;
            case ExprKind::FloatLit: return 2;
            case ExprKind::StrLit: return 3;
            case ExprKind::Ident: {
                auto* id = static_cast<const IdentExpr*>(e);
                if (id->constRef && id->constRef->init) {
                    return litKind(id->constRef->init);
                }
                return 0;
            }
            case ExprKind::Binary: {
                auto* b = static_cast<const BinaryExpr*>(e);
                if (!b->folded) return 0;
                if (b->foldedIsStr) return 3;
                return (b->ty && b->ty->kind == TyKind::Float) ? 2 : 1;
            }
            default: return 0;
        }
    }
    long long litI(const Expr* e) {
        if (e->kind == ExprKind::IntLit)
            return static_cast<const IntLitExpr*>(e)->value;
        if (e->kind == ExprKind::Ident) {
            auto* id = static_cast<const IdentExpr*>(e);
            if (id->constRef && id->constRef->init) return litI(id->constRef->init);
        }
        return static_cast<const BinaryExpr*>(e)->foldedI;
    }
    double litF(const Expr* e) {
        if (e->kind == ExprKind::FloatLit)
            return static_cast<const FloatLitExpr*>(e)->value;
        if (e->kind == ExprKind::Ident) {
            auto* id = static_cast<const IdentExpr*>(e);
            if (id->constRef && id->constRef->init) return litF(id->constRef->init);
        }
        return static_cast<const BinaryExpr*>(e)->foldedF;
    }
    std::string litS(const Expr* e) {
        if (e->kind == ExprKind::StrLit)
            return static_cast<const StrLitExpr*>(e)->value;
        if (e->kind == ExprKind::Ident) {
            auto* id = static_cast<const IdentExpr*>(e);
            if (id->constRef && id->constRef->init) return litS(id->constRef->init);
        }
        return static_cast<const BinaryExpr*>(e)->foldedS;
    }

    // 字面量之间的算术 / 位运算折叠成字面量；无法安全折叠时保持原样
    void tryFold(BinaryExpr* b) {
        if (b->op == BinOp::LogicAnd || b->op == BinOp::LogicOr ||
            b->op == BinOp::Eq || b->op == BinOp::Ne || b->op == BinOp::Lt ||
            b->op == BinOp::Le || b->op == BinOp::Gt || b->op == BinOp::Ge) {
            return;
        }
        int lk = litKind(b->lhs), rk = litKind(b->rhs);
        if (!lk || !rk) return;

        // 字符串拼接折叠
        if (b->ty == tString) {
            if (lk == 3 && rk == 3) {
                b->folded = true;
                b->foldedIsStr = true;
                b->foldedS = litS(b->lhs) + litS(b->rhs);
            }
            return;
        }
        if (!isNumeric(b->ty)) return;

        if (b->ty == tFloat) {
            double a = lk == 2 ? litF(b->lhs) : static_cast<double>(litI(b->lhs));
            double r = rk == 2 ? litF(b->rhs) : static_cast<double>(litI(b->rhs));
            double v = 0.0;
            switch (b->op) {
                case BinOp::Add: v = a + r; break;
                case BinOp::Sub: v = a - r; break;
                case BinOp::Mul: v = a * r; break;
                case BinOp::Div: v = a / r; break;  // 浮点除 0 合法（±inf）
                default: return;
            }
            // nan / inf 没有合法的 C 字面量，保留运行时计算
            if (!std::isfinite(v)) return;
            b->folded = true;
            b->foldedIsStr = false;
            b->foldedF = v;
            return;
        }

        // 整数折叠：用 __int128 检测溢出，溢出时不折叠（保留运行时行为）
        __int128 a = litI(b->lhs);
        __int128 r = litI(b->rhs);
        __int128 v = 0;
        switch (b->op) {
            case BinOp::Add: v = a + r; break;
            case BinOp::Sub: v = a - r; break;
            case BinOp::Mul: v = a * r; break;
            case BinOp::Div:
                if (r == 0) return;  // 除零交给运行时 panic，不折叠
                v = a / r;
                break;
            case BinOp::Mod:
                if (r == 0) return;
                v = a % r;
                break;
            case BinOp::BitAnd: v = a & r; break;
            case BinOp::BitOr: v = a | r; break;
            case BinOp::BitXor: v = a ^ r; break;
            case BinOp::Shl:
                if (r < 0 || r >= 64) return;
                v = a << r;
                break;
            case BinOp::Shr:
                if (r < 0 || r >= 64) return;
                v = a >> r;
                break;
            default: return;
        }
        if (v < INT64_MIN || v > INT64_MAX) return;  // 溢出：保留运行时行为
        b->folded = true;
        b->foldedIsStr = false;
        b->foldedI = static_cast<long long>(v);
    }

    // ---------------- 表达式 ----------------
    // hint：调用方给出的"期望类型"提示（如 let 标注 / 参数类型 / return 类型），
    // 目前只用于数组字面量的元素类型推断（空数组 [] 需要它才能确定类型）。
    const Ty* checkExpr(Expr* e, const Ty* hint = nullptr) {
        if (!e) return tInvalid;
        switch (e->kind) {
            case ExprKind::IntLit:
            case ExprKind::FloatLit:
            case ExprKind::BoolLit:
            case ExprKind::StrLit:
                // 字面量类型在构造时已确定
                return e->ty;

            case ExprKind::Ident: {
                auto* id = static_cast<IdentExpr*>(e);
                std::string prefix, member;
                if (splitQualified(id->name, prefix, member)) {
                    // 模块限定访问： mod.常量
                    std::string mod = resolveModule(modules, prefix);
                    if (!modules.imported.count(mod)) {
                        err(id->loc, "模块 '" + mod +
                                             "' 没有被 import（请先写 import \"" +
                                             mod + "\"）");
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    auto it = qConsts.find(mod + "\x01" + member);
                    if (it == qConsts.end()) {
                        err(id->loc, "模块 '" + mod + "' 中没有常量 '" + member +
                                             "'");
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    id->constRef = it->second.constRef;
                    e->ty = it->second.ty;
                    return it->second.ty;
                }
                VarInfo* v = lookup(id->name);
                if (!v) {
                    // 1.1：具名函数作为值（fn 类型）——需要上下文的 fn 类型 hint
                    auto fit = funcs.find(id->name);
                    if (fit != funcs.end()) {
                        FuncDecl* fn = fit->second;
                        if (!hint || hint->kind != TyKind::Fn) {
                            err(id->loc,
                                "函数 '" + id->name +
                                    "' 只能作为 fn 类型的值使用（需要类型标注，"
                                    "如 let f: fn(int) -> int = " +
                                    id->name + ";）");
                        } else if (fn->isExtern) {
                            err(id->loc, "extern fn '" + id->name +
                                             "' 不能作为值（原生后端没有 C 符号）");
                        } else if (fn->name == "main" && fn->module.empty()) {
                            err(id->loc, "main 函数不能作为值使用");
                        } else if (!fnMatchesType(fn, hint)) {
                            err(id->loc, "函数 '" + id->name + "' 的签名是 '" +
                                             fnSignName(fn) + "'，与所需的 '" +
                                             tyName(hint) + "' 不匹配");
                        } else {
                            id->funcRef = fn;
                            e->ty = hint;
                            return hint;
                        }
                    } else {
                        err(id->loc, "未定义的变量 '" + id->name + "'");
                    }
                    e->ty = tInvalid;
                    return tInvalid;
                }
                id->constRef = v->constRef;
                e->ty = v->ty;
                return v->ty;
            }

            case ExprKind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                const Ty* ot = checkExpr(u->operand);
                switch (u->op) {
                    case UnOp::Neg:
                        if (isNumeric(ot)) {
                            e->ty = ot;
                        } else if (ot != tInvalid) {
                            err(u->loc, "一元 '-' 只能用于 int 或 float，不能用于 '" +
                                                    tyName(ot) + "'");
                            e->ty = tInvalid;
                        } else {
                            e->ty = tInvalid;
                        }
                        break;
                    case UnOp::LogicNot:
                        if (ot == tBool) {
                            e->ty = tBool;
                        } else if (ot != tInvalid) {
                            err(u->loc, "'!' 只能用于 bool，不能用于 '" + tyName(ot) +
                                                    "'");
                            e->ty = tInvalid;
                        } else {
                            e->ty = tInvalid;
                        }
                        break;
                    case UnOp::BitNot:
                        if (ot == tInt) {
                            e->ty = tInt;
                        } else if (ot != tInvalid) {
                            err(u->loc, "'~' 只能用于 int，不能用于 '" + tyName(ot) +
                                                    "'");
                            e->ty = tInvalid;
                        } else {
                            e->ty = tInvalid;
                        }
                        break;
                }
                return e->ty;
            }

            case ExprKind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                const Ty* lt = checkExpr(b->lhs);
                // 0.8（A3.1）：`or` 关键字按左操作数类型分派。
                // 左边是 T? → 兜底（fallback）；否则仍是逻辑或。
                if (b->op == BinOp::LogicOr && b->orKeyword &&
                    isOptional(lt)) {
                    b->orFallback = true;
                    const Ty* elem = lt->elem;
                    const Ty* rt = checkExpr(b->rhs, elem);
                    if (rt == tInvalid) {
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    if (!canCoerce(rt, elem) &&
                        !tryCoerceArrayLit(b->rhs, elem)) {
                        typeError(b->rhs->loc, rt, elem,
                                  "'or' 右侧的兜底值类型与可选值内部类型不匹配");
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    e->ty = elem;
                    return e->ty;
                }
                const Ty* rt = checkExpr(b->rhs);
                if (lt == tInvalid || rt == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (isOptional(lt) || isOptional(rt)) {
                    err(b->loc,
                        "不能直接对可选值做运算（这里是 '" + tyName(lt) +
                            "' 和 '" + tyName(rt) +
                            "'）；请先用 ? 传播、or 兜底或 ! 解包");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                e->ty = checkBinary(b->op, lt, rt, b->loc);
                if (e->ty != tInvalid) tryFold(b);
                return e->ty;
            }

            case ExprKind::Call:
                return checkCall(static_cast<CallExpr*>(e));

            case ExprKind::Index: {
                // 下标访问 a[i]（数组取元素）或 s[i]（字符串取单字节字符）
                auto* ix = static_cast<IndexExpr*>(e);
                const Ty* bt = checkExpr(ix->base);
                const Ty* it = checkExpr(ix->index);
                if (it != tInvalid && it != tInt) {
                    typeError(ix->index->loc, it, tInt,
                              "下标必须是 int");
                }
                if (bt == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (bt->kind == TyKind::Array) {
                    e->ty = bt->elem;
                    return bt->elem;
                }
                if (bt == tString) {
                    e->ty = tString;  // 单字节字符仍是 string
                    return tString;
                }
                err(ix->loc, "'" + tyName(bt) +
                                 "' 类型不支持下标访问（只有数组 int[] 等和"
                                 "字符串 string 可以用 a[i]）");
                e->ty = tInvalid;
                return tInvalid;
            }

            case ExprKind::Slice: {
                // a[lo..hi] / a[lo..=hi]（0.6 新增）：复制语义切片
                auto* sl = static_cast<SliceExpr*>(e);
                const Ty* bt = checkExpr(sl->base);
                if (bt == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                // 端点：省略 start = 0，省略 end = 到末尾（运行时定长）
                if (sl->start) {
                    const Ty* st = checkExpr(sl->start);
                    if (st != tInvalid && st != tInt) {
                        typeError(sl->start->loc, st, tInt,
                                  "切片起点必须是 int");
                    }
                }
                if (sl->end) {
                    const Ty* en = checkExpr(sl->end);
                    if (en != tInvalid && en != tInt) {
                        typeError(sl->end->loc, en, tInt,
                                  "切片终点必须是 int");
                    }
                }
                if (bt->kind == TyKind::Array) {
                    e->ty = bt;  // 结果是同元素类型的新数组
                    return bt;
                }
                if (bt == tString) {
                    e->ty = tString;
                    return tString;
                }
                err(sl->loc, "'" + tyName(bt) +
                                 "' 类型不支持切片（只有数组和字符串可以"
                                 "用 a[lo..hi]）");
                e->ty = tInvalid;
                return tInvalid;
            }

            case ExprKind::ArrayLit: {
                // 数组字面量：元素类型统一推断；int/float 混合提升为 float[]
                auto* al = static_cast<ArrayLitExpr*>(e);
                if (al->elems.empty()) {
                    // 空数组：只能靠调用方的类型标注 / 参数类型确定元素类型
                    if (hint && hint->kind == TyKind::Array) {
                        al->elemTy = hint->elem;
                        e->ty = hint;
                        return hint;
                    }
                    err(e->loc,
                        "空数组字面量 [] 无法推断元素类型，请写显式类型标注，"
                        "例如 let a: int[] = [];");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                const Ty* elem = nullptr;
                for (Expr* el : al->elems) {
                    // 嵌套的空数组字面量继承本层的元素类型提示
                    const Ty* elHint =
                        (elem && elem->kind == TyKind::Array) ? elem : nullptr;
                    const Ty* t = checkExpr(el, elHint);
                    if (t == tInvalid) {
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    if (t == tVoid) {
                        err(el->loc, "数组元素不能是 void 表达式");
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                    if (!elem) {
                        elem = t;
                        continue;
                    }
                    if (elem == t) continue;
                    // 数值混合统一提升为 float；其余情况取更宽的类型
                    if (isNumeric(elem) && isNumeric(t)) {
                        elem = tFloat;
                        continue;
                    }
                    err(el->loc,
                        "数组字面量的元素类型不一致：前面是 '" + tyName(elem) +
                            "'，这里出现 '" + tyName(t) + "'（嵌套数组的元素"
                            "类型也必须完全一致）");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                al->elemTy = elem;
                e->ty = TyStore::arrayOf(elem);
                return e->ty;
            }

            case ExprKind::StructLit: {
                // struct 字面量 Point { x: 1, y: 2 }（0.7）
                auto* sl = static_cast<StructLitExpr*>(e);
                StructDecl* sd = findStruct(sl->typeName);
                if (!sd) {
                    err(sl->typeLoc,
                        "未定义的 struct 类型 '" + sl->typeName +
                            "'（struct 要先声明后使用）");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                sl->decl = sd;
                std::vector<bool> seen(sd->fields.size(), false);
                for (auto& kv : sl->inits) {
                    int idx = fieldIndex(sd, kv.first);
                    if (idx < 0) {
                        err(kv.second ? kv.second->loc : sl->loc,
                            "struct '" + sd->name + "' 没有字段 '" + kv.first +
                                "'");
                        checkExpr(kv.second);
                        continue;
                    }
                    if (seen[idx]) {
                        err(kv.second ? kv.second->loc : sl->loc,
                            "struct '" + sd->name + "' 的字段 '" + kv.first +
                                "' 被重复初始化");
                    }
                    seen[idx] = true;
                    const Ty* ft = sd->fields[idx].ty;
                    const Ty* at = checkExpr(kv.second, ft);
                    if (at != tInvalid && ft != tInvalid &&
                        !canCoerce(at, ft) &&
                        !tryCoerceArrayLit(kv.second, ft)) {
                        typeError(kv.second->loc, at, ft,
                                  "struct 字段 '" + kv.first + "' 的初始值类型不匹配");
                    }
                }
                for (size_t i = 0; i < sd->fields.size(); i++) {
                    if (!seen[i]) {
                        err(sl->loc, "struct '" + sd->name + "' 的字段 '" +
                                         sd->fields[i].name +
                                         "' 未初始化（构造时必须给出全部字段）");
                    }
                }
                e->ty = TyStore::named(sd->name);
                return e->ty;
            }

            case ExprKind::Member: {
                // 成员访问 p.x（0.7）；也用于模块限定常量 math.pi
                auto* m = static_cast<MemberExpr*>(e);
                if (m->base && m->base->kind == ExprKind::Ident) {
                    auto* id = static_cast<IdentExpr*>(m->base);
                    std::string mod = resolveModule(modules, id->name);
                    if (modules.imported.count(mod)) {
                        auto it = qConsts.find(mod + "\x01" + m->member);
                        if (it != qConsts.end()) {
                            m->constRef = it->second.constRef;
                            e->ty = it->second.ty;
                            return e->ty;
                        }
                        err(m->memberLoc, "模块 '" + mod + "' 中没有常量 '" +
                                              m->member + "'");
                        e->ty = tInvalid;
                        return tInvalid;
                    }
                }
                const Ty* bt = checkExpr(m->base);
                if (bt == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (bt->kind != TyKind::Named) {
                    err(m->memberLoc,
                        "'" + tyName(bt) + "' 类型没有成员 '" + m->member +
                            "'（成员访问只适用于 struct）");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                StructDecl* sd = findStruct(bt->name);
                if (!sd) {
                    err(m->memberLoc, "未知的 struct 类型 '" + bt->name + "'");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                int idx = fieldIndex(sd, m->member);
                if (idx < 0) {
                    err(m->memberLoc, "struct '" + sd->name + "' 没有字段 '" +
                                          m->member + "'");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                m->fieldIndex = idx;
                e->ty = sd->fields[idx].ty;
                return e->ty;
            }

            case ExprKind::If: {
                // if 表达式（0.7）：两分支类型必须相容
                auto* ie = static_cast<IfExpr*>(e);
                const Ty* ct = checkExpr(ie->cond);
                if (ct != tInvalid && ct != tBool) {
                    typeError(ie->cond->loc, ct, tBool,
                              "if 表达式的条件必须是 bool");
                }
                const Ty* branchHint =
                    (hint && hint != tVoid && hint != tInvalid) ? hint : nullptr;
                const Ty* tt = checkExpr(ie->thenVal, branchHint);
                const Ty* et = checkExpr(ie->elseVal, branchHint);
                if (tt == tInvalid || et == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (tt == et) {
                    e->ty = tt;
                } else if (isNumeric(tt) && isNumeric(et)) {
                    e->ty = tFloat;
                } else if (canCoerce(tt, et)) {
                    e->ty = et;
                } else if (canCoerce(et, tt)) {
                    e->ty = tt;
                } else {
                    err(ie->loc,
                        "if 表达式两个分支的类型不一致：'" + tyName(tt) +
                            "' 与 '" + tyName(et) + "'");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (e->ty == tVoid) {
                    err(ie->loc, "if 表达式的分支不能是 void");
                    e->ty = tInvalid;
                }
                return e->ty;
            }

            case ExprKind::NoneLit: {
                // none 字面量（0.8）：类型完全由上下文 hint 决定
                if (hint && hint->kind == TyKind::Optional) {
                    e->ty = hint;
                    return hint;
                }
                err(e->loc,
                    "'none' 只能用在可选类型 T? 的上下文里（如 let x: int? = "
                    "none; 或 return none;）");
                e->ty = tInvalid;
                return tInvalid;
            }

            case ExprKind::Try: {
                // expr?（0.8）：operand 必须是 T?，所在函数必须返回 U?
                auto* tr = static_cast<TryExpr*>(e);
                const Ty* ot = checkExpr(tr->operand);
                if (ot == tInvalid) {
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (!isOptional(ot)) {
                    err(tr->loc,
                        "'?' 只能用在可选类型 T? 的表达式上（这里是 '" +
                            tyName(ot) + "'）；不会失败的表达式无需传播");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                if (!curFunc || !isOptional(curFunc->retTy)) {
                    std::string ret =
                        curFunc ? tyName(curFunc->retTy) : std::string("void");
                    err(tr->loc,
                        "'?' 向上传播错误，要求所在函数的返回类型是 T?（当前 "
                        "'" + (curFunc ? curFunc->name : std::string("?")) +
                            "' 返回 '" + ret +
                            "'）。库代码用 ? 传播；脚本可用 int!()/read!() 直接 "
                            "panic，或用 'or 默认值' 兜底");
                    e->ty = tInvalid;
                    return tInvalid;
                }
                e->ty = ot->elem;
                return e->ty;
            }
        }
        return tInvalid;
    }

    const Ty* checkBinary(BinOp op, const Ty* lt, const Ty* rt, SourceLoc oloc) {
        auto fail2 = [&](const std::string& msg) {
            err(oloc, "运算符 '" + std::string(binOpText(op)) + "' " + msg +
                                  "（左边是 '" + tyName(lt) + "'，右边是 '" +
                                  tyName(rt) + "'）");
            return tInvalid;
        };

        switch (op) {
            case BinOp::Add:
                if (lt == tString && rt == tString) return tString;
                if (isNumeric(lt) && isNumeric(rt)) {
                    return (lt == tFloat || rt == tFloat) ? tFloat : tInt;
                }
                return fail2("要求两边都是数值，或者两边都是字符串（字符串用 '+' 拼接）");

            case BinOp::Sub:
            case BinOp::Mul:
            case BinOp::Div:
                if (isNumeric(lt) && isNumeric(rt)) {
                    return (lt == tFloat || rt == tFloat) ? tFloat : tInt;
                }
                return fail2("要求两边都是 int 或 float");

            case BinOp::Mod:
                if (lt == tInt && rt == tInt) return tInt;
                return fail2("要求两边都是 int（取模不支持 float，可以用 fmod 之类的库函数）");

            case BinOp::Eq:
            case BinOp::Ne:
                if (lt->kind == TyKind::Array || rt->kind == TyKind::Array) {
                    return fail2("数组不能用 == / != 比较（数组是引用语义，"
                                 "请逐个比较元素或比较 len()）");
                }
                if (lt == rt) return tBool;
                if (isNumeric(lt) && isNumeric(rt)) return tBool;
                return fail2("要求两边类型相同");

            case BinOp::Lt:
            case BinOp::Le:
            case BinOp::Gt:
            case BinOp::Ge:
                if (lt->kind == TyKind::Array || rt->kind == TyKind::Array) {
                    return fail2("数组不支持大小比较");
                }
                if (isNumeric(lt) && isNumeric(rt)) return tBool;
                if (lt == tString && rt == tString) return tBool;
                return fail2("要求两边都是数值，或者两边都是字符串（字符串按字典序比较）");

            case BinOp::LogicAnd:
            case BinOp::LogicOr:
                if (lt == tBool && rt == tBool) return tBool;
                return fail2("要求两边都是 bool");

            case BinOp::BitAnd:
            case BinOp::BitOr:
            case BinOp::BitXor:
            case BinOp::Shl:
            case BinOp::Shr:
                if (lt == tInt && rt == tInt) return tInt;
                return fail2("位运算要求两边都是 int");
        }
        return tInvalid;
    }

    // 逐个检查实参（无 hint）。用于无法提供形参提示的调用路径。
    std::vector<const Ty*> checkArgsNoHint(CallExpr* c) {
        std::vector<const Ty*> tys;
        for (Expr* a : c->args) tys.push_back(checkExpr(a));
        return tys;
    }

    const Ty* checkCall(CallExpr* c) {
        std::string prefix, member;
        if (splitQualified(c->callee, prefix, member)) {
            // ---- 数组方法（0.5 新增）： a.push(x) / a.pop() / ... ----
            VarInfo* recv = findVar(prefix);
            if (recv && recv->ty && recv->ty->kind == TyKind::Array) {
                recv->used = true;
                // sort（1.1）：比较器是函数名，用 fn(T,T)->int 作 hint 解析
                if (member == "sort" && c->args.size() == 1) {
                    const Ty* hint = TyStore::fnOf(
                        tInt, {recv->ty->elem, recv->ty->elem});
                    return checkArrayMethod(c, recv, member,
                                           {checkExpr(c->args[0], hint)});
                }
                return checkArrayMethod(c, recv, member, checkArgsNoHint(c));
            }
            if (recv && recv->ty && recv->ty->kind != TyKind::Invalid) {
                err(c->calleeLoc,
                    "'" + tyName(recv->ty) + "' 类型的变量 '" + prefix +
                        "' 没有 '" + member +
                        "' 方法（方法只存在于数组上，例如 a.push(x)）");
                checkArgsNoHint(c);
                c->ty = tInvalid;
                return tInvalid;
            }
            // ---- 模块限定访问： mod.fn(...) / 别名.fn(...) ----
            std::string mod = resolveModule(modules, prefix);
            if (!modules.imported.count(mod)) {
                err(c->calleeLoc, "模块 '" + mod +
                                          "' 没有被 import（请先写 import \"" +
                                          mod + "\"）");
                checkArgsNoHint(c);
                c->ty = tInvalid;
                return tInvalid;
            }
            auto it = builtinTable().find(member);
            if (it != builtinTable().end() &&
                builtinModule(it->second) == mod) {
                c->builtin = it->second;
                return checkBuiltinCall(c, checkArgsNoHint(c));
            }
            auto fit = modFuncs.find(mod + "\x01" + member);
            if (fit != modFuncs.end()) {
                c->target = fit->second;
                return checkUserCall(c, fit->second);
            }
            err(c->calleeLoc, "模块 '" + mod + "' 中没有函数 '" + member + "'");
            checkArgsNoHint(c);
            c->ty = tInvalid;
            return tInvalid;
        }

        // ---- 调用 fn 类型变量（1.1：函数成为值，间接调用）----
        if (c->callee.rfind("__", 0) != 0) {
            VarInfo* fv = findVar(c->callee);
            if (fv && fv->ty && fv->ty->kind == TyKind::Fn) {
                fv->used = true;
                c->viaValue = true;
                const Ty* ft = fv->ty;
                c->calleeFnTy = ft;
                if (c->args.size() != ft->members.size()) {
                    err(c->loc, "函数值 '" + c->callee + "' 需要 " +
                                    std::to_string(ft->members.size()) +
                                    " 个参数，但调用时给了 " +
                                    std::to_string(c->args.size()) + " 个");
                    for (Expr* a : c->args) checkExpr(a);
                    c->ty = tInvalid;
                    return tInvalid;
                }
                bool bad = false;
                for (size_t i = 0; i < c->args.size(); i++) {
                    const Ty* want = ft->members[i];
                    const Ty* at = checkExpr(c->args[i], want);
                    if (at != tInvalid && !canCoerce(at, want) &&
                        !tryCoerceArrayLit(c->args[i], want)) {
                        typeError(c->args[i]->loc, at, want,
                                  "函数值 '" + c->callee + "' 的第 " +
                                      std::to_string(i + 1) + " 个实参类型不匹配");
                        bad = true;
                    }
                }
                if (c->panicVariant) {
                    if (ft->elem && ft->elem->kind == TyKind::Optional) {
                        c->ty = ft->elem->elem;
                    } else {
                        err(c->calleeLoc, "'! ' 只能用于返回可选类型 T? 的函数值");
                        c->ty = tInvalid;
                    }
                } else {
                    c->ty = bad ? tInvalid : ft->elem;
                }
                return c->ty;
            }
        }

        // ---- 原生后端特权内建（__ 前缀，0.6）----
        // 仅 native 后端可用；参数个数与类型不做检查（运行时库专用 DSL），
        // 返回类型默认 int，浮点类内建返回 float。用户程序中误用会在
        // C 后端生成 #error，在原生后端直接可用。
        if (c->callee.rfind("__", 0) == 0) {
            static const std::unordered_map<std::string, Builtin> intrinsics = {
                {"__syscall", Builtin::IntrSyscall},
                {"__sys_open", Builtin::IntrSysOpen},
                {"__sys_unlink", Builtin::IntrSysUnlink},
                {"__sys_rename", Builtin::IntrSysRename},
                {"__sys_fork", Builtin::IntrSysFork},
                {"__bump_alloc", Builtin::IntrBumpAlloc},
                {"__peek64", Builtin::IntrPeek64},
                {"__peek8u", Builtin::IntrPeek8u},
                {"__poke64", Builtin::IntrPoke64},
                {"__poke8", Builtin::IntrPoke8},
                {"__mem_copy", Builtin::IntrMemCopy},
                {"__f_to_bits", Builtin::IntrFBits},
                {"__f_from", Builtin::IntrFFrom},
                {"__i_to_f", Builtin::IntrIToF},
                {"__f_to_i", Builtin::IntrFToI},
                {"__rand_next", Builtin::IntrRandNext},
                {"__seed_set", Builtin::IntrSeedSet},
                {"__sptr", Builtin::IntrSPtr},
                {"__sval", Builtin::IntrSVal},
                {"__sval_a", Builtin::IntrSValA},
                {"__call1", Builtin::IntrCall1},
                {"__call2", Builtin::IntrCall2},
                // 网络系统调用（1.2）
                {"__sys_socket", Builtin::IntrSysSocket},
                {"__sys_connect", Builtin::IntrSysConnect},
                {"__sys_sendto", Builtin::IntrSysSendto},
                {"__sys_recvfrom", Builtin::IntrSysRecvfrom},
                {"__sys_bind", Builtin::IntrSysBind},
                {"__sys_listen", Builtin::IntrSysListen},
                {"__sys_accept", Builtin::IntrSysAccept},
                {"__sys_setsockopt", Builtin::IntrSysSetsockopt},
                {"__poke16", Builtin::IntrPoke16},
                {"__poke32", Builtin::IntrPoke32},
            };
            auto iit = intrinsics.find(c->callee);
            if (iit != intrinsics.end()) {
                c->builtin = iit->second;
                // 特权内建的参数不做类型检查（运行时库专用 DSL），但仍要
                // 递归检查实参里的表达式，避免漏掉嵌套的语义错误。
                for (Expr* a : c->args) checkExpr(a);
                // 参数个数检查（0.7，C1）：缺参会让后端在发射阶段读越界，
                // 以前直接导致编译器自身崩溃；现在在 Sema 阶段报错。
                static const std::unordered_map<std::string,
                                                std::pair<int, int>>
                    arity = {
                        {"__syscall", {1, 7}},   {"__sys_open", {2, 2}},
                        {"__sys_unlink", {1, 1}}, {"__sys_rename", {2, 2}},
                        {"__sys_fork", {0, 0}},   {"__bump_alloc", {1, 1}},
                        {"__peek64", {2, 2}},     {"__peek8u", {2, 2}},
                        {"__poke64", {3, 3}},     {"__poke8", {3, 3}},
                        {"__mem_copy", {5, 5}},   {"__f_to_bits", {1, 1}},
                        {"__f_from", {1, 1}},     {"__i_to_f", {1, 1}},
                        {"__f_to_i", {1, 1}},     {"__rand_next", {0, 0}},
                        {"__seed_set", {1, 1}},   {"__sptr", {1, 1}},
                        {"__sval", {1, 1}},       {"__sval_a", {1, 1}},
                        {"__call1", {2, 2}},      {"__call2", {3, 3}},
                        {"__sys_socket", {3, 3}},     {"__sys_connect", {3, 3}},
                        {"__sys_sendto", {6, 6}},     {"__sys_recvfrom", {6, 6}},
                        {"__sys_bind", {3, 3}},       {"__sys_listen", {2, 2}},
                        {"__sys_accept", {3, 3}},     {"__sys_setsockopt", {5, 5}},
                        {"__poke16", {3, 3}},         {"__poke32", {3, 3}},
                    };
                auto ait = arity.find(c->callee);
                if (ait != arity.end()) {
                    int n = static_cast<int>(c->args.size());
                    if (n < ait->second.first || n > ait->second.second) {
                        std::string want =
                            (ait->second.first == ait->second.second)
                                ? std::to_string(ait->second.first)
                                : (std::to_string(ait->second.first) + "~" +
                                   std::to_string(ait->second.second));
                        err(c->loc, "内部内建 '" + c->callee + "' 需要 " + want +
                                        " 个参数，但给了 " + std::to_string(n) +
                                        " 个");
                        c->ty = tInvalid;
                        return tInvalid;
                    }
                }
                // 用户程序（非运行时库）直接调用特权内建时提示：这些内建绕过
                // 全部安全检查（可读写任意内存），属于运行时库专用 DSL。
                // 运行时库的 luxrt_ 函数体豁免（C2）。
                bool inLuxrt = curFunc &&
                               curFunc->name.rfind("luxrt_", 0) == 0;
                if (needMain_ && !inLuxrt) {
                    warn(DiagCode::kWPrivIntrinsic, c->calleeLoc,
                         "'" + c->callee +
                             "' 是原生后端运行时库专用的特权内建，会绕过语言的"
                             "安全检查；普通程序请勿直接使用");
                }
                // 无返回值的"语句型"内建必须是 tVoid（否则表达式语句层会
                // 按"+1 结果槽"约定多清 8 字节，破坏栈平衡）
                bool isVoidIntr = (c->builtin == Builtin::IntrPoke64 ||
                                   c->builtin == Builtin::IntrPoke8 ||
                                   c->builtin == Builtin::IntrPoke16 ||
                                   c->builtin == Builtin::IntrPoke32 ||
                                   c->builtin == Builtin::IntrMemCopy ||
                                   c->builtin == Builtin::IntrSeedSet);
                c->ty = isVoidIntr
                          ? tVoid
                          : (c->builtin == Builtin::IntrFFrom ||
                             c->builtin == Builtin::IntrIToF)
                                ? tFloat
                                : (c->builtin == Builtin::IntrSValA
                                       ? TyStore::arrayOf(tInt)
                                       : (c->builtin == Builtin::IntrSVal
                                              ? tString
                                              : tInt));
                return c->ty;
            }
            err(c->calleeLoc, "未知的内部内建 '" + c->callee + "'");
            c->ty = tInvalid;
            return tInvalid;
        }

        // ---- 裸名调用 ----
        auto it = builtinTable().find(c->callee);
        if (it != builtinTable().end()) {
            c->builtin = it->second;
            // 属于标准库模块的内置函数必须先 import 对应模块（默认导入方式）
            const char* mod = builtinModule(c->builtin);
            if (mod && !modules.memberVisible(mod, c->callee)) {
                err(c->calleeLoc,
                    "函数 '" + c->callee + "' 属于 '" + std::string(mod) +
                        "' 模块，需要在文件顶部写 import \"" + mod +
                        "\"（或者 import \"" + mod + "\" as 别名 后用限定访问）");
                c->ty = tInvalid;
                return tInvalid;
            }
            // 高阶三件套（1.1）：第二个参数是函数名，需要用它的真实签名作 hint
            if (c->builtin == Builtin::Map || c->builtin == Builtin::Filter ||
                c->builtin == Builtin::MapOpt) {
                std::vector<const Ty*> tys;
                tys.push_back(checkExpr(c->args[0]));
                const Ty* hint = nullptr;
                if (c->args.size() > 1 &&
                    c->args[1]->kind == ExprKind::Ident) {
                    hint = fnTypeOfName(
                        static_cast<IdentExpr*>(c->args[1])->name);
                }
                tys.push_back(checkExpr(c->args[1], hint));
                return checkBuiltinCall(c, tys);
            }
            return checkBuiltinCall(c, checkArgsNoHint(c));
        }

        FuncDecl* fn = nullptr;
        auto fit = funcs.find(c->callee);
        if (fit != funcs.end()) fn = fit->second;
        if (!fn) {
            err(c->calleeLoc, "未定义的函数 '" + c->callee + "'");
            if (c->callee == "printf" || c->callee == "puts") {
                warn(DiagCode::kWNoEffect, c->calleeLoc,
                     "Lux 里输出请用 println(...)，不要直接调用 C 函数");
            }
            checkArgsNoHint(c);
            c->ty = tInvalid;
            return tInvalid;
        }
        c->target = fn;
        return checkUserCall(c, fn);
    }

    // 用户函数调用的通用检查（实参个数 + 类型兼容）。
    // 0.8：逐个实参带上形参类型作为 hint，这样 f([]) / f(none) 能正确推断。
    const Ty* checkUserCall(CallExpr* c, FuncDecl* fn) {
        if (fn->params.size() != c->args.size()) {
            err(c->loc, "函数 '" + fn->name + "' 需要 " +
                                    std::to_string(fn->params.size()) +
                                    " 个参数，但调用时给了 " +
                                    std::to_string(c->args.size()) + " 个");
            for (Expr* a : c->args) checkExpr(a);
        } else {
            for (size_t i = 0; i < c->args.size(); i++) {
                const Ty* want = fn->params[i].ty;
                const Ty* hint =
                    (want && want != tVoid && want != tInvalid) ? want
                                                                : nullptr;
                const Ty* at = checkExpr(c->args[i], hint);
                if (at == tInvalid) continue;
                if (!canCoerce(at, want) &&
                    !tryCoerceArrayLit(c->args[i], want)) {
                    typeError(c->args[i]->loc, at, want,
                              "传给参数 '" + fn->params[i].name +
                                  "' 的实参类型不匹配");
                }
            }
        }
        // 0.8 panic 变体：f!(...) 要求 f 返回 T?，解包出 T
        if (c->panicVariant) {
            if (!isOptional(fn->retTy)) {
                err(c->calleeLoc,
                    "'! ' 只能用于返回可选类型 T? 的函数（'" + fn->name +
                        "' 返回 '" + tyName(fn->retTy) + "'）");
                c->ty = tInvalid;
                return tInvalid;
            }
            c->ty = fn->retTy->elem;
            return c->ty;
        }
        c->ty = fn->retTy;
        return fn->retTy;
    }

    // 数组方法检查（0.5 新增）：a.push(v) / a.pop() / a.insert(i, v) /
    // a.remove(i) / a.clear()。const 数组禁止使用会修改内容的方法。
    const Ty* checkArrayMethod(CallExpr* c, VarInfo* recv,
                               const std::string& method,
                               const std::vector<const Ty*>& argTys) {
        const Ty* arrTy = recv->ty;
        const Ty* elem = arrTy->elem;
        auto isMutating = [&]() {
            return method == "push" || method == "pop" || method == "insert" ||
                   method == "remove" || method == "clear" || method == "sort";
        };
        if (recv->isConst && isMutating()) {
            err(c->calleeLoc,
                "数组 '" + recv->name +
                    "' 用 const 声明，不能调用会修改内容的方法（" + method +
                    "）");
            c->ty = tInvalid;
            return tInvalid;
        }
        c->methodRecv = recv->name;
        c->methodElem = elem;
        auto needArgs = [&](size_t n) -> bool {
            if (c->args.size() != n) {
                err(c->loc, "数组方法 '" + method + "' 需要 " +
                                            std::to_string(n) + " 个参数，但给了 " +
                                            std::to_string(c->args.size()) + " 个");
                return false;
            }
            return true;
        };

        if (method == "push") {
            c->builtin = Builtin::ArrPush;
            c->ty = tVoid;
            if (needArgs(1) && argTys[0] != tInvalid) {
                requireCoerce(c->args[0], argTys[0], elem,
                              "push 的实参类型与数组元素类型不匹配",
                              c->args[0]->loc);
            }
            return tVoid;
        }
        if (method == "pop") {
            c->builtin = Builtin::ArrPop;
            if (needArgs(0)) {
                c->ty = elem;
                return elem;
            }
            c->ty = tInvalid;
            return tInvalid;
        }
        if (method == "insert") {
            c->builtin = Builtin::ArrInsert;
            c->ty = tVoid;
            if (needArgs(2)) {
                if (argTys[0] != tInvalid && argTys[0] != tInt) {
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "insert 的插入位置必须是 int");
                }
                if (argTys[1] != tInvalid) {
                    requireCoerce(c->args[1], argTys[1], elem,
                                  "insert 的实参类型与数组元素类型不匹配",
                                  c->args[1]->loc);
                }
            }
            return tVoid;
        }
        if (method == "remove") {
            c->builtin = Builtin::ArrRemove;
            if (needArgs(1)) {
                if (argTys[0] != tInvalid && argTys[0] != tInt) {
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "remove 的下标必须是 int");
                }
                c->ty = elem;
                return elem;
            }
            c->ty = tInvalid;
            return tInvalid;
        }
        if (method == "clear") {
            c->builtin = Builtin::ArrClear;
            c->ty = tVoid;
            needArgs(0);
            return tVoid;
        }
        if (method == "sort") {
            // 1.1：a.sort(cmp)，cmp: fn(T, T) -> int（原地排序）
            c->builtin = Builtin::ArrSort;
            c->ty = tVoid;
            if (needArgs(1)) {
                const Ty* ft = argTys[0];
                if (ft != tInvalid &&
                    (ft->kind != TyKind::Fn || ft->members.size() != 2 ||
                     ft->members[0] != elem || ft->members[1] != elem ||
                     ft->elem != tInt)) {
                    err(c->args[0]->loc,
                        "sort 的比较器必须是 fn(" + tyName(elem) + ", " +
                            tyName(elem) + ") -> int");
                }
            }
            return tVoid;
        }
        err(c->calleeLoc, "数组没有 '" + method +
                                      "' 方法（可用：push / pop / insert / "
                                      "remove / clear / sort）");
        c->ty = tInvalid;
        return tInvalid;
    }

    const Ty* checkBuiltinCall(CallExpr* c, const std::vector<const Ty*>& argTys) {
        // 0.8 panic 变体（name!）：只有可能失败的内建才配 ! 后缀
        if (c->panicVariant) {
            bool canPanic = c->builtin == Builtin::ToInt ||
                            c->builtin == Builtin::ToFloat ||
                            c->builtin == Builtin::FileRead;
            if (!canPanic) {
                err(c->calleeLoc,
                    "'!' 只能用于可能失败的内建函数（int / float / read）；'" +
                        c->callee + "' 不会失败，无需 '!'");
                c->ty = tInvalid;
                return tInvalid;
            }
        }
        auto arity = [&](size_t n) {
            if (c->args.size() != n) {
                err(c->loc, "内置函数 '" + c->callee + "' 需要 " +
                                        std::to_string(n) + " 个参数，但给了 " +
                                        std::to_string(c->args.size()) + " 个");
                return false;
            }
            return true;
        };
        auto strArg = [&](size_t i, const std::string& what) {
            if (argTys[i] == tInvalid) return;
            if (argTys[i] != tString) {
                typeError(c->args[i]->loc, argTys[i], tString, what);
            }
        };

        switch (c->builtin) {
            case Builtin::Print:
            case Builtin::PrintLn: {
                for (size_t i = 0; i < argTys.size(); i++) {
                    if (argTys[i] == tVoid) {
                        err(c->args[i]->loc,
                            "不能输出 void 表达式（函数没有返回值）");
                    }
                }
                c->ty = tVoid;
                return tVoid;
            }

            case Builtin::Len: {
                c->ty = tInt;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                // 0.5 起 len() 同时接受字符串和数组
                if (argTys[0] != tString && argTys[0]->kind != TyKind::Array) {
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "len() 的参数必须是字符串或数组");
                    return tInvalid;
                }
                return tInt;
            }

            case Builtin::Input: {
                c->ty = tString;
                if (c->args.size() == 1 && argTys[0] != tString) {
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "input() 的提示语必须是字符串");
                } else if (c->args.size() > 1) {
                    err(c->loc, "input() 最多接受 1 个参数（提示语）");
                }
                return tString;
            }

            case Builtin::ToInt: {
                if (!arity(1)) { c->ty = tInvalid; return tInvalid; }
                if (argTys[0] == tInvalid) { c->ty = tInvalid; return tInvalid; }
                // 0.8 语义迁移：字符串解析可能失败 → int?（int! 保留 panic 版）
                if (argTys[0] == tString) {
                    c->ty = c->panicVariant ? tInt : TyStore::optionalOf(tInt);
                    return c->ty;
                }
                if (c->panicVariant) {
                    err(c->calleeLoc,
                        "int! 只能用于字符串解析；数值 / bool 转 int 不会失败");
                    c->ty = tInvalid;
                    return tInvalid;
                }
                c->ty = tInt;
                return tInt;
            }

            case Builtin::ToFloat: {
                if (!arity(1)) { c->ty = tInvalid; return tInvalid; }
                if (argTys[0] == tInvalid) { c->ty = tInvalid; return tInvalid; }
                // 0.8 语义迁移：字符串解析可能失败 → float?（float! 保留 panic 版）
                if (argTys[0] == tString) {
                    c->ty =
                        c->panicVariant ? tFloat : TyStore::optionalOf(tFloat);
                    return c->ty;
                }
                if (c->panicVariant) {
                    err(c->calleeLoc,
                        "float! 只能用于字符串解析；数值 / bool 转 float 不会失败");
                    c->ty = tInvalid;
                    return tInvalid;
                }
                c->ty = tFloat;
                return tFloat;
            }

            case Builtin::ToString: {
                c->ty = tString;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                return tString;  // int / float / bool / 数组都可转
            }

            case Builtin::Assert: {
                c->ty = tVoid;
                if (c->args.empty() || c->args.size() > 2) {
                    err(c->loc, "assert() 需要 1~2 个参数：assert(条件, 提示语?)");
                    return tVoid;
                }
                if (argTys[0] != tInvalid && argTys[0] != tBool) {
                    typeError(c->args[0]->loc, argTys[0], tBool,
                              "assert() 的条件必须是 bool");
                }
                if (c->args.size() == 2 && argTys[1] != tInvalid &&
                    argTys[1] != tString) {
                    typeError(c->args[1]->loc, argTys[1], tString,
                              "assert() 的提示语必须是 string");
                }
                return tVoid;
            }

            case Builtin::Exit: {
                c->ty = tVoid;
                if (!arity(1)) return tVoid;
                if (argTys[0] != tInvalid && argTys[0] != tInt) {
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "exit() 的退出码必须是 int");
                }
                return tVoid;
            }

            case Builtin::Abs: {
                if (!arity(1)) {
                    c->ty = tInvalid;
                    return tInvalid;
                }
                const Ty* t = argTys[0];
                if (t == tInvalid) {
                    c->ty = tInvalid;
                    return tInvalid;
                }
                if (!isNumeric(t)) {
                    err(c->loc, "abs() 的参数必须是 int 或 float");
                    c->ty = tInvalid;
                    return tInvalid;
                }
                c->ty = t;
                return t;
            }

            case Builtin::Sqrt: {
                c->ty = tFloat;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                if (!isNumeric(argTys[0])) {
                    err(c->loc, "sqrt() 的参数必须是 int 或 float");
                    return tInvalid;
                }
                return tFloat;
            }

            // ---- math 模块：一元浮点函数 ----
            case Builtin::Floor:
            case Builtin::Ceil:
            case Builtin::Round:
            case Builtin::Sin:
            case Builtin::Cos:
            case Builtin::Tan:
            case Builtin::Asin:
            case Builtin::Acos:
            case Builtin::Atan:
            case Builtin::Log:
            case Builtin::Log10:
            case Builtin::Exp:
            case Builtin::Trunc: {
                c->ty = tFloat;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                if (!isNumeric(argTys[0])) {
                    err(c->loc, "math." + c->callee +
                                    "() 的参数必须是 int 或 float");
                    return tInvalid;
                }
                return tFloat;
            }

            case Builtin::Pow:
            case Builtin::Atan2:
            case Builtin::Fmod:
            case Builtin::Hypot: {
                c->ty = tFloat;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                if (!isNumeric(argTys[0]) || !isNumeric(argTys[1])) {
                    err(c->loc, c->callee + "() 的两个参数都必须是 int 或 float");
                    return tInvalid;
                }
                return tFloat;
            }

            case Builtin::IsNan:
            case Builtin::IsInf: {
                c->ty = tBool;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                if (!isNumeric(argTys[0])) {
                    err(c->loc, c->callee + "() 的参数必须是 int 或 float");
                    return tInvalid;
                }
                return tBool;
            }

            case Builtin::Random: {
                c->ty = tFloat;
                if (!c->args.empty()) {
                    err(c->loc, "random() 不接受任何参数");
                }
                return tFloat;
            }

            case Builtin::Seed: {
                c->ty = tVoid;
                if (!arity(1)) return tVoid;
                if (argTys[0] != tInvalid && argTys[0] != tInt) {
                    err(c->loc, "seed() 的种子必须是 int");
                }
                return tVoid;
            }

            case Builtin::Min:
            case Builtin::Max: {
                if (!arity(2)) {
                    c->ty = tInvalid;
                    return tInvalid;
                }
                const Ty* a = argTys[0];
                const Ty* b = argTys[1];
                if (!isNumeric(a) || !isNumeric(b)) {
                    err(c->loc, c->callee + "() 的两个参数都必须是 int 或 float");
                    c->ty = tInvalid;
                    return tInvalid;
                }
                const Ty* r = (a == tFloat || b == tFloat) ? tFloat : tInt;
                c->ty = r;
                return r;
            }

            // ---- time 模块 ----
            case Builtin::TimeNow:
            case Builtin::TimeMono: {
                c->ty = tFloat;
                if (!c->args.empty()) {
                    err(c->loc, c->callee + "() 不接受任何参数");
                }
                return tFloat;
            }

            case Builtin::Sleep: {
                c->ty = tVoid;
                if (!arity(1)) return tVoid;
                if (argTys[0] != tInvalid && !isNumeric(argTys[0])) {
                    err(c->loc, "sleep() 的参数必须是数值（秒数，int 或 float）");
                }
                return tVoid;
            }

            case Builtin::SleepMs: {
                c->ty = tVoid;
                if (!arity(1)) return tVoid;
                if (argTys[0] != tInvalid && argTys[0] != tInt) {
                    err(c->loc, "sleep_ms() 的参数必须是 int（毫秒数）");
                }
                return tVoid;
            }

            // ---- system 模块 ----
            case Builtin::SysCall: {
                c->ty = tInt;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                strArg(0, "system() 的命令必须是字符串");
                return tInt;
            }

            case Builtin::Env: {
                c->ty = tString;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                strArg(0, "env() 的环境变量名必须是字符串");
                return tString;
            }

            case Builtin::Setenv: {
                c->ty = tBool;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, "setenv() 的环境变量名必须是字符串");
                strArg(1, "setenv() 的值必须是字符串");
                return tBool;
            }

            // ---- file 模块 ----
            case Builtin::FileRead: {
                if (!arity(1)) { c->ty = tInvalid; return tInvalid; }
                if (argTys[0] == tInvalid) { c->ty = tInvalid; return tInvalid; }
                strArg(0, c->callee + "() 的文件路径必须是字符串");
                // 0.8：读文件可能失败 → string?（read! 保留 panic 版）
                c->ty =
                    c->panicVariant ? tString : TyStore::optionalOf(tString);
                return c->ty;
            }

            case Builtin::FileExists:
            case Builtin::FileRemove: {
                c->ty = tBool;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                strArg(0, c->callee + "() 的文件路径必须是字符串");
                return c->ty;
            }

            case Builtin::FileWrite:
            case Builtin::FileAppend:
            case Builtin::FileRename: {
                c->ty = tBool;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, c->callee + "() 的文件路径必须是字符串");
                strArg(1, c->callee + "() 的参数必须是字符串");
                return tBool;
            }

            // ---- string 模块 ----
            case Builtin::StrContains:
            case Builtin::StrStartsWith:
            case Builtin::StrEndsWith: {
                c->ty = tBool;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, c->callee + "() 的第一个参数必须是字符串");
                strArg(1, c->callee + "() 的第二个参数必须是字符串");
                return tBool;
            }

            case Builtin::StrFind: {
                c->ty = tInt;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, "find() 的第一个参数必须是字符串");
                strArg(1, "find() 的第二个参数必须是字符串");
                return tInt;
            }

            case Builtin::StrFindOpt: {
                // 0.9.4（P1-8）：find 的可选变体，扫掉 A3 尾巴
                const Ty* optInt = TyStore::optionalOf(tInt);
                c->ty = optInt;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, "find_opt() 的第一个参数必须是字符串");
                strArg(1, "find_opt() 的第二个参数必须是字符串");
                return optInt;
            }

            case Builtin::LastError: {
                // 0.9.4（P1-5）：最近一次标准库失败的说明（无并发，全局够用）
                c->ty = tString;
                if (!arity(0)) return tInvalid;
                return tString;
            }

            case Builtin::ByteAt: {
                // 1.1：byte_at(s, i) -> int（第 i 个字节的值 0..255）
                c->ty = tInt;
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tString)
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "byte_at() 的第一个参数必须是 string");
                if (argTys[1] != tInvalid && argTys[1] != tInt)
                    typeError(c->args[1]->loc, argTys[1], tInt,
                              "byte_at() 的第二个参数必须是 int");
                return tInt;
            }

            case Builtin::Bytes: {
                // 1.1：bytes(s) -> int[]
                c->ty = TyStore::arrayOf(tInt);
                if (!arity(1)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tString)
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "bytes() 的参数必须是 string");
                return c->ty;
            }

            case Builtin::ListDir: {
                // 1.1：list_dir(path) -> string[]?
                c->ty = TyStore::optionalOf(TyStore::arrayOf(tString));
                if (!arity(1)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tString)
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "list_dir() 的参数必须是 string");
                return c->ty;
            }

            // ---- net 模块（1.2）：全部检查参数类型与个数 ----
            case Builtin::NetDial: {
                c->ty = TyStore::optionalOf(tInt);
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tString)
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "dial() 的第一个参数（主机名）必须是 string");
                if (argTys[1] != tInvalid && argTys[1] != tInt)
                    typeError(c->args[1]->loc, argTys[1], tInt,
                              "dial() 的第二个参数（端口）必须是 int");
                return c->ty;
            }
            case Builtin::NetSend: {
                c->ty = TyStore::optionalOf(tInt);
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "send() 的第一个参数（fd）必须是 int");
                if (argTys[1] != tInvalid && argTys[1] != tString)
                    typeError(c->args[1]->loc, argTys[1], tString,
                              "send() 的第二个参数必须是 string");
                return c->ty;
            }
            case Builtin::NetRecv: {
                c->ty = TyStore::optionalOf(tString);
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "recv() 的第一个参数（fd）必须是 int");
                if (argTys[1] != tInvalid && argTys[1] != tInt)
                    typeError(c->args[1]->loc, argTys[1], tInt,
                              "recv() 的第二个参数（最大字节数）必须是 int");
                return c->ty;
            }
            case Builtin::NetClose: {
                c->ty = tBool;
                if (!arity(1)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "close() 的参数（fd）必须是 int");
                return tBool;
            }
            case Builtin::NetListen: {
                c->ty = TyStore::optionalOf(tInt);
                if (!arity(1)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "listen() 的参数（端口）必须是 int");
                return c->ty;
            }
            case Builtin::NetAccept: {
                c->ty = TyStore::optionalOf(tInt);
                if (!arity(1)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "accept() 的参数（监听 fd）必须是 int");
                return c->ty;
            }
            case Builtin::NetSetTimeout: {
                c->ty = tBool;
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0] != tInt)
                    typeError(c->args[0]->loc, argTys[0], tInt,
                              "set_timeout() 的第一个参数（fd）必须是 int");
                if (argTys[1] != tInvalid && !isNumeric(argTys[1]))
                    typeError(c->args[1]->loc, argTys[1], tFloat,
                              "set_timeout() 的第二个参数（秒）必须是 int 或 float");
                return tBool;
            }

            case Builtin::Map: {
                // 1.1：map(a, f) -> R[]，f: fn(T)->R
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0]->kind != TyKind::Array) {
                    typeError(c->args[0]->loc, argTys[0], nullptr,
                              "map() 的第一个参数必须是数组");
                    return tInvalid;
                }
                if (argTys[0] == tInvalid || argTys[1] == tInvalid) return tInvalid;
                const Ty* ft = argTys[1];
                if (ft->kind != TyKind::Fn || ft->members.size() != 1) {
                    err(c->args[1]->loc,
                        "map() 的第二个参数必须是单参函数值 fn(T) -> R");
                    return tInvalid;
                }
                if (ft->members[0] != argTys[0]->elem) {
                    typeError(c->args[1]->loc, ft->members[0], argTys[0]->elem,
                              "map() 的函数参数类型必须等于数组元素类型");
                    return tInvalid;
                }
                if (!ft->elem || ft->elem->kind == TyKind::Void) {
                    err(c->args[1]->loc, "map() 的函数不能返回 void");
                    return tInvalid;
                }
                if (ft->elem->kind == TyKind::Optional) {
                    err(c->args[1]->loc,
                        "map() 的函数不能返回可选类型；用 map_opt() 处理失败");
                    return tInvalid;
                }
                c->ty = TyStore::arrayOf(ft->elem);
                return c->ty;
            }

            case Builtin::Filter: {
                // 1.1：filter(a, f) -> T[]，f: fn(T)->bool
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0]->kind != TyKind::Array) {
                    typeError(c->args[0]->loc, argTys[0], nullptr,
                              "filter() 的第一个参数必须是数组");
                    return tInvalid;
                }
                if (argTys[0] == tInvalid || argTys[1] == tInvalid) return tInvalid;
                const Ty* ft = argTys[1];
                if (ft->kind != TyKind::Fn || ft->members.size() != 1) {
                    err(c->args[1]->loc,
                        "filter() 的第二个参数必须是单参函数值 fn(T) -> bool");
                    return tInvalid;
                }
                if (ft->members[0] != argTys[0]->elem) {
                    typeError(c->args[1]->loc, ft->members[0], argTys[0]->elem,
                              "filter() 的函数参数类型必须等于数组元素类型");
                    return tInvalid;
                }
                if (ft->elem != tBool)
                    typeError(c->args[1]->loc, ft->elem, tBool,
                              "filter() 的函数必须返回 bool");
                c->ty = TyStore::arrayOf(argTys[0]->elem);
                return c->ty;
            }

            case Builtin::MapOpt: {
                // 1.1：map_opt(a, f) -> R[]?，f: fn(T)->R?
                if (!arity(2)) return tInvalid;
                if (argTys[0] != tInvalid && argTys[0]->kind != TyKind::Array) {
                    typeError(c->args[0]->loc, argTys[0], nullptr,
                              "map_opt() 的第一个参数必须是数组");
                    return tInvalid;
                }
                if (argTys[0] == tInvalid || argTys[1] == tInvalid) return tInvalid;
                const Ty* ft = argTys[1];
                if (ft->kind != TyKind::Fn || ft->members.size() != 1) {
                    err(c->args[1]->loc,
                        "map_opt() 的第二个参数必须是单参函数值 fn(T) -> R?");
                    return tInvalid;
                }
                if (ft->members[0] != argTys[0]->elem) {
                    typeError(c->args[1]->loc, ft->members[0], argTys[0]->elem,
                              "map_opt() 的函数参数类型必须等于数组元素类型");
                    return tInvalid;
                }
                if (!ft->elem || ft->elem->kind != TyKind::Optional) {
                    err(c->args[1]->loc,
                        "map_opt() 的函数必须返回可选类型 R?");
                    return tInvalid;
                }
                c->ty = TyStore::optionalOf(TyStore::arrayOf(ft->elem->elem));
                return c->ty;
            }

            case Builtin::StrReplace: {
                c->ty = tString;
                if (!arity(3)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid ||
                    argTys[2] == tInvalid)
                    return tInvalid;
                strArg(0, "replace() 的第一个参数必须是字符串");
                strArg(1, "replace() 的第二个参数必须是字符串");
                strArg(2, "replace() 的第三个参数必须是字符串");
                return tString;
            }

            case Builtin::StrTrim:
            case Builtin::StrUpper:
            case Builtin::StrLower: {
                c->ty = tString;
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                strArg(0, c->callee + "() 的参数必须是字符串");
                return tString;
            }

            case Builtin::StrSubstr: {
                c->ty = tString;
                if (!arity(3)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid ||
                    argTys[2] == tInvalid)
                    return tInvalid;
                strArg(0, "substr() 的第一个参数必须是字符串");
                if (argTys[1] != tInt) {
                    typeError(c->args[1]->loc, argTys[1], tInt,
                              "substr() 的起始位置必须是 int");
                }
                if (argTys[2] != tInt) {
                    typeError(c->args[2]->loc, argTys[2], tInt,
                              "substr() 的长度必须是 int");
                }
                return tString;
            }

            case Builtin::StrFormat: {
                c->ty = tString;
                if (c->args.empty()) {
                    err(c->loc, "format() 至少需要一个参数（格式串）");
                    return tInvalid;
                }
                if (argTys[0] != tInvalid && argTys[0] != tString) {
                    typeError(c->args[0]->loc, argTys[0], tString,
                              "format() 的第一个参数必须是格式串");
                }
                // 格式串是字面量时，编译期校验 {} 占位符数量与实参一致
                if (c->args[0]->kind == ExprKind::StrLit) {
                    size_t placeholders = 0;
                    const std::string& fmt =
                        static_cast<StrLitExpr*>(c->args[0])->value;
                    for (size_t i = 0; i + 1 < fmt.size(); i++) {
                        if (fmt[i] == '{' && fmt[i + 1] == '}') {
                            placeholders++;
                            i++;
                        }
                    }
                    if (placeholders != c->args.size() - 1) {
                        err(c->loc,
                            "format() 的格式串里有 " +
                                std::to_string(placeholders) +
                                " 个 '{}' 占位符，但给了 " +
                                std::to_string(c->args.size() - 1) + " 个参数");
                        return tInvalid;
                    }
                }
                for (size_t i = 1; i < argTys.size(); i++) {
                    if (argTys[i] == tVoid) {
                        err(c->args[i]->loc, "不能格式化 void 表达式");
                    }
                }
                return tString;
            }

            // ---- string 模块数组扩展（0.5 新增） ----
            case Builtin::StrSplit: {
                c->ty = TyStore::arrayOf(tString);
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                strArg(0, "split() 的第一个参数必须是字符串");
                strArg(1, "split() 的第二个参数（分隔符）必须是字符串");
                return c->ty;
            }

            case Builtin::StrChars: {
                c->ty = TyStore::arrayOf(tString);
                if (!arity(1)) return tInvalid;
                if (argTys[0] == tInvalid) return tInvalid;
                strArg(0, "chars() 的参数必须是字符串");
                return c->ty;
            }

            case Builtin::StrJoin: {
                c->ty = tString;
                if (!arity(2)) return tInvalid;
                if (argTys[0] == tInvalid || argTys[1] == tInvalid)
                    return tInvalid;
                if (argTys[0]->kind != TyKind::Array ||
                    argTys[0]->elem != tString) {
                    err(c->args[0]->loc,
                        "join() 的第一个参数必须是 string[]（字符串数组），"
                        "这里是 '" + tyName(argTys[0]) + "'");
                }
                strArg(1, "join() 的第二个参数（连接符）必须是字符串");
                return tString;
            }

            default:
                c->ty = tVoid;
                return tVoid;
        }
    }

    // ---------------- 语句 ----------------
    void checkStmt(Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Block: {
                pushScope();
                for (Stmt* st : static_cast<BlockStmt*>(s)->stmts) checkStmt(st);
                popScope();
                break;
            }

            case StmtKind::Let: {
                auto* let = static_cast<LetStmt*>(s);
                const Ty* initTy = tInvalid;
                // 有类型标注时把标注类型作为 hint 传入，让空数组 [] 能推断元素类型
                const Ty* annHint =
                    (let->ann.present && let->ann.ty != tInvalid) ? let->ann.ty
                                                                  : nullptr;
                if (let->init) initTy = checkExpr(let->init, annHint);

                const Ty* finalTy;
                if (let->ann.present) {
                    finalTy = let->ann.ty;
                    if (let->init && initTy != tInvalid &&
                        !canCoerce(initTy, finalTy)) {
                        // 数组字面量可以适配目标元素类型（[1,2,3] → float[]）
                        if (!tryCoerceArrayLit(let->init, finalTy)) {
                            typeError(let->init->loc, initTy, finalTy,
                                      "变量 '" + let->name + "' 的初始值类型不匹配");
                        }
                    }
                } else if (let->init) {
                    finalTy = initTy;
                } else {
                    err(let->nameLoc,
                        "变量 '" + let->name +
                            "' 既没有类型标注也没有初始值，无法推断类型");
                    finalTy = tInvalid;
                }

                if (finalTy == tVoid && let->ann.present) {
                    err(let->nameLoc, "变量不能被声明为 void 类型");
                }
                let->resolved = finalTy;
                // 0.8（C2）：struct 的零值是空指针，未初始化声明会在访问字段时
                // 崩溃。在这里直接拒绝，引导用户初始化或改用可选类型。
                if (!let->init && finalTy && finalTy->kind == TyKind::Named) {
                    err(let->nameLoc,
                        "struct 变量 '" + let->name +
                            "' 必须初始化（struct 默认值是空指针，访问字段会崩溃）；"
                            "若确实需要空值请声明为 '" + tyName(finalTy) + "?'");
                }
                declare(VarInfo{let->name, finalTy, let->isConst, let->nameLoc,
                                nullptr, false});
                break;
            }

            case StmtKind::Assign: {
                auto* a = static_cast<AssignStmt*>(s);
                const Ty* tgtTy = checkAssignTarget(a->target);
                a->elemTy = tgtTy;
                const Ty* valHint = (tgtTy && tgtTy != tInvalid &&
                                     tgtTy != tVoid)
                                        ? tgtTy
                                        : nullptr;
                const Ty* vt = checkExpr(a->value, valHint);
                if (tgtTy == tInvalid) break;
                if (a->compound) {
                    // 复合赋值：先按二元运算检查再要求结果能写回目标
                    const Ty* rt =
                        checkBinary(a->compoundOp, tgtTy, vt, a->loc);
                    if (rt != tInvalid && !canCoerce(rt, tgtTy)) {
                        typeError(a->value->loc, rt, tgtTy,
                                  "复合赋值的结果类型与目标不匹配");
                    }
                    break;
                }
                if (vt != tInvalid && !canCoerce(vt, tgtTy)) {
                    if (!tryCoerceArrayLit(a->value, tgtTy)) {
                        typeError(a->value->loc, vt, tgtTy,
                                  "赋值的值类型不匹配");
                    }
                }
                break;
            }

            case StmtKind::Expr: {
                auto* es = static_cast<ExprStmt*>(s);
                const Ty* t = checkExpr(es->expr);
                if (t != tVoid && t != tInvalid) {
                    bool isCall = es->expr && es->expr->kind == ExprKind::Call;
                    if (!isCall) {
                        warn(DiagCode::kWNoEffect, es->loc,
                             "这条表达式的结果没有被使用");
                    }
                }
                break;
            }

            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                const Ty* ct = checkExpr(i->cond);
                if (ct != tInvalid && ct != tBool) {
                    typeError(i->cond->loc, ct, tBool, "if 的条件必须是 bool");
                }
                checkStmt(i->thenBranch);
                checkStmt(i->elseBranch);
                break;
            }

            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                const Ty* ct = checkExpr(w->cond);
                if (ct != tInvalid && ct != tBool) {
                    typeError(w->cond->loc, ct, tBool,
                              "while 的条件必须是 bool");
                }
                loopDepth++;
                checkStmt(w->body);
                loopDepth--;
                break;
            }

            case StmtKind::Loop: {
                auto* l = static_cast<LoopStmt*>(s);
                loopDepth++;
                checkStmt(l->body);
                loopDepth--;
                break;
            }

            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                pushScope();
                if (f->mode == ForMode::Range) {
                    const Ty* st = checkExpr(f->start);
                    const Ty* et = checkExpr(f->end);
                    if (st != tInvalid && st != tInt) {
                        typeError(f->start->loc, st, tInt,
                                  "区间循环的起点必须是 int");
                    }
                    if (et != tInvalid && et != tInt) {
                        typeError(f->end->loc, et, tInt,
                                  "区间循环的终点必须是 int");
                    }
                    f->varTy = tInt;
                } else {
                    // for-in：迭代数组（元素逐个绑定）或字符串（逐字节单字符）
                    const Ty* it = checkExpr(f->iterable);
                    f->iterTy = it;  // 记录被迭代对象本身的类型
                    if (it == tString) {
                        f->varTy = tString;
                    } else if (it != tInvalid && it->kind == TyKind::Array) {
                        f->varTy = it->elem;
                    } else if (it != tInvalid) {
                        err(f->iterable->loc,
                            "for-in 只能迭代数组或字符串，这里是 '" +
                                tyName(it) + "'（数值区间请用 for i in 0..n）");
                        f->varTy = tInvalid;
                    } else {
                        f->varTy = tInvalid;
                    }
                }
                declare(VarInfo{f->var, f->varTy, false, f->varLoc, nullptr, false});
                loopDepth++;
                checkStmt(f->body);
                loopDepth--;
                popScope();
                break;
            }

            case StmtKind::Break:
                if (loopDepth == 0) {
                    err(s->loc, "'break' 只能出现在循环（while/for/loop）内部");
                }
                break;

            case StmtKind::Continue:
                if (loopDepth == 0) {
                    err(s->loc, "'continue' 只能出现在循环（while/for/loop）内部");
                }
                break;

            case StmtKind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                const Ty* want = curFunc ? curFunc->retTy : tVoid;
                if (!r->value) {
                    // 0.8：返回类型是 T? 时，'return;' 等价于 'return none;'
                    if (want != tVoid && !isOptional(want)) {
                        err(r->loc, "函数 '" + std::string(curFunc ? curFunc->name : "?") +
                                                "' 的返回类型是 '" + tyName(want) +
                                                "'，这里必须 return 一个值");
                    }
                    break;
                }
                const Ty* gt = checkExpr(
                    r->value, (want != tVoid && want != tInvalid) ? want : nullptr);
                if (want == tVoid) {
                    err(r->loc, "函数 '" + (curFunc ? curFunc->name : "?") +
                                    "' 没有返回值，这里不能 return 一个 '" +
                                    tyName(gt) + "' 值");
                } else if (gt != tInvalid && !canCoerce(gt, want)) {
                    if (!tryCoerceArrayLit(r->value, want)) {
                        typeError(r->value->loc, gt, want, "return 的值类型不匹配");
                    }
                }
                break;
            }
        }
    }

    // ---------------- 函数 / 程序 ----------------
    void checkFunc(FuncDecl* fn) {
        curFunc = fn;
        curFile = fn->file;
        if (fn->isExtern) {
            // extern fn 只有签名（已在收集阶段检查），没有函数体
            curFunc = nullptr;
            return;
        }
        pushScope();
        for (const Param& p : fn->params) {
            if (p.ty == tVoid) {
                err(p.loc, "参数 '" + p.name + "' 不能是 void 类型");
            }
            declare(VarInfo{p.name, p.ty, false, p.loc, nullptr, false});
        }
        for (Stmt* s : fn->body->stmts) checkStmt(s);
        popScope(true);  // 函数参数作用域：未使用的参数也提示

        // 非 void 函数必须有可达的 return 路径（0.5：完整的返回路径分析）
        // if/else 两个分支都 return、while(true)/loop 且不含 break 都算覆盖；
        // 普通结尾没有 return 的情况仍然只是警告（返回默认值）。
        // 例外（0.9）：返回类型是 struct 时“默认值”是 NULL 指针，落穿 = 随后
        // 访问字段必然崩，因此升级为硬错误。Optional 落穿 = none 是正确的，
        // 不在此列。
        if (fn->retTy != tVoid && !isOptional(fn->retTy) &&
            !stmtAlwaysReturns(fn->body)) {
            if (fn->retTy->kind == TyKind::Named) {
                diags.error(DiagCode::kEStructReturn, curFile, fn->body->loc,
                            "函数 '" + fn->name + "' 的返回类型是 struct '" +
                                tyName(fn->retTy) +
                                "'，存在不经过 return 的执行路径；"
                                "struct 没有可用的默认值（落穿会返回空指针，"
                                "访问字段将崩溃）。请让所有分支都 return。");
            } else {
                warn(DiagCode::kWMissingReturn, fn->body->loc,
                     "函数 '" + fn->name + "' 的返回类型是 '" +
                         tyName(fn->retTy) +
                         "'，但存在不经过 return 的执行路径；"
                         "运行时将返回该类型的默认值");
            }
        }
        curFunc = nullptr;
    }

    bool run() {
        // 0. 顶层 struct 声明（0.7）：先全部登记，再校验字段
        for (StructDecl* sd : prog->structs) {
            curFile = sd->file;
            if (findStruct(sd->name)) {
                err(sd->nameLoc, "struct '" + sd->name + "' 被重复定义");
                continue;
            }
            structs[sd->name] = sd;
        }
        for (StructDecl* sd : prog->structs) {
            curFile = sd->file;
            std::set<std::string> fieldNames;
            for (FieldDecl& f : sd->fields) {
                if (f.ty == tVoid) {
                    err(f.loc, "struct 字段不能是 void 类型");
                }
                if (f.ty && isOptional(f.ty)) {
                    err(f.loc, "struct '" + sd->name + "' 的字段 '" + f.name +
                                   "' 不能是可选类型（T? 只活于局部 / 参数 /"
                                   "返回值）；请改用普通字段 + 单独的 bool 标记");
                }
                if (f.ty && f.ty->kind == TyKind::Named &&
                    !findStruct(f.ty->name)) {
                    err(f.loc, "struct '" + sd->name + "' 的字段 '" + f.name +
                                   "' 用了未定义的类型 '" + f.ty->name + "'");
                }
                if (!fieldNames.insert(f.name).second) {
                    err(f.loc, "struct '" + sd->name + "' 的字段 '" + f.name +
                                   "' 被重复声明");
                }
            }
        }

        // 0.5 顶层全局常量：放在最外层作用域里，对所有函数可见
        pushScope();
        // 预注册函数签名：让常量初始值中的函数调用能正常解析，
        // 从而对"常量引用了非常量表达式"给出'必须在编译期确定下来'的
        // 定向报错，而不是误导性的"未定义的函数"（0.5.1）。
        // 重复定义的检测仍由第 1 步统一完成，这里遇到同名只跳过。
        for (FuncDecl* fn : prog->funcs) {
            bool visible =
                fn->module.empty() ||
                modules.memberVisible(fn->module, fn->name);
            if (visible && !funcs.count(fn->name)) funcs[fn->name] = fn;
        }
        for (GlobalConstDecl* gc : prog->consts) {
            curFile = gc->file;
            Expr* e = gc->init;
            const Ty* t = checkExpr(e);
            // 全局常量必须能在编译期确定：字面量，或字面量之间的运算（已折叠）。
            // 函数签名已预注册，非常量表达式（如函数调用）在此处会得到
            // 正确的'必须在编译期确定下来'报错，而不会先误报"未定义的函数"。
            bool foldable =
                e &&
                (e->kind == ExprKind::IntLit || e->kind == ExprKind::FloatLit ||
                 e->kind == ExprKind::BoolLit || e->kind == ExprKind::StrLit ||
                 e->kind == ExprKind::Ident);
            if (!foldable && e && e->kind == ExprKind::Binary) {
                foldable = static_cast<BinaryExpr*>(e)->folded;
            }
            // 引用已声明全局常量的标识符也算可折叠（const B = A;），
            // 但不能成环：constFoldable 对非字面量引用返回 false。
            if (foldable && e->kind == ExprKind::Ident) {
                auto* id = static_cast<IdentExpr*>(e);
                foldable = id->constRef && constFoldable(id->constRef->init);
            }
            if (!foldable) {
                std::string extra;
                if (e && e->kind == ExprKind::ArrayLit) {
                    extra = "（数组是引用类型，不能作为全局常量；"
                            "请在函数内用 let 声明）";
                }
                err(e ? e->loc : gc->loc,
                    "全局常量 '" + gc->name +
                        "' 的初始值必须是字面量或字面量之间的运算"
                        "（它必须在编译期确定下来）" +
                        extra);
                gc->resolved = gc->ann.present ? gc->ann.ty : tInvalid;
                declare(VarInfo{gc->name, gc->resolved, true, gc->nameLoc, gc,
                                false});
                continue;
            }
            const Ty* finalTy = t;
            if (gc->ann.present) {
                finalTy = gc->ann.ty;
                if (t != tInvalid && !canCoerce(t, finalTy)) {
                    typeError(e->loc, t, finalTy,
                              "全局常量 '" + gc->name + "' 的初始值类型不匹配");
                }
            }
            if (finalTy == tVoid) {
                err(gc->nameLoc, "常量不能被声明为 void 类型");
            }
            gc->resolved = finalTy;
            // 别名导入的模块不注入全局命名空间，只能通过 模块名/别名.常量 访问
            if (gc->module.empty() ||
                modules.memberVisible(gc->module, gc->name)) {
                declare(VarInfo{gc->name, finalTy, true, gc->nameLoc, gc,
                                false});
            }
            if (!gc->module.empty()) {
                qConsts[gc->module + "\x01" + gc->name] =
                    VarInfo{gc->name, finalTy, true, gc->nameLoc, gc, false};
            }
        }

        // 1. 收集函数签名（可见性：根文件 + 默认导入的模块）
        for (FuncDecl* fn : prog->funcs) {
            curFile = fn->file;
            if (builtinTable().count(fn->name)) {
                err(fn->nameLoc, "函数名 '" + fn->name +
                                             "' 与内置函数重名，请换一个名字");
            }
            bool visible =
                fn->module.empty() ||
                modules.memberVisible(fn->module, fn->name);
            if (visible) {
                auto it = funcs.find(fn->name);
                // 预注册阶段写入的是 fn 自身，不算重复；只有不同的
                // FuncDecl 占用了同名才算"被重复定义"
                if (it != funcs.end() && it->second != fn) {
                    err(fn->nameLoc, "函数 '" + fn->name + "' 被重复定义");
                } else {
                    funcs[fn->name] = fn;
                }
            }
            if (!fn->module.empty()) {
                std::string key = fn->module + "\x01" + fn->name;
                auto it = modFuncs.find(key);
                if (it != modFuncs.end()) {
                    err(fn->nameLoc, "模块 '" + fn->module + "' 中函数 '" +
                                                 fn->name + "' 被重复定义");
                } else {
                    modFuncs[key] = fn;
                }
            }
            if (fn->isExtern) {
                if (fn->retTy && fn->retTy->kind == TyKind::Array) {
                    err(fn->nameLoc,
                        "extern fn '" + fn->name +
                            "' 的返回类型不能是数组：lx_arr 是 Lux 运行时的"
                            "私有结构，C 侧没有对应类型；请改用指针 / 标量类型");
                }
                if (fn->retTy && isOptional(fn->retTy)) {
                    err(fn->nameLoc,
                        "extern fn '" + fn->name +
                            "' 的返回类型不能是可选类型：T? 是 Lux 运行时的"
                            "指针状私有表示，C 侧没有对应 ABI；"
                            "请改用指针 / 标量类型，或在 Lux 侧用包装函数");
                }
                for (const Param& p : fn->params) {
                    if (p.ty && p.ty->kind == TyKind::Array) {
                        err(p.loc,
                            "extern fn '" + fn->name + "' 的参数 '" + p.name +
                                "' 不能是数组：lx_arr 是 Lux 运行时的私有"
                                "结构，C 侧没有对应类型；请改用指针 / 标量类型");
                    }
                    if (p.ty && isOptional(p.ty)) {
                        err(p.loc,
                            "extern fn '" + fn->name + "' 的参数 '" + p.name +
                                "' 不能是可选类型：T? 是 Lux 运行时的指针状"
                                "私有表示，C 侧没有对应 ABI；"
                                "请改用指针 / 标量类型，或在 Lux 侧先解包");
                    }
                }
                if (fn->name.rfind("lx_", 0) == 0) {
                    warn(DiagCode::kWExternReserved, fn->nameLoc,
                         "外部函数名 '" + fn->name +
                             "' 以 lx_ 开头，与 Lux 运行时命名空间冲突");
                } else if (reservedExternNames().count(fn->name)) {
                    warn(DiagCode::kWExternReserved, fn->nameLoc,
                         "外部函数名 '" + fn->name +
                             "' 与 C 标准库符号重名，可能产生意外的行为");
                }
            }
        }

        // 2. 必须有 main 作为程序入口（运行时库编译时跳过）
        curFile = filename;
        auto mit = funcs.find("main");
        if (mit == funcs.end()) {
            if (needMain_)
                err(SourceLoc{1, 1},
                    "程序缺少入口函数：请定义 fn main() { ... }");
        } else {
            FuncDecl* mainFn = mit->second;
            if (mainFn->isExtern) {
                err(mainFn->nameLoc, "入口函数 main 不能是 extern 声明");
            }
            if (!mainFn->params.empty()) {
                // 可选签名 fn main(argv: string[])（0.7，A4）
                const Ty* pt = mainFn->params[0].ty;
                if (mainFn->params.size() > 1) {
                    err(mainFn->nameLoc,
                        "入口函数 main 最多只能带一个参数：argv: string[]");
                } else if (!(pt && pt->kind == TyKind::Array &&
                             pt->elem == tString)) {
                    err(mainFn->params[0].loc,
                        "入口函数 main 的参数必须是 argv: string[]（当前是 '" +
                            tyName(pt) + "'）");
                }
            }
            if (mainFn->retTy != tVoid && mainFn->retTy != tInt) {
                err(mainFn->nameLoc,
                    "入口函数 main 的返回类型只能是 void 或 int");
            }
        }

        // 3. 逐个检查函数体
        for (FuncDecl* fn : prog->funcs) checkFunc(fn);

        popScope(false);  // 全局常量作用域：不提示未使用
        return diags.ok();
    }
};

}  // namespace

bool analyze(Program* prog, Diags& diags, const std::string& filename,
             const ModuleInfo& modules, bool needMain) {
    Analyzer a(prog, diags, filename, modules, needMain);
    return a.run();
}

}  // namespace lux
