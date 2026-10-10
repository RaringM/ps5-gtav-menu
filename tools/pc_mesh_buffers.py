"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import hashlib
import math
import struct

from gtavmenu_tools.asset_formats import AssetError, Limits
from gtavmenu_tools.asset_textures import ResourceView


def inspect_float_stream(raw: memoryview, count: int, stride: int, components: list, contract: dict) -> dict:
    if count <= 0 or stride <= 0 or len(raw) != count * stride:
        raise AssetError("mesh float stream extent disagrees with its vertex count/stride")
    if not any(c["semantic"] == "Position" and c["type"] == "Float3" for c in components):
        raise AssetError("mesh numeric inspection requires a Float3 position component")
    observations, packed = [], []
    for component in components:
        name, kind, offset, size = (component[k] for k in ("semantic", "type", "offset", "bytes"))
        if not 0 <= offset < stride or size <= 0 or offset + size > stride:
            raise AssetError("mesh numeric component leaves its vertex stride")
        if kind in ("Colour", "UByte4") and size == 4:
            packed.append(name)
            continue
        form = contract.get(kind)
        if form is None or form["bytes"] != size:
            raise AssetError(f"mesh numeric inspection does not support component type {kind}")
        decoder = struct.Struct("<" + "f" * form["scalarCount"])
        low, high = [math.inf] * form["scalarCount"], [-math.inf] * form["scalarCount"]
        for vertex in range(count):
            values = decoder.unpack_from(raw, vertex * stride + offset)
            if not all(math.isfinite(v) for v in values):
                raise AssetError(f"mesh {name} contains a non-finite scalar at vertex {vertex}")
            low = [min(a, b) for a, b in zip(low, values, strict=True)]
            high = [max(a, b) for a, b in zip(high, values, strict=True)]
        observations.append(
            {
                "semantic": name,
                "type": kind,
                "scalarValues": count * form["scalarCount"],
                "minimum": low,
                "maximum": high,
            }
        )
    return {
        "finiteFloatComponents": observations,
        "uninterpretedPackedComponents": packed,
        "attributeSemanticsValidated": False,
    }


def inspect(
    payload: bytes, header: dict, geometry: dict, geometry_contract: dict, contract: dict, limits: Limits
) -> dict:
    if header["version"] != 162 or header["graphicsBytes"]:
        raise AssetError("mesh reader supports only system-only Legacy YFT version 162")
    view = ResourceView(payload, header["systemBytes"], 0)
    spans = {(row["start"], row["end"], row["kind"]) for row in geometry["claimedSystemSpans"]}
    charged = 0
    visited = 0

    def claim(pointer, size, alignment, kind):
        nonlocal charged
        if not pointer or pointer % alignment or size <= 0:
            raise AssetError(f"mesh {kind} pointer/size is null, empty or unaligned")
        if size > limits.max_file_bytes:
            raise AssetError("mesh individual buffer exceeds byte budget")
        start = view.offset(pointer, size)
        end = start + size
        record = (start, end, kind)
        for a, b, other in sorted(spans):
            if start < b and a < end and record != (a, b, other):
                raise AssetError(f"mesh {kind} overlaps {other}")
        if record not in spans:
            charged += size
            if charged > limits.max_total_bytes:
                raise AssetError("mesh aggregate buffer byte budget exceeded")
            spans.add(record)
        return start

    def fields(at, layout):
        return {
            name: int.from_bytes(view.system[at + row["offset"] : at + row["offset"] + row["bytes"]], "little")
            for name, row in layout["fields"].items()
        }

    def buffer(pointer, layout, kind):
        at = claim(pointer, layout["bytes"], 8, kind)
        return fields(at, layout)

    def describe(pointer, size, alignment, kind):
        nonlocal visited
        visited += size
        if visited > limits.max_total_bytes:
            raise AssetError("mesh repeated buffer visits exceed the work-byte budget")
        at = claim(pointer, size, alignment, kind)
        raw = view.system[at : at + size]
        return {"systemOffset": at, "bytes": size, "sha256": hashlib.sha256(raw).hexdigest()}, raw

    semantics = {row["value"]: name for name, row in contract["semantics"].items()}
    observations, rejections = [], []
    for lod in geometry["lods"]:
        for model in lod["models"]:
            for item in model["geometries"]:
                if len(observations) >= limits.max_entries:
                    raise AssetError("mesh geometry occurrence budget exceeded")
                g = fields(item["systemOffset"], geometry_contract["geometry"])
                vb = buffer(g["VertexBufferPointer"], contract["vertexBuffer"], "vertex-buffer")
                ib = buffer(g["IndexBufferPointer"], contract["indexBuffer"], "index-buffer")
                decl = buffer(vb["InfoPointer"], contract["declaration"], "vertex-declaration")
                vc, stride, count = vb["VertexCount"], vb["VertexStride"], ib["IndicesCount"]
                if not 0 < vc <= 65535 or not 0 < stride <= 256 or not 0 < count <= limits.max_file_bytes // 2:
                    raise AssetError("mesh counts/stride are empty or exceed selected limits")
                if g["VerticesCount"] not in (0, vc) or (g["VertexStride"], g["IndicesCount"]) != (stride, count):
                    raise AssetError("mesh geometry and buffer counts/stride disagree")
                primitive = contract["triangleElements"]
                topology_agrees = (
                    g["Unknown_62h"] == primitive
                    and count % primitive == 0
                    and g["TrianglesCount"] == count // primitive
                )
                if not topology_agrees:
                    rejections.append(
                        {
                            "code": "triangle-declaration-unqualified",
                            "lod": lod["lod"],
                            "modelIndex": model["index"],
                            "geometryIndex": item["index"],
                            "storedPrimitiveWord": g["Unknown_62h"],
                            "storedTriangleCount": g["TrianglesCount"],
                            "indexCount": count,
                            "reason": "The selected triangle-list declaration does not agree with index triplets; no triangle count or primitive value is replaced",
                        }
                    )
                if not 0 < decl["Flags"] < (1 << 16) or decl["Count"] != decl["Flags"].bit_count():
                    raise AssetError("mesh declaration flags/count are unsupported")
                components, offset = [], 0
                for index in range(16):
                    if not decl["Flags"] & (1 << index):
                        continue
                    type_id = (decl["Types"] >> (index * 4)) & 15
                    component = contract["supportedComponents"].get(str(type_id))
                    if component is None:
                        raise AssetError(f"mesh enabled component {index} has unsupported type {type_id}")
                    components.append(
                        {
                            "semantic": semantics[index],
                            "type": component["name"],
                            "offset": offset,
                            "bytes": component["bytes"],
                        }
                    )
                    offset += component["bytes"]
                if decl["Stride"] != stride or offset != stride:
                    raise AssetError("mesh declaration extent does not equal the declared vertex stride")
                streams = []
                for name in ("DataPointer1", "DataPointer2"):
                    pointer = vb[name]
                    if pointer:
                        record, raw = describe(pointer, vc * stride, 4, "vertex-data")
                        numeric = inspect_float_stream(raw, vc, stride, components, contract["floatReads"])
                        streams.append({"field": name, **record, **numeric})
                selected = vb["DataPointer1"] or vb["DataPointer2"]
                if not selected or g["VertexDataPointer"] != selected:
                    raise AssetError("mesh geometry vertex-data pointer differs from the selected buffer stream")
                indices, raw = describe(ib["IndicesPointer"], count * contract["indexElementBytes"], 2, "index-data")
                minimum, maximum = vc, 0
                for (value,) in struct.iter_unpack("<H", raw):
                    if value >= vc:
                        raise AssetError("mesh index references a vertex outside the declared buffer")
                    minimum, maximum = min(minimum, value), max(maximum, value)
                bone_count, bone_pointer = g["BoneIdsCount"], g["BoneIdsPointer"]
                if bone_count > limits.max_entries or bool(bone_count) != bool(bone_pointer):
                    raise AssetError("mesh bone-ID count/pointer is unsupported")
                bones = []
                if bone_count:
                    _, raw = describe(bone_pointer, bone_count * 2, 2, "bone-ids")
                    bones = [row[0] for row in struct.iter_unpack("<H", raw)]
                observations.append(
                    {
                        "lod": lod["lod"],
                        "modelIndex": model["index"],
                        "geometryIndex": item["index"],
                        "shaderIndex": item["shaderIndex"],
                        "vertexCount": vc,
                        "vertexStride": stride,
                        "storedGeometryVertexCount": g["VerticesCount"],
                        "vertexCountRule": (
                            "reader-derived-from-buffer" if not g["VerticesCount"] else "explicit-count-agreement"
                        ),
                        "indexCount": count,
                        "indexTriplets": count // primitive,
                        "indexRemainder": count % primitive,
                        "storedTriangleCount": g["TrianglesCount"],
                        "storedPrimitiveWord": g["Unknown_62h"],
                        "triangleDeclarationAgrees": topology_agrees,
                        "vertexBufferFlags": vb["Flags"],
                        "declarationFlags": decl["Flags"],
                        "declarationTypes": hex(decl["Types"]),
                        "components": components,
                        "vertexStreams": streams,
                        "selectedVertexStream": "DataPointer1" if vb["DataPointer1"] else "DataPointer2",
                        "indices": indices | {"minimum": minimum, "maximum": maximum},
                        "boneIds": bones,
                    }
                )
    return {
        "geometries": observations,
        "geometryOccurrences": len(observations),
        "claimedMeshBytes": charged,
        "visitedBufferBytes": visited,
        "state": "rejected" if rejections else "selected-structural-checks-passed",
        "rejections": rejections,
        "claimedSystemSpans": [{"start": a, "end": b, "kind": k} for a, b, k in sorted(spans)],
        "pcBufferBoundsAndIndexRangesVerified": True,
        "vertexAttributeValuesValidated": False,
        "nativeGeometryContractValidated": False,
    }
