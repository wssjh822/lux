// =============================================================================
//  loader.cpp : 模块加载 —— 递归解析 import，把多个源文件合并成一个程序
//
//  import "路径" 支持四种形态：
//    1. "math" / "time" / "system" / "file" / "string"   内置标准库模块
//    2. "c:库名"                             链接外部 C 库（传给 cc -l库名）
//    3. "文件.lux"                           相对当前文件所在目录的源码文件
//    4. "目录"                               包：目录里有 lux.json 配置文件
//                                               （或直接有 main.lux / lib.lux）
//
//  每个被 import 的模块都有自己的命名空间（模块名 = 文件名 / 包名 / 标准库名）：
//    import "x"        成员直接可用（默认方式，向后兼容）
//    import "x" as y   只能用 y.成员 访问，避免命名空间污染
// =============================================================================
#include "lux.hpp"

#include <climits>
#include <cstdlib>
#include <fstream>

namespace lux {

namespace {

// 取路径所在目录；不含 '/' 时返回空串（表示当前目录）
std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash);
}

std::string pathJoin(const std::string& dir, const std::string& rel) {
    if (!rel.empty() && rel[0] == '/') return rel;  // 绝对路径原样使用
    if (dir.empty()) return rel;
    if (dir.back() == '/') return dir + rel;
    return dir + "/" + rel;
}

// 轻量规范化：去掉开头的 "./"，折叠 "x/../" 形式的段（不访问文件系统，
// 与文件是否存在无关，保证去重键稳定）
std::string normalizePath(const std::string& p) {
    std::vector<std::string> segs;
    std::string cur;
    bool absolute = !p.empty() && p[0] == '/';
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) {
                if (cur == "..") {
                    if (!segs.empty() && segs.back() != "..") {
                        segs.pop_back();
                    } else if (!absolute) {
                        segs.push_back(cur);
                    }
                } else if (cur != ".") {
                    segs.push_back(cur);
                }
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur == "..") {
            if (!segs.empty() && segs.back() != "..") {
                segs.pop_back();
            } else if (!absolute) {
                segs.push_back(cur);
            }
        } else if (cur != ".") {
            segs.push_back(cur);
        }
    }
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < segs.size(); i++) {
        if (i) out += "/";
        out += segs[i];
    }
    return out.empty() ? "." : out;
}

// 规范路径：文件已存在时用 realpath 展开，否则退化为普通规范化
std::string canonical(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return std::string(buf);
    return normalizePath(p);
}

bool hasSuffix(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

const std::set<std::string>& stdlibNames() {
    static const std::set<std::string> names = {"math", "time", "system",
                                                "file", "string"};
    return names;
}

}  // namespace

// -----------------------------------------------------------------------------
//  ModuleLoader
// -----------------------------------------------------------------------------

Program* ModuleLoader::load(const std::string& rootPath) {
    loadFile(rootPath, "");
    return &merged;
}

void ModuleLoader::loadFile(const std::string& path, const std::string& module) {
    std::string canon = canonical(path);
    if (loaded_.count(canon)) return;  // 已加载过（去重，也防止循环 import）
    loaded_.insert(canon);

    auto src = readFile(path);
    if (!src) {
        diags.error(DiagCode::kModule, normalizePath(path), SourceLoc{1, 1},
                    "无法读取源文件 '" + path + "'");
        return;
    }
    std::string display = normalizePath(path);
    // sources 同时以规范路径与显示路径为键：去重用 realpath，诊断用 display，
    // 两种键都必须能查到源码（0.5.1 修复诊断上下文可能丢失的问题）
    sources[canon] = *src;
    sources[display] = *src;

    int errBefore = diags.errors;
    std::vector<Token> toks = tokenize(*src, display, diags);
    if (diags.errors > errBefore) return;  // 有词法错误：跳过解析，避免级联报错

    auto parser = std::make_unique<Parser>(std::move(toks), display, diags);
    Program* p = parser->parseProgram();

    // 标记本文件所有声明的模块归属
    for (FuncDecl* fn : p->funcs) fn->module = module;
    for (GlobalConstDecl* gc : p->consts) gc->module = module;
    for (StructDecl* sd : p->structs) sd->module = module;

    // 先把本文件的声明合入程序，再递归处理 import（保持源文件内顺序）
    for (ImportDecl* imp : p->imports) processImport(imp, dirOf(display));
    for (FuncDecl* fn : p->funcs) merged.funcs.push_back(fn);
    for (GlobalConstDecl* gc : p->consts) merged.consts.push_back(gc);
    for (StructDecl* sd : p->structs) merged.structs.push_back(sd);
    parsers.push_back(std::move(parser));
}

void ModuleLoader::processImport(const ImportDecl* imp,
                                 const std::string& fromDir) {
    const std::string& p = imp->path;

    // 1. C 库： import "c:库名"  → 链接 -l库名（配合 extern fn 使用）
    if (p.rfind("c:", 0) == 0) {
        std::string lib = p.substr(2);
        if (lib.empty()) {
            diags.error(DiagCode::kModule, imp->file, imp->loc,
                        "import \"c:\" 后面需要写库名，例如 import \"c:m\""
                        "（等价于链接 -lm）");
            return;
        }
        linkLibs.push_back(lib);
        return;
    }

    // 2. 内置标准库模块（优先于本地文件：import "math" 永远拿到标准库；
    //    如果本地恰好有同名 .lux 文件，提醒它不会生效）
    if (stdlibNames().count(p)) {
        std::string localSame = normalizePath(pathJoin(fromDir, p)) + ".lux";
        if (fileExists(localSame)) {
            diags.warn(DiagCode::kWShadowStdlib, imp->file, imp->loc,
                       "本地文件 '" + localSame + "' 与标准库模块 '" + p +
                           "' 同名，import \"" + p +
                           "\" 拿到的是标准库；本地文件不会被打包加载"
                           "（建议改名避免混淆）");
        }
        // math 模块在第一次 import 时合成 pi / e 两个常量
        if (p == "math" && !modules.imported.count("math")) {
            auto mk = [&](const char* name, double v) {
                synthExprs_.push_back(
                    std::make_unique<FloatLitExpr>(imp->loc, v));
                synthConsts_.push_back(std::make_unique<GlobalConstDecl>());
                auto* gc = synthConsts_.back().get();
                gc->name = name;
                gc->nameLoc = imp->loc;
                gc->loc = imp->loc;
                gc->ann = TypeAnn{TyStore::float64Ty(), imp->loc, true};
                gc->init = synthExprs_.back().get();
                gc->synth = true;
                gc->module = "math";
                merged.consts.push_back(gc);
            };
            mk("pi", 3.14159265358979323846);
            mk("e", 2.71828182845904523536);
        }
        registerModule(p, imp);
        return;
    }

    // 3. .lux 文件 / 包目录（相对于 import 声明所在文件的目录解析）
    std::string base = normalizePath(pathJoin(fromDir, p));
    if (hasSuffix(base, ".lux")) {
        if (fileExists(base)) {
            loadFile(base, stemOf(basenameOf(base)));
            registerModule(stemOf(basenameOf(base)), imp);
            return;
        }
    } else {
        if (fileExists(base + ".lux")) {
            std::string mod = stemOf(basenameOf(base + ".lux"));
            loadFile(base + ".lux", mod);
            registerModule(mod, imp);
            return;
        }
        if (fileExists(base + "/lux.json")) {
            std::string mod = basenameOf(base);
            loadPackage(base, mod);
            registerModule(mod, imp);
            return;
        }
        if (fileExists(base + "/main.lux")) {
            std::string mod = basenameOf(base);
            loadFile(base + "/main.lux", mod);
            registerModule(mod, imp);
            return;
        }
        if (fileExists(base + "/lib.lux")) {
            std::string mod = basenameOf(base);
            loadFile(base + "/lib.lux", mod);
            registerModule(mod, imp);
            return;
        }
    }

    // 4. 已安装的包（~/.lux/packages/）
    std::string pd = normalizePath(pathJoin(packagesDir(), p));
    if (fileExists(pd + "/lux.json")) {
        std::string mod = basenameOf(pd);
        loadPackage(pd, mod);
        registerModule(mod, imp);
        return;
    }
    if (fileExists(pd + "/main.lux")) {
        std::string mod = basenameOf(pd);
        loadFile(pd + "/main.lux", mod);
        registerModule(mod, imp);
        return;
    }
    if (fileExists(pd + "/lib.lux")) {
        std::string mod = basenameOf(pd);
        loadFile(pd + "/lib.lux", mod);
        registerModule(mod, imp);
        return;
    }

    diags.error(DiagCode::kModule, imp->file, imp->loc,
                "找不到导入的模块 '" + p + "'（已尝试 '" + base +
                    ".lux'、目录 '" + base + "'、以及已安装包目录 '" + pd +
                    "'）");
}

// 把模块注册进 ModuleInfo：默认导入注入全局命名空间，别名导入只登记映射，
// from ... import a, b 只把列出的成员注入全局命名空间（0.9.3）。
void ModuleLoader::registerModule(const std::string& mod, const ImportDecl* imp) {
    modules.imported.insert(mod);
    if (imp->selective) {
        if (imp->star) {
            modules.flat.insert(mod);  // from "mod" import * 等价于默认导入
            return;
        }
        auto& set = modules.selected[mod];
        for (const std::string& n : imp->names) set.insert(n);
        return;
    }
    if (imp->alias.empty()) {
        modules.flat.insert(mod);
        return;
    }
    auto it = modules.aliases.find(imp->alias);
    if (it != modules.aliases.end() && it->second != mod) {
        diags.error(DiagCode::kModule, imp->file, imp->loc,
                    "导入别名 '" + imp->alias + "' 已经被 '" + it->second +
                        "' 占用，无法再用于 '" + mod + "'");
        return;
    }
    modules.aliases[imp->alias] = mod;
}

// 加载一个包目录：读取 lux.json 决定入口文件
void ModuleLoader::loadPackage(const std::string& dir, const std::string& module) {
    auto conf = readFile(dir + "/lux.json");
    if (!conf) {
        diags.error(DiagCode::kModule, dir + "/lux.json", SourceLoc{1, 1},
                    "无法读取包配置文件 '" + dir + "/lux.json'");
        return;
    }
    std::string mainFile;
    std::vector<std::string> files;
    if (jsonStringField(*conf, "main", mainFile)) {
        loadFile(normalizePath(pathJoin(dir, mainFile)), module);
        return;
    }
    if (jsonStringArray(*conf, "files", files)) {
        for (const std::string& f : files) {
            loadFile(normalizePath(pathJoin(dir, f)), module);
        }
        return;
    }
    if (fileExists(dir + "/main.lux")) {
        loadFile(dir + "/main.lux", module);
        return;
    }
    if (fileExists(dir + "/lib.lux")) {
        loadFile(dir + "/lib.lux", module);
        return;
    }
    diags.error(DiagCode::kModule, dir + "/lux.json", SourceLoc{1, 1},
                "包配置文件 '" + dir +
                    "/lux.json' 里既没有 \"main\" 也没有 \"files\"，且目录下"
                    "没有 main.lux / lib.lux");
}

}  // namespace lux
