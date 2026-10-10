"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import AssetError


def serialized_info(
    system: bytes, base: int, layout: dict, pages: dict, raw: bytes, occupied: list[tuple[int, int]]
) -> dict:
    regions = counts(layout, pages, raw)
    pointer_at = layout["rootPointerOffset"]
    if not 0 <= pointer_at <= len(system) - 8:
        raise AssetError("page-info root pointer field leaves the system observation")
    pointer = struct.unpack_from("<Q", system, pointer_at)[0]
    offset = pointer - base
    length = layout["recordOrigin"] + sum(regions) * layout["packedStride"]
    if pointer % 8 or offset < 0 or offset + length > len(system):
        raise AssetError("serialized page-info pointer/reservation leaves aligned system bounds")
    if any(offset < end and start < offset + length for start, end in occupied):
        raise AssetError("serialized page-info reservation overlaps another observed structure")
    info = system[offset : offset + length]
    if tuple(info[layout[k]] for k in ("systemCount", "graphicsCount")) != regions:
        raise AssetError("serialized page-info counts differ from independently decoded resource pages")
    if info[layout["extraCount"]] != raw[layout["extraSource"]] or info[layout["zeroField"]]:
        raise AssetError("serialized page-info auxiliary bytes differ from the observed form")
    if any(info[layout["recordOrigin"] :]):
        raise AssetError("nonzero serialized page-info records are outside the observed form")
    return {
        "systemOffset": offset,
        "observedBytes": length,
        "counts": list(regions),
        "headerHex": info[: layout["recordOrigin"]].hex(),
        "serializedRecordsZero": True,
        "reservationDisjointFromObservedStructures": True,
    }


def counts(layout: dict, pages: dict, raw: bytes) -> tuple[int, int]:
    if len(raw) != pages["observationBytes"]:
        raise AssetError("page-info needs a complete declared page observation")
    result = tuple(raw[at] for at in pages["countOffsets"])
    if sum(result) > layout["recordLimitForObservation"]:
        raise AssetError("page-info record count exceeds the observation limit")
    return result


def model(layout: dict, pages: dict, raw: bytes, initial: bytes) -> bytes:
    regions = counts(layout, pages, raw)
    count = sum(regions)
    length = layout["recordOrigin"] + count * layout["packedStride"]
    if len(initial) != length:
        raise AssetError("page-info output observation has the wrong length")
    result = bytearray(initial)
    result[layout["systemCount"]], result[layout["graphicsCount"]] = regions
    result[layout["extraCount"]] = raw[layout["extraSource"]]
    result[layout["zeroField"]] = 0
    for index in range(count):
        size = struct.unpack_from("<Q", raw, pages["sizeOffset"] + index * pages["stride"])[0]
        destination = struct.unpack_from("<Q", raw, pages["destinationOffset"] + index * pages["stride"])[0]
        word = (size >> layout["quantumShift"]) & ((1 << layout["tagInputBits"]) - 1)
        tag = (layout["tagInputBits"] - word.bit_length()) ^ layout["tagXor"]
        struct.pack_into("<Q", result, layout["recordOrigin"] + index * layout["packedStride"], destination | tag)
    return bytes(result)
