#!/usr/bin/env python3
"""Generate NeoHEN's embedded images from the originals in branding/.

The originals are never modified. Outputs:

  source/bootstrapper/assets/neohen/neohen_sicon.png
      128x128 RGBA, under 8 KB (24 colours), default. neohen_sicon_full.png is the
      same icon in full colour (~25 KB), selectable with NEOHEN_ICON=full.
      Colour emblem on a transparent background. Replaces
      onionhen_sicon.png: startup notification icon, HomeUI top-nav icon
      (normal and focused/inverted state) and the Settings entry icon.
  assets/neohen-logo.png
      512x512 RGBA, same emblem, for the README.
  assets/neohen-splash.png
      1280 px wide copy of the horizontal logo, for the README header.

The colour original (the crown) already has a transparent background, so
it is only cropped square and scaled; it shows on both the dark top bar and
the white focus highlight. The horizontal logo is drawn from the crown by
tools/branding/make_splash.py and stored in branding/ like an original,
because it depends on a font that is not in the repository.

Usage:  python3 tools/branding/make_icons.py [--check]
        --check only verifies that committed outputs are up to date.
Requires Pillow.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import sys
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "branding"
COLOR = SRC / "neohen-crown.png"
SPLASH = SRC / "neohen-splash.png"

OUT_SICON = ROOT / "source/bootstrapper/assets/neohen/neohen_sicon.png"
OUT_SICON_FULL = ROOT / "source/bootstrapper/assets/neohen/neohen_sicon_full.png"
OUT_LOGO = ROOT / "assets/neohen-logo.png"
OUT_SPLASH = ROOT / "assets/neohen-splash.png"

SICON_SIZE = 128
# The OnionHEN icon that HomeUI is known to load is 128x128 RGBA, 5.9 KB.
# Stay in the same order of magnitude until a larger file is tested.
SICON_MAX_BYTES = 32 * 1024
# The default icon is reduced towards that known-good size: colour noise is
# smoothed (alpha untouched) and the image limited to SICON_LIGHT_COLORS
# colours, then stored again as 128x128 RGBA.
SICON_LIGHT_COLORS = 24
SICON_LIGHT_SMOOTH = 0.9
SICON_LIGHT_MAX_BYTES = 8 * 1024
LOGO_SIZE = 512
SPLASH_WIDTH = 1280

def square_crop(img: Image.Image, margin: float) -> Image.Image:
    """Square, centred crop around the visible pixels, with a margin."""
    box = img.getchannel("A").point(lambda v: 255 if v >= 128 else 0
                                    ).getbbox()
    if not box:
        raise SystemExit("could not find the emblem in the colour original")
    left, top, right, bottom = box
    side = round(max(right - left, bottom - top) * (1 + 2 * margin))
    cx, cy = (left + right) / 2, (top + bottom) / 2
    l, t = round(cx - side / 2), round(cy - side / 2)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img.crop((l, t, l + side, t + side)), (0, 0))
    return canvas


def build_emblem(size: int, margin: float) -> Image.Image:
    color = Image.open(COLOR).convert("RGBA")
    if color.getchannel("A").getextrema()[0] != 0:
        raise SystemExit("the colour original must have a transparent "
                         "background")
    return square_crop(color, margin).resize((size, size), Image.LANCZOS)


def lighten(img: Image.Image) -> Image.Image:
    """Fewer colours and alpha levels, same size and RGBA mode: a much
    smaller PNG. The crown has far more detail than fits in 8 KB, so this
    one is soft; at the ~40 px HomeUI draws it, it reads the same."""
    rgb = img.convert("RGB").filter(ImageFilter.GaussianBlur(SICON_LIGHT_SMOOTH))
    alpha = img.getchannel("A").point(
        lambda v: 0 if v < 24 else 255 if v > 231 else round(v / 51) * 51)
    out = rgb.quantize(colors=SICON_LIGHT_COLORS,
                       method=Image.Quantize.MAXCOVERAGE,
                       dither=Image.Dither.NONE).convert("RGBA")
    out.putalpha(alpha)
    # fully transparent pixels all get the same colour, so they compress
    clear = Image.new("RGBA", out.size, (0, 0, 0, 0))
    return Image.composite(out, clear, alpha.point(lambda v: 255 if v else 0))


def png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    # Fixed settings and no metadata so the output is reproducible.
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def outputs() -> dict[Path, bytes]:
    for f in (COLOR, SPLASH):
        if not f.is_file():
            raise SystemExit(f"missing original: {f.relative_to(ROOT)}")
    sicon = build_emblem(SICON_SIZE, margin=0.06)
    logo = build_emblem(LOGO_SIZE, margin=0.06)
    splash = Image.open(SPLASH).convert("RGB")
    h = round(splash.height * SPLASH_WIDTH / splash.width)
    splash = splash.resize((SPLASH_WIDTH, h), Image.LANCZOS)

    data = {OUT_SICON: png_bytes(lighten(sicon)),
            OUT_SICON_FULL: png_bytes(sicon),
            OUT_LOGO: png_bytes(logo), OUT_SPLASH: png_bytes(splash)}
    for path, limit in ((OUT_SICON, SICON_LIGHT_MAX_BYTES),
                        (OUT_SICON_FULL, SICON_MAX_BYTES)):
        if len(data[path]) > limit:
            raise SystemExit(f"{path.name} is {len(data[path])} bytes "
                             f"(limit {limit})")
    return data


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="fail if committed outputs differ from a fresh run")
    args = ap.parse_args()

    stale = []
    for path, blob in outputs().items():
        rel = path.relative_to(ROOT)
        with Image.open(io.BytesIO(blob)) as im:
            desc = f"{im.size[0]}x{im.size[1]} {im.mode}"
        digest = hashlib.sha256(blob).hexdigest()[:12]
        if args.check:
            if not path.is_file() or path.read_bytes() != blob:
                stale.append(str(rel))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        print(f"{rel}: {desc}, {len(blob)} bytes, sha256 {digest}")
    if stale:
        print("out of date: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
