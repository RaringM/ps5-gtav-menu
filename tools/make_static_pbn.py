#!/usr/bin/env python3
"""Write a PS5 static-bounds .pbn (resource v43): a composite root (type 10) of world-space box children.

The static bounds store creates physics only for a composite root: one instance per child from the
children array (+0x70), child count u16 +0xa2, per-child {u32 type, u32 include} flags at +0x90. A
non-interior static child gets an identity instance matrix and the store box is the union of the
children's own +0x20/+0x30 boxes, so children are written in world space with identity child
matrices; boxes are therefore axis-aligned (heading must be a multiple of 90 degrees).

--template is a composite .pbn taken from the user's own game, a drawable that embeds a composite
bound at +0xC8, or a .pbn this tool wrote before; nothing of it ships with this tool. Fields not
derived from the geometry are copied from it: the bound object tags, the +0x04 word, the per-child
type/include pair (the most common pair among its box children) and the matrix w-words. A template
without box children cannot supply the box tag and is refused. The composite BVH (+0xa8) is left
NULL: the store inserts each child as its own instance. Output is a loose RSC7 (stored deflate),
read back and compared with the template's layout before it is written. Refuses to overwrite.

spec.json:
  {"name": "gmcol_box01",
   "boxes": [{"centre": [-70, -1765, 28.5], "half_extents": [2, 2, 0.5], "heading": 0, "material": 1}]}
Optional per box: heading (degrees about +Z, multiple of 90; 0), material (index or name; concrete).

  make_static_pbn.py spec.json --template T.pbn --output gmcol_box01.pbn
  make_static_pbn.py --check FILE.pbn [--template T.pbn]   # read back (+ compare with the template)
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import zlib
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from gtavmenu_tools.bounds import View, composite_info
from gtavmenu_tools.meta_resource import stored_deflate
from make_pmap import flags_for_pages

__all__ = ["BoundsError", "Box", "Template", "boxes_of", "build", "compare_layout", "main", "read_template", "verify"]

VERSION = 43  # static bounds resource version; header a=0x2002xxxx b=0xb0000000
SYS = 0x50000000
COMPOSITE, BOX = 10, 3
COMPOSITE_SIZE, BOX_SIZE, MATRIX_SIZE, BB_SIZE = 0xB0, 0x70, 0x40, 0x20
PAGE_MIN = 0x2000  # one 8 KiB system page (flags 0x20020000/0xb0000000)
MAX_BOXES = 256
MARGIN_MAX, MARGIN_SCALE = 0.04, 0.25  # box margin = min(0.04, smallest half-extent / 4)
MATRIX_W = (1, 0, 0, 0)  # u32 w-words of the four child matrix rows when the template has no matrices
# materials.dat order; 57/60 are the metal materials of a dumpster's boxes.
MATERIALS = {"default": 0, "concrete": 1, "tarmac": 4, "metal_solid_large": 57, "metal_hollow_large": 60}


class BoundsError(ValueError):
    """The spec, the template or a written .pbn is outside what this writer supports."""


@dataclass(frozen=True)
class Template:
    source: str
    composite_vft: int  # bound object tags, copied verbatim from the template
    box_vft: int
    word04: int  # u32 after the tag (1 in every game bound)
    type_flags: int
    include_flags: int
    matrix_w: tuple[int, int, int, int]  # u32 w-words of the four matrix rows


@dataclass(frozen=True)
class Box:
    centre: tuple[float, float, float]
    half: tuple[float, float, float]
    material: int

    @property
    def lo(self) -> tuple[float, ...]:
        return tuple(c - h for c, h in zip(self.centre, self.half, strict=True))

    @property
    def hi(self) -> tuple[float, ...]:
        return tuple(c + h for c, h in zip(self.centre, self.half, strict=True))

    @property
    def margin(self) -> float:
        return min(MARGIN_MAX, MARGIN_SCALE * min(self.half))

    @property
    def volume(self) -> float:
        return 8.0 * self.half[0] * self.half[1] * self.half[2]

    @property
    def unit_inertia(self) -> tuple[float, float, float]:
        a, b, c = (2.0 * h for h in self.half)
        return ((b * b + c * c) / 12.0, (a * a + c * c) / 12.0, (a * a + b * b) / 12.0)


def _vec3(value, what: str) -> tuple[float, float, float]:
    if not isinstance(value, list | tuple) or len(value) != 3:
        raise BoundsError(f"{what} needs three numbers")
    out = tuple(float(v) for v in value)
    if not all(math.isfinite(v) for v in out):
        raise BoundsError(f"{what} is not finite")
    return out  # type: ignore[return-value]


def _material(value) -> int:
    if isinstance(value, str):
        if value.lower() not in MATERIALS:
            raise BoundsError(f"material {value!r} unknown; use an index or one of {sorted(MATERIALS)}")
        return MATERIALS[value.lower()]
    index = int(value)
    if not 0 <= index <= 0xFF:
        raise BoundsError(f"material index {index} outside 0..255")
    return index


def boxes_of(spec: dict) -> list[Box]:
    items = spec.get("boxes")
    if not isinstance(items, list) or not 1 <= len(items) <= MAX_BOXES:
        raise BoundsError(f"spec needs 1..{MAX_BOXES} boxes")
    out = []
    for n, item in enumerate(items):
        centre = _vec3(item.get("centre", item.get("center")), f"box {n} centre")
        half = _vec3(item.get("half_extents"), f"box {n} half_extents")
        if min(half) <= 0:
            raise BoundsError(f"box {n} half_extents must be positive")
        heading = float(item.get("heading", 0.0))
        quarter = heading / 90.0
        if abs(quarter - round(quarter)) > 1e-6:
            raise BoundsError(
                f"box {n} heading {heading} is not a multiple of 90: static children are world-space boxes with"
                " an identity instance matrix (rotated boxes need a geometry child)"
            )
        if round(quarter) % 2:
            half = (half[1], half[0], half[2])
        out.append(Box(centre, half, _material(item.get("material", "concrete"))))
    return out


def _resource(blob: bytes) -> tuple[View, int, bool]:
    """(view, composite offset, is_file_root) of a .pbn or of a drawable's embedded composite."""
    view = View(blob)
    if view.payload[0x10] == COMPOSITE:
        return view, 0, True
    bound = view.u("<Q", 0xC8) if len(view.payload) > 0xD0 else 0
    if bound and view.payload[view.off(bound) + 0x10] == COMPOSITE:
        return view, view.off(bound), False
    raise BoundsError("template root is neither a composite .pbn nor a drawable with an embedded composite")


def read_template(path: Path, fallback: Template | None = None) -> Template:
    """Format fields of the composite in `path` (a .pbn or a drawable embedding one).

    `fallback` supplies the box tag when the template has no box children; without it such a
    template is refused.
    """
    view, root, _ = _resource(path.read_bytes())
    info = composite_info(view, root)
    pairs = [c["flags"] for c in info["children"] if c["pointer"] and "flags" in c]
    if not pairs:
        raise BoundsError(f"{path.name}: composite has no per-child type/include flags")
    boxes = [c for c in info["children"] if c.get("type") == BOX]
    if not boxes and fallback is None:
        raise BoundsError(f"{path.name}: composite has no box child to copy the box tag from")
    ranked = [c["flags"] for c in boxes] or pairs
    type_flags, include_flags = max(set(ranked), key=lambda pair: (ranked.count(pair), pair[0]))
    matrix_w = _matrix_w(view, root, 0) if view.u("<Q", root + 0x78) else MATRIX_W
    return Template(
        source=str(path),
        composite_vft=view.u("<I", root),
        box_vft=view.u("<I", boxes[0]["offset"]) if boxes else fallback.box_vft,
        word04=view.u("<I", root + 4),
        type_flags=type_flags,
        include_flags=include_flags,
        matrix_w=matrix_w,  # type: ignore[arg-type]
    )


def _align(value: int) -> int:
    return (value + 15) & ~15


def _put_bound_header(buf: bytearray, at: int, vft: int, word04: int, kind: int, lo, hi, margin: float) -> None:
    centre = [(a + b) / 2 for a, b in zip(lo, hi, strict=True)]
    radius = math.dist(centre, hi)
    struct.pack_into("<II", buf, at, vft, word04)
    buf[at + 0x10] = kind
    struct.pack_into("<f", buf, at + 0x14, radius)
    struct.pack_into("<3ff", buf, at + 0x20, *hi, margin)
    struct.pack_into("<3fI", buf, at + 0x30, *lo, 1)  # +0x3c: reference count, 1 in every game bound
    struct.pack_into("<3f", buf, at + 0x40, *centre)


def build(spec: dict, template: Template) -> bytes:
    """Return a loose RSC7 .pbn (stored deflate) for the spec's boxes."""
    boxes = boxes_of(spec)
    n = len(boxes)
    layout, at = {}, 0
    for name, size in (
        ("composite", COMPOSITE_SIZE),
        ("pages", 0x10 + 8),
        ("children", 8 * n),
        ("matrices", MATRIX_SIZE * n),
        ("bbs", BB_SIZE * n),
        ("flags", 8 * n),
        ("boxes", BOX_SIZE * n),
    ):
        layout[name] = at = _align(at)
        at += size
    page = max(PAGE_MIN, 1 << (at - 1).bit_length())
    buf = bytearray(page)

    lo = [min(b.lo[k] for b in boxes) for k in range(3)]
    hi = [max(b.hi[k] for b in boxes) for k in range(3)]
    volume = sum(b.volume for b in boxes)
    cg = [sum(b.volume * b.centre[k] for b in boxes) / volume for k in range(3)]
    inertia = [0.0, 0.0, 0.0]
    for b in boxes:  # volume-weighted, parallel axis about the composite centre of gravity [L; static: unused]
        d = [b.centre[k] - cg[k] for k in range(3)]
        for k in range(3):
            inertia[k] += b.volume * (b.unit_inertia[k] + d[(k + 1) % 3] ** 2 + d[(k + 2) % 3] ** 2)
    root = layout["composite"]
    _put_bound_header(buf, root, template.composite_vft, template.word04, COMPOSITE, lo, hi, 0.0)
    struct.pack_into("<Q", buf, root + 0x08, SYS + layout["pages"])  # file root: pages info
    struct.pack_into("<3f", buf, root + 0x50, *cg)
    struct.pack_into("<3ff", buf, root + 0x60, *(i / volume for i in inertia), volume)
    struct.pack_into(
        "<6Q",
        buf,
        root + 0x70,
        SYS + layout["children"],
        SYS + layout["matrices"],
        SYS + layout["matrices"],  # +0x80 aliases +0x78, as in game files
        SYS + layout["bbs"],
        SYS + layout["flags"],
        SYS + layout["flags"],  # +0x98 aliases +0x90, as in game files
    )
    struct.pack_into("<HH", buf, root + 0xA0, n, n)  # +0xa8 BVH stays NULL
    buf[layout["pages"] + 8] = 1  # ResourcePagesInfo: one system page, no graphics pages

    for i, b in enumerate(boxes):
        child = layout["boxes"] + BOX_SIZE * i
        _put_bound_header(buf, child, template.box_vft, template.word04, BOX, b.lo, b.hi, b.margin)
        buf[child + 0x4C] = b.material
        struct.pack_into("<3f", buf, child + 0x50, *b.centre)  # centre of gravity
        struct.pack_into("<3ff", buf, child + 0x60, *b.unit_inertia, b.volume)
        struct.pack_into("<Q", buf, layout["children"] + 8 * i, SYS + child)
        matrix = layout["matrices"] + MATRIX_SIZE * i
        for row in range(4):  # identity rotation, zero translation: the child is already in world space
            struct.pack_into(
                "<3fI", buf, matrix + 0x10 * row, *[float(row == k) for k in range(3)], template.matrix_w[row]
            )
        struct.pack_into("<3fI3ff", buf, layout["bbs"] + BB_SIZE * i, *b.lo, 1, *b.hi, b.margin)
        struct.pack_into("<II", buf, layout["flags"] + 8 * i, template.type_flags, template.include_flags)

    low = flags_for_pages([page])
    header = struct.pack("<4sIII", b"RSC7", VERSION, (VERSION >> 4) << 28 | low, (VERSION & 15) << 28)
    return header + stored_deflate(bytes(buf))


def verify(blob: bytes, boxes: list[Box] | None = None, template: Template | None = None) -> dict:
    """Read a written .pbn back; raise BoundsError on any structural problem. Returns a summary."""
    view, root, is_root = _resource(blob)
    if not is_root or root:
        raise BoundsError("root at offset 0 is not a composite")
    if view.header["version"] != VERSION or view.header["graphicsBytes"]:
        raise BoundsError(f"header version {view.header['version']} / graphics {view.header['graphicsBytes']:#x}")
    stream = zlib.decompressobj(-15)
    if stream.decompress(blob[16:]) != view.payload or not stream.eof:
        raise BoundsError("payload does not inflate cleanly")
    p = view.payload
    pages = view.off(view.u("<Q", 0x08))
    if (p[pages + 8], p[pages + 9]) != (1, 0):
        raise BoundsError("pages info is not one system page")
    info = composite_info(view, 0)
    problems = []
    if info["count1"] != info["count2"] or not info["count2"]:
        problems.append(f"counts +0xa0={info['count1']} +0xa2={info['count2']}")
    if not (info["matrices_alias"] and info["flags_alias"]):
        problems.append("+0x80/+0x98 do not alias +0x78/+0x90")
    if info["bvh"]:
        problems.append("unexpected composite BVH")
    if boxes is not None and len(boxes) != info["count2"]:
        problems.append(f"{info['count2']} children, expected {len(boxes)}")
    lo, hi = [math.inf] * 3, [-math.inf] * 3
    for i, child in enumerate(info["children"]):
        if not child["pointer"] or child["type"] != BOX:
            problems.append(f"child {i} is not a box")
            continue
        if child["flags"] == (0, 0) or (template and child["flags"] != (template.type_flags, template.include_flags)):
            problems.append(f"child {i} type/include {child['flags']}")
        if child["bb_min"] != child["min"] or child["bb_max"] != child["max"]:
            problems.append(f"child {i} composite box differs from the child box")
        if any(
            abs(v - float(row == k)) > 0
            for row in range(4)
            for k, v in enumerate(child["matrix"][4 * row : 4 * row + 3])
        ):
            problems.append(f"child {i} matrix is not identity")
        centre = tuple((a + b) / 2 for a, b in zip(child["min"], child["max"], strict=True))
        if any(abs(a - b) > 1e-3 for a, b in zip(centre, child["centroid"], strict=True)):
            problems.append(f"child {i} centroid {child['centroid']} != box centre {centre}")
        if boxes is not None:
            want = boxes[i]
            got = (*child["min"], *child["max"])
            if any(abs(a - b) > 1e-3 for a, b in zip(got, (*want.lo, *want.hi), strict=True)):
                problems.append(f"child {i} box {got} != spec")
            if child["material"] != want.material:
                problems.append(f"child {i} material {child['material']} != {want.material}")
        lo = [min(a, b) for a, b in zip(lo, child["min"], strict=True)]
        hi = [max(a, b) for a, b in zip(hi, child["max"], strict=True)]
    root_lo, root_hi = struct.unpack_from("<3f", p, 0x30), struct.unpack_from("<3f", p, 0x20)
    if any(abs(a - b) > 1e-3 for a, b in zip((*root_lo, *root_hi), (*lo, *hi), strict=True)):
        problems.append("composite box is not the union of the children")
    if problems:
        raise BoundsError("; ".join(problems))
    return {
        "flags": f"{view.header['systemFlags']}/{view.header['graphicsFlags']}",
        "children": info["count2"],
        "type_include": sorted({f"{a:#x}/{b:#x}" for a, b in (c["flags"] for c in info["children"])}),
        "min": root_lo,
        "max": root_hi,
    }


# Offsets (relative to a bound) whose bytes are format, not geometry: they must equal the template's.
COMPOSITE_FIXED = ((0x00, 8), (0x10, 4), (0x18, 8), (0x2C, 4), (0x3C, 4), (0x4C, 4), (0x5C, 4), (0xA4, 4))
BOX_FIXED = ((0x00, 16), (0x10, 4), (0x18, 8), (0x3C, 4), (0x4D, 3), (0x5C, 4))


def compare_layout(blob: bytes, template_blob: bytes) -> list[str]:
    """Differences between `blob` and a template outside the geometry fields (empty = same layout).

    Geometry fields (radius, boxes, margins, centroid, centre of gravity, inertia, volume, material, child
    count) are allowed to differ; so is +0x08 when the template is a drawable's embedded composite (no
    pages info) and the composite BVH, which is reported as a note when only the template has one.
    """
    ours, root, _ = _resource(blob)
    theirs, troot, _ = _resource(template_blob)
    diffs = []
    for offset, size in COMPOSITE_FIXED:  # +0x08 (pages info of a file root, 0 when embedded) is not compared
        a = ours.payload[root + offset : root + offset + size]
        b = theirs.payload[troot + offset : troot + offset + size]
        if a != b:
            diffs.append(f"composite +{offset:#x}: {a.hex()} != {b.hex()}")
    mine, other = composite_info(ours, root), composite_info(theirs, troot)
    for key in ("type", "matrices_alias", "flags_alias"):
        if mine[key] != other[key]:
            diffs.append(f"composite {key}: {mine[key]} != {other[key]}")
    if (mine["count1"] == mine["count2"]) != (other["count1"] == other["count2"]):
        diffs.append("count +0xa0/+0xa2 relation differs")
    template_boxes = [c for c in other["children"] if c.get("type") == BOX]
    pairs = [c["flags"] for c in template_boxes] or [c["flags"] for c in other["children"] if "flags" in c]
    for i, child in enumerate(mine["children"]):
        if child.get("type") != BOX:
            diffs.append(f"child {i} is not a box")
            continue
        if child.get("flags") not in pairs:
            diffs.append(f"child {i} type/include {child.get('flags')} not used by the template")
        if template_boxes:
            ref = template_boxes[0]["offset"]
            for offset, size in BOX_FIXED:
                a = ours.payload[child["offset"] + offset : child["offset"] + offset + size]
                b = theirs.payload[ref + offset : ref + offset + size]
                if a != b:
                    diffs.append(f"child {i} +{offset:#x}: {a.hex()} != {b.hex()}")
        if other["children"] and "matrix" in other["children"][0]:
            w_ours, w_theirs = _matrix_w(ours, root, i), _matrix_w(theirs, troot, 0)
            if w_ours != w_theirs:
                diffs.append(f"child {i} matrix w-words {w_ours} != {w_theirs}")
    return diffs


def _matrix_w(view: View, composite: int, index: int) -> tuple[int, ...]:
    matrices = view.off(view.u("<Q", composite + 0x78)) + MATRIX_SIZE * index
    return tuple(view.u("<I", matrices + 0x10 * row + 0xC) for row in range(4))


def main(argv: list[str] | None = None, default_templates: Sequence[Path] = (), builtin: Template | None = None) -> int:
    """CLI. `default_templates` (first existing path) and `builtin` are developer hooks; the published
    tool has neither, so building needs --template."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spec", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", type=Path, metavar="FILE.pbn", help="read back an existing .pbn")
    parser.add_argument(
        "--template", type=Path, help="composite .pbn (or drawable embedding one) from the user's own game"
    )
    parser.add_argument("--force", action="store_true", help="overwrite --output")
    args = parser.parse_args(argv)
    template_path = args.template or next((p for p in default_templates if p.exists()), None)
    if not args.check and (not args.spec or not args.output):
        parser.error("spec and --output are required unless --check is given")
    if not args.check and template_path is None and builtin is None:
        parser.error("--template is required to build (a composite .pbn from your game, or one built before)")
    try:
        template = read_template(template_path, builtin) if template_path else builtin
        if template is not None:
            print(
                f"template: {template.source} type/include={template.type_flags:#x}/{template.include_flags:#x}"
                f" vft composite={template.composite_vft:#x} box={template.box_vft:#x}"
            )
        if args.check:
            blob = args.check.read_bytes()
            boxes = None
        else:
            spec = json.loads(args.spec.read_text())
            boxes = boxes_of(spec)
            blob = build(spec, template)
            if args.output.exists() and not args.force:
                raise SystemExit(f"refusing to overwrite {args.output} (--force)")
        summary = verify(blob, boxes, template)
        print(f"readback ok: {summary}")
        if template_path:
            diffs = compare_layout(blob, template_path.read_bytes())
            print(f"layout vs {template_path.name}: {diffs or 'same (geometry only differs)'}")
            if diffs:
                raise SystemExit(1)
    except BoundsError as exc:
        raise SystemExit(f"make_static_pbn: {exc}") from exc
    if not args.check:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(blob)
        print(f"wrote {args.output} ({len(blob)} bytes, {summary['children']} boxes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
