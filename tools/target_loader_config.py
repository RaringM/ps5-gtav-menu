#!/usr/bin/env python3
"""Emit build values for the loader from an exact target manifest."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _manifest(target_manifest: Path) -> dict[str, object]:
    manifest = json.loads(target_manifest.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 2:
        raise ValueError("target manifest schemaVersion must be 2")
    return manifest


def readiness_values(target_manifest: Path) -> tuple[str, str, str, str]:
    manifest = _manifest(target_manifest)
    loader = manifest["loader"]
    if not isinstance(loader, dict):
        raise ValueError("target manifest loader must be an object")
    anchor = int(loader["playerPedAnchor"], 0)
    offset = int(loader["playerPedOffset"], 0)
    cave = int(loader["caveAddress"], 0)
    cave_allocation = int(loader["caveAllocationBytes"], 0)
    if min(anchor, offset, cave, cave_allocation) <= 0:
        raise ValueError("loader readiness and cave values must be nonzero")
    return f"0x{anchor:x}", f"0x{offset:x}", f"0x{cave:x}", f"0x{cave_allocation:x}"


def _byte_list(compact: object, *, name: str) -> str:
    if not isinstance(compact, str):
        raise ValueError(f"{name} must be a compact hex string")
    raw = bytes.fromhex(compact)
    if not raw:
        raise ValueError(f"{name} must not be empty")
    return ",".join(f"0x{value:02x}" for value in raw)


def loader_cflags(target_manifest: Path) -> list[str]:
    manifest = _manifest(target_manifest)
    loader = manifest["loader"]
    live_mapping = manifest["liveMapping"]
    if not isinstance(loader, dict) or not isinstance(live_mapping, dict):
        raise ValueError("target manifest loader/liveMapping must be objects")
    text = live_mapping["text"]
    if not isinstance(text, dict):
        raise ValueError("target manifest liveMapping.text must be an object")

    integers = {
        "GTAV_LOADER_VERSION_ADDR": (loader["versionSignatureAddr"], "ull"),
        "GTAV_LOADER_BROKER_TARGET": (loader["frameHookTarget"], "ull"),
        "GTAV_LOADER_BROKER_CONTINUATION": (loader["brokerContinuation"], "ull"),
        "GTAV_LOADER_BROKER_PATCH_LEN": (loader["brokerPatchLen"], "u"),
        "GTAV_LOADER_BROKER_STOLEN_LEN": (loader["brokerStolenLen"], "u"),
        "GTAV_LOADER_TEXT_LIVE_START": (text["liveStart"], "ull"),
        "GTAV_LOADER_TEXT_LIVE_END": (text["liveEnd"], "ull"),
    }
    flags = []
    for macro, (raw, suffix) in integers.items():
        value = int(raw, 0) if isinstance(raw, str) else int(raw)
        flags.append(f"-D{macro}=0x{value:x}{suffix}")
    flags.extend(
        [
            "-DGTAV_LOADER_VERSION_BYTES="
            + _byte_list(loader["versionSignatureExpectedCompact"], name="versionSignatureExpectedCompact"),
            "-DGTAV_LOADER_BROKER_EXPECTED_BYTES="
            + _byte_list(loader["brokerExpectedBytesCompact"], name="brokerExpectedBytesCompact"),
        ]
    )
    return flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-manifest", required=True, type=Path)
    parser.add_argument("--cflags", action="store_true", help="emit the exact loader pin compiler flags")
    args = parser.parse_args()
    try:
        values = loader_cflags(args.target_manifest) if args.cflags else readiness_values(args.target_manifest)
        print(" ".join(values))
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"target_loader_config: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
