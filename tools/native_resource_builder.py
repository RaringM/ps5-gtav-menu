"""Deterministic object placement and explicit internal pointer fixups.

This builds a relocatable system-memory component, not an RSC envelope. The
caller supplies a format-derived base and every field; there are no retail
templates, implicit external pointers, or runtime actions.
"""

from __future__ import annotations

import bisect
import struct
from collections.abc import Callable

from gtavmenu_tools.asset_formats import AssetError


class ObjectArena:
    def __init__(self, limit: int = 32 * 1024 * 1024):
        self.data = bytearray()
        self.blocks: dict[str, dict] = {}
        self.fixups: dict[int, int | None] = {}
        self.limit = limit

    def add(self, name: str, data: bytes, alignment: int = 16) -> int:
        if name in self.blocks or not data or alignment <= 0 or alignment & (alignment - 1) or alignment > 4096:
            raise AssetError("component block name, length or alignment is invalid")
        at = (len(self.data) + alignment - 1) & -alignment
        if at + len(data) > self.limit:
            raise AssetError("component exceeds system storage budget")
        self.data.extend(bytes(at - len(self.data)))
        self.data.extend(data)
        self.blocks[name] = {"offset": at, "bytes": len(data), "alignment": alignment}
        return at

    def pointer(self, at: int, target: int | None):
        if at % 8 or not self.contains(at, 8) or at in self.fixups:
            raise AssetError("component pointer slot is unaligned, unowned or duplicated")
        if target is not None and not self.contains(target, 1):
            raise AssetError("component pointer target is outside an allocated block")
        if any(self.data[at : at + 8]):
            raise AssetError("component pointer slot was not explicitly cleared")
        self.fixups[at] = target

    def contains(self, at: int, size: int) -> bool:
        return any(b["offset"] <= at and at + size <= b["offset"] + b["bytes"] for b in self.blocks.values())

    def bind(self, at: int, target: int):
        """Resolve a previously declared null external site to an owned object."""
        if (
            at not in self.fixups
            or self.fixups[at] is not None
            or any(self.data[at : at + 8])
            or not self.contains(target, 1)
        ):
            raise AssetError("component external binding is missing, resolved or outside owned storage")
        self.fixups[at] = target

    def include(self, prefix: str, data: bytes, placement: dict) -> int:
        """Compose an existing component while retaining its block/fixup ownership."""
        relocate(data, placement, placement["sourceBase"])
        blocks = sorted(placement["blocks"].items(), key=lambda item: item[1]["offset"])
        if not prefix or not blocks:
            raise AssetError("included component has no prefix or blocks")
        names = [prefix + "." + name for name, _ in blocks]
        if len(set(names)) != len(names) or any(name in self.blocks for name in names):
            raise AssetError("included component block names collide")
        previous = 0
        for _, block in blocks:
            start, length, alignment = (block[k] for k in ("offset", "bytes", "alignment"))
            if (
                alignment <= 0
                or alignment > 4096
                or alignment & (alignment - 1)
                or start % alignment
                or start < previous
                or length <= 0
                or start + length > len(data)
            ):
                raise AssetError("included component block spans are invalid")
            previous = start + length
        alignment = max(b["alignment"] for _, b in blocks)
        at = (len(self.data) + alignment - 1) & -alignment
        if at + len(data) > self.limit:
            raise AssetError("included component exceeds storage budget")
        raw = bytearray(data)
        for row in placement["pointerSlots"]:
            raw[row["offset"] : row["offset"] + 8] = bytes(8)
        self.data.extend(bytes(at - len(self.data)))
        self.data.extend(raw)
        for (_name, block), full_name in zip(blocks, names, strict=True):
            self.blocks[full_name] = block | {"offset": at + block["offset"]}
        for row in placement["pointerSlots"]:
            self.pointer(at + row["offset"], None if row["targetOffset"] is None else at + row["targetOffset"])
        return at

    def paginate(self, page: int, skip: frozenset[str] = frozenset()) -> tuple[ObjectArena, Callable[[int], int]]:
        """Re-place every block into `page`-sized system pages; no block crosses a page boundary.

        Blocks keep their bytes, alignment and order of consideration (offset order, so the root at
        offset 0 stays there); every fixup slot and target moves with its containing block. Bytes
        outside blocks must be zero (alignment gaps). Returns the paged arena (with `.pages`, the
        largest-first page sizes) and the logical -> paged offset map. `skip` names trailing zero
        padding blocks that exist only in the logical image (never pointed to, never paged).
        """
        import resource_page_layout

        ordered = sorted(self.blocks.items(), key=lambda item: item[1]["offset"])
        for name in skip:
            block = self.blocks[name]
            if any(self.data[block["offset"] : block["offset"] + block["bytes"]]) or any(
                block["offset"] <= t < block["offset"] + block["bytes"] for t in self.fixups.values() if t is not None
            ):
                raise AssetError("skipped padding block is nonzero or referenced")
        ordered = [item for item in ordered if item[0] not in skip]
        previous = 0
        for _, block in ordered:
            if block["offset"] < previous or any(self.data[previous : block["offset"]]):
                raise AssetError("component has overlapping blocks or bytes outside every block")
            previous = block["offset"] + block["bytes"]
        if any(self.data[previous:]):
            raise AssetError("component has bytes after its last block")
        if any(previous < at for at in self.fixups):
            raise AssetError("component pointer slot lies in skipped padding")
        offsets, sizes = resource_page_layout.pack([(name, b["bytes"], b["alignment"]) for name, b in ordered], page)
        starts = [b["offset"] for _, b in ordered]

        def remap(at: int) -> int:
            index = bisect.bisect_right(starts, at) - 1
            name, block = ordered[index]
            if index < 0 or not at < block["offset"] + block["bytes"]:
                raise AssetError(f"offset {at:#x} is outside every block")
            return offsets[name] + at - block["offset"]

        paged = ObjectArena(limit=sum(sizes))
        paged.data = bytearray(sum(sizes))
        for name, block in ordered:
            at, length = offsets[name], block["bytes"]
            paged.data[at : at + length] = self.data[block["offset"] : block["offset"] + length]
            paged.blocks[name] = block | {"offset": at}
        for at, target in self.fixups.items():
            paged.fixups[remap(at)] = None if target is None else remap(target)
        resource_page_layout.check_blocks([(b["offset"], b["bytes"]) for b in paged.blocks.values()], sizes)
        paged.pages = sizes
        return paged, remap

    def render(self, base: int) -> tuple[bytes, dict]:
        if base <= 0 or base % 16 or base + len(self.data) >= 1 << 64:
            raise AssetError("component virtual base is invalid")
        data = bytearray(self.data)
        for at, target in self.fixups.items():
            struct.pack_into("<Q", data, at, 0 if target is None else base + target)
        return bytes(data), {
            "sourceBase": base,
            "bytes": len(data),
            "blocks": self.blocks,
            "pointerSlots": [{"offset": at, "targetOffset": target} for at, target in sorted(self.fixups.items())],
        }


def relocate(data: bytes, placement: dict, base: int) -> bytes:
    """Check every recorded pointer before rebasing a component for composition."""
    if base <= 0 or base % 16 or base + len(data) >= 1 << 64 or len(data) != placement["bytes"]:
        raise AssetError("component relocation base/extent is invalid")
    output, seen = bytearray(data), set()
    blocks = placement["blocks"].values()
    for row in placement["pointerSlots"]:
        at, target = row["offset"], row["targetOffset"]
        if at in seen or at % 8 or not any(b["offset"] <= at and at + 8 <= b["offset"] + b["bytes"] for b in blocks):
            raise AssetError("component relocation has an invalid pointer slot")
        seen.add(at)
        if target is not None and not any(b["offset"] <= target < b["offset"] + b["bytes"] for b in blocks):
            raise AssetError("component relocation target is outside allocated blocks")
        expected = 0 if target is None else placement["sourceBase"] + target
        if struct.unpack_from("<Q", data, at)[0] != expected:
            raise AssetError("component relocation source pointer changed")
        struct.pack_into("<Q", output, at, 0 if target is None else base + target)
    return bytes(output)
