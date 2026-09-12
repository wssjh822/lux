// =============================================================================
//  codegen.cpp : C 后端 —— 把类型检查后的 AST 翻译成一份自包含的 C 源码
//
//  设计要点：
//    * 每个 Lux 类型直接映射到最贴近的 C 类型，没有装箱、没有虚表
//      int -> int64_t   float -> double   bool -> bool   string -> const char*
//    * 用户标识符统一加 "lx_" 前缀，避免与 C 关键字/库符号冲突；
//      来自模块的声明再加模块名前缀（lx_模块_名字）
//    * 生成的 C 自带运行时（打印、字符串、转换、读行、除零检查）
//    * 用 #line 指令把 C 编译器的诊断信息指回 .lux 源文件的行号
// =============================================================================
#include "lux.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace lux {

namespace {

// -----------------------------------------------------------------------------
//  Lux 运行时（内联进生成的 C 文件，保证单文件自包含）
// -----------------------------------------------------------------------------
const char* kRuntime = R"CLUX_RUNTIME(
/* ------------------------- Lux 运行时 ------------------------- */
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

static inline char* lx_alloc(size_t n) {
    char* p = (char*)malloc(n);
    if (!p) lx_panic("内存分配失败");
    return p;
}

static inline char* lx_str_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = lx_alloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
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

static inline int64_t lx_str_to_i64(const char* s) { return (int64_t)strtoll(s, NULL, 10); }
static inline double lx_str_to_f64(const char* s) { return strtod(s, NULL); }

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
        char* np = (char*)realloc(b->p, nc);
        if (!np) lx_panic("内存分配失败");
        b->p = np;
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
static inline void* lx_struct_new(size_t n) {
    void* p = calloc(1, n);
    if (!p) lx_panic("内存分配失败");
    return p;
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
};
typedef struct lx_arr* lx_arr;

/* 新建空数组头（len=cap=0，data=NULL） */
static inline lx_arr lx_arr_new(int64_t esz,
                                void (*pelem)(lx_sb*, const void*)) {
    lx_arr a = (lx_arr)calloc(1, sizeof(struct lx_arr));
    if (!a) lx_panic("内存分配失败");
    a->esz = esz;
    a->pelem = pelem;
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
        lx_panic("切片范围越界");
    int64_t esz = a->esz > 0 ? a->esz : 8;
    lx_arr r = lx_arr_new(esz, a->pelem);
    int64_t cnt = hi - lo;
    if (cnt > 0) {
        lx_arr_reserve(r, cnt);
        memcpy(r->data, (char*)a->data + lo * esz, (size_t)(cnt * esz));
        r->len = cnt;
    }
    return r;
}

/* 字符串切片（0.6）：复制 [lo, hi) 字节到新字符串；hi<0 表示到末尾。 */
static inline char* lx_str_slice(const char* s, int64_t lo, int64_t hi) {
    int64_t n = lx_str_len(s);
    if (hi < 0) hi = n;
    if (lo < 0 || hi > n || lo > hi)
        lx_panic("切片范围越界");
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


static inline void lx_arr_clear(lx_arr a) { a->len = 0; }
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
    lx_arr a = lx_arr_new(sizeof(char*), lx_pe_str);
    for (int i = 0; i < argc; i++) lx_arr_push_str(a, argv[i]);
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

static void lx_arr_print(lx_arr a) {
    lx_sb b = {0, 0, 0};
    lx_arr_sb(&b, a);
    if (!b.p) {
        b.p = lx_alloc(1);
        b.p[0] = '\0';
    }
    fwrite(b.p, 1, b.len, stdout);
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
    lx_arr a = lx_arr_new((int64_t)sizeof(char*), lx_pe_str);
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
    lx_arr a = lx_arr_new((int64_t)sizeof(char*), lx_pe_str);
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
    char* buf = lx_alloc(cap);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            if (cap >= LX_MAX_LINE) {
                lx_panic("输入行过长（超过 1 MiB 上限）");
            }
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) lx_panic("内存分配失败");
            buf = nb;
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
        fprintf(stderr, "lux: 断言失败 (第 %d 行): %s\n", line, msg);
        exit(1);
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
static inline void lx_file_err(const char* op, const char* path) {
    fprintf(stderr, "lux: 运行时错误: 无法%s文件 '%s'\n", op, path);
    exit(1);
}

static inline char* lx_file_read(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) lx_file_err("读取", path);
    if (fseek(f, 0, SEEK_END) != 0) lx_file_err("读取", path);
    long n = ftell(f);
    if (n < 0) lx_file_err("读取", path);
    rewind(f);
    char* buf = lx_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static inline bool lx_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
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

// 数组实体的"空值"：一个不指向任何数据的空数组（esz / pelem 按元素类型补全）
std::string arrZero(const Ty* elem) {
    return "lx_arr_new(" + arrEsz(elem) + ", " + arrPelem(elem) + ")";
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
        default:
            return "0";
    }
}

std::string mangle(const std::string& name) {
    if (name == "main") return "main";  // 入口函数直接映射到 C 的 main
    return "lx_" + name;
}

// 带模块前缀的名字改写：来自模块的声明避免与根文件 / 其他模块撞名
std::string mangleFor(const std::string& module, const std::string& name) {
    if (module.empty()) return mangle(name);
    return "lx_" + module + "_" + name;
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
    std::string curFile;  // 当前正在生成的声明所在的源文件（供 #line 使用）

    const Ty* tInt = TyStore::int64Ty();
    const Ty* tFloat = TyStore::float64Ty();
    const Ty* tBool = TyStore::boolTy();
    const Ty* tString = TyStore::stringTy();
    const Ty* tVoid = TyStore::voidTy();

    explicit CGen(const CodegenOptions& o) : opt(o), curFile(o.sourceName) {
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
                // Sema 折叠过的常量表达式直接落成字面量
                if (n->folded) {
                    if (n->foldedIsStr)
                        return "\"" + escapeCString(n->foldedS) + "\"";
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
                                arrEsz(et) + ", " + arrPelem(et) + ");";
                for (Expr* el : n->elems) {
                    std::string ev = expr(el);
                    if (et && et->kind == TyKind::Float && el->ty == tInt) {
                        ev = "((double)" + ev + ")";
                    }
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
                                "*)lx_struct_new(sizeof(" + cn + "));";
                StructDecl* sd = n->decl;
                for (auto& kv : n->inits) {
                    int idx = sd ? structFieldIndex(sd, kv.first) : -1;
                    if (idx < 0) continue;
                    const Ty* ft = sd->fields[idx].ty;
                    std::string fv = expr(kv.second);
                    if (ft == tFloat && kv.second->ty == tInt) {
                        fv = "((double)" + fv + ")";
                    }
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
                std::string a = expr(n->thenVal);
                std::string b = expr(n->elseVal);
                if (n->ty == tFloat) {
                    if (n->thenVal->ty == tInt) a = "((double)" + a + ")";
                    if (n->elseVal->ty == tInt) b = "((double)" + b + ")";
                }
                return "(" + expr(n->cond) + " ? " + a + " : " + b + ")";
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
                    return "lx_str_concat(" + expr(n->lhs) + ", " + expr(n->rhs) +
                           ")";
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
                return "((int64_t)(((uint64_t)" + expr(n->lhs) + ") >> " +
                       expr(n->rhs) + "))";
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
                        return "lx_str_to_i64(" + expr(a) + ")";
                    default:
                        return "((int64_t)(" + expr(a) + "))";
                }
            }
            case Builtin::ToFloat: {
                if (c->args.empty()) return "((double)0.0)";
                Expr* a = c->args[0];
                switch (a->ty->kind) {
                    case TyKind::String:
                        return "lx_str_to_f64(" + expr(a) + ")";
                    case TyKind::Bool:
                        return "((double)(" + expr(a) + " ? 1.0 : 0.0))";
                    default:
                        return "((double)(" + expr(a) + "))";
                }
            }
            case Builtin::ToString: {
                if (c->args.empty()) return "\"\"";
                Expr* a = c->args[0];
                if (a->ty && a->ty->kind == TyKind::Array) {
                    return "lx_arr_to_str(" + expr(a) + ")";
                }
                if (a->ty && a->ty->kind == TyKind::Named) {
                    return structCName(a->ty->name) + "_to_str(" + expr(a) +
                           ")";
                }
                switch (a->ty->kind) {
                    case TyKind::Int:
                        return "lx_i64_to_str(" + expr(a) + ")";
                    case TyKind::Float:
                        return "lx_f64_to_str(" + expr(a) + ")";
                    case TyKind::Bool:
                        return "lx_bool_to_str(" + expr(a) + ")";
                    default:
                        return "(" + expr(a) + ")";
                }
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
                return "lx_file_read(" + expr(c->args[0]) + ")";
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
                    if (a->ty && a->ty->kind == TyKind::Array) {
                        s += "lx_arr_to_str(" + expr(a) + ")";
                    } else if (a->ty && a->ty->kind == TyKind::Named) {
                        s += structCName(a->ty->name) + "_to_str(" + expr(a) +
                             ")";
                    } else if (a->ty == tString) {
                        s += expr(a);
                    } else if (a->ty == tInt) {
                        s += "lx_i64_to_str(" + expr(a) + ")";
                    } else if (a->ty == tFloat) {
                        s += "lx_f64_to_str(" + expr(a) + ")";
                    } else if (a->ty == tBool) {
                        s += "lx_bool_to_str(" + expr(a) + ")";
                    } else {
                        s += expr(a);
                    }
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
                std::string arg = expr(c->args[0]);
                if (c->methodElem && c->methodElem->kind == TyKind::Float &&
                    c->args[0]->ty == tInt) {
                    arg = "((double)" + arg + ")";
                }
                return "((void)lx_arr_push_" + arrRT(c->methodElem) + "(" +
                       recv + ", " + arg + "))";
            }
            case Builtin::ArrPop:
                return "lx_arr_pop_" + arrRT(c->methodElem) + "(" +
                       mangle(c->methodRecv) + ")";
            case Builtin::ArrInsert: {
                std::string arg = expr(c->args[1]);
                if (c->methodElem && c->methodElem->kind == TyKind::Float &&
                    c->args[1]->ty == tInt) {
                    arg = "((double)" + arg + ")";
                }
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
            // 实参到形参的隐式提升（int -> float）
            if (c->target && i < c->target->params.size() &&
                a->ty == tInt && c->target->params[i].ty == tFloat) {
                av = "((double)" + av + ")";
            }
            s += av;
        }
        s += ")";
        return s;
    }

    // 复合赋值右值：字符串用 concat（C 的 + 是指针算术）
    std::string compoundRhs(BinOp op, const std::string& lhs,
                            const std::string& rhs, const Ty* tt) {
        if (tt == tString && op == BinOp::Add)
            return "lx_str_concat(" + lhs + ", " + rhs + ")";
        return "((" + lhs + ") " + binOpText(op) + " " + rhs + ")";
    }

    // ---- 语句 ----
    // 左值赋值（0.7）：x / p.x / a[i] / a[i][j] / a[i].x 统一处理
    void assignStmt(AssignStmt* a) {
        Expr* t = a->target;
        const Ty* tt = a->elemTy ? a->elemTy : TyStore::int64Ty();
        std::string v = expr(a->value);
        if (tt == tFloat && a->value->ty == tInt) v = "((double)" + v + ")";
        if (t->kind == ExprKind::Index) {
            // 数组元素：用临时变量保证 base / index 只求值一次
            std::string base = expr(static_cast<IndexExpr*>(t)->base);
            std::string idx = expr(static_cast<IndexExpr*>(t)->index);
            std::string bv = tmpName("b"), iv = tmpName("i");
            line("lx_arr " + bv + " = " + base + ";");
            line("int64_t " + iv + " = " + idx + ";");
            std::string elem =
                "lx_arr_get_" + arrRT(tt) + "(" + bv + ", " + iv + ")";
            std::string rhs = v;
            if (a->compound)
                rhs = compoundRhs(a->compoundOp, elem, v, tt);
            line("lx_arr_set_" + arrRT(tt) + "(" + bv + ", " + iv + ", " +
                 rhs + ");");
            return;
        }
        // Ident / Member：本身就是合法的 C 左值
        std::string lv = lvalueOf(t);
        if (a->compound) {
            if (tt == tString && a->compoundOp == BinOp::Add) {
                line(lv + " = lx_str_concat(" + lv + ", " + v + ");");
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
                std::string decl;
                if (let->isConst) decl += "const ";
                decl += std::string(cType(let->resolved)) + " " +
                        mangle(let->name);
                if (let->init) {
                    std::string init = expr(let->init);
                    if (let->resolved == tFloat && let->init->ty == tInt) {
                        init = "((double)" + init + ")";
                    }
                    decl += " = " + init;
                } else {
                    decl += " = " + zeroOf(let->resolved);
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
                            switch (a->ty->kind) {
                                case TyKind::Int:
                                    line("lx_print_i64(" + expr(a) + ");");
                                    break;
                                case TyKind::Float:
                                    line("lx_print_f64(" + expr(a) + ");");
                                    break;
                                case TyKind::Bool:
                                    line("lx_print_bool(" + expr(a) + ");");
                                    break;
                                case TyKind::String:
                                    line("lx_print_str(" + expr(a) + ");");
                                    break;
                                case TyKind::Array:
                                    // 0.5：数组打印成 [1, 2, 3] 形式
                                    line("lx_arr_print(" + expr(a) + ");");
                                    break;
                                case TyKind::Named:
                                    // struct 打印成 Point { x: 1, y: 2 }（0.7）
                                    line("lx_print_str(" +
                                         structCName(a->ty->name) + "_to_str(" +
                                         expr(a) + "));");
                                    break;
                                default:
                                    break;
                            }
                        }
                        if (c->builtin == Builtin::PrintLn) line("lx_print_nl();");
                        break;
                    }
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
                    line("const char* " + itVar + " = " + expr(f->iterable) +
                         ";");
                    line("for (int64_t " + ixVar + " = 0; " + itVar + "[" +
                         ixVar + "] != 0; " + ixVar + " += 1) {");
                    indent++;
                    line("const char* " + mangle(f->var) +
                         " = lx_str_char_at(" + itVar + ", " + ixVar + ");");
                    stmt(f->body);
                    indent--;
                    line("}");
                } else {
                    const Ty* et = f->varTy ? f->varTy : TyStore::int64Ty();
                    line("lx_arr " + itVar + " = " + expr(f->iterable) + ";");
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
                    line("return;");
                } else {
                    std::string v = expr(r->value);
                    if (curRetTy == tFloat && r->value->ty == tInt) {
                        v = "((double)" + v + ")";
                    }
                    line("return " + v + ";");
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
            line("return " + zeroOf(fn->retTy) + ";");
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
            // pelem / to_str 前置声明（嵌套 struct 互相引用）
            for (StructDecl* sd : prog->structs) {
                std::string cn = structCName(sd->name);
                line("static void " + cn + "_pelem(lx_sb* b, const void* p);");
                line("static char* " + cn + "_to_str(" + cn + "* o);");
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
    }
};

}  // namespace

std::string generateC(Program* prog, const CodegenOptions& opt) {
    CGen gen(opt);
    gen.program(prog);
    return gen.buf;
}

}  // namespace lux
