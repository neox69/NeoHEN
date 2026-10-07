#!/usr/bin/env python3
"""Read a PS5 .rco resource bundle (RCOF, a CXML container), or swap files in it.

  python3 themes/tools/rco.py list <file.rco>            what is inside
  python3 themes/tools/rco.py extract <file.rco> <dir>   every file, unpacked
  python3 themes/tools/rco.py replace <file.rco> <out.rco> <id>=<file> ...
                                                         a copy with new files

replace takes the console's own bundle and gives a copy in which the named
elements (snd_focus_move=move.vag, iconid_app_explore=explore.svg, or
snd_enter=@snd_1_enter to reuse another element's file) point at new data.
Nothing in the bundle moves: the new data is added after the last file and
only the element's (offset, size) and the table's size change, so every
other byte is the original's. The copy only fits the firmware it came from.

An .rco is a tree of elements (texturetable/texture, filetable/file,
stringtable/...) whose attributes point into shared tables; the payload of a
texture or file lives in the file table, zlib-compressed when the element
says compress="on".
"""
import os
import struct
import sys
import zlib

TYPES = {1: "int", 2: "float", 3: "string", 4: "wstring", 5: "hash",
         6: "intarray", 7: "floatarray", 8: "file", 9: "id", 10: "idref"}


class Rco:
    def __init__(self, path):
        self.data = d = open(path, "rb").read()
        if d[:4] != b"RCOF":
            raise SystemExit("not an RCOF file")
        h = struct.unpack("<18I", d[8:80])
        (self.tree_off, self.tree_size, self.id_off, self.id_size,
         self.hash_off, self.hash_size, self.str_off, self.str_size,
         self.wstr_off, self.wstr_size, self.hashtab_off, self.hashtab_size,
         self.int_off, self.int_size, self.float_off, self.float_size,
         self.file_off, self.file_size) = h

    def cstr(self, off):
        a = self.str_off + off
        return self.data[a:self.data.index(b"\0", a)].decode("utf-8", "replace")

    def ident(self, off):
        # id table entry: int32 back-reference to the element, then the name
        a = self.id_off + off + 4
        return self.data[a:self.data.index(b"\0", a)].decode("utf-8", "replace")

    def element(self, off):
        base = self.tree_off + off
        name, nattr, parent, prev, nxt, first, last = struct.unpack(
            "<7i", self.data[base:base + 28])
        attrs = {}
        for i in range(nattr):
            a = base + 28 + i * 16
            aname, atype, v1, v2 = struct.unpack("<4i", self.data[a:a + 16])
            key = self.cstr(aname)
            if atype == 1:
                val = v1
            elif atype == 2:
                val = struct.unpack("<f", struct.pack("<i", v1))[0]
            elif atype == 3:
                val = self.cstr(v1)
            elif atype == 8:
                val = ("file", v1, v2)
            elif atype in (9, 10):
                val = self.ident(v1)
            else:
                val = (TYPES.get(atype, atype), v1, v2)
            attrs[key] = val
        return self.cstr(name), attrs, first, nxt

    def file_refs(self, ident, off=0):
        """Where the (offset, size) of each file attribute of an element is
        kept in the bundle: {attribute: position}. {} if there is none."""
        while off != -1:
            base = self.tree_off + off
            name, nattr, parent, prev, nxt, first, last = struct.unpack(
                "<7i", self.data[base:base + 28])
            found, mine = {}, False
            for i in range(nattr):
                a = base + 28 + i * 16
                aname, atype, v1, v2 = struct.unpack("<4i", self.data[a:a + 16])
                if atype == 8:
                    found[self.cstr(aname)] = a + 8
                elif atype in (9, 10) and self.cstr(aname) == "id":
                    mine = self.ident(v1) == ident
            if mine and found:
                return found
            if first != -1:
                deeper = self.file_refs(ident, first)
                if deeper:
                    return deeper
            off = nxt
        return {}

    def replaced(self, swaps):
        """A copy of the bundle with new data for {id: bytes}."""
        if self.file_off + self.file_size != len(self.data):
            raise SystemExit("the file table is not the last thing in this bundle")
        out = bytearray(self.data)
        for ident, blob in swaps.items():
            refs = self.file_refs(ident)
            if "src" not in refs:
                raise SystemExit(f"no element with a file is called {ident}")
            out += b"\0" * (-len(out) % 16)
            struct.pack_into("<2i", out, refs["src"], len(out) - self.file_off, len(blob))
            out += blob
        out += b"\0" * (-len(out) % 16)
        # the table's size is the last of the header's eighteen numbers
        struct.pack_into("<I", out, 8 + 17 * 4, len(out) - self.file_off)
        return bytes(out)

    def walk(self, off=0, depth=0):
        while off != -1:
            name, attrs, first, nxt = self.element(off)
            yield depth, name, attrs
            if first != -1:
                yield from self.walk(first, depth + 1)
            off = nxt

    def payload(self, attrs):
        """The unpacked bytes of an element with a file attribute, or None."""
        ref = next((v for v in attrs.values()
                    if isinstance(v, tuple) and v[0] == "file"), None)
        if not ref:
            return None
        raw = self.data[self.file_off + ref[1]:self.file_off + ref[1] + ref[2]]
        if attrs.get("compress") == "on":
            raw = zlib.decompress(raw)
        return raw


def kind(blob):
    for magic, ext in ((b"\x89PNG", "png"), (b"DDS ", "dds"), (b"RIFF", "at9"),
                       (b"<svg", "svg"), (b"<?xml", "xml"), (b"GNF ", "gnf"),
                       (b"\xff\xd8", "jpg"), (b"OTTO", "otf"), (b"VAGp", "vag")):
        if blob[:len(magic)] == magic:
            return ext
    return "bin"


def replace(rco, out, pairs):
    files = {a.get("id"): rco.payload(a) for _, _, a in rco.walk() if rco.payload(a) is not None}
    swaps = {}
    for pair in pairs:
        ident, _, source = pair.partition("=")
        if source.startswith("@"):
            if source[1:] not in files:
                raise SystemExit(f"nothing is called {source[1:]}")
            blob = files[source[1:]]
        else:
            blob = open(source, "rb").read()
        if ident not in files:
            raise SystemExit(f"nothing is called {ident}")
        if kind(blob) != kind(files[ident]):
            raise SystemExit(f"{ident} is a {kind(files[ident])} file, {source} is not")
        swaps[ident] = blob
    data = rco.replaced(swaps)
    with open(out, "wb") as f:
        f.write(data)
    # read it back: every element must give what was asked, or the original
    check = Rco(out)
    after = {a.get("id"): check.payload(a) for _, _, a in check.walk()
             if check.payload(a) is not None}
    if set(after) != set(files):
        raise SystemExit("the copy does not hold the same elements")
    for ident, blob in files.items():
        if after[ident] != swaps.get(ident, blob):
            raise SystemExit(f"{ident} does not read back right")
    print(f"{len(swaps)} replaced, {len(files)} checked -> {out}")
    return 0


def main():
    if len(sys.argv) >= 5 and sys.argv[1] == "replace":
        return replace(Rco(sys.argv[2]), sys.argv[3], sys.argv[4:])
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        print(__doc__, file=sys.stderr)
        return 2
    rco = Rco(sys.argv[2])
    out = sys.argv[3] if sys.argv[1] == "extract" else None
    count = {}
    for depth, name, attrs in rco.walk():
        blob = rco.payload(attrs)
        if blob is None:
            continue
        ident = attrs.get("id", "?")
        ext = kind(blob)
        count[(name, ext)] = count.get((name, ext), 0) + 1
        if out:
            os.makedirs(os.path.join(out, name), exist_ok=True)
            safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in str(ident))
            with open(os.path.join(out, name, safe + "." + ext), "wb") as f:
                f.write(blob)
        else:
            print(f"{name:10} {ext:4} {len(blob):9} {ident}")
    print("\n".join(f"# {n} x {k[0]} ({k[1]})" for k, n in sorted(count.items())),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
