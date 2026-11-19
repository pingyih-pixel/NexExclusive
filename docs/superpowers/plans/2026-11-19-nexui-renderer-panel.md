# nexui.dll：渲染进程内注入面板 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 设置面板改由新的 `nexui.dll` 在网易云渲染进程内注入，不再依赖 9333；挂接失败时宿主回退到现有 CDP 路径。

**Architecture:** 宿主把音频与界面拆成两条链路：看护定时器只管主进程的 `nexsink.dll`；新的界面工作线程跟踪主进程的子进程，向每个子进程注入 `nexui.dll`。`nexui.dll` 只在 `--type=renderer` 进程里动作，改 `cloudmusic.dll` 导入表里 5 个 libcef 函数，在主帧 `orpheus://` 页面的 V8 上下文里执行 `ui\panel.js`、`ui\simplify.js`，并用命名事件告诉宿主“已就绪”或“不支持”。宿主据此决定是否走 CDP。

**Tech Stack:** C（zig cc，x86_64-windows-gnu），CEF 91.2.3 C API 的结构体子集，Win32 命名事件，Node 契约测试。

**Spec:** 本计划依据本会话已确认的决定与验证结果，没有单独的规格文档：
- 独立 `nexui.dll`，只注渲染进程；音频 `nexsink.dll` 只注主进程，两条链路分开。
- CDP 保留为回退：渲染进程挂接失败（CEF 版本或结构体大小不符等）时再走 9333。
- 已验证（`work/cef-probe/`）：挂 `cloudmusic.dll` 导入表的 `cef_v8context_get_current_context` 等函数可在页面加载期（导航后约 0.3 s）进入主帧上下文；`eval` 正反对照正确；完整 `panel.js`、`simplify.js` 可加载；设置入口、源规格、原生进度条正常；刷新后能补注入；迷你播放器、桌面歌词不另起渲染进程。
- 音频 DLL 的构建与部署单独走一条线，不和界面验证混在一起。

## Global Constraints

- CEF 布局只认 91.2.3（branch 4472）：`sizeof(cef_v8context_t)` 必须为 `112`，`sizeof(cef_frame_t)` 必须为 `240`；不符则一律不调用任何成员，报“不支持”。
- 只挂这 5 个导入：`cef_v8context_get_current_context`、`cef_v8context_get_entered_context`、`cef_v8value_create_string`、`cef_v8value_create_function`、`cef_v8value_create_object`；只改 `cloudmusic.dll` 的导入表，导入模块名 `libcef.dll`（大小写不敏感）。
- 只在主帧、URL 以 `orpheus://` 开头的上下文里执行脚本。
- 命名事件：`Local\NexUI-Ready-<pid>`、`Local\NexUI-Unsupported-<pid>`，`<pid>` 为渲染进程十进制 PID。
- 回退超时 `20000` ms；单个子进程注入失败重试上限 `3` 次。
- 关闭开关：`nexui.dll` 同目录存在 `nexui.disable` 文件时，渲染进程内只报“不支持”、不挂接。
- 脚本路径固定为 `nexui.dll` 同目录下的 `ui\panel.js`、`ui\simplify.js`，UTF-8 读入。
- 日志 `nexui.log` 写在 `nexui.dll` 同目录；启动时超过 `524288` 字节则改名为 `nexui.prev.log`。
- 宿主仍带 `--remote-debugging-port=9333` 启动客户端（回退需要它）；是否去掉 9333 不在本计划范围。
- UI 验证不得部署新的 `nexsink.dll`：集成验证用暂存目录，音频 DLL 取自当前安装目录。
- 中文用户可见日志沿用宿主现有语气；代码注释英文或中文均可，跟随所在文件。

## Review Focus

1. **宿主后启动、客户端早已空闲**：注入后要等页面下一次原生调用才执行（实测空闲约 60 s 一次）。20 s 超时会先切到 CDP；之后渲染进程就绪必须能切回渲染进程路径。→ Task 1 `test_route_ready_after_timeout_wins`。
2. **CDP 与 nexui 都注入了同一页面**：面板不能出现两份。→ Task 6 场景 D 检查 `#nex-settings-module` 只有 1 个。
3. **宿主重启、客户端仍在运行**：渲染进程里 `nexui.dll` 已加载、事件已存在，宿主不得重复注入，且要直接认出“已就绪”。→ Task 4 `test_ui_child_already_loaded_is_not_reinjected`，Task 6 场景 E。
4. **GPU 等非渲染子进程也会被注入**：`nexui.dll` 在其中必须完全不动作、不建事件。→ Task 3 `test_dll_inert_outside_renderer`。
5. **客户端重启后 PID 全换**：旧子进程集合必须随主 PID 变化清空，新渲染进程要重新注入。→ Task 4 `test_ui_tracker_resets_on_new_main_pid`。

---

### Task 1: 共享约定头 nexui_shared.h（事件名 + 路由决策）

**Files:**
- Create: `mod/src/nexui_shared.h`
- Create: `mod/test/nexui_test.c`
- Modify: `mod/build.sh`（`build_test` 增加 `nexui_test.exe`）

**Interfaces:**
- Produces（宿主与 DLL 都包含此头，无 Windows 依赖，`wchar_t` 来自 `<wchar.h>`）：
  - `#define NEX_UI_ROUTE_TIMEOUT_MS 20000`
  - `#define NEX_UI_INJECT_MAX_FAILURES 3`
  - `int nex_ui_event_name(wchar_t *out, size_t n, const wchar_t *kind, unsigned long pid);` 写入 `L"Local\\NexUI-<kind>-<pid>"`，成功返回 1，缓冲不足返回 0。`kind` 取 `L"Ready"` 或 `L"Unsupported"`。
  - `enum { NEX_UI_ROUTE_WAIT = 0, NEX_UI_ROUTE_RENDERER = 1, NEX_UI_ROUTE_CDP = 2 };`
  - `typedef struct NexUiRouteInput { int dll_present; int renderer_injected; int renderer_ready; int renderer_unsupported; int inject_failures; unsigned long ms_since_client_seen; unsigned long ms_since_renderer_injected; } NexUiRouteInput;`
  - `int nex_ui_route_decide(const NexUiRouteInput *in);` 规则按优先级：`renderer_ready` → RENDERER；`!dll_present`、`renderer_unsupported`、`inject_failures >= NEX_UI_INJECT_MAX_FAILURES` → CDP；`renderer_injected && ms_since_renderer_injected >= 20000` → CDP；`!renderer_injected && ms_since_client_seen >= 20000` → CDP；其余 WAIT。`in == NULL` → CDP。

- [ ] **Step 1: 写失败测试** `mod/test/nexui_test.c`，沿用 `regression.c` 的 `CHECK` 宏风格，`main` 末尾打印 `ALL TESTS PASSED: %d failure(s)`。

```c
static void test_event_names(void) {
    wchar_t b[64];
    CHECK(nex_ui_event_name(b, 64, L"Ready", 1234) && wcscmp(b, L"Local\\NexUI-Ready-1234") == 0);
    CHECK(nex_ui_event_name(b, 64, L"Unsupported", 7) && wcscmp(b, L"Local\\NexUI-Unsupported-7") == 0);
    CHECK(!nex_ui_event_name(b, 10, L"Ready", 1234));
}
static NexUiRouteInput base(void) { NexUiRouteInput r = {1, 1, 0, 0, 0, 1000, 1000}; return r; }
static void test_route_rules(void) {
    NexUiRouteInput r = base();
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.renderer_ready = 1;              CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
    r = base(); r.dll_present = 0;     CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.renderer_unsupported = 1; CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.inject_failures = 2; CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.inject_failures = 3;             CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.ms_since_renderer_injected = 19999; CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.ms_since_renderer_injected = 20000; CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.renderer_injected = 0; r.ms_since_client_seen = 20000;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    CHECK(nex_ui_route_decide(NULL) == NEX_UI_ROUTE_CDP);
}
static void test_route_ready_after_timeout_wins(void) {
    NexUiRouteInput r = base();
    r.ms_since_renderer_injected = 90000; r.renderer_ready = 1;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
    r.renderer_unsupported = 1;        /* another renderer may say no; a ready one still wins */
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
}
```

- [ ] **Step 2: 在 `build_test` 里 `regression.exe` 之前加编译与运行 `nexui_test.exe`**（`"$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/nexui_test.exe" "$MOD_DIR/test/nexui_test.c"`），运行 `OUT_DIR=<scratch> ./build.sh test`。
Expected: 编译失败，提示找不到 `nexui_shared.h`。

- [ ] **Step 3: 实现 `mod/src/nexui_shared.h`**，函数用 `static __attribute__((unused))`（与 `audio_policy.h` 的 `NEX_AUDIO_POLICY_FN` 同做法），事件名用 `swprintf`/`_snwprintf` 并检查截断。

- [ ] **Step 4: 运行 `OUT_DIR=<scratch> ./build.sh test`**
Expected: `nexui_test` 打印 `ALL TESTS PASSED: 0 failure(s)`，其余测试仍全部通过。

- [ ] **Step 5: Commit** `git add mod/src/nexui_shared.h mod/test/nexui_test.c mod/build.sh && git commit -m "新增 nexui 共享约定：事件名与界面路由决策"`

---

### Task 2: 渲染进程内的纯逻辑 nexui_core.h

**Files:**
- Create: `mod/src/nexui_core.h`
- Modify: `mod/test/nexui_test.c`

**Interfaces:**
- Consumes: 无。
- Produces（需要 `<windows.h>`）：
  - `int nexui_is_renderer(const wchar_t *cmdline);` 命令行含独立参数 `--type=renderer`（后接空格、引号或结尾）时返回 1。
  - `int nexui_layout_ok(size_t context_size, size_t frame_size);` 仅 `112, 240` 返回 1。
  - `int nexui_should_inject(int is_main_frame, const char *url_utf8, int marker_present);` 主帧、URL 以 `orpheus://` 开头、且无标记时返回 1。
  - `int nexui_iat_patch(HMODULE module, const char *import_dll, const char *name, void *hook, void **original);` 在 `module` 的导入表里找 `import_dll`（大小写不敏感）下按名导入的 `name`，把槽位换成 `hook`；`original` 非空且 `*original == NULL` 时先存入原值。找到并改写返回 1，未导入返回 0。

- [ ] **Step 1: 写失败测试**

```c
static void test_renderer_detection(void) {
    CHECK(nexui_is_renderer(L"cloudmusic.exe --type=renderer --no-sandbox"));
    CHECK(nexui_is_renderer(L"\"cloudmusic.exe\" --lang=zh --type=renderer"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe --type=gpu-process"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe --type=renderer-helper"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe"));
    CHECK(!nexui_is_renderer(NULL));
}
static void test_layout_and_target(void) {
    CHECK(nexui_layout_ok(112, 240));
    CHECK(!nexui_layout_ok(120, 240));
    CHECK(!nexui_layout_ok(112, 232));
    CHECK(nexui_should_inject(1, "orpheus://orpheus/pub/app.html", 0));
    CHECK(!nexui_should_inject(1, "orpheus://orpheus/pub/app.html", 1));
    CHECK(!nexui_should_inject(0, "orpheus://orpheus/pub/app.html", 0));
    CHECK(!nexui_should_inject(1, "https://music.163.com/", 0));
    CHECK(!nexui_should_inject(1, NULL, 0));
}
static DWORD WINAPI fake_tick(void) { return 12345; }
static void test_iat_patch_own_import(void) {
    void *orig = NULL;
    HMODULE self = GetModuleHandleW(NULL);
    CHECK(nexui_iat_patch(self, "KERNEL32.dll", "GetTickCount", (void *)fake_tick, &orig) == 1);
    CHECK(GetTickCount() == 12345);
    CHECK(orig != NULL);
    CHECK(nexui_iat_patch(self, "kernel32.dll", "GetTickCount", orig, NULL) == 1);
    CHECK(GetTickCount() != 12345);
    CHECK(nexui_iat_patch(self, "kernel32.dll", "NoSuchExport", (void *)fake_tick, NULL) == 0);
    CHECK(nexui_iat_patch(self, "libcef.dll", "cef_v8context_get_current_context", (void *)fake_tick, NULL) == 0);
}
```

测试文件里要真正调用一次 `GetTickCount()`（上面已有），保证它出现在测试 exe 的导入表里。

- [ ] **Step 2: 运行 `OUT_DIR=<scratch> ./build.sh test`**
Expected: 编译失败，找不到 `nexui_core.h`。

- [ ] **Step 3: 实现 `mod/src/nexui_core.h`**。导入表遍历沿用 `work/cef-probe/cefprobe2.c` 的 `locate_slots` 写法（`IMAGE_DIRECTORY_ENTRY_IMPORT`、`OriginalFirstThunk`、跳过按序号导入）；改写槽位用 `VirtualProtect(PAGE_READWRITE)` + `InterlockedExchangePointer` 后恢复原保护属性。

- [ ] **Step 4: 运行测试**
Expected: `nexui_test` 全部通过；若 `GetTickCount` 在 zig 链接下不经过 `KERNEL32.dll` 导入（例如走 `api-ms-win-*`），改用测试 exe 实际导入的模块名，并在测试里注释说明。

- [ ] **Step 5: Commit** `git commit -m "新增 nexui 纯逻辑：渲染进程判定、CEF 布局检查、导入表改写"`

---

### Task 3: nexui.dll 本体与构建目标

**Files:**
- Create: `mod/src/nexui_cef.h`（CEF 91 C API 子集：`cef_base_ref_counted_t`、`cef_string_t`、`cef_v8context_t`、`cef_frame_t`、`cef_v8exception_t` 的成员顺序，照 `work/cef-probe/cefprobe2.c` 中已验证的布局）
- Create: `mod/src/nexui.c`
- Modify: `mod/build.sh`（新增 `build_ui` 生成 `nexui.dll`；`ui` 加入目标白名单与用法提示；`all` 依次 `build_sink; build_ui; build_host; build_audiohost`）
- Modify: `mod/test/nexui_test.c`

**Interfaces:**
- Consumes: Task 1 `nex_ui_event_name`；Task 2 全部函数。
- Produces: `nexui.dll`，无导出函数。行为约定（宿主依赖）：
  - 非渲染进程：只写一行 `nexui: inert in <type> process` 日志后返回，不挂接、不建事件。
  - 渲染进程里，以下任一情况创建并持有 `Local\NexUI-Unsupported-<pid>`：存在 `nexui.disable`；`ui\panel.js` 读不到；15 s 内等不到 `libcef.dll` 或 `cloudmusic.dll`；5 个导入一个都没改写成功；首次进入上下文时 `nexui_layout_ok` 为假。之后钩子只透传。
  - 首次在某个文档里 `panel.js` 执行无异常后，创建并持有 `Local\NexUI-Ready-<pid>`（整个进程生命周期不关句柄）。
  - 每个新文档（刷新、导航）检测不到 `window.__nexUiMarker` 时重新执行：先 `window.__nexUiMarker=1;`，再 `panel.js`，再 `simplify.js`（后者缺失只记日志）。
  - 钩子防重入（`InterlockedCompareExchange` 忙标志）；标记检查成功后 100 ms 内不再检查。
  - 每次注入、失败、异常消息写 `nexui.log`（Global Constraints 中的大小上限）。

- [ ] **Step 1: 写失败测试**（DLL 在非渲染进程里必须不动作）

```c
static void test_dll_inert_outside_renderer(const char *dll_path) {
    wchar_t name[64];
    HMODULE m = LoadLibraryA(dll_path);
    CHECK(m != NULL);
    Sleep(300);
    nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    HANDLE r = OpenEventW(SYNCHRONIZE, FALSE, name);
    nex_ui_event_name(name, 64, L"Unsupported", GetCurrentProcessId());
    HANDLE u = OpenEventW(SYNCHRONIZE, FALSE, name);
    CHECK(r == NULL && u == NULL);
    if (r) CloseHandle(r);
    if (u) CloseHandle(u);
}
```

`main(int argc, char **argv)`：`argc > 1` 时以 `argv[1]` 调用此测试，否则打印 `SKIP dll inert: no dll path` 并计为失败（防止被静默跳过）。`build_test` 运行 `nexui_test.exe "$OUT_DIR/nexui.dll"`，并在它之前调用 `build_ui`。

- [ ] **Step 2: 运行 `OUT_DIR=<scratch> ./build.sh test`**
Expected: 失败，`build_ui` 未定义或 `nexui.c` 不存在。

- [ ] **Step 3: 实现 `nexui_cef.h`、`nexui.c`、`build_ui`**。`build_ui` 为 `"$CC" cc "${FLAGS[@]}" -shared -o "$OUT_DIR/nexui.dll" "$MOD_DIR/src/nexui.c"`。`nexui.c` 的流程与钩子结构以 `work/cef-probe/cefprobe2.c` 为蓝本，去掉其中的探针专用部分（引导脚本里的 `fetch` 上报、`/probe/cmd` 轮询、`scripts.txt`），换成上面的固定脚本路径、事件与关闭开关。

- [ ] **Step 4: 运行测试与 `OUT_DIR=<scratch> ./build.sh all`**
Expected: 全部测试通过；`nexui.dll` 生成，编译无新增警告。

- [ ] **Step 5: Commit** `git commit -m "新增 nexui.dll：渲染进程内注入设置面板"`

---

### Task 4: 宿主界面链路（子进程跟踪 + 注入 + 路由）

**Files:**
- Modify: `mod/tools/nexhost.c`
  - `resolve_paths()`（约 131 行）：增加 `kUiDll` = `nexui.dll`
  - 新增界面链路段落（放在 `module_loaded()` 之后）
  - `watchdog_tick()`（约 2996 行起）：CDP 段改为受路由控制
  - `main()`：HTTP 线程之后启动界面工作线程
- Modify: `mod/test/source_probe_test.c` 所用的同一包含方式，新建 `mod/test/nexhost_ui_test.c`（`#define main nexhost_main_for_test` 后 `#include "../tools/nexhost.c"`）
- Modify: `mod/build.sh`（`build_test` 编译运行 `nexhost_ui_test.exe`，链接参数同 `source_probe_test`）
- Modify: `mod/tools/nexhost.contract.test.js`

**Interfaces:**
- Consumes: Task 1 `nex_ui_event_name`、`NexUiRouteInput`、`nex_ui_route_decide`；现有 `inject_dll`、`module_loaded`、`find_main_client`、`g_injected_pid`。
- Produces（宿主内部，供测试与 watchdog 使用）：
  - `static int find_child_pids(DWORD parent, const wchar_t *exe, DWORD *out, int cap);` Toolhelp 快照，返回父 PID 为 `parent`、映像名为 `exe` 的进程数。
  - `static int ui_event_exists(const wchar_t *kind, DWORD pid);` `OpenEventW(SYNCHRONIZE, …)` 成功即 1。
  - `typedef struct UiChild { DWORD pid; int injected; int failures; } UiChild;`
  - `typedef struct UiTracker { DWORD main_pid; UiChild child[32]; int count; ULONGLONG client_seen_at; ULONGLONG first_injected_at; } UiTracker;`
  - `static void ui_tracker_sync(UiTracker *t, DWORD main_pid, const DWORD *children, int n, ULONGLONG now);` 主 PID 变化时清空并记录 `client_seen_at`；加入新子进程、移除已消失的。
  - `static void ui_tracker_route_input(const UiTracker *t, int dll_present, ULONGLONG now, NexUiRouteInput *out);` 汇总各子进程的事件状态与计时。
  - `static volatile LONG g_ui_route;` 由界面线程写、看护线程读，取值为 `NEX_UI_ROUTE_*`。
  - `static DWORD WINAPI ui_thread(LPVOID);` 循环：读 `g_injected_pid`，同步子进程，对未注入且 `!module_loaded(pid, L"nexui.dll")` 的子进程调用 `inject_dll(pid, kUiDll)`（失败则 `failures++`，到上限不再试），已加载的直接视为已注入；计算路由并在变化时写一行宿主日志（`界面链路：渲染进程就绪` / `界面链路：回退到 CDP（原因）`）。无就绪渲染进程时每 100 ms 一轮，否则每 1000 ms。

- [ ] **Step 1: 写失败测试** `mod/test/nexhost_ui_test.c`

```c
static void test_find_child_pids(void) {
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"cmd.exe /c ping -n 3 127.0.0.1 >nul";
    CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    DWORD kids[8]; int n = find_child_pids(GetCurrentProcessId(), L"cmd.exe", kids, 8), hit = 0;
    for (int i = 0; i < n; i++) if (kids[i] == pi.dwProcessId) hit = 1;
    CHECK(hit);
    CHECK(find_child_pids(GetCurrentProcessId(), L"cloudmusic.exe", kids, 8) == 0);
    TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}
static void test_ui_event_exists(void) {
    wchar_t name[64];
    nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    CHECK(!ui_event_exists(L"Ready", GetCurrentProcessId()));
    HANDLE e = CreateEventW(NULL, TRUE, TRUE, name);
    CHECK(ui_event_exists(L"Ready", GetCurrentProcessId()));
    CloseHandle(e);
    CHECK(!ui_event_exists(L"Ready", GetCurrentProcessId()));
}
static void test_ui_tracker_resets_on_new_main_pid(void) {
    UiTracker t; memset(&t, 0, sizeof(t));
    DWORD a[] = {11, 12};
    ui_tracker_sync(&t, 10, a, 2, 1000);
    t.child[0].injected = 1; t.first_injected_at = 1500;
    DWORD b[] = {21};
    ui_tracker_sync(&t, 20, b, 1, 9000);
    CHECK(t.main_pid == 20 && t.count == 1 && t.child[0].pid == 21 && !t.child[0].injected);
    CHECK(t.client_seen_at == 9000 && t.first_injected_at == 0);
    DWORD c[] = {21, 22};
    ui_tracker_sync(&t, 20, c, 2, 9500);
    CHECK(t.count == 2 && t.client_seen_at == 9000);
    DWORD d[] = {22};
    ui_tracker_sync(&t, 20, d, 1, 9600);
    CHECK(t.count == 1 && t.child[0].pid == 22);
}
static void test_ui_child_already_loaded_is_not_reinjected(void) {
    /* the host's own process has kernel32 loaded: module_loaded is the gate ui_thread uses */
    CHECK(module_loaded(GetCurrentProcessId(), L"kernel32.dll"));
    CHECK(!module_loaded(GetCurrentProcessId(), L"nexui.dll"));
}
static void test_route_input_from_tracker(void) {
    UiTracker t; memset(&t, 0, sizeof(t));
    DWORD a[] = {GetCurrentProcessId()};
    ui_tracker_sync(&t, 1, a, 1, 1000);
    t.child[0].injected = 1; t.first_injected_at = 2000;
    NexUiRouteInput in;
    ui_tracker_route_input(&t, 1, 23000, &in);
    CHECK(in.renderer_injected && !in.renderer_ready && in.ms_since_renderer_injected == 21000);
    CHECK(nex_ui_route_decide(&in) == NEX_UI_ROUTE_CDP);
    wchar_t name[64]; nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    HANDLE e = CreateEventW(NULL, TRUE, TRUE, name);
    ui_tracker_route_input(&t, 1, 23000, &in);
    CHECK(in.renderer_ready && nex_ui_route_decide(&in) == NEX_UI_ROUTE_RENDERER);
    CloseHandle(e);
}
```

契约测试追加：`nexui.dll` 路径在 `resolve_paths` 中出现；`watchdog_tick` 内 CDP 分支以 `g_ui_route == NEX_UI_ROUTE_CDP` 为前提；`watchdog_tick` 内不出现 `kUiDll`（界面注入只在 `ui_thread`）；`nexsink.dll` 只注入 `find_main_client()` 返回的 PID（`watchdog_tick` 内 `inject_dll(pid, kSinkDll)` 保持唯一）。

- [ ] **Step 2: 运行 `OUT_DIR=<scratch> ./build.sh test` 与 `node tools/nexhost.contract.test.js`**
Expected: `nexhost_ui_test` 编译失败（函数未定义）；契约测试失败于新断言。

- [ ] **Step 3: 实现上述函数、`ui_thread` 与 `watchdog_tick` 的路由门控**。`watchdog_tick` 中：`g_ui_route == NEX_UI_ROUTE_CDP` 时执行现有 CDP 探测与注入；为 RENDERER 时跳过 CDP 并把 `g_cdp_miss_probes` 置 0；为 WAIT 时什么都不做。`restart_current_track()`（`/api/restart`）保持原样，CDP 不可用时仍返回 503。

- [ ] **Step 4: 运行全部测试、契约测试与 `./build.sh all`**
Expected: 全部通过，无新增警告。

- [ ] **Step 5: Commit** `git commit -m "宿主拆分音频与界面链路：渲染进程注入 nexui，失败回退 CDP"`

---

### Task 5: 打包、安装与文档

**Files:**
- Modify: `mod/tools/nexhost.c`：`remove_install_tree()` 与卸载文件表加入 `L"nexui.dll"`、`L"nexui.log"`、`L"nexui.prev.log"`；`install_payload_atomic()` 的 `base_files` 加入 `L"nexui.dll"`，`files[]` 容量相应加 1；文件头注释（约 43 行）的包内文件列表加 `nexui.dll`
- Modify: `release.sh`：`FILES` 加 `nexui.dll`；README 包内清单加 `nexui.dll`；Defender 排除示例同时排除 `nexui.dll`
- Modify: `mod/tools/nexhost.contract.test.js`：断言三处文件表都含 `nexui.dll`
- Modify: `docs/BUILDING.md`（新文件与 `ui` 目标）、`docs/DEVELOPMENT_RUNTIME.md`（两条链路、`nexui.log`、`nexui.disable`、排查表）、`README.md` 与 `README.en.md`（第 100 行附近 9333 段落改为“面板默认由 nexui.dll 在页面内加载，9333 仅作回退”）

- [ ] **Step 1: 写契约断言**（三处文件表含 `nexui.dll`）。
- [ ] **Step 2: 运行 `node tools/nexhost.contract.test.js`**，Expected: FAIL。
- [ ] **Step 3: 修改文件表、`release.sh` 与文档。**
- [ ] **Step 4: 运行契约测试、`./build.sh test`、`./build.sh all`**，Expected: 全部通过。
- [ ] **Step 5: Commit** `git commit -m "打包与文档纳入 nexui.dll"`

---

### Task 6: 真实客户端集成验证（不部署新音频 DLL）

**Files:**
- 无源码改动；结果写入 `docs/AUDIO_CORRECTNESS_AUDIT_2026-11-16.md` 之外的新文件 `docs/NEXUI_VERIFICATION_2026-11-19.md`

**暂存目录准备：** `OUT_DIR=<scratch>/stage ./build.sh all` 后，用 `%LOCALAPPDATA%\NexExclusive\nexsink.dll` 与 `hook.cfg` 覆盖暂存目录里的同名文件，删去暂存目录的 `NexAudioHost.exe`；确认已安装的 `NexExclusive.exe` 未运行；以环境变量 `NEX_NO_REDIRECT=1` 从暂存目录启动 `NexExclusive.exe --console`。

- [ ] **场景 A：宿主拉起客户端（9333 开着）**。Expected：`host.log` 出现 `界面链路：渲染进程就绪`，没有 CDP 注入日志；`nexui.log` 记录注入与 `panel.js` 成功；设置入口可见；播放 15 s 进度持续前进。
- [ ] **场景 B：用户先开客户端、宿主后到**（先退出宿主与客户端，直接启动 `cloudmusic.exe`，再启动暂存宿主）。Expected：面板在页面下一次原生调用后出现，记录从宿主启动到出现的时间；20 s 后宿主日志写回退到 CDP 且报面板不可用（9333 未开），就绪后切回渲染进程路径。
- [ ] **场景 C：页面刷新与客户端重启**。Expected：刷新后面板自动回来；客户端退出后宿主拉起新实例，新渲染进程 PID 被注入，面板回来。
- [ ] **场景 D：强制回退**（在暂存目录放 `nexui.disable`，重启客户端）。Expected：`Unsupported` 事件存在，宿主日志写回退原因，面板经 CDP 出现；页面里 `document.querySelectorAll('#nex-settings-module').length` 为 1（用 CDP 或面板自检读取）。删除 `nexui.disable`、再次重启客户端后，两条路都可能生效，仍只有 1 个面板。
- [ ] **场景 E：宿主重启、客户端不动**。Expected：新宿主不再注入 `nexui.dll`（`module_loaded` 为真），直接判定就绪。
- [ ] **场景 F：音频链路不受影响**。Expected：`sink.log` 正常出现 `sink ENGAGED`，`nexsink.dll` 只在主进程模块列表里，`nexui.dll` 只在子进程里。
- [ ] **收尾**：退出暂存宿主与客户端，删去暂存目录里的 `nexui.disable`，把各场景结果与耗时写进 `docs/NEXUI_VERIFICATION_2026-11-19.md`，Commit `git commit -m "记录 nexui 渲染进程注入的真机验证结果"`。
