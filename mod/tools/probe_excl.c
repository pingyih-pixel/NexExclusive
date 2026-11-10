/* probe_excl.exe — 独立于客户端进程的独占格式探测器。
 *
 * 目的：把 "18 candidates rejected" 的责任切开。
 *   - 如果本工具（不加载任何 hook）也全拒，说明是端点/驱动/其他进程占用；
 *   - 如果本工具能成功，说明是 nexsink 的 hook 路径出了问题。
 *
 * 输出：默认渲染端点的 mix format、每个候选格式的 IsFormatSupported 与
 *       Initialize 的 HRESULT、以及当前占用该端点的音频会话。
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
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

int main(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator *enumr = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator_l, (void **)&enumr);
    if (FAILED(hr) || !enumr) { printf("enumerator 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDevice *dev = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) { printf("no default endpoint 0x%08lX\n", (unsigned long)hr); return 1; }

    LPWSTR id = NULL;
    if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
        printf("endpoint: "); wstr(id); printf("\n");
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
    static const int depths[][2] = {{16, 0}, {32, 1}, {24, 0}};
    int ok = 0, total = 0;
    printf("\n%-8s %-8s %-28s %-28s\n", "rate", "depth", "IsFormatSupported(EXCL)", "Initialize(EXCL)");
    for (int r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); r++) {
        for (int d = 0; d < 3; d++) {
            WAVEFORMATEXTENSIBLE we;
            build(&we, rates[r], depths[d][0], depths[d][1], 2);
            total++;

            IAudioClient *c = NULL;
            hr = IMMDevice_Activate(dev, &IID_IAudioClient_l, CLSCTX_ALL, NULL, (void **)&c);
            if (FAILED(hr) || !c) { printf("%-8d activate failed\n", rates[r]); continue; }

            WAVEFORMATEX *closest = NULL;
            HRESULT hf = IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                        (WAVEFORMATEX *)&we, &closest);
            if (closest) CoTaskMemFree(closest);

            char fs[64], in[64];
            snprintf(fs, sizeof(fs), "0x%08lX %s", (unsigned long)hf, hrname(hf));

            HRESULT hi = 0xDEADBEEF;
            if (hf == S_OK) {
                /* 100ms，default period，与 nexsink 完全一致 */
                hi = IAudioClient_Initialize(c, AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
                                             1000000, 0, (WAVEFORMATEX *)&we, NULL);
                if (SUCCEEDED(hi)) {
                    UINT32 frames = 0;
                    if (SUCCEEDED(IAudioClient_GetBufferSize(c, &frames)) && frames) ok++;
                    IAudioClient_Release(c);
                    snprintf(in, sizeof(in), "0x%08lX %s", (unsigned long)hi, hrname(hi));
                    printf("%-8d %d/%s%-4s %-28s %s (frames=%u)\n", rates[r],
                           depths[d][0], depths[d][1] ? "f" : " ", "", fs, in, frames);
                    continue;
                }
            } else {
                snprintf(in, sizeof(in), "skipped");
            }
            IAudioClient_Release(c);
            printf("%-8d %d/%s%-4s %-28s %s\n", rates[r],
                   depths[d][0], depths[d][1] ? "f" : " ", "", fs, in);
        }
    }
    printf("\nresult: %d/%d exclusive formats accepted\n", ok, total);

    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(enumr);
    CoUninitialize();
    return ok > 0 ? 0 : 2;
}
