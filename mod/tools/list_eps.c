/* list_eps.exe — 枚举所有 WASAPI 渲染端点。
 *
 * 用途：确认耳机/DAC 是否已被系统识别为可用的播放端点，以及它支持哪些
 *       独占格式。不加载任何 hook，只读。
 *
 *   list_eps.exe          列出全部渲染端点
 *   list_eps.exe --probe  额外对每个端点做一次 44.1k/24bit 独占探测
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>

static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_IAudioClient_l =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID SUB_PCM_L =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* PKEY_Device_FriendlyName */
static const PROPERTYKEY PKEY_FriendlyName_l = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

static void wstr(const WCHAR *s) {
    if (!s) return;
    char buf[512];
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL) > 0)
        fputs(buf, stdout);
}

static void friendly_name(IMMDevice *dev, char *out, size_t n) {
    out[0] = 0;
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) || !ps) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal) {
        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, out, (int)n, NULL, NULL);
    }
    PropVariantClear(&pv);
    IPropertyStore_Release(ps);
}

int main(int argc, char **argv) {
    int do_probe = (argc > 1 && strcmp(argv[1], "--probe") == 0);

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;

    IMMDeviceEnumerator *enumr = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_l, (void **)&enumr)) || !enumr) {
        printf("enumerator unavailable\n");
        return 1;
    }

    IMMDevice *def = NULL;
    IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &def);

    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender, DEVICE_STATE_ACTIVE, &col)) || !col) {
        printf("no active render endpoints\n");
        return 1;
    }

    UINT count = 0;
    IMMDeviceCollection_GetCount(col, &count);
    printf("active render endpoints: %u\n\n", count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice *d = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d)) || !d) continue;

        LPWSTR id = NULL;
        IMMDevice_GetId(d, &id);

        int is_default = 0;
        if (def && id) {
            LPWSTR did = NULL;
            IMMDevice_GetId(def, &did);
            if (did) { is_default = (wcscmp(id, did) == 0); CoTaskMemFree(did); }
        }

        char name[256];
        friendly_name(d, name, sizeof(name));

        printf("[%u] %s%s\n", i, name[0] ? name : "(unnamed)",
               is_default ? "   <== DEFAULT" : "");
        if (id) { printf("    id: "); wstr(id); printf("\n"); }

        IAudioClient *ac = NULL;
        if (SUCCEEDED(IMMDevice_Activate(d, &IID_IAudioClient_l, CLSCTX_ALL, NULL, (void **)&ac)) && ac) {
            WAVEFORMATEX *mix = NULL;
            if (SUCCEEDED(IAudioClient_GetMixFormat(ac, &mix)) && mix) {
                printf("    mix: %lu Hz / %u bit / %u ch / tag 0x%04X\n",
                       (unsigned long)mix->nSamplesPerSec, mix->wBitsPerSample,
                       mix->nChannels, mix->wFormatTag);
                CoTaskMemFree(mix);
            }
            if (do_probe) {
                /* 44.1k/24 + 48k/24，音乐最常见的两种 */
                static const int rates[] = {44100, 48000};
                for (int r = 0; r < 2; r++) {
                    WAVEFORMATEXTENSIBLE we;
                    memset(&we, 0, sizeof(we));
                    we.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
                    we.Format.nChannels       = 2;
                    we.Format.nSamplesPerSec  = (DWORD)rates[r];
                    we.Format.wBitsPerSample  = 24;
                    we.Format.nBlockAlign     = 6;
                    we.Format.nAvgBytesPerSec = (DWORD)rates[r] * 6;
                    we.Format.cbSize          = 22;
                    we.Samples.wValidBitsPerSample = 24;
                    we.dwChannelMask          = 0x3;
                    we.SubFormat              = SUB_PCM_L;
                    HRESULT hf = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                                (WAVEFORMATEX *)&we, NULL);
                    printf("    exclusive %d/24: 0x%08lX%s\n", rates[r], (unsigned long)hf,
                           hf == S_OK ? "  OK" : "");
                }
            }
            IAudioClient_Release(ac);
        } else {
            printf("    (activate failed)\n");
        }
        if (id) CoTaskMemFree(id);
        IMMDevice_Release(d);
        printf("\n");
    }

    if (def) IMMDevice_Release(def);
    IMMDeviceCollection_Release(col);
    IMMDeviceEnumerator_Release(enumr);
    CoUninitialize();
    return 0;
}
