#!/usr/bin/env python3
"""Inspect an etaHEN .plugin container offline and print its metadata and hashes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from gtavmenu_tools.etahen import inspect_container


def inspect_plugin(
    path: Path, *, expect_id: str | None = None, expect_version: str | None = None
) -> dict[str, int | str]:
    return inspect_container(path.read_bytes(), expect_id=expect_id, expect_version=expect_version)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin", type=Path)
    parser.add_argument("--expect-id")
    parser.add_argument("--expect-version")
    args = parser.parse_args()
    try:
        result = inspect_plugin(args.plugin, expect_id=args.expect_id, expect_version=args.expect_version)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
