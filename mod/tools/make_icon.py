"""Regenerate mod/tools/nexhost.ico (the host's tray and file icon).

    python -I mod/tools/make_icon.py

A plain, generic mark: a dark rounded square with four level bars. It is drawn
at 1024 px and downsampled per size so the small tray sizes stay crisp.
Requires Pillow. The .ico is committed; this script only documents how.
"""
import os

from PIL import Image, ImageDraw

BACKGROUND = (38, 50, 92, 255)      # deep indigo
BAR = (255, 255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def draw(size):
    scale = 1024
    img = Image.new("RGBA", (scale, scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([40, 40, scale - 40, scale - 40], radius=220, fill=BACKGROUND)
    heights = [0.38, 0.62, 0.86, 0.52]          # left to right, share of the inner height
    left, right, bottom, top = 230, scale - 230, scale - 250, 250
    gap = 46
    width = (right - left - gap * (len(heights) - 1)) / len(heights)
    for i, h in enumerate(heights):
        x0 = left + i * (width + gap)
        y0 = bottom - (bottom - top) * h
        d.rounded_rectangle([x0, y0, x0 + width, bottom], radius=int(width / 2), fill=BAR)
    return img.resize((size, size), Image.LANCZOS)


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nexhost.ico")
    frames = [draw(s) for s in SIZES]
    frames[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print("wrote", out)


if __name__ == "__main__":
    main()
