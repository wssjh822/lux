// =============================================================================
//  代码生成主类
// =============================================================================

struct Var {
    std::string name;
    int slot;  // rbp 相对偏移（负数）
    const Ty* ty = nullptr;
};

// 数组元素种类
// 0.9.4：新增 EK_STRUCT —— struct 数组的元素是堆指针，ARC 释放时要逐个 release
enum { EK_INT = 0, EK_FLOAT = 1, EK_BOOL = 2, EK_STR = 3, EK_ARR = 4, EK_STRUCT = 5 };

static int ekindOf(const Ty* t) {
    switch (t->kind) {
        case TyKind::Float: return EK_FLOAT;
        case TyKind::Bool: return EK_BOOL;
        case TyKind::String: return EK_STR;
        case TyKind::Array: return EK_ARR;
        case TyKind::Named: return EK_STRUCT;  // 0.9.4：struct 数组（引用元素）
        default: return EK_INT;
    }
}

// 类型中（递归地）是否含 struct。含 struct 的数组不能走通用
// luxrt_arr_to_str（元素种类表里 Named 被当成 EK_INT），
// 必须由 codegen 逐层展开成字段字符串。
static bool containsNamed(const Ty* t) {
    if (!t) return false;
    if (t->kind == TyKind::Named) return true;
    if (t->kind == TyKind::Array) return containsNamed(t->elem);
    return false;
}

struct NativeGen {
    NativeArch c;
    std::vector<uint8_t> data;
    std::unordered_map<std::string, size_t> strPool;
    std::unordered_map<std::string, int> symLabels;
    std::unordered_set<std::string> definedFns;
    size_t entryOff = 0;
    int uid_ = 0;

    std::vector<std::vector<Var>> scopes;
    int frameSize = 0;
    std::vector<int> breakLbl, contLbl;
    std::vector<int> breakScope, contScope;  // 1.1：break/continue 时要释放到的作用域深度
    bool inRuntime = false;
    bool mainHasArgv_ = false;
    const Ty* curRet = nullptr;
    Diags* diags = nullptr;
    std::string curFile;
    std::string curFnKey;
    std::string curSourceName;
    std::unordered_map<std::string, StructDecl*> structs_;  // struct 名 -> 声明（0.7）

    // 数组元素种类编码（与 native_rt.lux 的 kEk* 一致；1.1 map/filter 用）
    static int ekOf(const Ty* t) {
        if (!t) return 0;
        switch (t->kind) {
            case TyKind::Float: return 1;
            case TyKind::Bool: return 2;
            case TyKind::String: return 3;
            case TyKind::Array: return 4;
            case TyKind::Named: return 5;
            default: return 0;
        }
    }

    // ---- 数据段 ----
    // 0.9.4（P0-1）：字符串字面量也带 32 字节计数头（refs = -1 的静态不可变
    // 对象），这样 retain/release 读到 -1 直接跳过，不会去动只读数据段。
    // 返回的是**载荷偏移**（[len][bytes][NUL] 的起点），所有引用字面量的
    // 代码路径保持原样。
    size_t addStr(const std::string& s) {
        auto it = strPool.find(s);
        if (it != strPool.end()) return it->second;
        while (data.size() % 8) data.push_back(0);
        // 头：refs = -1，desc / class / 保留 = 0
        for (int i = 0; i < 8; i++) data.push_back(0xFF);
        for (int i = 0; i < 24; i++) data.push_back(0);
        size_t at = data.size();
        strPool[s] = at;
        uint64_t len = s.size();
        for (int i = 0; i < 8; i++) data.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
        data.insert(data.end(), s.begin(), s.end());
        data.push_back(0);  // 恒以 NUL 结尾（execve / system 直接引用数据段字符串）
        while (data.size() % 8) data.push_back(0);
        return at;
    }
    // struct 引用字段描述符（数据段）：[count][idx0][idx1]…
    // release 时按它逐个释放字段引用。没有引用字段的 struct 用 0（= kDescNone）。
    std::unordered_map<std::string, size_t> structDesc_;
    size_t structDescOf(StructDecl* sd) {
        std::vector<int64_t> refs;
        for (size_t i = 0; i < sd->fields.size(); i++)
            if (eIsRefTy(sd->fields[i].ty)) refs.push_back((int64_t)i);
        if (refs.empty()) return 0;
        auto it = structDesc_.find(sd->name);
        if (it != structDesc_.end()) return it->second;
        while (data.size() % 8) data.push_back(0);
        size_t at = data.size();
        addQword((uint64_t)refs.size());
        for (int64_t r : refs) addQword((uint64_t)r);
        structDesc_[sd->name] = at;
        return at;
    }

    size_t addQword(uint64_t v) {
        size_t at = data.size();
        for (int i = 0; i < 8; i++) data.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
        return at;
    }

    // ---- 符号 ----
    static std::string keyOf(const FuncDecl& fn) {
        return fn.module.empty() ? fn.name : fn.module + "\x01" + fn.name;
    }
    int symLabel(const std::string& key) {
        auto it = symLabels.find(key);
        if (it != symLabels.end()) return it->second;
        int l = c.newLabel();
        symLabels[key] = l;
        return l;
    }
    int bindFn(const std::string& key, const std::string& display, SourceLoc loc) {
        if (!definedFns.insert(key).second) {
            if (diags)
                diags->error(DiagCode::kSema, curFile, loc,
                             "函数 '" + display +
                                 "' 与原生运行时保留名冲突（luxrt_ 前缀）");
            return -1;
        }
        int lb = symLabel(key);
        c.bind(lb);
        return lb;
    }

    void err(SourceLoc loc, const std::string& msg) {
        if (diags) diags->error(DiagCode::kSema, curFile, loc, msg);
    }

    // ---- 作用域 / 变量 ----
    void pushScope() { scopes.emplace_back(); }
    void popScope() { scopes.pop_back(); }
    Var* findVar(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            for (auto& v : *it)
                if (v.name == name) return &v;
        return nullptr;
    }
    int declareVar(const std::string& name, const Ty* ty = nullptr) {
        frameSize += 8;
        scopes.back().push_back(Var{name, -frameSize, ty});
        return -frameSize;
    }

    // ---- 栈操作 ----
    void pushImm(int64_t v) {
        c.push(RAX);
        c.mov64i(RAX, v);
        c.movmr(RSP, 0, RAX);
    }
    void pushStrRef(const std::string& s) {
        size_t at = addStr(s);
        c.push(RAX);
        c.movAbsData(RAX, at);   // 数据地址（布局后回填）
        c.movmr(RSP, 0, RAX);
    }
    void addRspImm(int n) { c.addRsp(n); }
    void subRspImm(int n) { c.subRsp(n); }
    void pushFloatXmm0() {
        subRspImm(8);
        c.movsdToMem(0, RSP, 0);
    }
    // 栈顶 int（补码）→ double 位模式（隐式 int→float 提升）
    void fpromoteTop() {
        c.pop(RAX);
        c.cvtsi2sd(0, RAX);
        pushFloatXmm0();
    }
    // 就地把 [rsp+off] 的 int 提升为 double 位模式（混合 int/float 运算用）
    void fpromoteMem(int off) {
        c.movrm(RAX, RSP, off);
        c.cvtsi2sd(0, RAX);
        c.movsdToMem(0, RSP, off);
    }

    // ---- 可选值（0.8 错误通道） ----
    // T? 统一是 8 字节槽里的指针，0 = none；装箱 / 解包都走运行时。
    // 0.9.4（P0-1）：装箱槽带引用计数头，内层是引用类型时传 kDescOpt，
    // 槽归零时顺带释放内层。
    // 把值栈顶的 T 装箱成 T?（栈顶被替换成箱指针）。
    // 参数顺序 push 左→右：arg0 = 值，arg1 = desc（1 = 内层是引用类型）。
    void boxTop(const Ty* elem) {
        bool refInner = eIsRefTy(elem);
        c.pop(RAX);                        // 取出待装箱的值
        c.push(RAX);                       // arg0 = v
        c.mov64i(RCX, refInner ? 1 : 0);
        c.push(RCX);                       // arg1 = desc
        c.call32(symLabel("luxrt_opt_box"));
        addRspImm(16);
        c.push(RAX);
    }

    // =====================================================================
    //  ARC（0.9.4，P0-1）
    //  模型：运行时 alloc 会把对象登记进本帧的 pending 区；每条语句结束时
    //  flush 掉本帧登记的全部对象。「存进变量 / 数组 / 结构体 / 返回值」的
    //  位置先 retain，引用计数天然配平。多 retain 只会短暂多占内存，不会
    //  误释放 —— 与 docs/arc.md §2.2「先做对，再做省」一致。
    // =====================================================================
    bool arcOn = false;
    int slotPend = 0;   // 本函数的 pending 基准深度
    int slotRv = 0;     // 返回值暂存（跨 flush 保住 RAX / xmm0）
    // 当前语句块内声明、需要在本块出口释放（含 flush 后仍持有引用）的局部槽
    std::vector<std::vector<int>> arcLocals;

    static bool eIsRefTy(const Ty* t) {
        if (!t) return false;
        return t->kind == TyKind::String || t->kind == TyKind::Array ||
               t->kind == TyKind::Named;
    }
    static bool eNeedsOwn(const Ty* t) {
        return eIsRefTy(t) || (t && t->kind == TyKind::Optional);
    }

    // 登记 / 保留栈顶的值（净栈效果为 0）
    void callWithTop(const char* fn) {
        c.push(RAX);
        c.movrm(RAX, RSP, 8);
        c.push(RAX);
        c.call32(symLabel(fn));
        addRspImm(8);
        c.pop(RAX);
    }
    void retainTop() { callWithTop("luxrt_retain"); }
    void pendPushTop() { callWithTop("luxrt_pend_push"); }

    // 释放本帧登记的全部临时对象
    void emitPendFlush() {
        c.push(RAX);
        c.movrm(RAX, RBP, slotPend);
        c.push(RAX);
        c.call32(symLabel("luxrt_pend_flush_to"));
        addRspImm(8);
        c.pop(RAX);
    }

    // 释放 [rbp+slot] 持有的引用并置零（块出口用；置零保证不会重复释放）
    void releaseLocal(int slot) {
        c.push(RAX);
        c.movrm(RAX, RBP, slot);
        c.push(RAX);
        c.call32(symLabel("luxrt_release"));
        addRspImm(8);
        c.pop(RAX);
        // 1.1：置零用 movmi，不能用 RAX 中转（调用方可能正用 RAX 持有返回值）
        c.movmi(RBP, slot, 0);
    }
    // 让 [rbp+slot] 自持一份引用（跨语句存活的值用）
    void retainLocal(int slot) {
        c.push(RAX);
        c.movrm(RAX, RBP, slot);
        c.push(RAX);
        c.call32(symLabel("luxrt_retain"));
        addRspImm(8);
        c.pop(RAX);
    }
    void releaseScope(int from) {
        if (!arcOn) return;
        while ((int)arcLocals.size() > from) {
            for (int slot : arcLocals.back()) releaseLocal(slot);
            arcLocals.pop_back();
        }
    }
    void pushArcScope() { arcLocals.emplace_back(); }
    void popArcScope() { if (!arcLocals.empty()) arcLocals.pop_back(); }
    // 1.1：只发射释放、不改变编译期作用域栈（供 break/continue/return 的
    // 提前离开路径用——不能 pop，否则正常路径的块出口释放会丢失）
    void emitReleaseDownTo(int from) {
        if (!arcOn) return;
        for (int i = (int)arcLocals.size() - 1; i >= from; i--) {
            for (int slot : arcLocals[i]) releaseLocal(slot);
        }
    }
    void noteArcLocal(int slot) {
        if (arcOn && !arcLocals.empty()) arcLocals.back().push_back(slot);
    }

    // 函数出口：retain 返回值 → flush 本帧临时对象 → 把返回值登记给调用方
    void emitFnExit(bool hasRet, bool retOwned) {
        if (!arcOn) return;
        if (hasRet) c.movmr(RBP, slotRv, RAX);
        if (hasRet && retOwned) {
            c.push(RAX);
            c.movrm(RAX, RBP, slotRv);
            c.push(RAX);
            c.call32(symLabel("luxrt_retain"));
            addRspImm(8);
            c.pop(RAX);
        }
        c.push(RAX);
        c.movrm(RAX, RBP, slotPend);
        c.push(RAX);
        c.call32(symLabel("luxrt_pend_flush_to"));
        addRspImm(8);
        c.pop(RAX);
        if (hasRet && retOwned) {
            // 只有引用型 / 可选型返回值登记进调用方的 pending 区；
            // 值类型（int / float / bool）绝不能登记 —— 否则 flush 会把
            // 整数当指针去 release。
            c.push(RAX);
            c.movrm(RAX, RBP, slotRv);
            c.push(RAX);
            c.call32(symLabel("luxrt_pend_push"));
            addRspImm(8);
            c.pop(RAX);
        }
        if (hasRet) c.movrm(RAX, RBP, slotRv);
    }
    // 浮点返回：值在 xmm0，跨 flush 需要经槽保存
    void emitFnExitFloat() {
        if (!arcOn) return;
        c.movsdToMem(0, RBP, slotRv);
        c.push(RAX);
        c.movrm(RAX, RBP, slotPend);
        c.push(RAX);
        c.call32(symLabel("luxrt_pend_flush_to"));
        addRspImm(8);
        c.pop(RAX);
        c.movsdFromMem(0, RBP, slotRv);
    }

    // 语句级 flush
    void emitStmtFlush() {
        if (!arcOn) return;
        c.push(RAX);
        c.movrm(RAX, RBP, slotPend);
        c.push(RAX);
        c.call32(symLabel("luxrt_pend_flush_to"));
        addRspImm(8);
        c.pop(RAX);
    }
    // 求值 e 并按目标类型 to 做隐式转换（int→float / T→T? 装箱）
    void evalCoerced(Expr* e, const Ty* to) {
        evalExpr(e);
        const Ty* from = e ? e->ty : nullptr;
        if (!from || !to || from == to) return;
        if (from->kind == TyKind::Int && to->kind == TyKind::Float) {
            fpromoteTop();
            return;
        }
        if (to->kind == TyKind::Optional) {
            if (from == to->elem) { boxTop(to->elem); return; }
            if (from->kind == TyKind::Int && to->elem &&
                to->elem->kind == TyKind::Float) {
                fpromoteTop();
                boxTop(to->elem);
            }
        }
    }
    // RAX 中是 T? 指针；为 none 时 panic，否则换成 T 压栈
    void panicUnwrapTop(const std::string& what, const Ty* elem) {
        int lok = c.newLabel();
        c.testRR(RAX, RAX);
        c.jcc32(CJne, lok);
        pushStrRef(what);
        c.call32(symLabel("luxrt_panic_opt"));
        addRspImm(8);
        c.bind(lok);
        c.movrm(RAX, RAX, 0);
        c.push(RAX);
        if (arcOn && eIsRefTy(elem)) {
            // 解包出的载荷自持一份引用（空槽稍后被 flush 回收，会释放它那一份）
            retainTop();
            pendPushTop();
        }
    }
    // read! 专用：栈顶还保留着路径（调用方多压了一份），panic 时一起报出来。
    // RAX 中是 lx_read_opt 的返回（string?）。
    void panicUnwrapFileTop() {
        int lok = c.newLabel();
        c.testRR(RAX, RAX);
        c.jcc32(CJne, lok);
        // 栈：[path]；luxrt_panic_opt_file(what, path)
        pushStrRef("文件读取");
        c.movrm(RAX, RSP, 8);   // 取出保存的 path
        c.push(RAX);
        c.call32(symLabel("luxrt_panic_opt_file"));
        addRspImm(24);
        c.bind(lok);
        c.movrm(RAX, RAX, 0);
        addRspImm(8);           // 丢掉保存的 path
        c.push(RAX);
        if (arcOn) {
            // string? 的载荷是引用类型：自持引用 + 登记
            retainTop();
            pendPushTop();
        }
    }
    // expr?（0.8）：为 none 时返回 none（当前函数必须返回 T?）
    void evalTry(TryExpr* e) {
        evalExpr(e->operand);
        c.pop(RAX);
        int lok = c.newLabel();
        c.testRR(RAX, RAX);
        c.jcc32(CJne, lok);
        c.zeroReg(RAX);
        emitFnExit(false, false);   // 返回 none：先归还本帧临时对象
        c.frameLeave();
        c.bind(lok);
        c.movrm(RAX, RAX, 0);
        c.push(RAX);
        if (arcOn && eIsRefTy(e->ty)) {
            retainTop();
            pendPushTop();
        }
    }
    // inc qword [base+disp]（for 循环自增用）
    void incMem(int base, int32_t disp) { c.incMem(base, disp); }

    // ---- 函数调用（参数已按左到右求值压栈） ----
    void callRet(const Ty* retTy) {
        if (retTy && retTy->kind == TyKind::Float) {
            // 返回值 xmm0 → 压栈（位模式）
            subRspImm(8);
            c.movsdToMem(0, RSP, 0);
        } else if (retTy && retTy->kind != TyKind::Void) {
            c.push(RAX);
        }
    }
    void callPanicMsg(const char* msg) {
        pushStrRef(msg);
        c.call32(symLabel("luxrt_panic_msg"));
        addRspImm(8);  // 不会返回，仅为形式平衡
    }
    void callPanic1(const char* rtFn, Expr* a) {
        evalExpr(a);
        c.call32(symLabel(rtFn));
        addRspImm(8);
    }

    // ---- 数据加载 / 存取（数组元素，SIB 寻址） ----
    void movSib(int dst, int base, int idx, int scale) {  // mov r64, [base+idx*scale]
        c.movSib(dst, base, idx, scale);
    }
    void movSibStore(int src, int base, int idx, int scale) {  // mov [base+idx*scale], r64
        c.movSibStore(src, base, idx, scale);
    }
    void movsdSib(int xmm, int base, int idx, int scale) {  // movsd xmm, [base+idx*scale]
        c.movsdSib(xmm, base, idx, scale);
    }

    // ---- 数组下标检查 + 元素读取 ----
    // 栈布局：[rsp]=i, [rsp+8]=arr（不弹出）；越界 panic（不返回）
    void arrLoadCheck(const char* oobFn) {
        c.movrm(RAX, RSP, 0);      // i
        c.movrm(RCX, RSP, 8);      // arr
        c.opRM(0x3B, RAX, RCX, 0); // cmp rax, [rcx]（len）
        int loob = c.newLabel(), lok = c.newLabel();
        c.jcc32(CJge, loob);
        c.testRR(RAX, RAX);
        c.jcc32(CJl, loob);
        c.jmp32(lok);
        c.bind(loob);
        // luxrt_panic_xxx_oob(i, len)：p0=i（先压）、p1=len（后压）
        c.push(RAX);
        c.push(RCX);
        c.call32(symLabel(oobFn));
        c.bind(lok);
    }

    // =====================================================================
    //  表达式求值：结果压栈（统一 8 字节槽）
    // =====================================================================
    void evalExpr(Expr* e) {
        switch (e->kind) {
            case ExprKind::IntLit:
                pushImm(static_cast<IntLitExpr*>(e)->value);
                return;
            case ExprKind::FloatLit: {
                double d = static_cast<FloatLitExpr*>(e)->value;
                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                pushImm((int64_t)bits);
                return;
            }
            case ExprKind::BoolLit:
                pushImm(static_cast<BoolLitExpr*>(e)->value ? 1 : 0);
                return;
            case ExprKind::StrLit:
                pushStrRef(static_cast<StrLitExpr*>(e)->value);
                return;
            case ExprKind::Ident: {
                auto* id = static_cast<IdentExpr*>(e);
                // 1.1：函数作为值 → 函数代码地址（8 字节地址槽）
                if (id->funcRef) {
                    c.push(RAX);
                    c.movAbsLabel(RAX, symLabel(keyOf(*id->funcRef)));
                    c.movmr(RSP, 0, RAX);
                    return;
                }
                if (id->constRef) { evalConstRef(id->constRef); return; }
                Var* v = findVar(id->name);
                if (!v) { pushImm(0); return; }  // Sema 已报错
                c.push(RAX);
                c.movrm(RAX, RBP, v->slot);
                c.movmr(RSP, 0, RAX);
                return;
            }
            case ExprKind::ArrayLit:
                evalArrayLit(static_cast<ArrayLitExpr*>(e));
                return;
            case ExprKind::StructLit:
                evalStructLit(static_cast<StructLitExpr*>(e));
                return;
            case ExprKind::Member:
                evalMember(static_cast<MemberExpr*>(e));
                return;
            case ExprKind::If:
                evalIfExpr(static_cast<IfExpr*>(e));
                return;
            case ExprKind::NoneLit:
                pushImm(0);  // none = 0
                return;
            case ExprKind::Try:
                evalTry(static_cast<TryExpr*>(e));
                return;
            case ExprKind::Index:
                evalIndex(static_cast<IndexExpr*>(e));
                return;
            case ExprKind::Slice:
                evalSlice(static_cast<SliceExpr*>(e));
                return;
            case ExprKind::Unary:
                evalUnary(static_cast<UnaryExpr*>(e));
                return;
            case ExprKind::Binary:
                evalBinary(static_cast<BinaryExpr*>(e));
                return;
            case ExprKind::Call:
                evalCall(static_cast<CallExpr*>(e));
                return;
            default:
                err(e->loc, "编译器内部错误：未知的表达式节点");
                pushImm(0);
                return;
        }
    }

    void evalConstRef(GlobalConstDecl* gc) {
        Expr* ini = gc->init;
        if (!ini) { pushImm(0); return; }
        // const B = A;（引用另一个全局常量）：Sema 已递归求值，这里递归取初值
        if (ini->kind == ExprKind::Ident) {
            auto* id = static_cast<IdentExpr*>(ini);
            if (id->constRef) { evalConstRef(id->constRef); return; }
            pushImm(0);
            return;
        }
        bool isF = gc->resolved && gc->resolved->kind == TyKind::Float;
        if (ini->kind == ExprKind::StrLit) {
            pushStrRef(static_cast<StrLitExpr*>(ini)->value);
            return;
        }
        if (ini->kind == ExprKind::Binary) {
            auto* b = static_cast<BinaryExpr*>(ini);
            if (b->folded) {
                if (b->foldedIsStr) { pushStrRef(b->foldedS); return; }
                if (isF) { pushFBits(b->foldedF); return; }
                pushImm(b->foldedI);
                return;
            }
        }
        if (isF && ini->kind == ExprKind::FloatLit) {
            pushFBits(static_cast<FloatLitExpr*>(ini)->value);
            return;
        }
        if (ini->kind == ExprKind::FloatLit) {
            pushFBits(static_cast<FloatLitExpr*>(ini)->value);
            return;
        }
        if (ini->kind == ExprKind::IntLit) {
            pushImm(static_cast<IntLitExpr*>(ini)->value);
            return;
        }
        if (ini->kind == ExprKind::BoolLit) {
            pushImm(static_cast<BoolLitExpr*>(ini)->value ? 1 : 0);
            return;
        }
        pushImm(0);
    }
    void pushFBits(double d) {
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        pushImm((int64_t)bits);
    }

    // struct 字面量（0.7）：堆上 N*8 字段对象，逐字段写入
    void evalStructLit(StructLitExpr* e) {
        StructDecl* sd = e->decl;
        if (!sd) { pushImm(0); return; }
        int n = static_cast<int>(sd->fields.size());
        pushImm(n * 8);
        c.call32(symLabel("luxrt_struct_new"));
        addRspImm(8);
        int slot = declareVar("__st" + std::to_string(uid_++));
        // 0.9.4（P0-1）：登记引用字段描述符，release 时按它释放字段引用
        if (arcOn) {
            size_t doff = structDescOf(sd);
            c.movmr(RBP, slot, RAX);          // 先存进槽，腾出 RAX
            c.movrm(RAX, RBP, slot);
            c.push(RAX);                      // arg0 = p
            c.movAbsData(RAX, doff);
            c.push(RAX);                      // arg1 = desc（数据段地址）
            c.call32(symLabel("luxrt_struct_desc"));
            addRspImm(16);
            c.movrm(RAX, RBP, slot);          // 恢复 RAX = struct
        }
        c.movmr(RBP, slot, RAX);
        for (auto& kv : e->inits) {
            int idx = -1;
            for (size_t i = 0; i < sd->fields.size(); i++)
                if (sd->fields[i].name == kv.first) idx = static_cast<int>(i);
            if (idx < 0) continue;
            const Ty* ft = sd->fields[idx].ty;
            evalCoerced(kv.second, ft);
            // 0.9.4：引用型字段值由 struct 持有一份
            if (arcOn && eIsRefTy(ft)) retainTop();
            c.pop(RAX);
            c.movrm(RCX, RBP, slot);
            c.movmr(RCX, idx * 8, RAX);
        }
        c.push(RAX);
        c.movrm(RAX, RBP, slot);
        c.movmr(RSP, 0, RAX);
    }

    // 成员读取 p.x（0.7）：base 求值为 struct 指针，读 [ptr + idx*8]
    void evalMember(MemberExpr* e) {
        if (e->constRef) { evalConstRef(e->constRef); return; }
        evalExpr(e->base);
        c.pop(RAX);
        c.movrm(RAX, RAX, e->fieldIndex * 8);
        c.push(RAX);
    }

    // if 表达式（0.7）：两分支各留一槽，含 int→float 提升
    void evalIfExpr(IfExpr* e) {
        evalExpr(e->cond);
        c.pop(RAX);
        c.testRR(RAX, RAX);
        int lelse = c.newLabel(), lend = c.newLabel();
        c.jcc32(CJe, lelse);
        evalCoerced(e->thenVal, e->ty);
        c.jmp32(lend);
        c.bind(lelse);
        evalCoerced(e->elseVal, e->ty);
        c.bind(lend);
    }

    void evalArrayLit(ArrayLitExpr* e) {
        int ek = ekindOf(e->elemTy);
        pushImm(ek);
        c.call32(symLabel("luxrt_arr_new"));
        // call 返回后：rax = 数组指针，[rsp] = ek 参数槽（call 不动数据栈）
        int slot = declareVar("__arrlit" + std::to_string(uid_++));
        c.movmr(RBP, slot, RAX);  // 直接从 rax 存槽
        addRspImm(8);             // 清 ek 参数槽
        for (Expr* el : e->elems) {
            // 压 a 副本（p0），求值 el 压栈（p1）
            c.push(RAX);
            c.movrm(RAX, RBP, slot);
            c.movmr(RSP, 0, RAX);
            evalExpr(el);
            // int 元素进 float[] 字面量（[1.5, 2, 3]）：转 double 位模式
            if (e->elemTy && e->elemTy->kind == TyKind::Float &&
                el->ty && el->ty->kind == TyKind::Int) {
                fpromoteTop();
            }
            c.call32(symLabel("luxrt_arr_push"));
            addRspImm(16);
        }
        c.push(RAX);
        c.movrm(RAX, RBP, slot);
        c.movmr(RSP, 0, RAX);
    }

    // 切片 a[lo..hi] / a[lo..=hi]（0.6 新增）：交给运行时做复制与越界
    // 检查。栈序：base 先压（最深），再 lo，最后 hi 在 [rsp]。
    // end 缺省 = [base]（len 字段，数组和字符串布局一致）。
    void evalSlice(SliceExpr* e) {
        const Ty* bt = e->base->ty;
        evalExpr(e->base);
        if (e->start)
            evalExpr(e->start);
        else
            pushImm(0);
        if (e->end) {
            evalExpr(e->end);
            if (e->inclusive) {
                // ..=: hi+1（排他上界），incl qword [rsp]
                c.incMem(RSP, 0);
            }
        } else {
            // end 缺省：此时栈上只有 [lo][base] 两槽，base 在 [rsp+8]
            c.movrm(RAX, RSP, 8);    // base
            c.movrm(RAX, RAX, 0);    // len
            c.push(RAX);
        }
        c.call32(symLabel(bt && bt->kind == TyKind::String
                              ? "luxrt_str_slice"
                              : "luxrt_arr_slice"));
        addRspImm(24);
        callRet(e->ty);
    }

    void evalIndex(IndexExpr* e) {
        const Ty* bt = e->base->ty;
        evalExpr(e->base);
        evalExpr(e->index);
        if (bt && bt->kind == TyKind::String) {
            // s[i] → luxrt_str_char_at(s, i)：p0=s（先压）✓ [rsp]=i,[rsp+8]=s
            // 检查 i∈[0,len)
            c.movrm(RAX, RSP, 0);      // i
            c.movrm(RCX, RSP, 8);      // s
            c.opRM(0x3B, RAX, RCX, 0);
            int loob = c.newLabel();
            c.jcc32(CJge, loob);
            c.testRR(RAX, RAX);
            c.jcc32(CJl, loob);
            int lok = c.newLabel();
            c.jmp32(lok);
            c.bind(loob);
            c.push(RCX);
            c.push(RAX);
            c.call32(symLabel("luxrt_panic_str_oob"));
            c.bind(lok);
            c.call32(symLabel("luxrt_str_char_at"));
            addRspImm(16);
            c.push(RAX);
            return;
        }
        bool elemF = e->ty && e->ty->kind == TyKind::Float;
        arrLoadCheck("luxrt_panic_arr_oob");
        // 栈：[rsp]=i, [rsp+8]=arr
        c.movrm(RAX, RSP, 0);
        c.movrm(RCX, RSP, 8);
        c.movrm(RDX, RCX, 16);     // data
        if (elemF) {
            c.movsdSib(0, RDX, RAX, 3);
            addRspImm(16);
            pushFloatXmm0();
        } else {
            movSib(RCX, RDX, RAX, 3);
            addRspImm(16);
            c.push(RCX);
        }
    }

    void evalUnary(UnaryExpr* e) {
        evalExpr(e->operand);
        bool isF = e->ty && e->ty->kind == TyKind::Float;
        switch (e->op) {
            case UnOp::Neg:
                if (isF) {
                    // 翻转符号位
                    c.movsdFromMem(0, RSP, 0);
                    c.mov64i(RAX, (int64_t)0x8000000000000000ULL);
                    c.movqXmmR(1, RAX);
                    c.pdRR(0x57, 0, 1);  // xorpd xmm0, xmm1
                    c.movsdToMem(0, RSP, 0);
                } else {
                    c.pop(RAX);
                    c.negr(RAX);
                    c.push(RAX);
                }
                return;
            case UnOp::LogicNot:
                c.pop(RAX);
                c.testRR(RAX, RAX);
                c.setcc(CJe, RAX);
                c.push(RAX);
                return;
            case UnOp::BitNot:
                c.pop(RAX);
                c.notr(RAX);
                c.push(RAX);
                return;
        }
    }

    void evalBinary(BinaryExpr* e) {
        // 0.8：or 兜底（lhs 是 T?）
        if (e->orFallback) {
            const Ty* elem = e->lhs->ty->elem;
            evalExpr(e->lhs);
            c.pop(RAX);
            int lnone = c.newLabel(), lend = c.newLabel();
            c.testRR(RAX, RAX);
            c.jcc32(CJe, lnone);
            c.movrm(RAX, RAX, 0);
            c.push(RAX);
            if (arcOn && eIsRefTy(elem)) {
                // 命中分支：引用类型载荷自持一份引用 + 登记（空槽由 flush 回收）
                retainTop();
                pendPushTop();
            }
            c.jmp32(lend);
            c.bind(lnone);
            // 兜底分支：rhs 若是函数调用，其返回值已由被调方登记进 pending；
            // 若是变量 / 字面量则本就不需要回收。两种情况都**不能**再登记一次
            // —— 重复登记会导致语句 flush 释放两次（双重释放）。
            evalCoerced(e->rhs, elem);
            c.bind(lend);
            return;
        }
        // 常量折叠（Sema）
        if (e->folded) {
            if (e->foldedIsStr) { pushStrRef(e->foldedS); return; }
            if (e->ty && e->ty->kind == TyKind::Float) { pushFBits(e->foldedF); return; }
            pushImm(e->foldedI);
            return;
        }
        // 浮点性看操作数而不是结果类型：比较（1 == 1.0）的结果是 Bool，
        // 混合算术（1 + 2.5）的操作数里有 int，都需要按浮点路径处理
        bool isF = (e->lhs->ty && e->lhs->ty->kind == TyKind::Float) ||
                   (e->rhs->ty && e->rhs->ty->kind == TyKind::Float);
        bool isS = e->lhs->ty && e->lhs->ty->kind == TyKind::String;

        if (e->op == BinOp::LogicAnd || e->op == BinOp::LogicOr) {
            bool isAnd = e->op == BinOp::LogicAnd;
            evalExpr(e->lhs);
            int lshort = c.newLabel(), lend = c.newLabel();
            c.cmpRI(RSP, 0, 0);
            // 注意：必须先跳转再清槽！addRspImm 会覆盖 cmp 设置的标志位
            // （add 结果=rsp+8 恒非零 → ZF=0 → jcc 恒走短路分支）。
            c.jcc32(isAnd ? CJe : CJne, lshort);
            addRspImm(8);  // 非短路路径：清 cond1 槽
            evalExpr(e->rhs);
            c.pop(RAX);
            c.testRR(RAX, RAX);
            c.setcc(CJne, RAX);
            c.push(RAX);
            c.jmp32(lend);
            c.bind(lshort);
            addRspImm(8);  // 短路路径：清 cond1 槽
            pushImm(isAnd ? 0 : 1);
            c.bind(lend);
            return;
        }

        evalExpr(e->lhs);
        evalExpr(e->rhs);  // [rsp]=rhs, [rsp+8]=lhs

        if (isF) {
            // int 操作数就地提升为 double（[rsp]=rhs、[rsp+8]=lhs）
            // 此前 int 操作数以原始位模式参与浮点运算/比较，1 + 2.0 会得 2
            if (!(e->lhs->ty && e->lhs->ty->kind == TyKind::Float)) fpromoteMem(8);
            if (!(e->rhs->ty && e->rhs->ty->kind == TyKind::Float)) fpromoteMem(0);
            evalBinaryFloat(e->op);
            return;
        }
        if (isS && (e->op == BinOp::Eq || e->op == BinOp::Ne)) {
            c.call32(symLabel("luxrt_str_eq"));
            addRspImm(16);
            // 返回值在 rax（bool）；此前误用 pop 覆盖返回值并破坏栈平衡
            if (e->op == BinOp::Ne) c.xorRI(RAX, 1);
            c.push(RAX);
            return;
        }
        if (isS && e->op == BinOp::Add) {
            // 字符串拼接：lhs + rhs → luxrt_str_concat(lhs, rhs)
            // （[rsp]=rhs、[rsp+8]=lhs，与 concat(a,b) 参数序一致）
            c.call32(symLabel("luxrt_str_concat"));
            addRspImm(16);
            c.push(RAX);
            return;
        }
        if (isS) {
            // 字符串字典序比较：luxrt_str_cmp → 负/0/正
            c.call32(symLabel("luxrt_str_cmp"));
            addRspImm(16);
            c.cmpRegRI(RAX, 0);
            int cc = e->op == BinOp::Lt ? CJl
                     : e->op == BinOp::Le ? CJle
                     : e->op == BinOp::Gt ? CJg
                                          : CJge;
            c.setcc(cc, RAX);
            c.push(RAX);
            return;
        }
        evalBinaryInt(e->op);
    }

    void evalBinaryInt(BinOp op) {
        switch (op) {
            case BinOp::Add:
                c.movrm(RAX, RSP, 0);
                c.opMR(0x01, RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::Sub:
                c.movrm(RAX, RSP, 0);
                c.opMR(0x29, RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::Mul:
                c.movrm(RAX, RSP, 0);
                c.imulrm(RAX, RSP, 8);
                c.movmr(RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::Div:
            case BinOp::Mod: {
                const char* div0Msg = op == BinOp::Div
                                          ? "整数除法的除数为 0"
                                          : "取模运算的除数为 0";
                const char* lovfMsg = op == BinOp::Div
                                          ? "整数除法结果溢出"
                                          : "取模运算结果溢出";
                int ldiv0 = c.newLabel(), lovf = c.newLabel(), lok = c.newLabel();
                c.movrm(RCX, RSP, 0);       // 除数
                c.testRR(RCX, RCX);
                c.jcc32(CJe, ldiv0);
                c.cmpRegRI(RCX, -1);
                c.jcc32(CJne, lok);
                c.mov64i(RAX, (int64_t)0x8000000000000000ULL);
                c.opRM(0x39, RAX, RSP, 8);  // cmp rax, [rsp+8]（被除数）
                c.jcc32(CJe, lovf);
                c.bind(lok);
                c.movrm(RAX, RSP, 8);
                c.cqo();
                c.idivm(RSP, 0);
                if (op == BinOp::Div) c.movmr(RSP, 8, RAX);
                else c.movmr(RSP, 8, RDX);
                addRspImm(8);
                int lend = c.newLabel();
                c.jmp32(lend);
                c.bind(ldiv0);
                callPanicMsg(div0Msg);
                c.bind(lovf);
                callPanicMsg(lovfMsg);
                c.bind(lend);
                return;
            }
            case BinOp::BitAnd:
                c.movrm(RAX, RSP, 0);
                c.opMR(0x21, RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::BitOr:
                c.movrm(RAX, RSP, 0);
                c.opMR(0x09, RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::BitXor:
                c.movrm(RAX, RSP, 0);
                c.opMR(0x31, RSP, 8, RAX);
                addRspImm(8);
                return;
            case BinOp::Shl:
                c.movrm(RCX, RSP, 0);
                c.shiftClMem(4, RSP, 8);
                addRspImm(8);
                return;
            case BinOp::Shr:
                c.movrm(RCX, RSP, 0);
                c.shiftClMem(7, RSP, 8);  // 算术右移（有符号）
                addRspImm(8);
                return;
            default: {  // 比较
                int cc;
                switch (op) {
                    case BinOp::Eq: cc = CJe; break;
                    case BinOp::Ne: cc = CJne; break;
                    case BinOp::Lt: cc = CJl; break;
                    case BinOp::Le: cc = CJle; break;
                    case BinOp::Gt: cc = CJg; break;
                    default: cc = CJge; break;
                }
                c.movrm(RAX, RSP, 0);
                c.opMR(0x39, RSP, 8, RAX);
                c.setcc(cc, RAX);
                c.movmr(RSP, 8, RAX);
                addRspImm(8);
                return;
            }
        }
    }

    void evalBinaryFloat(BinOp op) {
        switch (op) {
            case BinOp::Add: c.movsdFromMem(0, RSP, 0); c.movsdFromMem(1, RSP, 8);
                c.sdRR(0x58, 1, 0); c.movsdToMem(1, RSP, 8); addRspImm(8); return;
            case BinOp::Sub: c.movsdFromMem(0, RSP, 0); c.movsdFromMem(1, RSP, 8);
                c.sdRR(0x5C, 1, 0); c.movsdToMem(1, RSP, 8); addRspImm(8); return;
            case BinOp::Mul: c.movsdFromMem(0, RSP, 0); c.movsdFromMem(1, RSP, 8);
                c.sdRR(0x59, 1, 0); c.movsdToMem(1, RSP, 8); addRspImm(8); return;
            case BinOp::Div: c.movsdFromMem(0, RSP, 0); c.movsdFromMem(1, RSP, 8);
                c.sdRR(0x5E, 1, 0); c.movsdToMem(1, RSP, 8); addRspImm(8); return;
            default: {
                // 浮点比较（NaN 语义与 C 一致：eq: NaN→false; ne: NaN→true）。
                // 具体条件码/指令序列由后端决定（x86 ucomisd+JP；arm64 fcmp+VS）
                c.floatCompare(op);
                return;
            }
        }
    }

    // =====================================================================
    //  变量 / 栈顶辅助
    // =====================================================================
    void pushVarRef(const std::string& name) {
        Var* v = findVar(name);
        if (!v) { pushImm(0); return; }  // Sema 已报错
        c.push(RAX);
        c.movrm(RAX, RBP, v->slot);
        c.movmr(RSP, 0, RAX);
    }

    // 栈顶值（按类型 t）换成字符串所需的临时槽数（打印 / string / format）
    // 打印 struct / 可选 struct 需要的临时栈槽数。
    // 0.9.4：加 visited 集合 —— 自引用 struct（如 jsonx 的 Json 含 Json[]）
    // 以前会让本函数无限递归，直接把编译器打崩（栈溢出 SIGSEGV）。
    // 每个 struct 类型只展开一次：仍然给出足够的槽位，同时保证终止。
    int strSlotsForType(const Ty* t) {
        std::unordered_set<std::string> seen;
        return strSlotsForType_(t, seen, 0);
    }
    int strSlotsForType_(const Ty* t, std::unordered_set<std::string>& seen,
                         int depth) {
        if (!t || depth > 12) return 0;
        if (t->kind == TyKind::Named) {
            if (!seen.insert(t->name).second) return 1;  // 已展开过：只留 __pst
            int n = 1;  // __pst
            auto it = structs_.find(t->name);
            if (it != structs_.end())
                for (const FieldDecl& f : it->second->fields)
                    n += strSlotsForType_(f.ty, seen, depth + 1);
            return n;
        }
        if (t->kind == TyKind::Optional) {
            return 1 + strSlotsForType_(t->elem, seen, depth + 1);  // __opt（0.8）
        }
        if (t->kind == TyKind::Array && t->elem && containsNamed(t->elem)) {
            return 5 + strSlotsForType_(t->elem, seen, depth + 1);  // arr/len/i/acc
        }
        return 0;
    }

    // ---- struct → string（0.7）：拼出 "Point { x: 1, y: 2 }" ----
    void strConcatTop2() {
        c.call32(symLabel("luxrt_str_concat"));
        addRspImm(16);
        c.push(RAX);
    }

    // 栈顶是 struct 数组（元素为 Named）；换成 "[...]" 字符串（0.7）
    void arrayToStrOnStack(const Ty* elemTy) {
        if (printDepth_ > kPrintDepthMax) { c.pop(RAX); pushStrRef("<...>"); return; }
        printDepth_++;
        c.pop(RAX);
        int aslot = declareVar("__ast" + std::to_string(uid_++));
        c.movmr(RBP, aslot, RAX);
        int lenslot = declareVar("__asn" + std::to_string(uid_++));
        c.movrm(RAX, RBP, aslot);
        c.movrm(RAX, RAX, 0);
        c.movmr(RBP, lenslot, RAX);
        int islot = declareVar("__asi" + std::to_string(uid_++));
        c.movmi(RBP, islot, 0);
        int accslot = declareVar("__asa" + std::to_string(uid_++));
        pushStrRef("[");
        c.pop(RAX);
        c.movmr(RBP, accslot, RAX);
        auto loadAcc = [&]() {
            c.push(RAX);
            c.movrm(RAX, RBP, accslot);
            c.movmr(RSP, 0, RAX);
        };
        int ltop = c.newLabel(), lend = c.newLabel(), lskip = c.newLabel();
        c.bind(ltop);
        c.movrm(RAX, RBP, islot);
        c.opRM(0x3B, RAX, RBP, lenslot);  // cmp i, [rbp+lenslot]
        c.jcc32(CJge, lend);
        c.testRR(RAX, RAX);
        c.jcc32(CJe, lskip);
        loadAcc();
        pushStrRef(", ");
        c.call32(symLabel("luxrt_str_concat"));
        addRspImm(16);
        c.movmr(RBP, accslot, RAX);
        c.bind(lskip);
        loadAcc();
        c.movrm(RAX, RBP, aslot);
        c.movrm(RDX, RAX, 16);  // data
        c.movrm(RAX, RBP, islot);
        c.movSib(RAX, RDX, RAX, 3);
        c.push(RAX);
        c.movmr(RSP, 0, RAX);
        if (elemTy->kind == TyKind::Named)
            structToStr(elemTy->name);
        else
            replaceTopWithStr(elemTy);
        c.call32(symLabel("luxrt_str_concat"));
        addRspImm(16);
        c.movmr(RBP, accslot, RAX);
        incMem(RBP, islot);
        c.jmp32(ltop);
        c.bind(lend);
        loadAcc();
        pushStrRef("]");
        c.call32(symLabel("luxrt_str_concat"));
        addRspImm(16);
        c.push(RAX);
        printDepth_--;
    }

    // 栈顶值转字符串（不给 string 加引号）：供可选值 some(...) 拼接使用
    void valueToStrTop(const Ty* t) {
        if (t && t->kind == TyKind::String) return;
        replaceTopWithStr(t);
    }

    // 栈顶是 T? 指针；换成 "some(...)" / "none" 字符串（0.8）
    void optToStrTop(const Ty* optTy) {
        const Ty* elem = optTy->elem;
        c.pop(RAX);
        int slot = declareVar("__opt" + std::to_string(uid_++));
        c.movmr(RBP, slot, RAX);
        int lnone = c.newLabel(), lend = c.newLabel();
        c.testRR(RAX, RAX);
        c.jcc32(CJe, lnone);
        pushStrRef("some(");
        c.push(RAX);  // 占位，下一行覆写
        c.movrm(RAX, RBP, slot);
        c.movrm(RAX, RAX, 0);
        c.movmr(RSP, 0, RAX);
        valueToStrTop(elem);
        strConcatTop2();
        pushStrRef(")");
        strConcatTop2();
        c.jmp32(lend);
        c.bind(lnone);
        pushStrRef("none");
        c.bind(lend);
    }

    // 把栈顶值（按类型 t）原地换成它的字符串表示
    void replaceTopWithStr(const Ty* t) {
        switch (t ? t->kind : TyKind::Int) {
            case TyKind::Optional:
                optToStrTop(t);
                return;
            case TyKind::String:
                // struct 字段里的字符串带引号转义（与 C 后端 lx_pe_str 一致）
                c.call32(symLabel("luxrt_quote"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Int:
                c.call32(symLabel("luxrt_i64_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Float:
                c.call32(symLabel("luxrt_f64_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Bool:
                c.call32(symLabel("luxrt_bool_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Array:
                if (t->elem && containsNamed(t->elem)) {
                    arrayToStrOnStack(t->elem);
                    return;
                }
                c.call32(symLabel("luxrt_arr_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Named:
                structToStr(t->name);
                return;
            default:
                return;
        }
    }

    // 栈顶是 struct 指针；换成字符串（拼接链），结果在栈顶
    // 0.9.4：打印递归深度上限。自引用 struct（jsonx 的 Json 含 Json[]）
    // 以前让 structToStr ↔ arrayToStrOnStack 互相无限展开，把编译器打崩；
    // 超过上限就输出 "<...>" 占位（正常嵌套深度 ≤ 3，不受影响）。
    int printDepth_ = 0;
    static constexpr int kPrintDepthMax = 8;

    void structToStr(const std::string& name) {
        if (printDepth_ > kPrintDepthMax) { c.pop(RAX); pushStrRef("<...>"); return; }
        auto it = structs_.find(name);
        if (it == structs_.end()) { pushStrRef(""); return; }
        StructDecl* sd = it->second;
        printDepth_++;
        c.pop(RAX);
        int slot = declareVar("__pst" + std::to_string(uid_++));
        c.movmr(RBP, slot, RAX);
        pushStrRef(sd->name + " {");
        for (size_t i = 0; i < sd->fields.size(); i++) {
            const FieldDecl& f = sd->fields[i];
            pushStrRef(std::string(i == 0 ? " " : ", ") + f.name + ": ");
            strConcatTop2();
            // 压字段值
            c.push(RAX);
            c.movrm(RAX, RBP, slot);
            c.movrm(RAX, RAX, static_cast<int>(i) * 8);
            c.movmr(RSP, 0, RAX);
            replaceTopWithStr(f.ty);
            strConcatTop2();
        }
        pushStrRef(" }");
        strConcatTop2();
        printDepth_--;
    }

    // 栈顶值转 string 并压回（ToString / format 参数共用）
    void toStringTop(Expr* a) {
        const Ty* t = a ? a->ty : nullptr;
        if (t && t->kind == TyKind::Optional) {
            optToStrTop(t);
            return;
        }
        if (t && t->kind == TyKind::Named) {
            structToStr(t->name);
            return;
        }
        if (t && t->kind == TyKind::Array && t->elem &&
            containsNamed(t->elem)) {
            arrayToStrOnStack(t->elem);
            return;
        }
        if (t && t->kind == TyKind::Array) {
            c.call32(symLabel("luxrt_arr_to_str"));
            addRspImm(8);
            c.push(RAX);
            return;
        }
        switch (t ? t->kind : TyKind::Int) {
            case TyKind::Float:
                c.call32(symLabel("luxrt_f64_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::Bool:
                c.call32(symLabel("luxrt_bool_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
            case TyKind::String:
                return;  // 栈顶已是 string，原样（format / string() 不加引号）
            default:
                c.call32(symLabel("luxrt_i64_to_str"));
                addRspImm(8);
                c.push(RAX);
                return;
        }
    }

    // =====================================================================
    //  print / println 的单参数输出（语句位置展开，逐参数求值即打印）
    // =====================================================================
    void emitPrintOne(Expr* a) {
        evalExpr(a);
        if (a->ty && a->ty->kind == TyKind::Optional) {
            // 可选值：先转字符串再输出 some(x) / none（0.8）
            optToStrTop(a->ty);
            c.call32(symLabel("luxrt_print_str"));
            addRspImm(8);
            return;
        }
        if (a->ty && a->ty->kind == TyKind::Named) {
            // struct：先转字符串再输出（0.7）
            structToStr(a->ty->name);
            c.call32(symLabel("luxrt_print_str"));
            addRspImm(8);
            return;
        }
        if (a->ty && a->ty->kind == TyKind::Array && a->ty->elem &&
            containsNamed(a->ty->elem)) {
            arrayToStrOnStack(a->ty->elem);
            c.call32(symLabel("luxrt_print_str"));
            addRspImm(8);
            return;
        }
        const char* fn = "luxrt_print_i64";
        switch (a->ty ? a->ty->kind : TyKind::Int) {
            case TyKind::Float:  fn = "luxrt_print_f64";  break;
            case TyKind::Bool:   fn = "luxrt_print_bool"; break;
            case TyKind::String: fn = "luxrt_print_str";  break;
            case TyKind::Array:  fn = "luxrt_arr_print";  break;
            default: break;
        }
        c.call32(symLabel(fn));
        addRspImm(8);
    }

    // =====================================================================
    //  数组方法（a.push(x) 等；接收者一定是变量，Sema 保证）
    //  元素一律 8 字节槽，float 元素存位模式
    // =====================================================================
    void evalArrayMethod(CallExpr* e) {
        bool elemF = e->methodElem && e->methodElem->kind == TyKind::Float;
        auto evalElem = [&](Expr* a) {  // 求值元素，int→float 按元素类型提升
            evalExpr(a);
            if (elemF && a->ty && a->ty->kind == TyKind::Int) fpromoteTop();
        };
        switch (e->builtin) {
            case Builtin::ArrPush:
                pushVarRef(e->methodRecv);
                evalElem(e->args[0]);
                c.call32(symLabel("luxrt_arr_push"));
                addRspImm(16);
                return;
            case Builtin::ArrPop:
                pushVarRef(e->methodRecv);
                c.call32(symLabel("luxrt_arr_pop"));
                addRspImm(8);
                callRet(e->ty);
                return;
            case Builtin::ArrInsert:
                pushVarRef(e->methodRecv);
                evalExpr(e->args[0]);
                evalElem(e->args[1]);
                c.call32(symLabel("luxrt_arr_insert"));
                addRspImm(24);
                return;
            case Builtin::ArrRemove:
                pushVarRef(e->methodRecv);
                evalExpr(e->args[0]);
                c.call32(symLabel("luxrt_arr_remove"));
                addRspImm(16);
                callRet(e->ty);
                return;
            case Builtin::ArrClear:
                pushVarRef(e->methodRecv);
                c.call32(symLabel("luxrt_arr_clear"));
                addRspImm(8);
                return;
            case Builtin::ArrSort:
                // 1.1：a.sort(cmp) —— 运行时按 8 字节槽泛化排序
                pushVarRef(e->methodRecv);
                evalExpr(e->args[0]);
                c.call32(symLabel("luxrt_sort"));
                addRspImm(16);
                return;
            default:
                err(e->loc, "编译器内部错误：未知数组方法");
                pushImm(0);
                return;
        }
    }

    // =====================================================================
    //  函数调用：intrinsic → 数组方法 → builtin → 用户函数
    // =====================================================================
    void evalCall(CallExpr* e) {
        if ((int)e->builtin >= (int)Builtin::IntrSyscall) {
            evalIntrinsic(e);
            return;
        }
        if (!e->methodRecv.empty()) {
            evalArrayMethod(e);
            return;
        }
        if (e->builtin != Builtin::None) {
            genBuiltin(e);
            return;
        }
        // 1.1：通过 fn 类型变量间接调用
        if (e->viaValue) {
            int n = (int)e->args.size();
            const Ty* ft = e->calleeFnTy;
            for (int i = 0; i < n; i++) {
                const Ty* pt = (ft && i < (int)ft->members.size())
                                   ? ft->members[i]
                                   : nullptr;
                evalCoerced(e->args[i], pt);
            }
            Var* v = findVar(e->callee);
            if (!v) { pushImm(0); return; }  // Sema 已报错
            c.movrm(RAX, RBP, v->slot);
            c.callReg(RAX);
            addRspImm(8 * n);
            if (e->panicVariant && ft && ft->elem &&
                ft->elem->kind == TyKind::Optional) {
                panicUnwrapTop("\u51fd\u6570\u8c03\u7528", ft->elem->elem);
            } else {
                callRet(e->ty);
            }
            return;
        }
        if (e->target) {
            if (e->target->isExtern) {
                err(e->loc, "原生后端不支持 extern fn '" + e->target->name + "'");
                pushImm(0);
                return;
            }
            int n = (int)e->args.size();
            for (int i = 0; i < n; i++) {
                Expr* a = e->args[i];
                const Ty* pt = (i < (int)e->target->params.size())
                                   ? e->target->params[i].ty
                                   : nullptr;
                // 实参到形参的隐式转换（int→float / T→T? 装箱）
                evalCoerced(a, pt);
            }
            c.call32(symLabel(keyOf(*e->target)));
            addRspImm(8 * n);
            // 0.8：panic 变体 f!(...)（f 返回 T?，失败即退出）
            if (e->panicVariant && e->target->retTy &&
                e->target->retTy->kind == TyKind::Optional) {
                panicUnwrapTop(e->target->name, e->target->retTy->elem);
            } else {
                callRet(e->ty);
            }
            return;
        }
        pushImm(0);  // Sema 报错后的残局
    }

    // =====================================================================
    //  内置函数（与 C 后端语义对齐）
    // =====================================================================
    void genBuiltin(CallExpr* e) {
        auto& args = e->args;
        auto argIsInt = [&](size_t i) {
            return args[i]->ty && args[i]->ty->kind == TyKind::Int;
        };
        switch (e->builtin) {
            case Builtin::Print:
            case Builtin::PrintLn:
                for (Expr* a : args) emitPrintOne(a);
                if (e->builtin == Builtin::PrintLn)
                    c.call32(symLabel("luxrt_print_nl"));
                return;

            case Builtin::Len:
                evalExpr(args[0]);
                // string 与数组的首字段都是 len（i64）
                c.pop(RAX);
                c.movrm(RAX, RAX, 0);
                c.push(RAX);
                return;

            case Builtin::Input:
                if (args.empty()) {
                    c.call32(symLabel("luxrt_read_line"));
                } else {
                    evalExpr(args[0]);
                    c.call32(symLabel("luxrt_read_line_prompt"));
                    addRspImm(8);
                }
                c.push(RAX);
                return;

            case Builtin::ToInt:
                evalExpr(args[0]);
                switch (args[0]->ty ? args[0]->ty->kind : TyKind::Int) {
                    case TyKind::Float:
                        c.movsdFromMem(0, RSP, 0);
                        c.cvttsd2si(RAX, 0);
                        c.movmr(RSP, 0, RAX);
                        return;
                    case TyKind::String:
                        c.call32(symLabel("luxrt_str_to_i64_opt"));
                        addRspImm(8);
                        if (e->panicVariant)
                            panicUnwrapTop("int 解析", nullptr);
                        else
                            c.push(RAX);
                        return;
                    default:  // int / bool 位模式一致，透传
                        return;
                }

            case Builtin::ToFloat:
                evalExpr(args[0]);
                switch (args[0]->ty ? args[0]->ty->kind : TyKind::Int) {
                    case TyKind::Float:
                        return;
                    case TyKind::String:
                        c.call32(symLabel("luxrt_str_to_f64_opt"));
                        addRspImm(8);
                        if (e->panicVariant)
                            panicUnwrapTop("float 解析", nullptr);
                        else
                            c.push(RAX);
                        return;
                    default:  // int / bool → cvtsi2sd
                        c.pop(RAX);
                        c.cvtsi2sd(0, RAX);
                        pushFloatXmm0();
                        return;
                }

            case Builtin::ToString:
                if (args.empty()) { pushStrRef(""); return; }
                evalExpr(args[0]);  // toStringTop 约定栈顶已是参数值
                toStringTop(args[0]);
                return;

            case Builtin::Assert:
                evalExpr(args[0]);
                if (args.size() > 1) {
                    evalExpr(args[1]);
                } else {
                    pushStrRef("条件不成立");
                }
                pushImm(e->loc.line);
                c.call32(symLabel("luxrt_assert_at"));
                addRspImm(24);
                return;

            case Builtin::Exit:
                evalExpr(args[0]);
                c.pop(RAX);
                c.exitGroupReg(RAX);
                return;

            case Builtin::Abs: {
                evalExpr(args[0]);
                if (args[0]->ty && args[0]->ty->kind == TyKind::Float) {
                    // fabs：清符号位
                    c.movsdFromMem(0, RSP, 0);
                    c.mov64i(RAX, (int64_t)0x7FFFFFFFFFFFFFFFLL);
                    c.movqXmmR(1, RAX);
                    c.pdRR(0x54, 0, 1);  // andpd xmm0, xmm1
                    c.movsdToMem(0, RSP, 0);
                } else {
                    // llabs：rax = |rax|（经符号扩展技巧）
                    c.pop(RAX);
                    c.cqo();
                    c.opRR(0x31, RAX, RDX);  // xor rax, rdx
                    c.opRR(0x29, RAX, RDX);  // sub rax, rdx
                    c.push(RAX);
                }
                return;
            }

            case Builtin::Sqrt:
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                c.movsdFromMem(0, RSP, 0);
                c.sdRR(0x51, 0, 0);  // sqrtsd xmm0, xmm0
                c.movsdToMem(0, RSP, 0);
                return;

            case Builtin::Floor:
            case Builtin::Ceil:
            case Builtin::Trunc: {
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                c.movsdFromMem(0, RSP, 0);
                int imm = e->builtin == Builtin::Floor ? 1
                          : e->builtin == Builtin::Ceil ? 2
                                                        : 3;
                c.roundsd(0, imm);
                c.movsdToMem(0, RSP, 0);
                return;
            }

            case Builtin::Round:
                // round-half-away-from-zero（与 C 后端 round 对齐）
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                c.call32(symLabel("luxrt_round"));
                addRspImm(8);
                pushFloatXmm0();
                return;

            // ---- math：一元超越函数 ----
            // x86 用 x87 内联；arm64 无 x87，调用 Lux 运行时数学函数
            // （luxrt_math_*，参数/返回值走与普通函数相同的栈槽 + xmm0/d0）。
            case Builtin::Sin:
            case Builtin::Cos:
            case Builtin::Tan:
            case Builtin::Asin:
            case Builtin::Acos:
            case Builtin::Atan:
            case Builtin::Log:
            case Builtin::Log10:
            case Builtin::Exp: {
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                // 0.9.4：asin / acos 统一调运行时库实现（两个架构同一套代码）。
                // 旧 x86-64 用 x87 手写：FSUBP 的栈方向写反，算的是 √(x²−1)，
                // 对 |x|<1 恒得 NaN —— asin(0.99999) 直接输出 nan。
                if (e->builtin == Builtin::Asin || e->builtin == Builtin::Acos) {
                    c.call32(symLabel(e->builtin == Builtin::Asin
                                          ? "luxrt_math_asin"
                                          : "luxrt_math_acos"));
                    addRspImm(8);
                    pushFloatXmm0();
                    return;
                }
#if defined(__aarch64__)
                const char* mfn = e->builtin == Builtin::Sin    ? "luxrt_math_sin"
                                : e->builtin == Builtin::Cos    ? "luxrt_math_cos"
                                : e->builtin == Builtin::Tan    ? "luxrt_math_tan"
                                : e->builtin == Builtin::Asin   ? "luxrt_math_asin"
                                : e->builtin == Builtin::Acos   ? "luxrt_math_acos"
                                : e->builtin == Builtin::Atan   ? "luxrt_math_atan"
                                : e->builtin == Builtin::Log    ? "luxrt_math_log"
                                : e->builtin == Builtin::Log10  ? "luxrt_math_log10"
                                                                : "luxrt_math_exp";
                c.call32(symLabel(mfn));
                addRspImm(8);
                pushFloatXmm0();
                return;
#else
                switch (e->builtin) {
                    case Builtin::Sin:
                        c.fldM(RSP, 0); c.fsin(); c.fstpM(RSP, 0);
                        return;
                    case Builtin::Cos:
                        c.fldM(RSP, 0); c.fcos(); c.fstpM(RSP, 0);
                        return;
                    case Builtin::Tan:
                        // fptan：tan 在 st(1)，st(0)=1.0 → 丢 1.0 存结果
                        c.fldM(RSP, 0); c.fptan();
                        c.fxch(); c.fstpSt0(); c.fstpM(RSP, 0);
                        return;
                    case Builtin::Asin:
                        // asin(x) = atan2(x, √(1-x²))；fpatan = atan(st1/st0)
                        c.fldM(RSP, 0);       // x
                        c.fldSt0();           // x, x
                        c.fmulSt0St0();       // x², x
                        c.fld1();             // 1, x², x
                        c.fsubSt1();          // 1-x², x
                        c.fsqrt87();          // √(1-x²), x
                        c.fpatan();           // atan(x / √(1-x²))
                        c.fstpM(RSP, 0);
                        return;
                    case Builtin::Acos:
                        // acos(x) = atan2(√(1-x²), x)
                        c.fldM(RSP, 0);
                        c.fldSt0();
                        c.fmulSt0St0();
                        c.fld1();
                        c.fsubSt1();
                        c.fsqrt87();
                        c.fxch();             // x, √(1-x²)
                        c.fpatan();           // atan(√(1-x²) / x)
                        c.fstpM(RSP, 0);
                        return;
                    case Builtin::Atan:
                        c.fldM(RSP, 0);       // x
                        c.fld1();             // 1, x
                        c.fpatan();           // atan(x/1)
                        c.fstpM(RSP, 0);
                        return;
                    case Builtin::Log:
                        c.fldln2();
                        c.fldM(RSP, 0);
                        c.fyl2x();            // ln2 × log2(x) = ln(x)
                        c.fstpM(RSP, 0);
                        return;
                    case Builtin::Log10:
                        c.fldlg2();
                        c.fldM(RSP, 0);
                        c.fyl2x();
                        c.fstpM(RSP, 0);
                        return;
                    default: {  // Exp：2^(x·log2e)，与 Pow 同构
                        c.fldl2e();
                        c.fldM(RSP, 0);
                        c.fmulp();            // y = x·log2(e)
                        c.fldSt0();
                        c.frndint();          // n, y
                        c.fxch();             // y, n
                        c.fsubrSt1();         // f=y-n, n
                        c.f2xm1();            // 2^f-1, n
                        c.fld1();
                        c.faddp();            // 2^f, n
                        c.fscale();           // st0 = 2^f·2^n = 2^y
                        c.fstpM(RSP, 0);
                        c.fstpSt0();          // 清遗留的 n
                        return;
                    }
                }
#endif
            }

            case Builtin::Pow: {
                // [rsp]=y(后压)，[rsp+8]=x
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                evalExpr(args[1]);
                if (argIsInt(1)) fpromoteTop();
#if defined(__aarch64__)
                c.call32(symLabel("luxrt_math_pow"));
                addRspImm(16);
                pushFloatXmm0();
                return;
#else
                // 2^z = 2^f·2^n（n=⌊z⌋，f=z-n）：
                // FSCALE（实测）＝ st0 = st0·2^st1，不弹栈
                c.fldM(RSP, 0);       // y           st0=y
                c.fldM(RSP, 8);       // x, y        st0=x, st1=y
                c.fyl2x();            // z=y·log2(x)
                c.fldSt0();           // z, z
                c.frndint();          // n, z
                c.fxch();             // z, n
                c.fsubrSt1();         // f=z-n, n    （D8 E1: st0-=st1）
                c.f2xm1();            // 2^f-1, n
                c.fld1();
                c.faddp();            // 2^f, n
                c.fscale();           // st0 = 2^f·2^n = 2^z（st1=n 遗留）
                c.fstpM(RSP, 8);      // 存结果并弹
                c.fstpSt0();          // 清掉遗留的 n
                addRspImm(8);
                return;
#endif
            }

            case Builtin::Atan2: {
                // atan2(a0, a1) = atan(a0/a1)：[rsp]=a1，[rsp+8]=a0
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                evalExpr(args[1]);
                if (argIsInt(1)) fpromoteTop();
#if defined(__aarch64__)
                c.call32(symLabel("luxrt_math_atan2"));
                addRspImm(16);
                pushFloatXmm0();
                return;
#else
                c.fldM(RSP, 8);       // a0（分子）
                c.fldM(RSP, 0);       // a1（分母）
                c.fpatan();
                c.fstpM(RSP, 8);
                addRspImm(8);
                return;
#endif
            }

            case Builtin::Fmod: {
                // fprem：st0 = st0 mod st1
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                evalExpr(args[1]);
                if (argIsInt(1)) fpromoteTop();
#if defined(__aarch64__)
                c.call32(symLabel("luxrt_math_fmod"));
                addRspImm(16);
                pushFloatXmm0();
                return;
#else
                c.fldM(RSP, 0);       // a1
                c.fldM(RSP, 8);       // a0, a1
                c.fprem();
                c.fstpM(RSP, 8);
                addRspImm(8);
                return;
#endif
            }

            case Builtin::Hypot: {
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                evalExpr(args[1]);
                if (argIsInt(1)) fpromoteTop();
#if defined(__aarch64__)
                c.call32(symLabel("luxrt_math_hypot"));
                addRspImm(16);
                pushFloatXmm0();
                return;
#else
                c.fldM(RSP, 8);       // a0
                c.fmulSt0St0();       // a0²
                c.fldM(RSP, 0);       // a1, a0²
                c.fmulSt0St0();       // a1², a0²
                c.faddp();            // a0²+a1²
                c.fsqrt87();
                c.fstpM(RSP, 8);
                addRspImm(8);
                return;
#endif
            }

            case Builtin::IsNan:
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                // 结果写入 [rsp]（0/1），后端决定指令序列
                c.floatIsNan();
                return;

            case Builtin::IsInf: {
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                c.pop(RAX);
                c.mov64i(RCX, (int64_t)0x7FFFFFFFFFFFFFFFLL);
                c.opRR(0x21, RAX, RCX);  // and rax, |掩码|
                c.mov64i(RCX, (int64_t)0x7FF0000000000000LL);
                c.opRR(0x39, RAX, RCX);  // cmp rax, ±Inf 位模式
                c.setcc(CJe, RAX);
                c.push(RAX);
                return;
            }

            case Builtin::Random:
                c.call32(symLabel("luxrt_random"));
                pushFloatXmm0();
                return;

            case Builtin::Seed:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_seed"));
                addRspImm(8);
                return;

            case Builtin::Min:
            case Builtin::Max: {
                bool isMin = e->builtin == Builtin::Min;
                evalExpr(args[0]);
                if (e->ty && e->ty->kind == TyKind::Float && argIsInt(0))
                    fpromoteTop();
                evalExpr(args[1]);
                if (e->ty && e->ty->kind == TyKind::Float && argIsInt(1))
                    fpromoteTop();
                if (e->ty && e->ty->kind == TyKind::Float) {
                    // fmin/fmax 语义（NaN 让位于另一操作数）走运行时
                    c.call32(symLabel(isMin ? "luxrt_min_f" : "luxrt_max_f"));
                    addRspImm(16);
                    pushFloatXmm0();
                    return;
                }
                c.movrm(RAX, RSP, 8);         // rax = a
                c.opRM(0x3B, RAX, RSP, 0);    // cmp a, b（0x3B: r ← r/m）
                int lkeep = c.newLabel();
                c.jcc32(isMin ? CJle : CJge, lkeep);
                c.movrm(RAX, RSP, 0);         // 取 b
                c.bind(lkeep);
                // 结果写回 a 槽并清 b 槽：此时栈顶即结果（净 +1 槽约定）
                c.movmr(RSP, 8, RAX);
                addRspImm(8);
                return;                       // 此前误加 push(RAX)，净 +2 破坏栈平衡
            }

            // ---- time ----
            case Builtin::TimeNow:
                c.call32(symLabel("luxrt_time_now"));
                pushFloatXmm0();
                return;
            case Builtin::TimeMono:
                c.call32(symLabel("luxrt_time_mono"));
                pushFloatXmm0();
                return;
            case Builtin::Sleep:
                evalExpr(args[0]);
                if (argIsInt(0)) fpromoteTop();
                c.call32(symLabel("luxrt_sleep"));
                addRspImm(8);
                return;
            case Builtin::SleepMs:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_sleep_ms"));
                addRspImm(8);
                return;

            // ---- system ----
            case Builtin::SysCall:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_system"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::Env:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_getenv"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::Setenv:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_setenv"));
                addRspImm(16);
                c.push(RAX);
                return;

            // ---- file ----
            case Builtin::FileRead:
                if (e->panicVariant) {
                    evalExpr(args[0]);
                    c.movrm(RAX, RSP, 0);   // 保存一份 path
                    c.push(RAX);
                    c.call32(symLabel("luxrt_read_opt"));
                    addRspImm(8);           // 消费参数，保留保存的 path
                    panicUnwrapFileTop();
                } else {
                    evalExpr(args[0]);
                    c.call32(symLabel("luxrt_read_opt"));
                    addRspImm(8);
                    c.push(RAX);
                }
                return;
            case Builtin::FileExists:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_file_exists"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::FileWrite:
            case Builtin::FileAppend:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel(e->builtin == Builtin::FileWrite
                                      ? "luxrt_file_write"
                                      : "luxrt_file_append"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::FileRemove:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_file_remove"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::FileRename:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_file_rename"));
                addRspImm(16);
                c.push(RAX);
                return;

            // ---- string ----
            case Builtin::StrContains:
            case Builtin::StrStartsWith:
            case Builtin::StrEndsWith:
            case Builtin::StrFind: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                const char* fn = e->builtin == Builtin::StrContains
                                     ? "luxrt_str_contains"
                               : e->builtin == Builtin::StrStartsWith
                                     ? "luxrt_str_startswith"
                               : e->builtin == Builtin::StrEndsWith
                                     ? "luxrt_str_endswith"
                                     : "luxrt_str_find";
                c.call32(symLabel(fn));
                addRspImm(16);
                c.push(RAX);
                return;
            }
            case Builtin::StrFindOpt:
                // 0.9.4（P1-8）：找不到返回 none（装箱槽由 luxrt_alloc 登记）
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_str_find_opt"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::LastError:
                // 0.9.4（P1-5）：返回的是进程级根串（借用），无需登记
                c.call32(symLabel("luxrt_last_error"));
                c.push(RAX);
                return;
            case Builtin::ByteAt:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_str_byte_at"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::Bytes:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_str_bytes"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::ListDir:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_list_dir"));
                addRspImm(8);
                c.push(RAX);
                return;
            // ---- net（1.2）----
            case Builtin::NetDial:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_net_dial"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::NetSend:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_net_send"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::NetRecv:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_net_recv"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::NetClose:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_net_close"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::NetListen:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_net_listen"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::NetAccept:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_net_accept"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::NetSetTimeout:
                evalExpr(args[0]);
                evalCoerced(args[1], TyStore::float64Ty());
                c.call32(symLabel("luxrt_net_set_timeout"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::Map:
                evalExpr(args[0]);
                pushImm(ekOf(e->ty->elem));
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_map"));
                addRspImm(24);
                c.push(RAX);
                return;
            case Builtin::Filter:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_filter"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::MapOpt:
                evalExpr(args[0]);
                pushImm(ekOf(e->ty->elem->elem));
                evalExpr(args[1]);
                c.call32(symLabel("luxrt_map_opt"));
                addRspImm(24);
                c.push(RAX);
                return;
            case Builtin::StrReplace:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.call32(symLabel("luxrt_str_replace"));
                addRspImm(24);
                c.push(RAX);
                return;
            case Builtin::StrTrim:
            case Builtin::StrUpper:
            case Builtin::StrLower: {
                evalExpr(args[0]);
                const char* fn = e->builtin == Builtin::StrTrim
                                     ? "luxrt_str_trim"
                               : e->builtin == Builtin::StrUpper
                                     ? "luxrt_str_upper"
                                     : "luxrt_str_lower";
                c.call32(symLabel(fn));
                addRspImm(8);
                c.push(RAX);
                return;
            }
            case Builtin::StrSubstr:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.call32(symLabel("luxrt_str_substr"));
                addRspImm(24);
                c.push(RAX);
                return;
            case Builtin::StrSplit:
                evalExpr(args[0]); evalExpr(args[1]);
                c.call32(symLabel("luxrt_str_split"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::StrChars:
                evalExpr(args[0]);
                c.call32(symLabel("luxrt_str_chars"));
                addRspImm(8);
                c.push(RAX);
                return;
            case Builtin::StrJoin:
                evalExpr(args[0]); evalExpr(args[1]);
                c.call32(symLabel("luxrt_str_join"));
                addRspImm(16);
                c.push(RAX);
                return;
            case Builtin::StrFormat: {
                // format(模板, a, b, ...)：字面量模板编译期按 "{}" 分段，
                // 展开为 concat 链（与 C 后端 lx_str_format_concat 等价）
                auto* fmtE = args.empty() ? nullptr : args[0];
                if (!fmtE || fmtE->kind != ExprKind::StrLit) {
                    err(e->loc,
                        "原生后端要求 format() 的第一个参数是字符串字面量");
                    pushStrRef("");
                    return;
                }
                const std::string& fmt =
                    static_cast<StrLitExpr*>(fmtE)->value;
                std::vector<std::string> segs;
                {
                    std::string cur;
                    for (size_t i = 0; i < fmt.size(); i++) {
                        if (i + 1 < fmt.size() && fmt[i] == '{' &&
                            fmt[i + 1] == '}') {
                            segs.push_back(cur);
                            cur.clear();
                            ++i;
                        } else {
                            cur += fmt[i];
                        }
                    }
                    segs.push_back(cur);
                }
                // Sema 已校验：占位符数 == 参数数 → segs.size() == args.size()
                int slot = declareVar("__fmt" + std::to_string(uid_++));
                auto loadAcc = [&]() {  // 复制 acc 到栈顶（concat 的 p0）
                    c.push(RAX);
                    c.movrm(RAX, RBP, slot);
                    c.movmr(RSP, 0, RAX);
                };
                pushStrRef(segs[0]);
                c.pop(RAX);              // seg0 在栈顶，pop 清槽取值
                c.movmr(RBP, slot, RAX); // acc = segs[0]
                for (size_t i = 1; i < segs.size(); i++) {
                    loadAcc();
                    evalExpr(args[i]);
                    toStringTop(args[i]);  // 参数 → string
                    c.call32(symLabel("luxrt_str_concat"));
                    addRspImm(16);
                    // concat 返回值在 rax，栈已平衡，直接存槽
                    c.movmr(RBP, slot, RAX);
                    loadAcc();
                    pushStrRef(segs[i]);
                    c.call32(symLabel("luxrt_str_concat"));
                    addRspImm(16);
                    c.movmr(RBP, slot, RAX);
                }
                c.movrm(RAX, RBP, slot);
                c.push(RAX);
                return;
            }
            default:
                err(e->loc, "编译器内部错误：未知内置函数");
                pushImm(0);
                return;
        }
    }

    // =====================================================================
    //  特权内建（__ 前缀）：系统调用 / 裸内存 / 堆 / 随机数 / 数值透传
    // =====================================================================
    void evalIntrinsic(CallExpr* e) {
        auto& args = e->args;
        switch (e->builtin) {
            case Builtin::IntrSyscall: {
                int n = (int)args.size();
                if (n < 1 || n > 7) {
                    err(e->loc, "__syscall 需要 1~7 个参数（编号 + 最多 6 个参数）");
                    pushImm(0);
                    return;
                }
                for (Expr* a : args) evalExpr(a);
                // 槽布局：[rsp+8*(n-1)]=nr, [rsp+8*(n-2)]=a1, ..., [rsp]=a(n-1)
                // 具体寄存器装载（x86 rdi/rsi/rdx/r10/r8/r9 或 arm64 x0..x5/x8）
                // 由后端负责；返回后结果压栈
                c.rawSyscall(n);
                return;
            }

            case Builtin::IntrSysOpen:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.sysOpen();     // 消费 path/flags 两槽，结果压栈
                return;

            case Builtin::IntrSysUnlink:
                evalExpr(args[0]);
                c.sysUnlink();   // 消费一槽，结果压栈
                return;

            case Builtin::IntrSysRename:
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.sysRename();
                return;

            case Builtin::IntrSysFork:
                c.sysFork();
                return;

            case Builtin::IntrSysSocket:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.sysSocket();
                return;

            case Builtin::IntrSysConnect:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.sysConnect();
                return;

            case Builtin::IntrSysSendto:
                for (int i = 0; i < 6; i++) evalExpr(args[i]);
                c.sysSendto();
                return;

            case Builtin::IntrSysRecvfrom:
                for (int i = 0; i < 6; i++) evalExpr(args[i]);
                c.sysRecvfrom();
                return;

            case Builtin::IntrSysBind:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.sysBind();
                return;

            case Builtin::IntrSysListen:
                evalExpr(args[0]); evalExpr(args[1]);
                c.sysListen();
                return;

            case Builtin::IntrSysAccept:
                evalExpr(args[0]); evalExpr(args[1]); evalExpr(args[2]);
                c.sysAccept();
                return;

            case Builtin::IntrSysSetsockopt:
                for (int i = 0; i < 5; i++) evalExpr(args[i]);
                c.sysSetsockopt();
                return;

            case Builtin::IntrBumpAlloc: {
                // 全局堆指针在 bss 固定槽（kSlotHeap：cur / end），零初始化
                evalExpr(args[0]);  // n 压栈 → [rsp]（公共路径 mov (%rsp),%rsi 读取）
                c.mov64i(RAX, kSlotHeap);  // rax = 堆槽地址
                c.movrm(RCX, RAX, 0);
                c.testRR(RCX, RCX);
                int lhave = c.newLabel();
                c.jcc32(CJne, lhave);
                // 首次调用：mmap(NULL, 64MB, PROT_READ|WRITE,
                //                MAP_PRIVATE|MAP_ANONYMOUS)，更新槽并统一
                // 返回 RAX=槽地址、RCX=cur
                c.heapMmapFirst();
                c.bind(lhave);
                // 16 字节对齐
                c.movrr(RDX, RCX);                     // 旧 cur（返回值）
                c.addRI(RCX, 15);
                c.andRI(RCX, -16);
                c.movrm(RSI, RSP, 0);                  // n
                c.movrr(RDI, RCX);
                c.opRR(0x01, RDI, RSI);                // new = aligned + n
                c.movrm(8, RAX, 8);                    // r8 = end
                c.opRR(0x39, RDI, 8);                  // cmp new, end
                int lpanic = c.newLabel(), ldone = c.newLabel();
                c.jcc32(CJa, lpanic);                  // 无符号比较
                c.movmr(RAX, 0, RDI);                  // cur = new
                c.movmr(RSP, 0, RCX);                  // 栈顶 = 对齐后的旧 cur
                c.jmp32(ldone);
                c.bind(lpanic);
                callPanicMsg("堆空间耗尽（__bump_alloc 超出 64MB 预算）");
                c.bind(ldone);
                return;
            }

            case Builtin::IntrPeek64: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.movrm(RAX, RSP, 8);        // addr
                c.opRM(0x03, RAX, RSP, 0);   // add rax, off
                c.movrm(RAX, RAX, 0);        // 读 [addr+off]
                c.movmr(RSP, 8, RAX);
                addRspImm(8);
                return;
            }

            case Builtin::IntrPeek8u: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                c.movrm(RAX, RSP, 8);
                c.opRM(0x03, RAX, RSP, 0);
                c.movzx8mem(RAX, RAX);       // movzx eax, byte [rax]
                c.movmr(RSP, 8, RAX);
                addRspImm(8);
                return;
            }

            case Builtin::IntrPoke64: {
                // [rsp]=v, [rsp+8]=off, [rsp+16]=addr
                evalExpr(args[0]);
                evalExpr(args[1]);
                evalExpr(args[2]);
                c.movrm(RAX, RSP, 16);
                c.opRM(0x03, RAX, RSP, 8);   // addr + off
                c.movrm(RCX, RSP, 0);        // v
                c.movmr(RAX, 0, RCX);
                addRspImm(24);
                return;
            }

            case Builtin::IntrPoke8: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                evalExpr(args[2]);
                c.movrm(RAX, RSP, 16);
                c.opRM(0x03, RAX, RSP, 8);
                c.movrm(RCX, RSP, 0);
                c.store8Reg(RAX, RCX);       // mov [rax], cl
                addRspImm(24);
                return;
            }

            case Builtin::IntrPoke16: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                evalExpr(args[2]);
                c.movrm(RAX, RSP, 16);
                c.opRM(0x03, RAX, RSP, 8);
                c.movrm(RCX, RSP, 0);
                c.store16Reg(RAX, RCX);      // mov word ptr [rax], cx
                addRspImm(24);
                return;
            }

            case Builtin::IntrPoke32: {
                evalExpr(args[0]);
                evalExpr(args[1]);
                evalExpr(args[2]);
                c.movrm(RAX, RSP, 16);
                c.opRM(0x03, RAX, RSP, 8);
                c.movrm(RCX, RSP, 0);
                c.store32Reg(RAX, RCX);      // mov dword ptr [rax], ecx
                addRspImm(24);
                return;
            }

            case Builtin::IntrMemCopy: {
                // (dst, doff, src, soff, n)：[rsp]=n, +8=soff, +16=src,
                // +24=doff, +32=dst
                evalExpr(args[0]);
                evalExpr(args[1]);
                evalExpr(args[2]);
                evalExpr(args[3]);
                evalExpr(args[4]);
                c.movrm(RDI, RSP, 32);
                c.opRM(0x03, RDI, RSP, 24);  // dst + doff
                c.movrm(RSI, RSP, 16);
                c.opRM(0x03, RSI, RSP, 8);   // src + soff
                c.movrm(RCX, RSP, 0);        // n
                // memmove 语义（rdi=dst, rsi=src, rcx=n）：重叠区分方向，
                // 后端负责（x86 rep movsb + DF；arm64 显式循环）
                c.memmoveRaw();
                addRspImm(40);
                return;
            }

            case Builtin::IntrFBits:
                evalExpr(args[0]);
                if (args[0]->ty && args[0]->ty->kind == TyKind::Int)
                    fpromoteTop();
                c.movsdFromMem(0, RSP, 0);
                c.movqRXmm(RAX, 0);
                c.movmr(RSP, 0, RAX);
                return;

            case Builtin::IntrFFrom:
                evalExpr(args[0]);
                c.pop(RAX);
                c.movqXmmR(0, RAX);
                pushFloatXmm0();
                return;

            case Builtin::IntrIToF:
                evalExpr(args[0]);
                c.pop(RAX);
                c.cvtsi2sd(0, RAX);
                pushFloatXmm0();
                return;

            case Builtin::IntrFToI:
                evalExpr(args[0]);
                if (args[0]->ty && args[0]->ty->kind == TyKind::Int)
                    fpromoteTop();
                c.movsdFromMem(0, RSP, 0);
                c.cvttsd2si(RAX, 0);
                c.movmr(RSP, 0, RAX);
                return;

            case Builtin::IntrRandNext: {
                c.mov64i(RAX, kSlotRand);             // rax = 随机状态槽
                c.movrm(RCX, RAX, 0);
                c.movrr(RDX, RCX);                    // x ^= x << 13
                c.shlRI(RDX, 13);
                c.opRR(0x31, RCX, RDX);
                c.movrr(RDX, RCX);                    // x ^= x >> 7
                c.shrRI(RDX, 7);
                c.opRR(0x31, RCX, RDX);
                c.movrr(RDX, RCX);                    // x ^= x << 17
                c.shlRI(RDX, 17);
                c.opRR(0x31, RCX, RDX);
                c.movmr(RAX, 0, RCX);
                c.push(RCX);
                return;
            }

            case Builtin::IntrSeedSet: {
                evalExpr(args[0]);
                c.pop(RCX);
                c.mov64i(RAX, kSlotRand);
                c.movmr(RAX, 0, RCX);   // [槽] = seed
                return;
            }

            case Builtin::IntrSPtr:
            case Builtin::IntrSVal:
            case Builtin::IntrSValA:
                evalExpr(args[0]);  // 透传：先求值参数，其值压栈即结果（1 参数 1 槽）
                return;
            case Builtin::IntrCall1: {
                // __call1(p, a)：间接调用 1 参函数（1.1）
                evalExpr(args[1]);   // a 留在栈顶作实参
                evalExpr(args[0]);   // p
                c.pop(RAX);
                c.callReg(RAX);
                addRspImm(8);
                c.push(RAX);
                return;
            }
            case Builtin::IntrCall2: {
                // __call2(p, a, b)：间接调用 2 参函数（1.1）
                evalExpr(args[1]);   // a
                evalExpr(args[2]);   // b
                evalExpr(args[0]);   // p
                c.pop(RAX);
                c.callReg(RAX);
                addRspImm(16);
                c.push(RAX);
                return;
            }

            default:
                err(e->loc, "编译器内部错误：未知特权内建");
                pushImm(0);
                return;
        }
    }



    // =====================================================================
    //  语句生成
    // =====================================================================
    // ---- 赋值（0.7）：左值可以是 变量 / 成员 / 数组元素（可嵌套） ----
    // 栈顶两槽应用复合运算（[rsp]=rhs、[rsp+8]=lhs），结果留一槽
    void applyCompound(BinOp op, const Ty* tt) {
        if (tt && tt->kind == TyKind::String) {
            if (op == BinOp::Add) {
                c.call32(symLabel("luxrt_str_concat"));
                addRspImm(16);
                c.push(RAX);
            }
            return;
        }
        if (tt && tt->kind == TyKind::Float)
            evalBinaryFloat(op);
        else
            evalBinaryInt(op);
    }

    void genAssign(AssignStmt* a) {
        Expr* t = a->target;
        const Ty* tt = a->elemTy;
        bool toFloat = tt && tt->kind == TyKind::Float;
        auto promote = [&](Expr* val) {
            if (toFloat && val->ty && val->ty->kind == TyKind::Int) {
                fpromoteTop();
                return;
            }
            // 0.8：目标类型是 T?（或 float?）时把值装箱
            if (tt && tt->kind == TyKind::Optional) {
                if (val->ty == tt->elem) {
                    boxTop(tt->elem);
                } else if (val->ty && val->ty->kind == TyKind::Int &&
                           tt->elem && tt->elem->kind == TyKind::Float) {
                    fpromoteTop();
                    boxTop(tt->elem);
                }
            }
        };
        if (t->kind == ExprKind::Ident) {
            Var* v = findVar(static_cast<IdentExpr*>(t)->name);
            if (!v) return;  // Sema 已报错
            if (a->compound) {
                c.push(RAX);
                c.movrm(RAX, RBP, v->slot);
                c.movmr(RSP, 0, RAX);
                evalExpr(a->value);
                promote(a->value);
                applyCompound(a->compoundOp, tt);
                if (arcOn && eNeedsOwn(tt)) {
                    // 新串（pending 登记中）先自持一份，再释放被覆盖的旧值
                    retainTop();
                    c.pop(RAX);
                    c.push(RAX);
                    c.movrm(RAX, RBP, v->slot);
                    c.push(RAX);
                    c.call32(symLabel("luxrt_release"));
                    addRspImm(8);
                    c.pop(RAX);
                    c.movmr(RBP, v->slot, RAX);
                    return;
                }
                c.pop(RAX);
                c.movmr(RBP, v->slot, RAX);
                return;
            }
            evalExpr(a->value);
            promote(a->value);
            if (arcOn && eNeedsOwn(tt)) {
                retainTop();                       // 新值自持一份
                c.pop(RAX);
                c.push(RAX);
                c.movrm(RAX, RBP, v->slot);
                c.push(RAX);
                c.call32(symLabel("luxrt_release"));  // 先存新值再释旧值
                addRspImm(8);
                c.pop(RAX);
                c.movmr(RBP, v->slot, RAX);
                return;
            }
            c.pop(RAX);
            c.movmr(RBP, v->slot, RAX);
            return;
        }
        if (t->kind == ExprKind::Member) {
            auto* m = static_cast<MemberExpr*>(t);
            evalExpr(m->base);
            c.pop(RAX);
            int pslot = declareVar("__stp" + std::to_string(uid_++));
            c.movmr(RBP, pslot, RAX);
            int off = m->fieldIndex * 8;
            if (a->compound) {
                c.push(RAX);
                c.movrm(RAX, RBP, pslot);
                c.movrm(RAX, RAX, off);
                c.movmr(RSP, 0, RAX);
                evalExpr(a->value);
                promote(a->value);
                applyCompound(a->compoundOp, tt);
                if (arcOn && eNeedsOwn(tt)) {
                    retainTop();
                    c.pop(RAX);
                    c.push(RAX);
                    c.movrm(RCX, RBP, pslot);
                    c.movrm(RCX, RCX, off);
                    c.push(RCX);
                    c.call32(symLabel("luxrt_release"));
                    addRspImm(8);
                    c.pop(RAX);
                    c.movrm(RCX, RBP, pslot);
                    c.movmr(RCX, off, RAX);
                    return;
                }
                c.pop(RAX);
                c.movrm(RCX, RBP, pslot);
                c.movmr(RCX, off, RAX);
                return;
            }
            evalExpr(a->value);
            promote(a->value);
            if (arcOn && eNeedsOwn(tt)) {
                retainTop();                       // 新字段值自持一份
                c.pop(RAX);
                c.push(RAX);
                c.movrm(RCX, RBP, pslot);
                c.movrm(RCX, RCX, off);
                c.push(RCX);
                c.call32(symLabel("luxrt_release"));  // 释放被覆盖的旧字段
                addRspImm(8);
                c.pop(RAX);
                c.movrm(RCX, RBP, pslot);
                c.movmr(RCX, off, RAX);
                return;
            }
            c.pop(RAX);
            c.movrm(RCX, RBP, pslot);
            c.movmr(RCX, off, RAX);
            return;
        }
        if (t->kind == ExprKind::Index) {
            auto* ix = static_cast<IndexExpr*>(t);
            evalExpr(ix->base);
            c.pop(RAX);
            int aslot = declareVar("__arrp" + std::to_string(uid_++));
            c.movmr(RBP, aslot, RAX);
            evalExpr(ix->index);
            c.pop(RAX);
            int islot = declareVar("__arri" + std::to_string(uid_++));
            c.movmr(RBP, islot, RAX);
            auto pushSlot = [&](int slot) {
                c.push(RAX);
                c.movrm(RAX, RBP, slot);
                c.movmr(RSP, 0, RAX);
            };
            if (a->compound) {
                pushSlot(aslot);
                pushSlot(islot);
                c.call32(symLabel("luxrt_arr_get"));
                addRspImm(16);
                c.push(RAX);
                c.movmr(RSP, 0, RAX);
                evalExpr(a->value);
                promote(a->value);
                applyCompound(a->compoundOp, tt);
                c.pop(RAX);
                int vslot = declareVar("__arrv" + std::to_string(uid_++));
                c.movmr(RBP, vslot, RAX);
                pushSlot(aslot);
                pushSlot(islot);
                pushSlot(vslot);
                c.call32(symLabel("luxrt_arr_set"));
                addRspImm(24);
                return;
            }
            pushSlot(aslot);
            pushSlot(islot);
            evalExpr(a->value);
            promote(a->value);
            c.call32(symLabel("luxrt_arr_set"));
            addRspImm(24);
            return;
        }
    }

    // 语句包装：每条语句结束后 flush 掉本帧登记的临时对象（0.9.4）。
    // 存进变量 / 数组 / 结构体 / 返回值的对象已先 retain，因此这里只回收
    // 「用完就没」的中间结果（拼接串、to_str、装箱槽…）。
    void genStmt(Stmt* s) {
        if (!s) return;
        genStmtInner(s);
        if (arcOn) emitStmtFlush();
    }

    void genStmtInner(Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Block: {
                auto* b = static_cast<BlockStmt*>(s);
                pushScope();
                pushArcScope();
                int arcMark = (int)arcLocals.size() - 1;
                for (Stmt* st : b->stmts) genStmt(st);
                releaseScope(arcMark);   // 块出口释放本块声明的引用型局部
                popScope();
                return;
            }

            case StmtKind::Let: {
                auto* let = static_cast<LetStmt*>(s);
                int slot = declareVar(let->name, let->resolved);
                if (let->init) {
                    evalCoerced(let->init, let->resolved);
                } else if (let->resolved &&
                           let->resolved->kind == TyKind::String) {
                    // 0.9.3（A1）：未初始化的 string 必须指向合法的空串对象
                    // （布局 [len][bytes]），不能是 NULL，否则 println/len 会段错误。
                    pushStrRef("");
                } else {
                    pushImm(0);
                }
                // 0.9.4：引用型/可选型局部自持一份引用，块出口统一释放
                if (arcOn && eNeedsOwn(let->resolved)) {
                    retainTop();
                    noteArcLocal(slot);
                }
                c.pop(RAX);
                c.movmr(RBP, slot, RAX);
                return;
            }

            case StmtKind::Expr: {
                auto* es = static_cast<ExprStmt*>(s);
                Expr* e = es->expr;
                bool isVoid = e->ty && e->ty->kind == TyKind::Void;
                evalExpr(e);
                if (!isVoid) addRspImm(8);  // 丢弃表达式值
                return;
            }

            case StmtKind::Assign:
                genAssign(static_cast<AssignStmt*>(s));
                return;

            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                evalExpr(i->cond);
                c.pop(RAX);
                c.testRR(RAX, RAX);
                int lelse = c.newLabel(), lend = c.newLabel();
                bool hasElse = i->elseBranch != nullptr;
                c.jcc32(CJe, lelse);
                genStmt(i->thenBranch);
                if (hasElse) c.jmp32(lend);
                c.bind(lelse);
                if (hasElse) {
                    genStmt(i->elseBranch);
                    c.bind(lend);
                }
                return;
            }

            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                int lcond = c.newLabel(), lend = c.newLabel();
                c.bind(lcond);
                evalExpr(w->cond);
                c.pop(RAX);
                c.testRR(RAX, RAX);
                c.jcc32(CJe, lend);
                breakLbl.push_back(lend);
                contLbl.push_back(lcond);
                {
                    int osc = (int)arcLocals.size();
                    breakScope.push_back(osc);
                    contScope.push_back(osc);
                }
                genStmt(w->body);
                breakLbl.pop_back();
                contLbl.pop_back();
                breakScope.pop_back();
                contScope.pop_back();
                c.jmp32(lcond);
                c.bind(lend);
                return;
            }

            case StmtKind::Loop: {
                auto* l = static_cast<LoopStmt*>(s);
                int lcond = c.newLabel(), lend = c.newLabel();
                c.bind(lcond);
                breakLbl.push_back(lend);
                contLbl.push_back(lcond);
                {
                    int osc = (int)arcLocals.size();
                    breakScope.push_back(osc);
                    contScope.push_back(osc);
                }
                genStmt(l->body);
                breakLbl.pop_back();
                contLbl.pop_back();
                breakScope.pop_back();
                contScope.pop_back();
                c.jmp32(lcond);
                c.bind(lend);
                return;
            }

            case StmtKind::For:
                genFor(static_cast<ForStmt*>(s));
                return;

            case StmtKind::Break:
                // 1.1：提前离开块时释放块内声明的引用型局部（arc.md §2.4）
                if (!breakScope.empty()) emitReleaseDownTo(breakScope.back());
                c.jmp32(breakLbl.back());
                return;

            case StmtKind::Continue:
                if (!contScope.empty()) emitReleaseDownTo(contScope.back());
                c.jmp32(contLbl.back());
                return;

            case StmtKind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                if (r->value) {
                    evalCoerced(r->value, curRet);
                    if (curRet && curRet->kind == TyKind::Float) {
                        c.movsdFromMem(0, RSP, 0);
                        addRspImm(8);
                        emitFnExitFloat();
                    } else {
                        c.pop(RAX);
                        // 返回值归调用方所有：先 retain 保住，再归还本帧临时对象，
                        // 最后登记进调用方的 pending 区
                        emitFnExit(true, eNeedsOwn(curRet));
                    }
                } else {
                    // void 返回：返回值寄存器归零；Optional 返回：none = 0
                    c.zeroReg(RAX);
                    emitFnExit(true, false);
                }
                // 1.1：return 前释放所有引用型局部（返回值已在上一步 retain）
                emitReleaseDownTo(0);
                c.frameLeave();  // mov rsp,rbp / pop rbp / ret（arm64 恢复 x29,x30）
                return;
            }
        }
    }

    void genFor(ForStmt* f) {
        if (f->mode == ForMode::Range) {
            pushScope();
            pushArcScope();
            int arcMark = (int)arcLocals.size() - 1;
            int vslot = declareVar(f->var, TyStore::int64Ty());
            evalExpr(f->start);
            c.pop(RAX);
            c.movmr(RBP, vslot, RAX);
            // 终点只求值一次（与 C 后端语义一致）
            int eslot = declareVar("__forend" + std::to_string(uid_++));
            evalExpr(f->end);
            c.pop(RAX);
            c.movmr(RBP, eslot, RAX);
            int lcond = c.newLabel(), lcont = c.newLabel(),
                lend = c.newLabel();
            c.bind(lcond);
            c.movrm(RAX, RBP, vslot);
            c.opRM(0x3B, RAX, RBP, eslot);       // cmp i, end
            c.jcc32(f->inclusive ? CJg : CJge, lend);
            breakLbl.push_back(lend);
            contLbl.push_back(lcont);
            breakScope.push_back(arcMark);
            contScope.push_back(arcMark + 1);
            genStmt(f->body);
            breakLbl.pop_back();
            contLbl.pop_back();
            breakScope.pop_back();
            contScope.pop_back();
            c.bind(lcont);                        // continue 落到自增
            incMem(RBP, vslot);                   // i += 1
            c.jmp32(lcond);
            c.bind(lend);
            releaseScope(arcMark);
            popScope();
            return;
        }

        // ---- for-in：迭代数组或字符串 ----
        pushScope();
        pushArcScope();
        int arcMark2 = (int)arcLocals.size() - 1;
        int islot = declareVar("__iter", nullptr);
        evalExpr(f->iterable);
        c.pop(RAX);
        c.movmr(RBP, islot, RAX);
        // 0.9.4：被迭代对象要跨语句存活（循环体里每条语句结束都会 flush），
        // 因此这里自持一份引用，循环出口统一释放。
        if (arcOn && eIsRefTy(f->iterable->ty)) {
            retainLocal(islot);
            noteArcLocal(islot);
        }
        int vslot = declareVar(f->var, f->varTy);
        int ixslot = declareVar("__ix", nullptr);
        int nslot = declareVar("__n", nullptr);
        int lcond = c.newLabel(), lcont = c.newLabel(), lend = c.newLabel();
        // len 快照：迭代中 push/pop 不影响本次遍历（防无限循环）
        c.movrm(RAX, RBP, islot);
        c.movrm(RAX, RAX, 0);
        c.movmr(RBP, nslot, RAX);
        c.movmi(RBP, ixslot, 0);
        c.bind(lcond);
        c.movrm(RAX, RBP, ixslot);
        c.opRM(0x3B, RAX, RBP, nslot);           // cmp ix, n
        c.jcc32(CJge, lend);
        if (f->iterTy && f->iterTy->kind == TyKind::String) {
            // var = luxrt_str_char_at(iter, ix)：p0=iter（先压）、p1=ix
            c.push(RAX);
            c.movrm(RAX, RBP, islot);
            c.movmr(RSP, 0, RAX);
            c.push(RAX);
            c.movrm(RAX, RBP, ixslot);
            c.movmr(RSP, 0, RAX);
            c.call32(symLabel("luxrt_str_char_at"));
            addRspImm(16);
            c.movmr(RBP, vslot, RAX);
        } else {
            // elem = data[ix]（data = [iter+16]；ix < n 保证在界内）
            bool elemF = f->varTy && f->varTy->kind == TyKind::Float;
            c.movrm(RCX, RBP, islot);
            c.movrm(RDX, RCX, 16);               // data
            c.movrm(RAX, RBP, ixslot);
            if (elemF) {
                c.movsdSib(0, RDX, RAX, 3);
                c.movqRXmm(RAX, 0);
                c.movmr(RBP, vslot, RAX);
            } else {
                movSib(RCX, RDX, RAX, 3);
                c.movmr(RBP, vslot, RCX);
            }
        }
        breakLbl.push_back(lend);
        contLbl.push_back(lcont);
        breakScope.push_back(arcMark2);
        contScope.push_back(arcMark2 + 1);
        genStmt(f->body);
        breakLbl.pop_back();
        contLbl.pop_back();
        breakScope.pop_back();
        contScope.pop_back();
        c.bind(lcont);
        incMem(RBP, ixslot);
        c.jmp32(lcond);
        c.bind(lend);
        releaseScope(arcMark2);
        popScope();
    }

    // =====================================================================
    //  栈槽预扫描：prologue 按 countSlots + 余量预留，declareVar 不会越界
    //  （表达式内的 ArrayLit 会临时 declareVar __arrlit，一并计入）
    // =====================================================================
    int countExprSlots(Expr* e) {
        if (!e) return 0;
        switch (e->kind) {
            case ExprKind::ArrayLit: {
                auto* al = static_cast<ArrayLitExpr*>(e);
                int n = 1;  // __arrlit 临时槽
                for (Expr* el : al->elems) n += countExprSlots(el);
                return n;
            }
            case ExprKind::StructLit: {
                auto* sl = static_cast<StructLitExpr*>(e);
                int n = 1;  // __st 临时槽
                for (auto& kv : sl->inits) n += countExprSlots(kv.second);
                return n;
            }
            case ExprKind::Member:
                return countExprSlots(static_cast<MemberExpr*>(e)->base);
            case ExprKind::If: {
                auto* ie = static_cast<IfExpr*>(e);
                return countExprSlots(ie->cond) +
                       countExprSlots(ie->thenVal) +
                       countExprSlots(ie->elseVal);
            }
            case ExprKind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                return countExprSlots(b->lhs) + countExprSlots(b->rhs);
            }
            case ExprKind::Unary:
                return countExprSlots(static_cast<UnaryExpr*>(e)->operand);
            case ExprKind::Index: {
                auto* ix = static_cast<IndexExpr*>(e);
                return countExprSlots(ix->base) + countExprSlots(ix->index);
            }
            case ExprKind::Call: {
                auto* call = static_cast<CallExpr*>(e);
                // format() 在 evalCall 里额外 declareVar("__fmt") 占 1 槽，
                // 预扫描必须计入，否则 3 个以上 format 调用会让 __fmt 落到
                // rsp 之下，下一次 call 的返回地址会覆盖它（栈帧溢出）。
                int n = (call->builtin == Builtin::StrFormat) ? 1 : 0;
                // struct / struct 数组的打印 / to_str 会额外 declareVar 临时槽
                if (call->builtin == Builtin::Print ||
                    call->builtin == Builtin::PrintLn ||
                    call->builtin == Builtin::ToString ||
                    call->builtin == Builtin::StrFormat) {
                    for (Expr* a : call->args) n += strSlotsForType(a->ty);
                }
                for (Expr* a : call->args) n += countExprSlots(a);
                return n;
            }
            case ExprKind::Try:
                return countExprSlots(static_cast<TryExpr*>(e)->operand);
            default:
                return 0;
        }
    }

    int countSlots(Stmt* s) {
        if (!s) return 0;
        switch (s->kind) {
            case StmtKind::Let: {
                auto* let = static_cast<LetStmt*>(s);
                return 1 + countExprSlots(let->init);
            }
            case StmtKind::Expr:
                return countExprSlots(static_cast<ExprStmt*>(s)->expr);
            case StmtKind::Assign: {
                auto* a = static_cast<AssignStmt*>(s);
                int n = countExprSlots(a->value) + countExprSlots(a->target);
                // 代码生成在赋值时额外 declareVar 的临时槽
                if (a->target && a->target->kind == ExprKind::Member) n += 1;
                if (a->target && a->target->kind == ExprKind::Index)
                    n += a->compound ? 3 : 2;
                return n;
            }
            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                return countExprSlots(i->cond) + countSlots(i->thenBranch) +
                       countSlots(i->elseBranch);
            }
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                return countExprSlots(w->cond) + countSlots(w->body);
            }
            case StmtKind::Loop:
                return countSlots(static_cast<LoopStmt*>(s)->body);
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                // Range：循环变量 + 终点槽；In：迭代对象 + 变量 + 下标 + len
                int n = f->mode == ForMode::Range
                            ? 2 + countExprSlots(f->start) +
                                  countExprSlots(f->end)
                            : 4 + countExprSlots(f->iterable);
                return n + countSlots(f->body);
            }
            case StmtKind::Return:
                return countExprSlots(static_cast<ReturnStmt*>(s)->value);
            case StmtKind::Block: {
                int n = 0;
                for (Stmt* st : static_cast<BlockStmt*>(s)->stmts)
                    n += countSlots(st);
                return n;
            }
            default:
                return 0;
        }
    }

    // =====================================================================
    //  函数 / 程序 / 入口
    // =====================================================================
    // =====================================================================
    //  ARC 必要性分析（1.2 性能）
    //  仅当函数体可能创建堆对象（引用 / 可选类型，或调用可能创建堆对象的
    //  函数）时才需要 pending flush / 返回值登记。对纯值计算函数关闭 ARC
    //  插桩，省掉每条语句的 luxrt_pend_flush_to 调用——行为完全不变（ARC
    //  只影响内存回收，不影响可观察结果）。
    // =====================================================================
    std::unordered_map<const FuncDecl*, bool> pendCache_;
    std::unordered_set<const FuncDecl*> pendBusy_;

    static bool tyMayPend(const Ty* t) {
        if (!t) return false;
        return eIsRefTy(t) || t->kind == TyKind::Optional;
    }

    bool exprMayPend(Expr* e) {
        if (!e) return false;
        if (tyMayPend(e->ty)) return true;  // 引用 / 可选类型一律保守
        switch (e->kind) {
            case ExprKind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                return exprMayPend(b->lhs) || exprMayPend(b->rhs);
            }
            case ExprKind::Unary:
                return exprMayPend(static_cast<UnaryExpr*>(e)->operand);
            case ExprKind::Index: {
                auto* i = static_cast<IndexExpr*>(e);
                return exprMayPend(i->base) || exprMayPend(i->index);
            }
            case ExprKind::Slice: {
                auto* s = static_cast<SliceExpr*>(e);
                return exprMayPend(s->base) || exprMayPend(s->start) ||
                       exprMayPend(s->end);
            }
            case ExprKind::Member:
                return exprMayPend(static_cast<MemberExpr*>(e)->base);
            case ExprKind::ArrayLit: {
                for (Expr* el : static_cast<ArrayLitExpr*>(e)->elems)
                    if (exprMayPend(el)) return true;
                return false;
            }
            case ExprKind::StructLit: {
                for (auto& kv : static_cast<StructLitExpr*>(e)->inits)
                    if (exprMayPend(kv.second)) return true;
                return false;
            }
            case ExprKind::If: {
                auto* ie = static_cast<IfExpr*>(e);
                return exprMayPend(ie->cond) || exprMayPend(ie->thenVal) ||
                       exprMayPend(ie->elseVal);
            }
            case ExprKind::Call: {
                auto* c = static_cast<CallExpr*>(e);
                for (Expr* a : c->args)
                    if (exprMayPend(a)) return true;
                if (c->builtin != Builtin::None) {
                    // 特权内建的 __call* 会间接调用未知函数；其余不创建 Lux 对象
                    return c->builtin == Builtin::IntrCall1 ||
                           c->builtin == Builtin::IntrCall2;
                }
                if (c->viaValue) return true;  // 间接调用：目标未知
                if (c->target) return fnMayPend(c->target);
                return true;  // 未知调用
            }
            case ExprKind::Try:
                return exprMayPend(static_cast<TryExpr*>(e)->operand);
            default:
                return false;
        }
    }

    bool stmtMayPend(Stmt* s) {
        if (!s) return false;
        switch (s->kind) {
            case StmtKind::Let: {
                auto* l = static_cast<LetStmt*>(s);
                if (tyMayPend(l->resolved)) return true;
                return exprMayPend(l->init);
            }
            case StmtKind::Expr:
                return exprMayPend(static_cast<ExprStmt*>(s)->expr);
            case StmtKind::Assign: {
                auto* a = static_cast<AssignStmt*>(s);
                if (tyMayPend(a->elemTy)) return true;
                return exprMayPend(a->value) || exprMayPend(a->target);
            }
            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                return exprMayPend(i->cond) || stmtMayPend(i->thenBranch) ||
                       stmtMayPend(i->elseBranch);
            }
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                return exprMayPend(w->cond) || stmtMayPend(w->body);
            }
            case StmtKind::Loop:
                return stmtMayPend(static_cast<LoopStmt*>(s)->body);
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                return exprMayPend(f->start) || exprMayPend(f->end) ||
                       exprMayPend(f->iterable) || stmtMayPend(f->body);
            }
            case StmtKind::Return:
                return exprMayPend(static_cast<ReturnStmt*>(s)->value);
            case StmtKind::Block: {
                for (Stmt* st : static_cast<BlockStmt*>(s)->stmts)
                    if (stmtMayPend(st)) return true;
                return false;
            }
            default:
                return false;
        }
    }

    // 函数是否可能需要 pending flush。返回值类型是引用 / 可选时必须保留
    // ARC（返回值要 retain + 登记给调用方）。递归调用保守视为需要。
    bool fnMayPend(FuncDecl* fn) {
        auto it = pendCache_.find(fn);
        if (it != pendCache_.end()) return it->second;
        if (!pendBusy_.insert(fn).second) return true;  // 递归：保守
        bool r = tyMayPend(fn->retTy);
        if (!r) r = stmtMayPend(fn->body);
        pendBusy_.erase(fn);
        pendCache_[fn] = r;
        return r;
    }

    void genFunc(FuncDecl* fn) {
        curFile = fn->file.empty() ? curSourceName : fn->file;
        curFnKey = keyOf(*fn);
        curRet = fn->retTy;
        // 0.9.4（P0-1）：ARC 只作用于用户程序。运行时库（native_rt.lux）自身
        // 不插桩 —— 它分配的对象由 luxrt_alloc 在运行时自动登记进 pending，
        // 由**调用方**的语句级 flush 回收；运行时自身插桩反而会与
        // luxrt_pend_mark 等底层函数递归打架。
        arcOn = !inRuntime && g_nativeArc && fnMayPend(fn);
        pushScope();
        pushArcScope();
        int n = (int)fn->params.size();
        // prologue 预留：参数槽 + 函数体槽（预扫描，含 ArrayLit 临时）+ 余量
        // 0.9.4：另加 6 槽给 __pendbase / __rv 等 ARC 临时槽
        frameSize = 8 * n + 8 * countSlots(fn->body) + 8 * 6;
        c.frameEnter(frameSize);
        // 槽位从 -8 重新分配（预留量已覆盖参数 + 函数体 + 余量）
        frameSize = 0;
        // 参数从调用方栈区复制到局部槽：p_j = [rbp + 16 + 8*(n-1-j)]
        // （p0 先压在高地址；float 参数槽存位模式，与 int 槽同布局）
        for (int j = 0; j < n; j++) {
            int slot = declareVar(fn->params[j].name, fn->params[j].ty);
            c.loadParam(RAX, j, n);
            c.movmr(RBP, slot, RAX);
        }
        slotPend = declareVar("__pendbase", TyStore::int64Ty());
        slotRv = declareVar("__rv", TyStore::int64Ty());
        if (arcOn) {
            // 记下进入本函数时的 pending 深度：本函数所有语句都 flush 到它
            c.call32(symLabel("luxrt_pend_mark"));
            c.movmr(RBP, slotPend, RAX);
        }
        genStmt(fn->body);
        // 兜底 epilogue（0.5.1 起无条件生成；void 函数顺带把返回码归零，
        // 保证 void main 正常退出码 0）
        if (fn->retTy && fn->retTy->kind == TyKind::Void)
            c.zeroReg(RAX);
        emitFnExit(false, false);   // 归还本帧残余的临时对象
        c.frameLeave();
        popArcScope();
        popScope();
        frameSize = 0;
        arcOn = false;
    }

    void genProgram(Program* prog, bool isRt) {
        inRuntime = isRt;
        // 登记 struct 声明（用户程序，0.7）：打印 / to_str 需要查字段
        if (!isRt) {
            for (StructDecl* sd : prog->structs) structs_[sd->name] = sd;
            for (FuncDecl* fn : prog->funcs) {
                if (fn->name == "main" && fn->module.empty()) {
                    mainHasArgv_ = !fn->params.empty();
                }
            }
        }
        // 先注册全部函数符号（前向引用与相互调用都走同一标签）
        for (FuncDecl* fn : prog->funcs)
            if (!fn->isExtern) symLabel(keyOf(*fn));
        for (FuncDecl* fn : prog->funcs) {
            if (fn->isExtern) continue;
            int fnStart = (int)c.code.size();
            int flbl = bindFn(keyOf(*fn), fn->name, fn->nameLoc);
            genFunc(fn);
            if (getenv("LUX_SYMDUMP"))
                fprintf(stderr, "FN %-30s label=%d off=0x%x..0x%x\n", fn->name.c_str(), flbl, fnStart, (int)c.code.size());
        }
    }

    void genStart() {
        // _start：清帧指针，快照初始 sp（[sp]=argc, argv…, NULL, envp…——
        // 环境变量/参数从这里取，不依赖 /proc），调 main，exit
        c.entrySetup();
        if (mainHasArgv_) {
            // main(argv: string[])（0.7）：先构造 string[] 再作为实参入栈
            c.call32(symLabel("luxrt_argv"));
            c.push(RAX);
            c.movmr(RSP, 0, RAX);
        }
        c.call32(symLabel("main"));
        c.exitMain();
    }

};
