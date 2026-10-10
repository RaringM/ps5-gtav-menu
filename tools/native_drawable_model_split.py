#!/usr/bin/env python3
"""Split drawable models above 127 geometries into independently owned models.

The engine stores geometry indices in a signed byte. Each model keeps its first
127 geometries; remaining geometries are appended in models of at most 127 each.
Geometry order, shader mappings, skeleton bindings and per-geometry boxes survive
unchanged. New aggregate boxes are unions of their own geometries' boxes.
Single-page resources use verified zero slack; multi-page resources use the page
allocator. Resources already under the limit are written byte-identical.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import resource_page_layout  # noqa: E402
from fill_drawable_lods import drawables, encode  # noqa: E402
from gtavmenu_tools.resource_view import SYS, Resource  # noqa: E402

LIMIT = 127  # geometry indices 0..126 (0x7f and 0xff stay clear of the signed/none byte values)
LISTS = (0x50, 0x58, 0x60, 0x68)
MODEL_BYTES = 0x30
GEOMETRIES, COUNT1, COUNT2, BOUNDS, MAPPING, COUNT3 = 0x08, 0x10, 0x12, 0x18, 0x20, 0x2E
BOX = 32  # AABB_s: Vector4 Min, Vector4 Max


class SplitError(RuntimeError):
    pass


def boxes_of(r: Resource, model: int) -> tuple[list[bytes], bytes | None]:
    """(per-geometry boxes, aggregate box or None) of a model, as CodeWalker's DrawableModel reads them."""
    count = r.u(model, "<H", COUNT1)
    at = r.off(r.u(model, "<Q", BOUNDS))
    raw = r.payload[at : at + BOX * (count + (1 if count > 1 else 0))]
    if count == 1:
        return [raw], None
    return [raw[BOX * (i + 1) : BOX * (i + 2)] for i in range(count)], raw[:BOX]


def union(boxes: list[bytes], lanes_from: bytes | None) -> bytes:
    """Aggregate box: componentwise min/max of x, y, z; the fourth lanes are kept from the source aggregate."""
    rows = [struct.unpack("<8f", b) for b in boxes]
    low = [min(row[i] for row in rows) for i in range(3)]
    high = [max(row[i + 4] for row in rows) for i in range(3)]
    w_low, w_high = struct.unpack("<8f", lanes_from)[3::4] if lanes_from else (rows[0][3], rows[0][7])
    return struct.pack("<8f", *low, w_low, *high, w_high)


def rows_of(r: Resource, models: list[int]) -> list[tuple[int, int, bytes]]:
    """(geometry pointer, shader index, box) of every geometry of a model list, in order."""
    out = []
    for model in models:
        count = r.u(model, "<H", COUNT1)
        geometries, mapping = r.u(model, "<Q", GEOMETRIES), r.u(model, "<Q", MAPPING)
        boxes, _ = boxes_of(r, model)
        out += [(r.u(geometries, "<Q", 8 * i), r.u(mapping, "<H", 2 * i), boxes[i]) for i in range(count)]
    return out


def lod_lists(r: Resource) -> list[tuple[str, int, list[int]]]:
    """(label, collection, model pointers) of every non-null LOD list of every drawable."""
    out = []
    for name, d in drawables(r):
        for k, off in enumerate(LISTS):
            collection = r.u(d, "<Q", off)
            if not collection:
                continue
            items, count = r.u(collection, "<Q"), r.u(collection, "<H", 8)
            out.append((f"{name}.{'HMLV'[k]}", collection, [r.u(items, "<Q", 8 * i) for i in range(count)]))
    return out


def oversized(r: Resource) -> list[tuple[str, int, list[int]]]:
    return [row for row in lod_lists(r) if any(r.u(m, "<H", COUNT1) > LIMIT for m in row[2])]


def trailing_zeros_unreferenced(payload: bytearray, system: int) -> int:
    """End of the used bytes; refused when a pointer targets the zero bytes after it (an empty LOD
    collection is all zeros: the allocators below would overwrite it)."""
    used = len(payload.rstrip(b"\0"))
    values = struct.unpack_from(f"<{system // 8}Q", payload, 0)
    if any(SYS + used <= v < SYS + system for v in values):
        raise SplitError("a pointer targets the trailing zero region; split before the LOD lists are filled")
    return used


class Slack:
    """Single-page allocation in the zero bytes after the last used byte (fill_drawable_lods' route)."""

    def __init__(self, payload: bytearray, system: int):
        used = trailing_zeros_unreferenced(payload, system)
        self.cursor, self.end = (used + 0x40 + 15) & ~15, system

    def alloc(self, size: int, alignment: int = 16) -> int:
        at = (self.cursor + alignment - 1) & -alignment
        if at + size > self.end:
            raise SplitError(
                f"no slack for the split models in the single system page ({at + size:#x} > {self.end:#x}); "
                "convert with --multi-page / --parts-multi-page"
            )
        self.cursor = at + size
        return at


def split(r: Resource, payload: bytearray, alloc) -> list[str]:
    """Rewrite every oversized LOD list in `payload` (r is the unchanged source view). Returns log lines."""
    lines = []
    for label, collection, models in oversized(r):
        new_items: list[int] = []
        for index, model in enumerate(models):
            count = r.u(model, "<H", COUNT1)
            if r.u(model, "<H", COUNT2) != count or r.u(model, "<H", COUNT3) != count:
                raise SplitError(f"{label} model {index}: unequal geometry counts")
            if count <= LIMIT:
                new_items.append(model)
                continue
            boxes, aggregate = boxes_of(r, model)
            geometries, mapping, bounds = (r.u(model, "<Q", f) for f in (GEOMETRIES, MAPPING, BOUNDS))
            pointers = [r.u(geometries, "<Q", 8 * i) for i in range(count)]
            shaders = [r.u(mapping, "<H", 2 * i) for i in range(count)]
            chunks = [range(start, min(start + LIMIT, count)) for start in range(0, count, LIMIT)]
            header = bytes(r.payload[r.off(model) : r.off(model) + MODEL_BYTES])
            sizes = []
            for number, chunk in enumerate(chunks):
                n = len(chunk)
                box_rows = [boxes[i] for i in chunk]
                box_blob = box_rows[0] if n == 1 else union(box_rows, aggregate) + b"".join(box_rows)
                if number == 0:  # the source model keeps its arrays and its first geometries
                    at = r.off(model)
                    payload[r.off(geometries) + 8 * n : r.off(geometries) + 8 * count] = bytes(8 * (count - n))
                    payload[r.off(mapping) + 2 * n : r.off(mapping) + 2 * count] = bytes(2 * (count - n))
                    old_boxes = BOX * (count + 1)
                    payload[r.off(bounds) : r.off(bounds) + old_boxes] = box_blob + bytes(old_boxes - len(box_blob))
                    target = model
                else:
                    at = alloc.alloc(MODEL_BYTES)
                    geo_at = alloc.alloc(8 * n)
                    map_at = alloc.alloc(2 * n)
                    box_at = alloc.alloc(len(box_blob))
                    payload[at : at + MODEL_BYTES] = header
                    struct.pack_into(f"<{n}Q", payload, geo_at, *(pointers[i] for i in chunk))
                    struct.pack_into(f"<{n}H", payload, map_at, *(shaders[i] for i in chunk))
                    payload[box_at : box_at + len(box_blob)] = box_blob
                    for field, where in ((GEOMETRIES, geo_at), (MAPPING, map_at), (BOUNDS, box_at)):
                        struct.pack_into("<Q", payload, at + field, SYS + where)
                    target = SYS + at
                for field in (COUNT1, COUNT2, COUNT3):
                    struct.pack_into("<H", payload, at + field, n)
                new_items.append(target)
                sizes.append(n)
            lines.append(
                f"{label} model {index}: {count} geometries -> {len(chunks)} models "
                f"({' + '.join(map(str, sizes))}; engine keeps the geometry index in a signed byte)"
            )
        items, count = r.u(collection, "<Q"), r.u(collection, "<H", 8)
        if len(new_items) > 0xFFFF:
            raise SplitError(f"{label}: {len(new_items)} models")
        items_at = alloc.alloc(8 * len(new_items))
        struct.pack_into(f"<{len(new_items)}Q", payload, items_at, *new_items)
        payload[r.off(items) : r.off(items) + 8 * count] = bytes(8 * count)
        struct.pack_into("<QHH", payload, r.off(collection), SYS + items_at, len(new_items), len(new_items))
    return lines


def verify(source: Resource, out: Resource) -> list[str]:
    from copy_drawable_lods import census

    problems = [p for p in census(out) if "null LOD slot" not in p]
    before = {label: rows_of(source, models) for label, _, models in lod_lists(source)}
    for label, _, models in lod_lists(out):
        for m in models:
            n = out.u(m, "<H", COUNT1)
            if not 0 < n <= LIMIT or out.u(m, "<H", COUNT2) != n or out.u(m, "<H", COUNT3) != n:
                problems.append(f"{label}: model {m:#x} has {n} geometries")
        rows = rows_of(out, models)
        old = before.pop(label, None)
        if old is None or [r[:2] for r in rows] != [r[:2] for r in old] or [r[2] for r in rows] != [r[2] for r in old]:
            problems.append(f"{label}: geometry, shader or box rows differ from the source")
    if before:
        problems.append(f"LOD lists lost: {sorted(before)}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    blob = args.input.read_bytes()
    r = Resource(blob)
    if not oversized(r):
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(blob)
        print(f"wrote {args.output} unchanged: no model has more than {LIMIT} geometries")
        return 0
    sys_flags, gfx_flags = struct.unpack_from("<II", blob, 8)
    payload = bytearray(r.payload[: r.system])
    paged = len(resource_page_layout.pages_of(sys_flags)) > 1
    try:
        if paged:
            trailing_zeros_unreferenced(payload, r.system)
            alloc = resource_page_layout.TailAllocator.after_used(payload, sys_flags)
            lines = split(r, payload, alloc)
            system, sizes = alloc.finish()
            flags = (sys_flags & 0xF0000000) | resource_page_layout.flags_for_pages(sizes)
        else:
            lines = split(r, payload, Slack(payload, r.system))
            system, flags = bytes(payload), sys_flags
    except SplitError as error:
        raise SystemExit(f"model split: {error}") from None
    graphics = bytes(r.payload[r.system :])
    out = blob[:8] + struct.pack("<II", flags, gfx_flags) + encode(system + graphics)
    back = Resource(out)
    if back.payload != system + graphics or back.header["graphicsBytes"] != r.header["graphicsBytes"]:
        raise SystemExit("readback mismatch")
    problems = verify(r, back)
    if problems:
        raise SystemExit("verification failed:\n  " + "\n  ".join(problems[:20]))
    for line in lines:
        print(line)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f"wrote {args.output} bytes={len(out)} flags={flags:#x}/{gfx_flags:#x} ({r.system:#x} -> {back.system:#x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
