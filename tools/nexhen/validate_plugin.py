#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the exact native nexHEN PRX/SPRX build and write an evidence report."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

from generate_exports import CONTRACT_EXPORT, id_char, name_to_nid


class ValidationError(RuntimeError):
    pass


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_header(data: bytes) -> dict[str, int]:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValidationError("PRX is not an ELF")
    if data[4:9] != bytes((2, 1, 1, 9, 2)):
        raise ValidationError("PRX class, byte order or FreeBSD ABI identity differs")
    values = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    names = (
        "type",
        "machine",
        "version",
        "entry",
        "phoff",
        "shoff",
        "flags",
        "ehsize",
        "phentsize",
        "phnum",
        "shentsize",
        "shnum",
        "shstrndx",
    )
    header = dict(zip(names, values, strict=True))
    if header["type"] != 0xFE18 or header["machine"] != 0x3E or header["version"] != 1:
        raise ValidationError("PRX ELF type or machine differs")
    if header["ehsize"] != 64 or header["phentsize"] != 56 or header["phnum"] != 14:
        raise ValidationError("PRX program-header layout differs")
    return header


def program_headers(data: bytes, header: dict[str, int]) -> list[dict[str, int]]:
    result = []
    for index in range(header["phnum"]):
        offset = header["phoff"] + index * header["phentsize"]
        if offset + 56 > len(data):
            raise ValidationError("PRX program header exceeds file")
        values = struct.unpack_from("<IIQQQQQQ", data, offset)
        ph = dict(zip(("type", "flags", "offset", "vaddr", "paddr", "filesz", "memsz", "align"), values, strict=True))
        if ph["filesz"] > ph["memsz"] or ph["offset"] + ph["filesz"] > len(data):
            raise ValidationError("PRX segment range is invalid")
        if ph["type"] == 1 and ph["flags"] & 3 == 3:
            raise ValidationError("PRX contains a writable executable LOAD segment")
        result.append(ph)
    if not any(ph["type"] == 1 and ph["flags"] & 1 for ph in result):
        raise ValidationError("PRX has no executable LOAD segment")
    if not any(ph["type"] == 1 and ph["flags"] & 2 for ph in result):
        raise ValidationError("PRX has no writable LOAD segment")
    return result


def readelf(path: Path, *arguments: str) -> str:
    try:
        return subprocess.check_output(["readelf", *arguments, str(path)], text=True, env={"LC_ALL": "C"})
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValidationError(f"readelf failed for {path}") from exc


def dynamic_symbols(path: Path) -> dict[str, tuple[int, int, str, str, str]]:
    symbols: dict[str, tuple[int, int, str, str, str]] = {}
    in_dynamic = False
    pattern = re.compile(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.+)$")
    for line in readelf(path, "-sW").splitlines():
        if line.startswith("Symbol table '"):
            in_dynamic = ".dynsym" in line
            continue
        if not in_dynamic:
            continue
        match = pattern.match(line)
        if not match:
            continue
        value, size, kind, binding, _visibility, section, name = match.groups()
        if name:
            symbols[name] = (int(value, 16), int(size), kind, binding, section)
    return symbols


def validate(prx: Path, sprx: Path, providers_path: Path) -> dict[str, object]:
    if not prx.is_file() or not sprx.is_file() or not providers_path.is_file():
        raise ValidationError("PRX, SPRX or provider manifest is missing")
    providers = json.loads(providers_path.read_text(encoding="utf-8"))
    data = prx.read_bytes()
    header = elf_header(data)
    phdrs = program_headers(data, header)

    needed = re.findall(r"Shared library: \[([^]]+)]", readelf(prx, "-dW"))
    expected_needed = [provider["name"] + ".prx" for provider in providers["providers"]]
    if needed != expected_needed:
        raise ValidationError(f"PRX providers differ: {needed!r}")

    symbols = dynamic_symbols(prx)
    export_suffix = f"#{id_char(providers['export_library_id'])}#{id_char(providers['export_module_id'])}"
    expected_exports = {name: name_to_nid(name) + export_suffix for name in providers["exports"]}
    for clear, encoded in expected_exports.items():
        if encoded not in symbols or symbols[encoded][3] != "GLOBAL" or symbols[encoded][4] == "UND":
            raise ValidationError(f"missing native export: {clear}")
    if CONTRACT_EXPORT in expected_exports:
        contract_symbol = symbols[expected_exports[CONTRACT_EXPORT]]
        if contract_symbol[1] != 312 or contract_symbol[2] != "OBJECT":
            raise ValidationError("nexHEN contract export has the wrong ABI size or type")

    provider_by_key = {provider["key"]: provider for provider in providers["providers"]}
    expected_imports = {
        name_to_nid(name)
        + f"#{id_char(provider_by_key[key]['library_id'])}#{id_char(provider_by_key[key]['module_id'])}"
        for name, key in providers["imports"].items()
    }
    actual_imports = {
        name
        for name, (_value, _size, _kind, binding, section) in symbols.items()
        if section == "UND" and binding == "GLOBAL"
    }
    if not actual_imports or not actual_imports.issubset(expected_imports):
        raise ValidationError(f"PRX strong imports are unclassified: {sorted(actual_imports - expected_imports)}")
    weak = {
        name
        for name, (_value, _size, _kind, binding, section) in symbols.items()
        if section == "UND" and binding == "WEAK"
    }
    if weak:
        raise ValidationError(f"PRX retains unresolved weak imports: {sorted(weak)}")

    entry_tags = {
        name: int(value, 16) for name, value in re.findall(r"\((INIT|FINI)\)\s+0x([0-9a-fA-F]+)", readelf(prx, "-dW"))
    }
    local_entries: dict[str, set[int]] = {"_init": set(), "_fini": set()}
    symbol_pattern = re.compile(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+FUNC\s+\S+\s+\S+\s+(?!UND)\S+\s+(_init|_fini)$")
    for line in readelf(prx, "-sW").splitlines():
        match = symbol_pattern.match(line)
        if match:
            local_entries[match.group(2)].add(int(match.group(1), 16))
    if (
        entry_tags.keys() != {"INIT", "FINI"}
        or local_entries["_init"] != {entry_tags.get("INIT")}
        or local_entries["_fini"] != {entry_tags.get("FINI")}
    ):
        raise ValidationError(f"PRX init/fini tags differ: tags={entry_tags!r}, symbols={local_entries!r}")

    self_data = sprx.read_bytes()
    if len(self_data) < 0x40 or self_data[:4] != bytes.fromhex("4f153d1d"):
        raise ValidationError("SPRX is not a fake SELF container")
    _key_type, header_size, meta_size, file_size, entries, flags = struct.unpack_from("<I2HQ2H4x", self_data, 8)
    if (
        not len(self_data) <= file_size < len(self_data) + 16
        or not header_size
        or not meta_size
        or entries != 12
        or flags != 0x22
    ):
        raise ValidationError("SPRX container layout differs")
    embedded_elf = 32 + entries * 32
    expected_self_header = bytearray(data[:64])
    expected_self_header[60:62] = b"\0\0"
    if self_data[embedded_elf : embedded_elf + 64] != expected_self_header:
        raise ValidationError("SPRX embedded ELF header differs from the validated PRX")

    return {
        "schemaVersion": 1,
        "kind": "gtavmenu-nexhen-native-validation",
        "hardwareValidated": False,
        "targetId": "PPSA04264_01.010.002_DISC",
        "titleId": "PPSA04264",
        "contentVersion": "01.010.002",
        "loader": "corrected-broker" if CONTRACT_EXPORT in expected_exports else "nexHEN-stock",
        "contract": ({"abi": 1, "size": 312, "operationOffset": 264} if CONTRACT_EXPORT in expected_exports else None),
        "protocol": {"loaderBridge": 4} if CONTRACT_EXPORT in expected_exports else None,
        "artifacts": {
            "prx": {"path": str(prx), "size": len(data), "sha256": digest(prx)},
            "sprx": {"path": str(sprx), "size": len(self_data), "sha256": digest(sprx)},
        },
        "elf": {
            "osAbi": 9,
            "abiVersion": 2,
            "type": "0xfe18",
            "programHeaders": header["phnum"],
            "loadFlags": [ph["flags"] for ph in phdrs if ph["type"] == 1],
            "writableExecutableLoad": False,
        },
        "providers": needed,
        "exports": expected_exports,
        "strongImportCount": len(actual_imports),
        "linkDiscardedImportCount": len(expected_imports - actual_imports),
        "weakImports": sorted(weak),
        "entryPoints": {"init": entry_tags["INIT"], "fini": entry_tags["FINI"]},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prx", required=True, type=Path)
    parser.add_argument("--sprx", required=True, type=Path)
    parser.add_argument("--providers", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="NEW JSON evidence file")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    try:
        report = validate(args.prx, args.sprx, args.providers)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError, ValidationError) as exc:
        parser.error(str(exc))
    print(f"validated native nexHEN plugin: {report['artifacts']['sprx']['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
