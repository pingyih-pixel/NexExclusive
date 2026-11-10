"""WASAPI capability probe (stdlib ctypes, no third-party deps).

For every active render endpoint it reports:
  * friendly name / endpoint id
  * the shared-mode mix format the audio engine runs at
  * which (sample rate x bit depth) combinations the device accepts in
    EXCLUSIVE mode, queried through IAudioClient::IsFormatSupported

That matrix is exactly what an exclusive-output mod has to negotiate
against, so it doubles as the format table for the negotiation logic.

Usage:
    python wasapi_probe.py            # human readable
    python wasapi_probe.py --json     # machine readable
"""

import ctypes
import json
import sys
from ctypes import POINTER, byref, c_uint32, c_ulong, c_ushort, c_void_p, c_wchar_p

ole32 = ctypes.WinDLL("ole32")
ole32.CoInitialize(None)
ole32.CoCreateInstance.restype = ctypes.c_long

CLSCTX_ALL = 23
eRender, eCapture = 0, 1
DEVICE_STATE_ACTIVE = 1

AUDCLNT_SHAREMODE_SHARED = 0
AUDCLNT_SHAREMODE_EXCLUSIVE = 1

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_EXTENSIBLE = 0xFFFE

S_OK = 0
S_FALSE = 1
AUDCLNT_E_UNSUPPORTED_FORMAT = 0x88890008

SPEAKER_FRONT_LEFT = 0x1
SPEAKER_FRONT_RIGHT = 0x2

RATES = [44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000]
DEPTHS = [16, 24, 32]


class GUID(ctypes.Structure):
    _fields_ = [("Data1", c_ulong), ("Data2", c_ushort), ("Data3", c_ushort),
                ("Data4", ctypes.c_ubyte * 8)]


class PROPERTYKEY(ctypes.Structure):
    _fields_ = [("fmtid", GUID), ("pid", c_ulong)]


class WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("wFormatTag", c_ushort), ("nChannels", c_ushort),
                ("nSamplesPerSec", c_ulong), ("nAvgBytesPerSec", c_ulong),
                ("nBlockAlign", c_ushort), ("wBitsPerSample", c_ushort),
                ("cbSize", c_ushort)]


class WAVEFORMATEXTENSIBLE(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("Format", WAVEFORMATEX), ("wValidBitsPerSample", c_ushort),
                ("dwChannelMask", c_ulong), ("SubFormat", GUID)]


def guid(text):
    part = text.strip("{}").split("-")
    g = GUID()
    g.Data1 = int(part[0], 16)
    g.Data2 = int(part[1], 16)
    g.Data3 = int(part[2], 16)
    tail = part[3] + part[4]
    for i in range(8):
        g.Data4[i] = int(tail[i * 2:i * 2 + 2], 16)
    return g


CLSID_MMDeviceEnumerator = guid("BCDE0395-E52F-467C-8E3D-C4579291692E")
IID_IMMDeviceEnumerator = guid("A95664D2-9614-4F35-A746-DE8DB63617E6")
IID_IAudioClient = guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2")
PKEY_Device_FriendlyName = PROPERTYKEY(
    guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 14)
KSDATAFORMAT_SUBTYPE_PCM = guid("00000001-0000-0010-8000-00AA00389B71")
KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = guid("00000003-0000-0010-8000-00AA00389B71")


def vt(ptr, index, restype, *argtypes):
    """Return a callable for vtable slot `index` of a COM interface pointer."""
    table = ctypes.cast(ptr, POINTER(POINTER(c_void_p))).contents
    proto = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)
    return proto(table[index])


def release(ptr):
    if ptr:
        vt(ptr, 2, c_ulong)(ptr)


def hr(code):
    return code & 0xFFFFFFFF


def read_lpwstr_propvariant(pv):
    # PROPVARIANT: vt at +0, pointer payload at +8 (x64)
    vt_type = ctypes.cast(pv, POINTER(c_ushort)).contents.value
    if vt_type != 31:  # VT_LPWSTR
        return ""
    return ctypes.cast(ctypes.c_void_p.from_buffer(pv, 8),
                       c_wchar_p).value or ""


def make_format(rate, container_bits, valid_bits, subformat, channels, mask):
    """Build a WAVEFORMATEXTENSIBLE for a specific container/valid-bit layout."""
    wf = WAVEFORMATEXTENSIBLE()
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE
    wf.Format.nChannels = channels
    wf.Format.nSamplesPerSec = rate
    wf.Format.wBitsPerSample = container_bits
    wf.Format.nBlockAlign = channels * container_bits // 8
    wf.Format.nAvgBytesPerSec = rate * wf.Format.nBlockAlign
    wf.Format.cbSize = ctypes.sizeof(WAVEFORMATEXTENSIBLE) - ctypes.sizeof(WAVEFORMATEX)
    wf.wValidBitsPerSample = valid_bits
    wf.dwChannelMask = mask
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT if subformat == "float" \
        else KSDATAFORMAT_SUBTYPE_PCM
    return wf


# (label, container bits, valid bits, subformat)
VARIANTS = [
    ("16-bit pcm", 16, 16, "pcm"),
    ("24-bit pcm", 24, 24, "pcm"),
    ("24-in-32 pcm", 32, 24, "pcm"),
    ("32-bit pcm", 32, 32, "pcm"),
    ("32-bit float", 32, 32, "float"),
]


def describe(wf_ptr):
    if not wf_ptr:
        return None
    w = ctypes.cast(wf_ptr, POINTER(WAVEFORMATEX)).contents
    out = {"tag": w.wFormatTag, "channels": w.nChannels, "rate": w.nSamplesPerSec,
           "bits": w.wBitsPerSample,
           "block_align": w.nBlockAlign,
           "extensible": w.wFormatTag == WAVE_FORMAT_EXTENSIBLE}
    if out["extensible"]:
        we = ctypes.cast(wf_ptr, POINTER(WAVEFORMATEXTENSIBLE)).contents
        sub = we.SubFormat
        out["valid_bits"] = we.wValidBitsPerSample
        out["channel_mask"] = we.dwChannelMask
        key = "%08X-%04X" % (sub.Data1, sub.Data2)
        out["subformat"] = {"00000001-0000": "pcm",
                            "00000003-0000": "float"}.get(key, key)
    return out


def probe_device(device):
    dev_id = c_wchar_p()
    vt(device, 5, ctypes.c_long, POINTER(c_wchar_p))(device, byref(dev_id))

    store = c_void_p()
    vt(device, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(device, 0, byref(store))
    name = ""
    if store:
        pv = ctypes.create_string_buffer(32)
        if hr(vt(store, 5, ctypes.c_long, POINTER(PROPERTYKEY), c_void_p)(
                store, byref(PKEY_Device_FriendlyName), pv)) == S_OK:
            name = read_lpwstr_propvariant(pv)
        release(store)

    state = c_ulong()
    vt(device, 6, ctypes.c_long, POINTER(c_ulong))(device, byref(state))

    client = c_void_p()
    hr_code = hr(vt(device, 3, ctypes.c_long, POINTER(GUID), c_ulong,
                    c_void_p, POINTER(c_void_p))(
        device, byref(IID_IAudioClient), CLSCTX_ALL, None, byref(client)))
    if hr_code != S_OK or not client:
        return {"name": name or "(unnamed)", "id": dev_id.value,
                "state": state.value, "error": "Activate failed 0x%08X" % hr_code}

    mix_ptr = POINTER(WAVEFORMATEX)()
    mix = None
    if hr(vt(client, 8, ctypes.c_long, POINTER(POINTER(WAVEFORMATEX)))(
            client, byref(mix_ptr))) == S_OK:
        mix = describe(ctypes.cast(mix_ptr, c_void_p))

    channels = mix["channels"] if mix else 2
    mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    if channels > 2:
        mask = 0x3F  # 5.1 layout, good enough for a capability query

    matrix = {}
    is_supported = vt(client, 7, ctypes.c_long, ctypes.c_int,
                      POINTER(WAVEFORMATEX), POINTER(POINTER(WAVEFORMATEX)))
    for label, container, valid, sub in VARIANTS:
        row = {}
        for rate in RATES:
            wf = make_format(rate, container, valid, sub, channels, mask)
            closest = POINTER(WAVEFORMATEX)()
            code = hr(is_supported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   ctypes.cast(byref(wf), POINTER(WAVEFORMATEX)),
                                   byref(closest)))
            if code == S_OK:
                row[rate] = "ok"
            elif code == S_FALSE:
                near = describe(ctypes.cast(closest, c_void_p)) if closest else None
                row[rate] = "~%s/%s" % (near["rate"], near["bits"]) if near else "~"
            else:
                row[rate] = "-"
            if DEBUG:
                print("    [dbg] %-16s %d Hz -> 0x%08X" % (label, rate, code))
        matrix[label] = row

    release(client)
    return {"name": name or "(unnamed)", "id": dev_id.value, "state": state.value,
            "mix": mix, "exclusive": matrix}


def enumerate_devices():
    enumerator = c_void_p()
    ole32.CoCreateInstance(byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL,
                           byref(IID_IMMDeviceEnumerator), byref(enumerator))
    if not enumerator:
        raise RuntimeError("CoCreateInstance(MMDeviceEnumerator) failed")

    collection = c_void_p()
    vt(enumerator, 3, ctypes.c_long, ctypes.c_int, c_ulong, POINTER(c_void_p))(
        enumerator, eRender, DEVICE_STATE_ACTIVE, byref(collection))

    count = c_ulong()
    vt(collection, 3, ctypes.c_long, POINTER(c_ulong))(collection, byref(count))

    out = []
    for i in range(count.value):
        device = c_void_p()
        vt(collection, 4, ctypes.c_long, c_ulong, POINTER(c_void_p))(
            collection, i, byref(device))
        out.append(probe_device(device))
        release(device)

    release(collection)
    release(enumerator)
    return out


def main():
    global DEBUG
    DEBUG = "--debug" in sys.argv
    assert ctypes.sizeof(WAVEFORMATEX) == 18, ctypes.sizeof(WAVEFORMATEX)
    assert ctypes.sizeof(WAVEFORMATEXTENSIBLE) == 40, ctypes.sizeof(WAVEFORMATEXTENSIBLE)
    devices = enumerate_devices()
    if "--json" in sys.argv:
        print(json.dumps(devices, indent=2, ensure_ascii=False))
        return

    print("active render endpoints: %d\n" % len(devices))
    for d in devices:
        print("=" * 68)
        print(d["name"])
        print("  state: %d" % d["state"])
        if d.get("error"):
            print("  %s" % d["error"])
            continue
        mix = d["mix"]
        if mix:
            sub = mix.get("subformat", "pcm")
            print("  shared mix format (what an app writes): %d Hz / %d-bit %s container"
                  % (mix["rate"], mix["bits"], sub))
            if mix["extensible"]:
                print("  valid bits %d, channel mask 0x%X, %d ch"
                      % (mix["valid_bits"], mix["channel_mask"], mix["channels"]))
            print("  note: shared mode always hands out a 32-bit float mix; the depth"
                  " picked in the device property sheet is the engine->driver format")
        print("  exclusive-mode support:")
        header = "%-18s" % "" + "".join("%8s" % ("%.1fk" % (r / 1000.0)) for r in RATES)
        print(header)
        for label, _c, _v, _s in VARIANTS:
            cells = "".join("%8s" % d["exclusive"][label][r] for r in RATES)
            print("    %-14s%s" % (label, cells))
        print("    ok = exact   ~ = closest only   - = rejected")
    print("=" * 68)


if __name__ == "__main__":
    main()
