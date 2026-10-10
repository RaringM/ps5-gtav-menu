"""Serialize the observed PS5 v5 ordinary texture dictionary subset.

Inputs own their complete, already tiled pixel allocations. This module does
not infer tile modes, translate PC metadata, or evaluate game code. Target
metadata and the constructor view initializer are explicit caller policies.
The independent byte reader checks every emitted dictionary before return.

The 64-byte root, 88-byte objects and 32-byte views match the existing reader.
Ordinary object defaults are corroborated by the stock Banshee dictionary and
the authored/Razor candidate bytes. The historical constructor view uses 0x14
(Buffer) at offset 16; ordinary 2D serialized views use 0x41 (Texture2D).
Both policies are explicit. Zero incoming view-class placeholders remain
incompletely qualified;
structural round trips do not establish GPU, metadata or runtime compatibility.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass

from .asset_formats import AssetError, Limits, page_bytes
from .asset_textures import GRAPHICS_BASE, SYSTEM_BASE, linear_mip_sizes
from .hashes import joaat
from .ps5_texture_reader import PS5_TEXTURE_FORMATS, parse_ps5_texture_dictionary
from .texture_names import LEGACY_NAME_POLICY, texture_name_issue, validate_name_policy

# Host resource bounds, not target engine capacities. Each domain uses one
# power-of-two page, so individual objects/allocations never cross pages.
MAX_PAYLOAD_BYTES = 64 * 1024 * 1024
MAX_TEXTURES = 64
_MIN_PAGE_BYTES = 8192
_VIEW_INITIALIZERS = {
    "constructor-v1": bytes.fromhex("000000000000000000000000000000001400ffffffffffff0000000000000000"),
    "ordinary2d-serialized-v1": bytes.fromhex("000000000000000000000000000000004100ffffffffffff0000000000000000"),
}


@dataclass(frozen=True)
class Ps5TextureMetadata:
    """Explicit target policy; values are not copied from a PC header.

    ``metadata_candidate`` is an opaque target uint32. Supplying a value does
    not qualify its meaning. Only the observed ordinary flags/refcount/view
    kind and the named constructor initializer are supported.
    """

    flags: int
    metadata_candidate: int
    reference_count: int
    view_type: int
    view_initializer: str


@dataclass(frozen=True)
class Ps5TextureInput:
    """One complete ordinary 2D texture with an explicit storage layout."""

    name: str
    width: int
    height: int
    format_code: int
    mip_count: int
    tile_mode: int
    allocation: bytes
    storage_alignment: int
    metadata: Ps5TextureMetadata


@dataclass(frozen=True)
class Ps5TextureStorage:
    """Storage-only planning input; no pixel allocation or target preset needed."""

    name: str
    storage_bytes: int
    alignment_bytes: int


def _align(size: int, alignment: int) -> int:
    return (size + alignment - 1) & -alignment


def _page_size(size: int) -> int:
    return max(_MIN_PAGE_BYTES, 1 << (size - 1).bit_length())


def _page_flags(size: int) -> int:
    # Choose the lowest single-page encoding independently of the reader's
    # page iterator. The version nibbles are added only by the envelope writer.
    candidates = [
        (1 << shift) | scale
        for shift in (4, 5, 7, 11, 17, 24, 25, 26, 27)
        for scale in range(16)
        if page_bytes((1 << shift) | scale) == size
    ]
    if not candidates:
        raise AssetError("PS5 texture page size cannot be encoded")
    return min(candidates)


_PAGE_CLASSES = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))


def _pages_flags(sizes: list[int]) -> int:
    """Flags for a largest-first list of power-of-two pages (count fields per size class)."""
    for scale in range(16):
        flags = scale
        for rank, (shift, width) in enumerate(_PAGE_CLASSES):
            count = sizes.count((512 << scale) << (8 - rank))
            if count >= 1 << width:
                break
            flags |= count << shift
        else:
            if page_bytes(flags) == sum(sizes) and _class_count(flags) == len(sizes):
                return flags
    raise AssetError("PS5 texture page list cannot be encoded")


def _class_count(flags: int) -> int:
    return sum((flags >> shift) & ((1 << width) - 1) for shift, width in _PAGE_CLASSES)


def _graphics_pages(rows: list[tuple[int, int]], page: int) -> tuple[list[int], list[int]]:
    """First-fit (bytes, alignment) allocations into `page`-byte pages; none crosses a page.

    Returns each allocation's offset in the concatenated graphics space and the page sizes
    (all `page` bytes except the last, shrunk to the power of two that holds it).
    """
    if page < _MIN_PAGE_BYTES or page & (page - 1):
        raise AssetError("PS5 texture graphics page size must be a power of two >= 8 KiB")
    used: list[int] = []
    placed = []
    for size, alignment in rows:
        if size > page or alignment > page:
            raise AssetError("PS5 texture allocation exceeds the graphics page size")
        for index, cursor in enumerate(used):  # noqa: B007 (index is used after the loop)
            at = _align(cursor, alignment)
            if at + size <= page:
                break
        else:
            index, at = len(used), 0
            used.append(0)
        used[index] = at + size
        placed.append(index * page + at)
    sizes = [page] * len(used)
    sizes[-1] = _page_size(used[-1])
    return placed, sizes


def _validate(texture: Ps5TextureInput, name_policy: str) -> tuple[int, bytes, int]:
    if type(texture) is not Ps5TextureInput:
        raise AssetError("PS5 texture writer requires typed texture inputs")
    name_issue = texture_name_issue(texture.name, name_policy)
    if name_issue:
        raise AssetError(name_issue)
    if any(
        type(value) is not int
        for value in (
            texture.width,
            texture.height,
            texture.format_code,
            texture.mip_count,
            texture.tile_mode,
            texture.storage_alignment,
        )
    ):
        raise AssetError("PS5 texture shape, format, tile mode and alignment require integers")
    if not 1 <= texture.width <= 16384 or not 1 <= texture.height <= 16384:
        raise AssetError("PS5 texture dimensions exceed the supported range")
    if texture.format_code not in PS5_TEXTURE_FORMATS:
        raise AssetError("PS5 texture format is outside the ordinary writer subset")
    if texture.tile_mode not in (1, 5, 9):
        raise AssetError("PS5 texture writer requires an explicit supported tile mode; automatic mode is unresolved")
    _, edge, element_bytes = PS5_TEXTURE_FORMATS[texture.format_code]
    linear_sizes = linear_mip_sizes(texture.width, texture.height, texture.mip_count, edge, element_bytes)
    alignment = texture.storage_alignment
    if not element_bytes <= alignment <= 65536 or alignment & (alignment - 1):
        raise AssetError("PS5 texture storage alignment must be a bounded power of two covering its element")
    if (
        type(texture.allocation) is not bytes
        or not sum(linear_sizes) <= len(texture.allocation) <= MAX_PAYLOAD_BYTES
        or len(texture.allocation) % element_bytes
    ):
        raise AssetError("PS5 texture allocation must contain immutable, complete, bounded storage")
    metadata = texture.metadata
    if type(metadata) is not Ps5TextureMetadata or any(
        type(value) is not int
        for value in (metadata.flags, metadata.metadata_candidate, metadata.reference_count, metadata.view_type)
    ):
        raise AssetError("PS5 texture metadata requires explicit typed policy fields")
    if (
        (metadata.flags, metadata.reference_count, metadata.view_type) != (0x260208, 1, 2)
        or type(metadata.view_initializer) is not str
        or metadata.view_initializer not in _VIEW_INITIALIZERS
        or not 0 <= metadata.metadata_candidate <= 0xFFFFFFFF
    ):
        raise AssetError("PS5 texture metadata policy is outside the observed ordinary subset")
    return joaat(texture.name), texture.name.encode("ascii") + b"\0", element_bytes


def _stored_deflate(payload: bytes) -> bytes:
    """RFC 1951 stored blocks give compressor-version-independent output."""
    output = bytearray()
    for start in range(0, len(payload), 65535):
        stop = min(start + 65535, len(payload))
        count = stop - start
        output.extend(struct.pack("<BHH", int(stop == len(payload)), count, count ^ 0xFFFF))
        output.extend(payload[start:stop])
    return bytes(output)


def plan_ps5_texture_dictionary(
    textures: Sequence[Ps5TextureStorage],
    limits: Limits | None = None,
    *,
    name_policy: str = LEGACY_NAME_POLICY,
    graphics_page_bytes: int | None = None,
) -> dict:
    """Plan the writer's exact page/encoded sizes without allocating pixel bytes.

    ``graphics_page_bytes`` (a power of two) spreads the graphics allocations over pages of that
    size, retail-style (no allocation crosses a page; the last page shrinks). Without it the
    graphics domain is one power-of-two page, as before.

    Budget issues are returned together. Invalid storage/name inputs still fail
    closed. The buffer reservation is conservative, not a Python heap estimate.
    This does not validate source pixels, metadata choices or GPU compatibility.
    """
    validate_name_policy(name_policy)
    limits = Limits() if limits is None else limits
    if not isinstance(textures, Sequence) or not 1 <= len(textures) <= min(MAX_TEXTURES, limits.max_entries):
        raise AssetError("PS5 texture dictionary requires a bounded, nonempty sequence")
    for row in textures:
        if type(row) is not Ps5TextureStorage:
            raise AssetError("texture storage planning requires typed inputs")
        if issue := texture_name_issue(row.name, name_policy):
            raise AssetError(issue)
        if (
            type(row.storage_bytes) is not int
            or not 1 <= row.storage_bytes <= MAX_PAYLOAD_BYTES
            or type(row.alignment_bytes) is not int
            or not 1 <= row.alignment_bytes <= 65536
            or row.alignment_bytes & (row.alignment_bytes - 1)
        ):
            raise AssetError("texture storage planning requires bounded bytes and power-of-two alignment")
    entries = sorted(textures, key=lambda row: joaat(row.name))
    if len({joaat(row.name) for row in entries}) != len(entries):
        raise AssetError("PS5 texture dictionary names or hashes collide")
    count = len(entries)
    ordered = sorted(entries, key=lambda row: (-row.alignment_bytes, -row.storage_bytes, row.name))
    graphics_at = {}
    if graphics_page_bytes is None:
        end = 0
        for row in ordered:
            offset = _align(end, row.alignment_bytes)
            graphics_at[row.name] = offset
            end = offset + row.storage_bytes
        graphics_pages = [_page_size(end)]
    else:
        offsets, graphics_pages = _graphics_pages(
            [(row.storage_bytes, row.alignment_bytes) for row in ordered], graphics_page_bytes
        )
        graphics_at = {row.name: offset for row, offset in zip(ordered, offsets, strict=True)}
    # ResourcePagesInfo: 16 bytes + one 8-byte record per page (one system page).
    keys_at = 64 + 16 + 8 * (1 + len(graphics_pages))
    values_at = _align(keys_at + count * 4, 8)
    cursor = values_at + count * 8
    objects = {}
    for row in entries:
        obj_at = _align(cursor, 16)
        name_at = obj_at + 120
        objects[row.name] = (obj_at, name_at)
        cursor = name_at + len(row.name.encode("ascii")) + 1
    system_size = _page_size(cursor)
    graphics_size = sum(graphics_pages)
    graphics_flags = _page_flags(graphics_size) if graphics_page_bytes is None else _pages_flags(graphics_pages)
    payload_size = system_size + graphics_size
    encoded_size = 16 + payload_size + 5 * ((payload_size + 65534) // 65535)
    byte_budget = 2 * sum(row.storage_bytes for row in entries) + 2 * payload_size + 2 * encoded_size
    issues = []
    for value, maximum, label in (
        (payload_size, MAX_PAYLOAD_BYTES, "writer payload bytes"),
        (byte_budget, limits.max_total_bytes, "writer buffer reservation"),
        (system_size, limits.max_metadata_bytes, "writer system-page metadata bytes"),
        (encoded_size, limits.max_file_bytes, "writer encoded output bytes"),
    ):
        if value > maximum:
            issues.append(f"{label} {value} exceeds limit {maximum}")
    return {
        "systemBytes": system_size,
        "graphicsBytes": graphics_size,
        "graphicsPages": graphics_pages,
        "graphicsFlags": graphics_flags,
        "payloadBytes": payload_size,
        "encodedBytes": encoded_size,
        "writerBufferBytes": byte_budget,
        "keysOffset": keys_at,
        "valuesOffset": values_at,
        "objects": objects,
        "graphicsOffsets": graphics_at,
        "issues": issues,
    }


def write_ps5_texture_dictionary(
    textures: Sequence[Ps5TextureInput],
    limits: Limits | None = None,
    *,
    name_policy: str = LEGACY_NAME_POLICY,
    graphics_page_bytes: int | None = None,
) -> bytes:
    """Write all supplied textures or reject the complete request.

    Input order cannot change output: the dictionary uses ascending name hashes
    and graphics allocations use descending alignment/size then ascending name.
    The caller must independently verify its tiled pixel addressing and source
    metadata policy. No qualification claim is produced by this function.
    The optional source-name policy retains the exact ASCII spelling; it never
    renames, normalizes or merges entries, and hashes remain case-insensitive.
    """
    validate_name_policy(name_policy)
    limits = Limits() if limits is None else limits
    if not isinstance(textures, Sequence) or not 1 <= len(textures) <= min(MAX_TEXTURES, limits.max_entries):
        raise AssetError("PS5 texture dictionary requires a bounded, nonempty sequence")
    entries = sorted(((*_validate(texture, name_policy), texture) for texture in textures), key=lambda item: item[0])
    if len({entry[0] for entry in entries}) != len(entries):
        raise AssetError("PS5 texture dictionary names or hashes collide")

    count = len(entries)
    plan = plan_ps5_texture_dictionary(
        [Ps5TextureStorage(row.name, len(row.allocation), row.storage_alignment) for _, _, _, row in entries],
        limits,
        name_policy=name_policy,
        graphics_page_bytes=graphics_page_bytes,
    )
    if plan["issues"]:
        raise AssetError("PS5 texture dictionary exceeds the writer or supplied byte limits")
    system_size, graphics_size = plan["systemBytes"], plan["graphicsBytes"]
    keys_at, values_at = plan["keysOffset"], plan["valuesOffset"]
    objects, graphics_at = plan["objects"], plan["graphicsOffsets"]

    system, graphics = bytearray(system_size), bytearray(graphics_size)
    struct.pack_into("<Q", system, 8, SYSTEM_BASE + 64)
    struct.pack_into("<I", system, 24, 1)
    struct.pack_into("<QHH", system, 32, SYSTEM_BASE + keys_at, count, count)
    struct.pack_into("<QHH", system, 48, SYSTEM_BASE + values_at, count, count)
    system[72:74] = bytes((1, len(plan["graphicsPages"])))
    for index, (key, name, element_bytes, texture) in enumerate(entries):
        obj_at, name_at = objects[texture.name]
        data_at = graphics_at[texture.name]
        metadata = texture.metadata
        struct.pack_into("<I", system, keys_at + index * 4, key)
        struct.pack_into("<Q", system, values_at + index * 8, SYSTEM_BASE + obj_at)
        struct.pack_into(
            "<3I", system, obj_at + 8, len(texture.allocation) // element_bytes, element_bytes, metadata.flags
        )
        struct.pack_into(
            "<3H5B",
            system,
            obj_at + 24,
            texture.width,
            texture.height,
            1,
            1,
            texture.format_code,
            texture.tile_mode,
            0,
            texture.mip_count,
        )
        struct.pack_into("<H", system, obj_at + 38, metadata.reference_count)
        struct.pack_into(
            "<3Q", system, obj_at + 40, SYSTEM_BASE + name_at, SYSTEM_BASE + obj_at + 88, GRAPHICS_BASE + data_at
        )
        struct.pack_into("<IH", system, obj_at + 64, metadata.metadata_candidate, metadata.view_type)
        system[obj_at + 88 : obj_at + 120] = _VIEW_INITIALIZERS[metadata.view_initializer]
        system[name_at : name_at + len(name)] = name
        graphics[data_at : data_at + len(texture.allocation)] = texture.allocation
    blob = struct.pack("<4sIII", b"RSC7", 5, _page_flags(system_size), plan["graphicsFlags"] | 0x50000000)
    blob += _stored_deflate(bytes(system) + bytes(graphics))
    parsed = parse_ps5_texture_dictionary(blob, limits)
    if parsed["textureCount"] != count:
        raise AssetError("PS5 texture independent readback lost a dictionary entry")
    for row, (_, _, _, texture) in zip(parsed["textures"], entries, strict=True):
        actual = tuple(
            row[key] for key in ("name", "width", "height", "formatCode", "mipLevels", "tileMode", "allocation")
        )
        expected = (
            texture.name,
            texture.width,
            texture.height,
            texture.format_code,
            texture.mip_count,
            texture.tile_mode,
            texture.allocation,
        )
        metadata = row["metadata"]
        if (
            actual != expected
            or metadata["flags"] != texture.metadata.flags
            or metadata["referenceCount"] != texture.metadata.reference_count
            or metadata["metadataCandidate"] != texture.metadata.metadata_candidate
            or metadata["viewType"] != texture.metadata.view_type
            or metadata["rawViewHex"] != _VIEW_INITIALIZERS[texture.metadata.view_initializer].hex()
            or (GRAPHICS_BASE + row["graphicsOffset"]) % texture.storage_alignment
        ):
            raise AssetError("PS5 texture independent readback differs from requested storage or metadata")
    return blob
