"""Minimal PE inspector (stdlib only).

Dumps import / delay-import DLLs and greps ASCII + UTF-16LE strings for
audio-stack keywords, so we can tell which Windows audio API a binary uses.

Usage:
    python inspect_pe.py <pe-file> [keyword ...]
"""

import re
import struct
import sys

AUDIO_KEYWORDS = [
    "wasapi", "mmdevapi", "audioses", "audioclient", "iaudioclient",
    "iaudiorenderclient", "audclnt", "waveout", "directsound", "dsound",
    "asio", "wdmaud", "winmm", "avrt", "ksuser", "waveformat",
    "exclusive", "shared", "sample rate", "samplerate", "bits per sample",
    "bass", "portaudio", "cubeb", "miniaudio", "soxr", "swresample",
]


def load_sections(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    coff = e_lfanew + 4
    machine, nsec, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, coff)
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    pe32plus = magic == 0x20B
    dd_off = opt + (112 if pe32plus else 96)
    sections = []
    sec_off = opt + opt_size
    for i in range(nsec):
        base = sec_off + i * 40
        name = data[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((name, vaddr, max(vsize, rawsize), rawptr))
    return {
        "machine": machine,
        "pe32plus": pe32plus,
        "data_dirs": dd_off,
        "sections": sections,
        "image_base": struct.unpack_from("<Q" if pe32plus else "<I", data, opt + (24 if pe32plus else 28))[0],
    }


def rva_to_off(info, rva):
    for _, vaddr, vsize, rawptr in info["sections"]:
        if vaddr <= rva < vaddr + vsize:
            return rawptr + (rva - vaddr)
    return None


def read_cstr(data, off):
    if off is None:
        return ""
    end = data.find(b"\0", off)
    return data[off:end].decode("ascii", "replace") if end > off else ""


def parse_thunks(data, info, rva):
    out = []
    off = rva_to_off(info, rva)
    if off is None:
        return out
    step = 8 if info["pe32plus"] else 4
    flag = 1 << (63 if info["pe32plus"] else 31)
    fmt = "<Q" if info["pe32plus"] else "<I"
    for i in range(4096):
        thunk = struct.unpack_from(fmt, data, off + i * step)[0]
        if thunk == 0:
            break
        if thunk & flag:
            out.append("#%d" % (thunk & 0xFFFF))
        else:
            name_off = rva_to_off(info, thunk + 2)
            out.append(read_cstr(data, name_off))
    return out


def imports(data, info):
    dd = info["data_dirs"]
    result = {}
    for label, idx in (("import", 1), ("delay", 13)):
        rva, size = struct.unpack_from("<II", data, dd + idx * 8)
        if not rva:
            continue
        off = rva_to_off(info, rva)
        if off is None:
            continue
        entry = 32 if label == "delay" else 20
        for i in range(4096):
            base = off + i * entry
            if label == "delay":
                name_rva = struct.unpack_from("<I", data, base + 4)[0]
                int_rva = struct.unpack_from("<I", data, base + 16)[0]
            else:
                int_rva = struct.unpack_from("<I", data, base)[0]
                name_rva = struct.unpack_from("<I", data, base + 12)[0]
            if not name_rva:
                break
            dll = read_cstr(data, rva_to_off(info, name_rva))
            funcs = parse_thunks(data, info, int_rva) if int_rva else []
            result.setdefault("%s:%s" % (label, dll.lower()), []).extend(funcs)
    return result


def strings(data, min_len=4):
    seen = set()
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
        s = m.group().decode("ascii", "replace")
        if s not in seen:
            seen.add(s)
            yield s
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % min_len, data):
        s = m.group().decode("utf-16-le", "replace")
        if s not in seen:
            seen.add(s)
            yield s


def main():
    path = sys.argv[1]
    keywords = [k.lower() for k in sys.argv[2:]] or AUDIO_KEYWORDS
    data = open(path, "rb").read()
    info = load_sections(data)
    print("== %s ==" % path)
    print("size: %.2f MB  pe32+: %s  sections: %d" % (
        len(data) / 1048576.0, info["pe32plus"], len(info["sections"])))

    imp = imports(data, info)
    audio_dlls = [k for k in imp if any(t in k for t in
                  ("audio", "mmdev", "dsound", "winmm", "avrt", "ksuser",
                   "ffmpeg", "avcodec", "swresample", "flac", "cef", "curl"))]
    print("\n-- imported DLLs (%d) --" % len(imp))
    for k in sorted(imp):
        print("   %s" % k)

    print("\n-- audio-ish imports --")
    for k in sorted(audio_dlls):
        funcs = imp[k]
        shown = [f for f in funcs if any(w in f.lower() for w in
                 ("audio", "client", "render", "device", "wave", "direct",
                  "format", "buffer", "asio", "endpoint"))]
        print("   %s  (%d imports)" % (k, len(funcs)))
        for f in shown[:25]:
            print("       %s" % f)

    print("\n-- keyword strings --")
    hits = {}
    for s in strings(data, 5):
        low = s.lower()
        for kw in keywords:
            if kw in low:
                hits.setdefault(kw, set()).add(s[:120])
                break
    for kw in keywords:
        found = sorted(hits.get(kw, ()))
        if found:
            print("   [%s] %d hit(s)" % (kw, len(found)))
            for s in found[:12]:
                print("       %s" % s)


if __name__ == "__main__":
    main()
