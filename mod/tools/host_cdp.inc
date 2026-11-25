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
static int cdp_cmd_capture(SOCKET s, int id, const char *method, const char *params,
                           char *response, size_t responsen) {
    size_t cap = strlen(method) + (params ? strlen(params) : 0) + 128;
    char *msg = (char *)malloc(cap);
    if (!msg) return 0;
    if (params)
        snprintf(msg, cap, "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, params);
    else
        snprintf(msg, cap, "{\"id\":%d,\"method\":\"%s\"}", id, method);

    char local_resp[2048];
    if (!response || !responsen) { response = local_resp; responsen = sizeof(local_resp); }
    response[0] = 0;
    int ok = ws_send_text(s, msg, response, responsen);
    free(msg);
    if (ok && (!response[0] || !strstr(response, "\"id\""))) {
        host_log("CDP %s �����ؿɽ�����", method);
        return 0;
    }
    const char *resp = response;
    if (ok && (strstr(response, "\"error\"") || strstr(response, "\"exceptionDetails\""))) {
        host_log("CDP %s 被拒绝: %.200s", method, resp);
        return 0;
    }
    return ok;
}

/* 在页面里执行一段 JS。 */
static int cdp_cmd(SOCKET s, int id, const char *method, const char *params) {
    char resp[2048];
    return cdp_cmd_capture(s, id, method, params, resp, sizeof(resp));
}

static int cdp_eval(SOCKET s, const char *js, int id) {
    char *esc = json_escape(js);
    if (!esc) return 0;
    size_t cap = strlen(esc) + 256;
    char *params = (char *)malloc(cap);
    if (!params) { free(esc); return 0; }
    snprintf(params, cap,
             "{\"expression\":\"%s\",\"returnByValue\":true,\"awaitPromise\":true}", esc);
    char resp[4096];
    int ok = cdp_cmd_capture(s, id, "Runtime.evaluate", params, resp, sizeof(resp));
    if (ok && strstr(resp, "\"type\":\"boolean\"") && strstr(resp, "\"value\":false")) {
        host_log("CDP Runtime.evaluate ���� false: %.200s", resp);
        ok = 0;
    }
    free(params);
    free(esc);
    return ok;
}

static int cdp_document_has_panel(const char *page) {
    SOCKET s;
    char resp[4096];
    if (!page || !page[0]) return 0;
    s = ws_open(page);
    if (s == INVALID_SOCKET) return 0;
    int ok = cdp_cmd_capture(s, 1, "Runtime.evaluate",
                             "{\"expression\":\"typeof window.__nexPanelLoaded === 'string'\",\"returnByValue\":true}",
                             resp, sizeof(resp));
    closesocket(s);
    return ok && strstr(resp, "\"value\":true") != NULL;
}

/* Ask the client page to rebuild the current audio stream. The page owns the
   queue and progress bar; toggling playback makes the client run its normal
   Stop/Start path, where the injected DLL sees the new config generation. The
   current position and the original play/pause state are preserved. */
static int touch_config_file(void) {
    HANDLE f = CreateFileA(kHookCfg, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        host_log("重开当前曲目: 无法标记配置代次 %lu", GetLastError());
        return 0;
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    int ok = SetFileTime(f, NULL, NULL, &now) != FALSE;
    if (!ok) host_log("重开当前曲目: 配置代次标记失败 %lu", GetLastError());
    CloseHandle(f);
    return ok;
}

/* CDP 关着（默认）或连不上时，用客户端自己的媒体会话做同样的暂停 → 继续：
   正在播放就暂停、等它真停下、再播放；本来就暂停着则只标记配置代次，下次
   继续播放时按新配置重建。 */
static int smtc_status_of(NexSmtc *s, const WCHAR *match) {
    NexSmtcEntry e[16];
    int n = nex_smtc_poll(s, e, 16);
    for (int i = 0; i < n; i++)
        if (nex_wcs_contains_ci(e[i].app, match)) return e[i].status;
    return -1;
}

static int restart_current_track_smtc(void) {
    NexSmtc s;
    if (!nex_smtc_open(&s)) { host_log("重开当前曲目: 媒体会话不可用（%s）", s.error); return 0; }
    int ok = 0;
    int st = smtc_status_of(&s, L"cloudmusic");
    if (st < 0) {
        host_log("重开当前曲目: 没找到网易云的媒体会话");
    } else if (!touch_config_file()) {
        /* 已记录原因 */
    } else if (st != NEX_SMTC_PLAYING) {
        ok = 1;
        host_log("重开当前曲目: 当前暂停中，下次继续播放时按新配置重建");
    } else {
        nex_smtc_send_matching(&s, L"cloudmusic", NEX_SMTC_CMD_PAUSE);
        for (int i = 0; i < 20 && smtc_status_of(&s, L"cloudmusic") == NEX_SMTC_PLAYING; i++) Sleep(50);
        nex_smtc_send_matching(&s, L"cloudmusic", NEX_SMTC_CMD_PLAY);
        for (int i = 0; i < 20 && !ok; i++) {
            ok = smtc_status_of(&s, L"cloudmusic") == NEX_SMTC_PLAYING;
            if (!ok) Sleep(50);
        }
        host_log("重开当前曲目（媒体会话）%s", ok ? "完成" : "失败：没有恢复播放");
    }
    nex_smtc_close(&s);
    return ok;
}

static int restart_current_track(void) {
    char page[128];
    if (!g_cdp_enabled || !cdp_page_id(page, sizeof(page))) return restart_current_track_smtc();
    if (!touch_config_file()) return 0;
    SOCKET s = ws_open(page);
    if (s == INVALID_SOCKET) return 0;
    const char *js =
        "(function(){"
        "function visibleButton(){"
        "var buttons=document.querySelectorAll('[data-testid=\\\"tid_playbar_play_btn\\\"]');"
        "for(var i=0;i<buttons.length;i++){"
        "var rect=buttons[i].getBoundingClientRect();"
        "if(rect.width>0&&rect.height>0)return buttons[i];"
        "}"
        "return null;"
        "}"
        "var button=visibleButton();"
        "if(!button)return false;"
        "var icon=button.querySelector('[role=\\\"img\\\"]');"
        "var label=((icon&&icon.getAttribute('aria-label'))||'').toLowerCase();"
        "var wasPlaying=label==='pause'||label==='暂停';"
        "button.click();"
        "return new Promise(function(resolve){"
        "setTimeout(function(){"
        "var b=visibleButton();"
        "if(!b){resolve(false);return;}"
        "var i=b.querySelector('[role=\\\"img\\\"]');"
        "var now=((i&&i.getAttribute('aria-label'))||'').toLowerCase();"
        "var nowPlaying=now==='pause'||now==='暂停';"
        "if(nowPlaying!==wasPlaying)b.click();"
        "setTimeout(function(){"
        "var check=visibleButton();"
        "if(!check){resolve(false);return;}"
        "var ci=check.querySelector('[role=\\\"img\\\"]');"
        "var finalLabel=((ci&&ci.getAttribute('aria-label'))||'').toLowerCase();"
        "var finalPlaying=finalLabel==='pause'||finalLabel==='暂停';"
        "resolve(finalPlaying===wasPlaying);"
        "},40);"
        "},120);"
        "});"
        "})()";
    int ok = cdp_eval(s, js, 1);
    closesocket(s);
    if (ok) host_log("已请求重开当前曲目");
    return ok;
}

/* 注册成"每次文档加载都执行"。保存 identifier，下一次更新前先移除旧脚本，
   避免宿主长时间运行后在页面加载链上累积副本。 */
static int cdp_add_script(SOCKET s, int id, const char *js,
                          char *script_id, size_t script_id_n) {
    char *esc = json_escape(js);
    if (!esc) return 0;
    size_t cap = strlen(esc) + 64;
    char *params = (char *)malloc(cap);
    if (!params) { free(esc); return 0; }
    snprintf(params, cap, "{\"source\":\"%s\"}", esc);
    size_t msg_cap = strlen(params) + 128;
    char *msg = (char *)malloc(msg_cap);
    if (!msg) { free(params); free(esc); return 0; }
    snprintf(msg, msg_cap, "{\"id\":%d,\"method\":\"Page.addScriptToEvaluateOnNewDocument\",\"params\":%s}", id, params);
    char resp[2048];
    int ok = ws_send_text(s, msg, resp, sizeof(resp));
    if (ok && strstr(resp, "\"error\"")) {
        host_log("CDP Page.addScriptToEvaluateOnNewDocument 被拒绝: %.200s", resp);
        ok = 0;
    }
    if (ok && script_id && script_id_n) {
        const char *p = strstr(resp, "\"identifier\":\"");
        if (p) {
            p += strlen("\"identifier\":\"");
            size_t i = 0;
            while (*p && *p != '\"' && i + 1 < script_id_n) script_id[i++] = *p++;
            script_id[i] = 0;
            if (!i) ok = 0;
        } else ok = 0;
    }
    free(msg);
    free(params);
    free(esc);
    return ok;
}

static int cdp_remove_script(SOCKET s, int id, const char *script_id) {
    if (!script_id || !script_id[0]) return 1;
    char *esc = json_escape(script_id);
    if (!esc) return 0;
    char params[256];
    snprintf(params, sizeof(params), "{\"identifier\":\"%s\"}", esc);
    int ok = cdp_cmd(s, id, "Page.removeScriptToEvaluateOnNewDocument", params);
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
    if (g_panel_script_id[0]) {
        cdp_remove_script(s, id++, g_panel_script_id);
        g_panel_script_id[0] = 0;
    }
    if (g_simplify_script_id[0]) {
        cdp_remove_script(s, id++, g_simplify_script_id);
        g_simplify_script_id[0] = 0;
    }
    char *panel = read_file(kPanelJs);
    if (panel) {
        if (cdp_eval(s, panel, id++)) ok = 1;
        if (cdp_add_script(s, id++, panel, g_panel_script_id, sizeof(g_panel_script_id))) ok = 1;
        free(panel);
    }
    char *simp = read_file(kSimplifyJs);
    if (simp) {
        if (cdp_eval(s, simp, id++)) ok = 1;
        if (cdp_add_script(s, id++, simp, g_simplify_script_id, sizeof(g_simplify_script_id))) ok = 1;
        free(simp);
    }
    closesocket(s);
    if (!ok) host_log("界面注入失败 page=%s", page);
    return ok;
}
