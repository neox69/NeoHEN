#!/usr/bin/env python3
"""Make a NeoHEN theme folder from one picture.

The picture is cropped to 16:9, scaled to 3840x2160 and saved as every
background of the PS5 menu, in the console's format (DXT1 in a DDS file,
zstd), plus a preview.jpg and a theme.json. Copy the folder to
/data/neohen/themes/ on the console.

Usage:  python3 themes/tools/make_theme.py <picture> <theme name> <out_dir> [author]
Requires numpy, Pillow and zstandard.
"""
import io
import json
import os
import struct
import sys

import numpy as np
import zstandard
from PIL import Image

W, H = 3840, 2160
# Every background file of the PS5 menu (/system_ex/vsh_asset).
BGS = ["bg_hub_default", "Library_BG_blue_Low_Angle_001", "bg_NPXS40037",
       "bg_NPXS40047", "bg_NPXS40053", "bg_NPXS40054", "bg_NPXS40056",
       "bg_NPXS40063", "bg_NPXS40071", "bg_NPXS40075", "bg_NPXS40080",
       "bg_NPXS40110", "bg_NPXS40139", "bg_NPXS40144", "bg_NPXS40154"]


def dds_header():
    """The 128 bytes the console's own DXT1 backgrounds start with."""
    pf = struct.pack("<II4s5I", 32, 4, b"DXT1", 0, 0, 0, 0, 0)
    return (b"DDS " + struct.pack("<7I", 124, 0xA1007, H, W, W * H // 2, 1, 1)
            + b"\0" * 44 + pf + struct.pack("<5I", 0x1000, 0, 0, 0, 0))


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__, file=sys.stderr)
        return 2
    picture, name, out = sys.argv[1:4]
    author = sys.argv[4] if len(sys.argv) == 5 else ""

    im = Image.open(picture).convert("RGB")
    scale = max(W / im.width, H / im.height)
    im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    left, top = (im.width - W) // 2, (im.height - H) // 2
    im = im.crop((left, top, left + W, top + H))
    # a little noise hides the banding DXT1 gives smooth gradients
    px = np.asarray(im, np.float32)
    px += np.random.default_rng(1).normal(0, 1.2, px.shape).astype(np.float32)
    im = Image.fromarray(np.clip(px, 0, 255).astype(np.uint8), "RGB")

    buf = io.BytesIO()
    im.save(buf, format="DDS", pixel_format="DXT1")
    dds = dds_header() + buf.getvalue()[128:]
    assert len(dds) == 128 + W * H // 2
    packed = zstandard.ZstdCompressor(level=19).compress(dds)

    folder = os.path.join(out, name)
    os.makedirs(folder, exist_ok=True)
    for bg in BGS:
        with open(os.path.join(folder, bg + ".dds.zst"), "wb") as f:
            f.write(packed)
    im.resize((640, 360), Image.LANCZOS).save(os.path.join(folder, "preview.jpg"), quality=88)
    with open(os.path.join(folder, "theme.json"), "w", encoding="utf-8") as f:
        json.dump({"name": name, "by": author}, f)
        f.write("\n")
    print(name, len(packed), "bytes per background")
    return 0


if __name__ == "__main__":
    sys.exit(main())
