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

/* ---- the DLL itself ------------------------------------------------------- */
/* A test exe is not a renderer: nexui.dll must load, do nothing and create
   neither event. The host injects it into every client child, GPU included. */
static void test_dll_inert_outside_renderer(const char *dll_path) {
    wchar_t name[64];
    HANDLE r, u;
    HMODULE m = LoadLibraryA(dll_path);
    CHECK(m != NULL);
    Sleep(300);
    nex_ui_event_name(name, 64, L"Ready", GetCurrentProcessId());
    r = OpenEventW(SYNCHRONIZE, FALSE, name);
    nex_ui_event_name(name, 64, L"Unsupported", GetCurrentProcessId());
    u = OpenEventW(SYNCHRONIZE, FALSE, name);
    CHECK(r == NULL && u == NULL);
    if (r) CloseHandle(r);
    if (u) CloseHandle(u);
    printf("PASS nexui.dll stays inert outside a renderer process\n");
}

/* ---- final-review fixes --------------------------------------------------- */
/* A long route used to fail the whole conversion and yield "", so the document
   was skipped and the host, seeing Ready, never fell back. */
static void test_long_url_keeps_its_prefix(void) {
    static wchar_t url[2100];
    char out[300];
    int i;
    wcscpy(url, L"orpheus://orpheus/pub/app.html#/m/search/?s=");
    for (i = (int)wcslen(url); i < 2000; i++) url[i] = (i & 1) ? 0x4E2D : L'%';
    url[2000] = 0;
    nexui_utf8_prefix(url, wcslen(url), out, (int)sizeof(out));
    CHECK(strncmp(out, "orpheus://orpheus/pub/app.html", 30) == 0);
    CHECK(strlen(out) < sizeof(out));
    CHECK(nexui_should_inject(1, out, 0));
    nexui_utf8_prefix(L"orpheus://x", 11, out, (int)sizeof(out));
    CHECK(strcmp(out, "orpheus://x") == 0);
    /* a surrogate pair is never split at the cut */
    {
        wchar_t pair[200];
        char small[8];
        for (i = 0; i < 198; i += 2) { pair[i] = 0xD83C; pair[i + 1] = 0xDFB5; }
        pair[198] = 0;
        nexui_utf8_prefix(pair, 198, small, (int)sizeof(small));
        CHECK(strlen(small) % 4 == 0);
    }
    nexui_utf8_prefix(NULL, 0, out, (int)sizeof(out));
    CHECK(out[0] == 0);
    printf("PASS long URLs and messages are truncated, not dropped\n");
}

/* An empty panel.js evaluated without error and held Ready with no panel. */
static void test_empty_script_is_not_usable(void) {
    size_t n = 99;
    wchar_t *w;
    CHECK(nexui_decode_script("", 0, &n) == NULL);
    CHECK(nexui_decode_script("\xEF\xBB\xBF", 3, &n) == NULL);
    CHECK(nexui_decode_script(" \r\n\t", 4, &n) == NULL);
    w = nexui_decode_script("\xEF\xBB\xBF" "var a=1;", 11, &n);
    CHECK(w && n == 8 && wcscmp(w, L"var a=1;") == 0);
    free(w);
    printf("PASS empty or blank scripts are unreadable, BOM stripped\n");
}

/* Only CEF 91 is trusted: equal struct sizes do not prove equal member order. */
static void test_cef_version_gate(void) {
    CHECK(nexui_cef_version_ok(91));
    CHECK(!nexui_cef_version_ok(90));
    CHECK(!nexui_cef_version_ok(92));
    CHECK(!nexui_cef_version_ok(0));
    printf("PASS CEF version gate accepts 91 only\n");
}

/* on_hook always calls cef_v8context_get_current_context; without it, no hook. */
static void test_hooks_need_current_context(void) {
    CHECK(nexui_hooks_usable(1, 5));
    CHECK(nexui_hooks_usable(1, 1));
    CHECK(!nexui_hooks_usable(0, 4));
    CHECK(!nexui_hooks_usable(1, 0));
    printf("PASS hooks are installed only when get_current_context resolved\n");
}

int main(int argc, char **argv) {
    test_event_names();
    test_route_rules();
    test_route_ready_after_timeout_wins();
    test_renderer_detection();
    test_layout_and_target();
    test_iat_patch_own_import();
    test_long_url_keeps_its_prefix();
    test_empty_script_is_not_usable();
    test_cef_version_gate();
    test_hooks_need_current_context();
    if (argc > 1) {
        test_dll_inert_outside_renderer(argv[1]);
    } else {
        fprintf(stderr, "FAIL dll inert: no nexui.dll path given\n");
        failures++;
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
