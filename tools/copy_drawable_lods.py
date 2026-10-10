#!/usr/bin/env python3
"""Give fragment drawables independent deep copies of the high LOD list.

Every missing medium/low/very-low collection receives its own models, geometries,
buffers and data. Unknown pointers, aliasing and page-budget overflow are refused.
The default oversized-resource fallback gives each slot an independent empty list;
--no-fallback refuses instead. Existing complete LOD sets are verified and retained.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from collections import Counter
from itertools import pairwise
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

import resource_page_layout  # noqa: E402
from fill_drawable_lods import drawables, unchanged  # noqa: E402
from gtavmenu_tools.meta_resource import stored_deflate  # noqa: E402
from gtavmenu_tools.resource_view import SYS, Resource  # noqa: E402

LISTS = (0x50, 0x58, 0x60, 0x68)
MASKS = (0x80, 0x84, 0x88, 0x8C)
PAGE_SHIFT_16M = 0x7  # sys flags low nibble: one page of 0x2000 << (7 + 4) = 16 MiB
ALIGN = 16


def _sizes(r: Resource, model: int) -> tuple[int, int, int, int]:
    count = r.u(model, "<H", 0x10)
    bounds = count + (1 if count > 1 else 0)
    return count, count * 8, count * 2, bounds * 32


class Copier:
    def __init__(self, r: Resource, payload: bytearray, start: int, limit: int, allocator=None):
        self.r, self.payload, self.cursor, self.limit = r, payload, start, limit
        self.ranges: list[tuple[int, int]] = []
        self.allocator = allocator  # resource_page_layout.TailAllocator for multi-page input

    def alloc(self, size: int) -> int:
        if self.allocator:
            at = self.allocator.alloc(size, ALIGN)
            self.cursor = at + size
            self.ranges.append((at, at + size))
            return at
        at = (self.cursor + ALIGN - 1) & ~(ALIGN - 1)
        if at + size > self.limit:
            raise SystemExit(f"page budget exceeded at {at + size:#x} > {self.limit:#x}")
        self.cursor = at + size
        self.ranges.append((at, at + size))
        return at

    def copy(self, pointer: int, size: int) -> int:
        """Copy `size` bytes from a resource pointer; return the new resource pointer."""
        src = self.r.off(pointer)
        at = self.alloc(size)
        self.payload[at : at + size] = self.r.payload[src : src + size]
        return SYS + at

    def set_q(self, pointer: int, delta: int, value: int) -> None:
        struct.pack_into("<Q", self.payload, pointer - SYS + delta, value)

    def check_pointers(self, new_pointer: int, size: int, allowed: set[int]) -> None:
        base = new_pointer - SYS
        for offset in range(0, size - size % 8, 8):
            value = struct.unpack_from("<Q", self.payload, base + offset)[0]
            if SYS <= value < SYS + len(self.payload) and offset not in allowed:
                raise SystemExit(f"unexpected pointer {value:#x} at +{offset:#x} of a copied block {new_pointer:#x}")

    def vertex_buffer(self, vb: int) -> tuple[int, int]:
        r = self.r
        count, stride = r.u(vb, "<I", 0x08), r.u(vb, "<H", 0x0C)
        data, view, decl = r.u(vb, "<Q", 0x18), r.u(vb, "<Q", 0x30), r.u(vb, "<Q", 0x38)
        new = self.copy(vb, 0x40)
        self.check_pointers(new, 0x40, {0x18, 0x30, 0x38})
        new_data = self.copy(data, count * stride)
        self.set_q(new, 0x18, new_data)
        if view:
            new_view = self.copy(view, 0x20)
            self.check_pointers(new_view, 0x20, set())
            self.set_q(new, 0x30, new_view)
        if decl:
            new_decl = self.copy(decl, 0x140)
            self.check_pointers(new_decl, 0x140, set())
            self.set_q(new, 0x38, new_decl)
        return new, new_data

    def index_buffer(self, ib: int) -> int:
        r = self.r
        count, size = r.u(ib, "<I", 0x08), r.u(ib, "<H", 0x0C)
        data, view = r.u(ib, "<Q", 0x18), r.u(ib, "<Q", 0x30)
        new = self.copy(ib, 0x40)
        self.check_pointers(new, 0x40, {0x18, 0x30})
        self.set_q(new, 0x18, self.copy(data, count * size))
        if view:
            new_view = self.copy(view, 0x20)
            self.check_pointers(new_view, 0x20, set())
            self.set_q(new, 0x30, new_view)
        return new

    def geometry(self, geo: int) -> int:
        r = self.r
        bones, bone_count = r.u(geo, "<Q", 0x68), r.u(geo, "<H", 0x72)
        if bones and bones != geo + 0xA0:
            raise SystemExit(f"geometry {geo:#x}: bone ids not inline")
        size = 0xA0 + (bone_count * 2 if bones else 0)
        new = self.copy(geo, size)
        self.check_pointers(new, 0xA0, {0x18, 0x38, 0x68, 0x78})
        vb, ib, vdata = r.u(geo, "<Q", 0x18), r.u(geo, "<Q", 0x38), r.u(geo, "<Q", 0x78)
        new_vb, new_vdata = self.vertex_buffer(vb)
        if vdata != r.u(vb, "<Q", 0x18):
            raise SystemExit(f"geometry {geo:#x}: vertex data field differs from its buffer's data")
        self.set_q(new, 0x18, new_vb)
        self.set_q(new, 0x78, new_vdata)
        self.set_q(new, 0x38, self.index_buffer(ib))
        if bones:
            self.set_q(new, 0x68, new + 0xA0)
        return new

    def model(self, model: int) -> int:
        r = self.r
        count, array_bytes, map_bytes, bounds_bytes = _sizes(r, model)
        array, bounds, mapping = r.u(model, "<Q", 0x08), r.u(model, "<Q", 0x18), r.u(model, "<Q", 0x20)
        new = self.copy(model, 0x30)
        self.check_pointers(new, 0x30, {0x08, 0x18, 0x20})
        new_array = self.copy(array, array_bytes)
        for g in range(count):
            struct.pack_into("<Q", self.payload, new_array - SYS + 8 * g, self.geometry(r.u(array, "<Q", 8 * g)))
        self.set_q(new, 0x08, new_array)
        if bounds:
            self.set_q(new, 0x18, self.copy(bounds, bounds_bytes))
        if mapping:
            self.set_q(new, 0x20, self.copy(mapping, map_bytes))
        return new

    def collection(self, collection: int) -> int:
        r = self.r
        items, count = r.u(collection, "<Q"), r.u(collection, "<H", 8)
        new = self.copy(collection, 0x10)
        new_items = self.copy(items, 8 * count)
        for i in range(count):
            struct.pack_into("<Q", self.payload, new_items - SYS + 8 * i, self.model(r.u(items, "<Q", 8 * i)))
        self.set_q(new, 0x00, new_items)
        return new


def census(r: Resource) -> list[str]:
    """Count references reached from every LOD slot; every object must have exactly one."""
    problems: list[str] = []
    refs: Counter = Counter()
    data_refs: Counter = Counter()
    for name, d in drawables(r):
        lists = struct.unpack_from("<4Q", r.payload, r.off(d) + LISTS[0])
        if lists[0] and not all(lists):
            problems.append(f"{name}: null LOD slot {[hex(x) for x in lists]}")
        for collection in lists:
            if not collection:
                continue
            refs[("collection", collection)] += 1
            items, count = r.u(collection, "<Q"), r.u(collection, "<H", 8)
            refs[("items", items)] += 1 if count else 0
            for i in range(count):
                model = r.u(items, "<Q", 8 * i)
                refs[("model", model)] += 1
                gcount = r.u(model, "<H", 0x10)
                array = r.u(model, "<Q", 0x08)
                refs[("geometry-array", array)] += 1
                for key, at in (("bounds", 0x18), ("shader-map", 0x20)):
                    if r.u(model, "<Q", at):
                        refs[(key, r.u(model, "<Q", at))] += 1
                for g in range(gcount):
                    geo = r.u(array, "<Q", 8 * g)
                    refs[("geometry", geo)] += 1
                    vb, ib = r.u(geo, "<Q", 0x18), r.u(geo, "<Q", 0x38)
                    refs[("vertex-buffer", vb)] += 1
                    refs[("index-buffer", ib)] += 1
                    vdata = r.u(vb, "<Q", 0x18)
                    if r.u(geo, "<Q", 0x78) != vdata:
                        problems.append(f"geometry {geo:#x}: +0x78 differs from its buffer data")
                    data_refs[("vertex-data", vdata)] += 1
                    data_refs[("index-data", r.u(ib, "<Q", 0x18))] += 1
                    for key, obj in (
                        ("vertex-view", r.u(vb, "<Q", 0x30)),
                        ("declaration", r.u(vb, "<Q", 0x38)),
                        ("index-view", r.u(ib, "<Q", 0x30)),
                    ):
                        if obj:
                            refs[(key, obj)] += 1
    for (kind, pointer), count in list(refs.items()) + list(data_refs.items()):
        if count > 1:
            problems.append(f"{kind} {pointer:#x} referenced {count} times")
    return problems


def too_large(args, needed: int) -> int:
    """Deep copies beyond the paged bounds (128 pages, 64 MiB): own empty collections instead.

    A heavy single-LOD mod (LaFerrari: 24.7 MB high list) would need ~4x its size for three copies,
    past the supported resource-map capacity.
    The verified empty-collection route means passes asking for
    the medium/low list draw nothing instead of a copy. With --no-fallback (convert_vehicle.py
    --lower-lods copy) the run refuses instead.
    """
    import fill_drawable_lods

    if args.no_fallback:
        raise SystemExit(
            f"deep copies need {needed / 2**20:.1f} MiB, past the paged bounds ({resource_page_layout.MAX_PAGES} "
            f"pages, {resource_page_layout.MAX_PAGED_BYTES >> 20} MiB): keep the lower LODs empty "
            "(convert-vehicle --lower-lods empty or auto)"
        )
    print(
        f"deep copies need {needed / 2**20:.1f} MiB, past the paged bounds "
        f"({resource_page_layout.MAX_PAGES} pages, {resource_page_layout.MAX_PAGED_BYTES >> 20} MiB): "
        "own empty medium/low/very-low collections instead (fill_drawable_lods.py)"
    )
    saved = sys.argv
    sys.argv = [saved[0], "--input", str(args.input), "--output", str(args.output)]
    try:
        return fill_drawable_lods.main()
    finally:
        sys.argv = saved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True, help="converter output without LOD fill (e.g. a80.pft)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stored", action="store_true", help="stored deflate instead of compressed")
    parser.add_argument(
        "--no-fallback", action="store_true", help="refuse past the paged bounds instead of empty collections"
    )
    args = parser.parse_args()

    blob = args.input.read_bytes()
    r = Resource(blob)
    sys_flags, gfx_flags = struct.unpack_from("<II", blob, 8)
    paged = len(resource_page_layout.pages_of(sys_flags)) > 1
    if r.header["graphicsBytes"] or ((sys_flags & 0xF0) != 0x10 and not paged):
        raise SystemExit("expected one system page (or a multi-page system region) and no graphics pages")
    if paged:  # route B: copies fill the last page's tail and new pages; no object crosses a page
        payload = bytearray(r.payload)
        allocator = resource_page_layout.TailAllocator.after_used(payload, sys_flags, alignment=0x1000)
        copier = Copier(r, payload, allocator.cursor, 0, allocator)
    else:
        new_flags = (sys_flags & ~0xF) | PAGE_SHIFT_16M
        new_size = 0x2000 << (PAGE_SHIFT_16M + 4)
        if new_size < r.system:
            raise SystemExit("input page is already larger than 16 MiB")
        payload = bytearray(r.payload) + bytearray(new_size - r.system)
        used = max(i for i in range(r.system) if r.payload[i]) + 1
        copier = Copier(r, payload, (used + 0x40 + 0xFFF) & ~0xFFF, new_size)

    filled = []
    for name, d in drawables(r):
        lists = struct.unpack_from("<4Q", r.payload, r.off(d) + LISTS[0])
        if not lists[0]:
            continue
        high_mask = r.u(d, "<I", MASKS[0])
        for k in (1, 2, 3):
            if lists[k]:
                continue
            new = copier.collection(lists[0])
            struct.pack_into("<Q", payload, r.off(d) + LISTS[k], new)
            struct.pack_into("<I", payload, r.off(d) + MASKS[k], high_mask)
            filled.append((name, LISTS[k], new))
    if not filled:
        return unchanged(blob, r, args.output)
    rehomed: list[tuple[int, int]] = []
    if paged:
        info = r.u(SYS, "<Q", resource_page_layout.ROOT_INFO_POINTER) - SYS
        old_info = (info, info + 16 + 8 * len(resource_page_layout.pages_of(sys_flags)))
        try:
            system, sizes = allocator.finish()
        except resource_page_layout.PageBudgetError:
            return too_large(args, len(payload))
        payload = bytearray(system)
        new_flags = (sys_flags & 0xF0000000) | resource_page_layout.flags_for_pages(sizes)
        new_size = len(payload)
        # finish() may re-home ResourcePagesInfo (new page count): its old bytes, the root pointer
        # and the new block are the only other allowed changes.
        rehomed = [old_info, (8, 16)] + [span for span in allocator.ranges if span not in copier.ranges]

    header = blob[:8] + struct.pack("<II", new_flags, gfx_flags)
    if args.stored:
        out = header + stored_deflate(bytes(payload))
    else:  # raw deflate like retail members and the hardware-proven writer-built a80.ptd
        packer = zlib.compressobj(9, zlib.DEFLATED, -15)
        out = header + packer.compress(bytes(payload)) + packer.flush()
    check = zlib.decompressobj(-15)
    if check.decompress(out[16:]) != bytes(payload) or not check.eof:
        raise SystemExit("readback mismatch")
    back = Resource(out)
    if back.system != new_size:
        raise SystemExit(f"declared system page {back.system:#x} != {new_size:#x}")
    problems = census(back)
    spans = sorted(copier.ranges)
    for (_a0, a1), (b0, _b1) in pairwise(spans):
        if b0 < a1:
            problems.append(f"copied ranges overlap at {b0:#x}")
    changed = [i for i in range(min(r.system, back.system)) if r.payload[i] != back.payload[i]]
    if any(r.payload[back.system : r.system]):
        problems.append("page compaction removed non-zero source bytes")
    allowed = set()
    for _name, d in drawables(r):
        for slot in LISTS[1:] + MASKS[1:]:
            base = r.off(d) + slot
            allowed.update(range(base, base + 8))
    for a0, a1 in spans + rehomed:
        allowed.update(range(a0, min(a1, r.system)))
    if any(r.payload[i] for i in range(min(spans[0][0], r.system), r.system)):
        problems.append("the copy region overlapped non-zero bytes of the original page")
    values = struct.unpack_from(f"<{r.system // 8}Q", r.payload, 0)
    if any(SYS + spans[0][0] <= v < SYS + r.system for v in values):
        problems.append("an original pointer targets the slack used for copies")
    if any(i not in allowed for i in changed):
        problems.append("bytes outside the LOD slots/masks/copies changed below the old page end")
    if problems:
        raise SystemExit("verification failed:\n  " + "\n  ".join(problems[:20]))
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite {args.output}")
    args.output.write_bytes(out)
    for name, slot, new in filled:
        print(f"{name}: slot {slot:#x} -> deep copy {new:#x}")
    print(
        f"wrote {args.output} bytes={len(out)} flags={new_flags:#x}/{gfx_flags:#x} system={new_size:#x} "
        f"used={copier.cursor:#x} copies={len(filled)} objects={len(copier.ranges)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
