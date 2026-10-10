#!/usr/bin/env python3
"""Fill zero triangle counts in an imported PC vehicle fixture.

The public converter walk records only triangle-list geometry (primitive 3) with
zero stored triangles and an index count divisible by three. Every other mismatch
is refused. The new fixture stores IndicesCount / 3, with exact source payload and
old-value checks. The source is unchanged; the manifest records each repair.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pc_mesh_buffers as mesh  # noqa: E402
from gtavmenu_tools.host_paths import assets_dir, build_dir  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402


def collect(fixture: Path, material_report: Path, probe_output: Path, templates: Path | None = None) -> list[dict]:
    """Run the skeletons stage read-only and record every triangle-count rejection."""
    import convert_pc_vehicle as converter

    templates = templates or build_dir(ROOT) / "retail-templates"
    found: list[dict] = []
    original = mesh.inspect

    def wrapped(payload, header, geometry, geometry_contract, contract, limits):
        result = original(payload, header, geometry, geometry_contract, contract, limits)
        if result["rejections"]:
            field = geometry_contract["geometry"]["fields"]["TrianglesCount"]
            items = {
                (lod["lod"], model["index"], item["index"]): item["systemOffset"]
                for lod in geometry["lods"]
                for model in lod["models"]
                for item in model["geometries"]
            }
            payload_sha = hashlib.sha256(bytes(payload)).hexdigest()
            for row in result["rejections"]:
                if (
                    row["code"] != "triangle-declaration-unqualified"
                    or row["storedPrimitiveWord"] != 3
                    or row["storedTriangleCount"] != 0
                    or row["indexCount"] % 3
                ):
                    raise SystemExit(f"unsupported mesh rejection: {row}")
                found.append(
                    {
                        "payloadSha256": payload_sha,
                        "fieldOffset": items[(row["lod"], row["modelIndex"], row["geometryIndex"])] + field["offset"],
                        "fieldBytes": field["bytes"],
                        "triangles": row["indexCount"] // 3,
                    }
                )
            result["rejections"] = []  # let the walk continue to the next child
        return result

    mesh.inspect = wrapped
    argv = [
        "convert_pc_vehicle.py",
        "skeletons",
        "--pc-fixture",
        str(fixture),
        "--material-report",
        str(material_report),
        "--templates",
        str(templates),
        "--target",
        str(ROOT / "data/targets/ppsa04264-01.010.002.json"),
        "--native-reference",
        str(templates / "corpus/tornado6.pft"),
        "--output",
        str(probe_output),
    ]
    saved = sys.argv
    sys.argv = argv
    try:
        converter.main()
    except SystemExit:
        pass
    finally:
        sys.argv = saved
        mesh.inspect = original
    return found


@contextlib.contextmanager
def probe_dir(output: Path):
    """Own a unique scratch tree; never delete a caller's preexisting probe output."""
    base = assets_dir(ROOT)
    base.mkdir(parents=True, exist_ok=True)
    if base.is_symlink():
        raise SystemExit("repair scratch root must be a real build/assets directory")
    with tempfile.TemporaryDirectory(prefix=f"repair-{output.name[:40]}-", dir=base) as temporary:
        yield Path(temporary) / "probe"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--templates",
        type=Path,
        default=build_dir(ROOT) / "retail-templates",
        help="retail templates (corpus/tornado6.pft) root",
    )
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    with probe_dir(args.output) as probe:
        found = collect(args.fixture, args.material_report, probe, args.templates)
    if not found:
        raise SystemExit("no triangle-count rejections found")
    copy_fixture(args.fixture, args.output, manifest)
    changes = []
    for row in manifest["resources"]:
        if not row["member"].endswith(".yft"):
            continue
        path = args.output.joinpath("resources", *row["member"].replace("!/", "/").split("/"))
        blob = path.read_bytes()
        raw = bytearray(zlib.decompressobj(-15).decompress(blob[16:]))
        sha = hashlib.sha256(bytes(raw)).hexdigest()
        mine = {(f["fieldOffset"], f["fieldBytes"], f["triangles"]) for f in found if f["payloadSha256"] == sha}
        if not mine:
            continue
        for offset, size, triangles in sorted(mine):
            if size != 4 or struct.unpack_from("<I", raw, offset)[0] != 0:
                raise SystemExit(f"{row['member']}: field at {offset:#x} is not a zero u32")
            struct.pack_into("<I", raw, offset, triangles)
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        path.write_bytes(new_blob)
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        changes.append({"member": row["member"], "geometries": len(mine)})
    manifest.setdefault("repairs", {})
    manifest["triangleCountRepairs"] = {
        "tool": "tools/repair_pc_triangle_counts.py",
        "reason": "TrianglesCount 0 on triangle-list geometry with index count divisible by 3",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "resources": changes,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "changes": changes, "rejectionsRepaired": len(found)}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None
