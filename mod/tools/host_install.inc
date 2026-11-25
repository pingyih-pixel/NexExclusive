/* ---- 安装 / 卸载 -------------------------------------------------------- */
/* 装到 %LOCALAPPDATA%\NexExclusive，不需要管理员。注册 HKCU 卸载项，所以
   也能从「设置 → 应用」卸掉。自启走 HKCU\...\Run，同样不需要提权。
   不做 IFEO / DLL 代理劫持那类无感注入：省一次点击，换来杀软弹窗和
   「这是不是病毒」的怀疑，不划算。 */

/* APP_NAME / APP_VERSION / UNINST_KEY / RUN_KEY are at the top of nexhost.c:
   build.sh and release.sh read the version from there. */

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

static int sha256_file_hex(const wchar_t *path, char out[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_len = 0, result = 0;
    BYTE digest[32];
    HANDLE file = INVALID_HANDLE_VALUE;
    int ok = 0;
    out[0] = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                          sizeof(object_len), &result, 0) != 0 || !object_len) goto done;
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!object) goto done;
    if (BCryptCreateHash(alg, &hash, object, object_len, NULL, 0, 0) != 0) goto done;

    BYTE buffer[64 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) goto done;
        if (!got) break;
        if (BCryptHashData(hash, buffer, got, 0) != 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) goto done;
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    ok = 1;

done:
    if (hash) BCryptDestroyHash(hash);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) out[0] = 0;
    return ok;
}

static int verify_package_dir(const wchar_t *dir, int require_manifest) {
    WCHAR manifest[MAX_PATH];
    join_dir(manifest, MAX_PATH, dir, L"manifest.txt");
    FILE *file = _wfopen(manifest, L"rb");
    if (!file) {
        if (require_manifest) log_w("校验失败：缺少 manifest %s", manifest);
        return require_manifest ? 0 : 1;
    }
    char line[512], expected[65], rel[260];
    int count = 0;
    int ok = 1;
    while (fgets(line, sizeof(line), file)) {
        expected[0] = 0;
        rel[0] = 0;
        if (sscanf(line, "%64s %259[^\r\n]", expected, rel) != 2) continue;
        char *name = rel;
        while (*name == ' ' || *name == '\t') name++;
        if (*name == '*') name++;
        if (strstr(name, "..") || name[0] == '/' || name[0] == '\\') { ok = 0; break; }
        WCHAR path[MAX_PATH];
        int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, path, MAX_PATH);
        if (w <= 0) { ok = 0; break; }
        WCHAR full[MAX_PATH];
        join_dir(full, MAX_PATH, dir, path);
        char actual[65];
        if (!sha256_file_hex(full, actual) || _stricmp(actual, expected) != 0) {
            log_w("校验失败：文件摘要不匹配 %s", full);
            ok = 0;
            break;
        }
        count++;
    }
    fclose(file);
    if (!count) ok = 0;
    if (!ok) host_log("安装包校验失败");
    else host_log("安装包校验通过：%d 个文件", count);
    return ok;
}

static int do_verify(void) {
    WCHAR dir[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, g_base, -1, dir, MAX_PATH) <= 0)
        return 1;
    return verify_package_dir(dir, 1) ? 0 : 1;
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
    HANDLE handles[32];
    int n = 0;
    memset(handles, 0, sizeof(handles));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"NexExclusive.exe") != 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (h && n < (int)(sizeof(handles) / sizeof(handles[0]))) {
                TerminateProcess(h, 0);
                handles[n++] = h;
            } else if (h) CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (int i = 0; i < n; i++) {
        WaitForSingleObject(handles[i], 3000);
        CloseHandle(handles[i]);
    }
    return n;
}

static int stop_client_processes(void) {
    DWORD pid = find_main_client();
    if (!pid) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                 FALSE, pid);
    if (!process) return 0;
    HWND hwnd = client_window(pid, 0);
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    DWORD wait = WaitForSingleObject(process, 5000);
    if (wait != WAIT_OBJECT_0) {
        host_log("网易云未在 5 秒内退出，终止 pid=%lu", (unsigned long)pid);
        TerminateProcess(process, 0);
        wait = WaitForSingleObject(process, 3000);
    }
    CloseHandle(process);
    return wait == WAIT_OBJECT_0;
}

static void remove_install_tree(const wchar_t *dir) {
    static const wchar_t *files[] = {
        L"NexExclusive.exe", L"NexAudioHost.exe", L"nexsink.dll", L"nexui.dll", L"hook.cfg",
        L"ui.json", L"manifest.txt", L"ui\\panel.js", L"ui\\simplify.js", L"ui\\app.html"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        WCHAR path[MAX_PATH];
        join_dir(path, MAX_PATH, dir, files[i]);
        DeleteFileW(path);
    }
    { WCHAR ui[MAX_PATH]; join_dir(ui, MAX_PATH, dir, L"ui"); RemoveDirectoryW(ui); }
    RemoveDirectoryW(dir);
}

static int install_payload_atomic(const wchar_t *src, const wchar_t *dst) {
    static const wchar_t *base_files[] = {
        L"NexExclusive.exe", L"NexAudioHost.exe", L"nexsink.dll", L"nexui.dll", L"hook.cfg",
        L"ui.json", L"ui\\panel.js", L"ui\\simplify.js", L"ui\\app.html"
    };
    const wchar_t *files[10];
    size_t count = sizeof(base_files) / sizeof(base_files[0]);
    for (size_t i = 0; i < count; i++) files[i] = base_files[i];
    WCHAR source_manifest[MAX_PATH];
    join_dir(source_manifest, MAX_PATH, src, L"manifest.txt");
    if (file_exists_w(source_manifest)) files[count++] = L"manifest.txt";

    WCHAR stage[MAX_PATH], backup[MAX_PATH];
    _snwprintf(stage, MAX_PATH, L"%ls.staging-%lu", dst, (unsigned long)GetCurrentProcessId());
    _snwprintf(backup, MAX_PATH, L"%ls.backup-%lu", dst, (unsigned long)GetCurrentProcessId());
    stage[MAX_PATH - 1] = backup[MAX_PATH - 1] = 0;
    remove_install_tree(stage);
    remove_install_tree(backup);
    if (!CreateDirectoryW(stage, NULL)) return 0;
    { WCHAR ui[MAX_PATH]; join_dir(ui, MAX_PATH, stage, L"ui"); if (!CreateDirectoryW(ui, NULL)) { remove_install_tree(stage); return 0; } }

    for (size_t i = 0; i < count; i++) {
        if (!copy_into(src, stage, files[i])) {
            remove_install_tree(stage);
            return 0;
        }
    }
    if (!CreateDirectoryW(backup, NULL)) { remove_install_tree(stage); return 0; }
    { WCHAR ui[MAX_PATH]; join_dir(ui, MAX_PATH, backup, L"ui"); CreateDirectoryW(ui, NULL); }

    size_t moved = 0, touched = 0;
    int had_backup[8] = {0};
    for (size_t i = 0; i < count; i++) {
        WCHAR dest[MAX_PATH], old[MAX_PATH], fresh[MAX_PATH];
        join_dir(dest, MAX_PATH, dst, files[i]);
        join_dir(old, MAX_PATH, backup, files[i]);
        join_dir(fresh, MAX_PATH, stage, files[i]);
        if (file_exists_w(dest)) {
            if (!MoveFileExW(dest, old, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) goto rollback;
            had_backup[i] = 1;
            touched = i + 1;
        }
        if (!MoveFileExW(fresh, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) goto rollback;
        moved = i + 1;
    }
    remove_install_tree(stage);
    remove_install_tree(backup);
    return 1;

rollback:
    if (touched < moved) touched = moved;
    for (size_t j = touched; j > 0; j--) {
        size_t i = j - 1;
        WCHAR dest[MAX_PATH], old[MAX_PATH];
        join_dir(dest, MAX_PATH, dst, files[i]);
        join_dir(old, MAX_PATH, backup, files[i]);
        DeleteFileW(dest);
        if (had_backup[i]) MoveFileExW(old, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    remove_install_tree(stage);
    remove_install_tree(backup);
    return 0;
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

    if (!verify_package_dir(src, 0)) {
        host_log("安装中止：安装源文件校验失败");
        if (SUCCEEDED(co)) CoUninitialize();
        return 1;
    }

    /* 覆盖正在运行的文件前，先让宿主和网易云真正退出。 */
    if (stop_other_hosts()) host_log("已停止运行中的旧宿主");
    if (stop_client_processes()) host_log("已停止运行中的网易云客户端");

    if (!install_payload_atomic(src, dst)) {
        host_log("安装失败：文件提交未完成，旧安装保持不变");
        if (SUCCEEDED(co)) CoUninitialize();
        return 1;
    }

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

    /* 先停掉正在跑的宿主和网易云，否则文件删不掉。 */
    if (stop_other_hosts()) host_log("已停止运行中的宿主");
    if (stop_client_processes()) host_log("已停止运行中的网易云客户端");

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
        L"nexsink.dll", L"nexui.dll", L"NexAudioHost.exe", L"hook.cfg", L"ui.json", L"host.cfg",
        L"manifest.txt",
        L"ui\\panel.js", L"ui\\simplify.js", L"ui\\app.html", L"apps.cfg",
        L"host.log", L"sink.log", L"sink.prev.log", L"nexui.log", L"nexui.prev.log",
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
