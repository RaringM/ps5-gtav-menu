"""Read the PS4 Legacy RSC7 v13 texture dictionary subset used by the writer."""

from __future__ import annotations

import struct
from itertools import pairwise

from .asset_formats import AssetError, Limits, decode_resource
from .asset_textures import GRAPHICS_BASE, ResourceView
from .hashes import joaat

OBJECT_BYTES = 144
VIEW_ENTRY_BYTES = 8


def parse_ps4_texture_dictionary(blob: bytes, limits: Limits | None = None) -> dict:
    limits = Limits() if limits is None else limits
    header, payload = decode_resource(blob, limits.max_file_bytes)
    if header["version"] != 13:
        raise AssetError("PS4 texture reader requires RSC7 version 13")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    rows = []
    spans = []
    for name_hash, pointer in view.dictionary_entries(limits):
        start = view.offset(pointer, OBJECT_BYTES)
        if pointer % 16:
            raise AssetError("unaligned PS4 texture object")
        name = view.name(struct.unpack_from("<Q", view.system, start + 0x28)[0])
        if joaat(name) != name_hash:
            raise AssetError("PS4 texture name disagrees with dictionary hash")
        width, height, depth = struct.unpack_from("<3H", view.system, start + 0x18)
        if not width or not height or depth != 1:
            raise AssetError(f"invalid PS4 texture shape: {name}")
        format_code = struct.unpack_from("<I", view.system, start + 0x10)[0]
        levels = (format_code >> 8) & 0xFF
        if not 1 <= levels <= 16:
            raise AssetError(f"invalid PS4 mip count: {name}")
        tile_mode = struct.unpack_from("<I", view.system, start + 0x30)[0]
        data_pointer, data_bytes = struct.unpack_from("<QQ", view.system, start + 0x48)
        view_pointer = struct.unpack_from("<Q", view.system, start + 0x58)[0]
        view_bytes = levels * VIEW_ENTRY_BYTES
        view_at = view.offset(view_pointer, view_bytes)
        graphics_at = view.offset(data_pointer, data_bytes, graphics=True)
        rows.append(
            {
                "name": name,
                "nameHash": f"0x{name_hash:08x}",
                "width": width,
                "height": height,
                "depth": depth,
                "mipLevels": levels,
                "formatCode": format_code,
                "tileMode": tile_mode,
                "systemOffset": start,
                "graphicsOffset": graphics_at,
                "allocation": bytes(view.graphics[graphics_at : graphics_at + data_bytes]),
                "rawObject": bytes(view.system[start : start + OBJECT_BYTES]),
                "rawView": bytes(view.system[view_at : view_at + view_bytes]),
                "dataPointer": data_pointer,
                "viewPointer": view_pointer,
            }
        )
        spans.append((start, start + OBJECT_BYTES))
    spans.sort()
    if any(left[1] > right[0] for left, right in pairwise(spans)):
        raise AssetError("overlapping PS4 texture objects")
    return {
        "layout": "ps4-legacy-otd-v13",
        "header": header,
        "textureCount": len(rows),
        "textures": rows,
        "systemBase": 0x50000000,
        "graphicsBase": GRAPHICS_BASE,
    }
