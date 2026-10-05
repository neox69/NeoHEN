"""Validate a PS5/PS4 game folder before it is packed into an image.

Every check here protects against a failure that would otherwise only show
up on the console: a missing eboot, an extra folder level, names that the
target file system cannot store, or two names that collide once the file
system ignores case.
"""
from __future__ import annotations

import json
import os
import re
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

TITLE_ID_RE = re.compile(r"^(PPSA|CUSA|LAPY|FAKE)\d{5}$")
# Characters exFAT rejects in a file name, besides control characters.
EXFAT_FORBIDDEN = set('"*/:<>?\\|')
EXFAT_MAX_NAME_UTF16 = 255
PARAM_JSON_MAX = 4 * 1024 * 1024


@dataclass
class Issue:
    level: str  # "error" or "warning"
    code: str
    message: str
    path: str = ""

    def to_dict(self) -> dict:
        d = {"level": self.level, "code": self.code, "message": self.message}
        if self.path:
            d["path"] = self.path
        return d


@dataclass
class GameReport:
    root: Path
    title_id: str = ""
    title_name: str = ""
    content_id: str = ""
    content_version: str = ""
    platform: str = ""
    files: int = 0
    dirs: int = 0
    total_bytes: int = 0
    largest_file_bytes: int = 0
    issues: list[Issue] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not any(i.level == "error" for i in self.issues)

    def error(self, code: str, message: str, path: str = "") -> None:
        self.issues.append(Issue("error", code, message, path))

    def warn(self, code: str, message: str, path: str = "") -> None:
        self.issues.append(Issue("warning", code, message, path))

    def to_dict(self) -> dict:
        return {
            "root": str(self.root),
            "ok": self.ok,
            "title_id": self.title_id,
            "title_name": self.title_name,
            "content_id": self.content_id,
            "content_version": self.content_version,
            "platform": self.platform,
            "files": self.files,
            "dirs": self.dirs,
            "total_bytes": self.total_bytes,
            "largest_file_bytes": self.largest_file_bytes,
            "issues": [i.to_dict() for i in self.issues],
        }


def _rel(root: Path, p: Path) -> str:
    return p.relative_to(root).as_posix() or "."


def _check_name(report: GameReport, root: Path, path: Path) -> None:
    name = path.name
    rel = _rel(root, path)
    if any(ord(c) < 0x20 for c in name) or EXFAT_FORBIDDEN & set(name):
        report.error("name_forbidden_chars",
                     "name contains characters exFAT cannot store", rel)
    if len(name.encode("utf-16-le")) // 2 > EXFAT_MAX_NAME_UTF16:
        report.error("name_too_long",
                     f"name longer than {EXFAT_MAX_NAME_UTF16} UTF-16 units",
                     rel)
    if name != name.rstrip(" ."):
        report.warn("name_trailing_dot_space",
                    "name ends with a space or dot (Windows strips them)", rel)
    if unicodedata.normalize("NFC", name) != name:
        report.warn("name_not_nfc",
                    "name is not Unicode NFC (macOS decomposed form); the "
                    "console will see different bytes than the game expects",
                    rel)


def _read_param(report: GameReport, root: Path) -> None:
    param = root / "sce_sys" / "param.json"
    sfo = root / "sce_sys" / "param.sfo"
    if not param.is_file():
        if sfo.is_file():
            report.platform = "PS4"
            report.warn("param_sfo_only",
                        "only sce_sys/param.sfo found (PS4 title); title ID "
                        "is taken from the folder name if possible")
            m = re.search(r"(CUSA\d{5})", root.name.upper())
            if m:
                report.title_id = m.group(1)
            return
        report.error("param_missing", "sce_sys/param.json not found")
        return
    report.platform = "PS5"
    if param.stat().st_size > PARAM_JSON_MAX:
        report.error("param_too_big", "sce_sys/param.json is too large")
        return
    raw = param.read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        report.error("param_not_utf8", "sce_sys/param.json is not UTF-8")
        return
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        report.error("param_bad_json", f"sce_sys/param.json: {exc}")
        return
    if not isinstance(data, dict):
        report.error("param_bad_json", "sce_sys/param.json is not an object")
        return
    report.title_id = str(data.get("titleId", "")).strip().upper()
    report.content_id = str(data.get("contentId", "")).strip()
    report.content_version = str(data.get("contentVersion", "")).strip()
    loc = data.get("localizedParameters")
    if isinstance(loc, dict):
        lang = loc.get("defaultLanguage")
        entry = loc.get(lang) if isinstance(lang, str) else None
        if not isinstance(entry, dict):
            entry = next((v for v in loc.values() if isinstance(v, dict)), {})
        report.title_name = str(entry.get("titleName", "")).strip()
    if not report.title_id:
        report.error("title_id_missing", "param.json has no titleId")


def check_game_dir(root: str | os.PathLike,
                   fmt: str | None = None) -> GameReport:
    """Check a game folder; fmt adds the limits of one image format."""
    root = Path(root)
    report = GameReport(root=root)
    if not root.is_dir():
        report.error("not_a_dir", "source is not a folder")
        return report

    if not (root / "eboot.bin").is_file():
        nested = [p for p in root.iterdir()
                  if p.is_dir() and (p / "eboot.bin").is_file()]
        if nested:
            report.error("extra_folder_level",
                         "eboot.bin is one folder too deep; use "
                         f"'{nested[0].name}' as the source folder",
                         nested[0].name)
        else:
            report.error("eboot_missing", "eboot.bin not found at the root")

    _read_param(report, root)
    if report.title_id and not TITLE_ID_RE.match(report.title_id):
        report.warn("title_id_unusual",
                    f"title ID '{report.title_id}' does not match "
                    "PPSA/CUSA/LAPY/FAKE + 5 digits; ShadowMount+ skips "
                    "unknown prefixes for sandbox mounts")

    # Walk without following symlinks; exFAT/UFS images store real files only.
    folded: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dpath = Path(dirpath)
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            p = dpath / name
            rel = _rel(root, p)
            _check_name(report, root, p)
            if fmt == "ffpfs" and not name.isascii():
                report.error("pfs_non_ascii",
                             "uncompressed PFS (.ffpfs) only stores ASCII "
                             "names; use .ffpfsc or .exfat", rel)
            key = unicodedata.normalize("NFC", rel).casefold()
            if key in folded and folded[key] != rel:
                report.error("case_collision",
                             f"'{rel}' and '{folded[key]}' differ only in case "
                             "or Unicode form; exFAT keeps one of them", rel)
            folded.setdefault(key, rel)
            if p.is_symlink():
                report.error("symlink", "symbolic links are not supported",
                             rel)
        for name in dirnames:
            if not (dpath / name).is_symlink():
                report.dirs += 1
        for name in filenames:
            p = dpath / name
            if p.is_symlink() or not p.is_file():
                continue
            size = p.stat().st_size
            report.files += 1
            report.total_bytes += size
            report.largest_file_bytes = max(report.largest_file_bytes, size)
    return report
