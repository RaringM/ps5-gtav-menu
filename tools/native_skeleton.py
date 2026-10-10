"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import math
import struct

import pc_skeleton_bindings as pc_skeleton
import pc_transforms_pose as pc_pose
from gtavmenu_tools.asset_formats import AssetError, Limits
from native_resource_builder import ObjectArena

POINTER_FIELDS = (
    "BoneTagsPointer",
    "BonesPointer",
    "TransformationsInvertedPointer",
    "TransformationsPointer",
    "ParentIndicesPointer",
    "ChildIndicesPointer",
)


def encode_fields(layout: dict, values: dict) -> bytes:
    """Serialize named decoded fields; no unclassified retail byte spans."""
    output, occupied = bytearray(layout["bytes"]), set()
    for name, field in layout["fields"].items():
        at, size, value = field["offset"], field["bytes"], values[name]
        offsets = set(range(at, at + size))
        if at < 0 or at + size > len(output) or offsets & occupied:
            raise AssetError("skeleton field coverage overlaps or escapes its object")
        occupied |= offsets
        try:
            if field["type"] in ("Single", "Vector3", "Vector4"):
                if len(value) * 4 != size or not all(math.isfinite(v) for v in value):
                    raise AssetError("skeleton float field is nonfinite or has the wrong width")
                raw = struct.pack("<" + "f" * len(value), *value)
            else:
                raw = value.to_bytes(size, "little", signed=field["type"] == "Int16")
        except (OverflowError, struct.error) as exc:
            raise AssetError(f"skeleton field {name} exceeds its serialized type") from exc
        output[at : at + size] = raw
    if occupied != set(range(len(output))):
        raise AssetError("skeleton object contains uncovered bytes")
    return bytes(output)


def build(skeleton: dict, pose: dict, contract: dict, source_base: int) -> tuple[bytes, dict]:
    if skeleton["rejections"] or pose["rejections"]:
        bones = sorted({row.get("name", "?") for row in pose["rejections"]})
        detail = (
            f" (pose disagrees on bones {', '.join(bones)}: --repair seat-mirror/pose-precision=BONES)" if bones else ""
        )
        raise AssetError(f"rejected skeleton/pose cannot be serialized{detail}")
    layouts, arena = contract["layouts"], ObjectArena()
    original = skeleton["header"]
    count = len(skeleton["bones"])
    if (
        not 0 < count <= Limits().max_entries
        or original["BonesCount"] != count
        or skeleton["boneBlockHeader"]["Count"] != count
        or len(skeleton["parents"]) != count
        or original["ChildIndicesCount"] != len(skeleton["childIndices"])
        or original["BoneTagsCapacity"] > Limits().max_entries
    ):
        raise AssetError("skeleton source counts disagree or exceed the component budget")
    if any(original[name] for name in ("Unknown_8h", "Unknown_48h")):
        raise AssetError("skeleton contains an unsupported extension/page-info pointer")
    header = original | dict.fromkeys(POINTER_FIELDS, 0) | {"VFT": 0, "Unknown_4h": 0}
    root = arena.add("skeleton", encode_fields(layouts["Skeleton"], header))
    block = encode_fields(layouts["SkeletonBonesBlock"], skeleton["boneBlockHeader"])
    block += b"".join(encode_fields(layouts["Bone"], bone | {"NamePointer": 0}) for bone in skeleton["bones"])
    bone_at = arena.add("bones", block) + layouts["SkeletonBonesBlock"]["bytes"]
    targets = {"BonesPointer": bone_at}
    for name, indices in (
        ("ParentIndicesPointer", skeleton["parents"]),
        ("ChildIndicesPointer", skeleton["childIndices"]),
    ):
        targets[name] = arena.add(name, struct.pack("<" + "h" * len(indices), *indices)) if indices else None
    for name in ("TransformationsInvertedPointer", "TransformationsPointer"):
        values = pose["arrays"][name]["matrices"]
        if len(values) != len(skeleton["bones"]) or any(len(row) != 16 for row in values):
            raise AssetError("skeleton matrix count or width disagrees")
        if not all(math.isfinite(v) for row in values for v in row):
            raise AssetError("skeleton matrix contains nonfinite values")
        raw = b"".join(struct.pack("<16f", *row) for row in values)
        if hashlib.sha256(raw).hexdigest() != pose["arrays"][name]["sha256"]:
            raise AssetError("skeleton matrix bytes differ from the verified source array")
        targets[name] = arena.add(name, raw)
    capacity = original["BoneTagsCapacity"]
    targets["BoneTagsPointer"] = arena.add("tag-buckets", bytes(capacity * 8)) if capacity else None
    nodes, bucket_nodes = {}, {}
    for node in skeleton["tagNodes"]:
        index = node["BoneIndex"]
        if index in nodes or not 0 <= node["bucket"] < capacity:
            raise AssetError("skeleton tag index is duplicated or bucket escapes capacity")
        nodes[index] = arena.add(f"tag-{index}", encode_fields(layouts["SkeletonBoneTag"], node | {"NextPointer": 0}))
        bucket_nodes.setdefault(node["bucket"], []).append(index)
    for bucket in range(capacity):
        chain = bucket_nodes.get(bucket, [])
        arena.pointer(targets["BoneTagsPointer"] + bucket * 8, nodes[chain[0]] if chain else None)
        for n, index in enumerate(chain):
            arena.pointer(
                nodes[index] + layouts["SkeletonBoneTag"]["fields"]["NextPointer"]["offset"],
                nodes[chain[n + 1]] if n + 1 < len(chain) else None,
            )
    for index, bone in enumerate(skeleton["bones"]):
        raw = bone["name"].encode("ascii")
        if not raw or len(raw) > 256 or b"\0" in raw:
            raise AssetError("skeleton bone name is not a bounded C string")
        at = arena.add(f"name-{index}", raw + b"\0", 1)
        arena.pointer(
            bone_at + index * layouts["Bone"]["bytes"] + layouts["Bone"]["fields"]["NamePointer"]["offset"], at
        )
    for name in POINTER_FIELDS:
        arena.pointer(root + layouts["Skeleton"]["fields"][name]["offset"], targets[name])
    for name in ("Unknown_8h", "Unknown_48h"):
        arena.pointer(root + layouts["Skeleton"]["fields"][name]["offset"], None)
    data, placement = arena.render(source_base)
    return data, placement | {"rootOffset": root}


def reference_read(
    system: bytes, root: int, source_base: int, skeleton_contract: dict, pose_contract: dict
) -> tuple[dict, dict]:
    """Use the existing independently source-derived readers as a layout oracle.

    The scratch drawable and Legacy selector adapt the reader API only. They
    are not included in output and are not evidence for a native RSC envelope.
    Native applicability is checked separately against reviewed field layouts
    and verified retail templates. The reader is independent of the writer.
    """
    holder = (len(system) + 15) & -16
    payload = system + bytes(holder - len(system)) + struct.pack("<Q", source_base + root)
    header = {"version": 162, "systemBytes": len(payload), "graphicsBytes": 0}
    geometry = {"claimedSystemSpans": [{"start": holder, "end": holder + 8, "kind": "drawable"}], "lods": []}
    mesh = {"claimedSystemSpans": [], "geometries": []}
    drawable = {"drawable": {"fields": {"SkeletonPointer": {"offset": 0, "bytes": 8}}}}
    skel = pc_skeleton.inspect(payload, header, geometry, mesh, drawable, skeleton_contract, Limits())
    pose = pc_pose.inspect(payload, header, skel, mesh, skeleton_contract, pose_contract, Limits())
    return skel, pose


def semantics(skeleton: dict, pose: dict) -> dict:
    """Address-independent comparison retaining every serialized source scalar."""
    omitted = {*POINTER_FIELDS, "VFT", "Unknown_4h"}
    return {
        "header": {k: v for k, v in skeleton["header"].items() if k not in omitted},
        "boneBlockHeader": skeleton["boneBlockHeader"],
        "bones": [{k: v for k, v in bone.items() if k != "NamePointer"} for bone in skeleton["bones"]],
        "parents": skeleton["parents"],
        "childIndices": skeleton["childIndices"],
        "tagNodes": [{k: v for k, v in node.items() if k != "NextPointer"} for node in skeleton["tagNodes"]],
        "matrices": {name: array["sha256"] for name, array in pose["arrays"].items()},
    }
