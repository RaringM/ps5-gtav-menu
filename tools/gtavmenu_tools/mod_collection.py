"""Bounded, key-free collection of loose mod resources, OPEN RPFs and OIV/ZIP packages."""

from __future__ import annotations

import mmap
import os
import stat
import zipfile
from pathlib import Path, PurePosixPath

import extract_mod_files
from gtavmenu_tools.asset_formats import AssetError, rpf_header

META_NAME = "handling.meta"
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_PACKAGE_MEMBERS = 10000


class CollectionError(ValueError):
    """The mod cannot be collected within the input contract."""


class UnreadableArchive(CollectionError):
    """An adjacent archive cannot be decoded; loose exported files may still be used."""


def shown(text: str) -> str:
    """Text for the terminal: member names come from the mod, so control characters (escape sequences that could
    rewrite earlier output or set the clipboard) are printed as \\x/\\u escapes; newlines stay."""
    return "".join(
        c if c == "\n" or " " <= c <= "~" else (f"\\x{ord(c):02x}" if ord(c) < 0x100 else f"\\u{ord(c):04x}")
        for c in text
    )


def _add(found: dict[str, bytes], origin: dict[str, str], name: str, blob: bytes, where: str) -> None:
    key = name.lower()
    if len(blob) > MAX_FILE_BYTES:
        raise CollectionError(f"{where}: larger than {MAX_FILE_BYTES} bytes")
    if key in found and found[key] != blob:
        raise CollectionError(f"{key} appears twice with different bytes ({origin[key]}, {where}); keep one")
    total = sum(map(len, found.values())) - len(found.get(key, b"")) + len(blob)
    if total > MAX_TOTAL_BYTES:
        raise CollectionError("the mod's matching files exceed the 1 GiB reader budget")
    found[key] = blob
    origin.setdefault(key, where)


def _from_rpf(
    view, where: str, found: dict[str, bytes], origin: dict[str, str], notes: list[str], suffixes: tuple[str, ...]
) -> None:
    """Resources and handling.meta of a PC archive (OPEN/NONE tables; NG-encrypted archives refuse)."""
    try:
        header = rpf_header(bytes(view[:16]), len(view))
    except AssetError as error:
        raise CollectionError(f"{where}: {error}") from error
    if not header["tableReadable"]:
        raise UnreadableArchive(f"{where}: unsupported/encrypted archive table; export its files with OpenIV")
    try:
        members, skipped = extract_mod_files.members(view, suffixes)
    except AssetError as error:
        raise CollectionError(f"{where}: {error}") from error
    # Commit the archive together: a later conflict or budget error cannot leak earlier rows.
    staged, locations = dict(found), dict(origin)
    pending_notes = [f"{where}: {note}" for note in skipped]
    for key, (path, blob) in members.items():
        _add(staged, locations, key, blob, f"{where}:{path}")
    try:
        metas, _ = extract_mod_files.members(view, (".meta",))
    except AssetError as error:
        pending_notes.append(f"{where}: metas not read ({error})")
        metas = {}
    if META_NAME in metas:
        _add(staged, locations, META_NAME, metas[META_NAME][1], f"{where}:{metas[META_NAME][0]}")
    found.clear()
    found.update(staged)
    origin.clear()
    origin.update(locations)
    notes.extend(pending_notes)


def _from_zip(
    path: Path, found: dict[str, bytes], origin: dict[str, str], notes: list[str], suffixes: tuple[str, ...]
) -> None:
    """An .oiv package (a zip): loose files by base name, and any .rpf inside read as an archive."""
    with zipfile.ZipFile(path) as package:
        infos = package.infolist()
        if len(infos) > MAX_PACKAGE_MEMBERS:
            raise CollectionError(f"{path.name}: more than {MAX_PACKAGE_MEMBERS} entries")
        unpacked = 0  # every member read (nested archives too) counts: a package of compressed archives is bounded
        for info in infos:
            if info.is_dir():
                continue
            base = PurePosixPath(info.filename.replace("\\", "/")).name.lower()
            if not base.endswith((*suffixes, ".rpf")) and base != META_NAME:
                continue
            if info.file_size > MAX_FILE_BYTES:
                raise CollectionError(f"{path.name}:{info.filename}: larger than {MAX_FILE_BYTES} bytes")
            unpacked += info.file_size
            if unpacked > MAX_TOTAL_BYTES:
                raise CollectionError(f"{path.name}: the members read exceed the 1 GiB reader budget")
            blob = package.read(info)
            if base.endswith(".rpf"):
                _from_rpf(blob, f"{path.name}:{info.filename}", found, origin, notes, suffixes)
            else:
                _add(found, origin, base, blob, f"{path.name}:{info.filename}")


def collect(
    source: Path, suffixes: tuple[str, ...] = (".yft", ".ytd")
) -> tuple[dict[str, bytes], dict[str, str], list[str]]:
    """{lower base name: bytes} of the .yft/.ytd files and handling.meta a replace mod ships, where each
    came from, and notes. A folder is walked without following links (a symbolic link refuses)."""
    found: dict[str, bytes] = {}
    origin: dict[str, str] = {}
    notes: list[str] = []
    if source.is_symlink():
        raise CollectionError(f"{source}: symbolic links are refused")
    if source.is_dir():
        visited = 0
        for directory, dirs, files in os.walk(source, followlinks=False):
            visited += len(dirs) + len(files)
            if visited > MAX_PACKAGE_MEMBERS:
                raise CollectionError(f"{source.name}: more than {MAX_PACKAGE_MEMBERS} filesystem entries")
            for name in sorted(dirs + files):
                if Path(directory, name).is_symlink():
                    raise CollectionError(f"{Path(directory, name)}: symbolic links are refused")
            for name in sorted(files):
                path = Path(directory, name)
                if not stat.S_ISREG(path.stat().st_mode):
                    raise CollectionError(f"{path}: not a regular file")
                lower = name.lower()
                where = str(path.relative_to(source))
                if lower.endswith(suffixes) or lower == META_NAME:
                    if path.stat().st_size > MAX_FILE_BYTES:
                        raise CollectionError(f"{where}: larger than {MAX_FILE_BYTES} bytes")
                    with path.open("rb") as handle:
                        blob = handle.read(MAX_FILE_BYTES + 1)
                    _add(found, origin, lower, blob, where)
                elif lower.endswith(".rpf") and path.stat().st_size:
                    try:  # an archive beside loose files: unreadable ones (NG-encrypted) are noted, not fatal
                        with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
                            _from_rpf(view, where, found, origin, notes, suffixes)
                    except UnreadableArchive as error:
                        notes.append(f"{where}: not read ({error})")
                elif lower.endswith(".oiv"):
                    _from_zip(path, found, origin, notes, suffixes)
    elif source.is_file() and source.name.lower().endswith(".rpf"):
        with source.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
            _from_rpf(view, source.name, found, origin, notes, suffixes)
    elif source.is_file() and source.name.lower().endswith((".oiv", ".zip")):
        try:
            _from_zip(source, found, origin, notes, suffixes)
        except zipfile.BadZipFile as error:
            raise CollectionError(f"{source.name}: not a readable .oiv/.zip package ({error})") from error
    else:
        raise CollectionError(f"{source}: give the mod folder, its dlc.rpf or an .oiv package")
    return found, origin, notes
