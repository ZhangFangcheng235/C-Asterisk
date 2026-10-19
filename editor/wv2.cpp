/* wv2.cpp -- the small piece of C++ that holds the WebView2 browser.
 *
 * The rest of C* Studio is plain C.  This file only does the COM work
 * that WebView2 needs and offers four plain C calls to webide.c:
 *
 *     int  wv2_start(HWND, url, user_data_folder);   0 = ok
 *     void wv2_resize(void);                          fit the client area
 *     void wv2_close(void);
 *     const char *wv2_error(void);                    last failure, UTF-8
 *
 * The WebView2 loader DLL is found next to the exe (bin\WebView2Loader.dll).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include "WebView2.h"

typedef HRESULT(STDAPICALLTYPE *PFN_CreateEnv)(
    PCWSTR browser_exe, PCWSTR user_data,
    ICoreWebView2EnvironmentOptions *opts,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *done);

static HMODULE g_loader = NULL;
static PFN_CreateEnv g_create = NULL;
static ICoreWebView2Environment *g_env = NULL;
static ICoreWebView2Controller *g_ctl = NULL;
static ICoreWebView2 *g_web = NULL;
static HWND g_hwnd = NULL;
static wchar_t g_url[1024];
static char g_err[512];

static void seterr(const char *msg)
{
    snprintf(g_err, sizeof g_err, "%s", msg);
}

/* ---- the two COM callbacks, written by hand (no WRL on MinGW) ---- */
class EnvDone : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvDone() : m_ref(1) {}
    virtual ~EnvDone() {}
    /* MinGW has no __uuidof for the WebView2 interfaces, and only
     * WebView2 itself ever calls this, so any iid means "the same
     * interface, or IUnknown" here. */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv)
    {
        (void)riid;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef(void) { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release(void)
    {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env);
private:
    LONG m_ref;
};

class CtlDone : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    CtlDone() : m_ref(1) {}
    virtual ~CtlDone() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv)
    {
        (void)riid;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef(void) { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release(void)
    {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *ctl);
private:
    LONG m_ref;
};

static void fit(void)
{
    RECT rc;
    if (!g_ctl || !g_hwnd) return;
    GetClientRect(g_hwnd, &rc);
    g_ctl->put_Bounds(rc);
}

static void on_ctl(HRESULT hr, ICoreWebView2Controller *ctl)
{
    ICoreWebView2Settings *st = NULL;
    if (hr != S_OK || !ctl) { seterr("cannot start the browser (WebView2 controller)"); return; }
    g_ctl = ctl;
    ctl->AddRef();
    if (ctl->get_CoreWebView2(&g_web) != S_OK || !g_web) {
        seterr("cannot start the browser (no WebView2 core)");
        return;
    }
    fit();
    ctl->put_IsVisible(TRUE);
    if (g_web->get_Settings(&st) == S_OK && st) {
        st->put_AreDevToolsEnabled(TRUE);       /* F12 for curious kids */
        st->put_IsStatusBarEnabled(FALSE);
        st->put_AreDefaultContextMenusEnabled(TRUE);
        st->Release();
    }
    g_web->Navigate(g_url);
}

HRESULT EnvDone::Invoke(HRESULT hr, ICoreWebView2Environment *env)
{
    if (hr != S_OK || !env) { seterr("cannot start the browser (WebView2 environment)"); return S_OK; }
    g_env = env;
    env->AddRef();
    env->CreateCoreWebView2Controller(g_hwnd, new CtlDone());
    return S_OK;
}

HRESULT CtlDone::Invoke(HRESULT hr, ICoreWebView2Controller *ctl)
{
    on_ctl(hr, ctl);
    return S_OK;
}

extern "C" {

int wv2_start(HWND hwnd, const wchar_t *url, const wchar_t *userdata)
{
    wchar_t dll[1024];
    HRESULT hr;
    g_hwnd = hwnd;
    wcsncpy(g_url, url, 1023);
    g_url[1023] = 0;
    g_err[0] = 0;

    if (!GetModuleFileNameW(NULL, dll, 1000)) return -1;
    {
        wchar_t *slash = wcsrchr(dll, L'\\');
        if (slash) wcscpy(slash + 1, L"WebView2Loader.dll");
        else wcscpy(dll, L"WebView2Loader.dll");
    }
    g_loader = LoadLibraryW(dll);
    if (!g_loader) { seterr("WebView2Loader.dll not found next to caskide.exe"); return -1; }
    g_create = (PFN_CreateEnv)GetProcAddress(g_loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!g_create) { seterr("WebView2Loader.dll is broken (no entry point)"); return -1; }

    hr = g_create(NULL, userdata, NULL, new EnvDone());
    if (FAILED(hr)) { seterr("cannot ask Windows for the WebView2 browser"); return -1; }
    return 0;
}

void wv2_resize(void)
{
    fit();
}

void wv2_close(void)
{
    if (g_web) { g_web->Release(); g_web = NULL; }
    if (g_ctl) { g_ctl->Close(); g_ctl->Release(); g_ctl = NULL; }
    if (g_env) { g_env->Release(); g_env = NULL; }
}

const char *wv2_error(void)
{
    return g_err;
}

} /* extern "C" */
