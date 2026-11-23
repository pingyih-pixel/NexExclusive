"""Compare two raw PCM captures byte for byte.

Typical use: the DLL's capture (NEX_DUMP, the PCM it hands to the device or
to the separate host) against the separate host's capture (NEX_HOST_DUMP, the
PCM the host hands to the device). With audiohost=1 the two must be identical;
anything else means the transport changed samples.

    python compare_pcm.py dll.pcm host.pcm --frame-bytes 6
    python compare_pcm.py a.pcm b.pcm --frame-bytes 8 --search 48000

--frame-bytes is channels * container bytes (24-bit packed stereo = 6, 32-bit
stereo = 8). --search tries offsets of up to N frames in either direction and
reports the one with the longest identical run, for captures that did not
start at the same frame. Exit code 0 when the overlapping part is identical.
"""
import argparse
import sys


def identical_run(a, b, off_a, off_b):
    n = min(len(a) - off_a, len(b) - off_b)
    if n <= 0:
        return 0
    lo, hi = 0, n
    # binary search on the longest equal prefix, in chunks
    if a[off_a:off_a + n] == b[off_b:off_b + n]:
        return n
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if a[off_a:off_a + mid] == b[off_b:off_b + mid]:
            lo = mid
        else:
            hi = mid - 1
    return lo


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--frame-bytes", type=int, required=True)
    ap.add_argument("--search", type=int, default=0, help="frames of offset to try in each direction")
    args = ap.parse_args()
    fb = args.frame_bytes
    if fb <= 0:
        sys.exit("--frame-bytes must be positive")
    with open(args.a, "rb") as f:
        a = memoryview(f.read())
    with open(args.b, "rb") as f:
        b = memoryview(f.read())
    print(f"a: {len(a)} bytes, {len(a) // fb} frames{' (partial frame at end)' if len(a) % fb else ''}")
    print(f"b: {len(b)} bytes, {len(b) // fb} frames{' (partial frame at end)' if len(b) % fb else ''}")

    best = (0, 0, 0)  # run, off_a, off_b
    offsets = range(-args.search, args.search + 1) if args.search else [0]
    for shift in offsets:
        off_a, off_b = (shift * fb, 0) if shift >= 0 else (0, -shift * fb)
        if off_a >= len(a) or off_b >= len(b):
            continue
        run = identical_run(a, b, off_a, off_b)
        if run > best[0]:
            best = (run, off_a, off_b)
        if run == min(len(a) - off_a, len(b) - off_b):
            break
    run, off_a, off_b = best
    overlap = min(len(a) - off_a, len(b) - off_b)
    if args.search:
        print(f"best alignment: a+{off_a // fb} frames vs b+{off_b // fb} frames")
    print(f"overlap: {overlap // fb} frames; identical: {run // fb} frames")
    if run >= overlap:
        print("IDENTICAL over the whole overlap")
        return 0
    frame = run // fb
    i_a, i_b = off_a + frame * fb, off_b + frame * fb
    print(f"first difference at frame {frame} (a byte {i_a}, b byte {i_b})")
    print(f"  a: {a[i_a:i_a + fb].hex(' ')}")
    print(f"  b: {b[i_b:i_b + fb].hex(' ')}")
    diff_frames = 0
    for k in range(0, overlap - overlap % fb, fb):
        if a[off_a + k:off_a + k + fb] != b[off_b + k:off_b + k + fb]:
            diff_frames += 1
    print(f"frames that differ: {diff_frames} of {overlap // fb}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
