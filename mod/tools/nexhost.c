/* nexhost.c — NeteaseExclusive 一键宿主。
 *
 * 一个进程做完过去要手工串的五件事：
 *   1. 客户端没在跑就带 --remote-debugging-port 启动它
 *   2. 找到真正持有音频的主进程，注入 nexsink.dll
 *   3. 起本机配置服务（/api/cfg、/api/ui、/api/status），给设置面板用
 *   4. 通过 CDP 把 panel.js / simplify.js 注入客户端页面
 *   5. 常驻：客户端重启、页面重载、模块掉出进程，都会自动补回去
 *
 * 之所以要一个长驻进程而不是一次性脚本：客户端是被它启动的子进程，
 * 启动器一退出，子进程就会跟着被回收。
 *
 * 编译：
 *   zig cc -target x86_64-windows-gnu -O2 -Wall -o NexExclusive.exe \
 *          nexhost.c -lws2_32 -lole32 -loleaut32
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- 路径与端口 --------------------------------------------------------- */
#define CFG_PORT 9334
#define CDP_PORT 9333

static const wchar_t *kClientExe = L"D:\\Installed_Program\\CloudMusic\\cloudmusic.exe";
static const wchar_t *kClientDir = L"D:\\Installed_Program\\CloudMusic";
static const char *kSinkDll = "D:\\Coding\\NeteaseExclusive\\mod\\build-check\\nexsink.dll";
static const char *kHookCfg = "D:\\Coding\\NeteaseExclusive\\mod\\hook.cfg";
static const char *kUiJson  = "D:\\Coding\\NeteaseExclusive\\mod\\ui.json";
static const char *kSinkLog = "D:\\Coding\\NeteaseExclusive\\mod\\sink.log";
static const char *kHostLog = "D:\\Coding\\NeteaseExclusive\\mod\\host.log";
static const char *kPanelJs = "D:\\Coding\\NeteaseExclusive\\mod\\ui\\panel.js";
static const char *kSimplifyJs = "D:\\Coding\\NeteaseExclusive\\mod\\ui\\simplify.js";

/* ---- 日志 --------------------------------------------------------------- */
static CRITICAL_SECTION g_log_lock;

static void host_log(const char *fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen(kHostLog, "a");
    if (f) {
        fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, (unsigned long)GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    /* 双击运行时窗口里也能看到进展，不必去翻 host.log。
       源文件是 UTF-8，而控制台默认代码页通常是 936，直接输出会乱码，
       所以按控制台当前代码页转一次。 */
    if (GetConsoleWindow()) {
        char ubuf[1024];
        va_list ap2;
        va_start(ap2, fmt);
        vsnprintf(ubuf, sizeof(ubuf), fmt, ap2);
        va_end(ap2);

        wchar_t wbuf[1024];
        int wl = MultiByteToWideChar(CP_UTF8, 0, ubuf, -1, wbuf, 1024);
        fprintf(stderr, "%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
        if (wl > 0) {
            char cbuf[2048];
            int cl = WideCharToMultiByte(GetConsoleOutputCP(), 0, wbuf, -1,
                                         cbuf, sizeof(cbuf), NULL, NULL);
            if (cl > 0) fputs(cbuf, stderr);
            else fputs(ubuf, stderr);
        } else {
            fputs(ubuf, stderr);
        }
        fputc('\n', stderr);
        fflush(stderr);
    }
    LeaveCriticalSection(&g_log_lock);
}

/* ---- 小工具 ------------------------------------------------------------- */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* 只保留末尾 max 字节，读日志用，避免整份读进内存 */
static char *read_tail(const char *path, long max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    long start = n > max ? n - max : 0;
    fseek(f, start, SEEK_SET);
    char *buf = (char *)malloc((size_t)(n - start) + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)(n - start), f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* 把任意文本转成可以安全嵌进 JSON 字符串的形式（不含外层引号） */
static char *json_escape(const char *src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char *out = (char *)malloc(n * 6 + 1);
    if (!out) return NULL;
    char *p = out;
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        switch (*s) {
        case '"':  *p++ = '\\'; *p++ = '"';  break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n';  break;
        case '\r': *p++ = '\\'; *p++ = 'r';  break;
        case '\t': *p++ = '\\'; *p++ = 't';  break;
        case '\b': *p++ = '\\'; *p++ = 'b';  break;
        case '\f': *p++ = '\\'; *p++ = 'f';  break;
        default:
            if (*s < 0x20) p += sprintf(p, "\\u%04x", *s);
            else *p++ = (char)*s;
        }
    }
    *p = 0;
    return out;
}

/* 从扁平 JSON 对象里取一个整数值：{"bits":24,...} -> 24 */
static int json_get_int(const char *json, const char *key, int *out) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

/* ---- 配置：hook.cfg <-> JSON ------------------------------------------- */
/* 顺序固定，面板与日志都依赖这个顺序 */
static const char *kCfgKeys[] = {"enable","bits","rate","buffer","compat","dither","fade_ms","shared_fallback"};
#define CFG_N ((int)(sizeof(kCfgKeys)/sizeof(kCfgKeys[0])))

static void cfg_defaults(int *v) {
    v[0]=1; v[1]=24; v[2]=0; v[3]=0; v[4]=1; v[5]=2; v[6]=5; v[7]=0;
}

static void cfg_read(int *v) {
    cfg_defaults(v);
    char *txt = read_file(kHookCfg);
    if (!txt) return;
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = line;
            while (*k == ' ' || *k == '\t') k++;
            char *val = eq + 1;
            for (int i = 0; i < CFG_N; i++)
                if (strcmp(k, kCfgKeys[i]) == 0) v[i] = atoi(val);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(txt);
}

static void cfg_write(const int *v) {
    FILE *f = fopen(kHookCfg, "w");
    if (!f) { host_log("cfg: 写入失败 %s", kHookCfg); return; }
    for (int i = 0; i < CFG_N; i++) fprintf(f, "%s=%d\n", kCfgKeys[i], v[i]);
    fclose(f);
}

static void cfg_to_json(const int *v, char *out, size_t n) {
    size_t used = 0;
    used += (size_t)snprintf(out + used, n - used, "{");
    for (int i = 0; i < CFG_N; i++)
        used += (size_t)snprintf(out + used, n - used, "%s\"%s\":%d",
                                 i ? "," : "", kCfgKeys[i], v[i]);
    snprintf(out + used, n - used, "}");
}

/* ---- 配置：ui.json <-> JSON -------------------------------------------- */
static const char *kUiKeys[] = {"banner","charts","daily","podcast","choice","skin","pet"};
#define UI_N ((int)(sizeof(kUiKeys)/sizeof(kUiKeys[0])))

static void ui_read(int *v) {
    for (int i = 0; i < UI_N; i++) v[i] = 0;
    char *txt = read_file(kUiJson);
    if (!txt) return;
    for (int i = 0; i < UI_N; i++) json_get_int(txt, kUiKeys[i], &v[i]);
    free(txt);
}

static void ui_write(const int *v) {
    FILE *f = fopen(kUiJson, "w");
    if (!f) return;
    fprintf(f, "{\n");
    for (int i = 0; i < UI_N; i++)
        fprintf(f, "  \"%s\": %d%s\n", kUiKeys[i], v[i], i + 1 < UI_N ? "," : "");
    fprintf(f, "}\n");
    fclose(f);
}

static void ui_to_json(const int *v, char *out, size_t n) {
    size_t used = 0;
    used += (size_t)snprintf(out + used, n - used, "{");
    for (int i = 0; i < UI_N; i++)
        used += (size_t)snprintf(out + used, n - used, "%s\"%s\":%d",
                                 i ? "," : "", kUiKeys[i], v[i]);
    snprintf(out + used, n - used, "}");
}

/* ---- 端点：默认输出有没有硬件音量 --------------------------------------
 *
 * 独占模式下系统音量条还算不算数，取决于端点有没有硬件音量节点：有（多数
 * 耳机、USB DAC）就还能调；没有（多数有源音箱、HDMI 显示器的音频输出）
 * 则整条软件音量链被绕过——滑块会动，声音不变。这是 WASAPI 的语义，不是
 * 本模块的行为；面板上不写清楚，用户只会以为模块把音量弄坏了。
 *
 * 查询是只读的：IMMDeviceEnumerator 取默认渲染端点，IAudioEndpointVolume
 * 问 QueryHardwareSupport。结果缓存几秒，因为面板 5 秒轮询一次，没必要每
 * 次都开一遍 COM 对象。缓存只在 HTTP 线程里读写（accept 循环是单线程），
 * 所以不加锁。
 */
static const GUID CLSID_MMDeviceEnumerator_l =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_l =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_IAudioEndpointVolume_l =
    {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

/* PKEY_Device_FriendlyName */
static const PROPERTYKEY PKEY_FriendlyName_l = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

/* IAudioEndpointVolume 只按槽位取函数，避免整张 vtable 写错一个偏移就全错 */
#define IAEV_HWSUPPORT 19
#define EP_HW_VOLUME   0x1
#define EP_HW_MUTE     0x2

typedef struct EndpointInfo {
    char name[256];   /* UTF-8，空串表示没查到 */
    int  ok;          /* 1 = 有可以显示的答案 */
    int  hw_known;    /* 1 = hw_volume/hw_mute 有意义；0 = 能力未知 */
    int  hw_volume;
    int  hw_mute;
} EndpointInfo;

static EndpointInfo g_ep;
static DWORD        g_ep_stamp;
static int          g_com_ready;

static void ep_query(EndpointInfo *ei) {
    memset(ei, 0, sizeof(*ei));
    if (!g_com_ready) return;

    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_l, (void **)&en)) || !en)
        return;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eMultimedia, &dev)) && dev) {
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1,
                                    ei->name, (int)sizeof(ei->name), NULL, NULL);
            }
            PropVariantClear(&pv);
            ps->lpVtbl->Release(ps);
        }

        IUnknown *vol = NULL;
        if (SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_IAudioEndpointVolume_l,
                                            CLSCTX_ALL, NULL, (void **)&vol)) && vol) {
            void **vt = *(void ***)vol;
            DWORD hw = 0;
            if (SUCCEEDED(((HRESULT (STDMETHODCALLTYPE *)(void *, DWORD *))
                           vt[IAEV_HWSUPPORT])(vol, &hw))) {
                ei->hw_volume = (hw & EP_HW_VOLUME) != 0;
                ei->hw_mute   = (hw & EP_HW_MUTE) != 0;
                ei->hw_known  = 1;
                ei->ok        = 1;
            }
            vol->lpVtbl->Release(vol);
        }
        dev->lpVtbl->Release(dev);
    }
    en->lpVtbl->Release(en);
}

static const EndpointInfo *ep_cached(void) {
    DWORD now = GetTickCount();
    if (g_ep_stamp && (DWORD)(now - g_ep_stamp) < 3000) return &g_ep;
    ep_query(&g_ep);
    g_ep_stamp = now ? now : 1;
    return &g_ep;
}

/* 端点字段拼成 JSON 片段（不含前后逗号）。三种情况必须能分开：
   查不到 -> 三字段都是 null；知道名字但能力未知 -> 名字 + 两个 null；
   都查到 -> 名字 + 布尔。面板只在布尔时才显示那一行。 */
static void ep_json(char *out, size_t n, const EndpointInfo *ei) {
    if (!ei->ok || !ei->name[0]) {
        snprintf(out, n, "\"endpoint\":null,\"hwVolume\":null,\"hwMute\":null");
        return;
    }
    char *name = json_escape(ei->name);
    if (!ei->hw_known)
        snprintf(out, n, "\"endpoint\":\"%s\",\"hwVolume\":null,\"hwMute\":null",
                 name ? name : "");
    else
        snprintf(out, n, "\"endpoint\":\"%s\",\"hwVolume\":%s,\"hwMute\":%s",
                 name ? name : "", ei->hw_volume ? "true" : "false",
                 ei->hw_mute ? "true" : "false");
    free(name);
}

/* 从 "…  ep=\"耳机 (HyperX Cloud III)\" hw=3" 取名字和能力位。名字里可能有
   引号，所以从 " hw=" 反着定位结尾，而不是找下一个引号。 */
static int ep_from_line(const char *line, char *name, size_t n, int *hw) {
    const char *p = strstr(line, " ep=\"");
    if (!p) return 0;
    p += 5;
    const char *end = strstr(p, "\" hw=");
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= n) len = n - 1;
    memcpy(name, p, len);
    name[len] = 0;
    *hw = atoi(end + 5);
    return 1;
}

/* ---- 状态：解析 sink.log 末段 ------------------------------------------ */
static void status_json(char *out, size_t n) {
    char *tail = read_tail(kSinkLog, 12288);

    char engaged[512] = "";
    char lastline[256] = "";
    char fault[512] = "";
    char log_ep[256] = "";
    int  log_hw = -1;

    char *line = tail;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        {   /* 日志是 CRLF，行尾的 \r 不该出现在 JSON 里 */
            size_t L = strlen(line);
            while (L && (line[L-1] == '\r' || line[L-1] == '\n')) line[--L] = 0;
        }
        if (strstr(line, "sink ENGAGED")) {
            snprintf(engaged, sizeof(engaged), "%s", line);
            ep_from_line(line, log_ep, sizeof(log_ep), &log_hw);
        }
        if (strstr(line, "sink failed")) {
            snprintf(fault, sizeof(fault), "%s", line);
        }
        if (*line) snprintf(lastline, sizeof(lastline), "%s", line);
        if (!nl) break;
        line = nl + 1;
    }
    free(tail);

    /* 面板要描述的是 sink 实际接管的那个端点——客户端可以被指定到某个具体
       设备，这时查系统默认端点会解释错的硬件。只有日志里还没有任何 ENGAGED
       记录（一次都没播过）才退回默认端点。 */
    EndpointInfo from_log;
    const EndpointInfo *ei;
    if (log_ep[0]) {
        memset(&from_log, 0, sizeof(from_log));
        snprintf(from_log.name, sizeof(from_log.name), "%s", log_ep);
        from_log.ok = 1;
        if (log_hw >= 0) {
            from_log.hw_known  = 1;
            from_log.hw_volume = (log_hw & EP_HW_VOLUME) != 0;
            from_log.hw_mute   = (log_hw & EP_HW_MUTE) != 0;
        }
        ei = &from_log;
    } else {
        ei = ep_cached();
    }

    char ep[2048];
    ep_json(ep, sizeof(ep), ei);

    char *e = json_escape(engaged);
    char *l = json_escape(lastline);
    char *f = json_escape(fault);
    snprintf(out, n, "{\"loaded\":%s,\"lastEngaged\":\"%s\",\"lastLine\":\"%s\",\"fault\":\"%s\",%s}",
             engaged[0] || lastline[0] ? "true" : "false",
             e ? e : "", l ? l : "", f ? f : "", ep);
    free(e); free(l); free(f);
}

/* ---- 进程 --------------------------------------------------------------- */
static DWORD find_main_client(void) {
    DWORD pids[64], ppids[64];
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"cloudmusic.exe") != 0) continue;
            if (n < 64) { pids[n] = pe.th32ProcessID; ppids[n] = pe.th32ParentProcessID; n++; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!n) return 0;

    /* 主进程是唯一一个"父进程不是 cloudmusic.exe"的：渲染、GPU、音频子进程
       的父进程都是主进程。早先按内存挑，页面重载时渲染进程内存暴涨，会把
       模块注入到错误的进程，所以改成看父子关系。 */
    for (int i = 0; i < n; i++) {
        int parent_is_client = 0;
        for (int j = 0; j < n; j++)
            if (ppids[i] == pids[j]) { parent_is_client = 1; break; }
        if (!parent_is_client) return pids[i];
    }
    return pids[0]; /* 只剩子进程：主进程刚退出，交给上层去重启 */
}

static int client_running(void) { return find_main_client() != 0; }

static int launch_client(void) {
    wchar_t cmd[1024];
    _snwprintf(cmd, 1024, L"\"%ls\" --remote-debugging-port=%d", kClientExe, CDP_PORT);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(kClientExe, cmd, NULL, NULL, FALSE, 0, NULL, kClientDir, &si, &pi)) {
        host_log("启动客户端失败 %lu", GetLastError());
        return 0;
    }
    host_log("已启动客户端 pid=%lu", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ---- 注入 --------------------------------------------------------------- */
static int inject_dll(DWORD pid, const char *dll) {
    size_t len = strlen(dll) + 1;
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) { host_log("注入: OpenProcess(%lu) 失败 %lu", (unsigned long)pid, GetLastError()); return 0; }

    void *remote = VirtualAllocEx(h, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        host_log("注入: VirtualAllocEx 失败 %lu", GetLastError());
        CloseHandle(h);
        return 0;
    }
    if (!WriteProcessMemory(h, remote, dll, len, NULL)) {
        host_log("注入: WriteProcessMemory 失败 %lu", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 0;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE load = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = CreateRemoteThread(h, NULL, 0, load, remote, 0, NULL);
    if (!th) {
        host_log("注入: CreateRemoteThread 失败 %lu", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 0;
    }
    WaitForSingleObject(th, 8000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    host_log("注入 pid=%lu -> LoadLibraryA=0x%lX", (unsigned long)pid, (unsigned long)code);
    return code != 0;
}

/* 客户端是否已经加载过我们的模块：同名 DLL 再注入不会重新执行 DllMain，
   所以必须能查出来，避免误判成"注入失败"而反复重试。 */
static int module_loaded(DWORD pid, const wchar_t *name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    int found = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { found = 1; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

/* ---- 最小 HTTP 客户端（只用来访问 CDP 的 HTTP 端点） ------------------- */
static int http_get(int port, const char *path, char *out, size_t n) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");

    DWORD tv = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return 0; }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
                      path, port);
    send(s, req, rl, 0);

    size_t used = 0;
    for (;;) {
        if (used + 1 >= n) break;
        int got = recv(s, out + used, (int)(n - used - 1), 0);
        if (got <= 0) break;
        used += (size_t)got;
    }
    out[used] = 0;
    closesocket(s);

    char *body = strstr(out, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    memmove(out, body, strlen(body) + 1);
    return 1;
}

/* 取当前页面目标的 id（形如 DD2056...），页面重载后这个 id 会变 */
static int cdp_page_id(char *out, size_t n) {
    char *buf = (char *)malloc(65536);
    if (!buf) return 0;
    int ok = 0;
    out[0] = 0;
    if (http_get(CDP_PORT, "/json/list", buf, 65536)) {
        const char *p = strstr(buf, "\"webSocketDebuggerUrl\"");
        if (p) {
            const char *q = strstr(p, "/devtools/page/");
            if (q) {
                q += strlen("/devtools/page/");
                size_t i = 0;
                while (*q && *q != '"' && i + 1 < n) out[i++] = *q++;
                out[i] = 0;
                ok = i > 0;
            }
        }
    }
    free(buf);
    return ok;
}

/* ---- WebSocket：够用就好，只发文本帧，不校验握手签名 ------------------ */
static SOCKET ws_open(const char *page_id) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(CDP_PORT);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");

    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }

    /* 固定 key 即可：我们不校验 Accept，服务端也不要求 key 唯一 */
    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET /devtools/page/%s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: bmV4aG9zdC1rZXktMDEyMzQ1Ng==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", page_id, CDP_PORT);
    if (send(s, req, rl, 0) != rl) { closesocket(s); return INVALID_SOCKET; }

    /* 读到响应头结束 */
    char buf[2048];
    size_t used = 0;
    while (used < sizeof(buf) - 1) {
        int got = recv(s, buf + used, (int)(sizeof(buf) - 1 - used), 0);
        if (got <= 0) break;
        used += (size_t)got;
        buf[used] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[used] = 0;
    if (!strstr(buf, "101")) {
        host_log("CDP 握手失败: %.60s", buf);
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int ws_send_text(SOCKET s, const char *text, char *resp, size_t respn) {
    size_t len = strlen(text);
    unsigned char head[14];
    size_t hn = 0;
    head[hn++] = 0x81; /* FIN + text */
    if (len <= 125) {
        head[hn++] = (unsigned char)(0x80 | len);
    } else if (len <= 65535) {
        head[hn++] = 0x80 | 126;
        head[hn++] = (unsigned char)((len >> 8) & 0xFF);
        head[hn++] = (unsigned char)(len & 0xFF);
    } else {
        head[hn++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[hn++] = (unsigned char)((len >> (i * 8)) & 0xFF);
    }
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(head + hn, mask, 4);
    hn += 4;

    unsigned char *frame = (unsigned char *)malloc(hn + len);
    if (!frame) return 0;
    memcpy(frame, head, hn);
    for (size_t i = 0; i < len; i++) frame[hn + i] = (unsigned char)(text[i] ^ mask[i & 3]);

    size_t total = hn + len, sent = 0;
    while (sent < total) {
        int n = send(s, (const char *)frame + sent, (int)(total - sent), 0);
        if (n <= 0) { free(frame); return 0; }
        sent += (size_t)n;
    }
    free(frame);

    /* 把响应读掉，否则下一次写入会被未读数据堵住。顺手把内容带回去，
       好让调用方知道 CDP 是否真的接受了这条命令。 */
    if (resp && respn) {
        int n = recv(s, resp, (int)(respn - 1), 0);
        resp[n > 0 ? n : 0] = 0;
    } else {
        char sink[4096];
        recv(s, sink, sizeof(sink) - 1, 0);
    }
    return 1;
}

/* 发一个任意 CDP 命令。params 为 NULL 表示不带参数。 */
static int cdp_cmd(SOCKET s, int id, const char *method, const char *params) {
    size_t cap = strlen(method) + (params ? strlen(params) : 0) + 128;
    char *msg = (char *)malloc(cap);
    if (!msg) return 0;
    if (params)
        snprintf(msg, cap, "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, params);
    else
        snprintf(msg, cap, "{\"id\":%d,\"method\":\"%s\"}", id, method);

    char resp[2048];
    int ok = ws_send_text(s, msg, resp, sizeof(resp));
    free(msg);
    if (ok && strstr(resp, "\"error\"")) {
        host_log("CDP %s 被拒绝: %.200s", method, resp);
        return 0;
    }
    return ok;
}

/* 在页面里执行一段 JS。 */
static int cdp_eval(SOCKET s, const char *js, int id) {
    char *esc = json_escape(js);
    if (!esc) return 0;
    size_t cap = strlen(esc) + 256;
    char *params = (char *)malloc(cap);
    if (!params) { free(esc); return 0; }
    snprintf(params, cap,
             "{\"expression\":\"%s\",\"returnByValue\":false,\"awaitPromise\":false}", esc);
    int ok = cdp_cmd(s, id, "Runtime.evaluate", params);
    free(params);
    free(esc);
    return ok;
}

/* 注册成"每次文档加载都执行"。页面重载后 CEF 的 target id 不变，
   靠比较 id 判断重载是不可靠的，交给浏览器自己重放最稳。 */
static int cdp_add_script(SOCKET s, int id, const char *js) {
    char *esc = json_escape(js);
    if (!esc) return 0;
    size_t cap = strlen(esc) + 64;
    char *params = (char *)malloc(cap);
    if (!params) { free(esc); return 0; }
    snprintf(params, cap, "{\"source\":\"%s\"}", esc);
    int ok = cdp_cmd(s, id, "Page.addScriptToEvaluateOnNewDocument", params);
    free(params);
    free(esc);
    return ok;
}

/* 把两个界面脚本注入当前页面，并登记为随文档加载自动执行。
   文件缺失就跳过，方便单独调试。 */
static int inject_ui_once(void) {
    char page[128];
    if (!cdp_page_id(page, sizeof(page))) {
        host_log("界面注入: 拿不到页面 id");
        return 0;
    }
    SOCKET s = ws_open(page);
    if (s == INVALID_SOCKET) return 0;

    int ok = 0, id = 1;
    char *panel = read_file(kPanelJs);
    if (panel) {
        if (cdp_eval(s, panel, id++)) ok = 1;
        if (cdp_add_script(s, id++, panel)) ok = 1;
        free(panel);
    }
    char *simp = read_file(kSimplifyJs);
    if (simp) {
        if (cdp_eval(s, simp, id++)) ok = 1;
        if (cdp_add_script(s, id++, simp)) ok = 1;
        free(simp);
    }
    closesocket(s);
    if (!ok) host_log("界面注入失败 page=%s", page);
    return ok;
}

/* ---- HTTP 服务 ---------------------------------------------------------- */
static void send_all(SOCKET c, const char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int k = send(c, buf + sent, (int)(n - sent), 0);
        if (k <= 0) return;
        sent += (size_t)k;
    }
}

static void http_reply(SOCKET c, int code, const char *body) {
    char head[512];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Bad Request", (unsigned)strlen(body));
    send_all(c, head, (size_t)hl);
    send_all(c, body, strlen(body));
}

static DWORD WINAPI http_thread(LPVOID unused) {
    (void)unused;
    /* 端点查询要 COM。这个线程全程只有 accept，不投消息循环，所以用 MTA：
       STA 的跨线程调用要窗口消息泵，这里没有。失败（比如别处已按 STA 初始
       化过）就退回不查询，面板显示「未知」，不影响任何音频行为。 */
    g_com_ready = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { host_log("HTTP: socket 失败"); return 0; }

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(CFG_PORT);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0) {
        host_log("HTTP: 端口 %d 被占用，配置服务未启动", CFG_PORT);
        closesocket(srv);
        return 0;
    }
    listen(srv, 8);
    host_log("HTTP: 配置服务已监听 127.0.0.1:%d", CFG_PORT);

    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;

        DWORD tv = 3000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

        char *req = (char *)malloc(65536);
        if (!req) { closesocket(c); continue; }
        size_t used = 0;
        for (;;) {
            if (used + 1 >= 65536) break;
            int got = recv(c, req + used, (int)(65535 - used), 0);
            if (got <= 0) break;
            used += (size_t)got;
            req[used] = 0;
            if (strstr(req, "\r\n\r\n")) {
                /* 有 body 就继续读到 Content-Length 满足 */
                char *cl = strstr(req, "Content-Length:");
                if (!cl) cl = strstr(req, "content-length:");
                if (cl) {
                    long need = strtol(cl + 15, NULL, 10);
                    char *hdr_end = strstr(req, "\r\n\r\n") + 4;
                    long have = (long)(used - (size_t)(hdr_end - req));
                    if (have >= need) break;
                } else break;
            }
        }
        req[used] = 0;

        char method[8] = "", path[128] = "";
        sscanf(req, "%7s %127s", method, path);

        char body_buf[65536];
        body_buf[0] = 0;
        char *hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) snprintf(body_buf, sizeof(body_buf), "%s", hdr_end + 4);

        char out[65536];
        if (strcmp(method, "OPTIONS") == 0) {
            http_reply(c, 200, "{}");
        } else if (strcmp(path, "/api/cfg") == 0 && strcmp(method, "GET") == 0) {
            int v[CFG_N];
            cfg_read(v);
            cfg_to_json(v, out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/cfg") == 0 && strcmp(method, "POST") == 0) {
            int v[CFG_N];
            cfg_read(v);
            for (int i = 0; i < CFG_N; i++) json_get_int(body_buf, kCfgKeys[i], &v[i]);
            if (v[1] != 16 && v[1] != 24 && v[1] != 32) v[1] = 24;
            if (v[5] < 0 || v[5] > 2) v[5] = 2;
            if (v[6] < 0 || v[6] > 100) v[6] = 5;
            cfg_write(v);
            cfg_to_json(v, out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/ui") == 0 && strcmp(method, "GET") == 0) {
            int v[UI_N];
            ui_read(v);
            ui_to_json(v, out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/ui") == 0 && strcmp(method, "POST") == 0) {
            int v[UI_N];
            ui_read(v);
            for (int i = 0; i < UI_N; i++) json_get_int(body_buf, kUiKeys[i], &v[i]);
            ui_write(v);
            ui_to_json(v, out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/status") == 0) {
            status_json(out, sizeof(out));
            http_reply(c, 200, out);
        } else {
            http_reply(c, 400, "{\"error\":\"not found\"}");
        }

        free(req);
        closesocket(c);
    }
    return 0;
}

/* ---- 主循环 ------------------------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    InitializeCriticalSection(&g_log_lock);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    host_log("=== nexhost 启动 ===");

    /* 配置服务先起，面板随时可以连上来 */
    CreateThread(NULL, 0, http_thread, NULL, 0, NULL);

    if (!client_running()) {
        host_log("客户端未运行，开始启动");
        launch_client();
        Sleep(4000);
    } else {
        host_log("客户端已在运行 pid=%lu", (unsigned long)find_main_client());
    }

    DWORD injected_pid = 0;
    char last_page[128] = "";
    int ui_injected = 0;
    DWORD last_probe = 0;
    DWORD last_ui_time = 0;

    for (;;) {
        DWORD pid = find_main_client();

        if (!pid) {
            /* 客户端退出了：等它彻底退出再重启，避免反复拉起 */
            Sleep(2000);
            pid = find_main_client();
            if (!pid) {
                host_log("客户端已退出，重新启动");
                launch_client();
                injected_pid = 0;
                ui_injected = 0;
                last_page[0] = 0;
                Sleep(5000);
            }
            continue;
        }

        if (pid != injected_pid || !module_loaded(pid, L"nexsink.dll")) {
            if (inject_dll(pid, kSinkDll)) {
                injected_pid = pid;
                host_log("模块已挂载到 pid=%lu", (unsigned long)pid);
            }
            ui_injected = 0;
            last_page[0] = 0;
        }

        /* 页面重载会让 id 变化，界面脚本必须跟着重新注入 */
        char page[128];
        DWORD now = GetTickCount();
        if (cdp_page_id(page, sizeof(page))) {
            /* 重载后 CEF 的 target id 不变，所以除了 id 变化，还留一道
               5 秒兜底：CEF 的 addScriptToEvaluateOnNewDocument 时灵时不灵，
               实测连续重载会漏掉一次。两个脚本都是幂等的，重放没有副作用，
               所以用短周期兜底把最坏情况压到几秒。 */
            int need = !ui_injected || strcmp(page, last_page) != 0
                    || (now - last_ui_time > 5000);
            if (need) {
                int was = ui_injected;
                if (inject_ui_once()) {
                    if (!was) host_log("界面脚本已就位 page=%s", page);
                    ui_injected = 1;
                    last_ui_time = now;
                    snprintf(last_page, sizeof(last_page), "%s", page);
                }
            }
        } else if (now - last_probe > 15000) {
            last_probe = now;
            host_log("CDP 端口未就绪（客户端可能刚启动）");
        }

        Sleep(2500);
    }
}
