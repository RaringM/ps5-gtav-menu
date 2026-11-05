#!/usr/bin/env python3
"""Validate a PS4 target manifest and emit its build settings as a make fragment."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from gtavmenu_tools.ps4_build import load_config, make_fragment  # noqa: E402
from gtavmenu_tools.target_profile import TargetProfileError  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-manifest", type=Path)
    parser.add_argument("--make-fragment", type=Path, help="write the make settings to this file")
    args = parser.parse_args(argv)
    if args.target_manifest is None:
        parser.error("--target-manifest is required")
    try:
        config = load_config(args.target_manifest)
    except (OSError, ValueError, TargetProfileError) as exc:
        print(f"ps4_build: {exc}", file=sys.stderr)
        return 1
    text = make_fragment(config)
    if args.make_fragment:
        args.make_fragment.parent.mkdir(parents=True, exist_ok=True)
        if not args.make_fragment.is_file() or args.make_fragment.read_text(encoding="utf-8") != text:
            args.make_fragment.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
