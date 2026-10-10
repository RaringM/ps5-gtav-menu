#!/usr/bin/env python3
"""Check and repair octant vertex maps in polyhedron collision bounds.

An empty octant is replaced by an existing list containing every vertex of the
same bound. Only its count and pointer change. Missing full lists, invalid vertex
indices and pointers outside the system pages are refused. Clean resources remain
byte-identical. Reads loose RSC7 resources and plain-table RPF7 archives; callers
may supply a table-decryption callback through the Python API.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools import rpf7  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError, decode_resource, page_bytes  # noqa: E402
from gtavmenu_tools.meta_resource import stored_deflate  # noqa: E402

SYSTEM_BASE = 0x50000000
GEOMETRY_TYPE = 4
OCTANTS = 8
GEOMETRY_BYTES = 0x130
TYPE, SHRUNK, SHRUNK_COUNT, POLYGONS, QUANTUM = 0x10, 0x78, 0x84, 0x88, 0x90
VERTICES, COUNTS, ITEMS, VERTEX_COUNT, POLYGON_COUNT = 0xB0, 0xC0, 0xC8, 0xD0, 0xD4
VERTEX_BYTES, POLYGON_BYTES = 6, 16
LIMIT = 1 << 31
# Problems the engine cannot survive or that give a wrong support point.
PROBLEMS = {
    "empty-null": "count 0 with a NULL list: the engine reads 0xfffffffffffffffc",
    "empty-stale": "count 0 with a non-null list: the engine reads list[-1], outside the list",
    "arrays-disagree": "only one of the counts/items pointers is set",
    "array-outside": "counts or items pointer array is misaligned or outside the system pages",
    "list-outside": "list pointer is NULL, misaligned or [list, list + 4*count) leaves the system pages",
    "index-outside": "a list entry is not a vertex index (>= VerticesCount)",
}


class OctantError(ValueError):
    pass


def _inside(size: int, pointer: int, length: int, alignment: int) -> bool:
    at = pointer - SYSTEM_BASE
    return pointer % alignment == 0 and at >= 0 and at + length <= size


def _u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def _u64(data, at):
    return struct.unpack_from("<Q", data, at)[0]


def _is_geometry(system: bytes, at: int) -> bool:
    size = len(system)
    if at + GEOMETRY_BYTES > size or system[at + TYPE] != GEOMETRY_TYPE:
        return False
    vertices, polygons = _u32(system, at + VERTEX_COUNT), _u32(system, at + POLYGON_COUNT)
    if not 0 < vertices <= 32768 or not 0 < polygons <= 32767:
        return False
    if _u32(system, at + SHRUNK_COUNT) not in (0, vertices):
        return False
    quantum = struct.unpack_from("<3f", system, at + QUANTUM)
    if not all(math.isfinite(q) and q > 0 for q in quantum):
        return False
    shrunk = _u64(system, at + SHRUNK)
    return (
        _inside(size, _u64(system, at + VERTICES), vertices * VERTEX_BYTES, 2)
        and (not shrunk or _inside(size, shrunk, vertices * VERTEX_BYTES, 2))
        and _inside(size, _u64(system, at + POLYGONS), polygons * POLYGON_BYTES, 4)
    )


def geometry_bounds(system: bytes) -> list[int]:
    """System offsets of every phBoundGeometry that some pointer in the system pages targets."""
    size = len(system)
    count = size // 8
    targets = set()
    for value in struct.unpack_from(f"<{count}Q", system):
        if SYSTEM_BASE <= value < SYSTEM_BASE + size and value % 16 == 0:
            targets.add(value - SYSTEM_BASE)
    return sorted(at for at in targets if _is_geometry(system, at))


def octant_map(system: bytes, at: int) -> dict | None:
    counts_pointer, items_pointer = _u64(system, at + COUNTS), _u64(system, at + ITEMS)
    if not counts_pointer and not items_pointer:
        return None
    row = {"bound": at, "vertices": _u32(system, at + VERTEX_COUNT), "countsPointer": counts_pointer}
    row["itemsPointer"] = items_pointer
    size = len(system)
    if not counts_pointer or not items_pointer:
        row["problem"] = "arrays-disagree"
        return row
    if not _inside(size, counts_pointer, 4 * OCTANTS, 4) or not _inside(size, items_pointer, 8 * OCTANTS, 8):
        row["problem"] = "array-outside"
        return row
    row["counts"] = list(struct.unpack_from(f"<{OCTANTS}I", system, counts_pointer - SYSTEM_BASE))
    row["pointers"] = list(struct.unpack_from(f"<{OCTANTS}Q", system, items_pointer - SYSTEM_BASE))
    return row


def _list(system: bytes, pointer: int, count: int) -> tuple[int, ...]:
    return struct.unpack_from(f"<{count}I", system, pointer - SYSTEM_BASE)


def problems(system: bytes, bounds: list[int] | None = None) -> list[dict]:
    """Every octant the engine would read outside a valid list (see PROBLEMS)."""
    found = []
    for at in geometry_bounds(system) if bounds is None else bounds:
        row = octant_map(system, at)
        if row is None:
            continue
        if "problem" in row:
            found.append({"bound": at, "octant": None, "problem": row["problem"]})
            continue
        for octant, (count, pointer) in enumerate(zip(row["counts"], row["pointers"], strict=True)):
            problem = None
            if count == 0:
                problem = "empty-stale" if pointer else "empty-null"
            elif not pointer or not _inside(len(system), pointer, 4 * count, 4):
                problem = "list-outside"
            elif max(_list(system, pointer, count)) >= row["vertices"]:
                problem = "index-outside"
            if problem:
                found.append({"bound": at, "octant": octant, "count": count, "pointer": pointer, "problem": problem})
    return found


def plan(system: bytes, bounds: list[int] | None = None) -> list[dict]:
    """Field changes that give each empty octant an existing full vertex list of its bound."""
    changes = []
    for at in geometry_bounds(system) if bounds is None else bounds:
        row = octant_map(system, at)
        if row is None or "problem" in row or 0 not in row["counts"]:
            continue
        everything = set(range(row["vertices"]))
        full = [
            octant
            for octant, (count, pointer) in enumerate(zip(row["counts"], row["pointers"], strict=True))
            if count == row["vertices"]
            and pointer
            and _inside(len(system), pointer, 4 * count, 4)
            and set(_list(system, pointer, count)) == everything
        ]
        if not full:
            raise OctantError(f"bound {at:#x}: empty octant and no octant list holds all {row['vertices']} vertices")
        source = full[0]
        for octant, count in enumerate(row["counts"]):
            if count:
                continue
            changes.append(
                {
                    "bound": at,
                    "octant": octant,
                    "countOffset": row["countsPointer"] - SYSTEM_BASE + 4 * octant,
                    "pointerOffset": row["itemsPointer"] - SYSTEM_BASE + 8 * octant,
                    "oldCount": 0,
                    "oldPointer": row["pointers"][octant],
                    "newCount": row["counts"][source],
                    "newPointer": row["pointers"][source],
                    "sharedFrom": source,
                }
            )
    return changes


def apply(system: bytearray, changes: list[dict]) -> None:
    for change in changes:
        if _u32(system, change["countOffset"]) != change["oldCount"]:
            raise OctantError(f"count field {change['countOffset']:#x} changed before the repair")
        if _u64(system, change["pointerOffset"]) != change["oldPointer"]:
            raise OctantError(f"pointer field {change['pointerOffset']:#x} changed before the repair")
        struct.pack_into("<I", system, change["countOffset"], change["newCount"])
        struct.pack_into("<Q", system, change["pointerOffset"], change["newPointer"])


def changed_spans(old: bytes, new: bytes) -> list[tuple[int, int]]:
    """[start, end) runs of differing bytes (equal lengths)."""
    if len(old) != len(new):
        raise OctantError("payload length changed")
    spans, start = [], None
    for i, (a, b) in enumerate(zip(old, new, strict=True)):
        if a != b and start is None:
            start = i
        elif a == b and start is not None:
            spans.append((start, i))
            start = None
    if start is not None:
        spans.append((start, len(old)))
    return spans


def repair_resource(blob: bytes) -> tuple[bytes, list[dict], int]:
    """(repaired RSC7 bytes, changes, geometry bound count); the input bytes when nothing changes."""
    header, payload = decode_resource(blob, LIMIT)
    size = header["systemBytes"]
    system = bytearray(payload[:size])
    bounds = geometry_bounds(system)
    changes = plan(system, bounds)
    if not changes:
        left = problems(system, bounds)
        if left:
            raise OctantError(f"octant problems without a repair: {left}")
        return blob, [], len(bounds)
    apply(system, changes)
    left = problems(system, bounds)
    if left:
        raise OctantError(f"octant problems remain after the repair: {left}")
    allowed = set()
    for change in changes:
        allowed.update(range(change["countOffset"], change["countOffset"] + 4))
        allowed.update(range(change["pointerOffset"], change["pointerOffset"] + 8))
    new_payload = bytes(system) + payload[size:]
    if any(i not in allowed for start, end in changed_spans(payload, new_payload) for i in range(start, end)):
        raise OctantError("repair touched bytes outside the octant count/pointer fields")
    if blob[16:] == stored_deflate(payload):  # keep stored blocks: only the field bytes change
        out = blob[:16] + stored_deflate(new_payload)
    else:  # like copy_drawable_lods.py
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        out = blob[:16] + compressor.compress(new_payload) + compressor.flush()
    if decode_resource(out, LIMIT)[1] != new_payload:
        raise OctantError("repaired resource does not read back")
    return out, changes, len(bounds)


def _system_from_member(stored: bytes, flags: int, label: str) -> bytes:
    return rpf7.inflate_resource(stored, label)[: page_bytes(flags)]


def resources(path: Path, decrypt=None) -> list[tuple[str, bytes]]:
    """(label, system pages) of a loose RSC7 file or of every resource member of an RPF7.

    A keyed archive table needs `decrypt` (rpf7.TableCipher); plain tables never use it.
    """
    blob = path.read_bytes()
    if blob[:4] == b"RSC7":
        header, payload = decode_resource(blob, LIMIT)
        return [(str(path), payload[: header["systemBytes"]])]
    return _archive(blob, path.name, str(path), decrypt)


def _archive(blob: bytes, basename: str, label: str, decrypt=None) -> list[tuple[str, bytes]]:
    entries, _ = rpf7.read_table(blob, basename, decrypt)
    out = []
    for entry in entries:
        if entry["kind"] == "res":
            stored = blob[entry["offset"] : entry["offset"] + entry["size"]]
            out.append((f"{label}/{entry['name']}", _system_from_member(stored, entry["a"], entry["name"])))
        elif entry["kind"] == "bin" and entry["name"].endswith(".rpf"):
            nested = blob[entry["offset"] : entry["offset"] + entry["size"]]
            out += _archive(nested, entry["name"], f"{label}/{entry['name']}", decrypt)
    return out


def check_paths(paths: list[Path], decrypt=None) -> list[dict]:
    rows = []
    for path in paths:
        try:
            found = resources(path, decrypt)
        except (rpf7.Rpf7Error, AssetError) as error:  # keyed table, not a resource, ...
            rows.append({"resource": str(path), "skipped": str(error), "geometryBounds": 0, "problems": []})
            continue
        for label, system in found:
            bounds = geometry_bounds(system)
            found = problems(system, bounds)
            rows.append({"resource": label, "geometryBounds": len(bounds), "problems": found})
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="report octant problems (exit 1 when any)")
    check.add_argument("paths", type=Path, nargs="+")
    check.add_argument("--json", action="store_true")
    repair = sub.add_parser("repair", help="fill empty octants from a full list of the same bound")
    repair.add_argument("--member", action="append", required=True, metavar="IN=OUT")
    args = parser.parse_args(argv)
    if args.command == "check":
        rows = check_paths(args.paths)
        if args.json:
            print(json.dumps(rows, indent=1))
        else:
            for row in rows:
                if "skipped" in row:
                    print(f"skip {row['resource']}: {row['skipped']}")
                if not row["geometryBounds"]:
                    continue
                state = "FAIL" if row["problems"] else "ok"
                kinds = sorted({p["problem"] for p in row["problems"]})
                print(f"{state:4} {row['resource']}: {row['geometryBounds']} geometry bounds", end="")
                print(f", {len(row['problems'])} bad octants ({', '.join(kinds)})" if kinds else "")
        return 1 if any(row["problems"] for row in rows) else 0
    pairs = []
    for spec in args.member:
        source, sep, target = spec.partition("=")
        if not sep or not source or not target:
            parser.error(f"--member {spec!r}: expected IN=OUT")
        pairs.append((Path(source), Path(target)))
    for _, target in pairs:
        if target.exists():
            raise SystemExit(f"refusing to overwrite {target}")
    summary = []
    for source, target in pairs:
        blob = source.read_bytes()
        try:
            out, changes, bounds = repair_resource(blob)
        except OctantError as error:
            raise SystemExit(f"{source}: {error}") from None
        target.parent.mkdir(parents=True, exist_ok=True)
        if out is blob:
            shutil.copyfile(source, target)
        else:
            target.write_bytes(out)
        summary.append(
            {
                "input": str(source),
                "output": str(target),
                "geometryBounds": bounds,
                "filled": [
                    {k: (f"{v:#x}" if "Pointer" in k or "Offset" in k or k == "bound" else v) for k, v in c.items()}
                    for c in changes
                ],
            }
        )
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
