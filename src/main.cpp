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
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// 版本号由 Makefile 经 -DLUX_VERSION='"x.y.z"' 编译期注入（单源化）
#ifndef LUX_VERSION
#define LUX_VERSION "0.7.0"
#endif

const std::string kVersion = std::string("Lux ") + LUX_VERSION + " (C 后端 + 原生后端)";

void printHelp(const char* argv0) {
    std::printf(
        "用法:\n"
        "  %s <源文件.lux> [选项]          把 Lux 源码编译成本地可执行文件\n"
        "  %s repl                        交互式执行环境（REPL）\n"
        "  %s add <包目录 | .lux 文件>     安装包到 ~/.lux/packages/\n"
        "  %s list                        列出已安装的包\n"
        "  %s delete <包名>               删除已安装的包\n"
        "\n"
        "编译选项:\n"
        "  -o <文件>        指定输出的可执行文件名（默认取源文件名去掉扩展名）\n"
        "  -c <文件>        把生成的 C 代码写到指定文件\n"
        "  --emit-c         只把生成的 C 代码输出到标准输出，不调用 C 编译器\n"
        "  --keep-c         编译后把生成的 .c 文件保留在可执行文件旁\n"
        "  --native         原生代码生成后端：直接输出本机 ELF，不依赖 C 编译器\n"
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
        "  %s hello.lux --run          编译后立即运行\n"
        "  %s hello.lux --emit-c -o out.c   只生成 C 代码\n"
        "  %s add ./mypkg              安装包后即可 import \"mypkg\"\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

// 包管理子命令： add / list / delete
int runPackageCmd(const std::string& sub, const std::vector<std::string>& args,
                  const char* argv0) {
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

    if (sub == "list") {
        if (!args.empty()) {
            std::fprintf(stderr, "用法: %s list\n", argv0);
            return 2;
        }
        std::vector<lux::PkgInfo> pkgs = lux::pkgList();
        if (pkgs.empty()) {
            std::printf("还没有安装任何包（%s/ 为空）。\n"
                        "用 luxc add <包目录|.lux 文件> 安装。\n",
                        lux::packagesDir().c_str());
            return 0;
        }
        std::printf("已安装的包（%s/）：\n\n", lux::packagesDir().c_str());
        std::printf("  %-20s %-24s %s\n", "包名", "入口文件", "文件数");
        std::printf("  %-20s %-24s %s\n", "----", "--------", "------");
        for (const lux::PkgInfo& p : pkgs) {
            std::string main = p.main.empty() ? "-" : p.main;
            std::printf("  %-20s %-24s %d 个 .lux\n", p.name.c_str(),
                        main.c_str(), p.files);
        }
        return 0;
    }

    if (sub == "delete") {
        if (args.size() != 1) {
            std::fprintf(stderr, "用法: %s delete <包名>\n", argv0);
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
    bool run = false;
    bool werror = false;
    bool optSpecified = false;  // 用户是否显式传过 -O（--native 下提示忽略）
    int optLevel = 2;
    std::string cc;
    std::vector<std::string> ccFlags;  // -C 透传给 C 编译器的额外选项
};

}  // namespace

int main(int argc, char** argv) {
    // ---------------- 包管理 / REPL 子命令 ----------------
    if (argc >= 2) {
        std::string first = argv[1];
        if (first == "repl") {
            return lux::runRepl();
        }
        if (first == "add" || first == "list" || first == "delete") {
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

    // ---------------- 原生后端：x86-64 机器码直出 ELF ----------------
    if (opt.native) {
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
        if (std::system(("chmod 755 " + shellQuote(outExe)).c_str()) != 0) {
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
            int rrc = std::system(runCmd.c_str());
            if (rrc != 0) return (rrc >> 8) & 0xFF;
        }
        return 0;
    }

    lux::CodegenOptions copt;
    copt.sourceName = opt.input;
    copt.emitLineMarks = true;
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

    int rc = std::system(cmd.c_str());
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
        int rrc = std::system(runCmd.c_str());
        if (rrc != 0) return (rrc >> 8) & 0xFF;
    }

    return 0;
}
