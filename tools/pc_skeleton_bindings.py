"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import math
import struct

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import ResourceView


def inspect(
    payload: bytes, header: dict, geometry: dict, mesh: dict, geometry_contract: dict, contract: dict, limits: Limits
) -> dict:
    if header["version"] != 162 or header["graphicsBytes"]:
        raise AssetError("skeleton reader supports only system-only Legacy YFT version 162")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    layouts = contract["layouts"]
    spans = [(r["start"], r["end"], r["kind"]) for r in mesh["claimedSystemSpans"]]
    charged = 0
    comparisons = 0
    comparison_limit = limits.max_entries * 128

    def claim(pointer, size, alignment, kind):
        nonlocal charged, comparisons
        if not pointer or pointer % alignment or size <= 0:
            raise AssetError(f"skeleton {kind} pointer/size is null, unaligned or empty")
        at = view.offset(pointer, size)
        for start, end, old_kind in spans:
            comparisons += 1
            if comparisons > comparison_limit:
                raise AssetError("skeleton overlap comparison work budget exceeded")
            if start < at + size and at < end and (start, end, old_kind) != (at, at + size, kind):
                raise AssetError(f"skeleton {kind} overlaps {old_kind}")
        charged += size
        if charged > limits.max_total_bytes:
            raise AssetError("skeleton visited byte budget exceeded")
        spans.append((at, at + size, kind))
        return at

    def fields(at, layout):
        result = {}
        for name, spec in layout["fields"].items():
            raw = view.system[at + spec["offset"] : at + spec["offset"] + spec["bytes"]]
            if spec["type"] in ("Single", "Vector3", "Vector4"):
                values = struct.unpack("<" + "f" * (spec["bytes"] // 4), raw)
                if not all(math.isfinite(value) for value in values):
                    raise AssetError(f"skeleton {name} contains nonfinite floats")
                result[name] = list(values)
            else:
                result[name] = int.from_bytes(raw, "little", signed=spec["type"] == "Int16")
        return result

    drawables = [r for r in geometry["claimedSystemSpans"] if r["kind"] == "drawable"]
    if len(drawables) != 1:
        raise AssetError("skeleton requires one freshly verified main drawable")
    spec = geometry_contract["drawable"]["fields"]["SkeletonPointer"]
    at = drawables[0]["start"] + spec["offset"]
    pointer = int.from_bytes(view.system[at : at + spec["bytes"]], "little")
    model_rows = []
    for lod in geometry["lods"]:
        for model in lod["models"]:
            binding = model["skeletonBinding"]
            model_rows.append(
                {"lod": lod["lod"], "modelIndex": model["index"], "storedBinding": binding}
                | {name: (binding >> rule["shift"]) & rule["mask"] for name, rule in contract["binding"].items()}
            )
    if not pointer:
        return {
            "state": "external-skeleton-unresolved",
            "skeletonPointer": 0,
            "models": model_rows,
            "geometryOccurrences": len(mesh["geometries"]),
            "rejections": [
                "main drawable has no skeleton; geometry bone IDs and model binding require an independently established external owner"
            ],
        }
    at = claim(pointer, layouts["Skeleton"]["bytes"], 8, "skeleton")
    skel = fields(at, layouts["Skeleton"])
    count = skel["BonesCount"]
    if not 0 < count <= limits.max_entries:
        raise AssetError("skeleton bone count is empty or exceeds limit")
    header_bytes, bone_bytes = layouts["SkeletonBonesBlock"]["bytes"], layouts["Bone"]["bytes"]
    bone_at = claim(skel["BonesPointer"] - header_bytes, header_bytes + count * bone_bytes, 8, "skeleton-bones")
    bone_header = fields(bone_at, layouts["SkeletonBonesBlock"])
    if bone_header["Count"] != count:
        raise AssetError("skeleton bone block count differs from skeleton count")
    bones = []
    for index in range(count):
        bone = fields(bone_at + header_bytes + index * bone_bytes, layouts["Bone"])
        name_at = view.offset(bone["NamePointer"], 1)
        name = bytes(view.system[name_at : name_at + 257]).split(b"\0", 1)
        if len(name) != 2:
            raise AssetError("skeleton bone name exceeds bounded terminator search")
        try:
            bone["name"] = name[0].decode("ascii")
        except UnicodeError as exc:
            raise AssetError("skeleton bone name is not ASCII") from exc
        claim(bone["NamePointer"], len(name[0]) + 1, 1, "skeleton-name")
        if not bone["name"] or bone["Index"] != index or bone["Index2"] != index:
            raise AssetError("skeleton bone name/index/index2 is unsupported")
        bones.append(bone)
    if len({b["Tag"] for b in bones}) != count or len({b["name"] for b in bones}) != count:
        raise AssetError("skeleton duplicate bone tags or names are ambiguous")
    parent_at = claim(skel["ParentIndicesPointer"], count * 2, 2, "skeleton-parent-indices")
    parents = list(struct.unpack_from("<" + "h" * count, view.system, parent_at))
    for index, (bone, parent) in enumerate(zip(bones, parents, strict=True)):
        if parent < -1 or parent >= count or parent == index or bone["ParentIndex"] != parent:
            raise AssetError("skeleton parent array/bone relationship is invalid or disagrees")
        sibling = bone["NextSiblingIndex"]
        if sibling < -1 or sibling >= count or sibling == index:
            raise AssetError("skeleton sibling index is invalid")
    # Linear-time parent graph validation; no source order assumptions or recursion.
    done = set()
    for start in range(count):
        chain, current = set(), start
        while current != -1 and current not in done:
            if current in chain:
                raise AssetError("skeleton parent graph contains a cycle")
            chain.add(current)
            current = parents[current]
        done.update(chain)
    child_count = skel["ChildIndicesCount"]
    if child_count > limits.max_entries or child_count % 2 or (child_count and not skel["ChildIndicesPointer"]):
        raise AssetError("skeleton child pair count/pointer is unsupported")
    if not child_count and skel["ChildIndicesPointer"]:
        # The extracted read uses ChildIndicesCount. Retain the nonnull pointer
        # and check its empty range; no bytes or implied child records are read.
        if skel["ChildIndicesPointer"] % 2:
            raise AssetError("skeleton empty child pointer is unaligned")
        view.offset(skel["ChildIndicesPointer"], 0)
    children = []
    if child_count:
        child_at = claim(skel["ChildIndicesPointer"], child_count * 2, 2, "skeleton-child-indices")
        children = list(struct.unpack_from("<" + "h" * child_count, view.system, child_at))
        for child, parent in zip(children[::2], children[1::2], strict=True):
            if not 0 <= child < count or parents[child] != parent:
                raise AssetError("skeleton child pair disagrees with parent graph")
    tags, seen_nodes = [], set()
    capacity = skel["BoneTagsCapacity"]
    if (
        capacity > limits.max_entries
        or skel["BoneTagsCount"] != min(count, capacity)
        or bool(capacity) != bool(skel["BoneTagsPointer"])
    ):
        raise AssetError("skeleton tag count/capacity/pointer is unsupported")
    if capacity:
        tag_at = claim(skel["BoneTagsPointer"], capacity * 8, 8, "skeleton-tag-buckets")
        buckets = struct.unpack_from("<" + "Q" * capacity, view.system, tag_at)
        for bucket, next_pointer in enumerate(buckets):
            while next_pointer:
                if next_pointer in seen_nodes or len(seen_nodes) >= limits.max_entries:
                    raise AssetError("skeleton tag chain cycles, shares nodes or exceeds limit")
                seen_nodes.add(next_pointer)
                node_at = claim(next_pointer, layouts["SkeletonBoneTag"]["bytes"], 8, "skeleton-tag-node")
                node = fields(node_at, layouts["SkeletonBoneTag"])
                index = node["BoneIndex"]
                if index >= count or bones[index]["Tag"] != node["BoneTag"] or node["BoneTag"] % capacity != bucket:
                    raise AssetError("skeleton tag bucket/index/tag relationship disagrees")
                tags.append({"bucket": bucket, **node})
                next_pointer = node["NextPointer"]
        if sorted(row["BoneIndex"] for row in tags) != list(range(count)):
            raise AssetError("skeleton tag chains do not uniquely cover bones")
    elif count > 1:
        raise AssetError("multi-bone skeleton has no tag table")
    for model in model_rows:
        if model["BoneIndex"] >= count or model["HasSkin"] not in (0, 1):
            raise AssetError("skeleton model bone/skin binding is unsupported")
    palettes = []
    for geom in mesh["geometries"]:
        ids = geom["boneIds"]
        if any(index >= count for index in ids):
            raise AssetError("skeleton geometry palette references an out-of-range bone")
        palettes.append(
            {key: geom[key] for key in ("lod", "modelIndex", "geometryIndex")}
            | {
                "boneIds": ids,
                "boneNames": [bones[index]["name"] for index in ids],
                "identityPalette": ids == list(range(count)),
                "publicRendererRemapBranch": len(ids) != count,
            }
        )
    return {
        "state": "selected-skeleton-relationships-verified",
        "skeletonPointer": pointer,
        "header": skel,
        "boneBlockHeader": bone_header,
        "bones": bones,
        "parents": parents,
        "childIndices": children,
        "tagNodes": tags,
        "models": model_rows,
        "palettes": palettes,
        "geometryOccurrences": len(palettes),
        "visitedBytes": charged,
        "overlapComparisons": comparisons,
        "overlapComparisonLimit": comparison_limit,
        "rejections": [],
        "unvisitedReferences": {
            name: skel[name] for name in ("TransformationsPointer", "TransformationsInvertedPointer")
        },
        "nativeSkeletonContractValidated": False,
    }
