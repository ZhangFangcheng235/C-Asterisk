/* ============================================================
 * cask_rt.h -- C* (C Asterisk) language runtime
 * Included automatically by code generated from .cask sources.
 * Everything here is "static inline" or macro: header-only.
 * ============================================================ */
#ifndef CASK_RT_H
#define CASK_RT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
/* Do NOT include <windows.h> here: parsing it costs ~760ms of compile
 * time per program.  We only ever call these two console functions,
 * so declare them directly (kernel32, stdcall). */
#ifndef _WINDOWS_
#ifdef __cplusplus
extern "C" {
#endif
extern int __stdcall SetConsoleOutputCP(unsigned int);
extern int __stdcall SetConsoleCP(unsigned int);
#ifdef __cplusplus
}
#endif
#endif
#endif

#define CASK_RT_VERSION "0.1"

/* ---------------- console / buffering ---------------- */
/* codepage: 0 = keep default; 65001 = UTF-8 (sources saved as UTF-8) */
static void cask_init(int codepage)
{
#if defined(_WIN32) && !defined(CASK_NO_WINCON)
    if (codepage) {
        SetConsoleOutputCP((unsigned int)codepage);
        SetConsoleCP((unsigned int)codepage);
    }
#else
    (void)codepage;
#endif
    /* line-buffered stdout: every say ends with a newline so its whole
     * line goes to the OS in ONE write instead of one per part; input
     * flushes first so show-prompts still appear before reading */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    srand((unsigned)time(NULL));   /* a fresh random sequence each run */
}

/* ---------------- error reporting ---------------- */
static void cask_die(const char *file, int line, const char *msg)
{
    fflush(stdout);   /* keep any show/say text before the error */
    fprintf(stderr, "\nC* 运行错误: %s\n  位置 %s:%d\n", msg, file, line);
    exit(1);
}

/* ---------------- checked integer arithmetic ---------------- */
#if defined(__GNUC__) && !defined(__TINYC__)
#define CASK_GCC_OVF 1
#else
#define CASK_GCC_OVF 0
#endif

static long long cask_add(long long a, long long b, const char *f, int l)
{
#if CASK_GCC_OVF
    long long r;
    if (__builtin_add_overflow(a, b, &r)) cask_die(f, l, "整数加法溢出");
    return r;
#else
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b))
        cask_die(f, l, "整数加法溢出");
    return a + b;
#endif
}

static long long cask_sub(long long a, long long b, const char *f, int l)
{
#if CASK_GCC_OVF
    long long r;
    if (__builtin_sub_overflow(a, b, &r)) cask_die(f, l, "整数减法溢出");
    return r;
#else
    if ((b > 0 && a < LLONG_MIN + b) || (b < 0 && a > LLONG_MAX + b))
        cask_die(f, l, "整数减法溢出");
    return a - b;
#endif
}

static long long cask_mul(long long a, long long b, const char *f, int l)
{
#if CASK_GCC_OVF
    long long r;
    if (__builtin_mul_overflow(a, b, &r)) cask_die(f, l, "整数乘法溢出");
    return r;
#else
    if (a > 0) {
        if (b > 0) { if (a > LLONG_MAX / b) cask_die(f, l, "整数乘法溢出"); }
        else       { if (b < LLONG_MIN / a) cask_die(f, l, "整数乘法溢出"); }
    } else {
        if (b > 0) { if (a < LLONG_MIN / b) cask_die(f, l, "整数乘法溢出"); }
        else       { if (a != 0 && a < LLONG_MAX / b) cask_die(f, l, "整数乘法溢出"); }
    }
    return a * b;
#endif
}

static long long cask_div(long long a, long long b, const char *f, int l)
{
    if (b == 0) cask_die(f, l, "除以零");
    if (a == LLONG_MIN && b == -1) cask_die(f, l, "整数除法溢出");
    return a / b;
}

static long long cask_mod(long long a, long long b, const char *f, int l)
{
    if (b == 0) cask_die(f, l, "除以零（'%' 取余）");
    if (a == LLONG_MIN && b == -1) return 0;
    return a % b;
}

static double cask_fdiv(double a, double b, const char *f, int l)
{
    if (b == 0.0) cask_die(f, l, "除以零（float 除法）");
    return a / b;
}

static long long cask_abs(long long a, const char *f, int l)
{
    if (a == LLONG_MIN) cask_die(f, l, "'abs' 整数溢出");
    return a < 0 ? -a : a;
}

/* ---------------- array index check ---------------- */
/* returns the index so it can be used as:  arr[cask_ckidx(i, N, file, line)] */
static long long cask_ckidx(long long i, long long n, const char *f, int l)
{
    if (i < 0 || i >= n) cask_die(f, l, "数组下标越界");
    return i;
}

/* range check for letter (signed char) values */
static long long cask_ck_char(long long v, const char *f, int l)
{
    if (v < -128 || v > 127) cask_die(f, l, "这个值放不进 char（-128..127）");
    return v;
}

/* checked shifts */
static long long cask_shl(long long a, long long b, const char *f, int l)
{
    if (b < 0 || b > 63) cask_die(f, l, "移位位数必须是 0..63");
    if (a < 0 && b > 0) cask_die(f, l, "负数不能左移");
    if (b > 0 && a > (LLONG_MAX >> b)) cask_die(f, l, "整数左移溢出");
    return (long long)((unsigned long long)a << (unsigned long long)b);
}
static long long cask_shr(long long a, long long b, const char *f, int l)
{
    if (b < 0 || b > 63) cask_die(f, l, "移位位数必须是 0..63");
    return a >> b;
}

/* index into a text value: at(text, i) -> letter, checked */
static char cask_at(const char *s, long long i, const char *f, int l)
{
    if (!s) cask_die(f, l, "at(): 这段文本是空的");
    if (i < 0 || i >= (long long)strlen(s)) cask_die(f, l, "at(): 下标越界");
    return s[i];
}

/* ---------------- strings (text) ----------------
 * C* text values are read-only byte buffers. Operations never
 * modify a buffer in place; they allocate a new one. The runtime
 * never frees (small, short-lived programs -- keeps things simple).
 * Uninitialized text is NULL and treated as the empty string.      */
static char *cask_alloc(size_t n)
{
    char *p = (char *)malloc(n);
    if (!p) { fflush(stdout); fputs("C*: 内存不够了\n", stderr); exit(1); }
    return p;
}

static char *cask_cat(const char *a, const char *b)
{
    size_t la, lb;
    char *p;
    if (!a) a = "";
    if (!b) b = "";
    la = strlen(a); lb = strlen(b);
    p = cask_alloc(la + lb + 1);
    memcpy(p, a, la);
    memcpy(p + la, b, lb + 1);
    return p;
}

static long long cask_len(const char *s) { return s ? (long long)strlen(s) : 0; }

/* VALUE(text) -- the Excel VALUE(): text -> number, junk -> 0 */
static long long cask_val(const char *s) { return s ? atoll(s) : 0; }

/* ---------------- rcl() ----------------
 * 一个彩蛋（easter egg）：rcl 是方程235的女朋友，方程235永远爱 rcl。
 *   rcl();            -- 打印「方程235永远爱rcl！」，然后结束程序
 *   rcl(235);         -- 打印「方程235 爱 rcl」
 *   rcl("rcl");       -- 打印「rcl，方程235永远爱你！」
 *   rcl("方程235");   -- 打印「方程235 爱 rcl」
 *     还认这些写法（不分大小写）：235 / FangCheng235 / ZFC235 / JF235 / JF / FC235 / FC / ZFC / FangCheng
 *   别的值            -- 什么也不做（程序照常往下跑）
 */
static int cask_rcl_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static void cask_rcl_ask(void)
{
    printf("\u65b9\u7a0b235\u6c38\u8fdc\u7231rcl\uff01\n");   /* 方程235永远爱rcl！ */
    fflush(stdout);
    exit(0);
}
static long long cask_rcl_num(double n)
{
    if (n == 235.0) { printf("\u65b9\u7a0b235 \u7231 rcl\n"); fflush(stdout); }
    return 0;
}
static long long cask_rcl_text(const char *s)
{
    if (!s) return 0;
    if (cask_rcl_eq(s, "rcl")) {
        printf("rcl\uff0c\u65b9\u7a0b235\u6c38\u8fdc\u7231\u4f60\uff01\n");  /* rcl，方程235永远爱你！ */
        fflush(stdout);
        return 0;
    }
    if (cask_rcl_eq(s, "235") || cask_rcl_eq(s, "\u65b9\u7a0b235") ||
        cask_rcl_eq(s, "FangCheng235") || cask_rcl_eq(s, "ZFC235") ||
        cask_rcl_eq(s, "JF235") || cask_rcl_eq(s, "JF") ||
        cask_rcl_eq(s, "FC235") || cask_rcl_eq(s, "FC") ||
        cask_rcl_eq(s, "ZFC") || cask_rcl_eq(s, "FangCheng")) {
        printf("\u65b9\u7a0b235 \u7231 rcl\n");   /* 方程235 爱 rcl */
        fflush(stdout);
    }
    return 0;
}

/* number/real/bool/letter -> text (rotating buffers, nesting-safe) */
static const char *cask_ftos(long long v)
{
    static char buf[8][24]; static int k = 0;
    char *b = buf[k = (k + 1) & 7];
    sprintf(b, "%lld", v);
    return b;
}
static const char *cask_ftos_r(double v)
{
    static char buf[8][40]; static int k = 0;
    char *b = buf[k = (k + 1) & 7];
    sprintf(b, "%.15g", v);
    return b;
}
static const char *cask_ftos_b(int v)
{
    static char buf[8][8]; static int k = 0;
    char *b = buf[k = (k + 1) & 7];
    strcpy(b, v ? "true" : "false");
    return b;
}
static const char *cask_ftos_c(char v)
{
    static char buf[8][2]; static int k = 0;
    char *b = buf[k = (k + 1) & 7];
    b[0] = v; b[1] = 0;
    return b;
}

/* ---------------- printing (say / show) ---------------- */
static void cask_p_num(long long v)  { printf("%lld", v); }
static void cask_p_real(double v)    { printf("%.15g", v); }
static void cask_p_str(const char *s){ fputs(s ? s : "", stdout); }
static void cask_p_char(char c)      { putchar(c); }
static void cask_p_bool(int b)       { fputs(b ? "true" : "false", stdout); }
static void cask_nl(void)            { putchar('\n'); }

/* ---------------- input ----------------
 * Every input reads exactly one line from the keyboard, which keeps
 * numbers, text and letters from eating each other's line endings. */
static int cask_read_line(char **out)
{
    size_t cap = 128, len = 0;
    char *buf;
    int c, got = 0;
    fflush(stdout);           /* make sure a show-prompt is visible */
    buf = cask_alloc(cap);
    for (;;) {
        c = getchar();
        if (c == EOF) break;
        got = 1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 2 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) { fflush(stdout); fputs("C*: 内存不够了\n", stderr); exit(1); }
        }
        buf[len++] = (char)c;
    }
    if (!got) { free(buf); return 0; }
    buf[len] = 0;
    *out = buf;
    return 1;
}

static int cask_in_num(long long *v)
{
    for (;;) {
        char *line, *p, *end;
        if (!cask_read_line(&line)) return 0;
        p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p) {
            long long x = strtoll(p, &end, 10);
            while (isspace((unsigned char)*end)) end++;
            if (end != p && *end == 0) { free(line); *v = x; return 1; }
        }
        free(line);
        fputs("请输入一个整数: ", stdout);
    }
}

static int cask_in_real(double *v)
{
    for (;;) {
        char *line, *p, *end;
        if (!cask_read_line(&line)) return 0;
        p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p) {
            double x = strtod(p, &end);
            while (isspace((unsigned char)*end)) end++;
            if (end != p && *end == 0) { free(line); *v = x; return 1; }
        }
        free(line);
        fputs("请输入一个数: ", stdout);
    }
}

static int cask_in_char(char *v)
{
    for (;;) {
        char *line, *p;
        if (!cask_read_line(&line)) return 0;
        p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p) { *v = *p; free(line); return 1; }
        free(line);
        fputs("请输入一个字母: ", stdout);
    }
}

static int cask_in_str(char **v)
{
    char *line;
    if (!cask_read_line(&line)) return 0;
    *v = line;           /* kept for the life of the program (text never frees) */
    return 1;
}

static int cask_in_bool(int *v)
{
    long long x;
    if (!cask_in_num(&x)) return 0;
    *v = (x != 0);
    return 1;
}

/* ---------------- built-in functions ---------------- */
static double  cask_random(void) { return (double)rand() / (double)RAND_MAX; }
static long long cask_now(void)  { return (long long)time(NULL); }

/* ---------------- Excel-style helpers ----------------
 * Backing code for the UPPER-case built-in functions
 * (IF, MOD, ROUND, SUM, LEFT, ...). Functions that can fail take
 * __FILE__/__LINE__ so a runtime error points at the C* source line.  */

/* ROUND / INT / SIGN ----------------------------------------- */
static long long cask_sign(long long v)  { return (v > 0) - (v < 0); }
static long long cask_sign_r(double v)   { return (v > 0) - (v < 0); }
static long long cask_int(double v)      { return (long long)floor(v); }
static double cask_xround(double v, long long n, const char *f, int l)
{
    double p;
    if (n < 0 || n > 15) cask_die(f, l, "ROUND() 的小数位数必须是从 0 到 15");
    p = pow(10.0, (double)n);
    return (v >= 0.0 ? floor(v * p + 0.5) : ceil(v * p - 0.5)) / p;
}

/* MOD (Excel meaning: the result has the sign of the divisor) - */
static long long cask_xmod(long long a, long long b, const char *f, int l)
{
    long long r;
    if (b == 0) cask_die(f, l, "除以零");
    if (b == -1) return 0;            /* LLONG_MIN % -1 would crash */
    r = a % b;
    if (r != 0 && ((r ^ b) < 0)) r += b;
    return r;
}
static double cask_fxmod(double a, double b, const char *f, int l)
{
    if (b == 0.0) cask_die(f, l, "除以零");
    return a - b * floor(a / b);
}

/* LOG family (Excel LOG is base 10, base is optional) --------- */
static double cask_ln(double x, const char *f, int l)
{
    if (!(x > 0.0)) cask_die(f, l, "LN() 要一个大于 0 的数");
    return log(x);
}
static double cask_log10e(double x, const char *f, int l)
{
    if (!(x > 0.0)) cask_die(f, l, "LOG() 要一个大于 0 的数");
    return log10(x);
}
static double cask_logb(double x, double b, const char *f, int l)
{
    if (!(x > 0.0)) cask_die(f, l, "LOG() 要一个大于 0 的数");
    if (!(b > 0.0) || b == 1.0) cask_die(f, l, "LOG() 的底数要大于 0，而且不能等于 1");
    return log(x) / log(b);
}

/* RANDBETWEEN (whole numbers, both ends included) ------------- */
static long long cask_randbetween(long long lo, long long hi, const char *f, int l)
{
    double span;
    if (lo > hi) cask_die(f, l, "RANDBETWEEN() 的第一个数不能比第二个数大");
    span = (double)hi - (double)lo + 1.0;
    return lo + (long long)(span * cask_random());
}

/* SUM / AVERAGE / MAX / MIN over arrays ----------------------- */
static long long cask_sum(const long long *a, long long n, const char *f, int l)
{
    long long i, s = 0;
    for (i = 0; i < n; i++) s = cask_add(s, a[i], f, l);
    return s;
}
static double cask_sum_r(const double *a, long long n)
{
    long long i; double s = 0.0;
    for (i = 0; i < n; i++) s += a[i];
    return s;
}
static double cask_avg(const long long *a, long long n, const char *f, int l)
{
    if (n <= 0) cask_die(f, l, "AVERAGE() 至少要有一个数");
    return (double)cask_sum(a, n, f, l) / (double)n;
}
static double cask_avg_r(const double *a, long long n, const char *f, int l)
{
    long long i; double s = 0.0;
    if (n <= 0) cask_die(f, l, "AVERAGE() 至少要有一个数");
    for (i = 0; i < n; i++) s += a[i];
    return s / (double)n;
}
static long long cask_max(const long long *a, long long n, const char *f, int l)
{
    long long i, m;
    if (n <= 0) cask_die(f, l, "MAX() 至少要有一个数");
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] > m) m = a[i];
    return m;
}
static long long cask_min(const long long *a, long long n, const char *f, int l)
{
    long long i, m;
    if (n <= 0) cask_die(f, l, "MIN() 至少要有一个数");
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] < m) m = a[i];
    return m;
}
static double cask_max_r(const double *a, long long n, const char *f, int l)
{
    long long i; double m;
    if (n <= 0) cask_die(f, l, "MAX() 至少要有一个数");
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] > m) m = a[i];
    return m;
}
static double cask_min_r(const double *a, long long n, const char *f, int l)
{
    long long i; double m;
    if (n <= 0) cask_die(f, l, "MIN() 至少要有一个数");
    m = a[0];
    for (i = 1; i < n; i++) if (a[i] < m) m = a[i];
    return m;
}
/* MAX/MIN over separate values (each value is evaluated only once) */
static long long cask_max2(long long a, long long b) { return a > b ? a : b; }
static long long cask_min2(long long a, long long b) { return a < b ? a : b; }
static double cask_max2_r(double a, double b) { return a > b ? a : b; }
static double cask_min2_r(double a, double b) { return a < b ? a : b; }

/* text functions --------------------------------------------- */
static char *cask_substr_copy(const char *s, size_t from, size_t count)
{
    size_t len = s ? strlen(s) : 0;
    char *p;
    if (from > len) from = len;
    if (count > len - from) count = len - from;
    p = cask_alloc(count + 1);
    memcpy(p, (s ? s : "") + from, count);
    p[count] = 0;
    return p;
}
static char *cask_left(const char *s, long long n, const char *f, int l)
{
    if (n < 0) cask_die(f, l, "LEFT()/RIGHT() 的长度不能是负数");
    return cask_substr_copy(s, 0, (size_t)n);
}
static char *cask_right(const char *s, long long n, const char *f, int l)
{
    size_t len = s ? strlen(s) : 0, k;
    if (n < 0) cask_die(f, l, "LEFT()/RIGHT() 的长度不能是负数");
    k = (size_t)n;
    if (k > len) k = len;
    return cask_substr_copy(s, len - k, k);
}
static char *cask_mid(const char *s, long long start, long long n, const char *f, int l)
{
    if (start < 1) cask_die(f, l, "MID() 的位置从 1 开始（和 Excel 一样）");
    if (n < 0) cask_die(f, l, "MID() 的长度不能是负数");
    return cask_substr_copy(s, (size_t)(start - 1), (size_t)n);
}
static char *cask_upper(const char *s)
{
    size_t i, len = s ? strlen(s) : 0;
    char *p = cask_alloc(len + 1);
    for (i = 0; i < len; i++) p[i] = (char)toupper((unsigned char)s[i]);
    p[len] = 0;
    return p;
}
static char *cask_lower(const char *s)
{
    size_t i, len = s ? strlen(s) : 0;
    char *p = cask_alloc(len + 1);
    for (i = 0; i < len; i++) p[i] = (char)tolower((unsigned char)s[i]);
    p[len] = 0;
    return p;
}
static char *cask_trim(const char *s)
{
    /* Excel TRIM: drop spaces at both ends, squeeze inner runs to one */
    size_t len = s ? strlen(s) : 0, i = 0, o = 0;
    char *p = cask_alloc(len + 1);
    while (i < len && s[i] == ' ') i++;
    while (i < len) {
        if (s[i] == ' ') {
            while (i < len && s[i] == ' ') i++;
            if (i < len) p[o++] = ' ';
        } else {
            p[o++] = s[i++];
        }
    }
    p[o] = 0;
    return p;
}

/* ---------------- 新增：找位置 / 替换 / 重复 / 比较 / 字符 ----------------
 * 全部按字节处理（与 LEN / at 一致；一个汉字算 3 个字节）。
 * FIND     -- 1 起的下标，找不到给 0（Excel FIND，区分大小写）
 * SUBSTITUTE -- 把"旧文字"全部换成"新文字"（Excel SUBSTITUTE）
 * REPT     -- 把一段文字重复 n 遍接起来
 * EXACT    -- 两段文字逐字节完全一样（区分大小写）-> 1 / 0
 * CHAR / CODE -- 编号（0..255）<-> 字符（Excel CHAR / CODE）
 * ATAN2    -- 由 y、x 求角度（弧度）
 */
static long long cask_find(const char *s, const char *needle, long long start,
                           const char *f, int l)
{
    size_t len, nl, i;
    if (!s) s = "";
    if (!needle) needle = "";
    if (start < 1) cask_die(f, l, "FIND() 的位置从 1 开始");
    len = strlen(s); nl = strlen(needle);
    if (nl == 0) return start <= (long long)len ? start : 0;
    if ((size_t)start > len) return 0;
    for (i = (size_t)start - 1; i + nl <= len; i++)
        if (memcmp(s + i, needle, nl) == 0) return (long long)i + 1;
    return 0;
}

static char *cask_substitute(const char *s, const char *old, const char *nv)
{
    size_t len, ol, vl, cap, o, i;
    char *p;
    if (!s) s = "";
    if (!old) old = "";
    if (!nv) nv = "";
    len = strlen(s); ol = strlen(old); vl = strlen(nv);
    if (ol == 0) {
        /* 没有可替换的"旧文字"：原样返回 */
        p = cask_alloc(len + 1);
        memcpy(p, s, len + 1);
        return p;
    }
    cap = len + 1;
    p = cask_alloc(cap);
    o = 0;
    for (i = 0; i < len; ) {
        if (i + ol <= len && memcmp(s + i, old, ol) == 0) {
            if (o + vl + 1 > cap) {
                cap = (o + vl + 1) * 2;
                p = (char *)realloc(p, cap);
                if (!p) { fflush(stdout); fputs("C*: 内存不够了\n", stderr); exit(1); }
            }
            memcpy(p + o, nv, vl);
            o += vl; i += ol;
        } else {
            if (o + 2 > cap) {
                cap *= 2;
                p = (char *)realloc(p, cap);
                if (!p) { fflush(stdout); fputs("C*: 内存不够了\n", stderr); exit(1); }
            }
            p[o++] = s[i++];
        }
    }
    p[o] = 0;
    return p;
}

static char *cask_rept(const char *s, long long n, const char *f, int l)
{
    size_t len, i;
    char *p;
    if (!s) s = "";
    if (n < 0) cask_die(f, l, "REPT() 的次数不能是负数");
    len = strlen(s);
    if (n > 0 && len > 100000000ULL / (unsigned long long)n)
        cask_die(f, l, "REPT() 的结果太长了");
    p = cask_alloc((size_t)n * len + 1);
    for (i = 0; i < (size_t)n; i++) memcpy(p + i * len, s, len);
    p[(size_t)n * len] = 0;
    return p;
}

static long long cask_exact(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0 ? 1 : 0;
}

static char *cask_char_code(long long n, const char *f, int l)
{
    char *p;
    if (n < 0 || n > 255) cask_die(f, l, "CHAR() 的值要在 0..255");
    p = cask_alloc(2);
    p[0] = (char)n;
    p[1] = 0;
    return p;
}

static long long cask_code(const char *s)
{
    return (s && s[0]) ? (long long)(unsigned char)s[0] : 0;
}

static double cask_atan2(double y, double x) { return atan2(y, x); }

#endif /* CASK_RT_H */
