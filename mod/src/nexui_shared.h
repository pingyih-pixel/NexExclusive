/*
 * Contract between the host (NexExclusive.exe) and nexui.dll.
 *
 * nexui.dll runs inside a cloudmusic.exe renderer and announces its state with
 * two named events held for the life of that process:
 *   Local\NexUI-Ready-<pid>        the panel script ran in a page
 *   Local\NexUI-Unsupported-<pid>  this renderer cannot host the panel
 * The host reads them to pick the panel route: in-renderer, or the CDP
 * fallback on port 9333.
 *
 * Pure C, no Windows headers: the route rules are tested on their own.
 */
#ifndef NEX_UI_SHARED_H
#define NEX_UI_SHARED_H

#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#if defined(__GNUC__) || defined(__clang__)
#define NEX_UI_FN static __attribute__((unused))
#else
#define NEX_UI_FN static
#endif

#define NEX_UI_ROUTE_TIMEOUT_MS 20000
#define NEX_UI_INJECT_MAX_FAILURES 3

/* kind is L"Ready" or L"Unsupported". Returns 0 when out is too small. */
NEX_UI_FN int nex_ui_event_name(wchar_t *out, size_t n, const wchar_t *kind, unsigned long pid) {
    int written;
    if (!out || !n || !kind) return 0;
    written = swprintf(out, n, L"Local\\NexUI-%ls-%lu", kind, pid);
    if (written < 0 || (size_t)written >= n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

enum {
    NEX_UI_ROUTE_WAIT = 0,      /* give the renderer path more time        */
    NEX_UI_ROUTE_RENDERER = 1,  /* panel is in the page; CDP not needed     */
    NEX_UI_ROUTE_CDP = 2        /* fall back to the debugging port          */
};

typedef struct NexUiRouteInput {
    int dll_present;                       /* nexui.dll exists beside the host        */
    int renderer_injected;                 /* nexui.dll is loaded in some child       */
    int renderer_ready;                    /* some child holds the Ready event        */
    int renderer_unsupported;              /* some child holds the Unsupported event  */
    int inject_failures;                   /* most failed injections into one child   */
    unsigned long ms_since_client_seen;    /* since this main process was first seen  */
    unsigned long ms_since_renderer_injected; /* since the first successful injection */
} NexUiRouteInput;

/* A ready renderer always wins, even after a fallback has started: the panel
   script is idempotent, so the CDP copy that may already be in the page is
   harmless. Everything else that is not clearly progressing falls back. */
NEX_UI_FN int nex_ui_route_decide(const NexUiRouteInput *in) {
    if (!in) return NEX_UI_ROUTE_CDP;
    if (in->renderer_ready) return NEX_UI_ROUTE_RENDERER;
    if (!in->dll_present || in->renderer_unsupported ||
        in->inject_failures >= NEX_UI_INJECT_MAX_FAILURES)
        return NEX_UI_ROUTE_CDP;
    if (in->renderer_injected) {
        return in->ms_since_renderer_injected >= NEX_UI_ROUTE_TIMEOUT_MS
                   ? NEX_UI_ROUTE_CDP : NEX_UI_ROUTE_WAIT;
    }
    return in->ms_since_client_seen >= NEX_UI_ROUTE_TIMEOUT_MS
               ? NEX_UI_ROUTE_CDP : NEX_UI_ROUTE_WAIT;
}

#endif
