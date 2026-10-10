#!/usr/bin/env python3
"""Repair mirrored seat-bone transforms in an imported PC vehicle fixture.

Some exporters (e.g. Zmodeler) write a seat bone whose stored local/inverse matrices are a mirror
across X (determinant -1, forward axis kept) while the bone's quaternion holds the nearest proper
rotation, 180 degrees about Z (which would turn the seat backwards). The converter's pose gate
(tools/pc_transforms_pose.py) correctly rejects that disagreement. This tool writes a new
fixture where each selected bone gets one consistent, proper, forward-facing transform:
quaternion identity, stored local M11 = +1, stored inverse M11 = +1 and M41 = -translation.x. Every
other byte is unchanged; the original fixture is left intact and the new manifest records the repair.
Only bones whose stored matrices are exactly that X mirror of the identity rotation are accepted.

  repair_pc_seat_mirror.py --fixture build/assets/pc-fixtures/supra-a80-a \\
      --material-report build/assets/supra-a80-materials-a.json \\
      --bone seat_dside_f --bone seat_pside_f --output build/assets/pc-fixtures/supra-a80-b
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import inspect_pc_vehicle_pose as pose_runner  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402

BONE_STRIDE = 0x50
MATRIX_BYTES = 0x40
SYSTEM_BASE = 0x50000000


def pc_walk_args(fixture: Path, material_report: Path) -> argparse.Namespace:
    """Arguments of the converter's PC-side walks (skeletons stage; reads no native input)."""
    return argparse.Namespace(pc_archive=None, pc_fixture=fixture, material_report=material_report)


def inspect(fixture: Path, material_report: Path) -> dict:
    return pose_runner.inspect(pc_walk_args(fixture, material_report))


def repair_payload(raw: bytearray, header: dict, bone_index: int) -> dict:
    bone_at = header["BonesPointer"] - SYSTEM_BASE + bone_index * BONE_STRIDE
    local_at = header["TransformationsPointer"] - SYSTEM_BASE + bone_index * MATRIX_BYTES
    inverse_at = header["TransformationsInvertedPointer"] - SYSTEM_BASE + bone_index * MATRIX_BYTES
    rotation = struct.unpack_from("<4f", raw, bone_at)
    translation = struct.unpack_from("<3f", raw, bone_at + 0x10)
    local = list(struct.unpack_from("<16f", raw, local_at))
    inverse = list(struct.unpack_from("<16f", raw, inverse_at))
    rotation_3x3 = [local[0], local[1], local[2], local[4], local[5], local[6], local[8], local[9], local[10]]
    if [round(v, 5) + 0.0 for v in rotation_3x3] != [-1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]:
        raise SystemExit(f"bone {bone_index}: stored local is not the X mirror of identity")
    if round(inverse[0], 5) != -1.0 or abs(inverse[12] - translation[0]) > 1e-4:
        raise SystemExit(f"bone {bone_index}: stored inverse is not the X mirror inverse")
    struct.pack_into("<4f", raw, bone_at, 0.0, 0.0, 0.0, 1.0)
    struct.pack_into("<f", raw, local_at, 1.0)
    struct.pack_into("<f", raw, inverse_at, 1.0)
    struct.pack_into("<f", raw, inverse_at + 12 * 4, -translation[0])
    return {"boneIndex": bone_index, "oldRotation": list(rotation), "translation": list(translation)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--bone", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)

    pc = inspect(args.fixture, args.material_report)
    skeletons = pc["freshSkinningEvidence"]["freshSkeletonEvidence"]["resources"]
    rows = {row["member"]: row for row in manifest["resources"]}
    copy_fixture(args.fixture, args.output, manifest)
    repairs = []
    for skeleton, pose in zip(skeletons, pc["resources"], strict=True):
        member = skeleton["member"]
        names = {bone["name"]: index for index, bone in enumerate(skeleton["bones"])}
        targets = [names[name] for name in args.bone if name in names]
        rejected = {row["boneIndex"] for row in pose["rejections"]}
        if not targets or not rejected <= set(targets):
            raise SystemExit(f"{member}: rejections {sorted(rejected)} are not all selected seat bones")
        path = args.output.joinpath("resources", *member.replace("!/", "/").split("/"))
        blob = path.read_bytes()
        raw = bytearray(zlib.decompressobj(-15).decompress(blob[16:]))
        fixed = [repair_payload(raw, skeleton["header"], index) for index in targets]
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        path.write_bytes(new_blob)
        row = rows[member]
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        repairs.append({"member": member, "bones": fixed})
    manifest["repairs"] = {
        "tool": "tools/repair_pc_seat_mirror.py",
        "reason": "seat bones stored as an X mirror with a 180-degree-Z quaternion; replaced by identity rotation",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "resources": repairs,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "repairs": repairs}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None
