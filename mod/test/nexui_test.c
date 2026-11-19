/* nexui: shared host/DLL contract and renderer-side logic. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "../src/nexui_shared.h"
#include "../src/nexui_core.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

/* ---- shared contract ------------------------------------------------------ */
static void test_event_names(void) {
    wchar_t b[64];
    CHECK(nex_ui_event_name(b, 64, L"Ready", 1234) && wcscmp(b, L"Local\\NexUI-Ready-1234") == 0);
    CHECK(nex_ui_event_name(b, 64, L"Unsupported", 7) && wcscmp(b, L"Local\\NexUI-Unsupported-7") == 0);
    CHECK(!nex_ui_event_name(b, 10, L"Ready", 1234));
    printf("PASS event names: Local\\NexUI-<kind>-<pid>, truncation refused\n");
}

static NexUiRouteInput base(void) {
    NexUiRouteInput r = {1, 1, 0, 0, 0, 1000, 1000};
    return r;
}

static void test_route_rules(void) {
    NexUiRouteInput r = base();
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.renderer_ready = 1;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
    r = base(); r.dll_present = 0;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.renderer_unsupported = 1;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.inject_failures = 2;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.inject_failures = 3;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.ms_since_renderer_injected = 19999;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_WAIT);
    r.ms_since_renderer_injected = 20000;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    r = base(); r.renderer_injected = 0; r.ms_since_client_seen = 20000;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_CDP);
    CHECK(nex_ui_route_decide(NULL) == NEX_UI_ROUTE_CDP);
    printf("PASS route: ready wins, missing/unsupported/failed/timeout fall back to CDP\n");
}

static void test_route_ready_after_timeout_wins(void) {
    NexUiRouteInput r = base();
    r.ms_since_renderer_injected = 90000;
    r.renderer_ready = 1;
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
    r.renderer_unsupported = 1;        /* another renderer may say no; a ready one still wins */
    CHECK(nex_ui_route_decide(&r) == NEX_UI_ROUTE_RENDERER);
    printf("PASS route: a renderer that becomes ready after the CDP fallback takes over again\n");
}

/* ---- renderer-side logic -------------------------------------------------- */
static void test_renderer_detection(void) {
    CHECK(nexui_is_renderer(L"cloudmusic.exe --type=renderer --no-sandbox"));
    CHECK(nexui_is_renderer(L"\"cloudmusic.exe\" --lang=zh --type=renderer"));
    CHECK(nexui_is_renderer(L"cloudmusic.exe \"--type=renderer\" --no-sandbox"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe --type=gpu-process"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe --type=renderer-helper"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe --xtype=renderer"));
    CHECK(!nexui_is_renderer(L"cloudmusic.exe"));
    CHECK(!nexui_is_renderer(NULL));
    printf("PASS renderer detection: exact --type=renderer token only\n");
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
    printf("PASS CEF 91 layout check and main orpheus:// frame target\n");
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
    CHECK(nexui_iat_patch(self, "libcef.dll", "cef_v8context_get_current_context",
                          (void *)fake_tick, NULL) == 0);
    printf("PASS import table patch: hook, restore, absent names untouched\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_event_names();
    test_route_rules();
    test_route_ready_after_timeout_wins();
    test_renderer_detection();
    test_layout_and_target();
    test_iat_patch_own_import();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
