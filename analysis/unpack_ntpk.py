"""Unpack NetEase .ntpk / .skin containers.

Most of them are "NTPK"-magic archives, but orpheus.ntpk is a 100-byte
prefix followed by a plain ZIP stream. Detect both, then either dump the
NTPK header or extract the ZIP.

Usage:
    python unpack_ntpk.py <file> --out <dir>
    python unpack_ntpk.py <file> --scan "exclusive|wasapi"
"""

import argparse
import io
import os
import re
import struct
import sys
import zipfile


def zip_prefix_length(data):
    """If the file ends with a valid ZIP, return the length of the prefix."""
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0:
        return None
    cd_size, cd_offset = struct.unpack_from("<II", data, eocd + 12)
    actual = data.find(b"PK\x01\x02")
    if actual < 0:
        return None
    delta = actual - cd_offset
    if delta < 0:
        return None
    # sanity: the first local header must sit at the very start of the zip
    if data.find(b"PK\x03\x04") < delta:
        return None
    return delta


def dump_ntpk_header(data):
    magic = data[:4]
    version = struct.unpack_from("<I", data, 4)[0]
    count = struct.unpack_from("<I", data, 8)[0]
    print("NTPK magic=%r version=%d entries=%d" % (magic, version, count))
    for i in range(min(count, 12)):
        off = 12 + i * 24
        if off + 24 > len(data):
            break
        fields = struct.unpack_from("<IIIIII", data, off)
        print("  entry %d: %s" % (i, fields))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--out")
    ap.add_argument("--scan")
    ap.add_argument("--list", dest="list_pattern", help="List member names matching a regex")
    ap.add_argument("--member", action="append", default=[],
                    help="Extract only this exact ZIP member; may be repeated")
    ap.add_argument("--text-view", action="store_true",
                    help="Write a wrapped UTF-8 inspection copy beside each member")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    print("%s  %.1f MB" % (args.file, len(data) / 1048576.0))

    if data[:4] == b"NTPK":
        dump_ntpk_header(data)
        return

    prefix = zip_prefix_length(data)
    if prefix is None:
        print("not a recognizable container")
        return
    print("prefix: %d bytes" % prefix)
    print("prefix hex: %s" % data[:prefix].hex()[:120])

    blob = data[prefix:]
    zf = zipfile.ZipFile(io.BytesIO(blob))
    names = zf.namelist()
    print("zip entries: %d" % len(names))

    if args.list_pattern:
        pattern = re.compile(args.list_pattern, re.I)
        for name in names:
            if pattern.search(name):
                print(name)
        return

    if args.member:
        if not args.out:
            ap.error("--member requires --out")
        root = os.path.abspath(args.out)
        for name in args.member:
            if name not in names or zf.getinfo(name).is_dir():
                ap.error("archive member not found: " + name)
            target = os.path.abspath(os.path.join(root, name.replace("\\", "/")))
            if os.path.commonpath([root, target]) != root:
                ap.error("unsafe archive member: " + name)
        os.makedirs(root, exist_ok=True)
        for name in args.member:
            target = zf.extract(name, root)
            print("extracted member: " + name)
            if args.text_view:
                text = zf.read(name).decode("utf-8")
                with open(target + ".view.txt", "w", encoding="utf-8") as view:
                    view.write("INSPECTION COPY ONLY; wrapped every 240 characters; not executable.\n")
                    for offset in range(0, len(text), 240):
                        view.write("%08d: %s\n" % (offset, text[offset:offset + 240]))
        return

    if args.scan:
        pattern = re.compile(args.scan.encode(), re.I)
        hits = 0
        for info in zf.infolist():
            if info.is_dir():
                continue
            try:
                content = zf.read(info)
            except Exception as exc:
                print("  ! %s: %s" % (info.filename, exc))
                continue
            if pattern.search(content):
                hits += 1
                print("  [hit] %s (%d bytes)" % (info.filename, info.file_size))
        print("files matching: %d" % hits)
        return

    if not args.out:
        for n in names[:20]:
            print("  " + n)
        return

    os.makedirs(args.out, exist_ok=True)
    zf.extractall(args.out)
    print("extracted to %s" % args.out)


if __name__ == "__main__":
    main()
