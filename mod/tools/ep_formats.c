/* ep_formats.exe — 打印每个渲染端点自己声明的格式白名单。
 *
 * 为什么需要这个工具：WASAPI 独占的格式协商不是「范围检查」而是「白名单
 * 匹配」。端点通过 PKEY_AudioEndpoint_Formats 声明一组离散格式，
 * IsFormatSupported(EXCLUSIVE) 只在这组里找。所以「端点支持 96 kHz」并不
 * 蕴含「小于 96 的都支持」——88.2 完全可能不在名单上。
 *
 * 这个工具把那份名单解出来，用来解释 88.2 / 176.4 为什么会被 96 的端点拒掉，
 * 而不是靠「试过不行」这种结论。
 *
 * 用法：
 *   ep_formats.exe            全部渲染端点
 *   ep_formats.exe <名称片段>  只匹配名字里含该片段的端点
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmreg.h>      /* WAVEFORMATEX / WAVEFORMATEXTENSIBLE；LEAN_AND_MEAN 不含 mmsystem.h */
#include <mmdeviceapi.h>
#include <propsys.h>
#include <stdio.h>
#include <string.h>

/* mingw 的 libuuid 里没有这几个符号，按 nexsink.c 的做法本地定义 */
static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

/* 端点声明的支持格式列表（VT_BLOB）。Windows 声音设置的「默认格式」下拉框
   就是从这里生成的。 */
static const PROPERTYKEY PKEY_AEF = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0
};
/* 设备当前使用的引擎格式。 */
static const PROPERTYKEY PKEY_DeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0
};
/* 设备原生（OEM）格式，驱动最愿意接受的那个。 */
static const PROPERTYKEY PKEY_OemFormat = {
    {0xe4870e26, 0x3cc5, 0x4cd2, {0xba, 0x46, 0xca, 0x0a, 0x9a, 0x70, 0xed, 0x04}}, 3
};

static void wstr(const WCHAR *s) {
    if (!s) return;
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0) fputs(buf, stdout);
}

static void describe(const char *tag, const WAVEFORMATEX *w) {
    if (!w) { printf("  %-10s (none)\n", tag); return; }
    unsigned valid = w->wBitsPerSample;
    unsigned sub = 0;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)w;
        valid = we->Samples.wValidBitsPerSample;
        sub = we->SubFormat.Data1;
    }
    printf("  %-10s %lu Hz / %u bit (valid %u) / %u ch / tag 0x%04X%s\n",
           tag, (unsigned long)w->nSamplesPerSec, w->wBitsPerSample, valid,
           w->nChannels, w->wFormatTag,
           sub == 3 ? " [float]" : (sub == 1 ? " [pcm]" : ""));
}

/* PKEY_AudioEndpoint_Formats 的布局：一串 WAVEFORMATEX（cbSize=0，18 字节）
   各跟一个 WORD 标志位。用条目数 × 20 与 blob 长度对齐来确认解析正确，
   对不上就只打原始字节，不硬猜。 */
#define AEF_ENTRY 20

static void dump_formats(IPropertyStore *store) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = IPropertyStore_GetValue(store, &PKEY_AEF, &pv);
    if (FAILED(hr) || pv.vt != VT_BLOB || !pv.blob.pBlobData) {
        printf("  formats    (unavailable, hr=0x%08lX vt=%u)\n",
               (unsigned long)hr, (unsigned)pv.vt);
        PropVariantClear(&pv);
        return;
    }

    const BYTE *b = pv.blob.pBlobData;
    ULONG n = pv.blob.cbSize;
    printf("  formats    %lu bytes, %lu entries if 20 B each\n",
           (unsigned long)n, (unsigned long)(n / AEF_ENTRY));

    if (n % AEF_ENTRY != 0) {
        printf("  !! 长度不是 20 的整数倍，布局与预期不符，下面只打原始字节\n");
        for (ULONG i = 0; i < n; i++) {
            printf("%02X%s", b[i], (i + 1) % 16 ? " " : "\n");
        }
        printf("\n");
        PropVariantClear(&pv);
        return;
    }

    /* 采样率 -> 位深集合。按 rate 分组打印，这样「哪些 rate 在名单上」
       一眼可见，正是这次要回答的问题。 */
    unsigned long seen[64];
    int seen_n = 0;
    for (ULONG off = 0; off + AEF_ENTRY <= n; off += AEF_ENTRY) {
        const WAVEFORMATEX *w = (const WAVEFORMATEX *)(b + off);
        WORD flags = *(const WORD *)(b + off + 18);
        if (w->nSamplesPerSec == 0 || w->nChannels == 0) {
            printf("    [%lu] 非法条目，停止解析\n", (unsigned long)(off / AEF_ENTRY));
            break;
        }
        printf("    [%lu] %6lu Hz  %2u ch  %2u bit  tag 0x%04X  flags 0x%04X\n",
               (unsigned long)(off / AEF_ENTRY), (unsigned long)w->nSamplesPerSec,
               w->nChannels, w->wBitsPerSample, w->wFormatTag, flags);
        int known = 0;
        for (int i = 0; i < seen_n; i++) if (seen[i] == w->nSamplesPerSec) known = 1;
        if (!known && seen_n < 64) seen[seen_n++] = w->nSamplesPerSec;
    }

    printf("  rates in whitelist:");
    for (int i = 0; i < seen_n; i++) printf(" %lu", seen[i]);
    printf("\n");

    /* 直接回答「96 支持那 88.2 呢」：把 44.1 家族与 48 家族分开看。 */
    int has_882 = 0, has_1764 = 0, has_441 = 0, has_48 = 0, has_96 = 0, has_192 = 0;
    for (int i = 0; i < seen_n; i++) {
        switch (seen[i]) {
        case 44100:  has_441  = 1; break;
        case 48000:  has_48   = 1; break;
        case 88200:  has_882  = 1; break;
        case 96000:  has_96   = 1; break;
        case 176400: has_1764 = 1; break;
        case 192000: has_192  = 1; break;
        }
    }
    printf("  -> 44.1 family: 44.1 %s, 88.2 %s, 176.4 %s\n",
           has_441 ? "yes" : "NO", has_882 ? "yes" : "NO", has_1764 ? "yes" : "NO");
    printf("  -> 48 family  : 48 %s, 96 %s, 192 %s\n",
           has_48 ? "yes" : "NO", has_96 ? "yes" : "NO", has_192 ? "yes" : "NO");

    PropVariantClear(&pv);
}

static void dump_pkey_format(IPropertyStore *store, const PROPERTYKEY *key, const char *tag) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = IPropertyStore_GetValue(store, key, &pv);
    if (FAILED(hr) || pv.vt != VT_BLOB || !pv.blob.pBlobData ||
        pv.blob.cbSize < sizeof(WAVEFORMATEX)) {
        printf("  %-10s (unavailable, hr=0x%08lX vt=%u)\n", tag, (unsigned long)hr, (unsigned)pv.vt);
        PropVariantClear(&pv);
        return;
    }
    describe(tag, (const WAVEFORMATEX *)pv.blob.pBlobData);
    PropVariantClear(&pv);
}

/* 全量枚举端点属性。PKEY_AudioEndpoint_Formats 在不同驱动上可能根本没设，
   这时只能靠这里看清端点上到底有什么，别对着一个空 key 猜。 */
static void dump_all(IPropertyStore *store) {
    DWORD n = 0;
    if (FAILED(IPropertyStore_GetCount(store, &n))) { printf("  (GetCount failed)\n"); return; }
    printf("  properties: %lu\n", (unsigned long)n);
    for (DWORD i = 0; i < n; i++) {
        PROPERTYKEY key;
        if (FAILED(IPropertyStore_GetAt(store, i, &key))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        HRESULT hr = IPropertyStore_GetValue(store, &key, &pv);
        printf("    {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} pid=%lu vt=%u",
               (unsigned long)key.fmtid.Data1, key.fmtid.Data2, key.fmtid.Data3,
               key.fmtid.Data4[0], key.fmtid.Data4[1], key.fmtid.Data4[2], key.fmtid.Data4[3],
               key.fmtid.Data4[4], key.fmtid.Data4[5], key.fmtid.Data4[6], key.fmtid.Data4[7],
               (unsigned long)key.pid, (unsigned)pv.vt);
        if (SUCCEEDED(hr)) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) {
                printf(" = \"");
                wstr(pv.pwszVal);
                printf("\"");
            } else if (pv.vt == VT_BLOB && pv.blob.pBlobData) {
                printf(" = blob %lu B:", (unsigned long)pv.blob.cbSize);
                ULONG lim = pv.blob.cbSize < 128 ? pv.blob.cbSize : 128;
                for (ULONG k = 0; k < lim; k++) printf(" %02X", pv.blob.pBlobData[k]);
                if (pv.blob.cbSize > lim) printf(" ...");
            } else if (pv.vt == VT_UI4) {
                printf(" = %lu", (unsigned long)pv.ulVal);
            }
        } else {
            printf(" (hr=0x%08lX)", (unsigned long)hr);
        }
        printf("\n");
        PropVariantClear(&pv);
    }
}

int main(int argc, char **argv) {
    const char *filter = NULL;
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) all = 1;
        else filter = argv[i];
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator *enumr = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator_l, (void **)&enumr);
    if (FAILED(hr) || !enumr) { printf("enumerator 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceCollection *coll = NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr) || !coll) { printf("enum 0x%08lX\n", (unsigned long)hr); return 1; }

    UINT count = 0;
    IMMDeviceCollection_GetCount(coll, &count);
    int shown = 0;

    for (UINT i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)) || !dev) continue;

        IPropertyStore *store = NULL;
        if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &store)) || !store) {
            IMMDevice_Release(dev);
            continue;
        }

        /* 友好名用于过滤与显示 */
        char name[512] = "";
        PROPVARIANT pv;
        PropVariantInit(&pv);
        static const PROPERTYKEY PKEY_FriendlyName = {
            {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
        };
        if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal) {
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof(name), NULL, NULL);
        }
        PropVariantClear(&pv);

        if (filter && !strstr(name, filter)) {
            IPropertyStore_Release(store);
            IMMDevice_Release(dev);
            continue;
        }

        printf("\n=== [%u] %s ===\n", i, name);
        if (all) dump_all(store);
        dump_pkey_format(store, &PKEY_OemFormat, "oem");
        dump_pkey_format(store, &PKEY_DeviceFormat, "engine");
        dump_formats(store);
        shown++;

        IPropertyStore_Release(store);
        IMMDevice_Release(dev);
    }

    if (!shown) printf("\n没有匹配的端点%s%s\n", filter ? "：" : "", filter ? filter : "");

    IMMDeviceCollection_Release(coll);
    IMMDeviceEnumerator_Release(enumr);
    CoUninitialize();
    return shown ? 0 : 2;
}
