/*
 * The part of the CEF 91 C API (branch 4472, include/capi) that nexui.dll
 * touches. Member order is the ABI: it must match cef_v8_capi.h,
 * cef_frame_capi.h and cef_base_capi.h of that branch exactly. Members that
 * nexui never calls are kept as plain pointers so the offsets stay right.
 *
 * Every object carries base.size; nexui_layout_ok() compares it with the sizes
 * this file implies before any member is called.
 */
#ifndef NEX_UI_CEF_H
#define NEX_UI_CEF_H

#include <stddef.h>
#include <wchar.h>

typedef struct NexCefBase {
    size_t size;
    void (*add_ref)(void *self);
    int (*release)(void *self);
    int (*has_one_ref)(void *self);
    int (*has_at_least_one_ref)(void *self);
} NexCefBase;

/* cef_string_t is UTF-16 on Windows. */
typedef struct NexCefString {
    wchar_t *str;
    size_t length;
    void (*dtor)(wchar_t *str);
} NexCefString;

typedef struct NexCefV8Context {
    NexCefBase base;
    void *get_task_runner;
    int (*is_valid)(void *self);
    void *get_browser;
    void *(*get_frame)(void *self);
    void *get_global;
    void *enter;
    void *exit;
    void *is_same;
    int (*eval)(void *self, const NexCefString *code, const NexCefString *script_url,
                int start_line, void **retval, void **exception);
} NexCefV8Context;

typedef struct NexCefFrame {
    NexCefBase base;
    int (*is_valid)(void *self);
    void *undo, *redo, *cut, *copy, *paste, *del, *select_all, *view_source;
    void *get_source, *get_text, *load_request, *load_url;
    void *execute_java_script;
    int (*is_main)(void *self);
    void *is_focused;
    void *get_name;
    void *get_identifier;
    void *get_parent;
    NexCefString *(*get_url)(void *self);
    void *get_browser, *get_v8context, *visit_dom, *create_urlrequest, *send_process_message;
} NexCefFrame;

typedef struct NexCefV8Exception {
    NexCefBase base;
    NexCefString *(*get_message)(void *self);
} NexCefV8Exception;

#endif
