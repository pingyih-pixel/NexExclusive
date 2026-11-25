/* sessprobe - what does the session layer see while one process holds the
   default render endpoint in exclusive mode? (ROADMAP 5c; build with
   `bash mod/build.sh probe`, runs without nexsink.dll)

   sessprobe            phases A-C: another process tries shared output while
                        we are exclusive, while we are stopped but initialized
                        (a pause), and we take exclusive while it plays shared
   sessprobe replay     phase D: a process that already played (its session
                        survives) tries again while we are exclusive
   sessprobe hold N     only hold exclusive N seconds and print notifications
                        (for manual tests: play a browser video meanwhile)
   sessprobe list       print the endpoint's sessions with state and peak
                        for 3 s, without opening any stream
   child modes (spawned by the parent): shared-try, ctl-only, shared-play N,
   replay-child

   Every mode except list takes the default device exclusively for a few
   seconds and cuts off whatever else is playing there. Output is silence. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

DEFINE_GUID(CLSID_MMDevEnum_, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDevEnum_, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient_, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_IASM2_, 0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);
DEFINE_GUID(IID_IASM_, 0xBFA971F1, 0x4D5E, 0x40BB, 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4);
DEFINE_GUID(IID_IASC_, 0xF4B1A599, 0x7266, 0x4319, 0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD);
DEFINE_GUID(IID_IASC2_, 0xbfb7ff88, 0x7239, 0x4fc9, 0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d);
DEFINE_GUID(IID_IASN_, 0x641DD20B, 0x4D41, 0x49CC, 0xAB, 0xA3, 0x17, 0x4B, 0x94, 0x77, 0xBB, 0x08);
DEFINE_GUID(IID_IASE_, 0x24918ACC, 0x64B3, 0x37C1, 0x8C, 0xA9, 0x74, 0xA6, 0x6E, 0x99, 0x57, 0xA8);
DEFINE_GUID(IID_IAMI_, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);
DEFINE_GUID(SUBTYPE_PCM_, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
/* Sound control panel: "Allow applications to take exclusive control" (3)
   and "Give exclusive mode applications priority" (4). */
DEFINE_GUID(FMTID_Excl_, 0xb3f8fa53, 0x0004, 0x438e, 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc);

static ULONGLONG g_t0;
static const char *g_role = "parent";

static void say(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ULONGLONG now = GetTickCount64();
    printf("%6.3f [%-11s] %s\n", (double)(now - g_t0) / 1000.0, g_role, buf);
    fflush(stdout);
}

static const char *hrname(HRESULT hr) {
    switch ((unsigned)hr) {
    case 0: return "S_OK";
    case 0x8889000A: return "AUDCLNT_E_DEVICE_IN_USE";
    case 0x88890004: return "AUDCLNT_E_DEVICE_INVALIDATED";
    case 0x8889000E: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case 0x88890008: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
    case 0x88890019: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case 0x88890026: return "AUDCLNT_E_RESOURCES_INVALIDATED";
    case 0x88890002: return "AUDCLNT_E_NOT_STOPPED";
    case 0x88890001: return "AUDCLNT_E_NOT_INITIALIZED";
    default: return "";
    }
}

static IMMDevice *default_device(void) {
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDevEnum_, NULL, CLSCTX_ALL, &IID_IMMDevEnum_, (void **)&en)))
        return NULL;
    IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    return dev;
}

static void proc_name(DWORD pid, char *out, size_t cap) {
    snprintf(out, cap, "?");
    if (!pid) { snprintf(out, cap, "(system)"); return; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    WCHAR w[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, w, &n)) {
        const WCHAR *b = wcsrchr(w, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, out, (int)cap, NULL, NULL);
    }
    CloseHandle(h);
}

static const char *state_name(AudioSessionState s) {
    return s == AudioSessionStateActive ? "Active" : s == AudioSessionStateInactive ? "Inactive" : "Expired";
}

/* mingw only forward-declares IAudioMeterInformation; the first slot is all we need */
typedef struct Meter Meter;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Meter *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(Meter *);
    ULONG (STDMETHODCALLTYPE *Release)(Meter *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(Meter *, float *);
} MeterVtbl;
struct Meter { MeterVtbl *lpVtbl; };

static void describe_session(const char *what, IAudioSessionControl *ctl) {
    IAudioSessionControl2 *c2 = NULL;
    DWORD pid = 0;
    AudioSessionState st = AudioSessionStateExpired;
    float peak = -1;
    if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IASC2_, (void **)&c2))) {
        IAudioSessionControl2_GetProcessId(c2, &pid);
        IAudioSessionControl2_Release(c2);
    }
    IAudioSessionControl_GetState(ctl, &st);
    Meter *m = NULL;
    if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAMI_, (void **)&m))) {
        m->lpVtbl->GetPeakValue(m, &peak);
        m->lpVtbl->Release(m);
    }
    char name[260];
    proc_name(pid, name, sizeof name);
    say("%s pid=%lu %s state=%s peak=%.3f", what, (unsigned long)pid, name, state_name(st), peak);
}

static void dump_sessions(IAudioSessionManager2 *sm, const char *label) {
    IAudioSessionEnumerator *e = NULL;
    HRESULT hr = IAudioSessionManager2_GetSessionEnumerator(sm, &e);
    if (FAILED(hr)) { say("%s: enumerate failed 0x%08lx", label, (unsigned long)hr); return; }
    int n = 0;
    IAudioSessionEnumerator_GetCount(e, &n);
    say("%s: %d session(s) on the endpoint", label, n);
    for (int i = 0; i < n; i++) {
        IAudioSessionControl *c = NULL;
        if (SUCCEEDED(IAudioSessionEnumerator_GetSession(e, i, &c))) {
            describe_session("  session", c);
            IAudioSessionControl_Release(c);
        }
    }
    IAudioSessionEnumerator_Release(e);
}

/* ---- IAudioSessionNotification ---------------------------------------- */
typedef struct { IAudioSessionNotificationVtbl *lpVtbl; LONG ref; } Notif;
static HRESULT STDMETHODCALLTYPE n_qi(IAudioSessionNotification *t, REFIID r, void **o) {
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IASN_)) { *o = t; t->lpVtbl->AddRef(t); return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE n_add(IAudioSessionNotification *t) { return InterlockedIncrement(&((Notif *)t)->ref); }
static ULONG STDMETHODCALLTYPE n_rel(IAudioSessionNotification *t) { return InterlockedDecrement(&((Notif *)t)->ref); }
static HRESULT STDMETHODCALLTYPE n_created(IAudioSessionNotification *t, IAudioSessionControl *c) {
    (void)t;
    describe_session(">>> OnSessionCreated", c);
    return S_OK;
}
static IAudioSessionNotificationVtbl g_nvt = { n_qi, n_add, n_rel, n_created };
static Notif g_notif = { &g_nvt, 1 };

/* ---- IAudioSessionEvents (child: was I kicked out?) -------------------- */
typedef struct { IAudioSessionEventsVtbl *lpVtbl; LONG ref; } Events;
static HRESULT STDMETHODCALLTYPE e_qi(IAudioSessionEvents *t, REFIID r, void **o) {
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IASE_)) { *o = t; t->lpVtbl->AddRef(t); return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE e_add(IAudioSessionEvents *t) { return InterlockedIncrement(&((Events *)t)->ref); }
static ULONG STDMETHODCALLTYPE e_rel(IAudioSessionEvents *t) { return InterlockedDecrement(&((Events *)t)->ref); }
static HRESULT STDMETHODCALLTYPE e_name(IAudioSessionEvents *t, LPCWSTR a, LPCGUID b) { (void)t; (void)a; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE e_icon(IAudioSessionEvents *t, LPCWSTR a, LPCGUID b) { (void)t; (void)a; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE e_vol(IAudioSessionEvents *t, float a, BOOL b, LPCGUID c) { (void)t; (void)a; (void)b; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE e_chvol(IAudioSessionEvents *t, DWORD a, float *b, DWORD c, LPCGUID d) { (void)t; (void)a; (void)b; (void)c; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE e_group(IAudioSessionEvents *t, LPCGUID a, LPCGUID b) { (void)t; (void)a; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE e_state(IAudioSessionEvents *t, AudioSessionState s) {
    (void)t; say("OnStateChanged -> %s", state_name(s)); return S_OK;
}
static HRESULT STDMETHODCALLTYPE e_disc(IAudioSessionEvents *t, AudioSessionDisconnectReason r) {
    static const char *names[] = { "DeviceRemoval", "ServerShutdown", "FormatChanged",
                                   "SessionLogoff", "SessionDisconnected", "ExclusiveModeOverride" };
    (void)t;
    say("OnSessionDisconnected reason=%d %s", (int)r, (r >= 0 && r <= 5) ? names[r] : "");
    return S_OK;
}
static IAudioSessionEventsVtbl g_evt = { e_qi, e_add, e_rel, e_name, e_icon, e_vol, e_chvol, e_group, e_state, e_disc };
static Events g_events = { &g_evt, 1 };

/* ---- exclusive stream -------------------------------------------------- */
typedef struct {
    IAudioClient *ac;
    IAudioRenderClient *rc;
    UINT32 frames;
    int started;
} Excl;

static void fill_silence(Excl *x) {
    UINT32 pad = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(x->ac, &pad))) return;
    UINT32 n = x->frames - pad;
    if (!n) return;
    BYTE *p = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(x->rc, n, &p)))
        IAudioRenderClient_ReleaseBuffer(x->rc, n, AUDCLNT_BUFFERFLAGS_SILENT);
}

static void set_fmt(WAVEFORMATEXTENSIBLE *f, DWORD rate, WORD container, WORD valid) {
    memset(f, 0, sizeof *f);
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = 2;
    f->Format.nSamplesPerSec = rate;
    f->Format.wBitsPerSample = container;
    f->Format.nBlockAlign = (WORD)(2 * container / 8);
    f->Format.nAvgBytesPerSec = rate * f->Format.nBlockAlign;
    f->Format.cbSize = 22;
    f->Samples.wValidBitsPerSample = valid;
    f->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f->SubFormat = SUBTYPE_PCM_;
}

static HRESULT excl_open(IMMDevice *dev, Excl *x) {
    static const struct { DWORD rate; WORD c, v; } tries[] = {
        {48000, 32, 24}, {44100, 32, 24}, {48000, 24, 24}, {44100, 24, 24},
        {48000, 16, 16}, {44100, 16, 16}, {48000, 32, 32}, {44100, 32, 32},
    };
    memset(x, 0, sizeof *x);
    HRESULT hr = IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&x->ac);
    if (FAILED(hr)) return hr;
    WAVEFORMATEXTENSIBLE f;
    int chosen = -1;
    for (int i = 0; i < (int)(sizeof tries / sizeof tries[0]); i++) {
        set_fmt(&f, tries[i].rate, tries[i].c, tries[i].v);
        if (IAudioClient_IsFormatSupported(x->ac, AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, NULL) == S_OK) { chosen = i; break; }
    }
    if (chosen < 0) { IAudioClient_Release(x->ac); x->ac = NULL; return AUDCLNT_E_UNSUPPORTED_FORMAT; }
    REFERENCE_TIME def = 0, min = 0;
    IAudioClient_GetDevicePeriod(x->ac, &def, &min);
    hr = IAudioClient_Initialize(x->ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def, &f.Format, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr = 0;
        IAudioClient_GetBufferSize(x->ac, &fr);
        IAudioClient_Release(x->ac);
        IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&x->ac);
        def = (REFERENCE_TIME)(10000000.0 * fr / f.Format.nSamplesPerSec + 0.5);
        hr = IAudioClient_Initialize(x->ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def, &f.Format, NULL);
    }
    if (FAILED(hr)) { IAudioClient_Release(x->ac); x->ac = NULL; return hr; }
    say("exclusive format %lu Hz %u/%u bit, period %.1f ms", (unsigned long)tries[chosen].rate,
        tries[chosen].v, tries[chosen].c, def / 10000.0);
    IAudioClient_GetBufferSize(x->ac, &x->frames);
    IAudioClient_GetService(x->ac, &IID_IAudioRenderClient_, (void **)&x->rc);
    fill_silence(x);
    hr = IAudioClient_Start(x->ac);
    x->started = SUCCEEDED(hr);
    return hr;
}

static void excl_pump(Excl *x, DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (x->ac && x->started) fill_silence(x);
        Sleep(3);
    }
}

static void excl_close(Excl *x) {
    if (!x->ac) return;
    IAudioClient_Stop(x->ac);
    if (x->rc) IAudioRenderClient_Release(x->rc);
    IAudioClient_Release(x->ac);
    memset(x, 0, sizeof *x);
}

/* ---- child processes --------------------------------------------------- */
static HANDLE spawn(const wchar_t *args) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(cmd, MAX_PATH + 64, L"\"%ls\" %ls", exe, args);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void run_child(const wchar_t *args, DWORD wait_ms, const char *pump_note, Excl *x) {
    HANDLE h = spawn(args);
    if (!h) { say("spawn failed %lu", GetLastError()); return; }
    ULONGLONG end = GetTickCount64() + wait_ms;
    while (GetTickCount64() < end && WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        if (x && x->ac && x->started) fill_silence(x);
        Sleep(3);
    }
    (void)pump_note;
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
}

static int child_shared(int play_seconds) {
    IMMDevice *dev = default_device();
    if (!dev) { say("no device"); return 1; }
    IAudioClient *ac = NULL;
    HRESULT hr = IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { say("Activate 0x%08lx", (unsigned long)hr); return 1; }
    WAVEFORMATEX *mix = NULL;
    IAudioClient_GetMixFormat(ac, &mix);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, NULL);
    say("shared Initialize -> 0x%08lx %s", (unsigned long)hr, hrname(hr));
    if (FAILED(hr)) { IAudioClient_Release(ac); IMMDevice_Release(dev); return 0; }
    IAudioSessionControl *ctl = NULL;
    if (SUCCEEDED(IAudioClient_GetService(ac, &IID_IASC_, (void **)&ctl)))
        IAudioSessionControl_RegisterAudioSessionNotification(ctl, (IAudioSessionEvents *)&g_events);
    IAudioRenderClient *rc = NULL;
    IAudioClient_GetService(ac, &IID_IAudioRenderClient_, (void **)&rc);
    UINT32 frames = 0;
    IAudioClient_GetBufferSize(ac, &frames);
    hr = IAudioClient_Start(ac);
    say("shared Start -> 0x%08lx %s (silence for %d s)", (unsigned long)hr, hrname(hr), play_seconds);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)play_seconds * 1000;
    HRESULT last = S_OK;
    while (GetTickCount64() < end) {
        UINT32 pad = 0;
        HRESULT h2 = IAudioClient_GetCurrentPadding(ac, &pad);
        if (h2 != last) { say("GetCurrentPadding -> 0x%08lx %s", (unsigned long)h2, hrname(h2)); last = h2; }
        if (SUCCEEDED(h2) && frames > pad) {
            BYTE *p = NULL;
            if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, frames - pad, &p)))
                IAudioRenderClient_ReleaseBuffer(rc, frames - pad, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        Sleep(10);
    }
    IAudioClient_Stop(ac);
    if (ctl) {
        IAudioSessionControl_UnregisterAudioSessionNotification(ctl, (IAudioSessionEvents *)&g_events);
        IAudioSessionControl_Release(ctl);
    }
    if (rc) IAudioRenderClient_Release(rc);
    IAudioClient_Release(ac);
    CoTaskMemFree(mix);
    IMMDevice_Release(dev);
    return 0;
}

/* A long-lived app that already played once: its session survives, then it
   tries to play again while someone holds the device exclusively. */
static int child_replay(void) {
    child_shared(1);
    say("released the client; process stays alive, waiting 1.5 s");
    Sleep(1500);
    say("trying to play again");
    child_shared(1);
    Sleep(300);
    return 0;
}

static int child_ctl_only(void) {
    IMMDevice *dev = default_device();
    IAudioSessionManager *sm = NULL;
    HRESULT hr = IMMDevice_Activate(dev, &IID_IASM_, CLSCTX_ALL, NULL, (void **)&sm);
    if (FAILED(hr)) { say("IAudioSessionManager 0x%08lx", (unsigned long)hr); return 1; }
    IAudioSessionControl *ctl = NULL;
    hr = IAudioSessionManager_GetAudioSessionControl(sm, NULL, 0, &ctl);
    say("GetAudioSessionControl without Initialize -> 0x%08lx", (unsigned long)hr);
    Sleep(800);
    if (ctl) IAudioSessionControl_Release(ctl);
    IAudioSessionManager_Release(sm);
    IMMDevice_Release(dev);
    return 0;
}

/* ---- parent ------------------------------------------------------------ */
static void print_device(IMMDevice *dev) {
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        char name[512];
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof name, NULL, NULL);
        say("device: %s", name);
    }
    PropVariantClear(&v);
    for (DWORD pid = 3; pid <= 4; pid++) {
        PROPERTYKEY k = { FMTID_Excl_, pid };
        PropVariantInit(&v);
        HRESULT hr = IPropertyStore_GetValue(ps, &k, &v);
        const char *what = pid == 3 ? "allow exclusive" : "exclusive priority";
        if (FAILED(hr) || v.vt == VT_EMPTY) say("%s: (not stored, Windows default = on)", what);
        else if (v.vt == VT_UI4) say("%s: %lu", what, (unsigned long)v.ulVal);
        else if (v.vt == VT_BOOL) say("%s: %s", what, v.boolVal ? "on" : "off");
        else say("%s: vt=%u", what, v.vt);
        PropVariantClear(&v);
    }
    IPropertyStore_Release(ps);
}

int wmain(int argc, wchar_t **argv) {
    g_t0 = GetTickCount64();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (argc >= 2 && !wcscmp(argv[1], L"shared-try")) { g_role = "child-try"; return child_shared(1); }
    if (argc >= 2 && !wcscmp(argv[1], L"ctl-only")) { g_role = "child-ctl"; return child_ctl_only(); }
    if (argc >= 2 && !wcscmp(argv[1], L"replay-child")) { g_role = "child-replay"; return child_replay(); }
    if (argc >= 3 && !wcscmp(argv[1], L"shared-play")) { g_role = "child-play"; return child_shared(_wtoi(argv[2])); }

    int hold_only = argc >= 3 && !wcscmp(argv[1], L"hold");
    if (argc >= 2 && !wcscmp(argv[1], L"list")) {
        IMMDevice *d = default_device();
        print_device(d);
        IAudioSessionManager2 *m = NULL;
        IMMDevice_Activate(d, &IID_IASM2_, CLSCTX_ALL, NULL, (void **)&m);
        for (int i = 0; i < 6; i++) { dump_sessions(m, "list"); Sleep(500); }
        return 0;
    }
    IMMDevice *dev = default_device();
    if (!dev) { say("no default render device"); return 1; }
    print_device(dev);
    IAudioSessionManager2 *sm = NULL;
    HRESULT hr = IMMDevice_Activate(dev, &IID_IASM2_, CLSCTX_ALL, NULL, (void **)&sm);
    if (FAILED(hr)) { say("IAudioSessionManager2 0x%08lx", (unsigned long)hr); return 1; }
    /* Notifications start only after the first enumeration. */
    dump_sessions(sm, "before");
    hr = IAudioSessionManager2_RegisterSessionNotification(sm, (IAudioSessionNotification *)&g_notif);
    say("RegisterSessionNotification -> 0x%08lx", (unsigned long)hr);
    dump_sessions(sm, "after register");

    Excl x;
    if (argc >= 2 && !wcscmp(argv[1], L"replay")) {
        say("=== D: app that already has a session retries while we are exclusive ===");
        HANDLE h = spawn(L"replay-child");
        Sleep(1700);
        dump_sessions(sm, "D child released");
        /* listen on every existing session for state changes */
        IAudioSessionEnumerator *e = NULL;
        IAudioSessionManager2_GetSessionEnumerator(sm, &e);
        int n = 0;
        IAudioSessionEnumerator_GetCount(e, &n);
        IAudioSessionControl *keep[32] = {0};
        for (int i = 0; i < n && i < 32; i++)
            if (SUCCEEDED(IAudioSessionEnumerator_GetSession(e, i, &keep[i])))
                IAudioSessionControl_RegisterAudioSessionNotification(keep[i], (IAudioSessionEvents *)&g_events);
        hr = excl_open(dev, &x);
        say("exclusive open -> 0x%08lx %s", (unsigned long)hr, hrname(hr));
        ULONGLONG end = GetTickCount64() + 5000;
        while (GetTickCount64() < end && WaitForSingleObject(h, 0) == WAIT_TIMEOUT) { if (x.ac) fill_silence(&x); Sleep(3); }
        excl_pump(&x, 300);
        dump_sessions(sm, "D end");
        excl_close(&x);
        for (int i = 0; i < 32; i++) if (keep[i]) {
            IAudioSessionControl_UnregisterAudioSessionNotification(keep[i], (IAudioSessionEvents *)&g_events);
            IAudioSessionControl_Release(keep[i]);
        }
        IAudioSessionEnumerator_Release(e);
        CloseHandle(h);
        IAudioSessionManager2_UnregisterSessionNotification(sm, (IAudioSessionNotification *)&g_notif);
        return 0;
    }
    hr = excl_open(dev, &x);
    say("exclusive open+Start -> 0x%08lx %s", (unsigned long)hr, hrname(hr));
    if (FAILED(hr)) { say("cannot take the device exclusively; stop here"); return 2; }

    if (hold_only) {
        int secs = _wtoi(argv[2]);
        say("holding exclusive for %d s - start other audio now", secs);
        for (int i = 0; i < secs; i++) {
            excl_pump(&x, 1000);
            if (i % 5 == 4) dump_sessions(sm, "during hold");
        }
        excl_close(&x);
        dump_sessions(sm, "after release");
        IAudioSessionManager2_UnregisterSessionNotification(sm, (IAudioSessionNotification *)&g_notif);
        return 0;
    }

    say("=== A: exclusive running; another process tries shared ===");
    excl_pump(&x, 300);
    run_child(L"shared-try", 3000, NULL, &x);
    excl_pump(&x, 500);
    say("=== A2: another process asks for a session control only ===");
    run_child(L"ctl-only", 3000, NULL, &x);
    excl_pump(&x, 500);
    dump_sessions(sm, "A end");

    say("=== B: exclusive stopped but still initialized (a pause) ===");
    IAudioClient_Stop(x.ac);
    x.started = 0;
    excl_pump(&x, 300);
    run_child(L"shared-try", 3000, NULL, NULL);
    excl_pump(&x, 500);

    say("=== C: release exclusive; other process plays shared; we re-take exclusive ===");
    excl_close(&x);
    HANDLE play = spawn(L"shared-play 4");
    Sleep(1200);
    dump_sessions(sm, "C shared playing");
    hr = excl_open(dev, &x);
    say("exclusive re-open while shared plays -> 0x%08lx %s", (unsigned long)hr, hrname(hr));
    if (SUCCEEDED(hr)) excl_pump(&x, 1500);
    excl_close(&x);
    if (play) { WaitForSingleObject(play, 8000); CloseHandle(play); }
    dump_sessions(sm, "end");

    IAudioSessionManager2_UnregisterSessionNotification(sm, (IAudioSessionNotification *)&g_notif);
    IAudioSessionManager2_Release(sm);
    IMMDevice_Release(dev);
    return 0;
}
