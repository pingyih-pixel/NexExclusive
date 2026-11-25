/* nexsmtc.h - read Windows' media sessions (SMTC, the "now playing" list)
   from plain C, without the WinRT headers (the toolchain ships none for
   Windows.Media.Control).

   Why the host needs it (ROADMAP 5c): while the client holds the device
   exclusively, a browser that starts a video gets no device. Chrome then plays
   on a silent fake output, and the audio session layer says nothing; but its
   media session still reports Playing here, within a poll. That is the signal
   for "another application wants to sound".

   Only four interfaces are used, called by vtable slot. Every object is first
   QueryInterface'd for the interface it must be, so a wrong slot or IID ends
   in a failed QI and a disabled watcher, not in a call through the wrong
   method. combase.dll is loaded at run time: nothing new is linked.

   Header-only: one translation unit includes it. Calls happen on one thread,
   which nex_smtc_open initializes as a WinRT multithreaded apartment. */
#ifndef NEX_SMTC_H
#define NEX_SMTC_H

#ifndef NEX_HDR_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define NEX_HDR_UNUSED __attribute__((unused))
#else
#define NEX_HDR_UNUSED
#endif
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct HSTRING__ *NexHString;

enum {
    NEX_SMTC_CLOSED = 0, NEX_SMTC_OPENED = 1, NEX_SMTC_CHANGING = 2,
    NEX_SMTC_STOPPED = 3, NEX_SMTC_PLAYING = 4, NEX_SMTC_PAUSED = 5
};

typedef struct {
    WCHAR app[160];      /* SourceAppUserModelId, e.g. Chrome.GEBBYCINSSH34MXZFPEPBLRQC4 */
    int   status;        /* NEX_SMTC_* */
} NexSmtcEntry;

typedef struct {
    HMODULE combase;
    HRESULT (WINAPI *ro_initialize)(int);
    HRESULT (WINAPI *ro_get_factory)(NexHString, REFIID, void **);
    HRESULT (WINAPI *create_string)(const WCHAR *, UINT32, NexHString *);
    HRESULT (WINAPI *delete_string)(NexHString);
    const WCHAR *(WINAPI *raw_buffer)(NexHString, UINT32 *);
    void *manager;       /* IGlobalSystemMediaTransportControlsSessionManager */
    char  error[160];    /* why open failed, for the log */
} NexSmtc;

static const GUID NEX_IID_SMTC_STATICS =
    {0x2050C4EE, 0x11A0, 0x57DE, {0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45}};
static const GUID NEX_IID_SMTC_MANAGER =
    {0xCACE8EAC, 0xE86E, 0x504A, {0xAB, 0x31, 0x5F, 0xF8, 0xFF, 0x1B, 0xCE, 0x49}};
static const GUID NEX_IID_SMTC_SESSION =
    {0x7148C835, 0x9B14, 0x5AE2, {0xAB, 0x85, 0xDC, 0x9B, 0x1C, 0x14, 0xE1, 0xA8}};
static const GUID NEX_IID_SMTC_PLAYBACK_INFO =
    {0x94B4B6CF, 0xE8BA, 0x51AD, {0x87, 0xA7, 0xC1, 0x0A, 0xDE, 0x10, 0x61, 0x27}};
static const GUID NEX_IID_ASYNC_INFO =
    {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* IUnknown 0-2, IInspectable 3-5; the interface's own methods start at 6. */
#define NEX_VT(o) (*(void ***)(o))
static NEX_HDR_UNUSED ULONG nex_rt_release(void *o) {
    return o ? ((ULONG (STDMETHODCALLTYPE *)(void *))NEX_VT(o)[2])(o) : 0;
}
/* QI to the interface the object must implement; the caller releases both. */
static NEX_HDR_UNUSED void *nex_rt_as(void *o, const GUID *iid) {
    void *out = NULL;
    if (!o) return NULL;
    if (FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **))NEX_VT(o)[0])(o, iid, &out)))
        return NULL;
    return out;
}
static NEX_HDR_UNUSED HRESULT nex_rt_get(void *o, int slot, void *out) {
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, void *))NEX_VT(o)[slot])(o, out);
}

static NEX_HDR_UNUSED void nex_smtc_close(NexSmtc *s) {
    if (s->manager) nex_rt_release(s->manager);
    s->manager = NULL;
}

static NEX_HDR_UNUSED int nex_smtc_open(NexSmtc *s) {
    memset(s, 0, sizeof(*s));
    s->combase = LoadLibraryW(L"combase.dll");
    if (!s->combase) { strcpy(s->error, "combase.dll not loadable"); return 0; }
    s->ro_initialize = (HRESULT (WINAPI *)(int))(void *)GetProcAddress(s->combase, "RoInitialize");
    s->ro_get_factory = (HRESULT (WINAPI *)(NexHString, REFIID, void **))(void *)
        GetProcAddress(s->combase, "RoGetActivationFactory");
    s->create_string = (HRESULT (WINAPI *)(const WCHAR *, UINT32, NexHString *))(void *)
        GetProcAddress(s->combase, "WindowsCreateString");
    s->delete_string = (HRESULT (WINAPI *)(NexHString))(void *)
        GetProcAddress(s->combase, "WindowsDeleteString");
    s->raw_buffer = (const WCHAR *(WINAPI *)(NexHString, UINT32 *))(void *)
        GetProcAddress(s->combase, "WindowsGetStringRawBuffer");
    if (!s->ro_initialize || !s->ro_get_factory || !s->create_string || !s->delete_string ||
        !s->raw_buffer) { strcpy(s->error, "WinRT entry points missing"); return 0; }
    s->ro_initialize(1);                 /* RO_INIT_MULTITHREADED; S_FALSE if already */

    static const WCHAR cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    NexHString name = NULL;
    void *statics = NULL, *op = NULL, *info = NULL, *raw = NULL;
    HRESULT hr = s->create_string(cls, (UINT32)wcslen(cls), &name);
    if (SUCCEEDED(hr)) hr = s->ro_get_factory(name, &NEX_IID_SMTC_STATICS, &statics);
    if (name) s->delete_string(name);
    if (FAILED(hr) || !statics) {
        snprintf(s->error, sizeof(s->error), "activation factory 0x%08lX", (unsigned long)hr);
        return 0;
    }
    hr = nex_rt_get(statics, 6, &op);    /* RequestAsync */
    nex_rt_release(statics);
    if (FAILED(hr) || !op) {
        snprintf(s->error, sizeof(s->error), "RequestAsync 0x%08lX", (unsigned long)hr);
        return 0;
    }
    /* Wait on IAsyncInfo::get_Status instead of a completion handler. */
    info = nex_rt_as(op, &NEX_IID_ASYNC_INFO);
    int status = 0;
    for (int i = 0; info && i < 250 && status == 0; i++) {
        if (FAILED(nex_rt_get(info, 7, &status))) break;
        if (status == 0) Sleep(20);
    }
    if (info) nex_rt_release(info);
    if (status == 1) hr = nex_rt_get(op, 8, &raw);   /* GetResults */
    nex_rt_release(op);
    if (status != 1 || FAILED(hr) || !raw) {
        snprintf(s->error, sizeof(s->error), "RequestAsync status %d hr 0x%08lX",
                 status, (unsigned long)hr);
        if (raw) nex_rt_release(raw);
        return 0;
    }
    s->manager = nex_rt_as(raw, &NEX_IID_SMTC_MANAGER);
    nex_rt_release(raw);
    if (!s->manager) { strcpy(s->error, "manager interface not as expected"); return 0; }
    return 1;
}

/* Fill up to max entries; returns how many, or -1 when the call failed. */
static NEX_HDR_UNUSED int nex_smtc_poll(NexSmtc *s, NexSmtcEntry *out, int max) {
    if (!s->manager) return -1;
    void *view = NULL;
    if (FAILED(nex_rt_get(s->manager, 7, &view)) || !view) return -1;   /* GetSessions */
    UINT32 n = 0;
    int count = 0;
    if (SUCCEEDED(nex_rt_get(view, 7, &n))) {                          /* IVectorView::get_Size */
        for (UINT32 i = 0; i < n && count < max; i++) {
            void *raw = NULL;
            if (FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, void **))NEX_VT(view)[6])
                       (view, i, &raw)) || !raw) continue;             /* GetAt */
            void *session = nex_rt_as(raw, &NEX_IID_SMTC_SESSION);
            nex_rt_release(raw);
            if (!session) continue;
            NexSmtcEntry *e = &out[count];
            memset(e, 0, sizeof(*e));
            NexHString app = NULL;
            if (SUCCEEDED(nex_rt_get(session, 6, &app)) && app) {      /* get_SourceAppUserModelId */
                UINT32 len = 0;
                const WCHAR *p = s->raw_buffer(app, &len);
                if (p) {
                    if (len >= sizeof(e->app) / sizeof(e->app[0])) len = sizeof(e->app) / sizeof(e->app[0]) - 1;
                    memcpy(e->app, p, len * sizeof(WCHAR));
                    e->app[len] = 0;
                }
                s->delete_string(app);
            }
            void *pinfo_raw = NULL;
            if (SUCCEEDED(nex_rt_get(session, 9, &pinfo_raw)) && pinfo_raw) {  /* GetPlaybackInfo */
                void *pinfo = nex_rt_as(pinfo_raw, &NEX_IID_SMTC_PLAYBACK_INFO);
                nex_rt_release(pinfo_raw);
                if (pinfo) {
                    int st = 0;
                    if (SUCCEEDED(nex_rt_get(pinfo, 7, &st))) e->status = st;   /* get_PlaybackStatus */
                    nex_rt_release(pinfo);
                }
            }
            nex_rt_release(session);
            if (e->app[0]) count++;
        }
    }
    nex_rt_release(view);
    return count;
}

#endif
