#!/usr/bin/env python3
"""Recompute imprecise stored bone matrices in an imported PC fixture from the bone's own pose.

The wheelchair add-on (iak_wheelchair.yft and _hi) stores the handlebar grip bones hbgrip_l/hbgrip_r
with local matrices whose rotation entries are ~1.4e-5 off the matrix of the bone's own quaternion,
translation and scale (scale 0.9999924), and inverse-bind matrices off by as much. The converter's pose
gate (tools/pc_transforms_pose.py, 1e-5 tolerance) rejects both comparisons. This is the
rotation counterpart of repair_pc_inverse_precision.py (inverse translation only): for each selected
rejected bone the stored local matrix's twelve affine entries are set to the pose gate's local pose and
the stored inverse's twelve affine entries to the exact inverse of the gate's global pose (float64,
then float32). Column four of both matrices (M14, M24, M34, M44) is left as stored. Only a precision
repair is accepted: every rewritten value must lie within 1e-4 of the stored one, so a bone whose
matrices disagree with its pose for any other reason (mirror, wrong rotation) is refused. The repaired
fixture must pass the pose walk; the original fixture stays intact and the manifest records the change.

  repair_pc_pose_precision.py --fixture build/assets/convert-<id>/fixtures/iak_wheelchair-import \\
      --material-report <its material report> --bone hbgrip_l --bone hbgrip_r --output <new fixture>

--rejected selects every bone the pose gate rejects instead of a --bone list (convert_vehicle.py --repair
auto): the Gta5KoRn gmt400's boot and windscreen_r (8.5e-6 local, 2.6e-5 inverse). A mirrored seat or any
other larger disagreement is still refused by the 1e-4 bound (repair_pc_seat_mirror.py handles mirrors).
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
AFFINE = [i for i in range(16) if i % 4 != 3]  # row-major M11..M44 without column four (M14, M24, M34, M44)


def affine_inverse(m: list[float]) -> list[float]:
    """Inverse of a row-vector affine matrix [[R, 0], [t, 1]] (row-major 16 floats): [[R^-1, 0], [-t R^-1, 1]]."""
    if len(m) != 16 or not all(math.isfinite(m[i]) for i in AFFINE):
        raise SystemExit("bone global pose has non-finite affine entries")
    (a, b, c), (d, e, f), (g, h, i) = (m[0:3], m[4:7], m[8:11])
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-6:
        raise SystemExit("bone global pose is singular")
    r = [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]
    t = [-sum(m[12 + k] * r[k][j] for k in range(3)) for j in range(3)]
    return [*r[0], 0.0, *r[1], 0.0, *r[2], 0.0, *t, 1.0]


def repair_matrix(raw: bytearray, at: int, target: list[float], label: str) -> float:
    """Write target's affine entries (float32) over the stored matrix at `at`; returns the largest change."""
    stored = list(struct.unpack_from("<16f", raw, at))
    if not all(math.isfinite(stored[i]) and math.isfinite(target[i]) for i in AFFINE):
        raise SystemExit(f"{label}: non-finite affine matrix entry")
    new = list(stored)
    for index in AFFINE:
        new[index] = struct.unpack("<f", struct.pack("<f", target[index]))[0]
    change = max(abs(stored[i] - new[i]) for i in AFFINE)
    if change > PRECISION:
        raise SystemExit(
            f"{label}: stored matrix differs from the pose by {change:.3g} > {PRECISION}; not a precision error"
        )
    struct.pack_into("<16f", raw, at, *new)
    return change


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    bones = parser.add_mutually_exclusive_group(required=True)
    bones.add_argument("--bone", action="append")
    bones.add_argument(
        "--rejected",
        action="store_true",
        help="every bone the pose gate rejects (repair_pc_auto.py); the 1e-4 precision bound still refuses others",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)

    pc = inspect(args.fixture, args.material_report)
    skeletons = pc["freshSkinningEvidence"]["freshSkeletonEvidence"]["resources"]
    rows = {row["member"]: row for row in manifest["resources"]}
    planned = []
    for skeleton, pose in zip(skeletons, pc["resources"], strict=True):
        rejected = {row["boneIndex"] for row in pose["rejections"]}
        if not rejected:
            continue
        member = skeleton["member"]
        names = {bone["name"]: index for index, bone in enumerate(skeleton["bones"])}
        targets = rejected if args.rejected else {names[name] for name in args.bone if name in names}
        if not rejected <= targets:
            raise SystemExit(f"{member}: rejections {sorted(rejected)} are not all selected bones")
        planned.append((member, skeleton["header"], pose, sorted(rejected)))
    if not planned:
        raise SystemExit("no pose rejections to repair")
    copy_fixture(args.fixture, args.output, manifest)
    repairs = []
    for member, header, pose, indices in planned:
        path = args.output.joinpath("resources", *member.replace("!/", "/").split("/"))
        blob = path.read_bytes()
        raw = bytearray(zlib.decompressobj(-15).decompress(blob[16:]))
        fixed = []
        for index in indices:
            bone = pose["bones"][index]
            local_at = header["TransformationsPointer"] - SYSTEM_BASE + index * MATRIX_BYTES
            inverse_at = header["TransformationsInvertedPointer"] - SYSTEM_BASE + index * MATRIX_BYTES
            label = f"{member} bone {index} ({bone['name']})"
            local = repair_matrix(raw, local_at, bone["localPose"], label + " local")
            inverse = repair_matrix(raw, inverse_at, affine_inverse(bone["globalPose"]), label + " inverse")
            fixed.append({"boneIndex": index, "name": bone["name"], "localChange": local, "inverseChange": inverse})
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        path.write_bytes(new_blob)
        row = rows[member]
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        repairs.append({"member": member, "bones": fixed})
    manifest["posePrecisionRepairs"] = {
        "tool": "tools/repair_pc_pose_precision.py",
        "reason": "stored bone matrices off the bone's own pose by > 1e-5 (exporter precision); recomputed",
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
