#!/usr/bin/env python3
"""Validate and print an OnionHEN ABI-v1 descriptor from a plugin ELF."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from gtavmenu_tools.onionhen import inspect_plugin_bytes


def inspect_plugin(path: Path) -> dict[str, int | str]:
    return inspect_plugin_bytes(path.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--expect-id")
    parser.add_argument("--expect-version")
    args = parser.parse_args()
    try:
        result = inspect_plugin(args.plugin)
        if args.expect_id and result["id"] != args.expect_id:
            raise ValueError(f"expected plugin id {args.expect_id}, got {result['id']}")
        if args.expect_version and result["version"] != args.expect_version:
            raise ValueError(f"expected version {args.expect_version}, got {result['version']}")
    except (OSError, UnicodeDecodeError, ValueError, struct.error) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
