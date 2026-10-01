// =============================================================================
//  Lux 原生后端 — AArch64 (arm64) 机器码发射器与 ELF64 输出
//  与 native_emit_x64 暴露同一套语义方法，供 native_body.inc 共用。
//
//  调用约定（与 x64 后端逐条对应）：
//    * 参数走栈、从左到右求值依次压栈（每参数 8 字节，float 为 IEEE 位模式），
//      被调方参数 j = [x29 + 16 + 8*(n-1-j)]（LR/FP 保存占 16 字节，恰好等价
//      于 x86 的“push rbp + call 压返回地址”）；
//    * 返回值 x0（整型/指针）或 d0（float）；
//    * push/pop 用 8 字节步进的 pre/post-index str/ldr，保持 [sp+8k] 槽布局与
//      x86 完全一致（不要求 16 字节对齐——本后端自成一体的调用约定）。
//
//  寄存器映射（x86 枚举 → aarch64）：
//    RAX→x0  RCX→x1  RDX→x2  RBX→x3  RSP→sp  RBP→x29  RSI→x4  RDI→x5
//    r8→x9  r9→x10  r10→x11            （x16/x17 为发射器内部暂存）
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>

// x86 寄存器枚举名沿用，便于 native_body.inc 不改
enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7 };

// 条件码（与 x86 后端同名）
enum CC { CJo = 0, CJno = 1, CJb = 2, CJae = 3, CJe = 4, CJne = 5,
          CJbe = 6, CJa = 7, CJs = 8, CJns = 9, CJp = 10, CJnp = 11,
          CJl = 12, CJge = 13, CJle = 14, CJg = 15 };

constexpr uint64_t kBase = 0x400000;

inline int a64cond(int cc) {
    switch (cc) {
        case CJo:  return 6;   // VS
        case CJno: return 7;   // VC
        case CJb:  return 3;   // LO
        case CJae: return 2;   // HS
        case CJe:  return 0;   // EQ
        case CJne: return 1;   // NE
        case CJbe: return 9;   // LS
        case CJa:  return 8;   // HI
        case CJs:  return 4;   // MI
        case CJns: return 5;   // PL
        case CJp:  return 6;   // VS（无序）
        case CJnp: return 7;   // VC
        case CJl:  return 11;  // LT
        case CJge: return 10;  // GE
        case CJle: return 13;  // LE
        case CJg:  return 12;  // GT
    }
    return 0;
}

struct AFix {
    size_t at;   // 偏移
    int label;   // >=0 标签；<0 数据引用（-1-dataOff）
    int kind;    // 0=B/BL(imm26) 1=B.cond(imm19) 2=数据 movz/movk×4
    int reg;     // 数据引用时的目标寄存器（原始编号）
};

struct Arm64 {
    std::vector<uint8_t> code;
    std::vector<AFix> fixups;
    std::vector<long long> labels;

    int newLabel() { labels.push_back(-1); return (int)labels.size() - 1; }
    void bind(int l) {
        if (labels[l] >= 0) {
            std::fprintf(stderr, "编译器内部错误: 标签 %d 重复绑定\n", l);
            std::exit(3);
        }
        labels[l] = (long long)code.size();
    }
    size_t emit32(uint32_t w) {
        size_t at = code.size();
        for (int i = 0; i < 4; i++) code.push_back((uint8_t)((w >> (8 * i)) & 0xFF));
        return at;
    }

    // x86 寄存器编号 → aarch64 原始编号。
    // 注意：RSP 映射到 x28（独立的值栈指针），不是硬件 sp。arm64 内核对 EL0
    // 启用 SP 对齐检查，硬件 sp 必须始终 16 字节对齐，无法承载 8 字节步进的
    // 表达式栈；因此用一个自管理内存区（x28 指向栈顶，向下增长）作为值栈，
    // 硬件 sp 只用于保存 LR/帧指针（stp/ldp，保持 16 对齐）。
    static int R(int r) {
        switch (r) {
            case 0: return 0;
            case 1: return 1;
            case 2: return 2;
            case 3: return 3;
            case 4: return 28;  // RSP → 值栈指针 vsp
            case 5: return 29;  // fp
            case 6: return 4;
            case 7: return 5;
            case 8: return 9;
            case 9: return 10;
            case 10: return 11;
            case 11: return 12;
            case 12: return 13;
            case 13: return 14;
            default: return r;
        }
    }

    // ---- 基础发射 ----
    void movzRaw(int rd, uint16_t imm, int hw) {
        emit32(0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
    }
    void movkRaw(int rd, uint16_t imm, int hw) {
        emit32(0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)(rd & 31));
    }
    void mov64iRaw(int rd, int64_t v) {
        uint64_t u = (uint64_t)v;
        for (int i = 0; i < 4; i++) {
            uint16_t c = (uint16_t)((u >> (16 * i)) & 0xFFFF);
            if (i == 0) movzRaw(rd, c, 0);
            else if (c) movkRaw(rd, c, i);
        }
    }
    void movReg(int d, int m) { emit32(0xAA0003E0u | ((uint32_t)(m & 31) << 16) | (uint32_t)(d & 31)); }
    void addReg(int d, int n, int m) { emit32(0x8B000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31)); }
    void subReg(int d, int n, int m) { emit32(0xCB000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31)); }
    void andReg(int d, int n, int m) { emit32(0x8A000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31)); }
    void eorReg(int d, int n, int m) { emit32(0xCA000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31)); }
    void orrReg(int d, int n, int m) { emit32(0xAA000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31)); }
    // 地址计算：rd = base + disp（base 可为 sp=31）
    void addAddr(int rd, int rn, int32_t disp) {
        uint32_t imm = (disp < 0) ? (uint32_t)(-disp) : (uint32_t)disp;
        uint32_t base = (disp < 0) ? 0xD1000000u : 0x91000000u;
        if (imm <= 4095) {
            emit32(base | (imm << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rd & 31));
        } else {
            mov64iRaw(17, disp);
            addReg(rd, rn, 17);
        }
    }
    void ldrRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 32760 && (disp % 8 == 0)) {
            emit32(0xF9400000u | ((uint32_t)(disp / 8) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0xF8400000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0xF9400000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }
    void strRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 32760 && (disp % 8 == 0)) {
            emit32(0xF9000000u | ((uint32_t)(disp / 8) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0xF8000000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0xF9000000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }
    void ldrDRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 32760 && (disp % 8 == 0)) {
            emit32(0xFD400000u | ((uint32_t)(disp / 8) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0xFC400000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0xFD400000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }
    void strDRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 32760 && (disp % 8 == 0)) {
            emit32(0xFD000000u | ((uint32_t)(disp / 8) << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0xFC000000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0xFD000000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }
    void ldrbRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 4095) {
            emit32(0x39400000u | ((uint32_t)disp << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0x38400000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0x39400000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }
    void strbRaw(int rt, int rn, int32_t disp) {
        if (disp >= 0 && disp <= 4095) {
            emit32(0x39000000u | ((uint32_t)disp << 10) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else if (disp >= -256 && disp <= 255) {
            emit32(0x38000000u | ((uint32_t)(disp & 0x1FF) << 12) | ((uint32_t)(rn & 31) << 5) | (uint32_t)(rt & 31));
        } else {
            addAddr(17, rn, disp);
            emit32(0x39000000u | ((uint32_t)17 << 5) | (uint32_t)(rt & 31));
        }
    }

    // 二元整数运算（k：add/sub/xor/and/or）
    enum { A_ADD = 0, A_SUB, A_XOR, A_AND, A_OR };
    void binReg(int k, int d, int n, int m) {
        switch (k) {
            case A_ADD: addReg(d, n, m); return;
            case A_SUB: subReg(d, n, m); return;
            case A_XOR: eorReg(d, n, m); return;
            case A_AND: andReg(d, n, m); return;
            default: orrReg(d, n, m); return;
        }
    }
    int opcodeKind(int op) {
        switch (op) {
            case 0x01: case 0x03: return A_ADD;
            case 0x29: case 0x2B: return A_SUB;
            case 0x31: case 0x33: return A_XOR;
            case 0x21: case 0x23: return A_AND;
            case 0x09: case 0x0B: return A_OR;
            default: return -1;  // cmp / test
        }
    }

    // ---- 分支 ----
    void bcondRaw(int cond, int label) {
        size_t at = emit32(0x54000000u | ((uint32_t)(cond & 0xF) << 0));
        fixups.push_back({at, label, 1, 0});
    }
    void jcc32(int cc, int label) { bcondRaw(a64cond(cc), label); }
    void jmp32(int label) {
        size_t at = emit32(0x14000000u);
        fixups.push_back({at, label, 0, 0});
    }
    void call32(int label) {
        size_t at = emit32(0x94000000u);
        fixups.push_back({at, label, 0, 0});
    }
    void ret() { emit32(0xD65F03C0u); }
    void syscall() { emit32(0xD4000001u); }  // svc #0

    void fixData(size_t dataOff, int reg) {
        size_t at = code.size();
        movzRaw(reg, 0, 0); movkRaw(reg, 0, 1); movkRaw(reg, 0, 2); movkRaw(reg, 0, 3);
        fixups.push_back({at, (int)(-1 - (long long)dataOff), 2, reg});
    }

    void resolve(const std::vector<uint8_t>& data, size_t dataSegOff) {
        (void)data;
        for (const AFix& f : fixups) {
            if (f.kind == 2) {
                size_t doff = (size_t)(-1 - (long long)f.label);
                uint64_t addr = kBase + dataSegOff + doff;
                for (int h = 0; h < 4; h++) {
                    uint16_t c = (uint16_t)((addr >> (16 * h)) & 0xFFFF);
                    uint32_t w = (h == 0 ? 0xD2800000u : 0xF2800000u) |
                                 ((uint32_t)h << 21) | ((uint32_t)c << 5) | (uint32_t)(f.reg & 31);
                    for (int i = 0; i < 4; i++) code[f.at + h * 4 + i] = (uint8_t)((w >> (8 * i)) & 0xFF);
                }
                continue;
            }
            long long target = labels[f.label];
            if (target < 0) {
                std::fprintf(stderr, "编译器内部错误: 未绑定的标签 %d\n", f.label);
                std::exit(3);
            }
            int64_t rel = target - (int64_t)f.at;
            uint32_t w = (uint32_t)code[f.at] | ((uint32_t)code[f.at + 1] << 8) |
                         ((uint32_t)code[f.at + 2] << 16) | ((uint32_t)code[f.at + 3] << 24);
            if (f.kind == 0) {
                uint32_t imm26 = (uint32_t)((rel >> 2) & 0x03FFFFFF);
                w = (w & ~0x03FFFFFFu) | imm26;
            } else {
                uint32_t imm19 = (uint32_t)((rel >> 2) & 0x7FFFF);
                w = (w & ~(0x7FFFFu << 5)) | (imm19 << 5);
            }
            for (int i = 0; i < 4; i++) code[f.at + i] = (uint8_t)((w >> (8 * i)) & 0xFF);
        }
        fixups.clear();
    }

    // ---- 栈 / 数据移动（x86 语义接口；RSP→值栈指针 x28） ----
    void push(int r) {
        int x = R(r);
        emit32(0xF8000C00u | ((uint32_t)0x1F8 << 12) | (28u << 5) | (uint32_t)(x & 31));
    }
    void pop(int r) {
        int x = R(r);
        emit32(0xF8400400u | ((uint32_t)8 << 12) | (28u << 5) | (uint32_t)(x & 31));
    }
    void addRsp(int n) {
        if (n == 0) return;
        if (n > 0 && n <= 4095) { emit32(0x91000000u | ((uint32_t)n << 10) | (28u << 5) | 28u); return; }
        if (n < 0 && -n <= 4095) { emit32(0xD1000000u | ((uint32_t)(-n) << 10) | (28u << 5) | 28u); return; }
        mov64iRaw(16, n);
        addReg(28, 28, 16);
    }
    void subRsp(int n) {
        if (n > 0 && n <= 4095) { emit32(0xD1000000u | ((uint32_t)n << 10) | (28u << 5) | 28u); return; }
        if (n < 0) { addRsp(-n); return; }
        mov64iRaw(16, n);
        subReg(28, 28, 16);
    }
    void mov64i(int r, int64_t v) { mov64iRaw(R(r), v); }
    void movrr(int dst, int src) {
        if (R(src) == 31) { addAddr(R(dst), 31, 0); return; }
        if (R(dst) == 31) { addAddr(31, R(src), 0); return; }
        movReg(R(dst), R(src));
    }
    void movrm(int dst, int base, int32_t disp) { ldrRaw(R(dst), R(base), disp); }
    // 读取第 j 个形参（共 n 个）：arm64 下实参紧贴值栈帧指针
    void loadParam(int dst, int j, int n) { ldrRaw(R(dst), 29, 8 * (n - 1 - j)); }
    void movmr(int base, int32_t disp, int src) { strRaw(R(src), R(base), disp); }
    void movmi(int base, int32_t disp, int64_t imm) {
        if (imm >= 0 && imm <= 65535) movzRaw(16, (uint16_t)imm, 0);
        else mov64iRaw(16, imm);
        strRaw(16, R(base), disp);
    }
    void learr(int dst, int base, int32_t disp) { addAddr(R(dst), R(base), disp); }
    void movAbsData(int reg, size_t dataOff) { fixData(dataOff, R(reg)); }
    void incMem(int base, int32_t disp) {
        ldrRaw(16, R(base), disp);
        emit32(0x91000400u | (16u << 5) | 16u);  // add x16, x16, #1
        strRaw(16, R(base), disp);
    }
    void movSib(int dst, int base, int idx, int scale) {
        if (scale == 3) {
            emit32(0xF8600800u | ((uint32_t)(R(idx) & 31) << 16) | (3u << 13) | (1u << 12) |
                   ((uint32_t)(R(base) & 31) << 5) | (uint32_t)(R(dst) & 31));
        } else {
            emit32(0x8B000000u | ((uint32_t)(R(idx) & 31) << 16) | ((uint32_t)(scale & 31) << 10) |
                   ((uint32_t)(R(base) & 31) << 5) | 17u);  // add x17, base, idx, lsl #scale
            ldrRaw(R(dst), 17, 0);
        }
    }
    void movSibStore(int src, int base, int idx, int scale) {
        if (scale == 3) {
            emit32(0xF8200800u | ((uint32_t)(R(idx) & 31) << 16) | (3u << 13) | (1u << 12) |
                   ((uint32_t)(R(base) & 31) << 5) | (uint32_t)(R(src) & 31));
        } else {
            emit32(0x8B000000u | ((uint32_t)(R(idx) & 31) << 16) | ((uint32_t)(scale & 31) << 10) |
                   ((uint32_t)(R(base) & 31) << 5) | 17u);
            strRaw(R(src), 17, 0);
        }
    }
    void movsdSib(int xmm, int base, int idx, int scale) {
        if (scale == 3) {
            // ldr <Dt>, [Xn, Xm, lsl #3]：基码 0xFC600800
            // （旧值 0xFD600800 多置了 bit24，被译码成别的指令，
            //  导致 float 数组元素读出来全是 0——C11，0.8 修正）
            emit32(0xFC600800u | ((uint32_t)(R(idx) & 31) << 16) | (3u << 13) | (1u << 12) |
                   ((uint32_t)(R(base) & 31) << 5) | (uint32_t)(xmm & 31));
        } else {
            emit32(0x8B000000u | ((uint32_t)(R(idx) & 31) << 16) | ((uint32_t)(scale & 31) << 10) |
                   ((uint32_t)(R(base) & 31) << 5) | 17u);
            ldrDRaw(xmm, 17, 0);
        }
    }
    void movzx8rr(int dst, int src) {
        emit32(0x53001C00u | ((uint32_t)(R(src) & 31) << 5) | (uint32_t)(R(dst) & 31));
    }
    void store8Reg(int addr, int src) { strbRaw(R(src), R(addr), 0); }
    void movzx8mem(int dst, int addr) { ldrbRaw(R(dst), R(addr), 0); }

    // ---- 整数算术 / 逻辑 ----
    void opRR(int op, int dst, int src) {
        int k = opcodeKind(op);
        int d = R(dst), m = R(src);
        if (k < 0) {  // cmp 0x39 / test 0x85
            if (op == 0x85) emit32(0xEA00001Fu | ((uint32_t)m << 16) | ((uint32_t)d << 5));
            else emit32(0xEB00001Fu | ((uint32_t)m << 16) | ((uint32_t)d << 5));
            return;
        }
        binReg(k, d, d, m);
    }
    void opMem(int op, int reg, int base, int32_t disp, bool memDest) {
        int a = R(reg), k = opcodeKind(op);
        ldrRaw(16, R(base), disp);
        if (k < 0) {  // cmp：注意操作数方向（memDest = 内存是左操作数）
            if (memDest) emit32(0xEB00001Fu | ((uint32_t)a << 16) | (16u << 5));
            else emit32(0xEB00001Fu | (16u << 16) | ((uint32_t)a << 5));
        } else if (memDest) {
            binReg(k, 16, 16, a);
            strRaw(16, R(base), disp);
        } else {
            binReg(k, a, a, 16);
        }
    }
    void opMR(int op, int base, int32_t disp, int src) { opMem(op, src, base, disp, true); }
    void opRM(int op, int dst, int base, int32_t disp) { opMem(op, dst, base, disp, false); }
    void cmpRegRI(int r, int64_t imm) {
        int a = R(r);
        if (imm >= 0 && imm <= 4095) {
            emit32(0xF100001Fu | ((uint32_t)imm << 10) | ((uint32_t)a << 5));
        } else {
            mov64iRaw(16, imm);
            emit32(0xEB00001Fu | (16u << 16) | ((uint32_t)a << 5));
        }
    }
    void cmpRI(int base, int32_t disp, int64_t imm) {
        ldrRaw(16, R(base), disp);
        if (imm >= 0 && imm <= 4095) {
            emit32(0xF100001Fu | ((uint32_t)imm << 10) | (16u << 5));
        } else {
            mov64iRaw(17, imm);
            emit32(0xEB00001Fu | (17u << 16) | (16u << 5));
        }
    }
    void testRR(int a, int b) {
        emit32(0xEA00001Fu | ((uint32_t)R(b) << 16) | ((uint32_t)R(a) << 5));
    }
    void imulrm(int dst, int base, int32_t disp) {
        ldrRaw(16, R(base), disp);
        int d = R(dst);
        emit32(0x9B007C00u | (16u << 16) | ((uint32_t)d << 5) | (uint32_t)d);
    }
    void idivm(int base, int32_t disp) {
        ldrRaw(16, R(base), disp);                       // 除数
        emit32(0x9AC00C00u | (16u << 16) | (0u << 5) | 17u);   // sdiv x17, x0, x16
        emit32(0x9B008000u | (16u << 16) | (0u << 10) | (17u << 5) | 2u);  // msub x2, x17, x16, x0
        movReg(0, 17);                                        // x0 = 商
    }
    void negr(int r) { int d = R(r); emit32(0xCB0003E0u | ((uint32_t)d << 16) | (uint32_t)d); }
    void notr(int r) { int d = R(r); emit32(0xAA2003E0u | ((uint32_t)d << 16) | (uint32_t)d); }
    void cqo() { emit32(0x937FFC02u); }  // asr x2, x0, #63（rdx = rax 符号掩码）
    void shiftClMem(int kind, int base, int32_t disp) {
        ldrRaw(16, R(base), disp);
        if (kind == 4) emit32(0x9AC02000u | (1u << 16) | (16u << 5) | 16u);  // lsl x16,x16,x1
        else emit32(0x9AC02800u | (1u << 16) | (16u << 5) | 16u);            // asr x16,x16,x1
        strRaw(16, R(base), disp);
    }
    void xorRI(int r, int64_t imm) { mov64iRaw(17, imm); eorReg(R(r), R(r), 17); }
    void addRI(int r, int64_t imm) {
        if (imm >= 0 && imm <= 4095) { emit32(0x91000000u | ((uint32_t)imm << 10) | ((uint32_t)R(r) << 5) | (uint32_t)R(r)); return; }
        if (imm < 0 && -imm <= 4095) { emit32(0xD1000000u | ((uint32_t)(-imm) << 10) | ((uint32_t)R(r) << 5) | (uint32_t)R(r)); return; }
        mov64iRaw(17, imm); addReg(R(r), R(r), 17);
    }
    void andRI(int r, int64_t imm) { mov64iRaw(17, imm); andReg(R(r), R(r), 17); }
    void shlRI(int r, int imm) {
        int d = R(r);
        uint32_t immr = (uint32_t)((64 - imm) & 63), imms = (uint32_t)(63 - imm);
        emit32(0xD3400000u | (immr << 16) | (imms << 10) | ((uint32_t)d << 5) | (uint32_t)d);
    }
    void shrRI(int r, int imm) {
        int d = R(r);
        uint32_t immr = (uint32_t)(imm & 63), imms = 63;
        emit32(0xD3400000u | (immr << 16) | (imms << 10) | ((uint32_t)d << 5) | (uint32_t)d);
    }
    void zeroReg(int r) { movzRaw(R(r), 0, 0); }
    void setcc(int cc, int r) {
        int a = R(r), c = a64cond(cc);
        mov64iRaw(a, 0);
        int ldone = newLabel();
        bcondRaw((c ^ 1) & 0xF, ldone);
        mov64iRaw(a, 1);
        bind(ldone);
    }

    // ---- 浮点（SSE 风格接口 → 标量 FP） ----
    void sdRR(int op, int dst, int src) {
        uint32_t base;
        switch (op) {
            case 0x58: base = 0x1E602800u; break;  // fadd
            case 0x5C: base = 0x1E603800u; break;  // fsub
            case 0x59: base = 0x1E600800u; break;  // fmul
            case 0x5E: base = 0x1E601800u; break;  // fdiv
            case 0x51: emit32(0x1E61C000u | ((uint32_t)(dst & 31) << 5) | (uint32_t)(dst & 31)); return;  // fsqrt
            default: return;
        }
        emit32(base | ((uint32_t)(src & 31) << 16) | ((uint32_t)(dst & 31) << 5) | (uint32_t)(dst & 31));
    }
    void pdRR(int op, int dst, int src) {
        if (op == 0x57) emit32(0x6E201C00u | ((uint32_t)(src & 31) << 16) | ((uint32_t)(dst & 31) << 5) | (uint32_t)(dst & 31));
        else if (op == 0x54) emit32(0x4E201C00u | ((uint32_t)(src & 31) << 16) | ((uint32_t)(dst & 31) << 5) | (uint32_t)(dst & 31));
    }
    void movsdFromMem(int xmm, int base, int32_t disp) { ldrDRaw(xmm, R(base), disp); }
    void movsdToMem(int xmm, int base, int32_t disp) { strDRaw(xmm, R(base), disp); }
    void cvtsi2sd(int xmm, int rsrc) { emit32(0x9E620000u | ((uint32_t)R(rsrc) << 5) | (uint32_t)(xmm & 31)); }
    void cvttsd2si(int rdst, int xmm) { emit32(0x9E780000u | ((uint32_t)(xmm & 31) << 5) | (uint32_t)R(rdst)); }
    void movqXmmR(int xmm, int r) { emit32(0x9E670000u | ((uint32_t)R(r) << 5) | (uint32_t)(xmm & 31)); }
    void movqRXmm(int r, int xmm) { emit32(0x9E660000u | ((uint32_t)(xmm & 31) << 5) | (uint32_t)R(r)); }
    void roundsd(int xmm, int imm) {
        uint32_t base = imm == 1 ? 0x1E654000u : imm == 2 ? 0x1E64C000u : 0x1E65C000u;
        emit32(base | ((uint32_t)(xmm & 31) << 5) | (uint32_t)(xmm & 31));
    }
    void floatCompare(BinOp op) {
        ldrDRaw(0, 28, 8);   // d0 = lhs
        ldrDRaw(1, 28, 0);   // d1 = rhs
        emit32(0x1E612000u); // fcmp d0, d1
        int cond;
        switch (op) {
            case BinOp::Eq: cond = 0; break;   // EQ
            case BinOp::Ne: cond = 1; break;   // NE
            case BinOp::Lt: cond = 4; break;   // MI
            case BinOp::Le: cond = 9; break;   // LS
            case BinOp::Gt: cond = 12; break;  // GT
            default: cond = 10; break;         // GE
        }
        mov64iRaw(16, 0);
        int lset = newLabel(), ldone = newLabel();
        bcondRaw(cond, lset);
        jmp32(ldone);
        bind(lset);
        mov64iRaw(16, 1);
        bind(ldone);
        strRaw(16, 28, 8);
        addRsp(8);
    }
    void floatIsNan() {
        ldrDRaw(0, 28, 0);
        emit32(0x1E602000u);  // fcmp d0, d0（无序 → V=1）
        mov64iRaw(16, 0);
        int ldone = newLabel();
        bcondRaw(7, ldone);      // VC：有序则保持 0
        mov64iRaw(16, 1);
        bind(ldone);
        strRaw(16, 28, 0);
    }

    // ---- 系统调用 / 内存搬运 / 帧 ----
    void rawSyscall(int n) {
        static const int argR[6] = {0, 1, 2, 3, 4, 5};
        for (int i = 0; i < 6 && i < n - 1; i++) {
            // 第 i 个参数在 [sp + 8*(n-2-i)]
            ldrRaw(argR[i], 28, 8 * (n - 2 - i));
        }
        ldrRaw(8, 28, 8 * (n - 1));  // nr
        addRsp(8 * n);
        syscall();
        push(0);
    }
    void sysOpen() {
        mov64iRaw(0, -100);            // AT_FDCWD
        ldrRaw(1, 28, 8);              // path
        ldrRaw(2, 28, 0);              // flags
        mov64iRaw(3, 438);             // mode 0644
        movzRaw(8, 56, 0);             // __NR_openat
        addRsp(16);
        syscall();
        push(0);
    }
    void sysUnlink() {
        mov64iRaw(0, -100);            // AT_FDCWD
        ldrRaw(1, 28, 0);              // path
        movzRaw(2, 0, 0);
        movzRaw(8, 35, 0);             // __NR_unlinkat
        addRsp(8);
        syscall();
        push(0);
    }
    void sysRename() {
        mov64iRaw(0, -100);            // AT_FDCWD
        ldrRaw(1, 28, 8);              // from
        mov64iRaw(2, -100);            // AT_FDCWD
        ldrRaw(3, 28, 0);              // to
        movzRaw(8, 38, 0);             // __NR_renameat
        addRsp(16);
        syscall();
        push(0);
    }
    void sysFork() {
        movzRaw(0, 17, 0);             // SIGCHLD
        movzRaw(1, 0, 0);
        movzRaw(2, 0, 0);
        movzRaw(3, 0, 0);
        movzRaw(4, 0, 0);
        movzRaw(8, 220, 0);            // __NR_clone（fork 语义）
        syscall();
        push(0);
    }
    void heapMmapFirst() {        // 入参 x0 = 堆槽地址；出参 x0 = 槽地址，x1 = base
        movReg(19, 0);
        movzRaw(0, 0, 0);                 // addr = NULL
        mov64iRaw(1, 0x4000000);          // len = 64MB
        movzRaw(2, 3, 0);                 // prot = READ|WRITE
        movzRaw(3, 0x22, 0);              // flags = PRIVATE|ANONYMOUS
        mov64iRaw(4, (int64_t)-1);        // fd = -1
        movzRaw(5, 0, 0);                 // off = 0
        movzRaw(8, 222, 0);               // __NR_mmap
        syscall();
        movReg(1, 0);                     // x1 = base
        strRaw(0, 19, 0);                 // cur = base
        mov64iRaw(10, 0x4000000);
        addReg(10, 0, 10);                // end = base + 64MB
        strRaw(10, 19, 8);
        movReg(0, 19);                    // x0 = 槽地址
    }
    void memmoveRaw() {
        // RDI→x5=dst, RSI→x4=src, RCX→x1=n
        int dst = 5, src = 4, n = 1;
        int lback = newLabel(), lfwd = newLabel(), ldone = newLabel();
        emit32(0xF100003Fu);               // cmp x1, #0
        bcondRaw(0, ldone);                // b.eq done
        emit32(0xEB00001Fu | ((uint32_t)src << 16) | ((uint32_t)dst << 5));  // cmp dst, src
        bcondRaw(9, lfwd);                 // LS：dst <= src → 正向
        addReg(9, src, n);                 // src_end = src + n
        emit32(0xEB00001Fu | (9u << 16) | ((uint32_t)dst << 5));  // cmp dst, src_end
        bcondRaw(2, lfwd);                 // HS：dst >= src+n → 正向
        addReg(src, src, n);
        addReg(dst, dst, n);
        bind(lback);
        emit32(0xF100003Fu);               // cmp x1, #0
        bcondRaw(0, ldone);
        emit32(0xD1000400u | ((uint32_t)src << 5) | (uint32_t)src);  // sub src,src,#1
        emit32(0xD1000400u | ((uint32_t)dst << 5) | (uint32_t)dst);
        ldrbRaw(16, src, 0);
        strbRaw(16, dst, 0);
        emit32(0xD1000400u | ((uint32_t)n << 5) | (uint32_t)n);
        jmp32(lback);
        bind(lfwd);
        emit32(0xF100003Fu);               // cmp x1, #0
        bcondRaw(0, ldone);
        ldrbRaw(16, src, 0);
        strbRaw(16, dst, 0);
        emit32(0x91000400u | ((uint32_t)src << 5) | (uint32_t)src);
        emit32(0x91000400u | ((uint32_t)dst << 5) | (uint32_t)dst);
        emit32(0xD1000400u | ((uint32_t)n << 5) | (uint32_t)n);
        jmp32(lfwd);
        bind(ldone);
    }
    void frameEnter(int frameSize) {
        // 机器栈只保存 LR 与调用者帧指针（stp 16 字节，保持 sp 16 对齐）
        emit32(0xA9BF7BFDu);        // stp x29, x30, [sp, #-16]!
        movReg(29, 28);             // x29 = 值栈帧指针（= &最后一个实参）
        subRsp(frameSize);          // 在值栈上预留局部槽
    }
    void frameLeave() {
        movReg(28, 29);             // 值栈指针复位到实参基址（调用方随后清参）
        emit32(0xA8C17BFDu);        // ldp x29, x30, [sp], #16
        ret();
    }
    void entrySetup() {
        // 快照初始机器 sp（argc/argv/envp）到固定槽
        mov64iRaw(0, (int64_t)kSlotInitStack);
        addAddr(16, 31, 0);             // mov x16, sp
        strRaw(16, 0, 0);
        // 独立的值栈：mmap 64MB，x28 指向栈顶（向下增长）
        movzRaw(0, 0, 0);
        mov64iRaw(1, 0x4000000);
        movzRaw(2, 3, 0);
        movzRaw(3, 0x22, 0);
        mov64iRaw(4, (int64_t)-1);
        movzRaw(5, 0, 0);
        movzRaw(8, 222, 0);             // __NR_mmap
        syscall();
        mov64iRaw(16, 0x4000000);
        addReg(28, 0, 16);              // x28 = base + 64MB
    }
    void exitMain() {
        movzRaw(8, 94, 0);              // __NR_exit_group（arm64 = 94）
        syscall();
    }
    void exitGroupReg(int r) {
        movReg(0, R(r));
        movzRaw(8, 94, 0);
        syscall();
    }
};

// =============================================================================
//  ELF64 输出（AArch64，单 RWX LOAD 段）
// =============================================================================
static std::vector<uint8_t> buildElf(const Arm64& c, const std::vector<uint8_t>& data,
                                     size_t entryOff, size_t dataSegOff) {
    size_t codeOff = 128;
    size_t total = dataSegOff + data.size();
    constexpr uint64_t kMinMemSz = 0x800000;
    uint64_t memsz = total < kMinMemSz ? kMinMemSz : (uint64_t)total;
    std::vector<uint8_t> img(total, 0);
    const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(img.data(), ident, 16);
    put16(img, 16, 2);      // e_type = ET_EXEC
    put16(img, 18, 0xB7);   // e_machine = EM_AARCH64
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
    put64(img, 104, memsz); // p_memsz
    put64(img, 112, 0x1000);
    std::memcpy(img.data() + codeOff, c.code.data(), c.code.size());
    if (!data.empty())
        std::memcpy(img.data() + dataSegOff, data.data(), data.size());
    return img;
}
