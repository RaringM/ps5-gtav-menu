"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import itertools
import struct

from gtavmenu_tools.asset_formats import AssetError

MAX_BUFFERS = 32768


MAX_BYTES = 128 * 1024 * 1024  # host work bound: a 64 MiB resource and its moved copy


def read_buffers(system, graphics, bases, geometries, contract):
    if (
        not isinstance(system, bytes)
        or not isinstance(graphics, bytes)
        or len(system) + len(graphics) > MAX_BYTES
        or len(bases) != 2
        or any(
            type(value) is not int or not 0 < value <= value + len(data) < 1 << 64
            for value, data in zip(bases, (system, graphics), strict=True)
        )
    ):
        raise AssetError("native buffer immutable storage/base bounds are unsupported")
    if bases[0] < bases[1] + len(graphics) and bases[1] < bases[0] + len(system):
        raise AssetError("native buffer address spaces overlap")
    if len(geometries) * 2 > MAX_BUFFERS:
        raise AssetError("native buffer observation count exceeds host budget")
    rows, spans_seen, total = [], [], 0
    prefix = max(v for group in contract["groups"] for v in group["geometryOffsets"]) + 8

    def claim(space, offset, size, label):
        data = (system, graphics)[space]
        if size <= 0 or offset < 0 or offset + size > len(data):
            raise AssetError(f"native buffer {label} exceeds declared storage")
        spans_seen.append((space, offset, offset + size, label))

    for gi, geometry in enumerate(geometries):
        offset = geometry["systemOffset"]
        claim(0, offset, prefix, "geometry selected prefix")
        for group in contract["groups"]:
            pointers = [struct.unpack_from("<Q", system, offset + field)[0] for field in group["geometryOffsets"]]
            if not pointers[0] or any(pointers[1:]):
                raise AssetError(
                    f"native buffer geometry {gi} group {group['group']} requires exactly selected first pointer; additional/null buffers are unsupported"
                )
            at = pointers[0] - bases[0]
            if at % 8:
                raise AssetError("native buffer object pointer is misaligned")
            claim(0, at, contract["observedBytes"], "buffer selected prefix")
            count = struct.unpack_from("<I", system, at + contract["countOffset"])[0]
            word = struct.unpack_from("<H", system, at + contract["strideOffset"])[0]
            stride = word & contract["strideMask"]
            length = count * stride
            if not count or not stride or length > 0xFFFFFFFF or length > MAX_BYTES or total + length > MAX_BYTES:
                raise AssetError(
                    "native buffer count/stride product is zero, wraps native arithmetic, or exceeds host byte budget"
                )
            pointer = struct.unpack_from("<Q", system, at + contract["dataOffset"])[0]
            spaces = [s for s, base in enumerate(bases) if base <= pointer < base + len((system, graphics)[s])]
            if len(spaces) != 1:
                raise AssetError("native buffer data pointer has no unique declared address space")
            space = spaces[0]
            start = pointer - bases[space]
            claim(space, start, length, "buffer data")
            total += length
            rows.append(
                {
                    "geometry": gi,
                    "group": group["group"],
                    "groupPointers": [hex(p) for p in pointers],
                    "systemOffset": at,
                    "count": count,
                    "strideWord": word,
                    "stride": stride,
                    "uninterpretedStrideBits": word & ~contract["strideMask"],
                    "byteLength": length,
                    "dataSpace": ("system", "graphics")[space],
                    "dataOffset": start,
                    "dataSha256": hashlib.sha256((system, graphics)[space][start : start + length]).hexdigest(),
                    "prefixSha256": hashlib.sha256(system[at : at + contract["observedBytes"]]).hexdigest(),
                }
            )
    spans_seen.sort()
    for a, b in itertools.pairwise(spans_seen):
        if a[0] == b[0] and b[1] < a[2]:
            raise AssetError(f"native buffer selected storage overlaps: {a[3]} / {b[3]}")
    return {
        "buffers": rows,
        "bufferCount": len(rows),
        "totalBytes": total,
        "selectedSpanCount": len(spans_seen),
        "hostLimits": {"buffers": MAX_BUFFERS, "bytes": MAX_BYTES},
        "scope": "Disjoint selected geometry/buffer prefixes and data spans only; other upstream metadata and unobserved object extents are not a complete overlap proof",
    }
