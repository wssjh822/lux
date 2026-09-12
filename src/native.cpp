// =============================================================================
//  Lux 原生代码生成后端（0.6）— 编译入口
//  机器码发射与 ELF 布局见 native_emit.hpp；生成逻辑见 native_body.inc。
//
//  调用约定：参数走栈、从左到右求值依次压栈（每参数 8 字节，float 为 IEEE
//  位模式），被调方参数 j = [rbp + 16 + 8*(n-1-j)]；返回值 rax（整型/指针）
//  或 xmm0（float）。所有值在栈上统一占 8 字节槽。
//  值表示：int/bool 补码；float 位模式；string → {i64 len; u8 data[]}；
//  数组 → {i64 len, cap, data, ekind}（ekind: 0=int 1=float 2=bool 3=str 4=arr）。
//  运行时库由 Lux 语言自身编写（native_rt.lux，构建期嵌入为
//  native_rt_embed.h），经同一发射器编译后与用户程序共享符号表。
// =============================================================================

#include "lux.hpp"
#include "native_emit.hpp"
#include "native_rt_embed.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace lux {
namespace {

#include "native_body.inc"

}  // namespace

#if defined(__aarch64__)
// aarch64 的 Linux 系统调用号与 x86-64 不同。运行时库源码统一按 x86-64 编号
// 书写直接调用的系统调用，这里在编译运行时库前把相关全局常量改写为目标架构
// 的编号（open/fork/unlink/rename 这类结构性差异已由 __sys_* 内建吸收）。
static void patchArm64SyscallNumbers(Program* rt) {
    struct { const char* name; long long val; } mapping[] = {
        {"kSysRead",          63},
        {"kSysWrite",         64},
        {"kSysClose",         57},
        {"kSysFstat",         80},
        {"kSysLseek",         62},
        {"kSysNanosleep",    101},
        {"kSysExecve",       221},
        {"kSysWait4",        260},
        {"kSysClockGettime", 113},
        {"kSysExitGroup",     94},
    };
    for (GlobalConstDecl* gc : rt->consts) {
        for (const auto& m : mapping) {
            if (gc->name == m.name && gc->init &&
                gc->init->kind == ExprKind::IntLit) {
                static_cast<IntLitExpr*>(gc->init)->value = m.val;
            }
        }
    }
}
#endif

// 生成 x86-64 Linux ELF（不依赖 C 编译器 / libc）。见 lux.hpp 接口说明。
bool generateNative(Program* prog, Diags& diags, const std::string& sourceName,
                    std::vector<uint8_t>& outElf) {
    NativeGen g;
    g.diags = &diags;

    // 1. 编译运行时库（Lux 自身编写，随编译器发布，理论不可失败）
    {
        Diags rtDiags;
        auto toks = tokenize(kRuntimeLuxSrc, "native_rt.lux", rtDiags);
        Parser p(std::move(toks), "native_rt.lux", rtDiags);
        Program* rt = p.parseProgram();
        ModuleInfo mi;
        if (!rtDiags.ok() || !analyze(rt, rtDiags, "native_rt.lux", mi, false)) {
            rtDiags.flush({});  // 运行时库理论上不可失败，失败时先打印自身诊断
            diags.error(DiagCode::kSema, sourceName, SourceLoc{},
                        "内部错误：原生运行时库编译失败（这是编译器缺陷，"
                        "请附带源码报告）");
            return false;
        }
        g.inRuntime = true;
#if defined(__aarch64__)
        patchArm64SyscallNumbers(rt);
#endif
        g.genProgram(rt, true);
    }

    // 2. 原生后端不支持 extern fn（C 互操作是 C 后端能力）
    for (FuncDecl* fn : prog->funcs) {
        if (fn->isExtern) {
            diags.error(DiagCode::kSema,
                        fn->file.empty() ? sourceName : fn->file, fn->nameLoc,
                        "原生后端不支持 extern fn '" + fn->name +
                            "'（C 互操作请使用 C 后端，系统调用可用 __syscall）");
        }
    }
    if (!diags.ok()) return false;

    // 3. 编译用户程序（与运行时共享同一发射器与符号表）
    g.inRuntime = false;
    g.genProgram(prog, false);

    // 4. 入口桩
    g.entryOff = g.c.code.size();
    g.genStart();

    // 5. 数据段布局与重定位回填：code 从 128 起，data 16 对齐紧随其后
    size_t dataSegOff = (128 + g.c.code.size() + 15) & ~(size_t)15;
    g.c.resolve(g.data, dataSegOff);
    outElf = buildElf(g.c, g.data, g.entryOff, dataSegOff);
    return diags.ok();
}

}  // namespace lux
