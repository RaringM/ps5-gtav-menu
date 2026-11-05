#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Adapt the nexHEN PRX script and inject exact SCE provider records."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

PHDR_TYPES = {
    "PT_SCE_MODULEPARAM": "0x61000002",
    "PT_SCE_COMMENT": "0x6fffff00",
    "PT_SCE_LIBVERSION": "0x6fffff01",
}
DYNAMIC_INPUT = "        *(.dynamic)"


def record(tag: int, value: int) -> str:
    return f"        QUAD (0x{tag:08x}); QUAD (0x{value:016x})"


def adapt(text: str, manifest: dict) -> str:
    for name, value in PHDR_TYPES.items():
        if text.count(name) != 1:
            raise ValueError(f"expected one {name} in nexHEN prx.script")
        text = text.replace(name, value)
    if text.count(DYNAMIC_INPUT) != 1:
        raise ValueError("nexHEN prx.script .dynamic rule changed")

    lines = ["        /* SCE records generated from providers.json. */"]
    for provider in manifest["providers"]:
        module_id = provider["module_id"]
        library_id = provider["library_id"]
        lines.extend(
            [
                record(0x61000045, (module_id << 48) | 0x0000010100000000),
                record(0x61000049, (library_id << 48) | 0x0000000100000000),
                record(0x61000019, (library_id << 48) | 9),
            ]
        )
    lines.extend(
        [
            record(0x61000043, (manifest["export_module_id"] << 48) | 0x0000010100000000),
            record(0x61000011, 0),
            record(0x61000041, 0),
            record(0x61000047, (manifest["export_library_id"] << 48) | 0x0000000100000000),
            record(0x61000017, (manifest["export_library_id"] << 48) | 1),
            "        QUAD (0x6100003f); QUAD (SIZEOF (.dynsym))",
            "        QUAD (0x6100003d); QUAD (SIZEOF (.hash))",
        ]
    )
    if not manifest["providers"]:
        lines.extend(
            [
                "        QUAD (3); QUAD (ADDR (.got.plt))",
                "        QUAD (23); QUAD (ADDR (.rela.plt))",
                "        QUAD (20); QUAD (7)",
                "        QUAD (2); QUAD (SIZEOF (.rela.plt))",
            ]
        )
    lines.append(DYNAMIC_INPUT)
    return text.replace(DYNAMIC_INPUT, "\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--providers", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.providers.read_text(encoding="utf-8"))
    output = adapt(args.input.read_text(encoding="utf-8"), manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
