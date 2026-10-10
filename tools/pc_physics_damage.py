"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import math
import struct
from itertools import pairwise

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import SYSTEM_BASE, ResourceView


def digest(blob: bytes | memoryview) -> str:
    return hashlib.sha256(blob).hexdigest()


def _u8(data: memoryview, offset: int) -> int:
    return data[offset]


def _u16(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: memoryview, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def _f32(data: memoryview, offset: int) -> float:
    return struct.unpack_from("<f", data, offset)[0]


class Claims:
    def __init__(self, view: ResourceView):
        self.view = view
        self.rows: list[tuple[int, int, str]] = []

    def system(self, pointer: int, size: int, alignment: int, label: str) -> int:
        if not pointer or pointer % alignment:
            raise AssetError(f"physics/damage {label} pointer is null or unaligned")
        offset = self.view.offset(pointer, size)
        self.rows.append((offset, offset + size, label))
        return offset

    def finish(self) -> list[dict]:
        rows = sorted(self.rows)
        overlap = next(((left, right) for left, right in pairwise(rows) if left[1] > right[0]), None)
        if overlap is not None:
            raise AssetError(f"physics/damage selected system spans overlap: {overlap[0][2]} / {overlap[1][2]}")
        return [{"start": start, "stop": stop, "bytes": stop - start, "label": label} for start, stop, label in rows]


def _pointer_array(system: memoryview, claims: Claims, pointer: int, count: int, label: str) -> list[int]:
    if count == 0:
        if pointer:
            raise AssetError(f"physics/damage {label} pointer is present with zero count")
        return []
    offset = claims.system(pointer, count * 8, 8, label + " pointer array")
    values = list(struct.unpack_from("<" + "Q" * count, system, offset))
    if any(not value for value in values) or len(set(values)) != len(values):
        raise AssetError(f"physics/damage {label} pointers are null or duplicated")
    return values


def _finite(values: list[float], label: str) -> None:
    if any(not math.isfinite(value) for value in values):
        raise AssetError(f"physics/damage {label} is not finite")


def inspect_payload(payload: bytes, header: dict, contract: dict, limits: Limits) -> dict:
    if header.get("version") != 162 or header.get("graphicsBytes") != 0:
        raise AssetError("physics/damage requires a system-only Legacy YFT version 162")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    system = view.system
    claims = Claims(view)
    fragment_bytes = contract["classes"]["FragType"]["bytes"]
    claims.system(SYSTEM_BASE, fragment_bytes, 16, "fragment root")
    physics_pointer = _u64(system, contract["layout"]["fragmentPhysicsLodGroupPointer"])
    lod_group_bytes = contract["classes"]["FragPhysicsLODGroup"]["bytes"]
    lod_group_offset = claims.system(physics_pointer, lod_group_bytes, 16, "physics LOD group")
    lod_pointers = [_u64(system, lod_group_offset + offset) for offset in contract["layout"]["lodGroupPointers"]]
    if not lod_pointers[0] or len({pointer for pointer in lod_pointers if pointer}) != sum(
        bool(p) for p in lod_pointers
    ):
        raise AssetError("physics/damage LOD pointers are missing or duplicated")

    lods = []
    total_groups = 0
    total_children = 0
    for lod_index, lod_pointer in enumerate(lod_pointers, 1):
        if not lod_pointer:
            lods.append(None)
            continue
        lod_bytes = contract["classes"]["FragPhysicsLOD"]["bytes"]
        lod_offset = claims.system(lod_pointer, lod_bytes, 16, f"physics LOD{lod_index}")
        layout = contract["layout"]["lod"]
        groups_count = _u8(system, lod_offset + layout["groupsCount"])
        root_groups_count = _u8(system, lod_offset + layout["rootGroupsCount"])
        children_count = _u8(system, lod_offset + layout["childrenCount"])
        children_count2 = _u8(system, lod_offset + layout["childrenCount2"])
        if (
            groups_count > limits.max_entries
            or children_count > limits.max_entries
            or children_count != children_count2
            or root_groups_count > groups_count
        ):
            raise AssetError("physics/damage LOD counts disagree or exceed bounds")
        total_groups += groups_count
        total_children += children_count
        if total_groups + total_children > limits.max_entries:
            raise AssetError("physics/damage cumulative group/child count exceeds bound")
        group_pointers = _pointer_array(
            system, claims, _u64(system, lod_offset + layout["groupsPointer"]), groups_count, f"LOD{lod_index} groups"
        )
        child_pointers = _pointer_array(
            system,
            claims,
            _u64(system, lod_offset + layout["childrenPointer"]),
            children_count,
            f"LOD{lod_index} children",
        )

        groups = []
        group_layout = contract["layout"]["group"]
        group_class_bytes = contract["classes"]["FragPhysTypeGroup"]["bytes"]
        group_bytes = group_layout["selectedObservationBytes"]
        group_raw = []
        for index, pointer in enumerate(group_pointers):
            offset = claims.system(pointer, group_bytes, 16, f"LOD{lod_index} group {index}")
            raw = system[offset : offset + group_bytes]
            floats = {
                "strength": _f32(system, offset + group_layout["strength"]),
                "mass": _f32(system, offset + group_layout["mass"]),
                "minDamageForce": _f32(system, offset + group_layout["minDamageForce"]),
                "damageHealth": _f32(system, offset + group_layout["damageHealth"]),
            }
            _finite(list(floats.values()), "group damage fields")
            row = {
                "index": index,
                "pointer": hex(pointer),
                "bytes": group_bytes,
                "classBytes": group_class_bytes,
                "sha256": digest(raw),
                **floats,
                "childGroupIndex": _u8(system, offset + group_layout["childGroupIndex"]),
                "parentIndex": _u8(system, offset + group_layout["parentIndex"]),
                "childIndex": _u8(system, offset + group_layout["childIndex"]),
                "childCount": _u8(system, offset + group_layout["childCount"]),
                "childGroupCount": _u8(system, offset + group_layout["childGroupCount"]),
                "glassWindowIndex": _u8(system, offset + group_layout["glassWindowIndex"]),
                "glassFlags": _u8(system, offset + group_layout["glassFlags"]),
            }
            if row["parentIndex"] != 0xFF and row["parentIndex"] >= groups_count:
                raise AssetError("physics/damage group parent index exceeds group count")
            groups.append(row)
            group_raw.append(raw)

        children = []
        child_layout = contract["layout"]["child"]
        child_bytes = contract["classes"]["FragPhysTypeChild"]["bytes"]
        for index, pointer in enumerate(child_pointers):
            offset = claims.system(pointer, child_bytes, 16, f"LOD{lod_index} child {index}")
            raw = system[offset : offset + child_bytes]
            pristine = _f32(system, offset + child_layout["pristineMass"])
            damaged = _f32(system, offset + child_layout["damagedMass"])
            _finite([pristine, damaged], "child mass fields")
            group_index = _u16(system, offset + child_layout["groupIndex"])
            if group_index >= groups_count:
                raise AssetError("physics/damage child group index exceeds group count")
            children.append(
                {
                    "index": index,
                    "pointer": hex(pointer),
                    "bytes": child_bytes,
                    "sha256": digest(raw),
                    "pristineMass": pristine,
                    "damagedMass": damaged,
                    "groupIndex": group_index,
                    "boneTag": _u16(system, offset + child_layout["boneTag"]),
                    "drawable1Pointer": hex(_u64(system, offset + child_layout["drawable1Pointer"])),
                    "drawable2Pointer": hex(_u64(system, offset + child_layout["drawable2Pointer"])),
                    "eventSetPointer": hex(_u64(system, offset + child_layout["eventSetPointer"])),
                }
            )

        roots = [row["index"] for row in groups if row["parentIndex"] == 0xFF]
        if len(roots) != root_groups_count:
            raise AssetError("physics/damage root-group count disagrees with parent indices")
        for group in groups:
            child_groups = [row["index"] for row in groups if row["parentIndex"] == group["index"]]
            child_indices = [row["index"] for row in children if row["groupIndex"] == group["index"]]
            expected_group_index = child_groups[0] if child_groups else 0xFF
            expected_child_index = child_indices[0] if child_indices else 0xFF
            if (
                group["childGroupIndex"] != expected_group_index
                or group["childGroupCount"] != len(child_groups)
                or group["childIndex"] != expected_child_index
                or group["childCount"] != len(child_indices)
            ):
                raise AssetError("physics/damage group child ranges disagree with relationships")

        parent_arrays = []
        for key, width, alignment in (
            ("childrenUnknownFloatsPointer", 4, 4),
            ("childrenInertiaTensorsPointer", 16, 16),
            ("childrenUnknownVectorsPointer", 16, 16),
        ):
            pointer = _u64(system, lod_offset + layout[key])
            if children_count:
                offset = claims.system(pointer, children_count * width, alignment, f"LOD{lod_index} {key}")
                parent_arrays.append(
                    {
                        "field": key,
                        "pointer": hex(pointer),
                        "bytes": children_count * width,
                        "sha256": digest(system[offset : offset + children_count * width]),
                    }
                )
            elif pointer:
                raise AssetError("physics/damage parent child-array pointer is present with zero children")

        transform_pointer = _u64(system, lod_offset + layout["fragmentTransformsPointer"])
        transforms = None
        if transform_pointer:
            transform_offset = view.offset(transform_pointer, 32)
            matrix_count = _u32(system, transform_offset + 0x10)
            if matrix_count > limits.max_entries:
                raise AssetError("physics/damage fragment-transform count exceeds bound")
            transform_bytes = 32 + matrix_count * 64
            transform_offset = claims.system(
                transform_pointer, transform_bytes, 16, f"LOD{lod_index} fragment transforms"
            )
            transforms = {
                "pointer": hex(transform_pointer),
                "matrixCount": matrix_count,
                "bytes": transform_bytes,
                "sha256": digest(system[transform_offset : transform_offset + transform_bytes]),
            }

        lod_raw = system[lod_offset : lod_offset + lod_bytes]
        lods.append(
            {
                "index": lod_index,
                "pointer": hex(lod_pointer),
                "bytes": lod_bytes,
                "sha256": digest(lod_raw),
                "groupsCount": groups_count,
                "rootGroupsCount": root_groups_count,
                "childrenCount": children_count,
                "groups": groups,
                "children": children,
                "parentChildArrays": parent_arrays,
                "fragmentTransforms": transforms,
                "opaquePointers": {
                    key: hex(_u64(system, lod_offset + layout[key]))
                    for key in (
                        "groupNamesPointer",
                        "archetype1Pointer",
                        "archetype2Pointer",
                        "boundPointer",
                        "unknownData1Pointer",
                        "unknownData2Pointer",
                    )
                },
                "unknownDataCounts": {
                    "data1": _u8(system, lod_offset + layout["unknownData1Count"]),
                    "data2": _u8(system, lod_offset + layout["unknownData2Count"]),
                },
            }
        )

    return {
        "header": header,
        "fragmentRoot": hex(SYSTEM_BASE),
        "physicsLodGroup": {
            "pointer": hex(physics_pointer),
            "bytes": lod_group_bytes,
            "sha256": digest(system[lod_group_offset : lod_group_offset + lod_group_bytes]),
        },
        "lods": lods,
        "counts": {
            "lods": sum(row is not None for row in lods),
            "groups": total_groups,
            "children": total_children,
        },
        "selectedSpans": claims.finish(),
    }
