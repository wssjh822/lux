// =============================================================================
//  pkgs.cpp : 包管理 —— luxc add / list / delete
//
//  已安装的包统一放在 <luxHome>/packages/ 下（默认 ~/.lux/packages/，
//  可用环境变量 LUX_HOME 重定位）：
//
//    luxc add <包目录|.lux 文件>     安装（拷贝）到包目录
//    luxc list                       列出已安装的包
//    luxc delete <包名>              删除包
//
//  安装后的包可以直接被 import "包名" 引用（见 loader.cpp 的解析顺序）。
// =============================================================================
#include "lux.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace lux {

namespace {

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 逐级创建目录（已存在不算失败）
bool mkdirP(const std::string& path) {
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

// 包名只能是合法的路径分量
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
    return true;
}

// 统计目录顶层有多少个 .lux 文件
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

// 找出目录顶层的第一个 .lux 文件（作为兜底入口），没有返回空串
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

}  // namespace

// -----------------------------------------------------------------------------
//  pkgAdd / pkgList / pkgDelete
// -----------------------------------------------------------------------------

bool pkgAdd(const std::string& path, std::string& name, std::string& err) {
    name.clear();
    bool isFile = !isDir(path);
    if (!isFile && !isDir(path)) {
        err = "找不到要安装的包 '" + path + "'";
        return false;
    }

    // ---- 确定包名 ----
    if (isFile) {
        if (!hasSuffix(path, ".lux")) {
            err = "单文件包必须是 .lux 文件：'" + path + "'";
            return false;
        }
        name = stemOf(path);
    } else {
        // 目录：优先读 lux.json 的 "name" 字段，否则用目录名
        auto conf = readFile(path + "/lux.json");
        if (!conf || !jsonStringField(*conf, "name", name) || name.empty()) {
            name = basenameOf(path);
        }
    }
    if (!validPkgName(name, err)) return false;

    // ---- 目标位置 ----
    std::string dst = packagesDir() + "/" + name;
    if (isDir(dst)) {
        err = "包 '" + name + "' 已经安装过了（先 luxc delete " + name + "）";
        return false;
    }
    if (!mkdirP(packagesDir())) {
        err = "无法创建包目录 '" + packagesDir() + "'";
        return false;
    }

    // ---- 拷贝 ----
    if (isFile) {
        if (!mkdirP(dst) || !copyFile(path, dst + "/" + stemOf(path) + ".lux")) {
            err = "拷贝 '" + path + "' 失败";
            removeTree(dst);
            return false;
        }
        // 单文件包补一份 lux.json，让 list / import 都有据可查
        std::string conf = "{\n  \"name\": \"" + name + "\",\n  \"main\": \"" +
                           stemOf(path) + ".lux\"\n}\n";
        if (!writeFile(dst + "/lux.json", conf)) {
            err = "写入包配置失败";
            removeTree(dst);
            return false;
        }
    } else {
        if (!copyTree(path, dst)) {
            err = "拷贝包目录 '" + path + "' 失败";
            removeTree(dst);
            return false;
        }
        // 目录包没有 lux.json 时也补一份，方便 list 展示
        std::string confPath = dst + "/lux.json";
        if (!readFile(confPath)) {
            std::string main = firstLuxFile(dst);
            std::string conf = "{\n  \"name\": \"" + name + "\",\n  \"main\": \"" +
                               main + "\"\n}\n";
            writeFile(confPath, conf);
        }
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
        PkgInfo info;
        info.name = e->d_name;
        info.files = countLuxFiles(dir);
        auto conf = readFile(dir + "/lux.json");
        if (conf) jsonStringField(*conf, "main", info.main);
        out.push_back(std::move(info));
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

}  // namespace lux
