"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import AssetError, page_bytes
from resource_page_layout import PC_FIELDS, pc_pages  # noqa: F401


def model_pages(layout: dict, flags: tuple[int, int], initial: bytes) -> tuple[bytes, list[dict]]:
    if len(flags) != 2 or len(initial) != layout["observationBytes"]:
        raise AssetError("page probe requires two flag words and a complete output observation")
    groups = [pc_pages(value) for value in flags]
    if sum(map(len, groups)) > layout["recordLimitForObservation"]:
        raise AssetError("page count exceeds the bounded record observation")
    output, rows = bytearray(initial), []
    for field in layout["zeroFields"]:
        output[field["offset"] : field["offset"] + field["bytes"]] = bytes(field["bytes"])
    payload_offset = 0
    for region, (word, sizes, base, count_at) in enumerate(
        zip(flags, groups, layout["sourceBases"], layout["countOffsets"], strict=True)
    ):
        if sum(sizes) != page_bytes(word):
            raise AssetError("PC page sequence differs from its separately implemented size formula")
        output[count_at] = len(sizes)
        address = base
        for size in sizes:
            index = len(rows)
            for field, value in (("sourceOffset", address), ("destinationOffset", 0), ("sizeOffset", size)):
                struct.pack_into("<Q", output, layout[field] + index * layout["stride"], value)
            rows.append(
                {
                    "region": region,
                    "source": hex(address),
                    "destination": 0,
                    "bytes": size,
                    "payloadOffsetHypothesis": payload_offset,
                }
            )
            address += size
            payload_offset += size
    return bytes(output), rows
