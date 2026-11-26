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
#include <commctrl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <commdlg.h>
#include <winver.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <wchar.h>

#include "../src/nexui_shared.h"
#include "nexsmtc.h"
#include "nexcontrol.h"
#include "nexconflict.h"
#include "../src/nexlogfile.h"

/* ---- 程序身份 ---------------------------------------------------------- */
/* build.sh 与 release.sh 用 sed 从这个文件读 APP_VERSION，所以它必须留在这里。 */
#define APP_NAME    L"NexExclusive"
#define APP_VERSION L"1.0.0"
#define UNINST_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NexExclusive"
#define RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* ---- 路径与端口 --------------------------------------------------------- */
#define CFG_PORT 9334
#define CDP_PORT 9333

/* 自带文件一律从 exe 所在目录推导。这里原来躺着 7 条写死的开发机绝对路径
   （<项目目录>\mod\...），换台机器就全失效——正是「别人拿到能编译、但跑
   不起来」的根因。所以发布包就是 OUT_DIR 本身，扁平结构：
     NexExclusive.exe  nexsink.dll  nexui.dll  hook.cfg  ui.json  ui\panel.js  ui\simplify.js
   客户端不归我们管，它的位置要探测，见 resolve_client()。 */
extern IMAGE_DOS_HEADER __ImageBase;

static char g_base[MAX_PATH];        /* exe 目录，带尾反斜杠 */
static char kSinkDll[MAX_PATH];
static char kUiDll[MAX_PATH];
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
static int g_client_verified;
static char g_panel_script_id[128];
static char g_simplify_script_id[128];

typedef struct {
    char status[32];
    char play_id[128];
    char format[16];
    char encoding[24];
    int rate;
    int channels;
    int sample_bytes;
    int valid_bits;
    int container_bits;
    int lossless;
    int bitrate_kbps;
    int quality_tier;          /* 缓存文件名里的品质档（64..6999），0 = 非缓存来源 */
    int quality_tier_hint;     /* 面板从当前质量控件读到的选择提示，0 = 未取得 */
    char selection[32];        /* 缓存选择方式：quality/rate/mtime */
    ULONGLONG audio_generation;
    ULONGLONG updated_at_ms;
    /* 播放中由宿主自己读缓存头：换歌后在一段时间窗内反复确认，
       以便缓存文件晚到、或 DLL 协商出新的输入采样率后纠正选择。 */
    ULONGLONG probe_started_ms;
    ULONGLONG probe_last_ms;
    char probe_path[MAX_PATH];
} SourceMeta;
static SourceMeta g_source;

static void source_clear(void) {
    memset(&g_source, 0, sizeof(g_source));
    snprintf(g_source.status, sizeof(g_source.status), "unavailable");
}
static void source_refresh_probe(void);
static int source_status_has_metadata(const char *status);

static int module_dir(char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&__ImageBase, &self))
        return 0;
    WCHAR wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 0;
    WCHAR *slash = wcsrchr(wpath, L'\\');
    if (!slash) return 0;
    slash[1] = L'\0';
    BOOL used_default = FALSE;
    int converted = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wpath, -1,
                                        out, (int)n, NULL, &used_default);
    if (converted <= 0 || used_default || (size_t)converted > n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

static int join_into(char *out, size_t n, const char *rel) {
    if (!out || n == 0 || !g_base[0] || !rel) {
        if (out && n) out[0] = 0;
        return 0;
    }
    int written = snprintf(out, n, "%s%s", g_base, rel);
    if (written < 0 || (size_t)written >= n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

static int resolve_paths(void) {
    if (!module_dir(g_base, sizeof(g_base))) return 0;
    return join_into(kSinkDll,    sizeof(kSinkDll),    "nexsink.dll") &&
           join_into(kUiDll,      sizeof(kUiDll),      "nexui.dll") &&
           join_into(kHookCfg,    sizeof(kHookCfg),    "hook.cfg") &&
           join_into(kUiJson,     sizeof(kUiJson),     "ui.json") &&
           join_into(kSinkLog,    sizeof(kSinkLog),    "sink.log") &&
           join_into(kHostLog,    sizeof(kHostLog),    "host.log") &&
           join_into(kPanelJs,    sizeof(kPanelJs),    "ui\\panel.js") &&
           join_into(kSimplifyJs, sizeof(kSimplifyJs), "ui\\simplify.js") &&
           join_into(kHostCfg,    sizeof(kHostCfg),    "host.cfg");
}

/* ---- 日志 --------------------------------------------------------------- */
static CRITICAL_SECTION g_log_lock;

static void host_log(const char *fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_lock);
    static NexLogDay host_day;
    int rotated = nex_log_rotate_a(kHostLog, NEX_LOG_MAX_BYTES);
    FILE *f = fopen(kHostLog, "a");
    if (f) nex_log_day_marker(f, &host_day, &st, rotated);
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

/* 从扁平 JSON 对象读取一个不含转义的短字符串。源规格字段只接受
   playId、编码名等 ASCII 标识，遇到转义或控制字符直接拒绝。 */
static int json_get_string(const char *json, const char *key, char *out, size_t n) {
    char pat[128];
    if (!out || n == 0) return 0;
    out[0] = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != '"') return 0;
    size_t used = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20 || c == '\\' || used + 1 >= n) return 0;
        out[used++] = (char)c;
    }
    if (*p != '"') return 0;
    out[used] = 0;
    return used > 0;
}

static int source_allowed_id(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int source_format_allowed(const char *s) {
    return s && (!_stricmp(s, "flac") || !_stricmp(s, "pcm") ||
                  !_stricmp(s, "wav") || !_stricmp(s, "alac") ||
                  !_stricmp(s, "ape") || !_stricmp(s, "mp3") ||
                  !_stricmp(s, "aac") || !_stricmp(s, "ogg"));
}

static int source_format_is_lossless(const char *s) {
    return s && (!_stricmp(s, "flac") || !_stricmp(s, "pcm") ||
                 !_stricmp(s, "wav") || !_stricmp(s, "alac") ||
                 !_stricmp(s, "ape"));
}

static int source_quality_tier_allowed(int tier) {
    switch (tier) {
    case 0: case 64: case 128: case 192: case 320: case 999:
    case 1999: case 2999: case 3999: case 4999: case 5999: case 6999:
        return 1;
    default:
        return 0;
    }
}

static int source_bits_from_bytes(int bytes) {
    return bytes >= 1 && bytes <= 4 ? bytes * 8 : 0;
}

static void source_uppercase(char *out, size_t n, const char *format) {
    size_t i = 0;
    if (!out || !n) return;
    for (; format && format[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)format[i];
        out[i] = (char)((c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c);
    }
    out[i] = 0;
}

/* ---- 配置：hook.cfg <-> JSON ------------------------------------------- */
/* 顺序固定，面板与日志都依赖这个顺序 */
static const char *kCfgKeys[] = {"enable","bits","rate","buffer","compat","dither","fade_ms","shared_fallback","src_prefer","hifi","audiohost",
                                 "idle_release_ms","auto_shared"};
#define CFG_N ((int)(sizeof(kCfgKeys)/sizeof(kCfgKeys[0])))

/* Same defaults as nex_audio_policy_config_defaults(): bits=0 follows the
   source and the opening fade is off, so a hook.cfg without those keys shows
   the panel what the audio module will actually do. */
static void cfg_defaults(int *v) {
    v[0]=1; v[1]=0; v[2]=0; v[3]=0; v[4]=1; v[5]=2; v[6]=0; v[7]=0; v[8]=0; v[9]=0; v[10]=0;
    v[11]=0; v[12]=0;   /* idle release and automatic shared output: off */
}

/* Values posted by the panel that the audio module would reject fall back to
   the defaults here, so hook.cfg never holds something the panel cannot show. */
static void cfg_sanitize(int *v) {
    int d[CFG_N];
    cfg_defaults(d);
    if (v[1] != 0 && v[1] != 16 && v[1] != 24 && v[1] != 32) v[1] = d[1];
    if (v[5] < 0 || v[5] > 2) v[5] = d[5];
    if (v[6] < 0 || v[6] > 100) v[6] = d[6];
    if (v[8] != 0 && v[8] != 1) v[8] = d[8];
    if (v[9] != 0 && v[9] != 1) v[9] = d[9];
    if (v[10] != 0 && v[10] != 1) v[10] = d[10];
    /* same range as nex_audio_policy_idle_release_ms: 0 or 1 s .. 1 h */
    if (v[11] != 0 && (v[11] < 1000 || v[11] > 3600000)) v[11] = d[11];
    if (v[12] != 0 && v[12] != 1) v[12] = d[12];
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
            size_t val_len = strlen(val);
            while (val_len && (val[val_len - 1] == '\r' || val[val_len - 1] == '\n' ||
                               val[val_len - 1] == ' ' || val[val_len - 1] == '\t'))
                val[--val_len] = 0;
            for (int i = 0; i < CFG_N; i++) {
                if (strcmp(k, kCfgKeys[i]) != 0) continue;
                if (i == 8) {
                    if (_stricmp(val, "simplicity") == 0) v[i] = 1;
                    else if (_stricmp(val, "bandwidth") == 0) v[i] = 0;
                    else v[i] = atoi(val) ? 1 : 0;
                } else v[i] = atoi(val);
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(txt);
    /* Shared output is no longer exposed as a product mode. Keep the legacy
       field readable, but normalize it before it reaches the UI. */
    v[7] = 0;
}

/* True when a hook.cfg line sets one of the keys the panel owns. */
static int cfg_line_is_panel_key(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    for (int i = 0; i < CFG_N; i++) {
        size_t len = strlen(kCfgKeys[i]);
        if (strncmp(line, kCfgKeys[i], len) == 0 && line[len] == '=') return 1;
    }
    return 0;
}

static int cfg_line_has_key(const char *line, const char *key) {
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(key);
    return strncmp(line, key, len) == 0 && line[len] == '=';
}

/* Rewrites hook.cfg: the panel's keys from v, then every other line kept as
   it was (advanced keys set by hand - prime, event, memlock, log ... - used to
   be dropped by the first panel save), with string_key replaced by
   string_value (removed when the value is empty). */
static int cfg_rewrite(const int *v, const char *string_key, const char *string_value) {
    char temp[MAX_PATH];
    int n = snprintf(temp, sizeof(temp), "%s.tmp", kHookCfg);
    if (n < 0 || (size_t)n >= sizeof(temp)) return 0;
    char *old = read_file(kHookCfg);
    FILE *f = fopen(temp, "wb");
    if (!f) { free(old); host_log("cfg: temporary file open failed %s", temp); return 0; }
    for (int i = 0; i < CFG_N; i++) {
        if (i == 8) fprintf(f, "%s=%s\n", kCfgKeys[i], v[i] ? "simplicity" : "bandwidth");
        else fprintf(f, "%s=%d\n", kCfgKeys[i], v[i]);
    }
    for (char *line = old; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        if (len && !cfg_line_is_panel_key(line) &&
            !(string_key && cfg_line_has_key(line, string_key)))
            fprintf(f, "%s\n", line);
        if (!nl) break;
        line = nl + 1;
    }
    free(old);
    if (string_key && string_value && string_value[0])
        fprintf(f, "%s=%s\n", string_key, string_value);
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || !MoveFileExA(temp, kHookCfg, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        host_log("cfg: atomic update failed %s (%lu)", kHookCfg, GetLastError());
        DeleteFileA(temp);
        return 0;
    }
    return 1;
}

static int cfg_write(const int *v) { return cfg_rewrite(v, NULL, NULL); }

/* A string key of hook.cfg, "" when absent. */
static void cfg_read_string(const char *key, char *out, size_t n) {
    out[0] = 0;
    char *txt = read_file(kHookCfg);
    if (!txt) return;
    for (char *line = txt; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (cfg_line_has_key(line, key)) {
            const char *v = strchr(line, '=') + 1;
            snprintf(out, n, "%s", v);
            size_t len = strlen(out);
            while (len && (out[len - 1] == '\r' || out[len - 1] == ' ')) out[--len] = 0;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(txt);
}

/* Endpoint ids are "{0.0.0.00000000}.{guid}"; anything else is refused so a
   posted value can never inject a second hook.cfg line. */
static int endpoint_id_valid(const char *id) {
    size_t len = strlen(id);
    if (len > 190) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '{' || c == '}' || c == '.' || c == '-' || c == '#' || c == '_'))
            return 0;
    }
    return 1;
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
static const char *kUiKeys[] = {
    "banner","charts","daily","podcast","choice","skin","pet",
    "sidebar_recommend","sidebar_featured","sidebar_podcast","sidebar_fm",
    "sidebar_community","sidebar_heartbeat","sidebar_more",
    "main_recommend","main_featured","main_charts","main_recent",
    "main_liked","main_radar","main_blackvinyl","main_context",
    "main_daily","main_podcast","main_books",
    "header_message","header_soundbound","header_skin","header_pet",
    "header_mini","header_setting"
};
#define UI_N ((int)(sizeof(kUiKeys)/sizeof(kUiKeys[0])))

static void ui_read(int *v) {
    for (int i = 0; i < UI_N; i++) v[i] = 0;
    char *txt = read_file(kUiJson);
    if (!txt) return;
    for (int i = 0; i < UI_N; i++) json_get_int(txt, kUiKeys[i], &v[i]);
    free(txt);
}

static int ui_write(const int *v) {
    char temp[MAX_PATH];
    int n = snprintf(temp, sizeof(temp), "%s.tmp", kUiJson);
    if (n < 0 || (size_t)n >= sizeof(temp)) return 0;
    FILE *f = fopen(temp, "wb");
    if (!f) return 0;
    fprintf(f, "{\n");
    for (int i = 0; i < UI_N; i++)
        fprintf(f, "  \"%s\": %d%s\n", kUiKeys[i], v[i], i + 1 < UI_N ? "," : "");
    fprintf(f, "}\n");
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || !MoveFileExA(temp, kUiJson, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp);
        return 0;
    }
    return 1;
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

/* Every active render endpoint, for the panel's output-device picker. The
   chosen id is stored as hook.cfg endpoint=; the audio module opens that
   device and falls back to the client's own device when it is unavailable. */
static void endpoints_json(char *out, size_t n) {
    char current[200];
    cfg_read_string("endpoint", current, sizeof(current));
    size_t used = 0;
    char *cur = json_escape(current);
    used += (size_t)snprintf(out + used, n - used, "{\"current\":\"%s\",\"endpoints\":[",
                             cur ? cur : "");
    free(cur);
    int count = 0, current_listed = 0;
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *list = NULL;
    char default_id[200] = "";
    if (g_com_ready &&
        SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator_l, (void **)&en)) && en) {
        IMMDevice *def = NULL;
        if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eMultimedia, &def)) && def) {
            LPWSTR wid = NULL;
            if (SUCCEEDED(def->lpVtbl->GetId(def, &wid)) && wid) {
                WideCharToMultiByte(CP_UTF8, 0, wid, -1, default_id, (int)sizeof(default_id), NULL, NULL);
                CoTaskMemFree(wid);
            }
            def->lpVtbl->Release(def);
        }
        if (SUCCEEDED(en->lpVtbl->EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &list)) && list) {
            UINT total = 0;
            list->lpVtbl->GetCount(list, &total);
            for (UINT i = 0; i < total && used + 600 < n; i++) {
                IMMDevice *dev = NULL;
                if (FAILED(list->lpVtbl->Item(list, i, &dev)) || !dev) continue;
                char id[200] = "", name[256] = "";
                LPWSTR wid = NULL;
                if (SUCCEEDED(dev->lpVtbl->GetId(dev, &wid)) && wid) {
                    WideCharToMultiByte(CP_UTF8, 0, wid, -1, id, (int)sizeof(id), NULL, NULL);
                    CoTaskMemFree(wid);
                }
                IPropertyStore *ps = NULL;
                if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
                        pv.vt == VT_LPWSTR && pv.pwszVal)
                        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, (int)sizeof(name), NULL, NULL);
                    PropVariantClear(&pv);
                    ps->lpVtbl->Release(ps);
                }
                dev->lpVtbl->Release(dev);
                if (!id[0] || !endpoint_id_valid(id)) continue;
                if (strcmp(id, current) == 0) current_listed = 1;
                char *ename = json_escape(name[0] ? name : id);
                used += (size_t)snprintf(out + used, n - used,
                                         "%s{\"id\":\"%s\",\"name\":\"%s\",\"default\":%s}",
                                         count ? "," : "", id, ename ? ename : "",
                                         strcmp(id, default_id) == 0 ? "true" : "false");
                free(ename);
                count++;
            }
            list->lpVtbl->Release(list);
        }
        en->lpVtbl->Release(en);
    }
    snprintf(out + used, n - used, "],\"currentAvailable\":%s}",
             current[0] && !current_listed ? "false" : "true");
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

static unsigned long long line_ull(const char *line, const char *key) {
    const char *p = strstr(line ? line : "", key);
    if (!p) return 0;
    p += strlen(key);
    return _strtoui64(p, NULL, 10);
}

static void verification_json(char *out, size_t n, const char *line) {
    /* No verifier line means no measurement. Returning zeroed counters here
       makes the panel present an empty measurement as if it were live data. */
    if (!line || !*line) {
        snprintf(out, n, "\"verification\":null");
        return;
    }
    char *escaped = json_escape(line);
    const char *map = strstr(line ? line : "", "map=");
    char map_name[32] = "";
    if (map) sscanf(map + 4, "%31s", map_name);
    unsigned long long client_frames = line_ull(line, "client_frames=");
    unsigned long long endpoint_frames = line_ull(line, "endpoint_frames=");
    unsigned long long verified_frames = line_ull(line, "verified_frames=");
    unsigned long long changed_bytes = line_ull(line, "changed_bytes=");
    unsigned long long dropped_frames = line_ull(line, "dropped_frames=");
    unsigned long long underrun_events = line_ull(line, "underrun_events=");
    int exact = map_name[0] && strcmp(map_name, "client-native") == 0
             && client_frames == endpoint_frames
             && verified_frames == client_frames
             && changed_bytes == 0;
    const char *state = exact ? "bit-perfect" :
                        (strcmp(map_name, "converted") == 0 ? "converted" : "unverified");
    snprintf(out, n,
             "\"verification\":{\"source\":\"unverified\",\"clientToEndpoint\":\"%s\",\"clientFrames\":%llu,\"endpointFrames\":%llu,\"verifiedFrames\":%llu,\"changedBytes\":%llu,\"droppedFrames\":%llu,\"underrunEvents\":%llu,\"line\":\"%s\"}",
             state,
             client_frames, endpoint_frames, verified_frames, changed_bytes,
             dropped_frames, underrun_events,
             escaped ? escaped : "");
    free(escaped);
}

/* 控制块的最新一份快照（监视线程每 500 ms 写一次），给状态接口和托盘提示用。
   SRWLOCK 静态初始化，测试里不经 main 也能用。 */
static SRWLOCK g_snap_lock = SRWLOCK_INIT;
static NexControl g_ctl_snap;
static int g_ctl_snap_ok;          /* 有快照且模块在 3 s 内更新过 */

/* ---- 状态：解析 sink.log 末段 ------------------------------------------ */
static void status_json(char *out, size_t n) {
    char *tail = read_tail(kSinkLog, 12288);

    char engaged[512] = "";
    char lastline[256] = "";
    char fault[512] = "";
    char verifier[1024] = "";
    char remote_status[2048] = "";
    char log_ep[256] = "";
    int  log_hw = -1;
    /* How the device side is open right now. The ENGAGED line only covers a
       fresh stream; a resume after an idle release, or a switch between
       shared and exclusive, is a retarget line. */
    char output_mode[16] = "";
    char output_note[256] = "";

    char *line = tail;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        {   /* 日志是 CRLF，行尾的 \r 不该出现在 JSON 里 */
            size_t L = strlen(line);
            while (L && (line[L-1] == '\r' || line[L-1] == '\n')) line[--L] = 0;
        }
        if (strstr(line, "=== nexsink attached ===")) {
            /* A new client process has not negotiated a stream yet. Do not
               present the previous process's format and verifier as current. */
            engaged[0] = 0;
            verifier[0] = 0;
            fault[0] = 0;
            remote_status[0] = 0;
            log_ep[0] = 0;
            log_hw = -1;
            output_mode[0] = 0;
            output_note[0] = 0;
        }
        if (strstr(line, "sink ENGAGED")) {
            snprintf(engaged, sizeof(engaged), "%s", line);
            ep_from_line(line, log_ep, sizeof(log_ep), &log_hw);
            snprintf(output_mode, sizeof(output_mode), "%s",
                     strstr(line, "output=shared") ? "shared" : "exclusive");
            output_note[0] = 0;
        }
        if (strstr(line, "retarget(") && strstr(line, " output=")) {
            snprintf(output_mode, sizeof(output_mode), "%s",
                     strstr(line, "output=shared") ? "shared" : "exclusive");
        }
        if (strstr(line, "idle release: ")) {
            snprintf(output_mode, sizeof(output_mode), "released");
            output_note[0] = 0;
        }
        if (strstr(line, "output(") &&
            (strstr(line, "exclusive takes the device from ") || strstr(line, ": shared"))) {
            const char *o = strstr(line, "output(");
            snprintf(output_note, sizeof(output_note), "%s", o);
        }
        if (strstr(line, "sink failed")) {
            snprintf(fault, sizeof(fault), "%s", line);
        }
        if (strstr(line, "hifi verifier:")) {
            snprintf(verifier, sizeof(verifier), "%s", line);
        }
        if (strstr(line, "audiohost: close") ||
            strstr(line, "audiohost: ready") ||
            strstr(line, "audiohost: host_exit") ||
            strstr(line, "audiohost telemetry generation=") ||
            strstr(line, "telemetry generation=")) {
            snprintf(remote_status, sizeof(remote_status), "%s", line);
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
    char verification[1800];
    verification_json(verification, sizeof(verification), verifier);
    int cfg[CFG_N];
    cfg_read(cfg);
    int requested_audiohost = cfg[10] != 0;
    int active_audiohost = strstr(engaged, "engine=audiohost") != NULL;
    unsigned long long generation = line_ull(engaged, "generation=");
    if (!generation) generation = line_ull(remote_status, "generation=");
    unsigned long long converted = line_ull(remote_status, "converted=");
    unsigned long long source_waits = line_ull(remote_status, "source_waits=");
    unsigned long long backpressure = line_ull(remote_status, "backpressure=");
    unsigned long long underruns = line_ull(remote_status, "underrun=");
    unsigned long long dropped = line_ull(remote_status, "dropped=");
    unsigned long long eos_submitted = line_ull(remote_status, "eos_submitted=");
    unsigned long long eos_complete = line_ull(remote_status, "eos_complete=");
    unsigned long long ring_used = line_ull(remote_status, "ring_used=");
    unsigned long long ring_capacity = line_ull(remote_status, "ring_capacity=");
    unsigned long long device_padding = line_ull(remote_status, "device_padding=");
    unsigned long long latency_ms = line_ull(remote_status, "latency_ms=");
    unsigned long long write_waits = line_ull(remote_status, "write_waits=");
    unsigned long long write_wait_max = line_ull(remote_status, "write_wait_max_ms=");
    {   /* 控制块比日志新：有新鲜快照就以它为准 */
        AcquireSRWLockExclusive(&g_snap_lock);
        if (g_ctl_snap_ok) {
            const char *m = g_ctl_snap.state == NEX_CONTROL_STATE_EXCLUSIVE ? "exclusive"
                          : g_ctl_snap.state == NEX_CONTROL_STATE_SHARED ? "shared"
                          : g_ctl_snap.state == NEX_CONTROL_STATE_RELEASED ? "released" : "";
            if (*m) snprintf(output_mode, sizeof(output_mode), "%s", m);
        }
        ReleaseSRWLockExclusive(&g_snap_lock);
    }
    char *rs = json_escape(remote_status);
    char *on = json_escape(output_note);
    source_refresh_probe();
    if (!_stricmp(g_source.status, "pending") && !g_source.probe_started_ms &&
        g_source.updated_at_ms && GetTickCount64() - g_source.updated_at_ms > 30000) {
        source_clear();
    }
    char *source_play_id = json_escape(g_source.play_id);
    char *source_format = json_escape(g_source.format);
    char *source_encoding = json_escape(g_source.encoding);
    const char *source_status = g_source.status[0] ? g_source.status : "unavailable";
    snprintf(out, n,
             "{\"loaded\":%s,\"lastEngaged\":\"%s\",\"lastLine\":\"%s\","
             "\"fault\":\"%s\",\"requestedEngine\":\"wasapi\","
             "\"requestedDeployment\":\"%s\","
             "\"activeEngine\":\"%s\",\"deployment\":\"%s\","
             "\"convertedFrames\":%llu,\"sourceWaits\":%llu,"
             "\"backpressure\":%llu,\"underrunEvents\":%llu,"
             "\"droppedFrames\":%llu,\"currentGeneration\":%llu,"
             "\"eosSubmittedGeneration\":%llu,\"eosCompleteGeneration\":%llu,"
             "\"ringUsedFrames\":%llu,\"ringCapacityFrames\":%llu,"
             "\"devicePaddingFrames\":%llu,\"remoteLatencyMs\":%llu,"
             "\"writeWaits\":%llu,\"writeWaitMaxMs\":%llu,"
             "\"remoteStatus\":\"%s\",\"outputMode\":\"%s\",\"outputNote\":\"%s\",%s,%s,"
             "\"source_status\":\"%s\",\"source_play_id\":\"%s\","
             "\"source_format\":\"%s\",\"source_rate\":%d,"
             "\"source_channels\":%d,\"source_sample_bytes\":%d,"
             "\"source_valid_bits\":%d,\"source_container_bits\":%d,"
             "\"source_encoding\":\"%s\",\"source_lossless\":%s,"
             "\"source_bitrate_kbps\":%d,\"source_quality_tier\":%d,"
             "\"source_quality_tier_hint\":%d,\"source_selection\":\"%s\","
             "\"source_audio_generation\":%llu,\"source_updated_at_ms\":%llu}",
             engaged[0] || lastline[0] ? "true" : "false",
             e ? e : "", l ? l : "", f ? f : "",
             requested_audiohost ? "audiohost" : "in_process",
             active_audiohost ? "wasapi" : "wasapi",
             active_audiohost ? "audiohost" : "in_process",
             converted, source_waits, backpressure, underruns, dropped, generation,
             eos_submitted, eos_complete,
             ring_used, ring_capacity, device_padding, latency_ms, write_waits, write_wait_max,
             rs ? rs : "", output_mode, on ? on : "", ep, verification,
             source_status, source_play_id ? source_play_id : "",
             source_format ? source_format : "", g_source.rate, g_source.channels,
             g_source.sample_bytes, g_source.valid_bits, g_source.container_bits,
             source_encoding ? source_encoding : "",
             g_source.lossless ? "true" : (source_status_has_metadata(g_source.status) ? "false" : "null"),
             g_source.bitrate_kbps, g_source.quality_tier,
             g_source.quality_tier_hint, g_source.selection,
             (unsigned long long)g_source.audio_generation,
             (unsigned long long)g_source.updated_at_ms);
    free(e); free(l); free(f); free(rs); free(on); free(source_play_id); free(source_format); free(source_encoding);
}

static ULONGLONG source_current_generation(void) {
    char *tail = read_tail(kSinkLog, 8192);
    ULONGLONG generation = 0;
    char *line = tail;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strstr(line, "sink ENGAGED")) generation = line_ull(line, "generation=");
        if (!nl) break;
        line = nl + 1;
    }
    free(tail);
    return generation;
}

#include "host_source.inc"   /* 源规格：读网易云 .uc 缓存头 */

/* ---- 进程 --------------------------------------------------------------- */
typedef struct {
    DWORD pid;
    HWND first;
    HWND visible;
} client_window_probe;

static BOOL CALLBACK probe_client_window(HWND hwnd, LPARAM param) {
    client_window_probe *probe = (client_window_probe *)param;
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != probe->pid) return TRUE;
    if (!probe->first) probe->first = hwnd;
    if (IsWindowVisible(hwnd)) {
        probe->visible = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND client_window(DWORD pid, int visible_only) {
    client_window_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.pid = pid;
    EnumWindows(probe_client_window, (LPARAM)&probe);
    return visible_only ? probe.visible : (probe.visible ? probe.visible : probe.first);
}

static int client_window_visible(DWORD pid) {
    return client_window(pid, 1) != NULL;
}

static int reveal_client_window(DWORD pid) {
    HWND hwnd = client_window(pid, 0);
    if (!hwnd) return 0;
    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    ShowWindowAsync(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    host_log("已请求显示网易云窗口 pid=%lu hwnd=0x%p", (unsigned long)pid, (void *)hwnd);
    return client_window_visible(pid);
}

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

    /* 网易云的 Shell 启动流程会先留下一个无窗口的启动进程，再创建
       持有主窗口的 cloudmusic.exe。优先选有可见顶层窗口的实例，避免把
       DLL 加到启动器或后台子进程里。 */
    for (int i = 0; i < n; i++)
        if (client_window_visible(pids[i])) return pids[i];

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

/* host.cfg 的 cdp=1 才带调试端口启动客户端。面板正常由 nexui.dll 在渲染进程里
   加载，CDP 只是它失败时的备用；端口开着的时候，本机任何进程都能在客户端页面里
   执行脚本、拿到登录状态，所以默认关。设置随时可以从托盘的「设置…」改。 */
static int g_cdp_enabled;

static int launch_client(void) {
    WCHAR params[256];
    if (g_cdp_enabled)
        _snwprintf(params, sizeof(params) / sizeof(params[0]),
                   L"--remote-debugging-port=%d", CDP_PORT);
    else
        params[0] = 0;
    params[(sizeof(params) / sizeof(params[0])) - 1] = 0;

    /* ShellExecuteExW 走网易云正常的启动引导，同时保留 CEF 调试端口。
       当前版本实测这条路径既创建可见主窗口，也让 9333 监听；窗口显示
       与页面脚本注入因此回到同一条启动链。 */
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = kClientExe;
    sei.lpParameters = params[0] ? params : NULL;
    sei.lpDirectory = kClientDir;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        host_log("启动客户端失败 %lu", GetLastError());
        return 0;
    }
    DWORD launched = sei.hProcess ? GetProcessId(sei.hProcess) : 0;
    host_log("已启动客户端 pid=%lu（Shell 启动）", (unsigned long)launched);
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return 1;
}

/* 入口命令和托盘菜单都走这里：先把已有的后台实例尝试显示出来，
   没有可用窗口时才启动一份新的客户端。这样后台 cloudmusic.exe 不会
   把“打开网易云音乐”这件事挡住。 */
static int open_client_window(void) {
    DWORD pid = find_main_client();
    if (pid) {
        if (reveal_client_window(pid)) {
            host_log("复用现有网易云主进程 pid=%lu", (unsigned long)pid);
            return 1;
        }
        host_log("已有网易云主进程但没有可枚举窗口，尝试一次客户端启动");
        return launch_client();
    }
    return launch_client();
}

/* ---- 注入 --------------------------------------------------------------- */
static int enable_debug_privilege(void) {
    HANDLE token = NULL;
    TOKEN_PRIVILEGES privileges;
    LUID luid;
    int ok = 0;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        host_log("SeDebugPrivilege: OpenProcessToken 失败 %lu", GetLastError());
        return 0;
    }
    if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &luid)) {
        memset(&privileges, 0, sizeof(privileges));
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                                  NULL, NULL) && GetLastError() == ERROR_SUCCESS)
            ok = 1;
    }
    if (!ok) host_log("SeDebugPrivilege 未启用，错误 %lu", GetLastError());
    else host_log("SeDebugPrivilege 已启用");
    CloseHandle(token);
    return ok;
}

static int process_bitness_compatible(HANDLE target) {
    typedef BOOL (WINAPI *IsWow64Process2Fn)(HANDLE, USHORT *, USHORT *);
    IsWow64Process2Fn is_wow64_process2 = (IsWow64Process2Fn)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (is_wow64_process2) {
        USHORT host_process = IMAGE_FILE_MACHINE_UNKNOWN, host_native = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT target_process = IMAGE_FILE_MACHINE_UNKNOWN, target_native = IMAGE_FILE_MACHINE_UNKNOWN;
        if (is_wow64_process2(GetCurrentProcess(), &host_process, &host_native) &&
            is_wow64_process2(target, &target_process, &target_native)) {
            if (host_native != target_native) return 0;
            if (host_process == IMAGE_FILE_MACHINE_UNKNOWN &&
                target_process != IMAGE_FILE_MACHINE_UNKNOWN) return 0;
            if (host_process != IMAGE_FILE_MACHINE_UNKNOWN &&
                target_process == IMAGE_FILE_MACHINE_UNKNOWN) return 0;
            return 1;
        }
    }

    BOOL host_wow64 = FALSE, target_wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &host_wow64) &&
        IsWow64Process(target, &target_wow64)) {
#if defined(_WIN64)
        return !target_wow64;
#else
        return target_wow64;
#endif
    }
    return 1;
}

/* What a remote LoadLibrary told us. A thread that has not returned may still
   be reading the path buffer, so that buffer must not be freed, and STILL_ACTIVE
   is not a module handle. A module whose base has zero low 32 bits returns 0
   from the 32-bit exit code, so the module list has the last word. */
static int inject_outcome(DWORD wait, DWORD exit_code, int module_present, int *may_free) {
    if (wait != WAIT_OBJECT_0) {
        if (may_free) *may_free = 0;
        return module_present;
    }
    if (may_free) *may_free = 1;
    return exit_code != 0 || module_present;
}

static int module_loaded(DWORD pid, const wchar_t *name);

static int inject_dll(DWORD pid, const char *dll) {
    size_t len = strlen(dll) + 1;
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) { host_log("注入: OpenProcess(%lu) 失败 %lu", (unsigned long)pid, GetLastError()); return 0; }

    if (!process_bitness_compatible(h)) {
        host_log("注入: 拒绝位数不匹配 pid=%lu", (unsigned long)pid);
        CloseHandle(h);
        return 0;
    }

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
    DWORD wait = WaitForSingleObject(th, 8000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    wchar_t base[MAX_PATH];
    const char *slash = strrchr(dll, '\\');
    MultiByteToWideChar(CP_ACP, 0, slash ? slash + 1 : dll, -1, base, MAX_PATH);
    int may_free = 0;
    int ok = inject_outcome(wait, code, module_loaded(pid, base), &may_free);
    if (may_free) VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    if (wait != WAIT_OBJECT_0)
        host_log("注入 pid=%lu：LoadLibraryA 8 秒内未返回，%s", (unsigned long)pid,
                 ok ? "模块已在进程中" : "按失败处理，路径缓冲保留给仍在运行的线程");
    else
        host_log("注入 pid=%lu -> LoadLibraryA=0x%lX", (unsigned long)pid, (unsigned long)code);
    return ok;
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

/* ---- 界面链路：渲染进程里的 nexui.dll ------------------------------------ */
/* Audio and panel are separate chains. The watchdog puts nexsink.dll into the
   main process. This thread puts nexui.dll into every child of it; the module
   acts only in the renderer and announces Ready or Unsupported through named
   events (nexui_shared.h). The route it derives decides whether the watchdog
   also injects the panel over CDP. */
static int find_child_pids(DWORD parent, const wchar_t *exe, DWORD *out, int cap) {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID != parent || _wcsicmp(pe.szExeFile, exe) != 0) continue;
            if (n < cap) out[n] = pe.th32ProcessID;
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n < cap ? n : cap;
}

static int ui_event_exists(const wchar_t *kind, DWORD pid) {
    wchar_t name[64];
    HANDLE h;
    if (!nex_ui_event_name(name, 64, kind, (unsigned long)pid)) return 0;
    h = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

#define UI_MAX_CHILDREN 32
typedef struct UiChild { DWORD pid; int injected; int failures; int skip; } UiChild;
typedef struct UiTracker {
    DWORD main_pid;
    UiChild child[UI_MAX_CHILDREN];
    int count;
    ULONGLONG client_seen_at;
    ULONGLONG first_injected_at;
} UiTracker;

/* A different main process is a different client: start over. Children that
   are gone drop out; new ones join uninjected. */
static void ui_tracker_sync(UiTracker *t, DWORD main_pid, const DWORD *children, int n,
                            ULONGLONG now) {
    int i, j, w = 0;
    if (t->main_pid != main_pid) {
        memset(t, 0, sizeof(*t));
        t->main_pid = main_pid;
        t->client_seen_at = now;
    }
    for (i = 0; i < t->count; i++) {
        int alive = 0;
        for (j = 0; j < n; j++) if (children[j] == t->child[i].pid) { alive = 1; break; }
        if (alive) t->child[w++] = t->child[i];
    }
    t->count = w;
    for (j = 0; j < n && t->count < UI_MAX_CHILDREN; j++) {
        int known = 0;
        for (i = 0; i < t->count; i++) if (t->child[i].pid == children[j]) { known = 1; break; }
        if (known) continue;
        t->child[t->count].pid = children[j];
        t->child[t->count].injected = 0;
        t->child[t->count].failures = 0;
        t->child[t->count].skip = 0;
        t->count++;
    }
}

static void ui_tracker_route_input(const UiTracker *t, int dll_present, ULONGLONG now,
                                   NexUiRouteInput *out) {
    int i;
    memset(out, 0, sizeof(*out));
    out->dll_present = dll_present;
    for (i = 0; i < t->count; i++) {
        if (t->child[i].skip) continue;
        if (t->child[i].injected) out->renderer_injected = 1;
        if (ui_event_exists(L"Ready", t->child[i].pid)) out->renderer_ready = 1;
        if (ui_event_exists(L"Unsupported", t->child[i].pid)) out->renderer_unsupported = 1;
        if (t->child[i].failures > out->inject_failures) out->inject_failures = t->child[i].failures;
    }
    if (t->client_seen_at && now > t->client_seen_at)
        out->ms_since_client_seen = (unsigned long)(now - t->client_seen_at);
    if (t->first_injected_at && now > t->first_injected_at)
        out->ms_since_renderer_injected = (unsigned long)(now - t->first_injected_at);
}

static volatile LONG g_ui_route = NEX_UI_ROUTE_WAIT;

/* Fast only while waiting for a renderer: a CDP route can last the whole
   client session (nexui.disable, an unsupported CEF), and 1 s is still quick
   enough to notice a renderer that becomes ready late. */
static DWORD ui_poll_interval_ms(int route) {
    return route == NEX_UI_ROUTE_WAIT ? 100 : 1000;
}

/* ---- one round of injection ------------------------------------------------ */
/* Process operations behind a table so the round's rules are testable. */
typedef struct UiOps {
    int (*loaded)(void *ctx, DWORD pid);              /* nexui.dll already in pid  */
    int (*inject)(void *ctx, DWORD pid);              /* load nexui.dll into pid   */
    int (*has_client_children)(void *ctx, DWORD pid); /* pid is itself a main      */
    void *ctx;
} UiOps;

/* Each child is tried until it holds nexui.dll or reaches the failure cap. A
   child that has cloudmusic.exe children of its own is a main process seen
   through the shell launcher: it hosts the audio module and is left alone. */
static void ui_round_inject(UiTracker *t, const UiOps *ops, ULONGLONG now) {
    int i;
    for (i = 0; i < t->count; i++) {
        UiChild *c = &t->child[i];
        if (c->skip || c->injected || c->failures >= NEX_UI_INJECT_MAX_FAILURES) continue;
        if (ops->has_client_children(ops->ctx, c->pid)) { c->skip = 1; continue; }
        if (ops->loaded(ops->ctx, c->pid) || ops->inject(ops->ctx, c->pid)) {
            c->injected = 1;
            if (!t->first_injected_at) t->first_injected_at = now;
        } else {
            c->failures++;
        }
    }
}

static int ui_op_loaded(void *ctx, DWORD pid) { (void)ctx; return module_loaded(pid, L"nexui.dll"); }
static int ui_op_inject(void *ctx, DWORD pid) { (void)ctx; return inject_dll(pid, kUiDll); }
static int ui_op_has_client_children(void *ctx, DWORD pid) {
    DWORD one[1];
    (void)ctx;
    return find_child_pids(pid, L"cloudmusic.exe", one, 1) > 0;
}

/* ---- crash-loop guard ------------------------------------------------------- */
/* In this client a dead renderer takes the whole client down and the watchdog
   relaunches it. If nexui.dll were the cause, every relaunch would inject it
   again. Two clients in a row that end within the window after their first
   injection stop nexui.dll for the rest of this host session. */
#define UI_GUARD_WINDOW_MS 15000
#define UI_GUARD_LIMIT 2
typedef struct UiGuard { int quick_exits; int tripped; } UiGuard;

/* Returns 1 on the call that trips the guard. */
static int ui_guard_client_gone(UiGuard *g, ULONGLONG first_injected_at, ULONGLONG gone_at) {
    if (!first_injected_at) return 0;
    if (gone_at >= first_injected_at && gone_at - first_injected_at < UI_GUARD_WINDOW_MS)
        g->quick_exits++;
    else
        g->quick_exits = 0;
    if (!g->tripped && g->quick_exits >= UI_GUARD_LIMIT) {
        g->tripped = 1;
        return 1;
    }
    return 0;
}

/* ---- route reasons ------------------------------------------------------------ */
enum { UI_REASON_NONE = 0, UI_REASON_MISSING, UI_REASON_UNSUPPORTED, UI_REASON_INJECT_FAILED,
       UI_REASON_TIMEOUT, UI_REASON_GUARD };

static int ui_route_reason_code(const NexUiRouteInput *in, int guard_tripped) {
    if (guard_tripped) return UI_REASON_GUARD;
    if (!in->dll_present) return UI_REASON_MISSING;
    if (in->renderer_unsupported) return UI_REASON_UNSUPPORTED;
    if (in->inject_failures >= NEX_UI_INJECT_MAX_FAILURES) return UI_REASON_INJECT_FAILED;
    return UI_REASON_TIMEOUT;
}

static const char *ui_route_reason_text(int code) {
    switch (code) {
    case UI_REASON_GUARD: return "客户端两次在注入 nexui.dll 后很快退出，本次运行已停用 nexui.dll";
    case UI_REASON_MISSING: return "缺少 nexui.dll";
    case UI_REASON_UNSUPPORTED: return "渲染进程不支持（见 nexui.log）";
    case UI_REASON_INJECT_FAILED: return "注入渲染进程失败";
    default: return "20 秒内渲染进程未就绪";
    }
}

/* Second half of the watchdog's "panel unavailable" line when CDP is out too. */
static const char *ui_cdp_unavailable_hint(int code) {
    if (code == UI_REASON_TIMEOUT)
        return "nexui.dll 仍在等待页面的下一次调用，面板可能稍后出现，操作一下界面会更快";
    return "需要设置面板时，退出网易云后由 NexExclusive 重新拉起";
}

static volatile LONG g_ui_reason = UI_REASON_NONE;

static DWORD WINAPI ui_thread(LPVOID unused) {
    static UiTracker tracker;
    static UiGuard guard;
    const UiOps ops = {ui_op_loaded, ui_op_inject, ui_op_has_client_children, NULL};
    int last_route = -1;
    (void)unused;
    for (;;) {
        DWORD kids[UI_MAX_CHILDREN];
        DWORD main_pid = find_main_client();
        int n, route, reason, dll_present;
        NexUiRouteInput in;
        if (tracker.main_pid && tracker.main_pid != main_pid &&
            ui_guard_client_gone(&guard, tracker.first_injected_at, GetTickCount64()))
            host_log("界面链路：%s，改走 CDP", ui_route_reason_text(UI_REASON_GUARD));
        if (!main_pid) {
            memset(&tracker, 0, sizeof(tracker));
            InterlockedExchange(&g_ui_route, NEX_UI_ROUTE_WAIT);
            last_route = -1;
            Sleep(1000);
            continue;
        }
        n = find_child_pids(main_pid, L"cloudmusic.exe", kids, UI_MAX_CHILDREN);
        ui_tracker_sync(&tracker, main_pid, kids, n, GetTickCount64());
        dll_present = !guard.tripped && GetFileAttributesA(kUiDll) != INVALID_FILE_ATTRIBUTES;
        if (dll_present) ui_round_inject(&tracker, &ops, GetTickCount64());
        ui_tracker_route_input(&tracker, dll_present, GetTickCount64(), &in);
        route = nex_ui_route_decide(&in);
        reason = route == NEX_UI_ROUTE_CDP ? ui_route_reason_code(&in, guard.tripped) : UI_REASON_NONE;
        InterlockedExchange(&g_ui_reason, reason);
        if (route != last_route) {
            if (route == NEX_UI_ROUTE_RENDERER)
                host_log("界面链路：渲染进程就绪，面板由 nexui.dll 加载（客户端 pid=%lu）",
                         (unsigned long)main_pid);
            else if (route == NEX_UI_ROUTE_CDP)
                host_log("界面链路：回退到 CDP（%s）", ui_route_reason_text(reason));
            else
                host_log("界面链路：等待渲染进程（客户端 pid=%lu）", (unsigned long)main_pid);
            last_route = route;
        }
        InterlockedExchange(&g_ui_route, route);
        Sleep(ui_poll_interval_ms(route));
    }
    return 0;
}

#include "host_cdp.inc"   /* CDP：最小 HTTP 客户端与 WebSocket */

/* ---- HTTP 服务 ---------------------------------------------------------- */
static void send_all(SOCKET c, const char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int k = send(c, buf + sent, (int)(n - sent), 0);
        if (k <= 0) return;
        sent += (size_t)k;
    }
}

static int http_header_value(const char *req, const char *name, char *out, size_t n) {
    size_t name_len = strlen(name);
    const char *line = req;
    if (!out || n == 0) return 0;
    out[0] = 0;
    while (line && *line) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > name_len &&
            _strnicmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            size_t len = (size_t)(end - value);
            while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) len--;
            if (len >= n) len = n - 1;
            memcpy(out, value, len);
            out[len] = 0;
            return 1;
        }
        line = end + 2;
    }
    return 0;
}

static int http_allowed_host(const char *host) {
    return _stricmp(host, "127.0.0.1:9334") == 0 ||
           _stricmp(host, "localhost:9334") == 0 ||
           _stricmp(host, "127.0.0.1") == 0 ||
           _stricmp(host, "localhost") == 0;
}

static int http_json_content_type(const char *content_type) {
    const char *semi = strchr(content_type, ';');
    size_t len = semi ? (size_t)(semi - content_type) : strlen(content_type);
    while (len && (content_type[len - 1] == ' ' || content_type[len - 1] == '\t')) len--;
    return len == strlen("application/json") &&
           _strnicmp(content_type, "application/json", len) == 0;
}

static void http_reply(SOCKET c, int code, const char *body) {
    char head[512];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: orpheus://orpheus\r\n"
        "Vary: Origin\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : (code == 403 ? "Forbidden" : "Bad Request"),
        (unsigned)strlen(body));
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

        char origin[256] = "", host[256] = "", content_type[128] = "";
        int has_origin = http_header_value(req, "Origin", origin, sizeof(origin));
        int has_host = http_header_value(req, "Host", host, sizeof(host));
        int has_content_type = http_header_value(req, "Content-Type", content_type, sizeof(content_type));
        int origin_ok = !has_origin || strcmp(origin, "orpheus://orpheus") == 0;
        int host_ok = !has_host || http_allowed_host(host);
        int content_ok = strcmp(method, "POST") != 0 ||
                         (has_content_type && http_json_content_type(content_type));

        char body_buf[65536];
        body_buf[0] = 0;
        char *hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) snprintf(body_buf, sizeof(body_buf), "%s", hdr_end + 4);

        char out[65536];
        if (!origin_ok || !host_ok) {
            http_reply(c, 403, "{\"error\":\"origin or host rejected\"}");
        } else if (!content_ok) {
            http_reply(c, 400, "{\"error\":\"POST requires application/json\"}");
        } else if (strcmp(method, "OPTIONS") == 0) {
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
            cfg_sanitize(v);
            if (!cfg_write(v)) http_reply(c, 500, "{\"error\":\"config write failed\"}");
            else {
                cfg_to_json(v, out, sizeof(out));
                http_reply(c, 200, out);
            }
        } else if (strcmp(path, "/api/endpoints") == 0 && strcmp(method, "GET") == 0) {
            endpoints_json(out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/endpoint") == 0 && strcmp(method, "POST") == 0) {
            char id[200] = "";
            int v[CFG_N];
            /* an empty id means "follow the client's device" */
            int has_id = json_get_string(body_buf, "id", id, sizeof(id)) ||
                         strstr(body_buf, "\"id\":\"\"") != NULL;
            if (!has_id || !endpoint_id_valid(id)) {
                http_reply(c, 400, "{\"error\":\"invalid endpoint id\"}");
            } else {
                cfg_read(v);
                if (!cfg_rewrite(v, "endpoint", id)) http_reply(c, 500, "{\"error\":\"config write failed\"}");
                else {
                    host_log("cfg: output endpoint %s", id[0] ? id : "(follow client)");
                    endpoints_json(out, sizeof(out));
                    http_reply(c, 200, out);
                }
            }
        } else if (strcmp(path, "/api/ui") == 0 && strcmp(method, "GET") == 0) {
            int v[UI_N];
            ui_read(v);
            ui_to_json(v, out, sizeof(out));
            http_reply(c, 200, out);
        } else if (strcmp(path, "/api/ui") == 0 && strcmp(method, "POST") == 0) {
            int v[UI_N];
            ui_read(v);
            for (int i = 0; i < UI_N; i++) json_get_int(body_buf, kUiKeys[i], &v[i]);
            if (!ui_write(v)) http_reply(c, 500, "{\"error\":\"ui write failed\"}");
            else {
                ui_to_json(v, out, sizeof(out));
                http_reply(c, 200, out);
            }
        } else if (strcmp(path, "/api/restart") == 0 && strcmp(method, "POST") == 0) {
            if (restart_current_track()) http_reply(c, 200, "{\"ok\":true}");
            else http_reply(c, 503, "{\"error\":\"playback page unavailable\"}");
        } else if (strcmp(path, "/api/source") == 0 && strcmp(method, "POST") == 0) {
            int source_code = source_update_from_json(body_buf);
            if (source_code == 200) http_reply(c, 200, "{\"ok\":true}");
            else if (source_code == 409) http_reply(c, 409, "{\"error\":\"source playId or fields rejected\"}");
            else http_reply(c, 400, "{\"error\":\"invalid source metadata\"}");
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
#include "host_install.inc"   /* 安装 / 卸载 / 校验发布包 */

/* ---- 客户端版本 --------------------------------------------------------- */
/* 注入靠 IAudioClient / IMMDevice 的 vtable 槽位，客户端大版本更新有可能改动
   布局或模块加载顺序。这里不做硬门槛（拦下来用户也没法用），只是让日志说清楚
   「这个版本我们没验过」，而不是让用户对着「没效果」自己猜。 */
static const wchar_t *kVerified[] = {
    L"3.1.42.205594",
    L"3.1.43.205627",   /* 2026-11-19/20: exclusive output, nexui panel, 6c, PCM capture */
};
static int client_version_known(const wchar_t *ver) {
    if (!ver || !ver[0]) return 0;
    for (size_t i = 0; i < sizeof(kVerified) / sizeof(kVerified[0]); i++)
        if (_wcsicmp(ver, kVerified[i]) == 0) return 1;
    return 0;
}
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
    int known = client_version_known(ver);
    char v8[64];
    WideCharToMultiByte(CP_ACP, 0, ver, -1, v8, sizeof(v8), NULL, NULL);
    if (known) host_log("客户端版本 %s（已验证）", v8);
    else host_log("客户端版本 %s 未经验证——注入可能失效。若出现没声音/没效果，"
                  "先怀疑版本差异，而不是设备或配置。", v8);
    g_client_verified = known;
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
    printf("  NexExclusive.exe --show               显示/呼出网易云窗口并继续注入\n");
    printf("  NexExclusive.exe --install            安装到 %%LOCALAPPDATA%%\\NexExclusive\n");
    printf("  NexExclusive.exe --install --client <路径>\n");
    printf("                                        安装时指定 cloudmusic.exe\n");
    printf("  NexExclusive.exe --verify             校验当前目录的 manifest.txt\n");
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
#define MENU_SHARE    5
#define MENU_FORGET   6
#define MENU_SETTINGS 7

static NOTIFYICONDATAW g_nid;
static int g_tray_up = 0;

static DWORD g_injected_pid;
static char  g_last_page[128];
static int   g_ui_injected;
static DWORD g_last_probe;
static DWORD g_last_ui_time;
static DWORD g_last_restart_attempt;
static unsigned g_cdp_miss_probes;

/* Audio and panel are separate paths. The audio module is injected into
   whatever client is running. The panel needs Chromium's debugging port, which
   an already-running client does not open after the fact; a client without it
   keeps playing through the module and simply has no panel. The host never
   stops a running client to obtain one: the next client start that goes
   through the host carries the port. */
static void watchdog_tick(void) {
    DWORD pid = find_main_client();
    DWORD now = GetTickCount();

    if (!pid) {
        /* 客户端退出后由后续 tick 推进重启，不能在窗口消息线程里等待。 */
        if (!g_last_restart_attempt || (DWORD)(now - g_last_restart_attempt) >= 5000) {
            g_last_restart_attempt = now;
            host_log("客户端已退出，重新启动");
            open_client_window();
            g_injected_pid = 0;
            g_ui_injected = 0;
            g_last_page[0] = 0;
            g_cdp_miss_probes = 0;
            source_clear();
        }
        return;
    }

    if (pid != g_injected_pid || !module_loaded(pid, L"nexsink.dll")) {
        if (inject_dll(pid, kSinkDll)) {
            g_injected_pid = pid;
            source_clear();
            host_log("模块已挂载到 pid=%lu", (unsigned long)pid);
        }
        g_ui_injected = 0;
        g_last_page[0] = 0;
        /* A new client gets a full probe interval before its panel is judged. */
        g_cdp_miss_probes = 0;
        g_last_probe = now;
    }

    /* The panel's transport is the UI chain's call (ui_thread). CDP runs only
       when the renderer path is out; the scripts guard against a second copy. */
    if (g_ui_route != NEX_UI_ROUTE_CDP) {
        if (g_ui_route == NEX_UI_ROUTE_RENDERER) g_cdp_miss_probes = 0;
        return;
    }
    if (!g_cdp_enabled) {
        static DWORD said_for;
        if (said_for != pid) {
            said_for = pid;
            host_log("网易云里的面板没加载上（渲染进程那条路不通）；CDP 备用通道默认关闭，"
                     "请用托盘的「设置…」，或在 host.cfg 写 cdp=1 后重启");
        }
        return;
    }

    /* 页面重载会让 id 变化，界面脚本必须跟着重新注入 */
    char page[128];
    if (cdp_page_id(page, sizeof(page))) {
        /* 同一页面目标只登记一次。脚本注册会随文档加载自动执行，
           重复登记会让长时间运行的客户端累积副本。 */
        int document_has_panel = cdp_document_has_panel(page);
        int need = !g_ui_injected || strcmp(page, g_last_page) != 0 || !document_has_panel;
        if (need) {
            int was = g_ui_injected;
            if (inject_ui_once()) {
                if (!was) host_log("界面脚本已就位 page=%s", page);
                g_ui_injected = 1;
                g_last_ui_time = now;
                snprintf(g_last_page, sizeof(g_last_page), "%s", page);
                g_cdp_miss_probes = 0;
            }
        }
    } else if (now - g_last_probe > 15000) {
        g_last_probe = now;
        /* Said twice per client, then quiet: once while it may still be
           starting, once as the verdict. Probing itself continues, so a panel
           that becomes reachable later is still injected. */
        if (g_cdp_miss_probes < 2) {
            g_cdp_miss_probes++;
            if (g_cdp_miss_probes == 1)
                host_log("CDP 端口未就绪（客户端可能刚启动）");
            else
                host_log("面板不可用：当前客户端 pid=%lu 没有开放 9333；音频不受影响，"
                         "不会为此重启客户端。%s",
                         (unsigned long)pid, ui_cdp_unavailable_hint((int)g_ui_reason));
        }
    }
}

#define IDI_NEXHOST 100   /* nexhost.rc.in: 100 ICON "nexhost.ico" */

static void tray_add(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_EVENT;
    /* 自带图标按托盘尺寸加载；资源缺失（例如测试用的裸编译）时退回系统默认图标。
       IDI_APPLICATION 在非 UNICODE 编译下展开成 MAKEINTRESOURCEA，给 LoadIconW
       会报指针类型不匹配，所以直接写宽版本 */
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_NEXHOST),
                                    IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
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

#include "host_conflict.inc"   /* 别的应用想出声：SMTC 监视、询问、托盘提示 */
#include "host_settings.inc"

static void tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MENU_SETTINGS, L"设置…");
    SetMenuDefaultItem(m, MENU_SETTINGS, FALSE);
    AppendMenuW(m, MF_STRING, MENU_CLIENT, L"打开网易云音乐");
    AppendMenuW(m, MF_STRING, MENU_LOGS,   L"查看日志");
    AppendMenuW(m, MF_STRING, MENU_FOLDER, L"打开程序目录");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_SHARE,
                InterlockedCompareExchange(&g_ctl_manual, 0, 0)
                    ? L"恢复独占（下次继续播放时生效）"
                    : L"暂时关闭独占（让其他应用出声）");
    AppendMenuW(m, MF_STRING, MENU_FORGET, L"清除记住的应用选择");
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
            open_client_window();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MENU_CLIENT:
            open_client_window();
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
        case MENU_SHARE:
            InterlockedExchange(&g_ctl_manual_req,
                                InterlockedCompareExchange(&g_ctl_manual, 0, 0) ? 2 : 1);
            break;
        case MENU_SETTINGS:
            settings_open();
            break;
        case MENU_FORGET: {
            /* 改名而不是删除：误点了还能把 apps.cfg.bak 改回来 */
            char path[MAX_PATH], bak[MAX_PATH];
            apps_cfg_path(path, sizeof(path));
            snprintf(bak, sizeof(bak), "%s.bak", path);
            if (MoveFileExA(path, bak, MOVEFILE_REPLACE_EXISTING))
                host_log("已清除记住的应用选择（旧文件改名为 apps.cfg.bak）");
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
    source_clear();
    if (!resolve_paths()) {
        MessageBoxW(NULL, L"无法解析 NexExclusive 自身目录，已停止启动。", L"NexExclusive", MB_ICONERROR);
        return 1;
    }
    {
        char cdp[8];
        g_cdp_enabled = cfg_lookup(kHostCfg, "cdp", cdp, sizeof(cdp)) && atoi(cdp) != 0;
    }

    WCHAR client_arg[MAX_PATH];
    client_arg[0] = 0;
    int want_install = 0, want_uninstall = 0, want_verify = 0, want_help = 0, want_console = 0, want_show = 0;
    const char *bad = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install"))        want_install = 1;
        else if (!strcmp(argv[i], "--uninstall")) want_uninstall = 1;
        else if (!strcmp(argv[i], "--verify"))    want_verify = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) want_help = 1;
        else if (!strcmp(argv[i], "--console"))   want_console = 1;
        else if (!strcmp(argv[i], "--show") || !strcmp(argv[i], "--open")) want_show = 1;
        else if (!strcmp(argv[i], "--client") && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, client_arg, MAX_PATH);
            client_arg[MAX_PATH - 1] = 0;
        } else { bad = argv[i]; break; }
    }

    /* 这些模式是用户在命令行主动发起的，输出必须看得见 */
    if (want_console || want_show || want_install || want_uninstall || want_verify || want_help) ensure_console();

    if (bad) { fprintf(stderr, "未知参数：%s（试试 --help）\n", bad); return 2; }
    if (want_help) { usage(); return 0; }
    if (want_uninstall) return do_uninstall();
    if (want_verify) return do_verify();
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
        WCHAR no_redirect[8];
        int dev_no_redirect = GetEnvironmentVariableW(L"NEX_NO_REDIRECT", no_redirect,
                                                      sizeof(no_redirect) / sizeof(no_redirect[0])) > 0;
        if (!dev_no_redirect && installed_dir(inst, MAX_PATH)) {
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
    enable_debug_privilege();

    /* 配置服务先起，面板随时可以连上来 */
    CreateThread(NULL, 0, http_thread, NULL, 0, NULL);
    /* 界面链路单独一个线程：跟踪子进程、注入 nexui.dll、决定是否回退 CDP */
    CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, conflict_thread, NULL, 0, NULL);

    DWORD initial_pid = find_main_client();
    if (want_show || !initial_pid || !client_window_visible(initial_pid)) {
        host_log(want_show ? "收到呼出网易云窗口命令" : "客户端没有可见窗口，开始呼出");
        if (!open_client_window())
            host_log("未能确认网易云可见窗口，继续看护并等待客户端完成启动");
    } else {
        host_log("客户端已在运行 pid=%lu", (unsigned long)initial_pid);
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
        if (g_set_wnd && IsDialogMessageW(g_set_wnd, &msg)) continue;   /* Tab 在控件间切换 */
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    host_log("=== nexhost 退出 ===");
    return 0;
}
