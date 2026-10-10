"""Bounded byte reader for the observed PS5 v5 ordinary texture dictionary.

This target-specific subset has a 64-byte dictionary root, 88-byte texture
objects and separate 32-byte view records. It reads the RSC7 payload itself;
writer placement reports, constructor models and executable code are not used.
The shared container size arithmetic does not imply a shared PC object layout.

Structural acceptance establishes neither metadata equivalence nor GPU/runtime
compatibility. In particular, tile mode 255 is an unresolved automatic request,
and a serialized view may be an initializer rather than a completed GPU view.
"""

from __future__ import annotations

import hashlib
import struct
from itertools import pairwise

from .asset_formats import AssetError, Limits, decode_resource, safe_name
from .asset_textures import GRAPHICS_BASE, SYSTEM_BASE, linear_mip_sizes
from .hashes import joaat

# Finite ordinary formats observed by the separate target layout experiments.
# Code -> name, pixels per block edge, bytes per block/pixel.
PS5_TEXTURE_FORMATS = {
    71: ("BC1", 4, 8),
    77: ("BC3", 4, 16),
    83: ("BC5", 4, 16),
    87: ("BGRA8", 1, 4),
}
_PAGE_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))
_MAX_PAGES = 128
_OBJECT_BYTES = 88
_VIEW_BYTES = 32


def _pages(flags: int, size: int, base: int) -> list[dict]:
    rows, offset = [], 0
    for rank, (shift, width) in enumerate(_PAGE_FIELDS):
        length = (512 << (flags & 15)) << (8 - rank)
        for _ in range((flags >> shift) & ((1 << width) - 1)):
            if length < 8192 or len(rows) >= _MAX_PAGES:
                raise AssetError("PS5 texture pages exceed the supported size/count subset")
            rows.append({"offset": offset, "pointer": base + offset, "bytes": length})
            offset += length
    if not rows or offset != size:
        raise AssetError("PS5 texture page sequence disagrees with its envelope")
    return rows


def _disjoint(spans: list[tuple[int, int, str]], label: str) -> None:
    for left, right in pairwise(sorted(spans)):
        if left[1] > right[0]:
            raise AssetError(f"overlapping PS5 texture {label}: {left[2]} / {right[2]}")


def _zero_gaps(data: memoryview, spans: list[tuple[int, int, str]]) -> bool:
    """Observe unowned padding without imposing a policy on stock resources."""
    cursor = 0
    for start, end, _label in sorted(spans):
        if any(data[cursor:start]):
            return False
        cursor = end
    return not any(data[cursor:])


class _View:
    def __init__(self, payload: bytes, header: dict):
        self.system = memoryview(payload)[: header["systemBytes"]]
        self.graphics = memoryview(payload)[header["systemBytes"] :]
        self.pages = {
            "system": _pages(int(header["systemFlags"], 0), len(self.system), SYSTEM_BASE),
            "graphics": _pages(int(header["graphicsFlags"], 0), len(self.graphics), GRAPHICS_BASE),
        }
        if sum(map(len, self.pages.values())) > _MAX_PAGES:
            raise AssetError("PS5 texture page count exceeds the reader limit")
        self.structures: list[tuple[int, int, str]] = []
        self.allocations: list[tuple[int, int, str]] = []

    def span(self, pointer: int, size: int, label: str, alignment: int = 1, *, graphics: bool = False) -> int:
        base, data, pages, spans = (
            (GRAPHICS_BASE, self.graphics, self.pages["graphics"], self.allocations)
            if graphics
            else (SYSTEM_BASE, self.system, self.pages["system"], self.structures)
        )
        offset = pointer - base
        if pointer % alignment or size <= 0 or offset < 0 or offset > len(data) - size:
            raise AssetError(f"PS5 texture {label} pointer/range leaves its aligned address space")
        if not any(page["offset"] <= offset and offset + size <= page["offset"] + page["bytes"] for page in pages):
            raise AssetError(f"PS5 texture {label} crosses a resource page boundary")
        spans.append((offset, offset + size, label))
        return offset

    def unpack(self, fmt: str, offset: int) -> tuple:
        if not 0 <= offset <= len(self.system) - struct.calcsize(fmt):
            raise AssetError("PS5 texture field leaves system pages")
        return struct.unpack_from(fmt, self.system, offset)

    def name(self, pointer: int) -> tuple[str, int]:
        offset = pointer - SYSTEM_BASE
        if not 0 <= offset < len(self.system):
            raise AssetError("PS5 texture name pointer leaves system pages")
        raw = bytes(self.system[offset : offset + 128])
        stop = raw.find(b"\0")
        if stop < 0:
            raise AssetError("PS5 texture name is unterminated or exceeds 127 bytes")
        try:
            name = raw[:stop].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("PS5 texture name is not ASCII") from exc
        safe_name(name)
        if "/" in name or "script_rt_" in name.lower():
            raise AssetError("PS5 texture name is outside the ordinary static subset")
        self.span(pointer, stop + 1, f"name {name}")
        return name, offset


def _page_info(view: _View) -> dict:
    pointer = view.unpack("<Q", 8)[0]
    counts = [len(view.pages[key]) for key in ("system", "graphics")]
    size = 16 + sum(counts) * 8
    offset = view.span(pointer, size, "page information", 8)
    raw = view.system[offset : offset + size]
    if list(raw[8:10]) != counts or raw[10] or raw[11]:
        raise AssetError("PS5 texture page-information counts disagree with the envelope")
    if any(raw[16:]):
        raise AssetError("nonzero serialized PS5 texture page records are unsupported")
    return {
        "systemOffset": offset,
        "bytes": size,
        "systemPageCount": counts[0],
        "graphicsPageCount": counts[1],
        "headerHex": bytes(raw[:16]).hex(),
        "serializedRecordsZero": True,
    }


def _texture(view: _View, pointer: int, key: int) -> dict:
    offset = view.span(pointer, _OBJECT_BYTES, "texture object", 16)
    name_pointer, view_pointer, data_pointer = view.unpack("<3Q", offset + 40)
    name, name_offset = view.name(name_pointer)
    if joaat(name) != key:
        raise AssetError("PS5 texture name disagrees with its dictionary hash")
    flags = view.unpack("<I", offset + 16)[0]
    width, height, depth = view.unpack("<3H", offset + 24)
    dimension, format_code, tile_mode, anti_alias, levels = view.unpack("<5B", offset + 30)
    if depth != 1 or dimension != 1 or anti_alias or flags & 0x1E0:
        raise AssetError(f"PS5 texture requires unsupported dimensional or special handling: {name}")
    if format_code not in PS5_TEXTURE_FORMATS or tile_mode not in (1, 5, 9, 255):
        raise AssetError(f"PS5 texture format/tile mode is unsupported: {name}")
    if width > 16384 or height > 16384:
        raise AssetError(f"PS5 texture dimensions exceed the reader limit: {name}")
    format_name, edge, block_bytes = PS5_TEXTURE_FORMATS[format_code]
    sizes = linear_mip_sizes(width, height, levels, edge, block_bytes)
    count, packed_stride = view.unpack("<2I", offset + 8)
    stride = packed_stride & 0xFFF
    storage_bytes = count * stride
    if stride != block_bytes or not 0 < storage_bytes < 1 << 32 or storage_bytes < sum(sizes):
        raise AssetError(f"PS5 texture storage count/stride does not cover its format and mips: {name}")
    view_offset = view.span(view_pointer, _VIEW_BYTES, f"view {name}", 8)
    graphics_offset = view.span(data_pointer, storage_bytes, f"pixels {name}", block_bytes, graphics=True)
    return {
        "name": name,
        "nameHash": f"0x{key:08x}",
        "key": key,
        "systemOffset": offset,
        "nameOffset": name_offset,
        "viewOffset": view_offset,
        "graphicsOffset": graphics_offset,
        "width": width,
        "height": height,
        "depth": depth,
        "dimension": dimension,
        "formatCode": format_code,
        "format": format_name,
        "blockBytes": block_bytes,
        "tileMode": tile_mode,
        "antiAlias": anti_alias,
        "mipLevels": levels,
        "linearMipBytes": sizes,
        "storageBytes": storage_bytes,
        "alignmentBytes": None,
        "metadata": {
            "flags": flags,
            "usageBits": flags & 0x1FF,
            "kindBits": (flags >> 9) & 7,
            "memoryBits": (flags >> 13) & 3,
            "storageBlockCount": count,
            "packedStrideWord": packed_stride,
            "referenceCount": view.unpack("<H", offset + 38)[0],
            "metadataCandidate": view.unpack("<I", offset + 64)[0],
            "viewType": view.system[offset + 68],
            "rawObjectHex": bytes(view.system[offset : offset + _OBJECT_BYTES]).hex(),
            "rawViewHex": bytes(view.system[view_offset : view_offset + _VIEW_BYTES]).hex(),
        },
        "linearMipsRecovered": False,
        "automaticTileModeResolved": False,
        "metadataEquivalenceQualified": False,
    }


def parse_ps5_texture_dictionary(blob: bytes, limits: Limits | None = None) -> dict:
    """Read every entry of a supported standalone dictionary or reject it.

    Returned ``allocation`` bytes are the complete serialized graphics reservations,
    not linear mip data. Deriving independent mip addresses is a separate operation.
    Unsupported entries are never silently omitted from an otherwise valid result.
    """
    if limits is None:
        limits = Limits()
    if len(blob) > limits.max_file_bytes:
        raise AssetError("PS5 texture resource exceeds the file byte limit")
    header, payload = decode_resource(blob, min(limits.max_file_bytes, limits.max_total_bytes))
    if header["version"] != 5:
        raise AssetError("PS5 texture reader requires version 5; no layout fallback")
    if header["systemBytes"] > limits.max_metadata_bytes:
        raise AssetError("PS5 texture system pages exceed the metadata byte limit")
    if len(payload) + header["graphicsBytes"] > limits.max_total_bytes:
        raise AssetError("PS5 texture parsing exceeds the cumulative byte limit")
    view = _View(payload, header)
    view.span(SYSTEM_BASE, 64, "dictionary root", 16)
    if view.unpack("<Q", 16)[0]:
        raise AssetError("PS5 texture parent dictionaries are outside the standalone subset")
    info = _page_info(view)
    key_pointer, count, key_capacity = view.unpack("<QHH", 32)
    value_pointer, value_count, value_capacity = view.unpack("<QHH", 48)
    if (
        not 0 < count == value_count <= min(key_capacity, value_capacity)
        or max(key_capacity, value_capacity) > limits.max_entries
    ):
        raise AssetError("PS5 texture dictionary counts/capacities are inconsistent or over budget")
    keys_at = view.span(key_pointer, key_capacity * 4, "key table", 4)
    values_at = view.span(value_pointer, value_capacity * 8, "value table", 8)
    keys = [view.unpack("<I", keys_at + index * 4)[0] for index in range(count)]
    if any(left >= right for left, right in pairwise(keys)):
        raise AssetError("PS5 texture dictionary keys must be sorted and unique")
    rows = [_texture(view, view.unpack("<Q", values_at + index * 8)[0], key) for index, key in enumerate(keys)]
    _disjoint(view.structures, "system structures")
    _disjoint(view.allocations, "graphics allocations")
    # Check all ownership spans before copying pixels: repeated hostile pointers
    # must not multiply a bounded graphics page into an unbounded allocation.
    for row in rows:
        start, length = row["graphicsOffset"], row["storageBytes"]
        allocation = bytes(view.graphics[start : start + length])
        row.update(allocation=allocation, storageSha256=hashlib.sha256(allocation).hexdigest())
    return {
        "layout": "ps5-ptd-v5-ordinary-static",
        "header": header,
        "resourceSha256": hashlib.sha256(blob).hexdigest(),
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "textureCount": len(rows),
        "textures": rows,
        "pages": view.pages,
        "pageInfo": info,
        "root": {
            "keyTableOffset": keys_at,
            "valueTableOffset": values_at,
            "keyCapacity": key_capacity,
            "valueCapacity": value_capacity,
            "referenceCount": view.unpack("<I", 24)[0],
            "classWord": view.unpack("<Q", 0)[0],
            "rawHex": bytes(view.system[:64]).hex(),
        },
        "unownedBytesZero": {
            "system": _zero_gaps(view.system, view.structures),
            "graphics": _zero_gaps(view.graphics, view.allocations),
        },
        "structuralValidationPassed": True,
        "linearMipsRecovered": False,
        "metadataEquivalenceQualified": False,
        "nativeGpuLayoutValidated": False,
        "runtimeQualified": False,
    }
