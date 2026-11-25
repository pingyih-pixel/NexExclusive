/* ---- 主窗口（WebView2 里显示 ui/app.html）--------------------------------
   打开 NexExclusive 就是这个窗口；最小化或关闭缩到托盘（关于页可改成关闭即
   退出），双击托盘或再次运行 exe 时回来。窗口藏起来的时候把 WebView2 一起关掉，
   msedgewebview2 那几个进程随之退出，不在后台占内存；再显示时重建，大约半秒。
   WebView2 起不来（没装运行时、接口不对）就退回 host_settings.inc 的 Win32 窗口，
   设置照样能改。 */

static HWND g_main_wnd;
static NexWebView g_mwv;
static int g_mwv_live;            /* 正在创建或已经建好 */
static int g_mwv_broken;          /* 这次进程里起不来过，之后直接用简易窗口 */

static int system_uses_dark(void) {
    DWORD v = 1, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS) return 0;
    return v == 0;
}

/* 标题栏跟随系统深浅色（Windows 10 2004+ / 11），失败就保持默认 */
static void main_apply_theme(HWND h) {
    typedef HRESULT (WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
    static DwmSetFn set;
    static int looked;
    if (!looked) {
        looked = 1;
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        set = m ? (DwmSetFn)(void *)GetProcAddress(m, "DwmSetWindowAttribute") : NULL;
    }
    if (!set) return;
    BOOL dark = system_uses_dark();
    set(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

static void main_webview_ready(NexWebView *w) {
    host_log("主窗口：界面已加载（WebView2 进程 %lu）", (unsigned long)nex_wv_browser_pid(w));
}

static void main_webview_failed(NexWebView *w, const char *what) {
    host_log("主窗口：WebView2 起不来（%s，0x%08lX），改用简易设置窗口", what, (unsigned long)w->error);
    g_mwv_broken = 1;
    g_mwv_live = 0;
    nex_wv_close(w);
    if (g_main_wnd) ShowWindow(g_main_wnd, SW_HIDE);
    settings_open();
}

static void main_webview_start(void) {
    if (g_mwv_live || g_mwv_broken || !g_main_wnd) return;
    memset(&g_mwv, 0, sizeof(g_mwv));
    g_mwv.hwnd = g_main_wnd;
    _snwprintf(g_mwv.url, 512, L"http://127.0.0.1:%d/app", CFG_PORT);
    {
        WCHAR base[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, g_base, -1, base, MAX_PATH);
        _snwprintf(g_mwv.user_data, MAX_PATH, L"%lswebview", base);
        g_mwv.user_data[MAX_PATH - 1] = 0;
    }
    g_mwv.on_ready = main_webview_ready;
    g_mwv.on_failed = main_webview_failed;
    const char *why = "";
    g_mwv_live = 1;
    if (!nex_wv_start(&g_mwv, &why)) {
        g_mwv_live = 0;
        main_webview_failed(&g_mwv, why);
    }
}

static void main_webview_stop(void) {
    if (!g_mwv_live) return;
    nex_wv_close(&g_mwv);
    g_mwv_live = 0;
}

static void main_hide(void) {
    if (!g_main_wnd) return;
    ShowWindow(g_main_wnd, SW_HIDE);
    main_webview_stop();
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) { main_hide(); return 0; }   /* 最小化 = 缩到托盘 */
        nex_wv_set_bounds(&g_mwv);
        return 0;
    case WM_CLOSE:
        if (g_close_to_tray) main_hide();
        else if (g_tray_wnd) DestroyWindow(g_tray_wnd);       /* 关闭即退出 */
        return 0;
    case WM_SETTINGCHANGE:
        main_apply_theme(hwnd);
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        UINT dpi = set_dpi(hwnd);
        mm->ptMinTrackSize.x = MulDiv(760, (int)dpi, 96);
        mm->ptMinTrackSize.y = MulDiv(520, (int)dpi, 96);
        return 0;
    }
    case WM_DESTROY:
        main_webview_stop();
        g_main_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void main_show(void) {
    if (g_mwv_broken) { settings_open(); return; }
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!g_main_wnd) {
        static int registered;
        if (!registered) {
            WNDCLASSEXW wc;
            memset(&wc, 0, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = main_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
            /* 页面加载前的底色和页面一致，免得闪一下白 */
            wc.hbrBackground = CreateSolidBrush(system_uses_dark() ? RGB(0x20, 0x20, 0x20) : RGB(0xf3, 0xf3, 0xf3));
            wc.lpszClassName = L"NexExclusiveMain";
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_NEXHOST));
            wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_NEXHOST), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
            registered = RegisterClassExW(&wc) != 0;
        }
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* WebView2 要单线程套间 */
        UINT dpi = 96;
        {
            typedef UINT (WINAPI *GetDpiSysFn)(void);
            GetDpiSysFn f = (GetDpiSysFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
            if (f) dpi = f();
        }
        int w = MulDiv(1040, (int)dpi, 96), h = MulDiv(720, (int)dpi, 96);
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - h) / 2;
        g_main_wnd = CreateWindowExW(0, L"NexExclusiveMain", L"NexExclusive", WS_OVERLAPPEDWINDOW,
                                     x, y, w, h, NULL, NULL, inst, NULL);
        if (!g_main_wnd) { host_log("主窗口创建失败 %lu，改用简易设置窗口", GetLastError()); settings_open(); return; }
        main_apply_theme(g_main_wnd);
    }
    ShowWindow(g_main_wnd, IsIconic(g_main_wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main_wnd);
    main_webview_start();
}
