"""Read-only view of a PS5 bounds resource (.pbn, RSC7 v43) and of a composite bound's children.

A bound starts with a 16-byte object header; the type byte is at +0x10 (0 sphere, 1 capsule, 3 box,
4 geometry, 8 BVH, 10 composite, 12 disc, 13 cylinder), the radius at +0x14, max + margin at +0x20,
min at +0x30 and the centroid at +0x40. A composite (type 10) adds: children pointers +0x70, child
matrices +0x78 (aliased at +0x80), per-child boxes +0x88, per-child {u32 type, u32 include} flags
+0x90 (aliased at +0x98), two u16 child counts +0xa0/+0xa2 and an optional BVH pointer +0xa8.
Pointers are virtual: system pages at 0x50000000, graphics pages at 0x60000000.

Nothing here carries game bytes; the resource is always the caller's input.
"""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import decode_resource

__all__ = ["BOUND_TYPES", "GFX", "SYS", "View", "composite_info"]

SYS, GFX = 0x50000000, 0x60000000
RESOURCE_LIMIT = 1 << 31
BOUND_TYPES = {
    0: "sphere",
    1: "capsule",
    3: "box",
    4: "geometry",
    8: "bvh",
    10: "composite",
    12: "disc",
    13: "cylinder",
}


class View:
    """An inflated RSC7 resource with virtual-pointer translation."""

    def __init__(self, blob: bytes):
        self.header, self.payload = decode_resource(blob, RESOURCE_LIMIT)
        self.system = self.header["systemBytes"]

    def off(self, pointer: int) -> int:
        """Payload offset of a virtual pointer; ValueError when it points outside the resource."""
        if SYS <= pointer < SYS + self.system:
            return pointer - SYS
        if GFX <= pointer < GFX + self.header["graphicsBytes"]:
            return self.system + pointer - GFX
        raise ValueError(f"pointer {pointer:#x} outside the resource")

    def u(self, fmt: str, offset: int):
        return struct.unpack_from(fmt, self.payload, offset)[0]


def composite_info(view: View, offset: int) -> dict:
    """Fields of the composite bound at payload `offset` that the static bounds store reads.

    The store takes the child count from +0xa2, the children from +0x70 and each child's type/include
    pair from +0x90 (a NULL array reads as 0/0); its box is the union of each child's own +0x20/+0x30
    box. A non-interior static child gets an identity instance matrix, so the +0x78 matrices place
    nothing there.
    """
    p = view.payload

    def ptr(field: int) -> int:
        return view.u("<Q", offset + field)

    count1, count2 = view.u("<H", offset + 0xA0), view.u("<H", offset + 0xA2)
    children, flags, boxes, matrices = ptr(0x70), ptr(0x90), ptr(0x88), ptr(0x78)
    info = {
        "type": p[offset + 0x10],
        "count1": count1,
        "count2": count2,
        "matrices_alias": matrices == ptr(0x80),
        "flags_alias": flags == ptr(0x98),
        "bvh": ptr(0xA8),
        "children": [],
    }
    for i in range(count2):
        child = view.u("<Q", view.off(children) + 8 * i) if children else 0
        row: dict = {"pointer": child}
        if child:
            at = view.off(child)
            row.update(
                offset=at,
                type=p[at + 0x10],
                max=struct.unpack_from("<3f", p, at + 0x20),
                margin=view.u("<f", at + 0x2C),
                min=struct.unpack_from("<3f", p, at + 0x30),
                centroid=struct.unpack_from("<3f", p, at + 0x40),
                material=p[at + 0x4C],
            )
        if flags:
            row["flags"] = struct.unpack_from("<2I", p, view.off(flags) + 8 * i)
        if matrices:
            row["matrix"] = struct.unpack_from("<12f", p, view.off(matrices) + 0x40 * i)
        if boxes:
            row["bb_min"] = struct.unpack_from("<3f", p, view.off(boxes) + 0x20 * i)
            row["bb_max"] = struct.unpack_from("<3f", p, view.off(boxes) + 0x20 * i + 0x10)
        info["children"].append(row)
    return info
