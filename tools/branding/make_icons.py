#!/usr/bin/env python3
"""Generate NeoHEN's embedded images from the originals in branding/.

The originals are never modified. Outputs:

  source/bootstrapper/assets/neohen/neohen_sicon.png
      128x128 RGBA, colour emblem on a transparent background. Replaces
      onionhen_sicon.png: startup notification icon, HomeUI top-nav icon
      (normal and focused/inverted state) and the Settings entry icon.
  assets/neohen-logo.png
      512x512 RGBA, same emblem, for the README.
  assets/neohen-splash.png
      1280 px wide copy of the horizontal logo, for the README header.

The colour original has an opaque dark background with a glow, so the emblem
is cut out with the shape of the monochrome original: both are cropped to
their emblem bounding box, scaled to the same size and the mono alpha is
applied to the colour pixels. The script checks that both shapes really
match before using them (see --min-overlap).

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

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "branding"
COLOR = SRC / "neohen-icon.png"
MONO = SRC / "neohen-icon-mono.png"
SPLASH = SRC / "neohen-splash.png"

OUT_SICON = ROOT / "source/bootstrapper/assets/neohen/neohen_sicon.png"
OUT_LOGO = ROOT / "assets/neohen-logo.png"
OUT_SPLASH = ROOT / "assets/neohen-splash.png"

SICON_SIZE = 128
# The OnionHEN icon that HomeUI is known to load is 128x128 RGBA, 5.9 KB.
# Stay in the same order of magnitude until a larger file is tested.
SICON_MAX_BYTES = 32 * 1024
LOGO_SIZE = 512
SPLASH_WIDTH = 1280
# Fraction of the emblem area where the colour and mono shapes must agree.
MIN_OVERLAP = 0.90
# Brightness above which a colour pixel belongs to the emblem, not the glow.
EMBLEM_THRESHOLD = 150


def emblem_mask_from_color(img: Image.Image) -> Image.Image:
    """Binary mask of the emblem body in the colour original (no glow)."""
    gray = img.convert("L")
    return gray.point(lambda v: 255 if v >= EMBLEM_THRESHOLD else 0)


def emblem_mask_from_mono(img: Image.Image) -> Image.Image:
    rgba = img.convert("RGBA")
    alpha = rgba.getchannel("A")
    if alpha.getextrema() == (255, 255):
        # No transparency: fall back to brightness (white emblem).
        alpha = rgba.convert("L")
    return alpha.point(lambda v: 255 if v >= 128 else 0)


def square_box(box: tuple[int, int, int, int], margin: float,
               limit: tuple[int, int]) -> tuple[int, int, int, int]:
    left, top, right, bottom = box
    side = max(right - left, bottom - top)
    side = int(round(side * (1 + 2 * margin)))
    cx, cy = (left + right) / 2, (top + bottom) / 2
    l = int(round(cx - side / 2))
    t = int(round(cy - side / 2))
    return (l, t, l + side, t + side)


def crop_square(img: Image.Image, box: tuple[int, int, int, int],
                fill) -> Image.Image:
    """Crop that may extend past the image edges (padded with fill)."""
    side = box[2] - box[0]
    out = Image.new(img.mode, (side, side), fill)
    out.paste(img.crop(box), (0, 0))
    return out


def overlap(a: Image.Image, b: Image.Image) -> float:
    inter = ImageChops.multiply(a, b).histogram()[255]
    union = ImageChops.lighter(a, b).histogram()[255]
    return inter / union if union else 0.0


def build_emblem(size: int, margin: float, min_overlap: float) -> Image.Image:
    color = Image.open(COLOR).convert("RGBA")
    mono = Image.open(MONO).convert("RGBA")
    cmask = emblem_mask_from_color(color)
    mmask = emblem_mask_from_mono(mono)
    cbox, mbox = cmask.getbbox(), mmask.getbbox()
    if not cbox or not mbox:
        raise SystemExit("could not find the emblem in the originals")

    csq = square_box(cbox, margin, color.size)
    msq = square_box(mbox, margin, mono.size)
    big = 1024
    c = crop_square(color, csq, (0, 0, 0, 255)).resize((big, big),
                                                       Image.LANCZOS)
    cm = crop_square(cmask, csq, 0).resize((big, big), Image.NEAREST)
    mm = crop_square(mmask, msq, 0).resize((big, big), Image.NEAREST)

    score = overlap(cm, mm)
    print(f"shape overlap colour/mono: {score:.3f}")
    if score < min_overlap:
        raise SystemExit(
            f"colour and mono emblems do not match (overlap {score:.3f} < "
            f"{min_overlap}); a colour emblem on a transparent background is "
            "needed instead")

    alpha = crop_square(mono, msq, (0, 0, 0, 0)).getchannel("A")
    if alpha.getextrema() == (255, 255):
        alpha = crop_square(mmask, msq, 0)
    alpha = alpha.resize((big, big), Image.LANCZOS)
    c.putalpha(alpha)
    return c.resize((size, size), Image.LANCZOS)


def png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    # Fixed settings and no metadata so the output is reproducible.
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def outputs() -> dict[Path, bytes]:
    for f in (COLOR, MONO, SPLASH):
        if not f.is_file():
            raise SystemExit(f"missing original: {f.relative_to(ROOT)}")
    sicon = build_emblem(SICON_SIZE, margin=0.04, min_overlap=MIN_OVERLAP)
    logo = build_emblem(LOGO_SIZE, margin=0.04, min_overlap=MIN_OVERLAP)
    splash = Image.open(SPLASH).convert("RGB")
    h = round(splash.height * SPLASH_WIDTH / splash.width)
    splash = splash.resize((SPLASH_WIDTH, h), Image.LANCZOS)

    data = {OUT_SICON: png_bytes(sicon), OUT_LOGO: png_bytes(logo),
            OUT_SPLASH: png_bytes(splash)}
    if len(data[OUT_SICON]) > SICON_MAX_BYTES:
        raise SystemExit(f"{OUT_SICON.name} is {len(data[OUT_SICON])} bytes "
                         f"(limit {SICON_MAX_BYTES})")
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
