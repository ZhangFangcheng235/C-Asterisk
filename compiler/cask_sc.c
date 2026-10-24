/* ============================================================
 *  cask_sc.c -- the prebuilt runtime half for C_SC.cash
 *
 *  libcastrt.a ships this so .cask programs can
 *      use "C_SC.cash";
 *  and call isportable / copytoexe / freebytes / diskfree /
 *  is64bit / portcheck / portscan with no C compiler anywhere in
 *  the pipeline. Every export keeps the caskw_ prefix because the
 *  assembler backend maps ALL header natives to caskw_<C* name>.
 *
 *  Return-type contract (must match the NativeApi table in
 *  caskc.c): numeric natives -> long long; text natives ->
 *  const char*.
 * ============================================================ */
#include "C_SC.cash"

long long   caskw_isportable(void)              { return cask_sc_isportable(); }
long long   caskw_copytoexe(const char *name, const char *content)
{
    return cask_sc_copytoexe(name, content);
}
long long   caskw_freebytes(void)               { return cask_sc_freebytes(); }
long long   caskw_diskfree(const char *path)    { return cask_sc_diskfree(path); }
long long   caskw_is64bit(void)                { return cask_sc_is64bit(); }
long long   caskw_portcheck(long long port)     { return cask_sc_portcheck((int)port); }
long long   caskw_portscan(long long lo, long long hi)
{
    return cask_sc_portscan((int)lo, (int)hi);
}
