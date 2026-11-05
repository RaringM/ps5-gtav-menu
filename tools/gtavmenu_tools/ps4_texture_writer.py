"""PS4 Legacy RSC7 v13 textures with GCN Thin1DThin microtiles.

The supported stock profile uses tile index 13 and Legacy tile word 0x800001.
BC blocks are Morton ordered inside 8x8 microtiles; each mip occupies its own
padded region, in largest-to-smallest order. This differs from PS5 GFX10.
Only ordinary single-slice BC1/BC3/BC5 surfaces are supported. Stock Adder,
Asea and vehshare mip views establish pitch, offsets and allocation sizes.
"""

from __future__ import annotations

import struct
import zlib
from collections.abc import Iterator, Sequence
from dataclasses import dataclass

from .asset_formats import AssetError, Limits
from .asset_textures import GRAPHICS_BASE, SYSTEM_BASE
from .hashes import joaat
from .ps4_texture_reader import OBJECT_BYTES, VIEW_ENTRY_BYTES, parse_ps4_texture_dictionary
from .ps5_texture_writer import Ps5TextureStorage, _page_flags, _page_size, plan_ps5_texture_dictionary
from .texture_names import LEGACY_NAME_POLICY, texture_name_issue, validate_name_policy

MAX_STORAGE_BYTES = 64 * 1024 * 1024
MAX_BLOCKS = 4 * 1024 * 1024
TILE_MODE = 0x800001
FORMAT_CODES = {"BC1": 0x47, "BC3": 0x4D, "BC5": 0x53}
FORMAT_BYTES = {"BC1": 8, "BC3": 16, "BC5": 16}


@dataclass(frozen=True)
class Ps4MipLayout:
    width_blocks: int
    height_blocks: int
    pitch_blocks: int
    padded_height_blocks: int
    offset_bytes: int


@dataclass(frozen=True)
class Ps4TextureLayout:
    width: int
    height: int
    format_name: str
    block_bytes: int
    storage_bytes: int
    alignment_bytes: int
    mips: tuple[Ps4MipLayout, ...]

    def offsets(self, level: int) -> Iterator[int]:
        if type(level) is not int or not 0 <= level < len(self.mips):
            raise AssetError("PS4 texture mip index is outside the layout")
        mip = self.mips[level]
        for y in range(mip.height_blocks):
            for x in range(mip.width_blocks):
                tile = (y // 8) * (mip.pitch_blocks // 8) + x // 8
                # Morton order within a thin 8x8 microtile, in storage elements.
                within = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)
                yield mip.offset_bytes + (tile * 64 + within) * self.block_bytes


def surface_layout(width: int, height: int, mip_count: int, format_name: str) -> Ps4TextureLayout:
    if any(type(v) is not int or not 1 <= v <= 16384 for v in (width, height)):
        raise AssetError("PS4 texture dimensions exceed the supported range")
    if type(mip_count) is not int or not 1 <= mip_count <= max(width, height).bit_length():
        raise AssetError("PS4 texture mip count exceeds the supported range")
    if type(format_name) is not str or format_name not in FORMAT_BYTES:
        raise AssetError("PS4 texture layout requires BC1, BC3 or BC5")
    block_bytes = FORMAT_BYTES[format_name]
    rows = []
    storage = blocks = 0
    for level in range(mip_count):
        w, h = (max(1, width >> level) + 3) // 4, (max(1, height >> level) + 3) // 4
        pitch, padded_height = (w + 7) & -8, (h + 7) & -8
        rows.append(Ps4MipLayout(w, h, pitch, padded_height, storage))
        storage += pitch * padded_height * block_bytes
        blocks += w * h
    if storage > MAX_STORAGE_BYTES or blocks > MAX_BLOCKS:
        raise AssetError("PS4 texture layout exceeds its storage or work budget")
    return Ps4TextureLayout(width, height, format_name, block_bytes, storage, 64 * block_bytes, tuple(rows))


def tile_mips(mips: Sequence[bytes], layout: Ps4TextureLayout) -> bytes:
    if type(layout) is not Ps4TextureLayout or type(mips) not in (list, tuple) or len(mips) != len(layout.mips):
        raise AssetError("PS4 texture tiling requires a typed layout and every mip exactly once")
    allocation = bytearray(layout.storage_bytes)
    for level, (payload, mip) in enumerate(zip(mips, layout.mips, strict=True)):
        if type(payload) is not bytes or len(payload) != mip.width_blocks * mip.height_blocks * layout.block_bytes:
            raise AssetError("PS4 texture mip payload differs from its complete extent")
        for index, offset in enumerate(layout.offsets(level)):
            start = index * layout.block_bytes
            allocation[offset : offset + layout.block_bytes] = payload[start : start + layout.block_bytes]
    return bytes(allocation)


def untile_mips(allocation: bytes, layout: Ps4TextureLayout) -> tuple[bytes, ...]:
    if type(layout) is not Ps4TextureLayout or type(allocation) is not bytes or len(allocation) != layout.storage_bytes:
        raise AssetError("PS4 texture allocation differs from the layout storage extent")
    return tuple(
        b"".join(allocation[offset : offset + layout.block_bytes] for offset in layout.offsets(level))
        for level in range(len(layout.mips))
    )


MAX_PAYLOAD_BYTES = 64 * 1024 * 1024
MAX_TEXTURES = 64


@dataclass(frozen=True)
class Ps4TextureInput:
    name: str
    width: int
    height: int
    format_code: int
    mip_count: int
    tile_mode: int
    allocation: bytes
    storage_alignment: int
    raw_object: bytes
    raw_view: bytes


def texture_view(layout: Ps4TextureLayout) -> bytes:
    if type(layout) is not Ps4TextureLayout:
        raise AssetError("PS4 texture view requires a complete typed layout")
    rows = []
    for mip in layout.mips:
        offset = mip.offset_bytes
        pitch = mip.pitch_blocks * layout.block_bytes
        if not 0 <= offset < 1 << 44 or not 0 < pitch < 1 << 20:
            raise AssetError("PS4 texture mip offset or pitch exceeds its serialized fields")
        rows.append(struct.pack("<II", ((offset & 0xFFF) << 20) | pitch, offset >> 12))
    return b"".join(rows)


def _validate(texture: Ps4TextureInput, name_policy: str) -> tuple[int, bytes]:
    if type(texture) is not Ps4TextureInput:
        raise AssetError("PS4 texture writer requires typed texture inputs")
    if issue := texture_name_issue(texture.name, name_policy):
        raise AssetError(issue)
    if not 1 <= texture.width <= 16384 or not 1 <= texture.height <= 16384:
        raise AssetError("PS4 texture dimensions exceed the supported range")
    if not 1 <= texture.mip_count <= 16:
        raise AssetError("PS4 texture mip count is invalid")
    if (texture.format_code >> 8) & 0xFF != texture.mip_count:
        raise AssetError("PS4 texture format word and mip count disagree")
    if (
        not 1 <= texture.storage_alignment <= 65536
        or texture.storage_alignment & (texture.storage_alignment - 1)
        or not 1 <= len(texture.allocation) <= MAX_PAYLOAD_BYTES
    ):
        raise AssetError("PS4 texture allocation or alignment is invalid")
    if len(texture.raw_object) != OBJECT_BYTES or len(texture.raw_view) != texture.mip_count * VIEW_ENTRY_BYTES:
        raise AssetError("PS4 texture metadata must contain one object and one view entry per mip")
    formats = {code: name for name, code in FORMAT_CODES.items()}
    if texture.format_code & 0xFF not in formats or texture.tile_mode != TILE_MODE or texture.raw_object[0x1E] != 13:
        raise AssetError("PS4 texture writer requires a reviewed BC format and Thin1DThin tile mode")
    layout = surface_layout(texture.width, texture.height, texture.mip_count, formats[texture.format_code & 0xFF])
    if (
        len(texture.allocation) != layout.storage_bytes
        or texture.storage_alignment != layout.alignment_bytes
        or texture.raw_view != texture_view(layout)
    ):
        raise AssetError("PS4 texture allocation, alignment or mip view differs from its native layout")
    for first, second in struct.iter_unpack("<II", texture.raw_view):
        offset, pitch = (second << 12) | (first >> 20), first & 0xFFFFF
        if not pitch or offset >= len(texture.allocation):
            raise AssetError("PS4 texture view entry exceeds its allocation or has no pitch")
    return joaat(texture.name), texture.name.encode("ascii") + b"\0"


def write_ps4_texture_dictionary(
    textures: Sequence[Ps4TextureInput],
    limits: Limits | None = None,
    *,
    name_policy: str = LEGACY_NAME_POLICY,
    graphics_page_bytes: int | None = None,
    max_textures: int = MAX_TEXTURES,
) -> bytes:
    validate_name_policy(name_policy)
    limits = Limits() if limits is None else limits
    if not isinstance(textures, Sequence) or not 1 <= len(textures) <= min(max_textures, limits.max_entries):
        raise AssetError("PS4 texture dictionary requires a bounded, nonempty sequence")
    entries = sorted(((*_validate(texture, name_policy), texture) for texture in textures), key=lambda item: item[0])
    if len({entry[0] for entry in entries}) != len(entries):
        raise AssetError("PS4 texture dictionary names or hashes collide")
    plan = plan_ps5_texture_dictionary(
        [Ps5TextureStorage(row.name, len(row.allocation), row.storage_alignment) for _, _, row in entries],
        limits,
        name_policy=name_policy,
        graphics_page_bytes=graphics_page_bytes,
    )
    if plan["issues"]:
        raise AssetError(
            "PS4 texture dictionary exceeds the writer or supplied byte limits: " + "; ".join(plan["issues"])
        )
    graphics_size = plan["graphicsBytes"]
    objects = {}
    cursor = plan["valuesOffset"] + len(entries) * 8
    for _, _, texture in entries:
        obj_at = (cursor + 15) & -16
        name_at = obj_at + 120
        objects[texture.name] = (obj_at, name_at)
        cursor = max(obj_at + OBJECT_BYTES, name_at + len(texture.name.encode("ascii")) + 1)
    cursor = max(
        cursor,
        max(
            name_at + len(texture.name.encode("ascii")) + 1
            for _, _, texture in entries
            for name_at in (objects[texture.name][1],)
        ),
    )
    view_offsets = {}
    for _, _, texture in entries:
        cursor = (cursor + 15) & -16
        view_offsets[texture.name] = cursor
        cursor += len(texture.raw_view)
    system_size = _page_size(cursor)
    system, graphics = bytearray(system_size), bytearray(graphics_size)
    struct.pack_into("<Q", system, 8, SYSTEM_BASE + 64)
    struct.pack_into("<I", system, 24, 1)
    struct.pack_into("<QHH", system, 32, SYSTEM_BASE + plan["keysOffset"], len(entries), len(entries))
    struct.pack_into("<QHH", system, 48, SYSTEM_BASE + plan["valuesOffset"], len(entries), len(entries))
    system[72:74] = bytes((1, len(plan["graphicsPages"])))
    for index, (key, name, texture) in enumerate(entries):
        obj_at, name_at = objects[texture.name]
        data_at = plan["graphicsOffsets"][texture.name]
        view_at = view_offsets[texture.name]
        struct.pack_into("<I", system, plan["keysOffset"] + index * 4, key)
        struct.pack_into("<Q", system, plan["valuesOffset"] + index * 8, SYSTEM_BASE + obj_at)
        system[obj_at : obj_at + OBJECT_BYTES] = texture.raw_object
        struct.pack_into("<I", system, obj_at + 0x10, texture.format_code)
        struct.pack_into("<I", system, obj_at + 0x30, texture.tile_mode)
        struct.pack_into("<3H", system, obj_at + 0x18, texture.width, texture.height, 1)
        struct.pack_into("<Q", system, obj_at + 0x28, SYSTEM_BASE + name_at)
        # The retail packed allocation word carries byte size in bits 8..28;
        # all retained flag bits belong to the same-format donor.
        allocation_word = struct.unpack_from("<I", texture.raw_object, 0x40)[0]
        struct.pack_into("<I", system, obj_at + 0x40, (allocation_word & 0xE00000FF) | len(texture.allocation))
        struct.pack_into("<Q", system, obj_at + 0x48, GRAPHICS_BASE + data_at)
        struct.pack_into("<Q", system, obj_at + 0x50, len(texture.allocation))
        struct.pack_into("<Q", system, obj_at + 0x58, SYSTEM_BASE + view_at)
        system[view_at : view_at + len(texture.raw_view)] = texture.raw_view
        system[name_at : name_at + len(name)] = name
        graphics[data_at : data_at + len(texture.allocation)] = texture.allocation
    payload = bytes(system) + bytes(graphics)
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    blob = struct.pack("<4sIII", b"RSC7", 13, _page_flags(system_size), plan["graphicsFlags"] | 0xD0000000)
    blob += compressor.compress(payload) + compressor.flush()
    parsed = parse_ps4_texture_dictionary(blob, limits)
    if parsed["textureCount"] != len(entries):
        raise AssetError("PS4 texture independent readback lost a dictionary entry")
    for row, (_, _, expected) in zip(parsed["textures"], entries, strict=True):
        actual = tuple(
            row[key] for key in ("name", "width", "height", "formatCode", "mipLevels", "tileMode", "allocation")
        )
        wanted = (
            expected.name,
            expected.width,
            expected.height,
            expected.format_code,
            expected.mip_count,
            expected.tile_mode,
            expected.allocation,
        )
        if actual != wanted or row["rawView"] != expected.raw_view:
            raise AssetError("PS4 texture independent readback differs from requested storage or metadata")
        if struct.unpack_from("<I", row["rawObject"], 0x40)[0] & 0x1FFFFF00 != len(expected.allocation):
            raise AssetError("PS4 texture readback packed allocation size differs")
    return blob
