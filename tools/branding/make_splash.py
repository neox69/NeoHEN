#!/usr/bin/env python3
"""Draw branding/neohen-splash.png: the crown next to the NeoHEN wordmark.

The wordmark (branding/neohen-wordmark.png, white on transparency) is the
lettering of the first NeoHEN logo, cut out of branding/previous/. Run by
hand when either image changes; the result is committed.

Usage:  python3 tools/branding/make_splash.py
Requires Pillow.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
CROWN = ROOT / "branding/neohen-crown.png"
WORD = ROOT / "branding/neohen-wordmark.png"
OUT = ROOT / "branding/neohen-splash.png"

W, H = 2560, 960
BG = (7, 9, 26)


def main() -> int:
    canvas = Image.new("RGB", (W, H), BG)

    crown = Image.open(CROWN).convert("RGBA")
    crown = crown.crop(crown.getchannel("A").getbbox())
    ch = 640
    crown = crown.resize((round(crown.width * ch / crown.height), ch),
                         Image.LANCZOS)

    word = Image.open(WORD).convert("RGBA")
    tw = 1300
    word = word.resize((tw, round(word.height * tw / word.width)),
                       Image.LANCZOS)
    gap = 120
    x0 = (W - (crown.width + gap + tw)) // 2
    cy = (H - ch) // 2

    art = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    art.paste(crown, (x0, cy), crown)
    # soft glow behind the crown only; the lettering stays plain white
    halo = art.filter(ImageFilter.GaussianBlur(46))
    halo.putalpha(halo.getchannel("A").point(lambda v: v * 2 // 5))
    canvas.paste(halo, (0, 0), halo)
    art.alpha_composite(word, (x0 + crown.width + gap, (H - word.height) // 2))
    canvas.paste(art, (0, 0), art)

    canvas.save(OUT, optimize=True)
    print(OUT.relative_to(ROOT), canvas.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
