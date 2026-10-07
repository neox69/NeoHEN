#!/usr/bin/env python3
"""Pack the themes that come with NeoHEN into one file for neotheme.elf.

  python3 themes/tools/pack_themes.py <folder of themes> <out.pack>

Format: "NTPK", a number that changes when anything in the pack does, then
for each file its path's length (2 bytes), its size (4), the path
("Theme/...") and the bytes; a zero length ends it.
"""
import os
import struct
import sys
import zlib


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    root, out = sys.argv[1], sys.argv[2]
    body = bytearray()
    count = 0
    if os.path.isdir(root):
        for folder, dirs, files in os.walk(root):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(folder, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/").encode("utf-8")
                data = open(path, "rb").read()
                body += struct.pack("<HI", len(rel), len(data)) + rel + data
                count += 1
    body += struct.pack("<H", 0)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"NTPK" + struct.pack("<I", zlib.crc32(bytes(body))) + body)
    print(f"{count} files, {len(body) / 1048576:.1f} MB -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
