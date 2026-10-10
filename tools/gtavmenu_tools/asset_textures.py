"""Read-only PC Legacy YTD metadata and linear-mip preflight, not a PS5 writer.

Public format references (field layout facts, not copied implementation):
https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/Resources/Texture.cs
https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/Resources/ResourceBaseTypes.cs

Unknown layouts and formats are never assigned a fallback. Resource pointer
tags are file-relative address spaces, not runtime game addresses.
"""

from __future__ import annotations

import hashlib
import struct
from itertools import pairwise

from .asset_formats import AssetError, Limits, safe_name
from .hashes import joaat

SYSTEM_BASE = 0x50000000
GRAPHICS_BASE = 0x60000000
SCRIPT_TEXTURE_MARKER = "script_rt_"

# Name, pixels per block edge, bytes per block (or per pixel for edge=1).
LEGACY_FORMATS = {
    21: ("BGRA8", 1, 4),
    22: ("BGRX8", 1, 4),
    25: ("B5G5R5A1", 1, 2),
    28: ("A8", 1, 1),
    32: ("RGBA8", 1, 4),
    50: ("L8", 1, 1),
    int.from_bytes(b"DXT1", "little"): ("BC1", 4, 8),
    int.from_bytes(b"DXT3", "little"): ("BC2", 4, 16),
    int.from_bytes(b"DXT5", "little"): ("BC3", 4, 16),
    int.from_bytes(b"ATI1", "little"): ("BC4", 4, 8),
    int.from_bytes(b"ATI2", "little"): ("BC5", 4, 16),
    int.from_bytes(b"BC7 ", "little"): ("BC7", 4, 16),
}


class ResourceView:
    def __init__(self, payload: bytes, system_bytes: int, graphics_bytes: int):
        if min(system_bytes, graphics_bytes) < 0 or len(payload) != system_bytes + graphics_bytes:
            raise AssetError("resource-view page lengths disagree")
        self.system = memoryview(payload)[:system_bytes]
        self.graphics = memoryview(payload)[system_bytes:]

    def offset(self, pointer: int, size: int, *, graphics: bool = False) -> int:
        base, data = (GRAPHICS_BASE, self.graphics) if graphics else (SYSTEM_BASE, self.system)
        offset = pointer - base
        if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
            raise AssetError("resource pointer/range leaves its declared address space")
        return offset

    def system_struct(self, fmt: str, pointer: int) -> tuple:
        offset = self.offset(pointer, struct.calcsize(fmt))
        return struct.unpack_from(fmt, self.system, offset)

    def name(self, pointer: int) -> str:
        offset = self.offset(pointer, 1)
        data = bytes(self.system[offset : offset + 257])
        stop = data.find(b"\0")
        if stop < 0:
            raise AssetError("texture name exceeds limit or is unterminated")
        try:
            name = data[:stop].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("texture name is not ASCII") from exc
        # A texture name is a key, not a path: PC exporters leave trailing blanks ("NormalMap " in the Prowler
        # bike); the vehicle converter's texture-name repair turns them into identifiers before conversion.
        safe_name(name.rstrip(" ") or name)
        if "/" in name:
            raise AssetError("texture name contains a directory separator")
        return name

    def dictionary_entries(self, limits: Limits) -> list[tuple[int, int]]:
        self.offset(SYSTEM_BASE, 64)
        hp, hc, hcap, _ = self.system_struct("<QHHI", SYSTEM_BASE + 32)
        tp, tc, tcap, _ = self.system_struct("<QHHI", SYSTEM_BASE + 48)
        if hc != tc or hc > hcap or tc > tcap or max(hcap, tcap) > limits.max_entries:
            raise AssetError("texture dictionary counts/capacities disagree or exceed limit")
        for pointer, capacity, width in ((hp, hcap, 4), (tp, tcap, 8)):
            if capacity or pointer:
                self.offset(pointer, capacity * width)
                if pointer % width:
                    raise AssetError("unaligned texture dictionary table")
        entries = [
            (self.system_struct("<I", hp + index * 4)[0], self.system_struct("<Q", tp + index * 8)[0])
            for index in range(hc)
        ]
        hashes = [entry[0] for entry in entries]
        if len(set(hashes)) != len(hashes):
            raise AssetError("duplicate texture dictionary hash")
        if len({entry[1] for entry in entries}) != len(entries):
            raise AssetError("multiply referenced texture object")
        return entries


def linear_mip_sizes(width: int, height: int, levels: int, edge: int, block_bytes: int) -> list[int]:
    if not width or not height or not 1 <= levels <= max(width, height).bit_length():
        raise AssetError("invalid texture dimensions or mip count")
    return [
        ((max(1, width >> level) + edge - 1) // edge) * ((max(1, height >> level) + edge - 1) // edge) * block_bytes
        for level in range(levels)
    ]


def inspect_legacy_dictionary(payload: bytes, header: dict, limits: Limits) -> dict:
    if header["version"] != 13:
        raise AssetError("Legacy texture reader requires version 13; no version fallback")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    rows = []
    spans = []
    objects = []
    for name_hash, pointer in view.dictionary_entries(limits):
        start = view.offset(pointer, 144)
        if pointer % 16:
            raise AssetError("unaligned Legacy texture object")
        objects.append((start, start + 144))
        name = view.name(view.system_struct("<Q", pointer + 40)[0])
        if joaat(name) != name_hash:
            raise AssetError("texture name disagrees with dictionary hash")
        width, height, depth, stride, fmt, _, levels = view.system_struct("<4HIBB", pointer + 80)
        data_pointer = view.system_struct("<Q", pointer + 112)[0]
        row = {
            "name": name,
            "nameHash": f"0x{name_hash:08x}",
            "width": width,
            "height": height,
            "depth": depth,
            "stride": stride,
            "mipLevels": levels,
            "formatCode": f"0x{fmt:08x}",
            "systemOffset": start,
            "issues": [],
            "linearPayloadValidated": False,
            "ordinaryStaticConversionEligible": SCRIPT_TEXTURE_MARKER not in name.lower(),
        }
        rows.append(row)
        if not row["ordinaryStaticConversionEligible"]:
            row["issues"].append(
                "texture name selects target-native special handling; ordinary static conversion is unsupported"
            )
        if not width or not height or not stride or not 1 <= levels <= max(width, height).bit_length():
            raise AssetError(f"invalid texture shape: {name}")
        if depth != 1 or fmt not in LEGACY_FORMATS:
            row["issues"].append("unsupported texture depth/format; no substitution")
            continue
        format_name, edge, block_bytes = LEGACY_FORMATS[fmt]
        row["format"] = format_name
        # The Legacy reader advances by stride*height, quartered per mip. Check
        # this separately from block-rounded image sizes: malformed authoring
        # output must not cause bytes from the next texture to become a mip.
        length = stride * height
        stored_sizes = []
        for _ in range(levels):
            stored_sizes.append(length)
            length //= 4
        storage_bytes = sum(stored_sizes)
        offset = view.offset(data_pointer, storage_bytes, graphics=True)
        spans.append((offset, offset + storage_bytes, len(rows) - 1))
        expected = linear_mip_sizes(width, height, levels, edge, block_bytes)
        row.update(graphicsOffset=offset, legacyMipBytes=stored_sizes, linearMipBytes=expected)
        if stored_sizes != expected:
            row["issues"].append("Legacy stride/mip spans differ from block-rounded linear layout")
            continue
        row["linearPayloadValidated"] = True
    objects.sort()
    if any(left[1] > right[0] for left, right in pairwise(objects)):
        raise AssetError("overlapping texture objects")
    # Retain useful field diagnostics for unsupported source data, but withhold
    # mip hashes/validation for every affected span (including nested spans).
    spans.sort()
    prefix_end = -1
    for index, (start, end, row_index) in enumerate(spans):
        if prefix_end > start or (index + 1 < len(spans) and end > spans[index + 1][0]):
            rows[row_index]["issues"].append("declared Legacy graphics span overlaps another texture")
            rows[row_index]["linearPayloadValidated"] = False
        prefix_end = max(prefix_end, end)
    for start, end, row_index in spans:
        if rows[row_index]["linearPayloadValidated"]:
            rows[row_index]["payloadSha256"] = hashlib.sha256(view.graphics[start:end]).hexdigest()
    return {
        "layout": "pc-legacy-ytd-v13",
        "textureCount": len(rows),
        "textures": rows,
        "linearPayloadsValidated": all(row["linearPayloadValidated"] for row in rows),
        "ordinaryStaticTexturesEligible": all(row["ordinaryStaticConversionEligible"] for row in rows),
        "specialRuntimeTextureCount": sum(not row["ordinaryStaticConversionEligible"] for row in rows),
        "ps5LayoutValidated": False,
        "conversionAvailable": False,
    }
