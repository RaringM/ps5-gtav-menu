#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Finalize SCE identity and provider string offsets on a nexHEN PRX."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

DT_SCE_NEEDED_MODULE = 0x61000045
DT_SCE_IMPORT_LIB = 0x61000049
DT_SCE_MODULE_INFO = 0x61000043
DT_SCE_MODULE_FILENAME = 0x61000041
DT_SCE_EXPORT_LIB = 0x61000047


def sections(data: bytes) -> dict[str, tuple[int, int, int]]:
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shentsize < 64 or shoff + shentsize * shnum > len(data):
        raise ValueError("invalid ELF section table")
    raw = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    if not 0 <= shstrndx < len(raw):
        raise ValueError("invalid section-name table index")
    names_header = raw[shstrndx]
    names = data[names_header[4] : names_header[4] + names_header[5]]
    result = {}
    for header in raw:
        end = names.find(b"\0", header[0])
        name = names[header[0] : end].decode("ascii") if end >= 0 else ""
        result[name] = (header[4], header[5], header[9])
    return result


def dynstr_offset(data: bytes, offset: int, size: int, name: str) -> int:
    needle = name.encode("ascii") + b"\0"
    table = data[offset : offset + size]
    matches = []
    start = 0
    while True:
        found = table.find(needle, start)
        if found < 0:
            break
        if found == 0 or table[found - 1] == 0:
            matches.append(found)
        start = found + 1
    if len(matches) != 1:
        raise ValueError(f"expected one exact .dynstr entry for {name!r}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prx", type=Path)
    parser.add_argument("--providers", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.providers.read_text(encoding="utf-8"))
    module = manifest["module"]
    providers = manifest["providers"]

    data = bytearray(args.prx.read_bytes())
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected a little-endian ELF64 image")
    if struct.unpack_from("<H", data, 0x10)[0] != 3:
        raise ValueError("LLD output is not ET_DYN")
    if struct.unpack_from("<H", data, 0x38)[0] != 14:
        raise ValueError("LLD output does not have the expected 14 PHDR layout")

    section_map = sections(data)
    dynstr = section_map.get(".dynstr")
    dynamic = section_map.get(".dynamic")
    if dynstr is None or dynamic is None or dynamic[2] < 16:
        raise ValueError("PRX lacks dynamic sections")
    names = [*(provider["name"] for provider in providers), module, module + ".prx"]
    offsets = {name: dynstr_offset(data, dynstr[0], dynstr[1], name) for name in names}
    by_module = {provider["module_id"]: provider["name"] for provider in providers}
    by_library = {provider["library_id"]: provider["name"] for provider in providers}

    needed = imported = 0
    for entry_offset in range(dynamic[0], dynamic[0] + dynamic[1], dynamic[2]):
        tag, value = struct.unpack_from("<QQ", data, entry_offset)
        name = None
        if tag == DT_SCE_NEEDED_MODULE:
            name = by_module.get(value >> 48)
            needed += 1
        elif tag == DT_SCE_IMPORT_LIB:
            name = by_library.get(value >> 48)
            imported += 1
        elif tag in (DT_SCE_MODULE_INFO, DT_SCE_EXPORT_LIB):
            name = module
        elif tag == DT_SCE_MODULE_FILENAME:
            name = module + ".prx"
        if tag in (DT_SCE_NEEDED_MODULE, DT_SCE_IMPORT_LIB) and name is None:
            raise ValueError(f"unknown provider id in dynamic tag 0x{tag:x}")
        if name is not None:
            struct.pack_into("<Q", data, entry_offset + 8, (value & 0xFFFFFFFF00000000) | offsets[name])
    if needed != len(providers) or imported != len(providers):
        raise ValueError("provider metadata count does not match providers.json")

    data[7] = 9
    data[8] = 2
    struct.pack_into("<H", data, 0x10, 0xFE18)
    args.prx.write_bytes(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
