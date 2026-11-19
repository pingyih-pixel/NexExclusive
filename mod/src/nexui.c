/*
 * nexui.dll - loads the NexExclusive settings panel inside the client's page.
 *
 * The host injects this module into every child of the cloudmusic.exe main
 * process. It acts only in the renderer (--type=renderer): it rewrites five
 * libcef imports of cloudmusic.dll, and the first time one of them is called
 * inside a V8 context of the main orpheus:// document that has not seen the
 * panel yet, it evaluates ui\panel.js and ui\simplify.js there. The page needs
 * no debugging port.
 *
 * State is announced to the host through two named events held for the life
 * of the process (see nexui_shared.h). The audio module (nexsink.dll) lives in
 * the main process and shares nothing with this one.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nexui_core.h"
#include "nexui_cef.h"

#define NEXUI_LOG_LIMIT 524288u
#define NEXUI_MODULE_WAIT_MS 15000u
#define NEXUI_RECHECK_MS 100u

/* ---- log ------------------------------------------------------------------ */
static CRITICAL_SECTION g_log_lock;
static WCHAR g_dir[MAX_PATH];
static WCHAR g_log_path[MAX_PATH];

static void nexui_log(const char *fmt, ...) {
    char line[1024];
    SYSTEMTIME t;
    va_list ap;
    int n;
    HANDLE f;
    GetLocalTime(&t);
    n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d pid=%lu  ", t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds, (unsigned long)GetCurrentProcessId());
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 3, fmt, ap);
    va_end(ap);
    strcat(line, "\r\n");
    EnterCriticalSection(&g_log_lock);
    f = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void rotate_log(void) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    WCHAR prev[MAX_PATH];
    if (!GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &fad)) return;
    if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow <= NEXUI_LOG_LIMIT) return;
    _snwprintf(prev, MAX_PATH, L"%lsnexui.prev.log", g_dir);
    prev[MAX_PATH - 1] = 0;
    MoveFileExW(g_log_path, prev, MOVEFILE_REPLACE_EXISTING);
}

static void narrow(const wchar_t *w, size_t len, char *out, int outn) {
    nexui_utf8_prefix(w, len, out, outn);
}

/* ---- events --------------------------------------------------------------- */
static HANDLE g_ready_event;
static HANDLE g_unsupported_event;
static volatile LONG g_fatal;

static HANDLE hold_event(const wchar_t *kind) {
    wchar_t name[64];
    if (!nex_ui_event_name(name, 64, kind, GetCurrentProcessId())) return NULL;
    return CreateEventW(NULL, TRUE, TRUE, name);
}

/* After this the hooks only pass through; the host falls back to CDP. */
static void give_up(const char *why) {
    InterlockedExchange(&g_fatal, 1);
    if (!g_unsupported_event) g_unsupported_event = hold_event(L"Unsupported");
    nexui_log("unsupported: %s; the host will use the CDP fallback", why);
}

/* ---- scripts -------------------------------------------------------------- */
typedef struct Script { wchar_t *text; size_t length; const char *name; } Script;
static Script g_panel = {NULL, 0, "ui\\panel.js"};
static Script g_simplify = {NULL, 0, "ui\\simplify.js"};

static int load_script(Script *s, const wchar_t *rel) {
    WCHAR path[MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    char *bytes;
    _snwprintf(path, MAX_PATH, L"%ls%ls", g_dir, rel);
    path[MAX_PATH - 1] = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    bytes = (size != INVALID_FILE_SIZE) ? (char *)malloc((size_t)size + 1) : NULL;
    if (!bytes || !ReadFile(h, bytes, size, &got, NULL) || got != size) {
        CloseHandle(h);
        free(bytes);
        return 0;
    }
    CloseHandle(h);
    s->text = nexui_decode_script(bytes, got, &s->length);
    free(bytes);
    return s->text != NULL;
}

/* ---- libcef entry points -------------------------------------------------- */
static void *(*real_get_current_context)(void);
static void *(*real_get_entered_context)(void);
static void *(*real_create_string)(const NexCefString *);
static void *(*real_create_function)(const NexCefString *, void *);
static void *(*real_create_object)(void *, void *);
static int (*real_in_context)(void);
static void (*real_userfree_free)(NexCefString *);

static void release(void *obj) {
    if (obj) ((NexCefBase *)obj)->release(obj);
}

/* Returns 1 when the code ran without throwing. quiet skips the log. */
static int run(NexCefV8Context *ctx, const wchar_t *code, size_t length, const char *name,
               int quiet) {
    static wchar_t url_text[] = L"nexui://panel";
    NexCefString text = {(wchar_t *)code, length, NULL};
    NexCefString url = {url_text, (sizeof(url_text) / sizeof(url_text[0])) - 1, NULL};
    void *retval = NULL, *exception = NULL;
    int ok = ctx->eval(ctx, &text, &url, 1, &retval, &exception);
    if (!ok && !quiet) {
        char message[400] = "?";
        NexCefV8Exception *e = (NexCefV8Exception *)exception;
        if (e && e->base.size >= sizeof(NexCefV8Exception)) {
            NexCefString *m = e->get_message(e);
            if (m) {
                narrow(m->str, m->length, message, sizeof(message));
                if (real_userfree_free) real_userfree_free(m);
            }
        }
        nexui_log("%s threw: %s", name, message);
    }
    release(retval);
    release(exception);
    return ok;
}

/* ---- the hook body -------------------------------------------------------- */
static volatile LONG g_busy;
static volatile LONG g_documents;
static DWORD g_last_check;

static void inject_document(NexCefV8Context *ctx, const char *url) {
    static const wchar_t marker[] = L"window.__nexUiMarker=1;";
    LONG document = InterlockedIncrement(&g_documents);
    int panel_ok;
    run(ctx, marker, (sizeof(marker) / sizeof(marker[0])) - 1, "marker", 0);
    panel_ok = run(ctx, g_panel.text, g_panel.length, g_panel.name, 0);
    if (g_simplify.text) run(ctx, g_simplify.text, g_simplify.length, g_simplify.name, 0);
    nexui_log("document #%ld %s: panel %s%s", document, url, panel_ok ? "loaded" : "failed",
              g_simplify.text ? "" : " (ui\\simplify.js absent)");
    if (panel_ok && !g_ready_event) {
        g_ready_event = hold_event(L"Ready");
        nexui_log("ready event %s", g_ready_event ? "held" : "could not be created");
    }
}

static void on_hook(void) {
    static const wchar_t probe[] = L"if(!window.__nexUiMarker)throw 0;";
    NexCefV8Context *ctx;
    NexCefFrame *frame;
    char url[300] = "";
    int main_frame;
    DWORD now;

    if (g_fatal || !real_in_context || !real_in_context()) return;
    now = GetTickCount();
    if (g_last_check && now - g_last_check < NEXUI_RECHECK_MS) return;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;

    ctx = (NexCefV8Context *)real_get_current_context();
    if (!ctx) goto done;
    frame = ctx->base.size == NEXUI_CEF_CONTEXT_SIZE && ctx->is_valid(ctx)
                ? (NexCefFrame *)ctx->get_frame(ctx) : NULL;
    if (ctx->base.size != NEXUI_CEF_CONTEXT_SIZE ||
        (frame && !nexui_layout_ok(ctx->base.size, frame->base.size))) {
        char why[96];
        snprintf(why, sizeof(why), "CEF layout differs (context %zu, frame %zu)",
                 ctx->base.size, frame ? frame->base.size : (size_t)0);
        give_up(why);
        release(frame);
        release(ctx);
        goto done;
    }
    if (!frame) { release(ctx); goto done; }

    main_frame = frame->is_main(frame);
    if (main_frame) {
        NexCefString *u = frame->get_url(frame);
        if (u) {
            narrow(u->str, u->length, url, sizeof(url));
            if (real_userfree_free) real_userfree_free(u);
        }
    }
    if (nexui_should_inject(main_frame, url, 0)) {
        int marker_present = run(ctx, probe, (sizeof(probe) / sizeof(probe[0])) - 1, "probe", 1);
        if (nexui_should_inject(main_frame, url, marker_present)) inject_document(ctx, url);
    }
    release(frame);
    release(ctx);
    g_last_check = GetTickCount();
done:
    InterlockedExchange(&g_busy, 0);
}

static void *hook_get_current_context(void) { on_hook(); return real_get_current_context(); }
static void *hook_get_entered_context(void) { on_hook(); return real_get_entered_context(); }
static void *hook_create_string(const NexCefString *s) { on_hook(); return real_create_string(s); }
static void *hook_create_function(const NexCefString *n, void *h) {
    on_hook();
    return real_create_function(n, h);
}
static void *hook_create_object(void *a, void *i) { on_hook(); return real_create_object(a, i); }

typedef struct Hook { const char *name; void *hook; void **real; } Hook;
static Hook g_hooks[] = {
    {"cef_v8context_get_current_context", (void *)hook_get_current_context, (void **)&real_get_current_context},
    {"cef_v8context_get_entered_context", (void *)hook_get_entered_context, (void **)&real_get_entered_context},
    {"cef_v8value_create_string", (void *)hook_create_string, (void **)&real_create_string},
    {"cef_v8value_create_function", (void *)hook_create_function, (void **)&real_create_function},
    {"cef_v8value_create_object", (void *)hook_create_object, (void **)&real_create_object},
};
#define NEXUI_HOOKS ((int)(sizeof(g_hooks) / sizeof(g_hooks[0])))

/* ---- start-up --------------------------------------------------------------- */
static void process_type(char *out, int n) {
    const wchar_t *t = wcsstr(GetCommandLineW(), L"--type=");
    wchar_t w[48];
    int k = 0;
    if (!t) { snprintf(out, (size_t)n, "browser"); return; }
    t += 7;
    while (t[k] && t[k] != L' ' && t[k] != L'"' && k < 47) { w[k] = t[k]; k++; }
    narrow(w, (size_t)k, out, n);
}

static DWORD WINAPI nexui_thread(LPVOID unused) {
    HMODULE cef = NULL, app = NULL;
    WCHAR disable[MAX_PATH];
    DWORD waited = 0;
    int i, patched = 0;
    (void)unused;

    if (!nexui_is_renderer(GetCommandLineW())) {
        char kind[48];
        process_type(kind, sizeof(kind));
        nexui_log("inert in %s process", kind);
        return 0;
    }
    rotate_log();
    nexui_log("loaded in renderer");

    _snwprintf(disable, MAX_PATH, L"%lsnexui.disable", g_dir);
    disable[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(disable) != INVALID_FILE_ATTRIBUTES) {
        give_up("nexui.disable is present");
        return 0;
    }
    if (!load_script(&g_panel, L"ui\\panel.js")) {
        give_up("ui\\panel.js is not readable");
        return 0;
    }
    if (!load_script(&g_simplify, L"ui\\simplify.js"))
        nexui_log("ui\\simplify.js is not readable; loading the panel alone");

    while (waited < NEXUI_MODULE_WAIT_MS) {
        cef = GetModuleHandleW(L"libcef.dll");
        app = GetModuleHandleW(L"cloudmusic.dll");
        if (cef && app) break;
        Sleep(5);
        waited += 5;
    }
    if (!cef || !app) {
        give_up(!cef ? "libcef.dll is not loaded" : "cloudmusic.dll is not loaded");
        return 0;
    }
    {
        int (*version_info)(int) = (int (*)(int))GetProcAddress(cef, "cef_version_info");
        char why[64];
        if (!version_info) {
            give_up("cef_version_info is not exported");
            return 0;
        }
        nexui_log("CEF %d.%d.%d / Chromium %d.%d.%d.%d", version_info(0), version_info(1),
                  version_info(2), version_info(4), version_info(5), version_info(6),
                  version_info(7));
        if (!nexui_cef_version_ok(version_info(0))) {
            snprintf(why, sizeof(why), "CEF %d is not the verified CEF %d", version_info(0),
                     NEXUI_CEF_MAJOR);
            give_up(why);
            return 0;
        }
    }
    real_in_context = (int (*)(void))GetProcAddress(cef, "cef_v8context_in_context");
    real_userfree_free = (void (*)(NexCefString *))GetProcAddress(cef, "cef_string_userfree_utf16_free");
    if (!real_in_context) {
        give_up("cef_v8context_in_context is not exported");
        return 0;
    }
    /* Resolve every entry point before the first slot is rewritten: each hook
       body calls cef_v8context_get_current_context. */
    for (i = 0; i < NEXUI_HOOKS; i++)
        *g_hooks[i].real = (void *)GetProcAddress(cef, g_hooks[i].name);
    if (!nexui_hooks_usable(real_get_current_context != NULL, 1)) {
        give_up("cef_v8context_get_current_context is not exported");
        return 0;
    }
    for (i = 0; i < NEXUI_HOOKS; i++) {
        if (*g_hooks[i].real && nexui_iat_patch(app, "libcef.dll", g_hooks[i].name, g_hooks[i].hook, NULL))
            patched++;
        else
            nexui_log("not hooked: %s", g_hooks[i].name);
    }
    if (!nexui_hooks_usable(real_get_current_context != NULL, patched)) {
        give_up("no libcef import of cloudmusic.dll could be hooked");
        return 0;
    }
    nexui_log("hooked %d of %d libcef imports; waiting for the page", patched, NEXUI_HOOKS);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DWORD n = GetModuleFileNameW(instance, g_dir, MAX_PATH);
        HANDLE thread;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_log_lock);
        while (n && g_dir[n - 1] != L'\\') n--;
        g_dir[n] = 0;
        _snwprintf(g_log_path, MAX_PATH, L"%lsnexui.log", g_dir);
        g_log_path[MAX_PATH - 1] = 0;
        thread = CreateThread(NULL, 0, nexui_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
