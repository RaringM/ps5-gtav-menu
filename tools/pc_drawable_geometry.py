"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import struct

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import SYSTEM_BASE, ResourceView

LODS = {"High": "High", "Med": "Medium", "Low": "Low", "VLow": "VeryLow"}


def inspect(payload: bytes, header: dict, material: dict, contract: dict, limits: Limits) -> dict:
    if header["version"] != 162 or header["graphicsBytes"] != 0:
        raise AssetError("geometry binding reader supports only system-only Legacy YFT version 162")
    view = ResourceView(payload, header["systemBytes"], header["graphicsBytes"])
    spans = []

    def claim(pointer, size, alignment, label):
        if not pointer or pointer % alignment:
            raise AssetError(f"geometry {label} pointer is null or unaligned")
        start = view.offset(pointer, size)
        end = start + size
        for a, b, old_label in spans:
            if a < end and start < b and (a, b, old_label) != (start, end, label):
                raise AssetError(f"geometry {label} overlaps {old_label}")
        spans.append((start, end, label))
        return start

    def scalar(at, layout, name):
        field = layout["fields"][name]
        return int.from_bytes(view.system[at + field["offset"] : at + field["offset"] + field["bytes"]], "little")

    drawable_at = material["drawable"]["systemOffset"]
    parent = contract["drawable"]
    claim(SYSTEM_BASE + material["root"]["systemOffset"], contract["fragmentBytes"], 16, "fragment")
    claim(SYSTEM_BASE + drawable_at, contract["fragmentDrawableBytes"], 16, "drawable")
    pointers = {label: scalar(drawable_at, parent, f"DrawableModels{suffix}Pointer") for label, suffix in LODS.items()}
    models_pointer = scalar(drawable_at, parent, "DrawableModelsPointer") or pointers["High"]
    if not models_pointer and any(pointers.values()):
        raise AssetError("geometry LOD pointers exist without the model-block entry pointer")
    pointers["Extra"] = models_pointer if models_pointer != pointers["High"] else 0
    shaders = material["shaders"]
    if [row["index"] for row in shaders] != list(range(len(shaders))):
        raise AssetError("geometry material shader indices are not a complete ordered array")
    total_models, total_geometry = 0, 0
    lods, used = [], set()
    for lod, pointer in pointers.items():
        entry = {"lod": lod, "headerPointer": hex(pointer), "models": []}
        lods.append(entry)
        if not pointer:
            entry["state"] = "absent"
            continue
        at = claim(pointer, contract["listHeader"]["bytes"], 8, "model-list-header")
        array, count, capacity, unknown = struct.unpack_from("<QHHI", view.system, at)
        if count != capacity or capacity > limits.max_entries or bool(array) != bool(capacity):
            raise AssetError(f"geometry {lod} list count/capacity/pointer is unsupported")
        total_models += capacity
        if total_models > limits.max_entries:
            raise AssetError("geometry model occurrence budget exceeded")
        entry.update(state="present", count=count, capacity=capacity, unknown=unknown)
        if not capacity:
            continue
        array_at = claim(array, capacity * 8, 8, "model-pointer-array")
        for index in range(capacity):
            model_pointer = struct.unpack_from("<Q", view.system, array_at + index * 8)[0]
            model_at = claim(model_pointer, contract["model"]["prefixBytes"], 16, "model")
            model = contract["model"]
            counts = [scalar(model_at, model, f"GeometriesCount{i}") for i in (1, 2, 3)]
            if len(set(counts)) != 1 or not 0 < counts[0] <= limits.max_entries:
                raise AssetError("geometry model counts disagree, are empty or exceed the limit")
            n = counts[0]
            total_geometry += n
            if total_geometry > limits.max_entries:
                raise AssetError("geometry occurrence budget exceeded")
            geometry_at = claim(scalar(model_at, model, "GeometriesPointer"), n * 8, 8, "geometry-pointer-array")
            mapping_at = claim(scalar(model_at, model, "ShaderMappingPointer"), n * 2, 2, "shader-mapping")
            record = {
                "index": index,
                "systemOffset": model_at,
                "geometryCount": n,
                "skeletonBinding": scalar(model_at, model, "SkeletonBinding"),
                "geometries": [],
            }
            entry["models"].append(record)
            for gi in range(n):
                sid = struct.unpack_from("<H", view.system, mapping_at + gi * 2)[0]
                if sid >= len(shaders):
                    raise AssetError(f"geometry shader index {sid} leaves the main shader group")
                gp = struct.unpack_from("<Q", view.system, geometry_at + gi * 8)[0]
                go = claim(gp, contract["geometry"]["prefixBytes"], 8, "geometry")
                fields = {
                    name: scalar(go, contract["geometry"], name)
                    for name in (
                        "IndicesCount",
                        "TrianglesCount",
                        "VerticesCount",
                        "VertexStride",
                        "BoneIdsCount",
                        "VertexBufferPointer",
                        "IndexBufferPointer",
                        "VertexDataPointer",
                    )
                }
                used.add(sid)
                record["geometries"].append(
                    {
                        "index": gi,
                        "systemOffset": go,
                        "shaderIndex": sid,
                        "shaderNameHash": shaders[sid]["nameHash"],
                        "declaredFields": fields,
                    }
                )
    return {
        "lods": lods,
        "shaderBindings": [{key: row[key] for key in ("index", "nameHash", "textureParameters")} for row in shaders],
        "modelOccurrences": total_models,
        "geometryOccurrences": total_geometry,
        "usedShaderIndices": sorted(used),
        "unusedShaderIndices": sorted(set(range(len(shaders))) - used),
        "claimedSystemSpans": [{"start": a, "end": b, "kind": label} for a, b, label in sorted(set(spans))],
        "vertexIndexPayloadsValidated": False,
        "nativeGeometryContractValidated": False,
    }
