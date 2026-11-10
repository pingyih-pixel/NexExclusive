/* set_ep.exe — 列出渲染端点，或把 Windows 默认播放端点切到指定设备。
 *
 * 用途：验证 nexsink 在默认输出改变后能不能把正在播放的流迁移过去。人工去
 *       点系统设置既慢又不可重复，这里用一条命令完成切换。
 *
 *   set_ep.exe              列出全部渲染端点，默认的标 DEFAULT
 *   set_ep.exe 耳机          把默认输出切到名称含「耳机」的端点
 *   set_ep.exe --next       切到列表里的下一个端点（来回 A/B 用）
 *
 * 改默认值只能走 IPolicyConfig：IMMDeviceEnumerator 是只读的。这个接口没有
 * 公开头文件，但 vtable 布局多年未变。三个 role 都设——不同客户端读的 role
 * 不一样，网易云读 eMultimedia，多数 UWP 应用读 eConsole。
 *
 * 参数和输出全程用宽字符：命令行参数走 ANSI 代码页、端点名是 UTF-16，混在
 * 一起会在中文名字上错位（第一版就因此匹配不到「扬声器」）。
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

/* CPolicyConfigClient / IPolicyConfig：未公开，布局取自 Windows SDK 之外
   广为使用的定义。只用 SetDefaultEndpoint，其余槽位必须存在以对齐偏移。 */
static const GUID CLSID_CPolicyConfigClient_l =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID IID_IPolicyConfig_l =
    {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

typedef struct IPolicyConfig IPolicyConfig;
typedef struct IPolicyConfigVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPolicyConfig *, const IID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IPolicyConfig *);
    ULONG   (STDMETHODCALLTYPE *Release)(IPolicyConfig *);
    HRESULT (STDMETHODCALLTYPE *GetMixFormat)(IPolicyConfig *, LPCWSTR, void **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFormat)(IPolicyConfig *, LPCWSTR, INT, void **);
    HRESULT (STDMETHODCALLTYPE *ResetDeviceFormat)(IPolicyConfig *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *SetDeviceFormat)(IPolicyConfig *, LPCWSTR, void *, void *);
    HRESULT (STDMETHODCALLTYPE *GetProcessingPeriod)(IPolicyConfig *, LPCWSTR, INT, LONGLONG *, LONGLONG *);
    HRESULT (STDMETHODCALLTYPE *SetProcessingPeriod)(IPolicyConfig *, LPCWSTR, LONGLONG *);
    HRESULT (STDMETHODCALLTYPE *GetShareMode)(IPolicyConfig *, LPCWSTR, void *);
    HRESULT (STDMETHODCALLTYPE *SetShareMode)(IPolicyConfig *, LPCWSTR, void *);
    HRESULT (STDMETHODCALLTYPE *GetPropertyValue)(IPolicyConfig *, LPCWSTR, const void *, void *);
    HRESULT (STDMETHODCALLTYPE *SetPropertyValue)(IPolicyConfig *, LPCWSTR, const void *, void *);
    HRESULT (STDMETHODCALLTYPE *SetDefaultEndpoint)(IPolicyConfig *, LPCWSTR, int);
    HRESULT (STDMETHODCALLTYPE *SetEndpointVisibility)(IPolicyConfig *, LPCWSTR, INT);
} IPolicyConfigVtbl;
struct IPolicyConfig { const IPolicyConfigVtbl *lpVtbl; };

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

/* 宽字符子串匹配，大小写不敏感（中文不受影响）。 */
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

/* IAudioEndpointVolume：只按槽位取函数，避免整张 vtable 结构体写错一个偏移
   就全错。槽位顺序是 SDK 里的固定顺序：
     0-2 IUnknown, 3-4 notify, 5 GetChannelCount, 6-13 各级音量,
     14 SetMute, 15 GetMute, 16-18 step, 19 QueryHardwareSupport, 20 GetVolumeRange */
#define IAEV_GETCOUNT      5
#define IAEV_GETSCALAR     9
#define IAEV_GETMUTE       15
#define IAEV_HWSUPPORT     19

static const GUID IID_IAudioEndpointVolume_l =
    {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

/* 独占模式绕过的是软件音量；端点若带硬件音量节点，系统音量条在独占下依然
   起作用，这是 WASAPI 的语义。QueryHardwareSupport 直接给出答案。 */
static void report_volume(IMMDevice *dev) {
    IUnknown *vol = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume_l, CLSCTX_ALL, NULL,
                                  (void **)&vol)) || !vol) {
        put(L"    volume: unavailable\n");
        return;
    }
    void **vt = *(void ***)vol;
    DWORD hw = 0;
    UINT ch = 0;
    float scalar = 0.0f;
    BOOL mute = 0;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, DWORD *))vt[IAEV_HWSUPPORT])(vol, &hw);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT *))vt[IAEV_GETCOUNT])(vol, &ch);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, float *))vt[IAEV_GETSCALAR])(vol, &scalar);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, BOOL *))vt[IAEV_GETMUTE])(vol, &mute);

    WCHAR line[256];
    swprintf(line, 256, L"    volume: %.0f%%%s  ch=%u  hardware support=%s%s%s\n",
             scalar * 100.0f, mute ? L" (muted)" : L"", ch,
             (hw & 1) ? L"volume" : L"-",
             (hw & 2) ? L" mute" : L"",
             (hw & 4) ? L" meter" : L"");
    put(line);
    if (hw & 1)
        put(L"            -> exclusive mode CANNOT bypass the system slider on this endpoint\n");
    else
        put(L"            -> volume is software-only; exclusive mode bypasses it\n");
    vol->lpVtbl->Release(vol);
}

typedef struct { LPWSTR id; WCHAR name[256]; IMMDevice *dev; } Ep;

int main(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;

    IMMDeviceEnumerator *enumr = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_l, (void **)&enumr)) || !enumr) {
        put(L"enumerator unavailable\n");
        return 1;
    }

    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender, DEVICE_STATE_ACTIVE, &col)) || !col) {
        put(L"no active render endpoints\n");
        return 1;
    }

    UINT count = 0;
    IMMDeviceCollection_GetCount(col, &count);

    Ep eps[32];
    UINT n = 0;
    for (UINT i = 0; i < count && n < 32; i++) {
        IMMDevice *d = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d)) || !d) continue;
        if (FAILED(IMMDevice_GetId(d, &eps[n].id)) || !eps[n].id) { IMMDevice_Release(d); continue; }
        friendly_name(d, eps[n].name, 256);
        eps[n].dev = d;                 /* kept for --vol; released below */
        n++;
    }

    IMMDevice *def = NULL;
    LPWSTR def_id = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &def)) && def)
        IMMDevice_GetId(def, &def_id);

    int def_index = -1;
    for (UINT i = 0; i < n; i++)
        if (def_id && wcscmp(eps[i].id, def_id) == 0) def_index = (int)i;

    int want_vol = (argc > 1 && wcscmp(argv[1], L"--vol") == 0);
    int target = -1;
    if (want_vol) {
        /* listing only */
    } else if (argc > 1 && wcscmp(argv[1], L"--next") == 0) {
        if (n > 1) target = (def_index + 1) % (int)n;
    } else if (argc > 1) {
        for (UINT i = 0; i < n; i++)
            if (contains_ci(eps[i].name, argv[1])) { target = (int)i; break; }
    }

    if (target < 0) {
        WCHAR line[512];
        swprintf(line, 512, L"active render endpoints: %u\n\n", n);
        put(line);
        for (UINT i = 0; i < n; i++) {
            swprintf(line, 512, L"[%u] %s%s\n", i, eps[i].name[0] ? eps[i].name : L"(unnamed)",
                     (int)i == def_index ? L"   <== DEFAULT" : L"");
            put(line);
            if (want_vol && eps[i].dev) report_volume(eps[i].dev);
        }
        if (argc > 1 && !want_vol) {
            swprintf(line, 512, L"\nno endpoint matched \"%s\"\n", argv[1]);
            put(line);
        }
        goto done;
    }

    IPolicyConfig *pc = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_CPolicyConfigClient_l, NULL, CLSCTX_ALL,
                                  &IID_IPolicyConfig_l, (void **)&pc);
    if (FAILED(hr) || !pc) {
        WCHAR line[128];
        swprintf(line, 128, L"IPolicyConfig unavailable 0x%08lX\n", (unsigned long)hr);
        put(line);
        goto done;
    }

    {
        WCHAR line[512];
        swprintf(line, 512, L"switching default output to [%d] %s\n", target,
                 eps[target].name[0] ? eps[target].name : L"(unnamed)");
        put(line);
    }
    static const WCHAR *role_name[] = {L"console", L"multimedia", L"communications"};
    for (int role = 0; role < 3; role++) {
        HRESULT hs = pc->lpVtbl->SetDefaultEndpoint(pc, eps[target].id, role);
        WCHAR line[256];
        swprintf(line, 256, L"  %-14s 0x%08lX%s\n", role_name[role], (unsigned long)hs,
                 SUCCEEDED(hs) ? L"" : L"  FAILED");
        put(line);
    }
    pc->lpVtbl->Release(pc);

done:
    if (def_id) CoTaskMemFree(def_id);
    if (def) IMMDevice_Release(def);
    for (UINT i = 0; i < n; i++) { CoTaskMemFree(eps[i].id); if (eps[i].dev) IMMDevice_Release(eps[i].dev); }
    IMMDeviceCollection_Release(col);
    IMMDeviceEnumerator_Release(enumr);
    CoUninitialize();
    if (argv) LocalFree(argv);
    return (argc > 1 && target < 0 && !want_vol) ? 1 : 0;
}
