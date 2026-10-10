#!/usr/bin/env python3
"""Write an add-on .pmap (CMapData, PS5 meta resource v2) of any size from a JSON spec.

--template is a one-entity .pmap taken from the user's own game; nothing of it ships with this tool.
It must be one 8 KiB page: header @0, parser schema (struct/enum infos, member rows, enum values,
string blob) up to the page-info block, and three data blocks: 1 CMapData @0x70, 2 CEntityDef
@0x700, 3 the entity pointer array (block type 7) after the page info. The parser schema and the
entity defaults are copied from it at run time. Inside blocks a pointer is the block reference
(offset << 12) | block (1-based). Two layouts:

single  (hardware-proven; 1..40 entities): the template page with a new CEntityDef block
        and pointer array appended after its last used byte; block-table entries 2 and 3 re-pointed.
paged   (1..MAX_ENTITIES): a retail-style multi-page resource. The template's header and schema
        [0, 0x970) stay at offset 0 byte-for-byte (old entity and old block table zeroed). A new
        block table lists 1 CMapData, CEntityDef blocks of up to 128 entities (16 KiB, the meta block
        limit retail maps are paged by) and the pointer block; with the page-info block they are
        placed first-fit into 16 KiB pages (larger only when the pointer array needs it), no block
        crosses a page, the least-used page shrinks (>= 8 KiB) and goes last, and the flags use the
        retail 512-byte base (300 entities -> 0x1800 = 3 x 16 KiB). Retail metadata maps are 1-16
        pages of 8-64 KiB (base 512) with up to ~300 KiB of system data (~2000 entities).
auto    (default): single when the entities fit it (output byte-identical to the proven writer),
        else paged.

Both layouts: entitiesExtents = union of the entity boxes (the archetype's local `bbox` turned by
heading, scaled and moved to position; without one, position +/- radius * scale), streamingExtents
= union of each entity box grown by that entity's own lodDist (the map must be streamed in wherever
any entity can be seen, and no further); entities stay ORPHANHD like the template (parentIndex -1,
lodLevel 5, no children, priority REQUIRED): each draws until the camera is lodDist away, with no
LOD parent taking over, so a far-visible object needs a large `lod` (e.g. 600 for a 56 m crane seen
from 300 m) and its archetype's `bbox` (a tall object's top must be inside the extents). Every output
is parsed back by verify() (every pointer and block inside one page, page info, block references,
LOD fields, extents that hold every entity and its lodDist, no stray bytes for paged output);
`--check FILE` runs the same readback on any .pmap.

spec.json ("grids" expand x fastest, then y, then z; count/step take 2 or 3 values):
  {"name": "gmmap_davis2",
   "entities": [{"archetype": "prop_container_01a", "position": [-89.4, -1771.2, 29.2], "heading": 91.9},
                {"archetype": "gmprop_dumpster", "position": [-84.0, -1765.0, 28.0], "lod": 120, "radius": 2}],
   "grids": [{"archetype": "prop_roadcone02a", "origin": [-81, -1786, 27.9], "count": [20, 15],
              "step": [1, 1], "radius": 1}]}
Optional per entity/grid: heading (degrees about +Z, 0), lod (lodDist in metres, 150), radius (4),
scale (1), bbox ([[minx, miny, minz], [maxx, maxy, maxz]], the archetype's local bounding box from
its typ, e.g. prop_towercrane_01a [[-2.9, -44.06, 0], [2.93, 22.1, 56.12]]; replaces radius).
A spec-level "lod" sets the default lodDist for every entity and grid.
Converted maps (tools/convert_ymap.py) also use: rotation ([x, y, z, w], the CEntityDef quaternion
exactly as a ymap stores it, written normalized; replaces heading; its box is the union of the box
turned by the quaternion and by its inverse, so it holds either convention), flags (CEntityDef
flags, default 0), scaleZ (default scale) and tint (tintValue, default 0). An
archetype may be given as its hash, "hash_1a2b3c4d" (8 hex digits), when its name is unknown.

  make_pmap.py spec.json --template TEMPLATE.pmap --output gmmap_x.pmap [--layout auto|single|paged]
  make_pmap.py --check gmmap_x.pmap
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

from gtavmenu_tools.hashes import joaat
from gtavmenu_tools.meta_resource import RESOURCE_BASE, Meta, joaat_cs, stored_deflate

VERSION = 2
SYS_FLAGS, GFX_FLAGS = 0x00020000, 0x20000000  # retail entry flags: version 2, one 8 KiB page
ROOT_AT, ENTITY_AT, ENTITY_SIZE, PAGE = 0x70, 0x700, 0x80, 0x2000
ROOT_SIZE, HEADER_SIZE = 0x200, 0x70
MAX_ENTITIES = 40  # single layout (template page)
MAX_PAGED_ENTITIES = 4096  # ~2x the largest retail map; split bigger layouts into several maps by area
ENTITIES_PER_BLOCK = 128  # 128 * 0x80 = 16 KiB CEntityDef blocks
BLOCK_PAGE, MIN_PAGE = 0x4000, 0x2000
POINTER_BLOCK = 7  # block type of pointer arrays
ORPHAN_HD = 5  # rage__eLodType LODTYPES_DEPTH_ORPHANHD
PAGE_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))  # RSC7 page counts
INFO_HEADER, INFO_RECORD = 16, 8  # ResourcePagesInfo: u8 system count @+8, u8 graphics count @+9, 8 B/page


class MapError(ValueError):
    """The spec, the template or a written .pmap is outside what this writer supports."""


HASH_NAME = re.compile(r"hash_([0-9a-f]{8})\Z")


def archetype_hash(name: str) -> int:
    """joaat of an archetype name, or the value of a "hash_1a2b3c4d" literal."""
    match = HASH_NAME.fullmatch(name)
    return int(match.group(1), 16) if match else joaat(name)


def _rotate(q: tuple[float, float, float, float], v: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty, vy + w * ty + z * tx - x * tz, vz + w * tz + x * ty - y * tx)


@dataclass(frozen=True)
class Entity:
    archetype: str
    position: tuple[float, float, float]
    heading: float = 0.0
    lod: float = 150.0
    radius: float = 4.0
    scale: float = 1.0
    bbox: tuple[tuple[float, float, float], tuple[float, float, float]] | None = None
    rotation: tuple[float, float, float, float] | None = None  # stored quaternion, normalized
    flags: int | None = None
    scale_z: float | None = None
    tint: int = 0

    def box(self) -> tuple[list[float], list[float]]:
        """World axis-aligned box: the local bbox turned by heading about +Z (or by the rotation
        quaternion and its inverse), scaled, moved."""
        sz = self.scale if self.scale_z is None else self.scale_z
        if self.bbox is None:
            r = self.radius * max(self.scale, sz)
            return [v - r for v in self.position], [v + r for v in self.position]
        if self.rotation is not None:
            x, y, z, w = self.rotation
            corners = [
                (cx * self.scale, cy * self.scale, cz * sz)
                for cx in (self.bbox[0][0], self.bbox[1][0])
                for cy in (self.bbox[0][1], self.bbox[1][1])
                for cz in (self.bbox[0][2], self.bbox[1][2])
            ]
            turned = [_rotate(q, c) for q in ((x, y, z, w), (-x, -y, -z, w)) for c in corners]
            return (
                [p + min(t[a] for t in turned) for a, p in enumerate(self.position)],
                [p + max(t[a] for t in turned) for a, p in enumerate(self.position)],
            )
        (x0, y0, z0), (x1, y1, z1) = self.bbox
        c, s = math.cos(math.radians(self.heading)), math.sin(math.radians(self.heading))
        xs = [(x * c - y * s) * self.scale for x in (x0, x1) for y in (y0, y1)]
        ys = [(x * s + y * c) * self.scale for x in (x0, x1) for y in (y0, y1)]
        px, py, pz = self.position
        return (
            [px + min(xs), py + min(ys), pz + z0 * sz],
            [px + max(xs), py + max(ys), pz + z1 * sz],
        )


MAX_LOD = 16000.0  # metres; retail lodDist values stay far below


def _entity(item: dict, default_lod: float = 150.0) -> Entity:
    xyz = [float(v) for v in item["position"]]
    if len(xyz) != 3:
        raise MapError(f"entity {item!r}: position needs 3 values")
    bbox = None
    if "bbox" in item:
        corners = [[float(v) for v in corner] for corner in item["bbox"]]
        if len(corners) != 2 or any(len(c) != 3 for c in corners) or any(a > b for a, b in zip(*corners, strict=True)):
            raise MapError(f"entity {item!r}: bbox needs [[minx, miny, minz], [maxx, maxy, maxz]] with min <= max")
        bbox = ((corners[0][0], corners[0][1], corners[0][2]), (corners[1][0], corners[1][1], corners[1][2]))
    rotation = None
    if "rotation" in item:
        if "heading" in item:
            raise MapError(f"entity {item!r}: give heading or rotation, not both")
        q = [float(v) for v in item["rotation"]]
        norm = math.sqrt(sum(v * v for v in q)) if len(q) == 4 and all(math.isfinite(v) for v in q) else 0.0
        if abs(norm - 1.0) > 0.01:
            raise MapError(f"entity {item!r}: rotation needs a unit quaternion [x, y, z, w]")
        rotation = (q[0] / norm, q[1] / norm, q[2] / norm, q[3] / norm)
    flags = item.get("flags")
    tint = item.get("tint", 0)
    if flags is not None and (isinstance(flags, bool) or not isinstance(flags, int) or not 0 <= flags <= 0xFFFFFFFF):
        raise MapError(f"entity {item!r}: flags must be a u32")
    if isinstance(tint, bool) or not isinstance(tint, int) or not 0 <= tint <= 0xFFFFFFFF:
        raise MapError(f"entity {item!r}: tint must be a u32")
    entity = Entity(
        str(item["archetype"]),
        (xyz[0], xyz[1], xyz[2]),
        float(item.get("heading", 0.0)),
        float(item.get("lod", default_lod)),
        float(item.get("radius", 4.0)),
        float(item.get("scale", 1.0)),
        bbox,
        rotation,
        flags,
        float(item["scaleZ"]) if "scaleZ" in item else None,
        tint,
    )
    values = (*xyz, entity.heading, entity.lod, entity.radius, entity.scale, *(v for c in bbox or () for v in c))
    if not all(math.isfinite(v) for v in (*values, entity.scale_z or 1.0)):
        raise MapError(f"entity {item!r}: values must be finite")
    if not entity.archetype or not 0 < entity.lod <= MAX_LOD or entity.radius < 0 or entity.scale <= 0:
        raise MapError(f"entity {item!r}: needs an archetype, 0 < lod <= {MAX_LOD:.0f}, radius >= 0, scale > 0")
    if entity.scale_z is not None and entity.scale_z <= 0:
        raise MapError(f"entity {item!r}: scaleZ must be > 0")
    if entity.archetype.startswith("hash_") and not HASH_NAME.fullmatch(entity.archetype):
        raise MapError(f"entity {item!r}: a hash archetype is hash_ and 8 lowercase hex digits")
    return entity


def entities_of(spec: dict) -> list[Entity]:
    """Explicit entities first, then every grid expanded x fastest, then y, then z."""
    default_lod = float(spec.get("lod", 150.0))
    out = [_entity(item, default_lod) for item in spec.get("entities", [])]
    for grid in spec.get("grids", []):
        count, step = list(grid["count"]), [float(v) for v in grid["step"]]
        if not 2 <= len(count) <= 3 or len(step) != len(count) or any(int(c) < 1 for c in count):
            raise MapError(f"grid {grid!r}: count/step need 2 or 3 matching values, counts >= 1")
        nx, ny, nz = (*(int(c) for c in count), 1)[:3]
        dx, dy, dz = (*step, 0.0)[:3]
        ox, oy, oz = (float(v) for v in grid["origin"])
        rest = {k: v for k, v in grid.items() if k not in ("origin", "count", "step")}
        for k in range(nz):
            for j in range(ny):
                for i in range(nx):
                    out.append(_entity({**rest, "position": (ox + i * dx, oy + j * dy, oz + k * dz)}, default_lod))
    return out


def page_sizes(flags: int) -> list[int]:
    """Largest-first system page sizes of the low 28 RSC7 flag bits."""
    return [
        (512 << (flags & 15)) << (8 - rank)
        for rank, (shift, width) in enumerate(PAGE_FIELDS)
        for _ in range((flags >> shift) & ((1 << width) - 1))
    ]


def flags_for_pages(sizes: list[int]) -> int:
    """Low 28 flag bits for a largest-first page list, preferring the retail 512-byte base."""
    for nibble in range(16):
        base, flags = 512 << nibble, nibble
        for rank, (shift, width) in enumerate(PAGE_FIELDS):
            count = sizes.count(base << (8 - rank))
            if count >= 1 << width:
                break
            flags |= count << shift
        else:
            if page_sizes(flags) == sizes:
                return flags
    raise MapError(f"no RSC7 flags express pages {sizes}")


def _pow2(value: int) -> int:
    return 1 << max(0, (value - 1).bit_length())


def _align(value: int) -> int:
    return (value + 15) & ~15


def pack_pages(objects: list[tuple[str, int]], page: int) -> tuple[dict[str, int], list[int]]:
    """First-fit (name, bytes) objects into `page`-byte pages, 16-aligned, none crossing a page.

    The first object lands at offset 0. The least-used page other than the first shrinks to the
    smallest power of two >= its use (>= 8 KiB) and goes last; with one page, that page shrinks.
    Returns {name: offset} in the concatenated (largest-first) space and the page sizes.
    """
    cursors: list[int] = []
    placed: list[tuple[str, int, int]] = []
    for name, size in objects:
        if not 0 < size <= page:
            raise MapError(f"{name} ({size} bytes) does not fit a {page}-byte page")
        for index, used in enumerate(cursors):  # noqa: B007 (index is used after the loop)
            at = _align(used)
            if at + size <= page:
                break
        else:
            index, at = len(cursors), 0
            cursors.append(0)
        cursors[index] = at + size
        placed.append((name, index, at))
    sizes = [page] * len(cursors)
    small = 0 if len(cursors) == 1 else min(range(1, len(cursors)), key=lambda i: (cursors[i], -i))
    sizes[small] = min(page, max(MIN_PAGE, _pow2(cursors[small])))
    order = [i for i in range(len(cursors)) if i != small] + [small]  # the root page (0) stays first
    starts, at = {}, 0
    for i in order:
        starts[i] = at
        at += sizes[i]
    return {name: starts[index] + offset for name, index, offset in placed}, [sizes[i] for i in order]


def _template(template: bytes) -> tuple[bytearray, Meta]:
    stream = zlib.decompressobj(-15)
    payload = bytearray(stream.decompress(template[16:]))
    if not stream.eof or len(payload) != PAGE:
        raise MapError("template is not one inflating 8 KiB page")
    meta = Meta(bytes(payload))
    hashes = [b[0] for b in meta.blocks]
    if hashes != [joaat_cs("CMapData"), joaat_cs("CEntityDef"), POINTER_BLOCK]:
        raise MapError("template blocks are not CMapData, CEntityDef, pointer array")
    if meta.blocks[0][2] != ROOT_AT or meta.blocks[1][2] != ENTITY_AT:
        raise MapError("template layout differs")
    return payload, meta


def extents(entities: list[Entity]) -> tuple[list[float], list[float], list[float], list[float]]:
    """(entities min, max, streaming min, max): the union of the entity boxes, and of each entity
    box grown by that entity's lodDist."""
    lo, hi, slo, shi = [math.inf] * 3, [-math.inf] * 3, [math.inf] * 3, [-math.inf] * 3
    for item in entities:
        box_lo, box_hi = item.box()
        box_lo = [min(a, b) for a, b in zip(box_lo, item.position, strict=True)]  # a bbox off the origin
        box_hi = [max(a, b) for a, b in zip(box_hi, item.position, strict=True)]
        for axis in range(3):
            lo[axis], hi[axis] = min(lo[axis], box_lo[axis]), max(hi[axis], box_hi[axis])
            slo[axis] = min(slo[axis], box_lo[axis] - item.lod)
            shi[axis] = max(shi[axis], box_hi[axis] + item.lod)
    return lo, hi, slo, shi


def _write_entity(payload: bytearray, at: int, template_entity: bytes, item: Entity, guid: int) -> None:
    payload[at : at + ENTITY_SIZE] = template_entity
    half = math.radians(item.heading) / 2.0
    struct.pack_into("<I", payload, at + 0x08, archetype_hash(item.archetype))
    struct.pack_into("<II", payload, at + 0x0C, item.flags or 0, guid)
    struct.pack_into("<3f", payload, at + 0x20, *item.position)
    rotation = item.rotation if item.rotation is not None else (0.0, 0.0, math.sin(half), math.cos(half))
    struct.pack_into("<4f", payload, at + 0x30, *rotation)
    struct.pack_into("<ff", payload, at + 0x40, item.scale, item.scale if item.scale_z is None else item.scale_z)
    struct.pack_into("<f", payload, at + 0x4C, item.lod)
    if item.tint:
        struct.pack_into("<I", payload, at + 0x78, item.tint)


def _write_root(payload: bytearray, spec: dict, entities: list[Entity], pointer_block: int) -> None:
    lo, hi, slo, shi = extents(entities)
    struct.pack_into("<III", payload, ROOT_AT + 0x08, joaat(spec["name"]), 0, int(spec.get("flags", 0)))
    struct.pack_into("<3f", payload, ROOT_AT + 0x40, *lo)
    struct.pack_into("<3f", payload, ROOT_AT + 0x50, *hi)
    struct.pack_into("<3f", payload, ROOT_AT + 0x20, *slo)
    struct.pack_into("<3f", payload, ROOT_AT + 0x30, *shi)
    struct.pack_into("<QHH", payload, ROOT_AT + 0x60, pointer_block, len(entities), len(entities))  # offset 0


def _build_single(spec: dict, entities: list[Entity], payload: bytearray, meta: Meta) -> bytes:
    used = len(payload.rstrip(b"\0"))
    entity_block = _align(used)
    pointer_block = entity_block + len(entities) * ENTITY_SIZE
    if len(entities) > MAX_ENTITIES or pointer_block + len(entities) * 8 > PAGE:
        raise MapError(f"entities do not fit the template page (single layout holds 1..{MAX_ENTITIES})")
    template_entity = bytes(payload[ENTITY_AT : ENTITY_AT + ENTITY_SIZE])
    for i, item in enumerate(entities):
        _write_entity(payload, entity_block + i * ENTITY_SIZE, template_entity, item, joaat(f"{spec['name']}:{i}"))
        struct.pack_into("<Q", payload, pointer_block + i * 8, ((i * ENTITY_SIZE) << 12) | 2)
    table = meta.off(meta.block_ptr)
    hashes = [b[0] for b in meta.blocks]
    size = len(entities) * ENTITY_SIZE
    struct.pack_into("<IIQ", payload, table + 16, hashes[1], size, RESOURCE_BASE | entity_block)
    struct.pack_into("<IIQ", payload, table + 32, hashes[2], len(entities) * 8, RESOURCE_BASE | pointer_block)
    _write_root(payload, spec, entities, 3)
    return bytes(payload)


def _build_paged(spec: dict, entities: list[Entity], payload: bytearray, meta: Meta) -> tuple[bytes, list[int]]:
    info_at = meta.off(meta.u64(0x08))
    table_at = meta.off(meta.block_ptr)
    if not ENTITY_AT + ENTITY_SIZE <= table_at < table_at + 16 * len(meta.blocks) <= info_at <= meta.blocks[2][2]:
        raise MapError("template schema does not end at its page-info block")
    prefix = bytearray(payload[:info_at])  # header, root block, schema, string blob
    template_entity = bytes(prefix[ENTITY_AT : ENTITY_AT + ENTITY_SIZE])
    prefix[ENTITY_AT : ENTITY_AT + ENTITY_SIZE] = bytes(ENTITY_SIZE)
    prefix[table_at : table_at + 16 * len(meta.blocks)] = bytes(16 * len(meta.blocks))

    chunks = [entities[i : i + ENTITIES_PER_BLOCK] for i in range(0, len(entities), ENTITIES_PER_BLOCK)]
    n_blocks = len(chunks) + 2  # CMapData, CEntityDef chunks, pointer array
    objects = [("prefix", len(prefix)), ("table", 16 * n_blocks), ("info", 0), ("pointers", 8 * len(entities))]
    objects += [(f"entities{j}", len(chunk) * ENTITY_SIZE) for j, chunk in enumerate(chunks)]
    page = max(BLOCK_PAGE, _pow2(max(size for _, size in objects)))
    count = 1
    for _ in range(8):  # the page-info block grows with the page count it describes
        objects[2] = ("info", INFO_HEADER + INFO_RECORD * count)
        offsets, sizes = pack_pages(objects, page)
        if len(sizes) == count:
            break
        count = len(sizes)
    else:
        raise MapError("page count does not settle")

    out = bytearray(sum(sizes))
    out[: len(prefix)] = prefix
    struct.pack_into("<Q", out, 0x08, RESOURCE_BASE | offsets["info"])
    struct.pack_into("<Q", out, 0x30, RESOURCE_BASE | offsets["table"])
    struct.pack_into("<H", out, 0x4C, n_blocks)
    out[offsets["info"] + 8] = len(sizes)  # system pages; graphics pages 0
    rows = [(joaat_cs("CMapData"), ROOT_SIZE, ROOT_AT)]
    rows += [(joaat_cs("CEntityDef"), len(c) * ENTITY_SIZE, offsets[f"entities{j}"]) for j, c in enumerate(chunks)]
    rows += [(POINTER_BLOCK, 8 * len(entities), offsets["pointers"])]
    for i, (struct_hash, size, at) in enumerate(rows):
        struct.pack_into("<IIQ", out, offsets["table"] + 16 * i, struct_hash, size, RESOURCE_BASE | at)
    index = 0
    for j, chunk in enumerate(chunks):
        for i, item in enumerate(chunk):
            at = offsets[f"entities{j}"] + i * ENTITY_SIZE
            _write_entity(out, at, template_entity, item, joaat(f"{spec['name']}:{index}"))
            struct.pack_into("<Q", out, offsets["pointers"] + 8 * index, ((i * ENTITY_SIZE) << 12) | (j + 2))
            index += 1
    _write_root(out, spec, entities, n_blocks)
    return bytes(out), sizes


def build(spec: dict, template: bytes, layout: str = "auto") -> bytes:
    """Loose RSC7 v2 .pmap (stored deflate) for `spec`; parsed back by verify() before it is returned."""
    entities = entities_of(spec)
    if not 1 <= len(entities) <= MAX_PAGED_ENTITIES:
        raise MapError(f"need 1..{MAX_PAGED_ENTITIES} entities, got {len(entities)}; split the map by area")
    if layout not in ("auto", "single", "paged"):
        raise MapError(f"unknown layout {layout!r}")
    payload, meta = _template(template)
    paged = layout == "paged" or (layout == "auto" and len(entities) > MAX_ENTITIES)
    if paged:
        body, sizes = _build_paged(spec, entities, payload, meta)
        flags = flags_for_pages(sizes)
    else:
        body, flags = _build_single(spec, entities, payload, meta), SYS_FLAGS
    resource = struct.pack("<4sIII", b"RSC7", VERSION, flags, GFX_FLAGS) + stored_deflate(body)
    report = verify(resource, entities, template)
    if paged and (report["stray"] or not report["canonicalFlags"]):
        raise MapError(f"paged output: {report['stray']} unreferenced nonzero bytes, flags {flags:#x}")
    return resource


def _payload(resource: bytes, entry: tuple[int, int] | None = None) -> tuple[bytes, int, list[int]]:
    """Loose RSC7, or an archive-stored member ([16-byte prefix][deflate]) with its TOC `entry` flags."""
    magic, version, sys_flags, gfx_flags = struct.unpack_from("<4sIII", resource, 0)
    if magic != b"RSC7":
        if entry is None:
            raise MapError("not a loose RSC7 resource; pass the archive entry flags")
        sys_flags, gfx_flags = entry
    elif version != VERSION:
        raise MapError(f"RSC7 version {version} is not {VERSION}")
    if ((sys_flags >> 28) << 4 | gfx_flags >> 28) != VERSION:
        raise MapError("entry flags do not encode version 2")
    if gfx_flags & 0x0FFFFFFF:
        raise MapError("map data has no graphics pages")
    stream = zlib.decompressobj(-15)
    payload = stream.decompress(resource[16:])
    sizes = page_sizes(sys_flags & 0x0FFFFFFF)
    if not stream.eof or len(payload) != sum(sizes) or not sizes:
        raise MapError(f"payload ({len(payload)} bytes) does not inflate to its pages {sizes}")
    return payload, sys_flags, sizes


def _spans(meta: Meta, payload: bytes, info: int, pages: int) -> list[tuple[str, int, int]]:
    """Every structure the header reaches: (label, offset, bytes)."""
    out = [("header", 0, HEADER_SIZE)]
    if meta.n_struct:
        structs = meta.off(meta.struct_ptr)
        out.append(("struct infos", structs, 0x20 * meta.n_struct))
        for i in range(meta.n_struct):
            at = structs + 0x20 * i
            members = struct.unpack_from("<H", payload, at + 0x1E)[0]
            out.append((f"members {i}", meta.off(meta.u64(at + 0x10)), 16 * members))
    if meta.n_enum:
        enums = meta.off(meta.enum_ptr)
        out.append(("enum infos", enums, 0x18 * meta.n_enum))
        for i in range(meta.n_enum):
            at = enums + 0x18 * i
            values = struct.unpack_from("<I", payload, at + 0x10)[0]
            out.append((f"enum values {i}", meta.off(meta.u64(at + 8)), 8 * values))
    if meta.u64(0x40):
        at = meta.off(meta.u64(0x40))
        out.append(("strings", at, 4 + struct.unpack_from(">I", payload, at)[0]))
    out.append(("block table", meta.off(meta.block_ptr), 16 * meta.n_block))
    out += [(f"block {i + 1}", at, size) for i, (_hash, size, at) in enumerate(meta.blocks)]
    out.append(("page info", info, INFO_HEADER + INFO_RECORD * pages))
    return out


def _check_spans(spans: list[tuple[str, int, int]], payload: bytes, sizes: list[int]) -> int:
    """Each structure lies inside one page and overlaps no other; returns the uncovered nonzero bytes."""
    bounds, at = [], 0
    for size in sizes:
        bounds.append((at, at + size))
        at += size
    covered = bytearray(len(payload))
    for label, start, length in spans:
        if not any(a <= start and start + length <= b for a, b in bounds):
            raise MapError(f"{label} {start:#x}+{length:#x} is outside one page")
        if any(covered[start : start + length]):
            raise MapError(f"{label} overlaps another structure")
        covered[start : start + length] = b"\1" * length
    return sum(1 for b, c in zip(payload, covered, strict=True) if b and not c)


def _check_entities(meta: Meta, payload: bytes, expected: list[Entity] | None) -> list[tuple[tuple[float, ...], float]]:
    """Root entities array -> pointer block -> CEntityDef slots: whole, distinct, ORPHANHD; returns
    (position, lodDist) per entity."""
    root = meta.blocks[0][2]
    ref, count, capacity = struct.unpack_from("<QHH", payload, root + 0x60)
    if not count and not ref:
        return []
    pointer_block = ref & 0xFFF
    if count != capacity or ref >> 12 or not 0 < pointer_block <= meta.n_block:
        raise MapError("root entities array is not a whole pointer block")
    if meta.blocks[pointer_block - 1][0] != POINTER_BLOCK or meta.blocks[pointer_block - 1][1] < 8 * count:
        raise MapError("entities array does not name a pointer block of its size")
    array, entity_hash = meta.ref(ref), joaat_cs("CEntityDef")
    seen, guids, positions = set(), set(), []
    for i in range(count):
        value = meta.u64(array + 8 * i)
        block, offset = value & 0xFFF, value >> 12
        if not 0 < block <= meta.n_block or meta.blocks[block - 1][0] != entity_hash:
            raise MapError(f"entity {i} does not reference a CEntityDef block")
        if offset % ENTITY_SIZE or offset + ENTITY_SIZE > meta.blocks[block - 1][1]:
            raise MapError(f"entity {i} lies outside its block")
        at = meta.ref(value)
        archetype, _flags, guid = struct.unpack_from("<III", payload, at + 0x08)
        position = struct.unpack_from("<3f", payload, at + 0x20)
        parent, lod, child_lod, level, children = struct.unpack_from("<iffII", payload, at + 0x48)
        if (parent, child_lod, level, children) != (-1, 0.0, ORPHAN_HD, 0) or not lod > 0:
            raise MapError(f"entity {i} is not an ORPHANHD entity without LOD parent/children")
        if expected is not None:
            item = expected[i]
            if (
                archetype != archetype_hash(item.archetype)
                or lod != struct.unpack("<f", struct.pack("<f", item.lod))[0]
            ):
                raise MapError(f"entity {i} archetype/lod readback differs")
            if any(abs(a - b) > 1e-3 * max(1.0, abs(b)) for a, b in zip(position, item.position, strict=True)):
                raise MapError(f"entity {i} position readback differs")
        seen.add(at)
        guids.add(guid)
        positions.append((position, lod))
    slots = sum(size // ENTITY_SIZE for h, size, _ in meta.blocks if h == entity_hash)
    if not slots == len(seen) == len(guids) == count or (expected is not None and len(expected) != count):
        raise MapError("entity blocks, references, guids and the spec disagree")
    return positions


def verify(
    resource: bytes,
    expected: list[Entity] | None = None,
    template: bytes | None = None,
    entry: tuple[int, int] | None = None,
) -> dict:
    """Parse a .pmap back: pages, every structure inside one page, block references, LOD fields, extents.

    Returns a summary; `stray` counts nonzero bytes no structure covers (0 for retail and paged output;
    the single layout leaves the template's old entity and pointer array unreferenced).
    """
    payload, flags, sizes = _payload(resource, entry)
    meta = Meta(payload)
    info = meta.off(meta.u64(0x08))
    if payload[info + 8] != len(sizes) or payload[info + 9] or any(payload[info + 10 : info + 16]):
        raise MapError("page info does not declare the system pages")
    spans = _spans(meta, payload, info, len(sizes))
    stray = _check_spans(spans, payload, sizes)
    root_hash, root_size, root = meta.blocks[0]
    if (root_hash, root_size, struct.unpack_from("<I", payload, 0x1C)[0]) != (joaat_cs("CMapData"), ROOT_SIZE, 1):
        raise MapError("block 1 is not the CMapData root")
    placed = _check_entities(meta, payload, expected)
    positions = [position for position, _ in placed]
    lo, hi = struct.unpack_from("<3f", payload, root + 0x40), struct.unpack_from("<3f", payload, root + 0x50)
    slo, shi = struct.unpack_from("<3f", payload, root + 0x20), struct.unpack_from("<3f", payload, root + 0x30)
    for position in positions:
        if not all(a <= p <= b for a, p, b in zip(lo, position, hi, strict=True)):
            raise MapError(f"entity at {position} is outside entitiesExtents")
    if positions and not all(s <= a and b <= t for s, a, b, t in zip(slo, lo, hi, shi, strict=True)):
        raise MapError("streamingExtents do not contain entitiesExtents")
    for position, lod in placed:
        # The map must be resident wherever the entity can be seen: position +/- lodDist (f32 slack).
        slack = 1e-3 * max(1.0, lod, *(abs(v) for v in position))
        if lod > 0 and not all(
            s <= p - lod + slack and p + lod - slack <= t for s, p, t in zip(slo, position, shi, strict=True)
        ):
            raise MapError(f"streamingExtents do not reach lodDist {lod:g} around the entity at {position}")
    if template is not None:
        reference = bytes(_template(template)[0])
        if meta.structs != Meta(reference).structs:
            raise MapError("parser schema differs from the template")
        for label, start, length in spans:
            schema = label.startswith(("struct infos", "members", "enum", "strings"))
            if schema and payload[start : start + length] != reference[start : start + length]:
                raise MapError(f"{label} bytes differ from the template")
    return {
        "flags": flags,
        "pages": sizes,
        "entities": len(positions),
        "blocks": meta.n_block,
        "bytes": len(payload),
        "stray": stray,
        "canonicalFlags": flags_for_pages(sizes) == flags & 0x0FFFFFFF,  # retail base-512 encoding
        "entitiesExtents": (lo, hi),
        "streamingExtents": (slo, shi),
        "lodDist": (min((d for _, d in placed), default=0.0), max((d for _, d in placed), default=0.0)),
    }


def _describe(report: dict) -> str:
    pages = ", ".join(f"{size // 1024} KiB" for size in report["pages"])
    boxes = []
    for lo, hi in (report["entitiesExtents"], report["streamingExtents"]):
        boxes.append(f"({lo[0]:.1f},{lo[1]:.1f},{lo[2]:.1f})-({hi[0]:.1f},{hi[1]:.1f},{hi[2]:.1f})")
    low, high = report["lodDist"]
    return (
        f"flags={report['flags']:#010x} pages=[{pages}] blocks={report['blocks']} "
        f"entities={report['entities']} entitiesExtents={boxes[0]} streamingExtents={boxes[1]} "
        f"lodDist={low:g}..{high:g} stray={report['stray']}"
    )


def main(argv: list[str] | None = None, default_template: Path | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spec", type=Path, nargs="?")
    parser.add_argument(
        "--template", type=Path, default=default_template, help="one-entity .pmap from the user's own game"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--layout", choices=("auto", "single", "paged"), default="auto")
    parser.add_argument("--check", type=Path, metavar="PMAP", help="parse an existing .pmap back and describe it")
    parser.add_argument(
        "--entry-flags",
        nargs=2,
        type=lambda v: int(v, 0),
        default=(SYS_FLAGS, GFX_FLAGS),
        metavar=("SYS", "GFX"),
        help="TOC flags of an archive-stored --check member (default: retail one 8 KiB page)",
    )
    args = parser.parse_args(argv)
    try:
        if args.check:
            print(f"{args.check}: {_describe(verify(args.check.read_bytes(), entry=tuple(args.entry_flags)))}")
            return 0
        if not args.spec or not args.output or not args.template:
            parser.error("spec, --template and --output are required unless --check is given")
        spec = json.loads(args.spec.read_text())
        resource = build(spec, args.template.read_bytes(), args.layout)
    except MapError as exc:
        raise SystemExit(f"make_pmap: {exc}") from exc
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(resource)
    report = verify(resource)
    print(f"wrote {args.output} map={spec['name']} bytes={len(resource)} {_describe(report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
