// =============================================================================
//  parser.cpp : 递归下降语法分析 —— Token 流 -> 抽象语法树(AST)
//
//  Lux 语法（第一版，完整文法见 docs/grammar.md）：
//
//    program   := funcDecl*
//    funcDecl  := 'fn' IDENT '(' params? ')' ('->' type)? block
//    block     := '{' stmt* '}'
//    stmt      := letDecl | if | while | for | loop | return | break
//               | continue | block | assign | exprStmt
//    letDecl   := ('let' | 'const') IDENT (':' type)? ('=' expr)? ';'
//    for       := 'for' IDENT 'in' expr ('..' | '..=') expr block
//    expr      := orExpr        （优先级由低到高逐层下降）
// =============================================================================
#include "lux.hpp"

#include <cmath>

namespace lux {

namespace {

// 是否为（复合）赋值运算符
bool isAssignOp(Tok k) {
    switch (k) {
        case Tok::Assign:
        case Tok::PlusEq:
        case Tok::MinusEq:
        case Tok::StarEq:
        case Tok::SlashEq:
        case Tok::PercentEq:
        case Tok::AmpEq:
        case Tok::PipeEq:
        case Tok::CaretEq:
        case Tok::ShlEq:
        case Tok::ShrEq:
            return true;
        default:
            return false;
    }
}

// 复合赋值运算符对应的二元运算（语法糖 x += e → x = x + e）
BinOp binOpOfAssign(Tok k) {
    switch (k) {
        case Tok::PlusEq: return BinOp::Add;
        case Tok::MinusEq: return BinOp::Sub;
        case Tok::StarEq: return BinOp::Mul;
        case Tok::SlashEq: return BinOp::Div;
        case Tok::PercentEq: return BinOp::Mod;
        case Tok::AmpEq: return BinOp::BitAnd;
        case Tok::PipeEq: return BinOp::BitOr;
        case Tok::CaretEq: return BinOp::BitXor;
        case Tok::ShlEq: return BinOp::Shl;
        case Tok::ShrEq: return BinOp::Shr;
        default: return BinOp::Add;  // 不可达（isAssignOp 已过滤）
    }
}

// 是否为合法的赋值左值（0.7）：变量 / 数组元素（可嵌套）/ struct 成员
bool isLValue(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Ident:
            return true;
        case ExprKind::Index:
            return isLValue(static_cast<const IndexExpr*>(e)->base);
        case ExprKind::Member:
            return isLValue(static_cast<const MemberExpr*>(e)->base);
        default:
            return false;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  Parser 基础设施
// -----------------------------------------------------------------------------

Parser::Parser(std::vector<Token> toks, std::string filename, Diags& diags)
    : toks_(std::move(toks)), filename_(std::move(filename)), diags_(diags) {}

const Token& Parser::peek(size_t off) const {
    size_t i = pos_ + off;
    if (i >= toks_.size()) return toks_.back();  // 恒为 Eof
    return toks_[i];
}

Token Parser::advance() {
    Token tk = cur();
    if (cur().kind != Tok::Eof) pos_++;
    return tk;
}

bool Parser::match(Tok k) {
    if (cur().kind == k) {
        advance();
        return true;
    }
    return false;
}

Token Parser::expect(Tok k, const char* what) {
    if (cur().kind != k) {
        errorAt(cur(), std::string("期望 ") + what + "，但读到了 " +
                           tokDesc(cur()));
    }
    return advance();
}

void Parser::errorHere(const std::string& msg) { errorAt(cur(), msg); }

void Parser::errorAt(const Token& tk, const std::string& msg) {
    diags_.error(DiagCode::kParse, filename_, tk.loc, msg);
    throw ParseBail{};
}

// -----------------------------------------------------------------------------
//  错误恢复：语法错误后把 token 流同步到下一个安全边界
// -----------------------------------------------------------------------------

// 语句边界：跳过当前出错的语句，停在下一条语句的开头
void Parser::syncStmt() {
    int depth = 0;
    for (;;) {
        if (at(Tok::Eof)) return;
        Tok k = cur().kind;
        if (depth == 0 && k == Tok::Semicolon) {
            advance();  // 吃掉出错语句的 ';'
            return;
        }
        if (k == Tok::LBrace) depth++;
        if (k == Tok::RBrace) {
            if (depth == 0) return;  // 当前块结束，交给外层处理
            depth--;
        }
        if (depth == 0 && (k == Tok::KwFn || k == Tok::KwLet || k == Tok::KwConst ||
                           k == Tok::KwIf || k == Tok::KwWhile || k == Tok::KwFor ||
                           k == Tok::KwLoop || k == Tok::KwBreak ||
                           k == Tok::KwContinue || k == Tok::KwReturn ||
                           k == Tok::KwRepeat || k == Tok::KwImport ||
                           k == Tok::KwExtern || k == Tok::KwStruct)) {
            return;
        }
        advance();
    }
}

// 顶层边界：跳过出错的顶层声明，停在下一个顶层声明的开头
void Parser::syncTopLevel() {
    int depth = 0;
    for (;;) {
        if (at(Tok::Eof)) return;
        Tok k = cur().kind;
        if (k == Tok::LBrace || k == Tok::LBracket) depth++;
        if ((k == Tok::RBrace || k == Tok::RBracket) && depth > 0) depth--;
        if (depth == 0 && (k == Tok::KwFn || k == Tok::KwImport ||
                           k == Tok::KwExtern || k == Tok::KwConst ||
                           k == Tok::KwStruct)) {
            return;
        }
        advance();
    }
}

// -----------------------------------------------------------------------------
//  类型
// -----------------------------------------------------------------------------

const Ty* Parser::parseTypeTok() {
    // 数组类型（前缀形式）： [T]（可嵌套，如 [[int]]）
    if (at(Tok::LBracket)) {
        advance();
        const Ty* elem = parseTypeTok();
        expect(Tok::RBracket, "']' 结束数组类型");
        return TyStore::arrayOf(elem);
    }
    const Ty* base;
    switch (cur().kind) {
        case Tok::KwInt:
            advance();
            base = TyStore::int64Ty();
            break;
        case Tok::KwFloat:
            advance();
            base = TyStore::float64Ty();
            break;
        case Tok::KwBool:
            advance();
            base = TyStore::boolTy();
            break;
        case Tok::KwString:
            advance();
            base = TyStore::stringTy();
            break;
        case Tok::KwVoid:
            advance();
            base = TyStore::voidTy();
            break;
        case Tok::Ident: {  // struct / 具名类型（0.7）
            Token n = advance();
            base = TyStore::named(n.text);
            break;
        }
        default:
            errorHere("期望类型名（int / float / bool / string / void / struct "
                      "名，或数组类型 int[] / [int] 等）");
            return TyStore::invalid();  // 不可达（errorHere 会抛出）
    }
    // 数组类型（后缀形式）： int[] / int[][]（0.5 新增）
    while (at(Tok::LBracket)) {
        advance();
        expect(Tok::RBracket, "']'（数组类型写作 int[] 或 [int]）");
        base = TyStore::arrayOf(base);
    }
    return base;
}

TypeAnn Parser::parseTypeAnn() {
    expect(Tok::Colon, "':' 和类型标注");
    TypeAnn ann;
    ann.loc = cur().loc;
    ann.present = true;
    ann.ty = parseTypeTok();
    return ann;
}

// -----------------------------------------------------------------------------
//  顶层
// -----------------------------------------------------------------------------

Program* Parser::parseProgram() {
    while (!at(Tok::Eof)) {
        try {
            if (at(Tok::KwImport)) {
                program_.imports.push_back(parseImport());
            } else if (at(Tok::KwExtern)) {
                program_.funcs.push_back(parseExternFn());
            } else if (at(Tok::KwFn)) {
                program_.funcs.push_back(parseFunc(false));
            } else if (at(Tok::KwConst)) {
                program_.consts.push_back(parseTopLevelConst());
            } else if (at(Tok::KwStruct)) {
                program_.structs.push_back(parseStruct());
            } else {
                errorHere("顶层只允许出现 import / fn / extern fn / const / "
                          "struct");
            }
        } catch (const ParseBail&) {
            // 语法错误已记录：同步到下一个顶层声明，继续解析
            if (diags_.errors > 100) break;
            size_t before = pos_;
            syncTopLevel();
            if (pos_ == before && !at(Tok::Eof)) advance();
        }
    }
    return &program_;
}

// 顶层 import： import "路径" (as 别名)? ;
ImportDecl* Parser::parseImport() {
    SourceLoc l = cur().loc;
    expect(Tok::KwImport, "'import'");
    Token p = expect(Tok::StrLit,
                     "导入路径（字符串），例如 import \"math\"、import "
                     "\"./util.lux\" 或 import \"c:m\"");
    std::string alias;
    if (match(Tok::KwAs)) {
        alias = expect(Tok::Ident, "别名").text;
    }
    expect(Tok::Semicolon, "';'");
    importPool_.push_back(std::make_unique<ImportDecl>());
    auto* imp = importPool_.back().get();
    imp->path = p.text;
    imp->alias = std::move(alias);
    imp->loc = l;
    imp->file = filename_;
    return imp;
}

// 顶层全局常量： const NAME (: 类型)? = 字面量;
GlobalConstDecl* Parser::parseTopLevelConst() {
    SourceLoc l = cur().loc;
    expect(Tok::KwConst, "'const'");
    Token name = expect(Tok::Ident, "常量名");

    auto* gc = new GlobalConstDecl();
    constPool_.push_back(std::unique_ptr<GlobalConstDecl>(gc));
    gc->name = name.text;
    gc->nameLoc = name.loc;
    gc->loc = l;
    gc->file = filename_;

    if (at(Tok::Colon)) gc->ann = parseTypeAnn();
    expect(Tok::Assign, "'=' 和初始值");
    gc->init = parseExpr();
    expect(Tok::Semicolon, "';'");
    return gc;
}

// 顶层 struct 声明（0.7）： struct Point { let x: int; let y: int; }
StructDecl* Parser::parseStruct() {
    SourceLoc l = cur().loc;
    expect(Tok::KwStruct, "'struct'");
    Token name = expect(Tok::Ident, "struct 名");

    auto* sd = new StructDecl();
    structPool_.push_back(std::unique_ptr<StructDecl>(sd));
    sd->name = name.text;
    sd->nameLoc = name.loc;
    sd->loc = l;
    sd->file = filename_;

    expect(Tok::LBrace, "'{' 开始 struct 字段列表");
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof)) errorHere("struct 声明没有闭合的 '}'");
        // 字段前缀 let / const 可选（两者语义都是"字段"，无 const 限制）
        match(Tok::KwLet);
        match(Tok::KwConst);
        Token fn = expect(Tok::Ident, "字段名");
        FieldDecl f;
        f.name = fn.text;
        f.loc = fn.loc;
        f.ann = parseTypeAnn();
        f.ty = f.ann.ty;
        expect(Tok::Semicolon, "';'");
        sd->fields.push_back(std::move(f));
    }
    expect(Tok::RBrace, "'}' 结束 struct 声明");
    return sd;
}

// extern fn 声明：只声明外部 C 函数，没有函数体
FuncDecl* Parser::parseExternFn() {
    expect(Tok::KwExtern, "'extern'");
    return parseFunc(true);
}

FuncDecl* Parser::parseFunc(bool isExtern) {
    SourceLoc l = cur().loc;
    expect(Tok::KwFn, "'fn'");
    Token name = expect(Tok::Ident, "函数名");

    auto* fn = new FuncDecl();
    funcPool_.push_back(std::unique_ptr<FuncDecl>(fn));
    fn->name = name.text;
    fn->nameLoc = name.loc;
    fn->loc = l;
    fn->isExtern = isExtern;
    fn->file = filename_;

    expect(Tok::LParen, "'(' 开始参数列表");
    if (!at(Tok::RParen)) {
        do {
            Token pn = expect(Tok::Ident, "参数名");
            Param p;
            p.name = pn.text;
            p.loc = pn.loc;
            p.ann = parseTypeAnn();
            p.ty = p.ann.ty;
            fn->params.push_back(p);
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "')' 结束参数列表");

    if (match(Tok::Arrow)) {
        fn->retAnn.present = true;
        fn->retAnn.loc = cur().loc;
        fn->retAnn.ty = parseTypeTok();
        fn->retTy = fn->retAnn.ty;
    } else {
        fn->retTy = TyStore::voidTy();
    }

    if (isExtern) {
        expect(Tok::Semicolon,
               "';'（extern fn 只是声明，以分号结束，没有函数体）");
        fn->body = nullptr;
    } else {
        fn->body = parseBlock();
    }
    return fn;
}

// -----------------------------------------------------------------------------
//  语句
// -----------------------------------------------------------------------------

BlockStmt* Parser::parseBlock() {
    SourceLoc l = cur().loc;
    expect(Tok::LBrace, "'{' 开始代码块");
    std::vector<Stmt*> stmts;
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof)) errorHere("代码块没有闭合的 '}'");
        try {
            Stmt* s = parseStmt();
            if (s) stmts.push_back(s);
        } catch (const ParseBail&) {
            // 语法错误已记录：同步到语句边界后继续解析
            if (diags_.errors > 100) return newStmt<BlockStmt>(l, stmts);
            size_t before = pos_;
            syncStmt();
            if (pos_ == before && !at(Tok::Eof)) advance();
        }
    }
    expect(Tok::RBrace, "'}'");
    return newStmt<BlockStmt>(l, stmts);
}

Stmt* Parser::parseStmt() {
    SourceLoc l = cur().loc;
    switch (cur().kind) {
        case Tok::KwLet:
        case Tok::KwConst:
            return parseLetOrConst();
        case Tok::KwIf:
            return parseIf();
        case Tok::KwWhile:
            return parseWhile();
        case Tok::KwFor:
            return parseFor();
        case Tok::KwRepeat: {
            // 语法糖： repeat n { ... }  等价于  for __repN in 0..n { ... }
            advance();
            Expr* n = parseExpr();
            Stmt* body = parseBlock();
            std::string var = "__rep" + std::to_string(repeatId_++);
            Expr* zero = newExpr<IntLitExpr>(l, 0);
            return newStmt<ForStmt>(l, var, l, zero, n, false, body);
        }
        case Tok::KwLoop: {
            advance();
            Stmt* body = parseBlock();
            return newStmt<LoopStmt>(l, body);
        }
        case Tok::KwBreak: {
            advance();
            expect(Tok::Semicolon, "';'");
            return newStmt<BreakStmt>(l);
        }
        case Tok::KwContinue: {
            advance();
            expect(Tok::Semicolon, "';'");
            return newStmt<ContinueStmt>(l);
        }
        case Tok::KwReturn:
            return parseReturn();
        case Tok::LBrace:
            return parseBlock();
        case Tok::Semicolon:
            advance();  // 空语句
            return nullptr;
        default:
            break;
    }

    // 语法糖： x++ / x-- 展开为 x = x + 1 / x = x - 1
    if (cur().kind == Tok::Ident &&
        (peek(1).kind == Tok::PlusPlus || peek(1).kind == Tok::MinusMinus)) {
        Token name = advance();
        Token opTok = advance();
        expect(Tok::Semicolon, "';'");
        BinOp op = (opTok.kind == Tok::PlusPlus) ? BinOp::Add : BinOp::Sub;
        Expr* lhs = newExpr<IdentExpr>(name.loc, name.text);
        Expr* one = newExpr<IntLitExpr>(opTok.loc, 1);
        Expr* rhs = newExpr<BinaryExpr>(opTok.loc, op, lhs, one);
        Expr* tgt = newExpr<IdentExpr>(name.loc, name.text);
        return newStmt<AssignStmt>(l, tgt, name.loc, rhs);
    }

    // 赋值 / 表达式语句（0.7：统一按左值表达式解析目标，支持
    // a[i] = v、grid[i][j] = v、p.x = v、a[i] += v 等形态）
    Expr* e = parseExpr();
    if (isAssignOp(cur().kind)) {
        Token opTok = advance();
        Expr* v = parseExpr();
        expect(Tok::Semicolon, "';'");
        if (!isLValue(e)) {
            errorAt(opTok,
                    "赋值号左边必须是变量、数组元素或 struct 成员（可嵌套）");
        }
        auto* as = newStmt<AssignStmt>(l, e, e->loc, v);
        if (opTok.kind != Tok::Assign) {
            // 复合赋值：语法糖与普通赋值同形，但代码生成只求值一次左值
            as->compound = true;
            as->compoundOp = binOpOfAssign(opTok.kind);
        }
        return as;
    }
    expect(Tok::Semicolon, "';'");
    return newStmt<ExprStmt>(l, e);
}

Stmt* Parser::parseLetOrConst() {
    SourceLoc l = cur().loc;
    bool isConst = cur().kind == Tok::KwConst;
    advance();
    Token name = expect(Tok::Ident, "变量名");

    TypeAnn ann;
    if (at(Tok::Colon)) ann = parseTypeAnn();

    Expr* init = nullptr;
    if (match(Tok::Assign)) init = parseExpr();

    expect(Tok::Semicolon, "';'");
    return newStmt<LetStmt>(l, name.text, name.loc, ann, isConst, init);
}

Stmt* Parser::parseIf() {
    SourceLoc l = cur().loc;
    expect(Tok::KwIf, "'if'");
    return parseIfRest(l);
}

// 解析 if 关键字之后的共同部分；elif 语法糖在这里展开为 else if 链
Stmt* Parser::parseIfRest(SourceLoc l) {
    bool saved = noStructLit_;
    noStructLit_ = true;  // `if x {` 中的 x 不能当 struct 字面量
    Expr* cond = parseExpr();
    noStructLit_ = saved;
    Stmt* thenB = parseBlock();
    Stmt* elseB = nullptr;
    if (match(Tok::KwElse)) {
        if (at(Tok::KwIf)) {
            advance();
            elseB = parseIfRest(cur().loc);
        } else {
            elseB = parseBlock();
        }
    } else if (match(Tok::KwElif)) {
        elseB = parseIfRest(cur().loc);
    }
    return newStmt<IfStmt>(l, cond, thenB, elseB);
}

Stmt* Parser::parseWhile() {
    SourceLoc l = cur().loc;
    expect(Tok::KwWhile, "'while'");
    bool saved = noStructLit_;
    noStructLit_ = true;
    Expr* cond = parseExpr();
    noStructLit_ = saved;
    Stmt* body = parseBlock();
    return newStmt<WhileStmt>(l, cond, body);
}

Stmt* Parser::parseFor() {
    SourceLoc l = cur().loc;
    expect(Tok::KwFor, "'for'");
    Token var = expect(Tok::Ident, "循环变量名");
    expect(Tok::KwIn, "'in'");
    bool saved = noStructLit_;
    noStructLit_ = true;
    Expr* start = parseExpr();

    // for-in 迭代（0.5 新增）：for x in arr / for c in s
    if (!at(Tok::DotDot) && !at(Tok::DotDotEq)) {
        noStructLit_ = saved;
        Stmt* body = parseBlock();
        auto* f = newStmt<ForStmt>(l, var.text, var.loc, nullptr, nullptr, false,
                                   body);
        f->mode = ForMode::In;
        f->iterable = start;
        return f;
    }

    bool inclusive = false;
    if (match(Tok::DotDot)) {
        inclusive = false;
    } else {
        match(Tok::DotDotEq);
        inclusive = true;
    }
    Expr* end = parseExpr();
    noStructLit_ = saved;
    Stmt* body = parseBlock();
    return newStmt<ForStmt>(l, var.text, var.loc, start, end, inclusive, body);
}

Stmt* Parser::parseReturn() {
    SourceLoc l = cur().loc;
    expect(Tok::KwReturn, "'return'");
    Expr* v = nullptr;
    if (!at(Tok::Semicolon)) v = parseExpr();
    expect(Tok::Semicolon, "';'");
    return newStmt<ReturnStmt>(l, v);
}

// -----------------------------------------------------------------------------
//  表达式（优先级爬升）
// -----------------------------------------------------------------------------

// 表达式深度限制：防止恶意 / 意外深嵌套把递归下降解析器压爆栈
namespace {
struct DepthGuard {
    int& d;
    explicit DepthGuard(int& depth) : d(depth) { d++; }
    ~DepthGuard() { d--; }
};
}  // namespace

Expr* Parser::parseExpr() {
    // DepthGuard 必须先构造：先判断后守卫的话，抛出"嵌套过深"的那一层
    // 永远走不到守卫构造，depth_ 会单调泄漏（0.5.1 修复）
    DepthGuard g(depth_);
    if (depth_ > 256) {
        errorAt(cur(), "表达式嵌套过深（超过 256 层）");
    }
    return parseOr();
}

// or := and (('||' | 'or') and)*
Expr* Parser::parseOr() {
    Expr* lhs = parseAnd();
    while (at(Tok::OrOr) || at(Tok::KwOr)) {
        SourceLoc l = cur().loc;
        advance();
        Expr* rhs = parseAnd();
        lhs = newExpr<BinaryExpr>(l, BinOp::LogicOr, lhs, rhs);
    }
    return lhs;
}

// and := bitOr (('&&' | 'and') bitOr)*
Expr* Parser::parseAnd() {
    Expr* lhs = parseBitOr();
    while (at(Tok::AndAnd) || at(Tok::KwAnd)) {
        SourceLoc l = cur().loc;
        advance();
        Expr* rhs = parseBitOr();
        lhs = newExpr<BinaryExpr>(l, BinOp::LogicAnd, lhs, rhs);
    }
    return lhs;
}

// bitOr := bitXor ('|' bitXor)*
Expr* Parser::parseBitOr() {
    Expr* lhs = parseBitXor();
    while (at(Tok::Pipe)) {
        SourceLoc l = cur().loc;
        advance();
        Expr* rhs = parseBitXor();
        lhs = newExpr<BinaryExpr>(l, BinOp::BitOr, lhs, rhs);
    }
    return lhs;
}

// bitXor := bitAnd ('^' bitAnd)*
Expr* Parser::parseBitXor() {
    Expr* lhs = parseBitAnd();
    while (at(Tok::Caret)) {
        SourceLoc l = cur().loc;
        advance();
        Expr* rhs = parseBitAnd();
        lhs = newExpr<BinaryExpr>(l, BinOp::BitXor, lhs, rhs);
    }
    return lhs;
}

// bitAnd := equality ('&' equality)*
Expr* Parser::parseBitAnd() {
    Expr* lhs = parseEquality();
    while (at(Tok::Amp)) {
        SourceLoc l = cur().loc;
        advance();
        Expr* rhs = parseEquality();
        lhs = newExpr<BinaryExpr>(l, BinOp::BitAnd, lhs, rhs);
    }
    return lhs;
}

// equality := comparison (('==' | '!=') comparison)*
Expr* Parser::parseEquality() {
    Expr* lhs = parseComparison();
    while (at(Tok::Eq) || at(Tok::Ne)) {
        SourceLoc l = cur().loc;
        BinOp op = (cur().kind == Tok::Eq) ? BinOp::Eq : BinOp::Ne;
        advance();
        Expr* rhs = parseComparison();
        lhs = newExpr<BinaryExpr>(l, op, lhs, rhs);
    }
    return lhs;
}

// comparison := shift (('<' | '<=' | '>' | '>=') shift)*
Expr* Parser::parseComparison() {
    Expr* lhs = parseShift();
    while (at(Tok::Lt) || at(Tok::Le) || at(Tok::Gt) || at(Tok::Ge)) {
        SourceLoc l = cur().loc;
        BinOp op;
        switch (cur().kind) {
            case Tok::Lt: op = BinOp::Lt; break;
            case Tok::Le: op = BinOp::Le; break;
            case Tok::Gt: op = BinOp::Gt; break;
            default: op = BinOp::Ge; break;
        }
        advance();
        Expr* rhs = parseShift();
        lhs = newExpr<BinaryExpr>(l, op, lhs, rhs);
    }
    return lhs;
}

// shift := additive (('<<' | '>>') additive)*
Expr* Parser::parseShift() {
    Expr* lhs = parseAdditive();
    while (at(Tok::Shl) || at(Tok::Shr)) {
        SourceLoc l = cur().loc;
        BinOp op = (cur().kind == Tok::Shl) ? BinOp::Shl : BinOp::Shr;
        advance();
        Expr* rhs = parseAdditive();
        lhs = newExpr<BinaryExpr>(l, op, lhs, rhs);
    }
    return lhs;
}

// additive := multiplicative (('+' | '-') multiplicative)*
Expr* Parser::parseAdditive() {
    Expr* lhs = parseMultiplicative();
    while (at(Tok::Plus) || at(Tok::Minus)) {
        SourceLoc l = cur().loc;
        BinOp op = (cur().kind == Tok::Plus) ? BinOp::Add : BinOp::Sub;
        advance();
        Expr* rhs = parseMultiplicative();
        lhs = newExpr<BinaryExpr>(l, op, lhs, rhs);
    }
    return lhs;
}

// multiplicative := unary (('*' | '/' | '%') unary)*
Expr* Parser::parseMultiplicative() {
    Expr* lhs = parseUnary();
    while (at(Tok::Star) || at(Tok::Slash) || at(Tok::Percent)) {
        SourceLoc l = cur().loc;
        BinOp op;
        switch (cur().kind) {
            case Tok::Star: op = BinOp::Mul; break;
            case Tok::Slash: op = BinOp::Div; break;
            default: op = BinOp::Mod; break;
        }
        advance();
        Expr* rhs = parseUnary();
        lhs = newExpr<BinaryExpr>(l, op, lhs, rhs);
    }
    return lhs;
}

// unary := ('-' | '!' | 'not' | '~') unary | primary
Expr* Parser::parseUnary() {
    SourceLoc l = cur().loc;
    UnOp op;
    bool isUnary = true;
    switch (cur().kind) {
        case Tok::Minus: op = UnOp::Neg; break;
        case Tok::Bang: op = UnOp::LogicNot; break;
        case Tok::KwNot: op = UnOp::LogicNot; break;
        case Tok::Tilde: op = UnOp::BitNot; break;
        default: isUnary = false; break;
    }
    if (!isUnary) return parsePrimary();

    advance();
    // 常量折叠： -5 直接折叠成整数字面量，让生成的 C 更干净
    if (op == UnOp::Neg && at(Tok::IntLit)) {
        Token tk = advance();
        return newExpr<IntLitExpr>(l, -tk.ival);
    }
    if (op == UnOp::Neg && at(Tok::FloatLit)) {
        Token tk = advance();
        return newExpr<FloatLitExpr>(l, -tk.fval);
    }
    Expr* operand = parseUnary();
    return newExpr<UnaryExpr>(l, op, operand);
}

Expr* Parser::parsePrimary() {
    // 0.5 新增：primary 之后可以跟连续的后缀下标 a[i][j]
    return parsePostfix(parsePrimaryBase());
}

// 后缀下标： e '[' expr ']'（数组取元素 / 字符串取单字节字符）
// 0.6 新增切片：e[lo..hi] / e[lo..=hi]，端点可省略（e[..hi] / e[lo..]）
Expr* Parser::parsePostfix(Expr* e) {
    for (;;) {
        if (at(Tok::LBracket)) {
            SourceLoc l = cur().loc;
            advance();
            Expr* first = nullptr;
            // '[..' 开头：start 省略
            if (!at(Tok::DotDot) && !at(Tok::DotDotEq)) {
                first = parseExpr();
            }
            if (at(Tok::DotDot) || at(Tok::DotDotEq)) {
                bool inclusive = match(Tok::DotDotEq);
                if (!inclusive) match(Tok::DotDot);
                Expr* end = nullptr;
                // '..]' 结尾：end 省略（到末尾）
                if (!at(Tok::RBracket)) end = parseExpr();
                expect(Tok::RBracket, "']' 结束切片");
                e = newExpr<SliceExpr>(l, e, first, end, inclusive);
            } else {
                expect(Tok::RBracket, "']' 结束下标访问");
                e = newExpr<IndexExpr>(l, e, first);
            }
        } else if (at(Tok::Dot) && peek(1).kind == Tok::Ident) {
            // 成员访问 arr[0].x（0.7）
            advance();  // '.'
            Token m = advance();
            e = newExpr<MemberExpr>(m.loc, e, m.text, m.loc);
        } else {
            break;
        }
    }
    return e;
}

// if 表达式（0.7）： if c { a } else { b }，两分支都是单个表达式
Expr* Parser::parseIfExpr() {
    SourceLoc l = cur().loc;
    expect(Tok::KwIf, "'if'");
    bool saved = noStructLit_;
    noStructLit_ = true;
    Expr* cond = parseExpr();
    noStructLit_ = saved;
    expect(Tok::LBrace, "'{'");
    Expr* thenV = parseExpr();
    expect(Tok::RBrace, "'}'");
    expect(Tok::KwElse, "'else'（if 表达式必须有 else 分支）");
    expect(Tok::LBrace, "'{'");
    Expr* elseV = parseExpr();
    expect(Tok::RBrace, "'}'");
    return newExpr<IfExpr>(l, cond, thenV, elseV);
}

Expr* Parser::parsePrimaryBase() {
    SourceLoc l = cur().loc;

    switch (cur().kind) {
        case Tok::IntLit: {
            Token tk = advance();
            return newExpr<IntLitExpr>(l, tk.ival);
        }
        case Tok::FloatLit: {
            Token tk = advance();
            return newExpr<FloatLitExpr>(l, tk.fval);
        }
        case Tok::KwNan: {
            advance();
            return newExpr<FloatLitExpr>(l, std::nan(""));
        }
        case Tok::KwInf: {
            advance();
            return newExpr<FloatLitExpr>(l, HUGE_VAL);
        }
        case Tok::StrLit: {
            Token tk = advance();
            return newExpr<StrLitExpr>(l, tk.text);
        }
        case Tok::KwTrue: {
            advance();
            return newExpr<BoolLitExpr>(l, true);
        }
        case Tok::KwFalse: {
            advance();
            return newExpr<BoolLitExpr>(l, false);
        }
        case Tok::KwIf:
            return parseIfExpr();
        case Tok::LParen: {
            advance();
            Expr* e = parseExpr();
            expect(Tok::RParen, "')'");
            return e;
        }
        // 数组字面量： [e1, e2, ...]（0.5 新增）
        case Tok::LBracket: {
            advance();
            std::vector<Expr*> elems;
            if (!at(Tok::RBracket)) {
                do {
                    elems.push_back(parseExpr());
                } while (match(Tok::Comma) && !at(Tok::RBracket));
            }
            expect(Tok::RBracket, "']' 结束数组字面量");
            return newExpr<ArrayLitExpr>(l, std::move(elems));
        }
        // 类型转换： int(x) / float(x) / string(x)
        case Tok::KwInt:
        case Tok::KwFloat:
        case Tok::KwString: {
            if (peek(1).kind == Tok::LParen) {
                Token kw = advance();
                advance();  // '('
                return finishCall(l, kw.text, kw.loc);
            }
            errorHere("类型名只能在类型标注或类型转换中出现");
        }
        case Tok::Ident: {
            // 标识符，收集点号连接的分量（模块限定访问 / struct 成员 / 方法）
            Token name = advance();
            std::vector<std::pair<std::string, SourceLoc>> parts;
            parts.emplace_back(name.text, name.loc);
            while (at(Tok::Dot) && peek(1).kind == Tok::Ident) {
                advance();  // '.'
                Token m = advance();
                parts.emplace_back(m.text, m.loc);
            }
            // 函数调用：限定名整体作为 callee（mod.fn / a.push）
            if (at(Tok::LParen)) {
                advance();  // '('
                std::string full = parts[0].first;
                for (size_t i = 1; i < parts.size(); i++)
                    full += "." + parts[i].first;
                return finishCall(l, full, name.loc);
            }
            // struct 字面量 Point { x: 1, y: 2 }（0.7）
            // 只在 '{' 后是 '}' 或 IDENT ':' 时判定，避免与普通语句块冲突。
            if (!noStructLit_ && parts.size() == 1 && at(Tok::LBrace) &&
                ((peek(1).kind == Tok::RBrace) ||
                 (peek(1).kind == Tok::Ident && peek(2).kind == Tok::Colon))) {
                advance();  // '{'
                std::vector<std::pair<std::string, Expr*>> inits;
                if (!at(Tok::RBrace)) {
                    do {
                        Token fn = expect(Tok::Ident, "字段名");
                        expect(Tok::Colon, "':'");
                        inits.emplace_back(fn.text, parseExpr());
                    } while (match(Tok::Comma) && !at(Tok::RBrace));
                }
                expect(Tok::RBrace, "'}' 结束 struct 字面量");
                return newExpr<StructLitExpr>(l, name.text, name.loc,
                                              std::move(inits));
            }
            // 普通标识符 / 成员访问链
            Expr* e = newExpr<IdentExpr>(name.loc, parts[0].first);
            for (size_t i = 1; i < parts.size(); i++) {
                e = newExpr<MemberExpr>(parts[i].second, e, parts[i].first,
                                        parts[i].second);
            }
            return e;
        }
        default:
            errorHere(std::string("这里是表达式的开头位置，但读到了 ") +
                      tokDesc(cur()));
    }
}

// 解析 '(' 之后的参数列表并构造 CallExpr
Expr* Parser::finishCall(SourceLoc l, const std::string& name, SourceLoc nameLoc) {
    std::vector<Expr*> args;
    if (!at(Tok::RParen)) {
        do {
            args.push_back(parseExpr());
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "')' 结束参数列表");
    return newExpr<CallExpr>(l, name, nameLoc, args);
}

}  // namespace lux
