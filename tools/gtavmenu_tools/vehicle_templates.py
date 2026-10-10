"""Vehicle observations derived from verified user-supplied retail templates.

The publication carries format layouts and input identities, never dictionary entries,
texture usage words or fragment objects. Reports are generated from the caller's files.
Texture checks here establish bounded metadata/name access, not GPU pixel decoding.
"""

from __future__ import annotations

import hashlib
import json
from itertools import pairwise
from pathlib import Path

from . import drawable_contracts, retail_templates
from .asset_formats import AssetError, Limits, decode_resource
from .asset_textures import GRAPHICS_BASE, SYSTEM_BASE, ResourceView
from .assets import Budget, read_file
from .hashes import joaat

TARGET_ID = "PPSA04264_01.010.002_DISC"


def digest(value: dict) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    ).hexdigest()


def verified_input(path: Path, cache: str, *, manifest: Path | None = None, budget: Budget | None = None) -> bytes:
    """Read one bounded regular file and require the reviewed template identity."""
    try:
        document, rows = retail_templates.load_manifest(manifest)
    except retail_templates.TemplateError as error:
        raise AssetError(str(error)) from error
    if document.get("target") != retail_templates.DEFAULT_TARGET:
        raise AssetError("vehicle template manifest targets an unsupported build")
    matches = [row for row in rows if row.cache == cache]
    if len(matches) != 1:
        raise AssetError(f"vehicle template has no unique manifest identity: {cache}")
    data = read_file(path, budget or Budget(Limits()))
    expected = matches[0]
    if len(data) != expected.size or hashlib.sha256(data).hexdigest() != expected.sha256:
        raise AssetError(f"vehicle template identity mismatch: {cache}; fetch verified templates again")
    return data


def _dictionary_entries(view: ResourceView, limits: Limits) -> list[tuple[int, int]]:
    """Paired arrays with the original ownership, ordering and alias refusals."""
    pairs = view.dictionary_entries(limits)
    spans = [(0, 60)]
    for pointer_at, count_at, width in ((32, 40, 4), (48, 56, 8)):
        pointer = view.system_struct("<Q", SYSTEM_BASE + pointer_at)[0]
        capacity = view.system_struct("<H", SYSTEM_BASE + count_at + 2)[0]
        if capacity:
            at = view.offset(pointer, capacity * width)
            spans.append((at, at + capacity * width))
    if any(left[1] > right[0] for left, right in pairwise(sorted(spans))):
        raise AssetError("dictionary table/root spans overlap")
    if any(left[0] >= right[0] for left, right in pairwise(pairs)):
        raise AssetError("dictionary keys are not strictly unsigned ascending")
    for _, pointer in pairs:
        at = view.offset(pointer, 128)
        if pointer % 8 or any(at < end and start < at + 8 for start, end in spans):
            raise AssetError("dictionary texture reference overlaps or leaves aligned storage")
    return pairs


def stock_from_bytes(
    blob: bytes, *, dictionary_name: str = "vehshare", limits: Limits | None = None
) -> tuple[dict, dict]:
    """Observe names/storage/usage from a v5 dictionary; callers verify its input hash."""
    limits = limits or Limits()
    header, payload = decode_resource(blob, limits.max_file_bytes)
    if header["version"] != 5:
        raise AssetError("vehicle stock dictionary requires PS5 PTD version 5")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    entries, usages = [], []
    for key, pointer in _dictionary_entries(view, limits):
        name = view.name(view.system_struct("<Q", pointer + 40)[0])
        if joaat(name) != key:
            raise AssetError("stock texture name disagrees with its dictionary hash")
        width, height, depth = view.system_struct("<3H", pointer + 24)
        levels = view.system_struct("<B", pointer + 34)[0]
        if min(width, height, depth) == 0 or not 1 <= levels <= max(width, height).bit_length():
            raise AssetError("stock texture dimensions or mip count are invalid")
        count = view.system_struct("<I", pointer + 8)[0]
        stride = view.system_struct("<H", pointer + 12)[0] & 0xFFF
        size = count * stride
        if not 0 < size < 1 << 32:
            raise AssetError("stock texture storage product is empty or overflows uint32")
        view.offset(view.system_struct("<Q", pointer + 56)[0], size, graphics=True)
        view.offset(view.system_struct("<Q", pointer + 48)[0], 32)
        raw = view.system_struct("<I", pointer + 64)[0]
        entries.append({"key": hex(key), "serializedPointer": hex(pointer), "name": name})
        usages.append({"name": name, "raw": raw, "lowClassBits": raw & 31, "remainingFlagBits": raw >> 5})
    names = [row["name"] for row in entries]
    if len({name.casefold() for name in names}) != len(names):
        raise AssetError("stock dictionary has duplicate case-insensitive names")
    identity = hashlib.sha256(blob).hexdigest()
    stock = {
        "targetId": TARGET_ID,
        "dictionaryName": dictionary_name,
        "resourceSha256": identity,
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "observedVersion": header["version"],
        "entryCount": len(entries),
        "names": names,
        "systemBase": SYSTEM_BASE,
        "entries": entries,
        "provenance": "verified-runtime-template",
    }
    usage = {
        "candidateOffset": 64,
        "fieldBytes": 4,
        "classMask": 31,
        "flagsShift": 5,
        "resourceSha256": identity,
        "entryCount": len(usages),
        "rows": usages,
        "provenance": "verified-runtime-template",
        "nativeFieldMeaningValidated": False,
    }
    return stock | {"observationSha256": digest(stock)}, usage | {"observationSha256": digest(usage)}


def stock_observations(
    path: Path, *, dictionary_name: str = "vehshare", manifest: Path | None = None, budget: Budget | None = None
) -> tuple[dict, dict]:
    blob = verified_input(path, "corpus/vehshare-a.ptd", manifest=manifest, budget=budget)
    return stock_from_bytes(blob, dictionary_name=dictionary_name, limits=budget.limits if budget else None)


def geometry_observation(path: Path, *, manifest: Path | None = None) -> dict:
    """Read the pinned fragment's root drawable and retain actual input provenance."""
    blob = verified_input(path, "corpus/tornado6.pft", manifest=manifest)
    header, payload = decode_resource(blob, 32 << 20)
    if header["version"] != 171:
        raise AssetError("vehicle geometry template requires PS5 fragment version 171")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    material = drawable_contracts.load()["material"]
    field = material["fragment"]["fields"]["drawablePointer"]["offset"]
    pointer = view.system_struct("<Q", SYSTEM_BASE + field)[0]
    if pointer % 16:
        raise AssetError("geometry template drawable is unaligned")
    at = view.offset(pointer, material["drawable"]["fragmentObjectBytes"])
    return {
        "targetId": TARGET_ID,
        "sources": {"nativeFragment": hashlib.sha256(blob).hexdigest()},
        "provenance": "verified-runtime-template",
        "freshFragmentEvidence": {
            "contract": {
                "rootPlacement": {"sourceBase": SYSTEM_BASE, "sourceOffset": 0},
                "link": {"drawableOffset": field},
                "drawableObservationBytes": material["drawable"]["fragmentObjectBytes"],
            },
            "nativeReference": {
                "header": header,
                "payloadSha256": hashlib.sha256(payload).hexdigest(),
                "observation": {
                    "rootSystemOffset": 0,
                    "fieldOffset": field,
                    "serializedPointer": hex(pointer),
                    "drawableSystemOffset": at,
                },
            },
            "pages": {"sourceBases": [SYSTEM_BASE, GRAPHICS_BASE]},
        },
    }
