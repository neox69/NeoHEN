"""Minimal read-only UFS2 reader used as an independent test oracle.

Implements only what the tests need: superblock, inodes, directories and
file data through direct, single and double indirect blocks. Layout from
FreeBSD sys/ufs/ffs/fs.h and sys/ufs/ufs/dinode.h.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

SBLOCK_UFS2 = 65536
FS_UFS2_MAGIC = 0x19540119
UFS_ROOTINO = 2
NDADDR = 12
DINODE2_SIZE = 256
S_IFMT = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000


@dataclass
class Inode:
    number: int
    mode: int
    uid: int
    gid: int
    size: int
    direct: tuple[int, ...]
    indirect: tuple[int, ...]

    @property
    def is_dir(self) -> bool:
        return self.mode & S_IFMT == S_IFDIR

    @property
    def is_file(self) -> bool:
        return self.mode & S_IFMT == S_IFREG


class UFS2:
    def __init__(self, path: str) -> None:
        self.f = open(path, "rb")
        sb = self._read(SBLOCK_UFS2, 1376)
        magic = struct.unpack_from("<i", sb, 1372)[0] & 0xFFFFFFFF
        if magic != FS_UFS2_MAGIC:
            raise ValueError(f"not UFS2 (magic {magic:#x})")
        (self.iblkno,) = struct.unpack_from("<i", sb, 16)
        (self.bsize, self.fsize, self.frag) = struct.unpack_from("<iii", sb,
                                                                 48)
        (self.fragshift,) = struct.unpack_from("<i", sb, 96)
        (self.inopb,) = struct.unpack_from("<I", sb, 120)
        (self.ipg, self.fpg) = struct.unpack_from("<Ii", sb, 184)
        self.nindir = self.bsize // 8

    def close(self) -> None:
        self.f.close()

    def _read(self, offset: int, size: int) -> bytes:
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise ValueError("short read")
        return data

    def inode(self, ino: int) -> Inode:
        cg = ino // self.ipg
        frag = self.fpg * cg + self.iblkno \
            + (((ino % self.ipg) // self.inopb) << self.fragshift)
        off = frag * self.fsize + (ino % self.inopb) * DINODE2_SIZE
        raw = self._read(off, DINODE2_SIZE)
        mode, _nlink, uid, gid = struct.unpack_from("<HhII", raw, 0)
        (size,) = struct.unpack_from("<Q", raw, 16)
        direct = struct.unpack_from("<12q", raw, 112)
        indirect = struct.unpack_from("<3q", raw, 208)
        return Inode(ino, mode, uid, gid, size, direct, indirect)

    def _block_list(self, ind: int, level: int) -> list[int]:
        if ind == 0:
            return []
        ptrs = struct.unpack_from(f"<{self.nindir}q",
                                  self._read(ind * self.fsize, self.bsize))
        if level == 0:
            return list(ptrs)
        out: list[int] = []
        for p in ptrs:
            out.extend(self._block_list(p, level - 1))
        return out

    def data(self, ino: Inode) -> bytes:
        blocks = list(ino.direct)
        for level, ind in enumerate(ino.indirect[:2]):
            blocks.extend(self._block_list(ind, level))
        out = bytearray()
        remaining = ino.size
        for b in blocks:
            if remaining <= 0:
                break
            n = min(self.bsize, remaining)
            out += self._read(b * self.fsize, n) if b else bytes(n)
            remaining -= n
        if remaining > 0:
            raise ValueError("file larger than the supported indirect depth")
        return bytes(out)

    def listdir(self, ino: Inode) -> dict[str, int]:
        raw = self.data(ino)
        entries: dict[str, int] = {}
        pos = 0
        while pos + 8 <= len(raw):
            d_ino, reclen, _dtype, namlen = struct.unpack_from("<IHBB", raw,
                                                               pos)
            if reclen == 0:
                break
            if d_ino:
                name = raw[pos + 8:pos + 8 + namlen].decode("utf-8")
                if name not in (".", ".."):
                    entries[name] = d_ino
            pos += reclen
        return entries

    def walk(self) -> dict[str, Inode]:
        """Map of relative path -> inode for every entry below the root."""
        result: dict[str, Inode] = {}

        def rec(ino: Inode, prefix: str) -> None:
            for name, num in sorted(self.listdir(ino).items()):
                child = self.inode(num)
                rel = f"{prefix}{name}"
                result[rel] = child
                if child.is_dir:
                    rec(child, rel + "/")

        rec(self.inode(UFS_ROOTINO), "")
        return result
