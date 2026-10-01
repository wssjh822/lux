// =============================================================================
//  main.cpp : luxc 驱动程序
//
//  用法：
//    luxc <源文件.lux> [选项]       编译成可执行文件
//    luxc add <包目录|.lux 文件>    安装包到 ~/.lux/packages/
//    luxc list                      列出已安装的包
//    luxc delete <包名>             删除已安装的包
//    luxc hello.lux -o out         指定输出文件名
//    luxc hello.lux --emit-c       只输出生成的 C 代码（到 stdout）
//    luxc hello.lux --run          编译后立即运行
// =============================================================================
#include "lux.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

// 版本号由 Makefile 经 -DLUX_VERSION='"x.y.z"' 编译期注入（单源化）
#ifndef LUX_VERSION
#define LUX_VERSION "0.9.3"
#endif

const std::string kVersion = std::string("Lux ") + LUX_VERSION + " (C 后端 + 原生后端)";

void printHelp(const char* argv0) {
    std::printf(
        "用法:\n"
        "  %s <源文件.lux> [选项]          把 Lux 源码编译成本地可执行文件\n"
        "  %s build [路径] [动作]          按路径下的 LuxBuildFile 构建项目\n"
        "  %s repl                        交互式执行环境（REPL）\n"
        "\n"
        "构建动作（luxc build）：build（缺省）/ run / test / clean / rebuild\n"
        "\n"
        "包管理:\n"
        "  %s install <名字[@版本]|URL>    从注册表 / 网络下载并安装包\n"
        "  %s install --force <...>        覆盖已安装的同名包\n"
        "  %s add <包目录 | .lux 文件>     安装本地包（离线）\n"
        "  %s remove <包名>                卸载（delete 同义）\n"
        "  %s list                         列出已安装的包\n"
        "  %s search [关键词]              搜索注册表\n"
        "  %s info <包名>                  查看注册表里的包信息\n"
        "  %s update                       刷新注册表索引缓存\n"
        "  %s upgrade [包名]               升级已安装的包（缺省=全部）\n"
        "  %s publish <包目录>             发布包到注册表（需登录）\n"
        "  %s unpublish <包名> [版本]      从注册表删除自己的包\n"
        "  %s login [用户名]               登录注册表账号并保存令牌\n"
        "  %s logout                       退出登录\n"
        "  %s whoami                       显示当前登录账号\n"
        "  %s registry [URL]               查看 / 设置注册表地址\n"
        "\n"
        "注册表默认 %s（可用 $LUX_REGISTRY 或 luxc registry 改）\n"
        "包注册表网页：在注册表地址同级目录打开 index.php（注册 / 上传 / 浏览）\n"
        "\n"
        "编译选项:\n"
        "  -o <文件>        指定输出的可执行文件名（默认取源文件名去掉扩展名）\n"
        "  -c <文件>        把生成的 C 代码写到指定文件\n"
        "  --emit-c         只把生成的 C 代码输出到标准输出，不调用 C 编译器\n"
        "  --keep-c         编译后把生成的 .c 文件保留在可执行文件旁\n"
        "  --native         原生代码生成后端：直接输出本机 ELF，不依赖 C 编译器\n"
        "  --arc            引用计数（ARC）内存回收（0.9.4 起为默认，本开关冗余）\n"
        "  --no-arc         关闭 ARC：堆对象只增不减（0.9.1 语义，仅用于排查）\n"
        "  -O <级别>        优化级别 0~3（默认 2，直接传给 C 编译器）\n"
        "  --run            编译成功后立即运行生成的可执行文件\n"
        "  --cc <编译器>    指定使用的 C 编译器（默认自动探测 cc/clang/gcc）\n"
        "  -C <选项>        把额外选项原样传给 C 编译器（可多次使用）\n"
        "  -Werror          把警告当成错误\n"
        "  --no-color       关闭彩色诊断输出\n"
        "  -v, --version    显示版本信息\n"
        "  -h, --help       显示本帮助\n"
        "\n"
        "示例:\n"
        "  %s hello.lux                编译并生成 ./hello\n"
        "  %s install mathx            从注册表安装 mathx\n"
        "  %s install mathx@1.0.0      安装指定版本\n"
        "  %s upgrade                  升级全部已安装包\n"
        "  %s login                    登录后 luxc publish 需要账号\n"
        "  %s publish ./mypkg          发布自己的包\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, lux::registryUrl().c_str(),
        argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

// 读密码（终端上关闭回显；非终端时直接读一行）
std::string promptPassword(const std::string& prompt) {
    std::fputs(prompt.c_str(), stderr);
    std::fflush(stderr);
    std::string line;
    termios oldt{};
    bool tty = isatty(fileno(stdin)) == 1;
    if (tty && tcgetattr(fileno(stdin), &oldt) == 0) {
        termios nt = oldt;
        nt.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(fileno(stdin), TCSANOW, &nt);
        std::getline(std::cin, line);
        tcsetattr(fileno(stdin), TCSANOW, &oldt);
        std::fputs("\n", stderr);
    } else {
        std::getline(std::cin, line);
    }
    return line;
}

// 包管理子命令： install / add / remove / list / search / info / update /
//                upgrade / publish / registry
int runPackageCmd(const std::string& sub, const std::vector<std::string>& rawArgs,
                  const char* argv0) {
    // 通用开关：--force
    bool force = false;
    std::vector<std::string> args;
    for (const std::string& a : rawArgs) {
        if (a == "--force" || a == "-f") {
            force = true;
        } else {
            args.push_back(a);
        }
    }

    if (sub == "add") {
        if (args.size() != 1) {
            std::fprintf(stderr, "用法: %s add <包目录 | .lux 文件>\n", argv0);
            return 2;
        }
        std::string name, err;
        if (!lux::pkgAdd(args[0], name, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已安装包 '%s' 到 %s/（现在可以 import \"%s\"）\n",
                    name.c_str(), lux::packagesDir().c_str(), name.c_str());
        return 0;
    }

    if (sub == "install" || sub == "get") {
        if (args.size() != 1) {
            std::fprintf(stderr,
                         "用法: %s install [--force] <名字[@版本] | URL | 本地路径>\n",
                         argv0);
            return 2;
        }
        std::string name, err;
        if (!lux::pkgInstall(args[0], force, err, name)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已安装包 '%s' 到 %s/（现在可以 import \"%s\"）\n",
                    name.c_str(), lux::packagesDir().c_str(), name.c_str());
        return 0;
    }

    if (sub == "list") {
        if (!args.empty()) {
            std::fprintf(stderr, "用法: %s list\n", argv0);
            return 2;
        }
        std::vector<lux::PkgInfo> pkgs = lux::pkgList();
        if (pkgs.empty()) {
            std::printf("还没有安装任何包（%s/ 为空）。\n"
                        "用 luxc install <名字> 从注册表安装，或 luxc add <本地路径>。\n",
                        lux::packagesDir().c_str());
            return 0;
        }
        std::printf("已安装的包（%s/）：\n\n", lux::packagesDir().c_str());
        std::printf("  %-18s %-10s %-20s %-8s %s\n", "包名", "版本", "入口文件",
                    "文件数", "来源");
        std::printf("  %-18s %-10s %-20s %-8s %s\n", "----", "----", "--------",
                    "------", "----");
        for (const lux::PkgInfo& p : pkgs) {
            std::string ver = p.version.empty() ? "-" : p.version;
            std::string main = p.main.empty() ? "-" : p.main;
            std::string src = p.source.empty() ? "local" : p.source;
            if (src == "local") src = "本地";
            else if (src == "registry") src = "注册表";
            else if (src == "url") src = "URL";
            std::printf("  %-18s %-10s %-20s %-8d %s\n", p.name.c_str(),
                        ver.c_str(), main.c_str(), p.files, src.c_str());
        }
        return 0;
    }

    if (sub == "delete" || sub == "remove" || sub == "rm") {
        if (args.size() != 1) {
            std::fprintf(stderr, "用法: %s %s <包名>\n", argv0, sub.c_str());
            return 2;
        }
        std::string err;
        if (!lux::pkgDelete(args[0], err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已删除包 '%s'\n", args[0].c_str());
        return 0;
    }

    if (sub == "search") {
        std::string query = args.empty() ? "" : args[0];
        std::vector<lux::PkgInfo> out;
        std::string err;
        if (!lux::pkgSearch(query, out, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        if (out.empty()) {
            std::printf("没有找到匹配的包。\n");
            return 0;
        }
        std::printf("注册表 %s 共 %zu 个包：\n\n", lux::registryUrl().c_str(),
                    out.size());
        std::printf("  %-18s %-10s %s\n", "包名", "最新版本", "简介");
        std::printf("  %-18s %-10s %s\n", "----", "--------", "----");
        for (const lux::PkgInfo& p : out) {
            std::printf("  %-18s %-10s %s\n", p.name.c_str(), p.version.c_str(),
                        p.main.c_str());
        }
        return 0;
    }

    if (sub == "info") {
        if (args.size() != 1) {
            std::fprintf(stderr, "用法: %s info <包名>\n", argv0);
            return 2;
        }
        std::string text, err;
        if (!lux::pkgInfo(args[0], text, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::fputs(text.c_str(), stdout);
        return 0;
    }

    if (sub == "update") {
        std::string err;
        if (!lux::pkgUpdateRegistry(err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已刷新注册表索引：%s\n", lux::registryUrl().c_str());
        return 0;
    }

    if (sub == "upgrade") {
        std::string target = args.empty() ? "" : args[0];
        std::string err;
        if (!lux::pkgUpgrade(target, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        return 0;
    }

    if (sub == "publish") {
        if (args.size() != 1) {
            std::fprintf(stderr, "用法: %s publish <包目录>\n", argv0);
            return 2;
        }
        std::string err;
        if (!lux::pkgPublish(args[0], err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        return 0;
    }

    if (sub == "registry") {
        if (args.empty()) {
            std::printf("%s\n", lux::registryUrl().c_str());
            return 0;
        }
        if (!lux::setRegistryUrl(args[0])) {
            std::fprintf(stderr, "错误: 无法写入配置 %s/config.json\n",
                         lux::luxHome().c_str());
            return 1;
        }
        std::printf("注册表已设为 %s\n", args[0].c_str());
        return 0;
    }

    if (sub == "login") {
        if (args.size() > 1) {
            std::fprintf(stderr, "用法: %s login [用户名]\n", argv0);
            return 2;
        }
        std::string user = args.empty() ? "" : args[0];
        if (user.empty()) {
            std::fprintf(stderr, "用户名: ");
            std::fflush(stderr);
            std::getline(std::cin, user);
        }
        std::string pass = promptPassword("密码: ");
        std::string name, err;
        if (!lux::luxLogin(user, pass, name, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已登录：%s（令牌已保存到 %s/config.json）\n", name.c_str(),
                    lux::luxHome().c_str());
        return 0;
    }

    if (sub == "logout") {
        std::string err;
        lux::luxLogout(err);
        std::printf("已退出登录\n");
        return 0;
    }

    if (sub == "whoami") {
        std::string name, err;
        if (!lux::luxWhoami(name, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("%s\n", name.c_str());
        return 0;
    }

    if (sub == "unpublish") {
        if (args.empty() || args.size() > 2) {
            std::fprintf(stderr, "用法: %s unpublish <包名> [版本]\n", argv0);
            return 2;
        }
        std::string version = args.size() > 1 ? args[1] : "";
        std::string err;
        if (!lux::pkgUnpublish(args[0], version, err)) {
            std::fprintf(stderr, "错误: %s\n", err.c_str());
            return 1;
        }
        std::printf("已从注册表删除 %s%s%s\n", args[0].c_str(),
                    version.empty() ? "" : "@", version.c_str());
        return 0;
    }

    std::fprintf(stderr, "错误: 无法识别的子命令 '%s'（试试 --help）\n",
                 sub.c_str());
    return 2;
}

// 自动挑选一个可用的 C 编译器
std::string pickCC() {
    for (const char* cand : {"cc", "clang", "gcc"}) {
        std::string cmd = std::string("command -v ") + cand +
                          " >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return cand;
    }
    return "cc";
}

// 把字符串转成可安全拼接进 shell 命令的形式
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

struct Options {
    std::string input;
    std::string output;
    std::string cOutput;
    bool emitC = false;
    bool keepC = false;
    bool native = false;  // 0.6：原生代码生成后端（直接输出 ELF，不走 C 编译器）
    bool arc = true;      // 0.9.4：ARC 默认开启（--no-arc 关闭）
    bool arcExplicit = false;  // 用户是否显式写过 --arc / --no-arc
    bool run = false;
    bool werror = false;
    bool optSpecified = false;  // 用户是否显式传过 -O（--native 下提示忽略）
    int optLevel = 2;
    std::string cc;
    std::vector<std::string> ccFlags;  // -C 透传给 C 编译器的额外选项
};

}  // namespace

// 编译流水线（定义在文件末尾；此处前置声明供 luxc build 复用）
int compilePipeline(Options& opt, const std::vector<std::string>& runArgs);

// =============================================================================
//  luxc build：项目构建（0.9.3）
//
//  luxc build [路径] [动作]
//    路径缺省为当前目录；在该目录（或该文件）里找 LuxBuildFile。
//    动作： build（缺省） / run / test / clean / rebuild / help
//
//  LuxBuildFile 是行式 "键 = 值" 配置（'#' 起注释，值可用引号包住空格）：
//    name      = myapp            项目名（缺省取目录名）
//    main      = src/main.lux     入口源文件（可用 src / source / entry 同义）
//    out       = build/myapp      输出可执行文件（缺省 build/<name>）
//    outdir    = build            输出目录（缺省 build）
//    backend   = c | native       后端（缺省 c）
//    arc       = true | false     引用计数内存回收（缺省 false）
//    opt       = 0..3             C 后端优化级别（缺省 2）
//    cc        = gcc              指定 C 编译器
//    cflags    = -DFOO -O3        额外传给 C 编译器的选项
//    run_args  = a b              运行时参数
//    testdir   = tests            测试目录（缺省 tests）
//    test      = tests/a.lux ...  测试文件 / 目录 / 通配符（可重复）
//    clean     = extra.tmp ...    额外清理项（可重复）
// =============================================================================
namespace {

struct BuildConf {
    std::string dir;                 // 构建文件所在目录
    std::string name;
    std::vector<std::string> srcs;
    std::string out;
    std::string outdir = "build";
    std::string backend = "c";
    bool arc = true;   // 0.9.4：默认开启；LuxBuildFile 里 arc = false 可关闭
    int opt = 2;
    std::string cc;
    std::vector<std::string> cflags;
    std::vector<std::string> runArgs;
    std::vector<std::string> tests;
    std::string testdir = "tests";
    std::vector<std::string> cleans;
};

std::string bTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string bDirName(const std::string& p) {
    size_t s = p.find_last_of('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return p.substr(0, s);
}

std::string bBaseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string bStem(const std::string& p) {
    std::string b = bBaseName(p);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

bool bIsFile(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool bIsDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 去掉行内注释（未被引号包住的 '#'）
std::string bStripComment(const std::string& s) {
    bool inS = false, inD = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && (inS || inD)) { i++; continue; }
        if (c == '\'' && !inD) inS = !inS;
        else if (c == '"' && !inS) inD = !inD;
        else if (c == '#' && !inS && !inD) return s.substr(0, i);
    }
    return s;
}

// 按空白切分（尊重引号），并去掉引号
std::vector<std::string> bSplitTokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inS = false, inD = false;
    bool has = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && (inS || inD) && i + 1 < s.size()) {
            cur += s[++i];
            has = true;
            continue;
        }
        if (c == '\'' && !inD) { inS = !inS; has = true; continue; }
        if (c == '"' && !inS) { inD = !inD; has = true; continue; }
        if (!inS && !inD && (c == ' ' || c == '\t')) {
            if (has) { out.push_back(cur); cur.clear(); has = false; }
            continue;
        }
        cur += c;
        has = true;
    }
    if (has) out.push_back(cur);
    return out;
}

// 简单通配符匹配（* / ?）
bool bWild(const char* p, const char* s) {
    if (*p == '\0') return *s == '\0';
    if (*p == '*') return bWild(p + 1, s) || (*s && bWild(p, s + 1));
    if (*p == '?') return *s && bWild(p + 1, s + 1);
    return *p == *s && bWild(p + 1, s + 1);
}

// 把测试项展开成具体的 .lux 文件列表
std::vector<std::string> bExpandTests(const std::string& item) {
    std::vector<std::string> out;
    std::string pattern = item;
    if (bIsDir(pattern)) pattern += "/*.lux";
    if (pattern.find('*') == std::string::npos &&
        pattern.find('?') == std::string::npos) {
        if (bIsFile(pattern)) out.push_back(pattern);
        return out;
    }
    std::string dir = bDirName(pattern);
    std::string base = bBaseName(pattern);
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string nm = e->d_name;
        if (nm == "." || nm == "..") continue;
        if (bWild(base.c_str(), nm.c_str()))
            out.push_back(dir == "." ? nm : dir + "/" + nm);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool bReadBuildFile(const std::string& path, BuildConf& bf, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "无法读取构建文件 '" + path + "'";
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        std::string t = bTrim(bStripComment(line));
        if (t.empty()) continue;
        size_t eq = t.find('=');
        size_t colon = t.find(':');
        size_t sep = std::string::npos;
        if (eq != std::string::npos &&
            (colon == std::string::npos || eq < colon))
            sep = eq;
        else if (colon != std::string::npos)
            sep = colon;
        if (sep == std::string::npos) {
            err = path + ":" + std::to_string(lineNo) +
                  ": 这一行不是 '键 = 值' 形式";
            return false;
        }
        std::string key = bTrim(t.substr(0, sep));
        std::string val = bTrim(t.substr(sep + 1));
        std::vector<std::string> toks = bSplitTokens(val);
        auto one = [&]() -> std::string {
            return toks.empty() ? "" : toks[0];
        };
        if (key == "name") bf.name = one();
        else if (key == "main" || key == "src" || key == "source" ||
                 key == "entry")
            for (auto& s : toks) bf.srcs.push_back(s);
        else if (key == "out" || key == "output") bf.out = one();
        else if (key == "outdir" || key == "builddir") bf.outdir = one();
        else if (key == "backend") bf.backend = one();
        else if (key == "arc") bf.arc = (one() == "true" || one() == "1" ||
                                          one() == "yes" || one() == "on");
        else if (key == "opt") bf.opt = std::atoi(one().c_str());
        else if (key == "cc") bf.cc = one();
        else if (key == "cflags" || key == "ccflags" || key == "flags")
            for (auto& s : toks) bf.cflags.push_back(s);
        else if (key == "run_args" || key == "args")
            for (auto& s : toks) bf.runArgs.push_back(s);
        else if (key == "test") for (auto& s : toks) bf.tests.push_back(s);
        else if (key == "testdir") bf.testdir = one();
        else if (key == "clean") for (auto& s : toks) bf.cleans.push_back(s);
        else {
            err = path + ":" + std::to_string(lineNo) + ": 未知的键 '" +
                  key + "'";
            return false;
        }
    }
    if (bf.name.empty()) bf.name = bBaseName(bf.dir);
    return true;
}

std::string bReadAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
}

std::string bRstripNl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

const char* kBuildCandidates[] = {"LuxBuildFile", "LuxBuildFile.lux",
                                  "LuxBulidFile", "lux.build", "LuxBuild"};

// system() 的薄包装：避免 warn_unused_result（构建里有些调用不需要返回值）
int bSys(const std::string& cmd) { return std::system(cmd.c_str()); }

int runBuildCommand(const std::string& pathArg, const std::string& actionIn,
                    const char* argv0) {
    std::string action = actionIn.empty() ? "build" : actionIn;
    if (action == "help" || action == "-h" || action == "--help") {
        std::printf("用法: %s build [路径] [build|run|test|clean|rebuild]\n",
                    argv0);
        std::printf("  在路径（缺省当前目录）下读取 LuxBuildFile 并按配置构建。\n"
                    "  LuxBuildFile 是 '键 = 值' 配置，键见 README / docs。\n");
        return 0;
    }
    std::string buildFile;
    std::string dir;
    if (!pathArg.empty() && bIsFile(pathArg)) {
        buildFile = pathArg;
        dir = bDirName(pathArg);
    } else {
        dir = pathArg.empty() ? "." : pathArg;
        for (const char* cand : kBuildCandidates) {
            std::string p = (dir == "." ? std::string(cand)
                                         : dir + "/" + cand);
            if (bIsFile(p)) {
                buildFile = p;
                break;
            }
        }
        if (buildFile.empty()) {
            std::fprintf(stderr,
                         "错误: 在 '%s' 里找不到 LuxBuildFile\n\n"
                         "LuxBuildFile 示例：\n"
                         "  name    = myapp\n"
                         "  main    = src/main.lux\n"
                         "  out     = build/myapp\n"
                         "  backend = c\n"
                         "  arc     = true\n"
                         "  testdir = tests\n\n"
                         "然后： %s build %s\n",
                         dir.c_str(), argv0, dir.c_str());
            return 2;
        }
    }

    BuildConf bf;
    bf.dir = dir;
    std::string err;
    if (!bReadBuildFile(buildFile, bf, err)) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 1;
    }
    // 相对路径一律相对构建文件所在目录解析
    auto rel = [&](const std::string& p) {
        if (p.empty() || p[0] == '/') return p;
        return bf.dir == "." ? p : bf.dir + "/" + p;
    };

    if (action == "help" || action == "-h" || action == "--help") {
        std::printf("用法: %s build [路径] [build|run|test|clean|rebuild]\n",
                    argv0);
        std::printf("  在路径下读取 LuxBuildFile 并按配置构建。\n");
        return 0;
    }

    std::string outdir = rel(bf.outdir);
    if (action == "clean") {
        std::string cmd = "rm -rf " + shellQuote(outdir);
        if (!bf.out.empty()) cmd += " " + shellQuote(rel(bf.out));
        for (auto& c : bf.cleans) cmd += " " + shellQuote(rel(c));
        int rc = bSys(cmd.c_str());
        std::printf("已清理 %s%s\n", outdir.c_str(),
                    rc == 0 ? "" : "（部分失败）");
        return 0;
    }

    // 确保输出目录存在
    (void)bSys(("mkdir -p " + shellQuote(outdir)).c_str());

    if (bf.srcs.empty()) {
        std::fprintf(stderr,
                     "错误: %s 里没有配置入口源文件（main / src）\n",
                     buildFile.c_str());
        return 1;
    }
    if (bf.backend != "c" && bf.backend != "native") {
        std::fprintf(stderr, "错误: backend 只能是 c 或 native，收到 '%s'\n",
                     bf.backend.c_str());
        return 1;
    }
    if (bf.srcs.size() > 1 && !bf.out.empty()) {
        std::fprintf(stderr,
                     "错误: 配置了多个 main/src 时不能再指定单一的 out；"
                     "请去掉 out，让每个源文件输出到 outdir/<文件名>\n");
        return 1;
    }

    int failures = 0;
    std::vector<std::string> built;  // 构建出的可执行文件
    for (const std::string& srcRel : bf.srcs) {
        std::string src = rel(srcRel);
        std::string out = bf.out.empty()
                              ? outdir + "/" + bStem(srcRel)
                              : rel(bf.out);
        Options o;
        o.input = src;
        o.output = out;
        o.native = (bf.backend == "native");
        o.arc = bf.arc;
        o.optLevel = bf.opt;
        o.optSpecified = true;
        o.cc = bf.cc;
        o.ccFlags = bf.cflags;
        std::fprintf(stderr, "[build] %s -> %s\n", src.c_str(), out.c_str());
        int rc = compilePipeline(o, {});
        if (rc != 0) {
            std::fprintf(stderr, "[build] 失败: %s\n", src.c_str());
            failures++;
        } else {
            built.push_back(out);
        }
    }
    if (failures > 0) return 1;

    if (action == "run") {
        if (built.empty()) return 1;
        std::string cmd = built[0];
        if (cmd.find('/') == std::string::npos) cmd = "./" + cmd;
        for (const std::string& a : bf.runArgs) cmd += " " + shellQuote(a);
        return (bSys(cmd.c_str()) >> 8) & 0xFF;
    }

    if (action == "test") {
        std::vector<std::string> items = bf.tests;
        if (items.empty()) items.push_back(bf.testdir + "/*.lux");
        std::vector<std::string> files;
        for (auto& it : items) {
            std::vector<std::string> ex = bExpandTests(rel(it));
            files.insert(files.end(), ex.begin(), ex.end());
        }
        if (files.empty()) {
            std::fprintf(stderr, "[test] 没有找到测试文件（test/testdir）\n");
            return 1;
        }
        int pass = 0, fail = 0;
        std::string tdir = "/tmp/luxbuild-test-" + bf.name;
        (void)bSys(("rm -rf " + shellQuote(tdir) + " && mkdir -p " +
                     shellQuote(tdir))
                        .c_str());
        for (const std::string& tf : files) {
            std::string stem = bStem(tf);
            std::string bin = tdir + "/" + stem;
            Options o;
            o.input = tf;
            o.output = bin;
            o.native = (bf.backend == "native");
            o.arc = bf.arc;
            o.optLevel = bf.opt;
            o.cc = bf.cc;
            o.ccFlags = bf.cflags;
            int rc = compilePipeline(o, {});
            if (rc != 0) {
                std::printf("  FAIL %s（编译失败）\n", tf.c_str());
                fail++;
                continue;
            }
            std::string args;
            std::string argsFile = tf.substr(0, tf.find_last_of('.')) + ".args";
            if (bIsFile(argsFile))
                for (auto& a : bSplitTokens(bReadAll(argsFile)))
                    args += " " + shellQuote(a);
            std::string outFile = tdir + "/" + stem + ".out";
            std::string cmd = bin;
            if (cmd.find('/') == std::string::npos) cmd = "./" + cmd;
            cmd += args + " > " + shellQuote(outFile) + " 2>&1";
            (void)bSys(cmd.c_str());
            std::string got = bRstripNl(bReadAll(outFile));
            std::string expFile =
                tf.substr(0, tf.find_last_of('.')) + ".expected";
            if (bIsFile(expFile)) {
                std::string exp = bRstripNl(bReadAll(expFile));
                if (got == exp) {
                    std::printf("  ok   %s\n", tf.c_str());
                    pass++;
                } else {
                    std::printf("  FAIL %s（输出不一致）\n", tf.c_str());
                    fail++;
                }
            } else {
                std::printf("  ok   %s（无 .expected，仅运行）\n", tf.c_str());
                pass++;
            }
        }
        (void)bSys(("rm -rf " + shellQuote(tdir)).c_str());
        std::printf("[test] %d 通过，%d 失败\n", pass, fail);
        return fail == 0 ? 0 : 1;
    }

    if (action == "rebuild") {
        std::string cmd = "rm -rf " + shellQuote(outdir);
        (void)bSys(cmd.c_str());
        (void)bSys(("mkdir -p " + shellQuote(outdir)).c_str());
        // 递归重建
        return runBuildCommand(pathArg, "build", argv0);
    }

    if (action != "build" && action != "all") {
        std::fprintf(stderr, "错误: 未知的构建动作 '%s'（build/run/test/clean/rebuild）\n",
                     action.c_str());
        return 2;
    }

    std::printf("[build] 完成：%s（%zu 个可执行文件）\n", bf.name.c_str(),
                built.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // ---------------- 包管理 / REPL 子命令 ----------------
    if (argc >= 2) {
        std::string first = argv[1];
        if (first == "repl") {
            return lux::runRepl();
        }
        if (first == "build") {
            static const std::vector<std::string> actions = {
                "build", "run",   "test", "clean",
                "rebuild", "all", "help", "-h", "--help"};
            std::string path, action;
            for (int i = 2; i < argc; i++) {
                std::string a = argv[i];
                bool isAction = false;
                for (const std::string& x : actions)
                    if (a == x) isAction = true;
                if (isAction)
                    action = a;
                else if (path.empty())
                    path = a;
                else {
                    std::fprintf(stderr,
                                 "错误: build 只接受一个路径（收到 '%s'）\n",
                                 a.c_str());
                    return 2;
                }
            }
            return runBuildCommand(path, action, argv[0]);
        }
        if (first == "add" || first == "install" || first == "get" ||
            first == "list" || first == "delete" || first == "remove" ||
            first == "rm" || first == "search" || first == "info" ||
            first == "update" || first == "upgrade" || first == "publish" ||
            first == "unpublish" || first == "registry" || first == "login" ||
            first == "logout" || first == "whoami") {
            std::vector<std::string> args;
            for (int i = 2; i < argc; i++) args.emplace_back(argv[i]);
            return runPackageCmd(first, args, argv[0]);
        }
    }

    Options opt;
    std::vector<std::string> runArgs;  // 传给被运行程序的参数

    // ---------------- 命令行解析 ----------------
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: 选项 %s 缺少参数 %s\n", a.c_str(),
                             what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            printHelp(argv[0]);
            return 0;
        } else if (a == "-v" || a == "--version") {
            std::printf("%s\n", kVersion.c_str());
            return 0;
        } else if (a == "-o") {
            opt.output = next("<输出文件>");
        } else if (a == "-c") {
            opt.cOutput = next("<C 文件>");
        } else if (a == "--emit-c") {
            opt.emitC = true;
        } else if (a == "--keep-c") {
            opt.keepC = true;
        } else if (a == "--native") {
            opt.native = true;
        } else if (a == "--arc") {
            opt.arc = true;   // 0.9.4：ARC 已是默认，显式给出仅作兼容
            opt.arcExplicit = true;
        } else if (a == "--no-arc") {
            opt.arc = false;  // 0.9.4：关闭引用计数，退回 0.9.1 的只增不减语义
            opt.arcExplicit = true;
        } else if (a == "--run") {
            opt.run = true;
        } else if (a == "-Werror") {
            opt.werror = true;
        } else if (a == "-O") {
            std::string lvl = next("<优化级别>");
            if (lvl.size() != 1 || lvl[0] < '0' || lvl[0] > '3') {
                std::fprintf(stderr, "错误: -O 的取值只能是 0~3，收到 '%s'\n",
                             lvl.c_str());
                return 2;
            }
            opt.optLevel = lvl[0] - '0';
            opt.optSpecified = true;
        } else if (a.rfind("-O", 0) == 0 && a.size() == 3 &&
                   a[2] >= '0' && a[2] <= '3') {
            opt.optLevel = a[2] - '0';  // 兼容 -O2 写法
            opt.optSpecified = true;
        } else if (a == "--cc") {
            opt.cc = next("<C 编译器>");
        } else if (a == "-C") {
            opt.ccFlags.push_back(next("<C 编译器选项>"));
        } else if (a == "--no-color") {
            lux::setColorEnabled(false);
        } else if (a == "--") {
            for (int k = i + 1; k < argc; k++) runArgs.emplace_back(argv[k]);
            break;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "错误: 无法识别的选项 '%s'（试试 --help）\n",
                         a.c_str());
            return 2;
        } else {
            if (!opt.input.empty()) {
                std::fprintf(stderr, "错误: 一次只能编译一个源文件\n");
                return 2;
            }
            opt.input = a;
        }
    }

    if (opt.input.empty()) {
        printHelp(argv[0]);
        return 2;
    }

    return compilePipeline(opt, runArgs);
}

// ---------------- 编译流水线（主命令与 luxc build 共用） ----------------
int compilePipeline(Options& opt, const std::vector<std::string>& runArgs) {
    lux::setColorEnabled(isatty(fileno(stderr)) == 1);

    if (opt.cc.empty()) opt.cc = pickCC();

    // ---------------- 编译流水线 ----------------
    // 加载根文件及其全部 import；AST 由 loader 内部的 Parser 对象池持有。
    // 词法 / 语法 / 语义 / 模块错误全部经 Diags 收集，一次报全。
    lux::Diags diags;
    lux::ModuleLoader loader(diags);
    lux::Program* prog = loader.load(opt.input);
    bool ok = false;
    if (prog && diags.ok()) {
        ok = lux::analyze(prog, diags, opt.input, loader.modules);
    }
    diags.flush(loader.sources);
    if (!diags.ok() || !ok) return 1;
    if (opt.werror && diags.warnings > 0) {
        std::fprintf(stderr, "已开启 -Werror：存在警告，停止编译。\n");
        return 1;
    }

    // ---------------- 原生后端：本机机器码直出 ELF ----------------
    if (opt.native) {
        // 0.9.4：原生后端也支持 ARC（尺寸分级 free list + 引用计数）
        lux::setNativeArc(opt.arc);
        if (opt.optSpecified)
            std::fprintf(stderr,
                         "提示: --native 后端暂不支持优化，-O%d 已忽略\n",
                         opt.optLevel);
        std::vector<uint8_t> elf;
        if (!lux::generateNative(prog, diags, opt.input, elf)) {
            diags.flush(loader.sources);
            return 1;
        }
        diags.flush(loader.sources);
        std::string outExe =
            opt.output.empty() ? lux::stemOf(opt.input) : opt.output;
        if (!lux::writeFile(outExe,
                            std::string(elf.begin(), elf.end()))) {
            std::fprintf(stderr, "错误: 无法写入输出文件 '%s'\n",
                         outExe.c_str());
            return 1;
        }
        if (bSys(("chmod 755 " + shellQuote(outExe)).c_str()) != 0) {
            std::fprintf(stderr, "错误: 无法设置可执行权限 '%s'\n",
                         outExe.c_str());
            return 1;
        }
        std::fprintf(stderr, "  已生成原生可执行文件: %s\n", outExe.c_str());
        if (opt.run) {
            std::string runCmd = outExe.find('/') == std::string::npos
                                     ? ("./" + outExe)
                                     : outExe;
            for (const std::string& a : runArgs) runCmd += " " + shellQuote(a);
            int rrc = bSys(runCmd.c_str());
            if (rrc != 0) return (rrc >> 8) & 0xFF;
        }
        return 0;
    }

    lux::CodegenOptions copt;
    copt.sourceName = opt.input;
    copt.emitLineMarks = true;
    copt.arc = opt.arc;
    std::string cCode = lux::generateC(prog, copt);
    if (cCode.empty()) {
        std::fprintf(stderr, "编译器内部错误: 代码生成失败\n");
        return 3;
    }

    // ---------------- 输出 C 代码 ----------------
    if (opt.emitC) {
        if (!opt.output.empty()) {
            if (!lux::writeFile(opt.output, cCode)) {
                std::fprintf(stderr, "错误: 无法写入文件 '%s'\n", opt.output.c_str());
                return 1;
            }
            return 0;
        }
        std::fwrite(cCode.data(), 1, cCode.size(), stdout);
        return 0;
    }

    // ---------------- 写 C 文件并调用 C 编译器 ----------------
    std::string outExe = opt.output.empty() ? lux::stemOf(opt.input) : opt.output;
    std::string cFile;
    bool cFileIsTemp = false;

    if (!opt.cOutput.empty()) {
        cFile = opt.cOutput;
    } else if (opt.keepC) {
        cFile = outExe + ".c";
    } else {
        // 默认把中间 .c 写到系统临时目录（并发编译 / 只读源码目录也安全）
        const char* td = std::getenv("TMPDIR");
        std::string dir = (td && *td) ? td : "/tmp";
        // mkstemp 要求模板以 XXXXXX 结尾，所以先建无后缀文件再改名 .c
        std::string tmpl = dir + "/lux-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        if (fd < 0) {
            std::fprintf(stderr, "错误: 无法在临时目录创建中间文件\n");
            return 1;
        }
        close(fd);
        cFile = std::string(buf.data()) + ".c";
        if (rename(buf.data(), cFile.c_str()) != 0) {
            std::remove(buf.data());
            std::fprintf(stderr, "错误: 无法在临时目录创建中间文件\n");
            return 1;
        }
        cFileIsTemp = true;
    }

    if (!lux::writeFile(cFile, cCode)) {
        std::fprintf(stderr, "错误: 无法写入 C 中间文件 '%s'\n", cFile.c_str());
        return 1;
    }

    std::string cmd = opt.cc;
    cmd += " -std=c17 -O" + std::to_string(opt.optLevel);
    // 运行时函数是 static 的，未用到的会触发 -Wunused-function，这里统一关掉
    cmd += " -Wno-unused-function";
    for (const std::string& f : opt.ccFlags) {
        cmd += " " + f;  // -C 透传的选项（引号由用户负责）
    }
    cmd += " -o " + shellQuote(outExe);
    cmd += " " + shellQuote(cFile);
    cmd += " -lm";  // 数学库（sqrt / fabs）
    // import "c:库名" 引入的额外链接库
    for (const std::string& lib : loader.linkLibs) {
        cmd += " -l" + lib;
    }

    int rc = bSys(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "错误: C 编译器执行失败（命令: %s）\n", cmd.c_str());
        return 1;
    }

    if (cFileIsTemp) std::remove(cFile.c_str());

    // ---------------- 运行 ----------------
    if (opt.run) {
        std::string runCmd = outExe.find('/') == std::string::npos
                                 ? ("./" + outExe)
                                 : outExe;
        for (const std::string& a : runArgs) runCmd += " " + shellQuote(a);
        int rrc = bSys(runCmd.c_str());
        if (rrc != 0) return (rrc >> 8) & 0xFF;
    }

    return 0;
}
