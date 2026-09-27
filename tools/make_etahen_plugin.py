#!/usr/bin/env python3
"""Wrap an existing ELF in an etaHEN container offline; never overwrite an output."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from gtavmenu_tools.etahen import encode_plugin, inspect_container


def write_plugin(elf: Path, output: Path, *, plugin_id: str, version: str) -> dict[str, int | str]:
    data = encode_plugin(elf.read_bytes(), plugin_id=plugin_id, version=version)
    result = inspect_container(data)
    # Exclusive creation also protects the input if the caller supplies the same
    # path, a hard link, or a symlink as output. Validate before creating anything.
    with output.open("xb") as stream:
        stream.write(data)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--plugin-id", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        result = write_plugin(args.elf, args.output, plugin_id=args.plugin_id, version=args.version)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
