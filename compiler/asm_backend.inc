/* ============================================================
 * asm_backend.inc -- the C* assembler backend (full language)
 *
 *   .cask --front-end--> AST --this file--> x86-64 asm (AT&T)
 *             -> as -> ld (+ libcastrt.a + CRT) -> native .exe
 *
 * No C compiler is involved in building a user program.  The
 * runtime library (libcastrt.a) is pre-built once by build.bat
 * and holds the original cask_rt.h / C_OPED.cash / C_CSS.cash
 * implementations behind caskx_* / caskw_* exports, so runtime
 * semantics and error texts stay byte-identical with the old
 * C-translating backend.
 *
 * Speed design (tcc-style, see README):
 *   - locals/params live in the rbp frame (recursion works)
 *   - expression temporaries live on the machine stack via
 *     push/pop; the live depth is tracked at compile time so
 *     every call site restores 16-byte alignment exactly
 *   - hot integer checks are INLINED (add/sub/mul -> jo,
 *     div/mod -> test/cmp, index -> cmp/jae); only rare checks
 *     (shifts, char range, abs) call into the runtime
 *   - a tight loop like n = n + 1 compiles to 3 instructions
 *     with no calls at all
 * ============================================================ */

/* ---------------- state ---------------- */
static SBuf g_abuf;              /* .text */
static SBuf g_rbuf;              /* .rdata: file name, messages, string literals */
static SBuf g_dbuf;              /* .data : initialised globals */
static SBuf g_bbuf;              /* .bss  : zero-initialised globals */
static int   g_lbl = 0;          /* unique label counter */
static int   g_pdepth = 0;       /* live expression pushes (compile-time) */
static int   g_nslot = 0;        /* frame slots used in the current function */
static int   g_ferr = 0;         /* a backend error was reported */
static int   g_strid = 0;        /* string-literal counter */
static int   g_fnret = T_NUM;    /* return type of the function being emitted */
static int   g_instart = 0;      /* emitting the 'start' entry body */

/* tcc-style local caching: hot integer locals live in callee-saved
 * registers.  %rbx and %r12..%r15 are non-volatile on BOTH Windows x64
 * and SysV and are never argument registers, so a cached local survives
 * every call; each (recursive) invocation saves/restores its own set.  */
static const char *g_creg[5] = { "%rbx", "%r12", "%r13", "%r14", "%r15" };
static int g_ncreg = 0;      /* how many are in use in the current function */
static const char *creg_of(Sym *s) { return (s && s->areg > 0) ? g_creg[s->areg - 1] : NULL; }
static int cacheable_sym(Sym *s)
{
    return s && s->areg >= 0 && s->ndim == 0 &&
           (s->type == T_NUM || s->type == T_BOOL || s->type == T_LETTER);
}

/* die stubs: cold blocks appended after all functions */
typedef struct { int lbl; int line; int msg; } DieSite;
static DieSite *g_dies = NULL;
static int g_ndies = 0, g_cdies = 0;

/* message ids; the texts are the exact strings cask_rt.h uses, so
 * run-time error output stays byte-identical to the old backend   */
enum { M_ADD = 1, M_SUB, M_MUL, M_DIV0, M_DIVOVF, M_MOD0, M_IDX,
       M_SHRANGE, M_NEGSHL, M_SHLOVF, M_FDIV };

/* ---------------- small helpers ---------------- */
static void wa(const char *fmt, ...)
{
    va_list ap; char tmp[8192]; int n, i;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
    for (i = 0; i < n; i++) sb_putc(&g_abuf, tmp[i]);
}
static void wbuf(SBuf *b, const char *fmt, ...)
{
    va_list ap; char tmp[8192]; int n, i;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
    for (i = 0; i < n; i++) sb_putc(b, tmp[i]);
}
static int nlbl(void) { return ++g_lbl; }

/* append "<label>:" then the raw bytes as chunked `.byte` lines.
 * UTF-8 safe: no escaping problems, works for Chinese text.       */
static void wbytes(SBuf *b, const char *label, const char *s, size_t n)
{
    char tmp[96];
    size_t i;
    int col = 0;
    snprintf(tmp, sizeof tmp, "%s:\n", label);
    for (i = 0; tmp[i]; i++) sb_putc(b, tmp[i]);
    for (i = 0; i <= n; i++) {                     /* i == n appends the 0 */
        if (col == 0) {
            const char *kw = "    .byte ";
            for (; *kw; kw++) sb_putc(b, *kw);
        } else {
            sb_putc(b, ',');                       /* separator before the value,
                                                    * so a line never ends in a comma */
        }
        snprintf(tmp, sizeof tmp, "%u", i < n ? (unsigned)(unsigned char)s[i] : 0u);
        { size_t k; for (k = 0; tmp[k]; k++) sb_putc(b, tmp[k]); }
        if (++col == 16) { sb_putc(b, '\n'); col = 0; }
    }
    if (col != 0) sb_putc(b, '\n');
}

/* report an unsupported construct: no fallback, straight to the user */
static void berr(int line, int col, const char *fmt, ...)
{
    char msg[700]; va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    errs(SEV_SEVERE, line, col, "%s", msg);
    g_ferr = 1;
}

static unsigned long long d2bits(double d) { unsigned long long u; memcpy(&u, &d, 8); return u; }

/* ---- string literal pool (label per literal) ---- */
static int rstr(const char *s, size_t n)
{
    char lab[32];
    int id = g_strid++;
    snprintf(lab, sizeof lab, ".Ls%d", id);
    wbytes(&g_rbuf, lab, s, n);
    return id;
}

/* ---- die stub bookkeeping ---- */
static void die_add(int lbl, int line, int msg)
{
    if (g_ndies == g_cdies) {
        g_cdies = g_cdies ? g_cdies * 2 : 32;
        g_dies = (DieSite *)xrealloc(g_dies, sizeof(DieSite) * g_cdies);
    }
    g_dies[g_ndies].lbl = lbl; g_dies[g_ndies].line = line; g_dies[g_ndies].msg = msg;
    g_ndies++;
}

/* ---------------- frame slots ----------------
 * slot k occupies [rbp-8(k+1)];  slot_new(n) reserves n
 * contiguous slots and returns the offset of the first one
 * (the lowest address), so an array's element j lives at
 * off + 8*j.                                                        */
static int slot_new(int n)
{
    g_nslot += n;
    return -8 * g_nslot;
}

/* ---------------- operand pushes ----------------
 * every push is one 8-byte machine-stack slot; g_pdepth
 * counts the live ones so call sites can restore alignment. */
static void apush_rax(void)  { wa("    pushq %%rax\n"); g_pdepth++; }
static void apush_xmm0(void) { wa("    subq $8, %%rsp\n    movsd %%xmm0, (%%rsp)\n"); g_pdepth++; }
static void apush_cls(int real) { if (real) apush_xmm0(); else apush_rax(); }
static void apop_to(const char *reg) { wa("    popq %s\n", reg); g_pdepth--; }
static void apop_xmm(int n)  { wa("    popq %%rax\n    movq %%rax, %%xmm%d\n", n); g_pdepth--; }

/* ---------------- forward declarations ---------------- */
static void aexpr(Expr *e);
static void emit_conv(Type dst, Expr *v);          /* emit + convert (assign/args/return rules) */
static void emit_conv_reg(Type dst, Type src, int line);
static void emit_ftos_val(Expr *v);                /* value -> char* in rax (emit_ftos rules)   */
static void emit_truthy(Type vt);                  /* scalar -> 0/1 in rax                      */
static void av_addr(Expr *e);                      /* address of a variable in rax              */
static void aidx_addr(Expr *e);                    /* address of a[i][j] in rax (with checks)   */
static void aimm(const char *reg, long long v);    /* 64-bit-safe immediate move                */
static int asm_gid(const char *name);              /* global-variable index (defined below)     */

/* ============================================================
 * CALL MACHINERY
 *
 * Windows x64:  position i<4 -> rcx,rdx,r8,r9 (int) or xmm0-3
 * (float); positions >=4 on the stack at [rsp+32+8(i-4)] after
 * the 32-byte shadow space.  SysV: first 6 int args -> rdi..
 * r9, first 8 float -> xmm0-7, overflow in position order.
 *
 * All arguments are first evaluated and pushed (arg0 deepest),
 * then `sub rsp, S` makes room for shadow space + stack args,
 * args are copied/loaded into place, `call`, then everything is
 * released again.  S is chosen so rsp is 16-aligned at the call,
 * taking the compile-time push depth into account.
 * ============================================================ */

/* args already in registers; no stack args */
static void acall_regs(const char *sym)
{
    int S = 32;
    if ((S & 15) != ((g_pdepth & 1) ? 8 : 0)) S += 8;
    wa("    subq $%d, %%rsp\n", S);
    wa("    call %s\n", sym);
    wa("    addq $%d, %%rsp\n", S);
}

/* top-of-stack args (n pushes live, arg0 deepest) -> call */
static void acall_popped(const char *sym, const int *cls, int n, int setal)
{
    int i, S, pad = (g_pdepth & 1) ? 8 : 0;
#ifdef _WIN32
    int nstk = (n > 4) ? n - 4 : 0;
    static const char *ireg[4] = { "%rcx", "%rdx", "%r8", "%r9" };
    int nfp = 0;
    for (i = 0; i < n && i < 4; i++) if (cls[i]) nfp++;
    S = 32 + 8 * nstk;
    if ((S & 15) != pad) S += 8;
    if (S) wa("    subq $%d, %%rsp\n", S);
    for (i = 4; i < n; i++) {                      /* stack args, via rax (bit copy) */
        wa("    movq %d(%%rsp), %%rax\n    movq %%rax, %d(%%rsp)\n",
           S + 8 * (n - 1 - i), 32 + 8 * (i - 4));
    }
    for (i = 0; i < n && i < 4; i++) {             /* positional registers */
        if (cls[i]) wa("    movsd %d(%%rsp), %%xmm%d\n", S + 8 * (n - 1 - i), i);
        else        wa("    movq %d(%%rsp), %s\n",       S + 8 * (n - 1 - i), ireg[i]);
    }
    if (setal) wa("    movb $%d, %%al\n", nfp);
    wa("    call %s\n", sym);
    if (S || n) wa("    addq $%d, %%rsp\n", S + 8 * n);
    g_pdepth -= n;
#else
    /* SysV: classes counted separately; overflow in position order */
    static const char *ireg6[6] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
    int ic = 0, fc = 0, sc = 0, slot[64], stk[64], nstk = 0, nfp = 0;
    for (i = 0; i < n; i++) {
        if (cls[i]) { if (fc < 8) { slot[i] = fc++; stk[i] = -1; nfp++; } else { stk[i] = sc++; slot[i] = -1; } }
        else        { if (ic < 6) { slot[i] = ic++; stk[i] = -1; }          else { stk[i] = sc++; slot[i] = -1; } }
    }
    nstk = sc;
    S = 8 * nstk;
    if ((S & 15) != pad) S += 8;
    if (S) wa("    subq $%d, %%rsp\n", S);
    for (i = 0; i < n; i++) if (stk[i] >= 0)
        wa("    movq %d(%%rsp), %%rax\n    movq %%rax, %d(%%rsp)\n",
           S + 8 * (n - 1 - i), 8 * stk[i]);
    for (i = 0; i < n; i++) if (slot[i] >= 0) {
        if (cls[i]) wa("    movsd %d(%%rsp), %%xmm%d\n", S + 8 * (n - 1 - i), slot[i]);
        else        wa("    movq %d(%%rsp), %s\n",       S + 8 * (n - 1 - i), ireg6[slot[i]]);
    }
    if (setal) wa("    movb $%d, %%al\n", nfp);
    wa("    call %s\n", sym);
    if (S || n) wa("    addq $%d, %%rsp\n", S + 8 * n);
    g_pdepth -= n;
#endif
}

/* argument descriptor for asm_call */
typedef struct {
    int kind;       /* 0 = expression (convert to tgt), 1 = int imm,
                       2 = rdata label address, 3 = value already in rax,
                       4 = value already in xmm0 */
    Expr *e;
    Type tgt;
    long long iv;
    const char *lab;
} AArg;

static void asm_call(const char *sym, AArg *a, int n, int setal)
{
    int cls[64], i;
    if (n > 60) { berr(0, 0, "编译器内部：一次调用的参数太多"); return; }
    for (i = 0; i < n; i++) {
        switch (a[i].kind) {
        case 0: emit_conv(a[i].tgt, a[i].e); cls[i] = (a[i].tgt == T_REAL); break;
        case 1: wa("    movabsq $%lld, %%rax\n", a[i].iv); cls[i] = 0; break;
        case 2: wa("    leaq %s(%%rip), %%rax\n", a[i].lab); cls[i] = 0; break;
        case 3: cls[i] = 0; break;      /* rax already holds it */
        case 4: cls[i] = 1; break;      /* xmm0 already holds it */
        }
        apush_cls(cls[i]);
    }
    acall_popped(sym, cls, n, setal);
}

/* handy constructors */
static AArg A_e(Type tgt, Expr *e) { AArg a; a.kind = 0; a.e = e; a.tgt = tgt; a.iv = 0; a.lab = NULL; return a; }
static AArg A_i(long long v)       { AArg a; a.kind = 1; a.e = NULL; a.tgt = T_NUM; a.iv = v; a.lab = NULL; return a; }
static AArg A_l(const char *lab)   { AArg a; a.kind = 2; a.e = NULL; a.tgt = T_NUM; a.iv = 0; a.lab = lab; return a; }

/* the source path string (for run-time error locations) */
static char g_filelab[32];

/* ---------------- compile-time constant folding for emission ----
 * Integer constants are folded by the front-end (fold_const, or
 * has_cval).  Real/text constants keep their initialiser on the
 * Sym (cinit, recorded by analyze_const_init) and are folded here
 * at emission time.                                               */
static int fold_real_e(Expr *e, double *out)
{
    double a, b;
    if (!e) return 0;
    switch (e->k) {
    case E_REAL:  *out = e->dv; return 1;
    case E_NUM:   *out = (double)e->iv; return 1;
    case E_CHR:   *out = (double)(signed char)(unsigned char)e->iv; return 1;
    case E_TRUE:  *out = 1.0; return 1;
    case E_FALSE: *out = 0.0; return 1;
    case E_UN:
        if (e->sv[0] == '-') { if (!fold_real_e(e->a, &a)) return 0; *out = -a; return 1; }
        if (e->sv[0] == '+') return fold_real_e(e->a, out);
        if (e->sv[0] == '!') { if (!fold_real_e(e->a, &a)) return 0; *out = (a != 0) ? 1.0 : 0.0; return 1; }
        return 0;
    case E_BIN:
        if (!fold_real_e(e->a, &a) || !fold_real_e(e->b, &b)) return 0;
        if (!strcmp(e->sv, "+")) { *out = a + b; return 1; }
        if (!strcmp(e->sv, "-")) { *out = a - b; return 1; }
        if (!strcmp(e->sv, "*")) { *out = a * b; return 1; }
        if (!strcmp(e->sv, "/")) { if (b == 0) return 0; *out = a / b; return 1; }
        return 0;
    case E_VAR:
        if (e->sym && e->sym->kind == SK_CONST) {
            if (e->sym->cinit && e->sym->cinit != (struct Expr *)e)
                return fold_real_e((Expr *)e->sym->cinit, out);
            if (e->sym->has_cval) { *out = (double)e->sym->cval; return 1; }
        }
        return 0;
    default:
        return 0;
    }
}

static int fold_text_e(Expr *e, const char **s, size_t *n)
{
    if (!e) return 0;
    if (e->k == E_STR) { *s = e->sv; *n = e->svlen; return 1; }
    if (e->k == E_VAR && e->sym && e->sym->kind == SK_CONST && e->sym->cinit)
        return fold_text_e((Expr *)e->sym->cinit, s, n);
    return 0;
}

/* integer value of a constant (front-end fold_const, else cinit) */
static int fold_int_e(Expr *e, long long *out)
{
    if (!e) return 0;
    if (fold_const(e, out)) return 1;
    if (e->k == E_VAR && e->sym && e->sym->kind == SK_CONST) {
        if (e->sym->has_cval) { *out = e->sym->cval; return 1; }
        if (e->sym->cinit && e->sym->cinit != (struct Expr *)e)
            return fold_int_e((Expr *)e->sym->cinit, out);
    }
    return 0;
}

/* ---------------- truthiness ----------------
 * any scalar -> 0/1 in rax; NaN is true, exactly like C's
 * implicit conversion (x != 0).                                  */
static void emit_truthy(Type vt)
{
    if (vt == T_REAL) {
        wa("    xorl %%eax, %%eax\n");
        wa("    movq %%rax, %%xmm1\n");
        wa("    ucomisd %%xmm1, %%xmm0\n");
        wa("    setne %%al\n    setp %%cl\n    orb %%cl, %%al\n    movzbq %%al, %%rax\n");
    } else {
        wa("    testq %%rax, %%rax\n    setne %%al\n    movzbq %%al, %%rax\n");
    }
}

/* ---------------- conversions ----------------
 * emit_conv(): the C backend's emit_converted rules, used for
 * assignments, call arguments and return values:
 *   letter <- num   : runtime range check (caskx_ck_char)
 *   bool   <- other : truthiness (!= 0)
 *   real   <- int   : cvtsi2sd
 *   everything else : same 8-byte representation, pass through   */
/* runtime char range check on the value currently in rax */
static void ck_char_reg(int line)
{
    apush_rax();
    wa("    leaq %s(%%rip), %%rax\n", g_filelab);
    apush_rax();
    aimm("%rax", line);
    apush_rax();
    acall_popped("caskx_ck_char", (int[]){0,0,0}, 3, 0);
}

/* register-level conversion: value already in rax / xmm0 */
static void emit_conv_reg(Type dst, Type src, int line)
{
    if (dst == src || src == T_ERR || dst == T_ERR || dst == T_VOID) return;
    if (dst == T_LETTER && src == T_NUM) { ck_char_reg(line); return; }
    if (dst == T_BOOL && src != T_BOOL) { emit_truthy(src); return; }
    if (dst == T_REAL && src != T_REAL) { wa("    cvtsi2sd %%rax, %%xmm0\n"); return; }
}

static void emit_conv(Type dst, Expr *v)
{
    Type vt = v->t;
    aexpr(v);
    if (vt == T_ERR || dst == T_ERR || dst == T_VOID || dst == vt) return;
    if (dst == T_LETTER && vt == T_NUM) {
        /* literal already in range skips the check, exactly like the C backend */
        if (v->k == E_NUM && v->iv >= -128 && v->iv <= 127) return;
        ck_char_reg(v->line);
        return;
    }
    emit_conv_reg(dst, vt, v->line);
}

/* declaration initialiser: C's implicit conversion rules.
 * NOTE unlike emit_converted, bool is NOT normalised here: C* bool
 * is a plain C int, so `bool b = 5;` stores 5 -- byte-identical
 * with the old C-translating backend.                            */
static void emit_conv_decl(Type dst, Expr *v)
{
    Type vt = v->t;
    if (vt == T_ERR) { aexpr(v); return; }
    if (dst == T_REAL && vt != T_REAL) {
        aexpr(v);
        wa("    cvtsi2sd %%rax, %%xmm0\n");
        return;
    }
    if (dst == T_LETTER && vt != T_LETTER) {
        aexpr(v);
        if (vt == T_REAL) wa("    cvttsd2si %%xmm0, %%rax\n");
        wa("    movsbq %%al, %%rax\n");          /* C: char c = (char)value */
        return;
    }
    if ((dst == T_NUM || dst == T_BOOL) && vt == T_REAL) {
        aexpr(v);
        wa("    cvttsd2si %%xmm0, %%rax\n");      /* C: long long = double   */
        return;
    }
    aexpr(v);                                     /* identity */
}

/* value -> char* in rax (the C backend's emit_ftos) */
static void emit_ftos_val(Expr *v)
{
    aexpr(v);
    switch (v->t) {
    case T_BOOL:
        apush_rax();
        acall_popped("caskx_ftos_b", (int[]){0}, 1, 0);
        break;
    case T_NUM:
        apush_rax();
        acall_popped("caskx_ftos", (int[]){0}, 1, 0);
        break;
    case T_REAL:
        apush_xmm0();
        acall_popped("caskx_ftos_r", (int[]){1}, 1, 0);
        break;
    case T_LETTER:
        apush_rax();
        acall_popped("caskx_ftos_c", (int[]){0}, 1, 0);
        break;
    default:                                   /* T_TEXT: already a pointer */
        break;
    }
}

/* ---------------- die stubs ----------------
 * A checked site emits e.g. `jae .Ld17`; the cold stub for .Ld17 is
 * appended after all functions.  The stub resets rsp from rbp (so it
 * is 16-aligned no matter how deep expression pushes are), then calls
 * caskx_die(file, line, message) which prints the exact text and
 * exit(1).                                                           */
static int die_site(const char *jcc, int line, int msg)
{
    int lbl = nlbl();
    die_add(lbl, line, msg);
    wa("    %s .Ld%d\n", jcc, lbl);
    return lbl;
}

static const char *die_msg_id(int m)   /* .rdata label of a message */
{
    switch (m) {
    case M_ADD:    return "L_m1";
    case M_SUB:    return "L_m2";
    case M_MUL:    return "L_m3";
    case M_DIV0:   return "L_m4";
    case M_DIVOVF: return "L_m5";
    case M_MOD0:   return "L_m6";
    case M_IDX:    return "L_m7";
    case M_SHRANGE:return "L_m8";
    case M_NEGSHL: return "L_m9";
    case M_SHLOVF: return "L_m10";
    case M_FDIV:   return "L_m11";
    }
    return "L_m1";
}

/* immediate -> register (64-bit safe; hex avoids LLONG_MIN parse issues) */
static void aimm(const char *reg, long long v)
{
    if (v >= -2147483648LL && v <= 2147483647LL)
        wa("    movq $%lld, %s\n", v, reg);
    else
        wa("    movabsq $0x%llx, %s\n", (unsigned long long)v, reg);
}

static void ainxd(const char *reg, unsigned long long bits)  /* bits -> xmm reg */
{
    wa("    movabsq $0x%llx, %%rax\n", bits);
    wa("    movq %%rax, %s\n", reg);
}

/* ---------------- addresses ---------------- */
static void av_addr(Expr *e)     /* address of a variable -> rax */
{
    Sym *s = e->sym;
    if (!s) { berr(e->line, e->col, "编译器内部：变量未解析"); return; }
    if (s->is_global) wa("    leaq cask_g%d(%%rip), %%rax\n", asm_gid(s->name));
    else              wa("    leaq %d(%%rbp), %%rax\n", s->aoff);
}

/* address of a[i][j]... -> rax, with bounds checks mirroring the C
 * backend's cask_ckidx at every dimension (skipped by --fast or when
 * the index is an in-range literal, exactly like emit_c_expr).        */
static void aidx_addr(Expr *e)
{
    Expr *chain[8], *root, *n;
    int nl = 0, i;
    for (n = e; n && n->k == E_IDX && nl < 8; n = n->a) chain[nl++] = n;
    if (!n || n->k != E_VAR) { berr(e->line, e->col, "编译器内部：下标结构异常"); return; }
    root = n;
    av_addr(root);                     /* rax = base address */
    for (i = nl - 1; i >= 0; i--) {    /* inner -> outer */
        int lvl, dim, k;
        long long stride = 8;
        Expr *node = chain[i];
        apush_rax();                    /* park the running address */
        aexpr(node->b);                 /* rax = index */
        dim = node->dim;
        if (!g_fast && !node->litidx) {
            aimm("%rcx", (long long)dim);
            wa("    cmpq %%rcx, %%rax\n");    /* unsigned: negative index -> large */
            die_site("jae", node->line, M_IDX);
        }
        (void)dim;
        /* stride in bytes = 8 * product(dims[lvl+1 .. ndim-1]) */
        lvl = node->lvl;
        for (k = lvl + 1; k < root->sym->ndim; k++) {
            if (stride > 0x0FFFFFFFLL) { stride = -1; break; }
            stride *= root->sym->dims[k];
        }
        if (stride < 0) { berr(node->line, node->col, "数组太大：总元素数超出范围"); return; }
        if (stride != 1) {
            if (stride <= 2147483647LL && stride >= -2147483648LL)
                wa("    imulq $%lld, %%rax\n", stride);
            else {
                wa("    movabsq $%lld, %%rcx\n", stride);
                wa("    imulq %%rcx, %%rax\n");
            }
        }
        apop_to("%rcx");                /* rcx = running address */
        wa("    addq %%rcx, %%rax\n");  /* + index*stride */
    }
}

/* ---------------- symbol naming ----------------
 * globals live in .bss/.data as cask_g<name-id>; user functions keep
 * their ASCII name (so `on_click` binds to the weak runtime ref) and
 * non-ASCII names are mangled to cask_fn<position>.                */
static int asm_gid(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < g_nallnames; i++)
        if (strcmp(g_allnames[i], name) == 0) return i;
    return -1;
}
static int asm_fnid(Func *f)
{
    int i = 0; Func *x;
    if (!f || f->is_builtin) return -1;
    for (x = g_funcs; x; x = x->next, i++) if (x == f) return i;
    return -1;
}
static int is_ascii_name(const char *s)
{
    int first = 1;
    if (!s || !*s) return 0;
    for (; *s; s++, first = 0) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80) return 0;
        if (first) { if (!(isalpha(c) || c == '_')) return 0; }
        else       { if (!(isalnum(c) || c == '_')) return 0; }
    }
    return 1;
}
static const char *fn_sym(Func *f)
{
    static char buf[64];
    if (f->name && is_ascii_name(f->name)) return f->name;
    snprintf(buf, sizeof buf, "cask_fn%d", asm_fnid(f));
    return buf;
}

/* ---------------- scalar load / store ---------------- */
static void av_load(Expr *e)     /* value of a scalar variable -> rax / xmm0 */
{
    Sym *s = e->sym;
    const char *cr;
    if (!s) { berr(e->line, e->col, "编译器内部：变量未解析"); return; }
    if (s->type == T_REAL) {
        if (s->is_global) wa("    movsd cask_g%d(%%rip), %%xmm0\n", asm_gid(s->name));
        else              wa("    movsd %d(%%rbp), %%xmm0\n", s->aoff);
        return;
    }
    cr = creg_of(s);
    if (cr)                  wa("    movq %s, %%rax\n", cr);
    else if (s->is_global)   wa("    movq cask_g%d(%%rip), %%rax\n", asm_gid(s->name));
    else                     wa("    movq %d(%%rbp), %%rax\n", s->aoff);
}
static void astore_slot(Sym *s) /* value in rax / xmm0 -> variable slot */
{
    const char *cr;
    if (s->type == T_REAL) {
        if (s->is_global) wa("    movsd %%xmm0, cask_g%d(%%rip)\n", asm_gid(s->name));
        else              wa("    movsd %%xmm0, %d(%%rbp)\n", s->aoff);
        return;
    }
    cr = creg_of(s);
    if (cr)                  wa("    movq %%rax, %s\n", cr);
    else if (s->is_global)   wa("    movq %%rax, cask_g%d(%%rip)\n", asm_gid(s->name));
    else                     wa("    movq %%rax, %d(%%rbp)\n", s->aoff);
}

/* ---------------- expression emitter ----------------
 * value ends up in %rax (int/bool/letter/text pointer) or %xmm0
 * (real), according to e->t.  Node kinds not handled in the first
 * switch go to aexpr_ops() below.                                  */
static void aexpr_ops(Expr *e);   /* forward */
static void emit_assign(Expr *e, int want);
static int e_simple_op(Expr *e);
static void e_load_rcx(Expr *e);

static void aexpr(Expr *e)
{
    long long iv;
    double dv;
    const char *sv;
    size_t sn;

    if (!e) { berr(0, 0, "编译器内部：空表达式"); return; }

    switch (e->k) {
    case E_NUM:
        aimm("%rax", e->iv);
        return;
    case E_REAL:
        ainxd("%xmm0", d2bits(e->dv));
        return;
    case E_STR: {
        int id = rstr(e->sv, e->svlen);
        wa("    leaq .Ls%d(%%rip), %%rax\n", id);
        return;
    }
    case E_CHR:
        aimm("%rax", (long long)(signed char)(unsigned char)e->iv);
        return;
    case E_TRUE:  wa("    movq $1, %%rax\n"); return;
    case E_FALSE: wa("    movq $0, %%rax\n"); return;

    case E_VAR: {
        Sym *s = e->sym;
        if (!s) { berr(e->line, e->col, "编译器内部：变量未解析"); return; }
        if (s->kind == SK_CONST) {                 /* folded at compile time */
            Type t = s->type;
            if (t == T_TEXT) {
                if (!fold_text_e(e, &sv, &sn)) { berr(e->line, e->col, "常量 '%s' 的值必须能在编译期求出", s->name); return; }
                wa("    leaq .Ls%d(%%rip), %%rax\n", rstr(sv, sn));
                return;
            }
            if (t == T_REAL) {
                if (!fold_real_e(e, &dv)) { berr(e->line, e->col, "常量 '%s' 的值必须能在编译期求出", s->name); return; }
                ainxd("%xmm0", d2bits(dv));
                return;
            }
            if (!fold_int_e(e, &iv)) { berr(e->line, e->col, "常量 '%s' 的值必须能在编译期求出", s->name); return; }
            if (t == T_LETTER) iv = (long long)(signed char)(unsigned char)iv;
            aimm("%rax", iv);
            return;
        }
        if (s->ndim > 0) { av_addr(e); return; }   /* array decays to its address, like C */
        av_load(e);
        return;
    }

    case E_IDX: {
        Sym *s = e->sym;
        aidx_addr(e);
        if (s && s->type == T_REAL) wa("    movsd (%%rax), %%xmm0\n");
        else                         wa("    movq (%%rax), %%rax\n");
        return;
    }

    case E_LIST:
        berr(e->line, e->col, "编译器内部：'{...}' 只能出现在声明里");
        return;

    default:
        break;
    }

    aexpr_ops(e);
}

/* ++/-- on an lvalue.  Mirrors emit_un / emit_post exactly:
 *   real    : x = x ± 1 ; postfix result = (new) ∓ 1
 *   letter  : x = cask_ck_char(x ± 1, file, line)  -- ALWAYS, even --fast
 *   num/bool: fast -> raw ±1 ; else inline check (add/sub + jo die)
 * postfix result is new ∓ 1 (== old when no overflow, and the C backend
 * computes it the same way, so double rounding matches too).          */
static void incdec(Expr *t, int plus, int prefix, int line)
{
    int isreal, isletter;
    if (!t) return;
    isreal   = (t->t == T_REAL);
    isletter = (t->t == T_LETTER);
    if (t->k == E_VAR && t->sym && t->sym->ndim > 0) {
        berr(line, t->col, "数组不能使用 ++/--");
        return;
    }

    if (t->k == E_IDX) {
        aidx_addr(t);                  /* rax = address */
        apush_rax();                   /* park address */
    }

    /* load current value */
    if (t->k == E_IDX) {
        if (isreal) wa("    movsd (%%rax), %%xmm0\n");
        else        wa("    movq (%%rax), %%rax\n");
    } else {
        av_load(t);
    }

    /* compute new value in place */
    if (isreal) {
        ainxd("%xmm1", 0x3FF0000000000000ULL);      /* 1.0 */
        if (plus) wa("    addsd %%xmm1, %%xmm0\n");
        else      wa("    subsd %%xmm1, %%xmm0\n");
    } else if (isletter) {
        aimm("%rcx", 1);
        if (plus) wa("    addq %%rcx, %%rax\n");
        else      wa("    subq %%rcx, %%rax\n");
        ck_char_reg(line);                    /* letter: always checked, even --fast */
    } else {
        aimm("%rcx", 1);
        if (plus) wa("    addq %%rcx, %%rax\n");
        else      wa("    subq %%rcx, %%rax\n");
        if (!g_fast) die_site("jo", line, plus ? M_ADD : M_SUB);
    }

    /* store new value back */
    if (t->k == E_IDX) {
        /* parked address sits at (%rsp) */
        wa("    movq (%%rsp), %%rcx\n");
        if (isreal) wa("    movsd %%xmm0, (%%rcx)\n");
        else        wa("    movq %%rax, (%%rcx)\n");
    } else {
        astore_slot(t->sym);
    }

    /* postfix: result = new ∓ 1 (raw, exactly like the C backend) */
    if (!prefix) {
        if (isreal) {
            ainxd("%xmm1", 0x3FF0000000000000ULL);  /* 1.0 (xmm1 may have been clobbered) */
            if (plus) wa("    subsd %%xmm1, %%xmm0\n");
            else      wa("    addsd %%xmm1, %%xmm0\n");
        } else {
            aimm("%rcx", 1);
            if (plus) wa("    subq %%rcx, %%rax\n");
            else      wa("    addq %%rcx, %%rax\n");
        }
    }

    if (t->k == E_IDX) {
        wa("    addq $8, %%rsp\n");
        g_pdepth--;
    }
}

static void aexpr_bin(Expr *e);    /* forward */
static void aexpr_call(Expr *e);   /* forward */

static void aexpr_ops(Expr *e)
{
    switch (e->k) {

    case E_UN: {
        const char *op = e->sv;
        Expr *t = e->a;
        if (op[0] == '-' && op[1] == 0) {
            if (t->t == T_REAL) {
                aexpr(t);
                wa("    movabsq $0x8000000000000000, %%rax\n");
                wa("    movq %%rax, %%xmm1\n");
                wa("    xorpd %%xmm1, %%xmm0\n");
            } else if (g_fast) {
                aexpr(t);
                wa("    negq %%rax\n");
            } else {
                AArg a[4];
                a[0] = A_i(0);              /* cask_sub(0, x, file, line) */
                a[1] = A_e(T_NUM, t);
                a[2] = A_l(g_filelab);
                a[3] = A_i(e->line);
                asm_call("caskx_sub", a, 4, 0);
            }
            return;
        }
        if (op[0] == '+' && op[1] == 0) { aexpr(t); return; }
        if (op[0] == '!') {
            aexpr(t);
            emit_truthy(t->t);
            wa("    xorq $1, %%rax\n");
            return;
        }
        if (op[0] == '~') { aexpr(t); wa("    notq %%rax\n"); return; }
        /* prefix ++ / -- */
        incdec(t, op[0] == '+', 1, e->line);
        return;
    }

    case E_POST:
        incdec(e->a, e->sv[0] == '+', 0, e->line);
        return;

    case E_ASSIGN:
        emit_assign(e, 1);
        return;

    case E_BIN:  aexpr_bin(e);  return;
    case E_CALL: aexpr_call(e); return;

    default:
        berr(e->line, e->col, "编译器内部：暂不支持的表达式");
        return;
    }
}

/* assignment: want=1 keeps the assigned value in rax/xmm0 (expression
 * semantics); want=0 is the statement form where nobody reads it back. */
static void emit_assign(Expr *e, int want)
{
    Expr *t = e->a;
    int simple = (e->sv[0] == '=' && e->sv[1] == 0);
    if (t->k == E_VAR && t->sym && t->sym->ndim > 0) {
        berr(e->line, t->col, "数组不能整体赋值");
        return;
    }
    if (t->k != E_VAR && t->k != E_IDX) {
        berr(e->line, e->col, "编译器内部：赋值目标不是左值");
        return;
    }

    /* fast pattern: x = x OP y (and its compound spelling x OP= y), with
     * x cached in a register or living in .bss/.data -- operate in place,
     * no load/store round trip.  (x is a plain variable, so evaluating it
     * once more would give the same result.)                            */
    if (t->k == E_VAR && t->sym && t->sym->kind != SK_CONST &&
        t->sym->ndim == 0 && t->t == T_NUM &&
        e->b->k == E_BIN && e->b->t == T_NUM &&
        e->b->a->k == E_VAR && e->b->a->sym == t->sym && e->b->a->t == T_NUM) {
        const char *op2 = e->b->sv;
        const char *mn = NULL;
        int jmsg = -1;
        Expr *rhs = e->b->b;
        if (!strcmp(op2, "+")) { mn = "addq";  jmsg = M_ADD; }
        else if (!strcmp(op2, "-")) { mn = "subq";  jmsg = M_SUB; }
        else if (!strcmp(op2, "*")) { mn = "imulq"; jmsg = M_MUL; }
        else if (!strcmp(op2, "&")) mn = "andq";
        else if (!strcmp(op2, "|")) mn = "orq";
        else if (!strcmp(op2, "^")) mn = "xorq";
        if (mn && (rhs->t == T_NUM || rhs->t == T_BOOL || rhs->t == T_LETTER)) {
            const char *dst = creg_of(t->sym);
            char memdst[64];
            memdst[0] = 0;
            if (!dst && t->sym->is_global)
                snprintf(memdst, sizeof memdst, "cask_g%d(%%rip)", asm_gid(t->sym->name));
            if (dst || memdst[0]) {
                const char *d = dst ? dst : memdst;
                int imm_rhs = (rhs->k == E_NUM && rhs->iv >= -2147483648LL && rhs->iv <= 2147483647LL);
                if (dst) {
                    /* register destination: all two-operand ops are fine */
                    if (imm_rhs) wa("    %s $%lld, %s\n", mn, rhs->iv, d);
                    else if (e_simple_op(rhs))
                        { e_load_rcx(rhs); wa("    %s %%rcx, %s\n", mn, d); }
                    else goto generic;
                } else {
                    /* memory destination: imul has no memory form -> load/op/store */
                    if (!strcmp(mn, "imulq")) {
                        wa("    movq %s, %%rax\n", memdst);
                        if (imm_rhs) wa("    imulq $%lld, %%rax\n", rhs->iv);
                        else if (e_simple_op(rhs))
                            { e_load_rcx(rhs); wa("    imulq %%rcx, %%rax\n"); }
                        else goto generic;
                        if (jmsg >= 0 && !g_fast) die_site("jo", e->line, jmsg);
                        wa("    movq %%rax, %s\n", memdst);
                        if (want) { /* rax already holds the value */ }
                        return;
                    }
                    if (imm_rhs) wa("    %s $%lld, %s\n", mn, rhs->iv, memdst);
                    else if (e_simple_op(rhs))
                        { e_load_rcx(rhs); wa("    %s %%rcx, %s\n", mn, memdst); }
                    else goto generic;
                }
                if (jmsg >= 0 && !g_fast) die_site("jo", e->line, jmsg);
                if (want) {
                    if (dst) wa("    movq %s, %%rax\n", dst);
                    else     wa("    movq %s, %%rax\n", memdst);
                }
                return;
            }
        }
    }

generic:
    if (simple) {
        emit_conv(t->t, e->b);
    } else {
        aexpr(e->b);                    /* compound: shared target loaded inside the op */
        emit_conv_reg(t->t, e->b->t, e->line);
    }
    if (t->k == E_VAR) {
        astore_slot(t->sym);
    } else {
        /* value in rax/xmm0: park it, compute the address, store, restore */
        if (t->t == T_REAL) apush_xmm0();
        else                apush_rax();
        aidx_addr(t);
        wa("    popq %%rcx\n");
        g_pdepth--;
        wa("    movq %%rcx, (%%rax)\n");     /* 8 bytes: int or double bits */
        wa("    movq %%rcx, %%rax\n");
        if (t->t == T_REAL) wa("    movq %%rax, %%xmm0\n");
    }
}

/* ---------------- binary operators ---------------- */

static void aexpr_bin_real(Expr *e);   /* forward */
static void aexpr_bin_int(Expr *e);    /* forward */
static int is_cmpop(const char *op);   /* forward */

/* an operand that can be loaded without calls or operand parking:
 * literals and scalar variables (constants fold to immediates) */
static int e_simple_op(Expr *e)
{
    if (!e) return 0;
    switch (e->k) {
    case E_NUM: case E_CHR: case E_TRUE: case E_FALSE:
        return 1;
    case E_VAR:
        return e->sym && e->sym->ndim == 0;
    default:
        return 0;
    }
}

/* load a simple operand directly into %rcx (no push/pop round trip) */
static void e_load_rcx(Expr *e)
{
    Sym *s = e->sym;
    const char *cr;
    if (e->k == E_TRUE)  { aimm("%rcx", 1); return; }
    if (e->k == E_FALSE) { aimm("%rcx", 0); return; }
    if (e->k == E_VAR && s && s->kind == SK_CONST) {
        long long iv = 0;
        if (!fold_int_e(e, &iv)) { berr(e->line, e->col, "常量 '%s' 的值必须能在编译期求出", s->name); iv = 0; }
        if (s->type == T_LETTER) iv = (long long)(signed char)(unsigned char)iv;
        aimm("%rcx", iv);
        return;
    }
    if (e->k == E_VAR && s && s->type != T_REAL) {
        cr = creg_of(s);
        if (cr)                wa("    movq %s, %%rcx\n", cr);
        else if (s->is_global) wa("    movq cask_g%d(%%rip), %%rcx\n", asm_gid(s->name));
        else                   wa("    movq %d(%%rbp), %%rcx\n", s->aoff);
        return;
    }
    /* literal: sign-extend char bytes exactly like aexpr does */
    if (e->k == E_CHR)
        aimm("%rcx", (long long)(signed char)(unsigned char)e->iv);
    else
        aimm("%rcx", e->iv);
}

/* int operand dance: rax = left, rcx = right.
 * Simple right-hand sides skip the push/pop + xchg round trip entirely. */
static void bin_prelude(Expr *e)
{
    aexpr(e->a);                              /* left -> rax */
    if (e_simple_op(e->b)) { e_load_rcx(e->b); return; }
    apush_rax();
    aexpr(e->b);
    apop_to("%rcx");
    wa("    xchgq %%rcx, %%rax\n");           /* rax = left, rcx = right */
}

/* ---- jump straight on the flags (kills setcc -> test -> setcc -> test) ---- */
static const char *cc_true(const char *op)
{
    if (!strcmp(op, "==")) return "je";
    if (!strcmp(op, "!=")) return "jne";
    if (!strcmp(op, "<"))  return "jl";
    if (!strcmp(op, "<=")) return "jle";
    if (!strcmp(op, ">"))  return "jg";
    return "jge";                             /* ">=" */
}
static const char *cc_false(const char *op)
{
    if (!strcmp(op, "==")) return "jne";
    if (!strcmp(op, "!=")) return "je";
    if (!strcmp(op, "<"))  return "jge";
    if (!strcmp(op, "<=")) return "jg";
    if (!strcmp(op, ">"))  return "jle";
    return "jl";
}

/* emit `e`, then jump to lbl when the expression is true/false.
 * Comparisons jump directly on the flags; anything else gets a
 * truthiness test first.  Used by if/while/until/do/for/IF().   */
static void acond_jmp(Expr *e, int jump_if_true, int lbl)
{
    if (e && e->k == E_BIN && is_cmpop(e->sv)) {
        Type ta = e->a->t, tb = e->b->t;
        if (ta != T_TEXT && tb != T_TEXT && ta != T_REAL && tb != T_REAL) {
            /* integer comparison: operands -> flags -> jcc */
            if (e->b->k == E_NUM && e->b->iv >= -2147483648LL && e->b->iv <= 2147483647LL) {
                aexpr(e->a);
                wa("    cmpq $%lld, %%rax\n", e->b->iv);
            } else {
                bin_prelude(e);
                wa("    cmpq %%rcx, %%rax\n");
            }
            wa("    %s .L%d\n", jump_if_true ? cc_true(e->sv) : cc_false(e->sv), lbl);
            return;
        }
        /* text / float comparisons still produce a 0/1 value */
        aexpr(e);
        wa("    testq %%rax, %%rax\n");
        wa("    %s .L%d\n", jump_if_true ? "jne" : "je", lbl);
        return;
    }
    if (!e) { berr(0, 0, "编译器内部：空条件"); return; }
    aexpr(e);
    emit_truthy(e->t);
    wa("    testq %%rax, %%rax\n");
    wa("    %s .L%d\n", jump_if_true ? "jne" : "je", lbl);
}


static void setcc_bin(const char *op)    /* flags already set for left - right */
{
    const char *cc = NULL;
    if      (!strcmp(op, "==")) cc = "sete";
    else if (!strcmp(op, "!=")) cc = "setne";
    else if (!strcmp(op, "<"))  cc = "setl";
    else if (!strcmp(op, "<=")) cc = "setle";
    else if (!strcmp(op, ">"))  cc = "setg";
    else if (!strcmp(op, ">=")) cc = "setge";
    if (!cc) return;
    wa("    %s %%al\n", cc);
    wa("    movzbq %%al, %%rax\n");
}

static int is_cmpop(const char *op)
{
    return !strcmp(op,"==") || !strcmp(op,"!=") || !strcmp(op,"<") ||
           !strcmp(op,"<=") || !strcmp(op,">")  || !strcmp(op,">=");
}

static void aexpr_bin(Expr *e)
{
    const char *op = e->sv;
    Type ta = e->a->t, tb = e->b->t;
    int oplen = (int)strlen(op);

    /* ---- text concat: cask_cat(a, b) with per-side number->text ---- */
    if (oplen == 1 && op[0] == '+' && (ta == T_TEXT || tb == T_TEXT)) {
        if (ta == T_TEXT) aexpr(e->a); else emit_ftos_val(e->a);
        apush_rax();                        /* arg0 */
        if (tb == T_TEXT) aexpr(e->b); else emit_ftos_val(e->b);
        apush_rax();                        /* arg1 */
        acall_popped("caskx_cat", (int[]){0, 0}, 2, 0);
        return;
    }

    /* ---- text comparison: strcmp(a, b) op 0 ---- */
    if (ta == T_TEXT || tb == T_TEXT) {
        if (ta == T_TEXT && tb == T_TEXT && is_cmpop(op)) {
            AArg a[2];
            const char *cc = NULL;
            a[0] = A_e(T_TEXT, e->a);
            a[1] = A_e(T_TEXT, e->b);
            asm_call("caskx_strcmp", a, 2, 0);   /* rax = -1 / 0 / 1 */
            wa("    testq %%rax, %%rax\n");
            if      (!strcmp(op, "==")) cc = "sete";
            else if (!strcmp(op, "!=")) cc = "setne";
            else if (!strcmp(op, "<"))  cc = "setl";
            else if (!strcmp(op, "<=")) cc = "setle";
            else if (!strcmp(op, ">"))  cc = "setg";
            else if (!strcmp(op, ">=")) cc = "setge";
            if (cc) { wa("    %s %%al\n", cc); wa("    movzbq %%al, %%rax\n"); }
            return;
        }
        berr(e->line, e->col, "编译器内部：文本不支持该运算符");
        return;
    }

    /* ---- && || short circuit (result 0/1 in rax) ---- */
    if (oplen == 2 && op[0] == '&' && op[1] == '&') {
        int lfalse = nlbl(), lend = nlbl();
        aexpr(e->a); emit_truthy(ta);
        wa("    testq %%rax, %%rax\n");
        wa("    je .L%d\n", lfalse);
        aexpr(e->b); emit_truthy(tb);       /* rax = 0/1, the result */
        wa("    jmp .L%d\n", lend);
        wa(".L%d:\n", lfalse);
        wa("    xorl %%eax, %%eax\n");
        wa(".L%d:\n", lend);
        return;
    }
    if (oplen == 2 && op[0] == '|' && op[1] == '|') {
        int ltrue = nlbl(), lend = nlbl();
        aexpr(e->a); emit_truthy(ta);
        wa("    testq %%rax, %%rax\n");
        wa("    jne .L%d\n", ltrue);
        aexpr(e->b); emit_truthy(tb);
        wa("    jmp .L%d\n", lend);
        wa(".L%d:\n", ltrue);
        wa("    movq $1, %%rax\n");
        wa(".L%d:\n", lend);
        return;
    }

    /* ---- floating point path ---- */
    if (ta == T_REAL || tb == T_REAL) {
        aexpr_bin_real(e);
        return;
    }

    /* ---- integer path ---- */
    aexpr_bin_int(e);
}

/* floating point: xmm0 = left, xmm1 = right after staging;
 * comparisons arrange operands so the setcc pattern stays NaN-safe
 * (ordered-only for < <= > >=, and ==/!= handle unordered like C). */
static void aexpr_bin_real(Expr *e)
{
    const char *op = e->sv;
    Type ta = e->a->t, tb = e->b->t;
    int line = e->line;

    if (!strcmp(op, "/") && !g_fast) {
        /* cask_fdiv inline: b == 0.0 dies; NaN divisor passes through (a
         * == 0.0 is false for NaN, exactly like the C original)            */
        int lok = nlbl();
        aexpr(e->a);
        emit_conv_reg(T_REAL, ta, line);
        apush_xmm0();
        aexpr(e->b);
        emit_conv_reg(T_REAL, tb, line);
        wa("    movsd %%xmm0, %%xmm1\n");           /* xmm1 = right (divisor) */
        apop_xmm(0);                                /* xmm0 = left  (dividend) */
        wa("    xorpd %%xmm2, %%xmm2\n");
        wa("    ucomisd %%xmm2, %%xmm1\n");
        wa("    jp .L%d\n", lok);                   /* unordered -> NaN divisor is fine */
        die_site("je", line, M_FDIV);               /* exactly 0.0 -> "除以零（float 除法）" */
        wa(".L%d:\n", lok);
        wa("    divsd %%xmm1, %%xmm0\n");
        return;
    }

    aexpr(e->a);
    emit_conv_reg(T_REAL, ta, line);
    apush_xmm0();                               /* park left */
    aexpr(e->b);
    emit_conv_reg(T_REAL, tb, line);
    wa("    movsd %%xmm0, %%xmm1\n");           /* xmm1 = right */
    apop_xmm(0);                                /* xmm0 = left */

    if (is_cmpop(op)) {
        int left_first = 1;
        if (!strcmp(op, "<") || !strcmp(op, "<=")) left_first = 0;
        if (left_first) wa("    ucomisd %%xmm1, %%xmm0\n");   /* flags: left  ? right */
        else            wa("    ucomisd %%xmm0, %%xmm1\n");   /* flags: right ? left  */
        if (!strcmp(op, "==")) {
            wa("    sete %%al\n    setnp %%cl\n    andb %%cl, %%al\n");
            wa("    movzbq %%al, %%rax\n");
        } else if (!strcmp(op, "!=")) {
            wa("    setne %%al\n    setp %%cl\n    orb %%cl, %%al\n");
            wa("    movzbq %%al, %%rax\n");
        } else {
            const char *cc = NULL;
            /* right-operand comparisons use seta/setae on the swapped flags */
            if      (!strcmp(op, "<"))  cc = "seta";
            else if (!strcmp(op, "<=")) cc = "setae";
            else if (!strcmp(op, ">"))  cc = "seta";
            else if (!strcmp(op, ">=")) cc = "setae";
            if (cc) { wa("    %s %%al\n", cc); wa("    movzbq %%al, %%rax\n"); }
        }
        return;
    }

    if (!strcmp(op, "+")) { wa("    addsd %%xmm1, %%xmm0\n"); return; }
    if (!strcmp(op, "-")) { wa("    subsd %%xmm1, %%xmm0\n"); return; }
    if (!strcmp(op, "*")) { wa("    mulsd %%xmm1, %%xmm0\n"); return; }
    if (!strcmp(op, "/")) { wa("    divsd %%xmm1, %%xmm0\n"); return; }   /* --fast only */
    berr(line, 0, "编译器内部：float 不支持该运算符 '%s'", op);
}

/* integer ops: rax = left, rcx = right after bin_prelude */
static void aexpr_bin_int(Expr *e)
{
    const char *op = e->sv;
    int line = e->line;

    /* checked shifts: inline with the exact checks of cask_shl / cask_shr
     * (gcc -O2 used to inline those calls too, so this keeps parity) */
    if (!g_fast && (!strcmp(op, "<<") || !strcmp(op, ">>"))) {
        int sk1 = nlbl(), sk2 = nlbl();
        int is_shl = (op[0] == '<');
        bin_prelude(e);                          /* rax = a, rcx = b */
        /* b < 0 || b > 63 */
        wa("    testq %%rcx, %%rcx\n");
        die_site("js", line, M_SHRANGE);
        wa("    cmpq $63, %%rcx\n");
        die_site("jg", line, M_SHRANGE);
        if (is_shl) {
            /* a < 0 && b > 0 */
            wa("    testq %%rcx, %%rcx\n");
            wa("    jle .L%d\n", sk1);
            wa("    testq %%rax, %%rax\n");
            die_site("js", line, M_NEGSHL);
            wa(".L%d:\n", sk1);
            /* b > 0 && a > (LLONG_MAX >> b) */
            wa("    testq %%rcx, %%rcx\n");
            wa("    jle .L%d\n", sk2);
            wa("    movabsq $0x7fffffffffffffff, %%rdx\n");
            wa("    sarq %%cl, %%rdx\n");
            wa("    cmpq %%rdx, %%rax\n");
            die_site("jg", line, M_SHLOVF);
            wa(".L%d:\n", sk2);
            wa("    shlq %%cl, %%rax\n");
        } else {
            wa("    sarq %%cl, %%rax\n");
        }
        return;
    }

    bin_prelude(e);

    if (!strcmp(op, "+")) {
        wa("    addq %%rcx, %%rax\n");
        if (!g_fast) die_site("jo", line, M_ADD);
        return;
    }
    if (!strcmp(op, "-")) {
        wa("    subq %%rcx, %%rax\n");
        if (!g_fast) die_site("jo", line, M_SUB);
        return;
    }
    if (!strcmp(op, "*")) {
        wa("    imulq %%rcx, %%rax\n");
        if (!g_fast) die_site("jo", line, M_MUL);
        return;
    }
    if (!strcmp(op, "/")) {
        if (g_fast) { wa("    cqto\n    idivq %%rcx\n"); return; }
        int lok = nlbl();
        wa("    testq %%rcx, %%rcx\n");
        die_site("je", line, M_DIV0);
        wa("    cmpq $-1, %%rcx\n");
        wa("    jne .L%d\n", lok);
        aimm("%rdx", (-9223372036854775807LL - 1));     /* LLONG_MIN */
        wa("    cmpq %%rdx, %%rax\n");
        die_site("je", line, M_DIVOVF);                  /* MIN / -1 overflows */
        wa(".L%d:\n", lok);
        wa("    cqto\n    idivq %%rcx\n");
        return;
    }
    if (!strcmp(op, "%")) {
        if (g_fast) { wa("    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n"); return; }
        int lok = nlbl(), ldone = nlbl();
        wa("    testq %%rcx, %%rcx\n");
        die_site("je", line, M_MOD0);
        /* b == -1: the remainder is always 0; this also keeps
         * LLONG_MIN % -1 away from idiv's #DE (cask_mod returns 0 too) */
        wa("    cmpq $-1, %%rcx\n");
        wa("    jne .L%d\n", lok);
        wa("    xorl %%eax, %%eax\n");
        wa("    jmp .L%d\n", ldone);
        wa(".L%d:\n", lok);
        wa("    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n");
        wa(".L%d:\n", ldone);
        return;
    }
    if (!strcmp(op, "<<")) { wa("    shlq %%cl, %%rax\n"); return; }
    if (!strcmp(op, ">>")) { wa("    sarq %%cl, %%rax\n"); return; }
    if (!strcmp(op, "&"))  { wa("    andq %%rcx, %%rax\n"); return; }
    if (!strcmp(op, "|"))  { wa("    orq %%rcx, %%rax\n");  return; }
    if (!strcmp(op, "^"))  { wa("    xorq %%rcx, %%rax\n"); return; }
    if (is_cmpop(op)) {
        wa("    cmpq %%rcx, %%rax\n");       /* left - right, signed */
        setcc_bin(op);
        return;
    }
    berr(line, 0, "编译器内部：不认识的运算符 '%s'", op);
}

/* ---------------- calls ---------------- */

static void aexpr_builtin(Expr *e);   /* forward */

static void aexpr_call(Expr *e)
{
    EList *l;
    int n = 0;
    AArg a[64];

    /* builtin */
    if (e->fn && e->fn->is_builtin) { aexpr_builtin(e); return; }

    /* declared C function (only reachable with `use "C_OPED.cash"`):
     * signature unknown, so pass each argument in its C* type --
     * positional mapping matches the Windows x64 C ABI for the
     * int/double cases the toolkit actually uses.                   */
    if (e->native_sym && !e->napi) {
        for (l = e->args; l && n < 60; l = l->next, n++)
            a[n] = A_e(l->e->t, l->e);
        asm_call(e->sv, a, n, 1);      /* setal: varargs (printf-style) */
        return;
    }

    /* toolkit: caskw_<name>, arguments converted per the table */
    if (e->native_sym && e->napi) {
        char sym[160];
        snprintf(sym, sizeof sym, "caskw_%s", e->sv);
        for (l = e->args; l && n < 60; l = l->next, n++)
            a[n] = A_e(n < e->napi->npar ? e->napi->par[n] : l->e->t, l->e);
        asm_call(sym, a, n, 0);
        return;
    }

    /* user function */
    if (e->fn) {
        /* snapshot the symbol: staging the arguments re-enters aexpr and
         * fn_sym uses a single static buffer for mangled names          */
        char name[72];
        snprintf(name, sizeof name, "%s", fn_sym(e->fn));
        for (l = e->args; l && n < 60; l = l->next, n++)
            a[n] = A_e(n < e->fn->nparam ? e->fn->pt[n] : l->e->t, l->e);
        asm_call(name, a, n, 0);
        return;
    }

    berr(e->line, e->col, "编译器内部：无法解析的函数调用 '%s'", e->sv ? e->sv : "?");
}

/* MAX(a, b, c) -> cask_max2(a, cask_max2(b, c)): right-nested, each
 * value evaluated exactly once -- mirrors emit_minmax_args.          */
static void emit_minmax_chain(EList *l, int is_max, int real)
{
    if (!l->next) {
        emit_conv(real ? T_REAL : T_NUM, l->e);
        return;
    }
    if (real) { emit_conv(T_REAL, l->e); apush_xmm0(); }
    else      { emit_conv(T_NUM,  l->e); apush_rax(); }
    emit_minmax_chain(l->next, is_max, real);
    if (real) {
        wa("    movsd %%xmm0, %%xmm1\n");       /* second -> xmm1 */
        wa("    movq (%%rsp), %%rax\n    movq %%rax, %%xmm0\n");
        wa("    addq $8, %%rsp\n");
        g_pdepth--;
        acall_regs(is_max ? "caskx_max2_r" : "caskx_min2_r");   /* xmm0, xmm1 */
    } else {
#ifdef _WIN32
        wa("    movq %%rax, %%rdx\n");           /* second -> rdx */
        wa("    popq %%rcx\n");
#else
        wa("    movq %%rax, %%rsi\n");           /* SysV: arg2 = rsi */
        wa("    popq %%rdi\n");
#endif
        g_pdepth--;
        acall_regs(is_max ? "caskx_max2" : "caskx_min2");       /* arg0, arg1 */
    }
}

/* CONCAT(a, b, c) -> cask_cat(a, cask_cat(b, c)); non-text sides are
 * converted with the emit_ftos rules -- mirrors emit_concat_args.    */
static void emit_concat_chain(EList *l)
{
    if (!l->next) { emit_ftos_val(l->e); return; }
    emit_ftos_val(l->e);
    apush_rax();
    emit_concat_chain(l->next);
#ifdef _WIN32
    wa("    movq %%rax, %%rdx\n");
    wa("    popq %%rcx\n");
#else
    wa("    movq %%rax, %%rsi\n");
    wa("    popq %%rdi\n");
#endif
    g_pdepth--;
    acall_regs("caskx_cat");
}

static void aexpr_builtin(Expr *e)
{
    Func *f = e->fn;
    EList *l = e->args;
    int line = e->line;

    switch (f->bid) {

    case BI_LEN: {
        AArg a[1];
        a[0] = A_e(l->e->t, l->e);
        asm_call("caskx_len", a, 1, 0);
        break;
    }
    case BI_AT: {
        AArg a[4];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_NUM, l->next->e);
        a[2] = A_l(g_filelab);
        a[3] = A_i(line);
        asm_call("caskx_at", a, 4, 0);
        break;
    }
    case BI_ABS:
        if (l->e->t == T_REAL) {
            aexpr(l->e);
            wa("    movabsq $0x7fffffffffffffff, %%rax\n");
            wa("    movq %%rax, %%xmm1\n");
            wa("    andpd %%xmm1, %%xmm0\n");      /* fabs, inlined */
        } else {
            AArg a[3];
            a[0] = A_e(T_NUM, l->e);
            a[1] = A_l(g_filelab);
            a[2] = A_i(line);
            asm_call("caskx_abs", a, 3, 0);
        }
        break;

    case BI_FLOOR: case BI_CEIL: case BI_ROUND: case BI_TRUNC: {
        /* C: (long long)floor(x) etc. -- call libm, truncate toward zero */
        AArg a[1];
        a[0] = A_e(T_REAL, l->e);
        asm_call(bi_cname(f->bid), a, 1, 0);
        wa("    cvttsd2si %%xmm0, %%rax\n");
        break;
    }
    case BI_RANDOM:
        acall_popped("caskx_random", NULL, 0, 0);
        break;
    case BI_NOW:
        acall_popped("caskx_now", NULL, 0, 0);
        break;

    /* ---- Excel-style functions ---- */
    case BI_IF: {
        Expr *ca = l->e, *ya = l->next->e, *na = l->next->next->e;
        int lno = nlbl(), lend = nlbl();
        acond_jmp(ca, 0, lno);
        if (e->t == T_TEXT) emit_ftos_val(ya); else emit_conv(e->t, ya);
        wa("    jmp .L%d\n", lend);
        wa(".L%d:\n", lno);
        if (e->t == T_TEXT) emit_ftos_val(na); else emit_conv(e->t, na);
        wa(".L%d:\n", lend);
        break;
    }
    case BI_AND: {
        int lfalse = nlbl(), lend = nlbl();
        EList *it;
        for (it = e->args; it; it = it->next) {
            aexpr(it->e);
            emit_truthy(it->e->t);
            wa("    testq %%rax, %%rax\n");
            if (it->next) wa("    je .L%d\n", lfalse);
            else          wa("    jmp .L%d\n", lend);
        }
        wa(".L%d:\n", lfalse);
        wa("    xorl %%eax, %%eax\n");
        wa(".L%d:\n", lend);
        break;
    }
    case BI_OR: {
        int ltrue = nlbl(), lend = nlbl();
        EList *it;
        for (it = e->args; it; it = it->next) {
            aexpr(it->e);
            emit_truthy(it->e->t);
            wa("    testq %%rax, %%rax\n");
            if (it->next) wa("    jne .L%d\n", ltrue);
            else          wa("    jmp .L%d\n", lend);
        }
        wa(".L%d:\n", ltrue);
        wa("    movq $1, %%rax\n");
        wa(".L%d:\n", lend);
        break;
    }
    case BI_NOT:
        aexpr(l->e);
        emit_truthy(l->e->t);
        wa("    xorq $1, %%rax\n");
        break;
    case BI_XTRUE:  wa("    movq $1, %%rax\n"); break;
    case BI_XFALSE: wa("    movq $0, %%rax\n"); break;
    case BI_SIGN: {
        AArg a[1];
        if (l->e->t == T_REAL) {
            a[0] = A_e(T_REAL, l->e);
            asm_call("caskx_sign_r", a, 1, 0);
        } else {
            a[0] = A_e(T_NUM, l->e);
            asm_call("caskx_sign", a, 1, 0);
        }
        break;
    }
    case BI_XMOD:
        if (g_fast && e->t == T_NUM) {           /* --fast: plain remainder */
            aexpr(l->e);
            apush_rax();
            aexpr(l->next->e);
            apop_to("%rcx");
            wa("    xchgq %%rcx, %%rax\n");
            wa("    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n");
        } else if (e->t == T_REAL) {
            AArg a[4];
            a[0] = A_e(T_REAL, l->e);
            a[1] = A_e(T_REAL, l->next->e);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call("caskx_fxmod", a, 4, 0);
        } else {
            /* cask_xmod inline: die on b == 0, b == -1 -> 0, then the Excel
             * sign adjust (result takes the sign of the divisor)          */
            int lnorm = nlbl(), ldone = nlbl();
            aexpr(l->e);
            apush_rax();
            aexpr(l->next->e);
            apop_to("%rcx");
            wa("    xchgq %%rcx, %%rax\n");          /* rax = a, rcx = b */
            wa("    testq %%rcx, %%rcx\n");
            die_site("je", line, M_DIV0);            /* cask_xmod: "除以零" */
            wa("    cmpq $-1, %%rcx\n");
            wa("    jne .L%d\n", lnorm);
            wa("    xorl %%eax, %%eax\n");           /* b == -1 -> 0 */
            wa("    jmp .L%d\n", ldone);
            wa(".L%d:\n", lnorm);
            wa("    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n");
            wa("    testq %%rax, %%rax\n");
            wa("    je .L%d\n", ldone);
            wa("    movq %%rax, %%rdx\n    xorq %%rcx, %%rdx\n");
            wa("    testq %%rdx, %%rdx\n");
            wa("    jns .L%d\n", ldone);
            wa("    addq %%rcx, %%rax\n");
            wa(".L%d:\n", ldone);
        }
        break;
    case BI_XROUND:
        if (!l->next) {                            /* ROUND(x): (long long)round(x) */
            AArg a[1];
            a[0] = A_e(T_REAL, l->e);
            asm_call("round", a, 1, 0);
            wa("    cvttsd2si %%xmm0, %%rax\n");
        } else {                                    /* ROUND(x, digits) -> double */
            AArg a[4];
            a[0] = A_e(T_REAL, l->e);
            a[1] = A_e(T_NUM, l->next->e);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call("caskx_xround", a, 4, 0);
        }
        break;
    case BI_INT:
        if (l->e->t == T_REAL) {
            AArg a[1];
            a[0] = A_e(T_REAL, l->e);
            asm_call("caskx_int", a, 1, 0);
        } else {
            aexpr(l->e);                            /* whole numbers stay as they are */
        }
        break;
    case BI_XLN: {
        AArg a[3];
        a[0] = A_e(T_REAL, l->e);
        a[1] = A_l(g_filelab);
        a[2] = A_i(line);
        asm_call("caskx_ln", a, 3, 0);
        break;
    }
    case BI_XLOG: {
        if (l->next) {
            AArg a[4];
            a[0] = A_e(T_REAL, l->e);
            a[1] = A_e(T_REAL, l->next->e);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call("caskx_logb", a, 4, 0);
        } else {
            AArg a[3];
            a[0] = A_e(T_REAL, l->e);
            a[1] = A_l(g_filelab);
            a[2] = A_i(line);
            asm_call("caskx_log10e", a, 3, 0);
        }
        break;
    }
    case BI_PI:
        ainxd("%xmm0", d2bits(3.141592653589793));
        break;
    case BI_RANDBETWEEN: {
        AArg a[4];
        a[0] = A_e(T_NUM, l->e);
        a[1] = A_e(T_NUM, l->next->e);
        a[2] = A_l(g_filelab);
        a[3] = A_i(line);
        asm_call("caskx_randbetween", a, 4, 0);
        break;
    }

    case BI_SUM: case BI_AVG: case BI_COUNT: {
        Sym *s = l->e->sym;
        int isreal = (s && s->type == T_REAL);
        if (f->bid == BI_COUNT) {
            aimm("%rax", s ? (long long)s->dims[0] : 0);
            break;
        }
        if (f->bid == BI_SUM && isreal) {
            AArg a[2];
            a[0] = A_e(T_NUM, l->e);              /* lea: array decays to its address */
            a[1] = A_i((long long)s->dims[0]);
            asm_call("caskx_sum_r", a, 2, 0);
        } else if (f->bid == BI_SUM) {
            AArg a[4];
            a[0] = A_e(T_NUM, l->e);
            a[1] = A_i(s ? (long long)s->dims[0] : 0);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call("caskx_sum", a, 4, 0);
        } else {                                  /* AVG: both variants take file/line */
            AArg a[4];
            a[0] = A_e(T_NUM, l->e);
            a[1] = A_i(s ? (long long)s->dims[0] : 0);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call(isreal ? "caskx_avg_r" : "caskx_avg", a, 4, 0);
        }
        break;
    }

    case BI_MAX: case BI_MIN: {
        int is_max = (f->bid == BI_MAX);
        if (l->e->k == E_VAR && l->e->sym && l->e->sym->ndim > 0) {
            Sym *s = l->e->sym;
            int isreal = (s->type == T_REAL);
            const char *sym;
            AArg a[4];
            if (is_max) sym = isreal ? "caskx_max_r" : "caskx_max";
            else        sym = isreal ? "caskx_min_r" : "caskx_min";
            a[0] = A_e(T_NUM, l->e);
            a[1] = A_i((long long)s->dims[0]);
            a[2] = A_l(g_filelab);
            a[3] = A_i(line);
            asm_call(sym, a, 4, 0);
        } else {
            emit_minmax_chain(l, is_max, e->t == T_REAL);
        }
        break;
    }

    case BI_LEFT: case BI_RIGHT: {
        AArg a[4];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_NUM, l->next->e);
        a[2] = A_l(g_filelab);
        a[3] = A_i(line);
        asm_call(f->bid == BI_LEFT ? "caskx_left" : "caskx_right", a, 4, 0);
        break;
    }
    case BI_MID: {
        AArg a[5];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_NUM, l->next->e);
        a[2] = A_e(T_NUM, l->next->next->e);
        a[3] = A_l(g_filelab);
        a[4] = A_i(line);
        asm_call("caskx_mid", a, 5, 0);            /* 5th arg on the stack */
        break;
    }
    case BI_UPPER: case BI_LOWER: case BI_TRIM: {
        AArg a[1];
        const char *sym = f->bid == BI_UPPER ? "caskx_upper" :
                          f->bid == BI_LOWER ? "caskx_lower" : "caskx_trim";
        a[0] = A_e(T_TEXT, l->e);
        asm_call(sym, a, 1, 0);
        break;
    }
    case BI_CONCAT:
        emit_concat_chain(l);
        break;
    case BI_VAL: {
        AArg a[1];
        a[0] = A_e(T_TEXT, l->e);
        asm_call("caskx_val", a, 1, 0);
        break;
    }
    case BI_RCL:
        if (!e->args) {
            acall_popped("caskx_rcl_ask", NULL, 0, 0);
        } else if (e->args->e->t == T_TEXT) {
            AArg a[1];
            a[0] = A_e(T_TEXT, e->args->e);
            asm_call("caskx_rcl_text", a, 1, 0);
        } else {
            AArg a[1];
            a[0] = A_e(T_REAL, e->args->e);       /* C: cask_rcl_num(double) */
            asm_call("caskx_rcl_num", a, 1, 0);
        }
        break;
    case BI_LOG10: {
        AArg a[3];
        a[0] = A_e(T_REAL, l->e);
        a[1] = A_l(g_filelab);
        a[2] = A_i(line);
        asm_call("caskx_log10e", a, 3, 0);
        break;
    }
    case BI_FIND: {
        AArg a[5];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_TEXT, l->next->e);
        if (l->next->next) a[2] = A_e(T_NUM, l->next->next->e);
        else               a[2] = A_i(1);
        a[3] = A_l(g_filelab);
        a[4] = A_i(line);
        asm_call("caskx_find", a, 5, 0);
        break;
    }
    case BI_SUBSTITUTE: {
        AArg a[3];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_TEXT, l->next->e);
        a[2] = A_e(T_TEXT, l->next->next->e);
        asm_call("caskx_substitute", a, 3, 0);
        break;
    }
    case BI_REPT: {
        AArg a[4];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_NUM, l->next->e);
        a[2] = A_l(g_filelab);
        a[3] = A_i(line);
        asm_call("caskx_rept", a, 4, 0);
        break;
    }
    case BI_EXACT: {
        AArg a[2];
        a[0] = A_e(T_TEXT, l->e);
        a[1] = A_e(T_TEXT, l->next->e);
        asm_call("caskx_exact", a, 2, 0);
        break;
    }
    case BI_CHAR: {
        AArg a[3];
        a[0] = A_e(T_NUM, l->e);
        a[1] = A_l(g_filelab);
        a[2] = A_i(line);
        asm_call("caskx_char_code", a, 3, 0);
        break;
    }
    case BI_CODE: {
        AArg a[1];
        a[0] = A_e(T_TEXT, l->e);
        asm_call("caskx_code", a, 1, 0);
        break;
    }

    default: {
        /* sqrt, pow, sin, cos, tan, asin, acos, atan, log, exp, atan2 --
         * direct libm calls; C's implicit int -> double applies         */
        AArg a[64];
        int n = 0;
        EList *it;
        for (it = e->args; it && n < 60; it = it->next, n++)
            a[n] = A_e(T_REAL, it->e);
        asm_call(bi_cname(f->bid), a, n, 0);
        break;
    }
    }   /* switch (f->bid) */
}

/* ================= statements ================= */

static int g_brk = 0;             /* current break target label (0 = none) */
static int g_cont = 0;            /* current continue target label        */
static int g_retlbl = 0;          /* return label of the current function */

/* ---- slot pre-pass: every frame slot is allocated BEFORE emission so
 * the prologue can reserve the full frame up front (expressions only
 * use machine-stack pushes, never frame slots).
 * Locals whose address is taken (input x;) can never live in a register
 * cache -- mark them first. ---- */
static void apre_mark_slist(SList *l);
static void apre_mark_stmt(Stmt *s)
{
    switch (s->k) {
    case S_INPUT:
        if (s->val && s->val->k == E_VAR && s->val->sym && s->val->sym->ndim == 0)
            s->val->sym->areg = -1;              /* address taken: force a slot */
        break;
    case S_IF:     apre_mark_slist(s->body); apre_mark_slist(s->elseb); break;
    case S_WHILE:  apre_mark_slist(s->body); break;
    case S_UNTIL:  apre_mark_slist(s->body); break;
    case S_DO:     apre_mark_slist(s->body); break;
    case S_FOR:    if (s->initst) apre_mark_stmt(s->initst); apre_mark_slist(s->body); break;
    case S_FOREACH:apre_mark_slist(s->body); break;
    case S_SWITCH: {
        Case *c;
        for (c = s->cases; c; c = c->next) apre_mark_slist(c->body);
        apre_mark_slist(s->defbody);
        break;
    }
    case S_BLOCK:  apre_mark_slist(s->body); break;
    default: break;
    }
}
static void apre_mark_slist(SList *l)
{
    for (; l; l = l->next) if (l->s) apre_mark_stmt(l->s);
}

/* allocate a register cache entry for a scalar local (0 = use a slot) */
static void try_cache(Sym *sy)
{
    if (cacheable_sym(sy) && g_ncreg < 5) {
        sy->areg = 1 + g_ncreg++;
        sy->aoff = 0;
    } else {
        if (sy->areg >= 0) sy->areg = 0;
        sy->aoff = slot_new(1);
    }
}

static void apre_slist(SList *l);
static void apre_stmt(Stmt *s)
{
    switch (s->k) {
    case S_DECL: {
        int i;
        for (i = 0; i < s->ndecl; i++) {
            Decl *d = &s->decls[i];
            int n = 1, k;
            if (d->is_const || !d->sym || d->sym->is_global) continue;
            if (d->ndim > 0) {
                n = 0;
                for (k = 0; k < d->ndim; k++) {
                    if (d->dims[k] <= 0 || n > (1 << 24)) { n = -1; break; }
                    n = n ? n * d->dims[k] : d->dims[k];
                    if (n > (1 << 24)) { n = -1; break; }
                }
                if (n < 0) {
                    berr(d->line, 0, "数组太大：局部数组最多 16M 个元素");
                    n = 1;
                }
                d->sym->aoff = slot_new(n);
            } else {
                try_cache(d->sym);              /* register cache or one slot */
            }
        }
        break;
    }
    case S_IF:      apre_slist(s->body); apre_slist(s->elseb); break;
    case S_WHILE:   apre_slist(s->body); break;
    case S_UNTIL:   apre_slist(s->body); break;
    case S_DO:      apre_slist(s->body); break;
    case S_FOR:     if (s->initst) apre_stmt(s->initst); apre_slist(s->body); break;
    case S_FOREACH:
        if (s->isyms) try_cache(s->isyms);
        s->tmp = slot_new(1);                   /* loop counter stays a slot */
        apre_slist(s->body);
        break;
    case S_SWITCH: {
        Case *c;
        s->tmp = slot_new(1);                   /* evaluated condition */
        for (c = s->cases; c; c = c->next) apre_slist(c->body);
        apre_slist(s->defbody);
        break;
    }
    case S_BLOCK:   apre_slist(s->body); break;
    default: break;      /* S_EXPR/S_SAY/S_RETURN/S_BREAK/... allocate nothing */
    }
}
static void apre_slist(SList *l)
{
    for (; l; l = l->next) if (l->s) apre_stmt(l->s);
}

/* ---- statement emission ---- */
static void aslist(SList *l);
static void astmt(Stmt *s);

/* local array declaration: zero n elements, then store the initialiser
 * (E_LIST flattened row-major, converted per element like emit_converted).
 * Values past n are ignored -- the C backend let the C compiler do the
 * same (excess initialisers are dropped).                              */
static void ainit_elem(Expr *lst, Sym *sy, long long *idx, long long n)
{
    EList *l;
    for (l = lst->args; l; l = l->next) {
        if (l->e->k == E_LIST) { ainit_elem(l->e, sy, idx, n); continue; }
        if (*idx >= n) return;               /* excess: dropped, like C */
        emit_conv(sy->type, l->e);             /* value -> rax / xmm0 */
        if (sy->type == T_REAL)
            wa("    movsd %%xmm0, %d(%%rbp)\n", sy->aoff + 8 * (*idx));
        else
            wa("    movq %%rax, %d(%%rbp)\n", sy->aoff + 8 * (*idx));
        (*idx)++;
    }
}

static void astmt(Stmt *s)
{
    switch (s->k) {

    case S_DECL: {
        int i;
        for (i = 0; i < s->ndecl; i++) {
            Decl *d = &s->decls[i];
            Sym *sy = d->sym;
            if (d->is_const || !sy || sy->is_global) continue;
            if (d->ndim > 0) {
                int n = 1, k, ltop;
                for (k = 0; k < d->ndim; k++) n *= d->dims[k];
                /* zero fill: r10 walks the slots, r11 counts */
                if (n > 0) {
                    wa("    leaq %d(%%rbp), %%r10\n", sy->aoff);
                    aimm("%r11", n);
                    wa("    xorl %%eax, %%eax\n");
                    ltop = nlbl();
                    wa(".L%d:\n", ltop);
                    wa("    movq %%rax, (%%r10)\n");
                    wa("    addq $8, %%r10\n");
                    wa("    subq $1, %%r11\n");
                    wa("    jne .L%d\n", ltop);
                }
                if (d->init) {
                    long long idx = 0;
                    if (d->init->k != E_LIST) {
                        berr(d->line, 0, "数组初始化要用 { ... } 形式");
                        break;
                    }
                    ainit_elem(d->init, sy, &idx, n);
                }
            } else if (d->init) {
                emit_conv_decl(sy->type, d->init);
                astore_slot(sy);
            } else {
                const char *cr = creg_of(sy);
                if (cr) wa("    movq $0, %s\n", cr);
                else    wa("    movq $0, %d(%%rbp)\n", sy->aoff);   /* 0 / NULL */
            }
        }
        break;
    }

    case S_IF: {
        int lelse = nlbl(), lend = nlbl();
        acond_jmp(s->cond, 0, lelse);
        aslist(s->body);
        if (s->elseb) {
            wa("    jmp .L%d\n", lend);
            wa(".L%d:\n", lelse);
            aslist(s->elseb);
            wa(".L%d:\n", lend);
        } else {
            wa(".L%d:\n", lelse);
        }
        break;
    }

    case S_WHILE: {
        int ltop = nlbl(), lend = nlbl();
        int ob, oc;
        wa(".L%d:\n", ltop);
        acond_jmp(s->cond, 0, lend);
        ob = g_brk; oc = g_cont; g_brk = lend; g_cont = ltop;
        aslist(s->body);
        g_brk = ob; g_cont = oc;
        wa("    jmp .L%d\n", ltop);
        wa(".L%d:\n", lend);
        break;
    }

    case S_UNTIL: {                     /* C: while (!(cond)) { body }  */
        int ltop = nlbl(), lend = nlbl();
        int ob, oc;
        wa(".L%d:\n", ltop);
        acond_jmp(s->cond, 1, lend);      /* condition true -> done       */
        ob = g_brk; oc = g_cont; g_brk = lend; g_cont = ltop;
        aslist(s->body);
        g_brk = ob; g_cont = oc;
        wa("    jmp .L%d\n", ltop);
        wa(".L%d:\n", lend);
        break;
    }

    case S_DO: {                        /* runs at least once; continue -> condition */
        int lbody = nlbl(), lcond = nlbl(), lend = nlbl();
        int ob, oc;
        wa(".L%d:\n", lbody);
        ob = g_brk; oc = g_cont; g_brk = lend; g_cont = lcond;
        aslist(s->body);
        g_brk = ob; g_cont = oc;
        wa(".L%d:\n", lcond);
        acond_jmp(s->cond, 1, lbody);
        wa(".L%d:\n", lend);
        break;
    }

    case S_FOR: {
        int ltop = nlbl(), lstep = nlbl(), lend = nlbl();
        int ob, oc;
        if (s->initst) astmt(s->initst);
        wa(".L%d:\n", ltop);
        if (s->cond) {
            acond_jmp(s->cond, 0, lend);
        }
        ob = g_brk; oc = g_cont; g_brk = lend; g_cont = lstep;
        aslist(s->body);
        g_brk = ob; g_cont = oc;
        wa(".L%d:\n", lstep);
        if (s->step) aexpr(s->step);
        wa("    jmp .L%d\n", ltop);
        wa(".L%d:\n", lend);
        break;
    }

    case S_FOREACH: {
        Sym *sy = s->isyms;
        long long n;
        int ltop = nlbl(), lcont, lend = nlbl(), ob, oc;
        if (!s->cond || s->cond->k != E_VAR || !s->cond->sym || s->cond->sym->ndim == 0) {
            berr(s->line, 0, "foreach 要遍历一个数组");
            break;
        }
        n = s->cond->sym->dims[0];
        lcont = nlbl();
        wa("    movq $0, %d(%%rbp)\n", s->tmp);         /* counter = 0 */
        wa(".L%d:\n", ltop);
        wa("    movq %d(%%rbp), %%rax\n", s->tmp);
        aimm("%rcx", n);
        wa("    cmpq %%rcx, %%rax\n");                   /* counter - n */
        wa("    jge .L%d\n", lend);
        aexpr(s->cond);                                  /* lea base -> rax */
        wa("    movq %d(%%rbp), %%rcx\n", s->tmp);       /* rcx = counter */
        if (sy && sy->type == T_REAL) {
            wa("    movsd (%%rax,%%rcx,8), %%xmm0\n");
            wa("    movsd %%xmm0, %d(%%rbp)\n", sy->aoff);
        } else if (sy) {
            wa("    movq (%%rax,%%rcx,8), %%rdx\n");
            if (creg_of(sy)) wa("    movq %%rdx, %s\n", creg_of(sy));
            else             wa("    movq %%rdx, %d(%%rbp)\n", sy->aoff);
        }
        ob = g_brk; oc = g_cont; g_brk = lend; g_cont = lcont;
        aslist(s->body);
        g_brk = ob; g_cont = oc;
        wa(".L%d:\n", lcont);
        wa("    addq $1, %d(%%rbp)\n", s->tmp);
        wa("    jmp .L%d\n", ltop);
        wa(".L%d:\n", lend);
        break;
    }

    case S_SWITCH: {
        Case *c;
        int ncase = 0, i = 0;
        int *lbl = NULL;
        int ldef = nlbl(), lend = nlbl();
        int ob = g_brk, oc = g_cont;
        g_brk = lend;                       /* break exits the switch      */
        /* g_cont untouched: continue goes to the enclosing loop (C rule) */
        aexpr(s->cond);                     /* condition -> frame slot     */
        wa("    movq %%rax, %d(%%rbp)\n", s->tmp);
        for (c = s->cases; c; c = c->next) ncase++;
        if (ncase) lbl = (int *)xmalloc(sizeof(int) * (size_t)ncase);
        for (c = s->cases, i = 0; c; c = c->next, i++) {
            long long v;
            lbl[i] = nlbl();
            if (!fold_int_e(c->val, &v)) {
                berr(c->line, 0, "case 的值必须是编译期常量");
                v = 0;
            }
            aimm("%rax", v);
            wa("    cmpq %d(%%rbp), %%rax\n", s->tmp);   /* v - cond */
            wa("    je .L%d\n", lbl[i]);
        }
        wa("    jmp .L%d\n", s->defbody ? ldef : lend);   /* no match  */
        for (c = s->cases, i = 0; c; c = c->next, i++) {
            wa(".L%d:\n", lbl[i]);
            aslist(c->body);                /* falls through, like C */
        }
        if (s->defbody) {                   /* default sits last, as in the C backend */
            wa(".L%d:\n", ldef);
            aslist(s->defbody);
        }
        wa(".L%d:\n", lend);
        g_brk = ob; g_cont = oc;
        if (lbl) free(lbl);
        break;
    }

    case S_BLOCK:
        aslist(s->body);
        break;
    case S_EMPTY:
        break;
    case S_EXPR:
        if (s->val && s->val->k == E_ASSIGN) emit_assign(s->val, 0);
        else aexpr(s->val);
        break;
    case S_SAY: {
        EList *l;
        for (l = s->args; l; l = l->next) {
            const char *fn;
            int isreal = (l->e->t == T_REAL);
            switch (l->e->t) {
            case T_NUM:   fn = "caskx_p_num";   break;
            case T_REAL:  fn = "caskx_p_real";  break;
            case T_TEXT:  fn = "caskx_p_str";   break;
            case T_LETTER:fn = "caskx_p_char";  break;
            case T_BOOL:  fn = "caskx_p_bool";  break;
            default:      fn = "caskx_p_str";   break;
            }
            aexpr(l->e);                        /* raw: printed by own type */
            if (isreal) apush_xmm0(); else apush_rax();
            acall_popped(fn, isreal ? (int[]){1} : (int[]){0}, 1, 0);
        }
        if (s->nl) acall_popped("caskx_nl", NULL, 0, 0);
        break;
    }
    case S_INPUT: {
        const char *fn;
        switch (s->val->t) {
        case T_NUM:    fn = "caskx_in_num";  break;
        case T_REAL:   fn = "caskx_in_real"; break;
        case T_TEXT:   fn = "caskx_in_str";  break;
        case T_LETTER: fn = "caskx_in_char"; break;
        default:       fn = "caskx_in_bool"; break;
        }
        if (s->val->k == E_IDX)       aidx_addr(s->val);   /* checks included */
        else if (s->val->k == E_VAR)  av_addr(s->val);
        else { berr(s->line, 0, "编译器内部：input 目标不是左值"); break; }
        apush_rax();
        acall_popped(fn, (int[]){0}, 1, 0);
        break;
    }
    case S_RETURN:
        if (!s->val) {
            if (g_instart) wa("    xorl %%eax, %%eax\n");
        } else if (g_instart) {
            aexpr(s->val);
        } else {
            emit_conv(g_fnret, s->val);
        }
        wa("    jmp .L%d\n", g_retlbl);
        break;
    case S_BREAK:
        if (g_brk) wa("    jmp .L%d\n", g_brk);
        else berr(s->line, 0, "break 只能用在循环或 switch 里");
        break;
    case S_CONTINUE:
        if (g_cont) wa("    jmp .L%d\n", g_cont);
        else berr(s->line, 0, "continue 只能用在循环里");
        break;

    case S_FUNC:
    case S_START:
        berr(s->line, 0, "编译器内部：函数定义出现在函数体里");
        break;

    default:
        berr(s->line, 0, "编译器内部：暂不支持的语句");
        break;
    }
}

static void aslist(SList *l)
{
    for (; l; l = l->next) if (l->s) astmt(l->s);
}

/* ================= functions & globals ================= */

/* Windows x64 positional registers; SysV class-counted registers */
#ifdef _WIN32
static const char *g_wr[4]  = { "%rcx", "%rdx", "%r8", "%r9" };
#else
static const char *g_sr[6]  = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
#endif

/* Emit one function body (S_FUNC) or the main entry (S_START).
 * S_START (and a zero-param `number main()`) becomes the C `main`
 * symbol and calls caskx_init first -- exactly like the old backend's
 * generated `int main() { cask_init(...); return __cask_start(); }`.
 * The legacy `__cask_main()` hook is deliberately NOT emitted: no
 * runtime symbol defines it (the old C backend would have failed to
 * link there).                                                     */
static void afn(Stmt *s)
{
    Func *f = (s->k == S_FUNC) ? s->fn : NULL;
    const char *sym;
    int i, j, frame, ismain = 0, ob = g_brk, oc = g_cont;

    g_nslot = 0;
    g_pdepth = 0;
    g_brk = 0;
    g_cont = 0;

    if (s->k == S_START) {
        sym = "main"; ismain = 1;
        g_fnret = T_NUM; g_instart = 1;
    } else {
        sym = fn_sym(f);
        ismain = (f->name && !strcmp(f->name, "main") && f->nparam == 0);
        g_fnret = f->ret; g_instart = 0;
    }
    g_retlbl = nlbl();

    /* frame slots: parameters first, then every block-local declaration,
     * foreach counter, switch condition (all pre-allocated).  Locals whose
     * address is taken are marked first so they never get cached. */
    g_ncreg = 0;
    apre_mark_slist(s->body);
    if (f) {
        for (i = 0; i < f->nparam; i++) {
            Sym *ps = scope_find_local(s->scope, f->pn[i]);
            if (!ps) { berr(f->line, 0, "编译器内部：找不到参数 %s 的存储", f->pn[i]); continue; }
            try_cache(ps);
        }
    }
    apre_slist(s->body);
    frame = ((g_nslot * 8 + g_ncreg * 8) + 15) & ~15;

    /* prologue */
    wa("    .globl %s\n", sym);
    wa("%s:\n", sym);
    wa("    pushq %%rbp\n");
    wa("    movq %%rsp, %%rbp\n");
    if (frame) wa("    subq $%d, %%rsp\n", frame);
    /* save the callee-saved registers we cache locals in (below the slots) */
    for (j = 0; j < g_ncreg; j++)
        wa("    movq %s, %d(%%rbp)\n", g_creg[j], -frame + 8 * j);

    /* spill incoming parameters into their slots */
    if (f) {
#ifdef _WIN32
        for (i = 0; i < f->nparam; i++) {
            Sym *ps = scope_find_local(s->scope, f->pn[i]);
            if (!ps) continue;
            if (ps->type == T_REAL) {
                if (i < 4) wa("    movsd %%xmm%d, %d(%%rbp)\n", i, ps->aoff);
                else       wa("    movsd %d(%%rbp), %%xmm0\n    movsd %%xmm0, %d(%%rbp)\n",
                              48 + 8 * (i - 4), ps->aoff);
            } else if (creg_of(ps)) {
                if (i < 4) wa("    movq %s, %s\n", g_wr[i], creg_of(ps));
                else       wa("    movq %d(%%rbp), %s\n", 48 + 8 * (i - 4), creg_of(ps));
            } else {
                if (i < 4) wa("    movq %s, %d(%%rbp)\n", g_wr[i], ps->aoff);
                else       wa("    movq %d(%%rbp), %%rax\n    movq %%rax, %d(%%rbp)\n",
                              48 + 8 * (i - 4), ps->aoff);
            }
        }
#else
        {
            int ik = 0, fk = 0, sk = 0;
            for (i = 0; i < f->nparam; i++) {
                Sym *ps = scope_find_local(s->scope, f->pn[i]);
                if (!ps) continue;
                if (ps->type == T_REAL) {
                    if (fk < 8) wa("    movsd %%xmm%d, %d(%%rbp)\n", fk, ps->aoff);
                    else        wa("    movsd %d(%%rbp), %%xmm0\n    movsd %%xmm0, %d(%%rbp)\n",
                                   16 + 8 * sk, ps->aoff), sk++;
                    fk++;
                } else if (creg_of(ps)) {
                    if (ik < 6) wa("    movq %s, %s\n", g_sr[ik], creg_of(ps));
                    else { wa("    movq %d(%%rbp), %%rax\n    movq %%rax, %s\n", 16 + 8 * sk, creg_of(ps)); sk++; }
                    ik++;
                } else {
                    if (ik < 6) wa("    movq %s, %d(%%rbp)\n", g_sr[ik], ps->aoff);
                    else        wa("    movq %d(%%rbp), %%rax\n    movq %%rax, %d(%%rbp)\n",
                                   16 + 8 * sk, ps->aoff), sk++;
                    ik++;
                }
            }
        }
#endif
    }

    /* main initialises the console first (cask_init in the old backend) */
    if (ismain) {
        AArg a[1];
        a[0] = A_i(g_is_utf8 ? 65001 : 0);
        asm_call("caskx_init", a, 1, 0);
    }

    aslist(s->body);
    if (g_pdepth != 0) {
        berr(0, 0, "编译器内部：表达式栈不平衡 (%d)", g_pdepth);
        g_pdepth = 0;
    }

    /* default return value (mirror emit_tail_return) */
    switch (g_fnret) {
    case T_NUM: case T_BOOL: case T_TEXT: case T_LETTER:
        wa("    xorl %%eax, %%eax\n");
        break;
    case T_REAL:
        wa("    xorpd %%xmm0, %%xmm0\n");
        break;
    default:                                 /* none: nothing to return */
        break;
    }
    wa(".L%d:\n", g_retlbl);
    for (j = 0; j < g_ncreg; j++)               /* restore cached locals */
        wa("    movq %d(%%rbp), %s\n", -frame + 8 * j, g_creg[j]);
    wa("    movq %%rbp, %%rsp\n");
    wa("    popq %%rbp\n");
    wa("    ret\n");
    g_brk = ob;
    g_cont = oc;
}

/* compile-time value of a *global* initialiser, written as .quad / label.
 * Mirrors emit_plain_conv: bool <- non-bool normalises (!= 0), letter
 * truncates to a byte, everything else is the plain folded value. */
static void emit_static_val(SBuf *b, Type dst, Expr *v)
{
    if (dst == T_REAL) {
        double dv;
        if (!fold_real_e(v, &dv)) {
            berr(v->line, v->col, "全局初始化必须是编译期常量");
            wbuf(b, "    .quad 0\n");
            return;
        }
        wbuf(b, "    .quad 0x%llx\n", d2bits(dv));
        return;
    }
    if (dst == T_TEXT) {
        const char *sv; size_t sn;
        if (!fold_text_e(v, &sv, &sn)) {
            berr(v->line, v->col, "全局初始化必须是编译期常量");
            wbuf(b, "    .quad 0\n");
            return;
        }
        wbuf(b, "    .quad .Ls%d\n", rstr(sv, sn));
        return;
    }
    {
        long long iv;
        if (dst == T_BOOL && v->t != T_BOOL && v->t != T_ERR) {
            /* emit_plain_conv: (...) != 0 */
            double dv;
            if (fold_int_e(v, &iv)) { wbuf(b, "    .quad %d\n", iv != 0); return; }
            if (fold_real_e(v, &dv)) { wbuf(b, "    .quad %d\n", dv != 0.0); return; }
            berr(v->line, v->col, "全局初始化必须是编译期常量");
            wbuf(b, "    .quad 0\n");
            return;
        }
        if (!fold_int_e(v, &iv)) {
            /* number g = <int-valued fold> failed; allow double-valued statics
             * only for REAL dst (handled above) -- anything else is an error */
            berr(v->line, v->col, "全局初始化必须是编译期常量");
            wbuf(b, "    .quad 0\n");
            return;
        }
        if (dst == T_LETTER) iv = (long long)(signed char)(unsigned char)iv;
        wbuf(b, "    .quad %lld\n", iv);
    }
}

/* top-level declaration: place storage in .data (initialised) or .bss */
static void aglobal(Stmt *s)
{
    int i;
    for (i = 0; i < s->ndecl; i++) {
        Decl *d = &s->decls[i];
        Sym *sy = d->sym;
        int n = 1, k;
        SBuf *b;
        if (d->is_const || !sy || !sy->is_global) continue;
        if (d->ndim > 0) {
            n = 0;
            for (k = 0; k < d->ndim; k++) {
                if (d->dims[k] <= 0 || n > (1 << 24)) { n = -1; break; }
                n = n ? n * d->dims[k] : d->dims[k];
                if (n > (1 << 24)) { n = -1; break; }
            }
            if (n < 0) {                       /* same guard as local arrays */
                berr(d->line, 0, "数组太大：全局数组最多 16M 个元素");
                n = 1;
            }
        }
        b = d->init ? &g_dbuf : &g_bbuf;
        wbuf(b, "cask_g%d:\n", asm_gid(sy->name));
        if (!d->init) { wbuf(b, "    .zero %d\n", 8 * n); continue; }
        if (n == 1) {
            emit_static_val(b, s->dtype, d->init);
        } else if (d->init->k == E_LIST) {
            long long cnt = 0;
            EList *l;
            for (l = d->init->args; l && cnt < n; l = l->next) {
                if (l->e->k == E_LIST) {
                    EList *m;
                    for (m = l->e->args; m && cnt < n; m = m->next) {
                        emit_static_val(b, s->dtype, m->e);   /* excess dropped, like C */
                        cnt++;
                    }
                } else {
                    emit_static_val(b, s->dtype, l->e);
                    cnt++;
                }
            }
            if (cnt < n) wbuf(b, "    .zero %d\n", 8 * (n - (int)cnt));
        } else {
            berr(d->line, 0, "数组初始化要用 { ... } 形式");
        }
    }
}

/* ================= cold die stubs =================
 * caskx_die(file, line, message) prints the exact run-time error text
 * and exit(1).  rsp is re-based on rbp first, so the call is aligned
 * no matter how deep expression pushes were when the check fired.    */
static void emit_die_stubs(void)
{
    int i;
    for (i = 0; i < g_ndies; i++) {
        DieSite *d = &g_dies[i];
        wa(".Ld%d:\n", d->lbl);
        wa("    movq %%rbp, %%rsp\n");
        wa("    subq $32, %%rsp\n");               /* shadow space, keeps 16-align */
#ifdef _WIN32
        wa("    leaq %s(%%rip), %%rcx\n", g_filelab);
        wa("    movl $%d, %%edx\n", d->line);
        wa("    leaq %s(%%rip), %%r8\n", die_msg_id(d->msg));
#else
        wa("    leaq %s(%%rip), %%rdi\n", g_filelab);
        wa("    movl $%d, %%esi\n", d->line);
        wa("    leaq %s(%%rip), %%rdx\n", die_msg_id(d->msg));
#endif
        wa("    call caskx_die\n");
        wa("    ud2\n");
    }
    if (g_dies) free(g_dies);
    g_dies = NULL;
    g_ndies = g_cdies = 0;
}

/* ================= build driver ================= */

/* exact run-time message texts (byte-identical to cask_rt.h) */
static void emit_rdata_prelude(void)
{
    snprintf(g_filelab, sizeof g_filelab, ".Lfs");
    wbytes(&g_rbuf, g_filelab, g_file, strlen(g_file));
    wbytes(&g_rbuf, "L_m1", "整数加法溢出", strlen("整数加法溢出"));
    wbytes(&g_rbuf, "L_m2", "整数减法溢出", strlen("整数减法溢出"));
    wbytes(&g_rbuf, "L_m3", "整数乘法溢出", strlen("整数乘法溢出"));
    wbytes(&g_rbuf, "L_m4", "除以零", strlen("除以零"));
    wbytes(&g_rbuf, "L_m5", "整数除法溢出", strlen("整数除法溢出"));
    wbytes(&g_rbuf, "L_m6", "除以零（'%' 取余）", strlen("除以零（'%' 取余）"));
    wbytes(&g_rbuf, "L_m7", "数组下标越界", strlen("数组下标越界"));
    wbytes(&g_rbuf, "L_m8", "移位位数必须是 0..63", strlen("移位位数必须是 0..63"));
    wbytes(&g_rbuf, "L_m9", "负数不能左移", strlen("负数不能左移"));
    wbytes(&g_rbuf, "L_m10", "整数左移溢出", strlen("整数左移溢出"));
    wbytes(&g_rbuf, "L_m11", "除以零（float 除法）", strlen("除以零（float 除法）"));
}

static char g_slib[1800]  = "";     /* dir of crt2.o / libmingw32.a / libmsvcrt.a */
static char g_gccdir[1800] = "";    /* dir of libgcc.a */
static char g_crt2[1800]  = "";
static char g_rtlib[1800] = "";     /* libcastrt.a */
static int  g_verbose = 0;          /* -v: show as/ld command lines */
static char g_asm_cache_key[1700] = "";  /* written to the sidecar after a good link */

/* dir + suffix with a hard bound (also keeps -Wformat-truncation quiet
 * about worst-case path lengths that can never occur) */
static void pj(char *d, size_t n, const char *dir, const char *tail)
{
    size_t dl = strlen(dir), tl = strlen(tail);
    if (dl + tl + 1 > n) { d[0] = 0; return; }
    memcpy(d, dir, dl);
    memcpy(d + dl, tail, tl + 1);
}
#ifndef _WIN32
static void path_dirname_posix(char *p);
#endif

#ifdef _WIN32
/* dirname of a path (both separators, in place) */
static void path_dirname(char *p)
{
    char *s = strrchr(p, '\\');
    char *s2 = strrchr(p, '/');
    if (s2 && (!s || s2 > s)) s = s2;
    if (s) *s = 0;
}

static int find_asm_link_env(void)
{
    char cand[1700], exed[1800], libdir[1024];

    /* libcastrt.a: next to caskc.exe, else $CASK_RT_DIR */
    exe_dir(exed, sizeof exed);
    pj(cand, sizeof cand, exed, "\\libcastrt.a");
    if (file_exists(cand)) {
        snprintf(g_rtlib, sizeof g_rtlib, "%s", cand);
    } else {
        const char *env = getenv("CASK_RT_DIR");
        if (env && *env) {
            pj(cand, sizeof cand, env, "\\libcastrt.a");
            if (file_exists(cand)) snprintf(g_rtlib, sizeof g_rtlib, "%s", cand);
        }
    }
    if (!g_rtlib[0]) {
        fprintf(stderr, "caskc: 找不到 libcastrt.a（C* 运行时库）—— 请先运行 build.bat，"
                        "或设置环境变量 CASK_RT_DIR。\n");
        return 0;
    }

    /* crt2.o: next to caskc.exe (build.bat copies it), else the MinGW lib dir */
    pj(cand, sizeof cand, exed, "\\crt2.o");
    if (file_exists(cand)) {
        snprintf(g_crt2, sizeof g_crt2, "%s", cand);
        snprintf(g_slib, sizeof g_slib, "%s", exed);
    }
    if (!g_crt2[0]) {
        FILE *f = popen("where ld 2>nul", "r");
        char ldpath[512];
        if (f && fgets(ldpath, sizeof ldpath, f)) {
            char *nl = strpbrk(ldpath, "\r\n");
            if (nl) *nl = 0;
            snprintf(libdir, sizeof libdir, "%s", ldpath);
            path_dirname(libdir);                       /* ...\\bin */
            snprintf(cand, sizeof cand, "%s", libdir);
            path_dirname(cand);                          /* root */
            pj(exed, sizeof exed, cand, "\\x86_64-w64-mingw32\\lib\\crt2.o");
            if (file_exists(exed)) {
                snprintf(g_crt2, sizeof g_crt2, "%s", exed);
                path_dirname(g_crt2);
                snprintf(g_slib, sizeof g_slib, "%s", g_crt2);
                pj(g_crt2, sizeof g_crt2, g_slib, "\\crt2.o");
            } else {
                pj(exed, sizeof exed, cand, "\\lib\\crt2.o");
                if (file_exists(exed)) {
                    snprintf(g_crt2, sizeof g_crt2, "%s", exed);
                    path_dirname(g_crt2);
                    snprintf(g_slib, sizeof g_slib, "%s", g_crt2);
                    pj(g_crt2, sizeof g_crt2, g_slib, "\\crt2.o");
                }
            }
        }
        if (f) pclose(f);
    }
    if (!g_crt2[0]) {
        const char *fixed[] = {
            "C:\\Strawberry\\c\\x86_64-w64-mingw32\\lib\\crt2.o",
            "C:\\Strawberry\\c\\lib\\crt2.o", NULL
        };
        int i;
        for (i = 0; fixed[i]; i++) if (file_exists(fixed[i])) {
            snprintf(g_crt2, sizeof g_crt2, "%s", fixed[i]);
            snprintf(g_slib, sizeof g_slib, "%s", fixed[i]);
            path_dirname(g_slib);
            break;
        }
    }
    if (!g_crt2[0]) {
        fprintf(stderr, "caskc: 找不到 crt2.o（MinGW-w64 的 CRT 启动文件）—— 请先运行 build.bat，"
                        "或安装 MinGW-w64 工具链。\n");
        return 0;
    }
    if (!g_slib[0]) snprintf(g_slib, sizeof g_slib, "%s", g_crt2), path_dirname(g_slib);

    /* libgcc.a: next to caskc.exe (build.bat copies it), else gcc's print */
    /* exed may have been reused above -- recompute */
    exe_dir(exed, sizeof exed);
    pj(cand, sizeof cand, exed, "\\libgcc.a");
    if (file_exists(cand)) {
        snprintf(g_gccdir, sizeof g_gccdir, "%s", exed);
    } else {
        FILE *f = popen("gcc -print-libgcc-file-name 2>nul", "r");
        char line[600];
        if (f && fgets(line, sizeof line, f)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (file_exists(line)) {
                snprintf(g_gccdir, sizeof g_gccdir, "%s", line);
                path_dirname(g_gccdir);
            }
        }
        if (f) pclose(f);
    }
    if (!g_gccdir[0]) {
        fprintf(stderr, "caskc: 找不到 libgcc.a（MinGW 运行时，--fast 的栈检查和 "
                        "libmingw32 需要它）—— 请先运行 build.bat。\n");
        return 0;
    }
    return 1;
}
#else
/* ---- Linux: crt1.o / crti.o / crtn.o + libc; best effort ---- */
static int find_asm_link_env(void)
{
    char cand[1700], exed[1800], line[700];
    FILE *f;
    static const char *crt1_cands[] = {
        "/usr/lib/x86_64-linux-gnu/crt1.o", "/usr/lib64/crt1.o", "/usr/lib/crt1.o", NULL
    };
    static const char *ldso_cands[] = {
        "/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", NULL
    };
    int i;

    if (!have_prog("as") || !have_prog("ld")) {
        fprintf(stderr, "caskc: 找不到 as / ld（汇编后端需要它们）—— 请安装 binutils。\n");
        return 0;
    }

    exe_dir(exed, sizeof exed);
    pj(cand, sizeof cand, exed, "/libcastrt.a");
    if (file_exists(cand)) snprintf(g_rtlib, sizeof g_rtlib, "%s", cand);
    else {
        const char *env = getenv("CASK_RT_DIR");
        if (env && *env) {
            pj(cand, sizeof cand, env, "/libcastrt.a");
            if (file_exists(cand)) snprintf(g_rtlib, sizeof g_rtlib, "%s", cand);
        }
    }
    if (!g_rtlib[0]) {
        fprintf(stderr, "caskc: 找不到 libcastrt.a（C* 运行时库）—— 请先运行 build.sh。\n");
        return 0;
    }
    g_crt2[0] = 0;
    f = popen("gcc -print-file-name=crt1.o 2>/dev/null", "r");
    if (f && fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (file_exists(line)) {
            snprintf(g_crt2, sizeof g_crt2, "%s", line);   /* crt1.o */
            path_dirname_posix(g_crt2);
        }
    }
    if (f) pclose(f);
    if (!g_crt2[0]) {
        for (i = 0; crt1_cands[i]; i++) if (file_exists(crt1_cands[i])) {
            snprintf(g_crt2, sizeof g_crt2, "%s", crt1_cands[i]);
            path_dirname_posix(g_crt2);
            break;
        }
    }
    if (!g_crt2[0]) {
        fprintf(stderr, "caskc: 找不到 crt1.o（glibc 启动文件）—— 请安装 libc6-dev 或 gcc。\n");
        return 0;
    }
    /* dynamic loader */
    g_gccdir[0] = 0;
    for (i = 0; ldso_cands[i]; i++) if (file_exists(ldso_cands[i])) {
        snprintf(g_gccdir, sizeof g_gccdir, "%s", ldso_cands[i]);  /* reuse as ldso path */
        break;
    }
    /* libgcc: gcc -print-libgcc-file-name */
    {
        FILE *f2 = popen("gcc -print-libgcc-file-name 2>/dev/null", "r");
        if (f2 && fgets(line, sizeof line, f2)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            if (file_exists(line)) { path_dirname_posix(line); snprintf(exed, sizeof exed, "%s", line); }
        }
        if (f2) pclose(f2);
        snprintf(g_slib, sizeof g_slib, "%s", exed);   /* dir of libgcc.a (may be empty) */
    }
    return 1;
}
static void path_dirname_posix(char *p)
{
    char *s = strrchr(p, '/');
    if (s) *s = 0;
}
#endif

static int asm_backend_build(SList *top, const char *out, int keep_s)
{
    char sbuf[1400], obj[1400];
    SList *sl;
    int nstart = 0, nmainf = 0, entline = 0;

    sb_init(&g_abuf);
    sb_init(&g_rbuf);
    sb_init(&g_dbuf);
    sb_init(&g_bbuf);
    g_lbl = 0; g_pdepth = 0; g_nslot = 0; g_ferr = 0; g_strid = 0;
    g_ndies = g_cdies = 0; g_dies = NULL;
    g_brk = 0; g_cont = 0; g_instart = 0; g_fnret = T_NUM;

    emit_rdata_prelude();

    /* entry check (the front end already reports a missing entry) */
    for (sl = top; sl; sl = sl->next) {
        if (!sl->s) continue;
        if (sl->s->k == S_START) { nstart++; entline = sl->s->line; }
        if (sl->s->k == S_FUNC && sl->s->fn && sl->s->fn->name &&
            !strcmp(sl->s->fn->name, "main")) { nmainf++; entline = sl->s->line; }
    }
    if (nstart + nmainf > 1) {
        berr(entline, 0, "程序有多个入口：start{ } 和 function main 只能保留一个");
        return 0;
    }
    if (nstart + nmainf == 0) {
        berr(0, 0, "程序没有入口：需要 function main() 或 start{ }");
        return 0;
    }

    /* globals first, then functions, in source order */
    for (sl = top; sl; sl = sl->next)
        if (sl->s && sl->s->k == S_DECL) aglobal(sl->s);
    for (sl = top; sl; sl = sl->next) {
        Stmt *s = sl->s;
        if (!s) continue;
        if (s->k == S_START) afn(s);
        else if (s->k == S_FUNC) {
            if (!s->fn || s->fn->is_builtin || !s->fn->defined) continue;
            afn(s);
        }
    }

    emit_die_stubs();

#ifdef _WIN32
    if (g_native) {                            /* GUI subsystem entry */
        wa("    .globl WinMain\n");
        wa(".def WinMain; .scl 2; .type 32; .endef\n");
        wa("WinMain:\n");
        wa("    subq $40, %%rsp\n");           /* 32 shadow + 8 align */
        wa("    call main\n");
        wa("    xorl %%eax, %%eax\n");
        wa("    addq $40, %%rsp\n");
        wa("    ret\n");
    }
#endif

    if (g_ferr) return 0;

    /* ---- write the .s file ---- */
    snprintf(sbuf, sizeof sbuf, "%s.gen.s", out);
    snprintf(obj, sizeof obj, "%s.gen.o", out);
    {
        FILE *fp = fopen(sbuf, "wb");
        if (!fp) {
            fprintf(stderr, "caskc: 写不了汇编文件 %s\n", sbuf);
            return 0;
        }
        fputs("# generated by caskc (C* x86-64 backend) -- do not edit\n", fp);
        fputs(".section .rdata\n", fp);
        if (g_rbuf.p) fputs(g_rbuf.p, fp);
        fputs("\n.section .data\n", fp);
        if (g_dbuf.p) fputs(g_dbuf.p, fp);
        fputs("\n.section .bss\n", fp);
        if (g_bbuf.p) fputs(g_bbuf.p, fp);
        fputs("\n.section .text\n", fp);
        if (g_abuf.p) fputs(g_abuf.p, fp);
#ifndef _WIN32
        fputs(".section .note.GNU-stack,\"\",@progbits\n", fp);
#endif
        fclose(fp);
    }

    if (!find_asm_link_env()) return 0;

    /* ---- build cache: unchanged .s + unchanged runtime lib -> reuse the exe,
     * skipping as/ld entirely (sidecar: <out>.cskc, written only after a
     * successful link so a failed build never looks "fresh"). ---- */
    {
        char hex[24], lhex[24], key[1700], side[1700], prev[1700];
        char skey[1700];
        unsigned long long hh = file_hash(sbuf, hex, sizeof hex);
        unsigned long long lh = file_hash(g_rtlib, lhex, sizeof lhex);
        skey[0] = 0;
        if (hh && lh && file_exists(out)) {
            FILE *sf; size_t rl = 0;
            snprintf(key, sizeof key, "caskasm1|%s|rt:%s|%s|%d%d%d",
                     hex, lhex, out, g_fast, g_native, g_console);
            snprintf(skey, sizeof skey, "%s", key);
            snprintf(side, sizeof side, "%s.cskc", out);
            sf = fopen(side, "rb");
            if (sf) { rl = fread(prev, 1, sizeof prev - 1, sf); fclose(sf); }
            prev[rl] = 0;
            if (!strcmp(prev, key)) {
                if (!keep_s) remove(sbuf);
                printf("已生成 %s（内容没变，跳过汇编）\n", out);
                return 1;
            }
        }
        /* remembered below: written after a successful link */
        memcpy(g_asm_cache_key, skey, sizeof g_asm_cache_key);
    }

    /* ---- assemble ---- */
    {
#ifdef _WIN32
        char ascmd[4096];
        int rc;
        snprintf(ascmd, sizeof ascmd, "as --64 \"%s\" -o \"%s\"", sbuf, obj);
        if (g_verbose) fprintf(stderr, "caskc: %s\n", ascmd);
        rc = run_child(ascmd);
        if (rc < 0) { fprintf(stderr, "caskc: 启动 as 失败（汇编已保留: %s）\n", sbuf); return 0; }
        if (rc != 0) { fprintf(stderr, "caskc: as 报了问题。汇编已保留: %s\n", sbuf); return 0; }
#else
        char ascmd[4096];
        snprintf(ascmd, sizeof ascmd, "as --64 \"%s\" -o \"%s\"", sbuf, obj);
        if (g_verbose) fprintf(stderr, "caskc: %s\n", ascmd);
        if (system(ascmd) != 0) {
            fprintf(stderr, "caskc: as 报了问题。汇编已保留: %s\n", sbuf);
            return 0;
        }
#endif
    }

    /* ---- link ---- */
    {
        char ldl[8192];
        int n = 0;
#ifdef _WIN32
        const char *subsys = (g_native && !g_console) ? "windows" : "console";
        n = snprintf(ldl, sizeof ldl,
            "ld -m i386pep --subsystem %s -o \"%s\" \"%s\" \"%s\" \"%s\""
            " -L \"%s\" -L \"%s\""
            " -lmingw32 -lgcc -lmingwex -lmsvcrt -lkernel32",
            subsys, out, g_crt2, obj, g_rtlib,
            g_slib, g_gccdir);
        if (g_native && n > 0 && n < (int)sizeof ldl)
            n += snprintf(ldl + n, sizeof ldl - n,
                " -luser32 -lgdi32 -lcomctl32 -lole32 -luuid -lshell32 -lcomdlg32 -lmsimg32");
#else
        if (!g_gccdir[0]) {
            fprintf(stderr, "caskc: 找不到动态加载器 ld-linux-x86-64.so.2\n");
            remove(obj);
            if (!keep_s) remove(sbuf);
            return 0;
        }
        {
            /* crt1.o crti.o crtn.o + libc; keep it best effort */
            char crt1[1900], crti[1900], crtn[1900], libdir[1900], gtk[1200];
            gtk[0] = 0;
            pj(crt1, sizeof crt1, g_crt2, "/crt1.o");
            snprintf(libdir, sizeof libdir, "%s", g_crt2);       /* dir */
            pj(crti, sizeof crti, libdir, "/crti.o");
            pj(crtn, sizeof crtn, libdir, "/crtn.o");
            if (!file_exists(crti)) {
                static const char *ic[] = {
                    "/usr/lib/x86_64-linux-gnu/crti.o", "/usr/lib64/crti.o", "/usr/lib/crti.o", NULL };
                int i; for (i = 0; ic[i]; i++) if (file_exists(ic[i])) { snprintf(crti, sizeof crti, "%s", ic[i]); break; }
            }
            if (!file_exists(crtn)) {
                static const char *ic[] = {
                    "/usr/lib/x86_64-linux-gnu/crtn.o", "/usr/lib64/crtn.o", "/usr/lib/crtn.o", NULL };
                int i; for (i = 0; ic[i]; i++) if (file_exists(ic[i])) { snprintf(crtn, sizeof crtn, "%s", ic[i]); break; }
            }
            if (g_native) {
                /* GUI programs need GTK at link time (prebuilt runtime uses it) */
                FILE *gf = popen("pkg-config --libs gtk+-3.0 2>/dev/null", "r");
                if (gf) {
                    if (fgets(gtk, sizeof gtk, gf)) {
                        char *nl = strchr(gtk, '\n'); if (nl) *nl = 0;
                    }
                    pclose(gf);
                }
            }
            n = snprintf(ldl, sizeof ldl,
                "ld -o \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" -L \"%s\" -lc -lm %s"
                "-dynamic-linker \"%s\" \"%s\" ",
                out, crt1, crti, obj, g_rtlib, g_slib, gtk, g_gccdir, crtn);
        }
#endif
        if (g_verbose) fprintf(stderr, "caskc: %s\n", ldl);
#ifdef _WIN32
        {
            int rc = run_child(ldl);
            if (rc < 0) { fprintf(stderr, "caskc: 启动 ld 失败。汇编已保留: %s\n", sbuf); return 0; }
            if (rc != 0) { fprintf(stderr, "caskc: ld 报了问题。汇编已保留: %s\n", sbuf); return 0; }
        }
#else
        if (system(ldl) != 0) {
            fprintf(stderr, "caskc: ld 报了问题。汇编已保留: %s\n", sbuf);
            return 0;
        }
#endif
        (void)n;
    }

    /* record the cache key only after a successful link */
    if (g_asm_cache_key[0]) {
        char side[1700];
        FILE *wf;
        snprintf(side, sizeof side, "%s.cskc", out);
        wf = fopen(side, "wb");
        if (wf) { fputs(g_asm_cache_key, wf); fclose(wf); }
        g_asm_cache_key[0] = 0;
    }

    remove(obj);
    if (!keep_s) remove(sbuf);
    printf("已生成 %s\n", out);
    if (g_ferr) return 0;
    return 1;
}

