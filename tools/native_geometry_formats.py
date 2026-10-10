"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import itertools
import struct

from gtavmenu_tools.asset_formats import AssetError

MAX_DECLARATIONS = 16384


MAX_SYSTEM = 128 * 1024 * 1024  # host work bound: a 64 MiB resource and its moved copy


def read_declarations(system, base, observed_buffers, contract):
    if (
        not isinstance(system, bytes)
        or len(system) > MAX_SYSTEM
        or type(base) is not int
        or base <= 0
        or base + len(system) >= 1 << 64
    ):
        raise AssetError("native declaration storage/base is outside immutable uint64 host bounds")
    if len(observed_buffers) > MAX_DECLARATIONS * 2:
        raise AssetError("native declaration buffer input exceeds host work bound")
    selected = [row for row in observed_buffers if row["group"] == 0]
    if not selected or len(selected) > MAX_DECLARATIONS:
        raise AssetError("native declaration observation count exceeds host bounds")
    layout, mapping = contract["layout"], contract["formatMapper"]
    result, unique = [], {}
    for row in selected:
        field = row["systemOffset"] + contract["declarationOffset"]
        if field < 0 or field + 8 > len(system):
            raise AssetError("native declaration pointer field exceeds system storage")
        pointer = struct.unpack_from("<Q", system, field)[0]
        offset = pointer - base
        size = layout["observedBytes"]
        if pointer == 0 or offset < 0 or offset % 4 or offset + size > len(system):
            raise AssetError("native declaration pointer is null, misaligned or out of system bounds")
        mode = struct.unpack_from("<I", system, offset + layout["modeOffset"])[0]
        if mode & layout["modeMask"]:
            raise AssetError("native declaration alternate offset/stream mode is unsupported and is not repaired")
        attrs = []
        for spec in layout["fields"]:
            code = system[offset + spec["formatOffset"]]
            byte_offset = struct.unpack_from("<I", system, offset + spec["offsetField"])[0]
            kind = mapping["mapping"].get(str(code), mapping["default"])
            if code and kind == mapping["default"]:
                raise AssetError(
                    f"native declaration format {code} at selected field {spec['index']} reaches unsupported/default mapping"
                )
            if code and byte_offset >= row["stride"]:
                raise AssetError("native declaration active attribute offset exceeds selected buffer stride")
            attrs.append(
                {
                    "field": spec["index"],
                    "format": code,
                    "active": code != 0,
                    "byteOffset": byte_offset,
                    "mappedIdentity": kind if code else None,
                }
            )
        if not any(a["active"] for a in attrs):
            raise AssetError("native declaration has no active selected attributes")
        raw = system[offset : offset + size]
        digest = hashlib.sha256(raw).hexdigest()
        unique[offset] = {"systemOffset": offset, "observedBytes": size, "sha256": digest}
        result.append(
            {
                "geometry": row["geometry"],
                "bufferSystemOffset": row["systemOffset"],
                "declarationSystemOffset": offset,
                "declarationSha256": digest,
                "stride": row["stride"],
                "modeWord": mode,
                "selectedModeBits": mode & layout["modeMask"],
                "uninterpretedModeBits": mode & ~layout["modeMask"],
                "attributes": attrs,
            }
        )
    positions = sorted(unique)
    if any(a + layout["observedBytes"] > b for a, b in itertools.pairwise(positions)):
        raise AssetError("native declaration selected prefixes partially overlap")
    return {
        "declarations": result,
        "declarationCount": len(result),
        "uniqueDeclarations": [unique[k] for k in positions],
        "scope": "Active starts bounded by stride; widths, value decoding, semantic names, unused bytes and complete object/storage overlap remain unqualified; shared identical declaration pointers retained",
    }
