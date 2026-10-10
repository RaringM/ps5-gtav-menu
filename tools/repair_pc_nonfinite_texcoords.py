#!/usr/bin/env python3
"""Zero non-finite texture coordinates in an imported PC vehicle fixture.

The Gta5KoRn pack's gmt400 (Chevrolet Suburban) stores NaN in TexCoord1 of some vertices. The
converter's mesh gate (tools/pc_mesh_buffers.py inspect_float_stream) refuses every
non-finite vertex scalar ("mesh TexCoord1 contains a non-finite scalar at vertex N"), so every step
that reads the mesh stops. A NaN texture coordinate draws nothing meaningful on PC either (the sampler
gets NaN). This tool runs the converter's own geometry walk (skeletons stage, read-only, reviewed
format contracts, as repair_pc_triangle_counts.py does), records each vertex stream whose TexCoord*
components hold a NaN or infinity, and writes a new fixture in which exactly those scalars are 0.0.
Refused when a non-finite value sits in any other component (Position, Normal, Tangent, ...: no
safe value), or when a recorded stream cannot be found exactly in its member's payload. The encoded
payload is checked byte-for-byte; ordinary conversion still checks the full graph.
The original stays intact and the manifest records every change.

  repair_pc_nonfinite_texcoords.py --fixture build/assets/convert-<id>/fixtures/gmt400-import \\
      --material-report <its material report> --output <new fixture>   (convert_vehicle.py --repair nonfinite-texcoords)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pc_mesh_buffers as mesh  # noqa: E402
from gtavmenu_tools.asset_formats import decode_resource  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402
from repair_pc_triangle_counts import probe_dir  # noqa: E402

TEXCOORD = "TexCoord"


def sanitize(raw: bytes, count: int, stride: int, components: list, contract: dict) -> tuple[bytes, list[dict]]:
    """(stream with non-finite TexCoord scalars set to 0.0, the changed scalars); refuses other components."""
    out, changes = bytearray(raw), []
    for component in components:
        form = contract.get(component["type"])
        if form is None or component["type"] in ("Colour", "UByte4"):
            continue
        for vertex in range(count):
            at = vertex * stride + component["offset"]
            for index in range(form["scalarCount"]):
                (scalar,) = struct.unpack_from("<f", out, at + 4 * index)
                if math.isfinite(scalar):
                    continue
                if not component["semantic"].startswith(TEXCOORD):
                    raise SystemExit(f"non-finite {component['semantic']} at vertex {vertex}: no safe value")
                struct.pack_into("<f", out, at + 4 * index, 0.0)
                changes.append({"vertex": vertex, "semantic": component["semantic"], "scalar": index})
    return bytes(out), changes


def collect(fixture: Path, material_report: Path, probe_output: Path, templates: Path) -> list[dict]:
    """Walk the skeletons stage; every vertex stream with non-finite TexCoord scalars, sanitized in flight."""
    import convert_pc_vehicle as converter

    found: list[dict] = []
    original = mesh.inspect_float_stream

    def wrapped(raw, count, stride, components, contract):
        clean, changes = sanitize(bytes(raw), count, stride, components, contract)
        if changes and all(row["raw"] != bytes(raw) for row in found):  # a stream walked twice counts once
            found.append({"raw": bytes(raw), "clean": clean, "changes": changes})
        return original(clean, count, stride, components, contract)

    mesh.inspect_float_stream = wrapped
    argv = [
        "convert_pc_vehicle.py", "skeletons",
        "--pc-fixture", str(fixture),
        "--material-report", str(material_report),
        "--templates", str(templates),
        "--target", str(ROOT / "data/targets/ppsa04264-01.010.002.json"),
        "--native-reference", str(templates / "corpus/tornado6.pft"),
        "--output", str(probe_output),
    ]  # fmt: skip
    saved = sys.argv
    sys.argv = argv
    try:
        converter.main()
    except SystemExit:
        pass
    finally:
        sys.argv = saved
        mesh.inspect_float_stream = original
    return found


def verify_reencoded(blob: bytes, repaired: bytes) -> None:
    """Reopen the compressed output and require every byte of the repaired payload."""
    _, decoded = decode_resource(blob, 256 * 1024 * 1024)
    if decoded != repaired:
        raise SystemExit("encoded resource does not preserve the repaired vertex streams")


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
        raise SystemExit("no non-finite texture coordinates found")
    copy_fixture(args.fixture, args.output, manifest)
    changes, patched = [], set()
    for row in manifest["resources"]:
        if not row["member"].endswith(".yft"):
            continue
        path = args.output.joinpath("resources", *row["member"].replace("!/", "/").split("/"))
        blob = path.read_bytes()
        _, payload = decode_resource(blob, 256 * 1024 * 1024)
        raw = bytearray(payload)
        streams = 0
        for index, stream in enumerate(found):
            at = raw.find(stream["raw"])
            while at >= 0:  # every copy (the same bytes, so the same repair)
                raw[at : at + len(stream["raw"])] = stream["clean"]
                patched.add(index)
                streams += 1
                changes.append(
                    {
                        "member": row["member"],
                        "payloadOffset": at,
                        "bytes": len(stream["raw"]),
                        "scalars": len(stream["changes"]),
                        "semantics": sorted({c["semantic"] for c in stream["changes"]}),
                    }
                )
                at = raw.find(stream["raw"], at + len(stream["raw"]))
        if not streams:
            continue
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        verify_reencoded(new_blob, bytes(raw))
        path.write_bytes(new_blob)
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
    if len(patched) != len(found):
        shutil.rmtree(args.output)
        raise SystemExit("a vertex stream with non-finite values was not found in any member payload")
    manifest["nonfiniteTexcoordRepairs"] = {
        "tool": "tools/repair_pc_nonfinite_texcoords.py",
        "reason": "NaN/infinite TexCoord scalars set to 0.0 (the mesh gate refuses non-finite vertex values)",
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "streams": changes,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(
        json.dumps({"output": str(args.output), "streams": len(changes), "scalars": sum(c["scalars"] for c in changes)})
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None
