#!/usr/bin/env python3
"""Multi-page RSC7 system layouts (resources larger than one page).

Retail fragments split the system region into many pages (tornado6_hi: 19 pages, none above
512 KiB); at load every page is its own allocation, so an object must never cross a page boundary.
Here every page has one size P (256 KiB, or the next power of two above the largest object) except
the least-used page, which shrinks to the smallest power of two that holds it (>= P/16) and goes
last. Pages are concatenated largest first; resource pointers stay `0x50000000 | offset` into that
concatenated space. Flags use base = P/16 so the P pages sit in the 7-bit 16 x base class (<= 127).

`pack` places named blocks first-fit in their given order (the first block lands at offset 0);
`TailAllocator` appends objects to an existing multi-page payload (LOD tools) and rewrites the
ResourcePagesInfo block when the page count changes.

  python3 tools/resource_page_layout.py --synthetic   # > 16 MiB self-check
"""

from __future__ import annotations

import bisect
import struct
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:  # `python3 -I` drops the script directory
    sys.path.insert(0, str(_HERE))

from gtavmenu_tools.asset_formats import AssetError  # noqa: E402

PAGE_BYTES = 256 * 1024  # retail's common system page size
MAX_PAGED_BYTES = 64 * 1024 * 1024  # host bound for a paged system region, not a game capacity
MAX_PAGES = 128  # extracted page-record observation limit (native_vehicle_resource.numeric_check)
SYS = 0x50000000
INFO_HEADER, INFO_RECORD, INFO_SYSTEM_COUNT, INFO_GRAPHICS_COUNT = 16, 8, 8, 9
ROOT_INFO_POINTER = 8  # ResourceFileBase.FilePagesInfoPointer
# PC RSC7 page-size classes (flag bit shift, field width), in descending size order (resource_pages.py: the
# comparison model of the native page-table observations, not native structure offsets).
PC_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))


def pc_pages(flags: int) -> list[int]:
    if type(flags) is not int or not 0 <= flags <= 0xFFFFFFFF:
        raise AssetError("page flags must be uint32")
    return [
        (512 << (flags & 15)) << (8 - rank)
        for rank, (shift, width) in enumerate(PC_FIELDS)
        for _ in range((flags >> shift) & ((1 << width) - 1))
    ]


class PageBudgetError(AssetError):
    """The blocks need more pages of this size than the flags or host bounds allow."""


def _pow2(value: int) -> int:
    return 1 << max(0, (value - 1).bit_length())


def page_size_for(largest: int, minimum: int = PAGE_BYTES) -> int:
    return max(minimum, _pow2(largest))


def flags_for_pages(sizes: list[int]) -> int:
    """Low 28 flag bits for a largest-first page list; prefers the largest base that fits."""
    if not sizes or sizes != sorted(sizes, reverse=True) or any(s & (s - 1) for s in sizes):
        raise AssetError("page sizes must be a nonempty largest-first list of powers of two")
    for nibble in range(15, -1, -1):
        base, flags = 512 << nibble, nibble
        for rank, (shift, width) in enumerate(PC_FIELDS):
            count = sizes.count(base << (8 - rank))
            if count >= 1 << width:
                break
            flags |= count << shift
        else:
            if pc_pages(flags) == sizes:
                return flags
    raise AssetError(f"no page flags express {len(sizes)} pages {sorted(set(sizes))}")


def pages_of(flags: int) -> list[tuple[int, int]]:
    out, at = [], 0
    for size in pc_pages(flags):
        out.append((at, size))
        at += size
    return out


def pack(blocks: list[tuple[str, int, int]], page: int) -> tuple[dict[str, int], list[int]]:
    """First-fit (name, bytes, alignment) blocks into pages of `page` bytes; no block crosses a page.

    Returns {name: offset} in the final concatenated space and the largest-first page sizes.
    """
    if page & (page - 1) or page < 16 * 512:
        raise AssetError("page size must be a power of two >= 8 KiB")
    cursors: list[int] = []  # used bytes per page, in opening order
    placed: list[tuple[str, int, int]] = []  # name, page index, offset in page
    for name, size, alignment in blocks:
        if size <= 0 or size > page or alignment > page:
            raise AssetError(f"block {name} ({size} bytes) does not fit a {page}-byte page")
        for index, used in enumerate(cursors):  # noqa: B007 (index is used after the loop)
            at = (used + alignment - 1) & -alignment
            if at + size <= page:
                break
        else:
            index, at = len(cursors), 0
            cursors.append(0)
        cursors[index] = at + size
        placed.append((name, index, at))
    if not placed or placed[0][1:] != (0, 0):
        raise AssetError("the first (root) block must land at offset 0")
    sizes = [page] * len(cursors)
    # Shrink the least-used page (never the root page unless it is the only one) and put it last.
    candidates = range(len(cursors)) if len(cursors) == 1 else range(1, len(cursors))
    small = min(candidates, key=lambda i: (cursors[i], -i))
    sizes[small] = max(page // 16, _pow2(cursors[small]))
    order = [i for i in range(len(cursors)) if i != small] + [small]
    starts, at = {}, 0
    for i in order:
        starts[i] = at
        at += sizes[i]
    offsets = {name: starts[index] + offset for name, index, offset in placed}
    final = [sizes[i] for i in order]
    if len(final) > MAX_PAGES or sum(final) > MAX_PAGED_BYTES:
        raise PageBudgetError(f"{len(final)} pages / {sum(final)} bytes exceed the paged host bounds")
    try:
        flags_for_pages(final)
    except AssetError as exc:
        raise PageBudgetError(str(exc)) from exc
    return offsets, final


def check_blocks(spans: list[tuple[int, int]], sizes: list[int]) -> None:
    """Every (offset, bytes) span lies inside one page and no two spans overlap."""
    bounds, at = [], 0
    for size in sizes:
        bounds.append((at, at + size))
        at += size
    previous = 0
    for start, length in sorted(spans):
        if start < previous:
            raise AssetError(f"paged blocks overlap at {start:#x}")
        previous = start + length
        if not any(a <= start and start + length <= b for a, b in bounds):
            raise AssetError(f"block {start:#x}+{length:#x} crosses a page boundary")


class TailAllocator:
    """Append objects to a multi-page payload: the last page's tail, then new P-sized pages."""

    def __init__(self, payload: bytearray, flags: int, cursor: int):
        self.pages = [list(p) for p in pages_of(flags)]
        self.page = self.pages[0][1]
        if any(size > self.page for _, size in self.pages) or len(payload) != sum(s for _, s in self.pages):
            raise AssetError("payload does not match a largest-first page list")
        self.payload, self.original_count = payload, len(self.pages)
        last = self.pages[-1]
        if not last[0] <= cursor:
            raise AssetError("allocation cursor is not inside the last page")
        if last[1] < self.page:  # regrow the shrunk tail: it starts on a multiple of P
            payload.extend(bytes(self.page - last[1]))
            last[1] = self.page
        self.cursor = cursor
        self.ranges: list[tuple[int, int]] = []

    @classmethod
    def after_used(cls, payload: bytearray, flags: int, guard: int = 0x40, alignment: int = 16) -> TailAllocator:
        """Start after the last nonzero byte (plus a guard) and after the page-info reservation."""
        used = len(payload.rstrip(b"\0"))
        info = struct.unpack_from("<Q", payload, ROOT_INFO_POINTER)[0] - SYS
        info_end = info + INFO_HEADER + INFO_RECORD * len(pc_pages(flags))
        cursor = (max(used, info_end) + guard + alignment - 1) & -alignment
        return cls(payload, flags, max(cursor, pages_of(flags)[-1][0]))

    def _open(self) -> None:
        start = len(self.payload)
        self.payload.extend(bytes(self.page))
        self.pages.append([start, self.page])
        self.cursor = start

    def alloc(self, size: int, alignment: int = 16) -> int:
        if size <= 0 or size > self.page:
            raise AssetError(f"object of {size} bytes does not fit a {self.page}-byte page")
        start, length = self.pages[-1]
        at = (self.cursor + alignment - 1) & -alignment
        if at + size > start + length:
            self._open()
            at = self.cursor
        self.cursor = at + size
        self.ranges.append((at, at + size))
        return at

    def finish(self) -> tuple[bytes, list[int]]:
        """Re-home ResourcePagesInfo if the page count changed, shrink the tail, return payload + sizes."""
        p = self.payload
        info = struct.unpack_from("<Q", p, ROOT_INFO_POINTER)[0] - SYS
        old = INFO_HEADER + INFO_RECORD * self.original_count
        if (
            p[info + INFO_SYSTEM_COUNT] != self.original_count
            or p[info + INFO_GRAPHICS_COUNT]
            or any(p[info + 9 : info + old])
        ):
            raise AssetError("root page-info block differs from the declared page count")
        if len(self.pages) > MAX_PAGES or len(p) > MAX_PAGED_BYTES:  # before the page-info count byte
            raise PageBudgetError("appended pages exceed the paged host bounds")
        if len(self.pages) != self.original_count:
            start, length = self.pages[-1]
            if ((self.cursor + 15) & -16) + INFO_HEADER + INFO_RECORD * len(self.pages) > start + length:
                self._open()
            count = len(self.pages)
            new = self.alloc(INFO_HEADER + INFO_RECORD * count)
            p[info : info + old] = bytes(old)
            p[new + INFO_SYSTEM_COUNT] = count
            struct.pack_into("<Q", p, ROOT_INFO_POINTER, SYS + new)
        start, length = self.pages[-1]
        size = max(self.page // 16, _pow2(self.cursor - start))
        if size < length:
            del p[start + size :]
            self.pages[-1][1] = size
        sizes = [s for _, s in self.pages]
        if len(sizes) > MAX_PAGES or len(p) > MAX_PAGED_BYTES:
            raise PageBudgetError("appended pages exceed the paged host bounds")
        check_blocks([(a, b - a) for a, b in self.ranges], sizes)
        flags_for_pages(sizes)
        return bytes(p), sizes


def logical_view(payload: bytes, graph: dict) -> bytes:
    """The flat logical image of a paged vehicle resource (identity for single-page resources).

    `graph` is the resource placement (convert_pc_vehicle.py <stem>.resource.placement.json): typed
    offsets in it are logical; `vehicleResource.pageMap` rows [logical, paged, bytes] move every block.
    Every recorded pointer is checked against its remapped target before it is rewritten.
    """
    resource = graph["vehicleResource"]
    if "pages" not in resource:
        return payload
    rows, base = resource["pageMap"], graph["sourceBase"]
    starts = [row[0] for row in rows]

    def remap(at: int) -> int:
        index = bisect.bisect_right(starts, at) - 1
        logical, paged, length = rows[index]
        if index < 0 or not at < logical + length:
            raise AssetError("logical offset is outside every paged block")
        return paged + at - logical

    if len(payload) < resource["systemBytes"]:
        raise AssetError("paged payload is shorter than its declared pages")
    out = bytearray(resource["logicalBytes"])
    for at, paged, length in rows:
        out[at : at + length] = payload[paged : paged + length]
    for row in graph["pointerSlots"]:
        at, target = row["offset"], row["targetOffset"]
        if struct.unpack_from("<Q", payload, remap(at))[0] != (0 if target is None else base + remap(target)):
            raise AssetError("paged pointer differs from its remapped logical target")
        struct.pack_into("<Q", out, at, 0 if target is None else base + target)
    return bytes(out)


def _synthetic() -> int:
    """A > 16 MiB arena with cross-page pointers: paginate, encode, decode, compare."""
    import random

    import resource_envelope
    from gtavmenu_tools.asset_formats import decode_resource
    from native_resource_builder import ObjectArena

    rng = random.Random(7)
    arena = ObjectArena(limit=MAX_PAGED_BYTES)
    names = []
    for i in range(400):
        size = rng.choice((0x30, 0xA0, 0x140, 0x4000, 0x21000, 0x3FF00))
        data = bytearray(rng.randbytes(size))
        for at in range(0, min(size, 0x40), 8):
            data[at : at + 8] = bytes(8)  # pointer slots
        names.append(arena.add(f"b{i}", bytes(data), alignment=rng.choice((16, 16, 256))))
    for at in names:
        for k in range(4):
            target = rng.choice(names)
            arena.pointer(at + 8 * k, target + (8 * k if arena.contains(target + 8 * k, 1) else 0))
    page = page_size_for(max(b["bytes"] for b in arena.blocks.values()))
    paged, mapping = arena.paginate(page)
    logical, placement = arena.render(SYS)
    system, _ = paged.render(SYS)
    flags = flags_for_pages(paged.pages)
    envelope = resource_envelope.encode(0x2B, flags | 0x20000000, 0xB0000000, system, b"", max_bytes=MAX_PAGED_BYTES)
    header, payload = decode_resource(envelope, MAX_PAGED_BYTES)
    if payload != system or header["systemBytes"] != sum(paged.pages):
        raise AssetError("synthetic envelope readback differs")
    check_blocks([(b["offset"], b["bytes"]) for b in paged.blocks.values()], paged.pages)
    for row in placement["pointerSlots"]:
        at, target = mapping(row["offset"]), mapping(row["targetOffset"])
        if struct.unpack_from("<Q", payload, at)[0] != SYS + target:
            raise AssetError("synthetic pointer differs after pagination")
    flat, mp = bytearray(logical), bytearray(payload)
    for at in arena.fixups:
        flat[at : at + 8] = bytes(8)
    for at in paged.fixups:
        mp[at : at + 8] = bytes(8)
    for name, block in arena.blocks.items():
        a, b, n = block["offset"], paged.blocks[name]["offset"], block["bytes"]
        if flat[a : a + n] != mp[b : b + n]:
            raise AssetError(f"synthetic block {name} bytes differ")
    print(
        f"synthetic OK logical={len(logical):#x} paged={len(system):#x} pages={len(paged.pages)} "
        f"x {page // 1024} KiB (tail {paged.pages[-1] // 1024} KiB) flags={flags | 0x20000000:#010x} "
        f"blocks={len(arena.blocks)} pointers={len(placement['pointerSlots'])}"
    )
    return 0


if __name__ == "__main__":
    if sys.argv[1:] != ["--synthetic"]:
        raise SystemExit(__doc__)
    raise SystemExit(_synthetic())
