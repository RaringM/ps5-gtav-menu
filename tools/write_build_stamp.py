#!/usr/bin/env python3
"""Write a deterministic build-profile stamp only when its contents change."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def parse_setting(value: str) -> tuple[str, str]:
    key, separator, setting = value.partition("=")
    if not separator or not key:
        raise argparse.ArgumentTypeError("settings must be KEY=VALUE")
    return key, setting


def compiler_version(compiler: str) -> str:
    result = subprocess.run(
        [compiler, "--version"],
        check=False,
        capture_output=True,
        text=True,
    )
    first_line = next((line.strip() for line in result.stdout.splitlines() if line.strip()), "")
    if result.returncode != 0 or not first_line:
        raise RuntimeError(f"cannot identify compiler {compiler!r}")
    return first_line


def write_stamp(output: Path, settings: list[tuple[str, str]], *, compiler: str | None = None) -> bool:
    data = dict(settings)
    if compiler is not None:
        data["compiler"] = compiler
        data["compilerVersion"] = compiler_version(compiler)
    rendered = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if output.is_file() and output.read_text(encoding="utf-8") == rendered:
        return False
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    temporary.write_text(rendered, encoding="utf-8")
    temporary.replace(output)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", help="cross-compiler path to identify in the stamp")
    parser.add_argument("--set", action="append", default=[], type=parse_setting, dest="settings")
    args = parser.parse_args()
    try:
        write_stamp(args.output, args.settings, compiler=args.compiler)
    except (OSError, RuntimeError) as exc:
        print(f"build stamp failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
