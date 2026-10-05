#!/usr/bin/env python3
"""Generate NeoHEN's embedded images from the originals in branding/.

The originals are never modified. Outputs:

  source/bootstrapper/assets/neohen/neohen_sicon.png
      128x128 RGBA, ~6 KB (48 colours), default. neohen_sicon_full.png is the
      same icon in full colour (~23 KB), selectable with NEOHEN_ICON=full.
      Colour emblem on a transparent background. Replaces
      onionhen_sicon.png: startup notification icon, HomeUI top-nav icon
      (normal and focused/inverted state) and the Settings entry icon.
  assets/neohen-logo.png
      512x512 RGBA, same emblem, for the README.
  assets/neohen-splash.png
      1280 px wide copy of the horizontal logo, for the README header.

The colour original has an opaque dark background with a glow. The emblem is
keyed out of it (see key_out_background) so it shows on both the dark top
bar and the white focus highlight. The monochrome original is not used: its
geometry differs from the colour one (aspect 0.846 vs 0.876), so it cannot
serve as a cut-out mask; it stays in branding/ for single-colour uses.

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

from PIL import Image, ImageChops, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "branding"
COLOR = SRC / "neohen-icon.png"
MONO = SRC / "neohen-icon-mono.png"
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
SICON_LIGHT_COLORS = 48
SICON_LIGHT_SMOOTH = 0.6
SICON_LIGHT_MAX_BYTES = 8 * 1024
LOGO_SIZE = 512
SPLASH_WIDTH = 1280
# Background of the colour original (#0B0F1A, the NeoHEN palette).
BACKGROUND = (0x0B, 0x0F, 0x1A)
BACKGROUND_TOLERANCE = 16
# Brightest-channel values: at or below KEY_LOW a pixel is background or
# faint glow (transparent); at or above KEY_HIGH it is emblem body (opaque).
KEY_LOW = 70
KEY_HIGH = 200


def measure_background(img: Image.Image) -> tuple[int, int, int]:
    """Median colour of the four 32x32 corners of the colour original."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = []
    for x0, y0 in ((0, 0), (w - 32, 0), (0, h - 32), (w - 32, h - 32)):
        raw = rgb.crop((x0, y0, x0 + 32, y0 + 32)).tobytes()
        px.extend(tuple(raw[i:i + 3]) for i in range(0, len(raw), 3))
    return tuple(sorted(p[i] for p in px)[len(px) // 2] for i in range(3))


def key_out_background(img: Image.Image,
                       bg: tuple[int, int, int]) -> Image.Image:
    """Colour emblem on transparency, keyed against the known background.

    The colour original is a bright emblem plus glow on a flat dark
    background (bg). Alpha comes from the brightest channel, ramped
    between KEY_LOW (pure background/far glow) and KEY_HIGH (emblem body);
    colours of partly transparent pixels are un-mixed from the background so
    edges keep the emblem's hue instead of a dark fringe.
    """
    rgb = img.convert("RGB")
    r, g, b = rgb.split()
    peak = ImageChops.lighter(ImageChops.lighter(r, g), b)
    span = KEY_HIGH - KEY_LOW
    alpha = peak.point(lambda v: 0 if v <= KEY_LOW else 255 if v >= KEY_HIGH
                       else round((v - KEY_LOW) * 255 / span))
    out = Image.new("RGBA", rgb.size)
    src = rgb.load()
    al = alpha.load()
    dst = out.load()
    w, h = rgb.size
    for y in range(h):
        for x in range(w):
            a = al[x, y]
            if a == 0:
                dst[x, y] = (0, 0, 0, 0)
                continue
            f = a / 255
            px = src[x, y]
            dst[x, y] = tuple(
                max(0, min(255, round((px[i] - bg[i] * (1 - f)) / f)))
                for i in range(3)) + (a,)
    return out


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
    color = Image.open(COLOR)
    bg = measure_background(color)
    if max(abs(a - b) for a, b in zip(bg, BACKGROUND)) > BACKGROUND_TOLERANCE:
        raise SystemExit(f"colour original background {bg} is too far from "
                         f"the expected {BACKGROUND}")
    emblem = square_crop(key_out_background(color, bg), margin)
    return emblem.resize((size, size), Image.LANCZOS)


def lighten(img: Image.Image) -> Image.Image:
    """Fewer colours, same size and RGBA mode, for a much smaller PNG."""
    rgb = img.convert("RGB").filter(ImageFilter.GaussianBlur(SICON_LIGHT_SMOOTH))
    smooth = rgb.convert("RGBA")
    smooth.putalpha(img.getchannel("A"))
    return smooth.quantize(colors=SICON_LIGHT_COLORS,
                           method=Image.Quantize.FASTOCTREE,
                           dither=Image.Dither.NONE).convert("RGBA")


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
