#!/usr/bin/env python3
"""Fail the build when the menu worker carries an undefined weak import.

The injector (src/common/elf_inject.c) resolves every undefined symbol a relocation names through
the game's loaded modules and fails closed on a miss. It has no weak-symbol fallback, so an
undefined weak reference that no module exports stops the inject at "unresolved import". This check
walks the same dynamic relocation tables (DT_RELA and DT_JMPREL) and moves that failure to the
build. A build-gated weak hook needs a real definition (a stub) whenever its owner is compiled out.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1
PT_DYNAMIC = 2
DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_SYMENT, DT_PLTRELSZ, DT_JMPREL = 5, 6, 7, 8, 11, 2, 23
STB_WEAK = 2
SHN_UNDEF = 0


def _segments(elf: bytes) -> list[tuple[int, int, int, int]]:
    if len(elf) < 64 or elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise ValueError("not a little-endian ELF64 file")
    phoff = struct.unpack_from("<Q", elf, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    if phentsize < 56 or phoff + phentsize * phnum > len(elf):
        raise ValueError("program headers out of range")
    out = []
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", elf, phoff + i * phentsize)
        out.append((p_type, p_offset, p_vaddr, p_filesz))
    return out


def weak_undefined_imports(elf: bytes) -> list[str]:
    segments = _segments(elf)

    def offset(vaddr: int) -> int:
        for p_type, p_offset, p_vaddr, p_filesz in segments:
            if p_type == PT_LOAD and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + vaddr - p_vaddr
        raise ValueError(f"address 0x{vaddr:x} is outside the loaded segments")

    dynamic = [s for s in segments if s[0] == PT_DYNAMIC]
    if not dynamic:
        return []
    _, dyn_off, _, dyn_size = dynamic[0]
    tags: dict[int, int] = {}
    for pos in range(dyn_off, dyn_off + dyn_size - 15, 16):
        tag, value = struct.unpack_from("<qQ", elf, pos)
        if tag == 0:
            break
        tags.setdefault(tag, value)
    if DT_SYMTAB not in tags or DT_STRTAB not in tags:
        return []
    symtab, strtab = offset(tags[DT_SYMTAB]), offset(tags[DT_STRTAB])
    syment = tags.get(DT_SYMENT, 24)

    names: set[str] = set()
    for table, size in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        if table not in tags or not tags.get(size):
            continue
        start = offset(tags[table])
        for pos in range(start, start + tags[size], 24):
            symidx = struct.unpack_from("<Q", elf, pos + 8)[0] >> 32
            if symidx == 0:
                continue
            sym = symtab + symidx * syment
            st_name, st_info, _, st_shndx = struct.unpack_from("<IBBH", elf, sym)
            if st_shndx == SHN_UNDEF and st_info >> 4 == STB_WEAK:
                end = elf.index(b"\0", strtab + st_name)
                names.add(elf[strtab + st_name : end].decode("ascii", "replace"))
    return sorted(names)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", type=Path, help="the linked worker ELF")
    args = parser.parse_args(argv)
    try:
        names = weak_undefined_imports(args.elf.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"check_worker_imports: {args.elf}: {exc}", file=sys.stderr)
        return 1
    if names:
        print(
            "check_worker_imports: undefined weak imports the injector cannot resolve: "
            + ", ".join(names)
            + " (define a stub for the build that compiles their owner out)",
            file=sys.stderr,
        )
        return 1
    print("check_worker_imports: no undefined weak imports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
