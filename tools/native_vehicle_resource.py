"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import bisect
import hashlib
import math
import struct

import native_collision as collision
import resource_envelope
import resource_page_info
import resource_page_layout
import resource_pages
import texture_version
from gtavmenu_tools.asset_formats import AssetError, decode_resource
from native_resource_builder import ObjectArena, relocate
from texture_writer_fixups import Fixup, Page, serialize


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def plan(component_bytes: int, native: dict) -> dict:
    info = native["info"]
    quantum = 1 << info["quantumShift"]
    info_bytes = info["recordOrigin"] + info["packedStride"]
    info_offset = (component_bytes + 15) & -16
    used = info_offset + info_bytes
    size = max(quantum, 1 << (used - 1).bit_length())
    if not 0 < component_bytes < used <= size <= resource_envelope.MAX_PAYLOAD_BYTES:
        raise AssetError("vehicle graph exceeds the single-page resource budget")
    version = native["version"]
    flags = [flags_for(1, size) | (version >> 4) << 28, (version & 15) << 28]
    if resource_pages.pc_pages(flags[0]) != [size] or resource_pages.pc_pages(flags[1]):
        raise AssetError("vehicle page flag inverse disagrees")
    return {
        "componentBytes": component_bytes,
        "pageInfoOffset": info_offset,
        "pageInfoBytes": info_bytes,
        "systemBytes": size,
        "graphicsBytes": 0,
        "flags": flags,
        "version": version,
        "policy": "One power-of-two system page contains every owned object and referent; no graphics pages",
    }


def reference_check(envelope: bytes, contract: dict, native: dict) -> dict:
    header, payload = decode_resource(envelope, 32 * 1024 * 1024)
    if header["version"] != native["version"]:
        raise AssetError("vehicle native reference version differs from store request")
    flags = [int(header[k], 0) for k in ("systemFlags", "graphicsFlags")]
    pages = native["pages"]
    raw, rows = resource_pages.model_pages(pages, tuple(flags), bytes(pages["observationBytes"]))
    if [sum(row["bytes"] for row in rows if row["region"] == r) for r in (0, 1)] != [
        header["systemBytes"],
        header["graphicsBytes"],
    ]:
        raise AssetError("vehicle native reference page totals differ from envelope")
    info = resource_page_info.serialized_info(
        payload[: header["systemBytes"]],
        native["rootPlacement"]["sourceBase"],
        native["info"],
        pages,
        raw,
        [(0, contract["root"]["bytes"])],
    )
    values, _ = collision.typed(payload, info["systemOffset"], contract["header"])
    counts = [sum(row["region"] == r for row in rows) for r in (0, 1)]
    for name, value in values.items():
        expected = dict(zip(("SystemPagesCount", "GraphicsPagesCount"), counts, strict=True)).get(name, 0)
        if value != expected:
            raise AssetError("vehicle native reference page-info scalar policy differs")
    return {
        "resourceSha256": digest(envelope),
        "header": header,
        "pageInfo": info,
        "flags": flags,
        "regionCounts": counts,
        "nativeCodeExecuted": False,
    }


def readback(envelope: bytes, graph: dict, native: dict) -> tuple[bytes, dict]:
    metadata = graph["vehicleResource"]
    if "pages" in metadata:
        return readback_paged(envelope, graph, native)
    expected_plan = plan(metadata["componentBytes"], native)
    if any(metadata[k] != v for k, v in expected_plan.items()):
        raise AssetError("vehicle retained resource plan differs")
    header, payload = decode_resource(envelope, resource_envelope.MAX_PAYLOAD_BYTES)
    flags = [int(header[k], 0) for k in ("systemFlags", "graphicsFlags")]
    if (
        header["version"] != native["version"]
        or flags != metadata["flags"]
        or header["systemBytes"] != metadata["systemBytes"]
        or header["graphicsBytes"] != 0
    ):
        raise AssetError("vehicle resource envelope version/flags/regions differ")
    # The finite gate consumes an archive record with its resource sign bit set;
    # this is a numeric version check, not resource admission or registration.
    record = struct.pack("<QII", 1 << 63, *flags)
    if not texture_version.model(native["versionGate"]["gate"], native["version"], record):
        raise AssetError("vehicle resource flags fail the extracted version relation")
    relocate(payload, graph, graph["sourceBase"])
    pages = native["pages"]
    raw, _ = resource_pages.model_pages(pages, tuple(flags), bytes(pages["observationBytes"]))
    at, size = metadata["pageInfoOffset"], metadata["pageInfoBytes"]
    spans_in = [
        (b["offset"], b["offset"] + b["bytes"])
        for b in graph["blocks"].values()
        if not (b["offset"] == at and b["bytes"] == size)
    ]
    observed = resource_page_info.serialized_info(payload, graph["sourceBase"], native["info"], pages, raw, spans_in)
    if observed["systemOffset"] != at or observed["observedBytes"] != size:
        raise AssetError("vehicle resource page-info location differs")
    if any(payload[metadata["componentBytes"] : at]) or any(payload[at + size :]):
        raise AssetError("vehicle resource page padding changed")
    original = bytearray(payload[: metadata["componentBytes"]])
    slot = graph["fragment"]["pageInfoPointerOffset"]
    original[slot : slot + 8] = bytes(8)
    if digest(bytes(original)) != metadata["componentSha256"]:
        raise AssetError("vehicle resource changed the composed fragment preimage")
    info_expected = bytearray(size)
    info_expected[native["info"]["systemCount"]] = 1
    if payload[at : at + size] != info_expected:
        raise AssetError("vehicle serialized page-info fields differ")
    return payload, {
        "header": header,
        "payloadSha256": digest(payload),
        "pageInfo": observed,
        "pointerSlots": len(graph["pointerSlots"]),
        "componentPreimageRecovered": True,
        "unresolvedTextureLinks": len(graph["externalTextureLinks"]),
        "completeNativeResource": False,
        "resourceLoaderExecuted": False,
    }


def build(blob: bytes, graph: dict, contract: dict, native: dict, multi_page: bool = False) -> tuple[bytes, dict]:
    if multi_page:
        return build_paged(blob, graph, contract, native)
    if (
        graph.get("resourcePagesResolved")
        or graph["fragment"]["rootOffset"] != 0
        or graph["sourceBase"] != native["rootPlacement"]["sourceBase"]
        or graph["fragment"]["pageInfoPointerOffset"] != native["info"]["rootPointerOffset"]
    ):
        raise AssetError("vehicle page writer needs an unresolved graph at the selected system root")
    layout = plan(len(blob), native)
    arena = ObjectArena(limit=resource_envelope.MAX_PAYLOAD_BYTES)
    if arena.include("fragment", blob, graph) != 0:
        raise AssetError("vehicle fragment root placement moved")
    info = bytearray(layout["pageInfoBytes"])
    info[contract["header"]["fields"]["SystemPagesCount"]["offset"]] = 1
    info_at = arena.add("vehicle-page-info", bytes(info))
    if info_at != layout["pageInfoOffset"]:
        raise AssetError("vehicle page-info placement differs")
    arena.bind(graph["fragment"]["pageInfoPointerOffset"], info_at)
    padding = layout["systemBytes"] - len(arena.data)
    if padding:
        arena.add("vehicle-page-padding", bytes(padding), alignment=1)
    system, placement = arena.render(graph["sourceBase"])
    slots = placement["pointerSlots"]
    fixups = []
    for row in slots:
        at, target = row["offset"], row["targetOffset"]
        extent, alignment = 0, 1
        if target is not None:
            block = one(
                [b for b in placement["blocks"].values() if b["offset"] <= target < b["offset"] + b["bytes"]],
                "vehicle pointer target ownership",
            )
            extent = block["offset"] + block["bytes"] - target
            alignment = math.gcd(block["alignment"], target - block["offset"])
        fixups.append(
            Fixup(at, 0 if target is None else graph["sourceBase"] + target, extent, alignment, system[at : at + 8])
        )
    # Equal serialized/destination bases make this an identity map; the shared
    # checked serializer still enforces every preimage, extent and alignment.
    system = serialize(
        system,
        tuple(r["offset"] for r in slots),
        tuple(fixups),
        (Page(graph["sourceBase"], graph["sourceBase"], layout["systemBytes"]),),
        capacity=native["pages"]["recordLimitForObservation"],
        length_mask=native["lengthMask"],
    )
    result = graph | placement
    result["fragment"] = graph["fragment"] | {
        "targets": graph["fragment"]["targets"] | {"FilePagesInfoPointer": info_at}
    }
    result.update(
        vehicleResource=layout | {"componentSha256": digest(blob)},
        resourcePagesResolved=True,
        completeNativeResource=False,
        partialResource=True,
    )
    envelope = resource_envelope.encode(native["version"], *layout["flags"], system, b"")
    readback(envelope, result, native)
    return envelope, result


LOGICAL_PADDING = "vehicle-logical-padding"


def _paged_arena(blob: bytes, graph: dict, contract: dict, native: dict, count: int) -> tuple[ObjectArena, int]:
    info = native["info"]
    arena = ObjectArena(limit=resource_page_layout.MAX_PAGED_BYTES)
    if arena.include("fragment", blob, graph) != 0:
        raise AssetError("vehicle fragment root placement moved")
    record = bytearray(info["recordOrigin"] + info["packedStride"] * count)
    record[contract["header"]["fields"]["SystemPagesCount"]["offset"]] = count
    info_at = arena.add("vehicle-page-info", bytes(record))
    arena.bind(graph["fragment"]["pageInfoPointerOffset"], info_at)
    # The flat logical image the CPU-emulated loader checks consume is a 4 KiB multiple.
    padding = -len(arena.data) % 4096
    if padding:
        arena.add(LOGICAL_PADDING, bytes(padding), alignment=1)
    return arena, info_at


def plan_paged(blob: bytes, graph: dict, contract: dict, native: dict) -> tuple[ObjectArena, ObjectArena, int, dict]:
    """Deterministic page plan: page size, page list, block map and flags (re-derived by readback)."""
    largest = max(b["bytes"] for b in graph["blocks"].values())
    page = resource_page_layout.page_size_for(largest)
    count = 1
    for _ in range(16):
        arena, info_at = _paged_arena(blob, graph, contract, native, count)
        try:
            paged, remap = arena.paginate(page, frozenset({LOGICAL_PADDING}) & set(arena.blocks))
        except resource_page_layout.PageBudgetError:
            page, count = page * 2, 1  # too many pages of this size: use the next page size
            continue
        if len(paged.pages) == count:
            break
        count = len(paged.pages)
    else:
        raise AssetError("vehicle page plan does not converge")
    limit = native["pages"]["recordLimitForObservation"]
    if count > limit or count > native["info"]["recordLimitForObservation"]:
        raise AssetError("vehicle page count exceeds the native page-record observation")
    version = native["version"]
    flags = [resource_page_layout.flags_for_pages(paged.pages) | (version >> 4) << 28, (version & 15) << 28]
    if resource_pages.pc_pages(flags[0]) != paged.pages or resource_pages.pc_pages(flags[1]):
        raise AssetError("vehicle page flag inverse disagrees")
    ordered = sorted((b for n, b in arena.blocks.items() if n != LOGICAL_PADDING), key=lambda b: b["offset"])
    layout = {
        "componentBytes": len(blob),
        "pageInfoOffset": info_at,
        "pageInfoBytes": native["info"]["recordOrigin"] + native["info"]["packedStride"] * count,
        "logicalBytes": len(arena.data),
        "systemBytes": sum(paged.pages),
        "graphicsBytes": 0,
        "flags": flags,
        "version": version,
        "pageBytes": page,
        "pages": paged.pages,
        "pageMap": [[b["offset"], remap(b["offset"]), b["bytes"]] for b in ordered],
        "policy": "Retail-like system pages of one size plus a smaller tail; no object crosses a page; "
        "no graphics pages",
    }
    return arena, paged, info_at, layout


def build_paged(blob: bytes, graph: dict, contract: dict, native: dict) -> tuple[bytes, dict]:
    if (
        graph.get("resourcePagesResolved")
        or graph["fragment"]["rootOffset"] != 0
        or graph["sourceBase"] != native["rootPlacement"]["sourceBase"]
        or graph["fragment"]["pageInfoPointerOffset"] != native["info"]["rootPointerOffset"]
    ):
        raise AssetError("vehicle page writer needs an unresolved graph at the selected system root")
    arena, paged, info_at, layout = plan_paged(blob, graph, contract, native)
    base = graph["sourceBase"]
    _, logical_placement = arena.render(base)
    system, placement = paged.render(base)
    fixups = []
    for row in placement["pointerSlots"]:
        at, target = row["offset"], row["targetOffset"]
        extent, alignment = 0, 1
        if target is not None:
            block = one(
                [b for b in placement["blocks"].values() if b["offset"] <= target < b["offset"] + b["bytes"]],
                "vehicle pointer target ownership",
            )
            extent = block["offset"] + block["bytes"] - target
            alignment = math.gcd(block["alignment"], target - block["offset"])
        fixups.append(Fixup(at, 0 if target is None else base + target, extent, alignment, system[at : at + 8]))
    # One Page record per system page: the shared checked serializer rejects any referent that
    # crosses its page and any ambiguous page inverse.
    pages, at = [], 0
    for size in layout["pages"]:
        pages.append(Page(base + at, base + at, size))
        at += size
    system = serialize(
        system,
        tuple(r["offset"] for r in placement["pointerSlots"]),
        tuple(fixups),
        tuple(pages),
        capacity=native["pages"]["recordLimitForObservation"],
        length_mask=native["lengthMask"],
        max_bytes=resource_page_layout.MAX_PAGED_BYTES,
    )
    result = graph | logical_placement
    result["fragment"] = graph["fragment"] | {
        "targets": graph["fragment"]["targets"] | {"FilePagesInfoPointer": info_at}
    }
    result.update(
        vehicleResource=layout | {"componentSha256": digest(blob)},
        resourcePagesResolved=True,
        completeNativeResource=False,
        partialResource=True,
    )
    envelope = resource_envelope.encode(
        native["version"], *layout["flags"], system, b"", max_bytes=resource_page_layout.MAX_PAGED_BYTES
    )
    readback(envelope, result, native)
    return envelope, result


def readback_paged(envelope: bytes, graph: dict, native: dict) -> tuple[bytes, dict]:
    """Decode a paged envelope and rebuild the flat logical image the typed checks consume."""
    metadata = graph["vehicleResource"]
    base = graph["sourceBase"]
    header, payload = decode_resource(envelope, resource_page_layout.MAX_PAGED_BYTES)
    flags = [int(header[k], 0) for k in ("systemFlags", "graphicsFlags")]
    pages = metadata["pages"]
    if (
        header["version"] != native["version"]
        or flags != metadata["flags"]
        or resource_pages.pc_pages(flags[0]) != pages
        or resource_pages.pc_pages(flags[1])
        or header["systemBytes"] != sum(pages)
        or metadata["systemBytes"] != sum(pages)
        or header["graphicsBytes"] != 0
        or resource_page_layout.flags_for_pages(pages) != flags[0] & 0x0FFFFFFF
    ):
        raise AssetError("vehicle paged envelope version/flags/regions differ")
    record = struct.pack("<QII", 1 << 63, *flags)
    if not texture_version.model(native["versionGate"]["gate"], native["version"], record):
        raise AssetError("vehicle resource flags fail the extracted version relation")
    # The block map must be exactly the deterministic first-fit plan of the logical blocks.
    ordered = sorted(
        ((n, b) for n, b in graph["blocks"].items() if n != LOGICAL_PADDING), key=lambda item: item[1]["offset"]
    )
    expected, sizes = resource_page_layout.pack(
        [(name, b["bytes"], b["alignment"]) for name, b in ordered], metadata["pageBytes"]
    )
    rows = metadata["pageMap"]
    if sizes != pages or rows != [[b["offset"], expected[name], b["bytes"]] for name, b in ordered]:
        raise AssetError("vehicle page map differs from the deterministic page plan")
    resource_page_layout.check_blocks([(p, n) for _, p, n in rows], pages)
    starts = [row[0] for row in rows]

    def remap(at: int) -> int:
        index = bisect.bisect_right(starts, at) - 1
        logical, paged, length = rows[index]
        if index < 0 or not at < logical + length:
            raise AssetError("vehicle logical offset is outside every block")
        return paged + at - logical

    # Every byte outside the mapped blocks is page padding and must be zero.
    previous = 0
    for _, paged, length in sorted(rows, key=lambda row: row[1]):
        if any(payload[previous:paged]):
            raise AssetError("vehicle page padding changed")
        previous = paged + length
    if any(payload[previous:]):
        raise AssetError("vehicle page padding changed")
    logical = resource_page_layout.logical_view(payload, graph)
    relocate(logical, graph, base)
    raw, _ = resource_pages.model_pages(native["pages"], tuple(flags), bytes(native["pages"]["observationBytes"]))
    at, size = metadata["pageInfoOffset"], metadata["pageInfoBytes"]
    spans_in = [(p, p + n) for logical_at, p, n in rows if logical_at != at]
    observed = resource_page_info.serialized_info(payload, base, native["info"], native["pages"], raw, spans_in)
    expected_info = (remap(at), size, [len(pages), 0])
    if (observed["systemOffset"], observed["observedBytes"], observed["counts"]) != expected_info:
        raise AssetError("vehicle paged page-info location or counts differ")
    if any(logical[metadata["componentBytes"] : at]) or any(logical[at + size :]) or len(logical) % 4096:
        raise AssetError("vehicle logical image padding changed")
    original = bytearray(logical[: metadata["componentBytes"]])
    slot = graph["fragment"]["pageInfoPointerOffset"]
    original[slot : slot + 8] = bytes(8)
    if digest(bytes(original)) != metadata["componentSha256"]:
        raise AssetError("vehicle resource changed the composed fragment preimage")
    info_expected = bytearray(size)
    info_expected[native["info"]["systemCount"]] = len(pages)
    if logical[at : at + size] != info_expected:
        raise AssetError("vehicle serialized page-info fields differ")
    return logical, {
        "header": header,
        "payloadSha256": digest(payload),
        "logicalSha256": digest(logical),
        "pages": len(pages),
        "pageBytes": metadata["pageBytes"],
        "pageInfo": observed,
        "pointerSlots": len(graph["pointerSlots"]),
        "componentPreimageRecovered": True,
        "noObjectCrossesAPage": True,
        "unresolvedTextureLinks": len(graph["externalTextureLinks"]),
        "completeNativeResource": False,
        "resourceLoaderExecuted": False,
    }


def one(rows, label):
    if len(rows) != 1:
        raise AssetError(f"vehicle {label} is missing or ambiguous")
    return rows[0]


def flags_for(count: int, quantum: int) -> int:
    if count == 0:
        return 0
    matches = [
        (count << shift) | scale
        for shift, width in resource_pages.PC_FIELDS
        for scale in range(16)
        if count < 1 << width and resource_pages.pc_pages((count << shift) | scale) == [quantum] * count
    ]
    if not matches:
        raise AssetError("no bounded synthetic page flags for the requested shape")
    return min(matches)  # Explicit synthetic input choice, subsequently checked by native arithmetic.
