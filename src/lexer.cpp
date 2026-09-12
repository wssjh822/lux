// =============================================================================
//  lexer.cpp : 词法分析 —— 把源码字符流切分为 Token 流
// =============================================================================
#include "lux.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace lux {

// -----------------------------------------------------------------------------
//  Token 名称（用于诊断信息）
// -----------------------------------------------------------------------------

const char* binOpText(BinOp op) {
    switch (op) {
        case BinOp::Add:
            return "+";
        case BinOp::Sub:
            return "-";
        case BinOp::Mul:
            return "*";
        case BinOp::Div:
            return "/";
        case BinOp::Mod:
            return "%";
        case BinOp::Eq:
            return "==";
        case BinOp::Ne:
            return "!=";
        case BinOp::Lt:
            return "<";
        case BinOp::Le:
            return "<=";
        case BinOp::Gt:
            return ">";
        case BinOp::Ge:
            return ">=";
        case BinOp::LogicAnd:
            return "&&";
        case BinOp::LogicOr:
            return "||";
        case BinOp::BitAnd:
            return "&";
        case BinOp::BitOr:
            return "|";
        case BinOp::BitXor:
            return "^";
        case BinOp::Shl:
            return "<<";
        case BinOp::Shr:
            return ">>";
        default:
            return "?";
    }
}

const char* unOpText(UnOp op) {
    switch (op) {
        case UnOp::Neg:
            return "-";
        case UnOp::LogicNot:
            return "!";
        case UnOp::BitNot:
            return "~";
        default:
            return "?";
    }
}

const char* tokName(Tok t) {
    switch (t) {
        case Tok::Eof:
            return "Eof";
        case Tok::Ident:
            return "Ident";
        case Tok::IntLit:
            return "IntLit";
        case Tok::FloatLit:
            return "FloatLit";
        case Tok::StrLit:
            return "StrLit";
        case Tok::KwFn:
            return "fn";
        case Tok::KwLet:
            return "let";
        case Tok::KwConst:
            return "const";
        case Tok::KwReturn:
            return "return";
        case Tok::KwIf:
            return "if";
        case Tok::KwElse:
            return "else";
        case Tok::KwWhile:
            return "while";
        case Tok::KwFor:
            return "for";
        case Tok::KwLoop:
            return "loop";
        case Tok::KwIn:
            return "in";
        case Tok::KwBreak:
            return "break";
        case Tok::KwContinue:
            return "continue";
        case Tok::KwTrue:
            return "true";
        case Tok::KwFalse:
            return "false";
        case Tok::KwAnd:
            return "and";
        case Tok::KwOr:
            return "or";
        case Tok::KwNot:
            return "not";
        case Tok::KwImport:
            return "import";
        case Tok::KwExtern:
            return "extern";
        case Tok::KwAs:
            return "as";
        case Tok::KwElif:
            return "elif";
        case Tok::KwRepeat:
            return "repeat";
        case Tok::KwStruct:
            return "struct";
        case Tok::KwNan:
            return "nan";
        case Tok::KwInf:
            return "inf";
        case Tok::KwInt:
            return "int";
        case Tok::KwFloat:
            return "float";
        case Tok::KwBool:
            return "bool";
        case Tok::KwString:
            return "string";
        case Tok::KwVoid:
            return "void";
        case Tok::LParen:
            return "(";
        case Tok::RParen:
            return ")";
        case Tok::LBrace:
            return "{";
        case Tok::RBrace:
            return "}";
        case Tok::LBracket:
            return "[";
        case Tok::RBracket:
            return "]";
        case Tok::Comma:
            return ",";
        case Tok::Semicolon:
            return ";";
        case Tok::Colon:
            return ":";
        case Tok::Arrow:
            return "->";
        case Tok::Dot:
            return ".";
        case Tok::DotDot:
            return "..";
        case Tok::DotDotEq:
            return "..=";
        case Tok::Plus:
            return "+";
        case Tok::Minus:
            return "-";
        case Tok::Star:
            return "*";
        case Tok::Slash:
            return "/";
        case Tok::Percent:
            return "%";
        case Tok::Assign:
            return "=";
        case Tok::Eq:
            return "==";
        case Tok::Ne:
            return "!=";
        case Tok::Lt:
            return "<";
        case Tok::Le:
            return "<=";
        case Tok::Gt:
            return ">";
        case Tok::Ge:
            return ">=";
        case Tok::AndAnd:
            return "&&";
        case Tok::OrOr:
            return "||";
        case Tok::Bang:
            return "!";
        case Tok::Amp:
            return "&";
        case Tok::Pipe:
            return "|";
        case Tok::Caret:
            return "^";
        case Tok::Tilde:
            return "~";
        case Tok::Shl:
            return "<<";
        case Tok::Shr:
            return ">>";
        case Tok::PlusPlus:
            return "++";
        case Tok::MinusMinus:
            return "--";
        case Tok::PlusEq:
            return "+=";
        case Tok::MinusEq:
            return "-=";
        case Tok::StarEq:
            return "*=";
        case Tok::SlashEq:
            return "/=";
        case Tok::PercentEq:
            return "%=";
        case Tok::AmpEq:
            return "&=";
        case Tok::PipeEq:
            return "|=";
        case Tok::CaretEq:
            return "^=";
        case Tok::ShlEq:
            return "<<=";
        case Tok::ShrEq:
            return ">>=";
        default:
            return "?";
    }
}

std::string tokDesc(const Token& tk) {
    switch (tk.kind) {
        case Tok::Eof:
            return "文件结尾";
        case Tok::Ident:
            return "标识符 '" + tk.text + "'";
        case Tok::IntLit:
            return "整数 '" + tk.text + "'";
        case Tok::FloatLit:
            return "浮点数 '" + tk.text + "'";
        case Tok::StrLit:
            return "字符串字面量";
        default:
            return std::string("'") + tokName(tk.kind) + "'";
    }
}

// -----------------------------------------------------------------------------
//  Lexer
// -----------------------------------------------------------------------------

namespace {

const std::unordered_map<std::string, Tok>& keywordTable() {
    static const std::unordered_map<std::string, Tok> table = {
        {"fn", Tok::KwFn},         {"let", Tok::KwLet},
        {"const", Tok::KwConst},   {"return", Tok::KwReturn},
        {"if", Tok::KwIf},         {"else", Tok::KwElse},
        {"elif", Tok::KwElif},     {"while", Tok::KwWhile},
        {"for", Tok::KwFor},       {"loop", Tok::KwLoop},
        {"in", Tok::KwIn},         {"break", Tok::KwBreak},
        {"continue", Tok::KwContinue}, {"repeat", Tok::KwRepeat},
        {"true", Tok::KwTrue},     {"false", Tok::KwFalse},
        {"and", Tok::KwAnd},       {"or", Tok::KwOr},
        {"not", Tok::KwNot},       {"import", Tok::KwImport},
        {"extern", Tok::KwExtern}, {"int", Tok::KwInt},
        {"float", Tok::KwFloat},   {"bool", Tok::KwBool},
        {"string", Tok::KwString}, {"void", Tok::KwVoid},
        {"as", Tok::KwAs},
        {"struct", Tok::KwStruct},
        {"nan", Tok::KwNan},       {"inf", Tok::KwInf},
    };
    return table;
}

// 词法错误被记录后抛出的内部异常：run() 捕获后跳过坏字符继续，
// 一次词法分析尽量报全所有错误（错误太多时按预算截断）。
struct LexBail {};

struct Lexer {
    const std::string& src;
    const std::string& filename;
    Diags& diags;
    size_t pos = 0;
    int line = 1;
    int col = 1;

    Lexer(const std::string& s, const std::string& f, Diags& d)
        : src(s), filename(f), diags(d) {}

    bool eof() const { return pos >= src.size(); }
    char cur() const { return src[pos]; }
    char at(size_t off) const { return pos + off < src.size() ? src[pos + off] : '\0'; }

    void bump() {
        if (src[pos] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
        pos++;
    }

    SourceLoc loc() const { return SourceLoc{line, col}; }

    [[noreturn]] void error(SourceLoc l, const std::string& msg) const {
        diags.error(DiagCode::kLex, filename, l, msg);
        throw LexBail{};
    }

    void skipWhitespaceAndComments() {
        for (;;) {
            while (!eof() && (cur() == ' ' || cur() == '\t' || cur() == '\r' ||
                              cur() == '\n' || cur() == '\f' || cur() == '\v')) {
                bump();
            }
            if (eof()) return;
            // 行注释
            if (cur() == '/' && at(1) == '/') {
                while (!eof() && cur() != '\n') bump();
                continue;
            }
            // 块注释（支持嵌套）
            if (cur() == '/' && at(1) == '*') {
                SourceLoc start = loc();
                int depth = 0;
                while (!eof()) {
                    if (cur() == '/' && at(1) == '*') {
                        depth++;
                        bump();
                        bump();
                    } else if (cur() == '*' && at(1) == '/') {
                        depth--;
                        bump();
                        bump();
                        if (depth == 0) break;
                    } else {
                        bump();
                    }
                }
                if (depth != 0) error(start, "块注释 '/*' 没有对应的结束符 '*/'");
                continue;
            }
            return;
        }
    }

    // 把 UTF-32 码点编码为 UTF-8 追加到 out
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // 读取固定长度的十六进制数字（\xHH / \uXXXX）
    uint32_t readHex(int digits, SourceLoc escLoc) {
        uint32_t v = 0;
        for (int i = 0; i < digits; i++) {
            int h = hexVal(cur());
            if (h < 0) {
                error(escLoc, "转义序列需要 " + std::to_string(digits) +
                                  " 位十六进制数字");
            }
            v = v * 16 + static_cast<uint32_t>(h);
            bump();
        }
        return v;
    }

    // 原始字符串 r"..."（0.7，B2）：不做任何转义处理，
    // 内容原样保留（含反斜杠），遇到下一个 '"' 结束。
    std::string scanRawString() {
        SourceLoc start = loc();
        bump();  // 'r'
        bump();  // '"'
        std::string out;
        while (!eof() && cur() != '"') {
            if (cur() == '\n') error(start, "原始字符串字面量没有闭合的 '\"'");
            out += cur();
            bump();
        }
        if (eof()) error(start, "原始字符串字面量没有闭合的 '\"'");
        bump();  // 吃掉结尾的 "
        return out;
    }

    std::string scanString() {
        SourceLoc start = loc();
        bump();  // 吃掉开头的 "
        std::string out;
        while (!eof() && cur() != '"') {
            if (cur() == '\n') {
                error(start, "字符串字面量没有闭合的 '\"'");
            }
            if (cur() != '\\') {
                out += cur();
                bump();
                continue;
            }
            SourceLoc escLoc = loc();
            bump();  // 吃掉 '\'
            if (eof()) error(start, "字符串字面量没有闭合的 '\"'");
            char e = cur();
            switch (e) {
                case 'n': out += '\n'; bump(); break;
                case 't': out += '\t'; bump(); break;
                case 'r': out += '\r'; bump(); break;
                case '0': out += '\0'; bump(); break;
                case 'a': out += '\a'; bump(); break;
                case 'b': out += '\b'; bump(); break;
                case 'f': out += '\f'; bump(); break;
                case 'v': out += '\v'; bump(); break;
                case 'e': out += '\x1B'; bump(); break;
                case '\\': out += '\\'; bump(); break;
                case '"': out += '"'; bump(); break;
                case '\'': out += '\''; bump(); break;
                case 'x': {
                    bump();
                    uint32_t v = readHex(2, escLoc);
                    appendUtf8(out, v);
                    break;
                }
                case 'u': {
                    bump();
                    uint32_t v = readHex(4, escLoc);
                    appendUtf8(out, v);
                    break;
                }
                case 'U': {
                    bump();
                    uint32_t v = readHex(8, escLoc);
                    appendUtf8(out, v);
                    break;
                }
                default:
                    error(escLoc, std::string("无法识别的转义序列 '\\") + e + "'");
            }
        }
        if (eof()) error(start, "字符串字面量没有闭合的 '\"'");
        bump();  // 吃掉结尾的 "
        return out;
    }

    // 扫描数字字面量，返回 false 表示这只是个普通的 '.'
    bool scanNumber(Token& out) {
        SourceLoc start = loc();
        size_t begin = pos;
        bool isFloat = false;

        auto isDigitChar = [](char c) { return c >= '0' && c <= '9'; };

        // ---- 带前缀的进制 ----
        if (cur() == '0' && (at(1) == 'x' || at(1) == 'X' || at(1) == 'b' ||
                             at(1) == 'B' || at(1) == 'o' || at(1) == 'O')) {
            char p = at(1);
            int base = (p == 'x' || p == 'X') ? 16 : ((p == 'b' || p == 'B') ? 2 : 8);
            bump();
            bump();
            size_t digitsBegin = pos;
            unsigned long long v = 0;
            bool overflow = false;
            while (!eof() && (hexVal(cur()) >= 0 || cur() == '_')) {
                if (cur() == '_') {
                    bump();
                    continue;
                }
                int d = hexVal(cur());
                if (d >= base) {
                    error(loc(), std::string("数字 '") + cur() +
                                     "' 不属于该进制（基数 " + std::to_string(base) +
                                     "）");
                }
                if (v > (~0ULL - static_cast<unsigned long long>(d)) / base) {
                    overflow = true;
                }
                v = v * base + static_cast<unsigned long long>(d);
                bump();
            }
            if (pos == digitsBegin) error(start, "进制前缀后缺少数字");
            if (overflow) error(start, "整数字面量超出 64 位范围");
            // 数字后面不能紧跟标识符字符
            if (!eof() && (std::isalnum(static_cast<unsigned char>(cur())) ||
                           cur() == '_')) {
                error(loc(), "数字字面量后面紧跟了非法字符");
            }
            out.kind = Tok::IntLit;
            out.ival = static_cast<long long>(v);
            out.text = src.substr(begin, pos - begin);
            out.loc = start;
            return true;
        }

        // ---- 十进制 ----
        while (!eof() && (isDigitChar(cur()) || cur() == '_')) bump();
        // 小数点后必须紧跟数字，这样 "0..10" 才能正确切分成 0 .. 10
        if (cur() == '.' && isDigitChar(at(1))) {
            isFloat = true;
            bump();
            while (!eof() && (isDigitChar(cur()) || cur() == '_')) bump();
        }
        if (cur() == 'e' || cur() == 'E') {
            size_t save = pos;
            size_t peekOff = 1;
            if (at(peekOff) == '+' || at(peekOff) == '-') peekOff++;
            if (isDigitChar(at(peekOff))) {
                isFloat = true;
                bump();  // e
                if (cur() == '+' || cur() == '-') bump();
                while (!eof() && (isDigitChar(cur()) || cur() == '_')) bump();
            } else {
                (void)save;
            }
        }

        std::string text = src.substr(begin, pos - begin);
        std::string clean;
        for (char c : text) {
            if (c != '_') clean += c;
        }

        if (!eof() && (std::isalnum(static_cast<unsigned char>(cur())) ||
                       cur() == '_')) {
            error(loc(), "数字字面量后面紧跟了非法字符");
        }

        if (isFloat) {
            out.kind = Tok::FloatLit;
            out.fval = std::strtod(clean.c_str(), nullptr);
        } else {
            out.kind = Tok::IntLit;
            out.text = clean;
            errno = 0;
            char* endp = nullptr;
            long long v = std::strtoll(clean.c_str(), &endp, 10);
            if (errno == ERANGE) error(start, "整数字面量超出 64 位有符号范围");
            out.ival = v;
        }
        out.text = text;
        out.loc = start;
        return true;
    }

    Token scanIdentOrKeyword() {
        SourceLoc start = loc();
        size_t begin = pos;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(cur())) ||
                          cur() == '_')) {
            bump();
        }
        std::string text = src.substr(begin, pos - begin);
        Token out;
        out.text = text;
        out.loc = start;
        auto it = keywordTable().find(text);
        out.kind = (it != keywordTable().end()) ? it->second : Tok::Ident;
        return out;
    }

    std::vector<Token> run() {
        std::vector<Token> toks;
        for (;;) {
            skipWhitespaceAndComments();
            if (eof()) break;
            SourceLoc start = loc();
            Token tk;
            tk.loc = start;
            char c = cur();

            // 词法错误恢复：跳过坏字符继续，一次报全所有错误（按预算截断）
            try {
                // 原始字符串 r"..."（必须在标识符分支之前处理）
                if (c == 'r' && at(1) == '"') {
                    tk.kind = Tok::StrLit;
                    tk.text = scanRawString();
                    toks.push_back(tk);
                    continue;
                }
                // 标识符 / 关键字
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                    toks.push_back(scanIdentOrKeyword());
                    continue;
                }
                // 数字
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    Token num;
                    scanNumber(num);
                    toks.push_back(num);
                    continue;
                }
                // 字符串
                if (c == '"') {
                    tk.kind = Tok::StrLit;
                    tk.text = scanString();
                    toks.push_back(tk);
                    continue;
                }

                auto two = [&](char a, char b) { return c == a && at(1) == b; };
                auto three = [&](char a, char b, char d) {
                    return c == a && at(1) == b && at(2) == d;
                };

                // 多字符运算符优先匹配
                if (three('.', '.', '=')) {
                    tk.kind = Tok::DotDotEq;
                    tk.text = "..=";
                    bump();
                    bump();
                    bump();
                } else if (two('.', '.')) {
                    tk.kind = Tok::DotDot;
                    tk.text = "..";
                    bump();
                    bump();
                } else if (two('-', '>')) {
                    tk.kind = Tok::Arrow;
                    tk.text = "->";
                    bump();
                    bump();
                } else if (two('=', '=')) {
                    tk.kind = Tok::Eq;
                    tk.text = "==";
                    bump();
                    bump();
                } else if (two('!', '=')) {
                    tk.kind = Tok::Ne;
                    tk.text = "!=";
                    bump();
                    bump();
                } else if (two('<', '=')) {
                    tk.kind = Tok::Le;
                    tk.text = "<=";
                    bump();
                    bump();
                } else if (two('>', '=')) {
                    tk.kind = Tok::Ge;
                    tk.text = ">=";
                    bump();
                    bump();
                } else if (two('&', '&')) {
                    tk.kind = Tok::AndAnd;
                    tk.text = "&&";
                    bump();
                    bump();
                } else if (two('|', '|')) {
                    tk.kind = Tok::OrOr;
                    tk.text = "||";
                    bump();
                    bump();
                } else if (two('<', '<')) {
                    if (at(2) == '=') {
                        tk.kind = Tok::ShlEq;
                        tk.text = "<<=";
                        bump();
                        bump();
                        bump();
                    } else {
                        tk.kind = Tok::Shl;
                        tk.text = "<<";
                        bump();
                        bump();
                    }
                } else if (two('>', '>')) {
                    if (at(2) == '=') {
                        tk.kind = Tok::ShrEq;
                        tk.text = ">>=";
                        bump();
                        bump();
                        bump();
                    } else {
                        tk.kind = Tok::Shr;
                        tk.text = ">>";
                        bump();
                        bump();
                    }
                } else if (two('+', '+')) {
                    tk.kind = Tok::PlusPlus;
                    tk.text = "++";
                    bump();
                    bump();
                } else if (two('-', '-')) {
                    tk.kind = Tok::MinusMinus;
                    tk.text = "--";
                    bump();
                    bump();
                } else if (two('+', '=')) {
                    tk.kind = Tok::PlusEq;
                    tk.text = "+=";
                    bump();
                    bump();
                } else if (two('-', '=')) {
                    tk.kind = Tok::MinusEq;
                    tk.text = "-=";
                    bump();
                    bump();
                } else if (two('*', '=')) {
                    tk.kind = Tok::StarEq;
                    tk.text = "*=";
                    bump();
                    bump();
                } else if (two('/', '=')) {
                    tk.kind = Tok::SlashEq;
                    tk.text = "/=";
                    bump();
                    bump();
                } else if (two('%', '=')) {
                    tk.kind = Tok::PercentEq;
                    tk.text = "%=";
                    bump();
                    bump();
                } else if (two('&', '=')) {
                    tk.kind = Tok::AmpEq;
                    tk.text = "&=";
                    bump();
                    bump();
                } else if (two('|', '=')) {
                    tk.kind = Tok::PipeEq;
                    tk.text = "|=";
                    bump();
                    bump();
                } else if (two('^', '=')) {
                    tk.kind = Tok::CaretEq;
                    tk.text = "^=";
                    bump();
                    bump();
                } else {
                    switch (c) {
                        case '(': tk.kind = Tok::LParen; break;
                        case ')': tk.kind = Tok::RParen; break;
                        case '{': tk.kind = Tok::LBrace; break;
                        case '}': tk.kind = Tok::RBrace; break;
                        case '[': tk.kind = Tok::LBracket; break;
                        case ']': tk.kind = Tok::RBracket; break;
                        case ',': tk.kind = Tok::Comma; break;
                        case ';': tk.kind = Tok::Semicolon; break;
                        case ':': tk.kind = Tok::Colon; break;
                        case '.': tk.kind = Tok::Dot; break;
                        case '+': tk.kind = Tok::Plus; break;
                        case '-': tk.kind = Tok::Minus; break;
                        case '*': tk.kind = Tok::Star; break;
                        case '/': tk.kind = Tok::Slash; break;
                        case '%': tk.kind = Tok::Percent; break;
                        case '=': tk.kind = Tok::Assign; break;
                        case '<': tk.kind = Tok::Lt; break;
                        case '>': tk.kind = Tok::Gt; break;
                        case '!': tk.kind = Tok::Bang; break;
                        case '&': tk.kind = Tok::Amp; break;
                        case '|': tk.kind = Tok::Pipe; break;
                        case '^': tk.kind = Tok::Caret; break;
                        case '~': tk.kind = Tok::Tilde; break;
                        default:
                            error(start,
                                  std::string("无法识别的字符 '") + c +
                                      "'（Lux 标识符只支持 ASCII 字母、数字和下划线）");
                    }
                    tk.text = std::string(1, c);
                    bump();
                }
                toks.push_back(tk);
            } catch (const LexBail&) {
                // 错误已记录；预算耗尽就停止，否则跳过坏字符继续
                if (diags.errors > 100) break;
                if (!eof()) bump();
            }
        }
        Token eof;
        eof.kind = Tok::Eof;
        eof.text = "";
        eof.loc = loc();
        toks.push_back(eof);
        return toks;
    }
};

}  // namespace

std::vector<Token> tokenize(const std::string& src, const std::string& filename,
                            Diags& diags) {
    Lexer lex(src, filename, diags);
    return lex.run();
}

}  // namespace lux
