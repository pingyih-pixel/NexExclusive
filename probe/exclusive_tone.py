"""Open a real WASAPI exclusive stream and push a short test tone through it.

This is the smallest end-to-end proof of the output layer an exclusive mod
would install: pick an endpoint, negotiate an exclusive format, own the
render client, feed integer PCM, and read back the hardware buffer size.

Usage:
    python exclusive_tone.py --list
    python exclusive_tone.py --device 0 --rate 44100 --bits 16
    python exclusive_tone.py --device 0 --rate 48000 --bits 24 --seconds 1.0
"""

import argparse
import ctypes
import math
import struct
import sys
import time
from ctypes import POINTER, byref, c_ulong, c_ushort, c_void_p, c_wchar_p

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from wasapi_probe import (  # noqa: E402
    CLSCTX_ALL, IID_IAudioClient, IID_IMMDeviceEnumerator,
    CLSID_MMDeviceEnumerator, AUDCLNT_SHAREMODE_EXCLUSIVE,
    PKEY_Device_FriendlyName, GUID, PROPERTYKEY, WAVEFORMATEX,
    WAVEFORMATEXTENSIBLE, KSDATAFORMAT_SUBTYPE_PCM, describe, guid,
    hr, release, vt, read_lpwstr_propvariant,
)

IID_IAudioRenderClient = guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")
AUDCLNT_STREAMFLAGS_NONE = 0x0
BUFFER_HNS = 2000000  # 200 ms, in 100-ns units


class WAVEFORMAT24(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("Format", WAVEFORMATEX)]


def list_devices():
    enumerator = c_void_p()
    ctypes.windll.ole32.CoCreateInstance(
        byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL,
        byref(IID_IMMDeviceEnumerator), byref(enumerator))
    collection = c_void_p()
    vt(enumerator, 3, ctypes.c_long, ctypes.c_int, c_ulong, POINTER(c_void_p))(
        enumerator, 0, 1, byref(collection))
    count = c_ulong()
    vt(collection, 3, ctypes.c_long, POINTER(c_ulong))(collection, byref(count))
    names = []
    for i in range(count.value):
        dev = c_void_p()
        vt(collection, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(
            collection, i, byref(dev))
        store = c_void_p()
        vt(dev, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(dev, 0, byref(store))
        pv = ctypes.create_string_buffer(32)
        name = "(unnamed)"
        if store:
            if hr(vt(store, 5, ctypes.c_long, POINTER(PROPERTYKEY), c_void_p)(
                    store, byref(PKEY_Device_FriendlyName), pv)) == 0:
                name = read_lpwstr_propvariant(pv) or name
            release(store)
        names.append((i, name))
        release(dev)
    release(collection)
    release(enumerator)
    return names


def get_device(index):
    enumerator = c_void_p()
    ctypes.windll.ole32.CoCreateInstance(
        byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL,
        byref(IID_IMMDeviceEnumerator), byref(enumerator))
    collection = c_void_p()
    vt(enumerator, 3, ctypes.c_long, ctypes.c_int, c_ulong, POINTER(c_void_p))(
        enumerator, 0, 1, byref(collection))
    dev = c_void_p()
    vt(collection, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(
        collection, index, byref(dev))
    release(collection)
    release(enumerator)
    return dev


def build_format(rate, bits, channels=2, mask=0x3):
    wf = WAVEFORMATEXTENSIBLE()
    wf.Format.wFormatTag = 0xFFFE
    wf.Format.nChannels = channels
    wf.Format.nSamplesPerSec = rate
    wf.Format.wBitsPerSample = bits
    wf.Format.nBlockAlign = channels * bits // 8
    wf.Format.nAvgBytesPerSec = rate * wf.Format.nBlockAlign
    wf.Format.cbSize = ctypes.sizeof(WAVEFORMATEXTENSIBLE) - ctypes.sizeof(WAVEFORMATEX)
    wf.wValidBitsPerSample = bits
    wf.dwChannelMask = mask
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_PCM
    return wf


def render_tone(device, rate, bits, seconds, freq=440.0, gain=0.125):
    client = c_void_p()
    code = hr(vt(device, 3, ctypes.c_long, POINTER(GUID), c_ulong, c_void_p,
                 POINTER(c_void_p))(
        device, byref(IID_IAudioClient), CLSCTX_ALL, None, byref(client)))
    if code != 0:
        print("Activate(IAudioClient) failed 0x%08X" % code)
        return False

    wf = build_format(rate, bits)
    code = hr(vt(client, 3, ctypes.c_long, ctypes.c_int, c_ulong,
                 ctypes.c_longlong, ctypes.c_longlong,
                 POINTER(WAVEFORMATEX), c_void_p)(
        client, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_NONE,
        BUFFER_HNS, BUFFER_HNS,
        ctypes.cast(byref(wf), POINTER(WAVEFORMATEX)), None))
    if code != 0:
        print("Initialize(EXCLUSIVE, %d Hz / %d bit) failed 0x%08X" % (rate, bits, code))
        release(client)
        return False

    frames = c_ulong()
    vt(client, 4, ctypes.c_long, POINTER(c_ulong))(client, byref(frames))
    latency = ctypes.c_longlong()
    vt(client, 5, ctypes.c_long, POINTER(ctypes.c_longlong))(client, byref(latency))
    period = ctypes.c_longlong()
    minp = ctypes.c_longlong()
    vt(client, 9, ctypes.c_long, POINTER(ctypes.c_longlong), POINTER(ctypes.c_longlong))(
        client, byref(period), byref(minp))

    render = c_void_p()
    vt(client, 14, ctypes.c_long, POINTER(GUID), POINTER(c_void_p))(
        client, byref(IID_IAudioRenderClient), byref(render))

    print("  negotiated: %d Hz / %d bit / %d ch" % (rate, bits, wf.Format.nChannels))
    print("  buffer frames: %d  (%.1f ms)" % (frames.value,
                                              frames.value * 1000.0 / rate))
    print("  stream latency: %.2f ms   default period: %.2f ms   min period: %.2f ms"
          % (latency.value / 10000.0, period.value / 10000.0, minp.value / 10000.0))

    channels = wf.Format.nChannels
    bytes_per_sample = bits // 8
    frame_bytes = channels * bytes_per_sample
    total = int(rate * seconds)

    vt(client, 10, ctypes.c_long)(client)  # Start

    written = 0
    phase = 0.0
    step = 2.0 * math.pi * freq / rate
    started = time.perf_counter()
    try:
        while written < total:
            padding = c_ulong()
            vt(client, 6, ctypes.c_long, POINTER(c_ulong))(client, byref(padding))
            avail = frames.value - padding.value
            if avail <= 0:
                time.sleep(0.005)
                continue
            n = min(avail, total - written)
            data = bytearray()
            for _ in range(n):
                sample = int(gain * 32767 * math.sin(phase))
                phase += step
                for _ch in range(channels):
                    if bits == 16:
                        data += struct.pack("<h", sample)
                    elif bits == 24:
                        v = sample << 8
                        data += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))
                    else:
                        data += struct.pack("<i", sample << 16)
            buf = (ctypes.c_ubyte * len(data)).from_buffer(data)
            p = c_void_p()
            vt(render, 3, ctypes.c_long, ctypes.c_uint, POINTER(c_void_p))(
                render, n, byref(p))
            ctypes.memmove(p, buf, len(data))
            vt(render, 4, ctypes.c_long, ctypes.c_uint, c_ulong)(render, n, 0)
            written += n
            if time.perf_counter() - started > seconds + 2.0:
                print("  watchdog: bailing out")
                break
    finally:
        # let the device drain before stopping
        time.sleep(frames.value / float(rate) + 0.05)
        vt(client, 11, ctypes.c_long)(client)
        release(render)
        release(client)

    print("  played %d frames (%.2f s) and drained" % (written, written / float(rate)))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--bits", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=1.2)
    args = ap.parse_args()

    ctypes.windll.ole32.CoInitialize(None)

    if args.list:
        for i, name in list_devices():
            print("[%d] %s" % (i, name))
        return

    devices = list_devices()
    label = next((n for i, n in devices if i == args.device), "?")
    print("device [%d] %s" % (args.device, label))
    dev = get_device(args.device)
    ok = render_tone(dev, args.rate, args.bits, args.seconds)
    release(dev)
    print("result: %s" % ("exclusive stream OK" if ok else "failed"))


if __name__ == "__main__":
    main()
