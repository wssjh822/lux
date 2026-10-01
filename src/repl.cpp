// =============================================================================
//  repl.cpp : luxc repl —— 交互式执行环境（读-求值-打印 循环）
//
//  特性：
//    * 行编辑：左右移动 / 退格 / 删除 / Home / End
//    * 历史：上下方向键浏览，跨会话保存在 <luxHome>/repl_history
//    * TAB 补全：关键字 + 内置函数 + 会话中定义的名字
//    * 正确解析 ANSI 转义序列（方向键等），未知序列安全忽略
//    * 会话状态：import / fn / let / const / 赋值都会累积，表达式立即求值
//    * 多行输入：花括号不平衡时自动续行
//    * 非 TTY（管道）模式下退化为逐行执行，不带行编辑
//
//  特殊命令：.help / .clear / .exit（或 Ctrl-D）
//  只支持 POSIX（termios），Windows 请用 WSL / MSYS2。
// =============================================================================
#include "lux.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace lux {

namespace {

#ifndef LUX_VERSION
#define LUX_VERSION "0.8.0"
#endif

const std::string kReplVersion = std::string("Lux ") + LUX_VERSION + " REPL";

// -----------------------------------------------------------------------------
//  终端原始模式（行编辑需要逐字节读取输入）
// -----------------------------------------------------------------------------

struct TermRaw {
    bool active = false;
    struct termios orig {};
    ~TermRaw() { restore(); }

    bool enter() {
        if (tcgetattr(STDIN_FILENO, &orig) != 0) return false;
        struct termios raw = orig;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
        active = true;
        return true;
    }
    void restore() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            active = false;
        }
    }
};

// 前 nbytes 个字节在终端上的显示宽度（CJK 按 2 列计，与诊断渲染一致）
int displayWidth(const std::string& s, size_t nbytes) {
    int w = 0;
    size_t i = 0;
    while (i < nbytes && i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0) { i += 4; w += 2; }
        else if (c >= 0xE0) { i += 3; w += 2; }
        else if (c >= 0xC0) { i += 2; w += 2; }
        else { i += 1; w += (c == '\t') ? 4 : 1; }
    }
    return w;
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// 读取直到 CSI 终结字节，返回终结字节（例如 'A' / '~'）
char readCsiFinal(int fd) {
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) return '\0';
        if ((c >= '@' && c <= '~')) return c;
        if (c == '\x1b') return '\0';  // 序列异常，放弃
    }
}

// -----------------------------------------------------------------------------
//  TAB 补全
// -----------------------------------------------------------------------------

struct Completions {
    std::vector<std::string> words;

    Completions() { reset(); }

    // 关键字 + 全局内置函数（与 lexer/sema 的关键字表保持同步）
    void reset() {
        words = {
            "fn", "let", "const", "return", "if", "else", "elif", "while",
            "for", "loop", "in", "break", "continue", "repeat", "true",
            "false", "and", "or", "not", "import", "as", "extern", "int",
            "float", "bool", "string", "void", "struct", "nan", "inf",
            "none",
            "println", "print", "len",
            "input", "assert", "exit", "abs", "sqrt", "pow", "floor", "ceil",
            "round", "sin", "cos", "tan", "asin", "acos", "atan", "log",
            "log10", "exp", "min", "max", "atan2", "fmod", "hypot", "trunc",
            "isnan", "isinf", "random", "seed", "now", "monotonic", "sleep",
            "sleep_ms", "system", "env", "setenv", "read", "write", "append",
            "exists", "remove", "rename", "contains", "startswith",
            "endswith", "find", "replace", "trim", "upper", "lower", "substr",
            "format", "split", "chars", "join",
            "push", "pop", "insert", "clear",
            ".help", ".clear", ".exit", ".quit",
        };
    }

    void addWord(const std::string& w) {
        for (const std::string& x : words) {
            if (x == w) return;
        }
        words.push_back(w);
    }

    // 从会话代码里提取用户定义的名字（fn / let / const / struct）
    void harvest(const std::string& code) {
        for (const char* kw : {"fn", "let", "const", "struct"}) {
            size_t pos = 0;
            for (;;) {
                pos = code.find(kw, pos);
                if (pos == std::string::npos) break;
                pos += strlen(kw);
                while (pos < code.size() && code[pos] == ' ') pos++;
                std::string name;
                while (pos < code.size() && isIdentChar(code[pos])) {
                    name += code[pos++];
                }
                if (!name.empty()) addWord(name);
            }
        }
    }
};

// 提取光标前的单词前缀，做最长公共前缀补全；
// 唯一匹配直接补全并追加空格，多个匹配列出候选（listed 置 true）。
void doCompletion(std::string& buf, size_t& cur, const Completions& comp,
                  bool& listed) {
    listed = false;
    size_t start = cur;
    while (start > 0 && isIdentChar(buf[start - 1])) start--;
    std::string prefix = buf.substr(start, cur - start);
    if (prefix.empty()) return;

    std::vector<std::string> matches;
    for (const std::string& w : comp.words) {
        if (w.rfind(prefix, 0) == 0) matches.push_back(w);
    }
    if (matches.empty()) return;
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    if (matches.size() == 1) {
        std::string ins = matches[0].substr(prefix.size());
        buf.insert(cur, ins);
        cur += ins.size();
        if (isIdentChar(ins.back())) {
            buf.insert(cur, " ");
            cur++;
        }
        return;
    }

    // 公共最长前缀可以直接补
    std::string common = matches[0];
    for (const std::string& m : matches) {
        size_t i = 0;
        while (i < common.size() && i < m.size() && common[i] == m[i]) i++;
        common = common.substr(0, i);
    }
    if (common.size() > prefix.size()) {
        std::string ins = common.substr(prefix.size());
        buf.insert(cur, ins);
        cur += ins.size();
        return;
    }

    // 列出候选
    std::fprintf(stdout, "\x1b[1G\x1b[K");
    for (size_t i = 0; i < matches.size(); i++) {
        std::fprintf(stdout, "%s%s", i ? "  " : "", matches[i].c_str());
    }
    std::fprintf(stdout, "\n");
    listed = true;
}

// -----------------------------------------------------------------------------
//  行编辑
// -----------------------------------------------------------------------------

struct History {
    std::vector<std::string> lines;
    std::string path;

    explicit History(const std::string& p) : path(p) {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty()) lines.push_back(l);
        }
    }
    void push(const std::string& l) {
        if (!l.empty() && (lines.empty() || lines.back() != l)) {
            lines.push_back(l);
        }
    }
    ~History() {
        std::string dir = path.substr(0, path.find_last_of('/'));
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return;
        size_t begin = lines.size() > 1000 ? lines.size() - 1000 : 0;
        for (size_t i = begin; i < lines.size(); i++) f << lines[i] << "\n";
    }
};

// 交互式读一行；返回 nullopt 表示 EOF（Ctrl-D）
std::optional<std::string> readLineInteractive(const std::string& prompt,
                                               History& hist,
                                               Completions& comp) {
    std::string buf;
    size_t cur = 0;
    size_t histIdx = hist.lines.size();
    int lastWidth = 0;  // 上一次打印内容的显示宽度（用于回到行首）

    // 用 CSI D 后退代替 \r：pty 默认开启 ONLCR，\r 会被翻译成换行导致刷屏
    auto redraw = [&]() {
        if (lastWidth > 0) {
            std::fprintf(stdout, "\x1b[%dD", lastWidth);
        } else {
            std::fprintf(stdout, "\x1b[1G");
        }
        std::fprintf(stdout, "\x1b[K%s%s", prompt.c_str(), buf.c_str());
        lastWidth = displayWidth(prompt, prompt.size()) +
                    displayWidth(buf, buf.size());
        if (cur < buf.size()) {
            int w = displayWidth(prompt, prompt.size()) +
                    displayWidth(buf, cur);
            std::fprintf(stdout, "\x1b[%dD", lastWidth - w);
        }
        std::fflush(stdout);
    };
    redraw();

    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            std::fprintf(stdout, "\n");
            return std::nullopt;
        }

        if (c == '\x1b') {
            // 转义序列：ESC [ 参数... 终结字节
            char s1;
            if (read(STDIN_FILENO, &s1, 1) != 1) continue;
            if (s1 != '[') continue;  // ESC 后不是 [ ：忽略
            char fin = readCsiFinal(STDIN_FILENO);
            switch (fin) {
                case 'A':  // 上：更早的历史
                    if (histIdx > 0) {
                        histIdx--;
                        buf = hist.lines[histIdx];
                        cur = buf.size();
                        redraw();
                    }
                    break;
                case 'B':  // 下：更新的历史
                    if (histIdx < hist.lines.size()) {
                        histIdx++;
                        buf = histIdx < hist.lines.size()
                                  ? hist.lines[histIdx]
                                  : "";
                        cur = buf.size();
                        redraw();
                    }
                    break;
                case 'C':  // 右
                    if (cur < buf.size()) { cur++; redraw(); }
                    break;
                case 'D':  // 左
                    if (cur > 0) { cur--; redraw(); }
                    break;
                case 'H':  // Home
                    cur = 0;
                    redraw();
                    break;
                case 'F':  // End
                    cur = buf.size();
                    redraw();
                    break;
                case '~':  // 可能是删除键（简化处理：忽略参数直接删除光标后字符）
                    if (cur < buf.size()) {
                        buf.erase(cur, 1);
                        redraw();
                    }
                    break;
                default:
                    break;  // 其余序列安全忽略
            }
            continue;
        }

        if (c == '\t') {
            bool listed = false;
            doCompletion(buf, cur, comp, listed);
            if (listed) lastWidth = 0;  // 候选列表以换行结束，光标回到行首
            redraw();
            continue;
        }
        if (c == '\n' || c == '\r') {
            std::fprintf(stdout, "\n");
            return buf;
        }
        if (c == 0x7F || c == 0x08) {  // 退格
            if (cur > 0) {
                buf.erase(cur - 1, 1);
                cur--;
                redraw();
            }
            continue;
        }
        if (c == 0x04) {  // Ctrl-D
            if (buf.empty()) {
                std::fprintf(stdout, "\n");
                return std::nullopt;
            }
            continue;
        }
        if (c == 0x03) {  // Ctrl-C：清空当前行
            std::fprintf(stdout, "\n");
            buf.clear();
            cur = 0;
            lastWidth = 0;
            redraw();
            continue;
        }
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
            buf.insert(cur, 1, c);
            cur++;
            redraw();
        }
    }
}

// -----------------------------------------------------------------------------
//  多行输入：花括号不平衡时续行
// -----------------------------------------------------------------------------

// 忽略字符串字面量与注释，统计 {} 平衡度
int braceBalance(const std::string& code) {
    int bal = 0;
    size_t i = 0;
    while (i < code.size()) {
        char c = code[i];
        if (c == '"') {  // 字符串
            i++;
            while (i < code.size() && code[i] != '"') {
                if (code[i] == '\\' && i + 1 < code.size()) i++;
                i++;
            }
            i++;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            while (i < code.size() && code[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            i += 2;
            while (i + 1 < code.size() &&
                   !(code[i] == '*' && code[i + 1] == '/')) {
                i++;
            }
            i += 2;
            continue;
        }
        if (c == '{') bal++;
        if (c == '}') bal--;
        i++;
    }
    return bal;
}

// -----------------------------------------------------------------------------
//  编译 + 运行一段完整 Lux 程序（复用正式编译流水线）
// -----------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (a < b && ws(s[a])) a++;
    while (b > a && ws(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool endsWithSemicolon(const std::string& s) {
    std::string t = trim(s);
    return !t.empty() && t.back() == ';';
}

// 返回 0=成功已执行；1=编译失败（诊断已打印）；2=无法执行
int compileAndRun(const std::string& src) {
    static const char* kSrcPath = ".lux_repl.lux";  // 固定在当前目录，相对 import 可用
    if (!writeFile(kSrcPath, src)) {
        std::fprintf(stderr, "repl: 无法写入临时文件 '%s'\n", kSrcPath);
        return 2;
    }

    Diags diags;
    ModuleLoader loader(diags);
    Program* prog = loader.load(kSrcPath);
    bool ok = prog != nullptr && diags.ok() &&
              analyze(prog, diags, kSrcPath, loader.modules);
    if (!ok) {
        diags.flush(loader.sources);
        std::remove(kSrcPath);
        return 1;
    }

    CodegenOptions copt;
    copt.sourceName = kSrcPath;
    copt.emitLineMarks = false;
    std::string cCode = generateC(prog, copt);
    std::remove(kSrcPath);
    if (cCode.empty()) {
        std::fprintf(stderr, "repl: 代码生成失败\n");
        return 2;
    }

    // 中间 C 放临时目录，可执行文件放临时目录（每次求值重建）
    const char* td = std::getenv("TMPDIR");
    std::string dir = (td && *td) ? td : "/tmp";
    std::string cTmpl = dir + "/luxrepl-XXXXXX";
    std::vector<char> cBuf(cTmpl.begin(), cTmpl.end());
    cBuf.push_back('\0');
    int fd = mkstemp(cBuf.data());
    if (fd < 0) return 2;
    close(fd);
    std::string cPath = std::string(cBuf.data()) + ".c";
    if (rename(cBuf.data(), cPath.c_str()) != 0) {
        std::remove(cBuf.data());
        return 2;
    }
    std::string exe = cBuf.data();  // 复用随机名当可执行文件名
    if (!writeFile(cPath, cCode)) {
        std::remove(cPath.c_str());
        return 2;
    }

    std::string cmd = "cc -std=c17 -O1 -Wno-unused-function -o " + exe + " " +
                      cPath + " -lm";
    int rc = std::system(cmd.c_str());
    std::remove(cPath.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "repl: C 编译器执行失败\n");
        std::remove(exe.c_str());
        return 2;
    }
    int rrc = std::system(exe.c_str());
    std::remove(exe.c_str());
    return (rrc == 0) ? 0 : 2;
}

// -----------------------------------------------------------------------------
//  会话：累积 import / fn / 语句，让 let 变量跨行存活
// -----------------------------------------------------------------------------

struct Session {
    std::string imports;   // 顶层 import 行
    std::string topTypes;  // 顶层 struct 声明（0.7）
    std::string topFns;    // 顶层 fn 定义
    std::string bodyStmts; // 在 main 里逐行重放的语句（let / const / 赋值）
};

// 用词法分析给一行输入分类：返回 true 表示应当按语句处理（let / 赋值 / 控制流）
bool looksLikeStatement(const std::string& line) {
    Diags quiet;
    std::vector<Token> toks = tokenize(line, "<repl>", quiet);
    if (toks.size() < 1) return false;
    switch (toks[0].kind) {
        case Tok::KwLet:
        case Tok::KwConst:
        case Tok::KwIf:
        case Tok::KwWhile:
        case Tok::KwFor:
        case Tok::KwLoop:
        case Tok::KwBreak:
        case Tok::KwContinue:
        case Tok::KwReturn:
        case Tok::KwRepeat:
            return true;
        case Tok::Ident: {
            if (toks.size() < 2) return false;
            // 扫描后缀链：'.' ident 与 '[' ... ']'（支持 p.x / a[i][j] / a[i].x）
            size_t i = 1;
            while (i < toks.size()) {
                if (toks[i].kind == Tok::Dot && i + 1 < toks.size() &&
                    toks[i + 1].kind == Tok::Ident) {
                    i += 2;
                } else if (toks[i].kind == Tok::LBracket) {
                    int depth = 1;
                    i++;
                    while (i < toks.size() && depth > 0) {
                        if (toks[i].kind == Tok::LBracket) depth++;
                        if (toks[i].kind == Tok::RBracket) depth--;
                        i++;
                    }
                } else {
                    break;
                }
            }
            if (i < toks.size()) {
                Tok k = toks[i].kind;
                return k == Tok::Assign || k == Tok::PlusEq ||
                       k == Tok::MinusEq || k == Tok::StarEq ||
                       k == Tok::SlashEq || k == Tok::PercentEq ||
                       k == Tok::AmpEq || k == Tok::PipeEq ||
                       k == Tok::CaretEq || k == Tok::ShlEq ||
                       k == Tok::ShrEq || k == Tok::PlusPlus ||
                       k == Tok::MinusMinus;
            }
            // 数组方法（0.5）：push / insert / clear 返回 void，
            // 必须按语句执行（包装进 println 会编译失败）；
            // pop / remove 有返回值，保持表达式形态以打印结果
            if (toks[1].kind == Tok::Dot && toks.size() >= 3) {
                const std::string& m = toks[2].text;
                return m == "push" || m == "insert" || m == "clear";
            }
            return false;
        }
        default:
            return false;
    }
}

// 求值一行输入；返回 false 表示应当退出 REPL
bool evalReplLine(const std::string& raw, Session& s, bool& hadError) {
    std::string line = trim(raw);
    if (line.empty()) return true;
    if (line == ".exit" || line == ".quit") return false;
    if (line == ".help") {
        std::printf(
            "  Lux REPL 使用说明\n"
            "   直接输入表达式会立即求值并打印结果；语句（let / const / 赋值）\n"
            "   会累积到会话里，之后的行可以继续使用；fn 定义支持多行（花括号\n"
            "   不平衡时自动续行）。\n"
            "   方向键上下浏览历史，TAB 补全关键字 / 内置函数 / 已定义的名字，\n"
            "   Ctrl-D 或 .exit 退出，.clear 清空会话。\n"
            "   示例： let x = 40;\n"
            "         x + 2\n");
        return true;
    }
    if (line == ".clear") {
        s = Session{};
        std::printf("  会话已清空\n");
        return true;
    }

    std::string src;
    if (line.rfind("import", 0) == 0 && (line.size() == 6 || line[6] == ' ' ||
                                        line[6] == '"')) {
        // import 必须出现在顶层；REPL 里允许省略分号
        std::string imp = endsWithSemicolon(line) ? line : line + ";";
        src = s.imports + imp + "\n" + s.topTypes + s.topFns + "fn main() { " +
              s.bodyStmts + "}\n";
        int rc = compileAndRun(src);
        if (rc == 0) s.imports += imp + "\n";
        hadError |= (rc != 0);
        return true;
    }

    // struct 声明（0.7）：与 fn 一样属于顶层声明，累积后重放
    if (line.rfind("struct ", 0) == 0) {
        src = s.imports + s.topTypes + line + "\n" + s.topFns +
              "fn main() { " + s.bodyStmts + "}\n";
        int rc = compileAndRun(src);
        if (rc == 0) s.topTypes += line + "\n";
        hadError |= (rc != 0);
        return true;
    }

    if (line.rfind("fn ", 0) == 0 || line == "fn") {
        src = s.imports + s.topTypes + s.topFns + line + "\n" +
              "fn main() { " + s.bodyStmts + "}\n";
        int rc = compileAndRun(src);
        if (rc == 0) s.topFns += line + "\n";
        hadError |= (rc != 0);
        return true;
    }

    // 语句形态（let / const / 赋值 / 控制流）：不打印，直接执行并累积
    if (looksLikeStatement(line)) {
        std::string stmt = endsWithSemicolon(line) ? line : line + ";";
        src = s.imports + s.topTypes + s.topFns + "fn main() { " +
              s.bodyStmts + stmt + " }\n";
        int rc = compileAndRun(src);
        if (rc == 0) s.bodyStmts += stmt + " ";
        hadError |= (rc != 0);
        return true;
    }

    // 表达式：println 包裹后立即求值；失败再按语句兜底
    std::string srcExpr = s.imports + s.topTypes + s.topFns +
                          "fn main() { " + s.bodyStmts + "println(" + line +
                          "); }\n";
    int rc = compileAndRun(srcExpr);
    if (rc == 0) return true;
    if (rc == 2) {
        hadError = true;
        return true;
    }

    std::string stmt = endsWithSemicolon(line) ? line : line + ";";
    std::string srcStmt = s.imports + s.topTypes + s.topFns +
                          "fn main() { " + s.bodyStmts + stmt + " }\n";
    rc = compileAndRun(srcStmt);
    if (rc == 0) s.bodyStmts += stmt + " ";
    hadError |= (rc != 0);
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
//  入口： luxc repl
// -----------------------------------------------------------------------------

int runRepl() {
    Session session;
    Completions comp;
    bool tty = isatty(STDIN_FILENO) == 1;

    std::string histPath = luxHome() + "/repl_history";
    History hist(histPath);

    if (tty) {
        std::printf("%s —— 输入表达式立即求值（.help 帮助 / .exit 退出）\n",
                    kReplVersion.c_str());
        TermRaw raw;
        if (!raw.enter()) {
            std::fprintf(stderr, "repl: 无法切换到终端原始模式\n");
            return 1;
        }

        bool hadError = false;
        std::string buf;
        int balance = 0;
        for (;;) {
            std::string prompt = balance > 0 ? "...> " : "lux> ";
            auto line = readLineInteractive(prompt, hist, comp);
            if (!line) break;

            buf += *line + "\n";
            balance += braceBalance(*line);
            if (balance > 0) continue;  // 续行
            std::string trimmed = trim(buf);
            buf.clear();
            balance = 0;
            if (trimmed.empty()) continue;
            hist.push(trimmed);
            comp.harvest(trimmed);
            if (!evalReplLine(trimmed, session, hadError)) break;
        }
        return hadError ? 1 : 0;
    }

    // 非 TTY（管道）：逐行执行，不带行编辑
    bool hadError = false;
    std::string line;
    std::string buf;
    int balance = 0;
    while (std::getline(std::cin, line)) {
        buf += line + "\n";
        balance += braceBalance(line);
        if (balance > 0) continue;  // 续行
        std::string trimmed = trim(buf);
        buf.clear();
        balance = 0;
        if (trimmed.empty()) continue;
        if (!evalReplLine(trimmed, session, hadError)) break;
    }
    return hadError ? 1 : 0;
}

}  // namespace lux
