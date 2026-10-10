#!/usr/bin/env python3
"""PC collision (.ybn) -> PS5 static bounds (.pbn), and PS5 .pbn round trips.

Both files are RSC7 version 43 resources whose system pages hold the same phBound object graph. PS5
and PC differ only in authoring details, which `normalize()` sets the retail way:
- the object tag word at +0x00 (the PS5 class tag of each bound type, word +0x04 = 1);
- geometry +0x9c / +0xac (retail: margin / 2 / 32767 and margin / 2), Vector4 w words of the BVH
  block (retail 0x7f800001), composite child-matrix w words (retail 1,0,0,0), aliased matrix and
  flag arrays, BVH node capacity == count for geometry BVHs;
- page placement (retail: largest first, first fit; `place()`).
Vertices (int16 x quantum + centre), polygons (16-byte records, type in the low 3 bits of byte 0,
neighbour indices), materials (two u32, room id in bits 16..20), octants and BVH nodes keep their
PC encoding unchanged, so the conversion is a re-pack plus those fields.

The class tags are build-specific words of the game, so none ships with this tool: convert and merge
read them from --template, bounds taken from the user's own game (`./menu-ctl.sh fetch-templates`
caches them under bounds-templates/): a static .pbn (composite + BVH children) and a drawable that
embeds a composite bound at +0xC8 (box and geometry children). A bound type the output needs and no
template carries is refused.

The input is untrusted data: every pointer, count and index is bounds-checked; the graph must be a
tree (no shared or cyclic objects); unsupported bound types (sphere, capsule, cylinder, disc, cloth
children) are refused with an error that names the child.

  convert_pc_bounds.py inspect FILE                         # .ybn or .pbn: tree, counts, materials
  convert_pc_bounds.py roundtrip RETAIL.pbn [--output OUT]  # decode -> encode -> compare
  convert_pc_bounds.py convert IN.ybn --template T.pbn [--template T.pdr] --output OUT.pbn
                           [--translate DX DY DZ] [--force]
  convert_pc_bounds.py translate IN.pbn --output OUT.pbn --translate DX DY DZ
  convert_pc_bounds.py merge A.ybn B.ybn ... --template T.pbn [--template T.pdr] --output OUT.pbn
                           [--translate DX DY DZ] [--include 0x07f3bec0]
  convert_pc_bounds.py tags T.pbn [T.pdr ...]               # the class tags a template set supplies
  convert_pc_bounds.py compare A B                          # semantic comparison of two bound files
  convert_pc_bounds.py probe FILE X Y                       # surfaces a vertical line at (X, Y) crosses
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import argparse  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import struct  # noqa: E402
from collections import Counter  # noqa: E402
from collections.abc import Sequence  # noqa: E402
from dataclasses import dataclass, field  # noqa: E402

from gtavmenu_tools.asset_formats import AssetError, decode_resource  # noqa: E402
from gtavmenu_tools.bounds import View, composite_info  # noqa: E402
from gtavmenu_tools.meta_resource import stored_deflate  # noqa: E402
from make_pmap import PAGE_FIELDS, flags_for_pages, page_sizes  # noqa: E402

VERSION = 43
SYS = 0x50000000
SPHERE, CAPSULE, BOX, GEOMETRY, BVH, COMPOSITE, DISC, CYLINDER, CLOTH = 0, 1, 3, 4, 8, 10, 12, 13, 15
NAMES = {
    SPHERE: "sphere",
    CAPSULE: "capsule",
    BOX: "box",
    GEOMETRY: "geometry",
    BVH: "bvh",
    COMPOSITE: "composite",
    DISC: "disc",
    CYLINDER: "cylinder",
    CLOTH: "cloth",
}
SIZES = {BOX: 0x70, GEOMETRY: 0x130, BVH: 0x150, COMPOSITE: 0xB0}
# PS5 class tag word per bound type: read from --template (`read_tags()`); empty in this tool. A caller
# that imports the module may set it as the default for normalize()/convert()/merge().
TAGS: dict[int, int] = {}
POLY_TRIANGLE, POLY_SPHERE, POLY_CAPSULE, POLY_BOX, POLY_CYLINDER = 0, 1, 2, 3, 4
POLY_NAMES = ("tri", "sphere", "capsule", "box", "cylinder")
NAN_W = 0x7F800001  # retail Vector4 w word
MATRIX_W = (1, 0, 0, 0)  # retail composite child-matrix w words
PAGE_MIN = 0x2000
PAGE_MAX = 0x100000  # largest page retail .pbn files use (base 4 KiB)
COMPOSITE_BVH_MIN = 6  # retail: a composite carries a BVH exactly when it has 6 or more children
TREE_SPAN = 127  # nodes per BVH tree (subtree header) at most, as retail
MAX_MEMBER_BYTES = 0xFFFFFF  # an archive member's 24-bit size field
FILLER_NODE = struct.pack("<8h", 0, 0, 0, 0, 0, 0, 1, 0)  # retail composite BVH nodes past the count
LIMITS = {"children": 1024, "vertices": 65535, "polygons": 65535, "nodes": 65535, "trees": 4096, "materials": 255}
COMPOSITE_PTRS = (0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xA8)
GEOMETRY_PTRS = (0x78, 0x88, 0xB0, 0xB8, 0xC0, 0xC8, 0xF0, 0xF8, 0x118)
GEOMETRY_ARRAYS = (  # name, pointer field, element bytes; in retail gather (placement tie) order
    ("shrunk", 0x78, 6),
    ("polygons", 0x88, 16),
    ("materials", 0xF0, 8),
    ("material_colours", 0xF8, 4),
    ("vertices", 0xB0, 6),
    ("vertex_colours", 0xB8, 4),
    ("poly_materials", 0x118, 1),
)


class BoundsError(AssetError):
    """The input is outside what this converter reads or writes."""


@dataclass
class Tree:
    """The BVH block (0x80): nodes {int16 min[3], max[3], item, count}, trees {min, max, first, end}."""

    vectors: bytes  # block +0x10..+0x70: 16 unknown bytes, then min, max, centre, 1/quantum, quantum (Vector4)
    nodes: bytes  # capacity * 16
    node_count: int
    trees: bytes  # count * 16 (capacity == count)
    pad: int = 0  # block +0x7c

    @property
    def capacity(self) -> int:
        return len(self.nodes) // 16

    def vec(self, index: int) -> tuple[float, float, float]:
        return struct.unpack_from("<3f", self.vectors, 0x10 + 0x10 * index)


@dataclass
class Bound:
    kind: int
    fields: bytearray  # the whole object, pointer fields zeroed
    children: list[Bound] = field(default_factory=list)
    matrices: bytes | None = None
    matrices2: bytes | None = None  # None with matrices_alias: +0x80 points at +0x78's array
    matrices_alias: bool = True
    child_boxes: bytes | None = None
    child_flags: bytes | None = None
    child_flags2: bytes | None = None
    flags_alias: bool = True
    tree: Tree | None = None
    arrays: dict[str, bytes | None] = field(default_factory=dict)
    octants: list[bytes] | None = None  # 8 lists of u32 vertex indices

    def u(self, fmt: str, offset: int):
        return struct.unpack_from(fmt, self.fields, offset)[0]

    def vec(self, offset: int) -> tuple[float, float, float]:
        return struct.unpack_from("<3f", self.fields, offset)


@dataclass
class Resource:
    root: Bound
    pages_info: bytes  # the root's pages-info block (16 + 8 per page)
    header: dict
    source_pages: list[int] = field(default_factory=list)


# --------------------------------------------------------------------------------------------------
# decode


class _Reader:
    def __init__(self, payload: bytes, system: int):
        self.p = payload
        self.system = system
        self.claims: dict[int, tuple[int, str]] = {}

    def off(self, pointer: int, size: int, what: str, align: int = 1) -> int:
        if not SYS <= pointer < SYS + self.system:
            raise BoundsError(f"{what}: pointer {pointer:#x} outside the system pages")
        at = pointer - SYS
        if size < 0 or at + size > self.system or at % align:
            raise BoundsError(f"{what}: {size} bytes at {at:#x} outside the system pages or misaligned")
        return at

    def claim(self, pointer: int, size: int, what: str, align: int = 1) -> int:
        at = self.off(pointer, size, what, align)
        if at in self.claims:
            raise BoundsError(f"{what}: object at {at:#x} is referenced twice ({self.claims[at][1]})")
        self.claims[at] = (size, what)
        return at

    def bytes_at(self, pointer: int, size: int, what: str, align: int = 1) -> bytes:
        at = self.claim(pointer, size, what, align)
        return bytes(self.p[at : at + size])

    def u(self, fmt: str, offset: int):
        return struct.unpack_from(fmt, self.p, offset)[0]


def _finite(values, what: str) -> None:
    if not all(math.isfinite(v) for v in values):
        raise BoundsError(f"{what} is not finite")


def _read_tree(r: _Reader, pointer: int, what: str) -> Tree:
    at = r.claim(pointer, 0x80, what, 16)
    nodes_ptr, count, capacity = struct.unpack_from("<QII", r.p, at)
    trees_ptr, tcount, tcap, pad = struct.unpack_from("<QHHI", r.p, at + 0x70)
    if any(r.p[at + 0x10 : at + 0x20]):
        raise BoundsError(f"{what}: unknown words +0x10..+0x1f are not zero")
    if not 0 < count <= capacity <= LIMITS["nodes"]:
        raise BoundsError(f"{what}: node count {count} / capacity {capacity}")
    if not 0 < tcount == tcap <= LIMITS["trees"]:
        raise BoundsError(f"{what}: tree count {tcount} / capacity {tcap}")
    vectors = bytes(r.p[at + 0x10 : at + 0x70])
    for k in range(5):
        _finite(struct.unpack_from("<3f", vectors, 0x10 + 0x10 * k), f"{what} vector {k}")
    nodes = r.bytes_at(nodes_ptr, 16 * capacity, f"{what} nodes", 2)
    trees = r.bytes_at(trees_ptr, 16 * tcount, f"{what} trees", 2)
    return Tree(vectors, nodes, count, trees, pad)


def _read_bound(r: _Reader, pointer: int, path: str, depth: int, is_root: bool) -> Bound:
    if depth > 4:
        raise BoundsError(f"{path}: composites nested too deep")
    probe = r.off(pointer, 0x70, path, 16)
    kind = r.p[probe + 0x10]
    if kind not in SIZES:
        raise BoundsError(
            f"{path}: {NAMES.get(kind, f'type {kind}')} bounds are not supported"
            " (supported: composite, bvh, geometry, box)"
        )
    at = r.claim(pointer, SIZES[kind], f"{path} ({NAMES[kind]})", 16)
    raw = bytearray(r.p[at : at + SIZES[kind]])
    bound = Bound(kind, raw)
    pages = struct.unpack_from("<Q", raw, 0x08)[0]
    if bool(pages) != is_root:
        raise BoundsError(f"{path}: pages-info pointer {'missing on the root' if is_root else 'on a child'}")
    raw[0x08:0x10] = bytes(8)
    _finite(struct.unpack_from("<f3ff3f3f", raw, 0x14)[0:1] + bound.vec(0x20) + bound.vec(0x30), f"{path} box")
    if any(a > b for a, b in zip(bound.vec(0x30), bound.vec(0x20), strict=True)):
        raise BoundsError(f"{path}: box min above max")
    if kind == COMPOSITE:
        _read_composite(r, bound, path, depth)
    elif kind in (GEOMETRY, BVH):
        _read_geometry(r, bound, path)
    return bound


def _read_composite(r: _Reader, bound: Bound, path: str, depth: int) -> None:
    raw = bound.fields
    ptrs = struct.unpack_from("<6Q", raw, 0x70)
    count1, count2 = struct.unpack_from("<HH", raw, 0xA0)
    tree_ptr = struct.unpack_from("<Q", raw, 0xA8)[0]
    for offset in COMPOSITE_PTRS:
        raw[offset : offset + 8] = bytes(8)
    if not 0 < count2 <= LIMITS["children"] or count1 != count2:
        raise BoundsError(f"{path}: child counts +0xa0={count1} +0xa2={count2}")
    n = count2
    children, mats, mats2, boxes, flags, flags2 = ptrs
    if not children:
        raise BoundsError(f"{path}: composite without a children array")
    table = r.bytes_at(children, 8 * n, f"{path} children", 8)
    for i in range(n):
        child = struct.unpack_from("<Q", table, 8 * i)[0]
        if not child:
            raise BoundsError(f"{path}: child {i} is NULL")
        bound.children.append(_read_bound(r, child, f"{path}/{i}", depth + 1, False))
    if mats:
        bound.matrices = r.bytes_at(mats, 0x40 * n, f"{path} matrices", 16)
    bound.matrices_alias = mats2 == mats
    if mats2 and mats2 != mats:
        bound.matrices2 = r.bytes_at(mats2, 0x40 * n, f"{path} matrices 2", 16)
    if boxes:
        bound.child_boxes = r.bytes_at(boxes, 0x20 * n, f"{path} child boxes", 16)
    if flags:
        bound.child_flags = r.bytes_at(flags, 8 * n, f"{path} child flags", 4)
    bound.flags_alias = flags2 == flags
    if flags2 and flags2 != flags:
        bound.child_flags2 = r.bytes_at(flags2, 8 * n, f"{path} child flags 2", 4)
    if tree_ptr:
        bound.tree = _read_tree(r, tree_ptr, f"{path} composite bvh")


def _read_geometry(r: _Reader, bound: Bound, path: str) -> None:
    raw = bound.fields
    nv, npoly = struct.unpack_from("<II", raw, 0xD0)
    nshrunk = struct.unpack_from("<I", raw, 0x84)[0]
    nmat, ncol = raw[0x120], raw[0x121]
    if not 0 < nv <= LIMITS["vertices"] or not 0 < npoly <= LIMITS["polygons"]:
        raise BoundsError(f"{path}: {nv} vertices / {npoly} polygons")
    if nshrunk not in (0, nv):
        raise BoundsError(f"{path}: shrunk vertex count {nshrunk} != {nv}")
    _finite(struct.unpack_from("<3f", raw, 0x90) + struct.unpack_from("<3f", raw, 0xA0), f"{path} quantum/centre")
    if any(q <= 0 for q in struct.unpack_from("<3f", raw, 0x90)):
        raise BoundsError(f"{path}: quantum not positive")
    counts = {
        "shrunk": nv,
        "polygons": npoly,
        "vertices": nv,
        "vertex_colours": nv,
        "materials": max(nmat, 4),
        "material_colours": ncol,
        "poly_materials": npoly,
    }
    pointers = {name: struct.unpack_from("<Q", raw, offset)[0] for name, offset, _ in GEOMETRY_ARRAYS}
    octant_counts, octant_items = struct.unpack_from("<QQ", raw, 0xC0)
    tree_ptr = struct.unpack_from("<Q", raw, 0x130)[0] if bound.kind == BVH else 0
    for offset in GEOMETRY_PTRS + ((0x130,) if bound.kind == BVH else ()):
        raw[offset : offset + 8] = bytes(8)
    for name, _offset, size in GEOMETRY_ARRAYS:
        pointer = pointers[name]
        if not pointer:
            if name in ("polygons", "vertices", "materials", "poly_materials"):
                raise BoundsError(f"{path}: {name} array missing")
            bound.arrays[name] = None
            continue
        if counts[name] == 0:
            raise BoundsError(f"{path}: {name} pointer set with count 0")
        bound.arrays[name] = r.bytes_at(pointer, size * counts[name], f"{path} {name}", 2 if size == 6 else 1)
    if bool(octant_counts) != bool(octant_items):
        raise BoundsError(f"{path}: octant count and item pointers disagree")
    if octant_counts:
        block_counts = struct.unpack_from("<8I", r.p, r.off(octant_counts, 32, f"{path} octant counts", 4))
        item_ptrs = struct.unpack_from("<8Q", r.p, r.off(octant_items, 64, f"{path} octant items", 8))
        lists = []
        for k in range(8):
            if not 0 <= block_counts[k] <= nv:
                raise BoundsError(f"{path}: octant {k} count {block_counts[k]}")
            if not block_counts[k] or not item_ptrs[k]:  # the a80 crash pattern; check()/normalize() handle it
                lists.append(b"")
                continue
            at = r.off(item_ptrs[k], 4 * block_counts[k], f"{path} octant {k}", 4)
            lists.append(bytes(r.p[at : at + 4 * block_counts[k]]))
        bound.octants = lists
    if bound.kind == BVH:
        if not tree_ptr:
            raise BoundsError(f"{path}: BVH bound without a BVH block")
        bound.tree = _read_tree(r, tree_ptr, f"{path} bvh")


def decode(blob: bytes) -> Resource:
    """Decode a .pbn / .ybn (RSC7 v43, composite or single bound root) into the model."""
    try:
        header, payload = decode_resource(blob, 1 << 28)
    except AssetError as exc:
        raise BoundsError(str(exc)) from exc
    if header["version"] != VERSION:
        raise BoundsError(f"resource version {header['version']} is not {VERSION}")
    if header["graphicsBytes"]:
        raise BoundsError("bounds resource with graphics pages")
    system = header["systemBytes"]
    r = _Reader(payload, system)
    pages_ptr = struct.unpack_from("<Q", payload, 0x08)[0]
    root = _read_bound(r, SYS, "root", 0, True)
    npages = r.p[r.off(pages_ptr, 16, "pages info") + 8]
    pages_info = r.bytes_at(pages_ptr, 16 + 8 * npages, "pages info")
    return Resource(root, pages_info, header, page_sizes(int(header["systemFlags"], 16)))


# --------------------------------------------------------------------------------------------------
# class tags, from bounds of the user's own game


def _template_root(view: View) -> int:
    """Payload offset of the composite of a .pbn (its root) or of a drawable (the bound at +0xC8)."""
    if view.payload[0x10] == COMPOSITE:
        return 0
    bound = view.u("<Q", 0xC8) if len(view.payload) > 0xD0 else 0
    if bound and view.payload[view.off(bound) + 0x10] == COMPOSITE:
        return view.off(bound)
    raise BoundsError("neither a composite .pbn nor a drawable with an embedded composite bound")


def read_tags(paths: Sequence[Path]) -> dict[int, int]:
    """Class tag per bound type (composite, box, geometry, bvh) found in the composites of `paths`:
    .pbn files or drawables embedding a composite bound, from the user's own game. Every tag of a
    type must agree (a different game build is refused); types none of them carries are absent."""
    tags: dict[int, int] = {}
    for path in paths:
        try:
            view = View(path.read_bytes())
            root = _template_root(view)
            found = [(COMPOSITE, root)] + [
                (child["type"], child["offset"])
                for child in composite_info(view, root)["children"]
                if child["pointer"] and child["type"] in SIZES and child["type"] != COMPOSITE
            ]
            words = [(kind, *struct.unpack_from("<II", view.payload, at)) for kind, at in found]
        except BoundsError as exc:
            raise BoundsError(f"template {path.name}: {exc}") from exc
        except (AssetError, ValueError, IndexError, struct.error, OSError) as exc:
            raise BoundsError(f"template {path.name}: not a readable bounds resource ({exc})") from exc
        for kind, tag, word in words:
            if not tag or word != 1:
                raise BoundsError(
                    f"template {path.name}: {NAMES[kind]} bound tag {tag:#x}/{word} is not a game bound's"
                )
            if tags.setdefault(kind, tag) != tag:
                raise BoundsError(
                    f"template {path.name}: its {NAMES[kind]} tag {tag:#x} differs from another template's"
                    f" {tags[kind]:#x} (templates from different game builds?)"
                )
    return tags


# --------------------------------------------------------------------------------------------------
# validation (semantic checks shared by decode results and converter output)


def polygon_vertices(record: bytes) -> tuple[int, list[int]]:
    """(polygon type, vertex indices) of one 16-byte polygon record."""
    kind = record[0] & 7
    if kind == POLY_TRIANGLE:
        return kind, [v & 0x7FFF for v in struct.unpack_from("<3H", record, 4)]
    if kind == POLY_SPHERE:
        return kind, [struct.unpack_from("<H", record, 2)[0]]
    if kind in (POLY_CAPSULE, POLY_CYLINDER):
        return kind, [struct.unpack_from("<H", record, 2)[0], struct.unpack_from("<H", record, 8)[0]]
    if kind == POLY_BOX:
        return kind, list(struct.unpack_from("<4H", record, 4))
    raise BoundsError(f"polygon type {kind} unknown")


def check_geometry(bound: Bound, path: str, source: bool = False) -> dict:
    nv, npoly = bound.u("<I", 0xD0), bound.u("<I", 0xD4)
    nmat = bound.fields[0x120]
    polys = bound.arrays["polygons"]
    pmi = bound.arrays["poly_materials"]
    kinds: Counter = Counter()
    neighbours = 0
    for i in range(npoly):
        rec = polys[16 * i : 16 * i + 16]
        kind, verts = polygon_vertices(rec)
        kinds[POLY_NAMES[kind]] += 1
        if any(v >= nv for v in verts):
            raise BoundsError(f"{path}: polygon {i} ({POLY_NAMES[kind]}) vertex index beyond {nv}")
        if kind == POLY_TRIANGLE:
            for e in struct.unpack_from("<3H", rec, 10):
                if e != 0xFFFF and e >= npoly:
                    raise BoundsError(f"{path}: polygon {i} neighbour {e} beyond {npoly}")
                neighbours += e != 0xFFFF
        if pmi[i] >= nmat:
            raise BoundsError(f"{path}: polygon {i} material {pmi[i]} beyond {nmat}")
    rooms = sorted({(struct.unpack_from("<I", bound.arrays["materials"], 8 * k)[0] >> 16) & 0x1F for k in range(nmat)})
    ncol = bound.fields[0x121]
    for k in range(nmat):
        colour = (struct.unpack_from("<I", bound.arrays["materials"], 8 * k + 4)[0] >> 8) & 0xFF
        if colour and colour > ncol:
            raise BoundsError(f"{path}: material {k} colour index {colour} beyond {ncol}")
    if bound.octants is not None:
        for k, items in enumerate(bound.octants):
            if not items and not source:
                raise BoundsError(f"{path}: octant {k} is empty (the a80 octant crash pattern; convert repairs it)")
            if any(v >= nv for v in struct.unpack(f"<{len(items) // 4}I", items)):
                raise BoundsError(f"{path}: octant {k} vertex index out of range")
    centre, radius = bound.vec(0x40), bound.u("<f", 0x14)
    lo, hi = bound.vec(0x30), bound.vec(0x20)
    tolerance = 1e-3 + 1e-6 * max(abs(x) for x in centre)
    for v in vertices(bound):
        if math.dist(centre, v) > radius + tolerance:
            raise BoundsError(f"{path}: bounding sphere does not contain vertex {v}")
        if any(v[k] < lo[k] - tolerance or v[k] > hi[k] + tolerance for k in range(3)):
            raise BoundsError(f"{path}: box does not contain vertex {v}")
    leaves = check_tree(bound, path) if bound.tree else None
    return {"polygons": dict(kinds), "neighbours": neighbours, "rooms": rooms, "bvh_leaves": leaves}


def check_tree(bound: Bound, path: str) -> int:
    """Every item (polygon or child) in exactly one leaf; leaf/branch spans inside their tree."""
    tree = bound.tree
    items = len(bound.children) if bound.kind == COMPOSITE else bound.u("<I", 0xD4)
    nodes = [struct.unpack_from("<8h", tree.nodes, 16 * i) for i in range(tree.node_count)]
    covered = [0] * items
    for i, (*box, item, count) in enumerate(nodes):
        if any(box[k] > box[k + 3] for k in range(3)):
            raise BoundsError(f"{path}: bvh node {i} box inverted")
        if count > 0:
            if item < 0 or item + count > items:
                raise BoundsError(f"{path}: bvh leaf {i} items {item}+{count} beyond {items}")
            for j in range(item, item + count):
                covered[j] += 1
        elif count != 0 or item < 1 or i + item > len(nodes):
            raise BoundsError(f"{path}: bvh branch {i} span {item} invalid")
    if any(c != 1 for c in covered):
        raise BoundsError(f"{path}: bvh does not cover every item exactly once")
    for t in range(len(tree.trees) // 16):
        first, end = struct.unpack_from("<2h", tree.trees, 16 * t + 12)
        if not 0 <= first < end <= len(nodes):
            raise BoundsError(f"{path}: bvh tree {t} node range {first}..{end}")
    return sum(c for c in covered)


def check(resource: Resource, source: bool = False) -> dict:
    """Validate the whole graph; returns a summary. `source` admits what convert() repairs (empty
    octants)."""
    summary: dict = {"children": [], "type": NAMES[resource.root.kind]}
    root = resource.root
    if root.kind == COMPOSITE:
        if root.tree:
            check_tree(root, "root")
        flags = root.child_flags
        for i, child in enumerate(root.children):
            row = {"type": NAMES[child.kind]}
            if flags:
                pair = struct.unpack_from("<2I", flags, 8 * i)
                row["flags"] = f"{pair[0]:#x}/{pair[1]:#x}"
            if child.kind in (GEOMETRY, BVH):
                row.update(check_geometry(child, f"root/{i}", source))
                row["vertices"] = child.u("<I", 0xD0)
                row["materials"] = child.fields[0x120]
            summary["children"].append(row)
    elif root.kind in (GEOMETRY, BVH):
        summary["children"].append(check_geometry(root, "root", source))
    return summary


# --------------------------------------------------------------------------------------------------
# geometry helpers


def vertices(bound: Bound, name: str = "vertices") -> list[tuple[float, float, float]]:
    """World (resource) positions: int16 * quantum + centre, in float32 like the game."""
    q = bound.vec(0x90)
    c = bound.vec(0xA0)
    raw = bound.arrays[name]
    out = []
    for i in range(len(raw) // 6):
        xyz = struct.unpack_from("<3h", raw, 6 * i)
        out.append(tuple(_f32(xyz[k] * q[k] + c[k]) for k in range(3)))
    return out


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _iter_bounds(bound: Bound):
    yield bound
    for child in bound.children:
        yield from _iter_bounds(child)


# --------------------------------------------------------------------------------------------------
# transforms


def translate(resource: Resource, delta: tuple[float, float, float]) -> None:
    """Move every bound by `delta` (world units). Quantised vertices and BVH nodes are relative to
    their centre, so only the centres, boxes and sphere centres move: the shape stays exact."""
    if not any(delta):
        return

    def shift(buf: bytearray, offset: int) -> None:
        xyz = struct.unpack_from("<3f", buf, offset)
        struct.pack_into("<3f", buf, offset, *(_f32(xyz[k] + delta[k]) for k in range(3)))

    def shift_tree(tree: Tree) -> None:
        vec = bytearray(tree.vectors)
        for k in range(3):  # min, max, centre (quantum and its inverse are sizes)
            shift(vec, 0x10 + 0x10 * k)
        tree.vectors = bytes(vec)

    for bound in _iter_bounds(resource.root):
        for offset in (0x20, 0x30, 0x40, 0x50):  # max, min, sphere centre, centre of gravity
            shift(bound.fields, offset)
        if bound.kind in (GEOMETRY, BVH):
            shift(bound.fields, 0xA0)
        if bound.tree:
            shift_tree(bound.tree)
        if bound.kind == COMPOSITE and bound.child_boxes:
            boxes = bytearray(bound.child_boxes)
            for i in range(len(bound.children)):
                shift(boxes, 0x20 * i)
                shift(boxes, 0x20 * i + 0x10)
            bound.child_boxes = bytes(boxes)
        if bound.kind == COMPOSITE and bound.matrices:
            for name in ("matrices", "matrices2"):
                data = getattr(bound, name)
                if data is None:
                    continue
                for i in range(len(bound.children)):
                    if any(struct.unpack_from("<3f", data, 0x40 * i + 0x30)):
                        raise BoundsError(
                            "a child matrix carries a translation: static children are world space"
                            " (identity instance), so this file is not a static .pbn layout"
                        )


def normalize(
    resource: Resource,
    include_flags: int | None = None,
    type_flags: int | None = None,
    tags: dict[int, int] | None = None,
) -> list[str]:
    """Set the PS5 authoring details (module docstring); returns the notes of what changed. `tags`
    (bound type -> class tag, `read_tags()`) defaults to the module's TAGS."""
    tags = TAGS if tags is None else tags
    notes: list[str] = []
    for bound in _iter_bounds(resource.root):
        if bound.kind not in tags:
            raise BoundsError(
                f"no class tag for {NAMES[bound.kind]} bounds: pass a --template that has a {NAMES[bound.kind]}"
                " bound (./menu-ctl.sh fetch-templates caches them under bounds-templates/)"
            )
        tag, word = struct.unpack_from("<II", bound.fields, 0)
        if (tag, word) != (tags[bound.kind], 1):
            struct.pack_into("<II", bound.fields, 0, tags[bound.kind], 1)
            notes.append(f"{NAMES[bound.kind]} tag {tag:#x}/{word} -> {tags[bound.kind]:#x}/1")
        if bound.kind in (GEOMETRY, BVH):
            margin = bound.u("<f", 0x2C)
            half = _f32(margin * 0.5)
            struct.pack_into("<f", bound.fields, 0x9C, _f32(half / 32767.0))
            struct.pack_into("<f", bound.fields, 0xAC, half)
        if bound.tree:
            vec = bytearray(bound.tree.vectors)
            for k in range(5):
                struct.pack_into("<I", vec, 0x1C + 0x10 * k, NAN_W)
            bound.tree.vectors = bytes(vec)
            if bound.kind == BVH and bound.tree.capacity != bound.tree.node_count:
                notes.append(f"bvh capacity {bound.tree.capacity} -> {bound.tree.node_count}")
                bound.tree.nodes = bound.tree.nodes[: 16 * bound.tree.node_count]
        if bound.kind == COMPOSITE:
            _normalize_composite(bound, notes, include_flags, type_flags)
    return notes


def _normalize_composite(bound: Bound, notes: list[str], include_flags, type_flags) -> None:
    n = len(bound.children)
    identity = bytearray()
    for _ in range(n):
        for row in range(4):
            identity += struct.pack("<3fI", *[float(row == k) for k in range(3)], MATRIX_W[row])
    for name in ("matrices", "matrices2"):
        data = getattr(bound, name)
        if data is None:
            continue
        for i in range(n):
            m = struct.unpack_from("<16f", data, 0x40 * i)
            rot = [m[0:3], m[4:7], m[8:11], m[12:15]]
            if any(abs(rot[r][k] - float(r == k)) > 1e-6 for r in range(4) for k in range(3)):
                raise BoundsError(
                    f"child {i} matrix is not identity: static .pbn children are world space"
                    " (the store gives them an identity instance matrix); bake the transform first"
                )
    bound.matrices, bound.matrices2, bound.matrices_alias = bytes(identity), None, True
    flags = bytearray(bound.child_flags or bytes(8 * n))
    if bound.child_flags2 is not None and bound.child_flags2 != bound.child_flags:
        notes.append("separate child flags 2 array dropped (aliased to flags 1 as retail)")
    for i in range(n):
        t, inc = struct.unpack_from("<2I", flags, 8 * i)
        new = (t if type_flags is None else type_flags, inc if include_flags is None else include_flags)
        if new == (0, 0):
            raise BoundsError(f"child {i} has type/include flags 0/0 (no collision); pass --type-flags/--include")
        struct.pack_into("<2I", flags, 8 * i, *new)
    bound.child_flags, bound.child_flags2, bound.flags_alias = bytes(flags), None, True
    boxes = bytearray()
    for child in bound.children:
        boxes += struct.pack("<3fI3ff", *child.vec(0x30), 1, *child.vec(0x20), child.u("<f", 0x2C))
    if bound.child_boxes != bytes(boxes):
        notes.append("composite child boxes rebuilt from the children")
    bound.child_boxes = bytes(boxes)
    lo = [min(c.vec(0x30)[k] for c in bound.children) for k in range(3)]
    hi = [max(c.vec(0x20)[k] for c in bound.children) for k in range(3)]
    if (tuple(lo), tuple(hi)) != (bound.vec(0x30), bound.vec(0x20)):
        notes.append("composite box set to the union of the children")
        struct.pack_into("<3f", bound.fields, 0x20, *hi)
        struct.pack_into("<3f", bound.fields, 0x30, *lo)
    centre = [_f32((a + b) / 2) for a, b in zip(lo, hi, strict=True)]
    radius = _f32(math.dist(lo, hi) / 2)
    if (bound.vec(0x40), bound.u("<f", 0x14)) != (tuple(centre), radius):
        struct.pack_into("<3f", bound.fields, 0x40, *centre)  # retail: box centre, half diagonal
        struct.pack_into("<f", bound.fields, 0x14, radius)


def weld_and_link(bound: Bound) -> dict:
    """Merge vertices with identical quantised positions (lossless) and rebuild triangle neighbours
    the retail way: edge k of a triangle is (v[k], v[k+1]); its neighbour is the one other triangle
    using that index pair, else 0xFFFF (open or non-manifold edges). Polygon order is unchanged, so
    the BVH leaves stay valid. Type 4 geometry keeps its vertex array (octants, shrunk vertices)."""
    raw = bound.arrays["vertices"]
    nv = len(raw) // 6
    polys = bytearray(bound.arrays["polygons"])
    npoly = len(polys) // 16
    remap = list(range(nv))
    merged = 0
    if bound.kind == BVH:
        first: dict[bytes, int] = {}
        keep: list[int] = []
        for i in range(nv):
            key = raw[6 * i : 6 * i + 6]
            if key in first:
                remap[i] = first[key]
                merged += 1
            else:
                first[key] = len(keep)
                remap[i] = len(keep)
                keep.append(i)
        if merged:
            bound.arrays["vertices"] = b"".join(raw[6 * i : 6 * i + 6] for i in keep)
            colours = bound.arrays.get("vertex_colours")
            if colours is not None:
                bound.arrays["vertex_colours"] = b"".join(colours[4 * i : 4 * i + 4] for i in keep)
            struct.pack_into("<I", bound.fields, 0xD0, len(keep))
            struct.pack_into("<I", bound.fields, 0x84, len(keep))
    degenerate = 0
    edges: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for j in range(npoly):
        at = 16 * j
        kind = polys[at] & 7
        if kind == POLY_TRIANGLE:
            idx = list(struct.unpack_from("<3H", polys, at + 4))
            new = [(v & 0x8000) | remap[v & 0x7FFF] for v in idx]
            struct.pack_into("<3H", polys, at + 4, *new)
            plain = [v & 0x7FFF for v in new]
            if len(set(plain)) < 3:
                degenerate += 1
                continue
            for k in range(3):
                a, b = plain[k], plain[(k + 1) % 3]
                edges.setdefault((min(a, b), max(a, b)), []).append((j, k))
        elif kind == POLY_SPHERE:
            struct.pack_into("<H", polys, at + 2, remap[struct.unpack_from("<H", polys, at + 2)[0]])
        elif kind in (POLY_CAPSULE, POLY_CYLINDER):
            for off in (2, 8):
                struct.pack_into("<H", polys, at + off, remap[struct.unpack_from("<H", polys, at + off)[0]])
        elif kind == POLY_BOX:
            struct.pack_into("<4H", polys, at + 4, *(remap[v] for v in struct.unpack_from("<4H", polys, at + 4)))
    links = [[0xFFFF] * 3 for _ in range(npoly)]
    linked = open_edges = shared3 = 0
    for users in edges.values():
        if len(users) == 2:
            (j1, k1), (j2, k2) = users
            links[j1][k1], links[j2][k2] = j2, j1
            linked += 1
        elif len(users) == 1:
            open_edges += 1
        else:
            shared3 += 1
    for j in range(npoly):
        if polys[16 * j] & 7 == POLY_TRIANGLE:
            struct.pack_into("<3H", polys, 16 * j + 10, *links[j])
    bound.arrays["polygons"] = bytes(polys)
    return {
        "welded_vertices": merged,
        "linked_edges": linked,
        "open_edges": open_edges,
        "non_manifold_edges": shared3,
        "degenerate_triangles": degenerate,
    }


def fit_geometry(bound: Bound) -> list[str]:
    """Retail invariants of a geometry child that PC authoring tools leave out (all only grow or
    recompute derived fields; vertices and polygons are untouched)."""
    notes = []
    verts = vertices(bound)
    polys = bound.arrays["polygons"]
    margin = bound.u("<f", 0x2C)
    lo = [min(v[k] for v in verts) for k in range(3)]
    hi = [max(v[k] for v in verts) for k in range(3)]
    for j in range(len(polys) // 16):  # primitive polygons reach their radius past the vertex
        kind, idx = polygon_vertices(polys[16 * j : 16 * j + 16])
        if kind in (POLY_SPHERE, POLY_CAPSULE, POLY_CYLINDER):
            r = struct.unpack_from("<f", polys, 16 * j + 4)[0]
            for i in idx:
                for k in range(3):
                    lo[k], hi[k] = min(lo[k], verts[i][k] - r), max(hi[k], verts[i][k] + r)
    old_lo, old_hi = bound.vec(0x30), bound.vec(0x20)
    new_lo = [_f32(min(old_lo[k], lo[k] - margin)) for k in range(3)]
    new_hi = [_f32(max(old_hi[k], hi[k] + margin)) for k in range(3)]
    if (tuple(new_lo), tuple(new_hi)) != (old_lo, old_hi):
        notes.append("box grown to the vertex extent + margin (retail)")
        struct.pack_into("<3f", bound.fields, 0x30, *new_lo)
        struct.pack_into("<3f", bound.fields, 0x20, *new_hi)
    centre = bound.vec(0x40)
    far = max(math.dist(centre, v) for v in verts) + margin
    if far > bound.u("<f", 0x14):
        notes.append("bounding sphere grown to contain every vertex")
        struct.pack_into("<f", bound.fields, 0x14, _f32(far * (1 + 1e-6)))
    if bound.vec(0x50) != bound.vec(0xA0):
        notes.append("centre of gravity set to the geometry centre (retail)")
        struct.pack_into("<3f", bound.fields, 0x50, *bound.vec(0xA0))
    if bound.arrays.get("vertex_colours") is not None and bound.u("<H", 0x82) != 1:
        notes.append("vertex colours dropped (retail carries them only with +0x82 == 1)")
        bound.arrays["vertex_colours"] = None
    if bound.octants is not None:
        full = struct.pack(f"<{bound.u('<I', 0xD0)}I", *range(bound.u("<I", 0xD0)))
        for k, items in enumerate(bound.octants):
            if not items:
                notes.append(f"empty octant {k} set to every vertex (a80 crash pattern)")
                bound.octants[k] = full
    return notes


def prepare_pc(resource: Resource, link: bool = True) -> list[str]:
    """PC -> PS5 content fixes for every geometry child: weld + neighbours, then the retail fit."""
    notes: list[str] = []
    for path, bound in _walk_paths(resource.root, "root"):
        if bound.kind not in (GEOMETRY, BVH):
            continue
        if link:
            stats = weld_and_link(bound)
            notes.append(f"{path}: {stats}")
        notes += [f"{path}: {n}" for n in fit_geometry(bound)]
    return notes


def _walk_paths(bound: Bound, path: str):
    yield path, bound
    for i, child in enumerate(bound.children):
        yield from _walk_paths(child, f"{path}/{i}")


# --------------------------------------------------------------------------------------------------
# encode


def _align(value: int, align: int = 16) -> int:
    return (value + align - 1) & ~(align - 1)


def _pow2(value: int) -> int:
    return 1 << max(0, (value - 1).bit_length())


def _page_caps(nibble: int) -> dict[int, int]:
    """Largest page count per page size that the RSC7 flag fields hold for base 512 << nibble."""
    base = 512 << nibble
    return {base << (8 - rank): (1 << width) - 1 for rank, (_shift, width) in enumerate(PAGE_FIELDS)}


def place(objects: list[tuple[str, int]], nibble: int | None = None) -> tuple[dict[str, int], list[int]]:
    """Retail page placement: the first object at 0, then the rest largest first (stable; the key is
    the 16-byte aligned size), 16-byte aligned, first fit over the open pages in opening order. A new
    page is the smallest power of two >= 8 KiB that holds the object opening it (the first page also
    holds the first object); when the RSC7 count field of that size is full, the next larger size.
    The page base is the smallest (512 << nibble, nibble 0..3) that works. No object crosses a page.
    Returns {name: offset} and the page sizes in file order (largest first)."""
    errors = []
    for n in (range(4) if nibble is None else (nibble,)):
        try:
            return _place(objects, _page_caps(n))
        except BoundsError as exc:
            errors.append(f"base {512 << n}: {exc}")
    raise BoundsError("; ".join(errors))


def _place(objects: list[tuple[str, int]], caps: dict[int, int]) -> tuple[dict[str, int], list[int]]:
    first, first_size = objects[0]
    pages: list[list[int]] = []  # [size, used] in opening order
    where: dict[str, tuple[int, int]] = {first: (0, 0)}

    def open_page(need: int) -> int:
        size = max(PAGE_MIN, _pow2(need))
        while size in caps and sum(1 for page in pages if page[0] == size) >= caps[size]:
            size *= 2
        if size not in caps or size > PAGE_MAX:
            raise BoundsError(f"no page for {need} bytes")
        pages.append([size, 0])
        return len(pages) - 1

    for name, size in sorted(objects[1:], key=lambda item: -_align(item[1])):
        for number, page in enumerate(pages):  # noqa: B007 (number is used after the loop)
            at = _align(page[1])
            if at + size <= page[0]:
                break
        else:
            at = _align(first_size) if not pages else 0
            number = open_page(at + size)
        pages[number][1] = at + size
        where[name] = (number, at)
    if not pages:
        open_page(first_size)
    order = sorted(range(len(pages)), key=lambda k: (-pages[k][0], k))
    if order[0] != 0:
        raise BoundsError("a later page is larger than the first object's page")
    starts, cursor = {}, 0
    for k in order:
        starts[k] = cursor
        cursor += pages[k][0]
    return {name: starts[number] + at for name, (number, at) in where.items()}, [pages[k][0] for k in order]


class _Writer:
    def __init__(self) -> None:
        self.objects: list[tuple[str, bytes]] = []
        self.fixups: list[tuple[str, int, str]] = []  # (object, offset, target object)

    def add(self, name: str, data: bytes) -> str:
        self.objects.append((name, data))
        return name

    def ptr(self, owner: str, offset: int, target: str | None) -> None:
        if target is not None:
            self.fixups.append((owner, offset, target))


def _emit_tree(w: _Writer, tree: Tree, owner: str, offset: int, label: str) -> None:
    block = bytearray(0x80)
    struct.pack_into("<QII", block, 0, 0, tree.node_count, tree.capacity)
    block[0x10:0x70] = tree.vectors
    count = len(tree.trees) // 16
    struct.pack_into("<QHHI", block, 0x70, 0, count, count, tree.pad)
    name = w.add(label, bytes(block))
    w.ptr(owner, offset, name)
    w.ptr(name, 0, w.add(label + ".nodes", tree.nodes))
    w.ptr(name, 0x70, w.add(label + ".trees", tree.trees))


def _emit_bound(w: _Writer, bound: Bound, label: str) -> str:
    name = w.add(label, bytes(bound.fields))
    if bound.kind == COMPOSITE:
        n = len(bound.children)
        table = w.add(label + ".children", bytes(8 * n))
        w.ptr(name, 0x70, table)
        if bound.matrices is not None:
            mats = w.add(label + ".matrices", bound.matrices)
            w.ptr(name, 0x78, mats)
            if bound.matrices_alias:
                w.ptr(name, 0x80, mats)
            elif bound.matrices2 is not None:
                w.ptr(name, 0x80, w.add(label + ".matrices2", bound.matrices2))
        if bound.child_boxes is not None:
            w.ptr(name, 0x88, w.add(label + ".boxes", bound.child_boxes))
        if bound.child_flags is not None:
            flags = w.add(label + ".flags", bound.child_flags)
            w.ptr(name, 0x90, flags)
            if bound.flags_alias:
                w.ptr(name, 0x98, flags)
            elif bound.child_flags2 is not None:
                w.ptr(name, 0x98, w.add(label + ".flags2", bound.child_flags2))
        if bound.tree:
            _emit_tree(w, bound.tree, name, 0xA8, label + ".bvh")
        for i, child in enumerate(bound.children):
            w.ptr(table, 8 * i, _emit_bound(w, child, f"{label}/{i}"))
    elif bound.kind in (GEOMETRY, BVH):
        for array, offset, _size in GEOMETRY_ARRAYS:
            data = bound.arrays.get(array)
            if data is not None:
                w.ptr(name, offset, w.add(f"{label}.{array}", data))
        if bound.octants is not None:  # one block: 8 counts, 8 item pointers, the lists, 32 zero bytes
            counts = struct.pack("<8I", *(len(items) // 4 for items in bound.octants))
            octs = w.add(label + ".octants", counts + bytes(64) + b"".join(bound.octants) + bytes(32))
            at = 96
            for k, items in enumerate(bound.octants):
                w.fixups.append((octs, 32 + 8 * k, f"@{octs}+{at}"))
                at += len(items)
            w.ptr(name, 0xC0, octs)
            w.fixups.append((name, 0xC8, f"@{octs}+32"))
        if bound.kind == BVH:
            _emit_tree(w, bound.tree, name, 0x130, label + ".bvh")
    return name


def encode(resource: Resource) -> bytes:
    """Write the model as a loose RSC7 v43 .pbn (stored deflate, retail page placement)."""
    w = _Writer()
    root = _emit_bound(w, resource.root, "root")
    guess = 1  # the pages-info block (16 + 8 per page) is placed too: iterate to a fixed point
    for _ in range(8):
        objects = [*w.objects, ("pages", bytes(16 + 8 * guess))]
        where, pages = place([(n, len(d)) for n, d in objects])
        if len(pages) == guess:
            break
        guess = max(guess, len(pages))
    if len(pages) > guess:
        raise BoundsError("pages info block does not settle")
    buf = bytearray(sum(pages))
    data = dict(objects)
    for name, offset in where.items():
        buf[offset : offset + len(data[name])] = data[name]
    for owner, offset, target in w.fixups:
        if target.startswith("@"):
            base, extra = target[1:].rsplit("+", 1)
            pointer = SYS + where[base] + int(extra)
        else:
            pointer = SYS + where[target]
        struct.pack_into("<Q", buf, where[owner] + offset, pointer)
    struct.pack_into("<Q", buf, where[root] + 0x08, SYS + where["pages"])
    buf[where["pages"] + 8] = len(pages)  # system page count; the per-page records stay zero
    low = flags_for_pages(pages)
    header = struct.pack("<4sIII", b"RSC7", VERSION, (VERSION >> 4) << 28 | low, (VERSION & 15) << 28)
    return header + stored_deflate(bytes(buf))


# --------------------------------------------------------------------------------------------------
# semantic comparison


def semantic(resource: Resource) -> dict:
    """Canonical content (no pointers, no placement) for equality checks."""

    def tree(t: Tree | None):
        if t is None:
            return None
        return {"vectors": t.vectors.hex(), "nodes": t.nodes.hex(), "count": t.node_count, "trees": t.trees.hex()}

    def bound(b: Bound) -> dict:
        out = {"kind": b.kind, "fields": bytes(b.fields).hex(), "tree": tree(b.tree)}
        if b.kind == COMPOSITE:
            out.update(
                children=[bound(c) for c in b.children],
                matrices=b.matrices.hex() if b.matrices else None,
                matrices2=(None if b.matrices_alias else (b.matrices2.hex() if b.matrices2 else "")),
                boxes=b.child_boxes.hex() if b.child_boxes else None,
                flags=b.child_flags.hex() if b.child_flags else None,
                flags2=(None if b.flags_alias else (b.child_flags2.hex() if b.child_flags2 else "")),
            )
        else:
            out["arrays"] = {k: (v.hex() if v is not None else None) for k, v in sorted(b.arrays.items())}
            out["octants"] = [x.hex() for x in b.octants] if b.octants is not None else None
        return out

    return {"root": bound(resource.root)}  # the pages-info block is rebuilt by encode()


def diff(a, b, path="") -> list[str]:
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for key in sorted(set(a) | set(b)):
            out += diff(a.get(key), b.get(key), f"{path}.{key}")
        return out
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return [f"{path}: {len(a)} != {len(b)} items"]
        out = []
        for i, (x, y) in enumerate(zip(a, b, strict=True)):
            out += diff(x, y, f"{path}[{i}]")
        return out
    return [] if a == b else [f"{path}: differs"]


def geometry_report(src: Resource, out: Resource, delta=(0.0, 0.0, 0.0)) -> dict:
    """Converter readback: per geometry child and polygon, the same type, radius/area word, vertex
    positions (source + translation, within float32 + half a quantum) and material record. Vertex
    indices and neighbours may differ (weld, neighbour rebuild); their change is counted."""
    rows = []
    a_list = list(_iter_bounds(src.root))
    b_list = list(_iter_bounds(out.root))
    if [b.kind for b in a_list] != [b.kind for b in b_list]:
        raise BoundsError("readback tree shape differs")
    worst = 0.0
    for a, b in zip(a_list, b_list, strict=True):
        if a.kind not in (GEOMETRY, BVH):
            continue
        if a.u("<I", 0xD4) != b.u("<I", 0xD4):
            raise BoundsError("polygon count changed in the readback")
        va, vb = vertices(a), vertices(b)
        q = b.vec(0x90)
        pa, pb = a.arrays["polygons"], b.arrays["polygons"]
        ma, mb = a.arrays["materials"], b.arrays["materials"]
        ia, ib = a.arrays["poly_materials"], b.arrays["poly_materials"]
        relinked = 0
        for j in range(len(pa) // 16):
            ra, rb = pa[16 * j : 16 * j + 16], pb[16 * j : 16 * j + 16]
            ka, xa = polygon_vertices(ra)
            kb, xb = polygon_vertices(rb)
            # Triangles and boxes keep their first word (area / type); a sphere, capsule or cylinder holds
            # its first vertex index in bytes 2..3, which the weld may renumber (positions checked below).
            head = 4 if ka in (POLY_TRIANGLE, POLY_BOX) else 2
            if ka != kb or ra[:head] != rb[:head] or (ka in (POLY_CAPSULE, POLY_CYLINDER) and ra[4:8] != rb[4:8]):
                raise BoundsError(f"polygon {j} type or radius/area changed")
            if ka == POLY_SPHERE and ra[4:8] != rb[4:8]:
                raise BoundsError(f"polygon {j} sphere radius changed")
            for i, k2 in zip(xa, xb, strict=True):
                for k in range(3):
                    err = abs(va[i][k] + delta[k] - vb[k2][k])
                    worst = max(worst, err)
                    tolerance = 1e-4 + abs(vb[k2][k]) * 4.8e-7 + q[k] * 0.5
                    if err > tolerance:
                        raise BoundsError(f"polygon {j} vertex moved by {err} (tolerance {tolerance})")
            if ma[8 * ia[j] : 8 * ia[j] + 8] != mb[8 * ib[j] : 8 * ib[j] + 8]:
                raise BoundsError(f"polygon {j} material changed")
            if ka == POLY_TRIANGLE and ra[10:16] != rb[10:16]:
                relinked += 1
        rows.append(
            {
                "vertices": f"{len(va)}->{len(vb)}",
                "polygons": b.u("<I", 0xD4),
                "materials": b.fields[0x120],
                "relinked_triangles": relinked,
            }
        )
    return {"geometry": rows, "max_vertex_error": worst}


def probe(resource: Resource, x: float, y: float) -> list[tuple[float, str]]:
    """Surfaces a vertical line at (x, y) crosses: [(z, label)], highest first. Triangles are exact;
    box polygons use the box through their four corners and its reflection (exact when axis
    aligned), spheres are exact, capsules and cylinders use their axis-aligned box."""
    hits: list[tuple[float, str]] = []
    for path, bound in _walk_paths(resource.root, "root"):
        if bound.kind not in (GEOMETRY, BVH):
            continue
        lo, hi = bound.vec(0x30), bound.vec(0x20)
        if not (lo[0] <= x <= hi[0] and lo[1] <= y <= hi[1]):
            continue
        verts = vertices(bound)
        polys = bound.arrays["polygons"]
        mats, pmi = bound.arrays["materials"], bound.arrays["poly_materials"]
        for j in range(len(polys) // 16):
            rec = polys[16 * j : 16 * j + 16]
            kind, idx = polygon_vertices(rec)
            d1 = struct.unpack_from("<I", mats, 8 * pmi[j])[0]
            label = f"{path} {POLY_NAMES[kind]} {j} material {d1 & 0xFF} room {(d1 >> 16) & 0x1F}"
            pts = [verts[i] for i in idx]
            if kind == POLY_TRIANGLE:
                (ax, ay, az), (bx, by, bz), (cx, cy, cz) = pts
                det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
                if abs(det) < 1e-12:
                    continue
                u = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / det
                v = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / det
                if u >= -1e-6 and v >= -1e-6 and u + v <= 1 + 1e-6:
                    hits.append((u * az + v * bz + (1 - u - v) * cz, label))
            elif kind == POLY_SPHERE:
                r = struct.unpack_from("<f", rec, 4)[0]
                d2 = (x - pts[0][0]) ** 2 + (y - pts[0][1]) ** 2
                if d2 <= r * r:
                    dz = math.sqrt(r * r - d2)
                    hits += [(pts[0][2] + dz, label), (pts[0][2] - dz, label)]
            else:
                if kind == POLY_BOX:
                    c = [sum(pt[k] for pt in pts) / 4 for k in range(3)]
                    pts = pts + [tuple(2 * c[k] - pt[k] for k in range(3)) for pt in pts]
                    r = 0.0
                else:
                    r = struct.unpack_from("<f", rec, 4)[0]
                blo = [min(pt[k] for pt in pts) - r for k in range(3)]
                bhi = [max(pt[k] for pt in pts) + r for k in range(3)]
                if blo[0] <= x <= bhi[0] and blo[1] <= y <= bhi[1]:
                    hits += [(bhi[2], label), (blo[2], label)]
    return sorted(hits, key=lambda h: -h[0])


# --------------------------------------------------------------------------------------------------
# merge: several composites -> one composite


def _quantise(value: float, centre: float, inverse: float, up: bool) -> int:
    """float32 (value - centre) * inverse, floor (min) or ceil (max), clamped to int16: matches 2,126 of
    2,154 retail composite-BVH leaf bounds (the rest differ by one unit)."""
    v = _f32(_f32(value - centre) * inverse)
    v = math.ceil(v) if up else math.floor(v)
    return max(-32768, min(32767, v))


def composite_bvh(bound: Bound) -> Tree:
    """The retail composite BVH: quantised over the composite box (centre = box centre, quantum =
    half extent / 32767), one leaf per child (children keep their order), branches split at the
    median child centre along the longest axis, nodes in depth-first order (a branch's item = its
    subtree size). 2n-1 nodes, capacity 2n+1 (two filler nodes), subtree headers of at most 127
    nodes (the whole tree when it fits)."""
    n = len(bound.children)
    lo, hi = bound.vec(0x30), bound.vec(0x20)
    centre = [_f32((a + b) / 2) for a, b in zip(lo, hi, strict=True)]
    half = [_f32(max((b - a) / 2, 1e-3)) for a, b in zip(lo, hi, strict=True)]
    quantum = [_f32(h / 32767.0) for h in half]
    inverse = [_f32(32767.0 / h) for h in half]  # retail: 32767 / half, not 1 / quantum (46 of 46)
    boxes = [(child.vec(0x30), child.vec(0x20)) for child in bound.children]
    nodes: list[tuple[int, ...] | None] = []

    def ints(items: list[int]) -> tuple[int, ...]:
        mins = [min(boxes[i][0][k] for i in items) for k in range(3)]
        maxs = [max(boxes[i][1][k] for i in items) for k in range(3)]
        return tuple(_quantise(mins[k], centre[k], inverse[k], False) for k in range(3)) + tuple(
            _quantise(maxs[k], centre[k], inverse[k], True) for k in range(3)
        )

    def build(items: list[int]) -> None:
        if len(items) == 1:
            nodes.append((*ints(items), items[0], 1))
            return
        at = len(nodes)
        nodes.append(None)
        mids = {i: [(boxes[i][0][k] + boxes[i][1][k]) / 2 for k in range(3)] for i in items}
        spans = [max(mids[i][k] for i in items) - min(mids[i][k] for i in items) for k in range(3)]
        axis = spans.index(max(spans))
        ordered = sorted(items, key=lambda i: (mids[i][axis], i))
        half = len(ordered) // 2
        build(ordered[:half])
        build(ordered[half:])
        nodes[at] = (*ints(items), len(nodes) - at, 0)

    build(list(range(n)))
    trees: list[tuple[int, ...]] = []

    def headers(index: int) -> None:
        node = nodes[index]
        size = node[6] if node[7] == 0 else 1
        if size <= TREE_SPAN:
            trees.append((*node[:6], index, index + size))
            return
        left = index + 1
        headers(left)
        headers(left + (nodes[left][6] if nodes[left][7] == 0 else 1))

    headers(0)
    vectors = bytearray(0x60)
    for k, vec in enumerate((lo, hi, centre, inverse, quantum)):
        struct.pack_into("<3fI", vectors, 0x10 + 0x10 * k, *vec, NAN_W)
    packed = b"".join(struct.pack("<8h", *node) for node in nodes) + FILLER_NODE * 2
    return Tree(bytes(vectors), packed, len(nodes), b"".join(struct.pack("<8h", *t) for t in trees))


def composite_mass(bound: Bound) -> None:
    """Retail composite mass fields: centre of gravity = the mean of the children's, unit inertia =
    the children's (1 each) plus their parallel-axis terms, divided by the child count; volume = the
    child count."""
    n = len(bound.children)
    cogs = [child.vec(0x50) for child in bound.children]
    mean = [sum(c[k] for c in cogs) / n for k in range(3)]
    inertia = [
        sum(1.0 + (c[(k + 1) % 3] - mean[(k + 1) % 3]) ** 2 + (c[(k + 2) % 3] - mean[(k + 2) % 3]) ** 2 for c in cogs)
        / n
        for k in range(3)
    ]
    struct.pack_into("<3f", bound.fields, 0x50, *(_f32(v) for v in mean))
    struct.pack_into("<4f", bound.fields, 0x60, *(_f32(v) for v in inertia), float(n))


def _merged(sources: list[Resource]) -> Resource:
    """One composite holding every source composite's children (and their flag pairs), in order."""
    children, flags = [], b""
    for k, source in enumerate(sources):
        root = source.root
        if root.kind != COMPOSITE:
            raise BoundsError(f"input {k}: root is a {NAMES[root.kind]}, not a composite")
        for i, child in enumerate(root.children):
            if child.kind == COMPOSITE:
                raise BoundsError(f"input {k} child {i}: nested composites are not merged")
        children += root.children
        flags += root.child_flags or bytes(8 * len(root.children))
    if not 0 < len(children) <= LIMITS["children"]:
        raise BoundsError(f"{len(children)} children (1..{LIMITS['children']})")
    fields = bytearray(sources[0].root.fields)
    struct.pack_into("<HH", fields, 0xA0, len(children), len(children))
    root = Bound(COMPOSITE, fields, children=children)
    identity = b"".join(struct.pack("<3fI", *[float(r == k) for k in range(3)], MATRIX_W[r]) for r in range(4))
    root.matrices, root.matrices2, root.matrices_alias = identity * len(children), None, True
    root.child_flags, root.child_flags2, root.flags_alias = flags, None, True
    return Resource(root, b"", {})


def merge(
    blobs: list[bytes],
    delta=(0.0, 0.0, 0.0),
    include_flags=None,
    type_flags=None,
    link: bool = True,
    tags: dict[int, int] | None = None,
) -> tuple[bytes, dict]:
    """Several PC .ybn (or .pbn) composites -> one PS5 .pbn composite: each source is converted as by
    convert() (weld + neighbours, retail fields, identity matrices required), their children are
    concatenated with their own flag pairs, then translated; the composite gets the retail box,
    sphere and mass fields and, from 6 children, a composite BVH. Read back and checked like convert()."""
    if not blobs:
        raise BoundsError("nothing to merge")
    notes: list[str] = []
    models = []
    for k, blob in enumerate(blobs):
        source = decode(blob)
        if source.root.kind != COMPOSITE:
            raise BoundsError(f"input {k}: root is a {NAMES[source.root.kind]}: the store needs composites")
        check(source, source=True)
        model = decode(blob)
        notes += [f"input {k}: {n}" for n in prepare_pc(model, link)]
        notes += [f"input {k}: {n}" for n in normalize(model, include_flags, type_flags, tags)]
        models.append(model)
    source = _merged([decode(blob) for blob in blobs])  # untouched, for the readback comparison
    model = _merged(models)
    translate(model, delta)
    normalize(model, tags=tags)
    composite_mass(model.root)
    model.root.tree = composite_bvh(model.root) if len(model.root.children) >= COMPOSITE_BVH_MIN else None
    out = encode(model)
    readback = decode(out)
    summary = check(readback)
    report = geometry_report(source, readback, delta)
    if diff(semantic(readback), semantic(model)):
        raise BoundsError("readback differs from the model written")
    return out, {
        "inputs": len(blobs),
        "children": len(model.root.children),
        "compositeBvh": model.root.tree is not None,
        "notes": notes,
        "summary": summary,
        **report,
        "bytes": len(out),
    }


# --------------------------------------------------------------------------------------------------
# CLI


def describe(resource: Resource) -> str:
    lines = [
        f"version={resource.header['version']} flags={resource.header['systemFlags']} "
        f"system={resource.header['systemBytes']} pages={resource.source_pages}"
    ]

    def one(b: Bound, indent: str, flags: str = "") -> None:
        lo, hi = b.vec(0x30), b.vec(0x20)
        text = (
            f"{indent}{NAMES[b.kind]} tag={b.u('<I', 0):#x} box=({lo[0]:.2f},{lo[1]:.2f},{lo[2]:.2f})-"
            f"({hi[0]:.2f},{hi[1]:.2f},{hi[2]:.2f}) margin={b.u('<f', 0x2C):.4f}{flags}"
        )
        if b.kind in (GEOMETRY, BVH):
            nmat = b.fields[0x120]
            mats = [struct.unpack_from("<II", b.arrays["materials"], 8 * k) for k in range(nmat)]
            rooms = Counter((d1 >> 16) & 0x1F for d1, _ in mats)
            kinds = Counter(POLY_NAMES[b.arrays["polygons"][16 * i] & 7] for i in range(b.u("<I", 0xD4)))
            text += (
                f" vertices={b.u('<I', 0xD0)} polygons={dict(kinds)} materials={nmat}"
                f" material_types={sorted({d1 & 0xFF for d1, _ in mats})} rooms={dict(rooms)}"
                f" colours={b.fields[0x121]} vertex_colours={b.arrays.get('vertex_colours') is not None}"
                f" octants={b.octants is not None}"
            )
        if b.tree:
            text += f" bvh_nodes={b.tree.node_count}/{b.tree.capacity} trees={len(b.tree.trees) // 16}"
        lines.append(text)
        for i, child in enumerate(b.children):
            f = ""
            if b.child_flags:
                pair = struct.unpack_from("<2I", b.child_flags, 8 * i)
                f = f" flags={pair[0]:#x}/{pair[1]:#x}"
            one(child, indent + "  ", f)

    one(resource.root, "")
    return "\n".join(lines)


def _write(path: Path, blob: bytes, force: bool) -> None:
    if path.exists() and not force:
        raise SystemExit(f"refusing to overwrite {path} (--force)")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def convert(
    blob: bytes,
    delta=(0.0, 0.0, 0.0),
    include_flags=None,
    type_flags=None,
    link: bool = True,
    tags: dict[int, int] | None = None,
) -> tuple[bytes, dict]:
    """PC .ybn bytes -> (PS5 .pbn bytes, report). The output is decoded and compared before return."""
    source = decode(blob)
    if source.root.kind != COMPOSITE:
        raise BoundsError(f"root is a {NAMES[source.root.kind]}: the static bounds store needs a composite root")
    check(source, source=True)
    model = decode(blob)
    notes = prepare_pc(model, link)
    notes += normalize(model, include_flags, type_flags, tags)
    translate(model, delta)
    out = encode(model)
    readback = decode(out)
    summary = check(readback)
    report = geometry_report(source, readback, delta)
    if diff(semantic(readback), semantic(model)):
        raise BoundsError("readback differs from the model written")
    return out, {"notes": notes, "summary": summary, **report, "bytes": len(out)}


def roundtrip(blob: bytes) -> tuple[bytes, dict]:
    """Retail .pbn -> model -> .pbn; byte comparison of the inflated payloads plus semantic diff."""
    model = decode(blob)
    check(model)
    out = encode(model)
    again = decode(out)
    differences = diff(semantic(model), semantic(again))
    _, payload_in = decode_resource(blob, 1 << 28)
    _, payload_out = decode_resource(out, 1 << 28)
    same_flags = blob[4:16] == out[4:16]
    return out, {
        "semantic_differences": differences,
        "payload_identical": payload_in == payload_out,
        "header_identical": same_flags,
        "pages_in": model.source_pages,
        "pages_out": again.source_pages,
    }


def _class_tags(args, default_tags: dict[int, int] | None) -> dict[int, int]:
    if args.template:
        return read_tags(args.template)
    if default_tags is not None:
        return dict(default_tags)
    raise BoundsError(
        f"{args.command} needs --template: a retail .pbn (and a drawable for box/geometry children) from your"
        " own game; ./menu-ctl.sh fetch-templates caches them under bounds-templates/"
    )


def main(argv: list[str] | None = None, default_tags: dict[int, int] | None = None) -> int:
    """CLI. `default_tags` is a developer hook (class tags used without --template); the published
    tool has none, so convert and merge need --template."""
    template_help = "bounds from your own game supplying the class tags (.pbn or drawable; repeatable)"
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("inspect")
    p.add_argument("file", type=Path)
    p = sub.add_parser("roundtrip")
    p.add_argument("file", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--force", action="store_true")
    for name in ("convert", "translate"):
        p = sub.add_parser(name)
        p.add_argument("file", type=Path)
        p.add_argument("--output", type=Path, required=True)
        p.add_argument("--translate", type=float, nargs=3, default=(0.0, 0.0, 0.0), metavar=("DX", "DY", "DZ"))
        p.add_argument("--force", action="store_true")
        if name == "convert":
            p.add_argument("--template", type=Path, action="append", default=[], help=template_help)
            p.add_argument("--include", type=lambda s: int(s, 0), help="override every child's include flags")
            p.add_argument("--type-flags", type=lambda s: int(s, 0), help="override every child's type flags")
            p.add_argument("--report", type=Path, help="write the conversion report as JSON")
            p.add_argument(
                "--keep-neighbours", action="store_true", help="keep the source vertices and neighbour indices"
            )
    p = sub.add_parser("merge")
    p.add_argument("files", type=Path, nargs="+")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--template", type=Path, action="append", default=[], help=template_help)
    p.add_argument("--translate", type=float, nargs=3, default=(0.0, 0.0, 0.0), metavar=("DX", "DY", "DZ"))
    p.add_argument("--include", type=lambda s: int(s, 0), help="override every child's include flags")
    p.add_argument("--type-flags", type=lambda s: int(s, 0), help="override every child's type flags")
    p.add_argument("--report", type=Path, help="write the merge report as JSON")
    p.add_argument("--keep-neighbours", action="store_true", help="keep the source vertices and neighbour indices")
    p.add_argument("--max-bytes", type=int, default=MAX_MEMBER_BYTES, help="refuse a larger output (archive member)")
    p.add_argument("--force", action="store_true")
    p = sub.add_parser("probe")
    p.add_argument("file", type=Path)
    p.add_argument("x", type=float)
    p.add_argument("y", type=float)
    p.add_argument("--limit", type=int, default=12)
    p = sub.add_parser("compare")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p = sub.add_parser("tags")
    p.add_argument("template", type=Path, nargs="+")
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect":
            resource = decode(args.file.read_bytes())
            print(describe(resource))
            print(json.dumps(check(resource)["children"][:8]))
        elif args.command == "roundtrip":
            out, report = roundtrip(args.file.read_bytes())
            print(json.dumps(report))
            if args.output:
                _write(args.output, out, args.force)
            return 0 if not report["semantic_differences"] else 1
        elif args.command == "convert":
            tags = _class_tags(args, default_tags)
            out, report = convert(
                args.file.read_bytes(),
                tuple(args.translate),
                args.include,
                args.type_flags,
                not args.keep_neighbours,
                tags,
            )
            print(describe(decode(out)))
            print(json.dumps({k: v for k, v in report.items() if k != "summary"}))
            _write(args.output, out, args.force)
            if args.report:
                args.report.write_text(json.dumps(report, indent=1) + "\n")
            print(f"wrote {args.output} ({len(out)} bytes)")
        elif args.command == "translate":
            source = decode(args.file.read_bytes())
            check(source)
            model = decode(args.file.read_bytes())
            translate(model, tuple(args.translate))
            out = encode(model)
            readback = decode(out)
            check(readback)
            report = geometry_report(source, readback, tuple(args.translate))
            if diff(semantic(readback), semantic(model)):
                raise BoundsError("readback differs from the model written")
            print(describe(readback))
            print(json.dumps(report))
            _write(args.output, out, args.force)
            print(f"wrote {args.output} ({len(out)} bytes)")
        elif args.command == "merge":
            tags = _class_tags(args, default_tags)
            out, report = merge(
                [path.read_bytes() for path in args.files],
                tuple(args.translate),
                args.include,
                args.type_flags,
                not args.keep_neighbours,
                tags,
            )
            if len(out) > args.max_bytes:
                raise BoundsError(f"merged output is {len(out)} bytes (> {args.max_bytes}): merge fewer files")
            report["sources"] = [path.name for path in args.files]
            print(describe(decode(out)).splitlines()[0])
            print(json.dumps({k: v for k, v in report.items() if k not in ("summary", "notes", "geometry")}))
            _write(args.output, out, args.force)
            if args.report:
                args.report.write_text(json.dumps(report, indent=1) + "\n")
            print(f"wrote {args.output} ({len(out)} bytes, {report['children']} children)")
        elif args.command == "probe":
            for z, label in probe(decode(args.file.read_bytes()), args.x, args.y)[: args.limit]:
                print(f"z={z:.3f} {label}")
        elif args.command == "compare":
            a, b = decode(args.a.read_bytes()), decode(args.b.read_bytes())
            differences = diff(semantic(a), semantic(b))
            print("\n".join(differences[:40]) or "semantically identical")
            return 1 if differences else 0
        elif args.command == "tags":
            tags = read_tags(args.template)
            for kind in (COMPOSITE, BOX, GEOMETRY, BVH):
                print(f"{NAMES[kind]:9} {f'{tags[kind]:#010x}' if kind in tags else 'missing (no such bound)'}")
    except BoundsError as exc:
        raise SystemExit(f"convert_pc_bounds: {exc}") from exc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
