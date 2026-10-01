// =============================================================================
//  json.cpp : 极简 JSON 解析 / 序列化
//
//  包注册表（registry）与 lux.json 都用 JSON 描述，之前 util.cpp 里那两个
//  “找 key 再找引号”的函数只能读单层字符串，包索引这种嵌套结构（数组 +
//  对象 + 依赖映射）根本读不了。这里补一个完整但很小的递归下降解析器：
//
//    JsonValue v;
//    std::string err;
//    if (!jsonParse(text, v, err)) { ... }
//    const JsonValue* pkgs = v.find("packages");
//
//  只支持 JSON 标准子集：null / bool / number(double) / string / array /
//  object。不处理 \u 代理对的完整语义以外的花活，但转义、注释之外的
//  合法 JSON 都能解析。序列化只用于写安装元数据，输出稳定、可读。
// =============================================================================
#include "lux.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lux {

namespace {

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit JsonParser(const std::string& text) : s(text) {}

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                i++;
            } else {
                break;
            }
        }
    }

    bool fail(const std::string& m) {
        if (err.empty()) {
            // 把字节偏移换算成行:列，方便定位坏掉的注册表响应
            int line = 1, col = 1;
            for (size_t k = 0; k < i && k < s.size(); k++) {
                if (s[k] == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "（第 %d 行第 %d 列）", line, col);
            err = m + buf;
        }
        return false;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > 64) return fail("JSON 嵌套过深");
        skipWs();
        if (i >= s.size()) return fail("JSON 意外结束");
        char c = s[i];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                out.kind = JsonValue::Kind::Str;
                return parseString(out.str);
            }
            case 't':
            case 'f':
                return parseBool(out);
            case 'n':
                return parseNull(out);
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Obj;
        i++;  // '{'
        skipWs();
        if (i < s.size() && s[i] == '}') {
            i++;
            return true;
        }
        for (;;) {
            skipWs();
            if (i >= s.size() || s[i] != '"')
                return fail("JSON 对象缺少键名");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':')
                return fail("JSON 对象键名后缺少 ':'");
            i++;
            JsonValue val;
            if (!parseValue(val, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            return fail("JSON 对象缺少 ',' 或 '}'");
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Arr;
        i++;  // '['
        skipWs();
        if (i < s.size() && s[i] == ']') {
            i++;
            return true;
        }
        for (;;) {
            JsonValue val;
            if (!parseValue(val, depth + 1)) return false;
            out.arr.push_back(std::move(val));
            skipWs();
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            return fail("JSON 数组缺少 ',' 或 ']'");
        }
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("JSON 字符串缺少 '\"'");
        i++;
        out.clear();
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') {
                i++;
                return true;
            }
            if (c == '\\') {
                i++;
                if (i >= s.size()) return fail("JSON 转义不完整");
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 高代理：必须紧跟 \uDC00-\uDFFF 才算一对
                            if (i + 1 < s.size() && s[i] == '\\' &&
                                s[i + 1] == 'u') {
                                size_t save = i;
                                i += 2;
                                unsigned lo = 0;
                                if (!hex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (lo - 0xDC00);
                                } else {
                                    i = save;     // 不是低代理，回退重解析
                                    cp = 0xFFFD;  // 孤立高代理 → 替换字符
                                }
                            } else {
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            // 孤立低代理：绝不产出非法 UTF-8（CESU-8），
                            // 按 Unicode 惯例换成 U+FFFD。
                            cp = 0xFFFD;
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        return fail("JSON 非法转义 '\\" + std::string(1, e) + "'");
                }
            } else {
                out += static_cast<char>(c);
                i++;
            }
        }
        return fail("JSON 字符串未闭合");
    }

    bool hex4(unsigned& out) {
        if (i + 4 > s.size()) return fail("JSON \\u 转义长度不足");
        out = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("JSON \\u 转义含非十六进制字符");
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
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

    bool parseBool(JsonValue& out) {
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            out.kind = JsonValue::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            out.kind = JsonValue::Kind::Bool;
            out.boolean = false;
            return true;
        }
        return fail("JSON 非法字面量（应为 true/false）");
    }

    bool parseNull(JsonValue& out) {
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        return fail("JSON 非法字面量（应为 null）");
    }

    bool parseNumber(JsonValue& out) {
        size_t start = i;
        // 宽松：接受前导 '+'（RFC 8259 不允许）。注册表 / lux.json 由标准
        // 编码器生成，不会是 '+'；这里容忍手写配置，解析值不受影响。
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        bool any = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            i++;
            any = true;
        }
        if (i < s.size() && s[i] == '.') {
            i++;
            while (i < s.size() &&
                   std::isdigit(static_cast<unsigned char>(s[i]))) {
                i++;
                any = true;
            }
        }
        if (any && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
            while (i < s.size() &&
                   std::isdigit(static_cast<unsigned char>(s[i])))
                i++;
        }
        if (!any) {
            i = start;
            return fail("JSON 期望一个值");
        }
        out.kind = JsonValue::Kind::Num;
        out.number = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }
};

void dumpTo(const JsonValue& v, std::string& out, int indent) {
    auto pad = [&](int n) {
        for (int k = 0; k < n; k++) out += "  ";
    };
    switch (v.kind) {
        case JsonValue::Kind::Null: out += "null"; break;
        case JsonValue::Kind::Bool: out += v.boolean ? "true" : "false"; break;
        case JsonValue::Kind::Num: {
            char buf[64];
            // 先把 double 判成交整数前做范围检查：直接把 >= 2^63 或 < -2^63
            // 的 double 强转 long long 是 UB（0.9.2 修）。2^63 与 -2^63 都能
            // 被 double 精确表示，所以用半开区间 [-(2^63), 2^63) 判定。
            double d = v.number;
            bool isInt = std::isfinite(d) &&
                         d >= -9223372036854775808.0 &&
                         d < 9223372036854775808.0 &&
                         d == std::floor(d);
            if (isInt)
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(d));
            else
                std::snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
            break;
        }
        case JsonValue::Kind::Str:
            out += "\"";
            out += jsonEscape(v.str);
            out += "\"";
            break;
        case JsonValue::Kind::Arr: {
            if (v.arr.empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (size_t k = 0; k < v.arr.size(); k++) {
                pad(indent + 1);
                dumpTo(v.arr[k], out, indent + 1);
                if (k + 1 < v.arr.size()) out += ",";
                out += "\n";
            }
            pad(indent);
            out += "]";
            break;
        }
        case JsonValue::Kind::Obj: {
            if (v.obj.empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            for (size_t k = 0; k < v.obj.size(); k++) {
                pad(indent + 1);
                out += "\"";
                out += jsonEscape(v.obj[k].first);
                out += "\": ";
                dumpTo(v.obj[k].second, out, indent + 1);
                if (k + 1 < v.obj.size()) out += ",";
                out += "\n";
            }
            pad(indent);
            out += "}";
            break;
        }
    }
}

}  // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
    if (kind != Kind::Obj) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string JsonValue::asString(const std::string& def) const {
    return kind == Kind::Str ? str : def;
}

double JsonValue::asNumber(double def) const {
    return kind == Kind::Num ? number : def;
}

bool JsonValue::asBool(bool def) const {
    return kind == Kind::Bool ? boolean : def;
}

const JsonValue* JsonValue::at(size_t idx) const {
    if (kind != Kind::Arr || idx >= arr.size()) return nullptr;
    return &arr[idx];
}

bool jsonParse(const std::string& text, JsonValue& out, std::string& err) {
    JsonParser p(text);
    if (!p.parseValue(out, 0)) {
        err = p.err.empty() ? "JSON 解析失败" : p.err;
        return false;
    }
    p.skipWs();
    if (p.i != text.size()) {
        err = "JSON 结尾有多余内容";
        return false;
    }
    err.clear();
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    char buf[8];
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jsonDump(const JsonValue& v) {
    std::string out;
    dumpTo(v, out, 0);
    out += "\n";
    return out;
}

}  // namespace lux
