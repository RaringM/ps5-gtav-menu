"""Gen9 (PS5) mesh components: declaration, vertex/index buffers and views, written and read back.

The build/read half of native_mesh.py, split from the derivation of its contract: the layouts arrive as
data (`contract`: the `mesh` entry of the drawable contracts, frozen in data/drawable_contracts and
loaded by gtavmenu_tools.drawable_contracts; devtools/generators/generate_drawable_contracts.py rebuilds
that file from its pinned sources). Nothing here reads a reference source, a game executable or a
third-party package.
"""

from __future__ import annotations

import hashlib
import itertools
import struct

from gtavmenu_tools.asset_formats import AssetError
from native_resource_builder import ObjectArena

WIDTHS = {"UInt16": 2, "UInt32": 4, "UInt64": 8}


PACK = {2: "<H", 4: "<I", 8: "<Q"}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def put(data: bytearray, layout: dict, name: str, value: int):
    field = layout["fields"][name]
    if type(value) is not int or not 0 <= value < 1 << (8 * field["bytes"]):
        raise AssetError(f"mesh field {name} would truncate")
    struct.pack_into(PACK[field["bytes"]], data, field["offset"], value)


def get(data: bytes, at: int, layout: dict) -> dict:
    if at < 0 or at + layout["bytes"] > len(data):
        raise AssetError("mesh object exceeds storage")
    return {
        name: struct.unpack_from(PACK[row["bytes"]], data, at + row["offset"])[0]
        for name, row in layout["fields"].items()
    }


def declaration(components: list, stride: int, contract: dict) -> tuple[bytes, list]:
    layout = contract["layouts"]["declaration"]
    if not 0 < stride <= layout["strideMask"]:
        raise AssetError("mesh vertex stride exceeds declaration field")
    source = {row["semantic"]: row for row in components}
    if len(source) != len(components):
        raise AssetError("mesh has duplicate source semantics")
    data, offset, copies, represented = bytearray(layout["bytes"]), 0, [], set()
    for slot in range(layout["count"]):
        struct.pack_into("<I", data, layout["offsets"] + slot * 4, offset)
        spec = contract["components"].get(str(slot))
        row = source.get(spec["semantic"]) if spec else None
        if row is None:
            continue
        if (
            row["type"] != spec["type"]
            or row["bytes"] != spec["bytes"]
            or row["offset"] < 0
            or row["offset"] + row["bytes"] > stride
        ):
            raise AssetError("mesh source component is outside the supported GTAV1 profile")
        data[layout["sizes"] + slot] = stride
        data[layout["types"] + slot] = spec["format"]
        copies.append(
            {
                "semantic": spec["semantic"],
                "sourceOffset": row["offset"],
                "targetOffset": offset,
                "bytes": spec["bytes"],
                "slot": slot,
                "format": spec["format"],
            }
        )
        offset += spec["bytes"]
        represented.add(spec["semantic"])
    if represented != source.keys() or offset != stride:
        raise AssetError("mesh declaration would omit an attribute or stride bytes")
    spans = sorted((row["sourceOffset"], row["sourceOffset"] + row["bytes"]) for row in copies)
    if spans[0][0] != 0 or spans[-1][1] != stride or any(a[1] != b[0] for a, b in itertools.pairwise(spans)):
        raise AssetError("mesh source attributes overlap or leave gaps")
    struct.pack_into("<Q", data, layout["data"], stride << layout["strideShift"])
    return bytes(data), copies


def inverse_vertices(raw: bytes, declaration_bytes: bytes, stride: int, contract: dict) -> tuple[bytes, list]:
    """Independent public Gen9 reader: decode fields, reconstruct legacy order."""
    layout = contract["layouts"]["declaration"]
    if len(declaration_bytes) != layout["bytes"] or not stride or len(raw) % stride:
        raise AssetError("mesh inverse input extent mismatch")
    mode = struct.unpack_from("<Q", declaration_bytes, layout["data"])[0]
    if mode != stride << layout["strideShift"]:
        raise AssetError("mesh declaration mode/count/stride is outside the supported profile")
    attrs = []
    for slot in range(layout["count"]):
        code = declaration_bytes[layout["types"] + slot]
        size = declaration_bytes[layout["sizes"] + slot]
        offset = struct.unpack_from("<I", declaration_bytes, layout["offsets"] + slot * 4)[0]
        if not code:
            if size:
                raise AssetError("inactive mesh attribute has a nonzero step")
            continue
        spec = contract["components"].get(str(slot))
        if spec is None or code != spec["format"] or size != stride or offset + spec["bytes"] > stride:
            raise AssetError("mesh inverse attribute format/step/span mismatch")
        attrs.append(spec | {"offset": offset})
    spans = sorted((row["offset"], row["offset"] + row["bytes"]) for row in attrs)
    if (
        not spans
        or spans[0][0] != 0
        or spans[-1][1] != stride
        or any(a[1] != b[0] for a, b in itertools.pairwise(spans))
    ):
        raise AssetError("mesh inverse attributes overlap or leave gaps")
    output, legacy_components, destination = bytearray(len(raw)), [], 0
    for row in sorted(attrs, key=lambda row: row["legacyIndex"]):
        legacy_components.append(
            {"semantic": row["semantic"], "type": row["type"], "bytes": row["bytes"], "offset": destination}
        )
        for vertex in range(len(raw) // stride):
            start = vertex * stride + row["offset"]
            end = vertex * stride + destination
            output[end : end + row["bytes"]] = raw[start : start + row["bytes"]]
        destination += row["bytes"]
    return bytes(output), legacy_components


def view_bytes(contract: dict) -> bytes:
    layout = contract["layouts"]["view"]
    data = bytearray(layout["bytes"])
    for name, value in contract["defaults"]["view"].items():
        put(data, layout, name, value)
    return bytes(data)


def full_headers(system: bytes, base: int, observed: list, contract: dict, retail: bool):
    """Check entire buffer/view headers, including the finite reserved-zero profile."""
    layouts = contract["layouts"]
    for row in observed:
        cls = ("VertexBuffer", "IndexBuffer")[row["group"]]
        values = get(system, row["systemOffset"], layouts[cls])
        expected = dict.fromkeys(values, 0)
        dynamic = (
            ("VertexCount", "VertexStride", "DataPointer1", "InfoPointer")
            if row["group"] == 0
            else ("IndicesCount", "G9_IndexSize", "IndicesPointer")
        )
        for name in dynamic:
            expected[name] = values[name]
        if retail:
            for name in ("VFT", "Unknown_4h"):
                expected[name] = values[name]
        expected["G9_BindFlags"] = contract["defaults"][cls]
        expected["G9_SRVPointer"] = values["G9_SRVPointer"]
        if values != expected:
            raise AssetError("native full buffer header differs from finite Gen9 profile")
        offset = values["G9_SRVPointer"] - base
        fields = get(system, offset, layouts["view"])
        expected = dict.fromkeys(fields, 0) | contract["defaults"]["view"]
        if retail:
            expected["VFT"] = fields["VFT"]
        if fields != expected:
            raise AssetError("native full buffer view differs from public Gen9 profile")


def source_bytes(payload: bytes, row: dict) -> bytes:
    start, size = row["systemOffset"], row["bytes"]
    if start < 0 or size <= 0 or start + size > len(payload):
        raise AssetError("mesh source span exceeds storage")
    raw = payload[start : start + size]
    if digest(raw) != row["sha256"]:
        raise AssetError("mesh source span changed after decoding")
    return raw


def build(payload: bytes, groups: list, contract: dict, base: int) -> tuple[bytes, dict]:
    arena, rows = ObjectArena(), []
    layouts = contract["layouts"]
    for group in groups:
        geometry_rows = {
            (lod["lod"], model["index"], geo["index"]): geo
            for lod in group["geometry"]["lods"]
            for model in lod["models"]
            for geo in model["geometries"]
        }
        mesh_keys = [
            tuple(mesh[k] for k in ("lod", "modelIndex", "geometryIndex")) for mesh in group["mesh"]["geometries"]
        ]
        if len(mesh_keys) != len(set(mesh_keys)) or set(mesh_keys) != geometry_rows.keys():
            raise AssetError("mesh and geometry coverage differ or duplicate an occurrence")
        for mesh in group["mesh"]["geometries"]:
            key = tuple(mesh[k] for k in ("lod", "modelIndex", "geometryIndex"))
            source_geo = geometry_rows[key]
            if (
                mesh["declarationTypes"] != contract["legacyTypes"]
                or mesh["vertexBufferFlags"]
                or not mesh["triangleDeclarationAgrees"]
            ):
                why = (
                    "triangle count/primitive word disagree with the index count (--repair triangle-counts)"
                    if mesh["declarationTypes"] == contract["legacyTypes"] and not mesh["vertexBufferFlags"]
                    else f"declaration types {mesh['declarationTypes']}, vertex buffer flags {mesh['vertexBufferFlags']}"
                )
                raise AssetError(f"mesh source is outside supported declaration/buffer/topology profile: {why}")
            streams = [source_bytes(payload, row) for row in mesh["vertexStreams"]]
            if not streams or any(raw != streams[0] for raw in streams):
                raise AssetError("mesh has missing or differing vertex streams")
            raw, indices = streams[0], source_bytes(payload, mesh["indices"])
            stride, count = mesh["vertexStride"], mesh["vertexCount"]
            if len(raw) != count * stride or len(indices) != mesh["indexCount"] * contract["indexElementBytes"]:
                raise AssetError("mesh source count differs from storage")
            decl, copies = declaration(mesh["components"], stride, contract)
            vertices = bytearray(len(raw))
            for copy in copies:
                for vertex in range(count):
                    src, dst = vertex * stride + copy["sourceOffset"], vertex * stride + copy["targetOffset"]
                    vertices[dst : dst + copy["bytes"]] = raw[src : src + copy["bytes"]]
            if inverse_vertices(bytes(vertices), decl, stride, contract) != (raw, mesh["components"]):
                raise AssetError("mesh inverse conversion differs from every source attribute byte")
            tag = f"geometry-{len(rows)}"
            palette = struct.pack("<" + "H" * len(mesh["boneIds"]), *mesh["boneIds"])
            padding = (
                contract["palettePaddingBytes"] if len(mesh["boneIds"]) > contract["palettePaddingThreshold"] else 0
            )
            geometry_data = bytearray(layouts["geometry"]["bytes"] + padding + len(palette))
            original = get(payload, source_geo["systemOffset"], layouts["geometry"])
            replaced = {
                "VFT",
                "Unknown_4h",
                "VertexBufferPointer",
                "IndexBufferPointer",
                "BoneIdsPointer",
                "VertexDataPointer",
                "VerticesCount",
                "VertexStride",
                "IndicesCount",
                "TrianglesCount",
                "BoneIdsCount",
                "Unknown_62h",
            }
            if any(value for name, value in original.items() if name not in replaced):
                raise AssetError("mesh geometry has unsupported reserved fields or additional buffers")
            for name, value in {
                "VerticesCount": count,
                "VertexStride": stride,
                "IndicesCount": mesh["indexCount"],
                "TrianglesCount": mesh["indexCount"] // contract["triangleElements"],
                "BoneIdsCount": len(mesh["boneIds"]),
                "Unknown_62h": contract["triangleElements"],
            }.items():
                put(geometry_data, layouts["geometry"], name, value)
            if palette:
                geometry_data[-len(palette) :] = palette
            go = arena.add(tag, geometry_data)
            vo = arena.add(tag + ".vertices", vertices)
            io = arena.add(tag + ".indices", indices)
            do = arena.add(tag + ".declaration", decl)
            buffer_offsets = []
            for cls, n, step, data_at, count_name, stride_name, pointer_name in (
                ("VertexBuffer", count, stride, vo, "VertexCount", "VertexStride", "DataPointer1"),
                (
                    "IndexBuffer",
                    mesh["indexCount"],
                    contract["indexElementBytes"],
                    io,
                    "IndicesCount",
                    "G9_IndexSize",
                    "IndicesPointer",
                ),
            ):
                layout = layouts[cls]
                data = bytearray(layout["bytes"])
                for name, value in ((count_name, n), (stride_name, step), ("G9_BindFlags", contract["defaults"][cls])):
                    put(data, layout, name, value)
                bo = arena.add(tag + "." + cls, data)
                so = arena.add(tag + "." + cls + ".view", view_bytes(contract))
                arena.pointer(bo + layout["fields"][pointer_name]["offset"], data_at)
                arena.pointer(bo + layout["fields"]["G9_SRVPointer"]["offset"], so)
                if cls == "VertexBuffer":
                    arena.pointer(bo + layout["fields"]["InfoPointer"]["offset"], do)
                buffer_offsets.append(bo)
            targets = {
                "VertexBufferPointer": buffer_offsets[0],
                "IndexBufferPointer": buffer_offsets[1],
                "VertexDataPointer": vo,
                "BoneIdsPointer": go + layouts["geometry"]["bytes"] + padding if palette else None,
            }
            for name, target in targets.items():
                arena.pointer(go + layouts["geometry"]["fields"][name]["offset"], target)
            rows.append(
                {
                    "systemOffset": go,
                    "owner": group["owner"],
                    "sourceGeometryOffset": source_geo["systemOffset"],
                    **{
                        k: mesh[k]
                        for k in (
                            "lod",
                            "modelIndex",
                            "geometryIndex",
                            "shaderIndex",
                            "vertexCount",
                            "vertexStride",
                            "indexCount",
                            "boneIds",
                        )
                    },
                    "legacyVertexSha256": digest(raw),
                    "nativeVertexSha256": digest(vertices),
                    "indicesSha256": digest(indices),
                    "attributeCopies": copies,
                }
            )
    if not rows:
        raise AssetError("mesh component has no geometry")
    blob, placement = arena.render(base)
    placement["geometries"] = rows
    return blob, placement
