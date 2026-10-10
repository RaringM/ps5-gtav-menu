#!/usr/bin/env python3
"""Copy files of given types out of a PC mod archive (dlc.rpf) into a folder.

The archive is untrusted data, read with the bounded RPF7 reader of gtavmenu_tools (OPEN/NONE tables;
uncompressed nested archives up to four levels; fixed size limits). Each member whose name ends with
one of the --suffix values is written as <out>/<lower-case base name>: a resource with its RSC7 header
rebuilt from the table flags (the loose-file form the converters read), any other file as stored.
A base name that appears twice is refused, and so is an existing output file. Run it with
`python3 -I`, never from inside the mod's folder.

  extract_mod_files.py mod/dlc.rpf --suffix .ybn --out work/ybn [--only mm_basic_col ...]
  extract_mod_files.py mod/dlc.rpf --suffix .ybn --list

--only keeps the named members (base names without the suffix; each must exist). --list prints
`<name> <bytes> <path in the archive>` per match instead of writing.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import argparse  # noqa: E402
import mmap  # noqa: E402
import re  # noqa: E402
from dataclasses import replace  # noqa: E402

from gtavmenu_tools.asset_formats import AssetError, read_rpf_member, rpf_header, rpf_members  # noqa: E402
from gtavmenu_tools.ymap import MOD_LIMITS, label_name  # noqa: E402

MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_READ_BYTES = 1024 * 1024 * 1024
MAX_DEPTH = 4
_SUFFIX = re.compile(r"\.[a-z0-9]{1,8}")
_NAME = re.compile(r"[a-z0-9_.+-]{1,96}")


def members(view, suffixes: tuple[str, ...]) -> tuple[dict[str, tuple[str, bytes]], list[str]]:
    """{lower base name: (path in the archive, file bytes)} of the members with these suffixes, and
    notes for what was skipped (encrypted, compressed or too deeply nested archives, oversize files)."""
    found: dict[str, tuple[str, bytes]] = {}
    notes: list[str] = []
    pending = [(0, len(view), "", 0)]
    total = 0
    while pending:
        base, size, prefix, depth = pending.pop()

        def read_at(offset: int, count: int, base: int = base, size: int = size) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise AssetError("archive read outside its bounds")
            return bytes(view[base + offset : base + offset + count])

        try:
            end = rpf_header(read_at(0, 16), size)["tableEnd"]
            table = rpf_members(read_at(0, end), MOD_LIMITS, file_size=size, read_at=read_at, name_check=label_name)
        except AssetError as error:
            if not prefix:
                raise
            notes.append(f"{prefix.rstrip('/')}: {error}")
            continue
        for member in table:
            lower = member.name.lower()
            path = prefix + member.name
            if lower.endswith(".rpf") and not member.resource:
                if member.compressed or member.encrypted or depth >= MAX_DEPTH:
                    notes.append(f"{path}: nested archive is compressed, encrypted or too deep")
                else:
                    pending.append((base + member.offset, member.size, path + "/", depth + 1))
                continue
            if not lower.endswith(suffixes):
                continue
            key = lower.rsplit("/", 1)[-1]
            if not _NAME.fullmatch(key):
                notes.append(f"{path}: unusable file name")
                continue
            if member.size > MAX_MEMBER_BYTES or member.encrypted:
                notes.append(f"{path}: encrypted or larger than {MAX_MEMBER_BYTES} bytes")
                continue
            if key in found:
                raise AssetError(f"{key} appears twice in the mod archive ({found[key][0]}, {path})")
            total += member.size
            if total > MAX_READ_BYTES:
                raise AssetError("the matching files exceed the reader budget")
            blob = read_rpf_member(read_at(member.offset, member.size), replace(member, offset=0), MAX_MEMBER_BYTES)
            found[key] = (path, blob)
    return found, notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", type=Path, help="the mod's PC archive (dlc.rpf)")
    parser.add_argument("--suffix", action="append", required=True, help="file type to copy, e.g. .ybn (repeatable)")
    parser.add_argument("--only", action="append", default=[], metavar="NAME", help="keep only these base names")
    parser.add_argument("--out", type=Path, help="output folder (created)")
    parser.add_argument("--list", action="store_true", help="print the matches instead of writing them")
    args = parser.parse_args(argv)
    suffixes = tuple(s.lower() for s in args.suffix)
    if not all(_SUFFIX.fullmatch(s) for s in suffixes):
        parser.error("--suffix takes a file type such as .ybn")
    if not args.list and args.out is None:
        parser.error("--out is required (or --list)")
    try:
        with args.mod.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
            found, notes = members(view, suffixes)
    except (AssetError, OSError, ValueError) as error:
        print(f"extract_mod_files: {args.mod.name}: {error}", file=sys.stderr)
        return 1
    if args.only:
        wanted = {}
        for name in args.only:
            key = next((k for k in found if k.rsplit(".", 1)[0] == name.lower()), None)
            if key is None:
                print(f"extract_mod_files: the archive has no {name} ({', '.join(suffixes)})", file=sys.stderr)
                return 1
            wanted[key] = found[key]
        found = wanted
    for note in notes:
        print(f"note: {note}")
    if args.list:
        for key, (path, blob) in sorted(found.items()):
            print(f"{key} {len(blob)} {path}")
        return 0
    clashes = sorted(k for k in found if (args.out / k).exists())
    if clashes:
        print(f"extract_mod_files: refusing to overwrite {', '.join(clashes)} in {args.out}", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    for key, (_path, blob) in sorted(found.items()):
        (args.out / key).write_bytes(blob)
    print(f"{len(found)} file(s) ({', '.join(suffixes)}) from {args.mod.name} into {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
