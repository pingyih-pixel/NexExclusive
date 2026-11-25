/* ---- 独立设置窗口（托盘「设置…」）---------------------------------------
   面板原来只能在网易云页面里打开：要么 nexui.dll 进渲染进程，要么 CDP 走 9333
   调试端口（端口开着时本机任何进程都能在客户端页面里执行脚本）。这个窗口是宿主
   自己的 Win32 窗口，读写的仍是同一份 hook.cfg，用的也是 HTTP 接口那几个函数
   （cfg_read / cfg_sanitize / cfg_rewrite），两边改的是同一个东西。
   状态行来自控制块快照（g_ctl_snap），每秒刷新。 */

#define SET_ID_STATUS   200
#define SET_ID_SAVE     IDOK
#define SET_ID_CLOSE    IDCANCEL
#define SET_ID_FIRST    300      /* 第 i 行控件的 id = SET_ID_FIRST + i */
#define SET_TIMER       1

enum { SET_CHECK = 0, SET_COMBO = 1, SET_ENDPOINT = 2 };

typedef struct {
    int key;                 /* kCfgKeys 下标；SET_ENDPOINT 时不用 */
    int kind;
    const WCHAR *label;
    int count;
    const int *vals;
    const WCHAR *const *names;
} SetRow;

static const int kSetHifiV[] = {0, 1};
static const WCHAR *const kSetHifiN[] = {L"原率优先（推荐）", L"严格原样"};
static const int kSetIdleV[] = {0, 10000, 30000, 60000, 300000};
static const WCHAR *const kSetIdleN[] = {L"不让出", L"暂停 10 秒后", L"暂停 30 秒后",
                                         L"暂停 1 分钟后", L"暂停 5 分钟后"};
static const int kSetRateV[] = {0, 44100, 48000, 88200, 96000};
static const WCHAR *const kSetRateN[] = {L"跟随音源", L"44.1 kHz", L"48 kHz", L"88.2 kHz", L"96 kHz"};
static const int kSetBufV[] = {0, 512, 1024, 2048, 4096};
static const WCHAR *const kSetBufN[] = {L"自动", L"512", L"1024", L"2048", L"4096"};
static const int kSetSrcV[] = {0, 1};
static const WCHAR *const kSetSrcN[] = {L"带宽优先", L"整数比优先"};
static const int kSetDitherV[] = {0, 1, 2};
static const WCHAR *const kSetDitherN[] = {L"关闭", L"开启", L"自动"};
static const int kSetFadeV[] = {0, 3, 5, 10, 20};
static const WCHAR *const kSetFadeN[] = {L"关闭", L"3 ms", L"5 ms", L"10 ms", L"20 ms"};

#define SET_COMBO_OF(v, n) (int)(sizeof(v) / sizeof(v[0])), v, n
static const SetRow kSetRows[] = {
    {0,  SET_CHECK,    L"启用独占输出", 0, NULL, NULL},
    {9,  SET_COMBO,    L"输出策略", SET_COMBO_OF(kSetHifiV, kSetHifiN)},
    {-1, SET_ENDPOINT, L"独占输出设备", 0, NULL, NULL},
    {10, SET_CHECK,    L"独立音频宿主", 0, NULL, NULL},
    {11, SET_COMBO,    L"暂停时让出声卡", SET_COMBO_OF(kSetIdleV, kSetIdleN)},
    {12, SET_CHECK,    L"别的应用出声时改走共享（会先询问）", 0, NULL, NULL},
    {2,  SET_COMBO,    L"采样率", SET_COMBO_OF(kSetRateV, kSetRateN)},
    {3,  SET_COMBO,    L"缓冲帧数", SET_COMBO_OF(kSetBufV, kSetBufN)},
    {4,  SET_CHECK,    L"允许兼容转换", 0, NULL, NULL},
    {8,  SET_COMBO,    L"重采样策略", SET_COMBO_OF(kSetSrcV, kSetSrcN)},
    {5,  SET_COMBO,    L"抖动", SET_COMBO_OF(kSetDitherV, kSetDitherN)},
    {6,  SET_COMBO,    L"开头淡入", SET_COMBO_OF(kSetFadeV, kSetFadeN)},
};
#define SET_ROWS ((int)(sizeof(kSetRows) / sizeof(kSetRows[0])))

static HWND g_set_wnd;
static HFONT g_set_font;
static char g_set_ep_ids[32][200];   /* 设备下拉框第 i 项（0 = 跟随网易云）对应的 id */
static int g_set_ep_count;

/* 活动的输出设备：id（UTF-8）与友好名称 */
static int set_list_endpoints(char ids[][200], WCHAR names[][128], int max) {
    int count = 0;
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *list = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_l, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator_l, (void **)&en)) || !en) return 0;
    if (SUCCEEDED(en->lpVtbl->EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &list)) && list) {
        UINT total = 0;
        list->lpVtbl->GetCount(list, &total);
        for (UINT i = 0; i < total && count < max; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(list->lpVtbl->Item(list, i, &dev)) || !dev) continue;
            LPWSTR wid = NULL;
            ids[count][0] = 0;
            names[count][0] = 0;
            if (SUCCEEDED(dev->lpVtbl->GetId(dev, &wid)) && wid) {
                WideCharToMultiByte(CP_UTF8, 0, wid, -1, ids[count], 200, NULL, NULL);
                CoTaskMemFree(wid);
            }
            IPropertyStore *ps = NULL;
            if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
                    pv.vt == VT_LPWSTR && pv.pwszVal) {
                    wcsncpy(names[count], pv.pwszVal, 127);
                    names[count][127] = 0;
                }
                PropVariantClear(&pv);
                ps->lpVtbl->Release(ps);
            }
            dev->lpVtbl->Release(dev);
            if (ids[count][0] && endpoint_id_valid(ids[count])) count++;
        }
        list->lpVtbl->Release(list);
    }
    en->lpVtbl->Release(en);
    return count;
}

static void set_refresh_status(void) {
    if (!g_set_wnd) return;
    NexControl snap;
    int ok;
    AcquireSRWLockExclusive(&g_snap_lock);
    snap = g_ctl_snap;
    ok = g_ctl_snap_ok;
    ReleaseSRWLockExclusive(&g_snap_lock);
    WCHAR tip[128], ep[128] = L"", text[320];
    tray_tip_text(&snap, ok, tip, 128);
    if (ok && snap.endpoint[0])
        MultiByteToWideChar(CP_UTF8, 0, snap.endpoint, -1, ep, 128);
    const WCHAR *body = wcsncmp(tip, L"NexExclusive — ", 15) == 0 ? tip + 15 : tip;
    _snwprintf(text, 320, L"当前：%ls%ls%ls%ls", body, ep[0] ? L"\n设备：" : L"", ep,
               InterlockedCompareExchange(&g_ctl_manual, 0, 0) ? L"\n已从托盘暂时关闭独占" : L"");
    text[319] = 0;
    SetDlgItemTextW(g_set_wnd, SET_ID_STATUS, text);
}

static void set_load_values(HWND hwnd) {
    int v[CFG_N];
    cfg_read(v);
    for (int i = 0; i < SET_ROWS; i++) {
        HWND c = GetDlgItem(hwnd, SET_ID_FIRST + i);
        const SetRow *r = &kSetRows[i];
        if (r->kind == SET_CHECK) {
            SendMessageW(c, BM_SETCHECK, v[r->key] ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (r->kind == SET_COMBO) {
            int sel = 0;
            for (int k = 0; k < r->count; k++) if (r->vals[k] == v[r->key]) sel = k;
            SendMessageW(c, CB_SETCURSEL, sel, 0);
        } else {
            char current[200];
            cfg_read_string("endpoint", current, sizeof(current));
            int sel = 0;
            for (int k = 1; k < g_set_ep_count; k++)
                if (strcmp(g_set_ep_ids[k], current) == 0) sel = k;
            SendMessageW(c, CB_SETCURSEL, sel, 0);
        }
    }
}

static int set_save_values(HWND hwnd) {
    int v[CFG_N];
    cfg_read(v);
    char endpoint[200] = "";
    for (int i = 0; i < SET_ROWS; i++) {
        HWND c = GetDlgItem(hwnd, SET_ID_FIRST + i);
        const SetRow *r = &kSetRows[i];
        if (r->kind == SET_CHECK) {
            v[r->key] = SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
        } else if (r->kind == SET_COMBO) {
            LRESULT sel = SendMessageW(c, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < r->count) v[r->key] = r->vals[sel];
        } else {
            LRESULT sel = SendMessageW(c, CB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < g_set_ep_count) snprintf(endpoint, sizeof(endpoint), "%s", g_set_ep_ids[sel]);
        }
    }
    cfg_sanitize(v);
    if (!endpoint_id_valid(endpoint)) endpoint[0] = 0;
    int ok = cfg_rewrite(v, "endpoint", endpoint);
    host_log("设置窗口：%s（endpoint=%s）", ok ? "已保存" : "保存失败", endpoint[0] ? endpoint : "跟随客户端");
    return ok;
}

static UINT set_dpi(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn;
    static int looked;
    if (!looked) {
        looked = 1;
        fn = (GetDpiForWindowFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    UINT dpi = fn && hwnd ? fn(hwnd) : 96;
    return dpi ? dpi : 96;
}

static void set_build(HWND hwnd) {
    UINT dpi = set_dpi(hwnd);
#define S(x) MulDiv((x), (int)dpi, 96)
    if (g_set_font) DeleteObject(g_set_font);
    g_set_font = CreateFontW(-MulDiv(9, (int)dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HINSTANCE inst = GetModuleHandleW(NULL);
    int y = S(12);
    HWND st = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              S(16), y, S(440), S(54), hwnd, (HMENU)(INT_PTR)SET_ID_STATUS, inst, NULL);
    SendMessageW(st, WM_SETFONT, (WPARAM)g_set_font, TRUE);
    y += S(62);

    char ids[31][200];
    WCHAR names[31][128];
    int n = set_list_endpoints(ids, names, 31);
    g_set_ep_count = 0;
    g_set_ep_ids[g_set_ep_count++][0] = 0;          /* 跟随网易云 */
    for (int i = 0; i < n; i++) snprintf(g_set_ep_ids[g_set_ep_count++], 200, "%s", ids[i]);

    for (int i = 0; i < SET_ROWS; i++) {
        const SetRow *r = &kSetRows[i];
        HWND c;
        if (r->kind == SET_CHECK) {
            c = CreateWindowExW(0, L"BUTTON", r->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                S(16), y, S(440), S(24), hwnd, (HMENU)(INT_PTR)(SET_ID_FIRST + i), inst, NULL);
        } else {
            HWND l = CreateWindowExW(0, L"STATIC", r->label, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     S(16), y + S(4), S(150), S(22), hwnd, NULL, inst, NULL);
            SendMessageW(l, WM_SETFONT, (WPARAM)g_set_font, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                S(170), y, S(286), S(240), hwnd, (HMENU)(INT_PTR)(SET_ID_FIRST + i), inst, NULL);
            if (r->kind == SET_COMBO) {
                for (int k = 0; k < r->count; k++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)r->names[k]);
            } else {
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"跟随网易云");
                for (int k = 0; k < n; k++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)names[k]);
            }
        }
        SendMessageW(c, WM_SETFONT, (WPARAM)g_set_font, TRUE);
        y += S(32);
    }
    HWND note = CreateWindowExW(0, L"STATIC", L"保存后从下一首开始生效。设置和网易云里的面板是同一份。",
                                WS_CHILD | WS_VISIBLE | SS_LEFT, S(16), y + S(4), S(440), S(22),
                                hwnd, NULL, inst, NULL);
    SendMessageW(note, WM_SETFONT, (WPARAM)g_set_font, TRUE);
    y += S(34);
    HWND save = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                S(266), y, S(90), S(30), hwnd, (HMENU)(INT_PTR)SET_ID_SAVE, inst, NULL);
    HWND close = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 S(366), y, S(90), S(30), hwnd, (HMENU)(INT_PTR)SET_ID_CLOSE, inst, NULL);
    SendMessageW(save, WM_SETFONT, (WPARAM)g_set_font, TRUE);
    SendMessageW(close, WM_SETFONT, (WPARAM)g_set_font, TRUE);
    y += S(44);
    RECT rc = {0, 0, S(472), y};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
#undef S
    set_load_values(hwnd);
    set_refresh_status();
}

static LRESULT CALLBACK set_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_set_wnd = hwnd;
        set_build(hwnd);
        SetTimer(hwnd, SET_TIMER, 1000, NULL);
        return 0;
    case WM_TIMER:
        if (wp == SET_TIMER) set_refresh_status();
        return 0;
    case WM_CTLCOLORSTATIC: {
        /* 标签和复选框默认是按钮灰底，放在白窗口上像一块块补丁 */
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == SET_ID_SAVE) {
            if (set_save_values(hwnd)) {
                set_refresh_status();
                SetWindowTextW(hwnd, L"NexExclusive 设置 — 已保存");
            } else {
                MessageBoxW(hwnd, L"写不进 hook.cfg，设置没有保存。", L"NexExclusive", MB_ICONWARNING);
            }
        } else if (LOWORD(wp) == SET_ID_CLOSE) {
            DestroyWindow(hwnd);
        } else if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == CBN_SELCHANGE) {
            SetWindowTextW(hwnd, L"NexExclusive 设置");
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, SET_TIMER);
        g_set_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void settings_open(void) {
    if (g_set_wnd) {
        ShowWindow(g_set_wnd, SW_RESTORE);
        SetForegroundWindow(g_set_wnd);
        return;
    }
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = set_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NexExclusiveSettings";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_NEXHOST));
        registered = RegisterClassExW(&wc) != 0;
    }
    /* 设备列表要用 COM；主线程（托盘窗口所在）按需初始化成单线程套间 */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HWND h = CreateWindowExW(0, L"NexExclusiveSettings", L"NexExclusive 设置",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 480, 640, NULL, NULL, inst, NULL);
    if (!h) { host_log("设置窗口创建失败 %lu", GetLastError()); return; }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}
