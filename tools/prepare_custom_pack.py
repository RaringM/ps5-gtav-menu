#!/usr/bin/env python3
"""Prepare or verify the first additive custom texture package."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from gtavmenu_tools.custom_packs import (
    PACK_ID,
    CustomPackError,
    compare_package,
    expected_package,
    publish_package,
    read_regular,
    verify_package,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACT = ROOT / "build/assets/texture-authored-resource-agent-c.ptd"
DEFAULT_BUNDLE = ROOT / "build/assets/asset-experiment-bundle-b.zip"
DEFAULT_TARGET = ROOT / "data/targets/ppsa04264-01.010.002.json"
DEFAULT_OUTPUT = ROOT / f"build/custom-assets/{PACK_ID}"


def prepare(artifact: Path, bundle: Path, target: Path, output: Path) -> dict:
    files = expected_package(read_regular(artifact), read_regular(bundle), read_regular(target))
    if output.exists() and not output.is_symlink():
        compare_package(output, files)
    else:
        publish_package(output, files)
    return verify_package(output)


def result(manifest: dict, package: Path) -> dict:
    resource = manifest["resource"]
    return {
        "packId": manifest["packId"],
        "package": str(package),
        "dictionary": resource["logicalDictionary"],
        "bytes": resource["bytes"],
        "sha256": resource["sha256"],
        "liveTestReady": manifest["qualification"]["liveTestReady"],
        "verified": True,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--artifact", type=Path, default=DEFAULT_ARTIFACT)
    prepare_parser.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE)
    prepare_parser.add_argument("--target", type=Path, default=DEFAULT_TARGET)
    prepare_parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("package", type=Path, nargs="?", default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            package = args.output_dir
            manifest = prepare(args.artifact, args.bundle, args.target, package)
        else:
            package = args.package
            manifest = verify_package(package)
    except (CustomPackError, OSError, KeyError, TypeError) as exc:
        parser.exit(1, f"custom pack preparation failed: {exc}\n")
    print(json.dumps(result(manifest, package), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
