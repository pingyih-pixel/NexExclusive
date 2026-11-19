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

int main(void) {
    test_find_child_pids();
    test_ui_event_exists();
    test_ui_tracker_resets_on_new_main_pid();
    test_ui_child_already_loaded_is_not_reinjected();
    test_route_input_from_tracker();
    test_poll_interval_backs_off_outside_wait();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
