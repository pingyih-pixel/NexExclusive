/* ---- 别的应用想出声（ROADMAP 5c）------------------------------------------
   网易云独占声卡时，别的应用拿不到设备，会话层也不报任何事；但它的媒体会话
   （SMTC）照样报 Playing。这条线程每 500 ms 看一次 SMTC；有别的应用开始播放，
   就按 nexconflict.h 的规则问用户：
     · 别的音乐播放器：把独占交给它吗？是 → 通过网易云自己的媒体会话暂停网易云，
       并让出声卡，对方可以自己独占；
     · 其他应用（浏览器、视频）：关闭独占、一起播放吗？是 → 网易云改走共享。
   勾「记住」的回答写进 apps.cfg，之后这个应用直接按记住的办。命令经
   nexcontrol.h 的控制块交给 DLL。只在 hook.cfg 的 auto_shared=1（面板「别的应用
   出声时改走共享」）时询问；托盘的「暂时关闭独占」不受这个开关限制。 */
static volatile LONG g_ctl_manual;      /* 托盘手动关闭了独占 */
static volatile LONG g_ctl_manual_req;  /* 托盘点击：1 关闭独占，2 恢复独占 */

/* 询问窗口在自己的线程里（TaskDialog 是模态的），答案经这几个变量交回 */
static volatile LONG g_prompt_busy;
static volatile LONG g_prompt_answer;   /* 0 未答，1 是，2 否（含超时） */
static volatile LONG g_prompt_remember;
static WCHAR g_prompt_app[160];
static WCHAR g_prompt_shown[64];
static int   g_prompt_kind;

static void wide_to_utf8(const WCHAR *w, char *out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

static void tray_notify(const WCHAR *title, const WCHAR *text) {
    if (!g_tray_up) return;
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = g_nid.hWnd;
    n.uID = TRAY_ID;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}


static void rate_text(long rate, WCHAR *out, size_t cap) {
    if (rate % 1000) _snwprintf(out, cap, L"%ld.%ld kHz", rate / 1000, (rate % 1000) / 100);
    else _snwprintf(out, cap, L"%ld kHz", rate / 1000);
    out[cap - 1] = 0;
}

/* 托盘提示：一眼看出现在是独占、共享还是让出了，以及什么格式 */
static void tray_tip_text(const NexControl *c, int ok, WCHAR *out, size_t cap) {
    WCHAR rate[32];
    if (!ok || !c || c->state == NEX_CONTROL_STATE_IDLE) {
        _snwprintf(out, cap, L"NexExclusive — 等待播放");
    } else if (c->state == NEX_CONTROL_STATE_RELEASED) {
        _snwprintf(out, cap, L"NexExclusive — 暂停中，声卡已让出");
    } else if (c->state == NEX_CONTROL_STATE_SHARED) {
        rate_text(c->in_rate, rate, 32);
        _snwprintf(out, cap, L"NexExclusive — 共享输出 · %ls / %ld bit%ls（经系统混音器）",
                   rate, (long)c->in_bits, c->playing ? L"" : L" · 暂停");
    } else {
        static const WCHAR *paths[] = { L" · 原样", L" · 原样数值", L" · 位深转换", L" · 重采样" };
        rate_text(c->out_rate, rate, 32);
        _snwprintf(out, cap, L"NexExclusive — 独占 · %ls / %ld bit%ls%ls", rate, (long)c->out_bits,
                   c->path >= 0 && c->path <= 3 ? paths[c->path] : L"",
                   c->playing ? L"" : L" · 暂停");
    }
    out[cap - 1] = 0;
}

static void tray_set_tip(const WCHAR *tip) {
    if (!g_tray_up) return;
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = g_nid.hWnd;
    n.uID = TRAY_ID;
    n.uFlags = NIF_TIP;
    wcsncpy(n.szTip, tip, 127);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void apps_cfg_path(char *out, size_t cap) {
    snprintf(out, cap, "%sapps.cfg", g_base);
}

static int remembered_for(const WCHAR *app) {
    char path[MAX_PATH], app8[480];
    apps_cfg_path(path, sizeof(path));
    wide_to_utf8(app, app8, sizeof(app8));
    char *txt = read_file(path);
    int r = nex_remembered_choice(txt, app8);
    free(txt);
    return r;
}

static void remember_choice(const WCHAR *app, int yield) {
    char path[MAX_PATH], app8[480];
    apps_cfg_path(path, sizeof(path));
    wide_to_utf8(app, app8, sizeof(app8));
    FILE *f = fopen(path, "ab");
    if (!f) { host_log("记不下选择：打不开 %s", path); return; }
    fprintf(f, "%s=%s\n", yield ? "yield" : "keep", app8);
    fclose(f);
}

typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

static HRESULT CALLBACK prompt_callback(HWND hwnd, UINT note, WPARAM wp, LPARAM lp, LONG_PTR ref) {
    (void)lp; (void)ref;
    if (note == TDN_CREATED) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
    } else if (note == TDN_TIMER && (UINT)wp >= NEX_CONFLICT_PROMPT_TIMEOUT) {
        SendMessageW(hwnd, TDM_CLICK_BUTTON, 102, 0);
    }
    return S_OK;
}

static DWORD WINAPI prompt_thread(LPVOID unused) {
    (void)unused;
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    TaskDialogIndirectFn tdi = cc ? (TaskDialogIndirectFn)(void *)GetProcAddress(cc, "TaskDialogIndirect") : NULL;
    int button = 102;
    BOOL remember = FALSE;
    if (tdi) {
        WCHAR title[128], content[320], yes[200], no[160];
        if (g_prompt_kind == NEX_APP_PLAYER) {
            _snwprintf(title, 128, L"%ls 想播放", g_prompt_shown);
            _snwprintf(content, 320, L"网易云正在独占声卡，%ls 现在没有声音。", g_prompt_shown);
            _snwprintf(yes, 200, L"把独占交给 %ls\n网易云暂停并让出声卡，%ls 可以自己独占播放",
                       g_prompt_shown, g_prompt_shown);
            _snwprintf(no, 160, L"保持网易云独占\n%ls 这次不出声", g_prompt_shown);
        } else {
            _snwprintf(title, 128, L"%ls 想播放声音", g_prompt_shown);
            _snwprintf(content, 320, L"网易云正在独占声卡，%ls 现在没有声音。", g_prompt_shown);
            _snwprintf(yes, 200, L"关闭独占，一起播放\n网易云改走共享输出（不是原样）；"
                       L"%ls 可能要刷新或重新打开页面才有声音", g_prompt_shown);
            _snwprintf(no, 160, L"保持独占\n%ls 这次不出声", g_prompt_shown);
        }
        title[127] = content[319] = yes[199] = no[159] = 0;
        TASKDIALOG_BUTTON buttons[2] = { {101, yes}, {102, no} };
        TASKDIALOGCONFIG c;
        memset(&c, 0, sizeof(c));
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
        c.pszWindowTitle = L"NexExclusive";
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = title;
        c.pszContent = content;
        c.cButtons = 2;
        c.pButtons = buttons;
        c.nDefaultButton = 102;
        c.pszVerificationText = L"记住我的选择，以后不再询问这个应用";
        c.pszFooter = L"30 秒内不选，就保持独占。";
        c.pfCallback = prompt_callback;
        if (FAILED(tdi(&c, &button, NULL, &remember))) button = 102;
    } else {
        host_log("询问窗口不可用（TaskDialogIndirect），按保持独占处理");
    }
    InterlockedExchange(&g_prompt_remember, remember ? 1 : 0);
    InterlockedExchange(&g_prompt_answer, button == 101 ? 1 : 2);
    InterlockedExchange(&g_prompt_busy, 0);
    return 0;
}

static DWORD WINAPI conflict_thread(LPVOID unused) {
    (void)unused;
    NexSmtc smtc;
    int smtc_ok = nex_smtc_open(&smtc);
    if (smtc_ok) host_log("媒体会话监视就绪（SMTC）");
    else host_log("媒体会话监视不可用：%s；托盘的「暂时关闭独占」仍可用", smtc.error);
    int smtc_failures = 0;
    DWORD smtc_retry = GetTickCount();
    WCHAR last_tip[128] = L"";

    HANDLE map = NULL;
    NexControl *blk = NULL;
    DWORD blk_pid = 0, open_tick = 0, cfg_tick = 0;
    int auto_shared = 0;
    NexConflictState cs;                   /* 判断逻辑的状态（nexconflict.h） */
    memset(&cs, 0, sizeof(cs));

    for (;;) {
        Sleep(500);
        DWORD now = GetTickCount();
        if (!cfg_tick || now - cfg_tick > 3000) {
            int v[CFG_N];
            cfg_read(v);
            auto_shared = v[12] != 0;
            cfg_tick = now;
        }

        DWORD pid = g_injected_pid;
        if (pid != blk_pid) {
            if (blk) UnmapViewOfFile(blk);
            if (map) CloseHandle(map);
            blk = NULL; map = NULL; blk_pid = pid; open_tick = 0;
            memset(&cs, 0, sizeof(cs));
            InterlockedExchange(&g_ctl_manual, 0);
        }
        if (!blk && pid && (!open_tick || now - open_tick > 2000)) {
            open_tick = now;
            WCHAR name[64];
            nex_control_name(name, 64, pid);
            map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (map) {
                blk = (NexControl *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NexControl));
                if (blk && !nex_control_valid(blk)) {
                    UnmapViewOfFile(blk); blk = NULL;
                    CloseHandle(map); map = NULL;
                    host_log("控制块版本不符，pid=%lu 的模块太旧或太新", (unsigned long)pid);
                    open_tick = now + 60000;   /* 不必每两秒都说一遍 */
                } else if (blk) {
                    host_log("控制块已连接 pid=%lu", (unsigned long)pid);
                }
            }
        }
        {   /* 快照与托盘提示 */
            int ok = blk && nex_control_valid(blk) &&
                     (DWORD)(now - (DWORD)blk->heartbeat) < 3000;
            AcquireSRWLockExclusive(&g_snap_lock);
            if (ok) g_ctl_snap = *blk;
            g_ctl_snap_ok = ok;
            ReleaseSRWLockExclusive(&g_snap_lock);
            WCHAR tip[128];
            tray_tip_text(blk, ok, tip, 128);
            if (wcscmp(tip, last_tip) != 0) { tray_set_tip(tip); wcscpy(last_tip, tip); }
        }
        if (!blk) continue;

        LONG req = InterlockedExchange(&g_ctl_manual_req, 0);
        if (req == 1) {
            nex_control_post(blk, NEX_CONTROL_CMD_SHARE);
            InterlockedExchange(&g_ctl_manual, 1);
            host_log("托盘：暂时关闭独占");
        } else if (req == 2) {
            nex_control_post(blk, NEX_CONTROL_CMD_EXCLUSIVE);
            InterlockedExchange(&g_ctl_manual, 0);
            cs.yielded = 0;
            host_log("托盘：恢复独占（下次继续播放时生效）");
        }

        /* 用户回答了询问（或者记住过的「是」直接走到这里） */
        int act_kind = 0, act_yes = 0;
        WCHAR act_app[160], act_shown[64];
        LONG answer = InterlockedExchange(&g_prompt_answer, 0);
        if (answer) {
            act_kind = g_prompt_kind;
            act_yes = answer == 1;
            wcsncpy(act_app, g_prompt_app, 159); act_app[159] = 0;
            wcsncpy(act_shown, g_prompt_shown, 63); act_shown[63] = 0;
            if (InterlockedExchange(&g_prompt_remember, 0)) {
                remember_choice(act_app, act_yes);
                char a8[480];
                wide_to_utf8(act_app, a8, sizeof(a8));
                host_log("记住了：%s → %s", a8, act_yes ? "让出" : "保持独占");
            }
        }

        /* SMTC 服务重启（例如资源管理器重启）后旧的管理器对象就失效了：
           连续失败 10 次（5 s）就关掉重开；打开失败的话 30 s 重试一次 */
        if (!smtc_ok) {
            if (now - smtc_retry > 30000) {
                smtc_retry = now;
                smtc_ok = nex_smtc_open(&smtc);
                if (smtc_ok) { smtc_failures = 0; host_log("媒体会话监视已重新连上"); }
            }
            continue;
        }
        NexSmtcEntry e[24];
        int n = nex_smtc_poll(&smtc, e, 24);
        if (n < 0) {
            if (++smtc_failures >= 10) {
                nex_smtc_close(&smtc);
                smtc_ok = 0;
                smtc_retry = now - 30001;   /* 下一轮就重开 */
                host_log("媒体会话读取连续失败，重新连接");
            }
            continue;
        }
        smtc_failures = 0;
        NexConflictApp apps[24];
        for (int i = 0; i < n; i++) {
            apps[i].app = e[i].app;
            apps[i].kind = nex_app_kind(e[i].app);
            apps[i].playing = e[i].status == NEX_SMTC_PLAYING;
            apps[i].remembered = apps[i].kind != NEX_APP_SELF && apps[i].playing
                               ? remembered_for(e[i].app) : NEX_REMEMBER_NONE;
        }
        NexConflictStep step;
        nex_conflict_step(&cs, apps, n, auto_shared,
                          blk->state == NEX_CONTROL_STATE_EXCLUSIVE, blk->share_forced,
                          InterlockedCompareExchange(&g_ctl_manual, 0, 0),
                          g_prompt_busy || answer, now, &step);
        int pick = step.ask >= 0 ? step.ask : step.act >= 0 ? step.act : step.kept;
        if (pick >= 0) {
            WCHAR shown[64];
            char shown8[192], app8[480];
            nex_app_display_name(e[pick].app, shown, 64);
            wide_to_utf8(shown, shown8, sizeof(shown8));
            wide_to_utf8(e[pick].app, app8, sizeof(app8));
            if (step.act >= 0) {
                host_log("%s（%s）开始播放；按记住的选择让出", shown8, app8);
                act_kind = apps[pick].kind; act_yes = 1;
                wcsncpy(act_app, e[pick].app, 159); act_app[159] = 0;
                wcsncpy(act_shown, shown, 63); act_shown[63] = 0;
            } else if (step.kept >= 0) {
                host_log("%s（%s）开始播放；按记住的选择保持独占", shown8, app8);
            } else {
                host_log("%s（%s）开始播放，询问用户", shown8, app8);
                wcsncpy(g_prompt_app, e[pick].app, 159); g_prompt_app[159] = 0;
                wcsncpy(g_prompt_shown, shown, 63); g_prompt_shown[63] = 0;
                g_prompt_kind = apps[pick].kind;
                InterlockedExchange(&g_prompt_busy, 1);
                HANDLE t = CreateThread(NULL, 0, prompt_thread, NULL, 0, NULL);
                if (t) CloseHandle(t); else InterlockedExchange(&g_prompt_busy, 0);
            }
        }

        if (act_yes && !blk->share_forced) {
            char shown8[192];
            wide_to_utf8(act_shown, shown8, sizeof(shown8));
            WCHAR text[256];
            if (act_kind == NEX_APP_PLAYER) {
                int paused = 0;
                for (size_t k = 0; k < sizeof(kNexSelf) / sizeof(kNexSelf[0]); k++)
                    paused += nex_smtc_pause_matching(&smtc, kNexSelf[k]);
                nex_control_post(blk, NEX_CONTROL_CMD_SHARE);
                host_log("把独占交给 %s：%s，并让出声卡", shown8,
                         paused ? "已请网易云暂停" : "网易云没有媒体会话，没法自动暂停");
                if (paused)
                    _snwprintf(text, 256, L"网易云已暂停并让出声卡，%ls 可以独占播放了。", act_shown);
                else
                    _snwprintf(text, 256, L"网易云已让出独占，改走共享；请手动暂停网易云。");
            } else {
                nex_control_post(blk, NEX_CONTROL_CMD_SHARE);
                host_log("为 %s 关闭独占，网易云改走共享", shown8);
                _snwprintf(text, 256, L"网易云改走共享输出。%ls 如果还是没有声音，"
                           L"刷新或重新打开一下页面。", act_shown);
            }
            text[255] = 0;
            tray_notify(L"已关闭独占", text);
            cs.yielded = 1;
        } else if (answer && !act_yes) {
            char shown8[192];
            wide_to_utf8(act_shown, shown8, sizeof(shown8));
            host_log("保持独占（%s 这次不出声）", shown8);
        }

        if (step.release) {
            nex_control_post(blk, NEX_CONTROL_CMD_EXCLUSIVE);
            host_log("别的应用已停止播放 %lu 秒，下次继续播放时回到独占",
                     NEX_CONFLICT_QUIET_MS / 1000);
        }
    }
    return 0;
}
