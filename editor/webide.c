/* webide.c -- C* Studio 0.3: the web-style editor host.
 *
 * The window you see is a normal Win32 window; everything inside it is
 * HTML/CSS/JS.  This file:
 *   - serves the page from the web\ folder next to the exe,
 *   - runs a tiny loopback HTTP server (127.0.0.1 only) that the page
 *     calls for files, compiling and running,
 *   - holds the WebView2 browser (the C++ glue is editor\wv2.cpp).
 *
 * API used by web\app.js:
 *   GET  /                     the page and its files
 *   POST /api/info             {app,version,open}
 *   POST /api/dialog/open      native open dialog  -> {path}|{cancelled}
 *   POST /api/dialog/saveas    native save dialog  -> {path}|{cancelled}
 *   POST /api/load      {path}                      -> {path,text,encoding}
 *   POST /api/save      {path,text,encoding}        -> {ok}
 *   POST /api/check     {text,path}                 -> {errs,rc}
 *   POST /api/build     {text,path,fast}            -> {ok,errs,log,exe}
 *   POST /api/run       {text,path,fast}            -> {ok,id,errs,log}
 *   GET  /api/output    ?id=N                       -> {out,done,code}
 *   POST /api/stop      {id}                        -> {ok}
 *   POST /api/enc       {path,to}                   -> {text,encoding}
 *   POST /api/title     {title}                     -> {ok}
 *   POST /api/reveal    {path}                      -> {ok}
 * An error line from `caskc --diag` is  K|line|col|message  with
 * K = P spelling, S syntax, E severe, H hint.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* --- the C++ part (editor\wv2.cpp) --- */
int  wv2_start(HWND hwnd, const wchar_t *url, const wchar_t *userdata);
void wv2_resize(void);
void wv2_close(void);
const char *wv2_error(void);

#define APPNAME_A "C* Studio"
#define APPVER_A  "0.4.2"
#define LAPPNAMEW L"C* Studio"
#define WM_DLGREQ (WM_APP + 1)

/* ==================================================================== */
/* small helpers                                                         */
/* ==================================================================== */

typedef struct { char *p; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->p = NULL; b->len = b->cap = 0; }
static void buf_free(Buf *b) { free(b->p); buf_init(b); }
static void buf_add(Buf *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = (char *)realloc(b->p, nc);
        if (!b->p) exit(3);
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[8192];
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

/* a roomy scratch buffer for one request's text (never on the stack) */
#define BIG (8 << 20)
static char *big(void)
{
    char *p = (char *)malloc(BIG);
    if (!p) exit(4);
    p[0] = 0;
    return p;
}

static wchar_t *u8to16(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) s = "";
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) { n = 1; w = (wchar_t *)malloc(2); w[0] = 0; return w; }
    w = (wchar_t *)malloc((size_t)n * 2);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *u16to8(const wchar_t *w)
{
    int n;
    char *s;
    if (!w) w = L"";
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) { s = (char *)malloc(1); s[0] = 0; return s; }
    s = (char *)malloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static char g_exedir[1024];        /* UTF-8, no trailing backslash */
static char g_openfile[1024];      /* file passed on the command line */

static void find_exedir(void)
{
    wchar_t w[1000];
    char *s;
    wchar_t *sl;
    GetModuleFileNameW(NULL, w, 999);
    sl = wcsrchr(w, L'\\');
    if (sl) *sl = 0;
    s = u16to8(w);
    snprintf(g_exedir, sizeof g_exedir, "%s", s);
    free(s);
}

/* ---------------- JSON ---------------- */

static void json_escape(Buf *out, const char *s)
{
    const unsigned char *p;
    if (!s) return;
    for (p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        char tmp[8];
        if (c == '"') buf_puts(out, "\\\"");
        else if (c == '\\') buf_puts(out, "\\\\");
        else if (c == '\n') buf_puts(out, "\\n");
        else if (c == '\r') buf_puts(out, "\\r");
        else if (c == '\t') buf_puts(out, "\\t");
        else if (c == '\b') buf_puts(out, "\\b");
        else if (c == '\f') buf_puts(out, "\\f");
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", c); buf_puts(out, tmp); }
        else buf_add(out, &c, 1);
    }
}

static const char *jws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
/* skip one JSON value; returns the position after it */
static const char *jskip(const char *p)
{
    p = jws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p) p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (*p == '"') { p = jskip(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') depth--;
            if (*p) p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p;
}
/* decode a JSON string starting at the opening quote */
static int jdec_str(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') {
        unsigned int cp = 0;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': if (o + 1 < cap) out[o++] = '\n'; p++; break;
            case 'r': if (o + 1 < cap) out[o++] = '\r'; p++; break;
            case 't': if (o + 1 < cap) out[o++] = '\t'; p++; break;
            case 'b': if (o + 1 < cap) out[o++] = '\b'; p++; break;
            case 'f': if (o + 1 < cap) out[o++] = '\f'; p++; break;
            case 'u': {
                int i;
                char h[5];
                p++;
                for (i = 0; i < 4 && p[i]; i++) h[i] = p[i];
                h[i] = 0;
                cp = (unsigned int)strtoul(h, NULL, 16);
                p += i;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned int lo = 0;
                    char h2[5];
                    int j;
                    p += 2;
                    for (j = 0; j < 4 && p[j]; j++) h2[j] = p[j];
                    h2[j] = 0;
                    lo = (unsigned int)strtoul(h2, NULL, 16);
                    p += j;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp < 0x80) { if (o + 1 < cap) out[o++] = (char)cp; }
                else if (cp < 0x800) {
                    if (o + 2 < cap) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 63)); }
                } else if (cp < 0x10000) {
                    if (o + 3 < cap) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                } else {
                    if (o + 4 < cap) { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 63)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                }
                break;
            }
            default: if (o + 1 < cap) out[o++] = *p; p++; break;
            }
        } else {
            if (o + 1 < cap) out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    return 1;
}
/* look up a top-level key of a JSON object; returns 1 if found */
static int jget_str(const char *body, const char *key, char *out, size_t cap)
{
    const char *p = jws(body);
    size_t klen = strlen(key);
    out[0] = 0;
    if (*p != '{') return 0;
    p++;
    for (;;) {
        p = jws(p);
        if (*p == '}' || !*p) return 0;
        if (*p == '"') {
            const char *ks = ++p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
            if ((size_t)(p - ks - 1) == klen && strncmp(ks, key, klen) == 0) {
                /* key found (no escapes in our keys) */
                p = jws(p);
                if (*p == ':') {
                    p = jws(p + 1);
                    if (*p == '"') return jdec_str(p, out, cap);
                    return 0;
                }
            }
        }
        p = jskip(p);
        p = jws(p);
        if (*p == ',') p++;
        else if (*p == '}') return 0;
    }
}
static int jget_bool(const char *body, const char *key, int *out)
{
    const char *p = jws(body);
    size_t klen = strlen(key);
    *out = 0;
    if (*p != '{') return 0;
    p++;
    for (;;) {
        p = jws(p);
        if (*p == '}' || !*p) return 0;
        if (*p == '"') {
            const char *ks = ++p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
            if ((size_t)(p - ks - 1) == klen && strncmp(ks, key, klen) == 0) {
                p = jws(p);
                if (*p == ':') {
                    p = jws(p + 1);
                    if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
                    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
                    return 0;
                }
            }
        }
        p = jskip(p);
        p = jws(p);
        if (*p == ',') p++;
        else if (*p == '}') return 0;
    }
}

/* ---------------- files, encodings ---------------- */

static int read_file(const char *path, Buf *out)     /* UTF-8 bytes as they are */
{
    FILE *f = fopen(path, "rb");
    char tmp[65536];
    size_t n;
    if (!f) return 0;
    buf_init(out);
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(out, tmp, n);
    fclose(f);
    return 1;
}
static int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n) fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}
static int is_utf8(const char *s, size_t n)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, NULL, 0) > 0;
}
static void gbk_to_u8(const char *in, size_t n, Buf *out)
{
    int wn = MultiByteToWideChar(936, 0, in, (int)n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)wn * 2 + 2);
    int un;
    buf_init(out);
    MultiByteToWideChar(936, 0, in, (int)n, w, wn);
    w[wn] = 0;
    un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    {
        char *u = (char *)malloc((size_t)un + 1);
        WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL);
        u[un] = 0;
        buf_add(out, u, (size_t)un);
        free(u);
    }
    free(w);
}
static void u8_to_gbk(const char *in, size_t n, Buf *out)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, in, (int)n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)wn * 2 + 2);
    int bn;
    buf_init(out);
    MultiByteToWideChar(CP_UTF8, 0, in, (int)n, w, wn);
    w[wn] = 0;
    bn = WideCharToMultiByte(936, 0, w, wn, NULL, 0, NULL, NULL);
    {
        char *b = (char *)malloc((size_t)bn + 1);
        WideCharToMultiByte(936, 0, w, wn, b, bn, NULL, NULL);
        b[bn] = 0;
        buf_add(out, b, (size_t)bn);
        free(b);
    }
    free(w);
}
/* read a source file and hand it over as UTF-8 text */
static int load_source(const char *path, Buf *text, char *enc, size_t enccap)
{
    Buf raw;
    if (!read_file(path, &raw)) return 0;
    if (is_utf8(raw.p ? raw.p : "", raw.len)) {
        buf_init(text);
        buf_add(text, raw.p ? raw.p : "", raw.len);
        snprintf(enc, enccap, "utf-8");
    } else {
        gbk_to_u8(raw.p ? raw.p : "", raw.len, text);
        snprintf(enc, enccap, "gbk");
    }
    buf_free(&raw);
    return 1;
}

static void dir_of(const char *path, char *out, size_t cap)
{
    const char *sl = strrchr(path, '\\');
    const char *sl2 = strrchr(path, '/');
    if (sl2 && (!sl || sl2 > sl)) sl = sl2;
    if (sl) { size_t n = (size_t)(sl - path); if (n >= cap) n = cap - 1; memcpy(out, path, n); out[n] = 0; }
    else snprintf(out, cap, "%s", path);
}

/* ==================================================================== */
/* running caskc / the program                                           */
/* ==================================================================== */

static CRITICAL_SECTION g_runcs;
static HANDLE g_proc = NULL, g_rd = NULL;
static Buf g_runout;
static int g_rundone = 1, g_runcode = 0, g_runid = 0;

static unsigned __stdcall out_reader(void *arg)
{
    char tmp[4096];
    DWORD n;
    (void)arg;
    while (g_rd && ReadFile(g_rd, tmp, sizeof tmp, &n, NULL) && n) {
        EnterCriticalSection(&g_runcs);
        buf_add(&g_runout, tmp, n);
        LeaveCriticalSection(&g_runcs);
    }
    EnterCriticalSection(&g_runcs);
    if (g_proc) {
        WaitForSingleObject(g_proc, 10000);
        { DWORD code = 0; GetExitCodeProcess(g_proc, &code); g_runcode = (int)code; }
    }
    g_rundone = 1;
    LeaveCriticalSection(&g_runcs);
    return 0;
}

/* run an exe, capture everything it prints; waits until it ends */
static int run_capture(const char *exe, const char *args, Buf *out, int *rc, int maxms)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE wr = NULL, rd = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[4096];
    char tmp[8192];
    DWORD n;

    buf_init(out);
    *rc = -1;
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args ? args : "");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(wr); CloseHandle(rd);
        return 0;
    }
    CloseHandle(wr);
    /* read until the pipe breaks (the child and everything it started) */
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                while (ReadFile(rd, tmp, sizeof tmp, &n, NULL) && n) buf_add(out, tmp, n);
                break;
            }
            continue;
        }
        if (ReadFile(rd, tmp, sizeof tmp, &n, NULL) && n) buf_add(out, tmp, n);
    }
    if (WaitForSingleObject(pi.hProcess, (DWORD)maxms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    { DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); *rc = (int)code; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return 1;
}

/* ---------------- temp .cask + caskc ---------------- */

static volatile long g_tmpn = 0;

/* where a scratch copy of the buffer goes: next to the real file so
 * that `use "x.h"` still finds its headers, or in %TEMP% if unsaved */
static void make_temp(const char *text, size_t len, const char *nearp,
                      char *outpath, size_t cap)
{
    int n = (int)InterlockedIncrement(&g_tmpn);
    char dir[1024];
    if (nearp && *nearp) dir_of(nearp, dir, sizeof dir);
    else {
        char t[900];
        GetTempPathA(900, t);
        snprintf(dir, sizeof dir, "%s", t);
        { size_t l = strlen(dir); if (l && (dir[l-1] == '\\' || dir[l-1] == '/')) dir[l-1] = 0; }
    }
    snprintf(outpath, cap, "%s\\_caskstudio_%d_%d.cask", dir, (int)GetCurrentProcessId(), n);
    write_file(outpath, text, len);
}

/* the list of problems from `caskc --check --diag` as a JSON array */
static void diag_to_json(Buf *out, const char *text)
{
    const char *p = text;
    int first = 1;
    buf_puts(out, "[");
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > 3 && strchr("PSEH", p[0]) && p[1] == '|') {
            const char *c1 = strchr(p + 2, '|');
            if (c1 && c1 < p + n) {
                const char *c2 = strchr(c1 + 1, '|');
                if (c2 && c2 < p + n) {
                    int line = atoi(p + 2), col = atoi(c1 + 1);
                    char msg[700];
                    size_t mn = (size_t)(p + n - (c2 + 1));
                    if (mn >= sizeof msg) mn = sizeof msg - 1;
                    memcpy(msg, c2 + 1, mn);
                    msg[mn] = 0;
                    while (mn && (msg[mn - 1] == '\r' || msg[mn - 1] == '\n' || msg[mn - 1] == ' '))
                        msg[--mn] = 0;   /* the pipe turns \n into \r\n */
                    if (!first) buf_puts(out, ",");
                    first = 0;
                    buf_puts(out, "{\"k\":\"");
                    buf_add(out, &p[0], 1);
                    buf_puts(out, "\",\"line\":");
                    buf_printf(out, "%d", line);
                    buf_puts(out, ",\"col\":");
                    buf_printf(out, "%d", col);
                    buf_puts(out, ",\"msg\":\"");
                    json_escape(out, msg);
                    buf_puts(out, "\"}");
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
    buf_puts(out, "]");
}

static void caskc_path(char *out, size_t cap) { snprintf(out, cap, "%s\\caskc.exe", g_exedir); }

/* caskc --check --diag  on the buffer; fills errs (JSON) and returns rc */
static int do_check(const char *text, size_t len, const char *nearp,
                    Buf *errs, int *rc)
{
    char tmp[1200], cc[1200], args[2400];
    Buf out;
    caskc_path(cc, sizeof cc);
    make_temp(text, len, nearp, tmp, sizeof tmp);
    snprintf(args, sizeof args, "--check --diag \"%s\"", tmp);
    run_capture(cc, args, &out, rc, 30000);
    diag_to_json(errs, out.p ? out.p : "");
    buf_free(&out);
    remove(tmp);
    return 1;
}

/* build the buffer into an exe; fills errs (JSON), log and the exe path */
static int do_build(const char *text, size_t len, const char *nearp, int fast,
                    Buf *errs, Buf *log, char *exepath, size_t execap, int *rc)
{
    char tmp[1200], cc[1200], args[2400];
    char dir[1024];
    Buf out;

    caskc_path(cc, sizeof cc);
    make_temp(text, len, nearp, tmp, sizeof tmp);
    if (nearp && *nearp) {
        const char *base = strrchr(nearp, '\\');
        base = base ? base + 1 : nearp;
        dir_of(nearp, dir, sizeof dir);
        snprintf(exepath, execap, "%s\\%s", dir, base);
        { char *dot = strrchr(exepath, '.'); if (dot) *dot = 0; strcat(exepath, ".exe"); }
    } else {
        char t[900];
        GetTempPathA(900, t);
        snprintf(exepath, execap, "%scaskstudio_run_%d.exe", t, (int)GetCurrentProcessId());
    }
    snprintf(args, sizeof args, "\"%s\" -o \"%s\"%s --diag", tmp, exepath, fast ? " --fast" : "");
    run_capture(cc, args, &out, rc, 120000);
    diag_to_json(errs, out.p ? out.p : "");
    buf_init(log);
    buf_add(log, out.p ? out.p : "", out.len);
    buf_free(&out);
    remove(tmp);
    return *rc == 0;
}

/* ---------------- the running program ---------------- */

static HANDLE g_runin = NULL;

static void stop_run(void)
{
    EnterCriticalSection(&g_runcs);
    if (g_proc) {
        TerminateProcess(g_proc, 1);
        WaitForSingleObject(g_proc, 3000);
    }
    if (g_runin) { CloseHandle(g_runin); g_runin = NULL; }
    LeaveCriticalSection(&g_runcs);
}

static int start_run(const char *exe, Buf *errs, Buf *log, int *id, int *rc)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE wr = NULL, rd = NULL, rin_w = NULL, rin_r = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[1400];

    stop_run();
    buf_init(errs);
    buf_init(log);
    buf_puts(errs, "[]");
    EnterCriticalSection(&g_runcs);
    buf_free(&g_runout);
    buf_init(&g_runout);
    g_rundone = 0;
    g_runcode = -1;
    LeaveCriticalSection(&g_runcs);

    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&rin_r, &rin_w, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); return 0; }
    SetHandleInformation(rin_w, HANDLE_FLAG_INHERIT, 0);

    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = rin_r;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(wr); CloseHandle(rd); CloseHandle(rin_r); CloseHandle(rin_w);
        buf_printf(log, "cannot start %s", exe);
        *rc = -1;
        return 0;
    }
    CloseHandle(wr);
    CloseHandle(rin_r);
    g_proc = pi.hProcess;
    g_rd = rd;
    g_runin = rin_w;
    CloseHandle(pi.hThread);
    g_runid++;
    *id = g_runid;
    *rc = 0;
    {
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, out_reader, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return 1;
}

/* ==================================================================== */
/* the loopback web server (127.0.0.1 only)                              */
/* ==================================================================== */

static int g_port = 0;
static SOCKET g_lsn = INVALID_SOCKET;
static HWND g_mainhwnd = NULL;
static CRITICAL_SECTION g_dlgcs;

typedef struct { int kind; wchar_t path[MAX_PATH]; int ok; HANDLE ev; } DlgReq;
static DlgReq g_dlg;

static void send_all(SOCKET s, const void *data, size_t n)
{
    const char *p = (const char *)data;
    while (n) {
        int r = send(s, p, (int)(n > 60000 ? 60000 : n), 0);
        if (r <= 0) return;
        p += r;
        n -= (size_t)r;
    }
}
static void http_reply(SOCKET s, int code, const char *ctype, const void *body, size_t n)
{
    Buf h;
    buf_init(&h);
    buf_printf(&h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        code, code == 200 ? "OK" : (code == 403 ? "Forbidden" : "Bad Request"),
        ctype, (unsigned)n);
    send_all(s, h.p, h.len);
    if (n) send_all(s, body, n);
    buf_free(&h);
}
static void http_json(SOCKET s, Buf *b)
{
    http_reply(s, 200, "application/json; charset=utf-8", b->p ? b->p : "", b->len);
    buf_free(b);
}

static const char *mime_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (!_stricmp(dot, "html")) return "text/html; charset=utf-8";
    if (!_stricmp(dot, "css")) return "text/css; charset=utf-8";
    if (!_stricmp(dot, "js")) return "text/javascript; charset=utf-8";
    if (!_stricmp(dot, "svg")) return "image/svg+xml";
    if (!_stricmp(dot, "png")) return "image/png";
    if (!_stricmp(dot, "ico")) return "image/x-icon";
    if (!_stricmp(dot, "json")) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

static void serve_static(SOCKET s, const char *urlpath)
{
    char full[4096];
    const char *p = urlpath;
    Buf content;
    if (*p == '/') p++;
    if (strstr(p, "..") || strchr(p, '\\') || strchr(p, ':')) {
        http_reply(s, 400, "text/plain; charset=utf-8", "no", 2);
        return;
    }
    if (!*p) p = "index.html";
    snprintf(full, sizeof full, "%s\\web\\%s", g_exedir, p);
    if (!read_file(full, &content)) {
        http_reply(s, 400, "text/plain; charset=utf-8", "not found", 9);
        return;
    }
    http_reply(s, 200, mime_of(full), content.p, content.len);
    buf_free(&content);
}

/* a native file dialog has to open on the window's own thread */
static int ask_dialog(int kind, char *pathout, size_t cap)
{
    int ok;
    EnterCriticalSection(&g_dlgcs);
    memset(&g_dlg, 0, sizeof g_dlg);
    g_dlg.kind = kind;
    g_dlg.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!PostMessageW(g_mainhwnd, WM_DLGREQ, 0, 0)) {
        CloseHandle(g_dlg.ev);
        LeaveCriticalSection(&g_dlgcs);
        return 0;
    }
    WaitForSingleObject(g_dlg.ev, 60000);
    CloseHandle(g_dlg.ev);
    ok = g_dlg.ok;
    if (ok) {
        char *s = u16to8(g_dlg.path);
        snprintf(pathout, cap, "%s", s);
        free(s);
    }
    LeaveCriticalSection(&g_dlgcs);
    return ok;
}

static void to_crlf(const char *in, size_t n, Buf *out)
{
    size_t i;
    buf_init(out);
    for (i = 0; i < n; i++) {
        if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r')) buf_puts(out, "\r\n");
        else buf_add(out, &in[i], 1);
    }
}
/* save the buffer the way the file is kept: utf-8 or gbk, CRLF lines */
static int save_source(const char *path, const char *text, size_t len, const char *enc)
{
    Buf crlf, out;
    int ok;
    to_crlf(text, len, &crlf);
    if (enc && !_stricmp(enc, "gbk")) {
        u8_to_gbk(crlf.p ? crlf.p : "", crlf.len, &out);
        ok = write_file(path, out.p ? out.p : "", out.len);
        buf_free(&out);
    } else {
        ok = write_file(path, crlf.p ? crlf.p : "", crlf.len);
    }
    buf_free(&crlf);
    return ok;
}

/* ---------------- the API ---------------- */

static void api_output(SOCKET s, int id)
{
    Buf b;
    char *copy = NULL;
    size_t n = 0;
    int done, code;
    buf_init(&b);
    EnterCriticalSection(&g_runcs);
    if (id == g_runid && g_runout.p) {
        n = g_runout.len;
        copy = (char *)malloc(n + 1);
        memcpy(copy, g_runout.p, n);
        copy[n] = 0;
    }
    done = g_rundone;
    code = g_runcode;
    LeaveCriticalSection(&g_runcs);
    buf_puts(&b, "{\"id\":");
    buf_printf(&b, "%d", id);
    buf_puts(&b, ",\"out\":\"");
    json_escape(&b, copy ? copy : "");
    buf_printf(&b, "\",\"done\":%s,\"code\":%d}", done ? "true" : "false", code);
    free(copy);
    http_json(s, &b);
}

static void handle_api(SOCKET s, const char *path, const char *body)
{
    char key[512];

    if (!strcmp(path, "/api/info")) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"app\":\"" APPNAME_A "\",\"version\":\"" APPVER_A "\",\"open\":\"");
        json_escape(&b, g_openfile);
        buf_puts(&b, "\"}");
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/dialog/open")) {
        Buf b;
        char p[1024];
        buf_init(&b);
        if (ask_dialog(0, p, sizeof p)) {
            buf_puts(&b, "{\"path\":\"");
            json_escape(&b, p);
            buf_puts(&b, "\"}");
        } else buf_puts(&b, "{\"cancelled\":true}");
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/dialog/saveas")) {
        Buf b;
        char p[1024], *text = big(), enc[16];
        buf_init(&b);
        jget_str(body, "text", text, BIG);
        enc[0] = 0;
        jget_str(body, "encoding", enc, sizeof enc);
        if (ask_dialog(1, p, sizeof p)) {
            if (save_source(p, text, strlen(text), enc)) {
                buf_puts(&b, "{\"path\":\"");
                json_escape(&b, p);
                buf_puts(&b, "\"}");
            } else buf_puts(&b, "{\"error\":\"cannot write the file\"}");
        } else buf_puts(&b, "{\"cancelled\":true}");
        free(text);
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/load")) {
        Buf b, text;
        char p[1024], enc[16];
        buf_init(&b);
        p[0] = 0;
        jget_str(body, "path", p, sizeof p);
        if (!p[0]) { buf_puts(&b, "{\"error\":\"no path\"}"); http_json(s, &b); return; }
        if (!load_source(p, &text, enc, sizeof enc)) {
            buf_puts(&b, "{\"error\":\"cannot read the file\"}");
            http_json(s, &b);
            return;
        }
        buf_puts(&b, "{\"path\":\"");
        json_escape(&b, p);
        buf_puts(&b, "\",\"text\":\"");
        json_escape(&b, text.p ? text.p : "");
        buf_printf(&b, "\",\"encoding\":\"%s\"}", enc);
        buf_free(&text);
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/save")) {
        Buf b;
        char p[1024], *text = big(), enc[16];
        buf_init(&b);
        p[0] = enc[0] = 0;
        jget_str(body, "path", p, sizeof p);
        jget_str(body, "text", text, BIG);
        jget_str(body, "encoding", enc, sizeof enc);
        if (!p[0]) { buf_puts(&b, "{\"error\":\"no path\"}"); free(text); http_json(s, &b); return; }
        if (save_source(p, text, strlen(text), enc)) buf_puts(&b, "{\"ok\":true}");
        else buf_puts(&b, "{\"error\":\"cannot write the file\"}");
        free(text);
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/enc")) {
        Buf b, t;
        char p[1024], to[16], enc[16];
        buf_init(&b);
        p[0] = to[0] = 0;
        jget_str(body, "path", p, sizeof p);
        jget_str(body, "to", to, sizeof to);
        if (!p[0] || !load_source(p, &t, enc, sizeof enc)) {
            buf_puts(&b, "{\"error\":\"cannot read the file\"}");
            http_json(s, &b);
            return;
        }
        save_source(p, t.p ? t.p : "", t.len, to);
        buf_puts(&b, "{\"text\":\"");
        json_escape(&b, t.p ? t.p : "");
        buf_printf(&b, "\",\"encoding\":\"%s\"}", to);
        buf_free(&t);
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/check")) {
        Buf b, errs;
        char *text = big(), p[1024];
        int rc = 0;
        buf_init(&b);
        p[0] = 0;
        jget_str(body, "text", text, BIG);
        jget_str(body, "path", p, sizeof p);
        do_check(text, strlen(text), p, &errs, &rc);
        free(text);
        buf_puts(&b, "{\"errs\":");
        buf_add(&b, errs.p ? errs.p : "[]", errs.len ? errs.len : 2);
        buf_printf(&b, ",\"rc\":%d}", rc);
        buf_free(&errs);
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/build") || !strcmp(path, "/api/run")) {
        Buf b, errs, log;
        char *text = big(), p[1024], exe[1200];
        int fast = 0, rc = 0, id = 0, isrun = (strcmp(path, "/api/run") == 0);
        buf_init(&b);
        p[0] = 0;
        jget_str(body, "text", text, BIG);
        jget_str(body, "path", p, sizeof p);
        jget_bool(body, "fast", &fast);
        if (!do_build(text, strlen(text), p, fast, &errs, &log, exe, sizeof exe, &rc)) {
            free(text);
            buf_puts(&b, "{\"ok\":false,\"errs\":");
            buf_add(&b, errs.p ? errs.p : "[]", errs.len ? errs.len : 2);
            buf_puts(&b, ",\"log\":\"");
            json_escape(&b, log.p ? log.p : "");
            buf_puts(&b, "\"}");
            buf_free(&errs); buf_free(&log);
            http_json(s, &b);
            return;
        }
        free(text);
        if (!isrun) {
            buf_puts(&b, "{\"ok\":true,\"errs\":");
            buf_add(&b, errs.p ? errs.p : "[]", errs.len ? errs.len : 2);
            buf_puts(&b, ",\"log\":\"");
            json_escape(&b, log.p ? log.p : "");
            buf_puts(&b, "\",\"exe\":\"");
            json_escape(&b, exe);
            buf_puts(&b, "\"}");
            buf_free(&errs); buf_free(&log);
            http_json(s, &b);
            return;
        }
        {
            Buf e2, l2;
            int rc2 = 0;
            start_run(exe, &e2, &l2, &id, &rc2);
            buf_puts(&b, "{\"ok\":true,\"id\":");
            buf_printf(&b, "%d", id);
            buf_puts(&b, ",\"errs\":");
            buf_add(&b, errs.p ? errs.p : "[]", errs.len ? errs.len : 2);
            buf_puts(&b, ",\"log\":\"");
            json_escape(&b, l2.p ? l2.p : "");
            buf_puts(&b, "\"}");
            buf_free(&e2); buf_free(&l2);
            buf_free(&errs); buf_free(&log);
            http_json(s, &b);
        }
        return;
    }
    if (!strncmp(path, "/api/output", 11)) {
        int id = 0;
        const char *q = strchr(path, '?');
        if (q) id = atoi(q + 4);        /* "?id=N" */
        api_output(s, id);
        return;
    }
    if (!strcmp(path, "/api/stop")) {
        stop_run();
        { Buf b; buf_init(&b); buf_puts(&b, "{\"ok\":true}"); http_json(s, &b); }
        return;
    }
    if (!strcmp(path, "/api/send")) {
        Buf b;
        char text[65536];
        DWORD wr = 0;
        buf_init(&b);
        text[0] = 0;
        jget_str(body, "text", text, sizeof text);
        EnterCriticalSection(&g_runcs);
        if (g_runin) {
            WriteFile(g_runin, text, (DWORD)strlen(text), &wr, NULL);
            WriteFile(g_runin, "\r\n", 2, &wr, NULL);
        }
        LeaveCriticalSection(&g_runcs);
        buf_puts(&b, "{\"ok\":true}");
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/title")) {
        Buf b;
        char t[600];
        wchar_t *w;
        buf_init(&b);
        t[0] = 0;
        jget_str(body, "title", t, sizeof t);
        w = u8to16(t);
        SetWindowTextW(g_mainhwnd, w);
        free(w);
        buf_puts(&b, "{\"ok\":true}");
        http_json(s, &b);
        return;
    }
    if (!strcmp(path, "/api/reveal")) {
        Buf b;
        char p[1024];
        wchar_t *w, *args;
        buf_init(&b);
        p[0] = 0;
        jget_str(body, "path", p, sizeof p);
        w = u8to16(p);
        {
            wchar_t cmd[1200];
            _snwprintf(cmd, 1199, L"/select,%s", w);
            cmd[1199] = 0;
            args = _wcsdup(cmd);
        }
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        free(args);
        free(w);
        buf_puts(&b, "{\"ok\":true}");
        http_json(s, &b);
        return;
    }
    (void)key;
    { Buf b; buf_init(&b); buf_puts(&b, "{\"error\":\"unknown api\"}"); http_json(s, &b); }
}

/* ---------------- one connection ---------------- */

static unsigned __stdcall conn_thread(void *arg)
{
    SOCKET s = (SOCKET)(intptr_t)arg;
    Buf req, body;
    char line[8192];
    int got_hdr = 0, content_len = 0;
    char method[16], url[2048], origin[512];
    char *q, *sp1, *sp2;
    DWORD timeout = 20000;

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    buf_init(&req);
    /* headers */
    while (!got_hdr) {
        int n = recv(s, line, (int)sizeof line - 1, 0);
        char *hit;
        if (n <= 0) { closesocket(s); buf_free(&req); return 0; }
        line[n] = 0;
        buf_puts(&req, line);
        hit = strstr(req.p ? req.p : "", "\r\n\r\n");
        if (hit) got_hdr = 1;
        else if (req.len > 65536) { closesocket(s); buf_free(&req); return 0; }
    }
    {
        /* how big is the body? */
        char *cl = strstr(req.p, "\r\nContent-Length:");
        if (!cl) cl = strstr(req.p, "\r\ncontent-length:");
        if (cl) content_len = atoi(cl + 17);
        if (content_len < 0 || content_len > (16 << 20)) content_len = 0;
    }
    {
        char *h = strstr(req.p, "\r\n\r\n");
        size_t hdrlen = h ? (size_t)(h + 4 - req.p) : req.len;
        while (req.len < hdrlen + (size_t)content_len) {
            int n = recv(s, line, (int)sizeof line - 1, 0);
            if (n <= 0) break;
            buf_add(&req, line, (size_t)n);
        }
    }
    buf_init(&body);
    {
        char *h = strstr(req.p, "\r\n\r\n");
        if (h && content_len > 0) {
            size_t have = req.len - (size_t)(h + 4 - req.p);
            if ((size_t)content_len < have) have = (size_t)content_len;
            buf_add(&body, h + 4, have);
        }
    }
    /* request line */
    method[0] = url[0] = origin[0] = 0;
    sp1 = strchr(req.p, ' ');
    if (sp1) {
        size_t mn = (size_t)(sp1 - req.p);
        if (mn > 15) mn = 15;
        memcpy(method, req.p, mn);
        method[mn] = 0;
        sp2 = strchr(sp1 + 1, ' ');
        if (sp2) {
            size_t un = (size_t)(sp2 - sp1 - 1);
            if (un > 2047) un = 2047;
            memcpy(url, sp1 + 1, un);
            url[un] = 0;
        }
    }
    q = strstr(req.p, "\r\nOrigin:");
    if (q) {
        char *e = strstr(q + 9, "\r\n");
        size_t on = e ? (size_t)(e - (q + 9)) : 0;
        if (on > 511) on = 511;
        memcpy(origin, q + 9, on);
        origin[on] = 0;
    }
    /* only our own page may call us */
    if (origin[0]) {
        char want[128];
        snprintf(want, sizeof want, "http://127.0.0.1:%d", g_port);
        if (strstr(origin, want) == NULL) {
            http_reply(s, 403, "text/plain; charset=utf-8", "forbidden", 9);
            closesocket(s);
            buf_free(&req); buf_free(&body);
            return 0;
        }
    }
    if (!strcmp(method, "GET") && strncmp(url, "/api/", 5) != 0)
        serve_static(s, url);
    else if (!strcmp(method, "POST"))
        handle_api(s, url, body.p ? body.p : "");
    else if (!strcmp(method, "GET") && !strncmp(url, "/api/", 5))
        handle_api(s, url, body.p ? body.p : "");
    else {
        Buf e; buf_init(&e);
        buf_puts(&e, "{\"error\":\"method\"}");
        http_json(s, &e);
    }
    closesocket(s);
    buf_free(&req);
    buf_free(&body);
    return 0;
}

static unsigned __stdcall accept_thread(void *arg)
{
    (void)arg;
    for (;;) {
        SOCKET c = accept(g_lsn, NULL, NULL);
        HANDLE th;
        if (c == INVALID_SOCKET) break;
        th = (HANDLE)_beginthreadex(NULL, 0, conn_thread, (void *)(intptr_t)c, 0, NULL);
        if (th) CloseHandle(th);
        else closesocket(c);
    }
    return 0;
}

static int g_forceport = 0;      /* --port=N: handy when testing */

static int start_server(void)
{
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    WSADATA wsa;
    int yes = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_lsn == INVALID_SOCKET) return 0;
    setsockopt(g_lsn, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 only */
    a.sin_port = htons((unsigned short)g_forceport);  /* 0 = any free port */
    if (bind(g_lsn, (struct sockaddr *)&a, sizeof a) != 0) return 0;
    getsockname(g_lsn, (struct sockaddr *)&a, &alen);
    g_port = ntohs(a.sin_port);
    if (listen(g_lsn, 16) != 0) return 0;
    {
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, accept_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return 1;
}

/* ==================================================================== */
/* the window                                                            */
/* ==================================================================== */

static void do_dialogs(void)
{
    if (g_dlg.kind == 0) {          /* open */
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH];
        file[0] = 0;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_mainhwnd;
        ofn.lpstrFilter = L"C* program (*.cask)\0*.cask\0All files (*.*)\0*.*\0\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) { wcscpy(g_dlg.path, file); g_dlg.ok = 1; }
    } else {                        /* save as */
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH];
        file[0] = 0;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_mainhwnd;
        ofn.lpstrFilter = L"C* program (*.cask)\0*.cask\0All files (*.*)\0*.*\0\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"cask";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&ofn)) { wcscpy(g_dlg.path, file); g_dlg.ok = 1; }
    }
    SetEvent(g_dlg.ev);
}

static void make_dark(HWND hwnd)
{
    BOOL yes = TRUE;
    /* 20 = dark title bar on new Windows 11, 19 = older builds */
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &yes, sizeof yes)))
        DwmSetWindowAttribute(hwnd, 19, &yes, sizeof yes);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        wv2_resize();
        return 0;
    case WM_DLGREQ:
        do_dialogs();
        return 0;
    case WM_SETFOCUS:
        wv2_resize();
        return 0;
    case WM_DESTROY:
        stop_run();
        if (g_lsn != INVALID_SOCKET) {
            shutdown(g_lsn, SD_BOTH);
            closesocket(g_lsn);
            g_lsn = INVALID_SOCKET;
        }
        wv2_close();
        WSACleanup();
        DeleteCriticalSection(&g_runcs);
        DeleteCriticalSection(&g_dlgcs);
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show)
{
    HWND hwnd;
    WNDCLASSW wc;
    int argc = 0;
    wchar_t **argv;
    wchar_t url[256];
    wchar_t userdir[1100];

    (void)prev; (void)cmd; (void)show;
    find_exedir();
    InitializeCriticalSection(&g_runcs);
    InitializeCriticalSection(&g_dlgcs);
    buf_init(&g_runout);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int i;
        for (i = 1; i < argc; i++) {
            char *s = u16to8(argv[i]);
            if (!strncmp(s, "--port=", 7)) g_forceport = atoi(s + 7);
            else if (!g_openfile[0]) snprintf(g_openfile, sizeof g_openfile, "%s", s);
            free(s);
        }
    }
    if (argv) LocalFree(argv);

    if (!start_server()) {
        MessageBoxW(NULL, L"cannot start the local web server", LAPPNAMEW, MB_ICONERROR);
        return 1;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CStarStudio";
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, L"CStarStudio", LAPPNAMEW,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1240, 840,
        NULL, NULL, hInst, NULL);
    if (!hwnd) { MessageBoxW(NULL, L"cannot create the window", LAPPNAMEW, MB_ICONERROR); return 1; }
    make_dark(hwnd);
    g_mainhwnd = hwnd;
    ShowWindow(hwnd, show ? show : SW_SHOW);
    UpdateWindow(hwnd);

    _snwprintf(url, 255, L"http://127.0.0.1:%d/", g_port);
    url[255] = 0;
    {
        wchar_t *exedir = u8to16(g_exedir);
        _snwprintf(userdir, 1099, L"%s\\webdata", exedir);
        userdir[1099] = 0;
        free(exedir);
    }
    if (wv2_start(hwnd, url, userdir) != 0) {
        wchar_t msg[1600];
        const char *e = wv2_error();
        wchar_t *we = u8to16(e);
        _snwprintf(msg, 1599,
            L"%hs (%hs)\n\n"
            L"This program shows the editor inside a small browser window\n"
            L"that Windows provides (WebView2). It looks like it is missing.\n\n"
            L"Press Yes and the editor will open in your normal browser instead;\n"
            L"press No to close.\n\n"
            L"Download WebView2: https://developer.microsoft.com/microsoft-edge/webview2/",
            APPNAME_A, we);
        msg[1599] = 0;
        free(we);
        if (MessageBoxW(hwnd, msg, LAPPNAMEW, MB_ICONWARNING | MB_YESNO) == IDYES)
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        else { DestroyWindow(hwnd); }
    }

    {
        MSG m;
        while (GetMessageW(&m, NULL, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
