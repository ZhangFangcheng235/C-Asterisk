/* ============================================================
 *  cask_net.c -- the prebuilt runtime half for C_NET.cash
 *
 *  libcastrt.a ships this so .cask programs can
 *      use "C_NET.cash";
 *  and call isurl / isip / urlhost / urlport / fnv1a / base64 /
 *  unbase64 / hostalive with no C compiler anywhere in the
 *  pipeline. Every export keeps the caskw_ prefix because the
 *  assembler backend maps ALL header natives to caskw_<C* name>.
 *
 *  Return-type contract (must match the NativeApi table in
 *  caskc.c): numeric natives -> long long; text natives ->
 *  const char*.
 * ============================================================ */
#include "C_NET.cash"

long long   caskw_isurl(const char *s)          { return cask_net_isurl(s); }
long long   caskw_isip(const char *s)           { return cask_net_isip(s); }
const char *caskw_urlhost(const char *url)      { return cask_net_urlhost(url); }
long long   caskw_urlport(const char *url)      { return cask_net_urlport(url); }
long long   caskw_fnv1a(const char *s)         { return cask_net_fnv1a(s); }
const char *caskw_base64(const char *s)        { return cask_net_base64(s); }
const char *caskw_unbase64(const char *s)      { return cask_net_unbase64(s); }
long long   caskw_hostalive(const char *host, long long port)
{
    return cask_net_hostalive(host, (int)port);
}
