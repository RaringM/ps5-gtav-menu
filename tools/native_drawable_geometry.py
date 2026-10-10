"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import bisect
import hashlib
import struct

from gtavmenu_tools.asset_formats import AssetError

MAX_MODELS = 1024


MAX_GEOMETRIES = 16384


MAX_STORAGE = 32 * 1024 * 1024


class Spans:
    def __init__(self, system: bytes):
        self.system = system
        self.starts = []
        self.rows = []

    def claim(self, start: int, length: int, name: str):
        if (
            type(start) is not int
            or type(length) is not int
            or start < 0
            or length <= 0
            or start > len(self.system) - length
        ):
            raise AssetError(f"native geometry {name} leaves bounded system storage")
        at = bisect.bisect_left(self.starts, start)
        if (at and self.rows[at - 1][1] > start) or (at < len(self.rows) and self.rows[at][0] < start + length):
            raise AssetError(
                f"native geometry {name} overlaps another selected span; shared/aliased structures unsupported"
            )
        self.starts.insert(at, start)
        self.rows.insert(at, (start, start + length, name))


def read_geometry(system: bytes, base: int, drawable_offset: int, fragment_observation: dict, contract: dict) -> dict:
    if (
        type(system) is not bytes
        or not 1 <= len(system) <= MAX_STORAGE
        or type(base) is not int
        or not 0 < base < 1 << 64
        or base + len(system) >= 1 << 64
    ):
        raise AssetError("native geometry requires immutable bounded system storage and uint64 base")
    if type(drawable_offset) is not int or drawable_offset != fragment_observation["drawableSystemOffset"]:
        raise AssetError("native geometry drawable differs from freshly read fragment child")
    spans = Spans(system)
    spans.claim(0, fragment_observation["fieldOffset"] + 8, "selected fragment root")
    slots, geometry, leaf = contract["slots"], contract["geometryArray"], contract["geometryPrefix"]
    extent = max(
        fragment_observation["drawableObservationBytes"], max(row["drawableOffset"] + 8 for row in slots["slots"])
    )
    spans.claim(drawable_offset, extent, "selected drawable prefix")

    def pointer(at, length, name):
        if not 0 <= at <= len(system) - 8:
            raise AssetError(f"native geometry {name} pointer field leaves storage")
        value = struct.unpack_from("<Q", system, at)[0]
        offset = value - base
        if not value or value % 8:
            raise AssetError(f"native geometry {name} is null or unaligned; nullable elements unsupported")
        spans.claim(offset, length, name)
        return offset

    models, geometries, observations = 0, 0, []
    for index, slot in enumerate(slots["slots"]):
        field = drawable_offset + slot["drawableOffset"]
        value = struct.unpack_from("<Q", system, field)[0]
        if not value:
            observations.append(
                {"slot": index, "drawableFieldOffset": slot["drawableOffset"], "present": False, "models": []}
            )
            continue
        header = pointer(field, slot["countOffset"] + 2, f"slot {index} array header")
        count = struct.unpack_from("<H", system, header + slot["countOffset"])[0]
        if count == 0:
            raise AssetError(f"native geometry slot {index} nonnull empty array is unsupported")
        models += count
        if models > MAX_MODELS:
            raise AssetError("native geometry model traversal exceeds host work budget")
        array = pointer(
            header + slots["arrayPointerOffset"], count * slot["stride"], f"slot {index} model pointer array"
        )
        rows = []
        for model_index in range(count):
            model_bytes = max(
                geometry["pointerOffset"] + 8, geometry["countOffset"] + 2, geometry["consumerCountOffset"] + 2
            )
            model = pointer(array + model_index * slot["stride"], model_bytes, f"slot {index} model {model_index}")
            counts = [
                struct.unpack_from("<H", system, model + geometry[key])[0]
                for key in ("countOffset", "consumerCountOffset")
            ]
            if not counts[0] or counts[0] != counts[1]:
                raise AssetError(
                    f"native geometry slot {index} model {model_index} reader/consumer counts disagree or are zero: {counts}"
                )
            geometries += counts[0]
            if geometries > MAX_GEOMETRIES:
                raise AssetError("native geometry leaf traversal exceeds host work budget")
            table = pointer(
                model + geometry["pointerOffset"],
                counts[0] * geometry["stride"],
                f"slot {index} geometry pointer array",
            )
            leaves = []
            for geometry_index in range(counts[0]):
                at = pointer(
                    table + geometry_index * geometry["stride"],
                    leaf["observationBytes"],
                    f"slot {index} geometry {geometry_index}",
                )
                state = struct.unpack_from("<I", system, at + leaf["stateOffset"])[0]
                if state:
                    raise AssetError(
                        f"native geometry slot {index} geometry {geometry_index} is preinitialized; selected serialized construction path requires state zero"
                    )
                leaves.append(
                    {
                        "index": geometry_index,
                        "systemOffset": at,
                        "observedBytes": leaf["observationBytes"],
                        "serializedClassWord": hex(struct.unpack_from("<Q", system, at)[0]),
                        "state": state,
                        "prefixSha256": hashlib.sha256(system[at : at + leaf["observationBytes"]]).hexdigest(),
                    }
                )
            rows.append(
                {
                    "index": model_index,
                    "systemOffset": model,
                    "observedBytes": model_bytes,
                    "readerGeometryCount": counts[0],
                    "consumerGeometryCount": counts[1],
                    "prefixSha256": hashlib.sha256(system[model : model + model_bytes]).hexdigest(),
                    "geometries": leaves,
                }
            )
        observations.append(
            {
                "slot": index,
                "drawableFieldOffset": slot["drawableOffset"],
                "present": True,
                "headerSystemOffset": header,
                "modelCount": count,
                "models": rows,
            }
        )
    return {
        "slots": observations,
        "modelCount": models,
        "geometryCount": geometries,
        "selectedSpanCount": len(spans.rows),
        "hostLimits": {"models": MAX_MODELS, "geometries": MAX_GEOMETRIES, "systemBytes": MAX_STORAGE},
        "sourceUnmodified": True,
        "scope": "Disjoint selected prefix/array storage only; sizes are observation extents, not whole object sizes",
    }
