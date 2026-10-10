#!/usr/bin/env python3
"""Fail the build when the menu worker no longer fits the injected memory region.

The loader maps the worker into a fixed in-game allocation (PAYLOAD_LOADER_CAVE_ALLOC) and refuses
an image larger than it (src/common/elf_inject.c, "image exceeds the reserved"). That refusal only
shows at inject time on the console; this check moves it to the build. The span is computed the
same way as the injector: lowest PT_LOAD start rounded down to 16 KiB, highest end rounded up.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

PAGE = 0x4000
PT_LOAD = 1


def image_span(elf: bytes) -> int:
    if len(elf) < 64 or elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise ValueError("not a little-endian ELF64 file")
    phoff = struct.unpack_from("<Q", elf, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    if phentsize < 56 or phoff + phentsize * phnum > len(elf):
        raise ValueError("program headers out of range")
    lo, hi = None, 0
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type = struct.unpack_from("<I", elf, base)[0]
        p_vaddr = struct.unpack_from("<Q", elf, base + 16)[0]
        p_memsz = struct.unpack_from("<Q", elf, base + 40)[0]
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        start = p_vaddr // PAGE * PAGE
        end = -(-(p_vaddr + p_memsz) // PAGE) * PAGE
        lo = start if lo is None else min(lo, start)
        hi = max(hi, end)
    if lo is None or hi <= lo:
        raise ValueError("no loadable segments")
    return hi - lo


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", type=Path, help="the linked worker ELF")
    parser.add_argument("--limit", required=True, type=lambda v: int(v, 0), help="cave allocation in bytes")
    parser.add_argument(
        "--warn-margin", type=lambda v: int(v, 0), default=0x20000, help="warn when less than this is left (bytes)"
    )
    args = parser.parse_args(argv)
    try:
        span = image_span(args.elf.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"check_worker_span: {args.elf}: {exc}", file=sys.stderr)
        return 1
    left = args.limit - span
    print(f"check_worker_span: worker image 0x{span:x} of 0x{args.limit:x} (0x{max(left, 0):x} left)")
    if span > args.limit:
        print(
            f"check_worker_span: worker image 0x{span:x} exceeds the cave allocation 0x{args.limit:x}; the "
            "loader would refuse to inject it (raise PAYLOAD_LOADER_CAVE_ALLOC with a hardware test, or "
            "trim worker memory)",
            file=sys.stderr,
        )
        return 1
    if left < args.warn_margin:
        print(f"check_worker_span: WARNING only 0x{left:x} bytes left in the cave allocation", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
