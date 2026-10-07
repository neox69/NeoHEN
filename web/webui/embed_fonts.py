#!/usr/bin/env python3
"""Inline webui/fonts/*.woff2 into index.html, between the FONTS markers.

The page is one file linked into the payload, so its fonts travel inside it
and it looks the same with no Internet. Run after changing a font file.
Big Shoulders Display, Archivo and JetBrains Mono are under the SIL Open
Font License 1.1.
"""
import base64
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
FACES = (("Big Shoulders Display", "700 900", "big-shoulders-display"),
         ("Archivo", "400 600", "archivo"),
         ("JetBrains Mono", "400 500", "jetbrains-mono"))

css = "".join(
    '@font-face{font-family:"%s";font-style:normal;font-weight:%s;'
    'font-display:swap;src:url(data:font/woff2;base64,%s) format("woff2")}\n'
    % (name, weight,
       base64.b64encode((HERE / "fonts" / (f + ".woff2")).read_bytes()).decode())
    for name, weight, f in FACES)
page = HERE / "index.html"
text = page.read_text(encoding="utf-8")
text, n = re.subn(r"/\*FONTS\*/.*?/\*END FONTS\*/",
                  lambda m: "/*FONTS*/\n" + css + "/*END FONTS*/", text, flags=re.S)
if n != 1:
    raise SystemExit("FONTS markers not found in index.html")
page.write_bytes(text.encode("utf-8"))
print("index.html: %d bytes" % len(text.encode("utf-8")))
