// =============================================================================
//  Lux 原生后端 — x64 机器码发射器与 ELF64 输出（由 native.cpp 使用）
//  指令子集按 native.cpp 的生成需求定制；标签经 rel32 / 数据地址 imm64 回填。
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <initializer_list>

namespace lux {
namespace {

// =============================================================================
//  两种架构共用的 ELF 支撑（状态槽 + 小端写整数）
// =============================================================================

// 运行时全局状态槽（bss 固定地址，零初始化，见 buildElf 的 kMinMemSz）
constexpr uint64_t kSlotEnv = 0x600000;   // setenv 列表（数组指针）
constexpr uint64_t kSlotRand = 0x600008;  // xorshift64 随机状态
constexpr uint64_t kSlotHeap = 0x600010;  // bump 分配器 cur / [槽+8] end
constexpr uint64_t kSlotInitStack = 0x600058;  // _start 快照的初始 rsp
                                               // （[sp]=argc, argv…, NULL, envp…）

static void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    for (int i = 0; i < 2; i++) v[at + i] = (uint8_t)((x >> (8 * i)) & 0xFF);
}
static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)((x >> (8 * i)) & 0xFF);
}
static void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; i++) v[at + i] = (uint8_t)((x >> (8 * i)) & 0xFF);
}

#if defined(__aarch64__)
#include "native_emit_arm64.hpp"
using NativeArch = Arm64;
#else

// =============================================================================
//  x64 机器码发射器
// =============================================================================

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7 };

struct Fixup {
    size_t at;   // rel32 / imm64 字段位置
    int label;   // >= 0 标签；< 0 数据引用（dataRefs_ 的相反数-1）
};

// 条件码
enum CC { CJo = 0, CJno = 1, CJb = 2, CJae = 3, CJe = 4, CJne = 5,
          CJbe = 6, CJa = 7, CJs = 8, CJns = 9, CJp = 10, CJnp = 11,
          CJl = 12, CJge = 13, CJle = 14, CJg = 15 };

// 代码段基址（resolve / buildElf 共用；前置声明避免类内引用未声明名）
constexpr uint64_t kBase = 0x400000;

struct X64 {
    std::vector<uint8_t> code;
    std::vector<Fixup> fixups;
    std::vector<long long> labels;

    int newLabel() { labels.push_back(-1); return (int)labels.size() - 1; }
    void bind(int l) {
        if (labels[l] >= 0) {
            std::fprintf(stderr, "编译器内部错误: 标签 %d 重复绑定\n", l);
            std::exit(3);
        }
        labels[l] = (long long)code.size();
    }
    void db(int b) { code.push_back((uint8_t)b); }
    void dbs(std::initializer_list<int> bs) { for (int b : bs) db(b); }
    void dd(uint32_t v) { for (int i = 0; i < 4; i++) db((v >> (8 * i)) & 0xFF); }
    void dq(uint64_t v) { for (int i = 0; i < 8; i++) db((v >> (8 * i)) & 0xFF); }
    void fixLabel(int label) { fixups.push_back({code.size(), label}); dd(0); }
    void fixData(size_t dataOff) {  // imm64 = 数据绝对地址（布局后回填）
        fixups.push_back({code.size(), (int)(-1 - (long long)dataOff)});
        dq(0);
    }
    void resolve(const std::vector<uint8_t>& data, size_t dataSegOff) {
        bool dbg = getenv("LUX_FIXDUMP") != nullptr;
        for (const Fixup& f : fixups) {
            if (dbg)
                std::fprintf(stderr, "FIXUP at=0x%zx label=%d target=0x%llx prev=%02x %02x\n",
                             f.at, f.label,
                             f.label >= 0 ? (unsigned long long)labels[f.label]
                                          : 0ULL,
                             f.at >= 2 ? code[f.at - 2] : 0,
                             f.at >= 1 ? code[f.at - 1] : 0);
            if (f.label < 0) {
                // 数据引用：绝对地址
                size_t doff = (size_t)(-1 - (long long)f.label);
                uint64_t addr = kBase + dataSegOff + doff;
                for (int i = 0; i < 8; i++)
                    code[f.at + i] = (uint8_t)((addr >> (8 * i)) & 0xFF);
                continue;
            }
            long long target = labels[f.label];
            if (target < 0) {
                std::fprintf(stderr, "编译器内部错误: 未绑定的标签 %d\n", f.label);
                std::exit(3);
            }
            int32_t rel = (int32_t)(target - (long long)(f.at + 4));
            for (int i = 0; i < 4; i++)
                code[f.at + i] = (uint8_t)((rel >> (8 * i)) & 0xFF);
        }
        fixups.clear();
    }

    // ---- 前缀 / modrm ----
    void rexAny(int w, int r, int x, int b) {
        int v = 0x40 | (w << 3) | (r << 2) | (x << 1) | b;
        if (v != 0x40) db(v);
    }
    int pickMod(int base, int32_t disp) {
        if (base == RBP || base == 13) return 1;
        if (disp == 0) return 0;
        return 1;
    }
    void emitMemOperand(int mod, int reg, int base, int32_t disp) {
        if (base == RSP || base == 12) {
            db((mod << 6) | ((reg & 7) << 3) | 4);
            int m = mod;
            db(0x24);  // SIB: scale=0 index=none base=rsp
            (void)m;
            if (mod == 1) db((int8_t)disp);
            else if (mod == 2) dd((uint32_t)disp);
        } else {
            int m = mod;
            if (mod == 2 && disp >= -128 && disp <= 127) m = 1;
            db((m << 6) | ((reg & 7) << 3) | (base & 7));
            if (m == 1) db((int8_t)disp);
            else if (m == 2) dd((uint32_t)disp);
        }
    }
    // "op reg, [base+disp]" 或 "op [base+disp], reg"（w=1 写 64 位）
    void mem(int op, int reg, int base, int32_t disp, bool w = true) {
        int mod = pickMod(base, disp);
        if (mod == 0 && disp != 0) mod = 1;
        int m = (mod == 0) ? 0 : ((disp >= -128 && disp <= 127) ? 1 : 2);
        if (base == RBP || base == 13) m = (disp >= -128 && disp <= 127) ? 1 : 2;
        rexAny(w ? 1 : 0, (reg >= 8) ? 1 : 0, 0, (base >= 8) ? 1 : 0);
        db(op);
        emitMemOperand(m, reg, base, disp);
    }

    // ---- 数据移动 ----
    void push(int r) { if (r >= 8) db(0x41); db(0x50 + (r & 7)); }
    void pop(int r) { if (r >= 8) db(0x41); db(0x58 + (r & 7)); }
    void mov64i(int r, int64_t v) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        if (v >= -2147483648LL && v <= 2147483647LL) {
            db(0xC7);
            db((0x3 << 6) | (r & 7));
            dd((uint32_t)(int32_t)v);
        } else {
            db(0xB8 + (r & 7));
            dq((uint64_t)v);
        }
    }
    void movrr(int dst, int src) {
        rexAny(1, (src >= 8) ? 1 : 0, 0, (dst >= 8) ? 1 : 0);
        db(0x89);
        db((0x3 << 6) | ((src & 7) << 3) | (dst & 7));
    }
    void movmr(int base, int32_t disp, int src) { mem(0x89, src, base, disp); }
    void movrm(int dst, int base, int32_t disp) { mem(0x8B, dst, base, disp); }
    void loadParam(int dst, int j, int n) { movrm(dst, RBP, 16 + 8 * (n - 1 - j)); }
    void movmi(int base, int32_t disp, int64_t imm) {
        if (imm >= -2147483648LL && imm <= 2147483647LL) {
            mem(0xC7, 0, base, disp);
            dd((uint32_t)(int32_t)imm);
        } else {
            mov64i(RAX, imm);
            movmr(base, disp, RAX);
        }
    }
    void movzx8rr(int dst, int src) {  // movzx r64, r8(同一寄存器低 8 位)
        db(0x48 | ((src >= 8) ? 1 : 0));
        db(0x0F); db(0xB6);
        db((0x3 << 6) | ((dst & 7) << 3) | (src & 7));
    }
    void learr(int dst, int base, int32_t disp) { mem(0x8D, dst, base, disp); }

    // ---- 算术 / 逻辑 ----
    void opMR(int op, int base, int32_t disp, int src) { mem(op, src, base, disp); }
    // 注意：op 直接传 RM 形完整操作码（0x03 add / 0x3B cmp 等），不再做 +2 变换
    void opRM(int op, int dst, int base, int32_t disp) { mem(op, dst, base, disp); }
    void opRR(int op, int dst, int src) {
        rexAny(1, (src >= 8) ? 1 : 0, 0, (dst >= 8) ? 1 : 0);
        db(op);
        db((0x3 << 6) | ((src & 7) << 3) | (dst & 7));
    }
    void cmpRI(int base, int32_t disp, int64_t imm) {
        if (imm >= -128 && imm <= 127) {
            mem(0x83, 7, base, disp);
            db((int8_t)imm);
        } else {
            mem(0x81, 7, base, disp);
            dd((uint32_t)(int32_t)imm);
        }
    }
    void cmpRegRI(int r, int64_t imm) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        if (imm >= -128 && imm <= 127) {
            db(0x83);
            db((0x3 << 6) | (7 << 3) | (r & 7));
            db((int8_t)imm);
        } else {
            db(0x81);
            db((0x3 << 6) | (7 << 3) | (r & 7));
            dd((uint32_t)(int32_t)imm);
        }
    }
    void testRR(int a, int b) { opRR(0x85, a, b); }
    void imulrm(int dst, int base, int32_t disp) {
        // imul dst, [base+disp]：REX + 0F AF + modrm + SIB + disp
        int mod = (disp >= -128 && disp <= 127) ? 1 : 2;
        rexAny(1, (dst >= 8) ? 1 : 0, 0, (base >= 8) ? 1 : 0);
        db(0x0F); db(0xAF);
        emitMemOperand(mod, dst, base, disp);
    }
    void idivm(int base, int32_t disp) { mem(0xF7, 7, base, disp); }
    void negr(int r) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0xF7); db((0x3 << 6) | (3 << 3) | (r & 7));
    }
    void notr(int r) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0xF7); db((0x3 << 6) | (2 << 3) | (r & 7));
    }
    void cqo() { dbs({0x48, 0x99}); }
    void shiftClMem(int kind, int base, int32_t disp) { mem(0xD3, kind, base, disp); }

    // ---- 控制流 ----
    void jmp32(int label) { db(0xE9); fixLabel(label); }
    void jcc32(int cc, int label) { dbs({0x0F, 0x80 | cc}); fixLabel(label); }
    void call32(int label) { db(0xE8); fixLabel(label); }
    void ret() { db(0xC3); }
    void setcc(int cc, int r) {  // 结果写入 r（64 位 0/1）
        db(0x40 | ((r >= 8) ? 1 : 0));
        db(0x0F); db(0x90 | cc);
        db((0x3 << 6) | (r & 7));
        movzx8rr(r, r);
    }

    // ---- SSE2 浮点（66 前缀的 pd 组 / F2 的 sd 组） ----
    void sdMem(int op, int xmm, int base, int32_t disp) {  // F2 0F op xmm, [mem]
        db(0xF2);
        rexAny(0, (xmm >= 8) ? 1 : 0, 0, (base >= 8) ? 1 : 0);
        db(0x0F); db(op);
        int mod = pickMod(base, disp);
        int m = (mod == 0) ? 0 : ((disp >= -128 && disp <= 127) ? 1 : 2);
        if (base == RBP || base == 13) m = (disp >= -128 && disp <= 127) ? 1 : 2;
        emitMemOperand(m, xmm, base, disp);
    }
    void sdRR(int op, int dst, int src) {  // F2 0F op xmm, xmm
        db(0xF2);
        rexAny(0, (dst >= 8) ? 1 : 0, 0, (src >= 8) ? 1 : 0);
        db(0x0F); db(op);
        db((0x3 << 6) | ((dst & 7) << 3) | (src & 7));
    }
    void movsdFromMem(int xmm, int base, int32_t disp) {  // movsd xmm, [mem]
        sdMem(0x10, xmm, base, disp);
    }
    void movsdToMem(int xmm, int base, int32_t disp) {    // movsd [mem], xmm
        sdMem(0x11, xmm, base, disp);
    }
    void movsdSib(int xmm, int base, int idx, int scale) {
        // movsd xmm, [base+idx*scale]
        db(0xF2);
        rexAny(0, (xmm >= 8) ? 1 : 0, (idx >= 8) ? 1 : 0, (base >= 8) ? 1 : 0);
        db(0x0F); db(0x10);
        db((0 << 6) | ((xmm & 7) << 3) | 4);
        db((scale << 6) | ((idx & 7) << 3) | (base & 7));
    }
    void pdMem(int op, int xmm, int base, int32_t disp) {  // 66 0F op xmm, [mem]
        db(0x66);
        rexAny(0, (xmm >= 8) ? 1 : 0, 0, (base >= 8) ? 1 : 0);
        db(0x0F); db(op);
        int mod = pickMod(base, disp);
        int m = (mod == 0) ? 0 : ((disp >= -128 && disp <= 127) ? 1 : 2);
        if (base == RBP || base == 13) m = (disp >= -128 && disp <= 127) ? 1 : 2;
        emitMemOperand(m, xmm, base, disp);
    }
    void pdRR(int op, int dst, int src) {  // 66 0F op xmm_dst, xmm_src
        db(0x66);
        rexAny(0, (dst >= 8) ? 1 : 0, 0, (src >= 8) ? 1 : 0);
        db(0x0F); db(op);
        db((0x3 << 6) | ((dst & 7) << 3) | (src & 7));
    }
    void cvtsi2sd(int xmm, int rsrc) {
        db(0xF2);
        rexAny(1, (xmm >= 8) ? 1 : 0, 0, (rsrc >= 8) ? 1 : 0);
        db(0x0F); db(0x2A);
        db((0x3 << 6) | ((xmm & 7) << 3) | (rsrc & 7));
    }
    void cvttsd2si(int rdst, int xmm) {
        db(0xF2);
        rexAny(1, (rdst >= 8) ? 1 : 0, 0, (xmm >= 8) ? 1 : 0);
        db(0x0F); db(0x2C);
        db((0x3 << 6) | ((rdst & 7) << 3) | (xmm & 7));
    }
    void movqXmmR(int xmm, int r) {
        db(0x66);
        rexAny(1, (xmm >= 8) ? 1 : 0, 0, (r >= 8) ? 1 : 0);
        db(0x0F); db(0x6E);
        db((0x3 << 6) | ((xmm & 7) << 3) | (r & 7));
    }
    void movqRXmm(int r, int xmm) {  // movq r/m64, xmm（xmm 位模式 → GPR）
        db(0x66);
        rexAny(1, (xmm >= 8) ? 1 : 0, 0, (r >= 8) ? 1 : 0);
        db(0x0F); db(0x7E);
        db((0x3 << 6) | ((xmm & 7) << 3) | (r & 7));
    }
    void roundsd(int xmm, int imm) {  // 66 0F 3A 0B /r ib(寄存器形式,自身取整)
        db(0x66);
        rexAny(0, (xmm >= 8) ? 1 : 0, 0, (xmm >= 8) ? 1 : 0);
        db(0x0F); db(0x3A); db(0x0B);
        db((0x3 << 6) | ((xmm & 7) << 3) | (xmm & 7));
        db(imm);
    }

    // ---- x87 ----
    void x87Mem(int op, int reg, int base, int32_t disp) {
        rexAny(0, 0, 0, (base >= 8) ? 1 : 0);
        db(op);
        int mod = pickMod(base, disp);
        int m = (mod == 0) ? 0 : ((disp >= -128 && disp <= 127) ? 1 : 2);
        if (base == RBP || base == 13) m = (disp >= -128 && disp <= 127) ? 1 : 2;
        emitMemOperand(m, reg, base, disp);
    }
    void fldM(int base, int32_t disp) { x87Mem(0xDD, 0, base, disp); }
    void fstpM(int base, int32_t disp) { x87Mem(0xDD, 3, base, disp); }
    void f2xm1() { dbs({0xD9, 0xF0}); }
    void fyl2x() { dbs({0xD9, 0xF1}); }
    void fscale() { dbs({0xD9, 0xFD}); }
    void fsin() { dbs({0xD9, 0xFE}); }
    void fcos() { dbs({0xD9, 0xFF}); }
    void fptan() { dbs({0xD9, 0xF2}); }
    void fpatan() { dbs({0xD9, 0xF3}); }
    void fprem() { dbs({0xD9, 0xF8}); }
    void fsqrt87() { dbs({0xD9, 0xFA}); }
    void fld1() { dbs({0xD9, 0xE8}); }
    void fldl2e() { dbs({0xD9, 0xEA}); }
    void fldlg2() { dbs({0xD9, 0xEC}); }
    void fldln2() { dbs({0xD9, 0xED}); }
    void fstpSt1() { dbs({0xDD, 0xD9}); }  // fstp st(1)
    void fstpSt0() { dbs({0xDD, 0xD8}); }  // fstp st(0):弹出丢弃栈顶
    void fxch() { dbs({0xD9, 0xC9}); }     // fxch st(1)
    void fldSt0() { dbs({0xD9, 0xC0}); }   // fld st(0)（复制栈顶）
    void fmulp() { dbs({0xDE, 0xC9}); }    // fmulp st(1), st(0)
    void faddp() { dbs({0xDE, 0xC1}); }
    void fsubSt1() { dbs({0xD8, 0xE9}); }  // fsub st(0), st(1):st0 = st0 - st1
    void fsubrSt1() { dbs({0xD8, 0xE1}); } // fsub st(0), st(1)：st0 = st0 - st1
                                           // （本序列里用于取小数部分 f = z-n）
    void fmulSt0St0() { dbs({0xD8, 0xC8}); }  // fmul st(0), st(0)
    void frndint() { dbs({0xD9, 0xFC}); }

    // ---- 系统 ----
    void syscall() { dbs({0x0F, 0x05}); }

    // =====================================================================
    //  语义化辅助指令（native_body.inc 跨架构共用；x64 实现）
    // =====================================================================
    void movAbsData(int reg, size_t dataOff) {
        rexAny(1, 0, 0, (reg >= 8) ? 1 : 0);
        db(0xB8 + (reg & 7));
        fixData(dataOff);
    }
    void addRsp(int n) {
        if (n == 0) return;
        if (n >= -128 && n <= 127) { dbs({0x48, 0x83, 0xC4}); db(n); }
        else { dbs({0x48, 0x81, 0xC4}); dd((uint32_t)n); }
    }
    void subRsp(int n) {
        if (n >= -128 && n <= 127) { dbs({0x48, 0x83, 0xEC}); db(n); }
        else { dbs({0x48, 0x81, 0xEC}); dd((uint32_t)n); }
    }
    void incMem(int base, int32_t disp) {
        rexAny(1, 0, 0, (base >= 8) ? 1 : 0);
        db(0xFF);
        emitMemOperand((disp >= -128 && disp <= 127) ? 1 : 2, 0, base, disp);
    }
    void movSib(int dst, int base, int idx, int scale) {
        rexAny(1, (dst >= 8) ? 1 : 0, (idx >= 8) ? 1 : 0, (base >= 8) ? 1 : 0);
        db(0x8B);
        db((0 << 6) | ((dst & 7) << 3) | 4);
        db((scale << 6) | ((idx & 7) << 3) | (base & 7));
    }
    void movSibStore(int src, int base, int idx, int scale) {
        rexAny(1, (src >= 8) ? 1 : 0, (idx >= 8) ? 1 : 0, (base >= 8) ? 1 : 0);
        db(0x89);
        db((0 << 6) | ((src & 7) << 3) | 4);
        db((scale << 6) | ((idx & 7) << 3) | (base & 7));
    }
    void xorRI(int r, int64_t imm) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0x83); db(0xC0 | (6 << 3) | (r & 7)); db((int8_t)imm);
    }
    void addRI(int r, int64_t imm) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0x83); db(0xC0 | (0 << 3) | (r & 7)); db((int8_t)imm);
    }
    void andRI(int r, int64_t imm) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0x83); db(0xC0 | (4 << 3) | (r & 7)); db((int8_t)imm);
    }
    void shlRI(int r, int imm) {
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0xC1); db(0xC0 | (4 << 3) | (r & 7)); db(imm);
    }
    void shrRI(int r, int imm) {  // 逻辑右移
        rexAny(1, 0, 0, (r >= 8) ? 1 : 0);
        db(0xC1); db(0xC0 | (5 << 3) | (r & 7)); db(imm);
    }
    void zeroReg(int r) {
        if (r >= 8) db(0x45);
        db(0x31); db(0xC0 | ((r & 7) << 3) | (r & 7));
    }
    void store8Reg(int addr, int src) {
        if ((src >= 8) || (addr >= 8)) db(0x40 | ((src >= 8) ? 4 : 0) | ((addr >= 8) ? 1 : 0));
        db(0x88); db(((src & 7) << 3) | (addr & 7));
    }
    void movzx8mem(int dst, int addr) {
        if ((dst >= 8) || (addr >= 8)) db(0x40 | ((dst >= 8) ? 4 : 0) | ((addr >= 8) ? 1 : 0));
        db(0x0F); db(0xB6); db(((dst & 7) << 3) | (addr & 7));
    }
    void floatCompare(BinOp op) {
        int tcc;
        bool neNaN = false;
        switch (op) {
            case BinOp::Eq: tcc = CJe; break;
            case BinOp::Ne: tcc = CJe; neNaN = true; break;
            case BinOp::Lt: tcc = CJb; break;
            case BinOp::Le: tcc = CJbe; break;
            case BinOp::Gt: tcc = CJa; break;
            default: tcc = CJae; break;
        }
        movsdFromMem(0, RSP, 8);   // lhs
        pdMem(0x2E, 0, RSP, 0);    // ucomisd xmm0, [rsp]（rhs）
        (void)neNaN;
        int lt = newLabel(), lf = newLabel(), lend = newLabel();
        if (op == BinOp::Ne) {
            jcc32(CJp, lt);        // 无序 → true
            jcc32(CJne, lt);
            jmp32(lf);
        } else {
            jcc32(CJp, lf);        // 无序 → false
            jcc32(tcc, lt);
            jmp32(lf);
        }
        bind(lt);
        movmi(RSP, 8, 1);
        jmp32(lend);
        bind(lf);
        movmi(RSP, 8, 0);
        bind(lend);
        addRsp(8);
    }
    void floatIsNan() {
        movsdFromMem(0, RSP, 0);
        pdRR(0x2E, 0, 0);   // ucomisd xmm0, xmm0（NaN → PF=1）
        // 必须先 setcc 再清槽：addRsp 的 add 会覆盖 PF
        setcc(CJp, RAX);
        addRsp(8);
        push(RAX);
    }
    void rawSyscall(int n) {
        if (n >= 2) movrm(RDI, RSP, 8 * (n - 2));
        if (n >= 3) movrm(RSI, RSP, 8 * (n - 3));
        if (n >= 4) movrm(RDX, RSP, 8 * (n - 4));
        if (n >= 5) movrm(10, RSP, 8 * (n - 5));  // r10
        if (n >= 6) movrm(8, RSP, 8 * (n - 6));   // r8
        if (n >= 7) movrm(9, RSP, 8 * (n - 7));   // r9
        movrm(RAX, RSP, 8 * (n - 1));
        addRsp(8 * n);
        syscall();
        push(RAX);
    }
    void sysOpen() {
        movrm(RDI, RSP, 8);   // path
        movrm(RSI, RSP, 0);   // flags
        mov64i(RDX, 438);     // mode 0644
        mov64i(RAX, 2);       // __NR_open
        addRsp(16);
        syscall();
        push(RAX);
    }
    void sysUnlink() {
        movrm(RDI, RSP, 0);
        mov64i(RAX, 87);      // __NR_unlink
        addRsp(8);
        syscall();
        push(RAX);
    }
    void sysRename() {
        movrm(RDI, RSP, 8);   // from
        movrm(RSI, RSP, 0);   // to
        mov64i(RAX, 82);      // __NR_rename
        addRsp(16);
        syscall();
        push(RAX);
    }
    void sysFork() {
        mov64i(RAX, 57);      // __NR_fork
        syscall();
        push(RAX);
    }
    void heapMmapFirst() {
        // 入参：RAX = 堆槽地址；出参：RAX = 槽地址，RCX = base
        push(RAX);  // 保存槽地址（syscall 会破坏寄存器）
        dbs({0x31, 0xFF});               // xor edi, edi
        db(0x48); db(0xBE);
        dq(0x4000000);                   // movabs rsi, 64MB
        dbs({0xBA, 0x03, 0x00, 0x00, 0x00});  // mov edx, 3
        dbs({0x41, 0xBA, 0x22, 0x00, 0x00, 0x00});  // mov r10d, 0x22
        db(0x49); db(0xB8);
        dq(0xFFFFFFFFFFFFFFFFULL);       // movabs r8, -1（fd）
        dbs({0x45, 0x31, 0xC9});         // xor r9d, r9d（off=0）
        dbs({0xB8, 0x09, 0x00, 0x00, 0x00});  // mov eax, SYS_mmap
        syscall();
        pop(RDX);                        // 槽地址
        movrr(RCX, RAX);                 // base
        movmr(RDX, 0, RCX);              // cur = base
        db(0x48); db(0x81);
        db(0xC1); dd(0x4000000);         // add rcx, 64MB
        movmr(RDX, 8, RCX);              // end = base + 64MB
        movrm(RCX, RDX, 0);              // rcx = cur = base
        movrr(RAX, RDX);                 // rax = 槽地址（公共路径统一）
    }
    void memmoveRaw() {  // RDI=dst, RSI=src, RCX=n
        int lfwd = newLabel(), ldone = newLabel();
        opRR(0x39, RDI, RSI);      // cmp rdi, rsi
        jcc32(CJbe, lfwd);         // dst <= src → 正向安全
        opRR(0x01, RDI, RCX);      // add rdi, rcx
        dbs({0x48, 0xFF, 0xCF});   // dec rdi
        opRR(0x01, RSI, RCX);      // add rsi, rcx
        dbs({0x48, 0xFF, 0xCE});   // dec rsi
        db(0xFD);                  // std
        dbs({0xF3, 0xA4});         // rep movsb（DF=1 反向递减）
        db(0xFC);                  // cld（ABI 要求返回时 DF=0）
        jmp32(ldone);
        bind(lfwd);
        dbs({0xF3, 0xA4});         // rep movsb（DF=0 正向递增）
        bind(ldone);
    }
    void frameEnter(int frameSize) {
        push(RBP);
        dbs({0x48, 0x8B, 0xEC});  // mov rbp, rsp
        subRsp(frameSize);
    }
    void frameLeave() {
        dbs({0x48, 0x8B, 0xE5});  // mov rsp, rbp
        pop(RBP);
        ret();
    }
    void entrySetup() {
        dbs({0x31, 0xED});        // xor ebp, ebp
        db(0x48); db(0xB8); dq(kSlotInitStack);
        dbs({0x48, 0x89, 0x20});  // mov [slot], rsp
    }
    void exitMain() {
        dbs({0x48, 0x89, 0xC7});               // mov rdi, rax
        dbs({0xB8, 0xE7, 0x00, 0x00, 0x00});   // mov eax, 231 (exit_group)
        syscall();
    }
    void exitGroupReg(int r) {
        // mov rdi, r
        rexAny(1, (r >= 8) ? 1 : 0, 0, 0);
        db(0x89); db((0x3 << 6) | ((r & 7) << 3) | (RDI & 7));
        dbs({0xB8, 0xE7, 0x00, 0x00, 0x00});
        syscall();
    }
};

// =============================================================================
//  ELF64 输出（x86-64，单 RWX LOAD 段）
// =============================================================================

static std::vector<uint8_t> buildElf(const X64& c, const std::vector<uint8_t>& data,
                                     size_t entryOff, size_t dataSegOff) {
    // 布局：[Ehdr 64][Phdr 56][pad→128][code][data]，vaddr = kBase + offset
    // p_memsz 至少扩到 0x201000：文件尾之后的内存是零页（bss），运行时库
    // 用 0x600000 起的固定地址做全局状态槽（随机种子 / 堆指针 / env 列表）
    size_t codeOff = 128;
    size_t total = dataSegOff + data.size();
    constexpr uint64_t kMinMemSz = 0x800000;  // 覆盖 0x601000 起的 1MiB 行缓冲
    uint64_t memsz = total < kMinMemSz ? kMinMemSz : (uint64_t)total;
    std::vector<uint8_t> img(total, 0);
    const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(img.data(), ident, 16);
    put16(img, 16, 2);      // e_type = ET_EXEC
    put16(img, 18, 0x3E);   // e_machine = EM_X86_64
    put32(img, 20, 1);      // e_version
    put64(img, 24, kBase + codeOff + entryOff);  // e_entry
    put64(img, 32, 64);     // e_phoff
    put32(img, 48, 0);      // e_flags
    put16(img, 52, 64);     // e_ehsize
    put16(img, 54, 56);     // e_phentsize
    put16(img, 56, 1);      // e_phnum
    put32(img, 64, 1);      // p_type = PT_LOAD
    put32(img, 68, 7);      // p_flags = R+W+X
    put64(img, 72, 0);      // p_offset
    put64(img, 80, kBase);  // p_vaddr
    put64(img, 88, kBase);  // p_paddr
    put64(img, 96, total);  // p_filesz
    put64(img, 104, memsz); // p_memsz（> filesz 部分为零页 bss）
    put64(img, 112, 0x1000);
    std::memcpy(img.data() + codeOff, c.code.data(), c.code.size());
    if (!data.empty())
        std::memcpy(img.data() + dataSegOff, data.data(), data.size());
    return img;
}

using NativeArch = X64;

#endif  // __aarch64__

}  // namespace
}  // namespace lux
