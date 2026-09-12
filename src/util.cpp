// =============================================================================
//  util.cpp : 诊断信息渲染、文件读写、字符串转义等通用工具
// =============================================================================
#include "lux.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace lux {

// -----------------------------------------------------------------------------
//  诊断渲染
// -----------------------------------------------------------------------------

namespace {

struct Col {
    static bool enabled;
    static const char* reset() { return enabled ? "\033[0m" : ""; }
    static const char* bold() { return enabled ? "\033[1m" : ""; }
    static const char* red() { return enabled ? "\033[1;31m" : ""; }
    static const char* yellow() { return enabled ? "\033[1;33m" : ""; }
    static const char* cyan() { return enabled ? "\033[36m" : ""; }
    static const char* green() { return enabled ? "\033[32m" : ""; }
    static const char* dim() { return enabled ? "\033[90m" : ""; }
};

bool Col::enabled = false;

// 取出源码的第 n 行（1 起数），不存在时返回空串
std::string lineOf(const std::string& src, int n) {
    if (n < 1) return "";
    int cur = 1;
    size_t i = 0;
    while (i < src.size() && cur < n) {
        if (src[i] == '\n') cur++;
        i++;
    }
    if (cur != n) return "";
    size_t start = i;
    while (i < src.size() && src[i] != '\n') i++;
    size_t end = i;
    if (end > start && src[end - 1] == '\r') end--;
    return src.substr(start, end - start);
}

// 计算前 nbytes 个字节在终端上的显示宽度（CJK 字符按 2 列宽计）
int displayWidth(const std::string& s, size_t nbytes) {
    int w = 0;
    size_t i = 0;
    while (i < nbytes && i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0) {
            i += 4;
            w += 2;
        } else if (c >= 0xE0) {
            i += 3;
            w += 2;
        } else if (c >= 0xC0) {
            i += 2;
            w += 2;
        } else {
            i += 1;
            w += (c == '\t') ? 4 : 1;
        }
    }
    return w;
}

}  // namespace

void setColorEnabled(bool on) { Col::enabled = on; }

void Diags::flush(const SourceMap& sources) {
    for (const Diagnostic& d : items) {
        const bool isErr = d.isError;
        const char* lvl = isErr ? "error" : "warning";
        const char* lvlColor = isErr ? Col::red() : Col::yellow();
        const std::string filename = d.file;

        std::fprintf(stderr, "%s%s[%s]%s: %s\n", lvlColor, lvl,
                     d.code.c_str(), Col::reset(), d.msg.c_str());
        if (filename.empty()) {
            std::fprintf(stderr, "%s --> %d:%d%s\n", Col::cyan(), d.loc.line,
                         d.loc.col, Col::reset());
        } else {
            std::fprintf(stderr, "%s --> %s:%d:%d%s\n", Col::cyan(),
                         filename.c_str(), d.loc.line, d.loc.col, Col::reset());
        }

        // 打印源码上下文（能找到该文件的源码时）
        std::string text;
        auto it = sources.find(filename);
        if (it != sources.end()) text = lineOf(it->second, d.loc.line);
        if (!text.empty()) {
            std::string gutter = std::to_string(d.loc.line);
            std::string pad(gutter.size(), ' ');
            std::fprintf(stderr, "%s%s |%s\n", Col::cyan(), pad.c_str(),
                         Col::reset());
            std::fprintf(stderr, "%s%s | %s%s\n", Col::cyan(), gutter.c_str(),
                         Col::reset(), text.c_str());

            // 列指示符（col 以 1 起数，按终端显示宽度对齐）
            size_t bytes = d.loc.col > 0 ? static_cast<size_t>(d.loc.col - 1) : 0;
            if (bytes > text.size()) bytes = text.size();
            std::string caret(static_cast<size_t>(displayWidth(text, bytes)), ' ');
            caret += "^";
            std::fprintf(stderr, "%s%s | %s%s%s\n", Col::cyan(), pad.c_str(),
                         Col::reset(), Col::green(), caret.c_str());
        }
    }
    if (errors > 0) {
        std::fprintf(stderr, "%s编译失败：共 %d 个错误", Col::red(), errors);
        if (warnings > 0) std::fprintf(stderr, "，%d 个警告", warnings);
        std::fprintf(stderr, "。%s\n", Col::reset());
    } else if (warnings > 0) {
        std::fprintf(stderr, "%s编译完成，有 %d 个警告。%s\n", Col::yellow(),
                     warnings, Col::reset());
    }
}

// -----------------------------------------------------------------------------
//  文件读写
// -----------------------------------------------------------------------------

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return std::nullopt;
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// -----------------------------------------------------------------------------
//  字符串转义
// -----------------------------------------------------------------------------

std::string escapeCString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\v': out += "\\v"; break;
            default:
                // 其它控制字符用 3 位八进制转义（定长，不会与后续字符粘连）
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%03o", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 字节原样保留
                }
        }
    }
    return out;
}

std::string stemOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return base;
    return base.substr(0, dot);
}

std::string basenameOf(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

// -----------------------------------------------------------------------------
//  极简 JSON 读取（够 lux.json 用）
// -----------------------------------------------------------------------------

namespace {

// 把 UTF-32 码点编码为 UTF-8 追加到 out
void appendUtf8(std::string& out, uint32_t cp) {
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

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 在 text[pos] 处解析 \uXXXX（含代理对），返回读取的字符数；失败返回 0
size_t parseJsonUnicode(const std::string& text, size_t pos, std::string& out) {
    if (pos + 4 >= text.size()) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexVal(text[pos + i]);
        if (h < 0) return 0;
        v = v * 16 + static_cast<uint32_t>(h);
    }
    if (v >= 0xD800 && v <= 0xDBFF && pos + 10 < text.size() &&
        text[pos + 4] == '\\' && text[pos + 5] == 'u') {
        uint32_t lo = 0;
        for (int i = 0; i < 4; i++) {
            int h = hexVal(text[pos + 6 + i]);
            if (h < 0) break;
            lo = lo * 16 + static_cast<uint32_t>(h);
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
            appendUtf8(out, v);
            return 10;
        }
    }
    appendUtf8(out, v);
    return 4;
}

}  // namespace

bool jsonStringField(const std::string& text, const std::string& key,
                     std::string& out) {
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos);
    if (pos == std::string::npos) return false;
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    pos++;
    out.clear();
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos++;
            switch (text[pos]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'u': {
                    size_t n = parseJsonUnicode(text, pos + 1, out);
                    if (n) pos += n;  // 循环结尾还会 pos++，这里只补中间的
                    break;
                }
                default: out += text[pos]; break;
            }
        } else {
            out += text[pos];
        }
        pos++;
    }
    return pos < text.size();
}

bool jsonStringArray(const std::string& text, const std::string& key,
                     std::vector<std::string>& out) {
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = text.find('[', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < text.size() && text[pos] != ']') {
        if (text[pos] == '"') {
            size_t end = text.find('"', pos + 1);
            if (end == std::string::npos) return false;
            out.push_back(text.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        } else {
            pos++;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
//  返回路径分析（0.5 新增，Sema 与 Codegen 共用）
// -----------------------------------------------------------------------------

namespace {

// 判断语句子树里是否含有"作用于最外层循环"的 break。
// 遇到嵌套循环不再下钻：内层循环的 break 不影响外层的可达性判断。
bool containsBreakShallow(const Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Break:
            return true;
        case StmtKind::Block:
            for (const Stmt* st : static_cast<const BlockStmt*>(s)->stmts) {
                if (containsBreakShallow(st)) return true;
            }
            return false;
        case StmtKind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            return containsBreakShallow(i->thenBranch) ||
                   containsBreakShallow(i->elseBranch);
        }
        default:
            return false;  // While / For / Loop 内层的 break 不属于本层
    }
}

}  // namespace

// 这条语句执行后是否"必然"离开当前函数：
//   * return
//   * if / else 两个分支都必然返回
//   * while (true) 或 loop，且循环体不含直接的 break
bool stmtAlwaysReturns(const Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Return:
            return true;
        case StmtKind::Block: {
            const auto& v = static_cast<const BlockStmt*>(s)->stmts;
            return !v.empty() && stmtAlwaysReturns(v.back());
        }
        case StmtKind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            return i->elseBranch && stmtAlwaysReturns(i->thenBranch) &&
                   stmtAlwaysReturns(i->elseBranch);
        }
        case StmtKind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            const Expr* c = w->cond;
            if (c && c->kind == ExprKind::BoolLit &&
                static_cast<const BoolLitExpr*>(c)->value) {
                return !containsBreakShallow(w->body);
            }
            return false;
        }
        case StmtKind::Loop:
            return !containsBreakShallow(static_cast<const LoopStmt*>(s)->body);
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
//  包管理目录
// -----------------------------------------------------------------------------

std::string luxHome() {
    const char* h = std::getenv("LUX_HOME");  // 测试与重定位用
    if (h && *h) return h;
    h = std::getenv("HOME");
    if (h && *h) return std::string(h) + "/.lux";
    return ".lux";
}

std::string packagesDir() { return luxHome() + "/packages"; }

}  // namespace lux
