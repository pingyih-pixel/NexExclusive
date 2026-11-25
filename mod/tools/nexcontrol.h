/* nexcontrol.h - the control block shared by nexsink.dll and the host
   (ROADMAP 5c).

   Until now the host could only talk to the audio module by rewriting hook.cfg
   and reading sink.log. A decision the listener makes in a prompt ("close
   exclusive so Chrome can play") has to reach the module now, and the host has
   to know whether the module currently holds the device exclusively.

   The module creates one named mapping per client process,
   Local\NexExclusive.control.<pid>, and owns everything in it except the
   command pair. The host writes cmd, then bumps cmd_seq; the module applies
   the command on its watch thread (within ~400 ms) and copies cmd_seq to
   ack_seq. Commands are idempotent, so a lost race between two quick commands
   only means the later one wins. */
#ifndef NEX_CONTROL_H
#define NEX_CONTROL_H

#ifndef NEX_HDR_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define NEX_HDR_UNUSED __attribute__((unused))
#else
#define NEX_HDR_UNUSED
#endif
#endif

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define NEX_CONTROL_MAGIC   0x4C52544358454E00ull   /* "\0NEXCTRL" */
#define NEX_CONTROL_VERSION 1

enum {
    NEX_CONTROL_CMD_NONE      = 0,
    NEX_CONTROL_CMD_SHARE     = 1,  /* leave exclusive now, stay shared until CMD_EXCLUSIVE */
    NEX_CONTROL_CMD_EXCLUSIVE = 2   /* lift CMD_SHARE; exclusive again at the next resume */
};

enum {
    NEX_CONTROL_STATE_IDLE      = 0,  /* no stream intercepted */
    NEX_CONTROL_STATE_EXCLUSIVE = 1,  /* holds the device exclusively (playing or paused) */
    NEX_CONTROL_STATE_SHARED    = 2,  /* plays through the system mixer */
    NEX_CONTROL_STATE_RELEASED  = 3   /* paused, device given back or being rebuilt */
};

typedef struct NexControl {
    unsigned long long magic;
    UINT32        version;
    UINT32        size;
    UINT32        pid;            /* the client process that owns the mapping */
    volatile LONG state;          /* NEX_CONTROL_STATE_* */
    volatile LONG playing;        /* the client has started the stream */
    volatile LONG share_forced;   /* CMD_SHARE is in effect */
    volatile LONG cmd;            /* written by the host */
    volatile LONG cmd_seq;        /* bumped by the host after cmd */
    volatile LONG ack_seq;        /* = cmd_seq once applied */
    volatile LONG ack_result;     /* HRESULT of the last command */
    volatile LONG heartbeat;      /* GetTickCount of the module's last update */
    UINT32        reserved[16];
} NexControl;

static NEX_HDR_UNUSED void nex_control_name(WCHAR *out, size_t cap, DWORD pid) {
    _snwprintf(out, cap, L"Local\\NexExclusive.control.%lu", (unsigned long)pid);
    out[cap - 1] = 0;
}

static NEX_HDR_UNUSED int nex_control_valid(const NexControl *c) {
    return c && c->magic == NEX_CONTROL_MAGIC && c->version == NEX_CONTROL_VERSION &&
           c->size == sizeof(NexControl);
}

/* Host side: post a command. Returns the sequence number to wait for. */
static NEX_HDR_UNUSED LONG nex_control_post(NexControl *c, LONG cmd) {
    InterlockedExchange(&c->cmd, cmd);
    return InterlockedIncrement(&c->cmd_seq);
}

/* Module side: the command waiting to be applied, or NONE. */
static NEX_HDR_UNUSED LONG nex_control_pending(NexControl *c, LONG *seq) {
    LONG s = InterlockedCompareExchange(&c->cmd_seq, 0, 0);
    if (s == InterlockedCompareExchange(&c->ack_seq, 0, 0)) return NEX_CONTROL_CMD_NONE;
    *seq = s;
    return InterlockedCompareExchange(&c->cmd, 0, 0);
}

static NEX_HDR_UNUSED void nex_control_ack(NexControl *c, LONG seq, HRESULT hr) {
    InterlockedExchange(&c->ack_result, (LONG)hr);
    InterlockedExchange(&c->ack_seq, seq);
}

#endif
