#!/usr/bin/env python3
"""Recompute imprecise stored inverse-bind translations in an imported PC fixture (a80 mod kit).

The a80 exhaust parts (vehiclemods/a80_mods.rpf a80_exh_1/2.yft) store bone `exhaust_3`'s inverse
bind translation with ~1.5e-5 float error, just above the converter pose gate's 1e-5 tolerance
(tools/pc_transforms_pose.py). This tool writes a new fixture where each selected bone's
stored inverse translation (M41..M43) is recomputed so that the gate's product (stored inverse x
global pose) has a zero translation row (float64 solve against the gate's own global pose, then
float32). Only a precision repair is accepted: every recomputed value
must lie within 1e-4 of the stored one, and the stored rotation part is left untouched. Every other
byte is unchanged; the original fixture stays intact and the new manifest records the repair.

  repair_pc_inverse_precision.py --fixture build/assets/pc-fixtures/supra-a80-mods-a \\
      --material-report build/assets/supra-a80-mods-materials-a.json \\
      --bone exhaust_3 --bone exhaust_2 --output build/assets/pc-fixtures/supra-a80-mods-b
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402
from repair_pc_seat_mirror import MATRIX_BYTES, SYSTEM_BASE, inspect  # noqa: E402

PRECISION = 1e-4


def solve_row(row: list[float], m: list[list[float]]) -> list[float]:
    """x such that x * m = row (3x3, float64, Cramer's rule)."""
    if not all(math.isfinite(v) for v in [*row, *(v for r in m for v in r)]):
        raise SystemExit("bone inverse solve has non-finite values")
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-6:
        raise SystemExit("bone global rotation is singular")
    inverse = [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]
    return [sum(row[k] * inverse[k][j] for k in range(3)) for j in range(3)]


def repair_payload(raw: bytearray, header: dict, bone_index: int, global_pose: list[float]) -> dict:
    inverse_at = header["TransformationsInvertedPointer"] - SYSTEM_BASE + bone_index * MATRIX_BYTES
    # Row-major M11..M44, translation in row 4. The gate multiplies the stored inverse (M44 := 1)
    # by the bone's global pose, so row 4 of that product is x * G_rot + G_t: choose x to zero it.
    g_rot = [global_pose[r * 4 : r * 4 + 3] for r in range(3)]
    g_t = global_pose[12:15]
    new = solve_row([-v for v in g_t], g_rot)
    new = list(struct.unpack("<3f", struct.pack("<3f", *new)))
    old = list(struct.unpack_from("<3f", raw, inverse_at + 12 * 4))
    if not all(math.isfinite(v) for v in old):
        raise SystemExit(f"bone {bone_index}: non-finite stored inverse translation")
    if any(abs(a - b) > PRECISION for a, b in zip(old, new, strict=True)):
        raise SystemExit(f"bone {bone_index}: stored inverse translation {old} is not a precision variant of {new}")
    struct.pack_into("<3f", raw, inverse_at + 12 * 4, *new)
    return {"boneIndex": bone_index, "oldInverseTranslation": old, "newInverseTranslation": new}


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
        rejected = {row["boneIndex"] for row in pose["rejections"]}
        if not rejected:
            continue
        member = skeleton["member"]
        names = {bone["name"]: index for index, bone in enumerate(skeleton["bones"])}
        targets = [names[name] for name in args.bone if name in names]
        if not rejected <= set(targets):
            raise SystemExit(f"{member}: rejections {sorted(rejected)} are not all selected bones")
        path = args.output.joinpath("resources", *member.replace("!/", "/").split("/"))
        blob = path.read_bytes()
        raw = bytearray(zlib.decompressobj(-15).decompress(blob[16:]))
        fixed = [
            repair_payload(raw, skeleton["header"], index, pose["bones"][index]["globalPose"])
            for index in sorted(rejected)
        ]
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        path.write_bytes(new_blob)
        row = rows[member]
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        repairs.append({"member": member, "bones": fixed})
    if not repairs:
        raise SystemExit("no pose rejections to repair")
    manifest["inversePrecisionRepairs"] = {
        "tool": "tools/repair_pc_inverse_precision.py",
        "reason": "stored inverse-bind translation off by > 1e-5 (exporter precision); recomputed from the bone",
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
