/* ============================================================
 * cask_rt.c -- C* (C Asterisk) precompiled runtime library
 *
 * Compiled ONCE at toolchain build time into libcastrt.a:
 *
 *     gcc -O2 -c cask_rt.c -o cask_rt.o
 *     ar rcs libcastrt.a cask_rt.o [cask_gui.o]
 *
 * User programs (.cask) never touch a C compiler: the assembler
 * backend emits `call caskx_*` against the exports below, and the
 * final exe is produced by as + ld linking this archive.
 *
 * The implementations are the ORIGINAL ones from cask_rt.h /
 * C_OPED.cash / C_CSS.cash (included as static functions), wrapped
 * by caskx_* exports with C* integer/double/pointer arguments.
 * This guarantees byte-identical runtime semantics and error text
 * with the old C backend.
 * ============================================================ */
#include "cask_rt.h"

/* ---------------- exports: checks & init ---------------- */
void        caskx_init(int cp)                        { cask_init(cp); }
void        caskx_die(const char *f, int l, const char *m) { cask_die(f, l, m); }
long long   caskx_add(long long a, long long b, const char *f, int l) { return cask_add(a, b, f, l); }
long long   caskx_sub(long long a, long long b, const char *f, int l) { return cask_sub(a, b, f, l); }
long long   caskx_mul(long long a, long long b, const char *f, int l) { return cask_mul(a, b, f, l); }
long long   caskx_div(long long a, long long b, const char *f, int l) { return cask_div(a, b, f, l); }
long long   caskx_mod(long long a, long long b, const char *f, int l) { return cask_mod(a, b, f, l); }
double      caskx_fdiv(double a, double b, const char *f, int l)      { return cask_fdiv(a, b, f, l); }
long long   caskx_abs(long long a, const char *f, int l)  { return cask_abs(a, f, l); }
long long   caskx_ckidx(long long i, long long n, const char *f, int l) { return cask_ckidx(i, n, f, l); }
long long   caskx_ck_char(long long v, const char *f, int l) { return cask_ck_char(v, f, l); }
long long   caskx_shl(long long a, long long b, const char *f, int l) { return cask_shl(a, b, f, l); }
long long   caskx_shr(long long a, long long b, const char *f, int l) { return cask_shr(a, b, f, l); }
long long   caskx_at(const char *s, long long i, const char *f, int l)
{ return (long long)(signed char)cask_at(s, i, f, l); }

/* ---------------- exports: printing ---------------- */
void caskx_p_num(long long v)   { cask_p_num(v); }
void caskx_p_real(double v)     { cask_p_real(v); }
void caskx_p_str(const char *s) { cask_p_str(s); }
void caskx_p_char(long long c)  { cask_p_char((char)(signed char)c); }
void caskx_p_bool(long long b)  { cask_p_bool((int)b); }
void caskx_nl(void)             { cask_nl(); }

/* ---------------- exports: input ---------------- */
long long caskx_in_num(void *slot)  { long long v = 0; if (cask_in_num(&v)) { *(long long *)slot = v; return 1; } return 0; }
long long caskx_in_real(void *slot) { double v = 0;    if (cask_in_real(&v)) { *(double *)slot = v; return 1; } return 0; }
long long caskx_in_char(void *slot) { char v = 0;      if (cask_in_char(&v)) { *(long long *)slot = (long long)(signed char)v; return 1; } return 0; }
long long caskx_in_str(void *slot)  { char *v = NULL;  if (cask_in_str(&v))  { *(const char **)slot = v; return 1; } return 0; }
long long caskx_in_bool(void *slot) { int v = 0;       if (cask_in_bool(&v)) { *(long long *)slot = (v != 0); return 1; } return 0; }

/* ---------------- exports: strings ---------------- */
const char *caskx_cat(const char *a, const char *b) { return cask_cat(a, b); }
long long   caskx_len(const char *s) { return cask_len(s); }
long long   caskx_val(const char *s) { return cask_val(s); }
const char *caskx_left(const char *s, long long n, const char *f, int l)  { return cask_left(s, n, f, l); }
const char *caskx_right(const char *s, long long n, const char *f, int l) { return cask_right(s, n, f, l); }
const char *caskx_mid(const char *s, long long st, long long n, const char *f, int l) { return cask_mid(s, st, n, f, l); }
const char *caskx_upper(const char *s) { return cask_upper(s); }
const char *caskx_lower(const char *s) { return cask_lower(s); }
const char *caskx_trim(const char *s)  { return cask_trim(s); }
long long   caskx_find(const char *s, const char *nd, long long st, const char *f, int l) { return cask_find(s, nd, st, f, l); }
const char *caskx_substitute(const char *s, const char *o, const char *nv) { return cask_substitute(s, o, nv); }
const char *caskx_rept(const char *s, long long n, const char *f, int l)   { return cask_rept(s, n, f, l); }
long long   caskx_exact(const char *a, const char *b) { return cask_exact(a, b); }
const char *caskx_char_code(long long n, const char *f, int l) { return cask_char_code(n, f, l); }
long long   caskx_code(const char *s) { return cask_code(s); }
long long   caskx_strcmp(const char *a, const char *b) { int r = strcmp(a ? a : "", b ? b : ""); return r < 0 ? -1 : (r == 0 ? 0 : 1); }

/* ---------------- exports: value -> text ---------------- */
const char *caskx_ftos(long long v)  { return cask_ftos(v); }
const char *caskx_ftos_r(double v)   { return cask_ftos_r(v); }
const char *caskx_ftos_b(long long v){ return cask_ftos_b((int)v); }
const char *caskx_ftos_c(long long v){ return cask_ftos_c((char)(signed char)v); }

/* ---------------- exports: math / Excel ---------------- */
double  caskx_random(void)      { return cask_random(); }
long long caskx_now(void)       { return cask_now(); }
long long caskx_sign(long long v)   { return cask_sign(v); }
long long caskx_sign_r(double v)    { return cask_sign_r(v); }
long long caskx_int(double v)       { return cask_int(v); }
double  caskx_xround(double v, long long n, const char *f, int l) { return cask_xround(v, n, f, l); }
long long caskx_xmod(long long a, long long b, const char *f, int l) { return cask_xmod(a, b, f, l); }
double  caskx_fxmod(double a, double b, const char *f, int l) { return cask_fxmod(a, b, f, l); }
double  caskx_ln(double x, const char *f, int l)      { return cask_ln(x, f, l); }
double  caskx_log10e(double x, const char *f, int l)  { return cask_log10e(x, f, l); }
double  caskx_logb(double x, double b, const char *f, int l) { return cask_logb(x, b, f, l); }
long long caskx_randbetween(long long lo, long long hi, const char *f, int l) { return cask_randbetween(lo, hi, f, l); }
long long caskx_sum(const long long *a, long long n, const char *f, int l)  { return cask_sum(a, n, f, l); }
double  caskx_sum_r(const double *a, long long n)      { return cask_sum_r(a, n); }
double  caskx_avg(const long long *a, long long n, const char *f, int l)    { return cask_avg(a, n, f, l); }
double  caskx_avg_r(const double *a, long long n, const char *f, int l)     { return cask_avg_r(a, n, f, l); }
long long caskx_max(const long long *a, long long n, const char *f, int l)  { return cask_max(a, n, f, l); }
long long caskx_min(const long long *a, long long n, const char *f, int l)  { return cask_min(a, n, f, l); }
double  caskx_max_r(const double *a, long long n, const char *f, int l)     { return cask_max_r(a, n, f, l); }
double  caskx_min_r(const double *a, long long n, const char *f, int l)     { return cask_min_r(a, n, f, l); }
long long caskx_max2(long long a, long long b) { return cask_max2(a, b); }
long long caskx_min2(long long a, long long b) { return cask_min2(a, b); }
double  caskx_max2_r(double a, double b) { return cask_max2_r(a, b); }
double  caskx_min2_r(double a, double b) { return cask_min2_r(a, b); }
double  caskx_atan2(double y, double x)  { return cask_atan2(y, x); }

/* ---------------- exports: rcl() easter egg ---------------- */
void      caskx_rcl_ask(void)          { cask_rcl_ask(); }
long long caskx_rcl_num(double n)      { return cask_rcl_num(n); }
long long caskx_rcl_text(const char *s){ return cask_rcl_text(s); }
