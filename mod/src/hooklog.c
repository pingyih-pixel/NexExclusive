// Observe-first WASAPI vtable hook.
//
// Finds the IAudioClient / IAudioClient3 vtables inside the host process by
// creating one instance of each, then replaces two slots:
//
//   slot 3  Initialize      log share mode + requested format
//   slot 8  GetMixFormat    log the format handed to the app
//
// By default it only writes a log. Set NEX_FORCE="<rate>:<bits>" and it will
// additionally hand back a substituted mix format and promote a shared-mode
// Initialize to exclusive. That second part is the experiment, not the default.
//
// Log path comes from NEX_LOG, defaulting to hook.log beside this DLL.

#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define SLOT_INITIALIZE 3
#define SLOT_GETMIXFORMAT 8
#define SLOT_GETSERVICE 14
#define RSLOT_GETBUFFER 3
#define RSLOT_RELEASEBUFFER 4
#define MAX_VTABLES 4

typedef struct {
    void **vtbl;
    void *orig_init;
    void *orig_mix;
} VtblEntry;

static VtblEntry g_entries[MAX_VTABLES];
static int g_count = 0;
static CRITICAL_SECTION g_cs;
static int g_force_rate = 0;
static int g_force_bits = 0;
static int g_force_sub = 0;   /* 0 = pcm, 1 = float */
static int g_exclusive = 0;   /* promote shared -> exclusive */
static FILETIME g_cfg_mtime;
static int g_cfg_loaded = 0;

static const GUID IID_IAudioRenderClient_local =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

static void *g_render_vtbl = NULL;
static void *g_r_orig_get = NULL;
static void *g_r_orig_release = NULL;
static volatile LONG g_frames_total = 0;
static volatile LONG g_release_calls = 0;
static BYTE *g_last_ptr = NULL;
static UINT32 g_last_frames = 0;

static const GUID IID_IAudioClient3_local =
    {0x7ED4EE07, 0x8E67, 0x4CD4, {0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42}};

// 默认的日志与配置位置取「本 DLL 所在目录」，不再写死开发机上的绝对路径。
// 注入到别的进程后 GetModuleFileNameW 依旧返回 DLL 的完整路径，不受目标进程
// 工作目录影响；换盘符、换机器、换用户名都不必改代码。
// __ImageBase 是本 PE 的映像基址，由链接器提供，比把函数指针强转成 LPCWSTR
// 干净（后者在 -Wall 下会报指针类型不匹配）。
extern IMAGE_DOS_HEADER __ImageBase;

static void module_dir(char *out, size_t n) {
    out[0] = 0;
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&__ImageBase, &self))
        return;
    WCHAR wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    WCHAR *slash = wcsrchr(wpath, L'\\');
    if (!slash) return;
    slash[1] = L'\0';
    if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, (int)n, NULL, NULL) <= 0)
        out[0] = 0;
}

static void log_line(const char *fmt, ...) {
    char path[MAX_PATH];
    char buf[1024];
    va_list ap;
    const char *env = getenv("NEX_LOG");
    if (env && *env) {
        strncpy(path, env, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    } else {
        module_dir(path, sizeof(path));
        strncat(path, "hook.log", sizeof(path) - strlen(path) - 1);
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    FILE *f = fopen(path, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
    OutputDebugStringA(buf);
}

static VtblEntry *entry_for(void *self) {
    void **vt = *(void ***)self;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].vtbl == vt) return &g_entries[i];
    }
    return NULL;
}

static void describe(const char *tag, const WAVEFORMATEX *w, char *out, size_t n) {
    if (!w) { snprintf(out, n, "%s=(null)", tag); return; }
    unsigned valid = w->wBitsPerSample;
    unsigned sub = 0;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)w;
        valid = we->Samples.wValidBitsPerSample;
        sub = we->SubFormat.Data1;
    }
    snprintf(out, n, "%s=rate:%lu/container:%u/valid:%u/ch:%u/tag:0x%04X/sub:0x%08X",
             tag, (unsigned long)w->nSamplesPerSec, w->wBitsPerSample, valid,
             w->nChannels, w->wFormatTag, sub);
}

static void config_path(char *out, size_t n) {
    const char *env = getenv("NEX_CFG");
    if (env && *env) { snprintf(out, n, "%s", env); return; }
    module_dir(out, n);
    strncat(out, "hook.cfg", n - strlen(out) - 1);
}

// Re-read hook.cfg when its timestamp changes, so behaviour can be toggled
// without restarting the host process.
static void load_config(void) {
    char path[MAX_PATH];
    config_path(path, sizeof(path));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (g_cfg_loaded &&
        CompareFileTime(&fad.ftLastWriteTime, &g_cfg_mtime) == 0) return;
    g_cfg_mtime = fad.ftLastWriteTime;
    g_cfg_loaded = 1;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "force=", 6) == 0) {
            char *v = p + 6;
            if (_strnicmp(v, "off", 3) == 0) {
                g_force_rate = 0; g_force_bits = 0;
            } else {
                int r = 0, b = 0;
                if (sscanf(v, "%d:%d", &r, &b) == 2) { g_force_rate = r; g_force_bits = b; }
            }
        } else if (_strnicmp(p, "sub=", 4) == 0) {
            char *v = p + 4;
            g_force_sub = (_strnicmp(v, "float", 5) == 0) ? 1 : 0;
        } else if (_strnicmp(p, "exclusive=", 10) == 0) {
            g_exclusive = (atoi(p + 10) != 0);
        }
    }
    fclose(f);
    log_line("config: force=%d:%d sub=%s exclusive=%d", g_force_rate,
             g_force_bits, g_force_sub ? "float" : "pcm", g_exclusive);
}

static HRESULT STDMETHODCALLTYPE Hook_GetMixFormat(void *self, WAVEFORMATEX **pp) {
    load_config();
    VtblEntry *e = entry_for(self);
    if (!e || !e->orig_mix) return E_FAIL;
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, WAVEFORMATEX **))e->orig_mix)(self, pp);
    char d[256];
    describe("fmt", *pp, d, sizeof(d));
    log_line("GetMixFormat hr=0x%08lX %s", (unsigned long)hr, d);

    if (g_force_rate && pp && *pp) {
        int ch = (*pp)->nChannels;
        int bits = g_force_bits;
        WAVEFORMATEXTENSIBLE *we = (WAVEFORMATEXTENSIBLE *)CoTaskMemAlloc(sizeof(*we));
        if (we) {
            memset(we, 0, sizeof(*we));
            we->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            we->Format.nChannels = (WORD)ch;
            we->Format.nSamplesPerSec = g_force_rate;
            we->Format.wBitsPerSample = (WORD)bits;
            we->Format.nBlockAlign = (WORD)(ch * bits / 8);
            we->Format.nAvgBytesPerSec = g_force_rate * we->Format.nBlockAlign;
            we->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            we->Samples.wValidBitsPerSample = (WORD)bits;
            we->dwChannelMask = ch == 2 ? 3 : 0x3F;
            we->SubFormat.Data1 = g_force_sub ? 0x00000003 : 0x00000001;
            we->SubFormat.Data2 = 0x0000;
            we->SubFormat.Data3 = 0x0010;
            we->SubFormat.Data4[0] = 0x80;
            we->SubFormat.Data4[7] = 0xAA;
            we->SubFormat.Data4[6] = 0x00;
            we->SubFormat.Data4[5] = 0x38;
            we->SubFormat.Data4[4] = 0x9B;
            we->SubFormat.Data4[3] = 0x71;
            we->SubFormat.Data4[2] = 0x00;
            we->SubFormat.Data4[1] = 0x00;
            CoTaskMemFree(*pp);
            *pp = (WAVEFORMATEX *)we;
            char d2[256];
            describe("forced", *pp, d2, sizeof(d2));
            log_line("GetMixFormat -> substituted %s", d2);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetBuffer(void *self, UINT32 want, BYTE **pp) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, BYTE **))g_r_orig_get)(self, want, pp);
    if (SUCCEEDED(hr) && pp) {
        g_last_ptr = *pp;
        g_last_frames = want;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_ReleaseBuffer(void *self, UINT32 written, DWORD flags) {
    LONG calls = InterlockedIncrement(&g_release_calls);
    InterlockedAdd(&g_frames_total, (LONG)written);
    if (calls % 200 == 0)
        log_line("render: calls=%ld frames=%ld last=%lu ptr=%p",
                 (long)calls, (long)g_frames_total, (unsigned long)written,
                 (void *)g_last_ptr);
    return ((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, DWORD))g_r_orig_release)(self, written, flags);
}

static void patch_render(void *render) {
    if (!render) return;
    void **vt = *(void ***)render;
    if (g_render_vtbl == vt) return;
    DWORD old = 0;
    if (!VirtualProtect(vt, 8 * sizeof(void *), PAGE_READWRITE, &old)) return;
    g_r_orig_get = vt[RSLOT_GETBUFFER];
    g_r_orig_release = vt[RSLOT_RELEASEBUFFER];
    vt[RSLOT_GETBUFFER] = (void *)Hook_GetBuffer;
    vt[RSLOT_RELEASEBUFFER] = (void *)Hook_ReleaseBuffer;
    VirtualProtect(vt, 8 * sizeof(void *), old, &old);
    g_render_vtbl = vt;
    log_line("patched IAudioRenderClient vtable=%p get=%p release=%p",
             (void *)vt, g_r_orig_get, g_r_orig_release);
}

static HRESULT STDMETHODCALLTYPE Hook_GetService(void *self, const GUID *iid, void **out) {
    VtblEntry *e = entry_for(self);
    if (!e) return E_FAIL;
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, void **))e->vtbl[SLOT_GETSERVICE])(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && iid && iid->Data1 == 0xF294ACFC)
        patch_render(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Initialize(void *self, AUDCLNT_SHAREMODE mode,
                                                 DWORD flags, REFERENCE_TIME dur,
                                                 REFERENCE_TIME period,
                                                 const WAVEFORMATEX *fmt,
                                                 LPCGUID session) {
    load_config();
    VtblEntry *e = entry_for(self);
    if (!e || !e->orig_init) return E_FAIL;
    char d[256];
    describe("fmt", fmt, d, sizeof(d));
    log_line("Initialize mode=%d flags=0x%08lX dur=%lld period=%lld %s",
             (int)mode, (unsigned long)flags, (long long)dur, (long long)period, d);

    if (g_exclusive && mode == AUDCLNT_SHAREMODE_SHARED) {
        mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
        log_line("Initialize -> promoted to EXCLUSIVE");
    }

    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, AUDCLNT_SHAREMODE, DWORD,
                                                REFERENCE_TIME, REFERENCE_TIME,
                                                const WAVEFORMATEX *, LPCGUID))e->orig_init)(
        self, mode, flags, dur, period, fmt, session);
    log_line("Initialize returned 0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        UINT32 frames = 0;
        HRESULT hb = ((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32 *))e->vtbl[4])(self, &frames);
        if (SUCCEEDED(hb))
            log_line("  buffer frames=%lu (%.1f ms at %lu Hz)", (unsigned long)frames,
                     frames * 1000.0 / (fmt ? fmt->nSamplesPerSec : 1),
                     fmt ? (unsigned long)fmt->nSamplesPerSec : 0UL);
    }
    return hr;
}

static int patch_vtable(void *iface, const char *label) {
    if (!iface) return 0;
    void **vt = *(void ***)iface;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].vtbl == vt) {
            log_line("vtable for %s already patched", label);
            return 1;
        }
    }
    if (g_count >= MAX_VTABLES) return 0;

    VtblEntry *e = &g_entries[g_count];
    e->vtbl = vt;
    e->orig_init = vt[SLOT_INITIALIZE];
    e->orig_mix = vt[SLOT_GETMIXFORMAT];

    DWORD old = 0;
    if (!VirtualProtect(vt, 16 * sizeof(void *), PAGE_READWRITE, &old)) {
        log_line("VirtualProtect failed %lu for %s", GetLastError(), label);
        return 0;
    }
    vt[SLOT_INITIALIZE] = (void *)Hook_Initialize;
    vt[SLOT_GETMIXFORMAT] = (void *)Hook_GetMixFormat;
    vt[SLOT_GETSERVICE] = (void *)Hook_GetService;
    VirtualProtect(vt, 16 * sizeof(void *), old, &old);

    g_count++;
    log_line("patched %s vtable=%p init=%p mix=%p", label, (void *)vt,
             e->orig_init, e->orig_mix);
    return 1;
}

static DWORD WINAPI setup_thread(LPVOID param) {
    (void)param;
    const char *force = getenv("NEX_FORCE");
    if (force && *force) {
        int r = 0, b = 0;
        if (sscanf(force, "%d:%d", &r, &b) == 2) { g_force_rate = r; g_force_bits = b; }
    }
    log_line("=== hook attached, force=%d:%d, audioses=%p ===",
             g_force_rate, g_force_bits, (void *)GetModuleHandleA("AUDIOSES.DLL"));

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enumr = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { log_line("enumerator failed 0x%08lX", (unsigned long)hr); return 0; }

    IMMDevice *dev = NULL;
    hr = enumr->lpVtbl->GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) {
        IAudioClient *c1 = NULL;
        if (SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL,
                                            NULL, (void **)&c1)) && c1) {
            patch_vtable(c1, "IAudioClient");
            c1->lpVtbl->Release(c1);
        }
        IAudioClient *c3 = NULL;
        if (SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_IAudioClient3_local, CLSCTX_ALL,
                                            NULL, (void **)&c3)) && c3) {
            patch_vtable(c3, "IAudioClient3");
            c3->lpVtbl->Release(c3);
        }
        // The render-client vtable is only reachable through GetService. Create a
        // throwaway stream to grab it once; patching is retroactive for every
        // existing render client because they share the vtable.
        IAudioClient *probe = NULL;
        if (SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL,
                                            NULL, (void **)&probe)) && probe) {
            WAVEFORMATEX *mix = NULL;
            if (SUCCEEDED(probe->lpVtbl->GetMixFormat(probe, &mix)) && mix) {
                HRESULT hi = probe->lpVtbl->Initialize(probe, AUDCLNT_SHAREMODE_SHARED,
                                                       0, 10000000, 0, mix, NULL);
                if (SUCCEEDED(hi)) {
                    void *rc = NULL;
                    if (SUCCEEDED(probe->lpVtbl->GetService(
                            probe, &IID_IAudioRenderClient_local, &rc)) && rc) {
                        patch_render(rc);
                        ((IUnknown *)rc)->lpVtbl->Release(rc);
                    }
                    probe->lpVtbl->Reset(probe);
                } else {
                    log_line("probe Initialize failed 0x%08lX", (unsigned long)hi);
                }
                CoTaskMemFree(mix);
            }
            probe->lpVtbl->Release(probe);
        }
        dev->lpVtbl->Release(dev);
    } else {
        log_line("endpoint failed 0x%08lX", (unsigned long)hr);
    }
    enumr->lpVtbl->Release(enumr);
    log_line("=== setup done, %d vtable(s) patched ===", g_count);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(NULL, 0, setup_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
