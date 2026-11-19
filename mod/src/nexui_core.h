/*
 * nexui.dll: the decisions that do not need a live CEF.
 *
 * Kept apart from nexui.c so they are tested directly (mod/test/nexui_test.c).
 */
#ifndef NEX_UI_CORE_H
#define NEX_UI_CORE_H

#include <windows.h>
#include <string.h>
#include <wchar.h>

#include "nexui_shared.h"

/* Struct sizes of cef_v8context_t and cef_frame_t in CEF 91.2.3 (branch 4472).
   CEF stamps base.size on every object, so a different build is detected
   before any member is called. */
#define NEXUI_CEF_CONTEXT_SIZE 112u
#define NEXUI_CEF_FRAME_SIZE   240u

static int nexui_token_edge(wchar_t c) {
    return c == 0 || c == L' ' || c == L'\t' || c == L'"';
}

/* Chromium passes the process role as its own argument. */
NEX_UI_FN int nexui_is_renderer(const wchar_t *cmdline) {
    static const wchar_t token[] = L"--type=renderer";
    const size_t len = (sizeof(token) / sizeof(token[0])) - 1;
    const wchar_t *p;
    if (!cmdline) return 0;
    for (p = wcsstr(cmdline, token); p; p = wcsstr(p + 1, token)) {
        if ((p == cmdline || nexui_token_edge(p[-1])) && nexui_token_edge(p[len]))
            return 1;
    }
    return 0;
}

NEX_UI_FN int nexui_layout_ok(size_t context_size, size_t frame_size) {
    return context_size == NEXUI_CEF_CONTEXT_SIZE && frame_size == NEXUI_CEF_FRAME_SIZE;
}

/* The panel belongs in the client's own main document, once per document. */
NEX_UI_FN int nexui_should_inject(int is_main_frame, const char *url_utf8, int marker_present) {
    return is_main_frame && !marker_present && url_utf8 &&
           strncmp(url_utf8, "orpheus://", 10) == 0;
}

/* Replace module's import of import_dll!name with hook. The first original
   pointer is kept in *original when it is still NULL. Returns 1 when a slot
   was rewritten, 0 when module does not import that name by name. */
NEX_UI_FN int nexui_iat_patch(HMODULE module, const char *import_dll, const char *name,
                              void *hook, void **original) {
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;
    int patched = 0;
    if (!module || !import_dll || !name || !hook) return 0;
    if (((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name; desc++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (_stricmp((const char *)(base + desc->Name), import_dll) != 0) continue;
        if (!desc->OriginalFirstThunk) continue;
        names = (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            DWORD old;
            void **slot = (void **)&slots->u1.Function;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name) != 0)
                continue;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (original && !*original) *original = *slot;
            InterlockedExchangePointer(slot, hook);
            VirtualProtect(slot, sizeof(void *), old, &old);
            patched = 1;
        }
    }
    return patched;
}

#endif
