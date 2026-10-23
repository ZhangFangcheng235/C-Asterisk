/* ============================================================
 * cask_gui.c -- C* window toolkit, compiled into libcastrt.a
 *
 * Includes the ORIGINAL C_OPED.cash / C_CSS.cash implementations
 * (Win32 on Windows, GTK3 on Linux) and exports them under
 * caskw_* names so the assembler backend can call them with the
 * C* ABI (all integers/pointers in int registers, doubles in xmm).
 *
 * The C* program's event handlers (on_click / on_change / on_close)
 * are weak references inside C_OPED.cash; the assembler backend
 * emits them as global symbols with their original names, so the
 * linker binds them exactly like the old C backend did.
 * ============================================================ */

/* on Linux the toolkit defines a static GetTickCount() shim; rename it
 * so a global export of the same name can be provided below. */
#ifndef _WIN32
#define GetTickCount cask_gtk_GetTickCount
#endif

#include "C_OPED.cash"
#include "C_CSS.cash"

#undef GetTickCount

#ifndef _WIN32
long long GetTickCount(void) { return cask_gtk_GetTickCount(); }
#endif

/* ---------------- exports: the window toolkit ---------------- */
long long caskw_window(const char *t, long long w, long long h) { return window(t, w, h); }
long long caskw_label(const char *t, long long x, long long y, long long w, long long h) { return label(t, x, y, w, h); }
long long caskw_button(const char *t, long long x, long long y, long long w, long long h) { return button(t, x, y, w, h); }
long long caskw_textbox(const char *t, long long x, long long y, long long w, long long h) { return textbox(t, x, y, w, h); }
long long caskw_memo(const char *t, long long x, long long y, long long w, long long h) { return memo(t, x, y, w, h); }
long long caskw_checkbox(const char *t, long long x, long long y, long long w, long long h) { return checkbox(t, x, y, w, h); }
long long caskw_listbox(long long x, long long y, long long w, long long h) { return listbox(x, y, w, h); }
long long caskw_progress(long long x, long long y, long long w, long long h) { return progress(x, y, w, h); }

long long caskw_settext(long long id, const char *s) { return settext(id, s); }
const char *caskw_readtext(long long id)             { return readtext(id); }
long long caskw_settitle(long long id, const char *s){ return settitle(id, s); }
long long caskw_additem(long long id, const char *s) { return additem(id, s); }
long long caskw_checked(long long id)                { return checked(id); }
long long caskw_setchecked(long long id, long long v){ return setchecked(id, v); }
long long caskw_setvalue(long long id, long long v)  { return setvalue(id, v); }
long long caskw_getvalue(long long id)               { return getvalue(id); }

long long caskw_msgbox(const char *text, const char *title) { return msgbox(text, title); }
long long caskw_alert(const char *text)              { return alert(text); }
long long caskw_ask(const char *text, const char *title) { return ask(text, title); }
long long caskw_sleep(long long ms)                  { return sleep(ms); }
long long caskw_run(void)                            { return run(); }
long long caskw_launch(const char *cmd)              { return launch(cmd); }
long long caskw_destroy(long long id)                { return destroy(id); }

/* ---------------- exports: CSS styling (C_CSS.cash) ---------------- */
long long caskw_csswindow(void)                      { return csswindow(); }
long long caskw_cstext(long long id, const char *c)  { return cstext(id, c); }
long long caskw_csbg(long long id, const char *c)    { return csbg(id, c); }
long long caskw_csfont(long long id, long long pt, long long bold) { return csfont(id, pt, bold); }
long long caskw_csradius(long long id, long long px) { return csradius(id, px); }
long long caskw_csshadow(long long id, long long on) { return csshadow(id, on); }
long long caskw_cshide(long long id)                 { return cshide(id); }
long long caskw_csshow(long long id)                 { return csshow(id); }
long long caskw_cspass(long long id)                 { return cspass(id); }
long long caskw_cscolor(const char *name, const char *c) { return cscolor(name, c); }
long long caskw_csapply(const char *name, long long id)  { return csapply(name, id); }
