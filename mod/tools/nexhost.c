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
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <commdlg.h>
#include <winver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

/* ---- 路径与端口 --------------------------------------------------------- */
#define CFG_PORT 9334
#define CDP_PORT 9333

/* 自带文件一律从 exe 所在目录推导。这里原来躺着 7 条写死的开发机绝对路径
   （D:\Coding\NeteaseExclusive\mod\...），换台机器就全失效——正是「别人
   拿到能编译、但跑不起来」的根因。所以发布包就是 OUT_DIR 本身，扁平结构：
     NexExclusive.exe  nexsink.dll  hook.cfg  ui.json  ui\panel.js  ui\simplify.js
   客户端不归我们管，它的位置要探测，见 resolve_client()。 */
extern IMAGE_DOS_HEADER __ImageBase;

static char g_base[MAX_PATH];        /* exe 目录，带尾反斜杠 */
static char kSinkDll[MAX_PATH];
static char kHookCfg[MAX_PATH];
static char kUiJson[MAX_PATH];
static char kSinkLog[MAX_PATH];
static char kHostLog[MAX_PATH];
static char kPanelJs[MAX_PATH];
static char kSimplifyJs[MAX_PATH];
static char kHostCfg[MAX_PATH];
static wchar_t kClientExe[MAX_PATH];
static wchar_t kClientDir[MAX_PATH];
static const char *g_client_src = "(未找到)";

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

static void join_into(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s%s", g_base, rel);
}

static void resolve_paths(void) {
    module_dir(g_base, sizeof(g_base));
    join_into(kSinkDll,    sizeof(kSinkDll),    "nexsink.dll");
    join_into(kHookCfg,    sizeof(kHookCfg),    "hook.cfg");
    join_into(kUiJson,     sizeof(kUiJson),     "ui.json");
    join_into(kSinkLog,    sizeof(kSinkLog),    "sink.log");
    join_into(kHostLog,    sizeof(kHostLog),    "host.log");
    join_into(kPanelJs,    sizeof(kPanelJs),    "ui\\panel.js");
    join_into(kSimplifyJs, sizeof(kSimplifyJs), "ui\\simplify.js");
    join_into(kHostCfg,    sizeof(kHostCfg),    "host.cfg");
}

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
       转码要看输出**去了哪**，不是看有没有控制台窗口：
         · 目标是控制台（字符设备）→ 按控制台代码页转（通常是 936）
         · 目标是管道或文件 → 原样写 UTF-8，转成 GBK 反而让上游解析出错
       之前只判断 GetConsoleWindow()，在「有控制台但 stderr 被重定向」的场景
       下会把 GBK 字节灌进管道，看起来就是一堆乱码。 */
    HANDLE eh = GetStdHandle(STD_ERROR_HANDLE);
    int have_err = (eh != NULL && eh != INVALID_HANDLE_VALUE);
    int to_console = have_err && GetFileType(eh) == FILE_TYPE_CHAR;
    if (GetConsoleWindow() || have_err) {
        char ubuf[1024];
        va_list ap2;
        va_start(ap2, fmt);
        vsnprintf(ubuf, sizeof(ubuf), fmt, ap2);
        va_end(ap2);

        fprintf(stderr, "%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
        if (to_console) {
            wchar_t wbuf[1024];
            int wl = MultiByteToWideChar(CP_UTF8, 0, ubuf, -1, wbuf, 1024);
            char cbuf[2048];
            int cl = wl > 0
                ? WideCharToMultiByte(GetConsoleOutputCP(), 0, wbuf, -1,
                                      cbuf, sizeof(cbuf), NULL, NULL)
                : 0;
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

/* ---- 客户端路径探测 ----------------------------------------------------- */
/* 优先级：环境变量 > host.cfg > 正在运行的客户端 > 注册表 > 常见安装位置。
   「正在运行的进程」排在注册表前面，是因为它拿到的一定是真正在跑的那一份：
   用户可能装了多份、或用绿色版，注册表里根本没有；而我们要注入的正是那个
   正在出声的进程。注册表与常见路径只是它没跑的时候的兜底。 */

static int file_exists_w(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Windows API 接受正斜杠，但写进配置或显示时统一成反斜杠：Git Bash 传参数
   会把 \ 变成 /，留着会让 host.cfg 看起来像是用户手写的错路径。 */
static void normalize_slashes(WCHAR *p) {
    for (; p && *p; p++) if (*p == L'/') *p = L'\\';
}

static int adopt_client(const wchar_t *exe) {
    if (!exe || !exe[0] || !file_exists_w(exe)) return 0;
    wcsncpy(kClientExe, exe, MAX_PATH - 1);
    kClientExe[MAX_PATH - 1] = 0;
    normalize_slashes(kClientExe);
    wcsncpy(kClientDir, kClientExe, MAX_PATH - 1);
    kClientDir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(kClientDir, L'\\');
    if (slash) *slash = 0;
    else kClientDir[0] = 0;
    return 1;
}

/* host.cfg 里读 key=value，值按 UTF-8 存（用户可能写中文路径）。 */
static int cfg_lookup(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    size_t klen = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';') continue;
        if (strncmp(p, key, klen) != 0) continue;
        p += klen;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        snprintf(out, n, "%s", p);
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

static int client_from_env(wchar_t *out, size_t n) {
    WCHAR buf[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"NEX_CLIENT", buf, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return 0;
    wcsncpy(out, buf, n - 1);
    out[n - 1] = 0;
    return 1;
}

static int client_from_cfg(wchar_t *out, size_t n) {
    char utf8[MAX_PATH * 3];
    if (!cfg_lookup(kHostCfg, "client", utf8, sizeof(utf8))) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)n) <= 0) return 0;
    out[n - 1] = 0;
    return 1;
}

static int client_from_running(wchar_t *out, size_t n) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int ok = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"cloudmusic.exe") != 0) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            WCHAR buf[MAX_PATH];
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &sz)) {
                buf[MAX_PATH - 1] = 0;
                if (file_exists_w(buf)) {
                    wcsncpy(out, buf, n - 1);
                    out[n - 1] = 0;
                    ok = 1;
                }
            }
            CloseHandle(h);
            if (ok) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

static int reg_read_sz(HKEY root, const wchar_t *sub, const wchar_t *name,
                       wchar_t *out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0;
    DWORD bytes = n * sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &bytes);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    out[n - 1] = 0;
    return out[0] != 0;
}

/* 注册表只当兜底，所以多试几个位置：网易云不同版本/不同渠道写的 key 不一样，
   猜错也只是退回下一条，不会误判。 */
static int client_from_registry(wchar_t *out, size_t n) {
    static const struct { HKEY root; const wchar_t *sub; const wchar_t *val; } probes[] = {
        { HKEY_CURRENT_USER, L"Software\\NetEase\\CloudMusic", L"InstallPath" },
        { HKEY_CURRENT_USER, L"Software\\NetEase\\CloudMusic", L"InstallDir" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\网易云音乐", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\网易云音乐", L"InstallLocation" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\网易云音乐", L"InstallLocation" },
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        WCHAR dir[MAX_PATH];
        if (!reg_read_sz(probes[i].root, probes[i].sub, probes[i].val, dir, MAX_PATH)) continue;
        /* 值可能带引号、可能已经是 exe 全路径 */
        WCHAR *p = dir;
        while (*p == L' ' || *p == L'"') p++;
        size_t len = wcslen(p);
        while (len && (p[len - 1] == L'"' || p[len - 1] == L' ')) p[--len] = 0;
        WCHAR full[MAX_PATH];
        if (len > 4 && _wcsicmp(p + len - 4, L".exe") == 0)
            _snwprintf(full, MAX_PATH, L"%ls", p);
        else
            _snwprintf(full, MAX_PATH, L"%ls\\cloudmusic.exe", p);
        full[MAX_PATH - 1] = 0;
        if (file_exists_w(full)) {
            wcsncpy(out, full, n - 1);
            out[n - 1] = 0;
            return 1;
        }
    }
    return 0;
}

/* 拼路径的实现在后面（和 copy_into 放一起），这里先声明——扫快捷方式要用。 */
static void join_dir(WCHAR *out, size_t n, const wchar_t *dir, const wchar_t *rel);

/* 快捷方式兜底。绿色版、装在 D 盘自定义目录的客户端都不写注册表，但安装时
   几乎一定会在桌面或开始菜单留一个 .lnk。解析它的目标路径比猜目录可靠得多。
   只认目标是 cloudmusic.exe 的，免得把别的程序的快捷方式当成客户端。 */
static int lnk_target(const wchar_t *lnk, wchar_t *out, size_t n) {
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkW, (void **)&sl)) || !sl)
        return 0;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) && pf) {
        if (SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ))) {
            WCHAR path[MAX_PATH];
            path[0] = 0;
            if (SUCCEEDED(IShellLinkW_GetPath(sl, path, MAX_PATH, NULL, 0)) && path[0]) {
                path[MAX_PATH - 1] = 0;
                if (file_exists_w(path)) {
                    wcsncpy(out, path, n - 1);
                    out[n - 1] = 0;
                    ok = 1;
                }
            }
        }
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return ok;
}

/* 扫一个目录下的 *.lnk；subdirs 非 0 时再往下走一层（开始菜单里常有一层
   同名文件夹）。深度就到一层，再深就没必要了。 */
static int scan_lnk_dir(const wchar_t *dir, wchar_t *out, size_t n, int subdirs) {
    WCHAR pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%ls\\*.lnk", dir);
    pat[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int found = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            WCHAR full[MAX_PATH], tgt[MAX_PATH];
            join_dir(full, MAX_PATH, dir, fd.cFileName);
            if (!lnk_target(full, tgt, MAX_PATH)) continue;
            const wchar_t *base = wcsrchr(tgt, L'\\');
            base = base ? base + 1 : tgt;
            if (_wcsicmp(base, L"cloudmusic.exe") == 0) {
                wcsncpy(out, tgt, n - 1);
                out[n - 1] = 0;
                found = 1;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found || !subdirs) return found;

    _snwprintf(pat, MAX_PATH, L"%ls\\*", dir);
    pat[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        WCHAR sub[MAX_PATH];
        join_dir(sub, MAX_PATH, dir, fd.cFileName);
        if (scan_lnk_dir(sub, out, n, 0)) { found = 1; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static int client_from_lnk(wchar_t *out, size_t n) {
    WCHAR dirs[3][MAX_PATH];
    int nd = 0;
    WCHAR base[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) {
        _snwprintf(dirs[nd], MAX_PATH, L"%ls\\Microsoft\\Windows\\Start Menu\\Programs", base);
        dirs[nd][MAX_PATH - 1] = 0;
        nd++;
    }
    if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH)) {
        _snwprintf(dirs[nd], MAX_PATH, L"%ls\\Desktop", base);
        dirs[nd][MAX_PATH - 1] = 0;
        nd++;
    }
    if (GetEnvironmentVariableW(L"PUBLIC", base, MAX_PATH)) {
        _snwprintf(dirs[nd], MAX_PATH, L"%ls\\Desktop", base);
        dirs[nd][MAX_PATH - 1] = 0;
        nd++;
    }
    /* IShellLink 是 COM。这里可能在任何线程上被调到（安装时在主线程，
       看护循环里也可能），所以自己配对初始化/反初始化。 */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int found = 0;
    for (int i = 0; i < nd && !found; i++)
        found = scan_lnk_dir(dirs[i], out, n, 1);
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    return found;
}

/* 全都找不到时，让用户点一下 cloudmusic.exe。比让他去编辑一个
   藏在 AppData 里的文本文件靠谱。 */
static int pick_client(wchar_t *out, size_t n) {
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    WCHAR file[MAX_PATH];
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"网易云音乐客户端 (cloudmusic.exe)\0cloudmusic.exe\0程序 (*.exe)\0*.exe\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"找不到网易云客户端，请选择 cloudmusic.exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    wcsncpy(out, file, n - 1);
    out[n - 1] = 0;
    return 1;
}

static int client_from_common(wchar_t *out, size_t n) {
    static const wchar_t *roots[] = { L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA" };
    static const wchar_t *rel[] = {
        L"\\Netease\\CloudMusic\\cloudmusic.exe",
        L"\\NetEase\\CloudMusic\\cloudmusic.exe",
    };
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        WCHAR base[MAX_PATH];
        if (GetEnvironmentVariableW(roots[r], base, MAX_PATH) == 0) continue;
        for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
            WCHAR full[MAX_PATH];
            _snwprintf(full, MAX_PATH, L"%ls%ls", base, rel[i]);
            full[MAX_PATH - 1] = 0;
            if (file_exists_w(full)) {
                wcsncpy(out, full, n - 1);
                out[n - 1] = 0;
                return 1;
            }
        }
    }
    return 0;
}

static int resolve_client(void) {
    WCHAR buf[MAX_PATH];
    if (client_from_env(buf, MAX_PATH) && adopt_client(buf))      { g_client_src = "环境变量 NEX_CLIENT"; return 1; }
    if (client_from_cfg(buf, MAX_PATH) && adopt_client(buf))      { g_client_src = "host.cfg"; return 1; }
    if (client_from_running(buf, MAX_PATH) && adopt_client(buf))  { g_client_src = "运行中的客户端进程"; return 1; }
    if (client_from_registry(buf, MAX_PATH) && adopt_client(buf)) { g_client_src = "注册表"; return 1; }
    if (client_from_lnk(buf, MAX_PATH) && adopt_client(buf))      { g_client_src = "桌面/开始菜单快捷方式"; return 1; }
    if (client_from_common(buf, MAX_PATH) && adopt_client(buf))   { g_client_src = "常见安装位置"; return 1; }
    return 0;
}

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
/* ---- 安装 / 卸载 -------------------------------------------------------- */
/* 装到 %LOCALAPPDATA%\NexExclusive，不需要管理员。注册 HKCU 卸载项，所以
   也能从「设置 → 应用」卸掉。自启走 HKCU\...\Run，同样不需要提权。
   不做 IFEO / DLL 代理劫持那类无感注入：省一次点击，换来杀软弹窗和
   「这是不是病毒」的怀疑，不划算。 */

#define APP_NAME    L"NexExclusive"
#define APP_VERSION L"1.0.0"
#define UNINST_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NexExclusive"
#define RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static const GUID CLSID_ShellLink_l = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_IShellLinkW_l = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_IPersistFile_l = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

static void log_w(const char *fmt, const wchar_t *w) {
    char b[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof(b), NULL, NULL);
    host_log(fmt, b);
}

static void log_werr(const char *fmt, const wchar_t *w, DWORD err) {
    char b[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof(b), NULL, NULL);
    host_log(fmt, b, (unsigned long)err);
}

static int install_dir(wchar_t *out, size_t n) {
    WCHAR lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH) == 0) return 0;
    _snwprintf(out, n, L"%ls\\NexExclusive", lad);
    out[n - 1] = 0;
    return 1;
}

static void self_path(wchar_t *out, size_t n) {
    GetModuleFileNameW(NULL, out, (DWORD)n);
    out[n - 1] = 0;
}

/* 拼目录时自己补分隔符。之前直接 "%ls%ls" 拼，源目录带尾反斜杠而安装目录
   不带，结果把 NexExclusive.exe 写成了 ...\Local\NexExclusiveNexExclusive.exe
   ——落在 AppData\Local 根下，安装目录里反而什么都没有。 */
static void join_dir(WCHAR *out, size_t n, const wchar_t *dir, const wchar_t *rel) {
    size_t len = wcslen(dir);
    int tail = (len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/'));
    _snwprintf(out, n, tail ? L"%ls%ls" : L"%ls\\%ls", dir, rel);
    out[n - 1] = 0;
}

static int copy_into(const wchar_t *src_dir, const wchar_t *dst_dir, const wchar_t *rel) {
    WCHAR s[MAX_PATH], d[MAX_PATH];
    join_dir(s, MAX_PATH, src_dir, rel);
    join_dir(d, MAX_PATH, dst_dir, rel);
    if (!CopyFileW(s, d, FALSE)) {
        log_w("复制失败 %s -> ", s);
        log_werr("  %s (%lu)", d, GetLastError());
        return 0;
    }
    return 1;
}

static int make_shortcut(const wchar_t *lnk, const wchar_t *target, const wchar_t *workdir) {
    IShellLinkW *sl = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink_l, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IShellLinkW_l, (void **)&sl);
    if (FAILED(hr) || !sl) { host_log("快捷方式: CoCreateInstance 0x%08lX", (unsigned long)hr); return 0; }
    IShellLinkW_SetPath(sl, target);
    IShellLinkW_SetWorkingDirectory(sl, workdir);
    IShellLinkW_SetDescription(sl, L"NeteaseExclusive - WASAPI 独占播放");
    IPersistFile *pf = NULL;
    int ok = 0;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile_l, (void **)&pf)) && pf) {
        ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return ok;
}

static void shortcut_path(wchar_t *out, size_t n) {
    WCHAR appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) { out[0] = 0; return; }
    _snwprintf(out, n, L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\NexExclusive.lnk", appdata);
    out[n - 1] = 0;
}

static int reg_set_sz(HKEY root, const wchar_t *sub, const wchar_t *name, const wchar_t *val) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val,
                            (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int reg_set_dw(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD val) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int write_host_cfg(const wchar_t *dir, const wchar_t *client) {
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\host.cfg", dir);
    path[MAX_PATH - 1] = 0;
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) { log_w("安装: 写不了 %s", path); return 0; }
    fwprintf(f, L"# NexExclusive 宿主配置（安装程序生成，可手工修改）\n");
    fwprintf(f, L"# client: cloudmusic.exe 的完整路径。留空则自动探测。\n");
    fwprintf(f, L"# console: 1 = 保留控制台窗口（调试用）\n");
    fwprintf(f, L"client=%ls\n", client);
    fwprintf(f, L"console=0\n");
    fclose(f);
    return 1;
}

/* 把用户选定的客户端路径写回 host.cfg，下次就不用再问。复用安装时那套
   格式，别搞两份不一样的配置文件。 */
static int save_client_cfg(const wchar_t *exe) {
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, kHostCfg, -1, path, MAX_PATH) <= 0) return 0;
    path[MAX_PATH - 1] = 0;
    wcsncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    return write_host_cfg(dir, exe);
}

/* 已安装？卸载项里的 InstallLocation 就是答案。没写 UninstallString 的
   手工摆放不算安装——我们只在真装过的时候才提自启和卸载。 */
static int installed_dir(wchar_t *out, size_t n) {
    out[0] = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, sz = (DWORD)(n * sizeof(wchar_t));
    LONG r = RegQueryValueExW(k, L"InstallLocation", NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ || !out[0]) { out[0] = 0; return 0; }
    out[n - 1] = 0;
    return 1;
}

/* 两个目录是不是同一个。只做字面比较，够用：比的是我们自己的安装目录。 */
static int same_dir(const wchar_t *a, const wchar_t *b) {
    size_t la = wcslen(a), lb = wcslen(b);
    while (la > 0 && (a[la - 1] == L'\\' || a[la - 1] == L'/')) la--;
    while (lb > 0 && (b[lb - 1] == L'\\' || b[lb - 1] == L'/')) lb--;
    if (la != lb) return 0;
    return _wcsnicmp(a, b, la) == 0;
}

static void dir_of(const wchar_t *path, wchar_t *out, size_t n) {
    wcsncpy(out, path, n - 1);
    out[n - 1] = 0;
    WCHAR *s = wcsrchr(out, L'\\');
    if (s) s[1] = 0;
}

/* 拉起另一份自己（装好的那份）。装完直接从安装目录启动，省得用户再找快捷方式。 */
static int launch_exe(const wchar_t *exe, const wchar_t *workdir) {
    WCHAR cmd[MAX_PATH * 2];
    _snwprintf(cmd, MAX_PATH * 2, L"\"%ls\"", exe);
    cmd[MAX_PATH * 2 - 1] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, workdir, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* 停掉所有 NexExclusive.exe 进程（自己除外）。安装要覆盖正在运行的主程序、
   卸载要删掉它，都得先停。原来只有卸载做了这件事，于是「程序在跑的时候重装」
   会卡在 CopyFileW 的共享冲突上，报一句「有文件没复制成功」就中止。 */
static int stop_other_hosts(void) {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"NexExclusive.exe") != 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); n++; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static int do_install(const wchar_t *client_override) {
    /* IShellLink 是 COM，没初始化会返回 CO_E_NOTINITIALIZED (0x800401F0)。
       建不了快捷方式不影响主功能，但既然要做就做对。 */
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WCHAR src[MAX_PATH], dst[MAX_PATH], self[MAX_PATH];
    self_path(self, MAX_PATH);
    /* 源目录 = 本 exe 所在目录 */
    wcsncpy(src, self, MAX_PATH - 1);
    src[MAX_PATH - 1] = 0;
    { WCHAR *s = wcsrchr(src, L'\\'); if (s) s[1] = 0; }

    if (!install_dir(dst, MAX_PATH)) { host_log("安装: 读不到 LOCALAPPDATA"); return 1; }

    if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(dst, NULL)) {
            log_werr("安装: 建目录失败 %s (%lu)", dst, GetLastError());
            return 1;
        }
    }
    { WCHAR ui[MAX_PATH]; _snwprintf(ui, MAX_PATH, L"%ls\\ui", dst); ui[MAX_PATH - 1] = 0;
      CreateDirectoryW(ui, NULL); }

    log_w("安装到 %s", dst);

    /* 覆盖正在运行的主程序会撞共享冲突，先停掉；停完等它真正退出再复制。 */
    if (stop_other_hosts()) { host_log("已停止运行中的旧宿主"); Sleep(500); }

    int ok = 1;
    ok &= copy_into(src, dst, L"NexExclusive.exe");
    ok &= copy_into(src, dst, L"nexsink.dll");
    ok &= copy_into(src, dst, L"hook.cfg");
    ok &= copy_into(src, dst, L"ui.json");
    ok &= copy_into(src, dst, L"ui\\panel.js");
    ok &= copy_into(src, dst, L"ui\\simplify.js");
    if (!ok) { host_log("安装失败：有文件没复制成功，已中止"); return 1; }

    /* 客户端路径：命令行给的优先，否则用探测到的，再否则留空让运行时再探 */
    WCHAR client[MAX_PATH];
    if (client_override && client_override[0]) {
        wcsncpy(client, client_override, MAX_PATH - 1);
        client[MAX_PATH - 1] = 0;
        normalize_slashes(client);
    } else if (kClientExe[0]) {
        wcsncpy(client, kClientExe, MAX_PATH - 1);
        client[MAX_PATH - 1] = 0;
    } else {
        client[0] = 0;
    }
    if (!write_host_cfg(dst, client)) return 1;
    if (client[0]) log_w("已记录客户端路径 %s", client);
    else host_log("客户端路径留空，首次运行时自动探测");

    /* 卸载项：设置 → 应用 里能看到并卸载 */
    WCHAR uninst[MAX_PATH * 2];
    _snwprintf(uninst, MAX_PATH * 2, L"\"%ls\\NexExclusive.exe\" --uninstall", dst);
    uninst[MAX_PATH * 2 - 1] = 0;
    WCHAR exe_path[MAX_PATH];
    _snwprintf(exe_path, MAX_PATH, L"%ls\\NexExclusive.exe", dst);
    exe_path[MAX_PATH - 1] = 0;

    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayName", APP_NAME L" - WASAPI 独占播放");
    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayVersion", APP_VERSION);
    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"Publisher", L"NeteaseExclusive");
    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"InstallLocation", dst);
    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayIcon", exe_path);
    reg_set_sz(HKEY_CURRENT_USER, UNINST_KEY, L"UninstallString", uninst);
    reg_set_dw(HKEY_CURRENT_USER, UNINST_KEY, L"NoModify", 1);
    reg_set_dw(HKEY_CURRENT_USER, UNINST_KEY, L"NoRepair", 1);
    host_log("已注册卸载项");

    /* 自启：登录时把宿主拉起来，之后开客户端就自动注入 */
    WCHAR run[MAX_PATH * 2];
    _snwprintf(run, MAX_PATH * 2, L"\"%ls\"", exe_path);
    run[MAX_PATH * 2 - 1] = 0;
    if (reg_set_sz(HKEY_CURRENT_USER, RUN_KEY, L"NexExclusive", run))
        host_log("已设为登录自启（HKCU\\...\\Run）");
    else
        host_log("自启注册失败，可以手工把快捷方式放进启动文件夹");

    WCHAR lnk[MAX_PATH];
    shortcut_path(lnk, MAX_PATH);
    if (lnk[0] && make_shortcut(lnk, exe_path, dst)) host_log("已创建开始菜单快捷方式");
    else host_log("开始菜单快捷方式没建成，不影响使用");

    host_log("安装完成。从开始菜单启动 NexExclusive，或在「设置 → 应用」里卸载。");
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

static int do_uninstall(void) {
    WCHAR dst[MAX_PATH], self[MAX_PATH];
    self_path(self, MAX_PATH);
    if (!install_dir(dst, MAX_PATH)) return 1;

    /* 先停掉正在跑的宿主（从安装目录启动的那个），否则文件删不掉 */
    if (stop_other_hosts()) host_log("已停止运行中的宿主");
    Sleep(400);

    RegDeleteKeyW(HKEY_CURRENT_USER, UNINST_KEY);
    RegDeleteValueW(HKEY_CURRENT_USER, L"NexExclusive");
    { HKEY k;
      if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
          RegDeleteValueW(k, L"NexExclusive");
          RegCloseKey(k);
      } }
    host_log("已移除卸载项与自启");

    WCHAR lnk[MAX_PATH];
    shortcut_path(lnk, MAX_PATH);
    if (lnk[0]) DeleteFileW(lnk);

    /* 删文件。自己正在运行，删不掉，标记重启后删。 */
    static const wchar_t *files[] = {
        L"nexsink.dll", L"hook.cfg", L"ui.json", L"host.cfg",
        L"ui\\panel.js", L"ui\\simplify.js",
        L"host.log", L"sink.log", L"sink.prev.log",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\%ls", dst, files[i]);
        p[MAX_PATH - 1] = 0;
        DeleteFileW(p);
    }
    { WCHAR ui[MAX_PATH]; _snwprintf(ui, MAX_PATH, L"%ls\\ui", dst); ui[MAX_PATH - 1] = 0;
      RemoveDirectoryW(ui); }

    WCHAR exe_path[MAX_PATH];
    _snwprintf(exe_path, MAX_PATH, L"%ls\\NexExclusive.exe", dst);
    exe_path[MAX_PATH - 1] = 0;
    /* 如果当前就是从安装目录跑的，自己正在运行，文件删不掉。
       MOVEFILE_DELAY_UNTIL_REBOOT 要写 HKLM\...\PendingFileRenameOperations，
       需要管理员；普通权限下它是静默失败的——原来不管成败都打「将在下次重启后
       移除」，等于骗人：目录其实一直留着。所以按返回值说话。 */
    int leftover = 0;
    if (_wcsicmp(self, exe_path) == 0) {
        if (MoveFileExW(exe_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            host_log("主程序已登记为重启后删除");
        } else {
            leftover = 1;
            log_w("主程序正在运行，删不掉（登记重启删除需要管理员权限）。"
                  "关掉本程序后直接删掉目录 %s 即可。", dst);
        }
    } else if (!DeleteFileW(exe_path)) {
        leftover = 1;
    }
    RemoveDirectoryW(dst);
    /* 「卸载完成」和「主程序还在」同时出现会让人以为没卸干净，所以分开说 */
    if (leftover) host_log("卸载完成（除主程序文件外，其余已清除）");
    else          host_log("卸载完成");
    return 0;
}

/* ---- 客户端版本 --------------------------------------------------------- */
/* 注入靠 IAudioClient / IMMDevice 的 vtable 槽位，客户端大版本更新有可能改动
   布局或模块加载顺序。这里不做硬门槛（拦下来用户也没法用），只是让日志说清楚
   「这个版本我们没验过」，而不是让用户对着「没效果」自己猜。 */
static const wchar_t *kVerified[] = {
    L"3.1.42.205594",
};

static void client_version(wchar_t *out, size_t n) {
    out[0] = 0;
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(kClientExe, &dummy);
    if (!sz) return;
    BYTE *buf = (BYTE *)malloc(sz);
    if (!buf) return;
    if (GetFileVersionInfoW(kClientExe, 0, sz, buf)) {
        /* 网易把构建号写在字符串表里（3.1.42.205594），而 VS_FIXEDFILEINFO
           的数字字段是另一套（3.1.42.8986）。字符串那个才是客户端「关于」
           里显示的、也是我们验证时看到的，所以优先取它，取不到再退回数字。 */
        struct { WORD lang, cp; } *tr = NULL;
        UINT trlen = 0;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &trlen)
            && tr && trlen >= sizeof(*tr)) {
            WCHAR key[96];
            _snwprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\FileVersion", tr->lang, tr->cp);
            key[95] = 0;
            WCHAR *sv = NULL;
            UINT svlen = 0;
            if (VerQueryValueW(buf, key, (void **)&sv, &svlen) && sv && svlen) {
                wcsncpy(out, sv, n - 1);
                out[n - 1] = 0;
            }
        }
        if (!out[0]) {
            VS_FIXEDFILEINFO *fi = NULL;
            UINT len = 0;
            if (VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi) {
                _snwprintf(out, n, L"%u.%u.%u.%u",
                           (unsigned)HIWORD(fi->dwFileVersionMS), (unsigned)LOWORD(fi->dwFileVersionMS),
                           (unsigned)HIWORD(fi->dwFileVersionLS), (unsigned)LOWORD(fi->dwFileVersionLS));
                out[n - 1] = 0;
            }
        }
    }
    free(buf);
}

static void check_client_version(void) {
    WCHAR ver[64];
    client_version(ver, 64);
    if (!ver[0]) { host_log("客户端版本读取失败，按未知处理"); return; }
    int known = 0;
    for (size_t i = 0; i < sizeof(kVerified) / sizeof(kVerified[0]); i++)
        if (_wcsicmp(ver, kVerified[i]) == 0) known = 1;
    char v8[64];
    WideCharToMultiByte(CP_ACP, 0, ver, -1, v8, sizeof(v8), NULL, NULL);
    if (known) host_log("客户端版本 %s（已验证）", v8);
    else host_log("客户端版本 %s 未经验证——注入可能失效。若出现没声音/没效果，"
                  "先怀疑版本差异，而不是设备或配置。", v8);
}

/* 程序编成 WINDOWS 子系统，日常运行不弹控制台。但安装、卸载、帮助这些
   是用户在命令行里主动发起的，必须看得见输出，所以按需分配一个。
   注意别无条件 freopen：如果 stdout 已经被父进程重定向到管道或文件，
   接管过来反而会把输出吞掉（自动化测试就抓不到了）。 */
static void ensure_console(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    int have_out = (out != NULL && out != INVALID_HANDLE_VALUE);
    if (!have_out && !GetConsoleWindow()) {
        if (!AllocConsole()) return;
        have_out = 0;
    }
    if (!have_out) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    }
    SetConsoleOutputCP(CP_UTF8);   /* 源码是 UTF-8，不设的话中文是乱码 */
    SetConsoleTitleW(L"NexExclusive");
}

static void usage(void) {
    printf("NexExclusive %ls - 给网易云客户端加上 WASAPI 独占输出\n\n", APP_VERSION);
    printf("  双击运行                              首次会问要不要安装，装完即用\n");
    printf("  NexExclusive.exe                      启动客户端并注入（默认）\n");
    printf("  NexExclusive.exe --install            安装到 %%LOCALAPPDATA%%\\NexExclusive\n");
    printf("  NexExclusive.exe --install --client <路径>\n");
    printf("                                        安装时指定 cloudmusic.exe\n");
    printf("  NexExclusive.exe --uninstall          卸载\n");
    printf("  NexExclusive.exe --help               显示本说明\n\n");
    printf("日常运行不弹控制台，程序驻留在托盘。加 --console 可以保留窗口看输出。\n\n");
    printf("不带参数时，客户端路径按以下顺序探测：\n");
    printf("  环境变量 NEX_CLIENT > host.cfg 的 client= > 运行中的客户端\n");
    printf("  > 注册表 > 桌面/开始菜单快捷方式 > 常见安装位置\n");
    printf("  都找不到会弹文件选择框，选一次就记进 host.cfg。\n");
}

/* ---- 托盘与看护循环 ----------------------------------------------------- */
/* 主循环原来是 for(;;) + Sleep(2500)，控制台窗口必须留着才能退出。改成
   定时器驱动 + 托盘图标之后，窗口可以完全隐藏，退出走托盘菜单。
   看护状态从 main 的局部变量提成静态：循环体现在由 WM_TIMER 触发。 */
#define WM_TRAY_EVENT (WM_APP + 1)
#define TRAY_ID       1
#define TIMER_TICK    1
#define MENU_CLIENT   1
#define MENU_LOGS     2
#define MENU_FOLDER   3
#define MENU_QUIT     4

static NOTIFYICONDATAW g_nid;
static int g_tray_up = 0;

static DWORD g_injected_pid;
static char  g_last_page[128];
static int   g_ui_injected;
static DWORD g_last_probe;
static DWORD g_last_ui_time;

static void watchdog_tick(void) {
    DWORD pid = find_main_client();

    if (!pid) {
        /* 客户端退出了：等它彻底退出再重启，避免反复拉起 */
        Sleep(2000);
        pid = find_main_client();
        if (!pid) {
            host_log("客户端已退出，重新启动");
            launch_client();
            g_injected_pid = 0;
            g_ui_injected = 0;
            g_last_page[0] = 0;
            Sleep(5000);
        }
        return;
    }

    if (pid != g_injected_pid || !module_loaded(pid, L"nexsink.dll")) {
        if (inject_dll(pid, kSinkDll)) {
            g_injected_pid = pid;
            host_log("模块已挂载到 pid=%lu", (unsigned long)pid);
        }
        g_ui_injected = 0;
        g_last_page[0] = 0;
    }

    /* 页面重载会让 id 变化，界面脚本必须跟着重新注入 */
    char page[128];
    DWORD now = GetTickCount();
    if (cdp_page_id(page, sizeof(page))) {
        /* 重载后 CEF 的 target id 不变，所以除了 id 变化，还留一道
           5 秒兜底：CEF 的 addScriptToEvaluateOnNewDocument 时灵时不灵，
           实测连续重载会漏掉一次。两个脚本都是幂等的，重放没有副作用，
           所以用短周期兜底把最坏情况压到几秒。 */
        int need = !g_ui_injected || strcmp(page, g_last_page) != 0
                || (now - g_last_ui_time > 5000);
        if (need) {
            int was = g_ui_injected;
            if (inject_ui_once()) {
                if (!was) host_log("界面脚本已就位 page=%s", page);
                g_ui_injected = 1;
                g_last_ui_time = now;
                snprintf(g_last_page, sizeof(g_last_page), "%s", page);
            }
        }
    } else if (now - g_last_probe > 15000) {
        g_last_probe = now;
        host_log("CDP 端口未就绪（客户端可能刚启动）");
    }
}

static void tray_add(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_EVENT;
    /* IDI_APPLICATION 在非 UNICODE 编译下展开成 MAKEINTRESOURCEA，给 LoadIconW
       会报指针类型不匹配，所以直接写宽版本 */
    g_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    wcsncpy(g_nid.szTip, L"NexExclusive — WASAPI 独占播放", 127);
    g_nid.szTip[127] = 0;
    g_tray_up = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (!g_tray_up) host_log("托盘图标添加失败，程序仍在后台运行");
}

static void tray_remove(void) {
    if (g_tray_up) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray_up = 0;
    }
}

static void open_logs(void) {
    /* 路径是 ACP 的，转一次宽字符再交给记事本 */
    WCHAR wlog[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, kHostLog, -1, wlog, MAX_PATH);
    ShellExecuteW(NULL, L"open", L"notepad.exe", wlog, NULL, SW_SHOWNORMAL);
}

static void tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MENU_CLIENT, L"打开网易云音乐");
    AppendMenuW(m, MF_STRING, MENU_LOGS,   L"查看日志");
    AppendMenuW(m, MF_STRING, MENU_FOLDER, L"打开程序目录");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_QUIT,   L"退出（停止注入）");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   /* 不设的话菜单点外面不会消失 */
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_TICK) watchdog_tick();
        return 0;
    case WM_TRAY_EVENT:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK) {
            if (!client_running()) launch_client();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MENU_CLIENT:
            if (!client_running()) launch_client();
            break;
        case MENU_LOGS:
            open_logs();
            break;
        case MENU_FOLDER: {
            WCHAR wdir[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, g_base, -1, wdir, MAX_PATH);
            ShellExecuteW(NULL, L"open", wdir, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case MENU_QUIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_log_lock);
    resolve_paths();   /* 必须早于第一个 host_log：日志路径就是从这儿来的 */

    WCHAR client_arg[MAX_PATH];
    client_arg[0] = 0;
    int want_install = 0, want_uninstall = 0, want_help = 0, want_console = 0;
    const char *bad = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install"))        want_install = 1;
        else if (!strcmp(argv[i], "--uninstall")) want_uninstall = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) want_help = 1;
        else if (!strcmp(argv[i], "--console"))   want_console = 1;
        else if (!strcmp(argv[i], "--client") && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, client_arg, MAX_PATH);
            client_arg[MAX_PATH - 1] = 0;
        } else { bad = argv[i]; break; }
    }

    /* 这些模式是用户在命令行主动发起的，输出必须看得见 */
    if (want_console || want_install || want_uninstall || want_help) ensure_console();

    if (bad) { fprintf(stderr, "未知参数：%s（试试 --help）\n", bad); return 2; }
    if (want_help) { usage(); return 0; }
    if (want_uninstall) return do_uninstall();
    if (want_install) {
        /* 装之前先探一次客户端：探到就写进 host.cfg，省得用户再配 */
        resolve_client();
        return do_install(client_arg[0] ? client_arg : NULL);
    }

    /* 有没有人在看这个进程的输出？没有就是用户双击的（自启也算）。
       这个判断决定了两件事：出问题弹不弹窗、要不要走「首次安装」引导。 */
    HANDLE stdout_h = GetStdHandle(STD_OUTPUT_HANDLE);
    int interactive = (stdout_h == NULL || stdout_h == INVALID_HANDLE_VALUE);

    /* 单实例。装了自启之后，「开机拉起的那份」和「用户又双击下载目录里那份」
       会同时存在：两个宿主抢同一个配置端口 9334，还会互相把注入闸门判成
       busy。所以先到先得，后来的直接退。句柄故意不关——进程活着锁就在。 */
    HANDLE once = CreateMutexW(NULL, FALSE, L"Local\\NexExclusiveHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        host_log("已有一个 NexExclusive 在运行，本次启动退出");
        if (interactive)
            MessageBoxW(NULL, L"NexExclusive 已经在运行了。\n\n"
                              L"看右下角托盘图标；要退出请右键它选「退出」。",
                        L"NexExclusive", MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    /* 装过了、但这次是从别处（比如解压出来的下载目录）双击的：
       别再起一份，直接把装好的那份拉起来。 */
    {
        WCHAR inst[MAX_PATH], self[MAX_PATH], selfdir[MAX_PATH];
        if (installed_dir(inst, MAX_PATH)) {
            self_path(self, MAX_PATH);
            dir_of(self, selfdir, MAX_PATH);
            if (!same_dir(inst, selfdir)) {
                WCHAR iexe[MAX_PATH];
                join_dir(iexe, MAX_PATH, inst, L"NexExclusive.exe");
                if (GetFileAttributesW(iexe) != INVALID_FILE_ATTRIBUTES) {
                    log_w("已安装到 %s，改为启动那份", inst);
                    if (interactive) {
                        WCHAR msg[MAX_PATH * 2];
                        _snwprintf(msg, MAX_PATH * 2,
                                   L"NexExclusive 已经安装过了，正在启动已安装的版本。\n\n%ls",
                                   inst);
                        msg[MAX_PATH * 2 - 1] = 0;
                        MessageBoxW(NULL, msg, L"NexExclusive", MB_ICONINFORMATION | MB_SETFOREGROUND);
                    }
                    /* 先放锁再拉进程，否则新起的那个会撞上自己的单实例锁直接退出 */
                    if (once) CloseHandle(once);
                    launch_exe(iexe, inst);
                    return 0;
                }
            }
        }
    }

    /* 从没装过、又是双击启动：问一句要不要装。不装也能用（就地运行），
       只是没有自启和卸载入口——所以默认给「是」，但不强求。 */
    {
        WCHAR inst[MAX_PATH];
        if (!installed_dir(inst, MAX_PATH) && interactive) {
            int yes = MessageBoxW(NULL,
                L"要把 NexExclusive 安装到用户目录吗？\n\n"
                L"  · 装到 %LOCALAPPDATA%\\NexExclusive\n"
                L"  · 开机自动启动，之后开网易云就自动生效\n"
                L"  · 可在「设置 → 应用」里卸载\n\n"
                L"选「否」也可以直接就地运行，只是不会自启、也没有卸载入口。",
                L"NexExclusive", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
            if (yes) {
                resolve_client();
                if (do_install(client_arg[0] ? client_arg : NULL) != 0) return 1;
                /* 装完就把它跑起来。得先放开单实例锁，否则新进程会被自己挡掉。 */
                if (once) CloseHandle(once);
                WCHAR iexe[MAX_PATH];
                if (install_dir(inst, MAX_PATH)) {
                    join_dir(iexe, MAX_PATH, inst, L"NexExclusive.exe");
                    launch_exe(iexe, inst);
                }
                return 0;
            }
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    host_log("=== nexhost 启动 ===");
    host_log("程序目录 %s", g_base);

    if (!resolve_client()) {
        host_log("找不到网易云客户端。已依次尝试：环境变量 NEX_CLIENT、host.cfg、"
                 "运行中的进程、注册表、桌面/开始菜单快捷方式、常见安装位置。");
        /* 托盘程序没有控制台，光写日志用户什么也看不到。但如果是被脚本或
           测试调用（stdout 已重定向），弹窗会把流程卡死，所以只在不带输出
           句柄的交互式启动时才弹。 */
        ensure_console();
        fprintf(stderr, "找不到网易云客户端，详见 %s\n", kHostLog);
        if (interactive) {
            /* 让用户点一下，比让他去编辑 AppData 里的文本文件靠谱。
               选完写回 host.cfg，以后启动就不再问。 */
            WCHAR picked[MAX_PATH];
            if (pick_client(picked, MAX_PATH) && adopt_client(picked)) {
                g_client_src = "手动选择";
                log_w("已选择客户端 %s", kClientExe);
                if (save_client_cfg(kClientExe)) host_log("已写回 host.cfg，下次不用再选");
                else host_log("host.cfg 写不进去，下次启动还会再问一次");
            } else {
                WCHAR msg[MAX_PATH * 2];
                WCHAR wcfg[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, kHostCfg, -1, wcfg, MAX_PATH);
                _snwprintf(msg, MAX_PATH * 2,
                           L"没有选择网易云客户端（cloudmusic.exe），程序退出。\n\n"
                           L"也可以手工编辑下面的文件，加一行：\n"
                           L"    client=cloudmusic.exe 的完整路径\n\n%ls", wcfg);
                msg[MAX_PATH * 2 - 1] = 0;
                MessageBoxW(NULL, msg, L"NexExclusive", MB_ICONWARNING | MB_SETFOREGROUND);
                return 2;
            }
        } else {
            return 2;
        }
    }
    {
        char cl[MAX_PATH * 3];
        WideCharToMultiByte(CP_ACP, 0, kClientExe, -1, cl, sizeof(cl), NULL, NULL);
        host_log("客户端 %s（来源：%s）", cl, g_client_src);
    }
    check_client_version();

    /* 配置服务先起，面板随时可以连上来 */
    CreateThread(NULL, 0, http_thread, NULL, 0, NULL);

    if (!client_running()) {
        host_log("客户端未运行，开始启动");
        launch_client();
        Sleep(4000);
    } else {
        host_log("客户端已在运行 pid=%lu", (unsigned long)find_main_client());
    }

    /* 隐藏窗口 + 托盘 + 定时器。看护逻辑在 watchdog_tick 里，由 WM_TIMER 驱动。 */
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"NexExclusiveHost";
    if (!RegisterClassExW(&wc)) {
        host_log("窗口类注册失败 %lu", GetLastError());
        return 1;
    }
    HWND hwnd = CreateWindowExW(0, L"NexExclusiveHost", L"NexExclusive", WS_OVERLAPPEDWINDOW,
                                0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        host_log("窗口创建失败 %lu", GetLastError());
        return 1;
    }
    tray_add(hwnd);
    SetTimer(hwnd, TIMER_TICK, 2500, NULL);

    watchdog_tick();   /* 先跑一次，别让用户等 2.5 秒才看到注入 */

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    host_log("=== nexhost 退出 ===");
    return 0;
}
