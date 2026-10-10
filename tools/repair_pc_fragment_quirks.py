#!/usr/bin/env python3
"""Repair bounded exporter quirks in an imported PC vehicle fixture.

The public converter's references walk detects five admitted shapes: empty octant
item pointers, glass rows without data, identity/NaN placeholder fragment matrices,
shatter maps exactly one column short, and inverted BVH leaf bounds. Every other
reader refusal remains active. Recorded source page hashes and old field values
must match before a new fixture is written. Branch bounds and non-placeholder
matrices are never repaired.

Clearing an empty octant pointer only admits conversion. The converted resource
must subsequently pass native_octants.py, which fills each empty octant with the
bound's full vertex list; an empty native octant is unsafe at runtime.

Placeholder translations up to FRAG_TRANSLATION_LIMIT are recorded explicitly.
The source is unchanged. Verified retail inputs come from --templates; no game
executable, frozen reports, or emulator is used.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import inspect
import io
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import native_collision  # noqa: E402
import native_drawables  # noqa: E402
import native_fragment  # noqa: E402
from gtavmenu_tools.host_paths import build_dir  # noqa: E402
from gtavmenu_tools.vehicle_repair_inputs import FixtureError, copy_fixture, validate_fixture  # noqa: E402
from repair_pc_triangle_counts import probe_dir  # noqa: E402

FRAG_MATRIX_FIELDS = (
    "FragMatricesIndsPointer",
    "FragMatricesIndsCount",
    "FragMatricesCapacity",
    "FragMatricesPointer",
    "FragMatricesCount",
)
IDENTITY_ROWS = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, 0.0))
# An exporter's float noise on the placeholder (KoRn mk6pol: translation z -2.98e-7, the rest exact): still no transform.
IDENTITY_TOLERANCE = 1e-6
# The Prowler bike's placeholder matrix is identity with a 3.7 cm translation (0.0049, 0, 0.0364) that every
# model index points at; a translation up to this bound (metres) is dropped with the arrays and recorded.
FRAG_TRANSLATION_LIMIT = 0.1
DROPPED_TRANSLATIONS: list[list[float]] = []  # translations of the placeholder matrices dropped by the last walk

ORIGINAL_LOOP = """                for count, target in zip(arrays["OctantsPointer"]["values"], ptrs["values"], strict=True):
                    items = integers(target["value"], count["value"], "octant-items")"""
PATCHED_LOOP = """                for _slot, (count, target) in enumerate(
                    zip(arrays["OctantsPointer"]["values"], ptrs["values"], strict=True)
                ):
                    if count["value"] == 0 and target["value"]:
                        _FOUND.append(
                            (hashlib.sha256(system).hexdigest(), h["OctantItemsPointer"] + _slot * 8 - base,
                             8, target["value"])
                        )
                        node["octants"].append(None)
                        continue
                    items = integers(target["value"], count["value"], "octant-items")"""


ORIGINAL_GLASS = """        elif values["ItemDataCount"]:
            raise AssetError("fragment glass row count without data is unsupported")"""
PATCHED_GLASS = """        elif values["ItemDataCount"]:
            _field = contract["window"]["fields"]["ItemDataCount"]
            _FOUND.append(
                (hashlib.sha256(system).hexdigest(), data_start - contract["window"]["bytes"] + _field["offset"],
                 _field["bytes"], values["ItemDataCount"])
            )"""


# 5. BVH leaves with swapped bounds (Rockport Police Coquette, rpdcar5: one composite child's quantized box
#    has MinX 3805 > MaxX -3990): the reader refuses "collision BVH node bounds inverted". For a leaf
#    (ItemCount > 0) the min and max of each inverted axis are swapped back; an inverted branch still refuses.
ORIGINAL_BVH = """                if any(row["Min" + a] > row["Max" + a] for a in "XYZ"):
                    raise AssetError("collision BVH node bounds inverted")"""
PATCHED_BVH = """                if any(row["Min" + a] > row["Max" + a] for a in "XYZ"):
                    if row["ItemCount"] <= 0:
                        raise AssetError("collision BVH node bounds inverted")
                    _node = structs["BVHNode_s"]
                    _at = arrays["Nodes.EntriesPointer"]["sourcePointer"] - base + i * _node["bytes"]
                    for _axis in "XYZ":
                        if row["Min" + _axis] > row["Max" + _axis]:
                            for _name, _new in (("Min" + _axis, row["Max" + _axis]), ("Max" + _axis, row["Min" + _axis])):
                                _f = _node["fields"][_name]
                                _FOUND.append(
                                    (hashlib.sha256(system).hexdigest(), _at + _f["offset"], _f["bytes"],
                                     row[_name] & 0xFFFF, _new & 0xFFFF)
                                )"""


# 4. Shatter maps one column narrower than their rows (Dominator GTX, 6 windows): every full row is
#    [0, ShatterMapWidth] with ShatterMapWidth + 1 data bytes, so the width field is one short. The walk
#    records the width a row needs (end + 1); a row more than one column past the width still refuses.
ORIGINAL_WIDTH = """                    if last >= values["ShatterMapWidth"]:
                        raise AssetError("fragment glass shatter interval exceeds width")"""
PATCHED_WIDTH = """                    if last > values["ShatterMapWidth"]:
                        raise AssetError("fragment glass shatter interval exceeds width")
                    if last == values["ShatterMapWidth"]:
                        _width = contract["window"]["fields"]["ShatterMapWidth"]
                        _FOUND.append(
                            (hashlib.sha256(system).hexdigest(), data_start - contract["window"]["bytes"]
                             + _width["offset"], _width["bytes"], values["ShatterMapWidth"], last + 1)
                        )"""
ORIGINAL_WIDTH2 = """                        if n2 <= 0 or row["end2"] >= values["ShatterMapWidth"]:
                            raise AssetError("fragment glass second shatter interval invalid")"""
PATCHED_WIDTH2 = """                        if n2 <= 0 or row["end2"] > values["ShatterMapWidth"]:
                            raise AssetError("fragment glass second shatter interval invalid")
                        if row["end2"] == values["ShatterMapWidth"]:
                            _width = contract["window"]["fields"]["ShatterMapWidth"]
                            _FOUND.append(
                                (hashlib.sha256(system).hexdigest(), data_start - contract["window"]["bytes"]
                                 + _width["offset"], _width["bytes"], values["ShatterMapWidth"], row["end2"] + 1)
                            )"""


def _patched(module, name: str, original, patched, found: list):
    """MODULE.NAME recompiled with each ORIGINAL text replaced by its PATCHED text (str or parallel tuples)."""
    source = inspect.getsource(getattr(module, name))
    pairs = zip(
        (original,) if isinstance(original, str) else original,
        (patched,) if isinstance(patched, str) else patched,
        strict=True,
    )
    for old, new in pairs:
        if source.count(old) != 1:
            raise SystemExit(f"{module.__name__}.{name} changed; refusing to patch")
        source = source.replace(old, new)
    namespace = dict(vars(module))
    namespace["_FOUND"] = found
    namespace["hashlib"] = hashlib
    exec(compile(source, module.__file__, "exec"), namespace)
    return namespace[name]


def placeholder_frag_matrices(system: bytes, base: int, header: dict) -> bool:
    """True when the drawable's FragMatrices carry no transform: zero indices (or indices below the
    count), identity matrices with zero translation (fourth column ignored: NaN padding) and unused
    all-NaN matrices, every array inside the system pages."""
    count, capacity, index_count = (
        header["FragMatricesCount"],
        header["FragMatricesCapacity"],
        header["FragMatricesIndsCount"],
    )
    indices_at, matrices_at = header["FragMatricesIndsPointer"] - base, header["FragMatricesPointer"] - base
    if not (header["FragMatricesIndsPointer"] and header["FragMatricesPointer"]) or not 0 < count <= capacity <= 256:
        return False
    if index_count > 256 or min(indices_at, matrices_at) < 0:
        return False
    if indices_at + index_count * 8 > len(system) or matrices_at + capacity * 64 > len(system):
        return False
    if any(i >= count for i in struct.unpack_from(f"<{index_count}Q", system, indices_at)):
        return False
    moved = []
    for i in range(capacity):
        m = struct.unpack_from("<16f", system, matrices_at + 64 * i)
        rows = tuple(tuple(m[4 * r : 4 * r + 3]) for r in range(4))
        near = all(
            abs(a - b) <= IDENTITY_TOLERANCE for row, want in zip(rows, IDENTITY_ROWS, strict=True)
            for a, b in zip(row, want, strict=True)
        )  # fmt: skip
        if near or all(math.isnan(x) for x in m):
            continue
        if rows[:3] == IDENTITY_ROWS[:3] and math.hypot(*rows[3]) <= FRAG_TRANSLATION_LIMIT:
            moved.append([round(v, 6) for v in rows[3]])
            continue
        return False
    DROPPED_TRANSLATIONS.extend(t for t in moved if t not in DROPPED_TRANSLATIONS)
    return True


def _drawable_reader(saved, found: list):
    """native_drawables.read that records and nulls placeholder FragMatrices, then reads normally."""

    def read(system, base, root, contract):
        layout = contract["drawable"]
        header, _ = native_drawables.typed(system, root, layout)
        others = [n for n in native_drawables.ZERO_EXTENSION if n not in FRAG_MATRIX_FIELDS and header[n]]
        if (
            any(header[n] for n in FRAG_MATRIX_FIELDS)
            and not others
            and not header["DrawableModelsBlocksSize"]
            and placeholder_frag_matrices(system, base, header)
        ):
            system_sha = hashlib.sha256(system).hexdigest()
            patched = bytearray(system)
            for name in FRAG_MATRIX_FIELDS:
                field = layout["fields"][name]
                at = root + field["offset"]
                found.append((system_sha, at, field["bytes"], header[name]))
                patched[at : at + field["bytes"]] = bytes(field["bytes"])
            system = bytes(patched)
        return saved(system, base, root, contract)

    return read


WALK_STOPPED: list[str] = []  # the converter refusal that ended the last collect() walk early, if any


def collect(argv: list[str]) -> list[tuple]:
    """Run the converter with the quirk rejections recorded instead of raised."""
    import convert_pc_vehicle as converter

    found: list[tuple] = []  # (system sha256, offset, size, old value[, new value]); no new value = 0
    saved_read, saved_glass = native_collision.read, native_fragment.read_glass
    saved_drawable = native_drawables.read
    native_collision.read = _patched(
        native_collision, "read", (ORIGINAL_LOOP, ORIGINAL_BVH), (PATCHED_LOOP, PATCHED_BVH), found
    )
    native_fragment.read_glass = _patched(
        native_fragment,
        "read_glass",
        (ORIGINAL_GLASS, ORIGINAL_WIDTH, ORIGINAL_WIDTH2),
        (PATCHED_GLASS, PATCHED_WIDTH, PATCHED_WIDTH2),
        found,
    )
    native_drawables.read = _drawable_reader(saved_drawable, found)
    saved = sys.argv
    sys.argv = argv
    errors = io.StringIO()
    try:
        with contextlib.redirect_stderr(errors):
            status = converter.main()
        if status:  # the converter printed its refusal and returned 1: the walk did not reach the end
            lines = errors.getvalue().strip().splitlines() or ["(no message)"]
            WALK_STOPPED.append(lines[-1])
    except SystemExit as stop:
        print(f"walk ended: {stop}", file=sys.stderr)
    finally:
        sys.stderr.write(errors.getvalue())
        sys.argv = saved
        native_collision.read, native_fragment.read_glass = saved_read, saved_glass
        native_drawables.read = saved_drawable
    return sorted(set(found))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--material-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--templates", type=Path, default=build_dir(ROOT) / "retail-templates", help="verified retail template cache"
    )
    args = parser.parse_args(argv)
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit(f"refusing to overwrite {args.output}")
    manifest = validate_fixture(args.fixture)
    WALK_STOPPED.clear()
    DROPPED_TRANSLATIONS.clear()
    templates = args.templates
    argv = [
        "convert_pc_vehicle.py",
        "references",
        "--pc-fixture", str(args.fixture),
        "--material-report", str(args.material_report),
        "--templates", str(templates),
        "--target", str(ROOT / "data/targets/ppsa04264-01.010.002.json"),
        "--native-reference", str(templates / "corpus/tornado6.pft"),
    ]  # fmt: skip
    with probe_dir(args.output) as probe:
        found = collect([*argv, "--output", str(probe)])
    if WALK_STOPPED and "single-page resource budget" in WALK_STOPPED[-1]:
        WALK_STOPPED.clear()
        DROPPED_TRANSLATIONS.clear()
        with probe_dir(args.output) as probe:
            found = collect([*argv, "--output", str(probe), "--multi-page"])
    if not found:
        # A walk the converter stopped early (another quirk first, e.g. triangle counts on physics
        # children) has not seen every fragment: say so; repair_pc_auto.py runs this again next round.
        stopped = f" (walk stopped before the end: {WALK_STOPPED[-1]})" if WALK_STOPPED else ""
        raise SystemExit(f"no quirks found{stopped}")
    copy_fixture(args.fixture, args.output, manifest)
    changes = []
    for row in manifest["resources"]:
        if not row["member"].endswith(".yft"):
            continue
        path = args.output.joinpath("resources", *row["member"].replace("!/", "/").split("/"))
        blob = path.read_bytes()
        raw = bytearray(zlib.decompressobj(-15).decompress(blob[16:]))
        mine = {}
        for system_sha, offset, size, value, *new in found:
            # Both readers see the system pages, which start at payload offset 0.
            if any(hashlib.sha256(bytes(raw[:n])).hexdigest() == system_sha for n in _system_sizes(blob)):
                # a field recorded with new values (shatter widths: one per row) takes the largest
                key = (offset, size, value)
                mine[key] = max(mine.get(key, 0), *new) if new else 0
        if not mine:
            continue
        for (offset, size, value), new_value in sorted(mine.items()):
            fmt = {2: "<H", 4: "<I", 8: "<Q"}[size]
            if struct.unpack_from(fmt, raw, offset)[0] != value:
                raise SystemExit(f"{row['member']}: field {offset:#x} does not hold {value:#x}")
            struct.pack_into(fmt, raw, offset, new_value)
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        new_blob = blob[:16] + compressor.compress(bytes(raw)) + compressor.flush()
        path.write_bytes(new_blob)
        row["bytes"] = row["storedBytes"] = len(new_blob)
        row["sha256"] = hashlib.sha256(new_blob).hexdigest()
        changes.append(
            {
                "member": row["member"],
                "fields": [
                    {"offset": o, "bytes": s, "old": v, **({"new": n} if n else {})}
                    for (o, s, v), n in sorted(mine.items())
                ],
            }
        )
    manifest["fragmentQuirkRepairs"] = {
        "tool": "tools/repair_pc_fragment_quirks.py",
        "reason": (
            "octant item pointer non-null with count 0 (nulled for the reader; native_octants.py fills the"
            " octant after conversion); glass window rows without shatter data; placeholder fragment"
            " matrices (identity/NaN only, zero indices) dropped"
        ),
        "sourceFixtureManifestSha256": hashlib.sha256((args.fixture / "manifest.json").read_bytes()).hexdigest(),
        "resources": changes,
    }
    if DROPPED_TRANSLATIONS:  # placeholder matrices that were identity plus a small translation (Prowler)
        manifest["fragmentQuirkRepairs"]["fragMatrixTranslationsDropped"] = DROPPED_TRANSLATIONS
        print(f"placeholder fragment matrices: translations dropped (metres): {DROPPED_TRANSLATIONS}")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "repaired": len(found), "resources": [c["member"] for c in changes]}))
    return 0


def _system_sizes(blob: bytes) -> list[int]:
    """System page byte sizes the PC header may describe (Legacy v162 page flags)."""
    from gtavmenu_tools.asset_formats import resource_header

    header = resource_header(blob)
    return [header["systemBytes"]]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FixtureError as error:
        raise SystemExit(f"fixture refused: {error}") from None
