"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import struct
from collections import Counter
from dataclasses import dataclass

from gtavmenu_tools.asset_formats import AssetError, Limits


def binary32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


@dataclass(frozen=True)
class SkinBinding:
    weights: tuple[int, ...]
    palette_indices: tuple[int, ...]
    bone_indices: tuple[int, ...]

    def canonical_bytes(self) -> bytes:
        """Host evidence encoding: raw weights, raw indices, then four LE u16 bones."""
        return bytes(self.weights + self.palette_indices) + struct.pack("<4H", *self.bone_indices)


def decode_binding(
    weights: bytes, indices: bytes, palette: list[int], skeleton_count: int, contract: dict
) -> SkinBinding:
    if len(weights) != contract["laneCount"] or len(indices) != contract["laneCount"]:
        raise AssetError("skinning requires complete packed weight/index lanes")
    if not 0 < len(palette) <= contract["matrixCapacity"] or not 0 < skeleton_count <= 65536:
        raise AssetError("skinning palette/skeleton count is unsupported")
    if indices[contract["clothLane"]] > contract["clothThreshold"]:
        raise AssetError("skinning cloth sentinel is unsupported; separately scoped cloth conversion is required")
    if sum(weights) != contract["weightDenominator"]:
        raise AssetError(
            "skinning weights do not meet the selected unit-sum policy; source bytes will not be normalized"
        )
    bones = []
    for index in indices:
        if index >= len(palette):
            raise AssetError("skinning palette index out of range, including zero-weight lanes")
        bone = palette[index]
        if not 0 <= bone < skeleton_count:
            raise AssetError("skinning palette maps outside the skeleton")
        bones.append(bone)
    return SkinBinding(tuple(weights), tuple(indices), tuple(bones))


def inspect_stream(raw: memoryview, geometry: dict, skeleton_count: int, contract: dict, limits: Limits) -> dict:
    count, stride = geometry["vertexCount"], geometry["vertexStride"]
    if count <= 0 or stride <= 0 or len(raw) != count * stride or len(raw) > limits.max_total_bytes:
        raise AssetError("skinning stream extent or byte budget disagrees")
    if (
        str(geometry["declarationFlags"]) not in contract["selectedVertexTypes"]
        or int(geometry["declarationTypes"], 16) != contract["declarationTypes"]
    ):
        raise AssetError("skinning vertex declaration has no selected public consumer contract")
    offsets = {}
    for name in ("BlendWeights", "BlendIndices"):
        components = [c for c in geometry["components"] if c["semantic"] == name]
        if len(components) != 1:
            raise AssetError(f"skinning {name} component missing or ambiguous")
        component = components[0]
        if component["type"] != contract["componentType"] or component["bytes"] != contract["componentBytes"]:
            raise AssetError(f"skinning {name} packed component format unsupported")
        if not 0 <= component["offset"] <= stride - component["bytes"]:
            raise AssetError("skinning component leaves vertex stride")
        offsets[name] = component["offset"]
    woff, ioff = offsets["BlendWeights"], offsets["BlendIndices"]
    if abs(woff - ioff) < contract["componentBytes"]:
        raise AssetError("skinning packed components overlap")
    palette = geometry["boneIds"]
    if len(palette) == skeleton_count and palette != list(range(skeleton_count)):
        raise AssetError("skinning equal-length nonidentity palette bypasses public remapping; unsupported")
    if count * contract["laneCount"] > limits.max_total_bytes:
        raise AssetError("skinning lane work budget exceeded")
    digest = hashlib.sha256()
    weights_seen, rigid_bones, active_lanes = Counter(), Counter(), Counter()
    minimum, maximum = contract["weightDenominator"], 0
    for vertex in range(count):
        at = vertex * stride
        weights = bytes(raw[at + woff : at + woff + contract["componentBytes"]])
        indices = bytes(raw[at + ioff : at + ioff + contract["componentBytes"]])
        try:
            binding = decode_binding(weights, indices, palette, skeleton_count, contract)
        except AssetError as exc:
            raise AssetError(f"vertex {vertex}: {exc}") from exc
        digest.update(binding.canonical_bytes())
        weights_seen[binding.weights] += 1
        if len(weights_seen) > limits.max_entries:
            raise AssetError("skinning distinct weight tuple report budget exceeded")
        nonzero = [i for i, weight in enumerate(binding.weights) if weight]
        active_lanes[len(nonzero)] += 1
        if len(nonzero) == 1:
            rigid_bones[binding.bone_indices[nonzero[0]]] += 1
        minimum, maximum = min(minimum, *binding.palette_indices), max(maximum, *binding.palette_indices)
    return {
        "vertexVisits": count,
        "indexLaneVisits": count * contract["laneCount"],
        "weightTuples": [
            {"weights": list(weights), "vertices": occurrences} for weights, occurrences in sorted(weights_seen.items())
        ],
        "nonzeroWeightLaneCounts": [
            {"lanes": n, "vertices": occurrences} for n, occurrences in sorted(active_lanes.items())
        ],
        "rigidBoneVertices": [
            {"boneIndex": bone, "vertices": occurrences} for bone, occurrences in sorted(rigid_bones.items())
        ],
        "minimumPaletteIndex": minimum,
        "maximumPaletteIndex": maximum,
        "canonicalBindingSha256": digest.hexdigest(),
        "canonicalBindingBytes": count * 16,
        "sourceBytesPreserved": True,
        "nativeSkinningValidated": False,
    }
