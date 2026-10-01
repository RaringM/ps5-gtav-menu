#!/usr/bin/env python3
"""Emit build values for the loader from an exact target manifest."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from gtavmenu_tools.target_profile import load_manifest


def _manifest(target_manifest: Path) -> dict[str, object]:
    manifest, _ = load_manifest(target_manifest)
    return manifest


def readiness_values(target_manifest: Path) -> tuple[str, str, str, str]:
    manifest = _manifest(target_manifest)
    loader = manifest["loader"]
    if not isinstance(loader, dict):
        raise ValueError("target manifest loader must be an object")
    anchor = int(loader["playerPedAnchor"], 0)
    offset = int(loader["playerPedOffset"], 0)
    build = manifest["build"]
    if not isinstance(build, dict) or not isinstance(build.get("injection"), dict):
        raise ValueError("target manifest build.injection must be an object")
    cave_enabled = build["injection"].get("caveInject") is True
    cave = int(loader["caveAddress"], 0) if cave_enabled else 0
    cave_allocation = int(loader["caveAllocationBytes"], 0) if cave_enabled else 0
    if min(anchor, offset) <= 0 or (cave_enabled and min(cave, cave_allocation) <= 0):
        raise ValueError("loader readiness and enabled cave values must be nonzero")
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
    flags = [
        "-DGTAV_LOADER_TARGET_PIN_OVERRIDE=1",
        f'-DGTAV_LOADER_TARGET_PIN_ID=\\"{manifest["targetId"]}\\"',
        f'-DGTAV_LOADER_EXPECT_TARGET_ID=\\"{manifest["targetId"]}\\"',
        f"-DGTAV_LOADER_TARGET_SP_READY_ADDR=0x{int(loader['playerPedAnchor'], 0):x}ull",
        f"-DGTAV_LOADER_TARGET_SP_READY_OFFSET=0x{int(loader['playerPedOffset'], 0):x}ull",
    ]
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
    phase = manifest.get("renderPhase")
    if isinstance(phase, dict):
        cycle = phase.get("cycle")
        if not isinstance(cycle, dict):
            raise ValueError("renderPhase.cycle must be an object")
        flags.append("-DGTAV_RENDER_PHASE_TARGET_VALID=1")
        phase_integers = {
            "GTAV_RENDER_PHASE_DISCOVERY_ROOT": (phase["root"], "ull"),
            "GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE": (phase["leafVtable"], "ull"),
            "GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE": (phase["groupVtable"], "ull"),
            "GTAV_RENDER_PHASE_DISCOVERY_TASK_ID": (phase["taskId"], "u"),
            "GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL": (phase["original"], "ull"),
        }
        for macro, (raw, suffix) in phase_integers.items():
            flags.append(f"-D{macro}=0x{int(raw, 0):x}{suffix}")
        for key, macro in (
            ("epoch", "GTAV_CYCLE_EPOCH_ADDR"),
            ("reset", "GTAV_CYCLE_RESET_ADDR"),
            ("producer", "GTAV_CYCLE_PRODUCER_ADDR"),
            ("consumer", "GTAV_CYCLE_CONSUMER_ADDR"),
            ("count0", "GTAV_CYCLE_COUNT0_ADDR"),
            ("count1", "GTAV_CYCLE_COUNT1_ADDR"),
            ("text", "GTAV_CYCLE_TEXT_ADDR"),
            ("counter", "GTAV_CYCLE_COUNTER_ADDR"),
        ):
            flags.append(f"-D{macro}=0x{int(cycle[key], 0):x}ull")
        fingerprints = phase["fingerprints"]
        if not isinstance(fingerprints, dict):
            raise ValueError("renderPhase.fingerprints must be an object")
        for key, macro in (
            ("leafInvoke", "GTAV_RENDER_PHASE_LEAF_INVOKE"),
            ("groupInvoke", "GTAV_RENDER_PHASE_GROUP_INVOKE"),
            ("group2Dispatch", "GTAV_RENDER_PHASE_GROUP2_DISPATCH"),
            ("registration", "GTAV_RENDER_PHASE_REGISTRATION"),
        ):
            item = fingerprints.get(key)
            if not isinstance(item, dict):
                raise ValueError(f"renderPhase.fingerprints.{key} must be an object")
            flags.append(f"-D{macro}_ADDR=0x{int(item['address'], 0):x}ull")
            flags.append(f"-D{macro}_BYTES={_byte_list(item['bytes'], name=f'renderPhase.{key}.bytes')}")
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
