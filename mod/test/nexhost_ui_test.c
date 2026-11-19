/* Host side of the UI chain: child tracking, event checks, route input. */
#define main nexhost_main_for_test
#include "../tools/nexhost.c"
#undef main

static int g_fail;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void test_find_child_pids(void) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"cmd.exe /c ping -n 3 127.0.0.1 >nul";
    DWORD kids[8];
    int n, hit = 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    n = find_child_pids(GetCurrentProcessId(), L"cmd.exe", kids, 8);
    for (int i = 0; i < n; i++) if (kids[i] == pi.dwProcessId) hit = 1;
    CHECK(hit);
    CHECK(find_child_pids(GetCurrentProcessId(), L"cloudmusic.exe", kids, 8) == 0);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("PASS find_child_pids: children of a parent, filtered by image name\n");
}

static void test_ui_event_exists(void) {
    wchar_t name[64];
    HANDLE e;
    nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    CHECK(!ui_event_exists(L"Ready", GetCurrentProcessId()));
    e = CreateEventW(NULL, TRUE, TRUE, name);
    CHECK(ui_event_exists(L"Ready", GetCurrentProcessId()));
    CloseHandle(e);
    CHECK(!ui_event_exists(L"Ready", GetCurrentProcessId()));
    printf("PASS ui_event_exists: follows the holder's lifetime\n");
}

static void test_ui_tracker_resets_on_new_main_pid(void) {
    UiTracker t;
    DWORD a[] = {11, 12}, b[] = {21}, c[] = {21, 22}, d[] = {22};
    memset(&t, 0, sizeof(t));
    ui_tracker_sync(&t, 10, a, 2, 1000);
    t.child[0].injected = 1;
    t.first_injected_at = 1500;
    ui_tracker_sync(&t, 20, b, 1, 9000);
    CHECK(t.main_pid == 20 && t.count == 1 && t.child[0].pid == 21 && !t.child[0].injected);
    CHECK(t.client_seen_at == 9000 && t.first_injected_at == 0);
    ui_tracker_sync(&t, 20, c, 2, 9500);
    CHECK(t.count == 2 && t.client_seen_at == 9000);
    ui_tracker_sync(&t, 20, d, 1, 9600);
    CHECK(t.count == 1 && t.child[0].pid == 22);
    printf("PASS ui tracker: a new main pid starts a fresh child set; vanished children drop out\n");
}

static void test_ui_child_already_loaded_is_not_reinjected(void) {
    /* module_loaded is the gate ui_thread uses before injecting a child */
    CHECK(module_loaded(GetCurrentProcessId(), L"kernel32.dll"));
    CHECK(!module_loaded(GetCurrentProcessId(), L"nexui.dll"));
    printf("PASS module_loaded gates re-injection\n");
}

static void test_route_input_from_tracker(void) {
    UiTracker t;
    NexUiRouteInput in;
    DWORD a[1];
    wchar_t name[64];
    HANDLE e;
    a[0] = GetCurrentProcessId();
    memset(&t, 0, sizeof(t));
    ui_tracker_sync(&t, 1, a, 1, 1000);
    t.child[0].injected = 1;
    t.first_injected_at = 2000;
    ui_tracker_route_input(&t, 1, 23000, &in);
    CHECK(in.dll_present && in.renderer_injected && !in.renderer_ready);
    CHECK(in.ms_since_renderer_injected == 21000 && in.ms_since_client_seen == 22000);
    CHECK(nex_ui_route_decide(&in) == NEX_UI_ROUTE_CDP);
    nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    e = CreateEventW(NULL, TRUE, TRUE, name);
    ui_tracker_route_input(&t, 1, 23000, &in);
    CHECK(in.renderer_ready && nex_ui_route_decide(&in) == NEX_UI_ROUTE_RENDERER);
    CloseHandle(e);
    t.child[0].injected = 0;
    t.child[0].failures = 3;
    ui_tracker_route_input(&t, 1, 1500, &in);
    CHECK(in.inject_failures == 3 && nex_ui_route_decide(&in) == NEX_UI_ROUTE_CDP);
    printf("PASS route input: timing, ready event and failure count reach the route rule\n");
}

/* The CDP route can last for the whole client session (nexui.disable, an
   unsupported CEF): only WAIT deserves the fast cadence. */
static void test_poll_interval_backs_off_outside_wait(void) {
    CHECK(ui_poll_interval_ms(NEX_UI_ROUTE_WAIT) == 100);
    CHECK(ui_poll_interval_ms(NEX_UI_ROUTE_CDP) == 1000);
    CHECK(ui_poll_interval_ms(NEX_UI_ROUTE_RENDERER) == 1000);
    printf("PASS ui_thread polls fast only while waiting\n");
}

/* ---- one injection round, with fake process operations -------------------- */
typedef struct FakeProcs {
    DWORD loaded[4];        int nloaded;
    DWORD refuse[4];        int nrefuse;
    DWORD client_parent[4]; int nparent;
    DWORD injected[16];     int ninjected;
} FakeProcs;

static int fake_has(const DWORD *list, int n, DWORD pid) {
    for (int i = 0; i < n; i++) if (list[i] == pid) return 1;
    return 0;
}
static int fake_loaded(void *ctx, DWORD pid) {
    FakeProcs *f = (FakeProcs *)ctx;
    return fake_has(f->loaded, f->nloaded, pid);
}
static int fake_inject(void *ctx, DWORD pid) {
    FakeProcs *f = (FakeProcs *)ctx;
    if (fake_has(f->refuse, f->nrefuse, pid)) return 0;
    f->injected[f->ninjected++] = pid;
    return 1;
}
static int fake_parent(void *ctx, DWORD pid) {
    FakeProcs *f = (FakeProcs *)ctx;
    return fake_has(f->client_parent, f->nparent, pid);
}

static void test_round_skips_loaded_and_caps_failures(void) {
    FakeProcs f;
    UiOps ops = {fake_loaded, fake_inject, fake_parent, &f};
    UiTracker t;
    DWORD kids[] = {11, 12, 13};
    memset(&f, 0, sizeof(f));
    memset(&t, 0, sizeof(t));
    f.loaded[f.nloaded++] = 11;          /* left by a previous host */
    f.refuse[f.nrefuse++] = 13;          /* e.g. a protected helper */
    ui_tracker_sync(&t, 10, kids, 3, 1000);
    for (int round = 0; round < 5; round++) ui_round_inject(&t, &ops, 2000 + round);
    CHECK(t.child[0].injected && t.child[1].injected && !t.child[2].injected);
    CHECK(f.ninjected == 1 && f.injected[0] == 12);     /* 11 never re-injected */
    CHECK(t.child[2].failures == NEX_UI_INJECT_MAX_FAILURES); /* capped, not 5 */
    CHECK(t.first_injected_at == 2000);
    printf("PASS ui round: loaded children are not re-injected, failures stop at the cap\n");
}

/* While the shell launcher is still alive, find_main_client may return it and
   the real main (audio) process shows up as its "child". */
static void test_round_skips_child_that_is_a_main_process(void) {
    FakeProcs f;
    UiOps ops = {fake_loaded, fake_inject, fake_parent, &f};
    UiTracker t;
    NexUiRouteInput in;
    DWORD kids[] = {20};
    memset(&f, 0, sizeof(f));
    memset(&t, 0, sizeof(t));
    f.client_parent[f.nparent++] = 20;
    ui_tracker_sync(&t, 5, kids, 1, 1000);
    ui_round_inject(&t, &ops, 1100);
    ui_round_inject(&t, &ops, 1200);
    CHECK(f.ninjected == 0);
    CHECK(t.child[0].skip && !t.child[0].injected && t.child[0].failures == 0);
    ui_tracker_route_input(&t, 1, 1300, &in);
    CHECK(!in.renderer_injected && in.inject_failures == 0);
    printf("PASS ui round: a child that is itself a client main process is left alone\n");
}

/* ---- crash-loop guard ----------------------------------------------------- */
static void test_crash_guard_trips_after_two_quick_exits(void) {
    UiGuard g;
    memset(&g, 0, sizeof(g));
    CHECK(!ui_guard_client_gone(&g, 0, 5000));            /* nexui never ran there */
    CHECK(g.quick_exits == 0);
    CHECK(!ui_guard_client_gone(&g, 10000, 12000));       /* died 2 s after injection */
    CHECK(g.quick_exits == 1 && !g.tripped);
    CHECK(!ui_guard_client_gone(&g, 20000, 60000));       /* lived 40 s: streak resets */
    CHECK(g.quick_exits == 0);
    CHECK(!ui_guard_client_gone(&g, 70000, 71000));
    CHECK(ui_guard_client_gone(&g, 80000, 80000 + UI_GUARD_WINDOW_MS - 1)); /* second in a row */
    CHECK(g.tripped);
    CHECK(!ui_guard_client_gone(&g, 90000, 90500));        /* trips once */
    printf("PASS crash guard: two quick exits in a row after injection stop nexui\n");
}

static void test_route_reason_codes(void) {
    NexUiRouteInput in;
    memset(&in, 0, sizeof(in));
    in.dll_present = 1; in.renderer_injected = 1; in.ms_since_renderer_injected = 20000;
    CHECK(ui_route_reason_code(&in, 0) == UI_REASON_TIMEOUT);
    CHECK(ui_route_reason_code(&in, 1) == UI_REASON_GUARD);
    in.renderer_unsupported = 1;
    CHECK(ui_route_reason_code(&in, 0) == UI_REASON_UNSUPPORTED);
    in.dll_present = 0;
    CHECK(ui_route_reason_code(&in, 0) == UI_REASON_MISSING);
    in.dll_present = 1; in.renderer_unsupported = 0; in.inject_failures = 3;
    CHECK(ui_route_reason_code(&in, 0) == UI_REASON_INJECT_FAILED);
    CHECK(strstr(ui_route_reason_text(UI_REASON_GUARD), "停用") != NULL);
    printf("PASS route reasons: guard, missing, unsupported, failure, timeout\n");
}

/* After the 20 s timeout nexui may still load the panel on the page's next
   native call; telling the user to quit the client would be wrong. */
static void test_cdp_unavailable_hint_for_timeout(void) {
    const char *t = ui_cdp_unavailable_hint(UI_REASON_TIMEOUT);
    const char *m = ui_cdp_unavailable_hint(UI_REASON_MISSING);
    CHECK(strstr(t, "稍后") != NULL && strstr(t, "退出网易云") == NULL);
    CHECK(strstr(m, "退出网易云") != NULL);
    printf("PASS CDP-unavailable hint does not send the user away while nexui may still load\n");
}

/* A remote LoadLibrary that has not returned may still read the path buffer;
   a module whose base has zero low 32 bits returns exit code 0. */
static void test_inject_outcome(void) {
    int may_free = -1;
    CHECK(inject_outcome(WAIT_OBJECT_0, 0x5E000000, 0, &may_free) == 1 && may_free == 1);
    CHECK(inject_outcome(WAIT_OBJECT_0, 0, 0, &may_free) == 0 && may_free == 1);
    CHECK(inject_outcome(WAIT_OBJECT_0, 0, 1, &may_free) == 1 && may_free == 1);
    CHECK(inject_outcome(WAIT_TIMEOUT, STILL_ACTIVE, 0, &may_free) == 0 && may_free == 0);
    CHECK(inject_outcome(WAIT_TIMEOUT, STILL_ACTIVE, 1, &may_free) == 1 && may_free == 0);
    printf("PASS inject outcome: a timeout is not success and keeps the path buffer\n");
}

int main(void) {
    test_find_child_pids();
    test_ui_event_exists();
    test_ui_tracker_resets_on_new_main_pid();
    test_ui_child_already_loaded_is_not_reinjected();
    test_route_input_from_tracker();
    test_poll_interval_backs_off_outside_wait();
    test_round_skips_loaded_and_caps_failures();
    test_round_skips_child_that_is_a_main_process();
    test_crash_guard_trips_after_two_quick_exits();
    test_route_reason_codes();
    test_cdp_unavailable_hint_for_timeout();
    test_inject_outcome();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
