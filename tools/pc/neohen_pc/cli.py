"""neohen-pc: check a game folder and pack it for ShadowMount+.

  python -m neohen_pc check  <game_dir> [--json]
  python -m neohen_pc pack   <game_dir> <output.{exfat,ffpfsc,ffpfs,ffpkg}>
  python -m neohen_pc formats

Output is always UTF-8, whatever the console code page.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import __version__
from .formats import FORMATS, PackError, pack
from .gamedir import check_game_dir

# Status as documented by ShadowMount+ 1.7 (README, "Current image support").
# Console evidence comes from the NeoHEN user's own tests and is listed per
# firmware; nothing else is claimed.
FORMAT_INFO = {
    "exfat": ("exFAT, 64 KiB clusters", "compatibility",
              "for titles that need external-drive behaviour"),
    "ffpfsc": ("compressed PFS around exFAT", "experimental",
               "smallest; decompression ~150-250 MB/s, may stutter"),
    "ffpfs": ("uncompressed PFS", "experimental", ""),
    "ffpkg": ("UFS2", "recommended", "needs makefs (Linux/macOS/FreeBSD)"),
}
CONSOLE_EVIDENCE = {
    "ffpfsc": ["13.42: Syberia Remastered (PPSA30140) runs with "
               "ShadowMount+ 1.7beta2"],
}


def _force_utf8() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def _human(n: int) -> str:
    size = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if size < 1024 or unit == "TiB":
            return f"{size:.1f} {unit}" if unit != "B" else f"{n} B"
        size /= 1024
    return f"{n} B"


def cmd_check(args: argparse.Namespace) -> int:
    report = check_game_dir(args.game_dir, args.format)
    if args.json:
        print(json.dumps(report.to_dict(), ensure_ascii=False, indent=2))
        return 0 if report.ok else 1
    print(f"Folder:   {report.root}")
    print(f"Title ID: {report.title_id or '-'}   {report.title_name}")
    if report.content_version:
        print(f"Version:  {report.content_version}")
    print(f"Content:  {report.files} files, {report.dirs} folders, "
          f"{_human(report.total_bytes)}")
    for issue in report.issues:
        where = f" [{issue.path}]" if issue.path else ""
        print(f"{issue.level.upper()}: {issue.message}{where}")
    print("OK" if report.ok else "NOT READY")
    return 0 if report.ok else 1


def cmd_pack(args: argparse.Namespace) -> int:
    src, out = Path(args.game_dir), Path(args.output)
    fmt = out.suffix.lower().lstrip(".")
    if fmt not in FORMATS:
        print(f"ERROR: output extension must be one of: "
              f"{', '.join('.' + f for f in FORMATS)}", file=sys.stderr)
        return 2
    report = check_game_dir(src, fmt)
    for issue in report.issues:
        where = f" [{issue.path}]" if issue.path else ""
        print(f"{issue.level.upper()}: {issue.message}{where}")
    if not report.ok and not args.force:
        print("NOT PACKED: fix the errors above (or use --force)",
              file=sys.stderr)
        return 1
    print(f"Packing {report.title_id or src.name} as .{fmt} ...")
    try:
        result = pack(fmt, src, out, overwrite=args.overwrite)
    except PackError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"Written: {result.output} ({_human(result.size)}) via "
          f"{result.tool}")
    if result.verified:
        print(f"Verified with: {', '.join(result.verified)}")
    print("Console status: not verified by this tool; see "
          "'neohen_pc formats'.")
    return 0


def cmd_formats(_args: argparse.Namespace) -> int:
    for fmt, (fs, status, note) in FORMAT_INFO.items():
        line = f".{fmt:<7} {fs:<30} {status}"
        print(line + (f" - {note}" if note else ""))
        for ev in CONSOLE_EVIDENCE.get(fmt, []):
            print(f"         tested: {ev}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="neohen_pc",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("-V", "--version", action="version",
                    version=f"neohen_pc {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="validate a game folder")
    c.add_argument("game_dir")
    c.add_argument("--json", action="store_true")
    c.add_argument("--format", choices=FORMATS,
                   help="also check the limits of this image format")
    c.set_defaults(func=cmd_check)
    p = sub.add_parser("pack", help="pack a game folder into an image")
    p.add_argument("game_dir")
    p.add_argument("output")
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="pack even if the folder check reports errors")
    p.set_defaults(func=cmd_pack)
    f = sub.add_parser("formats", help="list formats and their status")
    f.set_defaults(func=cmd_formats)
    return ap


def main(argv: list[str] | None = None) -> int:
    _force_utf8()
    args = build_parser().parse_args(argv)
    return args.func(args)
