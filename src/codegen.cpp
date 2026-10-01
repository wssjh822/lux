// =============================================================================
//  codegen.cpp : C 后端 —— 把类型检查后的 AST 翻译成一份自包含的 C 源码
//
//  设计要点：
//    * 每个 Lux 类型直接映射到最贴近的 C 类型，没有装箱、没有虚表
//      int -> int64_t   float -> double   bool -> bool   string -> const char*
//    * 用户标识符统一加 "lxv_" / "lxm_" 前缀，避免与 C 关键字 / 库符号 / 运行时
//      lx_* 命名空间冲突；来自模块的声明再加模块名前缀（lxm_模块_名字）
//    * 生成的 C 自带运行时（打印、字符串、转换、读行、除零检查）
//    * 用 #line 指令把 C 编译器的诊断信息指回 .lux 源文件的行号
// =============================================================================
#include "lux.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace lux {

namespace {

// -----------------------------------------------------------------------------
//  Lux 运行时（内联进生成的 C 文件，保证单文件自包含）
// -----------------------------------------------------------------------------
const char* kRuntime = R"CLUX_RUNTIME(
/* ------------------------- Lux 运行时 ------------------------- */
/* 抑制 GCC 对静态字面量对象（refs = -1）的 free 误报：
   lx_gc_release 中 refs<0 必然提前返回，但 GCC 内联 + 常量传播后仍会
   触发 -Wfree-nonheap-object（已知假阳性）。 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif
#define _POSIX_C_SOURCE 200809L  /* clock_gettime / nanosleep */
#define _DEFAULT_SOURCE          /* random / srandom */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

static inline void lx_panic(const char* msg) {
    /* 先冲刷 stdout：panic 前已打印的内容不因块缓冲丢失，
     * 且保证与 stderr 的交错顺序确定（0.5.1） */
    fflush(stdout);
    fprintf(stderr, "lux: 运行时错误: %s\n", msg);
    fprintf(stderr, "lux: （如果是递归函数，也可能是调用层数过深导致栈溢出）\n");
    exit(1);
}

/* ---- 输出 ---- */
/* 最短往返表示：从 %.15g 试到 %.17g，取第一个能精确还原原值的写法。
 * 0.5 修复：旧版固定 %.15g 会截断精度（pi 打成 3.14159265358979）。 */
static inline int lx_f64_shortest(double v, char* out) {
    /* 0.9.4：NaN 一律打印 "nan"，不带符号。glibc 会按 NaN 的符号位印出
     * "-nan"，aarch64 的默认 NaN 却是正号 —— 同一份源码在不同架构上
     * 输出不同。打印层统一去掉 NaN 的符号，inf 仍保留 +- 。 */
    if (v != v) {
        memcpy(out, "nan", 4);   /* 含结尾 NUL：调用方按 n+1 字节拷贝 */
        return 3;
    }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(out, 40, "%.*g", prec, v);
        if (strtod(out, NULL) == v) return (int)strlen(out);
    }
    snprintf(out, 40, "%.17g", v);
    return (int)strlen(out);
}

static inline void lx_print_i64(int64_t v) { printf("%lld", (long long)v); }
static inline void lx_print_f64(double v) {
    char buf[40];
    int n = lx_f64_shortest(v, buf);
    fwrite(buf, 1, (size_t)n, stdout);
}
static inline void lx_print_bool(bool v)   { fputs(v ? "true" : "false", stdout); }
static inline void lx_print_str(const char* s) { fputs(s ? s : "", stdout); }
static inline void lx_print_nl(void) { putchar('\n'); }

/* ---- 字符串 ---- */
static inline int64_t lx_str_len(const char* s) { return (int64_t)strlen(s); }
static inline bool lx_str_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }
static inline int lx_str_cmp(const char* a, const char* b) { return strcmp(a, b); }

/* ---- 引用计数（ARC，0.9.2 实验性） ----
 * 每个堆对象（string / 数组 / struct）前面有一个 16 字节头：refs + on_zero。
 *   refs >= 0 : 堆对象，release 归零时调用 on_zero 再 free 头。
 *   refs == -1: 静态不可变对象（字符串字面量），retain/release 跳过。
 * 默认关闭（lx_gc_enabled=0）：生成的代码不插入 retain/release，
 * 堆对象只增不减，与 0.9.1 行为一致；--arc 时在 main 里置 1。
 * C 后端与原生后端输出不受其影响（内存管理不改变可观察行为）。 */
typedef struct lx_gc_hdr {
    int64_t refs;
    void (*on_zero)(void*);
} lx_gc_hdr;

static int lx_gc_enabled = 0;

static inline void* lx_gc_retain(void* p) {
    if (!lx_gc_enabled || !p) return p;
    lx_gc_hdr* h = ((lx_gc_hdr*)p) - 1;
    if (h->refs >= 0) h->refs++;
    return p;
}

static inline void lx_gc_release(void* p) {
    if (!lx_gc_enabled || !p) return;
    lx_gc_hdr* h = ((lx_gc_hdr*)p) - 1;
    if (h->refs < 0) return;
    if (--h->refs == 0) {
        if (h->on_zero) h->on_zero(p);
        free(h);
    }
}

/* 供 __attribute__((cleanup)) 使用：p 指向变量本身 */
static inline void lx_gc_releasep(void* p) {
    lx_gc_release(*(void**)p);
}

static inline char* lx_alloc(size_t n) {
    lx_gc_hdr* h = (lx_gc_hdr*)calloc(1, sizeof(lx_gc_hdr) + n);
    if (!h) lx_panic("内存分配失败");
    h->refs = 1;
    h->on_zero = 0;
    return (char*)(h + 1);
}

static inline char* lx_str_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = lx_alloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

static inline char* lx_str_concat3(const char* a, const char* b,
                                   const char* c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char* r = lx_alloc(la + lb + lc + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    memcpy(r + la + lb, c, lc + 1);
    return r;
}

/* ---- 全局错误通道（0.9.4，P1-5）----
 * T? 只回答“成没成”，不回答“为什么”。last_error() 补上这条信息：
 * 标准库失败路径把一句人话写进全局槽，脚本层据此区分“不存在 / 没权限”。
 * 无并发（Lux 不引入线程），单个全局量足够，且不改动 T? 的签名。
 * 存的是带 gc 头的 Lux 字符串对象，首次读取时构造并作为进程级根永久持有。 */
static const char* lx_last_err_msg = "";
static const char* lx_last_err_obj = NULL;

static inline void lx_set_error(const char* msg) {
    lx_last_err_msg = msg ? msg : "";
    lx_last_err_obj = NULL;   /* 懒重建 */
}

static inline const char* lx_last_error(void) {
    if (!lx_last_err_obj) {
        size_t n = strlen(lx_last_err_msg);
        char* r = lx_alloc(n + 1);
        memcpy(r, lx_last_err_msg, n + 1);
        lx_last_err_obj = r;   /* refs = 1，永不释放：进程级根 */
    }
    return lx_last_err_obj;
}

/* ---- 可选值（0.8 错误通道） ----
 * T? 统一表示为 <T>*：指向堆上一个存放 T 的槽，NULL 表示 none。
 * 这样 int? / float? / string? / 数组? / struct? 都是单个指针，
 * 两个后端的 ABI 都能用 8 字节槽传递。
 *
 * 0.9.4（P0-3）：槽不再「只增不减」。槽与堆对象同构，前面也有 16 字节
 * 的 lx_gc_hdr；载荷是引用类型时 on_zero = lx_gc_releasep，槽归零时
 * 顺带释放内层对象。局部 T? 变量挂 __attribute__((cleanup(lx_opt_releasep)))，
 * 在作用域出口统一回收（牺牲一点峰值内存换正确性，与设计文档 §2.2 一致）。
 *
 * 注意：槽的回收与 lx_gc_enabled 无关 —— 即便 --no-arc，装箱也不在
 * 循环里无限增长；--no-arc 下内层引用不参与回收（on_zero 里的
 * lx_gc_release 是空操作），语义与「只增不减」保持一致。 */
static inline void* lx_opt_alloc(size_t n, void (*on_zero)(void*)) {
    lx_gc_hdr* h = (lx_gc_hdr*)calloc(1, sizeof(lx_gc_hdr) + n);
    if (!h) lx_panic("内存分配失败（可选值装箱）");
    h->refs = 1;
    h->on_zero = on_zero;
    return (char*)(h + 1);
}

static inline void* lx_opt_retain(void* p) {
    if (!p) return p;
    lx_gc_hdr* h = ((lx_gc_hdr*)p) - 1;
    if (h->refs >= 0) h->refs++;
    return p;
}

static inline void lx_opt_release(void* p) {
    if (!p) return;
    lx_gc_hdr* h = ((lx_gc_hdr*)p) - 1;
    if (h->refs < 0) return;
    if (--h->refs == 0) {
        if (h->on_zero) h->on_zero(p);
        free(h);
    }
}

/* 供 __attribute__((cleanup)) 使用：p 指向变量本身 */
static inline void lx_opt_releasep(void* p) { lx_opt_release(*(void**)p); }

/* 所有权取出：释放槽本身但**不**调用 on_zero —— 载荷已被调用方取走。
 * 用于 or 兜底 / ? 传播 / name! 解包这三处「消费掉一个可选值」的位置。 */
static inline void lx_opt_drop(void* p) {
    if (!p) return;
    lx_gc_hdr* h = ((lx_gc_hdr*)p) - 1;
    h->on_zero = 0;
    h->refs = 1;
    lx_opt_release(p);
}

static inline void lx_panic_opt(const char* what) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%s 失败：值为 none", what);
    lx_panic(buf);
}

/* 文件读取专用：把路径一并写进 panic，用户知道是哪个文件失败 */
static inline void lx_panic_opt_file(const char* what, const char* path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s 失败：无法读取 '%s'", what,
             path ? path : "");
    lx_panic(buf);
}

/* 切片越界：文案与 stability.md §3 冻结表逐字对齐 */
static inline void lx_panic_slice(int64_t lo, int64_t hi, int64_t n) {
    char buf[160];
    snprintf(buf, sizeof(buf), "切片范围越界：起点 %lld，终点 %lld，长度 %lld",
             (long long)lo, (long long)hi, (long long)n);
    lx_panic(buf);
}

/* 字符串 → int?：解析失败（空 / 有残余字符 / 溢出）返回 NULL */
static inline int64_t* lx_str_to_i64_opt(const char* s) {
    if (!s) return NULL;
    char* end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || (end && *end != '\0') || errno != 0) {
        lx_set_error("int 解析失败：不是合法的 64 位整数");
        return NULL;
    }
    int64_t* p = (int64_t*)lx_opt_alloc(sizeof(int64_t), 0);
    *p = (int64_t)v;
    return p;
}

/* 字符串 → float?：解析失败返回 NULL */
static inline double* lx_str_to_f64_opt(const char* s) {
    if (!s) return NULL;
    char* end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || (end && *end != '\0')) {
        lx_set_error("float 解析失败：不是合法的浮点数");
        return NULL;
    }
    double* p = (double*)lx_opt_alloc(sizeof(double), 0);
    *p = v;
    return p;
}

static inline char* lx_str_dup(const char* s) {
    size_t n = strlen(s);
    char* r = lx_alloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

/* 手写 itoa：比 snprintf 快 3~5 倍（整数转字符串是热点路径） */
static inline char* lx_i64_to_str(int64_t v) {
    char tmp[24];
    size_t n = 0;
    uint64_t u = (uint64_t)v;
    if (v < 0) u = (uint64_t)(-(v + 1)) + 1;  /* 正确处理 INT64_MIN */
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    size_t neg = (v < 0) ? 1 : 0;
    char* r = lx_alloc(n + neg + 1);
    if (neg) r[0] = '-';
    for (size_t i = 0; i < n; i++) r[neg + i] = tmp[n - 1 - i];
    r[n + neg] = '\0';
    return r;
}

static inline char* lx_f64_to_str(double v) {
    char buf[40];
    int n = lx_f64_shortest(v, buf);
    char* r = lx_alloc((size_t)n + 1);
    memcpy(r, buf, (size_t)n + 1);
    return r;
}

static inline const char* lx_bool_to_str(bool v) { return v ? "true" : "false"; }

/* ---- string 模块 ---- */
static inline bool lx_str_contains(const char* s, const char* sub) {
    return strstr(s, sub) != NULL;
}

static inline bool lx_str_startswith(const char* s, const char* pre) {
    return strncmp(s, pre, strlen(pre)) == 0;
}

static inline bool lx_str_endswith(const char* s, const char* suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return lf <= ls && memcmp(s + ls - lf, suf, lf) == 0;
}

static inline int64_t lx_str_find(const char* s, const char* sub) {
    const char* p = strstr(s, sub);
    return p ? (int64_t)(p - s) : -1;
}

/* find_opt(s, sub)：find 的可选变体，找不到返回 none 并写 last_error()
 * （0.9.4，P1-8：扫掉 A3 尾巴，不再让调用方跟 -1 打交道） */
static inline int64_t* lx_str_find_opt(const char* s, const char* sub) {
    int64_t i = lx_str_find(s, sub);
    if (i < 0) {
        lx_set_error("find_opt 失败：未找到子串");
        return NULL;
    }
    int64_t* p = (int64_t*)lx_opt_alloc(sizeof(int64_t), 0);
    *p = i;
    return p;
}

static inline char* lx_str_trim(const char* s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    char* r = lx_alloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static inline char* lx_str_upper(const char* s) {
    size_t n = strlen(s);
    char* r = lx_alloc(n + 1);
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        r[i] = c;
    }
    return r;
}

static inline char* lx_str_lower(const char* s) {
    size_t n = strlen(s);
    char* r = lx_alloc(n + 1);
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        r[i] = c;
    }
    return r;
}

static inline char* lx_str_substr(const char* s, int64_t start, int64_t len) {
    int64_t n = (int64_t)strlen(s);
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0) len = 0;
    if (len > n - start) len = n - start;
    char* r = lx_alloc((size_t)len + 1);
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}

static inline char* lx_str_replace(const char* s, const char* old,
                                   const char* new) {
    size_t lo = strlen(old);
    if (lo == 0) return lx_str_dup(s);
    size_t ls = strlen(s), ln = strlen(new);
    /* 先数出现次数，一次算好容量 */
    size_t cnt = 0;
    const char* p = s;
    while ((p = strstr(p, old)) != NULL) {
        cnt++;
        p += lo;
    }
    size_t cap = ls + (ln > lo ? (ln - lo) * cnt : 0) + 1;
    char* r = lx_alloc(cap);
    char* w = r;
    p = s;
    while (*p) {
        const char* q = strstr(p, old);
        if (!q) {
            size_t rest = ls - (size_t)(p - s);
            memcpy(w, p, rest);
            w += rest;
            break;
        }
        size_t pre = (size_t)(q - p);
        memcpy(w, p, pre);
        w += pre;
        memcpy(w, new, ln);
        w += ln;
        p = q + lo;
    }
    *w = '\0';
    return r;
}

/* ---- 字符串构建器（format / 数组打印 / 未来 String Builder 的地基） ---- */
typedef struct lx_sb { char* p; size_t len, cap; } lx_sb;

static void lx_sb_push(lx_sb* b, const void* data, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n + 1) nc *= 2;
        /* 缓冲区带 ARC 头：realloc 的是头部，对外指针是头部之后 */
        lx_gc_hdr* h = b->p ? (((lx_gc_hdr*)b->p) - 1) : 0;
        lx_gc_hdr* nh = (lx_gc_hdr*)realloc(h, sizeof(lx_gc_hdr) + nc);
        if (!nh) lx_panic("内存分配失败");
        if (!h) { nh->refs = 1; nh->on_zero = 0; }
        b->p = (char*)(nh + 1);
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static inline void lx_sb_push_lit(lx_sb* b, const char* s) {
    lx_sb_push(b, s, strlen(s));
}

/* ---- struct（0.7）：字段对象堆分配，布局 = N*8 个槽 ----
 * struct 是引用语义（与数组一致）：变量 / 传参 / 返回共享同一个对象，
 * 字段可原地修改。字段类型为 int/float/bool/string/数组/struct，均占 8 字节。 */
static inline void* lx_struct_new(size_t n,
                                  void (*on_zero)(void*)) {
    lx_gc_hdr* h = (lx_gc_hdr*)calloc(1, sizeof(lx_gc_hdr) + n);
    if (!h) lx_panic("内存分配失败");
    h->refs = 1;
    h->on_zero = on_zero;
    return (void*)(h + 1);
}

/* ---- 数组（0.5 新增） ----
 * 动态数组：头部（len/cap/data/esz/元素打印器）堆分配，lx_arr 本身是
 * 指针——赋值 / 传参 / 返回都拷贝指针，多个变量共享同一个数组
 * （引用语义，与 Python list 一致，函数内 push 对调用方可见）。
 * 所有下标访问带越界检查，越界立即 panic。 */
struct lx_arr {
    int64_t len, cap;
    void* data;
    int64_t esz;  /* 单个元素的字节数 */
    void (*pelem)(lx_sb*, const void*);  /* 元素打印器（打印 / 转字符串用） */
    void (*relem)(void*);  /* 元素释放器（ARC：字符串/数组/struct 为
                              lx_gc_release，值类型为 0） */
};
typedef struct lx_arr* lx_arr;

/* 数组归零：先释放各元素（若元素是引用类型），再释放数据区。
   头本身由 lx_gc_release 统一 free。 */
static void lx_arr_on_zero(void* p) {
    lx_arr a = (lx_arr)p;
    if (a->relem && a->data) {
        for (int64_t i = 0; i < a->len; i++)
            a->relem((char*)a->data + (size_t)i * (size_t)a->esz);
    }
    free(a->data);
}

/* 新建空数组头（len=cap=0，data=NULL） */
static inline lx_arr lx_arr_new(int64_t esz,
                                void (*pelem)(lx_sb*, const void*),
                                void (*relem)(void*)) {
    lx_gc_hdr* h = (lx_gc_hdr*)calloc(1, sizeof(lx_gc_hdr) +
                                         sizeof(struct lx_arr));
    if (!h) lx_panic("内存分配失败");
    h->refs = 1;
    h->on_zero = lx_arr_on_zero;
    lx_arr a = (lx_arr)(h + 1);
    a->esz = esz;
    a->pelem = pelem;
    a->relem = relem;
    return a;
}

static void lx_bounds_fail(const char* what, int64_t i, int64_t len) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "%s越界：下标 %lld，但长度只有 %lld（下标从 0 开始）",
             what, (long long)i, (long long)len);
    lx_panic(buf);
}
static void lx_arr_bounds_fail(int64_t i, int64_t len) {
    lx_bounds_fail("数组下标", i, len);
}
static void lx_str_bounds_fail(int64_t i, int64_t len) {
    lx_bounds_fail("字符串下标", i, len);
}

static inline void lx_arr_reserve(lx_arr a, int64_t need) {
    if (need <= a->cap) return;
    int64_t nc = a->cap > 0 ? a->cap * 2 : 8;
    while (nc < need) nc *= 2;
    int64_t esz = a->esz > 0 ? a->esz : 8;
    void* nd = realloc(a->data, (size_t)(nc * esz));
    if (!nd) lx_panic("内存分配失败");
    a->data = nd;
    a->cap = nc;
}

/* 切片（0.6）：复制 [lo, hi) 元素到新数组；hi<0 表示到末尾。
   越界（lo<0、hi>len、lo>hi）一律 panic，与下标访问同风格。 */
static lx_arr lx_arr_slice(lx_arr a, int64_t lo, int64_t hi) {
    int64_t n = a->len;
    if (hi < 0) hi = n;
    if (lo < 0 || hi > n || lo > hi)
        lx_panic_slice(lo, hi, n);
    int64_t esz = a->esz > 0 ? a->esz : 8;
    lx_arr r = lx_arr_new(esz, a->pelem, a->relem);
    int64_t cnt = hi - lo;
    if (cnt > 0) {
        lx_arr_reserve(r, cnt);
        memcpy(r->data, (char*)a->data + lo * esz, (size_t)(cnt * esz));
        r->len = cnt;
        /* 切片与原数组各持一份元素引用（ARC）：retain 每个元素值 */
        if (r->relem)
            for (int64_t i = 0; i < cnt; i++)
                lx_gc_retain(*(void**)((char*)r->data +
                                       (size_t)i * (size_t)esz));
    }
    return r;
}

/* 字符串切片（0.6）：复制 [lo, hi) 字节到新字符串；hi<0 表示到末尾。 */
static inline char* lx_str_slice(const char* s, int64_t lo, int64_t hi) {
    int64_t n = lx_str_len(s);
    if (hi < 0) hi = n;
    if (lo < 0 || hi > n || lo > hi)
        lx_panic_slice(lo, hi, n);
    return lx_str_substr(s, lo, hi - lo);
}

/* 每种元素类型一套 push / get / set / pop / insert / remove（宏展开） */
#define LX_ARR_OPS(NAME, CTYPE)                                               \
    static inline void lx_arr_push_##NAME(lx_arr a, CTYPE v) {               \
        lx_arr_reserve(a, a->len + 1);                                       \
        ((CTYPE*)a->data)[a->len] = v;                                       \
        a->len++;                                                            \
    }                                                                        \
    static inline CTYPE lx_arr_get_##NAME(lx_arr a, int64_t i) {             \
        if (i < 0 || i >= a->len) lx_arr_bounds_fail(i, a->len);             \
        return ((CTYPE*)a->data)[i];                                         \
    }                                                                        \
    static inline void lx_arr_set_##NAME(lx_arr a, int64_t i, CTYPE v) {     \
        if (i < 0 || i >= a->len) lx_arr_bounds_fail(i, a->len);             \
        if (a->relem)                                                        \
            a->relem((char*)a->data + (size_t)i * (size_t)a->esz);           \
        ((CTYPE*)a->data)[i] = v;                                            \
    }                                                                        \
    static inline CTYPE lx_arr_pop_##NAME(lx_arr a) {                        \
        if (a->len == 0) lx_panic("对空数组调用 pop()");                     \
        a->len--;                                                            \
        return ((CTYPE*)a->data)[a->len];                                    \
    }                                                                        \
    static inline void lx_arr_insert_##NAME(lx_arr a, int64_t i, CTYPE v) {  \
        if (i < 0 || i > a->len) lx_arr_bounds_fail(i, a->len);              \
        lx_arr_reserve(a, a->len + 1);                                       \
        char* base = (char*)a->data;                                         \
        memmove(base + (size_t)(i + 1) * (size_t)a->esz,                     \
                base + (size_t)i * (size_t)a->esz,                           \
                (size_t)(a->len - i) * (size_t)a->esz);                      \
        ((CTYPE*)a->data)[i] = v;                                            \
        a->len++;                                                            \
    }                                                                        \
    static inline CTYPE lx_arr_remove_##NAME(lx_arr a, int64_t i) {          \
        if (i < 0 || i >= a->len) lx_arr_bounds_fail(i, a->len);             \
        CTYPE v = ((CTYPE*)a->data)[i];                                      \
        char* base = (char*)a->data;                                         \
        memmove(base + (size_t)i * (size_t)a->esz,                           \
                base + (size_t)(i + 1) * (size_t)a->esz,                     \
                (size_t)(a->len - i - 1) * (size_t)a->esz);                  \
        a->len--;                                                            \
        return v;                                                            \
    }

LX_ARR_OPS(i64, int64_t)
LX_ARR_OPS(f64, double)
LX_ARR_OPS(bool, bool)
LX_ARR_OPS(str, const char*)
LX_ARR_OPS(arr, lx_arr)
LX_ARR_OPS(st, void*)  /* struct 数组：元素是 struct 指针（0.7） */


static inline void lx_arr_clear(lx_arr a) {
    if (a->relem && a->data) {
        for (int64_t i = 0; i < a->len; i++)
            a->relem((char*)a->data + (size_t)i * (size_t)a->esz);
    }
    a->len = 0;
}
static inline int64_t lx_arr_len(lx_arr a) { return a->len; }

/* 字符串下标：返回单字节字符（堆分配的 1 字符字符串） */
static inline char* lx_str_char_at(const char* s, int64_t i) {
    int64_t n = (int64_t)strlen(s);
    if (i < 0 || i >= n) lx_str_bounds_fail(i, n);
    char* r = lx_alloc(2);
    r[0] = s[i];
    r[1] = '\0';
    return r;
}

/* 元素打印器：把一个元素追加到字符串构建器（数组打印 / to_str 共用） */
static void lx_arr_sb(lx_sb* b, lx_arr a);

static void lx_pe_i64(lx_sb* b, const void* p) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)*(const int64_t*)p);
    lx_sb_push(b, tmp, (size_t)n);
}
static void lx_pe_f64(lx_sb* b, const void* p) {
    char tmp[40];
    int n = lx_f64_shortest(*(const double*)p, tmp);
    lx_sb_push(b, tmp, (size_t)n);
}
static void lx_pe_bool(lx_sb* b, const void* p) {
    lx_sb_push(b, *(const bool*)p ? "true" : "false",
               (size_t)(*(const bool*)p ? 4 : 5));
}
static void lx_pe_str(lx_sb* b, const void* p) {
    const char* s = *(const char* const*)p;
    lx_sb_push(b, "\"", 1);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') lx_sb_push(b, "\\", 1);
        lx_sb_push(b, s, 1);
    }
    lx_sb_push(b, "\"", 1);
}
static void lx_pe_arr(lx_sb* b, const void* p) { lx_arr_sb(b, *(const lx_arr*)p); }

/* argc/argv → string[]（main(argv) 用，argv[0] 为程序名，同 C 约定） */
static inline lx_arr lx_argv_new(int argc, char** argv) {
    lx_arr a = lx_arr_new(sizeof(char*), lx_pe_str, lx_gc_releasep);
    for (int i = 0; i < argc; i++) lx_arr_push_str(a, lx_str_dup(argv[i]));
    return a;
}

/* 把数组渲染成 [e1, e2, ...] 形式追加到构建器（嵌套数组递归） */
static void lx_arr_sb(lx_sb* b, lx_arr a) {
    lx_sb_push(b, "[", 1);
    for (int64_t i = 0; i < a->len; i++) {
        if (i) lx_sb_push(b, ", ", 2);
        if (a->pelem) {
            a->pelem(b, (const char*)a->data + (size_t)i * (size_t)a->esz);
        } else {
            lx_sb_push(b, "?", 1);
        }
    }
    lx_sb_push(b, "]", 1);
}

static char* lx_arr_to_str(lx_arr a) {
    lx_sb b = {0, 0, 0};
    lx_arr_sb(&b, a);
    if (!b.p) {
        b.p = lx_alloc(1);
        b.p[0] = '\0';
    }
    return b.p;
}

/* ---- string 模块的数组扩展（0.5） ---- */
static lx_arr lx_str_split(const char* s, const char* sep) {
    if (*sep == '\0') lx_panic("split() 的分隔符不能是空字符串");
    lx_arr a = lx_arr_new((int64_t)sizeof(char*), lx_pe_str, lx_gc_releasep);
    size_t ls = strlen(sep);
    const char* p = s;
    for (;;) {
        const char* q = strstr(p, sep);
        if (!q) {
            lx_arr_push_str(a, lx_str_dup(p));
            break;
        }
        size_t n = (size_t)(q - p);
        char* piece = lx_alloc(n + 1);
        memcpy(piece, p, n);
        piece[n] = '\0';
        lx_arr_push_str(a, piece);
        p = q + ls;
    }
    return a;
}

static lx_arr lx_str_chars(const char* s) {
    lx_arr a = lx_arr_new((int64_t)sizeof(char*), lx_pe_str, lx_gc_releasep);
    int64_t n = (int64_t)strlen(s);
    lx_arr_reserve(a, n);
    for (int64_t i = 0; i < n; i++) {
        char* c = lx_alloc(2);
        c[0] = s[i];
        c[1] = '\0';
        ((char**)a->data)[i] = c;
        a->len++;
    }
    return a;
}

static char* lx_str_join(lx_arr a, const char* sep) {
    lx_sb b = {0, 0, 0};
    for (int64_t i = 0; i < a->len; i++) {
        if (i) lx_sb_push(&b, sep, strlen(sep));
        const char* s = ((const char* const*)a->data)[i];
        lx_sb_push(&b, s, s ? strlen(s) : 0);
    }
    if (!b.p) {
        b.p = lx_alloc(1);
        b.p[0] = '\0';
    }
    return b.p;
}

/* format("x={} y={}", a, b)：把每个 {} 依次替换成后面参数（已转字符串） */
static char* lx_str_format_concat(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lx_sb b = {0, 0, 0};
    const char* p = fmt;
    for (;;) {
        const char* q = strstr(p, "{}");
        if (!q) {
            lx_sb_push(&b, p, strlen(p));
            break;
        }
        lx_sb_push(&b, p, (size_t)(q - p));
        const char* arg = va_arg(ap, const char*);
        lx_sb_push(&b, arg, strlen(arg));
        p = q + 2;
    }
    va_end(ap);
    if (!b.p) {
        b.p = lx_alloc(1);
        b.p[0] = '\0';
    }
    return b.p;
}

/* ---- 整数除法 / 取模：带除零与溢出检查 ---- */
static inline int64_t lx_idiv(int64_t a, int64_t b) {
    if (b == 0) lx_panic("整数除法的除数为 0");
    if (a == INT64_MIN && b == -1) lx_panic("整数除法结果溢出");
    return a / b;
}
static inline int64_t lx_imod(int64_t a, int64_t b) {
    if (b == 0) lx_panic("取模运算的除数为 0");
    if (a == INT64_MIN && b == -1) lx_panic("取模运算结果溢出");
    return a % b;
}

/* ---- 输入 ---- */
#define LX_MAX_LINE (1024 * 1024)  /* 单行输入上限 1 MiB */

static inline char* lx_read_line(void) {
    fflush(stdout);  /* 保证此前 print 的内容在交互提示前全部显示 */
    size_t cap = 128, len = 0;
    lx_gc_hdr* h = (lx_gc_hdr*)calloc(1, sizeof(lx_gc_hdr) + cap);
    if (!h) lx_panic("内存分配失败");
    h->refs = 1;
    h->on_zero = 0;
    char* buf = (char*)(h + 1);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            if (cap >= LX_MAX_LINE) {
                lx_panic("输入行过长（超过 1 MiB 上限）");
            }
            cap *= 2;
            h = (lx_gc_hdr*)realloc(h, sizeof(lx_gc_hdr) + cap);
            if (!h) lx_panic("内存分配失败");
            buf = (char*)(h + 1);
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

static inline char* lx_read_line_prompt(const char* prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
    return lx_read_line();
}

/* ---- 断言 ---- */
static inline void lx_assert_at(bool cond, const char* msg, int line) {
    if (!cond) {
        char buf[256];
        snprintf(buf, sizeof(buf), "assert 失败：%s（第 %d 行）",
                 msg ? msg : "条件不成立", line);
        lx_panic(buf);
    }
}

/* ---- time 模块 ---- */
static inline double lx_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 单调时钟：不受系统校时影响，适合测量耗时 */
static inline double lx_time_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline void lx_sleep(double sec) {
    if (sec < 0) lx_panic("sleep() 的秒数不能是负数");
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    while (nanosleep(&ts, &ts) != 0) { /* 被信号打断时继续睡完剩余时间 */ }
}

static inline void lx_sleep_ms(int64_t ms) {
    if (ms < 0) lx_panic("sleep_ms() 的毫秒数不能是负数");
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { /* 被信号打断时继续睡完剩余时间 */ }
}

/* ---- math 模块：随机数 ---- */
static inline double lx_random(void) {
    /* random() 的取值范围是 [0, 2^31-1]。不能用 RAND_MAX 做分母：
       它是 rand() 的上限，标准只保证 >= 32767（0.5.1 修复） */
    return (double)random() / ((double)0x7FFFFFFF + 1.0);
}
static inline void lx_seed(unsigned int s) { srandom(s); }

/* ---- system 模块 ---- */
static inline const char* lx_getenv(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

static inline bool lx_setenv(const char* name, const char* val) {
    return setenv(name, val, 1) == 0;
}

static inline int64_t lx_system(const char* cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
    if (WIFEXITED(st)) return (int64_t)WEXITSTATUS(st);
    return (int64_t)(st >> 8);
}

/* ---- file 模块 ---- */

static inline bool lx_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* 读整个文件 → string?（0.8）：打开失败返回 NULL */
static inline const char** lx_read_opt(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        /* 0.9.4（P1-5）：把“为什么”写进 last_error()，脚本据此区分
         * “不存在”与“没权限”——这是 T? 单靠 none 给不出的信息 */
        if (errno == ENOENT) lx_set_error("read 失败：文件不存在");
        else if (errno == EACCES) lx_set_error("read 失败：没有读取权限");
        else lx_set_error("read 失败：无法打开文件");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = lx_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    const char** p = (const char**)lx_opt_alloc(sizeof(char*), lx_gc_releasep);
    *p = buf;
    return p;
}

static inline bool lx_file_write(const char* path, const char* data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fputs(data, f) != EOF;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static inline bool lx_file_append(const char* path, const char* data) {
    FILE* f = fopen(path, "ab");
    if (!f) return false;
    bool ok = fputs(data, f) != EOF;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static inline bool lx_file_remove(const char* path) {
    return remove(path) == 0;
}

static inline bool lx_file_rename(const char* from, const char* to) {
    return rename(from, to) == 0;
}
/* ----------------------- Lux 运行时结束 ----------------------- */
)CLUX_RUNTIME";

// -----------------------------------------------------------------------------
//  代码生成器
// -----------------------------------------------------------------------------

// struct 类型在生成的 C 里的名字（typedef 名）
std::string structCName(const std::string& s) { return "lx_st_" + s; }

// 字段下标（与 Sema 的 StructDecl::fields 顺序一致）
int structFieldIndex(const StructDecl* sd, const std::string& name) {
    if (!sd) return -1;
    for (size_t i = 0; i < sd->fields.size(); i++)
        if (sd->fields[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string cType(const Ty* t) {
    switch (t->kind) {
        case TyKind::Int:
            return "int64_t";
        case TyKind::Float:
            return "double";
        case TyKind::Bool:
            return "bool";
        case TyKind::String:
            return "const char*";
        case TyKind::Void:
            return "void";
        case TyKind::Array:
            return "lx_arr";  // 0.5：动态数组（lx_arr 即堆头指针，赋值/传参共享）
        case TyKind::Named:
            return structCName(t->name) + "*";  // struct 是字段对象指针（0.7）
        case TyKind::Optional:
            // T? 统一表示为指向堆槽的指针，NULL = none（0.8）
            return cType(t->elem) + "*";
        default:
            return "int64_t";  // 兜底，保证生成的 C 依旧合法
    }
}

// 数组元素类型对应的运行时后缀 / sizeof / 元素打印器
std::string arrRT(const Ty* elem) {
    if (!elem) return "i64";
    switch (elem->kind) {
        case TyKind::Float: return "f64";
        case TyKind::Bool: return "bool";
        case TyKind::String: return "str";
        case TyKind::Array: return "arr";
        case TyKind::Named: return "st";  // struct 数组（0.7）
        default: return "i64";
    }
}

std::string arrEsz(const Ty* elem) {
    if (!elem) return "sizeof(void*)";
    switch (elem->kind) {
        case TyKind::Int: return "sizeof(int64_t)";
        case TyKind::Float: return "sizeof(double)";
        case TyKind::Bool: return "sizeof(bool)";
        case TyKind::String: return "sizeof(char*)";
        case TyKind::Array: return "sizeof(lx_arr)";
        case TyKind::Named: return "sizeof(void*)";
        default: return "sizeof(void*)";
    }
}

std::string arrPelem(const Ty* elem) {
    if (!elem) return "lx_pe_i64";
    switch (elem->kind) {
        case TyKind::Int: return "lx_pe_i64";
        case TyKind::Float: return "lx_pe_f64";
        case TyKind::Bool: return "lx_pe_bool";
        case TyKind::String: return "lx_pe_str";
        case TyKind::Array: return "lx_pe_arr";
        case TyKind::Named: return structCName(elem->name) + "_pelem";
        default: return "lx_pe_i64";
    }
}

// 数组元素释放器（ARC）：引用类型元素用 lx_gc_releasep（收槽地址），
// 值类型为 0（无需释放）
std::string arrRelem(const Ty* elem) {
    if (!elem) return "0";
    switch (elem->kind) {
        case TyKind::String:
        case TyKind::Array:
        case TyKind::Named: return "lx_gc_releasep";
        default: return "0";
    }
}

// 数组实体的"空值"：一个不指向任何数据的空数组（esz / pelem / relem 按元素类型补全）
std::string arrZero(const Ty* elem) {
    return "lx_arr_new(" + arrEsz(elem) + ", " + arrPelem(elem) + ", " +
           arrRelem(elem) + ")";
}

std::string zeroOf(const Ty* t) {
    switch (t->kind) {
        case TyKind::Int:
            return "0";
        case TyKind::Float:
            return "0.0";
        case TyKind::Bool:
            return "false";
        case TyKind::String:
            return "\"\"";
        case TyKind::Array:
            return arrZero(t->elem);
        case TyKind::Named:
            return "((" + structCName(t->name) + "*)0)";  // struct 空值 = NULL
        case TyKind::Optional:
            return "((" + cType(t) + ")0)";  // none = NULL（0.8）
        default:
            return "0";
    }
}

// 用户标识符的 C 名字改写。
// 0.8：用户名字改用 lxv_ / lxm_ 前缀，与运行时的 lx_* 命名空间彻底分开，
// 避免用户变量名（如 arr → lx_arr）撞上运行时类型 / 辅助函数。
std::string mangle(const std::string& name) {
    if (name == "main") return "main";  // 入口函数直接映射到 C 的 main
    return "lxv_" + name;
}

// 带模块前缀的名字改写：来自模块的声明避免与根文件 / 其他模块撞名
std::string mangleFor(const std::string& module, const std::string& name) {
    if (module.empty()) return mangle(name);
    return "lxm_" + module + "_" + name;
}

// 把 double 格式化成能精确还原的 C 浮点字面量
std::string formatDouble(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s(buf);
    // 整数值补 ".0"，让生成的 C 一眼能看出是浮点数
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('n') == std::string::npos && s.find('i') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

struct CGen {
    std::string buf;
    int indent = 0;
    int tmpId = 0;
    const CodegenOptions& opt;
    bool arcMode = false;  // 0.9.2：是否启用 ARC 插入
    std::string curFile;  // 当前正在生成的声明所在的源文件（供 #line 使用）
    // ARC：字符串字面量的静态不可变对象（refs = -1）
    std::map<std::string, std::string> litNames;
    std::vector<std::string> litDefs;
    size_t litAnchor = 0;  // 字面量包装区在 buf 中的位置

    const Ty* tInt = TyStore::int64Ty();
    const Ty* tFloat = TyStore::float64Ty();
    const Ty* tBool = TyStore::boolTy();
    const Ty* tString = TyStore::stringTy();
    const Ty* tVoid = TyStore::voidTy();

    explicit CGen(const CodegenOptions& o)
        : opt(o), arcMode(o.arc), curFile(o.sourceName) {
        buf.reserve(64 * 1024);  // 减少反复扩容（codegen 输出量远大于此）
    }

    // ---- 输出辅助 ----
    void raw(const std::string& s) { buf += s; }
    void line(const std::string& s) {
        buf.append(static_cast<size_t>(indent) * 4, ' ');
        buf += s;
        buf += '\n';
    }
    void blank() { buf += '\n'; }
    void emitLineMark(SourceLoc loc) {
        if (opt.emitLineMarks) {
            buf += "#line " + std::to_string(loc.line) + " \"" +
                   escapeCString(curFile) + "\"\n";
        }
    }
    std::string tmpName(const char* tag) {
        return std::string("lx_") + tag + std::to_string(++tmpId);
    }

    // ---- 表达式 ----

    // 把表达式 e（类型 from）按需转换成目标类型 to 的 C 表达式。
    // 0.8：处理 int→float 与 T→T?（装箱）。其余情况原样返回（Sema 已校验）。
    std::string coerceTo(Expr* e, const Ty* to) {
        const Ty* from = e ? e->ty : nullptr;
        std::string v = expr(e);
        if (!from || !to || from == to) return v;
        if (from->kind == TyKind::Invalid || to->kind == TyKind::Invalid)
            return v;
        if (from->kind == TyKind::Int && to->kind == TyKind::Float)
            return "((double)" + v + ")";
        if (to->kind == TyKind::Optional) {
            const Ty* elem = to->elem;
            std::string inner;
            if (from == elem) {
                inner = v;
            } else if (from->kind == TyKind::Int && elem &&
                       elem->kind == TyKind::Float) {
                inner = "((double)" + v + ")";
            } else {
                return v;  // Sema 已报错
            }
            std::string ct = cType(elem);
            std::string o = tmpName("box");
            // 0.9.4（P0-3）：装箱参与回收。载荷是引用类型时槽自带
            // on_zero = lx_gc_releasep（槽归零顺带释放内层），并在装箱时
            // retain —— 否则源变量的 cleanup 会先把对象释放，箱里成悬空。
            // 装箱出的槽是「新对象」，归调用方所有（owned）。
            bool refInner = isRefTy(elem);
            std::string put = inner;
            if (arcMode && refInner)
                put = "((" + ct + ")lx_gc_retain((void*)(" + inner + ")))";
            return "({ " + ct + "* " + o + " = (" + ct + "*)lx_opt_alloc(sizeof(" +
                   ct + "), " + (refInner ? "lx_gc_releasep" : "0") + "); *" + o +
                   " = " + put + "; " + o + "; })";
        }
        return v;
    }

    // ---- ARC 辅助（0.9.2）----
    // 引用类型（需要引用计数管理）：string / 数组 / struct
    static bool isRefTy(const Ty* t) {
        if (!t) return false;
        return t->kind == TyKind::String || t->kind == TyKind::Array ||
               t->kind == TyKind::Named;
    }

    // 字符串字面量 → 静态不可变对象（refs = -1），返回其 data 指针
    std::string internLit(const std::string& s) {
        auto it = litNames.find(s);
        if (it != litNames.end()) return it->second + ".data";
        std::string name = "lx_lit_" + std::to_string(litDefs.size());
        litNames[s] = name;
        litDefs.push_back("static struct { lx_gc_hdr h; char data[" +
                          std::to_string(s.size() + 1) + "]; } " + name +
                          " = { { -1, 0 }, \"" + escapeCString(s) + "\" };");
        return name + ".data";
    }

    // 表达式是否“产生一个新的引用归调用方所有”（可以转移，不必 retain）。
    // 保守原则：判不准就返回 false（当成借用 → 存储时 retain），
    // 宁可多一次 retain / 泄露临时值，也不可误判为 owned 导致双重释放。
    bool isOwned(Expr* e) {
        if (!arcMode || !e || !isRefTy(e->ty)) return false;
        switch (e->kind) {
            case ExprKind::Binary: {
                // 拼接产生新串；or 兜底（0.9.4）按下面的构造保证结果自持引用
                auto* b = static_cast<BinaryExpr*>(e);
                if (b->orFallback) return isRefTy(e->ty);
                return e->ty->kind == TyKind::String;
            }
            case ExprKind::Try:
                // ? 传播（0.9.4）：结果已按「自持引用」构造，见 consumeOpt
                return isRefTy(e->ty);
            case ExprKind::Index: {
                // s[i] 产生单字节字符（新字符串）
                auto* ix = static_cast<IndexExpr*>(e);
                return ix->base && ix->base->ty == tString;
            }
            case ExprKind::Slice:
            case ExprKind::ArrayLit:
            case ExprKind::StructLit:
                return true;
            case ExprKind::Call: {
                auto* c = static_cast<CallExpr*>(e);
                if (c->target && c->target->isExtern)
                    return false;  // extern 返回借用视图（const char*）
                if (c->target) return true;  // 用户函数返回转移所有权
                // 内建：只信任确实新建堆对象的那些
                switch (c->builtin) {
                    case Builtin::ToString:
                    case Builtin::Input:
                    case Builtin::StrReplace:
                    case Builtin::StrTrim:
                    case Builtin::StrUpper:
                    case Builtin::StrLower:
                    case Builtin::StrSubstr:
                    case Builtin::StrFormat:
                    case Builtin::StrSplit:
                    case Builtin::StrChars:
                    case Builtin::StrJoin:
                        return true;
                    case Builtin::ArrPop:
                    case Builtin::ArrRemove:
                        return e->ty && isRefTy(e->ty);
                    default:
                        return false;  // Env / FileRead(string?) 等
                }
            }
            default:
                return false;
        }
    }

    // 返回“原始 C 字符串”的表达式（extern 返回、env）：它们没有 ARC 头，
    // 存储时必须先拷成 Lux 自己的字符串（否则 retain/cleanup 会读到非法头）。
    bool isRawCString(Expr* e) {
        if (!arcMode || !e || !e->ty || e->ty->kind != TyKind::String)
            return false;
        if (e->kind != ExprKind::Call) return false;
        auto* c = static_cast<CallExpr*>(e);
        if (c->target && c->target->isExtern) return true;
        return !c->target && c->builtin == Builtin::Env;
    }

    // 可选值表达式是否「产生一个新槽归调用方所有」（0.9.4，P0-3）。
    // 保守原则：判不准就当借用（多 retain 一次只是短暂多占内存，
    // 误判成 owned 会导致槽被提前 drop 而载荷悬空）。
    bool isOwnedOpt(Expr* e) {
        if (!arcMode || !e || !e->ty || e->ty->kind != TyKind::Optional)
            return false;
        if (!e->ty->elem || !isRefTy(e->ty->elem)) {
            // 值类型载荷：槽本身仍然要回收，以下同样按「新槽」判断
        }
        switch (e->kind) {
            case ExprKind::Call: {
                auto* c = static_cast<CallExpr*>(e);
                if (c->target) return !c->target->isExtern;  // 用户函数返回新槽
                switch (c->builtin) {
                    case Builtin::ToInt:
                    case Builtin::ToFloat:
                    case Builtin::FileRead:
                    case Builtin::StrFindOpt: return true;
                    default: return false;
                }
            }
            case ExprKind::NoneLit:
                return true;  // none 是空指针，drop/retain 都是空操作
            default:
                return false;  // 变量 / 其他：借用
        }
    }

    // 存储语义：把 e 存进一个引用类型槽位。
    //   原始 C 串 → lx_str_dup 拷成有头对象
    //   owned     → 转移（不 retain，调用方把引用交给槽位）
    //   借用     → retain 后再存（槽位自己持有一份）
    std::string coerceStore(Expr* e, const Ty* to) {
        std::string v = coerceTo(e, to);
        if (!arcMode) return v;
        if (to->kind == TyKind::Optional) {
            // 0.9.4（P0-3）：可选槽位同样有所有权语义。
            //   T → T?  新装箱，归槽位所有（coerceTo 已 return 新槽）
            //   owned   转移
            //   借用    retain（槽位自己持有一份）
            const Ty* from = e ? e->ty : nullptr;
            if (!from || from->kind != TyKind::Optional) return v;
            if (isOwnedOpt(e)) return v;
            return "((" + cType(to) + ")lx_opt_retain((void*)(" + v + ")))";
        }
        if (!isRefTy(to)) return v;
        if (isRawCString(e))
            return "lx_str_dup((const char*)(" + v + "))";
        if (isOwned(e)) return v;
        return "((" + cType(to) + ")lx_gc_retain((void*)(" + v + ")))";
    }

    // 消费一个可选值表达式（0.9.4，P0-3）：读出 elem 类型的载荷，
    // 并保证结果「自持一份引用」（引用类型时：借用 → retain，owned → 取走
    // 所有权后 drop 掉空槽）。泛型化的三处消费者：
    //   or 兜底 / ? 传播 / name! 解包。
    // panicWhat 非空 → 为 none 时 panic；为空 → 为 none 时 return 当前返回类型的零值。
    std::string consumeOpt(Expr* optExpr, const Ty* optTy,
                           const char* panicWhat) {
        return consumeOptCode(expr(optExpr), optTy, isOwnedOpt(optExpr),
                              panicWhat ? std::string(panicWhat) : std::string());
    }

    // 同上，但直接吃一段已经生成好的 C 代码（用于内建 panic 变体）
    std::string consumeOptCode(const std::string& optCode, const Ty* optTy,
                               bool owned, const std::string& panicWhat) {
        const Ty* elem = optTy->elem;
        std::string ct = cType(optTy);
        std::string et = cType(elem);
        std::string o = tmpName("oc");
        std::string tail;
        std::string take = "(*" + o + ")";
        if (arcMode && isRefTy(elem) && !owned)
            take = "((void*)lx_gc_retain((void*)(*" + o + ")))";
        if (arcMode && owned) tail = " lx_opt_drop((void*)" + o + ");";
        std::string guard;
        if (!panicWhat.empty())
            guard = "if (!" + o + ") lx_panic_opt(\"" +
                    escapeCString(panicWhat) + "\"); ";
        else
            guard = "if (!" + o + ") return " + zeroOfA(curRetTy) + "; ";
        return "({ " + ct + " " + o + " = " + optCode + "; " + guard + et +
               " " + o + "_v = " + take + ";" + tail + " " + o + "_v; })";
    }

    // 引用类型的“零值”：ARC 下 string 的空串用静态字面量包装，
    // 否则清理时会去 free 裸 C 字面量。
    std::string zeroOfA(const Ty* t) {
        if (arcMode && t && t->kind == TyKind::String) return internLit("");
        return zeroOf(t);
    }

    std::string toStrOf(const Ty* t, const std::string& v) {
        if (!t) return v;
        switch (t->kind) {
            case TyKind::Int: return "lx_i64_to_str(" + v + ")";
            case TyKind::Float: return "lx_f64_to_str(" + v + ")";
            case TyKind::Bool: return "lx_bool_to_str(" + v + ")";
            case TyKind::String: return "(" + v + ")";
            case TyKind::Array: return "lx_arr_to_str(" + v + ")";
            case TyKind::Named:
                return structCName(t->name) + "_to_str(" + v + ")";
            case TyKind::Optional: {
                std::string o = tmpName("opts");
                std::string inner = toStrOf(t->elem, "(*" + o + ")");
                return "({ " + cType(t) + " " + o + " = " + v + "; " + o +
                       " ? lx_str_concat3(\"some(\", " + inner +
                       ", \")\") : lx_str_dup(\"none\"); })";
            }
            default: return v;
        }
    }

    // panic 变体（name!）解包：opt 是 T? 表达式；为 none 时 panic，否则给 T。
    // 0.9.4：解包出来的值是「自持引用」，且空槽被回收（见 consumeOptCode）。
    std::string panicUnwrap(const std::string& opt, const Ty* optTy,
                            const std::string& what) {
        return consumeOptCode(opt, optTy, /*owned=*/true, what);
    }

    std::string expr(Expr* e) {
        if (!e) return "0";
        switch (e->kind) {
            case ExprKind::IntLit: {
                auto* n = static_cast<IntLitExpr*>(e);
                return "((int64_t)" + std::to_string(n->value) + "LL)";
            }
            case ExprKind::BoolLit: {
                auto* n = static_cast<BoolLitExpr*>(e);
                return n->value ? "true" : "false";
            }
            case ExprKind::StrLit: {
                auto* n = static_cast<StrLitExpr*>(e);
                // ARC：字面量换成静态不可变对象（refs = -1），保证
                // retain/release 安全；非 ARC 保持裸 C 字面量
                if (arcMode) return internLit(n->value);
                return "\"" + escapeCString(n->value) + "\"";
            }
            case ExprKind::FloatLit: {
                auto* n = static_cast<FloatLitExpr*>(e);
                // nan / inf 没有 C 字面量，用 0.0/0.0 等表达式表示（0.7）
                if (std::isnan(n->value)) return "((double)(0.0/0.0))";
                if (std::isinf(n->value))
                    return n->value < 0 ? "((double)(-1.0/0.0))"
                                        : "((double)(1.0/0.0))";
                return "((double)" + formatDouble(n->value) + ")";
            }
            case ExprKind::Ident: {
                auto* n = static_cast<IdentExpr*>(e);
                // 引用全局常量（含模块限定访问）时用它的模块归属改写名字
                if (n->constRef) {
                    return mangleFor(n->constRef->module, n->constRef->name);
                }
                return mangle(n->name);
            }
            case ExprKind::Unary: {
                auto* n = static_cast<UnaryExpr*>(e);
                std::string op;
                switch (n->op) {
                    case UnOp::Neg: op = "-"; break;
                    case UnOp::LogicNot: op = "!"; break;
                    case UnOp::BitNot: op = "~"; break;
                }
                return "(" + op + "(" + expr(n->operand) + "))";
            }
            case ExprKind::Binary: {
                auto* n = static_cast<BinaryExpr*>(e);
                // 0.8：or 兜底（lhs 是 T?）
                if (n->orFallback) {
                    const Ty* elem = n->lhs->ty->elem;
                    std::string o = tmpName("or");
                    if (!arcMode) {
                        return "({ " + cType(n->lhs->ty) + " " + o + " = " +
                               expr(n->lhs) + "; " + o + " ? (*" + o + ") : (" +
                               coerceTo(n->rhs, elem) + "); })";
                    }
                    // 0.9.4（P0-3）：两条分支都构造成「自持一份引用」——
                    //   命中：借用盒先 retain 内层，owned 盒取走所有权后 drop 槽；
                    //   兜底：走存储语义（owned 转移 / 借用 retain）。
                    // 因此 or 的结果对引用类型恒为 owned（见 isOwned）。
                    bool refE = isRefTy(elem);
                    std::string vslot = o + "_v";
                    std::string hit = "*" + o;
                    if (refE && !isOwnedOpt(n->lhs))
                        hit = "((void*)lx_gc_retain((void*)(*" + o + ")))";
                    std::string hitTail;
                    if (isOwnedOpt(n->lhs))
                        hitTail = " lx_opt_drop((void*)" + o + ");";
                    std::string miss = refE ? coerceStore(n->rhs, elem)
                                            : coerceTo(n->rhs, elem);
                    return "({ " + cType(n->lhs->ty) + " " + o + " = " +
                           expr(n->lhs) + "; " + cType(elem) + " " + vslot +
                           "; if (" + o + ") { " + vslot + " = " + hit + ";" +
                           hitTail + " } else { " + vslot + " = " + miss +
                           "; } " + vslot + "; })";
                }
                // Sema 折叠过的常量表达式直接落成字面量
                if (n->folded) {
                    if (n->foldedIsStr) {
                        // 0.9.3（A1）：ARC 下折叠出的字符串常量必须走字面量
                        // 包装（refs = -1），否则发裸 C 字面量会带着「无头指针」
                        // 进入引用计数世界，retain/release 读到非法头而崩溃。
                        if (arcMode) return internLit(n->foldedS);
                        return "\"" + escapeCString(n->foldedS) + "\"";
                    }
                    if (n->ty == tFloat) return formatDouble(n->foldedF);
                    return std::to_string(n->foldedI) + "LL";
                }
                return binary(n);
            }
            case ExprKind::Call: {
                auto* n = static_cast<CallExpr*>(e);
                return callExpr(n);
            }
            case ExprKind::Index: {
                // a[i]（数组元素）或 s[i]（单字节字符）
                // 注意：必须按 base 的类型区分——string[] 的元素也是 string，
                // 不能用结果类型判断
                auto* n = static_cast<IndexExpr*>(e);
                const Ty* bt = n->base ? n->base->ty : nullptr;
                if (bt == tString) {
                    return "lx_str_char_at(" + expr(n->base) + ", " +
                           expr(n->index) + ")";
                }
                const Ty* et = n->ty ? n->ty : TyStore::int64Ty();
                return "lx_arr_get_" + arrRT(et) + "(" + expr(n->base) + ", " +
                       expr(n->index) + ")";
            }
            case ExprKind::Slice: {
                // a[lo..hi] / a[lo..=hi]（0.6 新增）：复制语义切片。
                // 用语句表达式保证 base 只求值一次；end 缺省 = 长度。
                auto* n = static_cast<SliceExpr*>(e);
                const Ty* bt = n->base ? n->base->ty : nullptr;
                bool isStr = (bt == tString);
                std::string v = tmpName("sl");
                std::string s = std::string("({ ") +
                                (isStr ? "const char* " : "lx_arr ") + v +
                                " = " + expr(n->base) + ";";
                s += " int64_t lo = " +
                     (n->start ? expr(n->start) : std::string("0")) + ";";
                std::string hiE;
                if (!n->end) {
                    hiE = isStr ? "lx_str_len(" + v + ")"
                                : "((" + v + ")->len)";
                } else {
                    hiE = expr(n->end);
                    if (n->inclusive) hiE = "((" + hiE + ") + 1)";
                }
                s += " int64_t hi = " + hiE + ";";
                s += isStr ? " lx_str_slice(" : " lx_arr_slice(";
                s += v + ", lo, hi); })";
                return s;
            }
            case ExprKind::ArrayLit: {
                // [e1, e2, ...] → 堆分配头 + 逐元素 push（语句表达式，可安全返回）
                auto* n = static_cast<ArrayLitExpr*>(e);
                const Ty* et = n->elemTy;
                std::string v = tmpName("arr");
                std::string s = "({ lx_arr " + v + " = lx_arr_new(" +
                                arrEsz(et) + ", " + arrPelem(et) + ", " +
                                arrRelem(et) + ");";
                for (Expr* el : n->elems) {
                    std::string ev = coerceStore(el, et);
                    s += " lx_arr_push_" + arrRT(et) + "(" + v + ", " + ev +
                         ");";
                }
                s += " " + v + "; })";
                return s;
            }
            case ExprKind::StructLit: {
                // Point { x: 1, y: 2 } → ({ Point* p = lx_struct_new(...);
                //                           p->f_x = ...; ...; p; })
                auto* n = static_cast<StructLitExpr*>(e);
                std::string cn = structCName(n->typeName);
                std::string v = tmpName("st");
                std::string s = "({ " + cn + "* " + v + " = (" + cn +
                                "*)lx_struct_new(sizeof(" + cn + "), " + cn +
                                "_on_zero);";
                StructDecl* sd = n->decl;
                for (auto& kv : n->inits) {
                    int idx = sd ? structFieldIndex(sd, kv.first) : -1;
                    if (idx < 0) continue;
                    const Ty* ft = sd->fields[idx].ty;
                    std::string fv = coerceStore(kv.second, ft);
                    s += " " + v + "->f_" + kv.first + " = " + fv + ";";
                }
                s += " " + v + "; })";
                return s;
            }
            case ExprKind::Member: {
                auto* n = static_cast<MemberExpr*>(e);
                // 模块限定常量（math.pi）
                if (n->constRef) {
                    return mangleFor(n->constRef->module, n->constRef->name);
                }
                return "((" + cType(n->base->ty) + ")" + expr(n->base) +
                       ")->f_" + n->member;
            }
            case ExprKind::If: {
                auto* n = static_cast<IfExpr*>(e);
                std::string a = coerceTo(n->thenVal, n->ty);
                std::string b = coerceTo(n->elseVal, n->ty);
                return "(" + expr(n->cond) + " ? " + a + " : " + b + ")";
            }
            case ExprKind::NoneLit: {
                // none：类型由 Sema 确定（T?），C 层就是空指针。
                // 防御：万一 Sema 未设类型，不能拿空指针去 cType 崩溃。
                if (!e->ty || e->ty->kind == TyKind::Invalid) return "((void*)0)";
                return "((" + cType(e->ty) + ")0)";
            }
            case ExprKind::Try: {
                // expr?（0.8）：失败就 return none（当前函数必须返回 T?）
                auto* tr = static_cast<TryExpr*>(e);
                const Ty* ot = tr->operand->ty;
                if (!arcMode) {
                    std::string o = tmpName("try");
                    return "({ " + cType(ot) + " " + o + " = " +
                           expr(tr->operand) + "; if (!" + o + ") return " +
                           zeroOfA(curRetTy) + "; (*" + o + "); })";
                }
                // 0.9.4（P0-3）：结果自持引用（借用先 retain），
                // 且临时槽被回收。
                return consumeOpt(tr->operand, ot, nullptr);
            }
        }
        return "0";
    }

    // 需要把操作数提升到 float 时加显式转换
    std::string coerceToFloat(Expr* e) {
        std::string s = expr(e);
        if (e->ty == tInt) return "((double)" + s + ")";
        return s;
    }

    std::string binary(BinaryExpr* n) {
        const Ty* lt = n->lhs->ty;
        const Ty* rt = n->rhs->ty;

        // 字符串运算走运行时函数
        if (lt == tString && rt == tString) {
            switch (n->op) {
                case BinOp::Add:
                    // ARC：拼接产生新串；若某一侧是 owned 临时值，
                    // 用完即 release（解决 a+b+c 的中间串泄露）。
                    if (!arcMode)
                        return "lx_str_concat(" + expr(n->lhs) + ", " +
                               expr(n->rhs) + ")";
                    {
                        std::string L = tmpName("cl");
                        std::string R = tmpName("cr");
                        std::string O = tmpName("cc");
                        bool lOwn = isOwned(n->lhs), rOwn = isOwned(n->rhs);
                        std::string s = "({ const char* " + L + " = " +
                                        expr(n->lhs) + "; const char* " + R +
                                        " = " + expr(n->rhs) +
                                        "; const char* " + O +
                                        " = lx_str_concat(" + L + ", " + R +
                                        ");";
                        if (rOwn) s += " lx_gc_release((void*)" + R + ");";
                        if (lOwn) s += " lx_gc_release((void*)" + L + ");";
                        s += " " + O + "; })";
                        return s;
                    }
                case BinOp::Eq:
                    return "lx_str_eq(" + expr(n->lhs) + ", " + expr(n->rhs) + ")";
                case BinOp::Ne:
                    return "(!lx_str_eq(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           "))";
                case BinOp::Lt:
                    return "(lx_str_cmp(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           ") < 0)";
                case BinOp::Le:
                    return "(lx_str_cmp(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           ") <= 0)";
                case BinOp::Gt:
                    return "(lx_str_cmp(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           ") > 0)";
                case BinOp::Ge:
                    return "(lx_str_cmp(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           ") >= 0)";
                default:
                    return "0";
            }
        }

        bool useFloat = (lt == tFloat || rt == tFloat);

        switch (n->op) {
            case BinOp::Add:
            case BinOp::Sub:
            case BinOp::Mul: {
                std::string op = n->op == BinOp::Add
                                     ? "+"
                                     : (n->op == BinOp::Sub ? "-" : "*");
                if (useFloat) {
                    return "(" + coerceToFloat(n->lhs) + " " + op + " " +
                           coerceToFloat(n->rhs) + ")";
                }
                return "(" + expr(n->lhs) + " " + op + " " + expr(n->rhs) + ")";
            }
            case BinOp::Div: {
                if (useFloat) {
                    return "(" + coerceToFloat(n->lhs) + " / " +
                           coerceToFloat(n->rhs) + ")";
                }
                return "lx_idiv(" + expr(n->lhs) + ", " + expr(n->rhs) + ")";
            }
            case BinOp::Mod:
                return "lx_imod(" + expr(n->lhs) + ", " + expr(n->rhs) + ")";
            case BinOp::Eq:
            case BinOp::Ne: {
                std::string op = (n->op == BinOp::Eq) ? "==" : "!=";
                if (useFloat) {
                    return "(" + coerceToFloat(n->lhs) + " " + op + " " +
                           coerceToFloat(n->rhs) + ")";
                }
                return "(" + expr(n->lhs) + " " + op + " " + expr(n->rhs) + ")";
            }
            case BinOp::Lt:
            case BinOp::Le:
            case BinOp::Gt:
            case BinOp::Ge: {
                std::string op;
                switch (n->op) {
                    case BinOp::Lt: op = "<"; break;
                    case BinOp::Le: op = "<="; break;
                    case BinOp::Gt: op = ">"; break;
                    default: op = ">="; break;
                }
                if (useFloat) {
                    return "(" + coerceToFloat(n->lhs) + " " + op + " " +
                           coerceToFloat(n->rhs) + ")";
                }
                return "(" + expr(n->lhs) + " " + op + " " + expr(n->rhs) + ")";
            }
            case BinOp::LogicAnd:
                return "(" + expr(n->lhs) + " && " + expr(n->rhs) + ")";
            case BinOp::LogicOr:
                return "(" + expr(n->lhs) + " || " + expr(n->rhs) + ")";
            case BinOp::BitAnd:
                return "(" + expr(n->lhs) + " & " + expr(n->rhs) + ")";
            case BinOp::BitOr:
                return "(" + expr(n->lhs) + " | " + expr(n->rhs) + ")";
            case BinOp::BitXor:
                return "(" + expr(n->lhs) + " ^ " + expr(n->rhs) + ")";
            case BinOp::Shl:
                return "((int64_t)(((uint64_t)" + expr(n->lhs) + ") << " +
                       expr(n->rhs) + "))";
            case BinOp::Shr:
                // 算术右移（有符号），与原生后端的 sar 语义一致（C1，0.8）
                return "((int64_t)(" + expr(n->lhs) + ") >> " +
                       expr(n->rhs) + ")";
        }
        return "0";
    }

    std::string callExpr(CallExpr* c) {
        switch (c->builtin) {
            case Builtin::Print:
            case Builtin::PrintLn:
                // print 是多语句宏，只能出现在语句位置（Sema 已保证）
                return "((void)0)";
            case Builtin::Len: {
                Expr* a = c->args[0];
                if (a->ty && a->ty->kind == TyKind::Array) {
                    return "lx_arr_len(" + expr(a) + ")";
                }
                return "lx_str_len(" + expr(a) + ")";
            }
            case Builtin::Input:
                if (c->args.empty()) return "lx_read_line()";
                return "lx_read_line_prompt(" + expr(c->args[0]) + ")";
            case Builtin::ToInt: {
                if (c->args.empty()) return "((int64_t)0)";
                Expr* a = c->args[0];
                switch (a->ty->kind) {
                    case TyKind::Float:
                        return "((int64_t)(" + expr(a) + "))";
                    case TyKind::Bool:
                        return "((int64_t)(" + expr(a) + " ? 1 : 0))";
                    case TyKind::String:
                        if (c->panicVariant)
                            return panicUnwrap(
                                "lx_str_to_i64_opt(" + expr(a) + ")",
                                TyStore::optionalOf(tInt), "int 解析");
                        return "lx_str_to_i64_opt(" + expr(a) + ")";
                    default:
                        return "((int64_t)(" + expr(a) + "))";
                }
            }
            case Builtin::ToFloat: {
                if (c->args.empty()) return "((double)0.0)";
                Expr* a = c->args[0];
                switch (a->ty->kind) {
                    case TyKind::String:
                        if (c->panicVariant)
                            return panicUnwrap(
                                "lx_str_to_f64_opt(" + expr(a) + ")",
                                TyStore::optionalOf(tFloat), "float 解析");
                        return "lx_str_to_f64_opt(" + expr(a) + ")";
                    case TyKind::Bool:
                        return "((double)(" + expr(a) + " ? 1.0 : 0.0))";
                    default:
                        return "((double)(" + expr(a) + "))";
                }
            }
            case Builtin::ToString: {
                if (c->args.empty()) return "\"\"";
                return toStrOf(c->args[0]->ty, expr(c->args[0]));
            }
            case Builtin::Assert: {
                std::string cond = expr(c->args[0]);
                std::string msg = c->args.size() > 1
                                      ? expr(c->args[1])
                                      : "\"" + escapeCString("assert(" +
                                                             std::string("条件不成立") + ")") +
                                            "\"";
                return "((void)lx_assert_at(" + cond + ", " + msg + ", " +
                       std::to_string(c->loc.line) + "))";
            }
            case Builtin::Exit:
                return "((void)exit((int)(" + expr(c->args[0]) + ")))";
            case Builtin::Abs: {
                Expr* a = c->args[0];
                if (a->ty == tFloat) return "fabs(" + expr(a) + ")";
                return "llabs(" + expr(a) + ")";
            }
            case Builtin::Sqrt:
                return "sqrt((double)(" + expr(c->args[0]) + "))";
            case Builtin::Pow:
                return "pow((double)(" + expr(c->args[0]) + "), (double)(" +
                       expr(c->args[1]) + "))";
            case Builtin::Floor:
                return "floor((double)(" + expr(c->args[0]) + "))";
            case Builtin::Ceil:
                return "ceil((double)(" + expr(c->args[0]) + "))";
            case Builtin::Round:
                return "round((double)(" + expr(c->args[0]) + "))";
            case Builtin::Sin:
                return "sin((double)(" + expr(c->args[0]) + "))";
            case Builtin::Cos:
                return "cos((double)(" + expr(c->args[0]) + "))";
            case Builtin::Tan:
                return "tan((double)(" + expr(c->args[0]) + "))";
            case Builtin::Asin:
                return "asin((double)(" + expr(c->args[0]) + "))";
            case Builtin::Acos:
                return "acos((double)(" + expr(c->args[0]) + "))";
            case Builtin::Atan:
                return "atan((double)(" + expr(c->args[0]) + "))";
            case Builtin::Log:
                return "log((double)(" + expr(c->args[0]) + "))";
            case Builtin::Log10:
                return "log10((double)(" + expr(c->args[0]) + "))";
            case Builtin::Exp:
                return "exp((double)(" + expr(c->args[0]) + "))";
            case Builtin::Atan2:
                return "atan2((double)(" + expr(c->args[0]) + "), (double)(" +
                       expr(c->args[1]) + "))";
            case Builtin::Fmod:
                return "fmod((double)(" + expr(c->args[0]) + "), (double)(" +
                       expr(c->args[1]) + "))";
            case Builtin::Hypot:
                return "hypot((double)(" + expr(c->args[0]) + "), (double)(" +
                       expr(c->args[1]) + "))";
            case Builtin::Trunc:
                return "trunc((double)(" + expr(c->args[0]) + "))";
            case Builtin::IsNan:
                return "(isnan((double)(" + expr(c->args[0]) + ")) != 0)";
            case Builtin::IsInf:
                return "(isinf((double)(" + expr(c->args[0]) + ")) != 0)";
            case Builtin::Random:
                return "lx_random()";
            case Builtin::Seed:
                return "((void)lx_seed((unsigned int)(" + expr(c->args[0]) + ")))";
            case Builtin::Min:
            case Builtin::Max: {
                Expr* a = c->args[0];
                Expr* b = c->args[1];
                if (c->ty == tFloat) {
                    const char* fn = c->builtin == Builtin::Min ? "fmin" : "fmax";
                    std::string av = expr(a);
                    std::string bv = expr(b);
                    if (a->ty == tInt) av = "((double)" + av + ")";
                    if (b->ty == tInt) bv = "((double)" + bv + ")";
                    return std::string(fn) + "(" + av + ", " + bv + ")";
                }
                const char* op = c->builtin == Builtin::Min ? "<" : ">";
                return "((" + expr(a) + ") " + op + " (" + expr(b) + ") ? (" +
                       expr(a) + ") : (" + expr(b) + "))";
            }
            case Builtin::TimeNow:
                return "lx_time_now()";
            case Builtin::TimeMono:
                return "lx_time_mono()";
            case Builtin::Sleep:
                return "((void)lx_sleep((double)(" + expr(c->args[0]) + ")))";
            case Builtin::SleepMs:
                return "((void)lx_sleep_ms(" + expr(c->args[0]) + "))";
            case Builtin::SysCall:
                return "lx_system(" + expr(c->args[0]) + ")";
            case Builtin::Env:
                return "lx_getenv(" + expr(c->args[0]) + ")";
            case Builtin::Setenv:
                return "lx_setenv(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::FileRead:
                if (c->panicVariant) {
                    // read! 专用：把路径一并带进 panic（0.9，恢复 0.7 的 UX）
                    std::string pv = tmpName("path");
                    std::string o = tmpName("pu");
                    return "({ const char* " + pv + " = " +
                           expr(c->args[0]) + "; " + cType(TyStore::optionalOf(tString)) +
                           " " + o + " = lx_read_opt(" + pv + "); if (!" + o +
                           ") lx_panic_opt_file(\"文件读取\", " + pv + "); (*" + o +
                           "); })";
                }
                return "lx_read_opt(" + expr(c->args[0]) + ")";
            case Builtin::FileExists:
                return "lx_file_exists(" + expr(c->args[0]) + ")";
            case Builtin::FileWrite:
                return "lx_file_write(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::FileAppend:
                return "lx_file_append(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::FileRemove:
                return "lx_file_remove(" + expr(c->args[0]) + ")";
            case Builtin::FileRename:
                return "lx_file_rename(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrContains:
                return "lx_str_contains(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrStartsWith:
                return "lx_str_startswith(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrEndsWith:
                return "lx_str_endswith(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrFind:
                return "lx_str_find(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrFindOpt:
                // 0.9.4（P1-8）：找不到返回 none；同时写 last_error()
                return "lx_str_find_opt(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::LastError:
                return "lx_last_error()";
            case Builtin::StrReplace:
                return "lx_str_replace(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ", " + expr(c->args[2]) + ")";
            case Builtin::StrTrim:
                return "lx_str_trim(" + expr(c->args[0]) + ")";
            case Builtin::StrUpper:
                return "lx_str_upper(" + expr(c->args[0]) + ")";
            case Builtin::StrLower:
                return "lx_str_lower(" + expr(c->args[0]) + ")";
            case Builtin::StrSubstr:
                return "lx_str_substr(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ", " + expr(c->args[2]) + ")";
            case Builtin::StrFormat: {
                // format(模板, 参数...) → lx_str_format_concat(模板,
                // 各参数转字符串..., NULL)
                std::string s = "lx_str_format_concat(" + expr(c->args[0]);
                for (size_t i = 1; i < c->args.size(); i++) {
                    s += ", ";
                    Expr* a = c->args[i];
                    s += toStrOf(a->ty, expr(a));
                }
                s += ", (const char*)0)";
                return s;
            }
            // ---- string 模块数组扩展（0.5） ----
            case Builtin::StrSplit:
                return "lx_str_split(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            case Builtin::StrChars:
                return "lx_str_chars(" + expr(c->args[0]) + ")";
            case Builtin::StrJoin:
                return "lx_str_join(" + expr(c->args[0]) + ", " +
                       expr(c->args[1]) + ")";
            // ---- 数组方法（0.5）：接收者一定是可寻址的局部变量（Sema 保证）；
            //      lx_arr 本身就是堆头指针，直接传值即共享 ----
            case Builtin::ArrPush: {
                std::string recv = mangle(c->methodRecv);
                std::string arg = coerceStore(c->args[0], c->methodElem);
                return "((void)lx_arr_push_" + arrRT(c->methodElem) + "(" +
                       recv + ", " + arg + "))";
            }
            case Builtin::ArrPop:
                return "lx_arr_pop_" + arrRT(c->methodElem) + "(" +
                       mangle(c->methodRecv) + ")";
            case Builtin::ArrInsert: {
                std::string arg = coerceStore(c->args[1], c->methodElem);
                return "((void)lx_arr_insert_" + arrRT(c->methodElem) + "(" +
                       mangle(c->methodRecv) + ", " + expr(c->args[0]) + ", " +
                       arg + "))";
            }
            case Builtin::ArrRemove:
                return "lx_arr_remove_" + arrRT(c->methodElem) + "(" +
                       mangle(c->methodRecv) + ", " + expr(c->args[0]) + ")";
            case Builtin::ArrClear:
                return "((void)lx_arr_clear(" + mangle(c->methodRecv) + "))";
            default:
                // __ 系内建（intrinsic）只由原生后端内联实现：
                // 直接在生成的 C 流里插一行 #error，让 gcc/clang
                // 预处理阶段立即报错并指出原因。
                if ((int)c->builtin >= (int)Builtin::IntrSyscall) {
                    raw("#error Lux: __ 系内建仅原生后端支持 (" +
                        c->callee + ")\n");
                    return "0";
                }
                break;
        }

        // 用户函数调用（extern fn 直接引用 C 函数名，不做名字改写）
        std::string calleeName = (c->target && c->target->isExtern)
                                     ? c->callee
                                     : (c->target
                                            ? mangleFor(c->target->module,
                                                        c->target->name)
                                            : mangle(c->callee));
        std::string s = calleeName + "(";
        for (size_t i = 0; i < c->args.size(); i++) {
            if (i) s += ", ";
            Expr* a = c->args[i];
            std::string av = expr(a);
            // 实参到形参的隐式转换（int → float / T → T? 装箱）
            if (c->target && i < c->target->params.size()) {
                av = coerceTo(a, c->target->params[i].ty);
            }
            s += av;
        }
        s += ")";
        // 0.8：用户函数的 panic 变体 f!(...)（f 返回 T?，失败即退出）
        if (c->panicVariant && c->target && c->target->retTy &&
            c->target->retTy->kind == TyKind::Optional) {
            return panicUnwrap(s, c->target->retTy, c->target->name);
        }
        return s;
    }

    // 复合赋值右值：字符串用 concat；int 除法 / 取模走带零检查的运行时（C1，0.8）
    std::string compoundRhs(BinOp op, const std::string& lhs,
                            const std::string& rhs, const Ty* tt) {
        if (tt == tString && op == BinOp::Add)
            return "lx_str_concat(" + lhs + ", " + rhs + ")";
        if (tt == tInt && op == BinOp::Div)
            return "lx_idiv(" + lhs + ", " + rhs + ")";
        if (tt == tInt && op == BinOp::Mod)
            return "lx_imod(" + lhs + ", " + rhs + ")";
        return "((" + lhs + ") " + binOpText(op) + " " + rhs + ")";
    }

    // ---- 语句 ----
    // 左值赋值（0.7）：x / p.x / a[i] / a[i][j] / a[i].x 统一处理
    void assignStmt(AssignStmt* a) {
        Expr* t = a->target;
        const Ty* tt = a->elemTy ? a->elemTy : TyStore::int64Ty();
        bool refManaged = arcMode && isRefTy(tt);
        // 0.9.4（P0-3）：T? 变量赋值同样要先释放旧槽再存新槽
        bool optManaged =
            arcMode && tt && tt->kind == TyKind::Optional;
        bool managed = refManaged || optManaged;
        if (t->kind == ExprKind::Index) {
            // 数组元素：用临时变量保证 base / index 只求值一次
            std::string base = expr(static_cast<IndexExpr*>(t)->base);
            std::string idx = expr(static_cast<IndexExpr*>(t)->index);
            std::string bv = tmpName("b"), iv = tmpName("i");
            line("lx_arr " + bv + " = " + base + ";");
            line("int64_t " + iv + " = " + idx + ";");
            std::string elem =
                "lx_arr_get_" + arrRT(tt) + "(" + bv + ", " + iv + ")";
            std::string rhs;
            if (a->compound) {
                std::string v = coerceTo(a->value, tt);
                rhs = compoundRhs(a->compoundOp, elem, v, tt);
            } else {
                rhs = coerceStore(a->value, tt);
            }
            // set 会先释放旧元素（ARC），再存入新值（已 retain / 转移）
            line("lx_arr_set_" + arrRT(tt) + "(" + bv + ", " + iv + ", " +
                 rhs + ");");
            return;
        }
        // Ident / Member：Member 先把基址求值到临时指针（避免重复求值）
        std::string lv;
        if (t->kind == ExprKind::Member) {
            auto* m = static_cast<MemberExpr*>(t);
            std::string bt = tmpName("mb");
            line(std::string(cType(m->base->ty)) + " " + bt + " = " +
                 expr(m->base) + ";");
            lv = bt + "->f_" + m->member;
        } else {
            lv = lvalueOf(t);
        }
        if (managed) {
            std::string rhs;
            if (a->compound) {
                std::string v = coerceTo(a->value, tt);
                rhs = compoundRhs(a->compoundOp, lv, v, tt);  // 新串（owned）
            } else {
                rhs = coerceStore(a->value, tt);  // owned 转移 / 借用 retain
            }
            std::string tv = tmpName("as");
            // 先算新值再释放旧值，兼容 x = x + ... / x += ...
            line(std::string(cType(tt)) + " " + tv + " = " + rhs + ";");
            line(std::string(refManaged ? "lx_gc_release" : "lx_opt_release") +
                 "((void*)" + lv + ");");
            line(lv + " = " + tv + ";");
            return;
        }
        std::string v = coerceTo(a->value, tt);
        if (a->compound) {
            if (tt == tString && a->compoundOp == BinOp::Add) {
                line(lv + " = lx_str_concat(" + lv + ", " + v + ");");
            } else if (tt == tInt && a->compoundOp == BinOp::Div) {
                // 0.8（C1）：不能直接 /= （除零 SIGFPE + INT64_MIN/-1 溢出）
                line(lv + " = lx_idiv(" + lv + ", " + v + ");");
            } else if (tt == tInt && a->compoundOp == BinOp::Mod) {
                line(lv + " = lx_imod(" + lv + ", " + v + ");");
            } else {
                line(lv + " " + binOpText(a->compoundOp) + "= " + v + ";");
            }
        } else {
            line(lv + " = " + v + ";");
        }
    }

    // 生成 C 左值表达式（Ident / Member 链）
    std::string lvalueOf(Expr* t) {
        switch (t->kind) {
            case ExprKind::Ident:
                return mangle(static_cast<IdentExpr*>(t)->name);
            case ExprKind::Member: {
                auto* m = static_cast<MemberExpr*>(t);
                return "((" + cType(m->base->ty) + ")" + expr(m->base) +
                       ")->f_" + m->member;
            }
            default:
                return "0";
        }
    }

    void stmt(Stmt* s) {
        if (!s) return;
        emitLineMark(s->loc);
        switch (s->kind) {
            case StmtKind::Block: {
                auto* b = static_cast<BlockStmt*>(s);
                line("{");
                indent++;
                for (Stmt* st : b->stmts) stmt(st);
                indent--;
                line("}");
                break;
            }
            case StmtKind::Let: {
                auto* let = static_cast<LetStmt*>(s);
                bool refManaged = arcMode && isRefTy(let->resolved);
                // 0.9.4（P0-3）：T? 局部变量同样在作用域出口回收空槽
                bool optManaged =
                    arcMode && let->resolved &&
                    let->resolved->kind == TyKind::Optional;
                bool managed = refManaged || optManaged;
                std::string decl;
                if (let->isConst && !managed) decl += "const ";
                decl += std::string(cType(let->resolved)) + " " +
                        mangle(let->name);
                if (managed)
                    decl += std::string(" __attribute__((cleanup(") +
                            (refManaged ? "lx_gc_releasep" : "lx_opt_releasep") +
                            ")))";
                if (let->init) {
                    decl += " = " + (managed
                                         ? coerceStore(let->init, let->resolved)
                                         : coerceTo(let->init, let->resolved));
                } else {
                    decl += " = " + zeroOfA(let->resolved);
                }
                line(decl + ";");
                break;
            }
            case StmtKind::Assign: {
                assignStmt(static_cast<AssignStmt*>(s));
                break;
            }
            case StmtKind::Expr: {
                auto* es = static_cast<ExprStmt*>(s);
                Expr* e = es->expr;
                // print / println 展开成若干条输出语句
                if (e && e->kind == ExprKind::Call) {
                    auto* c = static_cast<CallExpr*>(e);
                    if (c->builtin == Builtin::Print ||
                        c->builtin == Builtin::PrintLn) {
                        for (Expr* a : c->args) {
                            // 统一走 toString：可选值输出 some(x) / none（0.8）
                            std::string s = toStrOf(a->ty, expr(a));
                            // 0.9.3（A2-8）：toStrOf 对非 string 类型会新建堆串
                            // （int/float/bool/数组/struct/可选值），打印完必须
                            // 释放，否则这是最热的泄漏路径。string 类型本身
                            // 走的是原串（借用），只有 isOwned 的临时串才释放。
                            bool owns = a->ty && a->ty->kind != TyKind::String &&
                                        a->ty->kind != TyKind::Bool;
                            if (arcMode && a->ty &&
                                a->ty->kind == TyKind::String && isOwned(a))
                                owns = true;
                            if (arcMode && owns) {
                                std::string tv = tmpName("pr");
                                line("const char* " + tv + " = " + s + ";");
                                line("lx_print_str(" + tv + ");");
                                line("lx_gc_release((void*)" + tv + ");");
                            } else {
                                line("lx_print_str(" + s + ");");
                            }
                        }
                        if (c->builtin == Builtin::PrintLn) line("lx_print_nl();");
                        break;
                    }
                }
                // 0.9.4（P0-3）：可选值表达式语句（f(...); 放弃返回值）——
                // 若是自己拥有的临时槽，顺手回收，否则整个槽连同载荷一起漏掉。
                if (arcMode && e->ty && e->ty->kind == TyKind::Optional &&
                    isOwnedOpt(e)) {
                    line("lx_opt_drop((void*)(" + expr(e) + "));");
                    break;
                }
                line(expr(e) + ";");
                break;
            }
            case StmtKind::If: {
                auto* i = static_cast<IfStmt*>(s);
                line("if (" + expr(i->cond) + ")");
                stmt(i->thenBranch);
                if (i->elseBranch) {
                    line("else");
                    stmt(i->elseBranch);  // 可能是 Block 也可能是 else-if 链
                }
                break;
            }
            case StmtKind::While: {
                auto* w = static_cast<WhileStmt*>(s);
                line("while (" + expr(w->cond) + ")");
                stmt(w->body);
                break;
            }
            case StmtKind::Loop: {
                auto* l = static_cast<LoopStmt*>(s);
                line("for (;;)");
                stmt(l->body);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<ForStmt*>(s);
                if (f->mode == ForMode::Range) {
                    // 终点只求值一次，存进临时变量，避免重复计算
                    std::string endVar = tmpName("end");
                    std::string iv = mangle(f->var);
                    std::string cmp = f->inclusive ? " <= " : " < ";
                    line("for (int64_t " + iv + " = " + expr(f->start) + ", " +
                         endVar + " = " + expr(f->end) + "; " + iv + cmp +
                         endVar + "; " + iv + " += 1)");
                    stmt(f->body);
                    break;
                }
                // ---- for-in（0.5）：迭代数组或字符串 ----
                std::string itVar = tmpName("iter");
                std::string ixVar = tmpName("ix");
                line("{");
                indent++;
                if (f->iterTy == tString) {
                    // for c in s：逐字节产出单字符 string
                    line("const char* " + itVar +
                         (arcMode ? " __attribute__((cleanup(lx_gc_releasep)))"
                                  : "") +
                         " = " +
                         (arcMode ? coerceStore(f->iterable, f->iterable->ty)
                                  : expr(f->iterable)) +
                         ";");
                    line("for (int64_t " + ixVar + " = 0; " + itVar + "[" +
                         ixVar + "] != 0; " + ixVar + " += 1) {");
                    indent++;
                    line("const char* " + mangle(f->var) +
                         (arcMode ? " __attribute__((cleanup(lx_gc_releasep)))"
                                  : "") +
                         " = lx_str_char_at(" + itVar + ", " + ixVar + ");");
                    stmt(f->body);
                    indent--;
                    line("}");
                } else {
                    const Ty* et = f->varTy ? f->varTy : TyStore::int64Ty();
                    line("lx_arr " + itVar +
                         (arcMode ? " __attribute__((cleanup(lx_gc_releasep)))"
                                  : "") +
                         " = " +
                         (arcMode ? coerceStore(f->iterable, f->iterable->ty)
                                  : expr(f->iterable)) +
                         ";");
                    // len 快照：迭代中 push/pop 不影响本次遍历（避免无限循环）
                    std::string nVar = tmpName("n");
                    line("int64_t " + nVar + " = " + itVar + "->len;");
                    line("for (int64_t " + ixVar + " = 0; " + ixVar + " < " +
                         nVar + "; " + ixVar + " += 1) {");
                    indent++;
                    line(std::string(cType(et)) + " " + mangle(f->var) +
                         " = lx_arr_get_" + arrRT(et) + "(" + itVar + ", " +
                         ixVar + ");");
                    stmt(f->body);
                    indent--;
                    line("}");
                }
                indent--;
                line("}");
                break;
            }
            case StmtKind::Break:
                line("break;");
                break;
            case StmtKind::Continue:
                line("continue;");
                break;
            case StmtKind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                if (!r->value) {
                    // 0.8：返回类型是 T? 时，'return;' 返回 none
                    if (curRetTy->kind == TyKind::Optional)
                        line("return " + zeroOfA(curRetTy) + ";");
                    else
                        line("return;");
                } else {
                    line("return " + coerceStore(r->value, curRetTy) + ";");
                }
                break;
            }
        }
    }

    const Ty* curRetTy = TyStore::voidTy();

    // 生成函数体：{ 语句... 兜底 return }
    // forceDefaultReturn 为非空时用它做返回值（C 的 main 必须返回 int，
    // 即使 Lux 的 main 声明为 void）；否则非 void 函数用该类型零值。
    // 0.5.1：兜底 return 无条件生成。返回路径分析（stmtAlwaysReturns）只
    // 用于 Sema 的 W1003 警告；代码生成不能把正确性押在分析的完备性上——
    // 一旦分析在某个边界判断错，省掉的兜底 return 就是落穿 UB。
    // C 编译器 -O2 会把不可达的兜底 return 删掉，性能零损失。
    void emitFuncBody(FuncDecl* fn, const char* forceDefaultReturn) {
        line("{");
        indent++;
        for (Stmt* s : fn->body->stmts) stmt(s);
        if (forceDefaultReturn) {
            line(std::string("return ") + forceDefaultReturn + ";");
        } else if (fn->retTy != tVoid) {
            line("return " + zeroOfA(fn->retTy) + ";");
        }
        indent--;
        line("}");
    }

    void func(FuncDecl* fn) {
        curFile = fn->file.empty() ? opt.sourceName : fn->file;
        curRetTy = fn->retTy;
        std::string sig = std::string(cType(fn->retTy)) + " " +
                          mangleFor(fn->module, fn->name) + "(";
        if (fn->params.empty()) {
            sig += "void";
        } else {
            for (size_t i = 0; i < fn->params.size(); i++) {
                if (i) sig += ", ";
                const Param& p = fn->params[i];
                sig += std::string(cType(p.ty)) + " " + mangle(p.name);
            }
        }
        sig += ")";
        line(sig);
        emitFuncBody(fn, nullptr);
        blank();
    }

    // extern fn 的 C 原型（extern 函数不参与名字改写，直接引用 C 符号）
    void externProto(FuncDecl* fn) {
        std::string sig = "extern " + std::string(cType(fn->retTy)) + " " +
                          fn->name + "(";
        if (fn->params.empty()) {
            sig += "void";
        } else {
            for (size_t i = 0; i < fn->params.size(); i++) {
                if (i) sig += ", ";
                sig += cType(fn->params[i].ty);
            }
        }
        sig += ");";
        line(sig);
    }

    void program(Program* prog) {
        // 头部注释
        raw("/* ============================================================\n");
        raw(" * 本文件由 Lux 编译器自动生成，请勿手工修改。\n");
        raw(" * 源文件: " + opt.sourceName + "\n");
        raw(" * 后端  : C（由 gcc/clang 进一步优化为机器码）\n");
        raw(" * ============================================================ */\n");
        raw(kRuntime);
        blank();
        // ARC 字面量包装区（占位，program() 末尾回填）
        raw("/*__LUX_LIT_WRAPPERS__*/\n");
        litAnchor = buf.size() - std::string("/*__LUX_LIT_WRAPPERS__*/\n").size();
        blank();

        // struct 类型定义（0.7）：字段对象，布局 = N*8
        // 先全部前向声明再定义，允许 struct 互相引用（A 的字段是 B*）。
        if (!prog->structs.empty()) {
            raw("/* --------------------------- struct --------------------------- */\n");
            for (StructDecl* sd : prog->structs) {
                std::string cn = structCName(sd->name);
                line("typedef struct " + cn + " " + cn + ";");
            }
            blank();
            for (StructDecl* sd : prog->structs) {
                std::string cn = structCName(sd->name);
                line("struct " + cn + " {");
                indent++;
                for (const FieldDecl& f : sd->fields) {
                    line(cType(f.ty) + " f_" + f.name + ";");
                }
                indent--;
                line("};");
            }
            blank();
            // pelem / to_str / on_zero 前置声明（嵌套 struct 互相引用）
            for (StructDecl* sd : prog->structs) {
                std::string cn = structCName(sd->name);
                line("static void " + cn + "_pelem(lx_sb* b, const void* p);");
                line("static char* " + cn + "_to_str(" + cn + "* o);");
                line("static void " + cn + "_on_zero(void* p);");
            }
            blank();
            for (StructDecl* sd : prog->structs) {
                std::string cn = structCName(sd->name);
                line("static void " + cn + "_pelem(lx_sb* b, const void* p) {");
                indent++;
                line("const " + cn + "* o = *(const " + cn + "**)p;");
                line("if (!o) { lx_sb_push_lit(b, \"null\"); return; }");
                line("lx_sb_push_lit(b, \"" + sd->name + " {\");");
                for (size_t i = 0; i < sd->fields.size(); i++) {
                    const FieldDecl& f = sd->fields[i];
                    const Ty* ft = f.ty;
                    line("lx_sb_push_lit(b, \" " + f.name + ": \");");
                    switch (ft->kind) {
                        case TyKind::Int:
                            line("lx_pe_i64(b, &o->f_" + f.name + ");");
                            break;
                        case TyKind::Float:
                            line("lx_pe_f64(b, &o->f_" + f.name + ");");
                            break;
                        case TyKind::Bool:
                            line("lx_pe_bool(b, &o->f_" + f.name + ");");
                            break;
                        case TyKind::String:
                            line("lx_pe_str(b, &o->f_" + f.name + ");");
                            break;
                        case TyKind::Array:
                            line("lx_pe_arr(b, &o->f_" + f.name + ");");
                            break;
                        case TyKind::Named:
                            line(structCName(ft->name) + "_pelem(b, &o->f_" +
                                 f.name + ");");
                            break;
                        default:
                            break;
                    }
                    if (i + 1 < sd->fields.size())
                        line("lx_sb_push_lit(b, \",\");");
                }
                line("lx_sb_push_lit(b, \" }\");");
                indent--;
                line("}");
                line("static char* " + cn + "_to_str(" + cn + "* o) {");
                indent++;
                line("lx_sb b; b.p = 0; b.len = 0; b.cap = 0;");
                line(cn + "_pelem(&b, &o);");
                line("if (!b.p) return lx_str_dup(\"\");");
                line("return b.p;");
                indent--;
                line("}");
                // ARC：释放 struct 内引用类型的字段（头由 lx_gc_release 统一 free）
                line("static void " + cn + "_on_zero(void* p) {");
                indent++;
                line(cn + "* o = (" + cn + "*)p;");
                for (const FieldDecl& f : sd->fields) {
                    if (isRefTy(f.ty))
                        line("lx_gc_release((void*)o->f_" + f.name + ");");
                }
                line("(void)o;");
                indent--;
                line("}");
            }
            blank();
        }

        // extern fn 原型（调用 C 库函数）
        bool hasExtern = false;
        for (FuncDecl* fn : prog->funcs) {
            if (fn->isExtern) {
                hasExtern = true;
                break;
            }
        }
        if (hasExtern) {
            raw("/* --------------------- 外部 C 函数声明 --------------------- */\n");
            for (FuncDecl* fn : prog->funcs) {
                if (fn->isExtern) externProto(fn);
            }
            blank();
        }

        // 顶层全局常量（编译期常量，直接落成 C 的 static const）
        if (!prog->consts.empty()) {
            raw("/* ------------------------- 全局常量 ------------------------- */\n");
            for (GlobalConstDecl* gc : prog->consts) {
                curFile = gc->file.empty() ? opt.sourceName : gc->file;
                std::string v = expr(gc->init);
                if (gc->resolved == tFloat && gc->init->ty == tInt) {
                    v = "((double)" + v + ")";
                }
                std::string name = mangleFor(gc->module, gc->name);
                std::string decl = (gc->resolved == tString)
                                       ? "static const char* const " + name
                                       : "static const " +
                                             std::string(cType(gc->resolved)) +
                                             " " + name;
                // 合成的常量（如 pi / e）可能没被用到，标记 unused 避免警告
                if (gc->synth) decl += " __attribute__((unused))";
                line(decl + " = " + v + ";");
            }
            curFile = opt.sourceName;
            blank();
        }

        // 前置声明（支持相互递归；extern fn 已在上方声明，这里跳过）
        FuncDecl* mainFn = nullptr;
        for (FuncDecl* fn : prog->funcs) {
            if (fn->name == "main" && fn->module.empty()) {
                mainFn = fn;
                continue;
            }
            if (fn->isExtern) continue;
            std::string sig = std::string(cType(fn->retTy)) + " " +
                              mangleFor(fn->module, fn->name) + "(";
            if (fn->params.empty()) {
                sig += "void";
            } else {
                for (size_t i = 0; i < fn->params.size(); i++) {
                    if (i) sig += ", ";
                    sig += cType(fn->params[i].ty);
                }
            }
            sig += ");";
            line(sig);
        }
        blank();

        // 函数定义（main 放最后；extern fn 没有函数体，跳过）
        for (FuncDecl* fn : prog->funcs) {
            if ((fn->name == "main" && fn->module.empty()) || fn->isExtern)
                continue;
            func(fn);
        }

        if (mainFn) {
            curFile = mainFn->file.empty() ? opt.sourceName : mainFn->file;
            curRetTy = mainFn->retTy;
            bool hasArgv = !mainFn->params.empty();
            line(hasArgv ? "int main(int argc, char** argv)"
                         : "int main(void)");
            line("{");
            indent++;
            if (arcMode) line("lx_gc_enabled = 1;");
            if (hasArgv) {
                const Param& p = mainFn->params[0];
                line(cType(p.ty) + " " + mangle(p.name) +
                     " = lx_argv_new(argc, argv);");
            }
            for (Stmt* s : mainFn->body->stmts) stmt(s);
            line("return 0;");  // C 的 main 必须返回 int
            indent--;
            line("}");
        }

        // 回填 ARC 字符串字面量的静态不可变对象
        if (!litDefs.empty()) {
            const std::string marker = "/*__LUX_LIT_WRAPPERS__*/";
            std::string w;
            for (const std::string& d : litDefs) w += d + "\n";
            buf.replace(litAnchor, marker.size(), w);
        }
    }
};

}  // namespace

std::string generateC(Program* prog, const CodegenOptions& opt) {
    CGen gen(opt);
    gen.program(prog);
    return gen.buf;
}

}  // namespace lux
