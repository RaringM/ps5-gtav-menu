"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import math
import struct

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import ResourceView


def f32(value):
    try:
        result = struct.unpack("<f", struct.pack("<f", value))[0]
    except (OverflowError, struct.error) as exc:
        raise AssetError("transform arithmetic exceeds finite binary32 domain") from exc
    if not math.isfinite(result):
        raise AssetError("transform arithmetic is nonfinite")
    return result


def run_method(method, inputs, contract):
    values = dict(inputs)
    for row in contract["methods"][method]:
        if "target" in row:
            values[row["target"]] = evaluate(row["expression"], values)
        elif row["control"] == "identity":
            values.update({"result." + name: value for name, value in contract["identity"].items()})
        elif row["control"] == "result":
            values.update({"result." + name: values["temp." + name] for name in contract["fieldOrder"]})
        elif row["control"] != "zero":
            raise AssetError("transform unknown arithmetic control")
    if not all("result." + name in values for name in contract["fieldOrder"]):
        raise AssetError("transform arithmetic result is incomplete")
    return [values["result." + name] for name in contract["fieldOrder"]]


def multiply(left, right, contract):
    values = {
        side + "." + name: value
        for side, matrix in (("left", left), ("right", right))
        for name, value in zip(contract["fieldOrder"], matrix, strict=True)
    }
    return run_method("multiply", values, contract)


def local_pose(bone, contract):
    rotation = run_method(
        "rotation", {"rotation." + axis: value for axis, value in zip("XYZW", bone["Rotation"], strict=True)}, contract
    )
    translation = run_method("translation", dict(zip("xyz", bone["Translation"], strict=True)), contract)
    identity = [contract["identity"][name] for name in contract["fieldOrder"]]
    pose = multiply(multiply(identity, rotation, contract), translation, contract)
    for field, scale in zip(contract["scaleFields"], bone["Scale"], strict=True):
        index = contract["fieldOrder"].index(field)
        pose[index] = f32(pose[index] * scale)
    return pose


def compare(actual, expected, fields, policy):
    rows = [
        {
            "field": name,
            "actual": a,
            "expected": e,
            "absoluteError": abs(a - e),
            "allowedError": policy["absoluteTolerance"] + policy["relativeTolerance"] * abs(e),
        }
        for name, a, e in zip(fields, actual, expected, strict=True)
    ]
    return {
        "maximumAbsoluteError": max(row["absoluteError"] for row in rows),
        "mismatches": [row for row in rows if row["absoluteError"] > row["allowedError"]],
    }


def inspect(payload, header, skeleton, mesh, skeleton_contract, contract, limits: Limits):
    if (
        header["version"] != 162
        or header["graphicsBytes"]
        or skeleton["state"] != "selected-skeleton-relationships-verified"
    ):
        raise AssetError("transform reader requires selected system-only Legacy skeletons")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    count, mc = len(skeleton["bones"]), contract["matrix"]
    if not 0 < count <= limits.max_entries or count * mc["bytes"] * 2 > limits.max_total_bytes:
        raise AssetError("transform array count/work budget exceeded")
    spans = [(r["start"], r["end"], r["kind"]) for r in mesh["claimedSystemSpans"]]
    layouts, skel = skeleton_contract["layouts"], skeleton["header"]

    def protect(pointer, size, kind):
        if size:
            at = view.offset(pointer, size)
            spans.append((at, at + size, kind))

    protect(skeleton["skeletonPointer"], layouts["Skeleton"]["bytes"], "skeleton")
    prefix = layouts["SkeletonBonesBlock"]["bytes"]
    protect(skel["BonesPointer"] - prefix, prefix + count * layouts["Bone"]["bytes"], "bones")
    protect(skel["ParentIndicesPointer"], count * 2, "parent indices")
    protect(skel["ChildIndicesPointer"], skel["ChildIndicesCount"] * 2, "child indices")
    for bone in skeleton["bones"]:
        protect(bone["NamePointer"], len(bone["name"].encode("ascii")) + 1, "bone name")
    pointer_width = layouts["SkeletonBoneTag"]["fields"]["NextPointer"]["bytes"]
    protect(skel["BoneTagsPointer"], skel["BoneTagsCapacity"] * pointer_width, "tag buckets")
    previous_bucket, next_pointer = None, 0
    for node in skeleton["tagNodes"]:
        if node["bucket"] != previous_bucket:
            at = view.offset(skel["BoneTagsPointer"] + node["bucket"] * pointer_width, pointer_width)
            next_pointer = int.from_bytes(view.system[at : at + pointer_width], "little")
            previous_bucket = node["bucket"]
        protect(next_pointer, layouts["SkeletonBoneTag"]["bytes"], "tag node")
        next_pointer = node["NextPointer"]
    arrays = {}
    for field in ("TransformationsPointer", "TransformationsInvertedPointer"):
        pointer, size = skel[field], count * mc["bytes"]
        if not pointer or pointer % mc["pack"]:
            raise AssetError(f"transform {field} is absent or unaligned; no synthesized fallback is allowed")
        at = view.offset(pointer, size)
        for start, end, kind in spans:
            if start < at + size and at < end:
                raise AssetError(f"transform {field} overlaps {kind}")
        protect(pointer, size, field)
        raw = view.system[at : at + size]
        values = [list(row) for row in struct.iter_unpack("<" + "f" * len(mc["fieldOrder"]), raw)]
        if not all(math.isfinite(value) for row in values for value in row):
            raise AssetError(f"transform {field} contains nonfinite matrix values")
        arrays[field] = {
            "pointer": pointer,
            "systemOffset": at,
            "bytes": size,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "matrices": values,
        }
    columns = [mc["fieldOrder"].index(name) for name in mc["column4"]]
    regular = [index for index in range(len(mc["fieldOrder"])) if index not in columns]
    identity = [mc["identity"][name] for name in mc["fieldOrder"]]
    results, globals_, rejections = [], [], []
    for index, bone in enumerate(skeleton["bones"]):
        parent = skeleton["parents"][index]
        if parent < -1 or parent >= index:
            raise AssetError("transform public read-order replay requires parent-before-child; source is not reordered")
        local = local_pose(bone, mc)
        global_pose = local if parent < 0 else multiply(local, globals_[parent], mc)
        globals_.append(global_pose)
        stored_local = arrays["TransformationsPointer"]["matrices"][index]
        stored_inverse = arrays["TransformationsInvertedPointer"]["matrices"][index]
        local_check = compare(
            [stored_local[j] for j in regular],
            [local[j] for j in regular],
            [mc["fieldOrder"][j] for j in regular],
            contract["comparisonPolicy"],
        )
        consumer_inverse = list(stored_inverse)
        consumer_inverse[mc["fieldOrder"].index("M44")] = (
            1.0  # Exact public consumer assignment; stored bytes stay intact.
        )
        skin_pose = multiply(consumer_inverse, global_pose, mc)
        inverse_check = compare(skin_pose, identity, mc["fieldOrder"], contract["comparisonPolicy"])
        if local_check["mismatches"] or inverse_check["mismatches"]:
            rejections.append(
                {"boneIndex": index, "name": bone["name"], "reason": "selected-pose-comparison-disagreement"}
            )
        results.append(
            {
                "boneIndex": index,
                "name": bone["name"],
                "localPose": local,
                "globalPose": global_pose,
                "storedLocalColumn4": [stored_local[j] for j in columns],
                "storedInverseColumn4": [stored_inverse[j] for j in columns],
                "localComparison": local_check,
                "inverseBindTimesGlobalComparison": inverse_check,
            }
        )
    return {
        "arrays": arrays,
        "bones": results,
        "rejections": rejections,
        "state": "rejected" if rejections else "selected-pose-comparisons-agree",
        "matrixVisits": count * 2,
        "scalarVisits": count * 2 * len(mc["fieldOrder"]),
        "maximumLocalAbsoluteError": max(row["localComparison"]["maximumAbsoluteError"] for row in results),
        "maximumInverseBindAbsoluteError": max(
            row["inverseBindTimesGlobalComparison"]["maximumAbsoluteError"] for row in results
        ),
        "sourceMatricesPreserved": True,
        "nativePoseValidated": False,
    }


def evaluate(expression, values):
    """Evaluate reviewed typed formulas with the original binary32 operation order."""

    def visit(node, depth=0):
        if depth > 32 or not isinstance(node, list) or len(node) not in (2, 3):
            raise AssetError("transform expression exceeds the typed arithmetic profile")
        op = node[0]
        if op == "number" and len(node) == 2 and type(node[1]) in (int, float):
            return f32(node[1])
        if op == "name" and len(node) == 2 and isinstance(node[1], str) and node[1] in values:
            return values[node[1]]
        if op in ("add", "sub", "mul") and len(node) == 3:
            left, right = visit(node[1], depth + 1), visit(node[2], depth + 1)
            if op == "add":
                return f32(left + right)
            if op == "sub":
                return f32(left - right)
            return f32(left * right)
        if op in ("negative", "positive") and len(node) == 2:
            value = visit(node[1], depth + 1)
            return f32(-value) if op == "negative" else value
        raise AssetError("transform arithmetic has an unknown operand")

    return visit(expression)
