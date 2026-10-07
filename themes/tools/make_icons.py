#!/usr/bin/env python3
"""Tint the console's own menu icons for a theme.

  python3 themes/tools/make_icons.py <stock dir> <theme dir> <#dark> <#accent>

<stock dir> holds the icons as read from a console, in their own tree
(system_ex/app/<ID>/sce_sys/icon0.png, system_ex/rnps/apps/<ID>/appdb/<ID>/
icon0.png). Each one is a light glyph on a dark tile: the tile is repainted
as a gradient from <#dark> to <#accent> and the glyph is kept. The results go
into the same tree inside <theme dir>, where the themes service finds them.

Needs Pillow and numpy.
"""
import os
import sys

import numpy as np
from PIL import Image

# what the home screen shows; debug and preview apps are left alone
WANTED = {
    "NPXS40053", "NPXS40054", "NPXS40056", "NPXS40037", "NPXS40047",
    "NPXS40071", "NPXS40139", "NPXS40075", "NPXS40074", "NPXS40099",
    "NPXS40140", "NPXS40149", "NPXS40150",
}


def rgb(text):
    text = text.lstrip("#")
    return np.array([int(text[i:i + 2], 16) for i in (0, 2, 4)], dtype=np.float32)


def tint(src, dst, dark, accent):
    im = np.asarray(Image.open(src).convert("RGBA"), dtype=np.float32)
    h, w = im.shape[:2]
    px, a = im[..., :3], im[..., 3:] / 255.0
    lum = px @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    sat = px.max(axis=2) - px.min(axis=2)
    # the glyph: whatever is clearly lighter, or more coloured, than the tile
    glyph = np.clip(np.maximum((lum - 70.0) / 110.0, (sat - 50.0) / 70.0), 0.0, 1.0)[..., None]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    t = np.clip((xx / w * 0.45 + yy / h * 0.75), 0.0, 1.0)[..., None]
    glow = np.exp(-(((xx - w * 0.5) ** 2 + (yy - h * 0.55) ** 2) / (2 * (w * 0.42) ** 2)))[..., None]
    tile = dark * (1.0 - t) + accent * t * 0.55 + accent * glow * 0.18
    out = np.clip(tile * (1.0 - glyph) + px * glyph, 0, 255)
    res = np.concatenate([out, a * 255.0], axis=2).astype(np.uint8)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    Image.fromarray(res, "RGBA").save(dst, optimize=True)


def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    stock, theme = sys.argv[1], sys.argv[2]
    dark, accent = rgb(sys.argv[3]), rgb(sys.argv[4])
    made = 0
    for folder, _, files in os.walk(stock):
        parts = folder.replace(os.sep, "/").split("/")
        ident = parts[-2] if parts[-1] == "sce_sys" else parts[-1]
        if ident == "default":
            ident = parts[-3]
        if ident not in WANTED:
            continue
        for name in files:
            if name in ("icon0.png", "icon0_4k.png"):
                rel = os.path.relpath(os.path.join(folder, name), stock)
                tint(os.path.join(folder, name), os.path.join(theme, rel), dark, accent)
                made += 1
    print(f"{made} icons -> {theme}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
