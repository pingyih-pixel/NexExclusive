/* sess_vol.exe — 列出渲染端点上的音频会话，把「会话音量」和「端点音量」分别打出来。
 *
 * 为什么要这个工具：独占模式下这两层的命运完全不同。
 *   - 端点音量（IAudioEndpointVolume）：端点带硬件音量节点时**仍然有效**，
 *     衰减发生在 DAC 之后，不改一个比特；不带硬件音量时是软件音量，被绕过。
 *   - 会话音量（ISimpleAudioVolume）：由音频引擎施加，独占模式引擎整个被绕开，
 *     所以**无论端点能力如何都不起作用**。
 * 客户端音量滑块到底写的是哪一层，光看「有没有声音变化」是分不清的（硬件音量
 * 同样让声音变小、样本同样不变）。这里直接读两层的读数：谁在动就是谁。
 *
 *   sess_vol.exe             默认端点（eRender / eMultimedia）
 *   sess_vol.exe 耳机         名称含该串的端点
 *   sess_vol.exe --watch     每 300 ms 采样一次，只在读数变化时打印
 *
 * 会话音量拿的是 IAudioSessionManager2::GetSessionEnumerator → 每个会话
 * QueryInterface(ISimpleAudioVolume)。这条路径和客户端自己取音量的路径无关，
 * 我们只是旁观者，不会改变任何会话状态。
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
/* mingw 的 libuuid 里没有这几个 IID（它们不是导入库符号），INITGUID 让
   DEFINE_GUID 在本编译单元里直接定义出数据，链接期就有实体。 */
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

/* PKEY_Device_FriendlyName */
static const PROPERTYKEY PKEY_FriendlyName_l = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

/* 控制台里直接写 UTF-16；重定向到管道时退回 UTF-8，方便脚本读取。 */
static void put(const WCHAR *s) {
    if (!s) return;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        DWORD n = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &n, NULL);
    } else {
        char buf[1024];
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
        if (n > 0) fwrite(buf, 1, (size_t)n - 1, stdout);
    }
}

static void friendly_name(IMMDevice *dev, WCHAR *out, size_t n) {
    out[0] = 0;
    if (!dev) return;
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) || !ps) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal) {
        wcsncpy(out, pv.pwszVal, n - 1);
        out[n - 1] = 0;
    }
    PropVariantClear(&pv);
    IPropertyStore_Release(ps);
}

static int contains_ci(const WCHAR *hay, const WCHAR *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nl = wcslen(needle);
    for (const WCHAR *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && towlower(h[i]) == towlower(needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* 进程名只用来认人（哪个 pid 是客户端），拿不到就留空。 */
static void process_name(DWORD pid, WCHAR *out, size_t n) {
    out[0] = 0;
    if (!pid) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    WCHAR full[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, full, &len)) {
        const WCHAR *base = full;
        for (const WCHAR *p = full; *p; p++)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        wcsncpy(out, base, n - 1);
        out[n - 1] = 0;
    }
    CloseHandle(h);
}

/* ---- 一层：端点音量 ---- */
typedef struct { float scalar; BOOL mute; DWORD hw; int ok; } EpVol;

static EpVol read_endpoint_volume(IMMDevice *dev) {
    EpVol r = {0, 0, 0, 0};
    IAudioEndpointVolume *v = NULL;
    if (!dev) return r;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL,
                                  (void **)&v)) || !v)
        return r;
    if (SUCCEEDED(v->lpVtbl->GetMasterVolumeLevelScalar(v, &r.scalar)) &&
        SUCCEEDED(v->lpVtbl->GetMute(v, &r.mute)) &&
        SUCCEEDED(v->lpVtbl->QueryHardwareSupport(v, &r.hw)))
        r.ok = 1;
    v->lpVtbl->Release(v);
    return r;
}

/* ---- 另一层：会话音量 ---- */
typedef struct { DWORD pid; float vol; BOOL mute; AudioSessionState state; WCHAR proc[128]; } Sess;

/* 返回会话数；sess 至少能装 cap 条。系统提示音（pid 0）也列出来，但排最后无所谓。 */
static int read_sessions(IMMDevice *dev, Sess *sess, int cap) {
    if (!dev) return 0;
    IAudioSessionManager2 *sm = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL,
                                  (void **)&sm)) || !sm)
        return 0;
    int n = 0;
    IAudioSessionEnumerator *en = NULL;
    if (SUCCEEDED(sm->lpVtbl->GetSessionEnumerator(sm, &en)) && en) {
        int count = 0;
        if (SUCCEEDED(en->lpVtbl->GetCount(en, &count))) {
            for (int i = 0; i < count && n < cap; i++) {
                IAudioSessionControl *ctl = NULL;
                if (FAILED(en->lpVtbl->GetSession(en, i, &ctl)) || !ctl) continue;
                Sess s;
                memset(&s, 0, sizeof(s));
                s.vol = -1.0f;
                AudioSessionState st = AudioSessionStateExpired;
                ctl->lpVtbl->GetState(ctl, &st);
                s.state = st;

                IAudioSessionControl2 *c2 = NULL;
                if (SUCCEEDED(ctl->lpVtbl->QueryInterface(ctl, &IID_IAudioSessionControl2,
                                                          (void **)&c2)) && c2) {
                    c2->lpVtbl->GetProcessId(c2, &s.pid);
                    c2->lpVtbl->Release(c2);
                }
                ISimpleAudioVolume *sv = NULL;
                if (SUCCEEDED(ctl->lpVtbl->QueryInterface(ctl, &IID_ISimpleAudioVolume,
                                                          (void **)&sv)) && sv) {
                    sv->lpVtbl->GetMasterVolume(sv, &s.vol);
                    sv->lpVtbl->GetMute(sv, &s.mute);
                    sv->lpVtbl->Release(sv);
                }
                process_name(s.pid, s.proc, 128);
                ctl->lpVtbl->Release(ctl);
                /* 过期会话没有意义，跳过但保留计数准确 */
                if (st == AudioSessionStateExpired) continue;
                sess[n++] = s;
            }
        }
        en->lpVtbl->Release(en);
    }
    sm->lpVtbl->Release(sm);
    return n;
}

static IMMDevice *pick_endpoint(const WCHAR *want) {
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_l, (void **)&enumr)) || !enumr)
        return NULL;
    if (!want || !*want) {
        IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eMultimedia, &dev);
    } else {
        IMMDeviceCollection *col = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender,
                                                             DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0;
            IMMDeviceCollection_GetCount(col, &n);
            for (UINT i = 0; i < n && !dev; i++) {
                IMMDevice *d = NULL;
                if (FAILED(IMMDeviceCollection_Item(col, i, &d)) || !d) continue;
                WCHAR name[256];
                friendly_name(d, name, 256);
                if (contains_ci(name, want)) dev = d;   /* 引用交给调用方 */
                else IMMDevice_Release(d);
            }
            IMMDeviceCollection_Release(col);
        }
    }
    IMMDeviceEnumerator_Release(enumr);
    return dev;
}

static void snapshot(const WCHAR *want, int header) {
    IMMDevice *dev = pick_endpoint(want);
    if (!dev) { put(L"没有可用的渲染端点\n"); return; }
    WCHAR name[256];
    friendly_name(dev, name, 256);
    EpVol ev = read_endpoint_volume(dev);

    WCHAR line[512];
    if (header) {
        swprintf(line, 512, L"endpoint: %s\n", name);
        put(line);
        if (ev.ok)
            swprintf(line, 512,
                     L"  endpoint volume: %.0f%%%s   hardware support=%s%s%s\n"
                     L"  -> %s\n",
                     ev.scalar * 100.0f, ev.mute ? L" (muted)" : L"",
                     (ev.hw & 1) ? L"volume" : L"-",
                     (ev.hw & 2) ? L" mute" : L"",
                     (ev.hw & 4) ? L" meter" : L"",
                     (ev.hw & 1) ? L"独占下这一层仍然有效（衰减在 DAC 之后）"
                                 : L"独占下这一层是软件音量，被绕过");
        else
            swprintf(line, 512, L"  endpoint volume: unavailable\n");
        put(line);
    }

    Sess sess[32];
    int n = read_sessions(dev, sess, 32);
    if (header) {
        swprintf(line, 512, L"  sessions: %d\n", n);
        put(line);
    }
    for (int i = 0; i < n; i++) {
        const WCHAR *st = sess[i].state == AudioSessionStateActive ? L"active"
                        : sess[i].state == AudioSessionStateInactive ? L"inactive" : L"?";
        WCHAR vol[32];
        if (sess[i].vol < 0) wcscpy(vol, L"  n/a");
        else swprintf(vol, 32, L"%3.0f%%", sess[i].vol * 100.0f);
        if (header)
            swprintf(line, 512, L"    [%d] pid=%-7lu %-24s %-8s session vol %s%s\n",
                     i, (unsigned long)sess[i].pid,
                     sess[i].proc[0] ? sess[i].proc : L"(unknown)", st, vol,
                     sess[i].mute ? L" (muted)" : L"");
        else
            swprintf(line, 512, L"  %-24s session vol %s%s   endpoint %3.0f%%\n",
                     sess[i].proc[0] ? sess[i].proc : L"(unknown)", vol,
                     sess[i].mute ? L" (muted)" : L"", ev.scalar * 100.0f);
        put(line);
    }
    IMMDevice_Release(dev);
}

int main(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int watch = 0;
    const WCHAR *want = NULL;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--watch") == 0) watch = 1;
        else want = argv[i];
    }
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        put(L"CoInitializeEx 失败\n");
        return 1;
    }
    if (!watch) {
        snapshot(want, 1);
    } else {
        /* 只在读数变化时打印一行，滚音量条时屏幕不会刷满。 */
        WCHAR last[512] = L"";
        DWORD t0 = GetTickCount();
        put(L"--watch: 每 300 ms 采样，读数变化才打印（Ctrl+C 结束）\n");
        for (;;) {
            IMMDevice *dev = pick_endpoint(want);
            if (dev) {
                EpVol ev = read_endpoint_volume(dev);
                Sess sess[32];
                int n = read_sessions(dev, sess, 32);
                WCHAR line[512];
                /* 只挑「音量不为 100% 或静音」的会话，噪音少一些 */
                WCHAR tail[256] = L"";
                for (int i = 0; i < n && i < 4; i++) {
                    if (sess[i].vol < 0) continue;
                    if (sess[i].vol > 0.995f && !sess[i].mute) continue;
                    WCHAR one[128];
                    swprintf(one, 128, L"  %s=%3.0f%%%s",
                             sess[i].proc[0] ? sess[i].proc : L"?",
                             sess[i].vol * 100.0f, sess[i].mute ? L"(mute)" : L"");
                    if (wcslen(tail) + wcslen(one) < 240) wcscat(tail, one);
                }
                swprintf(line, 512, L"%6lu ms  endpoint %.0f%%%s   session%s\n",
                         (unsigned long)(GetTickCount() - t0),
                         ev.scalar * 100.0f, ev.mute ? L"(mute)" : L"",
                         tail[0] ? tail : L" (all 100%)");
                if (wcscmp(line, last) != 0) {
                    put(line);
                    wcsncpy(last, line, 511);
                    last[511] = 0;
                }
                IMMDevice_Release(dev);
            }
            Sleep(300);
        }
    }
    LocalFree(argv);
    CoUninitialize();
    return 0;
}
