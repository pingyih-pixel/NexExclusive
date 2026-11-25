/* ---- 主窗口用的接口（docs/superpowers/specs/2026-11-26-main-window-design.md）----
   主窗口是 ui/app.html，由本机配置服务 127.0.0.1:9334 自己提供；它和网易云里的
   面板读写同一份 hook.cfg，再加上这里的几组接口：

     GET  /app                 主窗口页面（ui/app.html）
     GET  /api/status          原有状态 + control / manualShared / othersPlaying / client
     POST /api/control         {"action":"share"|"exclusive"}
     GET  /api/apps            记住的选择
     POST /api/apps            {"app":"…","choice":"yield"|"keep"|"forget"}
     GET  /api/host            宿主自己的设置与信息
     POST /api/host            {"autostart":0|1,"closeToTray":…,"launchClient":…,"clientPanel":…,"cdp":…}
     POST /api/action          {"action":"open_logs"|"open_folder"|"open_client"|"uninstall"}

   宿主自己的设置存在 host.cfg（close_to_tray / launch_client / client_panel /
   cdp），开机自启在 HKCU\...\Run。 */

#define WM_NEX_SHOW        (WM_APP + 20)   /* 显示主窗口（第二次运行、托盘） */
#define WM_NEX_OPEN_CLIENT (WM_APP + 21)   /* 在托盘线程里打开网易云窗口 */

static HWND g_tray_wnd;                     /* 托盘的隐藏窗口，接收上面两个消息 */

/* ---- host.cfg：改一个键，其余行原样保留 ---- */
static int hostcfg_set(const char *key, const char *value) {
    char *old = read_file(kHostCfg);
    size_t cap = (old ? strlen(old) : 0) + strlen(key) + strlen(value) + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(old); return 0; }
    size_t used = 0, klen = strlen(key);
    int done = 0;
    for (char *line = old; line && *line; ) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line + 1) : strlen(line);
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!done && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            used += (size_t)snprintf(out + used, cap - used, "%s=%s\n", key, value);
            done = 1;
        } else {
            memcpy(out + used, line, len);
            used += len;
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (!done) {
        if (used && out[used - 1] != '\n') out[used++] = '\n';
        used += (size_t)snprintf(out + used, cap - used, "%s=%s\n", key, value);
    }
    free(old);
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", kHostCfg);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(out, 1, used, f) == used;
    if (f) fclose(f);
    free(out);
    ok = ok && MoveFileExA(tmp, kHostCfg, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { DeleteFileA(tmp); host_log("host.cfg 写入失败：%s=%s", key, value); }
    return ok;
}

static int hostcfg_int(const char *key, int def) {
    char v[32];
    return cfg_lookup(kHostCfg, key, v, sizeof(v)) ? atoi(v) : def;
}

static void host_settings_load(void) {
    g_close_to_tray = hostcfg_int("close_to_tray", 1) != 0;
    g_launch_client = hostcfg_int("launch_client", 1) != 0;
    g_client_panel = hostcfg_int("client_panel", 1) != 0;
    g_cdp_enabled = hostcfg_int("cdp", 0) != 0;
}

/* ---- 开机自启：HKCU Run 值，带 --tray（开机只进托盘，不弹窗） ---- */
static int autostart_get(void) {
    WCHAR v[MAX_PATH * 2];
    DWORD n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"NexExclusive", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS;
}

static int autostart_set(int on) {
    if (!on) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return 1;
        LONG r = RegDeleteValueW(k, L"NexExclusive");
        RegCloseKey(k);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    WCHAR exe[MAX_PATH], run[MAX_PATH * 2];
    WCHAR inst[MAX_PATH];
    if (installed_dir(inst, MAX_PATH)) _snwprintf(exe, MAX_PATH, L"%ls\\NexExclusive.exe", inst);
    else self_path(exe, MAX_PATH);
    exe[MAX_PATH - 1] = 0;
    _snwprintf(run, MAX_PATH * 2, L"\"%ls\" --tray", exe);
    run[MAX_PATH * 2 - 1] = 0;
    return reg_set_sz(HKEY_CURRENT_USER, RUN_KEY, L"NexExclusive", run);
}

/* 旧版本写的 Run 值没有 --tray：开机会弹出主窗口。启动时补上。 */
static void autostart_migrate(void) {
    WCHAR v[MAX_PATH * 2];
    DWORD n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"NexExclusive", RRF_RT_REG_SZ, NULL, v, &n) != ERROR_SUCCESS) return;
    if (wcsstr(v, L"--tray")) return;
    wcsncat(v, L" --tray", MAX_PATH * 2 - wcslen(v) - 1);
    if (reg_set_sz(HKEY_CURRENT_USER, RUN_KEY, L"NexExclusive", v)) host_log("开机自启改为只进托盘（--tray）");
}

/* ---- 记住的选择 ---- */
static void apps_get_json(char *out, size_t n) {
    char path[MAX_PATH];
    apps_cfg_path(path, sizeof(path));
    char *txt = read_file(path);
    NexRememberedApp apps[64];
    int count = txt ? nex_apps_parse(txt, apps, 64) : 0;
    free(txt);
    size_t used = (size_t)snprintf(out, n, "{\"apps\":[");
    for (int i = 0; i < count && used + 600 < n; i++) {
        WCHAR wapp[200], shown[64];
        MultiByteToWideChar(CP_UTF8, 0, apps[i].app, -1, wapp, 200);
        nex_app_display_name(wapp, shown, 64);
        char shown8[192];
        wide_to_utf8(shown, shown8, sizeof(shown8));
        char *a = json_escape(apps[i].app), *s = json_escape(shown8);
        used += (size_t)snprintf(out + used, n - used,
                                 "%s{\"app\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"choice\":\"%s\"}",
                                 i ? "," : "", a ? a : "", s ? s : "",
                                 nex_app_kind(wapp) == NEX_APP_PLAYER ? "player" : "other",
                                 apps[i].choice == NEX_REMEMBER_YIELD ? "yield" : "keep");
        free(a); free(s);
    }
    snprintf(out + used, n - used, "]}");
}

static int apps_post(const char *body) {
    char app[200] = "", choice[16] = "";
    if (!json_get_string(body, "app", app, sizeof(app)) || !app[0]) return 400;
    json_get_string(body, "choice", choice, sizeof(choice));
    int c = !strcmp(choice, "yield") ? NEX_REMEMBER_YIELD : !strcmp(choice, "keep") ? NEX_REMEMBER_KEEP
          : !strcmp(choice, "forget") ? NEX_REMEMBER_NONE : -1;
    if (c < 0) return 400;
    for (const char *p = app; *p; p++) if (*p == '\n' || *p == '\r' || *p == '=') return 400;
    char path[MAX_PATH];
    apps_cfg_path(path, sizeof(path));
    char *txt = read_file(path);
    NexRememberedApp apps[64];
    int count = txt ? nex_apps_parse(txt, apps, 64) : 0;
    free(txt);
    int i = 0;
    while (i < count && strcmp(apps[i].app, app) != 0) i++;
    if (i == count) {
        if (c == NEX_REMEMBER_NONE) return 200;
        if (count == 64) return 400;
        snprintf(apps[count].app, sizeof(apps[count].app), "%s", app);
        count++;
    }
    apps[i].choice = c;
    char out[64 * 220];
    size_t len = nex_apps_render(apps, count, out, sizeof(out));
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(out, 1, len, f) == len;
    if (f) fclose(f);
    host_log("记住的选择：%s → %s", app, choice);
    return ok ? 200 : 500;
}

/* ---- 宿主设置与信息 ---- */
static long file_size_a(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    return GetFileAttributesExA(path, GetFileExInfoStandard, &fad) ? (long)fad.nFileSizeLow : -1;
}

static void host_get_json(char *out, size_t n) {
    char *base = json_escape(g_base);
    char ver[32];
    WideCharToMultiByte(CP_UTF8, 0, APP_VERSION, -1, ver, sizeof(ver), NULL, NULL);
    snprintf(out, n,
             "{\"autostart\":%s,\"closeToTray\":%s,\"launchClient\":%s,\"clientPanel\":%s,\"cdp\":%s,"
             "\"version\":\"%s\",\"build\":\"%s\",\"installDir\":\"%s\","
             "\"logs\":{\"sink\":%ld,\"host\":%ld}}",
             autostart_get() ? "true" : "false", g_close_to_tray ? "true" : "false",
             g_launch_client ? "true" : "false", g_client_panel ? "true" : "false",
             g_cdp_enabled ? "true" : "false", ver, NEX_BUILD_ID,
             base ? base : "", file_size_a(kSinkLog), file_size_a(kHostLog));
    free(base);
}

static int host_post(const char *body) {
    int v;
    int ok = 1;
    if (json_get_int(body, "autostart", &v)) ok &= autostart_set(v != 0);
    if (json_get_int(body, "closeToTray", &v)) { g_close_to_tray = v != 0; ok &= hostcfg_set("close_to_tray", v ? "1" : "0"); }
    if (json_get_int(body, "launchClient", &v)) { g_launch_client = v != 0; ok &= hostcfg_set("launch_client", v ? "1" : "0"); }
    if (json_get_int(body, "clientPanel", &v)) { g_client_panel = v != 0; ok &= hostcfg_set("client_panel", v ? "1" : "0"); }
    /* 端口只在启动网易云时决定开不开，所以 cdp 改了要等下次启动 */
    if (json_get_int(body, "cdp", &v)) ok &= hostcfg_set("cdp", v ? "1" : "0");
    host_log("主窗口：宿主设置已更新");
    return ok ? 200 : 500;
}

static int action_post(const char *body) {
    char a[32] = "";
    json_get_string(body, "action", a, sizeof(a));
    if (!strcmp(a, "open_logs")) { open_logs(); return 200; }
    if (!strcmp(a, "open_folder")) {
        WCHAR wdir[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, g_base, -1, wdir, MAX_PATH);
        ShellExecuteW(NULL, L"open", wdir, NULL, NULL, SW_SHOWNORMAL);
        return 200;
    }
    if (!strcmp(a, "open_client")) { if (g_tray_wnd) PostMessageW(g_tray_wnd, WM_NEX_OPEN_CLIENT, 0, 0); return 200; }
    if (!strcmp(a, "uninstall")) {
        /* 走装好的那份的 --uninstall：它会先停掉正在跑的宿主（也就是我们）和网易云 */
        WCHAR inst[MAX_PATH], exe[MAX_PATH];
        if (installed_dir(inst, MAX_PATH)) _snwprintf(exe, MAX_PATH, L"%ls\\NexExclusive.exe", inst);
        else self_path(exe, MAX_PATH);
        exe[MAX_PATH - 1] = 0;
        host_log("主窗口：开始卸载");
        ShellExecuteW(NULL, L"open", exe, L"--uninstall", NULL, SW_SHOWNORMAL);
        return 200;
    }
    return 400;
}

static int control_post(const char *body) {
    char a[16] = "";
    json_get_string(body, "action", a, sizeof(a));
    if (!strcmp(a, "share")) InterlockedExchange(&g_ctl_manual_req, 1);
    else if (!strcmp(a, "exclusive")) InterlockedExchange(&g_ctl_manual_req, 2);
    else return 400;
    return 200;
}

/* ---- /api/status 的补充字段 ---- */
static const char *path_name(long path, int shared) {
    if (shared) return "shared";
    switch (path) { case 0: return "native"; case 1: return "repacked"; case 2: return "depth"; case 3: return "converted"; }
    return "";
}

static void status_extra_json(char *out, size_t n) {
    NexControl c;
    int ok;
    NexOtherApp others[8];
    int n_others;
    AcquireSRWLockExclusive(&g_snap_lock);
    c = g_ctl_snap;
    ok = g_ctl_snap_ok;
    n_others = g_others_count;
    memcpy(others, g_others_snap, sizeof(others));
    ReleaseSRWLockExclusive(&g_snap_lock);
    const char *state = !ok ? "" : c.state == NEX_CONTROL_STATE_EXCLUSIVE ? "exclusive"
                      : c.state == NEX_CONTROL_STATE_SHARED ? "shared"
                      : c.state == NEX_CONTROL_STATE_RELEASED ? "released" : "idle";
    char ep[160];
    snprintf(ep, sizeof(ep), "%s", ok ? c.endpoint : "");
    char *epe = json_escape(ep);
    size_t used = (size_t)snprintf(out, n,
        "\"control\":{\"ok\":%s,\"state\":\"%s\",\"playing\":%s,\"inRate\":%ld,\"inBits\":%ld,\"inFloat\":%s,"
        "\"outRate\":%ld,\"outBits\":%ld,\"outContainer\":%ld,\"outFloat\":%s,\"path\":\"%s\",\"remote\":%s,"
        "\"forcedShared\":%s,\"endpoint\":\"%s\"},\"manualShared\":%s,\"othersPlaying\":[",
        ok ? "true" : "false", state, ok && c.playing ? "true" : "false",
        ok ? (long)c.in_rate : 0L, ok ? (long)c.in_bits : 0L, ok && c.in_float ? "true" : "false",
        ok ? (long)c.out_rate : 0L, ok ? (long)c.out_bits : 0L, ok ? (long)c.out_container : 0L,
        ok && c.out_float ? "true" : "false", ok ? path_name(c.path, c.state == NEX_CONTROL_STATE_SHARED) : "",
        ok && c.remote ? "true" : "false", ok && c.share_forced ? "true" : "false", epe ? epe : "",
        InterlockedCompareExchange(&g_ctl_manual, 0, 0) ? "true" : "false");
    free(epe);
    for (int i = 0; i < n_others && used + 400 < n; i++) {
        WCHAR shown[64];
        char app8[480], shown8[192];
        nex_app_display_name(others[i].app, shown, 64);
        wide_to_utf8(others[i].app, app8, sizeof(app8));
        wide_to_utf8(shown, shown8, sizeof(shown8));
        char *a = json_escape(app8), *s = json_escape(shown8);
        used += (size_t)snprintf(out + used, n - used, "%s{\"app\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\"}",
                                 i ? "," : "", a ? a : "", s ? s : "",
                                 others[i].kind == NEX_APP_PLAYER ? "player" : "other");
        free(a); free(s);
    }
    DWORD pid = g_injected_pid;
    char *ver = json_escape(g_client_ver8);
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, kClientExe, -1, path8, sizeof(path8), NULL, NULL);
    char *pe = json_escape(path8);
    LONG route = InterlockedCompareExchange(&g_ui_route, 0, 0);
    snprintf(out + used, n - used,
             "],\"client\":{\"pid\":%lu,\"injected\":%s,\"version\":\"%s\",\"verified\":%s,\"path\":\"%s\","
             "\"panel\":\"%s\"}",
             (unsigned long)pid, pid ? "true" : "false", ver ? ver : "", g_client_verified ? "true" : "false",
             pe ? pe : "", !pid ? "" : route == NEX_UI_ROUTE_RENDERER ? "renderer"
                         : route == NEX_UI_ROUTE_CDP ? (g_cdp_enabled ? "cdp" : "none") : "wait");
    free(ver); free(pe);
}

/* /api/ 下主窗口新增的那几个；没处理就返回 0 */
static int api_extra(const char *method, const char *path, const char *body, char *out, size_t n) {
    int post = !strcmp(method, "POST"), get = !strcmp(method, "GET");
    int code = 0;
    if (!strcmp(path, "/api/apps") && get) { apps_get_json(out, n); return 200; }
    if (!strcmp(path, "/api/apps") && post) code = apps_post(body);
    else if (!strcmp(path, "/api/host") && get) { host_get_json(out, n); return 200; }
    else if (!strcmp(path, "/api/host") && post) code = host_post(body);
    else if (!strcmp(path, "/api/action") && post) code = action_post(body);
    else if (!strcmp(path, "/api/control") && post) code = control_post(body);
    else return 0;
    snprintf(out, n, code == 200 ? "{\"ok\":true}" : "{\"error\":\"rejected\"}");
    return code;
}

/* GET /app：主窗口页面，原样发出 ui/app.html */
static void serve_app_page(SOCKET c) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sui\\app.html", g_base);
    char *html = read_file(path);
    if (!html) { http_reply(c, 400, "{\"error\":\"ui/app.html missing\"}"); return; }
    size_t len = strlen(html);
    char head[512];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n", (unsigned)len);
    send_all(c, head, (size_t)hl);
    send_all(c, html, len);
    free(html);
}
