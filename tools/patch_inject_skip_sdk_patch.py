#!/usr/bin/env python3
"""Patch ps5-payload-sdk startup to skip __patch_init in the injected worker.

The SDK CRT calls `__patch_init` from its startup path before `main`. In a game pid on this setup
that call fails at `kernel_set_ucred_caps` and the CRT then refuses to continue, so the injected
worker never reaches `main`. Neutralising the call (`xor eax,eax` plus NOPs, leaving the following
`test eax,eax` / `je` to take the success branch) lets startup continue.

The call site is located by resolving `__patch_init` in the ELF's symbol table and finding the CALL
that targets it -- not by matching the instructions around it. An earlier version keyed on the
following `je`'s displacement, which is pure code layout: an SDK update moved it from 0x15 to 0x14
and the build broke with nothing pointing at the cause.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

SHT_SYMTAB = 2
SHT_DYNSYM = 11
TARGET_SYMBOL = "__patch_init"
# The call's success value, written into exactly the space the call occupied.
REPLACEMENT = bytes.fromhex("31c0")  # xor eax, eax


def _sections(data: bytes) -> tuple[list[dict], int]:
    (shoff,) = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    out = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, typ, _flags, addr, offset, size, link, _info, _align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, off
        )
        out.append(
            {
                "name": name,
                "type": typ,
                "addr": addr,
                "offset": offset,
                "size": size,
                "link": link,
                "entsize": entsize,
            }
        )
    return out, shstrndx


def _cstr(data: bytes, base: int, index: int) -> str:
    end = data.index(b"\0", base + index)
    return data[base + index : end].decode("utf-8", "replace")


def _symbol_value(data: bytes, sections: list[dict], name: str) -> int | None:
    for sec in sections:
        if sec["type"] not in (SHT_SYMTAB, SHT_DYNSYM) or not sec["entsize"]:
            continue
        strtab = sections[sec["link"]]["offset"]
        for k in range(sec["size"] // sec["entsize"]):
            off = sec["offset"] + k * sec["entsize"]
            sym_name, _info, _other, _shndx, value, _size = struct.unpack_from("<IBBHQQ", data, off)
            if sym_name and value and _cstr(data, strtab, sym_name) == name:
                return value
    return None


def _text_sections(data: bytes, sections: list[dict], shstrndx: int) -> list[dict]:
    base = sections[shstrndx]["offset"]
    # No `sec["addr"]` requirement: the injected worker is a PIE whose .text starts at vaddr 0, and
    # symbol values are relative to that same base, so the address arithmetic works unchanged.
    return [sec for sec in sections if sec["size"] and _cstr(data, base, sec["name"]).startswith(".text")]


def _call_sites(data: bytes, text: dict, target: int) -> list[tuple[int, int]]:
    """(file offset, instruction length) of every `call rel32` in `text` reaching `target`.

    The CRT's call carries a 0x67 address-size prefix, which is part of the instruction and has to
    be overwritten too, so the prefixed form is matched first and reported with its real length.
    """
    found = []
    body = data[text["offset"] : text["offset"] + text["size"]]
    for i in range(len(body) - 6):
        for prefix, length in ((b"\x67\xe8", 6), (b"\xe8", 5)):
            if body[i : i + len(prefix)] != prefix:
                continue
            if prefix == b"\xe8" and i and body[i - 1] == 0x67:
                break  # already considered as the prefixed form
            (rel,) = struct.unpack_from("<i", body, i + len(prefix))
            if text["addr"] + i + length + rel == target:
                found.append((text["offset"] + i, length))
            break
    return found


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        raise SystemExit("usage: patch_inject_skip_sdk_patch.py <elf>")

    path = Path(argv[0])
    data = bytearray(path.read_bytes())
    sections, shstrndx = _sections(data)

    target = _symbol_value(data, sections, TARGET_SYMBOL)
    if target is None:
        raise SystemExit(f"{TARGET_SYMBOL} not found in {path} (is this an SDK-linked payload?)")

    sites = [site for text in _text_sections(data, sections, shstrndx) for site in _call_sites(data, text, target)]
    if len(sites) != 1:
        raise SystemExit(f"expected exactly one call to {TARGET_SYMBOL} at 0x{target:x}, found {len(sites)}")

    offset, length = sites[0]
    data[offset : offset + length] = REPLACEMENT + b"\x90" * (length - len(REPLACEMENT))
    path.write_bytes(data)
    print(f"patched {TARGET_SYMBOL} call at file offset 0x{offset:x} ({length} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
