#!/usr/bin/env python3
"""Static collision from drawn geometry: placed map models that carry no collision -> one PS5 .pbn row.

Some PC map mods ship collision only for the ground. The GTA VI Map Port tw3 district is one: its
archetypes have flags 0 and physicsDictionary 0, none of its .ydr carries an embedded bound, and its
.ybn hold the terrain, trees and a few walls; most buildings (the small houses, the trailers) have no
collision at all and others (museum_a, the post office) only bullet-only children (type flags 0x2,
MAP_WEAPON, which a ped's mover capsule never tests). In game the player walks through them.

This tool gives such entities collision from what is drawn: for every entity of MAP.ymap.xml whose
archetype matches an --archetype pattern, the high-LOD render triangles of DRAWABLES/<archetype>.ydr
(decal and water shaders left out) are placed by the entity's position, rotation and scale and become
one BVH geometry child (triangles welded and linked, BVH leaves of at most 4 polygons, one material).
The children form one static composite with retail mover + weapon flags (--type-flags 0x3e, the
mod's own ground children; --include 0x07f3bec0, the include word convert_pc_bounds.py merge writes),
translated like the merged rows (--translate) and written as a .pbn row for build_runtime_pack.py
--bounds. Every field is set the way convert_pc_bounds.py writes a converted child (the same
normalize/fit/encode path; the output is decoded and checked before it is written). The composite and
BVH class tags come from --template (repeatable): retail bounds of your own game, as convert_pc_bounds.py
takes them (./menu-ctl.sh fetch-templates caches them under bounds-templates/).

The stored CEntityDef quaternion of an ordinary entity is the inverse of its world orientation
(gtavmenu_tools.ymap): a model vertex v lands at position + conj(q) * (v scaled by scaleXY, scaleZ).

Inputs are third-party data: the drawables go through convert_pc_drawable.Source (bounded reader),
the map through gtavmenu_tools.ymap. Standard library only (run with `python3 -I`).

  drawable_collision.py MAP.ymap.xml --drawables DIR --archetype 'tw3_01_smallhouse*' ... \\
      --template bounds-templates/hut05_closed_1.pbn --output OUT.pbn [--translate DX DY DZ] [--material 54] \\
      [--report OUT.json] [--force]
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import convert_pc_bounds as cb  # noqa: E402
from gtavmenu_tools.asset_formats import AssetError  # noqa: E402
from gtavmenu_tools.hashes import joaat  # noqa: E402
from gtavmenu_tools.ymap import read_ymap  # noqa: E402

TYPE_FLAGS = 0x3E  # MAP_WEAPON | MAP_DYNAMIC (mover) | MAP_ANIMAL | MAP_COVER | MAP_VEHICLE
INCLUDE_FLAGS = 0x07F3BEC0
MARGIN = 0.005  # every BVH child of the mod's .ybn (and the retail converted rows)
LEAF_POLYGONS = 4  # retail/PC geometry BVH leaves hold 1..4 polygons
MIN_AREA = 1e-6  # square metres; smaller triangles (after quantisation) are dropped as degenerate
# Shaders whose surfaces are overlays (decals: coplanar with a solid surface) or water: never collision.
SKIP_SHADERS = (
    "decal",
    "decal_dirt",
    "decal_emissive_only",
    "decal_emissivenight_only",
    "decal_glue",
    "decal_normal_only",
    "decal_shadow_only",
    "decal_spec_only",
    "decal_tnt",
    "normal_decal",
    "normal_decal_pxm",
    "normal_decal_pxm_tnt",
    "normal_decal_tnt",
    "normal_spec_decal",
    "normal_spec_decal_detail",
    "normal_spec_decal_nopuddle",
    "normal_spec_decal_pxm",
    "normal_spec_decal_tnt",
    "spec_decal",
    "water_decal",
    "water_fountain",
    "water_poolenv",
    "water_river",
    "water_riverfoam",
    "water_riverlod",
    "water_riverocean",
    "water_rivershallow",
    "water_shallow",
    "water_terrainfoam",
)

Vec = tuple[float, float, float]
Triangle = tuple[Vec, Vec, Vec]


# --------------------------------------------------------------------------------------------------
# placement


def world_rotation(stored: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    """The world orientation of an ordinary ymap entity: the conjugate of the stored quaternion."""
    x, y, z, w = stored
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if not math.isfinite(norm) or norm < 1e-6:
        raise AssetError("entity rotation is not a quaternion")
    return (-x / norm, -y / norm, -z / norm, w / norm)


def rotate(q: tuple[float, float, float, float], v: Vec) -> Vec:
    """q * v * conj(q) for a unit quaternion (x, y, z, w)."""
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (
        v[0] + w * tx + (y * tz - z * ty),
        v[1] + w * ty + (z * tx - x * tz),
        v[2] + w * tz + (x * ty - y * tx),
    )


def place(triangles: list[Triangle], position: Vec, stored_rotation, scale_xy: float = 1.0, scale_z: float = 1.0):
    """Model-space triangles -> world triangles of one entity."""
    q = world_rotation(stored_rotation)
    px, py, pz = position
    out = []
    for tri in triangles:
        moved = []
        for v in tri:
            x, y, z = rotate(q, (v[0] * scale_xy, v[1] * scale_xy, v[2] * scale_z))
            moved.append((x + px, y + py, z + pz))
        out.append(tuple(moved))
    return out


# --------------------------------------------------------------------------------------------------
# one BVH geometry child from world triangles


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _area(a: Vec, b: Vec, c: Vec) -> float:
    u = [b[k] - a[k] for k in range(3)]
    v = [c[k] - a[k] for k in range(3)]
    cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return 0.5 * math.sqrt(sum(x * x for x in cross))


def _tree(items: list[tuple[tuple[int, ...], tuple[int, ...]]]) -> tuple[list[int], list[tuple[int, ...]]]:
    """(polygon order, depth-first nodes) of a BVH over int16 item boxes: median split on the longest
    axis of the box centres, leaves of at most LEAF_POLYGONS consecutive items (in the new order);
    a branch's item word is its subtree size, as convert_pc_bounds.composite_bvh writes."""
    order: list[int] = []
    nodes: list[tuple[int, ...] | None] = []

    def box(group: list[int]) -> tuple[int, ...]:
        return tuple(min(items[i][0][k] for i in group) for k in range(3)) + tuple(
            max(items[i][1][k] for i in group) for k in range(3)
        )

    def build(group: list[int]) -> None:
        if len(group) <= LEAF_POLYGONS:
            nodes.append((*box(group), len(order), len(group)))
            order.extend(group)
            return
        at = len(nodes)
        nodes.append(None)
        mids = {i: [items[i][0][k] + items[i][1][k] for k in range(3)] for i in group}
        spans = [max(mids[i][k] for i in group) - min(mids[i][k] for i in group) for k in range(3)]
        axis = spans.index(max(spans))
        ordered = sorted(group, key=lambda i: (mids[i][axis], i))
        half = len(ordered) // 2
        build(ordered[:half])
        build(ordered[half:])
        nodes[at] = (*box(group), len(nodes) - at, 0)

    build(list(range(len(items))))
    return order, nodes  # type: ignore[return-value]


def _headers(nodes: list[tuple[int, ...]]) -> list[tuple[int, ...]]:
    """Subtree headers of at most cb.TREE_SPAN nodes (composite_bvh's rule)."""
    out: list[tuple[int, ...]] = []

    def visit(index: int) -> None:
        node = nodes[index]
        size = node[6] if node[7] == 0 else 1
        if size <= cb.TREE_SPAN:
            out.append((*node[:6], index, index + size))
            return
        left = index + 1
        visit(left)
        visit(left + (nodes[left][6] if nodes[left][7] == 0 else 1))

    visit(0)
    return out


def bvh_child(triangles: list[Triangle], material: int = 0, tags: dict[int, int] | None = None) -> cb.Bound:
    """One BVH geometry child (world space) holding `triangles`: vertices quantised over the
    triangles' box (int16 x quantum + centre, quantum = half extent / 32767), identical quantised
    vertices welded, degenerate triangles dropped, polygons in BVH leaf order, one material."""
    if not triangles:
        raise AssetError("no triangles for a collision child")
    pts = [v for tri in triangles for v in tri]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    centre = [_f32((a + b) / 2) for a, b in zip(lo, hi, strict=True)]
    half = [max((b - a) / 2, 1e-3) for a, b in zip(lo, hi, strict=True)]
    quantum = [_f32(h / 32767.0) for h in half]
    index: dict[tuple[int, int, int], int] = {}
    ints: list[tuple[int, int, int]] = []

    def vertex(p: Vec) -> int:
        key = tuple(max(-32767, min(32767, round((p[k] - centre[k]) / quantum[k]))) for k in range(3))
        if key not in index:
            index[key] = len(ints)
            ints.append(key)  # type: ignore[arg-type]
        return index[key]

    def world(i: int) -> Vec:
        return tuple(_f32(ints[i][k] * quantum[k] + centre[k]) for k in range(3))  # type: ignore[return-value]

    polys: list[tuple[int, int, int, float]] = []
    for tri in triangles:
        a, b, c = (vertex(p) for p in tri)
        if len({a, b, c}) < 3:
            continue
        area = _area(world(a), world(b), world(c))
        if area >= MIN_AREA:
            polys.append((a, b, c, area))
    if not polys:
        raise AssetError("every triangle of the collision child is degenerate")
    if len(ints) > cb.LIMITS["vertices"] or len(polys) > cb.LIMITS["polygons"]:
        raise AssetError(f"{len(ints)} vertices / {len(polys)} polygons: split the child (<= 65535 each)")
    items = [
        (
            tuple(min(ints[v][k] for v in poly[:3]) for k in range(3)),
            tuple(max(ints[v][k] for v in poly[:3]) for k in range(3)),
        )
        for poly in polys
    ]
    order, nodes = _tree(items)
    if len(nodes) > cb.LIMITS["nodes"]:
        raise AssetError(f"{len(nodes)} BVH nodes (<= {cb.LIMITS['nodes']})")
    records = b""
    for j in order:
        a, b, c, area = polys[j]
        word = struct.unpack("<I", struct.pack("<f", area))[0] & ~7  # low 3 bits: polygon type 0 (triangle)
        records += struct.pack("<I3H3H", word, a, b, c, 0xFFFF, 0xFFFF, 0xFFFF)
    vlo = [min(world(i)[k] for i in range(len(ints))) for k in range(3)]
    vhi = [max(world(i)[k] for i in range(len(ints))) for k in range(3)]
    fields = bytearray(cb.SIZES[cb.BVH])
    struct.pack_into("<II", fields, 0, _tag(cb.TAGS if tags is None else tags, cb.BVH), 1)
    fields[0x10] = cb.BVH
    struct.pack_into("<f", fields, 0x14, _f32(math.dist(vlo, vhi) / 2))
    struct.pack_into("<3ff", fields, 0x20, *vhi, MARGIN)
    struct.pack_into("<3fI", fields, 0x30, *vlo, 1)
    struct.pack_into("<3f", fields, 0x40, *centre)
    struct.pack_into("<3f", fields, 0x50, *centre)
    struct.pack_into("<4f", fields, 0x60, 1.0, 1.0, 1.0, 1.0)
    struct.pack_into("<I", fields, 0x84, len(ints))
    struct.pack_into("<3f", fields, 0x90, *quantum)
    struct.pack_into("<3f", fields, 0xA0, *centre)
    struct.pack_into("<II", fields, 0xD0, len(ints), len(polys))
    fields[0x120] = 1
    struct.pack_into("<I", fields, 0x140, 0xFFFF)
    bound = cb.Bound(cb.BVH, fields)
    bound.arrays = {
        "shrunk": None,
        "polygons": records,
        "materials": struct.pack("<II", material & 0xFF, 0) + bytes(24),  # padded to 4 entries (as the PC children)
        "material_colours": None,
        "vertices": b"".join(struct.pack("<3h", *v) for v in ints),
        "vertex_colours": None,
        "poly_materials": bytes(len(polys)),
    }
    vectors = bytearray(0x60)
    inverse = [_f32(32767.0 / h) for h in half]
    for k, vec in enumerate((vlo, vhi, centre, inverse, quantum)):
        struct.pack_into("<3fI", vectors, 0x10 + 0x10 * k, *vec, cb.NAN_W)
    packed = b"".join(struct.pack("<8h", *node) for node in nodes)
    trees = b"".join(struct.pack("<8h", *t) for t in _headers(nodes))
    bound.tree = cb.Tree(bytes(vectors), packed, len(nodes), trees)
    return bound


def _tag(tags: dict[int, int], kind: int) -> int:
    if kind not in tags:
        raise AssetError(
            f"no class tag for {cb.NAMES[kind]} bounds: pass a --template that has one (a retail .pbn with a"
            " composite and a BVH child; ./menu-ctl.sh fetch-templates caches them under bounds-templates/)"
        )
    return tags[kind]


def chunks(triangles: list[Triangle], limit: int = 20000) -> list[list[Triangle]]:
    """Runs of at most `limit` triangles (at most 3 x limit vertices: within one child's 65535)."""
    return [triangles[i : i + limit] for i in range(0, len(triangles), limit)] or [[]]


# --------------------------------------------------------------------------------------------------
# the composite row


def build(
    groups: list[list[Triangle]],
    *,
    material: int = 0,
    delta: Vec = (0.0, 0.0, 0.0),
    type_flags: int = TYPE_FLAGS,
    include_flags: int = INCLUDE_FLAGS,
    tags: dict[int, int] | None = None,
) -> tuple[bytes, dict]:
    """World-space triangle groups (one child each) -> (.pbn bytes, report). `tags` (bound type -> class
    tag, convert_pc_bounds.read_tags of the --template bounds) defaults to convert_pc_bounds.TAGS."""
    tags = cb.TAGS if tags is None else tags
    children = []
    notes: list[str] = []
    for k, group in enumerate(groups):
        child = bvh_child(group, material, tags)
        stats = cb.weld_and_link(child)
        notes += [f"child {k}: {n}" for n in cb.fit_geometry(child)]
        if stats["degenerate_triangles"]:
            raise AssetError(f"child {k}: degenerate triangles after the weld")
        children.append(child)
    if not 0 < len(children) <= cb.LIMITS["children"]:
        raise AssetError(f"{len(children)} children (1..{cb.LIMITS['children']})")
    fields = bytearray(cb.SIZES[cb.COMPOSITE])
    struct.pack_into("<II", fields, 0, _tag(tags, cb.COMPOSITE), 1)
    fields[0x10] = cb.COMPOSITE
    struct.pack_into("<I", fields, 0x3C, 1)
    struct.pack_into("<HH", fields, 0xA0, len(children), len(children))
    root = cb.Bound(cb.COMPOSITE, fields, children=children)
    identity = b"".join(struct.pack("<3fI", *[float(r == k) for k in range(3)], cb.MATRIX_W[r]) for r in range(4))
    root.matrices, root.matrices2, root.matrices_alias = identity * len(children), None, True
    root.child_flags, root.child_flags2, root.flags_alias = bytes(8 * len(children)), None, True
    model = cb.Resource(root, b"", {})
    cb.normalize(model, include_flags, type_flags, tags)
    cb.translate(model, delta)
    cb.normalize(model, tags=tags)
    cb.composite_mass(model.root)
    model.root.tree = cb.composite_bvh(model.root) if len(children) >= cb.COMPOSITE_BVH_MIN else None
    out = cb.encode(model)
    readback = cb.decode(out)
    summary = cb.check(readback)
    if cb.diff(cb.semantic(readback), cb.semantic(model)):
        raise AssetError("readback differs from the model written")
    polygons = sum(c.u("<I", 0xD4) for c in readback.root.children)
    return out, {
        "children": len(children),
        "polygons": polygons,
        "vertices": sum(c.u("<I", 0xD0) for c in readback.root.children),
        "compositeBvh": model.root.tree is not None,
        "flags": f"{type_flags:#x}/{include_flags:#x}",
        "material": material,
        "box": [readback.root.vec(0x30), readback.root.vec(0x20)],
        "notes": notes,
        "summary": summary,
        "bytes": len(out),
    }


# --------------------------------------------------------------------------------------------------
# drawables


def drawable_triangles(blob: bytes, c: dict, skip: set[int]) -> tuple[list[Triangle], dict]:
    """Model-space triangles of a PC .ydr's high LOD (triangle lists), without the geometries whose
    shader name hash is in `skip`."""
    import convert_pc_drawable as drawable
    from gtavmenu_tools.asset_formats import Limits

    source = drawable.Source(blob, c, Limits(), graphics=True)
    source.buffer_pointer_wins = True  # only the buffer's own data is read (Map Builder props: stale copy)
    shaders = source.materials()["shaders"]
    geometry = source.geometry(len(shaders))
    out: list[Triangle] = []
    stats = {"geometries": 0, "skippedGeometries": 0, "triangles": 0}
    for row in source.meshes(geometry):
        if row["lod"] != "High":
            continue
        stats["geometries"] += 1
        if int(shaders[row["shaderIndex"]]["nameHash"], 16) in skip:
            stats["skippedGeometries"] += 1
            continue
        if not row["triangleDeclarationAgrees"]:
            raise AssetError("a high-LOD geometry is not a triangle list")
        position = [p for p in row["components"] if p["semantic"] == "Position"]
        if len(position) != 1 or position[0]["type"] != "Float3":
            raise AssetError("a high-LOD geometry has no Float3 position")
        offset, stride = position[0]["offset"], row["vertexStride"]
        base = row["vertexStreams"][0]["systemOffset"]
        verts = [
            struct.unpack_from("<3f", source.system, base + i * stride + offset) for i in range(row["vertexCount"])
        ]
        if not all(math.isfinite(x) for v in verts for x in v):
            raise AssetError("a vertex position is not finite")
        at, count = row["indices"]["systemOffset"], row["indexCount"]
        indices = struct.unpack_from(f"<{count}H", source.system, at)
        for i in range(0, count - count % 3, 3):
            out.append((verts[indices[i]], verts[indices[i + 1]], verts[indices[i + 2]]))
    stats["triangles"] = len(out)
    return out, stats


def archetype_name(entity, exact: dict[int, str]) -> str:
    """The entity's archetype name: the map's own (CodeWalker XML), else the --archetype name without
    wildcards that hashes to it (a binary .ymap stores only the hash)."""
    return entity.name or exact.get(entity.archetype, "")


def main(argv: list[str] | None = None, default_tags: dict[int, int] | None = None) -> int:
    """CLI. `default_tags` is a developer hook (class tags used without --template); the published tool has
    none, so it needs --template."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("map", type=Path, help="the map (.ymap.xml or .ymap) whose entities get collision")
    parser.add_argument("--drawables", type=Path, required=True, help="folder of the PC <archetype>.ydr")
    parser.add_argument("--archetype", action="append", required=True, help="archetype name pattern (fnmatch)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--translate", nargs=3, type=float, default=(0.0, 0.0, 0.0), metavar=("DX", "DY", "DZ"))
    parser.add_argument("--material", type=lambda s: int(s, 0), default=0, help="material index (0..255)")
    parser.add_argument("--type-flags", type=lambda s: int(s, 0), default=TYPE_FLAGS)
    parser.add_argument("--include", type=lambda s: int(s, 0), default=INCLUDE_FLAGS)
    parser.add_argument("--keep-shader", action="append", default=[], help="collide a shader SKIP_SHADERS lists")
    parser.add_argument(
        "--template",
        action="append",
        type=Path,
        default=[],
        help="retail bounds of your own game supplying the composite and BVH class tags (.pbn; repeatable)",
    )
    parser.add_argument("--reference-dir", type=Path, help="accepted and not read (layouts: data/drawable_contracts)")
    parser.add_argument("--max-bytes", type=int, default=8 << 20)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    if not 0 <= args.material <= 255:
        parser.error("--material must be 0..255")
    if args.template:
        try:
            tags = cb.read_tags(args.template)
        except cb.BoundsError as error:
            raise SystemExit(f"error: {error}") from None
    elif default_tags is not None:
        tags = dict(default_tags)
    else:
        raise SystemExit(
            "error: needs --template: a retail .pbn with a composite and a BVH child from your own game"
            " (./menu-ctl.sh fetch-templates caches them under bounds-templates/)"
        )
    for kind in (cb.COMPOSITE, cb.BVH):
        if kind not in tags:
            raise SystemExit(f"error: no --template carries a {cb.NAMES[kind]} bound (its class tag is needed)")

    import convert_pc_drawable as drawable

    c = drawable.contracts(args.reference_dir)
    skip = {joaat(name) for name in SKIP_SHADERS if name not in args.keep_shader}
    ymap = read_ymap(args.map.read_bytes())
    meshes: dict[str, tuple[list[Triangle], dict]] = {}
    groups: list[list[Triangle]] = []
    rows = []
    exact = {joaat(p.lower()): p.lower() for p in args.archetype if not any(c in p for c in "*?[")}
    for e in ymap.entities:
        name = archetype_name(e, exact)
        if e.mlo or not any(fnmatch.fnmatchcase(name, p) for p in args.archetype):
            continue
        if name not in meshes:
            path = args.drawables / f"{name}.ydr"
            if not path.is_file():
                rows.append({"archetype": name, "skipped": "no drawable"})
                meshes[name] = ([], {})
                continue
            meshes[name] = drawable_triangles(path.read_bytes(), c, skip)
        local, stats = meshes[name]
        if not local:
            rows.append({"archetype": name, "skipped": "no triangles", **stats})
            continue
        world = place(local, e.position, e.rotation, e.scale_xy, e.scale_z)
        for part in chunks(world):
            rows.append({"archetype": name, "child": len(groups), "position": list(e.position), **stats})
            groups.append(part)
    if not groups:
        raise SystemExit("no entity matched --archetype with a drawable")
    blob, report = build(
        groups,
        material=args.material,
        delta=tuple(args.translate),
        type_flags=args.type_flags,
        include_flags=args.include,
        tags=tags,
    )
    if len(blob) > args.max_bytes:
        raise SystemExit(f"{len(blob)} bytes > --max-bytes {args.max_bytes}: select fewer archetypes per row")
    if args.output.exists() and not args.force:
        raise SystemExit(f"refusing to overwrite {args.output} (--force)")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    report["entities"] = rows
    report["translate"] = list(args.translate)
    if args.report:
        args.report.write_text(json.dumps(report, indent=1, default=list) + "\n")
    print(
        f"wrote {args.output} ({len(blob)} bytes, {report['children']} children, {report['polygons']} polygons,"
        f" {sum(1 for r in rows if 'skipped' in r)} entities skipped)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
