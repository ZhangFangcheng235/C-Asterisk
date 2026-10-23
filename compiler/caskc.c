/* ============================================================
 * caskc.c -- C* (C Asterisk) compiler
 *   .cask source  ->  x86-64 assembly  ->  as  ->  ld  ->  native .exe
 *   (no C compiler is invoked when compiling a .cask program; the runtime
 *    comes from the prebuilt libcastrt.a)
 *
 * build:  gcc -O2 -o caskc.exe caskc.c
 * usage:  caskc file.cask [-o out.exe] [--emit-c out.c] [-O0..3]
 *         [-g] [--fast] [--keep] [--run] [-v] [-h]
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>
#include <setjmp.h>

#define CASK_VERSION "0.5.0"

/* ---------------- small utilities ---------------- */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fputs("caskc: 内存不够了\n", stderr); exit(1); }
    return p;
}
static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n);
    if (!p) { fputs("caskc: 内存不够了\n", stderr); exit(1); }
    return p;
}
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* dynamic string buffer */
typedef struct { char *p; size_t len, cap; } SBuf;
static void sb_init(SBuf *b) { b->cap = 64; b->len = 0; b->p = (char *)xmalloc(b->cap); b->p[0] = 0; }
static void sb_putc(SBuf *b, char c)
{
    if (b->len + 2 > b->cap) { b->cap *= 2; b->p = (char *)xrealloc(b->p, b->cap); }
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

/* ---------------- error reporting ----------------
 * Problems come in three grades (the editor shows a count for each),
 * plus hints that never make a program fail:
 *
 *   P  spelling   a name that does not exist -- with "did you mean ...?"
 *   S  syntax     the shape of the program is wrong (tokens, braces ...)
 *   E  severe     well formed, but it cannot work (types, values ...)
 *   H  hint       the old way still works; a better one exists
 *
 * On the command line you get the usual  file:line:col: error: ...  text.
 * With --diag every problem is one line on stdout:  K|line|col|message
 * so a program can count and list them.                                */
enum { SEV_SPELL = 0, SEV_SYNTAX = 1, SEV_SEVERE = 2, SEV_HINT = 3 };
static int g_nspell = 0, g_nsyntax = 0, g_nsevere = 0, g_nhint = 0;
static int g_diag = 0;      /* --diag  : machine readable lines on stdout */
static int g_check = 0;     /* --check : stop before code generation      */
static int g_native = 0;    /* use "C_OPED.cash"; -- the native world is open */
static int g_native_line = 0;   /* where that 'use' was written           */
static int g_use_css = 0;   /* use "C_CSS.cash"; -- the styling toolkit  */
static int g_use_io = 0;    /* use "C_IO.cash"; -- files & system        */
static int g_css_line = 0;
static int g_console = 0;   /* --console: keep the console in native mode  */

static const char *g_file = "?";   /* source file name (for messages/#line) */
static int g_errors = 0;
static int g_curline = 1, g_curcol = 1;   /* lexer position */

/* where a syntax error can jump back to and try the next statement */
static jmp_buf g_recover;
static int g_rec_level = 0;          /* >0 : a statement list can recover */
static int g_rec_case = 0;           /* 1 : inside a switch clause list   */

static void diag_rec(int sev, int line, int col, const char *fmt, va_list ap)
{
    static const char tag[4] = { 'P', 'S', 'E', 'H' };
    char msg[2048];
    char *q;

    if (sev == SEV_HINT && !g_diag) return;  /* 'old name' notes only in --diag */
    vsnprintf(msg, sizeof msg, fmt, ap);
    if (sev == SEV_SPELL) g_nspell++;
    else if (sev == SEV_SYNTAX) g_nsyntax++;
    else if (sev == SEV_SEVERE) g_nsevere++;
    else g_nhint++;
    if (sev != SEV_HINT) g_errors++;

    if (g_diag) {
        for (q = msg; *q; q++)                     /* keep it to one line */
            if (*q == '|') *q = '/';
            else if (*q == '\n' || *q == '\r') *q = ' ';
        printf("%c|%d|%d|%s\n", tag[sev], line, col, msg);
        fflush(stdout);
    } else if (sev == SEV_HINT) {
        fprintf(stderr, "%s:%d:%d: 提示: %s\n", g_file, line, col, msg);
    } else {
        fprintf(stderr, "%s:%d:%d: 错误: %s\n", g_file, line, col, msg);
    }
    if (sev != SEV_HINT && g_errors >= 25) {
        fprintf(stderr, "%s: 错误太多，先停下来\n", g_file);
        exit(1);
    }
}

/* err  : a real mistake in the meaning of the program (default grade) */
static void err(int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_rec(SEV_SEVERE, line, col, fmt, ap);
    va_end(ap);
}
/* errs : same, but the caller picks the grade */
static void errs(int sev, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_rec(sev, line, col, fmt, ap);
    va_end(ap);
}
/* hint : an old spelling that still works -- never counted as an error */
static void hint(int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_rec(SEV_HINT, line, col, fmt, ap);
    va_end(ap);
}
static void fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "caskc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Old spellings keep working (soft deprecation): the program builds as
 * before, but we point at the name to use now.                          */
static void dep_hint(const char *w, int line, int col)
{
    if (!strcmp(w, "say"))   hint(line, col, "'say' 是旧写法，现在写 'print'");
    else if (!strcmp(w, "func"))  hint(line, col, "'func' 是旧写法，现在写 'function'");
    else if (!strcmp(w, "start")) hint(line, col, "'start { }' 是旧的入口写法，现在写 'function main() { ... }'");
    else if (!strcmp(w, "const")) hint(line, col, "'const' 是简写，现在写 'constant'");
    else if (!strcmp(w, "void"))  hint(line, col, "'void' 是 C 的名字，现在写 'none'");
    else if (!strcmp(w, "number")) hint(line, col, "'number' 是旧写法，现在写 'int'");
    else if (!strcmp(w, "real"))   hint(line, col, "'real' 是旧写法，现在写 'float'");
    else if (!strcmp(w, "text"))   hint(line, col, "'text' 是旧写法，现在写 'string'");
    else if (!strcmp(w, "letter")) hint(line, col, "'letter' 是旧写法，现在写 'char'");
}

/* ---------------- keywords ---------------- */
enum {
    KW_NONE_ = 0,
    KW_START, KW_IF, KW_ELSE, KW_WHILE, KW_FOR, KW_BREAK, KW_CONTINUE,
    KW_RETURN, KW_SAY, KW_SHOW, KW_INPUT, KW_CONSTANT, KW_USE,
    KW_AND, KW_OR, KW_NOT, KW_TRUE, KW_FALSE,
    KW_NUMBER, KW_REAL, KW_TEXT, KW_LETTER, KW_BOOL, KW_VOID, KW_FUNCTION,
    /* 0.3 */
    KW_SWITCH, KW_CASE, KW_DEFAULT, KW_DO, KW_FOREACH, KW_IN, KW_UNTIL
};
static const struct { const char *word; int kw; } g_keywords[] = {
    { "start", KW_START }, { "if", KW_IF }, { "else", KW_ELSE },
    { "while", KW_WHILE }, { "for", KW_FOR }, { "break", KW_BREAK },
    { "continue", KW_CONTINUE }, { "return", KW_RETURN },
    { "print", KW_SAY }, { "say", KW_SAY }, { "show", KW_SHOW },
    { "input", KW_INPUT },
    { "constant", KW_CONSTANT }, { "const", KW_CONSTANT }, { "use", KW_USE },
    { "and", KW_AND }, { "or", KW_OR }, { "not", KW_NOT },
    { "true", KW_TRUE }, { "false", KW_FALSE },
    { "int", KW_NUMBER }, { "number", KW_NUMBER },
    { "float", KW_REAL }, { "real", KW_REAL },
    { "string", KW_TEXT }, { "text", KW_TEXT },
    { "char", KW_LETTER }, { "letter", KW_LETTER },
    { "bool", KW_BOOL },
    { "none", KW_VOID }, { "void", KW_VOID },
    { "function", KW_FUNCTION }, { "func", KW_FUNCTION },
    /* 0.3: real branching and looping that reads like a plan */
    { "switch", KW_SWITCH }, { "case", KW_CASE }, { "default", KW_DEFAULT },
    { "do", KW_DO }, { "foreach", KW_FOREACH }, { "in", KW_IN },
    { "until", KW_UNTIL },
    { NULL, 0 }
};
static int kw_lookup(const char *w)
{
    int i;
    for (i = 0; g_keywords[i].word; i++)
        if (strcmp(g_keywords[i].word, w) == 0) return g_keywords[i].kw;
    return -1;
}
static int is_type_kw(int kw)
{
    return kw == KW_NUMBER || kw == KW_REAL || kw == KW_TEXT ||
           kw == KW_LETTER || kw == KW_BOOL || kw == KW_VOID;
}

/* C keywords that must not be used as user identifiers */
static const char *g_c_keywords[] = {
    "double", "void", "long", "short", "unsigned",
    "signed", "struct", "union", "enum", "typedef", "static", "extern",
    "auto", "register", "volatile", "inline", "goto", "switch", "case",
    "default", "do", "sizeof", "asm", "restrict", "_Bool", "_Complex",
    "_Atomic", "nullptr", "class", "template", "namespace", "new", "delete",
    "this", "try", "catch", "operator", "public", "private", "protected",
    "virtual", "friend", "using", "throw", "virtual", NULL
};

/* ---------------- tokens ---------------- */
typedef enum { T_EOF, T_ID, T_INT, T_FLT, T_STR, T_CHR, T_OP } TokKind;
typedef struct {
    TokKind k;
    int line, col;
    char *text;      /* T_ID: name; T_OP: operator text; T_STR: decoded bytes */
    long long iv;    /* T_INT: value; T_CHR: byte value */
    double dv;       /* T_FLT: value */
    int kw;          /* keyword index, or -1 */
} Tok;

static Tok *g_toks = NULL;
static int g_ntok = 0, g_tokcap = 0;

static Tok *tok_new(TokKind k, int line, int col)
{
    Tok *t;
    if (g_ntok >= g_tokcap) {
        g_tokcap = g_tokcap ? g_tokcap * 2 : 256;
        g_toks = (Tok *)xrealloc(g_toks, sizeof(Tok) * g_tokcap);
    }
    t = &g_toks[g_ntok++];
    memset(t, 0, sizeof *t);
    t->k = k; t->line = line; t->col = col; t->kw = -1;
    return t;
}

/* ---------------- source buffer ---------------- */
static char *g_src = NULL;      /* NUL-terminated source bytes (BOM stripped) */
static size_t g_srclen = 0;
static int g_is_utf8 = 0;

static int utf8_check(const unsigned char *s, size_t n)
{
    size_t i = 0; int any = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c == 0) return -1;
        if (c < 0x80) { i++; any = 1; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { if (i + 1 >= n || (s[i+1] & 0xC0) != 0x80) return 0; i += 2; }
        else if (c == 0xE0) { if (i + 2 >= n || s[i+1] < 0xA0 || s[i+1] > 0xBF || (s[i+2] & 0xC0) != 0x80) return 0; i += 3; }
        else if (c >= 0xE1 && c <= 0xEF) { if (i + 2 >= n || (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return 0; i += 3; }
        else if (c == 0xF0) { if (i + 3 >= n || s[i+1] < 0x90 || s[i+1] > 0xBF || (s[i+2] & 0xC0) != 0x80 || (s[i+3] & 0xC0) != 0x80) return 0; i += 4; }
        else if (c >= 0xF1 && c <= 0xF3) { if (i + 3 >= n || (s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80 || (s[i+3] & 0xC0) != 0x80) return 0; i += 4; }
        else return 0;
        any = 1;
    }
    return any ? 1 : 0;
}

static void read_source(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) fatal("打不开源文件: %s", path);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) fatal("读不了这个文件: %s", path);
    g_src = (char *)xmalloc((size_t)sz + 1);
    g_srclen = fread(g_src, 1, (size_t)sz, f);
    fclose(f);
    g_src[g_srclen] = 0;
    /* strip UTF-8 BOM */
    if (g_srclen >= 3 && (unsigned char)g_src[0] == 0xEF &&
        (unsigned char)g_src[1] == 0xBB && (unsigned char)g_src[2] == 0xBF) {
        memmove(g_src, g_src + 3, g_srclen - 2);
        g_srclen -= 3;
        g_is_utf8 = 1;
    } else {
        int r = utf8_check((const unsigned char *)g_src, g_srclen);
        g_is_utf8 = (r == 1);
    }
}

/* ---------------- lexer ---------------- */
static size_t g_pos = 0;

static int peekc(void) { return (unsigned char)g_src[g_pos]; }
static int peekc2(void) { return g_src[g_pos + 1] ? (unsigned char)g_src[g_pos + 1] : -1; }
static int adv_char(void)
{
    int c = (unsigned char)g_src[g_pos];
    if (g_src[g_pos]) {
        if (g_src[g_pos] == '\n' ||
            (g_src[g_pos] == '\r' && g_src[g_pos + 1] != '\n')) {
            g_curline++;
            g_curcol = 1;
        } else {
            g_curcol++;
        }
        g_pos++;
    }
    return c;
}

static void lex_error(int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_rec(SEV_SYNTAX, line, col, fmt, ap);   /* a lexer problem is always syntax */
    va_end(ap);
    exit(1);                                    /* the token stream cannot continue */
}

static int decode_escape(int line, int col, int c)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    case '?': return '?';
    case 'x': {
        int v = 0, n = 0, h;
        while (n < 2 && isxdigit((h = peekc()))) {
            adv_char();
            v = v * 16 + (isdigit(h) ? h - '0' : (tolower(h) - 'a' + 10));
            n++;
        }
        if (!n) lex_error(line, col, "\\x 后面至少要有一位十六进制数字");
        return v;
    }
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0', n = 1;
            while (n < 3 && peekc() >= '0' && peekc() <= '7') { v = v * 8 + (adv_char() - '0'); n++; }
            return v;
        }
        lex_error(line, col, "不认识的转义 '\\%c'（可以写: \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH \\nnn）", c);
        return 0;
    }
}

static void lex(void)
{
    g_pos = 0;
    g_curline = 1; g_curcol = 1;
    for (;;) {
        int line = g_curline, col = g_curcol;
        int c;
        /* track position */
#define ADV() do { if (g_src[g_pos]) { \
            if (g_src[g_pos]=='\n' || (g_src[g_pos]=='\r' && g_src[g_pos+1]!='\n')) { \
                g_curline++; g_curcol=1; } else g_curcol++; \
            g_pos++; } } while (0)
        /* skip whitespace & comments */
        for (;;) {
            c = peekc();
            if (c == 0) break;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { ADV(); line = g_curline; col = g_curcol; continue; }
            if (c == '/' && peekc2() == '/') {
                /* a line comment ends at LF or at a lone CR (old Mac style) */
                while (peekc() && peekc() != '\n' && peekc() != '\r') ADV();
                continue;
            }
            if (c == '/' && peekc2() == '*') {
                ADV(); ADV();
                for (;;) {
                    if (!peekc()) lex_error(line, col, "/* 注释 */ 没有结束");
                    if (peekc() == '*' && peekc2() == '/') { ADV(); ADV(); break; }
                    ADV();
                }
                continue;
            }
            break;
        }
        line = g_curline; col = g_curcol;
        c = peekc();
        if (c == 0) { Tok *t = tok_new(T_EOF, line, col); (void)t; return; }

        /* use <header.h>;  -- '<' right after the 'use' keyword is a header name */
        if (c == '<' && g_ntok > 0 &&
            g_toks[g_ntok - 1].k == T_OP && g_toks[g_ntok - 1].kw == KW_USE) {
            SBuf h;
            sb_init(&h);
            sb_putc(&h, '<');
            ADV();
            while (peekc() && peekc() != '>' && peekc() != '\n') {
                sb_putc(&h, (char)peekc());
                ADV();
            }
            if (peekc() != '>') lex_error(line, col, "'use' 后面的 <...> 少了结尾的 '>'");
            else ADV();
            sb_putc(&h, '>');
            tok_new(T_STR, line, col)->text = h.p;
            continue;
        }

        if (isalpha(c) || c == '_' || c >= 0x80) {
            SBuf b; sb_init(&b);
            int hi = 0;
            while (isalnum(peekc()) || peekc() == '_' || peekc() >= 0x80) {
                if (peekc() >= 0x80) hi = 1;
                sb_putc(&b, (char)peekc());
                ADV();
            }
            if (hi && !g_is_utf8)
                lex_error(line, col,
                    "名字里有非 ASCII 字母，文件要存成 UTF-8");
            {
                Tok *t;
                int kw = kw_lookup(b.p);
                if (kw >= 0) {
                    t = tok_new(T_OP, line, col);   /* keyword stored as T_OP with kw flag */
                    t->kw = kw;
                    t->text = xstrdup(b.p);
                    dep_hint(b.p, line, col);       /* old spelling: still works, note the new one */
                    if (kw == KW_USE && peekc() == '<') {
                        /* use <header.h>;  -- scan raw up to '>' and keep the brackets */
                        SBuf h;
                        sb_init(&h);
                        sb_putc(&h, '<');
                        ADV();
                        while (peekc() && peekc() != '>' && peekc() != '\n') {
                            sb_putc(&h, (char)peekc());
                            ADV();
                        }
                        if (peekc() != '>') {
                            lex_error(line, col, "'use' 后面的 <...> 少了结尾的 '>'");
                            sb_putc(&h, '>');
                        } else {
                            ADV();
                            sb_putc(&h, '>');
                        }
                        t = tok_new(T_STR, line, col);
                        t->text = h.p;
                    }
                } else {
                    int i;
                    t = tok_new(T_ID, line, col);
                    t->text = b.p;
                    for (i = 0; g_c_keywords[i]; i++)
                        if (strcmp(g_c_keywords[i], b.p) == 0)
                            lex_error(line, col, "'%s' 是 C 的关键字，不能拿来当名字", b.p);
                    if (strncmp(b.p, "cask_", 5) == 0 || strncmp(b.p, "__cask", 6) == 0)
                        lex_error(line, col, "以 'cask_' 或 '__cask' 开头的名字是保留的");
                }
                (void)t;
            }
            continue;
        }

        if (isdigit(c)) {
            SBuf b; sb_init(&b);
            int isflt = 0;
            while (isdigit(peekc())) { sb_putc(&b, (char)peekc()); ADV(); }
            if (peekc() == '.' && isdigit(peekc2())) {
                isflt = 1;
                sb_putc(&b, (char)peekc()); ADV();
                while (isdigit(peekc())) { sb_putc(&b, (char)peekc()); ADV(); }
            }
            if (peekc() == 'e' || peekc() == 'E') {
                size_t save = g_pos; int sl = g_curline, sc = g_curcol;
                int ok = 0;
                ADV();
                if (peekc() == '+' || peekc() == '-') ADV();
                if (isdigit(peekc())) {
                    ok = 1;
                    while (isdigit(peekc())) ADV();
                }
                if (ok) isflt = 1;
                else { g_pos = save; g_curline = sl; g_curcol = sc; }
            }
            if (isflt) {
                Tok *t = tok_new(T_FLT, line, col);
                char *end = NULL;
                t->dv = strtod(b.p, &end);
                if (end != b.p + strlen(b.p)) lex_error(line, col, "数字 '%s' 写得不对", b.p);
                t->text = b.p;
            } else {
                Tok *t = tok_new(T_INT, line, col);
                unsigned long long v = 0; int ovf = 0; size_t i;
                for (i = 0; b.p[i]; i++) {
                    v = v * 10 + (unsigned)(b.p[i] - '0');
                    if (v > 9223372036854775807ULL) ovf = 1;
                }
                if (ovf) lex_error(line, col, "数字字面量超出范围（最大 9223372036854775807）");
                t->iv = (long long)v;
                t->text = b.p;
            }
            continue;
        }

        if (c == '"' || c == '\'') {
            int quote = c;
            SBuf b; sb_init(&b);
            ADV();
            for (;;) {
                int ch = peekc();
                if (ch == 0) lex_error(line, col, quote == '"' ? "字符串没有结束（要换行请写 \\n）" : "字符字面量没有结束");
                if (ch == '\n') lex_error(line, col, quote == '"' ? "字符串不能跨行（要换行请写 \\n）" : "字符字面量不能跨行");
                if (ch == quote) { ADV(); break; }
                if (ch == '\\') {
                    ADV();
                    {
                        int line2 = g_curline, col2 = g_curcol;
                        int e = adv_char();
                        int v = decode_escape(line2, col2, e);
                        sb_putc(&b, (char)v);
                    }
                } else {
                    sb_putc(&b, (char)adv_char());
                }
            }
            if (quote == '\'') {
                Tok *t;
                if (b.len == 0) lex_error(line, col, "空的字符字面量 ''");
                if (b.len > 1) lex_error(line, col, "一个 char 只放一个字节；'%s' 是 %d 个字节（请改用 string）", b.p, (int)b.len);
                t = tok_new(T_CHR, line, col);
                t->iv = (unsigned char)b.p[0];
                t->text = b.p;
            } else {
                Tok *t = tok_new(T_STR, line, col);
                t->text = b.p;
                t->iv = (long long)b.len;
            }
            continue;
        }

        {   /* operators / punctuation: longest match first */
            static const char *ops3[] = { "<<=", ">>=", NULL };
            static const char *ops2[] = {
                "++", "--", "+=", "-=", "*=", "/=", "%=", "==", "!=",
                "<=", ">=", "&&", "||", "<<", ">>", NULL
            };
            static const char ops1[] = "+-*/%=<>!&|^~?:()[]{},;";
            int i, matched = 0;
            (void)ops3;
            for (i = 0; ops2[i]; i++) {
                if (peekc() == ops2[i][0] && peekc2() == ops2[i][1]) {
                    Tok *t = tok_new(T_OP, line, col);
                    t->text = xstrdup(ops2[i]);
                    ADV(); ADV();
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            if (strchr(ops1, c) && c != 0) {
                Tok *t = tok_new(T_OP, line, col);
                char s[2] = { (char)c, 0 };
                t->text = xstrdup(s);
                ADV();
                (void)t;
                continue;
            }
            lex_error(line, col, "不认识的字符 '%c'（0x%02X）", c, c);
        }
#undef ADV
    }
}

/* ---------------- types ---------------- */
typedef enum { T_ERR = -1, T_NUM = 1, T_REAL, T_TEXT, T_LETTER, T_BOOL, T_VOID } Type;
#define T_UNKNOWN (-99)

static const char *type_name(Type t)
{
    switch (t) {
    case T_NUM: return "int"; case T_REAL: return "float"; case T_TEXT: return "string";
    case T_LETTER: return "char"; case T_BOOL: return "bool"; case T_VOID: return "none";
    default: return "?";
    }
}
static const char *ctype_name(Type t)
{
    switch (t) {
    case T_NUM: return "long long"; case T_REAL: return "double"; case T_TEXT: return "char *";
    case T_LETTER: return "char"; case T_BOOL: return "int"; case T_VOID: return "void";
    default: return "long long";
    }
}
static int is_intlike(Type t) { return t == T_NUM || t == T_LETTER || t == T_BOOL; }
static int is_scalar(Type t) { return is_intlike(t) || t == T_REAL; }

/* ---------------- C_OPED.cash: the window / native world ----------------
 * When the program writes   use "C_OPED.cash";   the compiler opens the
 * whole C / C++ / Win32 world: the header is included, the window
 * libraries are linked, and these helpers get real types so a program
 * can build a window without a single 'include'.                         */
typedef struct NativeApi {
    const char *name;
    Type ret;
    int npar;
    Type par[8];
} NativeApi;
static const NativeApi g_native_api[] = {
    /* 窗口(标题, 宽, 高) */
    { "window",     T_NUM,  3, { T_TEXT, T_NUM, T_NUM } },
    /* 控件(文字, x, y, 宽, 高) -- 放进第一个窗口 */
    { "label",      T_NUM,  5, { T_TEXT, T_NUM, T_NUM, T_NUM, T_NUM } },
    { "button",     T_NUM,  5, { T_TEXT, T_NUM, T_NUM, T_NUM, T_NUM } },
    { "textbox",    T_NUM,  5, { T_TEXT, T_NUM, T_NUM, T_NUM, T_NUM } },
    { "memo",       T_NUM,  5, { T_TEXT, T_NUM, T_NUM, T_NUM, T_NUM } },
    { "checkbox",   T_NUM,  5, { T_TEXT, T_NUM, T_NUM, T_NUM, T_NUM } },
    { "listbox",    T_NUM,  4, { T_NUM, T_NUM, T_NUM, T_NUM } },
    { "progress",   T_NUM,  4, { T_NUM, T_NUM, T_NUM, T_NUM } },
    /* 读 / 写控件 */
    { "settext",    T_NUM,  2, { T_NUM, T_TEXT } },
    { "readtext",   T_TEXT, 1, { T_NUM } },
    { "settitle",   T_NUM,  2, { T_NUM, T_TEXT } },
    { "additem",    T_NUM,  2, { T_NUM, T_TEXT } },
    { "checked",    T_NUM,  1, { T_NUM } },
    { "setchecked", T_NUM,  2, { T_NUM, T_NUM } },
    { "setvalue",   T_NUM,  2, { T_NUM, T_NUM } },
    { "getvalue",   T_NUM,  1, { T_NUM } },
    /* 弹窗 / 延时 / 事件循环 */
    { "launch",     T_NUM,  1, { T_TEXT } },
    { "msgbox",     T_NUM,  2, { T_TEXT, T_TEXT } },
    { "alert",      T_NUM,  1, { T_TEXT } },
    { "ask",        T_NUM,  2, { T_TEXT, T_TEXT } },
    { "sleep",      T_NUM,  1, { T_NUM } },
    { "run",        T_NUM,  0, { 0 } },
    { "destroy",    T_NUM,  1, { T_NUM } },
    { NULL, 0, 0, { 0 } }
};

/* CSS 美化工具包（C_CSS.cash 才认识这些名字） */
static const NativeApi g_css_api[] = {
    { "csswindow",  T_NUM,  0, { 0 } },
    { "cstext",     T_NUM,  2, { T_NUM, T_TEXT } },
    { "csbg",       T_NUM,  2, { T_NUM, T_TEXT } },
    { "csfont",     T_NUM,  3, { T_NUM, T_NUM, T_NUM } },
    { "csradius",   T_NUM,  2, { T_NUM, T_NUM } },
    { "csshadow",   T_NUM,  2, { T_NUM, T_NUM } },
    { "cshide",     T_NUM,  1, { T_NUM } },
    { "csshow",     T_NUM,  1, { T_NUM } },
    { "cspass",     T_NUM,  1, { T_NUM } },
    { "cscolor",    T_NUM,  2, { T_TEXT, T_TEXT } },
    { "csapply",    T_NUM,  2, { T_TEXT, T_NUM } },
    { NULL, 0, 0, { 0 } }
};

/* C_IO.cash: files & system helpers (opened by `use "C_IO.cash";`) */
static const NativeApi g_io_api[] = {
    { "readfile",   T_TEXT, 1, { T_TEXT } },         /* whole file as text ("" if missing) */
    { "writefile",  T_NUM,  2, { T_TEXT, T_TEXT } }, /* overwrite; 1 = ok                */
    { "appendfile", T_NUM,  2, { T_TEXT, T_TEXT } }, /* append; 1 = ok                   */
    { "fileexists", T_NUM,  1, { T_TEXT } },
    { "makedir",    T_NUM,  1, { T_TEXT } },         /* 1 = created or already there     */
    { "appdir",     T_TEXT, 0, { 0 } },              /* folder of this exe, no separator */
    { "envget",     T_TEXT, 1, { T_TEXT } },         /* environment variable ("" if unset)*/
    { "launchwait", T_NUM,  1, { T_TEXT } },         /* run, wait, return exit code      */
    { NULL, 0, 0, { 0 } }
};

static const NativeApi *native_find(const char *n)
{
    int i;
    for (i = 0; g_native_api[i].name; i++)
        if (strcmp(g_native_api[i].name, n) == 0) return &g_native_api[i];
    if (g_use_css)
        for (i = 0; g_css_api[i].name; i++)
            if (strcmp(g_css_api[i].name, n) == 0) return &g_css_api[i];
    if (g_use_io)
        for (i = 0; g_io_api[i].name; i++)
            if (strcmp(g_io_api[i].name, n) == 0) return &g_io_api[i];
    return NULL;
}
/* names the C world uses: MessageBoxA, MB_OK, WS_VISIBLE ... */
static int looks_like_c_name(const char *n)
{
    int i, up = 0;
    if (!n || !n[0]) return 0;
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') { up = 1; continue; }
        if (c == '_' || (c >= '0' && c <= '9')) continue;
        return 0;
    }
    return up || n[0] == '_';   /* has a capital, or starts with '_' */
}

/* ---------------- symbols / scopes ---------------- */
struct Expr;                    /* forward: expression AST comes later */
typedef enum { SK_VAR, SK_PARAM, SK_CONST } SymKind;
typedef struct Sym {
    char *name;
    SymKind kind;
    Type type;
    int dims[8];
    int ndim;
    int line;
    int is_global;
    long long cval;      /* folded value of a constant number */
    int has_cval;
    int aoff;            /* asm backend: frame offset for locals (0 = unset), -8*slot */
    int areg;            /* asm backend: -1 = excluded from reg cache, 0 = slot,
                          * 1..5 = cached in %rbx/%r12/%r13/%r14/%r15 (areg-1) */
    struct Expr *cinit;  /* asm backend: constant initialiser (for real/text const folding) */
    struct Sym *next;
} Sym;
typedef struct Scope {
    struct Scope *parent;
    Sym *first;
} Scope;

static Scope *g_gscope = NULL;   /* global scope */

static Scope *scope_push(Scope *parent)
{
    Scope *s = (Scope *)xmalloc(sizeof *s);
    s->parent = parent; s->first = NULL;
    return s;
}
static Sym *scope_find_local(Scope *s, const char *n)
{
    Sym *x;
    for (x = s->first; x; x = x->next)
        if (strcmp(x->name, n) == 0) return x;
    return NULL;
}
static Sym *scope_find(Scope *s, const char *n)
{
    for (; s; s = s->parent) {
        Sym *x = scope_find_local(s, n);
        if (x) return x;
    }
    return NULL;
}
/* every declared name, kept for "did you mean ...?" suggestions */
#define MAX_SUGGEST 1500
static char *g_allnames[MAX_SUGGEST];
static int g_nallnames = 0;
static void name_register(const char *n)
{
    int i;
    if (!n || !*n || g_nallnames >= MAX_SUGGEST) return;
    for (i = 0; i < g_nallnames; i++)
        if (!strcmp(g_allnames[i], n)) return;
    g_allnames[g_nallnames++] = xstrdup(n);
}

static Sym *sym_new(const char *name, SymKind kind, Type t, int line, int is_global)
{
    Sym *s = (Sym *)xmalloc(sizeof *s);
    s->name = xstrdup(name); s->kind = kind; s->type = t;
    s->ndim = 0; s->line = line; s->is_global = is_global; s->next = NULL;
    s->aoff = 0; s->cinit = NULL; s->areg = 0;
    name_register(name);
    return s;
}
static void scope_add(Scope *s, Sym *sym) { sym->next = s->first; s->first = sym; }

/* ---------------- functions ---------------- */
enum {
    BI_LEN, BI_AT, BI_ABS, BI_SQRT, BI_POW, BI_SIN, BI_COS, BI_TAN,
    BI_ASIN, BI_ACOS, BI_ATAN, BI_LOG, BI_LOG10, BI_EXP,
    BI_FLOOR, BI_CEIL, BI_ROUND, BI_RANDOM, BI_NOW,
    /* Excel-style UPPER-case functions (see docs: Excel 对照表) */
    BI_IF, BI_AND, BI_OR, BI_NOT, BI_XTRUE, BI_XFALSE,
    BI_SIGN, BI_XMOD, BI_XROUND, BI_INT, BI_XLN, BI_XLOG,
    BI_PI, BI_RANDBETWEEN,
    BI_SUM, BI_AVG, BI_COUNT, BI_MAX, BI_MIN,
    BI_LEFT, BI_RIGHT, BI_MID, BI_UPPER, BI_LOWER, BI_TRIM, BI_CONCAT,
    BI_VAL, BI_RCL,
    /* 0.4.2 补充：Excel 数学大写名 / 常用对数 / 截断 / 反正切二 / 文本处理 */
    BI_ATAN2, BI_TRUNC, BI_FIND, BI_SUBSTITUTE, BI_REPT,
    BI_EXACT, BI_CHAR, BI_CODE
};
typedef struct Func {
    char *name;
    Type ret;
    Type *pt;
    char **pn;
    int nparam;
    int line;
    int defined;
    int is_proto;      /* signature-only: number f(number x);  (body supplied elsewhere) */
    int is_builtin;
    int bid;
    struct Func *next;
} Func;
static Func *g_funcs = NULL;

/* ---------------- "did you mean ...?" ---------------- */
/* the spellings worth suggesting (the real, current keywords) */
static const char *const g_sugg_kw[] = {
    "print", "show", "input", "if", "else", "while", "for", "foreach",
    "in", "switch", "case", "default", "do", "until", "break", "continue",
    "return", "function", "int", "float", "string", "char", "bool",
    "none", "constant", "use", "and", "or", "not", "true", "false",
    NULL
};

/* 内置函数的旧写法 -> 现在的规范写法（大写 Excel 名）。
 * 旧名字照样能用，只是在 --diag 检查时给一条不计错的 H 提示。*/
static const struct { const char *alias; const char *canon; const char *extra; } g_builtin_dep[] = {
    { "VAL", "VALUE", NULL },
    { "abs", "ABS", NULL },
    { "sqrt", "SQRT", NULL },
    { "pow", "POWER", NULL },
    { "exp", "EXP", NULL },
    { "round", "ROUND", NULL },
    { "random", "RAND", NULL },
    { "len", "LEN", NULL },
    { "sin", "SIN", NULL },
    { "cos", "COS", NULL },
    { "tan", "TAN", NULL },
    { "asin", "ASIN", NULL },
    { "acos", "ACOS", NULL },
    { "atan", "ATAN", NULL },
    { "log", "LN", "（注意：'log' 是自然对数；常用对数请写 'LOG'，自然对数请写 'LN'）" },
    { "log10", "LOG10", NULL },
    { "floor", "FLOOR", NULL },
    { "ceil", "CEILING", NULL },
    { NULL, NULL, NULL }
};

static int lev_dist(const char *a, const char *b)
{
    int la, lb, i, j, prev[33], row[33];
    for (la = 0; a[la] && la < 30; la++) ;
    for (lb = 0; b[lb] && lb < 30; lb++) ;
    for (j = 0; j <= lb; j++) prev[j] = j;
    for (i = 1; i <= la; i++) {
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            int cost = tolower((unsigned char)a[i - 1]) == tolower((unsigned char)b[j - 1]) ? 0 : 1;
            int d = prev[j] + 1;
            if (row[j - 1] + 1 < d) d = row[j - 1] + 1;
            if (prev[j - 1] + cost < d) d = prev[j - 1] + cost;
            row[j] = d;
        }
        for (j = 0; j <= lb; j++) prev[j] = row[j];
    }
    return prev[lb];
}

/* the closest spelling of `name` among keywords / functions / variables */
static const char *did_you_mean(const char *name)
{
    const char *best = NULL;
    int bestd, i, allowed;
    Func *f;
    if (!name || !*name) return NULL;
    /* short names: only suggest for something close; 'hi' stays 'hi' */
    allowed = (int)strlen(name) >= 6 ? 2 : ((int)strlen(name) >= 3 ? 1 : 0);
    bestd = allowed + 1;
    for (i = 0; g_sugg_kw[i]; i++) {
        int d = lev_dist(g_sugg_kw[i], name);
        if (d < bestd) { bestd = d; best = g_sugg_kw[i]; }
    }
    for (f = g_funcs; f; f = f->next) {
        int d = lev_dist(f->name, name);
        if (d < bestd) { bestd = d; best = f->name; }
    }
    for (i = 0; i < g_nallnames; i++) {
        int d = lev_dist(g_allnames[i], name);
        if (d < bestd) { bestd = d; best = g_allnames[i]; }
    }
    return best;
}

static Func *func_find(const char *n)
{
    Func *f;
    for (f = g_funcs; f; f = f->next)
        if (strcmp(f->name, n) == 0) return f;
    return NULL;
}
static Func *func_new(const char *name)
{
    Func *f = (Func *)xmalloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->name = xstrdup(name);
    f->ret = T_ERR;
    f->next = g_funcs;
    g_funcs = f;
    return f;
}
static void reg_builtin(const char *name, int nparam, Type ret, int bid)
{
    Func *f = func_new(name);
    int i;
    f->is_builtin = 1; f->bid = bid; f->ret = ret; f->nparam = nparam;
    f->pt = (Type *)xmalloc(sizeof(Type) * (nparam ? nparam : 1));
    f->pn = (char **)xmalloc(sizeof(char *) * (nparam ? nparam : 1));
    for (i = 0; i < nparam; i++) {
        f->pt[i] = (bid == BI_AT && i == 1) ? T_NUM : T_REAL;
        f->pn[i] = (char *)"";
    }
    if (bid == BI_LEN || bid == BI_AT || bid == BI_VAL) f->pt[0] = T_TEXT;
    if (bid == BI_ABS) f->pt[0] = T_NUM;
}
static void reg_builtins(void)
{
    reg_builtin("len", 1, T_NUM, BI_LEN);
    reg_builtin("at", 2, T_LETTER, BI_AT);
    reg_builtin("abs", 1, T_NUM, BI_ABS);
    reg_builtin("sqrt", 1, T_REAL, BI_SQRT);
    reg_builtin("pow", 2, T_REAL, BI_POW);
    reg_builtin("sin", 1, T_REAL, BI_SIN);
    reg_builtin("cos", 1, T_REAL, BI_COS);
    reg_builtin("tan", 1, T_REAL, BI_TAN);
    reg_builtin("asin", 1, T_REAL, BI_ASIN);
    reg_builtin("acos", 1, T_REAL, BI_ACOS);
    reg_builtin("atan", 1, T_REAL, BI_ATAN);
    reg_builtin("log", 1, T_REAL, BI_LOG);
    reg_builtin("log10", 1, T_REAL, BI_LOG10);
    reg_builtin("exp", 1, T_REAL, BI_EXP);
    reg_builtin("floor", 1, T_NUM, BI_FLOOR);
    reg_builtin("ceil", 1, T_NUM, BI_CEIL);
    reg_builtin("round", 1, T_NUM, BI_ROUND);
    reg_builtin("random", 0, T_REAL, BI_RANDOM);
    reg_builtin("now", 0, T_NUM, BI_NOW);

    /* Excel-style names -- same spelling and meaning as in Excel so
     * what students know from spreadsheets transfers directly.
     * The ones without a fixed argument shape (IF, AND, MOD, SUM, ...)
     * are checked individually during type analysis.               */
    reg_builtin("IF", 3, T_NUM, BI_IF);
    reg_builtin("AND", 2, T_BOOL, BI_AND);
    reg_builtin("OR", 2, T_BOOL, BI_OR);
    reg_builtin("NOT", 1, T_BOOL, BI_NOT);
    reg_builtin("TRUE", 0, T_BOOL, BI_XTRUE);
    reg_builtin("FALSE", 0, T_BOOL, BI_XFALSE);
    reg_builtin("ABS", 1, T_NUM, BI_ABS);
    reg_builtin("SIGN", 1, T_NUM, BI_SIGN);
    reg_builtin("MOD", 2, T_NUM, BI_XMOD);
    reg_builtin("ROUND", 1, T_NUM, BI_XROUND);
    reg_builtin("INT", 1, T_NUM, BI_INT);
    reg_builtin("POWER", 2, T_REAL, BI_POW);
    reg_builtin("SQRT", 1, T_REAL, BI_SQRT);
    reg_builtin("EXP", 1, T_REAL, BI_EXP);
    reg_builtin("LN", 1, T_REAL, BI_XLN);
    reg_builtin("LOG", 1, T_REAL, BI_XLOG);
    reg_builtin("PI", 0, T_REAL, BI_PI);
    reg_builtin("RAND", 0, T_REAL, BI_RANDOM);
    reg_builtin("RANDBETWEEN", 2, T_NUM, BI_RANDBETWEEN);
    reg_builtin("SUM", 1, T_NUM, BI_SUM);
    reg_builtin("AVERAGE", 1, T_REAL, BI_AVG);
    reg_builtin("COUNT", 1, T_NUM, BI_COUNT);
    reg_builtin("MAX", 1, T_NUM, BI_MAX);
    reg_builtin("MIN", 1, T_NUM, BI_MIN);
    reg_builtin("LEN", 1, T_NUM, BI_LEN);
    reg_builtin("LEFT", 2, T_TEXT, BI_LEFT);
    reg_builtin("RIGHT", 2, T_TEXT, BI_RIGHT);
    reg_builtin("MID", 3, T_TEXT, BI_MID);
    reg_builtin("UPPER", 1, T_TEXT, BI_UPPER);
    reg_builtin("LOWER", 1, T_TEXT, BI_LOWER);
    reg_builtin("TRIM", 1, T_TEXT, BI_TRIM);
    reg_builtin("CONCAT", 2, T_TEXT, BI_CONCAT);
    reg_builtin("VALUE", 1, T_NUM, BI_VAL);
    reg_builtin("VAL", 1, T_NUM, BI_VAL);
    reg_builtin("rcl", 1, T_NUM, BI_RCL);   /* easter egg: rcl is 方程235's girlfriend, see cask_rt.h */

    /* 0.4.2 补充：Excel 数学大写名（和小写完全一样，小写是软弃用别名） */
    reg_builtin("SIN", 1, T_REAL, BI_SIN);
    reg_builtin("COS", 1, T_REAL, BI_COS);
    reg_builtin("TAN", 1, T_REAL, BI_TAN);
    reg_builtin("ASIN", 1, T_REAL, BI_ASIN);
    reg_builtin("ACOS", 1, T_REAL, BI_ACOS);
    reg_builtin("ATAN", 1, T_REAL, BI_ATAN);
    reg_builtin("LOG10", 1, T_REAL, BI_LOG10);
    reg_builtin("FLOOR", 1, T_NUM, BI_FLOOR);
    reg_builtin("CEILING", 1, T_NUM, BI_CEIL);
    reg_builtin("TRUNC", 1, T_NUM, BI_TRUNC);
    reg_builtin("ATAN2", 2, T_REAL, BI_ATAN2);
    /* 0.4.2 补充：文本处理 */
    reg_builtin("FIND", 2, T_NUM, BI_FIND);       /* 2 或 3 个参数，单独检查 */
    reg_builtin("SUBSTITUTE", 3, T_TEXT, BI_SUBSTITUTE);
    reg_builtin("REPT", 2, T_TEXT, BI_REPT);
    reg_builtin("EXACT", 2, T_BOOL, BI_EXACT);
    reg_builtin("CHAR", 1, T_TEXT, BI_CHAR);
    reg_builtin("CODE", 1, T_NUM, BI_CODE);

    /* 参数类型补正（reg_builtin 默认数学函数用 real/int） */
    {
        Func *g;
        g = func_find("FIND");
        if (g) { g->pt[0] = T_TEXT; g->pt[1] = T_TEXT; }
        g = func_find("SUBSTITUTE");
        if (g) { g->pt[0] = T_TEXT; g->pt[1] = T_TEXT; g->pt[2] = T_TEXT; }
        g = func_find("REPT");
        if (g) { g->pt[0] = T_TEXT; g->pt[1] = T_NUM; }
        g = func_find("EXACT");
        if (g) { g->pt[0] = T_TEXT; g->pt[1] = T_TEXT; }
        g = func_find("CHAR");
        if (g) g->pt[0] = T_NUM;
        g = func_find("CODE");
        if (g) g->pt[0] = T_TEXT;
    }
}

/* ---------------- expression AST ---------------- */
typedef enum {
    E_NUM, E_REAL, E_STR, E_CHR, E_TRUE, E_FALSE,
    E_VAR, E_IDX, E_LIST, E_CALL, E_UN, E_BIN, E_ASSIGN, E_POST
} EKind;
typedef struct Expr Expr;
typedef struct EList { Expr *e; struct EList *next; } EList;
struct Expr {
    EKind k;
    Type t;              /* T_UNKNOWN until analyzed */
    int line, col;
    long long iv;
    double dv;
    char *sv;            /* string bytes / identifier / operator */
    size_t svlen;
    Expr *a, *b, *c;
    EList *args;
    Sym *sym;            /* E_VAR; E_IDX: root array var */
    Func *fn;            /* E_CALL */
    int lvl;             /* E_IDX dimension level */
    long long dim;       /* E_IDX bound */
    int litidx;          /* E_IDX literal index already in range */
    int resolved;        /* E_CALL/E_VAR resolution done */
    int native_sym;      /* E_CALL/E_VAR: a name from the C_OPED world */
    const NativeApi *napi;   /* E_CALL: the typed helper it matched */
};
static Expr *expr_new(EKind k, int line, int col)
{
    Expr *e = (Expr *)xmalloc(sizeof *e);
    memset(e, 0, sizeof *e);
    e->k = k; e->t = T_UNKNOWN; e->line = line; e->col = col;
    return e;
}
static void elist_add(EList **head, Expr *e)
{
    EList *n = (EList *)xmalloc(sizeof *n), *p;
    n->e = e; n->next = NULL;
    if (!*head) { *head = n; return; }
    for (p = *head; p->next; p = p->next) ;
    p->next = n;
}

/* ---------------- statement AST ---------------- */
enum {
    S_DECL, S_IF, S_WHILE, S_FOR, S_RETURN, S_BREAK, S_CONTINUE,
    S_SAY, S_INPUT, S_BLOCK, S_EXPR, S_EMPTY, S_FUNC, S_START,
    /* 0.3 */
    S_UNTIL, S_DO, S_FOREACH, S_SWITCH
};
typedef struct { char *name; int dims[8]; int ndim; Expr *init; int line; int is_const; Sym *sym; } Decl;
typedef struct Stmt Stmt;
typedef struct SList { Stmt *s; struct SList *next; } SList;
typedef struct Case Case;             /* one 'case v:' of a switch */
struct Case { Expr *val; int line; SList *body; struct Case *next; };
struct Stmt {
    int k;
    int line;
    Type dtype;           /* S_DECL: declared type */
    Decl *decls;
    int ndecl;
    Expr *cond, *step;
    Stmt *initst;
    SList *body, *elseb;
    Expr *val;
    EList *args;
    int nl;               /* S_SAY: 1 = newline (say), 0 = show */
    Func *fn;
    Scope *scope;
    /* 0.3 */
    Sym *isyms;           /* S_FOREACH: the loop variable        */
    char *iname;          /* S_FOREACH: its name (for emitting)  */
    Case *cases;          /* S_SWITCH: the case list             */
    SList *defbody;       /* S_SWITCH: the 'default' body (or 0) */
    int tmp;              /* asm backend: hidden slot (switch cond / foreach counter) */
};
static Stmt *stmt_new(int k, int line)
{
    Stmt *s = (Stmt *)xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->k = k; s->line = line;
    return s;
}
static void slist_add(SList **head, Stmt *s)
{
    SList *n = (SList *)xmalloc(sizeof *n), *p;
    n->s = s; n->next = NULL;
    if (!*head) { *head = n; return; }
    for (p = *head; p->next; p = p->next) ;
    p->next = n;
}

/* ---------------- code emitter ---------------- */
static FILE *g_out = NULL;
static int g_ind = 0;
static int g_lastline = -1;
static int g_fast = 0;        /* --fast: no runtime checks */
static char *g_lfilepath = NULL; /* escaped source path for #line */

static void w(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
}
static void wind(void) { int i; for (i = 0; i < g_ind; i++) fputc('\t', g_out); }

/* insert a #line directive so gcc/runtime errors point at the .cask file */
static void eline(int ln)
{
    if (ln != g_lastline) {
        fprintf(g_out, "\n#line %d %s\n", ln, g_lfilepath);
        g_lastline = ln;
    }
}

static void emit_cstr(FILE *f, const char *s, size_t n)
{
    size_t i;
    fputc('"', f);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        case '?': fputs("\\?", f); break;
        default:
            if (c < 0x20 || c == 0x7F) fprintf(f, "\\%03o", c);
            else fputc(c, f);
        }
    }
    fputc('"', f);
}
static void emit_cchr(FILE *f, unsigned char c)
{
    fputc('\'', f);
    switch (c) {
    case '\'': fputs("\\'", f); break;
    case '\\': fputs("\\\\", f); break;
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\t': fputs("\\t", f); break;
    default:
        if (c < 0x20 || c == 0x7F) fprintf(f, "\\%03o", c);
        else fputc(c, f);
    }
    fputc('\'', f);
}
/* make a C-string-literal-safe copy of a path (for #line) */
static char *path_escape(const char *p)
{
    SBuf b;
    sb_init(&b);
    sb_putc(&b, '"');
    for (; *p; p++) {
        if (*p == '\\' || *p == '"') sb_putc(&b, '\\');
        sb_putc(&b, *p);
    }
    sb_putc(&b, '"');
    return b.p;
}

/* ============================================================
 * PART3: parser (expressions), type analysis, expression emitter
 * ============================================================ */

/* ---------------- token stream access ---------------- */
static int g_pi = 0;   /* parse index into g_toks */

static Tok *cur(void) { return &g_toks[g_pi]; }
static Tok *at(int i) { return &g_toks[g_pi + i]; }

static int is_op(Tok *t, const char *s)
{
    return t->k == T_OP && t->kw < 0 && t->text && strcmp(t->text, s) == 0;
}
static int is_kwt(int i, int kw)
{
    Tok *t = at(i);
    return t->k == T_OP && t->kw == kw;
}
static int at_type(int i, int allow_void)
{
    Tok *t = at(i);
    if (t->k == T_OP && t->kw >= 0 && is_type_kw(t->kw)) {
        if (t->kw == KW_VOID && !allow_void) return 0;
        return 1;
    }
    return 0;
}
static Type type_of_kw(int kw)
{
    switch (kw) {
    case KW_NUMBER: return T_NUM;
    case KW_REAL: return T_REAL;
    case KW_TEXT: return T_TEXT;
    case KW_LETTER: return T_LETTER;
    case KW_BOOL: return T_BOOL;
    case KW_VOID: return T_VOID;
    }
    return T_ERR;
}
static const char *tok_show(Tok *t)
{
    static char buf[64];
    if (t->k == T_EOF) return "文件的结尾";
    if (t->text) { snprintf(buf, sizeof buf, "'%s'", t->text); return buf; }
    snprintf(buf, sizeof buf, "未知记号");
    return buf;
}
/* ---- statement list being built by the innermost recovery point ---- */
static SList *g_reclist = NULL;

/* panic mode: after a syntax error jump over the rest of the broken
 * statement, so the next statement gets parsed and reported too.
 * eat=0 (inside a block) stops in front of '}' and leaves it to the
 * block that owns it; eat=1 (top level) swallows stray braces.
 * Inside a switch it also stops in front of 'case' / 'default'.      */
static void panic_skip(int eat)
{
    while (cur()->k != T_EOF) {
        if (is_op(cur(), ";")) { g_pi++; return; }
        if (g_rec_case && (is_kwt(0, KW_CASE) || is_kwt(0, KW_DEFAULT))) return;
        if (is_op(cur(), "}")) {
            if (eat) { g_pi++; continue; }
            return;
        }
        g_pi++;
    }
}

static void perr_s(int sev, const char *fmt, ...)   /* grade chosen by the caller */
{
    va_list ap;
    Tok *t = cur();
    va_start(ap, fmt);
    diag_rec(sev, t->line, t->col, fmt, ap);
    va_end(ap);
    if (g_rec_level) longjmp(g_recover, 1);   /* try the next statement */
    exit(1);
}
static void perr(const char *fmt, ...)         /* default: a syntax problem */
{
    va_list ap;
    Tok *t = cur();
    va_start(ap, fmt);
    diag_rec(SEV_SYNTAX, t->line, t->col, fmt, ap);
    va_end(ap);
    if (g_rec_level) longjmp(g_recover, 1);   /* try the next statement */
    exit(1);
}
/* Same, but for a missing piece that can simply be imagined to be
 * there (a forgotten ';'): the statement up to the gap is fine, so the
 * parser just carries on from the token where it stopped.  The caller
 * checks that the statement did make progress -- otherwise (a ';'
 * missing in the middle of a construct) it falls back to panic mode. */
static void perr_rep(const char *fmt, ...)
{
    va_list ap;
    Tok *t = cur();
    va_start(ap, fmt);
    diag_rec(SEV_SYNTAX, t->line, t->col, fmt, ap);
    va_end(ap);
    if (g_rec_level) longjmp(g_recover, 2);
    exit(1);
}
static Tok *expect_op(const char *s, const char *what)
{
    Tok *t = cur();
    if (is_op(t, s)) { g_pi++; return t; }
    if (!strcmp(s, ";"))     /* a forgotten ';' : report and carry on */
        perr_rep("这里要写 '%s'%s%s，但是碰到了 %s", s, what ? "，" : "", what ? what : "", tok_show(t));
    perr("这里要写 '%s'%s%s，但是碰到了 %s", s, what ? "，" : "", what ? what : "", tok_show(t));
    return NULL;
}
static Tok *expect_id(const char *what)
{
    Tok *t = cur();
    if (t->k == T_ID) { g_pi++; return t; }
    perr("这里要写%s，但是碰到了 %s", what, tok_show(t));
    return NULL;
}

/* ---------------- current scope during analysis ---------------- */
static Scope *g_ascope = NULL;

/* ---------------- expression parser ---------------- */
static Expr *parse_expr(void);
static Expr *parse_assign(void);

static Expr *parse_primary(void)
{
    Tok *t = cur();
    Expr *e;
    if (t->k == T_INT) { e = expr_new(E_NUM, t->line, t->col); e->iv = t->iv; g_pi++; return e; }
    if (t->k == T_FLT) { e = expr_new(E_REAL, t->line, t->col); e->dv = t->dv; g_pi++; return e; }
    if (t->k == T_STR) { e = expr_new(E_STR, t->line, t->col); e->sv = t->text; e->svlen = (size_t)t->iv; g_pi++; return e; }
    if (t->k == T_CHR) { e = expr_new(E_CHR, t->line, t->col); e->iv = t->iv; e->sv = t->text; e->svlen = 1; g_pi++; return e; }
    if (t->k == T_ID) { e = expr_new(E_VAR, t->line, t->col); e->sv = t->text; g_pi++; return e; }
    if (t->k == T_OP && t->kw == KW_TRUE) { e = expr_new(E_TRUE, t->line, t->col); g_pi++; return e; }
    if (t->k == T_OP && t->kw == KW_FALSE) { e = expr_new(E_FALSE, t->line, t->col); g_pi++; return e; }
    if (is_op(t, "(")) {
        g_pi++;
        e = parse_expr();
        expect_op(")", "它用来关掉 ( ... )");
        return e;
    }
    if (t->k == T_OP && t->kw >= 0 && is_type_kw(t->kw))
        perr("'%s' 是一个类型，不是值", t->text);
    perr("表达式里出现了意料之外的 %s", tok_show(t));
    return NULL;
}

static Expr *parse_postfix(void)
{
    Expr *e = parse_primary();
    for (;;) {
        Tok *t = cur();
        if (is_op(t, "[")) {
            Expr *n;
            g_pi++;
            n = expr_new(E_IDX, e->line, e->col);
            n->a = e;
            n->b = parse_expr();
            expect_op("]", "它用来关掉下标 [ ... ]");
            e = n;
            continue;
        }
        if (is_op(t, "(")) {
            Expr *n;
            if (e->k != E_VAR) perr("只有函数名才能带 ( ... ) 来调用");
            g_pi++;
            n = expr_new(E_CALL, e->line, e->col);
            n->sv = e->sv;
            if (!is_op(cur(), ")")) {
                for (;;) {
                    elist_add(&n->args, parse_assign());
                    if (is_op(cur(), ",")) { g_pi++; continue; }
                    break;
                }
            }
            expect_op(")", "它用来关掉括号里的参数");
            e = n;
            continue;
        }
        if (is_op(t, "++") || is_op(t, "--")) {
            Expr *n = expr_new(E_POST, e->line, e->col);
            n->sv = xstrdup(t->text);
            n->a = e;
            g_pi++;
            e = n;
            continue;
        }
        break;
    }
    return e;
}

static Expr *parse_unary(void)
{
    Tok *t = cur();
    Expr *n;
    if (is_op(t, "-") || is_op(t, "+") || is_op(t, "!") || is_op(t, "~")) {
        n = expr_new(E_UN, t->line, t->col);
        n->sv = xstrdup(t->text);
        g_pi++;
        n->a = parse_unary();
        return n;
    }
    if (t->k == T_OP && t->kw == KW_NOT) {
        n = expr_new(E_UN, t->line, t->col);
        n->sv = xstrdup("!");
        g_pi++;
        n->a = parse_unary();
        return n;
    }
    if (is_op(t, "++") || is_op(t, "--")) {
        n = expr_new(E_UN, t->line, t->col);
        n->sv = xstrdup(t->text);
        g_pi++;
        n->a = parse_unary();
        return n;
    }
    return parse_postfix();
}

static int op_prec(const char *op)
{
    if (!strcmp(op, "||")) return 1;
    if (!strcmp(op, "&&")) return 2;
    if (!strcmp(op, "|")) return 3;
    if (!strcmp(op, "^")) return 4;
    if (!strcmp(op, "&")) return 5;
    if (!strcmp(op, "==") || !strcmp(op, "!=")) return 6;
    if (!strcmp(op, "<") || !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=")) return 7;
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) return 8;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return 9;
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return 10;
    return 0;
}

static Expr *parse_bin(int minprec)
{
    Expr *lhs = parse_unary();
    for (;;) {
        Tok *t = cur();
        const char *op = NULL;
        int p;
        if (t->k == T_OP && t->kw == KW_AND) op = "&&";
        else if (t->k == T_OP && t->kw == KW_OR) op = "||";
        else if (t->k == T_OP && t->kw < 0 && t->text) op = t->text;
        if (!op) break;
        p = op_prec(op);
        if (p == 0 || p < minprec) break;
        {
            Expr *rhs, *n;
            g_pi++;
            rhs = parse_bin(p + 1);
            n = expr_new(E_BIN, lhs->line, lhs->col);
            n->sv = xstrdup(op);
            n->a = lhs;
            n->b = rhs;
            lhs = n;
        }
    }
    return lhs;
}

static Expr *parse_ternary(void)
{
    Expr *e = parse_bin(1);
    if (is_op(cur(), "?"))
        perr("C* 里不写 ? : 运算符 —— "
             "请改用 IF(condition, value_if_true, value_if_false)");
    return e;
}

static Expr *parse_assign(void)
{
    Expr *e = parse_ternary();
    Tok *t = cur();
    static const char *aops[] = { "=", "+=", "-=", "*=", "/=", "%=", NULL };
    int i;
    if (t->k == T_OP && t->kw < 0 && t->text) {
        for (i = 0; aops[i]; i++)
            if (strcmp(t->text, aops[i]) == 0) break;
        if (aops[i]) {
            Expr *rhs, *n;
            g_pi++;
            rhs = parse_assign();
            n = expr_new(E_ASSIGN, e->line, e->col);
            n->a = e;
            n->sv = xstrdup(aops[i]);
            if (strcmp(aops[i], "=") != 0) {
                Expr *b = expr_new(E_BIN, e->line, e->col);
                char op[2] = { aops[i][0], 0 };
                b->sv = xstrdup(op);
                b->a = e;          /* target node shared: emitted twice */
                b->b = rhs;
                n->b = b;
            } else {
                n->b = rhs;
            }
            return n;
        }
    }
    return e;
}

static Expr *parse_expr(void) { return parse_assign(); }

/* ---------------- initialiser lists (arrays) ---------------- */
static Expr *parse_init_list(void)
{
    Tok *t = cur();
    Expr *e;
    if (!is_op(t, "{")) perr("初始化列表要用 '{' 开头，但是碰到了 %s", tok_show(t));
    g_pi++;
    e = expr_new(E_LIST, t->line, t->col);
    if (!is_op(cur(), "}")) {
        for (;;) {
            if (is_op(cur(), "{")) elist_add(&e->args, parse_init_list());
            else elist_add(&e->args, parse_expr());
            if (is_op(cur(), ",")) { g_pi++; if (is_op(cur(), "}")) break; continue; }
            break;
        }
    }
    expect_op("}", "它用来关掉 { ... }");
    return e;
}

/* ---------------- type analysis ---------------- */
static Type etype(Expr *e);
static int lvalue_ok(Expr *e) { return e->k == E_VAR || e->k == E_IDX; }

static int assignable(Type dst, Type src)
{
    if (dst == T_ERR || src == T_ERR) return 1;
    if (dst == src) return 1;
    if (dst == T_REAL && (src == T_NUM || src == T_LETTER || src == T_BOOL)) return 1;
    if (dst == T_NUM && (src == T_LETTER || src == T_BOOL)) return 1;
    if (dst == T_BOOL && is_scalar(src)) return 1;
    if (dst == T_LETTER && (src == T_NUM || src == T_BOOL)) return 1;
    return 0;
}

static void check_letter_lit(Expr *v, const char *ctx)
{
    if (v->k == E_NUM && (v->iv < -128 || v->iv > 127))
        err(v->line, v->col, "%s: 值 %lld 放不进 char（-128..127）", ctx, v->iv);
}

/* report + return 1 when a whole array name is used where a single
 * value is expected (arrays must be indexed: name[i])            */
static int err_if_array(Expr *e, const char *fn)
{
    if (e && e->k == E_VAR && e->sym && e->sym->ndim > 0) {
        err(e->line, e->col, "%s() 要的是一个值，但是 '%s' 是整个数组（请写 %s[i]）",
            fn, e->sv, e->sv);
        return 1;
    }
    return 0;
}

static void check_static_init(Expr *e, const char *what)
{
    switch (e->k) {
    case E_NUM: case E_REAL: case E_STR: case E_CHR: case E_TRUE: case E_FALSE:
        return;
    case E_UN:
        check_static_init(e->a, what);
        return;
    case E_BIN:
        if (e->t == T_TEXT) {
            err(e->line, e->col, "%s: 这里的 string 值不是编译期常量（只能用字符串字面量）", what);
            return;
        }
        check_static_init(e->a, what);
        check_static_init(e->b, what);
        return;
    case E_VAR:
        if (!e->sym || e->sym->kind != SK_CONST)
            err(e->line, e->col, "%s只能用 'constant' 值和字面量", what);
        return;
    case E_CALL:
        err(e->line, e->col, "%s不能调用函数", what);
        return;
    case E_ASSIGN:
        err(e->line, e->col, "%s不能包含赋值", what);
        return;
    case E_IDX:
        err(e->line, e->col, "%s不能对数组取下标", what);
        return;
    case E_LIST: {
        EList *l;
        for (l = e->args; l; l = l->next) check_static_init(l->e, what);
        return;
    }
    default:
        err(e->line, e->col, "%s不是常量表达式", what);
    }
}

static Type etype(Expr *e)
{
    if (!e) return T_ERR;
    if (e->t != T_UNKNOWN) return e->t;
    switch (e->k) {
    case E_NUM: e->t = T_NUM; break;
    case E_REAL: e->t = T_REAL; break;
    case E_STR: e->t = T_TEXT; break;
    case E_CHR: e->t = T_LETTER; break;
    case E_TRUE: case E_FALSE: e->t = T_BOOL; break;
    case E_VAR: {
        Sym *s = scope_find(g_ascope, e->sv);
        if (!s) {
            const char *fix = did_you_mean(e->sv);      /* spelling mistake? */
            if (!fix && g_native && looks_like_c_name(e->sv)) {
                /* a constant from the C / Win32 headers: MB_OK, WS_VISIBLE ... */
                e->native_sym = 1;
                e->t = T_NUM;
                break;
            }
            if (fix) errs(SEV_SPELL, e->line, e->col,
                          "不认识的名称 '%s'，是不是想写 '%s'？", e->sv, fix);
            else errs(SEV_SPELL, e->line, e->col, "不认识的名称 '%s'", e->sv);
            e->t = T_ERR; break;
        }
        e->sym = s;
        e->t = s->type;
        e->resolved = 1;
        break;
    }
    case E_IDX: {
        Type tb = etype(e->a), ti = etype(e->b);
        Expr *r = e->a;
        int lvl = 0;
        if (tb == T_ERR || ti == T_ERR) { e->t = T_ERR; break; }
        if (!is_intlike(ti)) { err(e->line, e->col, "数组下标必须是 int"); e->t = T_ERR; break; }
        while (r->k == E_IDX) { lvl++; r = r->a; }
        if (r->k != E_VAR || !r->sym) { err(e->line, e->col, "只有数组才能用 [ ... ] 取下标"); e->t = T_ERR; break; }
        if (lvl >= r->sym->ndim) { err(e->line, e->col, "数组 '%s' 用不了这么多层 [ ... ]", r->sym->name); e->t = T_ERR; break; }
        e->sym = r->sym;
        e->lvl = lvl;
        e->dim = r->sym->dims[lvl];
        if (e->b->k == E_NUM && e->b->iv >= 0 && e->b->iv < e->dim) e->litidx = 1;
        e->t = tb;
        break;
    }
    case E_CALL: {
        Func *f = func_find(e->sv);
        EList *l;
        int n = 0;
        for (l = e->args; l; l = l->next) { etype(l->e); n++; }
        if (!f) {
        const char *fix = did_you_mean(e->sv);
        if (g_native || g_use_css || g_use_io) {
            const NativeApi *na = native_find(e->sv);
            if (na) {
                /* the C* window helpers: check the shape, then type it */
                EList *a2 = e->args;
                int i = 0;
                if (n != na->npar) {
                    err(e->line, e->col, "%s() 要 %d 个参数%s，这里是 %d 个",
                        e->sv, na->npar, na->npar == 1 ? "" : "", n);
                    e->t = T_ERR; break;
                }
                for (; a2; a2 = a2->next, i++) {
                    Type at = a2->e->t, want = na->par[i];
                    if (at == T_ERR) { e->t = T_ERR; break; }
                    if (want == T_TEXT && at != T_TEXT) {
                        err(a2->e->line, a2->e->col,
                            "参数 %d（%s()）必须是 string", i + 1, e->sv);
                        e->t = T_ERR; break;
                    }
                    if (want == T_NUM && !is_intlike(at)) {
                        err(a2->e->line, a2->e->col,
                            "参数 %d（%s()）必须是 int", i + 1, e->sv);
                        e->t = T_ERR; break;
                    }
                }
                if (e->t == T_ERR) break;
                e->native_sym = 1; e->napi = na; e->resolved = 1; e->t = na->ret;
                break;
            }
            if (!fix && g_native) {
                /* any function from the C / C++ / Win32 headers: call it
                 * (only while C_OPED.cash is open -- without it an unknown
                 *  name must stay a spelling error)                       */
                e->native_sym = 1; e->resolved = 1; e->t = T_NUM;
                break;
            }
        }
        if (fix)
        errs(SEV_SPELL, e->line, e->col, "不认识的函数 '%s'，是不是想写 '%s'？", e->sv, fix);
        else
        errs(SEV_SPELL, e->line, e->col,
            "不认识的函数 '%s'（如果它来自 'use' 的头文件，请先声明: int %s(...);）",
            e->sv, e->sv);
        e->t = T_ERR;
        break;
    }
        if (f->is_builtin) {
            int di;
            for (di = 0; g_builtin_dep[di].alias; di++)
                if (strcmp(f->name, g_builtin_dep[di].alias) == 0) {
                    if (g_builtin_dep[di].extra)
                        hint(e->line, e->col, "'%s()' 是旧写法：%s", f->name, g_builtin_dep[di].extra);
                    else
                        hint(e->line, e->col, "'%s()' 是旧写法，现在写 '%s()'", f->name, g_builtin_dep[di].canon);
                    break;
                }
            int handled = 1;
            switch (f->bid) {
            case BI_ABS: {
                Type ta = e->args ? e->args->e->t : T_ERR;
                if (n != 1) { err(e->line, e->col, "%s() 要 1 个参数，这里是 %d 个", e->sv, n); e->t = T_ERR; break; }
                if (ta == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(e->args->e, e->sv)) { e->t = T_ERR; break; }
                if (!is_scalar(ta)) { err(e->line, e->col, "%s() 要一个 int 或 float", e->sv); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1;
                e->t = (ta == T_REAL) ? T_REAL : T_NUM;
                break;
            }
            case BI_IF: {
                Expr *ca, *ya, *na;
                Type tc, ty, tn;
                if (n != 3) {
                    err(e->line, e->col, "IF() 要 3 个参数（条件、成立时的值、不成立时的值），这里是 %d 个", n);
                    e->t = T_ERR; break;
                }
                ca = e->args->e; ya = e->args->next->e; na = e->args->next->next->e;
                tc = ca->t; ty = ya->t; tn = na->t;
                if (tc == T_ERR || ty == T_ERR || tn == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(ca, "IF") || err_if_array(ya, "IF") || err_if_array(na, "IF")) { e->t = T_ERR; break; }
                if (tc != T_BOOL) {
                    err(ca->line, ca->col, "IF() 的第一个参数必须是一个比较（true 或 false）");
                    e->t = T_ERR; break;
                }
                if (ty == T_TEXT || tn == T_TEXT) {
                    if (!((ty == T_TEXT || is_scalar(ty)) && (tn == T_TEXT || is_scalar(tn)))) {
                        err(e->line, e->col, "IF() 的两个值要么都是 int，要么都是 string");
                        e->t = T_ERR; break;
                    }
                    e->t = T_TEXT;
                } else if (is_scalar(ty) && is_scalar(tn)) {
                    e->t = (ty == T_REAL || tn == T_REAL) ? T_REAL : T_NUM;
                } else {
                    err(e->line, e->col, "IF() 的值必须是 int 或 string");
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1;
                break;
            }
            case BI_AND:
            case BI_OR: {
                const char *nm = (f->bid == BI_AND) ? "AND" : "OR";
                EList *a; int i = 0;
                if (n < 1) { err(e->line, e->col, "%s() 至少要 1 个参数", nm); e->t = T_ERR; break; }
                for (a = e->args; a; a = a->next, i++) {
                    if (err_if_array(a->e, nm)) { e->t = T_ERR; break; }
                    if (a->e->t == T_ERR) { e->t = T_ERR; break; }
                    if (a->e->t != T_BOOL) {
                        err(a->e->line, a->e->col,
                            "参数 %d（%s()）必须是 true/false（像 x > 3 这样的比较）", i + 1, nm);
                        e->t = T_ERR; break;
                    }
                }
                if (e->t == T_ERR) break;
                e->fn = f; e->resolved = 1; e->t = T_BOOL;
                break;
            }
            case BI_NOT: {
                Expr *a = e->args ? e->args->e : NULL;
                if (n != 1) { err(e->line, e->col, "NOT() 要 1 个参数，这里是 %d 个", n); e->t = T_ERR; break; }
                if (err_if_array(a, "NOT")) { e->t = T_ERR; break; }
                if (a->t == T_ERR) { e->t = T_ERR; break; }
                if (a->t != T_BOOL) { err(a->line, a->col, "NOT() 要一个 true/false 值"); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1; e->t = T_BOOL;
                break;
            }
            case BI_SIGN: {
                Type ta;
                if (n != 1) { err(e->line, e->col, "SIGN() 要 1 个参数，这里是 %d 个", n); e->t = T_ERR; break; }
                ta = e->args->e->t;
                if (ta == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(e->args->e, "SIGN")) { e->t = T_ERR; break; }
                if (!is_scalar(ta)) { err(e->line, e->col, "SIGN() 要一个 int 或 float"); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1; e->t = T_NUM;
                break;
            }
            case BI_XMOD: {
                Type ta, tb;
                Expr *a, *b;
                if (n != 2) { err(e->line, e->col, "MOD() 要 2 个参数，这里是 %d 个", n); e->t = T_ERR; break; }
                a = e->args->e; b = e->args->next->e;
                ta = a->t; tb = b->t;
                if (ta == T_ERR || tb == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(a, "MOD") || err_if_array(b, "MOD")) { e->t = T_ERR; break; }
                if (!is_scalar(ta) || !is_scalar(tb)) { err(e->line, e->col, "MOD() 要两个 int"); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1;
                e->t = (ta == T_REAL || tb == T_REAL) ? T_REAL : T_NUM;
                break;
            }
            case BI_XROUND: {
                Expr *x, *d = NULL;
                Type tx;
                if (n != 1 && n != 2) {
                    err(e->line, e->col, "ROUND() 要 1 个或 2 个参数（值、小数位数），这里是 %d 个", n);
                    e->t = T_ERR; break;
                }
                x = e->args->e; tx = x->t;
                if (tx == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(x, "ROUND")) { e->t = T_ERR; break; }
                if (!is_scalar(tx)) { err(x->line, x->col, "ROUND() 要一个 int 或 float"); e->t = T_ERR; break; }
                if (n == 2) {
                    d = e->args->next->e;
                    if (d->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(d, "ROUND")) { e->t = T_ERR; break; }
                    if (!is_intlike(d->t)) {
                        err(d->line, d->col, "ROUND() 的小数位数参数必须是 int");
                        e->t = T_ERR; break;
                    }
                }
                e->fn = f; e->resolved = 1;
                e->t = (n == 2) ? T_REAL : T_NUM;
                break;
            }
            case BI_INT: {
                Type ta;
                if (n != 1) { err(e->line, e->col, "INT() 要 1 个参数，这里是 %d 个", n); e->t = T_ERR; break; }
                ta = e->args->e->t;
                if (ta == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(e->args->e, "INT")) { e->t = T_ERR; break; }
                if (!is_scalar(ta)) { err(e->line, e->col, "INT() 要一个 int 或 float"); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1; e->t = T_NUM;
                break;
            }
            case BI_XLN:
            case BI_XLOG: {
                const char *nm = f->name;
                EList *a;
                if (n < 1 || n > 2 || (f->bid == BI_XLN && n != 1)) {
                    err(e->line, e->col, "%s() 要 %s，这里是 %d 个", nm,
                        f->bid == BI_XLN ? "1 个参数" : "1 个或 2 个参数", n);
                    e->t = T_ERR; break;
                }
                for (a = e->args; a; a = a->next) {
                    if (a->e->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(a->e, nm)) { e->t = T_ERR; break; }
                    if (!is_scalar(a->e->t)) {
                        err(a->e->line, a->e->col, "%s() 要一个 int 或 float", nm);
                        e->t = T_ERR; break;
                    }
                }
                if (e->t == T_ERR) break;
                e->fn = f; e->resolved = 1; e->t = T_REAL;
                break;
            }
            case BI_PI:
                if (n != 0) { err(e->line, e->col, "PI() 不要参数，这里是 %d 个", n); e->t = T_ERR; break; }
                e->fn = f; e->resolved = 1; e->t = T_REAL;
                break;
            case BI_RANDBETWEEN: {
                Type ta, tb;
                Expr *a, *b;
                if (n != 2) { err(e->line, e->col, "RANDBETWEEN() 要 2 个参数，这里是 %d 个", n); e->t = T_ERR; break; }
                a = e->args->e; b = e->args->next->e;
                ta = a->t; tb = b->t;
                if (ta == T_ERR || tb == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(a, "RANDBETWEEN") || err_if_array(b, "RANDBETWEEN")) { e->t = T_ERR; break; }
                if (!is_intlike(ta) || !is_intlike(tb)) {
                    err(e->line, e->col, "RANDBETWEEN() 要两个 int（不能是 float）");
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1; e->t = T_NUM;
                break;
            }
            case BI_SUM:
            case BI_AVG:
            case BI_COUNT: {
                const char *nm = f->name;
                Expr *a;
                Sym *s;
                if (n != 1) { err(e->line, e->col, "%s() 要 1 个数组参数，这里是 %d 个", nm, n); e->t = T_ERR; break; }
                a = e->args->e;
                if (a->k != E_VAR || !a->sym || a->sym->ndim == 0) {
                    err(a->line, a->col, "%s() 要一个数组（比如 scores）", nm);
                    e->t = T_ERR; break;
                }
                s = a->sym;
                if (s->ndim > 1) { err(a->line, a->col, "%s() 要一个一维数组", nm); e->t = T_ERR; break; }
                if (s->type != T_NUM && s->type != T_REAL) {
                    err(a->line, a->col, "%s() 要一个 int 或 float 数组", nm);
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1;
                e->t = (f->bid == BI_AVG) ? T_REAL : (f->bid == BI_COUNT ? T_NUM : s->type);
                break;
            }
            case BI_MAX:
            case BI_MIN: {
                const char *nm = f->name;
                EList *a;
                int i = 0, allint = 1;
                if (n < 1) { err(e->line, e->col, "%s() 至少要 1 个 int", nm); e->t = T_ERR; break; }
                a = e->args;
                if (a->e->k == E_VAR && a->e->sym && a->e->sym->ndim > 0) {
                    Sym *s = a->e->sym;
                    if (n != 1) { err(a->e->line, a->e->col, "%s() 只接受这一个数组作为参数", nm); e->t = T_ERR; break; }
                    if (s->ndim > 1) { err(a->e->line, a->e->col, "%s() 要一个一维数组", nm); e->t = T_ERR; break; }
                    if (s->type != T_NUM && s->type != T_REAL) {
                        err(a->e->line, a->e->col, "%s() 要一个 int 或 float 数组", nm);
                        e->t = T_ERR; break;
                    }
                    e->fn = f; e->resolved = 1; e->t = s->type;
                    break;
                }
                for (a = e->args; a; a = a->next, i++) {
                    if (a->e->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(a->e, nm)) { e->t = T_ERR; break; }
                    if (!is_scalar(a->e->t)) {
                        err(a->e->line, a->e->col, "%s() 要 int，不要 %s", nm, type_name(a->e->t));
                        e->t = T_ERR; break;
                    }
                    if (a->e->t == T_REAL) allint = 0;
                }
                if (e->t == T_ERR) break;
                e->fn = f; e->resolved = 1;
                e->t = allint ? T_NUM : T_REAL;
                break;
            }
            case BI_LEFT:
            case BI_RIGHT: {
                const char *nm = f->name;
                Expr *s, *c;
                if (n != 2) {
                    err(e->line, e->col, "%s() 要 2 个参数（string、取几个字符），这里是 %d 个", nm, n);
                    e->t = T_ERR; break;
                }
                s = e->args->e; c = e->args->next->e;
                if (s->t == T_ERR || c->t == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(s, nm) || err_if_array(c, nm)) { e->t = T_ERR; break; }
                if (s->t != T_TEXT) {
                    err(s->line, s->col, "%s() 的第一个参数必须是 string", nm);
                    e->t = T_ERR; break;
                }
                if (!is_intlike(c->t)) {
                    err(c->line, c->col, "%s() 的第二个参数必须是 int", nm);
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1; e->t = T_TEXT;
                break;
            }
            case BI_MID: {
                Expr *s, *st, *c;
                if (n != 3) {
                    err(e->line, e->col, "MID() 要 3 个参数（string、起始位置、取几个字符），这里是 %d 个", n);
                    e->t = T_ERR; break;
                }
                s = e->args->e; st = e->args->next->e; c = e->args->next->next->e;
                if (s->t == T_ERR || st->t == T_ERR || c->t == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(s, "MID") || err_if_array(st, "MID") || err_if_array(c, "MID")) { e->t = T_ERR; break; }
                if (s->t != T_TEXT) {
                    err(s->line, s->col, "MID() 的第一个参数必须是 string");
                    e->t = T_ERR; break;
                }
                if (!is_intlike(st->t)) {
                    err(st->line, st->col, "MID() 的起始位置必须是 int");
                    e->t = T_ERR; break;
                }
                if (!is_intlike(c->t)) {
                    err(c->line, c->col, "MID() 的长度必须是 int");
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1; e->t = T_TEXT;
                break;
            }
            case BI_UPPER:
            case BI_LOWER:
            case BI_TRIM: {
                const char *nm = f->name;
                Expr *s;
                if (n != 1) { err(e->line, e->col, "%s() 要 1 个参数，这里是 %d 个", nm, n); e->t = T_ERR; break; }
                s = e->args->e;
                if (s->t == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(s, nm)) { e->t = T_ERR; break; }
                if (s->t != T_TEXT) {
                    err(s->line, s->col, "%s() 要一个 string", nm);
                    e->t = T_ERR; break;
                }
                e->fn = f; e->resolved = 1; e->t = T_TEXT;
                break;
            }
            case BI_CONCAT: {
                EList *a;
                int i = 0;
                if (n < 1) { err(e->line, e->col, "CONCAT() 至少要 1 个参数"); e->t = T_ERR; break; }
                for (a = e->args; a; a = a->next, i++) {
                    if (a->e->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(a->e, "CONCAT")) { e->t = T_ERR; break; }
                    if (a->e->t != T_TEXT && !is_scalar(a->e->t)) {
                        err(a->e->line, a->e->col, "CONCAT() 的第 %d 个参数必须是 string 或 int", i + 1);
                        e->t = T_ERR; break;
                    }
                }
                if (e->t == T_ERR) break;
                e->fn = f; e->resolved = 1; e->t = T_TEXT;
                break;
            }
            case BI_RCL: {
                EList *a;
                if (n > 1) {
                    err(e->line, e->col, "rcl() 要么不写参数、要么写一个值，这里是 %d 个", n);
                    e->t = T_ERR; break;
                }
                for (a = e->args; a; a = a->next) {
                    if (a->e->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(a->e, "rcl")) { e->t = T_ERR; break; }
                    if (!is_scalar(a->e->t) && a->e->t != T_TEXT) {
                        err(a->e->line, a->e->col, "rcl() 要 int 或 string");
                        e->t = T_ERR; break;
                    }
                }
                if (e->t == T_ERR) break;
                e->fn = f; e->resolved = 1; e->t = T_NUM;
                break;
            }
            case BI_FIND: {
                Expr *t0, *t1;
                if (n < 2 || n > 3) {
                    err(e->line, e->col, "FIND() 要 2 个或 3 个参数（文字、要找的、从第几位开始），这里是 %d 个", n);
                    e->t = T_ERR; break;
                }
                t0 = e->args->e; t1 = e->args->next->e;
                if (t0->t == T_ERR || t1->t == T_ERR) { e->t = T_ERR; break; }
                if (err_if_array(t0, "FIND") || err_if_array(t1, "FIND")) { e->t = T_ERR; break; }
                if (t0->t != T_TEXT) {
                    err(t0->line, t0->col, "FIND() 的第一个参数必须是 string");
                    e->t = T_ERR; break;
                }
                if (t1->t != T_TEXT) {
                    err(t1->line, t1->col, "FIND() 的第二个参数必须是 string");
                    e->t = T_ERR; break;
                }
                if (n == 3) {
                    Expr *t2 = e->args->next->next->e;
                    if (t2->t == T_ERR) { e->t = T_ERR; break; }
                    if (err_if_array(t2, "FIND")) { e->t = T_ERR; break; }
                    if (!is_intlike(t2->t)) {
                        err(t2->line, t2->col, "FIND() 的第三个参数必须是 int");
                        e->t = T_ERR; break;
                    }
                }
                e->fn = f; e->resolved = 1; e->t = T_NUM;
                break;
            }
            default:
                handled = 0;   /* fixed-shape built-ins (len, sqrt, sin, ...)
                                 * use the normal argument check below */
                break;
            }
            if (handled) break;
        }
        if (n != f->nparam) {
            err(e->line, e->col, "%s() 要 %d 个参数，但是写了 %d 个", e->sv, f->nparam, n);
            e->t = T_ERR;
            break;
        }
        {
            int i = 0;
            for (l = e->args; l; l = l->next, i++) {
                if (!assignable(f->pt[i], l->e->t) && l->e->t != T_ERR)
                    err(l->e->line, l->e->col, "参数 %d（%s()）应该是 %s，但实际是 %s",
                        i + 1, e->sv, type_name(f->pt[i]), type_name(l->e->t));
                if (f->pt[i] == T_LETTER && l->e->t == T_NUM) check_letter_lit(l->e, "参数");
            }
        }
        e->fn = f;
        e->resolved = 1;
        e->t = f->ret;
        break;
    }
    case E_UN:
    case E_POST: {
        Type ta = etype(e->a);
        const char *op = e->sv;
        if (ta == T_ERR) { e->t = T_ERR; break; }
        if (op[1] == 0 && (op[0] == '-' || op[0] == '+')) {
            if (!is_scalar(ta)) { err(e->line, e->col, "'%s' 要 int 或 float，不要 %s", op, type_name(ta)); e->t = T_ERR; break; }
            if (op[0] == '-' && e->a->k == E_NUM) { e->iv = -e->a->iv; e->k = E_NUM; e->t = T_NUM; break; }
            if (op[0] == '-' && e->a->k == E_REAL) { e->dv = -e->a->dv; e->k = E_REAL; e->t = T_REAL; break; }
            e->t = (ta == T_REAL) ? T_REAL : T_NUM;
            break;
        }
        if (op[0] == '!') {
            if (!is_scalar(ta)) { err(e->line, e->col, "'!' 要 int/float/bool/char"); e->t = T_ERR; break; }
            e->t = T_BOOL;
            break;
        }
        if (op[0] == '~') {
            if (!is_intlike(ta)) { err(e->line, e->col, "'~' 不能用在 %s 上", type_name(ta)); e->t = T_ERR; break; }
            e->t = T_NUM;
            break;
        }
        /* ++ / -- (prefix and postfix) */
        if (e->k == E_POST && !lvalue_ok(e->a)) { err(e->line, e->col, "'%s' 要作用在一个变量上", op); e->t = T_ERR; break; }
        if (e->k == E_POST && e->a->k == E_VAR && e->a->sym->kind == SK_CONST) { err(e->line, e->col, "常量不能改"); e->t = T_ERR; break; }
        if (!lvalue_ok(e->a)) { err(e->line, e->col, "'%s' 要作用在一个变量上", op); e->t = T_ERR; break; }
        if (e->a->k == E_VAR && e->a->sym->kind == SK_CONST) { err(e->line, e->col, "常量不能改"); e->t = T_ERR; break; }
        if (!(is_scalar(ta))) { err(e->line, e->col, "'%s' 不能用在 %s 上", op, type_name(ta)); e->t = T_ERR; break; }
        e->t = ta;
        break;
    }
    case E_BIN: {
        Type ta = etype(e->a), tb = etype(e->b);
        const char *op = e->sv;
        if (ta == T_ERR || tb == T_ERR) { e->t = T_ERR; break; }
        if (op[0] == '+' && op[1] == 0 && (ta == T_TEXT || tb == T_TEXT)) {
            if (ta == T_TEXT && tb == T_TEXT) { /* ok */ }
            else if (ta == T_TEXT) { if (!is_scalar(tb)) err(e->line, e->col, "string 只能和 int/float/bool/char 拼接，不能和 %s 拼接", type_name(tb)); }
            else { if (!is_scalar(ta)) err(e->line, e->col, "string 只能和 int/float/bool/char 拼接，不能和 %s 拼接", type_name(ta)); }
            e->t = T_TEXT;
            break;
        }
        if ((op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/' || op[0] == '%') && op[1] == 0) {
            if (ta == T_TEXT || tb == T_TEXT) { err(e->line, e->col, "string 不能用 '%s'", op); e->t = T_ERR; break; }
            if (!is_scalar(ta) || !is_scalar(tb)) { err(e->line, e->col, "'%s' 要 int", op); e->t = T_ERR; break; }
            if (op[0] == '%' && (ta == T_REAL || tb == T_REAL)) { err(e->line, e->col, "'%%' 不能用在 float 上"); e->t = T_ERR; break; }
            e->t = (ta == T_REAL || tb == T_REAL) ? T_REAL : T_NUM;
            break;
        }
        if ((op[0] == '=' && op[1] == '=') || (op[0] == '!' && op[1] == '=') ||
            (op[0] == '<' && op[1] == 0) || (op[0] == '>' && op[1] == 0) ||
            (op[0] == '<' && op[1] == '=') || (op[0] == '>' && op[1] == '=')) {
            if ((ta == T_TEXT) != (tb == T_TEXT)) { err(e->line, e->col, "string 只能和 string 比较"); e->t = T_ERR; break; }
            if (ta != T_TEXT && (!is_scalar(ta) || !is_scalar(tb))) { err(e->line, e->col, "'%s' 要 int", op); e->t = T_ERR; break; }
            e->t = T_BOOL;
            break;
        }
        if ((op[0] == '&' && op[1] == '&') || (op[0] == '|' && op[1] == '|')) {
            if (!is_scalar(ta) || !is_scalar(tb)) { err(e->line, e->col, "'%s' 要 int", op); e->t = T_ERR; break; }
            e->t = T_BOOL;
            break;
        }
        if (op[0] == '&' || op[0] == '|' || op[0] == '^' ||
            (op[0] == '<' && op[1] == '<') || (op[0] == '>' && op[1] == '>')) {
            if (!is_intlike(ta) || !is_intlike(tb)) { err(e->line, e->col, "'%s' 要 int（不能是 float）", op); e->t = T_ERR; break; }
            e->t = T_NUM;
            break;
        }
        err(e->line, e->col, "认不出的运算符 '%s'", op);
        e->t = T_ERR;
        break;
    }
    case E_ASSIGN: {
        Type tt = etype(e->a), tv = etype(e->b);
        if (tt == T_ERR || tv == T_ERR) { e->t = T_ERR; break; }
        if (!lvalue_ok(e->a)) { err(e->line, e->col, "'=' 左边必须是变量或数组项"); e->t = T_ERR; break; }
        if (e->a->k == E_VAR && e->a->sym->kind == SK_CONST) { err(e->line, e->col, "常量不能改"); e->t = T_ERR; break; }
        if (!assignable(tt, tv)) { err(e->line, e->col, "不能把 %s 放进 %s", type_name(tv), type_name(tt)); e->t = T_ERR; break; }
        if (tt == T_LETTER && tv == T_NUM) check_letter_lit(e->b, "赋值");
        e->t = tt;
        break;
    }
    case E_LIST:
        err(e->line, e->col, "'{ ... }' 只能在声明数组的时候用");
        e->t = T_ERR;
        break;
    }
    if (e->t == T_UNKNOWN) e->t = T_ERR;
    return e->t;
}

static void check_list(Expr *e, Type base)
{
    EList *l;
    e->t = base;
    for (l = e->args; l; l = l->next) {
        if (l->e->k == E_LIST) { check_list(l->e, base); continue; }
        {
            Type t = etype(l->e);
            if (t == T_ERR) continue;
            if (!assignable(base, t))
                err(l->e->line, l->e->col, "列表项是 %s，但这里�?%s", type_name(t), type_name(base));
            if (base == T_LETTER && t == T_NUM) check_letter_lit(l->e, "列表项");
        }
    }
}

/* ---------------- expression emitter ---------------- */
static void eexpr(Expr *e);
static void emit_converted(Type dst, Expr *v);

static void emit_ftos(Expr *v)
{
    switch (v->t) {
    case T_NUM: w("cask_ftos("); eexpr(v); w(")"); break;
    case T_REAL: w("cask_ftos_r("); eexpr(v); w(")"); break;
    case T_BOOL: w("cask_ftos_b("); eexpr(v); w(")"); break;
    case T_LETTER: w("cask_ftos_c("); eexpr(v); w(")"); break;
    default: eexpr(v);
    }
}

/* MAX(a, b, c) -> cask_max2(cask_max2(a, b), c): every value is
 * evaluated exactly once, unlike a plain ?: chain.               */
static void emit_minmax_args(EList *l, int is_max, int real)
{
    if (!l->next) { eexpr(l->e); return; }
    w(is_max ? "cask_max2" : "cask_min2");
    w(real ? "_r(" : "(");
    eexpr(l->e);
    w(", ");
    emit_minmax_args(l->next, is_max, real);
    w(")");
}

/* CONCAT(a, b, c) -> cask_cat(a, cask_cat(b, c)) (values in order) */
static void emit_concat_args(EList *l)
{
    if (!l->next) { emit_ftos(l->e); return; }
    w("cask_cat(");
    emit_ftos(l->e);
    w(", ");
    emit_concat_args(l->next);
    w(")");
}

/* the real C function behind a math built-in: the C* name may be
 * the Excel-style spelling (POWER, SQRT, EXP, ...) or the C one   */
static const char *bi_cname(int bid)
{
    switch (bid) {
    case BI_SQRT: return "sqrt";
    case BI_POW: return "pow";
    case BI_SIN: return "sin";
    case BI_COS: return "cos";
    case BI_TAN: return "tan";
    case BI_ASIN: return "asin";
    case BI_ACOS: return "acos";
    case BI_ATAN: return "atan";
    case BI_LOG: return "log";
    case BI_LOG10: return "log10";
    case BI_EXP: return "exp";
    case BI_FLOOR: return "floor";
    case BI_CEIL: return "ceil";
    case BI_ROUND: return "round";
    case BI_TRUNC: return "trunc";
    case BI_ATAN2: return "atan2";
    default: return "sqrt";
    }
}

static void emit_call(Expr *e)
{
    Func *f = e->fn;
    if (e->native_sym) {
        /* a function from the C / C++ / Win32 headers */
        EList *l;
        int i = 0;
        w("%s(", e->sv);
        for (l = e->args; l; l = l->next, i++) {
            if (l != e->args) w(", ");
            if (e->napi) emit_converted(e->napi->par[i], l->e);
            else eexpr(l->e);
        }
        w(")");
        return;
    }
    if (!f->is_builtin) {
        EList *l;
        int i = 0;
        w("%s(", f->name);
        for (l = e->args; l; l = l->next, i++) {
            if (l != e->args) w(", ");
            emit_converted(f->pt[i], l->e);
        }
        w(")");
        return;
    }
    {
        EList *l = e->args;
        switch (f->bid) {
        case BI_LEN:
            w("cask_len("); eexpr(l->e); w(")");
            break;
        case BI_AT:
            w("cask_at("); eexpr(l->e); w(", ");
            eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_ABS:
            if (l->e->t == T_REAL) { w("fabs("); eexpr(l->e); w(")"); }
            else { w("cask_abs("); eexpr(l->e); w(", __FILE__, %d)", e->line); }
            break;
        case BI_FLOOR: case BI_CEIL: case BI_ROUND: case BI_TRUNC:
            w("(long long)%s(", bi_cname(f->bid)); eexpr(l->e); w(")");
            break;
        case BI_RANDOM:
            w("cask_random()");
            break;
        case BI_NOW:
            w("cask_now()");
            break;
        /* ---- Excel-style functions ---- */
        case BI_IF: {
            Expr *ca = l->e, *ya = l->next->e, *na = l->next->next->e;
            w("("); eexpr(ca); w(" ? ");
            if (e->t == T_TEXT) { emit_ftos(ya); w(" : "); emit_ftos(na); }
            else { eexpr(ya); w(" : "); eexpr(na); }
            w(")");
            break;
        }
        case BI_AND:
        case BI_OR: {
            const char *j = (f->bid == BI_AND) ? " && " : " || ";
            w("(");
            for (; l; l = l->next) {
                if (l != e->args) w("%s", j);
                eexpr(l->e);
            }
            w(")");
            break;
        }
        case BI_NOT:
            w("(!"); eexpr(l->e); w(")");
            break;
        case BI_XTRUE:
            w("1");
            break;
        case BI_XFALSE:
            w("0");
            break;
        case BI_SIGN:
            w(l->e->t == T_REAL ? "cask_sign_r(" : "cask_sign(");
            eexpr(l->e); w(")");
            break;
        case BI_XMOD:
            if (g_fast && e->t == T_NUM) {   /* --fast: plain remainder */
                w("("); eexpr(l->e); w(" %% "); eexpr(l->next->e); w(")");
            } else {
                w(e->t == T_REAL ? "cask_fxmod(" : "cask_xmod(");
                eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            }
            break;
        case BI_XROUND:
            if (!l->next) {
                w("(long long)round("); eexpr(l->e); w(")");
            } else {
                w("cask_xround(");
                eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            }
            break;
        case BI_INT:
            if (l->e->t == T_REAL) { w("cask_int("); eexpr(l->e); w(")"); }
            else eexpr(l->e);          /* whole numbers stay as they are */
            break;
        case BI_XLN:
            w("cask_ln("); eexpr(l->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_XLOG:
            if (l->next) {
                w("cask_logb(");
                eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            } else {
                w("cask_log10e("); eexpr(l->e); w(", __FILE__, %d)", e->line);
            }
            break;
        case BI_PI:
            w("(3.141592653589793)");
            break;
        case BI_RANDBETWEEN:
            w("cask_randbetween(");
            eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_SUM:
        case BI_AVG: {
            int isreal = (l->e->sym->type == T_REAL);
            if (f->bid == BI_SUM)
                w(isreal ? "cask_sum_r(" : "cask_sum(");
            else
                w(isreal ? "cask_avg_r(" : "cask_avg(");
            eexpr(l->e);
            if (f->bid == BI_AVG || !isreal)
                w(", %lldLL, __FILE__, %d)", (long long)l->e->sym->dims[0], e->line);
            else
                w(", %lldLL)", (long long)l->e->sym->dims[0]);
            break;
        }
        case BI_COUNT:
            w("%lldLL", (long long)l->e->sym->dims[0]);
            break;
        case BI_MAX:
        case BI_MIN: {
            int is_max = (f->bid == BI_MAX);
            if (l->e->k == E_VAR && l->e->sym && l->e->sym->ndim > 0) {
                int isreal = (l->e->sym->type == T_REAL);
                if (is_max) w(isreal ? "cask_max_r(" : "cask_max(");
                else        w(isreal ? "cask_min_r(" : "cask_min(");
                eexpr(l->e);
                w(", %lldLL, __FILE__, %d)", (long long)l->e->sym->dims[0], e->line);
            } else {
                emit_minmax_args(l, is_max, e->t == T_REAL);
            }
            break;
        }
        case BI_LEFT:
            w("cask_left(");
            eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_RIGHT:
            w("cask_right(");
            eexpr(l->e); w(", "); eexpr(l->next->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_MID:
            w("cask_mid(");
            eexpr(l->e); w(", "); eexpr(l->next->e);
            w(", "); eexpr(l->next->next->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_UPPER:
            w("cask_upper("); eexpr(l->e); w(")");
            break;
        case BI_LOWER:
            w("cask_lower("); eexpr(l->e); w(")");
            break;
        case BI_TRIM:
            w("cask_trim("); eexpr(l->e); w(")");
            break;
        case BI_CONCAT:
            emit_concat_args(l);
            break;
        case BI_VAL:
            w("cask_val("); eexpr(l->e); w(")");
            break;
        case BI_RCL:                    /* easter egg: rcl 是方�?35的女朋友 */
            if (!e->args) w("cask_rcl_ask()");
            else if (e->args->e->t == T_TEXT) {
                w("cask_rcl_text("); eexpr(e->args->e); w(")");
            } else {
                w("cask_rcl_num("); eexpr(e->args->e); w(")");
            }
            break;
        case BI_LOG10:
            w("cask_log10e("); eexpr(l->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_FIND:
            w("cask_find(");
            eexpr(l->e); w(", "); eexpr(l->next->e);
            if (l->next->next) { w(", "); eexpr(l->next->next->e); }
            else w(", 1");
            w(", __FILE__, %d)", e->line);
            break;
        case BI_SUBSTITUTE:
            w("cask_substitute(");
            eexpr(l->e); w(", "); eexpr(l->next->e);
            w(", "); eexpr(l->next->next->e); w(")");
            break;
        case BI_REPT:
            w("cask_rept("); eexpr(l->e); w(", "); eexpr(l->next->e);
            w(", __FILE__, %d)", e->line);
            break;
        case BI_EXACT:
            w("cask_exact("); eexpr(l->e); w(", "); eexpr(l->next->e); w(")");
            break;
        case BI_CHAR:
            w("cask_char_code("); eexpr(l->e); w(", __FILE__, %d)", e->line);
            break;
        case BI_CODE:
            w("cask_code("); eexpr(l->e); w(")");
            break;
        default: /* sqrt,pow,sin,... all map 1:1 to C math.h */
            w("%s(", bi_cname(f->bid));
            for (; l; l = l->next) {
                if (l != e->args) w(", ");
                eexpr(l->e);
            }
            w(")");
            break;
        }
    }
}

static void emit_un(Expr *e)
{
    const char *op = e->sv;
    Expr *t = e->a;
    if (op[0] == '-' && op[1] == 0) {
        if (e->t == T_NUM && t->k == E_NUM) { w("(%lldLL)", t->iv); return; }
        if (e->t == T_REAL && t->k == E_REAL) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.17g", t->dv);
            if (!strpbrk(tmp, ".eE")) strncat(tmp, ".0", sizeof tmp - strlen(tmp) - 1);
            w("(-%s)", tmp);
            return;
        }
        if (t->t == T_REAL || g_fast) { w("(-("); eexpr(t); w("))"); return; }
        w("cask_sub(0, "); eexpr(t); w(", __FILE__, %d)", e->line);
        return;
    }
    if (op[0] == '+' && op[1] == 0) { w("("); eexpr(t); w(")"); return; }
    if (op[0] == '!') { w("(!("); eexpr(t); w("))"); return; }
    if (op[0] == '~') { w("(~("); eexpr(t); w("))"); return; }
    /* ++ -- prefix */
    {
        int plus = (op[0] == '+');
        if (t->t == T_REAL) {
            w("("); eexpr(t); w(" = "); eexpr(t); w(" %s 1)", plus ? "+" : "-");
        } else if (t->t == T_LETTER) {
            w("("); eexpr(t); w(" = cask_ck_char(("); eexpr(t); w(") %s 1, __FILE__, %d))", plus ? "+" : "-", e->line);
        } else if (g_fast) {
            w("("); eexpr(t); w(" = "); eexpr(t); w(" %s 1)", plus ? "+" : "-");
        } else {
            w("("); eexpr(t); w(" = cask_%s(", plus ? "add" : "sub");
            eexpr(t); w(", 1LL, __FILE__, %d))", e->line);
        }
    }
}

static void emit_post(Expr *e)
{
    const char *op = e->sv;
    Expr *t = e->a;
    int plus = (op[0] == '+');
    if (t->t == T_REAL) {
        w("(("); eexpr(t); w(" = "); eexpr(t); w(" %s 1), ", plus ? "+" : "-");
        eexpr(t); w(" %s 1)", plus ? "-" : "+");
    } else if (t->t == T_LETTER) {
        w("(("); eexpr(t); w(" = cask_ck_char(("); eexpr(t); w(") %s 1, __FILE__, %d)), ",
          plus ? "+" : "-", e->line);
        eexpr(t); w(" %s 1)", plus ? "-" : "+");
    } else if (g_fast) {
        w("(("); eexpr(t); w(" = "); eexpr(t); w(" %s 1), ", plus ? "+" : "-");
        eexpr(t); w(" %s 1)", plus ? "-" : "+");
    } else {
        w("(("); eexpr(t); w(" = cask_%s(", plus ? "add" : "sub");
        eexpr(t); w(", 1LL, __FILE__, %d)), ", e->line);
        eexpr(t); w(" %s 1LL)", plus ? "-" : "+");
    }
}

static void emit_bin(Expr *e)
{
    const char *op = e->sv;
    Type ta = e->a->t, tb = e->b->t;

    if (op[0] == '+' && op[1] == 0 && (ta == T_TEXT || tb == T_TEXT)) {
        if (ta == T_TEXT && tb == T_TEXT) {
            w("cask_cat("); eexpr(e->a); w(", "); eexpr(e->b); w(")");
        } else if (ta == T_TEXT) {
            w("cask_cat("); eexpr(e->a); w(", "); emit_ftos(e->b); w(")");
        } else {
            w("cask_cat("); emit_ftos(e->a); w(", "); eexpr(e->b); w(")");
        }
        return;
    }
    if (ta == T_TEXT && tb == T_TEXT &&
        ((op[0] == '=' && op[1] == '=') || (op[0] == '!' && op[1] == '=') ||
         (op[0] == '<' && op[1] == 0) || (op[0] == '>' && op[1] == 0) ||
         (op[0] == '<' && op[1] == '=') || (op[0] == '>' && op[1] == '='))) {
        w("(strcmp("); eexpr(e->a); w(", "); eexpr(e->b); w(") %s 0)", op);
        return;
    }
    if ((op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/' || op[0] == '%') && op[1] == 0) {
        if (ta == T_REAL || tb == T_REAL) {
            if (op[0] == '/') {
                if (g_fast) { w("("); eexpr(e->a); w(" / "); eexpr(e->b); w(")"); }
                else { w("cask_fdiv("); eexpr(e->a); w(", "); eexpr(e->b); w(", __FILE__, %d)", e->line); }
            } else {
                w("("); eexpr(e->a); w(" %s ", op); eexpr(e->b); w(")");
            }
            return;
        }
        if (g_fast) { w("("); eexpr(e->a); w(" %s ", op); eexpr(e->b); w(")"); return; }
        {
            const char *fn = op[0] == '+' ? "cask_add" :
                             op[0] == '-' ? "cask_sub" :
                             op[0] == '*' ? "cask_mul" :
                             op[0] == '/' ? "cask_div" : "cask_mod";
            w("%s(", fn); eexpr(e->a); w(", "); eexpr(e->b); w(", __FILE__, %d)", e->line);
        }
        return;
    }
    if ((op[0] == '<' && op[1] == '<') || (op[0] == '>' && op[1] == '>')) {
        if (g_fast) { w("("); eexpr(e->a); w(" %s ", op); eexpr(e->b); w(")"); return; }
        w("cask_sh%c(", op[0] == '<' ? 'l' : 'r');
        eexpr(e->a); w(", "); eexpr(e->b); w(", __FILE__, %d)", e->line);
        return;
    }
    /* comparisons, logical, bitwise & | ^: plain C, no overflow risk */
    w("("); eexpr(e->a); w(" %s ", op); eexpr(e->b); w(")");
}

static void emit_converted(Type dst, Expr *v)
{
    Type vt = v->t;
    if (dst == T_LETTER && vt == T_NUM) {
        if (v->k == E_NUM && v->iv >= -128 && v->iv <= 127) { eexpr(v); return; }
        w("cask_ck_char("); eexpr(v); w(", __FILE__, %d)", v->line);
        return;
    }
    if (dst == T_BOOL && vt != T_BOOL && vt != T_ERR) {
        w("("); eexpr(v); w(") != 0");
        return;
    }
    eexpr(v);
}

static void eexpr(Expr *e)
{
    switch (e->k) {
    case E_NUM:
        w("%lldLL", e->iv);
        break;
    case E_REAL: {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%.17g", e->dv);
        if (!strpbrk(tmp, ".eE")) strncat(tmp, ".0", sizeof tmp - strlen(tmp) - 1);
        w("(%s)", tmp);
        break;
    }
    case E_STR:
        emit_cstr(g_out, e->sv, e->svlen);
        break;
    case E_CHR:
        emit_cchr(g_out, (unsigned char)e->iv);
        break;
    case E_TRUE:
        w("1");
        break;
    case E_FALSE:
        w("0");
        break;
    case E_VAR:
        w("%s", e->sym ? e->sym->name : e->sv);
        break;
    case E_IDX:
        eexpr(e->a);
        if (g_fast || e->litidx) { w("["); eexpr(e->b); w("]"); }
        else { w("[cask_ckidx("); eexpr(e->b); w(", %lld, __FILE__, %d)]", e->dim, e->line); }
        break;
    case E_LIST: {
        EList *l;
        w("{");
        for (l = e->args; l; l = l->next) {
            if (l != e->args) w(", ");
            if (l->e->k == E_LIST) eexpr(l->e);
            else emit_converted(e->t, l->e);
        }
        w("}");
        break;
    }
    case E_CALL:
        emit_call(e);
        break;
    case E_UN:
        emit_un(e);
        break;
    case E_POST:
        emit_post(e);
        break;
    case E_BIN:
        emit_bin(e);
        break;
    case E_ASSIGN:
        w("("); eexpr(e->a); w(" = "); emit_converted(e->a->t, e->b); w(")");
        break;
    }
}

/* ============================================================
 * PART4: statement parser, prescan, analysis walk, code emitter
 * ============================================================ */

static SList *parse_block_stmts(Scope *sc, int mode);
static Stmt *parse_stmt(Scope *sc);
static void analyze_slist(SList *l, Scope *sc);
static void emit_slist(SList *l);
static void emit_plain(Expr *e);
static void emit_plain_conv(Type dst, Expr *v);

/* const names are globally unique; variables may not shadow them */
static char **g_constnames = NULL;
static int g_nconstnames = 0;

static int const_name_exists(const char *n)
{
    int i;
    for (i = 0; i < g_nconstnames; i++)
        if (strcmp(g_constnames[i], n) == 0) return 1;
    return 0;
}
static void add_const_name(const char *n)
{
    g_constnames = (char **)xrealloc(g_constnames, sizeof(char *) * (g_nconstnames + 1));
    g_constnames[g_nconstnames++] = xstrdup(n);
}

/* register a variable/const/param in `sc`; returns the symbol */
static Sym *reg_var(Scope *sc, const char *name, SymKind kind, Type t, int line, int is_global)
{
    Sym *s;
    if (kind == SK_CONST) {
        if (const_name_exists(name))
            perr_s(SEV_SEVERE, "常量名 '%s' 在程序里已经用过了", name);
        add_const_name(name);
    } else {
        Sym *old = is_global ? scope_find_local(g_gscope, name) : NULL;
        if (const_name_exists(name))
            perr_s(SEV_SEVERE, "'%s' 已经是程序里某个常量的名字了，请换一个名字", name);
        if (old && old->line == line) return old;   /* prescan already registered it */
        if (old)
            perr_s(SEV_SEVERE, "全局的 '%s' 已经声明过了（在第 %d 行）", name, old->line);
        if (scope_find_local(sc, name))
            perr_s(SEV_SEVERE, "'%s' 在这里已经声明过了", name);
    }
    s = sym_new(name, kind, t, line, is_global);
    scope_add(sc, s);
    return s;
}

/* ---------------- constant folding ---------------- */
static int fold_const(Expr *e, long long *out)
{
    if (!e) return 0;
    switch (e->k) {
    case E_NUM: *out = e->iv; return 1;
    case E_CHR: *out = e->iv; return 1;
    case E_TRUE: *out = 1; return 1;
    case E_FALSE: *out = 0; return 1;
    case E_UN: {
        long long v;
        if (!fold_const(e->a, &v)) return 0;
        switch (e->sv[0]) {
        case '-': *out = (long long)(0ULL - (unsigned long long)v); return 1;
        case '+': *out = v; return 1;
        case '!': *out = !v; return 1;
        case '~': *out = ~v; return 1;
        default: return 0;
        }
    }
    case E_BIN: {
        long long a, b;
        const char *op;
        if (!fold_const(e->a, &a) || !fold_const(e->b, &b)) return 0;
        op = e->sv;
        if (op[1] == 0) {
            switch (op[0]) {
            case '+': *out = (long long)((unsigned long long)a + (unsigned long long)b); return 1;
            case '-': *out = (long long)((unsigned long long)a - (unsigned long long)b); return 1;
            case '*': *out = (long long)((unsigned long long)a * (unsigned long long)b); return 1;
            case '/': if (b == 0) return 0; *out = a / b; return 1;
            case '%': if (b == 0) return 0; *out = a % b; return 1;
            case '<': *out = a < b; return 1;
            case '>': *out = a > b; return 1;
            case '&': *out = a & b; return 1;
            case '|': *out = a | b; return 1;
            case '^': *out = a ^ b; return 1;
            default: return 0;
            }
        }
        if (!strcmp(op, "==")) { *out = a == b; return 1; }
        if (!strcmp(op, "!=")) { *out = a != b; return 1; }
        if (!strcmp(op, "<=")) { *out = a <= b; return 1; }
        if (!strcmp(op, ">=")) { *out = a >= b; return 1; }
        if (!strcmp(op, "&&")) { *out = a && b; return 1; }
        if (!strcmp(op, "||")) { *out = a || b; return 1; }
        if (!strcmp(op, "<<")) { if (b < 0 || b > 63) return 0; *out = (long long)((unsigned long long)a << b); return 1; }
        if (!strcmp(op, ">>")) { if (b < 0 || b > 63) return 0; *out = a >> b; return 1; }
        return 0;
    }
    case E_VAR:
        if (e->sym && e->sym->kind == SK_CONST && e->sym->has_cval) { *out = e->sym->cval; return 1; }
        return 0;
    default:
        return 0;
    }
}

/* analyze a constant initialiser: typecheck + static check + fold.
 * Called while parsing (so later code can use the value, e.g. array sizes). */
static void analyze_const_init(Sym *sy, Expr *init)
{
    if (sy && init) sy->cinit = init;      /* asm backend folds real/text consts at emission */
    etype(init);
    if (init->t == T_ERR) return;
    if (!assignable(sy->type, init->t))
        err(init->line, init->col, "常量 '%s' �?%s，但它的值是 %s",
            sy->name, type_name(sy->type), type_name(init->t));
    check_static_init(init, "常量");
    if (g_errors) return;
    if (sy->type == T_NUM) {
        long long v;
        if (!fold_const(init, &v))
            err(init->line, init->col,
                "常量 '%s' 必须是由 int 和其它常量算出来的整数值",
                sy->name);
        else { sy->cval = v; sy->has_cval = 1; }
    }
}

/* parse a dimension:  [ 5 ]  or  [ ROWS ] (constant number) */
static long long parse_dim(Scope *sc)
{
    long long v;
    if (cur()->k == T_INT) {
        v = at(0)->iv;
        g_pi++;
    } else if (cur()->k == T_ID) {
        Sym *cv = scope_find(sc, at(0)->text);
        if (!cv || cv->kind != SK_CONST || cv->type != T_NUM || !cv->has_cval)
            perr_s(SEV_SEVERE, "数组大小必须是 int 或常量 int（这里是 '%s'）", at(0)->text);
        v = cv->cval;
        g_pi++;
    } else {
        perr_s(SEV_SEVERE, "数组大小必须是 int，例如 [10]");
        v = 0;
    }
    if (v < 1) perr_s(SEV_SEVERE, "数组大小至少要 1");
    if (v > 100000000LL) perr_s(SEV_SEVERE, "数组大小 %lld 太大了", v);
    return v;
}

/* ---------------- declarations ---------------- */
/* parses:  name [ '[' N ']' ]... [ '=' init ]    (caller handles ',' / ';') */
static void parse_declarator(Scope *sc, Type ty, int is_global, int is_const, Decl *out)
{
    Tok *t;
    int i;
    memset(out, 0, sizeof *out);
    t = expect_id("一个变量名");
    out->name = t->text;
    out->line = t->line;
    out->is_const = is_const;
    i = 0;
    while (is_op(cur(), "[")) {
        g_pi++;
        if (i >= 8) perr_s(SEV_SEVERE, "[ ... ] 的层数太多了（最多 8 层）");
        out->dims[i++] = (int)parse_dim(sc);
        expect_op("]", "它用来关掉数组大小 [ ... ]");
    }
    out->ndim = i;
    if (is_const && i > 0) perr_s(SEV_SEVERE, "常量不能是数组（暂时还不支持）");
    if (is_op(cur(), "=")) {
        g_pi++;
        if (i > 0) out->init = parse_init_list();
        else out->init = parse_expr();
    } else if (i > 0 && !is_op(cur(), ",") && !is_op(cur(), ";")) {
        perr("'%s' 后面要写 ';' 或 ','", out->name);
    }
    out->sym = reg_var(sc, out->name, is_const ? SK_CONST : SK_VAR, ty, out->line, is_global);
    if (out->sym && out->ndim > 0) {
        int k;
        out->sym->ndim = out->ndim;
        for (k = 0; k < out->ndim; k++) out->sym->dims[k] = out->dims[k];
    }
    if (is_const && out->init) {
        Scope *save = g_ascope;
        g_ascope = sc;
        analyze_const_init(out->sym, out->init);
        g_ascope = save;
    } else if (is_const && !out->init) {
        perr("常量要有值: constant %s = ...;", out->name);
    }
}

/* parses:  TYPE d [, d]* ;      returns a S_DECL statement */
static Stmt *parse_decl(Scope *sc, int is_global, int is_const)
{
    Tok *t = cur();
    Stmt *s;
    Type ty;
    if (t->k != T_OP || !is_type_kw(t->kw) || t->kw == KW_VOID)
        perr("这里要一个类型（int/float/string/char/bool），但是碰到了 %s", tok_show(t));
    ty = type_of_kw(t->kw);
    g_pi++;
    s = stmt_new(S_DECL, t->line);
    s->dtype = ty;
    for (;;) {
        Decl d;
        s->decls = (Decl *)xrealloc(s->decls, sizeof(Decl) * (s->ndecl + 1));
        parse_declarator(sc, ty, is_global, is_const, &d);
        s->decls[s->ndecl++] = d;
        if (is_op(cur(), ",")) { g_pi++; continue; }
        break;
    }
    expect_op(";", "它给这条声明收尾");
    return s;
}

/* ---------------- statements ---------------- */
/* mode 0: a normal { ... } body -- stops at '}'
 * mode 1: the clauses of a switch -- also stops at case / default     */
static int clause_end(int mode)
{
    if (is_op(cur(), "}")) return 1;
    if (mode == 1 && (is_kwt(0, KW_CASE) || is_kwt(0, KW_DEFAULT))) return 1;
    return 0;
}

static SList *parse_block_stmts(Scope *sc, int mode)
{
    /* body inside { ... } (braces already consumed) */
    SList *outer = g_reclist;      /* the caller's list  */
    int saved = g_rec_level;       /* the caller's level */
    int saved_case = g_rec_case;
    SList *l;
    g_reclist = NULL;
    g_rec_level = 1;
    g_rec_case = (mode == 1);
    while (!clause_end(mode)) {
        int r, start = g_pi;
        if (cur()->k == T_EOF) {
            /* jump out -- put the caller's context back first, so its
             * recovery (or a clean stop) sees consistent state */
            g_rec_level = saved;
            g_reclist = outer;
            perr("文件结尾之前少了 '}'");
        }
        r = setjmp(g_recover);
        if (r == 0) {
            slist_add(&g_reclist, parse_stmt(sc));
        } else if (r == 2 && g_pi > start) {
            /* a forgotten ';' : the statement itself was read, so just
             * start the next one right here (nothing is swallowed) */
        } else {
            panic_skip(0);                  /* broken statement: skip it */
            if (cur()->k == T_EOF) break;
        }
    }
    l = g_reclist;
    g_rec_level = saved;
    g_reclist = outer;
    g_rec_case = saved_case;
    /* a normal block owns the '}' it stops at; a switch clause list
     * leaves it for the switch parser to consume                     */
    if (mode == 0 && is_op(cur(), "}")) g_pi++;
    return l;
}

static SList *parse_slist_block(Scope *sc)
{
    /* '{' at cur() */
    Scope *ns = scope_push(sc);
    Stmt *b;
    g_pi++;
    b = stmt_new(S_BLOCK, cur()->line);
    b->scope = ns;
    b->body = parse_block_stmts(ns, 0);
    {
        SList *l = (SList *)xmalloc(sizeof *l);
        l->s = b; l->next = NULL;
        return l;
    }
}

static SList *parse_onestmt_or_block(Scope *sc)
{
    if (is_op(cur(), "{")) return parse_slist_block(sc);
    {
        SList *l = (SList *)xmalloc(sizeof *l);
        l->s = parse_stmt(sc);
        l->next = NULL;
        return l;
    }
}

static Stmt *parse_stmt(Scope *sc)
{
    Tok *t = cur();
    Stmt *s;
    if (is_op(t, "{")) {
        SList *l = parse_slist_block(sc);
        return l->s;
    }
    if (is_op(t, ";")) { g_pi++; return stmt_new(S_EMPTY, t->line); }
    if (t->k == T_OP && t->kw >= 0) {
        switch (t->kw) {
        case KW_IF: {
            Expr *cond;
            g_pi++;
            s = stmt_new(S_IF, t->line);
            expect_op("(", "它要写在 if 后面");
            cond = parse_expr();
            expect_op(")", "它用来关掉 if ( ... )");
            s->cond = cond;
            s->body = parse_onestmt_or_block(sc);
            if (is_kwt(0, KW_ELSE)) {
                g_pi++;
                s->elseb = parse_onestmt_or_block(sc);
            }
            return s;
        }
        case KW_WHILE: {
            g_pi++;
            s = stmt_new(S_WHILE, t->line);
            expect_op("(", "它要写在 while 后面");
            s->cond = parse_expr();
            expect_op(")", "它用来关掉 while ( ... )");
            s->body = parse_onestmt_or_block(sc);
            return s;
        }
        case KW_UNTIL: {            /* keep going until it becomes true */
            g_pi++;
            s = stmt_new(S_UNTIL, t->line);
            expect_op("(", "它要写在 until 后面");
            s->cond = parse_expr();
            expect_op(")", "它用来关掉 until ( ... )");
            s->body = parse_onestmt_or_block(sc);
            return s;
        }
        case KW_DO: {               /* do ... while: runs at least once */
            g_pi++;
            s = stmt_new(S_DO, t->line);
            s->body = parse_onestmt_or_block(sc);
            if (!is_kwt(0, KW_WHILE))
                perr("'do' 的语句体后面要写 'while ( ... );'");
            g_pi++;
            expect_op("(", "它要写在 while 后面");
            s->cond = parse_expr();
            expect_op(")", "它用来关掉 while ( ... )");
            expect_op(";", "它要写在 do ... while ( ... ) 后面");
            return s;
        }
        case KW_FOREACH: {          /* foreach (item in items) { ... } */
            Tok *nm;
            Scope *ns;
            Stmt *b;
            g_pi++;
            s = stmt_new(S_FOREACH, t->line);
            expect_op("(", "它要写在 foreach 后面");
            nm = expect_id("循环变量名，例如  foreach (score in scores) { ... }");
            if (!is_kwt(0, KW_IN))
                perr("循环变量后面要写 'in'，例如  foreach (score in scores) { ... }");
            g_pi++;
            s->cond = parse_expr();
            expect_op(")", "它用来关掉 foreach ( ... )");
            if (!is_op(cur(), "{")) perr("'foreach' 后面要跟 { ... } 语句体");
            g_pi++;                         /* consume '{' */
            ns = scope_push(sc);            /* the variable lives in the body */
            s->isyms = reg_var(ns, nm->text, SK_VAR, T_ERR, nm->line, 0);
            s->iname = xstrdup(nm->text);
            b = stmt_new(S_BLOCK, t->line);
            b->scope = ns;
            b->body = parse_block_stmts(ns, 0);
            {
                SList *l = (SList *)xmalloc(sizeof *l);
                l->s = b; l->next = NULL;
                s->body = l;
            }
            return s;
        }
        case KW_SWITCH: {           /* switch (v) { case 1: ... default: ... } */
            g_pi++;
            s = stmt_new(S_SWITCH, t->line);
            expect_op("(", "它要写在 switch 后面");
            s->cond = parse_expr();
            expect_op(")", "它用来关掉 switch ( ... )");
            expect_op("{", "它用来打开 switch 的大括号");
            while (!is_op(cur(), "}")) {
                if (cur()->k == T_EOF) perr("'switch' 少了结尾的 '}'");
                if (is_kwt(0, KW_CASE)) {
                    Case *c = (Case *)xmalloc(sizeof *c), *p;
                    memset(c, 0, sizeof *c);
                    c->line = cur()->line;
                    g_pi++;
                    c->val = parse_expr();
                    expect_op(":", "它要写在 case 的值后面");
                    c->body = parse_block_stmts(sc, 1);
                    if (!s->cases) s->cases = c;
                    else { for (p = s->cases; p->next; p = p->next) ; p->next = c; }
                    continue;
                }
                if (is_kwt(0, KW_DEFAULT)) {
                    if (s->defbody) perr_s(SEV_SEVERE, "'switch' 里只能有一个 'default'");
                    g_pi++;
                    expect_op(":", "它要写在 'default' 后面");
                    s->defbody = parse_block_stmts(sc, 1);
                    continue;
                }
                perr("'switch' 里面要写 'case <value>:' 或 'default:'，但是碰到了 %s",
                     tok_show(cur()));
            }
            g_pi++;                 /* consume '}' */
            return s;
        }
        case KW_FOR: {
            g_pi++;
            s = stmt_new(S_FOR, t->line);
            expect_op("(", "它要写在 for 后面");
            if (is_op(cur(), ";")) { g_pi++; s->initst = NULL; }
            else if (at_type(0, 0)) {
                if (is_kwt(0, KW_CONSTANT)) perr_s(SEV_SEVERE, "'for' 里面不能定义常量");
                s->initst = parse_decl(sc, 0, 0);
                /* parse_decl emitted ';' already; for needs the ';' removed */
                /* parse_decl consumed ';' -- fine */
            } else {
                Stmt *es = stmt_new(S_EXPR, cur()->line);
                es->val = parse_expr();
                expect_op(";", "它要写在 for 的第一段后面");
                s->initst = es;
            }
            if (is_op(cur(), ";")) { g_pi++; s->cond = NULL; }
            else {
                s->cond = parse_expr();
                expect_op(";", "它要写在 for 的条件后面");
            }
            if (is_op(cur(), ")")) { g_pi++; s->step = NULL; }
            else {
                s->step = parse_expr();
                expect_op(")", "它用来关掉 for ( ... ; ... ; ... )");
            }
            s->body = parse_onestmt_or_block(sc);
            return s;
        }
        case KW_RETURN: {
            g_pi++;
            s = stmt_new(S_RETURN, t->line);
            if (!is_op(cur(), ";")) s->val = parse_expr();
            expect_op(";", "它要写在 return 后面");
            return s;
        }
        case KW_BREAK:
            g_pi++;
            s = stmt_new(S_BREAK, t->line);
            expect_op(";", "它要写在 break 后面");
            return s;
        case KW_CONTINUE:
            g_pi++;
            s = stmt_new(S_CONTINUE, t->line);
            expect_op(";", "它要写在 continue 后面");
            return s;
        case KW_SAY:
        case KW_SHOW: {
            char msg[256];
            g_pi++;
            s = stmt_new(S_SAY, t->line);
            s->nl = (t->kw == KW_SAY);
            if (!is_op(cur(), ";")) {
                for (;;) {
                    elist_add(&s->args, parse_expr());
                    if (is_op(cur(), ",")) { g_pi++; continue; }
                    break;
                }
            }
            snprintf(msg, sizeof msg, "它要写在 %s 后面", t->text ? t->text : "print");
            expect_op(";", msg);
            return s;
        }
        case KW_INPUT: {
            g_pi++;
            s = stmt_new(S_INPUT, t->line);
            s->val = parse_expr();
            expect_op(";", "它要写在 input 后面");
            return s;
        }
        case KW_CONSTANT: {
            /* constant inside a function body */
            int is_const = 1;
            if (!at_type(1, 0)) perr("'constant' 后面要写一个类型");
            return parse_decl(sc, 0, is_const);
        }
        default:
            break;
        }
    }
    if (at_type(0, 0)) return parse_decl(sc, 0, 0);
    /* expression statement */
    s = stmt_new(S_EXPR, t->line);
    s->val = parse_expr();
    expect_op(";", "它给这条语句收尾");
    return s;
}

/* ---------------- top level ---------------- */
static char **g_uses = NULL;
static int g_nuses = 0;
static int g_has_start = 0, g_has_main = 0, g_start_line = 0;

static void add_use(const char *name)
{
    char buf[512];
    int i;
    /* C_OPED.cash -- the C* header that opens C / C++ / Win32 to the program */
    if (!strcmp(name, "C_OPED.cash") || !strcmp(name, "C_OPED.CASH")) {
        g_native = 1;
        name = "C_OPED.cash";
    }
    /* C_CSS.cash -- the CSS-like styling toolkit; it works on top of
     * C_OPED.cash and styles the controls that header creates */
    if (!strcmp(name, "C_CSS.cash") || !strcmp(name, "C_CSS.CASH")) {
        g_use_css = 1;
        name = "C_CSS.cash";
    }
    /* C_IO.cash -- files & system: read/write files, appdir, env, launchwait */
    if (!strcmp(name, "C_IO.cash") || !strcmp(name, "C_IO.CASH")) {
        g_use_io = 1;
        name = "C_IO.cash";
    }
    /* keep the caller's brackets:  <stdio.h>  or  "my_helper.h" */
    if (name[0] == '<') snprintf(buf, sizeof buf, "%s", name);
    else if (strstr(name, ".cash")) snprintf(buf, sizeof buf, "<%s>", name);
    else snprintf(buf, sizeof buf, "\"%s\"", name);
    for (i = 0; i < g_nuses; i++)
        if (strcmp(g_uses[i], buf) == 0) return;
    g_uses = (char **)xrealloc(g_uses, sizeof(char *) * (g_nuses + 1));
    g_uses[g_nuses++] = xstrdup(buf);
}

/* const definitions recorded by prescan: [sym, init expression] */
typedef struct { Sym *sym; Expr *init; } ConstRec;
static ConstRec *g_consts = NULL;
static int g_nconsts = 0;

static void add_constrec(Sym *s, Expr *init)
{
    g_consts = (ConstRec *)xrealloc(g_consts, sizeof(ConstRec) * (g_nconsts + 1));
    g_consts[g_nconsts].sym = s;
    g_consts[g_nconsts].init = init;
    g_nconsts++;
}

/* function parameter list parse: ( TYPE name, ... ) ; fills Func */
static void parse_param_list_into(Func *fn)
{
    int n = 0;
    expect_op("(", "它用来开始参数表");
    if (is_op(cur(), ")")) { g_pi++; }
    else {
        for (;;) {
            Tok *t = cur();
            Type pt;
            Tok *nm;
            if (t->k != T_OP || !is_type_kw(t->kw) || t->kw == KW_VOID)
                perr("第 %d 个参数: 这里要一个类型（int/float/string/char/bool）", n + 1);
            pt = type_of_kw(t->kw);
            g_pi++;
            nm = expect_id("一个参数名");
            if (is_op(cur(), "[")) perr_s(SEV_SEVERE, "还不支持数组参数（v0.1）");
            fn->pt = (Type *)xrealloc(fn->pt, sizeof(Type) * (n + 1));
            fn->pn = (char **)xrealloc(fn->pn, sizeof(char *) * (n + 1));
            fn->pt[n] = pt;
            fn->pn[n] = nm->text;
            n++;
            if (is_op(cur(), ",")) { g_pi++; continue; }
            break;
        }
        expect_op(")", "它用来关掉参数表");
    }
    fn->nparam = n;
}

/* skip a balanced { ... } block (the current token must be '{') */
static void skip_block(const char *what)
{
    int pd = 1;
    if (!is_op(cur(), "{")) perr("这里要写 '{'%s", what);
    g_pi++;
    while (pd > 0 && cur()->k != T_EOF) {
        if (is_op(cur(), "{")) pd++;
        else if (is_op(cur(), "}")) pd--;
        g_pi++;
    }
    if (pd > 0) perr("少了 '}'%s", what);
}

/* "function main() { ... }" -- the entry block.
 * The current token must be the '(' after 'main'. */
static Stmt *parse_main_block(int line)
{
    Stmt *s;
    g_pi++;                                  /* ( */
    if (!is_op(cur(), ")")) perr_s(SEV_SEVERE, "'function main' 不要参数");
    g_pi++;                                  /* ) */
    if (!is_op(cur(), "{")) perr("'main' 的语句体要用 '{' 开头");
    s = stmt_new(S_START, line);
    s->scope = scope_push(g_gscope);
    g_pi++;                                  /* { */
    s->body = parse_block_stmts(s->scope, 0);
    return s;
}

/* The current token must be the '(' after the function name.
 * Returns the S_FUNC node, or NULL for a prototype (nothing to add). */
static Stmt *parse_func_rest(Type ty, Tok *nm)
{
    Func *fn = func_find(nm->text);
    Stmt *s;
    Scope *sc;
    int i;
    if (fn) {
        if (fn->is_builtin) perr_s(SEV_SEVERE, "'%s' 是内置函数名", nm->text);
        if (fn->defined) perr_s(SEV_SEVERE, "函数 '%s' 已经定义过了（在第 %d 行）", nm->text, fn->line);
    } else {
        fn = func_new(nm->text);
        fn->ret = ty;
    }
    if (fn->ret != ty)
        perr_s(SEV_SEVERE, "'%s' 的返回类型和前面的声明不一致", nm->text);
    fn->line = nm->line;
    parse_param_list_into(fn);
    if (is_op(cur(), ";")) { fn->is_proto = 1; g_pi++; return NULL; }
    if (!is_op(cur(), "{")) perr("'%s' 的语句体要用 '{' 开头", nm->text);
    s = stmt_new(S_FUNC, nm->line);
    sc = scope_push(g_gscope);
    s->fn = fn;
    s->scope = sc;
    for (i = 0; i < fn->nparam; i++)
        reg_var(sc, fn->pn[i], SK_PARAM, fn->pt[i], nm->line, 0);
    g_pi++;
    s->body = parse_block_stmts(sc, 0);
    fn->defined = 1;
    return s;
}

static SList *parse_program(void)
{
    g_pi = 0;
    g_reclist = NULL;
    g_rec_level = 1;                 /* top level can recover too */
    while (cur()->k != T_EOF) {
        Tok *t = cur();
        int r, start = g_pi;
        r = setjmp(g_recover);
        if (r != 0) {                   /* perr() jumped back */
            if (!(r == 2 && g_pi > start))
                panic_skip(1);          /* skip the broken item ... */
            continue;                   /* ... and report the next */
        }
        /* use "header.h";  (already collected by prescan) */
        if (is_kwt(0, KW_USE)) {
            g_pi += 2;
            if (is_op(cur(), ";")) g_pi++;
            continue;
        }
        /* constant at top level (already analyzed; just skip) */
        if (is_kwt(0, KW_CONSTANT) && at_type(1, 0)) {
            g_pi += 2;
            expect_id("一个常量的名字");
            if (is_op(cur(), "[")) perr_s(SEV_SEVERE, "常量不能是数组");
            expect_op("=", "constant 定义里要有它");
            {
                int pd = 0;
                while (cur()->k != T_EOF) {
                    if (is_op(cur(), "(") || is_op(cur(), "[") || is_op(cur(), "{")) pd++;
                    else if (is_op(cur(), ")") || is_op(cur(), "]") || is_op(cur(), "}")) pd--;
                    else if (is_op(cur(), ";") && pd == 0) break;
                    g_pi++;
                }
            }
            expect_op(";", "它给 constant 定义收尾");
            continue;
        }
        /* start { ... } */
        if (is_kwt(0, KW_START)) {
            Stmt *s;
            g_pi++;
            if (!is_op(cur(), "{")) perr("'start' 后面必须写 '{'");
            s = stmt_new(S_START, t->line);
            s->scope = scope_push(g_gscope);
            g_pi++;
            s->body = parse_block_stmts(s->scope, 0);
            slist_add(&g_reclist, s);
            continue;
        }
        /* function main() { ... } -- the entry block
         * function name(...) { ... } -- a function with no return value */
        if (is_kwt(0, KW_FUNCTION)) {
            Tok *nm = at(1);
            if (nm->k != T_ID) perr("'function' 后面要一个名字");
            g_pi += 2;
            if (strcmp(nm->text, "main") == 0) {
                slist_add(&g_reclist, parse_main_block(nm->line));
                continue;
            }
            if (!is_op(cur(), "(")) perr("函数名 '%s' 后面要写 '('", nm->text);
            {
                Stmt *fs = parse_func_rest(T_VOID, nm);
                if (fs) slist_add(&g_reclist, fs);
            }
            continue;
        }
        /* function or global variable */
        if (at_type(0, 1)) {
            Type ty = type_of_kw(t->kw);
            Tok *nm = at(1);
            if (nm->k != T_ID) perr("类型 %s 后面要一个名字", t->text);
            if (is_op(at(2), "(")) {
                /* function definition (or prototype) */
                Stmt *fs;
                g_pi += 2;
                fs = parse_func_rest(ty, nm);
                if (fs) slist_add(&g_reclist, fs);
                continue;
            }
            /* global variable declaration */
            slist_add(&g_reclist, parse_decl(g_gscope, 1, 0));
            continue;
        }
        perr("顶层只允许写 'use'、'constant'、变量声明、函数和 'function main() { ... }'（这里是 %s）", tok_show(t));
    }
    g_rec_level = 0;
    return g_reclist;
}

/* ---------------- prescan ----------------
 * Walk all tokens once (skipping function bodies) to register:
 *   use headers, start, constants (with init token range),
 *   function signatures, global variables.
 * This lets bodies call functions and read globals declared later. */
static void prescan(void)
{
    int depth = 0;
    g_pi = 0;
    while (cur()->k != T_EOF) {
        Tok *t = cur();
        if (depth == 0 && is_kwt(0, KW_USE)) {
            Tok *s;
            g_pi++;
            s = cur();
            if (s->k != T_STR) perr("'use' 后面必须跟 \"file.h\"");
            add_use(s->text);
            if (g_native && !g_native_line) g_native_line = t->line;
            if (g_use_css && !g_css_line) g_css_line = t->line;
            g_pi++;
            if (is_op(cur(), ";")) g_pi++;
            else perr("'use' 后面要写 ';'");
            continue;
        }
        if (depth == 0 && is_kwt(0, KW_START)) {
            int pd;
            g_pi++;
            if (!is_op(cur(), "{")) perr("'start' 后面必须写 '{'");
            g_has_start = 1;
            g_start_line = t->line;
            pd = 1;
            g_pi++;
            while (pd > 0 && cur()->k != T_EOF) {
                if (is_op(cur(), "{")) pd++;
                else if (is_op(cur(), "}")) pd--;
                g_pi++;
            }
            if (pd > 0) perr("'start' 块少了 '}'");
            continue;
        }
        if (depth == 0 && is_kwt(0, KW_FUNCTION) && at(1)->k == T_ID) {
            Tok *nm = at(1);
            g_pi += 2;                            /* function, name */
            if (strcmp(nm->text, "main") == 0) {
                if (!is_op(cur(), "(")) perr("'function main' 后面要写 '()'");
                g_pi++;
                if (!is_op(cur(), ")")) perr_s(SEV_SEVERE, "'function main' 不要参数");
                g_pi++;
                g_has_start = 1;
                g_start_line = nm->line;
                skip_block("，也就是 'main' 的函数体");
                continue;
            }
            if (!is_op(cur(), "(")) perr("函数名 '%s' 后面要写 '('", nm->text);
            if (func_find(nm->text)) perr_s(SEV_SEVERE, "'%s' 已经是函数（或内置函数）名了", nm->text);
            {
                Func *fn = func_new(nm->text);
                fn->ret = T_VOID;
                fn->line = nm->line;
                parse_param_list_into(fn);
            }
            if (is_op(cur(), ";")) { g_pi++; continue; }
            if (!is_op(cur(), "{")) perr("'%s' 的参数表后面要写 '{'", nm->text);
            skip_block("，也就是函数的函数体");
            continue;
        }
        if (depth == 0 && is_kwt(0, KW_CONSTANT) && at_type(1, 0)) {
            Tok *ty = at(1), *nm;
            Type ct;
            Sym *sy;
            Expr *init;
            g_pi += 2;
            nm = expect_id("一个常量的名字");
            ct = type_of_kw(ty->kw);
            expect_op("=", "constant 定义里要有它");
            init = parse_expr();
            expect_op(";", "它给 constant 定义收尾");
            sy = reg_var(g_gscope, nm->text, SK_CONST, ct, nm->line, 1);
            g_ascope = g_gscope;
            analyze_const_init(sy, init);
            add_constrec(sy, init);
            continue;
        }
        if (depth == 0 && at_type(0, 1) && at(1)->k == T_ID && is_op(at(2), "(")) {
            Tok *nm = at(1);
            Type rt = type_of_kw(t->kw);
            Func *fn;
            int pd;
            if (func_find(nm->text)) perr_s(SEV_SEVERE, "'%s' 已经是函数（或内置函数）名了", nm->text);
            fn = func_new(nm->text);
            fn->ret = rt;
            fn->line = nm->line;
            g_pi += 2;
            parse_param_list_into(fn);
            if (is_op(cur(), ";")) { g_pi++; continue; }
            if (!is_op(cur(), "{")) perr("'%s' 的参数表后面要写 '{'", nm->text);
            pd = 1;
            g_pi++;
            while (pd > 0 && cur()->k != T_EOF) {
                if (is_op(cur(), "{")) pd++;
                else if (is_op(cur(), "}")) pd--;
                g_pi++;
            }
            if (pd > 0) perr("函数 '%s' 里少了 '}'", nm->text);
            if (strcmp(nm->text, "main") == 0) g_has_main = 1;
            continue;
        }
        if (depth == 0 && at_type(0, 0) && at(1)->k == T_ID) {
            Tok *nm = at(1);
            Type gt = type_of_kw(t->kw);
            Sym *sy;
            int i = 0;
            g_pi += 2;
            sy = reg_var(g_gscope, nm->text, SK_VAR, gt, nm->line, 1);
            while (is_op(cur(), "[")) {
                g_pi++;
                if (i >= 8) perr_s(SEV_SEVERE, "[ ... ] 的层数太多了（最多 8 层）");
                sy->dims[i++] = (int)parse_dim(g_gscope);
                expect_op("]", "它用来关掉数组大小 [ ... ]");
            }
            sy->ndim = i;
            {
                int pd = 0;
                while (cur()->k != T_EOF) {
                    if (is_op(cur(), "(") || is_op(cur(), "[") || is_op(cur(), "{")) pd++;
                    else if (is_op(cur(), ")") || is_op(cur(), "]") || is_op(cur(), "}")) pd--;
                    else if (is_op(cur(), ";") && pd == 0) break;
                    else if (pd == 0 && cur()->k == T_OP && cur()->kw >= 0 &&
                             cur()->kw != KW_AND && cur()->kw != KW_OR &&
                             cur()->kw != KW_NOT && cur()->kw != KW_TRUE &&
                             cur()->kw != KW_FALSE)
                        break;  /* no keyword can live in an initializer, so a
                                 * keyword here means the ';' was forgotten */
                    g_pi++;
                }
            }
            /* prescan itself never reports: if the ';' is there, take it and
             * move on; if it is missing, stop where we are and let
             * parse_program -- which has full recovery -- complain from the
             * real position instead of from end of file. */
            if (is_op(cur(), ";")) g_pi++;
            continue;
        }
        if (is_op(t, "{")) depth++;
        else if (is_op(t, "}")) depth--;
        g_pi++;
    }
}

/* ---------------- constant macros ----------------
 * Constants become #define macros so they can be used in
 * global initialisers too. Values must be constant expressions. */
static void emit_const_macros(void)
{
    int i;
    for (i = 0; i < g_nconsts; i++) {
        if (g_errors) exit(1);
        fprintf(g_out, "#define %s (", g_consts[i].sym->name);
        emit_plain(g_consts[i].init);
        fprintf(g_out, ")\n");
    }
}

/* ---------------- plain (constant-safe) emitter ---------------- */
static void emit_plain_conv(Type dst, Expr *v);
static void emit_plain(Expr *e)
{
    switch (e->k) {
    case E_NUM:
        w("%lldLL", e->iv);
        break;
    case E_REAL: {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%.17g", e->dv);
        if (!strpbrk(tmp, ".eE")) strncat(tmp, ".0", sizeof tmp - strlen(tmp) - 1);
        w("(%s)", tmp);
        break;
    }
    case E_STR:
        emit_cstr(g_out, e->sv, e->svlen);
        break;
    case E_CHR:
        emit_cchr(g_out, (unsigned char)e->iv);
        break;
    case E_TRUE: w("1"); break;
    case E_FALSE: w("0"); break;
    case E_VAR:
        w("%s", e->sym->name);   /* a constant -> expands to its macro */
        break;
    case E_UN:
        if (e->sv[0] == '!') w("(!");
        else if (e->sv[0] == '~') w("(~");
        else w("(%s", e->sv);
        emit_plain(e->a);
        w(")");
        break;
    case E_BIN:
        w("("); emit_plain(e->a); w(" %s ", e->sv); emit_plain(e->b); w(")");
        break;
    case E_LIST: {
        EList *l;
        w("{");
        for (l = e->args; l; l = l->next) {
            if (l != e->args) w(", ");
            if (l->e->k == E_LIST) emit_plain(l->e);
            else emit_plain_conv(e->t, l->e);
        }
        w("}");
        break;
    }
    default:
        w("0");
    }
}
static void emit_plain_conv(Type dst, Expr *v)
{
    if (dst == T_BOOL && v->t != T_BOOL && v->t != T_ERR) {
        w("("); emit_plain(v); w(") != 0");
        return;
    }
    emit_plain(v);
}

/* ---------------- analysis walk ---------------- */
static Type g_cret = T_NUM;
static int g_in_start = 0;

static void check_cond(Expr *c)
{
    Type t = etype(c);
    if (t == T_ERR) return;
    if (t != T_BOOL)
        err(c->line, c->col,
            "条件必须是一个比较（true 或 false），不能是 %s —— 例如写 'n != 0'",
            type_name(t));
}

static void analyze_decl(Stmt *s)
{
    int i;
    for (i = 0; i < s->ndecl; i++) {
        Decl *d = &s->decls[i];
        if (!d->init) continue;
        if (d->is_const) {
            if (d->init && d->init->t == T_UNKNOWN) analyze_const_init(d->sym, d->init);
            continue;
        }
        {
            Type t;
            if (d->ndim > 0) {
                if (d->init->k != E_LIST) {
                    err(d->line, 0, "数组要用 { ... } 来初始化");
                    continue;
                }
                check_list(d->init, s->dtype);
            } else {
                t = etype(d->init);
                if (t == T_ERR) continue;
                if (d->init->k == E_LIST) {
                    err(d->init->line, d->init->col, "'{ ... }' 只能用来初始化数组");
                    continue;
                }
                if (!assignable(s->dtype, t))
                    err(d->init->line, d->init->col, "不能把 %s 放进 %s", type_name(t), type_name(s->dtype));
                else if (s->dtype == T_LETTER && t == T_NUM)
                    check_letter_lit(d->init, "初始化");
            }
            if (d->sym && d->sym->is_global) check_static_init(d->init, "全局初始化");
        }
    }
}

static void analyze_stmt(Stmt *s, Scope *sc)
{
    switch (s->k) {
    case S_DECL:
        analyze_decl(s);
        break;
    case S_IF:
        check_cond(s->cond);
        analyze_slist(s->body, sc);
        if (s->elseb) analyze_slist(s->elseb, sc);
        break;
    case S_WHILE:
        check_cond(s->cond);
        analyze_slist(s->body, sc);
        break;
    case S_UNTIL:
    case S_DO:
        check_cond(s->cond);
        analyze_slist(s->body, sc);
        break;
    case S_FOREACH: {
        Expr *a = s->cond;
        etype(a);
        if (a->k != E_VAR || !a->sym || a->sym->ndim < 1) {
            err(s->line, 0, "'foreach' 后面要跟一个数组，例如  foreach (score in scores) { ... }");
            break;
        }
        if (a->sym->ndim > 1) {
            err(s->line, 0, "'foreach' 只能遍历一维数组");
            break;
        }
        if (s->isyms) s->isyms->type = a->sym->type;    /* the element's type */
        analyze_slist(s->body, sc);
        break;
    }
    case S_SWITCH: {
        Case *c;
        Type tc = etype(s->cond);
        if (tc != T_ERR && !is_intlike(tc))
            err(s->line, 0, "'switch' 要一个 int（string 或 float 请用 if / else if）");
        for (c = s->cases; c; c = c->next) {
            etype(c->val);
            check_static_init(c->val, "case 的值");
            if (c->val->t != T_ERR && !is_intlike(c->val->t))
                err(c->line, 0, "case 的值必须是 int");
            analyze_slist(c->body, sc);
        }
        if (s->defbody) analyze_slist(s->defbody, sc);
        break;
    }
    case S_FOR:
        if (s->initst) analyze_stmt(s->initst, sc);
        if (s->cond) check_cond(s->cond);
        if (s->step) etype(s->step);
        analyze_slist(s->body, sc);
        break;
    case S_RETURN: {
        Type t = s->val ? etype(s->val) : T_VOID;
        if (s->val) {
            if (g_in_start) {
                if (t != T_ERR && !is_intlike(t))
                    err(s->line, 0, "'start' 只能 'return' 一个 int（程序的退出码）");
            } else if (g_cret == T_VOID) {
                err(s->line, 0, "这个函数的类型是 none，不能返回值");
            } else if (!assignable(g_cret, t)) {
                err(s->line, 0, "这个函数必须 return %s，但值是 %s", type_name(g_cret), type_name(t));
            }
        } else {
            if (!g_in_start && g_cret != T_VOID)
                err(s->line, 0, "这个函数必须 'return' 一个 %s 值", type_name(g_cret));
        }
        break;
    }
    case S_BREAK:
    case S_CONTINUE:
    case S_EMPTY:
        break;
    case S_SAY: {
        EList *l;
        for (l = s->args; l; l = l->next) {
            Type t = etype(l->e);
            if (t != T_ERR && t != T_TEXT && !is_scalar(t))
                err(l->e->line, l->e->col, "say/show 可以打印 int、float、string、char 或 bool");
        }
        break;
    }
    case S_INPUT: {
        Type t = etype(s->val);
        if (t == T_ERR) break;
        if (!lvalue_ok(s->val)) { err(s->line, 0, "input 要一个变量，例如  input name;"); break; }
        if (s->val->k == E_VAR && s->val->sym->kind == SK_CONST) { err(s->line, 0, "input 改不了常量"); break; }
        if (t != T_TEXT && !is_scalar(t)) { err(s->line, 0, "input 只能给 int、float、string、char 或 bool 读入值"); break; }
        break;
    }
    case S_BLOCK:
        analyze_slist(s->body, s->scope);
        break;
    case S_EXPR:
        etype(s->val);
        break;
    case S_FUNC:
        g_ascope = s->scope;
        g_cret = s->fn->ret;
        g_in_start = 0;
        analyze_slist(s->body, s->scope);
        break;
    case S_START:
        g_ascope = s->scope;
        g_cret = T_NUM;
        g_in_start = 1;
        analyze_slist(s->body, s->scope);
        break;
    }
}

static void analyze_slist(SList *l, Scope *sc)
{
    Scope *save = g_ascope;
    g_ascope = sc;
    for (; l; l = l->next) analyze_stmt(l->s, sc);
    g_ascope = save;
}

/* ---------------- code emission walk ---------------- */
static int g_after_else = 0;

static void emit_decl(Stmt *s)
{
    int i;
    for (i = 0; i < s->ndecl; i++) {
        Decl *d = &s->decls[i];
        int isglob = d->sym && d->sym->is_global;
        if (d->is_const) {
            eline(d->line);
            fprintf(g_out, "#define %s (", d->name);
            emit_plain(d->init);
            fprintf(g_out, ")\n");
            g_lastline = -1;
            continue;
        }
        eline(d->line);
        wind();
        w("%s ", ctype_name(s->dtype));
        w("%s", d->name);
        {
            int k;
            for (k = 0; k < d->ndim; k++) w("[%d]", d->dims[k]);
        }
        if (d->init) {
            if (isglob) {
                if (d->ndim > 0) { w(" = "); emit_plain(d->init); }
                else { w(" = ("); emit_plain_conv(s->dtype, d->init); w(")"); }
            } else {
                w(" = ");
                eexpr(d->init);
            }
        } else if (d->ndim > 0) {
            w(" = {0}");
        } else if (s->dtype == T_TEXT) {
            w(" = NULL");
        } else {
            w(" = 0");
        }
        w(";\n");
    }
}

static void emit_tail_return(Type ret)
{
    wind();
    switch (ret) {
    case T_NUM:   w("return 0;\n"); break;
    case T_BOOL:  w("return 0;\n"); break;
    case T_REAL:  w("return 0.0;\n"); break;
    case T_TEXT:  w("return NULL;\n"); break;
    case T_LETTER:w("return '\\0';\n"); break;
    default: break;   /* none / void: nothing to return */
    }
}

static void emit_stmt(Stmt *s)
{
    switch (s->k) {
    case S_DECL:
        emit_decl(s);
        break;
    case S_IF: {
        int ae = g_after_else;
        g_after_else = 0;
        if (!ae) { eline(s->line); wind(); }
        w("if (");
        eexpr(s->cond);
        w(") {\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        if (s->elseb && s->elseb->next == NULL && s->elseb->s->k == S_IF) {
            w("} else ");
            g_after_else = 1;
            emit_stmt(s->elseb->s);
        } else if (s->elseb) {
            w("} else {\n");
            g_ind++;
            emit_slist(s->elseb);
            g_ind--;
            wind();
            w("}\n");
        } else {
            if (ae) w("}\n");
            else { wind(); w("}\n"); }
        }
        break;
    }
    case S_WHILE:
        eline(s->line);
        wind();
        w("while (");
        eexpr(s->cond);
        w(") {\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        wind();
        w("}\n");
        break;
    case S_UNTIL:                       /* repeat while it is NOT true yet */
        eline(s->line);
        wind();
        w("while (!(");
        eexpr(s->cond);
        w(")) {\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        wind();
        w("}\n");
        break;
    case S_DO:                          /* runs at least once */
        eline(s->line);
        wind();
        w("do {\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        wind();
        w("} while (");
        eexpr(s->cond);
        w(");\n");
        break;
    case S_FOREACH: {
        static int fw_uid = 0;          /* unique names for nested loops */
        int id = fw_uid++;
        eline(s->line);
        wind();
        w("for (long long _fw%d = 0, _fw%dn = (long long)(sizeof (", id, id);
        eexpr(s->cond);
        w(") / sizeof (");
        eexpr(s->cond);
        w("[0])); _fw%d < _fw%dn; _fw%d++) {\n", id, id, id);
        g_ind++;
        wind();
        w("%s %s = ", ctype_name(s->isyms ? s->isyms->type : T_NUM), s->iname);
        eexpr(s->cond);
        w("[_fw%d];\n", id);
        emit_slist(s->body);
        g_ind--;
        wind();
        w("}\n");
        break;
    }
    case S_SWITCH: {
        Case *c;
        eline(s->line);
        wind();
        w("switch (");
        eexpr(s->cond);
        w(") {\n");
        for (c = s->cases; c; c = c->next) {
            g_ind++;
            wind();
            w("case ");
            eexpr(c->val);
            w(": {\n");
            g_ind++;
            emit_slist(c->body);
            g_ind--;
            wind();
            w("}\n");
            g_ind--;
        }
        if (s->defbody) {
            g_ind++;
            wind();
            w("default: {\n");
            g_ind++;
            emit_slist(s->defbody);
            g_ind--;
            wind();
            w("}\n");
            g_ind--;
        }
        wind();
        w("}\n");
        break;
    }
    case S_FOR: {
        eline(s->line);
        wind();
        w("for (");
        if (s->initst) {
            if (s->initst->k == S_DECL) {
                Stmt *is = s->initst;
                int i;
                for (i = 0; i < is->ndecl; i++) {
                    Decl *d = &is->decls[i];
                    if (i) w(", ");
                    else w("%s ", ctype_name(is->dtype));   /* type only once:  int i = 0, j = 4 */
                    w("%s", d->name);
                    if (d->init) { w(" = "); eexpr(d->init); }
                    else if (is->dtype == T_TEXT) w(" = NULL");
                    else w(" = 0");
                }
            } else {
                eexpr(s->initst->val);
            }
        }
        w("; ");
        if (s->cond) eexpr(s->cond);
        w("; ");
        if (s->step) eexpr(s->step);
        w(") {\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        wind();
        w("}\n");
        break;
    }
    case S_RETURN:
        eline(s->line);
        wind();
        if (!s->val) {
            w(g_in_start ? "return 0;\n" : "return;\n");
        } else {
            w("return ");
            if (g_in_start) eexpr(s->val);
            else emit_converted(g_cret, s->val);
            w(";\n");
        }
        break;
    case S_BREAK:
        eline(s->line); wind(); w("break;\n");
        break;
    case S_CONTINUE:
        eline(s->line); wind(); w("continue;\n");
        break;
    case S_SAY: {
        EList *l;
        for (l = s->args; l; l = l->next) {
            const char *pn;
            eline(s->line);
            wind();
            switch (l->e->t) {
            case T_NUM: pn = "cask_p_num"; break;
            case T_REAL: pn = "cask_p_real"; break;
            case T_TEXT: pn = "cask_p_str"; break;
            case T_LETTER: pn = "cask_p_char"; break;
            case T_BOOL: pn = "cask_p_bool"; break;
            default: pn = "cask_p_str"; break;
            }
            w("%s(", pn);
            eexpr(l->e);
            w(");\n");
        }
        if (s->nl) {
            eline(s->line);
            wind();
            w("cask_nl();\n");
        }
        break;
    }
    case S_INPUT: {
        const char *fn;
        eline(s->line);
        wind();
        switch (s->val->t) {
        case T_NUM: fn = "cask_in_num"; break;
        case T_REAL: fn = "cask_in_real"; break;
        case T_TEXT: fn = "cask_in_str"; break;
        case T_LETTER: fn = "cask_in_char"; break;
        default: fn = "cask_in_bool"; break;
        }
        w("%s(&(", fn);
        eexpr(s->val);
        w("));\n");
        break;
    }
    case S_BLOCK:
        eline(s->line);
        wind();
        w("{\n");
        g_ind++;
        emit_slist(s->body);
        g_ind--;
        wind();
        w("}\n");
        break;
    case S_EXPR:
        eline(s->line);
        wind();
        eexpr(s->val);
        w(";\n");
        break;
    case S_EMPTY:
        break;
    case S_FUNC: {
        Func *f = s->fn;
        int i;
        eline(f->line);
        w("%s %s(", ctype_name(f->ret), f->name);
        for (i = 0; i < f->nparam; i++) {
            if (i) w(", ");
            w("%s ", ctype_name(f->pt[i]));
            w("%s", f->pn[i]);
        }
        w(")\n{\n");
        if (strcmp(f->name, "main") == 0 && f->nparam == 0) {
            w("\tcask_init(%d);\n", g_is_utf8 ? 65001 : 0);
#ifdef _WIN32
            if (g_native) w("\t__cask_main();\n");
#endif
        }
        g_ind++;
        g_cret = f->ret;
        g_in_start = 0;
        emit_slist(s->body);
        if (f->ret != T_VOID) emit_tail_return(f->ret);
        g_ind--;
        w("}\n");
        break;
    }
    case S_START:
        eline(s->line);
        w("static int __cask_start(void)\n{\n");
        g_ind++;
        g_cret = T_NUM;
        g_in_start = 1;
        emit_slist(s->body);
        emit_tail_return(T_NUM);
        g_ind--;
        w("}\n");
        w("int main(void)\n{\n\tcask_init(%d);\n\treturn __cask_start();\n}\n",
          g_is_utf8 ? 65001 : 0);
        break;
    }
}

static void emit_slist(SList *l)
{
    for (; l; l = l->next) emit_stmt(l->s);
}

/* ============================================================
 * PART7: driver -- read file, lex, prescan, emit C, invoke gcc
 * ============================================================ */
static char g_rtdir[1200] = "";
static char g_exedir[1024] = "";

#ifdef _WIN32
#include <windows.h>
static void exe_dir(char *out, size_t n)
{
    char tmp[1024];
    DWORD r = GetModuleFileNameA(NULL, tmp, (DWORD)sizeof tmp);
    char *bs;
    if (!r) { out[0] = 0; return; }
    bs = strrchr(tmp, '\\');
    if (!bs) bs = strrchr(tmp, '/');
    if (bs) *bs = 0;
    snprintf(out, n, "%s", tmp);
}
#else
#include <unistd.h>
static void exe_dir(char *out, size_t n)
{
    char tmp[1024];
    ssize_t r = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
    char *sl;
    if (r <= 0) { out[0] = 0; return; }
    tmp[r] = 0;
    sl = strrchr(tmp, '/');
    if (sl) *sl = 0;
    snprintf(out, n, "%s", tmp);
}
#endif

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* 64-bit FNV-1a over a file: cheap enough to run on every build, and it
 * lets caskc skip gcc when the generated C has not changed at all. */
static unsigned long long file_hash(const char *p, char *hex, size_t cap)
{
    FILE *f = fopen(p, "rb");
    unsigned long long h = 1469598103934665603ULL;
    unsigned char buf[65536];
    size_t n;
    if (!f) { hex[0] = 0; return 0; }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    snprintf(hex, cap, "%016llx", h);
    return h;
}

static int find_rt(void)
{
    char p[1200];
    const char *env = getenv("CASK_RT_DIR");
    if (env && env[0]) {
        snprintf(p, sizeof p, "%s\\cask_rt.h", env);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s", env); return 1; }
        snprintf(p, sizeof p, "%s/cask_rt.h", env);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s", env); return 1; }
    }
    if (g_exedir[0]) {
        snprintf(p, sizeof p, "%s\\cask_rt.h", g_exedir);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s", g_exedir); return 1; }
        snprintf(p, sizeof p, "%s/cask_rt.h", g_exedir);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s", g_exedir); return 1; }
        snprintf(p, sizeof p, "%s\\..\\compiler\\cask_rt.h", g_exedir);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s\\..\\compiler", g_exedir); return 1; }
        snprintf(p, sizeof p, "%s/../compiler/cask_rt.h", g_exedir);
        if (file_exists(p)) { snprintf(g_rtdir, sizeof g_rtdir, "%s/../compiler", g_exedir); return 1; }
    }
    if (file_exists("cask_rt.h")) { snprintf(g_rtdir, sizeof g_rtdir, "."); return 1; }
    if (file_exists("compiler\\cask_rt.h")) { snprintf(g_rtdir, sizeof g_rtdir, "compiler"); return 1; }
    if (file_exists("compiler/cask_rt.h")) { snprintf(g_rtdir, sizeof g_rtdir, "compiler"); return 1; }
    return 0;
}

#ifndef _WIN32
static int have_prog(const char *name)
{
    char cmd[320];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}
static void pause_if_console(void) { /* interactive pause is Windows-only */ }
#else
/* Run a program directly (no cmd.exe shell: saves ~40ms per build and
 * avoids quote mangling), wait for it, return its exit code.
 * Returns -1 when the program could not even be started. */
static int run_child(char *cmdline)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* When the exe is double-clicked in Explorer there is no way back to
 * show the message, so hold the window if we are on a real console. */
static void pause_if_console(void)
{
    DWORD mode;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);

    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        fprintf(stderr, "按回车键关闭…\n");
        (void)getchar();
    }
}
#endif

/* Full x86-64 backend: C* -> asm -> as -> ld (see asm_backend.inc). */
#include "asm_backend.inc"

static void usage(void)
{
    printf("caskc —— C*（C Asterisk）编译器，%s\n", CASK_VERSION);
    puts("");
    puts("用法: caskc <file.cask> [选项]");
    puts("  -o FILE       生成程序的名字（默认: file.exe）");
    puts("  --emit-c FILE 只写出生成的 C 代码（调试用，不编译）");
    puts("  --keep        保留生成的 .s 汇编文件");
    puts("  --fast        关掉溢出 / 下标检查（能快一点）");
    puts("  --check       只检查程序，不生成也不编译");
    puts("  --diag        把问题按  K|line|col|message  的格式列到屏幕上");
    puts("                （K: P 拼写, S 语法, E 严重, H 提示）");
    puts("  -O0 -O1 -O2 -O3   保留兼容（优化已内置在汇编后端里）");
    puts("  -g            保留生成的 .s 汇编文件（同 --keep）");
    puts("  --run         编译成功后运行程序");
    puts("  --console     窗口程序（C_OPED.cash）里保留控制台");
    puts("  -v            显示 as / ld 的命令行");
    puts("  --version     显示版本号并退出");
    puts("  -h            显示这份帮助");
    exit(0);
}

int main(int argc, char **argv)
{
    const char *src = NULL, *out = NULL, *emitc = NULL;
    int keep = 0, run = 0, verbose = 0;
    int i;
    char gen[1200], cmd[4096];
    static char outbuf[1200];
    SList *top;
    Func *f;

    exe_dir(g_exedir, sizeof g_exedir);
    gen[0] = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("caskc %s (C Asterisk)\n", CASK_VERSION);
            return 0;
        }
        else if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--emit-c") && i + 1 < argc) emitc = argv[++i];
        else if (!strcmp(a, "--keep")) keep = 1;
        else if (!strcmp(a, "--fast")) g_fast = 1;
        else if (!strcmp(a, "--asm")) { /* 后端现在始终是汇编，保留选项只为兼容 */ }
        else if (!strcmp(a, "--check")) g_check = 1;
        else if (!strcmp(a, "--diag")) g_diag = 1;
        else if (!strcmp(a, "--run")) run = 1;
        else if (!strcmp(a, "--console")) g_console = 1;
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-g")) keep = 1;          /* -g: 保留生成的 .s 供检查 */
        else if (!strcmp(a, "-O0") || !strcmp(a, "-O1") ||
                 !strcmp(a, "-O2") || !strcmp(a, "-O3"))
            { /* 优化已内置在汇编后端里，选项保留兼容 */ }
        else if (a[0] == '-' && a[1]) fatal("不认识的选项 '%s'（可以试试 -h）", a);
        else if (!src) src = a;
        else fatal("一次只能编译一个源文件");
    }
    g_verbose = verbose;
    if (!src) {
#ifdef _WIN32
        fprintf(stderr,
            "caskc: 没有输入文件（可以试试 -h 看帮助）\n"
            "  用法: caskc file.cask [-o out.exe] [--run]\n"
            "  注意: caskc.exe 是命令行编译器 —— 双击它\n"
            "        没有用；要用图形编辑器请运行\n"
            "        caskide.exe，或者这样编译一个文件:\n"
            "          caskc myfile.cask --run\n");
#else
        fprintf(stderr,
            "caskc: 没有输入文件（可以试试 -h 看帮助）\n"
            "  用法: caskc file.cask [-o out] [--run]\n"
            "  运行:  ./caskc myfile.cask --run\n"
            "  编辑:  ./bin/caskide.sh\n");
#endif
        pause_if_console();
        return 1;
    }

    snprintf(outbuf, sizeof outbuf, "%s", src);
    {
        char *dot = strrchr(outbuf, '.');
        char *slash = strrchr(outbuf, '\\');
        char *slash2 = strrchr(outbuf, '/');
        if (dot && (!slash || dot > slash) && (!slash2 || dot > slash2)) *dot = 0;
    }
#ifdef _WIN32
    snprintf(outbuf + strlen(outbuf), sizeof outbuf - strlen(outbuf), ".exe");
#endif
    if (!out) out = outbuf;
    snprintf(gen, sizeof gen, "%s.gen.c", out);

    if (!find_rt() && emitc) {
        fprintf(stderr,
            "caskc: 警告: 找不到 cask_rt.h（C* 的运行时头文件）。\n"
            "  请把 cask_rt.h 放在 caskc.exe 同一个文件夹里，\n"
            "  或者把环境变量 CASK_RT_DIR 设成放着它的文件夹。\n");
    }

    g_file = src;
    read_source(src);
    g_lfilepath = path_escape(src);
    lex();
    if (g_errors) return 1;

    /* --check and the normal asm path write to the null device; only
     * --emit-c actually creates the .gen.c file.                       */
    g_out = fopen((g_check || !emitc) ?
#ifdef _WIN32
        "NUL"
#else
        "/dev/null"
#endif
        : gen, "wb");
    if (!g_out) fatal("建不了 '%s'", gen);

    reg_builtins();
    g_gscope = scope_push(NULL);
    prescan();
    if (g_errors) { fclose(g_out); remove(gen); return 1; }

    if (emitc && g_native) {
        /* --emit-c needs C_OPED.cash next to the runtime header (the asm
         * path links the prebuilt libcastrt.a instead, which already
         * contains the toolkit) */
        char hp[1400];
        int ok = 0;
        if (g_rtdir[0]) {
            snprintf(hp, sizeof hp, "%s\\C_OPED.cash", g_rtdir);
            if (file_exists(hp)) ok = 1;
            if (!ok) {
                snprintf(hp, sizeof hp, "%s/C_OPED.cash", g_rtdir);
                if (file_exists(hp)) ok = 1;
            }
        }
        if (!ok)
            err(g_native_line ? g_native_line : 1, 0,
                "找不到 'C_OPED.cash' —— 请把它放在 caskc.exe 旁边，"
                "或者把 CASK_RT_DIR 设成放着它的文件夹");
    }
    if (g_use_css && !g_native)
        err(g_css_line ? g_css_line : 1, 0,
            "'C_CSS.cash' 要配 'C_OPED.cash' 用 —— 请先写 use \"C_OPED.cash\";");
    if (emitc && g_use_css) {
        char hp[1400];
        int ok = 0;
        if (g_rtdir[0]) {
            snprintf(hp, sizeof hp, "%s\\C_CSS.cash", g_rtdir);
            if (file_exists(hp)) ok = 1;
            if (!ok) {
                snprintf(hp, sizeof hp, "%s/C_CSS.cash", g_rtdir);
                if (file_exists(hp)) ok = 1;
            }
        }
        if (!ok)
            err(g_css_line ? g_css_line : 1, 0,
                "找不到 'C_CSS.cash' —— 请把它放在 caskc.exe 旁边，"
                "或者把 CASK_RT_DIR 设成放着它的文件夹");
    }
    if (g_errors) { fclose(g_out); remove(gen); return 1; }

    /* ---- generated header ---- */
    fprintf(g_out, "/* generated by caskc (C* compiler) from %s -- do not edit */\n", src);
    fprintf(g_out, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
    fprintf(g_out, "#include <math.h>\n#include <limits.h>\n#include <time.h>\n#include <ctype.h>\n");
    for (i = 0; i < g_nuses; i++) {
        const char *u = g_uses[i];
        fprintf(g_out, "#include %s\n", u);
    }
    fprintf(g_out, "#include \"cask_rt.h\"\n");

    emit_const_macros();

    /* ---- prototypes (so calls can come before definitions) ---- */
    for (f = g_funcs; f; f = f->next) {
        int k;
        if (f->is_builtin) continue;
        fprintf(g_out, "%s %s(", ctype_name(f->ret), f->name);
        if (f->nparam == 0) fprintf(g_out, "void");
        for (k = 0; k < f->nparam; k++) {
            if (k) fprintf(g_out, ", ");
            fprintf(g_out, "%s ", ctype_name(f->pt[k]));
            fprintf(g_out, "%s", f->pn[k]);
        }
        fprintf(g_out, ");\n");
    }
    if (g_errors) { fclose(g_out); g_out = NULL; remove(gen); return 1; }

    /* ---- full parse ---- */
    top = parse_program();
    if (g_errors) { fclose(g_out); g_out = NULL; remove(gen); return 1; }

    for (f = g_funcs; f; f = f->next)
        if (!f->is_builtin && !f->defined && !f->is_proto)
            err(f->line, 0, "函数 '%s' 只声明了，从来没有定义", f->name);
    if (!g_has_start && !g_has_main)
        err(1, 0, "程序没有入口 —— 请加一个 'function main() { ... }' 块（或者 'start { ... }'）");
    if (g_errors) { fclose(g_out); g_out = NULL; remove(gen); return 1; }

    /* ---- analysis ---- */
    analyze_slist(top, g_gscope);
    if (g_errors) { fclose(g_out); g_out = NULL; remove(gen); return 1; }

    /* --check: the caller only wanted the list of problems */
    if (g_check) { fclose(g_out); g_out = NULL; return 0; }

    /* ---- --emit-c: keep the old C-emitting path for debugging (the C is
     *      written out but never compiled by caskc itself). ---- */
    if (emitc) {
        emit_slist(top);
        if (g_native) {
#ifdef _WIN32
            fprintf(g_out,
                "\nint WINAPI WinMain(HINSTANCE hI, HINSTANCE hP, LPSTR lC, int nS)\n"
                "{\n\t(void)hI; (void)hP; (void)lC; (void)nS;\n"
                "\tmain();\n\treturn 0;\n}\n");
#endif
        }
        fclose(g_out);
        g_out = NULL;
        remove(emitc);
        if (rename(gen, emitc) != 0) fatal("写不了 '%s'", emitc);
        printf("已写出 %s\n", emitc);
        return 0;
    }

    /* ---- default: full x86-64 backend (C* -> .s -> as -> ld, no C compiler) ---- */
    fclose(g_out);
    g_out = NULL;                        /* only the null device was open */
    if (!asm_backend_build(top, out, keep)) return 1;
    if (run) {
        int rrc;
#ifdef _WIN32
        snprintf(cmd, sizeof cmd, "\"%s\"", out);
        rrc = run_child(cmd);
        if (rrc < 0) rrc = 1;
#else
        snprintf(cmd, sizeof cmd, "./%s", out);
        rrc = system(cmd);
#endif
        return rrc < 0 ? 1 : rrc;   /* keep the program's own exit code */
    }
    return 0;
}
