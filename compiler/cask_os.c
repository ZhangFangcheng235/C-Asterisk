/* ============================================================
 *  cask_os.c -- the prebuilt runtime half for C_OS.cash
 *
 *  libcastrt.a ships this so .cask programs can
 *      use "C_OS.cash";
 *  and call os_pid / os_cores / ... / findproc with no C
 *  compiler anywhere in the pipeline. Every export keeps the
 *  caskw_ prefix because the assembler backend maps ALL header
 *  natives to caskw_<C* name>.
 *
 *  Return-type contract (must match the NativeApi table in
 *  caskc.c): numeric natives return a 64-bit value -> long long;
 *  text natives return a pointer -> const char*.
 * ============================================================ */
#include "C_OS.cash"

long long   caskw_os_pid(void)             { return cask_os_pid(); }
long long   caskw_os_cores(void)           { return cask_os_cores(); }
long long   caskw_os_ram(void)            { return cask_os_ram(); }
const char *caskw_os_cpu(void)            { return cask_os_cpu(); }
const char *caskw_os_hostname(void)       { return cask_os_hostname(); }
const char *caskw_os_platform(void)       { return cask_os_platform(); }
long long   caskw_isadmin(void)           { return cask_os_isadmin(); }
long long   caskw_uptime(void)           { return cask_os_uptime(); }
long long   caskw_findproc(const char *name) { return cask_os_findproc(name); }
