"""Complete PC Legacy texture intake for offline conversion checks.

Field offsets are format facts from CodeWalker's Texture.cs (Legacy branch).
Recovering all source mip bytes does not establish a PS5 metadata mapping.
"""

from __future__ import annotations

import hashlib
import struct
from itertools import pairwise

from .asset_formats import AssetError, Limits, decode_resource
from .asset_textures import SYSTEM_BASE, ResourceView, inspect_legacy_dictionary

DEFAULT_LIMITS = Limits()
_PAGE_COUNT_FIELDS = ((4, 1), (5, 2), (7, 4), (11, 6), (17, 7), (24, 1), (25, 1), (26, 1), (27, 1))

# Keep every scalar outside the relocated pointers and image geometry. In
# particular, zero unknown fields are observations, not permission to discard
# the source metadata. VFT/Unknown_4h describe the source class and are separate.
LEGACY_METADATA_FIELDS = {
    **{f"Unknown_{offset:X}h": (offset, 4) for offset in range(8, 40, 4)},
    "Unknown_30h": (0x30, 2),
    "Unknown_32h": (0x32, 2),
    **{f"Unknown_{offset:X}h": (offset, 4) for offset in range(0x34, 0x40, 4)},
    "UsageData": (0x40, 4),
    "Unknown_44h": (0x44, 4),
    "ExtraFlags": (0x48, 4),
    "Unknown_4Ch": (0x4C, 4),
    "Unknown_5Ch": (0x5C, 1),
    "Unknown_5Eh": (0x5E, 2),
    **{f"Unknown_{offset:X}h": (offset, 4) for offset in range(0x60, 0x70, 4)},
    **{f"Unknown_{offset:X}h": (offset, 4) for offset in range(0x78, 0x90, 4)},
}


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def _dictionary_metadata(view: ResourceView, header: dict) -> dict:
    """Retain the entire source root and bounded page information verbatim."""

    view.offset(SYSTEM_BASE, 64)
    raw_root = bytes(view.system[:64])
    pointer = struct.unpack_from("<Q", raw_root, 8)[0]
    if not pointer or pointer % 8:
        raise AssetError("source page information pointer must be present and 8-byte aligned")
    start = view.offset(pointer, 16)
    system_count, graphics_count = struct.unpack_from("<BB", view.system, start + 8)
    expected_counts = [
        sum((int(header[key], 0) >> shift) & ((1 << width) - 1) for shift, width in _PAGE_COUNT_FIELDS)
        for key in ("systemFlags", "graphicsFlags")
    ]
    if [system_count, graphics_count] != expected_counts:
        raise AssetError("source page information counts disagree with RSC7 page flags")
    length = 16 + 8 * (system_count + graphics_count)
    view.offset(pointer, length)
    # Legacy authoring tools may leave these serialized records as 0xCD fill.
    # Preserve them as source evidence; a target writer supplies its own policy.
    raw_pages = bytes(view.system[start : start + length])
    return {
        "rawRootHex": raw_root.hex(),
        "sourceClassWords": list(struct.unpack_from("<II", raw_root)),
        "scalars": {
            **{f"Unknown_{offset:X}h": struct.unpack_from("<I", raw_root, offset)[0] for offset in range(16, 32, 4)},
            "keyReserved": struct.unpack_from("<I", raw_root, 44)[0],
            "valueReserved": struct.unpack_from("<I", raw_root, 60)[0],
        },
        "pageInfo": {
            "pointer": f"0x{pointer:016x}",
            "systemOffset": start,
            "bytes": length,
            "systemPagesCount": system_count,
            "graphicsPagesCount": graphics_count,
            "scalars": {
                name: int.from_bytes(raw_pages[offset : offset + width], "little")
                for name, offset, width in (
                    ("Unknown_0h", 0, 4),
                    ("Unknown_4h", 4, 4),
                    ("Unknown_Ah", 10, 2),
                    ("Unknown_Ch", 12, 4),
                )
            },
            "rawHex": raw_pages.hex(),
        },
    }


def _structural_ranges(view: ResourceView, rows: list[dict], dictionary_metadata: dict) -> None:
    """Reject aliased root/page information/tables/objects/names."""

    pages = dictionary_metadata["pageInfo"]
    spans = [
        (0, 64, "dictionary root"),
        (pages["systemOffset"], pages["systemOffset"] + pages["bytes"], "page information"),
    ]
    for offset, width, label in ((32, 4, "keys"), (48, 8, "values")):
        pointer, _count, capacity, _reserved = view.system_struct("<QHHI", SYSTEM_BASE + offset)
        start = view.offset(pointer, capacity * width)
        spans.append((start, start + capacity * width, label))
    keys = [int(row["nameHash"], 0) for row in rows]
    if any(left >= right for left, right in pairwise(keys)):
        raise AssetError("source dictionary keys must be strictly increasing")
    names = [row["name"].lower() for row in rows]
    if len(set(names)) != len(names):
        raise AssetError("source dictionary contains case-aliased names")
    for row in rows:
        start = row["systemOffset"]
        spans.append((start, start + 144, f"texture {row['name']}"))
        pointer = view.system_struct("<Q", SYSTEM_BASE + start + 40)[0]
        first = view.offset(pointer, len(row["name"]) + 1)
        spans.append((first, first + len(row["name"]) + 1, f"name {row['name']}"))
    spans.sort()
    for left, right in pairwise(spans):
        if left[1] > right[0]:
            raise AssetError(f"source structure ranges overlap: {left[2]} / {right[2]}")


def read_pc_texture_dictionary(blob: bytes, limits: Limits = DEFAULT_LIMITS) -> dict:
    """Recover every ordinary Legacy YTD entry, or reject the whole dictionary.

    The returned mip tuples contain the original pixel or compressed-block bytes,
    in linear order. No mip synthesis, format substitution, or metadata defaulting
    occurs. Metadata is exposed verbatim for a separately qualified mapper.
    """

    if type(blob) is not bytes or not 16 <= len(blob) <= limits.max_file_bytes:
        raise AssetError("source YTD is empty or exceeds the byte limit")
    header, payload = decode_resource(blob, min(limits.max_file_bytes, limits.max_total_bytes))
    if header["systemBytes"] > limits.max_metadata_bytes:
        raise AssetError("source YTD system pages exceed the metadata byte limit")
    if len(payload) + header["graphicsBytes"] > limits.max_total_bytes:
        raise AssetError("source YTD mip recovery exceeds the cumulative byte limit")
    dictionary = inspect_legacy_dictionary(payload, header, limits)
    rows = dictionary["textures"]
    if not rows:
        raise AssetError("source YTD must contain at least one texture")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    dictionary_metadata = _dictionary_metadata(view, header)
    _structural_ranges(view, rows, dictionary_metadata)
    failures = [
        f"{row['name']}: {', '.join(row['issues']) or 'unsupported source row'}"
        for row in rows
        if row["issues"] or not row["linearPayloadValidated"] or not row["ordinaryStaticConversionEligible"]
    ]
    if failures:
        raise AssetError("whole-dictionary admission failed: " + "; ".join(failures))
    textures = []
    for row in rows:
        start = row["systemOffset"]
        raw_header = bytes(view.system[start : start + 144])
        metadata = {
            name: int.from_bytes(raw_header[offset : offset + width], "little")
            for name, (offset, width) in LEGACY_METADATA_FIELDS.items()
        }
        offset = row["graphicsOffset"]
        mips = []
        for length in row["linearMipBytes"]:
            mips.append(bytes(view.graphics[offset : offset + length]))
            offset += length
        textures.append(
            {
                key: row[key]
                for key in (
                    "name",
                    "nameHash",
                    "width",
                    "height",
                    "depth",
                    "stride",
                    "format",
                    "formatCode",
                    "mipLevels",
                )
            }
            | {
                "mips": tuple(mips),
                "metadata": metadata,
                "sourceClassWords": list(struct.unpack_from("<II", raw_header)),
                "rawHeaderHex": raw_header.hex(),
                "headerSha256": digest(raw_header),
            }
        )
    return {
        "header": header,
        "sourceSha256": digest(blob),
        "dictionaryMetadata": dictionary_metadata,
        "textures": textures,
        "textureCount": len(textures),
        "mipCount": sum(row["mipLevels"] for row in textures),
        "linearBytes": sum(len(mip) for row in textures for mip in row["mips"]),
        "allSourceMipBytesRecovered": True,
        "sourceLegacyMetadataMapped": False,
        "conversionAvailable": False,
    }


def metadata_blockers(source: dict) -> list[dict]:
    """State the unresolved transfer explicitly; no metadata policy is inferred."""

    return [
        {
            "name": row["name"],
            "reason": "Legacy usage/flags and remaining header scalars have no qualified PS5 transfer policy",
            "usageData": f"0x{row['metadata']['UsageData']:08x}",
            "extraFlags": f"0x{row['metadata']['ExtraFlags']:08x}",
            "nonzeroMetadata": {key: value for key, value in row["metadata"].items() if value},
            "allMetadata": row["metadata"],
        }
        for row in source["textures"]
    ]
