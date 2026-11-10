/* probe_excl.exe — 独立于客户端进程的独占格式探测器。
 *
 * 目的：把 "18 candidates rejected" 的责任切开。
 *   - 如果本工具（不加载任何 hook）也全拒，说明是端点/驱动/其他进程占用；
 *   - 如果本工具能成功，说明是 nexsink 的 hook 路径出了问题。
 *
 * 同时回答「端点支持 96，为什么 88.2 也被拒」：对每个格式同时问
 *   IsFormatSupported(EXCLUSIVE)  硬件直通认不认
 *   IsFormatSupported(SHARED)     引擎能不能靠重采样接住
 * 同一个 88.2 若 SHARED 通过而 EXCLUSIVE 拒绝，就说明这不是我们的问题，
 * 也不是「96 比 88.2 大」能推出来的——共享模式允许 SRC，独占模式不允许。
 *
 * 输出：默认渲染端点的 mix format、每个候选格式的两种 IsFormatSupported、
 *       可选的 Initialize HRESULT、以及当前占用该端点的音频会话。
 *
 * 用法：
 *   probe_excl.exe            只问 IsFormatSupported（不占用端点）
 *   probe_excl.exe --init     额外尝试 Initialize，会短暂独占端点
 *   probe_excl.exe <名称片段>  指定端点，默认是系统默认渲染端点
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <stdio.h>
#include <string.h>

static const GUID SUB_PCM_L =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID SUB_FLOAT_L =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* mingw 的 libuuid 不带这些符号，按 nexsink.c 的做法本地定义 */
static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_IAudioClient_l =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_IAudioSessionManager2_l =
    {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x49, 0x9B, 0x9F, 0x14}};
static const GUID IID_IAudioSessionControl2_l =
    {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA3, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6F}};

static void build(WAVEFORMATEXTENSIBLE *w, int rate, int bits, int is_float, int ch) {
    memset(w, 0, sizeof(*w));
    w->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels       = (WORD)ch;
    w->Format.nSamplesPerSec  = (DWORD)rate;
    w->Format.wBitsPerSample  = (WORD)bits;
    w->Format.nBlockAlign     = (WORD)(ch * bits / 8);
    w->Format.nAvgBytesPerSec = (DWORD)rate * w->Format.nBlockAlign;
    w->Format.cbSize          = 22;
    w->Samples.wValidBitsPerSample = (WORD)bits;
    w->dwChannelMask          = ch == 1 ? 0x4 : (ch == 2 ? 0x3 : 0x3F);
    w->SubFormat              = is_float ? SUB_FLOAT_L : SUB_PCM_L;
}

/* 让 HRESULT 既可读又能直接和 nexsink 的日志对照 */
static const char *hrname(HRESULT hr) {
    switch ((unsigned long)hr) {
    case 0x00000000: return "S_OK";
    case 0x00000001: return "S_FALSE";   /* SHARED 下表示「不原生支持，但能转」 */
    case 0x88890001: return "AUDCLNT_E_NOT_INITIALIZED";
    case 0x88890002: return "AUDCLNT_E_ALREADY_INITIALIZED";
    case 0x88890003: return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
    case 0x88890004: return "AUDCLNT_E_DEVICE_INVALIDATED";
    case 0x88890005: return "AUDCLNT_E_NOT_STOPPED";
    case 0x88890006: return "AUDCLNT_E_BUFFER_TOO_LARGE";
    case 0x88890007: return "AUDCLNT_E_OUT_OF_ORDER";
    case 0x88890008: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
    case 0x88890009: return "AUDCLNT_E_INVALID_SIZE";
    case 0x8889000A: return "AUDCLNT_E_DEVICE_IN_USE";
    case 0x8889000B: return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
    case 0x8889000C: return "AUDCLNT_E_THREAD_NOT_REGISTERED";
    case 0x8889000E: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case 0x8889000F: return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
    case 0x88890010: return "AUDCLNT_E_SERVICE_NOT_RUNNING";
    case 0x88890011: return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED";
    case 0x88890012: return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY";
    case 0x88890013: return "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL";
    case 0x88890014: return "AUDCLNT_E_EVENTHANDLE_NOT_SET";
    case 0x88890015: return "AUDCLNT_E_INCORRECT_BUFFER_SIZE";
    case 0x88890016: return "AUDCLNT_E_BUFFER_SIZE_ERROR";
    case 0x88890017: return "AUDCLNT_E_CPUUSAGE_EXCEEDED";
    case 0x88890018: return "AUDCLNT_E_BUFFER_ERROR";
    case 0x88890019: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case 0x88890020: return "AUDCLNT_E_INVALID_DEVICE_PERIOD";
    case 0x80070005: return "E_ACCESSDENIED";
    case 0x80004005: return "E_FAIL";
    case 0x8007000E: return "E_OUTOFMEMORY";
    case 0x80070057: return "E_INVALIDARG";
    case 0x8007001F: return "ERROR_GEN_FAILURE";
    default:         return "?";
    }
}

static void wstr(const WCHAR *s) {
    if (!s) return;
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0) fputs(buf, stdout);
}

/* 谁正持有这个端点：把会话状态打出来，DEVICE_IN_USE 时才看得懂原因 */
static void dump_sessions(IMMDevice *dev) {
    IAudioSessionManager2 *mgr = NULL;
    IAudioSessionEnumerator *list = NULL;
    HRESULT hr = IMMDevice_Activate(dev, &IID_IAudioSessionManager2_l, CLSCTX_ALL,
                                    NULL, (void **)&mgr);
    if (FAILED(hr) || !mgr) { printf("  sessions: unavailable (0x%08lX)\n", (unsigned long)hr); return; }
    if (FAILED(IAudioSessionManager2_GetSessionEnumerator(mgr, &list)) || !list) {
        IAudioSessionManager2_Release(mgr);
        printf("  sessions: enumerate failed\n");
        return;
    }
    int n = 0;
    IAudioSessionEnumerator_GetCount(list, &n);
    printf("  sessions: %d\n", n);
    for (int i = 0; i < n; i++) {
        IAudioSessionControl *ctl = NULL;
        if (FAILED(IAudioSessionEnumerator_GetSession(list, i, &ctl)) || !ctl) continue;
        IAudioSessionControl2 *c2 = NULL;
        AudioSessionState st = AudioSessionStateInactive;
        IAudioSessionControl_GetState(ctl, &st);
        DWORD pid = 0;
        WCHAR *name = NULL;
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAudioSessionControl2_l, (void **)&c2)) && c2) {
            IAudioSessionControl2_GetProcessId(c2, &pid);
            IAudioSessionControl2_GetSessionIdentifier(c2, &name);
        }
        printf("    [%d] state=%d pid=%lu%s\n", i, (int)st, (unsigned long)pid,
               st == AudioSessionStateActive ? "  <-- ACTIVE" : "");
        if (name) CoTaskMemFree(name);
        if (c2) IAudioSessionControl2_Release(c2);
        IAudioSessionControl_Release(ctl);
    }
    IAudioSessionEnumerator_Release(list);
    IAudioSessionManager2_Release(mgr);
}

/* 端点友好名。释放 PROPVARIANT 手写，省掉 -lpropsys（IPropertyStore 走
   COBJMACROS 展开，不需要 IID 常量）。 */
static const PROPERTYKEY PKEY_FriendlyName_l = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

static void friendly_name(IMMDevice *dev, char *out, size_t n) {
    out[0] = 0;
    IPropertyStore *store = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &store)) || !store) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_FriendlyName_l, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal) {
        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, out, (int)n, NULL, NULL);
        CoTaskMemFree(pv.pwszVal);
    }
    IPropertyStore_Release(store);
}

static IMMDevice *find_device(IMMDeviceEnumerator *enumr, const char *needle) {
    IMMDeviceCollection *coll = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender,
                                                      DEVICE_STATE_ACTIVE, &coll)) || !coll)
        return NULL;
    UINT n = 0;
    IMMDeviceCollection_GetCount(coll, &n);
    IMMDevice *found = NULL;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)) || !dev) continue;
        char name[512];
        friendly_name(dev, name, sizeof(name));
        if (strstr(name, needle)) found = dev;
        else IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(coll);
    return found;
}

/* 把 closest 也打出来：SHARED 返回 S_FALSE 时它告诉我们是哪个格式顶替的，
   这正是「引擎做了 SRC」的书面证据。 */
static void fmt_brief(const WAVEFORMATEX *w, char *out, size_t n) {
    if (!w) { snprintf(out, n, "-"); return; }
    snprintf(out, n, "%lu/%u", (unsigned long)w->nSamplesPerSec, w->wBitsPerSample);
}

int main(int argc, char **argv) {
    int do_init = 0;
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--init") == 0) do_init = 1;
        else filter = argv[i];
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator *enumr = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator_l, (void **)&enumr);
    if (FAILED(hr) || !enumr) { printf("enumerator 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDevice *dev = NULL;
    if (filter) {
        dev = find_device(enumr, filter);
        if (!dev) {
            printf("没有名字含「%s」的渲染端点\n", filter);
            IMMDeviceEnumerator_Release(enumr);
            CoUninitialize();
            return 1;
        }
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
        if (FAILED(hr) || !dev) { printf("no default endpoint 0x%08lX\n", (unsigned long)hr); return 1; }
    }

    char epname[512];
    friendly_name(dev, epname, sizeof(epname));
    printf("endpoint: %s%s\n", epname[0] ? epname : "(unnamed)",
           filter ? "" : "  (system default)");

    LPWSTR id = NULL;
    if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
        printf("  id: "); wstr(id); printf("\n");
        CoTaskMemFree(id);
    }

    IAudioClient *ac = NULL;
    hr = IMMDevice_Activate(dev, &IID_IAudioClient_l, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr) || !ac) { printf("activate 0x%08lX\n", (unsigned long)hr); return 1; }

    WAVEFORMATEX *mix = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(ac, &mix)) && mix) {
        printf("mix: %lu Hz / %u bit / %u ch / tag 0x%04X\n",
               (unsigned long)mix->nSamplesPerSec, mix->wBitsPerSample,
               mix->nChannels, mix->wFormatTag);
        CoTaskMemFree(mix);
    }
    IAudioClient_Release(ac);

    dump_sessions(dev);

    static const int rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    static const int depths[][2] = {{16, 0}, {24, 0}, {32, 1}};
    int ok = 0, total = 0;
    printf("\n%-7s %-6s %-24s %-24s %s\n", "rate", "depth",
           "EXCLUSIVE", "SHARED", do_init ? "Initialize(EXCL)" : "");
    for (int r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); r++) {
        for (int d = 0; d < 3; d++) {
            WAVEFORMATEXTENSIBLE we;
            build(&we, rates[r], depths[d][0], depths[d][1], 2);
            total++;

            IAudioClient *c = NULL;
            hr = IMMDevice_Activate(dev, &IID_IAudioClient_l, CLSCTX_ALL, NULL, (void **)&c);
            if (FAILED(hr) || !c) {
                printf("%-7d activate failed 0x%08lX\n", rates[r], (unsigned long)hr);
                continue;
            }

            /* 同一个格式问两遍：硬件直通认不认，引擎能不能靠重采样接住。 */
            WAVEFORMATEX *closest_ex = NULL, *closest_sh = NULL;
            HRESULT hf = IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                        (WAVEFORMATEX *)&we, &closest_ex);
            HRESULT hs = IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_SHARED,
                                                        (WAVEFORMATEX *)&we, &closest_sh);

            char ex[64], sh[96], sub[32];
            snprintf(ex, sizeof(ex), "0x%08lX %s", (unsigned long)hf, hrname(hf));
            fmt_brief(closest_sh, sub, sizeof(sub));
            snprintf(sh, sizeof(sh), "0x%08lX %s -> %s", (unsigned long)hs, hrname(hs), sub);
            if (closest_ex) CoTaskMemFree(closest_ex);
            if (closest_sh) CoTaskMemFree(closest_sh);

            char tail[80] = "";
            if (do_init && hf == S_OK) {
                /* 100 ms、default period，与 nexsink 完全一致 */
                HRESULT hi = IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
                                                     1000000, 0, (WAVEFORMATEX *)&we, NULL);
                UINT32 frames = 0;
                if (SUCCEEDED(hi)) {
                    IAudioClient_GetBufferSize(c, &frames);
                    if (frames) ok++;
                }
                snprintf(tail, sizeof(tail), "0x%08lX %s frames=%u",
                         (unsigned long)hi, hrname(hi), frames);
            } else if (!do_init && hf == S_OK) {
                ok++;   /* 只问格式时，S_OK 就算通过 */
            }

            printf("%-7d %2d/%-3s %-24s %-24s %s\n", rates[r],
                   depths[d][0], depths[d][1] ? "f" : "", ex, sh, tail);
            IAudioClient_Release(c);
        }
    }
    printf("\nresult: %d/%d formats accepted by EXCLUSIVE%s\n", ok, total,
           do_init ? " (Initialize verified)" : " (IsFormatSupported only)");

    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(enumr);
    CoUninitialize();
    return ok > 0 ? 0 : 2;
}
