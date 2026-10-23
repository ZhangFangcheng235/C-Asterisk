/* ============================================================
 *  cask_io.c -- the prebuilt runtime half for C_IO.cash
 *
 *  libcastrt.a ships this so .cask programs can
 *      use "C_IO.cash";
 *  and then call readfile / writefile / ... / launchwait with no
 *  C compiler anywhere in the pipeline.  Every export keeps the
 *  caskw_ prefix because the assembler backend maps ALL header
 *  natives to caskw_<name> (see asm_backend.inc / aexpr_call).
 * ============================================================ */
#include "C_IO.cash"

const char *caskw_readfile(const char *path)                 { return cask_io_readfile(path); }
long long   caskw_writefile(const char *p, const char *d)    { return cask_io_writefile(p, d); }
long long   caskw_appendfile(const char *p, const char *d)   { return cask_io_appendfile(p, d); }
long long   caskw_fileexists(const char *path)               { return cask_io_fileexists(path); }
long long   caskw_makedir(const char *path)                  { return cask_io_makedir(path); }
const char *caskw_appdir(void)                               { return cask_io_appdir(); }
const char *caskw_envget(const char *name)                   { return cask_io_envget(name); }
long long   caskw_launchwait(const char *cmd)                { return cask_io_launchwait(cmd); }
