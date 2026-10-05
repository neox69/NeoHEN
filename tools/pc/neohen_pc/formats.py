"""Build ShadowMount+ game images from a game folder.

| Format  | File system           | Built with                         |
|---------|-----------------------|------------------------------------|
| .exfat  | exFAT, 64 KiB cluster | MkPFS (`pack exfat`), pure Python  |
| .ffpfsc | compressed PFS around an exFAT image | MkPFS (`pack folder`) |
| .ffpfs  | uncompressed PFS      | MkPFS (`pack folder --raw`)        |
| .ffpkg  | UFS2, 64 KiB blocks, 4 KiB sectors | makefs (Linux/macOS/BSD) |

The UFS2 parameters follow ShadowMount+'s mkufs2.sh (GPLv3, drakmor):
block = fragment = 64 KiB, 4 KiB sectors, 0 % reserved space, every file
mode 0777 owned by root, size rounded up to 1 MiB.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

FORMATS = ("exfat", "ffpfsc", "ffpfs", "ffpkg")
MKPFS_VERSION = "1.1.0"

MIB = 1024 * 1024
UFS_BLOCK = 65536
UFS_SECTOR = 4096
UFS_INODE_SIZE = 256
UFS_INODE_SPARE = 2048
UFS_META_FIXED = 64 * MIB
UFS_MIN_SLACK = 64 * MIB
UFS_SPARE_MIN = 64 * MIB
UFS_SPARE_MAX = 512 * MIB


class PackError(RuntimeError):
    pass


@dataclass
class PackResult:
    fmt: str
    output: Path
    size: int
    tool: str
    verified: list[str]


def utf8_env() -> dict[str, str]:
    """Child processes inherit UTF-8 regardless of the Windows code page."""
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def _run(cmd: list[str], what: str) -> None:
    proc = subprocess.run(cmd, env=utf8_env(), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, encoding="utf-8",
                          errors="replace")
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.strip().splitlines()[-25:])
        raise PackError(f"{what} failed (exit {proc.returncode}):\n{tail}")


def mkpfs_cmd() -> list[str]:
    try:
        import mkpfs  # noqa: F401
    except ImportError as exc:
        raise PackError(
            f"MkPFS is not installed: python -m pip install mkpfs=="
            f"{MKPFS_VERSION}") from exc
    return [sys.executable, "-m", "mkpfs"]


# --------------------------------------------------------------------------
# UFS2 (.ffpkg)
# --------------------------------------------------------------------------

def _ceil_to(value: int, unit: int) -> int:
    return (value + unit - 1) // unit * unit


def ufs_layout(root: Path) -> tuple[int, int]:
    """Image size and bytes-per-inode, as computed by mkufs2.sh."""
    files = dirs = raw = data = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirs += 1
        for name in filenames:
            size = (Path(dirpath) / name).stat().st_size
            files += 1
            raw += size
            data += _ceil_to(size, UFS_BLOCK)
    inode_count = files + dirs + UFS_INODE_SPARE
    base = data + dirs * UFS_BLOCK + inode_count * UFS_INODE_SIZE \
        + UFS_META_FIXED
    spare = min(max(base // 200, UFS_SPARE_MIN), UFS_SPARE_MAX)
    total = max(base + spare, raw + UFS_MIN_SLACK)
    image = _ceil_to(total, MIB)
    per_inode = 4 * UFS_BLOCK
    auto = (image // inode_count) // UFS_SECTOR * UFS_SECTOR
    per_inode = min(per_inode, max(auto, UFS_BLOCK))
    return image, per_inode


def _mtree_escape(name: str) -> str:
    out = []
    for byte in name.encode("utf-8"):
        ch = chr(byte)
        if 0x21 <= byte < 0x7F and ch not in "\\#":
            out.append(ch)
        else:
            out.append("\\%03o" % byte)
    return "".join(out)


def ufs_mtree_spec(root: Path) -> str:
    """mtree spec that gives every entry mode 0777 and root ownership."""
    lines = ["#mtree", "/set uid=0 gid=0 mode=0777 flags=none",
             ". type=dir"]

    def walk(d: Path) -> None:
        for entry in sorted(os.scandir(d), key=lambda e: e.name):
            if entry.is_dir(follow_symlinks=False):
                lines.append(f"{_mtree_escape(entry.name)} type=dir")
                walk(Path(entry.path))
                lines.append("..")
            elif entry.is_file(follow_symlinks=False):
                lines.append(f"{_mtree_escape(entry.name)} type=file")

    walk(root)
    lines.append("..")
    return "\n".join(lines) + "\n"


def pack_ffpkg(src: Path, out: Path) -> PackResult:
    makefs = shutil.which("makefs")
    if not makefs:
        raise PackError(
            ".ffpkg needs makefs (Debian/Ubuntu: apt install makefs; macOS "
            "and FreeBSD ship it). On Windows use WSL, or choose .exfat / "
            ".ffpfsc, which need only Python.")
    size, per_inode = ufs_layout(src)
    with tempfile.TemporaryDirectory() as tmp:
        spec = Path(tmp) / "spec.mtree"
        spec.write_text(ufs_mtree_spec(src), encoding="utf-8")
        _run([makefs, "-t", "ffs", "-B", "le", "-S", str(UFS_SECTOR),
              "-o", f"version=2,bsize={UFS_BLOCK},fsize={UFS_BLOCK},"
                    f"minfree=0,density={per_inode}",
              "-s", str(size), "-F", str(spec), str(out), str(src)],
             "makefs")
    return PackResult("ffpkg", out, out.stat().st_size, "makefs", [])


# --------------------------------------------------------------------------
# MkPFS-backed formats
# --------------------------------------------------------------------------

def pack_exfat(src: Path, out: Path) -> PackResult:
    _run(mkpfs_cmd() + ["pack", "exfat", "--cluster-size", "65536",
                        "--overwrite", "--no-progress", str(src), str(out)],
         "mkpfs pack exfat")
    verified = []
    fsck = shutil.which("fsck.exfat")
    if fsck:
        _run([fsck, "-n", str(out)], "fsck.exfat")
        verified.append("fsck.exfat")
    return PackResult("exfat", out, out.stat().st_size, "mkpfs", verified)


def pack_ffpfsc(src: Path, out: Path) -> PackResult:
    _run(mkpfs_cmd() + ["pack", "folder", "--verify",
                        "--no-adjust-output-file-extension", "--version",
                        "PS5", "--inode-bits", "32", str(src), str(out)],
         "mkpfs pack folder")
    return PackResult("ffpfsc", out, out.stat().st_size, "mkpfs",
                      ["mkpfs verify"])


def pack_ffpfs(src: Path, out: Path) -> PackResult:
    _run(mkpfs_cmd() + ["pack", "folder", "--raw", "--no-compress",
                        "--verify", "--no-adjust-output-file-extension",
                        "--version", "PS5", "--inode-bits", "32", str(src),
                        str(out)],
         "mkpfs pack folder --raw")
    return PackResult("ffpfs", out, out.stat().st_size, "mkpfs",
                      ["mkpfs verify"])


PACKERS = {"exfat": pack_exfat, "ffpfsc": pack_ffpfsc, "ffpfs": pack_ffpfs,
           "ffpkg": pack_ffpkg}


def pack(fmt: str, src: Path, out: Path, overwrite: bool = False
         ) -> PackResult:
    if fmt not in PACKERS:
        raise PackError(f"unknown format '{fmt}'")
    if out.suffix.lower() != f".{fmt}":
        raise PackError(f"output must end in .{fmt} (ShadowMount+ picks the "
                        "file system from the extension)")
    if out.exists():
        if not overwrite:
            raise PackError(f"{out} exists (use --overwrite)")
        out.unlink()
    out.parent.mkdir(parents=True, exist_ok=True)
    # Build next to the target and rename at the end: an interrupted run
    # never leaves a half-written image under the final name.
    partial = out.with_name(out.name + ".partial" + out.suffix)
    if partial.exists():
        partial.unlink()
    try:
        result = PACKERS[fmt](src, partial)
        os.replace(partial, out)
    finally:
        if partial.exists():
            partial.unlink()
    result.output = out
    return result
