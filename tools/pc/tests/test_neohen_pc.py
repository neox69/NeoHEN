"""Tests for the NeoHEN PC tool.

Every packed image is read back with a tool independent of the one that
wrote it whenever one is available:

- .ffpkg: tests/ufs2_reader.py (own UFS2 reader; makefs wrote the image).
- .exfat: fsck.exfat (exfatprogs) for structure, and a read-only exfat-fuse
  mount for contents when the test runs as root with loop devices.
- .ffpfsc/.ffpfs: MkPFS's own verify and unpack (no independent PFS reader
  exists; this is stated in the test names).

Run:  python3 -m unittest discover -s tools/pc/tests -v
"""
from __future__ import annotations

import filecmp
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from neohen_pc import cli  # noqa: E402
from neohen_pc.formats import (PackError, pack, ufs_layout,  # noqa: E402
                               ufs_mtree_spec)
from neohen_pc.gamedir import check_game_dir  # noqa: E402
from ufs2_reader import UFS2  # noqa: E402

try:
    import mkpfs  # noqa: F401
    HAVE_MKPFS = True
except ImportError:
    HAVE_MKPFS = False
HAVE_MAKEFS = shutil.which("makefs") is not None
HAVE_FSCK_EXFAT = shutil.which("fsck.exfat") is not None


def make_game(root: Path, title_id: str = "PPSA99999", ascii_only=False,
              name: str = "Prueba ñandú") -> Path:
    """A small but structurally real game folder (deterministic bytes)."""
    (root / "sce_sys").mkdir(parents=True)
    param = {
        "titleId": title_id,
        "contentId": f"UP0000-{title_id}_00-TEST000000000000",
        "contentVersion": "01.000.000",
        "applicationDrmType": "standard",
        "localizedParameters": {"defaultLanguage": "es-ES",
                                "es-ES": {"titleName": name}},
    }
    (root / "sce_sys" / "param.json").write_text(
        json.dumps(param, ensure_ascii=False), encoding="utf-8")
    # 3 MiB eboot: more than the 12 direct UFS blocks (768 KiB at 64 KiB),
    # so the UFS reader exercises the single-indirect path.
    (root / "eboot.bin").write_bytes(bytes(range(256)) * (3 * 4096))
    (root / "sce_module").mkdir()
    (root / "sce_module" / "libc.prx").write_bytes(b"\x7fELF" + b"\0" * 5000)
    (root / "empty.bin").write_bytes(b"")
    if ascii_only:
        (root / "data").mkdir()
        (root / "data" / "level1.pak").write_bytes(b"pak" * 70000)
    else:
        (root / "Música").mkdir()
        (root / "Música" / "canción 日本.txt").write_text("hola\n",
                                                        encoding="utf-8")
        (root / "data").mkdir()
        (root / "data" / "level1.pak").write_bytes(b"pak" * 70000)
    return root


def tree_files(root: Path) -> dict[str, bytes]:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = p.read_bytes()
    return out


class TempDirCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()


class CheckTests(TempDirCase):
    def test_valid_game(self):
        src = make_game(self.tmp / "g")
        r = check_game_dir(src)
        self.assertTrue(r.ok, [i.to_dict() for i in r.issues])
        self.assertEqual(r.title_id, "PPSA99999")
        self.assertEqual(r.title_name, "Prueba ñandú")
        self.assertEqual(r.content_version, "01.000.000")
        self.assertEqual(r.files, 6)

    def codes(self, root: Path, fmt=None) -> set[str]:
        return {i.code for i in check_game_dir(root, fmt).issues}

    def test_missing_eboot(self):
        src = make_game(self.tmp / "g")
        (src / "eboot.bin").unlink()
        self.assertIn("eboot_missing", self.codes(src))

    def test_extra_folder_level(self):
        make_game(self.tmp / "outer" / "PPSA99999-app")
        self.assertIn("extra_folder_level", self.codes(self.tmp / "outer"))

    def test_missing_param(self):
        src = make_game(self.tmp / "g")
        (src / "sce_sys" / "param.json").unlink()
        self.assertIn("param_missing", self.codes(src))

    def test_param_not_utf8(self):
        src = make_game(self.tmp / "g")
        (src / "sce_sys" / "param.json").write_bytes(
            '{"titleId":"PPSA99999","x":"ñ"}'.encode("cp1252"))
        self.assertIn("param_not_utf8", self.codes(src))

    def test_param_with_bom_is_accepted(self):
        src = make_game(self.tmp / "g")
        p = src / "sce_sys" / "param.json"
        p.write_bytes(b"\xef\xbb\xbf" + p.read_bytes())
        self.assertTrue(check_game_dir(src).ok)

    def test_unusual_title_id_warns(self):
        src = make_game(self.tmp / "g", title_id="ABCD12345")
        r = check_game_dir(src)
        self.assertTrue(r.ok)
        self.assertIn("title_id_unusual", {i.code for i in r.issues})

    @unittest.skipIf(os.name == "nt", "case-sensitive file system needed")
    def test_case_collision(self):
        src = make_game(self.tmp / "g")
        (src / "data" / "LEVEL1.PAK").write_bytes(b"x")
        self.assertIn("case_collision", self.codes(src))

    @unittest.skipIf(os.name == "nt", "name not creatable on Windows")
    def test_forbidden_characters(self):
        src = make_game(self.tmp / "g")
        (src / "data" / "bad:name?.bin").write_bytes(b"x")
        self.assertIn("name_forbidden_chars", self.codes(src))

    @unittest.skipIf(sys.platform == "darwin", "macOS normalises names")
    def test_decomposed_unicode_name_warns(self):
        src = make_game(self.tmp / "g")
        (src / "data" / "canción.txt").write_bytes(b"x")
        self.assertIn("name_not_nfc", self.codes(src))

    @unittest.skipIf(os.name == "nt", "symlinks need privileges on Windows")
    def test_symlink_rejected(self):
        src = make_game(self.tmp / "g")
        (src / "link.bin").symlink_to(src / "eboot.bin")
        self.assertIn("symlink", self.codes(src))

    def test_pfs_needs_ascii_names(self):
        src = make_game(self.tmp / "g")
        self.assertIn("pfs_non_ascii", self.codes(src, "ffpfs"))
        self.assertNotIn("pfs_non_ascii", self.codes(src, "ffpfsc"))

    def test_cli_json_is_utf8(self):
        src = make_game(self.tmp / "g")
        proc = subprocess.run(
            [sys.executable, "-m", "neohen_pc", "check", "--json", str(src)],
            cwd=HERE.parent, stdout=subprocess.PIPE, check=True,
            env={**os.environ, "PYTHONIOENCODING": "ascii"})
        data = json.loads(proc.stdout.decode("utf-8"))
        self.assertEqual(data["title_name"], "Prueba ñandú")


class UfsLayoutTests(TempDirCase):
    def test_size_rounded_and_has_slack(self):
        src = make_game(self.tmp / "g")
        size, per_inode = ufs_layout(src)
        self.assertEqual(size % (1024 * 1024), 0)
        raw = sum(len(b) for b in tree_files(src).values())
        self.assertGreaterEqual(size, raw + 64 * 1024 * 1024)
        self.assertEqual(per_inode % 4096, 0)
        self.assertGreaterEqual(per_inode, 65536)

    def test_mtree_escapes_utf8(self):
        src = make_game(self.tmp / "g")
        spec = ufs_mtree_spec(src)
        self.assertIn("M\\303\\272sica type=dir", spec)
        self.assertIn("\\040", spec)  # space in "canción 日本.txt"
        self.assertTrue(spec.isascii())


class PackTests(TempDirCase):
    def test_extension_must_match(self):
        src = make_game(self.tmp / "g")
        with self.assertRaises(PackError):
            pack("exfat", src, self.tmp / "out.img")

    def test_refuses_to_overwrite(self):
        src = make_game(self.tmp / "g")
        out = self.tmp / "x.ffpkg"
        out.write_bytes(b"keep")
        with self.assertRaises(PackError):
            pack("ffpkg", src, out)
        self.assertEqual(out.read_bytes(), b"keep")

    @unittest.skipUnless(HAVE_MAKEFS, "makefs not installed")
    def test_ffpkg_readback_with_independent_ufs2_reader(self):
        src = make_game(self.tmp / "g")
        out = self.tmp / "PPSA99999.ffpkg"
        res = pack("ffpkg", src, out)
        self.assertEqual(res.size % (1024 * 1024), 0)
        self.assertFalse(list(self.tmp.glob("*.partial*")))
        fs = UFS2(str(out))
        try:
            self.assertEqual(fs.bsize, 65536)
            self.assertEqual(fs.fsize, 65536)
            entries = fs.walk()
            expected = tree_files(src)
            got_files = {k: fs.data(v) for k, v in entries.items()
                         if v.is_file}
            self.assertEqual(got_files, expected)
            for rel, ino in entries.items():
                self.assertEqual(ino.mode & 0o7777, 0o777, rel)
                self.assertEqual((ino.uid, ino.gid), (0, 0), rel)
        finally:
            fs.close()

    @unittest.skipUnless(HAVE_MKPFS, "mkpfs not installed")
    def test_exfat_structure_and_cluster(self):
        src = make_game(self.tmp / "g")
        out = self.tmp / "PPSA99999.exfat"
        res = pack("exfat", src, out)
        if HAVE_FSCK_EXFAT:
            self.assertIn("fsck.exfat", res.verified)
        boot = out.read_bytes()[:512]
        self.assertEqual(boot[3:11], b"EXFAT   ")
        bytes_per_sector = 1 << boot[108]
        sectors_per_cluster = 1 << boot[109]
        self.assertEqual(bytes_per_sector * sectors_per_cluster, 65536)
        self.assertEqual(res.size % 65536, 0)

    @unittest.skipUnless(HAVE_MKPFS, "mkpfs not installed")
    def test_exfat_contents_via_kernel_independent_mount(self):
        mount = shutil.which("mount.exfat-fuse")
        if not (mount and hasattr(os, "geteuid") and os.geteuid() == 0
                and shutil.which("losetup")):
            self.skipTest("needs root, losetup and exfat-fuse")
        src = make_game(self.tmp / "g")
        out = self.tmp / "PPSA99999.exfat"
        pack("exfat", src, out)
        mnt = self.tmp / "mnt"
        mnt.mkdir()
        loop = subprocess.run(["losetup", "-f", "--show", "-r", str(out)],
                              stdout=subprocess.PIPE, text=True)
        if loop.returncode != 0:
            self.skipTest("no free loop device")
        dev = loop.stdout.strip()
        try:
            subprocess.run([mount, "-o", "ro", dev, str(mnt)], check=True)
            try:
                self.assertEqual(tree_files(mnt), tree_files(src))
            finally:
                subprocess.run(["fusermount3", "-u", str(mnt)])
        finally:
            subprocess.run(["losetup", "-d", dev])

    def _mkpfs_unpack_and_compare(self, image: Path, src: Path) -> None:
        dest = self.tmp / "unpacked"
        subprocess.run([sys.executable, "-m", "mkpfs", "unpack", "--deep",
                        str(image), str(dest)], check=True,
                       stdout=subprocess.DEVNULL)
        roots = [p.parent for p in dest.rglob("eboot.bin")]
        self.assertEqual(len(roots), 1, roots)
        self.assertEqual(tree_files(roots[0]), tree_files(src))

    @unittest.skipUnless(HAVE_MKPFS, "mkpfs not installed")
    def test_ffpfsc_roundtrip_with_mkpfs_reader(self):
        src = make_game(self.tmp / "g")
        out = self.tmp / "PPSA99999.ffpfsc"
        res = pack("ffpfsc", src, out)
        self.assertIn("mkpfs verify", res.verified)
        self._mkpfs_unpack_and_compare(out, src)

    @unittest.skipUnless(HAVE_MKPFS, "mkpfs not installed")
    def test_ffpfs_roundtrip_with_mkpfs_reader(self):
        src = make_game(self.tmp / "g", ascii_only=True)
        out = self.tmp / "PPSA99999.ffpfs"
        pack("ffpfs", src, out)
        self._mkpfs_unpack_and_compare(out, src)

    def test_cli_refuses_bad_folder(self):
        src = make_game(self.tmp / "g")
        (src / "eboot.bin").unlink()
        rc = cli.main(["pack", str(src), str(self.tmp / "x.exfat")])
        self.assertEqual(rc, 1)
        self.assertFalse((self.tmp / "x.exfat").exists())


if __name__ == "__main__":
    unittest.main()
