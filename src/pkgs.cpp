// =============================================================================
//  pkgs.cpp : 包管理 —— 本地安装 + 在线注册表（0.9）
//
//  已安装的包统一放在 <luxHome>/packages/ 下（默认 ~/.lux/packages/，
//  可用环境变量 LUX_HOME 重定位）：
//
//    luxc add <包目录|.lux 文件>         本地安装（拷贝）
//    luxc install <名字[@版本] | URL>    从注册表 / 网络下载安装
//    luxc list / search / info / update / upgrade / delete
//    luxc publish <包目录>               发布到 FTP 注册表
//
//  注册表协议（默认 https://lux.xfes.top/lux/lux.php）：
//    GET ?action=index               -> {"packages":[ {name,version,url,...}, ... ]}
//    GET ?action=info&name=x         -> 单包全部版本（可选，客户端可只用 index）
//    GET ?action=download&name=&version= -> 直接回包文件（url 缺省时的兜底）
//  只要外部链接返回同样结构的 JSON，就能当注册表用（$LUX_REGISTRY / config）。
//
//  网络下载统一走 curl（http/https/ftp 都支持），归档只认 .tar.gz / .tgz /
//  .lux；安装是「先解到临时目录 -> 校验 -> 再原子 rename」。
// =============================================================================
#include "lux.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace lux {

namespace {

// 展开成绝对路径（用于记录本地安装来源）
std::string normalizeAbs(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return std::string(buf);
    return p;
}

// -----------------------------------------------------------------------------
//  基础文件操作（沿用 0.7 的实现）
// -----------------------------------------------------------------------------

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool mkdirP(const std::string& path) {
    if (path.empty()) return true;
    size_t pos = 0;
    for (;;) {
        size_t slash = path.find('/', pos);
        std::string cur = path.substr(0, slash);
        if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        if (slash == std::string::npos) return true;
        pos = slash + 1;
    }
}

bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

// 递归拷贝目录树；符号链接一律跳过（防止链接成环导致无限递归）
bool copyTree(const std::string& src, const std::string& dst) {
    if (!mkdirP(dst)) return false;
    DIR* d = opendir(src.c_str());
    if (!d) return false;
    bool ok = true;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string s = src + "/" + e->d_name;
        std::string t = dst + "/" + e->d_name;
        struct stat st;
        if (lstat(s.c_str(), &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISLNK(st.st_mode)) continue;  // 跳过符号链接
        if (S_ISDIR(st.st_mode)) {
            if (!copyTree(s, t)) ok = false;
        } else if (!copyFile(s, t)) {
            ok = false;
        }
    }
    closedir(d);
    return ok;
}

// 递归删除目录树（也兼容普通文件）
bool removeTree(const std::string& path) {
    if (!isDir(path)) return std::remove(path.c_str()) == 0;
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool ok = true;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        if (!removeTree(path + "/" + e->d_name)) ok = false;
    }
    closedir(d);
    if (rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

bool hasSuffix(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool hasPrefix(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

// 包名 / 版本号只能是安全的路径分量
bool validPkgName(const std::string& name, std::string& err) {
    if (name.empty() || name == "." || name == "..") {
        err = "无法确定包名";
        return false;
    }
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        err = "包名 '" + name + "' 不合法（不能包含路径分隔符）";
        return false;
    }
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              c == '-' || c == '.')) {
            err = "包名 '" + name + "' 含非法字符（只允许字母、数字、_ - .）";
            return false;
        }
    }
    return true;
}

bool validVersion(const std::string& v) {
    if (v.empty() || v.size() > 64) return false;
    for (char c : v) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '-' || c == '_' || c == '+'))
            return false;
    }
    return true;
}

int countLuxFiles(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (hasSuffix(e->d_name, ".lux")) n++;
    }
    closedir(d);
    return n;
}

std::string firstLuxFile(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string found;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (hasSuffix(e->d_name, ".lux")) {
            found = e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

// 跑一条命令并捕获合并后的 stdout+stderr；返回退出码
int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* f = popen((cmd + " 2>&1").c_str(), "r");
    if (!f) return -1;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    int rc = pclose(f);
    if (rc == -1) return -1;
    return (rc >> 8) & 0xFF;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// -----------------------------------------------------------------------------
//  配置（<luxHome>/config.json）
// -----------------------------------------------------------------------------

const char* kDefaultRegistry = "https://lux.xfes.top/lux/lux.php";
const char* kDefaultFtpHost = "lux.xfes.top";
// FTP 凭据不内置（0.9.2 起）：必须由 LUX_FTP_USER / LUX_FTP_PASS 或
// ~/.lux/config.json 提供。正式发布请用账号 API（luxc login + luxc publish）。
const char* kDefaultFtpUser = "";
const char* kDefaultFtpPass = "";
const char* kDefaultFtpRoot = "/lux";

std::string configPath() { return luxHome() + "/config.json"; }

JsonValue loadConfig() {
    JsonValue v;
    auto text = readFile(configPath());
    if (text) {
        std::string err;
        JsonValue parsed;
        if (jsonParse(*text, parsed, err) && parsed.kind == JsonValue::Kind::Obj)
            return parsed;
    }
    v.kind = JsonValue::Kind::Obj;
    return v;
}

// 配置优先级：环境变量 > config.json > 内置默认
std::string configString(const JsonValue& cfg, const char* envName,
                         const char* key, const char* def) {
    const char* e = std::getenv(envName);
    if (e && *e) return e;
    const JsonValue* v = cfg.find(key);
    if (v && v->kind == JsonValue::Kind::Str && !v->str.empty()) return v->str;
    return def;
}

void configSet(const std::string& key, const std::string& value) {
    JsonValue cfg = loadConfig();
    bool replaced = false;
    for (auto& kv : cfg.obj) {
        if (kv.first == key) {
            kv.second.kind = JsonValue::Kind::Str;
            kv.second.str = value;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        JsonValue v;
        v.kind = JsonValue::Kind::Str;
        v.str = value;
        cfg.obj.emplace_back(key, std::move(v));
    }
    mkdirP(luxHome());
    writeFile(configPath(), jsonDump(cfg));
}

// -----------------------------------------------------------------------------
//  网络
// -----------------------------------------------------------------------------

// 给 ftp:// 地址补上默认用户名密码（已有 userinfo 就不动）
std::string withFtpCreds(std::string url) {
    if (!hasPrefix(url, "ftp://")) return url;
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest
                                                       : rest.substr(0, slash);
    if (authority.find('@') != std::string::npos) return url;  // 已带凭据
    JsonValue cfg = loadConfig();
    std::string user = configString(cfg, "LUX_FTP_USER", "ftpUser",
                                    kDefaultFtpUser);
    std::string pass = configString(cfg, "LUX_FTP_PASS", "ftpPass",
                                    kDefaultFtpPass);
    if (user.empty()) return url;  // 没配凭据就不注入，交给 curl 报错
    return "ftp://" + user + ":" + pass + "@" + rest;
}

// 用 curl 下载到文件；quiet=false 时把 curl 错误透传给用户
bool fetchToFile(const std::string& rawUrl, const std::string& path,
                 bool quiet, std::string& err) {
    std::string url = withFtpCreds(rawUrl);
    std::string cmd = "curl -fsSL --connect-timeout 20 --retry 2 -o " +
                      shellQuote(path) + " " + shellQuote(url);
    std::string out;
    int rc = runCapture(cmd, out);
    if (rc != 0) {
        if (!quiet) err = "下载失败（" + rawUrl + "）：" + trim(out);
        else err = "下载失败（" + rawUrl + "）";
        return false;
    }
    return true;
}

bool fetchToString(const std::string& rawUrl, bool quiet, std::string& out,
                   std::string& err) {
    std::string url = withFtpCreds(rawUrl);
    std::string cmd = "curl -fsSL --connect-timeout 20 --retry 2 " +
                      shellQuote(url);
    int rc = runCapture(cmd, out);
    if (rc != 0) {
        if (!quiet) err = "下载失败（" + rawUrl + "）：" + trim(out);
        else err = "下载失败（" + rawUrl + "）";
        return false;
    }
    return true;
}

// ---- 账号 / JSON API（lux.php）----

// 当前令牌：环境变量 > ~/.lux/config.json
std::string authTokenCfg() {
    const char* e = std::getenv("LUX_TOKEN");
    if (e && *e) return e;
    JsonValue cfg = loadConfig();
    const JsonValue* v = cfg.find("token");
    return (v && v->kind == JsonValue::Kind::Str) ? v->str : "";
}

std::string authUserCfg() {
    JsonValue cfg = loadConfig();
    const JsonValue* v = cfg.find("tokenUser");
    return (v && v->kind == JsonValue::Kind::Str) ? v->str : "";
}

void configRemove(const std::string& key) {
    JsonValue cfg = loadConfig();
    cfg.obj.erase(std::remove_if(cfg.obj.begin(), cfg.obj.end(),
                                 [&](const std::pair<std::string, JsonValue>& kv) {
                                     return kv.first == key;
                                 }),
                  cfg.obj.end());
    mkdirP(luxHome());
    writeFile(configPath(), jsonDump(cfg));
}

// curl --form-string 发一条 POST（可带一个文件字段）；token 走 Bearer 头
bool httpPostForm(const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& fields,
                  const std::string& fileField, const std::string& filePath,
                  const std::string& token, std::string& out, std::string& err) {
    std::string cmd = "curl -fsS --connect-timeout 30 -X POST";
    if (!token.empty())
        cmd += " -H " + shellQuote("Authorization: Bearer " + token);
    for (const auto& kv : fields)
        cmd += " --form-string " + shellQuote(kv.first + "=" + kv.second);
    if (!fileField.empty())
        cmd += " -F " + shellQuote(fileField + "=@" + filePath);
    cmd += " " + shellQuote(url);
    out.clear();
    int rc = runCapture(cmd, out);
    if (rc != 0) {
        err = "请求失败（" + url + "）：" + trim(out);
        return false;
    }
    return true;
}

// 解析服务器 JSON 响应；有 "error" 字段视为失败
bool apiResult(const std::string& out, JsonValue& resp, std::string& err) {
    std::string jerr;
    if (!jsonParse(out, resp, jerr)) {
        err = "服务器返回无法解析的响应：" + trim(out);
        return false;
    }
    if (const JsonValue* e = resp.find("error")) {
        err = e->asString();
        return false;
    }
    return true;
}

// 把相对 URL 解析成绝对 URL（基于 index 的地址）
std::string resolveUrl(const std::string& base, const std::string& rel) {
    if (rel.empty()) return "";
    if (rel.find("://") != std::string::npos) return rel;
    if (!rel.empty() && rel[0] == '/') {
        size_t schemeEnd = base.find("://");
        if (schemeEnd == std::string::npos) return rel;
        size_t pathStart = base.find('/', schemeEnd + 3);
        if (pathStart == std::string::npos) return base + rel;
        return base.substr(0, pathStart) + rel;
    }
    std::string b = base;
    size_t q = b.find_first_of("?#");
    if (q != std::string::npos) b = b.substr(0, q);
    size_t slash = b.find_last_of('/');
    std::string dir = slash == std::string::npos ? b : b.substr(0, slash + 1);
    std::string r = rel;
    while (hasPrefix(r, "./")) r = r.substr(2);
    return dir + r;
}

// 注册表 API / 静态索引地址拼接
std::string apiUrl(const std::string& base, const std::string& action,
                   const std::vector<std::pair<std::string, std::string>>& q = {}) {
    if (hasSuffix(base, ".json")) return base;  // 静态索引文件
    std::string sep = base.find('?') == std::string::npos ? "?" : "&";
    std::string url = base + sep + "action=" + action;
    for (const auto& kv : q) url += "&" + kv.first + "=" + kv.second;
    return url;
}

// -----------------------------------------------------------------------------
//  归档 / 校验
// -----------------------------------------------------------------------------

// 校验 tar 里没有绝对路径 / ".."（防目录穿越）
bool tarIsSafe(const std::string& archive, std::string& err) {
    std::string out;
    int rc = runCapture("tar -tzf " + shellQuote(archive), out);
    if (rc != 0) {
        err = "无法读取归档：" + trim(out);
        return false;
    }
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t nl = out.find('\n', pos);
        std::string entry = nl == std::string::npos ? out.substr(pos)
                                                    : out.substr(pos, nl - pos);
        pos = nl == std::string::npos ? out.size() + 1 : nl + 1;
        std::string name = entry;
        while (hasPrefix(name, "./")) name = name.substr(2);
        if (name.empty()) continue;
        if (name[0] == '/') {
            err = "归档包含绝对路径 '" + entry + "'，已拒绝安装";
            return false;
        }
        // 检查每一段是否有 ".."
        size_t p = 0;
        while (p <= name.size()) {
            size_t s = name.find('/', p);
            std::string seg = s == std::string::npos ? name.substr(p)
                                                     : name.substr(p, s - p);
            if (seg == "..") {
                err = "归档包含可疑路径 '" + entry + "'，已拒绝安装";
                return false;
            }
            if (s == std::string::npos) break;
            p = s + 1;
        }
    }
    return true;
}

bool extractArchive(const std::string& archive, const std::string& dest,
                    std::string& err) {
    if (!mkdirP(dest)) {
        err = "无法创建解压目录 '" + dest + "'";
        return false;
    }
    if (!tarIsSafe(archive, err)) return false;
    std::string out;
    int rc = runCapture("tar -xzf " + shellQuote(archive) + " -C " +
                            shellQuote(dest),
                        out);
    if (rc != 0) {
        err = "解压失败：" + trim(out);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  语义化版本
// -----------------------------------------------------------------------------

std::vector<long long> versionParts(const std::string& v) {
    std::vector<long long> out;
    std::string cur;
    for (size_t i = 0; i <= v.size(); i++) {
        char c = i < v.size() ? v[i] : '.';
        if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        } else {
            if (!cur.empty()) {
                out.push_back(std::strtoll(cur.c_str(), nullptr, 10));
                cur.clear();
            }
            if (c == '-' || c == '+') break;  // 预发布 / 构建元数据只比数字前缀
        }
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

int compareVersion(const std::string& a, const std::string& b) {
    std::vector<long long> x = versionParts(a), y = versionParts(b);
    size_t n = std::max(x.size(), y.size());
    for (size_t i = 0; i < n; i++) {
        long long l = i < x.size() ? x[i] : 0;
        long long r = i < y.size() ? y[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

// 支持的约束：空 / "*" / "1.2.3" / "=1.2.3" / ">=1.2.3" / ">..." / "<=..." /
// "<..." / "^1.2.3" / "~1.2.3"
bool versionSatisfies(const std::string& v, const std::string& c) {
    std::string con = trim(c);
    if (con.empty() || con == "*") return true;
    if (!con.empty() && (con[0] == '^' || con[0] == '~')) {
        char op = con[0];
        std::string base = con.substr(1);
        if (compareVersion(v, base) < 0) return false;
        std::vector<long long> p = versionParts(base);
        std::string upper;
        if (op == '^') {
            upper = std::to_string(p[0] + 1) + ".0.0";
        } else {  // ~1.2.3 -> < 1.3.0
            upper = std::to_string(p[0]) + "." + std::to_string(p[1] + 1) + ".0";
        }
        return compareVersion(v, upper) < 0;
    }
    const char* ops[] = {">=", "<=", "==", "!=", ">", "<", "="};
    for (const char* op : ops) {
        size_t len = std::strlen(op);
        if (con.compare(0, len, op) == 0) {
            std::string base = trim(con.substr(len));
            int cmp = compareVersion(v, base);
            if (!std::strcmp(op, ">=")) return cmp >= 0;
            if (!std::strcmp(op, "<=")) return cmp <= 0;
            if (!std::strcmp(op, ">")) return cmp > 0;
            if (!std::strcmp(op, "<")) return cmp < 0;
            if (!std::strcmp(op, "!=")) return cmp != 0;
            return cmp == 0;  // = / ==
        }
    }
    return compareVersion(v, con) == 0;  // 裸版本号 = 精确匹配
}

// -----------------------------------------------------------------------------
//  注册表索引
// -----------------------------------------------------------------------------

struct IndexEntry {
    std::string name, version, summary, description, main, license, homepage;
    std::string luxReq;
    std::vector<std::string> files, tags, authors;
    std::vector<std::pair<std::string, std::string>> deps;
    std::string url, sha256;
    long long size = 0;
};

std::string cachePath() { return luxHome() + "/cache/index.json"; }

// 发布 / 删除后作废本地索引缓存，下次 info / search / install 会重新拉取
void invalidateIndexCache() { std::remove(cachePath().c_str()); }

// 把一段 index JSON 解析成条目表；base 用于把相对 url 绝对化
bool parseIndex(const std::string& text, const std::string& base,
                std::vector<IndexEntry>& out, std::string& err) {
    JsonValue root;
    if (!jsonParse(text, root, err)) return false;
    const JsonValue* pkgs = nullptr;
    if (root.kind == JsonValue::Kind::Arr) {
        pkgs = &root;
    } else if (root.kind == JsonValue::Kind::Obj) {
        pkgs = root.find("packages");
        if (!pkgs) {
            err = "注册表 JSON 里没有 \"packages\" 数组";
            return false;
        }
    } else {
        err = "注册表 JSON 顶层必须是对象或数组";
        return false;
    }
    if (pkgs->kind != JsonValue::Kind::Arr) {
        err = "注册表的 \"packages\" 不是数组";
        return false;
    }
    out.clear();
    for (const JsonValue& p : pkgs->arr) {
        if (p.kind != JsonValue::Kind::Obj) continue;
        IndexEntry e;
        e.name = p.find("name") ? p.find("name")->asString() : "";
        if (e.name.empty()) continue;
        e.version = p.find("version") ? p.find("version")->asString("0.0.0")
                                      : "0.0.0";
        e.summary = p.find("summary") ? p.find("summary")->asString() : "";
        if (e.summary.empty() && p.find("description"))
            e.summary = p.find("description")->asString();
        e.description = p.find("description") ? p.find("description")->asString()
                                              : "";
        e.main = p.find("main") ? p.find("main")->asString() : "";
        e.license = p.find("license") ? p.find("license")->asString() : "";
        e.homepage = p.find("homepage") ? p.find("homepage")->asString() : "";
        e.luxReq = p.find("lux") ? p.find("lux")->asString() : "";
        if (const JsonValue* f = p.find("files"))
            if (f->kind == JsonValue::Kind::Arr)
                for (const JsonValue& x : f->arr)
                    if (x.kind == JsonValue::Kind::Str) e.files.push_back(x.str);
        if (const JsonValue* t = p.find("tags"))
            if (t->kind == JsonValue::Kind::Arr)
                for (const JsonValue& x : t->arr)
                    if (x.kind == JsonValue::Kind::Str) e.tags.push_back(x.str);
        if (const JsonValue* a = p.find("authors"))
            if (a->kind == JsonValue::Kind::Arr)
                for (const JsonValue& x : a->arr)
                    if (x.kind == JsonValue::Kind::Str) e.authors.push_back(x.str);
        if (const JsonValue* d = p.find("deps"))
            if (d->kind == JsonValue::Kind::Obj)
                for (const auto& kv : d->obj)
                    e.deps.emplace_back(kv.first, kv.second.asString("*"));
        std::string rawUrl = p.find("url") ? p.find("url")->asString() : "";
        if (rawUrl.empty() && p.find("tarball"))
            rawUrl = p.find("tarball")->asString();
        e.url = resolveUrl(base, rawUrl);
        if (e.url.empty()) e.url = apiUrl(base, "download",
                                          {{"name", e.name},
                                           {"version", e.version}});
        e.sha256 = p.find("sha256") ? p.find("sha256")->asString() : "";
        if (p.find("size")) e.size = (long long)p.find("size")->asNumber();
        out.push_back(std::move(e));
    }
    return true;
}

// 取索引：优先本地缓存，缓存不存在或 refresh=true 时联网
bool loadIndex(bool refresh, std::vector<IndexEntry>& out, std::string& err) {
    std::string base = registryUrl();
    std::string text;
    if (!refresh) {
        auto cached = readFile(cachePath());
        if (cached && !cached->empty()) text = *cached;
    }
    if (text.empty()) {
        std::string url = apiUrl(base, "index");
        if (!fetchToString(url, false, text, err)) {
            // 联网失败时退回旧缓存（离线可用）
            auto cached = readFile(cachePath());
            if (cached && !cached->empty()) {
                text = *cached;
            } else {
                return false;
            }
        } else {
            mkdirP(luxHome() + "/cache");
            writeFile(cachePath(), text);
        }
    }
    return parseIndex(text, base, out, err);
}

// -----------------------------------------------------------------------------
//  包元数据（安装记录）
// -----------------------------------------------------------------------------

std::string metaPath(const std::string& dir) { return dir + "/.lux-meta.json"; }

PkgInfo readInstalled(const std::string& name, const std::string& dir) {
    PkgInfo info;
    info.name = name;
    info.files = countLuxFiles(dir);
    auto conf = readFile(dir + "/lux.json");
    if (conf) {
        JsonValue j;
        std::string err;
        if (jsonParse(*conf, j, err)) {
            if (const JsonValue* m = j.find("main")) info.main = m->asString();
            if (info.version.empty())
                if (const JsonValue* v = j.find("version"))
                    info.version = v->asString();
            if (const JsonValue* d = j.find("deps"))
                if (d->kind == JsonValue::Kind::Obj)
                    for (const auto& kv : d->obj)
                        info.deps.push_back(kv.first + "@" +
                                            kv.second.asString("*"));
        } else {
            // 老版本 lux.json 是单层：退回极简解析
            jsonStringField(*conf, "main", info.main);
        }
    }
    auto meta = readFile(metaPath(dir));
    if (meta) {
        JsonValue j;
        std::string err;
        if (jsonParse(*meta, j, err)) {
            if (const JsonValue* v = j.find("version"))
                if (info.version.empty()) info.version = v->asString();
            if (const JsonValue* v = j.find("source"))
                info.source = v->asString();
            if (const JsonValue* v = j.find("origin"))
                info.origin = v->asString();
        } else {
            jsonStringField(*meta, "version", info.version);
            jsonStringField(*meta, "source", info.source);
            jsonStringField(*meta, "origin", info.origin);
        }
    }
    if (info.source.empty()) info.source = "local";
    return info;
}

void writeMeta(const std::string& dir, const std::string& version,
               const std::string& source, const std::string& origin,
               const std::vector<std::pair<std::string, std::string>>& deps) {
    JsonValue root;
    root.kind = JsonValue::Kind::Obj;
    auto put = [&](const char* k, const std::string& v) {
        JsonValue j;
        j.kind = JsonValue::Kind::Str;
        j.str = v;
        root.obj.emplace_back(k, std::move(j));
    };
    put("name", basenameOf(dir));
    put("version", version);
    put("source", source);
    put("origin", origin);
    if (!deps.empty()) {
        JsonValue d;
        d.kind = JsonValue::Kind::Obj;
        for (const auto& kv : deps) {
            JsonValue j;
            j.kind = JsonValue::Kind::Str;
            j.str = kv.second;
            d.obj.emplace_back(kv.first, std::move(j));
        }
        root.obj.emplace_back("deps", std::move(d));
    }
    writeFile(metaPath(dir), jsonDump(root));
}

// 在目录里找 lux.json；没有就让调用方合成
bool readPkgJson(const std::string& dir, JsonValue& out) {
    auto text = readFile(dir + "/lux.json");
    if (!text) return false;
    std::string err;
    return jsonParse(*text, out, err);
}

// 归档解压后定位包根目录：优先含 lux.json 的那一层
std::string findPackageRoot(const std::string& extractDir) {
    if (isFile(extractDir + "/lux.json")) return extractDir;
    DIR* d = opendir(extractDir.c_str());
    if (!d) return extractDir;
    std::string onlyDir;
    int dirCount = 0, fileCount = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string p = extractDir + "/" + e->d_name;
        if (isDir(p)) {
            dirCount++;
            onlyDir = p;
        } else {
            fileCount++;
        }
    }
    closedir(d);
    if (fileCount == 0 && dirCount == 1) {
        std::string nested = findPackageRoot(onlyDir);
        return nested;
    }
    return extractDir;
}

// 确保包目录有 lux.json；返回包名。保留原文件里的 description / deps 等字段，
// 只补齐 / 规范 name、version、main、files。
std::string ensureLuxJson(const std::string& dir, const std::string& fallbackName,
                          const std::string& version,
                          const std::vector<std::string>& files) {
    JsonValue conf;
    if (!readPkgJson(dir, conf) || conf.kind != JsonValue::Kind::Obj) {
        conf.kind = JsonValue::Kind::Obj;
    }
    const JsonValue* nameV = conf.find("name");
    std::string name = nameV ? nameV->asString() : "";
    if (name.empty()) name = fallbackName;
    const JsonValue* mainV = conf.find("main");
    std::string main = mainV ? mainV->asString() : "";
    if (main.empty() && !files.empty()) main = files[0];
    if (main.empty()) main = firstLuxFile(dir);

    auto setStr = [&](const char* k, const std::string& val) {
        if (val.empty()) return;
        for (auto& kv : conf.obj) {
            if (kv.first == k) {
                kv.second.kind = JsonValue::Kind::Str;
                kv.second.str = val;
                return;
            }
        }
        JsonValue j;
        j.kind = JsonValue::Kind::Str;
        j.str = val;
        conf.obj.emplace_back(k, std::move(j));
    };
    setStr("name", name);
    setStr("version", version);
    setStr("main", main);
    if (!files.empty()) {
        JsonValue a;
        a.kind = JsonValue::Kind::Arr;
        for (const std::string& f : files) {
            JsonValue j;
            j.kind = JsonValue::Kind::Str;
            j.str = f;
            a.arr.push_back(std::move(j));
        }
        bool replaced = false;
        for (auto& kv : conf.obj) {
            if (kv.first == "files") {
                kv.second = std::move(a);
                replaced = true;
                break;
            }
        }
        if (!replaced) conf.obj.emplace_back("files", std::move(a));
    }
    writeFile(dir + "/lux.json", jsonDump(conf));
    return name;
}

// 把包原子地放进 packagesDir：先建临时目录，成功后再 rename
std::string makeTempDir() {
    std::string tmpl = packagesDir() + "/.lux-tmp-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdirP(packagesDir())) return "";
    char* p = mkdtemp(buf.data());
    return p ? std::string(p) : "";
}

// -----------------------------------------------------------------------------
//  安装主流程
// -----------------------------------------------------------------------------

// 从已就绪的目录安装到 packagesDir/<name>（原子 rename）
bool commitDir(const std::string& rootDir, const std::string& name,
               bool force, const std::string& version,
               const std::string& source, const std::string& origin,
               const std::vector<std::string>& files,
               const std::vector<std::pair<std::string, std::string>>& deps,
               std::string& err) {
    std::string target = packagesDir() + "/" + name;
    std::string cleanErr;
    if (isDir(target) && !force) {
        err = "包 '" + name + "' 已经安装过了（用 luxc upgrade " + name +
              " 或 luxc install --force " + name + " 覆盖）";
        return false;
    }
    // 在 rootDir 补齐 lux.json（name / main / files）
    std::string realName = ensureLuxJson(rootDir, name, version, files);
    (void)realName;
    std::string stage = makeTempDir();
    if (stage.empty()) {
        err = "无法在 '" + packagesDir() + "' 创建临时目录";
        return false;
    }
    // 把 rootDir 的内容挪进 stage（rootDir 本身是解压临时目录的子目录，
    // 直接 rename 同级更省事，但 rootDir 可能就在 stage 里 —— 这里统一 copy）
    if (!copyTree(rootDir, stage)) {
        err = "准备安装目录失败";
        removeTree(stage);
        return false;
    }
    writeMeta(stage, version, source, origin, deps);

    if (isDir(target) && force) removeTree(target);
    if (rename(stage.c_str(), target.c_str()) != 0) {
        err = "无法把包安装到 '" + target + "'";
        removeTree(stage);
        return false;
    }
    return true;
}

// 从归档文件安装（tar.gz / tgz / lux）
bool installArchive(const std::string& archivePath, const std::string& tmpRoot,
                    const std::string& requestedName, bool force,
                    const std::string& version, const std::string& source,
                    const std::string& origin, const std::string& expectSha,
                    const std::vector<std::string>& overrideFiles,
                    const std::vector<std::pair<std::string, std::string>>& overrideDeps,
                    std::string& name, std::string& err) {
    // 校验 sha256（注册表给了就必须对得上）
    if (!expectSha.empty()) {
        std::string actual;
        if (!sha256File(archivePath, actual)) {
            err = "无法读取下载的包文件";
            return false;
        }
        if (actual != expectSha) {
            err = "校验失败：SHA-256 不匹配（期望 " + expectSha +
                  "，实际 " + actual + "）";
            return false;
        }
    }

    std::string extractDir = tmpRoot + "/extract";
    if (hasSuffix(archivePath, ".lux")) {
        if (!mkdirP(extractDir)) {
            err = "无法创建解压目录";
            return false;
        }
        std::string base = basenameOf(archivePath);
        if (!copyFile(archivePath, extractDir + "/" + base)) {
            err = "无法准备单文件包";
            return false;
        }
    } else {
        if (!extractArchive(archivePath, extractDir, err)) return false;
    }

    std::string root = findPackageRoot(extractDir);
    JsonValue conf;
    std::string fallback = requestedName.empty() ? basenameOf(root)
                                                 : requestedName;
    // 优先使用归档自带的 lux.json 名字
    std::string pkgName = fallback;
    std::vector<std::string> files = overrideFiles;
    std::vector<std::pair<std::string, std::string>> deps = overrideDeps;
    if (readPkgJson(root, conf)) {
        if (const JsonValue* n = conf.find("name"))
            if (!n->asString().empty()) pkgName = n->asString();
        if (files.empty())
            if (const JsonValue* f = conf.find("files"))
                if (f->kind == JsonValue::Kind::Arr)
                    for (const JsonValue& x : f->arr)
                        if (x.kind == JsonValue::Kind::Str)
                            files.push_back(x.str);
        if (deps.empty())
            if (const JsonValue* d = conf.find("deps"))
                if (d->kind == JsonValue::Kind::Obj)
                    for (const auto& kv : d->obj)
                        deps.emplace_back(kv.first, kv.second.asString("*"));
    }
    if (!overrideFiles.empty()) files = overrideFiles;
    if (pkgName.empty()) pkgName = fallback;
    std::string verr;
    if (!validPkgName(pkgName, verr)) {
        err = "包名不合法：" + verr;
        return false;
    }
    if (!commitDir(root, pkgName, force, version, source, origin, files, deps,
                   err))
        return false;
    name = pkgName;
    return true;
}

// 注册表条目 -> 安装。递归处理依赖。
struct InstallCtx {
    std::set<std::string>* inProgress;
};

bool installIndexEntry(const IndexEntry& e, bool force, std::string& name,
                       std::string& err, std::set<std::string>& inProgress);

bool installRegistryName(const std::string& spec, bool force, std::string& name,
                         std::string& err, std::set<std::string>& inProgress) {
    std::string pkgName = spec, constraint;
    size_t at = spec.rfind('@');
    if (at != std::string::npos && at > 0) {
        pkgName = spec.substr(0, at);
        constraint = spec.substr(at + 1);
    }
    std::string verr;
    if (!validPkgName(pkgName, verr)) {
        err = verr;
        return false;
    }

    std::vector<IndexEntry> index;
    if (!loadIndex(false, index, err)) return false;

    const IndexEntry* best = nullptr;
    for (const IndexEntry& e : index) {
        if (e.name != pkgName) continue;
        if (!versionSatisfies(e.version, constraint)) continue;
        if (!best || compareVersion(e.version, best->version) > 0) best = &e;
    }
    if (!best) {
        err = "注册表里找不到包 '" + spec + "'";
        if (constraint.empty()) {
            err += "（索引里共有 " + std::to_string(index.size()) +
                   " 个版本条目；可用 luxc search 查看）";
        } else {
            err += "（没有满足约束 '" + constraint + "' 的版本）";
        }
        return false;
    }
    return installIndexEntry(*best, force, name, err, inProgress);
}

bool installIndexEntry(const IndexEntry& e, bool force, std::string& name,
                       std::string& err, std::set<std::string>& inProgress) {
    if (inProgress.count(e.name)) {
        err = "依赖出现循环：'" + e.name + "'";
        return false;
    }
    std::string target = packagesDir() + "/" + e.name;
    if (isDir(target) && !force) {
        // 已安装：版本相同则直接成功（幂等），否则提示升级
        PkgInfo info = readInstalled(e.name, target);
        if (!e.version.empty() && info.version == e.version) {
            name = e.name;
            return true;
        }
        err = "包 '" + e.name + "' 已安装" +
              (info.version.empty() ? "" : "（版本 " + info.version + "）") +
              "，注册表最新为 " + e.version +
              "；用 luxc upgrade " + e.name + " 升级，或加 --force 覆盖";
        return false;
    }

    // 先装依赖
    inProgress.insert(e.name);
    for (const auto& dep : e.deps) {
        std::string depSpec = dep.first;
        if (!dep.second.empty() && dep.second != "*")
            depSpec += "@" + dep.second;
        std::string depName, derr;
        if (!installRegistryName(depSpec, false, depName, derr, inProgress)) {
            err = "安装依赖 '" + depSpec + "' 失败：" + derr;
            inProgress.erase(e.name);
            return false;
        }
    }

    std::string tmpRoot = makeTempDir();
    if (tmpRoot.empty()) {
        inProgress.erase(e.name);
        err = "无法创建临时目录";
        return false;
    }
    std::string archive = tmpRoot + "/pkg.archive";
    // 根据扩展名决定下载文件带什么后缀（单文件 .lux 需要保留后缀）
    if (hasSuffix(e.url, ".lux")) archive += ".lux";
    std::string dlErr;
    if (!fetchToFile(e.url, archive, false, dlErr)) {
        removeTree(tmpRoot);
        inProgress.erase(e.name);
        err = dlErr;
        return false;
    }
    bool ok = installArchive(archive, tmpRoot, e.name, force, e.version,
                             "registry", e.url, e.sha256, e.files, e.deps, name,
                             err);
    removeTree(tmpRoot);
    inProgress.erase(e.name);
    return ok;
}

// URL 直接安装
bool installUrl(const std::string& url, bool force, std::string& name,
                std::string& err) {
    std::string tmpRoot = makeTempDir();
    if (tmpRoot.empty()) {
        err = "无法创建临时目录";
        return false;
    }
    std::string archive = tmpRoot + "/pkg.archive";
    std::string tail = url;
    size_t q = tail.find_first_of("?#");
    if (q != std::string::npos) tail = tail.substr(0, q);
    if (hasSuffix(tail, ".lux")) archive += ".lux";
    else if (hasSuffix(tail, ".tar.gz")) archive += ".tar.gz";
    else if (hasSuffix(tail, ".tgz")) archive += ".tgz";
    std::string dlErr;
    if (!fetchToFile(url, archive, false, dlErr)) {
        removeTree(tmpRoot);
        err = dlErr;
        return false;
    }
    bool ok = installArchive(archive, tmpRoot, "", force, "", "url", url, "", {},
                             {}, name, err);
    removeTree(tmpRoot);
    return ok;
}

bool looksLikeUrl(const std::string& s) {
    return hasPrefix(s, "http://") || hasPrefix(s, "https://") ||
           hasPrefix(s, "ftp://") || hasPrefix(s, "ftps://");
}

}  // namespace

// =============================================================================
//  对外接口
// =============================================================================

bool pkgAdd(const std::string& path, std::string& name, std::string& err) {
    name.clear();
    bool dir = isDir(path);
    if (!dir && !isFile(path)) {
        err = "找不到要安装的包 '" + path + "'";
        return false;
    }

    // ---- 确定包名 ----
    if (!dir) {
        if (!hasSuffix(path, ".lux")) {
            err = "单文件包必须是 .lux 文件：'" + path + "'";
            return false;
        }
        name = stemOf(path);
    } else {
        JsonValue conf;
        if (readPkgJson(path, conf)) {
            if (const JsonValue* n = conf.find("name")) name = n->asString();
        }
        if (name.empty()) {
            // 兼容老的单层 lux.json
            auto raw = readFile(path + "/lux.json");
            if (!raw || !jsonStringField(*raw, "name", name) || name.empty())
                name = basenameOf(path);
        }
    }
    if (!validPkgName(name, err)) return false;

    std::string dst = packagesDir() + "/" + name;
    if (isDir(dst)) {
        err = "包 '" + name + "' 已经安装过了（先 luxc delete " + name + "）";
        return false;
    }
    if (!mkdirP(packagesDir())) {
        err = "无法创建包目录 '" + packagesDir() + "'";
        return false;
    }

    std::string stage = makeTempDir();
    if (stage.empty()) {
        err = "无法创建临时安装目录";
        return false;
    }
    bool ok;
    std::string version;
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> deps;
    if (!dir) {
        ok = copyFile(path, stage + "/" + stemOf(path) + ".lux");
        files.push_back(stemOf(path) + ".lux");
    } else {
        ok = copyTree(path, stage);
        JsonValue conf;
        if (readPkgJson(path, conf)) {
            if (const JsonValue* v = conf.find("version"))
                version = v->asString();
            if (const JsonValue* f = conf.find("files"))
                if (f->kind == JsonValue::Kind::Arr)
                    for (const JsonValue& x : f->arr)
                        if (x.kind == JsonValue::Kind::Str)
                            files.push_back(x.str);
            if (const JsonValue* d = conf.find("deps"))
                if (d->kind == JsonValue::Kind::Obj)
                    for (const auto& kv : d->obj)
                        deps.emplace_back(kv.first, kv.second.asString("*"));
        }
    }
    if (!ok) {
        err = "拷贝 '" + path + "' 失败";
        removeTree(stage);
        return false;
    }
    ensureLuxJson(stage, name, version, files);
    writeMeta(stage, version, "local",
              isDir(path) ? normalizeAbs(path) : path, deps);
    if (rename(stage.c_str(), dst.c_str()) != 0) {
        err = "无法把包安装到 '" + dst + "'";
        removeTree(stage);
        return false;
    }
    return true;
}

std::vector<PkgInfo> pkgList() {
    std::vector<PkgInfo> out;
    DIR* d = opendir(packagesDir().c_str());
    if (!d) return out;  // 还没有安装过任何包
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string dir = packagesDir() + "/" + e->d_name;
        if (!isDir(dir)) continue;
        out.push_back(readInstalled(e->d_name, dir));
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const PkgInfo& a, const PkgInfo& b) { return a.name < b.name; });
    return out;
}

bool pkgDelete(const std::string& name, std::string& err) {
    if (!validPkgName(name, err)) return false;
    std::string dir = packagesDir() + "/" + name;
    if (!isDir(dir)) {
        err = "包 '" + name + "' 没有安装";
        return false;
    }
    if (!removeTree(dir)) {
        err = "删除包目录 '" + dir + "' 失败";
        return false;
    }
    return true;
}

std::string registryUrl() {
    const char* e = std::getenv("LUX_REGISTRY");
    if (e && *e) return e;
    JsonValue cfg = loadConfig();
    const JsonValue* v = cfg.find("registry");
    if (v && v->kind == JsonValue::Kind::Str && !v->str.empty()) return v->str;
    return kDefaultRegistry;
}

bool setRegistryUrl(const std::string& url) {
    if (url.empty()) {
        // 清除配置项（回到内置默认）
        JsonValue cfg = loadConfig();
        cfg.obj.erase(
            std::remove_if(cfg.obj.begin(), cfg.obj.end(),
                           [](const std::pair<std::string, JsonValue>& kv) {
                               return kv.first == "registry";
                           }),
            cfg.obj.end());
        if (!mkdirP(luxHome())) return false;
        return writeFile(configPath(), jsonDump(cfg));
    }
    configSet("registry", url);
    return true;
}

bool pkgInstall(const std::string& spec, bool force, std::string& err,
                std::string& installedName) {
    installedName.clear();
    if (spec.empty()) {
        err = "缺少包名";
        return false;
    }
    if (looksLikeUrl(spec)) return installUrl(spec, force, installedName, err);
    if (isDir(spec) || isFile(spec)) {
        std::string name;
        if (!pkgAdd(spec, name, err)) return false;
        installedName = name;
        return true;
    }
    std::set<std::string> inProgress;
    return installRegistryName(spec, force, installedName, err, inProgress);
}

bool pkgUpdateRegistry(std::string& err) {
    std::vector<IndexEntry> index;
    if (!loadIndex(true, index, err)) return false;
    return true;
}

bool pkgSearch(const std::string& query, std::vector<PkgInfo>& out,
               std::string& err) {
    std::vector<IndexEntry> index;
    if (!loadIndex(false, index, err)) return false;
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // 同名多版本时只保留最新
    std::unordered_map<std::string, const IndexEntry*> latest;
    for (const IndexEntry& e : index) {
        auto it = latest.find(e.name);
        if (it == latest.end() || compareVersion(e.version, it->second->version) > 0)
            latest[e.name] = &e;
    }
    out.clear();
    for (const auto& kv : latest) {
        const IndexEntry& e = *kv.second;
        if (!q.empty()) {
            std::string hay = e.name + " " + e.summary + " " + e.description;
            for (const std::string& t : e.tags) hay += " " + t;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (hay.find(q) == std::string::npos) continue;
        }
        PkgInfo info;
        info.name = e.name;
        info.version = e.version;
        info.main = e.summary;
        info.source = "registry";
        info.origin = e.url;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const PkgInfo& a, const PkgInfo& b) { return a.name < b.name; });
    return true;
}

bool pkgInfo(const std::string& name, std::string& out, std::string& err) {
    std::vector<IndexEntry> index;
    if (!loadIndex(false, index, err)) return false;
    const IndexEntry* best = nullptr;
    int count = 0;
    for (const IndexEntry& e : index) {
        if (e.name != name) continue;
        count++;
        if (!best || compareVersion(e.version, best->version) > 0) best = &e;
    }
    if (!best) {
        err = "注册表里没有包 '" + name + "'";
        return false;
    }
    out = "包名:     " + best->name + "\n";
    out += "版本:     " + best->version;
    if (count > 1) out += "（共 " + std::to_string(count) + " 个版本）";
    out += "\n";
    if (!best->summary.empty()) out += "简介:     " + best->summary + "\n";
    if (!best->license.empty()) out += "许可:     " + best->license + "\n";
    if (!best->homepage.empty()) out += "主页:     " + best->homepage + "\n";
    if (!best->luxReq.empty()) out += "Lux 要求: " + best->luxReq + "\n";
    if (!best->authors.empty()) {
        out += "作者:     ";
        for (size_t i = 0; i < best->authors.size(); i++) {
            if (i) out += ", ";
            out += best->authors[i];
        }
        out += "\n";
    }
    if (!best->tags.empty()) {
        out += "标签:     ";
        for (size_t i = 0; i < best->tags.size(); i++) {
            if (i) out += ", ";
            out += best->tags[i];
        }
        out += "\n";
    }
    if (!best->deps.empty()) {
        out += "依赖:     ";
        for (size_t i = 0; i < best->deps.size(); i++) {
            if (i) out += ", ";
            out += best->deps[i].first + "@" + best->deps[i].second;
        }
        out += "\n";
    }
    out += "下载:     " + best->url + "\n";
    if (!best->sha256.empty()) out += "SHA-256:  " + best->sha256 + "\n";
    if (!best->description.empty() && best->description != best->summary)
        out += "\n" + best->description + "\n";
    return true;
}

bool pkgUpgrade(const std::string& name, std::string& err) {
    std::vector<PkgInfo> installed = pkgList();
    std::vector<std::string> targets;
    if (name.empty()) {
        for (const PkgInfo& p : installed) targets.push_back(p.name);
    } else {
        targets.push_back(name);
    }
    if (targets.empty()) {
        err = "还没有安装任何包";
        return false;
    }
    bool any = false;
    for (const std::string& t : targets) {
        std::string installedName;
        std::string ierr;
        if (pkgInstall(t, true, ierr, installedName)) {
            std::printf("  已升级 %s\n", t.c_str());
            any = true;
        } else {
            std::fprintf(stderr, "  跳过 %s：%s\n", t.c_str(), ierr.c_str());
            err = ierr;
        }
    }
    return any;
}

bool pkgPublish(const std::string& dir, std::string& err) {
    if (!isDir(dir)) {
        err = "'" + dir + "' 不是目录";
        return false;
    }
    JsonValue conf;
    if (!readPkgJson(dir, conf)) {
        err = "包目录缺少 lux.json（至少要有 name / version / main）";
        return false;
    }
    std::string name = conf.find("name") ? conf.find("name")->asString() : "";
    std::string version =
        conf.find("version") ? conf.find("version")->asString() : "";
    if (name.empty()) {
        err = "lux.json 缺少 \"name\"";
        return false;
    }
    std::string verr;
    if (!validPkgName(name, verr)) {
        err = verr;
        return false;
    }
    if (version.empty()) {
        err = "lux.json 缺少 \"version\"（发布必须带版本号）";
        return false;
    }
    if (!validVersion(version)) {
        err = "版本号 '" + version + "' 不合法";
        return false;
    }

    std::string tmp = makeTempDir();
    if (tmp.empty()) {
        err = "无法创建临时目录";
        return false;
    }
    std::string archive = tmp + "/" + name + "-" + version + ".tar.gz";
    std::string tarOut;
    int rc = runCapture("tar -czf " + shellQuote(archive) + " -C " +
                            shellQuote(dir) + " .",
                        tarOut);
    if (rc != 0) {
        removeTree(tmp);
        err = "打包失败：" + trim(tarOut);
        return false;
    }
    std::string sha;
    if (!sha256File(archive, sha)) {
        removeTree(tmp);
        err = "无法计算 SHA-256";
        return false;
    }
    struct stat st;
    long long size = (stat(archive.c_str(), &st) == 0) ? (long long)st.st_size : 0;

    // 生成注册表元数据
    std::string registryBase = registryUrl();
    size_t q = registryBase.find_first_of("?#");
    if (q != std::string::npos) registryBase = registryBase.substr(0, q);
    size_t slash = registryBase.find_last_of('/');
    std::string publicBase =
        slash == std::string::npos ? registryBase : registryBase.substr(0, slash);
    std::string dlUrl = publicBase + "/packages/" + name + "/" + version +
                        ".tar.gz";

    JsonValue meta;
    meta.kind = JsonValue::Kind::Obj;
    auto putS = [&](const char* k, const std::string& v) {
        JsonValue j;
        j.kind = JsonValue::Kind::Str;
        j.str = v;
        meta.obj.emplace_back(k, std::move(j));
    };
    putS("name", name);
    putS("version", version);
    for (const char* k : {"summary", "description", "main", "license",
                          "homepage", "lux"}) {
        if (const JsonValue* v = conf.find(k))
            if (v->kind == JsonValue::Kind::Str && !v->str.empty())
                putS(k, v->str);
    }
    putS("url", dlUrl);
    putS("sha256", sha);
    {
        JsonValue sz;
        sz.kind = JsonValue::Kind::Num;
        sz.number = (double)size;
        meta.obj.emplace_back("size", std::move(sz));
    }
    for (const char* k : {"files", "tags", "authors"}) {
        if (const JsonValue* v = conf.find(k))
            if (v->kind == JsonValue::Kind::Arr)
                meta.obj.emplace_back(k, *v);
    }
    if (const JsonValue* d = conf.find("deps"))
        if (d->kind == JsonValue::Kind::Obj) meta.obj.emplace_back("deps", *d);
    std::string metaText = jsonDump(meta);
    std::string metaFile = tmp + "/" + name + "-" + version + ".json";
    writeFile(metaFile, metaText);

    // 上传：HTTP(S) 注册表走账号 API（需 luxc login）；ftp:// 注册表走 FTP。
    // $LUX_PUBLISH_FTP=1 可强制走 FTP（仍用注册表地址推导下载 URL）。
    std::string reg = registryUrl();
    const char* forceFtp = std::getenv("LUX_PUBLISH_FTP");
    bool isHttp = (hasPrefix(reg, "http://") || hasPrefix(reg, "https://")) &&
                  !(forceFtp && *forceFtp);
    if (isHttp) {
        std::string token = authTokenCfg();
        if (token.empty()) {
            removeTree(tmp);
            err = "发布包需要登录：先运行 luxc login <用户名>（或在网页注册后登录）";
            return false;
        }
        std::vector<std::pair<std::string, std::string>> fields;
        fields.emplace_back("meta", metaText);
        std::string out;
        if (!httpPostForm(apiUrl(reg, "publish"), fields, "archive", archive,
                          token, out, err)) {
            removeTree(tmp);
            return false;
        }
        JsonValue resp;
        if (!apiResult(out, resp, err)) {
            removeTree(tmp);
            return false;
        }
        const JsonValue* urlV = resp.find("url");
        std::string finalUrl = urlV ? urlV->asString() : dlUrl;
        removeTree(tmp);
        invalidateIndexCache();
        std::printf("已发布 %s@%s\n", name.c_str(), version.c_str());
        std::printf("  归档: %s\n", finalUrl.c_str());
        return true;
    }

    // ftp:// 注册表：沿用 FTP 上传（需要 LUX_FTP_* 配置）
    JsonValue cfg = loadConfig();
    std::string host = configString(cfg, "LUX_FTP_HOST", "ftpHost", kDefaultFtpHost);
    std::string user = configString(cfg, "LUX_FTP_USER", "ftpUser", kDefaultFtpUser);
    std::string pass = configString(cfg, "LUX_FTP_PASS", "ftpPass", kDefaultFtpPass);
    std::string root = configString(cfg, "LUX_FTP_ROOT", "ftpRoot", kDefaultFtpRoot);
    if (user.empty() || pass.empty()) {
        err = "FTP 发布需要设置 LUX_FTP_USER / LUX_FTP_PASS（或在 " +
              configPath() + " 里配置 ftpUser / ftpPass）；"
              "正式发布请改用账号 API：luxc login 后 luxc publish";
        removeTree(tmp);
        return false;
    }
    if (!root.empty() && root[0] != '/') root = "/" + root;
    if (!root.empty() && root.back() == '/') root.pop_back();

    std::string remote = "ftp://" + host + root + "/packages/" + name + "/";
    auto upload = [&](const std::string& local, const std::string& dst) -> bool {
        std::string out;
        std::string cmd = "curl -fsS --connect-timeout 20 -T " +
                          shellQuote(local) + " --user " + shellQuote(user + ":" + pass) +
                          " --ftp-create-dirs " + shellQuote(remote + dst);
        int r = runCapture(cmd, out);
        if (r != 0) {
            err = "上传 '" + dst + "' 失败：" + trim(out);
            return false;
        }
        return true;
    };
    if (!upload(archive, version + ".tar.gz") ||
        !upload(metaFile, version + ".json") ||
        !upload(metaFile, "latest.json")) {
        removeTree(tmp);
        return false;
    }
    removeTree(tmp);
    std::printf("已发布 %s@%s\n", name.c_str(), version.c_str());
    std::printf("  归档: %s\n", dlUrl.c_str());
    std::printf("  元数据: ftp://%s%s/packages/%s/%s.json\n", host.c_str(),
                root.c_str(), name.c_str(), version.c_str());
    return true;
}

std::string authToken() { return authTokenCfg(); }
std::string authUser() { return authUserCfg(); }

bool luxLogin(const std::string& user, const std::string& pass,
              std::string& userOut, std::string& err) {
    std::string reg = registryUrl();
    if (!(hasPrefix(reg, "http://") || hasPrefix(reg, "https://"))) {
        err = "登录只支持 HTTP(S) 注册表（当前是 '" + reg + "'）";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("username", user);
    fields.emplace_back("password", pass);
    std::string out;
    if (!httpPostForm(apiUrl(reg, "login"), fields, "", "", "", out, err))
        return false;
    JsonValue resp;
    if (!apiResult(out, resp, err)) return false;
    const JsonValue* tok = resp.find("token");
    if (!tok || tok->asString().empty()) {
        err = "服务器没有返回令牌";
        return false;
    }
    std::string name = user;
    if (const JsonValue* u = resp.find("user"))
        if (const JsonValue* n = u->find("name")) name = n->asString();
    configSet("token", tok->asString());
    configSet("tokenUser", name);
    userOut = name;
    return true;
}

bool luxLogout(std::string& err) {
    std::string token = authTokenCfg();
    if (!token.empty()) {
        std::string reg = registryUrl();
        if (hasPrefix(reg, "http://") || hasPrefix(reg, "https://")) {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.emplace_back("token", token);
            std::string out, ignored;
            httpPostForm(apiUrl(reg, "logout"), fields, "", "", "", out, ignored);
        }
    }
    configRemove("token");
    configRemove("tokenUser");
    (void)err;
    return true;
}

bool luxWhoami(std::string& userOut, std::string& err) {
    std::string token = authTokenCfg();
    if (token.empty()) {
        err = "尚未登录（先运行 luxc login）";
        return false;
    }
    std::string reg = registryUrl();
    std::string url = apiUrl(reg, "whoami");
    std::string out;
    if (!httpPostForm(url, {}, "", "", token, out, err)) return false;
    JsonValue resp;
    if (!apiResult(out, resp, err)) return false;
    if (const JsonValue* u = resp.find("user"))
        if (const JsonValue* n = u->find("name")) userOut = n->asString();
    if (userOut.empty()) userOut = authUserCfg();
    return true;
}

bool pkgUnpublish(const std::string& name, const std::string& version,
                  std::string& err) {
    std::string reg = registryUrl();
    if (!(hasPrefix(reg, "http://") || hasPrefix(reg, "https://"))) {
        err = "unpublish 只支持 HTTP(S) 注册表";
        return false;
    }
    std::string token = authTokenCfg();
    if (token.empty()) {
        err = "删除包需要登录：先运行 luxc login";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("name", name);
    if (!version.empty()) fields.emplace_back("version", version);
    std::string out;
    if (!httpPostForm(apiUrl(reg, "delete"), fields, "", "", token, out, err))
        return false;
    JsonValue resp;
    if (!apiResult(out, resp, err)) return false;
    invalidateIndexCache();
    return true;
}

}  // namespace lux
