/* nexwebview.h - host a WebView2 in a plain Win32 window, from C, without
   WebView2Loader.dll or the WebView2 SDK headers.

   Microsoft's loader finds the installed Evergreen runtime and calls
   CreateWebViewEnvironmentWithOptionsInternal in its
   EBWebView\x64\EmbeddedBrowserWebView.dll; this does the same. The handful of
   interfaces used are called by vtable slot (their methods are declared in a
   fixed order in WebView2.idl), and the objects the runtime hands back are
   QueryInterface'd for the interface they must be first, so a mismatch ends
   in a failure code, not in a call through the wrong method.

   Used by the host's main window (host_window.inc). Everything runs on the
   window's thread, which must be a COM single-threaded apartment with a
   message loop; creation is asynchronous and finishes from that loop.
   Header-only, one translation unit. */
#ifndef NEX_WEBVIEW_H
#define NEX_WEBVIEW_H

#ifndef NEX_HDR_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define NEX_HDR_UNUSED __attribute__((unused))
#else
#define NEX_HDR_UNUSED
#endif
#endif

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static const GUID NEX_IID_WV_ENVIRONMENT =
    {0xb96d755e, 0x0319, 0x4e92, {0xa2, 0x96, 0x23, 0x43, 0x6f, 0x46, 0xa1, 0xfc}};
static const GUID NEX_IID_WV_CONTROLLER =
    {0x4d00c0d1, 0x9434, 0x4eb6, {0x80, 0x78, 0x86, 0x97, 0xa5, 0x60, 0x33, 0x4f}};
static const GUID NEX_IID_WV_WEBVIEW =
    {0x76eceacb, 0x0462, 0x4d94, {0xac, 0x83, 0x42, 0x3a, 0x67, 0x93, 0x77, 0x5e}};
static const GUID NEX_IID_WV_SETTINGS =
    {0xe562e4f0, 0xd7fa, 0x43ac, {0x8d, 0x71, 0xc0, 0x51, 0x50, 0x49, 0x9f, 0x00}};

/* slots, after IUnknown's 0..2 */
enum {
    NEX_WV_ENV_CREATE_CONTROLLER = 3,
    NEX_WV_CTL_PUT_ISVISIBLE = 4, NEX_WV_CTL_PUT_BOUNDS = 6,
    NEX_WV_CTL_CLOSE = 24, NEX_WV_CTL_GET_WEBVIEW = 25,
    NEX_WV_GET_SETTINGS = 3, NEX_WV_NAVIGATE = 5, NEX_WV_GET_BROWSER_PID = 37,
    NEX_WV_SET_DEVTOOLS = 12, NEX_WV_SET_CONTEXT_MENUS = 14, NEX_WV_SET_STATUS_BAR = 10,
    NEX_WV_SET_ZOOM_CONTROL = 18
};

typedef struct NexWebView NexWebView;

/* A completion handler: IUnknown plus Invoke(errorCode, result). */
typedef struct {
    void **vtbl;
    LONG ref;
    NexWebView *owner;
} NexWvHandler;

struct NexWebView {
    HWND hwnd;
    void *env;          /* ICoreWebView2Environment */
    void *controller;   /* ICoreWebView2Controller */
    void *webview;      /* ICoreWebView2 */
    WCHAR url[512];
    WCHAR user_data[MAX_PATH];
    HRESULT error;      /* first failure, for the log */
    int failed;
    NexWvHandler env_handler, ctl_handler;
    void (*on_ready)(NexWebView *);
    void (*on_failed)(NexWebView *, const char *what);
};

#define NEX_WV_VT(o) (*(void ***)(o))
static NEX_HDR_UNUSED ULONG nex_wv_release(void *o) {
    return o ? ((ULONG (STDMETHODCALLTYPE *)(void *))NEX_WV_VT(o)[2])(o) : 0;
}
static NEX_HDR_UNUSED void *nex_wv_as(void *o, const GUID *iid) {
    void *out = NULL;
    if (o && FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **))NEX_WV_VT(o)[0])(o, iid, &out)))
        return NULL;
    return out;
}

static HRESULT STDMETHODCALLTYPE nex_wvh_qi(NexWvHandler *h, REFIID iid, void **out) {
    (void)iid;              /* the runtime asks for the handler's own interface */
    *out = h;
    InterlockedIncrement(&h->ref);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE nex_wvh_addref(NexWvHandler *h) { return InterlockedIncrement(&h->ref); }
static ULONG STDMETHODCALLTYPE nex_wvh_release(NexWvHandler *h) { return InterlockedDecrement(&h->ref); }

static void nex_wv_fail(NexWebView *w, const char *what, HRESULT hr) {
    if (!w->failed) { w->failed = 1; w->error = hr; if (w->on_failed) w->on_failed(w, what); }
}

static NEX_HDR_UNUSED void nex_wv_set_bounds(NexWebView *w) {
    if (!w->controller || !w->hwnd) return;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, RECT))NEX_WV_VT(w->controller)[NEX_WV_CTL_PUT_BOUNDS])(w->controller, rc);
}

static HRESULT STDMETHODCALLTYPE nex_wvh_controller_done(NexWvHandler *h, HRESULT err, void *raw) {
    NexWebView *w = h->owner;
    if (FAILED(err) || !raw) { nex_wv_fail(w, "create controller", FAILED(err) ? err : E_POINTER); return S_OK; }
    w->controller = nex_wv_as(raw, &NEX_IID_WV_CONTROLLER);
    if (!w->controller) { nex_wv_fail(w, "controller interface", E_NOINTERFACE); return S_OK; }
    void *wv = NULL;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))NEX_WV_VT(w->controller)[NEX_WV_CTL_GET_WEBVIEW])(w->controller, &wv);
    w->webview = wv ? nex_wv_as(wv, &NEX_IID_WV_WEBVIEW) : NULL;
    if (wv) nex_wv_release(wv);
    if (!w->webview) { nex_wv_fail(w, "webview interface", E_NOINTERFACE); return S_OK; }
    void *s = NULL;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))NEX_WV_VT(w->webview)[NEX_WV_GET_SETTINGS])(w->webview, &s);
    void *settings = s ? nex_wv_as(s, &NEX_IID_WV_SETTINGS) : NULL;
    if (s) nex_wv_release(s);
    if (settings) {
        /* an app window, not a browser: no dev tools, context menu, status bar or ctrl+wheel zoom */
        typedef HRESULT (STDMETHODCALLTYPE *PutBool)(void *, BOOL);
        ((PutBool)NEX_WV_VT(settings)[NEX_WV_SET_DEVTOOLS])(settings, FALSE);
        ((PutBool)NEX_WV_VT(settings)[NEX_WV_SET_CONTEXT_MENUS])(settings, FALSE);
        ((PutBool)NEX_WV_VT(settings)[NEX_WV_SET_STATUS_BAR])(settings, FALSE);
        ((PutBool)NEX_WV_VT(settings)[NEX_WV_SET_ZOOM_CONTROL])(settings, FALSE);
        nex_wv_release(settings);
    }
    nex_wv_set_bounds(w);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, BOOL))NEX_WV_VT(w->controller)[NEX_WV_CTL_PUT_ISVISIBLE])(w->controller, TRUE);
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, LPCWSTR))NEX_WV_VT(w->webview)[NEX_WV_NAVIGATE])(w->webview, w->url);
    if (FAILED(hr)) { nex_wv_fail(w, "navigate", hr); return S_OK; }
    if (w->on_ready) w->on_ready(w);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE nex_wvh_env_done(NexWvHandler *h, HRESULT err, void *raw) {
    NexWebView *w = h->owner;
    if (FAILED(err) || !raw) { nex_wv_fail(w, "create environment", FAILED(err) ? err : E_POINTER); return S_OK; }
    w->env = nex_wv_as(raw, &NEX_IID_WV_ENVIRONMENT);
    if (!w->env) { nex_wv_fail(w, "environment interface", E_NOINTERFACE); return S_OK; }
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, HWND, void *))NEX_WV_VT(w->env)[NEX_WV_ENV_CREATE_CONTROLLER])
                 (w->env, w->hwnd, &w->ctl_handler);
    if (FAILED(hr)) nex_wv_fail(w, "create controller", hr);
    return S_OK;
}

static void *g_nex_wvh_env_vtbl[4] = { (void *)nex_wvh_qi, (void *)nex_wvh_addref, (void *)nex_wvh_release,
                                       (void *)nex_wvh_env_done };
static void *g_nex_wvh_ctl_vtbl[4] = { (void *)nex_wvh_qi, (void *)nex_wvh_addref, (void *)nex_wvh_release,
                                       (void *)nex_wvh_controller_done };

/* The installed Evergreen runtime's EmbeddedBrowserWebView.dll, or "". */
static NEX_HDR_UNUSED int nex_wv_runtime_dll(WCHAR *out, size_t cap) {
    static const WCHAR *keys[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    };
    HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    out[0] = 0;
    for (int r = 0; r < 2; r++) for (int k = 0; k < 2; k++) {
        HKEY h;
        if (RegOpenKeyExW(roots[r], keys[k], 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) continue;
        WCHAR ver[64] = L"", loc[MAX_PATH] = L"";
        DWORD n = sizeof(ver), m = sizeof(loc);
        RegQueryValueExW(h, L"pv", NULL, NULL, (BYTE *)ver, &n);
        LONG lr = RegQueryValueExW(h, L"location", NULL, NULL, (BYTE *)loc, &m);
        RegCloseKey(h);
        if (!ver[0] || wcscmp(ver, L"0.0.0.0") == 0) continue;
        if (lr != ERROR_SUCCESS || !loc[0]) {
            /* older installs do not store a location: use the default folders */
            WCHAR pf[MAX_PATH];
            if (!GetEnvironmentVariableW(r == 0 ? L"ProgramFiles(x86)" : L"LOCALAPPDATA", pf, MAX_PATH)) continue;
            _snwprintf(loc, MAX_PATH, L"%ls\\Microsoft\\EdgeWebView\\Application", pf);
        }
        _snwprintf(out, cap, L"%ls\\%ls\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", loc, ver);
        out[cap - 1] = 0;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        out[0] = 0;
    }
    return 0;
}

/* Start creating the WebView inside w->hwnd; returns 0 if it cannot even
   start (no runtime, entry point missing). Completion is reported through
   on_ready / on_failed from the window thread's message loop. */
static NEX_HDR_UNUSED int nex_wv_start(NexWebView *w, const char **why) {
    typedef HRESULT (STDMETHODCALLTYPE *CreateEnvFn)(BOOL, int, PCWSTR, void *, void *);
    WCHAR dll[MAX_PATH * 2];
    if (!nex_wv_runtime_dll(dll, MAX_PATH * 2)) { *why = "WebView2 runtime not installed"; return 0; }
    HMODULE mod = LoadLibraryW(dll);
    CreateEnvFn create = mod ? (CreateEnvFn)(void *)GetProcAddress(mod, "CreateWebViewEnvironmentWithOptionsInternal") : NULL;
    if (!create) { *why = "WebView2 entry point missing"; return 0; }
    w->env_handler.vtbl = g_nex_wvh_env_vtbl;  w->env_handler.ref = 1; w->env_handler.owner = w;
    w->ctl_handler.vtbl = g_nex_wvh_ctl_vtbl;  w->ctl_handler.ref = 1; w->ctl_handler.owner = w;
    /* checkRunningInstance=TRUE, runtime type 0 = the installed runtime */
    HRESULT hr = create(TRUE, 0, w->user_data[0] ? w->user_data : NULL, NULL, &w->env_handler);
    if (FAILED(hr)) { *why = "WebView2 environment refused"; w->error = hr; return 0; }
    return 1;
}

/* Tear the WebView down (the window can stay). */
static NEX_HDR_UNUSED void nex_wv_close(NexWebView *w) {
    if (w->controller)
        ((HRESULT (STDMETHODCALLTYPE *)(void *))NEX_WV_VT(w->controller)[NEX_WV_CTL_CLOSE])(w->controller);
    if (w->webview) nex_wv_release(w->webview);
    if (w->controller) nex_wv_release(w->controller);
    if (w->env) nex_wv_release(w->env);
    w->webview = w->controller = w->env = NULL;
}

static NEX_HDR_UNUSED DWORD nex_wv_browser_pid(NexWebView *w) {
    UINT32 pid = 0;
    if (w->webview)
        ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))NEX_WV_VT(w->webview)[NEX_WV_GET_BROWSER_PID])(w->webview, &pid);
    return pid;
}

#endif
