"""Enumerate audio sessions on a render endpoint.

Answers two questions that matter for an exclusive-output mod:
  * which processes currently hold the endpoint (and in what state)
  * whether the NetEase client shows up as a shared-mode session at all

A process playing through the shared mixer always owns a session here.
A process holding the endpoint exclusively owns none.

Usage:
    python sessions.py            # default render endpoint
    python sessions.py --device 0
"""

import argparse
import ctypes
import sys
from ctypes import POINTER, byref, c_ulong, c_void_p, c_wchar_p

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from wasapi_probe import (  # noqa: E402
    CLSCTX_ALL, IID_IMMDeviceEnumerator, CLSID_MMDeviceEnumerator, GUID,
    describe, guid, hr, release, vt,
)

IID_IAudioSessionManager2 = guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")
IID_IAudioSessionControl2 = guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")

STATE_NAMES = {0: "inactive", 1: "ACTIVE", 2: "expired"}

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def process_name(pid):
    if pid == 0:
        return "(system sounds)"
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return "(pid %d)" % pid
    try:
        size = c_ulong(1024)
        buf = ctypes.create_unicode_buffer(1024)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buf, byref(size)):
            return buf.value.rsplit("\\", 1)[-1]
        return "(pid %d)" % pid
    finally:
        kernel32.CloseHandle(handle)


def sessions_on(device):
    manager = c_void_p()
    code = hr(vt(device, 3, ctypes.c_long, POINTER(GUID), c_ulong, c_void_p,
        POINTER(c_void_p))(
        device, byref(IID_IAudioSessionManager2), CLSCTX_ALL, None, byref(manager)))
    if code != 0 or not manager:
        return None, "Activate(IAudioSessionManager2) failed 0x%08X" % code

    enum = c_void_p()
    code = hr(vt(manager, 5, ctypes.c_long, POINTER(c_void_p))(manager, byref(enum)))
    if code != 0 or not enum:
        release(manager)
        return None, "GetSessionEnumerator failed 0x%08X" % code

    count = ctypes.c_int()
    vt(enum, 3, ctypes.c_long, POINTER(ctypes.c_int))(enum, byref(count))

    out = []
    for i in range(count.value):
        control = c_void_p()
        vt(enum, 4, ctypes.c_long, ctypes.c_int, POINTER(c_void_p))(
            enum, i, byref(control))
        c2 = c_void_p()
        qi = hr(vt(control, 0, ctypes.c_long, POINTER(GUID),
                   POINTER(c_void_p))(control, byref(IID_IAudioSessionControl2),
                                      byref(c2)))
        state = ctypes.c_int()
        vt(control, 3, ctypes.c_long, POINTER(ctypes.c_int))(control, byref(state))
        pid = c_ulong()
        if qi == 0 and c2:
            vt(c2, 14, ctypes.c_long, POINTER(c_ulong))(c2, byref(pid))
        else:
            pid = c_ulong(0xFFFFFFFF)

        display = c_wchar_p()
        vt(control, 4, ctypes.c_long, POINTER(c_wchar_p))(control, byref(display))

        out.append({
            "pid": pid.value,
            "process": process_name(pid.value) if pid.value != 0xFFFFFFFF else "(no session2, qi=0x%08X)" % qi,
            "state": STATE_NAMES.get(state.value, str(state.value)),
            "display_name": display.value or "",
        })
        if c2:
            release(c2)
        release(control)

    release(enum)
    release(manager)
    return out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", type=int, default=None,
                    help="endpoint index (omit for the default endpoint)")
    args = ap.parse_args()

    ctypes.windll.ole32.CoInitialize(None)

    enumerator = c_void_p()
    ctypes.windll.ole32.CoCreateInstance(
        byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL,
        byref(IID_IMMDeviceEnumerator), byref(enumerator))

    device = c_void_p()
    if args.device is None:
        vt(enumerator, 4, ctypes.c_long, ctypes.c_int, ctypes.c_int,
           POINTER(c_void_p))(enumerator, 0, 0, byref(device))
        label = "default render endpoint"
    else:
        collection = c_void_p()
        vt(enumerator, 3, ctypes.c_long, ctypes.c_int, c_ulong,
           POINTER(c_void_p))(enumerator, 0, 1, byref(collection))
        vt(collection, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(
            collection, args.device, byref(device))
        release(collection)
        label = "endpoint #%d" % args.device

    client = c_void_p()
    from wasapi_probe import IID_IAudioClient
    vt(device, 3, ctypes.c_long, POINTER(GUID), c_ulong,
       c_void_p, POINTER(c_void_p))(device, byref(IID_IAudioClient), CLSCTX_ALL,
                                    None, byref(client))
    mix_ptr = POINTER(c_void_p)()
    if client and hr(vt(client, 8, ctypes.c_long, POINTER(POINTER(c_void_p)))(
            client, byref(mix_ptr))) == 0:
        print("%s  mix format: %s" % (label, describe(ctypes.cast(mix_ptr, c_void_p))))
    if client:
        release(client)

    rows, err = sessions_on(device)
    if err:
        print(err)
        return
    print("sessions: %d\n" % len(rows))
    print("  %-7s %-24s %-10s %s" % ("pid", "process", "state", "display name"))
    for r in rows:
        print("  %-7d %-24s %-10s %s" % (r["pid"], r["process"], r["state"],
                                         r["display_name"][:40]))
    release(device)
    release(enumerator)


if __name__ == "__main__":
    main()
