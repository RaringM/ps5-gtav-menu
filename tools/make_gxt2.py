#!/usr/bin/env python3
"""Write or read a GXT2 text table (game text labels: key hash -> UTF-8 string).

Layout (the PC GXT2 format, which the PS5 game's label lookup and DLC text merger read):

  +0x00  u32 magic 'GXT2' (bytes "2TXG")
  +0x04  u32 count
  +0x08  {u32 hash, u32 offset}[count]   ascending by hash (binary search), offset from file start
  ...    u32 magic 'GXT2', u32 end offset  (the merger sizes the last string as next.offset - offset)
  ...    NUL-terminated UTF-8 strings

Keys are hashed with the case-folded joaat (lowercase, '\\' -> '/'). The writer is not yet checked
against a retail PS5 .gxt2; `--check` parses any file, including one from the user's own game.

  make_gxt2.py --entry RAZOR=Razor --entry GMTORNADO="Tornado Custom" --output gtavmenu.gxt2
  make_gxt2.py --check some.gxt2
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"2TXG"


def label_hash(key: str) -> int:
    value = 0
    for byte in key.lower().replace("\\", "/").encode("utf-8"):
        value = (value + byte) & 0xFFFFFFFF
        value = (value + (value << 10)) & 0xFFFFFFFF
        value ^= value >> 6
    value = (value + (value << 3)) & 0xFFFFFFFF
    value ^= value >> 11
    return (value + (value << 15)) & 0xFFFFFFFF


def build(entries: dict[str, str]) -> bytes:
    rows = sorted((label_hash(k), v.encode("utf-8") + b"\0") for k, v in entries.items())
    if len({h for h, _ in rows}) != len(rows):
        raise SystemExit("label hash collision")
    table_end = 8 + 8 * len(rows) + 8
    offsets, cursor = [], table_end
    for _, text in rows:
        offsets.append(cursor)
        cursor += len(text)
    out = MAGIC + struct.pack("<I", len(rows))
    out += b"".join(struct.pack("<II", h, o) for (h, _), o in zip(rows, offsets, strict=True))
    out += MAGIC + struct.pack("<I", cursor)
    out += b"".join(text for _, text in rows)
    return out


def parse(blob: bytes) -> dict[int, str]:
    if blob[:4] != MAGIC:
        raise SystemExit("not a GXT2 file")
    (count,) = struct.unpack_from("<I", blob, 4)
    rows = [struct.unpack_from("<II", blob, 8 + 8 * i) for i in range(count)]
    trailer = 8 + 8 * count
    if blob[trailer : trailer + 4] != MAGIC:
        raise SystemExit("missing GXT2 trailer")
    if [h for h, _ in rows] != sorted(h for h, _ in rows):
        raise SystemExit("entries are not sorted by hash")
    return {h: blob[o : blob.index(b"\0", o)].decode("utf-8") for h, o in rows}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--entry", action="append", default=[], metavar="KEY=TEXT")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args(argv)
    if args.check:
        for h, text in sorted(parse(args.check.read_bytes()).items()):
            print(f"{h:#010x} {text}")
        return 0
    if not args.entry or not args.output:
        parser.error("--entry and --output are required unless --check")
    entries = dict(item.split("=", 1) for item in args.entry)
    blob = build(entries)
    readback = parse(blob)
    if readback != {label_hash(k): v for k, v in entries.items()}:
        raise SystemExit("readback mismatch")
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.write_bytes(blob)
    for key, text in entries.items():
        print(f"{key} -> {label_hash(key):#010x} {text!r}")
    print(f"wrote {args.output} bytes={len(blob)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
